// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "sblr_temporary_instance_cleanup_coordinator.hpp"
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
int main(){
  auto pattern=(std::filesystem::temp_directory_path()/"sb-temp-instance-binary-XXXXXX").string();
  auto* root=mkdtemp(pattern.data());Check(root);
  struct Cleanup{std::filesystem::path path;~Cleanup(){std::filesystem::remove_all(path);}}cleanup{root};
  a::EngineRequestContext c;c.database_path=(cleanup.path/"database").string();
  c.database_uuid=Id(1);c.transaction_uuid=Id(2);c.statement_uuid=Id(3);c.session_uuid=Id(4);c.local_transaction_id=11;
  c.security_context_present=true;c.statement_metadata_snapshot_engine_owned=true;
  c.trace_tags={"private_temporary_instance_compiler","private_temporary_instance_cleanup","right:SBLR_TEMPORARY_INSTANCE_ADMIN"};
  auto published=a::PublishSblrTemporaryInstance(c,c.statement_uuid,1,1);Check(published.ok&&!published.diagnostic.error);
  auto coordinated=a::CoordinateSblrTemporaryInstanceCleanup(c,c.statement_uuid,1,1,7);Check(coordinated.ok);
  const auto p=coordinated.snapshot;
  Check(!a::CoordinateSblrTemporaryInstanceCleanup(c,c.statement_uuid,1,1,0).ok);
  auto clean=[&](const auto& context,const auto& snapshot){return a::CleanupSblrTemporaryInstance(context,snapshot.descriptor_uuid,snapshot.descriptor_generation,snapshot.descriptor_evidence_sha256,7);};
  for(unsigned bit=0;bit<128;++bit){
    auto wrong=c;wrong.database_uuid.bytes[bit/8]^=1u<<(bit%8);Check(!clean(wrong,p).ok);
    wrong=c;wrong.session_uuid.bytes[bit/8]^=1u<<(bit%8);Check(!clean(wrong,p).ok);
    wrong=c;wrong.transaction_uuid.bytes[bit/8]^=1u<<(bit%8);Check(!clean(wrong,p).ok);
    auto wrong_snapshot=p;wrong_snapshot.descriptor_uuid.bytes[bit/8]^=1u<<(bit%8);Check(!clean(c,wrong_snapshot).ok);
  }
  const auto journal=c.database_path+".sb.sblr_temporary_instance.v2";const auto saved=cleanup.path/"saved";
  std::filesystem::rename(journal,saved);std::filesystem::create_directory(journal);
  Check(!clean(c,p).ok);
  Check(a::CoordinateSblrTemporaryInstanceCleanup(c,c.statement_uuid,1,1,7).ok);
  std::filesystem::remove(journal);std::filesystem::rename(saved,journal);
  const auto cleaned=clean(c,p);Check(cleaned.ok&&!cleaned.diagnostic.error);Check(!clean(c,p).ok);
  Check(!std::filesystem::exists(c.database_path+".sb.sblr_temporary_instance.v1"));
  std::ifstream file(journal,std::ios::binary);std::string bytes((std::istreambuf_iterator<char>(file)),{});
  Check(bytes.size()==392);
  for(unsigned n=0;n<2;++n){
    auto offset=n*196;Check(bytes.substr(offset,8)=="SBTINST2"&&bytes[offset+8]==(n?'C':'P'));
    unsigned field=0;
    for(const auto& identity:{c.database_uuid,c.statement_uuid,p.descriptor_uuid,p.definition_uuid,p.instance_uuid,c.session_uuid,c.transaction_uuid}){
      for(unsigned j=0;j<16;++j)Check(static_cast<std::uint8_t>(bytes[offset+9+field*16+j])==identity.bytes[j]);
      ++field;
    }
    Check(static_cast<std::uint8_t>(bytes[offset+195])==(n?2:1));
  }
  c.statement_uuid=Id(5);auto session_instance=a::PublishSblrTemporaryInstance(c,c.statement_uuid,2,2);Check(session_instance.ok&&session_instance.snapshot.owner_transaction_uuid.is_nil());
  c.transaction_uuid=Id(6);Check(a::CoordinateSblrTemporaryInstanceCleanup(c,c.statement_uuid,2,2,7).ok);
  auto other=c;other.database_uuid=Id(7);other.database_path=(cleanup.path/"other").string();
  Check(a::PublishSblrTemporaryInstance(other,other.statement_uuid,2,2).ok);
  std::filesystem::rename(journal,saved);std::filesystem::create_directory(journal);
  Check(a::RecoverSblrTemporaryInstances(c).error);
  Check(a::CoordinateSblrTemporaryInstanceCleanup(c,c.statement_uuid,2,2,7).ok);
  std::filesystem::remove(journal);std::filesystem::rename(saved,journal);
  Check(!a::RecoverSblrTemporaryInstances(c).error);
  Check(!a::CoordinateSblrTemporaryInstanceCleanup(c,c.statement_uuid,2,2,7).ok);
  Check(a::CoordinateSblrTemporaryInstanceCleanup(other,other.statement_uuid,2,2,7).ok);
  Check(!a::RecoverSblrTemporaryInstances(other).error);
}
