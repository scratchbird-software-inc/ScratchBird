# Multi-Generational Foundation

## Purpose

This page summarizes the Multi-Generational Architecture (MGA) as the storage
and transaction foundation of ScratchBird, explains the structural choices that
distinguish it from conventional approaches, and links to the deep treatment.
MGA is load-bearing for the CDE design: it is what allows many data models, many
client dialects, and many concurrent readers and writers to share one storage
authority without compromising correctness.

For the plain-language introduction with diagrams, see
[../Getting_Started/core_concepts/understanding_mga.md](../Getting_Started/core_concepts/understanding_mga.md).
For the full architectural treatment, see
[../Getting_Started/architecture/storage_transactions_and_recovery.md](../Getting_Started/architecture/storage_transactions_and_recovery.md).
For the transaction language contract, see
[../Language_Reference/core_paradigms/transactions_and_recovery.md](../Language_Reference/core_paradigms/transactions_and_recovery.md).

**This is a draft.** Nothing here is a performance claim or a production
certification.

---

## What MGA Is

MGA stands for Multi-Generational Architecture. The central idea: the storage
engine never destroys an existing row version when that row is updated or
deleted. Instead it writes a new version alongside the old one, and the old
version remains visible to any transaction whose snapshot predates the change.

This is the opposite of a conventional write-in-place model, where a row
update physically overwrites the old value, and a separate write-ahead log
is needed to reconstruct what the value was before the update in case of
a crash or concurrent read.

ScratchBird's MGA does not use a write-ahead log for transaction recovery. The
storage itself is versioned: the old versions are the recovery record.
Verified in `src/storage/database/physical_mga_cow_store.cpp` and
`src/transaction/mga/`.

---

## Versions, Not Overwrites

Every change to a row produces a new row version with its own transaction-id
range that defines when it is visible. The key fields in a row version:

- The transaction id that created this version.
- The transaction id that retired this version (zero if still active).
- A back-pointer to the previous version of the same logical row.

When a reader opens a snapshot, it sees all committed versions whose creation
transaction id is at or below its snapshot point, and whose retirement transaction
id (if set) is above its snapshot point. Readers never wait for writers; writers
never block readers.

The transaction cleanup mechanism (`src/transaction/mga/transaction_cleanup_horizon_service.hpp`)
maintains an authoritative cleanup horizon — the oldest transaction id that any
active snapshot still needs. Old row versions whose retirement id falls below
that horizon are eligible for reclamation. The `storage_version_cleanup_agent`
in the autonomous agent runtime handles background cleanup with explicit scoped
authority (see [autonomous_operation.md](autonomous_operation.md)).

---

## Snapshot Isolation And Time-Travel

Every transaction opens with a snapshot. The snapshot is a precise cut of the
transaction inventory at the moment the transaction begins. Data changes made
by other transactions after that point are invisible to the snapshot, even if
those transactions commit before the current one closes.

This supports:

- **Consistent reads** across model families within a single transaction, with
  no possibility of phantom reads from concurrent writes.
- **Point-in-time queries** against historical row versions, without a separate
  change-log product.
- **Change tracking** derived from the version chain: the history of a row is
  physically present in storage and queryable without a separate audit table.

The filespace model (`src/storage/filespace/`) organizes storage into tiers
that support the lifecycle of row versions: hot active data, archive data
that is old but not yet reclaimed, and filespaces that have been retired. This
layering underpins the time-travel and history query capabilities.

---

## No Write-Ahead Log For Transaction Authority

A critical design choice is the absence of a write-ahead log as a transaction
authority mechanism. In a write-ahead-log design, the log is the record of
committed intent, and the data pages are derived from replaying it. ScratchBird
inverts this: the data pages carry versioned row history directly, and the
engine's transaction inventory is the authority on commit finality.

This is enforced structurally in the `EngineNoSqlMgaRecheckProof` struct
(verified in `src/engine/internal_api/nosql/nosql_physical_provider_contract.hpp`):

```
bool write_ahead_log_claims_transaction_finality_authority = false;  // wal-not-authority
```

The comment `wal-not-authority` appears in the source as an explicit design
marker. The write-ahead log cannot be the authority; the engine transaction
inventory is.

---

## Why MGA Is The CDE Foundation

MGA is not incidental to the CDE design; it is what makes convergence safe:

1. **One transaction system for all model families.** Document inserts, graph
   edge writes, vector insertions, and relational row changes all participate in
   the same MGA transaction inventory. Commit boundaries are engine-controlled
   regardless of which model family was involved.

2. **The recheck invariant depends on MGA.** The candidate-evidence invariant
   (see [convergent_multi_model.md](convergent_multi_model.md)) requires that
   the engine can recheck every candidate row against MGA visibility. That
   recheck is only possible because MGA visibility is tracked centrally in the
   engine's transaction inventory, not delegated to any specialized provider.

3. **No per-model-family recovery path.** Because there is no write-ahead log
   and the versioned storage is the recovery record, there is no need for
   separate recovery logic per data model. The same cleanup and recovery
   mechanisms apply to all model families.

4. **Point-in-time history across all model families.** A historical query
   against a database snapshot sees consistent data across relational, document,
   graph, and vector data simultaneously, because they all share one version
   history and one snapshot mechanism.

---

## Key Source Locations

| Concept | Source path |
|---------|-------------|
| Physical versioned store | `src/storage/database/physical_mga_cow_store.cpp` |
| Transaction MGA layer | `src/transaction/mga/` |
| Cleanup horizon service | `src/transaction/mga/transaction_cleanup_horizon_service.hpp` |
| MGA recheck proof | `src/engine/internal_api/nosql/nosql_physical_provider_contract.hpp` |
| Filespace lifecycle | `src/storage/filespace/` |

For full detail, follow the links at the top of this page.
