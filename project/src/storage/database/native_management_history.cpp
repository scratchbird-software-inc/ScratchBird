// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_management_history.hpp"
#include "disk_device.hpp"
#include "hash_digest_parts.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <mutex>
#include <set>
#include <stdexcept>
#include <tuple>

namespace scratchbird::storage::database {
namespace {
using E=NativeManagementHistoryError;
using namespace core::platform;
void Require(bool ok,E error){if(!ok)throw error;}
bool V7(const Uuid& id){return core::uuid::IsEngineIdentityUuid(id);}
NativeManagementHistory Fail(E error){NativeManagementHistory r;r.error=error;return r;}
auto Hash(const std::vector<byte>& b){const auto hash=core::hash::ComputeSha256Digest(b);Require(hash.ok(),E::hash_failure);return hash.digest;}
disk::NativePageReference Self(const NativeCheckpointRoot& r){const auto& h=r.header;return {h.filespace_uuid,h.page_number,h.page_generation,h.page_size_profile_uuid};}
disk::NativePageReference Self(const NativePublicationPlan& r){const auto& h=r.header;return {h.filespace_uuid,h.page_number,h.page_generation,h.page_size_profile_uuid};}
void PlanError(NativePublicationPlanError e){if(e==NativePublicationPlanError::none)return;
  throw e==NativePublicationPlanError::resource_exhausted?E::resource_exhausted:e==NativePublicationPlanError::hash_failure?E::hash_failure:e==NativePublicationPlanError::cluster_requires_authority?E::cluster_requires_authority:E::plan_failure;}
void BootstrapError(disk::FilespacePageZeroError e){throw e==disk::FilespacePageZeroError::resource_exhausted?E::resource_exhausted:e==disk::FilespacePageZeroError::hash_provider_failure?E::hash_failure:e==disk::FilespacePageZeroError::io_failure?E::io_failure:E::bootstrap_failure;}
const NativeCheckpointRootReference* Head(const NativeCheckpointRoot& cp){const auto it=std::find_if(cp.roots.begin(),cp.roots.end(),[](const auto& r){return r.role==16;});return it==cp.roots.end()?nullptr:&*it;}
bool SameHead(const NativeCheckpointRootReference* a,const NativeCheckpointRootReference* b){return (!a||!b)?a==b:a->page==b->page&&a->object_uuid==b->object_uuid&&a->sha256==b->sha256&&a->page_type==b->page_type;}
struct Checkpoint {NativeCheckpointRoot root;std::vector<byte> bytes;std::array<byte,32> sha{};};
struct Context {
  Uuid database,primary,timeline;
  u64 budget=0,used=0;
  std::vector<disk::NativeFilespaceDevice> devices;
  std::vector<std::unique_lock<std::recursive_mutex>> guards;
  std::map<Uuid,disk::FilespacePageZero> zeros;
  void Charge(u64 n){Require(n<=budget-used,E::resource_exhausted);used+=n;}
  const disk::NativeFilespaceDevice& File(const Uuid& fs,const Uuid& profile){
    const auto it=std::lower_bound(devices.begin(),devices.end(),fs,[](const auto& a,const auto& id){return a.filespace_uuid<id;});
    Require(it!=devices.end()&&it->filespace_uuid==fs&&it->page_size_profile_uuid==profile,E::invalid_request);return *it;
  }
  Checkpoint Read(const disk::NativePageReference& ref,const Uuid& object,const std::array<byte,32>& expected){
    const auto& file=File(ref.filespace_uuid,ref.page_size_profile_uuid);const auto* profile=disk::FindCanonicalFilespacePageProfile(ref.page_size_profile_uuid);
    Require(profile,E::invalid_request);Charge(3*u64{profile->page_size_bytes});
    const disk::FilespaceRootReference root{9,0x300,ref.filespace_uuid,ref.page_number,ref.page_generation,ref.page_size_profile_uuid,object};
    auto r=ReadNativeCheckpointRootFromOpenDevice(*file.device,database,root);
    if(!r.ok())throw r.error==NativeCheckpointError::resource_exhausted?E::resource_exhausted:r.error==NativeCheckpointError::hash_failure?E::hash_failure:r.error==NativeCheckpointError::io_failure?E::io_failure:r.error==NativeCheckpointError::encrypted_requires_crypto_authority?E::encrypted_requires_authority:E::checkpoint_failure;
    Require(r.root->completed&&r.root->timeline_uuid==timeline,E::history_mismatch);Require(!(r.root->flags&4),E::cluster_requires_authority);
    const auto hash=Hash(r.bytes);Require(hash==expected,E::history_mismatch);return {std::move(*r.root),std::move(r.bytes),hash};
  }
  Checkpoint Parent(const Checkpoint& current){const auto& r=current.root;Require(r.predecessor.has_value(),E::history_mismatch);
    auto p=Read(*r.predecessor,r.object_uuid,r.predecessor_sha256);
    Require(p.root.checkpoint_generation<r.checkpoint_generation&&p.root.root_set_generation<r.root_set_generation,E::history_mismatch);return p;
  }
};
void Index(NativeManagementHistory& history){
  using Semantic=std::tuple<Uuid,u16,Uuid,Uuid,Uuid,Uuid,Uuid,std::optional<u64>,std::array<byte,32>>;
  std::map<Semantic,Uuid> semantics;std::map<Uuid,Uuid> steps;
  for(std::size_t i=0;i<history.entries.size();++i){const auto& o=history.entries[i].record;
    const auto old=history.latest.find(o.uuid);
    if(old==history.latest.end())Require(o.revision==1&&o.state==NativeManagementState::created,E::transition_failure);
    else{const auto e=ValidateNativeManagementOperationEvolution(history.entries[old->second].record,o);
      if(e==NativeManagementOperationError::resource_exhausted)throw E::resource_exhausted;
      Require(e==NativeManagementOperationError::none,E::transition_failure);}
    history.latest[o.uuid]=i;
    const auto key=std::make_pair(u16(o.scope),o.idempotency_key);const auto [k,added]=history.idempotency.emplace(key,o.uuid);(void)added;Require(k->second==o.uuid,E::idempotency_conflict);
    const Semantic semantic{o.descriptor_uuid,u16(o.scope),o.target_type_uuid,o.target_uuid,o.initiator_uuid,o.request_context_uuid,o.policy_snapshot_uuid,o.generation_guards[3],o.normalized_request_sha256};
    const auto [s,inserted]=semantics.emplace(semantic,o.uuid);(void)inserted;Require(s->second==o.uuid,E::idempotency_conflict);
    for(const auto& step:o.steps){const auto [entry,fresh]=steps.emplace(step.uuid,o.uuid);(void)fresh;Require(entry->second==o.uuid,E::history_mismatch);}
  }
  for(const auto& [step,owner]:steps){(void)owner;Require(!history.latest.contains(step),E::history_mismatch);}
}
} // namespace
NativeManagementHistory ReadNativeManagementHistoryFromOpenDevices(const Uuid& database,const std::vector<disk::NativeFilespaceDevice>& supplied,const Uuid& primary,u64 budget) noexcept {
  try{
    Require(V7(database)&&V7(primary)&&!supplied.empty(),E::invalid_request);Context c;c.database=database;c.primary=primary;c.budget=budget;c.devices=supplied;
    std::sort(c.devices.begin(),c.devices.end(),[](const auto& a,const auto& b){return a.filespace_uuid<b.filespace_uuid;});std::set<disk::FileDevice*> handles;c.guards.reserve(c.devices.size());
    for(std::size_t i=0;i<c.devices.size();++i){const auto& f=c.devices[i];const auto* profile=disk::FindCanonicalFilespacePageProfile(f.page_size_profile_uuid);
      Require(V7(f.filespace_uuid)&&profile&&f.device&&handles.insert(f.device).second&&(!i||c.devices[i-1].filespace_uuid!=f.filespace_uuid),E::invalid_request);
      c.guards.push_back(f.device->AcquireOperationGuard());Require(f.device->is_open(),E::invalid_request);}
    for(const auto& f:c.devices){const auto* profile=disk::FindCanonicalFilespacePageProfile(f.page_size_profile_uuid);c.Charge(2*u64{profile->page_size_bytes});
      const disk::FilespaceBootstrapBinding binding{database,f.filespace_uuid,f.page_size_profile_uuid};auto zero=disk::ReadFilespacePageZeroFromOpenDevice(*f.device,&binding);if(!zero.ok())BootstrapError(zero.error);
      Require(!(zero.record->bootstrap.flags&disk::FilespaceBootstrapFlag::payload_encrypted),E::encrypted_requires_authority);Require(!(zero.record->bootstrap.flags&disk::FilespaceBootstrapFlag::cluster_authority_required),E::cluster_requires_authority);
      c.zeros.emplace(f.filespace_uuid,std::move(*zero.record));}
    const auto zi=c.zeros.find(primary);Require(zi!=c.zeros.end(),E::invalid_request);const auto& z=zi->second;const auto& file=c.File(primary,z.bootstrap.page_size_profile_uuid);const u64 size=z.bootstrap.page_size_bytes;
    c.Charge(4*size);std::array<std::vector<byte>,2> slots;std::array<disk::FilespaceRootReference,2> roots;
    for(unsigned i=0;i<2;++i){const auto root=std::find_if(z.roots.begin(),z.roots.end(),[&](const auto& r){return r.kind==18+i;});Require(root!=z.roots.end(),E::selection_failure);roots[i]=*root;auto& b=slots[i];b.resize(size);
      const auto io=file.device->ReadAt(root->page_number*size,b.data(),b.size());Require(io.ok()&&io.bytes_transferred==b.size(),E::io_failure);
      const disk::NativeCommonPageHeaderBinding binding{{database,primary,z.bootstrap.page_size_profile_uuid},root->page_number,root->page_generation,0x30e,{}};Require(disk::DecodeNativeCommonPageHeader(b.data(),128,&binding).ok(),E::selection_failure);}
    const auto pair=ClassifyNativeCheckpointSelectionPair(slots[0],slots[1]);if(!pair.ok())throw pair.error==NativeCheckpointSelectionError::hash_failure?E::hash_failure:pair.error==NativeCheckpointSelectionError::resource_exhausted?E::resource_exhausted:E::selection_failure;
    const auto selection=*pair.selection;Require(selection.bootstrap_uuid==z.page_uuid&&selection.object_uuid==roots[0].object_uuid&&selection.object_uuid==roots[1].object_uuid,E::selection_failure);c.timeline=selection.timeline_uuid;
    auto current=c.Read(selection.checkpoint,selection.checkpoint_object_uuid,selection.checkpoint_sha256);
    Require(current.root.checkpoint_generation==selection.checkpoint_generation&&current.root.root_set_generation==selection.root_set_generation,E::history_mismatch);
    if(selection.selection_generation==1)Require(!current.root.predecessor,E::history_mismatch);
    else Require(current.root.predecessor==selection.previous_checkpoint&&current.root.predecessor_sha256==selection.previous_checkpoint_sha256&&current.root.object_uuid==selection.previous_checkpoint_object_uuid,E::history_mismatch);
    if(!current.root.creator_operation_uuid.is_nil())Require(current.root.creator_operation_uuid==selection.publication_uuid,E::history_mismatch);
    NativeManagementHistory result;std::set<Uuid> attempts;
    while(true){
      const auto head=Head(current.root);
      if(current.root.creator_operation_uuid.is_nil()){
        if(!current.root.predecessor){const auto genesis=std::find_if(z.roots.begin(),z.roots.end(),[](const auto& r){return r.kind==9;});Require(genesis!=z.roots.end()&&!head&&current.root.checkpoint_generation==1&&current.root.root_set_generation==1&&Self(current.root)==disk::NativePageReference{genesis->filespace_uuid,genesis->page_number,genesis->page_generation,genesis->page_size_profile_uuid}&&current.root.object_uuid==genesis->object_uuid,E::history_mismatch);break;}
        auto parent=c.Parent(current);Require(SameHead(head,Head(parent.root)),E::history_mismatch);current=std::move(parent);continue;
      }
      Require(head&&head->page_type==0x500&&head->page.filespace_uuid==primary&&head->page.page_size_profile_uuid==z.bootstrap.page_size_profile_uuid&&head->page.page_number<z.total_pages,E::history_mismatch);
      c.Charge(2*size);std::vector<byte> bytes(size);const auto io=file.device->ReadAt(head->page.page_number*size,bytes.data(),bytes.size());Require(io.ok()&&io.bytes_transferred==bytes.size(),E::io_failure);
      auto image=DecodeNativePublicationPlan(bytes);PlanError(image.error);auto p=std::move(*image.plan);
      Require(p.management_extent&&Self(p)==head->page&&p.object_uuid==head->object_uuid&&image.sha256==head->sha256&&p.bootstrap_uuid==z.page_uuid&&p.header.database_uuid==database&&p.timeline_uuid==c.timeline&&p.operation_uuid==current.root.creator_operation_uuid&&p.target_checkpoint==Self(current.root)&&p.target_checkpoint_object_uuid==current.root.object_uuid&&p.reserved_generation==current.root.checkpoint_generation&&p.target_root_set_generation==current.root.root_set_generation,E::history_mismatch);
      Require(attempts.insert(p.operation_uuid).second,E::history_mismatch);
      const auto graph=ComputeNativePublicationTargetGraphDigest(current.bytes);PlanError(graph.error);Require(graph.sha256==p.target_graph_sha256,E::history_mismatch);
      auto base=c.Parent(current);Require(p.base_checkpoint==Self(base.root)&&p.base_checkpoint_object_uuid==base.root.object_uuid&&p.base_checkpoint_sha256==base.sha&&p.base_checkpoint_generation==base.root.checkpoint_generation&&p.base_root_set_generation==base.root.root_set_generation,E::history_mismatch);
      const auto previous=Head(base.root);if(previous)Require(p.previous_plan&&*p.previous_plan==previous->page&&p.previous_plan_object_uuid==previous->object_uuid&&p.previous_plan_sha256==previous->sha256,E::history_mismatch);else Require(!p.previous_plan,E::history_mismatch);
      const auto& r=*p.management_extent;const u64 allowance=u64{r.page_count}*size+4*u64{r.aggregate_bytes}+2*size;c.Charge(allowance);
      auto extent=ReadNativeManagementExtentFromOpenDevice(file,r,database,z.page_uuid,allowance);
      if(!extent.ok())throw extent.error==NativeManagementExtentError::resource_exhausted?E::resource_exhausted:extent.error==NativeManagementExtentError::hash_failure?E::hash_failure:extent.error==NativeManagementExtentError::io_failure?E::io_failure:extent.error==NativeManagementExtentError::cluster_requires_authority?E::cluster_requires_authority:extent.error==NativeManagementExtentError::encrypted_requires_authority?E::encrypted_requires_authority:E::extent_failure;
      PlanError(BindNativePublicationPlanToManagementRecord(p,*extent.record));
      for(const auto& h:extent.page_headers)Require(h.page_uuid!=p.header.page_uuid,E::history_mismatch);
      result.entries.push_back({std::move(p),std::move(*extent.record),std::move(extent.page_headers),image.sha256,current.sha});current=std::move(base);
    }
    std::reverse(result.entries.begin(),result.entries.end());Index(result);result.selection=selection;result.verified_image_bytes=c.used;result.error=E::none;return result;
  }catch(E e){return Fail(e);}catch(const std::bad_alloc&){return Fail(E::resource_exhausted);}catch(const std::length_error&){return Fail(E::resource_exhausted);}catch(...){return Fail(E::io_failure);}
}
} // namespace scratchbird::storage::database
