# Host-side CPU test harness.
#
# Builds standalone executables that link the emulator core
# (../altair8800/x80.cxx via cpu_x80_adapter.cpp) and run CP/M-style .COM
# diagnostic ROMs through a BDOS trap. Two CPU specializations are built from
# the SAME mode-agnostic harness source:
#   * run_8080_tests  - core forced to the 8080 instruction set (X80_FORCE_8080)
#   * run_z80_tests   - core forced to the Z80 instruction set  (X80_FORCE_Z80)
#
# Build & run:
#   cmake -S host_tests -B build-host-tests
#   cmake --build build-host-tests
#   ctest --test-dir build-host-tests --output-on-failure
#
# 8080 ROMs live in host_tests/roms/:
#   TST8080.COM    (Microcosm Associates)
#   CPUTEST.COM    (SuperSoft)
#   8080PRE.COM    (Frank Cringle)
#   8080EXM.COM    (Frank Cringle, full exerciser - slow)
# Z80 ROMs live in host_tests/roms_z80/:
#   ZEXDOC.COM     (Frank Cringle, documented-flags exerciser)
#   ZEXALL.COM     (Frank Cringle, all-flags exerciser - slow)

cmake_minimum_required(VERSION 3.16)
project(altair8800_host_tests C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../altair8800)

# The harness drives the CPU through the i8080_* C API. Here that API is backed
# by the ntvcm x80 core (x80.cxx) running in 8080 mode through the adapter, so
# the diagnostic ROMs exercise the 8080 instruction set of the Z80 code base.
add_executable(run_8080_tests
    run_8080_tests.c
    ${CORE_DIR}/x80.cxx
    ${CORE_DIR}/cpu_x80_adapter.cpp
    ${CORE_DIR}/memory.c
)
target_include_directories(run_8080_tests PRIVATE
    ${CORE_DIR}
    ${CORE_DIR}/x80_compat
)
# X80_FORCE_8080 fixes the core to the 8080 instruction set at compile time, so
# the optimizer folds away every fZ80Mode check and drops the Z80-only paths --
# exercising the same branch-free 8080 specialization shape as the firmware Z80
# build, just with the opposite mode constant.
target_compile_definitions(run_8080_tests PRIVATE ALTAIR_X80_HOST_TEST=1 X80_FORCE_8080=1)
target_compile_options(run_8080_tests PRIVATE -D_GNU_SOURCE)
# The upstream x80 core trips format/fall-through/type-limits warnings; keep them
# non-fatal for the third-party sources rather than patching the core.
set_source_files_properties(
    ${CORE_DIR}/x80.cxx
    ${CORE_DIR}/cpu_x80_adapter.cpp
    PROPERTIES COMPILE_OPTIONS "-w"
)

# Z80 harness: the SAME harness source compiled with the core forced to the Z80
# instruction set (X80_FORCE_Z80). Runs the zexdoc/zexall Z80 exercisers.
add_executable(run_z80_tests
    run_8080_tests.c
    ${CORE_DIR}/x80.cxx
    ${CORE_DIR}/cpu_x80_adapter.cpp
    ${CORE_DIR}/memory.c
)
target_include_directories(run_z80_tests PRIVATE
    ${CORE_DIR}
    ${CORE_DIR}/x80_compat
)
target_compile_definitions(run_z80_tests PRIVATE ALTAIR_X80_HOST_TEST=1 X80_FORCE_Z80=1)
target_compile_options(run_z80_tests PRIVATE -D_GNU_SOURCE)
set_source_files_properties(
    ${CORE_DIR}/x80.cxx
    ${CORE_DIR}/cpu_x80_adapter.cpp
    PROPERTIES COMPILE_OPTIONS "-w"
)

enable_testing()

# JSON helper unit tests for the MCP server (../altair_mcp_server/json_util.c).
# Pure host code, no emulator dependencies.
set(MCP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../altair_mcp_server)
add_executable(test_json_util
    test_json_util.c
    ${MCP_DIR}/json_util.c
)
target_include_directories(test_json_util PRIVATE ${MCP_DIR})
target_compile_options(test_json_util PRIVATE
    -Wall -Wextra
    -D_GNU_SOURCE
)
add_test(NAME json_util COMMAND test_json_util)

# Always-on self-test: a tiny hand-assembled 8080 program that exercises
# ADD/SUB/CPI/JNZ/CALL/JMP and prints "SELFTEST PASS" via BDOS.
add_test(NAME selftest COMMAND run_8080_tests --selftest)

# Optional ROM tests: run when ROMs are present in host_tests/roms/.
add_test(NAME roms COMMAND run_8080_tests --roms ${CMAKE_CURRENT_SOURCE_DIR}/roms)
set_tests_properties(roms PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT 600
)

# Z80 self-test: the built-in self-test program uses only 8080 opcodes (a strict
# subset of Z80), so it also validates that the Z80-specialized build boots and
# executes basic instructions and BDOS output.
add_test(NAME selftest_z80 COMMAND run_z80_tests --selftest)

# Z80 exerciser ROMs: zexdoc/zexall from host_tests/roms_z80/. zexall in
# particular is slow, so allow a generous timeout.
add_test(NAME roms_z80 COMMAND run_z80_tests --roms ${CMAKE_CURRENT_SOURCE_DIR}/roms_z80)
set_tests_properties(roms_z80 PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT 1800
)
