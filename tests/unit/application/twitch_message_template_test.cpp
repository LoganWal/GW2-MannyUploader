#include "manny_uploader/application/twitch_message_template.hpp"
#include "support/test_suite.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace manny_uploader::test {
namespace {

using application::TwitchMessageTemplate;
using application::TwitchTemplateErrorCode;

[[nodiscard]] domain::DpsReportResult report(bool success = true, std::string mode = "CM") {
    return domain::DpsReportResult{
        .permalink = "https://dps.report/Example",
        .encounter_name = "Vale Guardian",
        .boss_id = 15438,
        .mode = std::move(mode),
        .success = success,
    };
}

void parser_tests(TestSuite& suite) {
    const auto parsed = TwitchMessageTemplate::parse(
        "{{log}} {encounter}|{mode}|{mode_suffix}|{result}|{boss_id}|{url} }}");
    MANNY_CHECK(suite, parsed.has_value());
    MANNY_CHECK(suite, parsed->source().starts_with("{{log}}"));
    const auto rendered = parsed->render(report());
    MANNY_CHECK(suite, rendered.has_value());
    MANNY_CHECK(suite, *rendered == "{log} Vale Guardian|CM| (CM)|Success|15438|"
                                    "https://dps.report/Example }");

    struct InvalidCase {
        std::string source;
        TwitchTemplateErrorCode code;
    };
    const InvalidCase invalid_cases[]{
        {.source = "", .code = TwitchTemplateErrorCode::EmptyTemplate},
        {.source = std::string(501, 'x'), .code = TwitchTemplateErrorCode::TemplateTooLong},
        {.source = std::string{"bad\xc0\x80{url}", 11},
         .code = TwitchTemplateErrorCode::InvalidUtf8},
        {.source = "line\n{url}", .code = TwitchTemplateErrorCode::ControlCharacter},
        {.source = "{url", .code = TwitchTemplateErrorCode::InvalidSyntax},
        {.source = "url}", .code = TwitchTemplateErrorCode::InvalidSyntax},
        {.source = "{encounter {url}", .code = TwitchTemplateErrorCode::InvalidSyntax},
        {.source = "{} {url}", .code = TwitchTemplateErrorCode::UnknownField},
        {.source = "{unknown} {url}", .code = TwitchTemplateErrorCode::UnknownField},
        {.source = "{{url}}", .code = TwitchTemplateErrorCode::MissingPermalinkField},
        {.source = "{encounter}", .code = TwitchTemplateErrorCode::MissingPermalinkField},
    };
    for (const auto& invalid : invalid_cases) {
        const auto result = TwitchMessageTemplate::parse(invalid.source);
        MANNY_CHECK(suite, !result.has_value());
        if (!result) {
            MANNY_CHECK(suite, result.error().code == invalid.code);
        }
    }
}

void field_semantics_tests(TestSuite& suite) {
    const auto parsed = TwitchMessageTemplate::parse(
        "{encounter}{mode_suffix}: {result} ({mode}, {boss_id}) {url}");
    MANNY_CHECK(suite, parsed.has_value());
    const auto success = parsed->render(report(true, "LCM"));
    MANNY_CHECK(suite, success == "Vale Guardian (LCM): Success (LCM, 15438) "
                                  "https://dps.report/Example");
    const auto failure = parsed->render(report(false, ""));
    MANNY_CHECK(suite, failure == "Vale Guardian: Failure (, 15438) https://dps.report/Example");

    auto zero = report();
    zero.boss_id = 0;
    const auto boss = TwitchMessageTemplate::parse("{boss_id}:{url}")->render(zero);
    MANNY_CHECK(suite, boss == "0:https://dps.report/Example");
}

void rendered_value_tests(TestSuite& suite) {
    const auto parsed = TwitchMessageTemplate::parse("{encounter}{url}");
    MANNY_CHECK(suite, parsed.has_value());

    auto invalid = report();
    invalid.permalink.clear();
    auto rendered = parsed->render(invalid);
    MANNY_CHECK(suite, !rendered.has_value());
    MANNY_CHECK(suite, rendered.error().code == TwitchTemplateErrorCode::InvalidFieldValue);

    invalid = report();
    invalid.encounter_name = std::string{"bad\xc0\x80", 5};
    rendered = parsed->render(invalid);
    MANNY_CHECK(suite, !rendered.has_value());
    MANNY_CHECK(suite, rendered.error().code == TwitchTemplateErrorCode::InvalidFieldValue);

    invalid = report();
    invalid.encounter_name = "bad\nname";
    rendered = parsed->render(invalid);
    MANNY_CHECK(suite, !rendered.has_value());
    MANNY_CHECK(suite, rendered.error().code == TwitchTemplateErrorCode::InvalidFieldValue);

    auto exact = report();
    exact.permalink = "u";
    exact.encounter_name.clear();
    for (std::size_t index = 0; index < 499; ++index) {
        exact.encounter_name += "😀";
    }
    rendered = parsed->render(exact);
    MANNY_CHECK(suite, rendered.has_value());

    exact.encounter_name += "😀";
    rendered = parsed->render(exact);
    MANNY_CHECK(suite, !rendered.has_value());
    MANNY_CHECK(suite, rendered.error().code == TwitchTemplateErrorCode::MessageTooLong);

    auto byte_limit = report();
    byte_limit.permalink = "u";
    byte_limit.encounter_name = std::string(2'000, 'x');
    rendered = parsed->render(byte_limit);
    MANNY_CHECK(suite, !rendered.has_value());
    MANNY_CHECK(suite, rendered.error().code == TwitchTemplateErrorCode::MessageTooLong);
}

} // namespace

void run_twitch_message_template_tests(TestSuite& suite) {
    parser_tests(suite);
    field_semantics_tests(suite);
    rendered_value_tests(suite);
}

} // namespace manny_uploader::test
