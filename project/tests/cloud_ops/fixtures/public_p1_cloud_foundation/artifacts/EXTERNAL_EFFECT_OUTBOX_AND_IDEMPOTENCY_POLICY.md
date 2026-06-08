# External Effect Outbox and Idempotency Policy

Search key: `PUBLIC_P1_CLOUD_FOUNDATION_EXTERNAL_EFFECT_OUTBOX_AND_IDEMPOTENCY_POLICY`

Protected material release, KMS release, operator reconcile, and CDN
invalidation are external effects when they leave the engine boundary. They
must be driven by committed engine state but never become transaction authority.

Required outbox fields:

- outbox event UUID
- source transaction UUID/local transaction ID
- source object UUID
- effect class
- provider profile UUID
- idempotency key
- redaction policy UUID
- payload hash
- attempt count
- next retry time
- final state
- audit event UUID

Required behavior:

- Outbox records are inserted only after MGA commit visibility.
- Rollback creates no external effect record.
- Retry uses idempotency keys and never duplicates successful external effects.
- Provider delivery failure leaves the engine transaction committed but the
  outbox item pending/failed with audit and metrics.
- Reconciliation can resume pending effects after clean shutdown/reopen.
- External provider acknowledgement does not prove engine commit.

Required tests:

- rollback emits no outbox event
- commit emits one event
- duplicate retry is deduplicated
- provider failure is audited and retryable where policy allows
- restart resumes pending event without duplicate final delivery
