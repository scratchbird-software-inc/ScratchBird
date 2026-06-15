# Observability and Control

## Purpose

This page describes how operators can observe agent state and influence agent behavior. It covers the `sys.information.*` agent projections and the `sys.agents` surface that expose runtime health, metric dependency freshness, policy bindings, pending actions, and (redacted) evidence. It then summarizes the levers operators hold: activation control, policy management, override management, manual approval, and quarantine release.

SBsql syntax for exercising these levers is not restated here — see [../Language_Reference/syntax_reference/agent.md](../Language_Reference/syntax_reference/agent.md). This page explains what the surfaces expose and what operators can do with them.

All type names and field names are verified against `project/src/engine/internal_api/catalog/sys_information_projection.hpp` and `project/src/sys/agents_views.cpp`.

---

## The Observability Architecture

Agent observability in ScratchBird is implemented through two distinct layers:

1. **`sys.information.*` agent projections** — standard-style information-schema views that expose agent state through the engine's normal query surface. These follow the same redaction, authorization, and MGA-snapshot-visibility rules as all other `sys.information` views.
2. **`sys.agents` and `cluster.sys.agents`** — engine-owned surfaces implemented by `EngineSysAgents` and `EngineClusterSysAgents` (anchored in `src/sys/agents_views.cpp`). These surfaces provide direct agent management capabilities alongside observation.

Both layers are authorization-filtered. A caller without the appropriate right sees only what is visible at their grant level; sensitive fields are redacted, not omitted entirely, so that operators can see that a field exists but cannot read its content without the required right.

---

## SysInformationSourceKind: Agent Projection Types

The `SysInformationSourceKind` enumeration defines the five agent-specific source kinds that feed `sys.information.*` views (verified in `sys_information_projection.hpp`):

| Source kind | Token | What it exposes |
|-------------|-------|----------------|
| Agent runtime | `agent_runtime` | Per-agent health state, lifecycle state, activation profile, policy binding, queue depth, action backlog, failure count, quarantine count, last decision, and last evidence UUID. |
| Agent metric dependency | `agent_metric_dependency` | Per-agent metric dependency contracts — which metric families each agent requires, whether they are required or optional, the freshness limit, current freshness, quality state, and fail behavior. |
| Agent policy | `agent_policy` | Policy bindings — which policy families are attached to each agent, their attachment UUIDs, active/valid states, and catalog generation. |
| Agent action | `agent_action` | Pending and recent actions — action UUID, action ID, agent, state, risk class, approval-required flag, actor, and diagnostic code. |
| Agent evidence | `agent_evidence` | Evidence records — evidence UUID, type, associated action, redaction class, creation time, actor, payload digest, and whether the payload is redacted (`payload_redacted = "YES"` by default). |
| Storage agent state | `storage_agent_state` | Specialized state projections for storage agents (filespace capacity, page allocation). Provides filespace health state, last metric timestamps, last recommendation and refusal codes. |

---

## What Operators See Per Projection

### Agent Runtime State (`agent_runtime`)

The `SysInformationAgentSource` struct (the backing source for `agent_runtime` projections) exposes:

- `agent_type_id` — the canonical agent type token (e.g. `transaction_pressure_manager`).
- `scope_kind` and `scope_uuid` / `scope_ref` — which scope the instance is bound to.
- `state` — the observable lifecycle state (one of the 14 in `AgentLifecycleState`).
- `health_state` — derived health summary.
- `enabled` — `"YES"` or `"NO"`.
- `policy_uuid` / `policy_name` — the active policy.
- `last_transition_at` — when the current state was entered.
- `last_diagnostic_code` — the most recent diagnostic code, if any.
- `last_evidence_uuid` — the UUID of the most recent evidence record emitted.
- `last_decision` — human-readable description of the last decision outcome.
- `retry_not_before` — when the agent is next eligible to run (after backoff or cooldown).
- `queue_depth` and `action_backlog` — current work queue state.
- `failure_count` and `quarantine_count` — cumulative failure and quarantine history.
- `overhead_budget_units` — current overhead budget consumption.
- `diagnostic_redaction_state` — whether the diagnostic fields are redacted for this caller.

Callers without `obs_agent_state_read` see the row but with sensitive fields suppressed (the `hidden` flag controls row-level visibility for restricted scopes).

### Metric Dependency Freshness (`agent_metric_dependency`)

The `SysInformationAgentMetricDependencySource` exposes per-dependency rows:

- `metric_family` and `metric_namespace` — which metric family the dependency refers to.
- `required_or_optional` — whether this dependency is required or optional.
- `freshness_limit` — the maximum age the engine will accept for this metric.
- `current_freshness` — the age of the most recent observation.
- `quality_state` — `"fresh"`, `"stale"`, `"missing"`, or other states reflecting the current metric health.
- `fail_behavior` — what happens when this dependency is not satisfied (typically `"fail_closed"`).
- `metric_values_visible` — whether the metric values themselves are visible to this caller (metric values may require `sec_auth_metrics_read`).

This view is the primary diagnostic surface for the most common agent health problem: an agent that is `failed_closed` because a required metric family is stale or absent. Operators can identify exactly which metric family is the bottleneck and investigate the metric source.

### Policy Bindings (`agent_policy`)

The `SysInformationAgentPolicySource` exposes:

- `policy_family`, `policy_uuid`, and `policy_name` — the attached policy.
- `active_state` — whether the attachment is currently active.
- `validation_state` — whether the policy passed its last validation run.
- `attached_at` and `attached_by` — when and by whom the policy was attached.
- `catalog_generation_id` — the catalog generation at which the binding is recorded.

Callers require `obs_policy_read` to see policy body content. Callers without it see the binding metadata but not the policy configuration fields.

### Pending Actions (`agent_action`)

The `SysInformationAgentActionSource` exposes:

- `action_uuid` and `action_id` — the specific action candidate.
- `agent_uuid` and `agent_ref` — which agent submitted the action.
- `state` — e.g. `"recommended"`, `"approval_required"`, `"accepted"`, `"suppressed"`.
- `risk_class` — the arbitration risk class for this action.
- `approval_required` — `"YES"` or `"NO"`.
- `actor_uuid` / `actor_ref` — the principal associated with the action (visible only to callers with sufficient right).
- `diagnostic_code` — reason code if the action is in a non-accepted state.
- `expires_at` — when the action record expires from the observable surface.

Actions with `approval_required = "YES"` are the primary surface for the manual approval workflow. Operators query this view to identify actions awaiting approval, then use the SBsql approval command to proceed or cancel.

### Evidence Records (`agent_evidence`)

The `SysInformationAgentEvidenceSource` exposes:

- `evidence_uuid` and `evidence_type` — unique identifier and type of the evidence record.
- `action_uuid` / `action_ref` — the action this evidence is associated with, if any.
- `redaction_class` — the redaction class applied (defaults to `"summary"` in the projection source, meaning only a summary view is presented without `obs_agent_evidence_read`).
- `payload_digest` — the digest of the evidence payload.
- `payload_redacted` — `"YES"` by default; `"NO"` only for callers with the appropriate evidence-read right.
- `created_at` and `actor_uuid` / `actor_ref` — when the evidence was created and by which agent/principal (actor visibility gated by right).

The evidence projection is intentionally limited by default. The digest is always present, allowing external verifiers to confirm evidence continuity without needing the full payload. The full payload requires `obs_agent_evidence_read`.

---

## The `sys.agents` Surface

`sys.agents` and `cluster.sys.agents` are implemented by engine-owned surfaces (`EngineSysAgents` and `EngineClusterSysAgents`) and provide direct management alongside observation. The management interface is accessed through SBsql agent statements (see [../Language_Reference/syntax_reference/agent.md](../Language_Reference/syntax_reference/agent.md)).

The surface gives operators:

- **Activation control** — view current activation profiles and request profile transitions.
- **Policy management** — attach, detach, validate, and version-bump policies.
- **Override management** — create, renew, and deactivate `AgentArbitrationOverride` records.
- **Manual approval** — approve or cancel actions in the `approval_required` state.
- **Quarantine release** — release instances from the `quarantined` lifecycle state.
- **Explanation queries** — retrieve the `ExplainAgentDecision` output for any recent decision.
- **Fault injection** — inject a named fault scenario for resilience testing (requires appropriate right; not available in all edition scopes).

---

## Operator Levers Summary

| Lever | What it does | Right required |
|-------|-------------|----------------|
| View agent state | Query `sys.information.*` agent projections for lifecycle state, health, and last decision. | `obs_agent_state_read` |
| Read evidence | View the full evidence payload for agent decisions. | `obs_agent_evidence_read` |
| Read recommendations | View pending recommendations from `recommend_only` agents. | `obs_agent_recommendation_read` |
| Control agent | Change activation profile, pause, resume, disable. | `obs_agent_control` |
| Approve action | Approve an action in the `approval_required` queue. | `obs_agent_action_approve` |
| Cancel action | Cancel a pending or queued action. | `obs_agent_action_cancel` |
| Create/deactivate override | Create a suppression or force-allow override; deactivate an existing one. | `obs_agent_override` |
| Read policy | View policy configuration attached to agents. | `obs_policy_read` |
| Simulate policy | Run a policy decision simulation without applying it. | `obs_policy_simulate` |
| Apply policy | Attach or update a policy binding. | `obs_policy_apply` |
| Rollback policy | Revert a policy to a previous generation. | `obs_policy_rollback` |
| Delete policy | Remove a policy record. | `obs_policy_delete` |
| Read cluster health | View cluster-scope agent health. | `obs_cluster_health_inspect` |
| Cluster control | Exercise cluster-scope agent management. | `obs_cluster_control` |
| Read auth metrics | View authentication-related metrics (session security surface). | `sec_auth_metrics_read` |
| Edit redaction policy | Modify evidence redaction policy. | `sec_redaction_policy_edit` |

These rights correspond to the `AgentSecurityRight` enumeration in `agent_runtime.hpp` and are enforced by `EvaluateAgentCommandGrant`.

---

## Activation Control: Moving Through the Rollout Profile

Operators change an agent's activation profile using the SBsql ALTER AGENT statement. The profile transition is validated by `ValidateRolloutTransition`. The critical constraint — moving from `dry_run` to `live_action` requires `explicit_operator_approval = true` — means that the SBsql statement must carry an explicit approval acknowledgement; the engine refuses the transition if it is absent.

`EffectiveActivationForLifecycle` computes the actual activation at runtime. Operators querying the agent state surface see both the configured profile and the effective activation. If these differ (for example, because the engine is in `read_only` lifecycle mode), the effective activation explains why an agent that appears configured for live-action is currently running as recommend-only.

---

## Diagnosing Common Issues

### Agent is `failed_closed` and not running

1. Query `agent_runtime` projection: check `last_diagnostic_code`.
2. Query `agent_metric_dependency` projection: look for a metric family with `quality_state = "stale"` or `"missing"`.
3. Investigate the metric source for that family. If the metric provider itself is unhealthy, resolve that first.
4. If the diagnostic code points to a policy issue, query `agent_policy` projection: look for `validation_state = "invalid"` or `active_state = "inactive"`.

### Agent is `approval_required` and stalled

1. Query `agent_action` projection: find actions with `approval_required = "YES"`.
2. Use the SBsql approval command (see [../Language_Reference/syntax_reference/agent.md](../Language_Reference/syntax_reference/agent.md)) to approve or cancel.
3. If the action has `risk_class = "critical"` or `risk_class = "high"`, review the `agent_evidence` projection for the associated dry-run evidence before approving.

### Agent is `quarantined`

1. Query `agent_runtime` projection: note `quarantine_count` and `failure_count`.
2. Query `agent_evidence` projection: read the quarantine-event evidence to understand what failure class triggered it.
3. Resolve the underlying cause (metric staleness, policy invalidity, actuator degradation).
4. Use the SBsql quarantine-release command to return the instance to `registered` state. A new run cycle begins after the configured cooldown.

---

## See Also

- [authority_and_activation_model.md](authority_and_activation_model.md) — the lifecycle state machine and activation profiles
- [action_lifecycle_and_arbitration.md](action_lifecycle_and_arbitration.md) — how actions enter the `approval_required` state
- [evidence_explainability_and_safety.md](evidence_explainability_and_safety.md) — evidence chain structure, quarantine lifecycle
- [governance_and_resource_control.md](governance_and_resource_control.md) — resource budget decisions that appear in the diagnostic surface
- [../Language_Reference/syntax_reference/agent.md](../Language_Reference/syntax_reference/agent.md) — complete SBsql syntax for all agent control operations
- [../CDE_Concepts/autonomous_operation.md](../CDE_Concepts/autonomous_operation.md) — CDE-level rationale for the observability design
