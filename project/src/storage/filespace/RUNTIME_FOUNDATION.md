# Storage Filespace Runtime Foundation

This module implements local non-cluster filespace lifecycle authority for the current private engine implementation.

Implemented behaviors:

- UUIDv7 database/filespace identity validation.
- Filespace descriptor state transitions using the ScratchBird role/type registry, not reference SQL tablespace semantics.
- Initial database file semantics: first physical database file is the first filespace and is `active_primary`.
- Create and attach for `active_primary`, `primary_shadow`, `primary_snapshot`, `primary_candidate`, `secondary_data`, `secondary_index`, `secondary_overflow`, `secondary_history`, `secondary_shard`, `archive_history`, `archive_log`, `archive_detached`, `temporary`, `import_candidate`, `drop_pending`, and `forbidden` role/type names.
- Detach refusal while active pins exist.
- Primary detach refusal unless policy explicitly allows it.
- Promotion refusal while another primary exists unless replacement policy explicitly allows it.
- Read-only and read-write transitions.
- Archive-owner and history-owner transitions.
- Primary replacement behavior with `primary_shadow` / `primary_candidate` promotion to `active_primary`.
- Active pin add/remove semantics.
- Drop refusal while active pins exist.
- Evidence-before-success records for every durable state change.
- Deterministic diagnostics and filespace lifecycle metrics.
- Registry serialization/parsing for durable evidence round-trip tests.

Cluster filespace authority remains outside this local module.
