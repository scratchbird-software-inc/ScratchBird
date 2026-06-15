# Agent Catalog

## Purpose

This page enumerates the complete, verified set of agents declared in the canonical agent manifest (`project/src/core/agents/agent_runtime_manifest.def`). The manifest is the single source of truth; the runtime manifest builder and a manifest-drift gate both read it. No agent may operate in the engine without a manifest entry.

The total verified count is **29 agents**.

Columns: **Agent** (type_id token), **Deployment** (local / cluster / both), **Scope**, **Authority Class**, **Default Activation**, **Purpose**.

Cluster-only agents (`deployment = cluster`) are automatically `disabled` on deployments without a cluster provider. Attempting to activate them without cluster authority fails closed.

---

## Domain Groups

Agents are presented by functional domain for readability. The ordering within each domain follows the manifest file.

---

### Resource and Capacity

| Agent | Deployment | Scope | Authority Class | Default Activation | Purpose |
|-------|-----------|-------|-----------------|-------------------|---------|
| `node_resource_agent` | local | `node` | `observe_only` | `observe_only` | Observes node-level resource state (CPU, memory, I/O) and publishes metrics for consumption by other agents. Takes no action. |
| `filespace_capacity_manager` | local | `node/database/filespace` | `request_action` | `recommend_only` | Monitors filespace utilization and requests capacity adjustments when available pages fall below configured thresholds. |
| `page_allocation_manager` | local | `database/filespace/page_family/page_type` | `request_action` | `recommend_only` | Coordinates page allocation and deallocation within a filespace; notifies the filespace capacity manager when free-page reserve falls to the notification threshold. |
| `cluster_autoscale_manager` | cluster | `cluster` | `request_action` | `disabled` | Requests cluster scaling operations based on aggregate workload metrics. Cluster-only; disabled without cluster authority. |

---

### Storage Health and Maintenance

| Agent | Deployment | Scope | Authority Class | Default Activation | Purpose |
|-------|-----------|-------|-----------------|-------------------|---------|
| `storage_health_manager` | local | `node/database/filespace` | `recommend_only` | `recommend_only` | Evaluates storage health indicators and publishes recommendations for operator review. Does not initiate repair actions directly. |
| `storage_version_cleanup_agent` | local | `database/filespace/page_family/row_version` | `direct_bounded_action` | `dry_run` | Cleans up obsolete row versions (version chain debt) within policy-bounded work windows. This is the relational cleanup worker within the broader cleanup debt scheduler. |
| `cleanup_archive_manager` | both | `database/cluster` | `direct_bounded_action` | `dry_run` | Coordinates archiving of completed cleanup work and manages the lifecycle of cleanup-related catalog records across local and cluster scopes. |

---

### Memory

| Agent | Deployment | Scope | Authority Class | Default Activation | Purpose |
|-------|-----------|-------|-----------------|-------------------|---------|
| `memory_governor` | local | `node/database/session/workload` | `direct_bounded_action` | `dry_run` | Monitors and governs memory usage across the engine, sessions, and workloads. Can apply direct bounded memory limits within policy. |

---

### Transaction and Session Pressure

| Agent | Deployment | Scope | Authority Class | Default Activation | Purpose |
|-------|-----------|-------|-----------------|-------------------|---------|
| `transaction_pressure_manager` | both | `database/cluster` | `request_action` | `recommend_only` | Monitors long-idle transactions and applies a policy-configured escalation ladder: warn, request restart, request reauth, request cancel, and (if policy explicitly permits) force action. The engine owns the transaction; the agent requests. |
| `session_control_manager` | both | `database/cluster/session` | `request_action` | `recommend_only` | Monitors session health and enforces session-level policy controls (idle limits, reauth requirements) by requesting engine action. |

---

### Index Health and Maintenance

| Agent | Deployment | Scope | Authority Class | Default Activation | Purpose |
|-------|-----------|-------|-----------------|-------------------|---------|
| `index_health_manager` | local | `database/index` | `recommend_only` | `recommend_only` | Evaluates index health, fragmentation, and freshness across all index types; publishes recommendations for rebuild, refresh, or operator review. |

---

### Policy, Learning, and Optimization

| Agent | Deployment | Scope | Authority Class | Default Activation | Purpose |
|-------|-----------|-------|-----------------|-------------------|---------|
| `policy_recommendation_manager` | both | `database/cluster` | `recommend_only` | `recommend_only` | Evaluates engine state against policy templates and publishes candidate policy recommendations for operator review and approval before any application. |
| `runtime_learning_agent` | local | `database/optimizer` | `recommend_only` | `recommend_only` | Observes query patterns, optimizer decisions, and plan quality to produce optimizer-tuning recommendations. Advisory only. |
| `parser_interface_manager` | local | `node/parser/interface` | `request_action` | `recommend_only` | Monitors the parser interface and can request adjustments to parser registration or routing within the engine's parser authority framework. Cannot modify parser behavior directly. |

---

### Admission and Scheduling

| Agent | Deployment | Scope | Authority Class | Default Activation | Purpose |
|-------|-----------|-------|-----------------|-------------------|---------|
| `admission_control_manager` | both | `database/cluster/workload` | `direct_bounded_action` | `dry_run` | Governs workload admission to protect foreground database work. Can apply direct bounded admission decisions within policy. |
| `metrics_registry_manager` | both | `node/database/cluster` | `direct_bounded_action` | `dry_run` | Maintains the health and freshness of the metrics registry across node, database, and cluster scopes. |
| `job_control_manager` | both | `database/cluster/jobs` | `request_action` | `recommend_only` | Manages lifecycle of background and scheduled jobs. Requests job scheduling and cancellation; does not own job finality. |
| `cluster_scheduler_manager` | cluster | `cluster/jobs` | `request_action` | `disabled` | Coordinates cluster-wide job scheduling. Cluster-only; disabled without cluster authority. |

---

### Backup, Restore, Archive, and PITR

| Agent | Deployment | Scope | Authority Class | Default Activation | Purpose |
|-------|-----------|-------|-----------------|-------------------|---------|
| `backup_manager` | both | `database/cluster/backup` | `request_action` | `recommend_only` | Initiates and monitors backup operations; requests backup execution through the engine's backup subsystem. |
| `archive_manager` | both | `database/cluster/archive` | `direct_bounded_action` | `dry_run` | Manages archive lifecycle including retention, expiry, and transition of backup artifacts within policy. |
| `restore_drill_manager` | both | `database/cluster/restore` | `request_action` | `recommend_only` | Automates restore-readiness verification by running isolated restore drills. Requires target isolation, backup manifest availability, and restore-inspection authorization before proceeding. |
| `pitr_manager` | both | `database/cluster/pitr` | `request_action` | `recommend_only` | Manages point-in-time recovery readiness, retention window validation, and PITR execution requests. |

---

### Identity and Security

| Agent | Deployment | Scope | Authority Class | Default Activation | Purpose |
|-------|-----------|-------|-----------------|-------------------|---------|
| `identity_manager` | both | `database/cluster/security` | `request_action` | `recommend_only` | Monitors identity and security policy state; requests security policy enforcement actions. Does not own authorization decisions. |

---

### Metrics, Alerting, and Observability

| Agent | Deployment | Scope | Authority Class | Default Activation | Purpose |
|-------|-----------|-------|-----------------|-------------------|---------|
| `alert_manager` | both | `node/database/cluster` | `direct_bounded_action` | `dry_run` | Evaluates alert conditions across node, database, and cluster scopes and can take direct bounded alerting actions (for example, recording alert evidence or triggering notification routes) within policy. |
| `distributed_query_metrics_agent` | cluster | `cluster/query` | `observe_only` | `disabled` | Observes distributed query execution metrics across the cluster. Cluster-only; disabled without cluster authority. Takes no action. |

---

### Query Routing

| Agent | Deployment | Scope | Authority Class | Default Activation | Purpose |
|-------|-----------|-------|-----------------|-------------------|---------|
| `remote_query_routing_agent` | cluster | `cluster/query/route` | `request_action` | `disabled` | Requests cluster-level query routing adjustments based on observed routing metrics. Cluster-only; disabled without cluster authority. |

---

### Diagnostics and Support

| Agent | Deployment | Scope | Authority Class | Default Activation | Purpose |
|-------|-----------|-------|-----------------|-------------------|---------|
| `support_bundle_triage_agent` | both | `node/database/cluster/support` | `request_action` | `recommend_only` | Monitors the support bundle surface; can request triage evidence capture and bundle assembly. |
| `export_adapter_manager` | both | `node/database/cluster/export` | `request_action` | `recommend_only` | Manages export adapter lifecycle, routing, and health monitoring. |

---

### Cluster Lifecycle

| Agent | Deployment | Scope | Authority Class | Default Activation | Purpose |
|-------|-----------|-------|-----------------|-------------------|---------|
| `cluster_upgrade_manager` | cluster | `cluster/upgrade` | `request_action` | `disabled` | Coordinates cluster upgrade sequencing and readiness checks. Cluster-only; disabled without cluster authority. |

---

## Summary Statistics

| Deployment class | Count |
|-----------------|-------|
| `local` only | 8 |
| `cluster` only | 5 |
| `both` (local and cluster scope) | 16 |
| **Total** | **29** |

| Default activation | Count |
|-------------------|-------|
| `disabled` | 5 (all cluster-only) |
| `observe_only` | 1 |
| `recommend_only` | 16 |
| `dry_run` | 7 |
| `live_action` | 0 |

No agent ships with a default activation of `live_action`. Reaching live-action requires an explicit operator-approved rollout transition.

---

## Cluster-Only Agents

The following five agents are `cluster` deployment only and default to `disabled`. They are inoperative on standalone (non-cluster) deployments:

- `cluster_autoscale_manager`
- `distributed_query_metrics_agent`
- `remote_query_routing_agent`
- `cluster_scheduler_manager`
- `cluster_upgrade_manager`

Attempting to activate these agents without cluster authority fails closed with `unavailable_cluster_authority`.

---

## See Also

- [authority_and_activation_model.md](authority_and_activation_model.md) — definitions of authority classes, activation profiles, and lifecycle states
- [maintenance_and_tuning_agents.md](maintenance_and_tuning_agents.md) — deep dives on agents with rich verified behavior
- [../Language_Reference/syntax_reference/agent.md](../Language_Reference/syntax_reference/agent.md) — SBsql statements for inspecting and controlling agents
