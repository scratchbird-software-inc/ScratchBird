# Runtime Configuration and Governed Operation

Status: Active
Last Updated: 2026-04-20

## 1. Purpose

Document the AI-local runtime controls that are already implemented in the
ScratchBird AI component and should be treated as the current operator-facing
baseline.

This guide covers:

- bounded compile-repair
- structured runtime JSONL event logging
- durable local approval evidence
- approval-record operator workflow
- audit bundle attestation issue/verify flow
- runtime diagnostics plus operator runbook/SLO bundle generation
- in-process quotas, rate limits, and cost attribution
- HTTP retry and circuit-breaker behavior
- fail-closed compatibility pinning

It does not widen the supported-product claim beyond the current early-beta
surface.

## 2. Current Boundary

Implemented and supportable now inside the ScratchBird AI component:

- bounded compile-repair for recoverable wrapper noise
- structured append-only runtime event logging
- durable file-backed approval ledger
- approval record listing and administrative revocation
- audit bundle listing, replay, and attestation verification
- local HMAC or external-reference audit attestation issuance
- runtime diagnostics and operator runbook/SLO bundle generation
- request, mutation, and cost-window enforcement
- HTTP retry and circuit-breaker controls
- fail-closed compatibility checks for declared ScratchBird server, parser, and
  driver/runtime versions

Also completed in ScratchBird core and now published by this repository:

- bounded engine-owned retrieval truth for vector, HNSW/ANN, and full-text
- bounded retrieval metadata/discovery truth through `opensearch_meta.*`
- bounded runtime-mode truth for `listener_direct`, `manager_proxy`,
  `local_ipc`, and `embedded_local_only`

Still environment-dependent here:

- rerunning live-native certification when the target test environment is
  refreshed
- executing additional live runs for runtime modes that are not currently
  exposed by the active shared harness

## 3. Compile-Repair Controls

Environment variable:

- `SCRATCHBIRD_AI_COMPILE_REPAIR_MAX_ATTEMPTS`

Current repair strategies are deterministic and bounded:

- original query text
- trimmed whitespace
- stripped markdown code fence
- stripped leading `sql:` / `query:` label
- stripped markdown code fence plus leading label

Compile-repair is for recoverable wrapper noise only. It is not a free-form SQL
rewrite system.

## 4. Durable Approval Ledger

Environment variable:

- `SCRATCHBIRD_AI_APPROVAL_LEDGER_PATH`

Default path:

- `${SCRATCHBIRD_AI_RUNTIME_ROOT}/approval_ledger.json`, or the platform user
  state runtime directory when `SCRATCHBIRD_AI_RUNTIME_ROOT` is not set

Current ledger behavior:

- approval records are persisted locally
- approval identifiers are stable and deterministic when not supplied
- reuse is validated against:
  - approval token hash
  - tenant identity
  - actor identity
  - statement hash
- expiry and revocation fail closed
- successful reuse updates `last_used_at` and `use_count`

Current ledger fields include:

- `approval_id`
- `approval_token_hash`
- `tenant_id`
- `actor_id`
- `statement_hash`
- `approved_by`
- `approved_at`
- `expires_at`
- `revoked_at`
- `revoked_by`
- `revocation_reason`
- `last_used_at`
- `use_count`

Operator-facing governance tools now cover:

- `list_approval_records`
- `revoke_approval_record`
- `validate_approval_evidence`

This remains local governed evidence. It is not a third-party workflow system.

## 5. Structured Event Logging And Runbook Packaging

Environment variables:

- `SCRATCHBIRD_AI_STRUCTURED_EVENT_LOG_PATH`
- `SCRATCHBIRD_AI_OPERATOR_BUNDLE_OUTPUT_DIR`

Default paths:

- `${SCRATCHBIRD_AI_RUNTIME_ROOT}/structured_events.jsonl`, or the platform user
  state runtime directory when `SCRATCHBIRD_AI_RUNTIME_ROOT` is not set
- `${SCRATCHBIRD_AI_RUNTIME_ROOT}/operator_bundle/`, or the platform user state
  runtime directory when `SCRATCHBIRD_AI_RUNTIME_ROOT` is not set

Current behavior:

- tool invocations and direct query execution outcomes emit structured JSONL
  events
- event summaries include counts, error rates, latency, and recent failures
- operator runbook bundles write:
  - `summary.json`
  - `runtime_diagnostics.json`
  - `slo_report.json`
  - `recent_errors.json`
  - `approval_summary.json`
  - `certification_manifest.json`

Current operator/admin tools:

- `get_runtime_diagnostics`
- `generate_operator_runbook_bundle`

## 6. Audit Attestation

Environment variables:

- `SCRATCHBIRD_AI_AUDIT_ATTESTATION_MODE`
- `SCRATCHBIRD_AI_AUDIT_ATTESTATION_SECRET`
- `SCRATCHBIRD_AI_AUDIT_ATTESTATION_ATTESTOR_ID`

Current supported modes:

- `hmac_sha256`
- `external_reference`

Current operator/admin tools:

- `list_audit_bundles`
- `replay_audit_bundle`
- `create_audit_attestation`
- `verify_audit_attestation`

The default local path is HMAC-backed attestation when a shared secret is
configured. `external_reference` can be used when an external governance system
or signing process owns the final attestation record.

## 7. Operational Control Windows

Environment variables:

- `SCRATCHBIRD_AI_OPERATION_WINDOW_SEC`
- `SCRATCHBIRD_AI_MAX_REQUESTS_PER_WINDOW`
- `SCRATCHBIRD_AI_MAX_MUTATIONS_PER_WINDOW`
- `SCRATCHBIRD_AI_MAX_COST_UNITS_PER_WINDOW`

Current enforcement key:

- per `tenant_id`
- per `actor_id`
- per active time window

Current failure policy:

- request budget exceeded -> `E_LIMIT_EXCEEDED` / `OPS-RATE-001`
- mutation budget exceeded -> `E_LIMIT_EXCEEDED` / `OPS-MUTATION-001`
- cost budget exceeded -> `E_LIMIT_EXCEEDED` / `OPS-COST-001`

Cost is estimated from the bounded request options plus a mutation uplift.

## 8. HTTP Retry and Circuit Breaker

Environment variables:

- `SCRATCHBIRD_AI_HTTP_TIMEOUT_SEC`
- `SCRATCHBIRD_AI_HTTP_RETRY_ATTEMPTS`
- `SCRATCHBIRD_AI_HTTP_RETRY_BACKOFF_MS`
- `SCRATCHBIRD_AI_HTTP_CIRCUIT_BREAKER_FAILURE_THRESHOLD`
- `SCRATCHBIRD_AI_HTTP_CIRCUIT_BREAKER_COOLDOWN_SEC`

Current retry policy:

- retries apply only to retryable bridge requests
- current retryable requests are:
  - `GET`
  - compile endpoints

Current circuit-breaker policy:

- consecutive failures open the circuit
- the circuit remains open until cooldown elapses
- open-circuit requests fail closed instead of hammering the bridge

## 9. Compatibility Pinning

Environment variables:

- `SCRATCHBIRD_AI_SUPPORTED_SERVER_VERSIONS`
- `SCRATCHBIRD_AI_SUPPORTED_PARSER_COMPILER_VERSIONS`
- `SCRATCHBIRD_AI_SUPPORTED_DRIVER_RUNTIME_VERSIONS`

Current policy:

- declared unsupported versions fail closed
- unknown requested interface profiles fail closed
- unsupported transports fail closed
- claims must stay aligned with the generated compatibility manifest and release
  evidence

Use these pins when you want the AI layer to reject environments outside the
validated release window instead of silently accepting them.

## 10. Example Development Baseline

```bash
export SCRATCHBIRD_AI_ADAPTER_MODE=http
export SCRATCHBIRD_AI_HTTP_BASE_URL=http://127.0.0.1:3095
export SCRATCHBIRD_AI_HTTP_DIALECTS=native
export SCRATCHBIRD_AI_APPROVAL_LEDGER_PATH=artifacts/runtime/approval_ledger.json
export SCRATCHBIRD_AI_STRUCTURED_EVENT_LOG_PATH=artifacts/runtime/structured_events.jsonl
export SCRATCHBIRD_AI_OPERATOR_BUNDLE_OUTPUT_DIR=artifacts/runtime/operator_bundle
export SCRATCHBIRD_AI_AUDIT_ATTESTATION_MODE=hmac_sha256
export SCRATCHBIRD_AI_AUDIT_ATTESTATION_SECRET=replace-with-local-attestation-secret
export SCRATCHBIRD_AI_COMPILE_REPAIR_MAX_ATTEMPTS=3
export SCRATCHBIRD_AI_OPERATION_WINDOW_SEC=60
export SCRATCHBIRD_AI_MAX_REQUESTS_PER_WINDOW=100
export SCRATCHBIRD_AI_MAX_MUTATIONS_PER_WINDOW=20
export SCRATCHBIRD_AI_MAX_COST_UNITS_PER_WINDOW=1000
export SCRATCHBIRD_AI_HTTP_RETRY_ATTEMPTS=1
export SCRATCHBIRD_AI_HTTP_RETRY_BACKOFF_MS=100
export SCRATCHBIRD_AI_HTTP_CIRCUIT_BREAKER_FAILURE_THRESHOLD=3
export SCRATCHBIRD_AI_HTTP_CIRCUIT_BREAKER_COOLDOWN_SEC=30
```

## 11. Release-Truth Note

These controls are real and implemented now.

They do not by themselves imply:

- third-party approval workflow orchestration
- PKI-backed or externally notarized signing infrastructure
- automatic certification for runtime modes that are not currently exposed in
  the active environment
