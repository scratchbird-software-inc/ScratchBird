// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog/constraint_metadata_codec.hpp"
#include "catalog/binary_view_options.hpp"
#include "catalog/schema_tree_codec.hpp"
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <source_location>
namespace a = scratchbird::engine::internal_api;
static void Check(bool value, std::source_location at = std::source_location::current()) {
  if (!value) { std::cerr << "failure at " << at.line() << '\n'; std::abort(); }
}
static a::EngineUuid Id(unsigned n) {
  a::EngineUuid id;
  id.bytes = {1,144,10,9,0,0,112,0,128,0,0,0,0,0,0,static_cast<std::uint8_t>(n)};
  return id;
}
template<class Record> void CheckRecord() {
  using Traits = a::catalog_record_codec::Traits<Record>;
  Record record; unsigned ordinal=1;
  std::apply([&](auto&... field) {
    const auto initialize = [&](auto& value) {
      using T = std::decay_t<decltype(value)>;
      if constexpr (std::is_same_v<T, a::EngineUuid>) value=Id(ordinal++);
      else if constexpr (std::is_same_v<T, std::string>) value=std::string("field\0\t\n;=",10);
      else if constexpr (std::is_same_v<T, bool>) value=true;
      else value=0x1234;
    };
    (initialize(field), ...);
  }, Traits::Fields(record));
  std::string encoded;
  Check(a::EncodeCatalogLifecycleRecord(record, &encoded));
  a::ApiBehaviorRecord frame;
  auto decode_frame = [](std::string_view bytes, a::ApiBehaviorRecord* out) {
    return a::DecodeApiBehaviorRecord({reinterpret_cast<const std::uint8_t*>(bytes.data()),bytes.size()},out);
  };
  Check(decode_frame(encoded,&frame));
  Check(frame.payload.substr(0,8)=="SBCAT002");
  // Creator is LE64 followed immediately by the first fixed-width UUID.
  for (unsigned i=0;i<16;++i) Check(static_cast<std::uint8_t>(frame.payload[16+i])==Traits::Identity(record).bytes[i]);
  a::CatalogLifecycleRecord decoded;
  Check(a::DecodeCatalogLifecycleFrame(frame,&decoded));
  Check(std::holds_alternative<Record>(decoded));
  const auto& copy=std::get<Record>(decoded);
  Check(Traits::Fields(copy)==Traits::Fields(record));
  std::string repeated;
  Check(a::EncodeCatalogLifecycleRecord(copy,&repeated)&&repeated==encoded);
  for (std::size_t n=0;n<encoded.size();++n) {
    Check(!decode_frame(encoded.substr(0,n),&frame));
    auto corrupt=encoded; corrupt[n]^=1;
    Check(!decode_frame(corrupt,&frame));
  }
  Check(decode_frame(encoded,&frame));
  frame.creator_tx++;
  Check(!a::DecodeCatalogLifecycleFrame(frame,&decoded));
  Check(decode_frame(encoded,&frame));
  frame.payload += 'x';
  Check(!a::DecodeCatalogLifecycleFrame(frame,&decoded));
  Check(decode_frame(encoded,&frame));
  frame.object_uuid=Id(200);
  Check(!a::DecodeCatalogLifecycleFrame(frame,&decoded));
  std::istringstream stream(encoded+encoded,std::ios::binary);
  Check(a::ReadCatalogLifecycleRecord(stream,&decoded));
  Check(a::ReadCatalogLifecycleRecord(stream,&decoded));
  Check(stream.peek()==std::char_traits<char>::eof());
  auto malformed=record;
  std::apply([](auto&... field) {
    const auto invalidate=[](auto& value) {
      if constexpr(std::is_same_v<std::decay_t<decltype(value)>, a::EngineUuid>) value.bytes[6]=0x40;
    };
    (invalidate(field), ...);
  },Traits::Fields(malformed));
  auto saved=encoded;
  Check(!a::EncodeCatalogLifecycleRecord(malformed,&encoded)&&encoded==saved);
}
int main() {
  CheckRecord<a::EngineCatalogObjectRecord>();
  CheckRecord<a::EngineCatalogNameRecord>();
  CheckRecord<a::EngineCatalogDependencyRecord>();
  CheckRecord<a::EngineCatalogColumnMetadataRecord>();
  CheckRecord<a::EngineCatalogConstraintDescriptorRecord>();
  CheckRecord<a::EngineCatalogKeyDescriptorRecord>();
  CheckRecord<a::EngineCatalogConstraintSubjectRecord>();
  CheckRecord<a::EngineCatalogConstraintDependencyRecord>();
  CheckRecord<a::EngineCatalogConstraintSupportStructureRecord>();
  CheckRecord<a::EngineCatalogRetireNamesRecord>();
  CheckRecord<a::EngineCatalogCacheInvalidationRecord>();
  a::CatalogLifecycleRecord output;
  std::istringstream legacy("SBCATOBJ1\tOBJECT\t0\t01900000-0000-7000-8000-000000000001\n");
  Check(!a::ReadCatalogLifecycleRecord(legacy,&output));
  a::CatalogConstraintMetadata fields;
  fields.text["predicate"] = std::string("a\0b;=\n",7);
  fields.identities["support_uuid"] = Id(3);
  fields.identities["optional_uuid"] = {};
  std::string encoded;
  Check(a::EncodeCatalogConstraintMetadata(fields,&encoded));
  a::CatalogConstraintMetadata copy;
  Check(a::DecodeCatalogConstraintMetadata(encoded,&copy));
  Check(copy.text==fields.text&&copy.identities==fields.identities);
  for(std::size_t n=0;n<encoded.size();++n) Check(!a::DecodeCatalogConstraintMetadata(encoded.substr(0,n),&copy));
  Check(!a::DecodeCatalogConstraintMetadata(encoded+"x",&copy));
  Check(!a::DecodeCatalogConstraintMetadata("support_uuid=01900000-0000-7000-8000-000000000003",&copy));
  fields.text["support_uuid"]="forbidden";
  Check(!a::EncodeCatalogConstraintMetadata(fields,&encoded));
  auto view_id=Id(9); view_id.bytes[4]=';'; view_id.bytes[5]='|';
  const std::vector<std::string> view_options={"view_query_shape:bounded",
      a::BinaryViewUuidOption("view_uuid:",view_id),std::string("alias:a\0b",9)};
  const auto view_bytes=a::EncodeBinaryViewOptions(view_options);
  std::vector<std::string> view_copy;
  Check(a::DecodeBinaryViewOptions(view_bytes,&view_copy)&&view_copy==view_options);
  Check(a::BinaryViewUuid(std::string_view(view_copy[1]).substr(10))==view_id);
  Check(a::BinaryViewUuid("01900a09-0000-7000-8000-000000000009").is_nil());
  for(std::size_t n=0;n<view_bytes.size();++n) Check(!a::DecodeBinaryViewOptions(view_bytes.substr(0,n),&view_copy));
  Check(!a::DecodeBinaryViewOptions(view_bytes+"x",&view_copy));
  Check(!a::DecodeBinaryViewOptions("options=view_uuid:01900a09-0000-7000-8000-000000000009",&view_copy));
  const auto semantic=a::GlobalAggregateSemanticPayload(view_id,7,"aggregate.v1","average");
  a::BinaryCatalogMetadata semantic_fields;
  Check(a::DecodeBinaryCatalogMetadata(semantic,"global_aggregate_semantic.v2",&semantic_fields));
  Check(semantic_fields.identities.at("view_uuid")==view_id);
  Check(!a::DecodeBinaryCatalogMetadata(semantic,"structured_type.v2",&semantic_fields));

  a::EngineLocalizedName name;
  name.language_tag="en";name.name_class="primary";name.name=std::string("a,;\0b",5);
  name.display_name=name.name;name.path="root.child";name.default_name=true;
  name.was_quoted=true;name.requires_exact_match=true;name.identifier_profile_uuid="sbsql_v3";
  std::vector<a::EngineLocalizedName> names={name},decoded_names;
  std::vector<std::pair<std::string,std::string>> comments={{"en",std::string("x;:y\0z",6)}},decoded_comments;
  a::BinaryCatalogMetadata extensions,decoded_extensions;
  Check(a::AddSchemaTreeExtension(a::BinaryViewUuidOption("schema_union_member:",Id(31)),&extensions));
  Check(a::AddSchemaTreeExtension(a::BinaryViewUuidOption("schema_union_member:",Id(32)),&extensions));
  Check(a::AddSchemaTreeExtension(a::BinaryViewUuidOption("catalog_ddl_mutation_audit:",Id(33)),&extensions));
  Check(!a::AddSchemaTreeExtension("schema_union_member:01900a09-0000-7000-8000-000000000031",&extensions));
  Check(a::EncodeSchemaTreeMetadata(names,comments,extensions,&encoded));
  Check(a::DecodeSchemaTreeMetadata(encoded,&decoded_names,&decoded_comments,&decoded_extensions));
  Check(decoded_names.size()==1&&a::SchemaNameFields(decoded_names[0])==a::SchemaNameFields(name));
  Check(decoded_comments==comments&&decoded_extensions.identities==extensions.identities);
  Check(decoded_extensions.identities.at("schema_union_member.0")==Id(31));
  Check(decoded_extensions.identities.at("schema_union_member.1")==Id(32));
  for(std::size_t n=0;n<encoded.size();++n) Check(!a::DecodeSchemaTreeMetadata(encoded.substr(0,n),&decoded_names,&decoded_comments));
  Check(!a::DecodeSchemaTreeMetadata("localized_name=en,primary,path,name,default",&decoded_names,&decoded_comments));

}
