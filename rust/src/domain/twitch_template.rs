use super::DpsReportResult;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TemplateError {
    EmptyTemplate,
    TemplateTooLong,
    ControlCharacter,
    InvalidSyntax,
    UnknownField,
    MissingUrl,
    InvalidFieldValue,
    MessageTooLong,
    AllocationFailed,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Field {
    Url,
    Encounter,
    Mode,
    ModeSuffix,
    Result,
    BossId,
}

#[derive(Clone, Debug, PartialEq, Eq)]
enum Part {
    Literal(String),
    Field(Field),
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TwitchMessageTemplate {
    source: String,
    parts: Vec<Part>,
}

impl TwitchMessageTemplate {
    pub fn parse(source: &str) -> Result<Self, TemplateError> {
        if source.is_empty() {
            return Err(TemplateError::EmptyTemplate);
        }
        if source.len() > 500 {
            return Err(TemplateError::TemplateTooLong);
        }
        if source.bytes().any(|byte| byte.is_ascii_control()) {
            return Err(TemplateError::ControlCharacter);
        }
        let mut parts = Vec::new();
        let mut literal = String::new();
        let mut input = source.chars().peekable();
        let mut url = false;
        while let Some(ch) = input.next() {
            match ch {
                '{' if input.peek() == Some(&'{') => {
                    input.next();
                    literal.push('{');
                }
                '}' if input.peek() == Some(&'}') => {
                    input.next();
                    literal.push('}');
                }
                '{' => {
                    if !literal.is_empty() {
                        parts.push(Part::Literal(std::mem::take(&mut literal)));
                    }
                    let mut name = String::new();
                    loop {
                        match input.next() {
                            Some('}') => break,
                            Some('{') | None => return Err(TemplateError::InvalidSyntax),
                            Some(ch) => name.push(ch),
                        }
                    }
                    let field = match name.as_str() {
                        "url" => {
                            url = true;
                            Field::Url
                        }
                        "encounter" => Field::Encounter,
                        "mode" => Field::Mode,
                        "mode_suffix" => Field::ModeSuffix,
                        "result" => Field::Result,
                        "boss_id" => Field::BossId,
                        _ => return Err(TemplateError::UnknownField),
                    };
                    parts.push(Part::Field(field));
                }
                '}' => return Err(TemplateError::InvalidSyntax),
                _ => literal.push(ch),
            }
        }
        if !url {
            return Err(TemplateError::MissingUrl);
        }
        if !literal.is_empty() {
            parts.push(Part::Literal(literal));
        }
        Ok(Self {
            source: source.into(),
            parts,
        })
    }

    pub fn source(&self) -> &str {
        &self.source
    }

    pub fn render(&self, report: &DpsReportResult) -> Result<String, TemplateError> {
        if report.permalink.is_empty() {
            return Err(TemplateError::InvalidFieldValue);
        }
        let boss = report.boss_id.to_string();
        let mut output = String::new();
        for part in &self.parts {
            let value = match part {
                Part::Literal(text) => text,
                Part::Field(Field::Url) => &report.permalink,
                Part::Field(Field::Encounter) => &report.encounter_name,
                Part::Field(Field::Mode) => &report.mode,
                Part::Field(Field::Result) => {
                    if report.success {
                        "Success"
                    } else {
                        "Failure"
                    }
                }
                Part::Field(Field::BossId) => &boss,
                Part::Field(Field::ModeSuffix) => {
                    if !report.mode.is_empty() {
                        append(&mut output, " (")?;
                        append(&mut output, &report.mode)?;
                        append(&mut output, ")")?;
                    }
                    continue;
                }
            };
            append(&mut output, value)?;
        }
        if output.chars().count() > 500 {
            return Err(TemplateError::MessageTooLong);
        }
        Ok(output)
    }
}

fn append(output: &mut String, value: &str) -> Result<(), TemplateError> {
    if value.len() > 2000 - output.len() {
        return Err(TemplateError::MessageTooLong);
    }
    if value.bytes().any(|byte| byte.is_ascii_control()) {
        return Err(TemplateError::InvalidFieldValue);
    }
    output
        .try_reserve(value.len())
        .map_err(|_| TemplateError::AllocationFailed)?;
    output.push_str(value);
    Ok(())
}
