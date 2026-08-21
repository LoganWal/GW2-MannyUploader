#pragma once

#include "manny_uploader/ports/http_client.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace manny_uploader::http {

inline constexpr std::size_t max_multipart_boundary_bytes = 70;
inline constexpr std::size_t max_multipart_part_count = 16;

struct MultipartPart {
    std::string name;
    std::optional<std::string> filename;
    std::optional<std::string> content_type;
    std::unique_ptr<ports::IHttpBodySource> body;
};

enum class MultipartErrorCode : std::uint8_t {
    EmptyParts,
    TooManyParts,
    InvalidBoundary,
    InvalidPartName,
    InvalidFilename,
    InvalidContentType,
    MissingBody,
    BodyTooLarge,
    BoundaryGenerationFailed,
    Internal,
};

struct MultipartError {
    MultipartErrorCode code;
    std::string message;
};

struct MultipartFormData {
    std::string content_type;
    std::unique_ptr<ports::IHttpBodySource> body;
};

[[nodiscard]] std::expected<MultipartFormData, MultipartError>
make_multipart_form_data(std::vector<MultipartPart> parts,
                         std::optional<std::string> boundary = std::nullopt);

} // namespace manny_uploader::http
