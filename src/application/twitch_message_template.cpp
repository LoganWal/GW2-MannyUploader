#include "manny_uploader/application/twitch_message_template.hpp"

#include "manny_uploader/support/utf8.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <optional>
#include <ranges>
#include <utility>

namespace manny_uploader::application {
namespace {

[[nodiscard]] TwitchTemplateError make_error(TwitchTemplateErrorCode code, std::string message) {
    return TwitchTemplateError{.code = code, .message = std::move(message)};
}

[[nodiscard]] bool has_ascii_control(std::string_view value) noexcept {
    return std::ranges::any_of(value, [](char character) {
        const auto byte = static_cast<unsigned char>(character);
        return byte < 0x20U || byte == 0x7fU;
    });
}

[[nodiscard]] std::optional<TwitchMessageTemplate::Field>
field_from_name(std::string_view name) noexcept {
    using Field = TwitchMessageTemplate::Field;
    constexpr std::array fields{
        std::pair{std::string_view{"url"}, Field::Url},
        std::pair{std::string_view{"encounter"}, Field::Encounter},
        std::pair{std::string_view{"mode"}, Field::Mode},
        std::pair{std::string_view{"mode_suffix"}, Field::ModeSuffix},
        std::pair{std::string_view{"result"}, Field::Result},
        std::pair{std::string_view{"boss_id"}, Field::BossId},
    };
    const auto* const found = std::ranges::find(fields, name, &decltype(fields)::value_type::first);
    if (found == fields.end()) {
        return std::nullopt;
    }
    return found->second;
}

void append_literal(std::vector<TwitchMessageTemplate::Part>& parts, std::string& literal) {
    if (literal.empty()) {
        return;
    }
    parts.emplace_back(std::move(literal));
    literal.clear();
}

[[nodiscard]] std::expected<void, TwitchTemplateError> append_text(std::string& output,
                                                                   std::string_view value) {
    if (!support::is_valid_utf8(value) || has_ascii_control(value)) {
        return std::unexpected(make_error(TwitchTemplateErrorCode::InvalidFieldValue,
                                          "A Twitch template field is invalid"));
    }
    if (value.size() > max_rendered_twitch_message_bytes - output.size()) {
        return std::unexpected(make_error(TwitchTemplateErrorCode::MessageTooLong,
                                          "The rendered Twitch message exceeds 500 characters"));
    }
    output.append(value);
    return {};
}

[[nodiscard]] std::string boss_id_text(std::uint16_t boss_id) {
    std::array<char, std::numeric_limits<std::uint16_t>::digits10 + 2> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), boss_id);
    return std::string{buffer.data(), converted.ptr};
}

struct ParsedParts {
    std::vector<TwitchMessageTemplate::Part> parts;
    bool has_permalink;
};

[[nodiscard]] std::expected<ParsedParts, TwitchTemplateError> parse_parts(std::string_view source) {
    std::vector<TwitchMessageTemplate::Part> parts;
    std::string literal;
    bool has_permalink{};
    std::size_t index{};
    while (index < source.size()) {
        if (source[index] == '{') {
            if (index + 1 < source.size() && source[index + 1] == '{') {
                literal.push_back('{');
                index += 2;
                continue;
            }
            append_literal(parts, literal);
            const auto close = source.find('}', index + 1);
            const auto nested = source.find('{', index + 1);
            if (close == std::string_view::npos ||
                (nested != std::string_view::npos && nested < close)) {
                return std::unexpected(make_error(TwitchTemplateErrorCode::InvalidSyntax,
                                                  "Twitch message template has unbalanced braces"));
            }
            const auto name = source.substr(index + 1, close - index - 1);
            const auto field = field_from_name(name);
            if (!field) {
                return std::unexpected(
                    make_error(TwitchTemplateErrorCode::UnknownField,
                               "Twitch message template contains an unknown or empty placeholder"));
            }
            has_permalink = has_permalink || *field == TwitchMessageTemplate::Field::Url;
            parts.emplace_back(*field);
            index = close + 1;
            continue;
        }
        if (source[index] == '}') {
            if (index + 1 < source.size() && source[index + 1] == '}') {
                literal.push_back('}');
                index += 2;
                continue;
            }
            return std::unexpected(make_error(TwitchTemplateErrorCode::InvalidSyntax,
                                              "Twitch message template has an unmatched brace"));
        }
        literal.push_back(source[index]);
        ++index;
    }
    append_literal(parts, literal);
    return ParsedParts{.parts = std::move(parts), .has_permalink = has_permalink};
}

} // namespace

std::expected<TwitchMessageTemplate, TwitchTemplateError>
TwitchMessageTemplate::parse(std::string_view source) {
    if (source.empty()) {
        return std::unexpected(make_error(TwitchTemplateErrorCode::EmptyTemplate,
                                          "Twitch message template must not be empty"));
    }
    if (source.size() > max_twitch_template_bytes) {
        return std::unexpected(make_error(TwitchTemplateErrorCode::TemplateTooLong,
                                          "Twitch message template exceeds 500 bytes"));
    }
    if (!support::is_valid_utf8(source)) {
        return std::unexpected(make_error(TwitchTemplateErrorCode::InvalidUtf8,
                                          "Twitch message template must be valid UTF-8"));
    }
    if (has_ascii_control(source)) {
        return std::unexpected(make_error(TwitchTemplateErrorCode::ControlCharacter,
                                          "Twitch message template contains a control character"));
    }

    try {
        auto parsed = parse_parts(source);
        if (!parsed) {
            return std::unexpected(std::move(parsed.error()));
        }
        if (!parsed->has_permalink) {
            return std::unexpected(make_error(TwitchTemplateErrorCode::MissingPermalinkField,
                                              "Twitch message template must contain {url}"));
        }
        return TwitchMessageTemplate{std::string{source}, std::move(parsed->parts)};
    } catch (...) {
        return std::unexpected(make_error(TwitchTemplateErrorCode::AllocationFailed,
                                          "Twitch message template could not be parsed"));
    }
}

TwitchMessageTemplate::TwitchMessageTemplate(std::string source, std::vector<Part> parts) noexcept
    : source_{std::move(source)}, parts_{std::move(parts)} {}

std::expected<std::string, TwitchTemplateError>
TwitchMessageTemplate::render(const domain::DpsReportResult& report) const {
    if (report.permalink.empty()) {
        return std::unexpected(make_error(TwitchTemplateErrorCode::InvalidFieldValue,
                                          "The dps.report permalink is unavailable"));
    }
    try {
        const auto mode_suffix = report.mode.empty() ? std::string{} : " (" + report.mode + ')';
        const auto result = std::string_view{report.success ? "Success" : "Failure"};
        const auto boss_id = boss_id_text(report.boss_id);
        std::string output;
        output.reserve(
            std::min(source_.size() + report.permalink.size(), max_rendered_twitch_message_bytes));
        for (const auto& part : parts_) {
            std::expected<void, TwitchTemplateError> appended;
            if (const auto* literal = std::get_if<std::string>(&part)) {
                appended = append_text(output, *literal);
            } else {
                switch (std::get<Field>(part)) {
                case Field::Url:
                    appended = append_text(output, report.permalink);
                    break;
                case Field::Encounter:
                    appended = append_text(output, report.encounter_name);
                    break;
                case Field::Mode:
                    appended = append_text(output, report.mode);
                    break;
                case Field::ModeSuffix:
                    appended = append_text(output, mode_suffix);
                    break;
                case Field::Result:
                    appended = append_text(output, result);
                    break;
                case Field::BossId:
                    appended = append_text(output, boss_id);
                    break;
                }
            }
            if (!appended) {
                return std::unexpected(std::move(appended.error()));
            }
        }
        if (output.empty()) {
            return std::unexpected(make_error(TwitchTemplateErrorCode::EmptyMessage,
                                              "The rendered Twitch message is empty"));
        }
        const auto code_points = support::utf8_code_point_count(output);
        if (!code_points) {
            return std::unexpected(make_error(TwitchTemplateErrorCode::InvalidFieldValue,
                                              "The rendered Twitch message is invalid"));
        }
        if (*code_points > max_rendered_twitch_message_code_points) {
            return std::unexpected(
                make_error(TwitchTemplateErrorCode::MessageTooLong,
                           "The rendered Twitch message exceeds 500 characters"));
        }
        return output;
    } catch (...) {
        return std::unexpected(make_error(TwitchTemplateErrorCode::AllocationFailed,
                                          "The Twitch message could not be rendered"));
    }
}

const std::string& TwitchMessageTemplate::source() const noexcept {
    return source_;
}

} // namespace manny_uploader::application
