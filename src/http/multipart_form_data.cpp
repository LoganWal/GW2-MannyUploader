#include "manny_uploader/http/multipart_form_data.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <random>
#include <span>
#include <stop_token>
#include <string_view>
#include <utility>

namespace manny_uploader::http {
namespace {

[[nodiscard]] MultipartError make_error(MultipartErrorCode code, std::string message) {
    return MultipartError{.code = code, .message = std::move(message)};
}

[[nodiscard]] bool is_token_character(char value) noexcept {
    if ((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
        (value >= '0' && value <= '9')) {
        return true;
    }
    constexpr std::string_view punctuation = "!#$%&'*+-.^_`|~";
    return punctuation.contains(value);
}

[[nodiscard]] bool valid_boundary(std::string_view value) noexcept {
    return !value.empty() && value.size() <= max_multipart_boundary_bytes &&
           std::ranges::all_of(value, is_token_character);
}

[[nodiscard]] bool valid_part_name(std::string_view value) noexcept {
    return !value.empty() && value.size() <= 64 && std::ranges::all_of(value, [](char character) {
        return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '_' || character == '-';
    });
}

[[nodiscard]] bool valid_filename(std::string_view value) noexcept {
    return !value.empty() && value.size() <= 255 && std::ranges::all_of(value, [](char character) {
        return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '_' || character == '-' ||
               character == '.';
    });
}

[[nodiscard]] bool valid_content_type(std::string_view value) noexcept {
    const auto slash = value.find('/');
    return slash != std::string_view::npos && slash != 0 && slash + 1 < value.size() &&
           value.find('/', slash + 1) == std::string_view::npos &&
           std::ranges::all_of(value.substr(0, slash), is_token_character) &&
           std::ranges::all_of(value.substr(slash + 1), is_token_character);
}

[[nodiscard]] std::expected<std::string, MultipartError> generate_boundary() {
    try {
        constexpr std::array hex{'0', '1', '2', '3', '4', '5', '6', '7',
                                 '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
        auto boundary = std::string{"------------------------manny-"};
        boundary.reserve(boundary.size() + 32);
        std::random_device random;
        for (std::size_t index = 0; index < 16; ++index) {
            const auto value = static_cast<unsigned int>(random());
            boundary.push_back(hex[(value >> 4U) & 0x0fU]);
            boundary.push_back(hex[value & 0x0fU]);
        }
        return boundary;
    } catch (...) {
        return std::unexpected(make_error(MultipartErrorCode::BoundaryGenerationFailed,
                                          "The multipart boundary could not be generated"));
    }
}

[[nodiscard]] std::vector<std::byte> as_bytes(std::string_view value) {
    const auto characters = std::span{value.data(), value.size()};
    const auto bytes = std::as_bytes(characters);
    return {bytes.begin(), bytes.end()};
}

struct PartState {
    std::vector<std::byte> prefix;
    std::unique_ptr<ports::IHttpBodySource> body;
    std::uint64_t body_bytes_read{};
};

class MultipartBodySource final : public ports::IHttpBodySource {
  public:
    MultipartBodySource(std::vector<PartState> parts, std::vector<std::byte> terminal,
                        std::uint64_t content_length)
        : parts_{std::move(parts)}, terminal_{std::move(terminal)},
          content_length_{content_length} {}

    [[nodiscard]] std::uint64_t content_length() const noexcept override {
        return content_length_;
    }

    [[nodiscard]] std::expected<std::size_t, ports::HttpBodyReadError>
    read(std::span<std::byte> destination, const std::stop_token& stop_token) override {
        try {
            if (stop_token.stop_requested()) {
                return std::unexpected(ports::HttpBodyReadError{
                    .code = ports::HttpBodyReadErrorCode::Cancelled,
                    .message = "The multipart body read was cancelled",
                    .system_error = std::nullopt,
                });
            }
            if (destination.empty()) {
                return 0;
            }

            while (part_index_ < parts_.size()) {
                auto& part = parts_[part_index_];
                if (prefix_offset_ < part.prefix.size()) {
                    const auto count =
                        std::min(destination.size(), part.prefix.size() - prefix_offset_);
                    std::copy_n(part.prefix.begin() + static_cast<std::ptrdiff_t>(prefix_offset_),
                                static_cast<std::ptrdiff_t>(count), destination.begin());
                    prefix_offset_ += count;
                    return count;
                }

                const auto body_length = part.body->content_length();
                if (part.body_bytes_read < body_length) {
                    const auto remaining = body_length - part.body_bytes_read;
                    const auto capacity = static_cast<std::size_t>(std::min<std::uint64_t>(
                        remaining, static_cast<std::uint64_t>(destination.size())));
                    auto result = part.body->read(destination.first(capacity), stop_token);
                    if (!result) {
                        return std::unexpected(std::move(result.error()));
                    }
                    if (*result == 0 || *result > capacity) {
                        return std::unexpected(ports::HttpBodyReadError{
                            .code = ports::HttpBodyReadErrorCode::ReadFailed,
                            .message = "A multipart part returned an invalid byte count",
                            .system_error = std::nullopt,
                        });
                    }
                    part.body_bytes_read += static_cast<std::uint64_t>(*result);
                    return *result;
                }

                ++part_index_;
                prefix_offset_ = 0;
            }

            if (terminal_offset_ < terminal_.size()) {
                const auto count =
                    std::min(destination.size(), terminal_.size() - terminal_offset_);
                std::copy_n(terminal_.begin() + static_cast<std::ptrdiff_t>(terminal_offset_),
                            static_cast<std::ptrdiff_t>(count), destination.begin());
                terminal_offset_ += count;
                return count;
            }
            return 0;
        } catch (...) {
            return std::unexpected(ports::HttpBodyReadError{
                .code = ports::HttpBodyReadErrorCode::ReadFailed,
                .message = "The multipart body failed unexpectedly",
                .system_error = std::nullopt,
            });
        }
    }

  private:
    std::vector<PartState> parts_;
    std::vector<std::byte> terminal_;
    std::uint64_t content_length_;
    std::size_t part_index_{};
    std::size_t prefix_offset_{};
    std::size_t terminal_offset_{};
};

[[nodiscard]] std::expected<std::string, MultipartError>
part_prefix(const MultipartPart& part, std::string_view boundary, bool first) {
    if (!valid_part_name(part.name)) {
        return std::unexpected(
            make_error(MultipartErrorCode::InvalidPartName, "Multipart part name is invalid"));
    }
    if (part.filename && !valid_filename(*part.filename)) {
        return std::unexpected(
            make_error(MultipartErrorCode::InvalidFilename, "Multipart filename is invalid"));
    }
    if (part.content_type && !valid_content_type(*part.content_type)) {
        return std::unexpected(make_error(MultipartErrorCode::InvalidContentType,
                                          "Multipart content type is invalid"));
    }
    if (!part.body) {
        return std::unexpected(
            make_error(MultipartErrorCode::MissingBody, "Multipart part body is missing"));
    }

    auto prefix = std::string{first ? "--" : "\r\n--"};
    prefix += boundary;
    prefix += "\r\nContent-Disposition: form-data; name=\"";
    prefix += part.name;
    prefix += '"';
    if (part.filename) {
        prefix += "; filename=\"";
        prefix += *part.filename;
        prefix += '"';
    }
    prefix += "\r\n";
    if (part.content_type) {
        prefix += "Content-Type: ";
        prefix += *part.content_type;
        prefix += "\r\n";
    }
    prefix += "\r\n";
    return prefix;
}

[[nodiscard]] bool add_without_overflow(std::uint64_t& total, std::uint64_t value) noexcept {
    if (value > ports::max_http_request_body_bytes - total) {
        return false;
    }
    total += value;
    return true;
}

} // namespace

std::expected<MultipartFormData, MultipartError>
make_multipart_form_data(std::vector<MultipartPart> parts, std::optional<std::string> boundary) {
    try {
        if (parts.empty()) {
            return std::unexpected(
                make_error(MultipartErrorCode::EmptyParts, "Multipart body requires a part"));
        }
        if (parts.size() > max_multipart_part_count) {
            return std::unexpected(
                make_error(MultipartErrorCode::TooManyParts, "Multipart body has too many parts"));
        }
        if (!boundary) {
            auto generated = generate_boundary();
            if (!generated) {
                return std::unexpected(std::move(generated.error()));
            }
            boundary = std::move(*generated);
        }
        if (!valid_boundary(*boundary)) {
            return std::unexpected(
                make_error(MultipartErrorCode::InvalidBoundary, "Multipart boundary is invalid"));
        }

        std::vector<PartState> states;
        states.reserve(parts.size());
        std::uint64_t content_length{};
        for (std::size_t index = 0; index < parts.size(); ++index) {
            auto prefix = part_prefix(parts[index], *boundary, index == 0);
            if (!prefix) {
                return std::unexpected(std::move(prefix.error()));
            }
            auto prefix_bytes = as_bytes(*prefix);
            const auto body_length = parts[index].body->content_length();
            if (!add_without_overflow(content_length, prefix_bytes.size()) ||
                !add_without_overflow(content_length, body_length)) {
                return std::unexpected(make_error(MultipartErrorCode::BodyTooLarge,
                                                  "Multipart body exceeds the HTTP limit"));
            }
            states.push_back(PartState{
                .prefix = std::move(prefix_bytes),
                .body = std::move(parts[index].body),
                .body_bytes_read = 0,
            });
        }

        auto terminal = as_bytes("\r\n--" + *boundary + "--\r\n");
        if (!add_without_overflow(content_length, terminal.size())) {
            return std::unexpected(make_error(MultipartErrorCode::BodyTooLarge,
                                              "Multipart body exceeds the HTTP limit"));
        }

        return MultipartFormData{
            .content_type = "multipart/form-data; boundary=" + *boundary,
            .body = std::make_unique<MultipartBodySource>(std::move(states), std::move(terminal),
                                                          content_length),
        };
    } catch (...) {
        return std::unexpected(
            make_error(MultipartErrorCode::Internal, "The multipart body could not be prepared"));
    }
}

} // namespace manny_uploader::http
