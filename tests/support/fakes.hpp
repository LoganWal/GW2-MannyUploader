#pragma once

#include "manny_uploader/ports/clock.hpp"
#include "manny_uploader/ports/log_metadata_parser.hpp"
#include "manny_uploader/ports/upload_provider.hpp"

#include <chrono>
#include <deque>
#include <expected>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace manny_uploader::test {

class FakeClock final : public ports::IClock {
  public:
    [[nodiscard]] std::chrono::system_clock::time_point system_now() const noexcept override {
        return system_now_;
    }

    [[nodiscard]] std::chrono::steady_clock::time_point steady_now() const noexcept override {
        return steady_now_;
    }

    void advance(std::chrono::seconds duration) noexcept {
        system_now_ += duration;
        steady_now_ += duration;
    }

    std::chrono::system_clock::time_point system_now_{};
    std::chrono::steady_clock::time_point steady_now_{};
};

class FakeUploadProvider final : public ports::IUploadProvider {
  public:
    explicit FakeUploadProvider(domain::Provider provider) : provider_(provider) {}

    [[nodiscard]] domain::Provider provider() const noexcept override {
        return provider_;
    }

    [[nodiscard]] std::expected<void, ports::DispatchError>
    enqueue(ports::UploadRequest request) override {
        if (reject_next_) {
            reject_next_ = false;
            return std::unexpected(ports::DispatchError{.message = rejection_message_});
        }

        requests.push_back(std::move(request));
        return {};
    }

    void cancel_pending() noexcept override {
        ++cancel_count;
    }

    [[nodiscard]] std::optional<ports::UploadResult> try_take_result() override {
        if (results.empty()) {
            return std::nullopt;
        }
        auto result = std::move(results.front());
        results.pop_front();
        return result;
    }

    void reject_next(std::string message) {
        reject_next_ = true;
        rejection_message_ = std::move(message);
    }

    std::vector<ports::UploadRequest> requests;
    std::deque<ports::UploadResult> results;
    std::size_t cancel_count{};

  private:
    domain::Provider provider_;
    bool reject_next_{};
    std::string rejection_message_;
};

class FakeMetadataParser final : public ports::ILogMetadataParser {
  public:
    [[nodiscard]] std::expected<void, ports::MetadataParseDispatchError>
    enqueue(ports::MetadataParseRequest request) override {
        if (reject_next_) {
            reject_next_ = false;
            return std::unexpected(
                ports::MetadataParseDispatchError{.message = rejection_message_});
        }

        requests.push_back(std::move(request));
        return {};
    }

    void cancel_pending() noexcept override {
        ++cancel_count;
    }

    [[nodiscard]] std::optional<ports::MetadataParseResult> try_take_result() override {
        if (results.empty()) {
            return std::nullopt;
        }
        auto result = std::move(results.front());
        results.pop_front();
        return result;
    }

    void reject_next(std::string message) {
        reject_next_ = true;
        rejection_message_ = std::move(message);
    }

    std::vector<ports::MetadataParseRequest> requests;
    std::deque<ports::MetadataParseResult> results;
    std::size_t cancel_count{};

  private:
    bool reject_next_{};
    std::string rejection_message_;
};

} // namespace manny_uploader::test
