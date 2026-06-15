# Filespaces And Storage

## Purpose

A filespace is the engine's unit of physical storage allocation. Every ScratchBird database is backed by at least one filespace (the active primary), and administrators can attach additional secondary filespaces to spread data across volumes, separate index storage from row storage, or hold overflow and history. This chapter explains how filespace identity works, which operations are native administrative operations versus emulated compatibility operations, how the engine responds when storage is unhealthy, and how a filespace move proceeds.

## The Active Primary Filespace

Every open database has exactly one active primary filespace. Its `physical_filespace_id` is always `kActivePrimaryPhysicalFilespaceId = 0` (defined in `filespace_identity.hpp`). The active primary:

- Holds the startup state record that encodes the `database_uuid` and `first_filespace_uuid`.
- Is the startup authority (`startup_authority = true` in `FilespaceDescriptor`).
- Is the catalog persistence owner (`catalog_persistence_owner = true`).
- Is the filespace manifest owner and recovery evidence owner.

A page ID in ScratchBird encodes both the physical filespace number (16 bits) and the page number (48 bits, up to `kMaxFilespacePageNumber = (1 << 48) - 1`). The reserved physical filespace ID `kReservedPhysicalFilespaceId = 1` is not allocatable.

The active primary is required to open the database. If it is missing, absent, or unreadable, the engine cannot open the database at all. Ordinary sessions cannot proceed without the primary filespace online.

## Filespace Roles

Each filespace carries a `FilespaceRole` that describes its purpose:

| Role | Meaning |
|------|---------|
| `active_primary` | The primary filespace; required for open |
| `primary_shadow` | A shadow copy of the primary for high availability |
| `primary_snapshot` | A snapshot of the primary |
| `primary_candidate` | A secondary being promoted to primary |
| `secondary_data` | Additional row storage |
| `secondary_index` | Dedicated index storage |
| `secondary_overflow` | Overflow data (large rows) |
| `secondary_history` | Historical versions for MGA |
| `secondary_shard` | Shard storage in partitioned configurations |
| `archive_history` | Archived historical data |
| `archive_log` | Archived transaction log segments |
| `archive_detached` | Archived filespace no longer online |
| `temporary` | Temporary spill storage |
| `import_candidate` | Being evaluated for import into the database |
| `drop_pending` | Scheduled for deletion |
| `forbidden` | Not usable |

## Filespace States

Each filespace also carries a `FilespaceState` describing its current availability:

| State | Meaning |
|-------|---------|
| `online` / `attached` | Active and writable |
| `read_only` | Attached but not writable |
| `detached` | Not currently attached |
| `archived` | Archived; not online |
| `deleted` / `dropped` | Removed |
| `creating` | Being created |
| `initializing` | Being initialized |
| `maintenance` | Under maintenance |
| `moving` | Physical relocation in progress |
| `relocating_objects` | Objects being moved between filespaces |
| `promoting` | Being promoted to primary |
| `demoting` | Being demoted from primary |
| `detaching` | Detach in progress |
| `drop_pending` | Scheduled for drop |
| `quarantine` | Suspended due to integrity or identity concern |
| `forbidden` | Not usable |

## Filespace Operations

Native filespace lifecycle operations are defined by the `FilespaceOperation` enum in `filespace_lifecycle.hpp`:

| Operation | SQL surface |
|-----------|-------------|
| `create_filespace` | `CREATE FILESPACE` |
| `attach_filespace` | `ALTER DATABASE ... ATTACH FILESPACE` |
| `detach_filespace` | `ALTER DATABASE ... DETACH FILESPACE` |
| `promote_filespace` | Promote a secondary to primary |
| `demote_filespace` | Demote the current primary |
| `set_read_only` | Mark a filespace read-only |
| `set_read_write` | Restore write access |
| `verify_filespace` | Structural verification |
| `compact_filespace` | Reclaim free space |
| `drop_filespace` | Remove filespace and catalog entry |
| `move_filespace` | Relocate physical files |
| `quarantine_filespace` | Operator-initiated quarantine |
| `repair_filespace` | Structural repair |
| `rebuild_filespace` | Full rebuild from sources |
| `salvage_filespace` | Best-effort recovery of salvageable data |

These are native ScratchBird operations. They are distinct from emulated compatibility operations such as `ALTER DATABASE ... FILE` (mapped to `sbsql.emulated.database_file_management` in the statement catalog). Operators should use native filespace operations for production storage management.

## Filespace Identity Verification

Each filespace carries a `writer_identity_uuid` in its descriptor. The foreign filespace quarantine mechanism (`foreign_filespace_quarantine.cpp`) checks whether a filespace being attached belongs to the database by verifying that the identity UUID matches. If the filespace was written by a different database instance, it is classified as foreign and placed in quarantine state (`FilespaceState::quarantine`) with the reason `import_into_foreign_filespace_quarantine`. The metric `sb_foreign_filespace_quarantine_total` is incremented for each such event.

This check prevents accidental or malicious attachment of a filespace from a different database. Quarantine is the safe-fail outcome when identity cannot be confirmed.

## Filespace Lifecycle Policy

The `FilespaceLifecyclePolicy` struct governs which operations are permitted at each stage. Key restrictions:

- Primary detach is disabled by default (`allow_primary_detach = false`).
- Physical deletion requires all retention and legal-hold conditions to be satisfied (`physical_delete_retention_satisfied`, `physical_delete_legal_hold_clear`).
- Most destructive operations (detach, promote, drop, quarantine, move, merge, repair, rebuild, salvage) require that no active pins are held on the filespace.
- A filespace move requires `allow_filespace_move = true`, `page_agent_relocation_complete_for_move = true`, and `startup_open_safe_for_move = true`.
- Attach requires a valid physical header by default (`require_physical_header_for_attach = true`).
- Evidence is recorded before the operation succeeds (`evidence_before_success = true`), not after.

## Filespace Pins

A `FilespacePin` holds a filespace in a particular state to prevent it from being dropped, moved, or detached while a long-running operation depends on it. Pin kinds are:

| Kind | Owner |
|------|-------|
| `page_owner` | Page cache or allocation system |
| `transaction` | Active transaction |
| `backup` | Backup operation |
| `archive` | Archive operation |
| `catalog` | Catalog access |
| `external` | Operator or external system hold |

Most lifecycle operations that could disrupt an active owner require that no pins of the relevant kind are present. A blocked operation returns a `FilespaceLifecycleBlocker` with the blocker kind, owner subsystem, reason, and evidence UUID.

## Filespace Growth and Preallocation

When insert pressure exceeds available free space, the engine requests filespace growth through `RequestInsertFilespaceGrowth`. Growth urgency is classified as `background`, `normal`, `high`, or `critical`. The wait policy for a growth request can be `no_wait`, `bounded_wait`, `background_only`, or `refused`.

The engine can also preallocate pages ahead of demand via `PreallocateFilespace`. Preallocation allows the physical file to grow incrementally rather than in large bursts at high-urgency moments. The preallocation state transitions through `absent`, `admitted_pending_allocation`, `allocation_complete`, `refused`, and `quarantine`.

Physical growth of the underlying file is tracked separately in `FilespacePhysicalGrowthEntry`. A physical growth operation extends the file, syncs it to disk, and updates the physical header before updating the metadata. The field `physical_extension_synced` must be `true` before the operation is considered complete. If the sync fails, the operation remains in an incomplete state and recovery must determine whether to retain or roll back the partial growth.

Growth operations that encounter an unresolvable problem transition to `quarantine` state. Recovery classification for growth (`ClassifyFilespaceGrowthForRecovery`) assigns one of `no_action`, `complete`, `roll_back`, `quarantine`, or `fail_closed`.

## What Happens When a Filespace Is Missing or Unavailable

If a secondary filespace is missing at startup, the database may still open if the primary is intact. The secondary's state will show as `absent` or `detached`. The engine applies the `FilespaceOpenSafetyMode` for the filespace:

| Safety mode | Meaning |
|-------------|---------|
| `normal` | No restriction; filespace is online |
| `read_only` | Attached in read-only mode |
| `maintenance` | Under maintenance |
| `restricted_open` | Restricted-open mode required |
| `recovery_required` | Recovery must complete before use |

A secondary filespace in `recovery_required` mode prevents the data it owns from being written until recovery completes. Reads may proceed depending on the specific data affected.

If the active primary filespace is missing, the database cannot open. The engine returns an identity or format error diagnostic. The startup state record cannot be read, so the engine cannot verify the database UUID or confirm which transaction was the last clean commit.

## How a Filespace Move Proceeds

A filespace move is a multi-phase operation governed by `FilespaceMovePlan` in `filespace_secondary.hpp`. The plan records:

- `source_path` — where the filespace currently lives
- `target_path` — where it will live after the move
- `operator_approved` — explicit operator approval recorded
- `page_agent_relocation_complete` — the page agent has finished moving all page references
- `startup_open_safe` — the engine has confirmed that the next startup can open the filespace at the target path

To verify a move is complete, inspect all three fields. A move where `page_agent_relocation_complete = false` means objects are still referencing the source location. A move where `startup_open_safe = false` means the startup authority has not yet confirmed the new path is safe to use on restart. Both must be true before the move is considered complete.

If a move is blocked, the result carries a `blockers` vector with one or more `FilespaceLifecycleBlocker` entries. Common blockers include active transactions, backup operations, and page allocations in progress.

## Low Space and Disk-Full Behavior

The `SecondaryFilespacePolicy` sets:

- `min_free_pages = 4` — the minimum number of free pages before further writes are refused.
- `target_free_pages = 8` — the target free page count that growth tries to maintain.
- `low_water_ratio = 0.50` — the ratio at which growth urgency escalates.

When free pages fall below the minimum, insert operations that would require new page allocations are refused. The growth urgency escalates from `background` to `normal` to `high` to `critical` as capacity tightens. At `critical` urgency, the engine uses the `bounded_wait` or `refused` wait policy depending on configuration.

`allow_auto_extend = true` by default. `allow_auto_shrink = false` by default — the engine does not automatically shrink filespaces.

## Storage Health Checks

The disk health API (`CheckDiskDeviceHealth` in `database_lifecycle.cpp`) checks the underlying file device before and during operations. The `DiskHealthSnapshot` captures the current health state. A filespace verify operation (`verify_filespace`) performs a structural check independent of the health snapshot.

## Relationship to Diagnostics and Support Bundles

Filespace state, quarantine events, and growth records are included in diagnostics. The support bundle excludes protected material (encryption keys associated with encrypted filespaces) but includes filespace path and state information. As noted in [Identity, Security, And Policy](identity_security_and_policy.md), paths in support bundles are replaced with `[path-redacted]`. Operators collecting a support bundle from a system with filespace problems should also capture the raw filespace directory listing via an administrative query to `sys.storage` before generating the bundle.

## Operator Questions Answered

**Where are durable files allowed to live?** The engine does not restrict the filesystem path for filespace files beyond what the operating system enforces. However, the storage filespace profile policy (`storage.filespace_profile`, `single_active_primary_v1`) requires exactly one active primary. Path is not identity — two filespaces at different paths with the same UUID will cause an ambiguous identity refusal.

**Which filespace is required to open the database?** The active primary (physical ID 0). It must be present, readable, and carry a valid format header. Secondary filespaces are optional for open but required for any data that lives exclusively in them.

**What happens if a filespace is missing or unavailable?** If secondary: the database may open with restricted capabilities; data on the missing filespace is inaccessible until it is reattached. If primary: the database cannot open.

**Which operations are native administrative operations rather than compatibility operations?** Native: `CREATE FILESPACE`, `ALTER DATABASE ATTACH FILESPACE`, `ALTER DATABASE DETACH FILESPACE`, promote/demote, verify, compact, drop, move, repair. Compatibility emulations: `ALTER DATABASE ... FILE`, `CREATE SHADOW` (non-filespace form), `BACKUP DATABASE` without a full path. Use native filespace operations in new deployments.

## Related Pages

- [Database Lifecycle](database_lifecycle.md)
- [Diagnostics, Message Vectors, And Support Bundles](diagnostics_message_vectors_and_support_bundles.md)
- [Language Reference: Filespace](../Language_Reference/syntax_reference/filespace.md)
