// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_selected_checkpoint_memory_lease.hpp"
#include "native_allocation_chain_backing.hpp"
#include "hash_digest.hpp"

namespace scratchbird::storage::database {
enum class NativeAllocationLeaseError {
 none,invalid_request,memory_failure,resource_exhausted,member_not_found,
 binding_mismatch,chain_failure,hash_failure,integrity_failure,lock_failure
};
struct NativeAllocationPreparedResult;
class NativeAllocationPreparedMemory {
 public:
  NativeAllocationPreparedMemory() noexcept=default;
  NativeAllocationPreparedMemory(const NativeAllocationPreparedMemory&)=delete;
  NativeAllocationPreparedMemory& operator=(const NativeAllocationPreparedMemory&)=delete;
  NativeAllocationPreparedMemory(NativeAllocationPreparedMemory&&) noexcept=default;
  NativeAllocationPreparedMemory& operator=(NativeAllocationPreparedMemory&&) noexcept=default;
  explicit operator bool() const noexcept{return bool(arena_);}
 private:
  NativeStorageArena arena_;
  std::span<byte> bytes_;
  friend class NativeAllocationLeaseReader;
  friend NativeAllocationPreparedResult PrepareNativeAllocationRead(
      usize,NativeStorageMemory&,const NativeStorageMemoryBinding&) noexcept;
};
struct NativeAllocationPreparedResult {
 NativeAllocationLeaseError error=NativeAllocationLeaseError::invalid_request;
 NativeStorageMemoryError memory_error=NativeStorageMemoryError::none;
 core::platform::Status allocation_status;
 core::platform::DiagnosticRecord allocation_diagnostic;
 NativeAllocationPreparedMemory workspace;
 bool ok() const noexcept{return error==NativeAllocationLeaseError::none&&bool(workspace);}
};
// Admit the entire bounded chain backing before acquiring the source. There is
// no heap fallback; the chain's actual metadata shape determines bytes used.
inline NativeAllocationPreparedResult PrepareNativeAllocationRead(usize bytes,
    NativeStorageMemory& memory,const NativeStorageMemoryBinding& binding) noexcept {
 using E=NativeAllocationLeaseError;NativeAllocationPreparedResult out;
 try{
  if(!bytes)return out;
  auto grant=memory.CreateArena(binding,bytes,alignof(std::max_align_t));
  out.memory_error=grant.error;out.allocation_status=grant.backing.status;
  out.allocation_diagnostic=std::move(grant.backing.diagnostic);
  if(!grant.ok()){out.error=E::memory_failure;return out;}
  const auto block=grant.arena.Allocate(bytes,alignof(std::max_align_t));
  if(!block.ok()){out.error=E::resource_exhausted;return out;}
  out.workspace.bytes_={static_cast<byte*>(block.pointer),block.bytes};
  out.workspace.arena_=std::move(grant.arena);out.error=E::none;
 }catch(const std::bad_alloc&){out.error=E::resource_exhausted;}
  catch(const std::length_error&){out.error=E::resource_exhausted;}
  catch(const std::system_error&){out.error=E::lock_failure;}
 return out;
}
struct NativeAllocationLeaseReadResult {
 NativeAllocationLeaseError error=NativeAllocationLeaseError::invalid_request;
 page::NativeAllocationError chain_error=page::NativeAllocationError::none;
 core::platform::Status io_status;
 core::platform::DiagnosticRecord io_diagnostic;
 u64 physical_bytes_read=0,retained_image_bytes=0;
 usize backing_bytes_used=0;
 std::optional<page::NativeFilespaceAllocationRoot> root;
 NativeReadAllocationChain chain;
 bool ok() const noexcept{return error==NativeAllocationLeaseError::none&&root&&chain.ok();}
};
// Resolve only through the privately selected checkpoint/directory. Primary
// maps borrow the source; secondary maps reuse the exact source device/batch.
// Release the source before backing. Reuse invalidates every previous result.
// This is an immutable observation, NOT a page reservation, security admission,
// transaction/operation completion or permission to reuse retired pages.
class NativeAllocationLeaseReader {
 public:
 static NativeAllocationLeaseReadResult Read(const NativeSelectedCheckpointMemoryLease& lease,
     const Uuid& filespace,u64 maximum_total_image_bytes,NativeAllocationPreparedMemory& backing) noexcept {
  using E=NativeAllocationLeaseError;NativeAllocationLeaseReadResult out;
  const auto fail=[&](E e){out.error=e;out.root.reset();out.chain={};out.retained_image_bytes=0;return std::move(out);};
  try{
   lease.CheckThread();
   if(!backing||!core::uuid::IsEngineIdentityUuid(filespace))return out;
   const auto& cp=*lease.selection_.checkpoint_inventory.checkpoint;
   if(backing.arena_.binding().database_uuid!=cp.header.database_uuid)return out;
   if(lease.retained_image_bytes_>maximum_total_image_bytes)return fail(E::resource_exhausted);
   const page::NativeFilespaceDirectoryRecord* member=nullptr;
   for(const auto& p:lease.directory_.directory.pages)for(const auto& r:p.directory.records)
    if(r.bootstrap.filespace_uuid==filespace)member=&r;
   if(!member)return fail(E::member_not_found);
   const auto file=std::find_if(lease.devices_.begin(),lease.devices_.end(),[&](const auto& f){return f.filespace_uuid==filespace;});
   if(file==lease.devices_.end()||file->page_size_profile_uuid!=member->bootstrap.page_size_profile_uuid)return fail(E::binding_mismatch);
   const bool primary=filespace==cp.header.filespace_uuid;
   NativeReadAllocationChain chain;
   std::array<byte,32> digest{};
   if(primary){
    chain=lease.selection_.allocation;
    const auto target=std::find_if(cp.roots.begin(),cp.roots.end(),[](const auto& r){return r.role==4;});
    if(target==cp.roots.end())return fail(E::binding_mismatch);
    digest=target->sha256;
   }else{
    disk::FilespaceRootReference selected;
    const auto* explicit_root=member->allocation_root?&*member->allocation_root:nullptr;
    if(explicit_root){const auto& r=*explicit_root;
     selected={3,3,r.page.filespace_uuid,r.page.page_number,r.page.page_generation,r.page.page_size_profile_uuid,r.object_uuid};}
    detail::NativeMetadataMemory resource(backing.bytes_);
    auto read=page::detail::ReadNativeAllocationChainBacked(*file->device,
      {cp.header.database_uuid,filespace,file->page_size_profile_uuid},maximum_total_image_bytes-lease.retained_image_bytes_,
      explicit_root?page::NativeAllocationChainReadContext::selected:page::NativeAllocationChainReadContext::bootstrap,
      explicit_root?&selected:nullptr,nullptr,{},lease.batches_[file-lease.devices_.begin()],resource);
    out.chain_error=read.chain.error;out.io_status=read.io_status;out.io_diagnostic=std::move(read.io_diagnostic);
    out.physical_bytes_read=read.physical_bytes_read;out.backing_bytes_used=resource.used();
    if(!read.ok())return fail(E::chain_failure);
    const auto hash=core::hash::ComputeSha256DigestNative(read.chain.pages.front().image.data(),read.chain.pages.front().image.size());
    if(!hash.ok())return fail(E::hash_failure);
    digest=hash.digest;
    detail::NativeMetadataScratch scratch(&resource);
    chain=detail::ReadOnly(read.chain,scratch);out.backing_bytes_used=resource.used();
   }
   const auto& m=chain.pages.front().map;
   page::NativeFilespaceAllocationRoot resolved{{m.header.filespace_uuid,m.header.page_number,m.header.page_generation,
     m.header.page_size_profile_uuid},m.object_uuid,digest,m.map_generation,m.capacity_generation};
   if(m.total_pages!=member->total_pages)return fail(E::binding_mismatch);
   if(member->allocation_root){
    if(member->allocation_root->sha256!=digest)return fail(E::integrity_failure);
    if(*member->allocation_root!=resolved)return fail(E::binding_mismatch);
   }
   out.root=resolved;out.chain=chain;
   out.retained_image_bytes=lease.retained_image_bytes_+(primary?0:chain.retained_image_bytes);
   out.error=E::none;return out;
  }catch(const std::bad_alloc&){return fail(E::resource_exhausted);}
   catch(const std::length_error&){return fail(E::resource_exhausted);}
   catch(const std::system_error&){return fail(E::lock_failure);}
 }
};
} // namespace scratchbird::storage::database
