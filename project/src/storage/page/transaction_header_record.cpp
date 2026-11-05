// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "transaction_header_record.hpp"

#include <algorithm>

namespace scratchbird::storage::page {
namespace {
using scratchbird::core::platform::LoadLittle16;
using scratchbird::core::platform::LoadLittle32;
using scratchbird::core::platform::LoadLittle64;
using scratchbird::core::platform::StoreLittle16;
using scratchbird::core::platform::StoreLittle32;
using scratchbird::core::platform::StoreLittle64;
using E = TransactionHeaderError;

bool V7(const Uuid& id) { return scratchbird::core::uuid::IsEngineIdentityUuid(id); }
bool NoneOrV7(const Uuid& id) { return id.is_nil() || V7(id); }
void PutUuid(byte* p, const Uuid& id) { std::copy(id.bytes.begin(), id.bytes.end(), p); }
Uuid GetUuid(const byte* p) {
  Uuid id;
  std::copy_n(p, 16, id.bytes.begin());
  return id;
}

// UUID fields in layout order, from +16 in 16-byte steps.
template <class Record> auto UuidFields(Record& r) {
  return std::array{&r.database_uuid,          &r.transaction_uuid,       &r.engine_instance_uuid,
                    &r.principal_or_agent_uuid, &r.attachment_uuid,        &r.session_uuid,
                    &r.policy_snapshot_uuid,    &r.security_snapshot_uuid, &r.start_snapshot_uuid,
                    &r.durable_operation_uuid,  &r.recovery_attempt_uuid,  &r.cluster_uuid,
                    &r.cluster_transaction_uuid};
}
constexpr std::size_t kUuidsAt = 16;

bool FinalState(TipStateCode s) {
  return s == TipStateCode::committed || s == TipStateCode::rolled_back || s == TipStateCode::dead;
}
}  // namespace

const char* TransactionHeaderErrorName(TransactionHeaderError error) noexcept {
  switch (error) {
    case E::none: return "none";
    case E::invalid_framing: return "invalid_framing";
    case E::invalid_order: return "invalid_order";
    case E::invalid_identity: return "invalid_identity";
    case E::invalid_optional_identity: return "invalid_optional_identity";
    case E::invalid_cluster: return "invalid_cluster";
    case E::invalid_class: return "invalid_class";
    case E::invalid_completion: return "invalid_completion";
    case E::already_complete: return "already_complete";
    case E::short_buffer: return "short_buffer";
  }
  return "invalid";
}

TransactionHeaderError ValidateTransactionHeaderRecord(const TransactionHeaderRecord& r) noexcept {
  if (r.transaction_order == 0) return E::invalid_order;
  if (!V7(r.database_uuid) || !V7(r.transaction_uuid)) return E::invalid_identity;
  const auto fields = UuidFields(r);
  for (std::size_t i = 2; i < fields.size(); ++i)
    if (!NoneOrV7(*fields[i])) return E::invalid_optional_identity;
  // A cluster transaction UUID or epoch needs the cluster it belongs to.
  if (r.cluster_uuid.is_nil() && (!r.cluster_transaction_uuid.is_nil() || r.cluster_epoch != 0))
    return E::invalid_cluster;
  if (static_cast<std::uint8_t>(r.authority) > 2 || r.intent > kTipIntentMax) return E::invalid_class;
  if (r.final_state == TipStateCode::unused) {
    if (r.commit_time_utc_ns != 0 || r.completed_at_utc_ns != 0) return E::invalid_completion;
  } else {
    if (!FinalState(r.final_state) || r.completed_at_utc_ns == 0) return E::invalid_completion;
    const bool committed = r.final_state == TipStateCode::committed;
    if (committed ? r.commit_time_utc_ns != r.completed_at_utc_ns : r.commit_time_utc_ns != 0)
      return E::invalid_completion;
  }
  return E::none;
}

TransactionHeaderError EncodeTransactionHeaderRecord(const TransactionHeaderRecord& r,
                                                     std::span<byte> output) noexcept {
  if (const auto e = ValidateTransactionHeaderRecord(r); e != E::none) return e;
  if (output.size() < kTransactionHeaderRecordBytes) return E::short_buffer;
  byte* p = output.data();
  std::fill_n(p, kTransactionHeaderRecordBytes, byte{0});
  StoreLittle16(p + 0, kTransactionHeaderRecordVersion);
  StoreLittle16(p + 2, static_cast<u16>(kTransactionHeaderRecordBytes));
  p[4] = static_cast<byte>(r.authority);
  p[5] = r.intent;
  p[6] = r.isolation_profile;
  p[7] = r.read_only ? kTransactionHeaderFlagReadOnly : 0;
  StoreLittle64(p + 8, r.transaction_order);
  const auto fields = UuidFields(r);
  for (std::size_t i = 0; i < fields.size(); ++i) PutUuid(p + kUuidsAt + 16 * i, *fields[i]);
  StoreLittle64(p + 224, r.cluster_epoch);
  StoreLittle32(p + 232, r.intent_generation);
  p[236] = static_cast<byte>(r.final_state);
  StoreLittle64(p + 240, r.commit_time_utc_ns);
  StoreLittle64(p + 248, r.completed_at_utc_ns);
  return E::none;
}

TransactionHeaderDecodeResult DecodeTransactionHeaderRecord(std::span<const byte> bytes) noexcept {
  TransactionHeaderDecodeResult result;
  if (bytes.size() != kTransactionHeaderRecordBytes) return result;
  const byte* p = bytes.data();
  if (LoadLittle16(p + 0) != kTransactionHeaderRecordVersion ||
      LoadLittle16(p + 2) != kTransactionHeaderRecordBytes || (p[7] & ~kTransactionHeaderFlagReadOnly) ||
      p[236] > kTipStateCodeMax || p[237] || p[238] || p[239] || p[4] > 2)
    return result;
  TransactionHeaderRecord r;
  r.authority = static_cast<TipAuthorityClass>(p[4]);
  r.intent = p[5];
  r.isolation_profile = p[6];
  r.read_only = (p[7] & kTransactionHeaderFlagReadOnly) != 0;
  r.transaction_order = LoadLittle64(p + 8);
  const auto fields = UuidFields(r);
  for (std::size_t i = 0; i < fields.size(); ++i) *fields[i] = GetUuid(p + kUuidsAt + 16 * i);
  r.cluster_epoch = LoadLittle64(p + 224);
  r.intent_generation = LoadLittle32(p + 232);
  r.final_state = static_cast<TipStateCode>(p[236]);
  r.commit_time_utc_ns = LoadLittle64(p + 240);
  r.completed_at_utc_ns = LoadLittle64(p + 248);
  result.error = ValidateTransactionHeaderRecord(r);
  if (result.error == E::none) result.record = r;
  return result;
}

TransactionHeaderError CompleteTransactionHeader(TransactionHeaderRecord& r, TipStateCode final_state,
                                                 u64 completed_at_utc_ns) noexcept {
  if (r.final_state != TipStateCode::unused) return E::already_complete;
  if (!FinalState(final_state) || completed_at_utc_ns == 0) return E::invalid_completion;
  auto next = r;
  next.final_state = final_state;
  next.completed_at_utc_ns = completed_at_utc_ns;
  next.commit_time_utc_ns = final_state == TipStateCode::committed ? completed_at_utc_ns : 0;
  if (const auto e = ValidateTransactionHeaderRecord(next); e != E::none) return e;
  r = next;
  return E::none;
}

TransactionHeaderError UpgradeTransactionHeaderIntent(TransactionHeaderRecord& r, std::uint8_t intent) noexcept {
  if (r.final_state != TipStateCode::unused) return E::already_complete;
  if (intent > kTipIntentMax || intent == r.intent) return E::invalid_class;
  if (r.intent_generation == 0xFFFFFFFFu) return E::invalid_class;
  auto next = r;
  next.intent = intent;
  next.intent_generation = r.intent_generation + 1;
  if (const auto e = ValidateTransactionHeaderRecord(next); e != E::none) return e;
  r = next;
  return E::none;
}

}  // namespace scratchbird::storage::page
