// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/engine/internal_api/nosql/key_value_api.cpp"
#include "../support/binary_uuid_fixture.hpp"
#include <cstdlib>
#include <iostream>
#include <source_location>
namespace a = scratchbird::engine::internal_api;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  if (!ok) { std::cerr << "failure at " << at.line() << '\n'; std::abort(); }
}
int main() {
  auto id = scratchbird::tests::FixtureUuid(1112, 1);
  id.bytes[9] = 0; id.bytes[15] = 0xff;
  a::PhysicalKeyValueRecord record;
  record.object_uuid = id;
  record.row_uuid = scratchbird::tests::FixtureUuid(1112, 2);
  record.key = "018f0000-0000-7000-8000-000000000001";
  record.value = "user value";
  a::EngineApiRequest request;
  request.context.database_uuid = scratchbird::tests::FixtureUuid(1112, 3);
  request.target_object.uuid = id;
  a::PhysicalStores()[a::StoreKey(request.context)][record.key] = record;
  Check(a::RequestKey(request, {}) == record.key);
  auto other = request;
  other.context.database_uuid.bytes[15] ^= 1;
  Check(a::RequestKey(other, {}).empty());
  Check(a::RequestKey(other, record.key) == record.key);
  Check(a::AdmitStoredRowUuid(record.row_uuid).value == record.row_uuid);
  auto invalid = record.row_uuid; invalid.bytes[8] = 0;
  Check(!a::AdmitStoredRowUuid(invalid).valid());
  a::EngineApiResult result;
  a::AddKvRow(&result, record);
  Check(result.result_shape.rows.size() == 1);
  for (const auto& [name, value] : result.result_shape.rows[0].fields) {
    if (name == "key_uuid" || name == "row_uuid") {
      const auto expected = name == "key_uuid" ? id : record.row_uuid;
      Check(value.encoded_value.empty());
      Check(value.binary_value == std::vector<std::uint8_t>(expected.bytes.begin(), expected.bytes.end()));
    } else if (name == "key") Check(value.encoded_value == record.key && value.binary_value.empty());
  }
  scratchbird::core::datatypes::DatatypeTypeCodecIdentityRowV1 registry;
  registry.type_uuid = scratchbird::tests::FixtureUuid(1112, 4);
  registry.descriptor_uuid = scratchbird::tests::FixtureUuid(1112, 5);
  registry.codec_uuid = scratchbird::tests::FixtureUuid(1112, 6);
  registry.descriptor_generation = 2; registry.type_generation = 3;
  registry.codec_id = "text.utf8"; registry.codec_version = 1; registry.codec_generation = 4;
  a::EngineDescriptor descriptor;
  descriptor.descriptor_uuid = id; descriptor.type_uuid = registry.type_uuid;
  descriptor.descriptor_kind = "canonical_type_descriptor"; descriptor.canonical_type_name = "text";
  a::CatalogColumnMetadata metadata;
  metadata.identities = {{"column_uuid", id}, {"type_uuid", registry.type_uuid},
      {"datatype_descriptor_uuid", registry.descriptor_uuid}, {"codec_uuid", registry.codec_uuid}};
  metadata.text = {{"canonical", "text"}, {"nullable", "false"},
      {"datatype_descriptor_generation", "2"}, {"type_generation", "3"},
      {"codec_id", registry.codec_id}, {"codec_version", "1"}, {"codec_generation", "4"},
      {"null_encoding", std::to_string(registry.null_encoding_code)}};
  Check(a::EncodeCatalogColumnMetadata(metadata, &descriptor.encoded_descriptor));
  auto admitted = [&] {return a::ExactKeyValueValueDescriptor(descriptor, "text", registry.type_uuid, &registry, id, false);};
  Check(admitted());
  descriptor.type_uuid = id; Check(!admitted()); descriptor.type_uuid = registry.type_uuid;
  metadata.identities["codec_uuid"] = id;
  Check(a::EncodeCatalogColumnMetadata(metadata, &descriptor.encoded_descriptor)); Check(!admitted());
  metadata.identities["codec_uuid"] = registry.codec_uuid;
  metadata.text["unrecognized"] = "true";
  Check(a::EncodeCatalogColumnMetadata(metadata, &descriptor.encoded_descriptor)); Check(!admitted());
  descriptor.encoded_descriptor = "canonical=text;type_uuid=018f0000-0000-7000-8000-000000000001;nullable=false";
  Check(!admitted());
}
