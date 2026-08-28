#include "manny_uploader/providers/dps_report_client.hpp"

#include "support/test_suite.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace manny_uploader::test {
namespace {

[[nodiscard]] std::vector<std::byte> bytes(std::string_view value) {
    const auto characters = std::span{value.data(), value.size()};
    const auto byte_view = std::as_bytes(characters);
    return {byte_view.begin(), byte_view.end()};
}

[[nodiscard]] std::string text(std::span<const std::byte> value) {
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

class TempLog {
  public:
    explicit TempLog(std::string_view contents) {
        static std::atomic_uint64_t next_id{};
        path_ = std::filesystem::temp_directory_path() /
                ("manny-dps-client-" + std::to_string(next_id.fetch_add(1)) + ".zevtc");
        std::ofstream stream{path_, std::ios::binary | std::ios::trunc};
        stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        if (!stream) {
            throw std::runtime_error{"Could not write dps.report test fixture"};
        }
    }

    ~TempLog() {
        std::error_code error;
        static_cast<void>(std::filesystem::remove(path_, error));
    }

    TempLog(const TempLog&) = delete;
    TempLog& operator=(const TempLog&) = delete;

    [[nodiscard]] domain::LogFileIdentity identity() const {
        return domain::LogFileIdentity{
            .canonical_path = path_,
            .size = std::filesystem::file_size(path_),
            .last_write_time = std::filesystem::last_write_time(path_),
        };
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

[[nodiscard]] ports::HttpResponse json_response(std::string_view document,
                                                std::uint16_t status = 200,
                                                std::vector<ports::HttpHeader> headers = {}) {
    return ports::HttpResponse{
        .status_code = status,
        .headers = std::move(headers),
        .body = bytes(document),
    };
}

[[nodiscard]] ports::HttpError http_error(ports::HttpErrorCode code,
                                          std::string message = "private transport marker") {
    return ports::HttpError{
        .code = code,
        .message = std::move(message),
        .transport_code = 7,
        .body_error = std::nullopt,
        .system_error = std::nullopt,
    };
}

constexpr std::string_view valid_response = R"json({
  "id": "test-id",
  "permalink": "https://dps.report/abc-20260820-123456_test",
  "userToken": "replacement-token",
  "encounter": {
    "success": true,
    "bossId": 15438,
    "boss": "Vale Guardian",
    "isCm": true,
    "futureEncounterField": {"nested": [1, true, null]}
  },
  "players": {},
  "futureTopLevelField": [1, 2, 3]
})json";

class FakeHttpClient final : public ports::IHttpClient {
  public:
    explicit FakeHttpClient(ports::HttpResponse response) : result_{std::move(response)} {}
    explicit FakeHttpClient(ports::HttpError error) : result_{std::unexpected(std::move(error))} {}

    [[nodiscard]] std::expected<ports::HttpResponse, ports::HttpError>
    execute(ports::HttpRequest request, const std::stop_token& stop_token) const override {
        ++calls;
        method = request.method;
        url = std::move(request.url);
        headers = std::move(request.headers);
        timeouts = request.timeouts;
        response_limits = request.response_limits;
        saw_cancelled_token = stop_token.stop_requested();
        if (request.body) {
            body_length = request.body->content_length();
            std::array<std::byte, 7> buffer{};
            std::vector<std::byte> captured;
            while (captured.size() < body_length) {
                auto read = request.body->read(buffer, stop_token);
                if (!read) {
                    return std::unexpected(http_error(ports::HttpErrorCode::BodyReadFailed));
                }
                if (*read == 0 || *read > buffer.size()) {
                    return std::unexpected(http_error(ports::HttpErrorCode::BodyReadFailed));
                }
                captured.insert(captured.end(), buffer.begin(),
                                buffer.begin() + static_cast<std::ptrdiff_t>(*read));
            }
            body = text(captured);
        }
        return result_;
    }

    mutable std::size_t calls{};
    mutable ports::HttpMethod method{ports::HttpMethod::Get};
    mutable std::string url;
    mutable std::vector<ports::HttpHeader> headers;
    mutable ports::HttpTimeouts timeouts;
    mutable ports::HttpResponseLimits response_limits;
    mutable std::uint64_t body_length{};
    mutable std::string body;
    mutable bool saw_cancelled_token{};

  private:
    std::expected<ports::HttpResponse, ports::HttpError> result_;
};

class ThrowingHttpClient final : public ports::IHttpClient {
  public:
    [[nodiscard]] std::expected<ports::HttpResponse, ports::HttpError>
    execute(ports::HttpRequest, const std::stop_token&) const override {
        throw std::runtime_error{"private exception marker"};
    }
};

[[nodiscard]] const ports::HttpHeader* find_header(const FakeHttpClient& client,
                                                   std::string_view name) {
    const auto found = std::ranges::find(client.headers, name, &ports::HttpHeader::name);
    return found == client.headers.end() ? nullptr : &*found;
}

void request_and_success_tests(TestSuite& suite, const domain::LogFileIdentity& file) {
    FakeHttpClient http{json_response(valid_response)};
    providers::DpsReportClient client{http};
    const auto token = support::SecretValue::from_text("original-secret-token");
    const auto result = client.upload(file, &token);

    MANNY_CHECK(suite, result.has_value());
    MANNY_CHECK(suite, http.calls == 1);
    MANNY_CHECK(suite, http.method == ports::HttpMethod::Post);
    MANNY_CHECK(suite, http.url == providers::dps_report_upload_url);
    MANNY_CHECK(suite, http.url.find("original-secret-token") == std::string::npos);
    MANNY_CHECK(suite, !http.saw_cancelled_token);
    MANNY_CHECK(suite, http.timeouts.connect == std::chrono::seconds{10});
    MANNY_CHECK(suite, http.timeouts.operation == std::chrono::minutes{15});
    MANNY_CHECK(suite, http.timeouts.stalled_transfer == std::chrono::minutes{15});
    MANNY_CHECK(suite,
                http.response_limits.max_header_bytes == ports::max_http_response_header_bytes);
    MANNY_CHECK(suite, http.response_limits.max_body_bytes == 1024U * 1024U);
    MANNY_CHECK(suite, http.body_length == http.body.size());

    const auto* accept = find_header(http, "Accept");
    const auto* content_type = find_header(http, "Content-Type");
    MANNY_CHECK(suite, accept != nullptr);
    MANNY_CHECK(suite, accept != nullptr && accept->value == "application/json");
    MANNY_CHECK(suite, content_type != nullptr);
    MANNY_CHECK(suite, content_type != nullptr &&
                           content_type->value.starts_with("multipart/form-data; boundary="));
    for (const auto& header : http.headers) {
        MANNY_CHECK(suite, header.value.find("original-secret-token") == std::string::npos);
    }
    MANNY_CHECK(suite, http.body.find("name=\"userToken\"") != std::string::npos);
    MANNY_CHECK(suite, http.body.find("original-secret-token") != std::string::npos);
    MANNY_CHECK(suite,
                http.body.find("name=\"file\"; filename=\"upload.zevtc\"") != std::string::npos);
    MANNY_CHECK(suite,
                http.body.find("Content-Type: application/octet-stream") != std::string::npos);
    MANNY_CHECK(suite, http.body.find("unique-log-payload") != std::string::npos);

    if (result) {
        MANNY_CHECK(suite,
                    result->report.permalink == "https://dps.report/abc-20260820-123456_test");
        MANNY_CHECK(suite, result->report.encounter_name == "Vale Guardian");
        MANNY_CHECK(suite, result->report.boss_id == 15438);
        MANNY_CHECK(suite, result->report.mode == "CM");
        MANNY_CHECK(suite, result->report.success);
        MANNY_CHECK(suite, !result->warning.has_value());
        MANNY_CHECK(suite, result->replacement_user_token.has_value());
        if (result->replacement_user_token) {
            const auto replacement = support::SecretValue::from_text("replacement-token");
            MANNY_CHECK(suite, *result->replacement_user_token == replacement);
        }
    }
    MANNY_CHECK(suite, !token.empty());

    FakeHttpClient detailed_http{json_response(valid_response)};
    providers::DpsReportClient detailed_client{detailed_http};
    const auto detailed = detailed_client.upload(
        file, nullptr, {}, providers::DpsReportUploadOptions{.detailed_wvw = true});
    MANNY_CHECK(suite, detailed.has_value());
    MANNY_CHECK(suite, detailed_http.url == providers::dps_report_detailed_wvw_upload_url);

    FakeHttpClient no_token_http{json_response(R"json({
      "permalink":"https://dps.report/no-token-report",
      "encounter":{"success":false,"bossId":1,"boss":"Golem"}
    })json")};
    providers::DpsReportClient no_token_client{no_token_http};
    const auto no_token = no_token_client.upload(file);
    MANNY_CHECK(suite, no_token.has_value());
    MANNY_CHECK(suite, no_token_http.body.find("name=\"userToken\"") == std::string::npos);
    if (no_token) {
        MANNY_CHECK(suite, !no_token->report.success);
        MANNY_CHECK(suite, no_token->report.mode.empty());
        MANNY_CHECK(suite, !no_token->replacement_user_token.has_value());
    }

    FakeHttpClient wvw_http{json_response(R"json({
      "permalink":"https://wvw.report/KKNj-20260318-212757_wvw",
      "encounter":{"success":true,"bossId":1,"boss":"World vs World"}
    })json")};
    providers::DpsReportClient wvw_client{wvw_http};
    const auto wvw = wvw_client.upload(file);
    MANNY_CHECK(suite, wvw.has_value());
    MANNY_CHECK(suite,
                wvw && wvw->report.permalink == "https://wvw.report/KKNj-20260318-212757_wvw");
}

void response_variant_tests(TestSuite& suite, const domain::LogFileIdentity& file) {
    struct SuccessCase {
        std::string_view document;
        std::string_view expected_mode;
        bool expected_success;
    };
    constexpr std::array cases{
        SuccessCase{
            R"json({"permalink":"https://dps.report/lcm","encounter":{"success":true,"bossId":1,"boss":"Boss","isCm":true,"isLegendaryCm":true,"emboldened":5}})json",
            "LCM", true},
        SuccessCase{
            R"json({"permalink":"https://b.dps.report/emboldened","encounter":{"success":true,"bossId":2,"boss":"Boss","emboldened":3}})json",
            "Emboldened 3", true},
        SuccessCase{
            R"json({"permalink":"https://dps.report/normal","encounter":{"success":false,"bossId":65535,"boss":"Boss","isCm":false,"emboldened":0}})json",
            "", false},
    };
    for (const auto& test_case : cases) {
        FakeHttpClient http{json_response(test_case.document)};
        providers::DpsReportClient client{http};
        const auto result = client.upload(file);
        MANNY_CHECK(suite, result.has_value());
        if (result) {
            MANNY_CHECK(suite, result->report.mode == test_case.expected_mode);
            MANNY_CHECK(suite, result->report.success == test_case.expected_success);
        }
    }

    const auto current_token = support::SecretValue::from_text("same-token");
    FakeHttpClient same_http{json_response(R"json({
      "permalink":"https://dps.report/same-token",
      "userToken":"same-token",
      "encounter":{"success":true,"bossId":1,"boss":"Boss"}
    })json")};
    providers::DpsReportClient same_client{same_http};
    const auto same = same_client.upload(file, &current_token);
    MANNY_CHECK(suite, same.has_value());
    MANNY_CHECK(suite, same && !same->replacement_user_token.has_value());

    FakeHttpClient warning_http{json_response(R"json({
      "permalink":"https://dps.report/warning",
      "error":"private server detail",
      "encounter":{"success":true,"bossId":1,"boss":"Boss"}
    })json")};
    providers::DpsReportClient warning_client{warning_http};
    const auto warning = warning_client.upload(file);
    MANNY_CHECK(suite, warning.has_value());
    MANNY_CHECK(suite, warning && warning->warning.has_value());
    MANNY_CHECK(suite,
                warning && warning->warning->find("private server detail") == std::string::npos);

    FakeHttpClient bad_token_http{json_response(R"json({
      "permalink":"https://dps.report/bad-token",
      "userToken":"not a valid token",
      "encounter":{"success":true,"bossId":1,"boss":"Boss"}
    })json")};
    providers::DpsReportClient bad_token_client{bad_token_http};
    const auto bad_token = bad_token_client.upload(file);
    MANNY_CHECK(suite, bad_token.has_value());
    MANNY_CHECK(suite, bad_token && bad_token->warning.has_value());
    MANNY_CHECK(suite, bad_token && !bad_token->replacement_user_token.has_value());
}

void invalid_response_tests(TestSuite& suite, const domain::LogFileIdentity& file) {
    constexpr std::array<std::string_view, 19> documents{
        "not-json",
        R"json({})json",
        R"json({"permalink":"https://dps.report/missing-encounter"})json",
        R"json({"permalink":"https://dps.report/missing-success","encounter":{"bossId":1,"boss":"Boss"}})json",
        R"json({"permalink":"https://dps.report/missing-id","encounter":{"success":true,"boss":"Boss"}})json",
        R"json({"permalink":"https://dps.report/missing-name","encounter":{"success":true,"bossId":1}})json",
        R"json({"permalink":"http://dps.report/plaintext","encounter":{"success":true,"bossId":1,"boss":"Boss"}})json",
        R"json({"permalink":"https://evil.example/report","encounter":{"success":true,"bossId":1,"boss":"Boss"}})json",
        R"json({"permalink":"https://dps.report.evil.example/report","encounter":{"success":true,"bossId":1,"boss":"Boss"}})json",
        R"json({"permalink":"https://user@dps.report/report","encounter":{"success":true,"bossId":1,"boss":"Boss"}})json",
        R"json({"permalink":"https://dps.report/report?private=value","encounter":{"success":true,"bossId":1,"boss":"Boss"}})json",
        R"json({"permalink":"https://dps.report/report#fragment","encounter":{"success":true,"bossId":1,"boss":"Boss"}})json",
        R"json({"permalink":"https://dps.report","encounter":{"success":true,"bossId":1,"boss":"Boss"}})json",
        R"json({"permalink":"https://dps.report/negative","encounter":{"success":true,"bossId":-1,"boss":"Boss"}})json",
        R"json({"permalink":"https://dps.report/large","encounter":{"success":true,"bossId":65536,"boss":"Boss"}})json",
        R"json({"permalink":"https://dps.report/control","encounter":{"success":true,"bossId":1,"boss":"Bad\u0001Name"}})json",
        R"json({"permalink":"https://wvw.report/pve","encounter":{"success":true,"bossId":2,"boss":"Boss"}})json",
        R"json({"permalink":"https://wvw.report","encounter":{"success":true,"bossId":1,"boss":"World vs World"}})json",
        R"json({"permalink":"https://wvw.report.evil.example/private","encounter":{"success":true,"bossId":1,"boss":"World vs World"}})json",
    };
    for (const auto document : documents) {
        FakeHttpClient http{json_response(document)};
        providers::DpsReportClient client{http};
        const auto result = client.upload(file);
        MANNY_CHECK(suite, !result.has_value());
        if (!result) {
            MANNY_CHECK(suite, result.error().disposition ==
                                   providers::DpsReportUploadDisposition::Failed);
            MANNY_CHECK(suite, !result.error().retry_after.has_value());
            MANNY_CHECK(suite, result.error().detail.find(document) == std::string::npos);
        }
    }

    FakeHttpClient trailing_http{json_response(
        R"json({"permalink":"https://dps.report/trailing","encounter":{"success":true,"bossId":1,"boss":"Boss"}})json"
        " trailing")};
    providers::DpsReportClient trailing_client{trailing_http};
    MANNY_CHECK(suite, !trailing_client.upload(file).has_value());
}

void status_tests(TestSuite& suite, const domain::LogFileIdentity& file) {
    struct StatusCase {
        std::uint16_t status;
        std::vector<ports::HttpHeader> headers;
        providers::DpsReportUploadDisposition disposition;
        std::optional<std::chrono::seconds> delay;
    };
    const std::vector<StatusCase> cases{
        {408, {}, providers::DpsReportUploadDisposition::Retry, std::chrono::seconds{30}},
        {429,
         {{.name = "Retry-After", .value = "12"}},
         providers::DpsReportUploadDisposition::Retry,
         std::chrono::seconds{12}},
        {429, {}, providers::DpsReportUploadDisposition::Retry, std::chrono::seconds{60}},
        {429,
         {{.name = "retry-after", .value = "0"}},
         providers::DpsReportUploadDisposition::Retry,
         std::chrono::seconds{60}},
        {429,
         {{.name = "Retry-After", .value = "901"}},
         providers::DpsReportUploadDisposition::Retry,
         std::chrono::seconds{60}},
        {429,
         {{.name = "Retry-After", .value = "5"}, {.name = "Retry-After", .value = "6"}},
         providers::DpsReportUploadDisposition::Retry,
         std::chrono::seconds{60}},
        {500, {}, providers::DpsReportUploadDisposition::Retry, std::chrono::seconds{30}},
        {503, {}, providers::DpsReportUploadDisposition::Retry, std::chrono::seconds{30}},
        {302, {}, providers::DpsReportUploadDisposition::Failed, std::nullopt},
        {400, {}, providers::DpsReportUploadDisposition::Failed, std::nullopt},
    };
    for (const auto& test_case : cases) {
        FakeHttpClient http{
            json_response("private response marker", test_case.status, test_case.headers)};
        providers::DpsReportClient client{http};
        const auto result = client.upload(file);
        MANNY_CHECK(suite, !result.has_value());
        if (!result) {
            MANNY_CHECK(suite, result.error().disposition == test_case.disposition);
            MANNY_CHECK(suite, result.error().retry_after == test_case.delay);
            MANNY_CHECK(suite, result.error().http_status == test_case.status);
            MANNY_CHECK(suite,
                        result.error().detail.find("private response marker") == std::string::npos);
        }
    }
}

void transport_tests(TestSuite& suite, const domain::LogFileIdentity& file) {
    constexpr std::array retryable{
        ports::HttpErrorCode::Timeout,          ports::HttpErrorCode::NameResolutionFailed,
        ports::HttpErrorCode::ConnectionFailed, ports::HttpErrorCode::TlsFailed,
        ports::HttpErrorCode::SendFailed,       ports::HttpErrorCode::ReceiveFailed,
    };
    for (const auto code : retryable) {
        FakeHttpClient http{http_error(code)};
        providers::DpsReportClient client{http};
        const auto result = client.upload(file);
        MANNY_CHECK(suite, !result.has_value());
        if (!result) {
            MANNY_CHECK(suite,
                        result.error().disposition == providers::DpsReportUploadDisposition::Retry);
            MANNY_CHECK(suite, result.error().retry_after == std::chrono::seconds{30});
            MANNY_CHECK(suite, result.error().http_error == code);
            MANNY_CHECK(suite, result.error().detail.find("private transport marker") ==
                                   std::string::npos);
        }
    }

    constexpr std::array permanent{
        ports::HttpErrorCode::InvalidRequest,
        ports::HttpErrorCode::BodyReadFailed,
        ports::HttpErrorCode::ResponseTooLarge,
        ports::HttpErrorCode::ProtocolError,
        ports::HttpErrorCode::UnsupportedEnvironment,
        ports::HttpErrorCode::InitializationFailed,
        ports::HttpErrorCode::Internal,
    };
    for (const auto code : permanent) {
        FakeHttpClient http{http_error(code)};
        providers::DpsReportClient client{http};
        const auto result = client.upload(file);
        MANNY_CHECK(suite, !result.has_value());
        if (!result) {
            MANNY_CHECK(suite, result.error().disposition ==
                                   providers::DpsReportUploadDisposition::Failed);
            MANNY_CHECK(suite, !result.error().retry_after.has_value());
            MANNY_CHECK(suite, result.error().http_error == code);
        }
    }

    FakeHttpClient cancelled_http{http_error(ports::HttpErrorCode::Cancelled)};
    providers::DpsReportClient cancelled_client{cancelled_http};
    const auto cancelled = cancelled_client.upload(file);
    MANNY_CHECK(suite, !cancelled.has_value());
    MANNY_CHECK(suite, !cancelled && cancelled.error().disposition ==
                                         providers::DpsReportUploadDisposition::Cancelled);
    MANNY_CHECK(suite, !cancelled && !cancelled.error().retry_after.has_value());

    ThrowingHttpClient throwing_http;
    providers::DpsReportClient throwing_client{throwing_http};
    const auto thrown = throwing_client.upload(file);
    MANNY_CHECK(suite, !thrown.has_value());
    MANNY_CHECK(suite, !thrown && thrown.error().detail.find("private exception marker") ==
                                      std::string::npos);
}

void local_validation_tests(TestSuite& suite, const TempLog& log) {
    const auto file = log.identity();

    FakeHttpClient pre_cancel_http{json_response(valid_response)};
    providers::DpsReportClient pre_cancel_client{pre_cancel_http};
    std::stop_source stop;
    stop.request_stop();
    const auto pre_cancelled = pre_cancel_client.upload(file, nullptr, stop.get_token());
    MANNY_CHECK(suite, !pre_cancelled.has_value());
    MANNY_CHECK(suite, pre_cancel_http.calls == 0);
    MANNY_CHECK(suite, !pre_cancelled && pre_cancelled.error().disposition ==
                                             providers::DpsReportUploadDisposition::Cancelled);

    std::vector<support::SecretValue> invalid_tokens;
    invalid_tokens.emplace_back();
    invalid_tokens.push_back(support::SecretValue::from_text("contains space"));
    invalid_tokens.push_back(support::SecretValue::from_text(std::string(257, 'x')));
    for (const auto& token : invalid_tokens) {
        FakeHttpClient http{json_response(valid_response)};
        providers::DpsReportClient client{http};
        const auto result = client.upload(file, &token);
        MANNY_CHECK(suite, !result.has_value());
        MANNY_CHECK(suite, http.calls == 0);
        if (!result) {
            MANNY_CHECK(suite,
                        result.error().detail.find(log.path().string()) == std::string::npos);
            MANNY_CHECK(suite, result.error().detail.find("contains space") == std::string::npos);
        }
    }

    FakeHttpClient missing_http{json_response(valid_response)};
    providers::DpsReportClient missing_client{missing_http};
    auto missing = file;
    missing.canonical_path += ".missing-private-path";
    const auto missing_result = missing_client.upload(missing);
    MANNY_CHECK(suite, !missing_result.has_value());
    MANNY_CHECK(suite, missing_http.calls == 0);
    MANNY_CHECK(suite, !missing_result && missing_result.error().detail.find(
                                              "missing-private-path") == std::string::npos);

    FakeHttpClient changed_http{json_response(valid_response)};
    providers::DpsReportClient changed_client{changed_http};
    auto changed = file;
    ++changed.size;
    const auto changed_result = changed_client.upload(changed);
    MANNY_CHECK(suite, !changed_result.has_value());
    MANNY_CHECK(suite, changed_http.calls == 0);

    FakeHttpClient empty_http{json_response(valid_response)};
    providers::DpsReportClient empty_client{empty_http};
    auto empty = file;
    empty.size = 0;
    const auto empty_result = empty_client.upload(empty);
    MANNY_CHECK(suite, !empty_result.has_value());
    MANNY_CHECK(suite, empty_http.calls == 0);
}

} // namespace

void run_dps_report_client_tests(TestSuite& suite) {
    TempLog log{"unique-log-payload"};
    const auto file = log.identity();
    request_and_success_tests(suite, file);
    response_variant_tests(suite, file);
    invalid_response_tests(suite, file);
    status_tests(suite, file);
    transport_tests(suite, file);
    local_validation_tests(suite, log);
}

} // namespace manny_uploader::test
