# Governance and Resource Control

## Purpose

This page describes the mechanisms that ensure agent activity remains bounded and cannot interfere with the engine's primary obligation to serve foreground work. It covers the 14-dimension resource budget, the six budget-decision outcomes, the foreground-protection principle, worker capacity planning, rollout profiles and the gated escalation from observe to live, the effect of engine lifecycle modes on effective activation, tenant coordination and metric quorum, and the feature-gate model.

All enumeration names, function signatures, and field names are verified against `project/src/core/agents/agent_runtime.hpp`, `project/src/core/agents/agent_rollout_profile.hpp`, `project/src/core/agents/agent_tenant_coordination.hpp`, and `project/src/core/agents/agent_feature_gates.hpp`.

---

## The 14-Dimension Resource Budget

Every agent action is evaluated against an `AgentResourceBudget` before execution is permitted. The budget is derived from the agent's active policy via `DefaultAgentResourceBudgetForPolicy`. The evaluation function is `EvaluateAgentResourceBudget`, which returns an `AgentResourceBudgetDecision` carrying a `decision` field from `AgentResourceBudgetDecisionKind`.

The 14 dimensions tracked by `AgentResourceBudgetDimension` are:

| Dimension | Token | What it tracks |
|-----------|-------|---------------|
| Foreground protection | `foreground_protection` | Whether foreground database work is currently active; overrides all other dimensions when active. |
| CPU time | `cpu_time` | Cumulative CPU microseconds consumed in the current run. Bounded by `max_cpu_time_microseconds`. |
| Memory bytes | `memory_bytes` | Peak memory bytes allocated by the agent. Bounded by `max_memory_bytes`. |
| I/O bytes | `io_bytes` | Cumulative I/O bytes transferred. Bounded by `max_io_bytes`. |
| I/O operations | `io_ops` | Cumulative I/O operation count. Bounded by `max_io_ops`. |
| Thread slots | `thread_slots` | Number of concurrent execution threads. Bounded by `max_thread_slots`. |
| Queue depth | `queue_depth` | Number of items in the agent's internal work queue. Bounded by `max_queue_depth`. |
| Cadence | `cadence` | Minimum run interval. Enforced via `min_run_interval_microseconds`. |
| Retry backoff | `retry_backoff` | Minimum time between retries after failure. Enforced via `retry_backoff_microseconds`. |
| Runtime timeout | `runtime_timeout` | Maximum wall-clock time for a single run. Enforced via `watchdog_timeout_microseconds`. |
| Cancellation drain | `cancellation_drain` | Whether a cancellation or drain has been requested; active drain blocks new work. |
| History rows | `history_rows` | Rows the agent may query from history or catalog tables in one run. Bounded by `max_history_query_rows`. |
| Evidence fanout | `evidence_fanout` | Number of evidence records the agent may emit per run. Bounded by `max_evidence_fanout`. |
| Label cardinality | `label_cardinality` | Number of distinct labels the agent may attach to emitted evidence. Bounded by `max_label_cardinality`. |

The budget struct `AgentResourceBudget` exposes all 14 corresponding limit fields. The current consumption is tracked in `AgentResourceUsage`.

### Budget Decision Outcomes

`EvaluateAgentResourceBudget` produces one of six `AgentResourceBudgetDecisionKind` outcomes:

| Decision | Token | Meaning |
|----------|-------|---------|
| Allow | `allow` | All dimensions are within budget; action may proceed. |
| Throttle and defer | `throttle_defer` | One or more soft limits exceeded; action is deferred to the next eligible interval. |
| Shed and refuse | `shed_refuse` | A hard limit was reached; action is refused for this cycle (not quarantined). |
| Fail closed | `fail_closed` | Budget data was unavailable or structurally invalid; action defaults to no-mutation. |
| Cancel and drain | `cancel_drain` | An active cancellation or drain was detected; all agent work stops. |
| Foreground protection | `foreground_protection` | Foreground database activity was detected; agent work is suspended until it clears. |

A `foreground_protection` decision is always raised before any other dimension is evaluated. The budget field `protect_foreground_work = true` is set by default on every agent resource budget. Agents never compete with foreground work.

The `AgentResourceBudgetDecision` struct also carries `action_allowed`, `mutation_allowed`, and `health_publish_allowed` flags. Health observation is permitted even when mutation is denied, so operators continue to see accurate status during resource-constrained periods.

---

## Foreground Protection

The foreground-protection principle appears in three places:

1. The `AgentResourceBudget` `protect_foreground_work = true` default — enforced by `EvaluateAgentResourceBudget`.
2. The `AgentResourceBudgetEvaluationInput.foreground_database_work_active` flag — populated by the runtime from live engine state.
3. The `DynamicCleanupDebtSchedulerPolicy.protect_foreground_work = true` default — the cleanup scheduler also independently suspends when foreground work is active.

All three operate independently. An agent cannot bypass foreground protection in one layer by winning at another.

---

## Worker Capacity Planning

`PlanAgentWorkerCapacity` produces an `AgentWorkerCapacitySnapshot` that shows how many background worker slots are available and which agent candidates can be assigned to them.

The inputs are:

- `AgentWorkerCapacityConfig` — `observed_cpu_count`, `configured_cpu_count`, `foreground_reserved_capacity`, `max_background_worker_slots`, and runtime context flags.
- A list of `AgentWorkerCapacityCandidate` records — each carrying a policy, current resource usage, and `requested_worker_slots`.

The snapshot output includes:

- `effective_cpu_count` — the lesser of observed and configured core counts.
- `foreground_reserved_capacity` — the number of CPU slots held exclusively for foreground work.
- `background_worker_slots` — slots available for agent work after the foreground reserve is subtracted.
- `foreground_work_active` — whether the engine is currently serving foreground demand.
- A per-candidate `AgentWorkerCapacityAssignment` with `selected`, `assigned`, `resource_decision`, and the `resource_dimension` that constrained the decision if applicable.

The snapshot is auditable (it carries evidence) and is not an authority for parser admission, transaction finality, catalog truth, or security decisions.

DML-prework agents (those flagged `dml_prework_agent = true` in their candidate record) may be scheduled ahead of foreground demand arrival; the snapshot marks these with `can_run_before_foreground_demand = true`.

---

## Rollout Profiles and Gated Escalation

### Activation Profile Progression

Activation follows a strictly ordered progression. `ValidateRolloutTransition` enforces the rules:

```
disabled → observe_only → recommend_only → dry_run → live_action
```

Any forward step is permitted except the `dry_run` → `live_action` transition. That step requires `explicit_operator_approval = true`. If the parameter is false, the transition is refused regardless of other conditions.

This means **no agent can reach live-action status without a documented operator decision**. The default activation for every agent in the canonical manifest is either `disabled`, `observe_only`, `recommend_only`, or `dry_run`. Zero agents ship with `live_action` as their default.

### Rollout Modes

The `AgentActionRolloutProfile` in `agent_rollout_profile.hpp` supports seven rollout modes:

| Mode | Token | Description |
|------|-------|-------------|
| Disabled | `disabled` | No execution. |
| Shadow | `shadow` | Execution occurs but results are not applied; used for correctness comparison. |
| Observe | `observe` | Metric-observation only. |
| Dry run | `dry_run` | Full decision logic; mutations suppressed. |
| Canary | `canary` | Live execution limited to `canary_percent` of eligible subjects, bounded by `canary_max_subjects`. |
| Phased | `phased` | Graduated rollout with a `phased_target_percent` ceiling. |
| Live | `live` | Full live execution. |

`AgentActionRolloutModeAllowsMutation` returns `true` only for `canary`, `phased`, and `live`. All other modes suppress mutation. `AgentActionRolloutModeRequiresDryRun` identifies modes that must complete a dry-run pass first.

The rollout profile carries a `failure_threshold` and `observed_failures` counter. If failures exceed the threshold, `quarantine_on_failure = true` (the default) causes the profile to transition to the `quarantined` rollout state, halting further progression.

### Rollout States

An `AgentActionRolloutProfile` can be in one of seven `AgentActionRolloutState` values: `disabled`, `pending`, `active`, `paused`, `completed`, `failed`, or `quarantined`.

---

## EffectiveActivationForLifecycle: Engine Mode Constraints

`EffectiveActivationForLifecycle(configured, mode)` computes the actual activation level from a configured profile and the current engine lifecycle mode. The effective activation is always the more restrictive of the two.

The `AgentLifecycleMode` values that constrain agent activation include (verified in `agent_runtime.hpp`):

| Mode | Effect on agents |
|------|-----------------|
| `normal` | No restriction; configured activation applies. |
| `backup` | Agents whose actions would interfere with backup consistency are restricted. |
| `restore` | Agents are restricted to observe-only or below during active restore. |
| `crash_recovery` | No agent mutations permitted; observe-only effective. |
| `read_only` | Mutations suppressed; observe and recommend profiles continue. |
| `maintenance` | Only maintenance-relevant agents may proceed; general agents are restricted. |
| `shutdown` | All agents enter stopping mode. |
| `archive_hold` | Archival agents restricted to avoid compounding an active hold. |
| `pitr` | Agents restricted during point-in-time recovery execution. |

Operators do not need to manually pause all agents when the engine enters a restricted lifecycle mode; `EffectiveActivationForLifecycle` handles it automatically. When the mode returns to `normal`, the configured activation resumes.

---

## Tenant Coordination and Metric Quorum

For deployments where multiple agent instances operate over the same tenant scope, `EvaluateAgentTenantWorkloadCoordination` governs admission. The request carries:

- An `AgentTenantCoordinationGroup` describing the group membership, roles, and whether follower live actions are permitted.
- An `AgentTenantWorkloadBudget` (per-tenant limits on live actions, queue depth, memory, worker slots, and I/O bytes).
- A `AgentTenantCoordinationLockRequest` for resources the action needs to hold exclusively.
- A list of `AgentTenantSharedMetricSnapshot` records and `required_metric_families`.
- `required_metric_quorum` — the minimum number of fresh, trusted metric sources that must agree before the coordination decision is admitted (default: **2**).

Coordination roles are `leader`, `follower`, and `observer`. By default, `require_single_leader = true` and `allow_follower_live_actions = false`. A follower can observe and recommend, but cannot execute live mutations unless the group policy explicitly permits it.

The coordination outcome is `admitted`, `queued`, or `refused`. A refused decision produces `fail_closed = true` on the coordination result.

If fewer than `required_metric_quorum` fresh metric sources are available, the coordination decision is refused regardless of other conditions. This is the metric quorum gate: the engine prefers to withhold action rather than act on a potentially stale or split metric picture.

---

## Feature Gates

`EvaluateAgentFeatureAvailability` checks whether a given agent type can run in the current runtime context. The result is one of five `AgentFeatureAvailability` values:

| Value | Token | Meaning |
|-------|-------|---------|
| Available | `available` | The feature is enabled and context requirements are met. |
| Unavailable — disabled stub | `unavailable_disabled_stub` | The agent exists in the manifest but is compiled as a stub. |
| Unavailable — edition | `unavailable_edition` | The current edition does not include this agent's capability. |
| Unavailable — cluster authority | `unavailable_cluster_authority` | The agent requires cluster authority, which is not present. |
| Unavailable — private feature | `unavailable_private_feature` | The feature is gated behind a private build configuration. |

The `InstalledCapabilityRecord` in `agent_feature_gates.hpp` tracks each capability's `edition_scope` (`community`, `private_build`, `enterprise`, `cluster`), its `lifecycle_state` (`installed`, `enabled`, `disabled`, `quarantined`, or `retired`), and whether it requires a parser package. Capability downgrades are rejected by `ValidateCapabilityNoDowngrade`.

Capabilities use an epoch-based policy model. `ValidateCapabilityPolicyEpoch` enforces that the agent's observed policy epoch is not behind the installed record's policy epoch, preventing stale policy from enabling a capability that has since been restricted.

---

## See Also

- [action_lifecycle_and_arbitration.md](action_lifecycle_and_arbitration.md) — resource budget in the full action pipeline
- [agent_catalog.md](agent_catalog.md) — each agent's default activation profile
- [authority_and_activation_model.md](authority_and_activation_model.md) — the five activation profiles and rollout gate
- [maintenance_and_tuning_agents.md](maintenance_and_tuning_agents.md) — cleanup scheduler foreground protection detail
- [../Language_Reference/syntax_reference/agent.md](../Language_Reference/syntax_reference/agent.md) — SBsql for adjusting activation profiles and resource budgets
- [../CDE_Concepts/autonomous_operation.md](../CDE_Concepts/autonomous_operation.md) — CDE-level rationale for bounded autonomy
