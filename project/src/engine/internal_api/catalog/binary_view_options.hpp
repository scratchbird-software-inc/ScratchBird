// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "binary_catalog_metadata.hpp"
namespace scratchbird::engine::internal_api {
inline EngineUuid BinaryViewUuid(std::string_view bytes) {
  EngineUuid id;
  if (bytes.size() != 16) return {};
  std::copy_n(reinterpret_cast<const std::uint8_t*>(bytes.data()),16,id.bytes.begin());
  return core::uuid::IsEngineIdentityUuid(id) ? id : EngineUuid{};
}
inline std::string BinaryViewUuidOption(std::string_view prefix, const EngineUuid& id) {
  if (!core::uuid::IsEngineIdentityUuid(id)) return {};
  std::string bytes(prefix);
  bytes.append(reinterpret_cast<const char*>(id.bytes.data()),16);
  return bytes;
}
inline std::string EncodeBinaryViewOptions(const std::vector<std::string>& options) {
  if (options.size()>65536) return {};
  std::string bytes="SBVIEW02";
  AppendBinaryU32(&bytes,static_cast<std::uint32_t>(options.size()));
  for (const auto& option:options) {
    if (!catalog_record_codec::Put(bytes,option)) return {};
  }
  return bytes.size()<=kApiBehaviorRecordMaximumBytes ? bytes : std::string{};
}
inline bool DecodeBinaryViewOptions(std::string_view payload,std::vector<std::string>* output) {
  if (!output||payload.size()<12||payload.size()>kApiBehaviorRecordMaximumBytes||payload.substr(0,8)!="SBVIEW02")return false;
  const std::span<const std::uint8_t> bytes(reinterpret_cast<const std::uint8_t*>(payload.data()),payload.size());
  std::size_t cursor=8;std::uint32_t count=0;
  if (!ReadBinaryU32(bytes,&cursor,&count)||count>65536||count>(bytes.size()-cursor)/4)return false;
  std::vector<std::string> options;
  for(std::uint32_t n=0;n<count;++n){std::string option;if(!ReadBinaryString(bytes,&cursor,&option))return false;options.push_back(std::move(option));}
  if(cursor!=bytes.size())return false;
  output->swap(options);return true;
}
// Retrieve one length-framed field. Duplicate keys are ambiguous authority.
inline std::string BinaryViewOptionValue(std::string_view payload, std::string_view prefix) {
  std::vector<std::string> fields;
  if (!DecodeBinaryViewOptions(payload, &fields)) return {};
  std::string value;
  bool found = false;
  for (const auto& field : fields) {
    if (!field.starts_with(prefix)) continue;
    if (found) return {};
    found = true;
    value = field.substr(prefix.size());
  }
  return value;
}
inline std::string GlobalAggregateSemanticPayload(const EngineUuid& view_uuid,std::uint64_t generation,
                                                 const std::string& marker,const std::string& alias) {
  BinaryCatalogMetadata fields;
  fields.identities["view_uuid"]=view_uuid;
  fields.text={{"marker",marker},{"view_descriptor_generation",std::to_string(generation)},
               {"result_alias",alias},{"result_type","int64"},{"result_nullable","true"}};
  std::string bytes;
  if(!EncodeBinaryCatalogMetadata(fields,"global_aggregate_semantic.v2",&bytes))return {};
  return bytes;
}
}  // namespace scratchbird::engine::internal_api
