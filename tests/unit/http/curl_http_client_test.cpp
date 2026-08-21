#include "manny_uploader/http/curl_http_client.hpp"

#include "support/test_suite.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>

#include <algorithm>
#include <charconv>
#include <limits>
#endif

namespace manny_uploader::test {
namespace {

using ports::HttpBodyReadError;
using ports::HttpErrorCode;
using ports::HttpMethod;
using ports::HttpRequest;

#ifdef _WIN32

[[nodiscard]] HttpRequest request_for(std::string url, HttpMethod method = HttpMethod::Get) {
    HttpRequest request;
    request.method = method;
    request.url = std::move(url);
    request.timeouts.connect = std::chrono::seconds{2};
    request.timeouts.operation = std::chrono::seconds{5};
    request.timeouts.stalled_transfer = std::chrono::seconds{1};
    return request;
}

template <typename Result> void report_http_failure(const Result& result, std::string_view label) {
    if (result) {
        return;
    }
    std::cerr << label << ": HTTP error=" << static_cast<int>(result.error().code)
              << " transport=" << result.error().transport_code.value_or(-1) << '\n';
}

class VectorBody final : public ports::IHttpBodySource {
  public:
    explicit VectorBody(std::string_view value) {
        bytes_.reserve(value.size());
        for (const auto character : value) {
            bytes_.push_back(static_cast<std::byte>(character));
        }
    }

    [[nodiscard]] std::uint64_t content_length() const noexcept override {
        return bytes_.size();
    }

    [[nodiscard]] std::expected<std::size_t, HttpBodyReadError>
    read(std::span<std::byte> destination, const std::stop_token&) override {
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

class EarlyEofBody final : public ports::IHttpBodySource {
  public:
    [[nodiscard]] std::uint64_t content_length() const noexcept override {
        return 4;
    }

    [[nodiscard]] std::expected<std::size_t, HttpBodyReadError>
    read(std::span<std::byte>, const std::stop_token&) override {
        return 0;
    }
};

class InvalidCountBody final : public ports::IHttpBodySource {
  public:
    [[nodiscard]] std::uint64_t content_length() const noexcept override {
        return 4;
    }

    [[nodiscard]] std::expected<std::size_t, HttpBodyReadError>
    read(std::span<std::byte> destination, const std::stop_token&) override {
        return destination.size() + 1;
    }
};

class ThrowingBody final : public ports::IHttpBodySource {
  public:
    [[nodiscard]] std::uint64_t content_length() const noexcept override {
        return 4;
    }

    [[nodiscard]] std::expected<std::size_t, HttpBodyReadError>
    read(std::span<std::byte>, const std::stop_token&) override {
        throw std::runtime_error{"private source detail"};
    }
};

class LargeStreamingBody final : public ports::IHttpBodySource {
  public:
    explicit LargeStreamingBody(std::atomic<std::size_t>& reads) : reads_{reads} {}

    [[nodiscard]] std::uint64_t content_length() const noexcept override {
        return ports::max_http_request_body_bytes;
    }

    [[nodiscard]] std::expected<std::size_t, HttpBodyReadError>
    read(std::span<std::byte> destination, const std::stop_token&) override {
        std::ranges::fill(destination, std::byte{0x5a});
        reads_.fetch_add(1, std::memory_order_release);
        return destination.size();
    }

  private:
    std::atomic<std::size_t>& reads_;
};

[[nodiscard]] bool initialize_winsock() noexcept {
    static const bool initialized = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return initialized;
}

enum class ServerBehavior : std::uint8_t {
    Reply,
    StallAfterHeaders,
};

class LoopbackServer final {
  public:
    explicit LoopbackServer(std::string response, ServerBehavior behavior = ServerBehavior::Reply)
        : response_{std::move(response)}, behavior_{behavior} {
        if (!initialize_winsock()) {
            return;
        }

        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener_ == INVALID_SOCKET) {
            return;
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (bind(listener_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
            listen(listener_, 1) != 0) {
            closesocket(listener_);
            listener_ = INVALID_SOCKET;
            return;
        }

        auto address_length = static_cast<int>(sizeof(address));
        if (getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &address_length) != 0) {
            closesocket(listener_);
            listener_ = INVALID_SOCKET;
            return;
        }
        port_ = ntohs(address.sin_port);
        thread_ = std::jthread{[this](const std::stop_token& stop_token) { serve(stop_token); }};
    }

    ~LoopbackServer() {
        thread_.request_stop();
        if (listener_ != INVALID_SOCKET) {
            const auto wake_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (wake_socket != INVALID_SOCKET) {
                sockaddr_in address{};
                address.sin_family = AF_INET;
                address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                address.sin_port = htons(port_);
                static_cast<void>(connect(wake_socket, reinterpret_cast<const sockaddr*>(&address),
                                          sizeof(address)));
                closesocket(wake_socket);
            }
            closesocket(listener_);
            listener_ = INVALID_SOCKET;
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    LoopbackServer(const LoopbackServer&) = delete;
    LoopbackServer& operator=(const LoopbackServer&) = delete;

    [[nodiscard]] bool valid() const noexcept {
        return port_ != 0;
    }

    [[nodiscard]] std::string url(std::string_view path = "/upload") const {
        return "http://127.0.0.1:" + std::to_string(port_) + std::string{path};
    }

    [[nodiscard]] std::string request() const {
        while (!completed_.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        return request_;
    }

  private:
    [[nodiscard]] static std::size_t content_length(std::string_view headers) noexcept {
        constexpr std::string_view name = "Content-Length:";
        const auto start = headers.find(name);
        if (start == std::string_view::npos) {
            return 0;
        }
        auto value = headers.substr(start + name.size());
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
            value.remove_prefix(1);
        }
        const auto end = value.find("\r\n");
        value = value.substr(0, end);
        std::size_t parsed{};
        const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
        return result.ec == std::errc{} ? parsed : 0;
    }

    static void send_all(SOCKET connection, std::string_view bytes) noexcept {
        while (!bytes.empty()) {
            const auto chunk = static_cast<int>(std::min<std::size_t>(
                bytes.size(), static_cast<std::size_t>(std::numeric_limits<int>::max())));
            const auto sent = send(connection, bytes.data(), chunk, 0);
            if (sent <= 0) {
                return;
            }
            bytes.remove_prefix(static_cast<std::size_t>(sent));
        }
    }

    void serve(const std::stop_token& stop_token) noexcept {
        const auto connection = accept(listener_, nullptr, nullptr);
        if (connection == INVALID_SOCKET) {
            completed_.store(true, std::memory_order_release);
            return;
        }

        std::string received;
        std::size_t expected_size = std::string::npos;
        while (!stop_token.stop_requested() &&
               (expected_size == std::string::npos || received.size() < expected_size)) {
            char buffer[4096]{};
            const auto count = recv(connection, buffer, static_cast<int>(sizeof(buffer)), 0);
            if (count <= 0) {
                break;
            }
            received.append(buffer, static_cast<std::size_t>(count));
            if (expected_size == std::string::npos) {
                const auto header_end = received.find("\r\n\r\n");
                if (header_end != std::string::npos) {
                    expected_size = header_end + 4 + content_length(received);
                    if (behavior_ == ServerBehavior::StallAfterHeaders) {
                        while (!stop_token.stop_requested()) {
                            std::this_thread::sleep_for(std::chrono::milliseconds{1});
                        }
                        break;
                    }
                }
            }
        }

        request_ = std::move(received);
        if (behavior_ == ServerBehavior::Reply && !stop_token.stop_requested()) {
            send_all(connection, response_);
        }
        shutdown(connection, SD_BOTH);
        closesocket(connection);
        completed_.store(true, std::memory_order_release);
    }

    std::string response_;
    ServerBehavior behavior_;
    SOCKET listener_{INVALID_SOCKET};
    std::uint16_t port_{};
    std::jthread thread_;
    std::string request_;
    std::atomic<bool> completed_{};
};

[[nodiscard]] std::unique_ptr<ports::IHttpClient> test_client(TestSuite& suite) {
    auto client = http::make_curl_http_client(
        ports::HttpTransportPolicy{.allow_plaintext_loopback_for_tests = true});
    MANNY_CHECK(suite, client.has_value());
    return client ? std::move(*client) : nullptr;
}

[[nodiscard]] std::string body_string(const ports::HttpResponse& response) {
    return {reinterpret_cast<const char*>(response.body.data()), response.body.size()};
}

void successful_streaming_and_response_tests(TestSuite& suite) {
    LoopbackServer server{"HTTP/1.1 201 Created\r\n"
                          "X-Test: first\r\n"
                          "X-Test: second\r\n"
                          "Set-Cookie: private-marker\r\n"
                          "Content-Length: 2\r\n"
                          "Connection: close\r\n\r\nOK"};
    MANNY_CHECK(suite, server.valid());
    if (!server.valid()) {
        return;
    }
    auto client = test_client(suite);
    if (!client) {
        return;
    }

    auto request = request_for(server.url("/streamed?public=1"), HttpMethod::Post);
    request.headers.push_back(ports::HttpHeader{
        .name = "Content-Type",
        .value = "application/octet-stream",
        .sensitivity = ports::HttpHeaderSensitivity::Public,
    });
    request.body = std::make_unique<VectorBody>("streamed-payload");
    auto response = client->execute(std::move(request));
    report_http_failure(response, "successful loopback request");
    MANNY_CHECK(suite, response.has_value());
    if (!response) {
        return;
    }
    MANNY_CHECK(suite, response->status_code == 201);
    MANNY_CHECK(suite, body_string(*response) == "OK");
    MANNY_CHECK(suite, response->headers.size() == 5);
    MANNY_CHECK(suite, response->headers[0].name == "X-Test");
    MANNY_CHECK(suite, response->headers[1].name == "X-Test");
    MANNY_CHECK(suite, response->headers[2].sensitivity == ports::HttpHeaderSensitivity::Sensitive);

    const auto received = server.request();
    MANNY_CHECK(suite, received.starts_with("POST /streamed?public=1 HTTP/1.1\r\n"));
    MANNY_CHECK(suite, received.find("Content-Length: 16\r\n") != std::string::npos);
    MANNY_CHECK(suite, received.ends_with("\r\n\r\nstreamed-payload"));
}

void redirect_and_response_limit_tests(TestSuite& suite) {
    LoopbackServer redirect{"HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:1/not-followed\r\n"
                            "Content-Length: 0\r\nConnection: close\r\n\r\n"};
    MANNY_CHECK(suite, redirect.valid());
    if (!redirect.valid()) {
        return;
    }
    auto client = test_client(suite);
    if (!client) {
        return;
    }
    auto response = client->execute(request_for(redirect.url()));
    report_http_failure(response, "redirect loopback request");
    MANNY_CHECK(suite, response.has_value());
    if (response) {
        MANNY_CHECK(suite, response->status_code == 302);
    }

    LoopbackServer large_body{
        "HTTP/1.1 200 OK\r\nContent-Length: 4\r\nConnection: close\r\n\r\nABCD"};
    MANNY_CHECK(suite, large_body.valid());
    if (!large_body.valid()) {
        return;
    }
    auto body_request = request_for(large_body.url());
    body_request.response_limits.max_body_bytes = 3;
    auto body_result = client->execute(std::move(body_request));
    MANNY_CHECK(suite, !body_result.has_value());
    if (!body_result) {
        MANNY_CHECK(suite, body_result.error().code == HttpErrorCode::ResponseTooLarge);
    }

    LoopbackServer large_header{
        "HTTP/1.1 200 OK\r\nX-Large: 0123456789012345678901234567890123456789\r\n"
        "Content-Length: 0\r\nConnection: close\r\n\r\n"};
    MANNY_CHECK(suite, large_header.valid());
    if (!large_header.valid()) {
        return;
    }
    auto header_request = request_for(large_header.url());
    header_request.response_limits.max_header_bytes = 30;
    auto header_result = client->execute(std::move(header_request));
    MANNY_CHECK(suite, !header_result.has_value());
    if (!header_result) {
        MANNY_CHECK(suite, header_result.error().code == HttpErrorCode::ResponseTooLarge);
    }
}

void body_contract_and_cancellation_tests(TestSuite& suite) {
    auto client = test_client(suite);
    if (!client) {
        return;
    }

    LoopbackServer early_eof{"HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n"};
    MANNY_CHECK(suite, early_eof.valid());
    if (!early_eof.valid()) {
        return;
    }
    auto eof_request = request_for(early_eof.url(), HttpMethod::Post);
    eof_request.body = std::make_unique<EarlyEofBody>();
    auto eof_result = client->execute(std::move(eof_request));
    MANNY_CHECK(suite, !eof_result.has_value());
    if (!eof_result) {
        MANNY_CHECK(suite, eof_result.error().code == HttpErrorCode::BodyReadFailed);
    }

    LoopbackServer invalid_count{"HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n"};
    MANNY_CHECK(suite, invalid_count.valid());
    if (!invalid_count.valid()) {
        return;
    }
    auto invalid_request = request_for(invalid_count.url(), HttpMethod::Post);
    invalid_request.body = std::make_unique<InvalidCountBody>();
    auto invalid_result = client->execute(std::move(invalid_request));
    MANNY_CHECK(suite, !invalid_result.has_value());
    if (!invalid_result) {
        MANNY_CHECK(suite, invalid_result.error().code == HttpErrorCode::BodyReadFailed);
    }

    LoopbackServer throwing_source{"HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n"};
    MANNY_CHECK(suite, throwing_source.valid());
    if (!throwing_source.valid()) {
        return;
    }
    auto throwing_request = request_for(throwing_source.url(), HttpMethod::Post);
    throwing_request.body = std::make_unique<ThrowingBody>();
    auto throwing_result = client->execute(std::move(throwing_request));
    MANNY_CHECK(suite, !throwing_result.has_value());
    if (!throwing_result) {
        MANNY_CHECK(suite, throwing_result.error().code == HttpErrorCode::Internal);
        MANNY_CHECK(suite, throwing_result.error().message.find("private source detail") ==
                               std::string::npos);
    }

    LoopbackServer stalled{"", ServerBehavior::StallAfterHeaders};
    MANNY_CHECK(suite, stalled.valid());
    if (!stalled.valid()) {
        return;
    }
    std::atomic<std::size_t> reads{};
    std::stop_source cancellation;
    auto cancel_request = request_for(stalled.url(), HttpMethod::Post);
    cancel_request.body = std::make_unique<LargeStreamingBody>(reads);
    const auto start = std::chrono::steady_clock::now();
    std::jthread canceller{[&reads, &cancellation] {
        while (reads.load(std::memory_order_acquire) == 0) {
            std::this_thread::yield();
        }
        cancellation.request_stop();
    }};
    auto cancel_result = client->execute(std::move(cancel_request), cancellation.get_token());
    const auto elapsed = std::chrono::steady_clock::now() - start;
    MANNY_CHECK(suite, !cancel_result.has_value());
    if (!cancel_result) {
        MANNY_CHECK(suite, cancel_result.error().code == HttpErrorCode::Cancelled);
    }
    MANNY_CHECK(suite, elapsed < std::chrono::seconds{3});
}

void optional_live_tls_probe(TestSuite& suite) {
    const auto* enabled = std::getenv("MANNY_HTTP_LIVE_PROBE");
    if (enabled == nullptr || std::string_view{enabled} != "1") {
        return;
    }

    auto client = http::make_curl_http_client();
    MANNY_CHECK(suite, client.has_value());
    if (!client) {
        return;
    }
    auto result = (*client)->execute(request_for("https://example.com/"));
    MANNY_CHECK(suite, result.has_value());
    if (result) {
        MANNY_CHECK(suite, result->status_code == 200);
    }
}

#endif

} // namespace

void run_curl_http_client_tests(TestSuite& suite) {
#ifdef _WIN32
    auto first = http::make_curl_http_client();
    auto second = http::make_curl_http_client();
    MANNY_CHECK(suite, first.has_value());
    MANNY_CHECK(suite, second.has_value());
    if (first) {
        first->reset();
    }
    auto invalid_request = request_for("not-an-absolute-url");
    auto invalid_result = (*second)->execute(std::move(invalid_request));
    MANNY_CHECK(suite, !invalid_result.has_value());
    if (!invalid_result) {
        MANNY_CHECK(suite, invalid_result.error().code == HttpErrorCode::InvalidRequest);
    }

    std::stop_source stopped;
    stopped.request_stop();
    auto cancelled = (*second)->execute(request_for("https://example.test/"), stopped.get_token());
    MANNY_CHECK(suite, !cancelled.has_value());
    if (!cancelled) {
        MANNY_CHECK(suite, cancelled.error().code == HttpErrorCode::Cancelled);
    }

    successful_streaming_and_response_tests(suite);
    redirect_and_response_limit_tests(suite);
    body_contract_and_cancellation_tests(suite);
    optional_live_tls_probe(suite);
#else
    auto client = http::make_curl_http_client();
    MANNY_CHECK(suite, !client.has_value());
    if (!client) {
        MANNY_CHECK(suite, client.error().code == HttpErrorCode::UnsupportedEnvironment);
    }
#endif
}

} // namespace manny_uploader::test
