#include "manny_uploader/evtc/metadata_parser_worker.hpp"

#include <exception>
#include <new>
#include <string>
#include <utility>

namespace manny_uploader::evtc {
namespace {

[[nodiscard]] MetadataParserWorkerError make_worker_error(MetadataParserWorkerErrorCode code,
                                                          std::string message) {
    return MetadataParserWorkerError{.code = code, .message = std::move(message)};
}

[[nodiscard]] ports::MetadataParseError internal_error(std::string message) {
    return ports::MetadataParseError{
        .code = ports::MetadataParseErrorCode::Internal,
        .message = std::move(message),
    };
}

} // namespace

std::expected<std::unique_ptr<MetadataParserWorker>, MetadataParserWorkerError>
MetadataParserWorker::create(ports::ILogMetadataReader& reader, std::size_t queue_capacity) {
    if (queue_capacity == 0) {
        return std::unexpected(
            make_worker_error(MetadataParserWorkerErrorCode::InvalidCapacity,
                              "Metadata parser queue capacity must be non-zero"));
    }

    try {
        auto worker =
            std::unique_ptr<MetadataParserWorker>{new MetadataParserWorker{reader, queue_capacity}};
        worker->thread_ = std::jthread{
            [instance = worker.get()](std::stop_token token) { instance->run(std::move(token)); }};
        return worker;
    } catch (const std::exception& error) {
        return std::unexpected(
            make_worker_error(MetadataParserWorkerErrorCode::ThreadStartFailed,
                              std::string{"Unable to start metadata parser: "} + error.what()));
    } catch (...) {
        return std::unexpected(make_worker_error(MetadataParserWorkerErrorCode::ThreadStartFailed,
                                                 "Unable to start metadata parser"));
    }
}

MetadataParserWorker::MetadataParserWorker(ports::ILogMetadataReader& reader,
                                           std::size_t queue_capacity)
    : reader_(reader), queue_capacity_(queue_capacity) {}

MetadataParserWorker::~MetadataParserWorker() {
    cancel_pending();
}

std::expected<void, ports::MetadataParseDispatchError>
MetadataParserWorker::enqueue(ports::MetadataParseRequest request) {
    std::unique_lock lock{mutex_};
    if (stopping_) {
        return std::unexpected(
            ports::MetadataParseDispatchError{.message = "Metadata parser is stopping"});
    }
    if (requests_.size() >= queue_capacity_) {
        return std::unexpected(
            ports::MetadataParseDispatchError{.message = "Metadata parser queue is full"});
    }

    try {
        requests_.push_back(std::move(request));
    } catch (const std::exception&) {
        return std::unexpected(
            ports::MetadataParseDispatchError{.message = "Unable to queue metadata parse"});
    }
    lock.unlock();
    condition_.notify_all();
    return {};
}

void MetadataParserWorker::cancel_pending() noexcept {
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

std::optional<ports::MetadataParseResult> MetadataParserWorker::try_take_result() {
    std::unique_lock lock{mutex_};
    auto result = take_result_locked();
    lock.unlock();
    if (result.has_value()) {
        condition_.notify_all();
    }
    return result;
}

std::optional<ports::MetadataParseResult>
MetadataParserWorker::wait_for_result(std::chrono::milliseconds timeout) {
    std::unique_lock lock{mutex_};
    condition_.wait_for(lock, timeout, [this] { return stopping_ || !results_.empty(); });
    auto result = take_result_locked();
    lock.unlock();
    if (result.has_value()) {
        condition_.notify_all();
    }
    return result;
}

std::expected<void, MetadataParserWorkerError>
MetadataParserWorker::update_queue_capacity(std::size_t queue_capacity) {
    if (queue_capacity == 0) {
        return std::unexpected(
            make_worker_error(MetadataParserWorkerErrorCode::InvalidCapacity,
                              "Metadata parser queue capacity must be non-zero"));
    }

    {
        const std::scoped_lock lock{mutex_};
        if (stopping_) {
            return std::unexpected(make_worker_error(MetadataParserWorkerErrorCode::Stopping,
                                                     "Metadata parser is stopping"));
        }
        queue_capacity_ = queue_capacity;
    }
    condition_.notify_all();
    return {};
}

std::size_t MetadataParserWorker::pending_count() const noexcept {
    const std::scoped_lock lock{mutex_};
    return requests_.size();
}

std::size_t MetadataParserWorker::result_count() const noexcept {
    const std::scoped_lock lock{mutex_};
    return results_.size();
}

std::size_t MetadataParserWorker::queue_capacity() const noexcept {
    const std::scoped_lock lock{mutex_};
    return queue_capacity_;
}

bool MetadataParserWorker::is_stopping() const noexcept {
    const std::scoped_lock lock{mutex_};
    return stopping_;
}

void MetadataParserWorker::run(std::stop_token stop_token) {
    while (!stop_token.stop_requested()) {
        ports::MetadataParseRequest request;
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

        std::expected<domain::EncounterMetadata, ports::MetadataParseError> metadata =
            std::unexpected(internal_error("Metadata reader did not return a result"));
        try {
            metadata = reader_.parse(request.file, stop_token);
        } catch (const std::exception& error) {
            metadata = std::unexpected(internal_error(
                std::string{"Metadata reader failed unexpectedly: "} + error.what()));
        } catch (...) {
            metadata = std::unexpected(internal_error("Metadata reader failed unexpectedly"));
        }

        std::unique_lock lock{mutex_};
        condition_.wait(lock, [this, &stop_token] {
            return stop_token.stop_requested() || results_.size() < queue_capacity_;
        });
        if (stop_token.stop_requested()) {
            return;
        }
        try {
            results_.push_back(ports::MetadataParseResult{
                .job_id = request.job_id,
                .metadata = std::move(metadata),
            });
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

std::optional<ports::MetadataParseResult> MetadataParserWorker::take_result_locked() {
    if (results_.empty()) {
        return std::nullopt;
    }
    auto result = std::move(results_.front());
    results_.pop_front();
    return result;
}

} // namespace manny_uploader::evtc
