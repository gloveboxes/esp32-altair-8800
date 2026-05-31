cmake_minimum_required(VERSION 3.16)
project(altair_cpm_mcp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(ALTAIR_DIR ${CMAKE_CURRENT_LIST_DIR}/../altair8800)
set(LOCAL_DIR  ${CMAKE_CURRENT_LIST_DIR}/../altair_local)

add_executable(altair-cpm-mcp
    mcp_server.c
    ${LOCAL_DIR}/PortDrivers/host_files_io.c
    ${LOCAL_DIR}/PortDrivers/time_io.c
    ${ALTAIR_DIR}/universal_88dcdd.c
    ${ALTAIR_DIR}/intel8080.c
    ${ALTAIR_DIR}/memory.c
)

target_include_directories(altair-cpm-mcp PRIVATE
    .
    ${ALTAIR_DIR}
    ${LOCAL_DIR}
    ${LOCAL_DIR}/PortDrivers
)

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(altair-cpm-mcp PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
endif()
