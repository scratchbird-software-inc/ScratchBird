# Agent Management

This page is part of the SBsql Language Reference Manual. It explains the user-facing language contract while preserving the ScratchBird authority model: SQL text parses to SBLR, durable identity is UUID based, descriptors own type behavior, security is materialized from catalog policy, and MGA owns transaction finality.

Generation task: `syntax_reference_agent_management`

Related pages: [Management And Operations](management_and_operations.md), [Security And Privileges](security_and_privilege_statements.md), [Filespace Lifecycle](filespace.md), [Backup, Restore, Replication, And Migration](backup_restore_replication_migration.md), [Cluster-Gated Statements](cluster_gated_statements.md), [Refusal Vectors](refusal_vectors.md), and [Agent Statement EBNF](ebnf/agent_statement.md).

## Purpose

Agents are engine-owned operational actors. They observe metrics, evaluate policy, publish recommendations, request bounded actions, or perform policy-admitted bounded actions. They do not own catalog identity, storage truth, authorization, recovery, parser execution, or transaction finality.

An agent decision is valid only when the engine can prove:

- the agent type is registered in the canonical manifest;
- the instance has a durable identity and policy snapshot;
- the effective user or agent context has the required rights;
- required metrics are present, fresh, trusted, and descriptor-compatible;
- lifecycle state admits the requested activation level;
- the action is safe under resource, cooldown, lease, approval, and feature gates;
- evidence is persisted and redacted according to policy.

The parser can route agent statements to SBLR. The parser cannot execute an agent, approve an action, mutate storage, or treat source text as authority.

## Statement Surface

```ebnf
agent_stmt ::=
      show_agent_statement
    | alter_agent_statement
    | create_agent_override_statement
    | drop_agent_override_statement ;
```

Common examples:

```sql
show agents;
show agents extended;
show agents where agent_type = memory_governor;
show agent memory_governor;
show agent memory_governor metrics;
show agent memory_governor policy;
show agent memory_governor evidence;
show agent memory_governor audit;
show agent actions;
show agent overrides;
alter agent memory_governor start;
alter agent memory_governor pause;
alter agent memory_governor resume;
alter agent memory_governor dry run;
alter agent memory_governor quarantine reason checksum_failure;
alter agent memory_governor attach policy baseline_policy;
alter agent action action_uuid approve;
alter agent action action_uuid cancel;
create agent override for memory_governor suppress page_preallocation until timestamp '2026-06-09 00:00:00' reason operator_hold;
drop agent override override_uuid;
```

Cluster-scoped agent commands are recognized by SBsql, but they are cluster-gated. In public builds they must either return an unsupported message vector or route to the public cluster stub and return an unlicensed/fail-closed message vector.

```sql
show cluster agents;
show cluster agent cluster_autoscale_manager;
alter cluster agent cluster_autoscale_manager drain;
```

## Core Concepts

| Concept | Meaning |
| --- | --- |
| Agent type | Canonical type such as `memory_governor` or `storage_health_manager`. |
| Agent instance | Durable runtime record for one type in one scope. |
| Agent UUID | Engine-owned durable identity for an instance. |
| Scope | The node, database, filespace, workload, index, job, or cluster area the agent may observe or affect. |
| Policy | Durable rules controlling activation, metrics, limits, actions, approvals, and fail-closed behavior. |
| Lease | Runtime guard preventing duplicate runs or stale action execution. |
| Evidence | Redacted operational facts produced by the agent or runtime. |
| Action | A requested or bounded operation evaluated by the engine action path. |
| Override | Explicit operator policy that suppresses, approves, changes, or constrains behavior for a bounded time or scope. |
| Quarantine | Fail-closed state used after crash loops, bad evidence, unsafe policy, stale metrics, or operator action. |

## Authority And Activation Classes

| Class | Meaning | Default behavior |
| --- | --- | --- |
| `observe_only` | Agent can report state and evidence only. | No recommendations or actions. |
| `recommend_only` | Agent can publish recommendations but cannot perform work. | Requires operator or policy route to apply. |
| `request_action` | Agent can request a bounded action through engine-owned action hooks. | Engine validates and may refuse, defer, or require approval. |
| `direct_bounded_action` | Agent can perform a bounded action when policy, metrics, lease, lifecycle, and safety gates admit it. | Often starts in `dry_run` until elevated. |

Activation profiles:

| Profile | Contract |
| --- | --- |
| `disabled` | Agent cannot run. |
| `observe_only` | Agent can observe and publish evidence. |
| `recommend_only` | Agent can publish recommendations. |
| `dry_run` | Agent can evaluate actions but must not perform durable live action. |
| `live_action` | Agent may perform admitted bounded actions. Escalation to this profile requires explicit policy approval. |

Lifecycle modes such as read-only, maintenance, backup, restore, shutdown, crash recovery, repair, archive hold, and PITR can downgrade or disable live action.

## Management And Control Model

Agents are controlled by the engine agent runtime. Human operators, administrative sessions, jobs, and internal engine services can request management changes only through authorized SBsql or internal API routes.

The control chain is:

1. SBsql parses an agent statement.
2. The binder resolves agent type, instance, policy, action, override, and result descriptor.
3. Security checks the effective user or agent UUID and required right.
4. SBLR admission verifies the operation family and result shape.
5. The engine agent management API evaluates the request.
6. The agent runtime applies lifecycle, policy, lease, feature, metric, approval, and resource gates.
7. The result is persisted as evidence or returned as a message vector.

Human command precedence wins over agent action. Emergency shutdown, quarantine, disable, safe mode, recovery fencing, and protected policy refusal must override agent recommendations and action requests.

## Agent Statement Details

### Listing And Inspection

```sql
show agents;
show agents extended;
show agents where agent_type = memory_governor;
show agent memory_governor;
```

Expected result fields include agent type, deployment, scope, authority class, activation profile, lifecycle state, policy UUID, instance UUID, lease state, readiness, feature availability, last diagnostic, and redacted evidence summary.

### Metrics, Policy, Evidence, And Audit

```sql
show agent memory_governor metrics;
show agent memory_governor policy;
show agent memory_governor evidence;
show agent memory_governor audit;
```

These surfaces require separate rights because they can reveal operational state. Metrics must be fresh and trusted before action. Policy output must redact protected fields. Evidence and audit output must preserve tamper-evidence and redaction policy.

### Lifecycle Control

```sql
alter agent memory_governor start;
alter agent memory_governor stop;
alter agent memory_governor pause;
alter agent memory_governor resume;
alter agent memory_governor drain;
alter agent memory_governor restart;
alter agent memory_governor enable;
alter agent memory_governor disable;
alter agent memory_governor dry run;
alter agent memory_governor set mode recommend_only;
```

Lifecycle changes require `OBS_AGENT_CONTROL`. They must be refused when the target is unknown, hidden, not controllable, quarantined, recovery-fenced, or when escalation requires approval.

### Quarantine

```sql
alter agent memory_governor quarantine reason checksum_failure;
alter agent memory_governor unquarantine reason operator_reviewed;
```

Quarantine prevents unsafe action and clears active leases. Unquarantine requires explicit authority and must not hide the diagnostic history that caused quarantine.

### Policy Attachment And Simulation

```sql
alter agent memory_governor attach policy baseline_policy;
alter agent memory_governor detach policy baseline_policy;
alter agent memory_governor validate policy baseline_policy;
alter agent memory_governor simulate policy baseline_policy;
alter agent memory_governor apply policy baseline_policy;
alter agent memory_governor rollback policy baseline_policy;
```

Policy changes must validate schema, scope, activation, metric dependencies, action permissions, approval requirements, rollout rules, and protected fields. Applying a policy that raises activation must require explicit approval.

### Actions And Overrides

```sql
show agent actions;
show agent overrides;
alter agent action action_uuid approve;
alter agent action action_uuid cancel;
alter agent action action_uuid retry;
alter agent action action_uuid suppress;
create agent override for memory_governor suppress page_preallocation until timestamp '2026-06-09 00:00:00' reason operator_hold;
drop agent override override_uuid;
```

Actions require idempotency keys, evidence, safety checks, cooldown checks, and outcome verification. Unverified live outcomes fail closed. Overrides must have scope, reason, expiry, and audit evidence.

## Canonical Agent Inventory

The following table is the public SBsql inventory of canonical agent types.

| Agent | Deployment | Scope | Authority | Default | Purpose | Responsibilities | Controlled By | Why It Exists |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `node_resource_agent` | local | `node` | observe only | observe only | Observe node-level resource condition. | Publishes CPU, memory, storage, process, and runtime suitability evidence. | Local engine runtime, node policy, authorized operations role. | Gives operators and other agents trusted local resource evidence without granting action authority. |
| `metrics_registry_manager` | both | `node/database/cluster` | direct bounded action | dry run | Maintain metric registry quality. | Validates metric schemas, rejects invalid samples, records freshness, and exposes metric readiness. | Engine metric registry policy; cluster authority where cluster scope is active. | Prevents stale, malformed, or untrusted metrics from driving decisions. |
| `storage_health_manager` | local | `node/database/filespace` | recommend only | recommend only | Assess storage health. | Reports degraded filespaces, sync risk, growth risk, page pressure, and repair/refusal evidence. | Engine storage policy and authorized storage operators. | Keeps storage advice separate from storage authority. |
| `filespace_capacity_manager` | local | `node/database/filespace` | request action | recommend only | Manage filespace capacity pressure. | Recommends or requests growth, shrink readiness, and capacity interventions. | Engine filespace policy, storage management route, human approval for unsafe changes. | Allows capacity automation while the engine keeps allocation authority. |
| `page_allocation_manager` | local | `database/filespace/page_family/page_type` | request action | recommend only | Assist page preallocation and relocation decisions. | Requests page preallocation, relocation, and allocation-family readiness through action hooks. | Engine storage allocator, policy, metrics, cooldown, and lifecycle fences. | Reduces allocation pressure without letting an agent allocate pages directly. |
| `memory_governor` | local | `node/database/session/workload` | direct bounded action | dry run | Govern memory pressure. | Evaluates memory budgets, spill pressure, cache pressure, workload budgets, and bounded throttles. | Engine memory policy, workload policy, lease, and authorized operations role. | Keeps memory response deterministic and auditable under load. |
| `index_health_manager` | local | `database/index` | recommend only | recommend only | Assess index readiness and health. | Reports stale evidence, rebuild need, divergence risk, and maintenance recommendations. | Engine index policy and index maintenance authority. | Indexes are evidence only; this agent recommends but does not make row truth. |
| `cluster_autoscale_manager` | cluster | `cluster` | request action | disabled | Recommend cluster autoscale decisions. | Evaluates cluster-scale resource pressure and autoscale requests. | Cluster provider boundary and cluster operations authority. | Autoscale is cluster-classified and must fail closed in public standalone use. |
| `admission_control_manager` | both | `database/cluster/workload` | direct bounded action | dry run | Control work admission under pressure. | Applies bounded admission, throttle, and refusal decisions from workload metrics. | Engine admission policy; cluster authority when cluster-scoped. | Prevents overload while keeping admission policy explicit and auditable. |
| `parser_interface_manager` | local | `node/parser/interface` | request action | recommend only | Monitor parser interfaces and pools. | Reports parser pool readiness, crash loops, reload needs, and assignment refusals. | Listener/manager policy, parser package registry, authorized operations role. | Keeps parser health visible without granting parser authority over engine state. |
| `transaction_pressure_manager` | both | `database/cluster` | request action | recommend only | Observe transaction and cleanup pressure. | Reports long readers, cleanup blockers, snapshot age, and pressure recommendations. | Engine transaction manager and cleanup policy. | Protects MGA cleanup and visibility from silent drift. |
| `storage_version_cleanup_agent` | local | `database/filespace/page_family/row_version` | direct bounded action | dry run | Assist storage version cleanup. | Evaluates cleanup-safe windows and bounded cleanup requests. | Engine cleanup policy, MGA visibility, recovery fences. | Cleanup can reclaim space only when no visible version can be lost. |
| `cleanup_archive_manager` | both | `database/cluster` | direct bounded action | dry run | Coordinate cleanup and archive pressure. | Holds or releases cleanup/archive work based on policy and evidence. | Engine archive policy and cluster authority where applicable. | Prevents cleanup and archival work from crossing recovery or retention boundaries. |
| `policy_recommendation_manager` | both | `database/cluster` | recommend only | recommend only | Recommend policy changes. | Produces policy recommendations, validates scope, and records review evidence. | Security and policy management authority. | Keeps policy tuning advisory until an authorized route applies it. |
| `distributed_query_metrics_agent` | cluster | `cluster/query` | observe only | disabled | Observe distributed query metrics. | Publishes cluster query fanout, fragment, merge, and latency evidence. | Cluster provider boundary. | Distributed query metrics are cluster-classified and unavailable without cluster authority. |
| `remote_query_routing_agent` | cluster | `cluster/query/route` | request action | disabled | Recommend or request cluster route changes. | Evaluates cluster route evidence and routing requests. | Cluster provider route authority. | Routing across cluster participants requires provider admission. |
| `runtime_learning_agent` | local | `database/optimizer` | recommend only | recommend only | Learn runtime plan and workload feedback. | Produces optimizer learning recommendations from trusted samples. | Optimizer policy and runtime learning gates. | Improves planning evidence without making optimizer evidence final authority. |
| `support_bundle_triage_agent` | both | `node/database/cluster/support` | request action | recommend only | Triage support bundle evidence. | Recommends bundle scope, redaction, manifest checks, and evidence grouping. | Support bundle policy and authorized support operators. | Produces useful diagnostics while preserving secret redaction. |
| `cluster_scheduler_manager` | cluster | `cluster/jobs` | request action | disabled | Coordinate cluster job scheduling. | Evaluates cluster job placement and scheduling requests. | Cluster provider job authority. | Cluster job scheduling is unavailable without cluster authority. |
| `job_control_manager` | both | `database/cluster/jobs` | request action | recommend only | Manage database-local and cluster job control requests. | Starts, pauses, resumes, cancels, and reports jobs where policy admits. | Job scheduler, workload quota, operations role. | Centralizes background job control and audit evidence. |
| `backup_manager` | both | `database/cluster/backup` | request action | recommend only | Coordinate backup operations. | Requests backup start, pause, cancellation, progress reporting, and policy checks. | Backup policy, storage policy, and operations role. | Backup has durability and disclosure risk, so action is policy-gated. |
| `archive_manager` | both | `database/cluster/archive` | direct bounded action | dry run | Manage archive flow. | Tracks archive windows, holds slices, and performs bounded archive-state actions. | Archive policy, retention policy, and recovery fences. | Archive work must not outrun recovery, retention, or cleanup safety. |
| `restore_drill_manager` | both | `database/cluster/restore` | request action | recommend only | Coordinate restore drills. | Requests drill setup, validation, progress, and result evidence. | Restore policy and authorized operators. | Restore readiness needs proof without risking production state. |
| `pitr_manager` | both | `database/cluster/pitr` | request action | recommend only | Manage point-in-time recovery readiness. | Evaluates PITR reachability, window evidence, and recovery-point requests. | PITR policy, archive policy, recovery authority. | Recovery planning must be explicit and fail closed when evidence is incomplete. |
| `identity_manager` | both | `database/cluster/security` | request action | recommend only | Assist identity lifecycle management. | Reports identity state, policy risk, credential rotation needs, and security recommendations. | Security policy and security administration authority. | Identity actions are security-sensitive and must not be autonomous by default. |
| `session_control_manager` | both | `database/cluster/session` | request action | recommend only | Manage session control recommendations. | Recommends drain, cancel, disconnect, and session policy changes. | Session registry, security policy, operations role. | Protects users and transactions while enabling controlled operational response. |
| `alert_manager` | both | `node/database/cluster` | direct bounded action | dry run | Emit alerts and operational events. | Converts validated evidence into local alerts, notifications, and alert state. | Alerting policy and diagnostics policy. | Alerts are bounded actions that should not mutate database truth. |
| `export_adapter_manager` | both | `node/database/cluster/export` | request action | recommend only | Govern export adapters. | Recommends export admission, backpressure, redaction, and adapter readiness. | Export policy, bridge/export authority, security policy. | Export can leak data, so all action requires explicit policy. |
| `cluster_upgrade_manager` | cluster | `cluster/upgrade` | request action | disabled | Coordinate cluster upgrade readiness. | Evaluates upgrade prerequisites and cluster upgrade requests. | Cluster provider upgrade authority. | Cluster upgrade is unavailable in standalone public execution. |

## Filespace Agents

Filespace agents are a specific registration pattern for filespace lifecycle assistance.

```sql
create filespace agent primary_growth_agent
for filespace primary
with type filespace_capacity_manager;
```

A filespace agent can assist growth, shrink readiness, relocation, and health decisions. It does not allocate pages, move pages, mark storage healthy, or promote filespaces by itself. Storage authority remains in the engine storage subsystem.

## System Views

`sys.agents` exposes authorized local agent rows. Cluster-scoped rows are exposed only through cluster-authorized projections and are cluster-gated in public builds.

Expected `sys.agents` fields include:

- agent type;
- instance UUID;
- scope;
- deployment;
- authority class;
- default activation;
- current lifecycle state;
- policy UUID;
- lease state;
- feature availability;
- last diagnostic;
- redacted evidence summary.

## Security Rights

| Right | Used For |
| --- | --- |
| `OBS_AGENT_STATE_READ` | List agents and read basic state. |
| `OBS_AGENT_EVIDENCE_READ` | Read evidence and audit projections. |
| `OBS_AGENT_CONTROL` | Start, stop, pause, resume, drain, enable, disable, quarantine, or unquarantine. |
| `OBS_AGENT_POLICY_CONTROL` | Attach, detach, validate, simulate, apply, or roll back agent policy. |
| `OBS_AGENT_RECOMMENDATION_READ` | Read recommendations and action lists. |
| `OBS_AGENT_ACTION_CANCEL` | Cancel pending or running agent actions. |
| `OBS_AGENT_ACTION_APPROVE` | Approve, retry, or apply controlled actions. |
| `OBS_AGENT_OVERRIDE` | Create, update, or drop operator overrides. |
| `OBS_METRICS_READ_FAMILY` | Read metric families needed by one agent. |
| `OBS_METRICS_READ_ALL` | Read broad operational metrics. |
| `OBS_CLUSTER_CONTROL` | Control cluster-scoped agents where cluster routing is admitted. |

Rights are examples of the public contract. Policy can require narrower or stronger rights for a specific target, scope, or build profile.

## Action Hooks

Agents request bounded work through engine-owned hooks. The hook validates policy, evidence sink availability, metric freshness, cooldown, manual override, lifecycle fences, dry-run state, and target descriptors before accepting, deferring, or refusing the action.

Public action-hook families include:

| Hook | Typical requester | Engine authority retained |
| --- | --- | --- |
| Page preallocation | `page_allocation_manager` | Page allocation and page ownership. |
| Page relocation | `page_allocation_manager` | Page movement and recovery safety. |
| Filespace growth | `filespace_capacity_manager` | Filespace allocation, growth, sync, and metadata updates. |
| Filespace shrink readiness | `filespace_capacity_manager` | Shrink admission and safety fencing. |
| Index delta merge | `index_health_manager` | Index merge, row recheck, and index evidence consistency. |
| Index rebuild or shadow build | `index_health_manager` | Index creation, shadow state, and final promotion. |

## Diagnostics And Refusals

| Condition | Expected diagnostic class |
| --- | --- |
| Unknown agent type | Agent resolution failure. |
| Hidden agent scope | Sandbox or disclosure denial. |
| Missing state read right | Agent inspection denied. |
| Missing control right | Agent control denied. |
| Cluster-scoped agent in standalone context | Cluster-gated unsupported or unlicensed message vector. |
| Metrics stale or absent | Metric stale or dependency failure. |
| Policy missing or invalid | Policy validation failure. |
| Unsafe activation escalation | Explicit approval required. |
| Duplicate lease | Run lease refused. |
| Crash loop | Agent quarantined. |
| Cooldown active | Action deferred or refused. |
| Lifecycle fence active | Action refused. |
| Manual approval missing | Approval required. |
| Unverified action outcome | Failed closed. |
| Protected evidence requested | Evidence redacted or denied. |

## Proof Expectations

The agent proof suite should include:

- canonical manifest count and drift checks for all 29 agent types;
- `SHOW AGENTS`, filtered `SHOW AGENTS`, `SHOW AGENT`, and `SHOW AGENTS EXTENDED`;
- metric, policy, evidence, audit, action, and override inspection;
- start, stop, pause, resume, drain, enable, disable, dry-run, run, configure, quarantine, and unquarantine routes;
- policy attach, detach, validate, simulate, apply, and rollback;
- action approve, cancel, retry, suppress, and failed-closed denial;
- override create, update, expiry, and drop;
- `sys.agents` visibility and redaction;
- cluster agent fail-closed behavior in standalone public builds;
- lease acquisition, duplicate lease refusal, crash-loop quarantine, cooldown, lifecycle fence, stale metric, corrupt metric, invalid policy, queue corruption, and restart-mid-action fault cases;
- proof that parser routes to SBLR but never executes agent action;
- proof that agent action never owns transaction finality, catalog identity, storage truth, security authority, recovery authority, parser authority, or row visibility.

## Verification Checklist

| Check | Required outcome |
| --- | --- |
| Parse | Agent statements are recognized by SBsql. |
| Bind | Agent type, scope, instance, action, policy, override, and descriptors resolve. |
| Authorize | Effective user or agent UUID has the required right. |
| Admit | SBLR agent management route is accepted by the verifier. |
| Gate | Metrics, policy, feature, lifecycle, lease, cooldown, and approval gates are checked. |
| Execute | Engine-owned management API performs the admitted action or refuses. |
| Persist | Evidence, audit, and durable runtime state are recorded where required. |
| Redact | Protected fields are omitted or redacted. |
| Fail Closed | Unsupported, unsafe, cluster-scoped, stale, or unauthorized paths return message vectors. |
