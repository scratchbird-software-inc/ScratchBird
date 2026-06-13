# Agent SHOW and sys Surfaces

<!--
Copyright (c) 2025-2026 Dalton Calford. All rights reserved.

TRADE SECRET / PRIVATE / CONFIDENTIAL.
-->

Status: accepted controlling private specification.
Search key: `AGH_005_AGENT_SHOW_SYS_SURFACES`.

## Surface rule

`SHOW` commands and `sys.*` views are projections over the same engine-owned agent registry, lifecycle, policy, action, evidence, and metric-dependency records. Compatibility aliases must inherit from these surfaces and must not define independent permissions.

## Local surfaces

| surface | required_right | columns | filters | evidence | redaction |
| --- | --- | --- | --- | --- | --- |
| sys.agents | OBS_AGENT_STATE_READ | agent_uuid, agent_type_id, scope_kind, scope_uuid, component, state, health_state, enabled, policy_uuid, last_transition_at, last_diagnostic_code | agent_type_id, scope_kind, state, health_state, component | none | scope_uuid redacted if caller lacks scope visibility |
| sys.agent_metric_dependencies | OBS_AGENT_STATE_READ plus metric read for raw values | agent_uuid, metric_family, namespace, required_or_optional, freshness_limit, current_freshness, quality_state, fail_behavior | agent_uuid, metric_family, quality_state | none | current values hidden without metric rights |
| sys.agent_policies | OBS_POLICY_READ | agent_uuid, policy_uuid, policy_family, version_uuid, active_state, validation_state, attached_at, attached_by | agent_uuid, policy_family, validation_state | none | policy body not included |
| sys.agent_actions | OBS_AGENT_RECOMMENDATION_READ | action_uuid, agent_uuid, action_id, state, risk_class, created_at, expires_at, approval_required, actor_uuid nullable, diagnostic_code | action_id, state, risk_class, agent_uuid | none | actor redacted without audit right |
| sys.agent_overrides | OBS_AGENT_STATE_READ | override_uuid, target_uuid, scope_uuid, suppression_class, starts_at, expires_at, state, reason_code, created_by | state, target_uuid, scope_uuid | none | reason text omitted |
| sys.agent_evidence | OBS_AGENT_EVIDENCE_READ | evidence_uuid, agent_uuid, evidence_type, action_uuid nullable, redaction_class, created_at, actor_uuid, payload_digest, payload_redacted | agent_uuid, evidence_type, action_uuid | agent_read_evidence on direct SHOW use | payload fields redacted by redaction class |
| sys.agent_audit | OBS_AGENT_EVIDENCE_READ | audit_uuid, evidence_uuid, actor_uuid, command_name, sblr_operation, api_call, result_state, diagnostic_code, created_at | actor_uuid, command_name, result_state | agent_read_evidence on direct SHOW use | actor details redacted when required |

## Cluster surfaces

| surface | required_right | columns | cluster-specific rule |
| --- | --- | --- | --- |
| cluster.sys.agents | OBS_CLUSTER_HEALTH_INSPECT and OBS_AGENT_STATE_READ | cluster_uuid, agent_uuid, agent_type_id, scope_kind, scope_uuid, leader_node_uuid, lease_epoch, fence_token, state, health_state | absent when cluster does not exist |
| cluster.sys.agent_actions | OBS_AGENT_RECOMMENDATION_READ | cluster_uuid, action_uuid, agent_uuid, action_id, state, risk_class, leader_node_uuid, fence_token | mutating actions require current lease/fence |
| cluster.sys.agent_evidence | OBS_AGENT_EVIDENCE_READ | cluster_uuid, evidence_uuid, agent_uuid, evidence_type, leader_epoch, payload_digest, created_at | local projections marked `projection_state` |
| cluster.sys.agent_metric_dependencies | OBS_CLUSTER_HEALTH_INSPECT | cluster_uuid, agent_uuid, metric_family, namespace, current_freshness, quality_state, projection_state | local projected metrics cannot authorize destructive action |

## SHOW command projection

`SHOW AGENTS` reads from `sys.agents`. `SHOW CLUSTER AGENTS` reads from `cluster.sys.agents`. `SHOW AGENT <ref> EVIDENCE` reads from `sys.agent_evidence` or `cluster.sys.agent_evidence` after permission and redaction checks.

## Filespace/page agent surfaces

Search key: `AGH_FILESPACE_PAGE_SHOW_SYS_SURFACES`.

| surface | required_right | columns | filters | evidence | redaction |
| --- | --- | --- | --- | --- | --- |
| sys.filespace_capacity_agent_state | OBS_AGENT_STATE_READ | agent_uuid, filespace_uuid, policy_uuid, mode, last_capacity_metric_at, last_health_metric_at, last_recommendation_code, last_refusal_code | filespace_uuid, health_state, mode | none | physical paths hidden without storage inspect right |
| sys.page_allocation_agent_state | OBS_AGENT_STATE_READ | agent_uuid, filespace_uuid, page_family, page_type, policy_uuid, mode, last_scan_generation, last_shrink_ready_state, last_refusal_code | filespace_uuid, page_family, page_type, mode | none | blocker principal/object details redacted |
| sys.filespace_shrink_readiness | OBS_METRICS_READ_FAMILY | filespace_uuid, safe_start_byte, safe_end_byte, truncate_ready_bytes, blocker_count, readiness_state, scan_generation, evidence_uuid | filespace_uuid, readiness_state | page_shrink_ready_evidence on direct detail access | blocker payload redacted by evidence class |

`SHOW FILESPACE` and `SHOW PAGE` commands are projections over these surfaces plus metric registry rows. Compatibility aliases inherit the same permission and redaction rules.
