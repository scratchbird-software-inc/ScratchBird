// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "transaction_evidence.hpp"
#include "canonical_diagnostic_catalog.hpp"
#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <string_view>
#include <type_traits>

namespace { long fail_after = -1; unsigned checks = 0, failures = 0, faults = 0; }
void* operator new(std::size_t n) {
  if (fail_after == 0) throw std::bad_alloc();
  if (fail_after > 0) --fail_after;
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
namespace m = scratchbird::transaction::mga;
namespace p = scratchbird::core::platform;
namespace d = scratchbird::core::diagnostics;
using E = m::TransactionEvidenceError;
static_assert(std::is_same_v<decltype(m::TransactionLineageEvidenceRecord::transaction_uuid), p::Uuid>);
namespace {
void Check(bool value, const char* why) {
  ++checks;
  if (!value) { ++failures; std::cerr << "FAIL " << why << '\n'; }
}
p::Uuid Id(unsigned tag) {
  p::Uuid id;
  id.bytes[0] = 1; id.bytes[6] = 0x70; id.bytes[8] = 0x80; id.bytes[15] = tag;
  return id;
}
m::TransactionEvidenceContext Context() {
  // Structural projection fixtures, not admitted snapshot/policy authority.
  return {Id(1), Id(2), Id(3), 11, 12, 13, 14};
}
m::LocalTransactionInventory Inventory() {
  m::LocalTransactionInventory inventory;
  inventory.next_local_transaction_id = 4;
  inventory.next_commit_sequence = 2;
  for (unsigned i = 1; i <= 3; ++i) {
    m::TransactionInventoryEntry entry;
    entry.identity.local_id.value = i;
    entry.identity.transaction_uuid = {p::UuidKind::transaction, Id(10 + i)};
    entry.identity.scope = m::TransactionScope::local_node;
    entry.state = i == 1 ? m::TransactionState::committed :
                  i == 2 ? m::TransactionState::rolled_back : m::TransactionState::read_only_active;
    entry.commit_sequence = i == 1 ? 1 : 0;
    entry.evidence_record_written = i < 3;
    inventory.entries.push_back(entry);
  }
  return inventory;
}
bool SameContext(const m::TransactionEvidenceContext& a, const m::TransactionEvidenceContext& b) {
  return a.database_uuid == b.database_uuid && a.snapshot_uuid == b.snapshot_uuid &&
      a.policy_snapshot_uuid == b.policy_snapshot_uuid && a.snapshot_generation == b.snapshot_generation &&
      a.catalog_generation == b.catalog_generation && a.security_generation == b.security_generation &&
      a.policy_generation == b.policy_generation;
}
void InvalidContext(const m::TransactionEvidenceContext& context) {
  const auto inventory = Inventory();
  const auto projection = m::BuildTransactionLineageEvidence(inventory, context);
  const auto restore = m::ClassifyTransactionInventoryForRestore(inventory, context, false);
  Check(projection.error == E::invalid_context && projection.records.empty(), "invalid context projection");
  Check(restore.error == E::invalid_context && restore.records.empty() &&
        !restore.restore_allowed && !restore.wal_required, "invalid context restore");
}
void FaultSweep(const m::LocalTransactionInventory& inventory, bool restore, E expected) {
  bool completed = false;
  unsigned observed = 0;
  for (long allocation = 0; allocation < 1024; ++allocation) {
    fail_after = allocation;
    E error;
    std::size_t count;
    bool allowed = false;
    if (restore) {
      const auto result = m::ClassifyTransactionInventoryForRestore(inventory, Context(), false);
      error = result.error; count = result.records.size(); allowed = result.restore_allowed;
    } else {
      const auto result = m::BuildTransactionLineageEvidence(inventory, Context());
      error = result.error; count = result.records.size();
    }
    fail_after = -1;
    if (error == E::resource_exhausted) {
      ++faults; ++observed;
      Check(count == 0 && !allowed, "allocation failure published a prefix or restore permission");
      continue;
    }
    Check(error == expected && count == inventory.entries.size(), "fault sweep changed completed classification");
    Check(!restore || allowed == (expected == E::none), "fault sweep invented restore permission");
    completed = true;
    break;
  }
  Check(completed && observed, "allocation sweep did not reach complete success");
}
}
int main() {
  const auto inventory = Inventory();
  const auto context = Context();
  const auto projection = m::BuildTransactionLineageEvidence(inventory, context);
  Check(projection.ok() && projection.records.size() == 3, "complete projection");
  if (projection.records.size() != 3) return 1;
  for (unsigned i = 0; i < 3; ++i) {
    const auto& record = projection.records[i];
    Check(record.transaction_uuid == inventory.entries[i].identity.transaction_uuid.value &&
          record.local_id.value == i + 1 && SameContext(record.context, context), "binary identity/context changed");
    Check(record.evidence_written == (i < 2) && !record.wal_required, "evidence facts invented");
  }
  Check(projection.records[0].terminal_state == "committed" &&
        projection.records[1].terminal_state == "rolled_back" &&
        !projection.records[2].terminal && projection.records[2].event_class == "read_only_begin",
        "terminal/read-only classification changed");
  for (auto member : {&m::TransactionEvidenceContext::database_uuid,
                      &m::TransactionEvidenceContext::snapshot_uuid,
                      &m::TransactionEvidenceContext::policy_snapshot_uuid}) {
    auto invalid = context; invalid.*member = {}; InvalidContext(invalid);
    for (unsigned version = 0; version < 16; ++version) if (version != 7) {
      invalid = context; (invalid.*member).bytes[6] = version << 4; InvalidContext(invalid);
    }
    for (unsigned variant : {0u, 0x40u, 0xc0u}) {
      invalid = context; (invalid.*member).bytes[8] = variant; InvalidContext(invalid);
    }
  }
  for (unsigned pair = 0; pair < 3; ++pair) {
    auto invalid = context;
    if (pair == 0) invalid.snapshot_uuid = invalid.database_uuid;
    if (pair == 1) invalid.policy_snapshot_uuid = invalid.database_uuid;
    if (pair == 2) invalid.policy_snapshot_uuid = invalid.snapshot_uuid;
    InvalidContext(invalid);
  }
  for (auto member : {&m::TransactionEvidenceContext::snapshot_generation,
                      &m::TransactionEvidenceContext::catalog_generation,
                      &m::TransactionEvidenceContext::security_generation,
                      &m::TransactionEvidenceContext::policy_generation}) {
    auto changed = context; changed.*member = 0; InvalidContext(changed);
    changed.*member = std::numeric_limits<p::u64>::max();
    const auto full = m::BuildTransactionLineageEvidence(inventory, changed);
    Check(full.ok() && SameContext(full.records.front().context, changed), "unsigned generation truncated");
  }
  const auto empty = m::ClassifyTransactionInventoryForRestore({}, context, false);
  Check(empty.ok() && empty.restore_allowed && empty.records.empty(), "valid empty inventory refused");
  const auto wal = m::ClassifyTransactionInventoryForRestore(inventory, context, true);
  Check(wal.error == E::wal_not_authority && !wal.restore_allowed && !wal.wal_required &&
        wal.records.empty(), "WAL became restore authority");
  for (unsigned position = 0; position < 16; ++position) for (unsigned byte = 0; byte < 256; ++byte) {
    auto malformed = inventory;
    malformed.entries[0].identity.transaction_uuid.value.bytes[position] = byte;
    const auto forensic = m::BuildTransactionLineageEvidence(malformed, context);
    Check(forensic.ok() && forensic.records.front().transaction_uuid ==
          malformed.entries[0].identity.transaction_uuid.value, "forensic UUID bytes rendered or discarded");
  }
  auto malformed = inventory;
  malformed.entries[0].identity.transaction_uuid.value = {};
  auto forensic = m::BuildTransactionLineageEvidence(malformed, context);
  Check(forensic.ok() && forensic.records.front().transaction_uuid.is_nil() &&
        forensic.records.front().restore_classification == "refuse_fail_closed",
        "malformed forensic identity acquired restore classification");
  for (unsigned kind = 0; kind < 3; ++kind) {
    malformed = inventory;
    if (kind == 0) malformed.entries.push_back(malformed.entries.front());
    if (kind == 1) malformed.entries[0].identity.transaction_uuid.value = {};
    if (kind == 2) malformed.entries[0].state = static_cast<m::TransactionState>(65535);
    const auto refused = m::ClassifyTransactionInventoryForRestore(malformed, context, false);
    Check(refused.error == E::invalid_inventory && refused.records.empty() && !refused.restore_allowed,
          "malformed inventory published restore prefix");
  }
  for (auto origin : {m::TransactionState::committed, m::TransactionState::rolled_back,
                      m::TransactionState::failed_terminal}) {
    auto archived = inventory;
    auto& entry = archived.entries[0]; entry.state = m::TransactionState::archived;
    entry.archived_from_state = origin; entry.commit_sequence = origin == m::TransactionState::committed ? 1 : 0;
    const auto result = m::ClassifyTransactionInventoryForRestore(archived, context, false);
    Check(result.records.size() == 3 && result.restore_allowed == (origin != m::TransactionState::failed_terminal) &&
          result.records.front().terminal_state == m::TransactionStateName(origin), "archive finality lost");
  }
  auto held = inventory; held.entries[2].identity.scope = m::TransactionScope::cluster_global;
  const auto refusal = m::ClassifyTransactionInventoryForRestore(held, context, false);
  Check(refusal.error == E::restore_refused && !refusal.restore_allowed && refusal.records.size() == 3,
        "unresolved cluster transaction granted local restore");
  FaultSweep(inventory, false, E::none);
  FaultSweep(inventory, true, E::none);
  FaultSweep(held, true, E::restore_refused);
  const std::array errors{E::invalid_context, E::wal_not_authority, E::invalid_inventory,
                          E::restore_refused, E::resource_exhausted, E::internal_failure};
  const std::array<std::string_view, 6> codes{
      "MGA.EVIDENCE.INVALID_CONTEXT", "MGA.EVIDENCE.WAL_NOT_AUTHORITY",
      "MGA.EVIDENCE.INVENTORY_INVALID", "MGA.EVIDENCE.RESTORE_REFUSED",
      "MGA.EVIDENCE.RESOURCE_EXHAUSTED", "MGA.EVIDENCE.INTERNAL_FAILURE"};
  const std::array<std::string_view, 6> states{"22023", "55000", "XX001", "55000", "53200", "XX000"};
  for (unsigned i = 0; i < errors.size(); ++i) {
    const auto* code = m::TransactionEvidenceErrorCode(errors[i]);
    const auto* definition = d::FindCanonicalDiagnosticCode(code);
    Check(code == codes[i] && definition && definition->is_failure &&
          definition->severity == d::CanonicalSeverity::error && definition->sqlstate == states[i] &&
          definition->numeric_binding == "not_applicable", "projection diagnostic metadata mismatch");
  }
  Check(m::TransactionEvidenceErrorCode(E::none) == nullptr &&
        std::string_view(m::TransactionEvidenceErrorCode(static_cast<E>(255))) == "MGA.EVIDENCE.INTERNAL_FAILURE",
        "unknown projection error became success");
  std::cout << "transaction_evidence_binary checks=" << checks << " faults=" << faults
            << " failures=" << failures << '\n';
  return failures ? 1 : 0;
}
