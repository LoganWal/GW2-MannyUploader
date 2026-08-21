#include "manny_uploader/application/donbot_configuration_controller.hpp"

#include <algorithm>
#include <limits>
#include <ranges>
#include <string_view>
#include <utility>

namespace manny_uploader::application {
namespace {

[[nodiscard]] DonBotConfigurationError make_error(DonBotConfigurationErrorCode code,
                                                  std::string message) {
    return DonBotConfigurationError{
        .code = code,
        .message = std::move(message),
        .configuration_error = std::nullopt,
        .verification_error = std::nullopt,
    };
}

[[nodiscard]] DonBotConfigurationError from_configuration_error(DonBotConfigurationErrorCode code,
                                                                std::string message,
                                                                const ConfigurationError& error) {
    auto mapped = make_error(code, std::move(message));
    mapped.configuration_error = error.code;
    return mapped;
}

[[nodiscard]] std::string normalize_base_url(std::string value) {
    constexpr std::size_t scheme_size = 8;
    while (value.size() > scheme_size && value.ends_with('/')) {
        value.pop_back();
    }
    return value;
}

[[nodiscard]] bool valid_base_url(const config::Settings& current, std::string_view api_base_url) {
    auto candidate = current;
    candidate.donbot.enabled = false;
    candidate.donbot.selected_guild_id.clear();
    candidate.donbot.api_base_url = api_base_url;
    return config::validate_settings(candidate).empty();
}

[[nodiscard]] bool contains_guild(const ports::DonBotVerification& identity,
                                  std::string_view guild_id) noexcept {
    return std::ranges::any_of(identity.guilds, [guild_id](const ports::DonBotGuild& guild) {
        return guild.guild_id == guild_id;
    });
}

} // namespace

std::expected<DonBotConfigurationController, DonBotConfigurationError>
DonBotConfigurationController::create(ConfigurationService& configuration,
                                      ports::IDonBotVerifier& verifier) {
    const auto current = configuration.snapshot();
    if (current.shutting_down) {
        return std::unexpected(make_error(DonBotConfigurationErrorCode::ShuttingDown,
                                          "DonBot configuration is shutting down"));
    }
    auto base_url = normalize_base_url(current.settings.donbot.api_base_url);
    if (!valid_base_url(current.settings, base_url)) {
        return std::unexpected(make_error(DonBotConfigurationErrorCode::InvalidConfiguration,
                                          "The configured DonBot API endpoint is invalid"));
    }

    return DonBotConfigurationController{
        configuration,
        verifier,
        DonBotConfigurationSnapshot{
            .state = DonBotConfigurationState::Unverified,
            .api_base_url = std::move(base_url),
            .account_name = std::nullopt,
            .guilds = {},
            .selected_guild_id = {},
            .diagnostic = {},
            .revision = 1,
            .shutting_down = false,
        },
    };
}

DonBotConfigurationController::DonBotConfigurationController(ConfigurationService& configuration,
                                                             ports::IDonBotVerifier& verifier,
                                                             DonBotConfigurationSnapshot snapshot)
    : configuration_{configuration}, verifier_{verifier}, snapshot_{std::move(snapshot)} {}

DonBotConfigurationSnapshot DonBotConfigurationController::snapshot() const {
    return snapshot_;
}

std::expected<void, DonBotConfigurationError>
DonBotConfigurationController::begin_verification(std::string api_base_url,
                                                  support::SecretValue api_key) {
    return dispatch(std::move(api_base_url), std::move(api_key), VerificationSource::Candidate);
}

std::expected<void, DonBotConfigurationError>
DonBotConfigurationController::begin_saved_verification() {
    if (snapshot_.shutting_down || configuration_.is_shutting_down()) {
        return std::unexpected(make_error(DonBotConfigurationErrorCode::ShuttingDown,
                                          "DonBot configuration is shutting down"));
    }
    if (in_flight_) {
        return std::unexpected(make_error(DonBotConfigurationErrorCode::Busy,
                                          "DonBot verification is already in progress"));
    }

    auto api_key = configuration_.load_secret(ports::SecretId::DonBotGw2ApiKey);
    if (!api_key) {
        return std::unexpected(publish_error(from_configuration_error(
            DonBotConfigurationErrorCode::SecretLoadFailed,
            "The saved DonBot API key could not be loaded", api_key.error())));
    }
    auto base_url = configuration_.snapshot().settings.donbot.api_base_url;
    return dispatch(std::move(base_url), std::move(*api_key), VerificationSource::Saved);
}

std::expected<bool, DonBotConfigurationError> DonBotConfigurationController::poll() {
    if (snapshot_.shutting_down) {
        return std::unexpected(make_error(DonBotConfigurationErrorCode::ShuttingDown,
                                          "DonBot configuration is shutting down"));
    }
    auto result = verifier_.try_take_result();
    if (!result) {
        return false;
    }
    return handle_result(std::move(*result));
}

std::expected<void, DonBotConfigurationError>
DonBotConfigurationController::select_guild(std::string guild_id) {
    if (snapshot_.shutting_down || configuration_.is_shutting_down()) {
        return std::unexpected(make_error(DonBotConfigurationErrorCode::ShuttingDown,
                                          "DonBot configuration is shutting down"));
    }
    if (snapshot_.state != DonBotConfigurationState::Verified || !snapshot_.account_name) {
        return std::unexpected(make_error(DonBotConfigurationErrorCode::NotVerified,
                                          "Verify DonBot before selecting a guild"));
    }
    const auto known = std::ranges::any_of(
        snapshot_.guilds, [&guild_id](const auto& guild) { return guild.guild_id == guild_id; });
    if (!known) {
        return std::unexpected(make_error(DonBotConfigurationErrorCode::UnknownGuild,
                                          "The selected guild was not authorized by DonBot"));
    }

    auto settings = configuration_.snapshot().settings;
    if (normalize_base_url(settings.donbot.api_base_url) != snapshot_.api_base_url) {
        return std::unexpected(
            publish_error(make_error(DonBotConfigurationErrorCode::StaleVerification,
                                     "DonBot settings changed after verification; verify again")));
    }
    settings.donbot.selected_guild_id = guild_id;
    if (auto saved = configuration_.save_settings(std::move(settings)); !saved) {
        return std::unexpected(publish_error(from_configuration_error(
            DonBotConfigurationErrorCode::SettingsSaveFailed,
            "The DonBot guild selection could not be saved", saved.error())));
    }

    snapshot_.selected_guild_id = std::move(guild_id);
    snapshot_.diagnostic.clear();
    advance_revision();
    return {};
}

std::expected<void, DonBotConfigurationError> DonBotConfigurationController::disconnect() {
    if (snapshot_.shutting_down || configuration_.is_shutting_down()) {
        return std::unexpected(make_error(DonBotConfigurationErrorCode::ShuttingDown,
                                          "DonBot configuration is shutting down"));
    }
    if (in_flight_) {
        return std::unexpected(make_error(DonBotConfigurationErrorCode::Busy,
                                          "DonBot verification is already in progress"));
    }

    auto settings = configuration_.snapshot().settings;
    settings.donbot.enabled = false;
    settings.donbot.selected_guild_id.clear();
    if (auto saved = configuration_.save_settings(std::move(settings)); !saved) {
        return std::unexpected(publish_error(
            from_configuration_error(DonBotConfigurationErrorCode::SettingsSaveFailed,
                                     "DonBot could not be disabled safely", saved.error())));
    }

    clear_identity();
    snapshot_.state = DonBotConfigurationState::Unverified;
    snapshot_.diagnostic.clear();
    snapshot_.api_base_url =
        normalize_base_url(configuration_.snapshot().settings.donbot.api_base_url);
    advance_revision();

    if (auto erased = configuration_.erase_secret(ports::SecretId::DonBotGw2ApiKey); !erased) {
        return std::unexpected(publish_error(from_configuration_error(
            DonBotConfigurationErrorCode::SecretEraseFailed,
            "The saved DonBot API key could not be removed", erased.error())));
    }
    return {};
}

void DonBotConfigurationController::shutdown() noexcept {
    if (snapshot_.shutting_down) {
        return;
    }
    snapshot_.shutting_down = true;
    snapshot_.state = DonBotConfigurationState::ShuttingDown;
    snapshot_.diagnostic.clear();
    clear_identity();
    in_flight_.reset();
    verifier_.cancel_pending();
    advance_revision();
}

bool DonBotConfigurationController::is_shutting_down() const noexcept {
    return snapshot_.shutting_down;
}

std::expected<void, DonBotConfigurationError>
DonBotConfigurationController::dispatch(std::string api_base_url, support::SecretValue api_key,
                                        VerificationSource source) {
    if (snapshot_.shutting_down || configuration_.is_shutting_down()) {
        return std::unexpected(make_error(DonBotConfigurationErrorCode::ShuttingDown,
                                          "DonBot configuration is shutting down"));
    }
    if (in_flight_) {
        return std::unexpected(make_error(DonBotConfigurationErrorCode::Busy,
                                          "DonBot verification is already in progress"));
    }

    api_base_url = normalize_base_url(std::move(api_base_url));
    const auto current = configuration_.snapshot();
    if (!valid_base_url(current.settings, api_base_url) || api_key.empty()) {
        return std::unexpected(
            publish_error(make_error(DonBotConfigurationErrorCode::InvalidConfiguration,
                                     "The DonBot API endpoint or API key is invalid")));
    }
    if (next_request_id_ == std::numeric_limits<std::uint64_t>::max()) {
        return std::unexpected(
            publish_error(make_error(DonBotConfigurationErrorCode::InvalidConfiguration,
                                     "No more DonBot verification requests can be created")));
    }

    const auto request_id = next_request_id_++;
    auto pending = InFlightVerification{
        .request_id = request_id,
        .api_base_url = api_base_url,
        .source = source,
    };
    auto queued = verifier_.enqueue(ports::DonBotVerificationRequest{
        .request_id = request_id,
        .api_base_url = api_base_url,
        .api_key = std::move(api_key),
    });
    if (!queued) {
        return std::unexpected(
            publish_error(make_error(DonBotConfigurationErrorCode::DispatchFailed,
                                     "DonBot verification could not be started")));
    }

    in_flight_.emplace(std::move(pending));
    clear_identity();
    snapshot_.state = DonBotConfigurationState::Verifying;
    snapshot_.api_base_url = std::move(api_base_url);
    snapshot_.diagnostic.clear();
    advance_revision();
    return {};
}

std::expected<bool, DonBotConfigurationError>
DonBotConfigurationController::handle_result(ports::DonBotVerificationResult result) {
    if (!in_flight_ || result.request_id != in_flight_->request_id) {
        return std::unexpected(make_error(DonBotConfigurationErrorCode::StaleVerification,
                                          "A stale DonBot verification result was ignored"));
    }

    const auto request = std::move(*in_flight_);
    in_flight_.reset();
    if (!result.verification) {
        auto failure = std::move(result.verification.error());
        auto error = make_error(failure.code == ports::DonBotVerificationFailureCode::Cancelled
                                    ? DonBotConfigurationErrorCode::VerificationCancelled
                                    : DonBotConfigurationErrorCode::VerificationFailed,
                                failure.detail.empty() ? "DonBot verification failed"
                                                       : std::move(failure.detail));
        error.verification_error = failure.code;
        return std::unexpected(publish_error(std::move(error)));
    }

    if (request.source == VerificationSource::Candidate) {
        return handle_candidate_success(std::move(*result.verification), request);
    }
    return handle_saved_success(std::move(*result.verification), request);
}

std::expected<bool, DonBotConfigurationError>
DonBotConfigurationController::handle_candidate_success(ports::DonBotVerificationSuccess success,
                                                        const InFlightVerification& request) {
    auto settings = configuration_.snapshot().settings;
    settings.donbot.enabled = false;
    settings.donbot.selected_guild_id.clear();
    settings.donbot.api_base_url = request.api_base_url;
    if (auto saved = configuration_.save_settings(std::move(settings)); !saved) {
        return std::unexpected(publish_error(from_configuration_error(
            DonBotConfigurationErrorCode::SettingsSaveFailed,
            "Verified DonBot settings could not be saved", saved.error())));
    }
    if (auto stored =
            configuration_.store_secret(ports::SecretId::DonBotGw2ApiKey, success.api_key);
        !stored) {
        return std::unexpected(publish_error(from_configuration_error(
            DonBotConfigurationErrorCode::SecretStoreFailed,
            "The verified DonBot API key could not be saved", stored.error())));
    }

    publish_verified(std::move(success.identity), request.api_base_url, {});
    return true;
}

std::expected<bool, DonBotConfigurationError>
DonBotConfigurationController::handle_saved_success(ports::DonBotVerificationSuccess success,
                                                    const InFlightVerification& request) {
    auto settings = configuration_.snapshot().settings;
    if (normalize_base_url(settings.donbot.api_base_url) != request.api_base_url) {
        return std::unexpected(
            publish_error(make_error(DonBotConfigurationErrorCode::StaleVerification,
                                     "DonBot settings changed after verification; verify again")));
    }

    auto selected_guild_id = settings.donbot.selected_guild_id;
    if (!selected_guild_id.empty() && !contains_guild(success.identity, selected_guild_id)) {
        settings.donbot.enabled = false;
        settings.donbot.selected_guild_id.clear();
        if (auto saved = configuration_.save_settings(std::move(settings)); !saved) {
            return std::unexpected(publish_error(from_configuration_error(
                DonBotConfigurationErrorCode::SettingsSaveFailed,
                "An unauthorized DonBot guild selection could not be disabled", saved.error())));
        }
        selected_guild_id.clear();
    }

    publish_verified(std::move(success.identity), request.api_base_url,
                     std::move(selected_guild_id));
    return true;
}

DonBotConfigurationError
DonBotConfigurationController::publish_error(DonBotConfigurationError error) {
    clear_identity();
    snapshot_.state = DonBotConfigurationState::Error;
    snapshot_.diagnostic = error.message;
    advance_revision();
    return error;
}

void DonBotConfigurationController::publish_verified(ports::DonBotVerification identity,
                                                     std::string api_base_url,
                                                     std::string selected_guild_id) {
    snapshot_.state = DonBotConfigurationState::Verified;
    snapshot_.api_base_url = std::move(api_base_url);
    snapshot_.account_name = std::move(identity.account_name);
    snapshot_.guilds = std::move(identity.guilds);
    snapshot_.selected_guild_id = std::move(selected_guild_id);
    snapshot_.diagnostic.clear();
    advance_revision();
}

void DonBotConfigurationController::clear_identity() noexcept {
    snapshot_.account_name.reset();
    snapshot_.guilds.clear();
    snapshot_.selected_guild_id.clear();
}

void DonBotConfigurationController::advance_revision() noexcept {
    if (snapshot_.revision < std::numeric_limits<std::uint64_t>::max()) {
        ++snapshot_.revision;
    }
}

} // namespace manny_uploader::application
