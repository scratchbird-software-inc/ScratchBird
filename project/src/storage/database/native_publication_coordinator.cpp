// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_publication_coordinator.hpp"
#include "disk_device.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <limits>
#include <mutex>
#include <new>
#include <set>
#include <stdexcept>

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
    c->bound.error==NativeCheckpointSelectionError::resource_exhausted?E::resource_exhausted:E::checkpoint_failure;
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
      c->decoded[0].state->watermark>c->decoded[1].state->watermark,E::image_failure);
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
struct NativePublicationLease::Impl {std::unique_ptr<Context> context;NativePublicationSnapshot snapshot;};
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
    const NativePublicationSnapshot& expected,const Uuid& operation,u64 budget) noexcept {
  try {
    Require(V7(operation),E::invalid_request);auto c=Prepare(db,devices,primary,budget,true,false);
    Require(SameBase(expected,c->snapshot),E::stale_base);
    auto w=c->snapshot.watermark;
    Require(operation!=w.operation_uuid&&operation!=c->snapshot.selection.publication_uuid,E::invalid_request);
    Require(w.watermark!=std::numeric_limits<u64>::max(),E::generation_exhausted);
    w.previous_watermark=w.watermark;++w.watermark;w.previous_state_sha256=c->snapshot.state_sha256;w.operation_uuid=operation;
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
} // namespace scratchbird::storage::database
