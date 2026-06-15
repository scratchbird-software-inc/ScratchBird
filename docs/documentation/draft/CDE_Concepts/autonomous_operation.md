# Autonomous Operation

## Purpose

This page describes how ScratchBird manages itself through an autonomous agent
runtime — what kinds of tasks agents handle, how their authority is governed,
and why this matters for the CDE design. This is a summary and orientation.
The full treatment is in the Agent Runtime Guide.

For comprehensive detail on the agent runtime architecture, all agent types,
authority governance, activation profiles, dry-run behavior, approval workflows,
and evidence requirements, see
[../Agent_Runtime_Guide/README.md](../Agent_Runtime_Guide/README.md).

**This is a draft.** Nothing here is a production readiness claim or a guarantee
of autonomous behavior in any specific build configuration.

---

## Why Autonomous Operation In A CDE

A system that converges many data models, many client dialects, and many
operational disciplines creates a management surface that is larger and more
complex than any single-model engine. Keeping that surface operational — cleaning
up old row versions, managing storage capacity, running backup drills, monitoring
health, tuning execution parameters, handling session pressure — would require
substantial ongoing human intervention if it were all manual.

ScratchBird addresses this through a governed autonomous agent runtime: a set of
agents that observe engine state, decide what to do, and execute within strictly
bounded authority. The key word is *governed* — agents operate within a defined
authority structure and can be configured to require operator approval before
acting. The engine does not give agents unlimited authority over itself.

---

## The Agent Manifest

The autonomous agent runtime is defined by a single machine-readable manifest
in `src/core/agents/agent_runtime_manifest.def`. The manifest lists 30 agent
types (verified by count), each with:

- A `type_id` (a stable string identifier).
- A deployment scope: `local` (single-node only), `cluster` (cluster only),
  or `both`.
- An operational scope string: the engine subsystems the agent may observe or
  touch (for example, `"database/filespace/page_family/row_version"` or
  `"node/database/cluster"`).
- A **production authority** level — what the agent may do in a production
  deployment.
- An **activation** level — the mode in which the agent starts by default.

### Authority Levels

The manifest uses the following authority levels:

| Authority level | What it permits |
|----------------|-----------------|
| `observe_only` | Read and report; no writes |
| `recommend_only` | Produce recommendations; no direct action |
| `request_action` | Submit a request that the engine or operator must approve |
| `direct_bounded_action` | Execute a bounded predefined action directly |
| `disabled` | Not active in this configuration |
| `dry_run` | Run in simulation mode; log what would happen without doing it |

These levels form the authority ladder. No agent has unlimited authority.
Agents that can take direct action (`direct_bounded_action`) are bounded
by predefined action envelopes; they cannot improvise outside their scope.

### Examples From The Manifest

| Agent | Deployment | Production authority | Default activation |
|-------|-----------|---------------------|-------------------|
| `node_resource_agent` | local | `observe_only` | `observe_only` |
| `storage_version_cleanup_agent` | local | `direct_bounded_action` | `dry_run` |
| `backup_manager` | both | `request_action` | `recommend_only` |
| `restore_drill_manager` | both | `request_action` | `recommend_only` |
| `memory_governor` | local | `direct_bounded_action` | `dry_run` |
| `admission_control_manager` | both | `direct_bounded_action` | `dry_run` |
| `policy_recommendation_manager` | both | `recommend_only` | `recommend_only` |
| `cluster_autoscale_manager` | cluster | `request_action` | `disabled` |
| `identity_manager` | both | `request_action` | `recommend_only` |
| `pitr_manager` | both | `request_action` | `recommend_only` |

The default activation levels are conservative — most agents start in
`recommend_only` or `dry_run` mode. Operators can promote agents to higher
authority levels through configuration, within the bounds of the production
authority ceiling.

---

## What Agents Do

The agent manifest covers the engine's core self-management disciplines:

**Storage and MGA maintenance:**
`storage_version_cleanup_agent` and `cleanup_archive_manager` handle the
reclamation of old MGA row versions and archive management. Without cleanup,
version chains grow indefinitely; these agents enforce the cleanup horizon
computed by the transaction cleanup horizon service.

**Capacity and resource governance:**
`filespace_capacity_manager`, `page_allocation_manager`, and `memory_governor`
observe and respond to storage and memory pressure.

**Backup and recovery:**
`backup_manager`, `restore_drill_manager`, `archive_manager`, and `pitr_manager`
manage backup scheduling, automated restore drills (verifying that backups are
actually restorable), archive lifecycle, and point-in-time recovery coordination.

**Admission control:**
`admission_control_manager` and `transaction_pressure_manager` handle workload
admission under pressure conditions.

**Index and optimizer health:**
`index_health_manager` and `runtime_learning_agent` monitor index health and
provide optimizer feedback.

**Security and identity:**
`identity_manager` and `session_control_manager` handle identity lifecycle and
session governance.

**Diagnostics and support:**
`support_bundle_triage_agent` and `alert_manager` handle diagnostic evidence
collection and alerting.

**Cluster coordination:**
`cluster_autoscale_manager`, `cluster_scheduler_manager`, `cluster_upgrade_manager`,
`distributed_query_metrics_agent`, and `remote_query_routing_agent` handle
cluster-scope operations (disabled by default in local deployments).

---

## Governed, Not Autonomous-At-Any-Cost

The design emphasis is on governance, not on maximum autonomy. A few structural
properties enforce this:

- **Dry-run by default for consequential agents.** Agents with
  `direct_bounded_action` production authority typically start in `dry_run`
  activation — they log what they would do without doing it, allowing operators
  to observe behavior before promoting to live action.

- **Evidence requirements.** Agents that take or recommend action are required
  to produce evidence records — structured proof that they observed the condition
  they acted on. This evidence is queryable and supports auditability.

- **Single source of truth for agent identity.** The manifest is the canonical
  agent definition. A drift gate (`AEIC_GENERATED_AGENT_MANIFEST_DRIFT_GATE`)
  is checked in CI to detect divergence between the manifest and generated code.

- **Approval path for consequential actions.** Agents at `request_action` level
  submit requests that can be configured to require explicit operator approval
  before execution.

---

## For The Full Treatment

The Agent Runtime Guide covers all of the above in depth, including:
- Full agent descriptions and their behavioral contract.
- Authority ladder detail and promotion/demotion configuration.
- Dry-run, approval, and evidence workflows.
- Cluster-scoped agents and their relationship to the cluster boundary.
- Diagnostic and observability surfaces for agent activity.

See [../Agent_Runtime_Guide/README.md](../Agent_Runtime_Guide/README.md).
