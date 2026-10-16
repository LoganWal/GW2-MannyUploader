cmake_minimum_required(VERSION 3.30)
cmake_policy(SET CMP0091 NEW)
project(MannyRustHttp LANGUAGES C)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The native Rust HTTP backend requires Windows x64")
endif()

# Rust links the release static CRT in every profile. RelWithDebInfo retains native symbols.
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)
# Cargo's output directory is already deep. Keep MSBuild intermediate paths below MAX_PATH.
set(MANNY_CURL_COMPACT_PATHS ON)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/../../../cmake")
include(MannyCurl)

file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/curl-library-$<CONFIG>.txt"
    CONTENT "$<TARGET_FILE:libcurl_static>\n")
