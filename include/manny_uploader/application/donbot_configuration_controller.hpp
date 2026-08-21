#pragma once

#include "manny_uploader/application/configuration_service.hpp"
#include "manny_uploader/ports/donbot_verifier.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

namespace manny_uploader::application {

enum class DonBotConfigurationState : std::uint8_t {
    Unverified,
    Verifying,
    Verified,
    Error,
    ShuttingDown,
};

struct DonBotConfigurationSnapshot {
    DonBotConfigurationState state;
    std::string api_base_url;
    std::optional<std::string> account_name;
    std::vector<ports::DonBotGuild> guilds;
    std::string selected_guild_id;
    std::string diagnostic;
    std::uint64_t revision;
    bool shutting_down;
};

enum class DonBotConfigurationErrorCode : std::uint8_t {
    InvalidConfiguration,
    Busy,
    DispatchFailed,
    VerificationFailed,
    VerificationCancelled,
    StaleVerification,
    SettingsSaveFailed,
    SecretLoadFailed,
    SecretStoreFailed,
    SecretEraseFailed,
    NotVerified,
    UnknownGuild,
    ShuttingDown,
};

struct DonBotConfigurationError {
    DonBotConfigurationErrorCode code;
    std::string message;
    std::optional<ConfigurationErrorCode> configuration_error;
    std::optional<ports::DonBotVerificationFailureCode> verification_error;
};

class DonBotConfigurationController {
  public:
    [[nodiscard]] static std::expected<DonBotConfigurationController, DonBotConfigurationError>
    create(ConfigurationService& configuration, ports::IDonBotVerifier& verifier);

    DonBotConfigurationController(DonBotConfigurationController&&) noexcept = default;
    DonBotConfigurationController& operator=(DonBotConfigurationController&&) = delete;
    DonBotConfigurationController(const DonBotConfigurationController&) = delete;
    DonBotConfigurationController& operator=(const DonBotConfigurationController&) = delete;

    [[nodiscard]] DonBotConfigurationSnapshot snapshot() const;

    [[nodiscard]] std::expected<void, DonBotConfigurationError>
    begin_verification(std::string api_base_url, support::SecretValue api_key);
    [[nodiscard]] std::expected<void, DonBotConfigurationError> begin_saved_verification();
    [[nodiscard]] std::expected<bool, DonBotConfigurationError> poll();
    [[nodiscard]] std::expected<void, DonBotConfigurationError> select_guild(std::string guild_id);
    [[nodiscard]] std::expected<void, DonBotConfigurationError> disconnect();

    void shutdown() noexcept;
    [[nodiscard]] bool is_shutting_down() const noexcept;

  private:
    enum class VerificationSource : std::uint8_t {
        Candidate,
        Saved,
    };

    struct InFlightVerification {
        std::uint64_t request_id;
        std::string api_base_url;
        VerificationSource source;
    };

    DonBotConfigurationController(ConfigurationService& configuration,
                                  ports::IDonBotVerifier& verifier,
                                  DonBotConfigurationSnapshot snapshot);

    [[nodiscard]] std::expected<void, DonBotConfigurationError>
    dispatch(std::string api_base_url, support::SecretValue api_key, VerificationSource source);
    [[nodiscard]] std::expected<bool, DonBotConfigurationError>
    handle_result(ports::DonBotVerificationResult result);
    [[nodiscard]] std::expected<bool, DonBotConfigurationError>
    handle_candidate_success(ports::DonBotVerificationSuccess success,
                             const InFlightVerification& request);
    [[nodiscard]] std::expected<bool, DonBotConfigurationError>
    handle_saved_success(ports::DonBotVerificationSuccess success,
                         const InFlightVerification& request);
    [[nodiscard]] DonBotConfigurationError publish_error(DonBotConfigurationError error);
    void publish_verified(ports::DonBotVerification identity, std::string api_base_url,
                          std::string selected_guild_id);
    void clear_identity() noexcept;
    void advance_revision() noexcept;

    ConfigurationService& configuration_;
    ports::IDonBotVerifier& verifier_;
    DonBotConfigurationSnapshot snapshot_;
    std::optional<InFlightVerification> in_flight_;
    std::uint64_t next_request_id_{1};
};

} // namespace manny_uploader::application
