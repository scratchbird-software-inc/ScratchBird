// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "careful_write.hpp"
#include "filespace_page_zero.hpp"
#include "node_uuid_issuer.hpp"
#include "session_ledger_page.hpp"
#include "transaction_inventory_writer.hpp"

#include <map>
#include <optional>
#include <vector>

namespace scratchbird::storage::page {

// SEARCH_KEY: SB_SESSION_LEDGER_CHAIN
// Plan 36 T3/T4 (owner Q165 = 1; Q308: built before S4e). The session ledger
// chain of SBSESL01 pages named by page-zero root kind 25
// (SB_SESSION_LEDGER_ROOT), and its writer. This class is the ledger as the
// database loop uses it: one object per open, thread-affine like the
// TransactionInventoryWriter (the owner serializes calls and holds the device
// guard its sink needs across each call; not thread-safe).
//
// APPEND. One record = one careful-write plan of ONE 512-byte write unit into
// the first slot that is not a complete record (see session_ledger_page.hpp),
// then a durability barrier (one write, one sync). When the last page is full
// the chain first grows by one page in its own plan (the shared W2c growth
// of careful_chain_support.hpp: the allocation record, the new page formatted
// empty, the link, each durable before the next), then the record is written.
// The record's ordinal is the next ordinal; the caller never chooses it. The
// IN-MEMORY INDEX is updated only after the write and its sync returned
// success, so a record is never found (and so never citable) before it is
// durable. A write or sync failure leaves the ledger in a crash state the next
// open resolves (a torn last record is ignored) and the ledger refusing every
// later call (writer_failed): the database must be reopened.
//
// REFERENCES (checked on every append and on every load, one code path): an
// attachment names a complete session (not ended) and a complete connection
// and, when it has a predecessor, a complete attachment of the SAME session; a
// settings change names a complete, not ended session and is that session's
// next generation (1, 2, 3, ...); a record UUID is used once, except that a
// session's END record repeats the open record's UUID and principal.
//
// INDEX and its memory cost. Load reads the chain once, page after page (one
// page image of scratch), and keeps one 72-byte entry per record UUID in a
// std::map (about 90 bytes with the key and the tree node): 90 B x records. It
// is bounded by `maximum_records` (default 1,048,576 = about 95 MB worst case,
// reached only by a ledger of a million records; the entry does not depend on
// the record size, so the 512-byte record (owner 2026-11-23) leaves this bound
// unchanged while the records at that bound occupy 512 MiB of file, twice the
// 256 MiB of the 256-byte record; the load scratch is one page; a typical ledger is a few
// thousand records = under 1 MB). Append past the bound is refused ledger_full
// with nothing written. Only the last page is read again per append (one page
// read; the plan's preimage check then compares the write unit with the disk).
//
// CITATION CHECK (CheckHeaderCitation, used by the transaction writer before
// it writes anything): a header's +80 attachment and +96 session must already
// be COMPLETE records of the right kind; the cited session's principal must be
// the header's +64; when both are cited the attachment must belong to that
// session; an ended session is refused.

struct SessionLedgerBinding {
  Uuid database_uuid;
  // Page-zero root kind 25: the first ledger page.
  scratchbird::storage::disk::FilespaceRootReference root;
  // The SBSYS002 read-only flag, from the caller's verified open.
  bool read_only_database = false;
  scratchbird::storage::disk::CarefulWriteBarrierPolicy barrier_policy =
      scratchbird::storage::disk::CarefulWriteBarrierPolicy::trust_forced_writes;
  // Bounds: the chain's length and the index's record count.
  std::size_t maximum_chain_pages = 4096;
  std::size_t maximum_records = std::size_t{1} << 20;
  // Chain growth: page-zero root kind 3 (the allocation map) and the issuer of
  // the new pages' identities. Both absent: no growth (chain_full).
  std::optional<scratchbird::storage::disk::FilespaceRootReference> allocation_root;
  scratchbird::core::uuid::StandaloneUuidV7Issuer* issuer = nullptr;
};

// What the index keeps of a record (not the whole record).
struct SessionLedgerEntry {
  u64 ordinal = 0;               // the record's ordinal (a session: its latest record's)
  u64 settings_generation = 0;   // a session: the latest settings-change generation (0 = none)
  Uuid ref;                      // a session: its principal; an attachment or settings change: its session
  SessionLedgerKind kind = SessionLedgerKind::connection;
  bool ended = false;            // a session with an end record
};

struct SessionLedgerLoadResult : TransactionWriterOutcome {
  u64 records = 0;    // complete records
  u64 pages = 0;      // pages in the chain
  bool torn_tail = false;  // one torn slot after the last complete record (ignored)
  u64 failing_ordinal = 0;  // ledger_invalid: the record or slot at fault
  u64 failing_page = 0;
  TipPageError page_error = TipPageError::none;  // ledger_invalid: the page-level cause
  SessionLedgerRecordError record_error = SessionLedgerRecordError::none;
  TransactionWriterError rule = TransactionWriterError::none;  // ledger_invalid: the reference rule broken
};

struct SessionLedgerAppendResult : TransactionBeginResult {
  u64 ordinal = 0;  // the record's ordinal when written (or refused at)
  SessionLedgerRecordError record_error = SessionLedgerRecordError::none;
};

class SessionLedger {
 public:
  explicit SessionLedger(const SessionLedgerBinding& binding) noexcept;
  SessionLedger(const SessionLedger&) = delete;
  SessionLedger& operator=(const SessionLedger&) = delete;

  // Reads and checks the whole chain through the sink and builds the index.
  // Writes nothing (a torn tail is only noted). Call once at open, before any
  // Append, on the thread that will append. May be called again to rebuild.
  SessionLedgerLoadResult Load(scratchbird::storage::disk::CarefulWriteSink& sink) noexcept;
  // Appends `record` (database_uuid and ordinal are set here). See above.
  SessionLedgerAppendResult Append(scratchbird::storage::disk::CarefulWriteSink& sink,
                                   const SessionLedgerRecord& record) noexcept;

  // The current entry of a record UUID, or nullptr. Only complete, durable
  // records are indexed.
  const SessionLedgerEntry* Find(const Uuid& record_uuid) const noexcept;
  // The "durable before cited" rule; none, or the refusal code. A nil
  // attachment / session is not a citation (the caller decides whether a zero
  // is allowed); `principal` is the header's +64.
  // S4e: `allow_ended_session` (false for Begin / CompleteAndBegin: a NEW header never cites an ended session; true for
  // the OPEN verification of existing headers: the header of a transaction that was rolled back when its session ended
  // still cites that session, and a file closed gracefully must open). Every other rule is the same in both modes.
  TransactionWriterError CheckHeaderCitation(const Uuid& attachment, const Uuid& session, const Uuid& principal,
                                             bool allow_ended_session = false) const noexcept;

  bool loaded() const noexcept { return loaded_; }
  bool failed() const noexcept { return failed_; }
  u32 page_size() const noexcept { return page_size_; }
  u64 record_count() const noexcept { return records_; }
  u64 page_count() const noexcept { return pages_.size(); }
  // S4e: sessions whose latest record is not an end record (a session still open in this process, or, right after
  // Load, one an earlier engine instance never ended: a crash or kill left it, "orphaned"). One pass over the index.
  u64 OpenSessionCount() const noexcept {
    u64 n = 0;
    for (const auto& [uuid, entry] : index_) {
      (void)uuid;
      if (entry.kind == SessionLedgerKind::session && !entry.ended) ++n;
    }
    return n;
  }
  const std::vector<u64>& pages() const noexcept { return pages_; }  // page numbers, chain order
  // 90 bytes per indexed record UUID (see above); an estimate for reports.
  static constexpr std::size_t kIndexBytesPerRecord = 90;
  std::size_t index_bytes_estimate() const noexcept { return index_.size() * kIndexBytesPerRecord; }

 private:
  // Applies a record to an index (Load and Append share it): the reference
  // rules. Returns none or the rule's code. `index` is changed only on none.
  TransactionWriterError Apply(std::map<Uuid, SessionLedgerEntry>& index, const SessionLedgerRecord& record) const noexcept;
  SessionLedgerBinding binding_;
  u32 page_size_ = 0;
  bool loaded_ = false;
  bool failed_ = false;
  u64 records_ = 0;
  std::vector<u64> pages_;  // page numbers, chain order
  std::map<Uuid, SessionLedgerEntry> index_;
};

}  // namespace scratchbird::storage::page
