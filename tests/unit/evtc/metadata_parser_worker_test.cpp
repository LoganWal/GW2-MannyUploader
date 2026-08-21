#include "manny_uploader/evtc/metadata_parser_worker.hpp"
#include "support/test_suite.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <utility>

namespace manny_uploader::test {
namespace {

using namespace std::chrono_literals;
using evtc::MetadataParserWorker;
using evtc::MetadataParserWorkerErrorCode;
using ports::MetadataParseError;
using ports::MetadataParseErrorCode;
using ports::MetadataParseRequest;

[[nodiscard]] domain::LogFileIdentity file(std::string name) {
    return domain::LogFileIdentity{
        .canonical_path = std::filesystem::path{"logs"} / std::move(name),
        .size = 4096,
        .last_write_time = {},
    };
}

[[nodiscard]] MetadataParseRequest request(std::uint64_t id, std::string name) {
    return MetadataParseRequest{
        .job_id = domain::UploadJobId{id},
        .file = file(std::move(name)),
    };
}

class FakeReader final : public ports::ILogMetadataReader {
  public:
    enum class Behavior : std::uint8_t {
        Succeed,
        Fail,
        Throw,
    };

    explicit FakeReader(Behavior behavior = Behavior::Succeed) : behavior_(behavior) {}

    [[nodiscard]] std::expected<domain::EncounterMetadata, MetadataParseError>
    parse(const domain::LogFileIdentity& input, const std::stop_token&) override {
        ++parse_count;
        {
            const std::scoped_lock lock{mutex_};
            last_path = input.canonical_path;
        }
        if (behavior_ == Behavior::Throw) {
            throw std::runtime_error{"reader exploded"};
        }
        if (behavior_ == Behavior::Fail) {
            return std::unexpected(MetadataParseError{
                .code = MetadataParseErrorCode::MalformedLog,
                .message = "malformed fixture",
            });
        }
        return domain::EncounterMetadata{
            .boss_id = 777,
            .pov_account = ":Broadcaster.1234",
        };
    }

    [[nodiscard]] std::filesystem::path captured_path() const {
        const std::scoped_lock lock{mutex_};
        return last_path;
    }

    std::atomic_size_t parse_count{};

  private:
    Behavior behavior_;
    mutable std::mutex mutex_;
    std::filesystem::path last_path;
};

class BlockingReader final : public ports::ILogMetadataReader {
  public:
    [[nodiscard]] std::expected<domain::EncounterMetadata, MetadataParseError>
    parse(const domain::LogFileIdentity&, const std::stop_token& stop_token) override {
        std::stop_callback wake_on_stop{stop_token, [this] { condition_.notify_all(); }};
        std::unique_lock lock{mutex_};
        started_ = true;
        condition_.notify_all();
        condition_.wait(lock,
                        [this, &stop_token] { return released_ || stop_token.stop_requested(); });
        if (stop_token.stop_requested()) {
            saw_stop_ = true;
            return std::unexpected(MetadataParseError{
                .code = MetadataParseErrorCode::Cancelled,
                .message = "cancelled",
            });
        }
        return domain::EncounterMetadata{.boss_id = 888, .pov_account = ":Player.1234"};
    }

    [[nodiscard]] bool wait_until_started(std::chrono::milliseconds timeout) {
        std::unique_lock lock{mutex_};
        return condition_.wait_for(lock, timeout, [this] { return started_; });
    }

    void release() {
        {
            const std::scoped_lock lock{mutex_};
            released_ = true;
        }
        condition_.notify_all();
    }

    [[nodiscard]] bool saw_stop() const {
        const std::scoped_lock lock{mutex_};
        return saw_stop_;
    }

  private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    bool started_{};
    bool released_{};
    bool saw_stop_{};
};

void creation_and_success_tests(TestSuite& suite) {
    FakeReader reader;
    const auto invalid = MetadataParserWorker::create(reader, 0);
    MANNY_CHECK(suite, !invalid.has_value());
    MANNY_CHECK(suite, invalid.error().code == MetadataParserWorkerErrorCode::InvalidCapacity);

    auto created = MetadataParserWorker::create(reader, 2);
    MANNY_CHECK(suite, created.has_value());
    auto worker = std::move(created.value());
    MANNY_CHECK(suite, !worker->is_stopping());
    MANNY_CHECK(suite, !worker->try_take_result().has_value());

    const auto queued = worker->enqueue(request(17, "success.zevtc"));
    MANNY_CHECK(suite, queued.has_value());
    const auto result = worker->wait_for_result(2s);
    MANNY_CHECK(suite, result.has_value());
    MANNY_CHECK(suite, result->job_id == domain::UploadJobId{17});
    MANNY_CHECK(suite, result->metadata.has_value());
    MANNY_CHECK(suite, result->metadata->boss_id == 777);
    MANNY_CHECK(suite, result->metadata->pov_account == ":Broadcaster.1234");
    MANNY_CHECK(suite, reader.parse_count.load() == 1);
    MANNY_CHECK(suite, reader.captured_path() == std::filesystem::path{"logs/success.zevtc"});
    MANNY_CHECK(suite, !worker->try_take_result().has_value());
}

void failure_boundary_tests(TestSuite& suite) {
    FakeReader failing{FakeReader::Behavior::Fail};
    auto failed_worker = MetadataParserWorker::create(failing, 1);
    MANNY_CHECK(suite, failed_worker.has_value());
    MANNY_CHECK(suite, failed_worker.value()->enqueue(request(21, "bad.zevtc")).has_value());
    const auto failed = failed_worker.value()->wait_for_result(2s);
    MANNY_CHECK(suite, failed.has_value());
    MANNY_CHECK(suite, failed->job_id == domain::UploadJobId{21});
    MANNY_CHECK(suite, !failed->metadata.has_value());
    MANNY_CHECK(suite, failed->metadata.error().code == MetadataParseErrorCode::MalformedLog);
    MANNY_CHECK(suite, failed->metadata.error().message == "malformed fixture");

    FakeReader throwing{FakeReader::Behavior::Throw};
    auto throwing_worker = MetadataParserWorker::create(throwing, 1);
    MANNY_CHECK(suite, throwing_worker.has_value());
    MANNY_CHECK(suite, throwing_worker.value()->enqueue(request(22, "throw.zevtc")).has_value());
    const auto thrown = throwing_worker.value()->wait_for_result(2s);
    MANNY_CHECK(suite, thrown.has_value());
    MANNY_CHECK(suite, thrown->job_id == domain::UploadJobId{22});
    MANNY_CHECK(suite, !thrown->metadata.has_value());
    MANNY_CHECK(suite, thrown->metadata.error().code == MetadataParseErrorCode::Internal);
    MANNY_CHECK(suite,
                thrown->metadata.error().message.find("reader exploded") != std::string::npos);
}

void bounded_queue_tests(TestSuite& suite) {
    BlockingReader reader;
    auto created = MetadataParserWorker::create(reader, 1);
    MANNY_CHECK(suite, created.has_value());
    auto worker = std::move(created.value());

    MANNY_CHECK(suite, worker->enqueue(request(31, "active.zevtc")).has_value());
    MANNY_CHECK(suite, reader.wait_until_started(2s));
    MANNY_CHECK(suite, worker->enqueue(request(32, "pending.zevtc")).has_value());
    MANNY_CHECK(suite, worker->pending_count() == 1);
    const auto full = worker->enqueue(request(33, "rejected.zevtc"));
    MANNY_CHECK(suite, !full.has_value());
    MANNY_CHECK(suite, full.error().message.find("full") != std::string::npos);

    reader.release();
    const auto first = worker->wait_for_result(2s);
    const auto second = worker->wait_for_result(2s);
    MANNY_CHECK(suite, first.has_value());
    MANNY_CHECK(suite, second.has_value());
    MANNY_CHECK(suite, first->job_id == domain::UploadJobId{31});
    MANNY_CHECK(suite, second->job_id == domain::UploadJobId{32});
    MANNY_CHECK(suite, worker->pending_count() == 0);
    MANNY_CHECK(suite, worker->result_count() == 0);
}

void cancellation_and_shutdown_tests(TestSuite& suite) {
    BlockingReader reader;
    auto created = MetadataParserWorker::create(reader, 2);
    MANNY_CHECK(suite, created.has_value());
    auto worker = std::move(created.value());
    MANNY_CHECK(suite, worker->enqueue(request(41, "active.zevtc")).has_value());
    MANNY_CHECK(suite, reader.wait_until_started(2s));
    MANNY_CHECK(suite, worker->enqueue(request(42, "pending.zevtc")).has_value());

    worker->cancel_pending();
    worker->cancel_pending();
    MANNY_CHECK(suite, worker->is_stopping());
    MANNY_CHECK(suite, worker->pending_count() == 0);
    MANNY_CHECK(suite, worker->result_count() == 0);
    MANNY_CHECK(suite, !worker->enqueue(request(43, "late.zevtc")).has_value());
    MANNY_CHECK(suite, !worker->wait_for_result(1ms).has_value());

    worker.reset();
    MANNY_CHECK(suite, reader.saw_stop());
}

} // namespace

void run_metadata_parser_worker_tests(TestSuite& suite) {
    creation_and_success_tests(suite);
    failure_boundary_tests(suite);
    bounded_queue_tests(suite);
    cancellation_and_shutdown_tests(suite);
}

} // namespace manny_uploader::test
