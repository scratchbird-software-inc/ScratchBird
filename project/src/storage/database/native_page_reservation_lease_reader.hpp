// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_allocation_lease_reader.hpp"
#include "native_page_reservation_intent.hpp"

namespace scratchbird::storage::database {
enum class NativePageReservationReadError {
 none,invalid_request,memory_failure,resource_exhausted,intent_failure,binding_mismatch,
 source_mismatch,transaction_not_writable,allocation_failure,identity_collision,
 page_not_free,page_not_zero,io_failure,physical_extent_changed,lock_failure
};
struct NativePageReservationPreparedResult;
class NativePageReservationPreparedMemory {
 public:
  NativePageReservationPreparedMemory() noexcept=default;
  NativePageReservationPreparedMemory(const NativePageReservationPreparedMemory&)=delete;
  NativePageReservationPreparedMemory& operator=(const NativePageReservationPreparedMemory&)=delete;
  NativePageReservationPreparedMemory(NativePageReservationPreparedMemory&&) noexcept=default;
  NativePageReservationPreparedMemory& operator=(NativePageReservationPreparedMemory&&) noexcept=default;
  explicit operator bool() const noexcept{return bool(arena_)&&bool(allocation_);}
 private:
  NativeStorageArena arena_;
  NativeAllocationPreparedMemory allocation_;
  std::span<NativePageReservationAssignment> pages_;
  std::span<Uuid> scratch_;
  std::span<byte> probe_;
  friend class NativePageReservationLeaseReader;
  friend NativePageReservationPreparedResult PrepareNativePageReservationRead(
      usize,usize,usize,NativeStorageMemory&,const NativeStorageMemoryBinding&) noexcept;
};
// Byte count for the request/probe arena; add the separately bounded allocation
// chain arena. All multiplication/alignment is checked before resource entry.
inline usize NativePageReservationReadBytes(usize pages,usize steps) noexcept {
 const auto max=std::numeric_limits<usize>::max();
 if(!pages||pages>max/2||steps>(max-7)/2)return 0;
 const auto scratch=std::max(2*pages,2*steps+7);
 usize bytes=0;
 for(const auto& p:disk::kCanonicalFilespacePageProfiles)bytes=std::max<usize>(bytes,p.page_size_bytes);
 for(const auto [count,width]:{std::pair{pages,sizeof(NativePageReservationAssignment)},std::pair{scratch,sizeof(Uuid)}}){
  if(max-bytes<alignof(std::max_align_t)||count>(max-bytes-alignof(std::max_align_t))/width)return 0;
  bytes+=count*width+alignof(std::max_align_t);
 }
 return bytes;
}
struct NativePageReservationPreparedResult {
 NativePageReservationReadError error=NativePageReservationReadError::invalid_request;
 NativeStorageMemoryError memory_error=NativeStorageMemoryError::none;
 core::platform::Status allocation_status;
 core::platform::DiagnosticRecord allocation_diagnostic;
 NativePageReservationPreparedMemory workspace;
 bool ok() const noexcept{return error==NativePageReservationReadError::none&&bool(workspace);}
};
// Must run before acquiring a device/source guard. Manager and both actual
// grants outlive the source. No fallback heap or grant operation inside Read.
inline NativePageReservationPreparedResult PrepareNativePageReservationRead(
    usize pages,usize steps,usize allocation_bytes,NativeStorageMemory& memory,
    const NativeStorageMemoryBinding& binding) noexcept {
 using E=NativePageReservationReadError;NativePageReservationPreparedResult out;
 try{
  const auto bytes=NativePageReservationReadBytes(pages,steps);
  if(!bytes||!allocation_bytes)return out;
  auto arena=memory.CreateArena(binding,bytes,alignof(std::max_align_t));
  out.memory_error=arena.error;out.allocation_status=arena.backing.status;
  out.allocation_diagnostic=std::move(arena.backing.diagnostic);
  if(!arena.ok()){out.error=E::memory_failure;return out;}
  auto maps=PrepareNativeAllocationRead(allocation_bytes,memory,binding);
  out.memory_error=maps.memory_error;out.allocation_status=maps.allocation_status;
  out.allocation_diagnostic=std::move(maps.allocation_diagnostic);
  if(!maps.ok()){out.error=E::memory_failure;return out;}
  auto& w=out.workspace;w.arena_=std::move(arena.arena);w.allocation_=std::move(maps.workspace);
  const auto array=[&]<class T>(usize count)->std::span<T>{
   const auto a=w.arena_.Allocate(count*sizeof(T),alignof(T));if(!a.ok())throw std::bad_alloc();
   auto* p=static_cast<T*>(a.pointer);std::uninitialized_value_construct_n(p,count);return {p,count};};
  usize probe=0;for(const auto& p:disk::kCanonicalFilespacePageProfiles)probe=std::max<usize>(probe,p.page_size_bytes);
  w.probe_=array.template operator()<byte>(probe);
  w.pages_=array.template operator()<NativePageReservationAssignment>(pages);
  w.scratch_=array.template operator()<Uuid>(std::max(2*pages,2*steps+7));
  out.error=E::none;
 }catch(const std::bad_alloc&){out.workspace={};out.error=E::resource_exhausted;}
  catch(const std::length_error&){out.workspace={};out.error=E::resource_exhausted;}
  catch(const std::system_error&){out.workspace={};out.error=E::lock_failure;}
 return out;
}
struct NativePageReservationReadResult {
 NativePageReservationReadError error=NativePageReservationReadError::invalid_request;
 NativePageReservationIntentError intent_error=NativePageReservationIntentError::none;
 NativeManagementOperationError operation_error=NativeManagementOperationError::none;
 NativeAllocationLeaseError allocation_error=NativeAllocationLeaseError::none;
 page::NativeAllocationError chain_error=page::NativeAllocationError::none;
 core::platform::Status io_status;
 core::platform::DiagnosticRecord io_diagnostic;
 u64 physical_bytes_read=0,pages_observed=0;
 bool ok() const noexcept{return error==NativePageReservationReadError::none&&pages_observed;}
};
// Observation ONLY: no returned reservation, allocation authority, policy/role
// admission, durable effect, or reusable capability. A publisher must recheck
// under its mutation fence. Request/operation material is immutable during this
// call; source and backing are privately owned, thread-affine and retained.
class NativePageReservationLeaseReader {
 public:
 static NativePageReservationReadResult Read(const NativeSelectedCheckpointMemoryLease& lease,
     const NativeManagementOperationView& operation,u64 maximum_image_bytes,
     NativePageReservationPreparedMemory& backing) noexcept {
  using E=NativePageReservationReadError;NativePageReservationReadResult out;
  const auto fail=[&](E e){out.error=e;out.pages_observed=0;return std::move(out);};
  try{
   lease.CheckThread();if(!backing)return out;
   const auto decoded=ReadNativePageReservationIntentFromOperationView(operation,backing.pages_,backing.scratch_,maximum_image_bytes);
   out.intent_error=decoded.error;out.operation_error=decoded.operation_error;
   if(!decoded.ok())return fail(E::intent_failure);
   const auto& i=*decoded.intent;const auto& b=backing.arena_.binding();
   if(b.database_uuid!=i.database_uuid||b.operation_uuid!=i.operation_uuid||
      b.owner_uuid!=i.initiator_uuid||b.context_uuid!=i.request_context_uuid)return fail(E::binding_mismatch);
   const auto& s=*lease.selection_.selection;
   const auto& cp=*lease.selection_.checkpoint_inventory.checkpoint;
   if(cp.header.database_uuid!=i.database_uuid||s.bootstrap_uuid!=operation.bootstrap_uuid||
      s.checkpoint!=i.checkpoint||s.checkpoint_object_uuid!=i.checkpoint_object_uuid||
      s.checkpoint_sha256!=i.checkpoint_sha256||s.selection_generation!=i.selection_generation||
      s.checkpoint_generation!=i.checkpoint_generation||s.root_set_generation!=i.checkpoint_root_set_generation||
      lease.directory_.directory.pages.front().directory.directory_generation!=i.directory_generation)return fail(E::source_mismatch);
   const auto& inventory=lease.selection_.checkpoint_inventory.inventory;
   const auto tx=std::find_if(inventory.entries.begin(),inventory.entries.end(),[&](const auto& e){
    return e.identity.local_id.value==i.local_transaction_id;});
   using namespace transaction::mga;
   if(tx==inventory.entries.end()||tx->identity.transaction_uuid.value!=i.transaction_uuid||
      tx->identity.scope!=TransactionScope::local_node||tx->state!=TransactionState::active||tx->rollback_only)
    return fail(E::transaction_not_writable);
   const page::NativeFilespaceDirectoryRecord* target=nullptr;
   for(const auto& p:lease.directory_.directory.pages)for(const auto& r:p.directory.records)
    if(r.bootstrap.filespace_uuid==i.filespace_uuid)target=&r;
   if(!target||target->bootstrap.page_size_profile_uuid!=i.page_size_profile_uuid||
      target->locator_uuid!=i.locator_uuid||target->page_zero_uuid!=i.page_zero_uuid||
      target->page_zero_generation!=i.page_zero_generation||target->root_set_generation!=i.filespace_root_set_generation||
      target->total_pages!=i.total_pages)return fail(E::source_mismatch);
   // Decoding sorted all newly assigned identities into scratch. Compare binary
   // identities in every retained member, not merely the requested member.
   const auto fresh=backing.scratch_.first(2*i.pages.size());
   const auto collision=[&](const Uuid& id){return std::binary_search(fresh.begin(),fresh.end(),id);};
   for(const auto& e:inventory.entries)if(collision(e.identity.transaction_uuid.value))return fail(E::identity_collision);
   for(const auto& p:lease.directory_.directory.pages)for(const auto& member:p.directory.records){
    if(collision(member.bootstrap.filespace_uuid)||collision(member.locator_uuid)||collision(member.page_zero_uuid))return fail(E::identity_collision);
    auto read=NativeAllocationLeaseReader::Read(lease,member.bootstrap.filespace_uuid,maximum_image_bytes,backing.allocation_);
    out.physical_bytes_read+=read.physical_bytes_read;out.allocation_error=read.error;out.chain_error=read.chain_error;
    out.io_status=read.io_status;out.io_diagnostic=std::move(read.io_diagnostic);
    if(!read.ok())return fail(E::allocation_failure);
    const auto& root=*read.root;const auto& head=read.chain.pages.front().map;
    if(member.bootstrap.filespace_uuid==i.filespace_uuid){
     if(root.page.page_number!=i.allocation_page_number||root.page.page_generation!=i.allocation_page_generation||
        root.object_uuid!=i.allocation_object_uuid||root.sha256!=i.allocation_sha256||
        root.map_generation!=i.map_generation||root.capacity_generation!=i.capacity_generation||head.total_pages!=i.total_pages)
      return fail(E::source_mismatch);
     for(const auto& a:i.pages){bool free=false;
      for(const auto& p:read.chain.pages){const auto& m=p.map;
       if(a.page_number>=m.first_page&&a.page_number-m.first_page<m.states.size())
        free=m.states[a.page_number-m.first_page]==page::NativeAllocationState::free;
       for(const auto& r:m.records)if(r.page_number==a.page_number)return fail(E::page_not_free);
      }
      if(!free)return fail(E::page_not_free);
     }
    }
    for(const auto& p:read.chain.pages){const auto& m=p.map;
     if(collision(m.object_uuid)||collision(m.header.page_uuid)||collision(m.creator_transaction_uuid)||collision(m.creator_operation_uuid))return fail(E::identity_collision);
     for(const auto& r:m.records)for(const auto& id:{r.allocation_uuid,r.page_uuid,r.owner_uuid,r.creator_transaction_uuid,r.creator_operation_uuid})
      if(collision(id))return fail(E::identity_collision);
    }
   }
   const auto f=std::find_if(lease.devices_.begin(),lease.devices_.end(),[&](const auto& f){return f.filespace_uuid==i.filespace_uuid;});
   if(f==lease.devices_.end())return fail(E::source_mismatch);
   auto& device=*f->device;auto& batch=lease.batches_[f-lease.devices_.begin()];
   const auto size=disk::FindCanonicalFilespacePageProfile(i.page_size_profile_uuid)->page_size_bytes;
   auto before=device.Size();out.io_status=before.status;out.io_diagnostic=std::move(before.diagnostic);
   if(!before.ok())return fail(E::io_failure);
   if(before.size_bytes!=i.total_pages*size)return fail(E::physical_extent_changed);
   for(const auto& a:i.pages){
    const auto offset=a.page_number*size;if(!disk::CheckFileDeviceExtent(offset,size).ok())return fail(E::invalid_request);
    auto io=batch.ReadAt(offset,backing.probe_.data(),size);out.physical_bytes_read+=io.bytes_transferred;
    out.io_status=io.status;out.io_diagnostic=std::move(io.diagnostic);
    if(!io.ok()||io.bytes_transferred!=size)return fail(E::io_failure);
    if(!native_page_reservation_detail::Zero(backing.probe_.first(size)))return fail(E::page_not_zero);
   }
   auto after=device.Size();out.io_status=after.status;out.io_diagnostic=std::move(after.diagnostic);
   if(!after.ok())return fail(E::io_failure);
   if(after.size_bytes!=before.size_bytes)return fail(E::physical_extent_changed);
   out.error=E::none;out.pages_observed=i.pages.size();return out;
  }catch(const std::bad_alloc&){return fail(E::resource_exhausted);}
   catch(const std::length_error&){return fail(E::resource_exhausted);}
   catch(const std::system_error&){return fail(E::lock_failure);}
 }
};
} // namespace scratchbird::storage::database
