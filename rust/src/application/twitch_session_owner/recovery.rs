use super::*;

impl TwitchDeliverySessionAccess for TwitchSessionOwner {
    fn acquire(
        &self,
        stop: &StopToken,
    ) -> Result<TwitchDeliverySession, TwitchDeliverySessionError> {
        if stop.is_stop_requested() {
            return Err(delivery_error(TwitchDeliverySessionErrorCode::Cancelled));
        }
        let state = self.lock()?;
        if state.stopping {
            return Err(delivery_error(TwitchDeliverySessionErrorCode::Cancelled));
        }
        if state.pending.is_some() {
            return Err(delivery_error(TwitchDeliverySessionErrorCode::Retry));
        }
        let session = state
            .active
            .as_ref()
            .ok_or_else(|| delivery_error(TwitchDeliverySessionErrorCode::NotConnected))?;
        Ok(lease(session, state.revision))
    }
    fn recover(
        &self,
        rejected: TwitchDeliverySession,
        stop: &StopToken,
    ) -> Result<TwitchDeliverySession, TwitchDeliverySessionError> {
        if stop.is_stop_requested() {
            return Err(delivery_error(TwitchDeliverySessionErrorCode::Cancelled));
        }
        let transaction = {
            let mut state = self.lock()?;
            state.idle()?;
            let active = state
                .active
                .as_ref()
                .ok_or_else(|| delivery_error(TwitchDeliverySessionErrorCode::NotConnected))?;
            if rejected.revision == 0
                || rejected.revision > state.revision
                || rejected.authenticated_user_id != active.user_id
            {
                return Err(delivery_error(
                    TwitchDeliverySessionErrorCode::ReconnectRequired,
                ));
            }
            if rejected.revision < state.revision {
                return Ok(lease(active, state.revision));
            }
            if rejected.access_token.expose() != active.credentials.access_token.expose() {
                return Err(delivery_error(
                    TwitchDeliverySessionErrorCode::ReconnectRequired,
                ));
            }
            let session = state.active.take().expect("active session");
            state.start(session, true)
        };
        let id = transaction.id;
        drop(rejected);
        catch_unwind(AssertUnwindSafe(|| self.recover_current(transaction, stop))).unwrap_or_else(
            |_| {
                let _ = self.discard(id, false);
                Err(delivery_error(TwitchDeliverySessionErrorCode::Failed))
            },
        )
    }
}

impl TwitchSessionOwner {
    fn check_recovery(&self, id: u64, stop: &StopToken) -> Result<(), TwitchDeliverySessionError> {
        if stop.is_stop_requested() {
            return Err(delivery_error(TwitchDeliverySessionErrorCode::Cancelled));
        }
        self.lock()?.check(id)?;
        Ok(())
    }
    fn apply_identity(
        &self,
        session: &mut TwitchSession,
        identity: TwitchValidatedIdentity,
    ) -> Result<(), TwitchDeliverySessionError> {
        if identity.user_id != session.user_id {
            return Err(delivery_error(
                TwitchDeliverySessionErrorCode::ReconnectRequired,
            ));
        }
        session.credentials.access_expires_at = self.expiry(identity.expires_in)?;
        session.login = identity.login;
        session.scopes = identity.scopes;
        // Ports may be fakes or other adapters. Validate before any protected replacement.
        encode_twitch_session(session)
            .map_err(|_| delivery_error(TwitchDeliverySessionErrorCode::ReconnectRequired))?;
        Ok(())
    }
    fn expiry(&self, duration: Duration) -> Result<SystemTime, TwitchDeliverySessionError> {
        if duration.is_zero() || duration > Duration::from_secs(604800) {
            return Err(delivery_error(
                TwitchDeliverySessionErrorCode::ReconnectRequired,
            ));
        }
        self.clock
            .system_now()
            .checked_add(duration)
            .ok_or_else(|| delivery_error(TwitchDeliverySessionErrorCode::ReconnectRequired))
    }
    fn end_failed_recovery(
        &self,
        transaction: TwitchSessionTransaction,
        error: TwitchDeliverySessionError,
        restore: bool,
        erase: bool,
    ) -> Result<TwitchDeliverySession, TwitchDeliverySessionError> {
        let closed = if restore {
            self.restore(transaction.id, transaction.session)
        } else {
            self.discard(transaction.id, erase)
        };
        if let Err(close_error) = closed {
            return Err(close_error.into());
        }
        Err(error)
    }
    fn recover_current(
        &self,
        mut transaction: TwitchSessionTransaction,
        stop: &StopToken,
    ) -> Result<TwitchDeliverySession, TwitchDeliverySessionError> {
        if let Err(error) = self.check_recovery(transaction.id, stop) {
            return self.end_failed_recovery(transaction, error, true, false);
        }
        let validated = self
            .client
            .validate_access_token(&transaction.session.credentials.access_token, stop);
        if let Err(error) = self.check_recovery(transaction.id, stop) {
            return self.end_failed_recovery(transaction, error, true, false);
        }
        match validated {
            Ok(identity) => {
                if let Err(error) = self.apply_identity(&mut transaction.session, identity) {
                    return self.end_failed_recovery(transaction, error, false, true);
                }
                return self.finish_recovery(transaction, stop);
            }
            Err(error) if error.disposition != TwitchDisposition::Reconnect => {
                let restore = matches!(
                    error.disposition,
                    TwitchDisposition::Retry | TwitchDisposition::Cancelled
                );
                return self.end_failed_recovery(transaction, error.into(), restore, false);
            }
            Err(_) => (),
        }
        if let Err(error) = self.check_recovery(transaction.id, stop) {
            return self.end_failed_recovery(transaction, error, true, false);
        }
        let refreshed = self
            .client
            .refresh_access_token(&transaction.session.credentials.refresh_token, stop);
        let grant = match refreshed {
            Ok(grant) => grant,
            Err(error) => {
                let restore = matches!(
                    error.disposition,
                    TwitchDisposition::Retry | TwitchDisposition::Cancelled
                );
                return self.end_failed_recovery(transaction, error.into(), restore, !restore);
            }
        };
        // The server may already have invalidated the old refresh token. Never restore it now.
        if let Err(error) = self.check_recovery(transaction.id, stop) {
            return self.end_failed_recovery(transaction, error, false, true);
        }
        let expiry = match self.expiry(grant.expires_in) {
            Ok(expiry) => expiry,
            Err(error) => return self.end_failed_recovery(transaction, error, false, true),
        };
        transaction.session.credentials = TwitchCredentialSet {
            access_token: grant.access_token,
            refresh_token: grant.refresh_token,
            access_expires_at: expiry,
        };
        transaction.session.scopes = grant.scopes;
        if let Err(error) = self.persist_pending(transaction.id, &transaction.session) {
            return self.end_failed_recovery(transaction, error.into(), false, false);
        }
        if let Err(error) = self.check_recovery(transaction.id, stop) {
            return self.end_failed_recovery(transaction, error, false, true);
        }
        let replacement = self
            .client
            .validate_access_token(&transaction.session.credentials.access_token, stop);
        if let Err(error) = self.check_recovery(transaction.id, stop) {
            return self.end_failed_recovery(transaction, error, false, true);
        }
        let identity = match replacement {
            Ok(identity) => identity,
            Err(error) => return self.end_failed_recovery(transaction, error.into(), false, true),
        };
        if let Err(error) = self.apply_identity(&mut transaction.session, identity) {
            return self.end_failed_recovery(transaction, error, false, true);
        }
        self.finish_recovery(transaction, stop)
    }
    fn finish_recovery(
        &self,
        transaction: TwitchSessionTransaction,
        stop: &StopToken,
    ) -> Result<TwitchDeliverySession, TwitchDeliverySessionError> {
        let id = transaction.id;
        if let Err(error) = self.commit(id, transaction.session) {
            let _ = self.discard(id, false);
            return Err(error.into());
        }
        self.acquire(stop)
    }
}
