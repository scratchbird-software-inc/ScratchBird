// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_publication_coordinator.hpp"
#include "native_publication_plan.hpp"
#include "native_management_control_allocation.hpp"
#include "native_management_control_authority.hpp"
#include "disk_device.hpp"
#include "hash_digest_parts.hpp"
#include "transaction_inventory_validation.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <iterator>
#include <limits>
#include <map>
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
static NativePublicationInspection AbandonNativeControlPublicationOnOpenDevices(
    const Uuid& database,const std::vector<disk::NativeFilespaceDevice>& devices,const Uuid& primary,
    const NativePublicationSnapshot& expected,const Uuid& operation,const NativePublicationIntent& intent,
    const Uuid& resolution,u16 profile,u64 budget) noexcept {
  try {
    Require(V7(operation)&&V7(resolution)&&resolution!=operation&&intent.recovery_profile==profile&&
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
NativePublicationInspection AbandonNativeMetadataPublicationOnOpenDevices(
    const Uuid& database,const std::vector<disk::NativeFilespaceDevice>& devices,const Uuid& primary,
    const NativePublicationSnapshot& expected,const Uuid& operation,const NativePublicationIntent& intent,
    const Uuid& resolution,u64 budget) noexcept {
  return AbandonNativeControlPublicationOnOpenDevices(database,devices,primary,expected,operation,intent,resolution,1,budget);
}
NativePublicationInspection AbandonNativeInventoryPublicationOnOpenDevices(
    const Uuid& database,const std::vector<disk::NativeFilespaceDevice>& devices,const Uuid& primary,
    const NativePublicationSnapshot& expected,const Uuid& operation,const NativePublicationIntent& intent,
    const Uuid& resolution,u64 budget) noexcept {
  return AbandonNativeControlPublicationOnOpenDevices(database,devices,primary,expected,operation,intent,resolution,2,budget);
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
  Require(r.map_count<=(budget-total)/(4*size),E::resource_exhausted);total+=4*r.map_count*size;
  Require(r.inventory_count<=(budget-total)/(4*size),E::resource_exhausted);return total+4*r.inventory_count*size;
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
    charge(plan.control_bundle->inventory_count,14*size);
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
    Pages before_inventory;
    if(plan.intent.recovery_profile==2){charge(c->bound.retained_image_bytes,1);const auto& cp=*c->bound.checkpoint_inventory.checkpoint;
      const auto root=std::find_if(cp.roots.begin(),cp.roots.end(),[](const auto& r){return r.role==1;});Require(root!=cp.roots.end(),E::binding_mismatch);
      const disk::FilespaceRootReference ref{4,root->page_type,root->page.filespace_uuid,root->page.page_number,root->page.page_generation,root->page.page_size_profile_uuid,root->object_uuid};
      auto actual=page::ReadNativeTransactionInventoryChainFromOpenDevices(plan.header.database_uuid,c->devices,ref,budget-allowance);
      if(!actual.ok())throw actual.error==page::NativeInventoryError::resource_exhausted?E::resource_exhausted:actual.error==page::NativeInventoryError::hash_failure?E::hash_failure:actual.error==page::NativeInventoryError::io_failure?E::io_failure:actual.error==page::NativeInventoryError::encrypted_requires_crypto_authority?E::encrypted_requires_authority:E::allocation_mismatch;
      charge(actual.retained_image_bytes,1);for(const auto& raw:actual.pages)charge(raw.bytes.size(),4);before_inventory.reserve(actual.pages.size());for(auto& raw:actual.pages)before_inventory.push_back(std::move(raw.bytes));
    }
    const auto delta=ValidateNativeManagementControlAllocation(base.bytes,checkpoint,image.bytes,extent,before_maps,reconstruction.allocation_images,budget,bundle,before_inventory);
    if(delta!=NativeManagementControlAllocationError::none)throw delta==NativeManagementControlAllocationError::resource_exhausted?E::resource_exhausted:delta==NativeManagementControlAllocationError::hash_failure?E::hash_failure:delta==NativeManagementControlAllocationError::cluster_requires_authority?E::cluster_requires_authority:delta==NativeManagementControlAllocationError::encrypted_requires_authority?E::encrypted_requires_authority:E::allocation_mismatch;
    struct Artifact {u64 page=0;Bytes bytes,before;};std::vector<Artifact> artifacts;artifacts.reserve(2+extent.size()+bundle.size()+reconstruction.allocation_images.size()+reconstruction.inventory_images.size());
    artifacts.push_back({plan.header.page_number,std::move(image.bytes),{}});const std::size_t extent_start=artifacts.size();
    for(std::size_t i=0;i<extent.size();++i)artifacts.push_back({plan.management_extent->first.page_number+i,std::move(extent[i]),{}});
    const std::size_t bundle_start=artifacts.size();
    for(std::size_t i=0;i<bundle.size();++i)artifacts.push_back({plan.control_bundle->first.page_number+i,std::move(bundle[i]),{}});
    const std::size_t inventory_start=artifacts.size();
    for(auto& raw:reconstruction.inventory_images){const auto h=disk::DecodeNativeCommonPageHeader(raw.data(),128);Require(h.ok(),E::image_failure);artifacts.push_back({h.header->page_number,std::move(raw),{}});}
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
    install(0,extent_start);install(extent_start,bundle_start);install(bundle_start,inventory_start);if(inventory_start!=map_start)install(inventory_start,map_start);install(map_start,checkpoint_start);install(checkpoint_start,artifacts.size());
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
    auto bundle=EncodeNativeManagementControlBundle(contents.allocation_images,plan.header.database_uuid,plan.bootstrap_uuid,plan.control_bundle->object_uuid,plan.operation_uuid,contents.page_headers,bundle_allowance,contents.inventory_images);ControlBundleError(bundle.error);Require(bundle.root==plan.control_bundle,E::binding_mismatch);
    std::optional<page::NativeAllocationMap> first;std::optional<page::NativeAllocationRecord> target_record;
    for(const auto& b:contents.allocation_images){auto map=page::DecodeNativeAllocationMap(b);if(!map.ok())throw map.error==page::NativeAllocationError::resource_exhausted?E::resource_exhausted:map.error==page::NativeAllocationError::hash_failure?E::hash_failure:E::allocation_mismatch;
      const auto& m=*map.map;const u64 n=plan.target_checkpoint.page_number;if(n>=m.first_page&&n-m.first_page<m.states.size()){const auto r=std::lower_bound(m.records.begin(),m.records.end(),n,[](const auto& r,u64 v){return r.page_number<v;});Require(m.states[n-m.first_page]==page::NativeAllocationState::allocated&&r!=m.records.end()&&r->page_number==n,E::allocation_mismatch);target_record=*r;}
      if(!first)first=std::move(map.map);
    }
    Require(first&&target_record&&target_record->page_type==0x300&&target_record->owner_uuid==plan.target_checkpoint_object_uuid&&target_record->creator_operation_uuid==plan.operation_uuid&&target_record->creator_transaction_uuid.is_nil()&&!target_record->creator_local_transaction_id&&target_record->page_generation==plan.reserved_generation,E::allocation_mismatch);
    auto cp=*context.bound.checkpoint_inventory.checkpoint;cp.header=plan.header;cp.header.page_type=0x300;cp.header.page_number=plan.target_checkpoint.page_number;cp.header.page_generation=plan.target_checkpoint.page_generation;cp.header.page_uuid=target_record->page_uuid;cp.object_uuid=plan.target_checkpoint_object_uuid;cp.creator_transaction_uuid={};cp.creator_local_transaction_id=0;cp.creator_operation_uuid=plan.operation_uuid;cp.checkpoint_generation=plan.reserved_generation;cp.root_set_generation=plan.target_root_set_generation;cp.predecessor=plan.base_checkpoint;cp.predecessor_sha256=plan.base_checkpoint_sha256;
    auto allocation=std::find_if(cp.roots.begin(),cp.roots.end(),[](const auto& r){return r.role==4;});Require(allocation!=cp.roots.end(),E::allocation_mismatch);*allocation={4,3,ControlRef(first->header),first->object_uuid,ControlHash(contents.allocation_images.front())};
    if(plan.intent.recovery_profile==2){Require(!contents.inventory_images.empty(),E::binding_mismatch);const auto decoded=page::DecodeNativeTransactionInventoryPage(contents.inventory_images.front());
      if(!decoded.ok())throw decoded.error==page::NativeInventoryError::resource_exhausted?E::resource_exhausted:decoded.error==page::NativeInventoryError::hash_failure?E::hash_failure:E::allocation_mismatch;
      const auto& candidate=*decoded.page;auto root=std::find_if(cp.roots.begin(),cp.roots.end(),[](const auto& r){return r.role==1;});Require(root!=cp.roots.end(),E::binding_mismatch);*root={1,0x301,ControlRef(candidate.header),candidate.object_uuid,ControlHash(contents.inventory_images.front())};cp.selected_local_transaction_id=candidate.inventory.next_local_transaction_id-1;
    }
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
namespace {
struct OwnedInventoryGraph {
  NativePublicationPlan plan;
  Bytes checkpoint;
  Pages extent, bundle;
};

OwnedInventoryGraph AssembleInventory(Context& context,
    const NativeManagementOperation& supplied_record,
    const mga::LocalTransactionInventory& supplied_inventory, u64 budget,
    core::uuid::StandaloneUuidV7Issuer& issuer) {
  const auto& snapshot=context.snapshot;
  const auto& watermark=snapshot.watermark;
  const auto& zero=context.zero;
  const auto& base=*context.bound.checkpoint_inventory.checkpoint;
  Require(watermark.intent&&watermark.intent->recovery_profile==2&&
    !watermark.abandonment&&!watermark.publication_plan&&
    watermark.watermark>snapshot.selection.checkpoint_generation,E::operation_pending);
  const auto encoded_record=EncodeNativeManagementOperation(supplied_record,budget);
  if(!encoded_record.ok()) throw encoded_record.error==NativeManagementOperationError::resource_exhausted?
    E::resource_exhausted:encoded_record.error==NativeManagementOperationError::hash_failure?E::hash_failure:E::invalid_request;
  const auto& record=*encoded_record.record;
  Require(record.scope!=NativeManagementScope::cluster&&record.cluster_uuid.is_nil()&&
    !record.generation_guards[3],E::cluster_requires_authority);
  const auto& intent=*watermark.intent;
  Require(record.database_uuid==zero.bootstrap.database_uuid&&record.bootstrap_uuid==zero.page_uuid&&
    record.initiator_uuid==intent.initiator_uuid&&record.initiator_kind==intent.initiator_kind&&
    record.request_context_uuid==intent.request_context_uuid&&record.policy_snapshot_uuid==intent.policy_snapshot_uuid&&
    record.normalized_request_sha256==intent.normalized_request_sha256,E::request_mismatch);
  Require(issuer.binding().database_uuid==zero.bootstrap.database_uuid&&
    issuer.binding().policy_snapshot_uuid==record.policy_snapshot_uuid,E::request_mismatch);
  const u64 size=zero.bootstrap.page_size_bytes, payload=size-384, per_inventory=payload/72;
  const auto ceil=[](u64 n,u64 d){return n/d+(n%d!=0);};
  const u64 inventory_count=std::max<u64>(1,ceil(supplied_inventory.entries.size(),per_inventory));
  const u64 map_count=context.bound.allocation.pages.size();
  const u64 extent_count=ceil(encoded_record.bytes.size(),payload);
  u64 charged=context.bound.retained_image_bytes;
  Require(charged<=budget,E::resource_exhausted);
  const auto charge=[&](u64 count,u64 unit){Require(unit&&count<=(budget-charged)/unit,E::resource_exhausted);charged+=count*unit;};
  charge(20,size);charge(map_count,10*size);charge(inventory_count,14*size);
  charge(extent_count,4*size);charge(encoded_record.bytes.size(),4);
  // The preceding charges bound both addition and multiplication below.
  const u64 bundle_count=ceil((map_count+inventory_count)*size,payload);
  charge(bundle_count,4*size);
  Require(inventory_count<=std::numeric_limits<std::size_t>::max()&&
    bundle_count<=std::numeric_limits<std::size_t>::max(),E::resource_exhausted);
  auto inventory=supplied_inventory;
  Require(!*mga::ValidateLocalTransactionInventoryEvolution(context.bound.checkpoint_inventory.inventory,inventory),E::invalid_request);
  std::sort(inventory.entries.begin(),inventory.entries.end(),[](const auto& a,const auto& b){
    return a.identity.local_id.value<b.identity.local_id.value;});
  const auto root=[&](u16 role)->const NativeCheckpointRootReference& {
    const auto at=std::find_if(base.roots.begin(),base.roots.end(),[&](const auto& r){return r.role==role;});
    Require(at!=base.roots.end(),E::binding_mismatch);return *at;
  };
  const auto& inventory_root=root(1);
  const auto old_plan=std::find_if(base.roots.begin(),base.roots.end(),[](const auto& r){return r.role==16;});

  // Only primary free/recordless, physically zero slots may be consumed. A
  // dirty free slot is an orphan to be resolved by its owner, not free bytes.
  std::set<u64> selected;
  std::map<u64,bool> probes;
  Bytes probe(size);
  const auto allocate=[&](u64 count) {
    Require(count&&count<=zero.total_pages,E::allocation_exhausted);
    std::vector<u64> run;run.reserve(static_cast<std::size_t>(count));
    for(const auto& image:context.bound.allocation.pages) {
      const auto& map=*image.map;
      for(std::size_t i=0;i<map.states.size();++i) {
        const u64 number=map.first_page+i;
        if(!number||map.states[i]!=page::NativeAllocationState::free||selected.contains(number)) {run.clear();continue;}
        auto known=probes.find(number);
        if(known==probes.end()) {
          charge(1,size);
          Require(number<std::numeric_limits<u64>::max()/size,E::allocation_mismatch);
          const auto io=context.primary->ReadAt(number*size,probe.data(),probe.size());
          Require(io.ok()&&io.bytes_transferred==probe.size(),E::io_failure);
          const bool empty=std::all_of(probe.begin(),probe.end(),[](byte value){return !value;});
          known=probes.emplace(number,empty).first;
        }
        if(!known->second) {run.clear();continue;}
        if(!run.empty()&&number-run.back()!=1)run.clear();
        run.push_back(number);
        if(run.size()==count) {selected.insert(run.begin(),run.end());return run;}
      }
    }
    throw E::allocation_exhausted;
  };
  const auto extent_slots=allocate(extent_count),bundle_slots=allocate(bundle_count);
  const auto plan_slot=allocate(1).front(),checkpoint_slot=allocate(1).front();
  std::vector<u64> map_slots,inventory_slots;
  for(u64 i=0;i<map_count;++i)map_slots.push_back(allocate(1).front());
  for(u64 i=0;i<inventory_count;++i)inventory_slots.push_back(allocate(1).front());

  std::set<Uuid> identities;
  const auto retain=[&](const Uuid& id){if(!id.is_nil())identities.insert(id);};
  for(const auto& id:{zero.bootstrap.database_uuid,zero.bootstrap.filespace_uuid,zero.page_uuid,
    zero.bootstrap.page_size_profile_uuid,zero.creation_operation_uuid,zero.writer_identity_uuid,
    watermark.operation_uuid,base.object_uuid,base.timeline_uuid,record.uuid,record.descriptor_uuid,
    record.family_uuid,record.target_type_uuid,record.target_uuid,record.initiator_uuid,record.request_context_uuid,
    record.policy_snapshot_uuid,record.security_snapshot_uuid,record.phase_uuid,record.boundary_uuid,
    record.created_at,record.updated_at,record.terminal_at,record.resource_plan_uuid,record.lock_plan_uuid,
    record.result_uuid,record.diagnostic_uuid,record.evidence_uuid,record.metric_evidence_uuid})retain(id);
  for(const auto& r:base.roots){retain(r.object_uuid);retain(r.page.filespace_uuid);retain(r.page.page_size_profile_uuid);}
  for(const auto& image:context.bound.allocation.pages)for(const auto& r:image.map->records)
    for(const auto& id:{r.allocation_uuid,r.page_uuid,r.owner_uuid,r.creator_transaction_uuid,r.creator_operation_uuid})retain(id);
  for(const auto& page:context.bound.checkpoint_inventory.inventory_pages){retain(page.header.page_uuid);retain(page.object_uuid);}
  for(const auto& entry:inventory.entries)retain(entry.identity.transaction_uuid.value);
  for(const auto& entry:context.bound.checkpoint_inventory.inventory.entries)retain(entry.identity.transaction_uuid.value);
  for(const auto& step:record.steps)for(const auto& id:{step.uuid,step.operation_uuid,step.family_uuid,step.target_uuid,
    step.started_at,step.completed_at,step.evidence_uuid,step.metric_evidence_uuid,step.diagnostic_uuid,step.boundary_uuid})retain(id);
  const auto issue=[&](core::platform::UuidKind kind) {
    const auto id=issuer.Issue(kind);
    Require(id.error!=core::uuid::StandaloneUuidV7Error::resource_exhausted,E::resource_exhausted);
    Require(id.ok()&&identities.insert(id.value->value).second,E::identity_failure);return id.value->value;
  };
  using core::platform::UuidKind;
  const auto plan_object=old_plan==base.roots.end()?issue(UuidKind::object):old_plan->object_uuid;
  const auto extent_object=issue(UuidKind::object),bundle_object=issue(UuidKind::object);
  std::vector<page::NativeAllocationMap> maps;
  maps.reserve(map_count);
  for(const auto& image:context.bound.allocation.pages)maps.push_back(*image.map);
  const auto control=[&](u64 number,u32 type,const Uuid& owner) {
    disk::NativeCommonPageHeader h{u32(size),type,zero.bootstrap.database_uuid,zero.bootstrap.filespace_uuid,
      issue(UuidKind::page),number,watermark.watermark,0,zero.bootstrap.page_size_profile_uuid};
    const auto found=std::upper_bound(maps.begin(),maps.end(),number,[](u64 n,const auto& m){return n<m.first_page;});
    Require(found!=maps.begin(),E::allocation_mismatch);auto& map=*std::prev(found);
    Require(number-map.first_page<map.states.size()&&map.states[number-map.first_page]==page::NativeAllocationState::free,E::allocation_mismatch);
    const auto at=std::lower_bound(map.records.begin(),map.records.end(),number,[](const auto& r,u64 n){return r.page_number<n;});
    Require(at==map.records.end()||at->page_number!=number,E::allocation_mismatch);
    page::NativeAllocationRecord r;r.page_number=number;r.allocation_uuid=issue(UuidKind::object);
    r.page_uuid=h.page_uuid;r.owner_uuid=owner;r.page_generation=h.page_generation;r.page_type=type;
    r.creator_operation_uuid=watermark.operation_uuid;map.records.insert(at,r);
    map.states[number-map.first_page]=page::NativeAllocationState::allocated;return h;
  };
  OwnedInventoryGraph graph;
  auto& plan=graph.plan;
  plan.header=control(plan_slot,0x500,plan_object);
  auto target=base;target.header=control(checkpoint_slot,0x300,base.object_uuid);
  std::vector<disk::NativeCommonPageHeader> extent_headers,bundle_headers,inventory_headers;
  for(const auto slot:extent_slots)extent_headers.push_back(control(slot,0x500,extent_object));
  for(const auto slot:bundle_slots)bundle_headers.push_back(control(slot,0x500,bundle_object));
  for(std::size_t i=0;i<maps.size();++i)maps[i].header=control(map_slots[i],3,maps[i].object_uuid);
  for(const auto slot:inventory_slots)inventory_headers.push_back(control(slot,0x301,inventory_root.object_uuid));
  Pages map_images(maps.size()),inventory_images(inventory_count);
  for(std::size_t i=maps.size();i--;) {
    auto& map=maps[i];map.map_generation=watermark.watermark;
    map.creator_transaction_uuid={};map.creator_local_transaction_id=0;map.creator_operation_uuid=watermark.operation_uuid;
    map.next=i+1<maps.size()?std::optional{ControlRef(maps[i+1].header)}:std::nullopt;
    map.next_sha256=i+1<maps.size()?ControlHash(map_images[i+1]):std::array<byte,32>{};
    const u64 records_at=(384+(map.states.size()+1)/2+7)&~u64{7};
    Require(records_at<=size&&map.records.size()<=(size-records_at)/128,E::resource_exhausted);
    auto encoded=page::EncodeNativeAllocationMap(map);
    if(!encoded.ok())throw encoded.error==page::NativeAllocationError::resource_exhausted?E::resource_exhausted:
      encoded.error==page::NativeAllocationError::hash_failure?E::hash_failure:E::image_failure;
    map_images[i]=std::move(encoded.bytes);
  }
  for(std::size_t i=0;i<inventory_images.size();++i) {
    page::NativeTransactionInventoryPage image;image.header=inventory_headers[i];image.object_uuid=inventory_root.object_uuid;
    image.inventory_generation=watermark.watermark;
    image.previous=i?std::optional{ControlRef(inventory_headers[i-1])}:std::nullopt;
    image.next=i+1<inventory_headers.size()?std::optional{ControlRef(inventory_headers[i+1])}:std::nullopt;
    image.inventory.next_local_transaction_id=inventory.next_local_transaction_id;
    image.inventory.next_commit_sequence=inventory.next_commit_sequence;
    const auto first=std::min<u64>(i*per_inventory,inventory.entries.size());
    const auto count=std::min<u64>(per_inventory,inventory.entries.size()-first);
    image.inventory.entries.assign(inventory.entries.begin()+first,inventory.entries.begin()+first+count);
    auto encoded=page::EncodeNativeTransactionInventoryPage(image);
    if(!encoded.ok())throw encoded.error==page::NativeInventoryError::resource_exhausted?E::resource_exhausted:
      encoded.error==page::NativeInventoryError::hash_failure?E::hash_failure:E::image_failure;
    inventory_images[i]=std::move(encoded.bytes);
  }
  auto extent=EncodeNativeManagementExtent(record,extent_object,extent_headers,budget);ControlExtentError(extent.error);
  auto bundle=EncodeNativeManagementControlBundle(map_images,zero.bootstrap.database_uuid,zero.page_uuid,
    bundle_object,watermark.operation_uuid,bundle_headers,budget,inventory_images);ControlBundleError(bundle.error);
  plan.object_uuid=plan_object;plan.bootstrap_uuid=zero.page_uuid;plan.timeline_uuid=base.timeline_uuid;
  plan.operation_uuid=watermark.operation_uuid;plan.security_snapshot_uuid=record.security_snapshot_uuid;
  plan.intent=intent;plan.reservation_state_sha256=snapshot.state_sha256;
  plan.base_checkpoint=ControlRef(base.header);plan.base_checkpoint_object_uuid=base.object_uuid;
  plan.base_checkpoint_sha256=context.bound.checkpoint_inventory.checkpoint_sha256;
  plan.base_checkpoint_generation=base.checkpoint_generation;plan.base_root_set_generation=base.root_set_generation;
  plan.base_selection_generation=snapshot.selection.selection_generation;
  plan.target_checkpoint=ControlRef(target.header);plan.target_checkpoint_object_uuid=base.object_uuid;
  plan.reserved_generation=plan.target_root_set_generation=watermark.watermark;
  if(old_plan!=base.roots.end()){plan.previous_plan=old_plan->page;plan.previous_plan_object_uuid=old_plan->object_uuid;plan.previous_plan_sha256=old_plan->sha256;}
  plan.generation_guard_flags=0;for(unsigned i=0;i<3;++i)if(record.generation_guards[i])plan.generation_guard_flags|=1u<<i;
  plan.catalog_generation=record.generation_guards[0].value_or(0);plan.configuration_generation=record.generation_guards[1].value_or(0);
  plan.security_generation=record.generation_guards[2].value_or(0);plan.management_extent=extent.root;plan.control_bundle=bundle.root;
  target.creator_transaction_uuid={};target.creator_local_transaction_id=0;target.creator_operation_uuid=watermark.operation_uuid;
  target.checkpoint_generation=target.root_set_generation=watermark.watermark;
  target.predecessor=plan.base_checkpoint;target.predecessor_sha256=plan.base_checkpoint_sha256;
  target.selected_local_transaction_id=inventory.next_local_transaction_id-1;
  for(auto& r:target.roots) {
    if(r.role==1)r={1,0x301,ControlRef(inventory_headers.front()),inventory_root.object_uuid,ControlHash(inventory_images.front())};
    if(r.role==4)r={4,3,ControlRef(maps.front().header),maps.front().object_uuid,ControlHash(map_images.front())};
  }
  NativeCheckpointRootReference plan_root{16,0x500,ControlRef(plan.header),plan.object_uuid,{}};
  plan_root.sha256.fill(1); // Projection excludes this digest; never persisted.
  const auto target_plan=std::find_if(target.roots.begin(),target.roots.end(),[](const auto& r){return r.role==16;});
  if(target_plan==target.roots.end())target.roots.push_back(plan_root);else *target_plan=plan_root;
  auto checkpoint=EncodeNativeCheckpointRoot(target);ControlCheckpointError(checkpoint.error);
  const auto projection=ComputeNativePublicationTargetGraphDigest(checkpoint.bytes);ControlPlanError(projection.error);
  plan.target_graph_sha256=projection.sha256;
  const auto plan_image=EncodeNativePublicationPlan(plan);ControlPlanError(plan_image.error);
  std::find_if(target.roots.begin(),target.roots.end(),[](const auto& r){return r.role==16;})->sha256=plan_image.sha256;
  checkpoint=EncodeNativeCheckpointRoot(target);ControlCheckpointError(checkpoint.error);
  graph.checkpoint=std::move(checkpoint.bytes);graph.extent=std::move(extent.pages);graph.bundle=std::move(bundle.pages);
  return graph;
}
} // namespace

NativePublicationInspection PublishNativeInventoryOnLease(NativePublicationLease& lease,
    const NativeManagementOperation& record,const mga::LocalTransactionInventory& inventory,u64 budget,
    core::uuid::StandaloneUuidV7Issuer& issuer) noexcept {
  try {
    Require(!lease.impl_->installation_ambiguous,E::stale_base);
    const auto& old=*lease.impl_->context;
    auto current=Prepare(old.zero.bootstrap.database_uuid,old.devices,old.zero.bootstrap.filespace_uuid,budget,true,false);
    Require(SameBase(lease.impl_->snapshot,current->snapshot),E::stale_base);
    auto graph=AssembleInventory(*current,record,inventory,budget,issuer);
    auto installed=InstallNativeManagementControlGraphOnLease(lease,graph.plan,graph.checkpoint,graph.extent,graph.bundle,budget);
    if(!installed.ok())return installed;
    return PublishNativeManagementControlGraphOnLease(lease,budget);
  }catch(E e){return {e,{}};}catch(const std::bad_alloc&){return {E::resource_exhausted,{}};}
   catch(const std::length_error&){return {E::resource_exhausted,{}};}catch(...){return {E::io_failure,{}};}
}
} // namespace scratchbird::storage::database
