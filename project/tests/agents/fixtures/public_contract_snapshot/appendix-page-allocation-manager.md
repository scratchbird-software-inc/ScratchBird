# Page Allocation Manager

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
Search key: `AGH_PAGE_ALLOCATION_MANAGER`.

## Purpose

`page_allocation_manager` is the owning operational agent for page-family and page-type allocation health. It manages free-page counts, reserved pages, page preallocation, page allocation latency, fragmentation, relocation backlog, relocatable and unmovable byte classification, and page movement needed to make filespace shrink or movement safe.

The agent works with `filespace_capacity_manager` when additional space is required or when a filespace shrink, movement, truncation, or deletion requires page evacuation. It does not own physical filespace lifecycle authority.

## Authority boundary

| Rule | Requirement |
| --- | --- |
| Agent id | `page_allocation_manager`. |
| Scope | `database/filespace/page_family/page_type`. |
| Runtime host | Engine or manager process. |
| Default install state | Installed for every database. |
| Default runtime state | `recommend_only`. |
| Authority class | `request_action`; direct bounded preallocation is allowed only when `page_preallocation_policy` explicitly enables it. |
| Policy families | `page_preallocation_policy`, `page_relocation_policy`. |
| Agent-owned metric namespace | `sys.metrics.agents.page_allocation.*`; cluster projections use `cluster.sys.metrics.agents.page_allocation.*` only when cluster authority exists. |
| Evidence rule | Evidence must be written before reporting any preallocation, relocation, blocker, or shrink-readiness result. |
| Fail-closed rule | If page metrics, policy, filespace state, version pins, backup/archive holds, transaction holds, or startup safety are unclear, the agent emits refusal evidence and performs no page movement. |

## Responsibilities

The agent MUST:

1. Track page counts by filespace, page family, and page type.
2. Track allocation latency and failures.
3. Track reserved and preallocated page pools.
4. Forecast page-family demand from metric history and policy-defined workload windows.
5. Preallocate page families only when policy permits and filespace capacity manager has not refused capacity.
6. Track fragmentation and relocation backlog.
7. Classify bytes/pages as relocatable or unmovable with blocker reasons.
8. Relocate pages only through page manager/storage rules that preserve MGA visibility and page identity.
9. Publish shrink-readiness proof to `filespace_capacity_manager` only when the target range is safe.
10. Refuse page relocation when any page identity, checksum, transaction hold, archive hold, backup hold, unknown page, or encryption/decryption requirement is unresolved.
11. Notify `filespace_capacity_manager` when released plus free pages falls below the active policy low-water ratio, defaulting to `50%` of the target free page count.
12. Maintain agent-owned operational metrics under `sys.metrics.agents.page_allocation.*`; page facts under `sys.metrics.storage.pages` remain storage/page input metrics.

## Forbidden actions

The agent MUST NOT:

1. Expand a filespace.
2. Truncate a filespace.
3. Delete or detach a filespace.
4. Promote or demote a primary/shadow/candidate filespace.
5. Change device placement or physical disk policy.
6. Rebuild an index without an index-management request.
7. Treat free bytes as proof that pages are movable.
8. Use names instead of UUIDv7 identities.

## Required metric dependencies

| Metric | Namespace | Required | Freshness | Use | Missing/stale behavior |
| --- | --- | --- | --- | --- | --- |
| `sb_page_free_count` | `sys.metrics.storage.pages` | yes | <=15s | allocation/preallocation decision | no live allocation recommendation |
| `sb_page_allocated_count` | `sys.metrics.storage.pages` | yes | <=15s | capacity and fragmentation | no live relocation recommendation |
| `sb_page_reserved_count` | `sys.metrics.storage.pages` | yes | <=15s | reservation pressure | no live preallocation |
| `sb_page_preallocated_count` | `sys.metrics.storage.pages` | yes | <=15s | ingestion readiness | preallocation recommendation suppressed |
| `sb_page_preallocation_target_count` | `sys.metrics.storage.pages` | yes for preallocation | <=300s | target comparison | preallocation denied |
| `sb_page_preallocation_deficit_count` | `sys.metrics.storage.pages` | yes for preallocation | <=300s | proactive allocation | preallocation denied |
| `sb_page_allocation_latency_microseconds` | `sys.metrics.storage.pages` | yes | <=15s | allocator health | health unknown |
| `sb_page_allocation_failures_total` | `sys.metrics.storage.pages` | yes | <=15s | pressure/refusal | deny preallocation |
| `sb_page_fragmentation_ratio` | `sys.metrics.storage.pages` | yes for relocation | <=300s | relocation trigger | relocation recommendation suppressed |
| `sb_page_relocation_backlog_count` | `sys.metrics.storage.pages` | yes for relocation | <=60s | backlog health | relocation denied |
| `sb_page_relocation_blocked_total` | `sys.metrics.storage.pages` | yes for shrink | <=60s | blocker proof | shrink readiness denied |
| `sb_page_relocatable_bytes` | `sys.metrics.storage.pages` | yes for shrink | <=60s | movable tail proof | shrink readiness denied |
| `sb_page_unmovable_bytes` | `sys.metrics.storage.pages` | yes for shrink | <=60s | blocker proof | shrink readiness denied |
| `sb_page_family_growth_rate_pages_per_second` | `sys.metrics.storage.pages` | yes for forecast | <=300s | preallocation forecast | forecast suppressed |
| `sb_page_family_depletion_eta_seconds` | `sys.metrics.storage.pages` | yes for forecast | <=300s | proactive allocation | proactive action denied |
| `sb_page_relocation_ready_for_filespace_shrink` | `sys.metrics.storage.pages` | yes for shrink | <=60s | shrink-ready proof | filespace truncate denied |

The metrics above are storage/page input metrics. Agent action, refusal, contract, and page-allocation decision metrics are produced under `sys.metrics.agents.page_allocation.*` and must not be written by `filespace_capacity_manager`.

## Actions

| Action id | Receiver | Evidence required | Permission | Policy gate | Fail-closed behavior |
| --- | --- | --- | --- | --- | --- |
| `preallocate_page_family` | page manager | page family, page type, target count, filespace UUID, policy UUID, capacity acceptance | `OBS_AGENT_CONTROL` internal plus policy | `page_preallocation_policy.preallocation_allowed` | emit recommendation only |
| `relocate_pages` | page manager | source/target page set or range, blocker scan, policy UUID, transaction safety proof | `OBS_AGENT_ACTION_APPROVE` for live movement | `page_relocation_policy.relocation_allowed` | deny relocation |
| `defragment_page_family` | page manager | fragmentation proof, affected range, policy UUID, max pages per interval | `OBS_AGENT_ACTION_APPROVE` | `page_relocation_policy.defragment_allowed` | recommendation only |
| `publish_shrink_ready` | `filespace_capacity_manager` | filespace UUID, safe byte range, blocker count zero, scan generation, metric freshness | internal evidence publish | `page_relocation_policy.publish_shrink_ready_allowed` | publish blocker report instead |
| `request_filespace_capacity` | `filespace_capacity_manager` | page family, target pages, projected bytes, policy UUID | internal recommendation | `page_preallocation_policy.request_capacity_allowed` | preallocation suppressed |

## Baseline preallocation policy schema

| Field | Type | Default | Valid range | Requirement |
| --- | --- | --- | --- | --- |
| `preallocation_allowed` | boolean | `false` | boolean | Live preallocation disabled by default. |
| `forecast_window_seconds` | unsigned integer | `3600` | `60..604800` | Window used for demand forecast. |
| `history_confidence_threshold` | ratio | `0.80` | `0..1` | Required before proactive preallocation. |
| `max_reserved_bytes_per_filespace` | unsigned integer | `1073741824` | `0..2^63-1` | Hard cap for page preallocation. |
| `allowed_page_families` | enum set | `data,index,blob,metrics` | known page families | Families eligible for proactive allocation. |
| `workload_calendar` | object reference | `none` | policy object or none | Optional recurring workload schedule. |
| `allocation_throttle_pages_per_second` | unsigned integer | `128` | `1..2^31-1` | Prevents preallocation from starving foreground work. |
| `minimum_free_pages` | unsigned integer | `4` | `0..2^31-1` | Below this, allocation enters emergency reserve behavior. |
| `target_free_pages` | unsigned integer | `8` | `minimum_free_pages..2^31-1` | Normal local target for immediately available free/released pages. |
| `low_water_notify_ratio` | ratio | `0.50` | `0..1` | Notify filespace capacity manager when released plus free pages falls below this ratio of `target_free_pages`. |
| `capacity_evidence_required` | boolean | `true` | boolean | Live preallocation that requires physical growth must have current filespace capacity evidence. |

## Baseline relocation policy schema

| Field | Type | Default | Valid range | Requirement |
| --- | --- | --- | --- | --- |
| `relocation_allowed` | boolean | `false` | boolean | Live relocation disabled by default. |
| `defragment_allowed` | boolean | `false` | boolean | Live defragmentation disabled by default. |
| `publish_shrink_ready_allowed` | boolean | `true` | boolean | Publishing evidence is allowed after read-only scan. |
| `max_pages_per_interval` | unsigned integer | `1024` | `1..2^31-1` | Relocation throttle. |
| `interval_seconds` | unsigned integer | `60` | `1..86400` | Throttle interval. |
| `unmovable_blocker_classes` | enum set | all blocker classes | known blockers | Any listed blocker prevents shrink readiness. |
| `require_checksum_valid` | boolean | `true` | boolean | Unknown or bad checksum pages cannot move. |
| `require_transaction_holds_clear` | boolean | `true` | boolean | MGA safety requirement. |
| `require_backup_archive_holds_clear` | boolean | `true` | boolean | Backup/archive safety requirement. |

## Cross-agent contract

Search key: `AGH_FILESPACE_PAGE_AGENT_CONTRACT_PAGESIDE`.

The agent MUST use explicit contract records when interacting with `filespace_capacity_manager`.

| Contract direction | Required record | Page-allocation manager duty |
| --- | --- | --- |
| Capacity request to filespace agent | `capacity_probe`, `capacity_reserve`, `capacity_extend` | Request physical capacity with page-family demand, requested pages/bytes, policy UUID, priority, and deadline. |
| Capacity response from filespace agent | `capacity_granted`, `capacity_denied`, `capacity_window_open`, `capacity_window_closed` | Consume only current evidence; fail closed if expired, stale, or mismatched to filespace UUID or physical filespace ID. |
| Shrink request from filespace agent | `shrink_probe`, `relocation_required` | Scan target range, classify pages, relocate only when policy permits, and publish blockers or readiness. |
| Shrink response to filespace agent | `shrink_blocked`, `shrink_ready`, `relocation_complete`, `relocation_failed` | Publish durable evidence; never truncate, move, attach, detach, promote, demote, or delete a physical filespace. |

The page-allocation manager MUST NOT create physical capacity. It converts available filespace capacity into page-family/page-type free, reserved, or preallocated pools only after filespace capacity evidence exists.

## Shrink-readiness proof

A `publish_shrink_ready` evidence record MUST include:

| Field | Requirement |
| --- | --- |
| `filespace_uuid` | Target filespace UUIDv7. |
| `scan_generation` | Monotonic page-agent scan generation. |
| `safe_start_byte` | First byte of truncate-safe range. |
| `safe_end_byte` | Last byte of truncate-safe range. |
| `relocated_page_count` | Count of pages moved for this proof. |
| `remaining_page_count` | Must be zero in the target range. |
| `unmovable_bytes` | Must be zero for truncate-ready proof. |
| `blocker_summary` | Empty for truncate-ready proof; otherwise deterministic blocker rows. |
| `policy_uuid` | Active relocation policy used. |
| `metric_snapshot_uuid` | Metric snapshot used for the decision. |
| `created_at` | Engine time with clock-quality annotation. |

## Diagnostics

| Diagnostic | Meaning |
| --- | --- |
| `PAGE_AGENT.METRIC_STALE` | Required page metric is missing or stale. |
| `PAGE_AGENT.POLICY_INVALID` | Active policy is invalid or scope-incompatible. |
| `PAGE_AGENT.FILESPACE_CAPACITY_REQUIRED` | More capacity is needed before page action can proceed. |
| `PAGE_AGENT.RELOCATION_BLOCKED` | One or more pages cannot move. |
| `PAGE_AGENT.UNKNOWN_PAGE` | Unknown page prevents relocation or shrink readiness. |
| `PAGE_AGENT.TRANSACTION_HOLD` | MGA transaction state blocks page movement. |
| `PAGE_AGENT.BACKUP_ARCHIVE_HOLD` | Backup or archive state blocks movement. |
| `PAGE_AGENT.PERMISSION_DENIED` | Required action permission is absent. |
