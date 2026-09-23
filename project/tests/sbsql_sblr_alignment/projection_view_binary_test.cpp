// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/engine/internal_api/catalog/relation_projection_view.cpp"
#include <cstdlib>
#include <iostream>
#include <source_location>
namespace a=scratchbird::engine::internal_api;
static void Check(bool value,std::source_location at=std::source_location::current()) {
  if(!value){std::cerr<<"failure at "<<at.line()<<'\n';std::abort();}
}
static a::EngineUuid Id(unsigned n) {
  a::EngineUuid id;id.bytes={1,144,10,9,0,59,112,0,128,0,0,0,0,0,0,static_cast<std::uint8_t>(n)};return id;
}
int main() {
  a::EngineRelationProjectionViewDescriptor descriptor;
  descriptor.present=true;descriptor.marker=a::kEngineRelationProjectionViewMarkerV2;
  descriptor.view_uuid=Id(1);descriptor.view_descriptor_uuid=Id(2);descriptor.view_descriptor_generation=3;
  descriptor.source_relation_uuid=Id(4);descriptor.source_relation_descriptor_uuid=Id(5);
  descriptor.source_relation_descriptor_generation=6;descriptor.source_resource_epoch=7;
  a::EngineRelationProjectionViewOutput output;
  output.output_column_uuid=Id(8);output.expression_uuid=Id(9);output.output_name="source_value";
  output.output_type.type_descriptor_uuid=Id(10);output.output_type.descriptor_kind="scalar";
  output.output_type.canonical_type_name="int32";
  output.output_type.encoded_descriptor=std::string("binary\0type;",12)+std::string(reinterpret_cast<const char*>(descriptor.view_uuid.bytes.data()),16);
  output.source_column_uuid=Id(11);output.source_column_type_descriptor_uuid=Id(12);
  descriptor.outputs={output};
  const auto options=a::PersistedOptions(descriptor);
  Check(options.size()==19);
  const auto bytes=a::EncodeBinaryViewOptions(options);
  const auto decoded=a::PayloadOptions(bytes);
  Check(decoded==options);
  Check(a::BinaryViewUuid(std::string_view(decoded[1]).substr(21))==Id(2));
  const auto decoded_output=a::ParseCommonOutput(decoded,8,0);
  Check(decoded_output.has_value());
  Check(decoded_output->output_column_uuid==output.output_column_uuid);
  Check(decoded_output->expression_uuid==output.expression_uuid);
  Check(decoded_output->output_type.type_descriptor_uuid==output.output_type.type_descriptor_uuid);
  Check(decoded_output->output_type.encoded_descriptor==output.output_type.encoded_descriptor);
  Check(decoded_output->output_name==output.output_name);
  auto legacy=decoded;legacy[8]="output_0_column_uuid:01900a09-003b-7000-8000-000000000008";
  const auto refused=a::ParseCommonOutput(legacy,8,0);
  Check(!refused||refused->output_column_uuid.is_nil());
  const auto semantic=a::EngineRelationProjectionViewSemanticDescriptor(descriptor);
  a::BinaryCatalogMetadata metadata;
  Check(a::DecodeBinaryCatalogMetadata(semantic.encoded_descriptor,"relation_projection_semantic.v2",&metadata));
  Check(metadata.identities.at("view_uuid")==Id(1));
}
