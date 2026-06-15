# Evidence, Explainability, and Safety

## Purpose

This page describes how ScratchBird makes agent decisions auditable, verifiable, and safe against tampering or replay. It covers the tamper-evident evidence chain and its key-management model, the dry-run and simulation mechanisms, the replay-quarantine system that handles compensation when actions must be revisited, the explainability output that operators can query for any decision, the fault-injection scenario registry used for resilience validation, and the safe-mode and quarantine lifecycle states that constrain compromised instances.

All struct and enum names are verified against `project/src/core/agents/agent_runtime.hpp`, `project/src/core/agents/agent_replay_quarantine.hpp`, and related headers.

---

## The Tamper-Evident Evidence Chain

Every agent decision that produces an observable outcome writes an `AgentEvidenceRecord`. The record is the authoritative audit trail for that decision. Its tamper-protection fields ensure that any post-hoc modification is detectable.

### Chain Structure

Each `AgentEvidenceRecord` carries:

| Field | Purpose |
|-------|---------|
| `tamper_digest` | SHA-256 hash of the record's content fields. |
| `previous_tamper_digest` | Digest of the immediately preceding evidence record for this agent instance, forming a chain. |
| `tamper_chain_digest` | Accumulated chain digest, binding this record into the sequence since instance creation. |
| `tamper_signature` | HMAC-SHA-256 signature over `tamper_chain_digest` using the current key. |
| `tamper_signature_algorithm` | Always `hmac-sha256-v1` (verified in source). |
| `tamper_key_id` | Identifier of the signing key in use. |
| `tamper_key_generation` | Monotonically increasing generation counter for key rotation. |
| `tamper_key_not_before_microseconds` | Epoch timestamp before which the key is not valid. |
| `tamper_key_not_after_microseconds` | Epoch timestamp after which the key is expired. |
| `tamper_key_rotation_epoch` | The rotation boundary this key belongs to. |
| `tamper_key_provenance` | Provenance descriptor for the signing key. |
| `evidence_key_policy_id` | Identifier of the key policy controlling this key's lifecycle. |
| `key_residency_class` | Where the key material resides (for compliance reporting). |
| `data_residency_class` | Data residency category of the evidence record. |
| `tamper_evidence_generation` | Monotone counter incremented on each tamper-chain update. |
| `storage_linkage_digest` | Digest linking this evidence record to the durable storage artifact it describes. |

A verifier examining a chain of evidence records can confirm:

1. Each record's `tamper_digest` matches its content.
2. Each record's `tamper_signature` validates against `tamper_chain_digest` using the key identified by `tamper_key_id` within its `not_before` / `not_after` window.
3. Each record's `previous_tamper_digest` matches the preceding record's `tamper_digest`.
4. The `tamper_chain_digest` accumulates correctly across the sequence.

Any gap or mismatch signals that one or more records were inserted, removed, or modified after creation.

### Key Rotation

Keys rotate on a generation boundary defined by `tamper_key_rotation_epoch`. The `tamper_key_generation` field distinguishes which key signed which records, allowing historic verification even after rotation. The `production_key_material` and `test_key_material` boolean flags distinguish production signing keys from test fixtures; `key_material_exported` is `false` for production keys by design.

### Redaction, Retention, and Legal Hold

Each evidence record carries three compliance-relevant fields:

- `redaction_class` — defaults to `standard`. Controls which fields are visible to which security roles. `RedactAgentEvidenceForSecurity` applies the class before the record leaves the engine.
- `retention_class` — defaults to `operational`. Informs the archive and cleanup agents how long to retain the record.
- `legal_hold_active` — when `true`, the record is exempt from all retention-driven deletion regardless of `retention_class`.

The `protected_material_suppressed` flag (verified in `AgentEvidenceRecord`) indicates that the record was created in a context where protected material was present but has been suppressed from the evidence payload per the applicable redaction policy. The record exists and is chained; its sensitive content is omitted.

`redaction_applied_before_buffering` records whether redaction was applied before the payload was passed to the evidence buffer, relevant for compliance scenarios where material must never appear in an intermediate store even transiently.

---

## Dry-Run Mode

Dry-run mode is a first-class operational posture, not a debug flag. When an agent's `AgentActivationProfile` is `dry_run`:

- The full action-decision pipeline executes (all safety checks, metric evaluations, policy gates, arbitration).
- `BuildDryRunDecision` is called; the resulting `AgentActionDecision` carries `result_class = dry_run_only`.
- An `AgentEvidenceRecord` is written with `result_state = "dry_run_only"` and a full tamper chain entry.
- No mutation is dispatched to the actuator.

This means dry-run evidence accumulates in the same chain as live evidence. Operators reviewing the chain after a transition to live can compare the decision inputs that dry-run produced against the inputs available when live execution later occurred.

The `AgentPolicy.require_dry_run_before_live` gate (described in [authority_and_activation_model.md](authority_and_activation_model.md)) enforces that at least one dry-run cycle appears in evidence history before live execution is permitted for policies that require it.

---

## Simulation and Decision Replay

`ReplayAgentDecision` re-executes the decision logic for a given agent type, policy, and context, using a captured set of metric families from a prior point in time. This allows operators to:

- Confirm that a past live decision would have been reached again with the same inputs (correctness oracle use case).
- Test what a policy change would have produced against a historical metric snapshot without modifying any live agent.

Replay does not write live evidence; it produces a `AgentRunDecision` that includes the action decision and explanation lines. Evidence generated during replay is not chained into the main evidence chain unless the replay is explicitly promoted to a quarantine-release operation.

---

## Replay Quarantine: Digest Capture, Compensation, and Release

The replay-quarantine subsystem in `agent_replay_quarantine.hpp` handles the case where an action needs to be revisited after execution — because the outcome was uncertain, the action failed mid-way, or a policy change occurred that renders the original decision invalid.

### Digest Capture

Before any live action with quarantine implications is dispatched, `CaptureAgentReplayDigests` records an `AgentReplayDigestCapture` snapshot. This snapshot includes digests for:

| Captured item | Field |
|---------------|-------|
| Active policy (at action time) | `policy_digest` |
| Metric input snapshot | `metric_digest` |
| Catalog root at decision time | `catalog_root_digest` |
| Security context | `security_digest` |
| Resource reservation state | `resource_reservation_digest` |
| Agent binary package identity | `binary_package_digest` |
| Action input parameters | `action_input_digest` |
| Action evidence record | `action_evidence_digest` |
| Full evidence chain link | `evidence_chain_digest` |

These digests allow a post-hoc reviewer (or the engine itself during recovery) to determine whether any input has changed since the original decision.

### Replay Operations

`ApplyAgentReplayControl` accepts an `AgentReplayControlRequest` with one of five `AgentReplayOperationKind` values:

| Operation | Token | What it does |
|-----------|-------|-------------|
| Mark replay pending | `mark_replay_pending` | Flags the action for review without changing its state. |
| Schedule retry | `schedule_retry` | Schedules a re-attempt after the configured `retry_after_microseconds`. |
| Record compensation | `record_compensation` | Records a compensating action that partially or fully reverses the original. |
| Quarantine | `quarantine` | Moves the action to quarantine; no further execution permitted until released. |
| Release quarantine | `release_quarantine` | Operator-authorized release from quarantine, allowing retry or closure. |

The `AgentReplayControlRequest` explicitly asserts that all authority flags (`parser_authority`, `client_authority`, `reference_authority`, `wal_authority`, etc.) are `false`. A replay operation that inadvertently asserts one of these flags is refused.

---

## Explainability Output

`ExplainAgentDecision(descriptor, policy, decision)` produces a list of human-readable explanation lines for any `AgentActionDecision`. The output describes:

- Which activation profile was in effect and how it constrained the decision.
- Which metric families were consulted and whether they were fresh, trusted, and present.
- Which policy fields influenced the outcome.
- Which arbitration rule resolved the competition (if multiple candidates were present).
- What the resource budget evaluation found.
- The final `AgentActionResultClass` and its reason.

The explanation is surfaced through `AgentRunDecision.explanation_lines` and is also accessible through the `sys.information.*` agent observability projections (see [observability_and_control.md](observability_and_control.md)).

Explanation output does not expose redacted evidence fields. A caller without the `obs_agent_evidence_read` right sees explanation lines that describe the decision category and reason code, but not the underlying metric values or policy content that would be subject to redaction.

---

## Fault Injection and the Scenario Registry

The fault-injection subsystem provides a structured set of scenarios for validating that the agent runtime behaves correctly when components fail. `AgentFaultInjectionScenarioDescriptors()` returns the full registry; `EvaluateAgentFaultInjectionScenarioDetailed` runs a named scenario and returns an `AgentFaultInjectionResult`.

### Fault Classes

`AgentFaultInjectionClass` defines six fault categories:

| Class | Token | What it simulates |
|-------|-------|------------------|
| Supervision | `supervision` | Watchdog timeout, tick timeout, or exception during a supervised run. |
| Storage I/O | `storage_io` | I/O failure during evidence persistence or catalog read. |
| Metric input | `metric_input` | Missing, stale, or schema-incompatible metric sample. |
| Policy input | `policy_input` | Missing, invalid, or scope-incompatible policy. |
| Queue integrity | `queue_integrity` | Corruption or gap in the action-queue state. |
| Partial action | `partial_action` | The action dispatched to the actuator but completed only partially. |

### Recovery Responses

Each scenario descriptor carries an `AgentFaultInjectionRecoveryResponse` that specifies the expected recovery behavior:

| Response | Token | What the engine does |
|----------|-------|---------------------|
| Fail closed | `fail_closed` | Default to no-mutation; record evidence; do not proceed. |
| Reject metric sample | `reject_metric_sample` | Discard the suspect metric observation; re-evaluate without it. |
| Reject policy | `reject_policy` | Discard the invalid policy record; use baseline or fail closed. |
| Supervision restart backoff | `supervision_restart_backoff` | Increment restart counter; apply exponential backoff before re-attempting. |
| Supervision quarantine | `supervision_quarantine` | Accumulate failure count; quarantine the instance when threshold is reached. |

The `AgentFaultInjectionResult` confirms whether the scenario produced the expected `AgentActionResultClass`, the expected `AgentLifecycleState` after the fault, and whether `durable_state_changed = false` (confirming no unsafe mutation occurred) and `evidence_recorded_before_success = true` (confirming the evidence was written before the action was reported as successful).

---

## Safe Mode

`safe_mode` is the `AgentLifecycleState` the engine supervisor assigns to an agent instance when a risk condition has been detected but the instance has not yet accumulated enough failures to warrant full quarantine. In safe mode:

- `ValidateAgentSafeMode` restricts the action scope to a reduced set of operations.
- The agent continues to run (observations and recommendations may continue).
- Live mutations are blocked.
- The safe-mode condition is recorded in the evidence chain.

Safe mode is designed to be transient. The supervisor can clear it when the risk condition resolves, returning the instance to its normal lifecycle state.

---

## Quarantine Lifecycle

`quarantined` is a stronger isolation state. An instance reaches quarantine through one of two paths:

1. **Supervision failure accumulation** — tracked via `AgentInstanceRecord.crash_loop_count`, `supervision_failure_count`, and `restart_attempts`. When the threshold is reached, `RecordAgentSupervisionFailure` transitions the instance to `quarantined`.
2. **Explicit quarantine** — an operator or the engine calls `QuarantineAgentInstance` directly, for example in response to a security or integrity alert surfaced by `AgentFaultInjectionClass.supervision`.

In the quarantined state:

- All action requests receive `quarantined` as the `AgentActionResultClass`.
- The instance's run lease is cleared (`lease_cleared = true` in `AgentSupervisionDecision`).
- The `quarantined = true` flag is set on the `AgentInstanceRecord` and persisted.
- Evidence is written recording the quarantine event and the reason.

Release from quarantine requires an explicit operator action (via the SBsql control surface described in [../Language_Reference/syntax_reference/agent.md](../Language_Reference/syntax_reference/agent.md)). Release is not automatic, even if the underlying fault condition has resolved. This ensures that every quarantine/release cycle appears in the evidence history with a human decision record.

---

## See Also

- [action_lifecycle_and_arbitration.md](action_lifecycle_and_arbitration.md) — where dry-run and arbitration fit in the action pipeline
- [governance_and_resource_control.md](governance_and_resource_control.md) — resource budget and foreground protection
- [observability_and_control.md](observability_and_control.md) — how to query evidence and explanation output
- [authority_and_activation_model.md](authority_and_activation_model.md) — quarantine in the full 14-state lifecycle
- [../Language_Reference/syntax_reference/agent.md](../Language_Reference/syntax_reference/agent.md) — SBsql for releasing quarantine, reviewing evidence
- [../CDE_Concepts/autonomous_operation.md](../CDE_Concepts/autonomous_operation.md) — CDE-level context for why tamper-evidence matters
