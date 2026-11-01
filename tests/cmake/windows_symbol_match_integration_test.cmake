cmake_minimum_required(VERSION 3.25)

foreach(required_variable MANNY_DLL_FILE MANNY_SYMBOL_FILE MANNY_OTHER_SYMBOL_FILE MANNY_DUMPBIN_EXECUTABLE)
    if(NOT DEFINED ${required_variable} OR NOT EXISTS "${${required_variable}}")
        message(FATAL_ERROR "${required_variable} must name an existing file")
    endif()
endforeach()
if(NOT DEFINED MANNY_TEST_ROOT OR NOT IS_ABSOLUTE "${MANNY_TEST_ROOT}" OR EXISTS "${MANNY_TEST_ROOT}")
    message(FATAL_ERROR "Symbol-match test root must be an absolute, fresh directory")
endif()
file(SHA256 "${MANNY_SYMBOL_FILE}" expected_hash)
file(SHA256 "${MANNY_OTHER_SYMBOL_FILE}" other_hash)
if(expected_hash STREQUAL other_hash)
    message(FATAL_ERROR "Symbol mismatch test requires a PDB from another build")
endif()
file(MAKE_DIRECTORY "${MANNY_TEST_ROOT}")
file(COPY_FILE "${MANNY_DLL_FILE}" "${MANNY_TEST_ROOT}/manny_uploader.dll")
file(COPY_FILE "${MANNY_SYMBOL_FILE}" "${MANNY_TEST_ROOT}/manny_uploader.pdb")

function(check_match expected_success)
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            "-DMANNY_DLL_FILE=${MANNY_TEST_ROOT}/manny_uploader.dll"
            "-DMANNY_SYMBOL_FILE=${MANNY_TEST_ROOT}/manny_uploader.pdb"
            "-DMANNY_DUMPBIN_EXECUTABLE=${MANNY_DUMPBIN_EXECUTABLE}"
            -P "${CMAKE_CURRENT_LIST_DIR}/../../cmake/VerifyWindowsSymbolMatch.cmake"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error
    )
    if(expected_success AND NOT result EQUAL 0)
        message(FATAL_ERROR "Matching linker PDB rejected: ${output}${error}")
    elseif(NOT expected_success AND result EQUAL 0)
        message(FATAL_ERROR "Mismatched linker PDB accepted")
    endif()
endfunction()
check_match(TRUE)
file(COPY_FILE "${MANNY_OTHER_SYMBOL_FILE}" "${MANNY_TEST_ROOT}/manny_uploader.pdb")
check_match(FALSE)
message(STATUS "Verified real DLL/PDB match and mismatched-build rejection")
