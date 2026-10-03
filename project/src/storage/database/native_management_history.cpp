// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_management_history.hpp"
#include "native_filespace_directory.hpp"
#include "disk_device.hpp"
#include "hash_digest_parts.hpp"
#include "uuid.hpp"
#include "native_metadata_decode_scratch.hpp"
#include "native_management_control_bundle_backing.hpp"
#include "native_decoded_storage_ranges.hpp"
#include <algorithm>
#include <mutex>
#include <set>
#include <stdexcept>
#include <tuple>

namespace scratchbird::storage::database {
namespace {
using E=NativeManagementHistoryError;
using Image=std::span<const byte>;
using Scratch=detail::NativeMetadataScratch;
using namespace core::platform;
void Require(bool ok,E error){if(!ok)throw error;}
bool V7(const Uuid& id){return core::uuid::IsEngineIdentityUuid(id);}
bool SameBootstrap(const disk::FilespaceBootstrap& a,const disk::FilespaceBootstrap& b){
  return std::tie(a.database_uuid,a.filespace_uuid,a.page_size_profile_uuid,a.checksum_profile_uuid,a.encryption_profile_uuid,
    a.page_size_bytes,a.durable_format_generation,a.flags,a.filespace_role,a.lifecycle_state)==
    std::tie(b.database_uuid,b.filespace_uuid,b.page_size_profile_uuid,b.checksum_profile_uuid,b.encryption_profile_uuid,
    b.page_size_bytes,b.durable_format_generation,b.flags,b.filespace_role,b.lifecycle_state);
}
NativeManagementHistory Fail(E error){NativeManagementHistory r;r.error=error;return r;}
NativeManagementHistoryView FailView(E error){NativeManagementHistoryView r;r.error=error;return r;}
auto Hash(Image b){const auto hash=core::hash::ComputeSha256DigestNative(b.data(),b.size());Require(hash.ok(),E::hash_failure);return hash.digest;}
disk::NativePageReference Self(const NativeCheckpointRootView& r){const auto& h=r.header;return {h.filespace_uuid,h.page_number,h.page_generation,h.page_size_profile_uuid};}
disk::NativePageReference Self(const NativePublicationPlan& r){const auto& h=r.header;return {h.filespace_uuid,h.page_number,h.page_generation,h.page_size_profile_uuid};}
void PlanError(NativePublicationPlanError e){if(e==NativePublicationPlanError::none)return;
  throw e==NativePublicationPlanError::resource_exhausted?E::resource_exhausted:e==NativePublicationPlanError::hash_failure?E::hash_failure:e==NativePublicationPlanError::cluster_requires_authority?E::cluster_requires_authority:E::plan_failure;}
void BootstrapError(disk::FilespacePageZeroError e){throw e==disk::FilespacePageZeroError::resource_exhausted?E::resource_exhausted:e==disk::FilespacePageZeroError::hash_provider_failure?E::hash_failure:e==disk::FilespacePageZeroError::io_failure?E::io_failure:E::bootstrap_failure;}
const NativeCheckpointRootReference* Head(const NativeCheckpointRootView& cp){const auto it=std::find_if(cp.roots.begin(),cp.roots.end(),[](const auto& r){return r.role==16;});return it==cp.roots.end()?nullptr:&*it;}
bool SameHead(const NativeCheckpointRootReference* a,const NativeCheckpointRootReference* b){return (!a||!b)?a==b:a->page==b->page&&a->object_uuid==b->object_uuid&&a->sha256==b->sha256&&a->page_type==b->page_type;}
struct Checkpoint {NativeCheckpointRootView root;Image bytes;std::array<byte,32> sha{};};
struct Context {
  Uuid database,primary,timeline;
  u64 budget=0,used=0;
  Scratch& scratch;
  NativeManagementHistoryDeviceRead* receipt;
  std::pmr::vector<disk::NativeFilespaceDevice> devices;
  std::pmr::vector<std::unique_lock<std::recursive_mutex>> guards;
  std::pmr::map<Uuid,disk::FilespacePageZeroView> zeros;
  std::pmr::map<Uuid,Image> zero_images,observed_zero_images;
  std::pmr::map<Uuid,u64> physical_sizes;
  std::pmr::map<disk::FileDevice*,disk::FileDevice::ReadLatencyBatch*> observations;
  bool historical=false;
  Context(Scratch& s,NativeManagementHistoryDeviceRead* r):scratch(s),receipt(r),
    devices(s.resource),guards(s.resource),zeros(s.resource),zero_images(s.resource),
    observed_zero_images(s.resource),physical_sizes(s.resource),observations(s.resource){}
  void Charge(u64 n){Require(n<=budget-used,E::resource_exhausted);used+=n;}
  const disk::NativeFilespaceDevice& File(const Uuid& fs,const Uuid& profile){
    const auto it=std::lower_bound(devices.begin(),devices.end(),fs,[](const auto& a,const auto& id){return a.filespace_uuid<id;});
    Require(it!=devices.end()&&it->filespace_uuid==fs&&it->page_size_profile_uuid==profile,E::invalid_request);return *it;
  }
  auto ReadAt(disk::FileDevice& device,u64 offset,void* data,std::size_t bytes){
    const auto found=observations.find(&device);
    auto io=found==observations.end()?device.ReadAt(offset,data,bytes):found->second->ReadAt(offset,data,bytes);
    if(receipt){receipt->io_status=io.status;receipt->io_diagnostic=std::move(io.diagnostic);
      Require(io.bytes_transferred<=std::numeric_limits<u64>::max()-receipt->physical_bytes_read,E::history_mismatch);
      receipt->physical_bytes_read+=io.bytes_transferred;}
    return io;
  }
  auto Size(disk::FileDevice& device){
    auto io=device.Size();if(receipt){receipt->io_status=io.status;receipt->io_diagnostic=std::move(io.diagnostic);}return io;
  }
  auto PageZero(const disk::NativeFilespaceDevice& file,Image& raw){
    const disk::FilespaceBootstrapBinding binding{database,file.filespace_uuid,file.page_size_profile_uuid};
    return detail::ReadNativeMetadataPageZero(binding,scratch,
      [&](u64 offset,void* bytes,std::size_t n){return ReadAt(*file.device,offset,bytes,n);},
      [&]{return Size(*file.device);},raw);
  }
  void HistoricalContexts(){if(!zero_images.empty())return;
    // Full canonical page-zero decoding already excluded every noncanonical
    // byte. Retain the exact observed image, not an owning re-encoding.
    for(const auto& [id,z]:zeros){Charge(z.bootstrap.page_size_bytes);
      zero_images.emplace(id,observed_zero_images.at(id));}}
  void Bound(const NativeCheckpointRootView& cp){
    const auto bound=[&](const disk::NativePageReference& r){const auto at=zeros.find(r.filespace_uuid);
      if(at!=zeros.end())Require(r.page_size_profile_uuid==at->second.bootstrap.page_size_profile_uuid&&r.page_number<at->second.total_pages,E::history_mismatch);};
    bound(Self(cp));if(cp.predecessor)bound(*cp.predecessor);for(const auto& root:cp.roots)bound(root.page);
  }
  Checkpoint Read(const disk::NativePageReference& ref,const Uuid& object,const std::array<byte,32>& expected){
    const auto& file=File(ref.filespace_uuid,ref.page_size_profile_uuid);const auto* profile=disk::FindCanonicalFilespacePageProfile(ref.page_size_profile_uuid);
    Require(profile,E::invalid_request);Charge(3*u64{profile->page_size_bytes});
    std::optional<disk::FilespacePageZeroView> actual;
    if(historical)Require(ref.page_number<zeros.at(ref.filespace_uuid).total_pages,E::history_mismatch);
    else {
      Image bytes;const auto zero=PageZero(file,bytes);
      if(!zero.ok())throw zero.error==disk::FilespacePageZeroError::resource_exhausted?E::resource_exhausted:
        zero.error==disk::FilespacePageZeroError::hash_provider_failure?E::hash_failure:
        zero.error==disk::FilespacePageZeroError::io_failure?E::io_failure:E::checkpoint_failure;
      actual=zero.record;
      Require(actual->bootstrap.filespace_role<=4&&ref.page_number<actual->total_pages,E::checkpoint_failure);
      Require(!(actual->bootstrap.flags&disk::FilespaceBootstrapFlag::payload_encrypted),E::encrypted_requires_authority);
    }
    auto raw=scratch.Array<byte>(profile->page_size_bytes);
    const auto io=ReadAt(*file.device,ref.page_number*u64{profile->page_size_bytes},raw.data(),raw.size());
    Require(io.ok()&&io.bytes_transferred==raw.size(),E::io_failure);
    const auto h=disk::DecodeNativeCommonPageHeader(raw.data(),128);
    Require(h.ok(),E::checkpoint_failure);Require(!(h.header->flags&1u),E::encrypted_requires_authority);
    auto decoded=scratch.Checkpoint(raw);
    if(!decoded.ok())throw decoded.error==NativeCheckpointError::resource_exhausted?E::resource_exhausted:
      decoded.error==NativeCheckpointError::hash_failure?E::hash_failure:E::checkpoint_failure;
    const auto& cp=*decoded.root;
    Require(cp.header.database_uuid==database&&Self(cp)==ref&&cp.object_uuid==object,
      historical?E::history_mismatch:E::checkpoint_failure);
    if(actual){
      if(cp.predecessor&&cp.predecessor->filespace_uuid==ref.filespace_uuid)
        Require(cp.predecessor->page_number<actual->total_pages,E::checkpoint_failure);
      for(const auto& root:cp.roots)if(root.page.filespace_uuid==ref.filespace_uuid)
        Require(root.page.page_number<actual->total_pages,E::checkpoint_failure);
    }
    Require(cp.completed&&cp.timeline_uuid==timeline,E::history_mismatch);
    Require(!(cp.flags&4),E::cluster_requires_authority);Bound(cp);
    const auto hash=Hash(raw);Require(hash==expected,E::history_mismatch);return {cp,raw,hash};
  }
  Checkpoint Parent(const Checkpoint& current){const auto& r=current.root;Require(r.predecessor.has_value(),E::history_mismatch);
    auto p=Read(*r.predecessor,r.object_uuid,r.predecessor_sha256);
    Require(p.root.checkpoint_generation<r.checkpoint_generation&&p.root.root_set_generation<r.root_set_generation,E::history_mismatch);return p;
  }
  NativeManagementExtentViewRead Extent(const disk::NativeFilespaceDevice& file,const NativeManagementExtentRoot& r,const Uuid& bootstrap,u64 allowance){
    const auto valid=ValidateNativeManagementExtentRoot(r,database,bootstrap,allowance);
    if(valid!=NativeManagementExtentError::none)return {valid,{},{},{}};
    const auto& z=zeros.at(file.filespace_uuid);
    Require(r.first.filespace_uuid==file.filespace_uuid&&r.first.page_size_profile_uuid==file.page_size_profile_uuid&&
      r.first.page_number<z.total_pages&&r.page_count<=z.total_pages-r.first.page_number,E::history_mismatch);
    const auto bound=[&](const auto& zero,E error){
      Require(zero.page_uuid==bootstrap&&
        std::any_of(zero.roots.begin(),zero.roots.end(),[](const auto& root){return root.kind==20;})&&
        std::any_of(zero.roots.begin(),zero.roots.end(),[](const auto& root){return root.kind==21;}),error);
      Require(r.first.page_number<zero.total_pages&&r.page_count<=zero.total_pages-r.first.page_number,error);
      for(const auto& root:zero.roots)if(root.filespace_uuid==file.filespace_uuid)
        Require(root.page_number<r.first.page_number||root.page_number-r.first.page_number>=r.page_count,error);
    };
    Require(z.page_uuid==bootstrap&&std::any_of(z.roots.begin(),z.roots.end(),[](const auto& root){return root.kind==20;})&&
      std::any_of(z.roots.begin(),z.roots.end(),[](const auto& root){return root.kind==21;}),E::bootstrap_failure);
    for(const auto& root:z.roots)if(root.filespace_uuid==file.filespace_uuid)
      Require(root.page_number<r.first.page_number||root.page_number-r.first.page_number>=r.page_count,E::history_mismatch);
    if(!historical){
      Image raw;const auto actual=PageZero(file,raw);
      if(!actual.ok())throw actual.error==disk::FilespacePageZeroError::resource_exhausted?E::resource_exhausted:
        actual.error==disk::FilespacePageZeroError::hash_provider_failure?E::hash_failure:
        actual.error==disk::FilespacePageZeroError::io_failure?E::io_failure:E::extent_failure;
      Require(actual.record->page_uuid==bootstrap,E::extent_failure);
      Require(!(actual.record->bootstrap.flags&disk::FilespaceBootstrapFlag::payload_encrypted),E::encrypted_requires_authority);
      Require(!(actual.record->bootstrap.flags&disk::FilespaceBootstrapFlag::cluster_authority_required),E::cluster_requires_authority);
      bound(*actual.record,E::extent_failure);
    }
    auto pages=scratch.Array<Image>(r.page_count);
    for(u64 n=0;n<r.page_count;++n){auto raw=scratch.Array<byte>(z.bootstrap.page_size_bytes);pages[n]=raw;
      const auto io=ReadAt(*file.device,(r.first.page_number+n)*u64{z.bootstrap.page_size_bytes},raw.data(),raw.size());
      Require(io.ok()&&io.bytes_transferred==raw.size(),E::io_failure);}
    const auto step_count=r.aggregate_bytes>=512?(r.aggregate_bytes-512)/256:0;
    NativeManagementExtentViewWorkspace workspace{scratch.Array<byte>(r.aggregate_bytes),
      scratch.Array<disk::NativeCommonPageHeader>(r.page_count),scratch.Array<NativeManagementStepView>(step_count),
      scratch.Array<Uuid>(std::max(std::size_t(r.page_count)*2+1,std::size_t(step_count)*2+7))};
    return DecodeNativeManagementExtentInto(pages,r,database,bootstrap,allowance,workspace);
  }
  NativeManagementControlBundleViewRead Bundle(const disk::NativeFilespaceDevice& file,
      const NativeManagementControlBundleRoot& root,const Uuid& bootstrap,u64 allowance){
    const auto at=observations.find(file.device);
    auto result=detail::ReadNativeManagementControlBundleBacked(file,root,database,bootstrap,allowance,
      zero_images.empty()?NativeManagementControlReadContext::current:NativeManagementControlReadContext::historical_result,
      zero_images.empty()?Image{}:zero_images.at(primary),at==observations.end()?nullptr:at->second,*scratch.resource);
    if(receipt){receipt->io_status=result.io_status;receipt->io_diagnostic=std::move(result.io_diagnostic);
      Require(result.physical_bytes_read<=std::numeric_limits<u64>::max()-receipt->physical_bytes_read,E::history_mismatch);
      receipt->physical_bytes_read+=result.physical_bytes_read;}
    return result.bundle;
  }
  void ObserveHistorical(const disk::NativeFilespaceDevice& file,Image image,bool final){
    const auto& z=zeros.at(file.filespace_uuid);const auto size=Size(*file.device);Require(size.ok(),E::io_failure);
    if(final)Require(size.size_bytes==physical_sizes.at(file.filespace_uuid),E::history_mismatch);
    else{Require(size.size_bytes>=z.total_pages*u64{z.bootstrap.page_size_bytes},E::history_mismatch);physical_sizes.emplace(file.filespace_uuid,size.size_bytes);}
    auto actual=scratch.Array<byte>(z.bootstrap.page_size_bytes);
    const auto io=ReadAt(*file.device,0,actual.data(),actual.size());Require(io.ok()&&io.bytes_transferred==actual.size(),E::io_failure);
    Require(std::equal(image.begin(),image.begin()+4096,actual.begin())&&std::equal(image.begin()+4480,image.end(),actual.begin()+4480),E::history_mismatch);
  }
};
using Semantic=std::tuple<Uuid,u16,Uuid,Uuid,Uuid,Uuid,Uuid,std::optional<u64>,std::array<byte,32>>;
template<class Entries,class Latest,class Idempotency,class Semantics,class Steps,class Operation,class Evolve>
void IndexValues(const Entries& entries,Latest& latest,Idempotency& idempotency,
    Semantics& semantics,Steps& steps,Evolve evolve,const Operation* append,
    const std::optional<NativeStartupBinding>& appended_binding={}){
  for(std::size_t i=0;i<entries.size()+(append?1:0);++i){const auto& o=i==entries.size()?*append:entries[i].record;
    const auto& binding=i==entries.size()?appended_binding:entries[i].plan.intent.startup_binding;
    if(binding)Require(binding->operation_uuid==o.uuid&&!o.normalized_request_bytes.empty(),E::history_mismatch);
    const auto old=latest.find(o.uuid);
    if(old==latest.end())Require(o.revision==1&&o.state==NativeManagementState::created,E::transition_failure);
    else{Require(entries[old->second].plan.intent.startup_binding==binding,E::history_mismatch);
      const auto e=evolve(entries[old->second].record,o);
      if(e==NativeManagementOperationError::hash_failure)throw E::hash_failure;
      if(e==NativeManagementOperationError::resource_exhausted)throw E::resource_exhausted;
      Require(e==NativeManagementOperationError::none,E::transition_failure);}
    latest[o.uuid]=i;
    const typename Idempotency::key_type key{u16(o.scope),o.idempotency_key};const auto [k,added]=idempotency.emplace(key,o.uuid);(void)added;Require(k->second==o.uuid,E::idempotency_conflict);
    const Semantic semantic{o.descriptor_uuid,u16(o.scope),o.target_type_uuid,o.target_uuid,o.initiator_uuid,o.request_context_uuid,o.policy_snapshot_uuid,o.generation_guards[3],o.normalized_request_sha256};
    const auto [s,inserted]=semantics.emplace(semantic,o.uuid);(void)inserted;Require(s->second==o.uuid,E::idempotency_conflict);
    for(const auto& step:o.steps){const auto [entry,fresh]=steps.emplace(step.uuid,o.uuid);(void)fresh;Require(entry->second==o.uuid,E::history_mismatch);}
  }
  for(const auto& [step,owner]:steps){(void)owner;Require(!latest.contains(step),E::history_mismatch);}
}
void Index(const std::vector<NativeManagementHistoryEntry>& entries,
    std::map<Uuid,std::size_t>& latest,std::map<std::pair<u16,std::string>,Uuid>& idempotency,
    const NativeManagementOperation* append=nullptr,const std::optional<NativeStartupBinding>& binding={}){
  std::map<Semantic,Uuid> semantics;std::map<Uuid,Uuid> steps;
  IndexValues(entries,latest,idempotency,semantics,steps,
    [](const auto& a,const auto& b){return ValidateNativeManagementOperationEvolution(a,b);},append,binding);
}
void IndexView(std::span<const NativeManagementHistoryEntryView> entries,
    std::pmr::map<Uuid,std::size_t>& latest,
    std::pmr::map<std::pair<u16,std::string_view>,Uuid>& idempotency,Scratch& scratch){
  std::pmr::map<Semantic,Uuid> semantics(scratch.resource);std::pmr::map<Uuid,Uuid> steps(scratch.resource);
  IndexValues(entries,latest,idempotency,semantics,steps,[&](const auto& a,const auto& b){
    const auto count=std::max(a.steps.size(),b.steps.size());
    Require(count<=(std::numeric_limits<std::size_t>::max()-7)/2,E::resource_exhausted);
    return ValidateNativeManagementOperationEvolutionView(a,b,scratch.Array<Uuid>(2*count+7));},
    static_cast<const NativeManagementOperationView*>(nullptr));
}
NativeManagementHistoryView ReadHistoryView(const Uuid& database,std::span<const disk::NativeFilespaceDevice> supplied,
    const Uuid& primary,u64 budget,const NativeManagementCheckpointAnchor* requested,
    const std::span<const NativeManagementHistoricalPageZero>* historical_context,
    Scratch& scratch,std::span<disk::FileDevice::ReadLatencyBatch* const> observations,
    NativeManagementHistoryDeviceRead* receipt) noexcept {
  try{
    std::optional<NativeManagementCheckpointAnchor> explicit_anchor;
    if(requested){explicit_anchor=*requested;const auto& a=*explicit_anchor;
      Require(V7(a.checkpoint.filespace_uuid)&&a.checkpoint.page_number&&a.checkpoint.page_generation&&disk::FindCanonicalFilespacePageProfile(a.checkpoint.page_size_profile_uuid)&&
        V7(a.checkpoint_object_uuid)&&V7(a.timeline_uuid)&&a.checkpoint_generation&&a.root_set_generation&&
        std::any_of(a.checkpoint_sha256.begin(),a.checkpoint_sha256.end(),[](byte v){return v!=0;}),E::invalid_request);}
    Require(V7(database)&&V7(primary)&&!supplied.empty(),E::invalid_request);Context c(scratch,receipt);c.database=database;c.primary=primary;c.budget=budget;c.devices.assign(supplied.begin(),supplied.end());
    for(std::size_t i=0;i<observations.size();++i)c.observations.emplace(supplied[i].device,observations[i]);
    c.historical=historical_context!=nullptr;Require(!c.historical||(requested&&historical_context->size()==supplied.size()),E::invalid_request);
    std::sort(c.devices.begin(),c.devices.end(),[](const auto& a,const auto& b){return a.filespace_uuid<b.filespace_uuid;});std::pmr::set<disk::FileDevice*> handles(scratch.resource);c.guards.reserve(c.devices.size());
    for(std::size_t i=0;i<c.devices.size();++i){const auto& f=c.devices[i];const auto* profile=disk::FindCanonicalFilespacePageProfile(f.page_size_profile_uuid);
      Require(V7(f.filespace_uuid)&&profile&&f.device&&handles.insert(f.device).second&&(!i||c.devices[i-1].filespace_uuid!=f.filespace_uuid),E::invalid_request);
      c.guards.push_back(f.device->AcquireOperationGuard());Require(f.device->is_open(),E::invalid_request);}
    for(const auto& f:c.devices){const auto* profile=disk::FindCanonicalFilespacePageProfile(f.page_size_profile_uuid);c.Charge(2*u64{profile->page_size_bytes});
      const disk::FilespaceBootstrapBinding binding{database,f.filespace_uuid,f.page_size_profile_uuid};disk::FilespacePageZeroViewResult zero;Image observed;
      if(historical_context){const auto at=std::find_if(historical_context->begin(),historical_context->end(),
          [&](const auto& v){return v.filespace_uuid==f.filespace_uuid;});
        Require(at!=historical_context->end(),E::invalid_request);c.Charge(profile->page_size_bytes);
        observed=scratch.Copy<byte>(at->image);
        zero=disk::DecodeFilespacePageZeroInto(observed,scratch.Array<disk::FilespaceRootReference>(32),&binding);
        if(zero.ok())c.zero_images.emplace(f.filespace_uuid,observed);
      }else zero=c.PageZero(f,observed);
      if(!zero.ok())BootstrapError(zero.error);
      Require(!(zero.record->bootstrap.flags&disk::FilespaceBootstrapFlag::payload_encrypted),E::encrypted_requires_authority);Require(!(zero.record->bootstrap.flags&disk::FilespaceBootstrapFlag::cluster_authority_required),E::cluster_requires_authority);
      c.zeros.emplace(f.filespace_uuid,*zero.record);c.observed_zero_images.emplace(f.filespace_uuid,observed);if(historical_context)c.ObserveHistorical(f,c.zero_images.at(f.filespace_uuid),false);}
    const auto zi=c.zeros.find(primary);Require(zi!=c.zeros.end(),E::invalid_request);const auto& z=zi->second;Require(z.bootstrap.filespace_role<=4,E::bootstrap_failure);const auto& file=c.File(primary,z.bootstrap.page_size_profile_uuid);const u64 size=z.bootstrap.page_size_bytes;
    NativeManagementCheckpointAnchor anchor;std::optional<NativeCheckpointSelection> actual_selection;
    if(explicit_anchor)anchor=*explicit_anchor;
    else{
    c.Charge(4*size);std::array<std::span<byte>,2> slots;std::array<disk::FilespaceRootReference,2> roots;
    for(unsigned i=0;i<2;++i){const auto root=std::find_if(z.roots.begin(),z.roots.end(),[&](const auto& r){return r.kind==18+i;});Require(root!=z.roots.end(),E::selection_failure);roots[i]=*root;auto& b=slots[i];b=scratch.Array<byte>(size);
      const auto io=c.ReadAt(*file.device,root->page_number*size,b.data(),b.size());Require(io.ok()&&io.bytes_transferred==b.size(),E::io_failure);
      const disk::NativeCommonPageHeaderBinding binding{{database,primary,z.bootstrap.page_size_profile_uuid},root->page_number,root->page_generation,0x30e,{}};Require(disk::DecodeNativeCommonPageHeader(b.data(),128,&binding).ok(),E::selection_failure);}
    const auto pair=ClassifyNativeCheckpointSelectionPair(slots[0],slots[1]);if(!pair.ok())throw pair.error==NativeCheckpointSelectionError::hash_failure?E::hash_failure:pair.error==NativeCheckpointSelectionError::resource_exhausted?E::resource_exhausted:E::selection_failure;
    const auto& selection=*pair.selection;Require(selection.bootstrap_uuid==z.page_uuid&&selection.object_uuid==roots[0].object_uuid&&selection.object_uuid==roots[1].object_uuid,E::selection_failure);
    anchor={selection.checkpoint,selection.checkpoint_object_uuid,selection.checkpoint_sha256,selection.checkpoint_generation,selection.root_set_generation,selection.timeline_uuid};actual_selection=selection;
    }
    c.timeline=anchor.timeline_uuid;auto current=c.Read(anchor.checkpoint,anchor.checkpoint_object_uuid,anchor.checkpoint_sha256);
    Require(current.root.checkpoint_generation==anchor.checkpoint_generation&&current.root.root_set_generation==anchor.root_set_generation,E::history_mismatch);
    if(actual_selection){const auto& selection=*actual_selection;
      if(selection.selection_generation==1)Require(!current.root.predecessor,E::history_mismatch);
      else Require(current.root.predecessor==selection.previous_checkpoint&&current.root.predecessor_sha256==selection.previous_checkpoint_sha256&&current.root.object_uuid==selection.previous_checkpoint_object_uuid,E::history_mismatch);
      if(!current.root.creator_operation_uuid.is_nil())Require(current.root.creator_operation_uuid==selection.publication_uuid,E::history_mismatch);
    }
    NativeManagementHistoryView result;std::pmr::vector<NativeManagementHistoryEntryView> entries(scratch.resource);
    std::pmr::set<Uuid> attempts(scratch.resource);
    while(true){
      const auto head=Head(current.root);
      if(current.root.creator_operation_uuid.is_nil()){
        if(!current.root.predecessor){const auto genesis=std::find_if(z.roots.begin(),z.roots.end(),[](const auto& r){return r.kind==9;});Require(genesis!=z.roots.end()&&!head&&current.root.checkpoint_generation==1&&current.root.root_set_generation==1&&Self(current.root)==disk::NativePageReference{genesis->filespace_uuid,genesis->page_number,genesis->page_generation,genesis->page_size_profile_uuid}&&current.root.object_uuid==genesis->object_uuid,E::history_mismatch);break;}
        auto parent=c.Parent(current);Require(SameHead(head,Head(parent.root)),E::history_mismatch);current=std::move(parent);continue;
      }
      Require(head&&head->page_type==0x500&&head->page.filespace_uuid==primary&&head->page.page_size_profile_uuid==z.bootstrap.page_size_profile_uuid&&head->page.page_number<z.total_pages,E::history_mismatch);
      c.Charge(2*size);auto bytes=scratch.Array<byte>(size);const auto io=c.ReadAt(*file.device,head->page.page_number*size,bytes.data(),bytes.size());Require(io.ok()&&io.bytes_transferred==bytes.size(),E::io_failure);
      auto image=DecodeNativePublicationPlanView(bytes);PlanError(image.error);auto p=std::move(*image.plan);
      if(actual_selection&&Self(current.root)==anchor.checkpoint&&p.base_selection_generation)
        Require(*p.base_selection_generation+1==actual_selection->selection_generation,E::history_mismatch);
      Require(p.management_extent&&Self(p)==head->page&&p.object_uuid==head->object_uuid&&image.sha256==head->sha256&&p.bootstrap_uuid==z.page_uuid&&p.header.database_uuid==database&&p.timeline_uuid==c.timeline&&p.operation_uuid==current.root.creator_operation_uuid&&p.target_checkpoint==Self(current.root)&&p.target_checkpoint_object_uuid==current.root.object_uuid&&p.reserved_generation==current.root.checkpoint_generation&&p.target_root_set_generation==current.root.root_set_generation,E::history_mismatch);
      Require(attempts.insert(p.operation_uuid).second,E::history_mismatch);
      const auto graph=ComputeNativePublicationTargetGraphDigestInto(current.bytes,scratch.Array<NativeCheckpointRootReference>(16));PlanError(graph.error);Require(graph.sha256==p.target_graph_sha256,E::history_mismatch);
      auto base=c.Parent(current);Require(p.base_checkpoint==Self(base.root)&&p.base_checkpoint_object_uuid==base.root.object_uuid&&p.base_checkpoint_sha256==base.sha&&p.base_checkpoint_generation==base.root.checkpoint_generation&&p.base_root_set_generation==base.root.root_set_generation,E::history_mismatch);
      const auto previous=Head(base.root);if(previous)Require(p.previous_plan&&*p.previous_plan==previous->page&&p.previous_plan_object_uuid==previous->object_uuid&&p.previous_plan_sha256==previous->sha256,E::history_mismatch);else Require(!p.previous_plan,E::history_mismatch);
      const auto& r=*p.management_extent;const u64 allowance=u64{r.page_count}*size+4*u64{r.aggregate_bytes}+2*size;c.Charge(allowance);
      auto extent=c.Extent(file,r,z.page_uuid,allowance);
      if(!extent.ok())throw extent.error==NativeManagementExtentError::resource_exhausted?E::resource_exhausted:extent.error==NativeManagementExtentError::hash_failure?E::hash_failure:extent.error==NativeManagementExtentError::io_failure?E::io_failure:extent.error==NativeManagementExtentError::cluster_requires_authority?E::cluster_requires_authority:extent.error==NativeManagementExtentError::encrypted_requires_authority?E::encrypted_requires_authority:E::extent_failure;
      PlanError(BindNativePublicationPlanToManagementRecordInto(p,extent.aggregate,scratch.OperationWorkspace(extent.aggregate)));
      for(const auto& h:extent.page_headers)Require(h.page_uuid!=p.header.page_uuid,E::history_mismatch);
      std::span<const disk::NativeCommonPageHeader> bundle_headers;std::span<const Image> allocation_images,inventory_images,directory_images,growth_images;
      if(p.control_bundle){const auto& root=*p.control_bundle;const u64 allowance=root.page_count*size+4*(root.directory_count?root.payload_bytes:(root.map_count+root.inventory_count)*size)+2*size;c.Charge(allowance);
        if(root.directory_count)c.HistoricalContexts();
        auto bundle=c.Bundle(file,root,z.page_uuid,allowance);
        if(!bundle.ok())throw bundle.error==NativeManagementControlBundleError::resource_exhausted?E::resource_exhausted:bundle.error==NativeManagementControlBundleError::hash_failure?E::hash_failure:bundle.error==NativeManagementControlBundleError::io_failure?E::io_failure:bundle.error==NativeManagementControlBundleError::cluster_requires_authority?E::cluster_requires_authority:bundle.error==NativeManagementControlBundleError::encrypted_requires_authority?E::encrypted_requires_authority:E::extent_failure;
        const auto primary_image=std::find_if(bundle.allocation_images.begin(),bundle.allocation_images.end(),[&](const auto& raw){const auto h=disk::DecodeNativeCommonPageHeader(raw.data(),128);return h.ok()&&h.header->filespace_uuid==primary;});
        Require(primary_image!=bundle.allocation_images.end(),E::history_mismatch);const auto first=scratch.Map(*primary_image);
        if(!first.ok())throw first.error==page::NativeAllocationError::resource_exhausted?E::resource_exhausted:first.error==page::NativeAllocationError::hash_failure?E::hash_failure:E::history_mismatch;
        const auto target=std::find_if(current.root.roots.begin(),current.root.roots.end(),[](const auto& r){return r.role==4;});const auto& h=first.map->header;
        Require(target!=current.root.roots.end()&&target->page==disk::NativePageReference{h.filespace_uuid,h.page_number,h.page_generation,h.page_size_profile_uuid}&&target->object_uuid==first.map->object_uuid&&target->sha256==Hash(*primary_image),E::history_mismatch);
        std::pmr::set<Uuid> control_ids(scratch.resource);control_ids.insert(p.header.page_uuid);for(const auto& e:extent.page_headers)control_ids.insert(e.page_uuid);
        for(const auto& h:bundle.page_headers)Require(control_ids.insert(h.page_uuid).second,E::history_mismatch);
        if(root.inventory_count){const auto head=scratch.Inventory(bundle.inventory_images.front());
          if(!head.ok())throw head.error==page::NativeInventoryError::resource_exhausted?E::resource_exhausted:head.error==page::NativeInventoryError::hash_failure?E::hash_failure:E::history_mismatch;
          const auto& inventory=*head.page;const auto& h=inventory.header;const auto target=std::find_if(current.root.roots.begin(),current.root.roots.end(),[](const auto& r){return r.role==1;});
          Require(target!=current.root.roots.end()&&target->page_type==0x301&&target->page==disk::NativePageReference{h.filespace_uuid,h.page_number,h.page_generation,h.page_size_profile_uuid}&&target->object_uuid==inventory.object_uuid&&target->sha256==Hash(bundle.inventory_images.front())&&current.root.selected_local_transaction_id==inventory.inventory.next_local_transaction_id-1,E::history_mismatch);
          for(const auto& bytes:bundle.inventory_images){const auto h=disk::DecodeNativeCommonPageHeader(bytes.data(),128);Require(h.ok()&&control_ids.insert(h.header->page_uuid).second,E::history_mismatch);}
        }
        if(root.directory_count){
          const auto target=std::find_if(current.root.roots.begin(),current.root.roots.end(),[](const auto& r){return r.role==3;});Require(target!=current.root.roots.end(),E::history_mismatch);
          for(std::size_t n=0;n<bundle.directory_images.size();++n){const auto& raw=bundle.directory_images[n];const auto decoded=scratch.Directory(raw);
            if(!decoded.ok())throw decoded.error==page::NativeDirectoryError::resource_exhausted?E::resource_exhausted:decoded.error==page::NativeDirectoryError::hash_failure?E::hash_failure:E::history_mismatch;
            const auto& dir=*decoded.directory;const auto& h=dir.header;Require(control_ids.insert(h.page_uuid).second,E::history_mismatch);
            if(!n)Require(target->page_type==9&&target->page==disk::NativePageReference{h.filespace_uuid,h.page_number,h.page_generation,h.page_size_profile_uuid}&&target->object_uuid==dir.object_uuid&&target->sha256==Hash(raw),E::history_mismatch);
            for(const auto& member:dir.records){const auto at=c.zeros.find(member.bootstrap.filespace_uuid);Require(at!=c.zeros.end(),E::history_mismatch);const auto& zero=at->second;
              Require(SameBootstrap(member.bootstrap,zero.bootstrap)&&member.page_zero_uuid==zero.page_uuid&&member.page_zero_generation==zero.page_generation&&
                member.root_set_generation==zero.root_set_generation&&member.total_pages==zero.total_pages,E::history_mismatch);}
          }
        }
        if(!bundle.growth_images.empty()){const auto& before=bundle.growth_images.front();const auto& after=bundle.growth_images.back();
          const auto decoded=scratch.PageZero(before);if(!decoded.ok())BootstrapError(decoded.error);const auto& fs=decoded.record->bootstrap.filespace_uuid;
          Require(c.zero_images.contains(fs)&&std::equal(c.zero_images.at(fs).begin(),c.zero_images.at(fs).end(),after.begin(),after.end()),E::history_mismatch);
          c.zeros.at(fs)=*decoded.record;c.zero_images.at(fs)=before;c.Bound(base.root);
          const auto capacity=c.zeros.at(primary).total_pages;
          Require(p.header.page_number<capacity&&p.target_checkpoint.page_number<capacity&&p.management_extent->first.page_number<capacity&&
            p.management_extent->page_count<=capacity-p.management_extent->first.page_number&&root.first.page_number<capacity&&root.page_count<=capacity-root.first.page_number,E::history_mismatch);
        }
        bundle_headers=std::move(bundle.page_headers);allocation_images=std::move(bundle.allocation_images);
        inventory_images=std::move(bundle.inventory_images);
        directory_images=std::move(bundle.directory_images);growth_images=std::move(bundle.growth_images);
      }
      entries.push_back({std::move(p),std::move(*extent.record),std::move(extent.page_headers),image.sha256,current.sha,std::move(bundle_headers),std::move(allocation_images),std::move(inventory_images),std::move(directory_images),std::move(growth_images),extent.aggregate});current=std::move(base);
    }
    std::reverse(entries.begin(),entries.end());
    std::pmr::map<Uuid,std::size_t> latest(scratch.resource);
    std::pmr::map<std::pair<u16,std::string_view>,Uuid> idempotency(scratch.resource);
    IndexView(entries,latest,idempotency,scratch);
    result.entries=scratch.Copy<NativeManagementHistoryEntryView>(entries);
    auto latest_values=scratch.Array<NativeManagementHistoryLatest>(latest.size());std::size_t at=0;
    for(const auto& [uuid,index]:latest)latest_values[at++]={uuid,index};result.latest=latest_values;
    auto keys=scratch.Array<NativeManagementHistoryIdempotency>(idempotency.size());at=0;
    for(const auto& [key,uuid]:idempotency)keys[at++]={key.first,key.second,uuid};result.idempotency=keys;
    if(c.historical)for(const auto& file:c.devices)c.ObserveHistorical(file,c.zero_images.at(file.filespace_uuid),true);
    result.anchor=anchor;result.selection=actual_selection;result.verified_image_bytes=c.used;result.error=E::none;return result;
  }catch(E e){return FailView(e);}catch(const std::bad_alloc&){return FailView(E::resource_exhausted);}catch(const std::length_error&){return FailView(E::resource_exhausted);}catch(...){return FailView(E::io_failure);}
}
NativeManagementHistory MaterializeHistory(const NativeManagementHistoryView& view){
  if(!view.ok())return Fail(view.error);
  NativeManagementHistory out;out.anchor=view.anchor;out.selection=view.selection;
  out.verified_image_bytes=view.verified_image_bytes;out.entries.reserve(view.entries.size());
  for(const auto& entry:view.entries){
    NativeManagementHistoryEntry e;e.plan=entry.plan;
    // Materialize only after complete source verification and device unlock.
    // Decode the retained full aggregate, never rebuild it from hashes/views.
    const std::vector<byte> aggregate(entry.aggregate.begin(),entry.aggregate.end());
    auto record=DecodeNativeManagementOperation(aggregate,aggregate.size());
    if(!record.ok())throw record.error==NativeManagementOperationError::hash_failure?E::hash_failure:
      record.error==NativeManagementOperationError::resource_exhausted?E::resource_exhausted:E::history_mismatch;
    e.record=std::move(*record.record);e.extent_pages.assign(entry.extent_pages.begin(),entry.extent_pages.end());
    e.bundle_pages.assign(entry.bundle_pages.begin(),entry.bundle_pages.end());
    e.plan_sha256=entry.plan_sha256;e.checkpoint_sha256=entry.checkpoint_sha256;
    const auto copy=[](const auto& source,auto& destination){
      destination.reserve(source.size());for(const auto& image:source)destination.emplace_back(image.begin(),image.end());};
    copy(entry.control_allocation_images,e.control_allocation_images);
    copy(entry.control_inventory_images,e.control_inventory_images);
    copy(entry.control_directory_images,e.control_directory_images);
    copy(entry.control_growth_images,e.control_growth_images);out.entries.push_back(std::move(e));
  }
  for(const auto& entry:view.latest)out.latest.emplace(entry.operation_uuid,entry.entry);
  for(const auto& entry:view.idempotency)out.idempotency.emplace(std::make_pair(entry.scope,std::string(entry.key)),entry.operation_uuid);
  out.error=E::none;return out;
}
NativeManagementHistory ReadHistory(const Uuid& database,const std::vector<disk::NativeFilespaceDevice>& supplied,
    const Uuid& primary,u64 budget,const NativeManagementCheckpointAnchor* anchor,
    const std::map<Uuid,std::vector<byte>>* historical=nullptr) noexcept {
  try {
    detail::NativeMetadataHeapMemory heap;std::pmr::monotonic_buffer_resource resource(&heap);
    Scratch scratch{&resource};std::span<NativeManagementHistoricalPageZero> contexts;
    if(historical){contexts=scratch.Array<NativeManagementHistoricalPageZero>(historical->size());
      std::size_t i=0;for(const auto& [id,raw]:*historical)contexts[i++]={id,raw};}
    const std::span<const NativeManagementHistoricalPageZero> input=contexts;
    return MaterializeHistory(ReadHistoryView(database,supplied,primary,budget,anchor,
      historical?&input:nullptr,scratch,{},nullptr));
  }catch(E error){return Fail(error);}catch(const std::bad_alloc&){return Fail(E::resource_exhausted);}
   catch(const std::length_error&){return Fail(E::resource_exhausted);}catch(...){return Fail(E::io_failure);}
}
} // namespace
NativeManagementHistoryDeviceRead ReadNativeManagementHistoryInto(
    const Uuid& database,std::span<const disk::NativeFilespaceDevice> files,const Uuid& primary,
    u64 budget,NativeManagementHistoryReadContext context,const NativeManagementCheckpointAnchor* anchor,
    std::span<const NativeManagementHistoricalPageZero> historical,
    std::span<disk::FileDevice::ReadLatencyBatch* const> observations,std::span<byte> backing) noexcept {
  NativeManagementHistoryDeviceRead out;
  if((context!=NativeManagementHistoryReadContext::selected&&context!=NativeManagementHistoryReadContext::anchored&&
      context!=NativeManagementHistoryReadContext::historical_result)||
     (context==NativeManagementHistoryReadContext::selected?anchor!=nullptr:anchor==nullptr)||
     (context==NativeManagementHistoryReadContext::historical_result?historical.size()!=files.size():!historical.empty())||
     files.empty()||observations.size()!=files.size())return out;
  for(std::size_t i=0;i<files.size();++i)
    if(!files[i].device||!observations[i]||&observations[i]->device()!=files[i].device)return out;
  const auto disjoint=[&](auto input){return disk::detail::DisjointNativeDecodeRegions(backing,input);};
  bool valid=disjoint(std::span{&database,1})&&disjoint(std::span{&primary,1})&&disjoint(files)&&
    disjoint(observations)&&disjoint(historical)&&(!anchor||disjoint(std::span{anchor,1}));
  for(std::size_t i=0;i<files.size();++i)
    valid=valid&&disjoint(std::span{files[i].device,1})&&disjoint(std::span{observations[i],1});
  for(const auto& entry:historical)valid=valid&&disjoint(entry.image);
  if(!valid){out.history.error=E::invalid_workspace;return out;}
  try{
    detail::NativeMetadataMemory resource(backing);Scratch scratch{&resource};
    out.history=ReadHistoryView(database,files,primary,budget,anchor,
      context==NativeManagementHistoryReadContext::historical_result?&historical:nullptr,scratch,observations,&out);
    if(out.ok())out.history.backing_bytes_used=resource.used();
  }catch(const std::bad_alloc&){out.history=FailView(E::resource_exhausted);}
   catch(const std::length_error&){out.history=FailView(E::resource_exhausted);}
   catch(...){out.history=FailView(E::io_failure);}
  return out;
}
NativeManagementHistoryError ValidateNativeManagementHistoryAppend(const NativeManagementGraphHistory& history,
    const NativeManagementOperation& record,u64 budget,const std::optional<NativeStartupBinding>& startup_binding) noexcept {
  try{
    Require(history.ok(),E::invalid_request);
    // Bound index nodes, retained strings and steps before constructing indexes.
    u64 used=0;const auto charge=[&](u64 bytes){Require(bytes<=budget-used,E::resource_exhausted);used+=bytes;};
    const auto account=[&](const NativeManagementOperation& o){charge(1024);charge(o.idempotency_key.size());
      Require(o.steps.size()<=(budget-used)/768,E::resource_exhausted);charge(o.steps.size()*768);
      for(const auto& step:o.steps)charge(step.idempotency_key.size());};
    for(const auto& entry:history.entries)account(entry.record);account(record);
    const auto valid=ValidateNativeManagementOperation(record);
    if(valid!=NativeManagementOperationError::none)throw valid==NativeManagementOperationError::resource_exhausted?E::resource_exhausted:valid==NativeManagementOperationError::hash_failure?E::hash_failure:E::transition_failure;
    std::map<Uuid,std::size_t> latest;std::map<std::pair<u16,std::string>,Uuid> idempotency;
    Index(history.entries,latest,idempotency,&record,startup_binding);return E::none;
  }catch(E e){return e;}catch(const std::bad_alloc&){return E::resource_exhausted;}catch(const std::length_error&){return E::resource_exhausted;}catch(...){return E::history_mismatch;}
}
NativeManagementHistory ReadNativeManagementHistoryFromOpenDevices(const Uuid& database,const std::vector<disk::NativeFilespaceDevice>& devices,const Uuid& primary,u64 budget) noexcept {
  return ReadHistory(database,devices,primary,budget,nullptr);
}
NativeManagementGraphHistory ReadNativeManagementGraphHistoryFromOpenDevices(const Uuid& database,const std::vector<disk::NativeFilespaceDevice>& devices,const Uuid& primary,const NativeManagementCheckpointAnchor& anchor,u64 budget) noexcept {
  auto result=ReadHistory(database,devices,primary,budget,&anchor);
  return std::move(static_cast<NativeManagementGraphHistory&>(result));
}
NativeManagementGraphHistory ReadNativeManagementGraphHistoryAtHistoricalContextFromOpenDevices(const Uuid& database,const std::vector<disk::NativeFilespaceDevice>& devices,const Uuid& primary,
    const NativeManagementCheckpointAnchor& anchor,const std::map<Uuid,std::vector<byte>>& context,u64 budget) noexcept {
  auto result=ReadHistory(database,devices,primary,budget,&anchor,&context);
  return std::move(static_cast<NativeManagementGraphHistory&>(result));
}
} // namespace scratchbird::storage::database
