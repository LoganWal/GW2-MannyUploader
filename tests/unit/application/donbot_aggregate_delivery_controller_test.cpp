#include "manny_uploader/application/donbot_aggregate_delivery_controller.hpp"

#include "support/fakes.hpp"
#include "support/test_suite.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace manny_uploader::test {
namespace {

class MemorySettingsStore final : public ports::ISettingsStore {
  public:
    explicit MemorySettingsStore(config::Settings settings) : settings_{std::move(settings)} {}

    [[nodiscard]] std::expected<ports::SettingsLoadResult, ports::SettingsStoreError>
    load() const override {
        return ports::SettingsLoadResult{
            .settings = settings_,
            .source = ports::SettingsLoadSource::Primary,
            .recovery_diagnostic = std::nullopt,
        };
    }

    [[nodiscard]] std::expected<void, ports::SettingsStoreError>
    save(const config::Settings& settings) const override {
        settings_ = settings;
        return {};
    }

  private:
    mutable config::Settings settings_;
};

class MemorySecretStore final : public ports::ISecretStore {
  public:
    [[nodiscard]] std::expected<support::SecretValue, ports::SecretStoreError>
    load(ports::SecretId) const override {
        return support::SecretValue::from_text("KEY");
    }

    [[nodiscard]] std::expected<void, ports::SecretStoreError>
    store(ports::SecretId, const support::SecretValue&) override {
        return {};
    }

    [[nodiscard]] std::expected<void, ports::SecretStoreError> erase(ports::SecretId) override {
        return {};
    }
};

class FakeAggregateDelivery final : public ports::IDonBotAggregateDelivery {
  public:
    [[nodiscard]] std::expected<void, ports::DonBotAggregateDeliveryDispatchError>
    enqueue(ports::DonBotAggregateDeliveryRequest request) override {
        requests.push_back(std::move(request));
        return {};
    }

    [[nodiscard]] std::optional<ports::DonBotAggregateDeliveryResult> try_take_result() override {
        if (results.empty()) {
            return std::nullopt;
        }
        auto result = std::move(results.front());
        results.pop_front();
        return result;
    }

    [[nodiscard]] bool busy() const noexcept override {
        return !requests.empty() && results.empty();
    }

    void cancel_pending() noexcept override {
        ++cancel_calls;
    }

    std::vector<ports::DonBotAggregateDeliveryRequest> requests;
    std::deque<ports::DonBotAggregateDeliveryResult> results;
    std::size_t cancel_calls{};
};

struct Fixture {
    FakeClock clock;
    FakeUploadProvider dps{domain::Provider::DpsReport};
    FakeUploadProvider wingman{domain::Provider::Wingman};
    FakeUploadProvider donbot{domain::Provider::DonBot};
    FakeUploadProvider twitch{domain::Provider::Twitch};
    std::array<ports::IUploadProvider*, domain::provider_count> providers{&dps, &wingman, &donbot,
                                                                          &twitch};
    FakeAggregateDelivery aggregate;
};

[[nodiscard]] application::DonBotConfigurationSnapshot donbot_snapshot() {
    return application::DonBotConfigurationSnapshot{
        .state = application::DonBotConfigurationState::Verified,
        .api_base_url = "https://donbot.example",
        .account_name = "Player.1234",
        .discord_summary_delivery_v1 = true,
        .discord_aggregate_delivery_v1 = true,
        .guilds =
            {
                ports::DonBotGuild{
                    .guild_id = "123",
                    .guild_name = "Guild",
                    .discord_delivery =
                        ports::DonBotDiscordDeliveryPolicy{
                            .enabled = true,
                            .defaults_available = true,
                            .channel_override_allowed = true,
                            .pve_summary = true,
                            .aggregate_enabled = true,
                            .max_aggregate_fight_logs = 50,
                            .channels = {},
                        },
                },
            },
        .selected_guild_id = "123",
        .diagnostic = {},
        .revision = 7,
        .shutting_down = false,
    };
}

[[nodiscard]] ports::UploadResult donbot_success(domain::UploadJobId id, std::uint64_t fight_id,
                                                 std::optional<std::string> guild_id = "123") {
    return ports::UploadResult{
        .job_id = id,
        .provider = domain::Provider::DonBot,
        .outcome = ports::UploadOutcome::Succeeded,
        .detail = "uploaded",
        .retry_after = std::nullopt,
        .dps_report_result = std::nullopt,
        .wingman_upload_receipt = std::nullopt,
        .donbot_upload_receipt =
            domain::DonBotUploadReceipt{
                .upload_id = fight_id + 100,
                .fight_log_id = fight_id,
                .discord_delivery = {},
                .guild_id = std::move(guild_id),
            },
        .twitch_delivery_receipt = std::nullopt,
    };
}

void dispatch_and_result_tests(TestSuite& suite) {
    Fixture fixture;
    auto settings = config::make_default_settings("C:/logs");
    settings.donbot.enabled = true;
    settings.donbot.api_base_url = "https://donbot.example";
    settings.donbot.selected_guild_id = "123";
    auto configuration = application::ConfigurationService::create(
        std::make_unique<MemorySettingsStore>(settings), std::make_unique<MemorySecretStore>());
    MANNY_CHECK(suite, configuration.has_value());
    auto uploads = application::UploadCoordinator::create(fixture.clock, fixture.providers);
    MANNY_CHECK(suite, uploads.has_value());

    domain::ProviderSelection providers{};
    providers[domain::provider_index(domain::Provider::DonBot)] = true;
    auto first =
        uploads->add_job(domain::LogFileIdentity{.canonical_path = "one.zevtc", .size = 1},
                         domain::EncounterMetadata{.boss_id = 1, .pov_account = {}}, providers);
    auto second =
        uploads->add_job(domain::LogFileIdentity{.canonical_path = "two.zevtc", .size = 2},
                         domain::EncounterMetadata{.boss_id = 2, .pov_account = {}}, providers);
    MANNY_CHECK(suite, first.has_value() && second.has_value());
    MANNY_CHECK(suite, uploads->handle_result(donbot_success(*first, 101)).has_value());
    MANNY_CHECK(suite, uploads->handle_result(donbot_success(*second, 202)).has_value());

    auto controller = application::DonBotAggregateDeliveryController::create(
        *uploads, *configuration, fixture.aggregate);
    MANNY_CHECK(suite, controller.has_value());
    const auto verified = donbot_snapshot();
    MANNY_CHECK(suite, (*controller)
                           ->submit(application::SendDonBotAggregateCommand{
                               .job_ids = {*first, *second},
                               .configuration_revision = configuration->snapshot().revision,
                               .donbot_revision = verified.revision,
                           })
                           .has_value());
    MANNY_CHECK(suite, (*controller)->tick(verified).has_value());
    MANNY_CHECK(suite, fixture.aggregate.requests.size() == 1);
    if (!fixture.aggregate.requests.empty()) {
        const auto& request = fixture.aggregate.requests.front();
        MANNY_CHECK(suite, request.fight_log_ids == std::vector<std::uint64_t>({101, 202}));
        MANNY_CHECK(suite, request.guild_id == "123");
        MANNY_CHECK(suite,
                    request.delivery_mode == domain::DonBotDiscordDeliveryMode::GuildDefaults);
    }
    MANNY_CHECK(suite, (*controller)->snapshot().state ==
                           application::DonBotAggregateDeliveryState::Sending);

    fixture.aggregate.results.push_back(ports::DonBotAggregateDeliveryResult{
        .request_id = fixture.aggregate.requests.front().request_id,
        .outcome = ports::DonBotAggregateDeliveryOutcome::Succeeded,
        .fight_log_count = 2,
        .detail = "DonBot aggregate delivery completed",
        .discord_delivery =
            domain::DonBotDiscordDeliveryReceipt{
                .outcome = domain::DonBotDiscordDeliveryOutcome::Sent,
                .sent = 2,
            },
    });
    MANNY_CHECK(suite, (*controller)->tick(verified).has_value());
    MANNY_CHECK(suite, (*controller)->snapshot().state ==
                           application::DonBotAggregateDeliveryState::Succeeded);
    MANNY_CHECK(suite, (*controller)->snapshot().discord_delivery.has_value());

    MANNY_CHECK(suite, (*controller)
                           ->submit(application::SendDonBotAggregateCommand{
                               .job_ids = {*first, *second},
                               .configuration_revision = configuration->snapshot().revision + 1,
                               .donbot_revision = verified.revision,
                           })
                           .has_value());
    MANNY_CHECK(suite, (*controller)->tick(verified).has_value());
    MANNY_CHECK(suite, (*controller)->snapshot().state ==
                           application::DonBotAggregateDeliveryState::Failed);
    MANNY_CHECK(suite, fixture.aggregate.requests.size() == 1);

    (*controller)->shutdown();
    MANNY_CHECK(suite, fixture.aggregate.cancel_calls == 1);
    MANNY_CHECK(suite, !(*controller)
                            ->submit(application::SendDonBotAggregateCommand{
                                .job_ids = {*first, *second},
                                .configuration_revision = configuration->snapshot().revision,
                                .donbot_revision = verified.revision,
                            })
                            .has_value());
}

} // namespace

void run_donbot_aggregate_delivery_controller_tests(TestSuite& suite) {
    dispatch_and_result_tests(suite);
}

} // namespace manny_uploader::test
