use manny_uploader::domain::*;

fn report() -> DpsReportResult {
    DpsReportResult {
        permalink: "https://dps.report/Example".into(),
        encounter_name: "Vale Guardian".into(),
        boss_id: 15438,
        mode: "CM".into(),
        success: true,
    }
}

#[test]
fn fields_escapes_and_empty_modes_render_from_the_same_report() {
    let source = "{{log}} {encounter}{mode_suffix}|{mode}|{result}|{boss_id}|{url} }}";
    let template = TwitchMessageTemplate::parse(source).unwrap();
    assert_eq!(template.source(), source);
    assert_eq!(
        template.render(&report()).unwrap(),
        "{log} Vale Guardian (CM)|CM|Success|15438|https://dps.report/Example }"
    );
    let mut failed = report();
    failed.success = false;
    failed.mode.clear();
    failed.boss_id = 0;
    assert_eq!(
        template.render(&failed).unwrap(),
        "{log} Vale Guardian||Failure|0|https://dps.report/Example }"
    );
    assert_eq!(
        TwitchMessageTemplate::parse("{{{url}}}")
            .unwrap()
            .render(&report())
            .unwrap(),
        "{https://dps.report/Example}"
    );
}

#[test]
fn malformed_templates_controls_and_missing_real_url_are_rejected() {
    for (text, error) in [
        ("", TemplateError::EmptyTemplate),
        ("{url", TemplateError::InvalidSyntax),
        ("url}", TemplateError::InvalidSyntax),
        ("{encounter{url}", TemplateError::InvalidSyntax),
        ("{} {url}", TemplateError::UnknownField),
        ("{secret}{url}", TemplateError::UnknownField),
        ("{{url}}", TemplateError::MissingUrl),
        ("{encounter}", TemplateError::MissingUrl),
    ] {
        assert_eq!(TwitchMessageTemplate::parse(text), Err(error));
    }
    assert_eq!(
        TwitchMessageTemplate::parse(&"x".repeat(501)),
        Err(TemplateError::TemplateTooLong)
    );
    for byte in (0..=31_u8).chain([127]) {
        assert_eq!(
            TwitchMessageTemplate::parse(&format!("{}{url}", char::from(byte), url = "{url}")),
            Err(TemplateError::ControlCharacter)
        );
    }
}

#[test]
fn expansion_bounds_bytes_and_unicode_code_points_without_truncation() {
    let template = TwitchMessageTemplate::parse("{encounter}{url}").unwrap();
    let mut result = report();
    result.permalink = "\u{1f600}".into();
    result.encounter_name = "\u{1f600}".repeat(499);
    let output = template.render(&result).unwrap();
    assert_eq!((output.len(), output.chars().count()), (2000, 500));
    result.encounter_name.push('a');
    assert_eq!(template.render(&result), Err(TemplateError::MessageTooLong));
    result.permalink = "u".into();
    result.encounter_name = "a".repeat(499);
    assert_eq!(template.render(&result).unwrap().len(), 500);
    result.encounter_name.push('a');
    assert_eq!(template.render(&result), Err(TemplateError::MessageTooLong));
    result.encounter_name = "a".repeat(2000);
    assert_eq!(template.render(&result), Err(TemplateError::MessageTooLong));
    result.permalink.clear();
    assert_eq!(
        template.render(&result),
        Err(TemplateError::InvalidFieldValue)
    );
}

#[test]
fn field_controls_cannot_escape_through_mode_suffix_or_repeated_placeholders() {
    let template = TwitchMessageTemplate::parse("{url}{mode_suffix}{encounter}").unwrap();
    for field in 0..3 {
        let mut result = report();
        match field {
            0 => result.permalink.push('\r'),
            1 => result.mode.push('\n'),
            _ => result.encounter_name.push('\0'),
        }
        assert_eq!(
            template.render(&result),
            Err(TemplateError::InvalidFieldValue)
        );
    }
    let template = TwitchMessageTemplate::parse(&"{url}".repeat(100)).unwrap();
    let mut result = report();
    result.permalink = "u".repeat(6);
    assert_eq!(template.render(&result), Err(TemplateError::MessageTooLong));
    result.permalink = "u".repeat(5);
    assert_eq!(template.render(&result).unwrap().len(), 500);
}
