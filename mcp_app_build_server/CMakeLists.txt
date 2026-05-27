cmake_minimum_required(VERSION 3.16)
project(altair_cpm_mcp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(altair-cpm-mcp
    mcp_server.c
    ../src/pico/PortDrivers/host_files_io.c
    ../Altair8800/universal_88dcdd.c
    ../Altair8800/intel8080.c
    ../Altair8800/memory.c
)

target_include_directories(altair-cpm-mcp PRIVATE
    ..
    ../Altair8800
    ../src/pico
    ../src/pico/PortDrivers
)

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(altair-cpm-mcp PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
endif()
