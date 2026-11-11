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
#include <utility>
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
  name_record_unsupported,  // no longer produced since name successors (N4a); kept so values do not shift
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
  name_tree_full,          // insert: the name tree leaf a name row's cell reaches has no room (splits: W3b-3)
  // SB_CATALOG_NEW_PAGES (W3b-3a, owner Q30 = 1): a new page was needed and
  // the request carries no further caller-issued identity for one.
  new_pages_exhausted,
  filespace_full,          // a new page was needed and the allocation map has no free slot
  // SB_NATIVE_NAME_CASCADE (N4b): a name change moves other entries' paths
  // and the request carries too few cascade_versions identities for them
  // (result.cascade_needed says how many).
  cascade_versions_exhausted,
  // SB_AUDIT_CATALOG_METADATA_CHANGE (W3b-2b): a definition (system-table) row
  // version that does not cite a catalog.metadata_change event of its own
  // transaction (in the same request, or already written by that transaction).
  unaudited_change,
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
  // SB_CATALOG_NEW_PAGES (W3b-3a): page-zero root kind 3, the allocation map,
  // from which a write that needs a new page allocates it. Absent: no new
  // pages (a write that needs one is refused relation_full / index_full).
  std::optional<disk::FilespaceRootReference> allocation_root;
  // Bounds on what one write reads.
  std::size_t maximum_chain_pages = 4096;
  std::size_t maximum_index_pages = 4096;
  std::size_t maximum_base_pages = 4096;
};

// SB_CATALOG_NEW_PAGES (W3b-3a, owner Q30 = 1). The caller-issued identities
// (UUIDv7) of pages a write may add, used in order; unused ones are not
// consumed. A page a write adds is the system's: its allocation record has no
// creator, and so does a new tree page; a new base leaf carries no creator by
// its format. Each is allocated, durably, before the write's own plan, and
// written before anything points to it. Allocation happens only after every
// check a refusal can come from has passed; a crash, or a failure, after an
// allocation and before the write leaves an allocated page nothing reaches:
// wasted space (owner Q17 = 1), never a page something reaches unwritten.
struct CatalogNewPageIdentity {
  core::platform::Uuid page_uuid, allocation_uuid;
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
  // SB_NATIVE_NAME_TREES (N4a): present iff the record is a localized name.
  // The payload preserves the base version's identity
  // (CatalogNamePayloadPreservesIdentity); a deleted version carries lifecycle
  // dropped and, for an entry, the retiring transaction; an entry's keys, path
  // and collisions are checked as an insert's are. Its envelope is encoded
  // where the version is placed, like an insert's. The base version's cells in
  // the name trees are stamped with this transaction (owner Q42) and a version
  // that is not a drop gets its own cells, in the same careful-write plan. A
  // name whose entry other names' paths run through cannot yet be renamed or
  // dropped (invalid_successor; the cascade is N4b).
  // N4b (SB_NATIVE_NAME_CASCADE) supersedes that last sentence: see
  // cascade_versions.
  std::optional<core::catalog::CatalogNamePayload> name;
  // SEARCH_KEY: SB_NATIVE_NAME_CASCADE (N4b). A name change (rename, drop,
  // a default_for_language or vector default change) moves the path of every
  // live entry whose path runs through the changed name. Each such entry gets
  // a successor version in the same write, by this transaction, with its
  // regenerated full_path_lookup_key, path_component_count and UUID-to-name
  // display components and nothing else changed; these are its identities
  // (UUIDv7, caller-issued), used in row-UUID order of the entries. Too few is
  // cascade_versions_exhausted, decided before any write; an entry the change
  // would leave without a path (an unnamed or ambiguous scope) is
  // invalid_successor; one whose newest version is another transaction's in
  // progress is a write_conflict with that blocker.
  // SB_NATIVE_NAME_PATH_CONFLICT (owner Q47 = 1, 2026-11-11): a path is
  // resolved over the names this transaction sees (a name another
  // transaction is changing is seen as its committed version). Writing an
  // entry (this one, or a cascaded one) whose path makes a lookup that a name
  // row another transaction is changing decides (that object's vector, its
  // entry marked default_for_language in the path's language, or the entry its
  // vector names as default, old or new version) is a write_conflict with that
  // transaction: the path would go stale whichever way it ends. Another
  // transaction's entry in progress whose path does not resolve in this view
  // conflicts when its path's lookups reach a name this request changes.
  std::vector<core::platform::Uuid> cascade_versions;
  // SB_CATALOG_NEW_PAGES: when no reached base leaf has room, the version goes
  // on a new base leaf, reached by a cell for this version in the relation's
  // row-UUID tree (which otherwise indexes a row's first version only).
  std::vector<CatalogNewPageIdentity> new_pages;
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
  // WriteCatalogInsertVersions: the request row a refusal concerns (unset for
  // a refusal of the insert's cascade, which concerns the request as a whole).
  std::optional<std::size_t> failed_row;
  // SB_CATALOG_NEW_PAGES: the pages allocated for this write, in the order of
  // the request's new_pages identities they took. Set also when the write
  // then failed: those pages are allocated and unreached (wasted, Q17 = 1).
  std::vector<core::platform::u64> allocated_pages;
  // SB_NATIVE_NAME_CASCADE (N4b): the entries' versions the cascade wrote
  // (row, version), in row-UUID order; on cascade_versions_exhausted, how
  // many identities it needs.
  std::vector<std::pair<core::platform::Uuid, core::platform::Uuid>> cascaded;
  std::size_t cascade_needed = 0;
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
// are refused (invalid_name). SB_NATIVE_NAME_TREES: when page zero carries the
// name trees (kinds 22 and 23), every inserted name row gets its cells in the
// same careful-write plan: one in the authoritative tree per entry, one in the
// UUID-to-name tree per name row; a leaf without room is name_tree_full
// (splits come with W3b-3). Its full path (full_path_lookup_key,
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
// (Since: successors and new pages are written; see WriteCatalogSuccessorVersion
// and SB_CATALOG_NEW_PAGES. The audit event is still W3b-2.)
//
// SB_NATIVE_NAME_CASCADE (owner Q48 = 1, 2026-11-11): the request's names may
// change which name a stored entry's path uses (an entry marked
// default_for_language for an existing object, in a language whose entries
// fell back to its vector's default). Each stored entry they move gets a
// successor version in the same write, exactly as a successor's cascade
// (CatalogVersionRequest::cascade_versions), from this request's
// cascade_versions; too few is cascade_versions_exhausted with
// result.cascade_needed; an entry left without a path (a second default in one
// language makes its scope ambiguous) is invalid_name. Q47's write_conflict
// applies to each inserted entry's path and each cascaded one.
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
  // SB_NATIVE_NAME_CASCADE (Q48 = 1): identities for the cascaded versions
  // (UUIDv7, caller-issued), used in row-UUID order of the entries moved.
  std::vector<core::platform::Uuid> cascade_versions;
  // SB_CATALOG_NEW_PAGES: when no reached base leaf of an indexed relation has
  // room for a row, it goes on a new base leaf.
  std::vector<CatalogNewPageIdentity> new_pages;
};
CatalogVersionWriteResult WriteCatalogInsertVersions(disk::CarefulWriteSink& sink,
                                                     const CatalogVersionWriterBinding& binding,
                                                     const CatalogInsertRequest& request) noexcept;

// SEARCH_KEY: SB_AUDIT_CATALOG_METADATA_CHANGE (W3b-2b). Every definition
// (system-table) row version a transaction writes, an insert's or a
// successor's, a drop included, cites (audit_uuid) a catalog.metadata_change
// event that transaction wrote: in the same request, or stored with that
// transaction as creator. Otherwise unaudited_change, before any write. Name
// rows (role 2) are data (owner, Q50) and are not bound; the event row is its
// own anchor; an event is immutable: a successor of an audit_evidence row is
// refused (invalid_successor). Writer enforcement is the implementing AI's
// reading of Q50, announced to the owner without objection (2026-11-11).
//
// WriteCatalogChangeEvent writes a transaction's event: one row in role 6
// through the insert path, its metadata the create's construction intent row's
// owner, owning schema and parent, its catalog generation the catalog root's;
// the identities, engine, event time (from the time service, never derived)
// and actor are the caller's. Q49 = 1: policy decision not_evaluated, no policy
// generation.
struct CatalogChangeEventRequest {
  core::platform::u64 transaction_order = 0;
  core::platform::Uuid transaction_uuid;
  core::platform::Uuid event_uuid, row_uuid, version_uuid, engine_uuid;
  core::platform::u64 event_time_utc_us = 0;
  std::optional<core::platform::TypedUuid> actor_uuid;
  std::vector<CatalogNewPageIdentity> new_pages;
};
CatalogVersionWriteResult WriteCatalogChangeEvent(disk::CarefulWriteSink& sink, const CatalogVersionWriterBinding& binding,
                                                  const CatalogChangeEventRequest& request) noexcept;

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
