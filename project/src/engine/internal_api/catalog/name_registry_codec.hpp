// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "name_registry.hpp"
#include "behavior_support/api_behavior_record_codec.hpp"
namespace scratchbird::engine::internal_api {
inline bool EncodeNameRegistryEntry(const NameRegistryEntry& e,std::string* output){
  if(!output||!core::uuid::IsEngineIdentityUuid(e.name_entry_uuid)||!core::uuid::IsEngineIdentityUuid(e.object_uuid))return false;
  std::string bytes(kNameRegistryEventMagic);
  if(!e.name_entry_uuid.is_nil()&&!core::uuid::IsEngineIdentityUuid(e.name_entry_uuid))return false;
  bytes.append(reinterpret_cast<const char*>(e.name_entry_uuid.bytes.data()),16);
  if(!e.object_uuid.is_nil()&&!core::uuid::IsEngineIdentityUuid(e.object_uuid))return false;
  bytes.append(reinterpret_cast<const char*>(e.object_uuid.bytes.data()),16);
  if(!e.scope_uuid.is_nil()&&!core::uuid::IsEngineIdentityUuid(e.scope_uuid))return false;
  bytes.append(reinterpret_cast<const char*>(e.scope_uuid.bytes.data()),16);
  if(!e.parent_object_uuid.is_nil()&&!core::uuid::IsEngineIdentityUuid(e.parent_object_uuid))return false;
  bytes.append(reinterpret_cast<const char*>(e.parent_object_uuid.bytes.data()),16);
  if(!e.parent_schema_uuid.is_nil()&&!core::uuid::IsEngineIdentityUuid(e.parent_schema_uuid))return false;
  bytes.append(reinterpret_cast<const char*>(e.parent_schema_uuid.bytes.data()),16);
  AppendBinaryU64(&bytes,e.creator_tx);
  AppendBinaryU64(&bytes,e.catalog_generation_id);
  AppendBinaryU64(&bytes,e.resource_epoch);
  AppendBinaryU64(&bytes,e.name_resolution_epoch);
  AppendBinaryU8(&bytes,e.was_quoted?1:0);
  AppendBinaryU8(&bytes,e.requires_exact_match?1:0);
  AppendBinaryU8(&bytes,e.deleted?1:0);
  if(!AppendBinaryString(&bytes,e.object_class))return false;
  if(!AppendBinaryString(&bytes,e.language_tag))return false;
  if(!AppendBinaryString(&bytes,e.name_class))return false;
  if(!AppendBinaryString(&bytes,e.reference_id))return false;
  if(!AppendBinaryString(&bytes,e.dialect_profile_uuid))return false;
  if(!AppendBinaryString(&bytes,e.identifier_profile_uuid))return false;
  if(!AppendBinaryString(&bytes,e.case_fold_profile_uuid))return false;
  if(!AppendBinaryString(&bytes,e.quoted_identifier_profile_uuid))return false;
  if(!AppendBinaryString(&bytes,e.raw_name_text))return false;
  if(!AppendBinaryString(&bytes,e.display_name))return false;
  if(!AppendBinaryString(&bytes,e.quote_style))return false;
  if(!AppendBinaryString(&bytes,e.normalized_lookup_key))return false;
  if(!AppendBinaryString(&bytes,e.exact_lookup_key))return false;
  if(!AppendBinaryString(&bytes,e.full_path_lookup_key))return false;
  if(!AppendBinaryString(&bytes,e.lifecycle_state))return false;
  if(bytes.size()>kApiBehaviorRecordMaximumBytes)return false;
  output->swap(bytes);return true;
}
inline bool DecodeNameRegistryEntry(std::string_view value,NameRegistryEntry* output){
  if(!output||value.size()<8||value.substr(0,8)!=kNameRegistryEventMagic)return false;
  std::span<const std::uint8_t> bytes(reinterpret_cast<const std::uint8_t*>(value.data()),value.size());std::size_t cursor=8;NameRegistryEntry e;
  if(cursor>bytes.size()||bytes.size()-cursor<16)return false;
  std::copy_n(bytes.begin()+cursor,16,e.name_entry_uuid.bytes.begin());cursor+=16;
  if(!e.name_entry_uuid.is_nil()&&!core::uuid::IsEngineIdentityUuid(e.name_entry_uuid))return false;
  if(cursor>bytes.size()||bytes.size()-cursor<16)return false;
  std::copy_n(bytes.begin()+cursor,16,e.object_uuid.bytes.begin());cursor+=16;
  if(!e.object_uuid.is_nil()&&!core::uuid::IsEngineIdentityUuid(e.object_uuid))return false;
  if(cursor>bytes.size()||bytes.size()-cursor<16)return false;
  std::copy_n(bytes.begin()+cursor,16,e.scope_uuid.bytes.begin());cursor+=16;
  if(!e.scope_uuid.is_nil()&&!core::uuid::IsEngineIdentityUuid(e.scope_uuid))return false;
  if(cursor>bytes.size()||bytes.size()-cursor<16)return false;
  std::copy_n(bytes.begin()+cursor,16,e.parent_object_uuid.bytes.begin());cursor+=16;
  if(!e.parent_object_uuid.is_nil()&&!core::uuid::IsEngineIdentityUuid(e.parent_object_uuid))return false;
  if(cursor>bytes.size()||bytes.size()-cursor<16)return false;
  std::copy_n(bytes.begin()+cursor,16,e.parent_schema_uuid.bytes.begin());cursor+=16;
  if(!e.parent_schema_uuid.is_nil()&&!core::uuid::IsEngineIdentityUuid(e.parent_schema_uuid))return false;
  if(!ReadBinaryU64(bytes,&cursor,&e.creator_tx))return false;
  if(!ReadBinaryU64(bytes,&cursor,&e.catalog_generation_id))return false;
  if(!ReadBinaryU64(bytes,&cursor,&e.resource_epoch))return false;
  if(!ReadBinaryU64(bytes,&cursor,&e.name_resolution_epoch))return false;
  std::uint8_t was_quoted=0;if(!ReadBinaryU8(bytes,&cursor,&was_quoted)||was_quoted>1)return false;e.was_quoted=was_quoted!=0;
  std::uint8_t requires_exact_match=0;if(!ReadBinaryU8(bytes,&cursor,&requires_exact_match)||requires_exact_match>1)return false;e.requires_exact_match=requires_exact_match!=0;
  std::uint8_t deleted=0;if(!ReadBinaryU8(bytes,&cursor,&deleted)||deleted>1)return false;e.deleted=deleted!=0;
  if(!ReadBinaryString(bytes,&cursor,&e.object_class))return false;
  if(!ReadBinaryString(bytes,&cursor,&e.language_tag))return false;
  if(!ReadBinaryString(bytes,&cursor,&e.name_class))return false;
  if(!ReadBinaryString(bytes,&cursor,&e.reference_id))return false;
  if(!ReadBinaryString(bytes,&cursor,&e.dialect_profile_uuid))return false;
  if(!ReadBinaryString(bytes,&cursor,&e.identifier_profile_uuid))return false;
  if(!ReadBinaryString(bytes,&cursor,&e.case_fold_profile_uuid))return false;
  if(!ReadBinaryString(bytes,&cursor,&e.quoted_identifier_profile_uuid))return false;
  if(!ReadBinaryString(bytes,&cursor,&e.raw_name_text))return false;
  if(!ReadBinaryString(bytes,&cursor,&e.display_name))return false;
  if(!ReadBinaryString(bytes,&cursor,&e.quote_style))return false;
  if(!ReadBinaryString(bytes,&cursor,&e.normalized_lookup_key))return false;
  if(!ReadBinaryString(bytes,&cursor,&e.exact_lookup_key))return false;
  if(!ReadBinaryString(bytes,&cursor,&e.full_path_lookup_key))return false;
  if(!ReadBinaryString(bytes,&cursor,&e.lifecycle_state))return false;
  if(cursor!=bytes.size()||e.name_entry_uuid.is_nil()||e.object_uuid.is_nil()||e.object_class.empty()||e.lifecycle_state.empty())return false;
  *output=std::move(e);return true;
}
} // namespace scratchbird::engine::internal_api
