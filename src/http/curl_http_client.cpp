#include "manny_uploader/http/curl_http_client.hpp"

#include "manny_uploader/support/secret_value.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#ifdef _WIN32
#include <curl/curl.h>

#include <algorithm>
#include <chrono>
#include <limits>
#include <mutex>
#include <span>
#include <string_view>
#include <vector>
#endif

namespace manny_uploader::http {
namespace {

[[nodiscard]] ports::HttpError
make_error(ports::HttpErrorCode code, std::string message,
           std::optional<std::int64_t> transport_code = std::nullopt,
           std::optional<ports::HttpBodyReadErrorCode> body_error = std::nullopt,
           std::optional<std::uint32_t> system_error = std::nullopt) {
    return ports::HttpError{
        .code = code,
        .message = std::move(message),
        .transport_code = transport_code,
        .body_error = body_error,
        .system_error = system_error,
    };
}

#ifdef _WIN32

class CurlRuntime final {
  public:
    ~CurlRuntime() {
        auto& state = runtime_state();
        const auto lock = std::scoped_lock{state.mutex};
        --state.users;
        if (state.users == 0) {
            curl_global_cleanup();
        }
    }

    CurlRuntime(const CurlRuntime&) = delete;
    CurlRuntime& operator=(const CurlRuntime&) = delete;

    [[nodiscard]] static std::expected<std::unique_ptr<CurlRuntime>, ports::HttpError> acquire() {
        auto& state = runtime_state();
        const auto lock = std::scoped_lock{state.mutex};
        if (state.users == 0) {
            const auto result = curl_global_init(CURL_GLOBAL_DEFAULT);
            if (result != CURLE_OK) {
                return std::unexpected(make_error(ports::HttpErrorCode::InitializationFailed,
                                                  "The HTTP transport could not initialize",
                                                  static_cast<std::int64_t>(result)));
            }
        }
        ++state.users;
        return std::unique_ptr<CurlRuntime>{new CurlRuntime{}};
    }

  private:
    struct RuntimeState {
        std::mutex mutex;
        std::size_t users{};
    };

    [[nodiscard]] static RuntimeState& runtime_state() {
        static RuntimeState state;
        return state;
    }

    CurlRuntime() = default;
};

struct EasyHandleDeleter {
    void operator()(CURL* handle) const noexcept {
        curl_easy_cleanup(handle);
    }
};

using EasyHandle = std::unique_ptr<CURL, EasyHandleDeleter>;

class HeaderList final {
  public:
    ~HeaderList() {
        for (auto* entry = head_; entry != nullptr; entry = entry->next) {
            if (entry->data != nullptr) {
                const auto size = std::char_traits<char>::length(entry->data);
                support::secure_erase(std::as_writable_bytes(std::span{entry->data, size}));
            }
        }
        curl_slist_free_all(head_);
    }

    HeaderList(const HeaderList&) = delete;
    HeaderList& operator=(const HeaderList&) = delete;

    HeaderList() = default;

    [[nodiscard]] bool append(const std::string& value) noexcept {
        auto* appended = curl_slist_append(head_, value.c_str());
        if (appended == nullptr) {
            return false;
        }
        head_ = appended;
        return true;
    }

    [[nodiscard]] curl_slist* get() const noexcept {
        return head_;
    }

  private:
    curl_slist* head_{};
};

struct CallbackFailure {
    ports::HttpErrorCode code;
    std::string_view message;
    std::optional<ports::HttpBodyReadErrorCode> body_error;
    std::optional<std::uint32_t> system_error;
};

struct TransferContext {
    ports::HttpRequest& request;
    const std::stop_token& stop_token;
    ports::HttpResponse response{.status_code = 0, .headers = {}, .body = {}};
    std::uint64_t uploaded_bytes{};
    std::size_t received_header_bytes{};
    std::optional<CallbackFailure> callback_failure;
};

[[nodiscard]] bool has_http_status_prefix(std::string_view line) noexcept {
    return line.size() >= 5 && line[0] == 'H' && line[1] == 'T' && line[2] == 'T' &&
           line[3] == 'P' && line[4] == '/';
}

[[nodiscard]] bool is_token_character(char value) noexcept {
    if ((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
        (value >= '0' && value <= '9')) {
        return true;
    }
    constexpr std::string_view punctuation = "!#$%&'*+-.^_`|~";
    return punctuation.contains(value);
}

[[nodiscard]] const char* method_c_string(ports::HttpMethod method) noexcept {
    switch (method) {
    case ports::HttpMethod::Get:
        return "GET";
    case ports::HttpMethod::Post:
        return "POST";
    case ports::HttpMethod::Put:
        return "PUT";
    case ports::HttpMethod::Patch:
        return "PATCH";
    case ports::HttpMethod::Delete:
        return "DELETE";
    }
    return "";
}

[[nodiscard]] std::string_view trim_optional_whitespace(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    return value;
}

void set_callback_failure(TransferContext& context, ports::HttpErrorCode code,
                          std::string_view message,
                          std::optional<ports::HttpBodyReadErrorCode> body_error = std::nullopt,
                          std::optional<std::uint32_t> system_error = std::nullopt) noexcept {
    if (!context.callback_failure) {
        context.callback_failure = CallbackFailure{
            .code = code,
            .message = message,
            .body_error = body_error,
            .system_error = system_error,
        };
    }
}

[[nodiscard]] std::size_t read_body(char* destination, std::size_t size, std::size_t count,
                                    void* user_data) noexcept {
    auto& context = *static_cast<TransferContext*>(user_data);
    try {
        if (context.stop_token.stop_requested()) {
            set_callback_failure(context, ports::HttpErrorCode::Cancelled,
                                 "The HTTP request was cancelled");
            return CURL_READFUNC_ABORT;
        }
        if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) {
            set_callback_failure(context, ports::HttpErrorCode::Internal,
                                 "The HTTP transport received an invalid upload buffer");
            return CURL_READFUNC_ABORT;
        }
        if (!context.request.body) {
            set_callback_failure(context, ports::HttpErrorCode::Internal,
                                 "The HTTP transport requested a missing upload body");
            return CURL_READFUNC_ABORT;
        }

        const auto capacity = size * count;
        const auto content_length = context.request.body->content_length();
        if (context.uploaded_bytes > content_length) {
            set_callback_failure(context, ports::HttpErrorCode::BodyReadFailed,
                                 "The HTTP body source exceeded its declared length");
            return CURL_READFUNC_ABORT;
        }
        const auto remaining = content_length - context.uploaded_bytes;
        if (remaining == 0 || capacity == 0) {
            return 0;
        }

        const auto bounded_capacity = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, static_cast<std::uint64_t>(capacity)));
        auto bytes = std::span{reinterpret_cast<std::byte*>(destination), bounded_capacity};
        auto result = context.request.body->read(bytes, context.stop_token);
        if (!result) {
            const auto& source_error = result.error();
            const auto cancelled = source_error.code == ports::HttpBodyReadErrorCode::Cancelled ||
                                   context.stop_token.stop_requested();
            set_callback_failure(context,
                                 cancelled ? ports::HttpErrorCode::Cancelled
                                           : ports::HttpErrorCode::BodyReadFailed,
                                 cancelled ? "The HTTP request was cancelled"
                                           : "The HTTP body source could not be read",
                                 source_error.code, source_error.system_error);
            return CURL_READFUNC_ABORT;
        }
        if (*result == 0) {
            set_callback_failure(context, ports::HttpErrorCode::BodyReadFailed,
                                 "The HTTP body source ended before its declared length");
            return CURL_READFUNC_ABORT;
        }
        if (*result > bounded_capacity) {
            set_callback_failure(context, ports::HttpErrorCode::BodyReadFailed,
                                 "The HTTP body source returned an invalid byte count");
            return CURL_READFUNC_ABORT;
        }

        context.uploaded_bytes += static_cast<std::uint64_t>(*result);
        return *result;
    } catch (...) {
        set_callback_failure(context, ports::HttpErrorCode::Internal,
                             "The HTTP body callback failed unexpectedly");
        return CURL_READFUNC_ABORT;
    }
}

[[nodiscard]] std::size_t
receive_body(char* data, // NOLINT(readability-non-const-parameter): libcurl callback ABI.
             std::size_t size, std::size_t count, void* user_data) noexcept {
    auto& context = *static_cast<TransferContext*>(user_data);
    try {
        if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) {
            set_callback_failure(context, ports::HttpErrorCode::ResponseTooLarge,
                                 "The HTTP response body exceeded its limit");
            return 0;
        }
        const auto byte_count = size * count;
        const auto limit = context.request.response_limits.max_body_bytes;
        if (byte_count > limit - context.response.body.size()) {
            set_callback_failure(context, ports::HttpErrorCode::ResponseTooLarge,
                                 "The HTTP response body exceeded its limit");
            return 0;
        }

        const auto* begin = reinterpret_cast<const std::byte*>(data);
        context.response.body.insert(context.response.body.end(), begin, begin + byte_count);
        return byte_count;
    } catch (...) {
        set_callback_failure(context, ports::HttpErrorCode::Internal,
                             "The HTTP response callback failed unexpectedly");
        return 0;
    }
}

[[nodiscard]] std::size_t receive_header(char* data, std::size_t size, std::size_t count,
                                         void* user_data) noexcept {
    auto& context = *static_cast<TransferContext*>(user_data);
    try {
        if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) {
            set_callback_failure(context, ports::HttpErrorCode::ResponseTooLarge,
                                 "The HTTP response headers exceeded their limit");
            return 0;
        }
        const auto byte_count = size * count;
        const auto limit = context.request.response_limits.max_header_bytes;
        if (byte_count > limit - context.received_header_bytes) {
            set_callback_failure(context, ports::HttpErrorCode::ResponseTooLarge,
                                 "The HTTP response headers exceeded their limit");
            return 0;
        }
        context.received_header_bytes += byte_count;

        auto line = std::string_view{data, byte_count};
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.remove_suffix(1);
        }
        if (line.empty()) {
            return byte_count;
        }
        if (has_http_status_prefix(line)) {
            context.response.headers.clear();
            return byte_count;
        }

        const auto colon = line.find(':');
        const auto name = line.substr(0, colon);
        if (colon == std::string_view::npos || name.empty() ||
            !std::ranges::all_of(name, is_token_character)) {
            set_callback_failure(context, ports::HttpErrorCode::ProtocolError,
                                 "The HTTP response contained an invalid header");
            return 0;
        }
        const auto value = trim_optional_whitespace(line.substr(colon + 1));
        context.response.headers.push_back(ports::HttpHeader{
            .name = std::string{name},
            .value = std::string{value},
            .sensitivity = ports::is_sensitive_http_header_name(name)
                               ? ports::HttpHeaderSensitivity::Sensitive
                               : ports::HttpHeaderSensitivity::Public,
        });
        return byte_count;
    } catch (...) {
        set_callback_failure(context, ports::HttpErrorCode::Internal,
                             "The HTTP header callback failed unexpectedly");
        return 0;
    }
}

[[nodiscard]] int report_progress(void* user_data, curl_off_t /*download_total*/,
                                  curl_off_t /*downloaded*/, curl_off_t /*upload_total*/,
                                  curl_off_t /*uploaded*/) noexcept {
    auto& context = *static_cast<TransferContext*>(user_data);
    if (!context.stop_token.stop_requested()) {
        return 0;
    }
    set_callback_failure(context, ports::HttpErrorCode::Cancelled,
                         "The HTTP request was cancelled");
    return 1;
}

[[nodiscard]] ports::HttpError map_curl_error(CURLcode code) {
    using ports::HttpErrorCode;
    auto mapped = HttpErrorCode::Internal;
    auto message = std::string{"The HTTP transport failed unexpectedly"};
    switch (code) {
    case CURLE_OPERATION_TIMEDOUT:
        mapped = HttpErrorCode::Timeout;
        message = "The HTTP request timed out";
        break;
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_RESOLVE_PROXY:
        mapped = HttpErrorCode::NameResolutionFailed;
        message = "The HTTP destination name could not be resolved";
        break;
    case CURLE_COULDNT_CONNECT:
        mapped = HttpErrorCode::ConnectionFailed;
        message = "The HTTP destination could not be reached";
        break;
    case CURLE_PEER_FAILED_VERIFICATION:
    case CURLE_SSL_CERTPROBLEM:
    case CURLE_SSL_CIPHER:
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_SSL_ENGINE_INITFAILED:
    case CURLE_SSL_ISSUER_ERROR:
    case CURLE_SSL_PINNEDPUBKEYNOTMATCH:
        mapped = HttpErrorCode::TlsFailed;
        message = "The HTTPS connection could not be verified";
        break;
    case CURLE_SEND_ERROR:
    case CURLE_UPLOAD_FAILED:
        mapped = HttpErrorCode::SendFailed;
        message = "The HTTP request could not be sent";
        break;
    case CURLE_RECV_ERROR:
    case CURLE_PARTIAL_FILE:
        mapped = HttpErrorCode::ReceiveFailed;
        message = "The HTTP response could not be received";
        break;
    case CURLE_FILESIZE_EXCEEDED:
        mapped = HttpErrorCode::ResponseTooLarge;
        message = "The HTTP response body exceeded its limit";
        break;
    case CURLE_UNSUPPORTED_PROTOCOL:
    case CURLE_WEIRD_SERVER_REPLY:
    case CURLE_HTTP2:
    case CURLE_HTTP2_STREAM:
        mapped = HttpErrorCode::ProtocolError;
        message = "The HTTP peer returned an unsupported response";
        break;
    case CURLE_URL_MALFORMAT:
        mapped = HttpErrorCode::InvalidRequest;
        message = "The HTTP URL is invalid";
        break;
    case CURLE_ABORTED_BY_CALLBACK:
        mapped = HttpErrorCode::Cancelled;
        message = "The HTTP request was cancelled";
        break;
    case CURLE_READ_ERROR:
        mapped = HttpErrorCode::BodyReadFailed;
        message = "The HTTP body source could not be read";
        break;
    case CURLE_WRITE_ERROR:
        mapped = HttpErrorCode::ReceiveFailed;
        message = "The HTTP response could not be stored";
        break;
    default:
        break;
    }
    return make_error(mapped, std::move(message), static_cast<std::int64_t>(code));
}

[[nodiscard]] bool set_common_options(CURL* handle, TransferContext& context,
                                      const ports::HttpTransportPolicy policy) noexcept {
    const auto connect_timeout = static_cast<long>(context.request.timeouts.connect.count());
    const auto operation_timeout = static_cast<long>(context.request.timeouts.operation.count());
    const auto stall_seconds = static_cast<long>(
        std::chrono::ceil<std::chrono::seconds>(context.request.timeouts.stalled_transfer).count());
    const auto* const protocols =
        policy.allow_plaintext_loopback_for_tests ? "http,https" : "https";

    const auto* const no_proxy = policy.allow_plaintext_loopback_for_tests ? "*" : nullptr;
    return curl_easy_setopt(handle, CURLOPT_URL, context.request.url.c_str()) == CURLE_OK &&
           curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST,
                            method_c_string(context.request.method)) == CURLE_OK &&
           curl_easy_setopt(handle, CURLOPT_PROTOCOLS_STR, protocols) == CURLE_OK &&
           curl_easy_setopt(handle, CURLOPT_REDIR_PROTOCOLS_STR, protocols) == CURLE_OK &&
           curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 0L) == CURLE_OK &&
           curl_easy_setopt(handle, CURLOPT_MAXREDIRS, 0L) == CURLE_OK &&
           curl_easy_setopt(handle, CURLOPT_UNRESTRICTED_AUTH, 0L) == CURLE_OK &&
           curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L) == CURLE_OK &&
           curl_easy_setopt(handle, CURLOPT_NOPROXY, no_proxy) == CURLE_OK &&
           curl_easy_setopt(handle, CURLOPT_VERBOSE, 0L) == CURLE_OK &&
           curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 1L) == CURLE_OK &&
           curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 2L) == CURLE_OK &&
           curl_easy_setopt(handle, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2) == CURLE_OK &&
           curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS, connect_timeout) == CURLE_OK &&
           curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, operation_timeout) == CURLE_OK &&
           curl_easy_setopt(handle, CURLOPT_LOW_SPEED_LIMIT, 1L) == CURLE_OK &&
           curl_easy_setopt(handle, CURLOPT_LOW_SPEED_TIME, stall_seconds) == CURLE_OK &&
           curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, receive_body) == CURLE_OK &&
           curl_easy_setopt(handle, CURLOPT_WRITEDATA, &context) == CURLE_OK &&
           curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, receive_header) == CURLE_OK &&
           curl_easy_setopt(handle, CURLOPT_HEADERDATA, &context) == CURLE_OK &&
           curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L) == CURLE_OK &&
           curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, report_progress) == CURLE_OK &&
           curl_easy_setopt(handle, CURLOPT_XFERINFODATA, &context) == CURLE_OK &&
           curl_easy_setopt(handle, CURLOPT_FAILONERROR, 0L) == CURLE_OK &&
           curl_easy_setopt(handle, CURLOPT_MAXFILESIZE_LARGE,
                            static_cast<curl_off_t>(
                                context.request.response_limits.max_body_bytes)) == CURLE_OK &&
           curl_easy_setopt(handle, CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L) == CURLE_OK;
}

class CurlHttpClient final : public ports::IHttpClient {
  public:
    CurlHttpClient(std::unique_ptr<CurlRuntime> runtime, ports::HttpTransportPolicy policy)
        : runtime_{std::move(runtime)}, policy_{policy} {}

    [[nodiscard]] std::expected<ports::HttpResponse, ports::HttpError>
    execute(ports::HttpRequest request, const std::stop_token& stop_token) const override {
        try {
            auto valid = ports::validate_http_request(request, policy_);
            if (!valid) {
                return std::unexpected(std::move(valid.error()));
            }
            if (stop_token.stop_requested()) {
                return std::unexpected(
                    make_error(ports::HttpErrorCode::Cancelled, "The HTTP request was cancelled"));
            }

            auto handle = EasyHandle{curl_easy_init()};
            if (!handle) {
                return std::unexpected(make_error(ports::HttpErrorCode::InitializationFailed,
                                                  "The HTTP request could not initialize"));
            }
            TransferContext context{
                .request = request,
                .stop_token = stop_token,
                .response = {.status_code = 0, .headers = {}, .body = {}},
                .uploaded_bytes = 0,
                .received_header_bytes = 0,
                .callback_failure = std::nullopt,
            };
            if (!set_common_options(handle.get(), context, policy_)) {
                return std::unexpected(make_error(ports::HttpErrorCode::InitializationFailed,
                                                  "The HTTP request could not initialize"));
            }

            HeaderList headers;
            for (const auto& header : request.headers) {
                auto line = header.name + ": " + header.value;
                const auto appended = headers.append(line);
                if (header.sensitivity == ports::HttpHeaderSensitivity::Sensitive) {
                    support::secure_erase(std::as_writable_bytes(std::span{line}));
                }
                if (!appended) {
                    return std::unexpected(make_error(ports::HttpErrorCode::InitializationFailed,
                                                      "The HTTP headers could not initialize"));
                }
            }
            if (request.body && !headers.append("Expect:")) {
                return std::unexpected(make_error(ports::HttpErrorCode::InitializationFailed,
                                                  "The HTTP headers could not initialize"));
            }
            if (headers.get() != nullptr &&
                curl_easy_setopt(handle.get(), CURLOPT_HTTPHEADER, headers.get()) != CURLE_OK) {
                return std::unexpected(make_error(ports::HttpErrorCode::InitializationFailed,
                                                  "The HTTP headers could not initialize"));
            }

            if (request.body) {
                const auto content_length = request.body->content_length();
                if (curl_easy_setopt(handle.get(), CURLOPT_UPLOAD, 1L) != CURLE_OK ||
                    curl_easy_setopt(handle.get(), CURLOPT_INFILESIZE_LARGE,
                                     static_cast<curl_off_t>(content_length)) != CURLE_OK ||
                    curl_easy_setopt(handle.get(), CURLOPT_READFUNCTION, read_body) != CURLE_OK ||
                    curl_easy_setopt(handle.get(), CURLOPT_READDATA, &context) != CURLE_OK) {
                    return std::unexpected(make_error(ports::HttpErrorCode::InitializationFailed,
                                                      "The HTTP body could not initialize"));
                }
            }

            const auto result = curl_easy_perform(handle.get());
            if (context.callback_failure) {
                const auto failure = *context.callback_failure;
                return std::unexpected(make_error(failure.code, std::string{failure.message},
                                                  static_cast<std::int64_t>(result),
                                                  failure.body_error, failure.system_error));
            }
            if (result != CURLE_OK) {
                return std::unexpected(map_curl_error(result));
            }
            if (request.body && context.uploaded_bytes != request.body->content_length()) {
                return std::unexpected(make_error(ports::HttpErrorCode::BodyReadFailed,
                                                  "The HTTP body source length did not match"));
            }

            long status_code{};
            if (curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &status_code) != CURLE_OK ||
                status_code < 100 || status_code > 599) {
                return std::unexpected(make_error(ports::HttpErrorCode::ProtocolError,
                                                  "The HTTP response status is invalid"));
            }
            context.response.status_code = static_cast<std::uint16_t>(status_code);
            return std::move(context.response);
        } catch (...) {
            return std::unexpected(
                make_error(ports::HttpErrorCode::Internal, "The HTTP request failed unexpectedly"));
        }
    }

  private:
    std::unique_ptr<CurlRuntime> runtime_;
    ports::HttpTransportPolicy policy_;
};

#endif

} // namespace

std::expected<std::unique_ptr<ports::IHttpClient>, ports::HttpError>
make_curl_http_client(ports::HttpTransportPolicy policy) {
#ifdef _WIN32
    auto runtime = CurlRuntime::acquire();
    if (!runtime) {
        return std::unexpected(std::move(runtime.error()));
    }
    return std::make_unique<CurlHttpClient>(std::move(*runtime), policy);
#else
    static_cast<void>(policy);
    return std::unexpected(make_error(ports::HttpErrorCode::UnsupportedEnvironment,
                                      "The HTTP transport requires Windows"));
#endif
}

} // namespace manny_uploader::http
