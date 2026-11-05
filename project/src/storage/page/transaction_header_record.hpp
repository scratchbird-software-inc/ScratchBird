// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "transaction_state_page.hpp"
#include "uuid.hpp"

#include <array>
#include <optional>
#include <span>

namespace scratchbird::storage::page {

using scratchbird::core::platform::Uuid;

// SEARCH_KEY: SB_TRANSACTION_HEADER_RECORD
// Owner direction 2026-11-05: there are no simple transactions. Every
// transaction has a durable header: the database's ID, the transaction's
// UUIDv7 (which carries its creation time), the commit time (UTC, the moment
// it became visible) and the transaction_order (the order transactions were
// started). Rows carry only the transaction ID and their row ID; the header is
// what traces a transaction to its stop time, and it travels with the
// transaction when it, or a subset of its changes, migrates to another shard.
// The remaining fields are the lifecycle spec's §4 identity fields and the
// inventory spec's §8 detail-record fields.
//
// Required identities: the database UUID and the transaction UUID (v7). The
// lifecycle spec §4 also requires the engine instance, principal or agent,
// and policy, security and start snapshot UUIDs; the subsystems that issue
// those are not bound to transactions yet, so the codec accepts zero for them
// (meaning not yet bound) rather than force a writer to invent values. Every
// non-zero UUID must be a v7 engine identity.
//
// This is the record codec only. Where headers are stored (fixed slot pages
// or a system table) is undecided until both are measured (owner,
// 2026-11-05), so the record is a fixed 256 bytes that either store can hold,
// two to a 512-byte write unit.
//
// Write order (careful write): BEGIN allocates Next, then writes the header
// once, then the state word that points to it. Commit writes the header's
// commit time and final state, then the state byte, which is the visibility
// point. A header that says committed while the state word says dead is the
// crash window between those two writes and is legal; the state word is the
// local authority.
//
// Layout (little-endian):
//   +0   u16 version (1)          +2   u16 record bytes (256)
//   +4   u8  authority class      +5   u8  intent (mutable, < 64)
//   +6   u8  isolation profile    +7   u8  flags (bit 0 read-only; others 0)
//   +8   u64 transaction_order (never 0)
//   +16  database UUID            +32  transaction UUID (v7)
//   +48  engine instance UUID     +64  principal or internal agent UUID
//   +80  attachment UUID (0 = none)          +96  session UUID (0 = none)
//   +112 policy snapshot UUID     +128 security snapshot UUID
//   +144 start snapshot UUID
//   +160 durable operation UUID (0 = none)   +176 recovery attempt UUID (0 = none)
//   +192 cluster UUID (0 = none)             +208 cluster transaction UUID (0 = none)
//   +224 u64 cluster epoch (0 unless cluster-scoped)
//   +232 u32 intent generation (mutable)
//   +236 u8  final state (mutable; 0 = not complete, else committed,
//            rolled_back or dead as TipStateCode)   +237..239 zero
//   +240 u64 commit time, UTC ns since the Unix epoch (mutable; 0 unless committed)
//   +248 u64 completed at, UTC ns (mutable; 0 until complete; equals the
//            commit time on commit)

inline constexpr std::size_t kTransactionHeaderRecordBytes = 256;
inline constexpr u16 kTransactionHeaderRecordVersion = 1;
inline constexpr std::uint8_t kTransactionHeaderFlagReadOnly = 1u << 0;

struct TransactionHeaderRecord {
  TipAuthorityClass authority = TipAuthorityClass::local;
  std::uint8_t intent = 0;
  std::uint8_t isolation_profile = 0;
  bool read_only = false;
  u64 transaction_order = 0;
  Uuid database_uuid;
  Uuid transaction_uuid;
  Uuid engine_instance_uuid;
  Uuid principal_or_agent_uuid;
  Uuid attachment_uuid;
  Uuid session_uuid;
  Uuid policy_snapshot_uuid;
  Uuid security_snapshot_uuid;
  Uuid start_snapshot_uuid;
  Uuid durable_operation_uuid;
  Uuid recovery_attempt_uuid;
  Uuid cluster_uuid;
  Uuid cluster_transaction_uuid;
  u64 cluster_epoch = 0;
  u32 intent_generation = 0;
  TipStateCode final_state = TipStateCode::unused;
  u64 commit_time_utc_ns = 0;
  u64 completed_at_utc_ns = 0;
  bool operator==(const TransactionHeaderRecord&) const = default;
};

enum class TransactionHeaderError : std::uint8_t {
  none,
  invalid_framing,
  invalid_order,
  invalid_identity,
  invalid_optional_identity,
  invalid_cluster,
  invalid_class,
  invalid_completion,
  already_complete,
  short_buffer,
};
const char* TransactionHeaderErrorName(TransactionHeaderError error) noexcept;

// Checks a record's values without encoding it.
TransactionHeaderError ValidateTransactionHeaderRecord(const TransactionHeaderRecord& record) noexcept;
// Writes exactly kTransactionHeaderRecordBytes; output is untouched on a refusal.
TransactionHeaderError EncodeTransactionHeaderRecord(const TransactionHeaderRecord& record,
                                                     std::span<byte> output) noexcept;
struct TransactionHeaderDecodeResult {
  TransactionHeaderError error = TransactionHeaderError::invalid_framing;
  std::optional<TransactionHeaderRecord> record;
  bool ok() const noexcept { return error == TransactionHeaderError::none && record.has_value(); }
};
TransactionHeaderDecodeResult DecodeTransactionHeaderRecord(std::span<const byte> bytes) noexcept;

// Pure transforms of the mutable fields. Completion happens once: committed
// sets the commit time and the completion time to the same value; rolled_back
// and dead set only the completion time. An intent change raises the intent
// generation by one and is refused once the transaction is complete. Whether
// the new intent is an upgrade (lifecycle spec §5: no downgrade) is the
// transaction layer's rule; the codec does not order intents.
TransactionHeaderError CompleteTransactionHeader(TransactionHeaderRecord& record, TipStateCode final_state,
                                                 u64 completed_at_utc_ns) noexcept;
TransactionHeaderError UpgradeTransactionHeaderIntent(TransactionHeaderRecord& record,
                                                      std::uint8_t intent) noexcept;

}  // namespace scratchbird::storage::page
