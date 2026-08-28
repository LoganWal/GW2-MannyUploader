#include "manny_uploader/application/donbot_aggregate_delivery_controller.hpp"

#include <algorithm>
#include <limits>
#include <ranges>
#include <unordered_set>
#include <utility>

namespace manny_uploader::application {
namespace {

[[nodiscard]] DonBotAggregateDeliveryError make_error(DonBotAggregateDeliveryErrorCode code,
                                                      std::string message) {
    return DonBotAggregateDeliveryError{.code = code, .message = std::move(message)};
}

[[nodiscard]] const ports::DonBotGuild*
find_selected_guild(const DonBotConfigurationSnapshot& donbot) {
    const auto found =
        std::ranges::find(donbot.guilds, donbot.selected_guild_id, &ports::DonBotGuild::guild_id);
    return found == donbot.guilds.end() ? nullptr : &*found;
}

} // namespace

std::expected<std::unique_ptr<DonBotAggregateDeliveryController>, DonBotAggregateDeliveryError>
DonBotAggregateDeliveryController::create(UploadCoordinator& uploads,
                                          ConfigurationService& configuration,
                                          ports::IDonBotAggregateDelivery& delivery) {
    try {
        return std::unique_ptr<DonBotAggregateDeliveryController>{
            new DonBotAggregateDeliveryController{uploads, configuration, delivery}};
    } catch (...) {
        return std::unexpected(make_error(DonBotAggregateDeliveryErrorCode::Unavailable,
                                          "Unable to initialize DonBot aggregate delivery"));
    }
}

DonBotAggregateDeliveryController::DonBotAggregateDeliveryController(
    UploadCoordinator& uploads, ConfigurationService& configuration,
    ports::IDonBotAggregateDelivery& delivery) noexcept
    : uploads_{uploads}, configuration_{configuration}, delivery_{delivery} {}

std::expected<void, DonBotAggregateDeliveryError>
DonBotAggregateDeliveryController::submit(SendDonBotAggregateCommand command) {
    if (command.configuration_revision == 0 || command.donbot_revision == 0 ||
        command.job_ids.size() < 2 ||
        command.job_ids.size() > ports::max_donbot_aggregate_fight_logs ||
        std::ranges::any_of(command.job_ids,
                            [](domain::UploadJobId id) { return id.value == 0; })) {
        return std::unexpected(make_error(DonBotAggregateDeliveryErrorCode::InvalidCommand,
                                          "The DonBot aggregate selection is invalid"));
    }
    std::unordered_set<std::uint64_t> unique_jobs;
    for (const auto id : command.job_ids) {
        if (!unique_jobs.insert(id.value).second) {
            return std::unexpected(make_error(DonBotAggregateDeliveryErrorCode::InvalidCommand,
                                              "The DonBot aggregate selection is invalid"));
        }
    }

    const std::scoped_lock lock{mutex_};
    if (published_.shutting_down) {
        return std::unexpected(make_error(DonBotAggregateDeliveryErrorCode::ShuttingDown,
                                          "DonBot aggregate delivery is shutting down"));
    }
    if (!commands_.empty() || active_request_id_ || ticking_) {
        return std::unexpected(make_error(DonBotAggregateDeliveryErrorCode::QueueFull,
                                          "A DonBot aggregate delivery is already active"));
    }
    commands_.push_back(std::move(command));
    published_.state = DonBotAggregateDeliveryState::Queued;
    published_.fight_log_count = commands_.front().job_ids.size();
    published_.detail = "DonBot aggregate delivery is queued";
    published_.discord_delivery.reset();
    ++published_.revision;
    return {};
}

std::expected<void, DonBotAggregateDeliveryError>
DonBotAggregateDeliveryController::tick(const DonBotConfigurationSnapshot& donbot) {
    std::optional<SendDonBotAggregateCommand> command;
    {
        const std::scoped_lock lock{mutex_};
        if (published_.shutting_down) {
            return std::unexpected(make_error(DonBotAggregateDeliveryErrorCode::ShuttingDown,
                                              "DonBot aggregate delivery is shutting down"));
        }
        if (ticking_) {
            return std::unexpected(make_error(DonBotAggregateDeliveryErrorCode::Busy,
                                              "DonBot aggregate delivery is already advancing"));
        }
        ticking_ = true;
        if (!commands_.empty() && !active_request_id_) {
            command = std::move(commands_.front());
            commands_.pop_front();
        }
    }

    try {
        if (auto result = delivery_.try_take_result()) {
            publish_result(std::move(*result));
        }
        if (command) {
            if (auto error = dispatch(*command, donbot)) {
                const std::scoped_lock lock{mutex_};
                published_.state = DonBotAggregateDeliveryState::Failed;
                published_.fight_log_count = command->job_ids.size();
                published_.detail = error->message;
                published_.discord_delivery.reset();
                ++published_.revision;
            }
        }
    } catch (...) {
        const std::scoped_lock lock{mutex_};
        active_request_id_.reset();
        published_.state = DonBotAggregateDeliveryState::Failed;
        published_.detail = "DonBot aggregate delivery failed unexpectedly";
        published_.discord_delivery.reset();
        ++published_.revision;
    }

    {
        const std::scoped_lock lock{mutex_};
        ticking_ = false;
    }
    return {};
}

DonBotAggregateDeliverySnapshot DonBotAggregateDeliveryController::snapshot() const {
    const std::scoped_lock lock{mutex_};
    return published_;
}

void DonBotAggregateDeliveryController::shutdown() noexcept {
    {
        const std::scoped_lock lock{mutex_};
        if (published_.shutting_down) {
            return;
        }
        commands_.clear();
        active_request_id_.reset();
        published_.state = DonBotAggregateDeliveryState::ShuttingDown;
        published_.detail.clear();
        published_.discord_delivery.reset();
        published_.shutting_down = true;
        ++published_.revision;
    }
    delivery_.cancel_pending();
}

std::optional<DonBotAggregateDeliveryError>
DonBotAggregateDeliveryController::dispatch(const SendDonBotAggregateCommand& command,
                                            const DonBotConfigurationSnapshot& donbot) {
    const auto configuration = configuration_.snapshot();
    if (configuration.revision != command.configuration_revision ||
        donbot.revision != command.donbot_revision) {
        return make_error(DonBotAggregateDeliveryErrorCode::StaleConfiguration,
                          "DonBot settings changed before aggregate delivery started");
    }
    const auto& settings = configuration.settings.donbot;
    const auto* guild = find_selected_guild(donbot);
    const auto route = authorized_donbot_aggregate_route(configuration.settings, donbot);
    if (!settings.enabled || donbot.state != DonBotConfigurationState::Verified ||
        !donbot.discord_aggregate_delivery_v1 || guild == nullptr ||
        !guild->discord_delivery.aggregate_enabled ||
        route.mode == domain::DonBotDiscordDeliveryMode::None ||
        command.job_ids.size() > guild->discord_delivery.max_aggregate_fight_logs) {
        return make_error(DonBotAggregateDeliveryErrorCode::Unavailable,
                          "DonBot aggregate delivery is unavailable for the selected server");
    }

    const auto jobs = uploads_.snapshots();
    std::vector<std::uint64_t> fight_log_ids;
    fight_log_ids.reserve(command.job_ids.size());
    std::unordered_set<std::uint64_t> unique_fights;
    for (const auto id : command.job_ids) {
        const auto found = std::ranges::find(jobs, id, &UploadJobSnapshot::id);
        if (found == jobs.end()) {
            return make_error(DonBotAggregateDeliveryErrorCode::UnknownJob,
                              "A selected log is no longer retained");
        }
        if (!found->donbot_upload_receipt || !found->donbot_upload_receipt->fight_log_id ||
            *found->donbot_upload_receipt->fight_log_id >
                static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
            !unique_fights.insert(*found->donbot_upload_receipt->fight_log_id).second) {
            return make_error(DonBotAggregateDeliveryErrorCode::IneligibleJob,
                              "A selected log does not have a valid completed DonBot result");
        }
        fight_log_ids.push_back(*found->donbot_upload_receipt->fight_log_id);
    }
    if (next_request_id_ == std::numeric_limits<std::uint64_t>::max()) {
        return make_error(DonBotAggregateDeliveryErrorCode::Unavailable,
                          "No more DonBot aggregate requests can be created");
    }
    const auto request_id = next_request_id_++;
    auto queued = delivery_.enqueue(ports::DonBotAggregateDeliveryRequest{
        .request_id = request_id,
        .api_base_url = settings.api_base_url,
        .guild_id = settings.selected_guild_id,
        .fight_log_ids = std::move(fight_log_ids),
        .delivery_mode = route.mode,
        .channel_id = route.channel_id,
    });
    if (!queued) {
        return make_error(DonBotAggregateDeliveryErrorCode::DispatchFailed,
                          "DonBot aggregate delivery could not be queued");
    }
    const std::scoped_lock lock{mutex_};
    active_request_id_ = request_id;
    published_.state = DonBotAggregateDeliveryState::Sending;
    published_.fight_log_count = command.job_ids.size();
    published_.detail = "Sending DonBot aggregate";
    published_.discord_delivery.reset();
    ++published_.revision;
    return std::nullopt;
}

void DonBotAggregateDeliveryController::publish_result(
    ports::DonBotAggregateDeliveryResult result) {
    const std::scoped_lock lock{mutex_};
    if (!active_request_id_ || result.request_id != *active_request_id_) {
        return;
    }
    active_request_id_.reset();
    published_.fight_log_count = result.fight_log_count;
    published_.detail = std::move(result.detail);
    published_.discord_delivery = std::move(result.discord_delivery);
    switch (result.outcome) {
    case ports::DonBotAggregateDeliveryOutcome::Succeeded:
        published_.state = DonBotAggregateDeliveryState::Succeeded;
        break;
    case ports::DonBotAggregateDeliveryOutcome::Failed:
        published_.state = DonBotAggregateDeliveryState::Failed;
        break;
    case ports::DonBotAggregateDeliveryOutcome::Ambiguous:
        published_.state = DonBotAggregateDeliveryState::Ambiguous;
        break;
    case ports::DonBotAggregateDeliveryOutcome::Cancelled:
        published_.state = DonBotAggregateDeliveryState::Cancelled;
        break;
    }
    ++published_.revision;
}

} // namespace manny_uploader::application
