// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "mga_relation_store/mga_relation_store.hpp"

#include "api_diagnostics.hpp"
#include "agents/index_garbage_cleanup_agent.hpp"
#include "ipar_fault_injection.hpp"
#include "local_transaction_store.hpp"
#include "secondary_index_delta_merge.hpp"
#include "transaction_inventory.hpp"
#include "transaction_state.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

namespace agents = scratchbird::core::agents::implemented_agents;
namespace idx = scratchbird::core::index;

constexpr const char* kRowStoreMagic = "SBMGA1";
constexpr const char* kDescriptorMagic = "SBMGADESC1";
constexpr const char* kEventSequenceAllocatorMagic = "SBMGAEVSEQ1";
constexpr std::size_t kMgaLargeValueChunkBytes = 2048;

using scratchbird::storage::database::LoadLocalTransactionInventoryFromDatabase;
using scratchbird::transaction::mga::LookupLocalTransaction;
using scratchbird::transaction::mga::MakeLocalTransactionId;
using scratchbird::transaction::mga::TransactionState;

std::string RowStorePath(const EngineRequestContext& context) {
  return context.database_path + ".sb.mga_row_versions";
}

std::string MetadataStorePath(const EngineRequestContext& context) {
  return context.database_path + ".sb.mga_relation_metadata";
}

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

std::string ScopedIndexStorePath(const EngineRequestContext& context,
                                 const std::string& table_uuid) {
  return ScopedRelationStoreRoot(context) + "/" +
         ScopedRelationSegmentName(table_uuid) + ".indexes";
}

std::string SecondaryIndexDeltaLedgerStorePath(const EngineRequestContext& context) {
  return context.database_path + ".sb.mga_secondary_index_delta_ledger";
}

std::string EventSequenceAllocatorStorePath(const EngineRequestContext& context) {
  return context.database_path + ".sb.mga_event_sequence_allocator";
}

std::string DescriptorStorePath(const EngineRequestContext& context) {
  return context.database_path + ".sb.mga_relation_descriptors";
}

std::string LargeValueStorePath(const EngineRequestContext& context) {
  return context.database_path + ".sb.mga_large_values";
}

std::string SavepointStorePath(const EngineRequestContext& context) {
  return context.database_path + ".sb.mga_savepoints";
}

EngineApiDiagnostic OkDiagnostic() {
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

std::vector<std::string> SplitTabs(const std::string& line) {
  std::vector<std::string> fields;
  std::size_t start = 0;
  while (start <= line.size()) {
    const auto tab = line.find('\t', start);
    if (tab == std::string::npos) {
      fields.push_back(line.substr(start));
      break;
    }
    fields.push_back(line.substr(start, tab - start));
    start = tab + 1;
  }
  return fields;
}

bool AppendLine(const std::string& path, const std::string& line) {
  if (path.empty()) { return false; }
  std::ofstream out(path, std::ios::app | std::ios::binary);
  if (!out) { return false; }
  out << line << '\n';
  out.flush();
  return static_cast<bool>(out);
}

bool AppendLines(const std::string& path,
                 const std::vector<std::string>& lines,
                 std::uint64_t* stream_opens,
                 std::uint64_t* stream_flushes) {
  if (lines.empty()) { return true; }
  if (path.empty()) { return false; }
  std::ofstream out(path, std::ios::app | std::ios::binary);
  if (!out) { return false; }
  if (stream_opens != nullptr) { ++(*stream_opens); }
  for (const auto& line : lines) {
    out << line << '\n';
  }
  out.flush();
  if (stream_flushes != nullptr) { ++(*stream_flushes); }
  return static_cast<bool>(out);
}

bool AppendScopedRelationLine(const std::string& path, const std::string& line) {
  if (path.empty()) { return false; }
  std::error_code ignored;
  std::filesystem::create_directories(std::filesystem::path(path).parent_path(),
                                      ignored);
  return AppendLine(path, line);
}

EngineApiDiagnostic AppendScopedRelationBufferedLine(
    std::map<std::string, std::ofstream>* streams,
    std::set<std::string>* dirty_paths,
    const std::string& path,
    const std::string& line,
    std::string_view operation_id,
    std::string_view failure_reason) {
  if (streams == nullptr || dirty_paths == nullptr || path.empty()) {
    return MakeInvalidRequestDiagnostic(std::string(operation_id),
                                        std::string(failure_reason));
  }
  auto it = streams->find(path);
  if (it == streams->end()) {
    std::error_code ignored;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(),
                                        ignored);
    auto [inserted, _] = streams->emplace(path, std::ofstream{});
    it = inserted;
    it->second.open(path, std::ios::app | std::ios::binary);
    if (!it->second) {
      return MakeInvalidRequestDiagnostic(std::string(operation_id),
                                          std::string(failure_reason));
    }
  }
  it->second << line << '\n';
  if (!it->second) {
    return MakeInvalidRequestDiagnostic(std::string(operation_id),
                                        std::string(failure_reason));
  }
  dirty_paths->insert(path);
  return OkDiagnostic();
}

EngineApiDiagnostic FlushScopedRelationBufferedLines(
    std::map<std::string, std::ofstream>* streams,
    std::set<std::string>* dirty_paths,
    std::string_view operation_id,
    std::string_view failure_reason) {
  if (streams == nullptr || dirty_paths == nullptr) {
    return MakeInvalidRequestDiagnostic(std::string(operation_id),
                                        std::string(failure_reason));
  }
  for (const auto& path : *dirty_paths) {
    auto it = streams->find(path);
    if (it == streams->end() || !it->second.is_open()) {
      return MakeInvalidRequestDiagnostic(std::string(operation_id),
                                          std::string(failure_reason));
    }
    it->second.flush();
    if (!it->second) {
      return MakeInvalidRequestDiagnostic(std::string(operation_id),
                                          std::string(failure_reason));
    }
  }
  dirty_paths->clear();
  return OkDiagnostic();
}

std::vector<std::string> ReadLines(const std::string& path) {
  std::vector<std::string> lines;
  std::ifstream in(path, std::ios::binary);
  if (!in) { return lines; }
  std::string line;
  while (std::getline(in, line)) { lines.push_back(line); }
  return lines;
}

bool FileExistsAndNotEmpty(const std::string& path) {
  std::error_code ignored;
  return std::filesystem::exists(path, ignored) &&
         std::filesystem::file_size(path, ignored) != 0;
}

std::vector<idx::byte> ReadBinaryFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) { return {}; }
  std::vector<idx::byte> bytes;
  const auto begin = std::istreambuf_iterator<char>(in);
  const auto end = std::istreambuf_iterator<char>();
  for (auto it = begin; it != end; ++it) {
    bytes.push_back(static_cast<idx::byte>(*it));
  }
  return bytes;
}

std::string JoinLine(const std::vector<std::string>& fields) {
  std::string line;
  for (std::size_t i = 0; i < fields.size(); ++i) {
    if (i != 0) { line += '\t'; }
    line += fields[i];
  }
  return line;
}

std::uint64_t ParseU64(const std::string& text, std::uint64_t fallback = 0) {
  if (text.empty()) { return fallback; }
  try {
    return static_cast<std::uint64_t>(std::stoull(text));
  } catch (...) {
    return fallback;
  }
}

std::mutex& EventSequenceCacheMutex() {
  static std::mutex mutex;
  return mutex;
}

std::map<std::string, std::uint64_t>& EventSequenceCache() {
  static std::map<std::string, std::uint64_t> cache;
  return cache;
}

std::string EventSequenceStreamKey(const std::string& stream_kind,
                                   const std::string& stream_path) {
  return stream_kind + "\n" + stream_path;
}

struct DurableEventSequenceState {
  bool found = false;
  std::uint64_t next = 0;
};

DurableEventSequenceState LoadDurableEventSequenceState(
    const EngineRequestContext& context,
    const std::string& stream_kind,
    const std::string& stream_path) {
  DurableEventSequenceState state;
  const std::string encoded_stream_path = EncodeCrudText(stream_path);
  for (const auto& line : ReadLines(EventSequenceAllocatorStorePath(context))) {
    const auto fields = SplitTabs(line);
    if (fields.size() < 7 ||
        fields[0] != kEventSequenceAllocatorMagic ||
        fields[1] != "RANGE" ||
        fields[2] != stream_kind ||
        fields[3] != encoded_stream_path) {
      continue;
    }
    const std::uint64_t next = ParseU64(fields[6]);
    if (next != 0) {
      state.found = true;
      state.next = next;
    }
  }
  return state;
}

MgaEventSequenceRangeReservation RefuseEventSequenceReservation(
    const EngineRequestContext& context,
    const std::string& stream_kind,
    const std::string& stream_path,
    const std::string& reason) {
  MgaEventSequenceRangeReservation reservation;
  reservation.allocator_path = EventSequenceAllocatorStorePath(context);
  reservation.stream_kind = stream_kind;
  reservation.stream_path = stream_path;
  reservation.diagnostic = MakeInvalidRequestDiagnostic("mga.event_sequence_allocator",
                                                        reason);
  return reservation;
}

template <typename Loader>
MgaEventSequenceRangeReservation ReserveEventSequenceRange(
    const EngineRequestContext& context,
    const std::string& stream_kind,
    const std::string& stream_path,
    std::uint64_t count,
    Loader loader) {
  if (context.database_path.empty()) {
    return RefuseEventSequenceReservation(context,
                                          stream_kind,
                                          stream_path,
                                          "database_path_required");
  }
  if (stream_kind.empty() || stream_path.empty()) {
    return RefuseEventSequenceReservation(context,
                                          stream_kind,
                                          stream_path,
                                          "stream_identity_required");
  }
  const std::uint64_t normalized_count = count == 0 ? 1 : count;
  const std::lock_guard<std::mutex> guard(EventSequenceCacheMutex());
  const std::string cache_key = EventSequenceStreamKey(stream_kind, stream_path);
  auto cache_it = EventSequenceCache().find(cache_key);
  std::uint64_t next_sequence = 0;
  bool bootstrapped = false;
  std::string route = "process_cache_after_durable_reservation";
  if (cache_it != EventSequenceCache().end() && cache_it->second != 0) {
    next_sequence = cache_it->second;
  } else {
    const auto durable = LoadDurableEventSequenceState(context, stream_kind, stream_path);
    if (durable.found) {
      next_sequence = durable.next;
      route = "durable_allocator_state";
    } else {
      next_sequence = loader();
      bootstrapped = true;
      route = "bootstrap_store_scan";
    }
  }
  if (next_sequence == 0) {
    next_sequence = 1;
  }
  if (normalized_count > std::numeric_limits<std::uint64_t>::max() - next_sequence) {
    return RefuseEventSequenceReservation(context,
                                          stream_kind,
                                          stream_path,
                                          "event_sequence_range_overflow");
  }
  const std::uint64_t first = next_sequence;
  const std::uint64_t next = next_sequence + normalized_count;
  const std::string allocator_path = EventSequenceAllocatorStorePath(context);
  const std::string line = JoinLine({kEventSequenceAllocatorMagic,
                                     "RANGE",
                                     stream_kind,
                                     EncodeCrudText(stream_path),
                                     std::to_string(first),
                                     std::to_string(normalized_count),
                                     std::to_string(next),
                                     route,
                                     bootstrapped ? "1" : "0"});
  if (!AppendLine(allocator_path, line)) {
    return RefuseEventSequenceReservation(context,
                                          stream_kind,
                                          stream_path,
                                          "durable_allocator_append_failed");
  }
  EventSequenceCache()[cache_key] = next;

  MgaEventSequenceRangeReservation reservation;
  reservation.ok = true;
  reservation.diagnostic = OkDiagnostic();
  reservation.allocator_path = allocator_path;
  reservation.stream_kind = stream_kind;
  reservation.stream_path = stream_path;
  reservation.first = first;
  reservation.count = normalized_count;
  reservation.next = next;
  reservation.bootstrapped_from_store = bootstrapped;
  return reservation;
}

std::uint64_t ScanNextRowEventSequence(const EngineRequestContext& context) {
  std::uint64_t max_sequence = 0;
  for (const auto& line : ReadLines(RowStorePath(context))) {
    const auto fields = SplitTabs(line);
    if (fields.size() >= 4 && fields[0] == kRowStoreMagic && fields[1] == "ROW_VERSION") {
      max_sequence = std::max(max_sequence, ParseU64(fields[3]));
    }
  }
  return max_sequence + 1;
}

std::uint64_t NextRowEventSequence(const EngineRequestContext& context) {
  return ScanNextRowEventSequence(context);
}

std::uint64_t ScanNextIndexEventSequence(const EngineRequestContext& context) {
  std::uint64_t max_sequence = 0;
  for (const auto& line : ReadLines(IndexStorePath(context))) {
    const auto fields = SplitTabs(line);
    if (fields.size() >= 4 && fields[0] == kRowStoreMagic && fields[1] == "INDEX_ENTRY") {
      max_sequence = std::max(max_sequence, ParseU64(fields[3]));
    }
  }
  return max_sequence + 1;
}

std::uint64_t NextIndexEventSequence(const EngineRequestContext& context) {
  return ScanNextIndexEventSequence(context);
}

std::uint64_t ScanNextMetadataEventSequence(const EngineRequestContext& context) {
  std::uint64_t max_sequence = 0;
  for (const auto& line : ReadLines(MetadataStorePath(context))) {
    const auto fields = SplitTabs(line);
    if (fields.size() >= 4 && fields[0] == kRowStoreMagic &&
        (fields[1] == "TABLE_METADATA" || fields[1] == "INDEX_METADATA")) {
      max_sequence = std::max(max_sequence, ParseU64(fields[3]));
    }
  }
  return max_sequence + 1;
}

std::uint64_t NextMetadataEventSequence(const EngineRequestContext& context) {
  return ScanNextMetadataEventSequence(context);
}

std::uint64_t ChecksumText(const std::string& value) {
  std::uint64_t checksum = 1469598103934665603ull;
  for (unsigned char c : value) {
    checksum ^= static_cast<std::uint64_t>(c);
    checksum *= 1099511628211ull;
  }
  return checksum;
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

std::uint64_t MaxCommittedLocalTransactionId(const CrudState& state) {
  std::uint64_t max_committed = 0;
  for (const auto& [tx, status] : state.transactions) {
    if (status == "committed" || status == "archived") {
      max_committed = std::max(max_committed, tx);
    }
  }
  return max_committed;
}

std::uint64_t SnapshotVisibleThroughForOverlay(const CrudState& state,
                                               const EngineRequestContext& context) {
  const std::string isolation = context.transaction_isolation_level.empty()
                                    ? std::string("read_committed")
                                    : context.transaction_isolation_level;
  if ((isolation == "snapshot" || isolation == "repeatable_read" ||
       isolation == "serializable") &&
      context.snapshot_visible_through_local_transaction_id != 0) {
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

std::optional<CrudIndexRecord> SelectCrudIndexForPredicate(const CrudState& state,
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

bool Dpc025PublishedBaseContainsRecord(const CrudState& state,
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

std::optional<CrudIndexRecord> FindVisibleCrudIndexByUuid(const CrudState& state,
                                                          const std::string& table_uuid,
                                                          const std::string& index_uuid,
                                                          std::uint64_t observer_tx) {
  for (const auto& index : VisibleCrudIndexesForTable(state, table_uuid, observer_tx)) {
    if (index.index_uuid == index_uuid) { return index; }
  }
  return std::nullopt;
}

bool LedgerRecordBelongsToUniqueIndex(const idx::SecondaryIndexDeltaLedgerRecord& record,
                                      const CrudState& state) {
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
    const CrudState& state,
    const CrudIndexRecord& index,
    const std::string& table_uuid,
    const std::vector<idx::SecondaryIndexBaseEntry>& base_entries) {
  const std::string path = IndexStorePath(context);
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

int HexValue(char c) {
  if (c >= '0' && c <= '9') { return c - '0'; }
  if (c >= 'a' && c <= 'f') { return 10 + (c - 'a'); }
  if (c >= 'A' && c <= 'F') { return 10 + (c - 'A'); }
  return -1;
}

std::string DecodeCrudTextLocal(const std::string& encoded) {
  std::string decoded;
  if ((encoded.size() % 2) != 0) { return decoded; }
  decoded.reserve(encoded.size() / 2);
  for (std::size_t i = 0; i < encoded.size(); i += 2) {
    const int hi = HexValue(encoded[i]);
    const int lo = HexValue(encoded[i + 1]);
    if (hi < 0 || lo < 0) { return {}; }
    decoded.push_back(static_cast<char>((hi << 4) | lo));
  }
  return decoded;
}

std::string MakeMgaLargeValueLocator(const std::string& overflow_uuid,
                                     const std::string& content_hash,
                                     std::uint64_t total_bytes) {
  return "SBMGA_LARGE_VALUE:" + overflow_uuid + ":" + content_hash + ":" + std::to_string(total_bytes);
}

bool StartsWith(const std::string& value, const std::string& prefix) {
  return value.rfind(prefix, 0) == 0;
}

bool IsMgaLargeValueLocator(const std::string& value) {
  return StartsWith(value, "SBMGA_LARGE_VALUE:");
}

EngineApiDiagnostic OverlayMgaTransactionAuthority(const EngineRequestContext& context,
                                                   CrudState* state);

struct LargeValueRecord {
  std::uint64_t total_bytes = 0;
  std::string content_hash;
  std::map<std::uint64_t, std::string> chunks;
};

struct LargeValueLoadResult {
  EngineApiDiagnostic diagnostic = OkDiagnostic();
  std::map<std::string, std::string> locator_payloads;
  std::set<std::string> reclaimed_locators;
};

struct LargeValueReclaimLoadResult {
  EngineApiDiagnostic diagnostic = OkDiagnostic();
  std::set<std::string> overflow_uuids;
};

LargeValueReclaimLoadResult LoadVisibleMgaLargeValueReclaims(
    const EngineRequestContext& context) {
  LargeValueReclaimLoadResult result;
  CrudState transaction_state;
  const auto authority = OverlayMgaTransactionAuthority(context, &transaction_state);
  if (authority.error) {
    result.diagnostic = authority;
    return result;
  }
  for (const auto& line : ReadLines(LargeValueStorePath(context))) {
    const auto fields = SplitTabs(line);
    if (fields.size() < 4 || fields[0] != kRowStoreMagic ||
        fields[1] != "LARGE_VALUE_RECLAIMED") {
      continue;
    }
    const std::uint64_t creator_tx = ParseU64(fields[2]);
    if (CrudCreatorVisible(transaction_state,
                           creator_tx,
                           0,
                           context.local_transaction_id)) {
      result.overflow_uuids.insert(fields[3]);
    }
  }
  return result;
}

LargeValueLoadResult LoadMgaLargeValuePayloads(const EngineRequestContext& context) {
  LargeValueLoadResult result;
  const auto reclaimed = LoadVisibleMgaLargeValueReclaims(context);
  if (reclaimed.diagnostic.error) {
    result.diagnostic = reclaimed.diagnostic;
    return result;
  }
  std::map<std::string, LargeValueRecord> records;
  for (const auto& line : ReadLines(LargeValueStorePath(context))) {
    const auto fields = SplitTabs(line);
    if (fields.size() < 2 || fields[0] != kRowStoreMagic) { continue; }
    if (fields[1] == "LARGE_VALUE" && fields.size() >= 11) {
      auto& record = records[fields[3]];
      record.total_bytes = ParseU64(fields[8]);
      record.content_hash = fields[9];
    } else if (fields[1] == "LARGE_VALUE_CHUNK" && fields.size() >= 7) {
      const std::string overflow_uuid = fields[3];
      const std::uint64_t ordinal = ParseU64(fields[4]);
      const std::string fragment = DecodeCrudTextLocal(fields[5]);
      const std::uint64_t expected_checksum = ParseU64(fields[6]);
      if (ChecksumText(fragment) != expected_checksum) {
        result.diagnostic = MakeInvalidRequestDiagnostic("mga.large_value", "large_value_chunk_checksum_mismatch");
        return result;
      }
      records[overflow_uuid].chunks[ordinal] = fragment;
    }
  }
  for (const auto& [overflow_uuid, record] : records) {
    const std::string locator =
        MakeMgaLargeValueLocator(overflow_uuid, record.content_hash, record.total_bytes);
    if (reclaimed.overflow_uuids.count(overflow_uuid) != 0) {
      result.reclaimed_locators.insert(locator);
      continue;
    }
    std::string payload;
    for (const auto& [ordinal, fragment] : record.chunks) {
      (void)ordinal;
      payload += fragment;
    }
    if (payload.size() != record.total_bytes ||
        std::to_string(ChecksumText(payload)) != record.content_hash) {
      result.diagnostic = MakeInvalidRequestDiagnostic("mga.large_value", "large_value_payload_checksum_mismatch");
      return result;
    }
    result.locator_payloads[locator] = payload;
  }
  return result;
}

EngineApiDiagnostic ExpandMgaLargeValueLocators(const EngineRequestContext& context,
                                                std::vector<CrudRowVersionRecord>* rows) {
  const auto payloads = LoadMgaLargeValuePayloads(context);
  if (payloads.diagnostic.error) { return payloads.diagnostic; }
  for (auto& row : *rows) {
    for (auto& [field, value] : row.values) {
      (void)field;
      if (!IsMgaLargeValueLocator(value)) { continue; }
      const auto payload_it = payloads.locator_payloads.find(value);
      if (payload_it == payloads.locator_payloads.end()) {
        if (payloads.reclaimed_locators.count(value) != 0) { continue; }
        return MakeInvalidRequestDiagnostic("mga.large_value", "large_value_locator_missing");
      }
      value = payload_it->second;
    }
  }
  return OkDiagnostic();
}

struct SavepointCutoffs {
  std::uint64_t row_event_sequence = 0;
  std::uint64_t metadata_event_sequence = 0;
  std::uint64_t index_event_sequence = 0;
};

struct SavepointRollbackRange {
  SavepointCutoffs cutoffs;
  std::uint64_t row_upper_event_sequence = std::numeric_limits<std::uint64_t>::max();
  std::uint64_t metadata_upper_event_sequence = std::numeric_limits<std::uint64_t>::max();
  std::uint64_t index_upper_event_sequence = std::numeric_limits<std::uint64_t>::max();
};

struct SavepointParsedState {
  std::map<std::uint64_t, std::map<std::string, SavepointCutoffs>> active_savepoints;
  std::map<std::uint64_t, std::vector<SavepointRollbackRange>> rollback_ranges;
};

SavepointParsedState ParseSavepoints(const EngineRequestContext& context) {
  SavepointParsedState state;
  for (const auto& line : ReadLines(SavepointStorePath(context))) {
    const auto fields = SplitTabs(line);
    if (fields.size() < 5 || fields[0] != kRowStoreMagic) { continue; }
    const std::string& kind = fields[1];
    const std::uint64_t tx = ParseU64(fields[2]);
    const std::string name = DecodeCrudTextLocal(fields[3]);
    SavepointCutoffs cutoffs;
    cutoffs.row_event_sequence = ParseU64(fields[4]);
    cutoffs.metadata_event_sequence = fields.size() >= 6 ? ParseU64(fields[5]) : cutoffs.row_event_sequence;
    cutoffs.index_event_sequence = fields.size() >= 7 ? ParseU64(fields[6]) : cutoffs.row_event_sequence;
    if (kind == "SAVEPOINT") {
      state.active_savepoints[tx][name] = cutoffs;
    } else if (kind == "RELEASE_SAVEPOINT") {
      const auto tx_it = state.active_savepoints.find(tx);
      if (tx_it != state.active_savepoints.end()) { tx_it->second.erase(name); }
    } else if (kind == "ROLLBACK_TO_SAVEPOINT") {
      SavepointRollbackRange range;
      range.cutoffs = cutoffs;
      if (fields.size() >= 10) {
        range.row_upper_event_sequence = ParseU64(fields[7]);
        range.metadata_upper_event_sequence = ParseU64(fields[8]);
        range.index_upper_event_sequence = ParseU64(fields[9]);
      }
      state.rollback_ranges[tx].push_back(range);
    }
  }
  return state;
}

bool RowEventRolledBackBySavepoint(const SavepointParsedState& savepoints,
                                   std::uint64_t creator_tx,
                                   std::uint64_t event_sequence) {
  const auto ranges_it = savepoints.rollback_ranges.find(creator_tx);
  if (ranges_it == savepoints.rollback_ranges.end()) {
    return false;
  }
  for (const auto& range : ranges_it->second) {
    if (event_sequence > range.cutoffs.row_event_sequence &&
        event_sequence <= range.row_upper_event_sequence) {
      return true;
    }
  }
  return false;
}

bool MetadataEventRolledBackBySavepoint(const SavepointParsedState& savepoints,
                                        std::uint64_t creator_tx,
                                        std::uint64_t event_sequence) {
  const auto ranges_it = savepoints.rollback_ranges.find(creator_tx);
  if (ranges_it == savepoints.rollback_ranges.end()) {
    return false;
  }
  for (const auto& range : ranges_it->second) {
    if (event_sequence > range.cutoffs.metadata_event_sequence &&
        event_sequence <= range.metadata_upper_event_sequence) {
      return true;
    }
  }
  return false;
}

bool IndexEventRolledBackBySavepoint(const SavepointParsedState& savepoints,
                                     std::uint64_t creator_tx,
                                     std::uint64_t event_sequence) {
  const auto ranges_it = savepoints.rollback_ranges.find(creator_tx);
  if (ranges_it == savepoints.rollback_ranges.end()) {
    return false;
  }
  for (const auto& range : ranges_it->second) {
    if (event_sequence > range.cutoffs.index_event_sequence &&
        event_sequence <= range.index_upper_event_sequence) {
      return true;
    }
  }
  return false;
}

std::map<std::string, std::vector<std::pair<std::string, std::string>>> LoadDescriptorFieldsByRelation(
    const EngineRequestContext& context) {
  std::map<std::string, std::vector<std::pair<std::string, std::string>>> descriptors;
  for (const auto& line : ReadLines(DescriptorStorePath(context))) {
    const auto fields = SplitTabs(line);
    if (fields.size() < 4 || fields[0] != kDescriptorMagic || fields[1] != "RELATION") { continue; }
    descriptors[fields[2]] = DecodeCrudPairs(fields[3]);
  }
  return descriptors;
}

EngineApiDiagnostic PersistDescriptorFields(const EngineRequestContext& context,
                                            const std::string& relation_uuid,
                                            const std::vector<std::pair<std::string, std::string>>& fields) {
  if (context.database_path.empty()) {
    return MakeInvalidRequestDiagnostic("mga.relation_descriptor", "database_path_required");
  }
  const std::string line = JoinLine({kDescriptorMagic, "RELATION", relation_uuid, EncodeCrudPairs(fields)});
  if (!AppendLine(DescriptorStorePath(context), line)) {
    return MakeInvalidRequestDiagnostic("mga.relation_descriptor", "descriptor_store_append_failed");
  }
  return OkDiagnostic();
}

std::string MgaTransactionStateName(TransactionState state) {
  switch (state) {
    case TransactionState::active: return "active";
    case TransactionState::read_only_active: return "read_only_active";
    case TransactionState::preparing: return "preparing";
    case TransactionState::prepared: return "prepared";
    case TransactionState::committing: return "committing";
    case TransactionState::committed: return "committed";
    case TransactionState::rolling_back: return "rolling_back";
    case TransactionState::rolled_back: return "rolled_back";
    case TransactionState::limbo: return "limbo";
    case TransactionState::recovering: return "recovering";
    case TransactionState::failed_terminal: return "failed_terminal";
    case TransactionState::archived: return "archived";
    case TransactionState::none:
    case TransactionState::created:
    default: return "none";
  }
}

EngineApiDiagnostic OverlayMgaTransactionAuthority(const EngineRequestContext& context, CrudState* state) {
  if (state == nullptr) {
    return MakeInvalidRequestDiagnostic("mga.transaction_authority", "state_required");
  }
  if (context.database_path.empty()) {
    return MakeInvalidRequestDiagnostic("mga.transaction_authority", "database_path_required");
  }
  const auto loaded = LoadLocalTransactionInventoryFromDatabase(context.database_path);
  if (!loaded.ok()) {
    return MakeEngineApiDiagnostic(loaded.diagnostic.diagnostic_code.empty() ? "SB-MGA-TXN-INV-LOAD-FAILED" : loaded.diagnostic.diagnostic_code,
                                   loaded.diagnostic.message_key.empty() ? "mga.transaction_inventory.load_failed" : loaded.diagnostic.message_key,
                                   loaded.diagnostic.remediation_hint,
                                   true);
  }
  for (const auto& entry : loaded.inventory.entries) {
    if (!entry.identity.local_id.valid()) { continue; }
    state->transactions[entry.identity.local_id.value] = MgaTransactionStateName(entry.state);
    state->max_transaction_id = std::max(state->max_transaction_id, entry.identity.local_id.value);
  }
  if (context.local_transaction_id != 0) {
    const auto lookup = LookupLocalTransaction(loaded.inventory, MakeLocalTransactionId(context.local_transaction_id));
    if (!lookup.ok()) {
      return MakeEngineApiDiagnostic(lookup.diagnostic.diagnostic_code.empty() ? "SB-MGA-TXN-INV-LOOKUP-FAILED" : lookup.diagnostic.diagnostic_code,
                                     lookup.diagnostic.message_key.empty() ? "mga.transaction_inventory.lookup_failed" : lookup.diagnostic.message_key,
                                     lookup.diagnostic.remediation_hint,
                                     true);
    }
    if (lookup.entry.state != TransactionState::active) {
      return MakeInvalidRequestDiagnostic("mga.transaction_authority", "active_local_transaction_required");
    }
  }
  return OkDiagnostic();
}

std::set<std::string> VisibleRetiredTemporaryTableMetadata(
    const EngineRequestContext& context,
    const CrudState& state) {
  std::set<std::string> retired_tables;
  for (const auto& line : ReadLines(MetadataStorePath(context))) {
    const auto fields = SplitTabs(line);
    if (fields.size() < 7 || fields[0] != kRowStoreMagic ||
        fields[1] != "TABLE_METADATA_RETIRED") {
      continue;
    }
    const std::uint64_t creator_tx = ParseU64(fields[2]);
    const std::uint64_t event_sequence = ParseU64(fields[3]);
    const std::string& table_uuid = fields[4];
    const std::string& session_uuid = fields[6];
    if (!session_uuid.empty() &&
        session_uuid != context.session_uuid.canonical) {
      continue;
    }
    if (CrudCreatorVisible(state,
                           creator_tx,
                           event_sequence,
                           context.local_transaction_id)) {
      retired_tables.insert(table_uuid);
    }
  }
  return retired_tables;
}

void FilterVisibleRetiredTemporaryMetadata(const EngineRequestContext& context,
                                           CrudState* state) {
  if (state == nullptr) { return; }
  const auto retired_tables =
      VisibleRetiredTemporaryTableMetadata(context, *state);
  if (retired_tables.empty()) { return; }
  state->tables.erase(std::remove_if(state->tables.begin(),
                                     state->tables.end(),
                                     [&retired_tables](const CrudTableRecord& table) {
                                       return retired_tables.count(table.table_uuid) != 0;
                                     }),
                      state->tables.end());
  state->indexes.erase(std::remove_if(state->indexes.begin(),
                                      state->indexes.end(),
                                      [&retired_tables](const CrudIndexRecord& index) {
                                        return retired_tables.count(index.table_uuid) != 0;
                                      }),
                       state->indexes.end());
  state->row_versions.erase(std::remove_if(state->row_versions.begin(),
                                           state->row_versions.end(),
                                           [&retired_tables](const CrudRowVersionRecord& row) {
                                             return retired_tables.count(row.table_uuid) != 0;
                                           }),
                            state->row_versions.end());
  state->index_entries.erase(std::remove_if(state->index_entries.begin(),
                                            state->index_entries.end(),
                                            [&retired_tables](const CrudIndexEntryRecord& entry) {
                                              return retired_tables.count(entry.table_uuid) != 0;
                                            }),
                             state->index_entries.end());
}

EngineApiDiagnostic ValidateMgaMutatingTransactionAuthority(const EngineRequestContext& context,
                                                           const std::string& operation_id) {
  if (context.local_transaction_id == 0) {
    return MakeInvalidRequestDiagnostic(operation_id, "local_transaction_id_required");
  }
  CrudState state;
  const auto authority = OverlayMgaTransactionAuthority(context, &state);
  if (authority.error) { return authority; }
  return OkDiagnostic();
}

EngineApiDiagnostic ValidateMgaRowVersionChains(const CrudState& state) {
  std::map<std::string, CrudRowVersionRecord> by_version_uuid;
  for (const auto& row : state.row_versions) {
    if (row.version_uuid.empty()) {
      return MakeInvalidRequestDiagnostic("mga.row_version_chain", "row_version_uuid_required");
    }
    if (by_version_uuid.count(row.version_uuid) != 0) {
      return MakeInvalidRequestDiagnostic("mga.row_version_chain", "duplicate_row_version_uuid");
    }
    by_version_uuid[row.version_uuid] = row;
  }
  for (const auto& row : state.row_versions) {
    if (row.previous_version_uuid.empty()) { continue; }
    const auto previous = by_version_uuid.find(row.previous_version_uuid);
    if (previous == by_version_uuid.end()) {
      return MakeInvalidRequestDiagnostic("mga.row_version_chain", "previous_row_version_missing");
    }
    if (previous->second.table_uuid != row.table_uuid || previous->second.row_uuid != row.row_uuid) {
      return MakeInvalidRequestDiagnostic("mga.row_version_chain", "previous_row_version_wrong_chain");
    }
    if (previous->second.sequence >= row.sequence) {
      return MakeInvalidRequestDiagnostic("mga.row_version_chain", "previous_row_version_not_older");
    }
    if (row.previous_sequence != 0 && previous->second.sequence != row.previous_sequence) {
      return MakeInvalidRequestDiagnostic("mga.row_version_chain", "previous_row_version_sequence_mismatch");
    }
  }
  return OkDiagnostic();
}

void FilterMgaTemporaryObjectsForSession(const EngineRequestContext& context, CrudState* state) {
  if (state == nullptr) { return; }
  std::map<std::string, bool> purged_tables;
  std::vector<CrudTableRecord> retained_tables;
  retained_tables.reserve(state->tables.size());
  for (const auto& table : state->tables) {
    const bool global_temporary_metadata =
        table.temporary && table.temporary_scope == "global";
    const bool visible = !table.temporary || global_temporary_metadata ||
                         (!context.session_uuid.canonical.empty() &&
                          table.temporary_session_uuid == context.session_uuid.canonical);
    if (visible) {
      retained_tables.push_back(table);
    } else {
      purged_tables[table.table_uuid] = true;
    }
  }
  state->tables = std::move(retained_tables);
  if (purged_tables.empty()) { return; }
  state->row_versions.erase(std::remove_if(state->row_versions.begin(), state->row_versions.end(),
                                           [&purged_tables](const CrudRowVersionRecord& row) {
                                             return purged_tables.count(row.table_uuid) != 0;
                                           }),
                            state->row_versions.end());
  state->indexes.erase(std::remove_if(state->indexes.begin(), state->indexes.end(),
                                      [&purged_tables](const CrudIndexRecord& index) {
                                        return purged_tables.count(index.table_uuid) != 0;
                                      }),
                       state->indexes.end());
  state->index_entries.erase(std::remove_if(state->index_entries.begin(), state->index_entries.end(),
                                            [&purged_tables](const CrudIndexEntryRecord& entry) {
                                              return purged_tables.count(entry.table_uuid) != 0;
                                            }),
                             state->index_entries.end());
}

EngineApiDiagnostic LoadMgaMetadata(CrudState* state, const EngineRequestContext& context) {
  if (state == nullptr) {
    return MakeInvalidRequestDiagnostic("mga.relation_metadata", "state_required");
  }
  const auto savepoints = ParseSavepoints(context);
  for (const auto& line : ReadLines(MetadataStorePath(context))) {
    const auto fields = SplitTabs(line);
    if (fields.size() < 4 || fields[0] != kRowStoreMagic) { continue; }
    if (fields[1] == "TABLE_METADATA") {
      if (fields.size() < 11) {
        return MakeInvalidRequestDiagnostic("mga.relation_metadata", "table_metadata_invalid");
      }
      CrudTableRecord table;
      table.creator_tx = ParseU64(fields[2]);
      table.event_sequence = ParseU64(fields[3]);
      table.table_uuid = fields[4];
      table.default_name = DecodeCrudTextLocal(fields[5]);
      table.columns = DecodeCrudPairs(fields[6]);
      table.temporary = fields[7] == "1";
      table.temporary_scope = fields[8];
      table.temporary_session_uuid = fields[9];
      table.on_commit_action = fields[10];
      if (MetadataEventRolledBackBySavepoint(savepoints,
                                             table.creator_tx,
                                             table.event_sequence)) {
        continue;
      }
      state->tables.push_back(std::move(table));
      state->max_event_sequence = std::max(state->max_event_sequence, ParseU64(fields[3]));
    } else if (fields[1] == "INDEX_METADATA") {
      if (fields.size() < 17) {
        return MakeInvalidRequestDiagnostic("mga.relation_metadata", "index_metadata_invalid");
      }
      CrudIndexRecord index;
      index.creator_tx = ParseU64(fields[2]);
      index.event_sequence = ParseU64(fields[3]);
      index.index_uuid = fields[4];
      index.table_uuid = fields[5];
      index.profile = NormalizeCrudIndexProfile(fields[6]);
      index.family = fields[7].empty() ? CrudIndexFamilyForProfile(index.profile) : fields[7];
      index.default_name = DecodeCrudTextLocal(fields[8]);
      index.column_name = DecodeCrudTextLocal(fields[9]);
      std::vector<std::string> key_envelopes;
      for (const auto& pair : DecodeCrudPairs(fields[10])) { key_envelopes.push_back(pair.second); }
      index.key_envelopes = std::move(key_envelopes);
      std::vector<std::string> include_columns;
      for (const auto& pair : DecodeCrudPairs(fields[11])) { include_columns.push_back(pair.second); }
      index.include_columns = std::move(include_columns);
      index.predicate_kind = fields[12];
      index.predicate_column = DecodeCrudTextLocal(fields[13]);
      index.predicate_value = DecodeCrudTextLocal(fields[14]);
      index.unique = fields[15] == "1";
      index.approximate = IsApproximateCrudIndexFamily(index.family);
      index.exact_fallback = index.approximate || fields[16] == "1";
      if (MetadataEventRolledBackBySavepoint(savepoints,
                                             index.creator_tx,
                                             index.event_sequence)) {
        continue;
      }
      state->indexes.push_back(std::move(index));
      state->max_event_sequence = std::max(state->max_event_sequence, ParseU64(fields[3]));
    }
  }
  return OkDiagnostic();
}

std::string EncodeStringListAsCrudPairs(const std::vector<std::string>& values) {
  std::vector<std::pair<std::string, std::string>> pairs;
  for (std::size_t i = 0; i < values.size(); ++i) {
    pairs.push_back({std::to_string(i), values[i]});
  }
  return EncodeCrudPairs(pairs);
}

std::string RelationDescriptorTrimAscii(std::string value) {
  std::size_t first = 0;
  while (first < value.size() &&
         (value[first] == ' ' || value[first] == '\t' ||
          value[first] == '\n' || value[first] == '\r')) {
    ++first;
  }
  std::size_t last = value.size();
  while (last > first &&
         (value[last - 1] == ' ' || value[last - 1] == '\t' ||
          value[last - 1] == '\n' || value[last - 1] == '\r')) {
    --last;
  }
  return value.substr(first, last - first);
}

std::string RelationDescriptorLowerAscii(std::string value) {
  for (char& ch : value) {
    if (ch >= 'A' && ch <= 'Z') {
      ch = static_cast<char>(ch - 'A' + 'a');
    }
  }
  return value;
}

std::map<std::string, std::string> RelationDescriptorFields(
    const std::string& descriptor) {
  std::map<std::string, std::string> fields;
  std::string current;
  auto flush = [&fields](std::string part) {
    part = RelationDescriptorTrimAscii(std::move(part));
    if (part.empty()) { return; }
    const auto equals = part.find('=');
    if (equals == std::string::npos) {
      fields[RelationDescriptorLowerAscii(std::move(part))] = "true";
      return;
    }
    fields[RelationDescriptorLowerAscii(
        RelationDescriptorTrimAscii(part.substr(0, equals)))] =
            RelationDescriptorTrimAscii(part.substr(equals + 1));
  };
  for (char ch : descriptor) {
    if (ch == ';') {
      flush(current);
      current.clear();
    } else {
      current.push_back(ch);
    }
  }
  flush(current);
  return fields;
}

std::optional<std::string> ParentTableUuidFromRelationDescriptor(
    const std::string& descriptor) {
  const auto fields = RelationDescriptorFields(descriptor);
  auto field = [&fields](const char* key) -> std::string {
    const auto found = fields.find(key);
    return found == fields.end() ? std::string{} : found->second;
  };
  std::string parent = field("referenced_table_uuid");
  if (parent.empty()) { parent = field("foreign_table_uuid"); }
  if (parent.empty()) { parent = field("foreign_table"); }
  if (!parent.empty()) { return parent; }
  std::string envelope = field("foreign_key");
  if (envelope.empty()) { envelope = field("references"); }
  if (envelope.empty()) { envelope = field("fk"); }
  if (envelope.empty()) { return std::nullopt; }
  envelope = RelationDescriptorTrimAscii(std::move(envelope));
  const auto colon = envelope.find(':');
  const auto dot = envelope.rfind('.');
  const auto open = envelope.find('(');
  if (colon != std::string::npos) {
    parent = envelope.substr(0, colon);
  } else if (dot != std::string::npos) {
    parent = envelope.substr(0, dot);
  } else if (open != std::string::npos) {
    parent = envelope.substr(0, open);
  }
  parent = RelationDescriptorTrimAscii(std::move(parent));
  if (parent.empty()) { return std::nullopt; }
  return parent;
}

std::set<std::string> InsertTargetRelationScope(const EngineRequestContext& context,
                                                const CrudState& metadata,
                                                const std::string& table_uuid) {
  std::set<std::string> table_scope;
  if (table_uuid.empty()) { return table_scope; }
  table_scope.insert(table_uuid);
  const auto table = FindVisibleCrudTable(metadata,
                                          table_uuid,
                                          context.local_transaction_id);
  if (!table) { return table_scope; }
  for (const auto& [column_name, descriptor] : table->columns) {
    (void)column_name;
    const auto parent = ParentTableUuidFromRelationDescriptor(descriptor);
    if (parent && !parent->empty()) {
      table_scope.insert(*parent);
    }
  }
  for (const auto& candidate : metadata.tables) {
    if (candidate.table_uuid.empty() || candidate.table_uuid == table_uuid) {
      continue;
    }
    if (!CrudCreatorVisible(metadata,
                            candidate.creator_tx,
                            candidate.event_sequence,
                            context.local_transaction_id)) {
      continue;
    }
    for (const auto& [column_name, descriptor] : candidate.columns) {
      (void)column_name;
      const auto parent = ParentTableUuidFromRelationDescriptor(descriptor);
      if (parent && *parent == table_uuid) {
        table_scope.insert(candidate.table_uuid);
        break;
      }
    }
  }
  return table_scope;
}

bool RowsContainLargeValueLocators(const std::vector<CrudRowVersionRecord>& rows) {
  for (const auto& row : rows) {
    for (const auto& [field, value] : row.values) {
      (void)field;
      if (CrudValueIsLargeValueLocator(value)) {
        return true;
      }
      if (IsMgaLargeValueLocator(value)) {
        return true;
      }
    }
  }
  return false;
}

void AddRelationLoadEvidence(MgaRelationStoreResult* result,
                             const std::string& route) {
  if (result == nullptr) { return; }
  result->evidence.push_back({"mga_relation_state_load_route", route});
  result->evidence.push_back({"mga_relation_state_full_load",
                              result->full_state_load ? "true" : "false"});
  result->evidence.push_back({"mga_relation_state_scoped_load",
                              result->scoped_state_load ? "true" : "false"});
  result->evidence.push_back({"mga_relation_state_row_versions_scanned",
                              std::to_string(result->row_versions_scanned)});
  result->evidence.push_back({"mga_relation_state_row_versions_retained",
                              std::to_string(result->row_versions_retained)});
  result->evidence.push_back({"mga_relation_state_index_entries_scanned",
                              std::to_string(result->index_entries_scanned)});
  result->evidence.push_back({"mga_relation_state_index_entries_retained",
                              std::to_string(result->index_entries_retained)});
  result->evidence.push_back({"mga_relation_state_scoped_physical_segments",
                              result->scoped_physical_segments_used ? "true" : "false"});
  result->evidence.push_back({"mga_relation_state_scoped_physical_fallback",
                              result->scoped_physical_segments_fallback ? "true" : "false"});
}

}  // namespace

MgaRelationStoreResult LoadMgaRelationStoreState(const EngineRequestContext& context) {
  MgaRelationStoreResult result;
  result.full_state_load = true;
  if (context.database_path.empty()) {
    result.diagnostic = MakeInvalidRequestDiagnostic("mga.row_store", "database_path_required");
    return result;
  }
  const auto metadata = LoadMgaMetadata(&result.state.crud_metadata, context);
  if (metadata.error) {
    result.diagnostic = metadata;
    return result;
  }
  const auto authority = OverlayMgaTransactionAuthority(context, &result.state.crud_metadata);
  if (authority.error) {
    result.diagnostic = authority;
    return result;
  }
  const auto savepoints = ParseSavepoints(context);
  for (const auto& line : ReadLines(RowStorePath(context))) {
    const auto fields = SplitTabs(line);
    if (fields.size() < 11 || fields[0] != kRowStoreMagic || fields[1] != "ROW_VERSION") { continue; }
    ++result.row_versions_scanned;
    CrudRowVersionRecord row;
    row.creator_tx = ParseU64(fields[2]);
    row.event_sequence = ParseU64(fields[3]);
    row.sequence = row.event_sequence;
    row.table_uuid = fields[4];
    row.row_uuid = fields[5];
    row.version_uuid = fields[6];
    row.deleted = fields[7] == "1";
    row.previous_version_uuid = fields[8];
    row.previous_sequence = ParseU64(fields[9]);
    row.values = DecodeCrudPairs(fields[10]);
    if (fields.size() >= 12) {
      row.temporary_session_uuid = fields[11];
    }
    if (RowEventRolledBackBySavepoint(savepoints,
                                      row.creator_tx,
                                      row.event_sequence)) {
      continue;
    }
    result.state.max_row_event_sequence = std::max(result.state.max_row_event_sequence, row.event_sequence);
    result.state.row_versions.push_back(std::move(row));
    ++result.row_versions_retained;
  }
  for (const auto& line : ReadLines(IndexStorePath(context))) {
    const auto fields = SplitTabs(line);
    if (fields.size() < 13 || fields[0] != kRowStoreMagic || fields[1] != "INDEX_ENTRY") { continue; }
    ++result.index_entries_scanned;
    CrudIndexEntryRecord entry;
    entry.creator_tx = ParseU64(fields[2]);
    entry.event_sequence = ParseU64(fields[3]);
    entry.sequence = entry.event_sequence;
    entry.index_uuid = fields[4];
    entry.table_uuid = fields[5];
    entry.column_name = fields[6];
    entry.family = fields[7];
    entry.entry_kind = fields[8];
    entry.key_value = fields[9];
    entry.payload_value = fields[10];
    entry.row_uuid = fields[11];
    entry.version_uuid = fields[12];
    if (IndexEventRolledBackBySavepoint(savepoints,
                                        entry.creator_tx,
                                        entry.event_sequence)) {
      continue;
    }
    result.state.max_index_event_sequence = std::max(result.state.max_index_event_sequence, entry.event_sequence);
    result.state.index_entries.push_back(std::move(entry));
    ++result.index_entries_retained;
  }
  const auto retired_tables =
      VisibleRetiredTemporaryTableMetadata(context, result.state.crud_metadata);
  FilterVisibleRetiredTemporaryMetadata(context, &result.state.crud_metadata);
  result.state.row_versions.erase(
      std::remove_if(result.state.row_versions.begin(),
                     result.state.row_versions.end(),
                     [&retired_tables](const CrudRowVersionRecord& row) {
                       return retired_tables.count(row.table_uuid) != 0;
                     }),
      result.state.row_versions.end());
  result.state.index_entries.erase(
      std::remove_if(result.state.index_entries.begin(),
                     result.state.index_entries.end(),
                     [&retired_tables](const CrudIndexEntryRecord& entry) {
                       return retired_tables.count(entry.table_uuid) != 0;
                     }),
      result.state.index_entries.end());
  if (RowsContainLargeValueLocators(result.state.row_versions)) {
    const auto expanded_large_values = ExpandMgaLargeValueLocators(context, &result.state.row_versions);
    if (expanded_large_values.error) {
      result.diagnostic = expanded_large_values;
      return result;
    }
  }
  const auto chain_status = ValidateMgaRowVersionChains(BuildCrudCompatibilityStateFromMga(result.state));
  if (chain_status.error) {
    result.diagnostic = chain_status;
    return result;
  }
  FilterMgaTemporaryObjectsForSession(context, &result.state.crud_metadata);
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  AddRelationLoadEvidence(&result, "full");
  return result;
}

MgaRelationStoreResult LoadMgaRelationStoreStateForInsertTarget(
    const EngineRequestContext& context,
    const std::string& table_uuid) {
  MgaRelationStoreResult result;
  result.scoped_state_load = true;
  if (context.database_path.empty()) {
    result.diagnostic = MakeInvalidRequestDiagnostic("mga.row_store", "database_path_required");
    return result;
  }
  if (table_uuid.empty()) {
    result.diagnostic = MakeInvalidRequestDiagnostic("mga.row_store", "target_table_uuid_required");
    return result;
  }
  const auto metadata = LoadMgaMetadata(&result.state.crud_metadata, context);
  if (metadata.error) {
    result.diagnostic = metadata;
    return result;
  }
  const auto authority = OverlayMgaTransactionAuthority(context, &result.state.crud_metadata);
  if (authority.error) {
    result.diagnostic = authority;
    return result;
  }
  const auto retired_tables =
      VisibleRetiredTemporaryTableMetadata(context, result.state.crud_metadata);
  FilterVisibleRetiredTemporaryMetadata(context, &result.state.crud_metadata);
  const std::set<std::string> table_scope =
      InsertTargetRelationScope(context, result.state.crud_metadata, table_uuid);
  const auto savepoints = ParseSavepoints(context);

  auto scoped_lines = [&context, &table_scope](bool row_store,
                                               bool* used_segments) {
    std::vector<std::string> lines;
    bool used = false;
    for (const auto& scoped_table_uuid : table_scope) {
      const std::string path = row_store
                                   ? ScopedRowStorePath(context, scoped_table_uuid)
                                   : ScopedIndexStorePath(context, scoped_table_uuid);
      if (!FileExistsAndNotEmpty(path)) {
        continue;
      }
      used = true;
      const auto segment_lines = ReadLines(path);
      lines.insert(lines.end(), segment_lines.begin(), segment_lines.end());
    }
    if (used_segments != nullptr) {
      *used_segments = used;
    }
    return lines;
  };

  bool row_segments_used = false;
  bool index_segments_used = false;
  auto row_lines = scoped_lines(true, &row_segments_used);
  auto index_lines = scoped_lines(false, &index_segments_used);
  const bool global_row_fallback = !row_segments_used;
  const bool global_index_fallback = !index_segments_used;
  if (global_row_fallback) {
    row_lines = ReadLines(RowStorePath(context));
  }
  if (global_index_fallback) {
    index_lines = ReadLines(IndexStorePath(context));
  }
  result.scoped_physical_segments_used = row_segments_used || index_segments_used;
  result.scoped_physical_segments_fallback =
      global_row_fallback || global_index_fallback;

  for (const auto& line : row_lines) {
    const auto fields = SplitTabs(line);
    if (fields.size() < 11 || fields[0] != kRowStoreMagic || fields[1] != "ROW_VERSION") { continue; }
    ++result.row_versions_scanned;
    if (table_scope.count(fields[4]) == 0 || retired_tables.count(fields[4]) != 0) {
      continue;
    }
    CrudRowVersionRecord row;
    row.creator_tx = ParseU64(fields[2]);
    row.event_sequence = ParseU64(fields[3]);
    row.sequence = row.event_sequence;
    row.table_uuid = fields[4];
    row.row_uuid = fields[5];
    row.version_uuid = fields[6];
    row.deleted = fields[7] == "1";
    row.previous_version_uuid = fields[8];
    row.previous_sequence = ParseU64(fields[9]);
    row.values = DecodeCrudPairs(fields[10]);
    if (fields.size() >= 12) {
      row.temporary_session_uuid = fields[11];
    }
    if (RowEventRolledBackBySavepoint(savepoints,
                                      row.creator_tx,
                                      row.event_sequence)) {
      continue;
    }
    result.state.max_row_event_sequence =
        std::max(result.state.max_row_event_sequence, row.event_sequence);
    result.state.row_versions.push_back(std::move(row));
    ++result.row_versions_retained;
  }

  for (const auto& line : index_lines) {
    const auto fields = SplitTabs(line);
    if (fields.size() < 13 || fields[0] != kRowStoreMagic || fields[1] != "INDEX_ENTRY") { continue; }
    ++result.index_entries_scanned;
    if (table_scope.count(fields[5]) == 0 || retired_tables.count(fields[5]) != 0) {
      continue;
    }
    CrudIndexEntryRecord entry;
    entry.creator_tx = ParseU64(fields[2]);
    entry.event_sequence = ParseU64(fields[3]);
    entry.sequence = entry.event_sequence;
    entry.index_uuid = fields[4];
    entry.table_uuid = fields[5];
    entry.column_name = fields[6];
    entry.family = fields[7];
    entry.entry_kind = fields[8];
    entry.key_value = fields[9];
    entry.payload_value = fields[10];
    entry.row_uuid = fields[11];
    entry.version_uuid = fields[12];
    if (IndexEventRolledBackBySavepoint(savepoints,
                                        entry.creator_tx,
                                        entry.event_sequence)) {
      continue;
    }
    result.state.max_index_event_sequence =
        std::max(result.state.max_index_event_sequence, entry.event_sequence);
    result.state.index_entries.push_back(std::move(entry));
    ++result.index_entries_retained;
  }

  if (RowsContainLargeValueLocators(result.state.row_versions)) {
    const auto expanded_large_values =
        ExpandMgaLargeValueLocators(context, &result.state.row_versions);
    if (expanded_large_values.error) {
      result.diagnostic = expanded_large_values;
      return result;
    }
  }
  const auto chain_status =
      ValidateMgaRowVersionChains(BuildCrudCompatibilityStateFromMga(result.state));
  if (chain_status.error) {
    result.diagnostic = chain_status;
    return result;
  }
  FilterMgaTemporaryObjectsForSession(context, &result.state.crud_metadata);
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  AddRelationLoadEvidence(&result, "insert_target_scoped");
  return result;
}

CrudState BuildCrudCompatibilityStateFromMga(const MgaRelationStoreState& state) {
  CrudState merged = state.crud_metadata;
  merged.row_versions = state.row_versions;
  merged.index_entries = state.index_entries;
  merged.max_sequence = state.max_row_event_sequence;
  merged.max_index_sequence = state.max_index_event_sequence;
  return merged;
}

MgaTemporaryTableVisibilityResult CheckMgaTemporaryTableVisibility(
    const EngineRequestContext& context,
    const std::string& table_uuid) {
  MgaTemporaryTableVisibilityResult result;
  if (table_uuid.empty()) {
    result.diagnostic = MakeInvalidRequestDiagnostic("mga.temporary_table_visibility",
                                                     "table_uuid_required");
    return result;
  }
  CrudState state;
  const auto metadata = LoadMgaMetadata(&state, context);
  if (metadata.error) {
    result.diagnostic = metadata;
    return result;
  }
  bool has_table_candidate = false;
  bool temporary_table_candidate = false;
  for (const auto& table : state.tables) {
    if (table.table_uuid == table_uuid) {
      has_table_candidate = true;
      temporary_table_candidate = table.temporary;
      break;
    }
  }
  if (!has_table_candidate) {
    result.ok = true;
    result.diagnostic = OkDiagnostic();
    return result;
  }
  const auto authority = OverlayMgaTransactionAuthority(context, &state);
  if (authority.error) {
    result.diagnostic = authority;
    return result;
  }
  FilterVisibleRetiredTemporaryMetadata(context, &state);
  const auto visible = FindVisibleCrudTable(state, table_uuid, context.local_transaction_id);
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  if (!visible) {
    result.known_temporary = temporary_table_candidate;
    result.hidden_by_temporary_visibility = temporary_table_candidate;
    return result;
  }
  result.table_visible = true;
  result.table = *visible;
  if (!visible->temporary) { return result; }
  result.known_temporary = true;
  result.visible_to_session =
      visible->temporary_scope == "global" ||
      (!visible->temporary_session_uuid.empty() &&
       visible->temporary_session_uuid == context.session_uuid.canonical);
  return result;
}

MgaTemporaryRecoveryClassificationResult ClassifyMgaTemporaryRecoveryState(
    const EngineRequestContext& context) {
  struct LatestRowState {
    std::uint64_t event_sequence = 0;
    bool deleted = false;
  };
  enum class EventAuthority {
    kCommitted,
    kRolledBack,
    kActiveOrUnresolved,
    kFenced,
  };

  MgaTemporaryRecoveryClassificationResult result;
  if (context.database_path.empty()) {
    result.diagnostic = MakeInvalidRequestDiagnostic(
        "mga.temporary_recovery",
        "database_path_required");
    return result;
  }
  const auto loaded = LoadLocalTransactionInventoryFromDatabase(
      context.database_path);
  if (!loaded.ok()) {
    result.diagnostic = MakeEngineApiDiagnostic(
        loaded.diagnostic.diagnostic_code.empty()
            ? "SB-MGA-TXN-INV-LOAD-FAILED"
            : loaded.diagnostic.diagnostic_code,
        loaded.diagnostic.message_key.empty()
            ? "mga.transaction_inventory.load_failed"
            : loaded.diagnostic.message_key,
        loaded.diagnostic.remediation_hint,
        true);
    return result;
  }
  std::map<std::uint64_t, std::string> transaction_states;
  for (const auto& entry : loaded.inventory.entries) {
    if (!entry.identity.local_id.valid()) { continue; }
    transaction_states[entry.identity.local_id.value] =
        MgaTransactionStateName(entry.state);
  }
  auto classify_event = [&](std::uint64_t creator_tx) {
    if (creator_tx == 0) { return EventAuthority::kCommitted; }
    const auto found = transaction_states.find(creator_tx);
    if (found == transaction_states.end()) {
      ++result.fenced_event_count;
      return EventAuthority::kFenced;
    }
    if (found->second == "committed" || found->second == "archived") {
      return EventAuthority::kCommitted;
    }
    if (found->second == "rolled_back") {
      ++result.rolled_back_event_count;
      return EventAuthority::kRolledBack;
    }
    ++result.active_or_unresolved_event_count;
    return EventAuthority::kActiveOrUnresolved;
  };

  std::set<std::string> temporary_tables;
  std::set<std::string> durable_global_tables;
  std::set<std::string> committed_private_tables;
  std::set<std::string> retired_private_tables;
  for (const auto& line : ReadLines(MetadataStorePath(context))) {
    const auto fields = SplitTabs(line);
    if (fields.size() >= 11 && fields[0] == kRowStoreMagic &&
        fields[1] == "TABLE_METADATA" && fields[7] == "1") {
      temporary_tables.insert(fields[4]);
      const auto authority = classify_event(ParseU64(fields[2]));
      if (authority != EventAuthority::kCommitted) { continue; }
      if (fields[8] == "global") {
        durable_global_tables.insert(fields[4]);
      } else {
        committed_private_tables.insert(fields[4]);
      }
    } else if (fields.size() >= 7 && fields[0] == kRowStoreMagic &&
               fields[1] == "TABLE_METADATA_RETIRED") {
      temporary_tables.insert(fields[4]);
      const auto authority = classify_event(ParseU64(fields[2]));
      if (authority == EventAuthority::kCommitted) {
        retired_private_tables.insert(fields[4]);
        ++result.retired_private_metadata_count;
      }
    }
  }
  result.durable_global_metadata_count =
      static_cast<std::uint64_t>(durable_global_tables.size());
  for (const auto& table_uuid : committed_private_tables) {
    if (retired_private_tables.count(table_uuid) == 0) {
      ++result.orphaned_private_metadata_count;
    }
  }

  std::map<std::string, LatestRowState> latest_rows;
  for (const auto& line : ReadLines(RowStorePath(context))) {
    const auto fields = SplitTabs(line);
    if (fields.size() < 12 || fields[0] != kRowStoreMagic ||
        fields[1] != "ROW_VERSION") {
      continue;
    }
    const std::string& table_uuid = fields[4];
    const std::string& row_uuid = fields[5];
    const std::string& session_uuid = fields[11];
    if (session_uuid.empty() && temporary_tables.count(table_uuid) == 0) {
      continue;
    }
    temporary_tables.insert(table_uuid);
    const auto authority = classify_event(ParseU64(fields[2]));
    if (authority != EventAuthority::kCommitted) { continue; }
    const std::uint64_t event_sequence = ParseU64(fields[3]);
    const std::string key = table_uuid + "\t" + row_uuid + "\t" + session_uuid;
    auto& latest = latest_rows[key];
    if (event_sequence >= latest.event_sequence) {
      latest.event_sequence = event_sequence;
      latest.deleted = fields[7] == "1";
    }
  }
  for (const auto& [_, row] : latest_rows) {
    if (row.deleted) {
      ++result.cleaned_row_count;
    } else {
      ++result.orphaned_row_count;
    }
  }

  std::set<std::string> committed_large_values;
  std::set<std::string> reclaimed_large_values;
  for (const auto& line : ReadLines(LargeValueStorePath(context))) {
    const auto fields = SplitTabs(line);
    if (fields.size() >= 11 && fields[0] == kRowStoreMagic &&
        fields[1] == "LARGE_VALUE") {
      if (temporary_tables.count(fields[4]) == 0) { continue; }
      const auto authority = classify_event(ParseU64(fields[2]));
      if (authority == EventAuthority::kCommitted) {
        committed_large_values.insert(fields[3]);
      }
    } else if (fields.size() >= 9 && fields[0] == kRowStoreMagic &&
               fields[1] == "LARGE_VALUE_RECLAIMED") {
      if (temporary_tables.count(fields[4]) == 0) { continue; }
      const auto authority = classify_event(ParseU64(fields[2]));
      if (authority == EventAuthority::kCommitted) {
        reclaimed_large_values.insert(fields[3]);
      }
    }
  }
  result.reclaimed_large_value_count =
      static_cast<std::uint64_t>(reclaimed_large_values.size());
  for (const auto& overflow_uuid : committed_large_values) {
    if (reclaimed_large_values.count(overflow_uuid) == 0) {
      ++result.orphaned_large_value_count;
    }
  }

  if (result.active_or_unresolved_event_count != 0) {
    result.classification = "recovery_required";
    result.action = "transaction_recovery_required_before_open";
    result.recovery_required = true;
    result.write_admission_must_remain_fenced = true;
  } else if (result.fenced_event_count != 0) {
    result.classification = "fenced";
    result.action = "operator_recovery_required_missing_transaction_authority";
    result.write_admission_must_remain_fenced = true;
  } else if (result.orphaned_private_metadata_count != 0 ||
             result.orphaned_row_count != 0 ||
             result.orphaned_large_value_count != 0) {
    result.classification = "recovery_required";
    result.action = "temporary_orphan_cleanup_required_before_open";
    result.recovery_required = true;
    result.write_admission_must_remain_fenced = true;
  } else if (result.durable_global_metadata_count != 0 ||
             result.cleaned_row_count != 0 ||
             result.reclaimed_large_value_count != 0 ||
             result.retired_private_metadata_count != 0) {
    result.classification = "new_state";
    result.action = "open_allowed_no_orphaned_temporary_state";
  } else {
    result.classification = "old_state";
    result.action = "open_allowed_no_visible_temporary_state";
  }
  result.evidence.push_back({"temporary_recovery_classification",
                             result.classification});
  result.evidence.push_back({"temporary_recovery_action", result.action});
  result.evidence.push_back({"temporary_recovery_active_or_unresolved_events",
                             std::to_string(result.active_or_unresolved_event_count)});
  result.evidence.push_back({"temporary_recovery_fenced_events",
                             std::to_string(result.fenced_event_count)});
  result.evidence.push_back({"temporary_recovery_orphaned_private_metadata",
                             std::to_string(result.orphaned_private_metadata_count)});
  result.evidence.push_back({"temporary_recovery_orphaned_rows",
                             std::to_string(result.orphaned_row_count)});
  result.evidence.push_back({"temporary_recovery_orphaned_large_values",
                             std::to_string(result.orphaned_large_value_count)});
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  return result;
}

MgaRelationPhysicalSweepResult ApplyMgaRelationPhysicalSweepToState(
    const MgaRelationPhysicalSweepRequest& request) {
  namespace mga = scratchbird::transaction::mga;
  auto fail = [](std::string detail) {
    MgaRelationPhysicalSweepResult result;
    result.fail_closed = true;
    result.diagnostic = MakeInvalidRequestDiagnostic(
        "mga.relation_physical_sweep", std::move(detail));
    return result;
  };

  if (!request.engine_mga_authoritative) {
    return fail("engine_mga_authority_required");
  }
  if (!request.cleanup_horizon_authoritative ||
      request.authoritative_cleanup_horizon_local_transaction_id == 0) {
    return fail("cleanup_horizon_authority_required");
  }
  if (request.max_row_versions_to_scan == 0 ||
      request.state.row_versions.size() > request.max_row_versions_to_scan) {
    return fail("bounded_row_version_scan_required");
  }
  if (request.max_index_entries_to_scan == 0 ||
      request.state.index_entries.size() > request.max_index_entries_to_scan) {
    return fail("bounded_index_entry_scan_required");
  }
  if (request.reclaim_evidence_records.empty()) {
    return fail("reclaim_evidence_required");
  }

  auto evidence_matches_row =
      [](const mga::LocalCleanupReclaimEvidenceRecord& evidence,
         const CrudRowVersionRecord& row) {
        const std::string row_uuid =
            scratchbird::core::uuid::UuidToString(
                evidence.row_version_identity.row.row_uuid.value);
        return row.creator_tx ==
                   evidence.row_version_identity.creator_transaction.local_id.value &&
               row.sequence == evidence.row_version_identity.version_sequence &&
               row.row_uuid == row_uuid;
      };
  auto evidence_for_row =
      [&](const CrudRowVersionRecord& row)
          -> const mga::LocalCleanupReclaimEvidenceRecord* {
        for (const auto& evidence : request.reclaim_evidence_records) {
          if (evidence_matches_row(evidence, row)) {
            return &evidence;
          }
        }
        return nullptr;
      };

  MgaRelationPhysicalSweepResult result;
  result.state = request.state;
  result.state.row_versions.clear();
  result.state.index_entries.clear();
  result.scanned_row_version_count =
      static_cast<std::uint64_t>(request.state.row_versions.size());
  result.scanned_index_entry_count =
      static_cast<std::uint64_t>(request.state.index_entries.size());

  std::vector<std::string> removed_version_uuids;
  std::vector<std::string> matched_evidence_ids;
  for (const auto& row : request.state.row_versions) {
    const auto* evidence = evidence_for_row(row);
    if (evidence == nullptr) {
      result.state.row_versions.push_back(row);
      ++result.retained_row_version_count;
      continue;
    }
    removed_version_uuids.push_back(row.version_uuid);
    matched_evidence_ids.push_back(evidence->stable_evidence_id);
    ++result.removed_row_version_count;
  }

  for (const auto& evidence : request.reclaim_evidence_records) {
    if (std::find(matched_evidence_ids.begin(),
                  matched_evidence_ids.end(),
                  evidence.stable_evidence_id) == matched_evidence_ids.end()) {
      return fail("reclaim_evidence_not_in_relation_state:" +
                  evidence.stable_evidence_id);
    }
  }

  for (const auto& entry : request.state.index_entries) {
    if (std::find(removed_version_uuids.begin(),
                  removed_version_uuids.end(),
                  entry.version_uuid) != removed_version_uuids.end()) {
      ++result.removed_index_entry_count;
      continue;
    }
    result.state.index_entries.push_back(entry);
    ++result.retained_index_entry_count;
  }

  result.ok = true;
  result.physical_state_mutated =
      result.removed_row_version_count != 0 ||
      result.removed_index_entry_count != 0;
  result.diagnostic = MakeEngineApiDiagnostic(
      "SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  result.evidence.push_back({"mga_relation_physical_sweep",
                             "durable_mga_cleanup_horizon:" +
                                 std::to_string(
                                     request
                                         .authoritative_cleanup_horizon_local_transaction_id)});
  result.evidence.push_back({"mga_relation_physical_sweep_authority",
                             "durable_mga_transaction_inventory"});
  return result;
}

namespace {

void AddBytes(std::uint64_t* total, std::uint64_t bytes) {
  if (total == nullptr) { return; }
  const std::uint64_t max = std::numeric_limits<std::uint64_t>::max();
  *total = bytes > max - *total ? max : *total + bytes;
}

std::uint64_t TextBytes(const std::string& value) {
  return static_cast<std::uint64_t>(value.size());
}

std::uint64_t PairBytes(const std::vector<std::pair<std::string, std::string>>& values) {
  std::uint64_t total = 0;
  for (const auto& [name, value] : values) {
    AddBytes(&total, 8 + TextBytes(name) + TextBytes(value));
  }
  return total;
}

std::uint64_t StringListBytes(const std::vector<std::string>& values) {
  std::uint64_t total = 0;
  for (const auto& value : values) {
    AddBytes(&total, 8 + TextBytes(value));
  }
  return total;
}

std::uint64_t TableMetadataEstimateBytes(const CrudTableRecord& table) {
  std::uint64_t total = 96;
  AddBytes(&total, TextBytes(table.table_uuid));
  AddBytes(&total, TextBytes(table.default_name));
  AddBytes(&total, PairBytes(table.columns));
  AddBytes(&total, TextBytes(table.temporary_scope));
  AddBytes(&total, TextBytes(table.temporary_session_uuid));
  AddBytes(&total, TextBytes(table.on_commit_action));
  return total;
}

std::uint64_t RowVersionEstimateBytes(const CrudRowVersionRecord& row) {
  std::uint64_t total = 128;
  AddBytes(&total, TextBytes(row.table_uuid));
  AddBytes(&total, TextBytes(row.row_uuid));
  AddBytes(&total, TextBytes(row.version_uuid));
  AddBytes(&total, TextBytes(row.previous_version_uuid));
  AddBytes(&total, PairBytes(row.values));
  return total;
}

std::uint64_t IndexMetadataEstimateBytes(const CrudIndexRecord& index) {
  std::uint64_t total = 128;
  AddBytes(&total, TextBytes(index.index_uuid));
  AddBytes(&total, TextBytes(index.table_uuid));
  AddBytes(&total, TextBytes(index.column_name));
  AddBytes(&total, TextBytes(index.family));
  AddBytes(&total, TextBytes(index.profile));
  AddBytes(&total, TextBytes(index.default_name));
  AddBytes(&total, StringListBytes(index.key_envelopes));
  AddBytes(&total, StringListBytes(index.include_columns));
  AddBytes(&total, TextBytes(index.predicate_kind));
  AddBytes(&total, TextBytes(index.predicate_column));
  AddBytes(&total, TextBytes(index.predicate_value));
  return total;
}

std::uint64_t IndexEntryEstimateBytes(const CrudIndexEntryRecord& entry) {
  std::uint64_t total = 112;
  AddBytes(&total, TextBytes(entry.index_uuid));
  AddBytes(&total, TextBytes(entry.table_uuid));
  AddBytes(&total, TextBytes(entry.column_name));
  AddBytes(&total, TextBytes(entry.family));
  AddBytes(&total, TextBytes(entry.entry_kind));
  AddBytes(&total, TextBytes(entry.key_value));
  AddBytes(&total, TextBytes(entry.payload_value));
  AddBytes(&total, TextBytes(entry.row_uuid));
  AddBytes(&total, TextBytes(entry.version_uuid));
  return total;
}

bool TableUuidSeen(const std::vector<std::string>& seen, const std::string& table_uuid) {
  return std::find(seen.begin(), seen.end(), table_uuid) != seen.end();
}

MgaRelationStatistics EstimateRelationStatisticsFromState(const EngineRequestContext& context,
                                                          const CrudState& state,
                                                          const std::string& table_uuid,
                                                          bool include_indexes) {
  MgaRelationStatistics statistics;
  const auto table = FindVisibleCrudTable(state, table_uuid, context.local_transaction_id);
  if (!table) { return statistics; }

  statistics.relation_found = true;
  statistics.visible_row_estimate =
      static_cast<std::uint64_t>(VisibleCrudRowsForContext(state, table_uuid, context).size());

  // Stable estimate from persisted MGA relation sidecar payload lengths. This
  // is not page-byte accounting; it is engine-owned row-version/catalog size.
  AddBytes(&statistics.row_store_bytes, TableMetadataEstimateBytes(*table));
  for (const auto& row : state.row_versions) {
    if (row.table_uuid != table_uuid) { continue; }
    ++statistics.retained_row_version_count;
    AddBytes(&statistics.row_store_bytes, RowVersionEstimateBytes(row));
  }
  statistics.table_size_bytes = statistics.row_store_bytes;

  if (include_indexes) {
    const auto indexes = VisibleCrudIndexesForTable(state, table_uuid, context.local_transaction_id);
    for (const auto& index : indexes) {
      AddBytes(&statistics.index_store_bytes, IndexMetadataEstimateBytes(index));
    }
    for (const auto& entry : state.index_entries) {
      if (entry.table_uuid != table_uuid) { continue; }
      AddBytes(&statistics.index_store_bytes, IndexEntryEstimateBytes(entry));
    }
    AddBytes(&statistics.table_size_bytes, statistics.index_store_bytes);
  }
  return statistics;
}

}  // namespace

MgaRelationStatisticsResult EstimateMgaRelationStatistics(const EngineRequestContext& context,
                                                          const std::string& table_uuid,
                                                          bool include_indexes) {
  MgaRelationStatisticsResult result;
  auto loaded = LoadMgaRelationStoreState(context);
  if (!loaded.ok) {
    result.diagnostic = loaded.diagnostic;
    return result;
  }
  const CrudState state = BuildCrudCompatibilityStateFromMga(loaded.state);
  result.statistics = EstimateRelationStatisticsFromState(context, state, table_uuid, include_indexes);
  result.ok = true;
  result.diagnostic = MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  return result;
}

MgaRelationStatisticsResult EstimateMgaCatalogStatistics(const EngineRequestContext& context,
                                                         bool include_indexes) {
  MgaRelationStatisticsResult result;
  auto loaded = LoadMgaRelationStoreState(context);
  if (!loaded.ok) {
    result.diagnostic = loaded.diagnostic;
    return result;
  }
  const CrudState state = BuildCrudCompatibilityStateFromMga(loaded.state);
  std::vector<std::string> table_uuids;
  for (const auto& table : state.tables) {
    if (table.table_uuid.empty() || TableUuidSeen(table_uuids, table.table_uuid)) { continue; }
    if (!FindVisibleCrudTable(state, table.table_uuid, context.local_transaction_id)) { continue; }
    table_uuids.push_back(table.table_uuid);
  }
  result.statistics.relation_found = true;
  for (const auto& table_uuid : table_uuids) {
    const auto table_stats = EstimateRelationStatisticsFromState(context, state, table_uuid, include_indexes);
    AddBytes(&result.statistics.visible_row_estimate, table_stats.visible_row_estimate);
    AddBytes(&result.statistics.retained_row_version_count, table_stats.retained_row_version_count);
    AddBytes(&result.statistics.row_store_bytes, table_stats.row_store_bytes);
    AddBytes(&result.statistics.index_store_bytes, table_stats.index_store_bytes);
    AddBytes(&result.statistics.table_size_bytes, table_stats.table_size_bytes);
  }
  result.ok = true;
  result.diagnostic = MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  return result;
}

EngineApiDiagnostic EnsureMgaRelationStorageDescriptor(const EngineRequestContext& context,
                                                       const CrudTableRecord& table,
                                                       const std::vector<CrudIndexRecord>& indexes,
                                                       MgaRelationStorageDescriptor* descriptor) {
  const auto persisted = LoadDescriptorFieldsByRelation(context);
  const auto existing = persisted.find(table.table_uuid);
  const auto fields = existing == persisted.end()
                          ? BuildPersistedMgaRelationDescriptorFields(context, table, indexes)
                          : existing->second;
  MgaRelationStorageDescriptor built =
      BuildMgaRelationStorageDescriptorFromCrudMetadata(context, table, indexes, fields);
  const auto validated = ValidateMgaRelationStorageDescriptor(built);
  if (validated.error) { return validated; }
  if (existing == persisted.end()) {
    const auto persisted_diagnostic = PersistDescriptorFields(context, table.table_uuid, fields);
    if (persisted_diagnostic.error) { return persisted_diagnostic; }
  }
  if (descriptor != nullptr) { *descriptor = std::move(built); }
  return OkDiagnostic();
}

EngineApiDiagnostic AppendMgaRowVersion(const EngineRequestContext& context,
                                         const CrudRowVersionRecord& row,
                                         std::uint64_t* written_event_sequence) {
  std::vector<CrudRowVersionRecord> rows{row};
  std::vector<std::uint64_t> sequences;
  const auto diagnostic = AppendMgaRowVersions(context, &rows, written_event_sequence == nullptr ? nullptr : &sequences);
  if (diagnostic.error) { return diagnostic; }
  if (written_event_sequence != nullptr && !sequences.empty()) {
    *written_event_sequence = sequences.front();
  }
  return OkDiagnostic();
}

struct MgaRelationHotAppendContext::Impl {
  explicit Impl(const EngineRequestContext& source_context) : context(source_context) {}

  EngineRequestContext context;
  std::ofstream row_out;
  std::ofstream index_out;
  std::map<std::string, std::ofstream> scoped_row_out;
  std::map<std::string, std::ofstream> scoped_index_out;
  std::set<std::string> scoped_row_dirty_paths;
  std::set<std::string> scoped_index_dirty_paths;
  bool row_dirty = false;
  bool index_dirty = false;
  MgaRelationHotAppendCounters counters;
};

MgaRelationHotAppendContext::MgaRelationHotAppendContext(
    const EngineRequestContext& context)
    : impl_(std::make_unique<Impl>(context)) {}

MgaRelationHotAppendContext::~MgaRelationHotAppendContext() = default;

MgaRelationHotAppendContext::MgaRelationHotAppendContext(
    MgaRelationHotAppendContext&&) noexcept = default;

MgaRelationHotAppendContext& MgaRelationHotAppendContext::operator=(
    MgaRelationHotAppendContext&&) noexcept = default;

const MgaRelationHotAppendCounters& MgaRelationHotAppendContext::counters() const {
  return impl_->counters;
}

EngineApiDiagnostic MgaRelationHotAppendContext::AppendRowVersions(
    std::vector<CrudRowVersionRecord>* rows,
    std::vector<std::uint64_t>* written_event_sequences) {
  if (impl_->context.database_path.empty()) {
    return MakeInvalidRequestDiagnostic("mga.row_store", "database_path_required");
  }
  if (rows == nullptr || rows->empty()) {
    return MakeInvalidRequestDiagnostic("mga.row_store", "row_versions_required");
  }
  if (!impl_->row_out.is_open()) {
    impl_->row_out.open(RowStorePath(impl_->context), std::ios::app | std::ios::binary);
    if (!impl_->row_out) {
      return MakeInvalidRequestDiagnostic("mga.row_store", "row_version_append_failed");
    }
    ++impl_->counters.row_stream_opens;
  }
  if (written_event_sequences != nullptr) {
    written_event_sequences->clear();
    written_event_sequences->reserve(rows->size());
  }
  const auto reservation = ReserveEventSequenceRange(
      impl_->context,
      "row_versions",
      RowStorePath(impl_->context),
      static_cast<std::uint64_t>(rows->size()),
      [this]() { return ScanNextRowEventSequence(impl_->context); });
  if (!reservation.ok) { return reservation.diagnostic; }
  ++impl_->counters.row_range_reservations;
  std::uint64_t event_sequence = reservation.first;
  for (auto& writable : *rows) {
    writable.event_sequence = event_sequence++;
    writable.sequence = writable.event_sequence;
    const std::string line = JoinLine({kRowStoreMagic,
                                       "ROW_VERSION",
                                       std::to_string(writable.creator_tx),
                                       std::to_string(writable.event_sequence),
                                       writable.table_uuid,
                                       writable.row_uuid,
                                       writable.version_uuid,
                                       writable.deleted ? "1" : "0",
                                       writable.previous_version_uuid,
                                       std::to_string(writable.previous_sequence),
                                       EncodeCrudPairs(writable.values),
                                       writable.temporary_session_uuid});
    impl_->row_out << line << '\n';
    if (!impl_->row_out) {
      return MakeInvalidRequestDiagnostic("mga.row_store", "row_version_append_failed");
    }
    const auto scoped_appended = AppendScopedRelationBufferedLine(
        &impl_->scoped_row_out,
        &impl_->scoped_row_dirty_paths,
        ScopedRowStorePath(impl_->context, writable.table_uuid),
        line,
        "mga.row_store",
        "scoped_row_version_append_failed");
    if (scoped_appended.error) {
      return scoped_appended;
    }
    impl_->row_dirty = true;
    ++impl_->counters.row_versions_appended;
    if (written_event_sequences != nullptr) {
      written_event_sequences->push_back(writable.event_sequence);
    }
  }
  return OkDiagnostic();
}

EngineApiDiagnostic MgaRelationHotAppendContext::FlushRowVersions() {
  if (!impl_->row_out.is_open() || !impl_->row_dirty) {
    return OkDiagnostic();
  }
  impl_->row_out.flush();
  if (!impl_->row_out) {
    return MakeInvalidRequestDiagnostic("mga.row_store", "row_version_append_failed");
  }
  const auto scoped_flushed = FlushScopedRelationBufferedLines(
      &impl_->scoped_row_out,
      &impl_->scoped_row_dirty_paths,
      "mga.row_store",
      "scoped_row_version_append_failed");
  if (scoped_flushed.error) {
    return scoped_flushed;
  }
  impl_->row_dirty = false;
  ++impl_->counters.row_stream_flushes;
  return OkDiagnostic();
}

EngineApiDiagnostic MgaRelationHotAppendContext::AppendIndexEntryBatches(
    const std::vector<MgaIndexEntryAppendBatch>& batches) {
  if (impl_->context.database_path.empty()) {
    return MakeInvalidRequestDiagnostic("mga.index_store", "database_path_required");
  }
  std::uint64_t entry_count = 0;
  for (const auto& batch : batches) {
    if (batch.rows.empty()) { continue; }
    for (const auto& row : batch.rows) {
      entry_count += static_cast<std::uint64_t>(
          CrudIndexKeysForValues(batch.index, row.values).size());
    }
  }
  if (entry_count == 0) {
    return OkDiagnostic();
  }
  if (!impl_->index_out.is_open()) {
    impl_->index_out.open(IndexStorePath(impl_->context), std::ios::app | std::ios::binary);
    if (!impl_->index_out) {
      return MakeInvalidRequestDiagnostic("mga.index_store", "index_entry_append_failed");
    }
    ++impl_->counters.index_stream_opens;
  }
  const auto reservation = ReserveEventSequenceRange(
      impl_->context,
      "index_entries",
      IndexStorePath(impl_->context),
      entry_count,
      [this]() { return ScanNextIndexEventSequence(impl_->context); });
  if (!reservation.ok) { return reservation.diagnostic; }
  ++impl_->counters.index_range_reservations;
  std::uint64_t event_sequence = reservation.first;
  for (const auto& batch : batches) {
    if (batch.rows.empty()) { continue; }
    const std::string table_uuid =
        batch.index.table_uuid.empty() ? batch.table_uuid : batch.index.table_uuid;
    for (const auto& row : batch.rows) {
      for (const auto& key : CrudIndexKeysForValues(batch.index, row.values)) {
        const std::string payload = CrudFieldValue(row.values, batch.index.column_name);
        const std::string line = JoinLine({kRowStoreMagic,
                                           "INDEX_ENTRY",
                                           std::to_string(impl_->context.local_transaction_id),
                                           std::to_string(event_sequence++),
                                           batch.index.index_uuid,
                                           table_uuid,
                                           batch.index.column_name,
                                           batch.index.family,
                                           "exact",
                                           key,
                                           payload,
                                           row.row_uuid,
                                           row.version_uuid});
        impl_->index_out << line << '\n';
        if (!impl_->index_out) {
          return MakeInvalidRequestDiagnostic("mga.index_store", "index_entry_append_failed");
        }
        const auto scoped_appended = AppendScopedRelationBufferedLine(
            &impl_->scoped_index_out,
            &impl_->scoped_index_dirty_paths,
            ScopedIndexStorePath(impl_->context, table_uuid),
            line,
            "mga.index_store",
            "scoped_index_entry_append_failed");
        if (scoped_appended.error) {
          return scoped_appended;
        }
        impl_->index_dirty = true;
        ++impl_->counters.index_entries_appended;
      }
    }
  }
  return OkDiagnostic();
}

EngineApiDiagnostic MgaRelationHotAppendContext::AppendExactIndexEntryBatches(
    const std::vector<MgaExactIndexEntryAppendBatch>& batches) {
  if (impl_->context.database_path.empty()) {
    return MakeInvalidRequestDiagnostic("mga.index_store", "database_path_required");
  }
  std::uint64_t entry_count = 0;
  for (const auto& batch : batches) {
    entry_count += static_cast<std::uint64_t>(batch.entries.size());
  }
  if (entry_count == 0) {
    return OkDiagnostic();
  }
  if (!impl_->index_out.is_open()) {
    impl_->index_out.open(IndexStorePath(impl_->context), std::ios::app | std::ios::binary);
    if (!impl_->index_out) {
      return MakeInvalidRequestDiagnostic("mga.index_store", "index_entry_append_failed");
    }
    ++impl_->counters.index_stream_opens;
  }
  const auto reservation = ReserveEventSequenceRange(
      impl_->context,
      "index_entries",
      IndexStorePath(impl_->context),
      entry_count,
      [this]() { return ScanNextIndexEventSequence(impl_->context); });
  if (!reservation.ok) { return reservation.diagnostic; }
  ++impl_->counters.index_range_reservations;
  std::uint64_t event_sequence = reservation.first;
  for (const auto& batch : batches) {
    if (batch.entries.empty()) { continue; }
    const std::string table_uuid =
        batch.index.table_uuid.empty() ? batch.table_uuid : batch.index.table_uuid;
    for (const auto& entry : batch.entries) {
      if (entry.encoded_key.empty() || entry.row_uuid.empty() || entry.version_uuid.empty()) {
        return MakeInvalidRequestDiagnostic("mga.index_store", "exact_index_entry_invalid");
      }
      const std::string line = JoinLine({kRowStoreMagic,
                                         "INDEX_ENTRY",
                                         std::to_string(impl_->context.local_transaction_id),
                                         std::to_string(event_sequence++),
                                         batch.index.index_uuid,
                                         table_uuid,
                                         batch.index.column_name,
                                         batch.index.family,
                                         "exact",
                                         entry.encoded_key,
                                         entry.payload_value,
                                         entry.row_uuid,
                                         entry.version_uuid});
      impl_->index_out << line << '\n';
      if (!impl_->index_out) {
        return MakeInvalidRequestDiagnostic("mga.index_store", "index_entry_append_failed");
      }
      const auto scoped_appended = AppendScopedRelationBufferedLine(
          &impl_->scoped_index_out,
          &impl_->scoped_index_dirty_paths,
          ScopedIndexStorePath(impl_->context, table_uuid),
          line,
          "mga.index_store",
          "scoped_index_entry_append_failed");
      if (scoped_appended.error) {
        return scoped_appended;
      }
      impl_->index_dirty = true;
      ++impl_->counters.index_entries_appended;
    }
  }
  return OkDiagnostic();
}

EngineApiDiagnostic MgaRelationHotAppendContext::FlushIndexEntries() {
  if (!impl_->index_out.is_open() || !impl_->index_dirty) {
    return OkDiagnostic();
  }
  impl_->index_out.flush();
  if (!impl_->index_out) {
    return MakeInvalidRequestDiagnostic("mga.index_store", "index_entry_append_failed");
  }
  const auto scoped_flushed = FlushScopedRelationBufferedLines(
      &impl_->scoped_index_out,
      &impl_->scoped_index_dirty_paths,
      "mga.index_store",
      "scoped_index_entry_append_failed");
  if (scoped_flushed.error) {
    return scoped_flushed;
  }
  impl_->index_dirty = false;
  ++impl_->counters.index_stream_flushes;
  return OkDiagnostic();
}

EngineApiDiagnostic MgaRelationHotAppendContext::Flush() {
  const auto rows = FlushRowVersions();
  if (rows.error) { return rows; }
  return FlushIndexEntries();
}

EngineApiDiagnostic AppendMgaRowVersions(const EngineRequestContext& context,
                                          std::vector<CrudRowVersionRecord>* rows,
                                          std::vector<std::uint64_t>* written_event_sequences) {
  MgaRelationHotAppendContext append_context(context);
  const auto appended = append_context.AppendRowVersions(rows, written_event_sequences);
  if (appended.error) { return appended; }
  return append_context.FlushRowVersions();
}

EngineApiDiagnostic AppendMgaTableMetadata(const EngineRequestContext& context,
                                           const CrudTableRecord& table) {
  if (context.database_path.empty()) {
    return MakeInvalidRequestDiagnostic("mga.relation_metadata", "database_path_required");
  }
  const auto authority = ValidateMgaMutatingTransactionAuthority(context, "mga.relation_metadata.table_create");
  if (authority.error) { return authority; }
  CrudTableRecord writable = table;
  writable.creator_tx = context.local_transaction_id;
  const auto reservation = ReserveEventSequenceRange(
      context,
      "relation_metadata",
      MetadataStorePath(context),
      1,
      [&context]() { return ScanNextMetadataEventSequence(context); });
  if (!reservation.ok) { return reservation.diagnostic; }
  writable.event_sequence = reservation.first;
  const std::string line = JoinLine({kRowStoreMagic,
                                     "TABLE_METADATA",
                                     std::to_string(writable.creator_tx),
                                     std::to_string(writable.event_sequence),
                                     writable.table_uuid,
                                     EncodeCrudText(writable.default_name),
                                     EncodeCrudPairs(writable.columns),
                                     writable.temporary ? "1" : "0",
                                     writable.temporary_scope,
                                     writable.temporary_session_uuid,
                                     writable.on_commit_action});
  if (!AppendLine(MetadataStorePath(context), line)) {
    return MakeInvalidRequestDiagnostic("mga.relation_metadata", "table_metadata_append_failed");
  }
  return OkDiagnostic();
}

EngineApiDiagnostic AppendMgaIndexMetadata(const EngineRequestContext& context,
                                           const CrudIndexRecord& index) {
  if (context.database_path.empty()) {
    return MakeInvalidRequestDiagnostic("mga.relation_metadata", "database_path_required");
  }
  const auto authority = ValidateMgaMutatingTransactionAuthority(context, "mga.relation_metadata.index_create");
  if (authority.error) { return authority; }
  CrudIndexRecord writable = index;
  writable.creator_tx = context.local_transaction_id;
  const auto reservation = ReserveEventSequenceRange(
      context,
      "relation_metadata",
      MetadataStorePath(context),
      1,
      [&context]() { return ScanNextMetadataEventSequence(context); });
  if (!reservation.ok) { return reservation.diagnostic; }
  writable.event_sequence = reservation.first;
  const std::string line = JoinLine({kRowStoreMagic,
                                     "INDEX_METADATA",
                                     std::to_string(writable.creator_tx),
                                     std::to_string(writable.event_sequence),
                                     writable.index_uuid,
                                     writable.table_uuid,
                                     NormalizeCrudIndexProfile(writable.profile),
                                     writable.family.empty() ? CrudIndexFamilyForProfile(writable.profile) : writable.family,
                                     EncodeCrudText(writable.default_name),
                                     EncodeCrudText(writable.column_name),
                                     EncodeStringListAsCrudPairs(writable.key_envelopes),
                                     EncodeStringListAsCrudPairs(writable.include_columns),
                                     writable.predicate_kind,
                                     EncodeCrudText(writable.predicate_column),
                                     EncodeCrudText(writable.predicate_value),
                                     writable.unique ? "1" : "0",
                                     writable.exact_fallback ? "1" : "0"});
  if (!AppendLine(MetadataStorePath(context), line)) {
    return MakeInvalidRequestDiagnostic("mga.relation_metadata", "index_metadata_append_failed");
  }
  return OkDiagnostic();
}

EngineApiDiagnostic AppendMgaIndexEntriesForRow(const EngineRequestContext& context,
                                                const CrudState& state,
                                                const std::string& table_uuid,
                                                const std::string& row_uuid,
                                                const std::string& version_uuid,
                                                const std::vector<std::pair<std::string, std::string>>& values) {
  return AppendMgaIndexEntriesForRows(context,
                                      state,
                                      table_uuid,
                                      std::vector<MgaIndexEntryRowInput>{{row_uuid, version_uuid, values}});
}

EngineApiDiagnostic AppendMgaIndexEntriesForRows(const EngineRequestContext& context,
                                                 const CrudState& state,
                                                 const std::string& table_uuid,
                                                 const std::vector<MgaIndexEntryRowInput>& rows) {
  if (rows.empty()) {
    return OkDiagnostic();
  }
  const auto indexes = VisibleCrudIndexesForTable(state, table_uuid, context.local_transaction_id);
  if (indexes.empty()) {
    return OkDiagnostic();
  }
  return AppendMgaIndexEntriesForRowsWithIndexes(context, indexes, table_uuid, rows);
}

EngineApiDiagnostic AppendMgaIndexEntriesForRowsWithIndexes(const EngineRequestContext& context,
                                                            const std::vector<CrudIndexRecord>& indexes,
                                                            const std::string& table_uuid,
                                                            const std::vector<MgaIndexEntryRowInput>& rows) {
  if (rows.empty() || indexes.empty()) {
    return OkDiagnostic();
  }
  std::vector<MgaIndexEntryAppendBatch> batches;
  batches.reserve(indexes.size());
  for (const auto& index : indexes) {
    MgaIndexEntryAppendBatch batch;
    batch.index = index;
    batch.table_uuid = table_uuid;
    batch.rows = rows;
    batches.push_back(std::move(batch));
  }
  MgaRelationHotAppendContext append_context(context);
  const auto appended = append_context.AppendIndexEntryBatches(batches);
  if (appended.error) { return appended; }
  return append_context.FlushIndexEntries();
}

EngineApiDiagnostic AppendMgaExactIndexEntryBatches(
    const EngineRequestContext& context,
    const std::vector<MgaExactIndexEntryAppendBatch>& batches) {
  bool empty = true;
  for (const auto& batch : batches) {
    if (!batch.entries.empty()) {
      empty = false;
      break;
    }
  }
  if (empty) {
    return OkDiagnostic();
  }
  MgaRelationHotAppendContext append_context(context);
  const auto appended = append_context.AppendExactIndexEntryBatches(batches);
  if (appended.error) { return appended; }
  return append_context.FlushIndexEntries();
}

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
  const auto authority = ValidateMgaMutatingTransactionAuthority(
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
  CrudState state = BuildCrudCompatibilityStateFromMga(loaded_state.state);
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
    const bool eligible =
        record.commit_state == idx::SecondaryIndexDeltaLedgerCommitState::committed_premerge &&
        record.delta.committed &&
        record.delta.local_transaction_id <= request.authoritative_cleanup_horizon_local_transaction_id;
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
    staged_delta_ledger.deltas.push_back(record.delta);
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
  CrudState state = BuildCrudCompatibilityStateFromMga(loaded_state.state);
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
          staged.commit_state =
              idx::SecondaryIndexDeltaLedgerCommitState::committed_premerge;
          staged.delta.committed = true;
          ++result.promoted_count;
          changed = true;
        } else if (IsDpc025RolledBackTerminal(lookup.entry.state)) {
          keep_record = false;
          ++result.removed_count;
          changed = true;
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
  CrudState state = BuildCrudCompatibilityStateFromMga(loaded_state.state);
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
    const CrudState& state,
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
    if (record.commit_state ==
        idx::SecondaryIndexDeltaLedgerCommitState::precommit_uncommitted &&
        record.delta.local_transaction_id != context.local_transaction_id) {
      continue;
    }
    overlay_ledger.deltas.push_back(record.delta);
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

EngineApiDiagnostic AppendMgaIndexEntriesForIndex(const EngineRequestContext& context,
                                                  const CrudIndexRecord& index,
                                                  const std::string& row_uuid,
                                                  const std::string& version_uuid,
                                                  const std::vector<std::pair<std::string, std::string>>& values) {
  MgaIndexEntryAppendBatch batch;
  batch.index = index;
  batch.table_uuid = index.table_uuid;
  batch.rows.push_back(MgaIndexEntryRowInput{row_uuid, version_uuid, values});
  MgaRelationHotAppendContext append_context(context);
  const auto appended = append_context.AppendIndexEntryBatches({std::move(batch)});
  if (appended.error) { return appended; }
  return append_context.FlushIndexEntries();
}

EngineApiDiagnostic PersistMgaLargeValuesForRow(const EngineRequestContext& context,
                                                const std::string& table_uuid,
                                                const std::string& row_uuid,
                                                const std::string& version_uuid,
                                                bool force_large_value,
                                                std::vector<std::pair<std::string, std::string>>* values,
                                                std::vector<EngineEvidenceReference>* evidence) {
  MgaLargeValuePersistBatchCounters counters;
  return PersistMgaLargeValuesForRows(
      context,
      {MgaLargeValuePersistBatchRowInput{table_uuid,
                                         row_uuid,
                                         version_uuid,
                                         force_large_value,
                                         values}},
      &counters,
      evidence);
}

EngineApiDiagnostic PersistMgaLargeValuesForRows(
    const EngineRequestContext& context,
    const std::vector<MgaLargeValuePersistBatchRowInput>& rows,
    MgaLargeValuePersistBatchCounters* counters,
    std::vector<EngineEvidenceReference>* evidence) {
  MgaLargeValuePersistBatchCounters local_counters;
  if (counters == nullptr) {
    counters = &local_counters;
  }
  *counters = MgaLargeValuePersistBatchCounters{};
  counters->rows_seen = static_cast<std::uint64_t>(rows.size());

  struct PendingValueMutation {
    std::vector<std::pair<std::string, std::string>>* values = nullptr;
    std::vector<std::pair<std::string, std::string>> replacement_values;
  };

  std::vector<PendingValueMutation> pending_mutations;
  pending_mutations.reserve(rows.size());
  std::vector<EngineEvidenceReference> pending_evidence;
  std::vector<std::string> lines;

  for (const auto& row : rows) {
    if (row.values == nullptr) {
      return MakeInvalidRequestDiagnostic("mga.large_value", "values_required");
    }
    PendingValueMutation mutation;
    mutation.values = row.values;
    mutation.replacement_values = *row.values;
    bool force_one_remaining = row.force_large_value;
    while (EncodedValueBytes(mutation.replacement_values) >
               kCrudVerticalSliceMaxEncodedValueBytes ||
           force_one_remaining) {
      auto selected = mutation.replacement_values.end();
      for (auto it = mutation.replacement_values.begin();
           it != mutation.replacement_values.end();
           ++it) {
        if (it->second == "<NULL>" ||
            CrudValueIsLargeValueLocator(it->second) ||
            IsMgaLargeValueLocator(it->second)) {
          continue;
        }
        if (selected == mutation.replacement_values.end() ||
            it->second.size() > selected->second.size()) {
          selected = it;
        }
      }
      if (selected == mutation.replacement_values.end()) {
        return MakeInvalidRequestDiagnostic("mga.large_value",
                                            "no_overflow_candidate_available");
      }

      force_one_remaining = false;
      const std::string original = selected->second;
      const std::string overflow_uuid = GenerateCrudEngineUuid("object");
      const std::string content_hash = std::to_string(ChecksumText(original));
      const std::uint64_t total_bytes =
          static_cast<std::uint64_t>(original.size());
      lines.push_back(JoinLine({kRowStoreMagic,
                                "LARGE_VALUE",
                                std::to_string(context.local_transaction_id),
                                overflow_uuid,
                                row.table_uuid,
                                row.row_uuid,
                                row.version_uuid,
                                selected->first,
                                std::to_string(total_bytes),
                                content_hash,
                                "durable_uncommitted"}));
      ++counters->values_overflowed;
      counters->payload_bytes += total_bytes;

      std::uint64_t ordinal = 0;
      for (std::size_t offset = 0; offset < original.size();
           offset += kMgaLargeValueChunkBytes) {
        const std::size_t end =
            std::min<std::size_t>(original.size(),
                                  offset + kMgaLargeValueChunkBytes);
        const std::string fragment = original.substr(offset, end - offset);
        lines.push_back(JoinLine({kRowStoreMagic,
                                  "LARGE_VALUE_CHUNK",
                                  std::to_string(context.local_transaction_id),
                                  overflow_uuid,
                                  std::to_string(ordinal++),
                                  EncodeCrudText(fragment),
                                  std::to_string(ChecksumText(fragment))}));
        ++counters->chunks_appended;
      }
      selected->second = MakeMgaLargeValueLocator(overflow_uuid,
                                                  content_hash,
                                                  total_bytes);
      pending_evidence.push_back({"mga_large_value_overflow", overflow_uuid});
    }
    pending_mutations.push_back(std::move(mutation));
  }

  counters->preallocated_chunk_slots = counters->chunks_appended;
  counters->store_lines_appended = static_cast<std::uint64_t>(lines.size());
  if (!AppendLines(LargeValueStorePath(context),
                   lines,
                   &counters->stream_opens,
                   &counters->stream_flushes)) {
    return MakeInvalidRequestDiagnostic("mga.large_value",
                                        "large_value_batch_append_failed");
  }

  for (auto& mutation : pending_mutations) {
    *mutation.values = std::move(mutation.replacement_values);
  }

  if (evidence != nullptr && counters->values_overflowed != 0) {
    evidence->insert(evidence->end(),
                     pending_evidence.begin(),
                     pending_evidence.end());
    evidence->push_back({"mga_large_value_batch_writer", "window"});
    evidence->push_back({"mga_large_value_batch_rows",
                         std::to_string(counters->rows_seen)});
    evidence->push_back({"mga_large_value_batch_overflows",
                         std::to_string(counters->values_overflowed)});
    evidence->push_back({"mga_large_value_batch_chunks",
                         std::to_string(counters->chunks_appended)});
    evidence->push_back({"mga_large_value_batch_preallocated_chunks",
                         std::to_string(counters->preallocated_chunk_slots)});
    evidence->push_back({"mga_large_value_batch_payload_bytes",
                         std::to_string(counters->payload_bytes)});
    evidence->push_back({"mga_large_value_batch_stream_opens",
                         std::to_string(counters->stream_opens)});
    evidence->push_back({"mga_large_value_batch_stream_flushes",
                         std::to_string(counters->stream_flushes)});
  }
  return OkDiagnostic();
}

EngineApiDiagnostic AppendMgaLargeValueReclaimMarkersForRowVersion(
    const EngineRequestContext& context,
    std::uint64_t local_transaction_id,
    const CrudRowVersionRecord& row,
    const std::string& cleanup_reason,
    std::set<std::string>* already_reclaimed_overflow_uuids,
    std::uint64_t* reclaimed_count) {
  if (already_reclaimed_overflow_uuids == nullptr || reclaimed_count == nullptr) {
    return MakeInvalidRequestDiagnostic("mga.large_value", "reclaim_state_required");
  }
  for (const auto& line : ReadLines(LargeValueStorePath(context))) {
    const auto fields = SplitTabs(line);
    if (fields.size() < 11 || fields[0] != kRowStoreMagic ||
        fields[1] != "LARGE_VALUE" ||
        fields[4] != row.table_uuid ||
        fields[5] != row.row_uuid ||
        fields[6] != row.version_uuid) {
      continue;
    }
    const std::string& overflow_uuid = fields[3];
    if (!already_reclaimed_overflow_uuids->insert(overflow_uuid).second) {
      continue;
    }
    const std::string reclaim_line =
        JoinLine({kRowStoreMagic,
                  "LARGE_VALUE_RECLAIMED",
                  std::to_string(local_transaction_id),
                  overflow_uuid,
                  row.table_uuid,
                  row.row_uuid,
                  row.version_uuid,
                  fields[7],
                  cleanup_reason});
    if (!AppendLine(LargeValueStorePath(context), reclaim_line)) {
      return MakeInvalidRequestDiagnostic("mga.large_value",
                                          "large_value_reclaim_append_failed");
    }
    ++(*reclaimed_count);
  }
  return OkDiagnostic();
}

EngineApiDiagnostic AppendMgaTemporaryTableMetadataRetirement(
    const EngineRequestContext& context,
    std::uint64_t local_transaction_id,
    const CrudTableRecord& table,
    const std::string& cleanup_reason) {
  if (context.database_path.empty()) {
    return MakeInvalidRequestDiagnostic("mga.temporary_session_cleanup",
                                        "database_path_required");
  }
  const auto reservation = ReserveEventSequenceRange(
      context,
      "relation_metadata",
      MetadataStorePath(context),
      1,
      [&context]() { return ScanNextMetadataEventSequence(context); });
  if (!reservation.ok) { return reservation.diagnostic; }
  const std::string line = JoinLine({kRowStoreMagic,
                                     "TABLE_METADATA_RETIRED",
                                     std::to_string(local_transaction_id),
                                     std::to_string(reservation.first),
                                     table.table_uuid,
                                     cleanup_reason,
                                     table.temporary_session_uuid});
  if (!AppendLine(MetadataStorePath(context), line)) {
    return MakeInvalidRequestDiagnostic("mga.temporary_session_cleanup",
                                        "table_metadata_retire_append_failed");
  }
  return OkDiagnostic();
}

EngineApiDiagnostic ApplyMgaTemporaryCleanupActions(
    const EngineRequestContext& context,
    std::uint64_t local_transaction_id,
    const std::string& cleanup_reason,
    bool include_delete_rows,
    bool include_preserve_rows,
    bool retire_private_metadata,
    std::uint64_t* deleted_row_count,
    std::uint64_t* reclaimed_large_value_count,
    std::uint64_t* retired_private_metadata_count) {
  if (deleted_row_count != nullptr) { *deleted_row_count = 0; }
  if (reclaimed_large_value_count != nullptr) { *reclaimed_large_value_count = 0; }
  if (retired_private_metadata_count != nullptr) {
    *retired_private_metadata_count = 0;
  }
  if (context.session_uuid.canonical.empty()) {
    return MakeInvalidRequestDiagnostic("mga.temporary_session_cleanup",
                                        "session_uuid_required");
  }
  if (local_transaction_id == 0) {
    return MakeInvalidRequestDiagnostic("mga.temporary_session_cleanup",
                                        "local_transaction_id_required");
  }
  auto load_context = context;
  load_context.local_transaction_id = local_transaction_id;
  auto loaded = LoadMgaRelationStoreState(load_context);
  if (!loaded.ok) { return loaded.diagnostic; }
  CrudState state = BuildCrudCompatibilityStateFromMga(loaded.state);
  const auto visible_reclaims = LoadVisibleMgaLargeValueReclaims(context);
  if (visible_reclaims.diagnostic.error) { return visible_reclaims.diagnostic; }
  std::set<std::string> already_reclaimed_overflow_uuids =
      visible_reclaims.overflow_uuids;
  std::uint64_t deleted = 0;
  std::uint64_t reclaimed = 0;
  std::uint64_t retired_metadata = 0;
  for (const auto& table : state.tables) {
    if (!table.temporary) { continue; }
    const bool delete_rows_policy = table.on_commit_action == "delete_rows";
    const bool preserve_rows_policy = table.on_commit_action == "preserve_rows";
    if ((delete_rows_policy && !include_delete_rows) ||
        (preserve_rows_policy && !include_preserve_rows) ||
        (!delete_rows_policy && !preserve_rows_policy)) {
      continue;
    }
    auto row_context = context;
    row_context.local_transaction_id = local_transaction_id;
    const auto rows =
        VisibleCrudRowsForContext(state, table.table_uuid, row_context);
    std::set<std::string> visible_row_uuids;
    for (const auto& row : rows) { visible_row_uuids.insert(row.row_uuid); }
    for (const auto& row_version : state.row_versions) {
      if (row_version.table_uuid != table.table_uuid ||
          visible_row_uuids.count(row_version.row_uuid) == 0 ||
          !CrudRowVersionVisibleToContext(state, row_version, row_context)) {
        continue;
      }
      const auto reclaimed_large = AppendMgaLargeValueReclaimMarkersForRowVersion(
          context,
          local_transaction_id,
          row_version,
          cleanup_reason,
          &already_reclaimed_overflow_uuids,
          &reclaimed);
      if (reclaimed_large.error) { return reclaimed_large; }
    }
    for (const auto& row : rows) {
      CrudRowVersionRecord tombstone;
      tombstone.creator_tx = local_transaction_id;
      tombstone.table_uuid = table.table_uuid;
      tombstone.row_uuid = row.row_uuid;
      tombstone.version_uuid = GenerateCrudEngineUuid("row");
      tombstone.temporary_session_uuid = row.temporary_session_uuid;
      tombstone.previous_version_uuid = row.version_uuid;
      tombstone.previous_sequence = row.sequence;
      tombstone.deleted = true;
      const auto appended = AppendMgaRowVersion(context, tombstone, nullptr);
      if (appended.error) { return appended; }
      ++deleted;
    }
    if (retire_private_metadata &&
        table.temporary_scope != "global" &&
        table.temporary_session_uuid == context.session_uuid.canonical) {
      const auto retired = AppendMgaTemporaryTableMetadataRetirement(
          context,
          local_transaction_id,
          table,
          cleanup_reason);
      if (retired.error) { return retired; }
      ++retired_metadata;
    }
  }
  if (deleted_row_count != nullptr) { *deleted_row_count = deleted; }
  if (reclaimed_large_value_count != nullptr) {
    *reclaimed_large_value_count = reclaimed;
  }
  if (retired_private_metadata_count != nullptr) {
    *retired_private_metadata_count = retired_metadata;
  }
  return OkDiagnostic();
}

EngineApiDiagnostic CreateMgaSavepointMarker(const EngineRequestContext& context, const std::string& savepoint_name) {
  if (context.local_transaction_id == 0) {
    return MakeInvalidRequestDiagnostic("transaction.create_savepoint", "local_transaction_id_required");
  }
  const std::uint64_t row_cutoff = NextRowEventSequence(context) - 1;
  const std::uint64_t metadata_cutoff = NextMetadataEventSequence(context) - 1;
  const std::uint64_t index_cutoff = NextIndexEventSequence(context) - 1;
  const std::string line = JoinLine({kRowStoreMagic,
                                     "SAVEPOINT",
                                     std::to_string(context.local_transaction_id),
                                     EncodeCrudText(savepoint_name),
                                     std::to_string(row_cutoff),
                                     std::to_string(metadata_cutoff),
                                     std::to_string(index_cutoff)});
  if (!AppendLine(SavepointStorePath(context), line)) {
    return MakeInvalidRequestDiagnostic("transaction.create_savepoint", "savepoint_append_failed");
  }
  return OkDiagnostic();
}

EngineApiDiagnostic ReleaseMgaSavepointMarker(const EngineRequestContext& context, const std::string& savepoint_name) {
  const auto exists = ValidateMgaSavepointExists(context, savepoint_name, "transaction.release_savepoint");
  if (exists.error) { return exists; }
  const std::uint64_t row_cutoff = NextRowEventSequence(context) - 1;
  const std::uint64_t metadata_cutoff = NextMetadataEventSequence(context) - 1;
  const std::uint64_t index_cutoff = NextIndexEventSequence(context) - 1;
  const std::string line = JoinLine({kRowStoreMagic,
                                     "RELEASE_SAVEPOINT",
                                     std::to_string(context.local_transaction_id),
                                     EncodeCrudText(savepoint_name),
                                     std::to_string(row_cutoff),
                                     std::to_string(metadata_cutoff),
                                     std::to_string(index_cutoff)});
  if (!AppendLine(SavepointStorePath(context), line)) {
    return MakeInvalidRequestDiagnostic("transaction.release_savepoint", "savepoint_release_append_failed");
  }
  return OkDiagnostic();
}

EngineApiDiagnostic RollbackToMgaSavepointMarker(const EngineRequestContext& context, const std::string& savepoint_name) {
  const auto savepoints = ParseSavepoints(context);
  const auto tx_it = savepoints.active_savepoints.find(context.local_transaction_id);
  if (tx_it == savepoints.active_savepoints.end()) {
    return MakeInvalidRequestDiagnostic("transaction.rollback_to_savepoint", "savepoint_not_found");
  }
  const auto savepoint_it = tx_it->second.find(savepoint_name);
  if (savepoint_it == tx_it->second.end()) {
    return MakeInvalidRequestDiagnostic("transaction.rollback_to_savepoint", "savepoint_not_found");
  }
  const std::uint64_t row_upper = NextRowEventSequence(context) - 1;
  const std::uint64_t metadata_upper = NextMetadataEventSequence(context) - 1;
  const std::uint64_t index_upper = NextIndexEventSequence(context) - 1;
  const std::string line = JoinLine({kRowStoreMagic,
                                     "ROLLBACK_TO_SAVEPOINT",
                                     std::to_string(context.local_transaction_id),
                                     EncodeCrudText(savepoint_name),
                                     std::to_string(savepoint_it->second.row_event_sequence),
                                     std::to_string(savepoint_it->second.metadata_event_sequence),
                                     std::to_string(savepoint_it->second.index_event_sequence),
                                     std::to_string(row_upper),
                                     std::to_string(metadata_upper),
                                     std::to_string(index_upper)});
  if (!AppendLine(SavepointStorePath(context), line)) {
    return MakeInvalidRequestDiagnostic("transaction.rollback_to_savepoint", "savepoint_rollback_append_failed");
  }
  return OkDiagnostic();
}

EngineApiDiagnostic ValidateMgaSavepointExists(const EngineRequestContext& context,
                                               const std::string& savepoint_name,
                                               const std::string& operation_id) {
  if (context.local_transaction_id == 0) {
    return MakeInvalidRequestDiagnostic(operation_id, "local_transaction_id_required");
  }
  const auto savepoints = ParseSavepoints(context);
  const auto tx_it = savepoints.active_savepoints.find(context.local_transaction_id);
  if (tx_it == savepoints.active_savepoints.end() ||
      tx_it->second.find(savepoint_name) == tx_it->second.end()) {
    return MakeInvalidRequestDiagnostic(operation_id, "savepoint_not_found");
  }
  return OkDiagnostic();
}

std::vector<std::string> ActiveMgaSavepointNames(const EngineRequestContext& context) {
  std::vector<std::string> names;
  if (context.local_transaction_id == 0 || context.database_path.empty()) {
    return names;
  }
  const auto savepoints = ParseSavepoints(context);
  const auto tx_it = savepoints.active_savepoints.find(context.local_transaction_id);
  if (tx_it == savepoints.active_savepoints.end()) {
    return names;
  }
  names.reserve(tx_it->second.size());
  for (const auto& entry : tx_it->second) {
    names.push_back(entry.first);
  }
  return names;
}

EngineApiDiagnostic ApplyMgaTemporaryOnCommitActions(const EngineRequestContext& context,
                                                     std::uint64_t local_transaction_id,
                                                     std::uint64_t* deleted_row_count,
                                                     std::uint64_t* reclaimed_large_value_count) {
  return ApplyMgaTemporaryCleanupActions(context,
                                         local_transaction_id,
                                         "temporary_on_commit_delete_rows",
                                         true,
                                         false,
                                         false,
                                         deleted_row_count,
                                         reclaimed_large_value_count,
                                         nullptr);
}

EngineApiDiagnostic ApplyMgaTemporarySessionCleanupActions(
    const EngineRequestContext& context,
    std::uint64_t local_transaction_id,
    std::uint64_t* deleted_row_count,
    std::uint64_t* reclaimed_large_value_count,
    std::uint64_t* retired_private_metadata_count) {
  return ApplyMgaTemporaryCleanupActions(context,
                                         local_transaction_id,
                                         "temporary_session_cleanup",
                                         true,
                                         true,
                                         true,
                                         deleted_row_count,
                                         reclaimed_large_value_count,
                                         retired_private_metadata_count);
}

MgaTemporaryTableDropResult DropMgaTemporaryTable(
    const EngineRequestContext& context,
    const std::string& table_uuid) {
  MgaTemporaryTableDropResult result;
  if (table_uuid.empty()) {
    result.diagnostic = MakeInvalidRequestDiagnostic("ddl.drop_object",
                                                     "target_table_uuid_required");
    return result;
  }
  const auto visibility = CheckMgaTemporaryTableVisibility(context, table_uuid);
  if (!visibility.ok) {
    result.diagnostic = visibility.diagnostic;
    return result;
  }
  if (!visibility.known_temporary) {
    result.ok = true;
    result.diagnostic = OkDiagnostic();
    return result;
  }
  result.target_was_temporary = true;
  if (context.session_uuid.canonical.empty()) {
    result.diagnostic = MakeInvalidRequestDiagnostic(
        "ddl.drop_object",
        "temporary_table_requires_session_uuid");
    return result;
  }
  if (!visibility.table_visible || !visibility.visible_to_session) {
    result.diagnostic = MakeInvalidRequestDiagnostic(
        "ddl.drop_object",
        "target_temporary_table_not_visible");
    return result;
  }
  if (context.local_transaction_id == 0) {
    result.diagnostic = MakeInvalidRequestDiagnostic(
        "ddl.drop_object",
        "local_transaction_id_required");
    return result;
  }

  const auto authority =
      ValidateMgaMutatingTransactionAuthority(context, "ddl.drop_object");
  if (authority.error) {
    result.diagnostic = authority;
    return result;
  }

  auto loaded = LoadMgaRelationStoreState(context);
  if (!loaded.ok) {
    result.diagnostic = loaded.diagnostic;
    return result;
  }
  CrudState state = BuildCrudCompatibilityStateFromMga(loaded.state);
  const auto table = FindVisibleCrudTable(
      state,
      table_uuid,
      context.local_transaction_id);
  if (!table || !table->temporary) {
    result.diagnostic = MakeInvalidRequestDiagnostic(
        "ddl.drop_object",
        "target_temporary_table_not_visible");
    return result;
  }

  const auto visible_reclaims = LoadVisibleMgaLargeValueReclaims(context);
  if (visible_reclaims.diagnostic.error) {
    result.diagnostic = visible_reclaims.diagnostic;
    return result;
  }
  std::set<std::string> already_reclaimed_overflow_uuids =
      visible_reclaims.overflow_uuids;

  auto row_context = context;
  const auto rows = VisibleCrudRowsForContext(state, table_uuid, row_context);
  std::set<std::string> visible_row_uuids;
  for (const auto& row : rows) { visible_row_uuids.insert(row.row_uuid); }
  for (const auto& row_version : state.row_versions) {
    if (row_version.table_uuid != table_uuid ||
        visible_row_uuids.count(row_version.row_uuid) == 0 ||
        !CrudRowVersionVisibleToContext(state, row_version, row_context)) {
      continue;
    }
    const auto reclaimed_large = AppendMgaLargeValueReclaimMarkersForRowVersion(
        context,
        context.local_transaction_id,
        row_version,
        "temporary_table_drop",
        &already_reclaimed_overflow_uuids,
        &result.reclaimed_large_value_count);
    if (reclaimed_large.error) {
      result.diagnostic = reclaimed_large;
      return result;
    }
  }
  for (const auto& row : rows) {
    CrudRowVersionRecord tombstone;
    tombstone.creator_tx = context.local_transaction_id;
    tombstone.table_uuid = row.table_uuid;
    tombstone.row_uuid = row.row_uuid;
    tombstone.version_uuid = GenerateCrudEngineUuid("row");
    tombstone.temporary_session_uuid = row.temporary_session_uuid;
    tombstone.previous_version_uuid = row.version_uuid;
    tombstone.previous_sequence = row.sequence;
    tombstone.deleted = true;
    const auto appended = AppendMgaRowVersion(context, tombstone, nullptr);
    if (appended.error) {
      result.diagnostic = appended;
      return result;
    }
    ++result.deleted_row_count;
  }

  const auto retired = AppendMgaTemporaryTableMetadataRetirement(
      context,
      context.local_transaction_id,
      *table,
      "temporary_table_drop");
  if (retired.error) {
    result.diagnostic = retired;
    return result;
  }

  result.ok = true;
  result.diagnostic = OkDiagnostic();
  result.metadata_retired = true;
  result.temporary_scope = table->temporary_scope;
  return result;
}

void ClearMgaEventSequenceRangeCacheForTesting() {
  const std::lock_guard<std::mutex> guard(EventSequenceCacheMutex());
  EventSequenceCache().clear();
}

MgaEventSequenceRangeReservation ReserveMgaRowEventSequenceRangeForTesting(
    const EngineRequestContext& context,
    std::uint64_t count) {
  return ReserveEventSequenceRange(
      context,
      "row_versions",
      RowStorePath(context),
      count,
      [&context]() { return ScanNextRowEventSequence(context); });
}

}  // namespace scratchbird::engine::internal_api
