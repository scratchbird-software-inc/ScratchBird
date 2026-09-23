// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "api_behavior_store.hpp"
#include "mga_relation_store/mga_binary_fields.hpp"
#include "hash_digest.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <array>
#include <istream>
namespace scratchbird::engine::internal_api {
inline constexpr std::size_t kApiBehaviorRecordMaximumBytes=64*1024*1024;
inline bool EncodeApiBehaviorRecord(const ApiBehaviorRecord& record,std::string* output){
  if(!output||!core::uuid::IsEngineIdentityUuid(record.object_uuid)||record.operation_id.empty()||record.object_kind.empty()||record.state.empty())return false;
  std::string payload(kApiBehaviorEventMagic);
  AppendBinaryU64(&payload,record.creator_tx);
  for(const auto& id:{record.object_uuid,record.target_database_uuid,record.target_schema_uuid,record.target_object_uuid}){
    if(!id.is_nil()&&!core::uuid::IsEngineIdentityUuid(id))return false;
    payload.append(reinterpret_cast<const char*>(id.bytes.data()),16);
  }
  for(const auto* field:{&record.operation_id,&record.object_kind,&record.default_name,&record.payload,&record.state}){
    if(field->size()>kApiBehaviorRecordMaximumBytes||payload.size()>kApiBehaviorRecordMaximumBytes-field->size()||!AppendBinaryString(&payload,*field))return false;
  }
  AppendBinaryU8(&payload,record.deleted?1:0);
  if(payload.size()>kApiBehaviorRecordMaximumBytes-32)return false;
  const auto hash=core::hash::ComputeSha256Digest(reinterpret_cast<const std::uint8_t*>(payload.data()),payload.size());
  if(!hash.ok())return false;
  payload.append(reinterpret_cast<const char*>(hash.digest.data()),32);
  std::string framed;AppendBinaryU32(&framed,static_cast<std::uint32_t>(payload.size()));framed+=payload;output->swap(framed);return true;
}
inline bool DecodeApiBehaviorRecord(std::span<const std::uint8_t> framed,ApiBehaviorRecord* output){
  if(!output)return false;
  std::size_t cursor=0;std::uint32_t length=0;
  if(!ReadBinaryU32(framed,&cursor,&length)||length>kApiBehaviorRecordMaximumBytes||length!=framed.size()-4||length<8+8+64+20+1+32)return false;
  const auto bytes=framed.subspan(4);const auto payload=bytes.first(bytes.size()-32);
  if(std::string_view(reinterpret_cast<const char*>(payload.data()),8)!=kApiBehaviorEventMagic)return false;
  const auto hash=core::hash::ComputeSha256Digest(payload.data(),payload.size());
  if(!hash.ok()||!std::equal(hash.digest.begin(),hash.digest.end(),bytes.end()-32))return false;
  cursor=8;ApiBehaviorRecord candidate;
  if(!ReadBinaryU64(payload,&cursor,&candidate.creator_tx))return false;
  for(auto* id:{&candidate.object_uuid,&candidate.target_database_uuid,&candidate.target_schema_uuid,&candidate.target_object_uuid}){
    if(payload.size()-cursor<16)return false;
    std::copy_n(payload.begin()+cursor,16,id->bytes.begin());cursor+=16;
    if(!id->is_nil()&&!core::uuid::IsEngineIdentityUuid(*id))return false;
  }
  if(candidate.object_uuid.is_nil())return false;
  for(auto* field:{&candidate.operation_id,&candidate.object_kind,&candidate.default_name,&candidate.payload,&candidate.state})if(!ReadBinaryString(payload,&cursor,field))return false;
  std::uint8_t deleted=0;
  if(!ReadBinaryU8(payload,&cursor,&deleted)||deleted>1||cursor!=payload.size()||candidate.operation_id.empty()||candidate.object_kind.empty()||candidate.state.empty())return false;
  candidate.deleted=deleted!=0;*output=std::move(candidate);return true;
}
inline bool ReadApiBehaviorRecord(std::istream& input,ApiBehaviorRecord* output){
  std::array<std::uint8_t,4> header{};if(!input.read(reinterpret_cast<char*>(header.data()),4))return false;
  std::size_t cursor=0;std::uint32_t length=0;
  if(!ReadBinaryU32(header,&cursor,&length)||length>kApiBehaviorRecordMaximumBytes||length<133)return false;
  std::string framed(reinterpret_cast<const char*>(header.data()),4);framed.resize(4+length);
  if(!input.read(framed.data()+4,length))return false;
  return DecodeApiBehaviorRecord({reinterpret_cast<const std::uint8_t*>(framed.data()),framed.size()},output);
}
} // namespace scratchbird::engine::internal_api
