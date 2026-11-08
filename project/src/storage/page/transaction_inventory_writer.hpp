// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "careful_write.hpp"
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
// The file is authoritative: every call reads both chains through the sink
// and checks them (CheckTransactionInventoryImages) before it plans a write;
// no transaction state is kept in memory. A write or sync failure leaves the
// file in one of the crash states above and the writer refusing every later
// call (writer_failed): the database must be closed and opened again, and the
// next writer's restart resolves the file. A refusal before any write or sync
// changes nothing and leaves the writer usable.
//
// Not covered yet (later steps): read-only databases get no transactions at
// all here (whether they get durable headers is deferred, they cannot write
// them); growing either chain when it is full is allocation from the map (W2);
// cluster-authority transactions belong to the cluster library.
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

}  // namespace scratchbird::storage::page
