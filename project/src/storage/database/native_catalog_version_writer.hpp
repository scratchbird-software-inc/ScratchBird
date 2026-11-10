// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "careful_write.hpp"
#include "catalog_name_envelope.hpp"
#include "catalog_record_codec.hpp"
#include "filespace_page_zero.hpp"
#include "native_catalog_leaf_page.hpp"
#include "transaction_inventory_writer.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace scratchbird::storage::database {

// SEARCH_KEY: SB_CATALOG_SINGLE_WRITER (owner Q36 = 1, 2026-11-09). ONE CATALOG
// WRITER PER DATABASE AT A TIME: the caller serializes every call into this
// file (WriteCatalogSuccessorVersion, WriteCatalogInsertVersions,
// AdvanceCatalogRoot), as it serializes the transaction inventory writer.
//
// Why: each writer reads the pages it changes, checks them (conflicts,
// duplicate rows, objects and names, room), then writes them. Nothing holds
// the pages between the read and the write: a second writer in that window
// would pass the same checks against the same images (two creates of one name
// would both commit, leaving a database that does not open, with no winner),
// and its whole-page write would replace the first writer's page. The
// careful-write plan notices a changed preimage only page by page, after
// earlier pages may already be written; it is not a lock.
//
// What the rule stands in for: FirebirdSQL keeps concurrent writers apart with
// short physical latches on pages in its shared page cache (exclusive to
// write, shared to read; latch coupling down the tree; a reader that finds its
// key moved by a split follows the right sibling; precedence in the cache
// orders the disk writes), and logical conflicts are settled by record
// versions (MGA), not by latches. These writers have no page cache: they write
// straight to the file. Catalog changes are DDL and rare, so one writer at a
// time costs little here.
//
// To be rebuilt with the page manager (owner, 2026-11-09): per-page write
// latches in the server process (one process owns a database), readers
// without latches (whole-page seal checked, retried on a torn read: no waits
// on read paths), right-sibling following for splits, write order from
// precedence in the page cache; tests will then choose the method. When that
// lands, this rule and every SB_CATALOG_SINGLE_WRITER note in the .cpp go.
//
// SEARCH_KEY: SB_CATALOG_VERSION_WRITER
// In-place writes, step W3a (owner Q20 = 1; Q23 = 1, 2026-11-08). A
// transaction writes the successor version of an existing catalog row (an
// update, or a drop: a successor whose metadata says deleted) into a catalog
// leaf of the database file, in place, through a CarefulWriteSink. The
// transaction's state word in the in-place inventory decides its visibility;
// nothing is published.
//
// Leaves keep their whole-page SHA-256 seal (Q23 = 1): each change rewrites
// the whole page, whose image before the change is the plan's preimage. A
// torn page fails its seal and is refused on read until repaired.
//
// Placement: the predecessor's page when the successor fits there (one write,
// the successor and the predecessor's forward link together); otherwise
// another base leaf of the relation that the relation's head already reaches
// (the successor first, back link to its predecessor; then the predecessor's
// page with the forward link, which readers treat as a hint). Readers group a
// row's versions across every leaf the head reaches, so neither placement
// changes an index. When no reached leaf has room the write is refused
// relation_full: new leaves and index cells come with W3b.
//
// Conflicts (FirebirdSQL prepare_update; MGA: only write-write blocks). Over
// the row's versions, newest first: one by this transaction is the base; one
// by a transaction still in progress refuses write_conflict (no waiting); a
// rolled-back or dead one is passed over; a committed one is the base. The base
// must be the version the caller's snapshot saw, else update_conflict. A
// dropped base refuses row_deleted.
//
// Generations and epochs never run ahead of the catalog root: a successor's
// metadata catalog_generation and schema, security and resource epochs are at
// most the root's current values (refused ahead_of_catalog_root; a name row's
// resource epoch, the epoch its keys were derived under, is exempt); a runtime
// authority binding's own catalog_generation and security_epoch equal its
// metadata's, so the security reader's rule (never above the root) holds. The root is
// rewritten in place only after the changing transaction commits (owner Q24 =
// 1); a crash between that COMMIT and the rewrite then loses only the
// invalidation, never leaves a committed row the security reader refuses.
//
// Not here: inserts (WriteCatalogInsertVersions below, W3b-1a);
// localized-name records, whose envelope binds the page and slot placement
// chooses (W3b-1b); waiting; backing out dead versions (sweep).
enum class CatalogVersionWriteError : std::uint8_t {
  none,
  invalid_request,         // malformed binding or request; nothing read
  read_only_database,      // the SBSYS002 read-only flag; nothing read
  io_failure,              // a read failed; nothing written
  invalid_inventory,       // the transaction chains do not read and check
  invalid_catalog,         // a catalog page does not read, decode or bind
  transaction_not_active,  // the transaction is not in progress in the TIP
  read_only_transaction,   // the transaction began read-only
  row_not_found,
  write_conflict,          // a newer version (insert: a row carrying the object) by another transaction in progress
  update_conflict,         // the base is not the version the caller saw
  row_deleted,             // the base is a drop
  invalid_successor,       // the new metadata cannot follow the base
  ahead_of_catalog_root,   // a generation or epoch above the catalog root's
  transaction_not_committed,  // AdvanceCatalogRoot: the named transaction has not committed
  root_regression,         // AdvanceCatalogRoot: generation not the next, or an epoch lowered
  name_record_unsupported,
  relation_full,           // no reached leaf has room
  duplicate_row,           // insert: the row UUID or version UUID is already in the relation (or the batch)
  duplicate_object,        // insert: a committed or own row of the relation (or the batch) carries the object UUID
  invalid_insert,          // insert: the metadata cannot be a first version by this transaction
  index_full,              // insert: the index leaf the key reaches has no room (splits come with W3b-3)
  invalid_role,            // insert: the record kind does not belong in the relation (startup verifier rules)
  invalid_name,            // insert: a name entry's profiles, keys or collision policy do not resolve or agree
  duplicate_name,          // insert: a committed or own active entry already holds the name in its scope
  plan_refused,            // the careful-write plan refused before any write
  write_failed,            // a write or sync failed after one was attempted
  resource_exhausted,
};
const char* CatalogVersionWriteErrorName(CatalogVersionWriteError error) noexcept;

struct CatalogVersionWriterBinding {
  core::platform::Uuid database_uuid;
  // Page-zero root kind 2 (the catalog) or 8 (the feature catalog).
  disk::FilespaceRootReference catalog_root;
  // Page-zero root kind 4 (the first transaction-state page).
  disk::FilespaceRootReference state_root;
  // AdvanceCatalogRoot only: page-zero root kinds 6 and 7, the configuration
  // and security route roots, which repeat the catalog root's generation and
  // epochs. Named, they are brought to the new values after the catalog root.
  std::optional<disk::FilespaceRootReference> configuration_root, security_root;
  // The SBSYS002 read-only flag, from the caller's verified open.
  bool read_only_database = false;
  disk::CarefulWriteBarrierPolicy barrier_policy = disk::CarefulWriteBarrierPolicy::trust_forced_writes;
  // Bounds on what one write reads.
  std::size_t maximum_chain_pages = 4096;
  std::size_t maximum_index_pages = 4096;
  std::size_t maximum_base_pages = 4096;
};

struct CatalogVersionRequest {
  core::platform::u16 role = 0;  // relation role 1..6 in the catalog root
  core::platform::u64 transaction_order = 0;
  core::platform::Uuid transaction_uuid;
  core::platform::Uuid row_uuid;
  // The version the caller's snapshot saw: the newest committed version, or
  // this transaction's own newest.
  core::platform::Uuid predecessor_version_uuid;
  // The new version's identity, a UUIDv7 the caller issued.
  core::platform::Uuid version_uuid;
  // Creator fields name this transaction; row UUID, object UUID and kind are
  // the predecessor's; definition_version is the predecessor's plus one.
  core::catalog::CatalogMetadataVersion metadata;
};

struct CatalogVersionWriteResult {
  CatalogVersionWriteError error = CatalogVersionWriteError::invalid_request;
  NativeCatalogLeafError leaf_error = NativeCatalogLeafError::none;  // detail of invalid_catalog / invalid_successor
  page::TipPageError inventory_error = page::TipPageError::none;    // detail of invalid_inventory
  std::optional<core::platform::u64> blocker_order;                  // write_conflict
  core::platform::Uuid base_version_uuid;                            // the base found (update_conflict detail)
  core::platform::u64 page_number = 0;         // the page the successor was written to
  core::platform::u64 forward_link_page = 0;   // the predecessor's page (same as page_number for one write)
  core::platform::u64 row_version = 0;
  disk::CarefulWriteError write_error = disk::CarefulWriteError::none;
  disk::BoundedIoResult io{};  // the failing sink call, if any
  core::platform::u32 writes = 0, syncs = 0;
  // A write or sync was attempted and failed: the file holds a prefix of the
  // planned writes.
  bool attempted = false;
  // WriteCatalogInsertVersions: the request row a refusal concerns.
  std::optional<std::size_t> failed_row;
  bool ok() const noexcept { return error == CatalogVersionWriteError::none; }
};

CatalogVersionWriteResult WriteCatalogSuccessorVersion(disk::CarefulWriteSink& sink,
                                                       const CatalogVersionWriterBinding& binding,
                                                       const CatalogVersionRequest& request) noexcept;

// SEARCH_KEY: SB_CATALOG_INSERT_WRITER
// In-place writes, step W3b-1a. A transaction inserts the first versions of
// new catalog rows, in one or more relations, in place, through the same
// CarefulWriteSink and the same read of each relation as
// WriteCatalogSuccessorVersion (the transaction's state word decides
// visibility; nothing is published).
//
// Placement: each row goes on the first base leaf the relation's head already
// reaches that has room (an unindexed relation has exactly one, its head),
// rows of one request accumulating on the pages they share. An indexed
// relation's row also gets a cell in the index leaf whose key range holds its
// key (the 16 bytes of the row UUID, then row and version UUIDs, as the create
// keys them), at the key's position: with low-key separators an insert inside
// a leaf's range changes only that leaf. No page is added: relation_full when
// no reached base leaf has room, index_full when the index leaf the key reaches
// has none (new leaves and splits come with W3b-3).
//
// Order (owner Q23 = 1, what is pointed to before what points to it): every
// changed base leaf is written before every changed index leaf; each page is
// one whole-page sealed write whose image as read is the plan's preimage.
//
// A row is a first version: definition_version 1, created by this
// transaction, not deleted, not cluster-scoped, generations and epochs within
// the catalog root's. Its row and version UUIDs are new to the relation and to
// the request (duplicate_row); its object UUID is carried by no other row of
// the relation whose creator committed or is this transaction, nor by another
// row of the request (duplicate_object); a row carrying it whose creator is
// another transaction still in progress refuses write_conflict with that
// transaction's order (no waiting; if it rolls back the object can be
// inserted). A rolled-back or dead insert leaves its row and cell in place;
// readers return nothing for it, and sweep removes it.
//
// Relations (invalid_role), as the startup verifiers place records: the names
// relation (role 2) holds exactly the localized-name records; audit evidence
// is in role 6; database, schema and filespace records in role 1; inline
// private-key policies in role 3. A database record is never inserted.
//
// SEARCH_KEY: SB_CATALOG_NAME_INSERT (in-place writes W3b-1b-1; owner Q32 = 1:
// the native name writer is admitted). A localized-name row carries its name
// vector or entry in `name` and an empty metadata payload; the payload becomes
// the resident envelope (CATALOG_NAME_RESIDENT_ENVELOPE_V1), encoded once the
// row is placed, binding the page's database and filespace, the page, slot,
// storage generation, row version 1 and this transaction, as readers rebuild
// it. The name must agree with its metadata (CatalogNamePayloadMatchesMetadata).
// An entry's keys are checked as startup checks them: its dialect profile and
// that profile's identifier profile (role 5, active, as this transaction sees
// them) and CheckCatalogIdentifierLookupKeys; case-fold and quoted supplements
// are refused (invalid_name). Its full path (full_path_lookup_key,
// path_component_count) must be the one SB_NATIVE_NAME_PATH resolves from the
// names this transaction sees and the request's own (invalid_name). Collisions
// (SB_NAME_COLLISION_ORDER, the order
// startup verification applies): the entry's vector names a policy descriptor
// whose body binds the object's class to a group. Another entry of a different
// object in the same scope and identifier profile, under the same body and
// group, with lane order 0, holds the name if its newest version neither
// rolled back nor died is committed and active, or this transaction's own
// (duplicate_name); is another transaction's in progress (a create, rename or
// drop: write_conflict with its order); is committed dropped or archived (the
// name is free). Two entries of one class in one scope and identifier profile
// under different policy bodies refuse invalid_name (startup's
// conflicting_policy). The vector an entry names is found in the request or
// the names relation.
//
// Per request only: that a new object's vector and default entry exist and
// agree with its definition, and that a drop takes all three, are
// transaction-level rules owed by the caller (a transaction may write them in
// several requests).
//
// Not here: name successors (rename, drop: W3b-1b-2); the catalog-change audit
// event (W3b-2); new pages (W3b-3).
struct CatalogInsertRow {
  core::platform::u16 role = 0;  // relation role 1..6 in the catalog root
  core::platform::Uuid row_uuid;
  core::platform::Uuid version_uuid;  // a UUIDv7 the caller issued
  core::catalog::CatalogMetadataVersion metadata;
  std::optional<core::catalog::CatalogNamePayload> name;  // present iff a localized-name record
};
struct CatalogInsertRequest {
  core::platform::u64 transaction_order = 0;
  core::platform::Uuid transaction_uuid;
  std::vector<CatalogInsertRow> rows;
};
CatalogVersionWriteResult WriteCatalogInsertVersions(disk::CarefulWriteSink& sink,
                                                     const CatalogVersionWriterBinding& binding,
                                                     const CatalogInsertRequest& request) noexcept;

// SEARCH_KEY: SB_CATALOG_ROOT_ADVANCE (owner Q24 = 1, 2026-11-08). After a
// transaction that changed the catalog has committed (its state word says
// committed), the catalog root page is rewritten in place: catalog_generation
// becomes the next, and each epoch the caller names may rise, never fall.
// FirebirdSQL's post-commit deferred work, not part of the commit: a crash
// between COMMIT and this rewrite loses only the invalidation (the caches it
// serves live in the server process), because no committed version may carry
// a generation or epoch above the root's (WriteCatalogSuccessorVersion refuses
// ahead_of_catalog_root). The root keeps no history: no predecessor page and
// no predecessor digest are written (the checkpoint-era history link is drift,
// like the allocation map's, Q22 = 1). The page keeps its whole-page seal
// (Q23 = 1), and every byte the rewrite changes (generation and epochs, the
// empty predecessor, the seal) lies in the page's first 512-byte unit, so
// only that unit is written, its image as read the preimage: with
// sector-atomic writes a crash leaves the old root or the new one. A tear
// inside the unit fails the seal, and with it every catalog read, until
// repaired.
// The configuration and security route roots (page-zero kinds 6 and 7) repeat
// the generation and epochs; three pages cannot change at once, so the
// catalog root, which every reader uses, is written first and each route root
// after it, one unit each. A crash between them leaves route roots that lag
// the catalog root, which open verification admits; the next advance brings
// them level. A route root ahead of the catalog root is refused.
struct CatalogRootAdvanceRequest {
  // The committed transaction whose catalog changes this publishes.
  core::platform::u64 transaction_order = 0;
  core::platform::Uuid transaction_uuid;
  core::platform::u64 catalog_generation = 0;  // the root's current plus one
  core::platform::u64 schema_epoch = 0, security_epoch = 0, resource_epoch = 0;  // each at least the root's
};
struct CatalogRootAdvanceResult {
  CatalogVersionWriteError error = CatalogVersionWriteError::invalid_request;
  page::TipPageError inventory_error = page::TipPageError::none;
  core::platform::u64 previous_generation = 0;
  disk::CarefulWriteError write_error = disk::CarefulWriteError::none;
  disk::BoundedIoResult io{};
  core::platform::u32 writes = 0, syncs = 0;
  bool attempted = false;
  bool ok() const noexcept { return error == CatalogVersionWriteError::none; }
};
CatalogRootAdvanceResult AdvanceCatalogRoot(disk::CarefulWriteSink& sink, const CatalogVersionWriterBinding& binding,
                                            const CatalogRootAdvanceRequest& request) noexcept;

}  // namespace scratchbird::storage::database
