cmake_minimum_required(VERSION 3.25)

foreach(required_variable MANNY_PACKAGE_OUTPUT_DIRECTORY MANNY_PACKAGE_EXTRACT_DIRECTORY)
    if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR "${required_variable} must be provided")
    endif()
endforeach()

cmake_path(
    ABSOLUTE_PATH MANNY_PACKAGE_OUTPUT_DIRECTORY
    BASE_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/.."
    NORMALIZE
)
cmake_path(
    ABSOLUTE_PATH MANNY_PACKAGE_EXTRACT_DIRECTORY
    BASE_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/.."
    NORMALIZE
)

if(NOT IS_DIRECTORY "${MANNY_PACKAGE_OUTPUT_DIRECTORY}")
    message(FATAL_ERROR "Windows package output directory does not exist")
endif()
if(EXISTS "${MANNY_PACKAGE_EXTRACT_DIRECTORY}")
    message(FATAL_ERROR "Windows package extraction directory must not already exist")
endif()

file(
    GLOB package_archives
    LIST_DIRECTORIES FALSE
    "${MANNY_PACKAGE_OUTPUT_DIRECTORY}/GW2-Manny-Uploader-*-windows-x64.zip"
)
list(LENGTH package_archives package_archive_count)
if(NOT package_archive_count EQUAL 1)
    message(FATAL_ERROR "Expected exactly one versioned Windows x64 package archive")
endif()
list(GET package_archives 0 package_archive)

get_filename_component(package_archive_name "${package_archive}" NAME)
set(package_checksum "${package_archive}.sha256")
if(NOT EXISTS "${package_checksum}")
    message(FATAL_ERROR "Windows package SHA-256 sidecar is missing")
endif()

file(SIZE "${package_archive}" package_archive_size)
if(package_archive_size LESS 65536 OR package_archive_size GREATER 33554432)
    message(FATAL_ERROR "Windows package archive has an implausible size")
endif()

file(SHA256 "${package_archive}" actual_checksum)
file(READ "${package_checksum}" checksum_document LIMIT 512)
string(STRIP "${checksum_document}" checksum_document)
string(TOLOWER "${checksum_document}" checksum_document)
string(TOLOWER "${actual_checksum}  ${package_archive_name}" expected_checksum_document)
if(NOT checksum_document STREQUAL expected_checksum_document)
    message(FATAL_ERROR "Windows package SHA-256 sidecar does not match the archive")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar tf "${package_archive}"
    RESULT_VARIABLE listing_result
    OUTPUT_VARIABLE package_listing
    ERROR_VARIABLE listing_error
)
if(NOT listing_result EQUAL 0)
    message(FATAL_ERROR "Unable to inspect Windows package archive: ${listing_error}")
endif()
string(REPLACE "\r\n" "\n" package_listing "${package_listing}")
string(REPLACE "\r" "\n" package_listing "${package_listing}")
string(REGEX REPLACE "\n+$" "" package_listing "${package_listing}")
if(NOT package_listing STREQUAL "manny_uploader.dll")
    message(FATAL_ERROR "Windows package must contain only manny_uploader.dll at its root")
endif()

file(MAKE_DIRECTORY "${MANNY_PACKAGE_EXTRACT_DIRECTORY}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar xf "${package_archive}"
    WORKING_DIRECTORY "${MANNY_PACKAGE_EXTRACT_DIRECTORY}"
    RESULT_VARIABLE extraction_result
    ERROR_VARIABLE extraction_error
)
if(NOT extraction_result EQUAL 0)
    message(FATAL_ERROR "Unable to extract Windows package archive: ${extraction_error}")
endif()

set(packaged_dll "${MANNY_PACKAGE_EXTRACT_DIRECTORY}/manny_uploader.dll")
if(NOT EXISTS "${packaged_dll}")
    message(FATAL_ERROR "Packaged Nexus DLL is missing after extraction")
endif()
file(SIZE "${packaged_dll}" packaged_dll_size)
if(packaged_dll_size LESS 65536 OR packaged_dll_size GREATER 33554432)
    message(FATAL_ERROR "Packaged Nexus DLL has an implausible size")
endif()
file(READ "${packaged_dll}" packaged_dll_magic OFFSET 0 LIMIT 2 HEX)
string(TOLOWER "${packaged_dll_magic}" packaged_dll_magic)
if(NOT packaged_dll_magic STREQUAL "4d5a")
    message(FATAL_ERROR "Packaged Nexus artifact is not a Windows PE image")
endif()

message(STATUS "Verified ${package_archive_name} (${package_archive_size} bytes)")
