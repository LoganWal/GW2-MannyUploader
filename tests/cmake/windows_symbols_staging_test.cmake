cmake_minimum_required(VERSION 3.25)

foreach(required_variable MANNY_STAGE_SCRIPT MANNY_TEST_ROOT)
    if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR "${required_variable} must be provided")
    endif()
endforeach()

cmake_path(ABSOLUTE_PATH MANNY_TEST_ROOT NORMALIZE)
get_filename_component(test_root_name "${MANNY_TEST_ROOT}" NAME)
if(NOT test_root_name STREQUAL "windows-symbols-staging-contract")
    message(FATAL_ERROR "Symbol-staging test root has an unexpected name")
endif()

file(REMOVE_RECURSE "${MANNY_TEST_ROOT}")
file(MAKE_DIRECTORY "${MANNY_TEST_ROOT}")

function(run_stage case_name expected_success search_directory symbol_file)
    execute_process(
        COMMAND
            "${CMAKE_COMMAND}"
            "-DMANNY_SYMBOL_SEARCH_DIRECTORY=${search_directory}"
            "-DMANNY_SYMBOL_FILE=${symbol_file}"
            -P "${MANNY_STAGE_SCRIPT}"
        RESULT_VARIABLE stage_result
        OUTPUT_VARIABLE stage_output
        ERROR_VARIABLE stage_error
    )
    if(expected_success AND NOT stage_result EQUAL 0)
        message(FATAL_ERROR "${case_name} unexpectedly failed:\n${stage_output}${stage_error}")
    endif()
    if(NOT expected_success AND stage_result EQUAL 0)
        message(FATAL_ERROR "${case_name} unexpectedly succeeded")
    endif()
endfunction()

set(valid_root "${MANNY_TEST_ROOT}/valid")
set(valid_source "${valid_root}/bin/Release/manny_uploader.pdb")
set(valid_output "${valid_root}/symbols/Release/manny_uploader.pdb")
file(MAKE_DIRECTORY "${valid_root}/bin/Release")
file(WRITE "${valid_source}" "fixture-symbols")
run_stage(valid TRUE "${valid_root}" "${valid_output}")
if(NOT EXISTS "${valid_output}")
    message(FATAL_ERROR "valid did not create the canonical symbol file")
endif()
file(READ "${valid_output}" valid_contents)
if(NOT valid_contents STREQUAL "fixture-symbols")
    message(FATAL_ERROR "valid changed the symbol contents")
endif()

set(in_place_root "${MANNY_TEST_ROOT}/in-place")
set(in_place_output "${in_place_root}/symbols/Release/manny_uploader.pdb")
file(MAKE_DIRECTORY "${in_place_root}/symbols/Release")
file(WRITE "${in_place_output}" "in-place-symbols")
run_stage(in_place TRUE "${in_place_root}" "${in_place_output}")

set(missing_root "${MANNY_TEST_ROOT}/missing")
file(MAKE_DIRECTORY "${missing_root}")
run_stage(missing FALSE "${missing_root}"
          "${missing_root}/symbols/Release/manny_uploader.pdb")

set(multiple_root "${MANNY_TEST_ROOT}/multiple")
file(MAKE_DIRECTORY "${multiple_root}/one" "${multiple_root}/two")
file(WRITE "${multiple_root}/one/manny_uploader.pdb" "one")
file(WRITE "${multiple_root}/two/manny_uploader.pdb" "two")
run_stage(multiple FALSE "${multiple_root}"
          "${multiple_root}/symbols/Release/manny_uploader.pdb")

set(contained_root "${MANNY_TEST_ROOT}/contained")
file(MAKE_DIRECTORY "${contained_root}")
file(WRITE "${contained_root}/manny_uploader.pdb" "contained")
run_stage(outside FALSE "${contained_root}"
          "${MANNY_TEST_ROOT}/outside/manny_uploader.pdb")

set(pipeline_root "${MANNY_TEST_ROOT}/pipeline")
set(pipeline_candidate "${pipeline_root}/bin/Release/manny_uploader.pdb")
set(
    pipeline_fixture
    "${pipeline_root}/tests/windows-symbols-verification-contract/valid/manny_uploader.pdb"
)
set(pipeline_output "${pipeline_root}/symbols/Release/manny_uploader.pdb")
file(MAKE_DIRECTORY "${pipeline_root}/bin/Release")
file(MAKE_DIRECTORY
     "${pipeline_root}/tests/windows-symbols-verification-contract/valid")
file(WRITE "${pipeline_candidate}" "linker-symbols")
file(WRITE "${pipeline_fixture}" "contract-fixture")
run_stage(pipeline-polluted FALSE "${pipeline_root}" "${pipeline_output}")
file(REMOVE_RECURSE "${pipeline_root}/tests")
run_stage(pipeline-clean TRUE "${pipeline_root}" "${pipeline_output}")
if(NOT EXISTS "${pipeline_output}")
    message(FATAL_ERROR "pipeline-clean did not stage the linker symbol")
endif()

file(REMOVE_RECURSE "${MANNY_TEST_ROOT}")
if(EXISTS "${MANNY_TEST_ROOT}")
    message(FATAL_ERROR "Symbol-staging fixtures were not removed")
endif()
