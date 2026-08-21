#pragma once

#include "manny_uploader/ports/http_client.hpp"

#include <expected>
#include <memory>

namespace manny_uploader::http {

[[nodiscard]] std::expected<std::unique_ptr<ports::IHttpClient>, ports::HttpError>
make_curl_http_client(ports::HttpTransportPolicy policy = {});

} // namespace manny_uploader::http
