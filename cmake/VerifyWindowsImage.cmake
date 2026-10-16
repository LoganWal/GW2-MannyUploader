cmake_minimum_required(VERSION 3.25)

foreach(required_variable MANNY_DLL_FILE MANNY_DUMPBIN_EXECUTABLE)
    if(NOT DEFINED ${required_variable} OR NOT EXISTS "${${required_variable}}")
        message(FATAL_ERROR "${required_variable} must name an existing file")
    endif()
endforeach()

foreach(part headers exports dependents)
    execute_process(
        COMMAND "${MANNY_DUMPBIN_EXECUTABLE}" /nologo "/${part}" "${MANNY_DLL_FILE}"
        RESULT_VARIABLE result OUTPUT_VARIABLE ${part} ERROR_VARIABLE error
    )
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Unable to inspect Windows image ${part}: ${error}")
    endif()
endforeach()
include("${CMAKE_CURRENT_LIST_DIR}/WindowsImageContract.cmake")
manny_verify_windows_image("${headers}" "${exports}" "${dependents}")
message(STATUS "Verified x64 Nexus image, sole GetAddonDef export, symbols reference and system imports")
