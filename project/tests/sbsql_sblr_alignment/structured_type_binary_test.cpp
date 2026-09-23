// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Component regression exercises the actual private structured-type codec and
// descriptor builder. Unused catalog execution paths are removed at link time.
#include "../../src/engine/internal_api/catalog/structured_type_api.cpp"
#include <cstdlib>
#include <iostream>
#include <source_location>
namespace a = scratchbird::engine::internal_api;
static void Check(bool value, std::source_location at=std::source_location::current()) {
  if (!value) { std::cerr << "failure at " << at.line() << '\n'; std::abort(); }
}
static a::EngineUuid Id(unsigned n) {
  a::EngineUuid id;
  id.bytes={1,144,10,9,0,124,112,0,128,0,0,0,0,0,0,static_cast<std::uint8_t>(n)};
  return id;
}
static std::string Octets(const a::EngineUuid& id) {
  return {reinterpret_cast<const char*>(id.bytes.data()),16};
}
int main() {
  a::EngineApiRequest request;
  request.target_object.uuid=Id(1);
  request.target_schema.uuid=Id(2);
  request.option_envelopes={"label:red|green",std::string("label:black\0white",17),
      "base_range_uuid:"+Octets(Id(3)),"element_range_uuid:"+Octets(Id(4)),
      "field:item:"+Octets(Id(1))};
  Check(a::StructuredTypeUuid(request)==Id(1));
  Check(a::BinaryOptionUuid(Octets(Id(3)))==Id(3));
  Check(a::BinaryOptionUuid("01900a09-007c-7000-8000-000000000003").is_nil());
  auto payload=a::DescriptorPayload(request,"create_type","enum");
  Check(!payload.empty());
  Check(a::PayloadUuid(payload,"base_range_uuid")==Id(3));
  Check(a::PayloadUuid(payload,"element_range_uuid")==Id(4));
  Check(a::DescriptorVersion(payload)==1);
  auto labels=a::DecodeValueList(a::PayloadField(payload,"labels"));
  Check(labels.size()==2&&labels[0]=="red|green"&&labels[1]==std::string("black\0white",11));
  auto fields=a::DecodeValueList(a::PayloadField(payload,"fields"));
  Check(fields.size()==1&&a::BinaryOptionUuid(a::FieldType(fields[0]))==Id(1));
  a::BinaryCatalogMetadata metadata;
  Check(a::DecodeStructuredPayload(payload,&metadata));
  Check(!metadata.text.contains("type_uuid")&&!metadata.text.contains("schema_uuid"));
  a::AppendField(&metadata,"structured_action",std::string("alter_type"));
  a::AppendField(&metadata,"retired_labels",a::EncodeValueList({labels[0]}));
  Check(a::EncodeBinaryCatalogMetadata(metadata,"structured_type.v2",&payload));
  Check(a::PayloadUuid(payload,"base_range_uuid")==Id(3));
  Check(a::DecodeValueList(a::PayloadField(payload,"retired_labels"))==std::vector<std::string>{labels[0]});
  Check(!a::DecodeBinaryCatalogMetadata(payload,"constraint.v2",&metadata));
  Check(!a::DecodeStructuredPayload("type_uuid=01900a09-007c-7000-8000-000000000001",&metadata));
}
