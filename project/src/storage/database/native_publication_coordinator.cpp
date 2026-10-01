// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_publication_coordinator.hpp"
#include "native_storage_action_intent.hpp"
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
E CapacityFailure(const NativeFilespaceCapacityResult& r){
  // Readers retain the failing layer separately from its backend diagnostic.
  // A failed read/hash/allocation is not evidence that the proposal was stale.
  const auto backend=[](auto error){
    using T=decltype(error);
    return error==T::resource_exhausted?E::resource_exhausted:
      error==T::hash_failure?E::hash_failure:error==T::io_failure?E::io_failure:E::none;
  };
  for(const auto error:{backend(r.error),backend(r.checkpoint_error),backend(r.inventory_error),
      backend(r.directory_error),backend(r.allocation_error),backend(r.management_error)})
    if(error!=E::none)return error;
  if(r.bootstrap_error==disk::FilespacePageZeroError::resource_exhausted)return E::resource_exhausted;
  if(r.bootstrap_error==disk::FilespacePageZeroError::hash_provider_failure)return E::hash_failure;
  if(r.bootstrap_error==disk::FilespacePageZeroError::io_failure)return E::io_failure;
  if(r.error==NativeFilespaceCapacityError::cluster_requires_authority)return E::cluster_requires_authority;
  if(r.error==NativeFilespaceCapacityError::encrypted_requires_authority)return E::encrypted_requires_authority;
  return E::stale_base;
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
  NativePublicationEffects local_effects{true};
  NativePublicationEffects* effects=&local_effects;
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
void EffectWrite(Context& c,disk::FileDevice& device,u64 offset,const std::vector<byte>& bytes,bool selector=false){
  auto& e=*c.effects;
  Require(e.write_attempts!=std::numeric_limits<u64>::max()&&
    bytes.size()<=std::numeric_limits<u64>::max()-e.attempted_bytes,E::resource_exhausted);
  ++e.write_attempts;e.attempted_bytes+=bytes.size();e.selector_write_attempted|=selector;
  // Set before entering I/O: even an exceptional backend cannot imply no effect.
  const bool prior_uncertainty=e.uncertain_write;e.uncertain_write=true;
  const auto io=device.WriteAt(offset,bytes.data(),bytes.size());
  if(io.bytes_transferred<=bytes.size())e.confirmed_bytes+=io.bytes_transferred;
  Require(io.ok()&&io.bytes_transferred==bytes.size(),E::io_failure);
  e.uncertain_write=prior_uncertainty;
}
void EffectSync(Context& c,disk::FileDevice& device){
  auto& e=*c.effects;
  Require(e.sync_attempts!=std::numeric_limits<u64>::max(),E::resource_exhausted);
  ++e.sync_attempts;Require(device.Sync().ok(),E::io_failure);++e.successful_syncs;
}
void Publish(Context& c,std::array<std::vector<byte>,2>& images,std::vector<byte>& scratch){
  const u64 size=c.zero.bootstrap.page_size_bytes;
  const auto read=[&](unsigned i,const auto& expected,E mismatch){
    const auto io=c.primary->ReadAt(c.headers[i].page_number*size,scratch.data(),scratch.size());
    Require(io.ok()&&io.bytes_transferred==scratch.size(),E::io_failure);Require(scratch==expected,mismatch);
  };
  for(unsigned i=0;i<2;++i)read(i,c.bytes[i],E::preimage_changed);
  for(const auto& file:c.devices)if(!file.device->read_only())EffectSync(c,*file.device);
  for(unsigned i=0;i<2;++i){if(images[i]!=c.bytes[i]){
      EffectWrite(c,*c.primary,c.headers[i].page_number*size,images[i]);
    }
    EffectSync(c,*c.primary);read(i,images[i],E::readback_mismatch);
  }
}
NativePublicationInspection InspectOrRecover(const Uuid& db,const std::vector<disk::NativeFilespaceDevice>& devices,
    const Uuid& primary,u64 budget,bool recover) noexcept {
  NativePublicationEffects effects{true};
  try {auto c=Prepare(db,devices,primary,budget,recover,recover);
    if(recover){auto images=EncodePair(*c,c->snapshot.watermark);std::vector<byte> scratch(c->zero.bootstrap.page_size_bytes);c->effects=&effects;Publish(*c,images,scratch);}
    return {E::none,c->snapshot,effects};
  }catch(E e){return {e,{},effects};}catch(const std::bad_alloc&){return {E::resource_exhausted,{},effects};}
   catch(const std::length_error&){return {E::resource_exhausted,{},effects};}catch(...){return {E::io_failure,{},effects};}
}
} // namespace
struct NativePublicationLease::Impl {NativePublicationEffects effects{true};std::unique_ptr<Context> context;NativePublicationSnapshot snapshot;bool installation_ambiguous=false;};
NativePublicationLease::NativePublicationLease(std::unique_ptr<Impl> p) noexcept:impl_(std::move(p)){}
NativePublicationLease::~NativePublicationLease()=default;
const NativePublicationSnapshot& NativePublicationLease::snapshot() const noexcept{return impl_->snapshot;}
const NativePublicationEffects& NativePublicationLease::effects() const noexcept{return impl_->effects;}
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
  NativePublicationEffects effects{true};
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
    lease->impl_->context->effects=&effects;
    Publish(*lease->impl_->context,images,scratch);
    lease->impl_->effects=effects;lease->impl_->context->effects=&lease->impl_->effects;
    return {E::none,std::move(lease),effects};
  }catch(E e){return {e,{},effects};}catch(const std::bad_alloc&){return {E::resource_exhausted,{},effects};}
   catch(const std::length_error&){return {E::resource_exhausted,{},effects};}catch(...){return {E::io_failure,{},effects};}
}
NativePublicationReservation ResumeNativePublicationGenerationOnOpenDevices(const Uuid& db,
    const std::vector<disk::NativeFilespaceDevice>& devices,const Uuid& primary,
    const NativePublicationSnapshot& expected,const Uuid& operation,const NativePublicationIntent& intent,u64 budget) noexcept {
  NativePublicationEffects effects{true};
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
    lease->impl_->context->effects=&effects;
    Publish(*lease->impl_->context,images,scratch);
    lease->impl_->effects=effects;lease->impl_->context->effects=&lease->impl_->effects;
    return {E::none,std::move(lease),effects};
  }catch(E e){return {e,{},effects};}catch(const std::bad_alloc&){return {E::resource_exhausted,{},effects};}
   catch(const std::length_error&){return {E::resource_exhausted,{},effects};}catch(...){return {E::io_failure,{},effects};}
}
static NativePublicationInspection AbandonNativeControlPublicationOnOpenDevices(
    const Uuid& database,const std::vector<disk::NativeFilespaceDevice>& devices,const Uuid& primary,
    const NativePublicationSnapshot& expected,const Uuid& operation,const NativePublicationIntent& intent,
    const Uuid& resolution,u16 profile,u64 budget) noexcept {
  NativePublicationEffects effects{true};
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
    c->effects=&effects;Publish(*c,images,scratch);
    auto final=Prepare(database,devices,primary,budget-used+6*size,true,false);charge(final->bound.retained_image_bytes);
    Require(SameBase(planned,final->snapshot)&&final->bytes==images,E::binding_mismatch);
    return {E::none,std::move(final->snapshot),effects};
  }catch(E e){return {e,{},effects};}catch(const std::bad_alloc&){return {E::resource_exhausted,{},effects};}
   catch(const std::length_error&){return {E::resource_exhausted,{},effects};}catch(...){return {E::io_failure,{},effects};}
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
  const auto action=[&]() -> NativePublicationInspection {
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
    c->effects=&lease.impl_->effects;lease.impl_->installation_ambiguous=true;
    Publish(*c,images,scratch);
    read_plan(scratch);Require(scratch==preimage,E::preimage_changed);
    if(preimage!=image.bytes){EffectWrite(*c,*c->primary,plan.header.page_number*size,image.bytes);}
    EffectSync(*c,*c->primary);read_plan(scratch);Require(scratch==image.bytes,E::readback_mismatch);
    for(std::size_t left=extent.size();left;--left){const auto i=left-1;read_extent(i,scratch);Require(scratch==extent_preimages[i],E::preimage_changed);
      if(scratch!=extent[i]){EffectWrite(*c,*c->primary,(plan.management_extent->first.page_number+i)*size,extent[i]);}}
    if(!extent.empty()){EffectSync(*c,*c->primary);
      for(std::size_t i=0;i<extent.size();++i){read_extent(i,scratch);Require(scratch==extent[i],E::readback_mismatch);}}
    c->bytes=std::move(images);
    lease.impl_->snapshot=snapshot;lease.impl_->context=std::move(c);
    lease.impl_->installation_ambiguous=false;
    return {E::none,std::move(snapshot)};
  }catch(E e){return {e,{}};}catch(const std::bad_alloc&){return {E::resource_exhausted,{}};}
   catch(const std::length_error&){return {E::resource_exhausted,{}};}catch(...){return {E::io_failure,{}};}

  };
  auto result=action();result.effects=lease.effects();return result;
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
  if(r.directory_count){Require(r.payload_bytes<=(budget-total)/4,E::resource_exhausted);return total+4*r.payload_bytes;}
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
  const auto action=[&]() -> NativePublicationInspection {
  try {
    Require(!lease.impl_->installation_ambiguous,E::stale_base);const auto plan=supplied_plan;
    Require(plan.control_bundle&&plan.management_extent,E::invalid_request);
    const auto* profile=disk::FindCanonicalFilespacePageProfile(plan.header.page_size_profile_uuid);
    Require(profile&&profile->page_size_bytes==plan.header.page_size_bytes,E::invalid_request);const u64 size=profile->page_size_bytes;
    u64 allowance=0;const auto charge=[&](u64 count,u64 unit){Require(count<=(budget-allowance)/unit,E::resource_exhausted);allowance+=count*unit;};
    charge(20,size);charge(plan.management_extent->page_count,4*size);charge(plan.management_extent->aggregate_bytes,4);charge(plan.control_bundle->page_count,4*size);
    if(plan.control_bundle->directory_count)charge(plan.control_bundle->payload_bytes,14);
    else{charge(plan.control_bundle->map_count,10*size);charge(plan.control_bundle->inventory_count,14*size);}
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
    const auto base=EncodeNativeCheckpointRoot(*c->bound.checkpoint_inventory.checkpoint);ControlCheckpointError(base.error);Require(ControlHash(base.bytes)==c->bound.checkpoint_inventory.checkpoint_sha256,E::binding_mismatch);
    Pages before_maps;before_maps.reserve(c->bound.allocation.pages.size());for(const auto& p:c->bound.allocation.pages)before_maps.push_back(p.bytes);
    NativeManagementDirectoryBase directory_base;
    std::map<Uuid,page::NativeAllocationChainResult> secondary_maps;
    std::map<Uuid,disk::FilespacePageZero> member_zeros;
    const auto device=[&](const Uuid& id)->const disk::NativeFilespaceDevice& {const auto it=std::lower_bound(c->devices.begin(),c->devices.end(),id,[](const auto& f,const auto& v){return f.filespace_uuid<v;});Require(it!=c->devices.end()&&it->filespace_uuid==id,E::invalid_device);return *it;};
    if(plan.control_bundle->directory_count){
      // A growth graph is staged against original capacity. Its retained after
      // body is reconstruction input only; this installer never extends a file
      // or writes page zero. Physical admission/execution remains separate.
      Require(plan.intent.recovery_profile==4?reconstruction.growth_images.size()==2:
        reconstruction.growth_images.empty(),E::invalid_request);
      const auto& cp=*c->bound.checkpoint_inventory.checkpoint;const auto root=std::find_if(cp.roots.begin(),cp.roots.end(),[](const auto& r){return r.role==3;});Require(root!=cp.roots.end(),E::binding_mismatch);
      const disk::FilespaceRootReference ref{5,root->page_type,root->page.filespace_uuid,root->page.page_number,root->page.page_generation,root->page.page_size_profile_uuid,root->object_uuid};
      auto directory=page::ReadNativeFilespaceDirectoryFromOpenDevices(plan.header.database_uuid,c->devices,ref,budget-allowance);
      if(!directory.ok()){using D=page::NativeDirectoryError;throw directory.error==D::resource_exhausted?E::resource_exhausted:directory.error==D::hash_failure?E::hash_failure:directory.error==D::io_failure?E::io_failure:E::binding_mismatch;}
      charge(directory.retained_image_bytes,2);Require(ControlHash(directory.pages.front().bytes)==root->sha256,E::binding_mismatch);
      for(const auto& image:directory.pages)directory_base.directory_images.push_back(image.bytes);
      std::set<Uuid> changed;for(const auto& raw:reconstruction.allocation_images){const auto h=disk::DecodeNativeCommonPageHeader(raw.data(),128);Require(h.ok(),E::binding_mismatch);changed.insert(h.header->filespace_uuid);}
      before_maps.clear();
      for(const auto& fs:changed){const auto& file=device(fs);Require(!file.device->read_only(),E::invalid_device);const disk::FilespaceBootstrapBinding binding{plan.header.database_uuid,fs,file.page_size_profile_uuid};
        const auto zero=disk::ReadFilespacePageZeroFromOpenDevice(*file.device,&binding);
        if(!zero.ok())throw zero.error==disk::FilespacePageZeroError::resource_exhausted?E::resource_exhausted:zero.error==disk::FilespacePageZeroError::hash_provider_failure?E::hash_failure:zero.error==disk::FilespacePageZeroError::io_failure?E::io_failure:E::bootstrap_failure;
        Require(!(zero.record->bootstrap.flags&disk::FilespaceBootstrapFlag::payload_encrypted),E::encrypted_requires_authority);
        Require(!(zero.record->bootstrap.flags&disk::FilespaceBootstrapFlag::cluster_authority_required),E::cluster_requires_authority);
        const auto encoded=disk::EncodeFilespacePageZero(*zero.record);if(!encoded.ok())throw encoded.error==disk::FilespacePageZeroError::resource_exhausted?E::resource_exhausted:encoded.error==disk::FilespacePageZeroError::hash_provider_failure?E::hash_failure:E::bootstrap_failure;
        charge(encoded.bytes->size(),3);directory_base.page_zero_images.push_back(*encoded.bytes);member_zeros.emplace(fs,*zero.record);
        const page::NativeAllocationChainResult* maps=&c->bound.allocation;
        if(fs!=c->zero.bootstrap.filespace_uuid){const page::NativeFilespaceDirectoryRecord* member=nullptr;
          for(const auto& image:directory.pages)for(const auto& r:image.directory->records)if(r.bootstrap.filespace_uuid==fs)member=&r;Require(member,E::binding_mismatch);
          page::NativeAllocationChainResult actual;
          if(member->allocation_root){const auto& r=*member->allocation_root;const disk::FilespaceRootReference head{3,3,fs,r.page.page_number,r.page.page_generation,r.page.page_size_profile_uuid,r.object_uuid};
            actual=page::ReadNativeAllocationChainAtRootFromOpenDevice(*file.device,binding,head,budget-allowance);
            if(actual.ok())Require(ControlHash(actual.pages.front().bytes)==r.sha256,E::binding_mismatch);
          }else actual=page::ReadNativeAllocationChainFromOpenDevice(*file.device,binding,budget-allowance);
          if(!actual.ok()){using A=page::NativeAllocationError;throw actual.error==A::resource_exhausted?E::resource_exhausted:actual.error==A::hash_failure?E::hash_failure:actual.error==A::io_failure?E::io_failure:actual.error==A::cluster_requires_authority?E::cluster_requires_authority:E::allocation_mismatch;}
          charge(actual.retained_image_bytes,2);maps=&secondary_maps.emplace(fs,std::move(actual)).first->second;
        }
        for(const auto& image:maps->pages)before_maps.push_back(image.bytes);
      }
      const auto provenance=ReadNativeManagementControlAuthorityFromOpenDevices(plan.header.database_uuid,c->devices,c->zero.bootstrap.filespace_uuid,budget-allowance);
      if(!provenance.ok()){using G=NativeManagementControlAuthorityError;throw provenance.error==G::resource_exhausted?E::resource_exhausted:provenance.error==G::hash_failure?E::hash_failure:provenance.error==G::io_failure?E::io_failure:provenance.error==G::cluster_requires_authority?E::cluster_requires_authority:provenance.error==G::encrypted_requires_authority?E::encrypted_requires_authority:E::allocation_mismatch;}
      charge(provenance.verified_image_bytes,1);
      const auto transaction=[&](const Uuid& id,u64 local,bool committed){const auto tx=mga::LookupLocalTransaction(c->bound.checkpoint_inventory.inventory,mga::MakeLocalTransactionId(local));
        Require(tx.ok()&&tx.entry.identity.transaction_uuid.value==id&&tx.entry.identity.scope==mga::TransactionScope::local_node&&(!committed||mga::HasCommittedInventoryOutcome(tx.entry)),E::allocation_mismatch);};
      const auto creators=[&](const page::NativeAllocationChainResult& chain){for(const auto& raw:chain.pages){const auto& map=*raw.map;
        if(map.creator_operation_uuid.is_nil())transaction(map.creator_transaction_uuid,map.creator_local_transaction_id,true);
        else Require(MatchesNativeManagementControlMap(provenance,map),E::allocation_mismatch);
        for(const auto& r:map.records){if(r.creator_operation_uuid.is_nil())transaction(r.creator_transaction_uuid,r.creator_local_transaction_id,false);
          else Require(MatchesNativeManagementControlAllocation(provenance,map.header.filespace_uuid,r,map.states[r.page_number-map.first_page]),E::allocation_mismatch);}}};
      creators(c->bound.allocation);for(const auto& [fs,chain]:secondary_maps)creators(chain);
    }
    Pages before_inventory;
    if(plan.intent.recovery_profile==2){charge(c->bound.retained_image_bytes,1);const auto& cp=*c->bound.checkpoint_inventory.checkpoint;
      const auto root=std::find_if(cp.roots.begin(),cp.roots.end(),[](const auto& r){return r.role==1;});Require(root!=cp.roots.end(),E::binding_mismatch);
      const disk::FilespaceRootReference ref{4,root->page_type,root->page.filespace_uuid,root->page.page_number,root->page.page_generation,root->page.page_size_profile_uuid,root->object_uuid};
      auto actual=page::ReadNativeTransactionInventoryChainFromOpenDevices(plan.header.database_uuid,c->devices,ref,budget-allowance);
      if(!actual.ok())throw actual.error==page::NativeInventoryError::resource_exhausted?E::resource_exhausted:actual.error==page::NativeInventoryError::hash_failure?E::hash_failure:actual.error==page::NativeInventoryError::io_failure?E::io_failure:actual.error==page::NativeInventoryError::encrypted_requires_crypto_authority?E::encrypted_requires_authority:E::allocation_mismatch;
      charge(actual.retained_image_bytes,1);for(const auto& raw:actual.pages)charge(raw.bytes.size(),4);before_inventory.reserve(actual.pages.size());for(auto& raw:actual.pages)before_inventory.push_back(std::move(raw.bytes));
    }
    const auto delta=plan.control_bundle->directory_count?
      ValidateNativeManagementDirectoryControlAllocation(base.bytes,checkpoint,image.bytes,extent,before_maps,reconstruction.allocation_images,directory_base,budget,bundle,before_inventory):
      ValidateNativeManagementControlAllocation(base.bytes,checkpoint,image.bytes,extent,before_maps,reconstruction.allocation_images,budget,bundle,before_inventory);
    if(delta!=NativeManagementControlAllocationError::none)throw delta==NativeManagementControlAllocationError::resource_exhausted?E::resource_exhausted:delta==NativeManagementControlAllocationError::hash_failure?E::hash_failure:delta==NativeManagementControlAllocationError::cluster_requires_authority?E::cluster_requires_authority:delta==NativeManagementControlAllocationError::encrypted_requires_authority?E::encrypted_requires_authority:E::allocation_mismatch;
    struct Artifact {Uuid filespace;u64 page=0;Bytes bytes,before;};std::vector<Artifact> artifacts;artifacts.reserve(2+extent.size()+bundle.size()+reconstruction.allocation_images.size()+reconstruction.inventory_images.size()+reconstruction.directory_images.size());
    const auto append=[&](Bytes raw){const auto h=disk::DecodeNativeCommonPageHeader(raw.data(),128);Require(h.ok(),E::image_failure);const auto& f=device(h.header->filespace_uuid);
      Require(h.header->database_uuid==plan.header.database_uuid&&h.header->page_size_profile_uuid==f.page_size_profile_uuid&&raw.size()==disk::FindCanonicalFilespacePageProfile(f.page_size_profile_uuid)->page_size_bytes,E::binding_mismatch);
      const auto& zero=f.filespace_uuid==c->zero.bootstrap.filespace_uuid?c->zero:member_zeros.at(f.filespace_uuid);
      Require(h.header->page_type==3||zero.bootstrap.filespace_role<=4,E::allocation_mismatch);
      artifacts.push_back({h.header->filespace_uuid,h.header->page_number,std::move(raw),{}});};
    append(std::move(image.bytes));const std::size_t extent_start=artifacts.size();
    for(auto& raw:extent)append(std::move(raw));
    const std::size_t bundle_start=artifacts.size();
    for(auto& raw:bundle)append(std::move(raw));
    const std::size_t inventory_start=artifacts.size();
    for(auto& raw:reconstruction.inventory_images)append(std::move(raw));
    for(auto& raw:reconstruction.directory_images)append(std::move(raw));
    const std::size_t map_start=artifacts.size();
    for(auto& raw:reconstruction.allocation_images)append(std::move(raw));const std::size_t checkpoint_start=artifacts.size();
    append(std::move(checkpoint));Bytes scratch;for(const auto& target:artifacts)if(target.bytes.size()>scratch.capacity())scratch.reserve(target.bytes.size());scratch.resize(size);
    const auto read=[&](const Artifact& target,Bytes& into){into.resize(target.bytes.size());const auto io=device(target.filespace).device->ReadAt(target.page*u64{into.size()},into.data(),into.size());Require(io.ok()&&io.bytes_transferred==into.size(),E::io_failure);};
    for(auto& target:artifacts){const auto& zero=target.filespace==c->zero.bootstrap.filespace_uuid?c->zero:member_zeros.at(target.filespace);
      for(const auto& root:zero.roots)if(root.filespace_uuid==target.filespace)Require(root.page_number!=target.page,E::allocation_mismatch);
      const auto& chain=target.filespace==c->zero.bootstrap.filespace_uuid?c->bound.allocation:secondary_maps.at(target.filespace);
      auto map=std::upper_bound(chain.pages.begin(),chain.pages.end(),target.page,[](u64 n,const auto& p){return n<p.map->first_page;});Require(map!=chain.pages.begin(),E::allocation_mismatch);--map;
      const auto& m=*map->map;Require(target.page-m.first_page<m.states.size()&&m.states[target.page-m.first_page]==page::NativeAllocationState::free,E::allocation_mismatch);
      const auto record=std::lower_bound(m.records.begin(),m.records.end(),target.page,[](const auto& r,u64 n){return r.page_number<n;});Require(record==m.records.end()||record->page_number!=target.page,E::allocation_mismatch);
      read(target,target.before);if(!original.publication_plan)Require(std::all_of(target.before.begin(),target.before.end(),[](byte b){return !b;}),E::preimage_changed);
    }
    auto next=original;next.publication_plan=NativePublicationWatermark::PlanAnchor{ControlRef(plan.header),plan.object_uuid,image.sha256,plan.reservation_state_sha256};
    auto images=EncodePair(*c,next);auto decoded=DecodeNativePublicationWatermark(images[0]);Backend(decoded.error);Require(decoded.ok(),E::image_failure);
    NativePublicationSnapshot snapshot{c->snapshot.selection,*decoded.state,decoded.state_sha256};Bytes().swap(decoded.bytes);
    for(unsigned i=0;i<2;++i){auto& slot=c->decoded[i];slot.error=NativePublicationWatermarkError::none;slot.state=next;slot.state->header=c->headers[i];slot.state_sha256=snapshot.state_sha256;}
    c->snapshot=snapshot;c->stable=true;
    const auto install=[&](std::size_t first,std::size_t end){for(std::size_t n=end;n>first;--n){auto& target=artifacts[n-1];read(target,scratch);Require(scratch==target.before,E::preimage_changed);
        if(scratch!=target.bytes){EffectWrite(*c,*device(target.filespace).device,target.page*u64{target.bytes.size()},target.bytes);}}
      for(const auto& file:c->devices)if(std::any_of(artifacts.begin()+first,artifacts.begin()+end,[&](const auto& target){return target.filespace==file.filespace_uuid;}))EffectSync(*c,*file.device);
      for(std::size_t n=first;n<end;++n){read(artifacts[n],scratch);Require(scratch==artifacts[n].bytes,E::readback_mismatch);}scratch.resize(size);};
    c->effects=&lease.impl_->effects;lease.impl_->installation_ambiguous=true;
    // Storage work has no metadata-only abandonment. An anchor may not escape
    // until its immutable reconstruction inputs are durable and verified.
    const bool storage_work=plan.intent.recovery_profile==3||plan.intent.recovery_profile==4;
    if(!storage_work)Publish(*c,images,scratch);
    install(0,extent_start);install(extent_start,bundle_start);install(bundle_start,inventory_start);
    if(storage_work)Publish(*c,images,scratch);
    if(inventory_start!=map_start)install(inventory_start,map_start);install(map_start,checkpoint_start);install(checkpoint_start,artifacts.size());
    lease.impl_->effects.installed_graph_verified=true;
    c->bytes=std::move(images);lease.impl_->snapshot=snapshot;lease.impl_->context=std::move(c);lease.impl_->installation_ambiguous=false;return {E::none,std::move(snapshot)};
  }catch(E e){return {e,{}};}catch(const std::bad_alloc&){return {E::resource_exhausted,{}};}catch(const std::length_error&){return {E::resource_exhausted,{}};}catch(...){return {E::io_failure,{}};}

  };
  auto result=action();result.effects=lease.effects();return result;
}
NativePublicationInspection ResumeNativeManagementControlGraphOnLease(NativePublicationLease& lease,u64 budget) noexcept {
  const auto action=[&]() -> NativePublicationInspection {
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
    NativeManagementControlBundleRead contents;
    if(plan.intent.recovery_profile==4){
      // This is pre-extension reconstruction under the original retained lease,
      // not an ordinary result-context read of an already completed growth.
      // Bound every immutable input to original capacity before reading it;
      // installation below revalidates the full actual before-image lineage.
      const auto& root=*plan.control_bundle;
      Require(root.first.filespace_uuid==file.filespace_uuid&&root.first.page_size_profile_uuid==file.page_size_profile_uuid&&
        root.first.page_number<context.zero.total_pages&&root.page_count<=context.zero.total_pages-root.first.page_number,E::binding_mismatch);
      Pages pages;pages.reserve(root.page_count);
      for(u64 i=0;i<root.page_count;++i){Bytes bytes(size);const auto read=context.primary->ReadAt((root.first.page_number+i)*size,bytes.data(),bytes.size());
        Require(read.ok()&&read.bytes_transferred==bytes.size(),E::io_failure);pages.push_back(std::move(bytes));}
      contents=DecodeNativeManagementControlBundle(pages,root,plan.header.database_uuid,plan.bootstrap_uuid,bundle_allowance);
    }else contents=ReadNativeManagementControlBundleFromOpenDevice(file,*plan.control_bundle,plan.header.database_uuid,plan.bootstrap_uuid,bundle_allowance);
    ControlBundleError(contents.error);
    auto bundle=EncodeNativeManagementControlBundle(contents.allocation_images,plan.header.database_uuid,plan.bootstrap_uuid,plan.control_bundle->object_uuid,plan.operation_uuid,contents.page_headers,bundle_allowance,contents.inventory_images,contents.directory_images,contents.growth_images);ControlBundleError(bundle.error);Require(bundle.root==plan.control_bundle,E::binding_mismatch);
    std::optional<page::NativeAllocationMap> first;std::optional<page::NativeAllocationRecord> target_record;
    const Bytes* primary_map_image=nullptr;
    for(const auto& b:contents.allocation_images){auto map=page::DecodeNativeAllocationMap(b);if(!map.ok())throw map.error==page::NativeAllocationError::resource_exhausted?E::resource_exhausted:map.error==page::NativeAllocationError::hash_failure?E::hash_failure:E::allocation_mismatch;
      const auto& m=*map.map;if(m.header.filespace_uuid!=plan.target_checkpoint.filespace_uuid)continue;
      const u64 n=plan.target_checkpoint.page_number;if(n>=m.first_page&&n-m.first_page<m.states.size()){const auto r=std::lower_bound(m.records.begin(),m.records.end(),n,[](const auto& r,u64 v){return r.page_number<v;});Require(m.states[n-m.first_page]==page::NativeAllocationState::allocated&&r!=m.records.end()&&r->page_number==n,E::allocation_mismatch);target_record=*r;}
      if(!first){first=std::move(map.map);primary_map_image=&b;}
    }
    Require(first&&target_record&&target_record->page_type==0x300&&target_record->owner_uuid==plan.target_checkpoint_object_uuid&&target_record->creator_operation_uuid==plan.operation_uuid&&target_record->creator_transaction_uuid.is_nil()&&!target_record->creator_local_transaction_id&&target_record->page_generation==plan.reserved_generation,E::allocation_mismatch);
    auto cp=*context.bound.checkpoint_inventory.checkpoint;cp.header=plan.header;cp.header.page_type=0x300;cp.header.page_number=plan.target_checkpoint.page_number;cp.header.page_generation=plan.target_checkpoint.page_generation;cp.header.page_uuid=target_record->page_uuid;cp.object_uuid=plan.target_checkpoint_object_uuid;cp.creator_transaction_uuid={};cp.creator_local_transaction_id=0;cp.creator_operation_uuid=plan.operation_uuid;cp.checkpoint_generation=plan.reserved_generation;cp.root_set_generation=plan.target_root_set_generation;cp.predecessor=plan.base_checkpoint;cp.predecessor_sha256=plan.base_checkpoint_sha256;
    auto allocation=std::find_if(cp.roots.begin(),cp.roots.end(),[](const auto& r){return r.role==4;});Require(allocation!=cp.roots.end(),E::allocation_mismatch);*allocation={4,3,ControlRef(first->header),first->object_uuid,ControlHash(*primary_map_image)};
    if(!contents.directory_images.empty()){const auto directory=page::DecodeNativeFilespaceDirectory(contents.directory_images.front());
      if(!directory.ok()){using D=page::NativeDirectoryError;throw directory.error==D::resource_exhausted?E::resource_exhausted:directory.error==D::hash_failure?E::hash_failure:E::binding_mismatch;}
      auto root=std::find_if(cp.roots.begin(),cp.roots.end(),[](const auto& r){return r.role==3;});Require(root!=cp.roots.end(),E::binding_mismatch);const auto& d=*directory.directory;
      *root={3,9,ControlRef(d.header),d.object_uuid,ControlHash(contents.directory_images.front())};
    }
    if(plan.intent.recovery_profile==2){Require(!contents.inventory_images.empty(),E::binding_mismatch);const auto decoded=page::DecodeNativeTransactionInventoryPage(contents.inventory_images.front());
      if(!decoded.ok())throw decoded.error==page::NativeInventoryError::resource_exhausted?E::resource_exhausted:decoded.error==page::NativeInventoryError::hash_failure?E::hash_failure:E::allocation_mismatch;
      const auto& candidate=*decoded.page;auto root=std::find_if(cp.roots.begin(),cp.roots.end(),[](const auto& r){return r.role==1;});Require(root!=cp.roots.end(),E::binding_mismatch);*root={1,0x301,ControlRef(candidate.header),candidate.object_uuid,ControlHash(contents.inventory_images.front())};cp.selected_local_transaction_id=candidate.inventory.next_local_transaction_id-1;
    }
    const NativeCheckpointRootReference head{16,0x500,ControlRef(plan.header),plan.object_uuid,image.sha256};auto history=std::find_if(cp.roots.begin(),cp.roots.end(),[](const auto& r){return r.role==16;});if(history==cp.roots.end())cp.roots.push_back(head);else *history=head;
    const auto target=EncodeNativeCheckpointRoot(cp);ControlCheckpointError(target.error);ControlPlanError(BindNativePublicationPlanToLease(plan,lease,target.bytes));
    return InstallNativeManagementControlGraphOnLease(lease,plan,target.bytes,extent.pages,bundle.pages,budget-allowance);
  }catch(E e){return {e,{}};}catch(const std::bad_alloc&){return {E::resource_exhausted,{}};}catch(const std::length_error&){return {E::resource_exhausted,{}};}catch(...){return {E::io_failure,{}};}

  };
  auto result=action();result.effects=lease.effects();return result;
}
NativePublicationInspection PublishNativeManagementControlGraphOnLease(NativePublicationLease& lease,u64 budget) noexcept {
  const auto action=[&]() -> NativePublicationInspection {
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
    charge(proof.verified_image_bytes);lease.impl_->effects.installed_graph_verified=true;
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
    c->effects=&lease.impl_->effects;lease.impl_->installation_ambiguous=true;
    for(const auto& file:c->devices)if(!file.device->read_only())EffectSync(*c,*file.device);
    for(unsigned i=0;i<2;++i){EffectWrite(*c,*c->primary,pages[i]*size,images[i],true);
      EffectSync(*c,*c->primary);verify(i,images[i],E::readback_mismatch);}
    auto final=Prepare(database,c->devices,primary,budget-used+6*size,true,false);charge(final->bound.retained_image_bytes);
    Require(SameBase(expected,final->snapshot)&&final->bound.slots==images,E::binding_mismatch);
    lease.impl_->effects.selected_graph_verified=true;final->effects=&lease.impl_->effects;
    const auto snapshot=final->snapshot;lease.impl_->snapshot=snapshot;lease.impl_->context=std::move(final);lease.impl_->installation_ambiguous=false;
    return {E::none,snapshot};
  }catch(E e){return {e,{}};}catch(const std::bad_alloc&){return {E::resource_exhausted,{}};}catch(const std::length_error&){return {E::resource_exhausted,{}};}catch(...){return {E::io_failure,{}};}

  };
  auto result=action();result.effects=lease.effects();return result;
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
    core::uuid::StandaloneUuidV7Issuer& issuer,const NativeStorageActionIntent* preallocation=nullptr) {
  const auto& snapshot=context.snapshot;
  const auto& watermark=snapshot.watermark;
  const auto& zero=context.zero;
  const auto& base=*context.bound.checkpoint_inventory.checkpoint;
  Require(watermark.intent&&watermark.intent->recovery_profile==(preallocation?3:2)&&
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
  const bool secondary=preallocation&&preallocation->filespace_uuid!=zero.bootstrap.filespace_uuid;
  const u64 secondary_size=secondary?preallocation->page_size_bytes:0;
  const auto ceil=[](u64 n,u64 d){return n/d+(n%d!=0);};
  const u64 inventory_count=preallocation?0:std::max<u64>(1,ceil(supplied_inventory.entries.size(),per_inventory));
  u64 map_count=context.bound.allocation.pages.size();
  const u64 extent_count=ceil(encoded_record.bytes.size(),payload);
  u64 charged=context.bound.retained_image_bytes;
  Require(charged<=budget,E::resource_exhausted);
  const auto charge=[&](u64 count,u64 unit){Require(unit&&count<=(budget-charged)/unit,E::resource_exhausted);charged+=count*unit;};
  charge(20,size);charge(map_count,10*size);charge(inventory_count,14*size);
  charge(extent_count,4*size);charge(encoded_record.bytes.size(),4);
  // The preceding charges bound both addition and multiplication below.
  u64 bundle_count=ceil((map_count+inventory_count)*size,payload);
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
  // A multi-member directory (or an already published directory descriptor)
  // must follow the new primary map root. Preserve every other member exactly.
  const auto& directory_root=root(3);
  bool directory_publication=false;
  page::NativeFilespaceDirectoryChainResult original_directory;
  std::vector<page::NativeFilespaceDirectoryRecord> directory_records;
  u64 directory_count=0;
  {
    const disk::FilespaceRootReference head{5,9,directory_root.page.filespace_uuid,directory_root.page.page_number,
      directory_root.page.page_generation,directory_root.page.page_size_profile_uuid,directory_root.object_uuid};
    original_directory=page::ReadNativeFilespaceDirectoryFromOpenDevices(zero.bootstrap.database_uuid,context.devices,head,budget-charged);
    if(!original_directory.ok())throw original_directory.error==page::NativeDirectoryError::resource_exhausted?E::resource_exhausted:
      original_directory.error==page::NativeDirectoryError::hash_failure?E::hash_failure:original_directory.error==page::NativeDirectoryError::io_failure?E::io_failure:E::binding_mismatch;
    charge(original_directory.retained_image_bytes,2);
    Require(ControlHash(original_directory.pages.front().bytes)==directory_root.sha256,E::binding_mismatch);
    for(const auto& raw:original_directory.pages)directory_records.insert(directory_records.end(),raw.directory->records.begin(),raw.directory->records.end());
    const auto member=std::find_if(directory_records.begin(),directory_records.end(),[&](const auto& r){return r.bootstrap.filespace_uuid==zero.bootstrap.filespace_uuid;});
    Require(member!=directory_records.end(),E::binding_mismatch);
    directory_publication=directory_records.size()>1||member->allocation_root.has_value();
  }
  if(directory_publication){
    directory_count=ceil(directory_records.size(),(size-384)/320);charge(directory_count,14*size);
    const auto framed=ceil((map_count+inventory_count+directory_count)*(size+8),payload);
    charge(framed-bundle_count,4*size);bundle_count=framed;
  }
  page::NativeAllocationChainResult secondary_allocation;
  disk::FileDevice* secondary_device=nullptr;
  if(secondary){
    Require(directory_publication,E::binding_mismatch);
    const auto member=std::find_if(context.devices.begin(),context.devices.end(),[&](const auto& f){return f.filespace_uuid==preallocation->filespace_uuid;});
    Require(member!=context.devices.end()&&!member->device->read_only(),E::invalid_device);secondary_device=member->device;
    const disk::FilespaceBootstrapBinding binding{zero.bootstrap.database_uuid,member->filespace_uuid,member->page_size_profile_uuid};
    secondary_allocation=page::ReadNativeAllocationChainAtRootFromOpenDevice(*secondary_device,binding,preallocation->allocation_root,budget-charged);
    if(!secondary_allocation.ok())throw secondary_allocation.error==page::NativeAllocationError::resource_exhausted?E::resource_exhausted:
      secondary_allocation.error==page::NativeAllocationError::hash_failure?E::hash_failure:secondary_allocation.error==page::NativeAllocationError::io_failure?E::io_failure:E::allocation_mismatch;
    Require(ControlHash(secondary_allocation.pages.front().bytes)==preallocation->allocation_sha256,E::binding_mismatch);
    charge(secondary_allocation.retained_image_bytes,2);charge(secondary_allocation.pages.size(),10*secondary_size);
    map_count+=secondary_allocation.pages.size();
    const auto framed=ceil((context.bound.allocation.pages.size()+inventory_count+directory_count)*(size+8)+secondary_allocation.pages.size()*(secondary_size+8),payload);
    charge(framed-bundle_count,4*size);bundle_count=framed;
  }
  std::vector<const page::NativeAllocationMapResult*> source_maps;
  for(const auto& image:context.bound.allocation.pages)source_maps.push_back(&image);
  for(const auto& image:secondary_allocation.pages)source_maps.push_back(&image);

  // Only original free/recordless, physically zero slots may be consumed. A
  // dirty free slot is an orphan to be resolved by its owner, not free bytes.
  std::array<std::set<u64>,2> selected;
  std::array<std::map<u64,bool>,2> probes;
  Bytes probe(std::max(size,secondary_size));
  const auto allocate=[&](u64 count,unsigned group=0) {
    const auto& sources=group?secondary_allocation.pages:context.bound.allocation.pages;
    const u64 page_size=group?secondary_size:size;
    Require(count&&count<=(group?preallocation->current_total_pages:zero.total_pages),E::allocation_exhausted);
    std::vector<u64> run;run.reserve(static_cast<std::size_t>(count));
    for(const auto& image:sources) {
      const auto& map=*image.map;
      for(std::size_t i=0;i<map.states.size();++i) {
        const u64 number=map.first_page+i;
        if(!number||map.states[i]!=page::NativeAllocationState::free||selected[group].contains(number)||
            (preallocation&&map.header.filespace_uuid==preallocation->filespace_uuid&&number>=preallocation->first_page&&number-preallocation->first_page<preallocation->page_count)) {run.clear();continue;}
        auto known=probes[group].find(number);
        if(known==probes[group].end()) {
          charge(1,page_size);
          Require(number<std::numeric_limits<u64>::max()/page_size,E::allocation_mismatch);
          const auto io=(group?secondary_device:context.primary)->ReadAt(number*page_size,probe.data(),page_size);
          Require(io.ok()&&io.bytes_transferred==page_size,E::io_failure);
          const bool empty=std::all_of(probe.begin(),probe.begin()+page_size,[](byte value){return !value;});
          known=probes[group].emplace(number,empty).first;
        }
        if(!known->second) {run.clear();continue;}
        if(!run.empty()&&number-run.back()!=1)run.clear();
        run.push_back(number);
        if(run.size()==count) {selected[group].insert(run.begin(),run.end());return run;}
      }
    }
    throw E::allocation_exhausted;
  };
  struct Partition {std::size_t source;u64 first,count;};
  std::vector<Partition> partitions;
  for(std::size_t i=0;i<source_maps.size();++i){const auto& m=*source_maps[i]->map;
    partitions.push_back({i,m.first_page,m.states.size()});}
  std::vector<u64> extent_slots,bundle_slots,map_slots,inventory_slots,directory_slots;
  u64 plan_slot=0,checkpoint_slot=0;
  for(;;){
    for(auto& slots:selected)slots.clear();map_slots.clear();inventory_slots.clear();directory_slots.clear();
    extent_slots=allocate(extent_count);bundle_slots=allocate(bundle_count);
    plan_slot=allocate(1).front();checkpoint_slot=allocate(1).front();
    for(const auto& part:partitions)map_slots.push_back(allocate(1,source_maps[part.source]->map->header.filespace_uuid!=zero.bootstrap.filespace_uuid).front());
    for(u64 i=0;i<inventory_count;++i)inventory_slots.push_back(allocate(1).front());
    for(u64 i=0;i<directory_count;++i)directory_slots.push_back(allocate(1).front());
    // Placement can add records to any existing range. Refine only: preserving
    // prior cuts bounds convergence even when a larger contiguous bundle moves.
    std::vector<Partition> refined;refined.reserve(partitions.size());
    for(const auto& part:partitions){const auto& m=*source_maps[part.source]->map;
      const unsigned group=m.header.filespace_uuid!=zero.bootstrap.filespace_uuid;const u64 page_size=m.header.page_size_bytes;
      auto record=std::lower_bound(m.records.begin(),m.records.end(),part.first,[](const auto& r,u64 n){return r.page_number<n;});
      const u64 at=(384+(part.count+1)/2+7)&~u64{7};
      bool overflow=at>page_size;u64 records=0;
      for(u64 n=part.first;n-part.first<part.count;++n){
        const bool old=record!=m.records.end()&&record->page_number==n;
        const bool added=selected[group].contains(n)||(preallocation&&m.header.filespace_uuid==preallocation->filespace_uuid&&n>=preallocation->first_page&&n-preallocation->first_page<preallocation->page_count);
        if(old||added)++records;if(old)++record;
        if(overflow||records>(page_size-at)/128){overflow=true;break;}
      }
      if(overflow){Require(part.count>1,E::resource_exhausted);charge(1,10*page_size);
        // A tightly packed prefix can overflow again on every added map's own
        // record. Halving avoids that positive feedback; a fully populated
        // small enough range eventually fits regardless of further placement.
        const auto half=part.count/2;refined.push_back({part.source,part.first,half});
        refined.push_back({part.source,part.first+half,part.count-half});
      }else refined.push_back(part);
    }
    if(refined.size()==partitions.size())break;
    Require(refined.size()>map_count,E::resource_exhausted);
    map_count=refined.size();
    u64 framed=(inventory_count+directory_count)*(directory_publication?size+8:size);
    for(const auto& part:refined)framed+=source_maps[part.source]->map->header.page_size_bytes+(directory_publication?8:0);
    const u64 next_bundle=ceil(framed,payload);
    charge(next_bundle-bundle_count,4*size);bundle_count=next_bundle;partitions=std::move(refined);
  }

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
  for(const auto* image:source_maps)for(const auto& r:image->map->records)
    for(const auto& id:{r.allocation_uuid,r.page_uuid,r.owner_uuid,r.creator_transaction_uuid,r.creator_operation_uuid})retain(id);
  for(const auto& page:context.bound.checkpoint_inventory.inventory_pages){retain(page.header.page_uuid);retain(page.object_uuid);}
  for(const auto& entry:inventory.entries)retain(entry.identity.transaction_uuid.value);
  for(const auto& entry:context.bound.checkpoint_inventory.inventory.entries)retain(entry.identity.transaction_uuid.value);
  for(const auto& image:original_directory.pages){retain(image.directory->header.page_uuid);retain(image.directory->object_uuid);
    retain(image.directory->creator_transaction_uuid);retain(image.directory->creator_operation_uuid);}
  for(const auto& member:directory_records){for(const auto& id:{member.bootstrap.filespace_uuid,member.bootstrap.page_size_profile_uuid,
      member.locator_uuid,member.page_zero_uuid})retain(id);if(member.allocation_root)retain(member.allocation_root->object_uuid);}
  for(const auto& step:record.steps)for(const auto& id:{step.uuid,step.operation_uuid,step.family_uuid,step.target_uuid,
    step.started_at,step.completed_at,step.evidence_uuid,step.metric_evidence_uuid,step.diagnostic_uuid,step.boundary_uuid})retain(id);
  if(preallocation)retain(preallocation->allocation_owner_uuid);
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
  for(const auto& part:partitions){const auto& source=*source_maps[part.source]->map;
    auto map=source;map.first_page=part.first;
    const auto begin=part.first-source.first_page;
    map.states.assign(source.states.begin()+begin,source.states.begin()+begin+part.count);
    const auto first=std::lower_bound(source.records.begin(),source.records.end(),part.first,[](const auto& r,u64 n){return r.page_number<n;});
    const auto last=std::lower_bound(first,source.records.end(),part.first+part.count,[](const auto& r,u64 n){return r.page_number<n;});
    map.records.assign(first,last);maps.push_back(std::move(map));}
  if(preallocation){
    for(const auto& map:maps){
      if(map.header.filespace_uuid!=preallocation->filespace_uuid)continue;
      const u64 page_size=map.header.page_size_bytes;
      const u64 begin=std::max(map.first_page,preallocation->first_page);
      const u64 end=std::min(map.first_page+map.states.size(),preallocation->first_page+preallocation->page_count);
      const u64 records_at=(384+(map.states.size()+1)/2+7)&~u64{7};
      Require(records_at<=page_size&&map.records.size()<=(page_size-records_at)/128,E::resource_exhausted);
      Require(end<=begin||end-begin<=(page_size-records_at)/128-map.records.size(),E::resource_exhausted);
    }
    for(u64 n=preallocation->first_page;n-preallocation->first_page<preallocation->page_count;++n){
      const auto found=std::find_if(maps.begin(),maps.end(),[&](const auto& m){return m.header.filespace_uuid==preallocation->filespace_uuid&&n>=m.first_page&&n-m.first_page<m.states.size();});
      Require(found!=maps.end(),E::allocation_mismatch);auto& map=*found;
      Require(n-map.first_page<map.states.size()&&map.states[n-map.first_page]==page::NativeAllocationState::free,E::allocation_mismatch);
      const auto at=std::lower_bound(map.records.begin(),map.records.end(),n,[](const auto& r,u64 number){return r.page_number<number;});
      Require(at==map.records.end()||at->page_number!=n,E::allocation_mismatch);
      page::NativeAllocationRecord r;r.page_number=n;r.allocation_uuid=issue(UuidKind::object);
      r.owner_uuid=preallocation->allocation_owner_uuid;r.page_type=preallocation->allocation_page_type;
      r.creator_operation_uuid=watermark.operation_uuid;map.records.insert(at,r);map.states[n-map.first_page]=page::NativeAllocationState::preallocated;
    }
  }
  const auto control=[&](u64 number,u32 type,const Uuid& owner,unsigned group=0) {
    const auto fs=group?preallocation->filespace_uuid:zero.bootstrap.filespace_uuid;
    disk::NativeCommonPageHeader h{u32(group?secondary_size:size),type,zero.bootstrap.database_uuid,fs,
      issue(UuidKind::page),number,watermark.watermark,0,group?preallocation->page_size_profile_uuid:zero.bootstrap.page_size_profile_uuid};
    const auto found=std::find_if(maps.begin(),maps.end(),[&](const auto& m){return m.header.filespace_uuid==fs&&number>=m.first_page&&number-m.first_page<m.states.size();});
    Require(found!=maps.end(),E::allocation_mismatch);auto& map=*found;
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
  std::vector<disk::NativeCommonPageHeader> extent_headers,bundle_headers,inventory_headers,directory_headers;
  for(const auto slot:extent_slots)extent_headers.push_back(control(slot,0x500,extent_object));
  for(const auto slot:bundle_slots)bundle_headers.push_back(control(slot,0x500,bundle_object));
  for(std::size_t i=0;i<maps.size();++i)maps[i].header=control(map_slots[i],3,maps[i].object_uuid,maps[i].header.filespace_uuid!=zero.bootstrap.filespace_uuid);
  for(const auto slot:inventory_slots)inventory_headers.push_back(control(slot,0x301,inventory_root.object_uuid));
  for(const auto slot:directory_slots)directory_headers.push_back(control(slot,9,directory_root.object_uuid));
  Pages map_images(maps.size()),inventory_images(inventory_count);
  for(std::size_t i=maps.size();i--;) {
    auto& map=maps[i];map.map_generation=watermark.watermark;
    map.creator_transaction_uuid={};map.creator_local_transaction_id=0;map.creator_operation_uuid=watermark.operation_uuid;
    const bool next=i+1<maps.size()&&maps[i+1].header.filespace_uuid==map.header.filespace_uuid;
    map.next=next?std::optional{ControlRef(maps[i+1].header)}:std::nullopt;
    map.next_sha256=next?ControlHash(map_images[i+1]):std::array<byte,32>{};
    const u64 records_at=(384+(map.states.size()+1)/2+7)&~u64{7};
    const u64 page_size=map.header.page_size_bytes;
    Require(records_at<=page_size&&map.records.size()<=(page_size-records_at)/128,E::resource_exhausted);
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
  Pages directory_images(directory_count);
  if(directory_publication){
    for(auto& member:directory_records)for(std::size_t i=0;i<maps.size();++i)if(member.bootstrap.filespace_uuid==maps[i].header.filespace_uuid&&!maps[i].first_page)
      member.allocation_root=page::NativeFilespaceAllocationRoot{ControlRef(maps[i].header),maps[i].object_uuid,
        ControlHash(map_images[i]),maps[i].map_generation,maps[i].capacity_generation};
    const u64 per_directory=(size-384)/320;
    for(std::size_t i=directory_count;i--;){page::NativeFilespaceDirectory image;
      image.header=directory_headers[i];image.object_uuid=directory_root.object_uuid;image.directory_generation=watermark.watermark;
      image.creator_operation_uuid=watermark.operation_uuid;image.total_records=directory_records.size();image.first_record=i*per_directory;
      const auto end=std::min<u64>(directory_records.size(),image.first_record+per_directory);
      image.records.assign(directory_records.begin()+image.first_record,directory_records.begin()+end);
      if(i+1<directory_count){image.next=ControlRef(directory_headers[i+1]);image.next_sha256=ControlHash(directory_images[i+1]);}
      auto encoded=page::EncodeNativeFilespaceDirectory(image);
      if(!encoded.ok())throw encoded.error==page::NativeDirectoryError::resource_exhausted?E::resource_exhausted:
        encoded.error==page::NativeDirectoryError::hash_failure?E::hash_failure:E::image_failure;
      directory_images[i]=std::move(encoded.bytes);
    }
  }
  // The bundle uses binary filespace order, independently of primary ownership.
  std::vector<std::size_t> map_order;map_order.reserve(maps.size());
  for(std::size_t i=0;i<maps.size();++i)map_order.push_back(i);
  std::sort(map_order.begin(),map_order.end(),[&](std::size_t a,std::size_t b){
    return maps[a].header.filespace_uuid==maps[b].header.filespace_uuid?maps[a].first_page<maps[b].first_page:
      maps[a].header.filespace_uuid<maps[b].header.filespace_uuid;});
  Pages ordered_maps;ordered_maps.reserve(maps.size());
  for(const auto i:map_order)ordered_maps.push_back(map_images[i]);
  auto bundle=EncodeNativeManagementControlBundle(ordered_maps,zero.bootstrap.database_uuid,zero.page_uuid,
    bundle_object,watermark.operation_uuid,bundle_headers,budget,inventory_images,directory_images);ControlBundleError(bundle.error);
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
    if(r.role==1&&!preallocation)r={1,0x301,ControlRef(inventory_headers.front()),inventory_root.object_uuid,ControlHash(inventory_images.front())};
    if(r.role==4)r={4,3,ControlRef(maps.front().header),maps.front().object_uuid,ControlHash(map_images.front())};
    if(r.role==3&&directory_publication)r={3,9,ControlRef(directory_headers.front()),directory_root.object_uuid,ControlHash(directory_images.front())};
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
  const auto action=[&]() -> NativePublicationInspection {
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

  };
  auto result=action();result.effects=lease.effects();return result;
}

NativePreallocationPublicationResult PublishNativePreallocationOnLease(NativePublicationLease& lease,
    const NativeManagementOperation& record,u64 budget,core::uuid::StandaloneUuidV7Issuer& issuer) noexcept {
  NativePreallocationPublicationResult result;
  try {
    Require(!lease.impl_->installation_ambiguous,E::stale_base);
    const auto request=ReadNativeStorageActionIntentFromOperation(record,budget);
    if(!request.ok())throw request.error==NativeStorageIntentError::resource_exhausted?E::resource_exhausted:
      request.error==NativeStorageIntentError::hash_failure?E::hash_failure:E::invalid_request;
    const auto& intent=*request.intent;
    const auto& old=*lease.impl_->context;
    Require(intent.action==NativeStorageAction::page_preallocation&&
      intent.checkpoint.filespace_uuid==old.zero.bootstrap.filespace_uuid&&
      lease.snapshot().watermark.intent&&lease.snapshot().watermark.intent->recovery_profile==3,E::invalid_request);
    Require(issuer.binding().database_uuid==intent.database_uuid&&
      issuer.binding().policy_snapshot_uuid==intent.policy_snapshot_uuid,E::request_mismatch);
    const u64 allowance=std::min(budget,intent.maximum_retained_image_bytes);
    Require(allowance>kNativeStorageActionIntentBytes+intent.page_size_bytes,E::resource_exhausted);
    {
      const auto prior=ReconcileNativePreallocationFromOpenDevices(intent.database_uuid,old.devices,
        old.zero.bootstrap.filespace_uuid,record,allowance-kNativeStorageActionIntentBytes);
      if(!prior.ok())throw prior.error;
      const auto& observed=*prior.observation;
      Require((observed.disposition==NativePreallocationDisposition::pending_unanchored||
        observed.disposition==NativePreallocationDisposition::pending_anchored)&&
        observed.publication_attempt_uuid==lease.snapshot().watermark.operation_uuid,E::request_mismatch);
      Require(SameBase(observed.snapshot,lease.snapshot()),E::stale_base);
    }
    // A retained proposal is never authority for stale selected capacity.
    const auto matched=CheckNativeStorageIntentCapacityFromOpenDevices(intent,old.devices,allowance);
    if(!matched.ok())throw CapacityFailure(matched.capacity);
    Require(matched.capacity.retained_image_bytes<=allowance-kNativeStorageActionIntentBytes-intent.page_size_bytes,E::resource_exhausted);
    u64 work_budget=allowance-matched.capacity.retained_image_bytes-kNativeStorageActionIntentBytes-intent.page_size_bytes;
    auto current=Prepare(intent.database_uuid,old.devices,old.zero.bootstrap.filespace_uuid,work_budget,true,false);
    Require(SameBase(lease.snapshot(),current->snapshot),E::stale_base);
    const auto target=std::find_if(current->devices.begin(),current->devices.end(),[&](const auto& f){return f.filespace_uuid==intent.filespace_uuid;});
    Require(target!=current->devices.end()&&!target->device->read_only(),E::invalid_device);
    page::NativeAllocationChainResult secondary_maps;
    if(target->device!=current->primary){
      const disk::FilespaceBootstrapBinding binding{intent.database_uuid,intent.filespace_uuid,intent.page_size_profile_uuid};
      secondary_maps=page::ReadNativeAllocationChainAtRootFromOpenDevice(*target->device,binding,intent.allocation_root,work_budget);
      if(!secondary_maps.ok())throw secondary_maps.error==page::NativeAllocationError::resource_exhausted?E::resource_exhausted:
        secondary_maps.error==page::NativeAllocationError::hash_failure?E::hash_failure:secondary_maps.error==page::NativeAllocationError::io_failure?E::io_failure:E::allocation_mismatch;
      Require(ControlHash(secondary_maps.pages.front().bytes)==intent.allocation_sha256,E::binding_mismatch);
      Require(secondary_maps.retained_image_bytes<=work_budget/2,E::resource_exhausted);
      work_budget-=2*secondary_maps.retained_image_bytes;
    }
    if(current->zero.bootstrap.page_size_bytes>intent.page_size_bytes){
      const u64 extra=current->zero.bootstrap.page_size_bytes-intent.page_size_bytes;
      Require(extra<work_budget,E::resource_exhausted);work_budget-=extra;
    }
    const auto& target_maps=target->device==current->primary?current->bound.allocation.pages:secondary_maps.pages;
    Bytes probe(intent.page_size_bytes);
    for(u64 n=intent.first_page;n-intent.first_page<intent.page_count;++n){
      const auto image=std::upper_bound(target_maps.begin(),target_maps.end(),n,
        [](u64 number,const auto& p){return number<p.map->first_page;});
      Require(image!=target_maps.begin(),E::allocation_mismatch);const auto& map=*std::prev(image)->map;
      Require(n-map.first_page<map.states.size()&&map.states[n-map.first_page]==page::NativeAllocationState::free&&
        std::none_of(map.records.begin(),map.records.end(),[&](const auto& r){return r.page_number==n;}),E::allocation_mismatch);
      const auto io=target->device->ReadAt(n*intent.page_size_bytes,probe.data(),probe.size());
      Require(io.ok()&&io.bytes_transferred==probe.size(),E::io_failure);
      Require(std::all_of(probe.begin(),probe.end(),[](byte value){return !value;}),E::preimage_changed);
    }
    if(current->snapshot.watermark.publication_plan){
      const auto& anchor=*current->snapshot.watermark.publication_plan;
      probe.resize(current->zero.bootstrap.page_size_bytes);
      const auto io=current->primary->ReadAt(anchor.page.page_number*u64{current->zero.bootstrap.page_size_bytes},probe.data(),probe.size());
      Require(io.ok()&&io.bytes_transferred==probe.size(),E::io_failure);
      const auto plan=DecodeNativePublicationPlan(probe);ControlPlanError(plan.error);
      Require(plan.plan->intent.recovery_profile==3,E::request_mismatch);
      ControlPlanError(BindNativePublicationPlanToManagementRecord(*plan.plan,record));
      result.publication=ResumeNativeManagementControlGraphOnLease(lease,work_budget);
    }else{
      auto graph=AssembleInventory(*current,record,current->bound.checkpoint_inventory.inventory,work_budget,issuer,&intent);
      result.publication=InstallNativeManagementControlGraphOnLease(lease,graph.plan,graph.checkpoint,graph.extent,graph.bundle,work_budget);
    }
    if(!result.publication.ok())return result;
    // From here an exception is an attempted physical operation with unknown
    // effects, not a fabricated no-effect refusal. Never rebuild this attempt.
    lease.impl_->installation_ambiguous=true;
    result.physical_attempted=true;
    result.physical=target->device->PreallocateExtent(intent.first_page*u64{intent.page_size_bytes},
      intent.page_count*u64{intent.page_size_bytes});
    Require(result.physical->ok()&&!result.physical->logical_size_extended,E::io_failure);
    current->effects=&lease.impl_->effects;EffectSync(*current,*target->device);
    result.physical_sync_completed=true;
    lease.impl_->installation_ambiguous=false;
    result.publication=PublishNativeManagementControlGraphOnLease(lease,work_budget);
  }catch(E e){result.publication.error=e;result.publication.snapshot.reset();}
   catch(const std::bad_alloc&){result.publication.error=E::resource_exhausted;result.publication.snapshot.reset();}
   catch(const std::length_error&){result.publication.error=E::resource_exhausted;result.publication.snapshot.reset();}
   catch(...){result.publication.error=E::io_failure;result.publication.snapshot.reset();}
  result.publication.effects=lease.effects();return result;
}
NativePreallocationReconciliation ReconcileNativePreallocationFromOpenDevices(
    const Uuid& database,const std::vector<disk::NativeFilespaceDevice>& devices,
    const Uuid& primary,const NativeManagementOperation& record,u64 budget) noexcept {
  try {
    const auto original=ReadNativeStorageActionIntentFromOperation(record,budget);
    if(!original.ok())throw original.error==NativeStorageIntentError::resource_exhausted?E::resource_exhausted:
      original.error==NativeStorageIntentError::hash_failure?E::hash_failure:E::invalid_request;
    const auto& request=*original.intent;
    Require(request.action==NativeStorageAction::page_preallocation&&request.database_uuid==database&&
      request.checkpoint.filespace_uuid==primary,E::invalid_request);
    Require(budget>kNativeStorageActionIntentBytes,E::resource_exhausted);
    auto current=Prepare(database,devices,primary,budget-kNativeStorageActionIntentBytes,false,false);
    const u64 retained=current->bound.retained_image_bytes+6*u64{current->zero.bootstrap.page_size_bytes};
    Require(retained<budget-kNativeStorageActionIntentBytes,E::resource_exhausted);
    const auto history=ReadNativeManagementHistoryFromOpenDevices(database,current->devices,primary,
      budget-kNativeStorageActionIntentBytes-retained);
    if(!history.ok()){using H=NativeManagementHistoryError;
      throw history.error==H::resource_exhausted?E::resource_exhausted:history.error==H::hash_failure?E::hash_failure:
        history.error==H::io_failure?E::io_failure:history.error==H::encrypted_requires_authority?E::encrypted_requires_authority:
        history.error==H::cluster_requires_authority?E::cluster_requires_authority:E::binding_mismatch;
    }
    Require(history.selection->checkpoint==current->snapshot.selection.checkpoint&&
      history.selection->checkpoint_sha256==current->snapshot.selection.checkpoint_sha256,E::stale_base);
    NativePreallocationObservation observed;
    observed.request_uuid=request.request_uuid;observed.operation_uuid=request.operation_uuid;
    observed.snapshot=current->snapshot;
    for(const auto& entry:history.entries){
      if(entry.plan.intent.recovery_profile!=3)continue;
      const auto retained_request=ReadNativeStorageActionIntentFromOperation(entry.record,budget);
      if(!retained_request.ok())throw retained_request.error==NativeStorageIntentError::resource_exhausted?E::resource_exhausted:
        retained_request.error==NativeStorageIntentError::hash_failure?E::hash_failure:E::binding_mismatch;
      const auto& actual=*retained_request.intent;
      if(actual.request_uuid!=request.request_uuid&&actual.operation_uuid!=request.operation_uuid)continue;
      Require(actual.request_uuid==request.request_uuid&&actual.operation_uuid==request.operation_uuid&&
        entry.record.normalized_request_bytes==record.normalized_request_bytes,E::request_mismatch);
      Require(observed.disposition!=NativePreallocationDisposition::selected,E::binding_mismatch);
      observed.disposition=NativePreallocationDisposition::selected;
      observed.publication_attempt_uuid=entry.plan.operation_uuid;
      observed.publication_generation=entry.plan.reserved_generation;
      observed.root_set_generation=entry.plan.target_root_set_generation;
    }
    const auto& pending=current->snapshot.watermark;
    if(observed.disposition!=NativePreallocationDisposition::selected&&
        pending.watermark>current->snapshot.selection.checkpoint_generation&&!pending.abandonment){
      const NativePublicationIntent expected{record.initiator_uuid,record.request_context_uuid,
        record.policy_snapshot_uuid,record.normalized_request_sha256,record.initiator_kind,3};
      if(pending.intent&&pending.intent->recovery_profile==3&&pending.publication_plan){
        const auto& anchor=*pending.publication_plan;const u64 size=current->zero.bootstrap.page_size_bytes;
        Require(history.verified_image_bytes<=budget-kNativeStorageActionIntentBytes-retained,E::resource_exhausted);
        const u64 remaining=budget-kNativeStorageActionIntentBytes-retained-history.verified_image_bytes;
        Require(size<=remaining/2,E::resource_exhausted);Bytes raw(size);
        Require(anchor.page.filespace_uuid==primary&&anchor.page.page_size_profile_uuid==current->zero.bootstrap.page_size_profile_uuid&&
          anchor.page.page_number<current->zero.total_pages,E::binding_mismatch);
        const auto io=current->primary->ReadAt(anchor.page.page_number*size,raw.data(),raw.size());
        Require(io.ok()&&io.bytes_transferred==raw.size(),E::io_failure);
        const auto image=DecodeNativePublicationPlan(raw);ControlPlanError(image.error);const auto& plan=*image.plan;
        Require(ControlRef(plan.header)==anchor.page&&plan.object_uuid==anchor.object_uuid&&image.sha256==anchor.sha256&&
          plan.intent==*pending.intent&&plan.operation_uuid==pending.operation_uuid&&plan.reserved_generation==pending.watermark&&
          plan.base_checkpoint==pending.base_checkpoint&&plan.base_checkpoint_sha256==pending.base_checkpoint_sha256&&
          plan.management_extent.has_value(),E::binding_mismatch);
        const auto allowance=ExtentAllowance(*plan.management_extent,size,remaining-2*size);
        const disk::NativeFilespaceDevice file{primary,current->zero.bootstrap.page_size_profile_uuid,current->primary};
        const auto extent=ReadNativeManagementExtentFromOpenDevice(file,*plan.management_extent,database,plan.bootstrap_uuid,allowance);
        ControlExtentError(extent.error);ControlPlanError(BindNativePublicationPlanToManagementRecord(plan,*extent.record));
        const auto actual=ReadNativeStorageActionIntentFromOperation(*extent.record,remaining);
        if(!actual.ok())throw actual.error==NativeStorageIntentError::resource_exhausted?E::resource_exhausted:
          actual.error==NativeStorageIntentError::hash_failure?E::hash_failure:E::binding_mismatch;
        if(actual.intent->request_uuid==request.request_uuid||actual.intent->operation_uuid==request.operation_uuid)
          Require(actual.intent->request_uuid==request.request_uuid&&actual.intent->operation_uuid==request.operation_uuid&&
            extent.record->normalized_request_bytes==record.normalized_request_bytes,E::request_mismatch);
      }
      if(pending.intent&&*pending.intent==expected){
        observed.disposition=pending.publication_plan?NativePreallocationDisposition::pending_anchored:
          NativePreallocationDisposition::pending_unanchored;
        observed.publication_attempt_uuid=pending.operation_uuid;
        observed.publication_generation=pending.watermark;
      }else observed.disposition=NativePreallocationDisposition::other_operation_pending;
    }
    return {E::none,std::move(observed)};
  }catch(E e){return {e,{}};}catch(const std::bad_alloc&){return {E::resource_exhausted,{}};}
   catch(const std::length_error&){return {E::resource_exhausted,{}};}catch(...){return {E::io_failure,{}};}
}
} // namespace scratchbird::storage::database
