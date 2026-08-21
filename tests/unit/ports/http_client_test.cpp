#include "manny_uploader/ports/http_client.hpp"

#include "support/test_suite.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace manny_uploader::test {
namespace {

using ports::HttpErrorCode;
using ports::HttpHeader;
using ports::HttpHeaderSensitivity;
using ports::HttpMethod;
using ports::HttpRequest;

class FixedLengthBody final : public ports::IHttpBodySource {
  public:
    explicit FixedLengthBody(std::uint64_t length) : length_{length} {}

    [[nodiscard]] std::uint64_t content_length() const noexcept override {
        return length_;
    }

    [[nodiscard]] std::expected<std::size_t, ports::HttpBodyReadError>
    read(std::span<std::byte>, const std::stop_token&) override {
        return 0;
    }

  private:
    std::uint64_t length_;
};

[[nodiscard]] HttpRequest valid_request() {
    HttpRequest request;
    request.method = HttpMethod::Post;
    request.url = "https://api.example.test/v1/upload?request=42";
    request.headers.push_back(HttpHeader{
        .name = "Content-Type",
        .value = "application/json",
        .sensitivity = HttpHeaderSensitivity::Public,
    });
    return request;
}

[[nodiscard]] bool invalid(const HttpRequest& request, ports::HttpTransportPolicy policy = {}) {
    const auto result = ports::validate_http_request(request, policy);
    return !result.has_value() && result.error().code == HttpErrorCode::InvalidRequest;
}

void method_and_redaction_tests(TestSuite& suite) {
    MANNY_CHECK(suite, ports::http_method_name(HttpMethod::Get) == "GET");
    MANNY_CHECK(suite, ports::http_method_name(HttpMethod::Post) == "POST");
    MANNY_CHECK(suite, ports::http_method_name(HttpMethod::Put) == "PUT");
    MANNY_CHECK(suite, ports::http_method_name(HttpMethod::Patch) == "PATCH");
    MANNY_CHECK(suite, ports::http_method_name(HttpMethod::Delete) == "DELETE");
    MANNY_CHECK(suite, ports::http_method_name(static_cast<HttpMethod>(255)).empty());

    const std::vector<std::string_view> sensitive_names{
        "Authorization", "PROXY-AUTHORIZATION", "cookie",
        "Set-Cookie",    "X-Api-Key",           "x-gw2-api-key",
    };
    for (const auto name : sensitive_names) {
        MANNY_CHECK(suite, ports::is_sensitive_http_header_name(name));
    }
    MANNY_CHECK(suite, !ports::is_sensitive_http_header_name("Content-Type"));

    const auto public_header = HttpHeader{
        .name = "X-Request-Id",
        .value = "public-value",
        .sensitivity = HttpHeaderSensitivity::Public,
    };
    const auto explicitly_sensitive = HttpHeader{
        .name = "X-Custom-Token",
        .value = "custom-secret-marker",
        .sensitivity = HttpHeaderSensitivity::Sensitive,
    };
    const auto known_sensitive = HttpHeader{
        .name = "Authorization",
        .value = "known-secret-marker",
        .sensitivity = HttpHeaderSensitivity::Public,
    };
    MANNY_CHECK(suite, ports::redacted_http_header_value(public_header) == "public-value");
    MANNY_CHECK(suite, ports::redacted_http_header_value(explicitly_sensitive) == "[REDACTED]");
    MANNY_CHECK(suite, ports::redacted_http_header_value(known_sensitive) == "[REDACTED]");
}

void url_tests(TestSuite& suite) {
    auto request = valid_request();
    MANNY_CHECK(suite, ports::validate_http_request(request).has_value());

    const std::vector<std::string> invalid_urls{
        "",
        "api.example.test/upload",
        "ftp://api.example.test/upload",
        "http://api.example.test/upload",
        "https://",
        "https://user:password@api.example.test/upload",
        "https://api.example.test/upload#fragment",
        "https://api.example.test/has a space",
        "https://api.example.test/line\nbreak",
    };
    for (const auto& url : invalid_urls) {
        request.url = url;
        MANNY_CHECK(suite, invalid(request));
    }
    request.url.assign(ports::max_http_url_bytes + 1, 'a');
    MANNY_CHECK(suite, invalid(request));

    const auto test_policy = ports::HttpTransportPolicy{.allow_plaintext_loopback_for_tests = true};
    const std::vector<std::string> loopback_urls{
        "http://localhost/test",
        "http://LOCALHOST:8080/test",
        "http://127.0.0.1:1/test",
        "http://[::1]:65535/test",
    };
    for (const auto& url : loopback_urls) {
        request.url = url;
        MANNY_CHECK(suite, ports::validate_http_request(request, test_policy).has_value());
        MANNY_CHECK(suite, invalid(request));
    }

    const std::vector<std::string> invalid_loopback_urls{
        "http://localhost.evil.test/", "http://127.0.0.2/",       "http://[::2]/",
        "http://localhost:0/",         "http://localhost:65536/", "http://localhost:not-a-port/",
    };
    for (const auto& url : invalid_loopback_urls) {
        request.url = url;
        MANNY_CHECK(suite, invalid(request, test_policy));
    }
}

void header_tests(TestSuite& suite) {
    auto request = valid_request();

    request.headers.push_back(HttpHeader{
        .name = "Authorization",
        .value = "Bearer marker",
        .sensitivity = HttpHeaderSensitivity::Sensitive,
    });
    MANNY_CHECK(suite, ports::validate_http_request(request).has_value());
    request.headers.back().sensitivity = HttpHeaderSensitivity::Public;
    MANNY_CHECK(suite, invalid(request));

    const std::vector<std::string> invalid_names{
        "", "Bad Header", "Header:", "Header\nInjected", std::string(129, 'A'),
    };
    for (const auto& name : invalid_names) {
        request = valid_request();
        request.headers.push_back(HttpHeader{.name = name, .value = "value"});
        MANNY_CHECK(suite, invalid(request));
    }

    const std::vector<std::string> invalid_values{
        "line\rbreak",
        "line\nbreak",
        std::string{"nul\0byte", 8},
        std::string(8193, 'x'),
    };
    for (const auto& value : invalid_values) {
        request = valid_request();
        request.headers.push_back(HttpHeader{.name = "X-Test", .value = value});
        MANNY_CHECK(suite, invalid(request));
    }

    const std::vector<std::string> adapter_owned_names{
        "Host", "content-length", "TRANSFER-ENCODING", "Connection", "Expect",
    };
    for (const auto& name : adapter_owned_names) {
        request = valid_request();
        request.headers.push_back(HttpHeader{.name = name, .value = "value"});
        MANNY_CHECK(suite, invalid(request));
    }

    request = valid_request();
    request.headers.resize(ports::max_http_header_count + 1,
                           HttpHeader{.name = "X-Test", .value = "value"});
    MANNY_CHECK(suite, invalid(request));

    request = valid_request();
    request.headers.clear();
    for (std::size_t index = 0; index < 5; ++index) {
        request.headers.push_back(HttpHeader{
            .name = "X-Large-Header-" + std::to_string(index),
            .value = std::string(ports::max_http_header_value_bytes, 'x'),
        });
    }
    MANNY_CHECK(suite, invalid(request));
}

void body_timeout_and_limit_tests(TestSuite& suite) {
    auto request = valid_request();
    request.body = std::make_unique<FixedLengthBody>(1);
    MANNY_CHECK(suite, ports::validate_http_request(request).has_value());

    request.method = HttpMethod::Get;
    MANNY_CHECK(suite, invalid(request));
    request.method = HttpMethod::Post;
    request.body = std::make_unique<FixedLengthBody>(0);
    MANNY_CHECK(suite, invalid(request));
    request.body = std::make_unique<FixedLengthBody>(ports::max_http_request_body_bytes + 1);
    MANNY_CHECK(suite, invalid(request));
    request.body.reset();

    request.timeouts.connect = std::chrono::milliseconds{0};
    MANNY_CHECK(suite, invalid(request));
    request.timeouts = {};
    request.timeouts.connect = std::chrono::seconds{61};
    MANNY_CHECK(suite, invalid(request));
    request.timeouts = {};
    request.timeouts.operation = std::chrono::minutes{16};
    MANNY_CHECK(suite, invalid(request));
    request.timeouts = {};
    request.timeouts.stalled_transfer = std::chrono::milliseconds{999};
    MANNY_CHECK(suite, invalid(request));
    request.timeouts = {};
    request.timeouts.stalled_transfer = std::chrono::minutes{16};
    MANNY_CHECK(suite, invalid(request));

    request.timeouts = {};
    request.timeouts.stalled_transfer = std::chrono::minutes{15};
    MANNY_CHECK(suite, ports::validate_http_request(request).has_value());

    request.timeouts = {};
    request.response_limits.max_header_bytes = 0;
    MANNY_CHECK(suite, invalid(request));
    request.response_limits = {};
    request.response_limits.max_header_bytes = ports::max_http_response_header_bytes + 1;
    MANNY_CHECK(suite, invalid(request));
    request.response_limits = {};
    request.response_limits.max_body_bytes = 0;
    MANNY_CHECK(suite, invalid(request));
    request.response_limits = {};
    request.response_limits.max_body_bytes = ports::max_http_response_body_bytes + 1;
    MANNY_CHECK(suite, invalid(request));

    request.response_limits = {};
    MANNY_CHECK(suite, ports::validate_http_request(request).has_value());
}

} // namespace

void run_http_client_tests(TestSuite& suite) {
    method_and_redaction_tests(suite);
    url_tests(suite);
    header_tests(suite);
    body_timeout_and_limit_tests(suite);
}

} // namespace manny_uploader::test
