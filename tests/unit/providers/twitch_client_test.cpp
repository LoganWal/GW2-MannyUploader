#include "manny_uploader/providers/twitch_client.hpp"

#include "support/test_suite.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace manny_uploader::test {
namespace {

using namespace std::chrono_literals;

constexpr std::string_view client_id = "abc123publicclient";

[[nodiscard]] std::vector<std::byte> bytes(std::string_view value) {
    const auto characters = std::span{value.data(), value.size()};
    const auto byte_view = std::as_bytes(characters);
    return {byte_view.begin(), byte_view.end()};
}

[[nodiscard]] std::string text(std::span<const std::byte> value) {
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

[[nodiscard]] std::string secret_text(const support::SecretValue& value) {
    return text(value.bytes());
}

[[nodiscard]] ports::HttpHeader header(std::string name, std::string value) {
    return ports::HttpHeader{
        .name = std::move(name),
        .value = std::move(value),
        .sensitivity = ports::HttpHeaderSensitivity::Public,
    };
}

[[nodiscard]] ports::HttpResponse response(std::uint16_t status, std::string_view body = {},
                                           std::vector<ports::HttpHeader> headers = {}) {
    return ports::HttpResponse{
        .status_code = status,
        .headers = std::move(headers),
        .body = bytes(body),
    };
}

[[nodiscard]] ports::HttpError http_error(ports::HttpErrorCode code) {
    return ports::HttpError{
        .code = code,
        .message = "private transport marker",
        .transport_code = 7,
        .body_error = std::nullopt,
        .system_error = std::nullopt,
    };
}

struct CapturedRequest {
    ports::HttpMethod method;
    std::string url;
    std::vector<ports::HttpHeader> headers;
    ports::HttpTimeouts timeouts;
    ports::HttpResponseLimits response_limits;
    std::uint64_t body_length{};
    std::string body;
};

class SequencedHttpClient final : public ports::IHttpClient {
  public:
    using Result = std::expected<ports::HttpResponse, ports::HttpError>;

    void push(Result result) {
        results_.push_back(std::move(result));
    }

    [[nodiscard]] std::expected<ports::HttpResponse, ports::HttpError>
    execute(ports::HttpRequest request, const std::stop_token& stop_token) const override {
        CapturedRequest captured{
            .method = request.method,
            .url = std::move(request.url),
            .headers = std::move(request.headers),
            .timeouts = request.timeouts,
            .response_limits = request.response_limits,
            .body_length = request.body ? request.body->content_length() : 0,
            .body = {},
        };
        if (request.body) {
            std::array<std::byte, 7> buffer{};
            std::vector<std::byte> body;
            while (body.size() < captured.body_length) {
                auto read = request.body->read(buffer, stop_token);
                if (!read || *read == 0 || *read > buffer.size()) {
                    return std::unexpected(http_error(ports::HttpErrorCode::BodyReadFailed));
                }
                body.insert(body.end(), buffer.begin(),
                            buffer.begin() + static_cast<std::ptrdiff_t>(*read));
            }
            captured.body = text(body);
        }
        requests_.push_back(std::move(captured));
        if (results_.empty()) {
            throw std::runtime_error{"Missing fake Twitch HTTP result"};
        }
        auto result = std::move(results_.front());
        results_.pop_front();
        return result;
    }

    [[nodiscard]] const std::vector<CapturedRequest>& requests() const noexcept {
        return requests_;
    }

  private:
    mutable std::deque<Result> results_;
    mutable std::vector<CapturedRequest> requests_;
};

[[nodiscard]] const ports::HttpHeader* find_header(const CapturedRequest& request,
                                                   std::string_view name) {
    for (const auto& current : request.headers) {
        if (current.name == name) {
            return &current;
        }
    }
    return nullptr;
}

[[nodiscard]] providers::TwitchClient make_client(SequencedHttpClient& http) {
    auto created = providers::TwitchClient::create(http, std::string{client_id});
    if (!created) {
        throw std::runtime_error{"Could not create Twitch test client"};
    }
    return std::move(*created);
}

void runtime_configuration_tests(TestSuite& suite) {
    SequencedHttpClient http;
    auto client = providers::TwitchClient::create_unconfigured(http);
    MANNY_CHECK(suite, !client.configured());
    MANNY_CHECK(suite, client.client_id().empty());
    const auto unconfigured = client.start_device_authorization();
    MANNY_CHECK(suite, !unconfigured.has_value());
    MANNY_CHECK(suite, unconfigured.error().detail.find("client ID") != std::string::npos);
    MANNY_CHECK(suite, http.requests().empty());

    const auto invalid = client.update_client_id("BAD-CLIENT-ID");
    MANNY_CHECK(suite, !invalid.has_value());
    MANNY_CHECK(suite, !client.configured());
    MANNY_CHECK(suite, client.update_client_id(std::string{client_id}).has_value());
    MANNY_CHECK(suite, client.configured());
    MANNY_CHECK(suite, client.client_id() == client_id);

    http.push(response(200, R"json({
      "device_code":"DEVICE-CODE",
      "expires_in":1800,
      "interval":5,
      "user_code":"ABCD-EFGH",
      "verification_uri":"https://www.twitch.tv/activate"
    })json"));
    MANNY_CHECK(suite, client.start_device_authorization().has_value());
    MANNY_CHECK(suite, http.requests().size() == 1);

    MANNY_CHECK(suite, client.update_client_id({}).has_value());
    MANNY_CHECK(suite, !client.configured());
}

void creation_and_device_start_tests(TestSuite& suite) {
    SequencedHttpClient http;
    const auto empty = providers::TwitchClient::create(http, "");
    const auto uppercase = providers::TwitchClient::create(http, "BAD-CLIENT-ID");
    MANNY_CHECK(suite, !empty.has_value());
    MANNY_CHECK(suite, !uppercase.has_value());
    MANNY_CHECK(suite, empty.error().detail.find("BAD-CLIENT-ID") == std::string::npos);

    http.push(response(200, R"json({
      "device_code":"DEVICE/SECRET+=",
      "expires_in":1800,
      "interval":5,
      "user_code":"ABCD-EFGH",
      "verification_uri":"https://www.twitch.tv/activate?public=true&device-code=ABCD-EFGH",
      "future":true
    })json"));
    auto client = make_client(http);
    auto started = client.start_device_authorization();
    MANNY_CHECK(suite, started.has_value());
    MANNY_CHECK(suite, client.client_id() == client_id);
    if (started) {
        MANNY_CHECK(suite, secret_text(started->device_code) == "DEVICE/SECRET+=");
        MANNY_CHECK(suite, started->user_code == "ABCD-EFGH");
        MANNY_CHECK(suite, started->verification_uri ==
                               "https://www.twitch.tv/activate?public=true&device-code=ABCD-EFGH");
        MANNY_CHECK(suite, started->expires_in == 1800s);
        MANNY_CHECK(suite, started->polling_interval == 5s);
    }
    MANNY_CHECK(suite, http.requests().size() == 1);
    if (!http.requests().empty()) {
        const auto& request = http.requests().front();
        MANNY_CHECK(suite, request.method == ports::HttpMethod::Post);
        MANNY_CHECK(suite, request.url == "https://id.twitch.tv/oauth2/device");
        MANNY_CHECK(suite,
                    request.body == "client_id=abc123publicclient&scopes=user%3Awrite%3Achat");
        MANNY_CHECK(suite, find_header(request, "Content-Type") != nullptr);
        MANNY_CHECK(suite, find_header(request, "Authorization") == nullptr);
        MANNY_CHECK(suite, request.timeouts.operation == 30s);
        MANNY_CHECK(suite, request.response_limits.max_body_bytes == 64U * 1024U);
    }

    constexpr std::array invalid_documents{
        std::string_view{"not-json"},
        std::string_view{R"({})"},
        std::string_view{
            R"({"device_code":"CODE","expires_in":0,"interval":5,"user_code":"USER","verification_uri":"https://www.twitch.tv/activate"})"},
        std::string_view{
            R"({"device_code":"CODE","expires_in":1800,"interval":0,"user_code":"USER","verification_uri":"https://www.twitch.tv/activate"})"},
        std::string_view{
            R"({"device_code":"CODE","expires_in":1800,"interval":5,"user_code":"USER","verification_uri":"https://www.twitch.tv.evil.example/activate"})"},
        std::string_view{
            R"({"device_code":"BAD\nCODE","expires_in":1800,"interval":5,"user_code":"USER","verification_uri":"https://www.twitch.tv/activate"})"},
    };
    for (const auto document : invalid_documents) {
        SequencedHttpClient invalid_http;
        invalid_http.push(response(200, document));
        auto invalid_client = make_client(invalid_http);
        const auto result = invalid_client.start_device_authorization();
        MANNY_CHECK(suite, !result.has_value());
        MANNY_CHECK(suite, result.error().disposition == providers::TwitchDisposition::Failed);
        MANNY_CHECK(suite, result.error().detail.find("CODE") == std::string::npos);
    }
}

void device_poll_and_refresh_tests(TestSuite& suite) {
    SequencedHttpClient http;
    http.push(response(400, R"({"status":400,"message":"authorization_pending"})"));
    http.push(response(200, R"json({
      "access_token":"ACCESS-TOKEN",
      "expires_in":14400,
      "refresh_token":"REFRESH/TOKEN+=",
      "scope":["user:write:chat"],
      "token_type":"bearer"
    })json"));
    http.push(response(200, R"json({
      "access_token":"NEW-ACCESS",
      "expires_in":12000,
      "refresh_token":"NEW/REFRESH+=",
      "scope":"user:write:chat",
      "token_type":"Bearer",
      "future":{"field":true}
    })json"));
    auto client = make_client(http);
    const auto device_code = support::SecretValue::from_text("DEVICE/SECRET+=");
    auto pending = client.poll_device_authorization(device_code);
    MANNY_CHECK(suite, pending.has_value());
    MANNY_CHECK(suite,
                pending && std::holds_alternative<providers::TwitchAuthorizationPending>(*pending));

    auto authorized = client.poll_device_authorization(device_code);
    MANNY_CHECK(suite, authorized.has_value());
    MANNY_CHECK(suite,
                authorized && std::holds_alternative<providers::TwitchTokenGrant>(*authorized));
    if (authorized && std::holds_alternative<providers::TwitchTokenGrant>(*authorized)) {
        const auto& grant = std::get<providers::TwitchTokenGrant>(*authorized);
        MANNY_CHECK(suite, secret_text(grant.access_token) == "ACCESS-TOKEN");
        MANNY_CHECK(suite, secret_text(grant.refresh_token) == "REFRESH/TOKEN+=");
        MANNY_CHECK(suite, grant.expires_in == 14400s);
        MANNY_CHECK(suite, grant.scopes == std::vector<std::string>({"user:write:chat"}));
    }
    MANNY_CHECK(suite, http.requests().size() == 2);
    if (http.requests().size() >= 2) {
        const auto& request = http.requests()[1];
        MANNY_CHECK(suite, request.url == "https://id.twitch.tv/oauth2/token");
        MANNY_CHECK(suite, request.body ==
                               "client_id=abc123publicclient&scopes=user%3Awrite%3Achat&"
                               "device_code=DEVICE%2FSECRET%2B%3D&grant_type=urn%3Aietf%3Aparams%"
                               "3Aoauth%3Agrant-type%3Adevice_code");
    }

    const auto refresh_token = support::SecretValue::from_text("OLD/REFRESH+=");
    auto refreshed = client.refresh_access_token(refresh_token);
    MANNY_CHECK(suite, refreshed.has_value());
    if (refreshed) {
        MANNY_CHECK(suite, secret_text(refreshed->access_token) == "NEW-ACCESS");
        MANNY_CHECK(suite, secret_text(refreshed->refresh_token) == "NEW/REFRESH+=");
        MANNY_CHECK(suite, refreshed->expires_in == 12000s);
    }
    MANNY_CHECK(suite, http.requests().size() == 3);
    if (http.requests().size() == 3) {
        MANNY_CHECK(suite, http.requests()[2].body ==
                               "grant_type=refresh_token&refresh_token=OLD%2FREFRESH%2B%3D&"
                               "client_id=abc123publicclient");
        MANNY_CHECK(suite, http.requests()[2].body.find("client_secret") == std::string::npos);
    }

    constexpr std::array invalid_grants{
        std::string_view{"not-json"},
        std::string_view{R"({})"},
        std::string_view{
            R"({"access_token":"PRIVATE-ACCESS","refresh_token":"PRIVATE-REFRESH","expires_in":14400,"scope":[],"token_type":"bearer"})"},
        std::string_view{
            R"({"access_token":"PRIVATE-ACCESS","refresh_token":"PRIVATE-REFRESH","expires_in":14400,"scope":["user:read:chat"],"token_type":"bearer"})"},
        std::string_view{
            R"({"access_token":"PRIVATE-ACCESS","refresh_token":"PRIVATE-REFRESH","expires_in":14400,"scope":["user:write:chat"],"token_type":"mac"})"},
    };
    for (const auto document : invalid_grants) {
        SequencedHttpClient invalid_http;
        invalid_http.push(response(200, document));
        auto invalid_client = make_client(invalid_http);
        const auto result = invalid_client.poll_device_authorization(device_code);
        MANNY_CHECK(suite, !result.has_value());
        MANNY_CHECK(suite, result.error().detail.find("PRIVATE") == std::string::npos);
    }

    SequencedHttpClient invalid_device_http;
    invalid_device_http.push(response(400, R"({"status":400,"message":"invalid device code"})"));
    auto invalid_device_client = make_client(invalid_device_http);
    const auto invalid_device = invalid_device_client.poll_device_authorization(device_code);
    MANNY_CHECK(suite, !invalid_device.has_value());
    MANNY_CHECK(suite, invalid_device.error().disposition == providers::TwitchDisposition::Failed);
}

void validation_and_revocation_tests(TestSuite& suite) {
    SequencedHttpClient http;
    http.push(response(200, R"json({
      "client_id":"abc123publicclient",
      "login":"streamer_name",
      "scopes":["user:write:chat"],
      "user_id":"141981764",
      "expires_in":14300,
      "future":true
    })json"));
    http.push(response(200));
    auto client = make_client(http);
    const auto access_token = support::SecretValue::from_text("ACCESS/PRIVATE+=");
    auto validated = client.validate_access_token(access_token);
    MANNY_CHECK(suite, validated.has_value());
    if (validated) {
        MANNY_CHECK(suite, validated->user_id == "141981764");
        MANNY_CHECK(suite, validated->login == "streamer_name");
        MANNY_CHECK(suite, validated->expires_in == 14300s);
        MANNY_CHECK(suite, validated->scopes == std::vector<std::string>({"user:write:chat"}));
    }
    MANNY_CHECK(suite, http.requests().size() == 1);
    if (!http.requests().empty()) {
        const auto& request = http.requests().front();
        MANNY_CHECK(suite, request.method == ports::HttpMethod::Get);
        MANNY_CHECK(suite, request.url == "https://id.twitch.tv/oauth2/validate");
        const auto* authorization = find_header(request, "Authorization");
        MANNY_CHECK(suite, authorization != nullptr);
        MANNY_CHECK(suite, authorization && authorization->value == "OAuth ACCESS/PRIVATE+=");
        MANNY_CHECK(suite, authorization && authorization->sensitivity ==
                                                ports::HttpHeaderSensitivity::Sensitive);
        MANNY_CHECK(suite, find_header(request, "Client-Id") == nullptr);
    }

    MANNY_CHECK(suite, client.revoke_access_token(access_token).has_value());
    MANNY_CHECK(suite, http.requests().size() == 2);
    if (http.requests().size() == 2) {
        MANNY_CHECK(suite, http.requests()[1].url == "https://id.twitch.tv/oauth2/revoke");
        MANNY_CHECK(suite, http.requests()[1].body ==
                               "client_id=abc123publicclient&token=ACCESS%2FPRIVATE%2B%3D");
    }

    constexpr std::array invalid_validations{
        std::string_view{"not-json"},
        std::string_view{
            R"({"client_id":"wrongclient","login":"streamer","scopes":["user:write:chat"],"user_id":"141981764","expires_in":100})"},
        std::string_view{
            R"({"client_id":"abc123publicclient","login":"Bad-Login","scopes":["user:write:chat"],"user_id":"141981764","expires_in":100})"},
        std::string_view{
            R"({"client_id":"abc123publicclient","login":"streamer","scopes":[],"user_id":"141981764","expires_in":100})"},
        std::string_view{
            R"({"client_id":"abc123publicclient","login":"streamer","scopes":["user:write:chat"],"user_id":"0","expires_in":100})"},
    };
    for (const auto document : invalid_validations) {
        SequencedHttpClient invalid_http;
        invalid_http.push(response(200, document));
        auto invalid_client = make_client(invalid_http);
        const auto result = invalid_client.validate_access_token(access_token);
        MANNY_CHECK(suite, !result.has_value());
        MANNY_CHECK(suite, result.error().disposition == providers::TwitchDisposition::Reconnect);
        MANNY_CHECK(suite, result.error().detail.find("streamer") == std::string::npos);
    }
}

void chat_tests(TestSuite& suite) {
    SequencedHttpClient http;
    http.push(
        response(200, R"({"data":[{"message_id":"message-123","is_sent":true}],"future":true})"));
    http.push(response(
        200,
        R"({"data":[{"is_sent":false,"drop_reason":{"code":"automod_held","message":"The message is being checked."}}]})"));
    http.push(response(200, R"({"data":[{"message_id":"emoji-500","is_sent":true}]})"));
    auto client = make_client(http);
    const auto access_token = support::SecretValue::from_text("CHAT-ACCESS-TOKEN");

    auto sent =
        client.send_chat_message("141981764", "Report: https://dps.report/abc", access_token);
    MANNY_CHECK(suite, sent.has_value());
    MANNY_CHECK(suite, sent && sent->is_sent);
    MANNY_CHECK(suite, sent && sent->message_id == "message-123");
    MANNY_CHECK(suite, sent && !sent->drop_reason.has_value());
    MANNY_CHECK(suite, http.requests().size() == 1);
    if (!http.requests().empty()) {
        const auto& request = http.requests().front();
        MANNY_CHECK(suite, request.method == ports::HttpMethod::Post);
        MANNY_CHECK(suite, request.url == "https://api.twitch.tv/helix/chat/messages");
        MANNY_CHECK(
            suite,
            request.body ==
                R"({"broadcaster_id":"141981764","sender_id":"141981764","message":"Report: https://dps.report/abc"})");
        MANNY_CHECK(suite, request.body.find("for_source_only") == std::string::npos);
        const auto* client_header = find_header(request, "Client-Id");
        const auto* authorization = find_header(request, "Authorization");
        MANNY_CHECK(suite, client_header && client_header->value == client_id);
        MANNY_CHECK(suite, authorization && authorization->value == "Bearer CHAT-ACCESS-TOKEN");
        MANNY_CHECK(suite, authorization && authorization->sensitivity ==
                                                ports::HttpHeaderSensitivity::Sensitive);
    }

    auto dropped = client.send_chat_message("141981764", "A test message", access_token);
    MANNY_CHECK(suite, dropped.has_value());
    MANNY_CHECK(suite, dropped && !dropped->is_sent);
    MANNY_CHECK(suite, dropped && !dropped->message_id.has_value());
    MANNY_CHECK(suite, dropped && dropped->drop_reason.has_value());
    MANNY_CHECK(suite, dropped && dropped->drop_reason->code == "automod_held");

    std::string five_hundred_emoji;
    for (std::size_t index = 0; index < 500; ++index) {
        five_hundred_emoji += "😀";
    }
    auto unicode = client.send_chat_message("141981764", five_hundred_emoji, access_token);
    MANNY_CHECK(suite, unicode.has_value());
    MANNY_CHECK(suite, unicode && unicode->is_sent);

    const auto calls_before_invalid = http.requests().size();
    auto too_long = five_hundred_emoji + "x";
    const auto long_result = client.send_chat_message("141981764", too_long, access_token);
    const auto bad_id = client.send_chat_message("not-an-id", "message", access_token);
    const auto bad_message = client.send_chat_message("141981764", "bad\nmessage", access_token);
    const auto empty_token = support::SecretValue{};
    const auto bad_token = client.send_chat_message("141981764", "message", empty_token);
    MANNY_CHECK(suite, !long_result.has_value());
    MANNY_CHECK(suite, !bad_id.has_value());
    MANNY_CHECK(suite, !bad_message.has_value());
    MANNY_CHECK(suite, !bad_token.has_value());
    MANNY_CHECK(suite, http.requests().size() == calls_before_invalid);

    constexpr std::array invalid_responses{
        std::string_view{"not-json"},
        std::string_view{R"({"data":[]})"},
        std::string_view{R"({"data":[{"is_sent":true}]})"},
        std::string_view{
            R"({"data":[{"message_id":"id","is_sent":true,"drop_reason":{"code":"drop","message":"bad"}}]})"},
        std::string_view{R"({"data":[{"is_sent":false}]})"},
        std::string_view{
            R"({"data":[{"message_id":"id","is_sent":false,"drop_reason":{"code":"drop","message":"bad"}}]})"},
    };
    for (const auto document : invalid_responses) {
        SequencedHttpClient invalid_http;
        invalid_http.push(response(200, document));
        auto invalid_client = make_client(invalid_http);
        const auto result = invalid_client.send_chat_message("141981764", "message", access_token);
        MANNY_CHECK(suite, !result.has_value());
        MANNY_CHECK(suite, result.error().disposition == providers::TwitchDisposition::Failed);
    }
}

void classification_and_cancellation_tests(TestSuite& suite) {
    const auto key = support::SecretValue::from_text("PRIVATE-TWITCH-TOKEN");
    {
        SequencedHttpClient http;
        http.push(std::unexpected(http_error(ports::HttpErrorCode::ConnectionFailed)));
        auto client = make_client(http);
        const auto result = client.start_device_authorization();
        MANNY_CHECK(suite, !result.has_value());
        MANNY_CHECK(suite, result.error().disposition == providers::TwitchDisposition::Retry);
        MANNY_CHECK(suite, result.error().retry_after == 30s);
        MANNY_CHECK(suite, result.error().detail.find("private") == std::string::npos);
    }
    {
        SequencedHttpClient http;
        http.push(response(429, {}, {header("Retry-After", "12")}));
        auto client = make_client(http);
        const auto result = client.start_device_authorization();
        MANNY_CHECK(suite, !result.has_value());
        MANNY_CHECK(suite, result.error().disposition == providers::TwitchDisposition::Retry);
        MANNY_CHECK(suite, result.error().retry_after == 12s);
        MANNY_CHECK(suite, result.error().http_status == std::uint16_t{429});
    }
    {
        SequencedHttpClient http;
        http.push(response(401, "PRIVATE SERVER BODY"));
        auto client = make_client(http);
        const auto result = client.validate_access_token(key);
        MANNY_CHECK(suite, !result.has_value());
        MANNY_CHECK(suite, result.error().disposition == providers::TwitchDisposition::Reconnect);
        MANNY_CHECK(suite, result.error().detail.find("PRIVATE") == std::string::npos);
    }
    {
        SequencedHttpClient http;
        http.push(response(400, "PRIVATE SERVER BODY"));
        auto client = make_client(http);
        const auto result = client.refresh_access_token(key);
        MANNY_CHECK(suite, !result.has_value());
        MANNY_CHECK(suite, result.error().disposition == providers::TwitchDisposition::Reconnect);
        MANNY_CHECK(suite, result.error().detail.find("PRIVATE-TWITCH-TOKEN") == std::string::npos);
    }
    {
        SequencedHttpClient http;
        http.push(response(503));
        auto client = make_client(http);
        const auto result = client.revoke_access_token(key);
        MANNY_CHECK(suite, !result.has_value());
        MANNY_CHECK(suite, result.error().disposition == providers::TwitchDisposition::Retry);
    }
    {
        SequencedHttpClient http;
        http.push(response(401));
        auto client = make_client(http);
        const auto result = client.send_chat_message("141981764", "message", key);
        MANNY_CHECK(suite, !result.has_value());
        MANNY_CHECK(suite, result.error().disposition == providers::TwitchDisposition::Reconnect);
    }

    SequencedHttpClient cancelled_http;
    auto cancelled_client = make_client(cancelled_http);
    std::stop_source cancellation;
    cancellation.request_stop();
    MANNY_CHECK(
        suite,
        cancelled_client.start_device_authorization(cancellation.get_token()).error().disposition ==
            providers::TwitchDisposition::Cancelled);
    MANNY_CHECK(suite, cancelled_client.poll_device_authorization(key, cancellation.get_token())
                               .error()
                               .disposition == providers::TwitchDisposition::Cancelled);
    MANNY_CHECK(
        suite,
        cancelled_client.validate_access_token(key, cancellation.get_token()).error().disposition ==
            providers::TwitchDisposition::Cancelled);
    MANNY_CHECK(
        suite,
        cancelled_client.refresh_access_token(key, cancellation.get_token()).error().disposition ==
            providers::TwitchDisposition::Cancelled);
    MANNY_CHECK(
        suite,
        cancelled_client.revoke_access_token(key, cancellation.get_token()).error().disposition ==
            providers::TwitchDisposition::Cancelled);
    MANNY_CHECK(
        suite,
        cancelled_client.send_chat_message("141981764", "message", key, cancellation.get_token())
                .error()
                .disposition == providers::TwitchDisposition::Cancelled);
    MANNY_CHECK(suite, cancelled_http.requests().empty());
}

} // namespace

void run_twitch_client_tests(TestSuite& suite) {
    runtime_configuration_tests(suite);
    creation_and_device_start_tests(suite);
    device_poll_and_refresh_tests(suite);
    validation_and_revocation_tests(suite);
    chat_tests(suite);
    classification_and_cancellation_tests(suite);
}

} // namespace manny_uploader::test
