# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# --------------------------------------------------------------------
# Require ESP-IDF v6.0.1 or newer.
#
# IDF_PATH must point at a valid ESP-IDF install. If it's missing or
# the version is older than the minimum, fail fast with a clear
# message instead of producing a confusing "Unknown CMake command
# idf_build_set_property" error from a partial include.
# --------------------------------------------------------------------
set(_altair_min_idf_major 6)
set(_altair_min_idf_minor 0)
set(_altair_min_idf_patch 1)
set(_altair_min_idf "v${_altair_min_idf_major}.${_altair_min_idf_minor}.${_altair_min_idf_patch}")
set(_altair_example_idf_path "$ENV{HOME}/.espressif/${_altair_min_idf}/esp-idf")

if(NOT DEFINED ENV{IDF_PATH})
    message(FATAL_ERROR
        "IDF_PATH is not set. This project requires ESP-IDF ${_altair_min_idf} or newer. "
        "Run: source ${_altair_example_idf_path}/export.sh")
endif()

if(NOT EXISTS "$ENV{IDF_PATH}/tools/cmake/project.cmake")
    message(FATAL_ERROR
        "IDF_PATH=$ENV{IDF_PATH} does not contain tools/cmake/project.cmake. "
        "This project requires ESP-IDF ${_altair_min_idf} or newer "
        "(e.g. installed at ${_altair_example_idf_path}). "
        "Run: source <esp-idf>/export.sh")
endif()

# Read tools/cmake/version.cmake to verify minimum version.
if(EXISTS "$ENV{IDF_PATH}/tools/cmake/version.cmake")
    file(READ "$ENV{IDF_PATH}/tools/cmake/version.cmake" _altair_ver_contents)
    string(REGEX MATCH "IDF_VERSION_MAJOR[ \t]+([0-9]+)" _ "${_altair_ver_contents}")
    set(_altair_idf_major "${CMAKE_MATCH_1}")
    string(REGEX MATCH "IDF_VERSION_MINOR[ \t]+([0-9]+)" _ "${_altair_ver_contents}")
    set(_altair_idf_minor "${CMAKE_MATCH_1}")
    string(REGEX MATCH "IDF_VERSION_PATCH[ \t]+([0-9]+)" _ "${_altair_ver_contents}")
    set(_altair_idf_patch "${CMAKE_MATCH_1}")
    set(_altair_idf_version "v${_altair_idf_major}.${_altair_idf_minor}.${_altair_idf_patch}")

    set(_altair_idf_too_old FALSE)
    if(_altair_idf_major LESS _altair_min_idf_major)
        set(_altair_idf_too_old TRUE)
    elseif(_altair_idf_major EQUAL _altair_min_idf_major)
        if(_altair_idf_minor LESS _altair_min_idf_minor)
            set(_altair_idf_too_old TRUE)
        elseif(_altair_idf_minor EQUAL _altair_min_idf_minor)
            if(_altair_idf_patch LESS _altair_min_idf_patch)
                set(_altair_idf_too_old TRUE)
            endif()
        endif()
    endif()

    if(_altair_idf_too_old)
        message(FATAL_ERROR
            "ESP-IDF ${_altair_idf_version} at $ENV{IDF_PATH} is too old. "
            "This project requires ${_altair_min_idf} or newer.")
    endif()
    message(STATUS "ESP-IDF version check OK: ${_altair_idf_version} (minimum ${_altair_min_idf})")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(altair_esp32)

# Keep enough file-descriptor space when increasing LWIP sockets
# FD_SETSIZE = sockets + non-socket FDs
idf_build_set_property(COMPILE_DEFINITIONS "FD_SETSIZE=96" APPEND)

# --------------------------------------------------------------------
# Workaround for ESP-IDF v6.0.1 picolibc.specs duplicate-flag bug.
#
# components/esp_libc/project_include.cmake adds an absolute-path
# "-specs=<build>/specs/picolibc.specs" via idf_toolchain_add_flags(),
# but the dedup helper (_add_flags_to_files) compares with STREQUAL,
# so a stale bare "-specs=picolibc.specs" line written by an earlier
# IDF reconfigure is left behind. GCC then loads the same specs file
# twice and aborts with:
#   "attempt to rename spec 'link' to already defined spec 'picolibc_link'"
#
# Strip the bare line from build/toolchain/{cflags,cxxflags,asmflags}
# after configure (all three response files inherit the duplicate).
# --------------------------------------------------------------------
foreach(_altair_tcfile cflags cxxflags asmflags)
    set(_altair_path "${CMAKE_BINARY_DIR}/toolchain/${_altair_tcfile}")
    if(EXISTS "${_altair_path}")
        file(STRINGS "${_altair_path}" _altair_lines)
        # Only strip the bare line if an absolute-path version is also
        # present (the actual v6.0.1 duplicate). On IDF v6.0 the bare
        # line is the ONLY specs flag and removing it would break
        # picolibc include resolution → newlib headers get used.
        set(_altair_has_abs FALSE)
        foreach(_line IN LISTS _altair_lines)
            if("${_line}" MATCHES "^-specs=.+/picolibc\\.specs$")
                set(_altair_has_abs TRUE)
            endif()
        endforeach()
        if(_altair_has_abs)
            set(_altair_new "")
            set(_altair_changed FALSE)
            foreach(_line IN LISTS _altair_lines)
                if("${_line}" STREQUAL "-specs=picolibc.specs")
                    set(_altair_changed TRUE)
                else()
                    list(APPEND _altair_new "${_line}")
                endif()
            endforeach()
            if(_altair_changed)
                message(STATUS "Pruning duplicate bare -specs=picolibc.specs from ${_altair_path}")
                file(WRITE "${_altair_path}" "")
                foreach(_line IN LISTS _altair_new)
                    file(APPEND "${_altair_path}" "${_line}\n")
                endforeach()
            endif()
        endif()
    endif()
endforeach()
