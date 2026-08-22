cmake_minimum_required(VERSION 3.25)

foreach(required_variable MANNY_VERIFY_SCRIPT MANNY_TEST_ROOT)
    if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR "${required_variable} must be provided")
    endif()
endforeach()

cmake_path(ABSOLUTE_PATH MANNY_TEST_ROOT NORMALIZE)
get_filename_component(test_root_name "${MANNY_TEST_ROOT}" NAME)
if(NOT test_root_name STREQUAL "windows-package-verification-contract")
    message(FATAL_ERROR "Package-verification test root has an unexpected name")
endif()

function(create_package fixture_name magic include_extra out_package_directory)
    set(fixture_directory "${MANNY_TEST_ROOT}/${fixture_name}-fixture")
    set(package_directory "${MANNY_TEST_ROOT}/${fixture_name}-package")
    file(MAKE_DIRECTORY "${fixture_directory}" "${package_directory}")

    string(
        RANDOM LENGTH 120000
        ALPHABET "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
        dll_payload
    )
    file(WRITE "${fixture_directory}/manny_uploader.dll" "${magic}${dll_payload}")
    if(include_extra)
        file(WRITE "${fixture_directory}/unexpected.txt" "unexpected package content")
        set(archive_entries manny_uploader.dll unexpected.txt)
    else()
        set(archive_entries manny_uploader.dll)
    endif()

    set(archive_name "GW2-Manny-Uploader-0.1.0-windows-x64.zip")
    set(archive "${package_directory}/${archive_name}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E tar cf "${archive}" --format=zip ${archive_entries}
        WORKING_DIRECTORY "${fixture_directory}"
        RESULT_VARIABLE archive_result
        ERROR_VARIABLE archive_error
    )
    if(NOT archive_result EQUAL 0)
        message(FATAL_ERROR "Unable to create package fixture: ${archive_error}")
    endif()
    file(SHA256 "${archive}" archive_checksum)
    file(WRITE "${archive}.sha256" "${archive_checksum}  ${archive_name}\n")
    set(${out_package_directory} "${package_directory}" PARENT_SCOPE)
endfunction()

function(run_verification package_directory extraction_directory out_result out_diagnostic)
    execute_process(
        COMMAND
            "${CMAKE_COMMAND}"
            "-DMANNY_PACKAGE_OUTPUT_DIRECTORY=${package_directory}"
            "-DMANNY_PACKAGE_EXTRACT_DIRECTORY=${extraction_directory}"
            -P "${MANNY_VERIFY_SCRIPT}"
        RESULT_VARIABLE verification_result
        OUTPUT_VARIABLE verification_output
        ERROR_VARIABLE verification_error
    )
    set(${out_result} "${verification_result}" PARENT_SCOPE)
    set(${out_diagnostic} "${verification_output}${verification_error}" PARENT_SCOPE)
endfunction()

function(expect_failure package_directory fixture_name expected_diagnostic)
    run_verification(
        "${package_directory}"
        "${MANNY_TEST_ROOT}/${fixture_name}-extracted"
        verification_result
        verification_diagnostic
    )
    if(verification_result EQUAL 0)
        message(FATAL_ERROR "Invalid ${fixture_name} package unexpectedly passed verification")
    endif()
    string(FIND "${verification_diagnostic}" "${expected_diagnostic}" diagnostic_offset)
    if(diagnostic_offset EQUAL -1)
        message(FATAL_ERROR "Invalid ${fixture_name} package returned the wrong diagnostic")
    endif()
endfunction()

file(REMOVE_RECURSE "${MANNY_TEST_ROOT}")
file(MAKE_DIRECTORY "${MANNY_TEST_ROOT}")

create_package(valid "MZ" FALSE valid_package)
run_verification(
    "${valid_package}"
    "${MANNY_TEST_ROOT}/valid-extracted"
    valid_result
    valid_diagnostic
)
if(NOT valid_result EQUAL 0)
    message(FATAL_ERROR "Valid package failed verification: ${valid_diagnostic}")
endif()

create_package(bad-checksum "MZ" FALSE bad_checksum_package)
file(
    WRITE
    "${bad_checksum_package}/GW2-Manny-Uploader-0.1.0-windows-x64.zip.sha256"
    "0000000000000000000000000000000000000000000000000000000000000000  GW2-Manny-Uploader-0.1.0-windows-x64.zip\n"
)
expect_failure("${bad_checksum_package}" bad-checksum "sidecar does not match")

create_package(extra-entry "MZ" TRUE extra_entry_package)
expect_failure("${extra_entry_package}" extra-entry "must contain only manny_uploader.dll")

create_package(bad-pe "ZZ" FALSE bad_pe_package)
expect_failure("${bad_pe_package}" bad-pe "is not a Windows PE image")
