// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "binary_catalog_metadata.hpp"
namespace scratchbird::engine::internal_api {
// Schema extension options carry UUIDs as exactly sixteen octets. Repeated
// union members retain their order through explicit sequence keys.
inline bool AddSchemaTreeExtension(std::string_view option, BinaryCatalogMetadata* fields) {
  if (!fields) return false;
  const auto colon=option.find(':');
  if(colon==std::string_view::npos)return false;
  const std::string key(option.substr(0,colon));
  const auto value=option.substr(colon+1);
  if(key=="schema_union_member"||key=="catalog_ddl_mutation_audit") {
    EngineUuid id;
    if(value.size()!=16)return false;
    std::copy_n(reinterpret_cast<const std::uint8_t*>(value.data()),16,id.bytes.begin());
    if(!core::uuid::IsEngineIdentityUuid(id))return false;
    if(key=="schema_union_member") {
      std::size_t index=0;
      while(fields->identities.contains(key+"."+std::to_string(index)))++index;
      fields->identities.emplace(key+"."+std::to_string(index),id);
      return true;
    }
    return fields->identities.emplace(key,id).second;
  }
  for(const auto allowed:{"schema_union_policy","schema_union_root","lifecycle_transition",
      "mga_root_mutation_registry","implementation_flavour","filespace_diagnostic"}) {
    if(key==allowed)return fields->text.emplace(key,std::string(value)).second;
  }
  return false;
}
template<class Name> auto SchemaNameFields(Name& name) {
  return std::tie(name.language_tag, name.name_class, name.path, name.name, name.default_name, name.reference_id, name.dialect_profile_uuid, name.identifier_profile_uuid, name.case_fold_profile_uuid, name.quoted_identifier_profile_uuid, name.raw_name_text, name.display_name, name.was_quoted, name.quote_style, name.requires_exact_match, name.normalized_lookup_key, name.exact_lookup_key, name.full_path_lookup_key);
}
inline bool EncodeSchemaTreeMetadata(const std::vector<EngineLocalizedName>& names,
    const std::vector<std::pair<std::string,std::string>>& comments,
    BinaryCatalogMetadata metadata,std::string* output) {
  if (names.size()>65536 || comments.size()>65536 || metadata.text.contains("localized_names") ||
      metadata.text.contains("localized_comments")) return false;
  std::string name_bytes,comment_bytes;
  AppendBinaryU32(&name_bytes,static_cast<std::uint32_t>(names.size()));
  for (const auto& name:names) {
    if (!std::apply([&](const auto&... value){return (catalog_record_codec::Put(name_bytes,value)&&...);},SchemaNameFields(name)))return false;
  }
  AppendBinaryU32(&comment_bytes,static_cast<std::uint32_t>(comments.size()));
  for(const auto& [language,text]:comments){
    if(!catalog_record_codec::Put(comment_bytes,language)||!catalog_record_codec::Put(comment_bytes,text))return false;
  }
  metadata.text["localized_names"]=std::move(name_bytes);
  metadata.text["localized_comments"]=std::move(comment_bytes);
  return EncodeBinaryCatalogMetadata(metadata,"schema_tree.v2",output);
}
inline bool DecodeSchemaTreeMetadata(std::string_view bytes,
    std::vector<EngineLocalizedName>* names,
    std::vector<std::pair<std::string,std::string>>* comments,
    BinaryCatalogMetadata* extensions=nullptr) {
  if(!names||!comments)return false;
  BinaryCatalogMetadata fields;
  if(!DecodeBinaryCatalogMetadata(bytes,"schema_tree.v2",&fields)||
      !fields.text.contains("localized_names")||!fields.text.contains("localized_comments"))return false;
  const auto& name_bytes=fields.text.at("localized_names");
  const auto& comment_bytes=fields.text.at("localized_comments");
  std::span<const std::uint8_t> input(reinterpret_cast<const std::uint8_t*>(name_bytes.data()),name_bytes.size());
  std::size_t cursor=0;std::uint32_t count=0;
  if(!ReadBinaryU32(input,&cursor,&count)||count>65536||count>(input.size()-cursor)/4)return false;
  std::vector<EngineLocalizedName> staged_names;
  for(std::uint32_t n=0;n<count;++n){
    EngineLocalizedName name;
    if(!std::apply([&](auto&... value){return (catalog_record_codec::Get(input,cursor,value)&&...);},SchemaNameFields(name)))return false;
    staged_names.push_back(std::move(name));
  }
  if(cursor!=input.size())return false;
  input={reinterpret_cast<const std::uint8_t*>(comment_bytes.data()),comment_bytes.size()};cursor=0;
  if(!ReadBinaryU32(input,&cursor,&count)||count>65536||count>(input.size()-cursor)/8)return false;
  std::vector<std::pair<std::string,std::string>> staged_comments;
  for(std::uint32_t n=0;n<count;++n){
    std::string language,text;
    if(!catalog_record_codec::Get(input,cursor,language)||!catalog_record_codec::Get(input,cursor,text))return false;
    staged_comments.emplace_back(std::move(language),std::move(text));
  }
  if(cursor!=input.size())return false;
  names->swap(staged_names);comments->swap(staged_comments);
  if(extensions){fields.text.erase("localized_names");fields.text.erase("localized_comments");*extensions=std::move(fields);}
  return true;
}
}  // namespace scratchbird::engine::internal_api
