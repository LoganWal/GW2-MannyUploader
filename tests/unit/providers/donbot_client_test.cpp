#include "manny_uploader/providers/donbot_client.hpp"

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
                ("manny-donbot-client-" + std::to_string(next_id.fetch_add(1)) + ".zevtc");
        std::ofstream stream{path_, std::ios::binary | std::ios::trunc};
        stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        if (!stream) {
            throw std::runtime_error{"Could not write DonBot test fixture"};
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

  private:
    std::filesystem::path path_;
};

[[nodiscard]] ports::HttpHeader header(std::string name, std::string value) {
    return ports::HttpHeader{
        .name = std::move(name),
        .value = std::move(value),
        .sensitivity = ports::HttpHeaderSensitivity::Public,
    };
}

[[nodiscard]] ports::HttpResponse response(std::uint16_t status,
                                           std::vector<ports::HttpHeader> headers = {},
                                           std::string_view body = {}) {
    return ports::HttpResponse{
        .status_code = status,
        .headers = std::move(headers),
        .body = bytes(body),
    };
}

[[nodiscard]] ports::HttpError http_error(ports::HttpErrorCode code) {
    return ports::HttpError{
        .code = code,
        .message = "private transport detail",
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
            std::array<std::byte, 5> buffer{};
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
            throw std::runtime_error{"Missing fake DonBot HTTP result"};
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
    const auto found = std::ranges::find(request.headers, name, &ports::HttpHeader::name);
    return found == request.headers.end() ? nullptr : &*found;
}

[[nodiscard]] std::string secret_text(const support::SecretValue& value) {
    return text(value.bytes());
}

[[nodiscard]] std::vector<ports::HttpHeader>
created_headers(std::string location = "/api/upload/tus/file-123") {
    std::vector<ports::HttpHeader> result;
    result.push_back(header("Location", std::move(location)));
    result.push_back(header("Tus-Resumable", "1.0.0"));
    result.push_back(header("X-Log-Upload-Id", "42"));
    return result;
}

[[nodiscard]] std::vector<ports::HttpHeader> patched_headers(std::uint64_t offset) {
    std::vector<ports::HttpHeader> result;
    result.push_back(header("Tus-Resumable", "1.0.0"));
    result.push_back(header("Upload-Offset", std::to_string(offset)));
    return result;
}

void verification_tests(TestSuite& suite) {
    SequencedHttpClient http;
    http.push(response(200, {}, R"json({
      "accountName":"Player.1234",
      "guilds":[
        {"guildId":"123456789012345678","guildName":"Raid Guild"},
        {"guildId":"223456789012345678","guildName":"Second Guild","future":true}
      ],
      "future":{"nested":[1,true]}
    })json"));
    providers::DonBotClient client{http};
    const auto key = support::SecretValue::from_text("AAAA-BBBB-CCCC-DDDD");
    const auto verified = client.verify("https://donbot-api.walmslo.com/", key);

    MANNY_CHECK(suite, verified.has_value());
    MANNY_CHECK(suite, verified && verified->account_name == "Player.1234");
    MANNY_CHECK(suite, verified && verified->guilds.size() == 2);
    MANNY_CHECK(suite, verified && verified->guilds.front().guild_id == "123456789012345678");
    MANNY_CHECK(suite, verified && !verified->discord_summary_delivery_v1);
    MANNY_CHECK(suite, http.requests().size() == 1);
    if (!http.requests().empty()) {
        const auto& request = http.requests().front();
        MANNY_CHECK(suite, request.method == ports::HttpMethod::Post);
        MANNY_CHECK(suite, request.url == "https://donbot-api.walmslo.com/api/upload/gw2/guilds");
        MANNY_CHECK(suite, request.url.find(secret_text(key)) == std::string::npos);
        MANNY_CHECK(suite, find_header(request, "X-GW2-API-Key") == nullptr);
        MANNY_CHECK(suite, find_header(request, "Content-Type") != nullptr);
        MANNY_CHECK(suite, request.body == R"({"apiKey":"AAAA-BBBB-CCCC-DDDD"})");
        MANNY_CHECK(suite, request.timeouts.operation == std::chrono::minutes{15});
        MANNY_CHECK(suite, request.response_limits.max_body_bytes == 256U * 1024U);
    }

    constexpr std::array invalid_documents{
        std::string_view{"not-json"},
        std::string_view{R"({})"},
        std::string_view{
            R"({"accountName":"Player.1234","guilds":[{"guildId":"0","guildName":"Guild"}]})"},
        std::string_view{
            R"({"accountName":"Player.1234","guilds":[{"guildId":"1","guildName":"Guild"},{"guildId":"1","guildName":"Duplicate"}]})"},
        std::string_view{R"({"accountName":"Bad\nName","guilds":[]})"},
    };
    for (const auto document : invalid_documents) {
        SequencedHttpClient invalid_http;
        invalid_http.push(response(200, {}, document));
        providers::DonBotClient invalid_client{invalid_http};
        const auto result = invalid_client.verify(providers::donbot_default_api_base, key);
        MANNY_CHECK(suite, !result.has_value());
        MANNY_CHECK(suite, result.error().disposition == providers::DonBotDisposition::Failed);
        MANNY_CHECK(suite, result.error().detail.find("Player.1234") == std::string::npos);
    }

    SequencedHttpClient delivery_http;
    delivery_http.push(response(200, {}, R"json({
      "accountName":"Player.1234",
      "capabilities":["future-capability","discord-summary-delivery-v1"],
      "guilds":[{
        "guildId":"123456789012345678",
        "guildName":"Raid Guild",
        "discordDelivery":{
          "enabled":true,
          "defaultsAvailable":true,
          "channelOverrideAllowed":true,
          "enabledMessageKinds":["pve-summary","wvw-summary","wvw-advanced","wvw-stream"],
          "channels":[
            {"channelId":"223456789012345678","channelName":"logs"},
            {"channelId":"323456789012345678","channelName":"command-logs"}
          ]
        }
      }]
    })json"));
    providers::DonBotClient delivery_client{delivery_http};
    const auto delivery_verified = delivery_client.verify(providers::donbot_default_api_base, key);
    MANNY_CHECK(suite, delivery_verified.has_value());
    MANNY_CHECK(suite, delivery_verified && delivery_verified->discord_summary_delivery_v1);
    MANNY_CHECK(suite, delivery_verified && delivery_verified->guilds.size() == 1);
    if (delivery_verified && !delivery_verified->guilds.empty()) {
        const auto& delivery = delivery_verified->guilds.front().discord_delivery;
        MANNY_CHECK(suite, delivery.enabled && delivery.defaults_available &&
                               delivery.channel_override_allowed);
        MANNY_CHECK(suite, delivery.pve_summary && delivery.wvw_summary && delivery.wvw_advanced &&
                               delivery.wvw_stream);
        MANNY_CHECK(suite, delivery.channels.size() == 2);
        MANNY_CHECK(suite, delivery.channels.front().channel_name == "logs");
    }

    SequencedHttpClient malformed_delivery_http;
    malformed_delivery_http.push(response(
        200, {},
        R"json({"accountName":"Player.1234","capabilities":["discord-summary-delivery-v1"],"guilds":[{"guildId":"123","guildName":"Guild","discordDelivery":{"enabled":true,"defaultsAvailable":true,"channelOverrideAllowed":true,"enabledMessageKinds":["pve-summary"],"channels":[{"channelId":"223","channelName":"one"},{"channelId":"223","channelName":"duplicate"}]}}]})json"));
    providers::DonBotClient malformed_delivery_client{malformed_delivery_http};
    MANNY_CHECK(
        suite,
        !malformed_delivery_client.verify(providers::donbot_default_api_base, key).has_value());

    constexpr std::array invalid_delivery_policies{
        std::string_view{
            R"json({"accountName":"Player.1234","capabilities":["discord-summary-delivery-v1"],"guilds":[{"guildId":"123","guildName":"Guild","discordDelivery":{"enabled":true,"defaultsAvailable":true,"channelOverrideAllowed":true,"enabledMessageKinds":["future-summary"],"channels":[]}}]})json"},
        std::string_view{
            R"json({"accountName":"Player.1234","capabilities":["discord-summary-delivery-v1"],"guilds":[{"guildId":"123","guildName":"Guild","discordDelivery":{"enabled":true,"defaultsAvailable":true,"channelOverrideAllowed":true,"enabledMessageKinds":["pve-summary","wvw-summary","wvw-advanced","wvw-stream","future-summary"],"channels":[]}}]})json"},
        std::string_view{
            R"json({"accountName":"Player.1234","capabilities":["discord-summary-delivery-v1"],"guilds":[{"guildId":"123","guildName":"Guild","discordDelivery":{"enabled":true,"defaultsAvailable":true,"channelOverrideAllowed":true,"enabledMessageKinds":[],"channels":[]}}]})json"},
    };
    for (const auto document : invalid_delivery_policies) {
        SequencedHttpClient invalid_policy_http;
        invalid_policy_http.push(response(200, {}, document));
        providers::DonBotClient invalid_policy_client{invalid_policy_http};
        MANNY_CHECK(
            suite,
            !invalid_policy_client.verify(providers::donbot_default_api_base, key).has_value());
    }
}

void discord_delivery_upload_tests(TestSuite& suite, const domain::LogFileIdentity& file) {
    const auto key = support::SecretValue::from_text("VALID-KEY");
    auto headers = created_headers();
    headers.push_back(header("X-DonBot-Discord-Delivery", "accepted"));
    SequencedHttpClient http;
    http.push(response(201, std::move(headers)));
    http.push(response(204, patched_headers(file.size)));
    http.push(response(200, {},
                       "data: {\"stage\":\"complete\",\"fightLogId\":314,\"discordDelivery\":{"
                       "\"requested\":true,\"outcome\":\"partial\",\"sent\":2,\"skipped\":1,"
                       "\"failed\":0,\"ambiguous\":0}}\n\n"));
    providers::DonBotClient client{http};
    const auto uploaded =
        client.upload(file, providers::donbot_default_api_base, "123456789012345678", key,
                      providers::DonBotDiscordDeliveryRequest{
                          .mode = domain::DonBotDiscordDeliveryMode::ChannelOverride,
                          .channel_id = "223456789012345678",
                      });
    MANNY_CHECK(suite, uploaded.has_value());
    MANNY_CHECK(suite, uploaded && uploaded->discord_delivery.outcome ==
                                       domain::DonBotDiscordDeliveryOutcome::Partial);
    MANNY_CHECK(suite, uploaded && uploaded->discord_delivery.sent == 2);
    MANNY_CHECK(suite, uploaded && uploaded->discord_delivery.skipped == 1);
    MANNY_CHECK(suite, http.requests().size() == 3);
    if (!http.requests().empty()) {
        const auto* metadata = find_header(http.requests().front(), "Upload-Metadata");
        MANNY_CHECK(suite, metadata != nullptr);
        MANNY_CHECK(suite, metadata && metadata->value ==
                                           "filename dXBsb2FkLnpldnRj,guildid "
                                           "MTIzNDU2Nzg5MDEyMzQ1Njc4,wingman ZmFsc2U=,"
                                           "discorddelivery Y2hhbm5lbF9vdmVycmlkZQ==,"
                                           "discordchannelid MjIzNDU2Nzg5MDEyMzQ1Njc4");
    }

    SequencedHttpClient missing_ack_http;
    missing_ack_http.push(response(201, created_headers()));
    providers::DonBotClient missing_ack_client{missing_ack_http};
    const auto missing_ack = missing_ack_client.upload(
        file, providers::donbot_default_api_base, "123456789012345678", key,
        providers::DonBotDiscordDeliveryRequest{
            .mode = domain::DonBotDiscordDeliveryMode::GuildDefaults,
            .channel_id = {},
        });
    MANNY_CHECK(suite, !missing_ack.has_value());
    MANNY_CHECK(suite, missing_ack_http.requests().size() == 1);
    if (!missing_ack_http.requests().empty()) {
        const auto* metadata = find_header(missing_ack_http.requests().front(), "Upload-Metadata");
        MANNY_CHECK(suite, metadata != nullptr);
        MANNY_CHECK(suite, metadata && metadata->value ==
                                           "filename dXBsb2FkLnpldnRj,guildid "
                                           "MTIzNDU2Nzg5MDEyMzQ1Njc4,wingman ZmFsc2U=,"
                                           "discorddelivery Z3VpbGRfZGVmYXVsdHM=");
    }

    auto no_id_headers = created_headers();
    no_id_headers.pop_back();
    no_id_headers.push_back(header("X-DonBot-Discord-Delivery", "accepted"));
    SequencedHttpClient no_id_http;
    no_id_http.push(response(201, std::move(no_id_headers)));
    providers::DonBotClient no_id_client{no_id_http};
    const auto no_id =
        no_id_client.upload(file, providers::donbot_default_api_base, "123456789012345678", key,
                            providers::DonBotDiscordDeliveryRequest{
                                .mode = domain::DonBotDiscordDeliveryMode::GuildDefaults,
                                .channel_id = {},
                            });
    MANNY_CHECK(suite, !no_id.has_value());
    MANNY_CHECK(suite, no_id_http.requests().size() == 1);

    const auto mismatched_receipt =
        "data: {\"stage\":\"complete\",\"fightLogId\":314,\"discordDelivery\":{"
        "\"requested\":false,\"outcome\":\"not_requested\",\"sent\":0,\"skipped\":0,"
        "\"failed\":0,\"ambiguous\":0}}\n\n";
    SequencedHttpClient mismatched_receipt_http;
    auto mismatched_headers = created_headers();
    mismatched_headers.push_back(header("X-DonBot-Discord-Delivery", "accepted"));
    mismatched_receipt_http.push(response(201, std::move(mismatched_headers)));
    mismatched_receipt_http.push(response(204, patched_headers(file.size)));
    mismatched_receipt_http.push(response(200, {}, mismatched_receipt));
    providers::DonBotClient mismatched_receipt_client{mismatched_receipt_http};
    MANNY_CHECK(suite,
                !mismatched_receipt_client
                     .upload(file, providers::donbot_default_api_base, "123456789012345678", key,
                             providers::DonBotDiscordDeliveryRequest{
                                 .mode = domain::DonBotDiscordDeliveryMode::GuildDefaults,
                                 .channel_id = {},
                             })
                     .has_value());

    SequencedHttpClient nonterminal_receipt_http;
    auto nonterminal_headers = created_headers();
    nonterminal_headers.push_back(header("X-DonBot-Discord-Delivery", "accepted"));
    nonterminal_receipt_http.push(response(201, std::move(nonterminal_headers)));
    nonterminal_receipt_http.push(response(204, patched_headers(file.size)));
    nonterminal_receipt_http.push(
        response(200, {},
                 "data: {\"stage\":\"complete\",\"fightLogId\":314,\"discordDelivery\":{"
                 "\"requested\":true,\"outcome\":\"sent\",\"sent\":1,\"skipped\":0,"
                 "\"failed\":0,\"ambiguous\":0}}\n\n"
                 "data: {\"stage\":\"parsing\"}\n\n"));
    providers::DonBotClient nonterminal_receipt_client{nonterminal_receipt_http};
    MANNY_CHECK(suite,
                !nonterminal_receipt_client
                     .upload(file, providers::donbot_default_api_base, "123456789012345678", key,
                             providers::DonBotDiscordDeliveryRequest{
                                 .mode = domain::DonBotDiscordDeliveryMode::GuildDefaults,
                                 .channel_id = {},
                             })
                     .has_value());
}

void upload_success_tests(TestSuite& suite, const domain::LogFileIdentity& file,
                          std::string_view payload) {
    constexpr std::array locations{
        std::string_view{"file-123"},
        std::string_view{"/api/upload/tus/file-123"},
        std::string_view{"https://donbot-api.walmslo.com/api/upload/tus/file-123"},
    };
    for (const auto location : locations) {
        SequencedHttpClient http;
        http.push(response(201, created_headers(std::string{location})));
        http.push(response(204, patched_headers(file.size)));
        http.push(response(200, {},
                           "data: {\"stage\":\"parsing\",\"message\":\"Working\"}\n\n"
                           "data: {\"stage\":\"complete\",\"message\":\"Done\","
                           "\"fightLogId\":314}\n\n"));
        providers::DonBotClient client{http};
        const auto key = support::SecretValue::from_text("SECRET-API-KEY-123");
        const auto uploaded =
            client.upload(file, providers::donbot_default_api_base, "123456789012345678", key);

        MANNY_CHECK(suite, uploaded.has_value());
        MANNY_CHECK(suite, uploaded && uploaded->upload_id == std::uint64_t{42});
        MANNY_CHECK(suite, uploaded && uploaded->fight_log_id == std::uint64_t{314});
        MANNY_CHECK(suite, http.requests().size() == 3);
        if (http.requests().size() != 3) {
            continue;
        }
        const auto& create = http.requests()[0];
        const auto& patch = http.requests()[1];
        const auto& progress = http.requests()[2];
        MANNY_CHECK(suite, create.method == ports::HttpMethod::Post);
        MANNY_CHECK(suite, create.url == "https://donbot-api.walmslo.com/api/upload/tus");
        MANNY_CHECK(suite, create.body.empty());
        MANNY_CHECK(suite, find_header(create, "Upload-Length") != nullptr);
        MANNY_CHECK(suite,
                    find_header(create, "Upload-Length") &&
                        find_header(create, "Upload-Length")->value == std::to_string(file.size));
        const auto* metadata = find_header(create, "Upload-Metadata");
        MANNY_CHECK(suite, metadata != nullptr);
        MANNY_CHECK(suite, metadata && metadata->value.find("filename dXBsb2FkLnpldnRj") !=
                                           std::string::npos);
        MANNY_CHECK(suite, metadata && metadata->value.find("guildid MTIzNDU2Nzg5MDEyMzQ1Njc4") !=
                                           std::string::npos);
        MANNY_CHECK(suite,
                    metadata && metadata->value.find("wingman ZmFsc2U=") != std::string::npos);
        const auto* create_key = find_header(create, "X-GW2-API-Key");
        MANNY_CHECK(suite, create_key != nullptr);
        MANNY_CHECK(suite, create_key && create_key->value == secret_text(key));
        MANNY_CHECK(suite, create_key &&
                               create_key->sensitivity == ports::HttpHeaderSensitivity::Sensitive);

        MANNY_CHECK(suite, patch.method == ports::HttpMethod::Patch);
        MANNY_CHECK(suite, patch.url == "https://donbot-api.walmslo.com/api/upload/tus/file-123");
        MANNY_CHECK(suite, patch.body == payload);
        MANNY_CHECK(suite, find_header(patch, "Upload-Offset") != nullptr);
        MANNY_CHECK(suite, find_header(patch, "Content-Type") &&
                               find_header(patch, "Content-Type")->value ==
                                   "application/offset+octet-stream");
        const auto* patch_key = find_header(patch, "X-GW2-API-Key");
        MANNY_CHECK(suite, patch_key && patch_key->value == secret_text(key));
        MANNY_CHECK(suite, patch.url.find(secret_text(key)) == std::string::npos);
        MANNY_CHECK(suite, progress.method == ports::HttpMethod::Get);
        MANNY_CHECK(suite, progress.url == "https://donbot-api.walmslo.com/api/upload/stream/42");
        const auto* progress_key = find_header(progress, "X-GW2-API-Key");
        MANNY_CHECK(suite, progress_key && progress_key->value == secret_text(key));
        MANNY_CHECK(suite, progress_key && progress_key->sensitivity ==
                                               ports::HttpHeaderSensitivity::Sensitive);
    }

    SequencedHttpClient no_id_http;
    auto headers = created_headers();
    headers.pop_back();
    no_id_http.push(response(201, std::move(headers)));
    no_id_http.push(response(204, patched_headers(file.size)));
    providers::DonBotClient no_id_client{no_id_http};
    const auto key = support::SecretValue::from_text("VALID-KEY");
    const auto no_id =
        no_id_client.upload(file, providers::donbot_default_api_base, "123456789012345678", key);
    MANNY_CHECK(suite, no_id.has_value());
    MANNY_CHECK(suite, no_id && !no_id->upload_id.has_value());
}

void unsafe_location_tests(TestSuite& suite, const domain::LogFileIdentity& file) {
    constexpr std::array locations{
        std::string_view{"https://evil.example/api/upload/tus/file"},
        std::string_view{"https://donbot-api.walmslo.com.evil.example/api/upload/tus/file"},
        std::string_view{"https://user@donbot-api.walmslo.com/api/upload/tus/file"},
        std::string_view{"/other/file"},
        std::string_view{"../file"},
        std::string_view{"file/extra"},
        std::string_view{"file?key=value"},
        std::string_view{"file#fragment"},
        std::string_view{"file%2fextra"},
        std::string_view{""},
    };
    const auto key = support::SecretValue::from_text("VALID-KEY");
    for (const auto location : locations) {
        SequencedHttpClient http;
        http.push(response(201, created_headers(std::string{location})));
        providers::DonBotClient client{http};
        const auto result =
            client.upload(file, providers::donbot_default_api_base, "123456789012345678", key);
        MANNY_CHECK(suite, !result.has_value());
        MANNY_CHECK(suite, result.error().disposition == providers::DonBotDisposition::Failed);
        MANNY_CHECK(suite, http.requests().size() == 1);
    }
}

void protocol_response_tests(TestSuite& suite, const domain::LogFileIdentity& file) {
    const auto key = support::SecretValue::from_text("VALID-KEY");

    std::vector<std::vector<ports::HttpHeader>> invalid_creation_headers;
    invalid_creation_headers.push_back({header("Tus-Resumable", "1.0.0")});
    invalid_creation_headers.push_back(
        {header("Location", "file"), header("Tus-Resumable", "0.2.2")});
    invalid_creation_headers.push_back({header("Location", "file"), header("Location", "second"),
                                        header("Tus-Resumable", "1.0.0")});
    invalid_creation_headers.push_back({header("Location", "file"),
                                        header("Tus-Resumable", "1.0.0"),
                                        header("X-Log-Upload-Id", "0")});
    for (auto& headers : invalid_creation_headers) {
        SequencedHttpClient http;
        http.push(response(201, std::move(headers)));
        providers::DonBotClient client{http};
        const auto result =
            client.upload(file, providers::donbot_default_api_base, "123456789012345678", key);
        MANNY_CHECK(suite, !result.has_value());
        MANNY_CHECK(suite, result.error().disposition == providers::DonBotDisposition::Failed);
        MANNY_CHECK(suite, http.requests().size() == 1);
    }

    std::vector<std::vector<ports::HttpHeader>> invalid_patch_headers;
    invalid_patch_headers.push_back({header("Tus-Resumable", "1.0.0")});
    invalid_patch_headers.push_back(
        {header("Tus-Resumable", "0.2.2"), header("Upload-Offset", std::to_string(file.size))});
    invalid_patch_headers.push_back({header("Tus-Resumable", "1.0.0"),
                                     header("Upload-Offset", std::to_string(file.size)),
                                     header("Upload-Offset", std::to_string(file.size))});
    for (auto& headers : invalid_patch_headers) {
        SequencedHttpClient http;
        http.push(response(201, created_headers()));
        http.push(response(204, std::move(headers)));
        providers::DonBotClient client{http};
        const auto result =
            client.upload(file, providers::donbot_default_api_base, "123456789012345678", key);
        MANNY_CHECK(suite, !result.has_value());
        MANNY_CHECK(suite, result.error().disposition == providers::DonBotDisposition::Failed);
        MANNY_CHECK(suite, http.requests().size() == 2);
    }

    SequencedHttpClient forbidden;
    forbidden.push(response(403));
    providers::DonBotClient forbidden_client{forbidden};
    const auto rejected = forbidden_client.verify(providers::donbot_default_api_base, key);
    MANNY_CHECK(suite, !rejected.has_value());
    MANNY_CHECK(suite, rejected.error().disposition == providers::DonBotDisposition::Failed);
    MANNY_CHECK(suite, rejected.error().http_status == std::uint16_t{403});

    SequencedHttpClient unavailable;
    unavailable.push(response(502));
    providers::DonBotClient unavailable_client{unavailable};
    const auto retry = unavailable_client.verify(providers::donbot_default_api_base, key);
    MANNY_CHECK(suite, !retry.has_value());
    MANNY_CHECK(suite, retry.error().disposition == providers::DonBotDisposition::Retry);
    MANNY_CHECK(suite, retry.error().retry_after == std::chrono::seconds{30});
}

void failure_policy_tests(TestSuite& suite, const domain::LogFileIdentity& file) {
    const auto key = support::SecretValue::from_text("VALID-KEY");

    SequencedHttpClient create_transport;
    create_transport.push(std::unexpected(http_error(ports::HttpErrorCode::Timeout)));
    providers::DonBotClient create_transport_client{create_transport};
    const auto create_retry = create_transport_client.upload(
        file, providers::donbot_default_api_base, "123456789012345678", key);
    MANNY_CHECK(suite, !create_retry.has_value());
    MANNY_CHECK(suite, create_retry.error().disposition == providers::DonBotDisposition::Retry);

    SequencedHttpClient rate_limited;
    rate_limited.push(response(429, {header("Retry-After", "17")}));
    providers::DonBotClient rate_limited_client{rate_limited};
    const auto retry = rate_limited_client.upload(file, providers::donbot_default_api_base,
                                                  "123456789012345678", key);
    MANNY_CHECK(suite, !retry.has_value());
    MANNY_CHECK(suite, retry.error().disposition == providers::DonBotDisposition::Retry);
    MANNY_CHECK(suite, retry.error().retry_after == std::chrono::seconds{17});

    SequencedHttpClient patch_transport;
    patch_transport.push(response(201, created_headers()));
    patch_transport.push(std::unexpected(http_error(ports::HttpErrorCode::Timeout)));
    providers::DonBotClient patch_transport_client{patch_transport};
    const auto ambiguous = patch_transport_client.upload(file, providers::donbot_default_api_base,
                                                         "123456789012345678", key);
    MANNY_CHECK(suite, !ambiguous.has_value());
    MANNY_CHECK(suite, ambiguous.error().disposition == providers::DonBotDisposition::Failed);
    MANNY_CHECK(suite, !ambiguous.error().retry_after.has_value());

    SequencedHttpClient patch_status;
    patch_status.push(response(201, created_headers()));
    patch_status.push(response(503));
    providers::DonBotClient patch_status_client{patch_status};
    const auto ambiguous_status = patch_status_client.upload(
        file, providers::donbot_default_api_base, "123456789012345678", key);
    MANNY_CHECK(suite, !ambiguous_status.has_value());
    MANNY_CHECK(suite,
                ambiguous_status.error().disposition == providers::DonBotDisposition::Failed);

    SequencedHttpClient bad_offset;
    bad_offset.push(response(201, created_headers()));
    bad_offset.push(response(204, patched_headers(file.size - 1)));
    providers::DonBotClient bad_offset_client{bad_offset};
    const auto offset_result = bad_offset_client.upload(file, providers::donbot_default_api_base,
                                                        "123456789012345678", key);
    MANNY_CHECK(suite, !offset_result.has_value());

    SequencedHttpClient invalid_input;
    providers::DonBotClient invalid_input_client{invalid_input};
    for (const auto base : {std::string_view{"http://donbot.example"},
                            std::string_view{"https://user@donbot.example"},
                            std::string_view{"https://donbot.example?x=1"}}) {
        const auto result = invalid_input_client.upload(file, base, "123", key);
        MANNY_CHECK(suite, !result.has_value());
    }
    MANNY_CHECK(suite,
                !invalid_input_client.upload(file, providers::donbot_default_api_base, "0", key)
                     .has_value());
    const auto invalid_key = support::SecretValue::from_text("bad key");
    MANNY_CHECK(suite, !invalid_input_client
                            .upload(file, providers::donbot_default_api_base, "123", invalid_key)
                            .has_value());
    MANNY_CHECK(suite, invalid_input.requests().empty());

    std::stop_source stopped;
    stopped.request_stop();
    MANNY_CHECK(suite, !invalid_input_client
                            .upload(file, providers::donbot_default_api_base, "123", key, {},
                                    stopped.get_token())
                            .has_value());
    MANNY_CHECK(suite, !invalid_input_client
                            .verify(providers::donbot_default_api_base, key, stopped.get_token())
                            .has_value());
}

void permalink_import_tests(TestSuite& suite) {
    const auto key = support::SecretValue::from_text("VALID-KEY");
    SequencedHttpClient http;
    http.push(response(
        202, {},
        R"json({"uploadId":42,"fightLogId":null,"status":"pending","duplicate":false})json"));
    http.push(response(200, {}, R"sse(data: {"stage":"complete","fightLogId":314}

)sse"));
    providers::DonBotClient client{http};
    const auto imported =
        client.import_permalink("https://dps.report/abc-123", providers::donbot_default_api_base,
                                "123456789012345678", key);
    MANNY_CHECK(suite, imported.has_value());
    MANNY_CHECK(suite, imported && imported->upload_id == 42);
    MANNY_CHECK(suite, imported && imported->fight_log_id == 314);
    MANNY_CHECK(suite, http.requests().size() == 2);
    if (http.requests().size() == 2) {
        const auto& request = http.requests()[0];
        MANNY_CHECK(suite, request.method == ports::HttpMethod::Post);
        MANNY_CHECK(suite, request.url == "https://donbot-api.walmslo.com/api/upload/gw2/url");
        MANNY_CHECK(suite,
                    request.body ==
                        R"({"url":"https://dps.report/abc-123","guildId":"123456789012345678"})");
        const auto* api_key = find_header(request, "X-GW2-API-Key");
        MANNY_CHECK(suite, api_key != nullptr && api_key->value == "VALID-KEY");
        MANNY_CHECK(suite, api_key != nullptr &&
                               api_key->sensitivity == ports::HttpHeaderSensitivity::Sensitive);
        MANNY_CHECK(suite, find_header(request, "Content-Type") != nullptr);
        MANNY_CHECK(suite, http.requests()[1].method == ports::HttpMethod::Get);
        MANNY_CHECK(suite, http.requests()[1].url ==
                               "https://donbot-api.walmslo.com/api/upload/stream/42");
        const auto* progress_key = find_header(http.requests()[1], "X-GW2-API-Key");
        MANNY_CHECK(suite, progress_key && progress_key->value == "VALID-KEY");
        MANNY_CHECK(suite, progress_key && progress_key->sensitivity ==
                                               ports::HttpHeaderSensitivity::Sensitive);
    }

    SequencedHttpClient duplicate_http;
    duplicate_http.push(response(
        200, {},
        R"json({"uploadId":43,"fightLogId":315,"status":"complete","duplicate":true})json"));
    providers::DonBotClient duplicate_client{duplicate_http};
    const auto duplicate = duplicate_client.import_permalink(
        "https://dps.report/existing", providers::donbot_default_api_base, "123", key);
    MANNY_CHECK(suite, duplicate.has_value());
    MANNY_CHECK(suite, duplicate && duplicate->fight_log_id == 315);
    MANNY_CHECK(suite, duplicate_http.requests().size() == 1);

    SequencedHttpClient wvw_http;
    wvw_http.push(response(
        200, {},
        R"json({"uploadId":44,"fightLogId":316,"status":"complete","duplicate":false})json"));
    providers::DonBotClient wvw_client{wvw_http};
    const auto wvw = wvw_client.import_permalink("https://wvw.report/KKNj-20260318-212757_wvw",
                                                 providers::donbot_default_api_base, "123", key);
    MANNY_CHECK(suite, wvw.has_value());
    MANNY_CHECK(suite, wvw_http.requests().size() == 1);
    if (wvw_http.requests().size() == 1) {
        MANNY_CHECK(suite,
                    wvw_http.requests()[0].body ==
                        R"({"url":"https://wvw.report/KKNj-20260318-212757_wvw","guildId":"123"})");
    }

    SequencedHttpClient invalid_http;
    providers::DonBotClient invalid_client{invalid_http};
    MANNY_CHECK(suite, !invalid_client
                            .import_permalink("https://dps.report.evil/private",
                                              providers::donbot_default_api_base, "123", key)
                            .has_value());
    MANNY_CHECK(suite, invalid_http.requests().empty());
    MANNY_CHECK(suite, !invalid_client
                            .import_permalink("https://wvw.report.evil/private",
                                              providers::donbot_default_api_base, "123", key)
                            .has_value());
    MANNY_CHECK(suite, invalid_http.requests().empty());

    SequencedHttpClient retry_http;
    retry_http.push(std::unexpected(http_error(ports::HttpErrorCode::Timeout)));
    providers::DonBotClient retry_client{retry_http};
    const auto retry = retry_client.import_permalink(
        "https://dps.report/retry", providers::donbot_default_api_base, "123", key);
    MANNY_CHECK(suite, !retry.has_value());
    MANNY_CHECK(suite, retry.error().disposition == providers::DonBotDisposition::Retry);

    SequencedHttpClient delivery_http;
    delivery_http.push(response(
        202, {},
        R"json({"uploadId":44,"fightLogId":null,"status":"pending","duplicate":false,"discordDeliveryAccepted":true})json"));
    delivery_http.push(response(
        200, {},
        R"sse(data: {"stage":"complete","fightLogId":316,"discordDelivery":{"requested":true,"outcome":"sent","sent":2,"skipped":0,"failed":0,"ambiguous":0}}

)sse"));
    providers::DonBotClient delivery_client{delivery_http};
    const auto delivered = delivery_client.import_permalink(
        "https://dps.report/discord", providers::donbot_default_api_base, "123", key,
        providers::DonBotDiscordDeliveryRequest{
            .mode = domain::DonBotDiscordDeliveryMode::GuildDefaults,
            .channel_id = {},
        });
    MANNY_CHECK(suite, delivered.has_value());
    MANNY_CHECK(suite, delivered && delivered->discord_delivery.outcome ==
                                        domain::DonBotDiscordDeliveryOutcome::Sent);
    MANNY_CHECK(suite, delivered && delivered->discord_delivery.sent == 2);
    MANNY_CHECK(
        suite,
        !delivery_http.requests().empty() &&
            delivery_http.requests().front().body ==
                R"({"url":"https://dps.report/discord","guildId":"123","discordDelivery":{"mode":"guild_defaults"}})");

    SequencedHttpClient completed_delivery_http;
    completed_delivery_http.push(response(
        200, {},
        R"json({"uploadId":45,"fightLogId":317,"status":"complete","duplicate":true,"discordDeliveryAccepted":true,"discordDelivery":{"requested":true,"outcome":"skipped","sent":0,"skipped":1,"failed":0,"ambiguous":0}})json"));
    providers::DonBotClient completed_delivery_client{completed_delivery_http};
    const auto completed_delivery = completed_delivery_client.import_permalink(
        "https://dps.report/discord-complete", providers::donbot_default_api_base, "123", key,
        providers::DonBotDiscordDeliveryRequest{
            .mode = domain::DonBotDiscordDeliveryMode::ChannelOverride,
            .channel_id = "223456789012345678",
        });
    MANNY_CHECK(suite, completed_delivery.has_value());
    MANNY_CHECK(suite, completed_delivery && completed_delivery->discord_delivery.outcome ==
                                                 domain::DonBotDiscordDeliveryOutcome::Skipped);
    MANNY_CHECK(
        suite,
        !completed_delivery_http.requests().empty() &&
            completed_delivery_http.requests().front().body ==
                R"({"url":"https://dps.report/discord-complete","guildId":"123","discordDelivery":{"mode":"channel_override","channelId":"223456789012345678"}})");

    SequencedHttpClient missing_ack_http;
    missing_ack_http.push(response(
        202, {},
        R"json({"uploadId":46,"fightLogId":null,"status":"pending","duplicate":false})json"));
    providers::DonBotClient missing_ack_client{missing_ack_http};
    const auto missing_ack = missing_ack_client.import_permalink(
        "https://dps.report/discord-unacknowledged", providers::donbot_default_api_base, "123", key,
        providers::DonBotDiscordDeliveryRequest{
            .mode = domain::DonBotDiscordDeliveryMode::GuildDefaults,
            .channel_id = {},
        });
    MANNY_CHECK(suite, !missing_ack.has_value());
    MANNY_CHECK(suite, missing_ack_http.requests().size() == 1);

    SequencedHttpClient mismatched_complete_http;
    mismatched_complete_http.push(response(
        200, {},
        R"json({"uploadId":47,"fightLogId":318,"status":"complete","duplicate":true,"discordDeliveryAccepted":true,"discordDelivery":{"requested":false,"outcome":"not_requested","sent":0,"skipped":0,"failed":0,"ambiguous":0}})json"));
    providers::DonBotClient mismatched_complete_client{mismatched_complete_http};
    const auto mismatched_complete = mismatched_complete_client.import_permalink(
        "https://dps.report/discord-mismatch", providers::donbot_default_api_base, "123", key,
        providers::DonBotDiscordDeliveryRequest{
            .mode = domain::DonBotDiscordDeliveryMode::GuildDefaults,
            .channel_id = {},
        });
    MANNY_CHECK(suite, !mismatched_complete.has_value());
}

} // namespace

void run_donbot_client_tests(TestSuite& suite) {
    constexpr std::string_view payload = "unique-donbot-log-payload";
    TempLog log{payload};
    const auto file = log.identity();
    verification_tests(suite);
    upload_success_tests(suite, file, payload);
    discord_delivery_upload_tests(suite, file);
    unsafe_location_tests(suite, file);
    protocol_response_tests(suite, file);
    failure_policy_tests(suite, file);
    permalink_import_tests(suite);
}

} // namespace manny_uploader::test
