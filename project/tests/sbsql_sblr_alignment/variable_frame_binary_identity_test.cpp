// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "sblr_variable_frame_coordinator.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <source_location>
#include <unistd.h>
#include <sys/wait.h>
namespace a=scratchbird::engine::internal_api;
static void Check(bool value,std::source_location at=std::source_location::current()){
  if(!value){std::cerr<<"failure at "<<at.line()<<'\n';std::abort();}
}
static a::EngineUuid Id(unsigned n){a::EngineUuid id;id.bytes={1,144,10,9,0,0,112,0,128,0,0,0,0,0,0,static_cast<std::uint8_t>(n)};return id;}
int main(int argc,char** argv){
  auto pattern=(std::filesystem::temp_directory_path()/"sb-variable-frame-binary-XXXXXX").string();
  auto* root=mkdtemp(pattern.data());Check(root);
  struct Cleanup{std::filesystem::path path;~Cleanup(){std::filesystem::remove_all(path);}}cleanup{root};
  a::EngineRequestContext c;c.database_path=(cleanup.path/"database").string();
  c.database_uuid=Id(1);c.session_uuid=Id(2);c.transaction_uuid=Id(3);c.statement_uuid=Id(4);
  c.catalog_epoch_uuid={{1,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,1}};c.catalog_generation_id=1;
  c.security_context_present=true;c.statement_metadata_snapshot_engine_owned=true;
  c.trace_tags={"private_variable_frame_coordination"};
  a::SblrVariableFrameDemand demand{1,1,true,a::SblrVariableMutability::mutable_value,a::SblrVariableValueState::null_value,"sha256:"+std::string(64,'a')};
  const auto op=Id(5);
  if(argc==2){c.database_path=argv[1];for(int retry=0;retry<2;++retry){auto refused=a::BeginSblrVariableFrame(c,op,1000000,{demand});Check(!refused.ok&&refused.diagnostic.message_key=="sblr.variable_frame.recovery_required");}return 0;}
  auto begun=a::BeginSblrVariableFrame(c,op,1000000,{demand});Check(begun.ok&&!begun.diagnostic.error);
  const auto s=begun.snapshot;Check(s.mappings.size()==1&&!s.mappings[0].datatype_type_uuid.is_nil());
  const auto path=c.database_path+".sb.sblr_variable_frame_coordinator.v2";
  auto read=[&](){std::ifstream file(path,std::ios::binary);return std::string((std::istreambuf_iterator<char>(file)),{});};
  auto write=[&](const std::string& bytes){std::ofstream file(path,std::ios::binary|std::ios::trunc);file.write(bytes.data(),bytes.size());file.close();Check(bool(file));};
  const auto golden=read();Check(golden.substr(4,8)=="SBVFC002"&&golden[12]==1);
  const auto child=fork();Check(child>=0);
  if(child==0){execl(argv[0],argv[0],c.database_path.c_str(),static_cast<char*>(nullptr));_exit(127);}
  int status=0;Check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
  Check(read()==golden);
  std::size_t offset=13;
  for(auto id:{s.public_coordination_uuid,op,c.database_uuid,c.session_uuid,c.transaction_uuid,c.statement_uuid,s.scope_uuid,s.frame_uuid,s.registry_snapshot_uuid}){
    for(unsigned n=0;n<16;++n)Check(static_cast<std::uint8_t>(golden[offset+n])==id.bytes[n]);offset+=16;
  }
  for(unsigned bit=0;bit<128;++bit){
    auto changed=[](a::EngineUuid id,unsigned bit){id.bytes[bit/8]^=1u<<(bit%8);return id;};
    for(auto member:{&a::EngineRequestContext::database_uuid,&a::EngineRequestContext::session_uuid,&a::EngineRequestContext::transaction_uuid}){auto wrong=c;wrong.*member=changed(c.*member,bit);Check(!a::AcquireSblrVariableFrame(wrong,s.public_coordination_uuid,op,s.coordinator_generation).ok);}
    Check(!a::AcquireSblrVariableFrame(c,changed(s.public_coordination_uuid,bit),op,s.coordinator_generation).ok);
    Check(!a::AcquireSblrVariableFrame(c,s.public_coordination_uuid,changed(op,bit),s.coordinator_generation).ok);
  }
  auto admin=c;admin.trace_tags={"right:SBLR_VARIABLE_FRAME_ADMIN"};
  for(std::size_t n=1;n<golden.size();++n){write(golden.substr(0,n));Check(a::RecoverSblrVariableFrameCoordinator(admin).error);Check(read()==golden.substr(0,n));}
  for(std::size_t n=0;n<golden.size();++n){auto damaged=golden;damaged[n]^=1;write(damaged);Check(a::RecoverSblrVariableFrameCoordinator(admin).error);}
  write(golden);std::filesystem::rename(path,path+".saved");std::filesystem::create_directory(path);
  Check(!a::AcquireSblrVariableFrame(c,s.public_coordination_uuid,op,s.coordinator_generation).ok);
  std::filesystem::remove(path);std::filesystem::rename(path+".saved",path);
  auto acquired=a::AcquireSblrVariableFrame(c,s.public_coordination_uuid,op,s.coordinator_generation);Check(acquired.ok);
  const auto& row=acquired.snapshot.mappings[0].descriptor;
  a::SblrVariableAssignment assignment{row.variable_descriptor_uuid,row.variable_descriptor_generation,row.value_generation,a::SblrVariableValueState::value,std::string("\0\n\t",3)};
  auto assigned=a::AssignSblrVariableFrameValues(c,s.public_coordination_uuid,op,c.statement_uuid,acquired.snapshot.coordinator_generation,acquired.snapshot.registry_generation,{assignment});Check(assigned.ok&&assigned.snapshot.mappings[0].descriptor.canonical_value_bytes==assignment.canonical_value_bytes);
  Check(!a::RecoverSblrVariableFrameCoordinator(admin).error);
  Check(!a::AcquireSblrVariableFrame(c,s.public_coordination_uuid,op,s.coordinator_generation).ok);
  Check(!a::RecoverSblrVariableFrameCoordinator(admin).error);
  auto next=a::BeginSblrVariableFrame(c,Id(6),1000000,{demand});Check(next.ok);
  auto closed=a::CloseSblrVariableFrame(c,next.snapshot.public_coordination_uuid,Id(6),next.snapshot.frame_generation,"frame.close");Check(closed.ok&&closed.snapshot.state==a::SblrVariableFrameState::revoked);
  auto legacy=c;legacy.database_uuid=Id(9);legacy.database_path=(cleanup.path/"legacy").string();
  {std::ofstream f(legacy.database_path+".sb.sblr_variable_frame_coordinator.v1");f<<"SBVFC1\tTEXT UUID journal\n";}
  Check(!a::BeginSblrVariableFrame(legacy,Id(7),1000000,{demand}).ok);
  Check(!std::filesystem::exists(legacy.database_path+".sb.sblr_variable_frame_coordinator.v2"));
}
