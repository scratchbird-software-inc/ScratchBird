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
constexpr std::string_view kSealedTableMetadataKindV2 =
    "TABLE_METADATA_SEALED_DESCRIPTOR_V2";
constexpr std::string_view kSealedTableMetadataFormatV2 =
    "mga_sealed_contextual_text_sidecar_set_v2";
namespace sealed_table_metadata_field_v2 {
inline constexpr std::size_t kMagic = 0;
inline constexpr std::size_t kRecordKind = 1;
inline constexpr std::size_t kCreatorTx = 2;
inline constexpr std::size_t kEventSequence = 3;
inline constexpr std::size_t kFormat = 4;
inline constexpr std::size_t kSealState = 5;
inline constexpr std::size_t kTableUuid = 6;
inline constexpr std::size_t kDefaultName = 7;
inline constexpr std::size_t kColumns = 8;
inline constexpr std::size_t kTemporary = 9;
inline constexpr std::size_t kTemporaryScope = 10;
inline constexpr std::size_t kTemporarySessionUuid = 11;
inline constexpr std::size_t kOnCommitAction = 12;
inline constexpr std::size_t kRelationDescriptorUuid = 13;
inline constexpr std::size_t kRelationDescriptorGeneration = 14;
inline constexpr std::size_t kDescriptorFieldCount = 15;
inline constexpr std::size_t kDescriptorFieldBytes = 16;
inline constexpr std::size_t kContextualSidecarCount = 17;
inline constexpr std::size_t kDescriptorFields = 18;
inline constexpr std::size_t kFieldCount = 19;
}  // namespace sealed_table_metadata_field_v2
constexpr std::string_view kDmlUpdateStatementSavepointCreateKind =
    "DML_UPDATE_STATEMENT_SAVEPOINT_CREATE_V1";
constexpr std::string_view kDmlUpdateStatementSavepointRollbackKind =
    "DML_UPDATE_STATEMENT_SAVEPOINT_ROLLBACK_V1";
constexpr std::string_view kDmlUpdateStatementSavepointReleaseKind =
    "DML_UPDATE_STATEMENT_SAVEPOINT_RELEASE_V1";
constexpr std::string_view kDmlUpdateStatementSavepointEvidenceDomain =
    "ScratchBird.MgaDmlUpdateStatementSavepointAuthority.V1";
constexpr const char* kDescriptorMagic = "SBMGADESC1";
constexpr std::string_view kLineHexFieldPrefix = "SBHEX:";
constexpr std::string_view kBigintMigrationFormat =
    "datatype_bigint_identity_migration_v1";
constexpr std::string_view kBigintMigrationId =
    "core.datatype.bigint.identity.v1";
constexpr std::string_view kLegacyBigintTypeUuid =
    "67000000-696e-7436-b400-000000000000";
constexpr std::string_view kCanonicalBigintTypeUuid =
    "019d0000-0000-7000-8000-00000000d712";
constexpr std::string_view kInt32MigrationFormat =
    "datatype_int32_identity_migration_v1";
constexpr std::string_view kInt32MigrationId =
    "core.datatype.int32.identity.v1";
constexpr std::string_view kLegacyInt32DescriptorUuid =
    "66000000-696e-7433-b200-000000000000";
constexpr std::string_view kLegacyInt32TypeUuid =
    "66000000-696e-7433-b200-000000000000";
constexpr std::string_view kCanonicalInt32DescriptorUuid =
    "019d0000-0000-7000-8000-00000000d716";
constexpr std::string_view kCanonicalInt32TypeUuid =
    "019d0000-0000-7000-8000-00000000d717";
constexpr std::string_view kTextMigrationFormat =
    "datatype_text_identity_migration_v1";
constexpr std::string_view kTextMigrationId =
    "core.datatype.text.identity.v1";
constexpr std::string_view kLegacyTextDescriptorUuid =
    "2c010000-6368-7172-a163-746572000000";
constexpr std::string_view kLegacyTextTypeUuid =
    "2c010000-6368-7172-a163-746572000000";
constexpr std::string_view kCanonicalTextDescriptorUuid =
    "019d0000-0000-7000-8000-00000000d718";
constexpr std::string_view kCanonicalTextTypeUuid =
    "019d0000-0000-7000-8000-00000000d719";
constexpr std::string_view kCanonicalTextCodecUuid =
    "019d0000-0000-7000-8000-00000000d71a";
constexpr std::string_view kCanonicalTextCodecId =
    "datatype.text.utf8.v1";

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

std::string SavepointStorePath(const EngineRequestContext& context) {
  return context.database_path + ".sb.mga_savepoints";
}

std::string DmlUpdateDurableOperationStorePath(
    const EngineRequestContext& context) {
  return context.database_path + ".sb.mga_durable_operations";
}

std::string DmlUpdateStatementSavepointBinaryStorePath(
    const EngineRequestContext& context) {
  return context.database_path + ".sb.mga_update_statement_savepoints.v1";
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
  std::map<std::uint64_t,
           std::vector<std::pair<std::uint64_t, std::uint64_t>>>
      normalized_row_rollback_ranges;
  bool row_rollback_ranges_normalized = false;
  bool update_statement_authority_corrupt = false;
};

bool ApplyDmlUpdateBinarySavepointRecords(
    const EngineRequestContext& context,
    SavepointParsedState* state,
    std::string* refusal_detail = nullptr);

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

SavepointParsedState ParseSavepoints(const EngineRequestContext& context) {
  SavepointParsedState state;
  for (const auto& line : ReadLines(SavepointStorePath(context))) {
    ApplySavepointRecordLine(line, &state);
  }
  std::string ignored_detail;
  if (!ApplyDmlUpdateBinarySavepointRecords(context, &state,
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
    if (!ApplyDmlUpdateBinarySavepointRecords(context, state,
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
  if (!ApplyDmlUpdateBinarySavepointRecords(context, state,
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

MgaTemporaryTableVisibilityResult CheckMgaTemporaryTableVisibility(
    const EngineRequestContext& context,
    const std::string& table_uuid) {
  MgaTemporaryTableVisibilityResult result;
  if (table_uuid.empty()) {
    result.diagnostic = MakeInvalidRequestDiagnostic("mga.temporary_table_visibility",
                                                     "table_uuid_required");
    return result;
  }
  const auto metadata = LoadMgaMetadataSnapshot(context);
  if (!metadata.ok()) {
    result.diagnostic = metadata.diagnostic;
    return result;
  }
  CrudState state;
  state.tables = metadata.snapshot->tables;
  state.indexes = metadata.snapshot->indexes;
  state.sealed_relation_descriptor_snapshots =
      metadata.snapshot->sealed_relation_descriptor_snapshots;
  state.max_event_sequence = metadata.snapshot->max_event_sequence;
  const bool known_temporary_relation =
      metadata.snapshot->known_temporary_relation_uuids.contains(table_uuid);
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
    result.known_temporary = known_temporary_relation;
    result.hidden_by_temporary_visibility = known_temporary_relation;
    return result;
  }
  const auto authority =
      OverlayMgaTransactionAuthority(context, &state, true);
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
    const bool legacy_temporary_table =
        fields.size() >= 11 && fields[0] == kRowStoreMagic &&
        fields[1] == "TABLE_METADATA" && fields[7] == "1";
    const bool sealed_temporary_table =
        fields.size() == sealed_table_metadata_field_v2::kFieldCount &&
        fields[0] == kRowStoreMagic &&
        fields[1] == kSealedTableMetadataKindV2 &&
        fields[sealed_table_metadata_field_v2::kTemporary] == "1";
    if (legacy_temporary_table || sealed_temporary_table) {
      const auto authority = classify_event(ParseU64(fields[2]));
      if (authority != EventAuthority::kCommitted) { continue; }
      const std::size_t table_uuid_index =
          sealed_temporary_table
              ? sealed_table_metadata_field_v2::kTableUuid
              : 4;
      const std::size_t temporary_scope_index =
          sealed_temporary_table
              ? sealed_table_metadata_field_v2::kTemporaryScope
              : 8;
      temporary_tables.insert(fields[table_uuid_index]);
      if (fields[temporary_scope_index] == "global") {
        durable_global_tables.insert(fields[table_uuid_index]);
      } else {
        committed_private_tables.insert(fields[table_uuid_index]);
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

  const auto large_values = ClassifyMgaTemporaryLargeValueRecovery(
      context, temporary_tables, transaction_states);
  if (large_values.diagnostic.error) {
    result.diagnostic = large_values.diagnostic;
    return result;
  }
  result.reclaimed_large_value_count =
      large_values.reclaimed_large_value_count;
  result.orphaned_large_value_count =
      large_values.orphaned_large_value_count;
  result.rolled_back_event_count += large_values.rolled_back_event_count;
  result.active_or_unresolved_event_count +=
      large_values.active_or_unresolved_event_count;
  result.fenced_event_count += large_values.fenced_event_count;

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

bool AccountHeapReadEngineDescriptorMemory(
    const EngineDescriptor& descriptor, std::uint64_t* total) {
  return AccountHeapReadOwnedString(descriptor.descriptor_uuid.canonical,
                                    total) &&
         AccountHeapReadOwnedString(descriptor.descriptor_kind, total) &&
         AccountHeapReadOwnedString(descriptor.canonical_type_name, total) &&
         AccountHeapReadOwnedString(descriptor.encoded_descriptor, total);
}

std::optional<std::uint64_t> HeapReadStorageDescriptorMemoryBytes(
    const MgaRelationStorageDescriptor& descriptor) {
  std::uint64_t bytes = sizeof(descriptor);
  std::uint64_t allocation_bytes = 0;
  const auto account_uuid = [&](const EngineUuid& uuid) {
    return AccountHeapReadOwnedString(uuid.canonical, &bytes);
  };
  if (!account_uuid(descriptor.descriptor_uuid) ||
      !account_uuid(descriptor.database_uuid) ||
      !account_uuid(descriptor.schema_uuid) ||
      !account_uuid(descriptor.relation_uuid) ||
      !account_uuid(descriptor.primary_filespace_uuid) ||
      !AccountHeapReadOwnedString(descriptor.relation_kind, &bytes) ||
      !AccountHeapReadOwnedString(descriptor.storage_profile, &bytes) ||
      !AccountHeapReadOwnedString(descriptor.row_identity_rule, &bytes) ||
      !AccountHeapReadOwnedString(descriptor.version_identity_rule, &bytes) ||
      !AccountHeapReadOwnedString(descriptor.mutation_rule, &bytes) ||
      !AccountHeapReadOwnedString(descriptor.visibility_rule, &bytes) ||
      !AccountHeapReadOwnedString(descriptor.cleanup_rule, &bytes) ||
      !AccountHeapReadOwnedString(descriptor.recovery_rule, &bytes) ||
      !AccountHeapReadOwnedString(descriptor.descriptor_status, &bytes) ||
      !CheckedHeapReadMemoryMultiply(
          static_cast<std::uint64_t>(descriptor.columns.capacity()),
          sizeof(MgaRelationColumnStorageDescriptor), &allocation_bytes) ||
      !CheckedHeapReadMemoryAdd(allocation_bytes, &bytes) ||
      !CheckedHeapReadMemoryMultiply(
          static_cast<std::uint64_t>(descriptor.indexes.capacity()),
          sizeof(MgaRelationIndexStorageDescriptor), &allocation_bytes) ||
      !CheckedHeapReadMemoryAdd(allocation_bytes, &bytes) ||
      !CheckedHeapReadMemoryMultiply(
          static_cast<std::uint64_t>(
              descriptor.required_evidence_kinds.capacity()),
          sizeof(std::string), &allocation_bytes) ||
      !CheckedHeapReadMemoryAdd(allocation_bytes, &bytes)) {
    return std::nullopt;
  }
  for (const auto& column : descriptor.columns) {
    if (!account_uuid(column.column_uuid) ||
        !AccountHeapReadOwnedString(column.canonical_name_key, &bytes) ||
        !AccountHeapReadEngineDescriptorMemory(column.value_descriptor,
                                               &bytes) ||
        !AccountHeapReadOwnedString(column.storage_class, &bytes) ||
        !AccountHeapReadOwnedString(column.charset_uuid, &bytes) ||
        !AccountHeapReadOwnedString(column.collation_uuid, &bytes) ||
        !AccountHeapReadOwnedString(column.overflow_policy, &bytes)) {
      return std::nullopt;
    }
  }
  for (const auto& index : descriptor.indexes) {
    if (!account_uuid(index.index_uuid) ||
        !AccountHeapReadOwnedString(index.family, &bytes) ||
        !AccountHeapReadOwnedString(index.profile, &bytes) ||
        !AccountHeapReadOwnedString(index.predicate_kind, &bytes) ||
        !AccountHeapReadOwnedString(index.predicate_column, &bytes) ||
        !AccountHeapReadOwnedString(index.predicate_value, &bytes) ||
        !AccountHeapReadOwnedString(index.residency_policy, &bytes) ||
        !CheckedHeapReadMemoryMultiply(
            static_cast<std::uint64_t>(index.key_envelopes.capacity()),
            sizeof(std::string), &allocation_bytes) ||
        !CheckedHeapReadMemoryAdd(allocation_bytes, &bytes) ||
        !CheckedHeapReadMemoryMultiply(
            static_cast<std::uint64_t>(index.include_columns.capacity()),
            sizeof(std::string), &allocation_bytes) ||
        !CheckedHeapReadMemoryAdd(allocation_bytes, &bytes)) {
      return std::nullopt;
    }
    for (const auto& value : index.key_envelopes) {
      if (!AccountHeapReadOwnedString(value, &bytes)) return std::nullopt;
    }
    for (const auto& value : index.include_columns) {
      if (!AccountHeapReadOwnedString(value, &bytes)) return std::nullopt;
    }
  }
  for (const auto& value : descriptor.required_evidence_kinds) {
    if (!AccountHeapReadOwnedString(value, &bytes)) return std::nullopt;
  }
  return bytes;
}

std::optional<std::uint64_t> HeapReadTransactionStateMemoryBytes(
    const std::map<std::uint64_t, std::string>& transactions) {
  constexpr std::uint64_t kMapNodeOverhead = 4 * sizeof(void*);
  std::uint64_t bytes = sizeof(transactions);
  std::uint64_t node_bytes = 0;
  if (!CheckedHeapReadMemoryMultiply(
          static_cast<std::uint64_t>(transactions.size()),
          sizeof(std::pair<const std::uint64_t, std::string>) +
              kMapNodeOverhead,
          &node_bytes) ||
      !CheckedHeapReadMemoryAdd(node_bytes, &bytes)) {
    return std::nullopt;
  }
  for (const auto& [transaction, state] : transactions) {
    (void)transaction;
    if (!AccountHeapReadOwnedString(state, &bytes)) return std::nullopt;
  }
  return bytes;
}

std::optional<std::uint64_t> HeapReadSavepointMemoryBytes(
    const SavepointParsedState& savepoints) {
  constexpr std::uint64_t kMapNodeOverhead = 4 * sizeof(void*);
  std::uint64_t bytes = sizeof(savepoints);
  for (const auto& [transaction, names] : savepoints.active_savepoints) {
    (void)transaction;
    std::uint64_t node_bytes =
        sizeof(std::pair<const std::uint64_t,
                         std::map<std::string, SavepointCutoffs>>) +
        kMapNodeOverhead;
    if (!CheckedHeapReadMemoryAdd(node_bytes, &bytes)) return std::nullopt;
    for (const auto& [name, cutoffs] : names) {
      (void)cutoffs;
      node_bytes =
          sizeof(std::pair<const std::string, SavepointCutoffs>) +
          kMapNodeOverhead;
      if (!CheckedHeapReadMemoryAdd(node_bytes, &bytes) ||
          !AccountHeapReadOwnedString(name, &bytes)) {
        return std::nullopt;
      }
    }
  }
  for (const auto& [transaction, ranges] : savepoints.rollback_ranges) {
    (void)transaction;
    std::uint64_t node_bytes =
        sizeof(std::pair<const std::uint64_t,
                         std::vector<SavepointRollbackRange>>) +
        kMapNodeOverhead;
    std::uint64_t allocation_bytes = 0;
    if (!CheckedHeapReadMemoryAdd(node_bytes, &bytes) ||
        !CheckedHeapReadMemoryMultiply(
            static_cast<std::uint64_t>(ranges.capacity()),
            sizeof(SavepointRollbackRange), &allocation_bytes) ||
        !CheckedHeapReadMemoryAdd(allocation_bytes, &bytes)) {
      return std::nullopt;
    }
  }
  for (const auto& [transaction, ranges] :
       savepoints.normalized_row_rollback_ranges) {
    (void)transaction;
    std::uint64_t node_bytes =
        sizeof(std::pair<
                   const std::uint64_t,
                   std::vector<std::pair<std::uint64_t, std::uint64_t>>>) +
        kMapNodeOverhead;
    std::uint64_t allocation_bytes = 0;
    if (!CheckedHeapReadMemoryAdd(node_bytes, &bytes) ||
        !CheckedHeapReadMemoryMultiply(
            static_cast<std::uint64_t>(ranges.capacity()),
            sizeof(std::pair<std::uint64_t, std::uint64_t>),
            &allocation_bytes) ||
        !CheckedHeapReadMemoryAdd(allocation_bytes, &bytes)) {
      return std::nullopt;
    }
  }
  return bytes;
}

std::optional<std::uint64_t> HeapReadVisibilityMapMemoryBytes(
    const std::unordered_map<std::string, std::size_t>& rows) {
  constexpr std::uint64_t kNodeOverhead = 4 * sizeof(void*);
  std::uint64_t bytes = sizeof(rows);
  std::uint64_t allocation_bytes = 0;
  if (!CheckedHeapReadMemoryMultiply(
          static_cast<std::uint64_t>(rows.bucket_count()), sizeof(void*),
          &allocation_bytes) ||
      !CheckedHeapReadMemoryAdd(allocation_bytes, &bytes) ||
      !CheckedHeapReadMemoryMultiply(
          static_cast<std::uint64_t>(rows.size()),
          sizeof(std::pair<const std::string, std::size_t>) + kNodeOverhead,
          &allocation_bytes) ||
      !CheckedHeapReadMemoryAdd(allocation_bytes, &bytes)) {
    return std::nullopt;
  }
  for (const auto& [uuid, ordinal] : rows) {
    (void)ordinal;
    if (!AccountHeapReadOwnedString(uuid, &bytes)) return std::nullopt;
  }
  return bytes;
}

struct PreparedMgaHeapReadAuthorityResult {
  bool ok{false};
  EngineApiDiagnostic diagnostic;
  PreparedMgaHeapReadAuthority authority;
};

EngineApiDiagnostic ValidateMgaHeapTemporaryRelationAuthority(
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

PreparedMgaHeapReadAuthorityCohortResult PrepareMgaHeapReadAuthoritiesImpl(
    const EngineRequestContext& context,
    const std::span<const std::string> relation_uuids,
    const scratchbird::transaction::mga::SnapshotVectorDescriptor*
        resolved_statement_snapshot = nullptr) {
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
        ValidateMgaHeapTemporaryRelationAuthority(context, table);
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

PreparedMgaHeapReadAuthorityResult PrepareMgaHeapReadAuthority(
    const EngineRequestContext& context, const std::string& relation_uuid) {
  PreparedMgaHeapReadAuthorityResult result;
  const std::array<std::string, 1> relations{relation_uuid};
  auto prepared = PrepareMgaHeapReadAuthoritiesImpl(context, relations);
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

MgaVisibleHeapRelationReadResult ReadVisibleMgaHeapRelationWithObservation(
    const EngineRequestContext& context,
    const MgaVisibleHeapRelationReadRequest& request,
    HeapReadRuntimeObservation* runtime_observation,
    const PreparedMgaHeapReadAuthority* prepared_authority) {
  MgaVisibleHeapRelationReadResult result;
  const auto refuse = [&](EngineApiDiagnostic diagnostic,
                          const BoundedScopedRowReadControl* control = nullptr,
                          const MgaHeapReadFailureCategoryV1 category =
                              MgaHeapReadFailureCategoryV1::kInvalidRequest) {
    result.ok = false;
    result.diagnostic = std::move(diagnostic);
    result.failure_category = category;
    if (control != nullptr) {
      result.failure_category =
          control->failure_category != MgaHeapReadFailureCategoryV1::kNone
              ? control->failure_category
              : category != MgaHeapReadFailureCategoryV1::kInvalidRequest
                    ? category
                    : MgaHeapReadFailureCategoryV1::kResource;
    }
    result.descriptor = {};
    std::vector<CrudRowVersionRecord>{}.swap(result.visible_rows);
    std::vector<EngineEvidenceReference>{}.swap(result.evidence);
    result.current_relation_base_generation = 0;
    result.current_live_memory_bytes = 0;
    result.memory_grant_bytes = request.maximum_memory_bytes;
    result.memory_receipt_complete = false;
    if (control != nullptr) {
      result.scanned_row_version_count = control->decoded_row_versions;
      result.decoded_byte_count = control->decoded_bytes;
      result.cancellation_observed = control->cancellation_observed;
      result.peak_live_memory_bytes = control->peak_live_memory_bytes;
    }
    return result;
  };
  const auto invalid = [&](std::string detail,
                           const BoundedScopedRowReadControl* control = nullptr,
                           const MgaHeapReadFailureCategoryV1 category =
                               MgaHeapReadFailureCategoryV1::kInvalidRequest) {
    return refuse(MakeInvalidRequestDiagnostic("mga.heap_relation_read",
                                               std::move(detail)),
                  control, category);
  };

  const auto& cancellation_requested =
      request.borrowed_cancellation_requested == nullptr
          ? request.cancellation_requested
          : *request.borrowed_cancellation_requested;
  const auto& relation_uuid = request.borrowed_relation_uuid == nullptr
                                  ? request.relation_uuid
                                  : *request.borrowed_relation_uuid;

  if (relation_uuid.empty()) {
    return invalid("relation_uuid_required");
  }
  if (context.local_transaction_id == 0 ||
      context.transaction_uuid.canonical.empty()) {
    return invalid("exact_active_transaction_and_snapshot_required", nullptr,
                   MgaHeapReadFailureCategoryV1::kMgaContext);
  }
  if (request.maximum_scanned_row_versions == 0 ||
      request.maximum_decoded_bytes == 0 ||
      request.maximum_output_rows == 0) {
    return invalid("nonzero_heap_read_resource_bounds_required", nullptr,
                   MgaHeapReadFailureCategoryV1::kResource);
  }
  if (!cancellation_requested) {
    return invalid("engine_cancellation_probe_required");
  }
  if (cancellation_requested()) {
    result.cancellation_observed = true;
    return invalid("heap_read_cancelled_before_descriptor_load", nullptr,
                   MgaHeapReadFailureCategoryV1::kCancellation);
  }

  const auto inventory_guard =
      AcquireTransactionInventoryGuard(context.database_path);
  BoundedScopedRowReadControl control;
  control.maximum_row_versions = request.maximum_scanned_row_versions;
  control.maximum_bytes = request.maximum_decoded_bytes;
  control.maximum_memory_bytes = request.maximum_memory_bytes;
  control.cancellation_requested = &cancellation_requested;
  control.runtime_observation = runtime_observation;
  scratchbird::transaction::mga::SnapshotVectorDescriptor owned_snapshot;
  std::map<std::uint64_t, std::string> owned_transaction_states;
  const scratchbird::transaction::mga::SnapshotVectorDescriptor*
      snapshot_vector = nullptr;
  const std::map<std::uint64_t, std::string>* transaction_states = nullptr;
  std::uint64_t current_relation_base_generation = 0;
  if (prepared_authority != nullptr) {
    const auto* prepared_statement = prepared_authority->statement.get();
    if (prepared_statement == nullptr ||
        prepared_statement->transaction_states == nullptr ||
        prepared_authority->relation_uuid != relation_uuid ||
        prepared_statement->database_uuid != context.database_uuid.canonical ||
        prepared_statement->statement_uuid != context.statement_uuid.canonical ||
        prepared_statement->transaction_uuid !=
            context.transaction_uuid.canonical ||
        prepared_statement->statement_snapshot_uuid !=
            context.statement_snapshot_uuid.canonical ||
        prepared_statement->statement_metadata_snapshot_uuid !=
            context.statement_metadata_snapshot_uuid.canonical ||
        prepared_statement->catalog_epoch_uuid !=
            context.catalog_epoch_uuid.canonical ||
        prepared_statement->authorization_authority_uuid !=
            context.authorization_context.authority_uuid.canonical ||
        prepared_statement->catalog_generation !=
            context.catalog_generation_id ||
        prepared_statement->security_epoch !=
            context.authorization_context.security_epoch ||
        prepared_statement->policy_epoch !=
            context.authorization_context.policy_epoch ||
        prepared_statement->resource_epoch != context.resource_epoch ||
        prepared_statement->local_transaction_id !=
            context.local_transaction_id ||
        !prepared_statement->snapshot_vector.inventory_authoritative ||
        !prepared_statement->snapshot_vector.complete) {
      return invalid("prepared_heap_read_authority_is_stale", &control,
                     MgaHeapReadFailureCategoryV1::kMgaContext);
    }
    snapshot_vector = &prepared_statement->snapshot_vector;
    transaction_states = prepared_statement->transaction_states.get();
    current_relation_base_generation =
        prepared_authority->current_relation_base_generation;
  } else {
    EngineResolveStatementSnapshotRequest snapshot_request;
    snapshot_request.context = context;
    auto snapshot = EngineResolveStatementSnapshot(snapshot_request);
    if (!snapshot.ok || !snapshot.snapshot_vector.inventory_authoritative ||
        !snapshot.snapshot_vector.complete) {
      return invalid("exact_current_statement_snapshot_vector_required",
                     nullptr, MgaHeapReadFailureCategoryV1::kMgaContext);
    }
    // Retain only the engine-issued visibility carrier.  Request/result
    // diagnostics and identity strings are catalog-authority transients and
    // must not remain live in the operator-local row-carrier phase.
    owned_snapshot = std::move(snapshot.snapshot_vector);
    snapshot_vector = &owned_snapshot;
    auto loaded_descriptor =
        LoadMgaRelationStorageDescriptor(context, relation_uuid);
    if (!loaded_descriptor.ok) {
      return refuse(std::move(loaded_descriptor.diagnostic), nullptr,
                    MgaHeapReadFailureCategoryV1::kCatalog);
    }
    const auto& loaded = loaded_descriptor.descriptor;
    if (loaded.relation_uuid.canonical != relation_uuid ||
        loaded.database_uuid.canonical != context.database_uuid.canonical ||
        loaded.relation_kind != "table" ||
        loaded.storage_profile != "local_mga_rowstore_v1" ||
        loaded.descriptor_uuid.canonical.empty() ||
        loaded.descriptor_generation == 0 ||
        loaded.descriptor_status.empty()) {
      return invalid("current_persisted_local_heap_descriptor_required",
                     nullptr, MgaHeapReadFailureCategoryV1::kCatalog);
    }
    result.descriptor = std::move(loaded_descriptor.descriptor);
    CrudState metadata;
    const auto metadata_loaded = LoadMgaMetadata(&metadata, context);
    if (metadata_loaded.error) {
      return refuse(metadata_loaded, nullptr,
                    MgaHeapReadFailureCategoryV1::kCatalog);
    }
    metadata.transactions.clear();
    metadata.max_transaction_id = 0;
    const auto authority =
        OverlayMgaTransactionAuthority(context, &metadata, true);
    if (authority.error) {
      return refuse(authority, nullptr,
                    MgaHeapReadFailureCategoryV1::kMgaContext);
    }
    FilterVisibleRetiredTemporaryMetadata(context, &metadata);
    FilterMgaTemporaryObjectsForSession(context, &metadata);
    const auto table = FindVisibleCrudTable(
        metadata, relation_uuid, context.local_transaction_id);
    if (!table) {
      return invalid("heap_relation_not_visible", nullptr,
                     MgaHeapReadFailureCategoryV1::kCatalog);
    }
    const auto temporary_authority =
        ValidateMgaHeapTemporaryRelationAuthority(context, *table);
    if (temporary_authority.error) {
      return refuse(temporary_authority, nullptr,
                    MgaHeapReadFailureCategoryV1::kCatalog);
    }
    current_relation_base_generation = table->event_sequence;
    owned_transaction_states = std::move(metadata.transactions);
    transaction_states = &owned_transaction_states;
  }
  const auto& descriptor = prepared_authority == nullptr
                               ? result.descriptor
                               : prepared_authority->descriptor;
  const auto creator_visible = [&](const std::uint64_t creator) {
    if (creator == 0) return false;
    const auto transaction = transaction_states->find(creator);
    if (transaction == transaction_states->end()) return false;
    if (creator == snapshot_vector->owning_transaction.value) {
      return transaction->second == "active" ||
             transaction->second == "preparing" ||
             transaction->second == "prepared";
    }
    if (transaction->second != "committed" &&
        transaction->second != "archived") {
      return false;
    }
    if (snapshot_vector->visible_committed_high_watermark == 0 ||
        creator > snapshot_vector->visible_committed_high_watermark) {
      return false;
    }
    return !std::binary_search(
               snapshot_vector->active_excluded_local_transaction_ids.begin(),
               snapshot_vector->active_excluded_local_transaction_ids.end(),
               creator) &&
           !std::binary_search(
               snapshot_vector->in_doubt_excluded_local_transaction_ids.begin(),
               snapshot_vector->in_doubt_excluded_local_transaction_ids.end(),
               creator);
  };
  const auto descriptor_memory =
      HeapReadStorageDescriptorMemoryBytes(descriptor);
  const auto transaction_memory = prepared_authority == nullptr
                                      ? HeapReadTransactionStateMemoryBytes(
                                            *transaction_states)
                                      : std::optional<std::uint64_t>{0};
  std::uint64_t authority_memory = sizeof(result);
  std::uint64_t snapshot_vector_bytes = 0;
  if (!descriptor_memory.has_value() || !transaction_memory.has_value() ||
      !CheckedHeapReadMemoryMultiply(
          static_cast<std::uint64_t>(prepared_authority == nullptr
                                         ? snapshot_vector
                                               ->active_excluded_local_transaction_ids
                                               .capacity()
                                         : 0),
          sizeof(std::uint64_t), &snapshot_vector_bytes) ||
      !CheckedHeapReadMemoryAdd(snapshot_vector_bytes, &authority_memory) ||
      !CheckedHeapReadMemoryMultiply(
          static_cast<std::uint64_t>(prepared_authority == nullptr
                                         ? snapshot_vector
                                               ->in_doubt_excluded_local_transaction_ids
                                               .capacity()
                                         : 0),
          sizeof(std::uint64_t), &snapshot_vector_bytes) ||
      !CheckedHeapReadMemoryAdd(snapshot_vector_bytes, &authority_memory) ||
      !CheckedHeapReadMemoryAdd(*descriptor_memory, &authority_memory) ||
      !CheckedHeapReadMemoryAdd(*transaction_memory, &authority_memory)) {
    control.refusal_detail = "heap_read_authority_memory_receipt_overflow";
    return invalid(control.refusal_detail, &control);
  }
  control.retained_parent_memory_bytes = authority_memory;
  if (!ObserveBoundedHeapReadMemory(&control, authority_memory)) {
    return invalid(control.refusal_detail, &control);
  }
  if (prepared_authority != nullptr) {
    result.descriptor = descriptor;
  }
  std::vector<CrudRowVersionRecord> row_versions;
  bool used_segment = false;
  if (!LoadDecodedScopedRowsForTableBounded(context,
                                            relation_uuid,
                                            &control,
                                            &row_versions,
                                            &used_segment)) {
    return invalid(control.refusal_detail.empty()
                       ? "bounded_scoped_heap_read_failed"
                       : control.refusal_detail,
                   &control);
  }
  result.scanned_row_version_count = control.decoded_row_versions;
  result.decoded_byte_count = control.decoded_bytes;
  result.scoped_physical_segment_used = used_segment;

  const auto row_version_memory =
      HeapReadRowVectorMemoryBytes(row_versions);
  if (!row_version_memory.has_value()) {
    control.refusal_detail = "heap_read_visibility_memory_receipt_overflow";
    return invalid(control.refusal_detail, &control);
  }
  std::uint64_t savepoint_parent_memory = authority_memory;
  if (!CheckedHeapReadMemoryAdd(*row_version_memory,
                                &savepoint_parent_memory)) {
    control.refusal_detail = "heap_read_visibility_memory_receipt_overflow";
    return invalid(control.refusal_detail, &control);
  }
  SavepointParsedState savepoints;
  if (!ParseSavepointsBounded(context, &control, savepoint_parent_memory,
                              &savepoints)) {
    return invalid(control.refusal_detail.empty()
                       ? "bounded_savepoint_read_failed"
                       : control.refusal_detail,
                   &control);
  }
  const auto savepoint_memory = HeapReadSavepointMemoryBytes(savepoints);
  std::uint64_t visibility_base_memory = authority_memory;
  if (!savepoint_memory.has_value() ||
      !CheckedHeapReadMemoryAdd(*savepoint_memory,
                                &visibility_base_memory) ||
      !CheckedHeapReadMemoryAdd(*row_version_memory,
                                &visibility_base_memory) ||
      !ObserveBoundedHeapReadMemory(&control, visibility_base_memory)) {
    if (control.refusal_detail.empty()) {
      control.refusal_detail = "heap_read_visibility_memory_receipt_overflow";
    }
    return invalid(control.refusal_detail, &control);
  }
  std::vector<CrudRowVersionRecord> admitted_versions;
  std::uint64_t admitted_reserve_bytes = 0;
  std::uint64_t admitted_preflight_memory = visibility_base_memory;
  if (!CheckedHeapReadMemoryMultiply(
          static_cast<std::uint64_t>(row_versions.size()),
          sizeof(CrudRowVersionRecord), &admitted_reserve_bytes) ||
      !CheckedHeapReadMemoryAdd(sizeof(admitted_versions),
                                &admitted_preflight_memory) ||
      !CheckedHeapReadMemoryAdd(admitted_reserve_bytes,
                                &admitted_preflight_memory) ||
      !ObserveBoundedHeapReadMemory(&control,
                                    admitted_preflight_memory)) {
    if (control.refusal_detail.empty()) {
      control.refusal_detail =
          "heap_read_admission_memory_receipt_overflow";
    }
    return invalid(control.refusal_detail, &control);
  }
  admitted_versions.reserve(row_versions.size());
  for (auto& row : row_versions) {
    if (cancellation_requested()) {
      control.cancellation_observed = true;
      control.refusal_detail = "heap_read_cancelled_during_visibility";
      return invalid(control.refusal_detail, &control);
    }
    if (row.table_uuid != relation_uuid) {
      return invalid("scoped_heap_row_relation_identity_mismatch", &control,
                     MgaHeapReadFailureCategoryV1::kCorruptStorage);
    }
    if (RowEventRolledBackBySavepoint(savepoints,
                                      row.creator_tx,
                                      row.event_sequence)) {
      ++result.invisible_row_version_count;
      continue;
    }
    current_relation_base_generation =
        std::max(current_relation_base_generation, row.event_sequence);
    admitted_versions.push_back(std::move(row));
  }
  {
    const auto current_rows = HeapReadRowVectorMemoryBytes(row_versions);
    const auto admitted_rows =
        HeapReadRowVectorMemoryBytes(admitted_versions);
    std::uint64_t phase_memory = authority_memory;
    if (!current_rows.has_value() || !admitted_rows.has_value() ||
        !CheckedHeapReadMemoryAdd(*savepoint_memory, &phase_memory) ||
        !CheckedHeapReadMemoryAdd(*current_rows, &phase_memory) ||
        !CheckedHeapReadMemoryAdd(*admitted_rows, &phase_memory) ||
        !ObserveBoundedHeapReadMemory(&control, phase_memory)) {
      if (control.refusal_detail.empty()) {
        control.refusal_detail =
            "heap_read_admission_memory_receipt_overflow";
      }
      return invalid(control.refusal_detail, &control);
    }
  }
  {
    const auto current_rows = HeapReadRowVectorMemoryBytes(row_versions);
    const auto admitted_rows =
        HeapReadRowVectorMemoryBytes(admitted_versions);
    const auto chain_index =
        HeapReadVersionIndexProjectionMemoryBytes(admitted_versions);
    std::uint64_t chain_phase_memory = authority_memory;
    if (!current_rows.has_value() || !admitted_rows.has_value() ||
        !chain_index.has_value() ||
        !CheckedHeapReadMemoryAdd(*savepoint_memory,
                                  &chain_phase_memory) ||
        !CheckedHeapReadMemoryAdd(*current_rows, &chain_phase_memory) ||
        !CheckedHeapReadMemoryAdd(*admitted_rows, &chain_phase_memory) ||
        !CheckedHeapReadMemoryAdd(*chain_index, &chain_phase_memory) ||
        !CheckedHeapReadMemoryAdd(512, &chain_phase_memory) ||
        !ObserveBoundedHeapReadMemory(&control, chain_phase_memory)) {
      if (control.refusal_detail.empty()) {
        control.refusal_detail =
            "heap_read_chain_validation_memory_receipt_overflow";
      }
      return invalid(control.refusal_detail, &control);
    }
    const auto chain_status =
        ValidateMgaRowVersionRecordChains(admitted_versions);
    if (chain_status.error) {
      return refuse(chain_status, &control,
                    MgaHeapReadFailureCategoryV1::kCorruptStorage);
    }
  }
  if (RowsContainLargeValueLocators(admitted_versions)) {
    return invalid("large_value_outside_bounded_inline_heap_profile",
                   &control, MgaHeapReadFailureCategoryV1::kCorruptStorage);
  }

  std::unordered_map<std::string, std::size_t> newest_visible_by_row;
  {
    const auto current_rows = HeapReadRowVectorMemoryBytes(row_versions);
    const auto admitted_rows =
        HeapReadRowVectorMemoryBytes(admitted_versions);
    const auto visibility_projection =
        HeapReadVisibilityMapProjectionMemoryBytes(admitted_versions);
    std::uint64_t phase_memory = authority_memory;
    if (!current_rows.has_value() || !admitted_rows.has_value() ||
        !visibility_projection.has_value() ||
        !CheckedHeapReadMemoryAdd(*savepoint_memory, &phase_memory) ||
        !CheckedHeapReadMemoryAdd(*current_rows, &phase_memory) ||
        !CheckedHeapReadMemoryAdd(*admitted_rows, &phase_memory) ||
        !CheckedHeapReadMemoryAdd(*visibility_projection, &phase_memory) ||
        !ObserveBoundedHeapReadMemory(&control, phase_memory)) {
      if (control.refusal_detail.empty()) {
        control.refusal_detail =
            "heap_read_visibility_map_memory_receipt_overflow";
      }
      return invalid(control.refusal_detail, &control);
    }
  }
  newest_visible_by_row.reserve(admitted_versions.size());
  for (std::size_t index = 0; index < admitted_versions.size(); ++index) {
    if (cancellation_requested()) {
      control.cancellation_observed = true;
      control.refusal_detail = "heap_read_cancelled_during_visibility";
      return invalid(control.refusal_detail, &control);
    }
    const auto& row = admitted_versions[index];
    ++result.visibility_recheck_count;
    if ((!row.temporary_session_uuid.empty() &&
         row.temporary_session_uuid != context.session_uuid.canonical) ||
        !creator_visible(row.creator_tx)) {
      ++result.invisible_row_version_count;
      continue;
    }
    const auto found = newest_visible_by_row.find(row.row_uuid);
    if (found == newest_visible_by_row.end() ||
        admitted_versions[found->second].sequence < row.sequence) {
      newest_visible_by_row[row.row_uuid] = index;
    }
  }
  {
    const auto current_rows = HeapReadRowVectorMemoryBytes(row_versions);
    const auto admitted_rows =
        HeapReadRowVectorMemoryBytes(admitted_versions);
    const auto visibility_map =
        HeapReadVisibilityMapMemoryBytes(newest_visible_by_row);
    std::uint64_t phase_memory = authority_memory;
    if (!current_rows.has_value() || !admitted_rows.has_value() ||
        !visibility_map.has_value() ||
        !CheckedHeapReadMemoryAdd(*savepoint_memory, &phase_memory) ||
        !CheckedHeapReadMemoryAdd(*current_rows, &phase_memory) ||
        !CheckedHeapReadMemoryAdd(*admitted_rows, &phase_memory) ||
        !CheckedHeapReadMemoryAdd(*visibility_map, &phase_memory) ||
        !ObserveBoundedHeapReadMemory(&control, phase_memory)) {
      if (control.refusal_detail.empty()) {
        control.refusal_detail =
            "heap_read_visibility_map_memory_receipt_overflow";
      }
      return invalid(control.refusal_detail, &control);
    }
  }

  std::vector<CrudRowVersionRecord> visible_rows;
  std::uint64_t visible_row_count = 0;
  std::uint64_t visible_projection_bytes = sizeof(visible_rows);
  for (std::size_t index = 0; index < admitted_versions.size(); ++index) {
    const auto& row = admitted_versions[index];
    const auto newest = newest_visible_by_row.find(row.row_uuid);
    if (newest == newest_visible_by_row.end() || newest->second != index ||
        row.deleted) {
      continue;
    }
    if (visible_row_count >= request.maximum_output_rows ||
        !AccountHeapReadRowDynamicMemoryBytes(
            row, &visible_projection_bytes)) {
      return invalid(visible_row_count >= request.maximum_output_rows
                         ? "heap_read_maximum_output_rows_exceeded"
                         : "heap_read_publication_memory_receipt_overflow",
                     &control);
    }
    ++visible_row_count;
  }
  std::uint64_t visible_structural_bytes = 0;
  if (!CheckedHeapReadMemoryMultiply(visible_row_count,
                                     sizeof(CrudRowVersionRecord),
                                     &visible_structural_bytes) ||
      !CheckedHeapReadMemoryAdd(visible_structural_bytes,
                                &visible_projection_bytes)) {
    control.refusal_detail = "heap_read_publication_memory_receipt_overflow";
    return invalid(control.refusal_detail, &control);
  }
  {
    const auto current_rows = HeapReadRowVectorMemoryBytes(row_versions);
    const auto admitted_rows =
        HeapReadRowVectorMemoryBytes(admitted_versions);
    const auto visibility_map =
        HeapReadVisibilityMapMemoryBytes(newest_visible_by_row);
    std::uint64_t phase_memory = authority_memory;
    if (!current_rows.has_value() || !admitted_rows.has_value() ||
        !visibility_map.has_value() ||
        !CheckedHeapReadMemoryAdd(*savepoint_memory, &phase_memory) ||
        !CheckedHeapReadMemoryAdd(*current_rows, &phase_memory) ||
        !CheckedHeapReadMemoryAdd(*admitted_rows, &phase_memory) ||
        !CheckedHeapReadMemoryAdd(*visibility_map, &phase_memory) ||
        !CheckedHeapReadMemoryAdd(visible_projection_bytes,
                                  &phase_memory) ||
        !ObserveBoundedHeapReadMemory(&control, phase_memory)) {
      if (control.refusal_detail.empty()) {
        control.refusal_detail =
            "heap_read_publication_memory_receipt_overflow";
      }
      return invalid(control.refusal_detail, &control);
    }
  }
  visible_rows.reserve(static_cast<std::size_t>(visible_row_count));
  for (std::size_t index = 0; index < admitted_versions.size(); ++index) {
    if (cancellation_requested()) {
      control.cancellation_observed = true;
      control.refusal_detail = "heap_read_cancelled_during_publication";
      return invalid(control.refusal_detail, &control);
    }
    const auto& row = admitted_versions[index];
    const auto newest = newest_visible_by_row.find(row.row_uuid);
    if (newest == newest_visible_by_row.end() || newest->second != index) {
      continue;
    }
    if (row.deleted) {
      ++result.tombstone_row_count;
      continue;
    }
    if (visible_rows.size() >= request.maximum_output_rows) {
      return invalid("heap_read_maximum_output_rows_exceeded", &control);
    }
    visible_rows.push_back(row);
  }
  {
    const auto current_rows = HeapReadRowVectorMemoryBytes(row_versions);
    const auto admitted_rows =
        HeapReadRowVectorMemoryBytes(admitted_versions);
    const auto visibility_map =
        HeapReadVisibilityMapMemoryBytes(newest_visible_by_row);
    const auto published_rows =
        HeapReadRowVectorMemoryBytes(visible_rows);
    std::uint64_t phase_memory = authority_memory;
    if (!current_rows.has_value() || !admitted_rows.has_value() ||
        !visibility_map.has_value() || !published_rows.has_value() ||
        !CheckedHeapReadMemoryAdd(*savepoint_memory, &phase_memory) ||
        !CheckedHeapReadMemoryAdd(*current_rows, &phase_memory) ||
        !CheckedHeapReadMemoryAdd(*admitted_rows, &phase_memory) ||
        !CheckedHeapReadMemoryAdd(*visibility_map, &phase_memory) ||
        !CheckedHeapReadMemoryAdd(*published_rows, &phase_memory) ||
        !ObserveBoundedHeapReadMemory(&control, phase_memory)) {
      if (control.refusal_detail.empty()) {
        control.refusal_detail =
            "heap_read_publication_memory_receipt_overflow";
      }
      return invalid(control.refusal_detail, &control);
    }
  }

  {
    const auto current_rows = HeapReadRowVectorMemoryBytes(row_versions);
    const auto admitted_rows =
        HeapReadRowVectorMemoryBytes(admitted_versions);
    const auto visibility_map =
        HeapReadVisibilityMapMemoryBytes(newest_visible_by_row);
    const auto published_rows = HeapReadRowVectorMemoryBytes(visible_rows);
    std::uint64_t publication_phase_memory = authority_memory;
    constexpr std::uint64_t kResultPublicationEnvelopeBytes =
        6 * sizeof(EngineEvidenceReference) + 2048;
    if (!current_rows.has_value() || !admitted_rows.has_value() ||
        !visibility_map.has_value() || !published_rows.has_value() ||
        !CheckedHeapReadMemoryAdd(*savepoint_memory,
                                  &publication_phase_memory) ||
        !CheckedHeapReadMemoryAdd(*current_rows,
                                  &publication_phase_memory) ||
        !CheckedHeapReadMemoryAdd(*admitted_rows,
                                  &publication_phase_memory) ||
        !CheckedHeapReadMemoryAdd(*visibility_map,
                                  &publication_phase_memory) ||
        !CheckedHeapReadMemoryAdd(*published_rows,
                                  &publication_phase_memory) ||
        !CheckedHeapReadMemoryAdd(kResultPublicationEnvelopeBytes,
                                  &publication_phase_memory) ||
        !ObserveBoundedHeapReadMemory(&control,
                                      publication_phase_memory)) {
      if (control.refusal_detail.empty()) {
        control.refusal_detail =
            "heap_read_result_memory_receipt_overflow";
      }
      return invalid(control.refusal_detail, &control);
    }
  }
  result.evidence.reserve(6);
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  result.visible_rows = std::move(visible_rows);
  result.current_relation_base_generation = current_relation_base_generation;
  result.evidence.push_back(
      {"mga_heap_read_relation_descriptor_uuid",
       descriptor.descriptor_uuid.canonical});
  result.evidence.push_back(
      {"mga_heap_read_relation_descriptor_generation",
       std::to_string(descriptor.descriptor_generation)});
  result.evidence.push_back(
      {"mga_heap_read_relation_base_generation",
       std::to_string(result.current_relation_base_generation)});
  result.evidence.push_back(
      {"mga_heap_read_storage_route", "bounded_scoped_physical_segment"});
  result.evidence.push_back(
      {"mga_heap_read_visibility_authority",
       "durable_transaction_inventory_statement_snapshot"});
  result.evidence.push_back(
      {"mga_heap_read_parser_or_candidate_authority", "false"});
  const auto current_rows =
      HeapReadRowVectorMemoryBytes(result.visible_rows);
  std::uint64_t evidence_bytes = 0;
  std::uint64_t evidence_allocation_bytes = 0;
  if (!current_rows.has_value() ||
      !CheckedHeapReadMemoryMultiply(
          static_cast<std::uint64_t>(result.evidence.capacity()),
          sizeof(EngineEvidenceReference), &evidence_allocation_bytes) ||
      !CheckedHeapReadMemoryAdd(evidence_allocation_bytes,
                                &evidence_bytes)) {
    control.refusal_detail = "heap_read_result_memory_receipt_overflow";
    return invalid(control.refusal_detail, &control);
  }
  for (const auto& evidence : result.evidence) {
    if (!AccountHeapReadOwnedString(evidence.evidence_kind,
                                    &evidence_bytes) ||
        !AccountHeapReadOwnedString(evidence.evidence_id,
                                    &evidence_bytes)) {
      control.refusal_detail = "heap_read_result_memory_receipt_overflow";
      return invalid(control.refusal_detail, &control);
    }
  }
  std::uint64_t current_memory = sizeof(result);
  if (!CheckedHeapReadMemoryAdd(*descriptor_memory, &current_memory) ||
      !CheckedHeapReadMemoryAdd(*current_rows, &current_memory) ||
      !CheckedHeapReadMemoryAdd(evidence_bytes, &current_memory)) {
    control.refusal_detail = "heap_read_result_memory_receipt_overflow";
    return invalid(control.refusal_detail, &control);
  }
  const auto retained_rows = HeapReadRowVectorMemoryBytes(row_versions);
  const auto admitted_rows =
      HeapReadRowVectorMemoryBytes(admitted_versions);
  const auto visibility_map =
      HeapReadVisibilityMapMemoryBytes(newest_visible_by_row);
  std::uint64_t final_phase_memory = authority_memory;
  if (!retained_rows.has_value() || !admitted_rows.has_value() ||
      !visibility_map.has_value() ||
      !CheckedHeapReadMemoryAdd(*savepoint_memory, &final_phase_memory) ||
      !CheckedHeapReadMemoryAdd(*retained_rows, &final_phase_memory) ||
      !CheckedHeapReadMemoryAdd(*admitted_rows, &final_phase_memory) ||
      !CheckedHeapReadMemoryAdd(*visibility_map, &final_phase_memory) ||
      !CheckedHeapReadMemoryAdd(*current_rows, &final_phase_memory) ||
      !CheckedHeapReadMemoryAdd(evidence_bytes, &final_phase_memory) ||
      !ObserveBoundedHeapReadMemory(&control, final_phase_memory)) {
    if (control.refusal_detail.empty()) {
      control.refusal_detail = "heap_read_final_memory_receipt_overflow";
    }
    return invalid(control.refusal_detail, &control);
  }
  result.current_live_memory_bytes = current_memory;
  result.peak_live_memory_bytes = control.peak_live_memory_bytes;
  result.memory_grant_bytes = request.maximum_memory_bytes;
  result.memory_receipt_complete =
      request.maximum_memory_bytes != 0 &&
      result.current_live_memory_bytes <= result.peak_live_memory_bytes &&
      result.peak_live_memory_bytes <= result.memory_grant_bytes;
  if (request.maximum_memory_bytes != 0 &&
      !result.memory_receipt_complete) {
    control.refusal_detail = "heap_read_memory_receipt_incomplete";
    return invalid(control.refusal_detail, &control);
  }
  return result;
}

// QOW-SOURCE-QRY-004-HEAP-MGA-V1
MgaVisibleHeapRelationReadResult ReadVisibleMgaHeapRelation(
    const EngineRequestContext& context,
    const MgaVisibleHeapRelationReadRequest& request) {
  return ReadVisibleMgaHeapRelationWithObservation(context, request, nullptr);
}

namespace {

constexpr std::uint32_t kStreamingCountNoFallback =
    std::numeric_limits<std::uint32_t>::max();
constexpr std::uint64_t kStreamingCountScratchBytes = 128 * 1024;
constexpr std::uint32_t kStreamingCountMaximumMetadataStringBytes =
    64 * 1024;

struct StreamingVisibleSelection {
  std::vector<std::uint8_t> visible_source_ordinals;
  std::string text_path;
  std::string binary_path;
  std::uint64_t text_bytes = 0;
  std::uint64_t binary_bytes = 0;
};

struct StreamingCountIdentity {
  std::array<std::uint8_t, 16> canonical_bytes{};
  std::uint32_t fallback_ordinal = kStreamingCountNoFallback;
};

struct StreamingCountRowVersion {
  StreamingCountIdentity row_uuid;
  StreamingCountIdentity version_uuid;
  StreamingCountIdentity previous_version_uuid;
  std::uint64_t creator_tx = 0;
  std::uint64_t event_sequence = 0;
  std::uint64_t previous_sequence = 0;
  std::uint64_t source_ordinal = 0;
  bool has_previous_version = false;
  bool deleted = false;
  bool temporary_session_visible = false;
  bool creator_visible = false;
};

bool StreamingCountIdentityLess(
    const StreamingCountIdentity& left,
    const StreamingCountIdentity& right,
    const std::vector<std::string>& fallbacks) {
  const bool left_canonical =
      left.fallback_ordinal == kStreamingCountNoFallback;
  const bool right_canonical =
      right.fallback_ordinal == kStreamingCountNoFallback;
  if (left_canonical != right_canonical) return left_canonical;
  if (left_canonical) {
    return std::lexicographical_compare(
        left.canonical_bytes.begin(), left.canonical_bytes.end(),
        right.canonical_bytes.begin(), right.canonical_bytes.end());
  }
  if (left.fallback_ordinal >= fallbacks.size() ||
      right.fallback_ordinal >= fallbacks.size()) {
    return left.fallback_ordinal < right.fallback_ordinal;
  }
  return fallbacks[left.fallback_ordinal] <
         fallbacks[right.fallback_ordinal];
}

bool StreamingCountIdentityEqual(
    const StreamingCountIdentity& left,
    const StreamingCountIdentity& right,
    const std::vector<std::string>& fallbacks) {
  return !StreamingCountIdentityLess(left, right, fallbacks) &&
         !StreamingCountIdentityLess(right, left, fallbacks);
}

std::optional<std::uint64_t> StreamingCountMetadataMemoryBytes(
    const std::vector<StreamingCountRowVersion>& rows,
    const std::vector<std::string>& fallback_identities,
    const SavepointParsedState& savepoints,
    const MgaRelationStorageDescriptor& descriptor,
    const std::uint64_t transient_string_bytes = 0) {
  std::uint64_t bytes = sizeof(MgaVisibleHeapRelationCountResult) +
                        sizeof(rows) + sizeof(fallback_identities) +
                        kStreamingCountScratchBytes;
  std::uint64_t allocation_bytes = 0;
  const auto descriptor_bytes =
      HeapReadStorageDescriptorMemoryBytes(descriptor);
  const auto savepoint_bytes = HeapReadSavepointMemoryBytes(savepoints);
  if (!descriptor_bytes.has_value() || !savepoint_bytes.has_value() ||
      !CheckedHeapReadMemoryMultiply(
          static_cast<std::uint64_t>(rows.capacity()),
          sizeof(StreamingCountRowVersion), &allocation_bytes) ||
      !CheckedHeapReadMemoryAdd(allocation_bytes, &bytes) ||
      !CheckedHeapReadMemoryMultiply(
          static_cast<std::uint64_t>(fallback_identities.capacity()),
          sizeof(std::string), &allocation_bytes) ||
      !CheckedHeapReadMemoryAdd(allocation_bytes, &bytes) ||
      !CheckedHeapReadMemoryAdd(*descriptor_bytes, &bytes) ||
      !CheckedHeapReadMemoryAdd(*savepoint_bytes, &bytes) ||
      !CheckedHeapReadMemoryAdd(transient_string_bytes, &bytes)) {
    return std::nullopt;
  }
  for (const auto& identity : fallback_identities) {
    if (!AccountHeapReadOwnedString(identity, &bytes)) return std::nullopt;
  }
  return bytes;
}

bool ObserveStreamingCountMemory(
    const std::vector<StreamingCountRowVersion>& rows,
    const std::vector<std::string>& fallback_identities,
    const SavepointParsedState& savepoints,
    const MgaRelationStorageDescriptor& descriptor,
    const std::uint64_t maximum_memory_bytes,
    std::uint64_t* peak_memory_bytes,
    std::string* detail,
    const std::uint64_t transient_string_bytes = 0) {
  const auto live = StreamingCountMetadataMemoryBytes(
      rows, fallback_identities, savepoints, descriptor,
      transient_string_bytes);
  if (!live.has_value()) {
    if (detail != nullptr) {
      *detail = "heap_count_memory_receipt_overflow";
    }
    return false;
  }
  if (peak_memory_bytes != nullptr) {
    *peak_memory_bytes = std::max(*peak_memory_bytes, *live);
  }
  if (maximum_memory_bytes == 0 || *live > maximum_memory_bytes) {
    if (detail != nullptr) {
      *detail = "heap_count_maximum_memory_bytes_exceeded";
    }
    return false;
  }
  return true;
}

bool ReserveStreamingCountRows(
    const std::uint64_t additional_rows,
    std::vector<StreamingCountRowVersion>* rows,
    const std::vector<std::string>& fallback_identities,
    const SavepointParsedState& savepoints,
    const MgaRelationStorageDescriptor& descriptor,
    const std::uint64_t maximum_memory_bytes,
    std::uint64_t* peak_memory_bytes,
    std::string* detail) {
  if (rows == nullptr ||
      additional_rows > std::numeric_limits<std::size_t>::max() ||
      static_cast<std::size_t>(additional_rows) >
          std::numeric_limits<std::size_t>::max() - rows->size()) {
    if (detail != nullptr) *detail = "heap_count_row_metadata_overflow";
    return false;
  }
  const auto required = rows->size() +
                        static_cast<std::size_t>(additional_rows);
  if (required > rows->capacity()) {
    std::uint64_t projected_bytes = 0;
    std::uint64_t structural_bytes = 0;
    const auto descriptor_bytes =
        HeapReadStorageDescriptorMemoryBytes(descriptor);
    const auto savepoint_bytes = HeapReadSavepointMemoryBytes(savepoints);
    if (!descriptor_bytes.has_value() || !savepoint_bytes.has_value() ||
        !CheckedHeapReadMemoryMultiply(required,
                                       sizeof(StreamingCountRowVersion),
                                       &structural_bytes) ||
        !CheckedHeapReadMemoryAdd(
            sizeof(MgaVisibleHeapRelationCountResult) + sizeof(*rows) +
                sizeof(fallback_identities) + kStreamingCountScratchBytes,
            &projected_bytes) ||
        !CheckedHeapReadMemoryAdd(structural_bytes, &projected_bytes) ||
        !CheckedHeapReadMemoryAdd(*descriptor_bytes, &projected_bytes) ||
        !CheckedHeapReadMemoryAdd(*savepoint_bytes, &projected_bytes)) {
      if (detail != nullptr) *detail = "heap_count_memory_receipt_overflow";
      return false;
    }
    for (const auto& identity : fallback_identities) {
      if (!AccountHeapReadOwnedString(identity, &projected_bytes)) {
        if (detail != nullptr) *detail = "heap_count_memory_receipt_overflow";
        return false;
      }
    }
    if (projected_bytes > maximum_memory_bytes) {
      if (detail != nullptr) {
        *detail = "heap_count_maximum_memory_bytes_exceeded";
      }
      return false;
    }
    rows->reserve(required);
  }
  return ObserveStreamingCountMemory(
      *rows, fallback_identities, savepoints, descriptor,
      maximum_memory_bytes, peak_memory_bytes, detail);
}

bool ParseStreamingCountIdentity(
    const std::string_view text,
    std::vector<std::string>* fallback_identities,
    StreamingCountIdentity* identity,
    const bool allow_empty,
    const SavepointParsedState& savepoints,
    const MgaRelationStorageDescriptor& descriptor,
    const std::vector<StreamingCountRowVersion>& rows,
    const std::uint64_t maximum_memory_bytes,
    std::uint64_t* peak_memory_bytes,
    std::string* detail) {
  if (fallback_identities == nullptr || identity == nullptr ||
      (!allow_empty && text.empty())) {
    if (detail != nullptr) *detail = "heap_count_row_identity_invalid";
    return false;
  }
  if (text.empty()) {
    *identity = {};
    return true;
  }
  const auto parsed = scratchbird::core::uuid::ParseUuid(std::string(text));
  if (parsed.ok()) {
    std::copy(parsed.value.bytes.begin(), parsed.value.bytes.end(),
              identity->canonical_bytes.begin());
    identity->fallback_ordinal = kStreamingCountNoFallback;
    return true;
  }
  if (fallback_identities->size() >= kStreamingCountNoFallback ||
      text.size() > kStreamingCountMaximumMetadataStringBytes) {
    if (detail != nullptr) *detail = "heap_count_row_identity_invalid";
    return false;
  }
  const std::uint64_t transient =
      static_cast<std::uint64_t>(text.size()) + 1 + sizeof(std::string);
  if (!ObserveStreamingCountMemory(
          rows, *fallback_identities, savepoints, descriptor,
          maximum_memory_bytes, peak_memory_bytes, detail, transient)) {
    return false;
  }
  fallback_identities->emplace_back(text);
  identity->fallback_ordinal = static_cast<std::uint32_t>(
      fallback_identities->size() - 1);
  return ObserveStreamingCountMemory(
      rows, *fallback_identities, savepoints, descriptor,
      maximum_memory_bytes, peak_memory_bytes, detail);
}

class StreamingCountBinaryReader {
 public:
  StreamingCountBinaryReader(
      std::string path, const std::uint64_t authorized_bytes,
      const std::uint64_t maximum_decoded_bytes,
      const std::function<bool()>* cancellation_requested,
      MgaVisibleHeapRelationCountResult* result,
      HeapReadRuntimeObservation* runtime_observation,
      std::string* detail)
      : path_(std::move(path)),
        remaining_(authorized_bytes),
        physical_remaining_(authorized_bytes),
        maximum_decoded_bytes_(maximum_decoded_bytes),
        cancellation_requested_(cancellation_requested),
        result_(result),
        runtime_observation_(runtime_observation),
        detail_(detail) {}

  bool Open() {
    const auto started = std::chrono::steady_clock::now();
    input_.open(path_, std::ios::binary);
    ObserveWait(started);
    if (!input_) return Fail("heap_count_scoped_binary_open_failed");
    return true;
  }

  bool ReadU8(std::uint8_t* value) {
    return ReadExact(reinterpret_cast<char*>(value), 1);
  }

  bool ReadU16(std::uint16_t* value) {
    std::array<std::uint8_t, 2> bytes{};
    if (!ReadExact(reinterpret_cast<char*>(bytes.data()), bytes.size())) {
      return false;
    }
    *value = static_cast<std::uint16_t>(bytes[0]) |
             (static_cast<std::uint16_t>(bytes[1]) << 8U);
    return true;
  }

  bool ReadU32(std::uint32_t* value) {
    std::array<std::uint8_t, 4> bytes{};
    if (!ReadExact(reinterpret_cast<char*>(bytes.data()), bytes.size())) {
      return false;
    }
    *value = 0;
    for (std::size_t index = 0; index < bytes.size(); ++index) {
      *value |= static_cast<std::uint32_t>(bytes[index]) << (index * 8U);
    }
    return true;
  }

  bool ReadU64(std::uint64_t* value) {
    std::array<std::uint8_t, 8> bytes{};
    if (!ReadExact(reinterpret_cast<char*>(bytes.data()), bytes.size())) {
      return false;
    }
    *value = 0;
    for (std::size_t index = 0; index < bytes.size(); ++index) {
      *value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8U);
    }
    return true;
  }

  bool ReadUuid(StreamingCountIdentity* identity) {
    if (identity == nullptr) return Fail("heap_count_row_identity_invalid");
    identity->fallback_ordinal = kStreamingCountNoFallback;
    return ReadExact(
        reinterpret_cast<char*>(identity->canonical_bytes.data()),
        identity->canonical_bytes.size());
  }

  bool ReadUuidText(std::string* value) {
    if (value == nullptr) return Fail("heap_stream_row_identity_invalid");
    scratchbird::core::platform::Uuid uuid;
    if (!ReadExact(reinterpret_cast<char*>(uuid.bytes.data()),
                   uuid.bytes.size())) {
      return false;
    }
    *value = scratchbird::core::uuid::UuidToString(uuid);
    return true;
  }

  bool ReadString(std::string* value, const bool allow_empty) {
    std::uint32_t size = 0;
    if (value == nullptr || !ReadU32(&size) || size > remaining_ ||
        size > kStreamingCountMaximumMetadataStringBytes ||
        (!allow_empty && size == 0)) {
      return Fail("heap_count_binary_string_invalid");
    }
    value->resize(size);
    return size == 0 || ReadExact(value->data(), size);
  }

  bool SkipString(const bool allow_empty) {
    std::uint32_t size = 0;
    return ReadU32(&size) && size <= remaining_ &&
           size <= kStreamingCountMaximumMetadataStringBytes &&
           (allow_empty || size != 0) && Skip(size);
  }

  bool SkipPayloadString() {
    std::uint32_t size = 0;
    return ReadU32(&size) && size <= remaining_ && Skip(size);
  }

  bool ReadPayloadString(std::string* value,
                         const std::uint64_t maximum_payload_bytes) {
    std::uint32_t size = 0;
    if (value == nullptr || !ReadU32(&size) || size > remaining_ ||
        size > maximum_payload_bytes) {
      return Fail("heap_stream_binary_payload_invalid");
    }
    value->resize(size);
    return size == 0 || ReadExact(value->data(), size);
  }

  bool ReadFixedPayload(std::string* value,
                        const std::size_t size,
                        const std::uint64_t maximum_payload_bytes) {
    if (value == nullptr || size > maximum_payload_bytes ||
        size > remaining_) {
      return Fail("heap_stream_binary_payload_invalid");
    }
    value->resize(size);
    return size == 0 || ReadExact(value->data(), size);
  }

  bool Skip(const std::uint64_t bytes) {
    if (bytes > remaining_) return Fail("heap_count_binary_truncated");
    std::array<char, 64 * 1024> scratch{};
    std::uint64_t remaining = bytes;
    while (remaining != 0) {
      const auto chunk = static_cast<std::size_t>(
          std::min<std::uint64_t>(remaining, scratch.size()));
      if (!ReadExact(scratch.data(), chunk)) return false;
      remaining -= chunk;
    }
    return true;
  }

  bool Finish() {
    if (remaining_ != 0) return Fail("heap_count_binary_trailing_bytes");
    const auto started = std::chrono::steady_clock::now();
    const auto next = input_.peek();
    ObserveWait(started);
    if (next != std::char_traits<char>::eof()) {
      return Fail("heap_count_scoped_binary_grew_during_read");
    }
    return !input_.bad() || Fail("heap_count_scoped_binary_read_failed");
  }

  [[nodiscard]] std::uint64_t remaining() const { return remaining_; }
  [[nodiscard]] bool cancellation_observed() const {
    return cancellation_observed_;
  }

 private:
  bool ReadExact(char* destination, const std::size_t bytes) {
    if (destination == nullptr || bytes > remaining_) {
      return Fail("heap_count_binary_truncated");
    }
    std::size_t copied = 0;
    while (copied != bytes) {
      if (buffer_offset_ == buffer_size_ && !FillBuffer()) return false;
      const auto available = buffer_size_ - buffer_offset_;
      const auto chunk = std::min(available, bytes - copied);
      std::memcpy(destination + copied, buffer_.data() + buffer_offset_,
                  chunk);
      buffer_offset_ += chunk;
      copied += chunk;
      remaining_ -= chunk;
      if (result_ == nullptr ||
          result_->decoded_byte_count >
              std::numeric_limits<std::uint64_t>::max() - chunk) {
        return Fail("heap_count_byte_counter_overflow");
      }
      result_->decoded_byte_count += chunk;
      if (result_->decoded_byte_count > maximum_decoded_bytes_) {
        return Fail("heap_count_maximum_decoded_bytes_exceeded");
      }
    }
    return true;
  }

  bool FillBuffer() {
    if (physical_remaining_ == 0) {
      return Fail("heap_count_binary_truncated");
    }
    if (Cancelled()) return false;
    const auto chunk = static_cast<std::size_t>(
        std::min<std::uint64_t>(physical_remaining_, buffer_.size()));
    const auto started = std::chrono::steady_clock::now();
    input_.read(buffer_.data(), static_cast<std::streamsize>(chunk));
    ObserveWait(started);
    if (input_.gcount() != static_cast<std::streamsize>(chunk) ||
        input_.bad()) {
      return Fail("heap_count_scoped_binary_read_failed");
    }
    if (result_ == nullptr ||
        result_->storage_bytes_read >
            std::numeric_limits<std::uint64_t>::max() - chunk) {
      return Fail("heap_count_byte_counter_overflow");
    }
    result_->storage_bytes_read += chunk;
    physical_remaining_ -= chunk;
    buffer_offset_ = 0;
    buffer_size_ = chunk;
    return true;
  }

  bool Cancelled() {
    if (cancellation_requested_ != nullptr && *cancellation_requested_ &&
        (*cancellation_requested_)()) {
      cancellation_observed_ = true;
      if (result_ != nullptr) result_->cancellation_observed = true;
      return Fail("heap_count_cancelled_during_physical_read");
    }
    return false;
  }

  bool Fail(const std::string_view detail) {
    if (detail_ != nullptr && detail_->empty()) detail_->assign(detail);
    return false;
  }

  void ObserveWait(const std::chrono::steady_clock::time_point started) {
    if (runtime_observation_ == nullptr) return;
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - started).count();
    if (elapsed < 0 ||
        static_cast<std::uintmax_t>(elapsed) >
            std::numeric_limits<std::uint64_t>::max() ||
        runtime_observation_->operator_wait_ns >
            std::numeric_limits<std::uint64_t>::max() -
                static_cast<std::uint64_t>(elapsed)) {
      runtime_observation_->complete = false;
      return;
    }
    runtime_observation_->operator_wait_ns +=
        static_cast<std::uint64_t>(elapsed);
  }

  std::string path_;
  std::ifstream input_;
  std::uint64_t remaining_ = 0;
  std::uint64_t physical_remaining_ = 0;
  std::array<char, 64 * 1024> buffer_{};
  std::size_t buffer_offset_ = 0;
  std::size_t buffer_size_ = 0;
  std::uint64_t maximum_decoded_bytes_ = 0;
  const std::function<bool()>* cancellation_requested_ = nullptr;
  MgaVisibleHeapRelationCountResult* result_ = nullptr;
  HeapReadRuntimeObservation* runtime_observation_ = nullptr;
  std::string* detail_ = nullptr;
  bool cancellation_observed_ = false;
};

bool StreamingCountCreatorVisible(
    const std::uint64_t creator,
    const scratchbird::transaction::mga::SnapshotVectorDescriptor& snapshot,
    const std::map<std::uint64_t, std::string>& transaction_states) {
  if (creator == 0) return false;
  const auto transaction = transaction_states.find(creator);
  if (transaction == transaction_states.end()) return false;
  if (creator == snapshot.owning_transaction.value) {
    return transaction->second == "active" ||
           transaction->second == "preparing" ||
           transaction->second == "prepared";
  }
  if (transaction->second != "committed" &&
      transaction->second != "archived") {
    return false;
  }
  if (snapshot.visible_committed_high_watermark == 0 ||
      creator > snapshot.visible_committed_high_watermark) {
    return false;
  }
  return !std::binary_search(
             snapshot.active_excluded_local_transaction_ids.begin(),
             snapshot.active_excluded_local_transaction_ids.end(), creator) &&
         !std::binary_search(
             snapshot.in_doubt_excluded_local_transaction_ids.begin(),
             snapshot.in_doubt_excluded_local_transaction_ids.end(), creator);
}

bool AppendStreamingCountRow(
    StreamingCountRowVersion row,
    const std::string_view table_uuid,
    const std::string_view required_relation_uuid,
    const SavepointParsedState& savepoints,
    const MgaRelationStorageDescriptor& descriptor,
    std::vector<StreamingCountRowVersion>* rows,
    const std::vector<std::string>& fallback_identities,
    const std::uint64_t maximum_memory_bytes,
    std::uint64_t* peak_memory_bytes,
    MgaVisibleHeapRelationCountResult* result,
    std::string* detail) {
  if (rows == nullptr || result == nullptr ||
      table_uuid != required_relation_uuid) {
    if (detail != nullptr) {
      *detail = "scoped_heap_row_relation_identity_mismatch";
    }
    return false;
  }
  if (result->scanned_row_version_count ==
      std::numeric_limits<std::uint64_t>::max()) {
    if (detail != nullptr) *detail = "heap_count_row_counter_overflow";
    return false;
  }
  ++result->scanned_row_version_count;
  row.source_ordinal = result->scanned_row_version_count;
  if (RowEventRolledBackBySavepoint(savepoints, row.creator_tx,
                                    row.event_sequence)) {
    if (result->invisible_row_version_count ==
        std::numeric_limits<std::uint64_t>::max()) {
      if (detail != nullptr) *detail = "heap_count_row_counter_overflow";
      return false;
    }
    ++result->invisible_row_version_count;
    return true;
  }
  result->current_relation_base_generation = std::max(
      result->current_relation_base_generation, row.event_sequence);
  if (!ReserveStreamingCountRows(
          1, rows, fallback_identities, savepoints, descriptor,
          maximum_memory_bytes, peak_memory_bytes, detail)) {
    return false;
  }
  rows->push_back(std::move(row));
  return ObserveStreamingCountMemory(
      *rows, fallback_identities, savepoints, descriptor,
      maximum_memory_bytes, peak_memory_bytes, detail);
}

bool DecodeStreamingCountBinaryFile(
    const std::string& path,
    const std::uint64_t authorized_file_bytes,
    const std::string& relation_uuid,
    const std::string& session_uuid,
    const scratchbird::transaction::mga::SnapshotVectorDescriptor& snapshot,
    const std::map<std::uint64_t, std::string>& transaction_states,
    const SavepointParsedState& savepoints,
    const MgaRelationStorageDescriptor& descriptor,
    const std::uint64_t maximum_decoded_bytes,
    const std::uint64_t maximum_memory_bytes,
    const std::function<bool()>* cancellation_requested,
    std::vector<StreamingCountRowVersion>* rows,
    std::vector<std::string>* fallback_identities,
    std::uint64_t* peak_memory_bytes,
    MgaVisibleHeapRelationCountResult* result,
    HeapReadRuntimeObservation* runtime_observation,
    std::string* detail) {
  if (rows == nullptr || fallback_identities == nullptr || result == nullptr ||
      detail == nullptr) {
    return false;
  }
  StreamingCountBinaryReader reader(
      path, authorized_file_bytes, maximum_decoded_bytes,
      cancellation_requested, result, runtime_observation, detail);
  if (!reader.Open()) return false;
  while (reader.remaining() != 0) {
    std::array<char, 8> magic{};
    std::uint16_t version = 0;
    std::uint16_t flags = 0;
    std::uint32_t column_count = 0;
    std::uint64_t row_count = 0;
    if (!reader.Skip(0) ||
        !reader.ReadU8(reinterpret_cast<std::uint8_t*>(&magic[0])) ||
        !reader.ReadU8(reinterpret_cast<std::uint8_t*>(&magic[1])) ||
        !reader.ReadU8(reinterpret_cast<std::uint8_t*>(&magic[2])) ||
        !reader.ReadU8(reinterpret_cast<std::uint8_t*>(&magic[3])) ||
        !reader.ReadU8(reinterpret_cast<std::uint8_t*>(&magic[4])) ||
        !reader.ReadU8(reinterpret_cast<std::uint8_t*>(&magic[5])) ||
        !reader.ReadU8(reinterpret_cast<std::uint8_t*>(&magic[6])) ||
        !reader.ReadU8(reinterpret_cast<std::uint8_t*>(&magic[7])) ||
        std::string_view(magic.data(), magic.size()) !=
            kScopedRowBinaryBatchMagic ||
        !reader.ReadU16(&version) || !reader.ReadU16(&flags) ||
        !reader.ReadU32(&column_count) || !reader.ReadU64(&row_count) ||
        (version != 1 && version != kScopedRowBinaryLegacyTypedVersion &&
         version != kScopedRowBinaryVersion &&
         version != kScopedRowBinaryNativePacketVersion) ||
        flags != 0 || column_count == 0 || column_count > 4096) {
      if (detail->empty()) *detail = "heap_count_scoped_binary_header_invalid";
      return false;
    }
    for (std::uint32_t column = 0; column < column_count; ++column) {
      if (!reader.SkipString(false)) return false;
    }
    std::vector<std::uint8_t> native_tags;
    if (version == kScopedRowBinaryNativePacketVersion) {
      native_tags.reserve(column_count);
      for (std::uint32_t column = 0; column < column_count; ++column) {
        std::uint8_t tag = 0;
        if (!reader.ReadU8(&tag) ||
            ScopedRowNativePacketTypeName(tag).empty()) {
          if (detail->empty()) {
            *detail = "heap_count_scoped_binary_type_invalid";
          }
          return false;
        }
        native_tags.push_back(tag);
      }
    } else if (version >= kScopedRowBinaryLegacyTypedVersion) {
      for (std::uint32_t column = 0; column < column_count; ++column) {
        if (!reader.SkipString(false)) return false;
      }
    }

    const bool compact_batch = version >= kScopedRowBinaryVersion;
    std::uint64_t compact_first_event_sequence = 0;
    std::uint64_t compact_creator_tx = 0;
    std::string compact_table_uuid;
    std::string compact_temporary_session_uuid;
    std::uint8_t compact_flags = 0;
    if (compact_batch &&
        (!reader.ReadU64(&compact_first_event_sequence) ||
         !reader.ReadU64(&compact_creator_tx) ||
         !reader.ReadString(&compact_table_uuid, false) ||
         !reader.ReadString(&compact_temporary_session_uuid, true) ||
         !reader.ReadU8(&compact_flags) || compact_flags != 0 ||
         compact_table_uuid != relation_uuid ||
         (row_count != 0 &&
          row_count - 1 > std::numeric_limits<std::uint64_t>::max() -
                              compact_first_event_sequence))) {
      if (detail->empty()) {
        *detail = "heap_count_scoped_binary_compact_header_invalid";
      }
      return false;
    }
    const std::uint64_t null_bitmap_bytes =
        (static_cast<std::uint64_t>(column_count) + 7U) / 8U;
    const std::uint64_t minimum_row_bytes =
        (compact_batch ? 32U : 45U) + null_bitmap_bytes;
    if (minimum_row_bytes == 0 ||
        row_count > reader.remaining() / minimum_row_bytes ||
        !ReserveStreamingCountRows(
            row_count, rows, *fallback_identities, savepoints, descriptor,
            maximum_memory_bytes, peak_memory_bytes, detail)) {
      if (detail->empty()) *detail = "heap_count_row_count_exceeds_segment";
      return false;
    }
    std::vector<std::uint8_t> null_bitmap(
        static_cast<std::size_t>(null_bitmap_bytes));
    for (std::uint64_t row_index = 0; row_index < row_count; ++row_index) {
      StreamingCountRowVersion row;
      std::string row_table_uuid;
      std::string row_uuid;
      std::string version_uuid;
      std::string previous_version_uuid;
      std::string temporary_session_uuid;
      std::uint8_t deleted = 0;
      if (compact_batch) {
        row.creator_tx = compact_creator_tx;
        row.event_sequence = compact_first_event_sequence + row_index;
        row_table_uuid = compact_table_uuid;
        row.temporary_session_visible =
            compact_temporary_session_uuid.empty() ||
            compact_temporary_session_uuid == session_uuid;
        if (!reader.ReadUuid(&row.row_uuid) ||
            !reader.ReadUuid(&row.version_uuid)) {
          return false;
        }
      } else {
        if (!reader.ReadU64(&row.creator_tx) ||
            !reader.ReadU64(&row.event_sequence) ||
            !reader.ReadU64(&row.previous_sequence) ||
            !reader.ReadU8(&deleted) ||
            !reader.ReadString(&row_table_uuid, false) ||
            !reader.ReadString(&row_uuid, false) ||
            !reader.ReadString(&version_uuid, false) ||
            !reader.ReadString(&previous_version_uuid, true) ||
            !reader.ReadString(&temporary_session_uuid, true) ||
            !ParseStreamingCountIdentity(
                row_uuid, fallback_identities, &row.row_uuid, false,
                savepoints, descriptor, *rows, maximum_memory_bytes,
                peak_memory_bytes, detail) ||
            !ParseStreamingCountIdentity(
                version_uuid, fallback_identities, &row.version_uuid, false,
                savepoints, descriptor, *rows, maximum_memory_bytes,
                peak_memory_bytes, detail)) {
          return false;
        }
        row.deleted = deleted != 0;
        row.has_previous_version = !previous_version_uuid.empty();
        if (row.has_previous_version &&
            !ParseStreamingCountIdentity(
                previous_version_uuid, fallback_identities,
                &row.previous_version_uuid, false, savepoints, descriptor,
                *rows, maximum_memory_bytes, peak_memory_bytes, detail)) {
          return false;
        }
        row.temporary_session_visible = temporary_session_uuid.empty() ||
                                        temporary_session_uuid == session_uuid;
      }
      row.creator_visible = StreamingCountCreatorVisible(
          row.creator_tx, snapshot, transaction_states);
      for (std::size_t byte = 0; byte < null_bitmap.size(); ++byte) {
        if (!reader.ReadU8(&null_bitmap[byte])) return false;
      }
      for (std::uint32_t column = 0; column < column_count; ++column) {
        const bool is_null =
            (null_bitmap[column / 8U] &
             static_cast<std::uint8_t>(1U << (column % 8U))) != 0;
        if (is_null) continue;
        if (version == kScopedRowBinaryNativePacketVersion) {
          const auto tag = native_tags[column];
          if ((tag == 3 && !reader.Skip(1)) ||
              (tag == 4 && !reader.Skip(4)) ||
              ((tag == 2 || tag == 5 || tag == 6) && !reader.Skip(8)) ||
              ((tag == 1 || tag == 7) && !reader.SkipPayloadString())) {
            return false;
          }
        } else if (!reader.SkipPayloadString()) {
          return false;
        }
      }
      if (!AppendStreamingCountRow(
              std::move(row), row_table_uuid, relation_uuid, savepoints,
              descriptor, rows, *fallback_identities,
              maximum_memory_bytes, peak_memory_bytes, result, detail)) {
        return false;
      }
    }
  }
  if (!reader.Finish()) {
    result->cancellation_observed = reader.cancellation_observed();
    return false;
  }
  return true;
}

std::vector<std::string_view> StreamingCountTextFields(
    const std::string& line) {
  std::vector<std::string_view> fields;
  fields.reserve(12);
  std::size_t begin = 0;
  while (begin <= line.size()) {
    const auto end = line.find('\t', begin);
    fields.emplace_back(line.data() + begin,
                        end == std::string::npos ? line.size() - begin
                                                 : end - begin);
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  return fields;
}

bool DecodeStreamingCountTextFile(
    const std::string& path,
    const std::uint64_t authorized_file_bytes,
    const std::string& relation_uuid,
    const std::string& session_uuid,
    const scratchbird::transaction::mga::SnapshotVectorDescriptor& snapshot,
    const std::map<std::uint64_t, std::string>& transaction_states,
    const SavepointParsedState& savepoints,
    const MgaRelationStorageDescriptor& descriptor,
    const std::uint64_t maximum_decoded_bytes,
    const std::uint64_t maximum_memory_bytes,
    const std::function<bool()>* cancellation_requested,
    std::vector<StreamingCountRowVersion>* rows,
    std::vector<std::string>* fallback_identities,
    std::uint64_t* peak_memory_bytes,
    MgaVisibleHeapRelationCountResult* result,
    HeapReadRuntimeObservation* runtime_observation,
    std::string* detail) {
  if (rows == nullptr || fallback_identities == nullptr || result == nullptr ||
      detail == nullptr) {
    return false;
  }
  const auto open_started = std::chrono::steady_clock::now();
  std::ifstream input(path, std::ios::binary);
  const auto observe_wait = [&](const auto started) {
    if (runtime_observation == nullptr) return;
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - started).count();
    if (elapsed < 0 ||
        static_cast<std::uintmax_t>(elapsed) >
            std::numeric_limits<std::uint64_t>::max() ||
        runtime_observation->operator_wait_ns >
            std::numeric_limits<std::uint64_t>::max() -
                static_cast<std::uint64_t>(elapsed)) {
      runtime_observation->complete = false;
      return;
    }
    runtime_observation->operator_wait_ns +=
        static_cast<std::uint64_t>(elapsed);
  };
  observe_wait(open_started);
  if (!input) {
    *detail = "heap_count_scoped_text_open_failed";
    return false;
  }
  // Retain only fields 0..9 and the optional temporary-session field. Field
  // 10 is the encoded value payload and can be arbitrarily large; COUNT(*)
  // must validate and count row-version metadata without materializing it.
  std::string line_metadata;
  std::uint32_t line_tab_count = 0;
  std::array<char, 64 * 1024> chunk{};
  std::uint64_t remaining = authorized_file_bytes;
  const auto consume_line = [&](const std::string& current) {
    const auto fields = StreamingCountTextFields(current);
    if (fields.size() < 11 || fields[0] != kRowStoreMagic ||
        fields[1] != "ROW_VERSION") {
      return true;
    }
    StreamingCountRowVersion row;
    row.creator_tx = ParseU64(std::string(fields[2]));
    row.event_sequence = ParseU64(std::string(fields[3]));
    row.deleted = fields[7] == "1";
    row.previous_sequence = ParseU64(std::string(fields[9]));
    row.has_previous_version = !fields[8].empty();
    row.temporary_session_visible =
        fields.size() < 12 || fields[11].empty() ||
        fields[11] == session_uuid;
    row.creator_visible = StreamingCountCreatorVisible(
        row.creator_tx, snapshot, transaction_states);
    if (!ParseStreamingCountIdentity(
            fields[5], fallback_identities, &row.row_uuid, false,
            savepoints, descriptor, *rows, maximum_memory_bytes,
            peak_memory_bytes, detail) ||
        !ParseStreamingCountIdentity(
            fields[6], fallback_identities, &row.version_uuid, false,
            savepoints, descriptor, *rows, maximum_memory_bytes,
            peak_memory_bytes, detail) ||
        (row.has_previous_version &&
         !ParseStreamingCountIdentity(
             fields[8], fallback_identities, &row.previous_version_uuid,
             false, savepoints, descriptor, *rows, maximum_memory_bytes,
             peak_memory_bytes, detail))) {
      return false;
    }
    return AppendStreamingCountRow(
        std::move(row), fields[4], relation_uuid, savepoints, descriptor,
        rows, *fallback_identities, maximum_memory_bytes,
        peak_memory_bytes, result, detail);
  };
  while (remaining != 0) {
    if (cancellation_requested != nullptr && *cancellation_requested &&
        (*cancellation_requested)()) {
      result->cancellation_observed = true;
      *detail = "heap_count_cancelled_during_physical_read";
      return false;
    }
    const auto requested = static_cast<std::size_t>(
        std::min<std::uint64_t>(remaining, chunk.size()));
    const auto read_started = std::chrono::steady_clock::now();
    input.read(chunk.data(), static_cast<std::streamsize>(requested));
    observe_wait(read_started);
    if (input.gcount() != static_cast<std::streamsize>(requested) ||
        input.bad()) {
      *detail = "heap_count_scoped_text_read_failed";
      return false;
    }
    remaining -= requested;
    if (result->storage_bytes_read >
            std::numeric_limits<std::uint64_t>::max() - requested ||
        result->decoded_byte_count >
            std::numeric_limits<std::uint64_t>::max() - requested) {
      *detail = "heap_count_byte_counter_overflow";
      return false;
    }
    result->storage_bytes_read += requested;
    result->decoded_byte_count += requested;
    if (result->decoded_byte_count > maximum_decoded_bytes) {
      *detail = "heap_count_maximum_decoded_bytes_exceeded";
      return false;
    }
    for (std::size_t index = 0; index < requested; ++index) {
      const char value = chunk[index];
      if (value == '\n') {
        if (!ObserveStreamingCountMemory(
                *rows, *fallback_identities, savepoints, descriptor,
                maximum_memory_bytes, peak_memory_bytes, detail,
                static_cast<std::uint64_t>(line_metadata.capacity()) + 1) ||
            !consume_line(line_metadata)) {
          return false;
        }
        line_metadata.clear();
        line_tab_count = 0;
        continue;
      }
      const bool payload_byte = line_tab_count == 10 && value != '\t';
      if (payload_byte) continue;
      if (line_metadata.size() >=
          kStreamingCountMaximumMetadataStringBytes) {
        *detail = "heap_count_text_record_exceeds_metadata_bound";
        return false;
      }
      line_metadata.push_back(value);
      if (value == '\t' &&
          line_tab_count != std::numeric_limits<std::uint32_t>::max()) {
        ++line_tab_count;
      }
      if ((index & 4095U) == 0 &&
          !ObserveStreamingCountMemory(
              *rows, *fallback_identities, savepoints, descriptor,
              maximum_memory_bytes, peak_memory_bytes, detail,
              static_cast<std::uint64_t>(line_metadata.capacity()) + 1)) {
        return false;
      }
    }
  }
  if (!line_metadata.empty() && !consume_line(line_metadata)) return false;
  const auto peek_started = std::chrono::steady_clock::now();
  const auto next = input.peek();
  observe_wait(peek_started);
  if (next != std::char_traits<char>::eof()) {
    *detail = "heap_count_scoped_text_grew_during_read";
    return false;
  }
  return !input.bad();
}

bool ValidateAndCountStreamingRows(
    std::vector<StreamingCountRowVersion>* rows,
    const std::vector<std::string>& fallback_identities,
    const std::function<bool()>* cancellation_requested,
    MgaVisibleHeapRelationCountResult* result,
    std::string* detail,
    std::vector<std::uint8_t>* visible_source_ordinals = nullptr) {
  if (rows == nullptr || result == nullptr || detail == nullptr) return false;
  const auto cancelled = [&](const std::string_view phase) {
    if (cancellation_requested != nullptr && *cancellation_requested &&
        (*cancellation_requested)()) {
      result->cancellation_observed = true;
      *detail = "heap_count_cancelled_" + std::string(phase);
      return true;
    }
    return false;
  };
  if (cancelled("before_chain_validation")) return false;
  const auto version_less = [&](const auto& left, const auto& right) {
    if (StreamingCountIdentityLess(left.version_uuid, right.version_uuid,
                                   fallback_identities)) {
      return true;
    }
    if (StreamingCountIdentityLess(right.version_uuid, left.version_uuid,
                                   fallback_identities)) {
      return false;
    }
    return left.source_ordinal < right.source_ordinal;
  };
  std::sort(rows->begin(), rows->end(), version_less);
  for (std::size_t index = 0; index < rows->size(); ++index) {
    if ((index & 1023U) == 0 && cancelled("during_chain_validation")) {
      return false;
    }
    const auto& row = (*rows)[index];
    if (index != 0 && StreamingCountIdentityEqual(
                          (*rows)[index - 1].version_uuid, row.version_uuid,
                          fallback_identities)) {
      *detail = "duplicate_row_version_uuid";
      return false;
    }
    if (!row.has_previous_version) continue;
    const auto previous = std::lower_bound(
        rows->begin(), rows->end(), row,
        [&](const auto& candidate, const auto& sought) {
          return StreamingCountIdentityLess(
              candidate.version_uuid, sought.previous_version_uuid,
              fallback_identities);
        });
    if (previous == rows->end() ||
        !StreamingCountIdentityEqual(
            previous->version_uuid, row.previous_version_uuid,
            fallback_identities)) {
      *detail = "previous_row_version_missing";
      return false;
    }
    if (!StreamingCountIdentityEqual(previous->row_uuid, row.row_uuid,
                                     fallback_identities)) {
      *detail = "previous_row_version_wrong_chain";
      return false;
    }
    if (previous->event_sequence >= row.event_sequence) {
      *detail = "previous_row_version_not_older";
      return false;
    }
    if (row.previous_sequence != 0 &&
        previous->event_sequence != row.previous_sequence) {
      *detail = "previous_row_version_sequence_mismatch";
      return false;
    }
  }
  if (cancelled("before_visibility_projection")) return false;
  const auto row_less = [&](const auto& left, const auto& right) {
    if (StreamingCountIdentityLess(left.row_uuid, right.row_uuid,
                                   fallback_identities)) {
      return true;
    }
    if (StreamingCountIdentityLess(right.row_uuid, left.row_uuid,
                                   fallback_identities)) {
      return false;
    }
    if (left.event_sequence != right.event_sequence) {
      return left.event_sequence < right.event_sequence;
    }
    return left.source_ordinal < right.source_ordinal;
  };
  std::sort(rows->begin(), rows->end(), row_less);
  std::size_t begin = 0;
  while (begin < rows->size()) {
    if ((begin & 1023U) == 0 && cancelled("during_visibility_projection")) {
      return false;
    }
    std::size_t end = begin + 1;
    while (end < rows->size() &&
           StreamingCountIdentityEqual((*rows)[begin].row_uuid,
                                       (*rows)[end].row_uuid,
                                       fallback_identities)) {
      ++end;
    }
    const StreamingCountRowVersion* newest_visible = nullptr;
    for (std::size_t index = begin; index < end; ++index) {
      if (result->visibility_recheck_count ==
          std::numeric_limits<std::uint64_t>::max()) {
        *detail = "heap_count_visibility_counter_overflow";
        return false;
      }
      ++result->visibility_recheck_count;
      const auto& row = (*rows)[index];
      if (!row.temporary_session_visible || !row.creator_visible) {
        if (result->invisible_row_version_count ==
            std::numeric_limits<std::uint64_t>::max()) {
          *detail = "heap_count_visibility_counter_overflow";
          return false;
        }
        ++result->invisible_row_version_count;
        continue;
      }
      if (newest_visible == nullptr ||
          newest_visible->event_sequence < row.event_sequence) {
        newest_visible = &row;
      }
    }
    if (newest_visible != nullptr) {
      if (newest_visible->deleted) {
        if (result->tombstone_row_count ==
            std::numeric_limits<std::uint64_t>::max()) {
          *detail = "heap_count_tombstone_counter_overflow";
          return false;
        }
        ++result->tombstone_row_count;
      } else {
        if (result->visible_row_count ==
            std::numeric_limits<std::uint64_t>::max()) {
          *detail = "heap_count_result_overflow";
          return false;
        }
        ++result->visible_row_count;
        if (visible_source_ordinals != nullptr) {
          if (newest_visible->source_ordinal == 0 ||
              newest_visible->source_ordinal >=
                  visible_source_ordinals->size()) {
            *detail = "heap_count_visible_source_ordinal_invalid";
            return false;
          }
          (*visible_source_ordinals)[newest_visible->source_ordinal] = 1;
        }
      }
    }
    begin = end;
  }
  return !cancelled("before_publication");
}

bool ObserveVisibleStreamMemory(
    const MgaVisibleHeapRelationStreamRequest& request,
    const MgaRelationStorageDescriptor& descriptor,
    const StreamingVisibleSelection& selection,
    const CrudRowVersionRecord* transient_row,
    const std::uint64_t consumer_retained_bytes,
    const std::uint64_t prospective_consumer_growth_bytes,
    std::uint64_t* peak_memory_bytes,
    std::string* detail) {
  if (peak_memory_bytes == nullptr || detail == nullptr) return false;
  const auto descriptor_bytes =
      HeapReadStorageDescriptorMemoryBytes(descriptor);
  std::uint64_t live = sizeof(MgaVisibleHeapRelationStreamResult) +
                       sizeof(selection) +
                       static_cast<std::uint64_t>(
                           selection.visible_source_ordinals.capacity()) +
                       kStreamingCountScratchBytes;
  if (!descriptor_bytes.has_value() ||
      !CheckedHeapReadMemoryAdd(*descriptor_bytes, &live) ||
      !AccountHeapReadOwnedString(selection.text_path, &live) ||
      !AccountHeapReadOwnedString(selection.binary_path, &live) ||
      !CheckedHeapReadMemoryAdd(consumer_retained_bytes, &live) ||
      !CheckedHeapReadMemoryAdd(prospective_consumer_growth_bytes, &live)) {
    *detail = "heap_stream_memory_receipt_overflow";
    return false;
  }
  if (transient_row != nullptr) {
    std::uint64_t row_bytes = sizeof(CrudRowVersionRecord);
    if (!AccountHeapReadRowDynamicMemoryBytes(*transient_row, &row_bytes) ||
        !CheckedHeapReadMemoryAdd(row_bytes, &live)) {
      *detail = "heap_stream_memory_receipt_overflow";
      return false;
    }
  }
  *peak_memory_bytes = std::max(*peak_memory_bytes, live);
  if (request.maximum_memory_bytes == 0 ||
      live > request.maximum_memory_bytes) {
    *detail = "heap_stream_maximum_memory_bytes_exceeded";
    return false;
  }
  return true;
}

bool StreamConsumerMemory(
    const MgaVisibleHeapRelationStreamRequest& request,
    std::uint64_t* bytes,
    std::string* detail) {
  if (bytes == nullptr || detail == nullptr ||
      !request.consumer_retained_memory_bytes) {
    if (detail != nullptr) *detail = "heap_stream_consumer_receipt_required";
    return false;
  }
  try {
    *bytes = request.consumer_retained_memory_bytes();
    return true;
  } catch (...) {
    *detail = "heap_stream_consumer_receipt_probe_failed";
    return false;
  }
}

bool VisibleStreamDeliveryBoundReached(
    const MgaVisibleHeapRelationStreamRequest& request,
    const MgaVisibleHeapRelationStreamResult& result) {
  return request.maximum_delivered_visible_rows.has_value() &&
         result.delivered_row_count >=
             *request.maximum_delivered_visible_rows;
}

bool PublishVisibleStreamSecondPassCounters(
    const MgaVisibleHeapRelationCountResult& phase,
    MgaVisibleHeapRelationStreamResult* result,
    std::string* detail) {
  if (result == nullptr || detail == nullptr ||
      phase.storage_bytes_read >
          std::numeric_limits<std::uint64_t>::max() -
              result->storage_bytes_read ||
      phase.decoded_byte_count >
          std::numeric_limits<std::uint64_t>::max() -
              result->decoded_byte_count ||
      phase.storage_bytes_read >
          std::numeric_limits<std::uint64_t>::max() -
              result->second_pass_storage_bytes_read ||
      phase.decoded_byte_count >
          std::numeric_limits<std::uint64_t>::max() -
              result->second_pass_decoded_byte_count) {
    if (detail != nullptr) *detail = "heap_stream_second_pass_counter_overflow";
    return false;
  }
  result->storage_bytes_read += phase.storage_bytes_read;
  result->decoded_byte_count += phase.decoded_byte_count;
  result->second_pass_storage_bytes_read += phase.storage_bytes_read;
  result->second_pass_decoded_byte_count += phase.decoded_byte_count;
  return true;
}

bool DeliverVisibleStreamRow(
    const MgaVisibleHeapRelationStreamRequest& request,
    const MgaRelationStorageDescriptor& descriptor,
    const StreamingVisibleSelection& selection,
    const std::uint64_t source_ordinal,
    const CrudRowVersionRecord& row,
    MgaVisibleHeapRelationStreamResult* result,
    std::string* detail) {
  if (result == nullptr || detail == nullptr ||
      !request.consume_visible_row) {
    if (detail != nullptr) *detail = "heap_stream_consumer_required";
    return false;
  }
  std::uint64_t before = 0;
  if (!StreamConsumerMemory(request, &before, detail) ||
      !ObserveVisibleStreamMemory(
          request, descriptor, selection, &row, before,
          request.maximum_consumer_growth_bytes_per_row,
          &result->peak_live_memory_bytes, detail)) {
    return false;
  }
  bool accepted = false;
  try {
    accepted = request.consume_visible_row(source_ordinal, row);
  } catch (...) {
    *detail = "heap_stream_consumer_threw";
    return false;
  }
  if (!accepted) {
    *detail = "heap_stream_consumer_refused_row";
    return false;
  }
  std::uint64_t after = 0;
  if (!StreamConsumerMemory(request, &after, detail) || after < before ||
      after - before > request.maximum_consumer_growth_bytes_per_row ||
      !ObserveVisibleStreamMemory(
          request, descriptor, selection, nullptr, after, 0,
          &result->peak_live_memory_bytes, detail)) {
    if (detail->empty()) *detail = "heap_stream_consumer_receipt_invalid";
    return false;
  }
  if (result->delivered_row_count ==
      std::numeric_limits<std::uint64_t>::max()) {
    *detail = "heap_stream_delivered_row_count_overflow";
    return false;
  }
  ++result->delivered_row_count;
  const auto& cancellation_requested =
      request.borrowed_cancellation_requested == nullptr
          ? request.cancellation_requested
          : *request.borrowed_cancellation_requested;
  try {
    if (cancellation_requested && cancellation_requested()) {
      result->cancellation_observed = true;
      *detail = "heap_stream_cancelled_after_row_delivery";
      return false;
    }
  } catch (...) {
    *detail = "heap_stream_cancellation_probe_failed";
    return false;
  }
  return true;
}

bool DecodeVisibleStreamTextFile(
    const MgaVisibleHeapRelationStreamRequest& request,
    const std::string& relation_uuid,
    const StreamingVisibleSelection& selection,
    std::uint64_t* source_ordinal,
    MgaVisibleHeapRelationStreamResult* result,
    std::string* detail) {
  if (source_ordinal == nullptr || result == nullptr || detail == nullptr ||
      selection.text_bytes == 0) {
    return selection.text_bytes == 0;
  }
  MgaVisibleHeapRelationCountResult phase;
  StreamingCountBinaryReader reader(
      selection.text_path, selection.text_bytes,
      request.maximum_decoded_bytes_per_pass,
      request.borrowed_cancellation_requested == nullptr
          ? &request.cancellation_requested
          : request.borrowed_cancellation_requested,
      &phase, nullptr, detail);
  if (!reader.Open()) return false;
  std::string line;
  const auto consume_line = [&]() {
    const auto fields = StreamingCountTextFields(line);
    if (fields.size() < 11 || fields[0] != kRowStoreMagic ||
        fields[1] != "ROW_VERSION") {
      return true;
    }
    if (*source_ordinal == std::numeric_limits<std::uint64_t>::max()) {
      *detail = "heap_stream_source_ordinal_overflow";
      return false;
    }
    ++*source_ordinal;
    if (*source_ordinal >= selection.visible_source_ordinals.size()) {
      *detail = "heap_stream_source_ordinal_out_of_range";
      return false;
    }
    if (fields[4] != relation_uuid) {
      *detail = "heap_stream_relation_identity_mismatch";
      return false;
    }
    if (selection.visible_source_ordinals[*source_ordinal] == 0) return true;
    CrudRowVersionRecord row;
    row.creator_tx = ParseU64(std::string(fields[2]));
    row.event_sequence = ParseU64(std::string(fields[3]));
    row.sequence = row.event_sequence;
    row.table_uuid.assign(fields[4]);
    row.row_uuid.assign(fields[5]);
    row.version_uuid.assign(fields[6]);
    row.deleted = fields[7] == "1";
    row.previous_version_uuid.assign(fields[8]);
    row.previous_sequence = ParseU64(std::string(fields[9]));
    row.values = DecodeCrudPairsWithKeyCache(std::string(fields[10]), nullptr);
    if (fields.size() >= 12) row.temporary_session_uuid.assign(fields[11]);
    return DeliverVisibleStreamRow(request, result->descriptor, selection,
                                   *source_ordinal, row, result, detail);
  };
  while (reader.remaining() != 0) {
    std::uint8_t byte = 0;
    if (!reader.ReadU8(&byte)) return false;
    if (byte == '\n') {
      if (!consume_line()) return false;
      line.clear();
      if (VisibleStreamDeliveryBoundReached(request, *result)) {
        return PublishVisibleStreamSecondPassCounters(phase, result, detail);
      }
      continue;
    }
    std::uint64_t consumer_bytes = 0;
    if (!StreamConsumerMemory(request, &consumer_bytes, detail) ||
        line.size() >= request.maximum_memory_bytes ||
        !ObserveVisibleStreamMemory(
            request, result->descriptor, selection, nullptr,
            consumer_bytes + static_cast<std::uint64_t>(line.size()) + 2,
            0, &result->peak_live_memory_bytes, detail)) {
      if (detail->empty()) *detail = "heap_stream_text_record_exceeds_memory_grant";
      return false;
    }
    line.push_back(static_cast<char>(byte));
  }
  if (!line.empty() && !consume_line()) return false;
  if (!reader.Finish()) {
    result->cancellation_observed = reader.cancellation_observed();
    return false;
  }
  return PublishVisibleStreamSecondPassCounters(phase, result, detail);
}

bool DecodeVisibleStreamBinaryFile(
    const MgaVisibleHeapRelationStreamRequest& request,
    const std::string& relation_uuid,
    const StreamingVisibleSelection& selection,
    std::uint64_t* source_ordinal,
    MgaVisibleHeapRelationStreamResult* result,
    std::string* detail) {
  if (source_ordinal == nullptr || result == nullptr || detail == nullptr ||
      selection.binary_bytes == 0) {
    return selection.binary_bytes == 0;
  }
  MgaVisibleHeapRelationCountResult phase;
  StreamingCountBinaryReader reader(
      selection.binary_path, selection.binary_bytes,
      request.maximum_decoded_bytes_per_pass,
      request.borrowed_cancellation_requested == nullptr
          ? &request.cancellation_requested
          : request.borrowed_cancellation_requested,
      &phase, nullptr, detail);
  if (!reader.Open()) return false;
  while (reader.remaining() != 0) {
    std::array<char, 8> magic{};
    std::uint16_t version = 0;
    std::uint16_t flags = 0;
    std::uint32_t column_count = 0;
    std::uint64_t row_count = 0;
    for (auto& byte : magic) {
      if (!reader.ReadU8(reinterpret_cast<std::uint8_t*>(&byte))) return false;
    }
    if (std::string_view(magic.data(), magic.size()) !=
            kScopedRowBinaryBatchMagic ||
        !reader.ReadU16(&version) || !reader.ReadU16(&flags) ||
        !reader.ReadU32(&column_count) || !reader.ReadU64(&row_count) ||
        (version != 1 && version != kScopedRowBinaryLegacyTypedVersion &&
         version != kScopedRowBinaryVersion &&
         version != kScopedRowBinaryNativePacketVersion) ||
        flags != 0 || column_count == 0 || column_count > 4096) {
      *detail = "heap_stream_scoped_binary_header_invalid";
      return false;
    }
    std::vector<std::string> field_order;
    field_order.reserve(column_count);
    for (std::uint32_t column = 0; column < column_count; ++column) {
      std::string field;
      if (!reader.ReadString(&field, false)) return false;
      field_order.push_back(std::move(field));
    }
    std::vector<std::string> field_types;
    std::vector<std::uint8_t> native_tags;
    field_types.reserve(column_count);
    native_tags.reserve(column_count);
    if (version == kScopedRowBinaryNativePacketVersion) {
      for (std::uint32_t column = 0; column < column_count; ++column) {
        std::uint8_t tag = 0;
        const auto name = reader.ReadU8(&tag)
                              ? ScopedRowNativePacketTypeName(tag)
                              : std::string_view{};
        if (name.empty()) {
          *detail = "heap_stream_scoped_binary_type_invalid";
          return false;
        }
        native_tags.push_back(tag);
        field_types.emplace_back(name);
      }
    } else if (version >= kScopedRowBinaryLegacyTypedVersion) {
      for (std::uint32_t column = 0; column < column_count; ++column) {
        std::string type;
        if (!reader.ReadString(&type, false)) return false;
        field_types.push_back(std::move(type));
      }
    } else {
      field_types.assign(column_count, "text");
    }
    const bool compact = version >= kScopedRowBinaryVersion;
    std::uint64_t compact_first_sequence = 0;
    std::uint64_t compact_creator_tx = 0;
    std::string compact_table_uuid;
    std::string compact_session_uuid;
    std::uint8_t compact_flags = 0;
    if (compact &&
        (!reader.ReadU64(&compact_first_sequence) ||
         !reader.ReadU64(&compact_creator_tx) ||
         !reader.ReadString(&compact_table_uuid, false) ||
         !reader.ReadString(&compact_session_uuid, true) ||
         !reader.ReadU8(&compact_flags) || compact_flags != 0 ||
         compact_table_uuid != relation_uuid ||
         (row_count != 0 &&
          row_count - 1 > std::numeric_limits<std::uint64_t>::max() -
                              compact_first_sequence))) {
      *detail = "heap_stream_scoped_binary_compact_header_invalid";
      return false;
    }
    const std::uint64_t bitmap_bytes =
        (static_cast<std::uint64_t>(column_count) + 7U) / 8U;
    if (bitmap_bytes > std::numeric_limits<std::size_t>::max()) {
      *detail = "heap_stream_null_bitmap_overflow";
      return false;
    }
    std::vector<std::uint8_t> null_bitmap(
        static_cast<std::size_t>(bitmap_bytes));
    for (std::uint64_t row_index = 0; row_index < row_count; ++row_index) {
      if (*source_ordinal == std::numeric_limits<std::uint64_t>::max()) {
        *detail = "heap_stream_source_ordinal_overflow";
        return false;
      }
      ++*source_ordinal;
      if (*source_ordinal >= selection.visible_source_ordinals.size()) {
        *detail = "heap_stream_source_ordinal_out_of_range";
        return false;
      }
      const bool selected =
          selection.visible_source_ordinals[*source_ordinal] != 0;
      CrudRowVersionRecord row;
      std::uint8_t deleted = 0;
      if (compact) {
        row.creator_tx = compact_creator_tx;
        row.event_sequence = compact_first_sequence + row_index;
        row.sequence = row.event_sequence;
        row.table_uuid = compact_table_uuid;
        row.temporary_session_uuid = compact_session_uuid;
        if (selected) {
          if (!reader.ReadUuidText(&row.row_uuid) ||
              !reader.ReadUuidText(&row.version_uuid)) {
            return false;
          }
        } else if (!reader.Skip(32)) {
          return false;
        }
      } else {
        std::string table_uuid;
        std::string row_uuid;
        std::string version_uuid;
        std::string previous_uuid;
        std::string session_uuid;
        if (!reader.ReadU64(&row.creator_tx) ||
            !reader.ReadU64(&row.event_sequence) ||
            !reader.ReadU64(&row.previous_sequence) ||
            !reader.ReadU8(&deleted) ||
            !reader.ReadString(&table_uuid, false) ||
            !reader.ReadString(&row_uuid, false) ||
            !reader.ReadString(&version_uuid, false) ||
            !reader.ReadString(&previous_uuid, true) ||
            !reader.ReadString(&session_uuid, true) ||
            table_uuid != relation_uuid) {
          *detail = "heap_stream_scoped_binary_row_header_invalid";
          return false;
        }
        row.sequence = row.event_sequence;
        row.deleted = deleted != 0;
        if (selected) {
          row.table_uuid = std::move(table_uuid);
          row.row_uuid = std::move(row_uuid);
          row.version_uuid = std::move(version_uuid);
          row.previous_version_uuid = std::move(previous_uuid);
          row.temporary_session_uuid = std::move(session_uuid);
        }
      }
      for (auto& byte : null_bitmap) {
        if (!reader.ReadU8(&byte)) return false;
      }
      if (selected) row.values.reserve(column_count);
      for (std::uint32_t column = 0; column < column_count; ++column) {
        const bool is_null =
            (null_bitmap[column / 8U] &
             static_cast<std::uint8_t>(1U << (column % 8U))) != 0;
        if (is_null) {
          if (selected) row.values.push_back({field_order[column], "<NULL>"});
          continue;
        }
        if (!selected) {
          if (version == kScopedRowBinaryNativePacketVersion) {
            const auto tag = native_tags[column];
            if ((tag == 3 && !reader.Skip(1)) ||
                (tag == 4 && !reader.Skip(4)) ||
                ((tag == 2 || tag == 5 || tag == 6) && !reader.Skip(8)) ||
                ((tag == 1 || tag == 7) && !reader.SkipPayloadString())) {
              return false;
            }
          } else if (!reader.SkipPayloadString()) {
            return false;
          }
          continue;
        }
        std::string payload;
        if (version == kScopedRowBinaryNativePacketVersion) {
          const auto tag = native_tags[column];
          const std::size_t fixed =
              tag == 3 ? 1 : (tag == 4 ? 4 :
                              ((tag == 2 || tag == 5 || tag == 6) ? 8 : 0));
          if (fixed != 0) {
            if (!reader.ReadFixedPayload(
                    &payload, fixed, request.maximum_memory_bytes)) {
              return false;
            }
          } else if (!reader.ReadPayloadString(
                         &payload, request.maximum_memory_bytes)) {
            return false;
          }
        } else if (!reader.ReadPayloadString(
                       &payload, request.maximum_memory_bytes)) {
          return false;
        }
        row.values.push_back(
            {field_order[column],
             ScopedRowBinaryMaterializeValue(field_types[column], payload)});
      }
      if (selected &&
          !DeliverVisibleStreamRow(request, result->descriptor, selection,
                                   *source_ordinal, row, result, detail)) {
        return false;
      }
      if (VisibleStreamDeliveryBoundReached(request, *result)) {
        return PublishVisibleStreamSecondPassCounters(phase, result, detail);
      }
    }
  }
  if (!reader.Finish()) {
    result->cancellation_observed = reader.cancellation_observed();
    return false;
  }
  return PublishVisibleStreamSecondPassCounters(phase, result, detail);
}

}  // namespace

PreparedMgaHeapReadAuthorityCohortResult PrepareMgaHeapReadAuthorities(
    const EngineRequestContext& context,
    const std::span<const std::string> relation_uuids) {
  return PrepareMgaHeapReadAuthoritiesImpl(context, relation_uuids);
}

PreparedMgaHeapReadAuthorityCohortResult PrepareMgaHeapReadAuthorities(
    const EngineRequestContext& context,
    const std::span<const std::string> relation_uuids,
    const scratchbird::transaction::mga::SnapshotVectorDescriptor&
        resolved_statement_snapshot) {
  return PrepareMgaHeapReadAuthoritiesImpl(
      context, relation_uuids, &resolved_statement_snapshot);
}

EngineApiDiagnostic RevalidatePreparedMgaHeapReadAuthorityCohort(
    const EngineRequestContext& context,
    const PreparedMgaHeapReadAuthorityCohort& cohort) {
  const auto* statement = cohort.statement.get();
  if (statement == nullptr || statement->transaction_states == nullptr ||
      statement->transaction_inventory_snapshot == nullptr ||
      cohort.relations.empty() ||
      statement->database_uuid != context.database_uuid.canonical ||
      statement->statement_uuid != context.statement_uuid.canonical ||
      statement->transaction_uuid != context.transaction_uuid.canonical ||
      statement->statement_snapshot_uuid !=
          context.statement_snapshot_uuid.canonical ||
      statement->statement_metadata_snapshot_uuid !=
          context.statement_metadata_snapshot_uuid.canonical ||
      statement->catalog_epoch_uuid != context.catalog_epoch_uuid.canonical ||
      statement->authorization_authority_uuid !=
          context.authorization_context.authority_uuid.canonical ||
      statement->catalog_generation != context.catalog_generation_id ||
      statement->security_epoch != context.authorization_context.security_epoch ||
      statement->policy_epoch != context.authorization_context.policy_epoch ||
      statement->resource_epoch != context.resource_epoch ||
      statement->local_transaction_id != context.local_transaction_id ||
      !statement->snapshot_vector.inventory_authoritative ||
      !statement->snapshot_vector.complete) {
    return MakeInvalidRequestDiagnostic(
        "mga.heap_relation_read.revalidate",
        "statement_authority_identity_stale");
  }
  const auto inventory_fence = scratchbird::storage::database::
      RevalidateLocalTransactionInventorySnapshot(
          *statement->transaction_inventory_snapshot);
  if (!inventory_fence.ok()) {
    // Inventory publication is database-wide, while this cohort is scoped to
    // one statement and owning transaction. An unrelated session may replace
    // the journal without invalidating either. Resolve through the exact
    // published snapshot authority on the slow path and compare the immutable
    // statement projection; this retains the execution-time TOCTOU fence
    // without turning unrelated transaction finality into a false conflict.
    EngineResolveStatementSnapshotRequest snapshot_request;
    snapshot_request.context = context;
    const auto current_snapshot =
        EngineResolveStatementSnapshot(snapshot_request);
    if (!current_snapshot.ok || !current_snapshot.snapshot_vector.complete ||
        !current_snapshot.snapshot_vector.inventory_authoritative ||
        current_snapshot.snapshot_vector.snapshot_uuid.kind !=
            statement->snapshot_vector.snapshot_uuid.kind ||
        current_snapshot.snapshot_vector.snapshot_uuid.value !=
            statement->snapshot_vector.snapshot_uuid.value ||
        current_snapshot.snapshot_vector.owning_transaction.value !=
            statement->snapshot_vector.owning_transaction.value ||
        current_snapshot.snapshot_vector.visible_committed_high_watermark !=
            statement->snapshot_vector.visible_committed_high_watermark) {
      return MakeInvalidRequestDiagnostic(
          "mga.heap_relation_read.revalidate",
          "transaction_inventory_statement_authority_stale");
    }
  }
  const auto metadata_identity = ExistingFileIdentity(statement->metadata_path);
  const auto savepoint_identity = ExistingFileIdentity(statement->savepoint_path);
  const auto descriptor_identity = ExistingFileIdentity(statement->descriptor_path);
  if ((metadata_identity.ok ? metadata_identity.file_size : 0) !=
          statement->metadata_file_size ||
      (metadata_identity.ok ? metadata_identity.file_mtime_ticks : 0) !=
          statement->metadata_file_mtime_ticks ||
      (savepoint_identity.ok ? savepoint_identity.file_size : 0) !=
          statement->savepoint_file_size ||
      (savepoint_identity.ok ? savepoint_identity.file_mtime_ticks : 0) !=
          statement->savepoint_file_mtime_ticks ||
      (descriptor_identity.ok ? descriptor_identity.file_size : 0) !=
          statement->descriptor_file_size ||
      (descriptor_identity.ok ? descriptor_identity.file_mtime_ticks : 0) !=
          statement->descriptor_file_mtime_ticks) {
    return MakeInvalidRequestDiagnostic(
        "mga.heap_relation_read.revalidate",
        "catalog_or_mga_file_identity_stale");
  }
  for (const auto& [relation_uuid, relation] : cohort.relations) {
    if (relation == nullptr || relation->statement.get() != statement ||
        relation->relation_uuid != relation_uuid ||
        relation->descriptor.relation_uuid.canonical != relation_uuid ||
        relation->descriptor.database_uuid.canonical !=
            context.database_uuid.canonical ||
        relation->descriptor.descriptor_uuid.canonical.empty() ||
        relation->descriptor.descriptor_generation == 0 ||
        relation->current_relation_base_generation == 0 ||
        (relation->temporary
             ? (relation->temporary_scope == "global"
                    ? (!relation->temporary_session_uuid.empty() ||
                       context.session_uuid.canonical.empty())
                    : (relation->temporary_scope != "private" ||
                       relation->temporary_session_uuid !=
                           context.session_uuid.canonical))
             : (!relation->temporary_scope.empty() ||
                !relation->temporary_session_uuid.empty()))) {
      return MakeInvalidRequestDiagnostic(
          "mga.heap_relation_read.revalidate",
          "relation_authority_identity_stale");
    }
  }
  return OkDiagnostic();
}

static MgaVisibleHeapRelationCountResult CountVisibleMgaHeapRelationObserved(
    const EngineRequestContext& context,
    const MgaVisibleHeapRelationCountRequest& request,
    HeapReadRuntimeObservation* runtime_observation,
    const PreparedMgaHeapReadAuthority* prepared_authority = nullptr,
    StreamingVisibleSelection* visible_selection = nullptr) {
  MgaVisibleHeapRelationCountResult result;
  result.memory_grant_bytes = request.maximum_memory_bytes;
  const auto refuse = [&](std::string detail,
                          const MgaHeapReadFailureCategoryV1 category) {
    if (const char* trace_path =
            std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");
        trace_path != nullptr && *trace_path != '\0') {
      std::ofstream trace(trace_path, std::ios::app | std::ios::binary);
      if (trace) {
        trace << "layer=mga_heap_count\taccepted=false\tcategory="
              << static_cast<unsigned>(category) << "\tdetail=" << detail
              << '\n';
      }
    }
    result.ok = false;
    result.diagnostic = MakeInvalidRequestDiagnostic(
        "mga.heap_relation_count", std::move(detail));
    result.failure_category =
        result.cancellation_observed
            ? MgaHeapReadFailureCategoryV1::kCancellation
            : category;
    result.visible_row_count = 0;
    result.current_live_memory_bytes = 0;
    result.memory_receipt_complete = false;
    return result;
  };
  const auto& cancellation_requested =
      request.borrowed_cancellation_requested == nullptr
          ? request.cancellation_requested
          : *request.borrowed_cancellation_requested;
  const auto& relation_uuid = request.borrowed_relation_uuid == nullptr
                                  ? request.relation_uuid
                                  : *request.borrowed_relation_uuid;
  HeapReadRuntimeObservation owned_runtime_observation;
  auto* const effective_runtime_observation =
      runtime_observation == nullptr ? &owned_runtime_observation
                                     : runtime_observation;
  if (relation_uuid.empty() || context.local_transaction_id == 0 ||
      context.transaction_uuid.canonical.empty()) {
    return refuse("exact_relation_transaction_and_snapshot_required",
                  MgaHeapReadFailureCategoryV1::kMgaContext);
  }
  if (request.maximum_decoded_bytes == 0 ||
      request.maximum_memory_bytes == 0) {
    return refuse("nonzero_heap_count_resource_bounds_required",
                  MgaHeapReadFailureCategoryV1::kResource);
  }
  if (!cancellation_requested) {
    return refuse("engine_cancellation_probe_required",
                  MgaHeapReadFailureCategoryV1::kInvalidRequest);
  }
  if (cancellation_requested()) {
    result.cancellation_observed = true;
    return refuse("heap_count_cancelled_before_descriptor_load",
                  MgaHeapReadFailureCategoryV1::kCancellation);
  }

  const auto inventory_guard =
      AcquireTransactionInventoryGuard(context.database_path);
  PreparedMgaHeapReadAuthority owned_authority;
  if (prepared_authority == nullptr) {
    auto prepared = PrepareMgaHeapReadAuthority(context, relation_uuid);
    if (!prepared.ok) {
      result.diagnostic = std::move(prepared.diagnostic);
      result.failure_category = MgaHeapReadFailureCategoryV1::kCatalog;
      return result;
    }
    owned_authority = std::move(prepared.authority);
    prepared_authority = &owned_authority;
  }
  const auto* prepared_statement = prepared_authority->statement.get();
  if (prepared_statement == nullptr ||
      prepared_statement->transaction_states == nullptr ||
      prepared_authority->relation_uuid != relation_uuid ||
      prepared_statement->database_uuid != context.database_uuid.canonical ||
      prepared_statement->statement_uuid != context.statement_uuid.canonical ||
      prepared_statement->transaction_uuid !=
          context.transaction_uuid.canonical ||
      prepared_statement->statement_snapshot_uuid !=
          context.statement_snapshot_uuid.canonical ||
      prepared_statement->statement_metadata_snapshot_uuid !=
          context.statement_metadata_snapshot_uuid.canonical ||
      prepared_statement->catalog_epoch_uuid !=
          context.catalog_epoch_uuid.canonical ||
      prepared_statement->authorization_authority_uuid !=
          context.authorization_context.authority_uuid.canonical ||
      prepared_statement->catalog_generation !=
          context.catalog_generation_id ||
      prepared_statement->security_epoch !=
          context.authorization_context.security_epoch ||
      prepared_statement->policy_epoch !=
          context.authorization_context.policy_epoch ||
      prepared_statement->resource_epoch != context.resource_epoch ||
      prepared_statement->local_transaction_id !=
          context.local_transaction_id ||
      !prepared_statement->snapshot_vector.inventory_authoritative ||
      !prepared_statement->snapshot_vector.complete) {
    return refuse("prepared_heap_count_authority_is_stale",
                  MgaHeapReadFailureCategoryV1::kMgaContext);
  }
  result.descriptor = prepared_authority->descriptor;
  result.current_relation_base_generation =
      prepared_authority->current_relation_base_generation;

  BoundedScopedRowReadControl savepoint_control;
  savepoint_control.maximum_row_versions =
      std::numeric_limits<std::uint64_t>::max();
  savepoint_control.maximum_bytes = request.maximum_decoded_bytes;
  savepoint_control.maximum_memory_bytes = request.maximum_memory_bytes;
  savepoint_control.cancellation_requested = &cancellation_requested;
  savepoint_control.runtime_observation = effective_runtime_observation;
  SavepointParsedState savepoints;
  const auto descriptor_memory =
      HeapReadStorageDescriptorMemoryBytes(result.descriptor);
  if (!descriptor_memory.has_value()) {
    return refuse("heap_count_authority_memory_receipt_overflow",
                  MgaHeapReadFailureCategoryV1::kResource);
  }
  savepoint_control.retained_parent_memory_bytes =
      sizeof(result) + *descriptor_memory + kStreamingCountScratchBytes;
  if (!ParseSavepointsBounded(
          context, &savepoint_control,
          savepoint_control.retained_parent_memory_bytes, &savepoints)) {
    result.cancellation_observed = savepoint_control.cancellation_observed;
    result.peak_live_memory_bytes = savepoint_control.peak_live_memory_bytes;
    return refuse(savepoint_control.refusal_detail.empty()
                      ? "bounded_savepoint_read_failed"
                      : savepoint_control.refusal_detail,
                  savepoint_control.failure_category ==
                          MgaHeapReadFailureCategoryV1::kNone
                      ? MgaHeapReadFailureCategoryV1::kResource
                      : savepoint_control.failure_category);
  }
  result.storage_bytes_read =
      effective_runtime_observation->storage_bytes_read;

  std::vector<StreamingCountRowVersion> rows;
  std::vector<std::string> fallback_identities;
  std::string detail;
  result.peak_live_memory_bytes = savepoint_control.peak_live_memory_bytes;
  if (!ObserveStreamingCountMemory(
          rows, fallback_identities, savepoints, result.descriptor,
          request.maximum_memory_bytes, &result.peak_live_memory_bytes,
          &detail)) {
    return refuse(detail, MgaHeapReadFailureCategoryV1::kResource);
  }

  const std::string text_path = ScopedRowStorePath(context, relation_uuid);
  const std::string binary_path =
      ScopedRowBinaryStorePath(context, relation_uuid);
  const bool text_exists = FileExistsAndNotEmpty(text_path);
  const bool binary_exists = FileExistsAndNotEmpty(binary_path);
  result.scoped_physical_segment_used = text_exists || binary_exists;
  const auto authorize_file = [&](const std::string& path,
                                  std::uint64_t* bytes) {
    std::error_code ignored;
    const auto size = std::filesystem::file_size(path, ignored);
    if (bytes == nullptr || ignored ||
        size == static_cast<std::uintmax_t>(-1) ||
        size > std::numeric_limits<std::uint64_t>::max()) {
      detail = "heap_count_scoped_segment_size_unavailable";
      return false;
    }
    *bytes = static_cast<std::uint64_t>(size);
    return true;
  };
  std::uint64_t text_bytes = 0;
  std::uint64_t binary_bytes = 0;
  if ((text_exists && !authorize_file(text_path, &text_bytes)) ||
      (binary_exists && !authorize_file(binary_path, &binary_bytes)) ||
      text_bytes > request.maximum_decoded_bytes ||
      binary_bytes > request.maximum_decoded_bytes - text_bytes) {
    return refuse(detail.empty()
                      ? "heap_count_maximum_decoded_bytes_exceeded"
                      : detail,
                  MgaHeapReadFailureCategoryV1::kResource);
  }
  if (visible_selection != nullptr) {
    visible_selection->text_path = text_path;
    visible_selection->binary_path = binary_path;
    visible_selection->text_bytes = text_bytes;
    visible_selection->binary_bytes = binary_bytes;
  }
  if (text_exists &&
      !DecodeStreamingCountTextFile(
          text_path, text_bytes, relation_uuid,
          context.session_uuid.canonical, prepared_statement->snapshot_vector,
          *prepared_statement->transaction_states, savepoints,
          result.descriptor, request.maximum_decoded_bytes,
          request.maximum_memory_bytes, &cancellation_requested, &rows,
          &fallback_identities, &result.peak_live_memory_bytes, &result,
          effective_runtime_observation, &detail)) {
    return refuse(detail.empty() ? "heap_count_scoped_text_decode_failed"
                                 : detail,
                  result.cancellation_observed
                      ? MgaHeapReadFailureCategoryV1::kCancellation
                      : MgaHeapReadFailureCategoryV1::kCorruptStorage);
  }
  if (binary_exists &&
      !DecodeStreamingCountBinaryFile(
          binary_path, binary_bytes, relation_uuid,
          context.session_uuid.canonical, prepared_statement->snapshot_vector,
          *prepared_statement->transaction_states, savepoints,
          result.descriptor, request.maximum_decoded_bytes,
          request.maximum_memory_bytes, &cancellation_requested, &rows,
          &fallback_identities, &result.peak_live_memory_bytes, &result,
          effective_runtime_observation, &detail)) {
    return refuse(detail.empty() ? "heap_count_scoped_binary_decode_failed"
                                 : detail,
                  result.cancellation_observed
                      ? MgaHeapReadFailureCategoryV1::kCancellation
                      : MgaHeapReadFailureCategoryV1::kCorruptStorage);
  }
  if (visible_selection != nullptr) {
    if (result.scanned_row_version_count ==
            std::numeric_limits<std::uint64_t>::max() ||
        result.scanned_row_version_count + 1 >
            std::numeric_limits<std::size_t>::max()) {
      return refuse("heap_count_visible_source_bitmap_overflow",
                    MgaHeapReadFailureCategoryV1::kResource);
    }
    const auto metadata_memory = StreamingCountMetadataMemoryBytes(
        rows, fallback_identities, savepoints, result.descriptor);
    std::uint64_t selection_memory = 0;
    if (!metadata_memory.has_value() ||
        !CheckedHeapReadMemoryAdd(
            result.scanned_row_version_count + 1, &selection_memory) ||
        !CheckedHeapReadMemoryAdd(*metadata_memory, &selection_memory) ||
        selection_memory > request.maximum_memory_bytes) {
      return refuse("heap_count_visible_source_bitmap_exceeds_memory_grant",
                    MgaHeapReadFailureCategoryV1::kResource);
    }
    result.peak_live_memory_bytes =
        std::max(result.peak_live_memory_bytes, selection_memory);
    visible_selection->visible_source_ordinals.assign(
        static_cast<std::size_t>(result.scanned_row_version_count + 1), 0);
  }
  if (!ValidateAndCountStreamingRows(
          &rows, fallback_identities, &cancellation_requested, &result,
          &detail,
          visible_selection == nullptr
              ? nullptr
              : &visible_selection->visible_source_ordinals)) {
    return refuse(detail.empty() ? "heap_count_visibility_failed" : detail,
                  result.cancellation_observed
                      ? MgaHeapReadFailureCategoryV1::kCancellation
                      : MgaHeapReadFailureCategoryV1::kCorruptStorage);
  }
  if (!ObserveStreamingCountMemory(
          rows, fallback_identities, savepoints, result.descriptor,
          request.maximum_memory_bytes, &result.peak_live_memory_bytes,
          &detail)) {
    return refuse(detail, MgaHeapReadFailureCategoryV1::kResource);
  }
  result.ok = true;
  result.failure_category = MgaHeapReadFailureCategoryV1::kNone;
  result.diagnostic = OkDiagnostic();
  result.current_live_memory_bytes = sizeof(result) + *descriptor_memory;
  if (!AccountHeapReadOwnedString(result.diagnostic.code,
                                  &result.current_live_memory_bytes) ||
      !AccountHeapReadOwnedString(result.diagnostic.message_key,
                                  &result.current_live_memory_bytes) ||
      !AccountHeapReadOwnedString(result.diagnostic.detail,
                                  &result.current_live_memory_bytes)) {
    return refuse("heap_count_memory_receipt_overflow",
                  MgaHeapReadFailureCategoryV1::kResource);
  }
  result.peak_live_memory_bytes = std::max(
      result.peak_live_memory_bytes, result.current_live_memory_bytes);
  result.memory_receipt_complete =
      result.current_live_memory_bytes <= result.peak_live_memory_bytes &&
      result.peak_live_memory_bytes <= result.memory_grant_bytes;
  if (!result.memory_receipt_complete) {
    return refuse("heap_count_memory_receipt_incomplete",
                  MgaHeapReadFailureCategoryV1::kResource);
  }
  effective_runtime_observation->storage_bytes_read =
      result.storage_bytes_read;
  effective_runtime_observation->decoded_bytes = result.decoded_byte_count;
  return result;
}

MgaVisibleHeapRelationCountResult CountVisibleMgaHeapRelation(
    const EngineRequestContext& context,
    const MgaVisibleHeapRelationCountRequest& request) {
  return CountVisibleMgaHeapRelationObserved(context, request, nullptr);
}

MgaVisibleHeapRelationCountResult CountVisibleMgaHeapRelationWithObservation(
    const EngineRequestContext& context,
    const MgaVisibleHeapRelationCountRequest& request,
    HeapReadRuntimeObservation* runtime_observation,
    const PreparedMgaHeapReadAuthority* prepared_authority) {
  return CountVisibleMgaHeapRelationObserved(
      context, request, runtime_observation, prepared_authority);
}

MgaVisibleHeapRelationStreamResult StreamVisibleMgaHeapRelation(
    const EngineRequestContext& context,
    const MgaVisibleHeapRelationStreamRequest& request) {
  MgaVisibleHeapRelationStreamResult result;
  result.memory_grant_bytes = request.maximum_memory_bytes;
  const auto refuse = [&](std::string detail,
                          const MgaHeapReadFailureCategoryV1 category) {
    result.ok = false;
    result.diagnostic = MakeInvalidRequestDiagnostic(
        "mga.heap_relation_stream", std::move(detail));
    result.failure_category =
        result.cancellation_observed
            ? MgaHeapReadFailureCategoryV1::kCancellation
            : category;
    result.memory_receipt_complete = false;
    return result;
  };
  const auto& relation_uuid = request.borrowed_relation_uuid == nullptr
                                  ? request.relation_uuid
                                  : *request.borrowed_relation_uuid;
  const auto& cancellation_requested =
      request.borrowed_cancellation_requested == nullptr
          ? request.cancellation_requested
          : *request.borrowed_cancellation_requested;
  if (relation_uuid.empty() || request.maximum_decoded_bytes_per_pass == 0 ||
      request.maximum_memory_bytes == 0 || !cancellation_requested ||
      !request.prepare_consumer_for_visible_rows ||
      !request.consumer_retained_memory_bytes ||
      !request.consume_visible_row) {
    return refuse("complete_stream_identity_bounds_and_callbacks_required",
                  MgaHeapReadFailureCategoryV1::kInvalidRequest);
  }
  if (cancellation_requested()) {
    result.cancellation_observed = true;
    return refuse("heap_stream_cancelled_before_visibility_pass",
                  MgaHeapReadFailureCategoryV1::kCancellation);
  }

  // The same recursive engine inventory guard spans both physical passes.
  // Engine-owned MGA append/finality routes therefore cannot change the
  // authorized segment extents between visibility selection and delivery.
  const auto inventory_guard =
      AcquireTransactionInventoryGuard(context.database_path);
  StreamingVisibleSelection selection;
  MgaVisibleHeapRelationCountRequest count_request;
  count_request.borrowed_relation_uuid = &relation_uuid;
  count_request.maximum_decoded_bytes =
      request.maximum_decoded_bytes_per_pass;
  count_request.maximum_memory_bytes = request.maximum_memory_bytes;
  count_request.borrowed_cancellation_requested = &cancellation_requested;
  auto counted = CountVisibleMgaHeapRelationObserved(
      context, count_request, nullptr, nullptr, &selection);
  result.visible_row_count = counted.visible_row_count;
  result.current_relation_base_generation =
      counted.current_relation_base_generation;
  result.scanned_row_version_count = counted.scanned_row_version_count;
  result.decoded_byte_count = counted.decoded_byte_count;
  result.storage_bytes_read = counted.storage_bytes_read;
  result.visibility_recheck_count = counted.visibility_recheck_count;
  result.invisible_row_version_count = counted.invisible_row_version_count;
  result.tombstone_row_count = counted.tombstone_row_count;
  result.scoped_physical_segment_used =
      counted.scoped_physical_segment_used;
  result.cancellation_observed = counted.cancellation_observed;
  result.peak_live_memory_bytes = counted.peak_live_memory_bytes;
  if (!counted.ok) {
    result.diagnostic = std::move(counted.diagnostic);
    result.failure_category = counted.failure_category;
    return result;
  }
  result.descriptor = std::move(counted.descriptor);
  result.complete_mga_chain_validation = true;

  const auto exact_extent = [](const std::string& path,
                               const std::uint64_t expected) {
    std::error_code ignored;
    const bool exists = std::filesystem::exists(path, ignored);
    if (ignored) return false;
    if (!exists) return expected == 0;
    const auto size = std::filesystem::file_size(path, ignored);
    return !ignored && size != static_cast<std::uintmax_t>(-1) &&
           size <= std::numeric_limits<std::uint64_t>::max() &&
           static_cast<std::uint64_t>(size) == expected;
  };
  if (!exact_extent(selection.text_path, selection.text_bytes) ||
      !exact_extent(selection.binary_path, selection.binary_bytes)) {
    return refuse("heap_stream_scoped_segment_changed_between_passes",
                  MgaHeapReadFailureCategoryV1::kCorruptStorage);
  }
  std::uint64_t consumer_bytes = 0;
  std::string detail;
  auto prepared_request = request;
  bool consumer_prepared = false;
  try {
    consumer_prepared = prepared_request.prepare_consumer_for_visible_rows(
        result.descriptor, result.visible_row_count,
        &prepared_request.maximum_consumer_growth_bytes_per_row);
  } catch (...) {
    return refuse("heap_stream_consumer_preparation_threw",
                  MgaHeapReadFailureCategoryV1::kInvalidRequest);
  }
  if (!consumer_prepared) {
    return refuse("heap_stream_consumer_preparation_refused",
                  MgaHeapReadFailureCategoryV1::kInvalidRequest);
  }
  const auto delivery_target =
      request.maximum_delivered_visible_rows.has_value()
          ? std::min(result.visible_row_count,
                     *request.maximum_delivered_visible_rows)
          : result.visible_row_count;
  if (delivery_target != 0 &&
      prepared_request.maximum_consumer_growth_bytes_per_row == 0) {
    return refuse("heap_stream_consumer_growth_bound_required",
                  MgaHeapReadFailureCategoryV1::kResource);
  }
  if (!StreamConsumerMemory(prepared_request, &consumer_bytes, &detail) ||
      !ObserveVisibleStreamMemory(
          prepared_request, result.descriptor, selection, nullptr,
          consumer_bytes, 0,
          &result.peak_live_memory_bytes, &detail)) {
    return refuse(detail.empty() ? "heap_stream_memory_admission_failed"
                                 : detail,
                  MgaHeapReadFailureCategoryV1::kResource);
  }
  std::uint64_t source_ordinal = 0;
  bool value_pass_ok = true;
  if (delivery_target != 0) {
    value_pass_ok = DecodeVisibleStreamTextFile(
        prepared_request, relation_uuid, selection, &source_ordinal, &result,
        &detail);
    if (value_pass_ok &&
        !VisibleStreamDeliveryBoundReached(prepared_request, result)) {
      value_pass_ok = DecodeVisibleStreamBinaryFile(
          prepared_request, relation_uuid, selection, &source_ordinal, &result,
          &detail);
    }
  }
  result.second_pass_scanned_row_version_count = source_ordinal;
  if (!value_pass_ok) {
    const bool cancelled = result.cancellation_observed ||
                           detail.find("cancelled") != std::string::npos;
    result.cancellation_observed = cancelled;
    const bool resource = detail.find("memory") != std::string::npos ||
                          detail.find("maximum") != std::string::npos ||
                          detail.find("overflow") != std::string::npos;
    return refuse(detail.empty() ? "heap_stream_value_pass_failed" : detail,
                  cancelled
                      ? MgaHeapReadFailureCategoryV1::kCancellation
                      : (resource ? MgaHeapReadFailureCategoryV1::kResource
                                  : MgaHeapReadFailureCategoryV1::kCorruptStorage));
  }
  try {
    if (cancellation_requested()) {
      result.cancellation_observed = true;
      return refuse("heap_stream_cancelled_before_value_publication",
                    MgaHeapReadFailureCategoryV1::kCancellation);
    }
  } catch (...) {
    return refuse("heap_stream_cancellation_probe_failed",
                  MgaHeapReadFailureCategoryV1::kCancellation);
  }
  result.complete_value_delivery =
      result.delivered_row_count == result.visible_row_count;
  result.delivery_stopped_by_bound =
      request.maximum_delivered_visible_rows.has_value() &&
      *request.maximum_delivered_visible_rows < result.visible_row_count &&
      result.delivered_row_count == *request.maximum_delivered_visible_rows;
  if (result.delivered_row_count != delivery_target ||
      source_ordinal > result.scanned_row_version_count ||
      (delivery_target != 0 &&
       !request.maximum_delivered_visible_rows.has_value() &&
       (source_ordinal != result.scanned_row_version_count ||
        !result.complete_value_delivery)) ||
      (!result.complete_value_delivery &&
       !result.delivery_stopped_by_bound)) {
    return refuse("heap_stream_visibility_selection_cardinality_mismatch",
                  MgaHeapReadFailureCategoryV1::kCorruptStorage);
  }
  if (!exact_extent(selection.text_path, selection.text_bytes) ||
      !exact_extent(selection.binary_path, selection.binary_bytes)) {
    return refuse("heap_stream_scoped_segment_changed_during_value_pass",
                  MgaHeapReadFailureCategoryV1::kCorruptStorage);
  }
  result.exact_segment_extent_revalidated = true;
  if (!StreamConsumerMemory(prepared_request, &consumer_bytes, &detail) ||
      !ObserveVisibleStreamMemory(
          prepared_request, result.descriptor, selection, nullptr,
          consumer_bytes, 0,
          &result.peak_live_memory_bytes, &detail)) {
    return refuse(detail.empty() ? "heap_stream_final_memory_receipt_failed"
                                 : detail,
                  MgaHeapReadFailureCategoryV1::kResource);
  }
  const auto descriptor_bytes =
      HeapReadStorageDescriptorMemoryBytes(result.descriptor);
  result.current_live_memory_bytes =
      sizeof(result) + sizeof(selection) +
      static_cast<std::uint64_t>(
          selection.visible_source_ordinals.capacity()) +
      consumer_bytes;
  if (!descriptor_bytes.has_value() ||
      !CheckedHeapReadMemoryAdd(*descriptor_bytes,
                                &result.current_live_memory_bytes) ||
      !AccountHeapReadOwnedString(selection.text_path,
                                  &result.current_live_memory_bytes) ||
      !AccountHeapReadOwnedString(selection.binary_path,
                                  &result.current_live_memory_bytes)) {
    return refuse("heap_stream_final_memory_receipt_overflow",
                  MgaHeapReadFailureCategoryV1::kResource);
  }
  result.peak_live_memory_bytes = std::max(
      result.peak_live_memory_bytes, result.current_live_memory_bytes);
  result.memory_receipt_complete =
      result.current_live_memory_bytes <= result.peak_live_memory_bytes &&
      result.peak_live_memory_bytes <= result.memory_grant_bytes;
  if (!result.memory_receipt_complete) {
    return refuse("heap_stream_final_memory_receipt_incomplete",
                  MgaHeapReadFailureCategoryV1::kResource);
  }
  result.ok = true;
  result.failure_category = MgaHeapReadFailureCategoryV1::kNone;
  result.diagnostic = OkDiagnostic();
  return result;
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

EngineApiDiagnostic AppendMgaTableMetadataWithSealedContextualTextDescriptorV2(
    const EngineRequestContext& context,
    const CrudTableRecord& table,
    const std::vector<CrudIndexRecord>& indexes,
    MgaRelationStorageDescriptor* descriptor) {
  constexpr const char* kOperation =
      "mga.relation_metadata.table_create.sealed_descriptor_v2";
  if (descriptor == nullptr) {
    return MakeInvalidRequestDiagnostic(kOperation, "descriptor_output_required");
  }
  *descriptor = {};
  if (context.database_path.empty()) {
    return MakeInvalidRequestDiagnostic(kOperation, "database_path_required");
  }
  const auto authority =
      ValidateMgaMutatingTransactionAuthority(context, kOperation);
  if (authority.error) return authority;
  if (!CanonicalNonNilMigrationUuid(table.table_uuid) ||
      table.columns.empty()) {
    return MakeInvalidRequestDiagnostic(
        kOperation, "table_identity_or_columns_invalid");
  }

  std::vector<std::string> allocator_lines;
  const auto reservation = ReserveEventSequenceRange(
      context, "relation_metadata", MetadataStorePath(context), 1,
      [&context]() { return ScanNextMetadataEventSequence(context); },
      &allocator_lines);
  if (!reservation.ok) return reservation.diagnostic;
  const auto abandon_reservation = [&]() {
    AbandonDeferredEventSequenceReservation(reservation);
    allocator_lines.clear();
  };

  CrudTableRecord writable = table;
  writable.creator_tx = context.local_transaction_id;
  writable.event_sequence = reservation.first;
  auto base_fields = BuildPersistedMgaRelationDescriptorFields(
      context, writable, indexes);
  auto relation_descriptor =
      DeserializeMgaRelationStorageDescriptor(base_fields);
  EngineApiDiagnostic material_diagnostic;
  if (!BindFreshCanonicalTextColumnIdentitiesV2(
          &writable, &relation_descriptor, &material_diagnostic)) {
    abandon_reservation();
    return material_diagnostic;
  }
  const auto validated =
      ValidateMgaRelationStorageDescriptor(relation_descriptor);
  if (validated.error) {
    abandon_reservation();
    return validated;
  }

  const bool requires_contextual_policy = std::ranges::any_of(
      relation_descriptor.columns, [](const auto& column) {
        const auto fields = StrictRelationDescriptorFields(
            column.value_descriptor.encoded_descriptor);
        const auto embedded =
            fields == std::nullopt
                ? std::map<std::string, std::string>::const_iterator{}
                : fields->find("datatype_descriptor_uuid");
        const bool canonical_text =
            column.value_descriptor.descriptor_uuid.canonical ==
                kCanonicalTextDescriptorUuid ||
            (fields != std::nullopt && embedded != fields->end() &&
             embedded->second == kCanonicalTextDescriptorUuid);
        return canonical_text && !column.charset_uuid.empty() &&
               !column.collation_uuid.empty();
      });
  EngineContextualTextPolicyRowSetV2 policy_rows;
  if (requires_contextual_policy) {
    const auto policy =
        LoadCurrentEngineContextualTextPolicyRowSetForPublicationV2();
    if (!policy.ok) {
      abandon_reservation();
      return policy.diagnostic;
    }
    policy_rows = policy.rows;
  }
  MgaSealedContextualTextDescriptorMaterialV2 material;
  if (!BuildMgaSealedContextualTextDescriptorMaterialV2(
          context, writable, std::move(relation_descriptor), policy_rows,
          &material, &material_diagnostic)) {
    abandon_reservation();
    return material_diagnostic;
  }

  std::vector<std::pair<std::string, std::string>> complete_fields;
  complete_fields.reserve(material.sealed_set.descriptor_fields.size());
  for (const auto& field : material.sealed_set.descriptor_fields) {
    complete_fields.emplace_back(
        std::string(field.key_raw_bytes.begin(), field.key_raw_bytes.end()),
        std::string(field.value_raw_bytes.begin(), field.value_raw_bytes.end()));
  }
  if (complete_fields.empty() ||
      complete_fields.size() !=
          material.sealed_set.descriptor_field_count ||
      complete_fields.back().first !=
          kMgaContextualTextSidecarSetSealKeyV2 ||
      complete_fields.back().second.size() !=
          material.sealed_set.seal_sha256.size()) {
    abandon_reservation();
    return ContextualTextMgaDiagnostic(
        "sealed descriptor vector header or final seal is invalid");
  }

  namespace stf = sealed_table_metadata_field_v2;
  std::vector<std::string> fields(stf::kFieldCount);
  fields[stf::kMagic] = kRowStoreMagic;
  fields[stf::kRecordKind] = std::string(kSealedTableMetadataKindV2);
  fields[stf::kCreatorTx] = std::to_string(writable.creator_tx);
  fields[stf::kEventSequence] = std::to_string(writable.event_sequence);
  fields[stf::kFormat] = std::string(kSealedTableMetadataFormatV2);
  fields[stf::kSealState] = "sealed";
  fields[stf::kTableUuid] = writable.table_uuid;
  fields[stf::kDefaultName] = EncodeCrudText(writable.default_name);
  fields[stf::kColumns] = EncodeCrudPairs(writable.columns);
  fields[stf::kTemporary] = writable.temporary ? "1" : "0";
  fields[stf::kTemporaryScope] = writable.temporary_scope;
  fields[stf::kTemporarySessionUuid] = writable.temporary_session_uuid;
  fields[stf::kOnCommitAction] = writable.on_commit_action;
  fields[stf::kRelationDescriptorUuid] =
      material.relation_descriptor.descriptor_uuid.canonical;
  fields[stf::kRelationDescriptorGeneration] =
      std::to_string(material.relation_descriptor.descriptor_generation);
  fields[stf::kDescriptorFieldCount] =
      std::to_string(material.sealed_set.descriptor_field_count);
  fields[stf::kDescriptorFieldBytes] =
      std::to_string(material.sealed_set.descriptor_field_bytes);
  fields[stf::kContextualSidecarCount] =
      std::to_string(material.sealed_set.contextual_sidecar_count);
  fields[stf::kDescriptorFields] = EncodeCrudPairs(complete_fields);
  if (!AppendLine(MetadataStorePath(context), JoinLine(fields))) {
    abandon_reservation();
    return MakeInvalidRequestDiagnostic(
        kOperation, "sealed_table_descriptor_append_failed");
  }
  // The sealed line is the atomic recovery authority. The allocator record is
  // acceleration only and is published after that one visibility barrier.
  (void)AppendDeferredEventSequenceAllocatorLines(
      context, &allocator_lines, nullptr);
  *descriptor = std::move(material.relation_descriptor);
  return OkDiagnostic();
}

EngineApiDiagnostic AppendMgaConstraintMutationBatch(
    const EngineRequestContext& context,
    const MgaConstraintMutationBatch& batch) {
  constexpr const char* kOperation = "mga.constraint_mutation_batch";
  if (context.database_path.empty()) {
    return MakeInvalidRequestDiagnostic(kOperation, "database_path_required");
  }
  const auto authority = ValidateMgaMutatingTransactionAuthority(
      context, kOperation);
  if (authority.error) return authority;
  if (batch.format_version != "neutral_fk_mutation_batch_v1" ||
      // The caller supplies the complete semantics, never the seal.  The
      // engine reserves the MGA event and hashes the final record below.
      !ValidConstraintBatchUuid(
          batch.batch_uuid,
          scratchbird::core::platform::UuidKind::row) ||
      !batch.batch_hash.empty() || batch.mutation_count != 1 ||
      !ValidConstraintBatchUuid(
          batch.database_uuid,
          scratchbird::core::platform::UuidKind::database) ||
      !ValidConstraintBatchUuid(
          batch.constraint_uuid,
          scratchbird::core::platform::UuidKind::object) ||
      batch.database_uuid != context.database_uuid.canonical ||
      !ValidConstraintBatchUuid(
          batch.owner_table_uuid,
          scratchbird::core::platform::UuidKind::object) ||
      !ValidConstraintBatchUuid(
          batch.child_schema_uuid,
          scratchbird::core::platform::UuidKind::schema) ||
      !ValidConstraintBatchUuid(
          batch.child_relation_descriptor_uuid,
          scratchbird::core::platform::UuidKind::object) ||
      batch.child_relation_descriptor_generation == 0 ||
      !ValidConstraintBatchUuid(
          batch.child_column_uuid,
          scratchbird::core::platform::UuidKind::object) ||
      !ValidConstraintBatchUuid(
          batch.parent_table_uuid,
          scratchbird::core::platform::UuidKind::object) ||
      !ValidConstraintBatchUuid(
          batch.parent_schema_uuid,
          scratchbird::core::platform::UuidKind::schema) ||
      !ValidConstraintBatchUuid(
          batch.parent_relation_descriptor_uuid,
          scratchbird::core::platform::UuidKind::object) ||
      batch.parent_relation_descriptor_generation == 0 ||
      !ValidConstraintBatchUuid(
          batch.parent_column_uuid,
          scratchbird::core::platform::UuidKind::object) ||
      !ValidConstraintBatchUuid(
          batch.parent_candidate_key_constraint_uuid,
          scratchbird::core::platform::UuidKind::object) ||
      !ValidConstraintBatchUuid(
          batch.key_descriptor_uuid,
          scratchbird::core::platform::UuidKind::object) ||
      !ValidConstraintBatchUuid(
          batch.support_uuid,
          scratchbird::core::platform::UuidKind::object) ||
      batch.support_family != "btree" ||
      batch.support_policy != "required_exact_unique_index" ||
      batch.match_policy != "simple" ||
      batch.on_update_action != "no_action" ||
      batch.on_delete_action != "no_action" ||
      batch.enforcement_timing != "immediate" ||
      batch.constraint_metadata_generation == 0 ||
      batch.base_table_event_sequence == 0 ||
      batch.parent_base_table_event_sequence == 0 ||
      batch.constraint_kind != "foreign_key" ||
      batch.canonical_constraint_envelope.empty() ||
      batch.updated_table.table_uuid != batch.owner_table_uuid) {
    return MakeInvalidRequestDiagnostic(kOperation,
                                        "complete_prevalidated_batch_required");
  }
  if (batch.updated_table.temporary ||
      !batch.updated_table.temporary_scope.empty() ||
      !batch.updated_table.temporary_session_uuid.empty() ||
      !batch.updated_table.on_commit_action.empty()) {
    return MakeInvalidRequestDiagnostic(
        kOperation, "temporary_constraint_mutation_batch_unsupported");
  }
  const auto current = LoadMgaRelationStoreState(context);
  if (!current.ok) return current.diagnostic;
  const RelationReadSnapshot current_state = BuildCrudCompatibilityStateFromMga(
      current.state);
  const auto current_owner = FindVisibleCrudTable(
      current_state, batch.owner_table_uuid, context.local_transaction_id);
  if (!current_owner ||
      current_owner->event_sequence != batch.base_table_event_sequence) {
    return MakeInvalidRequestDiagnostic(
        kOperation, "owner_metadata_event_changed_before_append");
  }
  const auto current_parent = FindVisibleCrudTable(
      current_state, batch.parent_table_uuid, context.local_transaction_id);
  if (!current_parent ||
      current_parent->event_sequence !=
          batch.parent_base_table_event_sequence) {
    return MakeInvalidRequestDiagnostic(
        kOperation, "parent_metadata_event_changed_before_append");
  }
  const CrudTableRecord& updated = batch.updated_table;
  if (updated.table_uuid != current_owner->table_uuid ||
      updated.default_name != current_owner->default_name ||
      updated.temporary != current_owner->temporary ||
      updated.temporary_scope != current_owner->temporary_scope ||
      updated.temporary_session_uuid !=
          current_owner->temporary_session_uuid ||
      updated.on_commit_action != current_owner->on_commit_action ||
      updated.columns.size() != current_owner->columns.size()) {
    return MakeInvalidRequestDiagnostic(
        kOperation, "constraint_batch_table_projection_changed");
  }
  std::size_t changed_column_count = 0;
  const std::pair<std::string, std::string>* old_changed_column = nullptr;
  const std::pair<std::string, std::string>* new_changed_column = nullptr;
  for (std::size_t index = 0; index < updated.columns.size(); ++index) {
    const auto& old_column = current_owner->columns[index];
    const auto& new_column = updated.columns[index];
    if (old_column.first != new_column.first) {
      return MakeInvalidRequestDiagnostic(
          kOperation, "constraint_batch_column_order_or_name_changed");
    }
    if (old_column.second == new_column.second) continue;
    ++changed_column_count;
    old_changed_column = &old_column;
    new_changed_column = &new_column;
  }
  if (changed_column_count != batch.mutation_count ||
      changed_column_count != 1 || old_changed_column == nullptr ||
      new_changed_column == nullptr) {
    return MakeInvalidRequestDiagnostic(
        kOperation, "constraint_batch_mutation_count_mismatch");
  }
  const auto old_fields =
      StrictRelationDescriptorFields(old_changed_column->second);
  const auto new_fields =
      StrictRelationDescriptorFields(new_changed_column->second);
  const auto envelope_fields = StrictRelationDescriptorFields(
      batch.canonical_constraint_envelope);
  if (!old_fields || !new_fields || !envelope_fields) {
    return MakeInvalidRequestDiagnostic(
        kOperation, "constraint_batch_descriptor_encoding_invalid");
  }
  const std::set<std::string> fk_projection_fields = {
      "foreign_key",
      "constraint_uuid",
      "constraint_name",
      "constraint_class",
      "owner_object_uuid",
      "owner_object_name",
      "child_column_uuid",
      "referenced_table_uuid",
      "referenced_table_name",
      "referenced_column_uuid",
      "referenced_column",
      "key_descriptor_uuid",
      "referenced_key_descriptor_uuid",
      "referenced_candidate_key_constraint_uuid",
      "support_uuid",
      "referenced_support_uuid",
      "support_family",
      "on_update",
      "on_delete",
      "referential_action",
      "enforcement_timing",
      "deferrable",
      "constraint_mutation_batch_uuid",
      "constraint_mutation_batch_state"};
  for (const auto& [key, value] : *old_fields) {
    const auto found = new_fields->find(key);
    if (fk_projection_fields.find(key) != fk_projection_fields.end() ||
        found == new_fields->end() || found->second != value) {
      return MakeInvalidRequestDiagnostic(
          kOperation, "constraint_batch_changed_base_column_descriptor");
    }
  }
  for (const auto& [key, value] : *new_fields) {
    (void)value;
    if (old_fields->find(key) == old_fields->end() &&
        fk_projection_fields.find(key) == fk_projection_fields.end()) {
      return MakeInvalidRequestDiagnostic(
          kOperation, "constraint_batch_added_unrelated_column_field");
    }
  }
  auto require_field = [&](const std::map<std::string, std::string>& fields,
                           const char* key,
                           const std::string& expected) {
    const auto found = fields.find(key);
    return found != fields.end() && found->second == expected;
  };
  if (!require_field(*new_fields, "foreign_key", "true") ||
      !require_field(*new_fields, "constraint_uuid", batch.constraint_uuid) ||
      !require_field(*new_fields, "constraint_name", batch.constraint_name) ||
      !require_field(*new_fields, "constraint_class", "foreign_key") ||
      !require_field(*new_fields, "owner_object_uuid",
                     batch.owner_table_uuid) ||
      !require_field(*new_fields, "owner_object_name",
                     current_owner->default_name) ||
      !require_field(*new_fields, "child_column_uuid",
                     batch.child_column_uuid) ||
      !require_field(*new_fields, "referenced_table_uuid",
                     batch.parent_table_uuid) ||
      !require_field(*new_fields, "referenced_table_name",
                     current_parent->default_name) ||
      !require_field(*new_fields, "referenced_column_uuid",
                     batch.parent_column_uuid) ||
      !require_field(*new_fields, "key_descriptor_uuid",
                     batch.key_descriptor_uuid) ||
      !require_field(*new_fields, "referenced_key_descriptor_uuid",
                     batch.key_descriptor_uuid) ||
      !require_field(*new_fields,
                     "referenced_candidate_key_constraint_uuid",
                     batch.parent_candidate_key_constraint_uuid) ||
      !require_field(*new_fields, "support_uuid", batch.support_uuid) ||
      !require_field(*new_fields, "referenced_support_uuid",
                     batch.support_uuid) ||
      !require_field(*new_fields, "support_family",
                     batch.support_family) ||
      !require_field(*new_fields, "on_update", batch.on_update_action) ||
      !require_field(*new_fields, "on_delete", batch.on_delete_action) ||
      !require_field(*new_fields, "referential_action", "no_action") ||
      !require_field(*new_fields, "enforcement_timing",
                     batch.enforcement_timing) ||
      !require_field(*new_fields, "deferrable", "false") ||
      !require_field(*new_fields, "constraint_mutation_batch_uuid",
                     batch.batch_uuid) ||
      !require_field(*new_fields, "constraint_mutation_batch_state",
                     "sealed")) {
    return MakeInvalidRequestDiagnostic(
        kOperation, "constraint_batch_column_projection_incoherent");
  }
  const std::set<std::string> final_envelope_fields = {
      "descriptor_version",
      "child_table_uuid",
      "child_column_uuid",
      "child_relation_descriptor_uuid",
      "child_relation_descriptor_generation",
      "parent_table_uuid",
      "parent_column_uuid",
      "parent_relation_descriptor_uuid",
      "parent_relation_descriptor_generation",
      "referenced_table_uuid",
      "referenced_column_uuid",
      "referenced_column",
      "child_column",
      "constraint_name_quoted",
      "on_update",
      "on_delete",
      "referential_action",
      "enforcement_timing",
      "deferrable",
      "constraint_uuid",
      "constraint_name",
      "owner_object_uuid",
      "key_descriptor_uuid",
      "referenced_candidate_key_constraint_uuid",
      "support_uuid",
      "support_family",
      "constraint_mutation_batch_uuid",
      "constraint_mutation_batch_state"};
  std::set<std::string> actual_envelope_fields;
  for (const auto& [key, value] : *envelope_fields) {
    (void)value;
    actual_envelope_fields.insert(key);
  }
  if (actual_envelope_fields != final_envelope_fields ||
      !require_field(*envelope_fields, "descriptor_version",
                     "neutral_fk_single_column_v1") ||
      !require_field(*envelope_fields, "child_table_uuid",
                     batch.owner_table_uuid) ||
      !require_field(*envelope_fields, "child_column_uuid",
                     batch.child_column_uuid) ||
      !require_field(*envelope_fields, "child_relation_descriptor_uuid",
                     batch.child_relation_descriptor_uuid) ||
      !require_field(*envelope_fields,
                     "child_relation_descriptor_generation",
                     std::to_string(
                         batch.child_relation_descriptor_generation)) ||
      !require_field(*envelope_fields, "parent_table_uuid",
                     batch.parent_table_uuid) ||
      !require_field(*envelope_fields, "parent_column_uuid",
                     batch.parent_column_uuid) ||
      !require_field(*envelope_fields, "parent_relation_descriptor_uuid",
                     batch.parent_relation_descriptor_uuid) ||
      !require_field(*envelope_fields,
                     "parent_relation_descriptor_generation",
                     std::to_string(
                         batch.parent_relation_descriptor_generation)) ||
      !require_field(*envelope_fields, "referenced_table_uuid",
                     batch.parent_table_uuid) ||
      !require_field(*envelope_fields, "referenced_column_uuid",
                     batch.parent_column_uuid) ||
      !require_field(*envelope_fields, "constraint_uuid",
                     batch.constraint_uuid) ||
      !require_field(*envelope_fields, "constraint_name",
                     batch.constraint_name) ||
      !require_field(*envelope_fields, "owner_object_uuid",
                     batch.owner_table_uuid) ||
      !require_field(*envelope_fields, "key_descriptor_uuid",
                     batch.key_descriptor_uuid) ||
      !require_field(*envelope_fields,
                     "referenced_candidate_key_constraint_uuid",
                     batch.parent_candidate_key_constraint_uuid) ||
      !require_field(*envelope_fields, "support_uuid", batch.support_uuid) ||
      !require_field(*envelope_fields, "support_family",
                     batch.support_family) ||
      !require_field(*envelope_fields, "on_update",
                     batch.on_update_action) ||
      !require_field(*envelope_fields, "on_delete",
                     batch.on_delete_action) ||
      !require_field(*envelope_fields, "referential_action", "no_action") ||
      !require_field(*envelope_fields, "enforcement_timing",
                     batch.enforcement_timing) ||
      !require_field(*envelope_fields, "deferrable", "false") ||
      !require_field(*envelope_fields, "constraint_mutation_batch_uuid",
                     batch.batch_uuid) ||
      !require_field(*envelope_fields, "constraint_mutation_batch_state",
                     "sealed") ||
      !require_field(*new_fields, "referenced_column",
                     RelationDescriptorFieldOrEmpty(
                         *envelope_fields, {"referenced_column"}))) {
    return MakeInvalidRequestDiagnostic(
        kOperation, "constraint_batch_canonical_envelope_incoherent");
  }
  const auto child_storage = LoadMgaRelationStorageDescriptor(
      context, batch.owner_table_uuid);
  const auto parent_storage = LoadMgaRelationStorageDescriptor(
      context, batch.parent_table_uuid);
  if (!child_storage.ok) return child_storage.diagnostic;
  if (!parent_storage.ok) return parent_storage.diagnostic;
  const auto& child_relation = child_storage.descriptor;
  const auto& parent_relation = parent_storage.descriptor;
  if (child_relation.database_uuid.canonical != batch.database_uuid ||
      child_relation.relation_uuid.canonical != batch.owner_table_uuid ||
      child_relation.schema_uuid.canonical != batch.child_schema_uuid ||
      child_relation.descriptor_uuid.canonical !=
          batch.child_relation_descriptor_uuid ||
      child_relation.descriptor_generation !=
          batch.child_relation_descriptor_generation ||
      parent_relation.database_uuid.canonical != batch.database_uuid ||
      parent_relation.relation_uuid.canonical != batch.parent_table_uuid ||
      parent_relation.schema_uuid.canonical != batch.parent_schema_uuid ||
      parent_relation.descriptor_uuid.canonical !=
          batch.parent_relation_descriptor_uuid ||
      parent_relation.descriptor_generation !=
          batch.parent_relation_descriptor_generation) {
    return MakeInvalidRequestDiagnostic(
        kOperation, "constraint_batch_relation_descriptor_binding_changed");
  }
  const auto child_column = std::find_if(
      child_relation.columns.begin(), child_relation.columns.end(),
      [&](const MgaRelationColumnStorageDescriptor& column) {
        return column.column_uuid.canonical == batch.child_column_uuid;
      });
  const auto parent_column = std::find_if(
      parent_relation.columns.begin(), parent_relation.columns.end(),
      [&](const MgaRelationColumnStorageDescriptor& column) {
        return column.column_uuid.canonical == batch.parent_column_uuid;
      });
  const std::string quoted = RelationDescriptorFieldOrEmpty(
      *envelope_fields, {"constraint_name_quoted"});
  if (child_column == child_relation.columns.end() ||
      parent_column == parent_relation.columns.end() ||
      child_column->canonical_name_key != new_changed_column->first ||
      !require_field(*envelope_fields, "child_column",
                     child_column->canonical_name_key) ||
      !require_field(*envelope_fields, "referenced_column",
                     parent_column->canonical_name_key) ||
      !require_field(*new_fields, "referenced_column",
                     parent_column->canonical_name_key) ||
      (quoted != "true" && quoted != "false")) {
    return MakeInvalidRequestDiagnostic(
        kOperation, "constraint_batch_column_descriptor_binding_changed");
  }
  const auto parent_metadata_column = std::find_if(
      current_parent->columns.begin(), current_parent->columns.end(),
      [&](const auto& column) {
        return column.first == parent_column->canonical_name_key;
      });
  if (parent_metadata_column == current_parent->columns.end()) {
    return MakeInvalidRequestDiagnostic(
        kOperation, "parent_candidate_key_column_not_visible");
  }
  const auto parent_key_fields =
      StrictRelationDescriptorFields(parent_metadata_column->second);
  if (!parent_key_fields ||
      !require_field(*parent_key_fields,
                     "candidate_key_constraint_uuid",
                     batch.parent_candidate_key_constraint_uuid) ||
      !require_field(*parent_key_fields,
                     "candidate_key_descriptor_uuid",
                     batch.key_descriptor_uuid) ||
      !require_field(*parent_key_fields, "support_uuid",
                     batch.support_uuid) ||
      !require_field(*parent_key_fields, "support_family", "btree") ||
      (RelationDescriptorFieldOrEmpty(
           *parent_key_fields, {"candidate_key_class"}) != "primary_key" &&
       RelationDescriptorFieldOrEmpty(
           *parent_key_fields, {"candidate_key_class"}) != "unique")) {
    return MakeInvalidRequestDiagnostic(
        kOperation, "parent_candidate_key_projection_changed");
  }
  const auto descriptor_support = std::find_if(
      parent_relation.indexes.begin(), parent_relation.indexes.end(),
      [&](const MgaRelationIndexStorageDescriptor& index) {
        return index.index_uuid.canonical == batch.support_uuid &&
               index.unique && index.family == "btree";
      });
  auto key_columns = [](const CrudIndexRecord& index) {
    std::vector<std::string> columns;
    for (const std::string& envelope : index.key_envelopes) {
      if (envelope.empty() || envelope == "unique" ||
          envelope == "primary_key" || envelope.starts_with("include:") ||
          envelope.starts_with("where_eq:") ||
          envelope.starts_with("where_mod_eq:") ||
          envelope == "where_true") {
        continue;
      }
      if (envelope.starts_with("identity:")) {
        columns.push_back(envelope.substr(9));
      } else if (envelope.starts_with("desc:")) {
        columns.push_back(envelope.substr(5));
      } else if (envelope.starts_with("cast:")) {
        const std::string rest = envelope.substr(5);
        const auto separator = rest.find(':');
        columns.push_back(separator == std::string::npos
                              ? rest
                              : rest.substr(0, separator));
      } else {
        columns.push_back(envelope);
      }
    }
    if (columns.empty() && !index.column_name.empty()) {
      columns.push_back(index.column_name);
    }
    return columns;
  };
  std::size_t exact_support_count = 0;
  for (const auto& index : VisibleCrudIndexesForTable(
           current_state,
           current_parent->table_uuid,
           context.local_transaction_id)) {
    const auto columns = key_columns(index);
    const std::string visible_support_family =
        index.family.empty() ? CrudIndexFamilyForProfile(index.profile)
                             : index.family;
    if (index.index_uuid == batch.support_uuid && index.unique &&
        visible_support_family == batch.support_family &&
        columns.size() == 1 &&
        columns.front() == parent_column->canonical_name_key) {
      ++exact_support_count;
    }
  }
  if (descriptor_support == parent_relation.indexes.end() ||
      exact_support_count != 1) {
    return MakeInvalidRequestDiagnostic(
        kOperation, "parent_candidate_key_exact_support_changed");
  }
  // D1 admits one immediate single-column FK per child table.  Enforce that
  // bounded generation model in the storage authority as well as the DDL
  // adapter so generation 1 can never silently duplicate.
  if (batch.constraint_metadata_generation != 1) {
    return MakeInvalidRequestDiagnostic(
        kOperation, "bounded_d1_constraint_generation_must_be_one");
  }
  for (const auto& [column_name, descriptor] : current_owner->columns) {
    (void)column_name;
    if (descriptor.find("constraint_mutation_batch_state=sealed") !=
        std::string::npos) {
      return MakeInvalidRequestDiagnostic(
          kOperation, "bounded_d1_prior_constraint_batch_unsupported");
    }
  }
  const auto reservation = ReserveEventSequenceRange(
      context,
      "relation_metadata",
      MetadataStorePath(context),
      1,
      [&context]() { return ScanNextMetadataEventSequence(context); });
  if (!reservation.ok) return reservation.diagnostic;

  CrudTableRecord table = batch.updated_table;
  table.creator_tx = context.local_transaction_id;
  table.event_sequence = reservation.first;
  MgaConstraintMutationBatch sealed_batch = batch;
  sealed_batch.updated_table = table;
  sealed_batch.batch_hash = ComputeMgaConstraintMutationBatchHash(
      sealed_batch, table.creator_tx, table.event_sequence);
  if (sealed_batch.batch_hash.empty()) {
    return MakeInvalidRequestDiagnostic(kOperation,
                                        "batch_hash_generation_failed");
  }
  // `sealed` is emitted here, after all required catalog and relation fields
  // have passed validation, and is never copied from an SBLR operand.
  const auto line_fields = ConstraintMutationBatchLineFields(
      sealed_batch, table.creator_tx, table.event_sequence);
  if (line_fields.size() != ConstraintMutationBatchFieldCount()) {
    return MakeInvalidRequestDiagnostic(kOperation,
                                        "batch_codec_field_count_invalid");
  }
  const std::string line = JoinLine(line_fields);
  if (!AppendLine(MetadataStorePath(context), line)) {
    return MakeInvalidRequestDiagnostic(kOperation,
                                        "sealed_batch_append_failed");
  }
  return OkDiagnostic();
}

MgaBigintIdentityMigrationResult AppendMgaBigintIdentityMigrationBatch(
    const EngineRequestContext& context,
    const MgaBigintIdentityMigrationRequest& request) {
  constexpr const char* kOperation = "mga.bigint_identity_migration";
  MgaBigintIdentityMigrationResult result;
  auto refuse = [&](std::string code, std::string key, std::string detail) {
    result.diagnostic = MakeEngineApiDiagnostic(
        std::move(code), std::move(key), std::move(detail));
    return result;
  };
  if (context.database_path.empty() || context.local_transaction_id == 0 ||
      context.transaction_uuid.canonical.empty()) {
    return refuse("DATATYPE.DESCRIPTOR_INVALID",
                  "bigint_identity_migration_context_invalid",
                  "active MGA transaction and database path required");
  }
  const auto authority = ValidateMgaMutatingTransactionAuthority(
      context, kOperation);
  if (authority.error) {
    result.diagnostic = authority;
    return result;
  }
  if (request.migration_id != kBigintMigrationId || request.rows.empty() ||
      request.prior_catalog_snapshot_uuid.empty() ||
      request.new_catalog_snapshot_uuid.empty() ||
      request.prior_catalog_snapshot_uuid == request.new_catalog_snapshot_uuid ||
      request.prior_catalog_generation == 0 ||
      request.new_catalog_generation != request.prior_catalog_generation + 1 ||
      (!context.statement_metadata_snapshot_uuid.canonical.empty() &&
       context.statement_metadata_snapshot_uuid.canonical !=
           request.prior_catalog_snapshot_uuid)) {
    return refuse("DATATYPE.DESCRIPTOR_INVALID",
                  "bigint_identity_migration_snapshot_stale",
                  "exact prior snapshot and consecutive catalog generation required");
  }

  const auto current = LoadMgaRelationStoreState(context);
  if (!current.ok) {
    result.diagnostic = current.diagnostic;
    return result;
  }
  std::set<std::pair<std::string, std::string>> identities;
  std::set<std::string> objects;
  std::vector<CrudTableRecord> tables;
  tables.reserve(request.rows.size());
  for (const auto& requested : request.rows) {
    if (requested.object_uuid.empty() || requested.column_uuid.empty() ||
        requested.old_row_generation == 0 ||
        !identities.emplace(requested.object_uuid,
                            requested.column_uuid).second ||
        !objects.emplace(requested.object_uuid).second) {
      return refuse("CORE.AUTHORITY.CONFLICT",
                    "bigint_identity_migration_multiple_mapping",
                    "each object and column identity must occur exactly once");
    }
    const CrudTableRecord* exact = nullptr;
    std::uint64_t newest_visible_generation = 0;
    for (const auto& table : current.state.relation_metadata.tables) {
      if (table.table_uuid != requested.object_uuid ||
          !CrudCreatorVisible(current.state.relation_metadata,
                              table.creator_tx,
                              table.event_sequence,
                              context.local_transaction_id)) {
        continue;
      }
      newest_visible_generation =
          std::max(newest_visible_generation, table.event_sequence);
      if (table.event_sequence != requested.old_row_generation) continue;
      if (exact != nullptr) {
        return refuse("CORE.AUTHORITY.CONFLICT",
                      "bigint_identity_migration_multiple_mapping",
                      "multiple visible rows share the expected generation");
      }
      exact = &table;
    }
    if (exact == nullptr ||
        newest_visible_generation != requested.old_row_generation) {
      return refuse("DATATYPE.DESCRIPTOR_INVALID",
                    "bigint_identity_migration_row_generation_stale",
                    requested.object_uuid);
    }
    CrudTableRecord updated = *exact;
    if (updated.temporary || !updated.temporary_scope.empty() ||
        !updated.temporary_session_uuid.empty() ||
        !updated.on_commit_action.empty()) {
      return refuse("CORE.AUTHORITY.CONFLICT",
                    "bigint_identity_migration_temporary_unsupported",
                    requested.object_uuid);
    }
    std::size_t matched_columns = 0;
    for (auto& [column_name, descriptor] : updated.columns) {
      (void)column_name;
      const auto descriptor_fields = StrictRelationDescriptorFields(descriptor);
      if (!descriptor_fields) {
        return refuse("CORE.AUTHORITY.CONFLICT",
                      "bigint_identity_migration_descriptor_contradiction",
                      requested.object_uuid);
      }
      const auto column = descriptor_fields->find("column_uuid");
      if (column == descriptor_fields->end() ||
          column->second != requested.column_uuid) {
        continue;
      }
      ++matched_columns;
      const auto type = descriptor_fields->find("type_uuid");
      if (type == descriptor_fields->end() ||
          type->second != kLegacyBigintTypeUuid) {
        return refuse("DATATYPE.DESCRIPTOR_INVALID",
                      "bigint_identity_migration_legacy_identity_required",
                      requested.column_uuid);
      }
      const auto first = descriptor.find(kLegacyBigintTypeUuid);
      if (first == std::string::npos ||
          descriptor.find(kLegacyBigintTypeUuid,
                          first + kLegacyBigintTypeUuid.size()) !=
              std::string::npos) {
        return refuse("CORE.AUTHORITY.CONFLICT",
                      "bigint_identity_migration_multiple_mapping",
                      requested.column_uuid);
      }
      descriptor.replace(first, kLegacyBigintTypeUuid.size(),
                         kCanonicalBigintTypeUuid);
    }
    if (matched_columns != 1) {
      return refuse("CORE.AUTHORITY.CONFLICT",
                    "bigint_identity_migration_column_mapping_conflict",
                    requested.column_uuid);
    }
    tables.push_back(std::move(updated));
  }

  const auto reservation = ReserveEventSequenceRange(
      context, "relation_metadata", MetadataStorePath(context), 1,
      [&context]() { return ScanNextMetadataEventSequence(context); });
  if (!reservation.ok) {
    result.diagnostic = reservation.diagnostic;
    return result;
  }
  for (std::size_t i = 0; i < tables.size(); ++i) {
    if (reservation.first <= request.rows[i].old_row_generation) {
      return refuse("DATATYPE.DESCRIPTOR_INVALID",
                    "bigint_identity_migration_generation_not_advanced",
                    request.rows[i].object_uuid);
    }
    tables[i].creator_tx = context.local_transaction_id;
    tables[i].event_sequence = reservation.first;
  }
  std::vector<std::string> decisions;
  decisions.reserve(request.rows.size());
  for (std::size_t i = 0; i < request.rows.size(); ++i) {
    decisions.push_back(BigintMigrationDecisionHash(
        request, request.rows[i], tables[i].event_sequence,
        context.transaction_uuid.canonical));
    if (decisions.back().empty()) {
      return refuse("DATATYPE.DESCRIPTOR_INVALID",
                    "bigint_identity_migration_hash_failed", "sha256");
    }
  }
  const std::string payload = CanonicalBigintMigrationPayload(
      request, context.local_transaction_id, reservation.first,
      context.transaction_uuid.canonical, tables,
      decisions);
  result.decision_sha256 = Sha256Tagged(payload);
  if (result.decision_sha256.empty()) {
    return refuse("DATATYPE.DESCRIPTOR_INVALID",
                  "bigint_identity_migration_hash_failed", "batch");
  }
  std::vector<std::string> fields{
      kRowStoreMagic,
      "BIGINT_IDENTITY_MIGRATION_BATCH",
      std::to_string(context.local_transaction_id),
      std::to_string(reservation.first),
      std::string(kBigintMigrationFormat),
      "sealed",
      result.decision_sha256,
      request.migration_id,
      context.transaction_uuid.canonical,
      request.prior_catalog_snapshot_uuid,
      request.new_catalog_snapshot_uuid,
      std::to_string(request.prior_catalog_generation),
      std::to_string(request.new_catalog_generation),
      std::to_string(request.rows.size())};
  for (std::size_t i = 0; i < request.rows.size(); ++i) {
    const auto& row = request.rows[i];
    const auto& table = tables[i];
    fields.insert(fields.end(), {
        row.object_uuid,
        row.column_uuid,
        std::string(kLegacyBigintTypeUuid),
        std::string(kCanonicalBigintTypeUuid),
        std::to_string(row.old_row_generation),
        std::to_string(table.event_sequence),
        decisions[i],
        EncodeCrudText(table.default_name),
        EncodeCrudPairs(table.columns),
        "0", "", "", ""});
  }
  // Publication is this single append. MGA visibility subsequently admits it
  // only for its creator or after the owning transaction commits.
  if (!AppendLine(MetadataStorePath(context), JoinLine(fields))) {
    return refuse("DATATYPE.DESCRIPTOR_INVALID",
                  "bigint_identity_migration_append_failed",
                  "sealed batch was not published");
  }
  result.ok = true;
  result.migrated_row_count = request.rows.size();
  result.diagnostic = OkDiagnostic();
  result.evidence = {
      {"migration_id", request.migration_id},
      {"transaction_uuid", context.transaction_uuid.canonical},
      {"prior_catalog_snapshot_uuid", request.prior_catalog_snapshot_uuid},
      {"new_catalog_snapshot_uuid", request.new_catalog_snapshot_uuid},
      {"prior_catalog_generation", std::to_string(request.prior_catalog_generation)},
      {"new_catalog_generation", std::to_string(request.new_catalog_generation)},
      {"decision_sha256", result.decision_sha256}};
  return result;
}

MgaInt32IdentityMigrationResult AppendMgaInt32IdentityMigrationBatch(
    const EngineRequestContext& context,
    const MgaInt32IdentityMigrationRequest& request) {
  constexpr const char* kOperation = "mga.int32_identity_migration";
  MgaInt32IdentityMigrationResult result;
  auto refuse = [&](std::string code, std::string key, std::string detail) {
    result.diagnostic = MakeEngineApiDiagnostic(
        std::move(code), std::move(key), std::move(detail));
    return result;
  };
  if (context.database_path.empty() || context.local_transaction_id == 0 ||
      context.transaction_uuid.canonical.empty()) {
    return refuse("DATATYPE.DESCRIPTOR_INVALID",
                  "int32_identity_migration_context_invalid",
                  "active MGA transaction and database path required");
  }
  const auto authority = ValidateMgaMutatingTransactionAuthority(
      context, kOperation);
  if (authority.error) {
    result.diagnostic = authority;
    return result;
  }
  if (request.migration_id != kInt32MigrationId || request.rows.empty() ||
      request.prior_catalog_snapshot_uuid.empty() ||
      request.new_catalog_snapshot_uuid.empty() ||
      request.prior_catalog_snapshot_uuid == request.new_catalog_snapshot_uuid ||
      request.prior_catalog_generation == 0 ||
      request.new_catalog_generation != request.prior_catalog_generation + 1 ||
      (!context.statement_metadata_snapshot_uuid.canonical.empty() &&
       context.statement_metadata_snapshot_uuid.canonical !=
           request.prior_catalog_snapshot_uuid)) {
    return refuse("DATATYPE.DESCRIPTOR_INVALID",
                  "int32_identity_migration_snapshot_stale",
                  "exact prior snapshot and consecutive catalog generation required");
  }
  if (context.query_cancellation_requested &&
      context.query_cancellation_requested()) {
    return refuse("PROCESS.CANCELLED",
                  "int32_identity_migration_cancelled_before_publication",
                  "cancellation was observed before the sealed batch append");
  }

  const auto current = LoadMgaRelationStoreState(context);
  if (!current.ok) {
    result.diagnostic = current.diagnostic;
    return result;
  }
  std::set<std::pair<std::string, std::string>> identities;
  std::map<std::string, std::uint64_t> object_generations;
  std::map<std::string, CrudTableRecord> updated_by_object;
  for (const auto& requested : request.rows) {
    if (requested.object_uuid.empty() || requested.column_uuid.empty() ||
        requested.old_row_generation == 0 ||
        !identities.emplace(requested.object_uuid,
                            requested.column_uuid).second) {
      return refuse("CORE.AUTHORITY.CONFLICT",
                    "int32_identity_migration_multiple_mapping",
                    "each object and column identity must occur exactly once");
    }
    const auto object_generation = object_generations.find(
        requested.object_uuid);
    if (object_generation != object_generations.end() &&
        object_generation->second != requested.old_row_generation) {
      return refuse("DATATYPE.DESCRIPTOR_INVALID",
                    "int32_identity_migration_row_generation_stale",
                    requested.object_uuid);
    }
    object_generations[requested.object_uuid] = requested.old_row_generation;

    auto updated = updated_by_object.find(requested.object_uuid);
    if (updated == updated_by_object.end()) {
      const CrudTableRecord* exact = nullptr;
      std::uint64_t newest_visible_generation = 0;
      for (const auto& table : current.state.relation_metadata.tables) {
        if (table.table_uuid != requested.object_uuid ||
            !CrudCreatorVisible(current.state.relation_metadata,
                                table.creator_tx,
                                table.event_sequence,
                                context.local_transaction_id)) {
          continue;
        }
        newest_visible_generation =
            std::max(newest_visible_generation, table.event_sequence);
        if (table.event_sequence != requested.old_row_generation) continue;
        if (exact != nullptr) {
          return refuse("CORE.AUTHORITY.CONFLICT",
                        "int32_identity_migration_multiple_mapping",
                        "multiple visible rows share the expected generation");
        }
        exact = &table;
      }
      if (exact == nullptr ||
          newest_visible_generation != requested.old_row_generation) {
        return refuse("DATATYPE.DESCRIPTOR_INVALID",
                      "int32_identity_migration_row_generation_stale",
                      requested.object_uuid);
      }
      if (exact->temporary || !exact->temporary_scope.empty() ||
          !exact->temporary_session_uuid.empty() ||
          !exact->on_commit_action.empty()) {
        return refuse("CORE.AUTHORITY.CONFLICT",
                      "int32_identity_migration_temporary_unsupported",
                      requested.object_uuid);
      }
      updated = updated_by_object.emplace(requested.object_uuid, *exact).first;
    }

    std::size_t matched_columns = 0;
    for (auto& [column_name, descriptor] : updated->second.columns) {
      (void)column_name;
      const auto descriptor_fields = StrictRelationDescriptorFields(descriptor);
      if (!descriptor_fields) {
        return refuse("CORE.AUTHORITY.CONFLICT",
                      "int32_identity_migration_descriptor_contradiction",
                      requested.object_uuid);
      }
      const auto column = descriptor_fields->find("column_uuid");
      if (column == descriptor_fields->end() ||
          column->second != requested.column_uuid) {
        continue;
      }
      ++matched_columns;
      const auto descriptor_uuid =
          descriptor_fields->find("datatype_descriptor_uuid");
      const auto type_uuid = descriptor_fields->find("type_uuid");
      if (descriptor_uuid == descriptor_fields->end() ||
          type_uuid == descriptor_fields->end() ||
          descriptor_uuid->second != kLegacyInt32DescriptorUuid ||
          type_uuid->second != kLegacyInt32TypeUuid) {
        return refuse("DATATYPE.DESCRIPTOR_INVALID",
                      "int32_identity_migration_legacy_identity_required",
                      requested.column_uuid);
      }
      const std::map<std::string,
                     std::pair<std::string_view, std::string_view>> replacements{
          {"datatype_descriptor_uuid",
           {kLegacyInt32DescriptorUuid, kCanonicalInt32DescriptorUuid}},
          {"type_uuid", {kLegacyInt32TypeUuid, kCanonicalInt32TypeUuid}}};
      if (!ReplaceExactRelationDescriptorIdentities(&descriptor,
                                                     replacements)) {
        return refuse("CORE.AUTHORITY.CONFLICT",
                      "int32_identity_migration_multiple_mapping",
                      requested.column_uuid);
      }
    }
    if (matched_columns != 1) {
      return refuse("CORE.AUTHORITY.CONFLICT",
                    "int32_identity_migration_column_mapping_conflict",
                    requested.column_uuid);
    }
  }

  if (context.query_cancellation_requested &&
      context.query_cancellation_requested()) {
    return refuse("PROCESS.CANCELLED",
                  "int32_identity_migration_cancelled_before_publication",
                  "cancellation was observed before the sealed batch append");
  }
  const auto reservation = ReserveEventSequenceRange(
      context, "relation_metadata", MetadataStorePath(context), 1,
      [&context]() { return ScanNextMetadataEventSequence(context); });
  if (!reservation.ok) {
    result.diagnostic = reservation.diagnostic;
    return result;
  }
  for (auto& [object_uuid, table] : updated_by_object) {
    if (reservation.first <= object_generations[object_uuid]) {
      return refuse("DATATYPE.DESCRIPTOR_INVALID",
                    "int32_identity_migration_generation_not_advanced",
                    object_uuid);
    }
    table.creator_tx = context.local_transaction_id;
    table.event_sequence = reservation.first;
  }
  std::vector<CrudTableRecord> tables;
  tables.reserve(request.rows.size());
  for (const auto& row : request.rows) {
    tables.push_back(updated_by_object.at(row.object_uuid));
  }
  std::vector<std::string> decisions;
  decisions.reserve(request.rows.size());
  for (std::size_t i = 0; i < request.rows.size(); ++i) {
    decisions.push_back(Int32MigrationDecisionHash(
        request, request.rows[i], tables[i].event_sequence,
        context.transaction_uuid.canonical));
    if (decisions.back().empty()) {
      return refuse("DATATYPE.DESCRIPTOR_INVALID",
                    "int32_identity_migration_hash_failed", "sha256");
    }
  }
  const std::string payload = CanonicalInt32MigrationPayload(
      request, context.local_transaction_id, reservation.first,
      context.transaction_uuid.canonical, tables, decisions);
  result.decision_sha256 = Sha256Tagged(payload);
  if (result.decision_sha256.empty()) {
    return refuse("DATATYPE.DESCRIPTOR_INVALID",
                  "int32_identity_migration_hash_failed", "batch");
  }
  std::vector<std::string> fields{
      kRowStoreMagic,
      "INT32_IDENTITY_MIGRATION_BATCH",
      std::to_string(context.local_transaction_id),
      std::to_string(reservation.first),
      std::string(kInt32MigrationFormat),
      "sealed",
      result.decision_sha256,
      request.migration_id,
      context.transaction_uuid.canonical,
      request.prior_catalog_snapshot_uuid,
      request.new_catalog_snapshot_uuid,
      std::to_string(request.prior_catalog_generation),
      std::to_string(request.new_catalog_generation),
      std::to_string(request.rows.size())};
  for (std::size_t i = 0; i < request.rows.size(); ++i) {
    const auto& row = request.rows[i];
    const auto& table = tables[i];
    fields.insert(fields.end(), {
        row.object_uuid,
        row.column_uuid,
        std::string(kLegacyInt32DescriptorUuid),
        std::string(kCanonicalInt32DescriptorUuid),
        std::string(kLegacyInt32TypeUuid),
        std::string(kCanonicalInt32TypeUuid),
        std::to_string(row.old_row_generation),
        std::to_string(table.event_sequence),
        decisions[i],
        EncodeCrudText(table.default_name),
        EncodeCrudPairs(table.columns),
        "0", "", "", ""});
  }
  if (!AppendLine(MetadataStorePath(context), JoinLine(fields))) {
    return refuse("DATATYPE.DESCRIPTOR_INVALID",
                  "int32_identity_migration_append_failed",
                  "sealed batch was not published");
  }
  result.ok = true;
  result.migrated_row_count = request.rows.size();
  result.diagnostic = OkDiagnostic();
  result.evidence = {
      {"migration_id", request.migration_id},
      {"transaction_uuid", context.transaction_uuid.canonical},
      {"prior_catalog_snapshot_uuid", request.prior_catalog_snapshot_uuid},
      {"new_catalog_snapshot_uuid", request.new_catalog_snapshot_uuid},
      {"prior_catalog_generation",
       std::to_string(request.prior_catalog_generation)},
      {"new_catalog_generation",
       std::to_string(request.new_catalog_generation)},
      {"old_descriptor_uuid", std::string(kLegacyInt32DescriptorUuid)},
      {"new_descriptor_uuid", std::string(kCanonicalInt32DescriptorUuid)},
      {"old_type_uuid", std::string(kLegacyInt32TypeUuid)},
      {"new_type_uuid", std::string(kCanonicalInt32TypeUuid)},
      {"decision_sha256", result.decision_sha256}};
  return result;
}

MgaTextIdentityMigrationResult AppendMgaTextIdentityMigrationBatch(
    const EngineRequestContext& context,
    const MgaTextIdentityMigrationRequest& request) {
  constexpr const char* kOperation = "mga.text_identity_migration";
  MgaTextIdentityMigrationResult result;
  auto refuse = [&](std::string code, std::string key, std::string detail) {
    result.diagnostic = MakeEngineApiDiagnostic(
        std::move(code), std::move(key), std::move(detail));
    return result;
  };
  if (context.database_path.empty() || context.local_transaction_id == 0 ||
      context.transaction_uuid.canonical.empty()) {
    return refuse("DATATYPE.DESCRIPTOR_INVALID",
                  "text_identity_migration_context_invalid",
                  "active MGA transaction and database path required");
  }
  const auto authority = ValidateMgaMutatingTransactionAuthority(
      context, kOperation);
  if (authority.error) {
    result.diagnostic = authority;
    return result;
  }
  if (!ExactCanonicalTextIdentityAuthorityAvailable(context) ||
      request.migration_id != kTextMigrationId || request.rows.empty() ||
      !CanonicalNonNilMigrationUuid(request.prior_catalog_snapshot_uuid) ||
      !CanonicalNonNilMigrationUuid(request.new_catalog_snapshot_uuid) ||
      request.prior_catalog_snapshot_uuid == request.new_catalog_snapshot_uuid ||
      request.prior_catalog_generation == 0 ||
      request.prior_catalog_generation ==
          std::numeric_limits<std::uint64_t>::max() ||
      request.new_catalog_generation != request.prior_catalog_generation + 1 ||
      context.catalog_generation_id != request.prior_catalog_generation ||
      context.statement_metadata_snapshot_uuid.canonical !=
          request.prior_catalog_snapshot_uuid) {
    return refuse("DATATYPE.DESCRIPTOR_INVALID",
                  "text_identity_migration_snapshot_stale",
                  "exact prior snapshot, registry row, and consecutive catalog generation required");
  }
  if (context.query_cancellation_requested &&
      context.query_cancellation_requested()) {
    return refuse("PROCESS.CANCELLED",
                  "text_identity_migration_cancelled_before_publication",
                  "cancellation was observed before the sealed batch append");
  }

  const auto current = LoadMgaRelationStoreState(context);
  if (!current.ok) {
    result.diagnostic = current.diagnostic;
    return result;
  }
  std::set<std::pair<std::string, std::string>> identities;
  std::map<std::string, std::uint64_t> object_generations;
  std::map<std::string, CrudTableRecord> updated_by_object;
  std::map<std::string, MgaRelationStorageDescriptor>
      updated_descriptors_by_object;
  std::map<std::string, std::set<std::string>> changed_columns_by_object;
  // Validate the complete mapping before transforming any descriptor.  This
  // keeps duplicate/stale authority precedence independent of row order and
  // prevents a malformed first row from masking a later duplicate identity.
  for (const auto& requested : request.rows) {
    const auto identity =
        std::make_pair(requested.object_uuid, requested.column_uuid);
    if (!CanonicalNonNilMigrationUuid(requested.object_uuid) ||
        !CanonicalNonNilMigrationUuid(requested.column_uuid) ||
        requested.old_row_generation == 0 ||
        identities.contains(identity)) {
      return refuse("CORE.AUTHORITY.CONFLICT",
                    "text_identity_migration_multiple_mapping",
                    "each object and column identity must occur exactly once");
    }
    identities.insert(identity);
    const auto object_generation = object_generations.find(
        requested.object_uuid);
    if (object_generation != object_generations.end() &&
        object_generation->second != requested.old_row_generation) {
      return refuse("DATATYPE.DESCRIPTOR_INVALID",
                    "text_identity_migration_row_generation_stale",
                    requested.object_uuid);
    }
    object_generations[requested.object_uuid] = requested.old_row_generation;
  }

  for (const auto& requested : request.rows) {
    auto updated = updated_by_object.find(requested.object_uuid);
    if (updated == updated_by_object.end()) {
      const CrudTableRecord* exact = nullptr;
      std::uint64_t newest_visible_generation = 0;
      for (const auto& table : current.state.relation_metadata.tables) {
        if (table.table_uuid != requested.object_uuid ||
            !CrudCreatorVisible(current.state.relation_metadata,
                                table.creator_tx,
                                table.event_sequence,
                                context.local_transaction_id)) {
          continue;
        }
        newest_visible_generation =
            std::max(newest_visible_generation, table.event_sequence);
        if (table.event_sequence != requested.old_row_generation) continue;
        if (exact != nullptr) {
          return refuse("CORE.AUTHORITY.CONFLICT",
                        "text_identity_migration_multiple_mapping",
                        "multiple visible rows share the expected generation");
        }
        exact = &table;
      }
      if (exact == nullptr ||
          newest_visible_generation != requested.old_row_generation) {
        return refuse("DATATYPE.DESCRIPTOR_INVALID",
                      "text_identity_migration_row_generation_stale",
                      requested.object_uuid);
      }
      if (exact->temporary || !exact->temporary_scope.empty() ||
          !exact->temporary_session_uuid.empty() ||
          !exact->on_commit_action.empty()) {
        return refuse("CORE.AUTHORITY.CONFLICT",
                      "text_identity_migration_temporary_unsupported",
                      requested.object_uuid);
      }
      const auto loaded_descriptor = LoadMgaRelationStorageDescriptor(
          context, requested.object_uuid);
      if (!loaded_descriptor.ok ||
          loaded_descriptor.descriptor.database_uuid.canonical !=
              context.database_uuid.canonical ||
          loaded_descriptor.descriptor.relation_uuid.canonical !=
              requested.object_uuid ||
          loaded_descriptor.descriptor.relation_generation !=
              requested.old_row_generation) {
        return refuse("DATATYPE.DESCRIPTOR_INVALID",
                      "text_identity_migration_relation_snapshot_stale",
                      requested.object_uuid);
      }
      updated = updated_by_object.emplace(requested.object_uuid, *exact).first;
      updated_descriptors_by_object.emplace(
          requested.object_uuid, loaded_descriptor.descriptor);
    }

    auto& storage = updated_descriptors_by_object.at(requested.object_uuid);
    auto storage_column = storage.columns.end();
    std::size_t storage_matches = 0;
    for (auto candidate = storage.columns.begin();
         candidate != storage.columns.end(); ++candidate) {
      if (candidate->column_uuid.canonical != requested.column_uuid) continue;
      ++storage_matches;
      storage_column = candidate;
    }
    if (storage_matches != 1 || storage_column == storage.columns.end() ||
        storage_column->canonical_name_key.empty() ||
        storage_column->column_generation != requested.old_row_generation ||
        storage_column->value_descriptor.descriptor_uuid.canonical !=
            requested.column_uuid) {
      return refuse("DATATYPE.DESCRIPTOR_INVALID",
                    "text_identity_migration_relation_column_stale",
                    requested.column_uuid);
    }

    std::size_t matched_columns = 0;
    for (auto& [column_name, descriptor] : updated->second.columns) {
      if (column_name != storage_column->canonical_name_key) continue;
      ++matched_columns;
      auto migrated_storage_descriptor =
          storage_column->value_descriptor.encoded_descriptor;
      if (!RewriteLegacyTextDescriptor(context, &descriptor,
                                       requested.column_uuid) ||
          !RewriteLegacyTextDescriptor(context, &migrated_storage_descriptor,
                                       requested.column_uuid) ||
          descriptor != migrated_storage_descriptor) {
        return refuse("DATATYPE.DESCRIPTOR_INVALID",
                      "text_identity_migration_legacy_identity_required",
                      requested.column_uuid);
      }
      const auto migrated_fields = StrictRelationDescriptorFields(descriptor);
      if (!migrated_fields ||
          migrated_fields->at("nullable") !=
              (storage_column->nullable ? "true" : "false")) {
        return refuse("CORE.AUTHORITY.CONFLICT",
                      "text_identity_migration_nullability_conflict",
                      requested.column_uuid);
      }
      storage_column->value_descriptor.descriptor_uuid.canonical =
          requested.column_uuid;
      storage_column->value_descriptor.canonical_type_name = "text";
      storage_column->value_descriptor.encoded_descriptor = descriptor;
      changed_columns_by_object[requested.object_uuid].insert(
          requested.column_uuid);
    }
    if (matched_columns != 1) {
      return refuse("CORE.AUTHORITY.CONFLICT",
                    "text_identity_migration_column_mapping_conflict",
                    requested.column_uuid);
    }
  }

  if (context.query_cancellation_requested &&
      context.query_cancellation_requested()) {
    return refuse("PROCESS.CANCELLED",
                  "text_identity_migration_cancelled_before_publication",
                  "cancellation was observed before the sealed batch append");
  }
  const bool requires_contextual_policy = std::ranges::any_of(
      updated_descriptors_by_object, [&context](const auto& entry) {
        return std::ranges::any_of(entry.second.columns,
                                   [&context](const auto& column) {
          return ExactCanonicalMigratedTextDescriptor(
                     context, column.value_descriptor.encoded_descriptor,
                     column.column_uuid.canonical) &&
                 !column.charset_uuid.empty() &&
                 !column.collation_uuid.empty();
        });
      });
  EngineContextualTextPolicyRowSetV2 policy_rows;
  if (requires_contextual_policy) {
    const auto policy =
        LoadCurrentEngineContextualTextPolicyRowSetForPublicationV2();
    if (!policy.ok) {
      result.diagnostic = policy.diagnostic;
      return result;
    }
    policy_rows = policy.rows;
  }
  std::vector<std::string> allocator_lines;
  const auto reservation = ReserveEventSequenceRange(
      context, "relation_metadata", MetadataStorePath(context), 1,
      [&context]() { return ScanNextMetadataEventSequence(context); },
      &allocator_lines);
  if (!reservation.ok) {
    result.diagnostic = reservation.diagnostic;
    return result;
  }
  const auto abandon_reservation = [&]() {
    AbandonDeferredEventSequenceReservation(reservation);
    allocator_lines.clear();
  };
  std::map<std::string, CrudSealedRelationDescriptorSnapshot>
      sealed_descriptors_by_object;
  for (auto& [object_uuid, table] : updated_by_object) {
    if (reservation.first <= object_generations[object_uuid]) {
      abandon_reservation();
      return refuse("DATATYPE.DESCRIPTOR_INVALID",
                    "text_identity_migration_generation_not_advanced",
                    object_uuid);
    }
    table.creator_tx = context.local_transaction_id;
    table.event_sequence = reservation.first;
    auto& descriptor = updated_descriptors_by_object.at(object_uuid);
    descriptor.relation_generation = reservation.first;
    for (auto& column : descriptor.columns) {
      if (changed_columns_by_object[object_uuid].contains(
              column.column_uuid.canonical)) {
        column.column_generation = reservation.first;
      }
    }
    const auto descriptor_validation =
        ValidateMgaRelationStorageDescriptor(descriptor);
    if (descriptor_validation.error) {
      abandon_reservation();
      return refuse("DATATYPE.DESCRIPTOR_INVALID",
                    "text_identity_migration_relation_descriptor_invalid",
                    descriptor_validation.detail);
    }
    const auto serialized = SerializeMgaRelationStorageDescriptor(descriptor);
    if (DeserializeMgaRelationStorageDescriptor(serialized)
            .relation_generation != reservation.first) {
      abandon_reservation();
      return refuse("DATATYPE.DESCRIPTOR_INVALID",
                    "text_identity_migration_relation_descriptor_roundtrip_failed",
                    object_uuid);
    }
    MgaSealedContextualTextDescriptorMaterialV2 material;
    EngineApiDiagnostic material_diagnostic;
    if (!BuildMgaSealedContextualTextDescriptorMaterialV2(
            context, table, descriptor, policy_rows, &material,
            &material_diagnostic)) {
      abandon_reservation();
      return refuse("CTB.TEXT.DESCRIPTOR_INVALID",
                    "text_identity_migration_sidecar_set_invalid",
                    material_diagnostic.detail);
    }
    CrudSealedRelationDescriptorSnapshot snapshot;
    snapshot.creator_tx = table.creator_tx;
    snapshot.event_sequence = table.event_sequence;
    snapshot.relation_uuid = object_uuid;
    snapshot.relation_descriptor_uuid =
        material.relation_descriptor.descriptor_uuid.canonical;
    snapshot.relation_descriptor_generation =
        material.relation_descriptor.descriptor_generation;
    snapshot.descriptor_field_count =
        material.sealed_set.descriptor_field_count;
    snapshot.descriptor_field_bytes =
        material.sealed_set.descriptor_field_bytes;
    snapshot.contextual_sidecar_count =
        material.sealed_set.contextual_sidecar_count;
    snapshot.descriptor_fields.reserve(
        material.sealed_set.descriptor_fields.size());
    for (const auto& field : material.sealed_set.descriptor_fields) {
      snapshot.descriptor_fields.emplace_back(
          std::string(field.key_raw_bytes.begin(),
                      field.key_raw_bytes.end()),
          std::string(field.value_raw_bytes.begin(),
                      field.value_raw_bytes.end()));
    }
    sealed_descriptors_by_object.emplace(object_uuid, std::move(snapshot));
  }

  std::vector<CrudTableRecord> tables;
  std::vector<CrudSealedRelationDescriptorSnapshot>
      relation_descriptor_snapshots;
  tables.reserve(request.rows.size());
  relation_descriptor_snapshots.reserve(request.rows.size());
  for (const auto& row : request.rows) {
    tables.push_back(updated_by_object.at(row.object_uuid));
    relation_descriptor_snapshots.push_back(
        sealed_descriptors_by_object.at(row.object_uuid));
  }
  std::vector<std::string> decisions;
  decisions.reserve(request.rows.size());
  for (std::size_t i = 0; i < request.rows.size(); ++i) {
    decisions.push_back(TextMigrationDecisionHash(
        request, request.rows[i], tables[i].event_sequence,
        context.transaction_uuid.canonical,
        context.datatype_catalog_snapshot_uuid.canonical,
        context.datatype_catalog_generation,
        context.datatype_registry_generation,
        relation_descriptor_snapshots[i]));
    if (decisions.back().empty()) {
      abandon_reservation();
      return refuse("DATATYPE.DESCRIPTOR_INVALID",
                    "text_identity_migration_hash_failed", "sha256");
    }
  }
  const std::string payload = CanonicalTextMigrationPayload(
      request, context.local_transaction_id, reservation.first,
      context.transaction_uuid.canonical,
      context.datatype_catalog_snapshot_uuid.canonical,
      context.datatype_catalog_generation,
      context.datatype_registry_generation,
      tables, relation_descriptor_snapshots, decisions);
  result.decision_sha256 = Sha256Tagged(payload);
  if (result.decision_sha256.empty()) {
    abandon_reservation();
    return refuse("DATATYPE.DESCRIPTOR_INVALID",
                  "text_identity_migration_hash_failed", "batch");
  }
  std::vector<std::string> fields{
      kRowStoreMagic,
      "TEXT_IDENTITY_MIGRATION_BATCH",
      std::to_string(context.local_transaction_id),
      std::to_string(reservation.first),
      std::string(kTextMigrationFormat),
      "sealed",
      result.decision_sha256,
      request.migration_id,
      context.transaction_uuid.canonical,
      request.prior_catalog_snapshot_uuid,
      request.new_catalog_snapshot_uuid,
      std::to_string(request.prior_catalog_generation),
      std::to_string(request.new_catalog_generation),
      context.datatype_catalog_snapshot_uuid.canonical,
      std::to_string(context.datatype_catalog_generation),
      std::to_string(context.datatype_registry_generation),
      std::to_string(request.rows.size())};
  for (std::size_t i = 0; i < request.rows.size(); ++i) {
    const auto& row = request.rows[i];
    const auto& table = tables[i];
    const auto& snapshot = relation_descriptor_snapshots[i];
    fields.insert(fields.end(), {
        row.object_uuid,
        row.column_uuid,
        std::string(kLegacyTextDescriptorUuid),
        std::string(kCanonicalTextDescriptorUuid),
        std::string(kLegacyTextTypeUuid),
        std::string(kCanonicalTextTypeUuid),
        std::string(kCanonicalTextCodecUuid),
        std::string(kCanonicalTextCodecId),
        "1",
        "1",
        std::to_string(row.old_row_generation),
        std::to_string(table.event_sequence),
        decisions[i],
        EncodeCrudText(table.default_name),
        EncodeCrudPairs(table.columns),
        EncodeCrudPairs(snapshot.descriptor_fields),
        snapshot.relation_descriptor_uuid,
        std::to_string(snapshot.relation_descriptor_generation),
        std::to_string(snapshot.descriptor_field_count),
        std::to_string(snapshot.descriptor_field_bytes),
        std::to_string(snapshot.contextual_sidecar_count),
        "0", "", "", ""});
  }
  if (!AppendLine(MetadataStorePath(context), JoinLine(fields))) {
    abandon_reservation();
    return refuse("DATATYPE.DESCRIPTOR_INVALID",
                  "text_identity_migration_append_failed",
                  "sealed batch and relation descriptor were not published");
  }
  // The sealed metadata line is authoritative and can bootstrap the allocator
  // on restart. Publish allocator acceleration only after the one-line seal;
  // failure here cannot turn an already-visible migration into a refusal.
  (void)AppendDeferredEventSequenceAllocatorLines(
      context, &allocator_lines, nullptr);
  result.ok = true;
  result.migrated_row_count = request.rows.size();
  result.diagnostic = OkDiagnostic();
  result.evidence = {
      {"migration_id", request.migration_id},
      {"transaction_uuid", context.transaction_uuid.canonical},
      {"datatype_catalog_snapshot_uuid",
       context.datatype_catalog_snapshot_uuid.canonical},
      {"datatype_catalog_generation",
       std::to_string(context.datatype_catalog_generation)},
      {"datatype_registry_generation",
       std::to_string(context.datatype_registry_generation)},
      {"prior_catalog_snapshot_uuid", request.prior_catalog_snapshot_uuid},
      {"new_catalog_snapshot_uuid", request.new_catalog_snapshot_uuid},
      {"prior_catalog_generation",
       std::to_string(request.prior_catalog_generation)},
      {"new_catalog_generation",
       std::to_string(request.new_catalog_generation)},
      {"old_descriptor_uuid", std::string(kLegacyTextDescriptorUuid)},
      {"new_descriptor_uuid", std::string(kCanonicalTextDescriptorUuid)},
      {"old_type_uuid", std::string(kLegacyTextTypeUuid)},
      {"new_type_uuid", std::string(kCanonicalTextTypeUuid)},
      {"new_codec_uuid", std::string(kCanonicalTextCodecUuid)},
      {"new_codec_id", std::string(kCanonicalTextCodecId)},
      {"new_codec_version", "1"},
      {"new_codec_generation", "1"},
      {"decision_sha256", result.decision_sha256}};
  return result;
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
                                                const RelationReadSnapshot& state,
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
                                                 const RelationReadSnapshot& state,
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
  const auto cleanup_work = HasMgaTemporaryCleanupMetadataWork(context,
                                                              include_delete_rows,
                                                              include_preserve_rows,
                                                              retire_private_metadata);
  if (!cleanup_work.ok) { return cleanup_work.diagnostic; }
  if (!cleanup_work.has_work) { return OkDiagnostic(); }

  auto load_context = context;
  load_context.local_transaction_id = local_transaction_id;
  auto loaded = LoadMgaRelationStoreState(load_context);
  if (!loaded.ok) { return loaded.diagnostic; }
  RelationReadSnapshot state = BuildCrudCompatibilityStateFromMga(loaded.state);
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

namespace {

constexpr std::array<std::uint8_t, 8> kDmlUpdateDurableFrameMagic{{
    'S', 'B', 'M', 'D', 'U', 'O', 'P', '1'}};
constexpr std::uint16_t kDmlUpdateDurableFrameVersion = 1;
constexpr std::uint32_t kDmlUpdateDurableFrameHeaderBytes = 352;
constexpr std::uint64_t kDmlUpdateDurableMaximumFrameBytes =
    static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max());
constexpr std::string_view kDmlUpdateDurableFrameEvidenceDomain =
    "ScratchBird.MgaDmlUpdateDurableOperationFrame.V1";

enum class DmlUpdateDurableFrameKindV1 : std::uint16_t {
  authority_snapshot = 1,
  journal = 2,
  statement_savepoint = 3,
  recovery_observation = 4,
  authority_reservation = 5,
  // Contains the already encoded canonical journal frame that may be
  // published after the statement barrier without any further
  // encode/hash/allocation step.
  prepared_successor = 6,
  // Durable tombstone for a prepared publication successor that was
  // cancelled before the publication barrier.  The tombstone names the exact
  // staged successor in its fixed frame fields; recovery clears that staged
  // successor before considering any later aborted successor.
  prepared_successor_invalidated = 7,
};

struct DmlUpdateDurableFrameV1 {
  DmlUpdateDurableFrameKindV1 kind =
      DmlUpdateDurableFrameKindV1::authority_snapshot;
  MgaDmlUpdateDurableOperationIdentityV1 identity;
  std::uint64_t sequence = 0;
  std::uint8_t state = 0;
  std::uint8_t flags = 0;
  MgaDmlUpdateDurableSha256V1 prior_record_sha256{};
  MgaDmlUpdateDurableSha256V1 record_evidence_sha256{};
  std::vector<std::uint8_t> payload;
};

struct DmlUpdateDurableFrameLoadV1 {
  bool ok = false;
  bool missing = false;
  std::string detail;
  std::vector<DmlUpdateDurableFrameV1> frames;
};

void DmlUpdateDurablePutU16(std::vector<std::uint8_t>* bytes,
                            std::size_t offset, std::uint16_t value) {
  (*bytes)[offset] = static_cast<std::uint8_t>(value & 0xffu);
  (*bytes)[offset + 1] =
      static_cast<std::uint8_t>((value >> 8u) & 0xffu);
}

void DmlUpdateDurablePutU32(std::vector<std::uint8_t>* bytes,
                            std::size_t offset, std::uint32_t value) {
  for (std::size_t index = 0; index < 4; ++index) {
    (*bytes)[offset + index] =
        static_cast<std::uint8_t>((value >> (index * 8u)) & 0xffu);
  }
}

void DmlUpdateDurablePutU64(std::vector<std::uint8_t>* bytes,
                            std::size_t offset, std::uint64_t value) {
  for (std::size_t index = 0; index < 8; ++index) {
    (*bytes)[offset + index] =
        static_cast<std::uint8_t>((value >> (index * 8u)) & 0xffu);
  }
}

bool DmlUpdateDurableReadU16(std::span<const std::uint8_t> bytes,
                             std::size_t offset, std::uint16_t* value) {
  if (value == nullptr || offset > bytes.size() ||
      bytes.size() - offset < 2) {
    return false;
  }
  *value = static_cast<std::uint16_t>(bytes[offset]) |
           (static_cast<std::uint16_t>(bytes[offset + 1]) << 8u);
  return true;
}

bool DmlUpdateDurableReadU32(std::span<const std::uint8_t> bytes,
                             std::size_t offset, std::uint32_t* value) {
  if (value == nullptr || offset > bytes.size() ||
      bytes.size() - offset < 4) {
    return false;
  }
  std::uint32_t parsed = 0;
  for (std::size_t index = 0; index < 4; ++index) {
    parsed |= static_cast<std::uint32_t>(bytes[offset + index])
              << (index * 8u);
  }
  *value = parsed;
  return true;
}

bool DmlUpdateDurableReadU64(std::span<const std::uint8_t> bytes,
                             std::size_t offset, std::uint64_t* value) {
  if (value == nullptr || offset > bytes.size() ||
      bytes.size() - offset < 8) {
    return false;
  }
  std::uint64_t parsed = 0;
  for (std::size_t index = 0; index < 8; ++index) {
    parsed |= static_cast<std::uint64_t>(bytes[offset + index])
              << (index * 8u);
  }
  *value = parsed;
  return true;
}

bool DmlUpdateDurableZero(std::span<const std::uint8_t> bytes) {
  return std::all_of(bytes.begin(), bytes.end(),
                     [](std::uint8_t value) { return value == 0; });
}

bool DmlUpdateDurableUuidBytes(
    std::string_view uuid, std::array<std::uint8_t, 16>* bytes) {
  if (bytes == nullptr || uuid.empty()) return false;
  const auto parsed = scratchbird::core::uuid::ParseUuid(std::string(uuid));
  if (!parsed.ok() || scratchbird::core::uuid::IsNilUuid(parsed.value) ||
      scratchbird::core::uuid::UuidToString(parsed.value) != uuid) {
    return false;
  }
  std::copy(parsed.value.bytes.begin(), parsed.value.bytes.end(),
            bytes->begin());
  return true;
}

bool DmlUpdateDurableTypedUuid(
    std::string_view uuid, scratchbird::wire::TypedUpdateUuid* bytes) {
  if (bytes == nullptr) return false;
  std::array<std::uint8_t, 16> parsed{};
  if (!DmlUpdateDurableUuidBytes(uuid, &parsed)) return false;
  std::copy(parsed.begin(), parsed.end(), bytes->begin());
  return true;
}

std::string DmlUpdateDurableUuidText(
    std::span<const std::uint8_t> bytes) {
  if (bytes.size() != 16 || DmlUpdateDurableZero(bytes)) return {};
  scratchbird::core::platform::Uuid value{};
  std::copy(bytes.begin(), bytes.end(), value.bytes.begin());
  return scratchbird::core::uuid::UuidToString(value);
}

std::string DmlUpdateDurableTypedUuidText(
    const scratchbird::wire::TypedUpdateUuid& bytes) {
  return DmlUpdateDurableUuidText(
      std::span<const std::uint8_t>(bytes.data(), bytes.size()));
}

bool DmlUpdateDurablePutUuid(std::vector<std::uint8_t>* bytes,
                             std::size_t offset, std::string_view uuid) {
  if (bytes == nullptr || offset > bytes->size() ||
      bytes->size() - offset < 16) {
    return false;
  }
  std::array<std::uint8_t, 16> parsed{};
  if (!DmlUpdateDurableUuidBytes(uuid, &parsed)) return false;
  std::copy(parsed.begin(), parsed.end(), bytes->begin() + offset);
  return true;
}

bool DmlUpdateDurableBaseIdentityValid(
    const MgaDmlUpdateDurableOperationIdentityV1& identity) {
  std::array<std::uint8_t, 16> ignored{};
  return DmlUpdateDurableUuidBytes(identity.database_uuid, &ignored) &&
         DmlUpdateDurableUuidBytes(identity.owning_transaction_uuid,
                                   &ignored) &&
         identity.owning_local_transaction_id != 0 &&
         DmlUpdateDurableUuidBytes(
             identity.authenticated_statement_receipt_uuid, &ignored) &&
         DmlUpdateDurableUuidBytes(identity.operation_uuid, &ignored) &&
         DmlUpdateDurableUuidBytes(identity.descriptor_uuid, &ignored) &&
         identity.descriptor_generation != 0 &&
         DmlUpdateDurableUuidBytes(identity.recovery_token_uuid, &ignored) &&
         identity.recovery_generation != 0;
}

bool DmlUpdateDurableIdentityValid(
    const MgaDmlUpdateDurableOperationIdentityV1& identity) {
  std::array<std::uint8_t, 16> ignored{};
  return DmlUpdateDurableBaseIdentityValid(identity) &&
         identity.operation_generation != 0 &&
         DmlUpdateDurableUuidBytes(identity.validated_durable_handle_uuid,
                                   &ignored) &&
         identity.validated_durable_handle_generation != 0 &&
         DmlUpdateDurableUuidBytes(identity.reserved_statement_barrier_uuid,
                                   &ignored) &&
         identity.reserved_statement_barrier_generation != 0;
}

bool DmlUpdateDurableIdentityMatchesContext(
    const EngineRequestContext& context,
    const MgaDmlUpdateDurableOperationIdentityV1& identity) {
  return !context.database_path.empty() &&
         DmlUpdateDurableIdentityValid(identity) &&
         context.database_uuid.canonical == identity.database_uuid &&
         context.transaction_uuid.canonical ==
             identity.owning_transaction_uuid &&
         context.local_transaction_id == identity.owning_local_transaction_id &&
         context.statement_receipt_uuid.canonical ==
             identity.authenticated_statement_receipt_uuid;
}

std::string DmlUpdateDurableDescriptorPath(
    const EngineRequestContext& context,
    const MgaDmlUpdateDurableOperationIdentityV1& identity) {
  return DmlUpdateDurableOperationStorePath(context) + "/" +
         identity.descriptor_uuid + ".duop";
}

std::string DmlUpdateDurableSavepointPath(
    const EngineRequestContext& context, std::string_view savepoint_uuid) {
  return DmlUpdateStatementSavepointBinaryStorePath(context) + "/" +
         std::string(savepoint_uuid) + ".dups";
}

MgaDmlUpdateDurableSha256V1 DmlUpdateDurableSha256(
    std::span<const std::uint8_t> bytes) {
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(
      bytes.data(), bytes.size());
  return digest.ok() ? digest.digest : MgaDmlUpdateDurableSha256V1{};
}

MgaDmlUpdateDurableSha256V1 DmlUpdateDurableFrameSha256(
    std::span<const std::uint8_t> header_without_checksum) {
  std::vector<std::uint8_t> material;
  material.reserve(kDmlUpdateDurableFrameEvidenceDomain.size() +
                   header_without_checksum.size());
  material.insert(material.end(),
                  kDmlUpdateDurableFrameEvidenceDomain.begin(),
                  kDmlUpdateDurableFrameEvidenceDomain.end());
  material.insert(material.end(), header_without_checksum.begin(),
                  header_without_checksum.end());
  return DmlUpdateDurableSha256(material);
}

bool DmlUpdateDurableEncodeFrame(
    const DmlUpdateDurableFrameV1& frame,
    std::vector<std::uint8_t>* encoded) {
  const bool savepoint_frame =
      frame.kind == DmlUpdateDurableFrameKindV1::statement_savepoint;
  if (encoded == nullptr ||
      !(savepoint_frame
            ? DmlUpdateDurableBaseIdentityValid(frame.identity)
            : DmlUpdateDurableIdentityValid(frame.identity)) ||
      frame.payload.size() > kDmlUpdateDurableMaximumFrameBytes ||
      static_cast<std::uint64_t>(frame.payload.size()) >
          std::numeric_limits<std::uint64_t>::max() -
              kDmlUpdateDurableFrameHeaderBytes) {
    return false;
  }
  std::vector<std::uint8_t> header(kDmlUpdateDurableFrameHeaderBytes, 0);
  std::copy(kDmlUpdateDurableFrameMagic.begin(),
            kDmlUpdateDurableFrameMagic.end(), header.begin());
  DmlUpdateDurablePutU16(&header, 8, kDmlUpdateDurableFrameVersion);
  DmlUpdateDurablePutU16(
      &header, 10, static_cast<std::uint16_t>(frame.kind));
  DmlUpdateDurablePutU32(&header, 12,
                         kDmlUpdateDurableFrameHeaderBytes);
  DmlUpdateDurablePutU64(
      &header, 16,
      kDmlUpdateDurableFrameHeaderBytes + frame.payload.size());
  DmlUpdateDurablePutU64(&header, 24, frame.payload.size());
  if (!DmlUpdateDurablePutUuid(&header, 32, frame.identity.database_uuid) ||
      !DmlUpdateDurablePutUuid(
          &header, 48, frame.identity.owning_transaction_uuid) ||
      !DmlUpdateDurablePutUuid(
          &header, 72,
          frame.identity.authenticated_statement_receipt_uuid) ||
      !DmlUpdateDurablePutUuid(&header, 88, frame.identity.operation_uuid) ||
      !DmlUpdateDurablePutUuid(&header, 112,
                               frame.identity.descriptor_uuid) ||
      !DmlUpdateDurablePutUuid(&header, 136,
                               frame.identity.recovery_token_uuid) ||
      (!savepoint_frame &&
       !DmlUpdateDurablePutUuid(
           &header, 160,
           frame.identity.validated_durable_handle_uuid)) ||
      (!savepoint_frame &&
       !DmlUpdateDurablePutUuid(
           &header, 184,
           frame.identity.reserved_statement_barrier_uuid))) {
    return false;
  }
  DmlUpdateDurablePutU64(
      &header, 64, frame.identity.owning_local_transaction_id);
  DmlUpdateDurablePutU64(&header, 104,
                         frame.identity.operation_generation);
  DmlUpdateDurablePutU64(&header, 128,
                         frame.identity.descriptor_generation);
  DmlUpdateDurablePutU64(&header, 152,
                         frame.identity.recovery_generation);
  DmlUpdateDurablePutU64(
      &header, 176, frame.identity.validated_durable_handle_generation);
  DmlUpdateDurablePutU64(
      &header, 200, frame.identity.reserved_statement_barrier_generation);
  DmlUpdateDurablePutU64(&header, 208, frame.sequence);
  header[216] = frame.state;
  header[217] = frame.flags;
  std::copy(frame.prior_record_sha256.begin(),
            frame.prior_record_sha256.end(), header.begin() + 224);
  std::copy(frame.record_evidence_sha256.begin(),
            frame.record_evidence_sha256.end(), header.begin() + 256);
  const auto payload_sha = DmlUpdateDurableSha256(frame.payload);
  std::copy(payload_sha.begin(), payload_sha.end(), header.begin() + 288);
  const auto frame_sha = DmlUpdateDurableFrameSha256(
      std::span<const std::uint8_t>(header).first(320));
  std::copy(frame_sha.begin(), frame_sha.end(), header.begin() + 320);
  if (DmlUpdateDurableZero(payload_sha) || DmlUpdateDurableZero(frame_sha)) {
    return false;
  }
  encoded->clear();
  encoded->reserve(header.size() + frame.payload.size());
  encoded->insert(encoded->end(), header.begin(), header.end());
  encoded->insert(encoded->end(), frame.payload.begin(), frame.payload.end());
  return true;
}

bool DmlUpdateDurableDecodeFrame(
    std::span<const std::uint8_t> encoded,
    DmlUpdateDurableFrameV1* frame, std::string* detail) {
  const auto fail = [&](std::string reason) {
    if (detail != nullptr) *detail = std::move(reason);
    return false;
  };
  if (frame == nullptr || encoded.size() < kDmlUpdateDurableFrameHeaderBytes) {
    return fail("durable_frame_header_truncated");
  }
  if (!std::equal(kDmlUpdateDurableFrameMagic.begin(),
                  kDmlUpdateDurableFrameMagic.end(), encoded.begin())) {
    return fail("durable_frame_magic_invalid");
  }
  std::uint16_t version = 0;
  std::uint16_t kind = 0;
  std::uint32_t header_bytes = 0;
  std::uint64_t total_bytes = 0;
  std::uint64_t payload_bytes = 0;
  if (!DmlUpdateDurableReadU16(encoded, 8, &version) ||
      !DmlUpdateDurableReadU16(encoded, 10, &kind) ||
      !DmlUpdateDurableReadU32(encoded, 12, &header_bytes) ||
      !DmlUpdateDurableReadU64(encoded, 16, &total_bytes) ||
      !DmlUpdateDurableReadU64(encoded, 24, &payload_bytes) ||
      version != kDmlUpdateDurableFrameVersion ||
      header_bytes != kDmlUpdateDurableFrameHeaderBytes ||
      total_bytes != encoded.size() ||
      payload_bytes != encoded.size() - header_bytes ||
      payload_bytes > kDmlUpdateDurableMaximumFrameBytes ||
      (kind < static_cast<std::uint16_t>(
                  DmlUpdateDurableFrameKindV1::authority_snapshot) ||
       kind > static_cast<std::uint16_t>(
                  DmlUpdateDurableFrameKindV1::prepared_successor_invalidated)) ||
      !DmlUpdateDurableZero(encoded.subspan(218, 6))) {
    return fail("durable_frame_extent_or_header_invalid");
  }
  frame->kind = static_cast<DmlUpdateDurableFrameKindV1>(kind);
  frame->identity.database_uuid =
      DmlUpdateDurableUuidText(encoded.subspan(32, 16));
  frame->identity.owning_transaction_uuid =
      DmlUpdateDurableUuidText(encoded.subspan(48, 16));
  frame->identity.authenticated_statement_receipt_uuid =
      DmlUpdateDurableUuidText(encoded.subspan(72, 16));
  frame->identity.operation_uuid =
      DmlUpdateDurableUuidText(encoded.subspan(88, 16));
  frame->identity.descriptor_uuid =
      DmlUpdateDurableUuidText(encoded.subspan(112, 16));
  frame->identity.recovery_token_uuid =
      DmlUpdateDurableUuidText(encoded.subspan(136, 16));
  frame->identity.validated_durable_handle_uuid =
      DmlUpdateDurableUuidText(encoded.subspan(160, 16));
  frame->identity.reserved_statement_barrier_uuid =
      DmlUpdateDurableUuidText(encoded.subspan(184, 16));
  if (!DmlUpdateDurableReadU64(
          encoded, 64, &frame->identity.owning_local_transaction_id) ||
      !DmlUpdateDurableReadU64(
          encoded, 104, &frame->identity.operation_generation) ||
      !DmlUpdateDurableReadU64(
          encoded, 128, &frame->identity.descriptor_generation) ||
      !DmlUpdateDurableReadU64(
          encoded, 152, &frame->identity.recovery_generation) ||
      !DmlUpdateDurableReadU64(
          encoded, 176,
          &frame->identity.validated_durable_handle_generation) ||
      !DmlUpdateDurableReadU64(
          encoded, 200,
          &frame->identity.reserved_statement_barrier_generation) ||
      !DmlUpdateDurableReadU64(encoded, 208, &frame->sequence) ||
      !(static_cast<DmlUpdateDurableFrameKindV1>(kind) ==
                DmlUpdateDurableFrameKindV1::statement_savepoint
            ? DmlUpdateDurableBaseIdentityValid(frame->identity)
            : DmlUpdateDurableIdentityValid(frame->identity))) {
    return fail("durable_frame_identity_invalid");
  }
  frame->state = encoded[216];
  frame->flags = encoded[217];
  std::copy_n(encoded.begin() + 224, 32,
              frame->prior_record_sha256.begin());
  std::copy_n(encoded.begin() + 256, 32,
              frame->record_evidence_sha256.begin());
  MgaDmlUpdateDurableSha256V1 payload_sha{};
  MgaDmlUpdateDurableSha256V1 frame_sha{};
  std::copy_n(encoded.begin() + 288, 32, payload_sha.begin());
  std::copy_n(encoded.begin() + 320, 32, frame_sha.begin());
  const auto payload = encoded.subspan(header_bytes, payload_bytes);
  if (DmlUpdateDurableSha256(payload) != payload_sha ||
      DmlUpdateDurableFrameSha256(encoded.first(320)) != frame_sha) {
    return fail("durable_frame_checksum_invalid");
  }
  frame->payload.assign(payload.begin(), payload.end());
  return true;
}

DmlUpdateDurableFrameLoadV1 DmlUpdateDurableLoadFrames(
    const std::string& path) {
  DmlUpdateDurableFrameLoadV1 result;
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    std::error_code ignored;
    result.missing = !std::filesystem::exists(path, ignored);
    result.ok = result.missing;
    result.detail = result.missing ? std::string{}
                                   : "durable_store_open_failed";
    return result;
  }
  while (true) {
    std::vector<std::uint8_t> header(kDmlUpdateDurableFrameHeaderBytes, 0);
    input.read(reinterpret_cast<char*>(header.data()),
               static_cast<std::streamsize>(header.size()));
    const auto header_read = input.gcount();
    if (header_read == 0 && input.eof()) break;
    if (header_read != static_cast<std::streamsize>(header.size())) {
      result.detail = "durable_store_partial_frame_header";
      return result;
    }
    std::uint64_t total_bytes = 0;
    std::uint64_t payload_bytes = 0;
    if (!DmlUpdateDurableReadU64(header, 16, &total_bytes) ||
        !DmlUpdateDurableReadU64(header, 24, &payload_bytes) ||
        total_bytes != kDmlUpdateDurableFrameHeaderBytes + payload_bytes ||
        payload_bytes > kDmlUpdateDurableMaximumFrameBytes ||
        payload_bytes >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::size_t>::max())) {
      result.detail = "durable_store_frame_extent_invalid";
      return result;
    }
    std::vector<std::uint8_t> encoded;
    try {
      encoded.reserve(static_cast<std::size_t>(total_bytes));
      encoded.insert(encoded.end(), header.begin(), header.end());
      const auto old_size = encoded.size();
      encoded.resize(old_size + static_cast<std::size_t>(payload_bytes));
    } catch (const std::bad_alloc&) {
      result.detail = "durable_store_frame_allocation_failed";
      return result;
    }
    input.read(
        reinterpret_cast<char*>(encoded.data() + header.size()),
        static_cast<std::streamsize>(payload_bytes));
    if (input.gcount() != static_cast<std::streamsize>(payload_bytes)) {
      result.detail = "durable_store_partial_frame_payload";
      return result;
    }
    DmlUpdateDurableFrameV1 decoded;
    if (!DmlUpdateDurableDecodeFrame(encoded, &decoded, &result.detail)) {
      return result;
    }
    result.frames.push_back(std::move(decoded));
  }
  if (input.bad()) {
    result.detail = "durable_store_read_failed";
    return result;
  }
  result.ok = true;
  return result;
}

bool DmlUpdateDurableEnsureDirectory(const std::string& directory) {
  std::error_code error;
  if (!std::filesystem::create_directories(directory, error) && error) {
    return false;
  }
#if defined(_WIN32)
  return true;
#else
  const int fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) return false;
  const bool ok = ::fsync(fd) == 0;
  ::close(fd);
  return ok;
#endif
}

class DmlUpdateDurableFileLock final {
 public:
  explicit DmlUpdateDurableFileLock(const std::string& data_path) {
    const std::string path = data_path + ".lock";
#if defined(_WIN32)
    handle_ = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                          OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) return;
    OVERLAPPED overlapped{};
    if (LockFileEx(handle_, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD,
                   &overlapped) == 0) {
      CloseHandle(handle_);
      handle_ = INVALID_HANDLE_VALUE;
      return;
    }
    ok_ = true;
#else
    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd_ < 0) return;
    if (::flock(fd_, LOCK_EX) != 0) {
      ::close(fd_);
      fd_ = -1;
      return;
    }
    ok_ = true;
#endif
  }

  ~DmlUpdateDurableFileLock() {
#if defined(_WIN32)
    if (handle_ != INVALID_HANDLE_VALUE) {
      OVERLAPPED overlapped{};
      UnlockFileEx(handle_, 0, MAXDWORD, MAXDWORD, &overlapped);
      CloseHandle(handle_);
    }
#else
    if (fd_ >= 0) {
      (void)::flock(fd_, LOCK_UN);
      ::close(fd_);
    }
#endif
  }

  bool ok() const { return ok_; }

 private:
  bool ok_ = false;
#if defined(_WIN32)
  HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
  int fd_ = -1;
#endif
};

std::atomic<std::uint64_t> g_dml_update_durable_prepare_calls{0};
std::atomic<std::uint64_t> g_dml_update_durable_frame_encode_calls{0};
std::atomic<std::uint64_t> g_dml_update_durable_checksum_calls{0};
std::atomic<std::uint64_t> g_dml_update_durable_commit_calls{0};
std::atomic<std::uint64_t> g_dml_update_durable_commit_write_calls{0};
std::atomic<std::uint64_t> g_dml_update_durable_commit_fsync_calls{0};
std::atomic<std::uint64_t> g_dml_update_durable_recovery_calls{0};
std::atomic<std::uint64_t> g_dml_update_durable_observation_encode_calls{0};
std::atomic<MgaDmlUpdateDurableFaultCutpointV1>
    g_dml_update_durable_fault_cutpoint{
        MgaDmlUpdateDurableFaultCutpointV1::none};

bool DmlUpdateDurableFault(MgaDmlUpdateDurableFaultCutpointV1 cutpoint) {
  return g_dml_update_durable_fault_cutpoint.load(
             std::memory_order_acquire) == cutpoint;
}

enum class DmlUpdateDurableRawAppendResultV1 : std::uint8_t {
  ok,
  write_failed,
  fsync_failed,
  after_fsync_ack_lost,
};

DmlUpdateDurableRawAppendResultV1 DmlUpdateDurableAppendEncodedFrame(
    const std::string& path, std::span<const std::uint8_t> encoded,
    bool successor_commit) {
  if (encoded.empty()) return DmlUpdateDurableRawAppendResultV1::write_failed;
#if defined(_WIN32)
  HANDLE handle = CreateFileA(path.c_str(), FILE_APPEND_DATA,
                              FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return DmlUpdateDurableRawAppendResultV1::write_failed;
  }
  std::size_t offset = 0;
  bool write_ok = true;
  while (offset < encoded.size()) {
    DWORD written = 0;
    const DWORD request = static_cast<DWORD>(std::min<std::size_t>(
        encoded.size() - offset, std::numeric_limits<DWORD>::max()));
    if (WriteFile(handle, encoded.data() + offset, request, &written,
                  nullptr) == 0 || written == 0) {
      write_ok = false;
      break;
    }
    offset += written;
  }
  if (successor_commit) {
    g_dml_update_durable_commit_write_calls.fetch_add(
        1, std::memory_order_relaxed);
  }
  if (!write_ok ||
      (successor_commit &&
       DmlUpdateDurableFault(
           MgaDmlUpdateDurableFaultCutpointV1::after_successor_write_before_fsync))) {
    CloseHandle(handle);
    return DmlUpdateDurableRawAppendResultV1::write_failed;
  }
  const bool fsync_ok = FlushFileBuffers(handle) != 0;
  if (successor_commit) {
    g_dml_update_durable_commit_fsync_calls.fetch_add(
        1, std::memory_order_relaxed);
  }
  CloseHandle(handle);
#else
  const int fd = ::open(path.c_str(),
                        O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
  if (fd < 0) return DmlUpdateDurableRawAppendResultV1::write_failed;
  std::size_t offset = 0;
  bool write_ok = true;
  while (offset < encoded.size()) {
    const ssize_t written =
        ::write(fd, encoded.data() + offset, encoded.size() - offset);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) {
      write_ok = false;
      break;
    }
    offset += static_cast<std::size_t>(written);
  }
  if (successor_commit) {
    g_dml_update_durable_commit_write_calls.fetch_add(
        1, std::memory_order_relaxed);
  }
  if (!write_ok ||
      (successor_commit &&
       DmlUpdateDurableFault(
           MgaDmlUpdateDurableFaultCutpointV1::after_successor_write_before_fsync))) {
    ::close(fd);
    return DmlUpdateDurableRawAppendResultV1::write_failed;
  }
  const bool fsync_ok = ::fsync(fd) == 0;
  if (successor_commit) {
    g_dml_update_durable_commit_fsync_calls.fetch_add(
        1, std::memory_order_relaxed);
  }
  ::close(fd);
#endif
  if (!fsync_ok) return DmlUpdateDurableRawAppendResultV1::fsync_failed;
  if (successor_commit &&
      DmlUpdateDurableFault(
          MgaDmlUpdateDurableFaultCutpointV1::after_successor_fsync_before_ack)) {
    return DmlUpdateDurableRawAppendResultV1::after_fsync_ack_lost;
  }
  return DmlUpdateDurableRawAppendResultV1::ok;
}

bool DmlUpdateDurableAppendFrame(const std::string& path,
                                 const DmlUpdateDurableFrameV1& frame) {
  std::vector<std::uint8_t> encoded;
  if (!DmlUpdateDurableEncodeFrame(frame, &encoded)) return false;
  return DmlUpdateDurableAppendEncodedFrame(path, encoded, false) ==
         DmlUpdateDurableRawAppendResultV1::ok;
}

// Replace the complete descriptor extent only after its new contents are
// durable. PublishBound uses this path because the authority snapshot and the
// root DUJR are one admission decision: recovery may observe the old
// reservation or the complete bound operation, never a snapshot without its
// root journal extent.
bool DmlUpdateDurableReplaceFileAtomically(
    const std::string& path, std::span<const std::uint8_t> bytes) {
  if (bytes.empty()) return false;
  const std::string temporary = path + ".publish.tmp";
  std::error_code ignored;
  std::filesystem::remove(temporary, ignored);
#if defined(_WIN32)
  HANDLE handle = CreateFileA(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  std::size_t offset = 0;
  bool write_ok = true;
  while (offset < bytes.size()) {
    DWORD written = 0;
    const DWORD request = static_cast<DWORD>(std::min<std::size_t>(
        bytes.size() - offset, std::numeric_limits<DWORD>::max()));
    if (WriteFile(handle, bytes.data() + offset, request, &written, nullptr) ==
            0 ||
        written == 0) {
      write_ok = false;
      break;
    }
    offset += written;
  }
  const bool durable = write_ok && FlushFileBuffers(handle) != 0;
  CloseHandle(handle);
  if (!durable ||
      MoveFileExA(temporary.c_str(), path.c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    std::filesystem::remove(temporary, ignored);
    return false;
  }
  return true;
#else
  const int fd = ::open(temporary.c_str(),
                        O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) return false;
  std::size_t offset = 0;
  bool write_ok = true;
  while (offset < bytes.size()) {
    const ssize_t written =
        ::write(fd, bytes.data() + offset, bytes.size() - offset);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) {
      write_ok = false;
      break;
    }
    offset += static_cast<std::size_t>(written);
  }
  const bool durable = write_ok && ::fsync(fd) == 0;
  ::close(fd);
  if (!durable || ::rename(temporary.c_str(), path.c_str()) != 0) {
    std::filesystem::remove(temporary, ignored);
    return false;
  }
  const std::filesystem::path parent =
      std::filesystem::path(path).parent_path();
  const int directory_fd =
      ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (directory_fd < 0) return false;
  const bool directory_durable = ::fsync(directory_fd) == 0;
  ::close(directory_fd);
  return directory_durable;
#endif
}

bool DmlUpdateDurableBytesEqualUuid(
    std::span<const std::uint8_t> bytes, std::size_t offset,
    std::string_view uuid) {
  std::array<std::uint8_t, 16> expected{};
  return DmlUpdateDurableUuidBytes(uuid, &expected) &&
         offset <= bytes.size() && bytes.size() - offset >= expected.size() &&
         std::equal(expected.begin(), expected.end(), bytes.begin() + offset);
}

bool DmlUpdateDurableBytesEqual(
    std::span<const std::uint8_t> left, std::size_t left_offset,
    std::span<const std::uint8_t> right, std::size_t right_offset,
    std::size_t count) {
  return left_offset <= left.size() && left.size() - left_offset >= count &&
         right_offset <= right.size() && right.size() - right_offset >= count &&
         std::equal(left.begin() + left_offset,
                    left.begin() + left_offset + count,
                    right.begin() + right_offset);
}

bool DmlUpdateDurableCarrierHeader(
    std::span<const std::uint8_t> bytes, std::string_view magic,
    std::uint16_t header_bytes, bool exact_total) {
  std::uint16_t version = 0;
  std::uint16_t parsed_header = 0;
  std::uint32_t total = 0;
  std::uint32_t flags = 0;
  return magic.size() == 4 && bytes.size() >= 16 &&
         std::equal(magic.begin(), magic.end(), bytes.begin()) &&
         DmlUpdateDurableReadU16(bytes, 4, &version) && version == 1 &&
         DmlUpdateDurableReadU16(bytes, 6, &parsed_header) &&
         parsed_header == header_bytes &&
         DmlUpdateDurableReadU32(bytes, 8, &total) &&
         total == bytes.size() &&
         (!exact_total || total == header_bytes) &&
         DmlUpdateDurableReadU32(bytes, 12, &flags) && flags == 0;
}

bool DmlUpdateDurableVectorCarrier(
    std::span<const std::uint8_t> bytes, std::string_view magic,
    std::uint32_t minimum_count, std::uint32_t maximum_count,
    std::uint32_t fixed_record_bytes, std::uint32_t* record_count = nullptr) {
  std::uint32_t count = 0;
  std::uint32_t exact_records = 0;
  if (!DmlUpdateDurableCarrierHeader(bytes, magic, 104, false) ||
      !DmlUpdateDurableReadU32(bytes, 64, &count) ||
      !DmlUpdateDurableReadU32(bytes, 68, &exact_records) ||
      count < minimum_count || count > maximum_count ||
      exact_records != bytes.size() - 104) {
    return false;
  }
  if (fixed_record_bytes != 0 &&
      (count > std::numeric_limits<std::uint32_t>::max() /
                   fixed_record_bytes ||
       exact_records != count * fixed_record_bytes)) {
    return false;
  }
  if (record_count != nullptr) *record_count = count;
  return true;
}

bool DmlUpdateDurableSnapshotShallowValid(
    const MgaDmlUpdateDurableOperationIdentityV1& identity,
    const MgaDmlUpdateDurableAuthoritySnapshotV1& snapshot,
    std::uint64_t* structural_occurrence_id,
    std::string* detail) {
  const auto fail = [&](std::string reason) {
    if (detail != nullptr) *detail = std::move(reason);
    return false;
  };
  const auto descriptor = std::span<const std::uint8_t>(
      snapshot.descriptor_dudc);
  if (!DmlUpdateDurableCarrierHeader(descriptor, "DUDC", 712, true) ||
      !DmlUpdateDurableBytesEqualUuid(descriptor, 16,
                                      identity.descriptor_uuid) ||
      !DmlUpdateDurableBytesEqualUuid(
          descriptor, 40, identity.authenticated_statement_receipt_uuid) ||
      !DmlUpdateDurableBytesEqualUuid(descriptor, 64,
                                      identity.operation_uuid) ||
      !DmlUpdateDurableBytesEqualUuid(
          descriptor, 88, identity.owning_transaction_uuid) ||
      !DmlUpdateDurableBytesEqualUuid(descriptor, 624,
                                      identity.recovery_token_uuid)) {
    return fail("durable_snapshot_descriptor_identity_invalid");
  }
  std::uint64_t descriptor_generation = 0;
  std::uint64_t operation_generation = 0;
  std::uint64_t local_transaction_id = 0;
  std::uint64_t recovery_generation = 0;
  std::uint64_t structural_occurrence = 0;
  if (!DmlUpdateDurableReadU64(descriptor, 32, &descriptor_generation) ||
      !DmlUpdateDurableReadU64(descriptor, 56, &structural_occurrence) ||
      !DmlUpdateDurableReadU64(descriptor, 80, &operation_generation) ||
      !DmlUpdateDurableReadU64(descriptor, 104, &local_transaction_id) ||
      !DmlUpdateDurableReadU64(descriptor, 640, &recovery_generation) ||
      descriptor_generation != identity.descriptor_generation ||
      operation_generation != identity.operation_generation ||
      structural_occurrence == 0 ||
      local_transaction_id != identity.owning_local_transaction_id ||
      recovery_generation != identity.recovery_generation) {
    return fail("durable_snapshot_descriptor_generation_invalid");
  }
  if (structural_occurrence_id != nullptr) {
    *structural_occurrence_id = structural_occurrence;
  }

  struct VectorRule {
    const std::vector<std::uint8_t>* bytes;
    std::string_view magic;
    std::size_t descriptor_reference_offset;
    std::uint32_t minimum_count;
    std::uint32_t maximum_count;
    std::uint32_t fixed_record_bytes;
  };
  const std::array<VectorRule, 5> vectors{{
      {&snapshot.assignment_vector_duav, "DUAV", 248, 1, 1024, 0},
      {&snapshot.predicate_vector_duev, "DUEV", 312, 1, 3, 0},
      {&snapshot.row_policy_vector_dupv, "DUPV", 384, 0, 2, 176},
      {&snapshot.constraint_vector_ducv, "DUCV", 448, 0, 1048576, 160},
      {&snapshot.trigger_vector_dutv, "DUTV", 512, 0, 1048576, 192},
  }};
  for (const auto& vector : vectors) {
    const auto bytes = std::span<const std::uint8_t>(*vector.bytes);
    std::uint32_t count = 0;
    std::uint64_t vector_generation = 0;
    std::uint64_t referenced_generation = 0;
    if (!DmlUpdateDurableVectorCarrier(
            bytes, vector.magic, vector.minimum_count, vector.maximum_count,
            vector.fixed_record_bytes, &count) ||
        (vector.magic == "DUEV" && count != 1 && count != 3) ||
        !DmlUpdateDurableBytesEqual(bytes, 16, descriptor,
                                    vector.descriptor_reference_offset, 16) ||
        !DmlUpdateDurableReadU64(bytes, 32, &vector_generation) ||
        !DmlUpdateDurableReadU64(
            descriptor, vector.descriptor_reference_offset + 16,
            &referenced_generation) ||
        vector_generation != referenced_generation ||
        !DmlUpdateDurableBytesEqualUuid(bytes, 40,
                                        identity.descriptor_uuid) ||
        !DmlUpdateDurableReadU64(bytes, 56, &vector_generation) ||
        vector_generation != identity.descriptor_generation) {
      return fail("durable_snapshot_vector_identity_invalid");
    }
  }

  const auto order =
      std::span<const std::uint8_t>(snapshot.target_order_duor);
  const auto budget =
      std::span<const std::uint8_t>(snapshot.resource_budget_dubr);
  const auto recovery =
      std::span<const std::uint8_t>(snapshot.recovery_token_durc);
  const auto source_policies =
      std::span<const std::uint8_t>(snapshot.source_policy_vector_dusv);
  const auto security_proof =
      std::span<const std::uint8_t>(snapshot.security_snapshot_proof_dusp);
  if (!DmlUpdateDurableCarrierHeader(order, "DUOR", 160, true) ||
      !DmlUpdateDurableBytesEqual(order, 16, descriptor, 576, 24) ||
      !DmlUpdateDurableBytesEqualUuid(
          order, 40, identity.authenticated_statement_receipt_uuid) ||
      !DmlUpdateDurableCarrierHeader(budget, "DUBR", 208, true) ||
      !DmlUpdateDurableBytesEqual(budget, 16, descriptor, 600, 24) ||
      !DmlUpdateDurableBytesEqualUuid(
          budget, 40, identity.authenticated_statement_receipt_uuid) ||
      !DmlUpdateDurableBytesEqualUuid(
          budget, 56, identity.owning_transaction_uuid) ||
      !DmlUpdateDurableCarrierHeader(recovery, "DURC", 208, true) ||
      !DmlUpdateDurableBytesEqual(recovery, 16, descriptor, 624, 24) ||
      !DmlUpdateDurableBytesEqualUuid(
          recovery, 16, identity.recovery_token_uuid) ||
      !DmlUpdateDurableBytesEqualUuid(
          recovery, 40, identity.authenticated_statement_receipt_uuid) ||
      !DmlUpdateDurableBytesEqualUuid(
          recovery, 56, identity.owning_transaction_uuid) ||
      !DmlUpdateDurableBytesEqualUuid(recovery, 72,
                                      identity.operation_uuid) ||
      !DmlUpdateDurableBytesEqualUuid(recovery, 88,
                                      identity.descriptor_uuid) ||
      !DmlUpdateDurableBytesEqualUuid(
          recovery, 136, identity.validated_durable_handle_uuid)) {
    return fail("durable_snapshot_scalar_authority_invalid");
  }
  std::uint64_t scalar_generation = 0;
  if (!DmlUpdateDurableReadU64(recovery, 32, &scalar_generation) ||
      scalar_generation != identity.recovery_generation ||
      !DmlUpdateDurableReadU64(recovery, 104, &scalar_generation) ||
      scalar_generation != identity.descriptor_generation ||
      !DmlUpdateDurableReadU64(recovery, 152, &scalar_generation) ||
      scalar_generation != identity.validated_durable_handle_generation) {
    return fail("durable_snapshot_recovery_generation_invalid");
  }

  std::uint32_t source_policy_count = 0;
  std::uint32_t effective_policy_count = 0;
  std::uint32_t proof_effective_count = 0;
  std::uint32_t proof_source_count = 0;
  if (!DmlUpdateDurableVectorCarrier(source_policies, "DUSV", 0, 1048576,
                                     256, &source_policy_count) ||
      !DmlUpdateDurableVectorCarrier(
          snapshot.row_policy_vector_dupv, "DUPV", 0, 2, 176,
          &effective_policy_count) ||
      !DmlUpdateDurableCarrierHeader(security_proof, "DUSP", 576, true) ||
      !DmlUpdateDurableReadU32(security_proof, 352,
                               &proof_effective_count) ||
      !DmlUpdateDurableReadU32(security_proof, 356,
                               &proof_source_count) ||
      proof_effective_count != effective_policy_count ||
      proof_source_count != source_policy_count ||
      ((effective_policy_count == 0) != (source_policy_count == 0)) ||
      security_proof[360] != 1 ||
      !DmlUpdateDurableZero(security_proof.subspan(361, 7)) ||
      !DmlUpdateDurableZero(security_proof.subspan(560, 16)) ||
      !DmlUpdateDurableBytesEqualUuid(
          security_proof, 80, identity.database_uuid) ||
      !DmlUpdateDurableBytesEqualUuid(
          security_proof, 96,
          identity.authenticated_statement_receipt_uuid) ||
      !DmlUpdateDurableBytesEqualUuid(
          security_proof, 112, identity.owning_transaction_uuid) ||
      !DmlUpdateDurableBytesEqualUuid(
          security_proof, 136, identity.operation_uuid) ||
      !DmlUpdateDurableBytesEqualUuid(
          security_proof, 160, identity.recovery_token_uuid) ||
      !DmlUpdateDurableBytesEqualUuid(
          security_proof, 280, identity.descriptor_uuid) ||
      !DmlUpdateDurableBytesEqual(
          security_proof, 304, descriptor, 384, 24) ||
      !DmlUpdateDurableBytesEqual(
          security_proof, 328, source_policies, 16, 24) ||
      !DmlUpdateDurableBytesEqual(
          security_proof, 368, descriptor, 680, 32) ||
      !DmlUpdateDurableBytesEqual(
          security_proof, 400, descriptor, 416, 32) ||
      !DmlUpdateDurableBytesEqual(
          security_proof, 496, source_policies, 72, 32) ||
      DmlUpdateDurableSha256(descriptor) !=
          [&] {
            MgaDmlUpdateDurableSha256V1 value{};
            std::copy_n(security_proof.begin() + 432, 32, value.begin());
            return value;
          }() ||
      DmlUpdateDurableSha256(snapshot.row_policy_vector_dupv) !=
          [&] {
            MgaDmlUpdateDurableSha256V1 value{};
            std::copy_n(security_proof.begin() + 464, 32, value.begin());
            return value;
          }()) {
    return fail("durable_snapshot_security_authority_invalid");
  }
  std::uint64_t scalar = 0;
  const std::array<std::pair<std::size_t, std::uint64_t>, 6>
      security_generations{{
          {128, identity.owning_local_transaction_id},
          {152, identity.operation_generation},
          {176, identity.recovery_generation},
          {296, identity.descriptor_generation},
          {344, [&] {
             std::uint64_t value = 0;
             (void)DmlUpdateDurableReadU64(source_policies, 32, &value);
             return value;
           }()},
          {32, [&] {
             std::uint64_t value = 0;
             (void)DmlUpdateDurableReadU64(security_proof, 32, &value);
             return value;
           }()},
      }};
  for (const auto& [offset, expected] : security_generations) {
    if (!DmlUpdateDurableReadU64(security_proof, offset, &scalar) ||
        scalar == 0 || scalar != expected) {
      return fail("durable_snapshot_security_generation_invalid");
    }
  }

  // The MGA store does not merely preserve carrier-shaped bytes.  It accepts
  // a bound snapshot only after the canonical carrier codec has validated the
  // complete set and the two recovery-only security carriers byte-for-byte.
  // This remains a storage admission check; the UPDATE consumer performs the
  // live datatype/operator/security/catalog revalidation before publication.
  scratchbird::wire::TypedUpdateCarrierSet carriers;
  scratchbird::wire::TypedUpdateSecurityPolicySourceVector typed_sources;
  scratchbird::wire::TypedUpdateSecuritySnapshotProof typed_proof;
  scratchbird::wire::TypedUpdateDatatypeAuthorityVector typed_datatypes;
  scratchbird::wire::TypedUpdateBuiltinOperatorAuthorityVector typed_operators;
  scratchbird::wire::TypedUpdateCarrierError carrier_error;
  if (!scratchbird::wire::DecodeAndValidateTypedUpdateDescriptor(
          snapshot.descriptor_dudc, &carriers.descriptor, &carrier_error) ||
      !scratchbird::wire::DecodeAndValidateTypedUpdateAssignmentVector(
          snapshot.assignment_vector_duav, &carriers.assignments,
          &carrier_error) ||
      !scratchbird::wire::DecodeAndValidateTypedUpdatePredicateVector(
          snapshot.predicate_vector_duev, &carriers.predicate,
          &carrier_error) ||
      !scratchbird::wire::DecodeAndValidateTypedUpdateRowPolicyVector(
          snapshot.row_policy_vector_dupv, &carriers.row_policies,
          &carrier_error) ||
      !scratchbird::wire::DecodeAndValidateTypedUpdateConstraintVector(
          snapshot.constraint_vector_ducv, &carriers.constraints,
          &carrier_error) ||
      !scratchbird::wire::DecodeAndValidateTypedUpdateTriggerVector(
          snapshot.trigger_vector_dutv, &carriers.triggers,
          &carrier_error) ||
      !scratchbird::wire::DecodeAndValidateTypedUpdateTargetOrder(
          snapshot.target_order_duor, &carriers.target_order,
          &carrier_error) ||
      !scratchbird::wire::DecodeAndValidateTypedUpdateResourceBudget(
          snapshot.resource_budget_dubr, &carriers.resource_budget,
          &carrier_error) ||
      !scratchbird::wire::DecodeAndValidateTypedUpdateRecoveryToken(
          snapshot.recovery_token_durc, &carriers.recovery_token,
          &carrier_error) ||
      !scratchbird::wire::ValidateTypedUpdateCarrierSet(carriers,
                                                         &carrier_error) ||
      !scratchbird::wire::DecodeAndValidateTypedUpdateSecurityPolicySourceVector(
          snapshot.source_policy_vector_dusv, &typed_sources,
          &carrier_error) ||
      !scratchbird::wire::DecodeAndValidateTypedUpdateSecuritySnapshotProof(
          snapshot.security_snapshot_proof_dusp, &typed_proof,
          &carrier_error) ||
      !scratchbird::wire::DecodeAndValidateTypedUpdateDatatypeAuthorityVector(
          snapshot.datatype_authority_vector_dudv, &typed_datatypes,
          &carrier_error) ||
      !scratchbird::wire::DecodeAndValidateTypedUpdateBuiltinOperatorAuthorityVector(
          snapshot.builtin_operator_authority_vector_duov, &typed_operators,
          &carrier_error) ||
      !scratchbird::wire::ValidateTypedUpdateDatatypeOperatorAuthority(
          carriers.descriptor, carriers.assignments, carriers.predicate,
          typed_datatypes, typed_operators, &carrier_error)) {
    return fail("durable_snapshot_canonical_carrier_invalid:" +
                carrier_error.field + ":" + carrier_error.detail);
  }
  if (typed_sources.identity.owner_descriptor_uuid !=
          carriers.descriptor.descriptor_uuid ||
      typed_sources.identity.owner_descriptor_generation !=
          carriers.descriptor.descriptor_generation ||
      typed_proof.descriptor_uuid != carriers.descriptor.descriptor_uuid ||
      typed_proof.descriptor_generation !=
          carriers.descriptor.descriptor_generation ||
      typed_proof.source_policy_vector_uuid !=
          typed_sources.identity.vector_uuid ||
      typed_proof.source_policy_vector_generation !=
          typed_sources.identity.vector_generation ||
      typed_proof.source_policy_count != typed_sources.records.size() ||
      carriers.recovery_token.durable_registry_uuid !=
          [&] {
            scratchbird::wire::TypedUpdateUuid value{};
            (void)DmlUpdateDurableTypedUuid(
                identity.validated_durable_handle_uuid, &value);
            return value;
          }() ||
      carriers.recovery_token.durable_registry_generation !=
          identity.validated_durable_handle_generation) {
    return fail("durable_snapshot_typed_recovery_authority_mismatch");
  }
  return true;
}

constexpr std::array<std::uint8_t, 8> kDmlUpdateDurableSnapshotMagic{{
    'S', 'B', 'M', 'D', 'U', 'A', 'S', '1'}};
constexpr std::uint16_t kDmlUpdateDurableSnapshotVersion = 1;
constexpr std::uint16_t kDmlUpdateDurableSnapshotHeaderBytes = 128;
constexpr std::size_t kDmlUpdateDurableSnapshotCarrierCount = 13;
constexpr std::uint64_t kDmlUpdateDurableMaximumSnapshotBytes =
    64ULL * 1024ULL * 1024ULL;

std::array<const std::vector<std::uint8_t>*,
           kDmlUpdateDurableSnapshotCarrierCount>
DmlUpdateDurableSnapshotCarriers(
    const MgaDmlUpdateDurableAuthoritySnapshotV1& snapshot) {
  return {{&snapshot.assignment_vector_duav,
           &snapshot.predicate_vector_duev,
           &snapshot.row_policy_vector_dupv,
           &snapshot.constraint_vector_ducv,
           &snapshot.trigger_vector_dutv,
           &snapshot.target_order_duor,
           &snapshot.resource_budget_dubr,
           &snapshot.recovery_token_durc,
           &snapshot.source_policy_vector_dusv,
           &snapshot.security_snapshot_proof_dusp,
           &snapshot.datatype_authority_vector_dudv,
           &snapshot.builtin_operator_authority_vector_duov,
           &snapshot.descriptor_dudc}};
}

std::array<std::vector<std::uint8_t>*,
           kDmlUpdateDurableSnapshotCarrierCount>
DmlUpdateDurableMutableSnapshotCarriers(
    MgaDmlUpdateDurableAuthoritySnapshotV1* snapshot) {
  return {{&snapshot->assignment_vector_duav,
           &snapshot->predicate_vector_duev,
           &snapshot->row_policy_vector_dupv,
           &snapshot->constraint_vector_ducv,
           &snapshot->trigger_vector_dutv,
           &snapshot->target_order_duor,
           &snapshot->resource_budget_dubr,
           &snapshot->recovery_token_durc,
           &snapshot->source_policy_vector_dusv,
           &snapshot->security_snapshot_proof_dusp,
           &snapshot->datatype_authority_vector_dudv,
           &snapshot->builtin_operator_authority_vector_duov,
           &snapshot->descriptor_dudc}};
}

bool DmlUpdateDurableAddSize(std::uint64_t* total, std::uint64_t value) {
  if (total == nullptr || value > kDmlUpdateDurableMaximumSnapshotBytes ||
      *total > kDmlUpdateDurableMaximumSnapshotBytes - value) {
    return false;
  }
  *total += value;
  return true;
}

bool DmlUpdateDurableEncodeSnapshot(
    const MgaDmlUpdateDurableAuthoritySnapshotV1& snapshot,
    std::vector<std::uint8_t>* payload) {
  if (payload == nullptr) return false;
  const auto carriers = DmlUpdateDurableSnapshotCarriers(snapshot);
  std::uint64_t total = kDmlUpdateDurableSnapshotHeaderBytes;
  for (const auto* carrier : carriers) {
    if (carrier == nullptr || carrier->empty() ||
        !DmlUpdateDurableAddSize(&total, carrier->size())) {
      return false;
    }
  }
  if (total > std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }
  try {
    payload->assign(static_cast<std::size_t>(total), 0);
  } catch (const std::bad_alloc&) {
    return false;
  }
  std::copy(kDmlUpdateDurableSnapshotMagic.begin(),
            kDmlUpdateDurableSnapshotMagic.end(), payload->begin());
  DmlUpdateDurablePutU16(payload, 8, kDmlUpdateDurableSnapshotVersion);
  DmlUpdateDurablePutU16(payload, 10,
                         kDmlUpdateDurableSnapshotHeaderBytes);
  DmlUpdateDurablePutU32(payload, 12, static_cast<std::uint32_t>(total));
  for (std::size_t index = 0; index < carriers.size(); ++index) {
    DmlUpdateDurablePutU64(payload, 16 + index * 8,
                           carriers[index]->size());
  }
  std::size_t cursor = kDmlUpdateDurableSnapshotHeaderBytes;
  for (const auto* carrier : carriers) {
    std::copy(carrier->begin(), carrier->end(), payload->begin() + cursor);
    cursor += carrier->size();
  }
  return cursor == payload->size();
}

bool DmlUpdateDurableDecodeSnapshot(
    std::span<const std::uint8_t> payload,
    MgaDmlUpdateDurableAuthoritySnapshotV1* snapshot,
    std::string* detail) {
  const auto fail = [&](std::string reason) {
    if (detail != nullptr) *detail = std::move(reason);
    return false;
  };
  std::uint16_t version = 0;
  std::uint16_t header = 0;
  std::uint32_t total = 0;
  if (snapshot == nullptr || payload.size() < kDmlUpdateDurableSnapshotHeaderBytes ||
      payload.size() > kDmlUpdateDurableMaximumSnapshotBytes ||
      !std::equal(kDmlUpdateDurableSnapshotMagic.begin(),
                  kDmlUpdateDurableSnapshotMagic.end(), payload.begin()) ||
      !DmlUpdateDurableReadU16(payload, 8, &version) ||
      !DmlUpdateDurableReadU16(payload, 10, &header) ||
      !DmlUpdateDurableReadU32(payload, 12, &total) ||
      version != kDmlUpdateDurableSnapshotVersion ||
      header != kDmlUpdateDurableSnapshotHeaderBytes ||
      total != payload.size() ||
      !DmlUpdateDurableZero(payload.subspan(120, 8))) {
    return fail("durable_snapshot_payload_header_invalid");
  }
  MgaDmlUpdateDurableAuthoritySnapshotV1 decoded;
  const auto carriers = DmlUpdateDurableMutableSnapshotCarriers(&decoded);
  std::size_t cursor = kDmlUpdateDurableSnapshotHeaderBytes;
  for (std::size_t index = 0; index < carriers.size(); ++index) {
    std::uint64_t bytes = 0;
    if (!DmlUpdateDurableReadU64(payload, 16 + index * 8, &bytes) ||
        bytes == 0 || bytes > payload.size() - cursor) {
      return fail("durable_snapshot_payload_carrier_extent_invalid");
    }
    carriers[index]->assign(payload.begin() + cursor,
                            payload.begin() + cursor + bytes);
    cursor += static_cast<std::size_t>(bytes);
  }
  if (cursor != payload.size()) {
    return fail("durable_snapshot_payload_authority_extent_invalid");
  }
  *snapshot = std::move(decoded);
  return true;
}

struct DmlUpdateDurableParsedJournalV1 {
  std::uint64_t sequence = 0;
  MgaDmlUpdateDurableJournalStateV1 state =
      MgaDmlUpdateDurableJournalStateV1::bound;
  MgaDmlUpdateDurableSha256V1 prior{};
  MgaDmlUpdateDurableSha256V1 evidence{};
};

bool DmlUpdateDurableJournalShallowValid(
    const MgaDmlUpdateDurableOperationIdentityV1& identity,
    const MgaDmlUpdateDurableAuthoritySnapshotV1& snapshot,
    std::span<const std::uint8_t> bytes,
    DmlUpdateDurableParsedJournalV1* parsed, std::string* detail) {
  const auto fail = [&](std::string reason) {
    if (detail != nullptr) *detail = std::move(reason);
    return false;
  };
  if (parsed == nullptr ||
      (bytes.size() != 968 && bytes.size() != 1224) ||
      !DmlUpdateDurableCarrierHeader(bytes, "DUJR", 256, false)) {
    return fail("durable_journal_extent_invalid");
  }
  const std::uint8_t state = bytes[16];
  if (state < static_cast<std::uint8_t>(
                  MgaDmlUpdateDurableJournalStateV1::bound) ||
      state > static_cast<std::uint8_t>(
                  MgaDmlUpdateDurableJournalStateV1::aborted) ||
      !DmlUpdateDurableZero(bytes.subspan(17, 7)) ||
      !DmlUpdateDurableZero(bytes.subspan(188, 4)) ||
      !DmlUpdateDurableBytesEqualUuid(bytes, 32, identity.database_uuid) ||
      !DmlUpdateDurableBytesEqualUuid(bytes, 48,
                                      identity.descriptor_uuid) ||
      !DmlUpdateDurableBytesEqualUuid(
          bytes, 72, identity.authenticated_statement_receipt_uuid) ||
      !DmlUpdateDurableBytesEqualUuid(
          bytes, 88, identity.owning_transaction_uuid) ||
      !DmlUpdateDurableBytesEqualUuid(bytes, 112,
                                      identity.operation_uuid) ||
      !DmlUpdateDurableBytesEqualUuid(bytes, 128,
                                      identity.recovery_token_uuid)) {
    return fail("durable_journal_identity_invalid");
  }
  std::uint64_t descriptor_generation = 0;
  std::uint64_t local_transaction_id = 0;
  std::uint64_t recovery_generation = 0;
  std::uint32_t descriptor_bytes = 0;
  std::uint32_t result_bytes = 0;
  std::uint32_t payload_bytes = 0;
  if (!DmlUpdateDurableReadU64(bytes, 24, &parsed->sequence) ||
      !DmlUpdateDurableReadU64(bytes, 64, &descriptor_generation) ||
      !DmlUpdateDurableReadU64(bytes, 104, &local_transaction_id) ||
      !DmlUpdateDurableReadU64(bytes, 144, &recovery_generation) ||
      !DmlUpdateDurableReadU32(bytes, 176, &descriptor_bytes) ||
      !DmlUpdateDurableReadU32(bytes, 180, &result_bytes) ||
      !DmlUpdateDurableReadU32(bytes, 184, &payload_bytes) ||
      descriptor_generation != identity.descriptor_generation ||
      local_transaction_id != identity.owning_local_transaction_id ||
      recovery_generation != identity.recovery_generation ||
      descriptor_bytes != 712 || payload_bytes != 712 + result_bytes ||
      bytes.size() != 256 + payload_bytes ||
      !DmlUpdateDurableBytesEqual(
          bytes, 256, snapshot.descriptor_dudc, 0, 712)) {
    return fail("durable_journal_payload_or_generation_invalid");
  }
  const auto typed_state =
      static_cast<MgaDmlUpdateDurableJournalStateV1>(state);
  const bool requires_result =
      typed_state == MgaDmlUpdateDurableJournalStateV1::prepared ||
      typed_state == MgaDmlUpdateDurableJournalStateV1::published;
  if ((requires_result && result_bytes != 256) ||
      (!requires_result && result_bytes != 0)) {
    return fail("durable_journal_state_extent_invalid");
  }
  parsed->state = typed_state;
  std::copy_n(bytes.begin() + 192, 32, parsed->prior.begin());
  std::copy_n(bytes.begin() + 224, 32, parsed->evidence.begin());
  if (DmlUpdateDurableZero(parsed->evidence)) {
    return fail("durable_journal_evidence_missing");
  }
  return true;
}

bool DmlUpdateDurableJournalExtentMatchesBytes(
    const MgaDmlUpdateDurableOperationIdentityV1& identity,
    const MgaDmlUpdateDurableAuthoritySnapshotV1& snapshot,
    const MgaDmlUpdateDurableJournalExtentV1& extent,
    std::string* detail) {
  DmlUpdateDurableParsedJournalV1 parsed;
  if (!DmlUpdateDurableJournalShallowValid(
          identity, snapshot, extent.exact_dujr_bytes, &parsed, detail)) {
    return false;
  }
  if (parsed.sequence != extent.journal_sequence ||
      parsed.state != extent.lifecycle_state ||
      parsed.prior != extent.prior_record_sha256 ||
      parsed.evidence != extent.record_evidence_sha256) {
    if (detail != nullptr) {
      *detail = "durable_journal_supplied_metadata_mismatch";
    }
    return false;
  }
  return true;
}

bool DmlUpdateDurableLegalTransition(
    MgaDmlUpdateDurableJournalStateV1 prior,
    MgaDmlUpdateDurableJournalStateV1 next) {
  switch (prior) {
    case MgaDmlUpdateDurableJournalStateV1::bound:
      return next == MgaDmlUpdateDurableJournalStateV1::intent ||
             next == MgaDmlUpdateDurableJournalStateV1::aborted;
    case MgaDmlUpdateDurableJournalStateV1::intent:
      return next == MgaDmlUpdateDurableJournalStateV1::prepared ||
             next == MgaDmlUpdateDurableJournalStateV1::aborted;
    case MgaDmlUpdateDurableJournalStateV1::prepared:
      return next == MgaDmlUpdateDurableJournalStateV1::published ||
             next == MgaDmlUpdateDurableJournalStateV1::aborted;
    case MgaDmlUpdateDurableJournalStateV1::published:
    case MgaDmlUpdateDurableJournalStateV1::aborted:
      return false;
  }
  return false;
}

EngineApiDiagnostic DmlUpdateDurableDiagnostic(
    std::string code, std::string key, std::string detail = {}) {
  return MakeEngineApiDiagnostic(std::move(code), std::move(key),
                                 std::move(detail), true);
}

MgaDmlUpdateDurableOperationMutationResultV1 DmlUpdateDurableMutation(
    MgaDmlUpdateDurableOperationOutcomeV1 outcome, std::string detail = {}) {
  MgaDmlUpdateDurableOperationMutationResultV1 result;
  result.outcome = outcome;
  if (result.ok()) {
    result.diagnostic = OkDiagnostic();
  } else {
    const bool stale = outcome == MgaDmlUpdateDurableOperationOutcomeV1::stale;
    const bool denied =
        outcome == MgaDmlUpdateDurableOperationOutcomeV1::access_denied;
    result.diagnostic = DmlUpdateDurableDiagnostic(
        denied ? "SECURITY.ACCESS_DENIED"
               : stale ? "MGA.TRANSACTION.STALE" : "DML.UPDATE_FAILED",
        denied ? "sblr.dml_update_rows.durable_operation_denied"
               : stale ? "sblr.dml_update_rows.durable_operation_stale"
                       : "sblr.dml_update_rows.durable_operation_failed",
        std::move(detail));
  }
  return result;
}

std::string DmlUpdateDurablePathForLookup(
    const EngineRequestContext& context,
    const MgaDmlUpdateDurableOperationLookupV1& lookup) {
  std::array<std::uint8_t, 16> ignored{};
  if (context.database_path.empty() ||
      !DmlUpdateDurableUuidBytes(lookup.descriptor_uuid, &ignored) ||
      lookup.descriptor_generation == 0 ||
      lookup.structural_occurrence_id == 0) {
    return {};
  }
  return DmlUpdateDurableOperationStorePath(context) + "/" +
         lookup.descriptor_uuid + ".duop";
}

bool DmlUpdateDurableSameReservationRequest(
    const EngineRequestContext& context,
    const MgaDmlUpdateDurableAuthorityReservationRequestV1& request,
    const MgaDmlUpdateDurableOperationIdentityV1& identity) {
  return identity.database_uuid == context.database_uuid.canonical &&
         identity.owning_transaction_uuid ==
             context.transaction_uuid.canonical &&
         identity.owning_local_transaction_id ==
             context.local_transaction_id &&
         identity.authenticated_statement_receipt_uuid ==
             context.statement_receipt_uuid.canonical &&
         identity.operation_uuid == request.operation_uuid &&
         identity.operation_generation == request.operation_generation &&
         identity.descriptor_uuid == request.descriptor_uuid &&
         identity.descriptor_generation == request.descriptor_generation &&
         identity.recovery_token_uuid == request.recovery_token_uuid &&
         identity.recovery_generation == request.recovery_generation;
}

std::string DmlUpdateDurableFreshIdentity(
    const MgaDmlUpdateDurableOperationIdentityV1& identity,
    std::string_view other = {}) {
  for (std::size_t attempt = 0; attempt < 16; ++attempt) {
    const std::string candidate = GenerateCrudEngineUuid("object");
    std::array<std::uint8_t, 16> ignored{};
    if (DmlUpdateDurableUuidBytes(candidate, &ignored) &&
        candidate != identity.database_uuid &&
        candidate != identity.owning_transaction_uuid &&
        candidate != identity.authenticated_statement_receipt_uuid &&
        candidate != identity.operation_uuid &&
        candidate != identity.descriptor_uuid &&
        candidate != identity.recovery_token_uuid && candidate != other) {
      return candidate;
    }
  }
  return {};
}

std::string DmlUpdateDurableQuarantinePath(const std::string& path) {
  return path + ".quarantine";
}

bool DmlUpdateDurableIsQuarantined(const std::string& path) {
  std::error_code error;
  return std::filesystem::exists(DmlUpdateDurableQuarantinePath(path), error) &&
         !error;
}

bool DmlUpdateDurableWriteQuarantine(const std::string& path) {
  const std::array<std::uint8_t, 16> marker{{
      'S', 'B', 'M', 'D', 'U', 'Q', '1', 0, 1, 0, 0, 0, 0, 0, 0, 0}};
  const std::string quarantine = DmlUpdateDurableQuarantinePath(path);
#if defined(_WIN32)
  HANDLE handle = CreateFileA(quarantine.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  const bool ok = WriteFile(handle, marker.data(), marker.size(), &written,
                            nullptr) != 0 &&
                  written == marker.size() && FlushFileBuffers(handle) != 0;
  CloseHandle(handle);
  return ok;
#else
  const int fd = ::open(quarantine.c_str(),
                        O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) return false;
  std::size_t offset = 0;
  while (offset < marker.size()) {
    const ssize_t written =
        ::write(fd, marker.data() + offset, marker.size() - offset);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) {
      ::close(fd);
      return false;
    }
    offset += static_cast<std::size_t>(written);
  }
  const bool ok = ::fsync(fd) == 0;
  ::close(fd);
  return ok;
#endif
}

struct DmlUpdateDurableStoredOperationV1 {
  bool ok = false;
  bool missing = false;
  bool reservation_only = false;
  bool snapshot_present = false;
  bool quarantined = false;
  std::string detail;
  MgaDmlUpdateDurableOperationIdentityV1 identity;
  MgaDmlUpdateDurableAuthoritySnapshotV1 snapshot;
  std::vector<MgaDmlUpdateDurableJournalExtentV1> journal;
  bool staged_successor_present = false;
  MgaDmlUpdateDurableJournalExtentV1 staged_successor;
  std::vector<std::uint8_t> staged_encoded_journal_frame;
  std::vector<std::uint8_t> latest_dumo;
  std::uint64_t structural_occurrence_id = 0;
};

DmlUpdateDurableStoredOperationV1 DmlUpdateDurableLoadOperation(
    const std::string& path, bool quarantine_on_corruption) {
  DmlUpdateDurableStoredOperationV1 result;
  if (DmlUpdateDurableIsQuarantined(path)) {
    result.quarantined = true;
    result.detail = "durable_operation_quarantined";
    return result;
  }
  const auto loaded = DmlUpdateDurableLoadFrames(path);
  if (!loaded.ok) {
    result.missing = loaded.missing;
    result.detail = loaded.detail;
    if (!loaded.missing && quarantine_on_corruption) {
      result.quarantined = DmlUpdateDurableWriteQuarantine(path);
    }
    return result;
  }
  if (loaded.missing || loaded.frames.empty()) {
    result.missing = true;
    return result;
  }
  auto corrupt = [&](std::string detail) {
    result.detail = std::move(detail);
    if (quarantine_on_corruption) {
      result.quarantined = DmlUpdateDurableWriteQuarantine(path);
    }
    return result;
  };
  const auto& reservation = loaded.frames.front();
  if (reservation.kind !=
          DmlUpdateDurableFrameKindV1::authority_reservation ||
      reservation.sequence != 0 || reservation.state != 0 ||
      reservation.flags != 0 || !reservation.payload.empty() ||
      !DmlUpdateDurableZero(reservation.prior_record_sha256) ||
      !DmlUpdateDurableZero(reservation.record_evidence_sha256)) {
    return corrupt("durable_reservation_frame_invalid");
  }
  result.identity = reservation.identity;
  if (loaded.frames.size() == 1) {
    result.ok = true;
    result.reservation_only = true;
    return result;
  }
  const auto& snapshot = loaded.frames[1];
  if (snapshot.kind != DmlUpdateDurableFrameKindV1::authority_snapshot ||
      snapshot.identity != result.identity || snapshot.sequence != 0 ||
      snapshot.state != 0 || snapshot.flags != 0 ||
      !DmlUpdateDurableZero(snapshot.prior_record_sha256) ||
      !DmlUpdateDurableZero(snapshot.record_evidence_sha256) ||
      !DmlUpdateDurableDecodeSnapshot(snapshot.payload, &result.snapshot,
                                      &result.detail) ||
      !DmlUpdateDurableSnapshotShallowValid(
          result.identity, result.snapshot,
          &result.structural_occurrence_id, &result.detail)) {
    return corrupt(result.detail.empty() ? "durable_snapshot_frame_invalid"
                                         : result.detail);
  }
  result.snapshot_present = true;
  std::size_t cursor = 2;
  for (; cursor < loaded.frames.size(); ++cursor) {
    const auto& frame = loaded.frames[cursor];
    if (frame.identity != result.identity || frame.flags != 0) {
      return corrupt("durable_frame_cross_identity");
    }
    if (frame.kind == DmlUpdateDurableFrameKindV1::recovery_observation) {
      if (frame.payload.size() != 416 ||
          !std::equal(frame.payload.begin(), frame.payload.begin() + 4,
                      "DUMO")) {
        return corrupt("durable_observation_extent_invalid");
      }
      result.latest_dumo = frame.payload;
      continue;
    }
    if (frame.kind ==
        DmlUpdateDurableFrameKindV1::prepared_successor_invalidated) {
      if (!result.staged_successor_present || !frame.payload.empty() ||
          frame.sequence != result.staged_successor.journal_sequence ||
          frame.state != static_cast<std::uint8_t>(
                             result.staged_successor.lifecycle_state) ||
          frame.prior_record_sha256 !=
              result.staged_successor.prior_record_sha256 ||
          frame.record_evidence_sha256 !=
              result.staged_successor.record_evidence_sha256) {
        return corrupt("durable_prepared_successor_invalidation_invalid");
      }
      result.staged_successor_present = false;
      result.staged_successor = {};
      result.staged_encoded_journal_frame.clear();
      continue;
    }
    if (frame.kind == DmlUpdateDurableFrameKindV1::prepared_successor) {
      if (result.journal.empty() || result.staged_successor_present ||
          frame.flags != 0 || frame.payload.empty()) {
        return corrupt("durable_prepared_successor_position_invalid");
      }
      DmlUpdateDurableFrameV1 staged_frame;
      if (!DmlUpdateDurableDecodeFrame(frame.payload, &staged_frame,
                                       &result.detail) ||
          staged_frame.kind != DmlUpdateDurableFrameKindV1::journal ||
          staged_frame.identity != result.identity ||
          staged_frame.flags != 0 ||
          frame.sequence != staged_frame.sequence ||
          frame.state != staged_frame.state ||
          frame.prior_record_sha256 !=
              staged_frame.prior_record_sha256 ||
          frame.record_evidence_sha256 !=
              staged_frame.record_evidence_sha256) {
        return corrupt(result.detail.empty()
                           ? "durable_prepared_successor_frame_invalid"
                           : result.detail);
      }
      MgaDmlUpdateDurableJournalExtentV1 staged_extent;
      staged_extent.journal_sequence = staged_frame.sequence;
      staged_extent.lifecycle_state =
          static_cast<MgaDmlUpdateDurableJournalStateV1>(staged_frame.state);
      staged_extent.prior_record_sha256 =
          staged_frame.prior_record_sha256;
      staged_extent.record_evidence_sha256 =
          staged_frame.record_evidence_sha256;
      staged_extent.exact_dujr_bytes = staged_frame.payload;
      const auto& prior = result.journal.back();
      if (!DmlUpdateDurableJournalExtentMatchesBytes(
              result.identity, result.snapshot, staged_extent,
              &result.detail) ||
          staged_extent.journal_sequence != prior.journal_sequence + 1 ||
          staged_extent.prior_record_sha256 !=
              prior.record_evidence_sha256 ||
          !DmlUpdateDurableLegalTransition(
              prior.lifecycle_state, staged_extent.lifecycle_state)) {
        return corrupt(result.detail.empty()
                           ? "durable_prepared_successor_cas_invalid"
                           : result.detail);
      }
      result.staged_successor_present = true;
      result.staged_successor = std::move(staged_extent);
      result.staged_encoded_journal_frame = frame.payload;
      continue;
    }
    if (frame.kind != DmlUpdateDurableFrameKindV1::journal) {
      return corrupt("durable_frame_kind_invalid");
    }
    MgaDmlUpdateDurableJournalExtentV1 extent;
    extent.journal_sequence = frame.sequence;
    extent.lifecycle_state =
        static_cast<MgaDmlUpdateDurableJournalStateV1>(frame.state);
    extent.prior_record_sha256 = frame.prior_record_sha256;
    extent.record_evidence_sha256 = frame.record_evidence_sha256;
    extent.exact_dujr_bytes = frame.payload;
    if (!DmlUpdateDurableJournalExtentMatchesBytes(
            result.identity, result.snapshot, extent, &result.detail)) {
      return corrupt(result.detail);
    }
    if (result.journal.empty()) {
      if (extent.journal_sequence != 1 ||
          extent.lifecycle_state !=
              MgaDmlUpdateDurableJournalStateV1::bound ||
          !DmlUpdateDurableZero(extent.prior_record_sha256)) {
        return corrupt("durable_journal_root_invalid");
      }
    } else {
      const auto& prior = result.journal.back();
      if (extent.journal_sequence != prior.journal_sequence + 1 ||
          extent.prior_record_sha256 != prior.record_evidence_sha256 ||
          !DmlUpdateDurableLegalTransition(prior.lifecycle_state,
                                           extent.lifecycle_state)) {
        return corrupt("durable_journal_chain_forked");
      }
    }
    if (result.staged_successor_present) {
      if (extent != result.staged_successor) {
        return corrupt("durable_prepared_successor_commit_mismatch");
      }
      result.staged_successor_present = false;
      result.staged_successor = {};
      result.staged_encoded_journal_frame.clear();
    }
    result.journal.push_back(std::move(extent));
  }
  if (result.journal.empty()) {
    result.reservation_only = false;
  }
  result.ok = true;
  return result;
}

DmlUpdateDurableFrameV1 DmlUpdateDurableJournalFrame(
    const MgaDmlUpdateDurableOperationIdentityV1& identity,
    const MgaDmlUpdateDurableJournalExtentV1& extent) {
  DmlUpdateDurableFrameV1 frame;
  frame.kind = DmlUpdateDurableFrameKindV1::journal;
  frame.identity = identity;
  frame.sequence = extent.journal_sequence;
  frame.state = static_cast<std::uint8_t>(extent.lifecycle_state);
  frame.prior_record_sha256 = extent.prior_record_sha256;
  frame.record_evidence_sha256 = extent.record_evidence_sha256;
  frame.payload = extent.exact_dujr_bytes;
  return frame;
}

bool DmlUpdateDurableEncodePreparedInvalidation(
    const MgaDmlUpdateDurableOperationIdentityV1& identity,
    const MgaDmlUpdateDurableJournalExtentV1& staged,
    std::vector<std::uint8_t>* encoded) {
  DmlUpdateDurableFrameV1 invalidation;
  invalidation.kind =
      DmlUpdateDurableFrameKindV1::prepared_successor_invalidated;
  invalidation.identity = identity;
  invalidation.sequence = staged.journal_sequence;
  invalidation.state =
      static_cast<std::uint8_t>(staged.lifecycle_state);
  invalidation.prior_record_sha256 = staged.prior_record_sha256;
  invalidation.record_evidence_sha256 = staged.record_evidence_sha256;
  g_dml_update_durable_frame_encode_calls.fetch_add(
      1, std::memory_order_relaxed);
  g_dml_update_durable_checksum_calls.fetch_add(
      2, std::memory_order_relaxed);
  return DmlUpdateDurableEncodeFrame(invalidation, encoded);
}

constexpr std::size_t kDmlUpdateStatementSavepointJournalFields = 27;

struct DmlUpdateStatementSavepointJournalRecordV1 {
  MgaDmlUpdateStatementSavepointAuthorityV1 authority;
  std::string private_marker;
  std::uint64_t journal_sequence = 0;
  SavepointCutoffs cutoffs;
  std::uint64_t row_upper_event_sequence = 0;
  std::uint64_t metadata_upper_event_sequence = 0;
  std::uint64_t index_upper_event_sequence = 0;
  MgaDmlUpdateStatementAuthoritySha256V1 prior_record_sha256{};
};

std::mutex& DmlUpdateStatementSavepointJournalMutex() {
  static std::mutex mutex;
  return mutex;
}

EngineApiDiagnostic DmlUpdateStatementSavepointDiagnostic(
    std::string code, std::string key, std::string detail = {}) {
  return MakeEngineApiDiagnostic(std::move(code), std::move(key),
                                 std::move(detail), true);
}

MgaDmlUpdateStatementSavepointAuthorityResultV1
DmlUpdateStatementSavepointFailure(std::string code, std::string key,
                                   std::string detail = {}) {
  MgaDmlUpdateStatementSavepointAuthorityResultV1 result;
  result.diagnostic = DmlUpdateStatementSavepointDiagnostic(
      std::move(code), std::move(key), std::move(detail));
  return result;
}

bool DmlUpdateStatementParseU64(std::string_view text,
                                std::uint64_t* value) {
  if (value == nullptr || text.empty()) return false;
  std::uint64_t parsed = 0;
  const auto converted =
      std::from_chars(text.data(), text.data() + text.size(), parsed, 10);
  if (converted.ec != std::errc{} ||
      converted.ptr != text.data() + text.size()) {
    return false;
  }
  *value = parsed;
  return true;
}

bool DmlUpdateStatementParseUuid(
    std::string_view text, std::array<std::uint8_t, 16>* bytes = nullptr) {
  if (text.empty()) return false;
  const auto parsed = scratchbird::core::uuid::ParseUuid(std::string(text));
  if (!parsed.ok() || scratchbird::core::uuid::IsNilUuid(parsed.value) ||
      scratchbird::core::uuid::UuidToString(parsed.value) != text) {
    return false;
  }
  if (bytes != nullptr) {
    std::copy(parsed.value.bytes.begin(), parsed.value.bytes.end(),
              bytes->begin());
  }
  return true;
}

bool DmlUpdateStatementShaNonzero(
    const MgaDmlUpdateStatementAuthoritySha256V1& value) {
  return std::any_of(value.begin(), value.end(),
                     [](std::uint8_t byte) { return byte != 0; });
}

std::string DmlUpdateStatementShaHex(
    const MgaDmlUpdateStatementAuthoritySha256V1& value) {
  return scratchbird::core::hash::HexLower(value);
}

bool DmlUpdateStatementParseSha(
    std::string_view text, MgaDmlUpdateStatementAuthoritySha256V1* value) {
  if (value == nullptr || text.size() != value->size() * 2) return false;
  MgaDmlUpdateStatementAuthoritySha256V1 parsed{};
  for (std::size_t index = 0; index < parsed.size(); ++index) {
    const int high = HexValue(text[index * 2]);
    const int low = HexValue(text[index * 2 + 1]);
    if (high < 0 || low < 0) return false;
    parsed[index] = static_cast<std::uint8_t>((high << 4) | low);
  }
  *value = parsed;
  return true;
}

void DmlUpdateStatementAppendU64(std::vector<std::uint8_t>* bytes,
                                 std::uint64_t value) {
  for (std::size_t offset = 0; offset < 8; ++offset) {
    bytes->push_back(
        static_cast<std::uint8_t>((value >> (offset * 8)) & 0xffu));
  }
}

bool DmlUpdateStatementAppendUuid(std::vector<std::uint8_t>* bytes,
                                  std::string_view uuid,
                                  bool optional = false) {
  if (optional && uuid.empty()) {
    bytes->insert(bytes->end(), 16, 0);
    return true;
  }
  std::array<std::uint8_t, 16> parsed{};
  if (!DmlUpdateStatementParseUuid(uuid, &parsed)) return false;
  bytes->insert(bytes->end(), parsed.begin(), parsed.end());
  return true;
}

MgaDmlUpdateStatementAuthoritySha256V1
DmlUpdateStatementSavepointRecordSha256(
    const DmlUpdateStatementSavepointJournalRecordV1& record) {
  std::vector<std::uint8_t> material;
  material.reserve(kDmlUpdateStatementSavepointEvidenceDomain.size() + 257);
  material.insert(material.end(),
                  kDmlUpdateStatementSavepointEvidenceDomain.begin(),
                  kDmlUpdateStatementSavepointEvidenceDomain.end());
  material.push_back(1);
  material.push_back(static_cast<std::uint8_t>(record.authority.lifecycle));
  material.push_back(record.authority.publication_barrier_present ? 1 : 0);
  DmlUpdateStatementAppendU64(&material, record.journal_sequence);
  DmlUpdateStatementAppendU64(&material,
                              record.cutoffs.row_event_sequence);
  DmlUpdateStatementAppendU64(&material,
                              record.cutoffs.metadata_event_sequence);
  DmlUpdateStatementAppendU64(&material,
                              record.cutoffs.index_event_sequence);
  DmlUpdateStatementAppendU64(&material, record.row_upper_event_sequence);
  DmlUpdateStatementAppendU64(&material,
                              record.metadata_upper_event_sequence);
  DmlUpdateStatementAppendU64(&material, record.index_upper_event_sequence);
  const auto& binding = record.authority.binding;
  if (!DmlUpdateStatementAppendUuid(&material, binding.database_uuid) ||
      !DmlUpdateStatementAppendUuid(&material,
                                    binding.owning_transaction_uuid) ||
      !DmlUpdateStatementAppendUuid(
          &material, binding.authenticated_statement_receipt_uuid) ||
      !DmlUpdateStatementAppendUuid(&material, binding.operation_uuid) ||
      !DmlUpdateStatementAppendUuid(&material, binding.descriptor_uuid) ||
      !DmlUpdateStatementAppendUuid(&material, binding.recovery_token_uuid) ||
      !DmlUpdateStatementAppendUuid(&material,
                                    record.authority.savepoint_uuid) ||
      !DmlUpdateStatementAppendUuid(
          &material, record.authority.publication_barrier_uuid, true)) {
    return {};
  }
  DmlUpdateStatementAppendU64(&material,
                              binding.owning_local_transaction_id);
  DmlUpdateStatementAppendU64(&material, binding.descriptor_generation);
  DmlUpdateStatementAppendU64(&material, binding.recovery_generation);
  DmlUpdateStatementAppendU64(&material,
                              record.authority.savepoint_generation);
  DmlUpdateStatementAppendU64(
      &material, record.authority.publication_barrier_generation);
  material.insert(material.end(), record.prior_record_sha256.begin(),
                  record.prior_record_sha256.end());
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(material);
  return digest.ok() ? digest.digest
                     : MgaDmlUpdateStatementAuthoritySha256V1{};
}

std::string DmlUpdateStatementPrivateSavepointMarker(
    std::string_view savepoint_uuid) {
  if (!DmlUpdateStatementParseUuid(savepoint_uuid)) return {};
  std::string marker = "__sblr_dml_update_rows_";
  marker.reserve(marker.size() + 32);
  for (const char value : savepoint_uuid) {
    if (value != '-') marker.push_back(value);
  }
  return marker;
}

MgaDmlUpdateDurableOperationIdentityV1
DmlUpdateStatementDurableIdentity(
    const MgaDmlUpdateStatementSavepointBindingV1& binding) {
  MgaDmlUpdateDurableOperationIdentityV1 identity;
  identity.database_uuid = binding.database_uuid;
  identity.owning_transaction_uuid = binding.owning_transaction_uuid;
  identity.owning_local_transaction_id =
      binding.owning_local_transaction_id;
  identity.authenticated_statement_receipt_uuid =
      binding.authenticated_statement_receipt_uuid;
  identity.operation_uuid = binding.operation_uuid;
  identity.descriptor_uuid = binding.descriptor_uuid;
  identity.descriptor_generation = binding.descriptor_generation;
  identity.recovery_token_uuid = binding.recovery_token_uuid;
  identity.recovery_generation = binding.recovery_generation;
  return identity;
}

bool DmlUpdateStatementEncodeBinaryPayload(
    const DmlUpdateStatementSavepointJournalRecordV1& record,
    std::vector<std::uint8_t>* payload) {
  if (payload == nullptr) return false;
  payload->assign(136, 0);
  if (!DmlUpdateDurablePutUuid(payload, 0,
                               record.authority.savepoint_uuid) ||
      !DmlUpdateDurablePutUuid(
          payload, 24, record.authority.publication_barrier_uuid)) {
    return false;
  }
  DmlUpdateDurablePutU64(payload, 16,
                         record.authority.savepoint_generation);
  DmlUpdateDurablePutU64(
      payload, 40, record.authority.publication_barrier_generation);
  (*payload)[48] = record.authority.publication_barrier_present ? 1 : 0;
  (*payload)[49] =
      static_cast<std::uint8_t>(record.authority.lifecycle);
  DmlUpdateDurablePutU64(payload, 56,
                         record.cutoffs.row_event_sequence);
  DmlUpdateDurablePutU64(payload, 64,
                         record.cutoffs.metadata_event_sequence);
  DmlUpdateDurablePutU64(payload, 72,
                         record.cutoffs.index_event_sequence);
  DmlUpdateDurablePutU64(payload, 80, record.row_upper_event_sequence);
  DmlUpdateDurablePutU64(payload, 88,
                         record.metadata_upper_event_sequence);
  DmlUpdateDurablePutU64(payload, 96,
                         record.index_upper_event_sequence);
  std::copy(record.authority.durable_presence_sha256.begin(),
            record.authority.durable_presence_sha256.end(),
            payload->begin() + 104);
  return true;
}

bool DmlUpdateStatementDecodeBinaryFrame(
    const DmlUpdateDurableFrameV1& frame,
    DmlUpdateStatementSavepointJournalRecordV1* record) {
  if (record == nullptr ||
      frame.kind != DmlUpdateDurableFrameKindV1::statement_savepoint ||
      frame.flags != 0 || frame.payload.size() != 136 ||
      frame.sequence < 1 || frame.sequence > 2 ||
      frame.state < static_cast<std::uint8_t>(
                        MgaDmlUpdateStatementSavepointLifecycleV1::active) ||
      frame.state > static_cast<std::uint8_t>(
                        MgaDmlUpdateStatementSavepointLifecycleV1::released) ||
      !DmlUpdateDurableZero(
          std::span<const std::uint8_t>(frame.payload).subspan(50, 6))) {
    return false;
  }
  record->authority.binding.database_uuid = frame.identity.database_uuid;
  record->authority.binding.owning_transaction_uuid =
      frame.identity.owning_transaction_uuid;
  record->authority.binding.owning_local_transaction_id =
      frame.identity.owning_local_transaction_id;
  record->authority.binding.authenticated_statement_receipt_uuid =
      frame.identity.authenticated_statement_receipt_uuid;
  record->authority.binding.operation_uuid = frame.identity.operation_uuid;
  record->authority.binding.descriptor_uuid = frame.identity.descriptor_uuid;
  record->authority.binding.descriptor_generation =
      frame.identity.descriptor_generation;
  record->authority.binding.recovery_token_uuid =
      frame.identity.recovery_token_uuid;
  record->authority.binding.recovery_generation =
      frame.identity.recovery_generation;
  record->authority.savepoint_uuid = DmlUpdateDurableUuidText(
      std::span<const std::uint8_t>(frame.payload).subspan(0, 16));
  record->authority.publication_barrier_uuid = DmlUpdateDurableUuidText(
      std::span<const std::uint8_t>(frame.payload).subspan(24, 16));
  if (!DmlUpdateDurableReadU64(frame.payload, 16,
                               &record->authority.savepoint_generation) ||
      !DmlUpdateDurableReadU64(
          frame.payload, 40,
          &record->authority.publication_barrier_generation) ||
      !DmlUpdateDurableReadU64(frame.payload, 56,
                               &record->cutoffs.row_event_sequence) ||
      !DmlUpdateDurableReadU64(frame.payload, 64,
                               &record->cutoffs.metadata_event_sequence) ||
      !DmlUpdateDurableReadU64(frame.payload, 72,
                               &record->cutoffs.index_event_sequence) ||
      !DmlUpdateDurableReadU64(frame.payload, 80,
                               &record->row_upper_event_sequence) ||
      !DmlUpdateDurableReadU64(frame.payload, 88,
                               &record->metadata_upper_event_sequence) ||
      !DmlUpdateDurableReadU64(frame.payload, 96,
                               &record->index_upper_event_sequence)) {
    return false;
  }
  record->authority.publication_barrier_present = frame.payload[48] != 0;
  record->authority.lifecycle =
      static_cast<MgaDmlUpdateStatementSavepointLifecycleV1>(
          frame.payload[49]);
  std::copy_n(frame.payload.begin() + 104, 32,
              record->authority.durable_presence_sha256.begin());
  record->journal_sequence = frame.sequence;
  record->prior_record_sha256 = frame.prior_record_sha256;
  record->private_marker = DmlUpdateStatementPrivateSavepointMarker(
      record->authority.savepoint_uuid);
  const bool terminal =
      record->authority.lifecycle !=
      MgaDmlUpdateStatementSavepointLifecycleV1::active;
  const bool barrier_shape =
      DmlUpdateStatementParseUuid(
          record->authority.publication_barrier_uuid) &&
      record->authority.publication_barrier_generation == 1 &&
      record->authority.publication_barrier_uuid !=
          record->authority.savepoint_uuid &&
      record->authority.publication_barrier_present ==
          (record->authority.lifecycle ==
           MgaDmlUpdateStatementSavepointLifecycleV1::released);
  const bool active_shape =
      record->authority.lifecycle !=
          MgaDmlUpdateStatementSavepointLifecycleV1::active ||
      (record->row_upper_event_sequence == 0 &&
       record->metadata_upper_event_sequence == 0 &&
       record->index_upper_event_sequence == 0 &&
       !DmlUpdateStatementShaNonzero(record->prior_record_sha256));
  const bool release_shape =
      record->authority.lifecycle !=
          MgaDmlUpdateStatementSavepointLifecycleV1::released ||
      (record->row_upper_event_sequence == 0 &&
       record->metadata_upper_event_sequence == 0 &&
       record->index_upper_event_sequence == 0);
  const bool rollback_shape =
      record->authority.lifecycle !=
          MgaDmlUpdateStatementSavepointLifecycleV1::rolled_back ||
      (record->row_upper_event_sequence >=
           record->cutoffs.row_event_sequence &&
       record->metadata_upper_event_sequence >=
           record->cutoffs.metadata_event_sequence &&
       record->index_upper_event_sequence >=
           record->cutoffs.index_event_sequence);
  const auto expected = DmlUpdateStatementSavepointRecordSha256(*record);
  return frame.state == frame.payload[49] &&
         record->authority.savepoint_generation == 1 &&
         DmlUpdateStatementParseUuid(record->authority.savepoint_uuid) &&
         !record->private_marker.empty() &&
         record->journal_sequence == (terminal ? 2 : 1) && barrier_shape &&
         active_shape && release_shape && rollback_shape &&
         frame.record_evidence_sha256 ==
             record->authority.durable_presence_sha256 &&
         DmlUpdateStatementShaNonzero(expected) &&
         expected == record->authority.durable_presence_sha256;
}

bool DmlUpdateStatementLoadBinaryChain(
    const EngineRequestContext& context, std::string_view savepoint_uuid,
    std::vector<DmlUpdateStatementSavepointJournalRecordV1>* records,
    std::string* detail) {
  if (records == nullptr || !DmlUpdateStatementParseUuid(savepoint_uuid)) {
    if (detail != nullptr) *detail = "savepoint_identity_invalid";
    return false;
  }
  const std::string path =
      DmlUpdateDurableSavepointPath(context, savepoint_uuid);
  DmlUpdateDurableFileLock lock(path);
  if (!lock.ok()) {
    if (detail != nullptr) *detail = "savepoint_store_lock_failed";
    return false;
  }
  const auto loaded = DmlUpdateDurableLoadFrames(path);
  if (!loaded.ok || loaded.missing || loaded.frames.empty() ||
      loaded.frames.size() > 2) {
    if (detail != nullptr) {
      *detail = loaded.detail.empty() ? "savepoint_identity_unknown"
                                     : loaded.detail;
    }
    return false;
  }
  records->clear();
  records->reserve(loaded.frames.size());
  for (const auto& frame : loaded.frames) {
    DmlUpdateStatementSavepointJournalRecordV1 record;
    if (!DmlUpdateStatementDecodeBinaryFrame(frame, &record) ||
        record.authority.savepoint_uuid != savepoint_uuid) {
      if (detail != nullptr) *detail = "savepoint_binary_record_invalid";
      return false;
    }
    records->push_back(std::move(record));
  }
  const auto& first = records->front();
  if (first.journal_sequence != 1 ||
      first.authority.lifecycle !=
          MgaDmlUpdateStatementSavepointLifecycleV1::active ||
      DmlUpdateStatementShaNonzero(first.prior_record_sha256)) {
    if (detail != nullptr) *detail = "savepoint_binary_chain_root_invalid";
    return false;
  }
  if (records->size() == 2) {
    const auto& terminal = records->back();
    if (terminal.journal_sequence != 2 ||
        terminal.authority.lifecycle ==
            MgaDmlUpdateStatementSavepointLifecycleV1::active ||
        terminal.authority.binding != first.authority.binding ||
        terminal.authority.publication_barrier_uuid !=
            first.authority.publication_barrier_uuid ||
        terminal.authority.publication_barrier_generation !=
            first.authority.publication_barrier_generation ||
        terminal.prior_record_sha256 !=
            first.authority.durable_presence_sha256) {
      if (detail != nullptr) *detail = "savepoint_binary_chain_forked";
      return false;
    }
  }
  return true;
}

bool DmlUpdateStatementAppendBinaryRecord(
    const EngineRequestContext& context,
    const DmlUpdateStatementSavepointJournalRecordV1& record) {
  const std::string directory =
      DmlUpdateStatementSavepointBinaryStorePath(context);
  if (!DmlUpdateDurableEnsureDirectory(directory)) return false;
  const std::string path =
      DmlUpdateDurableSavepointPath(context,
                                    record.authority.savepoint_uuid);
  DmlUpdateDurableFileLock lock(path);
  if (!lock.ok()) return false;
  const auto loaded = DmlUpdateDurableLoadFrames(path);
  if (!loaded.ok) return false;
  if ((record.journal_sequence == 1 && !loaded.frames.empty()) ||
      (record.journal_sequence == 2 && loaded.frames.size() != 1)) {
    return false;
  }
  DmlUpdateDurableFrameV1 frame;
  frame.kind = DmlUpdateDurableFrameKindV1::statement_savepoint;
  frame.identity =
      DmlUpdateStatementDurableIdentity(record.authority.binding);
  frame.sequence = record.journal_sequence;
  frame.state = static_cast<std::uint8_t>(record.authority.lifecycle);
  frame.prior_record_sha256 = record.prior_record_sha256;
  frame.record_evidence_sha256 =
      record.authority.durable_presence_sha256;
  if (!DmlUpdateStatementEncodeBinaryPayload(record, &frame.payload)) {
    return false;
  }
  return DmlUpdateDurableAppendFrame(path, frame);
}

bool ApplyDmlUpdateBinarySavepointRecords(
    const EngineRequestContext& context, SavepointParsedState* state,
    std::string* refusal_detail) {
  if (state == nullptr) {
    if (refusal_detail != nullptr) *refusal_detail = "savepoint_state_required";
    return false;
  }
  const std::string directory =
      DmlUpdateStatementSavepointBinaryStorePath(context);
  std::error_code error;
  if (!std::filesystem::exists(directory, error)) return !error;
  if (error || !std::filesystem::is_directory(directory, error) || error) {
    if (refusal_detail != nullptr) {
      *refusal_detail = "update_savepoint_store_directory_invalid";
    }
    return false;
  }
  std::vector<std::filesystem::path> paths;
  for (std::filesystem::directory_iterator iterator(directory, error), end;
       !error && iterator != end; iterator.increment(error)) {
    if (!iterator->is_regular_file(error) || error) break;
    const auto path = iterator->path();
    if (path.extension() == ".dups") paths.push_back(path);
  }
  if (error) {
    if (refusal_detail != nullptr) {
      *refusal_detail = "update_savepoint_store_enumeration_failed";
    }
    return false;
  }
  std::ranges::sort(paths);
  constexpr std::size_t kMaximumDurableSavepointFiles = 1048576;
  if (paths.size() > kMaximumDurableSavepointFiles) {
    if (refusal_detail != nullptr) {
      *refusal_detail = "update_savepoint_store_count_exceeded";
    }
    return false;
  }
  for (const auto& path : paths) {
    const std::string filename = path.stem().string();
    std::vector<DmlUpdateStatementSavepointJournalRecordV1> records;
    std::string detail;
    if (!DmlUpdateStatementLoadBinaryChain(context, filename, &records,
                                            &detail)) {
      if (refusal_detail != nullptr) {
        *refusal_detail = detail.empty()
                              ? "update_savepoint_store_record_invalid"
                              : detail;
      }
      return false;
    }
    const auto& first = records.front();
    const auto tx_id = first.authority.binding.owning_local_transaction_id;
    const auto marker = first.private_marker;
    const auto preexisting_tx = state->active_savepoints.find(tx_id);
    if (preexisting_tx != state->active_savepoints.end() &&
        preexisting_tx->second.find(marker) !=
            preexisting_tx->second.end()) {
      if (refusal_detail != nullptr) {
        *refusal_detail = "update_savepoint_text_binary_contradiction";
      }
      return false;
    }
    state->active_savepoints[tx_id][marker] = first.cutoffs;
    if (records.size() == 1) continue;
    const auto& terminal = records.back();
    if (terminal.authority.lifecycle ==
        MgaDmlUpdateStatementSavepointLifecycleV1::rolled_back) {
      SavepointRollbackRange range;
      range.cutoffs = terminal.cutoffs;
      range.row_upper_event_sequence = terminal.row_upper_event_sequence;
      range.metadata_upper_event_sequence =
          terminal.metadata_upper_event_sequence;
      range.index_upper_event_sequence = terminal.index_upper_event_sequence;
      state->rollback_ranges[tx_id].push_back(range);
    }
    state->active_savepoints[tx_id].erase(marker);
  }
  return true;
}

bool DmlUpdateStatementBindingMatchesContext(
    const EngineRequestContext& context,
    const MgaDmlUpdateStatementSavepointBindingV1& binding) {
  return !context.database_path.empty() &&
         DmlUpdateStatementParseUuid(binding.database_uuid) &&
         binding.database_uuid == context.database_uuid.canonical &&
         DmlUpdateStatementParseUuid(binding.owning_transaction_uuid) &&
         binding.owning_transaction_uuid == context.transaction_uuid.canonical &&
         binding.owning_local_transaction_id != 0 &&
         binding.owning_local_transaction_id == context.local_transaction_id &&
         DmlUpdateStatementParseUuid(
             binding.authenticated_statement_receipt_uuid) &&
         binding.authenticated_statement_receipt_uuid ==
             context.statement_receipt_uuid.canonical &&
         DmlUpdateStatementParseUuid(binding.operation_uuid) &&
         DmlUpdateStatementParseUuid(binding.descriptor_uuid) &&
         binding.descriptor_generation != 0 &&
         DmlUpdateStatementParseUuid(binding.recovery_token_uuid) &&
         binding.recovery_generation != 0;
}

bool DmlUpdateStatementRecordKind(
    std::string_view kind,
    MgaDmlUpdateStatementSavepointLifecycleV1* lifecycle) {
  if (lifecycle == nullptr) return false;
  if (kind == kDmlUpdateStatementSavepointCreateKind) {
    *lifecycle = MgaDmlUpdateStatementSavepointLifecycleV1::active;
    return true;
  }
  if (kind == kDmlUpdateStatementSavepointRollbackKind) {
    *lifecycle = MgaDmlUpdateStatementSavepointLifecycleV1::rolled_back;
    return true;
  }
  if (kind == kDmlUpdateStatementSavepointReleaseKind) {
    *lifecycle = MgaDmlUpdateStatementSavepointLifecycleV1::released;
    return true;
  }
  return false;
}

bool DmlUpdateStatementParseJournalRecord(
    const std::vector<std::string>& fields,
    DmlUpdateStatementSavepointJournalRecordV1* record) {
  if (record == nullptr ||
      fields.size() != kDmlUpdateStatementSavepointJournalFields ||
      fields[0] != kRowStoreMagic ||
      !DmlUpdateStatementRecordKind(fields[1],
                                    &record->authority.lifecycle) ||
      !DmlUpdateStatementParseU64(
          fields[2], &record->authority.binding.owning_local_transaction_id) ||
      record->authority.binding.owning_local_transaction_id == 0 ||
      !DmlUpdateStatementParseU64(fields[4],
                                  &record->cutoffs.row_event_sequence) ||
      !DmlUpdateStatementParseU64(fields[5],
                                  &record->cutoffs.metadata_event_sequence) ||
      !DmlUpdateStatementParseU64(fields[6],
                                  &record->cutoffs.index_event_sequence) ||
      !DmlUpdateStatementParseU64(fields[7],
                                  &record->row_upper_event_sequence) ||
      !DmlUpdateStatementParseU64(fields[8],
                                  &record->metadata_upper_event_sequence) ||
      !DmlUpdateStatementParseU64(fields[9],
                                  &record->index_upper_event_sequence)) {
    return false;
  }
  std::uint64_t format_version = 0;
  if (!DmlUpdateStatementParseU64(fields[10], &format_version) ||
      format_version != 1 ||
      !DmlUpdateStatementParseU64(fields[11], &record->journal_sequence)) {
    return false;
  }
  auto& binding = record->authority.binding;
  binding.database_uuid = fields[12];
  binding.owning_transaction_uuid = fields[13];
  binding.authenticated_statement_receipt_uuid = fields[14];
  binding.operation_uuid = fields[15];
  binding.descriptor_uuid = fields[16];
  if (!DmlUpdateStatementParseU64(fields[17],
                                  &binding.descriptor_generation)) {
    return false;
  }
  binding.recovery_token_uuid = fields[18];
  if (!DmlUpdateStatementParseU64(fields[19],
                                  &binding.recovery_generation)) {
    return false;
  }
  record->authority.savepoint_uuid = fields[20];
  if (!DmlUpdateStatementParseU64(
          fields[21], &record->authority.savepoint_generation)) {
    return false;
  }
  record->authority.publication_barrier_uuid = fields[22];
  if (!DmlUpdateStatementParseU64(
          fields[23], &record->authority.publication_barrier_generation)) {
    return false;
  }
  std::uint64_t barrier_present = 0;
  if (!DmlUpdateStatementParseU64(fields[24], &barrier_present) ||
      barrier_present > 1 ||
      !DmlUpdateStatementParseSha(fields[25],
                                  &record->prior_record_sha256) ||
      !DmlUpdateStatementParseSha(
          fields[26], &record->authority.durable_presence_sha256)) {
    return false;
  }
  record->authority.publication_barrier_present = barrier_present == 1;
  record->private_marker = DecodeCrudTextLocal(fields[3]);
  const bool terminal =
      record->authority.lifecycle !=
      MgaDmlUpdateStatementSavepointLifecycleV1::active;
  if (!DmlUpdateStatementParseUuid(binding.database_uuid) ||
      !DmlUpdateStatementParseUuid(binding.owning_transaction_uuid) ||
      !DmlUpdateStatementParseUuid(
          binding.authenticated_statement_receipt_uuid) ||
      !DmlUpdateStatementParseUuid(binding.operation_uuid) ||
      !DmlUpdateStatementParseUuid(binding.descriptor_uuid) ||
      binding.descriptor_generation == 0 ||
      !DmlUpdateStatementParseUuid(binding.recovery_token_uuid) ||
      binding.recovery_generation == 0 ||
      !DmlUpdateStatementParseUuid(record->authority.savepoint_uuid) ||
      record->authority.savepoint_generation != 1 ||
      record->private_marker != DmlUpdateStatementPrivateSavepointMarker(
                                    record->authority.savepoint_uuid) ||
      record->journal_sequence != (terminal ? 2 : 1)) {
    return false;
  }
  if (!DmlUpdateStatementParseUuid(
          record->authority.publication_barrier_uuid) ||
      record->authority.publication_barrier_generation != 1 ||
      record->authority.publication_barrier_uuid ==
          record->authority.savepoint_uuid ||
      record->authority.publication_barrier_uuid == binding.database_uuid ||
      record->authority.publication_barrier_uuid ==
          binding.owning_transaction_uuid ||
      record->authority.publication_barrier_uuid ==
          binding.authenticated_statement_receipt_uuid ||
      record->authority.publication_barrier_uuid == binding.operation_uuid ||
      record->authority.publication_barrier_uuid == binding.descriptor_uuid ||
      record->authority.publication_barrier_uuid ==
          binding.recovery_token_uuid ||
      record->authority.publication_barrier_present !=
          (record->authority.lifecycle ==
           MgaDmlUpdateStatementSavepointLifecycleV1::released)) {
    return false;
  }
  if (record->authority.lifecycle ==
          MgaDmlUpdateStatementSavepointLifecycleV1::active &&
      (record->row_upper_event_sequence != 0 ||
       record->metadata_upper_event_sequence != 0 ||
       record->index_upper_event_sequence != 0 ||
       DmlUpdateStatementShaNonzero(record->prior_record_sha256))) {
    return false;
  }
  if (record->authority.lifecycle ==
          MgaDmlUpdateStatementSavepointLifecycleV1::released &&
      (record->row_upper_event_sequence != 0 ||
       record->metadata_upper_event_sequence != 0 ||
       record->index_upper_event_sequence != 0)) {
    return false;
  }
  if (record->authority.lifecycle ==
          MgaDmlUpdateStatementSavepointLifecycleV1::rolled_back &&
      (record->row_upper_event_sequence <
           record->cutoffs.row_event_sequence ||
       record->metadata_upper_event_sequence <
           record->cutoffs.metadata_event_sequence ||
       record->index_upper_event_sequence <
           record->cutoffs.index_event_sequence)) {
    return false;
  }
  const auto expected = DmlUpdateStatementSavepointRecordSha256(*record);
  return DmlUpdateStatementShaNonzero(expected) &&
         expected == record->authority.durable_presence_sha256;
}

std::string DmlUpdateStatementEncodeJournalRecord(
    const DmlUpdateStatementSavepointJournalRecordV1& record) {
  std::string kind;
  switch (record.authority.lifecycle) {
    case MgaDmlUpdateStatementSavepointLifecycleV1::active:
      kind = std::string(kDmlUpdateStatementSavepointCreateKind);
      break;
    case MgaDmlUpdateStatementSavepointLifecycleV1::rolled_back:
      kind = std::string(kDmlUpdateStatementSavepointRollbackKind);
      break;
    case MgaDmlUpdateStatementSavepointLifecycleV1::released:
      kind = std::string(kDmlUpdateStatementSavepointReleaseKind);
      break;
  }
  const auto& authority = record.authority;
  const auto& binding = authority.binding;
  return JoinLine(
      {kRowStoreMagic,
       kind,
       std::to_string(binding.owning_local_transaction_id),
       EncodeCrudText(record.private_marker),
       std::to_string(record.cutoffs.row_event_sequence),
       std::to_string(record.cutoffs.metadata_event_sequence),
       std::to_string(record.cutoffs.index_event_sequence),
       std::to_string(record.row_upper_event_sequence),
       std::to_string(record.metadata_upper_event_sequence),
       std::to_string(record.index_upper_event_sequence),
       "1",
       std::to_string(record.journal_sequence),
       binding.database_uuid,
       binding.owning_transaction_uuid,
       binding.authenticated_statement_receipt_uuid,
       binding.operation_uuid,
       binding.descriptor_uuid,
       std::to_string(binding.descriptor_generation),
       binding.recovery_token_uuid,
       std::to_string(binding.recovery_generation),
       authority.savepoint_uuid,
       std::to_string(authority.savepoint_generation),
       authority.publication_barrier_uuid,
       std::to_string(authority.publication_barrier_generation),
       authority.publication_barrier_present ? "1" : "0",
       DmlUpdateStatementShaHex(record.prior_record_sha256),
       DmlUpdateStatementShaHex(authority.durable_presence_sha256)});
}

MgaDmlUpdateStatementSavepointAuthorityResultV1
DmlUpdateStatementLoadSavepointAuthority(
    const EngineRequestContext& context,
    const MgaDmlUpdateStatementSavepointBindingV1& binding,
    const std::string& savepoint_uuid, std::uint64_t savepoint_generation) {
  if (!DmlUpdateStatementBindingMatchesContext(context, binding) ||
      !DmlUpdateStatementParseUuid(savepoint_uuid) ||
      savepoint_generation != 1) {
    return DmlUpdateStatementSavepointFailure(
        "MGA.TRANSACTION.STALE",
        "sblr.dml_update_rows.statement_savepoint_stale",
        "savepoint_binding_or_generation_mismatch");
  }

  std::vector<DmlUpdateStatementSavepointJournalRecordV1> chain;
  std::string binary_detail;
  if (!DmlUpdateStatementLoadBinaryChain(
          context, savepoint_uuid, &chain, &binary_detail)) {
    return DmlUpdateStatementSavepointFailure(
        binary_detail == "savepoint_identity_unknown"
            ? "MGA.TRANSACTION.STALE"
            : "DML.UPDATE_FAILED",
        binary_detail == "savepoint_identity_unknown"
            ? "sblr.dml_update_rows.statement_savepoint_stale"
            : "sblr.dml_update_rows.statement_savepoint_corrupt",
        binary_detail);
  }
  if (chain.empty() || chain.size() > 2 ||
      chain.front().authority.lifecycle !=
          MgaDmlUpdateStatementSavepointLifecycleV1::active ||
      chain.front().journal_sequence != 1 ||
      DmlUpdateStatementShaNonzero(chain.front().prior_record_sha256)) {
    return DmlUpdateStatementSavepointFailure(
        "DML.UPDATE_FAILED",
        "sblr.dml_update_rows.statement_savepoint_corrupt",
        "savepoint_journal_chain_invalid");
  }
  if (chain.front().authority.binding != binding) {
    return DmlUpdateStatementSavepointFailure(
        "MGA.TRANSACTION.STALE",
        "sblr.dml_update_rows.statement_savepoint_stale",
        "savepoint_cross_authority_replay");
  }
  if (chain.size() == 2 &&
      (chain.back().authority.lifecycle ==
           MgaDmlUpdateStatementSavepointLifecycleV1::active ||
       chain.back().authority.binding != chain.front().authority.binding ||
       chain.back().cutoffs.row_event_sequence !=
           chain.front().cutoffs.row_event_sequence ||
       chain.back().cutoffs.metadata_event_sequence !=
           chain.front().cutoffs.metadata_event_sequence ||
       chain.back().cutoffs.index_event_sequence !=
           chain.front().cutoffs.index_event_sequence ||
       chain.back().authority.publication_barrier_uuid !=
           chain.front().authority.publication_barrier_uuid ||
       chain.back().authority.publication_barrier_generation !=
           chain.front().authority.publication_barrier_generation ||
       chain.back().prior_record_sha256 !=
           chain.front().authority.durable_presence_sha256)) {
    return DmlUpdateStatementSavepointFailure(
        "DML.UPDATE_FAILED",
        "sblr.dml_update_rows.statement_savepoint_corrupt",
        "savepoint_terminal_chain_invalid");
  }

  const auto& latest = chain.back();
  const auto parsed_savepoints = ParseSavepoints(context);
  if (parsed_savepoints.update_statement_authority_corrupt) {
    return DmlUpdateStatementSavepointFailure(
        "DML.UPDATE_FAILED",
        "sblr.dml_update_rows.statement_savepoint_corrupt",
        "text_binary_savepoint_authority_contradiction");
  }
  bool marker_active = false;
  const auto tx = parsed_savepoints.active_savepoints.find(
      binding.owning_local_transaction_id);
  if (tx != parsed_savepoints.active_savepoints.end()) {
    marker_active =
        tx->second.find(latest.private_marker) != tx->second.end();
  }
  const bool expected_active =
      latest.authority.lifecycle ==
      MgaDmlUpdateStatementSavepointLifecycleV1::active;
  if (marker_active != expected_active) {
    return DmlUpdateStatementSavepointFailure(
        "DML.UPDATE_FAILED",
        "sblr.dml_update_rows.statement_savepoint_corrupt",
        expected_active ? "active_savepoint_presence_missing"
                        : "terminal_savepoint_and_active_marker_contradictory");
  }

  MgaDmlUpdateStatementSavepointAuthorityResultV1 result;
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  result.authority = latest.authority;
  return result;
}

bool DmlUpdateStatementAuthorityExact(
    const MgaDmlUpdateStatementSavepointAuthorityV1& left,
    const MgaDmlUpdateStatementSavepointAuthorityV1& right) {
  return left == right;
}

std::string DmlUpdateStatementFreshDistinctUuid(
    const MgaDmlUpdateStatementSavepointBindingV1& binding,
    std::string_view other = {}) {
  for (std::size_t attempt = 0; attempt < 8; ++attempt) {
    const std::string candidate = GenerateCrudEngineUuid("object");
    if (DmlUpdateStatementParseUuid(candidate) &&
        candidate != binding.database_uuid &&
        candidate != binding.owning_transaction_uuid &&
        candidate != binding.authenticated_statement_receipt_uuid &&
        candidate != binding.operation_uuid && candidate != binding.descriptor_uuid &&
        candidate != binding.recovery_token_uuid && candidate != other) {
      return candidate;
    }
  }
  return {};
}

MgaDmlUpdateStatementSavepointBindingV1
DmlUpdateDurableStatementBinding(
    const MgaDmlUpdateDurableOperationIdentityV1& identity) {
  MgaDmlUpdateStatementSavepointBindingV1 binding;
  binding.database_uuid = identity.database_uuid;
  binding.owning_transaction_uuid = identity.owning_transaction_uuid;
  binding.owning_local_transaction_id = identity.owning_local_transaction_id;
  binding.authenticated_statement_receipt_uuid =
      identity.authenticated_statement_receipt_uuid;
  binding.operation_uuid = identity.operation_uuid;
  binding.descriptor_uuid = identity.descriptor_uuid;
  binding.descriptor_generation = identity.descriptor_generation;
  binding.recovery_token_uuid = identity.recovery_token_uuid;
  binding.recovery_generation = identity.recovery_generation;
  return binding;
}

enum class DmlUpdateDurableSavepointLookupStateV1 : std::uint8_t {
  absent = 0,
  present = 1,
  corrupt = 2,
};

struct DmlUpdateDurableSavepointLookupV1 {
  DmlUpdateDurableSavepointLookupStateV1 state =
      DmlUpdateDurableSavepointLookupStateV1::absent;
  MgaDmlUpdateStatementSavepointAuthorityV1 authority;
  std::string detail;
};

DmlUpdateDurableSavepointLookupV1 DmlUpdateDurableFindStatementSavepoint(
    const EngineRequestContext& context,
    const MgaDmlUpdateDurableOperationIdentityV1& identity,
    std::string_view required_savepoint_uuid = {}) {
  DmlUpdateDurableSavepointLookupV1 result;
  const auto expected_binding = DmlUpdateDurableStatementBinding(identity);
  const std::string directory =
      DmlUpdateStatementSavepointBinaryStorePath(context);
  std::error_code error;
  if (!std::filesystem::exists(directory, error)) {
    if (error) {
      result.state = DmlUpdateDurableSavepointLookupStateV1::corrupt;
      result.detail = "savepoint_store_presence_failed";
    }
    return result;
  }
  if (!std::filesystem::is_directory(directory, error) || error) {
    result.state = DmlUpdateDurableSavepointLookupStateV1::corrupt;
    result.detail = "savepoint_store_directory_invalid";
    return result;
  }

  std::vector<std::filesystem::path> paths;
  if (!required_savepoint_uuid.empty()) {
    if (!DmlUpdateStatementParseUuid(required_savepoint_uuid)) {
      result.state = DmlUpdateDurableSavepointLookupStateV1::corrupt;
      result.detail = "required_savepoint_identity_invalid";
      return result;
    }
    paths.emplace_back(DmlUpdateDurableSavepointPath(
        context, required_savepoint_uuid));
  } else {
    for (std::filesystem::directory_iterator iterator(directory, error), end;
         !error && iterator != end; iterator.increment(error)) {
      if (!iterator->is_regular_file(error) || error) break;
      if (iterator->path().extension() == ".dups") {
        paths.push_back(iterator->path());
      }
    }
    if (error || paths.size() > 1048576) {
      result.state = DmlUpdateDurableSavepointLookupStateV1::corrupt;
      result.detail = error ? "savepoint_store_enumeration_failed"
                            : "savepoint_store_count_exceeded";
      return result;
    }
    std::ranges::sort(paths);
  }

  for (const auto& path : paths) {
    if (!std::filesystem::exists(path, error)) {
      if (error) {
        result.state = DmlUpdateDurableSavepointLookupStateV1::corrupt;
        result.detail = "savepoint_store_presence_failed";
      }
      continue;
    }
    const std::string uuid = path.stem().string();
    std::vector<DmlUpdateStatementSavepointJournalRecordV1> chain;
    std::string detail;
    if (!DmlUpdateStatementLoadBinaryChain(context, uuid, &chain, &detail)) {
      result.state = DmlUpdateDurableSavepointLookupStateV1::corrupt;
      result.detail = detail.empty() ? "savepoint_chain_invalid" : detail;
      return result;
    }
    if (chain.empty() || chain.front().authority.binding != expected_binding) {
      continue;
    }
    if (result.state == DmlUpdateDurableSavepointLookupStateV1::present) {
      result.state = DmlUpdateDurableSavepointLookupStateV1::corrupt;
      result.detail = "multiple_statement_savepoints_for_operation";
      return result;
    }
    result.state = DmlUpdateDurableSavepointLookupStateV1::present;
    result.authority = chain.back().authority;
  }
  if (!required_savepoint_uuid.empty() &&
      result.state == DmlUpdateDurableSavepointLookupStateV1::absent) {
    result.detail = "required_savepoint_identity_unknown";
  }
  return result;
}

bool DmlUpdateDurableDecodeTypedJournalChain(
    const EngineRequestContext& context,
    const MgaDmlUpdateDurableOperationIdentityV1& identity,
    const MgaDmlUpdateDurableAuthoritySnapshotV1& snapshot,
    std::span<const MgaDmlUpdateDurableJournalExtentV1> extents,
    std::vector<scratchbird::wire::TypedUpdateJournalRecord>* records,
    std::string* detail) {
  const auto fail = [&](std::string reason) {
    if (detail != nullptr) *detail = std::move(reason);
    return false;
  };
  if (records == nullptr || extents.empty() ||
      snapshot.descriptor_dudc.size() !=
          scratchbird::wire::kTypedUpdateDescriptorBytes) {
    return fail("durable_journal_chain_required");
  }
  records->clear();
  records->reserve(extents.size());
  for (std::size_t index = 0; index < extents.size(); ++index) {
    scratchbird::wire::TypedUpdateJournalChainContext chain;
    chain.first_record = index == 0;
    if (!chain.first_record) {
      const auto& prior = records->back();
      chain.prior_sequence = prior.journal_sequence;
      chain.prior_state = prior.lifecycle_state;
      chain.prior_record_evidence_sha256 = prior.record_evidence_sha256;
      chain.prior_savepoint_uuid = prior.statement_savepoint_uuid;
      chain.prior_savepoint_generation = prior.statement_savepoint_generation;
      chain.require_same_descriptor = true;
      std::copy(snapshot.descriptor_dudc.begin(),
                snapshot.descriptor_dudc.end(),
                chain.expected_descriptor_bytes.begin());
      if (prior.lifecycle_state ==
              scratchbird::wire::TypedUpdateJournalState::prepared) {
        if (!prior.embedded_result_bytes.has_value() ||
            prior.embedded_result_bytes->size() !=
                scratchbird::wire::kTypedUpdateResultBytes) {
          return fail("prepared_journal_result_missing");
        }
        std::array<scratchbird::core::platform::byte,
                   scratchbird::wire::kTypedUpdateResultBytes> exact{};
        std::copy(prior.embedded_result_bytes->begin(),
                  prior.embedded_result_bytes->end(), exact.begin());
        chain.expected_prepared_result_bytes = exact;
      }
      // A crash may leave a provider-owned savepoint after bound but before
      // intent.  The bound-to-aborted codec edge can trust a nonnil savepoint
      // only after the MGA savepoint store authenticates that exact row.
      if (prior.lifecycle_state ==
              scratchbird::wire::TypedUpdateJournalState::bound &&
          extents[index].lifecycle_state ==
              MgaDmlUpdateDurableJournalStateV1::aborted &&
          extents[index].exact_dujr_bytes.size() >= 176 &&
          !DmlUpdateDurableZero(std::span<const std::uint8_t>(
              extents[index].exact_dujr_bytes).subspan(152, 16))) {
        const std::string savepoint_uuid = DmlUpdateDurableUuidText(
            std::span<const std::uint8_t>(extents[index].exact_dujr_bytes)
                .subspan(152, 16));
        std::uint64_t savepoint_generation = 0;
        const auto authority = DmlUpdateDurableFindStatementSavepoint(
            context, identity, savepoint_uuid);
        if (!DmlUpdateDurableReadU64(extents[index].exact_dujr_bytes, 168,
                                     &savepoint_generation) ||
            authority.state !=
                DmlUpdateDurableSavepointLookupStateV1::present ||
            authority.authority.savepoint_uuid != savepoint_uuid ||
            authority.authority.savepoint_generation != savepoint_generation ||
            authority.authority.lifecycle !=
                MgaDmlUpdateStatementSavepointLifecycleV1::rolled_back ||
            authority.authority.publication_barrier_uuid !=
                identity.reserved_statement_barrier_uuid ||
            authority.authority.publication_barrier_generation !=
                identity.reserved_statement_barrier_generation ||
            !DmlUpdateDurableTypedUuid(savepoint_uuid,
                                       &chain.prior_savepoint_uuid)) {
          return fail("bound_aborted_savepoint_authority_invalid");
        }
        chain.prior_savepoint_generation = savepoint_generation;
      }
    }
    scratchbird::wire::TypedUpdateJournalRecord decoded;
    scratchbird::wire::TypedUpdateCarrierError error;
    if (!scratchbird::wire::DecodeAndValidateTypedUpdateJournalRecord(
            extents[index].exact_dujr_bytes, chain, &decoded, &error)) {
      return fail("durable_journal_canonical_invalid:" + error.field + ":" +
                  error.detail);
    }
    records->push_back(std::move(decoded));
  }
  return true;
}

bool DmlUpdateDurableTransactionState(
    const EngineRequestContext& context,
    const MgaDmlUpdateDurableOperationIdentityV1& identity,
    scratchbird::wire::TypedUpdateTransactionState* state,
    std::string* detail) {
  if (state == nullptr) return false;
  const auto inventory =
      LoadLocalTransactionInventoryFromDatabase(context.database_path);
  if (!inventory.ok()) {
    if (detail != nullptr) *detail = "transaction_inventory_load_failed";
    return false;
  }
  const auto lookup = LookupLocalTransaction(
      inventory.inventory,
      MakeLocalTransactionId(identity.owning_local_transaction_id));
  if (!lookup.ok() ||
      scratchbird::core::uuid::UuidToString(
          lookup.entry.identity.transaction_uuid.value) !=
          identity.owning_transaction_uuid) {
    if (detail != nullptr) *detail = "transaction_inventory_identity_missing";
    return false;
  }
  switch (lookup.entry.state) {
    case TransactionState::active:
    case TransactionState::read_only_active:
      *state = scratchbird::wire::TypedUpdateTransactionState::active_live;
      return true;
    case TransactionState::committed:
    case TransactionState::archived:
      *state = scratchbird::wire::TypedUpdateTransactionState::committed_final;
      return true;
    case TransactionState::rolled_back:
      *state =
          scratchbird::wire::TypedUpdateTransactionState::rolled_back_final;
      return true;
    case TransactionState::none:
    case TransactionState::created:
    case TransactionState::preparing:
    case TransactionState::prepared:
    case TransactionState::committing:
    case TransactionState::rolling_back:
    case TransactionState::limbo:
    case TransactionState::recovering:
    case TransactionState::failed_terminal:
      *state =
          scratchbird::wire::TypedUpdateTransactionState::dead_or_abandoned;
      return true;
  }
  if (detail != nullptr) *detail = "transaction_inventory_state_invalid";
  return false;
}

}  // namespace

struct MgaDmlUpdateDurablePreparedSuccessorV1::Impl {
  std::string path;
  std::unique_ptr<DmlUpdateDurableFileLock> lock;
  std::vector<std::uint8_t> encoded_frame;
  std::vector<std::uint8_t> encoded_invalidation_frame;
  EngineApiDiagnostic committed_diagnostic = OkDiagnostic();
  EngineApiDiagnostic cancelled_diagnostic = OkDiagnostic();
  EngineApiDiagnostic committed_ack_lost_diagnostic =
      DmlUpdateDurableMutation(
          MgaDmlUpdateDurableOperationOutcomeV1::storage_failure,
          "successor_committed_ack_lost").diagnostic;
  EngineApiDiagnostic commit_write_failed_diagnostic =
      DmlUpdateDurableMutation(
          MgaDmlUpdateDurableOperationOutcomeV1::storage_failure,
          "successor_durable_write_failed").diagnostic;
  bool valid = false;
};

MgaDmlUpdateDurablePreparedSuccessorV1::
    MgaDmlUpdateDurablePreparedSuccessorV1() = default;
MgaDmlUpdateDurablePreparedSuccessorV1::
    ~MgaDmlUpdateDurablePreparedSuccessorV1() = default;
MgaDmlUpdateDurablePreparedSuccessorV1::
    MgaDmlUpdateDurablePreparedSuccessorV1(
        MgaDmlUpdateDurablePreparedSuccessorV1&&) noexcept = default;
MgaDmlUpdateDurablePreparedSuccessorV1&
MgaDmlUpdateDurablePreparedSuccessorV1::operator=(
    MgaDmlUpdateDurablePreparedSuccessorV1&&) noexcept = default;
bool MgaDmlUpdateDurablePreparedSuccessorV1::valid() const {
  return impl_ != nullptr && impl_->valid && impl_->lock != nullptr &&
         impl_->lock->ok() && !impl_->encoded_frame.empty() &&
         !impl_->encoded_invalidation_frame.empty();
}

MgaDmlUpdateValidatedDurableAuthorityHandleV1::
    MgaDmlUpdateValidatedDurableAuthorityHandleV1() = default;
MgaDmlUpdateValidatedDurableAuthorityHandleV1::
    ~MgaDmlUpdateValidatedDurableAuthorityHandleV1() = default;
MgaDmlUpdateValidatedDurableAuthorityHandleV1::
    MgaDmlUpdateValidatedDurableAuthorityHandleV1(
        MgaDmlUpdateValidatedDurableAuthorityHandleV1&&) noexcept = default;
MgaDmlUpdateValidatedDurableAuthorityHandleV1&
MgaDmlUpdateValidatedDurableAuthorityHandleV1::operator=(
    MgaDmlUpdateValidatedDurableAuthorityHandleV1&&) noexcept = default;
bool MgaDmlUpdateValidatedDurableAuthorityHandleV1::valid() const {
  return impl_ != nullptr && !impl_->identity.validated_durable_handle_uuid.empty() &&
         impl_->identity.validated_durable_handle_generation != 0 &&
         !impl_->journal.empty() && impl_->exact_dumo.size() == 416;
}

MgaDmlUpdateDurableAuthorityReservationResultV1
ReserveMgaDmlUpdateDurableOperationAuthorityV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateDurableAuthorityReservationRequestV1& request) {
  MgaDmlUpdateDurableAuthorityReservationResultV1 result;
  MgaDmlUpdateDurableOperationIdentityV1 identity;
  identity.database_uuid = context.database_uuid.canonical;
  identity.owning_transaction_uuid = context.transaction_uuid.canonical;
  identity.owning_local_transaction_id = context.local_transaction_id;
  identity.authenticated_statement_receipt_uuid =
      context.statement_receipt_uuid.canonical;
  identity.operation_uuid = request.operation_uuid;
  identity.operation_generation = request.operation_generation;
  identity.descriptor_uuid = request.descriptor_uuid;
  identity.descriptor_generation = request.descriptor_generation;
  identity.recovery_token_uuid = request.recovery_token_uuid;
  identity.recovery_generation = request.recovery_generation;
  if (context.database_path.empty() ||
      !DmlUpdateDurableBaseIdentityValid(identity) ||
      identity.operation_generation == 0) {
    result.outcome = MgaDmlUpdateDurableOperationOutcomeV1::conflict;
    result.diagnostic = DmlUpdateDurableDiagnostic(
        "SBLR.OPERAND_INVALID",
        "sblr.dml_update_rows.durable_reservation_invalid");
    return result;
  }
  const std::string directory = DmlUpdateDurableOperationStorePath(context);
  if (!DmlUpdateDurableEnsureDirectory(directory)) {
    result.outcome = MgaDmlUpdateDurableOperationOutcomeV1::storage_failure;
    result.diagnostic = DmlUpdateDurableDiagnostic(
        "DML.UPDATE_FAILED",
        "sblr.dml_update_rows.durable_reservation_storage_failed");
    return result;
  }
  const std::string path = directory + "/" + identity.descriptor_uuid + ".duop";
  DmlUpdateDurableFileLock lock(path);
  if (!lock.ok()) {
    result.outcome = MgaDmlUpdateDurableOperationOutcomeV1::storage_failure;
    result.diagnostic = DmlUpdateDurableDiagnostic(
        "DML.UPDATE_FAILED",
        "sblr.dml_update_rows.durable_reservation_lock_failed");
    return result;
  }
  auto current = DmlUpdateDurableLoadOperation(path, true);
  if (current.ok) {
    if (!DmlUpdateDurableSameReservationRequest(context, request,
                                                 current.identity)) {
      result.outcome = MgaDmlUpdateDurableOperationOutcomeV1::access_denied;
      result.diagnostic = DmlUpdateDurableDiagnostic(
          "SECURITY.ACCESS_DENIED",
          "sblr.dml_update_rows.durable_reservation_denied");
      return result;
    }
    result.outcome = MgaDmlUpdateDurableOperationOutcomeV1::already_exact;
    result.diagnostic = OkDiagnostic();
    result.identity = current.identity;
    return result;
  }
  if (!current.missing) {
    result.outcome = current.quarantined
                         ? MgaDmlUpdateDurableOperationOutcomeV1::quarantined
                         : MgaDmlUpdateDurableOperationOutcomeV1::storage_failure;
    result.diagnostic = DmlUpdateDurableDiagnostic(
        "DML.UPDATE_FAILED",
        "sblr.dml_update_rows.durable_reservation_corrupt",
        current.detail);
    return result;
  }
  identity.validated_durable_handle_uuid =
      DmlUpdateDurableFreshIdentity(identity);
  identity.validated_durable_handle_generation = 1;
  identity.reserved_statement_barrier_uuid =
      DmlUpdateDurableFreshIdentity(
          identity, identity.validated_durable_handle_uuid);
  identity.reserved_statement_barrier_generation = 1;
  if (!DmlUpdateDurableIdentityValid(identity)) {
    result.outcome = MgaDmlUpdateDurableOperationOutcomeV1::storage_failure;
    result.diagnostic = DmlUpdateDurableDiagnostic(
        "DML.UPDATE_FAILED",
        "sblr.dml_update_rows.durable_reservation_identity_failed");
    return result;
  }
  DmlUpdateDurableFrameV1 frame;
  frame.kind = DmlUpdateDurableFrameKindV1::authority_reservation;
  frame.identity = identity;
  if (!DmlUpdateDurableAppendFrame(path, frame)) {
    result.outcome = MgaDmlUpdateDurableOperationOutcomeV1::storage_failure;
    result.diagnostic = DmlUpdateDurableDiagnostic(
        "DML.UPDATE_FAILED",
        "sblr.dml_update_rows.durable_reservation_write_failed");
    return result;
  }
  result.outcome = MgaDmlUpdateDurableOperationOutcomeV1::committed;
  result.diagnostic = OkDiagnostic();
  result.identity = std::move(identity);
  return result;
}

MgaDmlUpdateDurableOperationMutationResultV1
AbandonMgaDmlUpdateDurableOperationAuthorityReservationV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateDurableOperationIdentityV1& identity) {
  if (!DmlUpdateDurableIdentityMatchesContext(context, identity)) {
    return DmlUpdateDurableMutation(
        MgaDmlUpdateDurableOperationOutcomeV1::access_denied,
        "reservation_abandon_cross_authority");
  }
  const std::string path = DmlUpdateDurableDescriptorPath(context, identity);
  DmlUpdateDurableFileLock lock(path);
  if (!lock.ok()) {
    return DmlUpdateDurableMutation(
        MgaDmlUpdateDurableOperationOutcomeV1::storage_failure,
        "reservation_abandon_lock_failed");
  }
  const auto current = DmlUpdateDurableLoadOperation(path, true);
  if (current.missing) {
    return DmlUpdateDurableMutation(
        MgaDmlUpdateDurableOperationOutcomeV1::already_exact);
  }
  if (!current.ok || current.identity != identity) {
    return DmlUpdateDurableMutation(
        current.quarantined
            ? MgaDmlUpdateDurableOperationOutcomeV1::quarantined
            : MgaDmlUpdateDurableOperationOutcomeV1::stale,
        current.detail.empty() ? "reservation_abandon_identity_mismatch"
                               : current.detail);
  }
  if (!current.reservation_only || current.snapshot_present ||
      !current.journal.empty()) {
    return DmlUpdateDurableMutation(
        MgaDmlUpdateDurableOperationOutcomeV1::fork_or_terminal_conflict,
        "reservation_already_bound");
  }
  std::error_code error;
  const bool removed = std::filesystem::remove(path, error);
  if (error || !removed || !DmlUpdateDurableEnsureDirectory(
                              DmlUpdateDurableOperationStorePath(context))) {
    return DmlUpdateDurableMutation(
        MgaDmlUpdateDurableOperationOutcomeV1::storage_failure,
        "reservation_abandon_delete_failed");
  }
  return DmlUpdateDurableMutation(
      MgaDmlUpdateDurableOperationOutcomeV1::committed);
}

MgaDmlUpdateDurableOperationMutationResultV1
PublishMgaDmlUpdateDurableOperationBoundV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateDurablePublishBoundRequestV1& request) {
  if (!DmlUpdateDurableIdentityMatchesContext(context, request.identity)) {
    return DmlUpdateDurableMutation(
        MgaDmlUpdateDurableOperationOutcomeV1::access_denied,
        "publish_bound_cross_authority");
  }
  std::uint64_t structural_occurrence = 0;
  std::string detail;
  if (!DmlUpdateDurableSnapshotShallowValid(
          request.identity, request.authority_snapshot,
          &structural_occurrence, &detail) ||
      !DmlUpdateDurableJournalExtentMatchesBytes(
          request.identity, request.authority_snapshot,
          request.bound_journal, &detail) ||
      request.bound_journal.journal_sequence != 1 ||
      request.bound_journal.lifecycle_state !=
          MgaDmlUpdateDurableJournalStateV1::bound ||
      !DmlUpdateDurableZero(
          request.bound_journal.prior_record_sha256)) {
    return DmlUpdateDurableMutation(
        MgaDmlUpdateDurableOperationOutcomeV1::conflict,
        detail.empty() ? "publish_bound_shape_invalid" : detail);
  }
  const std::string path =
      DmlUpdateDurableDescriptorPath(context, request.identity);
  DmlUpdateDurableFileLock lock(path);
  if (!lock.ok()) {
    return DmlUpdateDurableMutation(
        MgaDmlUpdateDurableOperationOutcomeV1::storage_failure,
        "publish_bound_lock_failed");
  }
  auto current = DmlUpdateDurableLoadOperation(path, true);
  if (!current.ok || current.identity != request.identity) {
    return DmlUpdateDurableMutation(
        current.quarantined
            ? MgaDmlUpdateDurableOperationOutcomeV1::quarantined
            : current.missing
                  ? MgaDmlUpdateDurableOperationOutcomeV1::stale
                  : MgaDmlUpdateDurableOperationOutcomeV1::access_denied,
        current.detail.empty() ? "prebound_reservation_missing_or_mismatched"
                               : current.detail);
  }
  if (current.snapshot_present) {
    if (current.snapshot != request.authority_snapshot) {
      return DmlUpdateDurableMutation(
          MgaDmlUpdateDurableOperationOutcomeV1::conflict,
          "authority_snapshot_conflict");
    }
    if (!current.journal.empty()) {
      return DmlUpdateDurableMutation(
          current.journal.front() == request.bound_journal
              ? MgaDmlUpdateDurableOperationOutcomeV1::already_exact
              : MgaDmlUpdateDurableOperationOutcomeV1::conflict,
          current.journal.front() == request.bound_journal
              ? std::string{}
              : "bound_journal_conflict");
    }
  }

  if (DmlUpdateDurableFault(
          MgaDmlUpdateDurableFaultCutpointV1::before_snapshot_write)) {
    return DmlUpdateDurableMutation(
        MgaDmlUpdateDurableOperationOutcomeV1::storage_failure,
        "fault_before_snapshot_write");
  }
  DmlUpdateDurableFrameV1 reservation_frame;
  reservation_frame.kind =
      DmlUpdateDurableFrameKindV1::authority_reservation;
  reservation_frame.identity = request.identity;
  DmlUpdateDurableFrameV1 snapshot_frame;
  snapshot_frame.kind = DmlUpdateDurableFrameKindV1::authority_snapshot;
  snapshot_frame.identity = request.identity;
  std::vector<std::uint8_t> reservation_bytes;
  std::vector<std::uint8_t> snapshot_bytes;
  if (!DmlUpdateDurableEncodeSnapshot(request.authority_snapshot,
                                      &snapshot_frame.payload) ||
      !DmlUpdateDurableEncodeFrame(reservation_frame, &reservation_bytes) ||
      !DmlUpdateDurableEncodeFrame(snapshot_frame, &snapshot_bytes)) {
    return DmlUpdateDurableMutation(
        MgaDmlUpdateDurableOperationOutcomeV1::storage_failure,
        "authority_snapshot_encode_failed");
  }
  if (DmlUpdateDurableFault(
          MgaDmlUpdateDurableFaultCutpointV1::after_snapshot_write_before_bound)) {
    return DmlUpdateDurableMutation(
        MgaDmlUpdateDurableOperationOutcomeV1::storage_failure,
        "fault_after_snapshot_before_bound");
  }
  const auto bound_frame =
      DmlUpdateDurableJournalFrame(request.identity, request.bound_journal);
  std::vector<std::uint8_t> bound_bytes;
  if (!DmlUpdateDurableEncodeFrame(bound_frame, &bound_bytes)) {
    return DmlUpdateDurableMutation(
        MgaDmlUpdateDurableOperationOutcomeV1::storage_failure,
        "bound_journal_encode_failed");
  }
  std::vector<std::uint8_t> atomic_extent;
  try {
    atomic_extent.reserve(reservation_bytes.size() + snapshot_bytes.size() +
                          bound_bytes.size());
    atomic_extent.insert(atomic_extent.end(), reservation_bytes.begin(),
                         reservation_bytes.end());
    atomic_extent.insert(atomic_extent.end(), snapshot_bytes.begin(),
                         snapshot_bytes.end());
    atomic_extent.insert(atomic_extent.end(), bound_bytes.begin(),
                         bound_bytes.end());
  } catch (const std::bad_alloc&) {
    return DmlUpdateDurableMutation(
        MgaDmlUpdateDurableOperationOutcomeV1::storage_failure,
        "publish_bound_extent_allocation_failed");
  }
  if (!DmlUpdateDurableReplaceFileAtomically(path, atomic_extent)) {
    return DmlUpdateDurableMutation(
        MgaDmlUpdateDurableOperationOutcomeV1::storage_failure,
        "publish_bound_atomic_replace_failed");
  }
  return DmlUpdateDurableMutation(
      MgaDmlUpdateDurableOperationOutcomeV1::committed);
}

MgaDmlUpdateDurableOperationPrepareResultV1
PrepareMgaDmlUpdateDurableOperationSuccessorV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateDurableAppendSuccessorRequestV1& request) {
  g_dml_update_durable_prepare_calls.fetch_add(1,
                                                std::memory_order_relaxed);
  MgaDmlUpdateDurableOperationPrepareResultV1 result;
  const auto fail = [&](MgaDmlUpdateDurableOperationOutcomeV1 outcome,
                        std::string detail) {
    result.outcome = outcome;
    result.diagnostic = DmlUpdateDurableMutation(outcome,
                                                  std::move(detail)).diagnostic;
    return std::move(result);
  };
  if (!DmlUpdateDurableIdentityMatchesContext(context, request.identity)) {
    return fail(MgaDmlUpdateDurableOperationOutcomeV1::access_denied,
                "successor_cross_authority");
  }
  const std::string path =
      DmlUpdateDurableDescriptorPath(context, request.identity);
  auto lock = std::make_unique<DmlUpdateDurableFileLock>(path);
  if (!lock->ok()) {
    return fail(MgaDmlUpdateDurableOperationOutcomeV1::storage_failure,
                "successor_lock_failed");
  }
  const auto current = DmlUpdateDurableLoadOperation(path, true);
  if (!current.ok || current.identity != request.identity ||
      current.journal.empty()) {
    return fail(current.quarantined
                    ? MgaDmlUpdateDurableOperationOutcomeV1::quarantined
                    : MgaDmlUpdateDurableOperationOutcomeV1::stale,
                current.detail.empty() ? "durable_chain_unavailable"
                                       : current.detail);
  }
  std::string detail;
  if (!DmlUpdateDurableJournalExtentMatchesBytes(
          request.identity, current.snapshot, request.successor, &detail)) {
    return fail(MgaDmlUpdateDurableOperationOutcomeV1::conflict,
                std::move(detail));
  }
  if (current.staged_successor_present) {
    if (current.staged_successor == request.successor &&
        !current.staged_encoded_journal_frame.empty()) {
      std::vector<std::uint8_t> encoded_invalidation;
      if (!DmlUpdateDurableEncodePreparedInvalidation(
              request.identity, current.staged_successor,
              &encoded_invalidation)) {
        return fail(MgaDmlUpdateDurableOperationOutcomeV1::storage_failure,
                    "prepared_successor_invalidation_encode_failed");
      }
      auto impl =
          std::make_unique<MgaDmlUpdateDurablePreparedSuccessorV1::Impl>();
      impl->path = path;
      impl->lock = std::move(lock);
      impl->encoded_frame = current.staged_encoded_journal_frame;
      impl->encoded_invalidation_frame = std::move(encoded_invalidation);
      impl->valid = true;
      result.prepared.impl_ = std::move(impl);
      result.outcome = MgaDmlUpdateDurableOperationOutcomeV1::committed;
      result.diagnostic = OkDiagnostic();
      return result;
    }
    const auto& latest = current.journal.back();
    const bool cancelled_prepublication =
        current.staged_successor.lifecycle_state ==
            MgaDmlUpdateDurableJournalStateV1::published &&
        request.successor.lifecycle_state ==
            MgaDmlUpdateDurableJournalStateV1::aborted &&
        current.staged_successor.journal_sequence ==
            request.successor.journal_sequence &&
        current.staged_successor.prior_record_sha256 ==
            request.successor.prior_record_sha256 &&
        latest.journal_sequence == request.expected_prior_sequence &&
        latest.lifecycle_state == request.expected_prior_state &&
        latest.record_evidence_sha256 ==
            request.expected_prior_record_evidence_sha256;
    if (!cancelled_prepublication) {
      return fail(
          MgaDmlUpdateDurableOperationOutcomeV1::fork_or_terminal_conflict,
          "prepared_successor_conflict");
    }
    DmlUpdateDurableFrameV1 invalidation;
    invalidation.kind =
        DmlUpdateDurableFrameKindV1::prepared_successor_invalidated;
    invalidation.identity = request.identity;
    invalidation.sequence =
        current.staged_successor.journal_sequence;
    invalidation.state = static_cast<std::uint8_t>(
        current.staged_successor.lifecycle_state);
    invalidation.prior_record_sha256 =
        current.staged_successor.prior_record_sha256;
    invalidation.record_evidence_sha256 =
        current.staged_successor.record_evidence_sha256;
    if (!DmlUpdateDurableAppendFrame(path, invalidation)) {
      return fail(MgaDmlUpdateDurableOperationOutcomeV1::storage_failure,
                  "prepared_successor_invalidation_write_failed");
    }
  }
  const auto& latest = current.journal.back();
  if (latest == request.successor) {
    result.outcome = MgaDmlUpdateDurableOperationOutcomeV1::already_exact;
    result.diagnostic = OkDiagnostic();
    return result;
  }
  if (latest.lifecycle_state == MgaDmlUpdateDurableJournalStateV1::published ||
      latest.lifecycle_state == MgaDmlUpdateDurableJournalStateV1::aborted) {
    return fail(
        MgaDmlUpdateDurableOperationOutcomeV1::fork_or_terminal_conflict,
        "durable_chain_terminal");
  }
  if (latest.journal_sequence != request.expected_prior_sequence ||
      latest.lifecycle_state != request.expected_prior_state ||
      latest.record_evidence_sha256 !=
          request.expected_prior_record_evidence_sha256 ||
      request.successor.journal_sequence != latest.journal_sequence + 1 ||
      request.successor.prior_record_sha256 !=
          latest.record_evidence_sha256 ||
      !DmlUpdateDurableLegalTransition(latest.lifecycle_state,
                                       request.successor.lifecycle_state)) {
    return fail(MgaDmlUpdateDurableOperationOutcomeV1::fork_or_terminal_conflict,
                "successor_compare_and_append_conflict");
  }
  if (DmlUpdateDurableFault(
          MgaDmlUpdateDurableFaultCutpointV1::before_successor_write)) {
    return fail(MgaDmlUpdateDurableOperationOutcomeV1::storage_failure,
                "fault_before_successor_write");
  }
  DmlUpdateDurableFrameV1 frame =
      DmlUpdateDurableJournalFrame(request.identity, request.successor);
  std::vector<std::uint8_t> encoded;
  g_dml_update_durable_frame_encode_calls.fetch_add(
      1, std::memory_order_relaxed);
  g_dml_update_durable_checksum_calls.fetch_add(
      2, std::memory_order_relaxed);
  if (!DmlUpdateDurableEncodeFrame(frame, &encoded)) {
    return fail(MgaDmlUpdateDurableOperationOutcomeV1::storage_failure,
                "successor_prepare_encode_failed");
  }
  std::vector<std::uint8_t> encoded_invalidation;
  if (!DmlUpdateDurableEncodePreparedInvalidation(
          request.identity, request.successor, &encoded_invalidation)) {
    return fail(MgaDmlUpdateDurableOperationOutcomeV1::storage_failure,
                "successor_invalidation_prepare_encode_failed");
  }
  DmlUpdateDurableFrameV1 staged;
  staged.kind = DmlUpdateDurableFrameKindV1::prepared_successor;
  staged.identity = request.identity;
  staged.sequence = request.successor.journal_sequence;
  staged.state =
      static_cast<std::uint8_t>(request.successor.lifecycle_state);
  staged.prior_record_sha256 = request.successor.prior_record_sha256;
  staged.record_evidence_sha256 =
      request.successor.record_evidence_sha256;
  staged.payload = encoded;
  if (!DmlUpdateDurableAppendFrame(path, staged)) {
    return fail(MgaDmlUpdateDurableOperationOutcomeV1::storage_failure,
                "successor_prepare_stage_write_failed");
  }
  auto impl = std::make_unique<MgaDmlUpdateDurablePreparedSuccessorV1::Impl>();
  impl->path = path;
  impl->lock = std::move(lock);
  impl->encoded_frame = std::move(encoded);
  impl->encoded_invalidation_frame = std::move(encoded_invalidation);
  impl->valid = true;
  result.prepared.impl_ = std::move(impl);
  result.outcome = MgaDmlUpdateDurableOperationOutcomeV1::committed;
  result.diagnostic = OkDiagnostic();
  return result;
}

MgaDmlUpdateDurableOperationMutationResultV1
CommitPreparedMgaDmlUpdateDurableOperationSuccessorV1(
    MgaDmlUpdateDurablePreparedSuccessorV1&& prepared) {
  g_dml_update_durable_commit_calls.fetch_add(1,
                                               std::memory_order_relaxed);
  if (!prepared.valid()) {
    return DmlUpdateDurableMutation(
        MgaDmlUpdateDurableOperationOutcomeV1::conflict,
        "prepared_successor_invalid");
  }
  auto impl = std::move(prepared.impl_);
  impl->valid = false;
  const auto appended = DmlUpdateDurableAppendEncodedFrame(
      impl->path, impl->encoded_frame, true);
  if (appended == DmlUpdateDurableRawAppendResultV1::ok) {
    MgaDmlUpdateDurableOperationMutationResultV1 result;
    result.outcome = MgaDmlUpdateDurableOperationOutcomeV1::committed;
    result.diagnostic = std::move(impl->committed_diagnostic);
    return result;
  }
  if (appended ==
      DmlUpdateDurableRawAppendResultV1::after_fsync_ack_lost) {
    MgaDmlUpdateDurableOperationMutationResultV1 result;
    result.outcome = MgaDmlUpdateDurableOperationOutcomeV1::storage_failure;
    result.diagnostic =
        std::move(impl->committed_ack_lost_diagnostic);
    return result;
  }
  MgaDmlUpdateDurableOperationMutationResultV1 result;
  result.outcome = MgaDmlUpdateDurableOperationOutcomeV1::storage_failure;
  result.diagnostic = std::move(impl->commit_write_failed_diagnostic);
  return result;
}

MgaDmlUpdateDurableOperationMutationResultV1
CancelPreparedMgaDmlUpdateDurableOperationSuccessorV1(
    MgaDmlUpdateDurablePreparedSuccessorV1&& prepared) {
  if (!prepared.valid()) {
    return DmlUpdateDurableMutation(
        MgaDmlUpdateDurableOperationOutcomeV1::conflict,
        "prepared_successor_cancel_invalid");
  }
  auto impl = std::move(prepared.impl_);
  impl->valid = false;
  const auto appended = DmlUpdateDurableAppendEncodedFrame(
      impl->path, impl->encoded_invalidation_frame, false);
  if (appended == DmlUpdateDurableRawAppendResultV1::ok) {
    MgaDmlUpdateDurableOperationMutationResultV1 result;
    result.outcome = MgaDmlUpdateDurableOperationOutcomeV1::committed;
    result.diagnostic = std::move(impl->cancelled_diagnostic);
    return result;
  }
  return DmlUpdateDurableMutation(
      MgaDmlUpdateDurableOperationOutcomeV1::storage_failure,
      "prepared_successor_cancel_write_failed");
}

MgaDmlUpdateDurableOperationMutationResultV1
AppendMgaDmlUpdateDurableOperationSuccessorV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateDurableAppendSuccessorRequestV1& request) {
  auto prepared =
      PrepareMgaDmlUpdateDurableOperationSuccessorV1(context, request);
  if (!prepared.ok()) {
    MgaDmlUpdateDurableOperationMutationResultV1 result;
    result.outcome = prepared.outcome;
    result.diagnostic = std::move(prepared.diagnostic);
    return result;
  }
  return CommitPreparedMgaDmlUpdateDurableOperationSuccessorV1(
      std::move(prepared.prepared));
}

namespace {

MgaDmlUpdateDurableOperationRecoveryResultV1 DmlUpdateDurableRecoveryFailure(
    MgaDmlUpdateDurableOperationOutcomeV1 outcome, std::string detail,
    bool quarantined = false) {
  MgaDmlUpdateDurableOperationRecoveryResultV1 result;
  result.outcome = outcome;
  result.quarantined = quarantined;
  const bool denied =
      outcome == MgaDmlUpdateDurableOperationOutcomeV1::access_denied;
  const bool stale = outcome == MgaDmlUpdateDurableOperationOutcomeV1::stale;
  result.diagnostic = DmlUpdateDurableDiagnostic(
      denied ? "SECURITY.ACCESS_DENIED"
             : stale ? "MGA.TRANSACTION.STALE" : "DML.UPDATE_FAILED",
      denied ? "sblr.dml_update_rows.durable_recovery_denied"
             : stale ? "sblr.dml_update_rows.durable_recovery_stale"
                     : "sblr.dml_update_rows.durable_recovery_failed",
      std::move(detail));
  return result;
}

bool DmlUpdateDurableBuildSavepointObservation(
    const EngineRequestContext& context,
    const DmlUpdateDurableStoredOperationV1& current,
    const scratchbird::wire::TypedUpdateJournalRecord& journal_head,
    scratchbird::wire::TypedUpdateMgaRecoveryObservation* observation,
    std::string* detail) {
  const auto fail = [&](std::string reason) {
    if (detail != nullptr) *detail = std::move(reason);
    return false;
  };
  if (observation == nullptr) return fail("recovery_observation_required");

  const auto journal_state = journal_head.lifecycle_state;
  std::string required_savepoint_uuid;
  const bool journal_savepoint_present =
      !DmlUpdateDurableTypedUuidText(
           journal_head.statement_savepoint_uuid).empty();
  if (journal_state != scratchbird::wire::TypedUpdateJournalState::bound) {
    required_savepoint_uuid = DmlUpdateDurableTypedUuidText(
        journal_head.statement_savepoint_uuid);
    const bool nil_aborted =
        journal_state == scratchbird::wire::TypedUpdateJournalState::aborted &&
        !journal_savepoint_present &&
        journal_head.statement_savepoint_generation == 0;
    if (!nil_aborted &&
        (required_savepoint_uuid.empty() ||
         journal_head.statement_savepoint_generation == 0)) {
      return fail("journal_savepoint_identity_invalid");
    }
  }
  DmlUpdateDurableSavepointLookupV1 savepoint;
  if (journal_state == scratchbird::wire::TypedUpdateJournalState::bound ||
      journal_savepoint_present) {
    savepoint = DmlUpdateDurableFindStatementSavepoint(
        context, current.identity, required_savepoint_uuid);
  }
  if (savepoint.state == DmlUpdateDurableSavepointLookupStateV1::corrupt) {
    return fail(savepoint.detail.empty() ? "savepoint_authority_corrupt"
                                        : savepoint.detail);
  }
  if (savepoint.state == DmlUpdateDurableSavepointLookupStateV1::present &&
      (savepoint.authority.publication_barrier_uuid !=
           current.identity.reserved_statement_barrier_uuid ||
       savepoint.authority.publication_barrier_generation !=
           current.identity.reserved_statement_barrier_generation)) {
    return fail("savepoint_reserved_barrier_mismatch");
  }

  // A crash between opening the provider savepoint and appending intent leaves
  // a bound DUJR with an active private savepoint.  DUMO forbids representing
  // that contradictory cutpoint, so MGA rolls it back before observing the
  // bound head and records only the resulting durable no-effect proof.
  if (journal_state == scratchbird::wire::TypedUpdateJournalState::bound &&
      savepoint.state == DmlUpdateDurableSavepointLookupStateV1::present &&
      savepoint.authority.lifecycle ==
          MgaDmlUpdateStatementSavepointLifecycleV1::active) {
    const auto rolled_back = RollbackMgaDmlUpdateStatementSavepointAuthorityV1(
        context, savepoint.authority);
    if (!rolled_back.ok) {
      return fail("bound_orphan_savepoint_rollback_failed:" +
                  rolled_back.diagnostic.detail);
    }
    savepoint.authority = rolled_back.authority;
  }

  observation->statement_barrier_present = false;
  observation->no_surviving_effect_proven = false;
  observation->statement_savepoint_uuid = {};
  observation->statement_savepoint_generation = 0;
  if (savepoint.state == DmlUpdateDurableSavepointLookupStateV1::absent) {
    if (journal_state != scratchbird::wire::TypedUpdateJournalState::bound &&
        !(journal_state ==
              scratchbird::wire::TypedUpdateJournalState::aborted &&
          required_savepoint_uuid.empty())) {
      return fail("journal_savepoint_authority_missing");
    }
    observation->savepoint_state =
        scratchbird::wire::TypedUpdateSavepointState::absent;
    observation->no_surviving_effect_proven = true;
  } else {
    if (!DmlUpdateDurableTypedUuid(
            savepoint.authority.savepoint_uuid,
            &observation->statement_savepoint_uuid)) {
      return fail("savepoint_uuid_invalid");
    }
    observation->statement_savepoint_generation =
        savepoint.authority.savepoint_generation;
    switch (savepoint.authority.lifecycle) {
      case MgaDmlUpdateStatementSavepointLifecycleV1::active:
        observation->savepoint_state =
            scratchbird::wire::TypedUpdateSavepointState::active;
        break;
      case MgaDmlUpdateStatementSavepointLifecycleV1::rolled_back:
        observation->savepoint_state =
            scratchbird::wire::TypedUpdateSavepointState::rolled_back_final;
        observation->no_surviving_effect_proven = true;
        break;
      case MgaDmlUpdateStatementSavepointLifecycleV1::released:
        observation->savepoint_state =
            scratchbird::wire::TypedUpdateSavepointState::
                released_at_statement_barrier;
        observation->statement_barrier_present = true;
        break;
    }
  }

  if (!DmlUpdateDurableTypedUuid(
          current.identity.reserved_statement_barrier_uuid,
          &observation->reserved_statement_barrier_uuid)) {
    return fail("reserved_statement_barrier_invalid");
  }
  observation->reserved_statement_barrier_generation =
      current.identity.reserved_statement_barrier_generation;
  return true;
}

bool DmlUpdateDurableDecodeRecoveryAuthority(
    const DmlUpdateDurableStoredOperationV1& current,
    std::span<const scratchbird::wire::TypedUpdateJournalRecord>
        journal_records,
    const scratchbird::wire::TypedUpdateMgaRecoveryObservation& observation,
    std::string* detail) {
  const auto fail = [&](const scratchbird::wire::TypedUpdateCarrierError& error,
                        std::string prefix) {
    if (detail != nullptr) {
      *detail = std::move(prefix) + ":" + error.field + ":" + error.detail;
    }
    return false;
  };
  if (journal_records.empty()) {
    if (detail != nullptr) *detail = "journal_chain_required";
    return false;
  }
  scratchbird::wire::TypedUpdateDescriptorCarrier descriptor;
  scratchbird::wire::TypedUpdateRowPolicyVector row_policies;
  scratchbird::wire::TypedUpdateRecoveryTokenCarrier recovery_token;
  scratchbird::wire::TypedUpdateSecurityPolicySourceVector source_policies;
  scratchbird::wire::TypedUpdateSecuritySnapshotProof security_proof;
  scratchbird::wire::TypedUpdateCarrierError error;
  if (!scratchbird::wire::DecodeAndValidateTypedUpdateDescriptor(
          current.snapshot.descriptor_dudc, &descriptor, &error)) {
    return fail(error, "DUDC");
  }
  if (!scratchbird::wire::DecodeAndValidateTypedUpdateRowPolicyVector(
          current.snapshot.row_policy_vector_dupv, &row_policies, &error)) {
    return fail(error, "DUPV");
  }
  if (!scratchbird::wire::DecodeAndValidateTypedUpdateRecoveryToken(
          current.snapshot.recovery_token_durc, &recovery_token, &error)) {
    return fail(error, "DURC");
  }
  if (!scratchbird::wire::DecodeAndValidateTypedUpdateSecurityPolicySourceVector(
          current.snapshot.source_policy_vector_dusv, &source_policies,
          &error)) {
    return fail(error, "DUSV");
  }
  if (!scratchbird::wire::DecodeAndValidateTypedUpdateSecuritySnapshotProof(
          current.snapshot.security_snapshot_proof_dusp, &security_proof,
          &error)) {
    return fail(error, "DUSP");
  }
  if (!scratchbird::wire::ValidateTypedUpdateSecurityRecoveryAuthority(
          descriptor, row_policies, recovery_token, source_policies,
          security_proof, journal_records.back(), observation, &error)) {
    return fail(error, "security_recovery_authority");
  }
  return true;
}

}  // namespace

MgaDmlUpdateDurableOperationRecoveryResultV1
RecoverMgaDmlUpdateDurableOperationChainV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateDurableOperationLookupV1& lookup) {
  g_dml_update_durable_recovery_calls.fetch_add(1,
                                                 std::memory_order_relaxed);
  const std::string path = DmlUpdateDurablePathForLookup(context, lookup);
  if (path.empty()) {
    return DmlUpdateDurableRecoveryFailure(
        MgaDmlUpdateDurableOperationOutcomeV1::access_denied,
        "authenticated_descriptor_lookup_invalid");
  }
  DmlUpdateDurableFileLock lock(path);
  if (!lock.ok()) {
    return DmlUpdateDurableRecoveryFailure(
        MgaDmlUpdateDurableOperationOutcomeV1::storage_failure,
        "durable_recovery_lock_failed");
  }
  auto current = DmlUpdateDurableLoadOperation(path, true);
  if (!current.ok || current.reservation_only || !current.snapshot_present ||
      current.journal.empty()) {
    const bool denied = current.missing || current.reservation_only;
    return DmlUpdateDurableRecoveryFailure(
        current.quarantined
            ? MgaDmlUpdateDurableOperationOutcomeV1::quarantined
            : denied ? MgaDmlUpdateDurableOperationOutcomeV1::access_denied
                     : MgaDmlUpdateDurableOperationOutcomeV1::storage_failure,
        current.detail.empty() ? "durable_chain_unavailable" : current.detail,
        current.quarantined);
  }
  if (!DmlUpdateDurableIdentityMatchesContext(context, current.identity)) {
    return DmlUpdateDurableRecoveryFailure(
        MgaDmlUpdateDurableOperationOutcomeV1::access_denied,
        "durable_chain_cross_authority");
  }
  if (current.identity.descriptor_generation != lookup.descriptor_generation ||
      current.structural_occurrence_id != lookup.structural_occurrence_id) {
    return DmlUpdateDurableRecoveryFailure(
        MgaDmlUpdateDurableOperationOutcomeV1::stale,
        "descriptor_generation_or_occurrence_stale");
  }

  std::vector<scratchbird::wire::TypedUpdateJournalRecord> journal_records;
  std::string detail;
  if (!DmlUpdateDurableDecodeTypedJournalChain(
          context, current.identity, current.snapshot, current.journal,
          &journal_records, &detail)) {
    (void)DmlUpdateDurableWriteQuarantine(path);
    return DmlUpdateDurableRecoveryFailure(
        MgaDmlUpdateDurableOperationOutcomeV1::quarantined,
        std::move(detail), true);
  }

  scratchbird::wire::TypedUpdateDescriptorCarrier descriptor;
  scratchbird::wire::TypedUpdateSecuritySnapshotProof security_proof;
  scratchbird::wire::TypedUpdateCarrierError carrier_error;
  if (!scratchbird::wire::DecodeAndValidateTypedUpdateDescriptor(
          current.snapshot.descriptor_dudc, &descriptor, &carrier_error) ||
      !scratchbird::wire::DecodeAndValidateTypedUpdateSecuritySnapshotProof(
          current.snapshot.security_snapshot_proof_dusp, &security_proof,
          &carrier_error)) {
    (void)DmlUpdateDurableWriteQuarantine(path);
    return DmlUpdateDurableRecoveryFailure(
        MgaDmlUpdateDurableOperationOutcomeV1::quarantined,
        "recovery_snapshot_decode_failed:" + carrier_error.field + ":" +
            carrier_error.detail,
        true);
  }

  scratchbird::wire::TypedUpdateMgaRecoveryObservation observation;
  if (!DmlUpdateDurableTypedUuid(current.identity.validated_durable_handle_uuid,
                                  &observation.validated_mga_durable_handle_uuid) ||
      !DmlUpdateDurableTypedUuid(current.identity.database_uuid,
                                  &observation.database_uuid) ||
      !DmlUpdateDurableTypedUuid(current.identity.descriptor_uuid,
                                  &observation.descriptor_uuid) ||
      !DmlUpdateDurableTypedUuid(current.identity.operation_uuid,
                                  &observation.operation_uuid) ||
      !DmlUpdateDurableTypedUuid(
          current.identity.authenticated_statement_receipt_uuid,
          &observation.authenticated_statement_receipt_uuid) ||
      !DmlUpdateDurableTypedUuid(current.identity.owning_transaction_uuid,
                                  &observation.owning_transaction_uuid) ||
      !DmlUpdateDurableTypedUuid(current.identity.recovery_token_uuid,
                                  &observation.recovery_token_uuid)) {
    (void)DmlUpdateDurableWriteQuarantine(path);
    return DmlUpdateDurableRecoveryFailure(
        MgaDmlUpdateDurableOperationOutcomeV1::quarantined,
        "durable_identity_uuid_decode_failed", true);
  }
  observation.validated_mga_durable_handle_generation =
      current.identity.validated_durable_handle_generation;
  observation.descriptor_generation = current.identity.descriptor_generation;
  observation.operation_generation = current.identity.operation_generation;
  observation.owning_local_transaction_id =
      current.identity.owning_local_transaction_id;
  observation.recovery_generation = current.identity.recovery_generation;
  observation.latest_journal_state = journal_records.back().lifecycle_state;
  observation.durable_chain_head_sequence =
      journal_records.back().journal_sequence;
  observation.durable_chain_head_record_evidence_sha256 =
      journal_records.back().record_evidence_sha256;
  observation.catalog_snapshot_uuid = descriptor.catalog_snapshot_uuid;
  observation.catalog_generation = descriptor.catalog_generation;
  observation.security_snapshot_uuid = security_proof.security_snapshot_uuid;
  observation.security_snapshot_generation =
      security_proof.security_snapshot_generation;
  observation.security_epoch = security_proof.security_epoch;
  if (!DmlUpdateDurableTransactionState(context, current.identity,
                                        &observation.transaction_state,
                                        &detail) ||
      !DmlUpdateDurableBuildSavepointObservation(
          context, current, journal_records.back(), &observation, &detail)) {
    (void)DmlUpdateDurableWriteQuarantine(path);
    return DmlUpdateDurableRecoveryFailure(
        MgaDmlUpdateDurableOperationOutcomeV1::quarantined,
        std::move(detail), true);
  }

  scratchbird::wire::TypedUpdateMgaRecoveryObservation prior_observation;
  bool prior_present = false;
  if (!current.latest_dumo.empty()) {
    if (!scratchbird::wire::DecodeAndValidateTypedUpdateMgaRecoveryObservation(
            current.latest_dumo, &prior_observation, &carrier_error)) {
      (void)DmlUpdateDurableWriteQuarantine(path);
      return DmlUpdateDurableRecoveryFailure(
          MgaDmlUpdateDurableOperationOutcomeV1::quarantined,
          "stored_DUMO_invalid:" + carrier_error.field + ":" +
              carrier_error.detail,
          true);
    }
    observation.observation_uuid = prior_observation.observation_uuid;
    observation.observation_generation =
        prior_observation.observation_generation;
    prior_present = true;
  } else {
    const std::string fresh = DmlUpdateDurableFreshIdentity(
        current.identity, current.identity.reserved_statement_barrier_uuid);
    if (!DmlUpdateDurableTypedUuid(fresh, &observation.observation_uuid)) {
      return DmlUpdateDurableRecoveryFailure(
          MgaDmlUpdateDurableOperationOutcomeV1::storage_failure,
          "observation_identity_issue_failed");
    }
    observation.observation_generation = 1;
  }

  std::vector<std::uint8_t> exact_dumo;
  g_dml_update_durable_observation_encode_calls.fetch_add(
      1, std::memory_order_relaxed);
  if (!scratchbird::wire::EncodeTypedUpdateMgaRecoveryObservation(
          observation, &exact_dumo, &carrier_error)) {
    (void)DmlUpdateDurableWriteQuarantine(path);
    return DmlUpdateDurableRecoveryFailure(
        MgaDmlUpdateDurableOperationOutcomeV1::quarantined,
        "DUMO_encode_failed:" + carrier_error.field + ":" +
            carrier_error.detail,
        true);
  }
  if (prior_present && exact_dumo != current.latest_dumo) {
    if (observation.observation_generation ==
        std::numeric_limits<std::uint64_t>::max()) {
      (void)DmlUpdateDurableWriteQuarantine(path);
      return DmlUpdateDurableRecoveryFailure(
          MgaDmlUpdateDurableOperationOutcomeV1::quarantined,
          "observation_generation_exhausted", true);
    }
    ++observation.observation_generation;
    g_dml_update_durable_observation_encode_calls.fetch_add(
        1, std::memory_order_relaxed);
    if (!scratchbird::wire::EncodeTypedUpdateMgaRecoveryObservation(
            observation, &exact_dumo, &carrier_error)) {
      (void)DmlUpdateDurableWriteQuarantine(path);
      return DmlUpdateDurableRecoveryFailure(
          MgaDmlUpdateDurableOperationOutcomeV1::quarantined,
          "DUMO_successor_encode_failed:" + carrier_error.field + ":" +
              carrier_error.detail,
          true);
    }
  }
  observation.exact_bytes = exact_dumo;
  if (!DmlUpdateDurableDecodeRecoveryAuthority(
          current, journal_records, observation, &detail)) {
    (void)DmlUpdateDurableWriteQuarantine(path);
    return DmlUpdateDurableRecoveryFailure(
        MgaDmlUpdateDurableOperationOutcomeV1::quarantined,
        std::move(detail), true);
  }

  if (exact_dumo != current.latest_dumo) {
    if (DmlUpdateDurableFault(
            MgaDmlUpdateDurableFaultCutpointV1::before_observation_write)) {
      return DmlUpdateDurableRecoveryFailure(
          MgaDmlUpdateDurableOperationOutcomeV1::storage_failure,
          "fault_before_observation_write");
    }
    DmlUpdateDurableFrameV1 frame;
    frame.kind = DmlUpdateDurableFrameKindV1::recovery_observation;
    frame.identity = current.identity;
    frame.sequence = observation.durable_chain_head_sequence;
    frame.state = static_cast<std::uint8_t>(observation.latest_journal_state);
    frame.record_evidence_sha256 =
        observation.observation_evidence_sha256;
    frame.payload = exact_dumo;
    if (!DmlUpdateDurableAppendFrame(path, frame)) {
      return DmlUpdateDurableRecoveryFailure(
          MgaDmlUpdateDurableOperationOutcomeV1::storage_failure,
          "recovery_observation_write_failed");
    }
    if (DmlUpdateDurableFault(
            MgaDmlUpdateDurableFaultCutpointV1::
                after_observation_write_before_ack)) {
      return DmlUpdateDurableRecoveryFailure(
          MgaDmlUpdateDurableOperationOutcomeV1::storage_failure,
          "recovery_observation_committed_ack_lost");
    }
    current.latest_dumo = exact_dumo;
  }

  auto impl =
      std::make_unique<MgaDmlUpdateValidatedDurableAuthorityHandleV1::Impl>();
  impl->identity = current.identity;
  impl->snapshot = current.snapshot;
  impl->journal = current.journal;
  impl->staged_successor_present = current.staged_successor_present;
  impl->staged_successor = current.staged_successor;
  impl->staged_encoded_journal_frame =
      current.staged_encoded_journal_frame;
  std::error_code extent_error;
  impl->authenticated_store_extent_bytes =
      std::filesystem::file_size(path, extent_error);
  if (extent_error || impl->authenticated_store_extent_bytes == 0) {
    return DmlUpdateDurableRecoveryFailure(
        MgaDmlUpdateDurableOperationOutcomeV1::storage_failure,
        "durable_store_extent_observation_failed");
  }
  impl->exact_dumo = exact_dumo;
  MgaDmlUpdateDurableOperationRecoveryResultV1 result;
  result.outcome = MgaDmlUpdateDurableOperationOutcomeV1::committed;
  result.diagnostic = OkDiagnostic();
  result.identity = current.identity;
  result.authority_snapshot = current.snapshot;
  result.journal = current.journal;
  result.staged_successor_present = current.staged_successor_present;
  result.staged_successor = current.staged_successor;
  result.recovery_observation_dumo = exact_dumo;
  result.validated_handle.impl_ = std::move(impl);
  return result;
}

MgaDmlUpdateDurableOperationMutationResultV1
RollbackMgaDmlUpdateStatementFromValidatedDurableAuthorityV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateValidatedDurableAuthorityHandleV1& validated_handle) {
  if (!validated_handle.valid() ||
      !DmlUpdateDurableIdentityMatchesContext(
          context, validated_handle.impl_->identity)) {
    return DmlUpdateDurableMutation(
        MgaDmlUpdateDurableOperationOutcomeV1::access_denied,
        "validated_durable_handle_cross_authority");
  }
  scratchbird::wire::TypedUpdateMgaRecoveryObservation observation;
  scratchbird::wire::TypedUpdateCarrierError error;
  if (!scratchbird::wire::DecodeAndValidateTypedUpdateMgaRecoveryObservation(
          validated_handle.impl_->exact_dumo, &observation, &error)) {
    return DmlUpdateDurableMutation(
        MgaDmlUpdateDurableOperationOutcomeV1::quarantined,
        "validated_durable_handle_DUMO_invalid:" + error.field + ":" +
            error.detail);
  }
  if (observation.savepoint_state ==
          scratchbird::wire::TypedUpdateSavepointState::absent ||
      observation.savepoint_state ==
          scratchbird::wire::TypedUpdateSavepointState::rolled_back_final) {
    return DmlUpdateDurableMutation(
        MgaDmlUpdateDurableOperationOutcomeV1::already_exact);
  }
  if (observation.savepoint_state !=
          scratchbird::wire::TypedUpdateSavepointState::active ||
      observation.statement_barrier_present) {
    return DmlUpdateDurableMutation(
        MgaDmlUpdateDurableOperationOutcomeV1::fork_or_terminal_conflict,
        "postbarrier_savepoint_cannot_rollback");
  }
  const std::string savepoint_uuid = DmlUpdateDurableTypedUuidText(
      observation.statement_savepoint_uuid);
  const auto binding = DmlUpdateDurableStatementBinding(
      validated_handle.impl_->identity);
  auto authority = RecoverMgaDmlUpdateStatementSavepointAuthorityV1(
      context, binding, savepoint_uuid,
      observation.statement_savepoint_generation);
  if (!authority.ok || authority.authority.lifecycle !=
                           MgaDmlUpdateStatementSavepointLifecycleV1::active ||
      authority.authority.publication_barrier_uuid !=
          validated_handle.impl_->identity.reserved_statement_barrier_uuid ||
      authority.authority.publication_barrier_generation !=
          validated_handle.impl_->identity
              .reserved_statement_barrier_generation) {
    return DmlUpdateDurableMutation(
        MgaDmlUpdateDurableOperationOutcomeV1::stale,
        "validated_savepoint_authority_not_current");
  }
  authority = RollbackMgaDmlUpdateStatementSavepointAuthorityV1(
      context, authority.authority);
  if (!authority.ok || authority.authority.lifecycle !=
                           MgaDmlUpdateStatementSavepointLifecycleV1::rolled_back ||
      authority.authority.publication_barrier_present) {
    return DmlUpdateDurableMutation(
        MgaDmlUpdateDurableOperationOutcomeV1::storage_failure,
        "validated_savepoint_rollback_failed");
  }
  return DmlUpdateDurableMutation(
      MgaDmlUpdateDurableOperationOutcomeV1::committed);
}

MgaDmlUpdateDurableOperationMutationResultV1
CommitRecoveredMgaDmlUpdateDurableOperationStagedSuccessorV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateValidatedDurableAuthorityHandleV1& validated_handle) {
  if (!validated_handle.valid() ||
      !DmlUpdateDurableIdentityMatchesContext(
          context, validated_handle.impl_->identity)) {
    return DmlUpdateDurableMutation(
        MgaDmlUpdateDurableOperationOutcomeV1::access_denied,
        "recovered_staged_successor_cross_authority");
  }
  const auto& impl = *validated_handle.impl_;
  if (!impl.staged_successor_present ||
      impl.staged_successor.lifecycle_state !=
          MgaDmlUpdateDurableJournalStateV1::published ||
      impl.staged_encoded_journal_frame.empty() || impl.journal.empty() ||
      impl.staged_successor.journal_sequence !=
          impl.journal.back().journal_sequence + 1 ||
      impl.staged_successor.prior_record_sha256 !=
          impl.journal.back().record_evidence_sha256 ||
      impl.authenticated_store_extent_bytes == 0) {
    return DmlUpdateDurableMutation(
        MgaDmlUpdateDurableOperationOutcomeV1::fork_or_terminal_conflict,
        "recovered_staged_published_successor_unavailable");
  }
  const std::string path =
      DmlUpdateDurableDescriptorPath(context, impl.identity);
  DmlUpdateDurableFileLock lock(path);
  if (!lock.ok()) {
    return DmlUpdateDurableMutation(
        MgaDmlUpdateDurableOperationOutcomeV1::storage_failure,
        "recovered_staged_successor_lock_failed");
  }
  std::error_code extent_error;
  const auto current_extent = std::filesystem::file_size(path, extent_error);
  if (extent_error) {
    return DmlUpdateDurableMutation(
        MgaDmlUpdateDurableOperationOutcomeV1::storage_failure,
        "recovered_staged_successor_extent_failed");
  }
  if (current_extent == impl.authenticated_store_extent_bytes +
                            impl.staged_encoded_journal_frame.size()) {
    return DmlUpdateDurableMutation(
        MgaDmlUpdateDurableOperationOutcomeV1::already_exact);
  }
  if (current_extent != impl.authenticated_store_extent_bytes) {
    return DmlUpdateDurableMutation(
        MgaDmlUpdateDurableOperationOutcomeV1::stale,
        "recovered_staged_successor_chain_changed");
  }
  const auto appended = DmlUpdateDurableAppendEncodedFrame(
      path, impl.staged_encoded_journal_frame, true);
  if (appended == DmlUpdateDurableRawAppendResultV1::ok) {
    return DmlUpdateDurableMutation(
        MgaDmlUpdateDurableOperationOutcomeV1::committed);
  }
  return DmlUpdateDurableMutation(
      MgaDmlUpdateDurableOperationOutcomeV1::storage_failure,
      appended == DmlUpdateDurableRawAppendResultV1::after_fsync_ack_lost
          ? "recovered_staged_successor_committed_ack_lost"
          : "recovered_staged_successor_write_failed");
}

MgaDmlUpdateDurableOperationInstrumentationV1
ReadMgaDmlUpdateDurableOperationInstrumentationV1() {
  MgaDmlUpdateDurableOperationInstrumentationV1 result;
  result.prepare_calls =
      g_dml_update_durable_prepare_calls.load(std::memory_order_acquire);
  result.frame_encode_calls =
      g_dml_update_durable_frame_encode_calls.load(std::memory_order_acquire);
  result.checksum_calls =
      g_dml_update_durable_checksum_calls.load(std::memory_order_acquire);
  result.commit_calls =
      g_dml_update_durable_commit_calls.load(std::memory_order_acquire);
  result.commit_write_calls =
      g_dml_update_durable_commit_write_calls.load(std::memory_order_acquire);
  result.commit_fsync_calls =
      g_dml_update_durable_commit_fsync_calls.load(std::memory_order_acquire);
  result.recovery_calls =
      g_dml_update_durable_recovery_calls.load(std::memory_order_acquire);
  result.observation_encode_calls =
      g_dml_update_durable_observation_encode_calls.load(
          std::memory_order_acquire);
  return result;
}

void ResetMgaDmlUpdateDurableOperationInstrumentationForTestingV1() {
  g_dml_update_durable_prepare_calls.store(0, std::memory_order_release);
  g_dml_update_durable_frame_encode_calls.store(0,
                                                 std::memory_order_release);
  g_dml_update_durable_checksum_calls.store(0, std::memory_order_release);
  g_dml_update_durable_commit_calls.store(0, std::memory_order_release);
  g_dml_update_durable_commit_write_calls.store(0,
                                                 std::memory_order_release);
  g_dml_update_durable_commit_fsync_calls.store(0,
                                                 std::memory_order_release);
  g_dml_update_durable_recovery_calls.store(0, std::memory_order_release);
  g_dml_update_durable_observation_encode_calls.store(
      0, std::memory_order_release);
  g_dml_update_durable_fault_cutpoint.store(
      MgaDmlUpdateDurableFaultCutpointV1::none,
      std::memory_order_release);
}

void SetMgaDmlUpdateDurableFaultCutpointForTestingV1(
    MgaDmlUpdateDurableFaultCutpointV1 cutpoint) {
  g_dml_update_durable_fault_cutpoint.store(cutpoint,
                                             std::memory_order_release);
}

MgaDmlUpdateDurableInspectionV1 InspectMgaDmlUpdateDurableOperationForTestingV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateDurableOperationLookupV1& lookup) {
  MgaDmlUpdateDurableInspectionV1 result;
  const std::string path = DmlUpdateDurablePathForLookup(context, lookup);
  if (path.empty()) {
    result.outcome = MgaDmlUpdateDurableOperationOutcomeV1::access_denied;
    result.diagnostic = DmlUpdateDurableDiagnostic(
        "SECURITY.ACCESS_DENIED",
        "sblr.dml_update_rows.durable_inspection_denied");
    return result;
  }
  DmlUpdateDurableFileLock lock(path);
  if (!lock.ok()) {
    result.outcome = MgaDmlUpdateDurableOperationOutcomeV1::storage_failure;
    result.diagnostic = DmlUpdateDurableDiagnostic(
        "DML.UPDATE_FAILED",
        "sblr.dml_update_rows.durable_inspection_failed",
        "lock_failed");
    return result;
  }
  const auto current = DmlUpdateDurableLoadOperation(path, true);
  result.quarantined = current.quarantined;
  if (!current.ok || current.reservation_only || !current.snapshot_present ||
      current.journal.empty() ||
      !DmlUpdateDurableIdentityMatchesContext(context, current.identity) ||
      current.identity.descriptor_generation != lookup.descriptor_generation ||
      current.structural_occurrence_id != lookup.structural_occurrence_id) {
    result.outcome = current.quarantined
                         ? MgaDmlUpdateDurableOperationOutcomeV1::quarantined
                         : current.missing
                               ? MgaDmlUpdateDurableOperationOutcomeV1::access_denied
                               : MgaDmlUpdateDurableOperationOutcomeV1::stale;
    result.diagnostic = DmlUpdateDurableDiagnostic(
        current.missing ? "SECURITY.ACCESS_DENIED"
                        : current.quarantined ? "DML.UPDATE_FAILED"
                                              : "MGA.TRANSACTION.STALE",
        "sblr.dml_update_rows.durable_inspection_refused",
        current.detail);
    return result;
  }
  result.outcome = MgaDmlUpdateDurableOperationOutcomeV1::committed;
  result.diagnostic = OkDiagnostic();
  result.authority_snapshot = current.snapshot;
  result.journal = current.journal;
  result.exact_dumo_bytes = current.latest_dumo;
  return result;
}

namespace {

bool DmlUpdateDurableAuthenticateTestingLookup(
    const EngineRequestContext& context,
    const MgaDmlUpdateDurableOperationLookupV1& lookup,
    std::string* path) {
  if (path == nullptr) return false;
  *path = DmlUpdateDurablePathForLookup(context, lookup);
  if (path->empty()) return false;
  const auto current = DmlUpdateDurableLoadOperation(*path, false);
  return current.ok && !current.reservation_only &&
         current.snapshot_present && !current.journal.empty() &&
         DmlUpdateDurableIdentityMatchesContext(context, current.identity) &&
         current.identity.descriptor_generation == lookup.descriptor_generation &&
         current.structural_occurrence_id == lookup.structural_occurrence_id;
}

bool DmlUpdateDurableFsyncParent(const EngineRequestContext& context) {
  return DmlUpdateDurableEnsureDirectory(
      DmlUpdateDurableOperationStorePath(context));
}

}  // namespace

bool CorruptMgaDmlUpdateDurableExtentByteForTestingV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateDurableOperationLookupV1& lookup,
    std::uint64_t exact_file_offset, std::uint8_t xor_mask) {
  if (xor_mask == 0) return false;
  std::string path;
  if (!DmlUpdateDurableAuthenticateTestingLookup(context, lookup, &path)) {
    return false;
  }
  DmlUpdateDurableFileLock lock(path);
  if (!lock.ok()) return false;
#if defined(_WIN32)
  HANDLE handle = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER position{};
  position.QuadPart = static_cast<LONGLONG>(exact_file_offset);
  bool ok = SetFilePointerEx(handle, position, nullptr, FILE_BEGIN) != 0;
  std::uint8_t value = 0;
  DWORD count = 0;
  ok = ok && ReadFile(handle, &value, 1, &count, nullptr) != 0 && count == 1;
  value ^= xor_mask;
  ok = ok && SetFilePointerEx(handle, position, nullptr, FILE_BEGIN) != 0 &&
       WriteFile(handle, &value, 1, &count, nullptr) != 0 && count == 1 &&
       FlushFileBuffers(handle) != 0;
  CloseHandle(handle);
  return ok;
#else
  const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
  if (fd < 0) return false;
  std::uint8_t value = 0;
  const ssize_t read = ::pread(fd, &value, 1,
                               static_cast<off_t>(exact_file_offset));
  value ^= xor_mask;
  const bool ok = read == 1 &&
                  ::pwrite(fd, &value, 1,
                           static_cast<off_t>(exact_file_offset)) == 1 &&
                  ::fsync(fd) == 0;
  ::close(fd);
  return ok;
#endif
}

bool TruncateMgaDmlUpdateDurableExtentForTestingV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateDurableOperationLookupV1& lookup,
    std::uint64_t exact_file_bytes) {
  std::string path;
  if (!DmlUpdateDurableAuthenticateTestingLookup(context, lookup, &path)) {
    return false;
  }
  DmlUpdateDurableFileLock lock(path);
  if (!lock.ok()) return false;
  std::error_code error;
  const auto current = std::filesystem::file_size(path, error);
  if (error || exact_file_bytes >= current) return false;
  std::filesystem::resize_file(path, exact_file_bytes, error);
  return !error && DmlUpdateDurableFsyncParent(context);
}

bool QuarantineMgaDmlUpdateDurableOperationForTestingV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateDurableOperationLookupV1& lookup) {
  std::string path;
  if (!DmlUpdateDurableAuthenticateTestingLookup(context, lookup, &path)) {
    return false;
  }
  DmlUpdateDurableFileLock lock(path);
  return lock.ok() && DmlUpdateDurableWriteQuarantine(path) &&
         DmlUpdateDurableFsyncParent(context);
}

bool DeleteMgaDmlUpdateDurableOperationForTestingV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateDurableOperationLookupV1& lookup) {
  std::string path;
  if (!DmlUpdateDurableAuthenticateTestingLookup(context, lookup, &path)) {
    return false;
  }
  DmlUpdateDurableFileLock lock(path);
  if (!lock.ok()) return false;
  std::error_code error;
  const bool removed = std::filesystem::remove(path, error);
  if (error || !removed) return false;
  std::filesystem::remove(DmlUpdateDurableQuarantinePath(path), error);
  return !error && DmlUpdateDurableFsyncParent(context);
}

MgaDmlUpdateStatementSavepointAuthorityResultV1
CreateMgaDmlUpdateStatementSavepointAuthorityV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateStatementSavepointBindingV1& binding) {
  const std::string reserved_barrier =
      DmlUpdateStatementFreshDistinctUuid(binding);
  if (reserved_barrier.empty()) {
    return DmlUpdateStatementSavepointFailure(
        "DML.UPDATE_FAILED",
        "sblr.dml_update_rows.statement_savepoint_create_failed",
        "engine_barrier_identity_issue_failed");
  }
  return CreateMgaDmlUpdateStatementSavepointAuthorityWithReservedBarrierV1(
      context, binding, reserved_barrier, 1);
}

MgaDmlUpdateStatementSavepointAuthorityResultV1
CreateMgaDmlUpdateStatementSavepointAuthorityWithReservedBarrierV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateStatementSavepointBindingV1& binding,
    const std::string& reserved_publication_barrier_uuid,
    std::uint64_t reserved_publication_barrier_generation) {
  std::lock_guard<std::mutex> guard(
      DmlUpdateStatementSavepointJournalMutex());
  if (!DmlUpdateStatementBindingMatchesContext(context, binding) ||
      !DmlUpdateStatementParseUuid(reserved_publication_barrier_uuid) ||
      reserved_publication_barrier_generation != 1 ||
      reserved_publication_barrier_uuid == binding.database_uuid ||
      reserved_publication_barrier_uuid == binding.owning_transaction_uuid ||
      reserved_publication_barrier_uuid ==
          binding.authenticated_statement_receipt_uuid ||
      reserved_publication_barrier_uuid == binding.operation_uuid ||
      reserved_publication_barrier_uuid == binding.descriptor_uuid ||
      reserved_publication_barrier_uuid == binding.recovery_token_uuid) {
    return DmlUpdateStatementSavepointFailure(
        "MGA.TRANSACTION.STALE",
        "sblr.dml_update_rows.statement_savepoint_stale",
        "create_binding_or_reserved_barrier_not_current");
  }
  DmlUpdateStatementSavepointJournalRecordV1 record;
  record.authority.binding = binding;
  record.authority.savepoint_uuid =
      DmlUpdateStatementFreshDistinctUuid(
          binding, reserved_publication_barrier_uuid);
  record.authority.savepoint_generation = 1;
  record.authority.publication_barrier_uuid =
      reserved_publication_barrier_uuid;
  record.authority.publication_barrier_generation =
      reserved_publication_barrier_generation;
  record.authority.publication_barrier_present = false;
  record.authority.lifecycle =
      MgaDmlUpdateStatementSavepointLifecycleV1::active;
  record.private_marker = DmlUpdateStatementPrivateSavepointMarker(
      record.authority.savepoint_uuid);
  record.journal_sequence = 1;
  record.cutoffs.row_event_sequence = NextRowEventSequence(context) - 1;
  record.cutoffs.metadata_event_sequence =
      NextMetadataEventSequence(context) - 1;
  record.cutoffs.index_event_sequence = NextIndexEventSequence(context) - 1;
  record.authority.durable_presence_sha256 =
      DmlUpdateStatementSavepointRecordSha256(record);
  if (record.authority.savepoint_uuid.empty() ||
      record.authority.publication_barrier_uuid.empty() ||
      record.private_marker.empty() ||
      !DmlUpdateStatementShaNonzero(
          record.authority.durable_presence_sha256)) {
    return DmlUpdateStatementSavepointFailure(
        "DML.UPDATE_FAILED",
        "sblr.dml_update_rows.statement_savepoint_create_failed",
        "engine_savepoint_identity_issue_failed");
  }
  if (!DmlUpdateStatementAppendBinaryRecord(context, record)) {
    return DmlUpdateStatementSavepointFailure(
        "DML.UPDATE_FAILED",
        "sblr.dml_update_rows.statement_savepoint_create_failed",
        "savepoint_create_durable_append_failed");
  }
  return DmlUpdateStatementLoadSavepointAuthority(
      context, binding, record.authority.savepoint_uuid,
      record.authority.savepoint_generation);
}

MgaDmlUpdateStatementSavepointAuthorityResultV1
RecoverMgaDmlUpdateStatementSavepointAuthorityV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateStatementSavepointBindingV1& binding,
    const std::string& savepoint_uuid,
    std::uint64_t savepoint_generation) {
  std::lock_guard<std::mutex> guard(
      DmlUpdateStatementSavepointJournalMutex());
  return DmlUpdateStatementLoadSavepointAuthority(
      context, binding, savepoint_uuid, savepoint_generation);
}

MgaDmlUpdateStatementSavepointAuthorityResultV1
RevalidateMgaDmlUpdateStatementSavepointAuthorityV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateStatementSavepointAuthorityV1& admitted) {
  std::lock_guard<std::mutex> guard(
      DmlUpdateStatementSavepointJournalMutex());
  auto current = DmlUpdateStatementLoadSavepointAuthority(
      context, admitted.binding, admitted.savepoint_uuid,
      admitted.savepoint_generation);
  if (!current.ok) return current;
  if (!DmlUpdateStatementAuthorityExact(current.authority, admitted)) {
    return DmlUpdateStatementSavepointFailure(
        "MGA.TRANSACTION.STALE",
        "sblr.dml_update_rows.statement_savepoint_stale",
        "admitted_savepoint_snapshot_not_current");
  }
  return current;
}

MgaDmlUpdateStatementSavepointAuthorityResultV1
RollbackMgaDmlUpdateStatementSavepointAuthorityV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateStatementSavepointAuthorityV1& admitted) {
  std::lock_guard<std::mutex> guard(
      DmlUpdateStatementSavepointJournalMutex());
  auto current = DmlUpdateStatementLoadSavepointAuthority(
      context, admitted.binding, admitted.savepoint_uuid,
      admitted.savepoint_generation);
  if (!current.ok) return current;
  if (!DmlUpdateStatementAuthorityExact(current.authority, admitted) ||
      current.authority.lifecycle !=
          MgaDmlUpdateStatementSavepointLifecycleV1::active) {
    return DmlUpdateStatementSavepointFailure(
        "MGA.TRANSACTION.STALE",
        "sblr.dml_update_rows.statement_savepoint_stale",
        "rollback_requires_current_active_savepoint");
  }
  const auto parsed = ParseSavepoints(context);
  const auto tx = parsed.active_savepoints.find(
      admitted.binding.owning_local_transaction_id);
  const std::string marker =
      DmlUpdateStatementPrivateSavepointMarker(admitted.savepoint_uuid);
  if (tx == parsed.active_savepoints.end() ||
      tx->second.find(marker) == tx->second.end()) {
    return DmlUpdateStatementSavepointFailure(
        "MGA.TRANSACTION.ROLLBACK_FAILED",
        "sblr.dml_update_rows.statement_savepoint_rollback_failed",
        "active_savepoint_marker_missing");
  }
  DmlUpdateStatementSavepointJournalRecordV1 record;
  record.authority = admitted;
  record.authority.lifecycle =
      MgaDmlUpdateStatementSavepointLifecycleV1::rolled_back;
  record.authority.publication_barrier_present = false;
  record.private_marker = marker;
  record.journal_sequence = 2;
  record.cutoffs = tx->second.at(marker);
  record.row_upper_event_sequence = NextRowEventSequence(context) - 1;
  record.metadata_upper_event_sequence =
      NextMetadataEventSequence(context) - 1;
  record.index_upper_event_sequence = NextIndexEventSequence(context) - 1;
  record.prior_record_sha256 = admitted.durable_presence_sha256;
  record.authority.durable_presence_sha256 =
      DmlUpdateStatementSavepointRecordSha256(record);
  if (!DmlUpdateStatementShaNonzero(
          record.authority.durable_presence_sha256) ||
      !DmlUpdateStatementAppendBinaryRecord(context, record)) {
    return DmlUpdateStatementSavepointFailure(
        "MGA.TRANSACTION.ROLLBACK_FAILED",
        "sblr.dml_update_rows.statement_savepoint_rollback_failed",
        "savepoint_rollback_durable_append_failed");
  }
  return DmlUpdateStatementLoadSavepointAuthority(
      context, admitted.binding, admitted.savepoint_uuid,
      admitted.savepoint_generation);
}

MgaDmlUpdateStatementSavepointAuthorityResultV1
ReleaseMgaDmlUpdateStatementSavepointAuthorityV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateStatementSavepointAuthorityV1& admitted) {
  std::lock_guard<std::mutex> guard(
      DmlUpdateStatementSavepointJournalMutex());
  auto current = DmlUpdateStatementLoadSavepointAuthority(
      context, admitted.binding, admitted.savepoint_uuid,
      admitted.savepoint_generation);
  if (!current.ok) return current;
  if (!DmlUpdateStatementAuthorityExact(current.authority, admitted) ||
      current.authority.lifecycle !=
          MgaDmlUpdateStatementSavepointLifecycleV1::active) {
    return DmlUpdateStatementSavepointFailure(
        "MGA.TRANSACTION.STALE",
        "sblr.dml_update_rows.statement_savepoint_stale",
        "release_requires_current_active_savepoint");
  }
  const auto parsed = ParseSavepoints(context);
  const auto tx = parsed.active_savepoints.find(
      admitted.binding.owning_local_transaction_id);
  const std::string marker =
      DmlUpdateStatementPrivateSavepointMarker(admitted.savepoint_uuid);
  if (tx == parsed.active_savepoints.end() ||
      tx->second.find(marker) == tx->second.end()) {
    return DmlUpdateStatementSavepointFailure(
        "DML.UPDATE_FAILED",
        "sblr.dml_update_rows.statement_savepoint_release_failed",
        "active_savepoint_marker_missing");
  }
  DmlUpdateStatementSavepointJournalRecordV1 record;
  record.authority = admitted;
  record.authority.lifecycle =
      MgaDmlUpdateStatementSavepointLifecycleV1::released;
  record.authority.publication_barrier_present = true;
  record.private_marker = marker;
  record.journal_sequence = 2;
  record.cutoffs = tx->second.at(marker);
  record.prior_record_sha256 = admitted.durable_presence_sha256;
  record.authority.durable_presence_sha256 =
      DmlUpdateStatementSavepointRecordSha256(record);
  // The publication barrier is the durable release append below.  Build the
  // complete success value before crossing it: after the append/fsync returns
  // success this function may only move already prepared state to its caller.
  // In particular, do not reload, decode, hash, or allocate from the durable
  // journal after publication.
  MgaDmlUpdateStatementSavepointAuthorityResultV1 success;
  success.ok = true;
  success.diagnostic = OkDiagnostic();
  success.authority = record.authority;
  if (record.authority.publication_barrier_uuid.empty() ||
      record.authority.publication_barrier_generation != 1 ||
      !DmlUpdateStatementShaNonzero(
          record.authority.durable_presence_sha256) ||
      !DmlUpdateStatementAppendBinaryRecord(context, record)) {
    return DmlUpdateStatementSavepointFailure(
        "DML.UPDATE_FAILED",
        "sblr.dml_update_rows.statement_savepoint_release_failed",
        "publication_barrier_durable_append_failed");
  }
  return success;
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
  RelationReadSnapshot state = BuildCrudCompatibilityStateFromMga(loaded.state);
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

}  // namespace scratchbird::engine::internal_api
