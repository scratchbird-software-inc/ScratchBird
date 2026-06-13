# Agent Metric Dependency Contracts

<!--
Copyright (c) 2025-2026 Dalton Calford. All rights reserved.

TRADE SECRET / PRIVATE / CONFIDENTIAL.
-->

Status: accepted controlling private specification.
Search key: `AGH_007_AGENT_METRIC_DEPENDENCIES`.

## Dependency rule

Every agent implementation must load this table or a generated registry derived from it. If a required metric is missing, stale, untrusted, or schema-incompatible, the agent must execute the listed `fail_behavior`, write evidence, and must not invent substitute inputs. Cluster metrics live under `cluster.sys.metrics.*` only when the cluster exists.

| agent_name | metric_family | namespace | required_or_optional | minimum_freshness | required_quality | aggregation | policy_field | decision_use | fail_behavior | evidence_field |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| node_resource_agent | sb_cluster_node_cpu_feature_available | sys.metrics.physical | required | <=60s | trusted node probe | latest | required_cpu_features | role suitability | suitability unknown | cpu_feature_snapshot |
| metrics_registry_manager | sb_metric_samples_rejected_total | sys.metrics.registry | required | <=15s | trusted counter | rate 5m | rejection_rate_limit | cardinality/backpressure | reject invalid samples | rejected_sample_count |
| storage_health_manager | sb_storage_fsync_latency_microseconds | sys.metrics.storage | required | <=15s | trusted histogram | p99 over 5m | fsync_p99_critical_us | health and optimizer cost | health unknown | fsync_p99 |
| storage_health_manager | sb_storage_unknown_pages_total | sys.metrics.storage | required | <=5s | trusted counter | latest/rate | unknown_page_quarantine_threshold | quarantine request | deny destructive actions | unknown_page_count |
| memory_governor | sb_memory_emergency_reserve_bytes | sys.metrics.memory | required | <=5s | trusted gauge | latest plus 30s min | emergency_reserve_percent | grant denial/spill | deny large grants | reserve_bytes_snapshot |
| memory_governor | sb_memory_allocation_failures_total | sys.metrics.memory | required | <=5s | trusted counter | rate 1m | allocation_failure_pressure | reserve protection | conservative grants | allocation_failure_rate |
| index_health_manager | sb_index_read_amplification_ratio | sys.metrics.indexes | required | <=300s | trusted gauge | p95/window | rebuild_threshold | rebuild recommendation | suppress recommendation | read_amplification_ratio |
| index_health_manager | sb_index_splits_total | sys.metrics.indexes | optional | <=300s | trusted counter | rate 1h | split_pressure_threshold | rebalance recommendation | no split-based recommendation | split_rate |
| cluster_autoscale_manager | sb_cluster_node_saturation_ratio | cluster.sys.metrics.autoscale | required | <=30s | cluster confirmed | max/window 5m | scale_up_threshold | scale recommendation | scale-down denied | saturation_snapshot |
| cluster_autoscale_manager | sb_idle_capacity_ratio | cluster.sys.metrics.autoscale | required | <=60s | cluster confirmed | min/window 15m | scale_down_threshold | scale-down recommendation | scale-down denied | idle_capacity_ratio |
| admission_control_manager | sb_listener_queue_depth | sys.metrics.listener | required | <=5s | trusted gauge | latest/p95 1m | listener_queue_threshold | throttle/deny | conservative throttle | listener_queue_depth |
| admission_control_manager | sb_scheduler_queue_depth | sys.metrics.scheduler | required | <=5s | trusted gauge | latest/p95 1m | scheduler_queue_threshold | admission throttle | conservative throttle | scheduler_queue_depth |
| parser_interface_manager | sb_parser_crashes_total | sys.metrics.parser | required | <=15s | trusted event stream | rate 5m | parser_crash_quarantine_threshold | drain/quarantine | deny new parser assignment | parser_crash_rate |
| parser_interface_manager | sb_parser_policy_attach_latency_microseconds | sys.metrics.parser | optional | <=60s | trusted histogram | p95 5m | policy_attach_latency_warn_us | recommendation | ignore optional input | attach_latency_p95 |
| transaction_pressure_manager | sb_tx_oldest_snapshot_age_microseconds | sys.metrics.transactions | required | <=5s | trusted gauge | latest | oldest_snapshot_pressure_seconds | warnings/recommendations | warning only | oldest_snapshot_age |
| transaction_pressure_manager | sb_cluster_limbo_transactions | cluster.sys.metrics.transactions | required for cluster scope | <=10s | cluster confirmed | latest | limbo_pressure_threshold | deny cleanup/route recommendations | cluster recommendations disabled | limbo_count |
| cleanup_archive_manager | sb_mga_cleanup_blocked_total | sys.metrics.mga.cleanup | required | <=10s | trusted counter | rate 5m | cleanup_blocked_threshold | cleanup pressure | cleanup denied | cleanup_blocked_rate |
| cleanup_archive_manager | sb_archive_slice_age_microseconds | sys.metrics.archive | required | <=60s | trusted gauge | max | archive_slice_max_age_us | archive verification/seal | hold slice | max_slice_age |
| policy_recommendation_manager | sb_workload_slo_burn_rate | sys.metrics.workloads | required | <=60s | trusted gauge | max/window | recommendation_burn_rate | policy recommendation | no recommendation | slo_burn_rate |
| distributed_query_metrics_agent | sb_query_fragment_queue_delay_microseconds | cluster.sys.metrics.query.fragments | required | <=5s | trusted histogram | p95/p99 | fragment_delay_warn_us | publish fragment health | fragment unknown | queue_delay_p95 |
| remote_query_routing_agent | sb_optimizer_remote_fragment_latency_microseconds | cluster.sys.metrics.optimizer.remote | required | <=10s | trusted histogram | p95/window | remote_route_latency_weight | route recommendation | local fallback | remote_latency_p95 |
| runtime_learning_agent | sb_optimizer_plan_estimate_error_ratio | sys.metrics.optimizer | required | <=300s | trusted gauge | p95/window | correction_threshold | planner correction | disable correction | estimate_error_ratio |
| support_bundle_triage_agent | sb_support_bundle_completeness_ratio | sys.metrics.supportability | required | <=60s | trusted gauge | latest | completeness_required | bundle readiness | deny bundle | completeness_ratio |
| cluster_scheduler_manager | sb_cluster_scheduler_queue_depth | cluster.sys.metrics.scheduler | required | <=10s | cluster confirmed | latest/p95 | scheduler_queue_threshold | job placement | keep queued | queue_depth |
| job_control_manager | sb_job_control_actions_total | sys.metrics.jobs | required | <=15s | trusted counter | event/rate | job_control_policy | control audit/suppression | deny control on audit gap | job_action_counter |
| backup_manager | sb_backup_progress_percent | sys.metrics.backup | required | <=30s | trusted gauge | latest | stuck_progress_window | progress/stuck detection | progress unknown | backup_percent |
| archive_manager | sb_archive_queue_depth | sys.metrics.archive | required | <=30s | trusted gauge | latest/p95 | archive_queue_pressure | seal/verify/shed | hold archive | archive_queue_depth |
| restore_drill_manager | sb_restore_drill_duration_microseconds | sys.metrics.restore | required | <=300s | trusted histogram | p95/window | restore_drill_max_duration_us | readiness | deny drill | drill_duration_p95 |
| pitr_manager | sb_pitr_window_available_seconds | sys.metrics.pitr | required | <=60s | trusted gauge | latest | minimum_pitr_window_seconds | PITR readiness | report unreachable | pitr_window_seconds |
| identity_manager | sb_identity_auth_attempts_total | sys.metrics.identity | required | <=15s | trusted event stream | rate 5m | auth_anomaly_policy | lock/review recommendation | security unknown | auth_attempt_rate |
| session_control_manager | sb_identity_sessions_active | sys.metrics.identity | required | <=10s | trusted gauge | latest | session_pressure_policy | disconnect/reauth control | deny session action | active_sessions |
| alert_manager | sb_alerts_fired_total | sys.metrics.alerts | required | <=60s | trusted event stream | dedupe window | alert_policy | dedupe/group/silence | local event only | alert_counter |
| export_adapter_manager | sb_export_adapter_queue_depth | sys.metrics.export | required | <=30s | trusted gauge | latest/p95 | export_queue_backpressure | pause/shed export | deny export | export_queue_depth |
| cluster_upgrade_manager | sb_cluster_rolling_upgrade_readiness_state | cluster.sys.metrics.version_drift | required | <=300s | cluster confirmed | latest | upgrade_readiness_required | allow/deny upgrade step | upgrade denied | readiness_state |

## Metrics-side reverse consumer requirement

Search key: `AGH_007_AGENT_METRIC_REVERSE_CONSUMER_REQUIREMENT`.

The metrics-side reverse matrix is `chapters/metrics/appendix-agent-metric-dependency-and-consumer-matrix.md`. Every row in this agent dependency table must have one matching reverse row by `metric_family` and `namespace`, and that reverse row must list the agent in `consuming_agents`.

An implementation must fail the agent/metrics release gate if:

1. An agent dependency row is absent from the metrics-side matrix.
2. A metrics-side reverse row lists an agent that does not have a matching dependency row here.
3. A required metric is listed as optional in either direction.
4. A namespace differs between the agent-side and metrics-side rows.
5. `policy_field`, `decision_use`, `fail_behavior`, or `evidence_field` differs between the two directions.

Metrics are still observation inputs only. A bidirectional dependency row does not grant authority to perform an action; action authority remains controlled by policy, grants, evidence, and the relevant subsystem authority.

## Metrics drift-closure producer/consumer rule

Agent specifications must distinguish metric producers from metric consumers.

Rules:

- Agents that only read metrics are consumers and are not required to exist before the metric family is registered.
- Agent-produced families live under `sys.metrics.agents` and use producer owner `agent_runtime`.
- Agent-produced families are `contract-ready-unwired` until the agent runtime exists.
- Agent dependencies must use canonical family names from `../metrics/appendix-concrete-metric-registry.md`.
- Deprecated namespace spellings are not allowed in new agent contracts.

## Filespace/page agent metric production obligations

Search key: `AGH_FILESPACE_PAGE_AGENT_METRIC_PRODUCTION_OBLIGATIONS`.

The filespace/page agents consume storage fact metrics and produce separate agent-operation metrics. These rows are production obligations, not input dependencies.

| agent_name | produced_metric_family | namespace | production_trigger | required_labels | authority_boundary |
| --- | --- | --- | --- | --- | --- |
| filespace_capacity_manager | sb_agent_filespace_capacity_requests_total | sys.metrics.agents.filespace | every capacity contract request result | database_uuid,filespace_uuid,node_uuid,request_class,result,policy_uuid | filespace agent only |
| filespace_capacity_manager | sb_agent_filespace_capacity_denied_total | sys.metrics.agents.filespace | every denied capacity or lifecycle request | database_uuid,filespace_uuid,node_uuid,request_class,result,policy_uuid,denial_class | filespace agent only |
| filespace_capacity_manager | sb_agent_filespace_extend_attempts_total | sys.metrics.agents.filespace | every physical extension attempt | database_uuid,filespace_uuid,node_uuid,request_class,result,policy_uuid,device_class | filespace agent only |
| filespace_capacity_manager | sb_agent_filespace_extend_failures_total | sys.metrics.agents.filespace | every physical extension failure | database_uuid,filespace_uuid,node_uuid,request_class,result,policy_uuid,failure_class | filespace agent only |
| filespace_capacity_manager | sb_agent_filespace_truncate_attempts_total | sys.metrics.agents.filespace | every physical truncate attempt | database_uuid,filespace_uuid,node_uuid,request_class,result,policy_uuid,approval_class | filespace agent only |
| filespace_capacity_manager | sb_agent_filespace_truncate_failures_total | sys.metrics.agents.filespace | every physical truncate failure | database_uuid,filespace_uuid,node_uuid,request_class,result,policy_uuid,failure_class | filespace agent only |
| filespace_capacity_manager | sb_agent_filespace_capacity_window_open | sys.metrics.agents.filespace | capacity window open/close or refresh | database_uuid,filespace_uuid,node_uuid,window_class,policy_uuid | filespace agent only |
| filespace_capacity_manager | sb_agent_filespace_physical_hold_blockers | sys.metrics.agents.filespace | physical hold scan or lifecycle gate check | database_uuid,filespace_uuid,node_uuid,hold_class,owner_subsystem | filespace agent only |
| page_allocation_manager | sb_agent_page_capacity_requests_total | sys.metrics.agents.page_allocation | every capacity request sent to filespace agent | database_uuid,filespace_uuid,node_uuid,page_family,page_type,request_class,result,policy_uuid | page allocation agent only |
| page_allocation_manager | sb_agent_page_preallocation_requests_total | sys.metrics.agents.page_allocation | every preallocation request | database_uuid,filespace_uuid,node_uuid,page_family,page_type,request_class,result,policy_uuid,reason_class | page allocation agent only |
| page_allocation_manager | sb_agent_page_preallocation_denied_total | sys.metrics.agents.page_allocation | every preallocation denial | database_uuid,filespace_uuid,node_uuid,page_family,page_type,request_class,result,policy_uuid,denial_class | page allocation agent only |
| page_allocation_manager | sb_agent_page_shrink_ready_publications_total | sys.metrics.agents.page_allocation | every shrink-ready or shrink-blocked publication | database_uuid,filespace_uuid,node_uuid,range_class,result,policy_uuid | page allocation agent only |
| page_allocation_manager | sb_agent_page_shrink_blocker_count | sys.metrics.agents.page_allocation | shrink scan or blocker refresh | database_uuid,filespace_uuid,node_uuid,blocker_class,owner_subsystem | page allocation agent only |
| page_allocation_manager | sb_agent_page_relocation_required_total | sys.metrics.agents.page_allocation | relocation requirement detected | database_uuid,filespace_uuid,node_uuid,page_family,page_type,reason_class | page allocation agent only |
| page_allocation_manager | sb_agent_page_relocation_complete_total | sys.metrics.agents.page_allocation | relocation completion or failure | database_uuid,filespace_uuid,node_uuid,page_family,page_type,reason_class,result | page allocation agent only |

The filespace/disk capacity agent must not produce `sys.metrics.agents.page_allocation.*`. The page-allocation manager must not produce `sys.metrics.agents.filespace.*`. Cluster projections under `cluster.sys.metrics.agents.*` are aggregation surfaces and do not change local write ownership.

## Filespace/page agent metric dependency addendum

Search key: `AGH_FILESPACE_PAGE_AGENT_METRIC_DEPENDENCIES`.

These rows are mandatory and must be mirrored in `chapters/metrics/appendix-agent-metric-dependency-and-consumer-matrix.md`.

| agent_name | metric_family | namespace | required_or_optional | minimum_freshness | required_quality | aggregation | policy_field | decision_use | fail_behavior | evidence_field |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| filespace_capacity_manager | sb_filespace_total_bytes | sys.metrics.storage.filespaces | required | <=15s | trusted filespace sample | latest | minimum_free_percent | capacity denominator | capacity action denied | filespace_capacity_snapshot |
| filespace_capacity_manager | sb_filespace_used_bytes | sys.metrics.storage.filespaces | required | <=15s | trusted filespace sample | latest | minimum_free_percent | capacity use | capacity action denied | filespace_capacity_snapshot |
| filespace_capacity_manager | sb_filespace_free_bytes | sys.metrics.storage.filespaces | required | <=15s | trusted filespace sample | latest | minimum_free_bytes | expansion/shrink decision | capacity action denied | filespace_capacity_snapshot |
| filespace_capacity_manager | sb_filespace_reserved_bytes | sys.metrics.storage.filespaces | required | <=15s | trusted filespace sample | latest | reservation_pressure | reservation pressure | capacity action denied | filespace_reservation_snapshot |
| filespace_capacity_manager | sb_filespace_health_state | sys.metrics.storage.filespaces | required | <=15s | trusted state | latest | device_health_thresholds | lifecycle safety | action denied | filespace_health_snapshot |
| filespace_capacity_manager | sb_filespace_role_state | sys.metrics.storage.filespaces | required | <=15s | trusted state | latest | role_transition_policy | role authority | action denied | filespace_role_snapshot |
| filespace_capacity_manager | sb_filespace_truncate_ready_bytes | sys.metrics.storage.filespaces | required for shrink | <=60s | page-agent proof | latest | truncate_allowed | truncate proof | truncate denied | filespace_shrink_proof |
| filespace_capacity_manager | sb_filespace_shrink_blocker_count | sys.metrics.storage.filespaces | required for shrink | <=60s | trusted blocker sample | latest | shrink_allowed | blocker proof | shrink denied | filespace_shrink_blockers |
| page_allocation_manager | sb_page_free_count | sys.metrics.storage.pages | required | <=15s | trusted page sample | latest | preallocation_allowed | allocation decision | preallocation denied | page_allocation_snapshot |
| page_allocation_manager | sb_page_allocated_count | sys.metrics.storage.pages | required | <=15s | trusted page sample | latest | relocation_allowed | fragmentation/capacity | relocation denied | page_allocation_snapshot |
| page_allocation_manager | sb_page_reserved_count | sys.metrics.storage.pages | required | <=15s | trusted page sample | latest | max_reserved_bytes_per_filespace | reservation pressure | preallocation denied | page_reservation_snapshot |
| page_allocation_manager | sb_page_allocation_latency_microseconds | sys.metrics.storage.pages | required | <=15s | trusted histogram | p95 5m | allocation_latency_warn_us | allocator health | health unknown | page_allocation_latency |
| page_allocation_manager | sb_page_allocation_failures_total | sys.metrics.storage.pages | required | <=15s | trusted counter | rate 5m | allocation_failure_pressure | allocator pressure | preallocation denied | page_allocation_failures |
| page_allocation_manager | sb_page_fragmentation_ratio | sys.metrics.storage.pages | optional | <=300s | trusted scanner | latest/window | fragmentation_threshold | relocation recommendation | recommendation suppressed | page_fragmentation_snapshot |
| page_allocation_manager | sb_page_relocation_backlog_count | sys.metrics.storage.pages | required for relocation | <=60s | trusted relocation queue | latest | relocation_allowed | relocation backlog | relocation denied | page_relocation_backlog |
| page_allocation_manager | sb_page_relocation_ready_for_filespace_shrink | sys.metrics.storage.pages | required for shrink | <=60s | page-agent proof | latest | publish_shrink_ready_allowed | shrink readiness | shrink readiness denied | page_shrink_readiness |
| storage_health_manager | sb_filespace_health_state | sys.metrics.storage.filespaces | required | <=15s | trusted state | latest | storage_health_policy | health summary | health unknown | storage_health_snapshot |
| storage_health_manager | sb_filespace_device_error_total | sys.metrics.storage.filespaces | required | <=5s | trusted counter | rate/latest | device_error_threshold | health escalation | destructive actions denied | device_error_snapshot |
| storage_health_manager | sb_page_allocation_failures_total | sys.metrics.storage.pages | optional | <=15s | trusted counter | rate 5m | storage_health_policy | allocator-health summary | page health unknown | page_allocation_failures |
