// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "sblr_transaction_begin_authority.hpp"
#include "hash_digest.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <source_location>
namespace a=scratchbird::engine::internal_api;
static void Check(bool value,std::source_location at=std::source_location::current()){
  if(!value){std::cerr<<"failure at "<<at.line()<<'\n';std::abort();}
}
static a::EngineUuid Id(unsigned n){a::EngineUuid id;id.bytes={1,144,0,0,0,0,112,0,128,0,0,0,0,0,0,static_cast<std::uint8_t>(n)};return id;}
static std::string Unhex(std::string_view text){std::string out;auto nibble=[](char c){return c<='9'?c-'0':c-'a'+10;};for(std::size_t n=0;n<text.size();n+=2)out.push_back(static_cast<char>((nibble(text[n])<<4)|nibble(text[n+1])));return out;}
int main(){
  auto pattern=(std::filesystem::temp_directory_path()/"sb-begin-policy-binary-XXXXXX").string();
  auto* root=mkdtemp(pattern.data());Check(root);
  struct Cleanup{std::filesystem::path path;~Cleanup(){std::filesystem::remove_all(path);}}cleanup{root};
  a::EngineRequestContext c;c.database_path=(cleanup.path/"database").string();c.database_uuid=Id(1);
  c.security_context_present=true;c.statement_metadata_snapshot_engine_owned=true;
  auto first=a::LoadSblrTransactionBeginAuthorityV1(c);Check(first.ok&&!first.diagnostic.error);
  auto second=a::LoadSblrTransactionBeginAuthorityV1(c);Check(second.ok);
  Check(first.authority.isolation_profile_uuid==second.authority.isolation_profile_uuid);
  Check(first.authority.transaction_policy_snapshot_uuid==second.authority.transaction_policy_snapshot_uuid);
  const auto path=c.database_path+".sb.sblr_txn_begin_authority.v2";
  auto read=[&](){std::ifstream file(path,std::ios::binary);return std::string((std::istreambuf_iterator<char>(file)),{});};
  const auto created=read();Check(created.size()==104&&created.substr(0,8)=="SBTBEG02");
  for(unsigned n=0;n<16;++n){
    Check(static_cast<std::uint8_t>(created[8+n])==c.database_uuid.bytes[n]);
    Check(static_cast<std::uint8_t>(created[24+n])==first.authority.isolation_profile_uuid.bytes[n]);
    Check(static_cast<std::uint8_t>(created[48+n])==first.authority.transaction_policy_snapshot_uuid.bytes[n]);
  }
  auto write=[&](const std::string& bytes){std::ofstream file(path,std::ios::binary|std::ios::trunc);file.write(bytes.data(),bytes.size());file.close();Check(bool(file));};
  // Python struct/hashlib golden, independent of the implementation writer.
  const auto golden=Unhex("534254424547303201900000000070008000000000000001019000000000700080000000000000021100000000000000019000000000700080000000000000031700000000000000573aa8998a5eeada3ae3565558c748f00d22d6f3f848ff3008d4d58ad8ff016e");write(golden);
  const auto known=a::LoadSblrTransactionBeginAuthorityV1(c);Check(known.ok);
  Check(known.authority.isolation_profile_uuid==Id(2)&&known.authority.isolation_profile_generation==17);
  Check(known.authority.transaction_policy_snapshot_uuid==Id(3)&&known.authority.transaction_policy_generation==23);
  for(unsigned bit=0;bit<128;++bit){auto wrong=c;wrong.database_uuid.bytes[bit/8]^=1u<<(bit%8);Check(!a::LoadSblrTransactionBeginAuthorityV1(wrong).ok);}
  auto reject=[&](const std::string& bytes){write(bytes);auto result=a::LoadSblrTransactionBeginAuthorityV1(c);Check(!result.ok&&result.diagnostic.error&&result.authority.isolation_profile_uuid.is_nil());};
  for(std::size_t n=0;n<golden.size();++n)reject(golden.substr(0,n));
  for(std::size_t n=0;n<golden.size();++n){auto damaged=golden;damaged[n]^=1;reject(damaged);}
  reject(golden+"x");
  auto seal=[](std::string bytes){auto digest=scratchbird::core::hash::ComputeSha256Digest(reinterpret_cast<const std::uint8_t*>(bytes.data()),72);Check(digest.ok());bytes.replace(72,32,reinterpret_cast<const char*>(digest.digest.data()),32);return bytes;};
  auto invalid=golden;invalid.replace(40,8,8,'\0');reject(seal(invalid));
  invalid=golden;invalid[30]=0x40;reject(seal(invalid));
  write(golden);Check(a::LoadSblrTransactionBeginAuthorityV1(c).ok);
  auto legacy=c;legacy.database_path=(cleanup.path/"legacy").string();
  {std::ofstream f(legacy.database_path+".sb.sblr_txn_begin_authority.v1");f<<"01900000-0000-7000-8000-000000000002 1 01900000-0000-7000-8000-000000000003 1\n";}
  Check(!a::LoadSblrTransactionBeginAuthorityV1(legacy).ok);
  Check(!std::filesystem::exists(legacy.database_path+".sb.sblr_txn_begin_authority.v2"));
  auto missing=c;missing.database_path=(cleanup.path/"absent"/"database").string();
  auto failed=a::LoadSblrTransactionBeginAuthorityV1(missing);Check(!failed.ok&&failed.authority.isolation_profile_uuid.is_nil());
}
