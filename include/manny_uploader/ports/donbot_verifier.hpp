#pragma once

#include "manny_uploader/support/secret_value.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

namespace manny_uploader::ports {

struct DonBotChannel {
    std::string channel_id;
    std::string channel_name;

    [[nodiscard]] friend bool operator==(const DonBotChannel&,
                                         const DonBotChannel&) noexcept = default;
};

struct DonBotDiscordDeliveryPolicy {
    bool enabled{};
    bool defaults_available{};
    bool channel_override_allowed{};
    bool pve_summary{};
    bool wvw_summary{};
    bool wvw_advanced{};
    bool wvw_stream{};
    bool aggregate_enabled{};
    std::uint16_t max_aggregate_fight_logs{};
    std::vector<DonBotChannel> channels;

    [[nodiscard]] friend bool operator==(const DonBotDiscordDeliveryPolicy&,
                                         const DonBotDiscordDeliveryPolicy&) noexcept = default;
};

struct DonBotGuild {
    std::string guild_id;
    std::string guild_name;
    DonBotDiscordDeliveryPolicy discord_delivery;

    [[nodiscard]] friend bool operator==(const DonBotGuild&, const DonBotGuild&) noexcept = default;
};

struct DonBotVerification {
    std::string account_name;
    bool discord_summary_delivery_v1{};
    bool discord_aggregate_delivery_v1{};
    std::vector<DonBotGuild> guilds;
};

struct DonBotVerificationRequest {
    std::uint64_t request_id;
    std::string api_base_url;
    support::SecretValue api_key;
};

struct DonBotVerificationDispatchError {
    std::string message;
};

enum class DonBotVerificationFailureCode : std::uint8_t {
    Failed,
    Cancelled,
};

struct DonBotVerificationFailure {
    DonBotVerificationFailureCode code;
    std::string detail;
};

struct DonBotVerificationSuccess {
    DonBotVerification identity;
    support::SecretValue api_key;
};

struct DonBotVerificationResult {
    std::uint64_t request_id;
    std::expected<DonBotVerificationSuccess, DonBotVerificationFailure> verification;
};

class IDonBotVerifier {
  public:
    virtual ~IDonBotVerifier() = default;

    [[nodiscard]] virtual std::expected<void, DonBotVerificationDispatchError>
    enqueue(DonBotVerificationRequest request) = 0;
    [[nodiscard]] virtual std::optional<DonBotVerificationResult> try_take_result() = 0;
    virtual void cancel_pending() noexcept = 0;
};

} // namespace manny_uploader::ports
