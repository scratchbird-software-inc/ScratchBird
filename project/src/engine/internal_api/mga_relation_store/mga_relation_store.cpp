// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "mga_relation_store/mga_relation_store.hpp"
#include "mga_relation_store/mga_contextual_text_descriptor.hpp"
#include "mga_relation_store/mga_event_sequence_allocator.hpp"
#include "mga_relation_store/mga_heap_runtime_support.hpp"
#include "mga_relation_store/mga_large_value_store.hpp"
#include "mga_relation_store/mga_relation_metadata_store.hpp"
#include "mga_relation_store/mga_row_codec.hpp"
#include "mga_relation_store/mga_row_version_reader.hpp"
#include "mga_relation_store/mga_savepoint_store.hpp"
#include "mga_relation_store/mga_update_durable_store.hpp"
#include "mga_relation_store/mga_relation_store_internal_support.hpp"

#include "api_diagnostics.hpp"
#include "catalog/name_resolution_api.hpp"
#include "datatype_catalog_manifest.hpp"
#include "descriptor_value_runtime.hpp"
#include "ipar_fault_injection.hpp"
#include "local_transaction_store.hpp"
#include "query/contextual_text_policy_registry_v2.hpp"
#include "query/contextual_text_target_authority_resolver_v2.hpp"
#include "query/plan_api.hpp"
#include "secondary_index_delta_merge.hpp"
#include "security/security_model.hpp"
#include "transaction/transaction_api.hpp"
#include "transaction_inventory.hpp"
#include "transaction_state.hpp"
#include "typed_update_carrier_codec.hpp"
#include "uuid.hpp"
#include "hash_digest.hpp"
#include "whole_store_crash_injection.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace scratchbird::engine::internal_api {
namespace {

// SEARCH_KEY: SB_ENGINE_MGA_RELATION_ROW_STORE_IMPLEMENTATION_AUTHORITY
// Canonical relation state and mutation implementation. Executor projection is
// isolated in mga_heap_executor.cpp and may consume only its narrow read bridge.

namespace idx = scratchbird::core::index;

constexpr const char* kRowStoreMagic = "SBMGA1";
constexpr std::string_view kLineHexFieldPrefix = "SBHEX:";

using scratchbird::storage::database::AcquireLocalTransactionInventorySnapshot;
using scratchbird::storage::database::LoadLocalTransactionInventoryFromDatabase;
using scratchbird::storage::database::LocalTransactionInventorySnapshot;
using scratchbird::storage::database::RevalidateLocalTransactionInventorySnapshot;
using scratchbird::transaction::mga::LocalTransactionInventory;
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

std::string DescriptorStorePath(const EngineRequestContext& context) {
  return context.database_path + ".sb.mga_relation_descriptors";
}

std::mutex& ScopedRelationSummaryMutex() {
  static std::mutex mutex;
  return mutex;
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
  std::size_t buffer_bytes = 0;
  for (const auto& line : lines) {
    buffer_bytes += line.size() + 1;
  }
  std::string buffer;
  buffer.reserve(buffer_bytes);
  for (const auto& line : lines) {
    buffer.append(line);
    buffer.push_back('\n');
  }
  if (!buffer.empty()) {
    out.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
  }
  out.flush();
  if (stream_flushes != nullptr) { ++(*stream_flushes); }
  return static_cast<bool>(out);
}

std::vector<std::string> ReadLines(const std::string& path) {
  std::vector<std::string> lines;
  std::ifstream in(path, std::ios::binary);
  if (!in) { return lines; }
  std::error_code ignored;
  const auto bytes = std::filesystem::file_size(path, ignored);
  if (!ignored && bytes != static_cast<std::uintmax_t>(-1)) {
    lines.reserve(static_cast<std::size_t>(std::max<std::uintmax_t>(1, bytes / 128)));
  }
  std::string line;
  while (std::getline(in, line)) { lines.push_back(line); }
  return lines;
}

bool FileExistsAndNotEmpty(const std::string& path) {
  std::error_code ignored;
  return std::filesystem::exists(path, ignored) &&
         std::filesystem::file_size(path, ignored) != 0;
}

std::vector<std::string> ReadScopedRelationLinesForTables(
    const EngineRequestContext& context,
    const std::set<std::string>& table_uuids,
    bool row_store,
    bool* used_segments) {
  std::vector<std::string> lines;
  bool used = false;
  for (const auto& table_uuid : table_uuids) {
    const std::string path = row_store
                                 ? ScopedRowStorePath(context, table_uuid)
                                 : ScopedIndexStorePath(context, table_uuid);
    if (!FileExistsAndNotEmpty(path)) {
      continue;
    }
    used = true;
    auto segment_lines = ReadLines(path);
    lines.reserve(lines.size() + segment_lines.size());
    lines.insert(lines.end(),
                 std::make_move_iterator(segment_lines.begin()),
                 std::make_move_iterator(segment_lines.end()));
  }
  if (used_segments != nullptr) {
    *used_segments = used;
  }
  return lines;
}

std::set<std::string> DiscoverScopedRelationTableUuids(
    const EngineRequestContext& context) {
  std::set<std::string> table_uuids;
  const std::filesystem::path root = ScopedRelationStoreRoot(context);
  std::error_code ignored;
  if (!std::filesystem::exists(root, ignored)) {
    return table_uuids;
  }
  for (const auto& entry : std::filesystem::directory_iterator(root, ignored)) {
    if (ignored) { break; }
    if (!entry.is_regular_file(ignored)) {
      ignored.clear();
      continue;
    }
    std::string name = entry.path().filename().string();
    for (const std::string_view suffix :
         {".rows.sbnr", ".indexes.sbnx", ".rows", ".indexes", ".summary"}) {
      if (name.size() > suffix.size() &&
          name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
        table_uuids.insert(name.substr(0, name.size() - suffix.size()));
        break;
      }
    }
    ignored.clear();
  }
  return table_uuids;
}

bool DecodeScopedIndexBinaryStore(
    const std::string& path,
    std::vector<CrudIndexEntryRecord>* entries) {
  if (entries == nullptr) {
    return false;
  }
  if (!FileExistsAndNotEmpty(path)) {
    return true;
  }
  const std::vector<idx::byte> bytes = ReadBinaryFile(path);
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    if (offset + kScopedIndexBinaryBatchMagic.size() > bytes.size() ||
        std::string_view(reinterpret_cast<const char*>(bytes.data() + offset),
                         kScopedIndexBinaryBatchMagic.size()) !=
            kScopedIndexBinaryBatchMagic) {
      return false;
    }
    offset += kScopedIndexBinaryBatchMagic.size();
    std::uint16_t version = 0;
    std::uint16_t flags = 0;
    std::uint64_t entry_count = 0;
    std::uint64_t creator_tx = 0;
    std::uint64_t first_event_sequence = 0;
    if (!ReadBinaryU16(bytes, &offset, &version) ||
        !ReadBinaryU16(bytes, &offset, &flags) ||
        !ReadBinaryU64(bytes, &offset, &entry_count) ||
        !ReadBinaryU64(bytes, &offset, &creator_tx) ||
        !ReadBinaryU64(bytes, &offset, &first_event_sequence) ||
        version != kScopedIndexBinaryVersion ||
        flags != 0 ||
        entry_count >
            static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
      return false;
    }
    std::string table_uuid;
    std::string index_uuid;
    std::string column_name;
    std::string family;
    std::string entry_kind;
    if (!ReadBinaryString(bytes, &offset, &table_uuid) ||
        !ReadBinaryString(bytes, &offset, &index_uuid) ||
        !ReadBinaryString(bytes, &offset, &column_name) ||
        !ReadBinaryString(bytes, &offset, &family) ||
        !ReadBinaryString(bytes, &offset, &entry_kind) ||
        table_uuid.empty() || index_uuid.empty() || entry_kind.empty()) {
      return false;
    }
    entries->reserve(entries->size() + static_cast<std::size_t>(entry_count));
    for (std::uint64_t index = 0; index < entry_count; ++index) {
      CrudIndexEntryRecord entry;
      entry.creator_tx = creator_tx;
      entry.event_sequence = first_event_sequence + index;
      entry.sequence = entry.event_sequence;
      entry.table_uuid = table_uuid;
      entry.index_uuid = index_uuid;
      entry.column_name = column_name;
      entry.family = family;
      entry.entry_kind = entry_kind;
      if (!ReadBinaryString(bytes, &offset, &entry.key_value) ||
          !ReadBinaryString(bytes, &offset, &entry.payload_value) ||
          !ReadBinaryString(bytes, &offset, &entry.row_uuid) ||
          !ReadBinaryString(bytes, &offset, &entry.version_uuid) ||
          entry.key_value.empty() || entry.row_uuid.empty() ||
          entry.version_uuid.empty()) {
        return false;
      }
      entries->push_back(std::move(entry));
    }
  }
  return true;
}

std::string JoinLine(const std::vector<std::string>& fields) {
  std::string line;
  for (std::size_t i = 0; i < fields.size(); ++i) {
    if (i != 0) { line += '\t'; }
    line += fields[i];
  }
  return line;
}

std::string BuildIndexEntryStoreLine(std::uint64_t creator_tx,
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
  const std::string creator_text = std::to_string(creator_tx);
  const std::string event_text = std::to_string(event_sequence);
  std::string line;
  line.reserve(96 + creator_text.size() + event_text.size() +
               index_uuid.size() + table_uuid.size() + column_name.size() +
               family.size() + entry_kind.size() +
               kLineHexFieldPrefix.size() + key.size() * 2 +
               kLineHexFieldPrefix.size() + payload.size() * 2 +
               row_uuid.size() + version_uuid.size());
  bool first = true;
  AppendLineField(&line, &first, kRowStoreMagic);
  AppendLineField(&line, &first, "INDEX_ENTRY");
  AppendLineField(&line, &first, creator_text);
  AppendLineField(&line, &first, event_text);
  AppendLineField(&line, &first, index_uuid);
  AppendLineField(&line, &first, table_uuid);
  AppendLineField(&line, &first, column_name);
  AppendLineField(&line, &first, family);
  AppendLineField(&line, &first, entry_kind);
  AppendLineSafeOrHexField(&line, &first, key);
  AppendLineSafeOrHexField(&line, &first, payload);
  AppendLineField(&line, &first, row_uuid);
  AppendLineField(&line, &first, version_uuid);
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

ScopedRelationSummary RebuildScopedRelationSummaryFromRows(
    const std::string& scoped_row_path) {
  ScopedRelationSummary summary;
  if (!FileExistsAndNotEmpty(scoped_row_path)) {
    summary.trusted = true;
    return summary;
  }
  for (const auto& line : ReadLines(scoped_row_path)) {
    const auto fields = SplitTabs(line);
    if (fields.size() < 11 || fields[0] != kRowStoreMagic ||
        fields[1] != "ROW_VERSION") {
      summary.malformed = true;
      summary.trusted = false;
      return summary;
    }
    ++summary.row_version_count;
    if (fields[7] == "1") {
      ++summary.tombstone_count;
    }
    if (!fields[8].empty()) {
      ++summary.update_count;
    }
  }
  summary.trusted = true;
  return summary;
}

void MergeScopedRelationSummary(ScopedRelationSummary* target,
                                const ScopedRelationSummary& source) {
  if (target == nullptr) { return; }
  if (source.malformed || !source.trusted) {
    target->malformed = true;
    target->trusted = false;
    return;
  }
  target->row_version_count += source.row_version_count;
  target->tombstone_count += source.tombstone_count;
  target->update_count += source.update_count;
  target->trusted = true;
}

ScopedRelationSummary RebuildScopedRelationSummaryFromStores(
    const EngineRequestContext& context,
    const std::string& table_uuid) {
  ScopedRelationSummary summary;
  summary.trusted = true;
  MergeScopedRelationSummary(
      &summary,
      RebuildScopedRelationSummaryFromRows(ScopedRowStorePath(context,
                                                              table_uuid)));
  ScopedRelationSummary binary_summary;
  std::vector<CrudRowVersionRecord> ignored_rows;
  if (!DecodeScopedRowBinaryStore(ScopedRowBinaryStorePath(context,
                                                          table_uuid),
                                  &ignored_rows,
                                  &binary_summary)) {
    binary_summary.malformed = true;
    binary_summary.trusted = false;
  }
  MergeScopedRelationSummary(&summary, binary_summary);
  return summary;
}

bool ScopedRelationAnyRowStoreExists(const EngineRequestContext& context,
                                     const std::string& table_uuid) {
  return FileExistsAndNotEmpty(ScopedRowStorePath(context, table_uuid)) ||
         FileExistsAndNotEmpty(ScopedRowBinaryStorePath(context, table_uuid));
}

ScopedRelationSummary LoadScopedRelationSummary(
    const EngineRequestContext& context,
    const std::string& table_uuid) {
  const std::string summary_path = ScopedSummaryStorePath(context, table_uuid);
  const std::string row_path = ScopedRowStorePath(context, table_uuid);
  const std::string binary_row_path = ScopedRowBinaryStorePath(context,
                                                              table_uuid);
  if (!FileExistsAndNotEmpty(summary_path)) {
    if (!FileExistsAndNotEmpty(row_path) &&
        !FileExistsAndNotEmpty(binary_row_path)) {
      ScopedRelationSummary empty;
      empty.trusted = true;
      return empty;
    }
    return RebuildScopedRelationSummaryFromStores(context, table_uuid);
  }

  ScopedRelationSummary summary;
  for (const auto& line : ReadLines(summary_path)) {
    const auto fields = SplitTabs(line);
    if (fields.size() < 8 || fields[0] != "SBMGASUM1" ||
        fields[1] != "RELATION_SCOPE") {
      summary.malformed = true;
      summary.trusted = false;
      return summary;
    }
    for (std::size_t index = 2; index + 1 < fields.size(); index += 2) {
      if (fields[index] == "row_versions") {
        summary.row_version_count = ParseU64(fields[index + 1]);
      } else if (fields[index] == "tombstones") {
        summary.tombstone_count = ParseU64(fields[index + 1]);
      } else if (fields[index] == "updates") {
        summary.update_count = ParseU64(fields[index + 1]);
      }
    }
    summary.trusted = true;
  }
  if (!summary.trusted &&
      (FileExistsAndNotEmpty(row_path) ||
       FileExistsAndNotEmpty(binary_row_path))) {
    return RebuildScopedRelationSummaryFromStores(context, table_uuid);
  }
  summary.trusted = true;
  return summary;
}

bool WriteScopedRelationSummary(const EngineRequestContext& context,
                                const std::string& table_uuid,
                                const ScopedRelationSummary& summary) {
  std::error_code ignored;
  std::filesystem::create_directories(ScopedRelationStoreRoot(context),
                                      ignored);
  const std::string path = ScopedSummaryStorePath(context, table_uuid);
  const std::string tmp_path = path + ".tmp." +
      std::to_string(context.local_transaction_id) + "." +
      std::to_string(summary.row_version_count);
  {
    std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
    if (!out) { return false; }
    out << JoinLine({"SBMGASUM1",
                     "RELATION_SCOPE",
                     "row_versions",
                     std::to_string(summary.row_version_count),
                     "tombstones",
                     std::to_string(summary.tombstone_count),
                     "updates",
                     std::to_string(summary.update_count)})
        << '\n';
    out.flush();
    if (!out) { return false; }
  }
  std::error_code rename_error;
  std::filesystem::rename(tmp_path, path, rename_error);
  if (rename_error) {
    std::error_code remove_error;
    std::filesystem::remove(path, remove_error);
    std::filesystem::rename(tmp_path, path, rename_error);
  }
  return !rename_error;
}

bool UpdateScopedRelationSummaries(
    const EngineRequestContext& context,
    const std::map<std::string, ScopedRelationSummaryDelta>& deltas) {
  if (deltas.empty()) {
    return true;
  }
  const std::lock_guard<std::mutex> guard(ScopedRelationSummaryMutex());
  for (const auto& [table_uuid, delta] : deltas) {
    if (table_uuid.empty() || delta.row_version_count == 0) {
      continue;
    }
    const std::string summary_path = ScopedSummaryStorePath(context,
                                                            table_uuid);
    if (FileExistsAndNotEmpty(summary_path)) {
      ScopedRelationSummary summary = LoadScopedRelationSummary(context,
                                                                table_uuid);
      if (summary.malformed) {
        return false;
      }
      summary.row_version_count += delta.row_version_count;
      summary.tombstone_count += delta.tombstone_count;
      summary.update_count += delta.update_count;
      summary.trusted = true;
      if (!WriteScopedRelationSummary(context, table_uuid, summary)) {
        return false;
      }
    } else if (delta.first_scoped_write) {
      ScopedRelationSummary summary;
      summary.row_version_count = delta.row_version_count;
      summary.tombstone_count = delta.tombstone_count;
      summary.update_count = delta.update_count;
      summary.trusted = true;
      if (!WriteScopedRelationSummary(context, table_uuid, summary)) {
        return false;
      }
    } else {
      ScopedRelationSummary summary =
          RebuildScopedRelationSummaryFromStores(context, table_uuid);
      if (!summary.trusted || summary.malformed) {
        return false;
      }
      if (!WriteScopedRelationSummary(context, table_uuid, summary)) {
        return false;
      }
    }
  }
  return true;
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

std::string DecodeLineHexFieldOrRaw(const std::string& field) {
  if (field.rfind(kLineHexFieldPrefix, 0) != 0) {
    return field;
  }
  const std::string encoded =
      field.substr(kLineHexFieldPrefix.size());
  return DecodeCrudTextLocal(encoded);
}

bool LoadScopedBinaryIndexEntriesForTables(
    const EngineRequestContext& context,
    const std::set<std::string>& table_uuids,
    std::vector<CrudIndexEntryRecord>* entries,
    bool* used_segment) {
  if (entries == nullptr) {
    return false;
  }
  if (used_segment != nullptr) {
    *used_segment = false;
  }
  for (const auto& table_uuid : table_uuids) {
    const std::string path = ScopedIndexBinaryStorePath(context, table_uuid);
    if (!FileExistsAndNotEmpty(path)) {
      continue;
    }
    if (used_segment != nullptr) {
      *used_segment = true;
    }
    if (!DecodeScopedIndexBinaryStore(path, entries)) {
      return false;
    }
  }
  return true;
}

std::vector<std::pair<std::string, std::string>> DecodeCrudPairsWithKeyCache(
    const std::string& encoded,
    std::unordered_map<std::string, std::string>* decoded_key_cache) {
  std::vector<std::pair<std::string, std::string>> pairs;
  pairs.reserve(8);
  std::size_t start = 0;
  while (start <= encoded.size()) {
    const auto pipe = encoded.find('|', start);
    const std::size_t end = pipe == std::string::npos ? encoded.size() : pipe;
    const auto equals = encoded.find('=', start);
    if (equals != std::string::npos && equals < end) {
      const std::string encoded_key = encoded.substr(start, equals - start);
      std::string key;
      if (decoded_key_cache == nullptr) {
        key = DecodeCrudTextLocal(encoded_key);
      } else {
        auto [found, inserted] = decoded_key_cache->try_emplace(encoded_key);
        if (inserted) { found->second = DecodeCrudTextLocal(encoded_key); }
        key = found->second;
      }
      pairs.push_back({std::move(key),
                       DecodeCrudTextLocal(encoded.substr(equals + 1,
                                                          end - equals - 1))});
    }
    if (pipe == std::string::npos) { break; }
    start = pipe + 1;
  }
  return pairs;
}

struct ExistingStoreFileIdentity {
  std::uintmax_t file_size = 0;
  std::int64_t file_mtime_ticks = 0;
  bool ok = false;
};

ExistingStoreFileIdentity ExistingFileIdentity(const std::string& path) {
  ExistingStoreFileIdentity identity;
  std::error_code ignored;
  if (path.empty() || !std::filesystem::exists(path, ignored)) {
    return identity;
  }
  identity.file_size = std::filesystem::file_size(path, ignored);
  if (ignored || identity.file_size == static_cast<std::uintmax_t>(-1)) {
    return {};
  }
  ignored.clear();
  const auto mtime = std::filesystem::last_write_time(path, ignored);
  if (ignored) { return {}; }
  identity.file_mtime_ticks =
      static_cast<std::int64_t>(mtime.time_since_epoch().count());
  identity.ok = true;
  return identity;
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

struct StatementTransactionInventoryAuthority {
  std::shared_ptr<const LocalTransactionInventorySnapshot> snapshot;
  EngineApiDiagnostic diagnostic;

  bool ok() const { return snapshot != nullptr && !diagnostic.error; }
};

StatementTransactionInventoryAuthority ResolveStatementTransactionInventory(
    const EngineRequestContext& context) {
  StatementTransactionInventoryAuthority result;
  if (context.statement_transaction_inventory_snapshot != nullptr) {
    const auto& retained = *context.statement_transaction_inventory_snapshot;
    if (retained.database_path != context.database_path ||
        retained.inventory_root_body_sha256.empty() ||
        (retained.publish_journal_present &&
         retained.publish_journal_sha256.empty())) {
      result.diagnostic = MakeInvalidRequestDiagnostic(
          "mga.transaction_authority",
          "statement_transaction_inventory_snapshot_stale");
      return result;
    }
    result.snapshot = context.statement_transaction_inventory_snapshot;
    result.diagnostic = OkDiagnostic();
    return result;
  }
  const auto acquired =
      AcquireLocalTransactionInventorySnapshot(context.database_path);
  if (!acquired.ok()) {
    result.diagnostic = MakeEngineApiDiagnostic(
        acquired.diagnostic.diagnostic_code.empty()
            ? "SB-MGA-TXN-INV-LOAD-FAILED"
            : acquired.diagnostic.diagnostic_code,
        acquired.diagnostic.message_key.empty()
            ? "mga.transaction_inventory.load_failed"
            : acquired.diagnostic.message_key,
        acquired.diagnostic.remediation_hint, true);
    return result;
  }
  result.snapshot = acquired.snapshot;
  result.diagnostic = OkDiagnostic();
  return result;
}

EngineApiDiagnostic OverlayMgaTransactionAuthority(
    const EngineRequestContext& context,
    RelationReadSnapshot* state,
    bool allow_read_only_active) {
  if (state == nullptr) {
    return MakeInvalidRequestDiagnostic("mga.transaction_authority", "state_required");
  }
  if (context.database_path.empty()) {
    return MakeInvalidRequestDiagnostic("mga.transaction_authority", "database_path_required");
  }
  const auto loaded = ResolveStatementTransactionInventory(context);
  if (!loaded.ok()) return loaded.diagnostic;
  for (const auto& entry : loaded.snapshot->inventory.entries) {
    if (!entry.identity.local_id.valid()) { continue; }
    state->transactions[entry.identity.local_id.value] = MgaTransactionStateName(entry.state);
    state->max_transaction_id = std::max(state->max_transaction_id, entry.identity.local_id.value);
  }
  if (context.local_transaction_id != 0) {
    const auto lookup = LookupLocalTransaction(
        loaded.snapshot->inventory,
        MakeLocalTransactionId(context.local_transaction_id));
    if (!lookup.ok()) {
      return MakeEngineApiDiagnostic(lookup.diagnostic.diagnostic_code.empty() ? "SB-MGA-TXN-INV-LOOKUP-FAILED" : lookup.diagnostic.diagnostic_code,
                                     lookup.diagnostic.message_key.empty() ? "mga.transaction_inventory.lookup_failed" : lookup.diagnostic.message_key,
                                     lookup.diagnostic.remediation_hint,
                                     true);
    }
    if (lookup.entry.state != TransactionState::active &&
        !(allow_read_only_active &&
          lookup.entry.state == TransactionState::read_only_active)) {
      return MakeInvalidRequestDiagnostic("mga.transaction_authority", "active_local_transaction_required");
    }
  }
  return OkDiagnostic();
}

bool ExactTextMigrationCreatorTransaction(
    const EngineRequestContext& context,
    const std::uint64_t creator_tx,
    const std::string_view transaction_uuid) {
  if (creator_tx == 0 || transaction_uuid.empty()) return false;
  const auto parsed = scratchbird::core::uuid::ParseTypedUuid(
      scratchbird::core::platform::UuidKind::transaction,
      std::string(transaction_uuid));
  if (!parsed.ok()) return false;
  const auto inventory = ResolveStatementTransactionInventory(context);
  if (!inventory.ok()) return false;
  const auto exact = LookupLocalTransaction(
      inventory.snapshot->inventory, MakeLocalTransactionId(creator_tx));
  return exact.ok() &&
         exact.entry.identity.transaction_uuid.value == parsed.value.value;
}

bool TextMigrationLineageCreatorVisible(
    const EngineRequestContext& context,
    const std::uint64_t migration_creator_tx,
    const std::uint64_t candidate_creator_tx) {
  if (candidate_creator_tx == 0) return true;
  const auto inventory = ResolveStatementTransactionInventory(context);
  if (!inventory.ok()) return false;
  const auto exact = LookupLocalTransaction(
      inventory.snapshot->inventory,
      MakeLocalTransactionId(candidate_creator_tx));
  if (!exact.ok()) return false;
  if (exact.entry.state == TransactionState::committed ||
      exact.entry.state == TransactionState::archived) {
    return true;
  }
  return candidate_creator_tx == migration_creator_tx &&
         (exact.entry.state == TransactionState::active ||
          exact.entry.state == TransactionState::prepared);
}

std::set<std::string> VisibleRetiredTemporaryTableMetadata(
    const EngineRequestContext& context,
    const RelationReadSnapshot& state) {
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
                                           RelationReadSnapshot* state) {
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
  const auto authority =
      OverlayMgaTransactionAuthority(context, &state, false);
  if (authority.error) { return authority; }
  return OkDiagnostic();
}

EngineApiDiagnostic ValidateMgaRowVersionRecordChains(
    const std::vector<CrudRowVersionRecord>& row_versions) {
  std::unordered_map<std::string, const CrudRowVersionRecord*> by_version_uuid;
  by_version_uuid.reserve(row_versions.size());
  for (const auto& row : row_versions) {
    if (row.version_uuid.empty()) {
      return MakeInvalidRequestDiagnostic("mga.row_version_chain", "row_version_uuid_required");
    }
    const auto inserted = by_version_uuid.emplace(row.version_uuid, &row);
    if (!inserted.second) {
      return MakeInvalidRequestDiagnostic("mga.row_version_chain", "duplicate_row_version_uuid");
    }
  }
  for (const auto& row : row_versions) {
    if (row.previous_version_uuid.empty()) { continue; }
    const auto previous = by_version_uuid.find(row.previous_version_uuid);
    if (previous == by_version_uuid.end()) {
      return MakeInvalidRequestDiagnostic("mga.row_version_chain", "previous_row_version_missing");
    }
    const auto* previous_row = previous->second;
    if (previous_row->table_uuid != row.table_uuid ||
        previous_row->row_uuid != row.row_uuid) {
      return MakeInvalidRequestDiagnostic("mga.row_version_chain", "previous_row_version_wrong_chain");
    }
    if (previous_row->sequence >= row.sequence) {
      return MakeInvalidRequestDiagnostic("mga.row_version_chain", "previous_row_version_not_older");
    }
    if (row.previous_sequence != 0 &&
        previous_row->sequence != row.previous_sequence) {
      return MakeInvalidRequestDiagnostic("mga.row_version_chain", "previous_row_version_sequence_mismatch");
    }
  }
  return OkDiagnostic();
}

EngineApiDiagnostic ValidateMgaRowVersionChains(const RelationReadSnapshot& state) {
  return ValidateMgaRowVersionRecordChains(state.row_versions);
}

void FilterMgaTemporaryObjectsForSession(
    const EngineRequestContext& context,
    RelationReadSnapshot* state) {
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

struct MgaStatementMetadataViewKey {
  MgaMetadataCacheKey metadata;
  std::string database_uuid;
  std::string session_uuid;
  std::string transaction_uuid;
  std::uint64_t local_transaction_id{0};
  std::uint64_t snapshot_visible_through_local_transaction_id{0};
  std::uint64_t catalog_generation{0};
  std::uint64_t security_epoch{0};
  std::uint64_t policy_epoch{0};
  std::uint64_t resource_epoch{0};
  std::string descriptor_path;
  std::uint64_t descriptor_file_size{0};
  std::int64_t descriptor_file_mtime_ticks{0};
  std::uint64_t inventory_file_size{0};
  std::int64_t inventory_file_mtime_ticks{0};
  bool publish_journal_present{false};
  std::uint64_t publish_journal_size{0};
  std::int64_t publish_journal_mtime_ticks{0};

  bool operator<(const MgaStatementMetadataViewKey& other) const {
    return std::tie(metadata, database_uuid, session_uuid, transaction_uuid,
                    local_transaction_id,
                    snapshot_visible_through_local_transaction_id,
                    catalog_generation, security_epoch, policy_epoch,
                    resource_epoch, descriptor_path, descriptor_file_size,
                    descriptor_file_mtime_ticks, inventory_file_size,
                    inventory_file_mtime_ticks, publish_journal_present,
                    publish_journal_size, publish_journal_mtime_ticks) <
           std::tie(other.metadata, other.database_uuid, other.session_uuid,
                    other.transaction_uuid, other.local_transaction_id,
                    other.snapshot_visible_through_local_transaction_id,
                    other.catalog_generation, other.security_epoch,
                    other.policy_epoch, other.resource_epoch,
                    other.descriptor_path, other.descriptor_file_size,
                    other.descriptor_file_mtime_ticks,
                    other.inventory_file_size,
                    other.inventory_file_mtime_ticks,
                    other.publish_journal_present,
                    other.publish_journal_size,
                    other.publish_journal_mtime_ticks);
  }
};

struct MgaStatementMetadataView {
  std::shared_ptr<const MgaMetadataCacheEntry> source_snapshot;
  std::shared_ptr<const LocalTransactionInventorySnapshot>
      transaction_inventory_snapshot;
  std::shared_ptr<const DescriptorFieldsByRelation> descriptor_fields;
  CrudState visible_metadata;
  std::unordered_map<std::string, std::size_t> visible_table_ordinals;
  std::unordered_map<std::string, std::vector<CrudIndexRecord>>
      visible_indexes_by_relation;
};

struct MgaStatementMetadataViewLoadResult {
  EngineApiDiagnostic diagnostic;
  std::shared_ptr<const MgaStatementMetadataView> view;
  MgaStatementMetadataViewKey key;

  bool ok() const { return view != nullptr && !diagnostic.error; }
};

std::mutex& MgaStatementMetadataViewCacheMutex() {
  static std::mutex mutex;
  return mutex;
}

std::map<MgaStatementMetadataViewKey,
         std::shared_ptr<const MgaStatementMetadataView>>&
MgaStatementMetadataViewCache() {
  static std::map<MgaStatementMetadataViewKey,
                  std::shared_ptr<const MgaStatementMetadataView>> cache;
  return cache;
}

MgaStatementMetadataViewLoadResult LoadMgaStatementMetadataView(
    const EngineRequestContext& context,
    const std::span<const std::string> required_relation_uuids = {}) {
  MgaStatementMetadataViewLoadResult result;
  EngineRequestContext statement_context = context;
  if (statement_context.statement_transaction_inventory_snapshot == nullptr) {
    auto acquired = AcquireLocalTransactionInventorySnapshot(
        statement_context.database_path);
    if (!acquired.ok()) {
      result.diagnostic = MakeEngineApiDiagnostic(
          acquired.diagnostic.diagnostic_code.empty()
              ? "SB-MGA-TXN-INV-LOAD-FAILED"
              : acquired.diagnostic.diagnostic_code,
          acquired.diagnostic.message_key.empty()
              ? "mga.transaction_inventory.load_failed"
              : acquired.diagnostic.message_key,
          acquired.diagnostic.remediation_hint, true);
      return result;
    }
    statement_context.statement_transaction_inventory_snapshot =
        std::move(acquired.snapshot);
  }
  const auto metadata = LoadMgaMetadataSnapshot(statement_context);
  if (!metadata.ok()) {
    result.diagnostic = metadata.diagnostic;
    return result;
  }
  const auto& inventory =
      *statement_context.statement_transaction_inventory_snapshot;
  const auto descriptor_path = DescriptorStorePath(statement_context);
  const auto descriptor_identity = ExistingFileIdentity(descriptor_path);
  result.key = MgaStatementMetadataViewKey{
      metadata.key,
      context.database_uuid.canonical,
      context.session_uuid.canonical,
      context.transaction_uuid.canonical,
      context.local_transaction_id,
      context.snapshot_visible_through_local_transaction_id,
      context.catalog_generation_id,
      context.authorization_context.security_epoch != 0
          ? context.authorization_context.security_epoch
          : context.security_epoch,
      context.authorization_context.policy_epoch,
      context.resource_epoch,
      descriptor_path,
      descriptor_identity.ok ? descriptor_identity.file_size : 0,
      descriptor_identity.ok ? descriptor_identity.file_mtime_ticks : 0,
      inventory.database_file_size,
      inventory.database_write_time_count,
      inventory.publish_journal_present,
      inventory.publish_journal_size,
      inventory.publish_journal_write_time_count};
  {
    const std::lock_guard<std::mutex> guard(
        MgaStatementMetadataViewCacheMutex());
    const auto cached = MgaStatementMetadataViewCache().find(result.key);
    if (cached != MgaStatementMetadataViewCache().end() &&
        cached->second != nullptr &&
        cached->second->descriptor_fields != nullptr &&
        std::ranges::all_of(required_relation_uuids,
                            [&cached](const std::string& relation_uuid) {
          return cached->second->descriptor_fields->contains(relation_uuid);
        })) {
      result.view = cached->second;
      result.diagnostic = OkDiagnostic();
      return result;
    }
  }

  auto view = std::make_shared<MgaStatementMetadataView>();
  view->source_snapshot = metadata.snapshot;
  view->transaction_inventory_snapshot =
      statement_context.statement_transaction_inventory_snapshot;
  view->visible_metadata.tables = metadata.snapshot->tables;
  view->visible_metadata.indexes = metadata.snapshot->indexes;
  view->visible_metadata.sealed_relation_descriptor_snapshots =
      metadata.snapshot->sealed_relation_descriptor_snapshots;
  view->visible_metadata.max_event_sequence =
      metadata.snapshot->max_event_sequence;
  const auto transaction_authority = OverlayMgaTransactionAuthority(
      statement_context, &view->visible_metadata, true);
  if (transaction_authority.error) {
    result.diagnostic = transaction_authority;
    return result;
  }
  FilterVisibleRetiredTemporaryMetadata(statement_context,
                                        &view->visible_metadata);
  FilterMgaTemporaryObjectsForSession(statement_context,
                                      &view->visible_metadata);
  view->descriptor_fields = LoadDescriptorFieldsSnapshot(statement_context);
  const auto missing_required = std::ranges::find_if(
      required_relation_uuids, [&view](const std::string& relation_uuid) {
        return view->descriptor_fields == nullptr ||
               !view->descriptor_fields->contains(relation_uuid);
      });
  if (missing_required != required_relation_uuids.end()) {
    // A negative snapshot can have the same coarse size/mtime identity as a
    // repaired durable store. Exact relation lookup must force one reread at
    // that boundary; a genuine durable miss remains visible to the caller.
    view->descriptor_fields =
        LoadDescriptorFieldsSnapshot(statement_context, *missing_required);
  }
  if (view->descriptor_fields == nullptr) {
    result.diagnostic = MakeInvalidRequestDiagnostic(
        "mga.relation_metadata", "descriptor_snapshot_unavailable");
    return result;
  }
  view->visible_table_ordinals.reserve(view->visible_metadata.tables.size());
  for (std::size_t ordinal = 0;
       ordinal < view->visible_metadata.tables.size(); ++ordinal) {
    const auto& table = view->visible_metadata.tables[ordinal];
    if (!CrudCreatorVisible(view->visible_metadata, table.creator_tx,
                            table.event_sequence,
                            statement_context.local_transaction_id)) {
      continue;
    }
    const auto found =
        view->visible_table_ordinals.find(table.table_uuid);
    if (found == view->visible_table_ordinals.end() ||
        table.creator_tx >=
            view->visible_metadata.tables[found->second].creator_tx) {
      view->visible_table_ordinals[table.table_uuid] = ordinal;
    }
  }
  view->visible_indexes_by_relation.reserve(
      view->visible_metadata.indexes.size());
  for (const auto& index : view->visible_metadata.indexes) {
    if (CrudCreatorVisible(view->visible_metadata, index.creator_tx,
                           index.event_sequence,
                           statement_context.local_transaction_id)) {
      view->visible_indexes_by_relation[index.table_uuid].push_back(index);
    }
  }
  result.view = view;
  result.diagnostic = OkDiagnostic();
  {
    const std::lock_guard<std::mutex> guard(
        MgaStatementMetadataViewCacheMutex());
    auto& cache = MgaStatementMetadataViewCache();
    cache[result.key] = std::move(view);
    constexpr std::size_t kMaximumStatementMetadataViews = 128;
    while (cache.size() > kMaximumStatementMetadataViews) {
      cache.erase(cache.begin());
    }
  }
  return result;
}

void RetainRelationMetadataScope(const std::set<std::string>& table_scope,
                                 RelationReadSnapshot* metadata) {
  if (metadata == nullptr) { return; }
  metadata->tables.erase(
      std::remove_if(metadata->tables.begin(), metadata->tables.end(),
                     [&table_scope](const CrudTableRecord& table) {
                       return table_scope.count(table.table_uuid) == 0;
                     }),
      metadata->tables.end());
  metadata->indexes.erase(
      std::remove_if(metadata->indexes.begin(), metadata->indexes.end(),
                     [&table_scope](const CrudIndexRecord& index) {
                       return table_scope.count(index.table_uuid) == 0;
                     }),
      metadata->indexes.end());
  metadata->large_values.erase(
      std::remove_if(metadata->large_values.begin(),
                     metadata->large_values.end(),
                     [&table_scope](const CrudLargeValueRecord& value) {
                       return table_scope.count(value.table_uuid) == 0;
                     }),
      metadata->large_values.end());
  metadata->sealed_relation_descriptor_snapshots.erase(
      std::remove_if(
          metadata->sealed_relation_descriptor_snapshots.begin(),
          metadata->sealed_relation_descriptor_snapshots.end(),
          [&table_scope](
              const CrudSealedRelationDescriptorSnapshot& snapshot) {
            return table_scope.count(snapshot.relation_uuid) == 0;
          }),
      metadata->sealed_relation_descriptor_snapshots.end());
}

void AddMaterializedString(const std::string& value,
                           std::uint64_t* bytes,
                           std::uint64_t* allocation_units) {
  if (bytes == nullptr || allocation_units == nullptr) { return; }
  *bytes += static_cast<std::uint64_t>(value.size());
  if (!value.empty()) { ++(*allocation_units); }
}

void AddMaterializedPairs(
    const std::vector<std::pair<std::string, std::string>>& values,
    std::uint64_t* bytes,
    std::uint64_t* allocation_units) {
  if (values.empty()) { return; }
  ++(*allocation_units);
  *bytes += static_cast<std::uint64_t>(
      values.size() * sizeof(std::pair<std::string, std::string>));
  for (const auto& [key, value] : values) {
    AddMaterializedString(key, bytes, allocation_units);
    AddMaterializedString(value, bytes, allocation_units);
  }
}

void CaptureRelationLoadMaterialization(MgaRelationStoreResult* result) {
  if (result == nullptr) { return; }
  const auto& state = result->state;
  auto& bytes = result->bytes_materialized;
  auto& allocations = result->allocation_units_materialized;
  bytes = sizeof(MgaRelationStoreState);
  allocations = 0;

  result->metadata_records_materialized =
      static_cast<std::uint64_t>(state.relation_metadata.tables.size() +
                                 state.relation_metadata.indexes.size() +
                                 state.relation_metadata.transactions.size() +
                                 state.relation_metadata.large_values.size() +
                                 state.relation_metadata
                                     .sealed_relation_descriptor_snapshots
                                     .size());
  result->rows_materialized =
      static_cast<std::uint64_t>(state.row_versions.size());

  if (!state.relation_metadata.transactions.empty()) { ++allocations; }
  for (const auto& [transaction_id, status] :
       state.relation_metadata.transactions) {
    (void)transaction_id;
    bytes += sizeof(transaction_id) + sizeof(status);
    ++allocations;
    AddMaterializedString(status, &bytes, &allocations);
  }
  if (!state.relation_metadata.tables.empty()) { ++allocations; }
  bytes += static_cast<std::uint64_t>(
      state.relation_metadata.tables.size() * sizeof(CrudTableRecord));
  for (const auto& table : state.relation_metadata.tables) {
    AddMaterializedString(table.table_uuid, &bytes, &allocations);
    AddMaterializedString(table.default_name, &bytes, &allocations);
    AddMaterializedPairs(table.columns, &bytes, &allocations);
    AddMaterializedString(table.temporary_scope, &bytes, &allocations);
    AddMaterializedString(table.temporary_session_uuid, &bytes, &allocations);
    AddMaterializedString(table.on_commit_action, &bytes, &allocations);
  }
  if (!state.row_versions.empty()) { ++allocations; }
  bytes += static_cast<std::uint64_t>(
      state.row_versions.size() * sizeof(CrudRowVersionRecord));
  for (const auto& row : state.row_versions) {
    AddMaterializedString(row.table_uuid, &bytes, &allocations);
    AddMaterializedString(row.row_uuid, &bytes, &allocations);
    AddMaterializedString(row.version_uuid, &bytes, &allocations);
    AddMaterializedString(row.temporary_session_uuid, &bytes, &allocations);
    AddMaterializedString(row.previous_version_uuid, &bytes, &allocations);
    AddMaterializedPairs(row.values, &bytes, &allocations);
  }
  if (!state.relation_metadata.indexes.empty()) { ++allocations; }
  bytes += static_cast<std::uint64_t>(
      state.relation_metadata.indexes.size() * sizeof(CrudIndexRecord));
  for (const auto& index : state.relation_metadata.indexes) {
    AddMaterializedString(index.index_uuid, &bytes, &allocations);
    AddMaterializedString(index.table_uuid, &bytes, &allocations);
    AddMaterializedString(index.column_name, &bytes, &allocations);
    AddMaterializedString(index.family, &bytes, &allocations);
    AddMaterializedString(index.profile, &bytes, &allocations);
    AddMaterializedString(index.default_name, &bytes, &allocations);
    if (!index.key_envelopes.empty()) { ++allocations; }
    for (const auto& key : index.key_envelopes) {
      AddMaterializedString(key, &bytes, &allocations);
    }
    if (!index.include_columns.empty()) { ++allocations; }
    for (const auto& column : index.include_columns) {
      AddMaterializedString(column, &bytes, &allocations);
    }
    AddMaterializedString(index.predicate_kind, &bytes, &allocations);
    AddMaterializedString(index.predicate_column, &bytes, &allocations);
    AddMaterializedString(index.predicate_value, &bytes, &allocations);
  }
  if (!state.index_entries.empty()) { ++allocations; }
  bytes += static_cast<std::uint64_t>(
      state.index_entries.size() * sizeof(CrudIndexEntryRecord));
  for (const auto& entry : state.index_entries) {
    AddMaterializedString(entry.index_uuid, &bytes, &allocations);
    AddMaterializedString(entry.table_uuid, &bytes, &allocations);
    AddMaterializedString(entry.column_name, &bytes, &allocations);
    AddMaterializedString(entry.family, &bytes, &allocations);
    AddMaterializedString(entry.entry_kind, &bytes, &allocations);
    AddMaterializedString(entry.key_value, &bytes, &allocations);
    AddMaterializedString(entry.payload_value, &bytes, &allocations);
    AddMaterializedString(entry.row_uuid, &bytes, &allocations);
    AddMaterializedString(entry.version_uuid, &bytes, &allocations);
  }
  if (!state.relation_metadata.large_values.empty()) { ++allocations; }
  bytes += static_cast<std::uint64_t>(
      state.relation_metadata.large_values.size() *
      sizeof(CrudLargeValueRecord));
  for (const auto& value : state.relation_metadata.large_values) {
    AddMaterializedString(value.overflow_uuid, &bytes, &allocations);
    AddMaterializedString(value.table_uuid, &bytes, &allocations);
    AddMaterializedString(value.row_uuid, &bytes, &allocations);
    AddMaterializedString(value.version_uuid, &bytes, &allocations);
    AddMaterializedString(value.field_name, &bytes, &allocations);
    AddMaterializedString(value.content_hash, &bytes, &allocations);
    AddMaterializedString(value.state, &bytes, &allocations);
    if (!value.chunks.empty()) { ++allocations; }
    bytes += static_cast<std::uint64_t>(
        value.chunks.size() * sizeof(CrudLargeValueChunkRecord));
    for (const auto& chunk : value.chunks) {
      AddMaterializedString(chunk.overflow_uuid, &bytes, &allocations);
      AddMaterializedString(chunk.payload_fragment, &bytes, &allocations);
    }
  }
  if (!state.relation_metadata.sealed_relation_descriptor_snapshots.empty()) {
    ++allocations;
  }
  bytes += static_cast<std::uint64_t>(
      state.relation_metadata.sealed_relation_descriptor_snapshots.size() *
      sizeof(CrudSealedRelationDescriptorSnapshot));
  for (const auto& snapshot :
       state.relation_metadata.sealed_relation_descriptor_snapshots) {
    AddMaterializedString(snapshot.relation_uuid, &bytes, &allocations);
    AddMaterializedString(snapshot.relation_descriptor_uuid, &bytes,
                          &allocations);
    AddMaterializedPairs(snapshot.descriptor_fields, &bytes, &allocations);
  }
  if (!state.relation_metadata.savepoints.empty()) { ++allocations; }
  for (const auto& [transaction_id, transaction_savepoints] :
       state.relation_metadata.savepoints) {
    (void)transaction_id;
    bytes += sizeof(transaction_id) + sizeof(transaction_savepoints);
    ++allocations;
    for (const auto& [name, sequence] : transaction_savepoints) {
      bytes += sizeof(name) + sizeof(sequence);
      ++allocations;
      AddMaterializedString(name, &bytes, &allocations);
    }
  }
}

void AddRelationLoadEvidence(MgaRelationStoreResult* result,
                             const std::string& route) {
  if (result == nullptr) { return; }
  CaptureRelationLoadMaterialization(result);
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
  result->evidence.push_back({"mga_relation_state_metadata_records_materialized",
                              std::to_string(result->metadata_records_materialized)});
  result->evidence.push_back({"mga_relation_state_rows_materialized",
                              std::to_string(result->rows_materialized)});
  result->evidence.push_back({"mga_relation_state_bytes_materialized",
                              std::to_string(result->bytes_materialized)});
  result->evidence.push_back({"mga_relation_state_allocation_units_materialized",
                              std::to_string(
                                  result->allocation_units_materialized)});
  result->evidence.push_back({"mga_relation_state_scoped_physical_segments",
                              result->scoped_physical_segments_used ? "true" : "false"});
  result->evidence.push_back({"mga_relation_state_scoped_physical_fallback",
                              result->scoped_physical_segments_fallback ? "true" : "false"});
}

}  // namespace

EngineApiDiagnostic OverlayMgaTransactionAuthorityForStoreModule(
    const EngineRequestContext& context,
    RelationReadSnapshot* state,
    bool allow_read_only_active) {
  return OverlayMgaTransactionAuthority(context, state,
                                        allow_read_only_active);
}

EngineApiDiagnostic ValidateMgaMutatingTransactionAuthorityForStoreModule(
    const EngineRequestContext& context,
    const std::string& operation_id) {
  return ValidateMgaMutatingTransactionAuthority(context, operation_id);
}

std::function<bool(std::uint64_t, std::uint64_t)>
MakeMgaMetadataRollbackPredicateForStoreModule(
    const EngineRequestContext& context) {
  auto savepoints = ParseSavepoints(context);
  return [savepoints = std::move(savepoints)](
             const std::uint64_t creator_tx,
             const std::uint64_t event_sequence) {
    return MetadataEventRolledBackBySavepoint(
        savepoints, creator_tx, event_sequence);
  };
}

bool ExactTextMigrationCreatorTransactionForStoreModule(
    const EngineRequestContext& context,
    const std::uint64_t creator_tx,
    const std::string_view transaction_uuid) {
  return ExactTextMigrationCreatorTransaction(
      context, creator_tx, transaction_uuid);
}

bool TextMigrationLineageCreatorVisibleForStoreModule(
    const EngineRequestContext& context,
    const std::uint64_t migration_creator_tx,
    const std::uint64_t candidate_creator_tx) {
  return TextMigrationLineageCreatorVisible(
      context, migration_creator_tx, candidate_creator_tx);
}

bool UpdateScopedRelationSummariesForStoreModule(
    const EngineRequestContext& context,
    const std::map<std::string, ScopedRelationSummaryDelta>& deltas) {
  return UpdateScopedRelationSummaries(context, deltas);
}

void FilterMgaRelationMetadataForStoreModule(
    const EngineRequestContext& context,
    RelationReadSnapshot* metadata) {
  FilterVisibleRetiredTemporaryMetadata(context, metadata);
  FilterMgaTemporaryObjectsForSession(context, metadata);
}

void FilterVisibleRetiredTemporaryMetadataForStoreModule(
    const EngineRequestContext& context,
    RelationReadSnapshot* metadata) {
  FilterVisibleRetiredTemporaryMetadata(context, metadata);
}

EngineApiDiagnostic ValidateMgaRowVersionRecordChainsForStoreModule(
    const std::vector<CrudRowVersionRecord>& rows) {
  return ValidateMgaRowVersionRecordChains(rows);
}

bool HeapReadMemoryAdd(const std::uint64_t value, std::uint64_t* total) {
  return CheckedHeapReadMemoryAdd(value, total);
}

bool HeapReadMemoryMultiply(const std::uint64_t left,
                            const std::uint64_t right,
                            std::uint64_t* product) {
  return CheckedHeapReadMemoryMultiply(left, right, product);
}

bool AddHeapReadOwnedStringMemory(const std::string& value,
                                  std::uint64_t* total) {
  return AccountHeapReadOwnedString(value, total);
}

std::string ComputeMgaConstraintMutationBatchHash(
    const MgaConstraintMutationBatch& batch,
    std::uint64_t creator_local_transaction_id,
    std::uint64_t metadata_event_sequence) {
  if (creator_local_transaction_id == 0 || metadata_event_sequence == 0) {
    return {};
  }
  return ConstraintMutationBatchSha256(
      batch, creator_local_transaction_id, metadata_event_sequence);
}

MgaMetadataWorkPresenceResult HasVisibleMgaDeferredConstraintMetadata(
    const EngineRequestContext& context) {
  MgaMetadataWorkPresenceResult result;
  if (context.database_path.empty()) {
    result.diagnostic =
        MakeInvalidRequestDiagnostic("mga.deferred_constraint_metadata",
                                     "database_path_required");
    return result;
  }
  CrudState metadata;
  const auto loaded = LoadMgaMetadata(&metadata, context);
  if (loaded.error) {
    result.diagnostic = loaded;
    return result;
  }
  const auto authority =
      OverlayMgaTransactionAuthority(context, &metadata, true);
  if (authority.error) {
    result.diagnostic = authority;
    return result;
  }
  FilterVisibleRetiredTemporaryMetadata(context, &metadata);
  FilterMgaTemporaryObjectsForSession(context, &metadata);
  for (const auto& table : metadata.tables) {
    ++result.metadata_tables_scanned;
    if (!CrudCreatorVisible(metadata,
                            table.creator_tx,
                            table.event_sequence,
                            context.local_transaction_id)) {
      continue;
    }
    for (const auto& [column_name, descriptor] : table.columns) {
      (void)column_name;
      if (RelationDescriptorRequiresDeferredStore(
              RelationDescriptorFields(descriptor))) {
        result.has_work = true;
        ++result.metadata_tables_matched;
        result.ok = true;
        result.diagnostic = OkDiagnostic();
        return result;
      }
    }
  }
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  return result;
}

MgaMetadataWorkPresenceResult HasMgaTemporaryCleanupMetadataWork(
    const EngineRequestContext& context,
    bool include_delete_rows,
    bool include_preserve_rows,
    bool retire_private_metadata) {
  MgaMetadataWorkPresenceResult result;
  if (context.database_path.empty()) {
    result.diagnostic =
        MakeInvalidRequestDiagnostic("mga.temporary_cleanup_metadata",
                                     "database_path_required");
    return result;
  }
  CrudState metadata;
  const auto loaded = LoadMgaMetadata(&metadata, context);
  if (loaded.error) {
    result.diagnostic = loaded;
    return result;
  }
  const auto authority =
      OverlayMgaTransactionAuthority(context, &metadata, true);
  if (authority.error) {
    result.diagnostic = authority;
    return result;
  }
  FilterVisibleRetiredTemporaryMetadata(context, &metadata);
  FilterMgaTemporaryObjectsForSession(context, &metadata);
  for (const auto& table : metadata.tables) {
    ++result.metadata_tables_scanned;
    if (!table.temporary) { continue; }
    if (!CrudCreatorVisible(metadata,
                            table.creator_tx,
                            table.event_sequence,
                            context.local_transaction_id)) {
      continue;
    }
    const bool delete_rows_policy = table.on_commit_action == "delete_rows";
    const bool preserve_rows_policy = table.on_commit_action == "preserve_rows";
    if ((delete_rows_policy && include_delete_rows) ||
        (preserve_rows_policy && include_preserve_rows) ||
        retire_private_metadata) {
      result.has_work = true;
      ++result.metadata_tables_matched;
      result.ok = true;
      result.diagnostic = OkDiagnostic();
      return result;
    }
  }
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  return result;
}

MgaRelationStoreResult LoadMgaRelationStoreState(const EngineRequestContext& context) {
  MgaRelationStoreResult result;
  result.full_state_load = true;
  if (context.database_path.empty()) {
    result.diagnostic = MakeInvalidRequestDiagnostic("mga.row_store", "database_path_required");
    return result;
  }
  const auto metadata = LoadMgaMetadata(&result.state.relation_metadata, context);
  if (metadata.error) {
    result.diagnostic = metadata;
    return result;
  }
  const auto authority = OverlayMgaTransactionAuthority(
      context, &result.state.relation_metadata, false);
  if (authority.error) {
    result.diagnostic = authority;
    return result;
  }
  const auto savepoints = ParseSavepoints(context);
  std::unordered_map<std::string, std::string> row_value_key_cache;
  std::set<std::string> all_table_uuids;
  for (const auto& table : result.state.relation_metadata.tables) {
    if (!table.table_uuid.empty()) {
      all_table_uuids.insert(table.table_uuid);
    }
  }
  const auto discovered_scoped_tables = DiscoverScopedRelationTableUuids(context);
  all_table_uuids.insert(discovered_scoped_tables.begin(),
                         discovered_scoped_tables.end());
  std::set<std::string> scoped_row_tables_used;
  for (const auto& table_uuid : all_table_uuids) {
    std::vector<CrudRowVersionRecord> decoded_rows;
    bool used_segment = false;
    if (!LoadDecodedScopedRowsForTable(context,
                                       table_uuid,
                                       &decoded_rows,
                                       &used_segment)) {
      result.diagnostic = MakeInvalidRequestDiagnostic(
          "mga.row_store",
          "scoped_row_segment_decode_failed");
      return result;
    }
    if (!used_segment) { continue; }
    scoped_row_tables_used.insert(table_uuid);
    result.row_versions_scanned +=
        static_cast<std::uint64_t>(decoded_rows.size());
    for (auto& row : decoded_rows) {
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
  }
  for (const auto& line : ReadLines(RowStorePath(context))) {
    const auto fields = SplitTabs(line);
    if (fields.size() < 11 || fields[0] != kRowStoreMagic ||
        fields[1] != "ROW_VERSION") {
      continue;
    }
    if (scoped_row_tables_used.count(fields[4]) != 0) {
      continue;
    }
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
    row.values = DecodeCrudPairsWithKeyCache(fields[10], &row_value_key_cache);
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
  bool scoped_index_segments_used = false;
  bool scoped_binary_index_segments_used = false;
  std::vector<CrudIndexEntryRecord> binary_index_entries;
  if (!LoadScopedBinaryIndexEntriesForTables(context,
                                             all_table_uuids,
                                             &binary_index_entries,
                                             &scoped_binary_index_segments_used)) {
    result.diagnostic = MakeInvalidRequestDiagnostic(
        "mga.index_store",
        "scoped_index_binary_segment_decode_failed");
    return result;
  }
  std::vector<std::string> index_lines =
      ReadScopedRelationLinesForTables(context,
                                       all_table_uuids,
                                       false,
                                       &scoped_index_segments_used);
  if (!scoped_index_segments_used && !scoped_binary_index_segments_used) {
    index_lines = ReadLines(IndexStorePath(context));
  }
  result.scoped_physical_segments_used =
      !scoped_row_tables_used.empty() || scoped_index_segments_used ||
      scoped_binary_index_segments_used;
  result.scoped_physical_segments_fallback =
      scoped_row_tables_used.empty() && !scoped_index_segments_used &&
      !scoped_binary_index_segments_used;

  for (const auto& line : index_lines) {
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
    entry.key_value = DecodeLineHexFieldOrRaw(fields[9]);
    entry.payload_value = DecodeLineHexFieldOrRaw(fields[10]);
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
  for (auto& entry : binary_index_entries) {
    ++result.index_entries_scanned;
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
  const auto retired_tables =
      VisibleRetiredTemporaryTableMetadata(context, result.state.relation_metadata);
  FilterVisibleRetiredTemporaryMetadata(context, &result.state.relation_metadata);
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
  const auto chain_status =
      ValidateMgaRowVersionRecordChains(result.state.row_versions);
  if (chain_status.error) {
    result.diagnostic = chain_status;
    return result;
  }
  FilterMgaTemporaryObjectsForSession(context, &result.state.relation_metadata);
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  AddRelationLoadEvidence(&result, "full");
  return result;
}

MgaRelationStoreResult LoadMgaRelationStoreStateForTargetScope(
    const EngineRequestContext& context,
    const std::vector<std::string>& table_uuids,
    const std::string& evidence_route,
    bool include_index_entries = true,
    bool include_row_versions = true,
    bool expand_constraint_scope = true,
    const std::string& row_uuid_filter = {}) {
  MgaRelationStoreResult result;
  result.scoped_state_load = true;
  if (context.database_path.empty()) {
    result.diagnostic = MakeInvalidRequestDiagnostic("mga.row_store", "database_path_required");
    return result;
  }
  if (table_uuids.empty()) {
    result.diagnostic = MakeInvalidRequestDiagnostic("mga.row_store", "target_table_uuid_required");
    return result;
  }
  for (const auto& table_uuid : table_uuids) {
    if (table_uuid.empty()) {
      result.diagnostic =
          MakeInvalidRequestDiagnostic("mga.row_store",
                                       "target_table_uuid_required");
      return result;
    }
  }
  const auto metadata = LoadMgaMetadata(&result.state.relation_metadata, context);
  if (metadata.error) {
    result.diagnostic = metadata;
    return result;
  }
  const auto authority = OverlayMgaTransactionAuthority(
      context, &result.state.relation_metadata, true);
  if (authority.error) {
    result.diagnostic = authority;
    return result;
  }
  const auto retired_tables =
      VisibleRetiredTemporaryTableMetadata(context, result.state.relation_metadata);
  FilterVisibleRetiredTemporaryMetadata(context, &result.state.relation_metadata);
  std::set<std::string> table_scope;
  for (const auto& table_uuid : table_uuids) {
    if (expand_constraint_scope) {
      const auto scoped = InsertTargetRelationScope(
          context, result.state.relation_metadata, table_uuid);
      table_scope.insert(scoped.begin(), scoped.end());
    } else {
      table_scope.insert(table_uuid);
    }
  }
  const auto savepoints = ParseSavepoints(context);

  bool row_segments_used = false;
  bool index_segments_used = false;
  bool binary_index_segments_used = false;
  std::vector<CrudIndexEntryRecord> binary_index_entries;
  std::vector<std::string> index_lines;
  if (include_index_entries) {
    if (!LoadScopedBinaryIndexEntriesForTables(context,
                                               table_scope,
                                               &binary_index_entries,
                                               &binary_index_segments_used)) {
      result.diagnostic = MakeInvalidRequestDiagnostic(
          "mga.index_store",
          "scoped_index_binary_segment_decode_failed");
      return result;
    }
    index_lines = ReadScopedRelationLinesForTables(context,
                                                   table_scope,
                                                   false,
                                                   &index_segments_used);
  }

  if (include_row_versions) {
    for (const auto& scoped_table_uuid : table_scope) {
      std::vector<CrudRowVersionRecord> decoded_rows;
      bool used_segment = false;
      if (!LoadDecodedScopedRowsForTable(context,
                                         scoped_table_uuid,
                                         &decoded_rows,
                                         &used_segment)) {
        result.diagnostic = MakeInvalidRequestDiagnostic(
            "mga.row_store",
            "scoped_row_segment_decode_failed");
        return result;
      }
      row_segments_used = row_segments_used || used_segment;
      result.row_versions_scanned +=
          static_cast<std::uint64_t>(decoded_rows.size());
      for (auto& row : decoded_rows) {
        if (table_scope.count(row.table_uuid) == 0 ||
            retired_tables.count(row.table_uuid) != 0) {
          continue;
        }
        if (!row_uuid_filter.empty() && row.row_uuid != row_uuid_filter) {
          continue;
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
    }
  }
  result.scoped_physical_segments_used =
      row_segments_used || index_segments_used || binary_index_segments_used;
  result.scoped_physical_segments_fallback = false;

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
    entry.key_value = DecodeLineHexFieldOrRaw(fields[9]);
    entry.payload_value = DecodeLineHexFieldOrRaw(fields[10]);
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
  for (auto& entry : binary_index_entries) {
    ++result.index_entries_scanned;
    if (table_scope.count(entry.table_uuid) == 0 ||
        retired_tables.count(entry.table_uuid) != 0) {
      continue;
    }
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

  if (include_row_versions && RowsContainLargeValueLocators(result.state.row_versions)) {
    const auto expanded_large_values =
        ExpandMgaLargeValueLocators(context, &result.state.row_versions);
    if (expanded_large_values.error) {
      result.diagnostic = expanded_large_values;
      return result;
    }
  }
  if (include_row_versions) {
    const auto chain_status =
        ValidateMgaRowVersionRecordChains(result.state.row_versions);
    if (chain_status.error) {
      result.diagnostic = chain_status;
      return result;
    }
  }
  RetainRelationMetadataScope(table_scope, &result.state.relation_metadata);
  FilterMgaTemporaryObjectsForSession(context, &result.state.relation_metadata);
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  AddRelationLoadEvidence(&result, evidence_route);
  result.evidence.push_back({"mga_relation_state_row_versions_loaded",
                             include_row_versions ? "true" : "false"});
  result.evidence.push_back({"mga_relation_state_index_entries_loaded",
                             include_index_entries ? "true" : "false"});
  return result;
}

MgaRelationStoreResult LoadMgaRelationStoreStateForInsertTarget(
    const EngineRequestContext& context,
    const std::string& table_uuid) {
  return LoadMgaRelationStoreStateForTargetScope(
      context,
      std::vector<std::string>{table_uuid},
      "insert_target_scoped");
}

MgaRelationStoreResult LoadMgaRelationStoreIndexesOnlyForInsertTarget(
    const EngineRequestContext& context,
    const std::string& table_uuid) {
  return LoadMgaRelationStoreStateForTargetScope(
      context,
      std::vector<std::string>{table_uuid},
      "insert_target_index_only_scoped",
      true,
      false);
}

MgaRelationStoreResult LoadMgaRelationStoreMetadataOnlyForInsertTarget(
    const EngineRequestContext& context,
    const std::string& table_uuid) {
  return LoadMgaRelationStoreStateForTargetScope(
      context,
      std::vector<std::string>{table_uuid},
      "insert_target_metadata_only_scoped",
      false,
      false);
}

MgaRelationStoreResult LoadMgaRelationStoreStateForMutationTarget(
    const EngineRequestContext& context,
    const std::string& table_uuid) {
  return LoadMgaRelationStoreStateForTargetScope(
      context,
      std::vector<std::string>{table_uuid},
      "mutation_target_scoped");
}

MgaRelationStoreResult LoadMgaRelationStoreStateForMutationTargets(
    const EngineRequestContext& context,
    const std::vector<std::string>& table_uuids) {
  return LoadMgaRelationStoreStateForTargetScope(context,
                                                table_uuids,
                                                "mutation_targets_scoped");
}

MgaRelationStoreResult LoadMgaRelationStoreRowsOnlyForMutationTarget(
    const EngineRequestContext& context,
    const std::string& table_uuid) {
  return LoadMgaRelationStoreStateForTargetScope(
      context,
      std::vector<std::string>{table_uuid},
      "mutation_target_rows_only_scoped",
      false);
}

MgaRelationStoreResult LoadMgaRelationStoreRowsOnlyForMutationTargets(
    const EngineRequestContext& context,
    const std::vector<std::string>& table_uuids) {
  return LoadMgaRelationStoreStateForTargetScope(context,
                                                table_uuids,
                                                "mutation_targets_rows_only_scoped",
                                                false);
}

MgaRelationStoreResult LoadMgaRelationStoreRowsForPointLookup(
    const EngineRequestContext& context,
    const std::string& table_uuid,
    const std::string& row_uuid) {
  if (row_uuid.empty()) {
    MgaRelationStoreResult result;
    result.scoped_state_load = true;
    result.diagnostic = MakeInvalidRequestDiagnostic(
        "mga.row_store", "row_uuid_required");
    return result;
  }
  return LoadMgaRelationStoreStateForTargetScope(
      context,
      std::vector<std::string>{table_uuid},
      "relation_point_cursor_scoped",
      false,
      true,
      false,
      row_uuid);
}

MgaRelationStoreResult LoadMgaRelationStoreStateForRelationScans(
    const EngineRequestContext& context,
    const std::vector<std::string>& table_uuids) {
  return LoadMgaRelationStoreStateForTargetScope(
      context, table_uuids, "relation_scan_scoped", true, true, false);
}

MgaRelationStoreResult LoadMgaRelationStoreIndexesForRelation(
    const EngineRequestContext& context,
    const std::string& table_uuid) {
  return LoadMgaRelationStoreStateForTargetScope(
      context, std::vector<std::string>{table_uuid},
      "relation_index_cursor_scoped", true, false, false);
}

MgaRelationStoreResult LoadMgaRelationStoreMetadataForRelation(
    const EngineRequestContext& context,
    const std::string& table_uuid) {
  return LoadMgaRelationStoreStateForTargetScope(
      context, std::vector<std::string>{table_uuid},
      "relation_metadata_scoped", false, false, false);
}

std::uint64_t CurrentMgaRelationMetadataEventSequence(
    const EngineRequestContext& context) {
  if (context.database_path.empty()) {
    return 0;
  }
  const std::uint64_t next = ScanNextMetadataEventSequence(context);
  return next == 0 ? 0 : next - 1;
}

MgaRelationIndexOnlyProofEligibilityResult
CanUseMgaRelationIndexOnlyProofForInsertTarget(
    const EngineRequestContext& context,
    const std::string& table_uuid) {
  MgaRelationIndexOnlyProofEligibilityResult result;
  if (context.database_path.empty()) {
    result.diagnostic = MakeInvalidRequestDiagnostic("mga.row_store",
                                                     "database_path_required");
    result.refusal_reason = "database_path_required";
    return result;
  }
  if (table_uuid.empty()) {
    result.diagnostic = MakeInvalidRequestDiagnostic("mga.row_store",
                                                     "target_table_uuid_required");
    result.refusal_reason = "target_table_uuid_required";
    return result;
  }

  const std::lock_guard<std::mutex> guard(ScopedRelationSummaryMutex());
  const ScopedRelationSummary summary = LoadScopedRelationSummary(context,
                                                                  table_uuid);
  result.summary_trusted = summary.trusted && !summary.malformed;
  result.row_version_count = summary.row_version_count;
  result.tombstone_count = summary.tombstone_count;
  result.update_count = summary.update_count;
  result.evidence.push_back({"mga_relation_index_only_summary_trusted",
                             result.summary_trusted ? "true" : "false"});
  result.evidence.push_back({"mga_relation_index_only_row_versions",
                             std::to_string(result.row_version_count)});
  result.evidence.push_back({"mga_relation_index_only_tombstones",
                             std::to_string(result.tombstone_count)});
  result.evidence.push_back({"mga_relation_index_only_updates",
                             std::to_string(result.update_count)});
  if (summary.malformed) {
    result.diagnostic = MakeInvalidRequestDiagnostic(
        "mga.row_store",
        "scoped_relation_summary_malformed");
    result.refusal_reason = "scoped_relation_summary_malformed";
    return result;
  }
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  if (!summary.trusted) {
    result.refusal_reason = "scoped_relation_summary_untrusted";
  } else if (summary.tombstone_count != 0) {
    result.refusal_reason = "scoped_relation_tombstones_present";
  } else if (summary.update_count != 0) {
    result.refusal_reason = "scoped_relation_updates_present";
  } else {
    result.eligible = true;
    result.refusal_reason = "eligible";
  }
  result.evidence.push_back({"mga_relation_index_only_eligible",
                             result.eligible ? "true" : "false"});
  result.evidence.push_back({"mga_relation_index_only_reason",
                             result.refusal_reason});
  return result;
}

RelationReadSnapshot BuildCrudCompatibilityStateFromMga(
    const MgaRelationStoreState& state) {
  RelationReadSnapshot merged = state.relation_metadata;
  merged.row_versions = state.row_versions;
  merged.index_entries = state.index_entries;
  merged.max_sequence = state.max_row_event_sequence;
  merged.max_index_sequence = state.max_index_event_sequence;
  return merged;
}

RelationReadSnapshot BuildCrudCompatibilityStateFromMga(
    MgaRelationStoreState&& state) {
  RelationReadSnapshot merged = std::move(state.relation_metadata);
  merged.row_versions = std::move(state.row_versions);
  merged.index_entries = std::move(state.index_entries);
  merged.max_sequence = state.max_row_event_sequence;
  merged.max_index_sequence = state.max_index_event_sequence;
  return merged;
}

struct VisibleSealedRelationDescriptorSelection {
  bool found = false;
  bool conflict = false;
  CrudSealedRelationDescriptorSnapshot snapshot;
  std::vector<std::pair<std::string, std::string>> fields;
};

VisibleSealedRelationDescriptorSelection
SelectVisibleSealedRelationDescriptorSnapshot(
    const EngineRequestContext& context,
    const RelationReadSnapshot& state,
    const CrudTableRecord& table) {
  VisibleSealedRelationDescriptorSelection result;
  const CrudSealedRelationDescriptorSnapshot* newest = nullptr;
  std::size_t newest_count = 0;
  for (const auto& candidate : state.sealed_relation_descriptor_snapshots) {
    if (candidate.relation_uuid != table.table_uuid ||
        !CrudCreatorVisible(state, candidate.creator_tx,
                            candidate.event_sequence,
                            context.local_transaction_id)) {
      continue;
    }
    if (newest == nullptr ||
        candidate.event_sequence > newest->event_sequence) {
      newest = &candidate;
      newest_count = 1;
    } else if (candidate.event_sequence == newest->event_sequence) {
      ++newest_count;
    }
  }
  if (newest == nullptr) {
    return result;
  }
  // Once a relation has a sealed descriptor lineage, a later ordinary table
  // row without an equally visible sealed descriptor cannot fall back to the
  // pre-migration sidecar. That would resurrect the provisional TEXT identity.
  if (newest->event_sequence < table.event_sequence) {
    result.conflict = true;
    return result;
  }
  if (newest_count != 1 || newest->event_sequence != table.event_sequence ||
      newest->creator_tx != table.creator_tx ||
      newest->descriptor_fields.empty()) {
    result.conflict = true;
    return result;
  }
  result.found = true;
  result.snapshot = *newest;
  result.fields = newest->descriptor_fields;
  return result;
}

EngineApiDiagnostic EnsureMgaRelationStorageDescriptor(const EngineRequestContext& context,
                                                       const CrudTableRecord& table,
                                                       const std::vector<CrudIndexRecord>& indexes,
                                                       MgaRelationStorageDescriptor* descriptor) {
  const auto persisted =
      LoadDescriptorFieldsByRelation(context, table.table_uuid);
  const auto existing = persisted.find(table.table_uuid);
  const auto state = LoadMgaRelationStoreState(context);
  if (!state.ok) return state.diagnostic;
  const auto sealed = SelectVisibleSealedRelationDescriptorSnapshot(
      context, state.state.relation_metadata, table);
  if (sealed.conflict) {
    return MakeInvalidRequestDiagnostic(
        "mga.relation_descriptor",
        "sealed_relation_descriptor_snapshot_conflict");
  }
  const auto fields = sealed.found
                          ? sealed.fields
                          : (existing == persisted.end()
                                 ? BuildPersistedMgaRelationDescriptorFields(
                                       context, table, indexes)
                                 : existing->second);
  MgaRelationStorageDescriptor built =
      BuildMgaRelationStorageDescriptorFromCrudMetadata(context, table, indexes, fields);
  const auto validated = ValidateMgaRelationStorageDescriptor(built);
  if (validated.error) { return validated; }
  if (!sealed.found && existing == persisted.end()) {
    const auto persisted_diagnostic = PersistDescriptorFields(context, table.table_uuid, fields);
    if (persisted_diagnostic.error) { return persisted_diagnostic; }
  }
  if (descriptor != nullptr) { *descriptor = std::move(built); }
  return OkDiagnostic();
}

MgaRelationStorageDescriptorLoadResult LoadMgaRelationStorageDescriptor(
    const EngineRequestContext& context,
    const std::string& relation_uuid) {
  MgaRelationStorageDescriptorLoadResult result;
  if (context.database_path.empty()) {
    result.diagnostic = MakeInvalidRequestDiagnostic(
        "mga.relation_descriptor.load", "database_path_required");
    return result;
  }
  if (relation_uuid.empty()) {
    result.diagnostic = MakeInvalidRequestDiagnostic(
        "mga.relation_descriptor.load", "relation_uuid_required");
    return result;
  }
  if (context.local_transaction_id == 0 ||
      context.transaction_uuid.canonical.empty()) {
    result.diagnostic = MakeInvalidRequestDiagnostic(
        "mga.relation_descriptor.load",
        "exact_active_transaction_identity_required");
    return result;
  }

  const auto parsed_transaction = scratchbird::core::uuid::ParseTypedUuid(
      scratchbird::core::platform::UuidKind::transaction,
      context.transaction_uuid.canonical);
  if (!parsed_transaction.ok()) {
    result.diagnostic = MakeInvalidRequestDiagnostic(
        "mga.relation_descriptor.load", "transaction_uuid_invalid");
    return result;
  }
  const auto inventory = ResolveStatementTransactionInventory(context);
  if (!inventory.ok()) {
    result.diagnostic = inventory.diagnostic;
    return result;
  }
  const auto exact_transaction = LookupLocalTransaction(
      inventory.snapshot->inventory,
      MakeLocalTransactionId(context.local_transaction_id));
  if (!exact_transaction.ok() ||
      exact_transaction.entry.identity.transaction_uuid.value !=
          parsed_transaction.value.value ||
      (exact_transaction.entry.state != TransactionState::active &&
       exact_transaction.entry.state != TransactionState::read_only_active)) {
    result.diagnostic = MakeInvalidRequestDiagnostic(
        "mga.relation_descriptor.load",
        "exact_active_transaction_identity_required");
    return result;
  }

  const std::array<std::string, 1> required_relations{relation_uuid};
  const auto metadata_view =
      LoadMgaStatementMetadataView(context, required_relations);
  if (!metadata_view.ok()) {
    result.diagnostic = metadata_view.diagnostic;
    return result;
  }
  const auto& statement_view = *metadata_view.view;
  const auto& metadata = statement_view.visible_metadata;
  const auto table_ordinal =
      statement_view.visible_table_ordinals.find(relation_uuid);
  if (table_ordinal == statement_view.visible_table_ordinals.end() ||
      table_ordinal->second >= metadata.tables.size()) {
    result.diagnostic = MakeInvalidRequestDiagnostic(
        "mga.relation_descriptor.load", "relation_not_visible");
    return result;
  }
  const auto& table = metadata.tables[table_ordinal->second];

  const auto& persisted = *statement_view.descriptor_fields;
  const auto fields = persisted.find(relation_uuid);
  const auto sealed = SelectVisibleSealedRelationDescriptorSnapshot(
      context, metadata, table);
  if (sealed.conflict) {
    result.diagnostic = MakeInvalidRequestDiagnostic(
        "mga.relation_descriptor.load",
        "sealed_relation_descriptor_snapshot_conflict");
    return result;
  }
  if (!sealed.found && fields == persisted.end()) {
    result.diagnostic = MakeInvalidRequestDiagnostic(
        "mga.relation_descriptor.load", "persisted_descriptor_required");
    return result;
  }
  static const std::vector<CrudIndexRecord> kNoVisibleIndexes;
  const auto indexed =
      statement_view.visible_indexes_by_relation.find(relation_uuid);
  const auto& indexes =
      indexed == statement_view.visible_indexes_by_relation.end()
          ? kNoVisibleIndexes
          : indexed->second;
  result.descriptor = BuildMgaRelationStorageDescriptorFromCrudMetadata(
      context, table, indexes,
      sealed.found ? sealed.fields : fields->second);
  const auto validated =
      ValidateMgaRelationStorageDescriptor(result.descriptor);
  if (validated.error) {
    result.diagnostic = validated;
    return result;
  }
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  return result;
}

MgaVisibleContextualTextSidecarSnapshotLoadResultV2
LoadVisibleMgaContextualTextSidecarSnapshotV2(
    const EngineRequestContext& context,
    const std::string& relation_uuid,
    const std::string& relation_descriptor_uuid,
    const std::uint64_t relation_descriptor_generation) {
  constexpr const char* kOperation =
      "mga.contextual_text_sidecar_snapshot.load";
  MgaVisibleContextualTextSidecarSnapshotLoadResultV2 result;
  const auto refuse = [&](std::string detail) {
    result.diagnostic = MakeEngineApiDiagnostic(
        "CTB.TEXT.DESCRIPTOR_INVALID", kOperation, std::move(detail), true);
    return result;
  };
  if (!CanonicalNonNilMigrationUuid(relation_uuid) ||
      !CanonicalNonNilMigrationUuid(relation_descriptor_uuid) ||
      relation_descriptor_generation == 0) {
    return refuse("exact relation descriptor identity required");
  }

  const auto loaded_descriptor =
      LoadMgaRelationStorageDescriptor(context, relation_uuid);
  if (!loaded_descriptor.ok) {
    result.diagnostic = loaded_descriptor.diagnostic;
    return result;
  }
  if (loaded_descriptor.descriptor.relation_uuid.canonical != relation_uuid ||
      loaded_descriptor.descriptor.descriptor_uuid.canonical !=
          relation_descriptor_uuid ||
      loaded_descriptor.descriptor.descriptor_generation !=
          relation_descriptor_generation) {
    return refuse("visible relation descriptor identity does not match claim");
  }

  const std::array<std::string, 1> required_relations{relation_uuid};
  const auto metadata_view =
      LoadMgaStatementMetadataView(context, required_relations);
  if (!metadata_view.ok()) {
    result.diagnostic = metadata_view.diagnostic;
    return result;
  }
  const auto& metadata = metadata_view.view->visible_metadata;
  const auto table_ordinal =
      metadata_view.view->visible_table_ordinals.find(relation_uuid);
  if (table_ordinal == metadata_view.view->visible_table_ordinals.end() ||
      table_ordinal->second >= metadata.tables.size()) {
    return refuse("relation is not visible");
  }
  const auto& table = metadata.tables[table_ordinal->second];
  const auto selected = SelectVisibleSealedRelationDescriptorSnapshot(
      context, metadata, table);
  if (!selected.found || selected.conflict) {
    return refuse("one exact sealed descriptor snapshot is required");
  }
  const auto& persisted = selected.snapshot;
  if (persisted.creator_tx != table.creator_tx ||
      persisted.event_sequence != table.event_sequence ||
      persisted.relation_uuid != relation_uuid ||
      persisted.relation_descriptor_uuid != relation_descriptor_uuid ||
      persisted.relation_descriptor_generation !=
          relation_descriptor_generation ||
      persisted.descriptor_field_count == 0 ||
      persisted.descriptor_field_bytes == 0 ||
      persisted.descriptor_fields.empty()) {
    return refuse("sealed descriptor owner or complete-vector header differs");
  }

  MgaVisibleContextualTextSidecarSnapshotV2 snapshot;
  snapshot.table = table;
  snapshot.relation_descriptor = loaded_descriptor.descriptor;
  const auto copy_uuid = [](const std::string& text,
                            MgaContextualTextUuidV2* output) {
    if (output == nullptr) return false;
    const auto parsed = scratchbird::core::uuid::ParseUuid(text);
    if (!parsed.ok() || scratchbird::core::uuid::IsNilUuid(parsed.value) ||
        scratchbird::core::uuid::UuidToString(parsed.value) != text) {
      return false;
    }
    std::copy(parsed.value.bytes.begin(), parsed.value.bytes.end(),
              output->begin());
    return true;
  };
  snapshot.owner.creator_transaction_id = persisted.creator_tx;
  snapshot.owner.event_sequence = persisted.event_sequence;
  snapshot.owner.relation_descriptor_generation =
      persisted.relation_descriptor_generation;
  if (!copy_uuid(persisted.relation_uuid,
                 &snapshot.owner.relation_uuid) ||
      !copy_uuid(persisted.relation_descriptor_uuid,
                 &snapshot.owner.relation_descriptor_uuid)) {
    return refuse("sealed descriptor owner UUID is invalid");
  }

  const auto base_fields = SerializeMgaRelationStorageDescriptor(
      snapshot.relation_descriptor);
  snapshot.base_descriptor_fields.reserve(base_fields.size());
  for (const auto& [key, value] : base_fields) {
    snapshot.base_descriptor_fields.push_back(
        {{key.begin(), key.end()}, {value.begin(), value.end()}});
  }
  snapshot.sealed_sidecar_set.owner = snapshot.owner;
  snapshot.sealed_sidecar_set.descriptor_field_count =
      persisted.descriptor_field_count;
  snapshot.sealed_sidecar_set.descriptor_field_bytes =
      persisted.descriptor_field_bytes;
  snapshot.sealed_sidecar_set.contextual_sidecar_count =
      persisted.contextual_sidecar_count;
  snapshot.sealed_sidecar_set.descriptor_fields.reserve(
      persisted.descriptor_fields.size());
  for (const auto& [key, value] : persisted.descriptor_fields) {
    snapshot.sealed_sidecar_set.descriptor_fields.push_back(
        {{key.begin(), key.end()}, {value.begin(), value.end()}});
  }
  if (snapshot.sealed_sidecar_set.descriptor_fields.size() <
          snapshot.base_descriptor_fields.size() + 1 ||
      !std::equal(snapshot.base_descriptor_fields.begin(),
                  snapshot.base_descriptor_fields.end(),
                  snapshot.sealed_sidecar_set.descriptor_fields.begin())) {
    return refuse("sealed descriptor base field order or bytes changed");
  }
  const auto& final_pair =
      snapshot.sealed_sidecar_set.descriptor_fields.back();
  const std::string final_key(final_pair.key_raw_bytes.begin(),
                              final_pair.key_raw_bytes.end());
  if (final_key != kMgaContextualTextSidecarSetSealKeyV2 ||
      final_pair.value_raw_bytes.size() !=
          snapshot.sealed_sidecar_set.seal_sha256.size()) {
    return refuse("sealed descriptor final seal pair is invalid");
  }
  std::copy(final_pair.value_raw_bytes.begin(),
            final_pair.value_raw_bytes.end(),
            snapshot.sealed_sidecar_set.seal_sha256.begin());
  MgaContextualTextRawBytesV2 canonical_fields;
  std::uint64_t canonical_field_bytes = 0;
  MgaContextualTextSidecarSetDiagnosticV2 sidecar_diagnostic;
  if (!SerializeMgaContextualTextDescriptorFieldVectorV2(
          snapshot.sealed_sidecar_set.descriptor_fields,
          &canonical_fields, &canonical_field_bytes,
          &sidecar_diagnostic) ||
      snapshot.sealed_sidecar_set.descriptor_field_count !=
          snapshot.sealed_sidecar_set.descriptor_fields.size() ||
      snapshot.sealed_sidecar_set.descriptor_field_bytes !=
          canonical_field_bytes) {
    return refuse(sidecar_diagnostic.detail.empty()
                      ? "sealed descriptor vector count or byte total differs"
                      : sidecar_diagnostic.detail);
  }
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  result.snapshot = std::move(snapshot);
  return result;
}

MgaContextualTextTargetSelectionResultV2
SelectVisibleMgaContextualTextTargetV2(
    const EngineRequestContext& context,
    const sblr::ContextualTextLiteralDemandV2& structural_claim,
    const EngineContextualTextPolicyRowSetV2& exact_policy_rows) {
  constexpr const char* kOperation =
      "mga.contextual_text_target.select_visible_v2";
  MgaContextualTextTargetSelectionResultV2 result;
  const auto refuse = [&](std::string detail) {
    result.diagnostic = MakeEngineApiDiagnostic(
        "CTB.TEXT.DESCRIPTOR_INVALID", kOperation, std::move(detail), true);
    return result;
  };
  const std::string relation_uuid =
      ContextualUuidTextV2(structural_claim.relation_uuid);
  const std::string descriptor_uuid =
      ContextualUuidTextV2(structural_claim.relation_descriptor_uuid);
  const std::string column_uuid =
      ContextualUuidTextV2(structural_claim.column_uuid);
  if (!CanonicalNonNilMigrationUuid(relation_uuid) ||
      !CanonicalNonNilMigrationUuid(descriptor_uuid) ||
      !CanonicalNonNilMigrationUuid(column_uuid) ||
      structural_claim.relation_descriptor_generation == 0) {
    return refuse("contextual target structural identity is invalid");
  }
  EngineApiDiagnostic policy_diagnostic;
  if (!RevalidateEngineContextualTextPolicyRowSetV2(
          context, exact_policy_rows, &policy_diagnostic)) {
    result.diagnostic = std::move(policy_diagnostic);
    return result;
  }

  auto loaded = LoadVisibleMgaContextualTextSidecarSnapshotV2(
      context, relation_uuid, descriptor_uuid,
      structural_claim.relation_descriptor_generation);
  if (!loaded.ok) {
    result.diagnostic = std::move(loaded.diagnostic);
    return result;
  }
  MgaContextualTextProjectionMaterialV2 projection;
  EngineApiDiagnostic projection_diagnostic;
  if (!BuildMgaContextualTextProjectionMaterialV2(
          context, loaded.snapshot.relation_descriptor, exact_policy_rows,
          &projection, &projection_diagnostic)) {
    result.diagnostic = std::move(projection_diagnostic);
    return result;
  }
  const auto target = std::ranges::find_if(
      projection.projected_columns,
      [&](const MgaContextualTextProjectedColumnV2& candidate) {
        return candidate.column_ordinal == structural_claim.column_ordinal &&
               candidate.column_uuid == structural_claim.column_uuid;
      });
  if (target == projection.projected_columns.end() ||
      !target->comparable_persisted_text) {
    return refuse(
        "claimed target is not one exact comparable persisted d718 column");
  }

  MgaContextualTextSidecarSetDiagnosticV2 sidecar_diagnostic;
  if (!ValidateMgaContextualTextSidecarSetV2(
          loaded.snapshot.owner, loaded.snapshot.base_descriptor_fields,
          projection.projected_columns, loaded.snapshot.sealed_sidecar_set,
          &sidecar_diagnostic)) {
    result.diagnostic = MakeEngineApiDiagnostic(
        sidecar_diagnostic.code.empty()
            ? "CTB.TEXT.DESCRIPTOR_INVALID"
            : sidecar_diagnostic.code,
        kOperation, sidecar_diagnostic.detail, true);
    return result;
  }
  std::vector<std::uint8_t> exact_projection;
  EngineApiDiagnostic encoded_diagnostic;
  if (!EncodeEnginePublicRelationProjectionV3(
          projection.public_projection, &exact_projection,
          &encoded_diagnostic)) {
    result.diagnostic = std::move(encoded_diagnostic);
    return result;
  }

  result.selection.exact_public_relation_projection_v3 =
      std::move(exact_projection);
  result.selection.sidecar_owner = loaded.snapshot.owner;
  result.selection.base_descriptor_fields =
      std::move(loaded.snapshot.base_descriptor_fields);
  result.selection.projected_columns =
      std::move(projection.projected_columns);
  result.selection.sealed_sidecar_set =
      std::move(loaded.snapshot.sealed_sidecar_set);
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  return result;
}

EngineApiDiagnostic ValidateMgaHeapTemporaryRelationAuthorityForStoreModule(
    const EngineRequestContext& context, const CrudTableRecord& table) {
  const auto exact_session_uuid = [](const std::string& value) {
    const auto parsed = scratchbird::core::uuid::ParseTypedUuid(
        scratchbird::core::platform::UuidKind::session, value);
    return parsed.ok() &&
           scratchbird::core::uuid::UuidToString(parsed.value.value) == value;
  };
  if (!table.temporary) {
    if (!table.temporary_scope.empty() ||
        !table.temporary_session_uuid.empty()) {
      return MakeInvalidRequestDiagnostic(
          "mga.heap_relation_read.prepare",
          "permanent_relation_temporary_authority_invalid");
    }
    return OkDiagnostic();
  }
  if (!exact_session_uuid(context.session_uuid.canonical)) {
    return MakeInvalidRequestDiagnostic(
        "mga.heap_relation_read.prepare",
        "temporary_relation_session_authority_required");
  }
  if (table.temporary_scope == "global") {
    if (!table.temporary_session_uuid.empty()) {
      return MakeInvalidRequestDiagnostic(
          "mga.heap_relation_read.prepare",
          "global_temporary_relation_owner_invalid");
    }
    return OkDiagnostic();
  }
  if (table.temporary_scope != "private" ||
      !exact_session_uuid(table.temporary_session_uuid) ||
      table.temporary_session_uuid != context.session_uuid.canonical) {
    return MakeInvalidRequestDiagnostic(
        "mga.heap_relation_read.prepare",
        "private_temporary_relation_owner_mismatch");
  }
  return OkDiagnostic();
}

PreparedMgaHeapReadAuthorityCohortResult
PrepareMgaHeapReadAuthoritiesForStoreModule(
    const EngineRequestContext& context,
    const std::span<const std::string> relation_uuids,
    const scratchbird::transaction::mga::SnapshotVectorDescriptor*
        resolved_statement_snapshot) {
  PreparedMgaHeapReadAuthorityCohortResult result;
  const auto refuse = [&](EngineApiDiagnostic diagnostic) {
    result = {};
    result.diagnostic = std::move(diagnostic);
    return result;
  };
  if (relation_uuids.empty() || context.database_path.empty() ||
      context.database_uuid.canonical.empty() ||
      context.local_transaction_id == 0 ||
      context.transaction_uuid.canonical.empty() ||
      context.statement_uuid.canonical.empty() ||
      context.statement_snapshot_uuid.canonical.empty() ||
      !context.statement_metadata_snapshot_engine_owned ||
      context.statement_metadata_snapshot_uuid.canonical.empty() ||
      context.catalog_epoch_uuid.canonical.empty() ||
      !context.security_context_present ||
      !context.authorization_context.present ||
      context.catalog_generation_id == 0 || context.security_epoch == 0 ||
      context.resource_epoch == 0) {
    return refuse(MakeInvalidRequestDiagnostic(
        "mga.heap_relation_read.prepare",
        "exact_statement_authority_cohort_required"));
  }
  std::set<std::string> unique_relations;
  for (const auto& relation_uuid : relation_uuids) {
    if (relation_uuid.empty() || !unique_relations.insert(relation_uuid).second) {
      if (!relation_uuid.empty()) continue;
      return refuse(MakeInvalidRequestDiagnostic(
          "mga.heap_relation_read.prepare", "exact_relation_uuid_required"));
    }
  }

  const auto inventory_guard =
      AcquireTransactionInventoryGuard(context.database_path);
  EngineRequestContext statement_context = context;
  if (statement_context.statement_transaction_inventory_snapshot == nullptr) {
    auto acquired = scratchbird::storage::database::
        AcquireLocalTransactionInventorySnapshot(context.database_path);
    if (!acquired.ok()) {
      return refuse(MakeEngineApiDiagnostic(
          acquired.diagnostic.diagnostic_code.empty()
              ? "SB-MGA-TXN-INV-LOAD-FAILED"
              : acquired.diagnostic.diagnostic_code,
          acquired.diagnostic.message_key.empty()
              ? "mga.transaction_inventory.load_failed"
              : acquired.diagnostic.message_key,
          acquired.diagnostic.remediation_hint, true));
    }
    statement_context.statement_transaction_inventory_snapshot =
        std::move(acquired.snapshot);
  }

  scratchbird::transaction::mga::SnapshotVectorDescriptor snapshot_vector;
  if (resolved_statement_snapshot != nullptr) {
    snapshot_vector = *resolved_statement_snapshot;
  } else {
    EngineResolveStatementSnapshotRequest snapshot_request;
    snapshot_request.context = statement_context;
    auto snapshot = EngineResolveStatementSnapshot(snapshot_request);
    if (!snapshot.ok) {
      return refuse(MakeInvalidRequestDiagnostic(
          "mga.heap_relation_read.prepare",
          "exact_current_statement_snapshot_vector_required"));
    }
    snapshot_vector = std::move(snapshot.snapshot_vector);
  }
  if (!snapshot_vector.inventory_authoritative ||
      !snapshot_vector.complete ||
      snapshot_vector.owning_transaction.value !=
          context.local_transaction_id) {
    return refuse(MakeInvalidRequestDiagnostic(
        "mga.heap_relation_read.prepare",
        "exact_current_statement_snapshot_vector_required"));
  }
  const auto metadata_view =
      LoadMgaStatementMetadataView(statement_context, relation_uuids);
  if (!metadata_view.ok()) return refuse(metadata_view.diagnostic);
  const auto& metadata = metadata_view.view->visible_metadata;

  auto statement = std::make_shared<PreparedMgaHeapStatementAuthority>();
  statement->snapshot_vector = std::move(snapshot_vector);
  statement->transaction_states =
      std::make_shared<const std::map<std::uint64_t, std::string>>(
          metadata.transactions);
  statement->transaction_inventory_snapshot =
      metadata_view.view->transaction_inventory_snapshot;
  statement->database_uuid = context.database_uuid.canonical;
  statement->statement_uuid = context.statement_uuid.canonical;
  statement->transaction_uuid = context.transaction_uuid.canonical;
  statement->statement_snapshot_uuid =
      context.statement_snapshot_uuid.canonical;
  statement->statement_metadata_snapshot_uuid =
      context.statement_metadata_snapshot_uuid.canonical;
  statement->catalog_epoch_uuid = context.catalog_epoch_uuid.canonical;
  statement->authorization_authority_uuid =
      context.authorization_context.authority_uuid.canonical;
  statement->catalog_generation = context.catalog_generation_id;
  statement->security_epoch = context.authorization_context.security_epoch;
  statement->policy_epoch = context.authorization_context.policy_epoch;
  statement->resource_epoch = context.resource_epoch;
  statement->local_transaction_id = context.local_transaction_id;
  statement->metadata_path = metadata_view.key.metadata.metadata_path;
  statement->metadata_file_size =
      metadata_view.key.metadata.metadata_file_size;
  statement->metadata_file_mtime_ticks =
      metadata_view.key.metadata.metadata_file_mtime_ticks;
  statement->savepoint_path = metadata_view.key.metadata.savepoint_path;
  statement->savepoint_file_size =
      metadata_view.key.metadata.savepoint_file_size;
  statement->savepoint_file_mtime_ticks =
      metadata_view.key.metadata.savepoint_file_mtime_ticks;

  statement->descriptor_path = DescriptorStorePath(statement_context);
  const auto descriptor_identity =
      ExistingFileIdentity(statement->descriptor_path);
  statement->descriptor_file_size =
      descriptor_identity.ok ? descriptor_identity.file_size : 0;
  statement->descriptor_file_mtime_ticks =
      descriptor_identity.ok ? descriptor_identity.file_mtime_ticks : 0;

  const auto& persisted = *metadata_view.view->descriptor_fields;
  auto cohort = std::make_shared<PreparedMgaHeapReadAuthorityCohort>();
  cohort->statement = statement;
  for (const auto& relation_uuid : unique_relations) {
    const auto table_ordinal =
        metadata_view.view->visible_table_ordinals.find(relation_uuid);
    if (table_ordinal ==
            metadata_view.view->visible_table_ordinals.end() ||
        table_ordinal->second >= metadata.tables.size()) {
      return refuse(MakeInvalidRequestDiagnostic(
          "mga.heap_relation_read.prepare", "heap_relation_not_visible"));
    }
    const auto& table = metadata.tables[table_ordinal->second];
    const auto temporary_authority =
        ValidateMgaHeapTemporaryRelationAuthorityForStoreModule(context,
                                                                table);
    if (temporary_authority.error) return refuse(temporary_authority);
    const auto authorization = EvaluateMaterializedAuthorization(
        context, context.authorization_context, "SELECT", relation_uuid);
    if (!authorization.authorized || authorization.denied ||
        authorization.policy_recheck_required ||
        !authorization.diagnostics.empty()) {
      return refuse(MakeInvalidRequestDiagnostic(
          "mga.heap_relation_read.prepare",
          authorization.diagnostics.empty()
              ? "select_authorization_is_indeterminate"
              : authorization.diagnostics.front().detail));
    }
    const auto sealed = SelectVisibleSealedRelationDescriptorSnapshot(
        statement_context, metadata, table);
    if (sealed.conflict) {
      return refuse(MakeInvalidRequestDiagnostic(
          "mga.heap_relation_read.prepare",
          "sealed_relation_descriptor_snapshot_conflict"));
    }
    const auto fields = persisted.find(relation_uuid);
    if (!sealed.found && fields == persisted.end()) {
      return refuse(MakeInvalidRequestDiagnostic(
          "mga.heap_relation_read.prepare", "persisted_descriptor_required"));
    }
    static const std::vector<CrudIndexRecord> kNoVisibleIndexes;
    const auto indexed =
        metadata_view.view->visible_indexes_by_relation.find(relation_uuid);
    const auto& indexes =
        indexed == metadata_view.view->visible_indexes_by_relation.end()
            ? kNoVisibleIndexes
            : indexed->second;
    auto relation = std::make_shared<PreparedMgaHeapReadAuthority>();
    relation->statement = statement;
    relation->descriptor = BuildMgaRelationStorageDescriptorFromCrudMetadata(
        statement_context, table, indexes,
        sealed.found ? sealed.fields : fields->second);
    const auto validated =
        ValidateMgaRelationStorageDescriptor(relation->descriptor);
    if (validated.error ||
        relation->descriptor.relation_uuid.canonical != relation_uuid ||
        relation->descriptor.database_uuid.canonical !=
            context.database_uuid.canonical ||
        relation->descriptor.relation_kind != "table" ||
        relation->descriptor.storage_profile != "local_mga_rowstore_v1" ||
        relation->descriptor.descriptor_uuid.canonical.empty() ||
        relation->descriptor.descriptor_generation == 0 ||
        relation->descriptor.descriptor_status.empty()) {
      return refuse(validated.error
                        ? validated
                        : MakeInvalidRequestDiagnostic(
                              "mga.heap_relation_read.prepare",
                              "current_persisted_local_heap_descriptor_required"));
    }
    relation->current_relation_base_generation = table.event_sequence;
    relation->relation_uuid = relation_uuid;
    relation->temporary = table.temporary;
    relation->temporary_scope = table.temporary_scope;
    relation->temporary_session_uuid = table.temporary_session_uuid;
    cohort->relations.emplace(relation_uuid, std::move(relation));
  }
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  result.cohort = std::move(cohort);
  return result;
}

PreparedMgaHeapReadAuthorityResult PrepareMgaHeapReadAuthorityForStoreModule(
    const EngineRequestContext& context, const std::string& relation_uuid) {
  PreparedMgaHeapReadAuthorityResult result;
  const std::array<std::string, 1> relations{relation_uuid};
  auto prepared = PrepareMgaHeapReadAuthoritiesForStoreModule(context,
                                                              relations);
  if (!prepared.ok || prepared.cohort == nullptr) {
    result.diagnostic = std::move(prepared.diagnostic);
    return result;
  }
  const auto found = prepared.cohort->relations.find(relation_uuid);
  if (found == prepared.cohort->relations.end() || found->second == nullptr) {
    result.diagnostic = MakeInvalidRequestDiagnostic(
        "mga.heap_relation_read.prepare", "heap_relation_not_prepared");
    return result;
  }
  result.authority = *found->second;
  result.ok = true;
  return result;
}


}  // namespace scratchbird::engine::internal_api
