use super::upload_worker::{Secrets, load_key, normalize_options};
use super::*;
use crate::domain::DonBotDiscordDeliveryOutcome;
use crate::providers::{WorkerError, WorkerOptions, WorkerSnapshot};
use crate::workers::{Processor, QueueError, WorkerPool};
use std::collections::HashSet;

struct Captured {
    request: DonBotAggregateRequest,
    options: DonBotOptions,
    dispatched: bool,
}
struct Deliver {
    client: Arc<dyn DonBotUploader>,
    secrets: Secrets,
}
impl Captured {
    fn result(&self, outcome: DonBotAggregateOutcome) -> DonBotAggregateResult {
        DonBotAggregateResult {
            request_id: self.request.request_id,
            outcome,
            fight_log_count: self.request.fight_log_ids.len(),
            discord_delivery: None,
            credential_failure: None,
        }
    }
}
impl Processor for Deliver {
    type Request = Captured;
    type Output = DonBotAggregateResult;
    fn process(&self, request: &mut Captured, stop: &StopToken) -> DonBotAggregateResult {
        use DonBotAggregateOutcome::*;
        if stop.is_stop_requested() {
            return request.result(Cancelled);
        }
        let key = match load_key(&self.secrets) {
            Ok(key) => key,
            Err(reason) => {
                return DonBotAggregateResult {
                    credential_failure: Some(reason),
                    ..request.result(Failed)
                };
            }
        };
        if stop.is_stop_requested() {
            return request.result(Cancelled);
        }
        request.dispatched = true;
        let result = self.client.deliver_aggregate(
            &request.request.fight_log_ids,
            &request.options,
            &key,
            stop,
        );
        if stop.is_stop_requested() {
            return request.result(Ambiguous);
        }
        match result {
            Err(error) => {
                let uncertain = error.disposition != DonBotDisposition::Failed
                    || error.http_error.is_some()
                    || error
                        .http_status
                        .is_some_and(|status| matches!(status, 200 | 408 | 429) || status >= 500)
                    || error.code == DonBotErrorCode::Internal;
                request.result(if uncertain { Ambiguous } else { Failed })
            }
            Ok(success) => {
                let receipt = success.discord_delivery;
                if success.fight_log_count != request.request.fight_log_ids.len()
                    || !receipt.is_valid_with_limit(MAX_DONBOT_AGGREGATE_FIGHTS)
                    || receipt.outcome == DonBotDiscordDeliveryOutcome::NotRequested
                {
                    return request.result(Ambiguous);
                }
                let outcome = if receipt.ambiguous != 0 {
                    Ambiguous
                } else if receipt.sent != 0 {
                    Succeeded
                } else {
                    Failed
                };
                DonBotAggregateResult {
                    discord_delivery: Some(receipt),
                    ..request.result(outcome)
                }
            }
        }
    }
    fn failed(&self, request: &Captured) -> DonBotAggregateResult {
        request.result(if request.dispatched {
            DonBotAggregateOutcome::Ambiguous
        } else {
            DonBotAggregateOutcome::Failed
        })
    }
}
pub struct DonBotAggregateWorker {
    pool: WorkerPool<Deliver>,
}
impl DonBotAggregateWorker {
    pub fn new(client: Arc<dyn DonBotUploader>, secrets: Secrets) -> Result<Self, WorkerError> {
        Ok(Self {
            pool: WorkerPool::new(
                Deliver { client, secrets },
                WorkerOptions {
                    queue_capacity: 1,
                    parallelism: 1,
                },
            )?,
        })
    }
    pub fn snapshot(&self) -> WorkerSnapshot {
        self.pool.snapshot()
    }
    pub fn wait_for_result(&mut self, timeout: Duration) -> Option<DonBotAggregateResult> {
        self.pool.wait_for_result(timeout)
    }
}
impl DonBotAggregateDelivery for DonBotAggregateWorker {
    fn enqueue(
        &mut self,
        mut request: DonBotAggregateRequest,
    ) -> Result<(), DonBotAggregateDispatchError> {
        let snapshot = self.pool.snapshot();
        if snapshot.stopping {
            return Err(DonBotAggregateDispatchError::Stopping);
        }
        if snapshot.active + snapshot.pending + snapshot.results != 0 {
            return Err(DonBotAggregateDispatchError::Busy);
        }
        let mut ids = HashSet::new();
        if request.request_id == 0
            || !(2..=usize::from(MAX_DONBOT_AGGREGATE_FIGHTS))
                .contains(&request.fight_log_ids.len())
            || request
                .fight_log_ids
                .iter()
                .any(|&id| !super::upload_response::positive_id(id) || !ids.insert(id))
            || !request.delivery.requested()
        {
            return Err(DonBotAggregateDispatchError::InvalidRequest);
        }
        let options = normalize_options(DonBotOptions {
            api_base_url: std::mem::take(&mut request.api_base_url),
            guild_id: std::mem::take(&mut request.guild_id),
            delivery: std::mem::take(&mut request.delivery),
        })
        .ok_or(DonBotAggregateDispatchError::InvalidRequest)?;
        self.pool
            .enqueue(Captured {
                request,
                options,
                dispatched: false,
            })
            .map_err(|error| match error {
                QueueError::Full => DonBotAggregateDispatchError::Busy,
                QueueError::Stopping => DonBotAggregateDispatchError::Stopping,
            })
    }
    fn try_take_result(&mut self) -> Option<DonBotAggregateResult> {
        self.pool.try_take_result()
    }
    fn busy(&self) -> bool {
        let snapshot = self.pool.snapshot();
        snapshot.active + snapshot.pending != 0
    }
    fn cancel_pending(&mut self) {
        self.pool.cancel_pending();
    }
}
