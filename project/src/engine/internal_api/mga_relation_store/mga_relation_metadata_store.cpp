// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "mga_relation_store/mga_relation_metadata_store.hpp"
#include "mga_relation_store/mga_contextual_text_descriptor.hpp"
#include "mga_relation_store/mga_relation_store_internal_support.hpp"

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

// SEARCH_KEY: SB_ENGINE_MGA_RELATION_METADATA_STORE_IMPLEMENTATION_AUTHORITY
// Owns persisted relation-metadata and descriptor-field decoding, immutable
// generation caches, and raw metadata snapshots. Savepoint exclusion is
// supplied by the canonical relation/MGA authority and is never inferred here.

constexpr const char* kRowStoreMagic = "SBMGA1";
constexpr const char* kDescriptorMagic = "SBMGADESC1";
constexpr std::string_view kLineHexFieldPrefix = "SBHEX:";
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

std::string JoinLine(const std::vector<std::string>& fields) {
  std::string line;
  for (std::size_t index = 0; index < fields.size(); ++index) {
    if (index != 0) line.push_back('\t');
    line += fields[index];
  }
  return line;
}

std::vector<std::string> ReadLines(const std::string& path) {
  std::vector<std::string> lines;
  std::ifstream input(path, std::ios::binary);
  std::string line;
  while (std::getline(input, line)) lines.push_back(std::move(line));
  return lines;
}

bool AppendLine(const std::string& path, const std::string& line) {
  std::ofstream output(path, std::ios::app | std::ios::binary);
  if (!output) return false;
  output << line << '\n';
  output.flush();
  return static_cast<bool>(output);
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

int HexValue(const char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return 10 + value - 'a';
  if (value >= 'A' && value <= 'F') return 10 + value - 'A';
  return -1;
}

std::string DecodeCrudTextLocal(const std::string& encoded) {
  if ((encoded.size() % 2) != 0) return {};
  std::string decoded;
  decoded.reserve(encoded.size() / 2);
  for (std::size_t index = 0; index < encoded.size(); index += 2) {
    const int high = HexValue(encoded[index]);
    const int low = HexValue(encoded[index + 1]);
    if (high < 0 || low < 0) return {};
    decoded.push_back(static_cast<char>((high << 4) | low));
  }
  return decoded;
}

std::string DecodeLineHexFieldOrRaw(const std::string& field) {
  if (field.rfind(kLineHexFieldPrefix, 0) != 0) return field;
  return DecodeCrudTextLocal(field.substr(kLineHexFieldPrefix.size()));
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

bool MetadataEventRolledBackBySavepoint(
    const std::function<bool(std::uint64_t, std::uint64_t)>& predicate,
    const std::uint64_t creator_tx,
    const std::uint64_t event_sequence) {
  return predicate && predicate(creator_tx, event_sequence);
}

std::vector<std::pair<std::string, std::string>> DecodeCrudPairsWithKeyCache(
    const std::string& encoded,
    std::unordered_map<std::string, std::string>* decoded_key_cache) {
  std::vector<std::pair<std::string, std::string>> pairs;
  std::size_t start = 0;
  while (start <= encoded.size()) {
    const std::size_t end = encoded.find(',', start);
    const std::size_t part_end =
        end == std::string::npos ? encoded.size() : end;
    const std::size_t equals = encoded.find('=', start);
    if (equals != std::string::npos && equals < part_end) {
      const std::string encoded_key = encoded.substr(start, equals - start);
      std::string key;
      if (decoded_key_cache != nullptr) {
        auto found = decoded_key_cache->find(encoded_key);
        if (found == decoded_key_cache->end()) {
          found = decoded_key_cache
                      ->emplace(encoded_key, DecodeCrudTextLocal(encoded_key))
                      .first;
        }
        key = found->second;
      } else {
        key = DecodeCrudTextLocal(encoded_key);
      }
      pairs.emplace_back(
          std::move(key),
          DecodeCrudTextLocal(
              encoded.substr(equals + 1, part_end - equals - 1)));
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return pairs;
}

}  // namespace

std::uint64_t ChecksumText(const std::string& value) {
  std::uint64_t checksum = 1469598103934665603ull;
  for (unsigned char c : value) {
    checksum ^= static_cast<std::uint64_t>(c);
    checksum *= 1099511628211ull;
  }
  return checksum;
}

void AppendCanonicalBatchField(std::string* out,
                               std::string_view key,
                               std::string_view value) {
  if (out == nullptr) return;
  out->append(std::to_string(key.size()));
  out->push_back(':');
  out->append(key);
  out->append(std::to_string(value.size()));
  out->push_back(':');
  out->append(value);
}

std::string CanonicalConstraintMutationBatchPayload(
    const MgaConstraintMutationBatch& batch,
    std::uint64_t creator_local_transaction_id,
    std::uint64_t metadata_event_sequence) {
  std::string payload;
  auto field = [&](std::string_view key, std::string_view value) {
    AppendCanonicalBatchField(&payload, key, value);
  };
  field("format_version", batch.format_version);
  field("seal_state", "sealed");
  field("creator_local_transaction_id",
        std::to_string(creator_local_transaction_id));
  // MGA savepoint rollback and metadata ordering both depend on this value;
  // bind it into the seal so a batch cannot be replayed at another event.
  field("metadata_event_sequence", std::to_string(metadata_event_sequence));
  field("batch_uuid", batch.batch_uuid);
  field("mutation_count", std::to_string(batch.mutation_count));
  field("database_uuid", batch.database_uuid);
  field("constraint_uuid", batch.constraint_uuid);
  field("owner_table_uuid", batch.owner_table_uuid);
  field("child_schema_uuid", batch.child_schema_uuid);
  field("child_relation_descriptor_uuid",
        batch.child_relation_descriptor_uuid);
  field("child_relation_descriptor_generation",
        std::to_string(batch.child_relation_descriptor_generation));
  field("child_column_uuid", batch.child_column_uuid);
  field("parent_table_uuid", batch.parent_table_uuid);
  field("parent_schema_uuid", batch.parent_schema_uuid);
  field("parent_relation_descriptor_uuid",
        batch.parent_relation_descriptor_uuid);
  field("parent_relation_descriptor_generation",
        std::to_string(batch.parent_relation_descriptor_generation));
  field("parent_column_uuid", batch.parent_column_uuid);
  field("parent_candidate_key_constraint_uuid",
        batch.parent_candidate_key_constraint_uuid);
  field("key_descriptor_uuid", batch.key_descriptor_uuid);
  field("support_uuid", batch.support_uuid);
  field("support_family", batch.support_family);
  field("support_policy", batch.support_policy);
  field("match_policy", batch.match_policy);
  field("on_update_action", batch.on_update_action);
  field("on_delete_action", batch.on_delete_action);
  field("enforcement_timing", batch.enforcement_timing);
  field("constraint_metadata_generation",
        std::to_string(batch.constraint_metadata_generation));
  field("base_table_event_sequence",
        std::to_string(batch.base_table_event_sequence));
  field("parent_base_table_event_sequence",
        std::to_string(batch.parent_base_table_event_sequence));
  field("constraint_name", batch.constraint_name);
  field("constraint_kind", batch.constraint_kind);
  field("canonical_constraint_envelope",
        batch.canonical_constraint_envelope);
  field("updated_table_uuid", batch.updated_table.table_uuid);
  field("updated_table_default_name", batch.updated_table.default_name);
  field("updated_table_columns", EncodeCrudPairs(batch.updated_table.columns));
  field("updated_table_temporary",
        batch.updated_table.temporary ? "true" : "false");
  field("updated_table_temporary_scope", batch.updated_table.temporary_scope);
  field("updated_table_temporary_session_uuid",
        batch.updated_table.temporary_session_uuid);
  field("updated_table_on_commit_action", batch.updated_table.on_commit_action);
  return payload;
}

std::string ConstraintMutationBatchSha256(
    const MgaConstraintMutationBatch& batch,
    std::uint64_t creator_local_transaction_id,
    std::uint64_t metadata_event_sequence) {
  const std::string payload = CanonicalConstraintMutationBatchPayload(
      batch, creator_local_transaction_id, metadata_event_sequence);
  const auto* bytes = reinterpret_cast<
      const scratchbird::core::platform::byte*>(payload.data());
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(
      bytes, payload.size());
  if (!digest.ok() ||
      digest.digest_bytes != scratchbird::core::hash::kSha256DigestBytes) {
    return {};
  }
  return "sha256:" + scratchbird::core::hash::HexLower(digest.digest);
}

std::string CanonicalBigintMigrationPayload(
    const MgaBigintIdentityMigrationRequest& request,
    std::uint64_t creator_tx,
    std::uint64_t event_sequence,
    std::string_view transaction_uuid,
    const std::vector<CrudTableRecord>& tables,
    const std::vector<std::string>& decision_hashes) {
  std::string payload;
  auto field = [&](std::string_view key, std::string_view value) {
    AppendCanonicalBatchField(&payload, key, value);
  };
  field("format_version", kBigintMigrationFormat);
  field("seal_state", "sealed");
  field("migration_id", request.migration_id);
  field("creator_tx", std::to_string(creator_tx));
  field("event_sequence", std::to_string(event_sequence));
  field("transaction_uuid", transaction_uuid);
  field("prior_catalog_snapshot_uuid", request.prior_catalog_snapshot_uuid);
  field("new_catalog_snapshot_uuid", request.new_catalog_snapshot_uuid);
  field("prior_catalog_generation",
        std::to_string(request.prior_catalog_generation));
  field("new_catalog_generation",
        std::to_string(request.new_catalog_generation));
  field("mutation_count", std::to_string(request.rows.size()));
  for (std::size_t i = 0; i < request.rows.size(); ++i) {
    const auto& row = request.rows[i];
    const auto& table = tables[i];
    field("object_uuid", row.object_uuid);
    field("column_uuid", row.column_uuid);
    field("old_type_uuid", kLegacyBigintTypeUuid);
    field("new_type_uuid", kCanonicalBigintTypeUuid);
    field("old_row_generation", std::to_string(row.old_row_generation));
    field("new_row_generation", std::to_string(table.event_sequence));
    field("decision_sha256", decision_hashes[i]);
    field("table_default_name", table.default_name);
    field("table_columns", EncodeCrudPairs(table.columns));
  }
  return payload;
}

std::string Sha256Tagged(std::string_view payload) {
  const auto* bytes = reinterpret_cast<const scratchbird::core::platform::byte*>(
      payload.data());
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(
      bytes, payload.size());
  if (!digest.ok() ||
      digest.digest_bytes != scratchbird::core::hash::kSha256DigestBytes) {
    return {};
  }
  return "sha256:" + scratchbird::core::hash::HexLower(digest.digest);
}

std::string BigintMigrationDecisionHash(
    const MgaBigintIdentityMigrationRequest& request,
    const MgaBigintIdentityMigrationRow& row,
    std::uint64_t new_row_generation,
    std::string_view transaction_uuid) {
  std::string payload;
  auto field = [&](std::string_view key, std::string_view value) {
    AppendCanonicalBatchField(&payload, key, value);
  };
  field("migration_id", request.migration_id);
  field("transaction_uuid", transaction_uuid);
  field("prior_catalog_snapshot_uuid", request.prior_catalog_snapshot_uuid);
  field("new_catalog_snapshot_uuid", request.new_catalog_snapshot_uuid);
  field("prior_catalog_generation", std::to_string(request.prior_catalog_generation));
  field("new_catalog_generation", std::to_string(request.new_catalog_generation));
  field("object_uuid", row.object_uuid);
  field("column_uuid", row.column_uuid);
  field("old_type_uuid", kLegacyBigintTypeUuid);
  field("new_type_uuid", kCanonicalBigintTypeUuid);
  field("old_row_generation", std::to_string(row.old_row_generation));
  field("new_row_generation", std::to_string(new_row_generation));
  return Sha256Tagged(payload);
}

std::string CanonicalInt32MigrationPayload(
    const MgaInt32IdentityMigrationRequest& request,
    std::uint64_t creator_tx,
    std::uint64_t event_sequence,
    std::string_view transaction_uuid,
    const std::vector<CrudTableRecord>& tables,
    const std::vector<std::string>& decision_hashes) {
  std::string payload;
  auto field = [&](std::string_view key, std::string_view value) {
    AppendCanonicalBatchField(&payload, key, value);
  };
  field("format_version", kInt32MigrationFormat);
  field("seal_state", "sealed");
  field("migration_id", request.migration_id);
  field("creator_tx", std::to_string(creator_tx));
  field("event_sequence", std::to_string(event_sequence));
  field("transaction_uuid", transaction_uuid);
  field("prior_catalog_snapshot_uuid", request.prior_catalog_snapshot_uuid);
  field("new_catalog_snapshot_uuid", request.new_catalog_snapshot_uuid);
  field("prior_catalog_generation",
        std::to_string(request.prior_catalog_generation));
  field("new_catalog_generation",
        std::to_string(request.new_catalog_generation));
  field("mutation_count", std::to_string(request.rows.size()));
  for (std::size_t i = 0; i < request.rows.size(); ++i) {
    const auto& row = request.rows[i];
    const auto& table = tables[i];
    field("object_uuid", row.object_uuid);
    field("column_uuid", row.column_uuid);
    field("old_descriptor_uuid", kLegacyInt32DescriptorUuid);
    field("new_descriptor_uuid", kCanonicalInt32DescriptorUuid);
    field("old_type_uuid", kLegacyInt32TypeUuid);
    field("new_type_uuid", kCanonicalInt32TypeUuid);
    field("old_row_generation", std::to_string(row.old_row_generation));
    field("new_row_generation", std::to_string(table.event_sequence));
    field("decision_sha256", decision_hashes[i]);
    field("table_default_name", table.default_name);
    field("table_columns", EncodeCrudPairs(table.columns));
  }
  return payload;
}

std::string Int32MigrationDecisionHash(
    const MgaInt32IdentityMigrationRequest& request,
    const MgaInt32IdentityMigrationRow& row,
    std::uint64_t new_row_generation,
    std::string_view transaction_uuid) {
  std::string payload;
  auto field = [&](std::string_view key, std::string_view value) {
    AppendCanonicalBatchField(&payload, key, value);
  };
  field("migration_id", request.migration_id);
  field("transaction_uuid", transaction_uuid);
  field("prior_catalog_snapshot_uuid", request.prior_catalog_snapshot_uuid);
  field("new_catalog_snapshot_uuid", request.new_catalog_snapshot_uuid);
  field("prior_catalog_generation",
        std::to_string(request.prior_catalog_generation));
  field("new_catalog_generation",
        std::to_string(request.new_catalog_generation));
  field("object_uuid", row.object_uuid);
  field("column_uuid", row.column_uuid);
  field("old_descriptor_uuid", kLegacyInt32DescriptorUuid);
  field("new_descriptor_uuid", kCanonicalInt32DescriptorUuid);
  field("old_type_uuid", kLegacyInt32TypeUuid);
  field("new_type_uuid", kCanonicalInt32TypeUuid);
  field("old_row_generation", std::to_string(row.old_row_generation));
  field("new_row_generation", std::to_string(new_row_generation));
  return Sha256Tagged(payload);
}

std::string CanonicalTextMigrationPayload(
    const MgaTextIdentityMigrationRequest& request,
    std::uint64_t creator_tx,
    std::uint64_t event_sequence,
    std::string_view transaction_uuid,
    std::string_view datatype_catalog_snapshot_uuid,
    std::uint64_t datatype_catalog_generation,
    std::uint64_t datatype_registry_generation,
    const std::vector<CrudTableRecord>& tables,
    const std::vector<CrudSealedRelationDescriptorSnapshot>&
        relation_descriptor_snapshots,
    const std::vector<std::string>& decision_hashes) {
  std::string payload;
  auto field = [&](std::string_view key, std::string_view value) {
    AppendCanonicalBatchField(&payload, key, value);
  };
  field("format_version", kTextMigrationFormat);
  field("seal_state", "sealed");
  field("migration_id", request.migration_id);
  field("creator_tx", std::to_string(creator_tx));
  field("event_sequence", std::to_string(event_sequence));
  field("transaction_uuid", transaction_uuid);
  field("datatype_catalog_snapshot_uuid", datatype_catalog_snapshot_uuid);
  field("datatype_catalog_generation",
        std::to_string(datatype_catalog_generation));
  field("datatype_registry_generation",
        std::to_string(datatype_registry_generation));
  field("prior_catalog_snapshot_uuid", request.prior_catalog_snapshot_uuid);
  field("new_catalog_snapshot_uuid", request.new_catalog_snapshot_uuid);
  field("prior_catalog_generation",
        std::to_string(request.prior_catalog_generation));
  field("new_catalog_generation",
        std::to_string(request.new_catalog_generation));
  field("mutation_count", std::to_string(request.rows.size()));
  for (std::size_t i = 0; i < request.rows.size(); ++i) {
    const auto& row = request.rows[i];
    const auto& table = tables[i];
    field("object_uuid", row.object_uuid);
    field("column_uuid", row.column_uuid);
    field("old_descriptor_uuid", kLegacyTextDescriptorUuid);
    field("new_descriptor_uuid", kCanonicalTextDescriptorUuid);
    field("old_type_uuid", kLegacyTextTypeUuid);
    field("new_type_uuid", kCanonicalTextTypeUuid);
    field("new_codec_uuid", kCanonicalTextCodecUuid);
    field("new_codec_id", kCanonicalTextCodecId);
    field("new_codec_version", "1");
    field("new_codec_generation", "1");
    field("old_row_generation", std::to_string(row.old_row_generation));
    field("new_row_generation", std::to_string(table.event_sequence));
    field("decision_sha256", decision_hashes[i]);
    field("table_default_name", table.default_name);
    field("table_columns", EncodeCrudPairs(table.columns));
    const auto& snapshot = relation_descriptor_snapshots[i];
    field("relation_descriptor_uuid", snapshot.relation_descriptor_uuid);
    field("relation_descriptor_generation",
          std::to_string(snapshot.relation_descriptor_generation));
    field("descriptor_field_count",
          std::to_string(snapshot.descriptor_field_count));
    field("descriptor_field_bytes",
          std::to_string(snapshot.descriptor_field_bytes));
    field("contextual_sidecar_count",
          std::to_string(snapshot.contextual_sidecar_count));
    field("relation_descriptor_fields",
          EncodeCrudPairs(snapshot.descriptor_fields));
  }
  return payload;
}

std::string TextMigrationDecisionHash(
    const MgaTextIdentityMigrationRequest& request,
    const MgaTextIdentityMigrationRow& row,
    std::uint64_t new_row_generation,
    std::string_view transaction_uuid,
    std::string_view datatype_catalog_snapshot_uuid,
    std::uint64_t datatype_catalog_generation,
    std::uint64_t datatype_registry_generation,
    const CrudSealedRelationDescriptorSnapshot& relation_snapshot) {
  std::string payload;
  auto field = [&](std::string_view key, std::string_view value) {
    AppendCanonicalBatchField(&payload, key, value);
  };
  field("migration_id", request.migration_id);
  field("transaction_uuid", transaction_uuid);
  field("datatype_catalog_snapshot_uuid", datatype_catalog_snapshot_uuid);
  field("datatype_catalog_generation",
        std::to_string(datatype_catalog_generation));
  field("datatype_registry_generation",
        std::to_string(datatype_registry_generation));
  field("prior_catalog_snapshot_uuid", request.prior_catalog_snapshot_uuid);
  field("new_catalog_snapshot_uuid", request.new_catalog_snapshot_uuid);
  field("prior_catalog_generation",
        std::to_string(request.prior_catalog_generation));
  field("new_catalog_generation",
        std::to_string(request.new_catalog_generation));
  field("object_uuid", row.object_uuid);
  field("column_uuid", row.column_uuid);
  field("old_descriptor_uuid", kLegacyTextDescriptorUuid);
  field("new_descriptor_uuid", kCanonicalTextDescriptorUuid);
  field("old_type_uuid", kLegacyTextTypeUuid);
  field("new_type_uuid", kCanonicalTextTypeUuid);
  field("new_codec_uuid", kCanonicalTextCodecUuid);
  field("new_codec_id", kCanonicalTextCodecId);
  field("new_codec_version", "1");
  field("new_codec_generation", "1");
  field("old_row_generation", std::to_string(row.old_row_generation));
  field("new_row_generation", std::to_string(new_row_generation));
  field("relation_descriptor_uuid",
        relation_snapshot.relation_descriptor_uuid);
  field("relation_descriptor_generation",
        std::to_string(relation_snapshot.relation_descriptor_generation));
  field("descriptor_field_count",
        std::to_string(relation_snapshot.descriptor_field_count));
  field("descriptor_field_bytes",
        std::to_string(relation_snapshot.descriptor_field_bytes));
  field("contextual_sidecar_count",
        std::to_string(relation_snapshot.contextual_sidecar_count));
  field("relation_descriptor_fields",
        EncodeCrudPairs(relation_snapshot.descriptor_fields));
  return Sha256Tagged(payload);
}

bool ValidConstraintBatchUuid(
    std::string_view value,
    scratchbird::core::platform::UuidKind kind) {
  if (value.empty()) return false;
  return scratchbird::core::uuid::ParseDurableEngineIdentityUuid(
             kind, std::string(value))
      .ok();
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
      batch.batch_uuid,
      "sealed",
      batch.batch_hash,
      std::to_string(batch.mutation_count),
      batch.database_uuid,
      batch.constraint_uuid,
      batch.owner_table_uuid,
      batch.child_schema_uuid,
      batch.child_relation_descriptor_uuid,
      std::to_string(batch.child_relation_descriptor_generation),
      batch.child_column_uuid,
      batch.parent_table_uuid,
      batch.parent_schema_uuid,
      batch.parent_relation_descriptor_uuid,
      std::to_string(batch.parent_relation_descriptor_generation),
      batch.parent_column_uuid,
      batch.parent_candidate_key_constraint_uuid,
      batch.key_descriptor_uuid,
      batch.support_uuid,
      batch.support_family,
      batch.support_policy,
      batch.match_policy,
      batch.on_update_action,
      batch.on_delete_action,
      batch.enforcement_timing,
      std::to_string(batch.constraint_metadata_generation),
      std::to_string(batch.base_table_event_sequence),
      std::to_string(batch.parent_base_table_event_sequence),
      EncodeCrudText(batch.constraint_name),
      batch.constraint_kind,
      EncodeCrudText(batch.canonical_constraint_envelope),
      table.table_uuid,
      EncodeCrudText(table.default_name),
      EncodeCrudPairs(table.columns),
      table.temporary ? "1" : "0",
      table.temporary_scope,
      table.temporary_session_uuid,
      table.on_commit_action};
  return fields;
}

struct DescriptorFieldsCacheRecord {
  std::uintmax_t file_size = 0;
  std::int64_t file_mtime_ticks = 0;
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

std::uintmax_t ExistingFileSize(const std::string& path) {
  std::error_code ignored;
  if (path.empty() || !std::filesystem::exists(path, ignored)) {
    return 0;
  }
  return std::filesystem::file_size(path, ignored);
}

MetadataStoreFileIdentity ExistingFileIdentity(const std::string& path) {
  std::error_code ignored;
  if (path.empty() || !std::filesystem::exists(path, ignored)) {
    return {};
  }
  return MetadataStoreTextFileIdentity(path);
}

std::shared_ptr<const DescriptorFieldsByRelation>
LoadDescriptorFieldsSnapshot(
    const EngineRequestContext& context,
    std::string_view required_relation_uuid) {
  const std::string path = DescriptorStorePath(context);
  const auto identity = ExistingFileIdentity(path);
  const std::uintmax_t file_size = identity.ok ? identity.file_size : 0;
  const std::int64_t file_mtime_ticks =
      identity.ok ? identity.file_mtime_ticks : 0;
  {
    const std::lock_guard<std::mutex> guard(DescriptorFieldsCacheMutex());
    const auto cached = DescriptorFieldsCache().find(path);
    if (cached != DescriptorFieldsCache().end() &&
        cached->second.file_size == file_size &&
        cached->second.file_mtime_ticks == file_mtime_ticks &&
        cached->second.descriptors != nullptr &&
        (required_relation_uuid.empty() ||
         cached->second.descriptors->contains(
             std::string(required_relation_uuid)))) {
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
  for (const auto& line : ReadLines(path)) {
    const auto fields = SplitTabs(line);
    if (fields.size() < 4 || fields[0] != kDescriptorMagic || fields[1] != "RELATION") { continue; }
    descriptors[fields[2]] = DecodeCrudPairs(fields[3]);
  }
  {
    const std::lock_guard<std::mutex> guard(DescriptorFieldsCacheMutex());
    auto immutable =
        std::make_shared<const DescriptorFieldsByRelation>(std::move(descriptors));
    DescriptorFieldsCache()[path] = {file_size, file_mtime_ticks, immutable};
    return immutable;
  }
}

DescriptorFieldsByRelation LoadDescriptorFieldsByRelation(
    const EngineRequestContext& context,
    std::string_view required_relation_uuid) {
  const auto snapshot =
      LoadDescriptorFieldsSnapshot(context, required_relation_uuid);
  return snapshot == nullptr ? DescriptorFieldsByRelation{} : *snapshot;
}

EngineApiDiagnostic PersistDescriptorFields(const EngineRequestContext& context,
                                            const std::string& relation_uuid,
                                            const std::vector<std::pair<std::string, std::string>>& fields) {
  if (context.database_path.empty()) {
    return MakeInvalidRequestDiagnostic("mga.relation_descriptor", "database_path_required");
  }
  const std::string line = JoinLine({kDescriptorMagic, "RELATION", relation_uuid, EncodeCrudPairs(fields)});
  const std::string path = DescriptorStorePath(context);
  if (!AppendLine(path, line)) {
    return MakeInvalidRequestDiagnostic("mga.relation_descriptor", "descriptor_store_append_failed");
  }
  {
    const std::lock_guard<std::mutex> guard(DescriptorFieldsCacheMutex());
    auto cached = DescriptorFieldsCache().find(path);
    if (cached != DescriptorFieldsCache().end()) {
      const auto updated_identity = ExistingFileIdentity(path);
      auto updated = std::make_shared<DescriptorFieldsByRelation>(
          cached->second.descriptors == nullptr
              ? DescriptorFieldsByRelation{}
              : *cached->second.descriptors);
      (*updated)[relation_uuid] = fields;
      cached->second.descriptors = std::move(updated);
      cached->second.file_size =
          updated_identity.ok ? updated_identity.file_size : 0;
      cached->second.file_mtime_ticks =
          updated_identity.ok ? updated_identity.file_mtime_ticks : 0;
    }
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
  const auto metadata_identity = ExistingFileIdentity(metadata_path);
  const auto savepoint_identity = ExistingFileIdentity(savepoint_path);
  const MgaMetadataCacheKey cache_key{
      context.database_uuid.canonical,
      metadata_path,
      metadata_identity.ok ? metadata_identity.file_size : 0,
      metadata_identity.ok ? metadata_identity.file_mtime_ticks : 0,
      savepoint_path,
      savepoint_identity.ok ? savepoint_identity.file_size : 0,
      savepoint_identity.ok ? savepoint_identity.file_mtime_ticks : 0,
      context.local_transaction_id};
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
  const auto savepoints =
      MakeMgaMetadataRollbackPredicateForStoreModule(context);
  MgaMetadataCacheEntry decoded;
  for (const auto& line : ReadLines(metadata_path)) {
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
      if (table.temporary && !table.table_uuid.empty()) {
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
      table.table_uuid = fields[stf::kTableUuid];
      table.default_name = DecodeCrudTextLocal(fields[stf::kDefaultName]);
      table.columns = DecodeCrudPairs(fields[stf::kColumns]);
      table.temporary = fields[stf::kTemporary] == "1";
      table.temporary_scope = fields[stf::kTemporaryScope];
      table.temporary_session_uuid =
          fields[stf::kTemporarySessionUuid];
      table.on_commit_action = fields[stf::kOnCommitAction];
      if (table.temporary && !table.table_uuid.empty()) {
        decoded.known_temporary_relation_uuids.insert(table.table_uuid);
      }
      const auto complete_fields =
          DecodeCrudPairs(fields[stf::kDescriptorFields]);
      if (EncodeCrudText(table.default_name) !=
              fields[stf::kDefaultName] ||
          EncodeCrudPairs(table.columns) != fields[stf::kColumns] ||
          EncodeCrudPairs(complete_fields) !=
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
          descriptor.database_uuid.canonical !=
              context.database_uuid.canonical ||
          descriptor.relation_uuid.canonical != table.table_uuid ||
          descriptor.relation_generation != event_sequence ||
          descriptor.descriptor_uuid.canonical !=
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
      snapshot.relation_descriptor_uuid =
          fields[stf::kRelationDescriptorUuid];
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
          fields[cbf::kDatabaseUuid] != context.database_uuid.canonical) {
        return MakeInvalidRequestDiagnostic(
            "mga.relation_metadata", "constraint_mutation_batch_invalid");
      }
      MgaConstraintMutationBatch batch;
      batch.format_version = fields[cbf::kFormatVersion];
      batch.batch_uuid = fields[cbf::kBatchUuid];
      batch.batch_hash = fields[cbf::kBatchHash];
      batch.mutation_count = static_cast<std::uint32_t>(
          ParseU64(fields[cbf::kMutationCount]));
      batch.database_uuid = fields[cbf::kDatabaseUuid];
      batch.constraint_uuid = fields[cbf::kConstraintUuid];
      batch.owner_table_uuid = fields[cbf::kOwnerTableUuid];
      batch.child_schema_uuid = fields[cbf::kChildSchemaUuid];
      batch.child_relation_descriptor_uuid = fields[cbf::kChildDescriptorUuid];
      batch.child_relation_descriptor_generation =
          ParseU64(fields[cbf::kChildDescriptorGeneration]);
      batch.child_column_uuid = fields[cbf::kChildColumnUuid];
      batch.parent_table_uuid = fields[cbf::kParentTableUuid];
      batch.parent_schema_uuid = fields[cbf::kParentSchemaUuid];
      batch.parent_relation_descriptor_uuid = fields[cbf::kParentDescriptorUuid];
      batch.parent_relation_descriptor_generation =
          ParseU64(fields[cbf::kParentDescriptorGeneration]);
      batch.parent_column_uuid = fields[cbf::kParentColumnUuid];
      batch.parent_candidate_key_constraint_uuid =
          fields[cbf::kParentCandidateConstraintUuid];
      batch.key_descriptor_uuid = fields[cbf::kReferencedKeyDescriptorUuid];
      batch.support_uuid = fields[cbf::kSupportUuid];
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
      batch.constraint_name = DecodeCrudTextLocal(fields[cbf::kConstraintName]);
      batch.constraint_kind = fields[cbf::kConstraintKind];
      batch.canonical_constraint_envelope =
          DecodeCrudTextLocal(fields[cbf::kCanonicalEnvelope]);
      CrudTableRecord table;
      table.creator_tx = ParseU64(fields[cbf::kCreatorTx]);
      table.event_sequence = ParseU64(fields[cbf::kEventSequence]);
      table.table_uuid = fields[cbf::kTableUuid];
      table.default_name = DecodeCrudTextLocal(fields[cbf::kTableDefaultName]);
      table.columns = DecodeCrudPairs(fields[cbf::kTableColumns]);
      table.temporary = fields[cbf::kTableTemporary] == "1";
      table.temporary_scope = fields[cbf::kTableTemporaryScope];
      table.temporary_session_uuid = fields[cbf::kTableTemporarySessionUuid];
      table.on_commit_action = fields[cbf::kTableOnCommitAction];
      if (table.temporary || !table.temporary_scope.empty() ||
          !table.temporary_session_uuid.empty() ||
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
      MgaBigintIdentityMigrationRequest request;
      request.migration_id = fields[7];
      request.prior_catalog_snapshot_uuid = fields[9];
      request.new_catalog_snapshot_uuid = fields[10];
      request.prior_catalog_generation = ParseU64(fields[11]);
      request.new_catalog_generation = ParseU64(fields[12]);
      std::vector<CrudTableRecord> tables;
      std::vector<std::string> decisions;
      std::set<std::pair<std::string, std::string>> identities;
      for (std::size_t i = 0; i < mutation_count; ++i) {
        const std::size_t base = kHeaderFields + i * kFieldsPerRow;
        MgaBigintIdentityMigrationRow row;
        row.object_uuid = fields[base];
        row.column_uuid = fields[base + 1];
        row.old_row_generation = ParseU64(fields[base + 4]);
        if (fields[base + 2] != kLegacyBigintTypeUuid ||
            fields[base + 3] != kCanonicalBigintTypeUuid ||
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
        table.table_uuid = row.object_uuid;
        table.default_name = DecodeCrudTextLocal(fields[base + 7]);
        table.columns = DecodeCrudPairs(fields[base + 8]);
        table.temporary = fields[base + 9] == "1";
        table.temporary_scope = fields[base + 10];
        table.temporary_session_uuid = fields[base + 11];
        table.on_commit_action = fields[base + 12];
        if (table.temporary || !table.temporary_scope.empty() ||
            !table.temporary_session_uuid.empty() ||
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
            request, request.rows[i], tables[i].event_sequence, fields[8]);
        if (!scratchbird::core::hash::ConstantTimeEqual(
                expected_decision, decisions[i])) {
          return MakeInvalidRequestDiagnostic(
              "mga.relation_metadata",
              "bigint_migration_decision_hash_mismatch");
        }
      }
      const std::string payload = CanonicalBigintMigrationPayload(
          request, creator_tx, event_sequence, fields[8], tables, decisions);
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
      MgaInt32IdentityMigrationRequest request;
      request.migration_id = fields[7];
      request.prior_catalog_snapshot_uuid = fields[9];
      request.new_catalog_snapshot_uuid = fields[10];
      request.prior_catalog_generation = ParseU64(fields[11]);
      request.new_catalog_generation = ParseU64(fields[12]);
      std::vector<CrudTableRecord> tables;
      std::vector<std::string> decisions;
      std::set<std::pair<std::string, std::string>> identities;
      std::map<std::string, std::string> table_projections;
      std::map<std::string, CrudTableRecord> unique_tables;
      for (std::size_t i = 0; i < mutation_count; ++i) {
        const std::size_t base = kHeaderFields + i * kFieldsPerRow;
        MgaInt32IdentityMigrationRow row;
        row.object_uuid = fields[base];
        row.column_uuid = fields[base + 1];
        row.old_row_generation = ParseU64(fields[base + 6]);
        if (fields[base + 2] != kLegacyInt32DescriptorUuid ||
            fields[base + 3] != kCanonicalInt32DescriptorUuid ||
            fields[base + 4] != kLegacyInt32TypeUuid ||
            fields[base + 5] != kCanonicalInt32TypeUuid ||
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
        table.table_uuid = row.object_uuid;
        table.default_name = DecodeCrudTextLocal(fields[base + 9]);
        table.columns = DecodeCrudPairs(fields[base + 10]);
        table.temporary = fields[base + 11] == "1";
        table.temporary_scope = fields[base + 12];
        table.temporary_session_uuid = fields[base + 13];
        table.on_commit_action = fields[base + 14];
        if (table.temporary || !table.temporary_scope.empty() ||
            !table.temporary_session_uuid.empty() ||
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
            request, request.rows[i], tables[i].event_sequence, fields[8]);
        if (!scratchbird::core::hash::ConstantTimeEqual(
                expected_decision, decisions[i])) {
          return MakeInvalidRequestDiagnostic(
              "mga.relation_metadata",
              "int32_migration_decision_hash_mismatch");
        }
      }
      const std::string payload = CanonicalInt32MigrationPayload(
          request, creator_tx, event_sequence, fields[8], tables, decisions);
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
              context.datatype_catalog_snapshot_uuid.canonical ||
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
      MgaTextIdentityMigrationRequest request;
      request.migration_id = fields[7];
      request.prior_catalog_snapshot_uuid = fields[9];
      request.new_catalog_snapshot_uuid = fields[10];
      request.prior_catalog_generation = ParseU64(fields[11]);
      request.new_catalog_generation = ParseU64(fields[12]);
      std::vector<CrudTableRecord> tables;
      std::vector<CrudSealedRelationDescriptorSnapshot>
          relation_descriptor_snapshots;
      std::vector<std::string> decisions;
      std::set<std::pair<std::string, std::string>> identities;
      std::map<std::string, std::string> table_projections;
      std::map<std::string, std::string> descriptor_projections;
      std::map<std::string, CrudTableRecord> unique_tables;
      std::map<std::string, CrudSealedRelationDescriptorSnapshot>
          unique_descriptors;
      for (std::size_t i = 0; i < mutation_count; ++i) {
        const std::size_t base = kHeaderFields + i * kFieldsPerRow;
        if (!CanonicalNonNilMigrationUuid(fields[base]) ||
            !CanonicalNonNilMigrationUuid(fields[base + 1]) ||
            !identities.emplace(fields[base], fields[base + 1]).second) {
          return MakeInvalidRequestDiagnostic(
              "mga.relation_metadata", "text_migration_batch_conflict");
        }
      }
      for (std::size_t i = 0; i < mutation_count; ++i) {
        const std::size_t base = kHeaderFields + i * kFieldsPerRow;
        MgaTextIdentityMigrationRow row;
        row.object_uuid = fields[base];
        row.column_uuid = fields[base + 1];
        row.old_row_generation = ParseU64(fields[base + 10]);
        if (fields[base + 2] != kLegacyTextDescriptorUuid ||
            fields[base + 3] != kCanonicalTextDescriptorUuid ||
            fields[base + 4] != kLegacyTextTypeUuid ||
            fields[base + 5] != kCanonicalTextTypeUuid ||
            fields[base + 6] != kCanonicalTextCodecUuid ||
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
        table.table_uuid = row.object_uuid;
        table.default_name = DecodeCrudTextLocal(fields[base + 13]);
        table.columns = DecodeCrudPairs(fields[base + 14]);
        table.temporary = fields[base + 21] == "1";
        table.temporary_scope = fields[base + 22];
        table.temporary_session_uuid = fields[base + 23];
        table.on_commit_action = fields[base + 24];
        if (fields[base + 21] != "0" || table.temporary ||
            EncodeCrudText(table.default_name) != fields[base + 13] ||
            EncodeCrudPairs(table.columns) != fields[base + 14] ||
            !table.temporary_scope.empty() ||
            !table.temporary_session_uuid.empty() ||
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
        const auto relation_fields = DecodeCrudPairs(fields[base + 15]);
        if (EncodeCrudPairs(relation_fields) != fields[base + 15]) {
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
          if (column.column_uuid.canonical != row.column_uuid) continue;
          if (column.canonical_name_key != migrated_column_name ||
              column.value_descriptor.descriptor_uuid.canonical !=
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
            relation_descriptor.database_uuid.canonical !=
                context.database_uuid.canonical ||
            relation_descriptor.relation_uuid.canonical != row.object_uuid ||
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
                relation_descriptor.descriptor_uuid.canonical ||
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
        relation_snapshot.relation_descriptor_uuid = fields[base + 16];
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
            !prior_table->temporary_session_uuid.empty() ||
            !prior_table->on_commit_action.empty()) {
          return MakeInvalidRequestDiagnostic(
              "mga.relation_metadata",
              "text_migration_prior_lineage_missing");
        }
        const auto persisted =
            LoadDescriptorFieldsByRelation(context, row.object_uuid);
        const auto prior_fields = persisted.find(row.object_uuid);
        if (prior_fields == persisted.end()) {
          return MakeInvalidRequestDiagnostic(
              "mga.relation_metadata",
              "text_migration_prior_relation_projection_missing");
        }
        auto expected_relation =
            DeserializeMgaRelationStorageDescriptor(prior_fields->second);
        if (ValidateMgaRelationStorageDescriptor(expected_relation).error ||
            expected_relation.database_uuid.canonical !=
                context.database_uuid.canonical ||
            expected_relation.relation_uuid.canonical != row.object_uuid ||
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
              {row.object_uuid, column.column_uuid.canonical});
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
                  column.column_uuid.canonical) ||
              !RewriteLegacyTextDescriptor(
                  context,
                  &column.value_descriptor.encoded_descriptor,
                  column.column_uuid.canonical) ||
              migrated_table_descriptor !=
                  column.value_descriptor.encoded_descriptor ||
              column.value_descriptor.encoded_descriptor !=
                  sealed_column->second) {
            return MakeInvalidRequestDiagnostic(
                "mga.relation_metadata",
                "text_migration_prior_relation_column_invalid");
          }
          prior_column->second = sealed_column->second;
          column.value_descriptor.descriptor_uuid.canonical =
              column.column_uuid.canonical;
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
            request, request.rows[i], tables[i].event_sequence, fields[8],
            fields[13], datatype_catalog_generation,
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
          request, creator_tx, event_sequence, fields[8], fields[13],
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
      decoded.max_event_sequence =
          std::max(decoded.max_event_sequence, ParseU64(fields[3]));
      decoded.indexes.push_back(std::move(index));
    }
  }
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
  const auto metadata_identity = ExistingFileIdentity(metadata_path);
  const auto savepoint_identity = ExistingFileIdentity(savepoint_path);
  result.key = MgaMetadataCacheKey{
      context.database_uuid.canonical,
      metadata_path,
      metadata_identity.ok ? metadata_identity.file_size : 0,
      metadata_identity.ok ? metadata_identity.file_mtime_ticks : 0,
      savepoint_path,
      savepoint_identity.ok ? savepoint_identity.file_size : 0,
      savepoint_identity.ok ? savepoint_identity.file_mtime_ticks : 0,
      context.local_transaction_id};
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
