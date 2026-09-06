// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "mga_relation_store/mga_relation_store.hpp"
#include "mga_relation_store/mga_event_sequence_allocator.hpp"
#include "mga_relation_store/mga_relation_store_internal_support.hpp"

#include "api_diagnostics.hpp"
#include "agents/index_garbage_cleanup_agent.hpp"
#include "crud_support/crud_store.hpp"
#include "ipar_fault_injection.hpp"
#include "local_transaction_store.hpp"
#include "secondary_index_delta_merge.hpp"
#include "transaction_state.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api {
namespace {

// SEARCH_KEY: SB_ENGINE_MGA_SECONDARY_INDEX_COORDINATION_IMPLEMENTATION_AUTHORITY
// Owns secondary-index delta staging, transactional overlay, merge, recovery,
// validation, repair, and garbage cleanup. Transaction finality and visibility
// remain projections of the durable MGA transaction inventory.

namespace agents = scratchbird::core::agents::implemented_agents;
namespace idx = scratchbird::core::index;

using scratchbird::storage::database::LoadLocalTransactionInventoryFromDatabase;
using scratchbird::transaction::mga::LookupLocalTransaction;
using scratchbird::transaction::mga::MakeLocalTransactionId;
using scratchbird::transaction::mga::TransactionState;

constexpr const char* kRowStoreMagic = "SBMGA1";

std::string IndexStorePath(const EngineRequestContext& context) {
  return context.database_path + ".sb.mga_index_entries";
}

std::string ScopedRelationStoreRoot(const EngineRequestContext& context) {
  return context.database_path + ".sb.mga_relation_scope";
}

std::string ScopedRelationSegmentName(const std::string& table_uuid) {
  std::string name;
  name.reserve(table_uuid.size());
  for (const char ch : table_uuid) {
    const bool safe = (ch >= 'a' && ch <= 'z') ||
                      (ch >= 'A' && ch <= 'Z') ||
                      (ch >= '0' && ch <= '9') ||
                      ch == '-' || ch == '_';
    name.push_back(safe ? ch : '_');
  }
  return name.empty() ? std::string("unknown") : name;
}

std::string ScopedRowStorePath(const EngineRequestContext& context,
                               const std::string& table_uuid) {
  return ScopedRelationStoreRoot(context) + "/" +
         ScopedRelationSegmentName(table_uuid) + ".rows";
}

std::string ScopedRowBinaryStorePath(const EngineRequestContext& context,
                                     const std::string& table_uuid) {
  return ScopedRelationStoreRoot(context) + "/" +
         ScopedRelationSegmentName(table_uuid) + ".rows.sbnr";
}

std::string ScopedIndexStorePath(const EngineRequestContext& context,
                                 const std::string& table_uuid) {
  return ScopedRelationStoreRoot(context) + "/" +
         ScopedRelationSegmentName(table_uuid) + ".indexes";
}

std::string ScopedIndexBinaryStorePath(const EngineRequestContext& context,
                                       const std::string& table_uuid) {
  return ScopedRelationStoreRoot(context) + "/" +
         ScopedRelationSegmentName(table_uuid) + ".indexes.sbnx";
}

std::string ScopedSummaryStorePath(const EngineRequestContext& context,
                                   const std::string& table_uuid) {
  return ScopedRelationStoreRoot(context) + "/" +
         ScopedRelationSegmentName(table_uuid) + ".summary";
}

std::string SecondaryIndexDeltaLedgerStorePath(
    const EngineRequestContext& context) {
  return context.database_path + ".sb.mga_secondary_index_delta_ledger";
}

EngineApiDiagnostic OkDiagnostic() {
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

std::uint64_t ChecksumText(std::string_view text) {
  std::uint64_t checksum = 1469598103934665603ULL;
  for (const unsigned char value : text) {
    checksum ^= value;
    checksum *= 1099511628211ULL;
  }
  return checksum;
}

bool FileExistsAndNotEmpty(const std::string& path) {
  std::error_code error;
  return std::filesystem::exists(path, error) &&
         std::filesystem::file_size(path, error) != 0;
}

std::vector<std::string> ReadLines(const std::string& path) {
  std::vector<std::string> lines;
  std::ifstream in(path, std::ios::binary);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (!line.empty()) {
      lines.push_back(std::move(line));
    }
  }
  return lines;
}

std::vector<std::uint8_t> ReadBinaryFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return {};
  }
  return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in),
                                   std::istreambuf_iterator<char>());
}

std::vector<std::string> SplitTabs(const std::string& line) {
  std::vector<std::string> fields;
  std::size_t begin = 0;
  while (begin <= line.size()) {
    const auto end = line.find('\t', begin);
    fields.push_back(line.substr(begin, end == std::string::npos
                                           ? std::string::npos
                                           : end - begin));
    if (end == std::string::npos) {
      break;
    }
    begin = end + 1;
  }
  return fields;
}

std::string JoinLine(const std::vector<std::string>& fields) {
  std::string line;
  for (std::size_t i = 0; i < fields.size(); ++i) {
    if (i != 0) {
      line.push_back('\t');
    }
    line += fields[i];
  }
  return line;
}

std::uint64_t ParseU64(const std::string& value) {
  std::uint64_t parsed = 0;
  const auto result = std::from_chars(value.data(), value.data() + value.size(),
                                      parsed);
  return result.ec == std::errc{} ? parsed : 0;
}

idx::SecondaryIndexDeltaLedgerLimits DefaultSecondaryIndexDeltaLedgerLimits() {
  return {};
}

EngineApiDiagnostic DiagnosticFromSecondaryIndexDeltaLedger(
    const scratchbird::core::platform::DiagnosticRecord& diagnostic,
    const std::string& fallback_code,
    const std::string& fallback_key) {
  std::string detail = diagnostic.remediation_hint;
  for (const auto& argument : diagnostic.arguments) {
    if (!detail.empty()) { detail += ";"; }
    detail += argument.key + "=" + argument.value;
  }
  return MakeEngineApiDiagnostic(diagnostic.diagnostic_code.empty() ? fallback_code
                                                                    : diagnostic.diagnostic_code,
                                 diagnostic.message_key.empty() ? fallback_key
                                                                : diagnostic.message_key,
                                 detail,
                                 true);
}

bool IsUniqueMgaIndex(const CrudIndexRecord& index) {
  return index.unique ||
         std::find(index.key_envelopes.begin(), index.key_envelopes.end(), "unique") !=
             index.key_envelopes.end();
}

EngineApiDiagnostic ParseLedgerTypedUuid(const std::string& text,
                                         scratchbird::core::platform::UuidKind kind,
                                         idx::TypedUuid* out) {
  if (text.empty() || out == nullptr) {
    return MakeInvalidRequestDiagnostic("mga.secondary_index_delta_ledger",
                                        "typed_uuid_required");
  }
  const auto parsed = scratchbird::core::uuid::ParseDurableEngineIdentityUuid(kind, text);
  if (!parsed.ok()) {
    return DiagnosticFromSecondaryIndexDeltaLedger(
        parsed.diagnostic,
        "SB-MGA-SECONDARY-DELTA-UUID-INVALID",
        "mga.secondary_index_delta_ledger.invalid_uuid");
  }
  *out = parsed.value;
  return OkDiagnostic();
}

std::string MakeSecondaryIndexDeltaKeyPayload(
    const CrudIndexRecord& index,
    const std::string& key,
    const std::vector<std::pair<std::string, std::string>>& values) {
  return EncodeCrudPairs({{"key", key},
                          {"payload", CrudFieldValue(values, index.column_name)},
                          {"family", index.family.empty() ? CrudIndexFamilyForProfile(index.profile)
                                                          : index.family}});
}

std::string MakeSecondaryIndexDeltaCleanupHorizonToken(const EngineRequestContext& context) {
  return "mga_cleanup_horizon:visible_through=" +
         std::to_string(context.snapshot_visible_through_local_transaction_id) +
         ":local_tx=" + std::to_string(context.local_transaction_id);
}

std::string MakeSecondaryIndexDeltaEvidenceReference(
    const EngineRequestContext& context,
    const MgaSecondaryIndexDeltaLedgerEntryInput& input,
    const std::string& key) {
  const std::string request_id = context.request_id.empty() ? "request_uuid_unset"
                                                           : context.request_id;
  return "engine.dml.secondary_index_delta:" + request_id +
         ":index=" + input.index.index_uuid +
         ":row=" + input.row_uuid +
         ":version=" + input.version_uuid +
         ":key_hash=" + std::to_string(ChecksumText(key));
}

std::uint64_t MaxCommittedLocalTransactionId(const RelationReadSnapshot& state) {
  std::uint64_t max_committed = 0;
  for (const auto& [tx, status] : state.transactions) {
    if (status == "committed" || status == "archived") {
      max_committed = std::max(max_committed, tx);
    }
  }
  return max_committed;
}

std::uint64_t SnapshotVisibleThroughForOverlay(const RelationReadSnapshot& state,
                                               const EngineRequestContext& context) {
  const std::string isolation = context.transaction_isolation_level.empty()
                                    ? std::string("read_committed")
                                    : context.transaction_isolation_level;
  if ((isolation == "snapshot" || isolation == "repeatable_read" ||
       isolation == "serializable")) {
    return context.snapshot_visible_through_local_transaction_id;
  }
  return MaxCommittedLocalTransactionId(state);
}

std::string DeltaPayloadField(const std::string& key_payload,
                              const std::string& field) {
  for (const auto& [key, value] : DecodeCrudPairs(key_payload)) {
    if (key == field) {
      return value;
    }
  }
  return {};
}

int CompareOverlayScalar(const std::string& left, const std::string& right) {
  try {
    std::size_t left_end = 0;
    std::size_t right_end = 0;
    const auto left_number = std::stoll(left, &left_end);
    const auto right_number = std::stoll(right, &right_end);
    if (left_end == left.size() && right_end == right.size()) {
      if (left_number < right_number) { return -1; }
      if (left_number > right_number) { return 1; }
      return 0;
    }
  } catch (...) {
  }
  if (left < right) { return -1; }
  if (left > right) { return 1; }
  return 0;
}

bool OverlayPredicateSupported(const EnginePredicateEnvelope& predicate) {
  return predicate.predicate_kind == "column_equals" ||
         predicate.predicate_kind == "column_in_list" ||
         predicate.predicate_kind == "column_range";
}

bool OverlayEntryMatchesPredicate(const idx::SecondaryIndexOverlayEntry& entry,
                                  const EnginePredicateEnvelope& predicate) {
  const std::string key = DeltaPayloadField(entry.key_payload, "key");
  if (predicate.predicate_kind == "column_equals") {
    return !predicate.bound_values.empty() &&
           key == predicate.bound_values.front().encoded_value;
  }
  if (predicate.predicate_kind == "column_in_list") {
    for (const auto& bound : predicate.bound_values) {
      if (key == bound.encoded_value) { return true; }
    }
    return false;
  }
  if (predicate.predicate_kind == "column_range") {
    const bool lower_ok = predicate.bound_values.empty() ||
        CompareOverlayScalar(key, predicate.bound_values[0].encoded_value) >= 0;
    const bool upper_ok = predicate.bound_values.size() < 2 ||
        CompareOverlayScalar(key, predicate.bound_values[1].encoded_value) <= 0;
    return lower_ok && upper_ok;
  }
  return false;
}

bool LedgerRecordRelevantToIndex(const idx::SecondaryIndexDeltaLedgerRecord& record,
                                 const CrudIndexRecord& index,
                                 const std::string& table_uuid) {
  return scratchbird::core::uuid::UuidToString(record.delta.index_uuid.value) ==
             index.index_uuid &&
         scratchbird::core::uuid::UuidToString(record.delta.table_uuid.value) ==
             table_uuid;
}

EngineApiDiagnostic OverlayLookupDiagnostic(
    const scratchbird::core::platform::DiagnosticRecord& diagnostic,
    const std::string& fallback_code,
    const std::string& fallback_key) {
  std::string detail = diagnostic.remediation_hint;
  for (const auto& argument : diagnostic.arguments) {
    if (!detail.empty()) { detail += ";"; }
    detail += argument.key + "=" + argument.value;
  }
  return MakeEngineApiDiagnostic(diagnostic.diagnostic_code.empty() ? fallback_code
                                                                    : diagnostic.diagnostic_code,
                                 diagnostic.message_key.empty() ? fallback_key
                                                                : diagnostic.message_key,
                                 detail,
                                 true);
}

std::optional<CrudIndexRecord> SelectCrudIndexForPredicate(const RelationReadSnapshot& state,
                                                           const std::string& table_uuid,
                                                           const EnginePredicateEnvelope& predicate,
                                                           std::uint64_t observer_tx) {
  for (const auto& index : VisibleCrudIndexesForTable(state, table_uuid, observer_tx)) {
    if (CrudIndexSupportsPredicate(index, predicate)) {
      return index;
    }
  }
  return std::nullopt;
}

idx::SecondaryIndexKind SecondaryIndexKindForCrudIndex(const CrudIndexRecord& index) {
  return IsUniqueMgaIndex(index) ? idx::SecondaryIndexKind::unique
                                : idx::SecondaryIndexKind::non_unique;
}

EngineApiDiagnostic BaseEntryForOverlay(const CrudIndexEntryRecord& entry,
                                        idx::SecondaryIndexBaseEntry* out) {
  if (out == nullptr) {
    return MakeInvalidRequestDiagnostic("mga.secondary_index_delta_overlay",
                                        "base_entry_required");
  }
  idx::SecondaryIndexBaseEntry base;
  auto diagnostic = ParseLedgerTypedUuid(entry.index_uuid,
                                         scratchbird::core::platform::UuidKind::object,
                                         &base.index_uuid);
  if (diagnostic.error) { return diagnostic; }
  diagnostic = ParseLedgerTypedUuid(entry.table_uuid,
                                    scratchbird::core::platform::UuidKind::object,
                                    &base.table_uuid);
  if (diagnostic.error) { return diagnostic; }
  diagnostic = ParseLedgerTypedUuid(entry.row_uuid,
                                    scratchbird::core::platform::UuidKind::row,
                                    &base.row_uuid);
  if (diagnostic.error) { return diagnostic; }
  diagnostic = ParseLedgerTypedUuid(entry.version_uuid,
                                    scratchbird::core::platform::UuidKind::row,
                                    &base.version_uuid);
  if (diagnostic.error) { return diagnostic; }
  base.key_payload = EncodeCrudPairs({{"key", entry.key_value},
                                      {"payload", entry.payload_value},
                                      {"family", entry.family}});
  base.committed_local_transaction_id = 0;
  *out = std::move(base);
  return OkDiagnostic();
}

MgaIndexedRowsLookupResult RefuseIndexedLookup(std::string detail,
                                               EngineApiDiagnostic diagnostic = {}) {
  MgaIndexedRowsLookupResult result;
  result.ok = false;
  result.index_refused = true;
  result.diagnostic = diagnostic.error ? std::move(diagnostic)
                                       : MakeInvalidRequestDiagnostic(
                                             "mga.secondary_index_delta_overlay",
                                             std::move(detail));
  result.evidence.push_back({"mga_secondary_index_delta_overlay_refused_code",
                             result.diagnostic.code});
  result.evidence.push_back({"mga_secondary_index_delta_overlay_refused",
                             result.diagnostic.detail});
  return result;
}

EngineApiDiagnostic Dpc024MergeDiagnostic(const std::string& code,
                                          const std::string& message_key,
                                          const std::string& detail) {
  return MakeEngineApiDiagnostic(code, message_key, detail, true);
}

void AddMergeEvidence(std::vector<EngineEvidenceReference>* evidence,
                      const std::string& kind,
                      const std::string& value) {
  if (evidence == nullptr) { return; }
  evidence->push_back({kind, value});
}

EngineApiDiagnostic Dpc025RecoveryDiagnostic(const std::string& code,
                                             const std::string& message_key,
                                             const std::string& detail) {
  return MakeEngineApiDiagnostic(code, message_key, detail, true);
}

void AddRecoveryEvidence(std::vector<EngineEvidenceReference>* evidence,
                         const std::string& kind,
                         const std::string& value) {
  if (evidence == nullptr) { return; }
  evidence->push_back({kind, value});
}

EngineApiDiagnostic Dpc033CleanupDiagnostic(const std::string& code,
                                            const std::string& message_key,
                                            const std::string& detail,
                                            bool error = true) {
  return MakeEngineApiDiagnostic(code, message_key, detail, error);
}

void AddIndexGarbageCleanupEvidence(std::vector<EngineEvidenceReference>* evidence,
                                    const std::string& kind,
                                    const std::string& value) {
  if (evidence == nullptr) { return; }
  evidence->push_back({kind, value});
}

void AddEventSequenceReservationEvidence(
    std::vector<EngineEvidenceReference>* evidence,
    const MgaEventSequenceRangeReservation& reservation) {
  if (evidence == nullptr || !reservation.ok) { return; }
  evidence->push_back({"mga_event_sequence_allocator_path",
                       reservation.allocator_path});
  evidence->push_back({"mga_event_sequence_allocator_stream",
                       reservation.stream_kind});
  evidence->push_back({"mga_event_sequence_allocator_first",
                       std::to_string(reservation.first)});
  evidence->push_back({"mga_event_sequence_allocator_count",
                       std::to_string(reservation.count)});
  evidence->push_back({"mga_event_sequence_allocator_next",
                       std::to_string(reservation.next)});
  evidence->push_back({"mga_event_sequence_allocator_bootstrap",
                       reservation.bootstrapped_from_store ? "true" : "false"});
}

std::string Dpc033DiagnosticDetail(
    const scratchbird::core::platform::DiagnosticRecord& diagnostic) {
  if (!diagnostic.remediation_hint.empty()) {
    return diagnostic.remediation_hint;
  }
  std::string detail;
  for (const auto& argument : diagnostic.arguments) {
    if (!detail.empty()) {
      detail += ";";
    }
    detail += argument.key + "=" + argument.value;
  }
  return detail;
}

bool IsDpc025CommittedTerminal(TransactionState state) {
  return state == TransactionState::committed ||
         state == TransactionState::archived;
}

bool IsDpc025RolledBackTerminal(TransactionState state) {
  return state == TransactionState::rolled_back ||
         state == TransactionState::failed_terminal;
}

bool Dpc025DeltaRequiresPublishedBase(idx::SecondaryIndexDeltaKind kind) {
  return kind == idx::SecondaryIndexDeltaKind::insert ||
         kind == idx::SecondaryIndexDeltaKind::update_after;
}

bool Dpc025PublishedBaseContainsRecord(const RelationReadSnapshot& state,
                                       const CrudIndexRecord& index,
                                       const std::string& table_uuid,
                                       const idx::SecondaryIndexDeltaLedgerRecord& record,
                                       EngineApiDiagnostic* diagnostic) {
  const std::string row_uuid =
      scratchbird::core::uuid::UuidToString(record.delta.row_uuid.value);
  const std::string version_uuid =
      scratchbird::core::uuid::UuidToString(record.delta.version_uuid.value);
  for (const auto& entry : state.index_entries) {
    if (entry.index_uuid != index.index_uuid ||
        entry.table_uuid != table_uuid ||
        entry.row_uuid != row_uuid ||
        entry.version_uuid != version_uuid ||
        !CrudCreatorVisible(state, entry.creator_tx, entry.event_sequence, 0)) {
      continue;
    }
    idx::SecondaryIndexBaseEntry base;
    const auto converted = BaseEntryForOverlay(entry, &base);
    if (converted.error) {
      if (diagnostic != nullptr) { *diagnostic = converted; }
      return false;
    }
    if (base.key_payload == record.delta.key_payload) {
      return true;
    }
  }
  if (diagnostic != nullptr) {
    *diagnostic = Dpc025RecoveryDiagnostic(
        "secondary_index_delta_recovery_base_entry_missing",
        "mga.secondary_index_delta_recovery.base_entry_missing",
        "merged_cleaned ledger record has no matching committed base index entry");
  }
  return false;
}

EngineApiDiagnostic Dpc033TableSnapshotEntryForCleanup(
    const CrudIndexRecord& index,
    const CrudRowVersionRecord& row,
    const std::string& key,
    idx::SecondaryIndexTableSnapshotEntry* out) {
  if (out == nullptr) {
    return Dpc033CleanupDiagnostic("INDEX_GARBAGE_CLEANUP.VALIDATION_REFUSED",
                                   "mga.secondary_index_garbage_cleanup.snapshot_entry_required",
                                   "table snapshot entry output is required");
  }
  idx::SecondaryIndexTableSnapshotEntry entry;
  auto diagnostic = ParseLedgerTypedUuid(index.index_uuid,
                                         scratchbird::core::platform::UuidKind::object,
                                         &entry.index_uuid);
  if (diagnostic.error) { return diagnostic; }
  diagnostic = ParseLedgerTypedUuid(index.table_uuid,
                                    scratchbird::core::platform::UuidKind::object,
                                    &entry.table_uuid);
  if (diagnostic.error) { return diagnostic; }
  diagnostic = ParseLedgerTypedUuid(row.row_uuid,
                                    scratchbird::core::platform::UuidKind::row,
                                    &entry.row_uuid);
  if (diagnostic.error) { return diagnostic; }
  diagnostic = ParseLedgerTypedUuid(row.version_uuid,
                                    scratchbird::core::platform::UuidKind::row,
                                    &entry.version_uuid);
  if (diagnostic.error) { return diagnostic; }
  entry.key_payload = MakeSecondaryIndexDeltaKeyPayload(index, key, row.values);
  entry.deleted = row.deleted;
  *out = std::move(entry);
  return OkDiagnostic();
}

std::optional<CrudIndexRecord> FindVisibleCrudIndexByUuid(const RelationReadSnapshot& state,
                                                          const std::string& table_uuid,
                                                          const std::string& index_uuid,
                                                          std::uint64_t observer_tx) {
  for (const auto& index : VisibleCrudIndexesForTable(state, table_uuid, observer_tx)) {
    if (index.index_uuid == index_uuid) { return index; }
  }
  return std::nullopt;
}

bool LedgerRecordBelongsToUniqueIndex(const idx::SecondaryIndexDeltaLedgerRecord& record,
                                      const RelationReadSnapshot& state) {
  const std::string index_uuid =
      scratchbird::core::uuid::UuidToString(record.delta.index_uuid.value);
  for (const auto& index : state.indexes) {
    if (index.index_uuid == index_uuid && IsUniqueMgaIndex(index)) {
      return true;
    }
  }
  return false;
}

EngineApiDiagnostic CrudIndexEntryForMergedBase(const CrudIndexRecord& index,
                                                const idx::SecondaryIndexBaseEntry& base,
                                                CrudIndexEntryRecord* out) {
  if (out == nullptr) {
    return MakeInvalidRequestDiagnostic("mga.secondary_index_delta_merge",
                                        "index_entry_required");
  }
  CrudIndexEntryRecord entry;
  entry.creator_tx = base.committed_local_transaction_id;
  entry.index_uuid = scratchbird::core::uuid::UuidToString(base.index_uuid.value);
  entry.table_uuid = scratchbird::core::uuid::UuidToString(base.table_uuid.value);
  entry.column_name = index.column_name;
  entry.family = DeltaPayloadField(base.key_payload, "family");
  if (entry.family.empty()) {
    entry.family = index.family.empty() ? CrudIndexFamilyForProfile(index.profile)
                                        : index.family;
  }
  entry.entry_kind = "exact";
  entry.key_value = DeltaPayloadField(base.key_payload, "key");
  entry.payload_value = DeltaPayloadField(base.key_payload, "payload");
  entry.row_uuid = scratchbird::core::uuid::UuidToString(base.row_uuid.value);
  entry.version_uuid = scratchbird::core::uuid::UuidToString(base.version_uuid.value);
  if (entry.index_uuid.empty() || entry.table_uuid.empty() || entry.row_uuid.empty() ||
      entry.version_uuid.empty() || entry.key_value.empty()) {
    return Dpc024MergeDiagnostic("corrupt_ledger_refused",
                                 "mga.secondary_index_delta_merge.corrupt_base_entry_refused",
                                 "merged base index entry lost required identity or key payload");
  }
  *out = std::move(entry);
  return OkDiagnostic();
}

EngineApiDiagnostic RewriteMgaIndexEntriesForMergedIndex(
    const EngineRequestContext& context,
    const RelationReadSnapshot& state,
    const CrudIndexRecord& index,
    const std::string& table_uuid,
    const std::vector<idx::SecondaryIndexBaseEntry>& base_entries) {
  // New writes are relation-scoped and readers deliberately prefer the
  // scoped segment over the legacy database-wide sidecar.  Publishing a
  // merge into the legacy file while a scoped segment exists makes the
  // merged rows unreachable as soon as the delta ledger is cleaned.  Rewrite
  // the same physical authority that supplied this relation's base entries;
  // retain the legacy path only for databases that have not yet created a
  // scoped segment.
  const std::string scoped_path = ScopedIndexStorePath(context, table_uuid);
  const bool relation_uses_scoped_storage =
      FileExistsAndNotEmpty(ScopedRowStorePath(context, table_uuid)) ||
      FileExistsAndNotEmpty(ScopedRowBinaryStorePath(context, table_uuid)) ||
      FileExistsAndNotEmpty(scoped_path) ||
      FileExistsAndNotEmpty(ScopedIndexBinaryStorePath(context, table_uuid)) ||
      FileExistsAndNotEmpty(ScopedSummaryStorePath(context, table_uuid));
  const std::string path = relation_uses_scoped_storage
                               ? scoped_path
                               : IndexStorePath(context);
  const auto existing_lines = ReadLines(path);
  std::vector<std::string> output_lines;
  output_lines.reserve(existing_lines.size() + base_entries.size());
  std::uint64_t max_sequence = 0;
  for (const auto& line : existing_lines) {
    const auto fields = SplitTabs(line);
    if (fields.size() >= 13 && fields[0] == kRowStoreMagic &&
        fields[1] == "INDEX_ENTRY") {
      max_sequence = std::max(max_sequence, ParseU64(fields[3]));
      if (fields[4] == index.index_uuid && fields[5] == table_uuid) {
        const std::uint64_t creator_tx = ParseU64(fields[2]);
        const auto tx = state.transactions.find(creator_tx);
        const bool transaction_still_live =
            tx != state.transactions.end() &&
            (tx->second == "active" || tx->second == "prepared");
        if (!CrudCreatorVisible(state, creator_tx, ParseU64(fields[3]), 0) &&
            transaction_still_live) {
          output_lines.push_back(line);
        }
        continue;
      }
    }
    output_lines.push_back(line);
  }

  std::uint64_t event_sequence = max_sequence + 1;
  for (const auto& base : base_entries) {
    if (scratchbird::core::uuid::UuidToString(base.index_uuid.value) != index.index_uuid ||
        scratchbird::core::uuid::UuidToString(base.table_uuid.value) != table_uuid ||
        base.deleted) {
      continue;
    }
    CrudIndexEntryRecord entry;
    const auto converted = CrudIndexEntryForMergedBase(index, base, &entry);
    if (converted.error) { return converted; }
    entry.event_sequence = event_sequence++;
    entry.sequence = entry.event_sequence;
    output_lines.push_back(JoinLine({kRowStoreMagic,
                                     "INDEX_ENTRY",
                                     std::to_string(entry.creator_tx),
                                     std::to_string(entry.event_sequence),
                                     entry.index_uuid,
                                     entry.table_uuid,
                                     entry.column_name,
                                     entry.family,
                                     entry.entry_kind,
                                     entry.key_value,
                                     entry.payload_value,
                                     entry.row_uuid,
                                     entry.version_uuid}));
  }

  const std::string tmp_path = path + ".tmp.merge." +
      std::to_string(context.local_transaction_id) + "." +
      std::to_string(output_lines.size());
  {
    std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
    if (!out) {
      return Dpc024MergeDiagnostic("corrupt_ledger_refused",
                                   "mga.secondary_index_delta_merge.index_store_rewrite_failed",
                                   "temporary index-entry sidecar could not be opened");
    }
    for (const auto& line : output_lines) {
      out << line << '\n';
    }
    out.flush();
    if (!out) {
      return Dpc024MergeDiagnostic("corrupt_ledger_refused",
                                   "mga.secondary_index_delta_merge.index_store_rewrite_failed",
                                   "temporary index-entry sidecar could not be written");
    }
  }
  std::error_code rename_error;
  std::filesystem::rename(tmp_path, path, rename_error);
  if (rename_error) {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::rename(tmp_path, path, rename_error);
  }
  if (rename_error) {
    return Dpc024MergeDiagnostic("corrupt_ledger_refused",
                                 "mga.secondary_index_delta_merge.index_store_rewrite_failed",
                                 "index-entry sidecar replacement failed");
  }
  return OkDiagnostic();
}

EngineApiDiagnostic BuildSecondaryIndexDeltaLedgerRecord(
    const EngineRequestContext& context,
    const MgaSecondaryIndexDeltaLedgerEntryInput& input,
    const std::string& key,
    idx::SecondaryIndexDeltaLedgerRecord* out) {
  if (out == nullptr) {
    return MakeInvalidRequestDiagnostic("mga.secondary_index_delta_ledger",
                                        "ledger_record_required");
  }
  if (IsUniqueMgaIndex(input.index)) {
    return MakeInvalidRequestDiagnostic("mga.secondary_index_delta_ledger",
                                        "unique_index_delta_refused");
  }
  idx::SecondaryIndexDeltaLedgerRecord record;
  auto diagnostic = ParseLedgerTypedUuid(GenerateCrudEngineUuid("object"),
                                         scratchbird::core::platform::UuidKind::object,
                                         &record.delta.delta_id);
  if (diagnostic.error) { return diagnostic; }
  diagnostic = ParseLedgerTypedUuid(input.index.index_uuid,
                                    scratchbird::core::platform::UuidKind::object,
                                    &record.delta.index_uuid);
  if (diagnostic.error) { return diagnostic; }
  diagnostic = ParseLedgerTypedUuid(input.table_uuid.empty() ? input.index.table_uuid
                                                            : input.table_uuid,
                                    scratchbird::core::platform::UuidKind::object,
                                    &record.delta.table_uuid);
  if (diagnostic.error) { return diagnostic; }
  diagnostic = ParseLedgerTypedUuid(input.row_uuid,
                                    scratchbird::core::platform::UuidKind::row,
                                    &record.delta.row_uuid);
  if (diagnostic.error) { return diagnostic; }
  diagnostic = ParseLedgerTypedUuid(input.version_uuid,
                                    scratchbird::core::platform::UuidKind::row,
                                    &record.delta.version_uuid);
  if (diagnostic.error) { return diagnostic; }
  diagnostic = ParseLedgerTypedUuid(context.transaction_uuid.canonical,
                                    scratchbird::core::platform::UuidKind::transaction,
                                    &record.delta.transaction_uuid);
  if (diagnostic.error) { return diagnostic; }

  record.delta.local_transaction_id = context.local_transaction_id;
  record.delta.delta_kind = input.delta_kind;
  record.delta.key_payload = MakeSecondaryIndexDeltaKeyPayload(input.index, key, input.values);
  record.delta.cleanup_horizon_token =
      input.cleanup_horizon_token.empty()
          ? MakeSecondaryIndexDeltaCleanupHorizonToken(context)
          : input.cleanup_horizon_token;
  record.delta.committed = false;
  record.commit_state = idx::SecondaryIndexDeltaLedgerCommitState::precommit_uncommitted;
  record.source_evidence_reference =
      input.source_evidence_reference.empty()
          ? MakeSecondaryIndexDeltaEvidenceReference(context, input, key)
          : input.source_evidence_reference;
  *out = std::move(record);
  return OkDiagnostic();
}

MgaSecondaryIndexDeltaLedgerResult LoadSecondaryIndexDeltaLedgerFromPath(
    const EngineRequestContext& context) {
  MgaSecondaryIndexDeltaLedgerResult result;
  if (context.database_path.empty()) {
    result.diagnostic = MakeInvalidRequestDiagnostic("mga.secondary_index_delta_ledger",
                                                    "database_path_required");
    return result;
  }
  const std::string path = SecondaryIndexDeltaLedgerStorePath(context);
  std::error_code exists_error;
  if (!std::filesystem::exists(path, exists_error)) {
    result.ok = true;
    result.diagnostic = OkDiagnostic();
    return result;
  }
  const auto bytes = ReadBinaryFile(path);
  if (bytes.empty()) {
    result.ok = true;
    result.diagnostic = OkDiagnostic();
    return result;
  }
  const auto decoded = idx::DecodePersistentSecondaryIndexDeltaLedger(
      bytes,
      DefaultSecondaryIndexDeltaLedgerLimits());
  if (!decoded.ok()) {
    result.diagnostic = DiagnosticFromSecondaryIndexDeltaLedger(
        decoded.diagnostic,
        "SB-MGA-SECONDARY-DELTA-LOAD-FAILED",
        "mga.secondary_index_delta_ledger.load_failed");
    return result;
  }
  result.ok = true;
  result.ledger = decoded.ledger;
  result.diagnostic = OkDiagnostic();
  return result;
}

EngineApiDiagnostic WriteSecondaryIndexDeltaLedger(
    const EngineRequestContext& context,
    const idx::PersistentSecondaryIndexDeltaLedger& ledger) {
  const auto encoded = idx::EncodePersistentSecondaryIndexDeltaLedger(
      ledger,
      DefaultSecondaryIndexDeltaLedgerLimits());
  if (!encoded.ok()) {
    return DiagnosticFromSecondaryIndexDeltaLedger(
        encoded.diagnostic,
        "SB-MGA-SECONDARY-DELTA-ENCODE-FAILED",
        "mga.secondary_index_delta_ledger.encode_failed");
  }
  const std::string path = SecondaryIndexDeltaLedgerStorePath(context);
  const std::string tmp_path = path + ".tmp." +
                               std::to_string(context.local_transaction_id) + "." +
                               std::to_string(ledger.records.size());
  {
    std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
    if (!out) {
      return MakeInvalidRequestDiagnostic("mga.secondary_index_delta_ledger",
                                          "ledger_store_append_failed");
    }
    out.write(reinterpret_cast<const char*>(encoded.bytes.data()),
              static_cast<std::streamsize>(encoded.bytes.size()));
    out.flush();
    if (!out) {
      return MakeInvalidRequestDiagnostic("mga.secondary_index_delta_ledger",
                                          "ledger_store_append_failed");
    }
  }
  std::error_code rename_error;
  std::filesystem::rename(tmp_path, path, rename_error);
  if (rename_error) {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::rename(tmp_path, path, rename_error);
  }
  if (rename_error) {
    return MakeInvalidRequestDiagnostic("mga.secondary_index_delta_ledger",
                                        "ledger_store_replace_failed");
  }
  return OkDiagnostic();
}


}  // namespace

MgaSecondaryIndexDeltaLedgerResult LoadMgaSecondaryIndexDeltaLedger(
    const EngineRequestContext& context) {
  return LoadSecondaryIndexDeltaLedgerFromPath(context);
}

EngineApiDiagnostic AppendMgaSecondaryIndexDeltaLedgerEntries(
    const EngineRequestContext& context,
    const std::vector<MgaSecondaryIndexDeltaLedgerEntryInput>& entries,
    std::vector<EngineEvidenceReference>* evidence) {
  // DPC_DEFERRED_INDEX_WRITE_PATH
  if (entries.empty()) {
    return OkDiagnostic();
  }
  const auto authority = ValidateMgaMutatingTransactionAuthorityForStoreModule(
      context,
      "mga.secondary_index_delta_ledger.append");
  if (authority.error) { return authority; }
  if (context.transaction_uuid.canonical.empty()) {
    return MakeInvalidRequestDiagnostic("mga.secondary_index_delta_ledger",
                                        "transaction_uuid_required");
  }

  auto loaded = LoadSecondaryIndexDeltaLedgerFromPath(context);
  if (!loaded.ok) { return loaded.diagnostic; }
  auto ledger = loaded.ledger;
  const std::uint64_t existing_delta_record_count =
      static_cast<std::uint64_t>(ledger.records.size());
  std::vector<idx::SecondaryIndexDeltaLedgerRecord> staged_records;
  for (const auto& input : entries) {
    if (IsUniqueMgaIndex(input.index)) {
      return MakeInvalidRequestDiagnostic("mga.secondary_index_delta_ledger",
                                          "unique_index_delta_refused");
    }
    const auto keys = CrudIndexKeysForValues(input.index, input.values);
    for (const auto& key : keys) {
      idx::SecondaryIndexDeltaLedgerRecord record;
      const auto built = BuildSecondaryIndexDeltaLedgerRecord(context, input, key, &record);
      if (built.error) { return built; }
      const auto appended = idx::AppendPersistentSecondaryIndexDelta(
          &ledger,
          record,
          DefaultSecondaryIndexDeltaLedgerLimits());
      if (!appended.ok()) {
        return DiagnosticFromSecondaryIndexDeltaLedger(
            appended.diagnostic,
            "SB-MGA-SECONDARY-DELTA-APPEND-FAILED",
            "mga.secondary_index_delta_ledger.append_failed");
      }
      staged_records.push_back(std::move(record));
    }
  }
  if (staged_records.empty()) {
    return OkDiagnostic();
  }
  const auto reservation = ReserveEventSequenceRange(
      context,
      "secondary_index_delta_ledger",
      SecondaryIndexDeltaLedgerStorePath(context),
      static_cast<std::uint64_t>(staged_records.size()),
      [existing_delta_record_count]() { return existing_delta_record_count + 1; });
  if (!reservation.ok) { return reservation.diagnostic; }
  const auto written = WriteSecondaryIndexDeltaLedger(context, ledger);
  if (written.error) { return written; }
  if (evidence != nullptr) {
    AddEventSequenceReservationEvidence(evidence, reservation);
    for (const auto& record : staged_records) {
      evidence->push_back({"mga_secondary_index_delta_ledger",
                           scratchbird::core::uuid::UuidToString(record.delta.delta_id.value)});
      evidence->push_back({"mga_secondary_index_delta_index",
                           scratchbird::core::uuid::UuidToString(record.delta.index_uuid.value)});
      evidence->push_back({"mga_secondary_index_delta_kind",
                           idx::SecondaryIndexDeltaKindName(record.delta.delta_kind)});
    }
  }
  return OkDiagnostic();
}

EngineApiDiagnostic CommitMgaSecondaryIndexDeltaLedgerTransaction(
    const EngineRequestContext& context,
    std::uint64_t local_transaction_id) {
  if (local_transaction_id == 0) {
    return OkDiagnostic();
  }
  // Compatibility cleanup only.  This marker is derived from durable MGA
  // inventory finality and can never lead it or participate in commit success.
  const auto inventory = LoadLocalTransactionInventoryFromDatabase(
      context.database_path);
  if (!inventory.ok()) {
    return MakeInvalidRequestDiagnostic(
        "mga.secondary_index_delta_ledger",
        "transaction_inventory_unavailable_for_derived_promotion");
  }
  const auto transaction = LookupLocalTransaction(
      inventory.inventory, MakeLocalTransactionId(local_transaction_id));
  if (!transaction.ok() ||
      (transaction.entry.state != TransactionState::committed &&
       transaction.entry.state != TransactionState::archived)) {
    return MakeInvalidRequestDiagnostic(
        "mga.secondary_index_delta_ledger",
        "committed_inventory_finality_required_for_derived_promotion");
  }
  auto loaded = LoadSecondaryIndexDeltaLedgerFromPath(context);
  if (!loaded.ok) { return loaded.diagnostic; }
  bool changed = false;
  for (auto& record : loaded.ledger.records) {
    if (record.delta.local_transaction_id != local_transaction_id) {
      continue;
    }
    if (record.commit_state ==
        idx::SecondaryIndexDeltaLedgerCommitState::precommit_uncommitted) {
      record.commit_state =
          idx::SecondaryIndexDeltaLedgerCommitState::committed_premerge;
      record.delta.committed = true;
      changed = true;
    }
  }
  if (!changed) {
    return OkDiagnostic();
  }
  return WriteSecondaryIndexDeltaLedger(context, loaded.ledger);
}

EngineApiDiagnostic RollbackMgaSecondaryIndexDeltaLedgerTransaction(
    const EngineRequestContext& context,
    std::uint64_t local_transaction_id) {
  if (local_transaction_id == 0) {
    return OkDiagnostic();
  }
  auto loaded = LoadSecondaryIndexDeltaLedgerFromPath(context);
  if (!loaded.ok) { return loaded.diagnostic; }
  const auto before = loaded.ledger.records.size();
  loaded.ledger.records.erase(
      std::remove_if(loaded.ledger.records.begin(),
                     loaded.ledger.records.end(),
                     [local_transaction_id](const idx::SecondaryIndexDeltaLedgerRecord& record) {
                       return record.delta.local_transaction_id == local_transaction_id;
                     }),
      loaded.ledger.records.end());
  if (loaded.ledger.records.size() == before) {
    return OkDiagnostic();
  }
  return WriteSecondaryIndexDeltaLedger(context, loaded.ledger);
}

MgaSecondaryIndexDeltaMergeAgentResult MergeMgaSecondaryIndexDeltasForIndex(
    const EngineRequestContext& context,
    const MgaSecondaryIndexDeltaMergeAgentRequest& request) {
  // DPC_SECONDARY_INDEX_DELTA_MERGE_AGENT_GATE: supplied-horizon merge path.
  MgaSecondaryIndexDeltaMergeAgentResult result;
  result.index_uuid = request.index_uuid;
  result.table_uuid = request.table_uuid;
  result.authoritative_cleanup_horizon_local_transaction_id =
      request.authoritative_cleanup_horizon_local_transaction_id;
  AddMergeEvidence(&result.evidence, "mga_secondary_index_delta_merge_index", request.index_uuid);
  AddMergeEvidence(&result.evidence, "mga_secondary_index_delta_merge_table", request.table_uuid);
  AddMergeEvidence(&result.evidence,
                   "mga_secondary_index_delta_merge_horizon",
                   std::to_string(request.authoritative_cleanup_horizon_local_transaction_id));

  if (request.merge_disabled) {
    result.throttle_or_refusal_reason = "merge_agent_disabled";
    result.diagnostic = Dpc024MergeDiagnostic("merge_agent_disabled",
                                              "mga.secondary_index_delta_merge.disabled",
                                              "secondary-index delta merge agent is disabled by request");
    AddMergeEvidence(&result.evidence, "mga_secondary_index_delta_merge_refused",
                     result.throttle_or_refusal_reason);
    return result;
  }
  if (!request.cleanup_horizon_authoritative) {
    result.throttle_or_refusal_reason = "not_authoritative_horizon";
    result.diagnostic = Dpc024MergeDiagnostic("not_authoritative_horizon",
                                              "mga.secondary_index_delta_merge.not_authoritative_horizon",
                                              "authoritative cleanup horizon is required");
    AddMergeEvidence(&result.evidence, "mga_secondary_index_delta_merge_refused",
                     result.throttle_or_refusal_reason);
    return result;
  }
  if (request.max_records_to_scan == 0 || request.max_records_to_merge == 0) {
    result.throttled = true;
    result.throttle_or_refusal_reason = "resource_governor_throttled";
    result.diagnostic = Dpc024MergeDiagnostic("resource_governor_throttled",
                                              "mga.secondary_index_delta_merge.resource_governor_throttled",
                                              "nonzero scan and merge budgets are required");
    AddMergeEvidence(&result.evidence, "mga_secondary_index_delta_merge_refused",
                     result.throttle_or_refusal_reason);
    return result;
  }

  auto loaded_state = LoadMgaRelationStoreState(context);
  if (!loaded_state.ok) {
    result.throttle_or_refusal_reason = "corrupt_ledger_refused";
    result.diagnostic = Dpc024MergeDiagnostic("corrupt_ledger_refused",
                                              "mga.secondary_index_delta_merge.state_load_refused",
                                              loaded_state.diagnostic.detail);
    AddMergeEvidence(&result.evidence, "mga_secondary_index_delta_merge_refused",
                     result.throttle_or_refusal_reason);
    return result;
  }
  RelationReadSnapshot state = BuildCrudCompatibilityStateFromMga(loaded_state.state);
  const auto selected = FindVisibleCrudIndexByUuid(
      state,
      request.table_uuid,
      request.index_uuid,
      context.local_transaction_id);
  if (!selected) {
    result.throttle_or_refusal_reason = "corrupt_ledger_refused";
    result.diagnostic = Dpc024MergeDiagnostic("corrupt_ledger_refused",
                                              "mga.secondary_index_delta_merge.index_not_found",
                                              "requested index/table identity is not visible");
    AddMergeEvidence(&result.evidence, "mga_secondary_index_delta_merge_refused",
                     result.throttle_or_refusal_reason);
    return result;
  }
  if (IsUniqueMgaIndex(*selected)) {
    result.throttle_or_refusal_reason = "unique_index_delta_refused";
    result.diagnostic = Dpc024MergeDiagnostic("unique_index_delta_refused",
                                              "mga.secondary_index_delta_merge.unique_index_delta_refused",
                                              "unique secondary indexes cannot use deferred delta merge");
    AddMergeEvidence(&result.evidence, "mga_secondary_index_delta_merge_refused",
                     result.throttle_or_refusal_reason);
    return result;
  }

  auto loaded_ledger = LoadSecondaryIndexDeltaLedgerFromPath(context);
  if (!loaded_ledger.ok) {
    result.throttle_or_refusal_reason = "corrupt_ledger_refused";
    result.diagnostic = Dpc024MergeDiagnostic("corrupt_ledger_refused",
                                              "mga.secondary_index_delta_merge.ledger_load_refused",
                                              loaded_ledger.diagnostic.detail);
    AddMergeEvidence(&result.evidence, "mga_secondary_index_delta_merge_refused",
                     result.throttle_or_refusal_reason);
    return result;
  }
  if (loaded_ledger.ledger.records.size() > request.max_records_to_scan) {
    result.throttled = true;
    result.throttle_or_refusal_reason = "resource_governor_throttled";
    result.scanned_count = request.max_records_to_scan;
    result.retained_count = loaded_ledger.ledger.records.size();
    result.diagnostic = Dpc024MergeDiagnostic("resource_governor_throttled",
                                              "mga.secondary_index_delta_merge.resource_governor_throttled",
                                              "persistent delta ledger exceeds bounded scan budget");
    AddMergeEvidence(&result.evidence, "mga_secondary_index_delta_merge_refused",
                     result.throttle_or_refusal_reason);
    AddMergeEvidence(&result.evidence, "mga_secondary_index_delta_merge_retained_count",
                     std::to_string(result.retained_count));
    return result;
  }

  const auto recovery = idx::ClassifySecondaryIndexDeltaLedgerForRecovery(
      loaded_ledger.ledger);
  if (!recovery.ok() ||
      recovery.action == idx::SecondaryIndexDeltaLedgerRecoveryAction::refuse_open ||
      recovery.action == idx::SecondaryIndexDeltaLedgerRecoveryAction::fail_closed ||
      recovery.action == idx::SecondaryIndexDeltaLedgerRecoveryAction::rebuild_from_authoritative_base) {
    result.throttle_or_refusal_reason = "corrupt_ledger_refused";
    result.diagnostic = Dpc024MergeDiagnostic("corrupt_ledger_refused",
                                              "mga.secondary_index_delta_merge.recovery_refused",
                                              recovery.stable_reason);
    AddMergeEvidence(&result.evidence, "mga_secondary_index_delta_merge_refused",
                     result.throttle_or_refusal_reason);
    return result;
  }
  if (request.ipar_fault_injection_point == "secondary_merge") {
    result.throttle_or_refusal_reason = "ipar_fault_injection_secondary_merge";
    result.diagnostic = IparFaultDiagnostic("mga.secondary_index_delta_merge",
                                            "secondary_merge",
                                            "phase=before_merge_publish");
    AddMergeEvidence(&result.evidence,
                     "mga_secondary_index_delta_merge_refused",
                     result.throttle_or_refusal_reason);
    AddMergeEvidence(&result.evidence,
                     "mga_secondary_index_delta_merge_authority",
                     kIparP706Authority);
    AppendIparFaultEvidence(&result.evidence,
                            "secondary_merge",
                            "retain_delta_overlay_or_retry_under_inventory_horizon");
    return result;
  }

  idx::SecondaryIndexDeltaLedger staged_delta_ledger;
  std::vector<std::size_t> processed_record_indexes;
  std::uint64_t eligible_count = 0;
  for (std::size_t i = 0; i < loaded_ledger.ledger.records.size(); ++i) {
    const auto& record = loaded_ledger.ledger.records[i];
    if (LedgerRecordBelongsToUniqueIndex(record, state)) {
      result.throttle_or_refusal_reason = "unique_index_delta_refused";
      result.diagnostic = Dpc024MergeDiagnostic("unique_index_delta_refused",
                                                "mga.secondary_index_delta_merge.unique_index_delta_refused",
                                                "persistent ledger contains a unique-index delta record");
      AddMergeEvidence(&result.evidence, "mga_secondary_index_delta_merge_refused",
                       result.throttle_or_refusal_reason);
      return result;
    }
    if (!LedgerRecordRelevantToIndex(record, *selected, request.table_uuid)) {
      continue;
    }
    ++result.scanned_count;
    if (record.commit_state == idx::SecondaryIndexDeltaLedgerCommitState::repair_rebuild_required ||
        record.commit_state == idx::SecondaryIndexDeltaLedgerCommitState::refused) {
      result.throttle_or_refusal_reason = "corrupt_ledger_refused";
      result.diagnostic = Dpc024MergeDiagnostic("corrupt_ledger_refused",
                                                "mga.secondary_index_delta_merge.record_state_refused",
                                                "persistent ledger record is repair-required or refused");
      AddMergeEvidence(&result.evidence, "mga_secondary_index_delta_merge_refused",
                       result.throttle_or_refusal_reason);
      return result;
    }
    const auto transaction = state.transactions.find(
        record.delta.local_transaction_id);
    const bool inventory_committed =
        transaction != state.transactions.end() &&
        (transaction->second == "committed" || transaction->second == "archived");
    const bool recoverably_committed =
        (record.commit_state ==
             idx::SecondaryIndexDeltaLedgerCommitState::committed_premerge &&
         record.delta.committed) ||
        (record.commit_state ==
             idx::SecondaryIndexDeltaLedgerCommitState::precommit_uncommitted &&
         inventory_committed);
    const bool eligible =
        recoverably_committed &&
        record.delta.local_transaction_id <=
            request.authoritative_cleanup_horizon_local_transaction_id;
    if (!eligible) {
      ++result.retained_count;
      continue;
    }
    ++eligible_count;
    if (processed_record_indexes.size() >= request.max_records_to_merge) {
      ++result.retained_count;
      result.throttled = true;
      result.throttle_or_refusal_reason = "resource_governor_throttled";
      continue;
    }
    auto staged_delta = record.delta;
    staged_delta.committed = true;
    staged_delta_ledger.deltas.push_back(std::move(staged_delta));
    processed_record_indexes.push_back(i);
  }

  std::vector<idx::SecondaryIndexBaseEntry> base_entries;
  base_entries.reserve(state.index_entries.size() + staged_delta_ledger.deltas.size());
  for (const auto& entry : state.index_entries) {
    if (entry.index_uuid != selected->index_uuid ||
        entry.table_uuid != request.table_uuid ||
        !CrudCreatorVisible(state,
                            entry.creator_tx,
                            entry.event_sequence,
                            context.local_transaction_id)) {
      continue;
    }
    idx::SecondaryIndexBaseEntry base;
    const auto converted = BaseEntryForOverlay(entry, &base);
    if (converted.error) {
      result.throttle_or_refusal_reason = "corrupt_ledger_refused";
      result.diagnostic = Dpc024MergeDiagnostic("corrupt_ledger_refused",
                                                "mga.secondary_index_delta_merge.base_entry_refused",
                                                converted.detail);
      AddMergeEvidence(&result.evidence, "mga_secondary_index_delta_merge_refused",
                       result.throttle_or_refusal_reason);
      return result;
    }
    base.committed_local_transaction_id = entry.creator_tx;
    base_entries.push_back(std::move(base));
  }

  idx::SecondaryIndexDeltaMergeLedger merge_ledger;
  idx::SecondaryIndexMergeRequest core_request;
  auto diagnostic = ParseLedgerTypedUuid(request.index_uuid,
                                         scratchbird::core::platform::UuidKind::object,
                                         &core_request.index_uuid);
  if (diagnostic.error) {
    result.throttle_or_refusal_reason = "corrupt_ledger_refused";
    result.diagnostic = Dpc024MergeDiagnostic("corrupt_ledger_refused",
                                              "mga.secondary_index_delta_merge.index_uuid_refused",
                                              diagnostic.detail);
    AddMergeEvidence(&result.evidence, "mga_secondary_index_delta_merge_refused",
                     result.throttle_or_refusal_reason);
    return result;
  }
  diagnostic = ParseLedgerTypedUuid(request.table_uuid,
                                    scratchbird::core::platform::UuidKind::object,
                                    &core_request.table_uuid);
  if (diagnostic.error) {
    result.throttle_or_refusal_reason = "corrupt_ledger_refused";
    result.diagnostic = Dpc024MergeDiagnostic("corrupt_ledger_refused",
                                              "mga.secondary_index_delta_merge.table_uuid_refused",
                                              diagnostic.detail);
    AddMergeEvidence(&result.evidence, "mga_secondary_index_delta_merge_refused",
                     result.throttle_or_refusal_reason);
    return result;
  }
  core_request.authoritative_cleanup_horizon_local_transaction_id =
      request.authoritative_cleanup_horizon_local_transaction_id;
  core_request.cleanup_horizon_authoritative = request.cleanup_horizon_authoritative;
  core_request.index_kind = idx::SecondaryIndexKind::non_unique;
  core_request.max_records_to_scan = request.max_records_to_scan;
  core_request.max_records_to_merge = request.max_records_to_merge;
  core_request.merge_disabled = request.merge_disabled;
  const auto merged = idx::MergeSecondaryIndexDeltas(
      &merge_ledger,
      &base_entries,
      &staged_delta_ledger,
      core_request);
  if (!merged.ok()) {
    result.throttled = merged.throttled;
    result.throttle_or_refusal_reason = merged.diagnostic.diagnostic_code;
    result.diagnostic = Dpc024MergeDiagnostic(merged.diagnostic.diagnostic_code,
                                              merged.diagnostic.message_key,
                                              merged.diagnostic.remediation_hint);
    AddMergeEvidence(&result.evidence, "mga_secondary_index_delta_merge_refused",
                     result.throttle_or_refusal_reason);
    return result;
  }

  const auto rewritten = RewriteMgaIndexEntriesForMergedIndex(
      context,
      state,
      *selected,
      request.table_uuid,
      base_entries);
  if (rewritten.error) {
    result.throttle_or_refusal_reason = "corrupt_ledger_refused";
    result.diagnostic = rewritten;
    AddMergeEvidence(&result.evidence, "mga_secondary_index_delta_merge_refused",
                     result.throttle_or_refusal_reason);
    return result;
  }

  for (std::size_t index : processed_record_indexes) {
    loaded_ledger.ledger.records[index].commit_state =
        idx::SecondaryIndexDeltaLedgerCommitState::merged_cleaned;
    loaded_ledger.ledger.records[index].delta.committed = true;
  }
  const auto written = WriteSecondaryIndexDeltaLedger(context, loaded_ledger.ledger);
  if (written.error) {
    result.throttle_or_refusal_reason = "corrupt_ledger_refused";
    result.diagnostic = Dpc024MergeDiagnostic("corrupt_ledger_refused",
                                              "mga.secondary_index_delta_merge.ledger_replace_refused",
                                              written.detail);
    AddMergeEvidence(&result.evidence, "mga_secondary_index_delta_merge_refused",
                     result.throttle_or_refusal_reason);
    return result;
  }

  result.ok = true;
  result.merged_count = merged.evidence.merged_count;
  result.cleaned_count = static_cast<std::uint64_t>(processed_record_indexes.size());
  result.diagnostic = OkDiagnostic();
  AddMergeEvidence(&result.evidence, "mga_secondary_index_delta_merge_result",
                   result.throttled ? "resource_governor_throttled" : "successful_merge");
  AddMergeEvidence(&result.evidence, "mga_secondary_index_delta_merge_merged_count",
                   std::to_string(result.merged_count));
  AddMergeEvidence(&result.evidence, "mga_secondary_index_delta_merge_retained_count",
                   std::to_string(result.retained_count));
  AddMergeEvidence(&result.evidence, "mga_secondary_index_delta_merge_cleaned_count",
                   std::to_string(result.cleaned_count));
  AddMergeEvidence(&result.evidence, "mga_secondary_index_delta_merge_scanned_count",
                   std::to_string(result.scanned_count));
  if (result.throttled) {
    AddMergeEvidence(&result.evidence, "mga_secondary_index_delta_merge_throttle_reason",
                     result.throttle_or_refusal_reason);
  }
  (void)eligible_count;
  return result;
}

MgaSecondaryIndexDeltaRecoveryRepairResult ValidateAndRepairMgaSecondaryIndexDeltaLedgerForIndex(
    const EngineRequestContext& context,
    const MgaSecondaryIndexDeltaRecoveryRepairRequest& request) {
  // DPC_SECONDARY_INDEX_DELTA_RECOVERY_REPAIR: engine-owned bounded recovery
  // validation. Transaction finality comes only from durable MGA inventory.
  MgaSecondaryIndexDeltaRecoveryRepairResult result;
  AddRecoveryEvidence(&result.evidence,
                      "mga_secondary_index_delta_recovery_search_key",
                      "DPC_SECONDARY_INDEX_DELTA_RECOVERY_REPAIR");
  AddRecoveryEvidence(&result.evidence,
                      "mga_secondary_index_delta_recovery_index",
                      request.index_uuid);
  AddRecoveryEvidence(&result.evidence,
                      "mga_secondary_index_delta_recovery_table",
                      request.table_uuid);
  AddRecoveryEvidence(&result.evidence,
                      "mga_secondary_index_delta_recovery_scan_budget",
                      std::to_string(request.max_records_to_scan));

  auto refuse = [&](std::string code,
                    std::string message_key,
                    std::string detail) {
    result.ok = false;
    result.refused = true;
    result.fail_closed = true;
    result.diagnostic = Dpc025RecoveryDiagnostic(code, message_key, detail);
    AddRecoveryEvidence(&result.evidence,
                        "mga_secondary_index_delta_recovery_refused",
                        result.diagnostic.code);
    return result;
  };

  if (request.index_uuid.empty() || request.table_uuid.empty()) {
    return refuse("secondary_index_delta_recovery_invalid_request",
                  "mga.secondary_index_delta_recovery.invalid_request",
                  "index_uuid and table_uuid are required");
  }
  if (request.max_records_to_scan == 0) {
    return refuse("secondary_index_delta_recovery_scan_budget_exhausted",
                  "mga.secondary_index_delta_recovery.scan_budget_exhausted",
                  "recovery validation requires a nonzero bounded scan budget");
  }

  auto loaded_state = LoadMgaRelationStoreState(context);
  if (!loaded_state.ok) {
    return refuse("secondary_index_delta_recovery_state_load_refused",
                  "mga.secondary_index_delta_recovery.state_load_refused",
                  loaded_state.diagnostic.detail);
  }
  RelationReadSnapshot state = BuildCrudCompatibilityStateFromMga(loaded_state.state);
  const auto selected = FindVisibleCrudIndexByUuid(
      state,
      request.table_uuid,
      request.index_uuid,
      context.local_transaction_id);
  if (!selected) {
    return refuse("secondary_index_delta_recovery_index_not_found",
                  "mga.secondary_index_delta_recovery.index_not_found",
                  "requested index/table identity is not visible to recovery validation");
  }
  if (IsUniqueMgaIndex(*selected)) {
    return refuse("unique_index_delta_refused",
                  "mga.secondary_index_delta_recovery.unique_index_delta_refused",
                  "unique secondary indexes cannot use deferred delta recovery repair");
  }

  auto loaded_ledger = LoadSecondaryIndexDeltaLedgerFromPath(context);
  if (!loaded_ledger.ok) {
    result.diagnostic = loaded_ledger.diagnostic;
    result.ok = false;
    result.refused = true;
    result.fail_closed = true;
    AddRecoveryEvidence(&result.evidence,
                        "mga_secondary_index_delta_recovery_refused",
                        result.diagnostic.code);
    return result;
  }

  const auto recovery = idx::ClassifySecondaryIndexDeltaLedgerForRecovery(
      loaded_ledger.ledger);
  result.recovery_class =
      idx::SecondaryIndexDeltaLedgerRecoveryClassName(recovery.recovery_class);
  result.recovery_action =
      idx::SecondaryIndexDeltaLedgerRecoveryActionName(recovery.action);
  AddRecoveryEvidence(&result.evidence,
                      "mga_secondary_index_delta_recovery_class",
                      result.recovery_class);
  AddRecoveryEvidence(&result.evidence,
                      "mga_secondary_index_delta_recovery_action",
                      result.recovery_action);
  if (recovery.fail_closed ||
      recovery.action == idx::SecondaryIndexDeltaLedgerRecoveryAction::fail_closed ||
      recovery.action == idx::SecondaryIndexDeltaLedgerRecoveryAction::refuse_open) {
    return refuse("secondary_index_delta_recovery_ledger_refused",
                  "mga.secondary_index_delta_recovery.ledger_refused",
                  recovery.stable_reason);
  }

  const auto loaded_inventory =
      LoadLocalTransactionInventoryFromDatabase(context.database_path);
  if (!loaded_inventory.ok()) {
    return refuse(loaded_inventory.diagnostic.diagnostic_code.empty()
                      ? "secondary_index_delta_recovery_inventory_load_refused"
                      : loaded_inventory.diagnostic.diagnostic_code,
                  loaded_inventory.diagnostic.message_key.empty()
                      ? "mga.secondary_index_delta_recovery.inventory_load_refused"
                      : loaded_inventory.diagnostic.message_key,
                  loaded_inventory.diagnostic.remediation_hint);
  }

  idx::PersistentSecondaryIndexDeltaLedger repaired_ledger = loaded_ledger.ledger;
  std::vector<idx::SecondaryIndexDeltaLedgerRecord> retained_records;
  retained_records.reserve(repaired_ledger.records.size());
  bool changed = false;
  for (const auto& record : repaired_ledger.records) {
    if (!LedgerRecordRelevantToIndex(record, *selected, request.table_uuid)) {
      retained_records.push_back(record);
      continue;
    }
    ++result.scanned_count;
    if (result.scanned_count > request.max_records_to_scan) {
      return refuse("secondary_index_delta_recovery_scan_budget_exhausted",
                    "mga.secondary_index_delta_recovery.scan_budget_exhausted",
                    "persistent delta ledger exceeds bounded recovery scan budget");
    }
    if (LedgerRecordBelongsToUniqueIndex(record, state)) {
      return refuse("unique_index_delta_refused",
                    "mga.secondary_index_delta_recovery.unique_index_delta_refused",
                    "persistent ledger contains a unique-index delta record");
    }

    idx::SecondaryIndexDeltaLedgerRecord staged = record;
    bool keep_record = true;
    switch (record.commit_state) {
      case idx::SecondaryIndexDeltaLedgerCommitState::precommit_uncommitted: {
        const auto lookup = LookupLocalTransaction(
            loaded_inventory.inventory,
            MakeLocalTransactionId(record.delta.local_transaction_id));
        if (!lookup.ok()) {
          return refuse("secondary_index_delta_recovery_missing_transaction_authority",
                        "mga.secondary_index_delta_recovery.missing_transaction_authority",
                        "precommit delta has no durable MGA transaction inventory entry");
        }
        if (IsDpc025CommittedTerminal(lookup.entry.state)) {
          ++result.committed_premerge_count;
          ++result.retained_count;
          result.recovery_class =
              "committed_premerge_requires_overlay_merge";
          result.recovery_action = "apply_overlay_then_merge";
          if (request.repair_enabled) {
            staged.commit_state =
                idx::SecondaryIndexDeltaLedgerCommitState::committed_premerge;
            staged.delta.committed = true;
            ++result.promoted_count;
            changed = true;
          }
        } else if (IsDpc025RolledBackTerminal(lookup.entry.state)) {
          if (request.repair_enabled) {
            keep_record = false;
            ++result.removed_count;
            changed = true;
          } else {
            ++result.retained_count;
          }
        } else {
          ++result.retained_count;
        }
        break;
      }
      case idx::SecondaryIndexDeltaLedgerCommitState::committed_premerge: {
        const auto lookup = LookupLocalTransaction(
            loaded_inventory.inventory,
            MakeLocalTransactionId(record.delta.local_transaction_id));
        if (!lookup.ok()) {
          return refuse("secondary_index_delta_recovery_missing_transaction_authority",
                        "mga.secondary_index_delta_recovery.missing_transaction_authority",
                        "committed_premerge delta has no durable MGA transaction inventory entry");
        }
        if (IsDpc025CommittedTerminal(lookup.entry.state)) {
          ++result.committed_premerge_count;
          ++result.retained_count;
        } else if (IsDpc025RolledBackTerminal(lookup.entry.state)) {
          keep_record = false;
          ++result.removed_count;
          changed = true;
        } else {
          return refuse("secondary_index_delta_recovery_commit_state_inconsistent",
                        "mga.secondary_index_delta_recovery.commit_state_inconsistent",
                        "committed_premerge delta is not backed by committed MGA inventory state");
        }
        break;
      }
      case idx::SecondaryIndexDeltaLedgerCommitState::merged_cleaned: {
        ++result.merged_cleaned_count;
        ++result.retained_count;
        if (request.require_authoritative_base &&
            Dpc025DeltaRequiresPublishedBase(record.delta.delta_kind)) {
          EngineApiDiagnostic base_diagnostic;
          if (!Dpc025PublishedBaseContainsRecord(state,
                                                 *selected,
                                                 request.table_uuid,
                                                 record,
                                                 &base_diagnostic)) {
            result.diagnostic = base_diagnostic;
            result.ok = false;
            result.refused = true;
            result.fail_closed = true;
            AddRecoveryEvidence(&result.evidence,
                                "mga_secondary_index_delta_recovery_refused",
                                result.diagnostic.code);
            return result;
          }
        }
        break;
      }
      case idx::SecondaryIndexDeltaLedgerCommitState::repair_rebuild_required:
      case idx::SecondaryIndexDeltaLedgerCommitState::refused:
        return refuse("secondary_index_delta_recovery_record_state_refused",
                      "mga.secondary_index_delta_recovery.record_state_refused",
                      "persistent ledger contains repair-required or refused record state");
    }

    if (keep_record) {
      retained_records.push_back(std::move(staged));
    }
  }

  if (changed && !request.repair_enabled) {
    return refuse("secondary_index_delta_recovery_repair_disabled",
                  "mga.secondary_index_delta_recovery.repair_disabled",
                  "bounded repair is required but was not enabled by request");
  }
  if (changed) {
    repaired_ledger.records = std::move(retained_records);
    const auto written = WriteSecondaryIndexDeltaLedger(context, repaired_ledger);
    if (written.error) {
      return refuse("secondary_index_delta_recovery_write_refused",
                    "mga.secondary_index_delta_recovery.write_refused",
                    written.detail);
    }
    result.repaired = true;
  }

  result.ok = true;
  result.fail_closed = false;
  result.diagnostic = OkDiagnostic();
  AddRecoveryEvidence(&result.evidence,
                      "mga_secondary_index_delta_recovery_result",
                      result.repaired ? "repaired" : "validated");
  AddRecoveryEvidence(&result.evidence,
                      "mga_secondary_index_delta_recovery_scanned_count",
                      std::to_string(result.scanned_count));
  AddRecoveryEvidence(&result.evidence,
                      "mga_secondary_index_delta_recovery_retained_count",
                      std::to_string(result.retained_count));
  AddRecoveryEvidence(&result.evidence,
                      "mga_secondary_index_delta_recovery_removed_count",
                      std::to_string(result.removed_count));
  AddRecoveryEvidence(&result.evidence,
                      "mga_secondary_index_delta_recovery_promoted_count",
                      std::to_string(result.promoted_count));
  return result;
}

MgaSecondaryIndexGarbageCleanupResult CleanupMgaSecondaryIndexGarbageForIndex(
    const EngineRequestContext& context,
    const MgaSecondaryIndexGarbageCleanupRequest& request) {
  // DPC_SECONDARY_INDEX_GARBAGE_CLEANUP_AGENT: engine-owned cleanup using
  // DPC-030 durable MGA cleanup horizon authority.
  MgaSecondaryIndexGarbageCleanupResult result;
  AddIndexGarbageCleanupEvidence(&result.evidence,
                                 "mga_secondary_index_garbage_cleanup_search_key",
                                 "DPC_SECONDARY_INDEX_GARBAGE_CLEANUP_AGENT");
  AddIndexGarbageCleanupEvidence(&result.evidence,
                                 "mga_secondary_index_garbage_cleanup_index",
                                 request.index_uuid);
  AddIndexGarbageCleanupEvidence(&result.evidence,
                                 "mga_secondary_index_garbage_cleanup_table",
                                 request.table_uuid);
  AddIndexGarbageCleanupEvidence(&result.evidence,
                                 "mga_secondary_index_garbage_cleanup_scan_budget",
                                 std::to_string(request.max_records_to_scan));
  AddIndexGarbageCleanupEvidence(&result.evidence,
                                 "mga_secondary_index_garbage_cleanup_clean_budget",
                                 std::to_string(request.max_records_to_clean));

  auto refuse = [&](std::string code,
                    std::string message_key,
                    std::string detail) {
    result.ok = false;
    result.refused = true;
    result.fail_closed = true;
    result.decision = "refused";
    result.diagnostic = Dpc033CleanupDiagnostic(code, message_key, detail);
    AddIndexGarbageCleanupEvidence(&result.evidence,
                                   "mga_secondary_index_garbage_cleanup_refused",
                                   result.diagnostic.code);
    return result;
  };

  if (request.index_uuid.empty() || request.table_uuid.empty()) {
    return refuse("INDEX_GARBAGE_CLEANUP.INVALID_IDENTITY",
                  "mga.secondary_index_garbage_cleanup.invalid_identity",
                  "index_uuid and table_uuid are required");
  }
  if (request.max_records_to_scan == 0 || request.max_records_to_clean == 0) {
    return refuse("INDEX_GARBAGE_CLEANUP.BUDGET_REQUIRED",
                  "mga.secondary_index_garbage_cleanup.budget_required",
                  "cleanup requires nonzero bounded scan and clean budgets");
  }

  auto loaded_state = LoadMgaRelationStoreState(context);
  if (!loaded_state.ok) {
    return refuse("INDEX_GARBAGE_CLEANUP.VALIDATION_REFUSED",
                  "mga.secondary_index_garbage_cleanup.state_load_refused",
                  loaded_state.diagnostic.detail);
  }
  RelationReadSnapshot state = BuildCrudCompatibilityStateFromMga(loaded_state.state);
  const auto selected = FindVisibleCrudIndexByUuid(
      state,
      request.table_uuid,
      request.index_uuid,
      context.local_transaction_id);
  if (!selected) {
    return refuse("INDEX_GARBAGE_CLEANUP.VALIDATION_REFUSED",
                  "mga.secondary_index_garbage_cleanup.index_not_found",
                  "requested index/table identity is not visible");
  }
  if (IsUniqueMgaIndex(*selected)) {
    return refuse("INDEX_GARBAGE_CLEANUP.UNIQUE_INDEX_REFUSED",
                  "mga.secondary_index_garbage_cleanup.unique_index_refused",
                  "unique secondary indexes remain on the synchronous path");
  }

  const auto loaded_inventory =
      LoadLocalTransactionInventoryFromDatabase(context.database_path);
  if (!loaded_inventory.ok()) {
    return refuse(loaded_inventory.diagnostic.diagnostic_code.empty()
                      ? "INDEX_GARBAGE_CLEANUP.NON_AUTHORITATIVE_REFUSAL"
                      : loaded_inventory.diagnostic.diagnostic_code,
                  loaded_inventory.diagnostic.message_key.empty()
                      ? "mga.secondary_index_garbage_cleanup.inventory_load_refused"
                      : loaded_inventory.diagnostic.message_key,
                  loaded_inventory.diagnostic.remediation_hint);
  }

  auto loaded_ledger = LoadSecondaryIndexDeltaLedgerFromPath(context);
  if (!loaded_ledger.ok) {
    return refuse("INDEX_GARBAGE_CLEANUP.VALIDATION_REFUSED",
                  "mga.secondary_index_garbage_cleanup.ledger_load_refused",
                  loaded_ledger.diagnostic.detail);
  }
  const auto recovery = idx::ClassifySecondaryIndexDeltaLedgerForRecovery(
      loaded_ledger.ledger);
  if (!recovery.ok() ||
      recovery.action == idx::SecondaryIndexDeltaLedgerRecoveryAction::refuse_open ||
      recovery.action == idx::SecondaryIndexDeltaLedgerRecoveryAction::fail_closed ||
      recovery.action == idx::SecondaryIndexDeltaLedgerRecoveryAction::rebuild_from_authoritative_base) {
    return refuse("INDEX_GARBAGE_CLEANUP.VALIDATION_REFUSED",
                  "mga.secondary_index_garbage_cleanup.recovery_refused",
                  recovery.stable_reason);
  }

  agents::IndexGarbageCleanupAgentRequest agent_request;
  agent_request.horizon_request.inventory = loaded_inventory.inventory;
  agent_request.horizon_request.inventory_authoritative =
      request.inventory_authoritative;
  agent_request.horizon_request.inventory_complete = request.inventory_complete;
  agent_request.horizon_request.active_snapshot_inventory_authoritative =
      request.active_snapshot_inventory_authoritative;
  agent_request.index_kind = idx::SecondaryIndexKind::non_unique;
  agent_request.ledger = loaded_ledger.ledger;
  agent_request.max_records_to_scan = request.max_records_to_scan;
  agent_request.max_records_to_clean = request.max_records_to_clean;
  agent_request.engine_mga_authoritative = request.engine_mga_authoritative;
  auto diagnostic = ParseLedgerTypedUuid(request.index_uuid,
                                         scratchbird::core::platform::UuidKind::object,
                                         &agent_request.index_uuid);
  if (diagnostic.error) {
    return refuse("INDEX_GARBAGE_CLEANUP.INVALID_IDENTITY",
                  "mga.secondary_index_garbage_cleanup.index_uuid_refused",
                  diagnostic.detail);
  }
  diagnostic = ParseLedgerTypedUuid(request.table_uuid,
                                    scratchbird::core::platform::UuidKind::object,
                                    &agent_request.table_uuid);
  if (diagnostic.error) {
    return refuse("INDEX_GARBAGE_CLEANUP.INVALID_IDENTITY",
                  "mga.secondary_index_garbage_cleanup.table_uuid_refused",
                  diagnostic.detail);
  }

  for (const auto& entry : state.index_entries) {
    if (entry.index_uuid != selected->index_uuid ||
        entry.table_uuid != request.table_uuid ||
        !CrudCreatorVisible(state,
                            entry.creator_tx,
                            entry.event_sequence,
                            context.local_transaction_id)) {
      continue;
    }
    idx::SecondaryIndexBaseEntry base;
    diagnostic = BaseEntryForOverlay(entry, &base);
    if (diagnostic.error) {
      return refuse("INDEX_GARBAGE_CLEANUP.VALIDATION_REFUSED",
                    "mga.secondary_index_garbage_cleanup.base_entry_refused",
                    diagnostic.detail);
    }
    base.committed_local_transaction_id = entry.creator_tx;
    agent_request.base_entries.push_back(std::move(base));
  }

  for (const auto& row :
       VisibleCrudRowsForContext(state, request.table_uuid, context)) {
    for (const auto& key : CrudIndexKeysForValues(*selected, row.values)) {
      idx::SecondaryIndexTableSnapshotEntry snapshot;
      diagnostic = Dpc033TableSnapshotEntryForCleanup(
          *selected,
          row,
          key,
          &snapshot);
      if (diagnostic.error) {
        return refuse("INDEX_GARBAGE_CLEANUP.VALIDATION_REFUSED",
                      "mga.secondary_index_garbage_cleanup.table_snapshot_refused",
                      diagnostic.detail);
      }
      agent_request.table_snapshot.push_back(std::move(snapshot));
    }
  }

  const auto agent = agents::RunIndexGarbageCleanupAgentBatch(agent_request);
  result.decision = idx::SecondaryIndexGarbageCleanupDecisionKindName(agent.decision);
  result.fail_closed = agent.fail_closed;
  result.refused = agent.fail_closed;
  result.budget_exhausted = agent.budget_exhausted;
  result.horizon_blocked = agent.horizon_blocked;
  result.validation_before_ok = agent.validation_before_ok;
  result.validation_after_ok = agent.validation_after_ok;
  result.before_delta_ledger_records = agent.before.delta_ledger_records;
  result.after_delta_ledger_records = agent.after.delta_ledger_records;
  result.cleaned_count = agent.after.cleaned_garbage_records;
  result.retained_count = agent.after.relevant_delta_records;
  result.scanned_count = agent.before.scanned_delta_records;
  if (agent.horizon.cleanup_horizon.valid()) {
    result.authoritative_cleanup_horizon_local_transaction_id =
        agent.horizon.cleanup_horizon.value;
  }
  for (const auto& field : agent.evidence) {
    AddIndexGarbageCleanupEvidence(&result.evidence,
                                   "mga_secondary_index_garbage_cleanup." + field.key,
                                   field.value);
  }
  result.diagnostic = Dpc033CleanupDiagnostic(
      agent.diagnostic.diagnostic_code,
      agent.diagnostic.message_key,
      Dpc033DiagnosticDetail(agent.diagnostic),
      agent.fail_closed);
  if (!agent.ok()) {
    AddIndexGarbageCleanupEvidence(&result.evidence,
                                   "mga_secondary_index_garbage_cleanup_refused",
                                   result.diagnostic.code);
    return result;
  }

  if (result.cleaned_count != 0) {
    const auto written = WriteSecondaryIndexDeltaLedger(context, agent.cleaned_ledger);
    if (written.error) {
      return refuse("INDEX_GARBAGE_CLEANUP.VALIDATION_REFUSED",
                    "mga.secondary_index_garbage_cleanup.ledger_replace_refused",
                    written.detail);
    }
  }

  result.ok = true;
  result.refused = false;
  result.fail_closed = false;
  AddIndexGarbageCleanupEvidence(&result.evidence,
                                 "mga_secondary_index_garbage_cleanup_result",
                                 result.decision);
  AddIndexGarbageCleanupEvidence(&result.evidence,
                                 "mga_secondary_index_garbage_cleanup_cleaned_count",
                                 std::to_string(result.cleaned_count));
  AddIndexGarbageCleanupEvidence(&result.evidence,
                                 "mga_secondary_index_garbage_cleanup_retained_count",
                                 std::to_string(result.retained_count));
  AddIndexGarbageCleanupEvidence(&result.evidence,
                                 "mga_secondary_index_garbage_cleanup_scanned_count",
                                 std::to_string(result.scanned_count));
  return result;
}

MgaIndexedRowsLookupResult IndexedMgaRowsForPredicateForContext(
    const RelationReadSnapshot& state,
    const std::string& table_uuid,
    const EnginePredicateEnvelope& predicate,
    const EngineRequestContext& context,
    std::uint64_t limit) {
  // DPC_SECONDARY_INDEX_DELTA_OVERLAY_LOOKUP: non-unique deferred secondary
  // index readers must combine persisted base entries with visible MGA deltas.
  MgaIndexedRowsLookupResult result;
  const auto selected = SelectCrudIndexForPredicate(
      state,
      table_uuid,
      predicate,
      context.local_transaction_id);
  if (!selected) {
    result.ok = true;
    result.diagnostic = OkDiagnostic();
    result.evidence.push_back({"mga_secondary_index_lookup_path",
                               "no_usable_index"});
    return result;
  }

  if (IsUniqueMgaIndex(*selected)) {
    result.rows = IndexedCrudRowsForPredicateForContext(
        state,
        table_uuid,
        predicate,
        context,
        limit,
        &result.index_evidence_id);
    result.ok = true;
    result.index_used = !result.index_evidence_id.empty();
    result.diagnostic = OkDiagnostic();
    result.evidence.push_back({"mga_secondary_index_lookup_path",
                               "unique_synchronous_bypass"});
    return result;
  }

  const auto loaded_ledger = LoadSecondaryIndexDeltaLedgerFromPath(context);
  if (!loaded_ledger.ok) {
    return RefuseIndexedLookup(
        "secondary_index_delta_ledger_load_refused",
        loaded_ledger.diagnostic);
  }

  const auto recovery =
      idx::ClassifySecondaryIndexDeltaLedgerForRecovery(loaded_ledger.ledger);
  if (!recovery.ok() ||
      recovery.recovery_class ==
          idx::SecondaryIndexDeltaLedgerRecoveryClass::repair_rebuild_required ||
      recovery.action == idx::SecondaryIndexDeltaLedgerRecoveryAction::refuse_open ||
      recovery.action == idx::SecondaryIndexDeltaLedgerRecoveryAction::fail_closed ||
      recovery.action == idx::SecondaryIndexDeltaLedgerRecoveryAction::rebuild_from_authoritative_base) {
    return RefuseIndexedLookup(
        "secondary_index_delta_ledger_recovery_refused",
        OverlayLookupDiagnostic(recovery.diagnostic,
                                "SB-MGA-SECONDARY-DELTA-RECOVERY-REFUSED",
                                "mga.secondary_index_delta_ledger.recovery_refused"));
  }

  bool has_relevant_delta = false;
  idx::SecondaryIndexDeltaLedger overlay_ledger;
  overlay_ledger.deltas.reserve(loaded_ledger.ledger.records.size());
  for (const auto& record : loaded_ledger.ledger.records) {
    if (!LedgerRecordRelevantToIndex(record, *selected, table_uuid)) {
      continue;
    }
    if (record.commit_state ==
            idx::SecondaryIndexDeltaLedgerCommitState::repair_rebuild_required ||
        record.commit_state == idx::SecondaryIndexDeltaLedgerCommitState::refused) {
      return RefuseIndexedLookup(
          "secondary_index_delta_ledger_record_refused");
    }
    if (record.commit_state ==
        idx::SecondaryIndexDeltaLedgerCommitState::merged_cleaned) {
      continue;
    }
    auto visible_delta = record.delta;
    if (record.commit_state ==
            idx::SecondaryIndexDeltaLedgerCommitState::precommit_uncommitted &&
        record.delta.local_transaction_id != context.local_transaction_id) {
      const auto transaction = state.transactions.find(
          record.delta.local_transaction_id);
      if (transaction == state.transactions.end() ||
          (transaction->second != "committed" &&
           transaction->second != "archived")) {
        continue;
      }
      // The durable transaction inventory is finality authority.  The
      // precommit marker is recoverable classification evidence and does not
      // require a correctness-critical post-inventory rewrite.
      visible_delta.committed = true;
    }
    overlay_ledger.deltas.push_back(std::move(visible_delta));
    has_relevant_delta = true;
  }

  if (!has_relevant_delta) {
    result.rows = IndexedCrudRowsForPredicateForContext(
        state,
        table_uuid,
        predicate,
        context,
        limit,
        &result.index_evidence_id);
    result.ok = true;
    result.index_used = !result.index_evidence_id.empty();
    result.diagnostic = OkDiagnostic();
    result.evidence.push_back({"mga_secondary_index_lookup_path",
                               "non_unique_synchronous_no_delta"});
    return result;
  }

  if (!OverlayPredicateSupported(predicate)) {
    return RefuseIndexedLookup(
        "secondary_index_delta_overlay_predicate_unsupported");
  }

  std::vector<idx::SecondaryIndexBaseEntry> base_entries;
  for (const auto& entry : state.index_entries) {
    if (entry.index_uuid != selected->index_uuid ||
        entry.table_uuid != table_uuid ||
        !CrudCreatorVisible(state,
                            entry.creator_tx,
                            entry.event_sequence,
                            context.local_transaction_id)) {
      continue;
    }
    idx::SecondaryIndexBaseEntry base;
    const auto diagnostic = BaseEntryForOverlay(entry, &base);
    if (diagnostic.error) {
      return RefuseIndexedLookup("secondary_index_base_entry_invalid",
                                 diagnostic);
    }
    base_entries.push_back(std::move(base));
  }

  idx::SecondaryIndexOverlayRequest overlay_request;
  auto diagnostic = ParseLedgerTypedUuid(selected->index_uuid,
                                         scratchbird::core::platform::UuidKind::object,
                                         &overlay_request.index_uuid);
  if (diagnostic.error) {
    return RefuseIndexedLookup("secondary_index_overlay_index_uuid_invalid",
                               diagnostic);
  }
  diagnostic = ParseLedgerTypedUuid(table_uuid,
                                    scratchbird::core::platform::UuidKind::object,
                                    &overlay_request.table_uuid);
  if (diagnostic.error) {
    return RefuseIndexedLookup("secondary_index_overlay_table_uuid_invalid",
                               diagnostic);
  }
  diagnostic = ParseLedgerTypedUuid(context.transaction_uuid.canonical,
                                    scratchbird::core::platform::UuidKind::transaction,
                                    &overlay_request.transaction_uuid);
  if (diagnostic.error) {
    return RefuseIndexedLookup("secondary_index_overlay_transaction_uuid_invalid",
                               diagnostic);
  }
  overlay_request.local_transaction_id = context.local_transaction_id;
  overlay_request.snapshot_high_water_local_transaction_id =
      SnapshotVisibleThroughForOverlay(state, context);
  overlay_request.index_kind = SecondaryIndexKindForCrudIndex(*selected);
  overlay_request.include_own_transaction = true;

  idx::SecondaryIndexOverlayLedger evidence_ledger;
  const auto overlay = idx::BuildSecondaryIndexDeltaOverlay(
      &evidence_ledger,
      base_entries,
      overlay_ledger,
      overlay_request);
  if (!overlay.ok()) {
    return RefuseIndexedLookup(
        "secondary_index_delta_overlay_refused",
        OverlayLookupDiagnostic(overlay.diagnostic,
                                "SB-MGA-SECONDARY-OVERLAY-REFUSED",
                                "mga.secondary_index_delta_overlay.refused"));
  }

  std::set<std::string> seen_candidates;
  std::size_t candidate_count = 0;
  for (const auto& entry : overlay.entries) {
    if (!OverlayEntryMatchesPredicate(entry, predicate)) {
      continue;
    }
    const std::string row_uuid =
        scratchbird::core::uuid::UuidToString(entry.row_uuid.value);
    if (!seen_candidates.insert(row_uuid).second) {
      continue;
    }
    ++candidate_count;
    const auto row = FindVisibleCrudRowForContext(
        state,
        table_uuid,
        row_uuid,
        context);
    if (row && CrudRowMatchesPredicate(*row, predicate)) {
      result.rows.push_back(*row);
      if (limit != 0 && result.rows.size() >= limit) {
        break;
      }
    }
  }
  result.index_evidence_id = CrudIndexEvidenceId(
      *selected,
      predicate,
      candidate_count,
      result.rows.size());
  result.ok = true;
  result.index_used = true;
  result.diagnostic = OkDiagnostic();
  result.evidence.push_back({"mga_secondary_index_lookup_path",
                             "non_unique_committed_delta_overlay"});
  result.evidence.push_back({"mga_secondary_index_delta_overlay_used",
                             selected->index_uuid});
  result.evidence.push_back({"mga_secondary_index_delta_overlay_visible_delta_count",
                             std::to_string(overlay.evidence.visible_delta_entries)});
  result.evidence.push_back({"mga_secondary_index_delta_overlay_result_count",
                             std::to_string(result.rows.size())});
  return result;
}


}  // namespace scratchbird::engine::internal_api
