cmake_minimum_required(VERSION 3.25)

foreach(required_variable MANNY_SYMBOL_FILE MANNY_SYMBOL_CHECKSUM_FILE)
    if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR "${required_variable} must be provided")
    endif()
endforeach()

cmake_path(ABSOLUTE_PATH MANNY_SYMBOL_FILE BASE_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/.." NORMALIZE)
cmake_path(
    ABSOLUTE_PATH MANNY_SYMBOL_CHECKSUM_FILE
    BASE_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/.."
    NORMALIZE
)

if(NOT EXISTS "${MANNY_SYMBOL_FILE}" OR IS_DIRECTORY "${MANNY_SYMBOL_FILE}")
    message(FATAL_ERROR "Windows linker PDB is missing")
endif()
if("${MANNY_SYMBOL_FILE}" STREQUAL "${MANNY_SYMBOL_CHECKSUM_FILE}")
    message(FATAL_ERROR "Windows linker PDB and checksum paths must be distinct")
endif()

get_filename_component(symbol_name "${MANNY_SYMBOL_FILE}" NAME)
if(NOT symbol_name STREQUAL "manny_uploader.pdb")
    message(FATAL_ERROR "Windows linker PDB has an unexpected name")
endif()

file(SIZE "${MANNY_SYMBOL_FILE}" symbol_size)
if(symbol_size LESS 65536 OR symbol_size GREATER 536870912)
    message(FATAL_ERROR "Windows linker PDB has an implausible size")
endif()

file(READ "${MANNY_SYMBOL_FILE}" symbol_magic OFFSET 0 LIMIT 24)
if(NOT symbol_magic STREQUAL "Microsoft C/C++ MSF 7.00")
    message(FATAL_ERROR "Windows symbol artifact is not a Microsoft Program Database")
endif()

file(SHA256 "${MANNY_SYMBOL_FILE}" symbol_checksum)
file(WRITE "${MANNY_SYMBOL_CHECKSUM_FILE}" "${symbol_checksum}  ${symbol_name}\n")
message(STATUS "Verified ${symbol_name} (${symbol_size} bytes)")
