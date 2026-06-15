# Action Lifecycle and Arbitration

## Purpose

This page traces the path of a single agent action from initial proposal through safety evaluation, dry-run, arbitration, optional manual approval, execution, and the recording of evidence. It also defines the full arbitration priority model — the ordered rules the engine uses to resolve competing actions — and explains how operators can influence that model through suppression and force-allow overrides.

All enumeration names and function signatures are verified against `project/src/core/agents/agent_runtime.hpp`.

---

## The Action Lifecycle

Every agent action follows the same ordered pipeline. A failure at any stage produces an `AgentActionResultClass` outcome and records evidence; nothing is silently dropped.

### Stage 1 — Proposal

An agent that holds `request_action` or `direct_bounded_action` authority constructs an `AgentActionRequest`. The request carries:

- A unique `action_uuid`
- The `agent_type_id` and `instance_uuid` of the submitting agent
- An `AgentActionClass` (for example, `direct_bounded_action` or `recommendation`)
- An `actuator_id` and `operation_id` identifying the registered actuator route
- An `idempotency_key`
- A `dry_run` flag (always `true` until all gates pass)
- Input parameters as a string map

At this point no mutation has occurred. The engine has received a candidate, nothing more.

### Stage 2 — Safety Precondition Evaluation

Before the request reaches arbitration, the engine checks:

- **Agent lifecycle state** — the instance must not be in `quarantined`, `stopped`, `retired`, or `failed` states.
- **Safe-mode gate** (`ValidateAgentSafeMode`) — if the instance is in `safe_mode`, the action scope is restricted.
- **Actuator route registration** — the actuator identified by `actuator_id` must be registered and not degraded.
- **Feature gate** (`EvaluateAgentFeatureAvailability`) — the capability must be `available` in the current runtime context.
- **Metric dependency resolution** — required metric families must be present, fresh, and at the required source quality.
- **Policy dependency state** — required policy families must be present, valid, and scope-compatible.
- **Security context** — the runtime context must include a security context; its absence causes `failed_closed`.
- **Action safety budget** (`ValidateActionSafetyBudget`) — the number of actions already used in the current window must not exceed `policy.action_budget_per_window`.
- **Overhead gate** (`ValidateAgentOverheadGate`) — runtime, metric-query, and evidence-write totals must remain within bounds.
- **Human command precedence** (`ValidateHumanCommandPrecedence`) — any directly issued human command takes precedence over agent proposals of the same class.

If any required gate is absent or fails, the action resolves to `failed_closed` immediately. This is not a refusal due to policy judgment; it is a proof-not-present condition.

### Stage 3 — Dry-Run Evaluation

An action is evaluated in dry-run mode first (`BuildDryRunDecision`) regardless of the agent's activation profile. In dry-run mode:

- The full decision logic executes
- Mutation is suppressed
- An `AgentEvidenceRecord` is written with result state `dry_run_only`
- The evidence record is tamper-chained (see [evidence_explainability_and_safety.md](evidence_explainability_and_safety.md))

If the agent's `AgentActivationProfile` is `dry_run`, the action resolves to `dry_run_only` and stops here. Live execution requires the profile to be `live_action`.

### Stage 4 — Arbitration

When multiple action candidates are active over the same scope, `ArbitrateAgentActionCandidates` selects at most one. The outcome is one of four `AgentArbitrationOutcome` values:

| Outcome | Token | Meaning |
|---------|-------|---------|
| Winner executes | `winner_executes` | One candidate won; the rest are suppressed. |
| Both denied | `both_denied` | No candidate satisfied the priority rules. |
| Operator review required | `operator_review_required` | The tie was unresolvable without human input. |
| Suppressed by override | `suppressed_by_override` | An active `AgentArbitrationOverride` forced the outcome. |

Arbitration is described in detail in the next section.

### Stage 5 — Manual Approval (when required)

If `AgentPolicy.require_manual_approval` is `true`, or if the action contract marks `manual_approval_required`, the action produces an `approval_required` result and enters the approval queue. It remains there until an operator-level principal holding the `obs_agent_action_approve` right calls `ValidateManualApproval` for that action UUID.

If `AgentPolicy.require_dry_run_before_live` is `true`, at least one successful dry-run cycle must appear in the evidence history before the action can proceed to live execution regardless of approval status.

**Break-glass.** An `AgentArbitrationOverride` with the relevant `action_uuid` in its `allowed_action_uuids` list can clear the approval gate for actions in the `approval_required` queue, subject to the override's own expiry and the `obs_agent_override` right requirement.

### Stage 6 — Resource Budget Evaluation

`EvaluateAgentResourceBudget` checks the 14-dimension resource budget (see [governance_and_resource_control.md](governance_and_resource_control.md)). If the budget decision is anything other than `allow`, the action is deferred, refused, or drained without mutation.

### Stage 7 — Execution

With all gates cleared, `EvaluateAgentAction` sets `dry_run = false` on the request and dispatches through the actuator route. The actuator operates with the engine's bounded-action authority; it does not acquire independent authorization.

### Stage 8 — Outcome Verification and Evidence

`VerifyActionOutcome` compares the actuator's reported success against whether the intended state is actually observed. An outcome that reports success without observed state change is recorded as `failed_closed`. An outcome that reports failure with observed state change triggers compensation logic.

The final `AgentEvidenceRecord` is written with the resolved `AgentActionResultClass`, the full tamper chain, and the `outcome_verification_evidence_uuid`. This record is the authoritative audit trail for the action.

---

## The Seven Action Result Classes

| Result | Token | Meaning |
|--------|-------|---------|
| Accepted | `accepted` | Evaluated and executed (or queued for execution). |
| Refused | `refused` | Actively declined by a safety check, policy gate, or authority constraint. |
| Suppressed | `suppressed` | Not executed due to a conflicting override or arbitration loss; evidence is recorded. |
| Dry run only | `dry_run_only` | Evaluated but mutation suppressed because the profile is `dry_run`. |
| Approval required | `approval_required` | Held pending explicit operator approval. |
| Failed closed | `failed_closed` | A required proof was absent; defaulted to no-mutation. |
| Quarantined | `quarantined` | The submitting agent instance is in the `quarantined` lifecycle state; all actions refused. |

`failed_closed` is the default. The engine never proceeds under uncertainty when a required gate is absent.

---

## Arbitration Priority Ordering

When two or more action candidates compete, the arbitrator walks a deterministic priority-rule ladder (`AgentArbitrationPriorityRule`). The first rule that resolves the competition terminates the walk.

### Priority-Rule Ladder

| Step | Rule | Token | What it does |
|------|------|-------|--------------|
| 1 | No actions | `no_actions` | If there are zero candidates, outcome is `both_denied`. |
| 2 | Safety precondition failed | `safety_precondition_failed` | Any candidate with `safety_preconditions_passed = false` is eliminated first. |
| 3 | Single action | `single_action` | If exactly one candidate remains, it wins without further comparison. |
| 4 | Override — suppression | `override_suppression` | Active overrides with suppressed action UUIDs eliminate matching candidates. |
| 5 | Override — right required | `override_right_required` | If an override is present but the requester lacks `obs_agent_override`, the override is invalid and both are denied. |
| 6 | Override — authority forbidden | `override_authority_forbidden` | An override cannot elevate a candidate beyond its authority ceiling; if attempted, it is rejected. |
| 7 | Action class priority | `action_class_priority` | Candidates are ranked by `AgentArbitrationActionClass` priority order (see below). Higher priority wins. |
| 8 | Evidence quality | `evidence_quality` | Among candidates at the same class, the one with the higher `evidence_quality` score wins. |
| 9 | Exact tie — operator review | `exact_tie_operator_review` | If two candidates are indistinguishable, outcome is `operator_review_required` and an operator-review action record is created. |

### Arbitration Action Class Priority Order

`AgentArbitrationActionClass` values are compared by `AgentArbitrationActionClassPriority`. Higher priority wins:

| Priority | Class | Token |
|----------|-------|-------|
| 1 (highest) | Protect correctness | `protect_correctness` |
| 2 | Protect security | `protect_security` |
| 3 | Protect durability | `protect_durability` |
| 4 | Protect availability | `protect_availability` |
| 5 | Reduce pressure | `reduce_pressure` |
| 6 | Optimize performance | `optimize_performance` |
| 7 (lowest) | Reduce cost | `reduce_cost` |

An action with class `protect_correctness` always beats one with class `reduce_pressure` at the same scope, regardless of evidence quality.

### Risk and Reversibility

Each arbitration candidate carries two additional attributes that qualify the winner's profile:

**AgentArbitrationRisk** — `low`, `medium`, `high`, or `critical`. Risk is informational for the arbitrator but is surfaced in the evidence record for operator review. High-risk or critical-risk winners may trigger additional approval requirements depending on policy configuration.

**AgentArbitrationReversibility** — `reversible`, `bounded_reversible`, or `irreversible`. Irreversible actions receive additional scrutiny in the evidence record. An irreversible action at high or critical risk is the canonical trigger for requiring manual approval.

---

## Overrides: Suppression and Force-Allow

Operators can install `AgentArbitrationOverride` records to influence arbitration without modifying policy. An override requires:

- The `obs_agent_override` right
- A specified `expires_at_microseconds` (overrides are always time-bounded)
- An `evidence_uuid` referencing the override-creation evidence record

An override can contain:

- `suppressed_action_uuids` — specific action UUIDs that the arbitrator will eliminate via `override_suppression`
- `allowed_action_uuids` — specific action UUIDs that can bypass the normal arbitration competition

A `renewal_rule` and `rollback_rule` can be attached. The rollback rule specifies what happens if an operator clears the override before it expires — for example, whether any queued suppressed actions are released.

Overrides are scoped to a `scope_uuid`. An override for one database scope does not affect another database. The `active` flag on the override record allows operators to deactivate an override without deleting it, preserving the evidence trail.

---

## See Also

- [authority_and_activation_model.md](authority_and_activation_model.md) — action result classes, lifecycle states, manual approval mechanics
- [governance_and_resource_control.md](governance_and_resource_control.md) — resource budget evaluation, rollout profiles
- [evidence_explainability_and_safety.md](evidence_explainability_and_safety.md) — tamper-evident evidence chain, dry-run mode, replay quarantine
- [../Language_Reference/syntax_reference/agent.md](../Language_Reference/syntax_reference/agent.md) — SBsql syntax for submitting approvals and managing overrides
- [../CDE_Concepts/autonomous_operation.md](../CDE_Concepts/autonomous_operation.md) — why governed autonomous action matters in a convergent engine
