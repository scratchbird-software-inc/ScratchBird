# Maintenance and Tuning Agents

## Purpose

This page provides deep dives into five agents that have rich, verifiable behavior in the source tree. Together they span the full breadth of what autonomous maintenance means in a Convergent Data Engine: managing session pressure, cleaning up stale data across every storage model, running long-horizon index operations safely, tuning performance knobs within strict bounds, and proving backup-readiness before it is needed.

All types and field names are verified against their respective source headers in `project/src/core/agents/`.

---

## Transaction Pressure Manager

**Source:** `project/src/core/agents/agents/transaction_pressure_manager.hpp`
**Agent token:** `transaction_pressure_manager`
**Authority class:** `request_action` — the engine owns the transaction; the agent requests.
**Default activation:** `recommend_only`

### What It Does

Long-idle transactions are one of the most common sources of storage pressure in a multi-generational engine. A transaction that stays open holds a cleanup horizon that prevents all older row versions from being reclaimed, regardless of whether they are still needed. The transaction pressure manager monitors idle transactions and applies an escalation ladder to request engine action.

The key design constraint is explicit in the source: `parser_finality_authority = false` and `client_state_authority = false` on the result struct. The engine owns the transaction. The agent requests.

### The Escalation Ladder

The `TransactionPressureManagerPolicy` defines five idle-time thresholds, all configurable (defaults verified in source):

| Step | Token | Default idle time | What the agent requests |
|------|-------|------------------|------------------------|
| 1 | `warn_notify` | **300 s** (5 min) | Emit a warning notification observable in the agent surface. |
| 2 | `request_restart` | **900 s** (15 min) | Request that the engine offer the session a clean restart. |
| 3 | `request_reauth` | **1 200 s** (20 min) | Request that the engine require the session to re-authenticate. |
| 4 | `request_cancel` | **1 500 s** (25 min) | Request that the engine cancel the idle transaction (requires `request_cancel_allowed = true` in policy, which defaults to `false`). |
| 5 | `force_*` | **1 800 s** (30 min) | Force action (rollback, commit, or restart) — requires explicit `force_authority_gate_present` and `force_authority_gate_allows` in the policy. Force is explicitly off by default. |

Force action requires two independent gates: `force_authority_gate_present` must be true (confirming the gate was provided), and `force_authority_gate_allows` must be true (confirming the gate permits force for this specific context). Missing either gate means the agent stops at `request_cancel` at most.

The decision struct `TransactionPressureManagerTickResult` also carries `denied_non_authoritative` — set when the session or transaction binding is not fully authoritative (for example, during a mid-flight ownership transfer). In that case the agent takes no action regardless of idle time.

For background on the MGA transaction architecture that makes cleanup horizons relevant, see [../Getting_Started/core_concepts/understanding_mga.md](../Getting_Started/core_concepts/understanding_mga.md).

---

## Dynamic Cleanup Debt Scheduler

**Source:** `project/src/core/agents/dynamic_cleanup_debt_scheduler.hpp`
**Used by:** `storage_version_cleanup_agent` and the broader cleanup subsystem.
**Authority class (cleanup agents):** `direct_bounded_action`
**Default activation (cleanup agents):** `dry_run`

### What It Does

`PlanDynamicCleanupDebt` is a prioritization and work-selection surface. Given a list of `DynamicCleanupDebtSource` records and a `DynamicCleanupDebtSchedulerPolicy`, it selects which cleanup items to schedule, how many work units to allocate, and which to defer or refuse — all without executing the cleanup itself. Execution happens through the actuator after the plan is approved by the full agent action pipeline.

Cleanup respects the MGA cleanup horizon: `requires_mga_cleanup_horizon = true` on most source records means the scheduler will not schedule cleanup beyond what the authoritative cleanup horizon service has confirmed is safe.

### The 12 Cleanup Debt Families

This is one of the clearest structural proofs that ScratchBird is a true convergent engine: a single cleanup scheduler manages debt across every data model in use.

| # | Family | Token | What it cleans |
|---|--------|-------|----------------|
| 1 | Version chain | `version_chain` | Obsolete row versions from the relational MGA version chain. |
| 2 | Exact index leaf | `exact_index_leaf` | Dead entries in exact (B-tree style) index leaf pages. |
| 3 | Secondary delta ledger | `secondary_delta_ledger` | Pending merges and tombstones in secondary index change buffers. |
| 4 | Summary page range | `summary_page_range` | Stale page-range summaries used for aggregate push-down. |
| 5 | Large value | `large_value` | Orphaned large-value (overflow) storage from dropped rows or updated blobs. |
| 6 | Hot leaf | `hot_leaf` | Pressure relief for hot B-tree leaf pages accumulating write contention. |
| 7 | NoSQL key-value | `nosql_key_value` | TTL expiry and generation compaction for the key-value model. |
| 8 | NoSQL document | `nosql_document` | Generation merges for the document model. |
| 9 | NoSQL search | `nosql_search` | Segment merges for the full-text search model. |
| 10 | NoSQL vector | `nosql_vector` | Generation retirement for the approximate-search vector model. |
| 11 | NoSQL graph | `nosql_graph` | Adjacency list compaction for the graph model. |
| 12 | NoSQL time series | `nosql_time_series` | Bucket retirement for the time-series model. |

Families 1–6 cover relational and shared storage structures. Families 7–12 cover each NoSQL model family supported by the engine. All 12 are scheduled by the same planner under the same foreground-protection and policy-budget rules.

### Scheduling Policy

`DynamicCleanupDebtSchedulerPolicy` controls:

- `max_total_work_units` (default 64) — total work units the scheduler may commit in one tick.
- `max_scheduled_items` (default 8) — maximum number of debt items scheduled per tick.
- `default_max_family_work_units` (default 16) — per-family work-unit cap (also configurable per family via `DynamicCleanupDebtFamilyCap`).
- `default_max_family_items` (default 2) — per-family item count cap.
- `protect_foreground_work = true` — suspends all scheduling when foreground activity is detected.
- Retry backoff range: 1 s minimum to 60 s maximum.
- Lease duration: 5 s (to prevent duplicate scheduling when workers overlap).

Each scheduled item receives a lease token and a next-eligible timestamp. If the lease is still active when the scheduler runs again, the item is deferred with `deferred_lease` rather than double-scheduled.

Failure modes are explicit: `fail_closed_retain_debt` (the default for most families) means the scheduler refuses to proceed when its source data is not authoritative rather than cleaning up data it cannot prove is safe to remove.

---

## Online Maintenance Progress

**Source:** `project/src/core/agents/online_maintenance_progress.hpp`, `project/src/core/agents/vector_maintenance_jobs.hpp`
**Used by:** index rebuild, vector maintenance, nosql compaction, optimizer stats refresh, and other long-horizon operations.

### What It Does

Long-running maintenance operations — rebuilding a vector index, refreshing optimizer statistics, compacting a large NoSQL segment — cannot be atomic. They run over time, may need to be cancelled, and must survive crashes without repeating completed work. `OnlineMaintenanceStateStore` and the functions that operate on it provide a crash-safe checkpoint-and-resume mechanism for these operations.

### Operation Phases

An `OnlineMaintenanceProgressSnapshot` moves through `OnlineMaintenancePhase` values:

| Phase | Token | Description |
|-------|-------|-------------|
| Requested | `requested` | Operation admitted; not yet running. |
| Running | `running` | Active; progress being recorded. |
| Cancel requested | `cancel_requested` | Operator or policy has requested cancellation; drain in progress. |
| Cancelled | `cancelled` | Cleanly cancelled; checkpoint written. |
| Resumable | `resumable` | Operation was cancelled or crashed but left a valid checkpoint. |
| Publish ready | `publish_ready` | Work is complete but the result has not yet been made visible. |
| Published | `published` | Result is visible; resources released. |
| Completed | `completed` | Full lifecycle done. |
| Failed closed | `failed_closed` | Unrecoverable error; checkpoint preserved for review. |

The `COMPLETED_UNPUBLISHED` pattern (evident from the `kOnlineMaintenanceCompletedUnpublished` constant in source) is a first-class phase: work completes successfully and the result is validated before being made visible. `PublishOnlineMaintenanceOperation` requires `authoritative_generation_validated = true` before transitioning to `published`. If this flag is not present, the publish is refused with `kOnlineMaintenanceUnsafePublishRefused`.

This validate-before-publish gate is critical for approximate-search vector indexes: a rebuilt index is not made available for search queries until the engine has verified the new training generation against the previous one. Partial visibility is controlled by `no_partial_visibility` in the publish request.

### Cancel and Resume

`CancelOnlineMaintenanceOperation` checkpoints the current progress before stopping. The checkpoint is crash-durable when `durable_checkpoint_persisted = true`. On restart, `RecoverOnlineMaintenanceOperation` reads the checkpoint and returns a `recovered_resumable` decision, allowing `ResumeOnlineMaintenanceOperation` to pick up from where the operation stopped rather than restarting from the beginning.

`kOnlineMaintenanceUnsafeResumeRefused` is raised if the checkpoint is structurally invalid or if the MGA cleanup horizon has advanced past the point the operation assumed when it was checkpointed — in which case the operation must restart rather than resume to avoid inconsistency.

### Vector-Specific Maintenance

`VectorMaintenanceJobRequest` in `vector_maintenance_jobs.hpp` integrates with `OnlineMaintenanceProgressSnapshot` for the three vector maintenance action kinds:

- `adaptive_tuning` — adjusting approximate-search parameters without rebuilding.
- `retrain` — retraining the index with updated data distribution.
- `rebuild` — full index rebuild with validate-before-publish.

`VectorMaintenancePublishState` tracks whether a rebuilt vector index is `waiting_validation`, `publish_after_validation`, `published`, or `refused`. The `runtime_correctness_unproven` failure class (`VectorMaintenanceFailureClass`) prevents publication of an index whose correctness properties cannot be verified against the current engine state.

---

## Adaptive Tuning Controller

**Source:** `project/src/core/agents/adaptive_tuning_controller.hpp`
**Used by:** agents that adjust bounded performance knobs based on observed metric evidence.

### What It Does

The adaptive tuning controller selects a value for one tuning knob at a time, using current metric evidence and resource governance state as inputs. It is strictly advisory: `advisory_only = true` in the result struct, and the safety policy explicitly denies all authority that would make it non-advisory.

### The Eight Tunable Knobs

`AdaptiveTuningKnob` defines eight knobs (verified in source):

| Knob | Token | What it controls |
|------|-------|-----------------|
| Prefetch depth | `kPrefetchDepth` | Read-ahead depth for sequential I/O patterns. |
| Merge workers | `kMergeWorkers` | Number of concurrent segment-merge workers. |
| Refresh interval | `kRefreshInterval` | How frequently a materialized or cached structure is refreshed. |
| Candidate budget | `kCandidateBudget` | Number of candidates evaluated during a selection or pruning step. |
| Cache partition | `kCachePartition` | Relative size of a cache partition for a given workload class. |
| Evidence sample rate | `kEvidenceSampleRate` | Fraction of decision events that produce a full evidence record. |
| Vector ef_search | `kVectorEfSearch` | The ef_search parameter for approximate nearest-neighbor search (HNSW-style traversal width). |
| Vector nprobe | `kVectorNprobe` | The nprobe parameter for partition-based approximate search (IVF-style probe count). |

The `kVectorEfSearch` and `kVectorNprobe` knobs are particularly notable: they allow the engine to adaptively balance recall quality against query latency for approximate-search workloads without requiring operator intervention or index rebuilds.

### Actions

`AdaptiveTuningActionClass` defines six outcomes per evaluation:

| Action | Token | Meaning |
|--------|-------|---------|
| Refuse | `kRefuse` | Safety or proof constraints prevent any change. |
| Hold | `kHold` | Evidence insufficient to recommend a direction; retain current value. |
| Increase | `kIncrease` | Metric evidence supports raising the knob value. |
| Decrease | `kDecrease` | Metric evidence supports lowering the knob value. |
| Reset | `kReset` | Return to the last operator-set value. |
| Default | `kDefault` | Return to the compiled-in default value. |

### The Authority-Denying Safety Policy

`AdaptiveTuningSafetyPolicy` is the most explicit statement of bounded authority in the adaptive tuning subsystem. All of the following fields are `false` by design:

- `parser_or_reference_authority` — the controller cannot affect parser behavior or reference resolution.
- `provider_transaction_finality_authority` — the controller cannot affect whether transactions commit or abort.
- `provider_visibility_authority` — the controller cannot change what rows or versions are visible.
- `client_autocommit_authority` — the controller cannot affect client autocommit semantics.
- `wal_recovery_authority` — the controller cannot influence WAL replay or crash recovery.

Additionally:

- `mga_recheck_required = true` — any knob change must be rechecked against the MGA state before the change is applied.
- `security_recheck_required = true` — a security policy recheck is mandatory before applying a change.
- `engine_mga_authoritative = false` — the controller does not assert MGA authority.

Any request that sets any of these authority flags to `true` is refused.

The controller selects from a bounded `[min_value, max_value]` range with a known `default_value`. It cannot produce a value outside this range, and the range is validated against the `semantics_neutral_proof` field: a proof that changing the knob within the range does not alter the logical meaning of any query result.

---

## Restore Drill Manager

**Source:** `project/src/core/agents/agents/restore_drill_manager.hpp`
**Agent token:** `restore_drill_manager`
**Authority class:** `request_action`
**Default activation:** `recommend_only`

### What It Does

Backup systems fail in the one moment they matter: when a restore is attempted. The restore drill manager automates restore-readiness verification by running isolated restore drills on a schedule, so that the evidence of a successful restore exists in the audit record before it is urgently needed.

### The Four Preconditions

`RestoreDrillManagerRequest` carries four boolean preconditions that must all be true before the manager will produce a `run_restore_drill` decision:

| Precondition | Field | What it confirms |
|-------------|-------|-----------------|
| Target isolated | `target_isolated` | The restore target is isolated from production data; a drill cannot contaminate live data. |
| Backup manifest available | `backup_manifest_available` | A valid backup manifest exists and is reachable; the drill has something to restore from. |
| Restore inspection open | `restore_inspection_open` | The inspection surface is open, allowing the drill to verify the restored state. |
| Intended state observed | `intended_state_observed` | After the restore, the intended catalog/data state was actually observed in the restored target. |

If any of these four is false, the decision is `refused` with `fail_closed = true`. The manager will not claim a drill succeeded unless all four conditions are independently verifiable.

This four-gate model means the restore drill manager produces `run_restore_drill` outcomes only when all of the following are provably true: there is something to restore from, the restore target is safe to use, the inspection channel is open, and the result matches expectations. The evidence record for a successful drill is a direct attestation of all four.

### Workflow Integration

The drill result integrates with `AgentLocalWorkflowRecord`. Each drill attempt — including refused attempts — produces a workflow record, ensuring that both successes and refused attempts appear in the observable evidence stream. Operators can query the frequency of refused drills to identify configuration gaps (for example, a backup manifest that is chronically unavailable or a restore target that is never isolated on schedule).

---

## See Also

- [agent_catalog.md](agent_catalog.md) — authority class and default activation for each of these agents
- [governance_and_resource_control.md](governance_and_resource_control.md) — foreground protection and resource budgets that bound all maintenance work
- [evidence_explainability_and_safety.md](evidence_explainability_and_safety.md) — how maintenance evidence is chained and made tamper-evident
- [../Getting_Started/core_concepts/understanding_mga.md](../Getting_Started/core_concepts/understanding_mga.md) — MGA cleanup horizon, which the cleanup scheduler respects
- [../CDE_Concepts/autonomous_operation.md](../CDE_Concepts/autonomous_operation.md) — why convergent self-maintenance across all model families matters
