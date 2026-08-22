#pragma once

#include "manny_uploader/ports/log_metadata_parser.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace manny_uploader::evtc {

enum class MetadataParserWorkerErrorCode : std::uint8_t {
    InvalidCapacity,
    ThreadStartFailed,
    Stopping,
};

struct MetadataParserWorkerError {
    MetadataParserWorkerErrorCode code;
    std::string message;
};

class MetadataParserWorker final : public ports::ILogMetadataParser {
  public:
    [[nodiscard]] static std::expected<std::unique_ptr<MetadataParserWorker>,
                                       MetadataParserWorkerError>
    create(ports::ILogMetadataReader& reader, std::size_t queue_capacity = 8);

    ~MetadataParserWorker() override;

    MetadataParserWorker(const MetadataParserWorker&) = delete;
    MetadataParserWorker& operator=(const MetadataParserWorker&) = delete;

    [[nodiscard]] std::expected<void, ports::MetadataParseDispatchError>
    enqueue(ports::MetadataParseRequest request) override;
    void cancel_pending() noexcept override;

    [[nodiscard]] std::optional<ports::MetadataParseResult> try_take_result() override;
    [[nodiscard]] std::optional<ports::MetadataParseResult>
    wait_for_result(std::chrono::milliseconds timeout);

    [[nodiscard]] std::expected<void, MetadataParserWorkerError>
    update_queue_capacity(std::size_t queue_capacity);

    [[nodiscard]] std::size_t pending_count() const noexcept;
    [[nodiscard]] std::size_t result_count() const noexcept;
    [[nodiscard]] std::size_t queue_capacity() const noexcept;
    [[nodiscard]] bool is_stopping() const noexcept;

  private:
    MetadataParserWorker(ports::ILogMetadataReader& reader, std::size_t queue_capacity);

    void run(std::stop_token stop_token);
    [[nodiscard]] std::optional<ports::MetadataParseResult> take_result_locked();

    ports::ILogMetadataReader& reader_;
    std::size_t queue_capacity_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<ports::MetadataParseRequest> requests_;
    std::deque<ports::MetadataParseResult> results_;
    bool stopping_{};
    std::jthread thread_;
};

} // namespace manny_uploader::evtc
