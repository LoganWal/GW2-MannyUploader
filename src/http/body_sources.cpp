#include "manny_uploader/http/body_sources.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
#include <span>
#include <stop_token>
#include <system_error>
#include <utility>

namespace manny_uploader::http {
namespace {

[[nodiscard]] ports::HttpBodyReadError
make_error(ports::HttpBodyReadErrorCode code, std::string message,
           std::optional<std::uint32_t> system_error = std::nullopt) {
    return ports::HttpBodyReadError{
        .code = code,
        .message = std::move(message),
        .system_error = system_error,
    };
}

[[nodiscard]] std::optional<std::uint32_t> system_code(const std::error_code& error) noexcept {
    if (!error) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(error.value());
}

[[nodiscard]] std::expected<void, ports::HttpBodyReadError>
verify_file_identity(const std::filesystem::path& path, std::uintmax_t expected_size,
                     std::filesystem::file_time_type expected_last_write_time) {
    std::error_code error;
    const auto status = std::filesystem::status(path, error);
    if (error || !std::filesystem::is_regular_file(status)) {
        return std::unexpected(make_error(ports::HttpBodyReadErrorCode::SourceUnavailable,
                                          "The HTTP body file is unavailable", system_code(error)));
    }

    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        return std::unexpected(make_error(ports::HttpBodyReadErrorCode::SourceUnavailable,
                                          "The HTTP body file size is unavailable",
                                          system_code(error)));
    }
    const auto last_write_time = std::filesystem::last_write_time(path, error);
    if (error) {
        return std::unexpected(make_error(ports::HttpBodyReadErrorCode::SourceUnavailable,
                                          "The HTTP body file timestamp is unavailable",
                                          system_code(error)));
    }
    if (size != expected_size || last_write_time != expected_last_write_time) {
        return std::unexpected(make_error(ports::HttpBodyReadErrorCode::SourceChanged,
                                          "The HTTP body file identity changed"));
    }
    return {};
}

class FileBodySource final : public ports::IHttpBodySource {
  public:
    FileBodySource(std::filesystem::path path, std::uint64_t expected_size,
                   std::filesystem::file_time_type expected_last_write_time, std::ifstream stream)
        : path_{std::move(path)}, expected_size_{expected_size},
          expected_last_write_time_{expected_last_write_time}, stream_{std::move(stream)} {}

    [[nodiscard]] std::uint64_t content_length() const noexcept override {
        return expected_size_;
    }

    [[nodiscard]] std::expected<std::size_t, ports::HttpBodyReadError>
    read(std::span<std::byte> destination, const std::stop_token& stop_token) override {
        if (stop_token.stop_requested()) {
            return std::unexpected(make_error(ports::HttpBodyReadErrorCode::Cancelled,
                                              "The HTTP body read was cancelled"));
        }
        if (offset_ == expected_size_ || destination.empty()) {
            return 0;
        }

        const auto remaining = expected_size_ - offset_;
        const auto count = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, static_cast<std::uint64_t>(destination.size())));
        if (count > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
            return std::unexpected(make_error(ports::HttpBodyReadErrorCode::ReadFailed,
                                              "The HTTP body read size is unsupported"));
        }

        stream_.read(reinterpret_cast<char*>(destination.data()),
                     static_cast<std::streamsize>(count));
        if (stream_.gcount() != static_cast<std::streamsize>(count)) {
            return std::unexpected(
                make_error(stream_.eof() ? ports::HttpBodyReadErrorCode::SourceChanged
                                         : ports::HttpBodyReadErrorCode::ReadFailed,
                           stream_.eof() ? "The HTTP body file ended before its declared length"
                                         : "The HTTP body file could not be read"));
        }

        offset_ += static_cast<std::uint64_t>(count);
        if (offset_ == expected_size_) {
            auto unchanged = verify_file_identity(path_, expected_size_, expected_last_write_time_);
            if (!unchanged) {
                return std::unexpected(std::move(unchanged.error()));
            }
        }
        return count;
    }

  private:
    std::filesystem::path path_;
    std::uint64_t expected_size_;
    std::filesystem::file_time_type expected_last_write_time_;
    std::ifstream stream_;
    std::uint64_t offset_{};
};

class MemoryBodySource final : public ports::IHttpBodySource {
  public:
    explicit MemoryBodySource(std::vector<std::byte> bytes) : bytes_{std::move(bytes)} {}

    [[nodiscard]] std::uint64_t content_length() const noexcept override {
        return bytes_.size();
    }

    [[nodiscard]] std::expected<std::size_t, ports::HttpBodyReadError>
    read(std::span<std::byte> destination, const std::stop_token& stop_token) override {
        if (stop_token.stop_requested()) {
            return std::unexpected(make_error(ports::HttpBodyReadErrorCode::Cancelled,
                                              "The HTTP body read was cancelled"));
        }
        const auto count = std::min(destination.size(), bytes_.size() - offset_);
        std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(offset_),
                    static_cast<std::ptrdiff_t>(count), destination.begin());
        offset_ += count;
        return count;
    }

  private:
    std::vector<std::byte> bytes_;
    std::size_t offset_{};
};

class SecretBodySource final : public ports::IHttpBodySource {
  public:
    explicit SecretBodySource(support::SecretValue value) : value_{std::move(value)} {}

    [[nodiscard]] std::uint64_t content_length() const noexcept override {
        return value_.size();
    }

    [[nodiscard]] std::expected<std::size_t, ports::HttpBodyReadError>
    read(std::span<std::byte> destination, const std::stop_token& stop_token) override {
        if (stop_token.stop_requested()) {
            return std::unexpected(make_error(ports::HttpBodyReadErrorCode::Cancelled,
                                              "The HTTP body read was cancelled"));
        }
        const auto bytes = value_.bytes();
        const auto count = std::min(destination.size(), bytes.size() - offset_);
        std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset_),
                    static_cast<std::ptrdiff_t>(count), destination.begin());
        offset_ += count;
        return count;
    }

  private:
    support::SecretValue value_;
    std::size_t offset_{};
};

} // namespace

std::expected<std::unique_ptr<ports::IHttpBodySource>, ports::HttpBodyReadError>
make_file_http_body_source(std::filesystem::path path, std::uintmax_t expected_size,
                           std::filesystem::file_time_type expected_last_write_time) {
    if (expected_size > std::numeric_limits<std::uint64_t>::max()) {
        return std::unexpected(make_error(ports::HttpBodyReadErrorCode::ReadFailed,
                                          "The HTTP body file exceeds the supported size"));
    }
    auto unchanged = verify_file_identity(path, expected_size, expected_last_write_time);
    if (!unchanged) {
        return std::unexpected(std::move(unchanged.error()));
    }

    std::ifstream stream{path, std::ios::binary};
    if (!stream.is_open()) {
        return std::unexpected(make_error(ports::HttpBodyReadErrorCode::SourceUnavailable,
                                          "The HTTP body file could not be opened"));
    }
    return std::make_unique<FileBodySource>(std::move(path),
                                            static_cast<std::uint64_t>(expected_size),
                                            expected_last_write_time, std::move(stream));
}

std::unique_ptr<ports::IHttpBodySource> make_memory_http_body_source(std::vector<std::byte> bytes) {
    return std::make_unique<MemoryBodySource>(std::move(bytes));
}

std::expected<std::unique_ptr<ports::IHttpBodySource>, ports::HttpBodyReadError>
make_secret_http_body_source(const support::SecretValue& value) {
    try {
        auto copy = std::vector<std::byte>{value.bytes().begin(), value.bytes().end()};
        return std::make_unique<SecretBodySource>(support::SecretValue{std::move(copy)});
    } catch (...) {
        return std::unexpected(make_error(ports::HttpBodyReadErrorCode::ReadFailed,
                                          "The protected HTTP body could not be prepared"));
    }
}

} // namespace manny_uploader::http
