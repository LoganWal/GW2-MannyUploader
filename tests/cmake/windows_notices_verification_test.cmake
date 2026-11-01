cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED MANNY_TEST_ROOT)
    message(FATAL_ERROR "MANNY_TEST_ROOT must be provided")
endif()
file(MAKE_DIRECTORY "${MANNY_TEST_ROOT}")
set(notices "${MANNY_TEST_ROOT}/notices.txt")
set(dll "${MANNY_TEST_ROOT}/fixture.dll")
file(WRITE "${notices}" "Synthetic copyright notice.\nComplete permission and disclaimer.\n")

function(check expected_success)
    execute_process(COMMAND "${CMAKE_COMMAND}" "-DMANNY_DLL_FILE=${dll}"
        "-DMANNY_NOTICES_FILE=${notices}"
        -P "${CMAKE_CURRENT_LIST_DIR}/../../cmake/VerifyWindowsNotices.cmake"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(expected_success AND NOT result EQUAL 0)
        message(FATAL_ERROR "Valid notices rejected: ${output}${error}")
    elseif(NOT expected_success AND result EQUAL 0)
        message(FATAL_ERROR "Incomplete notices accepted")
    endif()
endfunction()

file(WRITE "${dll}" "MZsynthetic-payload")
check(FALSE)
file(APPEND "${dll}" "Synthetic copyright notice.\n")
check(FALSE)
file(APPEND "${dll}" "Complete permission and disclaimer.\n")
check(TRUE)
execute_process(COMMAND "${CMAKE_COMMAND}" -DMANNY_DLL_FILE=fixture.dll
    -DMANNY_NOTICES_FILE=notices.txt
    -P "${CMAKE_CURRENT_LIST_DIR}/../../cmake/VerifyWindowsNotices.cmake"
    WORKING_DIRECTORY "${MANNY_TEST_ROOT}"
    RESULT_VARIABLE relative_result OUTPUT_VARIABLE relative_output ERROR_VARIABLE relative_error)
if(NOT relative_result EQUAL 0)
    message(FATAL_ERROR "Relative notices paths rejected: ${relative_output}${relative_error}")
endif()
file(APPEND "${notices}" "A newly added dependency notice.\n")
check(FALSE)
file(WRITE "${notices}" "")
check(FALSE)
file(WRITE "${notices}" "A")
string(ASCII 4 16 unaligned)
file(WRITE "${dll}" "${unaligned}")
check(FALSE)
file(APPEND "${dll}" "A")
check(TRUE)
