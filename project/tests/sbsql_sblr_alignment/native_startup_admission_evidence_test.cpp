// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "engine/internal_api/transaction/native_startup_admission_evidence.hpp"
#include <cstdlib>
#include <iostream>
#include <new>
#include <source_location>
#include <stdexcept>
namespace {bool deny_allocations=false;unsigned allocation_attempts=0;}
void* operator new(std::size_t n){if(deny_allocations){++allocation_attempts;throw std::bad_alloc();}if(auto p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p)noexcept{std::free(p);}void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
namespace {
namespace v=scratchbird::engine::internal_api;namespace c=scratchbird::core::catalog;
using namespace scratchbird::core::platform;
using I=v::NativeAdmissionIdentity;using N=v::NativeAdmissionNumber;using E=v::NativeAdmissionEvidenceError;
using Bytes=std::vector<byte>;
unsigned checks=0;
void Check(bool condition,const char* reason,std::source_location location=std::source_location::current()){
 ++checks;if(!condition)throw std::runtime_error(std::string(reason)+" line="+std::to_string(location.line()));
}
Uuid Id(unsigned n){Uuid id;id.bytes[0]=1;id.bytes[6]=0x70;id.bytes[8]=0x80;id.bytes[14]=n>>8;id.bytes[15]=n;return id;}
template<class Call>auto NoHeap(Call&& call){allocation_attempts=0;deny_allocations=true;auto result=call();deny_allocations=false;
 Check(!allocation_attempts,"bounded evidence codec makes no C++ allocation");return result;}
void Empty(const v::NativeStartupAdmissionEvidenceImage& r){Check(!r.ok()&&!r.record&&r.bytes.empty(),"no admitted evidence prefix on failure");}
struct Fixture {
 std::array<Uuid,1> subjects{Id(9)};std::array<Uuid,2> targets{Id(4),Id(24)};
 std::array<Uuid,5> versions{Id(17),Id(19),Id(21),Id(23),Id(101)};
 std::array<v::NativeStartupPolicyBinding,1> policies;
 v::NativeStartupAdmissionEvidenceInput input;
 Fixture(){for(unsigned n=0;n<24;++n)input.identities[n]=Id(n+1);input.numbers.fill(1);
  input.Number(N::transaction_number)=2;input.Number(N::authorization_time_utc_us)=100;input.Number(N::policy_time_utc_us)=101;
  input.Number(N::effective_policy_generation)=7;input.source_checkpoint_sha256.fill(0x43);
  input.outcome=v::NativeStartupPolicyOutcome::allow;input.reason=v::NativeAdmissionReason::startup_policy_allow;
  policies[0]={Id(100),Id(101),Id(102),7,2,0xa5,c::CatalogPolicyDefault::allow,1,c::CatalogPolicyScope::database,true};
  input.subjects=subjects;input.targets=targets;input.metadata_versions=versions;input.policies=policies;
 }
};
void Num(Bytes& b,std::size_t at,unsigned width,u64 value){for(unsigned n=0;n<width;++n)b[at+n]=byte(value>>(8*n));}
void Identity(Bytes& b,std::size_t at,Uuid id){std::copy(id.bytes.begin(),id.bytes.end(),b.begin()+at);}
void Seal(Bytes& b){std::fill_n(b.begin()+544,32,0);const auto d=scratchbird::core::hash::ComputeSha256DigestNative(b.data(),b.size());
 Check(d.ok(),"independent one-shot SHA256 succeeds");std::copy(d.digest.begin(),d.digest.end(),b.begin()+544);}
Bytes Oracle(){Bytes b(848);std::copy_n("SBSA0001",8,b.begin());Num(b,8,2,1);Num(b,10,2,640);Num(b,12,4,848);
 for(unsigned n=0;n<24;++n)Identity(b,16+16*n,Id(n+1));
 const std::array<u64,12> numbers{2,100,101,1,1,7,1,1,1,1,1,1};
 for(unsigned n=0;n<12;++n)Num(b,416+8*n,8,numbers[n]);Num(b,512,2,1);Num(b,514,2,0xa5);Num(b,516,4,7);
 Num(b,520,4,1);Num(b,524,4,2);Num(b,528,4,5);Num(b,532,4,1);Num(b,536,2,1);std::fill_n(b.begin()+576,32,0x43);
 const std::array<unsigned,8> tail{9,4,24,17,19,21,23,101};for(unsigned n=0;n<8;++n)Identity(b,640+16*n,Id(tail[n]));
 Identity(b,768,Id(100));Identity(b,784,Id(101));Identity(b,800,Id(102));Num(b,816,8,7);Num(b,824,8,2);Num(b,832,8,0xa5);
 Num(b,840,2,1);Num(b,842,2,1);Num(b,844,2,1);b[846]=1;Seal(b);return b;
}
void Codec(){Fixture f;const auto expected=Oracle();Bytes output(expected.size()+5,0xae);
 const auto encode=[&]{return NoHeap([&]{return v::EncodeNativeStartupAdmissionEvidenceInto(f.input,4096,std::span(output).subspan(1));});};
 auto encoded=encode();Check(encoded.ok()&&std::ranges::equal(encoded.bytes,expected)&&output.front()==0xae&&output.back()==0xae,
     "complete independent byte oracle, unaligned output and spare guard bytes");
 auto decoded=NoHeap([&]{return v::DecodeNativeStartupAdmissionEvidence(expected,expected.size());});
 Check(decoded.ok()&&decoded.record->Identity(I::actor)==Id(9)&&decoded.record->Number(N::transaction_number)==2&&
     decoded.record->Policy(0).descriptor_version_uuid==Id(101)&&decoded.record->Item(2,4)==Id(101)&&
     decoded.record->cache_mask()==0xa5&&decoded.bytes.data()==expected.data(),"complete borrowed evidence and policy source binding");
 for(std::size_t n=0;n<expected.size();++n){
  Empty(NoHeap([&]{return v::DecodeNativeStartupAdmissionEvidence(std::span(expected).first(n),4096);}));
  std::fill(output.begin(),output.end(),0xae);
  Empty(NoHeap([&]{return v::EncodeNativeStartupAdmissionEvidenceInto(f.input,4096,std::span(output).first(n));}));
  Check(std::ranges::all_of(output,[](byte b){return b==0xae;}),"short output has no write");
  auto bad=expected;bad[n]^=1;Empty(NoHeap([&]{return v::DecodeNativeStartupAdmissionEvidence(bad,4096);}));
 }
 for(unsigned offset:{538u,539u,540u,541u,542u,543u,608u,639u,847u}){auto bad=expected;bad[offset]=1;Seal(bad);
  Empty(NoHeap([&]{return v::DecodeNativeStartupAdmissionEvidence(bad,4096);}));}
 auto bad=expected;bad.push_back(0);Seal(bad);Empty(v::DecodeNativeStartupAdmissionEvidence(bad,4096));
 Empty(v::DecodeNativeStartupAdmissionEvidence(expected,expected.size()-1));
 Empty(v::EncodeNativeStartupAdmissionEvidenceInto(f.input,expected.size()-1,output));
 // Resealed semantic substitutions cannot turn a refusal into an admission.
 for(unsigned fault=0;fault<13;++fault){bad=expected;
  switch(fault){case 0:Identity(bad,16,Id(2));break;case 1:Num(bad,416,8,1);break;
   case 2:Num(bad,432,8,99);break;case 3:Num(bad,456,8,8);break;
   case 4:Num(bad,512,2,2);break;case 5:Num(bad,514,2,1);break;case 6:Num(bad,516,4,3);break;
   case 7:Num(bad,520,4,129);break;case 8:Num(bad,536,2,2);break;case 9:Identity(bad,640,Id(10));break;
   case 10:Identity(bad,688,Id(18));break;case 11:bad[846]=0;break;case 12:bad[846]=2;break;}
  Seal(bad);Empty(NoHeap([&]{return v::DecodeNativeStartupAdmissionEvidence(bad,4096);}));
 }
 const auto alias=std::span<byte>{reinterpret_cast<byte*>(f.subjects.data()),sizeof(f.subjects)};
 Check(v::EncodeNativeStartupAdmissionEvidenceInto(f.input,4096,alias).error==E::invalid_workspace,"output cannot overwrite subject backing");
 for(const auto decision:{c::CatalogPolicyDefault::deny,c::CatalogPolicyDefault::defer,c::CatalogPolicyDefault::require_higher_authority,c::CatalogPolicyDefault::require_human_review}){
  f.policies[0].decision=decision;f.input.Identity(I::decisive_policy)=Id(100);f.input.reason=v::NativeAdmissionReason::applicable_policy_refusal;
  f.input.outcome=decision==c::CatalogPolicyDefault::defer?v::NativeStartupPolicyOutcome::defer:
      decision==c::CatalogPolicyDefault::require_human_review?v::NativeStartupPolicyOutcome::require_human_review:v::NativeStartupPolicyOutcome::deny;
  Check(encode().ok(),"actual policy refusal remains expressible without permission");
 }
 f.policies[0].decision=c::CatalogPolicyDefault::allow;f.policies[0].body_resolved=false;
 f.input.reason=v::NativeAdmissionReason::unresolved_applicable_body;f.input.outcome=v::NativeStartupPolicyOutcome::deny;
 Check(encode().ok(),"unresolved body evidence retains denial");
 f.input.outcome=v::NativeStartupPolicyOutcome::allow;Empty(encode());
 f.policies[0].body_resolved=true;f.input.Identity(I::decisive_policy)={};
 f.input.outcome=v::NativeStartupPolicyOutcome::require_revalidation;f.input.reason=v::NativeAdmissionReason::policy_generation_changed;
 Check(encode().ok(),"stale-generation decision remains explicit");
 f.input.outcome=v::NativeStartupPolicyOutcome(65536);Empty(encode());
}
}
// W4d (2026-11-08): StateCarrier (the evidence carried inside the verification
// operation state record) was deleted with that record (the checkpoint-era
// verification history). The evidence codec itself is unchanged.
int main(){try{Codec();std::cout<<"native startup admission evidence PASS checks="<<checks<<" representation_only=true\n";return 0;}
 catch(const std::exception& e){deny_allocations=false;std::cerr<<"native startup admission evidence FAIL "<<e.what()<<" checks="<<checks<<'\n';return 1;}}
