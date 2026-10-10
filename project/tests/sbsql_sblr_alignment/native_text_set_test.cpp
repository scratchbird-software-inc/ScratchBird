// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "datatype_text_set.hpp"
#include "datatype_catalog_manifest.hpp"
#include "resource_seed_pack.hpp"
#include "../support/bounded_memory_resource_probe.hpp"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>

namespace dt=scratchbird::core::datatypes;
namespace res=scratchbird::core::resources;
namespace p=scratchbird::core::platform;
long allocation_budget=-1;unsigned checks=0;std::size_t largest_allocation=0;
void* operator new(std::size_t n){if(allocation_budget==0){allocation_budget=-1;throw std::bad_alloc();}
  largest_allocation=std::max(largest_allocation,n);
  if(allocation_budget>0)--allocation_budget;if(auto* a=std::malloc(n?n:1))return a;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* a)noexcept{std::free(a);}void operator delete[](void* a)noexcept{std::free(a);}
void operator delete(void* a,std::size_t)noexcept{std::free(a);}void operator delete[](void* a,std::size_t)noexcept{std::free(a);}
#include "../support/aligned_allocation_fault_bridge.hpp"
void Check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
p::Uuid Id(unsigned n){return {{1,0x9f,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0,static_cast<std::uint8_t>(n)}};}
dt::NativeTextSetControlV1 Limits(){dt::NativeTextSetControlV1 c{100000,4*1024*1024,65536,64*1024*1024,4*1024*1024};
  c.memory=std::pmr::new_delete_resource();return c;}
dt::NativeTextSetBindingV1 Binding(unsigned profile=2){
  static const auto resources=[] {
    res::ResourceSeedLoadConfig c;c.seed_pack_root=SB_TEXT_SET_SEED_ROOT;
    auto r=res::LoadResourceSeedPack(c);Check(r.ok()&&r.image.unicode_collation,"load qualified Unicode collation");
    return std::move(r.image);
  }();
  dt::NativeTextSetBindingV1 b;b.policy_uuid=dt::kNativeTextSetPolicyV1;b.policy_generation=1;
  auto& s=b.text_seed;s.active=true;s.database_uuid=Id(1);s.charset_uuid=Id(2);s.collation_uuid=Id(3);
  s.resource_epoch=10;s.collation_epoch=11;s.comparison_profile=static_cast<res::CollationProfile>(profile);
  s.collation_case_insensitive=profile==2||profile==3;s.collation_accent_insensitive=profile==2;
  if(profile!=1)s.unicode_collation=resources.unicode_collation;
  const auto catalog=dt::LoadCurrentCoreDatatypeCatalogManifest();Check(catalog.ok(),"catalog");
  const auto row=dt::LookupDatatypeCatalogRow(catalog.manifest,dt::CanonicalTypeId::character);
  Check(row.ok()&&row.manifest.descriptor_rows.size()==1,"character descriptor");
  dt::CatalogExecutionTypeMetadata md;md.descriptor_uuid=row.manifest.descriptor_rows[0].descriptor_uuid;
  md.descriptor_epoch=row.manifest.descriptor_rows[0].descriptor_epoch;
  md.charset_uuid={p::UuidKind::object,s.charset_uuid};md.collation_uuid={p::UuidKind::object,s.collation_uuid};
  auto d=dt::LookupExecutionTypeDescriptorFromCatalog(dt::CanonicalTypeId::character,md);
  Check(d.ok(),"bound descriptor");b.element=d.descriptor;b.element.nullable_allowed=true;b.allow_null_elements=true;
  return b;
}
std::string Encode(const dt::NativeTextSetBindingV1& b,const std::vector<dt::NativeTextSetElementV1>& values){
  auto result=dt::EncodeNativeTextSetV1(b,values,Limits());
  if(!result.ok())std::cerr<<result.diagnostic.diagnostic_code<<'\n';
  Check(result.ok()&&!result.encoded_set.empty()&&result.value.encoded_value==result.encoded_set,"encode native SET");
  return result.encoded_set;
}
bool Truth(const dt::DatatypeSetOperationResult& r){Check(r.ok()&&r.value.type_id==dt::CanonicalTypeId::boolean&&r.value.encoded_value.size()==1,"Boolean native result");return r.value.encoded_value[0]==1;}
void Semantics(){
  auto b=Binding();const std::vector<dt::NativeTextSetElementV1> input={{false,"a"},{false,"A"},{false,"\xc3\xa9"},{false,"e\xcc\x81"},{true,{}},{false,"<NULL>"},{false,{}}};
  const auto frame=Encode(b,input);auto reversed=input;std::reverse(reversed.begin(),reversed.end());
  Check(Encode(b,reversed)==frame,"unordered representative depends on input order");
  auto decoded=dt::DecodeNativeTextSetV1(b,frame,Limits());Check(decoded.ok()&&decoded.elements.size()==5,"collation dedup cardinality");
  Check(std::find(decoded.elements.begin(),decoded.elements.end(),dt::NativeTextSetElementV1{false,"A"})!=decoded.elements.end(),"minimum original ASCII representative");
  Check(std::find(decoded.elements.begin(),decoded.elements.end(),dt::NativeTextSetElementV1{false,"e\xcc\x81"})!=decoded.elements.end(),"canonical-equivalence representative rewritten");
  Check(Encode(b,decoded.elements)==frame,"binary read/encode roundtrip");
  for(const auto& member:input)Check(Truth(dt::ApplyNativeTextSetOperationV1(b,dt::DatatypeSetOperationKind::membership,frame,{},member,Limits())),"collated or NULL membership");
  Check(!Truth(dt::ApplyNativeTextSetOperationV1(b,dt::DatatypeSetOperationKind::membership,frame,{}, {false,"missing"},Limits())),"absent member");
  const auto other=Encode(b,{{false,"a"},{false,"E"},{true,{}},{false,"<null>"},{false,{}}});
  Check(other!=frame&&Truth(dt::ApplyNativeTextSetOperationV1(b,dt::DatatypeSetOperationKind::equals,frame,other,{},Limits())),"representative bytes changed equality");
  const auto subset=Encode(b,{{false,"a"}});
  Check(Truth(dt::ApplyNativeTextSetOperationV1(b,dt::DatatypeSetOperationKind::subset,subset,frame,{},Limits())),"subset");
  Check(Truth(dt::ApplyNativeTextSetOperationV1(b,dt::DatatypeSetOperationKind::superset,frame,subset,{},Limits())),"superset");
  const auto count=dt::ApplyNativeTextSetOperationV1(b,dt::DatatypeSetOperationKind::cardinality,frame,{}, {},Limits());
  Check(count.ok()&&count.value.encoded_value.size()==8&&count.value.encoded_value[0]==5,"native cardinality");
  auto renamed=b;renamed.element.stable_name="renamed";renamed.text_seed.collation_name="untrusted label";
  Check(Encode(renamed,input)==frame,"display rename changed binary frame");
  Check(frame.starts_with("SBTSET01")&&frame.find("019f0000-")==std::string::npos,"text UUID in frame");
  b.duplicates=dt::TextSetDuplicatePolicyV1::reject;
  const auto duplicate=dt::EncodeNativeTextSetV1(b,input,Limits());Check(!duplicate.ok()&&duplicate.diagnostic.diagnostic_code=="CSCR.SET.DUPLICATE_REFUSED","explicit rejection");
  b.duplicates=dt::TextSetDuplicatePolicyV1::preserve;
  Check(dt::DecodeNativeTextSetV1(b,Encode(b,input),Limits()).elements.size()==input.size(),"preserve multiplicity");
  b.ordered=true;b.duplicates=dt::TextSetDuplicatePolicyV1::collapse_first;
  const auto ordered=dt::DecodeNativeTextSetV1(b,Encode(b,input),Limits());
  Check(ordered.ok()&&ordered.elements.front().bytes=="a","ordered first representative changed");
  for(unsigned profile=1;profile<=5;++profile){auto q=Binding(profile);const auto f=Encode(q,input);
    Check(dt::DecodeNativeTextSetV1(q,f,Limits()).ok(),"all declared collation recipes");}
}
void Invalid(){auto b=Binding();const auto f=Encode(b,{{false,"abc"},{true,{}}});
  for(std::size_t n=0;n<f.size();++n)Check(!dt::DecodeNativeTextSetV1(b,std::string_view(f).substr(0,n),Limits()).ok(),"truncated frame accepted");
  auto garbage=f;garbage+='x';Check(!dt::DecodeNativeTextSetV1(b,garbage,Limits()).ok(),"trailing byte accepted");
  for(unsigned i=0;i<12;++i){auto bad=b;switch(i){case 0:++bad.policy_generation;break;case 1:bad.policy_uuid.bytes[0]^=1;break;
    case 2:++bad.text_seed.resource_epoch;break;case 3:++bad.text_seed.collation_epoch;break;case 4:bad.text_seed.database_uuid.bytes[0]^=1;break;
    case 5:bad.text_seed.charset_uuid.bytes[0]^=1;break;case 6:bad.element.collation_uuid.bytes[0]^=1;break;
    case 7:bad.element.domain_uuid.bytes[0]=1;break;case 8:++bad.element.descriptor_epoch;break;case 9:bad.text_seed.unicode_collation.reset();break;
    case 10:bad.ordered=true;break;case 11:bad.element.modifier_flags^=0x80000000;break;}
    auto r=dt::DecodeNativeTextSetV1(bad,f,Limits());Check(!r.ok()&&r.elements.empty(),"binding mutation accepted");}
  for(const auto& v:std::vector<dt::NativeTextSetElementV1>{{false,std::string("\xc0\x80",2)},{true,"<NULL>"}}){auto r=dt::EncodeNativeTextSetV1(b,std::vector{v},Limits());Check(!r.ok()&&r.encoded_set.empty()&&r.value.encoded_value.empty(),"malformed value output");}
  for(unsigned i=0;i<5;++i){auto limits=Limits();if(i==0)limits.maximum_elements=1;if(i==1)limits.maximum_value_bytes=1;if(i==2)limits.maximum_key_bytes=1;if(i==3)limits.maximum_work_bytes=1;if(i==4)limits.maximum_output_bytes=1;
    auto r=dt::DecodeNativeTextSetV1(b,f,limits);Check(!r.ok()&&r.elements.empty(),"resource bound ignored");}
  unsigned calls=0;auto control=Limits();control.cancellation_context=&calls;
  control.cancelled=[](void* p)noexcept{++*static_cast<unsigned*>(p);return true;};
  const auto cancelled=dt::EncodeNativeTextSetV1(b,std::vector<dt::NativeTextSetElementV1>{{false,"a"}},control);
  Check(!cancelled.ok()&&cancelled.encoded_set.empty()&&cancelled.diagnostic.diagnostic_code=="PROCESS.CANCELLED"&&calls==1,"cancelled encode published");
}
void Allocation(){auto b=Binding();const std::vector<dt::NativeTextSetElementV1> values={{false,"long enough to allocate value bytes"},{false,"LONG ENOUGH TO ALLOCATE VALUE BYTES"},{true,{}}};
  bool success=false;unsigned failures=0;
  for(long budget=0;budget<1000&&!success;++budget){allocation_budget=budget;const auto r=dt::EncodeNativeTextSetV1(b,values,Limits());allocation_budget=-1;
    if(r.ok())success=true;else{++failures;Check(r.encoded_set.empty()&&r.value.encoded_value.empty()&&r.status.code==p::StatusCode::memory_allocation_failed,"allocation failure lost resource status or emitted bytes");}}
  Check(success&&failures>1,"allocation sweep incomplete");
  const auto frame=Encode(b,values);
  for(unsigned operation=0;operation<6;++operation){success=false;failures=0;
    for(long budget=0;budget<1000&&!success;++budget){allocation_budget=budget;
      if(operation==0){const auto r=dt::DecodeNativeTextSetV1(b,frame,Limits());allocation_budget=-1;
        if(r.ok())success=true;else{++failures;Check(r.elements.empty()&&r.status.code==p::StatusCode::memory_allocation_failed,"decode allocation failure published");}}
      else{const auto kind=operation==1?dt::DatatypeSetOperationKind::membership:operation==2?dt::DatatypeSetOperationKind::cardinality:
          operation==3?dt::DatatypeSetOperationKind::equals:operation==4?dt::DatatypeSetOperationKind::subset:dt::DatatypeSetOperationKind::superset;
        const auto r=dt::ApplyNativeTextSetOperationV1(b,kind,frame,frame,values[0],Limits());allocation_budget=-1;
        if(r.ok())success=true;else{++failures;Check(r.value.encoded_value.empty()&&r.encoded_set.empty()&&r.status.code==p::StatusCode::memory_allocation_failed,"operator allocation failure published");}}
    }
    Check(success&&failures>1,"decode/operator allocation sweep incomplete");
  }
}
void Boundaries(){
  auto b=Binding();auto control=Limits();control.maximum_key_bytes=1;
  std::string marks;for(unsigned i=0;i<128;++i)marks+="\xcc\x81";
  std::vector<dt::NativeTextSetElementV1> values={{false,marks}};
  auto one=dt::EncodeNativeTextSetV1(b,values,control);
  Check(one.ok(),"normalization incorrectly uses final key limit");
  Check(dt::DecodeNativeTextSetV1(b,one.encoded_set,control).ok(),"exact one-byte primary ignorable key decode");
  control.maximum_key_bytes=0;auto no_key=dt::EncodeNativeTextSetV1(b,values,control);
  Check(!no_key.ok()&&no_key.diagnostic.diagnostic_code=="RESOURCE.BUDGET_EXCEEDED","one-short key limit");
  control=Limits();const auto frame=Encode(b,{{false,"a"},{false,"b"},{true,{}}});
  for(unsigned op=0;op<3;++op){
    std::uint64_t lo=0,hi=control.maximum_work_bytes;
    const auto attempt=[&](std::uint64_t budget){auto limits=control;limits.maximum_work_bytes=budget;
      if(op==0)return dt::EncodeNativeTextSetV1(b,values,limits).ok();
      if(op==1)return dt::DecodeNativeTextSetV1(b,frame,limits).ok();
      return dt::ApplyNativeTextSetOperationV1(b,dt::DatatypeSetOperationKind::equals,frame,frame,{},limits).ok();};
    Check(attempt(hi),"work budget baseline");
    while(lo+1<hi){const auto mid=lo+(hi-lo)/2;if(attempt(mid))hi=mid;else lo=mid;}
    Check(attempt(hi)&&!attempt(hi-1),"exact work-budget boundary");
    auto small=control;small.maximum_work_bytes=hi-1;
    const auto code=op==0?dt::EncodeNativeTextSetV1(b,values,small).diagnostic.diagnostic_code:
        op==1?dt::DecodeNativeTextSetV1(b,frame,small).diagnostic.diagnostic_code:
        dt::ApplyNativeTextSetOperationV1(b,dt::DatatypeSetOperationKind::equals,frame,frame,{},small).diagnostic.diagnostic_code;
    Check(code=="RESOURCE.BUDGET_EXCEEDED","work refusal misclassified as allocator exhaustion");
  }
  auto renamed=b;renamed.element.stable_name.assign(1024*1024,'x');renamed.text_seed.collation_name.assign(1024*1024,'x');
  largest_allocation=0;const auto renamed_result=dt::EncodeNativeTextSetV1(renamed,values,Limits());
  Check(renamed_result.ok()&&largest_allocation<65536,"binding copied user-facing labels");
  renamed.element.domain_stack.resize(100000);largest_allocation=0;
  auto rejected=dt::EncodeNativeTextSetV1(renamed,values,Limits());
  Check(!rejected.ok()&&largest_allocation<65536,"invalid domain stack copied before rejection");
  b=Binding(1);const auto big=Encode(b,{{false,std::string(60000,'x')}});
  auto limits=Limits();limits.maximum_value_bytes=1;largest_allocation=0;
  const auto too_big=dt::DecodeNativeTextSetV1(b,big,limits);
  Check(!too_big.ok()&&too_big.diagnostic.diagnostic_code=="RESOURCE.BUDGET_EXCEEDED"&&largest_allocation<5000,"decode value copied before limit");
  const auto empty=Encode(b,{});auto dirty=big;dirty[empty.size()]=0;largest_allocation=0;
  const auto dirty_null=dt::DecodeNativeTextSetV1(b,dirty,limits);
  Check(!dirty_null.ok()&&dirty_null.diagnostic.diagnostic_code=="DATATYPE.NULL_STATE.INVALID"&&largest_allocation<5000,"dirty NULL copied or budget hid diagnostic");
  for(std::size_t offset=0;offset<empty.size()-8;++offset){auto bad=big;bad[offset]^=1;
    Check(!dt::DecodeNativeTextSetV1(b,bad,Limits()).ok(),"mutated binary binding admitted");}
  Check(std::equal(b.policy_uuid.bytes.begin(),b.policy_uuid.bytes.end(),reinterpret_cast<const unsigned char*>(empty.data()+8)),"policy UUID not raw binary at defined offset");
  scratchbird::test_support::BoundedMemoryProbe memory;
  control=Limits();control.memory=&memory;
  Check(dt::EncodeNativeTextSetV1(b,values,control).ok()&&memory.calls>0&&memory.live==0,
        "SET scratch bypassed selected upstream or escaped operation lifetime");
  control.memory=nullptr;
  Check(!dt::EncodeNativeTextSetV1(b,values,control).ok(),"absent scratch resource accepted");
}
void Cancellation(){auto b=Binding();const std::vector<dt::NativeTextSetElementV1> values={{false,"z"},{false,"a"},{false,"A"},{false,"\xc3\xa9"},{true,{}}};
  const auto frame=Encode(b,values);
  for(unsigned operation=0;operation<7;++operation){
    struct State{unsigned count=0,stop=0;} state;
    auto control=Limits();control.cancellation_context=&state;
    control.cancelled=[](void* p)noexcept{auto& s=*static_cast<State*>(p);return ++s.count==s.stop;};
    const auto run=[&](bool cancel){
      if(operation==1){const auto r=dt::DecodeNativeTextSetV1(b,frame,control);
        Check(cancel? !r.ok()&&r.elements.empty()&&r.diagnostic.diagnostic_code=="PROCESS.CANCELLED":r.ok(),"decode cancellation atomicity");}
      else{const auto r=operation==0?dt::EncodeNativeTextSetV1(b,values,control):dt::ApplyNativeTextSetOperationV1(b,
          operation==2?dt::DatatypeSetOperationKind::membership:operation==3?dt::DatatypeSetOperationKind::cardinality:
          operation==4?dt::DatatypeSetOperationKind::equals:operation==5?dt::DatatypeSetOperationKind::subset:dt::DatatypeSetOperationKind::superset,
          frame,frame,values[0],control);
        Check(cancel? !r.ok()&&r.encoded_set.empty()&&r.value.encoded_value.empty()&&r.diagnostic.diagnostic_code=="PROCESS.CANCELLED":r.ok(),"SET cancellation atomicity");}
    };
    run(false);const auto count=state.count;Check(count>0,"no cancellation checkpoint");
    for(unsigned stop=1;stop<=count;++stop){state={0,stop};run(true);Check(state.count==stop,"cancelled operation continued");}
  }
}
int main()try{Semantics();Invalid();Allocation();Boundaries();Cancellation();std::cout<<checks<<" native text SET checks passed\n";}
catch(const std::exception&e){allocation_budget=-1;std::cerr<<e.what()<<" after "<<checks<<" checks\n";return 1;}
