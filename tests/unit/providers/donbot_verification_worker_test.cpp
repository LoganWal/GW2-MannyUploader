#include "manny_uploader/providers/donbot_verification_worker.hpp"

#include "support/test_suite.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <expected>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

namespace manny_uploader::test {
namespace {

using namespace std::chrono_literals;

[[nodiscard]] providers::DonBotError client_error(providers::DonBotDisposition disposition,
                                                  std::string detail) {
    return providers::DonBotError{
        .disposition = disposition,
        .detail = std::move(detail),
        .retry_after = std::nullopt,
        .http_error = std::nullopt,
        .http_status = std::nullopt,
    };
}

[[nodiscard]] std::string secret_text(const support::SecretValue& value) {
    const auto bytes = value.bytes();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

[[nodiscard]] ports::DonBotVerificationRequest request(std::uint64_t id,
                                                       std::string key = "GW2-KEY") {
    return ports::DonBotVerificationRequest{
        .request_id = id,
        .api_base_url = "https://donbot.example",
        .api_key = support::SecretValue::from_text(key),
    };
}

class FakeVerificationClient final : public providers::IDonBotClient {
  public:
    using Result = std::expected<providers::DonBotVerification, providers::DonBotError>;

    void push(Result result) {
        const std::scoped_lock lock{mutex_};
        results_.push_back(std::move(result));
    }

    void block() {
        const std::scoped_lock lock{mutex_};
        blocked_ = true;
        released_ = false;
    }

    void release() {
        {
            const std::scoped_lock lock{mutex_};
            released_ = true;
        }
        condition_.notify_all();
    }

    void throw_next() {
        const std::scoped_lock lock{mutex_};
        throw_next_ = true;
    }

    [[nodiscard]] bool wait_for_calls(std::size_t count, std::chrono::milliseconds timeout) const {
        std::unique_lock lock{mutex_};
        return condition_.wait_for(lock, timeout, [this, count] { return calls_ >= count; });
    }

    [[nodiscard]] bool saw_stop() const {
        const std::scoped_lock lock{mutex_};
        return saw_stop_;
    }

    [[nodiscard]] std::string last_key() const {
        const std::scoped_lock lock{mutex_};
        return last_key_;
    }

    [[nodiscard]] std::expected<providers::DonBotVerification, providers::DonBotError>
    verify(std::string_view, const support::SecretValue& api_key,
           const std::stop_token& stop_token) const override {
        std::stop_callback wake_on_stop{stop_token, [this] { condition_.notify_all(); }};
        std::unique_lock lock{mutex_};
        ++calls_;
        last_key_ = secret_text(api_key);
        condition_.notify_all();
        condition_.wait(lock, [this, &stop_token] {
            return !blocked_ || released_ || stop_token.stop_requested();
        });
        if (stop_token.stop_requested()) {
            saw_stop_ = true;
            return std::unexpected(
                client_error(providers::DonBotDisposition::Cancelled, "cancelled"));
        }
        if (throw_next_) {
            throw_next_ = false;
            throw std::runtime_error{"private verification exception"};
        }
        if (results_.empty()) {
            return providers::DonBotVerification{
                .account_name = "Player.1234",
                .guilds = {{.guild_id = "123", .guild_name = "Guild"}},
            };
        }
        auto result = std::move(results_.front());
        results_.pop_front();
        return result;
    }

    [[nodiscard]] std::expected<providers::DonBotUploadSuccess, providers::DonBotError>
    upload(const domain::LogFileIdentity&, std::string_view, std::string_view,
           const support::SecretValue&, const std::stop_token&) const override {
        return std::unexpected(client_error(providers::DonBotDisposition::Failed, "unused"));
    }

    [[nodiscard]] std::expected<providers::DonBotUploadSuccess, providers::DonBotError>
    import_permalink(std::string_view, std::string_view, std::string_view,
                     const support::SecretValue&, const std::stop_token&) const override {
        return std::unexpected(client_error(providers::DonBotDisposition::Failed, "unused"));
    }

  private:
    mutable std::mutex mutex_;
    mutable std::condition_variable condition_;
    mutable std::deque<Result> results_;
    mutable std::size_t calls_{};
    mutable std::string last_key_;
    mutable bool blocked_{};
    mutable bool released_{};
    mutable bool throw_next_{};
    mutable bool saw_stop_{};
};

void success_and_failure_tests(TestSuite& suite) {
    FakeVerificationClient client;
    const auto invalid = providers::DonBotVerificationWorker::create(client, 0);
    MANNY_CHECK(suite, !invalid.has_value());
    MANNY_CHECK(suite, invalid.error().code ==
                           providers::DonBotVerificationWorkerErrorCode::InvalidCapacity);

    auto created = providers::DonBotVerificationWorker::create(client, 3);
    MANNY_CHECK(suite, created.has_value());
    if (!created) {
        return;
    }
    auto worker = std::move(*created);
    client.push(providers::DonBotVerification{
        .account_name = "Player.1234",
        .guilds = {{.guild_id = "123", .guild_name = "Guild One"}},
    });
    client.push(std::unexpected(
        client_error(providers::DonBotDisposition::Retry, "Please verify again later")));
    client.push(std::unexpected(
        client_error(providers::DonBotDisposition::Cancelled, "Verification cancelled")));

    MANNY_CHECK(suite, worker->enqueue(request(11, "FIRST-KEY")).has_value());
    auto success = worker->wait_for_result(2s);
    MANNY_CHECK(suite, success.has_value());
    MANNY_CHECK(suite, success && success->request_id == 11);
    MANNY_CHECK(suite, success && success->verification.has_value());
    if (success && success->verification) {
        MANNY_CHECK(suite, success->verification->identity.account_name == "Player.1234");
        MANNY_CHECK(suite, success->verification->identity.guilds.size() == 1);
        MANNY_CHECK(suite, secret_text(success->verification->api_key) == "FIRST-KEY");
    }
    MANNY_CHECK(suite, client.last_key() == "FIRST-KEY");

    MANNY_CHECK(suite, worker->enqueue(request(12)).has_value());
    auto failed = worker->wait_for_result(2s);
    MANNY_CHECK(suite, failed.has_value());
    MANNY_CHECK(suite, failed && !failed->verification.has_value());
    MANNY_CHECK(suite, failed && failed->verification.error().code ==
                                     ports::DonBotVerificationFailureCode::Failed);

    MANNY_CHECK(suite, worker->enqueue(request(13)).has_value());
    auto cancelled = worker->wait_for_result(2s);
    MANNY_CHECK(suite, cancelled.has_value());
    MANNY_CHECK(suite, cancelled && !cancelled->verification.has_value());
    MANNY_CHECK(suite, cancelled && cancelled->verification.error().code ==
                                        ports::DonBotVerificationFailureCode::Cancelled);
}

void backpressure_exception_and_cancellation_tests(TestSuite& suite) {
    FakeVerificationClient blocked;
    blocked.block();
    auto created = providers::DonBotVerificationWorker::create(blocked, 1);
    MANNY_CHECK(suite, created.has_value());
    if (!created) {
        return;
    }
    auto worker = std::move(*created);
    MANNY_CHECK(suite, worker->enqueue(request(21)).has_value());
    MANNY_CHECK(suite, blocked.wait_for_calls(1, 2s));
    MANNY_CHECK(suite, worker->enqueue(request(22)).has_value());
    MANNY_CHECK(suite, !worker->enqueue(request(23)).has_value());
    MANNY_CHECK(suite, worker->pending_count() == 1);
    worker->cancel_pending();
    MANNY_CHECK(suite, worker->is_stopping());
    MANNY_CHECK(suite, worker->pending_count() == 0);
    MANNY_CHECK(suite, worker->result_count() == 0);
    MANNY_CHECK(suite, !worker->enqueue(request(24)).has_value());
    worker.reset();
    MANNY_CHECK(suite, blocked.saw_stop());

    FakeVerificationClient throwing;
    throwing.throw_next();
    auto throwing_worker = providers::DonBotVerificationWorker::create(throwing);
    MANNY_CHECK(suite, throwing_worker.has_value());
    MANNY_CHECK(suite, (*throwing_worker)->enqueue(request(31)).has_value());
    const auto result = (*throwing_worker)->wait_for_result(2s);
    MANNY_CHECK(suite, result.has_value());
    MANNY_CHECK(suite, result && !result->verification.has_value());
    MANNY_CHECK(suite,
                result && result->verification.error().detail.find("private") == std::string::npos);
}

} // namespace

void run_donbot_verification_worker_tests(TestSuite& suite) {
    success_and_failure_tests(suite);
    backpressure_exception_and_cancellation_tests(suite);
}

} // namespace manny_uploader::test
