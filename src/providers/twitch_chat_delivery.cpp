#include "manny_uploader/providers/twitch_chat_delivery.hpp"

#include <utility>

namespace manny_uploader::providers {
namespace {

constexpr auto default_retry_delay = std::chrono::seconds{30};

[[nodiscard]] TwitchChatDeliveryResult
make_result(TwitchChatDeliveryOutcome outcome, std::string detail,
            std::optional<std::chrono::seconds> retry_after = std::nullopt,
            std::optional<domain::TwitchDeliveryReceipt> receipt = std::nullopt,
            bool delivery_ambiguous = false) {
    return TwitchChatDeliveryResult{
        .outcome = outcome,
        .detail = std::move(detail),
        .retry_after = retry_after,
        .receipt = std::move(receipt),
        .delivery_ambiguous = delivery_ambiguous,
    };
}

[[nodiscard]] std::chrono::seconds
bounded_retry_delay(std::optional<std::chrono::seconds> delay) noexcept {
    if (!delay || *delay <= std::chrono::seconds::zero() || *delay > std::chrono::hours{24}) {
        return default_retry_delay;
    }
    return *delay;
}

[[nodiscard]] domain::TwitchDeliveryStatus drop_status(std::string_view code) noexcept {
    if (code == "automod_held" || code == "automod_caught") {
        return domain::TwitchDeliveryStatus::AutoMod;
    }
    if (code == "blocked_term") {
        return domain::TwitchDeliveryStatus::BlockedTerm;
    }
    if (code == "msg_duplicate") {
        return domain::TwitchDeliveryStatus::Duplicate;
    }
    if (code == "rate_limited") {
        return domain::TwitchDeliveryStatus::RateLimited;
    }
    if (code == "followers_only") {
        return domain::TwitchDeliveryStatus::FollowersOnly;
    }
    if (code == "slow_mode") {
        return domain::TwitchDeliveryStatus::SlowMode;
    }
    if (code == "subscribers_only") {
        return domain::TwitchDeliveryStatus::SubscribersOnly;
    }
    if (code == "banned" || code == "channel_suspended" || code == "restricted") {
        return domain::TwitchDeliveryStatus::Restricted;
    }
    return domain::TwitchDeliveryStatus::OtherDrop;
}

[[nodiscard]] std::string drop_detail(domain::TwitchDeliveryStatus status) {
    switch (status) {
    case domain::TwitchDeliveryStatus::AutoMod:
        return "Twitch held the message for moderation";
    case domain::TwitchDeliveryStatus::BlockedTerm:
        return "Twitch rejected a blocked term in the message";
    case domain::TwitchDeliveryStatus::Duplicate:
        return "Twitch rejected a duplicate chat message";
    case domain::TwitchDeliveryStatus::RateLimited:
        return "Twitch rejected the message because chat is rate limited";
    case domain::TwitchDeliveryStatus::FollowersOnly:
        return "Twitch rejected the message because chat is followers-only";
    case domain::TwitchDeliveryStatus::SlowMode:
        return "Twitch rejected the message because slow mode is active";
    case domain::TwitchDeliveryStatus::SubscribersOnly:
        return "Twitch rejected the message because chat is subscribers-only";
    case domain::TwitchDeliveryStatus::Restricted:
        return "Twitch rejected the message because the account or channel is restricted";
    case domain::TwitchDeliveryStatus::OtherDrop:
        return "Twitch rejected the chat message";
    case domain::TwitchDeliveryStatus::Sent:
        break;
    }
    return "Posted to Twitch chat";
}

[[nodiscard]] bool safely_retryable_chat_failure(const TwitchError& error) noexcept {
    if (error.http_status == 429) {
        return true;
    }
    if (!error.http_error) {
        return false;
    }
    return *error.http_error == ports::HttpErrorCode::NameResolutionFailed ||
           *error.http_error == ports::HttpErrorCode::ConnectionFailed ||
           *error.http_error == ports::HttpErrorCode::TlsFailed;
}

[[nodiscard]] TwitchChatDeliveryResult session_failure(ports::TwitchDeliverySessionError error) {
    switch (error.code) {
    case ports::TwitchDeliverySessionErrorCode::Retry:
        return make_result(TwitchChatDeliveryOutcome::Retry, std::move(error.detail),
                           bounded_retry_delay(error.retry_after));
    case ports::TwitchDeliverySessionErrorCode::Cancelled:
        return make_result(TwitchChatDeliveryOutcome::Cancelled, std::move(error.detail));
    case ports::TwitchDeliverySessionErrorCode::NotConnected:
    case ports::TwitchDeliverySessionErrorCode::ReconnectRequired:
    case ports::TwitchDeliverySessionErrorCode::Failed:
        return make_result(TwitchChatDeliveryOutcome::Failed, std::move(error.detail));
    }
    return make_result(TwitchChatDeliveryOutcome::Failed, "The Twitch session is unavailable");
}

[[nodiscard]] TwitchChatDeliveryResult finalize(std::expected<TwitchChatResult, TwitchError> sent) {
    if (!sent) {
        auto error = std::move(sent.error());
        if (error.disposition == TwitchDisposition::Retry && safely_retryable_chat_failure(error)) {
            return make_result(TwitchChatDeliveryOutcome::Retry, std::move(error.detail),
                               bounded_retry_delay(error.retry_after));
        }
        if (error.disposition == TwitchDisposition::Reconnect) {
            return make_result(TwitchChatDeliveryOutcome::Failed,
                               "Twitch must be reconnected before posting again");
        }
        if (error.disposition == TwitchDisposition::Cancelled) {
            return make_result(TwitchChatDeliveryOutcome::Cancelled, std::move(error.detail),
                               std::nullopt, std::nullopt, true);
        }
        const bool definitive_rejection =
            error.http_status && *error.http_status >= 400 && *error.http_status < 500;
        if (!definitive_rejection) {
            return make_result(
                TwitchChatDeliveryOutcome::Failed,
                "Twitch delivery could not be confirmed; automatic retry was suppressed",
                std::nullopt, std::nullopt, true);
        }
        return make_result(TwitchChatDeliveryOutcome::Failed, std::move(error.detail));
    }

    if (!sent->is_sent) {
        const auto status = sent->drop_reason ? drop_status(sent->drop_reason->code)
                                              : domain::TwitchDeliveryStatus::OtherDrop;
        return make_result(TwitchChatDeliveryOutcome::Dropped, drop_detail(status), std::nullopt,
                           domain::TwitchDeliveryReceipt{
                               .status = status,
                               .message_id = std::nullopt,
                           });
    }
    if (!sent->message_id) {
        return make_result(TwitchChatDeliveryOutcome::Failed,
                           "Twitch delivery could not be confirmed; automatic retry was suppressed",
                           std::nullopt, std::nullopt, true);
    }
    return make_result(TwitchChatDeliveryOutcome::Sent, "Posted to Twitch chat", std::nullopt,
                       domain::TwitchDeliveryReceipt{
                           .status = domain::TwitchDeliveryStatus::Sent,
                           .message_id = std::move(sent->message_id),
                       });
}

} // namespace

TwitchChatDelivery::TwitchChatDelivery(
    const ITwitchClient& client, const ports::ITwitchDeliverySessionAccess& session_access) noexcept
    : client_{client}, session_access_{session_access} {}

TwitchChatDeliveryResult TwitchChatDelivery::send(std::string_view message,
                                                  const std::stop_token& stop_token) const {
    if (stop_token.stop_requested()) {
        return make_result(TwitchChatDeliveryOutcome::Cancelled,
                           "Twitch chat delivery was cancelled");
    }
    auto session = session_access_.acquire(stop_token);
    if (!session) {
        return session_failure(std::move(session.error()));
    }

    auto sent = client_.send_chat_message(session->authenticated_user_id, message,
                                          session->access_token, stop_token);
    if (!sent && sent.error().disposition == TwitchDisposition::Reconnect) {
        auto recovered = session_access_.recover(std::move(*session), stop_token);
        if (!recovered) {
            return session_failure(std::move(recovered.error()));
        }
        sent = client_.send_chat_message(recovered->authenticated_user_id, message,
                                         recovered->access_token, stop_token);
    }
    return finalize(std::move(sent));
}

} // namespace manny_uploader::providers
