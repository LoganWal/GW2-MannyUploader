use super::*;

const DONBOT_BASE: &str = "https://donbot.example";

#[derive(Default)]
struct FanoutHttp {
    deliveries: Mutex<Vec<&'static str>>,
}

impl HttpClient for FanoutHttp {
    fn execute(
        &self,
        mut request: HttpRequest,
        stop: &StopToken,
    ) -> Result<HttpResponse, HttpError> {
        validate_http_request(&request, HttpTransportPolicy::HttpsOnly).unwrap();
        let mut body = Vec::new();
        if let Some(source) = request.body.as_mut() {
            let mut chunk = [0; 1024];
            loop {
                let count = source.read(&mut chunk, stop).unwrap();
                if count == 0 {
                    break;
                }
                body.extend_from_slice(&chunk[..count]);
            }
            assert_eq!(body.len() as u64, source.content_length());
        }
        let mut deliveries = self.deliveries.lock().unwrap();
        let reply = if request.url == DPS_REPORT_UPLOAD_URL {
            assert!(body.windows(4).any(|part| part == b"PK\x03\x04"));
            deliveries.push("dps");
            r#"{"permalink":"https://dps.report/fixture","encounter":{"bossId":123,"boss":"Fixture","success":true}}"#.into()
        } else if request.url == format!("{WINGMAN_IMPORT_URL}https%3A%2F%2Fdps.report%2Ffixture") {
            assert!(deliveries.contains(&"dps"));
            deliveries.push("wingman");
            r#"{"success":1}"#.into()
        } else if request.url
            == format!("{WINGMAN_CHECK_IMPORT_URL}https%3A%2F%2Fdps.report%2Ffixture")
        {
            format!(r#"{{"inQueue":false,"inDB":true,"targetURL":"{WINGMAN_LOG_URL}fixture"}}"#)
        } else if request.url == format!("{DONBOT_BASE}/api/upload/gw2/guilds") {
            assert_eq!(body, br#"{"apiKey":"synthetic-donbot-key"}"#);
            r#"{"accountName":"Synthetic.1234","guilds":[{"guildId":"123","guildName":"Fixture"}]}"#
                .into()
        } else if request.url == format!("{DONBOT_BASE}/api/upload/gw2/url") {
            assert!(deliveries.contains(&"dps"));
            assert_eq!(
                body,
                br#"{"url":"https://dps.report/fixture","guildId":"123"}"#
            );
            assert!(
                request
                    .headers
                    .iter()
                    .any(|header| header.name() == "X-GW2-API-Key"
                        && header.expose_value() == b"synthetic-donbot-key")
            );
            deliveries.push("donbot");
            r#"{"uploadId":42,"fightLogId":99,"status":"complete","duplicate":false}"#.into()
        } else if request.url == "https://id.twitch.tv/oauth2/validate" {
            assert!(
                request
                    .headers
                    .iter()
                    .any(|header| header.name() == "Authorization"
                        && header.expose_value() == b"OAuth synthetic-access")
            );
            format!(
                r#"{{"client_id":"{PACKAGED_TWITCH_CLIENT_ID}","user_id":"456","login":"fixture","expires_in":14400,"scopes":["user:write:chat"]}}"#
            )
        } else if request.url == "https://api.twitch.tv/helix/chat/messages" {
            assert!(deliveries.contains(&"dps"));
            let body = String::from_utf8(body).unwrap();
            assert!(body.contains(r#""broadcaster_id":"456""#));
            assert!(body.contains(r#""sender_id":"456""#));
            assert!(body.contains("https://dps.report/fixture"));
            assert!(
                request
                    .headers
                    .iter()
                    .any(|header| header.name() == "Authorization"
                        && header.expose_value() == b"Bearer synthetic-access")
            );
            deliveries.push("twitch");
            r#"{"data":[{"is_sent":true,"message_id":"fixture-message"}]}"#.into()
        } else {
            panic!("unexpected synthetic HTTP operation");
        };
        Ok(HttpResponse {
            status_code: 200,
            headers: vec![],
            body: SecretValue::from_bytes(reply.into_bytes()),
        })
    }
}

#[test]
fn native_fanout_uses_one_report_and_preserves_all_receipts_on_restart() {
    let fixture = Fixture::new();
    let session = TwitchSession {
        credentials: TwitchCredentialSet {
            access_token: SecretValue::from_bytes(b"synthetic-access".to_vec()),
            refresh_token: SecretValue::from_bytes(b"synthetic-refresh".to_vec()),
            access_expires_at: SystemTime::now() + Duration::from_secs(14400),
        },
        user_id: "456".into(),
        login: "fixture".into(),
        scopes: vec![TWITCH_CHAT_SCOPE.into()],
    };
    protected_store(&fixture.addon())
        .unwrap()
        .store(
            SecretId::TwitchOAuthSession,
            &encode_twitch_session(&session).unwrap(),
        )
        .unwrap();
    let http = Arc::new(FanoutHttp::default());
    let mut runtime = fixture.runtime(http.clone());
    let handle = runtime.handle();
    let stop = CancellationSource::default();
    handle
        .submit_options(NexusOptionsCommand::VerifyDonBot {
            api_base_url: DONBOT_BASE.into(),
            api_key: SecretValue::from_bytes(b"synthetic-donbot-key".to_vec()),
        })
        .unwrap();
    until(|| {
        runtime.tick(&stop.token()).unwrap();
        let options = handle.snapshot().options;
        options.donbot.state == DonBotConfigurationState::Verified
            && options.twitch.state == TwitchConnectionState::Connected
    });
    for command in [
        NexusOptionsCommand::SelectDonBotGuild("123".into()),
        NexusOptionsCommand::SetDonBotEnabled(true),
        NexusOptionsCommand::SetTwitchEnabled(true),
    ] {
        handle.submit_options(command).unwrap();
    }
    runtime.tick(&stop.token()).unwrap();
    let settings = handle.snapshot().options.configuration.settings;
    assert!(settings.dps_report.enabled && settings.wingman.enabled);
    assert!(settings.donbot.enabled && settings.twitch.enabled);
    let root = fixture.game().join("arcdps.cbtlogs");
    std::fs::create_dir_all(&root).unwrap();
    std::fs::write(root.join("fixture.zevtc"), synthetic_archive()).unwrap();
    handle.select_logs(LogSelectionMode::Last24Hours).unwrap();
    until(|| {
        runtime.tick(&stop.token()).unwrap();
        handle.snapshot().jobs.first().is_some_and(|job| {
            job.record
                .providers
                .iter()
                .all(|provider| provider.state == ProviderState::Succeeded)
        })
    });
    let record = handle.snapshot().jobs[0].record.clone();
    assert!(record.dps_report_result.is_some());
    assert!(record.wingman_upload_receipt.is_some());
    assert!(record.donbot_upload_receipt.is_some());
    assert!(record.twitch_delivery_receipt.is_some());
    drop(runtime);
    let history =
        JsonUploadHistoryStore::new(files(), fixture.addon().join("upload-history.json"), 10_000)
            .unwrap();
    assert_eq!(history.records().len(), 1);
    let persisted = &history.records()[0];
    assert!(persisted.file == record.file);
    assert_eq!(persisted.providers, record.providers);
    assert_eq!(persisted.dps_report_result, record.dps_report_result);
    assert_eq!(
        persisted.wingman_upload_receipt,
        record.wingman_upload_receipt
    );
    assert_eq!(
        persisted.donbot_upload_receipt,
        record.donbot_upload_receipt
    );
    assert_eq!(
        persisted.twitch_delivery_receipt,
        record.twitch_delivery_receipt
    );
    let mut restarted = fixture.runtime(http.clone());
    let handle = restarted.handle();
    handle.select_logs(LogSelectionMode::Last24Hours).unwrap();
    until(|| {
        restarted.tick(&stop.token()).unwrap();
        let snapshot = handle.snapshot();
        snapshot.root_available
            && snapshot.options.donbot.state == DonBotConfigurationState::Verified
            && snapshot.options.twitch.state == TwitchConnectionState::Connected
    });
    assert_eq!(handle.snapshot().jobs.len(), 1);
    // Allow enough observations to admit the unchanged archive if restored deduplication breaks.
    let observation_window = Instant::now()
        + Duration::from_millis(
            u64::from(settings.general.poll_interval_ms)
                * u64::from(settings.general.stability_observations + 1),
        );
    until(|| {
        restarted.tick(&stop.token()).unwrap();
        Instant::now() >= observation_window
    });
    assert_eq!(handle.snapshot().jobs.len(), 1);
    drop(restarted);
    let mut deliveries = http.deliveries.lock().unwrap().clone();
    deliveries.sort_unstable();
    assert_eq!(deliveries, ["donbot", "dps", "twitch", "wingman"]);
}
