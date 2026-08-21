#include "manny_uploader/providers/twitch_authentication_worker.hpp"

#include <exception>
#include <type_traits>
#include <utility>
#include <variant>

namespace manny_uploader::providers {
namespace {

[[nodiscard]] TwitchAuthenticationWorkerError
make_worker_error(TwitchAuthenticationWorkerErrorCode code, std::string message) {
    return TwitchAuthenticationWorkerError{.code = code, .message = std::move(message)};
}

[[nodiscard]] ports::TwitchAuthenticationFailureCode
map_disposition(TwitchDisposition disposition) noexcept {
    switch (disposition) {
    case TwitchDisposition::Retry:
        return ports::TwitchAuthenticationFailureCode::Retry;
    case TwitchDisposition::Reconnect:
        return ports::TwitchAuthenticationFailureCode::Reconnect;
    case TwitchDisposition::Failed:
        return ports::TwitchAuthenticationFailureCode::Failed;
    case TwitchDisposition::Cancelled:
        return ports::TwitchAuthenticationFailureCode::Cancelled;
    }
    return ports::TwitchAuthenticationFailureCode::Failed;
}

[[nodiscard]] ports::TwitchAuthenticationFailure failure_from(TwitchError error) {
    return ports::TwitchAuthenticationFailure{
        .code = map_disposition(error.disposition),
        .detail = std::move(error.detail),
        .retry_after = error.retry_after,
        .device_code = std::nullopt,
        .credentials = std::nullopt,
    };
}

[[nodiscard]] ports::TwitchAuthenticationFailure unexpected_failure() {
    return ports::TwitchAuthenticationFailure{
        .code = ports::TwitchAuthenticationFailureCode::Failed,
        .detail = "Twitch authentication failed unexpectedly",
        .retry_after = std::nullopt,
        .device_code = std::nullopt,
        .credentials = std::nullopt,
    };
}

template <typename Value>
[[nodiscard]] std::expected<ports::TwitchAuthenticationSuccess, ports::TwitchAuthenticationFailure>
make_success(Value value) {
    return ports::TwitchAuthenticationSuccess{std::in_place_type<Value>, std::move(value)};
}

void retain_command_secret(ports::TwitchAuthenticationFailure& failure,
                           ports::TwitchAuthenticationCommand& command) {
    std::visit(
        [&failure]<typename Command>(Command& value) {
            if constexpr (std::is_same_v<Command, ports::TwitchPollAuthentication>) {
                failure.device_code.emplace(std::move(value.device_code));
            } else if constexpr (std::is_same_v<Command, ports::TwitchValidateAuthentication> ||
                                 std::is_same_v<Command, ports::TwitchRefreshAuthentication>) {
                failure.credentials.emplace(std::move(value.credentials));
            }
        },
        command);
}

} // namespace

std::expected<std::unique_ptr<TwitchAuthenticationWorker>, TwitchAuthenticationWorkerError>
TwitchAuthenticationWorker::create(const ITwitchClient& client, std::size_t queue_capacity) {
    if (queue_capacity == 0) {
        return std::unexpected(
            make_worker_error(TwitchAuthenticationWorkerErrorCode::InvalidCapacity,
                              "Twitch authentication queue capacity must be non-zero"));
    }

    try {
        auto worker = std::unique_ptr<TwitchAuthenticationWorker>{
            new TwitchAuthenticationWorker{client, queue_capacity}};
        worker->thread_ = std::jthread{
            [instance = worker.get()](std::stop_token token) { instance->run(std::move(token)); }};
        return worker;
    } catch (...) {
        return std::unexpected(
            make_worker_error(TwitchAuthenticationWorkerErrorCode::ThreadStartFailed,
                              "Unable to start Twitch authentication"));
    }
}

TwitchAuthenticationWorker::TwitchAuthenticationWorker(const ITwitchClient& client,
                                                       std::size_t queue_capacity)
    : client_{client}, queue_capacity_{queue_capacity} {}

TwitchAuthenticationWorker::~TwitchAuthenticationWorker() {
    cancel_pending();
}

std::expected<void, ports::TwitchAuthenticationDispatchError>
TwitchAuthenticationWorker::enqueue(ports::TwitchAuthenticationRequest request) {
    std::unique_lock lock{mutex_};
    if (stopping_) {
        return std::unexpected(ports::TwitchAuthenticationDispatchError{
            .message = "Twitch authentication is stopping"});
    }
    if (request.request_id == 0) {
        return std::unexpected(ports::TwitchAuthenticationDispatchError{
            .message = "Twitch authentication request ID must be non-zero"});
    }
    if (requests_.size() >= queue_capacity_) {
        return std::unexpected(ports::TwitchAuthenticationDispatchError{
            .message = "Twitch authentication queue is full"});
    }

    try {
        requests_.push_back(std::move(request));
    } catch (...) {
        return std::unexpected(ports::TwitchAuthenticationDispatchError{
            .message = "Unable to queue Twitch authentication"});
    }
    lock.unlock();
    condition_.notify_all();
    return {};
}

std::optional<ports::TwitchAuthenticationResult> TwitchAuthenticationWorker::try_take_result() {
    std::unique_lock lock{mutex_};
    auto result = take_result_locked();
    lock.unlock();
    if (result) {
        condition_.notify_all();
    }
    return result;
}

void TwitchAuthenticationWorker::cancel_pending() noexcept {
    {
        const std::scoped_lock lock{mutex_};
        if (stopping_) {
            return;
        }
        stopping_ = true;
        requests_.clear();
        results_.clear();
        thread_.request_stop();
    }
    condition_.notify_all();
}

std::optional<ports::TwitchAuthenticationResult>
TwitchAuthenticationWorker::wait_for_result(std::chrono::milliseconds timeout) {
    std::unique_lock lock{mutex_};
    condition_.wait_for(lock, timeout, [this] { return stopping_ || !results_.empty(); });
    auto result = take_result_locked();
    lock.unlock();
    if (result) {
        condition_.notify_all();
    }
    return result;
}

std::size_t TwitchAuthenticationWorker::pending_count() const noexcept {
    const std::scoped_lock lock{mutex_};
    return requests_.size();
}

std::size_t TwitchAuthenticationWorker::result_count() const noexcept {
    const std::scoped_lock lock{mutex_};
    return results_.size();
}

bool TwitchAuthenticationWorker::is_stopping() const noexcept {
    const std::scoped_lock lock{mutex_};
    return stopping_;
}

void TwitchAuthenticationWorker::run(std::stop_token stop_token) {
    while (!stop_token.stop_requested()) {
        ports::TwitchAuthenticationRequest request;
        {
            std::unique_lock lock{mutex_};
            condition_.wait(lock, [this, &stop_token] {
                return stop_token.stop_requested() || !requests_.empty();
            });
            if (stop_token.stop_requested()) {
                return;
            }
            request = std::move(requests_.front());
            requests_.pop_front();
        }
        condition_.notify_all();

        auto result = execute(std::move(request), stop_token);
        std::unique_lock lock{mutex_};
        condition_.wait(lock, [this, &stop_token] {
            return stop_token.stop_requested() || results_.size() < queue_capacity_;
        });
        if (stop_token.stop_requested()) {
            return;
        }
        try {
            results_.push_back(std::move(result));
        } catch (...) {
            stopping_ = true;
            requests_.clear();
            results_.clear();
            thread_.request_stop();
            lock.unlock();
            condition_.notify_all();
            return;
        }
        lock.unlock();
        condition_.notify_all();
    }
}

ports::TwitchAuthenticationResult
TwitchAuthenticationWorker::execute(ports::TwitchAuthenticationRequest request,
                                    const std::stop_token& stop_token) const {
    const auto operation = ports::authentication_operation(request.command);
    std::expected<ports::TwitchAuthenticationSuccess, ports::TwitchAuthenticationFailure> outcome =
        std::unexpected(unexpected_failure());

    try {
        outcome = std::visit(
            [this, &stop_token]<typename Command>(
                Command& command) -> std::expected<ports::TwitchAuthenticationSuccess,
                                                   ports::TwitchAuthenticationFailure> {
                if constexpr (std::is_same_v<Command, ports::TwitchStartAuthentication>) {
                    auto started = client_.start_device_authorization(stop_token);
                    if (!started) {
                        return std::unexpected(failure_from(std::move(started.error())));
                    }
                    return make_success(ports::TwitchAuthorizationStarted{
                        .device_code = std::move(started->device_code),
                        .user_code = std::move(started->user_code),
                        .verification_uri = std::move(started->verification_uri),
                        .expires_in = started->expires_in,
                        .polling_interval = started->polling_interval,
                    });
                } else if constexpr (std::is_same_v<Command, ports::TwitchPollAuthentication>) {
                    auto polled =
                        client_.poll_device_authorization(command.device_code, stop_token);
                    if (!polled) {
                        auto failure = failure_from(std::move(polled.error()));
                        failure.device_code.emplace(std::move(command.device_code));
                        return std::unexpected(std::move(failure));
                    }
                    if (std::holds_alternative<TwitchAuthorizationPending>(*polled)) {
                        return make_success(ports::TwitchAuthorizationPending{
                            .device_code = std::move(command.device_code),
                        });
                    }
                    auto grant = std::get<TwitchTokenGrant>(std::move(*polled));
                    return make_success(ports::TwitchAuthorizationGranted{
                        .access_token = std::move(grant.access_token),
                        .refresh_token = std::move(grant.refresh_token),
                        .expires_in = grant.expires_in,
                        .scopes = std::move(grant.scopes),
                    });
                } else if constexpr (std::is_same_v<Command, ports::TwitchValidateAuthentication>) {
                    auto validated =
                        client_.validate_access_token(command.credentials.access_token, stop_token);
                    if (!validated) {
                        auto failure = failure_from(std::move(validated.error()));
                        failure.credentials.emplace(std::move(command.credentials));
                        return std::unexpected(std::move(failure));
                    }
                    return make_success(ports::TwitchValidationSucceeded{
                        .credentials = std::move(command.credentials),
                        .user_id = std::move(validated->user_id),
                        .login = std::move(validated->login),
                        .expires_in = validated->expires_in,
                        .scopes = std::move(validated->scopes),
                    });
                } else if constexpr (std::is_same_v<Command, ports::TwitchRefreshAuthentication>) {
                    auto refreshed =
                        client_.refresh_access_token(command.credentials.refresh_token, stop_token);
                    if (!refreshed) {
                        auto failure = failure_from(std::move(refreshed.error()));
                        failure.credentials.emplace(std::move(command.credentials));
                        return std::unexpected(std::move(failure));
                    }
                    return make_success(ports::TwitchRefreshSucceeded{
                        .access_token = std::move(refreshed->access_token),
                        .refresh_token = std::move(refreshed->refresh_token),
                        .expires_in = refreshed->expires_in,
                        .scopes = std::move(refreshed->scopes),
                    });
                } else {
                    auto revoked = client_.revoke_access_token(command.access_token, stop_token);
                    if (!revoked) {
                        return std::unexpected(failure_from(std::move(revoked.error())));
                    }
                    return make_success(ports::TwitchRevocationSucceeded{});
                }
            },
            request.command);
    } catch (...) {
        auto failure = unexpected_failure();
        retain_command_secret(failure, request.command);
        outcome = std::unexpected(std::move(failure));
    }

    return ports::TwitchAuthenticationResult{
        .request_id = request.request_id,
        .operation = operation,
        .outcome = std::move(outcome),
    };
}

std::optional<ports::TwitchAuthenticationResult> TwitchAuthenticationWorker::take_result_locked() {
    if (results_.empty()) {
        return std::nullopt;
    }
    auto result = std::move(results_.front());
    results_.pop_front();
    return result;
}

} // namespace manny_uploader::providers
