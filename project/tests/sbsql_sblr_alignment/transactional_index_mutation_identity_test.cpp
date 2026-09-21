// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "dml/transactional_index_provider.hpp"
#include "hash_digest.hpp"
#include <cstdlib>
#include <iostream>
#include <new>
#include <set>

namespace { long fail_after=-1; unsigned checks=0, failures=0, faults=0; }
void* operator new(std::size_t size) {
  if(fail_after==0)throw std::bad_alloc();
  if(fail_after>0)--fail_after;
  if(auto* p=std::malloc(size?size:1))return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}
void operator delete[](void* p)noexcept{std::free(p);}
void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
namespace api=scratchbird::engine::internal_api;
namespace {
void Check(bool ok,const char* why){++checks;if(!ok){++failures;std::cerr<<"FAIL "<<why<<'\n';}}
api::EngineUuid Id(unsigned n){api::EngineUuid id;id.bytes[0]=1;id.bytes[6]=0x70;id.bytes[8]=0x80;id.bytes[15]=n;return id;}
api::EngineUuid& Role(api::EngineRequestContext& c,api::DmlTransactionalIndexEntryRequest& r,unsigned n){
  switch(n){case 0:return c.database_uuid;case 1:return c.transaction_uuid;
    case 2:return r.index.index_uuid;case 3:return r.table_uuid;
    case 4:return r.row_uuid;case 5:return r.version_uuid;default:return r.predecessor_version_uuid;}
}
}
int main(){
  // Fixed-width lowercase formatting must preserve each of the256 byte values
  // at every digest position, including leading zeroes.
  constexpr char alphabet[]="0123456789abcdef";
  for(unsigned pos=0;pos<32;++pos)for(unsigned value=0;value<256;++value){
    scratchbird::core::hash::Digest256 input{};input[pos]=value;
    std::string expected_hex(64,'0');expected_hex[2*pos]=alphabet[value>>4];
    expected_hex[2*pos+1]=alphabet[value&15];
    Check(scratchbird::core::hash::HexLower(input)==expected_hex,"digest formatter lost bytes or padding");
  }
  api::EngineRequestContext context;context.database_uuid=Id(1);context.transaction_uuid=Id(2);context.local_transaction_id=7;
  api::DmlTransactionalIndexEntryRequest request;
  request.index.index_uuid=Id(3);request.index.event_sequence=11;
  request.table_uuid=Id(4);request.index.table_uuid=request.table_uuid;
  request.row_uuid=Id(5);request.version_uuid=Id(6);request.key_value="key";request.payload_value="payload";
  const auto digest=[](const auto& c,const auto& r,const char* kind="insert"){
    return api::DmlTransactionalIndexMutationIdentity(c,r,kind);
  };
  // Independent hashlib oracle over the owning Core contract's exact193 bytes.
  const std::string expected="2ba44dbf89d3b9e98ef4e141fe657f1194119568bdd28469dce2b3792567a17b";
  Check(digest(context,request)==expected,"known binary framing digest mismatch");
  Check(digest(context,request)==digest(context,request),"digest nondeterministic");
  Check(digest(context,request,"retire").empty(),"retire accepted nil predecessor");
  auto parent=request;parent.predecessor_version_uuid=Id(7);
  std::set<std::string> kinds;
  for(auto kind:{"insert","retire","rebuild"})kinds.insert(digest(context,parent,kind));
  Check(kinds.size()==3 && !kinds.contains(""),"operation domains alias");
  for(auto kind:{"","INSERT","exact","delete","insert\tretire"})
    Check(digest(context,request,kind).empty(),"unknown operation admitted");
  for(unsigned role=0;role<7;++role){
    for(unsigned pos=0;pos<16;++pos){
      auto c=context;auto r=parent;Role(c,r,role).bytes[pos]^=1;
      if(role==3)r.index.table_uuid=r.table_uuid;
      const auto changed=digest(c,r);
      Check(changed.size()==64 && changed!=digest(context,parent),"identity byte ignored");
    }
    for(unsigned version=0;version<16;++version)if(version!=7){
      auto c=context;auto r=parent;Role(c,r,role).bytes[6]=version<<4;
      if(role==3)r.index.table_uuid=r.table_uuid;
      Check(digest(c,r).empty(),"non-v7 system identity admitted");
    }
    for(unsigned variant:{0u,0x40u,0xc0u}){
      auto c=context;auto r=parent;Role(c,r,role).bytes[8]=variant;
      if(role==3)r.index.table_uuid=r.table_uuid;
      Check(digest(c,r).empty(),"invalid UUID variant admitted");
    }
    auto c=context;auto r=parent;Role(c,r,role)={};
    if(role==3)r.index.table_uuid=r.table_uuid;
    Check(role==6 ? digest(c,r)==expected : digest(c,r).empty(),"nil role policy changed");
  }
  for(unsigned pos=0;pos<16;++pos){
    auto r=request;r.index.table_uuid.bytes[pos]^=1;
    Check(digest(context,r).empty(),"index/table cohort mismatch admitted");
  }
  auto c=context;c.local_transaction_id=0;Check(digest(c,request).empty(),"zero transaction admitted");
  c.local_transaction_id=8;Check(digest(c,request)!=expected,"local transaction omitted");
  auto r=request;r.index.event_sequence=0;Check(digest(context,r).empty(),"zero generation admitted");
  r.index.event_sequence=12;Check(digest(context,r)!=expected,"generation omitted");
  for(unsigned byte=0;byte<8;++byte){
    c=context;c.local_transaction_id^=std::uint64_t{1}<<(8*byte);
    Check(digest(c,request)!=expected,"local transaction high byte omitted");
    r=request;r.index.event_sequence^=std::uint64_t{1}<<(8*byte);
    Check(digest(context,r)!=expected,"generation high byte omitted");
  }
  for(unsigned byte=0;byte<256;++byte){
    auto a=request,b=request;
    a.key_value=std::string("a")+static_cast<char>(byte)+"b";a.payload_value="c";
    b.key_value="a";b.payload_value=std::string("b")+static_cast<char>(byte)+"c";
    Check(digest(context,a)!=digest(context,b),"key/payload frame collision");
  }
  r=request;r.key_value.clear();r.payload_value.clear();
  Check(digest(context,r).size()==64,"empty key/payload refused");
  r.key_value=std::string(1,'\0');Check(digest(context,r).size()==64,"binary NUL refused");
  // Fault injection concerns the real helper's owned allocations. No caller
  // mutation is simulated, and it is not proof of the whole provider lifecycle.
  bool completed=false;
  for(long point=0;point<64;++point){
    std::string result="unchanged";
    fail_after=point;
    try {result=digest(context,request);fail_after=-1;completed=true;Check(result==expected,"allocation retry changed digest");break;}
    catch(const std::bad_alloc&){fail_after=-1;++faults;Check(result=="unchanged","failed digest published partial result");}
  }
  Check(completed && faults>0,"allocation fault sweep incomplete");
  Check(request.key_value=="key" && request.payload_value=="payload" && request.table_uuid==Id(4),"input mutated");
  std::cout<<"transactional_index_mutation_identity checks="<<checks<<" faults="<<faults<<" failures="<<failures<<'\n';
  return failures?1:0;
}
