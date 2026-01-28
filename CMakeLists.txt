# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(hello_esp32)

# Keep enough file-descriptor space when increasing LWIP sockets
# FD_SETSIZE = sockets + non-socket FDs
idf_build_set_property(COMPILE_DEFINITIONS "FD_SETSIZE=96" APPEND)
