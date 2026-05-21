# CMake generated Testfile for 
# Source directory: /Users/dave/GitHub/esp32/esp32-altair-8800/host_tests
# Build directory: /Users/dave/GitHub/esp32/esp32-altair-8800/build-host-tests
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(selftest "/Users/dave/GitHub/esp32/esp32-altair-8800/build-host-tests/run_8080_tests" "--selftest")
set_tests_properties(selftest PROPERTIES  _BACKTRACE_TRIPLES "/Users/dave/GitHub/esp32/esp32-altair-8800/host_tests/CMakeLists.txt;43;add_test;/Users/dave/GitHub/esp32/esp32-altair-8800/host_tests/CMakeLists.txt;0;")
add_test(roms "/Users/dave/GitHub/esp32/esp32-altair-8800/build-host-tests/run_8080_tests" "--roms" "/Users/dave/GitHub/esp32/esp32-altair-8800/host_tests/roms")
set_tests_properties(roms PROPERTIES  SKIP_RETURN_CODE "77" TIMEOUT "600" _BACKTRACE_TRIPLES "/Users/dave/GitHub/esp32/esp32-altair-8800/host_tests/CMakeLists.txt;46;add_test;/Users/dave/GitHub/esp32/esp32-altair-8800/host_tests/CMakeLists.txt;0;")
