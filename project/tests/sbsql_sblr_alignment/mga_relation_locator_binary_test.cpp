// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/engine/internal_api/mga_relation_store/mga_relation_locator.hpp"
#include "../../src/engine/internal_api/mga_relation_store/mga_update_durable_frame_store.cpp"
#include <cstdlib>
#include <iostream>
#include <source_location>
#include <thread>
#include <unistd.h>
namespace a=scratchbird::engine::internal_api;
void Check(bool ok,std::source_location at=std::source_location::current()) {
  if(!ok){std::cerr<<"failure at "<<at.line()<<'\n';std::abort();}
}
a::EngineUuid Id(unsigned n){return a::EngineUuid{{1,144,0,0,0,0,112,0,128,0,0,0,0,0,0,static_cast<std::uint8_t>(n)}};}
int main(){
  const auto base=std::filesystem::temp_directory_path()/("sb-locator-binary-"+std::to_string(getpid()));
  a::EngineRequestContext context;context.database_path=base.string();
  const auto root=base.string()+".sb.mga_relation_scope";
  Check(a::MgaScopedRelationPath(context,Id(1),".rows").empty());
  Check(!std::filesystem::exists(root));
  const auto first=a::MgaScopedRelationPath(context,Id(1),".rows",true);
  Check(first==root+"/relation-1.rows");
  Check(a::MgaScopedRelationPath(context,Id(2),".rows",true)==root+"/relation-2.rows");
  Check(a::MgaScopedRelationPath(context,Id(1),".rows")==first);
  Check(a::MgaScopedRelationPath(context,Id(3),".rows").empty());
  std::vector<std::thread> threads;
  for(unsigned n=0;n<8;++n)threads.emplace_back([&]{Check(a::MgaScopedRelationPath(context,Id(3),".indexes",true)==root+"/relation-3.indexes");});
  for(auto& t:threads)t.join();
  auto locators=a::LoadMgaRelationLocators(root);
  Check(locators.size()==3&&locators.at(Id(1))==1&&locators.at(Id(2))==2&&locators.at(Id(3))==3);
  std::ifstream in(root+"/locators.v2",std::ios::binary);
  std::string bytes((std::istreambuf_iterator<char>(in)),{});
  std::vector<std::string> fields;Check(a::DecodeMgaMetadataFields(bytes,&fields));
  Check(fields.size()==7&&fields[1]==a::MetadataUuidBytes(Id(1))&&fields[2].size()==8);
  auto bad=bytes;bad[20]^=1;
  {std::ofstream out(root+"/locators.v2",std::ios::binary|std::ios::trunc);out.write(bad.data(),bad.size());}
  bool refused=false;try{a::MgaScopedRelationPath(context,Id(1),".rows");}catch(const std::runtime_error&){refused=true;}
  Check(refused);
  std::filesystem::remove_all(root);
  std::filesystem::create_directory(root);
  {std::ofstream legacy(root+"/legacy-uuid.rows");legacy<<"old";}
  refused=false;try{a::MgaScopedRelationPath(context,Id(1),".rows",true);}catch(const std::runtime_error&){refused=true;}
  Check(refused);
  std::filesystem::remove_all(root);
}
