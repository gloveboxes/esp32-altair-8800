cmake_minimum_required(VERSION 3.16)
project(altair_local C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(CURL QUIET)
find_package(Threads REQUIRED)

set(ALTAIR_DIR ${CMAKE_CURRENT_LIST_DIR}/../altair8800)
set(MAIN_DIR ${CMAKE_CURRENT_LIST_DIR}/../main)
set(FRONT_PANEL_DIR ${CMAKE_CURRENT_LIST_DIR}/../front_panel)

add_executable(altair-local
    main.c
    host_platform.c
    ansi_input.c
    io_ports.c
    websocket_console.c
    PortDrivers/host_chat_io.c
    PortDrivers/environment_io.c
    PortDrivers/host_files_io.c
    PortDrivers/time_io.c
    PortDrivers/utility_io.c
    PortDrivers/weather_io.c
    ${CMAKE_CURRENT_LIST_DIR}/../port_drivers/json_scan.c
    ${ALTAIR_DIR}/universal_88dcdd.c
    ${ALTAIR_DIR}/intel8080.c
    ${ALTAIR_DIR}/memory.c
    # Shared monitor / disassembler sources reused from the ESP32 firmware.
    # The host build provides a stub sdkconfig.h (no CONFIG_ALTAIR_DISPLAY_*)
    # and a stub websocket_console.{h,c}, so these files compile unchanged.
    ${MAIN_DIR}/i8080_disasm.c
    ${MAIN_DIR}/cpu_state.c
    ${FRONT_PANEL_DIR}/virtual_monitor.c
)

target_include_directories(altair-local PRIVATE
    .
    ./PortDrivers
    ${ALTAIR_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../port_drivers
    ${MAIN_DIR}
    ${FRONT_PANEL_DIR}
)

target_compile_definitions(altair-local PRIVATE
    LOCAL_RUNNER_REPO_ROOT="${CMAKE_CURRENT_LIST_DIR}/.."
)

target_link_libraries(altair-local PRIVATE Threads::Threads)

if(CURL_FOUND)
    target_compile_definitions(altair-local PRIVATE HAVE_LIBCURL=1)
    target_link_libraries(altair-local PRIVATE CURL::libcurl)
    message(STATUS "altair-local: libcurl found, OpenAI chat port enabled")
else()
    message(STATUS "altair-local: libcurl NOT found, OpenAI chat port disabled")
endif()

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(altair-local PRIVATE
        -Wall
        -Wextra
        -Wno-missing-field-initializers
    )
elseif(MSVC)
    target_compile_options(altair-local PRIVATE /W4)
    target_compile_definitions(altair-local PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
