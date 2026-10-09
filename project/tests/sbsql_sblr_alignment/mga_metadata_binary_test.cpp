// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/engine/internal_api/mga_relation_store/mga_relation_metadata_store.cpp"
#include "../support/owned_temp_directory.hpp"
#include <cstdlib>
#include <iostream>
#include <source_location>
namespace a = scratchbird::engine::internal_api;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  if (!ok) throw std::runtime_error("failure at " + std::to_string(at.line()));
}
int main() try {
  a::EngineUuid id{{1,144,0,0,0,0,112,0,128,0,0,0,0,0,0,9}}, decoded_id;
  const auto binary = a::MetadataUuidBytes(id);
  Check(binary.size() == 16 && a::ReadMetadataUuid(binary, &decoded_id) && decoded_id == id);
  Check(!a::ReadMetadataUuid("01900000-0000-7000-8000-000000000009", &decoded_id));
  Check(!a::ReadMetadataUuid(std::string(16, '\0'), &decoded_id));
  Check(a::ReadMetadataUuid(std::string(16, '\0'), &decoded_id, true) && decoded_id.is_nil());
  const std::vector<std::string> fields{"TABLE_METADATA", binary, std::string("\0\n\t;=",5), ""};
  const auto frame = a::EncodeMgaMetadataFields(fields);
  std::vector<std::string> read;
  Check(a::DecodeMgaMetadataFields(frame, &read) && read == fields);
  for (std::size_t n=0; n<frame.size(); ++n) {
    read = {"unchanged"};
    Check(!a::DecodeMgaMetadataFields(frame.substr(0,n), &read) && read == std::vector<std::string>{"unchanged"});
    auto corrupted = frame; corrupted[n] ^= 1;
    Check(!a::DecodeMgaMetadataFields(corrupted, &read));
  }
  Check(!a::DecodeMgaMetadataFields(frame + "x", &read));
  Check(!a::DecodeMgaMetadataFields("SBMGA1\tTABLE_METADATA\t1\t2\n", &read));
  const auto stream = frame + frame;
  const auto span = [](const std::string& value) { return std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(value.data()), value.size()); };
  Check(a::DecodeMgaMetadataStream(span(stream), &read) && read == std::vector<std::string>({frame,frame}));
  for (std::size_t n=1; n<frame.size(); ++n) {
    const auto damaged = frame + frame.substr(0,n);
    read = {"unchanged"};
    Check(!a::DecodeMgaMetadataStream(span(damaged), &read) && read == std::vector<std::string>{"unchanged"});
  }
  const std::vector<std::pair<std::string,std::string>> pairs{{std::string("key\0",4),binary},{"column",frame}};
  std::vector<std::pair<std::string,std::string>> decoded;
  Check(a::DecodeMetadataPairs(a::EncodeMetadataPairs(pairs), &decoded) && decoded == pairs);
  Check(a::DecodeMetadataPairs(a::EncodeMetadataPairs({}), &decoded) && decoded.empty());
  Check(!a::DecodeMetadataPairs("a=b,c=d", &decoded));
  a::MgaConstraintMutationBatch batch;
  batch.batch_uuid = id; batch.database_uuid = id; batch.constraint_uuid = id;
  batch.owner_table_uuid = id; batch.child_schema_uuid = id; batch.child_relation_descriptor_uuid = id;
  batch.child_column_uuid = id; batch.parent_table_uuid = id; batch.parent_schema_uuid = id;
  batch.parent_relation_descriptor_uuid = id; batch.parent_column_uuid = id;
  batch.parent_candidate_key_constraint_uuid = id; batch.key_descriptor_uuid = id; batch.support_uuid = id;
  batch.updated_table.table_uuid = id; batch.updated_table.columns = pairs;
  batch.canonical_constraint_envelope = std::string("envelope\0\t",10);
  const auto journal_fields = a::ConstraintMutationBatchLineFields(batch, 7, 9);
  Check(journal_fields.size() == a::ConstraintMutationBatchFieldCount());
  Check(journal_fields[5] == binary && journal_fields[9] == binary && journal_fields[36] == binary);
  Check(journal_fields[41] == std::string(16, '\0'));
  Check(a::DecodeMetadataPairs(journal_fields[38], &decoded) && decoded == pairs);
  Check(journal_fields[35] == batch.canonical_constraint_envelope);
  const auto hash = a::ConstraintMutationBatchSha256(batch,7,9);
  Check(!hash.empty() && hash != a::ConstraintMutationBatchSha256(batch,7,10));
  batch.constraint_uuid.bytes[15] ^= 1;
  Check(hash != a::ConstraintMutationBatchSha256(batch,7,9));
  // Serialization/fingerprint only: this arbitrary vector is not claimed as
  // an admitted descriptor. Live publication/reopen checks the actual seal.
  batch.format_version = "neutral_fk_mutation_batch_v2";
  batch.descriptor_field_count = pairs.size();
  batch.descriptor_field_bytes = 123;
  batch.contextual_sidecar_count = 1;
  batch.sealed_descriptor_fields = pairs;
  const auto sealed_fields = a::ConstraintMutationBatchLineFields(batch, 7, 9);
  Check(sealed_fields.size() == 47 && sealed_fields.size() ==
      a::ConstraintMutationBatchFieldCount(batch.format_version));
  Check(a::ConstraintMutationBatchFieldCount("unrecognized") == 0);
  Check(sealed_fields[43] == "2" && sealed_fields[44] == "123" && sealed_fields[45] == "1");
  Check(a::DecodeMetadataPairs(sealed_fields[46], &decoded) && decoded == pairs);
  const auto sealed_hash = a::ConstraintMutationBatchSha256(batch, 7, 9);
  Check(sealed_hash != hash);
  for (unsigned dimension = 0; dimension < 4; ++dimension) {
    auto changed = batch;
    if (dimension == 0) ++changed.descriptor_field_count;
    if (dimension == 1) ++changed.descriptor_field_bytes;
    if (dimension == 2) ++changed.contextual_sidecar_count;
    if (dimension == 3) changed.sealed_descriptor_fields.front().second[0] ^= 1;
    Check(sealed_hash != a::ConstraintMutationBatchSha256(changed, 7, 9));
  }
  Check(sealed_hash != a::ConstraintMutationBatchSha256(batch, 8, 9));
  Check(sealed_hash != a::ConstraintMutationBatchSha256(batch, 7, 10));
  const auto sealed_frame = a::EncodeMgaMetadataFields(sealed_fields);
  Check(a::DecodeMgaMetadataFields(sealed_frame, &read) && read == sealed_fields);
  for (std::size_t size = 0; size < sealed_frame.size(); ++size) {
    read = {"unchanged"};
    Check(!a::DecodeMgaMetadataFields(sealed_frame.substr(0, size), &read) &&
          read == std::vector<std::string>{"unchanged"});
  }
  // These frames and outer hashes are complete and valid. Refusal must arise
  // from descriptor semantics, not checksum damage, framing or an exception.
  scratchbird::tests::OwnedTempDirectory temporary;
  a::EngineRequestContext context;
  context.database_uuid = id;
  context.database_path = (temporary.path() / "malformed-metadata").string();
  batch.mutation_count = 1;
  batch.child_relation_descriptor_generation = 1;
  batch.parent_relation_descriptor_generation = 1;
  batch.constraint_metadata_generation = 1;
  batch.base_table_event_sequence = 1;
  batch.parent_base_table_event_sequence = 1;
  batch.support_family = "btree";
  batch.support_policy = "required_exact_unique_index";
  batch.match_policy = "simple";
  batch.on_update_action = batch.on_delete_action = "no_action";
  batch.enforcement_timing = "immediate";
  batch.constraint_kind = "foreign_key";
  for (const auto& malformed : std::vector<std::vector<std::pair<std::string,std::string>>>{
           {}, pairs, {{"relation_generation", "0"}, {"descriptor_generation", "0"}}}) {
    batch.sealed_descriptor_fields = malformed;
    batch.descriptor_field_count = malformed.size();
    batch.batch_hash = a::ConstraintMutationBatchSha256(batch, 7, 9);
    const auto record = a::EncodeMgaMetadataFields(a::ConstraintMutationBatchLineFields(batch, 7, 9));
    {
      std::ofstream out(context.database_path + ".sb.mga_relation_metadata", std::ios::binary | std::ios::trunc);
      out.write(record.data(), record.size());
      Check(out.good());
    }
    a::RelationReadSnapshot state;
    const auto result = a::LoadMgaMetadata(&state, context);
    Check(result.error && result.detail.find("constraint_sealed_descriptor_invalid") != std::string::npos);
    Check(state.tables.empty() && state.sealed_relation_descriptor_snapshots.empty());
    if (!malformed.empty()) {
      const auto table_frame = a::EncodeMgaMetadataFields({
          "SBMGA1", "TABLE_METADATA_SEALED_DESCRIPTOR_V2", "7", "9",
          "mga_sealed_contextual_text_sidecar_set_v2", "sealed", binary,
          "malformed", a::EncodeMetadataPairs(pairs), "0", "", std::string(16,'\0'), "",
          binary, "1", std::to_string(malformed.size()), "123", "1", a::EncodeMetadataPairs(malformed)});
      {
        std::ofstream out(context.database_path + ".sb.mga_relation_metadata", std::ios::binary | std::ios::trunc);
        out.write(table_frame.data(), table_frame.size());
        Check(out.good());
      }
      const auto table_result = a::LoadMgaMetadata(&state, context);
      Check(table_result.error && table_result.detail.find("sealed_table_metadata_v2_descriptor_invalid") != std::string::npos);
      Check(state.tables.empty() && state.sealed_relation_descriptor_snapshots.empty());
    }
  }
  temporary.Cleanup();
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
