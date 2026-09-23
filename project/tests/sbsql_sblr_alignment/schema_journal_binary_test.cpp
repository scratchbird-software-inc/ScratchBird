// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/engine/internal_api/sblr_ddl_create_schema_execution_journal.cpp"
#include <cstdlib>
#include <iostream>
#include <source_location>
#if !defined(_WIN32)
#include <sys/wait.h>
#endif
namespace a=scratchbird::engine::internal_api;
namespace s=scratchbird::engine::sblr;
static void Check(bool value,std::source_location at=std::source_location::current()) {
  if(!value){std::cerr<<"failure at "<<at.line()<<'\n';std::abort();}
}
static std::array<std::uint8_t,16> Id(unsigned n){
  return {1,144,10,9,0,59,112,0,128,0,0,0,0,0,0,static_cast<std::uint8_t>(n)};
}
static a::SblrDdlCreateSchemaJournalKeyV1 Key(unsigned n){
  s::SblrDdlCreateSchemaDescriptorV1 d;
  d.receipt=Id(1);d.occurrence=1;d.schema_occurrence=1;d.schema_uuid=Id(n+40);d.schema_generation=1;
  d.database_uuid=Id(2);d.owning_transaction_uuid=Id(3);d.owning_local_transaction_id=7;
  d.statement_snapshot_uuid=Id(4);d.catalog_epoch_uuid=Id(5);d.catalog_generation=11;
  d.security_context_uuid=Id(6);d.security_epoch=13;d.policy_snapshot_uuid=Id(7);d.policy_generation=17;
  d.resource_grant_uuid=Id(8);d.resource_generation=19;d.owner_principal_uuid=Id(9);
  d.binding_uuid=Id(n+80);d.recovery_uuid=Id(n+120);d.binding_generation=1;d.recovery_generation=1;
  d.normalized_path_sha256.fill(0x11);d.syntax_demand_sha256.fill(0x21);d.authorization_evidence_sha256.fill(0x31);
  d.availability=23;
  a::SblrDdlCreateSchemaJournalKeyV1 key;
  key.database_uuid=d.database_uuid;key.recovery_uuid=d.recovery_uuid;
  key.canonical_descriptor_bytes=s::EncodeSblrDdlCreateSchemaDescriptorV1(d,true);
  Check(!key.canonical_descriptor_bytes.empty());return key;
}
int main(){
  const auto directory=std::filesystem::temp_directory_path()/("sb_schema_binary_"+std::to_string(a::ProcessOrdinal()));
  Check(std::filesystem::create_directory(directory));
  a::EngineRequestContext context;context.database_path=(directory/"database.sbdb").string();context.database_uuid=a::NativeUuid(Id(2));
  std::vector<a::SblrDdlCreateSchemaJournalKeyV1> keys;
  a::JournalRecords records;
  for(unsigned n=1;n<=6;++n){
    keys.push_back(Key(n));a::SblrDdlCreateSchemaJournalSnapshotV1 snapshot;
    Check(a::MakeBegunSnapshot(keys.back(),&snapshot));records.push_back(a::Encode(snapshot));Check(!records.back().empty());
  }
  const auto encoded=a::EncodeContainer({records[0],records[1]});
  Check(!encoded.empty());a::JournalRecords decoded;
  Check(a::DecodeContainer(encoded,&decoded)&&decoded==a::JournalRecords({records[0],records[1]}));
  for(std::size_t n=0;n<encoded.size();++n){
    auto damaged=encoded;damaged[n]^=1;Check(!a::DecodeContainer(damaged,&decoded));
    Check(!a::DecodeContainer({encoded.begin(),encoded.begin()+n},&decoded));
  }
  Check(a::EncodeContainer({records[0],records[0]}).empty());
  Check(a::Path(context,keys[0])==a::Path(context,keys[1]));
  {
    a::ScopedFileLock lock;Check(lock.Acquire(a::Path(context,keys[0])+".lock"));
    Check(a::StoreRecord(context,keys[0],records[0],false)==a::CreateStatus::created);
    Check(a::StoreRecord(context,keys[1],records[1],false)==a::CreateStatus::created);
    Check(a::StoreRecord(context,keys[0],records[0],false)==a::CreateStatus::exists);
    std::vector<std::uint8_t> read;
    Check(a::ReadRecord(context,keys[1],&read)==a::ReadStatus::ok&&read==records[1]);
    a::SblrDdlCreateSchemaJournalSnapshotV1 next;Check(a::Decode(records[0],&next));
    next.state=a::SblrDdlCreateSchemaJournalStateV1::published;++next.journal_generation;next.record_evidence_sha256={};
    records[0]=a::Encode(next);Check(!records[0].empty());
    Check(a::StoreRecord(context,keys[0],records[0],true)==a::CreateStatus::created);
    Check(a::ReadRecord(context,keys[0],&read)==a::ReadStatus::ok&&read==records[0]);
    Check(a::ReadRecord(context,keys[1],&read)==a::ReadStatus::ok&&read==records[1]);
    Check(a::StoreRecord(context,keys[2],records[2],true)==a::CreateStatus::failed);
  }
#if !defined(_WIN32)
  std::vector<pid_t> children;
  for(std::size_t n=2;n<keys.size();++n){
    const pid_t child=::fork();Check(child>=0);
    if(child==0){a::ScopedFileLock lock;const bool ok=lock.Acquire(a::Path(context,keys[n])+".lock")&&a::StoreRecord(context,keys[n],records[n],false)==a::CreateStatus::created;::_exit(ok?0:1);}
    children.push_back(child);
  }
  for(const auto child:children){int status=0;Check(::waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);}
#else
  for(std::size_t n=2;n<keys.size();++n){a::ScopedFileLock lock;Check(lock.Acquire(a::Path(context,keys[n])+".lock"));Check(a::StoreRecord(context,keys[n],records[n],false)==a::CreateStatus::created);}
#endif
  {
    a::ScopedFileLock lock;Check(lock.Acquire(a::Path(context,keys[0])+".lock"));
    for(std::size_t n=0;n<keys.size();++n){std::vector<std::uint8_t> read;Check(a::ReadRecord(context,keys[n],&read)==a::ReadStatus::ok&&read==records[n]);}
    std::ofstream legacy(context.database_path+".sb.sblr_ddl_create_schema_execution_journal.v1.legacy");legacy<<"old";legacy.close();
    std::vector<std::uint8_t> read;Check(a::ReadRecord(context,keys[0],&read)==a::ReadStatus::invalid);
  }
  std::filesystem::remove_all(directory);
}
