#include "manny_uploader/application/donbot_configuration_controller.hpp"

#include "support/test_suite.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace manny_uploader::test {
namespace {

[[nodiscard]] config::Settings settings() {
    return config::make_default_settings("C:/Users/Streamer/Documents/Guild Wars 2/logs");
}

[[nodiscard]] ports::SettingsStoreError settings_error() {
    return ports::SettingsStoreError{
        .code = ports::SettingsStoreErrorCode::FileWriteFailed,
        .message = "settings write failed",
        .path = "C:/addon/settings.json",
        .validation_errors = {},
    };
}

[[nodiscard]] ports::SecretStoreError secret_error(ports::SecretStoreErrorCode code) {
    return ports::SecretStoreError{
        .code = code,
        .id = ports::SecretId::DonBotGw2ApiKey,
        .message = "protected credential operation failed",
        .system_error = std::nullopt,
    };
}

[[nodiscard]] std::string secret_text(const support::SecretValue& value) {
    const auto bytes = value.bytes();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

class RecordingSettingsStore final : public ports::ISettingsStore {
  public:
    RecordingSettingsStore(config::Settings initial, std::vector<std::string>& events)
        : initial_{std::move(initial)}, events_{events} {}

    [[nodiscard]] std::expected<ports::SettingsLoadResult, ports::SettingsStoreError>
    load() const override {
        events_.push_back("settings.load");
        return ports::SettingsLoadResult{
            .settings = initial_,
            .source = ports::SettingsLoadSource::Primary,
            .recovery_diagnostic = std::nullopt,
        };
    }

    [[nodiscard]] std::expected<void, ports::SettingsStoreError>
    save(const config::Settings& value) const override {
        events_.push_back("settings.save");
        ++save_calls;
        if (fail_save) {
            return std::unexpected(settings_error());
        }
        saved.push_back(value);
        return {};
    }

    config::Settings initial_;
    std::vector<std::string>& events_;
    mutable std::vector<config::Settings> saved;
    mutable std::size_t save_calls{};
    bool fail_save{};
};

class RecordingSecretStore final : public ports::ISecretStore {
  public:
    RecordingSecretStore(std::string initial, std::vector<std::string>& events)
        : value_{std::move(initial)}, events_{events} {}

    [[nodiscard]] std::expected<support::SecretValue, ports::SecretStoreError>
    load(ports::SecretId id) const override {
        events_.push_back("secret.load");
        load_ids.push_back(id);
        if (fail_load) {
            return std::unexpected(secret_error(ports::SecretStoreErrorCode::NotFound));
        }
        return support::SecretValue::from_text(value_);
    }

    [[nodiscard]] std::expected<void, ports::SecretStoreError>
    store(ports::SecretId id, const support::SecretValue& value) override {
        events_.push_back("secret.store");
        store_ids.push_back(id);
        if (fail_store) {
            return std::unexpected(secret_error(ports::SecretStoreErrorCode::ProtectionFailed));
        }
        value_ = secret_text(value);
        return {};
    }

    [[nodiscard]] std::expected<void, ports::SecretStoreError> erase(ports::SecretId id) override {
        events_.push_back("secret.erase");
        erase_ids.push_back(id);
        if (fail_erase) {
            return std::unexpected(secret_error(ports::SecretStoreErrorCode::DeleteFailed));
        }
        value_.clear();
        return {};
    }

    std::string value_;
    std::vector<std::string>& events_;
    bool fail_load{};
    bool fail_store{};
    bool fail_erase{};
    mutable std::vector<ports::SecretId> load_ids;
    std::vector<ports::SecretId> store_ids;
    std::vector<ports::SecretId> erase_ids;
};

class FakeDonBotVerifier final : public ports::IDonBotVerifier {
  public:
    explicit FakeDonBotVerifier(std::vector<std::string>& events) : events_{events} {}

    [[nodiscard]] std::expected<void, ports::DonBotVerificationDispatchError>
    enqueue(ports::DonBotVerificationRequest request) override {
        events_.push_back("verifier.enqueue");
        if (stopping_ || reject_next) {
            reject_next = false;
            return std::unexpected(
                ports::DonBotVerificationDispatchError{.message = "queue rejected"});
        }
        requests.push_back(std::move(request));
        return {};
    }

    [[nodiscard]] std::optional<ports::DonBotVerificationResult> try_take_result() override {
        if (results.empty()) {
            return std::nullopt;
        }
        auto result = std::move(results.front());
        results.pop_front();
        return result;
    }

    void cancel_pending() noexcept override {
        ++cancel_calls;
        stopping_ = true;
        requests.clear();
        results.clear();
    }

    void
    succeed_next(std::string account = "Player.1234",
                 std::vector<ports::DonBotGuild> guilds =
                     {
                         {.guild_id = "123", .guild_name = "Guild One", .discord_delivery = {}},
                         {.guild_id = "456", .guild_name = "Guild Two", .discord_delivery = {}},
                     },
                 bool discord_summary_delivery_v1 = false,
                 bool discord_aggregate_delivery_v1 = false) {
        auto request = std::move(requests.front());
        requests.pop_front();
        results.push_back(ports::DonBotVerificationResult{
            .request_id = request.request_id,
            .verification =
                ports::DonBotVerificationSuccess{
                    .identity =
                        ports::DonBotVerification{
                            .account_name = std::move(account),
                            .discord_summary_delivery_v1 = discord_summary_delivery_v1,
                            .discord_aggregate_delivery_v1 = discord_aggregate_delivery_v1,
                            .guilds = std::move(guilds),
                        },
                    .api_key = std::move(request.api_key),
                },
        });
    }

    void fail_next(ports::DonBotVerificationFailureCode code,
                   std::string detail = "Verification rejected") {
        auto request = std::move(requests.front());
        requests.pop_front();
        results.push_back(ports::DonBotVerificationResult{
            .request_id = request.request_id,
            .verification = std::unexpected(ports::DonBotVerificationFailure{
                .code = code,
                .detail = std::move(detail),
            }),
        });
    }

    std::vector<std::string>& events_;
    std::deque<ports::DonBotVerificationRequest> requests;
    std::deque<ports::DonBotVerificationResult> results;
    bool reject_next{};
    bool stopping_{};
    std::size_t cancel_calls{};
};

struct Fixture {
    explicit Fixture(config::Settings initial = settings(), std::string secret = "SAVED-KEY")
        : verifier{events} {
        auto settings_store = std::make_unique<RecordingSettingsStore>(std::move(initial), events);
        settings_observer = settings_store.get();
        auto secret_store = std::make_unique<RecordingSecretStore>(std::move(secret), events);
        secret_observer = secret_store.get();
        auto created = application::ConfigurationService::create(std::move(settings_store),
                                                                 std::move(secret_store));
        configuration.emplace(std::move(*created));
        auto controller_created =
            application::DonBotConfigurationController::create(*configuration, verifier);
        controller.emplace(std::move(*controller_created));
        events.clear();
    }

    std::vector<std::string> events;
    RecordingSettingsStore* settings_observer{};
    RecordingSecretStore* secret_observer{};
    std::optional<application::ConfigurationService> configuration;
    FakeDonBotVerifier verifier;
    std::optional<application::DonBotConfigurationController> controller;
};

[[nodiscard]] bool snapshot_contains(const application::DonBotConfigurationSnapshot& snapshot,
                                     std::string_view marker) {
    const auto contains = [marker](std::string_view value) {
        return value.find(marker) != std::string_view::npos;
    };
    return contains(snapshot.api_base_url) || contains(snapshot.account_name.value_or("")) ||
           contains(snapshot.selected_guild_id) || contains(snapshot.diagnostic) ||
           std::ranges::any_of(snapshot.guilds, [&contains](const auto& guild) {
               return contains(guild.guild_id) || contains(guild.guild_name) ||
                      std::ranges::any_of(
                          guild.discord_delivery.channels, [&contains](const auto& channel) {
                              return contains(channel.channel_id) || contains(channel.channel_name);
                          });
           });
}

[[nodiscard]] ports::DonBotGuild delivery_guild() {
    return ports::DonBotGuild{
        .guild_id = "123",
        .guild_name = "Guild One",
        .discord_delivery =
            ports::DonBotDiscordDeliveryPolicy{
                .enabled = true,
                .defaults_available = true,
                .channel_override_allowed = true,
                .pve_summary = true,
                .wvw_summary = true,
                .wvw_advanced = true,
                .wvw_stream = false,
                .aggregate_enabled = true,
                .max_aggregate_fight_logs = 50,
                .channels =
                    {
                        {.channel_id = "223", .channel_name = "logs"},
                        {.channel_id = "224", .channel_name = "command-logs"},
                    },
            },
    };
}

void candidate_success_and_selection_tests(TestSuite& suite) {
    constexpr std::string_view marker = "PRIVATE-CANDIDATE-KEY";
    auto initial_settings = settings();
    initial_settings.donbot.enabled = true;
    initial_settings.donbot.selected_guild_id = "123";
    initial_settings.donbot.discord_delivery_enabled = true;
    initial_settings.donbot.discord_channel_override_explicit = true;
    initial_settings.donbot.selected_discord_channel_id = "223";
    Fixture fixture{initial_settings};
    auto& controller = *fixture.controller;
    const auto initial = controller.snapshot();
    MANNY_CHECK(suite, initial.state == application::DonBotConfigurationState::Unverified);
    MANNY_CHECK(suite, initial.revision == 1);
    MANNY_CHECK(suite, !initial.account_name.has_value());

    auto started = controller.begin_verification("https://new-donbot.example/root///",
                                                 support::SecretValue::from_text(marker));
    MANNY_CHECK(suite, started.has_value());
    MANNY_CHECK(suite, fixture.verifier.requests.size() == 1);
    MANNY_CHECK(suite, fixture.verifier.requests.front().api_base_url ==
                           "https://new-donbot.example/root");
    MANNY_CHECK(suite, secret_text(fixture.verifier.requests.front().api_key) == marker);
    MANNY_CHECK(suite, fixture.secret_observer->store_ids.empty());
    MANNY_CHECK(suite, fixture.configuration->snapshot().settings.donbot.api_base_url ==
                           config::default_donbot_api_base);
    auto verifying = controller.snapshot();
    MANNY_CHECK(suite, verifying.state == application::DonBotConfigurationState::Verifying);
    MANNY_CHECK(suite, verifying.revision == 2);
    MANNY_CHECK(suite, !snapshot_contains(verifying, marker));

    fixture.verifier.succeed_next();
    fixture.events.clear();
    auto polled = controller.poll();
    MANNY_CHECK(suite, polled.has_value() && *polled);
    MANNY_CHECK(suite,
                fixture.events == std::vector<std::string>({"settings.save", "secret.store"}));
    MANNY_CHECK(suite, fixture.secret_observer->value_ == marker);
    const auto persisted = fixture.configuration->snapshot().settings.donbot;
    MANNY_CHECK(suite, !persisted.enabled);
    MANNY_CHECK(suite, persisted.selected_guild_id.empty());
    MANNY_CHECK(suite, !persisted.discord_delivery_enabled);
    MANNY_CHECK(suite, !persisted.discord_channel_override_explicit);
    MANNY_CHECK(suite, persisted.selected_discord_channel_id.empty());
    MANNY_CHECK(suite, persisted.api_base_url == "https://new-donbot.example/root");

    auto verified = controller.snapshot();
    MANNY_CHECK(suite, verified.state == application::DonBotConfigurationState::Verified);
    MANNY_CHECK(suite, verified.account_name == "Player.1234");
    MANNY_CHECK(suite, verified.guilds.size() == 2);
    MANNY_CHECK(suite, verified.selected_guild_id.empty());
    MANNY_CHECK(suite, !snapshot_contains(verified, marker));

    const auto unknown = controller.select_guild("999");
    MANNY_CHECK(suite, !unknown.has_value());
    MANNY_CHECK(suite,
                unknown.error().code == application::DonBotConfigurationErrorCode::UnknownGuild);
    fixture.events.clear();
    MANNY_CHECK(suite, controller.select_guild("456").has_value());
    MANNY_CHECK(suite, fixture.events == std::vector<std::string>({"settings.save"}));
    MANNY_CHECK(suite,
                fixture.configuration->snapshot().settings.donbot.selected_guild_id == "456");
    MANNY_CHECK(suite, !fixture.configuration->snapshot().settings.donbot.enabled);
    MANNY_CHECK(suite, controller.snapshot().selected_guild_id == "456");
}

void verification_failure_and_dispatch_tests(TestSuite& suite) {
    Fixture fixture;
    auto& controller = *fixture.controller;
    MANNY_CHECK(suite, controller
                           .begin_verification("https://donbot.example",
                                               support::SecretValue::from_text("KEY"))
                           .has_value());
    const auto busy = controller.begin_saved_verification();
    MANNY_CHECK(suite, !busy.has_value());
    MANNY_CHECK(suite, busy.error().code == application::DonBotConfigurationErrorCode::Busy);
    fixture.verifier.fail_next(ports::DonBotVerificationFailureCode::Failed,
                               "DonBot rejected the key");
    fixture.events.clear();
    const auto failed = controller.poll();
    MANNY_CHECK(suite, !failed.has_value());
    MANNY_CHECK(suite, failed.error().code ==
                           application::DonBotConfigurationErrorCode::VerificationFailed);
    MANNY_CHECK(suite, fixture.events.empty());
    MANNY_CHECK(suite, fixture.secret_observer->value_ == "SAVED-KEY");
    MANNY_CHECK(suite, controller.snapshot().state == application::DonBotConfigurationState::Error);

    fixture.verifier.reject_next = true;
    const auto rejected = controller.begin_verification(
        "https://donbot.example", support::SecretValue::from_text("OTHER-KEY"));
    MANNY_CHECK(suite, !rejected.has_value());
    MANNY_CHECK(suite,
                rejected.error().code == application::DonBotConfigurationErrorCode::DispatchFailed);
    const auto invalid = controller.begin_verification("http://insecure.example",
                                                       support::SecretValue::from_text("KEY"));
    MANNY_CHECK(suite, !invalid.has_value());
    MANNY_CHECK(suite, invalid.error().code ==
                           application::DonBotConfigurationErrorCode::InvalidConfiguration);

    MANNY_CHECK(suite, controller
                           .begin_verification("https://donbot.example",
                                               support::SecretValue::from_text("KEY"))
                           .has_value());
    fixture.verifier.fail_next(ports::DonBotVerificationFailureCode::Cancelled,
                               "Verification cancelled");
    const auto cancelled = controller.poll();
    MANNY_CHECK(suite, !cancelled.has_value());
    MANNY_CHECK(suite, cancelled.error().code ==
                           application::DonBotConfigurationErrorCode::VerificationCancelled);
    MANNY_CHECK(suite, cancelled.error().verification_error ==
                           ports::DonBotVerificationFailureCode::Cancelled);
}

void discord_delivery_selection_tests(TestSuite& suite) {
    Fixture fixture;
    auto& controller = *fixture.controller;
    auto second_guild = delivery_guild();
    second_guild.guild_id = "456";
    second_guild.guild_name = "Guild Two";
    second_guild.discord_delivery.channels = {{.channel_id = "323", .channel_name = "wvw-logs"}};
    MANNY_CHECK(suite, controller
                           .begin_verification("https://donbot.example",
                                               support::SecretValue::from_text("KEY"))
                           .has_value());
    fixture.verifier.succeed_next("Player.1234", {delivery_guild(), second_guild}, true, true);
    MANNY_CHECK(suite, controller.poll().has_value());
    MANNY_CHECK(suite, controller.snapshot().discord_summary_delivery_v1);
    MANNY_CHECK(suite, controller.snapshot().discord_aggregate_delivery_v1);
    MANNY_CHECK(suite, controller.select_guild("123").has_value());

    const auto disabled = controller.set_discord_delivery_enabled(true);
    MANNY_CHECK(suite, !disabled.has_value());
    MANNY_CHECK(suite, disabled.error().code ==
                           application::DonBotConfigurationErrorCode::DeliveryUnavailable);

    auto settings = fixture.configuration->snapshot().settings;
    settings.donbot.enabled = true;
    MANNY_CHECK(suite, fixture.configuration->save_settings(std::move(settings)).has_value());
    MANNY_CHECK(suite, controller.set_discord_delivery_enabled(true).has_value());
    MANNY_CHECK(suite, fixture.configuration->snapshot().settings.donbot.discord_delivery_enabled);
    const auto default_delivery = application::authorized_donbot_delivery(
        fixture.configuration->snapshot().settings, controller.snapshot());
    MANNY_CHECK(suite, default_delivery.mode == domain::DonBotDiscordDeliveryMode::GuildDefaults);
    MANNY_CHECK(suite, default_delivery.channel_id.empty());
    MANNY_CHECK(suite, controller.select_discord_channel("223").has_value());
    MANNY_CHECK(
        suite, fixture.configuration->snapshot().settings.donbot.discord_channel_override_explicit);
    MANNY_CHECK(suite,
                fixture.configuration->snapshot().settings.donbot.selected_discord_channel_id ==
                    "223");
    const auto override_delivery = application::authorized_donbot_delivery(
        fixture.configuration->snapshot().settings, controller.snapshot());
    MANNY_CHECK(suite,
                override_delivery.mode == domain::DonBotDiscordDeliveryMode::ChannelOverride);
    MANNY_CHECK(suite, override_delivery.channel_id == "223");
    MANNY_CHECK(suite, controller.set_discord_delivery_enabled(false).has_value());
    MANNY_CHECK(suite, !fixture.configuration->snapshot().settings.donbot.discord_delivery_enabled);
    MANNY_CHECK(suite, application::authorized_donbot_delivery(
                           fixture.configuration->snapshot().settings, controller.snapshot())
                               .mode == domain::DonBotDiscordDeliveryMode::None);
    const auto aggregate_route = application::authorized_donbot_route(
        fixture.configuration->snapshot().settings, controller.snapshot());
    MANNY_CHECK(suite, aggregate_route.mode == domain::DonBotDiscordDeliveryMode::ChannelOverride);
    MANNY_CHECK(suite, aggregate_route.channel_id == "223");
    MANNY_CHECK(suite, controller.set_discord_delivery_enabled(true).has_value());

    const auto unknown = controller.select_discord_channel("999");
    MANNY_CHECK(suite, !unknown.has_value());
    MANNY_CHECK(suite,
                unknown.error().code == application::DonBotConfigurationErrorCode::UnknownChannel);
    MANNY_CHECK(suite, controller.select_discord_channel({}).has_value());
    MANNY_CHECK(
        suite,
        !fixture.configuration->snapshot().settings.donbot.discord_channel_override_explicit);
    MANNY_CHECK(
        suite,
        fixture.configuration->snapshot().settings.donbot.selected_discord_channel_id.empty());
    const auto restored_defaults = application::authorized_donbot_delivery(
        fixture.configuration->snapshot().settings, controller.snapshot());
    MANNY_CHECK(suite, restored_defaults.mode == domain::DonBotDiscordDeliveryMode::GuildDefaults);
    MANNY_CHECK(suite, restored_defaults.channel_id.empty());

    MANNY_CHECK(suite, controller.select_discord_channel("223").has_value());
    MANNY_CHECK(suite, controller.select_guild("123").has_value());
    MANNY_CHECK(suite, fixture.configuration->snapshot().settings.donbot.discord_delivery_enabled);
    MANNY_CHECK(suite,
                fixture.configuration->snapshot().settings.donbot.selected_discord_channel_id ==
                    "223");

    MANNY_CHECK(suite, controller.select_guild("456").has_value());
    MANNY_CHECK(suite, !fixture.configuration->snapshot().settings.donbot.discord_delivery_enabled);
    MANNY_CHECK(
        suite,
        !fixture.configuration->snapshot().settings.donbot.discord_channel_override_explicit);
    MANNY_CHECK(
        suite,
        fixture.configuration->snapshot().settings.donbot.selected_discord_channel_id.empty());
}

void persistence_failure_tests(TestSuite& suite) {
    {
        Fixture fixture;
        fixture.settings_observer->fail_save = true;
        MANNY_CHECK(suite, fixture.controller
                               ->begin_verification("https://donbot.example",
                                                    support::SecretValue::from_text("NEW-KEY"))
                               .has_value());
        fixture.verifier.succeed_next();
        fixture.events.clear();
        const auto failed = fixture.controller->poll();
        MANNY_CHECK(suite, !failed.has_value());
        MANNY_CHECK(suite, failed.error().code ==
                               application::DonBotConfigurationErrorCode::SettingsSaveFailed);
        MANNY_CHECK(suite, fixture.events == std::vector<std::string>({"settings.save"}));
        MANNY_CHECK(suite, fixture.secret_observer->store_ids.empty());
        MANNY_CHECK(suite, fixture.secret_observer->value_ == "SAVED-KEY");
    }

    {
        auto initial = settings();
        initial.donbot.enabled = true;
        initial.donbot.selected_guild_id = "999";
        initial.donbot.discord_delivery_enabled = true;
        initial.donbot.discord_channel_override_explicit = true;
        initial.donbot.selected_discord_channel_id = "223";
        Fixture fixture{initial};
        fixture.secret_observer->fail_store = true;
        MANNY_CHECK(suite, fixture.controller
                               ->begin_verification("https://new.example/",
                                                    support::SecretValue::from_text("NEW-KEY"))
                               .has_value());
        fixture.verifier.succeed_next();
        fixture.events.clear();
        const auto failed = fixture.controller->poll();
        MANNY_CHECK(suite, !failed.has_value());
        MANNY_CHECK(suite, failed.error().code ==
                               application::DonBotConfigurationErrorCode::SecretStoreFailed);
        MANNY_CHECK(suite,
                    fixture.events == std::vector<std::string>({"settings.save", "secret.store"}));
        const auto safe = fixture.configuration->snapshot().settings.donbot;
        MANNY_CHECK(suite, !safe.enabled);
        MANNY_CHECK(suite, safe.selected_guild_id.empty());
        MANNY_CHECK(suite, !safe.discord_delivery_enabled);
        MANNY_CHECK(suite, safe.selected_discord_channel_id.empty());
        MANNY_CHECK(suite, safe.api_base_url == "https://new.example");
        MANNY_CHECK(suite, fixture.secret_observer->value_ == "SAVED-KEY");
    }
}

void saved_verification_tests(TestSuite& suite) {
    {
        auto initial = settings();
        initial.donbot.enabled = true;
        initial.donbot.selected_guild_id = "123";
        initial.donbot.api_base_url = "https://saved.example/root/";
        Fixture fixture{initial, "PERSISTED-KEY"};
        MANNY_CHECK(suite, fixture.controller->begin_saved_verification().has_value());
        MANNY_CHECK(suite, fixture.events ==
                               std::vector<std::string>({"secret.load", "verifier.enqueue"}));
        MANNY_CHECK(suite,
                    fixture.verifier.requests.front().api_base_url == "https://saved.example/root");
        MANNY_CHECK(suite,
                    secret_text(fixture.verifier.requests.front().api_key) == "PERSISTED-KEY");
        fixture.verifier.succeed_next();
        fixture.events.clear();
        MANNY_CHECK(suite, fixture.controller->poll().has_value());
        MANNY_CHECK(suite, fixture.events.empty());
        MANNY_CHECK(suite, fixture.controller->snapshot().selected_guild_id == "123");
        MANNY_CHECK(suite, fixture.configuration->snapshot().settings.donbot.enabled);
        MANNY_CHECK(suite, fixture.secret_observer->store_ids.empty());
    }

    {
        auto initial = settings();
        initial.donbot.enabled = true;
        initial.donbot.selected_guild_id = "999";
        initial.donbot.discord_delivery_enabled = true;
        initial.donbot.discord_channel_override_explicit = true;
        initial.donbot.selected_discord_channel_id = "223";
        Fixture fixture{initial};
        MANNY_CHECK(suite, fixture.controller->begin_saved_verification().has_value());
        fixture.verifier.succeed_next();
        fixture.events.clear();
        MANNY_CHECK(suite, fixture.controller->poll().has_value());
        MANNY_CHECK(suite, fixture.events == std::vector<std::string>({"settings.save"}));
        const auto safe = fixture.configuration->snapshot().settings.donbot;
        MANNY_CHECK(suite, !safe.enabled);
        MANNY_CHECK(suite, safe.selected_guild_id.empty());
        MANNY_CHECK(suite, !safe.discord_delivery_enabled);
        MANNY_CHECK(suite, safe.selected_discord_channel_id.empty());
        MANNY_CHECK(suite, fixture.controller->snapshot().selected_guild_id.empty());
    }

    {
        auto initial = settings();
        initial.donbot.enabled = true;
        initial.donbot.selected_guild_id = "123";
        initial.donbot.discord_delivery_enabled = true;
        initial.donbot.selected_discord_channel_id = "223";
        Fixture fixture{initial};
        MANNY_CHECK(suite, fixture.controller->begin_saved_verification().has_value());
        fixture.verifier.succeed_next("Player.1234", {delivery_guild()}, true);
        fixture.events.clear();
        MANNY_CHECK(suite, fixture.controller->poll().has_value());
        MANNY_CHECK(suite, fixture.events == std::vector<std::string>({"settings.save"}));
        const auto sanitized = fixture.configuration->snapshot().settings.donbot;
        MANNY_CHECK(suite, sanitized.enabled);
        MANNY_CHECK(suite, sanitized.discord_delivery_enabled);
        MANNY_CHECK(suite, !sanitized.discord_channel_override_explicit);
        MANNY_CHECK(suite, sanitized.selected_discord_channel_id.empty());
        const auto authorized = application::authorized_donbot_delivery(
            fixture.configuration->snapshot().settings, fixture.controller->snapshot());
        MANNY_CHECK(suite, authorized.mode == domain::DonBotDiscordDeliveryMode::GuildDefaults);
        MANNY_CHECK(suite, authorized.channel_id.empty());
    }

    {
        auto initial = settings();
        initial.donbot.enabled = true;
        initial.donbot.selected_guild_id = "123";
        initial.donbot.discord_delivery_enabled = true;
        initial.donbot.discord_channel_override_explicit = true;
        initial.donbot.selected_discord_channel_id = "223";
        Fixture fixture{initial};
        MANNY_CHECK(
            suite,
            application::authorized_donbot_delivery(initial, fixture.controller->snapshot()).mode ==
                domain::DonBotDiscordDeliveryMode::None);
        MANNY_CHECK(suite, fixture.controller->begin_saved_verification().has_value());
        MANNY_CHECK(
            suite,
            application::authorized_donbot_delivery(initial, fixture.controller->snapshot()).mode ==
                domain::DonBotDiscordDeliveryMode::None);
        fixture.verifier.succeed_next("Player.1234", {delivery_guild()}, true);
        fixture.events.clear();
        MANNY_CHECK(suite, fixture.controller->poll().has_value());
        MANNY_CHECK(suite, fixture.events.empty());
        const auto preserved = fixture.configuration->snapshot().settings.donbot;
        MANNY_CHECK(suite, preserved.enabled);
        MANNY_CHECK(suite, preserved.discord_delivery_enabled);
        MANNY_CHECK(suite, preserved.discord_channel_override_explicit);
        MANNY_CHECK(suite, preserved.selected_discord_channel_id == "223");
        const auto authorized = application::authorized_donbot_delivery(
            fixture.configuration->snapshot().settings, fixture.controller->snapshot());
        MANNY_CHECK(suite, authorized.mode == domain::DonBotDiscordDeliveryMode::ChannelOverride);
        MANNY_CHECK(suite, authorized.channel_id == "223");
        auto stale_settings = fixture.configuration->snapshot().settings;
        stale_settings.donbot.api_base_url = "https://other.example";
        MANNY_CHECK(suite, application::authorized_donbot_delivery(stale_settings,
                                                                   fixture.controller->snapshot())
                                   .mode == domain::DonBotDiscordDeliveryMode::None);
        stale_settings = fixture.configuration->snapshot().settings;
        stale_settings.donbot.selected_discord_channel_id = "999";
        MANNY_CHECK(suite, application::authorized_donbot_delivery(stale_settings,
                                                                   fixture.controller->snapshot())
                                   .mode == domain::DonBotDiscordDeliveryMode::None);
    }

    {
        auto initial = settings();
        initial.donbot.enabled = true;
        initial.donbot.selected_guild_id = "123";
        initial.donbot.discord_delivery_enabled = true;
        initial.donbot.discord_channel_override_explicit = true;
        initial.donbot.selected_discord_channel_id = "223";
        Fixture fixture{initial};
        MANNY_CHECK(suite, fixture.controller->begin_saved_verification().has_value());
        fixture.verifier.succeed_next();
        fixture.events.clear();
        MANNY_CHECK(suite, fixture.controller->poll().has_value());
        MANNY_CHECK(suite, fixture.events == std::vector<std::string>({"settings.save"}));
        const auto revoked = fixture.configuration->snapshot().settings.donbot;
        MANNY_CHECK(suite, revoked.enabled);
        MANNY_CHECK(suite, !revoked.discord_delivery_enabled);
        MANNY_CHECK(suite, !revoked.discord_channel_override_explicit);
        MANNY_CHECK(suite, revoked.selected_discord_channel_id.empty());
    }

    {
        Fixture fixture;
        fixture.secret_observer->fail_load = true;
        const auto failed = fixture.controller->begin_saved_verification();
        MANNY_CHECK(suite, !failed.has_value());
        MANNY_CHECK(suite, failed.error().code ==
                               application::DonBotConfigurationErrorCode::SecretLoadFailed);
        MANNY_CHECK(suite, fixture.verifier.requests.empty());
    }
}

void stale_disconnect_and_shutdown_tests(TestSuite& suite) {
    {
        Fixture fixture;
        MANNY_CHECK(suite, fixture.controller
                               ->begin_verification("https://donbot.example",
                                                    support::SecretValue::from_text("KEY"))
                               .has_value());
        fixture.verifier.results.push_back(ports::DonBotVerificationResult{
            .request_id = 999,
            .verification = std::unexpected(ports::DonBotVerificationFailure{
                .code = ports::DonBotVerificationFailureCode::Failed,
                .detail = "stale",
            }),
        });
        const auto stale = fixture.controller->poll();
        MANNY_CHECK(suite, !stale.has_value());
        MANNY_CHECK(suite, stale.error().code ==
                               application::DonBotConfigurationErrorCode::StaleVerification);
        MANNY_CHECK(suite, fixture.controller->snapshot().state ==
                               application::DonBotConfigurationState::Verifying);
        fixture.verifier.succeed_next();
        MANNY_CHECK(suite, fixture.controller->poll().has_value());
        MANNY_CHECK(suite, fixture.controller->snapshot().state ==
                               application::DonBotConfigurationState::Verified);
    }

    {
        Fixture fixture;
        MANNY_CHECK(suite, fixture.controller->begin_saved_verification().has_value());
        auto changed = fixture.configuration->snapshot().settings;
        changed.donbot.api_base_url = "https://changed.example";
        MANNY_CHECK(suite, fixture.configuration->save_settings(std::move(changed)).has_value());
        fixture.verifier.succeed_next();
        fixture.events.clear();
        const auto stale = fixture.controller->poll();
        MANNY_CHECK(suite, !stale.has_value());
        MANNY_CHECK(suite, stale.error().code ==
                               application::DonBotConfigurationErrorCode::StaleVerification);
        MANNY_CHECK(suite, fixture.events.empty());
    }

    {
        Fixture fixture;
        MANNY_CHECK(suite, fixture.controller
                               ->begin_verification("https://donbot.example",
                                                    support::SecretValue::from_text("NEW-KEY"))
                               .has_value());
        fixture.verifier.succeed_next();
        MANNY_CHECK(suite, fixture.controller->poll().has_value());
        MANNY_CHECK(suite, fixture.controller->select_guild("123").has_value());
        auto enabled = fixture.configuration->snapshot().settings;
        enabled.donbot.enabled = true;
        MANNY_CHECK(suite, fixture.configuration->save_settings(std::move(enabled)).has_value());
        fixture.events.clear();
        MANNY_CHECK(suite, fixture.controller->disconnect().has_value());
        MANNY_CHECK(suite,
                    fixture.events == std::vector<std::string>({"settings.save", "secret.erase"}));
        MANNY_CHECK(suite, fixture.secret_observer->value_.empty());
        const auto disconnected = fixture.controller->snapshot();
        MANNY_CHECK(suite, disconnected.state == application::DonBotConfigurationState::Unverified);
        MANNY_CHECK(suite, !disconnected.account_name.has_value());
        MANNY_CHECK(suite, !fixture.configuration->snapshot().settings.donbot.enabled);
    }

    {
        Fixture fixture;
        MANNY_CHECK(suite, fixture.controller
                               ->begin_verification("https://donbot.example",
                                                    support::SecretValue::from_text("KEY"))
                               .has_value());
        fixture.verifier.succeed_next();
        MANNY_CHECK(suite, fixture.controller->poll().has_value());
        fixture.settings_observer->fail_save = true;
        const auto failed = fixture.controller->select_guild("123");
        MANNY_CHECK(suite, !failed.has_value());
        MANNY_CHECK(suite, failed.error().code ==
                               application::DonBotConfigurationErrorCode::SettingsSaveFailed);
        MANNY_CHECK(suite, fixture.controller->snapshot().state ==
                               application::DonBotConfigurationState::Error);
        MANNY_CHECK(suite, !fixture.controller->snapshot().account_name.has_value());
    }

    {
        Fixture fixture;
        fixture.settings_observer->fail_save = true;
        fixture.events.clear();
        const auto failed = fixture.controller->disconnect();
        MANNY_CHECK(suite, !failed.has_value());
        MANNY_CHECK(suite, fixture.events == std::vector<std::string>({"settings.save"}));
        MANNY_CHECK(suite, fixture.secret_observer->erase_ids.empty());
    }

    {
        Fixture fixture;
        fixture.secret_observer->fail_erase = true;
        fixture.events.clear();
        const auto failed = fixture.controller->disconnect();
        MANNY_CHECK(suite, !failed.has_value());
        MANNY_CHECK(suite, failed.error().code ==
                               application::DonBotConfigurationErrorCode::SecretEraseFailed);
        MANNY_CHECK(suite,
                    fixture.events == std::vector<std::string>({"settings.save", "secret.erase"}));
        MANNY_CHECK(suite, !fixture.configuration->snapshot().settings.donbot.enabled);
    }

    {
        Fixture fixture;
        MANNY_CHECK(suite, fixture.controller
                               ->begin_verification("https://donbot.example",
                                                    support::SecretValue::from_text("KEY"))
                               .has_value());
        fixture.controller->shutdown();
        fixture.controller->shutdown();
        MANNY_CHECK(suite, fixture.controller->is_shutting_down());
        MANNY_CHECK(suite, fixture.verifier.cancel_calls == 1);
        MANNY_CHECK(suite, fixture.verifier.requests.empty());
        MANNY_CHECK(suite, fixture.controller->snapshot().state ==
                               application::DonBotConfigurationState::ShuttingDown);
        MANNY_CHECK(suite, !fixture.controller->begin_saved_verification().has_value());
        MANNY_CHECK(suite, !fixture.controller->poll().has_value());
        MANNY_CHECK(suite, !fixture.controller->select_guild("123").has_value());
        MANNY_CHECK(suite, !fixture.controller->disconnect().has_value());
    }
}

} // namespace

void run_donbot_configuration_controller_tests(TestSuite& suite) {
    candidate_success_and_selection_tests(suite);
    verification_failure_and_dispatch_tests(suite);
    discord_delivery_selection_tests(suite);
    persistence_failure_tests(suite);
    saved_verification_tests(suite);
    stale_disconnect_and_shutdown_tests(suite);
}

} // namespace manny_uploader::test
