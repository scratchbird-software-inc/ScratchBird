// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_management_operation.hpp"
#include "hash_digest_parts.hpp"
#include "uuid.hpp"
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
constexpr std::array<std::pair<std::size_t,Uuid O::*>,23> operation_ids{{
 {16,&O::database_uuid},{32,&O::bootstrap_uuid},{48,&O::uuid},{64,&O::descriptor_uuid},
 {80,&O::family_uuid},{96,&O::target_type_uuid},{112,&O::target_uuid},{128,&O::initiator_uuid},
 {144,&O::request_context_uuid},{160,&O::policy_snapshot_uuid},{176,&O::security_snapshot_uuid},
 {192,&O::phase_uuid},{208,&O::boundary_uuid},{224,&O::created_at},{240,&O::updated_at},
 {256,&O::terminal_at},{304,&O::resource_plan_uuid},{320,&O::lock_plan_uuid},{336,&O::result_uuid},
 {352,&O::diagnostic_uuid},{368,&O::evidence_uuid},{384,&O::metric_evidence_uuid},{400,&O::cluster_uuid}}};
constexpr std::array<std::pair<std::size_t,Uuid S::*>,10> step_ids{{
 {0,&S::uuid},{16,&S::operation_uuid},{32,&S::family_uuid},{48,&S::target_uuid},
 {128,&S::started_at},{144,&S::completed_at},{160,&S::evidence_uuid},
 {176,&S::metric_evidence_uuid},{192,&S::diagnostic_uuid},{208,&S::boundary_uuid}}};
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
void Optional(const Uuid& id){Require(id.is_nil()||V7(id),E::invalid_identity);}
void Time(const Uuid& id,const O& o){if(!id.is_nil())Require(!Before(id,o.created_at)&&!Before(o.updated_at,id),E::invalid_record);}
void Validate(const O& o){
 for(const auto* id:{&o.database_uuid,&o.bootstrap_uuid,&o.uuid,&o.descriptor_uuid,&o.family_uuid,
   &o.target_type_uuid,&o.initiator_uuid,&o.request_context_uuid,&o.policy_snapshot_uuid,&o.created_at,&o.updated_at})Require(V7(*id),E::invalid_identity);
 for(const auto& [offset,member]:operation_ids){(void)offset;Optional(o.*member);}
 Require(o.revision&&u16(o.state)>=1&&u16(o.state)<=15&&u16(o.scope)>=1&&u16(o.scope)<=3&&
   o.initiator_kind>=1&&o.initiator_kind<=8&&u16(o.restart)>=1&&u16(o.restart)<=6,E::invalid_record);
 Require(Utf8(o.idempotency_key),E::invalid_utf8);
 Require(!EmptyHash(o.normalized_request_sha256)&&!Before(o.updated_at,o.created_at),E::invalid_record);
 const bool cluster=o.scope==NativeManagementScope::cluster;
 Require(cluster?(!o.cluster_uuid.is_nil()&&o.generation_guards[3].has_value()):(o.cluster_uuid.is_nil()&&!o.generation_guards[3]),E::invalid_record);
 Require(o.steps.size()<=std::numeric_limits<u32>::max(),E::resource_exhausted);
 const bool has_planning=!o.steps.empty()||!o.phase_uuid.is_nil()||!o.resource_plan_uuid.is_nil()||!o.lock_plan_uuid.is_nil();
 if(o.state==OS::created||o.state==OS::authorized)Require(!has_planning,E::invalid_record);
 if((o.state!=OS::created&&o.state!=OS::failed_terminal)||has_planning)
   Require(!o.security_snapshot_uuid.is_nil()&&o.generation_guards[0]&&o.generation_guards[1]&&o.generation_guards[2],E::invalid_record);
 if((o.state!=OS::created&&o.state!=OS::authorized&&o.state!=OS::failed_terminal)||!o.steps.empty())
   Require(!o.phase_uuid.is_nil()&&!o.resource_plan_uuid.is_nil()&&!o.lock_plan_uuid.is_nil(),E::invalid_record);
 if(Terminal(o.state)){
   Require(!o.terminal_at.is_nil()&&!o.result_uuid.is_nil(),E::invalid_record);Time(o.terminal_at,o);
   if(o.evidence_required)Require(!o.evidence_uuid.is_nil(),E::invalid_record);
 }else Require(o.terminal_at.is_nil(),E::invalid_record);
 if(o.state==OS::completed||o.state==OS::cancelled)Require(!o.boundary_uuid.is_nil(),E::invalid_record);
 if(o.state==OS::failed_terminal)Require(!o.diagnostic_uuid.is_nil(),E::invalid_record);
 if(o.state==OS::completed&&o.metrics_required)Require(!o.metric_evidence_uuid.is_nil(),E::invalid_record);
 std::set<Uuid> ids{o.database_uuid,o.bootstrap_uuid,o.uuid};Require(ids.size()==3,E::invalid_identity);
 for(std::size_t i=0;i<o.steps.size();++i){const auto& s=o.steps[i];
   Require(V7(s.uuid)&&V7(s.family_uuid)&&s.operation_uuid==o.uuid&&ids.insert(s.uuid).second,E::invalid_identity);
   for(const auto& [offset,member]:step_ids){(void)offset;Optional(s.*member);}
   Require(s.ordinal==i+1&&u16(s.state)>=1&&u16(s.state)<=10&&u16(s.mutation)<=11&&u16(s.compensation)<=4&&u16(s.recovery)>=1&&u16(s.recovery)<=7,E::invalid_record);
   Require(Utf8(s.idempotency_key),E::invalid_utf8);
   Require(s.mutation!=NativeManagementMutation::cluster||cluster,E::invalid_record);
   Require(s.idempotent||s.mutation==NativeManagementMutation::none||s.compensation!=NativeManagementCompensation::none,E::invalid_record);
   if(s.state!=SS::pending&&s.state!=SS::skipped&&s.state!=SS::failed_terminal)Require(!EmptyHash(s.precondition_sha256),E::invalid_record);
   if(s.state==SS::running||s.state==SS::compensating||s.state==SS::completed||s.state==SS::compensated)Require(!s.started_at.is_nil(),E::invalid_record);
   Time(s.started_at,o);Time(s.completed_at,o);
   if(!s.started_at.is_nil()&&!s.completed_at.is_nil())Require(!Before(s.completed_at,s.started_at),E::invalid_record);
   Require(Terminal(s.state)?!s.completed_at.is_nil():s.completed_at.is_nil(),E::invalid_record);
   if(!Terminal(s.state))Require(EmptyHash(s.postcondition_sha256),E::invalid_record);
   if(s.state==SS::completed||s.state==SS::compensated){Require(!EmptyHash(s.postcondition_sha256)&&!s.boundary_uuid.is_nil(),E::invalid_record);
     if(s.metrics_required)Require(!s.metric_evidence_uuid.is_nil(),E::invalid_record);}
   if(s.state==SS::failed_terminal)Require(!s.diagnostic_uuid.is_nil(),E::invalid_record);
   if(Terminal(s.state)&&s.evidence_required)Require(!s.evidence_uuid.is_nil(),E::invalid_record);
   if(s.state==SS::pending)Require(s.started_at.is_nil()&&s.completed_at.is_nil()&&EmptyHash(s.postcondition_sha256)&&s.boundary_uuid.is_nil(),E::invalid_record);
   if(s.state==SS::skipped)Require(EmptyHash(s.postcondition_sha256),E::invalid_record);
   if(o.state==OS::completed||o.state==OS::cancelled)Require(Terminal(s.state)&&(o.state!=OS::completed||s.state!=SS::failed_terminal),E::invalid_record);
   if(o.state==OS::failed_terminal&&!s.started_at.is_nil())Require(!o.boundary_uuid.is_nil(),E::invalid_record);
 }
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
void Evolution(const O& a,const O& b){
 Validate(a);Validate(b);if(a==b)return;
 Require(!Terminal(a.state)&&a.revision!=std::numeric_limits<u64>::max()&&b.revision==a.revision+1&&Before(a.updated_at,b.updated_at),E::invalid_transition);
 Require(a.state==b.state||(operation_edges[u16(a.state)-1]&(1u<<(u16(b.state)-1))),E::invalid_transition);
 if(a.terminal_at.is_nil()&&!b.terminal_at.is_nil())Require(!Before(b.terminal_at,a.updated_at),E::invalid_transition);
 for(auto member:{&O::database_uuid,&O::bootstrap_uuid,&O::uuid,&O::descriptor_uuid,&O::family_uuid,&O::target_type_uuid,
   &O::target_uuid,&O::initiator_uuid,&O::request_context_uuid,&O::policy_snapshot_uuid,&O::created_at,&O::cluster_uuid})Require(a.*member==b.*member,E::immutable_field);
 Require(a.scope==b.scope&&a.initiator_kind==b.initiator_kind&&a.restart==b.restart&&a.idempotency_key==b.idempotency_key&&
   a.normalized_request_sha256==b.normalized_request_sha256&&a.generation_guards[3]==b.generation_guards[3]&&
   a.evidence_required==b.evidence_required&&a.metrics_required==b.metrics_required,E::immutable_field);
 if(!a.security_snapshot_uuid.is_nil())Require(a.security_snapshot_uuid==b.security_snapshot_uuid,E::immutable_field);
 for(unsigned i=0;i<3;++i)if(a.generation_guards[i])Require(a.generation_guards[i]==b.generation_guards[i],E::immutable_field);
 Require(b.steps.size()>=a.steps.size(),E::invalid_transition);
 for(std::size_t i=0;i<b.steps.size();++i){const auto& y=b.steps[i];if(i>=a.steps.size()){Require(y.state==SS::pending,E::invalid_transition);continue;}
   const auto& x=a.steps[i];if(x==y)continue;Require(!Terminal(x.state),E::invalid_transition);
   Require(x.state==y.state||(step_edges[u16(x.state)-1]&(1u<<(u16(y.state)-1))),E::invalid_transition);
   for(auto member:{&S::uuid,&S::operation_uuid,&S::family_uuid,&S::target_uuid})Require(x.*member==y.*member,E::immutable_field);
   Require(x.ordinal==y.ordinal&&x.idempotency_key==y.idempotency_key&&x.mutation==y.mutation&&x.compensation==y.compensation&&
     x.recovery==y.recovery&&x.evidence_required==y.evidence_required&&x.metrics_required==y.metrics_required&&x.idempotent==y.idempotent,E::immutable_field);
   if(!EmptyHash(x.precondition_sha256))Require(x.precondition_sha256==y.precondition_sha256,E::immutable_field);
   if(!x.started_at.is_nil())Require(x.started_at==y.started_at,E::immutable_field);
   else if(!y.started_at.is_nil())Require(y.state==SS::running&&!Before(y.started_at,a.updated_at),E::invalid_transition);
   if(x.completed_at.is_nil()&&!y.completed_at.is_nil())Require(!Before(y.completed_at,a.updated_at),E::invalid_transition);
 }
}
NativeManagementOperationImage Fail(E e){NativeManagementOperationImage r;r.error=e;return r;}
auto Seal(const std::vector<byte>& b){const std::array<byte,32> zeros{};const core::hash::HashDigestSegment parts[]={{b.data(),480},{zeros.data(),32},{b.data()+512,b.size()-512}};return core::hash::ComputeSha256DigestParts(parts,3);}
void Put(byte* p,const Uuid& id){std::copy(id.bytes.begin(),id.bytes.end(),p);}
Uuid Get(const byte* p){Uuid id;std::copy_n(p,16,id.bytes.begin());return id;}
std::size_t Size(const O& o,u64 budget){u64 n=512;Require(budget>=n,E::resource_exhausted);
 const auto add=[&](u64 size){Require(size<=budget-n&&size<=std::numeric_limits<u32>::max()-n,E::resource_exhausted);n+=size;};
 add(o.idempotency_key.size());for(const auto& s:o.steps){add(256);add(s.idempotency_key.size());}return static_cast<std::size_t>(n);
}
} // namespace
NativeManagementOperationError ValidateNativeManagementOperation(const O& o) noexcept {
 try{Validate(o);return E::none;}catch(E e){return e;}catch(const std::bad_alloc&){return E::resource_exhausted;}catch(const std::length_error&){return E::resource_exhausted;}catch(...){return E::invalid_record;}
}
NativeManagementOperationError ValidateNativeManagementOperationEvolution(const O& a,const O& b) noexcept {
 try{Evolution(a,b);return E::none;}catch(E e){return e;}catch(const std::bad_alloc&){return E::resource_exhausted;}catch(const std::length_error&){return E::resource_exhausted;}catch(...){return E::invalid_record;}
}
NativeManagementOperationImage EncodeNativeManagementOperation(const O& o,u64 budget) noexcept {
 try{const auto size=Size(o,budget);Validate(o);std::vector<byte> b(size,0);std::copy_n("SBMGO001",8,b.begin());StoreLittle16(b.data()+8,1);StoreLittle16(b.data()+10,512);StoreLittle32(b.data()+12,size);
  for(const auto& [at,member]:operation_ids)Put(b.data()+at,o.*member);
  std::copy(o.normalized_request_sha256.begin(),o.normalized_request_sha256.end(),b.begin()+272);
  u32 flags=(o.evidence_required?16u:0u)|(o.metrics_required?32u:0u);for(unsigned i=0;i<4;++i)if(o.generation_guards[i]){flags|=1u<<i;StoreLittle64(b.data()+416+8*i,*o.generation_guards[i]);}
  StoreLittle64(b.data()+448,o.revision);StoreLittle32(b.data()+456,o.steps.size());StoreLittle32(b.data()+460,o.idempotency_key.size());StoreLittle16(b.data()+464,u16(o.state));StoreLittle16(b.data()+466,u16(o.scope));StoreLittle16(b.data()+468,o.initiator_kind);StoreLittle16(b.data()+470,u16(o.restart));StoreLittle32(b.data()+472,flags);
  std::size_t at=512;std::copy(o.idempotency_key.begin(),o.idempotency_key.end(),b.begin()+at);at+=o.idempotency_key.size();
  for(const auto& s:o.steps){auto* p=b.data()+at;for(const auto& [offset,member]:step_ids)Put(p+offset,s.*member);
   std::copy(s.precondition_sha256.begin(),s.precondition_sha256.end(),p+64);std::copy(s.postcondition_sha256.begin(),s.postcondition_sha256.end(),p+96);
   StoreLittle32(p+224,s.ordinal);StoreLittle32(p+228,s.idempotency_key.size());StoreLittle16(p+232,u16(s.state));StoreLittle16(p+234,u16(s.mutation));StoreLittle16(p+236,u16(s.compensation));StoreLittle16(p+238,u16(s.recovery));StoreLittle32(p+240,(s.evidence_required?1u:0u)|(s.metrics_required?2u:0u)|(s.idempotent?4u:0u));
   at+=256;std::copy(s.idempotency_key.begin(),s.idempotency_key.end(),b.begin()+at);at+=s.idempotency_key.size();}
  const auto seal=Seal(b);Require(seal.ok(),E::hash_failure);std::copy(seal.digest.begin(),seal.digest.end(),b.begin()+480);
  const auto digest=core::hash::ComputeSha256Digest(b);Require(digest.ok(),E::hash_failure);return {E::none,o,std::move(b),digest.digest};
 }catch(E e){return Fail(e);}catch(const std::bad_alloc&){return Fail(E::resource_exhausted);}catch(const std::length_error&){return Fail(E::resource_exhausted);}catch(...){return Fail(E::invalid_record);}
}
NativeManagementOperationImage DecodeNativeManagementOperation(const std::vector<byte>& b,u64 budget) noexcept {
 try{Require(b.size()<=budget&&b.size()<=std::numeric_limits<u32>::max(),E::resource_exhausted);
  Require(b.size()>=512,E::invalid_header);const auto* p=b.data();
  Require(std::string_view(reinterpret_cast<const char*>(p),8)=="SBMGO001"&&LoadLittle16(p+8)==1&&LoadLittle16(p+10)==512&&LoadLittle32(p+12)==b.size()&&Zero(p+476,4),E::invalid_header);
  const auto seal=Seal(b);Require(seal.ok(),E::hash_failure);Require(std::equal(seal.digest.begin(),seal.digest.end(),p+480),E::invalid_integrity);
  const auto flags=LoadLittle32(p+472);Require(!(flags&~63u),E::invalid_header);
  O o;for(const auto& [at,member]:operation_ids)o.*member=Get(p+at);std::copy_n(p+272,32,o.normalized_request_sha256.begin());
  for(unsigned i=0;i<4;++i){const auto value=LoadLittle64(p+416+8*i);if(flags&(1u<<i))o.generation_guards[i]=value;else Require(!value,E::invalid_header);}
  o.evidence_required=flags&16;o.metrics_required=flags&32;o.revision=LoadLittle64(p+448);o.state=OS(LoadLittle16(p+464));o.scope=NativeManagementScope(LoadLittle16(p+466));o.initiator_kind=LoadLittle16(p+468);o.restart=NativeManagementRestart(LoadLittle16(p+470));
  const auto count=LoadLittle32(p+456),key=LoadLittle32(p+460);Require(key>=1&&key<=512&&key<=b.size()-512,E::invalid_header);
  std::size_t at=512+key;Require(count<=(b.size()-at)/257,E::invalid_header);o.idempotency_key.assign(reinterpret_cast<const char*>(p+512),key);o.steps.reserve(count);
  for(u32 i=0;i<count;++i){Require(b.size()-at>=256,E::invalid_header);p=b.data()+at;S s;for(const auto& [offset,member]:step_ids)s.*member=Get(p+offset);
   std::copy_n(p+64,32,s.precondition_sha256.begin());std::copy_n(p+96,32,s.postcondition_sha256.begin());s.ordinal=LoadLittle32(p+224);const auto length=LoadLittle32(p+228);
   s.state=SS(LoadLittle16(p+232));s.mutation=NativeManagementMutation(LoadLittle16(p+234));s.compensation=NativeManagementCompensation(LoadLittle16(p+236));s.recovery=NativeManagementRecovery(LoadLittle16(p+238));
   const auto sf=LoadLittle32(p+240);Require(!(sf&~7u)&&Zero(p+244,12)&&length>=1&&length<=512&&length<=b.size()-at-256,E::invalid_header);
   s.evidence_required=sf&1;s.metrics_required=sf&2;s.idempotent=sf&4;at+=256;s.idempotency_key.assign(reinterpret_cast<const char*>(b.data()+at),length);at+=length;o.steps.push_back(std::move(s));}
  Require(at==b.size(),E::invalid_header);Validate(o);const auto digest=core::hash::ComputeSha256Digest(b);Require(digest.ok(),E::hash_failure);
  return {E::none,std::move(o),b,digest.digest};
 }catch(E e){return Fail(e);}catch(const std::bad_alloc&){return Fail(E::resource_exhausted);}catch(const std::length_error&){return Fail(E::resource_exhausted);}catch(...){return Fail(E::invalid_record);}
}
} // namespace scratchbird::storage::database
