// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "mga_relation_metadata_store.hpp"
#include "mga_metadata_record_codec.hpp"
#include "hash_digest.hpp"
namespace scratchbird::engine::internal_api {
// SEARCH_KEY: SB_ENGINE_MGA_CONSTRAINT_BATCH_FINGERPRINT_IMPLEMENTATION_AUTHORITY
// Pure canonical binary fingerprint construction; no I/O or finality authority.
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
std::string CanonicalConstraintMutationBatchPayload(
    const MgaConstraintMutationBatch& batch,
    std::uint64_t creator_local_transaction_id,
    std::uint64_t metadata_event_sequence) {
  std::string payload;
  auto field = [&](std::string_view key, const auto& value) {
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
  field("updated_table_columns", EncodeMetadataPairs(batch.updated_table.columns));
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

} // namespace scratchbird::engine::internal_api
