// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_publication_coordinator.hpp"
#include "native_publication_plan.hpp"
#include "native_management_control_allocation.hpp"
#include "native_management_control_authority.hpp"
#include "disk_device.hpp"
#include "hash_digest_parts.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <limits>
#include <mutex>
#include <new>
#include <set>
#include <stdexcept>
#include <type_traits>

namespace scratchbird::storage::database {
namespace {
using E=NativePublicationError;
namespace mga=transaction::mga;
void Require(bool ok,E error){if(!ok)throw error;}
bool V7(const Uuid& id){return core::uuid::IsEngineIdentityUuid(id);}
void Backend(NativePublicationWatermarkError error){
  if(error==NativePublicationWatermarkError::hash_failure)throw E::hash_failure;
  if(error==NativePublicationWatermarkError::resource_exhausted)throw E::resource_exhausted;
}
struct Context {
  std::vector<disk::NativeFilespaceDevice> devices;
  std::vector<std::unique_lock<std::recursive_mutex>> guards;
  disk::FileDevice* primary=nullptr;
  disk::FilespacePageZero zero;
  NativeBoundCheckpointSelection bound;
  std::array<disk::NativeCommonPageHeader,2> headers;
  std::array<std::vector<byte>,2> bytes;
  std::array<NativePublicationWatermarkImage,2> decoded;
  NativePublicationSnapshot snapshot;
  bool stable=false;
  Uuid watermark_object;
};
bool SameBase(const NativePublicationSnapshot& a,const NativePublicationSnapshot& b){
  const auto& x=a.selection;const auto& y=b.selection;
  return a.state_sha256==b.state_sha256&&a.watermark.watermark==b.watermark.watermark&&
    a.watermark.header.database_uuid==b.watermark.header.database_uuid&&
    a.watermark.header.filespace_uuid==b.watermark.header.filespace_uuid&&
    x.selection_generation==y.selection_generation&&x.publication_uuid==y.publication_uuid&&
    x.bootstrap_uuid==y.bootstrap_uuid&&x.object_uuid==y.object_uuid&&
    x.checkpoint==y.checkpoint&&x.checkpoint_object_uuid==y.checkpoint_object_uuid&&
    x.checkpoint_sha256==y.checkpoint_sha256&&x.checkpoint_generation==y.checkpoint_generation&&
    x.root_set_generation==y.root_set_generation&&x.timeline_uuid==y.timeline_uuid;
}
void BindState(const Context& c,const NativePublicationWatermark& w){
  const auto& cp=*c.bound.checkpoint_inventory.checkpoint;
  Require(!w.abandonment||w.watermark>cp.checkpoint_generation,E::binding_mismatch);
  Require(w.object_uuid==c.watermark_object&&
    w.bootstrap_uuid==c.zero.page_uuid&&w.timeline_uuid==cp.timeline_uuid&&
    w.watermark>=cp.checkpoint_generation,E::binding_mismatch);
  const auto* base=&c.bound.checkpoint_inventory;
  if(w.watermark==cp.checkpoint_generation&&w.watermark>1){
    Require(c.bound.predecessor.ok()&&w.operation_uuid==c.bound.selection->publication_uuid,E::binding_mismatch);
    base=&c.bound.predecessor;
  }else if(w.watermark==1){
    Require(cp.checkpoint_generation==1&&cp.root_set_generation==1&&!cp.predecessor&&
      w.operation_uuid==c.zero.creation_operation_uuid,E::binding_mismatch);
  }else Require(w.operation_uuid!=c.bound.selection->publication_uuid,E::binding_mismatch);
  const auto& b=*base->checkpoint;
  const disk::NativePageReference ref{b.header.filespace_uuid,b.header.page_number,b.header.page_generation,b.header.page_size_profile_uuid};
  Require(w.base_checkpoint==ref&&w.base_checkpoint_object_uuid==b.object_uuid&&
    w.base_checkpoint_sha256==base->checkpoint_sha256&&w.base_checkpoint_generation==b.checkpoint_generation&&
    w.base_root_set_generation==b.root_set_generation,E::binding_mismatch);
}
std::unique_ptr<Context> Prepare(const Uuid& database,const std::vector<disk::NativeFilespaceDevice>& supplied,
    const Uuid& primary_uuid,u64 budget,bool writable,bool recover){
  Require(V7(database)&&V7(primary_uuid)&&!supplied.empty(),E::invalid_request);
  auto c=std::make_unique<Context>();c->devices=supplied;
  std::sort(c->devices.begin(),c->devices.end(),[](const auto& a,const auto& b){return a.filespace_uuid<b.filespace_uuid;});
  std::set<disk::FileDevice*> handles;c->guards.reserve(c->devices.size());
  for(std::size_t i=0;i<c->devices.size();++i){const auto& f=c->devices[i];
    Require(V7(f.filespace_uuid)&&disk::FindCanonicalFilespacePageProfile(f.page_size_profile_uuid)&&f.device&&
      handles.insert(f.device).second&&(!i||c->devices[i-1].filespace_uuid!=f.filespace_uuid),E::invalid_request);
    c->guards.push_back(f.device->AcquireOperationGuard());Require(f.device->is_open(),E::invalid_device);
  }
  const auto target=std::find_if(c->devices.begin(),c->devices.end(),[&](const auto& f){return f.filespace_uuid==primary_uuid;});
  Require(target!=c->devices.end(),E::invalid_device);c->primary=target->device;
  Require(!writable||!c->primary->read_only(),E::invalid_device);
  const disk::FilespaceBootstrapBinding binding{database,primary_uuid,target->page_size_profile_uuid};
  auto zero=disk::ReadFilespacePageZeroFromOpenDevice(*c->primary,&binding);
  if(!zero.ok())throw zero.error==disk::FilespacePageZeroError::hash_provider_failure?E::hash_failure:
    zero.error==disk::FilespacePageZeroError::resource_exhausted?E::resource_exhausted:
    zero.error==disk::FilespacePageZeroError::io_failure?E::io_failure:E::bootstrap_failure;
  c->zero=std::move(*zero.record);const auto& z=c->zero;const u64 size=z.bootstrap.page_size_bytes;
  Require(z.bootstrap.filespace_role<=4,E::invalid_device);
  Require(!(z.bootstrap.flags&disk::FilespaceBootstrapFlag::cluster_authority_required),E::cluster_requires_authority);
  Require(!(z.bootstrap.flags&disk::FilespaceBootstrapFlag::payload_encrypted),E::encrypted_requires_authority);
  Require(budget>=6*size,E::resource_exhausted);
  for(const auto& file:c->devices)if(file.filespace_uuid!=primary_uuid){
    const auto* profile=disk::FindCanonicalFilespacePageProfile(file.page_size_profile_uuid);
    Require(profile&&profile->page_size_bytes<=budget,E::resource_exhausted);
    const disk::FilespaceBootstrapBinding expected{database,file.filespace_uuid,file.page_size_profile_uuid};
    const auto secondary=disk::ReadFilespacePageZeroFromOpenDevice(*file.device,&expected);
    if(!secondary.ok())throw secondary.error==disk::FilespacePageZeroError::hash_provider_failure?E::hash_failure:
      secondary.error==disk::FilespacePageZeroError::resource_exhausted?E::resource_exhausted:
      secondary.error==disk::FilespacePageZeroError::io_failure?E::io_failure:E::bootstrap_failure;
  }
  c->bound=ReadNativeBoundCheckpointSelectionFromOpenDevices(database,c->devices,primary_uuid,budget-6*size);
  if(!c->bound.ok())throw c->bound.error==NativeCheckpointSelectionError::hash_failure?E::hash_failure:
    c->bound.error==NativeCheckpointSelectionError::resource_exhausted?E::resource_exhausted:
    c->bound.error==NativeCheckpointSelectionError::encrypted_requires_authority?E::encrypted_requires_authority:
    c->bound.error==NativeCheckpointSelectionError::cluster_requires_authority?E::cluster_requires_authority:
    c->bound.error==NativeCheckpointSelectionError::io_failure?E::io_failure:E::checkpoint_failure;
  for(unsigned i=0;i<2;++i){
    const auto root=std::find_if(z.roots.begin(),z.roots.end(),[&](const auto& r){return r.kind==20+i;});
    Require(root!=z.roots.end(),E::bootstrap_failure);
    if(i==0)c->watermark_object=root->object_uuid;
    const page::NativeAllocationRecord* allocation=nullptr;
    for(const auto& image:c->bound.allocation.pages){const auto& m=*image.map;
      if(root->page_number<m.first_page||root->page_number-m.first_page>=m.states.size())continue;
      Require(m.states[root->page_number-m.first_page]==page::NativeAllocationState::allocated,E::allocation_mismatch);
      const auto r=std::lower_bound(m.records.begin(),m.records.end(),root->page_number,[](const auto& a,u64 n){return a.page_number<n;});
      if(r!=m.records.end()&&r->page_number==root->page_number)allocation=&*r;
      break;
    }
    Require(allocation&&allocation->page_type==0x500&&allocation->page_generation==root->page_generation&&
      allocation->owner_uuid==root->object_uuid,E::allocation_mismatch);
    const auto creator=mga::LookupLocalTransaction(c->bound.checkpoint_inventory.inventory,mga::MakeLocalTransactionId(allocation->creator_local_transaction_id));
    Require(creator.ok()&&creator.entry.identity.transaction_uuid.value==allocation->creator_transaction_uuid&&
      creator.entry.identity.scope==mga::TransactionScope::local_node&&mga::HasCommittedInventoryOutcome(creator.entry),E::allocation_mismatch);
    c->headers[i]={z.bootstrap.page_size_bytes,0x500,database,primary_uuid,allocation->page_uuid,root->page_number,
      root->page_generation,0,z.bootstrap.page_size_profile_uuid};
    auto& bytes=c->bytes[i];bytes.resize(size);
    const auto read=c->primary->ReadAt(root->page_number*size,bytes.data(),bytes.size());
    Require(read.ok()&&read.bytes_transferred==bytes.size(),E::io_failure);
    // Independently intact headers retain their allocated identity even when
    // the watermark family is corrupt. Never overwrite a contradictory valid
    // header under the single-damaged-replica recovery rule.
    const auto common=disk::DecodeNativeCommonPageHeader(bytes.data(),128);
    if(common.error==disk::NativeCommonPageHeaderError::resource_exhausted)throw E::resource_exhausted;
    if(common.ok()){
      const disk::NativeCommonPageHeaderBinding expected{binding,root->page_number,root->page_generation,0x500,allocation->page_uuid};
      const auto bound_header=disk::DecodeNativeCommonPageHeader(bytes.data(),128,&expected);
      if(bound_header.error==disk::NativeCommonPageHeaderError::resource_exhausted)throw E::resource_exhausted;
      Require(bound_header.ok()&&common.header->flags==0&&common.header->page_size_bytes==size,E::binding_mismatch);
    }
    c->decoded[i]=DecodeNativePublicationWatermark(bytes);auto& d=c->decoded[i];Backend(d.error);
    if(d.ok()){
      const disk::NativeCommonPageHeaderBinding expected{binding,root->page_number,root->page_generation,0x500,allocation->page_uuid};
      Require(disk::DecodeNativeCommonPageHeader(bytes.data(),128,&expected).ok()&&d.state->object_uuid==root->object_uuid,E::binding_mismatch);
      BindState(*c,*d.state);
    }
    std::vector<byte>().swap(d.bytes);
  }
  Require(c->headers[0].page_uuid!=c->headers[1].page_uuid,E::allocation_mismatch);
  unsigned selected=0;
  if(c->decoded[0].ok()&&c->decoded[1].ok()){
    const auto pair=ClassifyNativePublicationWatermarkPair(c->bytes[0],c->bytes[1]);Backend(pair.error);
    c->stable=pair.ok();
    if(!c->stable)Require(pair.error==NativePublicationWatermarkError::repair_required&&
      (c->decoded[0].state->watermark>c->decoded[1].state->watermark||
       (c->decoded[0].state->watermark==c->decoded[1].state->watermark&&
        ((c->decoded[0].state->publication_plan&&!c->decoded[1].state->publication_plan)||
         (c->decoded[0].state->abandonment&&!c->decoded[1].state->abandonment)))),E::image_failure);
  }else {Require(c->decoded[0].ok()||c->decoded[1].ok(),E::image_failure);selected=c->decoded[0].ok()?0:1;}
  Require(c->stable||recover,E::repair_required);
  c->snapshot={*c->bound.selection,*c->decoded[selected].state,c->decoded[selected].state_sha256};
  return c;
}
std::array<std::vector<byte>,2> EncodePair(const Context& c,const NativePublicationWatermark& state){
  std::array<std::vector<byte>,2> images;
  for(unsigned i=0;i<2;++i){auto slot=state;slot.header=c.headers[i];auto image=EncodeNativePublicationWatermark(slot);
    Backend(image.error);Require(image.ok(),E::image_failure);images[i]=std::move(image.bytes);}
  return images;
}
void Publish(Context& c,std::array<std::vector<byte>,2>& images,std::vector<byte>& scratch){
  const u64 size=c.zero.bootstrap.page_size_bytes;
  const auto read=[&](unsigned i,const auto& expected,E mismatch){
    const auto io=c.primary->ReadAt(c.headers[i].page_number*size,scratch.data(),scratch.size());
    Require(io.ok()&&io.bytes_transferred==scratch.size(),E::io_failure);Require(scratch==expected,mismatch);
  };
  for(unsigned i=0;i<2;++i)read(i,c.bytes[i],E::preimage_changed);
  for(const auto& file:c.devices)if(!file.device->read_only())Require(file.device->Sync().ok(),E::io_failure);
  for(unsigned i=0;i<2;++i){if(images[i]!=c.bytes[i]){
      const auto io=c.primary->WriteAt(c.headers[i].page_number*size,images[i].data(),images[i].size());
      Require(io.ok()&&io.bytes_transferred==images[i].size(),E::io_failure);
    }
    Require(c.primary->Sync().ok(),E::io_failure);read(i,images[i],E::readback_mismatch);
  }
}
NativePublicationInspection InspectOrRecover(const Uuid& db,const std::vector<disk::NativeFilespaceDevice>& devices,
    const Uuid& primary,u64 budget,bool recover) noexcept {
  try {auto c=Prepare(db,devices,primary,budget,recover,recover);
    if(recover){auto images=EncodePair(*c,c->snapshot.watermark);std::vector<byte> scratch(c->zero.bootstrap.page_size_bytes);Publish(*c,images,scratch);}
    return {E::none,c->snapshot};
  }catch(E e){return {e,{}};}catch(const std::bad_alloc&){return {E::resource_exhausted,{}};}
   catch(const std::length_error&){return {E::resource_exhausted,{}};}catch(...){return {E::io_failure,{}};}
}
} // namespace
struct NativePublicationLease::Impl {std::unique_ptr<Context> context;NativePublicationSnapshot snapshot;bool installation_ambiguous=false;};
NativePublicationLease::NativePublicationLease(std::unique_ptr<Impl> p) noexcept:impl_(std::move(p)){}
NativePublicationLease::~NativePublicationLease()=default;
const NativePublicationSnapshot& NativePublicationLease::snapshot() const noexcept{return impl_->snapshot;}
NativePublicationInspection InspectNativePublicationGenerationOnOpenDevices(const Uuid& db,
    const std::vector<disk::NativeFilespaceDevice>& devices,const Uuid& primary,u64 budget) noexcept {
  return InspectOrRecover(db,devices,primary,budget,false);
}
NativePublicationInspection RecoverNativePublicationGenerationOnOpenDevices(const Uuid& db,
    const std::vector<disk::NativeFilespaceDevice>& devices,const Uuid& primary,u64 budget) noexcept {
  return InspectOrRecover(db,devices,primary,budget,true);
}
NativePublicationReservation ReserveNativePublicationGenerationOnOpenDevices(const Uuid& db,
    const std::vector<disk::NativeFilespaceDevice>& devices,const Uuid& primary,
    const NativePublicationSnapshot& expected,const Uuid& operation,u64 budget,const NativePublicationIntent* intent) noexcept {
  try {
    Require(V7(operation),E::invalid_request);auto c=Prepare(db,devices,primary,budget,true,false);
    Require(SameBase(expected,c->snapshot),E::stale_base);
    auto w=c->snapshot.watermark;
    Require(!w.intent||w.abandonment||w.watermark==c->snapshot.selection.checkpoint_generation,E::operation_pending);
    Require(operation!=w.operation_uuid&&operation!=c->snapshot.selection.publication_uuid,E::invalid_request);
    Require(w.watermark!=std::numeric_limits<u64>::max(),E::generation_exhausted);
    w.previous_watermark=w.watermark;++w.watermark;w.previous_state_sha256=c->snapshot.state_sha256;w.operation_uuid=operation;
    w.intent=intent?std::optional<NativePublicationIntent>(*intent):std::nullopt;
    w.publication_plan.reset();
    w.abandonment.reset();
    const auto& s=c->snapshot.selection;w.base_checkpoint=s.checkpoint;w.base_checkpoint_object_uuid=s.checkpoint_object_uuid;
    w.base_checkpoint_sha256=s.checkpoint_sha256;w.base_checkpoint_generation=s.checkpoint_generation;w.base_root_set_generation=s.root_set_generation;
    auto images=EncodePair(*c,w);const auto encoded=DecodeNativePublicationWatermark(images[0]);Backend(encoded.error);Require(encoded.ok(),E::image_failure);
    auto impl=std::make_unique<NativePublicationLease::Impl>();impl->snapshot={s,*encoded.state,encoded.state_sha256};impl->context=std::move(c);
    auto lease=std::unique_ptr<NativePublicationLease>(new NativePublicationLease(std::move(impl)));
    std::vector<byte> scratch(lease->impl_->context->zero.bootstrap.page_size_bytes);
    Publish(*lease->impl_->context,images,scratch);
    return {E::none,std::move(lease)};
  }catch(E e){return {e,{}};}catch(const std::bad_alloc&){return {E::resource_exhausted,{}};}
   catch(const std::length_error&){return {E::resource_exhausted,{}};}catch(...){return {E::io_failure,{}};}
}
NativePublicationReservation ResumeNativePublicationGenerationOnOpenDevices(const Uuid& db,
    const std::vector<disk::NativeFilespaceDevice>& devices,const Uuid& primary,
    const NativePublicationSnapshot& expected,const Uuid& operation,const NativePublicationIntent& intent,u64 budget) noexcept {
  try {
    auto c=Prepare(db,devices,primary,budget,true,false);
    Require(SameBase(expected,c->snapshot),E::stale_base);
    const auto& w=c->snapshot.watermark;
    Require(w.intent&&!w.abandonment&&w.watermark>c->snapshot.selection.checkpoint_generation,E::invalid_request);
    Require(w.operation_uuid==operation&&*w.intent==intent,E::request_mismatch);
    auto images=EncodePair(*c,w);
    auto impl=std::make_unique<NativePublicationLease::Impl>();impl->snapshot=c->snapshot;impl->context=std::move(c);
    auto lease=std::unique_ptr<NativePublicationLease>(new NativePublicationLease(std::move(impl)));
    std::vector<byte> scratch(lease->impl_->context->zero.bootstrap.page_size_bytes);
    Publish(*lease->impl_->context,images,scratch);
    return {E::none,std::move(lease)};
  }catch(E e){return {e,{}};}catch(const std::bad_alloc&){return {E::resource_exhausted,{}};}
   catch(const std::length_error&){return {E::resource_exhausted,{}};}catch(...){return {E::io_failure,{}};}
}
NativePublicationInspection AbandonNativeMetadataPublicationOnOpenDevices(
    const Uuid& database,const std::vector<disk::NativeFilespaceDevice>& devices,const Uuid& primary,
    const NativePublicationSnapshot& expected,const Uuid& operation,const NativePublicationIntent& intent,
    const Uuid& resolution,u64 budget) noexcept {
  try {
    Require(V7(operation)&&V7(resolution)&&resolution!=operation&&intent.recovery_profile==1&&
      !expected.watermark.abandonment&&expected.watermark.operation_uuid==operation&&
      expected.watermark.intent&&*expected.watermark.intent==intent,E::invalid_request);
    const auto file=std::find_if(devices.begin(),devices.end(),[&](const auto& f){return f.filespace_uuid==primary;});
    Require(file!=devices.end(),E::invalid_device);const auto* profile=disk::FindCanonicalFilespacePageProfile(file->page_size_profile_uuid);Require(profile,E::invalid_device);
    const u64 size=profile->page_size_bytes;u64 used=0;
    const auto charge=[&](u64 count,u64 unit=1){Require(unit&&count<=(budget-used)/unit,E::resource_exhausted);used+=count*unit;};
    charge(20,size);for(const auto& f:devices)if(f.filespace_uuid!=primary){const auto* p=disk::FindCanonicalFilespacePageProfile(f.page_size_profile_uuid);Require(p,E::invalid_device);charge(4,p->page_size_bytes);}
    auto c=Prepare(database,devices,primary,budget-used+6*size,true,false);charge(c->bound.retained_image_bytes);
    const auto& w=c->snapshot.watermark;
    Require(w.intent&&*w.intent==intent&&w.operation_uuid==operation,E::request_mismatch);
    Require(w.watermark>c->snapshot.selection.checkpoint_generation,E::invalid_request);
    auto next=w;
    if(w.abandonment){
      Require(w.abandonment->resolution_uuid==resolution,E::request_mismatch);
      auto pending=c->snapshot;pending.watermark.abandonment.reset();auto encoded=EncodeNativePublicationWatermark(pending.watermark);
      Backend(encoded.error);Require(encoded.ok(),E::image_failure);pending.state_sha256=encoded.state_sha256;
      Require(SameBase(expected,pending),E::stale_base);
    }else{
      Require(SameBase(expected,c->snapshot),E::stale_base);
      next.abandonment=NativePublicationWatermark::Abandonment{resolution,c->snapshot.state_sha256};
    }
    auto images=EncodePair(*c,next);const auto decoded=DecodeNativePublicationWatermark(images[0]);Backend(decoded.error);Require(decoded.ok(),E::image_failure);
    NativePublicationSnapshot planned{c->snapshot.selection,*decoded.state,decoded.state_sha256};std::vector<byte> scratch(size);
    Publish(*c,images,scratch);
    auto final=Prepare(database,devices,primary,budget-used+6*size,true,false);charge(final->bound.retained_image_bytes);
    Require(SameBase(planned,final->snapshot)&&final->bytes==images,E::binding_mismatch);
    return {E::none,std::move(final->snapshot)};
  }catch(E e){return {e,{}};}catch(const std::bad_alloc&){return {E::resource_exhausted,{}};}
   catch(const std::length_error&){return {E::resource_exhausted,{}};}catch(...){return {E::io_failure,{}};}
}
NativePublicationInspection InstallNativeManagementPublicationOnLease(NativePublicationLease& lease,
    const NativePublicationPlan& plan,const std::vector<byte>& checkpoint,
    const std::vector<std::vector<byte>>& supplied_extent,u64 budget) noexcept {
  if(plan.control_bundle)return {E::invalid_request,{}};
  try {
    Require(!lease.impl_->installation_ambiguous,E::stale_base);
    const u64 size=plan.header.page_size_bytes;
    Require(size&&budget>=6*size&&checkpoint.size()<=(budget-6*size)/2,E::resource_exhausted);
    u64 allowance=6*size+2*checkpoint.size();
    if(plan.management_extent){const auto& r=*plan.management_extent;
      Require(r.page_count<=(budget-allowance)/(3*size),E::resource_exhausted);allowance+=3*u64{r.page_count}*size;
      Require(r.aggregate_bytes<=(budget-allowance)/4,E::resource_exhausted);allowance+=4*u64{r.aggregate_bytes};}
    const auto backend=[](NativePublicationPlanError e){
      if(e==NativePublicationPlanError::hash_failure)throw E::hash_failure;
      if(e==NativePublicationPlanError::resource_exhausted)throw E::resource_exhausted;
      if(e==NativePublicationPlanError::cluster_requires_authority)throw E::cluster_requires_authority;
      Require(e==NativePublicationPlanError::none,E::binding_mismatch);
    };
    backend(BindNativePublicationPlanToLease(plan,lease,checkpoint));
    backend(BindNativePublicationPlanToManagementExtent(plan,supplied_extent,budget));
    auto extent=supplied_extent;
    auto image=EncodeNativePublicationPlan(plan);backend(image.error);
    const auto& held=lease.impl_->snapshot;const auto& owned=*lease.impl_->context;
    auto c=Prepare(held.watermark.header.database_uuid,owned.devices,
      owned.zero.bootstrap.filespace_uuid,budget-allowance,true,false);
    Require(SameBase(held,c->snapshot),E::stale_base);
    const auto& original=c->snapshot.watermark;
    Require(original.intent&&original.watermark>c->snapshot.selection.checkpoint_generation,E::invalid_request);
    Require(plan.header.filespace_uuid==c->zero.bootstrap.filespace_uuid&&
      plan.header.page_size_profile_uuid==c->zero.bootstrap.page_size_profile_uuid&&
      size==c->zero.bootstrap.page_size_bytes,E::invalid_request);
    const auto require_free=[&](u64 page_number){
      for(const auto& root:c->zero.roots)if(root.filespace_uuid==c->zero.bootstrap.filespace_uuid)Require(root.page_number!=page_number,E::allocation_mismatch);
      bool free=false;for(const auto& allocation:c->bound.allocation.pages){const auto& m=*allocation.map;
        if(page_number<m.first_page||page_number-m.first_page>=m.states.size())continue;
        free=m.states[page_number-m.first_page]==page::NativeAllocationState::free&&
          std::none_of(m.records.begin(),m.records.end(),[&](const auto& r){return r.page_number==page_number;});break;}
      Require(free,E::allocation_mismatch);
    };
    require_free(plan.header.page_number);
    for(std::size_t i=0;i<extent.size();++i)require_free(plan.management_extent->first.page_number+i);
    std::vector<byte> preimage(size),scratch(size);
    const auto read_plan=[&](std::vector<byte>& into){const auto r=c->primary->ReadAt(plan.header.page_number*size,into.data(),into.size());Require(r.ok()&&r.bytes_transferred==into.size(),E::io_failure);};
    read_plan(preimage);
    if(!original.publication_plan)Require(std::all_of(preimage.begin(),preimage.end(),[](byte b){return b==0;}),E::preimage_changed);
    std::vector<std::vector<byte>> extent_preimages(extent.size());
    const auto read_extent=[&](std::size_t i,std::vector<byte>& into){const auto r=c->primary->ReadAt((plan.management_extent->first.page_number+i)*size,into.data(),into.size());Require(r.ok()&&r.bytes_transferred==into.size(),E::io_failure);};
    for(std::size_t i=0;i<extent.size();++i){auto& before=extent_preimages[i];before.resize(size);read_extent(i,before);
      if(!original.publication_plan)Require(std::all_of(before.begin(),before.end(),[](byte b){return b==0;}),E::preimage_changed);}
    auto next=original;
    next.publication_plan=NativePublicationWatermark::PlanAnchor{
      {plan.header.filespace_uuid,plan.header.page_number,plan.header.page_generation,plan.header.page_size_profile_uuid},
      plan.object_uuid,image.sha256,plan.reservation_state_sha256};
    auto images=EncodePair(*c,next);auto decoded=DecodeNativePublicationWatermark(images[0]);Backend(decoded.error);Require(decoded.ok(),E::image_failure);
    NativePublicationSnapshot snapshot{c->snapshot.selection,*decoded.state,decoded.state_sha256};
    std::vector<byte>().swap(decoded.bytes);
    static_assert(std::is_nothrow_copy_assignable_v<NativePublicationSnapshot>);
    for(unsigned i=0;i<2;++i){auto& slot=c->decoded[i];slot.error=NativePublicationWatermarkError::none;
      slot.state=next;slot.state->header=c->headers[i];slot.state_sha256=snapshot.state_sha256;}
    c->snapshot=snapshot;c->stable=true;
    // The durable anchor owns the intended free page before its first byte
    // changes. Recovery of metadata alone is not plan-installation success.
    lease.impl_->installation_ambiguous=true;
    Publish(*c,images,scratch);
    read_plan(scratch);Require(scratch==preimage,E::preimage_changed);
    if(preimage!=image.bytes){const auto r=c->primary->WriteAt(plan.header.page_number*size,image.bytes.data(),image.bytes.size());Require(r.ok()&&r.bytes_transferred==image.bytes.size(),E::io_failure);}
    Require(c->primary->Sync().ok(),E::io_failure);read_plan(scratch);Require(scratch==image.bytes,E::readback_mismatch);
    for(std::size_t left=extent.size();left;--left){const auto i=left-1;read_extent(i,scratch);Require(scratch==extent_preimages[i],E::preimage_changed);
      if(scratch!=extent[i]){const auto r=c->primary->WriteAt((plan.management_extent->first.page_number+i)*size,extent[i].data(),extent[i].size());Require(r.ok()&&r.bytes_transferred==extent[i].size(),E::io_failure);}}
    if(!extent.empty()){Require(c->primary->Sync().ok(),E::io_failure);
      for(std::size_t i=0;i<extent.size();++i){read_extent(i,scratch);Require(scratch==extent[i],E::readback_mismatch);}}
    c->bytes=std::move(images);
    lease.impl_->snapshot=snapshot;lease.impl_->context=std::move(c);
    lease.impl_->installation_ambiguous=false;
    return {E::none,std::move(snapshot)};
  }catch(E e){return {e,{}};}catch(const std::bad_alloc&){return {E::resource_exhausted,{}};}
   catch(const std::length_error&){return {E::resource_exhausted,{}};}catch(...){return {E::io_failure,{}};}
}
NativePublicationInspection InstallNativePublicationPlanOnLease(NativePublicationLease& lease,
    const NativePublicationPlan& plan,const std::vector<byte>& checkpoint,u64 budget) noexcept {
  if(plan.management_extent)return {E::invalid_request,{}};
  return InstallNativeManagementPublicationOnLease(lease,plan,checkpoint,{},budget);
}
namespace {
using Bytes=std::vector<byte>;
using Pages=std::vector<Bytes>;
void ControlPlanError(NativePublicationPlanError e){if(e==NativePublicationPlanError::none)return;
  throw e==NativePublicationPlanError::hash_failure?E::hash_failure:e==NativePublicationPlanError::resource_exhausted?E::resource_exhausted:e==NativePublicationPlanError::cluster_requires_authority?E::cluster_requires_authority:E::binding_mismatch;}
void ControlBundleError(NativeManagementControlBundleError e){if(e==NativeManagementControlBundleError::none)return;
  throw e==NativeManagementControlBundleError::hash_failure?E::hash_failure:e==NativeManagementControlBundleError::resource_exhausted?E::resource_exhausted:e==NativeManagementControlBundleError::io_failure?E::io_failure:e==NativeManagementControlBundleError::cluster_requires_authority?E::cluster_requires_authority:e==NativeManagementControlBundleError::encrypted_requires_authority?E::encrypted_requires_authority:E::binding_mismatch;}
void ControlCheckpointError(NativeCheckpointError e){if(e==NativeCheckpointError::none)return;
  throw e==NativeCheckpointError::hash_failure?E::hash_failure:e==NativeCheckpointError::resource_exhausted?E::resource_exhausted:E::checkpoint_failure;}
void ControlExtentError(NativeManagementExtentError e){if(e==NativeManagementExtentError::none)return;
  throw e==NativeManagementExtentError::hash_failure?E::hash_failure:e==NativeManagementExtentError::resource_exhausted?E::resource_exhausted:e==NativeManagementExtentError::io_failure?E::io_failure:e==NativeManagementExtentError::cluster_requires_authority?E::cluster_requires_authority:e==NativeManagementExtentError::encrypted_requires_authority?E::encrypted_requires_authority:E::binding_mismatch;}
auto ControlHash(const Bytes& b){const auto hash=core::hash::ComputeSha256Digest(b);Require(hash.ok(),E::hash_failure);return hash.digest;}
auto ControlRef(const disk::NativeCommonPageHeader& h){return disk::NativePageReference{h.filespace_uuid,h.page_number,h.page_generation,h.page_size_profile_uuid};}
u64 BundleAllowance(const NativeManagementControlBundleRoot& r,u64 size,u64 budget){
  Require(budget>=2*size,E::resource_exhausted);u64 total=2*size;
  Require(r.page_count<=(budget-total)/size,E::resource_exhausted);total+=r.page_count*size;
  Require(r.map_count<=(budget-total)/(4*size),E::resource_exhausted);return total+4*r.map_count*size;
}
u64 ExtentAllowance(const NativeManagementExtentRoot& r,u64 size,u64 budget){
  Require(budget>=2*size,E::resource_exhausted);u64 total=2*size;
  Require(r.page_count<=(budget-total)/size,E::resource_exhausted);total+=u64{r.page_count}*size;
  Require(r.aggregate_bytes<=(budget-total)/4,E::resource_exhausted);return total+4*u64{r.aggregate_bytes};
}
}
NativePublicationInspection InstallNativeManagementControlGraphOnLease(NativePublicationLease& lease,
    const NativePublicationPlan& supplied_plan,const Bytes& supplied_checkpoint,
    const Pages& supplied_extent,const Pages& supplied_bundle,u64 budget) noexcept {
  try {
    Require(!lease.impl_->installation_ambiguous,E::stale_base);const auto plan=supplied_plan;
    Require(plan.control_bundle&&plan.management_extent,E::invalid_request);
    const auto* profile=disk::FindCanonicalFilespacePageProfile(plan.header.page_size_profile_uuid);
    Require(profile&&profile->page_size_bytes==plan.header.page_size_bytes,E::invalid_request);const u64 size=profile->page_size_bytes;
    u64 allowance=0;const auto charge=[&](u64 count,u64 unit){Require(count<=(budget-allowance)/unit,E::resource_exhausted);allowance+=count*unit;};
    charge(20,size);charge(plan.management_extent->page_count,4*size);charge(plan.management_extent->aggregate_bytes,4);charge(plan.control_bundle->page_count,4*size);charge(plan.control_bundle->map_count,10*size);
    // Reject inconsistent caller lengths before copying caller-owned images.
    Require(supplied_checkpoint.size()==size&&supplied_extent.size()==plan.management_extent->page_count&&supplied_bundle.size()==plan.control_bundle->page_count,E::invalid_request);
    for(const auto& bytes:supplied_extent)Require(bytes.size()==size,E::invalid_request);
    for(const auto& bytes:supplied_bundle)Require(bytes.size()==size,E::invalid_request);
    Bytes checkpoint=supplied_checkpoint;Pages extent=supplied_extent,bundle=supplied_bundle;
    ControlPlanError(BindNativePublicationPlanToLease(plan,lease,checkpoint));
    auto image=EncodeNativePublicationPlan(plan);ControlPlanError(image.error);
    auto reconstruction=DecodeNativeManagementControlBundle(bundle,*plan.control_bundle,plan.header.database_uuid,plan.bootstrap_uuid,BundleAllowance(*plan.control_bundle,size,budget));ControlBundleError(reconstruction.error);
    const auto& held=lease.impl_->snapshot;const auto& owned=*lease.impl_->context;
    auto c=Prepare(held.watermark.header.database_uuid,owned.devices,owned.zero.bootstrap.filespace_uuid,budget-allowance,true,false);
    Require(SameBase(held,c->snapshot),E::stale_base);const auto& original=c->snapshot.watermark;
    Require(original.intent&&original.watermark>c->snapshot.selection.checkpoint_generation,E::invalid_request);
    Require(plan.header.filespace_uuid==c->zero.bootstrap.filespace_uuid&&plan.header.page_size_profile_uuid==c->zero.bootstrap.page_size_profile_uuid,E::invalid_request);
    Require(c->bound.allocation.pages.size()==reconstruction.allocation_images.size(),E::allocation_mismatch);
    const auto base=EncodeNativeCheckpointRoot(*c->bound.checkpoint_inventory.checkpoint);ControlCheckpointError(base.error);Require(ControlHash(base.bytes)==c->bound.checkpoint_inventory.checkpoint_sha256,E::binding_mismatch);
    Pages before_maps;before_maps.reserve(c->bound.allocation.pages.size());for(const auto& p:c->bound.allocation.pages)before_maps.push_back(p.bytes);
    const auto delta=ValidateNativeManagementControlAllocation(base.bytes,checkpoint,image.bytes,extent,before_maps,reconstruction.allocation_images,budget,bundle);
    if(delta!=NativeManagementControlAllocationError::none)throw delta==NativeManagementControlAllocationError::resource_exhausted?E::resource_exhausted:delta==NativeManagementControlAllocationError::hash_failure?E::hash_failure:delta==NativeManagementControlAllocationError::cluster_requires_authority?E::cluster_requires_authority:E::allocation_mismatch;
    struct Artifact {u64 page=0;Bytes bytes,before;};std::vector<Artifact> artifacts;artifacts.reserve(2+extent.size()+bundle.size()+reconstruction.allocation_images.size());
    artifacts.push_back({plan.header.page_number,std::move(image.bytes),{}});const std::size_t extent_start=artifacts.size();
    for(std::size_t i=0;i<extent.size();++i)artifacts.push_back({plan.management_extent->first.page_number+i,std::move(extent[i]),{}});
    const std::size_t bundle_start=artifacts.size();
    for(std::size_t i=0;i<bundle.size();++i)artifacts.push_back({plan.control_bundle->first.page_number+i,std::move(bundle[i]),{}});
    const std::size_t map_start=artifacts.size();
    for(auto& raw:reconstruction.allocation_images){const auto h=disk::DecodeNativeCommonPageHeader(raw.data(),128);Require(h.ok(),E::image_failure);artifacts.push_back({h.header->page_number,std::move(raw),{}});}const std::size_t checkpoint_start=artifacts.size();
    artifacts.push_back({plan.target_checkpoint.page_number,std::move(checkpoint),{}});Bytes scratch(size);
    const auto read=[&](u64 page,Bytes& into){const auto io=c->primary->ReadAt(page*size,into.data(),into.size());Require(io.ok()&&io.bytes_transferred==into.size(),E::io_failure);};
    for(auto& target:artifacts){for(const auto& root:c->zero.roots)if(root.filespace_uuid==c->zero.bootstrap.filespace_uuid)Require(root.page_number!=target.page,E::allocation_mismatch);
      auto map=std::upper_bound(c->bound.allocation.pages.begin(),c->bound.allocation.pages.end(),target.page,[](u64 n,const auto& p){return n<p.map->first_page;});Require(map!=c->bound.allocation.pages.begin(),E::allocation_mismatch);--map;
      const auto& m=*map->map;Require(target.page-m.first_page<m.states.size()&&m.states[target.page-m.first_page]==page::NativeAllocationState::free,E::allocation_mismatch);
      const auto record=std::lower_bound(m.records.begin(),m.records.end(),target.page,[](const auto& r,u64 n){return r.page_number<n;});Require(record==m.records.end()||record->page_number!=target.page,E::allocation_mismatch);
      target.before.resize(size);read(target.page,target.before);if(!original.publication_plan)Require(std::all_of(target.before.begin(),target.before.end(),[](byte b){return !b;}),E::preimage_changed);
    }
    auto next=original;next.publication_plan=NativePublicationWatermark::PlanAnchor{ControlRef(plan.header),plan.object_uuid,image.sha256,plan.reservation_state_sha256};
    auto images=EncodePair(*c,next);auto decoded=DecodeNativePublicationWatermark(images[0]);Backend(decoded.error);Require(decoded.ok(),E::image_failure);
    NativePublicationSnapshot snapshot{c->snapshot.selection,*decoded.state,decoded.state_sha256};Bytes().swap(decoded.bytes);
    for(unsigned i=0;i<2;++i){auto& slot=c->decoded[i];slot.error=NativePublicationWatermarkError::none;slot.state=next;slot.state->header=c->headers[i];slot.state_sha256=snapshot.state_sha256;}
    c->snapshot=snapshot;c->stable=true;
    const auto install=[&](std::size_t first,std::size_t end){for(std::size_t n=end;n>first;--n){auto& target=artifacts[n-1];read(target.page,scratch);Require(scratch==target.before,E::preimage_changed);
        if(scratch!=target.bytes){const auto io=c->primary->WriteAt(target.page*size,target.bytes.data(),target.bytes.size());Require(io.ok()&&io.bytes_transferred==target.bytes.size(),E::io_failure);}}
      Require(c->primary->Sync().ok(),E::io_failure);for(std::size_t n=first;n<end;++n){read(artifacts[n].page,scratch);Require(scratch==artifacts[n].bytes,E::readback_mismatch);}};
    lease.impl_->installation_ambiguous=true;Publish(*c,images,scratch);
    install(0,extent_start);install(extent_start,bundle_start);install(bundle_start,map_start);install(map_start,checkpoint_start);install(checkpoint_start,artifacts.size());
    c->bytes=std::move(images);lease.impl_->snapshot=snapshot;lease.impl_->context=std::move(c);lease.impl_->installation_ambiguous=false;return {E::none,std::move(snapshot)};
  }catch(E e){return {e,{}};}catch(const std::bad_alloc&){return {E::resource_exhausted,{}};}catch(const std::length_error&){return {E::resource_exhausted,{}};}catch(...){return {E::io_failure,{}};}
}
NativePublicationInspection ResumeNativeManagementControlGraphOnLease(NativePublicationLease& lease,u64 budget) noexcept {
  try {
    Require(!lease.impl_->installation_ambiguous,E::stale_base);const auto& context=*lease.impl_->context;const auto& held=lease.impl_->snapshot;const auto& anchor=held.watermark.publication_plan;
    Require(anchor&&held.watermark.intent&&!held.watermark.abandonment&&held.watermark.watermark>held.selection.checkpoint_generation,E::invalid_request);
    const u64 size=context.zero.bootstrap.page_size_bytes;Require(budget>=6*size,E::resource_exhausted);u64 allowance=6*size;
    Require(anchor->page.page_number<context.zero.total_pages,E::binding_mismatch);Bytes raw(size);const auto io=context.primary->ReadAt(anchor->page.page_number*size,raw.data(),raw.size());Require(io.ok()&&io.bytes_transferred==raw.size(),E::io_failure);
    auto image=DecodeNativePublicationPlan(raw);ControlPlanError(image.error);const auto& plan=*image.plan;
    Require(plan.control_bundle&&plan.management_extent&&ControlRef(plan.header)==anchor->page&&plan.object_uuid==anchor->object_uuid&&image.sha256==anchor->sha256&&plan.header.database_uuid==context.zero.bootstrap.database_uuid&&plan.bootstrap_uuid==context.zero.page_uuid,E::binding_mismatch);
    const u64 extent_allowance=ExtentAllowance(*plan.management_extent,size,budget-allowance);allowance+=extent_allowance;
    const u64 bundle_allowance=BundleAllowance(*plan.control_bundle,size,budget-allowance);allowance+=bundle_allowance;
    const disk::NativeFilespaceDevice file{context.zero.bootstrap.filespace_uuid,context.zero.bootstrap.page_size_profile_uuid,context.primary};
    auto record=ReadNativeManagementExtentFromOpenDevice(file,*plan.management_extent,plan.header.database_uuid,plan.bootstrap_uuid,extent_allowance);
    ControlExtentError(record.error);
    auto extent=EncodeNativeManagementExtent(*record.record,plan.management_extent->object_uuid,record.page_headers,extent_allowance);
    ControlExtentError(extent.error);
    Require(extent.root==plan.management_extent,E::binding_mismatch);
    auto contents=ReadNativeManagementControlBundleFromOpenDevice(file,*plan.control_bundle,plan.header.database_uuid,plan.bootstrap_uuid,bundle_allowance);ControlBundleError(contents.error);
    auto bundle=EncodeNativeManagementControlBundle(contents.allocation_images,plan.header.database_uuid,plan.bootstrap_uuid,plan.control_bundle->object_uuid,plan.operation_uuid,contents.page_headers,bundle_allowance);ControlBundleError(bundle.error);Require(bundle.root==plan.control_bundle,E::binding_mismatch);
    std::optional<page::NativeAllocationMap> first;std::optional<page::NativeAllocationRecord> target_record;
    for(const auto& b:contents.allocation_images){auto map=page::DecodeNativeAllocationMap(b);if(!map.ok())throw map.error==page::NativeAllocationError::resource_exhausted?E::resource_exhausted:map.error==page::NativeAllocationError::hash_failure?E::hash_failure:E::allocation_mismatch;
      const auto& m=*map.map;const u64 n=plan.target_checkpoint.page_number;if(n>=m.first_page&&n-m.first_page<m.states.size()){const auto r=std::lower_bound(m.records.begin(),m.records.end(),n,[](const auto& r,u64 v){return r.page_number<v;});Require(m.states[n-m.first_page]==page::NativeAllocationState::allocated&&r!=m.records.end()&&r->page_number==n,E::allocation_mismatch);target_record=*r;}
      if(!first)first=std::move(map.map);
    }
    Require(first&&target_record&&target_record->page_type==0x300&&target_record->owner_uuid==plan.target_checkpoint_object_uuid&&target_record->creator_operation_uuid==plan.operation_uuid&&target_record->creator_transaction_uuid.is_nil()&&!target_record->creator_local_transaction_id&&target_record->page_generation==plan.reserved_generation,E::allocation_mismatch);
    auto cp=*context.bound.checkpoint_inventory.checkpoint;cp.header=plan.header;cp.header.page_type=0x300;cp.header.page_number=plan.target_checkpoint.page_number;cp.header.page_generation=plan.target_checkpoint.page_generation;cp.header.page_uuid=target_record->page_uuid;cp.object_uuid=plan.target_checkpoint_object_uuid;cp.creator_transaction_uuid={};cp.creator_local_transaction_id=0;cp.creator_operation_uuid=plan.operation_uuid;cp.checkpoint_generation=plan.reserved_generation;cp.root_set_generation=plan.target_root_set_generation;cp.predecessor=plan.base_checkpoint;cp.predecessor_sha256=plan.base_checkpoint_sha256;
    auto allocation=std::find_if(cp.roots.begin(),cp.roots.end(),[](const auto& r){return r.role==4;});Require(allocation!=cp.roots.end(),E::allocation_mismatch);*allocation={4,3,ControlRef(first->header),first->object_uuid,ControlHash(contents.allocation_images.front())};
    const NativeCheckpointRootReference head{16,0x500,ControlRef(plan.header),plan.object_uuid,image.sha256};auto history=std::find_if(cp.roots.begin(),cp.roots.end(),[](const auto& r){return r.role==16;});if(history==cp.roots.end())cp.roots.push_back(head);else *history=head;
    const auto target=EncodeNativeCheckpointRoot(cp);ControlCheckpointError(target.error);ControlPlanError(BindNativePublicationPlanToLease(plan,lease,target.bytes));
    return InstallNativeManagementControlGraphOnLease(lease,plan,target.bytes,extent.pages,bundle.pages,budget-allowance);
  }catch(E e){return {e,{}};}catch(const std::bad_alloc&){return {E::resource_exhausted,{}};}catch(const std::length_error&){return {E::resource_exhausted,{}};}catch(...){return {E::io_failure,{}};}
}
NativePublicationInspection PublishNativeManagementControlGraphOnLease(NativePublicationLease& lease,u64 budget) noexcept {
  try {
    Require(!lease.impl_->installation_ambiguous,E::stale_base);
    const auto& held=lease.impl_->snapshot;const auto& owned=*lease.impl_->context;
    const auto database=owned.zero.bootstrap.database_uuid,primary=owned.zero.bootstrap.filespace_uuid;
    const u64 size=owned.zero.bootstrap.page_size_bytes;u64 used=0;
    const auto charge=[&](u64 count,u64 unit=1){Require(unit&&count<=(budget-used)/unit,E::resource_exhausted);used+=count*unit;};
    charge(28,size);
    for(const auto& file:owned.devices)if(file.filespace_uuid!=primary){const auto* profile=disk::FindCanonicalFilespacePageProfile(file.page_size_profile_uuid);Require(profile,E::invalid_device);charge(2,profile->page_size_bytes);}
    auto c=Prepare(database,owned.devices,primary,budget-used+6*size,true,false);
    charge(c->bound.retained_image_bytes);Require(SameBase(held,c->snapshot),E::stale_base);
    const auto& w=c->snapshot.watermark;
    Require(w.intent&&!w.abandonment&&w.publication_plan&&w.watermark>c->snapshot.selection.checkpoint_generation,E::invalid_request);
    const auto& anchor=*w.publication_plan;
    Require(anchor.page.filespace_uuid==primary&&anchor.page.page_size_profile_uuid==c->zero.bootstrap.page_size_profile_uuid&&anchor.page.page_number<c->zero.total_pages,E::binding_mismatch);
    Bytes raw(size);const auto read=c->primary->ReadAt(anchor.page.page_number*size,raw.data(),raw.size());Require(read.ok()&&read.bytes_transferred==raw.size(),E::io_failure);
    auto image=DecodeNativePublicationPlan(raw);ControlPlanError(image.error);const auto& plan=*image.plan;
    Require(plan.base_selection_generation&&ControlRef(plan.header)==anchor.page&&plan.object_uuid==anchor.object_uuid&&image.sha256==anchor.sha256&&plan.header.database_uuid==database&&plan.bootstrap_uuid==c->zero.page_uuid,E::binding_mismatch);
    const auto& target=plan.target_checkpoint;Require(target.filespace_uuid==primary&&target.page_number<c->zero.total_pages,E::binding_mismatch);
    const disk::FilespaceRootReference target_ref{9,0x300,target.filespace_uuid,target.page_number,target.page_generation,target.page_size_profile_uuid,plan.target_checkpoint_object_uuid};
    const auto checkpoint=ReadNativeCheckpointRootFromOpenDevice(*c->primary,database,target_ref);
    if(!checkpoint.ok())throw checkpoint.error==NativeCheckpointError::io_failure?E::io_failure:checkpoint.error==NativeCheckpointError::encrypted_requires_crypto_authority?E::encrypted_requires_authority:checkpoint.error==NativeCheckpointError::hash_failure?E::hash_failure:checkpoint.error==NativeCheckpointError::resource_exhausted?E::resource_exhausted:E::checkpoint_failure;
    ControlPlanError(BindNativePublicationPlanToLease(plan,lease,checkpoint.bytes));
    const auto target_sha=ControlHash(checkpoint.bytes);
    const NativeManagementCheckpointAnchor graph_anchor{target,plan.target_checkpoint_object_uuid,target_sha,plan.reserved_generation,plan.target_root_set_generation,plan.timeline_uuid};
    const auto proof=ReadNativeManagementControlGraphFromOpenDevices(database,c->devices,primary,graph_anchor,budget-used);
    if(!proof.ok()){using G=NativeManagementControlAuthorityError;throw proof.error==G::resource_exhausted?E::resource_exhausted:proof.error==G::hash_failure?E::hash_failure:proof.error==G::io_failure?E::io_failure:proof.error==G::encrypted_requires_authority?E::encrypted_requires_authority:proof.error==G::cluster_requires_authority?E::cluster_requires_authority:E::allocation_mismatch;}
    charge(proof.verified_image_bytes);
    std::array<Bytes,2> images;std::array<u64,2> pages{};NativePublicationSnapshot expected=c->snapshot;
    for(unsigned i=0;i<2;++i){auto old=DecodeNativeCheckpointSelection(c->bound.slots[i]);
      if(!old.ok())throw old.error==NativeCheckpointSelectionError::resource_exhausted?E::resource_exhausted:old.error==NativeCheckpointSelectionError::hash_failure?E::hash_failure:E::binding_mismatch;
      auto next=*old.selection;pages[i]=next.header.page_number;next.selection_generation=*plan.base_selection_generation+1;next.previous_selection_generation=*plan.base_selection_generation;
      next.previous_checkpoint=plan.base_checkpoint;next.previous_checkpoint_object_uuid=plan.base_checkpoint_object_uuid;next.previous_checkpoint_sha256=plan.base_checkpoint_sha256;
      next.publication_uuid=plan.operation_uuid;next.checkpoint=target;next.checkpoint_object_uuid=plan.target_checkpoint_object_uuid;next.checkpoint_sha256=target_sha;next.checkpoint_generation=plan.reserved_generation;next.root_set_generation=plan.target_root_set_generation;
      auto encoded=EncodeNativeCheckpointSelection(next);if(!encoded.ok())throw encoded.error==NativeCheckpointSelectionError::resource_exhausted?E::resource_exhausted:encoded.error==NativeCheckpointSelectionError::hash_failure?E::hash_failure:E::image_failure;
      images[i]=std::move(encoded.bytes);if(!i)expected.selection=next;
    }
    Bytes scratch(size);
    const auto verify=[&](unsigned i,const Bytes& bytes,E mismatch){const auto io=c->primary->ReadAt(pages[i]*size,scratch.data(),scratch.size());Require(io.ok()&&io.bytes_transferred==scratch.size(),E::io_failure);Require(scratch==bytes,mismatch);};
    for(unsigned i=0;i<2;++i)verify(i,c->bound.slots[i],E::preimage_changed);
    lease.impl_->installation_ambiguous=true;
    for(const auto& file:c->devices)if(!file.device->read_only())Require(file.device->Sync().ok(),E::io_failure);
    for(unsigned i=0;i<2;++i){const auto io=c->primary->WriteAt(pages[i]*size,images[i].data(),images[i].size());Require(io.ok()&&io.bytes_transferred==images[i].size(),E::io_failure);
      Require(c->primary->Sync().ok(),E::io_failure);verify(i,images[i],E::readback_mismatch);}
    auto final=Prepare(database,c->devices,primary,budget-used+6*size,true,false);charge(final->bound.retained_image_bytes);
    Require(SameBase(expected,final->snapshot)&&final->bound.slots==images,E::binding_mismatch);
    const auto snapshot=final->snapshot;lease.impl_->snapshot=snapshot;lease.impl_->context=std::move(final);lease.impl_->installation_ambiguous=false;
    return {E::none,snapshot};
  }catch(E e){return {e,{}};}catch(const std::bad_alloc&){return {E::resource_exhausted,{}};}catch(const std::length_error&){return {E::resource_exhausted,{}};}catch(...){return {E::io_failure,{}};}
}
} // namespace scratchbird::storage::database
