use super::*;

impl TwitchAuthenticationController {
    pub(super) fn handle_result(
        &mut self,
        result: TwitchAuthenticationResult,
    ) -> Result<bool, Error> {
        let Some(pending) = &self.pending else {
            return Err(Error::StaleResult);
        };
        if pending.id != result.request_id {
            return Err(Error::StaleResult);
        }
        let phase = pending.phase;
        self.pending = None;
        if phase.operation() != result.operation {
            if phase == Phase::Revoke {
                self.finish_disconnect(Some(Error::InvalidResult))?;
            }
            return self.fail(Error::InvalidResult, phase == Phase::ValidateRotated);
        }
        match result.outcome {
            Err(error) => self.handle_failure(phase, error, result.retained)?,
            Ok(success) => {
                if result.retained.is_some() {
                    if phase == Phase::Revoke {
                        self.finish_disconnect(Some(Error::InvalidResult))?;
                    }
                    return self.fail(Error::InvalidResult, phase == Phase::ValidateRotated);
                }
                match (phase, success) {
                    (Phase::Start, TwitchAuthenticationSuccess::Started(device)) => {
                        self.accept_device(device)?
                    }
                    (Phase::Poll, TwitchAuthenticationSuccess::Pending { device_code }) => {
                        self.defer_poll(device_code, None)?
                    }
                    (Phase::Poll, TwitchAuthenticationSuccess::Granted(grant)) => {
                        self.authorization = None;
                        self.clear_public();
                        let credentials = match self.credentials(grant) {
                            Ok(value) => value,
                            Err(error) => return self.fail(error, false),
                        };
                        self.draft = Some(Draft {
                            credentials: Some(credentials),
                            identity: None,
                            rotated: false,
                        });
                        self.dispatch_credentials(Phase::ValidateInitial)?;
                    }
                    (
                        Phase::ValidateInitial
                        | Phase::ValidateSaved
                        | Phase::ValidateActive
                        | Phase::ValidateRotated,
                        TwitchAuthenticationSuccess::Validated {
                            credentials,
                            identity,
                        },
                    ) => self.accept_validation(credentials, identity)?,
                    (Phase::Refresh, TwitchAuthenticationSuccess::Refreshed(grant)) => {
                        self.accept_rotation(grant)?
                    }
                    (Phase::Revoke, TwitchAuthenticationSuccess::Revoked) => {
                        self.finish_disconnect(None)?
                    }
                    (Phase::Revoke, _) => self.finish_disconnect(Some(Error::InvalidResult))?,
                    _ => return self.fail(Error::InvalidResult, phase == Phase::ValidateRotated),
                }
            }
        }
        Ok(true)
    }
    fn accept_device(&mut self, device: TwitchDeviceAuthorization) -> Result<(), Error> {
        let uri_valid = device.verification_uri.len() <= 2048
            && device
                .verification_uri
                .strip_prefix("https://www.twitch.tv/activate")
                .is_some_and(|suffix| suffix.is_empty() || suffix.starts_with('?'))
            && !device.verification_uri.contains(['@', '#', '\\'])
            && device
                .verification_uri
                .bytes()
                .all(|byte| (0x21..=0x7e).contains(&byte));
        if !valid_twitch_token(&device.device_code)
            || device.user_code.is_empty()
            || device.user_code.len() > 64
            || device.user_code.bytes().any(|byte| byte.is_ascii_control())
            || !uri_valid
            || !duration_valid(device.expires_in)
            || device.polling_interval.is_zero()
            || device.polling_interval > Duration::from_secs(300)
        {
            return self.fail(Error::InvalidResult, false);
        }
        let now = self.clock.steady_now();
        let deadlines = (
            now.checked_add(device.expires_in),
            now.checked_add(device.polling_interval),
            self.clock.system_now().checked_add(device.expires_in),
        );
        let (Some(expires), Some(next_poll), Some(absolute)) = deadlines else {
            return self.fail(Error::InvalidResult, false);
        };
        self.authorization = Some(Authorization {
            code: Some(device.device_code),
            expires,
            next_poll,
            interval: device.polling_interval,
        });
        self.snapshot.state = State::AwaitingUser;
        self.snapshot.user_code = Some(device.user_code);
        self.snapshot.verification_uri = Some(device.verification_uri);
        self.snapshot.authorization_expires_at = Some(absolute);
        self.snapshot.diagnostic = None;
        self.bump();
        Ok(())
    }
    fn defer_poll(&mut self, code: SecretValue, retry: Option<Duration>) -> Result<(), Error> {
        if !valid_twitch_token(&code) {
            return self.fail(Error::InvalidResult, false);
        }
        let now = self.clock.steady_now();
        let Some(authorization) = &mut self.authorization else {
            return self.fail(Error::InvalidState, false);
        };
        let delay = retry
            .unwrap_or(authorization.interval)
            .max(authorization.interval);
        let Some(deadline) = now.checked_add(delay) else {
            return self.fail(Error::InvalidResult, false);
        };
        authorization.code = Some(code);
        authorization.next_poll = deadline;
        self.snapshot.state = State::AwaitingUser;
        self.snapshot.diagnostic = retry.map(|_| Error::AuthenticationFailed);
        self.bump();
        Ok(())
    }
    fn credentials(&self, grant: TwitchTokenGrant) -> Result<TwitchCredentialSet, Error> {
        if !valid_twitch_token(&grant.access_token)
            || !valid_twitch_token(&grant.refresh_token)
            || grant.scopes != [TWITCH_CHAT_SCOPE]
            || !duration_valid(grant.expires_in)
        {
            return Err(Error::InvalidResult);
        }
        Ok(TwitchCredentialSet {
            access_token: grant.access_token,
            refresh_token: grant.refresh_token,
            access_expires_at: self
                .clock
                .system_now()
                .checked_add(grant.expires_in)
                .ok_or(Error::InvalidResult)?,
        })
    }
    fn accept_validation(
        &mut self,
        mut credentials: TwitchCredentialSet,
        identity: TwitchValidatedIdentity,
    ) -> Result<(), Error> {
        let Some(draft) = &self.draft else {
            return self.fail(Error::InvalidState, false);
        };
        let rotated = draft.rotated;
        if draft
            .identity
            .as_ref()
            .is_some_and(|(user_id, _)| *user_id != identity.user_id)
        {
            return self.fail(Error::IdentityMismatch, true);
        }
        if !duration_valid(identity.expires_in) {
            return self.fail(Error::InvalidResult, true);
        }
        let Some(expiry) = self.clock.system_now().checked_add(identity.expires_in) else {
            return self.fail(Error::InvalidResult, true);
        };
        credentials.access_expires_at = expiry;
        let session = TwitchSession {
            credentials,
            user_id: identity.user_id,
            login: identity.login,
            scopes: identity.scopes,
        };
        if encode_twitch_session(&session).is_err() {
            return self.fail(Error::InvalidResult, true);
        }
        let Some(id) = self.transaction else {
            return self.fail(Error::InvalidState, false);
        };
        let schedule = match self.make_schedule(expiry) {
            Ok(schedule) => schedule,
            Err(error) => return self.fail(error, rotated),
        };
        let connected = match self.owner.commit(id, session) {
            Ok(connected) => connected,
            Err(error) => return self.fail(owner_error(error), rotated),
        };
        self.transaction = None;
        self.set_connected(connected, schedule);
        Ok(())
    }
    fn accept_rotation(&mut self, grant: TwitchTokenGrant) -> Result<(), Error> {
        let Some(draft) = &mut self.draft else {
            return self.fail(Error::InvalidState, true);
        };
        draft.rotated = true;
        let Some((user_id, login)) = draft.identity.clone() else {
            return self.fail(Error::InvalidState, true);
        };
        let credentials = match self.credentials(grant) {
            Ok(credentials) => credentials,
            Err(error) => return self.fail(error, true),
        };
        let session = TwitchSession {
            credentials,
            user_id,
            login,
            scopes: vec![TWITCH_CHAT_SCOPE.into()],
        };
        let Some(id) = self.transaction else {
            return self.fail(Error::InvalidState, true);
        };
        if let Err(error) = self.owner.persist_pending(id, &session) {
            return self.fail(owner_error(error), true);
        }
        let mut draft = Draft::from_session(session);
        draft.rotated = true;
        self.draft = Some(draft);
        self.dispatch_credentials(Phase::ValidateRotated)
    }
    fn handle_failure(
        &mut self,
        phase: Phase,
        error: TwitchError,
        retained: Option<TwitchAuthenticationSecret>,
    ) -> Result<(), Error> {
        if phase == Phase::Revoke {
            return self.finish_disconnect(Some(Error::AuthenticationFailed));
        }
        let mapped = match error.disposition {
            TwitchDisposition::Cancelled => Error::AuthenticationCancelled,
            TwitchDisposition::Reconnect => Error::ReconnectRequired,
            _ => Error::AuthenticationFailed,
        };
        if phase == Phase::Poll && error.disposition == TwitchDisposition::Retry {
            let Some(TwitchAuthenticationSecret::DeviceCode(code)) = retained else {
                return self.fail(Error::InvalidResult, false);
            };
            return self.defer_poll(code, Some(retry_delay(error.retry_after)));
        }
        if matches!(
            phase,
            Phase::ValidateInitial
                | Phase::ValidateSaved
                | Phase::ValidateActive
                | Phase::ValidateRotated
                | Phase::Refresh
        ) {
            let Some(TwitchAuthenticationSecret::Credentials(credentials)) = retained else {
                return self.fail(
                    Error::InvalidResult,
                    phase == Phase::Refresh || phase == Phase::ValidateRotated,
                );
            };
            let Some(draft) = &mut self.draft else {
                return self.fail(Error::InvalidState, false);
            };
            if !valid_twitch_token(&credentials.access_token)
                || !valid_twitch_token(&credentials.refresh_token)
            {
                return self.fail(Error::InvalidResult, true);
            }
            draft.credentials = Some(credentials);
            if error.disposition == TwitchDisposition::Retry {
                let Some(deadline) = self
                    .clock
                    .steady_now()
                    .checked_add(retry_delay(error.retry_after))
                else {
                    return self.fail(Error::InvalidResult, false);
                };
                self.deferred = Some((phase, deadline));
                self.snapshot.state = phase.state();
                self.snapshot.diagnostic = Some(mapped);
                self.bump();
                return Ok(());
            }
            if error.disposition == TwitchDisposition::Reconnect
                && matches!(phase, Phase::ValidateSaved | Phase::ValidateActive)
            {
                return self.dispatch_credentials(Phase::Refresh);
            }
        }
        self.fail(
            mapped,
            matches!(phase, Phase::Refresh | Phase::ValidateRotated),
        )
    }
}
fn duration_valid(duration: Duration) -> bool {
    !duration.is_zero() && duration <= Duration::from_secs(604800)
}
fn retry_delay(delay: Option<Duration>) -> Duration {
    delay
        .filter(|value| !value.is_zero() && *value <= Duration::from_secs(86400))
        .unwrap_or(Duration::from_secs(30))
}
