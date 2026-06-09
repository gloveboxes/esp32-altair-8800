cmake_minimum_required(VERSION 3.16)
project(altair_local C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(CURL QUIET)
find_package(Threads REQUIRED)

# Raspberry Pi Sense HAT front panel support. This peripheral is Linux-only
# (Linux framebuffer + I2C) and is intended for the Docker image, which builds
# with -DALTAIR_LOCAL_ENABLE_SENSE_HAT=ON. It stays OFF for desktop builds.
option(ALTAIR_LOCAL_ENABLE_SENSE_HAT "Build Raspberry Pi Sense HAT front panel support (Linux/Docker only)" OFF)

set(ALTAIR_DIR ${CMAKE_CURRENT_LIST_DIR}/../altair8800)
set(MAIN_DIR ${CMAKE_CURRENT_LIST_DIR}/../main)
set(FRONT_PANEL_DIR ${CMAKE_CURRENT_LIST_DIR}/../front_panel)
set(WSSERVER_DIR ${CMAKE_CURRENT_LIST_DIR}/external/wsServer)

# wsServer (Theldus) provides the WebSocket protocol implementation for the
# browser terminal. Build its sources directly into a small static library
# instead of add_subdirectory() so we don't also build its examples/tests.
# Only built on POSIX; the web terminal proxy in web_terminal.c is POSIX-only.
if(NOT WIN32 AND EXISTS "${WSSERVER_DIR}/src/ws.c")
    add_library(wsserver STATIC
        ${WSSERVER_DIR}/src/ws.c
        ${WSSERVER_DIR}/src/base64.c
        ${WSSERVER_DIR}/src/sha1.c
        ${WSSERVER_DIR}/src/handshake.c
        ${WSSERVER_DIR}/src/utf8.c
    )
    target_include_directories(wsserver PUBLIC ${WSSERVER_DIR}/include)
    target_compile_definitions(wsserver PRIVATE VALIDATE_UTF8)
    target_link_libraries(wsserver PUBLIC Threads::Threads)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        # wsServer is third-party; don't fail our -Werror-free build on its warnings.
        target_compile_options(wsserver PRIVATE -w)
    endif()
    set(ALTAIR_LOCAL_HAVE_WEB_TERMINAL ON)
else()
    set(ALTAIR_LOCAL_HAVE_WEB_TERMINAL OFF)
endif()

# Sources shared by every build target. The CPU core (x80.cxx + adapter) is
# added on the executable below.
set(ALTAIR_LOCAL_COMMON_SOURCES
    main.c
    host_platform.c
    ansi_input.c
    websocket_console.c
    web_terminal.c
    PortDrivers/io_ports.c
    PortDrivers/host_chat_io.c
    PortDrivers/environment_io.c
    PortDrivers/host_files_io.c
    PortDrivers/time_io.c
    PortDrivers/utility_io.c
    PortDrivers/weather_io.c
    # Sense HAT integration glue. Always compiled; the body is an inert stub
    # unless ALTAIR_LOCAL_ENABLE_SENSE_HAT is set on a Linux target below.
    drivers/sense_hat/sense_hat_panel.c
    ${CMAKE_CURRENT_LIST_DIR}/../port_drivers/json_scan.c
    ${ALTAIR_DIR}/universal_88dcdd.c
    ${ALTAIR_DIR}/memory.c
    # Shared monitor / disassembler sources reused from the ESP32 firmware.
    # The host build provides a stub sdkconfig.h (no CONFIG_ALTAIR_DISPLAY_*)
    # and a stub websocket_console.{h,c}, so these files compile unchanged.
    ${MAIN_DIR}/i8080_disasm.c
    ${MAIN_DIR}/cpu_state.c
    ${FRONT_PANEL_DIR}/virtual_monitor.c
)

# Apply the include paths, definitions, libraries and warning flags shared by
# every altair-local CPU variant to the given target.
function(configure_altair_local target)
    target_include_directories(${target} PRIVATE
        .
        ./PortDrivers
        ./drivers/sense_hat
        ${ALTAIR_DIR}
        ${CMAKE_CURRENT_LIST_DIR}/../port_drivers
        ${MAIN_DIR}
        ${FRONT_PANEL_DIR}
    )

    target_compile_definitions(${target} PRIVATE
        LOCAL_RUNNER_REPO_ROOT="${CMAKE_CURRENT_LIST_DIR}/.."
    )

    target_link_libraries(${target} PRIVATE Threads::Threads)

    # Sense HAT hardware drivers (framebuffer LED matrix + I2C sensors) require
    # Linux headers, so they are only added on Linux when explicitly enabled.
    if(ALTAIR_LOCAL_ENABLE_SENSE_HAT AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_sources(${target} PRIVATE
            drivers/sense_hat/led_panel.c
            drivers/sense_hat/graphics.c
            drivers/sense_hat/sense_hat.c
        )
        target_compile_definitions(${target} PRIVATE ALTAIR_LOCAL_ENABLE_SENSE_HAT=1)
        message(STATUS "${target}: Raspberry Pi Sense HAT front panel ENABLED")
    elseif(ALTAIR_LOCAL_ENABLE_SENSE_HAT)
        message(WARNING "${target}: ALTAIR_LOCAL_ENABLE_SENSE_HAT requested but target is not Linux; Sense HAT disabled")
    endif()

    if(ALTAIR_LOCAL_HAVE_WEB_TERMINAL)
        target_compile_definitions(${target} PRIVATE ALTAIR_LOCAL_HAVE_WEB_TERMINAL=1)
        target_link_libraries(${target} PRIVATE wsserver)
    endif()

    if(CURL_FOUND)
        target_compile_definitions(${target} PRIVATE HAVE_LIBCURL=1)
        target_link_libraries(${target} PRIVATE CURL::libcurl)
    endif()

    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wno-missing-field-initializers
        )
    elseif(MSVC)
        target_compile_options(${target} PRIVATE /W4)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
endfunction()

# --- Local runner --------------------------------------------------------
# The CPU core is the ntvcm 8080/Z80 core (x80.cxx) driven in Z80 mode through
# cpu_x80_adapter.cpp, which implements the i8080_* API the rest of the
# emulator expects. x80_compat/ supplies minimal djl_os/djltrace shims so
# x80.cxx builds without the full ntvcm utility library. X80_FORCE_Z80 fixes
# the core to the Z80 instruction set at compile time, so the optimizer folds
# away every fZ80Mode check and drops the 8080-only paths.
add_executable(altair-local
    ${ALTAIR_LOCAL_COMMON_SOURCES}
    ${ALTAIR_DIR}/x80.cxx
    ${ALTAIR_DIR}/cpu_x80_adapter.cpp
)
configure_altair_local(altair-local)
target_include_directories(altair-local PRIVATE ${ALTAIR_DIR}/x80_compat)
target_compile_definitions(altair-local PRIVATE ALTAIR_CPU_Z80=1 X80_FORCE_Z80=1)
# The upstream x80 core trips format/fall-through/type-limits warnings; keep them
# non-fatal for the third-party sources rather than patching the core.
set_source_files_properties(
    ${ALTAIR_DIR}/x80.cxx
    ${ALTAIR_DIR}/cpu_x80_adapter.cpp
    PROPERTIES COMPILE_OPTIONS "-w"
)

if(ALTAIR_LOCAL_HAVE_WEB_TERMINAL)
    message(STATUS "altair-local: web terminal (wsServer) enabled")
else()
    message(STATUS "altair-local: web terminal disabled (Windows or wsServer submodule missing)")
endif()

if(CURL_FOUND)
    message(STATUS "altair-local: libcurl found, OpenAI chat port enabled")
else()
    message(STATUS "altair-local: libcurl NOT found, OpenAI chat port disabled")
endif()
