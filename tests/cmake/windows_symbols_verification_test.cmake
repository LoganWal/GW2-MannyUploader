cmake_minimum_required(VERSION 3.25)

foreach(required_variable MANNY_VERIFY_SCRIPT MANNY_TEST_ROOT)
    if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR "${required_variable} must be provided")
    endif()
endforeach()

cmake_path(ABSOLUTE_PATH MANNY_TEST_ROOT NORMALIZE)
get_filename_component(test_root_name "${MANNY_TEST_ROOT}" NAME)
if(NOT test_root_name STREQUAL "windows-symbols-verification-contract")
    message(FATAL_ERROR "Symbol-verification test root has an unexpected name")
endif()

function(create_symbol fixture_name magic payload_length out_symbol)
    set(fixture_directory "${MANNY_TEST_ROOT}/${fixture_name}")
    file(MAKE_DIRECTORY "${fixture_directory}")
    string(
        RANDOM LENGTH ${payload_length}
        ALPHABET "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
        symbol_payload
    )
    set(symbol "${fixture_directory}/manny_uploader.pdb")
    file(WRITE "${symbol}" "${magic}${symbol_payload}")
    set(${out_symbol} "${symbol}" PARENT_SCOPE)
endfunction()

function(run_verification symbol checksum out_result out_diagnostic)
    execute_process(
        COMMAND
            "${CMAKE_COMMAND}"
            "-DMANNY_SYMBOL_FILE=${symbol}"
            "-DMANNY_SYMBOL_CHECKSUM_FILE=${checksum}"
            -P "${MANNY_VERIFY_SCRIPT}"
        RESULT_VARIABLE verification_result
        OUTPUT_VARIABLE verification_output
        ERROR_VARIABLE verification_error
    )
    set(${out_result} "${verification_result}" PARENT_SCOPE)
    set(${out_diagnostic} "${verification_output}${verification_error}" PARENT_SCOPE)
endfunction()

function(expect_failure symbol fixture_name expected_diagnostic)
    run_verification(
        "${symbol}"
        "${MANNY_TEST_ROOT}/${fixture_name}/manny_uploader.pdb.sha256"
        verification_result
        verification_diagnostic
    )
    if(verification_result EQUAL 0)
        message(FATAL_ERROR "Invalid ${fixture_name} symbol unexpectedly passed verification")
    endif()
    string(FIND "${verification_diagnostic}" "${expected_diagnostic}" diagnostic_offset)
    if(diagnostic_offset EQUAL -1)
        message(FATAL_ERROR "Invalid ${fixture_name} symbol returned the wrong diagnostic")
    endif()
endfunction()

file(REMOVE_RECURSE "${MANNY_TEST_ROOT}")
file(MAKE_DIRECTORY "${MANNY_TEST_ROOT}")

create_symbol(valid "Microsoft C/C++ MSF 7.00" 70000 valid_symbol)
set(valid_checksum "${MANNY_TEST_ROOT}/valid/manny_uploader.pdb.sha256")
run_verification("${valid_symbol}" "${valid_checksum}" valid_result valid_diagnostic)
if(NOT valid_result EQUAL 0)
    message(FATAL_ERROR "Valid Windows symbol failed verification: ${valid_diagnostic}")
endif()
file(SHA256 "${valid_symbol}" expected_checksum)
file(READ "${valid_checksum}" checksum_document)
if(NOT checksum_document STREQUAL "${expected_checksum}  manny_uploader.pdb\n")
    message(FATAL_ERROR "Windows symbol checksum sidecar has unexpected content")
endif()

expect_failure(
    "${MANNY_TEST_ROOT}/missing/manny_uploader.pdb"
    missing
    "Windows linker PDB is missing"
)

create_symbol(too-small "Microsoft C/C++ MSF 7.00" 128 too_small_symbol)
expect_failure("${too_small_symbol}" too-small "implausible size")

create_symbol(bad-magic "Not a Microsoft PDB file" 70000 bad_magic_symbol)
expect_failure("${bad_magic_symbol}" bad-magic "not a Microsoft Program Database")

create_symbol(wrong-name "Microsoft C/C++ MSF 7.00" 70000 wrong_name_symbol)
set(unexpected_name "${MANNY_TEST_ROOT}/wrong-name/unexpected.pdb")
file(RENAME "${wrong_name_symbol}" "${unexpected_name}")
expect_failure("${unexpected_name}" wrong-name "unexpected name")

create_symbol(path-collision "Microsoft C/C++ MSF 7.00" 70000 collision_symbol)
run_verification(
    "${collision_symbol}"
    "${collision_symbol}"
    collision_result
    collision_diagnostic
)
if(collision_result EQUAL 0)
    message(FATAL_ERROR "Symbol/checksum path collision unexpectedly passed verification")
endif()
string(FIND "${collision_diagnostic}" "paths must be distinct" collision_diagnostic_offset)
if(collision_diagnostic_offset EQUAL -1)
    message(FATAL_ERROR "Symbol/checksum path collision returned the wrong diagnostic")
endif()

file(REMOVE_RECURSE "${MANNY_TEST_ROOT}")
if(EXISTS "${MANNY_TEST_ROOT}")
    message(FATAL_ERROR "Symbol-verification fixtures were not removed")
endif()
