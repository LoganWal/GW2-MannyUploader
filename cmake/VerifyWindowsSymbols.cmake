cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED MANNY_SYMBOL_FILE OR "${MANNY_SYMBOL_FILE}" STREQUAL "")
    message(FATAL_ERROR "MANNY_SYMBOL_FILE must be provided")
endif()

cmake_path(
    ABSOLUTE_PATH MANNY_SYMBOL_FILE
    BASE_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/.."
    NORMALIZE
    OUTPUT_VARIABLE symbol_file
)
set(symbol_checksum_file "${symbol_file}.sha256")

if(NOT EXISTS "${symbol_file}" OR IS_DIRECTORY "${symbol_file}")
    message(FATAL_ERROR "Windows linker PDB is missing")
endif()

get_filename_component(symbol_name "${symbol_file}" NAME)
if(NOT symbol_name STREQUAL "manny_uploader.pdb")
    message(FATAL_ERROR "Windows linker PDB has an unexpected name")
endif()

file(SIZE "${symbol_file}" symbol_size)
if(symbol_size LESS 65536 OR symbol_size GREATER 536870912)
    message(FATAL_ERROR "Windows linker PDB has an implausible size")
endif()

file(READ "${symbol_file}" symbol_magic_hex OFFSET 0 LIMIT 29 HEX)
set(
    expected_symbol_magic_hex
    "4d6963726f736f667420432f432b2b204d534620372e30300d0a1a4453"
)
if(NOT "${symbol_magic_hex}" STREQUAL "${expected_symbol_magic_hex}")
    message(FATAL_ERROR "Windows symbol artifact is not a Microsoft Program Database")
endif()

file(SHA256 "${symbol_file}" symbol_checksum)
file(WRITE "${symbol_checksum_file}" "${symbol_checksum}  ${symbol_name}\n")
message(STATUS "Verified ${symbol_name} (${symbol_size} bytes)")
