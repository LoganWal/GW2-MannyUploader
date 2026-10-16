cmake_minimum_required(VERSION 3.25)

foreach(required_variable MANNY_DLL_FILE MANNY_SYMBOL_FILE MANNY_DUMPBIN_EXECUTABLE)
    if(NOT DEFINED ${required_variable} OR NOT EXISTS "${${required_variable}}")
        message(FATAL_ERROR "${required_variable} must name an existing file")
    endif()
endforeach()

# Require the selected PDB, even if another matching build exists on a debugger search path.
get_filename_component(symbol_directory "${MANNY_SYMBOL_FILE}" DIRECTORY)
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "_NT_SYMBOL_PATH=${symbol_directory}" --unset=_NT_ALT_SYMBOL_PATH
        "${MANNY_DUMPBIN_EXECUTABLE}" /nologo /pdbpath "${MANNY_DLL_FILE}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 30
)
if(NOT result EQUAL 0 OR NOT output MATCHES "PDB file found at '([^\r\n]+)'[\r\n]")
    message(FATAL_ERROR "No matching Windows linker PDB was found")
endif()
set(found_symbol "${CMAKE_MATCH_1}")
file(REAL_PATH "${found_symbol}" found_symbol)
file(REAL_PATH "${MANNY_SYMBOL_FILE}" expected_symbol)
string(TOLOWER "${found_symbol}" found_symbol)
string(TOLOWER "${expected_symbol}" expected_symbol)
if(NOT found_symbol STREQUAL expected_symbol)
    message(FATAL_ERROR "Selected Windows linker PDB does not match the DLL")
endif()
message(STATUS "Verified DLL and selected linker PDB match")
