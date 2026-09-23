// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/engine/internal_api/mga_relation_store/mga_relation_metadata_store.cpp"
#include <cstdlib>
#include <iostream>
#include <source_location>
namespace a = scratchbird::engine::internal_api;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  if (!ok) { std::cerr << "failure at " << at.line() << '\n'; std::abort(); }
}
int main() {
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

}
