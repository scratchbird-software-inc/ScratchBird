// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_publication_plan.hpp"
#include "disk_device.hpp"
#include "hash_digest_parts.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace scratchbird::storage::database {
namespace {
using namespace core::platform;
using E=NativePublicationPlanError;
bool Zero(const byte* b,std::size_t n){return std::all_of(b,b+n,[](byte v){return v==0;});}
bool V7(const Uuid& id){return core::uuid::IsEngineIdentityUuid(id);}
void Put(byte* b,const Uuid& id){std::copy(id.bytes.begin(),id.bytes.end(),b);}
Uuid Get(const byte* b){Uuid id;std::copy_n(b,16,id.bytes.begin());return id;}
void PutRef(byte* b,const disk::NativePageReference& r){Put(b,r.filespace_uuid);StoreLittle64(b+16,r.page_number);StoreLittle64(b+24,r.page_generation);Put(b+32,r.page_size_profile_uuid);}
disk::NativePageReference GetRef(const byte* b){return {Get(b),LoadLittle64(b+16),LoadLittle64(b+24),Get(b+32)};}
disk::NativePageReference Self(const disk::NativeCommonPageHeader& h){return {h.filespace_uuid,h.page_number,h.page_generation,h.page_size_profile_uuid};}
bool Ref(const disk::NativePageReference& r){const auto* p=disk::FindCanonicalFilespacePageProfile(r.page_size_profile_uuid);
  return V7(r.filespace_uuid)&&p&&r.page_number&&r.page_generation&&r.page_number<std::numeric_limits<u64>::max()/p->page_size_bytes&&disk::CheckFileDeviceExtent(r.page_number*p->page_size_bytes,p->page_size_bytes).ok();}
NativePublicationPlanImage Fail(E e){NativePublicationPlanImage r;r.error=e;return r;}
E CheckpointError(NativeCheckpointError e){return e==NativeCheckpointError::resource_exhausted?E::resource_exhausted:e==NativeCheckpointError::hash_failure?E::hash_failure:E::invalid_checkpoint;}
auto ImageHash(const std::vector<byte>& b){const std::array<byte,32> zero{};const core::hash::HashDigestSegment parts[]={{b.data(),688},{zero.data(),32},{b.data()+720,b.size()-720}};return core::hash::ComputeSha256DigestParts(parts,3);}
E Validate(const NativePublicationPlan& p){
  const auto& h=p.header;if(!disk::EncodeNativeCommonPageHeader(h).ok()||h.page_type!=0x500||h.flags)return E::invalid_header;
  for(const auto* id:{&p.object_uuid,&p.bootstrap_uuid,&p.timeline_uuid,&p.operation_uuid,&p.security_snapshot_uuid,&p.intent.initiator_uuid,&p.intent.request_context_uuid,&p.intent.policy_snapshot_uuid,&p.base_checkpoint_object_uuid,&p.target_checkpoint_object_uuid})if(!V7(*id))return E::invalid_identity;
  if(p.object_uuid==p.bootstrap_uuid||h.page_uuid==p.object_uuid||h.page_uuid==p.bootstrap_uuid||p.object_uuid==p.base_checkpoint_object_uuid||p.base_checkpoint_object_uuid!=p.target_checkpoint_object_uuid)return E::invalid_identity;
  if(!p.base_checkpoint_generation||!p.base_root_set_generation||p.reserved_generation<=p.base_checkpoint_generation||p.target_root_set_generation<=p.base_root_set_generation||p.target_root_set_generation>p.reserved_generation||!p.catalog_generation||!p.configuration_generation||!p.security_generation||p.intent.initiator_kind<1||p.intent.initiator_kind>8)return E::invalid_family;
  for(const auto* sha:{&p.intent.normalized_request_sha256,&p.reservation_state_sha256,&p.base_checkpoint_sha256,&p.target_graph_sha256})if(Zero(sha->data(),32))return E::invalid_family;
  if(p.previous_plan){if(!V7(p.previous_plan_object_uuid)||p.previous_plan_object_uuid!=p.object_uuid||Zero(p.previous_plan_sha256.data(),32))return E::invalid_reference;}
  else if(!p.previous_plan_object_uuid.is_nil()||!Zero(p.previous_plan_sha256.data(),32))return E::invalid_reference;
  const std::array<disk::NativePageReference,4> refs{Self(h),p.base_checkpoint,p.target_checkpoint,p.previous_plan.value_or(Self(h))};const auto count=p.previous_plan?4u:3u;
  for(unsigned i=0;i<count;++i){if(!Ref(refs[i]))return E::invalid_reference;for(unsigned j=0;j<i;++j)if(refs[i].filespace_uuid==refs[j].filespace_uuid&&(refs[i].page_number==refs[j].page_number||refs[i].page_size_profile_uuid!=refs[j].page_size_profile_uuid))return E::invalid_reference;}
  return E::none;
}
}
NativePublicationPlanImage EncodeNativePublicationPlan(const NativePublicationPlan& p) noexcept {
  try {
    const auto valid=Validate(p);if(valid!=E::none)return Fail(valid);
    const auto h=disk::EncodeNativeCommonPageHeader(p.header);if(!h.ok())return Fail(E::invalid_header);
    std::vector<byte> b(p.header.page_size_bytes,0);std::copy(h.bytes->begin(),h.bytes->end(),b.begin());auto* f=b.data()+128;
    std::copy_n("SBPPM001",8,f);StoreLittle16(f+8,1);StoreLittle16(f+10,640);StoreLittle32(f+12,768);
    Put(f+16,p.object_uuid);Put(f+32,p.bootstrap_uuid);Put(f+48,p.timeline_uuid);Put(f+64,p.operation_uuid);Put(f+80,p.intent.initiator_uuid);Put(f+96,p.intent.request_context_uuid);Put(f+112,p.intent.policy_snapshot_uuid);Put(f+128,p.security_snapshot_uuid);
    std::copy(p.intent.normalized_request_sha256.begin(),p.intent.normalized_request_sha256.end(),f+144);std::copy(p.reservation_state_sha256.begin(),p.reservation_state_sha256.end(),f+176);
    StoreLittle64(f+208,p.reserved_generation);StoreLittle64(f+216,p.base_checkpoint_generation);StoreLittle64(f+224,p.base_root_set_generation);StoreLittle64(f+232,p.target_root_set_generation);
    PutRef(f+240,p.base_checkpoint);Put(f+288,p.base_checkpoint_object_uuid);std::copy(p.base_checkpoint_sha256.begin(),p.base_checkpoint_sha256.end(),f+304);
    PutRef(f+336,p.target_checkpoint);Put(f+384,p.target_checkpoint_object_uuid);std::copy(p.target_graph_sha256.begin(),p.target_graph_sha256.end(),f+400);
    if(p.previous_plan)PutRef(f+432,*p.previous_plan);
    Put(f+480,p.previous_plan_object_uuid);std::copy(p.previous_plan_sha256.begin(),p.previous_plan_sha256.end(),f+496);
    StoreLittle64(f+528,p.catalog_generation);StoreLittle64(f+536,p.configuration_generation);StoreLittle64(f+544,p.security_generation);StoreLittle16(f+552,p.intent.initiator_kind);StoreLittle16(f+554,1);StoreLittle16(f+556,2);
    const auto sha=ImageHash(b);if(!sha.ok())return Fail(E::hash_failure);std::copy(sha.digest.begin(),sha.digest.end(),f+560);
    const auto reference=core::hash::ComputeSha256Digest(b);if(!reference.ok())return Fail(E::hash_failure);
    return {E::none,p,reference.digest,std::move(b)};
  }catch(const std::bad_alloc&){return Fail(E::resource_exhausted);}catch(const std::length_error&){return Fail(E::resource_exhausted);}catch(...){return Fail(E::invalid_family);}
}
NativePublicationPlanImage DecodeNativePublicationPlan(const std::vector<byte>& b) noexcept {
  try {
    if(b.size()<768)return Fail(E::invalid_header);
    const auto h=disk::DecodeNativeCommonPageHeader(b.data(),128);
    if(!h.ok()||h.header->page_type!=0x500||h.header->flags||b.size()!=h.header->page_size_bytes)return Fail(E::invalid_header);
    const auto sha=ImageHash(b);if(!sha.ok())return Fail(E::hash_failure);if(!std::equal(sha.digest.begin(),sha.digest.end(),b.begin()+688))return Fail(E::invalid_integrity);
    const auto* f=b.data()+128;if(std::string_view(reinterpret_cast<const char*>(f),8)!="SBPPM001"||LoadLittle16(f+8)!=1||LoadLittle16(f+10)!=640||LoadLittle32(f+12)!=768||LoadLittle16(f+554)!=1||LoadLittle16(f+556)!=2||!Zero(f+558,2)||!Zero(f+592,48)||!Zero(b.data()+768,b.size()-768))return Fail(E::invalid_family);
    NativePublicationPlan p;p.header=*h.header;p.object_uuid=Get(f+16);p.bootstrap_uuid=Get(f+32);p.timeline_uuid=Get(f+48);p.operation_uuid=Get(f+64);p.intent.initiator_uuid=Get(f+80);p.intent.request_context_uuid=Get(f+96);p.intent.policy_snapshot_uuid=Get(f+112);p.security_snapshot_uuid=Get(f+128);
    std::copy_n(f+144,32,p.intent.normalized_request_sha256.begin());std::copy_n(f+176,32,p.reservation_state_sha256.begin());p.reserved_generation=LoadLittle64(f+208);p.base_checkpoint_generation=LoadLittle64(f+216);p.base_root_set_generation=LoadLittle64(f+224);p.target_root_set_generation=LoadLittle64(f+232);
    p.base_checkpoint=GetRef(f+240);p.base_checkpoint_object_uuid=Get(f+288);std::copy_n(f+304,32,p.base_checkpoint_sha256.begin());p.target_checkpoint=GetRef(f+336);p.target_checkpoint_object_uuid=Get(f+384);std::copy_n(f+400,32,p.target_graph_sha256.begin());
    if(!Zero(f+432,48))p.previous_plan=GetRef(f+432);
    p.previous_plan_object_uuid=Get(f+480);std::copy_n(f+496,32,p.previous_plan_sha256.begin());p.catalog_generation=LoadLittle64(f+528);p.configuration_generation=LoadLittle64(f+536);p.security_generation=LoadLittle64(f+544);p.intent.initiator_kind=LoadLittle16(f+552);
    const auto valid=Validate(p);if(valid!=E::none)return Fail(valid);
    const auto reference=core::hash::ComputeSha256Digest(b);if(!reference.ok())return Fail(E::hash_failure);
    return {E::none,p,reference.digest,b};
  }catch(const std::bad_alloc&){return Fail(E::resource_exhausted);}catch(const std::length_error&){return Fail(E::resource_exhausted);}catch(...){return Fail(E::invalid_family);}
}
NativePublicationGraphDigest ComputeNativePublicationTargetGraphDigest(const std::vector<byte>& b) noexcept {
  try {
    const auto decoded=DecodeNativeCheckpointRoot(b);if(!decoded.ok())return {CheckpointError(decoded.error),{}};
    const auto& cp=*decoded.root;if(!cp.completed||cp.creator_operation_uuid.is_nil()||(cp.flags&4))return {E::invalid_checkpoint,{}};
    const auto role=std::find_if(cp.roots.begin(),cp.roots.end(),[](const auto& r){return r.role==16;});if(role==cp.roots.end())return {E::invalid_checkpoint,{}};
    const auto at=512+112*static_cast<std::size_t>(role-cp.roots.begin())+72;const std::array<byte,64> zero{};const std::array<byte,8> domain{'S','B','P','P','G','R','0','1'};
    const core::hash::HashDigestSegment parts[]={{domain.data(),8},{b.data(),336},{zero.data(),64},{b.data()+400,at-400},{zero.data(),32},{b.data()+at+32,b.size()-at-32}};
    const auto sha=core::hash::ComputeSha256DigestParts(parts,6);return sha.ok()?NativePublicationGraphDigest{E::none,sha.digest}:NativePublicationGraphDigest{E::hash_failure,{}};
  }catch(const std::bad_alloc&){return {E::resource_exhausted,{}};}catch(const std::length_error&){return {E::resource_exhausted,{}};}catch(...){return {E::invalid_checkpoint,{}};}
}
NativePublicationPlanError BindNativePublicationPlanToLease(const NativePublicationPlan& p,const NativePublicationLease& lease,const std::vector<byte>& b) noexcept {
  try {
    const auto image=EncodeNativePublicationPlan(p);if(!image.ok())return image.error;
    const auto& s=lease.snapshot();const auto& w=s.watermark;
    if(!w.intent||*w.intent!=p.intent||w.operation_uuid!=p.operation_uuid||w.header.database_uuid!=p.header.database_uuid||w.bootstrap_uuid!=p.bootstrap_uuid||w.timeline_uuid!=p.timeline_uuid||s.state_sha256!=p.reservation_state_sha256||w.watermark!=p.reserved_generation||w.base_checkpoint!=p.base_checkpoint||w.base_checkpoint_object_uuid!=p.base_checkpoint_object_uuid||w.base_checkpoint_sha256!=p.base_checkpoint_sha256||w.base_checkpoint_generation!=p.base_checkpoint_generation||w.base_root_set_generation!=p.base_root_set_generation)return E::binding_mismatch;
    const auto decoded=DecodeNativeCheckpointRoot(b);if(!decoded.ok())return CheckpointError(decoded.error);const auto& cp=*decoded.root;
    if(cp.header.database_uuid!=p.header.database_uuid||Self(cp.header)!=p.target_checkpoint||cp.object_uuid!=p.target_checkpoint_object_uuid||cp.creator_operation_uuid!=p.operation_uuid||cp.timeline_uuid!=p.timeline_uuid||cp.checkpoint_generation!=p.reserved_generation||cp.root_set_generation!=p.target_root_set_generation||!cp.predecessor||*cp.predecessor!=p.base_checkpoint||cp.predecessor_sha256!=p.base_checkpoint_sha256)return E::binding_mismatch;
    const auto role=std::find_if(cp.roots.begin(),cp.roots.end(),[](const auto& r){return r.role==16;});if(role==cp.roots.end()||role->page!=Self(p.header)||role->object_uuid!=p.object_uuid||role->sha256!=image.sha256)return E::binding_mismatch;
    const auto graph=ComputeNativePublicationTargetGraphDigest(b);if(!graph.ok())return graph.error;return graph.sha256==p.target_graph_sha256?E::none:E::binding_mismatch;
  }catch(const std::bad_alloc&){return E::resource_exhausted;}catch(const std::length_error&){return E::resource_exhausted;}catch(...){return E::binding_mismatch;}
}
} // namespace scratchbird::storage::database
