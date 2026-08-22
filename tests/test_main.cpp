#include "support/test_suite.hpp"

int main() {
    manny_uploader::test::TestSuite suite;
    manny_uploader::test::run_project_info_tests(suite);
    manny_uploader::test::run_addon_lifecycle_tests(suite);
    manny_uploader::test::run_settings_tests(suite);
    manny_uploader::test::run_settings_store_tests(suite);
    manny_uploader::test::run_protected_secret_store_tests(suite);
    manny_uploader::test::run_configuration_service_tests(suite);
    manny_uploader::test::run_donbot_configuration_controller_tests(suite);
    manny_uploader::test::run_twitch_authentication_controller_tests(suite);
    manny_uploader::test::run_twitch_message_template_tests(suite);
    manny_uploader::test::run_twitch_session_tests(suite);
    manny_uploader::test::run_twitch_session_owner_tests(suite);
    manny_uploader::test::run_application_pump_tests(suite);
    manny_uploader::test::run_upload_job_tests(suite);
    manny_uploader::test::run_upload_coordinator_tests(suite);
    manny_uploader::test::run_log_discovery_tests(suite);
    manny_uploader::test::run_polling_log_candidate_source_tests(suite);
    manny_uploader::test::run_change_notifying_log_candidate_source_tests(suite);
#if defined(_WIN32)
    manny_uploader::test::run_windows_directory_change_monitor_tests(suite);
#endif
    manny_uploader::test::run_http_client_tests(suite);
    manny_uploader::test::run_http_body_source_tests(suite);
    manny_uploader::test::run_curl_http_client_tests(suite);
    manny_uploader::test::run_dps_report_client_tests(suite);
    manny_uploader::test::run_dps_report_provider_worker_tests(suite);
    manny_uploader::test::run_donbot_client_tests(suite);
    manny_uploader::test::run_donbot_provider_worker_tests(suite);
    manny_uploader::test::run_donbot_verification_worker_tests(suite);
    manny_uploader::test::run_twitch_client_tests(suite);
    manny_uploader::test::run_twitch_authentication_worker_tests(suite);
    manny_uploader::test::run_twitch_provider_worker_tests(suite);
    manny_uploader::test::run_twitch_test_message_worker_tests(suite);
    manny_uploader::test::run_wingman_client_tests(suite);
    manny_uploader::test::run_wingman_provider_worker_tests(suite);
    manny_uploader::test::run_log_ingestion_coordinator_tests(suite);
    manny_uploader::test::run_nexus_options_controller_tests(suite);
    manny_uploader::test::run_recent_log_actions_controller_tests(suite);
    manny_uploader::test::run_nexus_options_model_tests(suite);
    manny_uploader::test::run_evtc_metadata_decoder_tests(suite);
    manny_uploader::test::run_zevtc_archive_tests(suite);
    manny_uploader::test::run_metadata_parser_worker_tests(suite);
    return suite.finish();
}
