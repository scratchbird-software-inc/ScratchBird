// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "sblr_variable_descriptor_registry.hpp"
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
  auto pattern=(std::filesystem::temp_directory_path()/"sb-variable-binary-XXXXXX").string();
  auto* root=mkdtemp(pattern.data());Check(root);
  struct Cleanup{std::filesystem::path path;~Cleanup(){std::filesystem::remove_all(path);}}cleanup{root};
  a::EngineRequestContext c;c.database_path=(cleanup.path/"database").string();
  c.database_uuid=Id(1);c.session_uuid=Id(2);c.transaction_uuid=Id(3);c.statement_uuid=Id(4);
  c.security_context_present=true;c.statement_metadata_snapshot_engine_owned=true;
  c.trace_tags={"private_variable_registry","canonical_datatype_value_validated"};
  const auto scope=Id(5),frame=Id(6);
  a::SblrVariableDemand demand;demand.datatype_descriptor_uuid=Id(7);demand.datatype_descriptor_generation=1;
  demand.nullable=true;demand.mutability=a::SblrVariableMutability::mutable_value;
  demand.initial_state=a::SblrVariableValueState::value;demand.canonical_value_bytes=std::string("\0\n\t\xff",4);
  if(argc==2){c.database_path=argv[1];for(int retry=0;retry<2;++retry){auto refused=a::PublishSblrVariableFrame(c,c.statement_uuid,scope,1,frame,1,{demand});Check(!refused.ok&&refused.diagnostic.message_key=="sblr.variable.recovery_required");}return 0;}
  auto published=a::PublishSblrVariableFrame(c,c.statement_uuid,scope,1,frame,1,{demand});
  Check(published.ok&&!published.diagnostic.error&&published.rows.size()==1);
  const auto row=published.rows[0];
  const auto path=c.database_path+".sb.sblr_variable_registry.v2";
  auto read=[&](){std::ifstream file(path,std::ios::binary);return std::string((std::istreambuf_iterator<char>(file)),{});};
  auto write=[&](const std::string& bytes){std::ofstream file(path,std::ios::binary|std::ios::trunc);file.write(bytes.data(),bytes.size());file.close();Check(bool(file));};
  const auto golden=read();Check(golden.substr(4,8)=="SBVDR002"&&golden[12]==1);
  const auto child=fork();Check(child>=0);
  if(child==0){execl(argv[0],argv[0],c.database_path.c_str(),static_cast<char*>(nullptr));_exit(127);}
  int status=0;Check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
  Check(read()==golden);
  std::size_t offset=13;
  for(auto id:{row.variable_descriptor_uuid,c.statement_uuid,c.database_uuid,c.session_uuid,c.transaction_uuid,scope,frame,demand.datatype_descriptor_uuid}){
    for(unsigned n=0;n<16;++n)Check(static_cast<std::uint8_t>(golden[offset+n])==id.bytes[n]);offset+=16;
  }
  auto lookup=[&](const a::EngineRequestContext& context,const a::EngineUuid& receipt,const a::EngineUuid& sc,const a::EngineUuid& fr,const a::EngineUuid& variable,std::uint64_t generation){return a::LookupSblrVariable(context,receipt,sc,1,fr,1,variable,1,generation);};
  Check(lookup(c,c.statement_uuid,scope,frame,row.variable_descriptor_uuid,1).row.canonical_value_bytes==demand.canonical_value_bytes);
  for(unsigned bit=0;bit<128;++bit){
    auto changed=[](a::EngineUuid id,unsigned bit){id.bytes[bit/8]^=1u<<(bit%8);return id;};
    auto wrong=c;wrong.database_uuid=changed(c.database_uuid,bit);Check(!lookup(wrong,c.statement_uuid,scope,frame,row.variable_descriptor_uuid,1).ok);
    wrong=c;wrong.session_uuid=changed(c.session_uuid,bit);Check(!lookup(wrong,c.statement_uuid,scope,frame,row.variable_descriptor_uuid,1).ok);
    wrong=c;wrong.transaction_uuid=changed(c.transaction_uuid,bit);Check(!lookup(wrong,c.statement_uuid,scope,frame,row.variable_descriptor_uuid,1).ok);
    Check(!lookup(c,changed(c.statement_uuid,bit),scope,frame,row.variable_descriptor_uuid,1).ok);
    Check(!lookup(c,c.statement_uuid,changed(scope,bit),frame,row.variable_descriptor_uuid,1).ok);
    Check(!lookup(c,c.statement_uuid,scope,changed(frame,bit),row.variable_descriptor_uuid,1).ok);
    Check(!lookup(c,c.statement_uuid,scope,frame,changed(row.variable_descriptor_uuid,bit),1).ok);
  }
  auto admin=c;admin.trace_tags={"right:SBLR_VARIABLE_REGISTRY_ADMIN"};
  // Corrupt input must not erase the live row or append recovery output.
  for(std::size_t n=1;n<golden.size();++n){write(golden.substr(0,n));Check(a::RecoverSblrVariableDescriptorRegistry(admin).error);Check(read()==golden.substr(0,n));}
  for(std::size_t n=0;n<golden.size();++n){auto damaged=golden;damaged[n]^=1;write(damaged);Check(a::RecoverSblrVariableDescriptorRegistry(admin).error);}
  Check(lookup(c,c.statement_uuid,scope,frame,row.variable_descriptor_uuid,1).ok);write(golden);
  a::SblrVariableAssignment assignment;assignment.variable_descriptor_uuid=row.variable_descriptor_uuid;assignment.variable_descriptor_generation=1;assignment.expected_value_generation=1;assignment.value_state=a::SblrVariableValueState::value;assignment.canonical_value_bytes=std::string("a\0b\n",4);
  Check(!a::AssignSblrVariableBatch(c,c.statement_uuid,scope,1,frame,1,{assignment,assignment}).ok);
  auto assigned=a::AssignSblrVariableBatch(c,c.statement_uuid,scope,1,frame,1,{assignment});Check(assigned.ok&&assigned.row.value_generation==2);
  Check(lookup(c,c.statement_uuid,scope,frame,row.variable_descriptor_uuid,2).row.canonical_value_bytes==assignment.canonical_value_bytes);
  const auto saved=read();std::filesystem::rename(path,path+".saved");std::filesystem::create_directory(path);
  auto failed=a::AssignSblrVariable(c,c.statement_uuid,scope,1,frame,1,row.variable_descriptor_uuid,1,2,a::SblrVariableValueState::value,"bad");Check(!failed.ok);
  Check(lookup(c,c.statement_uuid,scope,frame,row.variable_descriptor_uuid,2).row.canonical_value_bytes==assignment.canonical_value_bytes);
  std::filesystem::remove(path);std::filesystem::rename(path+".saved",path);Check(read()==saved);
  auto other=c;other.database_uuid=Id(8);other.database_path=(cleanup.path/"other").string();
  auto other_row=a::PublishSblrVariableFrame(other,other.statement_uuid,scope,1,frame,1,{demand});Check(other_row.ok);
  Check(!a::RecoverSblrVariableDescriptorRegistry(admin).error);
  Check(!lookup(c,c.statement_uuid,scope,frame,row.variable_descriptor_uuid,2).ok);
  Check(lookup(other,other.statement_uuid,scope,frame,other_row.rows[0].variable_descriptor_uuid,1).ok);
  Check(!a::RecoverSblrVariableDescriptorRegistry(admin).error);
  auto legacy=c;legacy.database_uuid=Id(9);legacy.database_path=(cleanup.path/"legacy").string();
  {std::ofstream f(legacy.database_path+".sb.sblr_variable_registry.v1");f<<"SBVDR1\tTEXT UUID journal\n";}
  Check(!a::PublishSblrVariableFrame(legacy,legacy.statement_uuid,scope,1,frame,1,{demand}).ok);
  Check(!std::filesystem::exists(legacy.database_path+".sb.sblr_variable_registry.v2"));
}
