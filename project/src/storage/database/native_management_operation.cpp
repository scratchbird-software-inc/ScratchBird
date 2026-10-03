// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_management_operation.hpp"
#include "hash_digest_parts.hpp"
#include "uuid.hpp"
#include "../disk/native_decoded_storage_ranges.hpp"
#include <type_traits>
#include <algorithm>
#include <limits>
#include <new>
#include <set>
#include <stdexcept>
#include <string_view>

namespace scratchbird::storage::database {
namespace {
using namespace core::platform;
using E=NativeManagementOperationError;using O=NativeManagementOperation;
using S=NativeManagementStep;using OS=NativeManagementState;using SS=NativeManagementStepState;
template<class O> constexpr std::array<std::pair<std::size_t,Uuid O::*>,23> OperationIds{{
 {16,&O::database_uuid},{32,&O::bootstrap_uuid},{48,&O::uuid},{64,&O::descriptor_uuid},
 {80,&O::family_uuid},{96,&O::target_type_uuid},{112,&O::target_uuid},{128,&O::initiator_uuid},
 {144,&O::request_context_uuid},{160,&O::policy_snapshot_uuid},{176,&O::security_snapshot_uuid},
 {192,&O::phase_uuid},{208,&O::boundary_uuid},{224,&O::created_at},{240,&O::updated_at},
 {256,&O::terminal_at},{304,&O::resource_plan_uuid},{320,&O::lock_plan_uuid},{336,&O::result_uuid},
 {352,&O::diagnostic_uuid},{368,&O::evidence_uuid},{384,&O::metric_evidence_uuid},{400,&O::cluster_uuid}}};
template<class S> constexpr std::array<std::pair<std::size_t,Uuid S::*>,10> StepIds{{
 {0,&S::uuid},{16,&S::operation_uuid},{32,&S::family_uuid},{48,&S::target_uuid},
 {128,&S::started_at},{144,&S::completed_at},{160,&S::evidence_uuid},
 {176,&S::metric_evidence_uuid},{192,&S::diagnostic_uuid},{208,&S::boundary_uuid}}};
constexpr auto& operation_ids=OperationIds<O>;
constexpr auto& step_ids=StepIds<S>;
struct OwningIdentities {
  std::set<Uuid> values;
  bool Insert(const Uuid& id){return values.insert(id).second;}
};
struct BorrowedIdentities {
  std::span<Uuid> values;
  explicit BorrowedIdentities(std::span<Uuid> slots):values(slots){std::fill(values.begin(),values.end(),Uuid{});}
  bool Insert(const Uuid& id) {
    u64 hash=14695981039346656037ULL;
    for(auto byte:id.bytes){hash^=byte;hash*=1099511628211ULL;}
    auto at=hash%values.size();
    for(std::size_t probe=0;probe<values.size();++probe){
      if(values[at].is_nil()){values[at]=id;return true;}
      if(values[at]==id)return false;
      if(++at==values.size())at=0;
    }
    return false;
  }
};
void Require(bool b,E e){if(!b)throw e;}
bool Zero(const byte* p,std::size_t n){return std::all_of(p,p+n,[](byte b){return !b;});}
bool EmptyHash(const std::array<byte,32>& h){return Zero(h.data(),h.size());}
bool V7(const Uuid& id){return core::uuid::IsEngineIdentityUuid(id);}
bool Terminal(OS s){return s==OS::completed||s==OS::cancelled||s==OS::failed_terminal;}
bool Terminal(SS s){return s==SS::completed||s==SS::skipped||s==SS::compensated||s==SS::failed_terminal;}
bool Before(const Uuid& a,const Uuid& b){return a.bytes<b.bytes;}
bool Utf8(std::string_view text){
 if(text.empty()||text.size()>512)return false;
 for(std::size_t at=0;at<text.size();){const auto first=static_cast<byte>(text[at++]);u32 scalar=first;unsigned extra=0;
  if(first>=0xc2&&first<=0xdf){scalar=first&31;extra=1;}
  else if(first>=0xe0&&first<=0xef){scalar=first&15;extra=2;}
  else if(first>=0xf0&&first<=0xf4){scalar=first&7;extra=3;}
  else if(first>=0x80)return false;
  if(extra>text.size()-at)return false;
  for(unsigned i=0;i<extra;++i){const auto b=static_cast<byte>(text[at++]);if((b&0xc0)!=0x80)return false;scalar=(scalar<<6)|(b&63);}
  if((extra==1&&scalar<0x80)||(extra==2&&scalar<0x800)||(extra==3&&scalar<0x10000)||
    scalar>0x10ffff||(scalar>=0xd800&&scalar<=0xdfff))return false;
 }return true;
}
template<class Op> bool Time(const Uuid& id,const Op& o){return id.is_nil()||(!Before(id,o.created_at)&&!Before(o.updated_at,id));}
template<class Op,class Identities> E ValidateValues(const Op& o,Identities& ids){
 using Step=typename std::remove_cvref_t<decltype(o.steps)>::value_type;
 for(const auto* id:{&o.database_uuid,&o.bootstrap_uuid,&o.uuid,&o.descriptor_uuid,&o.family_uuid,
   &o.target_type_uuid,&o.initiator_uuid,&o.request_context_uuid,&o.policy_snapshot_uuid,&o.created_at,&o.updated_at})if(!(V7(*id)))return E::invalid_identity;
 for(const auto& [offset,member]:OperationIds<Op>){(void)offset;if(!((o.*member).is_nil()||V7(o.*member)))return E::invalid_identity;}
 if(!(o.revision&&u16(o.state)>=1&&u16(o.state)<=15&&u16(o.scope)>=1&&u16(o.scope)<=3&&
   o.initiator_kind>=1&&o.initiator_kind<=8&&u16(o.restart)>=1&&u16(o.restart)<=6))return E::invalid_record;
 if(!(Utf8(o.idempotency_key)))return E::invalid_utf8;
 if(!(!EmptyHash(o.normalized_request_sha256)&&!Before(o.updated_at,o.created_at)))return E::invalid_record;
 if(!o.normalized_request_bytes.empty()){
   if(!(o.normalized_request_bytes.size()<=std::numeric_limits<u32>::max()))return E::resource_exhausted;
   const auto digest=core::hash::ComputeSha256DigestNative(o.normalized_request_bytes.data(),o.normalized_request_bytes.size());
   if(!(digest.ok()))return E::hash_failure;
   if(!(digest.digest==o.normalized_request_sha256))return E::invalid_integrity;
 }
 const bool cluster=o.scope==NativeManagementScope::cluster;
 if(!(cluster?(!o.cluster_uuid.is_nil()&&o.generation_guards[3].has_value()):(o.cluster_uuid.is_nil()&&!o.generation_guards[3])))return E::invalid_record;
 if(!(o.steps.size()<=std::numeric_limits<u32>::max()))return E::resource_exhausted;
 const bool has_planning=!o.steps.empty()||!o.phase_uuid.is_nil()||!o.resource_plan_uuid.is_nil()||!o.lock_plan_uuid.is_nil();
 if(o.state==OS::created||o.state==OS::authorized)if(!(!has_planning))return E::invalid_record;
 if((o.state!=OS::created&&o.state!=OS::failed_terminal)||has_planning)
   if(!(!o.security_snapshot_uuid.is_nil()&&o.generation_guards[0]&&o.generation_guards[1]&&o.generation_guards[2]))return E::invalid_record;
 if((o.state!=OS::created&&o.state!=OS::authorized&&o.state!=OS::failed_terminal)||!o.steps.empty())
   if(!(!o.phase_uuid.is_nil()&&!o.resource_plan_uuid.is_nil()&&!o.lock_plan_uuid.is_nil()))return E::invalid_record;
 if(Terminal(o.state)){
   if(!(!o.terminal_at.is_nil()&&!o.result_uuid.is_nil()))return E::invalid_record;if(!(Time(o.terminal_at,o)))return E::invalid_record;
   if(o.evidence_required)if(!(!o.evidence_uuid.is_nil()))return E::invalid_record;
 }else if(!(o.terminal_at.is_nil()))return E::invalid_record;
 if(o.state==OS::completed||o.state==OS::cancelled)if(!(!o.boundary_uuid.is_nil()))return E::invalid_record;
 if(o.state==OS::failed_terminal)if(!(!o.diagnostic_uuid.is_nil()))return E::invalid_record;
 if(o.state==OS::completed&&o.metrics_required)if(!(!o.metric_evidence_uuid.is_nil()))return E::invalid_record;
 if(!(ids.Insert(o.database_uuid)&&ids.Insert(o.bootstrap_uuid)&&ids.Insert(o.uuid)))return E::invalid_identity;
 for(std::size_t i=0;i<o.steps.size();++i){const auto& s=o.steps[i];
   if(!(V7(s.uuid)&&V7(s.family_uuid)&&s.operation_uuid==o.uuid&&ids.Insert(s.uuid)))return E::invalid_identity;
   for(const auto& [offset,member]:StepIds<Step>){(void)offset;if(!((s.*member).is_nil()||V7(s.*member)))return E::invalid_identity;}
   if(!(s.ordinal==i+1&&u16(s.state)>=1&&u16(s.state)<=10&&u16(s.mutation)<=11&&u16(s.compensation)<=4&&u16(s.recovery)>=1&&u16(s.recovery)<=7))return E::invalid_record;
   if(!(Utf8(s.idempotency_key)))return E::invalid_utf8;
   if(!(s.mutation!=NativeManagementMutation::cluster||cluster))return E::invalid_record;
   if(!(s.idempotent||s.mutation==NativeManagementMutation::none||s.compensation!=NativeManagementCompensation::none))return E::invalid_record;
   if(s.state!=SS::pending&&s.state!=SS::skipped&&s.state!=SS::failed_terminal)if(!(!EmptyHash(s.precondition_sha256)))return E::invalid_record;
   if(s.state==SS::running||s.state==SS::compensating||s.state==SS::completed||s.state==SS::compensated)if(!(!s.started_at.is_nil()))return E::invalid_record;
   if(!(Time(s.started_at,o)&&Time(s.completed_at,o)))return E::invalid_record;
   if(!s.started_at.is_nil()&&!s.completed_at.is_nil())if(!(!Before(s.completed_at,s.started_at)))return E::invalid_record;
   if(!(Terminal(s.state)?!s.completed_at.is_nil():s.completed_at.is_nil()))return E::invalid_record;
   if(!Terminal(s.state))if(!(EmptyHash(s.postcondition_sha256)))return E::invalid_record;
   if(s.state==SS::completed||s.state==SS::compensated){if(!(!EmptyHash(s.postcondition_sha256)&&!s.boundary_uuid.is_nil()))return E::invalid_record;
     if(s.metrics_required)if(!(!s.metric_evidence_uuid.is_nil()))return E::invalid_record;}
   if(s.state==SS::failed_terminal)if(!(!s.diagnostic_uuid.is_nil()))return E::invalid_record;
   if(Terminal(s.state)&&s.evidence_required)if(!(!s.evidence_uuid.is_nil()))return E::invalid_record;
   if(s.state==SS::pending)if(!(s.started_at.is_nil()&&s.completed_at.is_nil()&&EmptyHash(s.postcondition_sha256)&&s.boundary_uuid.is_nil()))return E::invalid_record;
   if(s.state==SS::skipped)if(!(EmptyHash(s.postcondition_sha256)))return E::invalid_record;
   if(o.state==OS::completed||o.state==OS::cancelled)if(!(Terminal(s.state)&&(o.state!=OS::completed||s.state!=SS::failed_terminal)))return E::invalid_record;
   if(o.state==OS::failed_terminal&&!s.started_at.is_nil())if(!(!o.boundary_uuid.is_nil()))return E::invalid_record;
 }
 return E::none;
}
constexpr std::array<u16,15> operation_edges{{
 (1u<<1)|(1u<<14),(1u<<2)|(1u<<14),(1u<<3)|(1u<<5)|(1u<<12)|(1u<<14),
 (1u<<4)|(1u<<6)|(1u<<7)|(1u<<8)|(1u<<9)|(1u<<10)|(1u<<14),
 (1u<<5)|(1u<<3)|(1u<<9),(1u<<3)|(1u<<6)|(1u<<14),
 (1u<<12)|(1u<<9)|(1u<<14)|(1u<<11),(1u<<3)|(1u<<4)|(1u<<6)|(1u<<9)|(1u<<11),
 (1u<<13)|(1u<<9)|(1u<<11),(1u<<3)|(1u<<6)|(1u<<14),
 (1u<<3)|(1u<<13)|(1u<<9)|(1u<<14)|(1u<<11),(1u<<10)|(1u<<14)|(1u<<12),0,0,0}};
constexpr std::array<u16,10> step_edges{{
 (1u<<1)|(1u<<7)|(1u<<9),(1u<<2)|(1u<<3)|(1u<<7)|(1u<<5),
 (1u<<6)|(1u<<3)|(1u<<4)|(1u<<5)|(1u<<9),(1u<<1)|(1u<<2)|(1u<<7)|(1u<<5)|(1u<<9),
 (1u<<8)|(1u<<5)|(1u<<9),(1u<<1)|(1u<<2)|(1u<<4)|(1u<<9),0,0,0,0}};
template<class Op> bool SameOperation(const Op& a,const Op& b) {
 for(const auto& [at,member]:OperationIds<Op>){(void)at;if(a.*member!=b.*member)return false;}
 return a.normalized_request_sha256==b.normalized_request_sha256&&
   std::equal(a.normalized_request_bytes.begin(),a.normalized_request_bytes.end(),b.normalized_request_bytes.begin(),b.normalized_request_bytes.end())&&
   a.generation_guards==b.generation_guards&&a.revision==b.revision&&a.idempotency_key==b.idempotency_key&&
   a.state==b.state&&a.scope==b.scope&&a.initiator_kind==b.initiator_kind&&a.restart==b.restart&&
   a.evidence_required==b.evidence_required&&a.metrics_required==b.metrics_required&&
   std::equal(a.steps.begin(),a.steps.end(),b.steps.begin(),b.steps.end());
}
template<class Op,class Validator> E EvolutionValues(const Op& a,const Op& b,Validator validate){
 using Step=typename std::remove_cvref_t<decltype(a.steps)>::value_type;
 const auto first=validate(a);if(first!=E::none)return first;
 const auto second=validate(b);if(second!=E::none)return second;
 if(SameOperation(a,b))return E::none;
 if(!(!Terminal(a.state)&&a.revision!=std::numeric_limits<u64>::max()&&b.revision==a.revision+1&&Before(a.updated_at,b.updated_at)))return E::invalid_transition;
 if(!(a.state==b.state||(operation_edges[u16(a.state)-1]&(1u<<(u16(b.state)-1)))))return E::invalid_transition;
 if(a.terminal_at.is_nil()&&!b.terminal_at.is_nil())if(!(!Before(b.terminal_at,a.updated_at)))return E::invalid_transition;
 for(auto member:{&Op::database_uuid,&Op::bootstrap_uuid,&Op::uuid,&Op::descriptor_uuid,&Op::family_uuid,&Op::target_type_uuid,
   &Op::target_uuid,&Op::initiator_uuid,&Op::request_context_uuid,&Op::policy_snapshot_uuid,&Op::created_at,&Op::cluster_uuid})if(!(a.*member==b.*member))return E::immutable_field;
 if(!(a.scope==b.scope&&a.initiator_kind==b.initiator_kind&&a.restart==b.restart&&a.idempotency_key==b.idempotency_key&&
   a.normalized_request_sha256==b.normalized_request_sha256&&std::equal(a.normalized_request_bytes.begin(),a.normalized_request_bytes.end(),b.normalized_request_bytes.begin(),b.normalized_request_bytes.end())&&a.generation_guards[3]==b.generation_guards[3]&&
   a.evidence_required==b.evidence_required&&a.metrics_required==b.metrics_required))return E::immutable_field;
 if(!a.security_snapshot_uuid.is_nil())if(!(a.security_snapshot_uuid==b.security_snapshot_uuid))return E::immutable_field;
 for(unsigned i=0;i<3;++i)if(a.generation_guards[i])if(!(a.generation_guards[i]==b.generation_guards[i]))return E::immutable_field;
 if(!(b.steps.size()>=a.steps.size()))return E::invalid_transition;
 for(std::size_t i=0;i<b.steps.size();++i){const auto& y=b.steps[i];if(i>=a.steps.size()){if(!(y.state==SS::pending))return E::invalid_transition;continue;}
   const auto& x=a.steps[i];if(x==y)continue;if(!(!Terminal(x.state)))return E::invalid_transition;
   if(!(x.state==y.state||(step_edges[u16(x.state)-1]&(1u<<(u16(y.state)-1)))))return E::invalid_transition;
   for(auto member:{&Step::uuid,&Step::operation_uuid,&Step::family_uuid,&Step::target_uuid})if(!(x.*member==y.*member))return E::immutable_field;
   if(!(x.ordinal==y.ordinal&&x.idempotency_key==y.idempotency_key&&x.mutation==y.mutation&&x.compensation==y.compensation&&
     x.recovery==y.recovery&&x.evidence_required==y.evidence_required&&x.metrics_required==y.metrics_required&&x.idempotent==y.idempotent))return E::immutable_field;
   if(!EmptyHash(x.precondition_sha256))if(!(x.precondition_sha256==y.precondition_sha256))return E::immutable_field;
   if(!x.started_at.is_nil()){
     if(x.started_at!=y.started_at)return E::immutable_field;
   }else if(!y.started_at.is_nil()){
     if(y.state!=SS::running||Before(y.started_at,a.updated_at))return E::invalid_transition;
   }
   if(x.completed_at.is_nil()&&!y.completed_at.is_nil())if(!(!Before(y.completed_at,a.updated_at)))return E::invalid_transition;
 }
 return E::none;
}
NativeManagementOperationImage Fail(E e){NativeManagementOperationImage r;r.error=e;return r;}
auto Seal(std::span<const byte> b){const std::array<byte,32> zeros{};const core::hash::HashDigestSegment parts[]={{b.data(),480},{zeros.data(),32},{b.data()+512,b.size()-512}};return core::hash::ComputeSha256DigestPartsNative(parts,3);}
void Put(byte* p,const Uuid& id){std::copy(id.bytes.begin(),id.bytes.end(),p);}
Uuid Get(const byte* p){Uuid id;std::copy_n(p,16,id.bytes.begin());return id;}
std::size_t Size(const O& o,u64 budget){u64 n=512;Require(budget>=n,E::resource_exhausted);
 const auto add=[&](u64 size){Require(size<=budget-n&&size<=std::numeric_limits<u32>::max()-n,E::resource_exhausted);n+=size;};
 add(o.idempotency_key.size());add(o.normalized_request_bytes.size());for(const auto& s:o.steps){add(256);add(s.idempotency_key.size());}return static_cast<std::size_t>(n);
}
} // namespace
NativeManagementOperationError ValidateNativeManagementOperation(const O& o) noexcept {
 try{OwningIdentities ids;return ValidateValues(o,ids);}catch(E e){return e;}catch(const std::bad_alloc&){return E::resource_exhausted;}catch(const std::length_error&){return E::resource_exhausted;}catch(...){return E::invalid_record;}
}
NativeManagementOperationError ValidateNativeManagementOperationEvolution(const O& a,const O& b) noexcept {
 try{return EvolutionValues(a,b,[](const auto& o){OwningIdentities ids;return ValidateValues(o,ids);});}catch(E e){return e;}catch(const std::bad_alloc&){return E::resource_exhausted;}catch(const std::length_error&){return E::resource_exhausted;}catch(...){return E::invalid_record;}
}
NativeManagementOperationImage EncodeNativeManagementOperation(const O& o,u64 budget) noexcept {
 try{const auto size=Size(o,budget);const auto valid=ValidateNativeManagementOperation(o);Require(valid==E::none,valid);const bool exact=!o.normalized_request_bytes.empty();
  std::vector<byte> b(size,0);std::copy_n(exact?"SBMGO002":"SBMGO001",8,b.begin());StoreLittle16(b.data()+8,exact?2:1);StoreLittle16(b.data()+10,512);StoreLittle32(b.data()+12,size);
  for(const auto& [at,member]:operation_ids)Put(b.data()+at,o.*member);
  std::copy(o.normalized_request_sha256.begin(),o.normalized_request_sha256.end(),b.begin()+272);
  u32 flags=(o.evidence_required?16u:0u)|(o.metrics_required?32u:0u);for(unsigned i=0;i<4;++i)if(o.generation_guards[i]){flags|=1u<<i;StoreLittle64(b.data()+416+8*i,*o.generation_guards[i]);}
  StoreLittle64(b.data()+448,o.revision);StoreLittle32(b.data()+456,o.steps.size());StoreLittle32(b.data()+460,o.idempotency_key.size());StoreLittle16(b.data()+464,u16(o.state));StoreLittle16(b.data()+466,u16(o.scope));StoreLittle16(b.data()+468,o.initiator_kind);StoreLittle16(b.data()+470,u16(o.restart));StoreLittle32(b.data()+472,flags);
  std::size_t at=512;std::copy(o.idempotency_key.begin(),o.idempotency_key.end(),b.begin()+at);at+=o.idempotency_key.size();
  StoreLittle32(b.data()+476,o.normalized_request_bytes.size());
  std::copy(o.normalized_request_bytes.begin(),o.normalized_request_bytes.end(),b.begin()+at);at+=o.normalized_request_bytes.size();
  for(const auto& s:o.steps){auto* p=b.data()+at;for(const auto& [offset,member]:step_ids)Put(p+offset,s.*member);
   std::copy(s.precondition_sha256.begin(),s.precondition_sha256.end(),p+64);std::copy(s.postcondition_sha256.begin(),s.postcondition_sha256.end(),p+96);
   StoreLittle32(p+224,s.ordinal);StoreLittle32(p+228,s.idempotency_key.size());StoreLittle16(p+232,u16(s.state));StoreLittle16(p+234,u16(s.mutation));StoreLittle16(p+236,u16(s.compensation));StoreLittle16(p+238,u16(s.recovery));StoreLittle32(p+240,(s.evidence_required?1u:0u)|(s.metrics_required?2u:0u)|(s.idempotent?4u:0u));
   at+=256;std::copy(s.idempotency_key.begin(),s.idempotency_key.end(),b.begin()+at);at+=s.idempotency_key.size();}
  const auto seal=Seal(b);Require(seal.ok(),E::hash_failure);std::copy(seal.digest.begin(),seal.digest.end(),b.begin()+480);
  const auto digest=core::hash::ComputeSha256Digest(b);Require(digest.ok(),E::hash_failure);return {E::none,o,std::move(b),digest.digest};
 }catch(E e){return Fail(e);}catch(const std::bad_alloc&){return Fail(E::resource_exhausted);}catch(const std::length_error&){return Fail(E::resource_exhausted);}catch(...){return Fail(E::invalid_record);}
}
namespace {
template<bool Borrowed> auto DecodeOperation(std::span<const byte> b,u64 budget,NativeManagementOperationViewWorkspace workspace) noexcept
 -> std::conditional_t<Borrowed,NativeManagementOperationViewImage,NativeManagementOperationImage> {
 using Result=std::conditional_t<Borrowed,NativeManagementOperationViewImage,NativeManagementOperationImage>;
 using Op=std::conditional_t<Borrowed,NativeManagementOperationView,O>;
 using Step=std::conditional_t<Borrowed,NativeManagementStepView,S>;
 const auto fail=[](E e){Result result;result.error=e;return result;};
 try{if(!(b.size()<=budget&&b.size()<=std::numeric_limits<u32>::max()))return fail(E::resource_exhausted);
  if(!(b.size()>=512))return fail(E::invalid_header);const auto* p=b.data();
  const auto version=LoadLittle16(p+8);const auto request_bytes=LoadLittle32(p+476);
  if(!((version==1||version==2)&&std::string_view(reinterpret_cast<const char*>(p),8)==(version==2?"SBMGO002":"SBMGO001")&&
    LoadLittle16(p+10)==512&&LoadLittle32(p+12)==b.size()&&(version==2?request_bytes!=0:request_bytes==0)))return fail(E::invalid_header);
  const auto seal=Seal(b);if(!(seal.ok()))return fail(E::hash_failure);if(!(std::equal(seal.digest.begin(),seal.digest.end(),p+480)))return fail(E::invalid_integrity);
  const auto flags=LoadLittle32(p+472);if(!(!(flags&~63u)))return fail(E::invalid_header);
  Op o;for(const auto& [at,member]:OperationIds<Op>)o.*member=Get(p+at);std::copy_n(p+272,32,o.normalized_request_sha256.begin());
  for(unsigned i=0;i<4;++i){const auto value=LoadLittle64(p+416+8*i);if(flags&(1u<<i))o.generation_guards[i]=value;else if(!(!value))return fail(E::invalid_header);}
  o.evidence_required=flags&16;o.metrics_required=flags&32;o.revision=LoadLittle64(p+448);o.state=OS(LoadLittle16(p+464));o.scope=NativeManagementScope(LoadLittle16(p+466));o.initiator_kind=LoadLittle16(p+468);o.restart=NativeManagementRestart(LoadLittle16(p+470));
  const auto count=LoadLittle32(p+456),key=LoadLittle32(p+460);if(!(key>=1&&key<=512&&key<=b.size()-512))return fail(E::invalid_header);
  std::size_t at=512+key;if(!(request_bytes<=b.size()-at))return fail(E::invalid_header);
  if(!(count<=(b.size()-at-request_bytes)/257))return fail(E::invalid_header);
  o.idempotency_key=decltype(o.idempotency_key){reinterpret_cast<const char*>(p+512),key};
  if constexpr(Borrowed){
    if(!(disk::detail::DisjointNativeDecodeRegions(b,workspace.steps,workspace.identities)))return fail(E::invalid_workspace);
    if(!(workspace.steps.size()>=count&&workspace.identities.size()>=2*(std::size_t{count}+3)+1))return fail(E::resource_exhausted);
    o.normalized_request_bytes=b.subspan(at,request_bytes);o.steps=workspace.steps.first(count);
  }else{o.normalized_request_bytes.assign(b.begin()+at,b.begin()+at+request_bytes);o.steps.resize(count);}
  at+=request_bytes;
  for(u32 i=0;i<count;++i){if(!(b.size()-at>=256))return fail(E::invalid_header);p=b.data()+at;Step s;for(const auto& [offset,member]:StepIds<Step>)s.*member=Get(p+offset);
   std::copy_n(p+64,32,s.precondition_sha256.begin());std::copy_n(p+96,32,s.postcondition_sha256.begin());s.ordinal=LoadLittle32(p+224);const auto length=LoadLittle32(p+228);
   s.state=SS(LoadLittle16(p+232));s.mutation=NativeManagementMutation(LoadLittle16(p+234));s.compensation=NativeManagementCompensation(LoadLittle16(p+236));s.recovery=NativeManagementRecovery(LoadLittle16(p+238));
   const auto sf=LoadLittle32(p+240);if(!(!(sf&~7u)&&Zero(p+244,12)&&length>=1&&length<=512&&length<=b.size()-at-256))return fail(E::invalid_header);
   s.evidence_required=sf&1;s.metrics_required=sf&2;s.idempotent=sf&4;at+=256;s.idempotency_key=decltype(s.idempotency_key){reinterpret_cast<const char*>(b.data()+at),length};at+=length;
   if constexpr(Borrowed)workspace.steps[i]=s;else o.steps[i]=std::move(s);}
  if(!(at==b.size()))return fail(E::invalid_header);E valid;
  if constexpr(Borrowed){BorrowedIdentities ids(workspace.identities.first(2*(std::size_t{count}+3)+1));valid=ValidateValues(o,ids);}
  else{OwningIdentities ids;valid=ValidateValues(o,ids);}
  if(valid!=E::none)return fail(valid);
  const auto digest=core::hash::ComputeSha256DigestNative(b.data(),b.size());if(!(digest.ok()))return fail(E::hash_failure);
  if constexpr(Borrowed)return {E::none,std::move(o),b,digest.digest};
  else return {E::none,std::move(o),std::vector<byte>(b.begin(),b.end()),digest.digest};
 }catch(E e){return fail(e);}catch(const std::bad_alloc&){return fail(E::resource_exhausted);}catch(const std::length_error&){return fail(E::resource_exhausted);}catch(...){return fail(E::invalid_record);}
}
bool ViewScratchDisjoint(const NativeManagementOperationView& o,std::span<Uuid> ids) {
 const auto bytes=[](const void* p,std::size_t n){return std::span<const byte>{static_cast<const byte*>(p),n};};
 if(!disk::detail::DisjointNativeDecodeRegions(bytes(&o,sizeof(o)),o.steps,ids)||
    !disk::detail::DisjointNativeDecodeRegions(o.normalized_request_bytes,ids)||
    !disk::detail::DisjointNativeDecodeRegions(bytes(o.idempotency_key.data(),o.idempotency_key.size()),ids))return false;
 for(const auto& s:o.steps)if(!disk::detail::DisjointNativeDecodeRegions(bytes(s.idempotency_key.data(),s.idempotency_key.size()),ids))return false;
 return true;
}
E ValidateView(const NativeManagementOperationView& o,std::span<Uuid> scratch) {
 if(o.steps.size()>(std::numeric_limits<std::size_t>::max()-7)/2)return E::resource_exhausted;
 const auto count=2*o.steps.size()+7;
 if(scratch.size()<count)return E::resource_exhausted;
 BorrowedIdentities ids(scratch.first(count));return ValidateValues(o,ids);
}
} // namespace
NativeManagementOperationImage DecodeNativeManagementOperation(const std::vector<byte>& b,u64 budget) noexcept {
 return DecodeOperation<false>(b,budget,{});
}
NativeManagementOperationViewImage DecodeNativeManagementOperationInto(
 std::span<const byte> b,u64 budget,NativeManagementOperationViewWorkspace workspace) noexcept {
 return DecodeOperation<true>(b,budget,workspace);
}
NativeManagementOperationError ValidateNativeManagementOperationView(
 const NativeManagementOperationView& o,std::span<Uuid> ids) noexcept {
 if(!ViewScratchDisjoint(o,ids))return E::invalid_workspace;
 return ValidateView(o,ids);
}
NativeManagementOperationError ValidateNativeManagementOperationEvolutionView(
 const NativeManagementOperationView& a,const NativeManagementOperationView& b,std::span<Uuid> ids) noexcept {
 if(!ViewScratchDisjoint(a,ids)||!ViewScratchDisjoint(b,ids))return E::invalid_workspace;
 return EvolutionValues(a,b,[&](const auto& o){return ValidateView(o,ids);});
}

} // namespace scratchbird::storage::database
