// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_management_operation.hpp"
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <new>
#include <source_location>
#include <stdexcept>
namespace {long allocation_budget=-1;bool counting=false;unsigned long allocations=0;unsigned hash_fault=0,hash_target=1,hash_seen=0;bool hash_active=false;}
void* operator new(std::size_t n){if(counting)++allocations;if(allocation_budget==0){allocation_budget=-1;throw std::bad_alloc();}if(allocation_budget>0)--allocation_budget;if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept{std::free(p);}void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
extern "C" EVP_MD_CTX* __real_EVP_MD_CTX_new();
extern "C" EVP_MD_CTX* __wrap_EVP_MD_CTX_new(){hash_active=hash_fault&&++hash_seen==hash_target;if(hash_active&&hash_fault==1){hash_fault=0;return nullptr;}return __real_EVP_MD_CTX_new();}
extern "C" int __real_EVP_DigestInit_ex(EVP_MD_CTX*,const EVP_MD*,ENGINE*);
extern "C" int __wrap_EVP_DigestInit_ex(EVP_MD_CTX* c,const EVP_MD* m,ENGINE* e){if(hash_active&&hash_fault==2){hash_fault=0;return 0;}return __real_EVP_DigestInit_ex(c,m,e);}
extern "C" int __real_EVP_DigestUpdate(EVP_MD_CTX*,const void*,size_t);
extern "C" int __wrap_EVP_DigestUpdate(EVP_MD_CTX* c,const void* b,size_t n){if(hash_active&&hash_fault==3){hash_fault=0;return 0;}return __real_EVP_DigestUpdate(c,b,n);}
extern "C" int __real_EVP_DigestFinal_ex(EVP_MD_CTX*,unsigned char*,unsigned int*);
extern "C" int __wrap_EVP_DigestFinal_ex(EVP_MD_CTX* c,unsigned char* b,unsigned int* n){if(hash_active&&hash_fault==4){hash_fault=0;return 0;}const int r=__real_EVP_DigestFinal_ex(c,b,n);if(hash_active&&hash_fault==5){hash_fault=0;*n=31;}return r;}
extern "C" int __real_EVP_Digest(const void*,size_t,unsigned char*,unsigned int*,const EVP_MD*,ENGINE*);
extern "C" int __wrap_EVP_Digest(const void* p,size_t n,unsigned char* out,unsigned int* size,const EVP_MD* md,ENGINE* e){const bool hit=hash_fault&&++hash_seen==hash_target;const auto mode=hit?hash_fault:0;if(hit)hash_fault=0;if(mode&&mode!=5)return 0;const auto r=__real_EVP_Digest(p,n,out,size,md,e);if(mode==5)*size=31;return r;}
namespace {
using namespace scratchbird::core::platform;
namespace db=scratchbird::storage::database;
using O=db::NativeManagementOperation;using S=db::NativeManagementStep;
using OS=db::NativeManagementState;using SS=db::NativeManagementStepState;
using E=db::NativeManagementOperationError;using Bytes=std::vector<byte>;
unsigned checks=0;constexpr u64 budget=1<<20;
void Check(bool v,const char* text,std::source_location at=std::source_location::current()){++checks;if(!v)throw std::runtime_error(std::string(text)+" line="+std::to_string(at.line()));}
Uuid Id(unsigned n){Uuid u;u.bytes[0]=1;u.bytes[6]=0x70;u.bytes[8]=0x80;u.bytes[14]=n>>8;u.bytes[15]=n;return u;}
bool Terminal(unsigned s){return s>=13;}
O Example(unsigned state=4){O o;o.database_uuid=Id(1);o.bootstrap_uuid=Id(2);o.uuid=Id(3);o.descriptor_uuid=Id(4);o.family_uuid=Id(5);o.target_type_uuid=Id(6);o.target_uuid=Id(7);
 o.initiator_uuid=Id(8);o.request_context_uuid=Id(9);o.policy_snapshot_uuid=Id(10);o.security_snapshot_uuid=Id(11);
 o.created_at=Id(1000);o.updated_at=Id(2000);o.normalized_request_sha256.fill(19);o.generation_guards={0,1,2,std::nullopt};
 o.idempotency_key=std::string("operation\0key",13);o.state=OS(state);o.initiator_kind=4;
 if(state>=3){o.phase_uuid=Id(12);o.resource_plan_uuid=Id(13);o.lock_plan_uuid=Id(14);}
 if(Terminal(state)){o.terminal_at=Id(1900);o.result_uuid=Id(15);o.boundary_uuid=Id(16);o.diagnostic_uuid=Id(17);}
 return o;
}
S Step(unsigned state=1,unsigned ordinal=1){S s;s.uuid=Id(3000+ordinal);s.operation_uuid=Id(3);s.family_uuid=Id(21);s.target_uuid=Id(7);s.ordinal=ordinal;s.state=SS(state);
 s.idempotency_key=std::string("step\0key",8);s.precondition_sha256.fill(22);s.mutation=db::NativeManagementMutation::page;s.compensation=db::NativeManagementCompensation::retry;
 if(state==3||state==4||state==5||state==6||state==7||state==9||state==10)s.started_at=Id(1200);
 if(state>=7){s.completed_at=Id(1300);s.diagnostic_uuid=Id(25);s.evidence_uuid=Id(26);s.metric_evidence_uuid=Id(27);}
 if(state==7||state==9){s.postcondition_sha256.fill(23);s.boundary_uuid=Id(24);}return s;
}
void Num(Bytes& b,std::size_t at,unsigned n,u64 v){for(unsigned i=0;i<n;++i)b[at+i]=static_cast<byte>(v>>(8*i));}
void Put(Bytes& b,std::size_t at,const Uuid& u){std::copy(u.bytes.begin(),u.bytes.end(),b.begin()+at);}
auto Sha(const Bytes& b){std::array<byte,32> h{};Check(SHA256(b.data(),b.size(),h.data()),"independent SHA256");return h;}
void Seal(Bytes& b){std::fill(b.begin()+480,b.begin()+512,0);const auto h=Sha(b);std::copy(h.begin(),h.end(),b.begin()+480);}
Bytes Oracle(const O& o){Bytes b(512);std::copy_n("SBMGO001",8,b.begin());Num(b,8,2,1);Num(b,10,2,512);
 Put(b,16,o.database_uuid);Put(b,32,o.bootstrap_uuid);Put(b,48,o.uuid);Put(b,64,o.descriptor_uuid);Put(b,80,o.family_uuid);Put(b,96,o.target_type_uuid);Put(b,112,o.target_uuid);Put(b,128,o.initiator_uuid);Put(b,144,o.request_context_uuid);Put(b,160,o.policy_snapshot_uuid);Put(b,176,o.security_snapshot_uuid);Put(b,192,o.phase_uuid);Put(b,208,o.boundary_uuid);Put(b,224,o.created_at);Put(b,240,o.updated_at);Put(b,256,o.terminal_at);
 std::copy(o.normalized_request_sha256.begin(),o.normalized_request_sha256.end(),b.begin()+272);Put(b,304,o.resource_plan_uuid);Put(b,320,o.lock_plan_uuid);Put(b,336,o.result_uuid);Put(b,352,o.diagnostic_uuid);Put(b,368,o.evidence_uuid);Put(b,384,o.metric_evidence_uuid);Put(b,400,o.cluster_uuid);
 u32 flags=(o.evidence_required?16u:0u)|(o.metrics_required?32u:0u);for(unsigned i=0;i<4;++i)if(o.generation_guards[i]){flags|=1u<<i;Num(b,416+8*i,8,*o.generation_guards[i]);}
 Num(b,448,8,o.revision);Num(b,456,4,o.steps.size());Num(b,460,4,o.idempotency_key.size());Num(b,464,2,u16(o.state));Num(b,466,2,u16(o.scope));Num(b,468,2,o.initiator_kind);Num(b,470,2,u16(o.restart));Num(b,472,4,flags);
 b.insert(b.end(),o.idempotency_key.begin(),o.idempotency_key.end());
 for(const auto& s:o.steps){const auto at=b.size();b.resize(at+256);Put(b,at,s.uuid);Put(b,at+16,s.operation_uuid);Put(b,at+32,s.family_uuid);Put(b,at+48,s.target_uuid);std::copy(s.precondition_sha256.begin(),s.precondition_sha256.end(),b.begin()+at+64);std::copy(s.postcondition_sha256.begin(),s.postcondition_sha256.end(),b.begin()+at+96);Put(b,at+128,s.started_at);Put(b,at+144,s.completed_at);Put(b,at+160,s.evidence_uuid);Put(b,at+176,s.metric_evidence_uuid);Put(b,at+192,s.diagnostic_uuid);Put(b,at+208,s.boundary_uuid);
  Num(b,at+224,4,s.ordinal);Num(b,at+228,4,s.idempotency_key.size());Num(b,at+232,2,u16(s.state));Num(b,at+234,2,u16(s.mutation));Num(b,at+236,2,u16(s.compensation));Num(b,at+238,2,u16(s.recovery));Num(b,at+240,4,(s.evidence_required?1u:0u)|(s.metrics_required?2u:0u)|(s.idempotent?4u:0u));b.insert(b.end(),s.idempotency_key.begin(),s.idempotency_key.end());}
 Num(b,12,4,b.size());Seal(b);return b;
}
void Empty(const db::NativeManagementOperationImage& r){Check(!r.ok()&&!r.record&&r.bytes.empty()&&std::all_of(r.sha256.begin(),r.sha256.end(),[](byte b){return !b;}),"failure exposes no prefix");}
void Good(const O& o){const auto expected=Oracle(o);const auto r=db::EncodeNativeManagementOperation(o,budget);Check(r.ok()&&r.bytes==expected&&r.sha256==Sha(expected),"complete independent native bytes and reference digest");const auto d=db::DecodeNativeManagementOperation(expected,budget);Check(d.ok()&&*d.record==o&&d.sha256==r.sha256,"all operation and step fields preserved");}
void Bad(const O& o){Check(db::ValidateNativeManagementOperation(o)!=E::none,"invalid shape");Empty(db::EncodeNativeManagementOperation(o,budget));Empty(db::DecodeNativeManagementOperation(Oracle(o),budget));}
void Transitions(){
 const std::array<std::vector<unsigned>,15> edges{{{2,15},{3,15},{4,6,13,15},{5,7,8,9,10,11,15},{6,4,10},{4,7,15},{13,10,15,12},{4,5,7,10,12},{14,10,12},{4,7,15},{4,14,10,15,12},{11,15,13},{},{},{}}};
 for(unsigned from=1;from<=15;++from)for(unsigned to=1;to<=15;++to){const auto a=Example(from);auto b=Example(to);b.revision=2;b.updated_at=Id(2002);if(Terminal(to))b.terminal_at=Id(2001);
  Check(db::ValidateNativeManagementOperation(a)==E::none&&db::ValidateNativeManagementOperation(b)==E::none,"transition operands independently valid");
  const bool allowed=(!Terminal(from)&&from==to)||std::find(edges[from-1].begin(),edges[from-1].end(),to)!=edges[from-1].end();
  Check((db::ValidateNativeManagementOperationEvolution(a,b)==E::none)==allowed,"complete envelope transition matrix");
  Check(db::ValidateNativeManagementOperationEvolution(a,a)==E::none,"exact replay even terminal");}
 const std::array<std::vector<unsigned>,10> step_edges{{{2,8,10},{3,4,8,6},{7,4,5,6,10},{2,3,8,6,10},{9,6,10},{2,3,5,10},{},{},{},{}}};
 for(unsigned from=1;from<=10;++from)for(unsigned to=1;to<=10;++to){auto a=Example();a.steps={Step(from)};auto b=a;b.revision=2;b.updated_at=Id(2002);b.steps[0]=Step(to);auto& y=b.steps[0];
  if(from==to)y=a.steps[0];else{if(!a.steps[0].started_at.is_nil())y.started_at=a.steps[0].started_at;else y.started_at=to==3?Id(2001):Uuid{};if(to>=7)y.completed_at=Id(2001);}
  const bool allowed=from==to||std::find(step_edges[from-1].begin(),step_edges[from-1].end(),to)!=step_edges[from-1].end();
  Check((db::ValidateNativeManagementOperationEvolution(a,b)==E::none)==allowed,"complete step transition matrix");}
 auto a=Example(),b=a;b.revision=2;b.updated_at=Id(2002);b.steps={Step()};Check(db::ValidateNativeManagementOperationEvolution(a,b)==E::none,"append pending step before execution");
 a=b;b.revision++;b.updated_at=Id(2003);b.steps.clear();Check(db::ValidateNativeManagementOperationEvolution(a,b)==E::invalid_transition,"cannot drop retained step");
 b=a;b.revision++;b.updated_at=Id(2003);b.steps[0]=Step(3);Check(db::ValidateNativeManagementOperationEvolution(a,b)==E::invalid_transition,"cannot jump pending to running");
}
void RetainedHistory(){
 auto a=Example(),b=a;b.revision=2;b.updated_at=Id(2002);
 for(auto member:{&O::database_uuid,&O::bootstrap_uuid,&O::uuid,&O::descriptor_uuid,&O::family_uuid,&O::target_type_uuid,&O::target_uuid,&O::initiator_uuid,&O::request_context_uuid,&O::policy_snapshot_uuid,&O::created_at}){
  auto changed=b;changed.*member=Id(999);Check(db::ValidateNativeManagementOperation(changed)==E::none,"rewritten envelope identity structurally valid");Check(db::ValidateNativeManagementOperationEvolution(a,changed)==E::immutable_field,"every immutable envelope identity retained");}
 for(unsigned i=0;i<3;++i){auto changed=b;changed.generation_guards[i]=44;Check(db::ValidateNativeManagementOperationEvolution(a,changed)==E::immutable_field,"each local guard retained including present zero");}
 for(unsigned field=0;field<5;++field){auto changed=b;switch(field){case 0:changed.scope=db::NativeManagementScope::local_database;break;case 1:changed.initiator_kind=1;break;case 2:changed.restart=db::NativeManagementRestart::compensate;break;case 3:changed.metrics_required=true;break;case 4:changed.evidence_required=true;break;}Check(db::ValidateNativeManagementOperationEvolution(a,changed)==E::immutable_field,"envelope policy immutable");}
 a.scope=db::NativeManagementScope::cluster;a.cluster_uuid=Id(77);a.generation_guards[3]=0;b=a;b.revision=2;b.updated_at=Id(2002);
 auto changed=b;changed.cluster_uuid=Id(78);Check(db::ValidateNativeManagementOperationEvolution(a,changed)==E::immutable_field,"cluster identity retained");changed=b;changed.generation_guards[3]=1;Check(db::ValidateNativeManagementOperationEvolution(a,changed)==E::immutable_field,"cluster epoch retained including zero");
 a=Example(1);a.security_snapshot_uuid={};a.generation_guards={};b=a;b.revision=2;b.updated_at=Id(2002);b.state=OS::authorized;b.security_snapshot_uuid=Id(11);b.generation_guards={0,1,2,std::nullopt};Check(db::ValidateNativeManagementOperationEvolution(a,b)==E::none,"first authorization acquires snapshot and guards");
 a=Example();a.steps={Step(3)};b=a;b.revision=2;b.updated_at=Id(2002);
 for(unsigned field=0;field<13;++field){changed=b;auto& s=changed.steps[0];switch(field){case 0:s.uuid=Id(44);break;case 1:s.family_uuid=Id(44);break;case 2:s.target_uuid=Id(44);break;case 3:s.idempotency_key="changed";break;case 4:s.mutation=db::NativeManagementMutation::catalog;break;case 5:s.compensation=db::NativeManagementCompensation::compensate;break;case 6:s.recovery=db::NativeManagementRecovery::retry;break;case 7:s.evidence_required=true;break;case 8:s.metrics_required=true;break;case 9:s.idempotent=true;break;case 10:s.precondition_sha256[0]^=1;break;case 11:s.started_at=Id(1201);break;case 12:s.precondition_sha256={};break;}
  Check(db::ValidateNativeManagementOperationEvolution(a,changed)!=E::none,"retained step identity policy and preconditions immutable");}
 a.steps={Step(7)};b=a;b.revision=2;b.updated_at=Id(2002);
 for(unsigned field=0;field<6;++field){changed=b;auto& s=changed.steps[0];switch(field){case 0:s.postcondition_sha256[0]^=1;break;case 1:s.completed_at=Id(1301);break;case 2:s.evidence_uuid=Id(44);break;case 3:s.metric_evidence_uuid=Id(44);break;case 4:s.diagnostic_uuid=Id(44);break;case 5:s.boundary_uuid=Id(44);break;}Check(db::ValidateNativeManagementOperation(changed)==E::none&&db::ValidateNativeManagementOperationEvolution(a,changed)==E::invalid_transition,"terminal step payload cannot be rewritten");}
 a=Example();a.steps={Step(2)};b=a;b.revision=2;b.updated_at=Id(2002);b.steps[0].state=SS::running;b.steps[0].started_at=Id(1999);Check(db::ValidateNativeManagementOperation(b)==E::none&&db::ValidateNativeManagementOperationEvolution(a,b)==E::invalid_transition,"first start cannot predate retained update");b.steps[0].started_at=Id(2000);Check(db::ValidateNativeManagementOperationEvolution(a,b)==E::none,"first start may equal retained update");
 a.steps={Step(3)};b=a;b.revision=2;b.updated_at=Id(2002);b.steps[0]=Step(7);b.steps[0].completed_at=Id(1999);Check(db::ValidateNativeManagementOperation(b)==E::none&&db::ValidateNativeManagementOperationEvolution(a,b)==E::invalid_transition,"step completion cannot predate retained update");
 a=Example(9);b=Example(14);b.revision=2;b.updated_at=Id(2002);Check(db::ValidateNativeManagementOperation(b)==E::none&&db::ValidateNativeManagementOperationEvolution(a,b)==E::invalid_transition,"terminal envelope cannot backdate event");b.terminal_at=Id(2000);Check(db::ValidateNativeManagementOperationEvolution(a,b)==E::none,"terminal envelope accepts current event");
 a=Example();a.revision=~u64{0};b=a;b.revision=1;b.updated_at=Id(2002);Check(db::ValidateNativeManagementOperationEvolution(a,b)==E::invalid_transition,"revision cannot wrap");
}
void MalformedWire(const O& o,const Bytes& raw){
 const auto step=512+o.idempotency_key.size();
 for(std::size_t n=0;n<raw.size();++n){Bytes truncated(raw.begin(),raw.begin()+n);if(n>=512){Num(truncated,12,4,n);Seal(truncated);}Empty(db::DecodeNativeManagementOperation(truncated,budget));}
 auto b=raw;b.push_back(0);Num(b,12,4,b.size());Seal(b);Empty(db::DecodeNativeManagementOperation(b,budget));
 for(const auto [at,width]:std::vector<std::pair<std::size_t,unsigned>>{{456,4},{460,4},{464,2},{466,2},{468,2},{470,2},{472,4},{step+224,4},{step+228,4},{step+232,2},{step+234,2},{step+236,2},{step+238,2},{step+240,4}}){b=raw;Num(b,at,width,~u64{0});Seal(b);Empty(db::DecodeNativeManagementOperation(b,budget));}
 // A nonzero stored guard with its presence bit absent is not canonical.
 b=raw;Num(b,472,4,0);Seal(b);Empty(db::DecodeNativeManagementOperation(b,budget));
 for(std::size_t at=16;at<=256;at+=16){b=raw;b[at+6]=0x40;Seal(b);Empty(db::DecodeNativeManagementOperation(b,budget));}
 for(std::size_t at=304;at<=400;at+=16){b=raw;b[at+6]=0x40;Seal(b);Empty(db::DecodeNativeManagementOperation(b,budget));}
 for(std::size_t at:{step,step+16,step+32,step+48,step+128,step+144,step+160,step+176,step+192,step+208}){b=raw;b[at+6]=0x40;Seal(b);Empty(db::DecodeNativeManagementOperation(b,budget));}
}
void Test(){
 for(unsigned state=1;state<=15;++state)Good(Example(state));
 auto early=Example(15);early.phase_uuid=early.resource_plan_uuid=early.lock_plan_uuid=early.security_snapshot_uuid={};early.generation_guards={};Good(early);
 for(auto member:{&O::phase_uuid,&O::resource_plan_uuid,&O::lock_plan_uuid}){auto invalid=early;invalid.*member=Id(44);Bad(invalid);for(unsigned state:{1u,2u}){invalid=Example(state);invalid.*member=Id(44);Bad(invalid);}}
 for(unsigned state=1;state<=10;++state){auto o=Example();o.steps={Step(state)};Good(o);}
 auto o=Example(14);o.evidence_required=o.metrics_required=true;o.evidence_uuid=Id(30);o.metric_evidence_uuid=Id(31);o.steps={Step(7),Step(9,2)};
 for(auto& s:o.steps)s.evidence_required=s.metrics_required=true;Good(o);
 auto cluster=o;cluster.scope=db::NativeManagementScope::cluster;cluster.cluster_uuid=Id(32);cluster.generation_guards[3]=0;cluster.steps[0].mutation=db::NativeManagementMutation::cluster;Good(cluster);
 const auto raw=Oracle(o);const auto start=512+o.idempotency_key.size();
 for(std::size_t at=0;at<raw.size();++at){auto bad=raw;bad[at]^=1;Empty(db::DecodeNativeManagementOperation(bad,budget));}
 for(std::size_t at:{std::size_t{0},std::size_t{8},std::size_t{10},std::size_t{12},std::size_t{476},std::size_t{479},start+244,start+255}){auto bad=raw;bad[at]^=1;Seal(bad);Empty(db::DecodeNativeManagementOperation(bad,budget));}
 for(unsigned field=0;field<29;++field){auto b=o;
  switch(field){case 0:b.uuid={};break;case 1:b.database_uuid.bytes[6]=0x40;break;case 2:b.bootstrap_uuid=b.database_uuid;break;case 3:b.target_uuid.bytes[8]=0;break;case 4:b.normalized_request_sha256={};break;case 5:b.revision=0;break;case 6:b.state=OS(16);break;case 7:b.scope=db::NativeManagementScope(0);break;case 8:b.initiator_kind=9;break;case 9:b.restart=db::NativeManagementRestart(0);break;case 10:b.steps[0].uuid=b.uuid;break;case 11:b.steps[1].uuid=b.steps[0].uuid;break;case 12:b.steps[1].ordinal=1;break;case 13:b.steps[0].operation_uuid=Id(42);break;case 14:b.steps[0].precondition_sha256={};break;case 15:b.steps[0].postcondition_sha256={};break;case 16:b.steps[0].started_at={};break;case 17:b.steps[0].completed_at={};break;case 18:b.steps[0].boundary_uuid={};break;case 19:b.steps[0].evidence_uuid={};break;case 20:b.steps[0].metric_evidence_uuid={};break;case 21:b.steps[0].compensation=db::NativeManagementCompensation::none;break;case 22:b.security_snapshot_uuid={};break;case 23:b.generation_guards[0].reset();break;case 24:b.terminal_at={};break;case 25:b.result_uuid={};break;case 26:b.boundary_uuid={};break;case 27:b.evidence_uuid={};break;case 28:b.metric_evidence_uuid={};break;}Bad(b);}
 for(const auto& key:std::vector<std::string>{std::string{},std::string(513,'x'),std::string("\x80"),std::string("\xc0\x80"),std::string("\xe0\x80\x80"),std::string("\xed\xa0\x80"),std::string("\xf4\x90\x80\x80"),std::string("\xf0\x90")}){auto b=o;b.idempotency_key=key;Bad(b);b=o;b.steps[0].idempotency_key=key;Bad(b);}
 auto keys=o;keys.idempotency_key=std::string(512,'k');keys.steps[0].idempotency_key="\xf0\x90\x80\x80";Good(keys);
 Empty(db::EncodeNativeManagementOperation(o,raw.size()-1));Empty(db::DecodeNativeManagementOperation(raw,raw.size()-1));Check(db::EncodeNativeManagementOperation(o,raw.size()).ok(),"exact encoded byte budget");
 for(unsigned mode=0;mode<2;++mode){counting=true;allocations=0;const auto measured=mode?db::DecodeNativeManagementOperation(raw,budget):db::EncodeNativeManagementOperation(o,budget);counting=false;Check(measured.ok(),"allocation baseline");const auto sites=allocations;
  for(unsigned long at=0;at<sites;++at){allocation_budget=at;const auto r=mode?db::DecodeNativeManagementOperation(raw,budget):db::EncodeNativeManagementOperation(o,budget);allocation_budget=-1;Check(r.error==E::resource_exhausted,"every codec allocation classified");Empty(r);}
  for(unsigned fault=1;fault<=5;++fault)for(unsigned digest=1;digest<=2;++digest){hash_fault=fault;hash_target=digest;hash_seen=0;hash_active=false;const auto r=mode?db::DecodeNativeManagementOperation(raw,budget):db::EncodeNativeManagementOperation(o,budget);Check(hash_fault==0&&r.error==E::hash_failure,"every embedded and raw hash-provider failure");Empty(r);}}
 Transitions();RetainedHistory();MalformedWire(o,raw);
 auto a=Example(),b=a;b.revision=2;b.updated_at=Id(2002);
 for(unsigned field=0;field<12;++field){auto bad=b;switch(field){case 0:bad.descriptor_uuid=Id(44);break;case 1:bad.initiator_uuid=Id(44);break;case 2:bad.request_context_uuid=Id(44);break;case 3:bad.policy_snapshot_uuid=Id(44);break;case 4:bad.idempotency_key="changed";break;case 5:bad.normalized_request_sha256[0]^=1;break;case 6:bad.generation_guards[0]=1;break;case 7:bad.security_snapshot_uuid=Id(44);break;case 8:bad.target_uuid=Id(44);break;case 9:bad.revision=3;break;case 10:bad.updated_at=a.updated_at;break;case 11:bad.evidence_required=true;break;}
  Check(db::ValidateNativeManagementOperation(bad)==E::none&&db::ValidateNativeManagementOperationEvolution(a,bad)!=E::none,"individually valid rewritten history rejected");}
 counting=true;allocations=0;Check(db::ValidateNativeManagementOperationEvolution(a,b)==E::none,"evolution allocation baseline");counting=false;const auto sites=allocations;
 for(unsigned long at=0;at<sites;++at){allocation_budget=at;const auto result=db::ValidateNativeManagementOperationEvolution(a,b);allocation_budget=-1;Check(result==E::resource_exhausted,"evolution allocation failure");}
 std::cout<<"PASS native management operation checks="<<checks<<" representation_only=true not_authorization_or_SQL_E2E=true\n";
}
}
int main(){try{Test();return 0;}catch(const std::exception& e){allocation_budget=-1;hash_fault=0;std::cerr<<"FAIL native management operation checks="<<checks<<" "<<e.what()<<'\n';return 1;}}
