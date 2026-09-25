// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "mga_metadata_migration_fingerprint.hpp"
#include "mga_metadata_record_codec.hpp"
#include "hash_digest.hpp"
#include <vector>
namespace scratchbird::engine::internal_api {
using namespace metadata_migration;
// SEARCH_KEY: SB_ENGINE_MGA_METADATA_MIGRATION_FINGERPRINT_IMPLEMENTATION_AUTHORITY
// Pure fingerprint encoding only. UUID fields remain framed binary16; these
// hashes confer no catalog, visibility, transaction or publication authority.
namespace {
void AppendCanonicalBatchField(std::string* out, std::string_view key, std::string_view value) {
  if (!out) return;
  AppendBinaryString(out, key);
  AppendBinaryString(out, value);
}
void AppendCanonicalBatchField(std::string* out, std::string_view key, const EngineUuid& value) {
  AppendCanonicalBatchField(out, key, MetadataUuidBytes(value));
}

} // namespace

std::string CanonicalBigintMigrationPayload(
    const MgaBigintIdentityMigrationRequest& request,
    std::uint64_t creator_tx,
    std::uint64_t event_sequence,
    const EngineUuid& transaction_uuid,
    const std::vector<CrudTableRecord>& tables,
    const std::vector<std::string>& decision_hashes) {
  std::string payload;
  auto field = [&](std::string_view key, const auto& value) {
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
    field("table_columns", EncodeMetadataPairs(table.columns));
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
    const EngineUuid& transaction_uuid) {
  std::string payload;
  auto field = [&](std::string_view key, const auto& value) {
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
    const EngineUuid& transaction_uuid,
    const std::vector<CrudTableRecord>& tables,
    const std::vector<std::string>& decision_hashes) {
  std::string payload;
  auto field = [&](std::string_view key, const auto& value) {
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
    field("table_columns", EncodeMetadataPairs(table.columns));
  }
  return payload;
}

std::string Int32MigrationDecisionHash(
    const MgaInt32IdentityMigrationRequest& request,
    const MgaInt32IdentityMigrationRow& row,
    std::uint64_t new_row_generation,
    const EngineUuid& transaction_uuid) {
  std::string payload;
  auto field = [&](std::string_view key, const auto& value) {
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
    const EngineUuid& transaction_uuid,
    const EngineUuid& datatype_catalog_snapshot_uuid,
    std::uint64_t datatype_catalog_generation,
    std::uint64_t datatype_registry_generation,
    const std::vector<CrudTableRecord>& tables,
    const std::vector<CrudSealedRelationDescriptorSnapshot>&
        relation_descriptor_snapshots,
    const std::vector<std::string>& decision_hashes) {
  std::string payload;
  auto field = [&](std::string_view key, const auto& value) {
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
    field("table_columns", EncodeMetadataPairs(table.columns));
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
          EncodeMetadataPairs(snapshot.descriptor_fields));
  }
  return payload;
}

std::string TextMigrationDecisionHash(
    const MgaTextIdentityMigrationRequest& request,
    const MgaTextIdentityMigrationRow& row,
    std::uint64_t new_row_generation,
    const EngineUuid& transaction_uuid,
    const EngineUuid& datatype_catalog_snapshot_uuid,
    std::uint64_t datatype_catalog_generation,
    std::uint64_t datatype_registry_generation,
    const CrudSealedRelationDescriptorSnapshot& relation_snapshot) {
  std::string payload;
  auto field = [&](std::string_view key, const auto& value) {
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
        EncodeMetadataPairs(relation_snapshot.descriptor_fields));
  return Sha256Tagged(payload);
}

} // namespace scratchbird::engine::internal_api
