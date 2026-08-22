#pragma once

#include <cstddef>
#include <iostream>
#include <string_view>

namespace manny_uploader::test {

class TestSuite {
  public:
    void check(bool condition, std::string_view expression, std::string_view file,
               std::size_t line) {
        ++checks_;
        if (condition) {
            return;
        }

        ++failures_;
        std::cerr << file << ':' << line << ": check failed: " << expression << '\n';
    }

    [[nodiscard]] int finish() const {
        if (failures_ == 0) {
            std::cout << checks_ << " checks passed\n";
            return 0;
        }

        std::cerr << failures_ << " of " << checks_ << " checks failed\n";
        return 1;
    }

  private:
    std::size_t checks_{};
    std::size_t failures_{};
};

void run_project_info_tests(TestSuite& suite);
void run_addon_lifecycle_tests(TestSuite& suite);
void run_settings_tests(TestSuite& suite);
void run_settings_store_tests(TestSuite& suite);
void run_protected_secret_store_tests(TestSuite& suite);
void run_configuration_service_tests(TestSuite& suite);
void run_donbot_configuration_controller_tests(TestSuite& suite);
void run_twitch_authentication_controller_tests(TestSuite& suite);
void run_twitch_message_template_tests(TestSuite& suite);
void run_twitch_session_tests(TestSuite& suite);
void run_twitch_session_owner_tests(TestSuite& suite);
void run_application_pump_tests(TestSuite& suite);
void run_log_discovery_tests(TestSuite& suite);
void run_polling_log_candidate_source_tests(TestSuite& suite);
void run_http_client_tests(TestSuite& suite);
void run_http_body_source_tests(TestSuite& suite);
void run_curl_http_client_tests(TestSuite& suite);
void run_dps_report_client_tests(TestSuite& suite);
void run_dps_report_provider_worker_tests(TestSuite& suite);
void run_donbot_client_tests(TestSuite& suite);
void run_donbot_provider_worker_tests(TestSuite& suite);
void run_donbot_verification_worker_tests(TestSuite& suite);
void run_twitch_client_tests(TestSuite& suite);
void run_twitch_authentication_worker_tests(TestSuite& suite);
void run_twitch_provider_worker_tests(TestSuite& suite);
void run_twitch_test_message_worker_tests(TestSuite& suite);
void run_wingman_client_tests(TestSuite& suite);
void run_wingman_provider_worker_tests(TestSuite& suite);
void run_log_ingestion_coordinator_tests(TestSuite& suite);
void run_nexus_options_controller_tests(TestSuite& suite);
void run_recent_log_actions_controller_tests(TestSuite& suite);
void run_nexus_options_model_tests(TestSuite& suite);
void run_evtc_metadata_decoder_tests(TestSuite& suite);
void run_zevtc_archive_tests(TestSuite& suite);
void run_metadata_parser_worker_tests(TestSuite& suite);
void run_upload_coordinator_tests(TestSuite& suite);
void run_upload_job_tests(TestSuite& suite);

} // namespace manny_uploader::test

#define MANNY_CHECK(suite, condition)                                                              \
    (suite).check(static_cast<bool>(condition), #condition, __FILE__, __LINE__)
