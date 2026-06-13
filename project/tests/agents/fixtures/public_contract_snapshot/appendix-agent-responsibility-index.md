# Agent Responsibility Index

<!--
Copyright (c) 2025-2026 Dalton Calford. All rights reserved.

TRADE SECRET / PRIVATE / CONFIDENTIAL.

This file contains private ScratchBird specification material.
No license or rights are granted to any person or entity except by specific
written permission from the author/owner.

Unauthorized use, copying, modification, distribution, disclosure, publication,
or creation of derivative works is prohibited.
-->

Status: accepted controlling private specification.
Search key: `AGH_AGENT_RESPONSIBILITY_INDEX`.

## Purpose

This document is the human-readable index of ScratchBird operational agents and their responsibilities. It does not replace `appendix-agent-canonical-registry.md`; the canonical registry remains the machine-authoritative registry for scope, authority class, metrics, policies, actions, and fail-closed behavior.

This index exists so implementers can determine, without guessing, which agent owns a responsibility and which agent is forbidden from performing adjacent work.

## Responsibility rules

1. Every agent has one owning responsibility domain.
2. Agents may consume metrics outside their domain, but consuming a metric does not grant action authority.
3. Agents may request work from another agent or subsystem only through a specified action contract.
4. If two agents appear to overlap, the `owns` and `does_not_own` columns below decide ownership.
5. If ownership is unclear, the implementation MUST fail closed and emit `AGENT.RESPONSIBILITY_AMBIGUOUS` evidence.
6. Cluster-only agents are absent in standalone deployments and MUST fail closed if invoked without cluster authority.
7. Compatibility surfaces, parsers, dashboards, and third-party tools never become agent authority.

## Agent responsibility matrix

| Agent | Responsibility summary | Owns | Does not own | Primary action style | Detailed authority source |
| --- | --- | --- | --- | --- | --- |
| `node_resource_agent` | Observes physical node capability and suitability. | CPU feature availability, page-size capability, role suitability observations, node capability publication. | Query routing, shard placement, filespace movement, page relocation. | Observe/publish. | `appendix-agent-canonical-registry.md`; metrics `appendix-node-resource-agent.md`. |
| `metrics_registry_manager` | Owns metric registry integrity and metric sample acceptance. | Metric descriptor validation, schema/cardinality enforcement, rollups, rejected-sample evidence. | Subsystem metric production semantics, agent business decisions, transaction/catalog authority. | Direct bounded registry action. | `appendix-agent-canonical-registry.md`; `../metrics/appendix-metrics-registry-manager.md`. |
| `storage_health_manager` | Summarizes storage health and routes recommendations to owning agents. | Storage health summary, device/page/filespace health recommendations, advisory optimizer storage-cost update. | Filespace expand/move/shrink/truncate/delete/promotion; page allocation/relocation; index rebuild. | Recommendation/request only. | `appendix-storage-health-manager.md`; metrics `../metrics/appendix-storage-health-manager.md`. |
| `filespace_capacity_manager` | Owns filespace capacity and lifecycle pressure decisions. | Filespace capacity, growth, depletion ETA, physical storage pressure, lifecycle action requests, shrink/truncate readiness consumption, primary shadow/candidate promotion recommendations. | Individual page allocation, page relocation, page compaction, index rebuild, MGA cleanup low-water marks. | Request action; live mutation only by explicit policy/approval. | `appendix-filespace-capacity-manager.md`; `appendix-agent-canonical-registry.md`. |
| `page_allocation_manager` | Owns page-family and page-type allocation health. | Page counts, page reservations, page preallocation, allocation latency/failures, fragmentation, relocation backlog, relocatable/unmovable classification, shrink-readiness proof. | Filespace expansion/truncation/deletion/promotion, physical device management, index rebuild ownership. | Request action; bounded preallocation only by policy. | `appendix-page-allocation-manager.md`; `appendix-agent-canonical-registry.md`. |
| `memory_governor` | Protects memory availability and enforces memory allocation policy. | Memory pressure detection, grant denial/shrink, spill forcing, cache shrink recommendations/actions. | Session termination, query semantics, transaction rollback, OS memory management outside ScratchBird authority. | Direct bounded action. | `appendix-agent-canonical-registry.md`; metrics `../metrics/appendix-memory-governor.md`. |
| `index_health_manager` | Evaluates index health and recommends index maintenance. | Index read amplification, split pressure, unused-index recommendations, index rebuild/drop recommendations, storage assistance requests. | Direct filespace/page mutation, direct index drop/rebuild without approval/job authority. | Recommendation/request. | `appendix-agent-canonical-registry.md`; metrics `../metrics/appendix-index-health-manager.md`. |
| `cluster_autoscale_manager` | Recommends cluster capacity scale up/down. | Cluster saturation, idle-capacity evaluation, autoscale recommendations. | Direct cloud/provider machine creation unless a management actuator and policy authorize it; scale-down on stale metrics/limbo. | Request/recommend. | `appendix-agent-canonical-registry.md`; metrics `../metrics/appendix-cluster-autoscale-manager.md`. |
| `admission_control_manager` | Controls admission under resource or workload pressure. | Admission throttle, denial, degraded admission class, pressure-based admission decisions. | Security authentication/authorization, committed transaction rollback, parser package quarantine. | Direct bounded action. | `appendix-agent-canonical-registry.md`; metrics `../metrics/appendix-admission-control-manager.md`. |
| `parser_interface_manager` | Supervises parser/interface health and assignment. | Parser crash/failure response, parser family drain, parser package quarantine request, parser assignment denial. | Engine security authority, parser dialect semantics, engine state mutation. | Request action. | `appendix-agent-canonical-registry.md`; metrics `../metrics/appendix-parser-interface-manager.md`. |
| `transaction_pressure_manager` | Detects long-running/idle transaction pressure. | Long transaction warning, reauth request, cancel recommendation, cleanup-pressure escalation. | Final transaction outcome, committed rollback, cleanup low-water mark advancement. | Recommendation/request. | `appendix-agent-canonical-registry.md`; metrics `../metrics/appendix-transaction-pressure-manager.md`. |
| `storage_version_cleanup_agent` | Executes bounded row-version cleanup under authoritative MGA horizon evidence. | Cleanup batch admission, retained-version pressure handling, blocked-cleanup evidence. | Transaction finality, cleanup low-water mark authority, filespace capacity decisions. | Direct bounded dry-run by default. | `appendix-agent-canonical-registry.md`; metrics `../metrics/appendix-storage-version-cleanup-agent.md`. |
| `cleanup_archive_manager` | Owns cleanup/archive pressure decisions under MGA safety rules. | Cleanup low-water-mark advancement when authoritative, cleanup blocker response, archive verification request, retention pressure handling. | Cleanup based on projected cluster state, filespace truncation, transaction finality, archive deletion without retention authority. | Direct bounded action with authoritative proof. | `appendix-agent-canonical-registry.md`; metrics `../metrics/appendix-cleanup-archive-manager.md`. |
| `policy_recommendation_manager` | Recommends policy changes from operational evidence. | Draft policy recommendations, confidence scoring, policy simulation recommendations. | Applying policy, bypassing approval, changing security grants. | Recommendation only. | `appendix-agent-canonical-registry.md`; metrics `../metrics/appendix-policy-recommendation-manager.md`. |
| `distributed_query_metrics_agent` | Publishes cluster query-fragment metrics. | Fragment queue delay, route-generation observations, distributed query metric publication. | Query routing, admission, cancellation, cluster route publication. | Observe/publish. | `appendix-agent-canonical-registry.md`; metrics `../metrics/appendix-distributed-query-metrics-agent.md`. |
| `remote_query_routing_agent` | Recommends remote query routing weights. | Remote-fragment latency evaluation, route-weight recommendations, local fallback recommendation. | Publishing cluster routes without fence authority, executing query fragments, moving shards. | Recommendation/request. | `appendix-agent-canonical-registry.md`; metrics `../metrics/appendix-remote-query-routing-agent.md`. |
| `runtime_learning_agent` | Recommends optimizer/runtime corrections from observed plan behavior. | Plan-estimate error evaluation, planner correction recommendations, runtime learning evidence. | Changing logical semantics, forcing unsafe plans, changing storage/page/filespace authority. | Recommendation only. | `appendix-agent-canonical-registry.md`; metrics `../metrics/appendix-runtime-learning-agent.md`. |
| `support_bundle_triage_agent` | Prepares support-bundle recommendations and redacted bundle drafts. | Support evidence completeness checks, redaction readiness, support bundle recommendation/preparation. | Secret disclosure, policy bypass, source database mutation. | Request/recommend. | `appendix-agent-canonical-registry.md`; metrics `../metrics/appendix-support-bundle-triage-agent.md`. |
| `cluster_scheduler_manager` | Places and routes cluster jobs under cluster fences. | Cluster job placement recommendation, route_cluster_job under valid fence, queued-job decisions. | Routing without epoch/fence, changing cluster membership, bypassing job rights. | Request action. | `appendix-agent-canonical-registry.md`; metrics `../metrics/appendix-cluster-management-and-cluster-agent-metrics.md`. |
| `job_control_manager` | Owns job control recommendations and approved job actions. | Cancel/retry/suppress job actions, job-control audit, job-owner/admin checks. | Bypassing job permissions, scheduler placement, arbitrary session termination. | Operator/request action. | `appendix-agent-canonical-registry.md`; metrics `../metrics/appendix-scheduler-job-execution-and-job-control-metrics.md`. |
| `backup_manager` | Owns backup operation control and verification decisions. | Start/cancel/verify backup requests, backup progress/stuck detection, backup blocker state. | Archive slice deletion, restore drill source mutation, transaction finality. | Request action. | `appendix-agent-canonical-registry.md`; metrics `../metrics/appendix-backup-archive-slice-restore-drill-and-retention-operation-metrics.md`. |
| `archive_manager` | Owns archive queue/slice operational actions. | Archive slice sealing, slice verification request, archive queue pressure handling. | Deleting protected slices, cleanup low-water mark authority, PITR restore planning. | Direct bounded action. | `appendix-agent-canonical-registry.md`; metrics `../metrics/appendix-replication-archive-and-pitr-service-metrics.md`. |
| `restore_drill_manager` | Runs isolated restore drills. | Restore drill scheduling/request, isolated target validation, drill evidence. | Mutating source database, PITR plan authority, backup validity claims without verification. | Operator/request action. | `appendix-agent-canonical-registry.md`; metrics `../metrics/appendix-backup-archive-slice-restore-drill-and-retention-operation-metrics.md`. |
| `pitr_manager` | Owns point-in-time restore readiness and planning recommendations. | PITR window estimation, restore target reachability, restore plan request. | Starting restore without approval, archive deletion, backup verification. | Request/recommend. | `appendix-agent-canonical-registry.md`; metrics `../metrics/appendix-replication-archive-and-pitr-service-metrics.md`. |
| `identity_manager` | Owns identity lifecycle recommendations/actions under security authority. | User lock, reauth requirement, identity lifecycle evidence, auth anomaly response. | Granting rights without security authority, session disconnect without session-control right, parser authentication policy attachment. | Operator/request action. | `appendix-agent-canonical-registry.md`; metrics `../metrics/appendix-security-auth-policy-metrics.md`. |
| `session_control_manager` | Owns approved session control actions. | Force disconnect, require reauth, revoke session, session pressure handling. | Identity lifecycle mutation outside security authority, transaction rollback, parser package quarantine. | Operator/request action. | `appendix-agent-canonical-registry.md`; metrics `../metrics/appendix-user-presence-identity-lifecycle-and-session-management-metrics.md`. |
| `alert_manager` | Owns alert event creation, grouping, silencing, and clearing. | Fire/clear/silence alert actions, dedupe/grouping, alert state evidence. | Suppressing critical alerts without override policy, changing root health facts, executing remediation. | Direct bounded notification action. | `appendix-agent-canonical-registry.md`; metrics `../metrics/appendix-alert-notification-silence-maintenance.md`. |
| `export_adapter_manager` | Owns third-party telemetry/export adapter control. | Enable/disable/shed export adapter requests, queue backpressure response, adapter health evidence. | Bypassing data residency/redaction, changing metric authority, changing third-party system state outside adapter contract. | Request action. | `appendix-agent-canonical-registry.md`; metrics `../metrics/appendix-third-party-adapter-protocol-contracts.md`. |
| `cluster_upgrade_manager` | Owns cluster rolling-upgrade step approval/blocking. | Upgrade readiness, step approval/blocking, version drift response, fence/readiness validation. | Cluster membership changes without decision service, bypassing route/epoch fences, standalone operation. | Operator/request action. | `appendix-agent-canonical-registry.md`; metrics `../metrics/appendix-cluster-management-and-cluster-agent-metrics.md`. |

## Cross-agent responsibility boundaries

| Boundary | Owning agent | Requesting agents | Rule |
| --- | --- | --- | --- |
| Filespace capacity and lifecycle | `filespace_capacity_manager` | storage_health_manager, page_allocation_manager, index_health_manager, node_resource_agent | Requesters may recommend/request; only filespace capacity authority can ask lifecycle subsystem to act. |
| Page allocation and relocation | `page_allocation_manager` | filespace_capacity_manager, storage_health_manager, index_health_manager | Requesters may ask for page preparation; only page allocation authority can publish shrink-readiness proof. |
| Storage health summary | `storage_health_manager` | all storage-related agents | Summary never grants mutation authority. |
| Index rebuild/drop recommendation | `index_health_manager` | storage_health_manager, runtime_learning_agent | Index maintenance remains recommendation/job-controlled until approved. |
| Cleanup low-water mark | `cleanup_archive_manager` | transaction_pressure_manager, storage_health_manager, filespace_capacity_manager | Cleanup requires authoritative MGA low-water mark, never free-space pressure alone. |
| Row-version cleanup execution | `storage_version_cleanup_agent` | cleanup_archive_manager, transaction_pressure_manager, storage_health_manager | Cleanup execution requires authoritative MGA horizon evidence and bounded batch policy. |
| Session termination | `session_control_manager` | transaction_pressure_manager, identity_manager, admission_control_manager | Requesting agent must provide policy/evidence; session-control rights still apply. |
| Identity lifecycle | `identity_manager` | session_control_manager, alert_manager, security metrics | Security authority and redaction rules are mandatory. |
| Cluster route publication | cluster decision/route authority, not an operational agent | remote_query_routing_agent, cluster_scheduler_manager | Agents may recommend; route publication requires cluster fence/decision authority. |

## Implementation conformance checks

An implementation MUST provide release gates for:

1. Every canonical registry row appears in this responsibility index.
2. Every responsibility-index agent resolves in `appendix-agent-canonical-registry.md`.
3. No agent implementation exposes a forbidden action listed here.
4. Every cross-agent request uses a declared action contract.
5. Every action writes evidence before success.
6. Cluster-only agents fail closed in standalone mode.
7. Agent management surfaces show the owning agent for each requested responsibility.
