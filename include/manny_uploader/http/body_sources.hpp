#pragma once

#include "manny_uploader/ports/http_client.hpp"
#include "manny_uploader/support/secret_value.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <vector>

namespace manny_uploader::http {

[[nodiscard]] std::expected<std::unique_ptr<ports::IHttpBodySource>, ports::HttpBodyReadError>
make_file_http_body_source(std::filesystem::path path, std::uintmax_t expected_size,
                           std::filesystem::file_time_type expected_last_write_time);

[[nodiscard]] std::unique_ptr<ports::IHttpBodySource>
make_memory_http_body_source(std::vector<std::byte> bytes);

[[nodiscard]] std::expected<std::unique_ptr<ports::IHttpBodySource>, ports::HttpBodyReadError>
make_secret_http_body_source(const support::SecretValue& value);

} // namespace manny_uploader::http
