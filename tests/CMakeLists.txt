add_executable(
    manny_uploader_tests
    project_info_test.cpp
    test_main.cpp
    unit/addon/addon_lifecycle_test.cpp
    unit/application/application_pump_test.cpp
    unit/application/configuration_service_test.cpp
    unit/application/donbot_configuration_controller_test.cpp
    unit/application/twitch_authentication_controller_test.cpp
    unit/application/twitch_message_template_test.cpp
    unit/application/log_ingestion_coordinator_test.cpp
    unit/application/nexus_options_controller_test.cpp
    unit/application/recent_log_actions_controller_test.cpp
    unit/application/twitch_session_test.cpp
    unit/application/twitch_session_owner_test.cpp
    unit/application/upload_coordinator_test.cpp
    unit/config/protected_file_secret_store_test.cpp
    unit/config/settings_test.cpp
    unit/config/settings_store_test.cpp
    unit/domain/upload_job_test.cpp
    unit/evtc/metadata_decoder_test.cpp
    unit/evtc/metadata_parser_worker_test.cpp
    unit/evtc/zevtc_archive_test.cpp
    unit/filesystem/log_discovery_test.cpp
    unit/filesystem/polling_log_candidate_source_test.cpp
    unit/filesystem/change_notifying_log_candidate_source_test.cpp
    unit/http/body_sources_test.cpp
    unit/http/curl_http_client_test.cpp
    unit/ports/http_client_test.cpp
    unit/providers/dps_report_client_test.cpp
    unit/providers/dps_report_provider_worker_test.cpp
    unit/providers/donbot_client_test.cpp
    unit/providers/donbot_provider_worker_test.cpp
    unit/providers/donbot_verification_worker_test.cpp
    unit/providers/twitch_client_test.cpp
    unit/providers/twitch_authentication_worker_test.cpp
    unit/providers/twitch_provider_worker_test.cpp
    unit/providers/twitch_test_message_worker_test.cpp
    unit/providers/wingman_client_test.cpp
    unit/providers/wingman_provider_worker_test.cpp
    unit/ui/nexus_options_model_test.cpp
)

target_link_libraries(manny_uploader_tests PRIVATE manny_uploader_core)
target_compile_features(manny_uploader_tests PRIVATE cxx_std_23)
target_include_directories(manny_uploader_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(manny_uploader_tests PROPERTIES CXX_EXTENSIONS OFF)
manny_apply_compiler_warnings(manny_uploader_tests)

if(WIN32)
    target_sources(
        manny_uploader_tests
        PRIVATE integration/windows_directory_change_monitor_test.cpp
    )
    target_link_libraries(manny_uploader_tests PRIVATE ws2_32)
endif()

if(MINGW)
    target_link_options(manny_uploader_tests PRIVATE -static -static-libgcc -static-libstdc++)
endif()

add_test(NAME unit_tests COMMAND manny_uploader_tests)
set_tests_properties(unit_tests PROPERTIES LABELS deterministic)

add_test(
    NAME windows_package_verification_contract
    COMMAND
        "${CMAKE_COMMAND}"
        "-DMANNY_VERIFY_SCRIPT=${PROJECT_SOURCE_DIR}/cmake/VerifyWindowsPackage.cmake"
        "-DMANNY_TEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/windows-package-verification-contract"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/windows_package_verification_test.cmake"
)
set_tests_properties(
    windows_package_verification_contract
    PROPERTIES LABELS "deterministic;packaging"
)

add_test(
    NAME windows_symbols_verification_contract
    COMMAND
        "${CMAKE_COMMAND}"
        "-DMANNY_VERIFY_SCRIPT=${PROJECT_SOURCE_DIR}/cmake/VerifyWindowsSymbols.cmake"
        "-DMANNY_TEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/windows-symbols-verification-contract"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/windows_symbols_verification_test.cmake"
)
set_tests_properties(
    windows_symbols_verification_contract
    PROPERTIES LABELS "deterministic;packaging;symbols"
)

add_test(
    NAME windows_symbols_staging_contract
    COMMAND
        "${CMAKE_COMMAND}"
        "-DMANNY_STAGE_SCRIPT=${PROJECT_SOURCE_DIR}/cmake/StageWindowsSymbols.cmake"
        "-DMANNY_TEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/windows-symbols-staging-contract"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/windows_symbols_staging_test.cmake"
)
set_tests_properties(
    windows_symbols_staging_contract
    PROPERTIES LABELS "deterministic;packaging;symbols"
)

if(WIN32)
    add_executable(manny_nexus_addon_smoke_test integration/nexus_addon_smoke_test.cpp)
    target_link_libraries(manny_nexus_addon_smoke_test PRIVATE manny_nexus_api manny_imgui)
    target_compile_features(manny_nexus_addon_smoke_test PRIVATE cxx_std_23)
    set_target_properties(manny_nexus_addon_smoke_test PROPERTIES CXX_EXTENSIONS OFF)
    manny_apply_compiler_warnings(manny_nexus_addon_smoke_test)
    if(MINGW)
        target_link_options(
            manny_nexus_addon_smoke_test
            PRIVATE -static -static-libgcc -static-libstdc++
        )
    endif()
    add_dependencies(manny_nexus_addon_smoke_test manny_uploader)
    add_test(
        NAME nexus_addon_smoke_test
        COMMAND manny_nexus_addon_smoke_test "$<TARGET_FILE:manny_uploader>"
    )
    set_tests_properties(nexus_addon_smoke_test PROPERTIES LABELS "nexus;windows")
endif()
