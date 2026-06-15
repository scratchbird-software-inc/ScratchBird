# Governance, Quotas, and Audit

**DRAFT — Early Beta documentation. Subject to revision.**

## Purpose

This chapter describes the governance controls that are implemented in the
current early-beta ScratchBird AI component. These controls are real and
active. They do not imply third-party approval workflow orchestration,
PKI-backed signing infrastructure, or automatic certification for runtime
modes that are not currently exposed.

Topics covered:

- Safe-by-default read-only mode versus approval-gated mutation
- Bounded compile-repair
- Durable approval evidence ledger
- Audit bundles and attestation
- In-process quotas, rate limits, and cost attribution
- HTTP retry and circuit-breaker behavior
- Fail-closed compatibility pinning

---

## Safe-by-Default and Approval-Gated Mutation

The layer operates in read-only mode by default. Read operations (`run_query`,
`execute_readonly_query`, `explain_query`, retrieval tools) proceed without an
approval token.

Mutation operations (`execute_mutation`, `run_mutation`) require:

1. An `approval_token` or `approval_evidence` object.
2. The token to be valid in the durable approval ledger: not expired, not
   revoked, matching the correct tenant, actor, and statement hash.

If these conditions are not met, the mutation call returns `E_APPROVAL_INVALID`
and does not execute.

---

## Bounded Compile-Repair

Environment variable: `SCRATCHBIRD_AI_COMPILE_REPAIR_MAX_ATTEMPTS`

Before giving up on a compile failure, the layer can attempt a bounded set of
deterministic repairs on query text:

1. Original query text (no repair)
2. Trimmed whitespace
3. Stripped markdown code fence
4. Stripped leading `sql:` / `query:` label
5. Stripped markdown code fence plus leading label

Compile-repair is for recoverable wrapper noise only. It does not rewrite
query semantics. If the query itself is malformed, repair will not help and
the compile failure is returned to the caller.

---

## Durable Approval Evidence Ledger

Environment variable: `SCRATCHBIRD_AI_APPROVAL_LEDGER_PATH`

Default path: `${SCRATCHBIRD_AI_RUNTIME_ROOT}/approval_ledger.json`, or the
platform user state runtime directory when `SCRATCHBIRD_AI_RUNTIME_ROOT` is
not set.

The approval ledger is a file-backed, append-only store of approval records.
Each approval record contains:

| Field | Purpose |
| --- | --- |
| `approval_id` | Stable, deterministic record identifier |
| `approval_token_hash` | Hash of the approval token |
| `tenant_id` | Tenant scope for the approval |
| `actor_id` | Identity of the approving actor |
| `statement_hash` | Hash of the approved statement |
| `approved_by` | Identity of the approver |
| `approved_at` | Approval timestamp |
| `expires_at` | Expiry timestamp |
| `revoked_at` | Revocation timestamp (null if not revoked) |
| `revoked_by` | Revoking identity (null if not revoked) |
| `revocation_reason` | Free-text revocation reason |
| `last_used_at` | Timestamp of most recent valid use |
| `use_count` | Count of successful reuse events |

Reuse validation checks all of: approval token hash, tenant identity, actor
identity, and statement hash. Expired or revoked approvals fail closed.
Successful reuse updates `last_used_at` and `use_count`.

Operator tools for the approval ledger:

| Tool | Purpose |
| --- | --- |
| `list_approval_records` | List records, filterable by tenant and actor |
| `revoke_approval_record` | Administratively revoke a record by ID |
| `validate_approval_evidence` | Validate an evidence object without executing |

---

## Structured Event Logging and Runbook Packaging

Environment variables:

- `SCRATCHBIRD_AI_STRUCTURED_EVENT_LOG_PATH`
- `SCRATCHBIRD_AI_OPERATOR_BUNDLE_OUTPUT_DIR`

Default paths: `${SCRATCHBIRD_AI_RUNTIME_ROOT}/structured_events.jsonl` and
`${SCRATCHBIRD_AI_RUNTIME_ROOT}/operator_bundle/` respectively.

Tool invocations and direct query execution outcomes emit structured JSONL
events to the event log. Event summaries include counts, error rates, latency
statistics, and recent failures.

Operator runbook bundles write the following files to the bundle output
directory:

| File | Contents |
| --- | --- |
| `summary.json` | High-level summary of current state |
| `runtime_diagnostics.json` | Detailed runtime diagnostics |
| `slo_report.json` | SLO/error budget report |
| `recent_errors.json` | Recent error events |
| `approval_summary.json` | Approval ledger summary |
| `certification_manifest.json` | Current certification manifest |

Use `generate_operator_runbook_bundle` to produce a bundle, and
`get_runtime_diagnostics` for an immediate in-memory summary.

---

## Audit Bundles and Attestation

Environment variables:

- `SCRATCHBIRD_AI_AUDIT_ATTESTATION_MODE` — `hmac_sha256` or `external_reference`
- `SCRATCHBIRD_AI_AUDIT_ATTESTATION_SECRET` — shared secret for HMAC mode
- `SCRATCHBIRD_AI_AUDIT_ATTESTATION_ATTESTOR_ID` — attestor identity for the attestation record

Audit bundles are deterministic, replayable records of a tool invocation's
policy decision, plan hash, execution result, and security context. Key
properties:

- Equivalent audit bundles hash identically.
- Replay detects tamper (payload modification) and policy mismatch
  (re-evaluation against the current policy produces a different decision).

Attestation modes:

| Mode | Behavior |
| --- | --- |
| `hmac_sha256` | A HMAC-SHA256 over the bundle payload using the configured shared secret. Default when a shared secret is configured. |
| `external_reference` | An external governance system owns the final attestation record. The field carries an external URI or identifier. |

Audit tools:

| Tool | Purpose |
| --- | --- |
| `list_audit_bundles` | List stored bundles |
| `replay_audit_bundle` | Replay a bundle; verify expected policy decision, rule ID, and plan hash |
| `create_audit_attestation` | Issue a new attestation for a bundle |
| `verify_audit_attestation` | Verify a previously issued attestation |

---

## In-Process Quotas, Rate Limits, and Cost Attribution

Environment variables:

| Variable | Purpose |
| --- | --- |
| `SCRATCHBIRD_AI_OPERATION_WINDOW_SEC` | Rolling window size in seconds |
| `SCRATCHBIRD_AI_MAX_REQUESTS_PER_WINDOW` | Maximum requests per window |
| `SCRATCHBIRD_AI_MAX_MUTATIONS_PER_WINDOW` | Maximum mutation operations per window |
| `SCRATCHBIRD_AI_MAX_COST_UNITS_PER_WINDOW` | Maximum cost units per window |

Enforcement is per `tenant_id` and per `actor_id` within each active time
window.

When a budget is exceeded, the operation returns `E_LIMIT_EXCEEDED` with a
policy rule ID:

| Limit | Error code | Rule ID |
| --- | --- | --- |
| Request budget | `E_LIMIT_EXCEEDED` | `OPS-RATE-001` |
| Mutation budget | `E_LIMIT_EXCEEDED` | `OPS-MUTATION-001` |
| Cost budget | `E_LIMIT_EXCEEDED` | `OPS-COST-001` |

Cost is estimated from the bounded request options plus a mutation uplift. The
cost model is intentionally conservative in the current release; it is not a
production billing system.

---

## HTTP Retry and Circuit Breaker

Environment variables:

| Variable | Purpose |
| --- | --- |
| `SCRATCHBIRD_AI_HTTP_TIMEOUT_SEC` | Timeout for HTTP adapter requests |
| `SCRATCHBIRD_AI_HTTP_RETRY_ATTEMPTS` | Number of retry attempts for retryable requests |
| `SCRATCHBIRD_AI_HTTP_RETRY_BACKOFF_MS` | Backoff between retries in milliseconds |
| `SCRATCHBIRD_AI_HTTP_CIRCUIT_BREAKER_FAILURE_THRESHOLD` | Consecutive failures before the circuit opens |
| `SCRATCHBIRD_AI_HTTP_CIRCUIT_BREAKER_COOLDOWN_SEC` | Cooldown before the circuit closes again |

Retry policy:

- Retries apply only to retryable bridge requests.
- Retryable requests: `GET` and compile endpoints.
- Execute and mutation endpoints are not retried automatically.

Circuit-breaker policy:

- Consecutive failures open the circuit.
- The circuit remains open until the cooldown period elapses.
- Open-circuit requests fail closed immediately instead of sending further
  requests to the bridge.

---

## Fail-Closed Compatibility Pinning

Environment variables:

| Variable | Purpose |
| --- | --- |
| `SCRATCHBIRD_AI_SUPPORTED_SERVER_VERSIONS` | Accepted ScratchBird server version strings |
| `SCRATCHBIRD_AI_SUPPORTED_PARSER_COMPILER_VERSIONS` | Accepted parser/compiler version strings |
| `SCRATCHBIRD_AI_SUPPORTED_DRIVER_RUNTIME_VERSIONS` | Accepted local/bridge driver runtime version strings |

When these variables are set, the compatibility negotiation path rejects any
declared version that is outside the configured set. Unknown interface profiles
and unsupported transports also fail closed.

Error codes:

| Condition | Error code |
| --- | --- |
| Unsupported server version | `E_SERVER_RUNTIME_UNSUPPORTED` |
| Unsupported component version | `E_COMPONENT_VERSION_UNSUPPORTED` |
| Unsupported driver runtime | `E_DRIVER_RUNTIME_UNSUPPORTED` |
| Unsupported interface profile | `E_INTERFACE_PROFILE_UNSUPPORTED` |
| Unsupported transport | `E_TRANSPORT_PROFILE_UNSUPPORTED` |

Use these pins when you want the AI layer to reject environments outside the
validated release window rather than silently accepting them. Do not suppress
these checks unless the environment is genuinely inside the supported window.

---

## What These Controls Do Not Provide

- Third-party approval workflow orchestration.
- PKI-backed or externally notarized signing infrastructure. The shipped
  attestation path uses HMAC-SHA256 or an external reference pointer.
- Automatic certification for runtime modes that are not currently exposed in
  the active test environment.
- A production billing or metering system. Cost attribution is an in-process
  estimate for governance guardrails, not an external billing record.
