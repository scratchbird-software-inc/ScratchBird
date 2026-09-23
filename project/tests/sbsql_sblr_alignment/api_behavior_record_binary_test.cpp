// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "behavior_support/api_behavior_record_codec.hpp"
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <source_location>
namespace a=scratchbird::engine::internal_api;
static void Check(bool value,std::source_location at=std::source_location::current()){
  if(!value){std::cerr<<"failure at "<<at.line()<<'\n';std::abort();}
}
static a::EngineUuid Id(unsigned n){a::EngineUuid id;id.bytes={1,144,10,9,0,0,112,0,128,0,0,0,0,0,0,static_cast<std::uint8_t>(n)};return id;}
int main(){
  a::ApiBehaviorRecord r;r.creator_tx=0x0102030405060708;r.object_uuid=Id(1);r.target_database_uuid=Id(2);r.target_schema_uuid=Id(3);r.target_object_uuid=Id(4);r.operation_id="catalog.create";r.object_kind="schema";r.default_name=std::string("name\0\t\n",7);r.payload=std::string("\xff\0\n;=",5);r.state="active";
  std::string bytes;Check(a::EncodeApiBehaviorRecord(r,&bytes));Check(bytes.substr(4,8)=="SBAPI002");
  for(unsigned n=0;n<8;++n)Check(static_cast<std::uint8_t>(bytes[12+n])==(r.creator_tx>>(8*n)&255));
  std::size_t offset=20;for(auto id:{r.object_uuid,r.target_database_uuid,r.target_schema_uuid,r.target_object_uuid}){for(unsigned n=0;n<16;++n)Check(static_cast<std::uint8_t>(bytes[offset+n])==id.bytes[n]);offset+=16;}
  auto decode=[](std::string_view bytes,a::ApiBehaviorRecord* out){return a::DecodeApiBehaviorRecord({reinterpret_cast<const std::uint8_t*>(bytes.data()),bytes.size()},out);};
  a::ApiBehaviorRecord read;Check(decode(bytes,&read));Check(read.object_uuid==r.object_uuid&&read.target_database_uuid==r.target_database_uuid&&read.target_schema_uuid==r.target_schema_uuid&&read.target_object_uuid==r.target_object_uuid);Check(read.payload==r.payload&&read.default_name==r.default_name&&read.creator_tx==r.creator_tx);
  std::string repeated;Check(a::EncodeApiBehaviorRecord(read,&repeated)&&repeated==bytes);
  auto reject=[&](const std::string& damaged){a::ApiBehaviorRecord out;out.object_uuid=Id(77);out.payload="unchanged";Check(!decode(damaged,&out));Check(out.object_uuid==Id(77)&&out.payload=="unchanged");};
  for(std::size_t n=0;n<bytes.size();++n)reject(bytes.substr(0,n));
  for(std::size_t n=0;n<bytes.size();++n){auto damaged=bytes;damaged[n]^=1;reject(damaged);}
  reject(bytes+"x");reject("SBAPI1\tRECORD\t0\tcreate\t01900000-0000-7000-8000-000000000001\tschema\t\t\tactive\t0\n");
  auto seal=[](std::string record){auto h=scratchbird::core::hash::ComputeSha256Digest(reinterpret_cast<const std::uint8_t*>(record.data()+4),record.size()-36);Check(h.ok());record.replace(record.size()-32,32,reinterpret_cast<const char*>(h.digest.data()),32);return record;};
  auto invalid=bytes;invalid.replace(20,16,16,'\0');reject(seal(invalid));invalid=bytes;invalid[26]=0x40;reject(seal(invalid));invalid=bytes;invalid[bytes.size()-33]=2;reject(seal(invalid));
  std::istringstream stream(bytes+bytes,std::ios::binary);Check(a::ReadApiBehaviorRecord(stream,&read));Check(a::ReadApiBehaviorRecord(stream,&read));Check(stream.peek()==std::char_traits<char>::eof());
  r.target_schema_uuid={};Check(a::EncodeApiBehaviorRecord(r,&bytes));Check(decode(bytes,&read)&&read.target_schema_uuid.is_nil());
  auto saved=bytes;r.object_uuid={};Check(!a::EncodeApiBehaviorRecord(r,&bytes)&&bytes==saved);
}
