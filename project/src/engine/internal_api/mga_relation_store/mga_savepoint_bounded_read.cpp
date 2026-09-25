// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "mga_savepoint_store.hpp"
#include "mga_savepoint_marker_replay.hpp"
#include "mga_update_durable_store.hpp"
#include "api_diagnostics.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <utility>
namespace scratchbird::engine::internal_api {
// SEARCH_KEY: SB_ENGINE_MGA_SAVEPOINT_BOUNDED_READ_IMPLEMENTATION_AUTHORITY
// Owns resource-accounted marker reads. Replay and transaction finality remain
// with their existing owners; file-change and corruption refusals are retained.
using savepoint_replay::ConsumeMarkerBytes;
using savepoint_replay::NormalizeSavepointRowRollbackRanges;
bool ParseSavepointsBounded(const EngineRequestContext& context,
                            BoundedScopedRowReadControl* control,
                            const std::uint64_t retained_memory_bytes,
                            SavepointParsedState* output) {
  if (output == nullptr) return false;
  *output = {};
  output->diagnostic = MakeInvalidRequestDiagnostic("mga.savepoints", "savepoint_read_incomplete");
  if (control == nullptr) return false;
  SavepointParsedState staged;
  auto* state = &staged;
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
  const std::string path = (context.database_path + ".sb.mga_savepoints");
  std::error_code error;
  const auto size_started = std::chrono::steady_clock::now();
  const auto link_status = std::filesystem::symlink_status(path, error);
  if (!AccountHeapReadWait(control, size_started)) return false;
  if (error == std::errc::no_such_file_or_directory ||
      (!error && link_status.type() == std::filesystem::file_type::not_found)) {
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
    state->diagnostic = MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
    *output = std::move(staged);
    return true;
  }
  if (error) {
    control->failure_category = MgaHeapReadFailureCategoryV1::kStorage;
    control->refusal_detail = "heap_read_savepoint_status_failed";
    return false;
  }
  const auto metadata_started = std::chrono::steady_clock::now();
  const auto status = std::filesystem::status(path, error);
  if (error || !std::filesystem::is_regular_file(status)) {
    control->failure_category = MgaHeapReadFailureCategoryV1::kStorage;
    control->refusal_detail = "heap_read_savepoint_status_failed";
    return false;
  }
  const auto raw_size = std::filesystem::file_size(path, error);
  if (error) {
    control->failure_category = MgaHeapReadFailureCategoryV1::kStorage;
    control->refusal_detail = "heap_read_savepoint_size_failed";
    return false;
  }
  const auto original_time = std::filesystem::last_write_time(path, error);
  if (!AccountHeapReadWait(control, metadata_started)) return false;
  if (error) {
    control->failure_category = MgaHeapReadFailureCategoryV1::kStorage;
    control->refusal_detail = "heap_read_savepoint_time_failed";
    return false;
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
  const auto final_stat_started = std::chrono::steady_clock::now();
  const auto final_status = std::filesystem::status(path, error);
  if (error || !std::filesystem::is_regular_file(final_status)) {
    control->failure_category = MgaHeapReadFailureCategoryV1::kStorage;
    control->refusal_detail = "heap_read_savepoint_final_status_failed";
    return false;
  }
  const auto final_size = std::filesystem::file_size(path, error);
  if (error || final_size != raw_size) {
    control->failure_category = MgaHeapReadFailureCategoryV1::kStorage;
    control->refusal_detail = "heap_read_savepoint_final_size_changed";
    return false;
  }
  const auto final_time = std::filesystem::last_write_time(path, error);
  if (!AccountHeapReadWait(control, final_stat_started)) return false;
  // ConsumeMarkerBytes(final=true) already classifies every residual byte as
  // corrupt marker authority. Keep file-change failures distinct so a torn
  // frame reaches the owning corruption diagnostic below.
  if (error || final_time != original_time) {
    control->failure_category = MgaHeapReadFailureCategoryV1::kStorage;
    control->refusal_detail = "heap_read_savepoint_partial_or_changed_record";
    return false;
  }
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
  state->diagnostic = MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  *output = std::move(staged);
  return true;
}

} // namespace scratchbird::engine::internal_api
