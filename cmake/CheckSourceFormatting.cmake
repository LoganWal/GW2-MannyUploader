cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED MANNY_SOURCE_DIRECTORY OR "${MANNY_SOURCE_DIRECTORY}" STREQUAL "")
    message(FATAL_ERROR "MANNY_SOURCE_DIRECTORY must be provided")
endif()

cmake_path(ABSOLUTE_PATH MANNY_SOURCE_DIRECTORY NORMALIZE)
if(NOT IS_DIRECTORY "${MANNY_SOURCE_DIRECTORY}")
    message(FATAL_ERROR "MANNY_SOURCE_DIRECTORY does not exist")
endif()

if(DEFINED MANNY_CLANG_FORMAT_EXECUTABLE AND NOT "${MANNY_CLANG_FORMAT_EXECUTABLE}" STREQUAL "")
    find_program(
        clang_format
        NAMES "${MANNY_CLANG_FORMAT_EXECUTABLE}"
        DOC "clang-format executable used by the GW2 Manny Uploader source check"
    )
else()
    find_program(
        clang_format
        NAMES clang-format-18 clang-format
        DOC "clang-format executable used by the GW2 Manny Uploader source check"
    )
endif()
if(NOT clang_format)
    message(FATAL_ERROR "A clang-format executable is required")
endif()

file(
    GLOB_RECURSE source_files
    LIST_DIRECTORIES FALSE
    "${MANNY_SOURCE_DIRECTORY}/include/*.h"
    "${MANNY_SOURCE_DIRECTORY}/include/*.hpp"
    "${MANNY_SOURCE_DIRECTORY}/src/*.c"
    "${MANNY_SOURCE_DIRECTORY}/src/*.cpp"
    "${MANNY_SOURCE_DIRECTORY}/src/*.h"
    "${MANNY_SOURCE_DIRECTORY}/src/*.hpp"
    "${MANNY_SOURCE_DIRECTORY}/tests/*.c"
    "${MANNY_SOURCE_DIRECTORY}/tests/*.cpp"
    "${MANNY_SOURCE_DIRECTORY}/tests/*.h"
    "${MANNY_SOURCE_DIRECTORY}/tests/*.hpp"
)
list(SORT source_files)
list(LENGTH source_files source_file_count)
if(source_file_count EQUAL 0)
    message(FATAL_ERROR "No C or C++ source files were found")
endif()

execute_process(
    COMMAND "${clang_format}" --dry-run --Werror ${source_files}
    RESULT_VARIABLE format_result
    OUTPUT_VARIABLE format_output
    ERROR_VARIABLE format_error
)
if(NOT format_result EQUAL 0)
    message(FATAL_ERROR "Source formatting check failed:\n${format_output}${format_error}")
endif()

message(STATUS "Verified formatting for ${source_file_count} source files with ${clang_format}")
