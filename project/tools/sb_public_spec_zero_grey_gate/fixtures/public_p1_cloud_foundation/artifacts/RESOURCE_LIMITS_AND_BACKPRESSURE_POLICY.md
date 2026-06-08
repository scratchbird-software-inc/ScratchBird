# Resource Limits and Backpressure Policy

Search key: `PUBLIC_P1_CLOUD_FOUNDATION_RESOURCE_LIMITS_AND_BACKPRESSURE_POLICY`

The target features add high-fanout and retryable surfaces that need bounded
resource behavior.

Required limits:

- maximum protected material versions per material before retention enforcement
- maximum protected material release attempts per session/transaction
- maximum KMS retries and retry backoff window
- maximum edge cache tags per object
- maximum invalidation fanout per transaction
- maximum outbox pending records before backpressure
- maximum operator reconcile attempts and conflict retries
- maximum audit/metric event volume per transaction

Required behavior:

- Exceeded limits fail with deterministic diagnostics.
- Backpressure does not roll back already committed engine transactions unless
  the external-effect outbox admission itself is part of the transaction and
  fails before commit.
- Metrics report throttling/refusal counts with bounded labels.
- Tests cover denial, retry window, and no unbounded memory growth.
