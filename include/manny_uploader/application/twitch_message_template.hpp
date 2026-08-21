#pragma once

#include "manny_uploader/domain/upload_job.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace manny_uploader::application {

inline constexpr std::size_t max_twitch_template_bytes = 500;
inline constexpr std::size_t max_rendered_twitch_message_bytes = 2'000;
inline constexpr std::size_t max_rendered_twitch_message_code_points = 500;

enum class TwitchTemplateErrorCode : std::uint8_t {
    EmptyTemplate,
    InvalidUtf8,
    TemplateTooLong,
    ControlCharacter,
    InvalidSyntax,
    UnknownField,
    MissingPermalinkField,
    InvalidFieldValue,
    EmptyMessage,
    MessageTooLong,
    AllocationFailed,
};

struct TwitchTemplateError {
    TwitchTemplateErrorCode code;
    std::string message;
};

class TwitchMessageTemplate {
  public:
    enum class Field : std::uint8_t {
        Url,
        Encounter,
        Mode,
        ModeSuffix,
        Result,
        BossId,
    };

    using Part = std::variant<std::string, Field>;

    [[nodiscard]] static std::expected<TwitchMessageTemplate, TwitchTemplateError>
    parse(std::string_view source);

    [[nodiscard]] std::expected<std::string, TwitchTemplateError>
    render(const domain::DpsReportResult& report) const;

    [[nodiscard]] const std::string& source() const noexcept;

  private:
    TwitchMessageTemplate(std::string source, std::vector<Part> parts) noexcept;

    std::string source_;
    std::vector<Part> parts_;
};

} // namespace manny_uploader::application
