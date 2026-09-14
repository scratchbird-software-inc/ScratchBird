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
  if(p.control_bundle&&!p.management_extent)return E::invalid_family;
  for(const auto* id:{&p.object_uuid,&p.bootstrap_uuid,&p.timeline_uuid,&p.operation_uuid,&p.intent.initiator_uuid,&p.intent.request_context_uuid,&p.intent.policy_snapshot_uuid,&p.base_checkpoint_object_uuid,&p.target_checkpoint_object_uuid})if(!V7(*id))return E::invalid_identity;
  if(!p.management_extent){if(!V7(p.security_snapshot_uuid))return E::invalid_identity;if(p.generation_guard_flags!=7||!p.catalog_generation||!p.configuration_generation||!p.security_generation)return E::invalid_family;}
  else {if(!p.security_snapshot_uuid.is_nil()&&!V7(p.security_snapshot_uuid))return E::invalid_identity;
    if(p.generation_guard_flags&~7u)return E::invalid_family;
    const std::array<u64,3> guards{p.catalog_generation,p.configuration_generation,p.security_generation};for(unsigned i=0;i<3;++i)if(!(p.generation_guard_flags&(1u<<i))&&guards[i])return E::invalid_family;}
  if(p.object_uuid==p.bootstrap_uuid||h.page_uuid==p.object_uuid||h.page_uuid==p.bootstrap_uuid||p.object_uuid==p.base_checkpoint_object_uuid||p.base_checkpoint_object_uuid!=p.target_checkpoint_object_uuid)return E::invalid_identity;
  if(!p.base_checkpoint_generation||!p.base_root_set_generation||p.reserved_generation<=p.base_checkpoint_generation||p.target_root_set_generation<=p.base_root_set_generation||p.target_root_set_generation>p.reserved_generation||p.intent.initiator_kind<1||p.intent.initiator_kind>8)return E::invalid_family;
  for(const auto* sha:{&p.intent.normalized_request_sha256,&p.reservation_state_sha256,&p.base_checkpoint_sha256,&p.target_graph_sha256})if(Zero(sha->data(),32))return E::invalid_family;
  if(p.previous_plan){if(!V7(p.previous_plan_object_uuid)||p.previous_plan_object_uuid!=p.object_uuid||Zero(p.previous_plan_sha256.data(),32))return E::invalid_reference;}
  else if(!p.previous_plan_object_uuid.is_nil()||!Zero(p.previous_plan_sha256.data(),32))return E::invalid_reference;
  const std::array<disk::NativePageReference,4> refs{Self(h),p.base_checkpoint,p.target_checkpoint,p.previous_plan.value_or(Self(h))};const auto count=p.previous_plan?4u:3u;
  for(unsigned i=0;i<count;++i){if(!Ref(refs[i]))return E::invalid_reference;for(unsigned j=0;j<i;++j)if(refs[i].filespace_uuid==refs[j].filespace_uuid&&(refs[i].page_number==refs[j].page_number||refs[i].page_size_profile_uuid!=refs[j].page_size_profile_uuid))return E::invalid_reference;}
  if(p.management_extent){const auto& r=*p.management_extent;const auto valid=ValidateNativeManagementExtentRoot(r,h.database_uuid,p.bootstrap_uuid,std::numeric_limits<u64>::max());
    if(valid==NativeManagementExtentError::resource_exhausted)return E::resource_exhausted;
    if(valid!=NativeManagementExtentError::none)return E::invalid_reference;
    if(r.first.filespace_uuid!=h.filespace_uuid||r.first.page_size_profile_uuid!=h.page_size_profile_uuid||r.first.page_generation!=p.reserved_generation||h.page_generation!=p.reserved_generation)return E::binding_mismatch;
    if(r.object_uuid==p.object_uuid||r.object_uuid==p.base_checkpoint_object_uuid||r.object_uuid==h.page_uuid||r.object_uuid==p.operation_uuid)return E::invalid_identity;
    for(unsigned i=0;i<count;++i)if(refs[i].filespace_uuid==h.filespace_uuid&&refs[i].page_number>=r.first.page_number&&refs[i].page_number-r.first.page_number<r.page_count)return E::invalid_reference;
  }
  if(p.control_bundle){const auto& r=*p.control_bundle;const auto valid=ValidateNativeManagementControlBundleRoot(r,h.database_uuid,p.bootstrap_uuid,std::numeric_limits<u64>::max());
    if(valid==NativeManagementControlBundleError::resource_exhausted)return E::resource_exhausted;
    if(valid!=NativeManagementControlBundleError::none)return E::invalid_reference;
    if(r.operation_uuid!=p.operation_uuid||r.first.filespace_uuid!=h.filespace_uuid||r.first.page_size_profile_uuid!=h.page_size_profile_uuid||r.first.page_generation!=p.reserved_generation)return E::binding_mismatch;
    for(const auto* id:{&p.object_uuid,&p.base_checkpoint_object_uuid,&p.management_extent->object_uuid,&p.management_extent->operation_uuid,&h.page_uuid})if(r.object_uuid==*id)return E::invalid_identity;
    for(unsigned i=0;i<count;++i)if(refs[i].filespace_uuid==h.filespace_uuid&&refs[i].page_number>=r.first.page_number&&refs[i].page_number-r.first.page_number<r.page_count)return E::invalid_reference;
    const auto& e=*p.management_extent;const bool overlap=r.first.page_number<=e.first.page_number?e.first.page_number-r.first.page_number<r.page_count:r.first.page_number-e.first.page_number<e.page_count;if(overlap)return E::invalid_reference;
  }
  return E::none;
}
}
NativePublicationPlanImage EncodeNativePublicationPlan(const NativePublicationPlan& p) noexcept {
  try {
    const auto valid=Validate(p);if(valid!=E::none)return Fail(valid);
    const auto h=disk::EncodeNativeCommonPageHeader(p.header);if(!h.ok())return Fail(E::invalid_header);
    std::vector<byte> b(p.header.page_size_bytes,0);std::copy(h.bytes->begin(),h.bytes->end(),b.begin());auto* f=b.data()+128;
    const bool extent=p.management_extent.has_value(),bundle=p.control_bundle.has_value();std::copy_n(bundle?"SBPPM003":extent?"SBPPM002":"SBPPM001",8,f);StoreLittle16(f+8,bundle?3:extent?2:1);StoreLittle16(f+10,bundle?1024:extent?896:640);StoreLittle32(f+12,bundle?1152:extent?1024:768);
    Put(f+16,p.object_uuid);Put(f+32,p.bootstrap_uuid);Put(f+48,p.timeline_uuid);Put(f+64,p.operation_uuid);Put(f+80,p.intent.initiator_uuid);Put(f+96,p.intent.request_context_uuid);Put(f+112,p.intent.policy_snapshot_uuid);Put(f+128,p.security_snapshot_uuid);
    std::copy(p.intent.normalized_request_sha256.begin(),p.intent.normalized_request_sha256.end(),f+144);std::copy(p.reservation_state_sha256.begin(),p.reservation_state_sha256.end(),f+176);
    StoreLittle64(f+208,p.reserved_generation);StoreLittle64(f+216,p.base_checkpoint_generation);StoreLittle64(f+224,p.base_root_set_generation);StoreLittle64(f+232,p.target_root_set_generation);
    PutRef(f+240,p.base_checkpoint);Put(f+288,p.base_checkpoint_object_uuid);std::copy(p.base_checkpoint_sha256.begin(),p.base_checkpoint_sha256.end(),f+304);
    PutRef(f+336,p.target_checkpoint);Put(f+384,p.target_checkpoint_object_uuid);std::copy(p.target_graph_sha256.begin(),p.target_graph_sha256.end(),f+400);
    if(p.previous_plan)PutRef(f+432,*p.previous_plan);
    Put(f+480,p.previous_plan_object_uuid);std::copy(p.previous_plan_sha256.begin(),p.previous_plan_sha256.end(),f+496);
    StoreLittle64(f+528,p.catalog_generation);StoreLittle64(f+536,p.configuration_generation);StoreLittle64(f+544,p.security_generation);StoreLittle16(f+552,p.intent.initiator_kind);StoreLittle16(f+554,1);StoreLittle16(f+556,2);
    if(extent){const auto& r=*p.management_extent;StoreLittle32(f+592,p.generation_guard_flags);PutRef(f+608,r.first);Put(f+656,r.object_uuid);Put(f+672,r.operation_uuid);StoreLittle64(f+688,r.revision);StoreLittle32(f+696,r.aggregate_bytes);StoreLittle32(f+700,r.page_count);std::copy(r.aggregate_sha256.begin(),r.aggregate_sha256.end(),f+704);std::copy(r.first_page_sha256.begin(),r.first_page_sha256.end(),f+736);}
    if(bundle){const auto& r=*p.control_bundle;PutRef(f+768,r.first);Put(f+816,r.object_uuid);StoreLittle64(f+832,r.map_count);StoreLittle64(f+840,r.page_count);std::copy(r.aggregate_sha256.begin(),r.aggregate_sha256.end(),f+848);std::copy(r.first_page_sha256.begin(),r.first_page_sha256.end(),f+880);}
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
    const auto* f=b.data()+128;const bool bundle=std::string_view(reinterpret_cast<const char*>(f),8)=="SBPPM003",extent=bundle||std::string_view(reinterpret_cast<const char*>(f),8)=="SBPPM002";
    if((!extent&&std::string_view(reinterpret_cast<const char*>(f),8)!="SBPPM001")||LoadLittle16(f+8)!=(bundle?3:extent?2:1)||LoadLittle16(f+10)!=(bundle?1024:extent?896:640)||LoadLittle32(f+12)!=(bundle?1152:extent?1024:768)||LoadLittle16(f+554)!=1||LoadLittle16(f+556)!=2||!Zero(f+558,2))return Fail(E::invalid_family);
    if(bundle){if(!Zero(f+596,12)||!Zero(f+912,112)||!Zero(b.data()+1152,b.size()-1152))return Fail(E::invalid_family);}
    else if(extent){if(!Zero(f+596,12)||!Zero(f+768,128)||!Zero(b.data()+1024,b.size()-1024))return Fail(E::invalid_family);}
    else if(!Zero(f+592,48)||!Zero(b.data()+768,b.size()-768))return Fail(E::invalid_family);
    NativePublicationPlan p;p.header=*h.header;p.object_uuid=Get(f+16);p.bootstrap_uuid=Get(f+32);p.timeline_uuid=Get(f+48);p.operation_uuid=Get(f+64);p.intent.initiator_uuid=Get(f+80);p.intent.request_context_uuid=Get(f+96);p.intent.policy_snapshot_uuid=Get(f+112);p.security_snapshot_uuid=Get(f+128);
    std::copy_n(f+144,32,p.intent.normalized_request_sha256.begin());std::copy_n(f+176,32,p.reservation_state_sha256.begin());p.reserved_generation=LoadLittle64(f+208);p.base_checkpoint_generation=LoadLittle64(f+216);p.base_root_set_generation=LoadLittle64(f+224);p.target_root_set_generation=LoadLittle64(f+232);
    p.base_checkpoint=GetRef(f+240);p.base_checkpoint_object_uuid=Get(f+288);std::copy_n(f+304,32,p.base_checkpoint_sha256.begin());p.target_checkpoint=GetRef(f+336);p.target_checkpoint_object_uuid=Get(f+384);std::copy_n(f+400,32,p.target_graph_sha256.begin());
    if(!Zero(f+432,48))p.previous_plan=GetRef(f+432);
    p.previous_plan_object_uuid=Get(f+480);std::copy_n(f+496,32,p.previous_plan_sha256.begin());p.catalog_generation=LoadLittle64(f+528);p.configuration_generation=LoadLittle64(f+536);p.security_generation=LoadLittle64(f+544);p.intent.initiator_kind=LoadLittle16(f+552);
    if(extent){p.generation_guard_flags=LoadLittle32(f+592);NativeManagementExtentRoot r;r.first=GetRef(f+608);r.object_uuid=Get(f+656);r.operation_uuid=Get(f+672);r.revision=LoadLittle64(f+688);r.aggregate_bytes=LoadLittle32(f+696);r.page_count=LoadLittle32(f+700);std::copy_n(f+704,32,r.aggregate_sha256.begin());std::copy_n(f+736,32,r.first_page_sha256.begin());p.management_extent=r;}
    if(bundle){NativeManagementControlBundleRoot r;r.first=GetRef(f+768);r.object_uuid=Get(f+816);r.operation_uuid=p.operation_uuid;r.map_count=LoadLittle64(f+832);r.page_count=LoadLittle64(f+840);std::copy_n(f+848,32,r.aggregate_sha256.begin());std::copy_n(f+880,32,r.first_page_sha256.begin());p.control_bundle=r;}
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
    const auto& origin=w.publication_plan?w.publication_plan->reservation_state_sha256:s.state_sha256;
    if(w.publication_plan&&(w.publication_plan->page!=Self(p.header)||w.publication_plan->object_uuid!=p.object_uuid||w.publication_plan->sha256!=image.sha256))return E::binding_mismatch;
    if(!w.intent||*w.intent!=p.intent||w.operation_uuid!=p.operation_uuid||w.header.database_uuid!=p.header.database_uuid||w.bootstrap_uuid!=p.bootstrap_uuid||w.timeline_uuid!=p.timeline_uuid||origin!=p.reservation_state_sha256||w.watermark!=p.reserved_generation||w.base_checkpoint!=p.base_checkpoint||w.base_checkpoint_object_uuid!=p.base_checkpoint_object_uuid||w.base_checkpoint_sha256!=p.base_checkpoint_sha256||w.base_checkpoint_generation!=p.base_checkpoint_generation||w.base_root_set_generation!=p.base_root_set_generation)return E::binding_mismatch;
    const auto decoded=DecodeNativeCheckpointRoot(b);if(!decoded.ok())return CheckpointError(decoded.error);const auto& cp=*decoded.root;
    if(cp.header.database_uuid!=p.header.database_uuid||Self(cp.header)!=p.target_checkpoint||cp.object_uuid!=p.target_checkpoint_object_uuid||cp.creator_operation_uuid!=p.operation_uuid||cp.timeline_uuid!=p.timeline_uuid||cp.checkpoint_generation!=p.reserved_generation||cp.root_set_generation!=p.target_root_set_generation||!cp.predecessor||*cp.predecessor!=p.base_checkpoint||cp.predecessor_sha256!=p.base_checkpoint_sha256)return E::binding_mismatch;
    const auto role=std::find_if(cp.roots.begin(),cp.roots.end(),[](const auto& r){return r.role==16;});if(role==cp.roots.end()||role->page!=Self(p.header)||role->object_uuid!=p.object_uuid||role->sha256!=image.sha256)return E::binding_mismatch;
    const auto graph=ComputeNativePublicationTargetGraphDigest(b);if(!graph.ok())return graph.error;return graph.sha256==p.target_graph_sha256?E::none:E::binding_mismatch;
  }catch(const std::bad_alloc&){return E::resource_exhausted;}catch(const std::length_error&){return E::resource_exhausted;}catch(...){return E::binding_mismatch;}
}
namespace {
E RecordFields(const NativePublicationPlan& p,const NativeManagementOperation& o){
  if(!p.management_extent||o.database_uuid!=p.header.database_uuid||o.bootstrap_uuid!=p.bootstrap_uuid||o.uuid!=p.management_extent->operation_uuid||o.revision!=p.management_extent->revision)return E::binding_mismatch;
  if(o.scope==NativeManagementScope::cluster)return E::cluster_requires_authority;
  if(o.initiator_uuid!=p.intent.initiator_uuid||o.initiator_kind!=p.intent.initiator_kind||o.request_context_uuid!=p.intent.request_context_uuid||o.policy_snapshot_uuid!=p.intent.policy_snapshot_uuid||o.normalized_request_sha256!=p.intent.normalized_request_sha256||o.security_snapshot_uuid!=p.security_snapshot_uuid)return E::binding_mismatch;
  const std::array<u64,3> values{p.catalog_generation,p.configuration_generation,p.security_generation};
  for(unsigned i=0;i<3;++i)if(o.generation_guards[i].has_value()!=bool(p.generation_guard_flags&(1u<<i))||o.generation_guards[i].value_or(0)!=values[i])return E::binding_mismatch;
  return E::none;
}
}
NativePublicationPlanError BindNativePublicationPlanToManagementExtent(const NativePublicationPlan& p,const std::vector<std::vector<byte>>& pages,u64 budget) noexcept {
  try{
    const auto valid=Validate(p);if(valid!=E::none)return valid;
    if(!p.management_extent)return pages.empty()?E::none:E::binding_mismatch;
    const auto decoded=DecodeNativeManagementExtent(pages,*p.management_extent,p.header.database_uuid,p.bootstrap_uuid,budget);
    if(!decoded.ok())return decoded.error==NativeManagementExtentError::hash_failure?E::hash_failure:decoded.error==NativeManagementExtentError::resource_exhausted?E::resource_exhausted:E::binding_mismatch;
    const auto fields=RecordFields(p,*decoded.record);if(fields!=E::none)return fields;
    for(const auto& b:pages){const auto h=disk::DecodeNativeCommonPageHeader(b.data(),128);if(!h.ok()||h.header->page_uuid==p.header.page_uuid)return E::invalid_identity;}
    return E::none;
  }catch(const std::bad_alloc&){return E::resource_exhausted;}catch(const std::length_error&){return E::resource_exhausted;}catch(...){return E::binding_mismatch;}
}
NativePublicationPlanError BindNativePublicationPlanToManagementRecord(const NativePublicationPlan& p,const NativeManagementOperation& o) noexcept {
  try{
    const auto valid=Validate(p);if(valid!=E::none)return valid;
    const auto fields=RecordFields(p,o);if(fields!=E::none)return fields;
    const auto encoded=EncodeNativeManagementOperation(o,p.management_extent->aggregate_bytes);
    if(!encoded.ok())return encoded.error==NativeManagementOperationError::resource_exhausted?E::resource_exhausted:encoded.error==NativeManagementOperationError::hash_failure?E::hash_failure:E::binding_mismatch;
    return encoded.bytes.size()==p.management_extent->aggregate_bytes&&encoded.sha256==p.management_extent->aggregate_sha256?E::none:E::binding_mismatch;
  }catch(const std::bad_alloc&){return E::resource_exhausted;}catch(const std::length_error&){return E::resource_exhausted;}catch(...){return E::binding_mismatch;}
}
} // namespace scratchbird::storage::database
