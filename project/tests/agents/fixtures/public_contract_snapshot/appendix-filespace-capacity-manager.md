# Filespace Capacity Manager

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
Search key: `AGH_FILESPACE_CAPACITY_MANAGER`.

## Purpose

`filespace_capacity_manager` is the owning operational agent for physical filespace capacity, device health as it affects filespaces, filespace lifecycle pressure, growth, movement, shrink, truncation readiness, shadow/candidate promotion recommendations, and policy-controlled filespace expansion.

A filespace is not a tablespace. A filespace is a ScratchBird physical storage authority scope that can store any supported SQL object family and can carry roles such as active primary, primary shadow, primary candidate, secondary data, secondary index, secondary overflow, secondary history, secondary shard, archive history, archive log, temporary, import candidate, drop pending, or forbidden.

## Authority boundary

| Rule | Requirement |
| --- | --- |
| Agent id | `filespace_capacity_manager`. |
| Scope | `node/database/filespace`. |
| Runtime host | Engine or manager process. |
| Default install state | Installed for every database. |
| Default runtime state | `recommend_only`. |
| Authority class | `request_action`; live lifecycle mutation requires explicit policy and approval class. |
| Policy family | `filespace_capacity_policy`; optional specialized `filespace_shadow_promotion_policy`. |
| Required permission for live lifecycle action | `OBS_AGENT_CONTROL` plus filespace lifecycle permission and action approval when the action is destructive or moves authority. |
| Agent-owned metric namespace | `sys.metrics.agents.filespace.*`; cluster projections use `cluster.sys.metrics.agents.filespace.*` only when cluster authority exists. |
| Evidence rule | Evidence must be written before reporting action success. |
| Fail-closed rule | If policy, metric freshness, page shrink proof, startup safety, recovery state, or authority is missing, the agent emits recommendation/refusal evidence and performs no mutation. |

## Responsibilities

The agent MUST:

1. Track filespace total, used, free, reserved, expandable, growth-rate, and depletion-ETA metrics.
2. Track filespace device latency, fsync latency, device errors, role state, and health state.
3. Recommend or request filespace expansion when capacity policy thresholds are crossed.
4. Recommend or request filespace movement when device health or role policy requires migration.
5. Request shrink preparation from `page_allocation_manager` before any shrink or truncate action.
6. Consume page-agent shrink-readiness proof before requesting filespace truncation or deletion.
7. Refuse shrink/truncate/delete when page relocation proof is stale, incomplete, or reports blockers.
8. Coordinate primary shadow and primary candidate promotion recommendations without directly moving pages.
9. Emit deterministic diagnostics for every refusal.
10. Record action evidence for requests, denials, recommendations, and policy simulations.
11. Maintain agent-owned operational metrics under `sys.metrics.agents.filespace.*`; storage facts under `sys.metrics.storage.filespaces` remain inputs and do not become agent action authority by themselves.
12. Provide physical capacity evidence to `page_allocation_manager` through the cross-agent contract before page preallocation consumes newly extended capacity.

## Forbidden actions

The agent MUST NOT:

1. Allocate a page.
2. Relocate a page.
3. Compact a page family.
4. Rebuild an index.
5. Rewrite object contents.
6. Advance MGA cleanup low-water marks.
7. Infer shrink safety from free-byte metrics alone.
8. Treat local projected cluster metrics as authority for destructive work.
9. Use a user-facing filespace name as durable authority; all decisions use UUIDv7 identity.

## Required metric dependencies

| Metric | Namespace | Required | Freshness | Use | Missing/stale behavior |
| --- | --- | --- | --- | --- | --- |
| `sb_filespace_total_bytes` | `sys.metrics.storage.filespaces` | yes | <=15s | capacity denominator | capacity action denied |
| `sb_filespace_used_bytes` | `sys.metrics.storage.filespaces` | yes | <=15s | capacity use | capacity action denied |
| `sb_filespace_free_bytes` | `sys.metrics.storage.filespaces` | yes | <=15s | expansion/shrink decision | capacity action denied |
| `sb_filespace_reserved_bytes` | `sys.metrics.storage.filespaces` | yes | <=15s | reservation pressure | capacity action denied |
| `sb_filespace_expandable_bytes` | `sys.metrics.storage.filespaces` | yes for expansion | <=60s | expansion proof | expansion denied |
| `sb_filespace_growth_rate_bytes_per_second` | `sys.metrics.storage.filespaces` | yes for forecast | <=300s | depletion forecast | forecast recommendation suppressed |
| `sb_filespace_depletion_eta_seconds` | `sys.metrics.storage.filespaces` | yes for forecast | <=300s | proactive expansion | proactive action denied |
| `sb_filespace_shrink_candidate_bytes` | `sys.metrics.storage.filespaces` | yes for shrink | <=60s | shrink candidate proof | shrink denied |
| `sb_filespace_truncate_ready_bytes` | `sys.metrics.storage.filespaces` | yes for truncate | <=60s | truncate safe bytes | truncate denied |
| `sb_filespace_pending_page_relocation_bytes` | `sys.metrics.storage.filespaces` | yes for shrink | <=60s | page relocation backlog | shrink denied |
| `sb_filespace_shrink_blocker_count` | `sys.metrics.storage.filespaces` | yes for shrink | <=60s | blocker proof | shrink denied |
| `sb_filespace_device_read_latency_microseconds` | `sys.metrics.storage.filespaces` | yes | <=15s | placement/cost/health | health unknown |
| `sb_filespace_device_write_latency_microseconds` | `sys.metrics.storage.filespaces` | yes | <=15s | placement/cost/health | health unknown |
| `sb_filespace_fsync_latency_microseconds` | `sys.metrics.storage.filespaces` | yes | <=15s | durability pressure | health unknown |
| `sb_filespace_device_error_total` | `sys.metrics.storage.filespaces` | yes | <=5s | degraded state | destructive work denied |
| `sb_filespace_health_state` | `sys.metrics.storage.filespaces` | yes | <=15s | lifecycle safety | action denied if unknown/degraded unless policy explicitly allows migration away |
| `sb_filespace_role_state` | `sys.metrics.storage.filespaces` | yes | <=15s | role/lifecycle authority | action denied |

The metrics above are storage/input metrics. Agent action, refusal, contract, and lifecycle-decision metrics are produced under `sys.metrics.agents.filespace.*` and must not be written by `page_allocation_manager`.

## Actions

| Action id | Receiver | Evidence required | Permission | Policy gate | Fail-closed behavior |
| --- | --- | --- | --- | --- | --- |
| `request_filespace_expand` | filespace lifecycle subsystem | filespace UUID, target bytes, policy UUID, capacity proof, device proof | `OBS_AGENT_ACTION_APPROVE` for apply | `filespace_capacity_policy.expand_allowed` | deny expansion request |
| `request_filespace_move` | filespace lifecycle subsystem | source UUID, target UUID, object list proof, policy UUID, startup safety state | `OBS_AGENT_ACTION_APPROVE` | `filespace_capacity_policy.move_allowed` | emit recommendation only |
| `request_filespace_shrink` | `page_allocation_manager` first | target byte range, page relocation request UUID, policy UUID | `OBS_AGENT_ACTION_APPROVE` | `filespace_capacity_policy.shrink_allowed` | deny shrink |
| `request_filespace_truncate` | filespace lifecycle subsystem | `publish_shrink_ready` evidence UUID, safe tail bytes, blocker count zero | `OBS_AGENT_ACTION_APPROVE` | `filespace_capacity_policy.truncate_allowed` | deny truncate |
| `request_filespace_quarantine` | filespace lifecycle subsystem | device/checksum/unknown-page evidence | `OBS_AGENT_ACTION_APPROVE` unless critical policy allows automatic quarantine | `storage_health_policy` and `filespace_capacity_policy` | operator review |
| `recommend_primary_shadow_promotion` | operator/manager | primary degradation proof, candidate readiness proof, catalog persistence migration requirement | `OBS_AGENT_RECOMMENDATION_READ` | `filespace_shadow_promotion_policy` | no recommendation if proof missing |

## Baseline policy schema

| Field | Type | Default | Valid range | Requirement |
| --- | --- | --- | --- | --- |
| `minimum_free_bytes` | unsigned integer | `1073741824` | `0..2^63-1` | Below this, emit capacity warning. |
| `minimum_free_percent` | ratio | `10` | `0..100` | Below this, emit capacity warning. |
| `growth_rate_window_seconds` | unsigned integer | `3600` | `60..604800` | Window for growth-rate calculation. |
| `depletion_eta_warning_seconds` | unsigned integer | `86400` | `0..31536000` | Warning horizon. |
| `depletion_eta_action_seconds` | unsigned integer | `14400` | `0..31536000` | Action horizon. |
| `expand_allowed` | boolean | `false` | boolean | Live expansion disabled by default. |
| `move_allowed` | boolean | `false` | boolean | Live movement disabled by default. |
| `shrink_allowed` | boolean | `false` | boolean | Live shrink disabled by default. |
| `truncate_allowed` | boolean | `false` | boolean | Live truncation disabled by default. |
| `critical_device_error_threshold` | unsigned integer | `1` | `0..2^31-1` | Device errors above threshold block destructive action. |
| `fsync_p99_critical_us` | unsigned integer | `50000` | `1..2^63-1` | Durability health threshold. |
| `required_approval_class` | enum | `operator_approval` | `none`, `operator_approval`, `sysarch_approval`, `break_glass` | Destructive defaults cannot use `none`. |
| `minimum_free_pages` | unsigned integer | `4` | `0..2^31-1` | Below this, the agent treats the filespace as below the emergency page reserve. |
| `target_free_pages` | unsigned integer | `8` | `minimum_free_pages..2^31-1` | Normal local target for immediately available free/released pages. |
| `page_low_water_notify_ratio` | ratio | `0.50` | `0..1` | Page-allocation manager must notify this agent when released plus free pages falls below this ratio of `target_free_pages`. |
| `autoextend_increment_pages` | unsigned integer | `8` | `1..2^31-1` | Default physical page increment considered when autoextend is enabled by policy. |
| `autoextend_cooldown_seconds` | unsigned integer | `60` | `0..86400` | Minimum cooldown between live extension attempts. |
| `device_pressure_autoextend_refusal_state` | enum | `degraded` | `unknown`, `degraded`, `critical`, `failed` | Autoextend fails closed at or above the configured state. |

## Cross-agent contract

Search key: `AGH_FILESPACE_PAGE_AGENT_CONTRACT_FILESIDE`.

The agent MUST use explicit contract records when interacting with `page_allocation_manager`.

| Contract direction | Required record | Filespace agent duty |
| --- | --- | --- |
| Capacity request from page manager | `capacity_probe`, `capacity_reserve`, `capacity_extend` | Validate policy, physical capacity, device pressure, owner lock, and hold gates before accepting. |
| Capacity response to page manager | `capacity_granted`, `capacity_denied`, `capacity_window_open`, `capacity_window_closed` | Publish durable evidence with request UUID, filespace UUID, physical filespace ID, granted bytes/pages, expiry, and blocker list. |
| Shrink request to page manager | `shrink_probe`, `relocation_required` | Identify candidate physical tail range but never move pages. |
| Shrink response from page manager | `shrink_blocked`, `shrink_ready`, `relocation_complete`, `relocation_failed` | Consume page-manager evidence before physical truncate/delete and fail closed if evidence is stale or incomplete. |

The filespace agent MUST NOT infer page-family readiness from free bytes. It may only perform shrink, truncate, or delete after page-manager evidence proves that page-level blockers are clear.

## Shrink/truncate sequence

1. Filespace agent detects candidate free tail range or operator requests shrink.
2. Filespace agent writes `filespace_shrink_request_evidence` and sends `request_filespace_shrink` to `page_allocation_manager`.
3. Page agent scans target range and either emits blockers or relocates eligible pages by policy.
4. Page agent emits `publish_shrink_ready` only when the target range contains no movable/unmovable page blockers and all holds are clear.
5. Filespace agent validates `publish_shrink_ready`, filespace health, startup safety, backup/archive/transaction holds, and policy.
6. Filespace agent requests lifecycle truncation.
7. Filespace lifecycle subsystem writes evidence before reporting success.

## Diagnostics

| Diagnostic | Meaning |
| --- | --- |
| `FILESPACE_AGENT.METRIC_STALE` | Required metric is missing or stale. |
| `FILESPACE_AGENT.POLICY_INVALID` | Active policy is invalid or scope-incompatible. |
| `FILESPACE_AGENT.PAGE_AUTHORITY_REQUIRED` | Action requires page-agent proof. |
| `FILESPACE_AGENT.SHRINK_BLOCKED` | Page, transaction, backup, archive, startup, or recovery blocker exists. |
| `FILESPACE_AGENT.ACTION_RECOMMEND_ONLY` | Policy does not permit live action. |
| `FILESPACE_AGENT.PERMISSION_DENIED` | Caller or agent lacks required right. |
| `FILESPACE_AGENT.STARTUP_UNSAFE` | Startup/recovery state forbids mutation. |
