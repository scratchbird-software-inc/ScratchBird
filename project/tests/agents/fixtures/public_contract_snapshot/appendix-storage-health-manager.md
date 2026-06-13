# Storage Health Manager Agent Boundary

<!--
Copyright (c) 2025-2026 Dalton Calford. All rights reserved.

TRADE SECRET / PRIVATE / CONFIDENTIAL.
-->

Status: accepted controlling private specification.
Search key: `AGH_STORAGE_HEALTH_MANAGER_BOUNDARY`.

## Corrected role

`storage_health_manager` is an observe/recommend health summarizer. It consumes storage, filespace, page, checksum, device, and unknown-page metrics and emits health summaries, recommendations, and escalation evidence. It does not own filespace lifecycle actions and does not own page allocation or page relocation actions.

## Authority

| Field | Requirement |
| --- | --- |
| Agent id | `storage_health_manager`. |
| Scope | `node/database/filespace`. |
| Authority class | `recommend_only`. |
| Default runtime state | `recommend_only`. |
| Policy family | `storage_health_policy`. |
| Required metrics | Filespace health, page allocation health, legacy storage device latency/checksum/unknown-page families. |
| Allowed actions | `request_filespace_quarantine`, `update_storage_cost`, `emit_storage_health_summary`. |
| Forbidden actions | Expand, shrink, truncate, detach, delete, promote, demote, allocate pages, relocate pages, rebuild indexes. |

## Decision rule

If a storage-health condition requires physical filespace action, the agent routes a recommendation to `filespace_capacity_manager`. If it requires page movement, it routes a recommendation to `page_allocation_manager`. If it requires index rebuild or storage-class change for index work, it routes a recommendation to `index_health_manager` and the owning filespace/page agents.

The agent MUST NOT convert a health metric directly into a destructive action.
