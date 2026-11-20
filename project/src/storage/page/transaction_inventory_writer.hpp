// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "careful_write.hpp"
#include "allocation_map_writer.hpp"
#include "node_uuid_issuer.hpp"
#include "filespace_page_zero.hpp"
#include "transaction_header_page.hpp"
#include "transaction_header_record.hpp"
#include "transaction_state_page.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace scratchbird::storage::page {

// SEARCH_KEY: SB_TRANSACTION_INVENTORY_WRITER
// Owner rulings 2026-11-05 (careful write, no intent records, nothing
// replayed) and 2026-11-07 (Q20 = 1: the in-place writes come before the
// checkpoint layer is deleted). BEGIN, COMMIT, ROLLBACK and restart
// dead-marking of the in-place transaction inventory: the transaction-state
// chain (SBTIPW01) and the transaction header chain (SBTXHD01) that page zero
// names (root kind 4), changed in place in the database file through a
// CarefulWriteSink, each write one 512-byte unit with a durability barrier
// before the write that depends on it (FirebirdSQL CCH_precedence).
//
// BEGIN of transaction n (n = the stored Next):
//   1. first state page: Next := n + 1 and the six horizons;   barrier
//   2. the header record of n, written once into its empty slot; barrier
//   3. the state word of n: active (or read-only active).        BEGIN returns
// A crash after 1 leaves an unused slot below Next and after 2 a header over
// an unused slot; restart marks either dead and the number is never reused.
// The word is written active directly, not starting then active as the
// inventory spec's appendix C allocation does: careful write has no two-phase
// publication for the starting state to protect, and an unused slot below
// Next is already read as a transaction that crashed while starting.
// Owner ruling Q21 = 1 (2026-11-07): active directly; the spec's starting
// phase is annotated SB_DRIFT_TIP_STARTING_BEFORE_ACTIVE.
//
// COMMIT of n: 1. the header's final state, commit time and completion time;
// barrier; 2. the state byte: committed, which is the visibility point. A
// crash between them leaves a committed header over an active word: dead at
// restart. ROLLBACK is the same with rolled_back.
//
// Restart (FirebirdSQL TRA_cleanup): the first write after a clean read-only
// open verification marks every transaction left in progress dead, through
// PlanTransactionStateRestart; the counters are not rewritten. It is a
// one-shot step of a writer: BEGIN, COMMIT and ROLLBACK are refused until it
// has run, and it is refused once it has run. Only one writer may exist for a
// database at a time, and the caller must create it when the database is
// opened: a restart run while transactions of a live process are in progress
// would mark them dead.
//
// Horizons at BEGIN: the projection open verification uses
// (ProjectValidatedLocalHorizons, no retained snapshots), over the inventory
// as it stands before n is allocated, gives OIT, OAT and OST, each at most n
// (the restart planner's writer rule). OAR and OLR have nothing to hold back
// yet except prepared or limbo transactions (OLR), so they are the new Next;
// OCT is min(OAT, OST, OLR, OAR) (inventory spec §12). A value below the one
// stored is refused as a horizon regression, with no write. COMMIT, ROLLBACK
// and restart never rewrite the horizons, so the stored values trail the
// projected ones until the next BEGIN; readers admit stored <= projected
// (StoredTransactionHorizonsAdmissible in transaction_inventory_reader.hpp).
//
// Commit times are strictly increasing: the commit time is the latest of the
// caller's clock, the transaction's begin time (its UUIDv7 prefix) and one
// nanosecond after the latest commit time any header holds. The reader orders
// commit sequences by commit time, so a clock that steps back or two commits
// in one nanosecond must not reorder them.
//
// CompleteAndBegin (plan 36 T1, owner Q150 = 1, Q305/Q308 2026-11-19): the
// commit or rollback of transaction n and the start of its replacement m, as
// ONE careful-write plan, m = the stored Next. All refusals come before any
// write (chain growth for m's slots, when needed, is its own earlier plan, as
// for BEGIN). The writes, in the order of plan 36:
//   W1 first state page: Next := m + 1 and the six horizons;
//   W2 the header record of m (all identities as the caller gives them);
//   W3 the state word of m: active (or read-only active);
//   W4 the header of n: final state, commit time, completion time;
//   W5 the state byte of n: committed or rolled back (the visibility point).
// A barrier follows every node that has a successor. Edits that land in the
// same 512-byte write unit are MERGED into one node (one write per offset, no
// overlapping extents), always on ONE page image:
//   - W2 and W4 share a unit when n's header slot is even (two 256-byte
//     records per unit and an even slot count at every page size: n and m =
//     n + 1 are the two records of one unit; at the end of a header page m is
//     on the next page and they do not share). The merged node sits at W2's
//     place: W4 only has to precede W5, so it may come early.
//   - W3 and W5 share a unit unless n's word is the last of a 128-word unit
//     or the last of its page. The merged node sits at W5's place, after W4.
// Plans, in write order (syncs on a sink that is not forced-ordered; one
// final sync only on a forced one): no merge W1 W2 W3 W4 W5 (5 syncs); headers
// W1 [W2 W4] W3 W5 (4); words W1 W2 W4 [W3 W5] (4); both W1 [W2 W4] [W3 W5]
// (3). BEGIN then COMMIT today is 3 + 2 = 5.
// Crash table (every row is a file a reopen's Restart resolves): after W1 n is
// still active and m is an unused slot below Next; after W2 a header of m sits
// over an unused word; after W3 m is active and n is unchanged; after W4 n's
// header says final over an active word; after W5 n is final and m is active.
// Restart marks every one of those in-progress or unused slots dead: n is dead
// before W5 (it never committed), m is always dead (the server that began it
// crashed), and no number is reused. A merged node has only its before and
// after.
// Horizons are computed over the inventory as it stands before the plan, with
// n still active, exactly as BEGIN does: stored horizons may not exceed what
// the file projects (StoredTransactionHorizonsAdmissible), and until W5 the
// file still shows n active. Computing them with n final (the first reading
// of plan 36) makes the file after W1..W4 inadmissible to the reader, so
// open verification would refuse the database after such a crash (measured by
// this slice's tear sweep, see the T1 log). The stored OAT/OST therefore
// trail by the completing transaction, which is the same trailing COMMIT
// leaves today until the next BEGIN.
// m's visibility snapshot is the caller's: taken in memory from the file after
// this call returns (it sees n as final); the header's start-snapshot
// identity (+144) is whatever the caller gives, plan 36 gives none.
// PLAN36_RETAINING: COMMIT/ROLLBACK RETAINING (owner Q169 = BOTH, Firebird
// retain_context) will attach here: the new number keeps the old snapshot and
// the list of earlier numbers whose committed work it sees, and the horizon
// projection below passes that retained snapshot instead of none. NOT
// implemented; a plain call is the fresh-snapshot replacement (Q168/Q169).
// The file is authoritative: every call reads both chains through the sink
// and checks them (CheckTransactionInventoryImages) before it plans a write;
// no transaction state is kept in memory. A write or sync failure leaves the
// file in one of the crash states above and the writer refusing every later
// call (writer_failed): the database must be closed and opened again, and the
// next writer's restart resolves the file. A refusal before any write or sync
// changes nothing and leaves the writer usable.
//
// Chain growth (in-place writes W2c, owner Q22 = 1): when n has no formatted
// slot in the state chain or the header chain, and the binding names the
// allocation map and an identity issuer, BEGIN first grows that chain in its
// own careful-write plan: the map's record and state for a new page (created
// by the system: no transaction), the new page formatted empty, then the link
// in the chain's last page, each durable before the next. BEGIN then re-reads
// the chains and makes its usual three writes. Growth is never merged into
// BEGIN's plan: the new page's image and BEGIN's header record would share a
// page, and the record must not be durable before Next. A crash before the
// link leaves an allocated page nothing reaches (wasted space, owner Q17 = 1);
// after it, a chain one page longer than Next needs, which readers admit and
// the next BEGIN uses. Without the map binding, or with no free page, BEGIN
// past the last formatted slot is refused chain_full with no write.
// Not covered yet (later steps): read-only databases get no transactions at
// all here (whether they get durable headers is deferred, they cannot write
// them); cluster-authority transactions belong to the cluster library.
// Not thread-safe: the owner serializes calls and holds whatever device guard
// its sink needs across each call.

enum class TransactionWriterError : std::uint8_t {
  none,
  invalid_request,      // bad binding or argument; nothing read or written
  read_only_database,   // the database state carries the read-only flag
  restart_required,     // BEGIN/COMMIT/ROLLBACK before the restart step
  restart_already_run,  // the restart step was already run on this writer
  writer_failed,        // an earlier write or sync failed; reopen the database
  io_failure,           // a read failed; nothing written
  invalid_inventory,    // the chains on disk fail the inventory check, or changed underneath
  chain_full,           // no formatted slot left for the next transaction
  horizon_regression,   // a computed horizon is below the stored one
  unknown_transaction,  // no transaction with that order
  invalid_state,        // the transaction is not in progress
  invalid_header,       // the header record was refused by its codec
  write_failed,         // a write or sync failed; the writer is now failed
  resource_exhausted,
  // S3d (plan 38, owner Q241 = 1): Restart found committing / recovering slots
  // and the binding asked to refuse then. Nothing was written, the writer is
  // not failed and has not restarted; result.unresolved lists the orders.
  unresolved_transactions,
};
const char* TransactionWriterErrorName(TransactionWriterError error) noexcept;

struct TransactionWriterBinding {
  scratchbird::core::platform::Uuid database_uuid;
  // Page-zero root kind 4: the first transaction-state page.
  scratchbird::storage::disk::FilespaceRootReference state_root;
  // The SBSYS002 read-only flag, from the caller's verified open.
  bool read_only_database = false;
  scratchbird::storage::disk::CarefulWriteBarrierPolicy barrier_policy =
      scratchbird::storage::disk::CarefulWriteBarrierPolicy::trust_forced_writes;
  // Bound on either chain's length while it is read.
  std::size_t maximum_chain_pages = 4096;
  // Chain growth: page-zero root kind 3 (the allocation map) and the issuer
  // of the new pages' identities. Both absent: no growth (chain_full).
  std::optional<scratchbird::storage::disk::FilespaceRootReference> allocation_root;
  scratchbird::core::uuid::StandaloneUuidV7Issuer* issuer = nullptr;
  // SB_TRANSACTION_RESTART_REFUSE_UNRESOLVED (S3d, owner Q241 = 1, 2026-11-16):
  // Restart refuses, before any write, when a slot is committing or recovering
  // (no current writer stores them; the outcome needs the header, which the
  // TIP-only restart does not read). The caller opens the database restricted
  // and reports the orders. False (the default): Restart marks the dead slots
  // and reports the unresolved ones as it always did (existing writer tests).
  bool refuse_restart_with_unresolved = false;
};

struct TransactionWriterOutcome {
  TransactionWriterError error = TransactionWriterError::invalid_request;
  TipPageError page_error = TipPageError::none;         // detail of invalid_inventory / chain_full
  TransactionHeaderError header_error = TransactionHeaderError::none;  // detail of invalid_header
  std::optional<u64> transaction_order;                 // the transaction written or refused
  scratchbird::storage::disk::CarefulWriteError write_error = scratchbird::storage::disk::CarefulWriteError::none;
  scratchbird::storage::disk::BoundedIoResult io{};    // the failing sink call, if any
  u32 writes = 0;  // sink writes that succeeded
  u32 syncs = 0;   // sink syncs issued, including a failed one
  bool ok() const noexcept { return error == TransactionWriterError::none; }
};

struct TransactionRestartResult : TransactionWriterOutcome {
  u64 next_transaction = 0;
  u64 scanned_from = 0;
  u64 marked_dead = 0;
  std::vector<u64> unresolved;   // committing / recovering slots, left as they are
  bool read_only_skipped = false;  // read-only database: nothing written
};

struct TransactionBeginResult : TransactionWriterOutcome {
  TipCounters counters;  // as written in step 1
  // Chain growth before BEGIN's own writes (writes and syncs above are
  // BEGIN's own): pages added to the state and header chains, and the
  // growth plans' writes and syncs.
  u32 grown_state_pages = 0, grown_header_pages = 0;
  u32 growth_writes = 0, growth_syncs = 0;
  AllocationWriteError allocation_error = AllocationWriteError::none;  // detail of a growth refusal
};

struct TransactionCompleteResult : TransactionWriterOutcome {
  TipStateCode final_state = TipStateCode::unused;
  u64 completed_at_utc_ns = 0;
  // A failure at or after the state-byte write: the outcome is decided by the
  // file when it is next opened (committed or rolled back if the byte reached
  // the file, dead otherwise). False for every other failure: the transaction
  // is still in progress and becomes dead at restart.
  bool outcome_uncertain = false;
};

// CompleteAndBegin: transaction_order is n (the transaction completed or
// refused), replacement_order and replacement_uuid are m. writes/syncs are the
// one plan's own; growth_* are chain growth before it.
struct TransactionCompleteAndBeginResult : TransactionBeginResult {
  TipStateCode final_state = TipStateCode::unused;
  u64 completed_at_utc_ns = 0;
  u64 replacement_order = 0;
  scratchbird::core::platform::Uuid replacement_uuid;
  // A failure at or after the write that holds n's state byte: n's outcome is
  // decided by the file when it is next opened (final if the byte reached it,
  // dead otherwise); m is dead either way. False for every other failure: n is
  // still in progress and becomes dead at restart.
  bool outcome_uncertain = false;
  // Which merges the plan made (see the design comment above).
  bool merged_headers = false, merged_state_words = false;
};

class TransactionInventoryWriter {
 public:
  explicit TransactionInventoryWriter(const TransactionWriterBinding& binding) noexcept;
  TransactionInventoryWriter(const TransactionInventoryWriter&) = delete;
  TransactionInventoryWriter& operator=(const TransactionInventoryWriter&) = delete;

  // The one-shot restart step (see above). A read-only database is checked
  // and nothing is written.
  TransactionRestartResult Restart(scratchbird::storage::disk::CarefulWriteSink& sink) noexcept;
  // `header` carries the transaction's identities: a UUIDv7 transaction id,
  // local authority, read_only, isolation profile and intent, and the
  // optional identities. The writer sets transaction_order and the database
  // id; the completion fields and the intent generation must be zero.
  TransactionBeginResult Begin(scratchbird::storage::disk::CarefulWriteSink& sink,
                               const TransactionHeaderRecord& header) noexcept;
  // now_utc_ns: the caller's UTC clock in nanoseconds (non-zero).
  TransactionCompleteResult Commit(scratchbird::storage::disk::CarefulWriteSink& sink, u64 transaction_order,
                                   u64 now_utc_ns) noexcept;
  TransactionCompleteResult Rollback(scratchbird::storage::disk::CarefulWriteSink& sink, u64 transaction_order,
                                     u64 now_utc_ns) noexcept;

  // Commits (final_state = committed) or rolls back (rolled_back) transaction n
  // and begins m = the stored Next in one plan; see the design comment.
  // `header_m` is as for Begin. now_utc_ns as for Commit. Any other
  // final_state is invalid_request.
  TransactionCompleteAndBeginResult CompleteAndBegin(scratchbird::storage::disk::CarefulWriteSink& sink,
                                                     u64 transaction_order, TipStateCode final_state,
                                                     const TransactionHeaderRecord& header_m,
                                                     u64 now_utc_ns) noexcept;

  bool restarted() const noexcept { return restarted_; }
  bool failed() const noexcept { return failed_; }
  u32 page_size() const noexcept { return page_size_; }

 private:
  TransactionCompleteResult Complete(scratchbird::storage::disk::CarefulWriteSink& sink, u64 transaction_order,
                                     u64 now_utc_ns, TipStateCode final_state) noexcept;
  TransactionWriterBinding binding_;
  u32 page_size_ = 0;
  bool restarted_ = false;
  bool failed_ = false;
};

// The in-place inventory as this writer reads it, for the other in-place
// writers (W3, the catalog version writer): both chains read through the
// sink, each page decoded and bound (the first state page to the root), and
// the whole checked by CheckTransactionInventoryImages. The binding is checked
// as the writer's constructor checks it. Nothing is written.
struct TransactionChainsRead : TransactionWriterOutcome {
  u32 page_size = 0;
  std::vector<byte> state_images, header_images;  // each chain, page after page
  std::vector<u64> state_pages, header_pages;
  TipCounters counters;
};
TransactionChainsRead ReadTransactionChainsThroughSink(scratchbird::storage::disk::CarefulWriteSink& sink,
                                                       const TransactionWriterBinding& binding) noexcept;

}  // namespace scratchbird::storage::page
