cmake_minimum_required(VERSION 3.16)
project(altair_cpm_mcp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(CURL QUIET)
find_package(Threads REQUIRED)

set(ALTAIR_DIR ${CMAKE_CURRENT_LIST_DIR}/../altair8800)
set(LOCAL_DIR  ${CMAKE_CURRENT_LIST_DIR}/../altair_local)

add_executable(altair-cpm-mcp
    mcp_server.c
    ${LOCAL_DIR}/PortDrivers/io_ports.c
    ${LOCAL_DIR}/PortDrivers/host_chat_io.c
    ${LOCAL_DIR}/PortDrivers/environment_io.c
    ${LOCAL_DIR}/PortDrivers/host_files_io.c
    ${LOCAL_DIR}/PortDrivers/time_io.c
    ${LOCAL_DIR}/PortDrivers/utility_io.c
    ${LOCAL_DIR}/PortDrivers/weather_io.c
    ${LOCAL_DIR}/../port_drivers/json_scan.c
    ${ALTAIR_DIR}/universal_88dcdd.c
    ${ALTAIR_DIR}/intel8080.c
    ${ALTAIR_DIR}/memory.c
)

target_include_directories(altair-cpm-mcp PRIVATE
    .
    ${ALTAIR_DIR}
    ${LOCAL_DIR}
    ${LOCAL_DIR}/PortDrivers
    ${LOCAL_DIR}/../port_drivers
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
