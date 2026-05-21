# Host-side 8080 test harness.
#
# Builds a standalone executable that links the existing CPU core
# (../altair8800/intel8080.c + memory.c) and runs CP/M-style .COM
# diagnostic ROMs through a BDOS trap.
#
# Build & run:
#   cmake -S host_tests -B build-host-tests
#   cmake --build build-host-tests
#   ctest --test-dir build-host-tests --output-on-failure
#
# To run the standard 8080 test ROMs, drop them into host_tests/roms/:
#   TST8080.COM    (Microcosm Associates)
#   CPUTEST.COM    (SuperSoft)
#   8080PRE.COM    (Frank Cringle)
#   8080EXM.COM    (Frank Cringle, full exerciser - slow)

cmake_minimum_required(VERSION 3.16)
project(altair8800_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../altair8800)

add_executable(run_8080_tests
    run_8080_tests.c
    ${CORE_DIR}/intel8080.c
    ${CORE_DIR}/memory.c
)
target_include_directories(run_8080_tests PRIVATE ${CORE_DIR})
target_compile_options(run_8080_tests PRIVATE
    -Wall -Wextra
    -Wno-unused-parameter
    -Wno-unused-function
    -D_GNU_SOURCE
)

enable_testing()

# Always-on self-test: a tiny hand-assembled 8080 program that exercises
# ADD/SUB/CPI/JNZ/CALL/JMP and prints "SELFTEST PASS" via BDOS.
add_test(NAME selftest COMMAND run_8080_tests --selftest)

# Optional ROM tests: run when ROMs are present in host_tests/roms/.
add_test(NAME roms COMMAND run_8080_tests --roms ${CMAKE_CURRENT_SOURCE_DIR}/roms)
set_tests_properties(roms PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT 600
)
