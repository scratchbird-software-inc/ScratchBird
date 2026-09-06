// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "mga_relation_store/mga_savepoint_store.hpp"
#include "mga_relation_store/mga_event_sequence_allocator.hpp"
#include "mga_relation_store/mga_update_durable_store.hpp"

#include "api_diagnostics.hpp"
#include "crud_support/crud_store.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api {
namespace {

// SEARCH_KEY: SB_ENGINE_MGA_SAVEPOINT_STORE_IMPLEMENTATION_AUTHORITY
// Owns durable savepoint markers and their rollback-range projection.
// Savepoints remain transaction-local boundaries and never allocate or publish
// independent transaction finality.

constexpr const char* kRowStoreMagic = "SBMGA1";
constexpr std::string_view kDmlUpdateStatementSavepointCreateKind =
    "DML_UPDATE_STATEMENT_SAVEPOINT_CREATE_V1";
constexpr std::string_view kDmlUpdateStatementSavepointRollbackKind =
    "DML_UPDATE_STATEMENT_SAVEPOINT_ROLLBACK_V1";
constexpr std::string_view kDmlUpdateStatementSavepointReleaseKind =
    "DML_UPDATE_STATEMENT_SAVEPOINT_RELEASE_V1";

std::string SavepointStorePath(const EngineRequestContext& context) {
  return context.database_path + ".sb.mga_savepoints";
}

std::vector<std::string> ReadLines(const std::string& path) {
  std::vector<std::string> lines;
  std::ifstream in(path, std::ios::binary);
  std::string line;
  while (std::getline(in, line)) { lines.push_back(std::move(line)); }
  return lines;
}

std::vector<std::string> SplitTabs(const std::string& line) {
  std::vector<std::string> fields;
  std::size_t start = 0;
  while (start <= line.size()) {
    const auto tab = line.find('\t', start);
    fields.push_back(line.substr(start, tab == std::string::npos
                                           ? std::string::npos
                                           : tab - start));
    if (tab == std::string::npos) { break; }
    start = tab + 1;
  }
  return fields;
}

std::uint64_t ParseU64(const std::string& text) {
  std::uint64_t value = 0;
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), value);
  return parsed.ec == std::errc{} ? value : 0;
}

int HexValue(const char c) {
  if (c >= '0' && c <= '9') { return c - '0'; }
  if (c >= 'a' && c <= 'f') { return 10 + c - 'a'; }
  if (c >= 'A' && c <= 'F') { return 10 + c - 'A'; }
  return -1;
}

std::string DecodeCrudTextLocal(const std::string& encoded) {
  if ((encoded.size() % 2) != 0) { return {}; }
  std::string decoded;
  decoded.reserve(encoded.size() / 2);
  for (std::size_t index = 0; index < encoded.size(); index += 2) {
    const int high = HexValue(encoded[index]);
    const int low = HexValue(encoded[index + 1]);
    if (high < 0 || low < 0) { return {}; }
    decoded.push_back(static_cast<char>((high << 4) | low));
  }
  return decoded;
}

bool AppendLine(const std::string& path, const std::string& line) {
  std::ofstream out(path, std::ios::app | std::ios::binary);
  if (!out) { return false; }
  out << line << '\n';
  out.flush();
  return static_cast<bool>(out);
}

std::string JoinLine(const std::vector<std::string>& fields) {
  std::string line;
  for (std::size_t index = 0; index < fields.size(); ++index) {
    if (index != 0) { line.push_back('\t'); }
    line.append(fields[index]);
  }
  return line;
}

EngineApiDiagnostic OkDiagnostic() {
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

void ApplySavepointRecordLine(const std::string& line,
                              SavepointParsedState* state) {
  if (state == nullptr) return;
  const auto fields = SplitTabs(line);
  if (fields.size() < 5 || fields[0] != kRowStoreMagic) return;
  const std::string& kind = fields[1];
  const bool update_statement_create =
      kind == kDmlUpdateStatementSavepointCreateKind;
  const bool update_statement_rollback =
      kind == kDmlUpdateStatementSavepointRollbackKind;
  const bool update_statement_release =
      kind == kDmlUpdateStatementSavepointReleaseKind;
  if (update_statement_create || update_statement_rollback ||
      update_statement_release) {
    // UPDATE statement identity/barrier authority is binary MGA durability.
    // A historical host-text record is never admitted as a compatibility
    // authority and forces current transaction reads to fail closed.
    state->update_statement_authority_corrupt = true;
    return;
  }
  const std::uint64_t tx = ParseU64(fields[2]);
  const std::string name = DecodeCrudTextLocal(fields[3]);
  SavepointCutoffs cutoffs;
  cutoffs.row_event_sequence = ParseU64(fields[4]);
  cutoffs.metadata_event_sequence =
      fields.size() >= 6 ? ParseU64(fields[5]) : cutoffs.row_event_sequence;
  cutoffs.index_event_sequence =
      fields.size() >= 7 ? ParseU64(fields[6]) : cutoffs.row_event_sequence;
  if (kind == "SAVEPOINT") {
    state->active_savepoints[tx][name] = cutoffs;
  } else if (kind == "RELEASE_SAVEPOINT") {
    const auto tx_it = state->active_savepoints.find(tx);
    if (tx_it != state->active_savepoints.end()) {
      tx_it->second.erase(name);
    }
  } else if (kind == "ROLLBACK_TO_SAVEPOINT") {
    SavepointRollbackRange range;
    range.cutoffs = cutoffs;
    if (fields.size() >= 10) {
      range.row_upper_event_sequence = ParseU64(fields[7]);
      range.metadata_upper_event_sequence = ParseU64(fields[8]);
      range.index_upper_event_sequence = ParseU64(fields[9]);
    }
    state->rollback_ranges[tx].push_back(range);
  }
}

void NormalizeSavepointRowRollbackRanges(SavepointParsedState* state) {
  if (state == nullptr) return;
  state->normalized_row_rollback_ranges.clear();
  for (const auto& [transaction, source_ranges] : state->rollback_ranges) {
    auto& normalized = state->normalized_row_rollback_ranges[transaction];
    normalized.reserve(source_ranges.size());
    for (const auto& range : source_ranges) {
      if (range.row_upper_event_sequence <=
          range.cutoffs.row_event_sequence) {
        continue;
      }
      normalized.push_back({range.cutoffs.row_event_sequence,
                            range.row_upper_event_sequence});
    }
    std::ranges::sort(normalized);
    std::size_t write = 0;
    for (const auto& range : normalized) {
      if (write != 0 && range.first <= normalized[write - 1].second) {
        normalized[write - 1].second =
            std::max(normalized[write - 1].second, range.second);
      } else {
        normalized[write++] = range;
      }
    }
    normalized.resize(write);
  }
  state->row_rollback_ranges_normalized = true;
}

}  // namespace

SavepointParsedState ParseSavepoints(const EngineRequestContext& context) {
  SavepointParsedState state;
  for (const auto& line : ReadLines(SavepointStorePath(context))) {
    ApplySavepointRecordLine(line, &state);
  }
  std::string ignored_detail;
  if (!ApplyDmlUpdateBinarySavepointRecordsForStoreModule(context, &state,
                                            &ignored_detail)) {
    state.update_statement_authority_corrupt = true;
  }
  if (state.update_statement_authority_corrupt &&
      context.local_transaction_id != 0) {
    SavepointRollbackRange fail_closed;
    fail_closed.cutoffs = {};
    state.rollback_ranges[context.local_transaction_id].push_back(
        fail_closed);
    state.active_savepoints.erase(context.local_transaction_id);
  }
  NormalizeSavepointRowRollbackRanges(&state);
  return state;
}

bool ParseSavepointsBounded(const EngineRequestContext& context,
                            BoundedScopedRowReadControl* control,
                            const std::uint64_t retained_memory_bytes,
                            SavepointParsedState* state) {
  if (control == nullptr || state == nullptr) return false;
  *state = {};
  std::uint64_t path_projection = retained_memory_bytes;
  if (!CheckedHeapReadMemoryAdd(
          static_cast<std::uint64_t>(context.database_path.size()),
          &path_projection) ||
      !CheckedHeapReadMemoryAdd(64, &path_projection) ||
      !ObserveBoundedHeapReadMemory(control, path_projection)) {
    if (control->refusal_detail.empty()) {
      control->refusal_detail =
          "heap_read_savepoint_memory_receipt_overflow";
    }
    return false;
  }
  const std::string path = SavepointStorePath(context);
  std::error_code ignored;
  const auto size_started = std::chrono::steady_clock::now();
  const auto raw_size = std::filesystem::file_size(path, ignored);
  if (!AccountHeapReadWait(control, size_started)) return false;
  if (ignored) {
    std::string durable_detail;
    if (!ApplyDmlUpdateBinarySavepointRecordsForStoreModule(context, state,
                                               &durable_detail) ||
        state->update_statement_authority_corrupt) {
      control->failure_category = MgaHeapReadFailureCategoryV1::kStorage;
      control->refusal_detail = durable_detail.empty()
                                    ? "heap_read_update_savepoint_corrupt"
                                    : durable_detail;
      return false;
    }
    NormalizeSavepointRowRollbackRanges(state);
    return true;
  }
  if (raw_size > std::numeric_limits<std::uint64_t>::max()) {
    control->failure_category = MgaHeapReadFailureCategoryV1::kResource;
    control->refusal_detail = "heap_read_savepoint_size_overflow";
    return false;
  }
  const auto file_bytes = static_cast<std::uint64_t>(raw_size);
  // A valid record can create map/vector nodes and decoded field temporaries.
  // Reserve a conservative logical envelope before opening or parsing it so
  // the operator grant is an admission gate, not a post-allocation sample.
  std::uint64_t parse_projection = 0;
  if (!CheckedHeapReadMemoryMultiply(file_bytes, 128,
                                     &parse_projection) ||
      !CheckedHeapReadMemoryAdd(sizeof(*state) + 4096,
                                &parse_projection) ||
      !CheckedHeapReadMemoryAdd(retained_memory_bytes,
                                &parse_projection) ||
      !ObserveBoundedHeapReadMemory(control, parse_projection)) {
    if (control->refusal_detail.empty()) {
      control->failure_category = MgaHeapReadFailureCategoryV1::kResource;
      control->refusal_detail =
          "heap_read_savepoint_memory_receipt_overflow";
    }
    return false;
  }
  std::ifstream input;
  const auto open_started = std::chrono::steady_clock::now();
  input.open(path, std::ios::binary);
  if (!AccountHeapReadWait(control, open_started)) return false;
  if (!input) {
    control->failure_category = MgaHeapReadFailureCategoryV1::kStorage;
    control->refusal_detail = "heap_read_savepoint_open_failed";
    return false;
  }
  constexpr std::size_t kReadChunkBytes = 64 * 1024;
  char chunk[kReadChunkBytes];
  std::string line;
  std::uint64_t actual_file_bytes = 0;
  bool reached_eof = false;
  while (!reached_eof) {
    if (BoundedScopedReadCancelled(control)) return false;
    const std::uint64_t remaining = file_bytes - actual_file_bytes;
    const std::size_t requested =
        remaining == 0
            ? 1
            : static_cast<std::size_t>(
                  std::min<std::uint64_t>(remaining, kReadChunkBytes));
    const auto read_started = std::chrono::steady_clock::now();
    input.read(chunk, static_cast<std::streamsize>(requested));
    if (!AccountHeapReadWait(control, read_started)) return false;
    const std::streamsize read_count = input.gcount();
    if (read_count < 0 ||
        static_cast<std::uint64_t>(read_count) > remaining) {
      control->failure_category = MgaHeapReadFailureCategoryV1::kStorage;
      control->refusal_detail =
          "heap_read_savepoint_grew_during_read";
      return false;
    }
    actual_file_bytes += static_cast<std::uint64_t>(read_count);
    if (!AccountHeapStorageBytes(
            control, static_cast<std::uint64_t>(read_count))) {
      return false;
    }
    std::size_t begin = 0;
    const std::size_t count = static_cast<std::size_t>(read_count);
    for (std::size_t index = 0; index < count; ++index) {
      if (chunk[index] != '\n') continue;
      line.append(chunk + begin, index - begin);
      ApplySavepointRecordLine(line, state);
      line.clear();
      begin = index + 1;
    }
    if (begin < count) line.append(chunk + begin, count - begin);
    if (input.bad()) {
      control->failure_category = MgaHeapReadFailureCategoryV1::kStorage;
      control->refusal_detail = "heap_read_savepoint_read_failed";
      return false;
    }
    if (read_count < static_cast<std::streamsize>(requested)) {
      if (!input.eof()) {
        control->failure_category = MgaHeapReadFailureCategoryV1::kStorage;
        control->refusal_detail = "heap_read_savepoint_read_failed";
        return false;
      }
      reached_eof = true;
    }
  }
  if (actual_file_bytes != file_bytes) {
    control->failure_category = MgaHeapReadFailureCategoryV1::kStorage;
    control->refusal_detail =
        "heap_read_savepoint_changed_during_read";
    return false;
  }
  if (!line.empty()) ApplySavepointRecordLine(line, state);
  std::string durable_detail;
  if (!ApplyDmlUpdateBinarySavepointRecordsForStoreModule(context, state,
                                             &durable_detail) ||
      state->update_statement_authority_corrupt) {
    control->failure_category = MgaHeapReadFailureCategoryV1::kStorage;
    control->refusal_detail = durable_detail.empty()
                                  ? "heap_read_update_savepoint_corrupt"
                                  : durable_detail;
    return false;
  }
  NormalizeSavepointRowRollbackRanges(state);
  return true;
}

bool RowEventRolledBackBySavepoint(const SavepointParsedState& savepoints,
                                   std::uint64_t creator_tx,
                                   std::uint64_t event_sequence) {
  if (savepoints.row_rollback_ranges_normalized) {
    const auto ranges_it =
        savepoints.normalized_row_rollback_ranges.find(creator_tx);
    if (ranges_it == savepoints.normalized_row_rollback_ranges.end() ||
        ranges_it->second.empty()) {
      return false;
    }
    const auto& ranges = ranges_it->second;
    auto candidate = std::upper_bound(
        ranges.begin(), ranges.end(), event_sequence,
        [](const std::uint64_t value, const auto& range) {
          return value < range.first;
        });
    if (candidate == ranges.begin()) return false;
    --candidate;
    return event_sequence > candidate->first &&
           event_sequence <= candidate->second;
  }
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


std::uint64_t CurrentMgaSavepointAuthorityGeneration(
    const EngineRequestContext& context) {
  if (context.database_path.empty()) {
    return 0;
  }
  std::error_code ignored;
  const auto bytes = std::filesystem::file_size(SavepointStorePath(context),
                                                ignored);
  if (ignored || bytes > std::numeric_limits<std::uint64_t>::max()) {
    return 0;
  }
  return static_cast<std::uint64_t>(bytes);
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

}  // namespace scratchbird::engine::internal_api
