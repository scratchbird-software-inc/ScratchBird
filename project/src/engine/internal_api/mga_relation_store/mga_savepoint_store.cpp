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
#include "mga_relation_store/mga_relation_metadata_store.hpp"
#include "mga_relation_store/mga_savepoint_marker_codec.hpp"

#include "api_diagnostics.hpp"
#include "crud_support/crud_store.hpp"
#include "disk_device.hpp"
#include "uuid.hpp"
#include "core/platform/savepoint_crash_injection.hpp"

#include <algorithm>
#include <array>
#include "mga_savepoint_marker_replay.hpp"
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
using savepoint_replay::ConsumeMarkerBytes;
using savepoint_replay::NormalizeSavepointRowRollbackRanges;
namespace {

// SEARCH_KEY: SB_ENGINE_MGA_SAVEPOINT_STORE_IMPLEMENTATION_AUTHORITY
// Owns durable savepoint markers and their rollback-range projection.
// Savepoints remain transaction-local boundaries and never allocate or publish
// independent transaction finality.

std::string SavepointStorePath(const EngineRequestContext& context) {
  return context.database_path + ".sb.mga_savepoints";
}

bool AppendMarker(const std::string& path, std::uint8_t kind, std::uint64_t transaction,
                  const std::string& identity, std::array<std::uint64_t, 3> cutoffs,
                  std::array<std::uint64_t, 3> upper = {}) {
  MgaSavepointMarkerRecord record;
  record.kind = kind;
  record.transaction = transaction;
  record.uuid_identity = !identity.empty() && identity.front() == '\0';
  if (record.uuid_identity) {
    if (!DecodeMgaSavepointUuidKey(identity, &record.uuid)) return false;
  } else {
    record.identity = identity;
  }
  std::copy(cutoffs.begin(), cutoffs.end(), record.cutoffs);
  std::copy(upper.begin(), upper.end(), record.upper);
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

EngineApiDiagnostic OkDiagnostic() {
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

}  // namespace

static SavepointParsedState ParseSavepointBytesWithObservation(
    const EngineRequestContext& context, std::span<const std::uint8_t> bytes,
    MgaSavepointMarkerObservation* observation) {
  SavepointParsedState state;
  state.observation = observation;
  std::string pending(bytes.begin(), bytes.end());
  ConsumeMarkerBytes(&pending, &state, true);
  std::string detail;
  if (!ApplyDmlUpdateBinarySavepointRecordsForStoreModule(context, &state,
                                            &detail)) {
    state.update_statement_authority_corrupt = true;
  }
  if (state.marker_authority_corrupt || state.update_statement_authority_corrupt) {
    SavepointParsedState failed;
    failed.marker_authority_corrupt = state.marker_authority_corrupt;
    failed.update_statement_authority_corrupt = state.update_statement_authority_corrupt;
    failed.diagnostic = MakeInvalidRequestDiagnostic("mga.savepoints",
        detail.empty() ? "savepoint_authority_invalid" : detail);
    return failed;
  }
  NormalizeSavepointRowRollbackRanges(&state);
  state.diagnostic = OkDiagnostic();
  state.observation = nullptr;
  return state;
}

static SavepointParsedState ParseSavepointsWithObservation(
    const EngineRequestContext& context, MgaSavepointMarkerObservation* observation) {
  std::vector<scratchbird::core::index::byte> bytes;
  if (!ReadCompleteMgaBinaryFile(SavepointStorePath(context), &bytes)) {
    SavepointParsedState failed;
    failed.marker_authority_corrupt = true;
    failed.diagnostic = MakeInvalidRequestDiagnostic(
        "mga.savepoints", "savepoint_store_read_failed");
    return failed;
  }
  return ParseSavepointBytesWithObservation(context, bytes, observation);
}

SavepointParsedState ParseSavepointBytes(
    const EngineRequestContext& context, std::span<const std::uint8_t> bytes) {
  return ParseSavepointBytesWithObservation(context, bytes, nullptr);
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
  scratchbird::core::platform::Uuid uuid;
  if (context.database_path.empty() || !result.transaction ||
      !DecodeMgaSavepointUuidKey(identity, &uuid)) {
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
  auto success = OkDiagnostic();
  auto failure = MakeInvalidRequestDiagnostic("transaction.create_savepoint", "savepoint_append_failed");
  if (!AppendMarker(SavepointStorePath(context), 1, context.local_transaction_id, savepoint_name,
      {row_cutoff, metadata_cutoff, index_cutoff})) return failure;
  return success;
}

EngineApiDiagnostic ReleaseMgaSavepointMarker(const EngineRequestContext& context, const std::string& savepoint_name) {
  const auto exists = ValidateMgaSavepointExists(context, savepoint_name, "transaction.release_savepoint");
  if (exists.error) { return exists; }
  const std::uint64_t row_cutoff = NextRowEventSequence(context) - 1;
  const std::uint64_t metadata_cutoff = NextMetadataEventSequence(context) - 1;
  const std::uint64_t index_cutoff = NextIndexEventSequence(context) - 1;
  auto success = OkDiagnostic();
  auto failure = MakeInvalidRequestDiagnostic("transaction.release_savepoint", "savepoint_release_append_failed");
  if (!AppendMarker(SavepointStorePath(context), 2, context.local_transaction_id, savepoint_name,
      {row_cutoff, metadata_cutoff, index_cutoff})) return failure;
  return success;
}

EngineApiDiagnostic RollbackToMgaSavepointMarker(const EngineRequestContext& context, const std::string& savepoint_name) {
  const auto authority = ValidateMgaSavepointMarkerAuthority(context);
  if (authority.error) return authority;
  const auto savepoints = ParseSavepoints(context);
  if (savepoints.diagnostic.error) return savepoints.diagnostic;
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
  auto success = OkDiagnostic();
  auto failure = MakeInvalidRequestDiagnostic("transaction.rollback_to_savepoint", "savepoint_rollback_append_failed");
  scratchbird::core::platform::MaybeCrashAtMgaSavepointBoundary(
      "rollback_before_marker", context.local_transaction_id);
  if (!AppendMarker(SavepointStorePath(context), 3, context.local_transaction_id, savepoint_name,
      {savepoint_it->second.row_event_sequence, savepoint_it->second.metadata_event_sequence,
       savepoint_it->second.index_event_sequence}, {row_upper, metadata_upper, index_upper})) return failure;
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
  if (savepoints.diagnostic.error) return savepoints.diagnostic;
  const auto tx_it = savepoints.active_savepoints.find(context.local_transaction_id);
  if (tx_it == savepoints.active_savepoints.end() ||
      tx_it->second.find(savepoint_name) == tx_it->second.end()) {
    return MakeInvalidRequestDiagnostic(operation_id, "savepoint_not_found");
  }
  return OkDiagnostic();
}


MgaSavepointNamesResult ActiveMgaSavepointNames(const EngineRequestContext& context) {
  MgaSavepointNamesResult result;
  result.diagnostic = OkDiagnostic();
  if (context.local_transaction_id == 0 || context.database_path.empty()) {
    return result;
  }
  const auto savepoints = ParseSavepoints(context);
  if (savepoints.diagnostic.error) {
    result.diagnostic = savepoints.diagnostic;
    return result;
  }
  const auto tx_it = savepoints.active_savepoints.find(context.local_transaction_id);
  if (tx_it == savepoints.active_savepoints.end()) {
    return result;
  }
  result.names.reserve(tx_it->second.size());
  for (const auto& entry : tx_it->second) {
    result.names.push_back(entry.first);
  }
  return result;
}

}  // namespace scratchbird::engine::internal_api
