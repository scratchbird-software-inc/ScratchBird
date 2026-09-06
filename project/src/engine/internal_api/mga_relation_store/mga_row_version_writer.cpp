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
#include "mga_relation_store/mga_row_codec.hpp"
#include "mga_relation_store/mga_row_version_reader.hpp"

#include "api_diagnostics.hpp"
#include "crud_support/crud_store.hpp"
#include "whole_store_crash_injection.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api {
namespace {

// SEARCH_KEY: SB_ENGINE_MGA_ROW_VERSION_WRITER_IMPLEMENTATION_AUTHORITY
// Owns row-version append materialization and the shared hot-append flush
// coordinator. It creates mutation evidence but cannot publish transaction
// finality, which remains durable MGA transaction-inventory authority.

constexpr const char* kRowStoreMagic = "SBMGA1";
constexpr std::string_view kLineHexFieldPrefix = "SBHEX:";

std::string RowStorePath(const EngineRequestContext& context) {
  return context.database_path + ".sb.mga_row_versions";
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

bool FileExistsAndNotEmpty(const std::string& path) {
  std::error_code ignored;
  return std::filesystem::exists(path, ignored) &&
         std::filesystem::file_size(path, ignored) != 0;
}

bool ScopedRelationAnyRowStoreExists(const EngineRequestContext& context,
                                     const std::string& table_uuid) {
  return FileExistsAndNotEmpty(ScopedRowStorePath(context, table_uuid)) ||
         FileExistsAndNotEmpty(ScopedRowBinaryStorePath(context, table_uuid));
}

EngineApiDiagnostic OkDiagnostic() {
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

bool AppendBuffer(const std::string& path,
                  const std::string& buffer,
                  std::uint64_t* stream_opens,
                  std::uint64_t* stream_flushes) {
  if (buffer.empty()) { return true; }
  if (path.empty()) { return false; }
  std::ofstream out(path, std::ios::app | std::ios::binary);
  if (!out) { return false; }
  if (stream_opens != nullptr) { ++(*stream_opens); }
  out.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
  out.flush();
  if (stream_flushes != nullptr) { ++(*stream_flushes); }
  return static_cast<bool>(out);
}

bool AppendScopedRelationBuffer(const std::string& path,
                                const std::string& buffer,
                                std::uint64_t* stream_opens,
                                std::uint64_t* stream_flushes) {
  if (path.empty()) { return false; }
  if (buffer.empty()) { return true; }
  std::error_code ignored;
  std::filesystem::create_directories(std::filesystem::path(path).parent_path(),
                                      ignored);
  return AppendBuffer(path, buffer, stream_opens, stream_flushes);
}

struct ScopedRelationWriteTicket {
  std::string path;
  bool ok = false;
  std::uint64_t stream_opens = 0;
  std::uint64_t stream_flushes = 0;
};

struct ScopedRelationAppendResult {
  bool ok = false;
  std::uint64_t stream_opens = 0;
  std::uint64_t stream_flushes = 0;
  std::uint64_t write_batches = 0;
  std::uint64_t write_tickets_issued = 0;
  std::uint64_t write_tickets_completed = 0;
  std::uint64_t write_worker_count = 0;
};

ScopedRelationWriteTicket AppendScopedRelationBufferTicket(
    const std::string& path,
    const std::string& buffer) {
  ScopedRelationWriteTicket ticket;
  ticket.path = path;
  ticket.ok = AppendScopedRelationBuffer(ticket.path,
                                         buffer,
                                         &ticket.stream_opens,
                                         &ticket.stream_flushes);
  return ticket;
}

bool AppendScopedRelationLinesParallel(
    const std::map<std::string, std::string>& pending,
    std::uint64_t* stream_opens,
    std::uint64_t* stream_flushes,
    std::uint64_t* write_batches,
    std::uint64_t* write_tickets_issued,
    std::uint64_t* write_tickets_completed,
    std::uint64_t* write_worker_count) {
  if (pending.empty()) {
    return true;
  }
  if (write_batches != nullptr) { ++(*write_batches); }
  if (write_tickets_issued != nullptr) {
    *write_tickets_issued += static_cast<std::uint64_t>(pending.size());
  }
  if (write_worker_count != nullptr) {
    *write_worker_count = std::max(*write_worker_count,
                                   static_cast<std::uint64_t>(pending.size()));
  }
  if (pending.size() == 1) {
    const auto& [path, buffer] = *pending.begin();
    const auto ticket = AppendScopedRelationBufferTicket(path, buffer);
    if (stream_opens != nullptr) { *stream_opens += ticket.stream_opens; }
    if (stream_flushes != nullptr) { *stream_flushes += ticket.stream_flushes; }
    if (write_tickets_completed != nullptr) { ++(*write_tickets_completed); }
    return ticket.ok;
  }
  std::vector<std::future<ScopedRelationWriteTicket>> futures;
  futures.reserve(pending.size());
  for (const auto& [path, buffer] : pending) {
    const auto* path_ptr = &path;
    const auto* buffer_ptr = &buffer;
    futures.push_back(std::async(std::launch::async,
                                 [path_ptr, buffer_ptr]() {
                                   return AppendScopedRelationBufferTicket(
                                       *path_ptr,
                                       *buffer_ptr);
                                 }));
  }
  bool ok = true;
  for (auto& future : futures) {
    const auto ticket = future.get();
    if (stream_opens != nullptr) { *stream_opens += ticket.stream_opens; }
    if (stream_flushes != nullptr) { *stream_flushes += ticket.stream_flushes; }
    if (write_tickets_completed != nullptr) { ++(*write_tickets_completed); }
    ok = ok && ticket.ok;
  }
  return ok;
}

ScopedRelationAppendResult AppendScopedRelationLinesWithCounters(
    const std::map<std::string, std::string>& pending) {
  ScopedRelationAppendResult result;
  result.ok = AppendScopedRelationLinesParallel(
      pending,
      &result.stream_opens,
      &result.stream_flushes,
      &result.write_batches,
      &result.write_tickets_issued,
      &result.write_tickets_completed,
      &result.write_worker_count);
  return result;
}

void AddScopedRelationAppendCounters(
    const ScopedRelationAppendResult& result,
    std::uint64_t* stream_opens,
    std::uint64_t* stream_flushes,
    std::uint64_t* write_batches,
    std::uint64_t* write_tickets_issued,
    std::uint64_t* write_tickets_completed,
    std::uint64_t* write_worker_count) {
  if (stream_opens != nullptr) { *stream_opens += result.stream_opens; }
  if (stream_flushes != nullptr) { *stream_flushes += result.stream_flushes; }
  if (write_batches != nullptr) { *write_batches += result.write_batches; }
  if (write_tickets_issued != nullptr) {
    *write_tickets_issued += result.write_tickets_issued;
  }
  if (write_tickets_completed != nullptr) {
    *write_tickets_completed += result.write_tickets_completed;
  }
  if (write_worker_count != nullptr) {
    *write_worker_count = std::max(*write_worker_count,
                                   result.write_worker_count);
  }
}

bool AppendScopedExactIndexBinaryBatch(
    std::string* out,
    const MgaExactIndexEntryAppendBatch& batch,
    std::uint64_t creator_tx,
    std::uint64_t first_event_sequence) {
  if (out == nullptr || batch.entries.empty()) {
    return false;
  }
  const std::string table_uuid =
      batch.index.table_uuid.empty() ? batch.table_uuid : batch.index.table_uuid;
  if (table_uuid.empty() || batch.index.index_uuid.empty()) {
    return false;
  }
  std::size_t estimate = kScopedIndexBinaryBatchMagic.size() + 2 + 2 + 8 + 8 +
                         8 + 24 + table_uuid.size() +
                         batch.index.index_uuid.size() +
                         batch.index.column_name.size() +
                         batch.index.family.size();
  for (const auto& entry : batch.entries) {
    estimate += 16 + entry.encoded_key.size() + entry.payload_value.size() +
                entry.row_uuid.size() + entry.version_uuid.size();
  }
  ReserveAmortizedAppendCapacity(out, estimate);
  out->append(kScopedIndexBinaryBatchMagic.data(),
              kScopedIndexBinaryBatchMagic.size());
  AppendBinaryU16(out, kScopedIndexBinaryVersion);
  AppendBinaryU16(out, 0);
  AppendBinaryU64(out, static_cast<std::uint64_t>(batch.entries.size()));
  AppendBinaryU64(out, creator_tx);
  AppendBinaryU64(out, first_event_sequence);
  if (!AppendBinaryString(out, table_uuid) ||
      !AppendBinaryString(out, batch.index.index_uuid) ||
      !AppendBinaryString(out, batch.index.column_name) ||
      !AppendBinaryString(out, batch.index.family) ||
      !AppendBinaryString(out, batch.entry_kind.empty() ? "exact"
                                                        : batch.entry_kind)) {
    return false;
  }
  for (const auto& entry : batch.entries) {
    if (entry.encoded_key.empty() || entry.row_uuid.empty() ||
        entry.version_uuid.empty()) {
      return false;
    }
    if (!AppendBinaryString(out, entry.encoded_key) ||
        !AppendBinaryString(out, entry.payload_value) ||
        !AppendBinaryString(out, entry.row_uuid) ||
        !AppendBinaryString(out, entry.version_uuid)) {
      return false;
    }
  }
  return true;
}

void AppendIndexEntryStoreLine(std::string* out,
                               std::uint64_t creator_tx,
                               std::uint64_t event_sequence,
                               std::string_view index_uuid,
                               std::string_view table_uuid,
                               std::string_view column_name,
                               std::string_view family,
                               std::string_view entry_kind,
                               std::string_view key,
                               std::string_view payload,
                               std::string_view row_uuid,
                               std::string_view version_uuid) {
  if (out == nullptr) { return; }
  ReserveAmortizedAppendCapacity(out,
                                 128 + index_uuid.size() + table_uuid.size() +
                                     column_name.size() + family.size() +
                                     entry_kind.size() +
                                     kLineHexFieldPrefix.size() + key.size() * 2 +
                                     kLineHexFieldPrefix.size() + payload.size() * 2 +
                                     row_uuid.size() +
                                     version_uuid.size());
  bool first = true;
  AppendLineField(out, &first, kRowStoreMagic);
  AppendLineField(out, &first, "INDEX_ENTRY");
  AppendLineU64Field(out, &first, creator_tx);
  AppendLineU64Field(out, &first, event_sequence);
  AppendLineField(out, &first, index_uuid);
  AppendLineField(out, &first, table_uuid);
  AppendLineField(out, &first, column_name);
  AppendLineField(out, &first, family);
  AppendLineField(out, &first, entry_kind);
  AppendLineSafeOrHexField(out, &first, key);
  AppendLineSafeOrHexField(out, &first, payload);
  AppendLineField(out, &first, row_uuid);
  AppendLineField(out, &first, version_uuid);
  out->push_back('\n');
}

struct PreparedIndexEntryLine {
  std::string table_uuid;
  std::string index_uuid;
  std::string column_name;
  std::string family;
  std::string entry_kind;
  std::string key;
  std::string payload;
  std::string row_uuid;
  std::string version_uuid;
};

struct PreparedIndexAppendJob {
  bool ok = true;
  EngineApiDiagnostic diagnostic = OkDiagnostic();
  std::vector<PreparedIndexEntryLine> entries;
  std::uint64_t sorted_batch_count = 0;
};

struct PreparedIndexLineBufferJob {
  bool ok = true;
  std::map<std::string, std::string> scoped_lines;
};

constexpr std::size_t kHotAppendRowLineReserveBytes = 384;
constexpr std::size_t kHotAppendIndexLineReserveBytes = 384;

bool PreparedIndexEntryLineLess(const PreparedIndexEntryLine& left,
                                const PreparedIndexEntryLine& right) {
  return std::tie(left.table_uuid,
                  left.index_uuid,
                  left.key,
                  left.row_uuid,
                  left.version_uuid) <
         std::tie(right.table_uuid,
                  right.index_uuid,
                  right.key,
                  right.row_uuid,
                  right.version_uuid);
}

void SortPreparedIndexEntryRangeIfNeeded(
    std::vector<PreparedIndexEntryLine>* entries,
    std::size_t first,
    std::uint64_t* sorted_batch_count) {
  if (entries == nullptr || sorted_batch_count == nullptr ||
      entries->size() <= first + 1) {
    return;
  }
  auto begin = entries->begin() + static_cast<std::ptrdiff_t>(first);
  auto end = entries->end();
  if (std::is_sorted(begin, end, PreparedIndexEntryLineLess)) {
    return;
  }
  ++(*sorted_batch_count);
  std::stable_sort(begin, end, PreparedIndexEntryLineLess);
}

bool BulkSortIndexMaterialAllowed(const CrudIndexRecord& index) {
  const std::string family =
      index.family.empty() ? CrudIndexFamilyForProfile(index.profile) : index.family;
  return family == kCrudIndexFamilyBtree ||
         family == "unique_btree" ||
         index.unique ||
         family == kCrudIndexFamilyExpression ||
         family == kCrudIndexFamilyPartial ||
         family == kCrudIndexFamilyCovering;
}

bool ExactIndexEntryInputLess(const MgaExactIndexEntryAppendBatch& batch,
                              const MgaExactIndexEntryInput& left,
                              const MgaExactIndexEntryInput& right) {
  const std::string table_uuid =
      batch.index.table_uuid.empty() ? batch.table_uuid : batch.index.table_uuid;
  return std::tie(table_uuid,
                  batch.index.index_uuid,
                  left.encoded_key,
                  left.row_uuid,
                  left.version_uuid) <
         std::tie(table_uuid,
                  batch.index.index_uuid,
                  right.encoded_key,
                  right.row_uuid,
                  right.version_uuid);
}

bool ExactIndexBatchAlreadyInAppendOrder(
    const MgaExactIndexEntryAppendBatch& batch) {
  if (batch.entries.size() <= 1 || !BulkSortIndexMaterialAllowed(batch.index)) {
    return true;
  }
  return std::is_sorted(batch.entries.begin(),
                        batch.entries.end(),
                        [&](const auto& left, const auto& right) {
                          return ExactIndexEntryInputLess(batch, left, right);
                        });
}

void AddPreparedIndexAppendBatch(const MgaIndexEntryAppendBatch& batch,
                                 PreparedIndexAppendJob* job) {
  if (job == nullptr || batch.rows.empty()) { return; }
  const std::string table_uuid =
      batch.index.table_uuid.empty() ? batch.table_uuid : batch.index.table_uuid;
  const bool sort_allowed = BulkSortIndexMaterialAllowed(batch.index);
  const std::size_t before_batch = job->entries.size();
  job->entries.reserve(before_batch + batch.rows.size());
  for (const auto& row : batch.rows) {
    const auto keys = CrudIndexKeysForValues(batch.index, row.values);
    if (keys.empty()) { continue; }
    if (keys.size() > 1) {
      job->entries.reserve(job->entries.size() + keys.size());
    }
    const std::string payload =
        CrudFieldValue(row.values, batch.index.column_name);
    for (const auto& key : keys) {
      job->entries.push_back({table_uuid,
                              batch.index.index_uuid,
                              batch.index.column_name,
                              batch.index.family,
                              batch.entry_kind.empty() ? "exact"
                                                       : batch.entry_kind,
                              key,
                              payload,
                              row.row_uuid,
                              row.version_uuid});
    }
  }
  if (sort_allowed && job->entries.size() > before_batch + 1) {
    SortPreparedIndexEntryRangeIfNeeded(&job->entries,
                                        before_batch,
                                        &job->sorted_batch_count);
  }
}

PreparedIndexAppendJob BuildPreparedIndexAppendJob(
    const std::vector<MgaIndexEntryAppendBatch>& batches) {
  PreparedIndexAppendJob job;
  try {
    for (const auto& batch : batches) {
      AddPreparedIndexAppendBatch(batch, &job);
    }
  } catch (const std::exception& ex) {
    job.ok = false;
    job.diagnostic = MakeInvalidRequestDiagnostic("mga.index_store",
                                                  std::string("index_materialization_failed:") +
                                                      ex.what());
  } catch (...) {
    job.ok = false;
    job.diagnostic = MakeInvalidRequestDiagnostic("mga.index_store",
                                                  "index_materialization_failed");
  }
  return job;
}

PreparedIndexAppendJob BuildPreparedIndexAppendJob(
    const MgaIndexEntryAppendBatch& batch) {
  PreparedIndexAppendJob job;
  try {
    AddPreparedIndexAppendBatch(batch, &job);
  } catch (const std::exception& ex) {
    job.ok = false;
    job.diagnostic = MakeInvalidRequestDiagnostic(
        "mga.index_store",
        std::string("index_materialization_failed:") + ex.what());
  } catch (...) {
    job.ok = false;
    job.diagnostic = MakeInvalidRequestDiagnostic(
        "mga.index_store",
        "index_materialization_failed");
  }
  return job;
}

void AddPreparedExactIndexAppendBatch(const MgaExactIndexEntryAppendBatch& batch,
                                      PreparedIndexAppendJob* job) {
  if (job == nullptr || batch.entries.empty()) { return; }
  const std::string table_uuid =
      batch.index.table_uuid.empty() ? batch.table_uuid : batch.index.table_uuid;
  const bool sort_allowed = BulkSortIndexMaterialAllowed(batch.index);
  const std::size_t before_batch = job->entries.size();
  job->entries.reserve(before_batch + batch.entries.size());
  for (const auto& entry : batch.entries) {
    if (entry.encoded_key.empty() || entry.row_uuid.empty() ||
        entry.version_uuid.empty()) {
      job->ok = false;
      job->diagnostic = MakeInvalidRequestDiagnostic("mga.index_store",
                                                     "exact_index_entry_invalid");
      return;
    }
    job->entries.push_back({table_uuid,
                            batch.index.index_uuid,
                            batch.index.column_name,
                            batch.index.family,
                            batch.entry_kind.empty() ? "exact"
                                                     : batch.entry_kind,
                            entry.encoded_key,
                            entry.payload_value,
                            entry.row_uuid,
                            entry.version_uuid});
  }
  if (sort_allowed && job->entries.size() > before_batch + 1) {
    SortPreparedIndexEntryRangeIfNeeded(&job->entries,
                                        before_batch,
                                        &job->sorted_batch_count);
  }
}

PreparedIndexAppendJob BuildPreparedExactIndexAppendJob(
    const std::vector<MgaExactIndexEntryAppendBatch>& batches) {
  PreparedIndexAppendJob job;
  try {
    for (const auto& batch : batches) {
      AddPreparedExactIndexAppendBatch(batch, &job);
      if (!job.ok) { return job; }
    }
  } catch (const std::exception& ex) {
    job.ok = false;
    job.diagnostic = MakeInvalidRequestDiagnostic("mga.index_store",
                                                  std::string("exact_index_materialization_failed:") +
                                                      ex.what());
  } catch (...) {
    job.ok = false;
    job.diagnostic = MakeInvalidRequestDiagnostic("mga.index_store",
                                                  "exact_index_materialization_failed");
  }
  return job;
}

PreparedIndexAppendJob BuildPreparedExactIndexAppendJob(
    const MgaExactIndexEntryAppendBatch& batch) {
  PreparedIndexAppendJob job;
  try {
    AddPreparedExactIndexAppendBatch(batch, &job);
  } catch (const std::exception& ex) {
    job.ok = false;
    job.diagnostic = MakeInvalidRequestDiagnostic("mga.index_store",
                                                  std::string("exact_index_materialization_failed:") +
                                                      ex.what());
  } catch (...) {
    job.ok = false;
    job.diagnostic = MakeInvalidRequestDiagnostic("mga.index_store",
                                                  "exact_index_materialization_failed");
  }
  return job;
}

PreparedIndexLineBufferJob BuildPreparedIndexLineBuffers(
    const EngineRequestContext& context,
    std::vector<PreparedIndexEntryLine> entries,
    std::uint64_t first_event_sequence) {
  PreparedIndexLineBufferJob job;
  if (entries.empty()) {
    return job;
  }
  const std::string single_table_uuid = entries.front().table_uuid;
  const bool single_table_batch =
      std::all_of(std::next(entries.begin()), entries.end(), [&](const auto& entry) {
        return entry.table_uuid == single_table_uuid;
      });
  if (single_table_batch) {
    const std::string scoped_path = ScopedIndexStorePath(context, single_table_uuid);
    std::string& scoped_buffer = job.scoped_lines[scoped_path];
    scoped_buffer.reserve(entries.size() * kHotAppendIndexLineReserveBytes);
    std::uint64_t event_sequence = first_event_sequence;
    for (const auto& entry : entries) {
      AppendIndexEntryStoreLine(&scoped_buffer,
                                context.local_transaction_id,
                                event_sequence++,
                                entry.index_uuid,
                                entry.table_uuid,
                                entry.column_name,
                                entry.family,
                                entry.entry_kind,
                                entry.key,
                                entry.payload,
                                entry.row_uuid,
                                entry.version_uuid);
    }
    return job;
  }
  std::map<std::string, std::size_t> entries_per_path;
  for (const auto& entry : entries) {
    entries_per_path[ScopedIndexStorePath(context, entry.table_uuid)] += 1;
  }
  for (const auto& [path, count] : entries_per_path) {
    job.scoped_lines[path].reserve(count * kHotAppendIndexLineReserveBytes);
  }
  std::uint64_t event_sequence = first_event_sequence;
  for (const auto& entry : entries) {
    std::string& scoped_buffer =
        job.scoped_lines[ScopedIndexStorePath(context, entry.table_uuid)];
    AppendIndexEntryStoreLine(&scoped_buffer,
                              context.local_transaction_id,
                              event_sequence++,
                              entry.index_uuid,
                              entry.table_uuid,
                              entry.column_name,
                              entry.family,
                              entry.entry_kind,
                              entry.key,
                              entry.payload,
                              entry.row_uuid,
                              entry.version_uuid);
  }
  return job;
}

}  // namespace

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
  std::vector<std::string> allocator_lines;
  std::map<std::string, std::string> scoped_row_lines;
  std::map<std::string, std::string> scoped_row_binary_buffers;
  std::map<std::string, std::string> scoped_index_lines;
  std::map<std::string, std::string> scoped_index_binary_buffers;
  std::map<std::string, std::vector<CrudRowVersionRecord>>
      scoped_decoded_row_appends;
  std::vector<PreparedIndexAppendJob> pending_prepared_index_jobs;
  std::vector<std::future<PreparedIndexAppendJob>> pending_index_materialization_jobs;
  std::map<std::string, ScopedRelationSummaryDelta> scoped_row_summary_deltas;
  bool decoded_row_cache_auto_warm = true;
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

void MgaRelationHotAppendContext::SetDecodedRowCacheAutoWarm(bool enabled) {
  impl_->decoded_row_cache_auto_warm = enabled;
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
      [this]() { return ScanNextRowEventSequence(impl_->context); },
      &impl_->allocator_lines);
  if (!reservation.ok) { return reservation.diagnostic; }
  ++impl_->counters.row_range_reservations;
  std::uint64_t event_sequence = reservation.first;
  std::string row_buffer;
  row_buffer.reserve(rows->size() * kHotAppendRowLineReserveBytes);
  const std::string single_table_uuid = rows->front().table_uuid;
  const bool single_table_batch =
      std::all_of(std::next(rows->begin()), rows->end(), [&](const auto& row) {
        return row.table_uuid == single_table_uuid;
      });
  std::string single_scoped_path;
  std::string* single_scoped_buffer = nullptr;
  std::vector<CrudRowVersionRecord>* single_decoded_appends = nullptr;
  ScopedRelationSummaryDelta* single_summary_delta = nullptr;
  std::map<std::string, std::size_t> rows_per_table;
  std::map<std::string, std::string> scoped_row_path_by_table;
  if (single_table_batch) {
    single_scoped_path = ScopedRowStorePath(impl_->context, single_table_uuid);
    single_scoped_buffer = &impl_->scoped_row_lines[single_scoped_path];
    single_scoped_buffer->reserve(
        single_scoped_buffer->size() +
        rows->size() * kHotAppendRowLineReserveBytes);
    if (impl_->decoded_row_cache_auto_warm &&
        rows->size() <= kScopedDecodedRowCacheMaxAutoWarmRows) {
      single_decoded_appends =
          &impl_->scoped_decoded_row_appends[single_scoped_path];
      single_decoded_appends->reserve(single_decoded_appends->size() +
                                      rows->size());
    }
    single_summary_delta = &impl_->scoped_row_summary_deltas[single_table_uuid];
    auto& summary_delta = *single_summary_delta;
    if (summary_delta.row_version_count == 0 &&
        summary_delta.tombstone_count == 0 &&
        summary_delta.update_count == 0) {
      summary_delta.first_scoped_write =
          !ScopedRelationAnyRowStoreExists(impl_->context,
                                           single_table_uuid) &&
          !FileExistsAndNotEmpty(ScopedSummaryStorePath(impl_->context,
                                                        single_table_uuid));
    }
  } else {
    for (const auto& row : *rows) {
      ++rows_per_table[row.table_uuid];
    }
    for (const auto& [table_uuid, row_count] : rows_per_table) {
      const std::string scoped_path = ScopedRowStorePath(impl_->context, table_uuid);
      std::string& scoped_buffer = impl_->scoped_row_lines[scoped_path];
      scoped_buffer.reserve(scoped_buffer.size() +
                            row_count * kHotAppendRowLineReserveBytes);
      scoped_row_path_by_table.emplace(table_uuid, scoped_path);
      if (impl_->decoded_row_cache_auto_warm &&
          row_count <= kScopedDecodedRowCacheMaxAutoWarmRows) {
        auto& decoded_appends = impl_->scoped_decoded_row_appends[scoped_path];
        decoded_appends.reserve(decoded_appends.size() + row_count);
      }
      auto& summary_delta = impl_->scoped_row_summary_deltas[table_uuid];
      if (summary_delta.row_version_count == 0 &&
          summary_delta.tombstone_count == 0 &&
          summary_delta.update_count == 0) {
        summary_delta.first_scoped_write =
            !ScopedRelationAnyRowStoreExists(impl_->context, table_uuid) &&
            !FileExistsAndNotEmpty(ScopedSummaryStorePath(impl_->context,
                                                          table_uuid));
      }
    }
  }
  for (auto& writable : *rows) {
    writable.event_sequence = event_sequence++;
    writable.sequence = writable.event_sequence;
    const std::size_t line_start = row_buffer.size();
    AppendRowVersionStoreLine(&row_buffer, writable);
    row_buffer.push_back('\n');
    const std::string& scoped_path =
        single_table_batch
            ? single_scoped_path
            : scoped_row_path_by_table.find(writable.table_uuid)->second;
    std::string& scoped_buffer =
        single_table_batch ? *single_scoped_buffer
                           : impl_->scoped_row_lines[scoped_path];
    scoped_buffer.append(row_buffer.data() + line_start,
                         row_buffer.size() - line_start);
    if (single_table_batch) {
      if (single_decoded_appends != nullptr) {
        single_decoded_appends->push_back(writable);
      }
    } else {
      auto decoded = impl_->scoped_decoded_row_appends.find(scoped_path);
      if (decoded != impl_->scoped_decoded_row_appends.end()) {
        decoded->second.push_back(writable);
      }
    }
    auto& summary_delta = single_table_batch
                              ? *single_summary_delta
                              : impl_->scoped_row_summary_deltas[writable.table_uuid];
    ++summary_delta.row_version_count;
    if (writable.deleted) {
      ++summary_delta.tombstone_count;
    }
    if (!writable.previous_version_uuid.empty()) {
      ++summary_delta.update_count;
    }
    impl_->row_dirty = true;
    ++impl_->counters.row_versions_appended;
    if (written_event_sequences != nullptr) {
      written_event_sequences->push_back(writable.event_sequence);
    }
  }
  if (!row_buffer.empty()) {
    impl_->row_out.write(row_buffer.data(),
                         static_cast<std::streamsize>(row_buffer.size()));
    if (!impl_->row_out) {
      return MakeInvalidRequestDiagnostic("mga.row_store", "row_version_append_failed");
    }
  }
  return OkDiagnostic();
}

EngineApiDiagnostic MgaRelationHotAppendContext::AppendRowVersions(
    std::vector<CrudRowVersionRecord>* rows,
    const std::vector<std::vector<std::pair<std::string, std::string>>>*
        value_batch,
    std::vector<std::uint64_t>* written_event_sequences) {
  if (value_batch == nullptr) {
    return AppendRowVersions(rows, written_event_sequences);
  }
  if (impl_->context.database_path.empty()) {
    return MakeInvalidRequestDiagnostic("mga.row_store", "database_path_required");
  }
  if (rows == nullptr || rows->empty()) {
    return MakeInvalidRequestDiagnostic("mga.row_store", "row_versions_required");
  }
  if (value_batch->size() != rows->size()) {
    return MakeInvalidRequestDiagnostic("mga.row_store",
                                        "row_value_batch_shape_invalid");
  }
  if (!impl_->row_out.is_open()) {
    impl_->row_out.open(RowStorePath(impl_->context),
                        std::ios::app | std::ios::binary);
    if (!impl_->row_out) {
      return MakeInvalidRequestDiagnostic("mga.row_store",
                                          "row_version_append_failed");
    }
    ++impl_->counters.row_stream_opens;
  }
  const auto reservation = ReserveEventSequenceRange(
      impl_->context,
      "row_versions",
      RowStorePath(impl_->context),
      static_cast<std::uint64_t>(rows->size()),
      [this]() { return ScanNextRowEventSequence(impl_->context); },
      &impl_->allocator_lines);
  if (!reservation.ok) { return reservation.diagnostic; }
  ++impl_->counters.row_range_reservations;
  std::uint64_t event_sequence = reservation.first;
  std::string row_buffer;
  row_buffer.reserve(rows->size() * kHotAppendRowLineReserveBytes);
  const std::string single_table_uuid = rows->front().table_uuid;
  const bool single_table_batch =
      std::all_of(std::next(rows->begin()), rows->end(), [&](const auto& row) {
        return row.table_uuid == single_table_uuid;
      });
  std::string single_scoped_path;
  std::string* single_scoped_buffer = nullptr;
  std::vector<CrudRowVersionRecord>* single_decoded_appends = nullptr;
  ScopedRelationSummaryDelta* single_summary_delta = nullptr;
  std::map<std::string, std::size_t> rows_per_table;
  std::map<std::string, std::string> scoped_row_path_by_table;
  if (single_table_batch) {
    single_scoped_path = ScopedRowStorePath(impl_->context, single_table_uuid);
    single_scoped_buffer = &impl_->scoped_row_lines[single_scoped_path];
    single_scoped_buffer->reserve(
        single_scoped_buffer->size() +
        rows->size() * kHotAppendRowLineReserveBytes);
    if (impl_->decoded_row_cache_auto_warm &&
        rows->size() <= kScopedDecodedRowCacheMaxAutoWarmRows) {
      single_decoded_appends =
          &impl_->scoped_decoded_row_appends[single_scoped_path];
      single_decoded_appends->reserve(single_decoded_appends->size() +
                                      rows->size());
    }
    single_summary_delta = &impl_->scoped_row_summary_deltas[single_table_uuid];
    auto& summary_delta = *single_summary_delta;
    if (summary_delta.row_version_count == 0 &&
        summary_delta.tombstone_count == 0 &&
        summary_delta.update_count == 0) {
      summary_delta.first_scoped_write =
          !ScopedRelationAnyRowStoreExists(impl_->context,
                                           single_table_uuid) &&
          !FileExistsAndNotEmpty(ScopedSummaryStorePath(impl_->context,
                                                        single_table_uuid));
    }
  } else {
    for (const auto& row : *rows) {
      ++rows_per_table[row.table_uuid];
    }
    for (const auto& [table_uuid, row_count] : rows_per_table) {
      const std::string scoped_path = ScopedRowStorePath(impl_->context, table_uuid);
      std::string& scoped_buffer = impl_->scoped_row_lines[scoped_path];
      scoped_buffer.reserve(scoped_buffer.size() +
                            row_count * kHotAppendRowLineReserveBytes);
      scoped_row_path_by_table.emplace(table_uuid, scoped_path);
      if (impl_->decoded_row_cache_auto_warm &&
          row_count <= kScopedDecodedRowCacheMaxAutoWarmRows) {
        auto& decoded_appends = impl_->scoped_decoded_row_appends[scoped_path];
        decoded_appends.reserve(decoded_appends.size() + row_count);
      }
      auto& summary_delta = impl_->scoped_row_summary_deltas[table_uuid];
      if (summary_delta.row_version_count == 0 &&
          summary_delta.tombstone_count == 0 &&
          summary_delta.update_count == 0) {
        summary_delta.first_scoped_write =
            !ScopedRelationAnyRowStoreExists(impl_->context, table_uuid) &&
            !FileExistsAndNotEmpty(ScopedSummaryStorePath(impl_->context,
                                                          table_uuid));
      }
    }
  }
  for (std::size_t index = 0; index < rows->size(); ++index) {
    auto& writable = (*rows)[index];
    const auto& values = (*value_batch)[index];
    writable.event_sequence = event_sequence++;
    writable.sequence = writable.event_sequence;
    const std::size_t line_start = row_buffer.size();
    AppendRowVersionStoreLine(&row_buffer,
                              writable,
                              writable.event_sequence,
                              values);
    row_buffer.push_back('\n');
    const std::string& scoped_path =
        single_table_batch
            ? single_scoped_path
            : scoped_row_path_by_table.find(writable.table_uuid)->second;
    std::string& scoped_buffer =
        single_table_batch ? *single_scoped_buffer
                           : impl_->scoped_row_lines[scoped_path];
    scoped_buffer.append(row_buffer.data() + line_start,
                         row_buffer.size() - line_start);
    std::vector<CrudRowVersionRecord>* decoded_appends = nullptr;
    if (single_table_batch) {
      decoded_appends = single_decoded_appends;
    } else {
      auto decoded = impl_->scoped_decoded_row_appends.find(scoped_path);
      if (decoded != impl_->scoped_decoded_row_appends.end()) {
        decoded_appends = &decoded->second;
      }
    }
    if (decoded_appends != nullptr) {
      CrudRowVersionRecord cache_row = writable;
      cache_row.values = values;
      decoded_appends->push_back(std::move(cache_row));
    }
    auto& summary_delta = single_table_batch
                              ? *single_summary_delta
                              : impl_->scoped_row_summary_deltas[writable.table_uuid];
    ++summary_delta.row_version_count;
    if (writable.deleted) {
      ++summary_delta.tombstone_count;
    }
    if (!writable.previous_version_uuid.empty()) {
      ++summary_delta.update_count;
    }
    impl_->row_dirty = true;
    ++impl_->counters.row_versions_appended;
    if (written_event_sequences != nullptr) {
      written_event_sequences->push_back(writable.event_sequence);
    }
  }
  if (!row_buffer.empty()) {
    impl_->row_out.write(row_buffer.data(),
                         static_cast<std::streamsize>(row_buffer.size()));
    if (!impl_->row_out) {
      return MakeInvalidRequestDiagnostic("mga.row_store",
                                          "row_version_append_failed");
    }
  }
  return OkDiagnostic();
}

EngineApiDiagnostic MgaRelationHotAppendContext::AppendRowVersionsReadOnly(
    const std::vector<CrudRowVersionRecord>& rows) {
  return AppendRowVersionsReadOnly(
      rows,
      static_cast<const std::vector<std::vector<std::pair<std::string, std::string>>>*>(
          nullptr));
}

EngineApiDiagnostic MgaRelationHotAppendContext::AppendRowVersionsReadOnly(
    const std::vector<CrudRowVersionRecord>& rows,
    const std::vector<std::vector<std::pair<std::string, std::string>>>*
        value_batch) {
  if (impl_->context.database_path.empty()) {
    return MakeInvalidRequestDiagnostic("mga.row_store", "database_path_required");
  }
  if (rows.empty()) {
    return MakeInvalidRequestDiagnostic("mga.row_store", "row_versions_required");
  }
  if (value_batch != nullptr && value_batch->size() != rows.size()) {
    return MakeInvalidRequestDiagnostic("mga.row_store",
                                        "row_value_batch_shape_invalid");
  }
  if (!impl_->row_out.is_open()) {
    impl_->row_out.open(RowStorePath(impl_->context), std::ios::app | std::ios::binary);
    if (!impl_->row_out) {
      return MakeInvalidRequestDiagnostic("mga.row_store", "row_version_append_failed");
    }
    ++impl_->counters.row_stream_opens;
  }
  const auto reservation = ReserveEventSequenceRange(
      impl_->context,
      "row_versions",
      RowStorePath(impl_->context),
      static_cast<std::uint64_t>(rows.size()),
      [this]() { return ScanNextRowEventSequence(impl_->context); },
      &impl_->allocator_lines);
  if (!reservation.ok) { return reservation.diagnostic; }
  ++impl_->counters.row_range_reservations;
  std::uint64_t event_sequence = reservation.first;
  std::string row_buffer;
  row_buffer.reserve(rows.size() * kHotAppendRowLineReserveBytes);
  const std::string single_table_uuid = rows.front().table_uuid;
  const bool single_table_batch =
      std::all_of(std::next(rows.begin()), rows.end(), [&](const auto& row) {
        return row.table_uuid == single_table_uuid;
      });
  std::string single_scoped_path;
  std::string* single_scoped_buffer = nullptr;
  std::vector<CrudRowVersionRecord>* single_decoded_appends = nullptr;
  ScopedRelationSummaryDelta* single_summary_delta = nullptr;
  std::map<std::string, std::size_t> rows_per_table;
  std::map<std::string, std::string> scoped_row_path_by_table;
  if (single_table_batch) {
    single_scoped_path = ScopedRowStorePath(impl_->context, single_table_uuid);
    single_scoped_buffer = &impl_->scoped_row_lines[single_scoped_path];
    single_scoped_buffer->reserve(
        single_scoped_buffer->size() +
        rows.size() * kHotAppendRowLineReserveBytes);
    if (impl_->decoded_row_cache_auto_warm &&
        rows.size() <= kScopedDecodedRowCacheMaxAutoWarmRows) {
      single_decoded_appends =
          &impl_->scoped_decoded_row_appends[single_scoped_path];
      single_decoded_appends->reserve(single_decoded_appends->size() +
                                      rows.size());
    }
    single_summary_delta = &impl_->scoped_row_summary_deltas[single_table_uuid];
    auto& summary_delta = *single_summary_delta;
    if (summary_delta.row_version_count == 0 &&
        summary_delta.tombstone_count == 0 &&
        summary_delta.update_count == 0) {
      summary_delta.first_scoped_write =
          !ScopedRelationAnyRowStoreExists(impl_->context,
                                           single_table_uuid) &&
          !FileExistsAndNotEmpty(ScopedSummaryStorePath(impl_->context,
                                                        single_table_uuid));
    }
  } else {
    for (const auto& row : rows) {
      ++rows_per_table[row.table_uuid];
    }
    for (const auto& [table_uuid, row_count] : rows_per_table) {
      const std::string scoped_path = ScopedRowStorePath(impl_->context, table_uuid);
      std::string& scoped_buffer = impl_->scoped_row_lines[scoped_path];
      scoped_buffer.reserve(scoped_buffer.size() +
                            row_count * kHotAppendRowLineReserveBytes);
      scoped_row_path_by_table.emplace(table_uuid, scoped_path);
      if (impl_->decoded_row_cache_auto_warm &&
          row_count <= kScopedDecodedRowCacheMaxAutoWarmRows) {
        auto& decoded_appends = impl_->scoped_decoded_row_appends[scoped_path];
        decoded_appends.reserve(decoded_appends.size() + row_count);
      }
      auto& summary_delta = impl_->scoped_row_summary_deltas[table_uuid];
      if (summary_delta.row_version_count == 0 &&
          summary_delta.tombstone_count == 0 &&
          summary_delta.update_count == 0) {
        summary_delta.first_scoped_write =
            !ScopedRelationAnyRowStoreExists(impl_->context, table_uuid) &&
            !FileExistsAndNotEmpty(ScopedSummaryStorePath(impl_->context,
                                                          table_uuid));
      }
    }
  }
  for (std::size_t index = 0; index < rows.size(); ++index) {
    const auto& row = rows[index];
    const auto& values =
        value_batch == nullptr ? row.values : (*value_batch)[index];
    const std::uint64_t row_event_sequence = event_sequence++;
    const std::size_t line_start = row_buffer.size();
    AppendRowVersionStoreLine(&row_buffer, row, row_event_sequence, values);
    row_buffer.push_back('\n');
    const std::string& scoped_path =
        single_table_batch
            ? single_scoped_path
            : scoped_row_path_by_table.find(row.table_uuid)->second;
    std::string& scoped_buffer =
        single_table_batch ? *single_scoped_buffer
                           : impl_->scoped_row_lines[scoped_path];
    scoped_buffer.append(row_buffer.data() + line_start,
                         row_buffer.size() - line_start);
    std::vector<CrudRowVersionRecord>* decoded_appends = nullptr;
    if (single_table_batch) {
      decoded_appends = single_decoded_appends;
    } else {
      auto decoded = impl_->scoped_decoded_row_appends.find(scoped_path);
      if (decoded != impl_->scoped_decoded_row_appends.end()) {
        decoded_appends = &decoded->second;
      }
    }
    if (decoded_appends != nullptr) {
      CrudRowVersionRecord cache_row = row;
      cache_row.event_sequence = row_event_sequence;
      cache_row.sequence = row_event_sequence;
      cache_row.values = values;
      decoded_appends->push_back(std::move(cache_row));
    }
    auto& summary_delta =
        single_table_batch ? *single_summary_delta
                           : impl_->scoped_row_summary_deltas[row.table_uuid];
    ++summary_delta.row_version_count;
    if (row.deleted) {
      ++summary_delta.tombstone_count;
    }
    if (!row.previous_version_uuid.empty()) {
      ++summary_delta.update_count;
    }
    impl_->row_dirty = true;
    ++impl_->counters.row_versions_appended;
  }
  if (!row_buffer.empty()) {
    impl_->row_out.write(row_buffer.data(),
                         static_cast<std::streamsize>(row_buffer.size()));
    if (!impl_->row_out) {
      return MakeInvalidRequestDiagnostic("mga.row_store",
                                          "row_version_append_failed");
    }
  }
  return OkDiagnostic();
}

EngineApiDiagnostic
MgaRelationHotAppendContext::AppendRowVersionsReadOnlyScopedOnly(
    const std::vector<CrudRowVersionRecord>& rows,
    const std::vector<std::vector<std::pair<std::string, std::string>>>*
        value_batch,
    bool shared_key_order_known) {
  if (impl_->context.database_path.empty()) {
    return MakeInvalidRequestDiagnostic("mga.row_store", "database_path_required");
  }
  if (rows.empty()) {
    return MakeInvalidRequestDiagnostic("mga.row_store", "row_versions_required");
  }
  if (value_batch != nullptr && value_batch->size() != rows.size()) {
    return MakeInvalidRequestDiagnostic("mga.row_store",
                                        "row_value_batch_shape_invalid");
  }
  const auto reservation = ReserveEventSequenceRange(
      impl_->context,
      "row_versions",
      RowStorePath(impl_->context),
      static_cast<std::uint64_t>(rows.size()),
      [this]() { return ScanNextRowEventSequence(impl_->context); },
      &impl_->allocator_lines);
  if (!reservation.ok) { return reservation.diagnostic; }
  ++impl_->counters.row_range_reservations;
  std::uint64_t event_sequence = reservation.first;
  std::vector<std::string> encoded_key_cache;
  const auto row_values = [&](std::size_t index)
      -> const std::vector<std::pair<std::string, std::string>>& {
    return value_batch == nullptr ? rows[index].values : (*value_batch)[index];
  };
  if (!rows.empty()) {
    const auto& first_values = row_values(0);
    bool shared_key_order = !first_values.empty();
    if (!shared_key_order_known) {
      for (std::size_t row_index = 1;
           shared_key_order && row_index < rows.size();
           ++row_index) {
        const auto& values = row_values(row_index);
        if (values.size() != first_values.size()) {
          shared_key_order = false;
          break;
        }
        for (std::size_t field_index = 0; field_index < values.size();
             ++field_index) {
          if (values[field_index].first != first_values[field_index].first) {
            shared_key_order = false;
            break;
          }
        }
      }
    }
    if (shared_key_order) {
      encoded_key_cache.reserve(first_values.size());
      for (const auto& [key, _] : first_values) {
        std::string encoded_key;
        encoded_key.reserve(key.size() * 2);
        AppendHexEncoded(&encoded_key, key);
        encoded_key_cache.push_back(std::move(encoded_key));
      }
    }
  }
  const auto* encoded_key_cache_ptr =
      encoded_key_cache.empty() ? nullptr : &encoded_key_cache;

  const std::string single_table_uuid = rows.front().table_uuid;
  const bool single_table_batch =
      std::all_of(std::next(rows.begin()), rows.end(), [&](const auto& row) {
        return row.table_uuid == single_table_uuid;
      });
  std::string single_scoped_path;
  std::string* single_scoped_buffer = nullptr;
  std::vector<CrudRowVersionRecord>* single_decoded_appends = nullptr;
  ScopedRelationSummaryDelta* single_summary_delta = nullptr;
  std::map<std::string, std::size_t> rows_per_table;
  std::map<std::string, std::string> scoped_row_path_by_table;
  if (single_table_batch) {
    single_scoped_path = ScopedRowStorePath(impl_->context, single_table_uuid);
    single_scoped_buffer = &impl_->scoped_row_lines[single_scoped_path];
    single_scoped_buffer->reserve(
        single_scoped_buffer->size() +
        rows.size() * kHotAppendRowLineReserveBytes);
    if (impl_->decoded_row_cache_auto_warm &&
        rows.size() <= kScopedDecodedRowCacheMaxAutoWarmRows) {
      single_decoded_appends =
          &impl_->scoped_decoded_row_appends[single_scoped_path];
      single_decoded_appends->reserve(single_decoded_appends->size() +
                                      rows.size());
    }
    single_summary_delta = &impl_->scoped_row_summary_deltas[single_table_uuid];
    auto& summary_delta = *single_summary_delta;
    if (summary_delta.row_version_count == 0 &&
        summary_delta.tombstone_count == 0 &&
        summary_delta.update_count == 0) {
      summary_delta.first_scoped_write =
          !ScopedRelationAnyRowStoreExists(impl_->context,
                                           single_table_uuid) &&
          !FileExistsAndNotEmpty(ScopedSummaryStorePath(impl_->context,
                                                        single_table_uuid));
    }
  } else {
    for (const auto& row : rows) {
      ++rows_per_table[row.table_uuid];
    }
    for (const auto& [table_uuid, row_count] : rows_per_table) {
      const std::string scoped_path = ScopedRowStorePath(impl_->context, table_uuid);
      std::string& scoped_buffer = impl_->scoped_row_lines[scoped_path];
      scoped_buffer.reserve(scoped_buffer.size() +
                            row_count * kHotAppendRowLineReserveBytes);
      scoped_row_path_by_table.emplace(table_uuid, scoped_path);
      if (impl_->decoded_row_cache_auto_warm &&
          row_count <= kScopedDecodedRowCacheMaxAutoWarmRows) {
        auto& decoded_appends = impl_->scoped_decoded_row_appends[scoped_path];
        decoded_appends.reserve(decoded_appends.size() + row_count);
      }
      auto& summary_delta = impl_->scoped_row_summary_deltas[table_uuid];
      if (summary_delta.row_version_count == 0 &&
          summary_delta.tombstone_count == 0 &&
          summary_delta.update_count == 0) {
        summary_delta.first_scoped_write =
            !ScopedRelationAnyRowStoreExists(impl_->context, table_uuid) &&
            !FileExistsAndNotEmpty(ScopedSummaryStorePath(impl_->context,
                                                          table_uuid));
      }
    }
  }

  for (std::size_t index = 0; index < rows.size(); ++index) {
    const auto& row = rows[index];
    const auto& values = row_values(index);
    const std::uint64_t row_event_sequence = event_sequence++;
    const std::string& scoped_path =
        single_table_batch
            ? single_scoped_path
            : scoped_row_path_by_table.find(row.table_uuid)->second;
    std::string& scoped_buffer =
        single_table_batch ? *single_scoped_buffer
                           : impl_->scoped_row_lines[scoped_path];
    AppendRowVersionStoreLine(&scoped_buffer,
                              row,
                              row_event_sequence,
                              values,
                              encoded_key_cache_ptr);
    scoped_buffer.push_back('\n');
    std::vector<CrudRowVersionRecord>* decoded_appends = nullptr;
    if (single_table_batch) {
      decoded_appends = single_decoded_appends;
    } else {
      auto decoded = impl_->scoped_decoded_row_appends.find(scoped_path);
      if (decoded != impl_->scoped_decoded_row_appends.end()) {
        decoded_appends = &decoded->second;
      }
    }
    if (decoded_appends != nullptr) {
      CrudRowVersionRecord cache_row = row;
      cache_row.event_sequence = row_event_sequence;
      cache_row.sequence = row_event_sequence;
      cache_row.values = values;
      decoded_appends->push_back(std::move(cache_row));
    }
    auto& summary_delta =
        single_table_batch ? *single_summary_delta
                           : impl_->scoped_row_summary_deltas[row.table_uuid];
    ++summary_delta.row_version_count;
    if (row.deleted) {
      ++summary_delta.tombstone_count;
    }
    if (!row.previous_version_uuid.empty()) {
      ++summary_delta.update_count;
    }
    ++impl_->counters.row_versions_appended;
  }
  return OkDiagnostic();
}

EngineApiDiagnostic
MgaRelationHotAppendContext::AppendRowVersionsReadOnlyScopedOnlyTyped(
    const std::vector<CrudRowVersionRecord>& rows,
    std::span<const EngineRowValue> typed_rows,
    std::span<const std::string> shared_field_order) {
  if (impl_->context.database_path.empty()) {
    return MakeInvalidRequestDiagnostic("mga.row_store", "database_path_required");
  }
  if (rows.empty()) {
    return MakeInvalidRequestDiagnostic("mga.row_store", "row_versions_required");
  }
  if (typed_rows.size() != rows.size()) {
    return MakeInvalidRequestDiagnostic("mga.row_store",
                                        "typed_row_value_batch_shape_invalid");
  }
  if (shared_field_order.empty()) {
    return MakeInvalidRequestDiagnostic("mga.row_store",
                                        "typed_row_field_order_required");
  }

  const auto reservation = ReserveEventSequenceRange(
      impl_->context,
      "row_versions",
      RowStorePath(impl_->context),
      static_cast<std::uint64_t>(rows.size()),
      [this]() { return ScanNextRowEventSequence(impl_->context); },
      &impl_->allocator_lines);
  if (!reservation.ok) { return reservation.diagnostic; }
  ++impl_->counters.row_range_reservations;

  const std::string single_table_uuid = rows.front().table_uuid;
  const bool single_table_batch =
      std::all_of(std::next(rows.begin()), rows.end(), [&](const auto& row) {
        return row.table_uuid == single_table_uuid;
      });
  if (!single_table_batch) {
    return MakeInvalidRequestDiagnostic("mga.row_store",
                                        "typed_row_single_table_required");
  }

  for (const auto& typed_row : typed_rows) {
    if (typed_row.fields.size() != shared_field_order.size()) {
      return MakeInvalidRequestDiagnostic("mga.row_store",
                                          "typed_row_field_order_mismatch");
    }
  }

  const std::string scoped_text_path = ScopedRowStorePath(impl_->context,
                                                         single_table_uuid);
  const std::string scoped_binary_path = ScopedRowBinaryStorePath(
      impl_->context,
      single_table_uuid);
  std::string& scoped_buffer =
      impl_->scoped_row_binary_buffers[scoped_binary_path];
  auto& summary_delta = impl_->scoped_row_summary_deltas[single_table_uuid];
  if (summary_delta.row_version_count == 0 &&
      summary_delta.tombstone_count == 0 &&
      summary_delta.update_count == 0) {
    summary_delta.first_scoped_write =
        !FileExistsAndNotEmpty(scoped_text_path) &&
        !FileExistsAndNotEmpty(scoped_binary_path) &&
        !FileExistsAndNotEmpty(ScopedSummaryStorePath(impl_->context,
                                                      single_table_uuid));
  }

  const std::size_t binary_buffer_start = scoped_buffer.size();
  if (!AppendScopedRowBinaryBatch(&scoped_buffer,
                                  rows,
                                  typed_rows,
                                  shared_field_order,
                                  reservation.first)) {
    return MakeInvalidRequestDiagnostic("mga.row_store",
                                        "typed_row_binary_batch_encode_failed");
  }
  ++impl_->counters.scoped_row_binary_batches;
  impl_->counters.scoped_row_binary_rows +=
      static_cast<std::uint64_t>(rows.size());
  impl_->counters.scoped_row_binary_bytes +=
      static_cast<std::uint64_t>(scoped_buffer.size() - binary_buffer_start);

  for (const auto& row : rows) {
    ++summary_delta.row_version_count;
    if (row.deleted) {
      ++summary_delta.tombstone_count;
    }
    if (!row.previous_version_uuid.empty()) {
      ++summary_delta.update_count;
    }
    ++impl_->counters.row_versions_appended;
  }
  return OkDiagnostic();
}

EngineApiDiagnostic
MgaRelationHotAppendContext::AppendRowVersionIdentitiesReadOnlyScopedOnlyTyped(
    const std::vector<CrudRowVersionRecord>& row_identities,
    const std::string& table_uuid,
    const std::string& temporary_session_uuid,
    std::span<const EngineRowValue> typed_rows,
    std::span<const std::string> shared_field_order) {
  if (impl_->context.database_path.empty()) {
    return MakeInvalidRequestDiagnostic("mga.row_store", "database_path_required");
  }
  if (row_identities.empty()) {
    return MakeInvalidRequestDiagnostic("mga.row_store", "row_versions_required");
  }
  if (table_uuid.empty()) {
    return MakeInvalidRequestDiagnostic("mga.row_store", "target_table_uuid_required");
  }
  if (table_uuid == "unknown") {
    return MakeInvalidRequestDiagnostic("mga.row_store",
                                        "target_table_uuid_unresolved");
  }
  if (typed_rows.size() != row_identities.size()) {
    return MakeInvalidRequestDiagnostic("mga.row_store",
                                        "typed_row_value_batch_shape_invalid");
  }
  if (shared_field_order.empty()) {
    return MakeInvalidRequestDiagnostic("mga.row_store",
                                        "typed_row_field_order_required");
  }
  for (const auto& typed_row : typed_rows) {
    if (typed_row.fields.size() != shared_field_order.size()) {
      return MakeInvalidRequestDiagnostic("mga.row_store",
                                          "typed_row_field_order_mismatch");
    }
  }
  for (const auto& row : row_identities) {
    if (!row.table_uuid.empty() && row.table_uuid != table_uuid) {
      return MakeInvalidRequestDiagnostic("mga.row_store",
                                          "typed_row_table_uuid_mismatch");
    }
  }

  const auto reservation = ReserveEventSequenceRange(
      impl_->context,
      "row_versions",
      RowStorePath(impl_->context),
      static_cast<std::uint64_t>(row_identities.size()),
      [this]() { return ScanNextRowEventSequence(impl_->context); },
      &impl_->allocator_lines);
  if (!reservation.ok) { return reservation.diagnostic; }
  ++impl_->counters.row_range_reservations;

  const std::string scoped_text_path = ScopedRowStorePath(impl_->context,
                                                         table_uuid);
  const std::string scoped_binary_path = ScopedRowBinaryStorePath(impl_->context,
                                                                 table_uuid);
  std::string& scoped_buffer =
      impl_->scoped_row_binary_buffers[scoped_binary_path];
  auto& summary_delta = impl_->scoped_row_summary_deltas[table_uuid];
  if (summary_delta.row_version_count == 0 &&
      summary_delta.tombstone_count == 0 &&
      summary_delta.update_count == 0) {
    summary_delta.first_scoped_write =
        !FileExistsAndNotEmpty(scoped_text_path) &&
        !FileExistsAndNotEmpty(scoped_binary_path) &&
        !FileExistsAndNotEmpty(ScopedSummaryStorePath(impl_->context,
                                                      table_uuid));
  }

  const std::size_t binary_buffer_start = scoped_buffer.size();
  if (!AppendScopedRowIdentityBinaryBatch(&scoped_buffer,
                                          row_identities,
                                          table_uuid,
                                          temporary_session_uuid,
                                          typed_rows,
                                          shared_field_order,
                                          impl_->context.local_transaction_id,
                                          reservation.first)) {
    return MakeInvalidRequestDiagnostic("mga.row_store",
                                        "typed_row_binary_batch_encode_failed");
  }
  ++impl_->counters.scoped_row_binary_batches;
  impl_->counters.scoped_row_binary_rows +=
      static_cast<std::uint64_t>(row_identities.size());
  impl_->counters.scoped_row_binary_bytes +=
      static_cast<std::uint64_t>(scoped_buffer.size() - binary_buffer_start);
  summary_delta.row_version_count +=
      static_cast<std::uint64_t>(row_identities.size());
  impl_->counters.row_versions_appended +=
      static_cast<std::uint64_t>(row_identities.size());
  return OkDiagnostic();
}

EngineApiDiagnostic
MgaRelationHotAppendContext::AppendRowVersionIdentitiesReadOnlyScopedOnlyNativePacket(
    const std::vector<CrudRowVersionRecord>& row_identities,
    const std::string& table_uuid,
    const std::string& temporary_session_uuid,
    const EngineNativeRowPacketFrame& frame) {
  if (impl_->context.database_path.empty()) {
    return MakeInvalidRequestDiagnostic("mga.row_store", "database_path_required");
  }
  if (row_identities.empty()) {
    return MakeInvalidRequestDiagnostic("mga.row_store", "row_versions_required");
  }
  if (table_uuid.empty() || table_uuid == "unknown") {
    return MakeInvalidRequestDiagnostic("mga.row_store",
                                        "target_table_uuid_unresolved");
  }
  if (!frame.present || frame.row_count != row_identities.size() ||
      frame.field_order.empty()) {
    return MakeInvalidRequestDiagnostic("mga.row_store",
                                        "native_row_packet_shape_invalid");
  }
  for (const auto& row : row_identities) {
    if (!row.table_uuid.empty() && row.table_uuid != table_uuid) {
      return MakeInvalidRequestDiagnostic("mga.row_store",
                                          "native_row_packet_table_uuid_mismatch");
    }
  }

  const auto reservation = ReserveEventSequenceRange(
      impl_->context,
      "row_versions",
      RowStorePath(impl_->context),
      static_cast<std::uint64_t>(row_identities.size()),
      [this]() { return ScanNextRowEventSequence(impl_->context); },
      &impl_->allocator_lines);
  if (!reservation.ok) { return reservation.diagnostic; }
  ++impl_->counters.row_range_reservations;

  const std::string scoped_text_path = ScopedRowStorePath(impl_->context,
                                                         table_uuid);
  const std::string scoped_binary_path = ScopedRowBinaryStorePath(impl_->context,
                                                                 table_uuid);
  std::string& scoped_buffer =
      impl_->scoped_row_binary_buffers[scoped_binary_path];
  auto& summary_delta = impl_->scoped_row_summary_deltas[table_uuid];
  if (summary_delta.row_version_count == 0 &&
      summary_delta.tombstone_count == 0 &&
      summary_delta.update_count == 0) {
    summary_delta.first_scoped_write =
        !FileExistsAndNotEmpty(scoped_text_path) &&
        !FileExistsAndNotEmpty(scoped_binary_path) &&
        !FileExistsAndNotEmpty(ScopedSummaryStorePath(impl_->context,
                                                      table_uuid));
  }

  const std::size_t binary_buffer_start = scoped_buffer.size();
  if (!AppendScopedRowIdentityNativePacketBatch(&scoped_buffer,
                                                row_identities,
                                                table_uuid,
                                                temporary_session_uuid,
                                                frame,
                                                impl_->context.local_transaction_id,
                                                reservation.first)) {
    return MakeInvalidRequestDiagnostic("mga.row_store",
                                        "native_row_packet_binary_batch_encode_failed");
  }
  ++impl_->counters.scoped_row_binary_batches;
  impl_->counters.scoped_row_binary_rows +=
      static_cast<std::uint64_t>(row_identities.size());
  impl_->counters.scoped_row_binary_bytes +=
      static_cast<std::uint64_t>(scoped_buffer.size() - binary_buffer_start);
  summary_delta.row_version_count +=
      static_cast<std::uint64_t>(row_identities.size());
  impl_->counters.row_versions_appended +=
      static_cast<std::uint64_t>(row_identities.size());
  return OkDiagnostic();
}

EngineApiDiagnostic MgaRelationHotAppendContext::FlushRowVersions() {
  if (!AppendDeferredEventSequenceAllocatorLines(impl_->context,
                                                 &impl_->allocator_lines,
                                                 &impl_->counters)) {
    return MakeInvalidRequestDiagnostic("mga.row_store",
                                        "event_sequence_allocator_batch_append_failed");
  }
  const bool has_scoped_rows =
      !impl_->scoped_row_lines.empty() ||
      !impl_->scoped_row_binary_buffers.empty();
  std::map<std::string, std::string> scoped_row_write_buffers =
      std::move(impl_->scoped_row_lines);
  for (auto& [path, buffer] : impl_->scoped_row_binary_buffers) {
    auto found = scoped_row_write_buffers.find(path);
    if (found != scoped_row_write_buffers.end()) {
      found->second.append(buffer);
    } else {
      scoped_row_write_buffers.emplace(std::move(path), std::move(buffer));
    }
  }
  impl_->scoped_row_binary_buffers.clear();
  const bool can_overlap_scoped_rows =
      has_scoped_rows && impl_->row_out.is_open() && impl_->row_dirty;
  std::future<ScopedRelationAppendResult> scoped_row_future;
  if (can_overlap_scoped_rows) {
    scoped_row_future = std::async(
        std::launch::async,
        [pending = &scoped_row_write_buffers]() {
          return AppendScopedRelationLinesWithCounters(*pending);
        });
  }
  bool row_flush_ok = true;
  if (impl_->row_out.is_open() && impl_->row_dirty) {
    impl_->row_out.flush();
    if (!impl_->row_out) {
      row_flush_ok = false;
    } else {
      impl_->row_dirty = false;
      ++impl_->counters.row_stream_flushes;
    }
  }
  ScopedRelationAppendResult scoped_row_result;
  scoped_row_result.ok = true;
  if (can_overlap_scoped_rows) {
    scoped_row_result = scoped_row_future.get();
    AddScopedRelationAppendCounters(
        scoped_row_result,
        &impl_->counters.scoped_row_stream_opens,
        &impl_->counters.scoped_row_stream_flushes,
        &impl_->counters.scoped_row_write_batches,
        &impl_->counters.scoped_row_write_tickets_issued,
        &impl_->counters.scoped_row_write_tickets_completed,
        &impl_->counters.scoped_row_write_worker_count);
  } else if (has_scoped_rows) {
    scoped_row_result = AppendScopedRelationLinesWithCounters(
        scoped_row_write_buffers);
    AddScopedRelationAppendCounters(
        scoped_row_result,
        &impl_->counters.scoped_row_stream_opens,
        &impl_->counters.scoped_row_stream_flushes,
        &impl_->counters.scoped_row_write_batches,
        &impl_->counters.scoped_row_write_tickets_issued,
        &impl_->counters.scoped_row_write_tickets_completed,
        &impl_->counters.scoped_row_write_worker_count);
  }
  if (!row_flush_ok) {
    return MakeInvalidRequestDiagnostic("mga.row_store",
                                        "row_version_append_failed");
  }
  if (!scoped_row_result.ok) {
    return MakeInvalidRequestDiagnostic("mga.row_store",
                                        "scoped_row_version_append_failed");
  }
  if (!UpdateScopedRelationSummariesForStoreModule(impl_->context,
                                     impl_->scoped_row_summary_deltas)) {
    return MakeInvalidRequestDiagnostic("mga.row_store",
                                        "scoped_relation_summary_update_failed");
  }
  scratchbird::core::platform::MaybeCrashAtWholeStoreRealDmlBoundary(
      "directory_mutation");
  if (!impl_->scoped_row_binary_buffers.empty()) {
    ClearScopedDecodedRowCache();
  } else {
    UpdateScopedDecodedRowCacheAfterAppend(impl_->scoped_decoded_row_appends,
                                           impl_->scoped_row_lines);
  }
  impl_->scoped_row_lines.clear();
  impl_->scoped_row_binary_buffers.clear();
  impl_->scoped_decoded_row_appends.clear();
  impl_->scoped_row_summary_deltas.clear();
  return OkDiagnostic();
}

EngineApiDiagnostic MgaRelationHotAppendContext::AppendIndexEntryBatches(
    const std::vector<MgaIndexEntryAppendBatch>& batches) {
  if (impl_->context.database_path.empty()) {
    return MakeInvalidRequestDiagnostic("mga.index_store", "database_path_required");
  }
  const bool inline_materialization = batches.size() <= 1;
  for (const auto& batch : batches) {
    if (batch.rows.empty()) { continue; }
    try {
      ++impl_->counters.index_materialization_jobs_queued;
      if (inline_materialization) {
        PreparedIndexAppendJob job =
            BuildPreparedIndexAppendJob(batch);
        if (!job.ok) { return job.diagnostic; }
        impl_->pending_prepared_index_jobs.push_back(std::move(job));
        ++impl_->counters.index_materialization_inline_jobs;
      } else {
        std::vector<MgaIndexEntryAppendBatch> job_batches;
        job_batches.push_back(batch);
        impl_->pending_index_materialization_jobs.push_back(
            std::async(std::launch::async,
                       [job_batches = std::move(job_batches)]() mutable {
                         return BuildPreparedIndexAppendJob(std::move(job_batches));
                       }));
        impl_->counters.index_materialization_worker_count =
            std::max<std::uint64_t>(
                impl_->counters.index_materialization_worker_count,
                static_cast<std::uint64_t>(
                    impl_->pending_index_materialization_jobs.size()));
      }
    } catch (const std::exception& ex) {
      return MakeInvalidRequestDiagnostic("mga.index_store",
                                          std::string("index_materialization_enqueue_failed:") +
                                              ex.what());
    } catch (...) {
      return MakeInvalidRequestDiagnostic("mga.index_store",
                                          "index_materialization_enqueue_failed");
    }
  }
  return OkDiagnostic();
}

EngineApiDiagnostic MgaRelationHotAppendContext::AppendExactIndexEntryBatches(
    const std::vector<MgaExactIndexEntryAppendBatch>& batches) {
  if (impl_->context.database_path.empty()) {
    return MakeInvalidRequestDiagnostic("mga.index_store", "database_path_required");
  }
  if (batches.size() == 1 && impl_->pending_prepared_index_jobs.empty() &&
      impl_->pending_index_materialization_jobs.empty() &&
      ExactIndexBatchAlreadyInAppendOrder(batches.front())) {
    const auto& batch = batches.front();
    if (batch.entries.empty()) {
      return OkDiagnostic();
    }
    const std::string table_uuid =
        batch.index.table_uuid.empty() ? batch.table_uuid : batch.index.table_uuid;
    if (table_uuid.empty() || batch.index.index_uuid.empty()) {
      return MakeInvalidRequestDiagnostic("mga.index_store",
                                          "exact_index_entry_invalid");
    }
    for (const auto& entry : batch.entries) {
      if (entry.encoded_key.empty() || entry.row_uuid.empty() ||
          entry.version_uuid.empty()) {
        return MakeInvalidRequestDiagnostic("mga.index_store",
                                            "exact_index_entry_invalid");
      }
    }

    const auto reservation = ReserveEventSequenceRange(
        impl_->context,
        "index_entries",
        IndexStorePath(impl_->context),
        static_cast<std::uint64_t>(batch.entries.size()),
        [this]() { return ScanNextIndexEventSequence(impl_->context); },
        &impl_->allocator_lines);
    if (!reservation.ok) { return reservation.diagnostic; }
    ++impl_->counters.index_range_reservations;

    const std::string scoped_path = ScopedIndexBinaryStorePath(impl_->context,
                                                              table_uuid);
    std::string& scoped_buffer =
        impl_->scoped_index_binary_buffers[scoped_path];
    if (!AppendScopedExactIndexBinaryBatch(&scoped_buffer,
                                           batch,
                                           impl_->context.local_transaction_id,
                                           reservation.first)) {
      return MakeInvalidRequestDiagnostic("mga.index_store",
                                          "exact_index_binary_batch_encode_failed");
    }

    ++impl_->counters.index_materialization_jobs_queued;
    ++impl_->counters.index_materialization_jobs_completed;
    ++impl_->counters.index_materialization_inline_jobs;
    impl_->counters.index_materialized_entries +=
        static_cast<std::uint64_t>(batch.entries.size());
    impl_->counters.index_entries_appended +=
        static_cast<std::uint64_t>(batch.entries.size());
    return OkDiagnostic();
  }
  const bool inline_materialization = batches.size() <= 1;
  for (const auto& batch : batches) {
    if (batch.entries.empty()) { continue; }
    for (const auto& entry : batch.entries) {
      if (entry.encoded_key.empty() || entry.row_uuid.empty() || entry.version_uuid.empty()) {
        return MakeInvalidRequestDiagnostic("mga.index_store", "exact_index_entry_invalid");
      }
    }
    try {
      ++impl_->counters.index_materialization_jobs_queued;
      if (inline_materialization) {
        PreparedIndexAppendJob job =
            BuildPreparedExactIndexAppendJob(batch);
        if (!job.ok) { return job.diagnostic; }
        impl_->pending_prepared_index_jobs.push_back(std::move(job));
        ++impl_->counters.index_materialization_inline_jobs;
      } else {
        std::vector<MgaExactIndexEntryAppendBatch> job_batches;
        job_batches.push_back(batch);
        impl_->pending_index_materialization_jobs.push_back(
            std::async(std::launch::async,
                       [job_batches = std::move(job_batches)]() mutable {
                         return BuildPreparedExactIndexAppendJob(std::move(job_batches));
                       }));
        impl_->counters.index_materialization_worker_count =
            std::max<std::uint64_t>(
                impl_->counters.index_materialization_worker_count,
                static_cast<std::uint64_t>(
                    impl_->pending_index_materialization_jobs.size()));
      }
    } catch (const std::exception& ex) {
      return MakeInvalidRequestDiagnostic("mga.index_store",
                                          std::string("exact_index_materialization_enqueue_failed:") +
                                              ex.what());
    } catch (...) {
      return MakeInvalidRequestDiagnostic("mga.index_store",
                                          "exact_index_materialization_enqueue_failed");
    }
  }
  return OkDiagnostic();
}

EngineApiDiagnostic MgaRelationHotAppendContext::FlushIndexEntries() {
  if (!impl_->pending_prepared_index_jobs.empty() ||
      !impl_->pending_index_materialization_jobs.empty()) {
    std::vector<PreparedIndexAppendJob> prepared_jobs;
    prepared_jobs.reserve(impl_->pending_prepared_index_jobs.size() +
                          impl_->pending_index_materialization_jobs.size());
    std::uint64_t entry_count = 0;
    for (auto& job : impl_->pending_prepared_index_jobs) {
      ++impl_->counters.index_materialization_jobs_completed;
      if (!job.ok) {
        impl_->pending_prepared_index_jobs.clear();
        impl_->pending_index_materialization_jobs.clear();
        return job.diagnostic;
      }
      entry_count += static_cast<std::uint64_t>(job.entries.size());
      impl_->counters.index_materialized_entries +=
          static_cast<std::uint64_t>(job.entries.size());
      impl_->counters.index_materialization_sort_batches +=
          job.sorted_batch_count;
      prepared_jobs.push_back(std::move(job));
    }
    impl_->pending_prepared_index_jobs.clear();
    for (auto& future : impl_->pending_index_materialization_jobs) {
      PreparedIndexAppendJob job;
      try {
        job = future.get();
      } catch (const std::exception& ex) {
        impl_->pending_prepared_index_jobs.clear();
        impl_->pending_index_materialization_jobs.clear();
        return MakeInvalidRequestDiagnostic("mga.index_store",
                                            std::string("index_materialization_worker_failed:") +
                                                ex.what());
      } catch (...) {
        impl_->pending_prepared_index_jobs.clear();
        impl_->pending_index_materialization_jobs.clear();
        return MakeInvalidRequestDiagnostic("mga.index_store",
                                            "index_materialization_worker_failed");
      }
      ++impl_->counters.index_materialization_jobs_completed;
      if (!job.ok) {
        impl_->pending_prepared_index_jobs.clear();
        impl_->pending_index_materialization_jobs.clear();
        return job.diagnostic;
      }
      entry_count += static_cast<std::uint64_t>(job.entries.size());
      impl_->counters.index_materialized_entries +=
          static_cast<std::uint64_t>(job.entries.size());
      impl_->counters.index_materialization_sort_batches +=
          job.sorted_batch_count;
      prepared_jobs.push_back(std::move(job));
    }
    impl_->pending_index_materialization_jobs.clear();

    if (entry_count != 0) {
      const auto reservation = ReserveEventSequenceRange(
          impl_->context,
          "index_entries",
          IndexStorePath(impl_->context),
          entry_count,
          [this]() { return ScanNextIndexEventSequence(impl_->context); },
          &impl_->allocator_lines);
      if (!reservation.ok) { return reservation.diagnostic; }
      ++impl_->counters.index_range_reservations;

      std::vector<std::future<PreparedIndexLineBufferJob>> line_futures;
      line_futures.reserve(prepared_jobs.size());
      std::uint64_t event_sequence = reservation.first;
      for (auto& job : prepared_jobs) {
        if (job.entries.empty()) { continue; }
        const std::uint64_t first_sequence = event_sequence;
        event_sequence += static_cast<std::uint64_t>(job.entries.size());
        if (prepared_jobs.size() == 1) {
          PreparedIndexLineBufferJob buffer_job =
              BuildPreparedIndexLineBuffers(impl_->context,
                                            std::move(job.entries),
                                            first_sequence);
          if (!buffer_job.ok) {
            return MakeInvalidRequestDiagnostic("mga.index_store",
                                                "index_line_buffer_worker_refused");
          }
          for (auto& [path, buffer] : buffer_job.scoped_lines) {
            impl_->scoped_index_lines[path].append(buffer);
          }
        } else {
          try {
            line_futures.push_back(
                std::async(std::launch::async,
                           [context = impl_->context,
                            entries = std::move(job.entries),
                            first_sequence]() mutable {
                             return BuildPreparedIndexLineBuffers(context,
                                                                  std::move(entries),
                                                                  first_sequence);
                           }));
          } catch (const std::exception& ex) {
            return MakeInvalidRequestDiagnostic("mga.index_store",
                                                std::string("index_line_buffer_enqueue_failed:") +
                                                    ex.what());
          } catch (...) {
            return MakeInvalidRequestDiagnostic("mga.index_store",
                                                "index_line_buffer_enqueue_failed");
          }
        }
      }
      impl_->counters.index_materialization_worker_count =
          std::max<std::uint64_t>(
              impl_->counters.index_materialization_worker_count,
              static_cast<std::uint64_t>(line_futures.size()));

      for (auto& future : line_futures) {
        PreparedIndexLineBufferJob buffer_job;
        try {
          buffer_job = future.get();
        } catch (const std::exception& ex) {
          return MakeInvalidRequestDiagnostic("mga.index_store",
                                              std::string("index_line_buffer_worker_failed:") +
                                                  ex.what());
        } catch (...) {
          return MakeInvalidRequestDiagnostic("mga.index_store",
                                              "index_line_buffer_worker_failed");
        }
        if (!buffer_job.ok) {
          return MakeInvalidRequestDiagnostic("mga.index_store",
                                              "index_line_buffer_worker_refused");
        }
        for (auto& [path, buffer] : buffer_job.scoped_lines) {
          impl_->scoped_index_lines[path].append(buffer);
        }
      }
      impl_->counters.index_entries_appended += entry_count;
      impl_->index_dirty = true;
    }
  }
  if (!AppendDeferredEventSequenceAllocatorLines(impl_->context,
                                                 &impl_->allocator_lines,
                                                 &impl_->counters)) {
    return MakeInvalidRequestDiagnostic("mga.index_store",
                                        "event_sequence_allocator_batch_append_failed");
  }
  bool index_flush_ok = true;
  if (impl_->index_out.is_open() && impl_->index_dirty) {
    impl_->index_out.flush();
    if (!impl_->index_out) {
      index_flush_ok = false;
    } else {
      impl_->index_dirty = false;
      ++impl_->counters.index_stream_flushes;
    }
  }
  ScopedRelationAppendResult scoped_index_result;
  scoped_index_result.ok = true;
  const bool has_scoped_indexes =
      !impl_->scoped_index_lines.empty() ||
      !impl_->scoped_index_binary_buffers.empty();
  std::map<std::string, std::string> scoped_index_write_buffers =
      std::move(impl_->scoped_index_lines);
  for (auto& [path, buffer] : impl_->scoped_index_binary_buffers) {
    auto found = scoped_index_write_buffers.find(path);
    if (found != scoped_index_write_buffers.end()) {
      found->second.append(buffer);
    } else {
      scoped_index_write_buffers.emplace(std::move(path), std::move(buffer));
    }
  }
  impl_->scoped_index_binary_buffers.clear();
  if (has_scoped_indexes) {
    scoped_index_result = AppendScopedRelationLinesWithCounters(
        scoped_index_write_buffers);
    AddScopedRelationAppendCounters(
        scoped_index_result,
        &impl_->counters.scoped_index_stream_opens,
        &impl_->counters.scoped_index_stream_flushes,
        &impl_->counters.scoped_index_write_batches,
        &impl_->counters.scoped_index_write_tickets_issued,
        &impl_->counters.scoped_index_write_tickets_completed,
        &impl_->counters.scoped_index_write_worker_count);
  }
  if (!index_flush_ok) {
    return MakeInvalidRequestDiagnostic("mga.index_store",
                                        "index_entry_append_failed");
  }
  if (!scoped_index_result.ok) {
    return MakeInvalidRequestDiagnostic("mga.index_store",
                                        "scoped_index_entry_append_failed");
  }
  impl_->scoped_index_lines.clear();
  impl_->scoped_index_binary_buffers.clear();
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

}  // namespace scratchbird::engine::internal_api
