use super::upload_response as response;
use super::*;
use crate::domain::{DonBotDiscordDeliveryReceipt, LogFileIdentity, canonical_discord_id};
use crate::http::FileBodySource;
use crate::providers::wire::trusted_report_permalink;
use serde::Serialize;
use std::collections::HashSet;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DonBotOptions {
    pub api_base_url: String,
    pub guild_id: String,
    pub delivery: DonBotDeliveryRoute,
}
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct DonBotUploadSuccess {
    pub upload_id: Option<u64>,
    pub fight_log_id: Option<u64>,
    pub discord_delivery: DonBotDiscordDeliveryReceipt,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DonBotAggregateSuccess {
    pub fight_log_count: usize,
    pub discord_delivery: DonBotDiscordDeliveryReceipt,
}
pub trait DonBotUploader: Send + Sync {
    fn upload(
        &self,
        file: &LogFileIdentity,
        options: &DonBotOptions,
        key: &SecretValue,
        stop: &StopToken,
    ) -> Result<DonBotUploadSuccess, DonBotError>;
    fn import_permalink(
        &self,
        permalink: &str,
        options: &DonBotOptions,
        key: &SecretValue,
        stop: &StopToken,
    ) -> Result<DonBotUploadSuccess, DonBotError>;
    /// The action owner must never retry this operation automatically, including Retry errors.
    fn deliver_aggregate(
        &self,
        ids: &[u64],
        options: &DonBotOptions,
        key: &SecretValue,
        stop: &StopToken,
    ) -> Result<DonBotAggregateSuccess, DonBotError>;
}
impl DonBotClient {
    fn execute_upload(
        &self,
        request: HttpRequest,
        stop: &StopToken,
    ) -> Result<HttpResponse, DonBotError> {
        if stop.is_stop_requested() {
            return Err(DonBotError::cancelled());
        }
        let response = self.http.execute(request, stop);
        if stop.is_stop_requested() {
            return Err(DonBotError::cancelled());
        }
        response.map_err(classify_transport)
    }
    fn upload_inner(
        &self,
        file: &LogFileIdentity,
        options: &DonBotOptions,
        key: &SecretValue,
        stop: &StopToken,
    ) -> Result<DonBotUploadSuccess, DonBotError> {
        let base = validate(options, key, stop)?;
        if file.size == 0 || file.size > MAX_HTTP_REQUEST_BODY_BYTES {
            return Err(DonBotError::failed(DonBotErrorCode::InvalidInput));
        }
        let create_url = format!("{base}/api/upload/tus");
        let mut request = upload_request(HttpMethod::Post, create_url.clone(), key);
        request
            .headers
            .push(HttpHeader::public("Tus-Resumable", "1.0.0"));
        request
            .headers
            .push(HttpHeader::public("Upload-Length", file.size.to_string()));
        let mut metadata = format!(
            "filename dXBsb2FkLnpldnRj,guildid {},wingman ZmFsc2U=",
            base64(&options.guild_id)
        );
        if let Some(delivery) = delivery_wire(&options.delivery) {
            metadata.push_str(&format!(",discorddelivery {}", base64(delivery.mode)));
            if let Some(channel) = delivery.channel_id {
                metadata.push_str(&format!(",discordchannelid {}", base64(channel)));
            }
        }
        request
            .headers
            .push(HttpHeader::public("Upload-Metadata", metadata));
        let created = self.execute_upload(request, stop)?;
        if created.status_code != 201 {
            return Err(classify_status(&created));
        }
        if header(&created, "Tus-Resumable")? != Some(b"1.0.0") {
            return Err(response::invalid(201));
        }
        let location = header(&created, "Location")?.ok_or_else(|| response::invalid(201))?;
        let location = std::str::from_utf8(location).map_err(|_| response::invalid(201))?;
        let url = resolve_location(&create_url, location)
            .ok_or(DonBotError::failed(DonBotErrorCode::InvalidLocation))?;
        let upload_id = header(&created, "X-Log-Upload-Id")?
            .map(decimal_id)
            .transpose()
            .map_err(|_| response::invalid(201))?;
        if options.delivery.requested()
            && (upload_id.is_none()
                || header(&created, "X-DonBot-Discord-Delivery")? != Some(b"accepted"))
        {
            return Err(DonBotError::failed(DonBotErrorCode::UnconfirmedDelivery));
        }
        if stop.is_stop_requested() {
            return Err(DonBotError::cancelled());
        }
        let body = FileBodySource::new(file.clone())
            .map_err(|_| DonBotError::failed(DonBotErrorCode::FileUnavailable))?;
        let mut request = upload_request(HttpMethod::Patch, url, key);
        request.headers.extend([
            HttpHeader::public("Tus-Resumable", "1.0.0"),
            HttpHeader::public("Upload-Offset", "0"),
            HttpHeader::public("Content-Type", "application/offset+octet-stream"),
        ]);
        request.body = Some(Box::new(body));
        let patched = self.execute_upload(request, stop).map_err(unconfirmed)?;
        if patched.status_code != 204 {
            return Err(unconfirmed(classify_status(&patched)));
        }
        let tus = header(&patched, "Tus-Resumable").map_err(unconfirmed)?;
        let offset = header(&patched, "Upload-Offset")
            .map_err(unconfirmed)?
            .and_then(|bytes| decimal_id(bytes).ok());
        if tus != Some(b"1.0.0") || offset != Some(file.size) {
            return Err(unconfirmed(response::invalid(204)));
        }
        let completed = match upload_id {
            Some(id) => {
                self.wait_for_processing(&base, id, key, options.delivery.requested(), stop)?
            }
            None => response::Completed::default(),
        };
        Ok(DonBotUploadSuccess {
            upload_id,
            fight_log_id: completed.fight_log_id,
            discord_delivery: completed.delivery,
        })
    }
    fn wait_for_processing(
        &self,
        base: &str,
        id: u64,
        key: &SecretValue,
        requested: bool,
        stop: &StopToken,
    ) -> Result<response::Completed, DonBotError> {
        let mut request = upload_request(
            HttpMethod::Get,
            format!("{base}/api/upload/stream/{id}"),
            key,
        );
        request.headers.retain(|header| header.name() != "Accept");
        request
            .headers
            .push(HttpHeader::public("Accept", "text/event-stream"));
        request.response_limits.max_body_bytes = 256 * 1024;
        match self.execute_upload(request, stop) {
            Ok(reply) if reply.status_code == 200 => response::progress(&reply, requested),
            Err(error) if error.disposition == DonBotDisposition::Cancelled => Err(error),
            result if requested => Err(DonBotError {
                code: DonBotErrorCode::UnconfirmedDelivery,
                disposition: DonBotDisposition::Failed,
                retry_after: None,
                http_status: result.as_ref().ok().map(|r| r.status_code),
                http_error: result.err().and_then(|error| error.http_error),
            }),
            _ => Ok(response::Completed::default()),
        }
    }
    fn import_inner(
        &self,
        permalink: &str,
        options: &DonBotOptions,
        key: &SecretValue,
        stop: &StopToken,
    ) -> Result<DonBotUploadSuccess, DonBotError> {
        let base = validate(options, key, stop)?;
        if !trusted_report_permalink(permalink) {
            return Err(DonBotError::failed(DonBotErrorCode::InvalidInput));
        }
        let body = ImportBody {
            url: permalink,
            guild_id: &options.guild_id,
            discord_delivery: delivery_wire(&options.delivery),
        };
        let request = json_request(format!("{base}/api/upload/gw2/url"), key, &body)?;
        let reply = self.execute_upload(request, stop)?;
        if !matches!(reply.status_code, 200 | 202) {
            return Err(classify_status(&reply));
        }
        let parsed: response::Import = response::decode(&reply)?;
        if !response::positive_id(parsed.upload_id)
            || parsed
                .fight_log_id
                .is_some_and(|id| !response::positive_id(id))
            || !matches!(parsed.status.0.as_str(), "pending" | "complete")
            || (parsed.status.0 == "complete") != parsed.fight_log_id.is_some()
        {
            return Err(response::invalid(reply.status_code));
        }
        let requested = options.delivery.requested();
        if requested && parsed.discord_delivery_accepted != Some(true) {
            return Err(DonBotError::failed(DonBotErrorCode::UnconfirmedDelivery));
        }
        let completed = if parsed.status.0 == "complete" {
            response::Completed {
                fight_log_id: parsed.fight_log_id,
                delivery: response::delivery(
                    parsed.discord_delivery,
                    requested,
                    reply.status_code,
                )?,
            }
        } else {
            if let Some(receipt) = parsed.discord_delivery {
                response::delivery(Some(receipt), requested, reply.status_code)?;
            }
            self.wait_for_processing(&base, parsed.upload_id, key, requested, stop)?
        };
        Ok(DonBotUploadSuccess {
            upload_id: Some(parsed.upload_id),
            fight_log_id: completed.fight_log_id,
            discord_delivery: completed.delivery,
        })
    }
    fn aggregate_inner(
        &self,
        ids: &[u64],
        options: &DonBotOptions,
        key: &SecretValue,
        stop: &StopToken,
    ) -> Result<DonBotAggregateSuccess, DonBotError> {
        let base = validate(options, key, stop)?;
        if !options.delivery.requested()
            || !(2..=usize::from(MAX_DONBOT_AGGREGATE_FIGHTS)).contains(&ids.len())
        {
            return Err(DonBotError::failed(DonBotErrorCode::InvalidInput));
        }
        let mut seen = HashSet::new();
        if ids
            .iter()
            .any(|&id| !response::positive_id(id) || !seen.insert(id))
        {
            return Err(DonBotError::failed(DonBotErrorCode::InvalidInput));
        }
        let body = AggregateBody {
            guild_id: &options.guild_id,
            fight_log_ids: ids.iter().map(u64::to_string).collect(),
            discord_delivery: delivery_wire(&options.delivery),
        };
        let request = json_request(format!("{base}/api/upload/gw2/aggregate"), key, &body)?;
        let reply = self.execute_upload(request, stop)?;
        if reply.status_code != 200 {
            return Err(classify_status(&reply));
        }
        let parsed: response::Aggregate = response::decode(&reply)?;
        if parsed.fight_log_count != ids.len() {
            return Err(response::invalid(reply.status_code));
        }
        let receipt = parsed
            .discord_delivery
            .0
            .receipt(true, MAX_DONBOT_AGGREGATE_FIGHTS)
            .ok_or_else(|| response::invalid(reply.status_code))?;
        Ok(DonBotAggregateSuccess {
            fight_log_count: ids.len(),
            discord_delivery: receipt,
        })
    }
}
impl DonBotUploader for DonBotClient {
    fn upload(
        &self,
        file: &LogFileIdentity,
        options: &DonBotOptions,
        key: &SecretValue,
        stop: &StopToken,
    ) -> Result<DonBotUploadSuccess, DonBotError> {
        catch_unwind(AssertUnwindSafe(|| {
            self.upload_inner(file, options, key, stop)
        }))
        .unwrap_or(Err(DonBotError::failed(DonBotErrorCode::Internal)))
    }
    fn import_permalink(
        &self,
        permalink: &str,
        options: &DonBotOptions,
        key: &SecretValue,
        stop: &StopToken,
    ) -> Result<DonBotUploadSuccess, DonBotError> {
        catch_unwind(AssertUnwindSafe(|| {
            self.import_inner(permalink, options, key, stop)
        }))
        .unwrap_or(Err(DonBotError::failed(DonBotErrorCode::Internal)))
    }
    fn deliver_aggregate(
        &self,
        ids: &[u64],
        options: &DonBotOptions,
        key: &SecretValue,
        stop: &StopToken,
    ) -> Result<DonBotAggregateSuccess, DonBotError> {
        catch_unwind(AssertUnwindSafe(|| {
            self.aggregate_inner(ids, options, key, stop)
        }))
        .unwrap_or(Err(DonBotError::failed(DonBotErrorCode::Internal)))
    }
}
fn validate(
    options: &DonBotOptions,
    key: &SecretValue,
    stop: &StopToken,
) -> Result<String, DonBotError> {
    if stop.is_stop_requested() {
        return Err(DonBotError::cancelled());
    }
    let base = normalize_donbot_api_base(&options.api_base_url)
        .ok_or(DonBotError::failed(DonBotErrorCode::InvalidBase))?;
    if !canonical_discord_id(&options.guild_id) || !options.delivery.is_valid() {
        return Err(DonBotError::failed(DonBotErrorCode::InvalidInput));
    }
    if !valid_donbot_api_key(key) {
        return Err(DonBotError::failed(DonBotErrorCode::InvalidKey));
    }
    Ok(base)
}
fn upload_request(method: HttpMethod, url: String, key: &SecretValue) -> HttpRequest {
    let mut request = HttpRequest::new(method, url);
    request.headers = vec![
        HttpHeader::public("Accept", "application/json"),
        HttpHeader::sensitive(
            "X-GW2-API-Key",
            SecretValue::from_bytes(key.expose().to_vec()),
        ),
    ];
    request.timeouts = HttpTimeouts {
        connect: Duration::from_secs(10),
        operation: Duration::from_secs(900),
        stalled_transfer: Duration::from_secs(900),
    };
    request.response_limits = HttpResponseLimits {
        max_header_bytes: MAX_HTTP_RESPONSE_HEADER_BYTES,
        max_body_bytes: 64 * 1024,
    };
    request
}
fn json_request(
    url: String,
    key: &SecretValue,
    body: &impl Serialize,
) -> Result<HttpRequest, DonBotError> {
    let mut request = upload_request(HttpMethod::Post, url, key);
    request
        .headers
        .push(HttpHeader::public("Content-Type", "application/json"));
    let bytes =
        serde_json::to_vec(body).map_err(|_| DonBotError::failed(DonBotErrorCode::BodyFailed))?;
    request.body =
        Some(Box::new(MemoryBodySource::new(bytes).map_err(|_| {
            DonBotError::failed(DonBotErrorCode::BodyFailed)
        })?));
    Ok(request)
}
fn unconfirmed(mut error: DonBotError) -> DonBotError {
    if error.disposition != DonBotDisposition::Cancelled {
        error.disposition = DonBotDisposition::Failed;
        error.retry_after = None;
        error.code = DonBotErrorCode::UnconfirmedUpload;
    }
    error
}
fn header<'a>(response: &'a HttpResponse, name: &str) -> Result<Option<&'a [u8]>, DonBotError> {
    let mut values = response
        .headers
        .iter()
        .filter(|h| h.name().eq_ignore_ascii_case(name));
    let value = values.next().map(HttpHeader::expose_value);
    if values.next().is_some() {
        return Err(response::invalid(response.status_code));
    }
    Ok(value)
}
fn decimal_id(bytes: &[u8]) -> Result<u64, ()> {
    let text = std::str::from_utf8(bytes).map_err(|_| ())?;
    if !canonical_discord_id(text) {
        return Err(());
    }
    text.parse().map_err(|_| ())
}
fn resolve_location(create_url: &str, location: &str) -> Option<String> {
    let origin_end = create_url[8..].find('/').map(|index| index + 8)?;
    let path = &create_url[origin_end..];
    let relative_prefix = format!("{path}/");
    let id = if let Some(rest) = location.strip_prefix("https://") {
        let end = rest.find('/').map(|index| index + 8)?;
        if !location[..end].eq_ignore_ascii_case(&create_url[..origin_end]) {
            return None;
        }
        location[end..].strip_prefix(&relative_prefix)?
    } else if location.starts_with('/') {
        location.strip_prefix(&relative_prefix)?
    } else {
        location
    };
    if id.is_empty()
        || id.len() > 256
        || !id
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || matches!(b, b'-' | b'_'))
    {
        return None;
    }
    Some(format!("{create_url}/{id}"))
}
#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct DeliveryWire<'a> {
    mode: &'static str,
    #[serde(skip_serializing_if = "Option::is_none")]
    channel_id: Option<&'a str>,
}
fn delivery_wire(route: &DonBotDeliveryRoute) -> Option<DeliveryWire<'_>> {
    match route {
        DonBotDeliveryRoute::None => None,
        DonBotDeliveryRoute::GuildDefaults => Some(DeliveryWire {
            mode: "guild_defaults",
            channel_id: None,
        }),
        DonBotDeliveryRoute::ChannelOverride { channel_id } => Some(DeliveryWire {
            mode: "channel_override",
            channel_id: Some(channel_id),
        }),
    }
}
#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct ImportBody<'a> {
    url: &'a str,
    guild_id: &'a str,
    #[serde(skip_serializing_if = "Option::is_none")]
    discord_delivery: Option<DeliveryWire<'a>>,
}
#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct AggregateBody<'a> {
    guild_id: &'a str,
    fight_log_ids: Vec<String>,
    discord_delivery: Option<DeliveryWire<'a>>,
}
fn base64(value: &str) -> String {
    const TABLE: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut result = String::with_capacity(value.len().div_ceil(3) * 4);
    for chunk in value.as_bytes().chunks(3) {
        let a = chunk[0];
        let b = chunk.get(1).copied().unwrap_or(0);
        let c = chunk.get(2).copied().unwrap_or(0);
        result.push(char::from(TABLE[usize::from(a >> 2)]));
        result.push(char::from(TABLE[usize::from(((a & 3) << 4) | (b >> 4))]));
        result.push(if chunk.len() > 1 {
            char::from(TABLE[usize::from(((b & 15) << 2) | (c >> 6))])
        } else {
            '='
        });
        result.push(if chunk.len() > 2 {
            char::from(TABLE[usize::from(c & 63)])
        } else {
            '='
        });
    }
    result
}
