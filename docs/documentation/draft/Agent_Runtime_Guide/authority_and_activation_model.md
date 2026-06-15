# Authority and Activation Model

## Purpose

This page defines the authority and activation framework that governs every ScratchBird agent. Before reading about specific agents or their behavior, operators and integrators should understand the four-tier authority ladder, the five activation profiles, the fourteen lifecycle states, the action result classes, and the hard authority boundaries that no agent is permitted to cross.

All enumerations and field names on this page are verified against `project/src/core/agents/agent_runtime.hpp`.

---

## The Four-Tier Authority Ladder

Every agent in the canonical manifest is assigned exactly one `AgentAuthorityClass`. This class defines the ceiling of what the agent can do; the actual effective authority at runtime is further constrained by activation profile, lifecycle state, policy gates, resource budgets, dry-run requirements, and manual approval requirements.

| Authority Class | Token | What the agent may do |
|-----------------|-------|----------------------|
| Observe only | `observe_only` | Read metrics, health, and state. Produce no recommendations and initiate no actions. |
| Recommend only | `recommend_only` | Produce policy recommendations visible to operators. No binding actions. |
| Request action | `request_action` | Submit action requests to the engine for evaluation; the engine decides whether to admit them. |
| Direct bounded action | `direct_bounded_action` | Execute bounded, resource-governed actions within a pre-approved actuator contract; still subject to all safety, arbitration, and evidence requirements. |

Moving up the ladder always requires operator-level configuration. No agent self-promotes its authority class.

---

## The Five Activation Profiles

An agent's `AgentActivationProfile` is the configured operational posture. It controls what the agent will attempt when the lifecycle state is compatible.

| Profile | Token | Behavior |
|---------|-------|----------|
| Disabled | `disabled` | Agent does not run. Cluster-only agents default to this on non-cluster deployments. |
| Observe only | `observe_only` | Agent collects and publishes metric observations and health state. No recommendations or actions. |
| Recommend only | `recommend_only` | Agent produces recommendations that appear in the observability surface. No live mutations. |
| Dry run | `dry_run` | Agent evaluates actions and records what it would do, but mutations are suppressed. Evidence is recorded. |
| Live action | `live_action` | Agent may execute mutations, subject to all safety, arbitration, approval, and resource gates. |

Advancing from `dry_run` to `live_action` requires explicit operator approval. The function `ValidateRolloutTransition` enforces this: the `explicit_operator_approval` parameter must be true, and the transition is refused otherwise. This is the gated escalation mechanism — no agent accidentally goes live.

`EffectiveActivationForLifecycle` adjusts the configured profile based on current engine lifecycle mode (for example, reducing effective activation during backup, restore, crash recovery, or read-only modes). The effective activation is always the more restrictive of the configured profile and the lifecycle-derived constraint.

---

## The Fourteen Lifecycle States

Every agent instance moves through a state machine with exactly fourteen states. Transitions are validated by `AgentLifecycleTransitionAllowed` and `ValidateAgentLifecycleTransition`.

| State | Token | Meaning |
|-------|-------|---------|
| Created | `created` | Instance record allocated; not yet registered. |
| Registered | `registered` | Manifest and policy checks passed; instance is known to the runtime. |
| Disabled | `disabled` | Operator or policy has disabled this instance. |
| Observe only | `observe_only` | Running in metric-observation mode only. |
| Recommend only | `recommend_only` | Running; producing recommendations but no actions. |
| Dry run | `dry_run` | Running; evaluating actions and recording evidence without executing mutations. |
| Running | `running` | Running in live-action mode; mutations permitted within bounds. |
| Paused | `paused` | Temporarily suspended by operator request; can be resumed. |
| Safe mode | `safe_mode` | Engine or agent supervisor has detected a risk condition and reduced the agent to safe-mode scope. |
| Quarantined | `quarantined` | Multiple supervision failures or a security/integrity fault has isolated the instance. Recovery requires operator action. |
| Stopping | `stopping` | Graceful shutdown initiated; draining current work. |
| Stopped | `stopped` | Agent has completed shutdown. |
| Retired | `retired` | Instance is superseded by a new generation; evidence record is preserved. |
| Failed | `failed` | The agent encountered an unrecoverable error. |

Quarantine is reached through supervision failure accumulation (tracked in `AgentInstanceRecord.crash_loop_count`, `supervision_failure_count`, and `restart_attempts`) or through an explicit `QuarantineAgentInstance` call. Release from quarantine requires operator action and is not automatic.

---

## Action Result Classes

Every evaluated action resolves to one of seven `AgentActionResultClass` outcomes.

| Result | Token | Meaning |
|--------|-------|---------|
| Accepted | `accepted` | Action was evaluated and executed (or queued for execution). |
| Refused | `refused` | Action was actively declined by a safety check, policy gate, or authority constraint. |
| Suppressed | `suppressed` | Action was not executed because of a conflicting override or arbitration loss; evidence is recorded. |
| Dry run only | `dry_run_only` | Action was evaluated but mutation was suppressed because the profile is `dry_run`. |
| Approval required | `approval_required` | Action cannot proceed without manual operator approval. |
| Failed closed | `failed_closed` | A required gate (dependency, metric, policy, actuator) was unavailable and the agent defaulted to no-mutation. |
| Quarantined | `quarantined` | The requesting agent instance is in the quarantine state; all actions refused. |

`failed_closed` is the runtime default. When any required proof is absent — policy missing, metric stale, actuator route unregistered, security context absent — the action result is `failed_closed` rather than proceeding under uncertainty.

---

## Hard Authority Boundaries

These prohibitions are enforced in source through boolean flags on `AgentEvidenceRecord` and `AgentReplayControlRequest`, and explicitly stated in comments throughout the agent subsystem. No agent in the canonical manifest is permitted to claim or exercise:

- **Transaction finality authority** (`finality_authority = false` in evidence records) — agents cannot commit, abort, or finalize any user transaction.
- **Visibility authority** (`visibility_authority = false`) — agents cannot alter what rows or catalog objects are visible to other sessions.
- **Parser authority** (`parser_authority = false`) — agents cannot interpret SQL text, manipulate the parse-to-SBLR pipeline, or influence query routing.
- **Recovery authority** (`recovery_authority = false`) — agents cannot drive WAL replay, crash recovery, or PITR execution as authority holders. They may observe and recommend.
- **WAL authority** (`wal_authority = false` in replay control) — agent persistence uses the engine's storage authority, not a write-ahead log of its own.
- **Client authority** (`client_authority = false`) — agents cannot impersonate or act on behalf of client sessions.

The `AdaptiveTuningControllerRequest.safety` struct (`AdaptiveTuningSafetyPolicy`) makes these denials explicit for the adaptive tuning subsystem: `parser_or_reference_authority`, `provider_transaction_finality_authority`, `provider_visibility_authority`, `client_autocommit_authority`, and `wal_recovery_authority` are all false by design, and the tuning controller refuses any request that asserts otherwise.

---

## Manual Approval and Break-Glass

`AgentPolicy.require_manual_approval` and `AgentPolicy.require_dry_run_before_live` are the two primary gates for high-consequence actions. When `require_manual_approval` is true, the action produces an `approval_required` result and is held until an operator-level principal with the `obs_agent_action_approve` right explicitly approves it via `ValidateManualApproval`.

Break-glass overrides (`AgentArbitrationOverride`) allow an operator to explicitly allow or suppress specific action UUIDs within a bounded expiry window. Overrides carry their own evidence UUID, require the `obs_agent_override` right, and are recorded in the arbitration evidence chain.

---

## See Also

- [agent_catalog.md](agent_catalog.md) — how each agent is assigned to a tier
- [action_lifecycle_and_arbitration.md](action_lifecycle_and_arbitration.md) — the full action flow from proposal to evidence
- [governance_and_resource_control.md](governance_and_resource_control.md) — rollout profiles and the gated escalation mechanism
- [../Language_Reference/syntax_reference/agent.md](../Language_Reference/syntax_reference/agent.md) — SBsql syntax for controlling agents
