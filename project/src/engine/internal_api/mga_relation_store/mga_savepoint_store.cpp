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
#include "mga_relation_store/mga_savepoint_marker_codec.hpp"

#include "api_diagnostics.hpp"
#include "crud_support/crud_store.hpp"
#include "disk_device.hpp"
#include "uuid.hpp"
#include "core/platform/savepoint_crash_injection.hpp"

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
#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

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

bool ParseU64(const std::string& text, std::uint64_t* value) {
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), *value);
  return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
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
  // The legacy field projection is internal only. All new persisted records
  // are checksummed binary frames; a native identity occupies exactly 16 bytes.
  const auto fields = SplitTabs(line);
  if (fields.size() != 7 && fields.size() != 10) return false;
  MgaSavepointMarkerRecord record;
  record.kind = fields[1] == "SAVEPOINT" ? 1 : fields[1] == "RELEASE_SAVEPOINT" ? 2 : 3;
  record.identity = DecodeCrudTextLocal(fields[3]);
  record.uuid_identity = !record.identity.empty() && record.identity.front() == '\0';
  if (record.uuid_identity) record.identity.erase(0, 1);
  if (!ParseU64(fields[2], &record.transaction)) return false;
  for (unsigned i = 0; i < 3; ++i) {
    if (!ParseU64(fields[4 + i], &record.cutoffs[i])) return false;
    if (fields.size() == 10 && !ParseU64(fields[7 + i], &record.upper[i])) return false;
  }
  const auto bytes = EncodeMgaSavepointMarker(record);
  if (bytes.empty()) return false;
  // Prepare names/handles before the first write. Marker publication must
  // not be followed by diagnostic or filesystem-path allocations that can
  // throw and disguise an already durable release as an ordinary failure.
#if defined(_WIN32)
  HANDLE file = CreateFileA(path.c_str(), FILE_APPEND_DATA,
      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
      FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  const bool durable = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()),
      &written, nullptr) && written == bytes.size() && FlushFileBuffers(file);
  const bool closed = CloseHandle(file) != 0;
  return durable && closed;
#else
  const auto parent = std::filesystem::path(path).parent_path();
  const auto parent_name = parent.empty() ? std::string(".") : parent.string();
  const int directory = ::open(parent_name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (directory < 0) return false;
  const int file = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (file < 0) { ::close(directory); return false; }
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto written = ::write(file, bytes.data() + offset, bytes.size() - offset);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) break;
    offset += static_cast<std::size_t>(written);
  }
  const bool durable = offset == bytes.size() && ::fsync(file) == 0 && ::fsync(directory) == 0;
  const bool closed = ::close(file) == 0;
  ::close(directory);
  return durable && closed;
#endif
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
  if (fields.size() < 5 || fields[0] != kRowStoreMagic) {
    state->marker_authority_corrupt = true;
    return;
  }
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
  const bool create = kind == "SAVEPOINT";
  const bool release = kind == "RELEASE_SAVEPOINT";
  const bool rollback = kind == "ROLLBACK_TO_SAVEPOINT";
  if ((!create && !release && !rollback) ||
      !((fields.size() >= 5 && fields.size() <= 7) ||
        (rollback && fields.size() == 10))) {
    state->marker_authority_corrupt = true;
    return;
  }
  std::uint64_t tx = 0;
  const std::string name = DecodeCrudTextLocal(fields[3]);
  SavepointCutoffs cutoffs;
  if (!ParseU64(fields[2], &tx) || tx == 0 || name.empty() ||
      !ParseU64(fields[4], &cutoffs.row_event_sequence) ||
      (fields.size() >= 6 &&
       !ParseU64(fields[5], &cutoffs.metadata_event_sequence)) ||
      (fields.size() >= 7 &&
       !ParseU64(fields[6], &cutoffs.index_event_sequence))) {
    state->marker_authority_corrupt = true;
    return;
  }
  if (fields.size() < 6) cutoffs.metadata_event_sequence = cutoffs.row_event_sequence;
  if (fields.size() < 7) cutoffs.index_event_sequence = cutoffs.row_event_sequence;
  if (create) {
    if (state->next_creation_ordinal == UINT64_MAX) {
      state->marker_authority_corrupt = true;
      return;
    }
    cutoffs.creation_ordinal = state->next_creation_ordinal++;
    if (auto* observed = state->observation;
        observed && observed->transaction == tx && observed->identity == name) {
      if (observed->require_unique_identity) {
        if (observed->creation_ordinal) {
          state->marker_authority_corrupt = true;
          return;
        }
        observed->creation_ordinal = cutoffs.creation_ordinal;
      }
      if (cutoffs.creation_ordinal == observed->creation_ordinal) {
        observed->lifecycle = MgaSavepointMarkerLifecycle::active;
        observed->cutoffs = cutoffs;
      } else if (observed->lifecycle == MgaSavepointMarkerLifecycle::active) {
        observed->lifecycle = MgaSavepointMarkerLifecycle::invalidated;
      }
    }
    state->active_savepoints[tx][name] = cutoffs;
  } else if (release) {
    const auto tx_it = state->active_savepoints.find(tx);
    if (auto* observed = state->observation;
        observed && observed->transaction == tx && observed->identity == name &&
        observed->lifecycle == MgaSavepointMarkerLifecycle::active &&
        tx_it != state->active_savepoints.end() && tx_it->second.contains(name) &&
        tx_it->second.at(name).creation_ordinal == observed->creation_ordinal) {
      observed->lifecycle = MgaSavepointMarkerLifecycle::released;
    }
    if (tx_it == state->active_savepoints.end() ||
        tx_it->second.erase(name) == 0) {
      state->marker_authority_corrupt = true;
    }
  } else if (rollback) {
    SavepointRollbackRange range;
    range.cutoffs = cutoffs;
    if (fields.size() == 10 &&
        (!ParseU64(fields[7], &range.row_upper_event_sequence) ||
         !ParseU64(fields[8], &range.metadata_upper_event_sequence) ||
         !ParseU64(fields[9], &range.index_upper_event_sequence) ||
         range.row_upper_event_sequence < cutoffs.row_event_sequence ||
         range.metadata_upper_event_sequence < cutoffs.metadata_event_sequence ||
         range.index_upper_event_sequence < cutoffs.index_event_sequence)) {
      state->marker_authority_corrupt = true;
      return;
    }
    auto tx_it = state->active_savepoints.find(tx);
    if (tx_it == state->active_savepoints.end() ||
        !tx_it->second.contains(name)) {
      state->marker_authority_corrupt = true;
      return;
    }
    const auto target = tx_it->second.at(name);
    if (target.row_event_sequence != cutoffs.row_event_sequence ||
        target.metadata_event_sequence != cutoffs.metadata_event_sequence ||
        target.index_event_sequence != cutoffs.index_event_sequence) {
      state->marker_authority_corrupt = true;
      return;
    }
    if (auto* observed = state->observation;
        observed && observed->transaction == tx &&
        observed->lifecycle == MgaSavepointMarkerLifecycle::active) {
      if (observed->creation_ordinal > target.creation_ordinal)
        observed->lifecycle = MgaSavepointMarkerLifecycle::invalidated;
      else if (observed->creation_ordinal == target.creation_ordinal)
        observed->rolled_back = true;
    }
    std::erase_if(tx_it->second, [&](const auto& entry) {
      return entry.second.creation_ordinal > target.creation_ordinal;
    });
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

void ConsumeMarkerBytes(std::string* pending, SavepointParsedState* state,
                        bool final) {
  std::size_t consumed = 0;
  while (consumed < pending->size() && !state->marker_authority_corrupt) {
    const std::string_view remaining(pending->data() + consumed, pending->size() - consumed);
    if (static_cast<unsigned char>(remaining.front()) == kMgaSavepointFrameLead) {
      const auto size = MgaSavepointMarkerFrameSize(remaining);
      if (size == UINT32_MAX) { state->marker_authority_corrupt = true; break; }
      if (size == 0 || remaining.size() < size) break;
      MgaSavepointMarkerRecord record;
      if (!DecodeMgaSavepointMarker(remaining.substr(0, size), &record)) {
        state->marker_authority_corrupt = true;
        break;
      }
      // Reuse the range/stack validator through an in-memory field projection.
      // This projection is never written to disk or sent over a wire protocol.
      std::vector<std::string> fields{
          kRowStoreMagic, record.kind == 1 ? "SAVEPOINT" :
              record.kind == 2 ? "RELEASE_SAVEPOINT" : "ROLLBACK_TO_SAVEPOINT",
          std::to_string(record.transaction), EncodeCrudText(record.uuid_identity
              ? MgaSavepointUuidKey(record.identity) : record.identity)};
      for (auto value : record.cutoffs) fields.push_back(std::to_string(value));
      if (record.kind == 3)
        for (auto value : record.upper) fields.push_back(std::to_string(value));
      ApplySavepointRecordLine(JoinLine(fields), state);
      consumed += size;
    } else {
      const auto end = remaining.find('\n');
      if (end == std::string_view::npos) break;
      ApplySavepointRecordLine(std::string(remaining.substr(0, end)), state);
      consumed += end + 1;
    }
  }
  pending->erase(0, consumed);
  if ((final && !pending->empty()) || pending->size() > kMgaSavepointFrameMaximum)
    state->marker_authority_corrupt = true;
}

}  // namespace

static SavepointParsedState ParseSavepointsWithObservation(
    const EngineRequestContext& context, MgaSavepointMarkerObservation* observation) {
  SavepointParsedState state;
  state.observation = observation;
  const auto path = SavepointStorePath(context);
  std::error_code error;
  const bool exists = std::filesystem::exists(path, error);
  if (error) state.marker_authority_corrupt = true;
  if (exists) {
    std::ifstream input(path, std::ios::binary);
    if (!input) state.marker_authority_corrupt = true;
    std::string pending;
    char chunk[64 * 1024];
    while (input && !state.marker_authority_corrupt) {
      input.read(chunk, sizeof(chunk));
      pending.append(chunk, static_cast<std::size_t>(input.gcount()));
      ConsumeMarkerBytes(&pending, &state, false);
    }
    ConsumeMarkerBytes(&pending, &state, true);
    if (input.bad()) state.marker_authority_corrupt = true;
  }
  std::string ignored_detail;
  if (!ApplyDmlUpdateBinarySavepointRecordsForStoreModule(context, &state,
                                            &ignored_detail)) {
    state.update_statement_authority_corrupt = true;
  }
  if ((state.update_statement_authority_corrupt || state.marker_authority_corrupt) &&
      context.local_transaction_id != 0) {
    SavepointRollbackRange fail_closed;
    fail_closed.cutoffs = {};
    state.rollback_ranges[context.local_transaction_id].push_back(
        fail_closed);
    state.active_savepoints.erase(context.local_transaction_id);
  }
  NormalizeSavepointRowRollbackRanges(&state);
  state.observation = nullptr;
  return state;
}

SavepointParsedState ParseSavepoints(const EngineRequestContext& context) {
  return ParseSavepointsWithObservation(context, nullptr);
}

MgaSavepointMarkerObservation ObserveMgaSavepointMarker(
    const EngineRequestContext& context, const std::string& identity,
    std::uint64_t creation_ordinal) {
  MgaSavepointMarkerObservation result;
  result.transaction = context.local_transaction_id;
  result.identity = identity;
  result.creation_ordinal = creation_ordinal;
  if (context.database_path.empty() || !result.transaction || identity.empty() || !creation_ordinal) {
    result.diagnostic = MakeInvalidRequestDiagnostic("mga.savepoint.observe", "exact_marker_identity_required");
    return result;
  }
  const auto state = ParseSavepointsWithObservation(context, &result);
  if (state.marker_authority_corrupt || state.update_statement_authority_corrupt) {
    result.lifecycle = MgaSavepointMarkerLifecycle::missing;
    result.diagnostic = MakeEngineApiDiagnostic("MGA.SAVEPOINT.AUTHORITY_CORRUPT",
        "mga.savepoint.authority_corrupt", "savepoint_marker_stream_invalid");
    return result;
  }
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  return result;
}

MgaSavepointMarkerObservation ObserveUniqueMgaSavepointMarkerV1(
    const EngineRequestContext& context, const std::string& identity) {
  MgaSavepointMarkerObservation result;
  result.transaction = context.local_transaction_id; result.identity = identity;
  result.require_unique_identity = true;
  const auto uuid = scratchbird::core::uuid::ParseUuid(
      identity.size() == 37 && identity.front() == '\0'
          ? identity.substr(1) : std::string{});
  if (context.database_path.empty() || !result.transaction || !uuid.ok() ||
      scratchbird::core::uuid::IsNilUuid(uuid.value) ||
      identity.substr(1) != scratchbird::core::uuid::UuidToString(uuid.value)) {
    result.diagnostic = MakeInvalidRequestDiagnostic("mga.savepoint.observe", "reserved_marker_identity_required");
    return result;
  }
  const auto state = ParseSavepointsWithObservation(context, &result);
  if (state.marker_authority_corrupt || state.update_statement_authority_corrupt) {
    result.lifecycle = MgaSavepointMarkerLifecycle::missing;
    result.diagnostic = MakeEngineApiDiagnostic("MGA.SAVEPOINT.AUTHORITY_CORRUPT",
        "mga.savepoint.authority_corrupt", "reserved_marker_history_invalid");
    return result;
  }
  result.ok = true; result.diagnostic = OkDiagnostic(); return result;
}

EngineApiDiagnostic ValidateMgaSavepointMarkerAuthority(
    const EngineRequestContext& context) {
  if (context.database_path.empty()) {
    return MakeInvalidRequestDiagnostic("mga.savepoint", "database_path_required");
  }
  const auto state = ParseSavepoints(context);
  if (state.marker_authority_corrupt || state.update_statement_authority_corrupt) {
    return MakeEngineApiDiagnostic("MGA.SAVEPOINT.AUTHORITY_CORRUPT",
        "mga.savepoint.authority_corrupt", "savepoint_marker_stream_invalid");
  }
  return OkDiagnostic();
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
    if (ignored != std::errc::no_such_file_or_directory) {
      control->failure_category = MgaHeapReadFailureCategoryV1::kStorage;
      control->refusal_detail = "heap_read_savepoint_stat_failed";
      return false;
    }
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
    const std::size_t count = static_cast<std::size_t>(read_count);
    line.append(chunk, count);
    ConsumeMarkerBytes(&line, state, false);
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
  ConsumeMarkerBytes(&line, state, true);
  std::string durable_detail;
  if (!ApplyDmlUpdateBinarySavepointRecordsForStoreModule(context, state,
                                             &durable_detail) ||
      state->update_statement_authority_corrupt || state->marker_authority_corrupt) {
    control->failure_category = MgaHeapReadFailureCategoryV1::kStorage;
    control->refusal_detail = durable_detail.empty()
                                  ? (state->marker_authority_corrupt
                                         ? "heap_read_savepoint_authority_corrupt"
                                         : "heap_read_update_savepoint_corrupt")
                                  : durable_detail;
    return false;
  }
  NormalizeSavepointRowRollbackRanges(state);
  return true;
}

bool RowEventRolledBackBySavepoint(const SavepointParsedState& savepoints,
                                   std::uint64_t creator_tx,
                                   std::uint64_t event_sequence) {
  if (savepoints.marker_authority_corrupt) return true;
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
  if (savepoints.marker_authority_corrupt) return true;
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
  if (savepoints.marker_authority_corrupt) return true;
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
  if (savepoint_name.empty()) {
    return MakeInvalidRequestDiagnostic("transaction.create_savepoint", "savepoint_name_required");
  }
  const auto authority = ValidateMgaSavepointMarkerAuthority(context);
  if (authority.error) return authority;
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
  auto success = OkDiagnostic();
  auto failure = MakeInvalidRequestDiagnostic("transaction.create_savepoint", "savepoint_append_failed");
  if (!AppendLine(SavepointStorePath(context), line)) return failure;
  return success;
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
  auto success = OkDiagnostic();
  auto failure = MakeInvalidRequestDiagnostic("transaction.release_savepoint", "savepoint_release_append_failed");
  if (!AppendLine(SavepointStorePath(context), line)) return failure;
  return success;
}

EngineApiDiagnostic RollbackToMgaSavepointMarker(const EngineRequestContext& context, const std::string& savepoint_name) {
  const auto authority = ValidateMgaSavepointMarkerAuthority(context);
  if (authority.error) return authority;
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
  auto success = OkDiagnostic();
  auto failure = MakeInvalidRequestDiagnostic("transaction.rollback_to_savepoint", "savepoint_rollback_append_failed");
  scratchbird::core::platform::MaybeCrashAtMgaSavepointBoundary(
      "rollback_before_marker", context.local_transaction_id);
  if (!AppendLine(SavepointStorePath(context), line)) return failure;
  scratchbird::core::platform::MaybeCrashAtMgaSavepointBoundary(
      "rollback_after_marker", context.local_transaction_id);
  return success;
}

EngineApiDiagnostic ValidateMgaSavepointExists(const EngineRequestContext& context,
                                               const std::string& savepoint_name,
                                               const std::string& operation_id) {
  if (context.local_transaction_id == 0) {
    return MakeInvalidRequestDiagnostic(operation_id, "local_transaction_id_required");
  }
  const auto authority = ValidateMgaSavepointMarkerAuthority(context);
  if (authority.error) return authority;
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
