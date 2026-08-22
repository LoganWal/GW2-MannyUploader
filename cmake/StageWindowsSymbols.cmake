cmake_minimum_required(VERSION 3.25)

foreach(required_variable MANNY_SYMBOL_SEARCH_DIRECTORY MANNY_SYMBOL_FILE)
    if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR "${required_variable} must be provided")
    endif()
endforeach()

cmake_path(
    ABSOLUTE_PATH MANNY_SYMBOL_SEARCH_DIRECTORY
    BASE_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/.."
    NORMALIZE
)
cmake_path(
    ABSOLUTE_PATH MANNY_SYMBOL_FILE
    BASE_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/.."
    NORMALIZE
)

if(NOT IS_DIRECTORY "${MANNY_SYMBOL_SEARCH_DIRECTORY}")
    message(FATAL_ERROR "Windows symbol search directory is missing")
endif()
cmake_path(
    IS_PREFIX MANNY_SYMBOL_SEARCH_DIRECTORY "${MANNY_SYMBOL_FILE}"
    NORMALIZE symbol_file_is_contained
)
if(NOT symbol_file_is_contained)
    message(FATAL_ERROR "Windows symbol staging path must remain inside the build tree")
endif()

file(
    GLOB_RECURSE symbol_candidates
    LIST_DIRECTORIES FALSE
    "${MANNY_SYMBOL_SEARCH_DIRECTORY}/manny_uploader.pdb"
)
list(SORT symbol_candidates)
list(LENGTH symbol_candidates symbol_candidate_count)
if(NOT symbol_candidate_count EQUAL 1)
    message(FATAL_ERROR "Expected exactly one manny_uploader.pdb, found ${symbol_candidate_count}")
endif()

list(GET symbol_candidates 0 symbol_candidate)
cmake_path(NORMAL_PATH symbol_candidate)
get_filename_component(symbol_output_directory "${MANNY_SYMBOL_FILE}" DIRECTORY)
file(MAKE_DIRECTORY "${symbol_output_directory}")
if(NOT symbol_candidate STREQUAL MANNY_SYMBOL_FILE)
    file(COPY_FILE "${symbol_candidate}" "${MANNY_SYMBOL_FILE}" ONLY_IF_DIFFERENT)
endif()

message(STATUS "Staged Windows linker PDB from ${symbol_candidate}")
