#include "manny_uploader/providers/wingman_client.hpp"

#include "support/test_suite.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
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
    const auto view = std::as_bytes(std::span{value.data(), value.size()});
    return {view.begin(), view.end()};
}

[[nodiscard]] std::string text(std::span<const std::byte> value) {
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

class TempLog {
  public:
    explicit TempLog(std::string_view contents) {
        static std::atomic_uint64_t next_id{};
        path_ = std::filesystem::temp_directory_path() /
                ("manny-wingman-client-" + std::to_string(next_id.fetch_add(1)) + ".zevtc");
        std::ofstream stream{path_, std::ios::binary | std::ios::trunc};
        stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        if (!stream) {
            throw std::runtime_error{"Could not write GW2Wingman test fixture"};
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

[[nodiscard]] ports::HttpResponse response(std::string_view document, std::uint16_t status = 200,
                                           std::vector<ports::HttpHeader> headers = {}) {
    return ports::HttpResponse{
        .status_code = status,
        .headers = std::move(headers),
        .body = bytes(document),
    };
}

[[nodiscard]] ports::HttpError transport_error(ports::HttpErrorCode code) {
    return ports::HttpError{
        .code = code,
        .message = "private transport detail",
        .transport_code = 7,
        .body_error = std::nullopt,
        .system_error = std::nullopt,
    };
}

class FakeHttpClient final : public ports::IHttpClient {
  public:
    explicit FakeHttpClient(ports::HttpResponse value) : result_{std::move(value)} {}
    explicit FakeHttpClient(ports::HttpError value) : result_{std::unexpected(std::move(value))} {}

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
            std::array<std::byte, 5> buffer{};
            std::vector<std::byte> captured;
            while (captured.size() < body_length) {
                auto read = request.body->read(buffer, stop_token);
                if (!read || *read == 0 || *read > buffer.size()) {
                    return std::unexpected(transport_error(ports::HttpErrorCode::BodyReadFailed));
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

class SequentialHttpClient final : public ports::IHttpClient {
  public:
    explicit SequentialHttpClient(std::vector<ports::HttpResponse> responses)
        : responses_{std::make_move_iterator(responses.begin()),
                     std::make_move_iterator(responses.end())} {}

    [[nodiscard]] std::expected<ports::HttpResponse, ports::HttpError>
    execute(ports::HttpRequest request, const std::stop_token& stop_token) const override {
        methods.push_back(request.method);
        urls.push_back(std::move(request.url));
        if (request.body) {
            std::array<std::byte, 64> buffer{};
            std::vector<std::byte> captured;
            while (true) {
                auto read = request.body->read(buffer, stop_token);
                if (!read) {
                    return std::unexpected(transport_error(ports::HttpErrorCode::BodyReadFailed));
                }
                if (*read == 0) {
                    break;
                }
                captured.insert(captured.end(), buffer.begin(),
                                buffer.begin() + static_cast<std::ptrdiff_t>(*read));
            }
            bodies.push_back(text(captured));
        } else {
            bodies.emplace_back();
        }
        if (responses_.empty()) {
            return std::unexpected(transport_error(ports::HttpErrorCode::Internal));
        }
        auto next = std::move(responses_.front());
        responses_.pop_front();
        return next;
    }

    mutable std::vector<ports::HttpMethod> methods;
    mutable std::vector<std::string> urls;
    mutable std::vector<std::string> bodies;

  private:
    mutable std::deque<ports::HttpResponse> responses_;
};

class ThrowingHttpClient final : public ports::IHttpClient {
  public:
    [[nodiscard]] std::expected<ports::HttpResponse, ports::HttpError>
    execute(ports::HttpRequest, const std::stop_token&) const override {
        throw std::runtime_error{"private client exception"};
    }
};

[[nodiscard]] const ports::HttpHeader* find_header(const FakeHttpClient& client,
                                                   std::string_view name) {
    const auto found = std::ranges::find(client.headers, name, &ports::HttpHeader::name);
    return found == client.headers.end() ? nullptr : &*found;
}

[[nodiscard]] domain::EncounterMetadata metadata() {
    return domain::EncounterMetadata{.boss_id = 15438, .pov_account = "Player.1234"};
}

void request_and_success_tests(TestSuite& suite, const domain::LogFileIdentity& file) {
    FakeHttpClient http{response(R"json({"result":true,"future":{"value":1}})json")};
    providers::WingmanClient client{http};
    const auto result = client.upload(file, metadata());

    MANNY_CHECK(suite, result.has_value());
    MANNY_CHECK(suite, result && !result->duplicate);
    MANNY_CHECK(suite, result && !result->permalink.has_value());
    MANNY_CHECK(suite, http.calls == 1);
    MANNY_CHECK(suite, http.method == ports::HttpMethod::Post);
    MANNY_CHECK(suite, http.url == providers::wingman_compat_upload_url);
    MANNY_CHECK(suite, !http.saw_cancelled_token);
    MANNY_CHECK(suite, http.timeouts.connect == std::chrono::seconds{10});
    MANNY_CHECK(suite, http.timeouts.operation == std::chrono::minutes{15});
    MANNY_CHECK(suite, http.timeouts.stalled_transfer == std::chrono::minutes{15});
    MANNY_CHECK(suite,
                http.response_limits.max_header_bytes == ports::max_http_response_header_bytes);
    MANNY_CHECK(suite, http.response_limits.max_body_bytes == 64U * 1024U);
    MANNY_CHECK(suite, http.body_length == http.body.size());

    const auto* accept = find_header(http, "Accept");
    const auto* content_type = find_header(http, "Content-Type");
    MANNY_CHECK(suite, accept != nullptr && accept->value == "application/json");
    MANNY_CHECK(suite, content_type != nullptr &&
                           content_type->value.starts_with("multipart/form-data; boundary="));
    MANNY_CHECK(suite, http.body.find("name=\"account\"") != std::string::npos);
    MANNY_CHECK(suite, http.body.find("Player.1234") != std::string::npos);
    MANNY_CHECK(suite, http.body.find("name=\"filesize\"") != std::string::npos);
    MANNY_CHECK(suite, http.body.find(std::to_string(file.size)) != std::string::npos);
    MANNY_CHECK(suite, http.body.find("name=\"triggerID\"") != std::string::npos);
    MANNY_CHECK(suite, http.body.find("15438") != std::string::npos);
    MANNY_CHECK(suite,
                http.body.find("name=\"file\"; filename=\"upload.zevtc\"") != std::string::npos);
    MANNY_CHECK(suite,
                http.body.find("Content-Type: application/octet-stream") != std::string::npos);
    MANNY_CHECK(suite, http.body.find("wingman-log-payload") != std::string::npos);

    FakeHttpClient duplicate_http{response("private duplicate response", 409)};
    providers::WingmanClient duplicate_client{
        duplicate_http, std::string{providers::wingman_compat_upload_url},
        providers::WingmanPollingOptions{.check_timeout = std::chrono::milliseconds::zero()}};
    const auto duplicate = duplicate_client.upload(file, metadata());
    MANNY_CHECK(suite, duplicate.has_value());
    MANNY_CHECK(suite, duplicate && duplicate->duplicate);
}

void ticket_and_permalink_tests(TestSuite& suite, const domain::LogFileIdentity& file) {
    SequentialHttpClient http{{
        response(R"json({"result":true,"ticket":42})json"),
        response(R"json({"state":"processing"})json"),
        response(R"json({"state":"uploaded"})json"),
        response(R"json({"success":false})json"),
        response(R"json({"success":true,"log":{"html":"20260825-123456_boss_kill"}})json"),
    }};
    providers::WingmanClient client{http, std::string{providers::wingman_compat_upload_url},
                                    providers::WingmanPollingOptions{
                                        .ticket_interval = std::chrono::milliseconds::zero(),
                                        .check_interval = std::chrono::milliseconds::zero(),
                                        .ticket_timeout = std::chrono::seconds{1},
                                        .check_timeout = std::chrono::seconds{1},
                                    }};
    const auto result = client.upload(file, metadata());

    MANNY_CHECK(suite, result.has_value());
    MANNY_CHECK(suite, result && !result->duplicate);
    MANNY_CHECK(suite,
                result && result->permalink == std::optional<std::string>{
                                                   "https://gw2wingman.nevermindcreations.de/log/"
                                                   "20260825-123456_boss_kill"});
    MANNY_CHECK(suite, http.urls.size() == 5);
    MANNY_CHECK(suite, http.urls.size() > 2 && http.urls[1] == "https://evtc.bel.st/status/42" &&
                           http.urls[2] == "https://evtc.bel.st/status/42");
    MANNY_CHECK(suite, http.urls.size() > 4 &&
                           http.urls[3] == providers::wingman_check_upload_url &&
                           http.urls[4] == providers::wingman_check_upload_url);
    MANNY_CHECK(suite, http.bodies.size() > 3 &&
                           http.bodies[3].contains("file=Player1234_upload.zevtc") &&
                           http.bodies[3].contains("bossID=15438") &&
                           http.bodies[3].contains("account=Player.1234"));

    SequentialHttpClient unsafe_http{{
        response(R"json({"result":true,"ticket":7})json"),
        response(R"json({"state":"uploaded"})json"),
        response(R"json({"success":true,"log":{"html":"../private"}})json"),
    }};
    providers::WingmanClient unsafe_client{unsafe_http,
                                           std::string{providers::wingman_compat_upload_url},
                                           providers::WingmanPollingOptions{
                                               .ticket_interval = std::chrono::milliseconds::zero(),
                                               .check_interval = std::chrono::milliseconds::zero(),
                                               .ticket_timeout = std::chrono::seconds{1},
                                               .check_timeout = std::chrono::seconds{1},
                                           }};
    const auto unsafe = unsafe_client.upload(file, metadata());
    MANNY_CHECK(suite, !unsafe.has_value());
    MANNY_CHECK(suite, !unsafe && unsafe.error().disposition ==
                                      providers::WingmanUploadDisposition::Failed);
}

void response_and_status_tests(TestSuite& suite, const domain::LogFileIdentity& file) {
    constexpr std::array<std::string_view, 5> invalid_responses{
        "not-json",
        R"json({})json",
        R"json({"result":false})json",
        R"json({"result":"true"})json",
        R"json({"result":true} trailing)json",
    };
    for (const auto document : invalid_responses) {
        FakeHttpClient http{response(document)};
        providers::WingmanClient client{http};
        const auto result = client.upload(file, metadata());
        MANNY_CHECK(suite, !result.has_value());
        MANNY_CHECK(suite, !result && result.error().disposition ==
                                          providers::WingmanUploadDisposition::Failed);
        MANNY_CHECK(suite, !result && result.error().detail.find(document) == std::string::npos);
    }

    struct StatusCase {
        std::uint16_t status;
        std::vector<ports::HttpHeader> headers;
        providers::WingmanUploadDisposition disposition;
        std::optional<std::chrono::seconds> delay;
    };
    const std::vector<StatusCase> cases{
        {408, {}, providers::WingmanUploadDisposition::Retry, std::chrono::seconds{30}},
        {429,
         {{.name = "Retry-After", .value = "12"}},
         providers::WingmanUploadDisposition::Retry,
         std::chrono::seconds{12}},
        {429,
         {{.name = "retry-after", .value = "0"}},
         providers::WingmanUploadDisposition::Retry,
         std::chrono::seconds{60}},
        {429,
         {{.name = "Retry-After", .value = "901"}},
         providers::WingmanUploadDisposition::Retry,
         std::chrono::seconds{60}},
        {429,
         {{.name = "Retry-After", .value = "5"}, {.name = "Retry-After", .value = "6"}},
         providers::WingmanUploadDisposition::Retry,
         std::chrono::seconds{60}},
        {500, {}, providers::WingmanUploadDisposition::Retry, std::chrono::seconds{30}},
        {503, {}, providers::WingmanUploadDisposition::Retry, std::chrono::seconds{30}},
        {302, {}, providers::WingmanUploadDisposition::Failed, std::nullopt},
        {400, {}, providers::WingmanUploadDisposition::Failed, std::nullopt},
    };
    for (const auto& test_case : cases) {
        FakeHttpClient http{
            response("private server response", test_case.status, test_case.headers)};
        providers::WingmanClient client{http};
        const auto result = client.upload(file, metadata());
        MANNY_CHECK(suite, !result.has_value());
        MANNY_CHECK(suite, !result && result.error().disposition == test_case.disposition);
        MANNY_CHECK(suite, !result && result.error().retry_after == test_case.delay);
        MANNY_CHECK(suite, !result && result.error().http_status == test_case.status);
        MANNY_CHECK(suite, !result && result.error().detail.find("private") == std::string::npos);
    }
}

void transport_tests(TestSuite& suite, const domain::LogFileIdentity& file) {
    constexpr std::array retryable{
        ports::HttpErrorCode::Timeout,          ports::HttpErrorCode::NameResolutionFailed,
        ports::HttpErrorCode::ConnectionFailed, ports::HttpErrorCode::TlsFailed,
        ports::HttpErrorCode::SendFailed,       ports::HttpErrorCode::ReceiveFailed,
    };
    for (const auto code : retryable) {
        FakeHttpClient http{transport_error(code)};
        providers::WingmanClient client{http};
        const auto result = client.upload(file, metadata());
        MANNY_CHECK(suite, !result.has_value());
        MANNY_CHECK(suite, !result && result.error().disposition ==
                                          providers::WingmanUploadDisposition::Retry);
        MANNY_CHECK(suite, !result && result.error().retry_after == std::chrono::seconds{30});
        MANNY_CHECK(suite, !result && result.error().http_error == code);
        MANNY_CHECK(suite, !result && result.error().detail.find("private") == std::string::npos);
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
        FakeHttpClient http{transport_error(code)};
        providers::WingmanClient client{http};
        const auto result = client.upload(file, metadata());
        MANNY_CHECK(suite, !result.has_value());
        MANNY_CHECK(suite, !result && result.error().disposition ==
                                          providers::WingmanUploadDisposition::Failed);
        MANNY_CHECK(suite, !result && !result.error().retry_after.has_value());
        MANNY_CHECK(suite, !result && result.error().http_error == code);
    }

    FakeHttpClient cancelled_http{transport_error(ports::HttpErrorCode::Cancelled)};
    providers::WingmanClient cancelled_client{cancelled_http};
    const auto cancelled = cancelled_client.upload(file, metadata());
    MANNY_CHECK(suite, !cancelled.has_value());
    MANNY_CHECK(suite, !cancelled && cancelled.error().disposition ==
                                         providers::WingmanUploadDisposition::Cancelled);

    ThrowingHttpClient throwing_http;
    providers::WingmanClient throwing_client{throwing_http};
    const auto thrown = throwing_client.upload(file, metadata());
    MANNY_CHECK(suite, !thrown.has_value());
    MANNY_CHECK(suite, !thrown && thrown.error().detail.find("private") == std::string::npos);
}

void validation_tests(TestSuite& suite, const TempLog& log) {
    const auto file = log.identity();
    const auto expect_local_failure = [&](domain::LogFileIdentity candidate,
                                          domain::EncounterMetadata encounter) {
        FakeHttpClient http{response(R"json({"result":true})json")};
        providers::WingmanClient client{http};
        const auto result = client.upload(candidate, encounter);
        MANNY_CHECK(suite, !result.has_value());
        MANNY_CHECK(suite, http.calls == 0);
        MANNY_CHECK(suite, !result && result.error().detail.find(log.path().string()) ==
                                          std::string::npos);
    };

    auto empty_file = file;
    empty_file.size = 0;
    expect_local_failure(empty_file, metadata());
    auto missing_file = file;
    missing_file.canonical_path += ".private-missing";
    expect_local_failure(missing_file, metadata());
    auto changed_file = file;
    ++changed_file.size;
    expect_local_failure(changed_file, metadata());

    auto empty_account = metadata();
    empty_account.pov_account.clear();
    expect_local_failure(file, empty_account);
    auto control_account = metadata();
    control_account.pov_account = "private\naccount";
    expect_local_failure(file, control_account);
    auto oversized_account = metadata();
    oversized_account.pov_account.assign(257, 'x');
    expect_local_failure(file, oversized_account);
    auto zero_boss = metadata();
    zero_boss.boss_id = 0;
    expect_local_failure(file, zero_boss);

    FakeHttpClient endpoint_http{response(R"json({"result":true})json")};
    providers::WingmanClient endpoint_client{endpoint_http, "http://private.invalid/evtc"};
    const auto bad_endpoint = endpoint_client.upload(file, metadata());
    MANNY_CHECK(suite, !bad_endpoint.has_value());
    MANNY_CHECK(suite, endpoint_http.calls == 0);
    MANNY_CHECK(suite, !bad_endpoint && bad_endpoint.error().detail.find("private.invalid") ==
                                            std::string::npos);

    FakeHttpClient cancel_http{response(R"json({"result":true})json")};
    providers::WingmanClient cancel_client{cancel_http};
    std::stop_source stop;
    stop.request_stop();
    const auto cancelled = cancel_client.upload(file, metadata(), stop.get_token());
    MANNY_CHECK(suite, !cancelled.has_value());
    MANNY_CHECK(suite, cancel_http.calls == 0);
    MANNY_CHECK(suite, !cancelled && cancelled.error().disposition ==
                                         providers::WingmanUploadDisposition::Cancelled);
}

void permalink_import_tests(TestSuite& suite) {
    SequentialHttpClient http{{
        response(R"json({"link":"https://dps.report/abc-123","success":1})json"),
        response(
            R"json({"link":"https://dps.report/abc-123","inQueue":true,"inDB":false,"targetURL":"https://gw2wingman.nevermindcreations.de/log/abc-123"})json"),
    }};
    providers::WingmanClient client{http};
    const auto imported = client.import_permalink("https://dps.report/abc-123");
    MANNY_CHECK(suite, imported.has_value());
    MANNY_CHECK(suite, imported && !imported->duplicate);
    MANNY_CHECK(suite, imported && imported->permalink ==
                                       std::optional<std::string>{
                                           "https://gw2wingman.nevermindcreations.de/log/abc-123"});
    MANNY_CHECK(suite,
                (http.methods == std::vector{ports::HttpMethod::Post, ports::HttpMethod::Get}));
    MANNY_CHECK(suite, http.urls.size() == 2);
    if (http.urls.size() == 2) {
        constexpr std::string_view encoded = "https%3A%2F%2Fdps.report%2Fabc-123";
        MANNY_CHECK(suite, http.urls[0] == std::string{providers::wingman_import_queued_url} +
                                               std::string{encoded});
        MANNY_CHECK(suite, http.urls[1] == std::string{providers::wingman_check_queued_url} +
                                               std::string{encoded});
    }

    SequentialHttpClient wvw_http{{
        response(R"json({"success":1})json"),
        response(
            R"json({"inQueue":true,"inDB":false,"targetURL":"https://gw2wingman.nevermindcreations.de/log/wvw-fight"})json"),
    }};
    providers::WingmanClient wvw_client{wvw_http};
    const auto wvw = wvw_client.import_permalink("https://wvw.report/KKNj-20260318-212757_wvw");
    MANNY_CHECK(suite, wvw.has_value());
    MANNY_CHECK(suite, wvw_http.urls.size() == 2);
    if (wvw_http.urls.size() == 2) {
        constexpr std::string_view encoded = "https%3A%2F%2Fwvw.report%2FKKNj-20260318-212757_wvw";
        MANNY_CHECK(suite, wvw_http.urls[0] == std::string{providers::wingman_import_queued_url} +
                                                   std::string{encoded});
        MANNY_CHECK(suite, wvw_http.urls[1] == std::string{providers::wingman_check_queued_url} +
                                                   std::string{encoded});
    }

    SequentialHttpClient duplicate_http{{
        response(R"json({"success":0})json"),
        response(
            R"json({"inQueue":false,"inDB":true,"targetURL":"https://gw2wingman.nevermindcreations.de/log/existing"})json"),
    }};
    providers::WingmanClient duplicate_client{duplicate_http};
    const auto duplicate = duplicate_client.import_permalink("https://dps.report/existing");
    MANNY_CHECK(suite, duplicate.has_value() && duplicate->duplicate);

    SequentialHttpClient already_queued_http{{
        response(R"json({"success":0})json"),
        response(
            R"json({"inQueue":true,"inDB":false,"targetURL":"https://gw2wingman.nevermindcreations.de/log/already-queued"})json"),
    }};
    providers::WingmanClient already_queued_client{already_queued_http};
    const auto already_queued =
        already_queued_client.import_permalink("https://dps.report/already-queued");
    MANNY_CHECK(suite, already_queued.has_value());
    MANNY_CHECK(suite, already_queued && !already_queued->duplicate);

    SequentialHttpClient not_visible_yet_http{{
        response(R"json({"success":1})json"),
        response(
            R"json({"inQueue":false,"inDB":false,"targetURL":"https://gw2wingman.nevermindcreations.de/log/not-visible-yet"})json"),
    }};
    providers::WingmanClient not_visible_yet_client{not_visible_yet_http};
    const auto not_visible_yet =
        not_visible_yet_client.import_permalink("https://dps.report/not-visible-yet");
    MANNY_CHECK(suite, not_visible_yet.has_value());
    MANNY_CHECK(suite, not_visible_yet && !not_visible_yet->duplicate);

    SequentialHttpClient declined_http{{
        response(R"json({"success":0})json"),
        response(
            R"json({"inQueue":false,"inDB":false,"targetURL":"https://gw2wingman.nevermindcreations.de/log/declined"})json"),
    }};
    providers::WingmanClient declined_client{declined_http};
    MANNY_CHECK(suite,
                !declined_client.import_permalink("https://dps.report/declined").has_value());

    FakeHttpClient invalid_http{response(R"json({"success":1})json")};
    providers::WingmanClient invalid_client{invalid_http};
    const auto invalid = invalid_client.import_permalink("https://dps.report.evil/private");
    MANNY_CHECK(suite, !invalid.has_value());
    MANNY_CHECK(suite, invalid_http.calls == 0);

    const auto invalid_wvw = invalid_client.import_permalink("https://wvw.report.evil/private");
    MANNY_CHECK(suite, !invalid_wvw.has_value());
    MANNY_CHECK(suite, invalid_http.calls == 0);

    SequentialHttpClient unsafe_target{{
        response(R"json({"success":1})json"),
        response(
            R"json({"inQueue":true,"inDB":false,"targetURL":"https://gw2wingman.nevermindcreations.de.evil/log/private"})json"),
    }};
    providers::WingmanClient unsafe_client{unsafe_target};
    MANNY_CHECK(suite,
                !unsafe_client.import_permalink("https://dps.report/unsafe-target").has_value());
}

} // namespace

void run_wingman_client_tests(TestSuite& suite) {
    TempLog log{"wingman-log-payload"};
    const auto file = log.identity();
    request_and_success_tests(suite, file);
    ticket_and_permalink_tests(suite, file);
    response_and_status_tests(suite, file);
    transport_tests(suite, file);
    validation_tests(suite, log);
    permalink_import_tests(suite);
}

} // namespace manny_uploader::test
