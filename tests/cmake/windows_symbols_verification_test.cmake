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

function(run_verification symbol out_result out_diagnostic)
    execute_process(
        COMMAND
            "${CMAKE_COMMAND}"
            "-DMANNY_SYMBOL_FILE=${symbol}"
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

string(ASCII 26 pdb_substitute_character)
if(WIN32)
    # file(WRITE) expands LF to CRLF on Windows; including CR explicitly would produce CRCRLF.
    set(valid_symbol_magic "Microsoft C/C++ MSF 7.00\n${pdb_substitute_character}DS")
else()
    set(valid_symbol_magic "Microsoft C/C++ MSF 7.00\r\n${pdb_substitute_character}DS")
endif()

create_symbol(valid "${valid_symbol_magic}" 70000 valid_symbol)
set(valid_checksum "${MANNY_TEST_ROOT}/valid/manny_uploader.pdb.sha256")
run_verification("${valid_symbol}" valid_result valid_diagnostic)
if(NOT valid_result EQUAL 0)
    message(FATAL_ERROR "Valid Windows symbol failed verification: ${valid_diagnostic}")
endif()
file(SHA256 "${valid_symbol}" expected_checksum)
file(READ "${valid_checksum}" checksum_document)
if(NOT checksum_document STREQUAL "${expected_checksum}  manny_uploader.pdb\n")
    message(FATAL_ERROR "Windows symbol checksum sidecar has unexpected content")
endif()

create_symbol(relative-valid "${valid_symbol_magic}" 70000 relative_valid_symbol)
set(relative_valid_checksum "${relative_valid_symbol}.sha256")
get_filename_component(verify_script_directory "${MANNY_VERIFY_SCRIPT}" DIRECTORY)
cmake_path(
    ABSOLUTE_PATH verify_script_directory
    NORMALIZE
    OUTPUT_VARIABLE normalized_verify_script_directory
)
get_filename_component(verify_base_directory "${normalized_verify_script_directory}/.." ABSOLUTE)
cmake_path(
    RELATIVE_PATH relative_valid_symbol
    BASE_DIRECTORY "${verify_base_directory}"
    OUTPUT_VARIABLE relative_symbol_argument
)
run_verification(
    "${relative_symbol_argument}"
    relative_valid_result
    relative_valid_diagnostic
)
if(NOT relative_valid_result EQUAL 0)
    message(
        FATAL_ERROR
        "Valid repository-relative Windows symbol failed verification: "
        "${relative_valid_diagnostic}"
    )
endif()
if(NOT EXISTS "${relative_valid_checksum}")
    message(FATAL_ERROR "Repository-relative verification did not create the checksum sidecar")
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

create_symbol(incomplete-magic "Microsoft C/C++ MSF 7.00" 70000 incomplete_magic_symbol)
expect_failure(
    "${incomplete_magic_symbol}"
    incomplete-magic
    "not a Microsoft Program Database"
)

create_symbol(wrong-name "${valid_symbol_magic}" 70000 wrong_name_symbol)
set(unexpected_name "${MANNY_TEST_ROOT}/wrong-name/unexpected.pdb")
file(RENAME "${wrong_name_symbol}" "${unexpected_name}")
expect_failure("${unexpected_name}" wrong-name "unexpected name")

file(REMOVE_RECURSE "${MANNY_TEST_ROOT}")
if(EXISTS "${MANNY_TEST_ROOT}")
    message(FATAL_ERROR "Symbol-verification fixtures were not removed")
endif()
