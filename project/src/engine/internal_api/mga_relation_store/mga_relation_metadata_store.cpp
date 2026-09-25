// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "mga_relation_store/mga_relation_metadata_store.hpp"
#include "mga_relation_store/mga_metadata_record_codec.hpp"
#include "mga_metadata_migration_fingerprint.hpp"
#include "mga_relation_store/mga_contextual_text_descriptor.hpp"
#include "mga_relation_store/mga_relation_store_internal_support.hpp"
#include "mga_relation_store/mga_row_codec.hpp"
#include "mga_relation_store/mga_savepoint_store.hpp"
#include "mga_relation_store/mga_descriptor_record_codec.hpp"

#include "api_diagnostics.hpp"
#include "crud_support/crud_store.hpp"
#include "hash_digest.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api {
namespace {
using namespace metadata_migration;

// SEARCH_KEY: SB_ENGINE_MGA_RELATION_METADATA_STORE_IMPLEMENTATION_AUTHORITY
// Owns persisted relation-metadata and descriptor-field decoding, immutable
// generation caches, and raw metadata snapshots. Savepoint exclusion is
// supplied by the canonical relation/MGA authority and is never inferred here.

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
}
struct MetadataStoreFileIdentity {
  bool ok = false;
  std::uintmax_t file_size = 0;
  std::int64_t file_mtime_ticks = 0;
};

std::string MetadataStorePath(const EngineRequestContext& context) {
  return context.database_path + ".sb.mga_relation_metadata";
}

std::string DescriptorStorePath(const EngineRequestContext& context) {
  return context.database_path + ".sb.mga_relation_descriptors";
}

std::string SavepointStorePath(const EngineRequestContext& context) {
  return context.database_path + ".sb.mga_savepoints";
}

EngineApiDiagnostic OkDiagnostic() {
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

std::vector<std::string> SplitTabs(const std::string& record) {
  std::vector<std::string> fields;
  DecodeMgaMetadataFields(record, &fields);
  return fields;
}
MetadataStoreFileIdentity MetadataStoreTextFileIdentity(const std::string& path);

struct MetadataReadResult {
  bool ok = false;
  MetadataStoreFileIdentity identity;
  std::vector<std::string> lines;
  std::vector<scratchbird::core::index::byte> binary_bytes;
  scratchbird::core::hash::Digest256 content_sha256{};
};

MetadataReadResult ReadContent(const std::string& path, bool binary_records) {
  MetadataReadResult result;
  const auto before = MetadataStoreTextFileIdentity(path);
  std::vector<scratchbird::core::index::byte> bytes;
  if (!ReadCompleteMgaBinaryFile(path,&bytes)) return result;
  // Hash the exact same admitted bytes that supply the decoded records.
  // A size/mtime pair is an observation fence, never cache content authority.
  if (!binary_records && !bytes.empty() && bytes.back()!='\n') return result;
  const auto digest=scratchbird::core::hash::ComputeSha256Digest(bytes);
  if (!digest.ok() || digest.digest_bytes!=scratchbird::core::hash::kSha256DigestBytes) return result;
  result.content_sha256=digest.digest;
  if (binary_records) {
    result.binary_bytes = std::move(bytes);
  } else {
    auto begin=bytes.begin();
    while(begin!=bytes.end()) {
      const auto end=std::find(begin,bytes.end(),'\n');
      result.lines.emplace_back(begin,end);
      begin=end+1; // The final-delimiter check above proves this stays in range.
    }
  }
  const auto after = MetadataStoreTextFileIdentity(path);
  if (before.ok!=after.ok || before.file_size != after.file_size ||
      before.file_mtime_ticks != after.file_mtime_ticks) return {};
  result.identity = after;
  result.ok = true;
  return result;
}

MetadataReadResult ReadMetadataRecords(const std::string& path) {
  auto result = ReadContent(path, true);
  if (!result.ok || !DecodeMgaMetadataStream(result.binary_bytes, &result.lines)) {
    result.ok = false;
    result.lines.clear();
  }
  return result;
}

MetadataReadResult ReadLines(const std::string& path) {
  return ReadContent(path, false);
}

MetadataReadResult ReadSavepointBytes(const std::string& path) {
  return ReadContent(path, true);
}

std::uint64_t ParseU64(const std::string& text,
                       const std::uint64_t fallback = 0) {
  if (text.empty()) return fallback;
  try {
    return static_cast<std::uint64_t>(std::stoull(text));
  } catch (...) {
    return fallback;
  }
}

MetadataStoreFileIdentity MetadataStoreTextFileIdentity(
    const std::string& path) {
  MetadataStoreFileIdentity identity;
  std::error_code error;
  if (!std::filesystem::exists(path, error)) return identity;
  identity.file_size = std::filesystem::file_size(path, error);
  if (error) return {};
  const auto write_time = std::filesystem::last_write_time(path, error);
  if (error) return {};
  identity.file_mtime_ticks =
      static_cast<std::int64_t>(write_time.time_since_epoch().count());
  identity.ok = true;
  return identity;
}

auto MetadataSavepointCacheDigest(const MetadataReadResult& records,
                                  const SavepointParsedState& savepoints) {
  // Cache identity, not a durable format. Include the exact admitted marker
  // bytes and the effective rollback ranges from all owned savepoint journals.
  std::vector<scratchbird::core::index::byte> bytes(
      records.content_sha256.begin(), records.content_sha256.end());
  const auto append = [&](std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8)
      bytes.push_back(static_cast<std::uint8_t>(value >> shift));
  };
  append(savepoints.rollback_ranges.size());
  for (const auto& [tx, ranges] : savepoints.rollback_ranges) {
    append(tx);
    append(ranges.size());
    for (const auto& range : ranges) {
      append(range.cutoffs.row_event_sequence);
      append(range.cutoffs.metadata_event_sequence);
      append(range.cutoffs.index_event_sequence);
      append(range.row_upper_event_sequence);
      append(range.metadata_upper_event_sequence);
      append(range.index_upper_event_sequence);
    }
  }
  return scratchbird::core::hash::ComputeSha256Digest(bytes);
}

}  // namespace

bool ReadCompleteMgaMetadataRecords(const std::string& path, std::vector<std::string>* records) {
  if (!records) return false;
  const auto read = ReadMetadataRecords(path);
  if (!read.ok) { records->clear(); return false; }
  *records = read.lines;
  return true;
}

bool ReadCompleteMgaTextRecords(const std::string& path,
                               std::vector<std::string>* records) {
  if (records == nullptr) return false;
  records->clear();
  auto read = ReadLines(path);
  if (!read.ok) return false;
  *records = std::move(read.lines);
  return true;
}

bool ValidConstraintBatchUuid(const EngineUuid& value, core::platform::UuidKind kind) {
  return core::uuid::MakeDurableEngineIdentityUuid(kind, value).ok();
}
bool ValidConstraintBatchUuid(std::string_view bytes, core::platform::UuidKind kind) {
  EngineUuid value;
  return ReadMetadataUuid(bytes, &value) && ValidConstraintBatchUuid(value, kind);
}

namespace constraint_batch_field {
inline constexpr std::size_t kMagic = 0;
inline constexpr std::size_t kRecordKind = 1;
inline constexpr std::size_t kCreatorTx = 2;
inline constexpr std::size_t kEventSequence = 3;
inline constexpr std::size_t kFormatVersion = 4;
inline constexpr std::size_t kBatchUuid = 5;
inline constexpr std::size_t kSealState = 6;
inline constexpr std::size_t kBatchHash = 7;
inline constexpr std::size_t kMutationCount = 8;
inline constexpr std::size_t kDatabaseUuid = 9;
inline constexpr std::size_t kConstraintUuid = 10;
inline constexpr std::size_t kOwnerTableUuid = 11;
inline constexpr std::size_t kChildSchemaUuid = 12;
inline constexpr std::size_t kChildDescriptorUuid = 13;
inline constexpr std::size_t kChildDescriptorGeneration = 14;
inline constexpr std::size_t kChildColumnUuid = 15;
inline constexpr std::size_t kParentTableUuid = 16;
inline constexpr std::size_t kParentSchemaUuid = 17;
inline constexpr std::size_t kParentDescriptorUuid = 18;
inline constexpr std::size_t kParentDescriptorGeneration = 19;
inline constexpr std::size_t kParentColumnUuid = 20;
inline constexpr std::size_t kParentCandidateConstraintUuid = 21;
inline constexpr std::size_t kReferencedKeyDescriptorUuid = 22;
inline constexpr std::size_t kSupportUuid = 23;
inline constexpr std::size_t kSupportFamily = 24;
inline constexpr std::size_t kSupportPolicy = 25;
inline constexpr std::size_t kMatchPolicy = 26;
inline constexpr std::size_t kOnUpdate = 27;
inline constexpr std::size_t kOnDelete = 28;
inline constexpr std::size_t kEnforcementTiming = 29;
inline constexpr std::size_t kConstraintMetadataGeneration = 30;
inline constexpr std::size_t kBaseTableEventSequence = 31;
inline constexpr std::size_t kParentBaseTableEventSequence = 32;
inline constexpr std::size_t kConstraintName = 33;
inline constexpr std::size_t kConstraintKind = 34;
inline constexpr std::size_t kCanonicalEnvelope = 35;
inline constexpr std::size_t kTableUuid = 36;
inline constexpr std::size_t kTableDefaultName = 37;
inline constexpr std::size_t kTableColumns = 38;
inline constexpr std::size_t kTableTemporary = 39;
inline constexpr std::size_t kTableTemporaryScope = 40;
inline constexpr std::size_t kTableTemporarySessionUuid = 41;
inline constexpr std::size_t kTableOnCommitAction = 42;
inline constexpr std::size_t kFieldCount = 43;
}  // namespace constraint_batch_field

std::size_t ConstraintMutationBatchFieldCount() {
  return constraint_batch_field::kFieldCount;
}

std::vector<std::string> ConstraintMutationBatchLineFields(
    const MgaConstraintMutationBatch& batch,
    std::uint64_t creator_tx,
    std::uint64_t event_sequence) {
  const CrudTableRecord& table = batch.updated_table;
  std::vector<std::string> fields{
      kRowStoreMagic,
      "CONSTRAINT_MUTATION_BATCH",
      std::to_string(creator_tx),
      std::to_string(event_sequence),
      batch.format_version,
      MetadataUuidBytes(batch.batch_uuid),
      "sealed",
      batch.batch_hash,
      std::to_string(batch.mutation_count),
      MetadataUuidBytes(batch.database_uuid),
      MetadataUuidBytes(batch.constraint_uuid),
      MetadataUuidBytes(batch.owner_table_uuid),
      MetadataUuidBytes(batch.child_schema_uuid),
      MetadataUuidBytes(batch.child_relation_descriptor_uuid),
      std::to_string(batch.child_relation_descriptor_generation),
      MetadataUuidBytes(batch.child_column_uuid),
      MetadataUuidBytes(batch.parent_table_uuid),
      MetadataUuidBytes(batch.parent_schema_uuid),
      MetadataUuidBytes(batch.parent_relation_descriptor_uuid),
      std::to_string(batch.parent_relation_descriptor_generation),
      MetadataUuidBytes(batch.parent_column_uuid),
      MetadataUuidBytes(batch.parent_candidate_key_constraint_uuid),
      MetadataUuidBytes(batch.key_descriptor_uuid),
      MetadataUuidBytes(batch.support_uuid),
      batch.support_family,
      batch.support_policy,
      batch.match_policy,
      batch.on_update_action,
      batch.on_delete_action,
      batch.enforcement_timing,
      std::to_string(batch.constraint_metadata_generation),
      std::to_string(batch.base_table_event_sequence),
      std::to_string(batch.parent_base_table_event_sequence),
      std::string(batch.constraint_name),
      batch.constraint_kind,
      std::string(batch.canonical_constraint_envelope),
      MetadataUuidBytes(table.table_uuid),
      std::string(table.default_name),
      EncodeMetadataPairs(table.columns),
      table.temporary ? "1" : "0",
      table.temporary_scope,
      MetadataUuidBytes(table.temporary_session_uuid),
      table.on_commit_action};
  return fields;
}

struct DescriptorFieldsCacheRecord {
  std::uintmax_t file_size = 0;
  std::int64_t file_mtime_ticks = 0;
  scratchbird::core::hash::Digest256 content_sha256{};
  std::shared_ptr<const DescriptorFieldsByRelation> descriptors;
};

std::mutex& DescriptorFieldsCacheMutex() {
  static std::mutex mutex;
  return mutex;
}

std::map<std::string, DescriptorFieldsCacheRecord>& DescriptorFieldsCache() {
  static std::map<std::string, DescriptorFieldsCacheRecord> cache;
  return cache;
}

std::mutex& MgaMetadataCacheMutex() {
  static std::mutex mutex;
  return mutex;
}

std::map<MgaMetadataCacheKey,
         std::shared_ptr<const MgaMetadataCacheEntry>>&
MgaMetadataCache() {
  static std::map<MgaMetadataCacheKey,
                  std::shared_ptr<const MgaMetadataCacheEntry>> cache;
  return cache;
}

static std::shared_ptr<const DescriptorFieldsByRelation>
LoadAdmittedDescriptorFieldsSnapshot(
    const std::string& path,
    const MetadataReadResult& lines,
    const EngineUuid& required_relation_uuid) {
  if (!lines.ok) return nullptr;
  const auto identity = lines.identity;
  const std::uintmax_t file_size = identity.ok ? identity.file_size : 0;
  const std::int64_t file_mtime_ticks =
      identity.ok ? identity.file_mtime_ticks : 0;
  {
    const std::lock_guard<std::mutex> guard(DescriptorFieldsCacheMutex());
    const auto cached = DescriptorFieldsCache().find(path);
    if (cached != DescriptorFieldsCache().end() &&
        cached->second.file_size == file_size &&
        cached->second.file_mtime_ticks == file_mtime_ticks &&
        cached->second.content_sha256 == lines.content_sha256 &&
        cached->second.descriptors != nullptr &&
        (required_relation_uuid.is_nil() ||
         cached->second.descriptors->contains(required_relation_uuid))) {
      return cached->second.descriptors;
    }
  }
  // Exact relation authority must not be refused solely by a negative cache
  // entry.  The descriptor store is append-published and can be populated by
  // another engine facade linked into the same server process; those facades
  // do not share this translation unit's in-memory cache.  When the caller
  // names an exact required relation and the matching cache entry omits it,
  // re-read the durable store even if its coarse file identity is unchanged.
  // A genuine durable miss remains fail-closed in the caller.
  DescriptorFieldsByRelation descriptors;
  if (!DecodeMgaDescriptorRecords(lines.binary_bytes, &descriptors)) return nullptr;
  {
    const std::lock_guard<std::mutex> guard(DescriptorFieldsCacheMutex());
    auto immutable =
        std::make_shared<const DescriptorFieldsByRelation>(std::move(descriptors));
    DescriptorFieldsCache()[path] = {file_size, file_mtime_ticks, lines.content_sha256, immutable};
    return immutable;
  }
}

std::shared_ptr<const DescriptorFieldsByRelation>
LoadDescriptorFieldsSnapshot(
    const EngineRequestContext& context,
    const EngineUuid& required_relation_uuid) {
  const std::string path = DescriptorStorePath(context);
  return LoadAdmittedDescriptorFieldsSnapshot(path, ReadContent(path, true), required_relation_uuid);
}

DescriptorFieldsByRelation LoadDescriptorFieldsByRelation(
    const EngineRequestContext& context,
    const EngineUuid& required_relation_uuid) {
  const auto snapshot =
      LoadDescriptorFieldsSnapshot(context, required_relation_uuid);
  return snapshot == nullptr ? DescriptorFieldsByRelation{} : *snapshot;
}

EngineApiDiagnostic PersistDescriptorFields(const EngineRequestContext& context,
                                            const EngineUuid& relation_uuid,
                                            const std::vector<std::pair<std::string, std::string>>& fields) {
  if (context.database_path.empty()) {
    return MakeInvalidRequestDiagnostic("mga.relation_descriptor", "database_path_required");
  }
  std::string record;
  if (!AppendMgaDescriptorRecord(relation_uuid, fields, &record)) {
    return MakeInvalidRequestDiagnostic("mga.relation_descriptor", "invalid_descriptor_record");
  }
  const std::string path = DescriptorStorePath(context);
  std::ofstream output(path, std::ios::app | std::ios::binary);
  output.write(record.data(), static_cast<std::streamsize>(record.size()));
  output.flush();
  if (!output) {
    return MakeInvalidRequestDiagnostic("mga.relation_descriptor", "descriptor_store_append_failed");
  }
  {
    const std::lock_guard<std::mutex> guard(DescriptorFieldsCacheMutex());
    // An append does not prove all older bytes still match the cached image.
    // Existing statement holders retain their immutable snapshots.
    DescriptorFieldsCache().erase(path);
  }
  return OkDiagnostic();
}


EngineApiDiagnostic LoadMgaMetadata(RelationReadSnapshot* state,
                                    const EngineRequestContext& context) {
  if (state == nullptr) {
    return MakeInvalidRequestDiagnostic("mga.relation_metadata", "state_required");
  }
  const std::string metadata_path = MetadataStorePath(context);
  const std::string savepoint_path = SavepointStorePath(context);
  const std::string descriptor_path = DescriptorStorePath(context);
  const auto metadata_lines = ReadMetadataRecords(metadata_path);
  const auto savepoint_lines = ReadSavepointBytes(savepoint_path);
  const auto descriptor_lines = ReadContent(descriptor_path, true);
  if (!metadata_lines.ok || !savepoint_lines.ok || !descriptor_lines.ok) {
    return MakeInvalidRequestDiagnostic("mga.relation_metadata",
        !metadata_lines.ok ? "metadata_store_read_failed" :
        !savepoint_lines.ok ? "savepoint_store_read_failed" : "descriptor_store_read_failed");
  }
  const auto metadata_identity = metadata_lines.identity;
  const auto savepoint_identity = savepoint_lines.identity;
  const auto savepoints = ParseSavepointBytes(context, savepoint_lines.binary_bytes);
  if (savepoints.diagnostic.error) return savepoints.diagnostic;
  const auto savepoint_digest = MetadataSavepointCacheDigest(savepoint_lines, savepoints);
  if (!savepoint_digest.ok()) return MakeInvalidRequestDiagnostic(
      "mga.relation_metadata", "savepoint_cache_digest_failed");
  const MgaMetadataCacheKey cache_key{
      context.database_uuid,
      metadata_path,
      metadata_identity.ok ? metadata_identity.file_size : 0,
      metadata_identity.ok ? metadata_identity.file_mtime_ticks : 0,
      savepoint_path,
      savepoint_identity.ok ? savepoint_identity.file_size : 0,
      savepoint_identity.ok ? savepoint_identity.file_mtime_ticks : 0,
      context.local_transaction_id,metadata_lines.content_sha256,savepoint_digest.digest,
      descriptor_path,descriptor_lines.content_sha256};
  {
    const std::lock_guard<std::mutex> guard(MgaMetadataCacheMutex());
    const auto cached = MgaMetadataCache().find(cache_key);
    if (cached != MgaMetadataCache().end() && cached->second != nullptr) {
      const auto& snapshot = *cached->second;
      state->tables.insert(state->tables.end(),
                           snapshot.tables.begin(),
                           snapshot.tables.end());
      state->indexes.insert(state->indexes.end(),
                            snapshot.indexes.begin(),
                            snapshot.indexes.end());
      state->sealed_relation_descriptor_snapshots.insert(
          state->sealed_relation_descriptor_snapshots.end(),
          snapshot.sealed_relation_descriptor_snapshots.begin(),
          snapshot.sealed_relation_descriptor_snapshots.end());
      state->max_event_sequence =
          std::max(state->max_event_sequence,
                   snapshot.max_event_sequence);
      return OkDiagnostic();
    }
  }
  bool nested_metadata_valid = true;
  const auto decode_pairs = [&](const std::string& bytes) {
    std::vector<std::pair<std::string, std::string>> pairs;
    if (!DecodeMetadataPairs(bytes, &pairs)) nested_metadata_valid = false;
    return pairs;
  };
  MgaMetadataCacheEntry decoded;
  for (const auto& line : metadata_lines.lines) {
    const auto fields = SplitTabs(line);
    if (fields.size() < 4 || fields[0] != kRowStoreMagic) { continue; }
    if (fields[1] == "TABLE_METADATA") {
      if (fields.size() < 11) {
        return MakeInvalidRequestDiagnostic("mga.relation_metadata", "table_metadata_invalid");
      }
      CrudTableRecord table;
      table.creator_tx = ParseU64(fields[2]);
      table.event_sequence = ParseU64(fields[3]);
      if (!ReadMetadataUuid(fields[4], &table.table_uuid)) {
        return MakeInvalidRequestDiagnostic(
            "mga.relation_metadata", "binary_metadata_uuid_invalid");
      }
      table.default_name = std::string(fields[5]);
      table.columns = decode_pairs(fields[6]);
      table.temporary = fields[7] == "1";
      table.temporary_scope = fields[8];
      if (!ReadMetadataUuid(fields[9], &table.temporary_session_uuid, true)) {
        return MakeInvalidRequestDiagnostic(
            "mga.relation_metadata", "binary_metadata_uuid_invalid");
      }
      table.on_commit_action = fields[10];
      if (table.temporary && !table.table_uuid.is_nil()) {
        decoded.known_temporary_relation_uuids.insert(table.table_uuid);
      }
      if (MetadataEventRolledBackBySavepoint(savepoints,
                                             table.creator_tx,
                                             table.event_sequence)) {
        continue;
      }
      decoded.max_event_sequence =
          std::max(decoded.max_event_sequence, ParseU64(fields[3]));
      decoded.tables.push_back(std::move(table));
    } else if (fields[1] == kSealedTableMetadataKindV2) {
      namespace stf = sealed_table_metadata_field_v2;
      const auto canonical_u64 = [&](const std::size_t index,
                                     std::uint64_t* output,
                                     const bool allow_zero) {
        if (output == nullptr || index >= fields.size() ||
            fields[index].empty()) {
          return false;
        }
        const std::uint64_t parsed = ParseU64(fields[index]);
        if ((!allow_zero && parsed == 0) ||
            std::to_string(parsed) != fields[index]) {
          return false;
        }
        *output = parsed;
        return true;
      };
      std::uint64_t creator_tx = 0;
      std::uint64_t event_sequence = 0;
      std::uint64_t descriptor_generation = 0;
      std::uint64_t descriptor_field_count = 0;
      std::uint64_t descriptor_field_bytes = 0;
      std::uint64_t contextual_sidecar_count_u64 = 0;
      if (fields.size() != stf::kFieldCount ||
          fields[stf::kMagic] != kRowStoreMagic ||
          fields[stf::kRecordKind] != kSealedTableMetadataKindV2 ||
          fields[stf::kFormat] != kSealedTableMetadataFormatV2 ||
          fields[stf::kSealState] != "sealed" ||
          !canonical_u64(stf::kCreatorTx, &creator_tx, false) ||
          !canonical_u64(stf::kEventSequence, &event_sequence, false) ||
          !canonical_u64(stf::kRelationDescriptorGeneration,
                         &descriptor_generation, false) ||
          !canonical_u64(stf::kDescriptorFieldCount,
                         &descriptor_field_count, false) ||
          !canonical_u64(stf::kDescriptorFieldBytes,
                         &descriptor_field_bytes, false) ||
          !canonical_u64(stf::kContextualSidecarCount,
                         &contextual_sidecar_count_u64, true) ||
          contextual_sidecar_count_u64 >
              std::numeric_limits<std::uint32_t>::max() ||
          !CanonicalNonNilMigrationUuid(fields[stf::kTableUuid]) ||
          !CanonicalNonNilMigrationUuid(
              fields[stf::kRelationDescriptorUuid]) ||
          (fields[stf::kTemporary] != "0" &&
           fields[stf::kTemporary] != "1")) {
        return MakeInvalidRequestDiagnostic(
            "mga.relation_metadata",
            "sealed_table_metadata_v2_header_invalid");
      }
      CrudTableRecord table;
      table.creator_tx = creator_tx;
      table.event_sequence = event_sequence;
      if (!ReadMetadataUuid(fields[stf::kTableUuid], &table.table_uuid)) {
        return MakeInvalidRequestDiagnostic(
            "mga.relation_metadata", "binary_metadata_uuid_invalid");
      }
      table.default_name = std::string(fields[stf::kDefaultName]);
      table.columns = decode_pairs(fields[stf::kColumns]);
      table.temporary = fields[stf::kTemporary] == "1";
      table.temporary_scope = fields[stf::kTemporaryScope];
      if (!ReadMetadataUuid(fields[stf::kTemporarySessionUuid], &table.temporary_session_uuid, true)) {
        return MakeInvalidRequestDiagnostic(
            "mga.relation_metadata", "binary_metadata_uuid_invalid");
      }
      table.on_commit_action = fields[stf::kOnCommitAction];
      if (table.temporary && !table.table_uuid.is_nil()) {
        decoded.known_temporary_relation_uuids.insert(table.table_uuid);
      }
      const auto complete_fields =
          decode_pairs(fields[stf::kDescriptorFields]);
      if (std::string(table.default_name) !=
              fields[stf::kDefaultName] ||
          EncodeMetadataPairs(table.columns) != fields[stf::kColumns] ||
          EncodeMetadataPairs(complete_fields) !=
              fields[stf::kDescriptorFields] ||
          complete_fields.size() != descriptor_field_count) {
        return MakeInvalidRequestDiagnostic(
            "mga.relation_metadata",
            "sealed_table_metadata_v2_vector_invalid");
      }
      const auto descriptor =
          DeserializeMgaRelationStorageDescriptor(complete_fields);
      const auto descriptor_validation =
          ValidateMgaRelationStorageDescriptor(descriptor);
      const auto base_fields =
          SerializeMgaRelationStorageDescriptor(descriptor);
      if (descriptor_validation.error || base_fields.empty() ||
          complete_fields.size() < base_fields.size() + 1 ||
          !std::equal(base_fields.begin(), base_fields.end(),
                      complete_fields.begin()) ||
          descriptor.database_uuid !=
              context.database_uuid ||
          descriptor.relation_uuid != table.table_uuid ||
          // Catalog relation generation and metadata event sequence are
          // separate counters. The sealed set binds both independently.
          MetadataUuidBytes(descriptor.descriptor_uuid) !=
              fields[stf::kRelationDescriptorUuid] ||
          descriptor.descriptor_generation != descriptor_generation) {
        return MakeInvalidRequestDiagnostic(
            "mga.relation_metadata",
            "sealed_table_metadata_v2_descriptor_invalid");
      }
      std::vector<MgaContextualTextDescriptorFieldPairV2> raw_fields;
      raw_fields.reserve(complete_fields.size());
      for (const auto& [key, value] : complete_fields) {
        raw_fields.push_back(
            {{key.begin(), key.end()}, {value.begin(), value.end()}});
      }
      MgaContextualTextRawBytesV2 canonical_vector;
      std::uint64_t canonical_vector_bytes = 0;
      MgaContextualTextSidecarSetDiagnosticV2 sidecar_diagnostic;
      const auto& final_pair = complete_fields.back();
      if (final_pair.first != kMgaContextualTextSidecarSetSealKeyV2 ||
          final_pair.second.size() !=
              MgaContextualTextSha256V2{}.size() ||
          !SerializeMgaContextualTextDescriptorFieldVectorV2(
              raw_fields, &canonical_vector, &canonical_vector_bytes,
              &sidecar_diagnostic) ||
          canonical_vector_bytes != descriptor_field_bytes) {
        return MakeInvalidRequestDiagnostic(
            "mga.relation_metadata",
            "sealed_table_metadata_v2_seal_or_extent_invalid");
      }
      if (MetadataEventRolledBackBySavepoint(
              savepoints, creator_tx, event_sequence)) {
        continue;
      }
      CrudSealedRelationDescriptorSnapshot snapshot;
      snapshot.creator_tx = creator_tx;
      snapshot.event_sequence = event_sequence;
      snapshot.relation_uuid = table.table_uuid;
      if (!ReadMetadataUuid(fields[stf::kRelationDescriptorUuid], &snapshot.relation_descriptor_uuid)) {
        return MakeInvalidRequestDiagnostic(
            "mga.relation_metadata", "binary_metadata_uuid_invalid");
      }
      snapshot.relation_descriptor_generation = descriptor_generation;
      snapshot.descriptor_field_count = descriptor_field_count;
      snapshot.descriptor_field_bytes = descriptor_field_bytes;
      snapshot.contextual_sidecar_count =
          static_cast<std::uint32_t>(contextual_sidecar_count_u64);
      snapshot.descriptor_fields = complete_fields;
      decoded.max_event_sequence =
          std::max(decoded.max_event_sequence, event_sequence);
      decoded.tables.push_back(std::move(table));
      decoded.sealed_relation_descriptor_snapshots.push_back(
          std::move(snapshot));
    } else if (fields[1] == "CONSTRAINT_MUTATION_BATCH") {
      // The constraint metadata and its table-column projection are sealed in
      // this one physical record.  The immutable relation-storage descriptor
      // UUID/generation remains the exact base binding and is not updated by
      // this bounded D1 bridge.
      namespace cbf = constraint_batch_field;
      if (fields.size() != cbf::kFieldCount ||
          fields[cbf::kMagic] != kRowStoreMagic ||
          fields[cbf::kRecordKind] != "CONSTRAINT_MUTATION_BATCH" ||
          ParseU64(fields[cbf::kCreatorTx]) == 0 ||
          ParseU64(fields[cbf::kEventSequence]) == 0 ||
          fields[cbf::kFormatVersion] != "neutral_fk_mutation_batch_v1" ||
          fields[cbf::kSealState] != "sealed" ||
          fields[cbf::kBatchHash].size() != 71 ||
          !fields[cbf::kBatchHash].starts_with("sha256:") ||
          fields[cbf::kMutationCount] != "1" ||
          !ValidConstraintBatchUuid(fields[cbf::kDatabaseUuid],
                                    scratchbird::core::platform::UuidKind::database) ||
          !ValidConstraintBatchUuid(fields[cbf::kBatchUuid],
                                    scratchbird::core::platform::UuidKind::row) ||
          !ValidConstraintBatchUuid(fields[cbf::kConstraintUuid],
                                    scratchbird::core::platform::UuidKind::object) ||
          !ValidConstraintBatchUuid(fields[cbf::kOwnerTableUuid],
                                    scratchbird::core::platform::UuidKind::object) ||
          !ValidConstraintBatchUuid(fields[cbf::kChildSchemaUuid],
                                    scratchbird::core::platform::UuidKind::schema) ||
          !ValidConstraintBatchUuid(fields[cbf::kChildDescriptorUuid],
                                    scratchbird::core::platform::UuidKind::object) ||
          ParseU64(fields[cbf::kChildDescriptorGeneration]) == 0 ||
          !ValidConstraintBatchUuid(fields[cbf::kChildColumnUuid],
                                    scratchbird::core::platform::UuidKind::object) ||
          !ValidConstraintBatchUuid(fields[cbf::kParentTableUuid],
                                    scratchbird::core::platform::UuidKind::object) ||
          !ValidConstraintBatchUuid(fields[cbf::kParentSchemaUuid],
                                    scratchbird::core::platform::UuidKind::schema) ||
          !ValidConstraintBatchUuid(fields[cbf::kParentDescriptorUuid],
                                    scratchbird::core::platform::UuidKind::object) ||
          ParseU64(fields[cbf::kParentDescriptorGeneration]) == 0 ||
          !ValidConstraintBatchUuid(fields[cbf::kParentColumnUuid],
                                    scratchbird::core::platform::UuidKind::object) ||
          !ValidConstraintBatchUuid(fields[cbf::kParentCandidateConstraintUuid],
                                    scratchbird::core::platform::UuidKind::object) ||
          !ValidConstraintBatchUuid(fields[cbf::kReferencedKeyDescriptorUuid],
                                    scratchbird::core::platform::UuidKind::object) ||
          !ValidConstraintBatchUuid(fields[cbf::kSupportUuid],
                                    scratchbird::core::platform::UuidKind::object) ||
          fields[cbf::kSupportFamily] != "btree" ||
          fields[cbf::kSupportPolicy] != "required_exact_unique_index" ||
          fields[cbf::kMatchPolicy] != "simple" ||
          fields[cbf::kOnUpdate] != "no_action" ||
          fields[cbf::kOnDelete] != "no_action" ||
          fields[cbf::kEnforcementTiming] != "immediate" ||
          ParseU64(fields[cbf::kConstraintMetadataGeneration]) != 1 ||
          ParseU64(fields[cbf::kBaseTableEventSequence]) == 0 ||
          ParseU64(fields[cbf::kParentBaseTableEventSequence]) == 0 ||
          fields[cbf::kConstraintKind] != "foreign_key" ||
          fields[cbf::kTableUuid] != fields[cbf::kOwnerTableUuid] ||
          fields[cbf::kDatabaseUuid] != MetadataUuidBytes(context.database_uuid)) {
        return MakeInvalidRequestDiagnostic(
            "mga.relation_metadata", "constraint_mutation_batch_invalid");
      }
      MgaConstraintMutationBatch batch;
      batch.format_version = fields[cbf::kFormatVersion];
      if (!ReadMetadataUuid(fields[cbf::kBatchUuid], &batch.batch_uuid)) return MakeInvalidRequestDiagnostic("mga.relation_metadata", "binary_metadata_uuid_invalid");
      batch.batch_hash = fields[cbf::kBatchHash];
      batch.mutation_count = static_cast<std::uint32_t>(
          ParseU64(fields[cbf::kMutationCount]));
      if (!ReadMetadataUuid(fields[cbf::kDatabaseUuid], &batch.database_uuid)) return MakeInvalidRequestDiagnostic("mga.relation_metadata", "binary_metadata_uuid_invalid");
      if (!ReadMetadataUuid(fields[cbf::kConstraintUuid], &batch.constraint_uuid)) return MakeInvalidRequestDiagnostic("mga.relation_metadata", "binary_metadata_uuid_invalid");
      if (!ReadMetadataUuid(fields[cbf::kOwnerTableUuid], &batch.owner_table_uuid)) return MakeInvalidRequestDiagnostic("mga.relation_metadata", "binary_metadata_uuid_invalid");
      if (!ReadMetadataUuid(fields[cbf::kChildSchemaUuid], &batch.child_schema_uuid)) return MakeInvalidRequestDiagnostic("mga.relation_metadata", "binary_metadata_uuid_invalid");
      if (!ReadMetadataUuid(fields[cbf::kChildDescriptorUuid], &batch.child_relation_descriptor_uuid)) return MakeInvalidRequestDiagnostic("mga.relation_metadata", "binary_metadata_uuid_invalid");
      batch.child_relation_descriptor_generation =
          ParseU64(fields[cbf::kChildDescriptorGeneration]);
      if (!ReadMetadataUuid(fields[cbf::kChildColumnUuid], &batch.child_column_uuid)) return MakeInvalidRequestDiagnostic("mga.relation_metadata", "binary_metadata_uuid_invalid");
      if (!ReadMetadataUuid(fields[cbf::kParentTableUuid], &batch.parent_table_uuid)) return MakeInvalidRequestDiagnostic("mga.relation_metadata", "binary_metadata_uuid_invalid");
      if (!ReadMetadataUuid(fields[cbf::kParentSchemaUuid], &batch.parent_schema_uuid)) return MakeInvalidRequestDiagnostic("mga.relation_metadata", "binary_metadata_uuid_invalid");
      if (!ReadMetadataUuid(fields[cbf::kParentDescriptorUuid], &batch.parent_relation_descriptor_uuid)) return MakeInvalidRequestDiagnostic("mga.relation_metadata", "binary_metadata_uuid_invalid");
      batch.parent_relation_descriptor_generation =
          ParseU64(fields[cbf::kParentDescriptorGeneration]);
      if (!ReadMetadataUuid(fields[cbf::kParentColumnUuid], &batch.parent_column_uuid)) return MakeInvalidRequestDiagnostic("mga.relation_metadata", "binary_metadata_uuid_invalid");
      if (!ReadMetadataUuid(fields[cbf::kParentCandidateConstraintUuid], &batch.parent_candidate_key_constraint_uuid)) return MakeInvalidRequestDiagnostic("mga.relation_metadata", "binary_metadata_uuid_invalid");
      if (!ReadMetadataUuid(fields[cbf::kReferencedKeyDescriptorUuid], &batch.key_descriptor_uuid)) return MakeInvalidRequestDiagnostic("mga.relation_metadata", "binary_metadata_uuid_invalid");
      if (!ReadMetadataUuid(fields[cbf::kSupportUuid], &batch.support_uuid)) return MakeInvalidRequestDiagnostic("mga.relation_metadata", "binary_metadata_uuid_invalid");
      batch.support_family = fields[cbf::kSupportFamily];
      batch.support_policy = fields[cbf::kSupportPolicy];
      batch.match_policy = fields[cbf::kMatchPolicy];
      batch.on_update_action = fields[cbf::kOnUpdate];
      batch.on_delete_action = fields[cbf::kOnDelete];
      batch.enforcement_timing = fields[cbf::kEnforcementTiming];
      batch.constraint_metadata_generation =
          ParseU64(fields[cbf::kConstraintMetadataGeneration]);
      batch.base_table_event_sequence =
          ParseU64(fields[cbf::kBaseTableEventSequence]);
      batch.parent_base_table_event_sequence =
          ParseU64(fields[cbf::kParentBaseTableEventSequence]);
      batch.constraint_name = std::string(fields[cbf::kConstraintName]);
      batch.constraint_kind = fields[cbf::kConstraintKind];
      batch.canonical_constraint_envelope =
          std::string(fields[cbf::kCanonicalEnvelope]);
      CrudTableRecord table;
      table.creator_tx = ParseU64(fields[cbf::kCreatorTx]);
      table.event_sequence = ParseU64(fields[cbf::kEventSequence]);
      if (!ReadMetadataUuid(fields[cbf::kTableUuid], &table.table_uuid)) {
        return MakeInvalidRequestDiagnostic(
            "mga.relation_metadata", "binary_metadata_uuid_invalid");
      }
      table.default_name = std::string(fields[cbf::kTableDefaultName]);
      table.columns = decode_pairs(fields[cbf::kTableColumns]);
      table.temporary = fields[cbf::kTableTemporary] == "1";
      table.temporary_scope = fields[cbf::kTableTemporaryScope];
      if (!ReadMetadataUuid(fields[cbf::kTableTemporarySessionUuid], &table.temporary_session_uuid, true)) {
        return MakeInvalidRequestDiagnostic(
            "mga.relation_metadata", "binary_metadata_uuid_invalid");
      }
      table.on_commit_action = fields[cbf::kTableOnCommitAction];
      if (table.temporary || !table.temporary_scope.empty() ||
          !table.temporary_session_uuid.is_nil() ||
          !table.on_commit_action.empty()) {
        return MakeInvalidRequestDiagnostic(
            "mga.relation_metadata",
            "temporary_constraint_mutation_batch_unsupported");
      }
      batch.updated_table = table;
      const auto canonical_fields = ConstraintMutationBatchLineFields(
          batch, table.creator_tx, table.event_sequence);
      if (canonical_fields != fields) {
        return MakeInvalidRequestDiagnostic(
            "mga.relation_metadata",
            "constraint_mutation_batch_noncanonical_encoding");
      }
      const std::string expected_hash =
          ComputeMgaConstraintMutationBatchHash(
              batch, table.creator_tx, table.event_sequence);
      if (expected_hash.empty() ||
          !scratchbird::core::hash::ConstantTimeEqual(expected_hash,
                                                       batch.batch_hash)) {
        return MakeInvalidRequestDiagnostic(
            "mga.relation_metadata",
            "constraint_mutation_batch_hash_mismatch");
      }
      if (MetadataEventRolledBackBySavepoint(savepoints,
                                             table.creator_tx,
                                             table.event_sequence)) {
        continue;
      }
      decoded.max_event_sequence =
          std::max(decoded.max_event_sequence, table.event_sequence);
      decoded.tables.push_back(std::move(table));
    } else if (fields[1] == "BIGINT_IDENTITY_MIGRATION_BATCH") {
      constexpr std::size_t kHeaderFields = 14;
      constexpr std::size_t kFieldsPerRow = 13;
      // A torn append has no authority and is ignored. A complete-looking
      // record with a bad seal or digest is a catalog contradiction.
      if (fields.size() < kHeaderFields) continue;
      const std::uint64_t creator_tx = ParseU64(fields[2]);
      const std::uint64_t event_sequence = ParseU64(fields[3]);
      const std::uint64_t mutation_count = ParseU64(fields[13]);
      if (mutation_count == 0 ||
          fields.size() != kHeaderFields + mutation_count * kFieldsPerRow) {
        continue;
      }
      if (creator_tx == 0 || event_sequence == 0 ||
          fields[4] != kBigintMigrationFormat || fields[5] != "sealed" ||
          fields[6].size() != 71 || !fields[6].starts_with("sha256:") ||
          fields[7] != kBigintMigrationId || fields[8].empty() ||
          ParseU64(fields[11]) == 0 ||
          ParseU64(fields[12]) != ParseU64(fields[11]) + 1) {
        return MakeInvalidRequestDiagnostic(
            "mga.relation_metadata", "bigint_migration_batch_invalid");
      }
      EngineUuid migration_transaction_uuid;
      if (!ReadMetadataUuid(fields[8], &migration_transaction_uuid)) return MakeInvalidRequestDiagnostic(
          "mga.relation_metadata", "migration_transaction_uuid_invalid");
      MgaBigintIdentityMigrationRequest request;
      request.migration_id = fields[7];
      if (!ReadMetadataUuid(fields[9], &request.prior_catalog_snapshot_uuid)) return MakeInvalidRequestDiagnostic("mga.relation_metadata", "binary_metadata_uuid_invalid");
      if (!ReadMetadataUuid(fields[10], &request.new_catalog_snapshot_uuid)) return MakeInvalidRequestDiagnostic("mga.relation_metadata", "binary_metadata_uuid_invalid");
      request.prior_catalog_generation = ParseU64(fields[11]);
      request.new_catalog_generation = ParseU64(fields[12]);
      std::vector<CrudTableRecord> tables;
      std::vector<std::string> decisions;
      std::set<std::pair<EngineUuid, EngineUuid>> identities;
      for (std::size_t i = 0; i < mutation_count; ++i) {
        const std::size_t base = kHeaderFields + i * kFieldsPerRow;
        MgaBigintIdentityMigrationRow row;
        if (!ReadMetadataUuid(fields[base], &row.object_uuid)) return MakeInvalidRequestDiagnostic("mga.relation_metadata", "binary_metadata_uuid_invalid");
        if (!ReadMetadataUuid(fields[base + 1], &row.column_uuid)) return MakeInvalidRequestDiagnostic("mga.relation_metadata", "binary_metadata_uuid_invalid");
        row.old_row_generation = ParseU64(fields[base + 4]);
        if (fields[base + 2] != MetadataUuidBytes(kLegacyBigintTypeUuid) ||
            fields[base + 3] != MetadataUuidBytes(kCanonicalBigintTypeUuid) ||
            row.old_row_generation == 0 ||
            ParseU64(fields[base + 5]) != event_sequence ||
            fields[base + 6].size() != 71 ||
            !fields[base + 6].starts_with("sha256:") ||
            !identities.emplace(row.object_uuid, row.column_uuid).second) {
          return MakeInvalidRequestDiagnostic(
              "mga.relation_metadata", "bigint_migration_batch_conflict");
        }
        CrudTableRecord table;
        table.creator_tx = creator_tx;
        table.event_sequence = event_sequence;
        if (!core::uuid::IsEngineIdentityUuid(row.object_uuid)) {
          return MakeInvalidRequestDiagnostic(
              "mga.relation_metadata", "binary_metadata_uuid_invalid");
        }
        table.table_uuid = row.object_uuid;
        table.default_name = std::string(fields[base + 7]);
        table.columns = decode_pairs(fields[base + 8]);
        table.temporary = fields[base + 9] == "1";
        table.temporary_scope = fields[base + 10];
        if (!ReadMetadataUuid(fields[base + 11], &table.temporary_session_uuid, true)) {
          return MakeInvalidRequestDiagnostic(
              "mga.relation_metadata", "binary_metadata_uuid_invalid");
        }
        table.on_commit_action = fields[base + 12];
        if (table.temporary || !table.temporary_scope.empty() ||
            !table.temporary_session_uuid.is_nil() ||
            !table.on_commit_action.empty()) {
          return MakeInvalidRequestDiagnostic(
              "mga.relation_metadata", "bigint_migration_temporary_unsupported");
        }
        request.rows.push_back(std::move(row));
        decisions.push_back(fields[base + 6]);
        tables.push_back(std::move(table));
      }
      for (std::size_t i = 0; i < request.rows.size(); ++i) {
        const std::string expected_decision = BigintMigrationDecisionHash(
            request, request.rows[i], tables[i].event_sequence, migration_transaction_uuid);
        if (!scratchbird::core::hash::ConstantTimeEqual(
                expected_decision, decisions[i])) {
          return MakeInvalidRequestDiagnostic(
              "mga.relation_metadata",
              "bigint_migration_decision_hash_mismatch");
        }
      }
      const std::string payload = CanonicalBigintMigrationPayload(
          request, creator_tx, event_sequence, migration_transaction_uuid, tables, decisions);
      if (!scratchbird::core::hash::ConstantTimeEqual(
              Sha256Tagged(payload), fields[6])) {
        return MakeInvalidRequestDiagnostic(
            "mga.relation_metadata", "bigint_migration_batch_hash_mismatch");
      }
      if (MetadataEventRolledBackBySavepoint(savepoints, creator_tx,
                                             event_sequence)) {
        continue;
      }
      decoded.max_event_sequence =
          std::max(decoded.max_event_sequence, event_sequence);
      decoded.tables.insert(decoded.tables.end(),
                            std::make_move_iterator(tables.begin()),
                            std::make_move_iterator(tables.end()));
    } else if (fields[1] == "INT32_IDENTITY_MIGRATION_BATCH") {
      constexpr std::size_t kHeaderFields = 14;
      constexpr std::size_t kFieldsPerRow = 15;
      if (fields.size() < kHeaderFields) continue;
      const std::uint64_t creator_tx = ParseU64(fields[2]);
      const std::uint64_t event_sequence = ParseU64(fields[3]);
      const std::uint64_t mutation_count = ParseU64(fields[13]);
      if (mutation_count == 0 ||
          fields.size() != kHeaderFields + mutation_count * kFieldsPerRow) {
        continue;
      }
      if (creator_tx == 0 || event_sequence == 0 ||
          fields[4] != kInt32MigrationFormat || fields[5] != "sealed" ||
          fields[6].size() != 71 || !fields[6].starts_with("sha256:") ||
          fields[7] != kInt32MigrationId || fields[8].empty() ||
          ParseU64(fields[11]) == 0 ||
          ParseU64(fields[12]) != ParseU64(fields[11]) + 1) {
        return MakeInvalidRequestDiagnostic(
            "mga.relation_metadata", "int32_migration_batch_invalid");
      }
      EngineUuid migration_transaction_uuid;
      if (!ReadMetadataUuid(fields[8], &migration_transaction_uuid)) return MakeInvalidRequestDiagnostic(
          "mga.relation_metadata", "migration_transaction_uuid_invalid");
      MgaInt32IdentityMigrationRequest request;
      request.migration_id = fields[7];
      if (!ReadMetadataUuid(fields[9], &request.prior_catalog_snapshot_uuid)) return MakeInvalidRequestDiagnostic("mga.relation_metadata", "binary_metadata_uuid_invalid");
      if (!ReadMetadataUuid(fields[10], &request.new_catalog_snapshot_uuid)) return MakeInvalidRequestDiagnostic("mga.relation_metadata", "binary_metadata_uuid_invalid");
      request.prior_catalog_generation = ParseU64(fields[11]);
      request.new_catalog_generation = ParseU64(fields[12]);
      std::vector<CrudTableRecord> tables;
      std::vector<std::string> decisions;
      std::set<std::pair<EngineUuid, EngineUuid>> identities;
      std::map<EngineUuid, std::string> table_projections;
      std::map<EngineUuid, CrudTableRecord> unique_tables;
      for (std::size_t i = 0; i < mutation_count; ++i) {
        const std::size_t base = kHeaderFields + i * kFieldsPerRow;
        MgaInt32IdentityMigrationRow row;
        if (!ReadMetadataUuid(fields[base], &row.object_uuid)) return MakeInvalidRequestDiagnostic("mga.relation_metadata", "binary_metadata_uuid_invalid");
        if (!ReadMetadataUuid(fields[base + 1], &row.column_uuid)) return MakeInvalidRequestDiagnostic("mga.relation_metadata", "binary_metadata_uuid_invalid");
        row.old_row_generation = ParseU64(fields[base + 6]);
        if (fields[base + 2] != MetadataUuidBytes(kLegacyInt32DescriptorUuid) ||
            fields[base + 3] != MetadataUuidBytes(kCanonicalInt32DescriptorUuid) ||
            fields[base + 4] != MetadataUuidBytes(kLegacyInt32TypeUuid) ||
            fields[base + 5] != MetadataUuidBytes(kCanonicalInt32TypeUuid) ||
            row.old_row_generation == 0 ||
            ParseU64(fields[base + 7]) != event_sequence ||
            fields[base + 8].size() != 71 ||
            !fields[base + 8].starts_with("sha256:") ||
            !identities.emplace(row.object_uuid, row.column_uuid).second) {
          return MakeInvalidRequestDiagnostic(
              "mga.relation_metadata", "int32_migration_batch_conflict");
        }
        CrudTableRecord table;
        table.creator_tx = creator_tx;
        table.event_sequence = event_sequence;
        if (!core::uuid::IsEngineIdentityUuid(row.object_uuid)) {
          return MakeInvalidRequestDiagnostic(
              "mga.relation_metadata", "binary_metadata_uuid_invalid");
        }
        table.table_uuid = row.object_uuid;
        table.default_name = std::string(fields[base + 9]);
        table.columns = decode_pairs(fields[base + 10]);
        table.temporary = fields[base + 11] == "1";
        table.temporary_scope = fields[base + 12];
        if (!ReadMetadataUuid(fields[base + 13], &table.temporary_session_uuid, true)) {
          return MakeInvalidRequestDiagnostic(
              "mga.relation_metadata", "binary_metadata_uuid_invalid");
        }
        table.on_commit_action = fields[base + 14];
        if (table.temporary || !table.temporary_scope.empty() ||
            !table.temporary_session_uuid.is_nil() ||
            !table.on_commit_action.empty()) {
          return MakeInvalidRequestDiagnostic(
              "mga.relation_metadata", "int32_migration_temporary_unsupported");
        }
        const std::string projection =
            fields[base + 9] + "\n" + fields[base + 10] + "\n" +
            fields[base + 11] + "\n" + fields[base + 12] + "\n" +
            fields[base + 13] + "\n" + fields[base + 14];
        const auto prior_projection = table_projections.find(row.object_uuid);
        if (prior_projection != table_projections.end() &&
            prior_projection->second != projection) {
          return MakeInvalidRequestDiagnostic(
              "mga.relation_metadata",
              "int32_migration_object_projection_conflict");
        }
        table_projections[row.object_uuid] = projection;
        unique_tables[row.object_uuid] = table;
        request.rows.push_back(std::move(row));
        decisions.push_back(fields[base + 8]);
        tables.push_back(std::move(table));
      }
      for (std::size_t i = 0; i < request.rows.size(); ++i) {
        const std::string expected_decision = Int32MigrationDecisionHash(
            request, request.rows[i], tables[i].event_sequence, migration_transaction_uuid);
        if (!scratchbird::core::hash::ConstantTimeEqual(
                expected_decision, decisions[i])) {
          return MakeInvalidRequestDiagnostic(
              "mga.relation_metadata",
              "int32_migration_decision_hash_mismatch");
        }
      }
      const std::string payload = CanonicalInt32MigrationPayload(
          request, creator_tx, event_sequence, migration_transaction_uuid, tables, decisions);
      if (!scratchbird::core::hash::ConstantTimeEqual(
              Sha256Tagged(payload), fields[6])) {
        return MakeInvalidRequestDiagnostic(
            "mga.relation_metadata", "int32_migration_batch_hash_mismatch");
      }
      if (MetadataEventRolledBackBySavepoint(savepoints, creator_tx,
                                             event_sequence)) {
        continue;
      }
      decoded.max_event_sequence =
          std::max(decoded.max_event_sequence, event_sequence);
      for (auto& [object_uuid, table] : unique_tables) {
        (void)object_uuid;
        decoded.tables.push_back(std::move(table));
      }
    } else if (fields[1] == "TEXT_IDENTITY_MIGRATION_BATCH") {
      constexpr std::size_t kHeaderFields = 17;
      constexpr std::size_t kFieldsPerRow = 25;
      if (fields.size() < kHeaderFields) continue;
      const std::uint64_t creator_tx = ParseU64(fields[2]);
      const std::uint64_t event_sequence = ParseU64(fields[3]);
      const std::uint64_t datatype_catalog_generation = ParseU64(fields[14]);
      const std::uint64_t datatype_registry_generation = ParseU64(fields[15]);
      const std::uint64_t mutation_count = ParseU64(fields[16]);
      if (mutation_count == 0 ||
          std::to_string(mutation_count) != fields[16] ||
          mutation_count >
              (std::numeric_limits<std::size_t>::max() - kHeaderFields) /
                  kFieldsPerRow ||
          fields.size() != kHeaderFields + mutation_count * kFieldsPerRow) {
        continue;
      }
      if (creator_tx == 0 || event_sequence == 0 ||
          fields[4] != kTextMigrationFormat || fields[5] != "sealed" ||
          fields[6].size() != 71 || !fields[6].starts_with("sha256:") ||
          fields[7] != kTextMigrationId || fields[8].empty() ||
          !ExactTextMigrationCreatorTransactionForStoreModule(
              context, creator_tx, fields[8]) ||
          !CanonicalNonNilMigrationUuid(fields[9]) ||
          !CanonicalNonNilMigrationUuid(fields[10]) ||
          fields[9] == fields[10] ||
          ParseU64(fields[11]) == 0 ||
          ParseU64(fields[11]) ==
              std::numeric_limits<std::uint64_t>::max() ||
          ParseU64(fields[12]) != ParseU64(fields[11]) + 1 ||
          !CanonicalNonNilMigrationUuid(fields[13]) ||
          fields[13] !=
              MetadataUuidBytes(context.datatype_catalog_snapshot_uuid) ||
          datatype_catalog_generation == 0 ||
          std::to_string(datatype_catalog_generation) != fields[14] ||
          datatype_catalog_generation !=
              context.datatype_catalog_generation ||
          datatype_registry_generation == 0 ||
          std::to_string(datatype_registry_generation) != fields[15] ||
          datatype_registry_generation !=
              context.datatype_registry_generation ||
          !ExactCanonicalTextIdentityAuthorityAvailable(context)) {
        return MakeInvalidRequestDiagnostic(
            "mga.relation_metadata", "text_migration_batch_invalid");
      }
      EngineUuid migration_transaction_uuid;
      if (!ReadMetadataUuid(fields[8], &migration_transaction_uuid)) return MakeInvalidRequestDiagnostic(
          "mga.relation_metadata", "migration_transaction_uuid_invalid");
      MgaTextIdentityMigrationRequest request;
      request.migration_id = fields[7];
      if (!ReadMetadataUuid(fields[9], &request.prior_catalog_snapshot_uuid)) return MakeInvalidRequestDiagnostic("mga.relation_metadata", "binary_metadata_uuid_invalid");
      if (!ReadMetadataUuid(fields[10], &request.new_catalog_snapshot_uuid)) return MakeInvalidRequestDiagnostic("mga.relation_metadata", "binary_metadata_uuid_invalid");
      request.prior_catalog_generation = ParseU64(fields[11]);
      request.new_catalog_generation = ParseU64(fields[12]);
      std::vector<CrudTableRecord> tables;
      std::vector<CrudSealedRelationDescriptorSnapshot>
          relation_descriptor_snapshots;
      std::vector<std::string> decisions;
      std::set<std::pair<EngineUuid, EngineUuid>> identities;
      std::map<EngineUuid, std::string> table_projections;
      std::map<EngineUuid, std::string> descriptor_projections;
      std::map<EngineUuid, CrudTableRecord> unique_tables;
      std::map<EngineUuid, CrudSealedRelationDescriptorSnapshot>
          unique_descriptors;
      for (std::size_t i = 0; i < mutation_count; ++i) {
        const std::size_t base = kHeaderFields + i * kFieldsPerRow;
        EngineUuid object_uuid, column_uuid;
        if (!ReadMetadataUuid(fields[base], &object_uuid) ||
            !ReadMetadataUuid(fields[base + 1], &column_uuid) ||
            !identities.emplace(object_uuid, column_uuid).second) {
          return MakeInvalidRequestDiagnostic(
              "mga.relation_metadata", "text_migration_batch_conflict");
        }
      }
      for (std::size_t i = 0; i < mutation_count; ++i) {
        const std::size_t base = kHeaderFields + i * kFieldsPerRow;
        MgaTextIdentityMigrationRow row;
        if (!ReadMetadataUuid(fields[base], &row.object_uuid)) return MakeInvalidRequestDiagnostic("mga.relation_metadata", "binary_metadata_uuid_invalid");
        if (!ReadMetadataUuid(fields[base + 1], &row.column_uuid)) return MakeInvalidRequestDiagnostic("mga.relation_metadata", "binary_metadata_uuid_invalid");
        row.old_row_generation = ParseU64(fields[base + 10]);
        if (fields[base + 2] != MetadataUuidBytes(kLegacyTextDescriptorUuid) ||
            fields[base + 3] != MetadataUuidBytes(kCanonicalTextDescriptorUuid) ||
            fields[base + 4] != MetadataUuidBytes(kLegacyTextTypeUuid) ||
            fields[base + 5] != MetadataUuidBytes(kCanonicalTextTypeUuid) ||
            fields[base + 6] != MetadataUuidBytes(kCanonicalTextCodecUuid) ||
            fields[base + 7] != kCanonicalTextCodecId ||
            fields[base + 8] != "1" || fields[base + 9] != "1" ||
            row.old_row_generation == 0 ||
            ParseU64(fields[base + 11]) != event_sequence ||
            fields[base + 12].size() != 71 ||
            !fields[base + 12].starts_with("sha256:")) {
          return MakeInvalidRequestDiagnostic(
              "mga.relation_metadata", "text_migration_batch_conflict");
        }
        CrudTableRecord table;
        table.creator_tx = creator_tx;
        table.event_sequence = event_sequence;
        if (!core::uuid::IsEngineIdentityUuid(row.object_uuid)) {
          return MakeInvalidRequestDiagnostic(
              "mga.relation_metadata", "binary_metadata_uuid_invalid");
        }
        table.table_uuid = row.object_uuid;
        table.default_name = std::string(fields[base + 13]);
        table.columns = decode_pairs(fields[base + 14]);
        table.temporary = fields[base + 21] == "1";
        table.temporary_scope = fields[base + 22];
        if (!ReadMetadataUuid(fields[base + 23], &table.temporary_session_uuid, true)) {
          return MakeInvalidRequestDiagnostic(
              "mga.relation_metadata", "binary_metadata_uuid_invalid");
        }
        table.on_commit_action = fields[base + 24];
        if (fields[base + 21] != "0" || table.temporary ||
            std::string(table.default_name) != fields[base + 13] ||
            EncodeMetadataPairs(table.columns) != fields[base + 14] ||
            !table.temporary_scope.empty() ||
            !table.temporary_session_uuid.is_nil() ||
            !table.on_commit_action.empty()) {
          return MakeInvalidRequestDiagnostic(
              "mga.relation_metadata", "text_migration_temporary_unsupported");
        }

        std::size_t migrated_table_columns = 0;
        std::string migrated_column_name;
        std::string migrated_column_descriptor;
        for (const auto& [column_name, descriptor] : table.columns) {
          if (!ExactCanonicalMigratedTextDescriptor(context, descriptor,
                                                     row.column_uuid)) {
            continue;
          }
          ++migrated_table_columns;
          migrated_column_name = column_name;
          migrated_column_descriptor = descriptor;
        }
        const auto relation_fields = decode_pairs(fields[base + 15]);
        if (EncodeMetadataPairs(relation_fields) != fields[base + 15]) {
          return MakeInvalidRequestDiagnostic(
              "mga.relation_metadata",
              "text_migration_relation_descriptor_encoding_invalid");
        }
        const auto relation_descriptor =
            DeserializeMgaRelationStorageDescriptor(relation_fields);
        const auto relation_validation =
            ValidateMgaRelationStorageDescriptor(relation_descriptor);
        std::size_t migrated_storage_columns = 0;
        for (const auto& column : relation_descriptor.columns) {
          if (column.column_uuid != row.column_uuid) continue;
          if (column.canonical_name_key != migrated_column_name ||
              column.value_descriptor.descriptor_uuid !=
                  row.column_uuid ||
              column.value_descriptor.encoded_descriptor !=
                  migrated_column_descriptor ||
              !ExactCanonicalMigratedTextDescriptor(
                  context, column.value_descriptor.encoded_descriptor,
                  row.column_uuid) ||
              column.column_generation != event_sequence) {
            return MakeInvalidRequestDiagnostic(
                "mga.relation_metadata",
                "text_migration_relation_descriptor_conflict");
          }
          ++migrated_storage_columns;
        }
        if (migrated_table_columns != 1 || migrated_storage_columns != 1 ||
            relation_validation.error ||
            relation_descriptor.database_uuid !=
                context.database_uuid ||
            relation_descriptor.relation_uuid != row.object_uuid ||
            relation_descriptor.relation_generation != event_sequence) {
          return MakeInvalidRequestDiagnostic(
              "mga.relation_metadata",
              "text_migration_relation_descriptor_invalid");
        }
        const auto base_relation_fields =
            SerializeMgaRelationStorageDescriptor(relation_descriptor);
        const std::uint64_t descriptor_generation =
            ParseU64(fields[base + 17]);
        const std::uint64_t descriptor_field_count =
            ParseU64(fields[base + 18]);
        const std::uint64_t descriptor_field_bytes =
            ParseU64(fields[base + 19]);
        const std::uint64_t contextual_sidecar_count =
            ParseU64(fields[base + 20]);
        if (!CanonicalNonNilMigrationUuid(fields[base + 16]) ||
            fields[base + 16] !=
                MetadataUuidBytes(relation_descriptor.descriptor_uuid) ||
            descriptor_generation == 0 ||
            descriptor_generation !=
                relation_descriptor.descriptor_generation ||
            std::to_string(descriptor_generation) != fields[base + 17] ||
            descriptor_field_count == 0 ||
            std::to_string(descriptor_field_count) != fields[base + 18] ||
            descriptor_field_bytes == 0 ||
            std::to_string(descriptor_field_bytes) != fields[base + 19] ||
            contextual_sidecar_count >
                std::numeric_limits<std::uint32_t>::max() ||
            std::to_string(contextual_sidecar_count) != fields[base + 20] ||
            relation_fields.size() != descriptor_field_count ||
            relation_fields.size() < base_relation_fields.size() + 1 ||
            !std::equal(base_relation_fields.begin(),
                        base_relation_fields.end(),
                        relation_fields.begin()) ||
            relation_fields.back().first !=
                kMgaContextualTextSidecarSetSealKeyV2 ||
            relation_fields.back().second.size() !=
                MgaContextualTextSha256V2{}.size()) {
          return MakeInvalidRequestDiagnostic(
              "mga.relation_metadata",
              "text_migration_relation_descriptor_seal_invalid");
        }
        EngineContextualTextPolicyRowSetV2 policy_rows;
        if (contextual_sidecar_count != 0) {
          const auto policy =
              LoadCurrentEngineContextualTextPolicyRowSetForPublicationV2();
          if (!policy.ok) return policy.diagnostic;
          policy_rows = policy.rows;
        }
        MgaContextualTextProjectionMaterialV2 projection_material;
        EngineApiDiagnostic projection_diagnostic;
        if (!BuildMgaContextualTextProjectionMaterialV2(
                context, relation_descriptor, policy_rows,
                &projection_material, &projection_diagnostic)) {
          return projection_diagnostic;
        }
        CrudSealedRelationDescriptorSnapshot relation_snapshot;
        relation_snapshot.creator_tx = creator_tx;
        relation_snapshot.event_sequence = event_sequence;
        relation_snapshot.relation_uuid = row.object_uuid;
        if (!ReadMetadataUuid(fields[base + 16], &relation_snapshot.relation_descriptor_uuid)) {
          return MakeInvalidRequestDiagnostic(
              "mga.relation_metadata", "binary_metadata_uuid_invalid");
        }
        relation_snapshot.relation_descriptor_generation =
            descriptor_generation;
        relation_snapshot.descriptor_field_count = descriptor_field_count;
        relation_snapshot.descriptor_field_bytes = descriptor_field_bytes;
        relation_snapshot.contextual_sidecar_count =
            static_cast<std::uint32_t>(contextual_sidecar_count);
        relation_snapshot.descriptor_fields = relation_fields;
        MgaContextualTextSidecarSetV2 candidate_set;
        candidate_set.owner.creator_transaction_id = creator_tx;
        candidate_set.owner.event_sequence = event_sequence;
        candidate_set.owner.relation_descriptor_generation =
            descriptor_generation;
        if (!CopyContextualUuidV2(row.object_uuid,
                                  &candidate_set.owner.relation_uuid) ||
            !CopyContextualUuidV2(
                relation_snapshot.relation_descriptor_uuid,
                &candidate_set.owner.relation_descriptor_uuid)) {
          return MakeInvalidRequestDiagnostic(
              "mga.relation_metadata",
              "text_migration_relation_descriptor_owner_invalid");
        }
        candidate_set.descriptor_field_count = descriptor_field_count;
        candidate_set.descriptor_field_bytes = descriptor_field_bytes;
        candidate_set.contextual_sidecar_count =
            relation_snapshot.contextual_sidecar_count;
        candidate_set.descriptor_fields =
            RawContextualDescriptorFieldsV2(relation_fields);
        std::copy(relation_fields.back().second.begin(),
                  relation_fields.back().second.end(),
                  candidate_set.seal_sha256.begin());
        const auto raw_base_fields =
            RawContextualDescriptorFieldsV2(base_relation_fields);
        MgaContextualTextSidecarSetDiagnosticV2 sidecar_diagnostic;
        if (!ValidateMgaContextualTextSidecarSetV2(
                candidate_set.owner, raw_base_fields,
                projection_material.projected_columns, candidate_set,
                &sidecar_diagnostic)) {
          return MakeEngineApiDiagnostic(
              sidecar_diagnostic.code.empty()
                  ? "CTB.TEXT.DESCRIPTOR_INVALID"
                  : sidecar_diagnostic.code,
              "mga.relation_metadata.text_migration_sidecar_invalid",
              sidecar_diagnostic.detail, true);
        }

        // A seal is a transition from an exact visible provisional row, not
        // an authority to inject a self-consistent canonical replacement. The
        // prior table row and its persisted physical descriptor must both be
        // present, visible, and byte-for-byte transform into this sealed row.
        const CrudTableRecord* prior_table = nullptr;
        std::uint64_t newest_prior_generation = 0;
        for (const auto& candidate : decoded.tables) {
          if (candidate.table_uuid != row.object_uuid ||
              candidate.event_sequence >= event_sequence ||
              !TextMigrationLineageCreatorVisibleForStoreModule(
                  context, creator_tx, candidate.creator_tx)) {
            continue;
          }
          newest_prior_generation =
              std::max(newest_prior_generation, candidate.event_sequence);
          if (candidate.event_sequence != row.old_row_generation) continue;
          if (prior_table != nullptr) {
            return MakeInvalidRequestDiagnostic(
                "mga.relation_metadata",
                "text_migration_prior_lineage_ambiguous");
          }
          prior_table = &candidate;
        }
        if (prior_table == nullptr ||
            newest_prior_generation != row.old_row_generation ||
            prior_table->temporary || !prior_table->temporary_scope.empty() ||
            !prior_table->temporary_session_uuid.is_nil() ||
            !prior_table->on_commit_action.empty()) {
          return MakeInvalidRequestDiagnostic(
              "mga.relation_metadata",
              "text_migration_prior_lineage_missing");
        }
        const auto persisted = LoadAdmittedDescriptorFieldsSnapshot(
            descriptor_path, descriptor_lines, row.object_uuid);
        if (!persisted) {
          return MakeInvalidRequestDiagnostic(
              "mga.relation_metadata", "descriptor_store_decode_failed");
        }
        const auto prior_fields = persisted->find(row.object_uuid);
        if (prior_fields == persisted->end()) {
          return MakeInvalidRequestDiagnostic(
              "mga.relation_metadata",
              "text_migration_prior_relation_projection_missing");
        }
        auto expected_relation =
            DeserializeMgaRelationStorageDescriptor(prior_fields->second);
        if (ValidateMgaRelationStorageDescriptor(expected_relation).error ||
            expected_relation.database_uuid !=
                context.database_uuid ||
            expected_relation.relation_uuid != row.object_uuid ||
            expected_relation.relation_generation !=
                row.old_row_generation) {
          return MakeInvalidRequestDiagnostic(
              "mga.relation_metadata",
              "text_migration_prior_relation_projection_invalid");
        }
        CrudTableRecord expected_table = *prior_table;
        expected_table.creator_tx = creator_tx;
        expected_table.event_sequence = event_sequence;
        std::size_t migrated_lineage_columns = 0;
        for (auto& column : expected_relation.columns) {
          const auto prior_column = std::find_if(
              expected_table.columns.begin(), expected_table.columns.end(),
              [&](const auto& candidate) {
                return candidate.first == column.canonical_name_key;
              });
          const auto sealed_column = std::find_if(
              table.columns.begin(), table.columns.end(),
              [&](const auto& candidate) {
                return candidate.first == column.canonical_name_key;
              });
          if (prior_column == expected_table.columns.end() ||
              sealed_column == table.columns.end()) {
            return MakeInvalidRequestDiagnostic(
                "mga.relation_metadata",
                "text_migration_prior_relation_column_invalid");
          }
          const bool declared = identities.contains(
              {row.object_uuid, column.column_uuid});
          if (!declared) {
            if (column.value_descriptor.encoded_descriptor !=
                    prior_column->second ||
                prior_column->second != sealed_column->second) {
              return MakeInvalidRequestDiagnostic(
                  "mga.relation_metadata",
                  "text_migration_undeclared_column_transition");
            }
            continue;
          }
          auto migrated_table_descriptor = prior_column->second;
          if (column.column_generation != row.old_row_generation ||
              !RewriteLegacyTextDescriptor(
                  context, &migrated_table_descriptor,
                  column.column_uuid) ||
              !RewriteLegacyTextDescriptor(
                  context,
                  &column.value_descriptor.encoded_descriptor,
                  column.column_uuid) ||
              migrated_table_descriptor !=
                  column.value_descriptor.encoded_descriptor ||
              column.value_descriptor.encoded_descriptor !=
                  sealed_column->second) {
            return MakeInvalidRequestDiagnostic(
                "mga.relation_metadata",
                "text_migration_prior_relation_column_invalid");
          }
          prior_column->second = sealed_column->second;
          column.value_descriptor.descriptor_uuid =
              column.column_uuid;
          column.value_descriptor.canonical_type_name = "text";
          column.column_generation = event_sequence;
          ++migrated_lineage_columns;
        }
        expected_relation.relation_generation = event_sequence;
        const auto declared_for_object = std::count_if(
            identities.begin(), identities.end(), [&](const auto& identity) {
              return identity.first == row.object_uuid;
            });
        if (migrated_lineage_columns != declared_for_object ||
            expected_table.default_name != table.default_name ||
            expected_table.columns != table.columns ||
            expected_table.temporary != table.temporary ||
            expected_table.temporary_scope != table.temporary_scope ||
            expected_table.temporary_session_uuid !=
                table.temporary_session_uuid ||
            expected_table.on_commit_action != table.on_commit_action ||
            SerializeMgaRelationStorageDescriptor(expected_relation) !=
                base_relation_fields) {
          return MakeInvalidRequestDiagnostic(
              "mga.relation_metadata",
              "text_migration_prior_relation_transition_invalid");
        }

        const std::string table_projection =
            fields[base + 13] + "\n" + fields[base + 14] + "\n" +
            fields[base + 21] + "\n" + fields[base + 22] + "\n" +
            fields[base + 23] + "\n" + fields[base + 24];
        const auto prior_table_projection =
            table_projections.find(row.object_uuid);
        if (prior_table_projection != table_projections.end() &&
            prior_table_projection->second != table_projection) {
          return MakeInvalidRequestDiagnostic(
              "mga.relation_metadata",
              "text_migration_object_projection_conflict");
        }
        const auto prior_descriptor =
            descriptor_projections.find(row.object_uuid);
        if (prior_descriptor != descriptor_projections.end() &&
            prior_descriptor->second != fields[base + 15]) {
          return MakeInvalidRequestDiagnostic(
              "mga.relation_metadata",
              "text_migration_relation_projection_conflict");
        }
        table_projections[row.object_uuid] = table_projection;
        descriptor_projections[row.object_uuid] = fields[base + 15];
        unique_tables[row.object_uuid] = table;
        unique_descriptors[row.object_uuid] = relation_snapshot;
        request.rows.push_back(std::move(row));
        decisions.push_back(fields[base + 12]);
        relation_descriptor_snapshots.push_back(
            std::move(relation_snapshot));
        tables.push_back(std::move(table));
      }
      for (std::size_t i = 0; i < request.rows.size(); ++i) {
        const std::string expected_decision = TextMigrationDecisionHash(
            request, request.rows[i], tables[i].event_sequence, migration_transaction_uuid,
            context.datatype_catalog_snapshot_uuid, datatype_catalog_generation,
            datatype_registry_generation,
            relation_descriptor_snapshots[i]);
        if (!scratchbird::core::hash::ConstantTimeEqual(
                expected_decision, decisions[i])) {
          return MakeInvalidRequestDiagnostic(
              "mga.relation_metadata",
              "text_migration_decision_hash_mismatch");
        }
      }
      const std::string payload = CanonicalTextMigrationPayload(
          request, creator_tx, event_sequence, migration_transaction_uuid, context.datatype_catalog_snapshot_uuid,
          datatype_catalog_generation, datatype_registry_generation, tables,
          relation_descriptor_snapshots, decisions);
      if (!scratchbird::core::hash::ConstantTimeEqual(
              Sha256Tagged(payload), fields[6])) {
        return MakeInvalidRequestDiagnostic(
            "mga.relation_metadata", "text_migration_batch_hash_mismatch");
      }
      if (MetadataEventRolledBackBySavepoint(savepoints, creator_tx,
                                             event_sequence)) {
        continue;
      }
      decoded.max_event_sequence =
          std::max(decoded.max_event_sequence, event_sequence);
      for (auto& [object_uuid, table] : unique_tables) {
        (void)object_uuid;
        decoded.tables.push_back(std::move(table));
      }
      for (auto& [object_uuid, descriptor] : unique_descriptors) {
        (void)object_uuid;
        decoded.sealed_relation_descriptor_snapshots.push_back(
            std::move(descriptor));
      }
    } else if (fields[1] == "INDEX_METADATA") {
      if (fields.size() < 17) {
        return MakeInvalidRequestDiagnostic("mga.relation_metadata", "index_metadata_invalid");
      }
      CrudIndexRecord index;
      index.creator_tx = ParseU64(fields[2]);
      index.event_sequence = ParseU64(fields[3]);
      if (!ReadMetadataUuid(fields[4], &index.index_uuid)) {
        return MakeInvalidRequestDiagnostic(
            "mga.relation_metadata", "binary_metadata_uuid_invalid");
      }
      if (!ReadMetadataUuid(fields[5], &index.table_uuid)) {
        return MakeInvalidRequestDiagnostic(
            "mga.relation_metadata", "binary_metadata_uuid_invalid");
      }
      index.profile = NormalizeCrudIndexProfile(fields[6]);
      index.family = fields[7].empty() ? CrudIndexFamilyForProfile(index.profile) : fields[7];
      index.default_name = std::string(fields[8]);
      index.column_name = std::string(fields[9]);
      std::vector<std::string> key_envelopes;
      for (const auto& pair : decode_pairs(fields[10])) { key_envelopes.push_back(pair.second); }
      index.key_envelopes = std::move(key_envelopes);
      std::vector<std::string> include_columns;
      for (const auto& pair : decode_pairs(fields[11])) { include_columns.push_back(pair.second); }
      index.include_columns = std::move(include_columns);
      index.predicate_kind = fields[12];
      index.predicate_column = std::string(fields[13]);
      index.predicate_value = std::string(fields[14]);
      index.unique = fields[15] == "1";
      index.approximate = IsApproximateCrudIndexFamily(index.family);
      index.exact_fallback = index.approximate || fields[16] == "1";
      if (MetadataEventRolledBackBySavepoint(savepoints,
                                             index.creator_tx,
                                             index.event_sequence)) {
        continue;
      }
      decoded.max_event_sequence =
          std::max(decoded.max_event_sequence, ParseU64(fields[3]));
      decoded.indexes.push_back(std::move(index));
    }
  }
  if (!nested_metadata_valid) return MakeInvalidRequestDiagnostic(
      "mga.relation_metadata", "nested_binary_metadata_invalid");
  state->tables.insert(state->tables.end(),
                       decoded.tables.begin(),
                       decoded.tables.end());
  state->indexes.insert(state->indexes.end(),
                        decoded.indexes.begin(),
                        decoded.indexes.end());
  state->sealed_relation_descriptor_snapshots.insert(
      state->sealed_relation_descriptor_snapshots.end(),
      decoded.sealed_relation_descriptor_snapshots.begin(),
      decoded.sealed_relation_descriptor_snapshots.end());
  state->max_event_sequence =
      std::max(state->max_event_sequence, decoded.max_event_sequence);
  {
    const std::lock_guard<std::mutex> guard(MgaMetadataCacheMutex());
    auto& cache = MgaMetadataCache();
    cache[cache_key] =
        std::make_shared<const MgaMetadataCacheEntry>(std::move(decoded));
    // Old generations remain alive through any statement that borrowed them;
    // only the process-level lookup entry is evicted. This is copy-on-write
    // publication with bounded cache residency, not in-place mutation.
    constexpr std::size_t kMaximumMgaMetadataGenerations = 128;
    while (cache.size() > kMaximumMgaMetadataGenerations) {
      cache.erase(cache.begin());
    }
  }
  return OkDiagnostic();
}


MgaMetadataSnapshotLoadResult LoadMgaMetadataSnapshot(
    const EngineRequestContext& context) {
  MgaMetadataSnapshotLoadResult result;
  const std::string metadata_path = MetadataStorePath(context);
  const std::string savepoint_path = SavepointStorePath(context);
  const std::string descriptor_path = DescriptorStorePath(context);
  // A previously decoded generation cannot turn current I/O failure into
  // authoritative metadata. Validate all three complete streams before cache use.
  const auto metadata_lines = ReadMetadataRecords(metadata_path);
  const auto savepoint_lines = ReadSavepointBytes(savepoint_path);
  const auto descriptor_lines = ReadContent(descriptor_path, true);
  if (!metadata_lines.ok || !savepoint_lines.ok || !descriptor_lines.ok) {
    result.diagnostic = MakeInvalidRequestDiagnostic("mga.relation_metadata",
        !metadata_lines.ok ? "metadata_store_read_failed" :
        !savepoint_lines.ok ? "savepoint_store_read_failed" : "descriptor_store_read_failed");
    return result;
  }
  const auto metadata_identity = metadata_lines.identity;
  const auto savepoint_identity = savepoint_lines.identity;
  const auto savepoints = ParseSavepointBytes(context, savepoint_lines.binary_bytes);
  if (savepoints.diagnostic.error) {
    result.diagnostic = savepoints.diagnostic;
    return result;
  }
  const auto savepoint_digest = MetadataSavepointCacheDigest(savepoint_lines, savepoints);
  if (!savepoint_digest.ok()) {
    result.diagnostic = MakeInvalidRequestDiagnostic(
        "mga.relation_metadata", "savepoint_cache_digest_failed");
    return result;
  }
  result.key = MgaMetadataCacheKey{
      context.database_uuid,
      metadata_path,
      metadata_identity.ok ? metadata_identity.file_size : 0,
      metadata_identity.ok ? metadata_identity.file_mtime_ticks : 0,
      savepoint_path,
      savepoint_identity.ok ? savepoint_identity.file_size : 0,
      savepoint_identity.ok ? savepoint_identity.file_mtime_ticks : 0,
      context.local_transaction_id,metadata_lines.content_sha256,savepoint_digest.digest,
      descriptor_path,descriptor_lines.content_sha256};
  {
    const std::lock_guard<std::mutex> guard(MgaMetadataCacheMutex());
    const auto cached = MgaMetadataCache().find(result.key);
    if (cached != MgaMetadataCache().end() && cached->second != nullptr) {
      result.snapshot = cached->second;
      result.diagnostic = OkDiagnostic();
      return result;
    }
  }

  // The compatibility decoder remains the single canonical parser for the
  // metadata/savepoint streams.  A cold generation is decoded once and then
  // published as an immutable shared snapshot; this temporary state is never
  // retained by statement consumers.
  CrudState compatibility_state;
  result.diagnostic = LoadMgaMetadata(&compatibility_state, context);
  if (result.diagnostic.error) return result;
  {
    const std::lock_guard<std::mutex> guard(MgaMetadataCacheMutex());
    const auto cached = MgaMetadataCache().find(result.key);
    if (cached != MgaMetadataCache().end()) result.snapshot = cached->second;
  }
  if (result.snapshot == nullptr) {
    result.diagnostic = MakeInvalidRequestDiagnostic(
        "mga.relation_metadata", "immutable_snapshot_publication_failed");
  }
  return result;
}


}  // namespace scratchbird::engine::internal_api
