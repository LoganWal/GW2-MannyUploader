#include "manny_uploader/http/body_sources.hpp"
#include "manny_uploader/http/multipart_form_data.hpp"

#include "support/test_suite.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace manny_uploader::test {
namespace {

[[nodiscard]] std::vector<std::byte> bytes(std::string_view text) {
    const auto characters = std::span{text.data(), text.size()};
    const auto byte_view = std::as_bytes(characters);
    return {byte_view.begin(), byte_view.end()};
}

[[nodiscard]] std::string text(std::span<const std::byte> value) {
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

[[nodiscard]] std::expected<std::vector<std::byte>, ports::HttpBodyReadError>
read_all(ports::IHttpBodySource& source, std::size_t chunk_size = 3,
         const std::stop_token& stop_token = {}) {
    std::vector<std::byte> result;
    result.reserve(static_cast<std::size_t>(source.content_length()));
    std::vector<std::byte> buffer(chunk_size);
    while (result.size() < source.content_length()) {
        auto read = source.read(buffer, stop_token);
        if (!read) {
            return std::unexpected(std::move(read.error()));
        }
        if (*read == 0 || *read > buffer.size()) {
            return std::unexpected(ports::HttpBodyReadError{
                .code = ports::HttpBodyReadErrorCode::ReadFailed,
                .message = "Test source returned an invalid byte count",
                .system_error = std::nullopt,
            });
        }
        result.insert(result.end(), buffer.begin(),
                      buffer.begin() + static_cast<std::ptrdiff_t>(*read));
    }
    return result;
}

class TempBodyFile {
  public:
    explicit TempBodyFile(std::string_view contents) {
        static std::atomic_uint64_t next_id{};
        path_ = std::filesystem::temp_directory_path() /
                ("manny-http-body-" + std::to_string(next_id.fetch_add(1)) + ".zevtc");
        write(contents, false);
    }

    ~TempBodyFile() {
        std::error_code error;
        static_cast<void>(std::filesystem::remove(path_, error));
    }

    TempBodyFile(const TempBodyFile&) = delete;
    TempBodyFile& operator=(const TempBodyFile&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

    [[nodiscard]] std::uintmax_t size() const {
        return std::filesystem::file_size(path_);
    }

    [[nodiscard]] std::filesystem::file_time_type last_write_time() const {
        return std::filesystem::last_write_time(path_);
    }

    void append(std::string_view contents) {
        write(contents, true);
    }

  private:
    void write(std::string_view contents, bool append) const {
        auto mode = std::ios::binary | std::ios::out;
        mode |= append ? std::ios::app : std::ios::trunc;
        std::ofstream stream{path_, mode};
        stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        if (!stream) {
            throw std::runtime_error{"Could not write HTTP body fixture"};
        }
    }

    std::filesystem::path path_;
};

class DeclaredLengthBody final : public ports::IHttpBodySource {
  public:
    explicit DeclaredLengthBody(std::uint64_t length) : length_{length} {}

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

void file_body_tests(TestSuite& suite) {
    TempBodyFile file{"abcdefgh"};
    auto source =
        http::make_file_http_body_source(file.path(), file.size(), file.last_write_time());
    MANNY_CHECK(suite, source.has_value());
    if (source) {
        MANNY_CHECK(suite, (*source)->content_length() == 8);
        auto contents = read_all(**source, 2);
        MANNY_CHECK(suite, contents.has_value());
        if (contents) {
            MANNY_CHECK(suite, text(*contents) == "abcdefgh");
        }
        std::array<std::byte, 1> extra{};
        const auto eof = (*source)->read(extra, {});
        MANNY_CHECK(suite, eof.has_value());
        MANNY_CHECK(suite, eof.value_or(1) == 0);
    }

    auto wrong_size =
        http::make_file_http_body_source(file.path(), file.size() + 1, file.last_write_time());
    MANNY_CHECK(suite, !wrong_size.has_value());
    if (!wrong_size) {
        MANNY_CHECK(suite, wrong_size.error().code == ports::HttpBodyReadErrorCode::SourceChanged);
        MANNY_CHECK(suite,
                    wrong_size.error().message.find(file.path().string()) == std::string::npos);
    }

    auto missing = http::make_file_http_body_source(file.path().string() + ".missing", 1, {});
    MANNY_CHECK(suite, !missing.has_value());
    if (!missing) {
        MANNY_CHECK(suite, missing.error().code == ports::HttpBodyReadErrorCode::SourceUnavailable);
    }

    auto cancelled =
        http::make_file_http_body_source(file.path(), file.size(), file.last_write_time());
    MANNY_CHECK(suite, cancelled.has_value());
    if (cancelled) {
        std::stop_source stop;
        stop.request_stop();
        std::array<std::byte, 4> buffer{};
        const auto result = (*cancelled)->read(buffer, stop.get_token());
        MANNY_CHECK(suite, !result.has_value());
        if (!result) {
            MANNY_CHECK(suite, result.error().code == ports::HttpBodyReadErrorCode::Cancelled);
        }
    }

    const auto original_size = file.size();
    const auto original_time = file.last_write_time();
    auto changed = http::make_file_http_body_source(file.path(), original_size, original_time);
    MANNY_CHECK(suite, changed.has_value());
    if (changed) {
        std::array<std::byte, 4> buffer{};
        const auto first = (*changed)->read(buffer, {});
        MANNY_CHECK(suite, first.has_value());
        file.append("changed");
        const auto second = (*changed)->read(buffer, {});
        MANNY_CHECK(suite, !second.has_value());
        if (!second) {
            MANNY_CHECK(suite, second.error().code == ports::HttpBodyReadErrorCode::SourceChanged);
        }
    }
}

void memory_body_tests(TestSuite& suite) {
    auto memory = http::make_memory_http_body_source(bytes("memory-body"));
    auto memory_result = read_all(*memory, 1);
    MANNY_CHECK(suite, memory_result.has_value());
    if (memory_result) {
        MANNY_CHECK(suite, text(*memory_result) == "memory-body");
    }

    const auto secret = support::SecretValue::from_text("secret-marker");
    auto secret_body = http::make_secret_http_body_source(secret);
    MANNY_CHECK(suite, secret_body.has_value());
    if (secret_body) {
        auto secret_result = read_all(**secret_body, 4);
        MANNY_CHECK(suite, secret_result.has_value());
        if (secret_result) {
            MANNY_CHECK(suite, text(*secret_result) == "secret-marker");
        }
    }
    MANNY_CHECK(suite, !secret.empty());
}

[[nodiscard]] http::MultipartPart memory_part(std::string name, std::string value) {
    return http::MultipartPart{
        .name = std::move(name),
        .filename = std::nullopt,
        .content_type = "text/plain",
        .body = http::make_memory_http_body_source(bytes(value)),
    };
}

void multipart_success_tests(TestSuite& suite) {
    std::vector<http::MultipartPart> parts;
    parts.push_back(memory_part("field", "value"));
    parts.push_back(http::MultipartPart{
        .name = "file",
        .filename = "upload.zevtc",
        .content_type = "application/octet-stream",
        .body = http::make_memory_http_body_source(bytes("FILE")),
    });
    auto form = http::make_multipart_form_data(std::move(parts), "manny-test-boundary");
    MANNY_CHECK(suite, form.has_value());
    if (!form) {
        return;
    }
    MANNY_CHECK(suite, form->content_type == "multipart/form-data; boundary=manny-test-boundary");
    auto contents = read_all(*form->body, 5);
    MANNY_CHECK(suite, contents.has_value());
    if (!contents) {
        return;
    }
    constexpr std::string_view expected =
        "--manny-test-boundary\r\n"
        "Content-Disposition: form-data; name=\"field\"\r\n"
        "Content-Type: text/plain\r\n\r\n"
        "value\r\n"
        "--manny-test-boundary\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"upload.zevtc\"\r\n"
        "Content-Type: application/octet-stream\r\n\r\n"
        "FILE\r\n"
        "--manny-test-boundary--\r\n";
    MANNY_CHECK(suite, text(*contents) == expected);
    MANNY_CHECK(suite, form->body->content_length() == expected.size());

    std::vector<http::MultipartPart> generated_parts;
    generated_parts.push_back(memory_part("field", "value"));
    const auto generated = http::make_multipart_form_data(std::move(generated_parts));
    MANNY_CHECK(suite, generated.has_value());
    if (generated) {
        MANNY_CHECK(suite, generated->content_type.starts_with("multipart/form-data; boundary="));
        MANNY_CHECK(suite, generated->content_type.size() <=
                               std::string_view{"multipart/form-data; boundary="}.size() +
                                   http::max_multipart_boundary_bytes);
    }
}

void multipart_validation_tests(TestSuite& suite) {
    const auto empty = http::make_multipart_form_data({}, "boundary");
    MANNY_CHECK(suite, !empty.has_value());
    MANNY_CHECK(suite, empty.error().code == http::MultipartErrorCode::EmptyParts);

    for (const auto& boundary : {std::string{}, std::string{"bad boundary"},
                                 std::string(http::max_multipart_boundary_bytes + 1, 'x')}) {
        std::vector<http::MultipartPart> parts;
        parts.push_back(memory_part("field", "value"));
        const auto result = http::make_multipart_form_data(std::move(parts), boundary);
        MANNY_CHECK(suite, !result.has_value());
        if (!result) {
            MANNY_CHECK(suite, result.error().code == http::MultipartErrorCode::InvalidBoundary);
        }
    }

    const auto expect_part_error = [&suite](http::MultipartPart part,
                                            http::MultipartErrorCode code) {
        std::vector<http::MultipartPart> parts;
        parts.push_back(std::move(part));
        const auto result = http::make_multipart_form_data(std::move(parts), "boundary");
        MANNY_CHECK(suite, !result.has_value());
        if (!result) {
            MANNY_CHECK(suite, result.error().code == code);
        }
    };
    expect_part_error(memory_part("bad name", "value"), http::MultipartErrorCode::InvalidPartName);
    auto bad_filename = memory_part("file", "value");
    bad_filename.filename = "bad\"name";
    expect_part_error(std::move(bad_filename), http::MultipartErrorCode::InvalidFilename);
    auto bad_type = memory_part("file", "value");
    bad_type.content_type = "not-a-media-type";
    expect_part_error(std::move(bad_type), http::MultipartErrorCode::InvalidContentType);
    expect_part_error(
        http::MultipartPart{
            .name = "field",
            .filename = std::nullopt,
            .content_type = std::nullopt,
            .body = nullptr,
        },
        http::MultipartErrorCode::MissingBody);

    std::vector<http::MultipartPart> too_many;
    for (std::size_t index = 0; index <= http::max_multipart_part_count; ++index) {
        too_many.push_back(memory_part("field", "value"));
    }
    const auto too_many_result = http::make_multipart_form_data(std::move(too_many), "boundary");
    MANNY_CHECK(suite, !too_many_result.has_value());
    MANNY_CHECK(suite, too_many_result.error().code == http::MultipartErrorCode::TooManyParts);

    std::vector<http::MultipartPart> too_large;
    too_large.push_back(http::MultipartPart{
        .name = "file",
        .filename = "upload.zevtc",
        .content_type = "application/octet-stream",
        .body = std::make_unique<DeclaredLengthBody>(ports::max_http_request_body_bytes),
    });
    const auto too_large_result = http::make_multipart_form_data(std::move(too_large), "boundary");
    MANNY_CHECK(suite, !too_large_result.has_value());
    MANNY_CHECK(suite, too_large_result.error().code == http::MultipartErrorCode::BodyTooLarge);

    std::vector<http::MultipartPart> early_eof;
    early_eof.push_back(http::MultipartPart{
        .name = "field",
        .filename = std::nullopt,
        .content_type = std::nullopt,
        .body = std::make_unique<DeclaredLengthBody>(4),
    });
    auto early_eof_form = http::make_multipart_form_data(std::move(early_eof), "boundary");
    MANNY_CHECK(suite, early_eof_form.has_value());
    if (early_eof_form) {
        const auto result = read_all(*early_eof_form->body);
        MANNY_CHECK(suite, !result.has_value());
        if (!result) {
            MANNY_CHECK(suite, result.error().code == ports::HttpBodyReadErrorCode::ReadFailed);
        }
    }

    std::vector<http::MultipartPart> cancelled_parts;
    cancelled_parts.push_back(memory_part("field", "value"));
    auto cancelled_form = http::make_multipart_form_data(std::move(cancelled_parts), "boundary");
    MANNY_CHECK(suite, cancelled_form.has_value());
    if (cancelled_form) {
        std::stop_source stop;
        stop.request_stop();
        std::array<std::byte, 8> buffer{};
        const auto result = cancelled_form->body->read(buffer, stop.get_token());
        MANNY_CHECK(suite, !result.has_value());
        if (!result) {
            MANNY_CHECK(suite, result.error().code == ports::HttpBodyReadErrorCode::Cancelled);
        }
    }
}

} // namespace

void run_http_body_source_tests(TestSuite& suite) {
    file_body_tests(suite);
    memory_body_tests(suite);
    multipart_success_tests(suite);
    multipart_validation_tests(suite);
}

} // namespace manny_uploader::test
