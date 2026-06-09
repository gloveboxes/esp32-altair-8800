cmake_minimum_required(VERSION 3.16)
project(altair_cpm_mcp C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(CURL QUIET)
find_package(Threads REQUIRED)

set(ALTAIR_DIR ${CMAKE_CURRENT_LIST_DIR}/../altair8800)
set(LOCAL_DIR  ${CMAKE_CURRENT_LIST_DIR}/../altair_local)

add_executable(altair-cpm-mcp
    mcp_server.c
    json_util.c
    jsonrpc.c
    ${LOCAL_DIR}/PortDrivers/io_ports.c
    ${LOCAL_DIR}/PortDrivers/host_chat_io.c
    ${LOCAL_DIR}/PortDrivers/environment_io.c
    ${LOCAL_DIR}/PortDrivers/host_files_io.c
    ${LOCAL_DIR}/PortDrivers/time_io.c
    ${LOCAL_DIR}/PortDrivers/utility_io.c
    ${LOCAL_DIR}/PortDrivers/weather_io.c
    ${LOCAL_DIR}/../port_drivers/json_scan.c
    ${LOCAL_DIR}/drivers/sense_hat/sense_hat_panel.c
    ${ALTAIR_DIR}/universal_88dcdd.c
    ${ALTAIR_DIR}/x80.cxx
    ${ALTAIR_DIR}/cpu_x80_adapter.cpp
    ${ALTAIR_DIR}/memory.c
)

target_include_directories(altair-cpm-mcp PRIVATE
    .
    ${ALTAIR_DIR}
    ${ALTAIR_DIR}/x80_compat
    ${LOCAL_DIR}
    ${LOCAL_DIR}/PortDrivers
    ${LOCAL_DIR}/../port_drivers
)

# CPU core is the ntvcm 8080/Z80 core (x80.cxx) in Z80 mode via the adapter.
# X80_FORCE_Z80 fixes the instruction set at compile time so the optimizer
# folds away every fZ80Mode check; -w keeps the third-party core's warnings
# non-fatal rather than patching it.
set_source_files_properties(
    ${ALTAIR_DIR}/x80.cxx
    ${ALTAIR_DIR}/cpu_x80_adapter.cpp
    PROPERTIES
    COMPILE_OPTIONS "-w"
    COMPILE_DEFINITIONS "X80_FORCE_Z80=1"
)

target_link_libraries(altair-cpm-mcp PRIVATE Threads::Threads)

if(CURL_FOUND)
    target_compile_definitions(altair-cpm-mcp PRIVATE HAVE_LIBCURL=1)
    target_link_libraries(altair-cpm-mcp PRIVATE CURL::libcurl)
    message(STATUS "altair-cpm-mcp: libcurl found, OpenAI chat/weather ports enabled")
else()
    message(STATUS "altair-cpm-mcp: libcurl NOT found, chat/weather ports disabled")
endif()

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(altair-cpm-mcp PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
endif()
