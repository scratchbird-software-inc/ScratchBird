// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_catalog_root_memory.hpp"
#include "native_selected_checkpoint_memory_lease.hpp"
#include "hash_digest.hpp"

namespace scratchbird::storage::database {
enum class NativeCatalogRootsLeaseError {
 none,invalid_request,memory_failure,bootstrap_failure,page_zero_failure,
 binding_mismatch,catalog_failure,integrity_failure,creator_mismatch,
 creator_not_committed,hash_failure,io_failure,resource_exhausted,lock_failure,
 encrypted_requires_authority,cluster_requires_authority,header_policy_requires_authority
};
struct NativeReadCatalogRoot {
 disk::NativeCommonPageHeader header;
 u16 root_kind=0;
 Uuid object_uuid,creator_transaction_uuid;
 u64 creator_local_transaction_id=0,catalog_generation=0,schema_epoch=0,security_epoch=0,resource_epoch=0;
 std::optional<disk::NativePageReference> predecessor;
 std::array<byte,32> predecessor_sha256{};
 std::span<const page::NativeCatalogRootReference> roots;
};
struct NativeReadCatalogRootImage {
 NativeReadCatalogRoot root;
 std::span<const byte> image;
 std::size_t inventory_entry_index=0;
};
inline usize NativeCatalogRootsWorkspaceBytes(const Uuid& maximum_profile) noexcept {
 const auto* p=disk::FindCanonicalFilespacePageProfile(maximum_profile);
 return p?2*p->page_size_bytes+32*sizeof(disk::FilespaceRootReference)+
   12*sizeof(page::NativeCatalogRootReference)+2*sizeof(NativeReadCatalogRootImage)+
   3*(alignof(std::max_align_t)-1):0;
}
struct NativeCatalogRootsPreparedResult;
// Maximum profile is a backing-size hint, not a placement or ownership claim.
// Admit before the source lease, release afterwards; reuse invalidates all views.
class NativeCatalogRootsPreparedMemory {
 public:
  NativeCatalogRootsPreparedMemory() noexcept=default;
  NativeCatalogRootsPreparedMemory(const NativeCatalogRootsPreparedMemory&)=delete;
  NativeCatalogRootsPreparedMemory& operator=(const NativeCatalogRootsPreparedMemory&)=delete;
  NativeCatalogRootsPreparedMemory(NativeCatalogRootsPreparedMemory&&) noexcept=default;
  NativeCatalogRootsPreparedMemory& operator=(NativeCatalogRootsPreparedMemory&&) noexcept=default;
  explicit operator bool() const noexcept{return bool(arena_);}
 private:
  NativeStorageArena arena_;
  std::span<byte> images_;
  usize maximum_page_bytes_=0;
  std::span<disk::FilespaceRootReference> zero_roots_;
  std::span<page::NativeCatalogRootReference> references_;
  std::span<NativeReadCatalogRootImage> decoded_;
  friend class NativeCatalogRootsLeaseReader;
  friend NativeCatalogRootsPreparedResult PrepareNativeCatalogRootsRead(
      const Uuid&,NativeStorageMemory&,const NativeStorageMemoryBinding&) noexcept;
};
struct NativeCatalogRootsPreparedResult {
 NativeCatalogRootsLeaseError error=NativeCatalogRootsLeaseError::invalid_request;
 NativeStorageMemoryError memory_error=NativeStorageMemoryError::none;
 core::platform::Status allocation_status;
 core::platform::DiagnosticRecord allocation_diagnostic;
 NativeCatalogRootsPreparedMemory workspace;
 bool ok() const noexcept{return error==NativeCatalogRootsLeaseError::none&&bool(workspace);}
};
inline NativeCatalogRootsPreparedResult PrepareNativeCatalogRootsRead(
    const Uuid& maximum_profile,NativeStorageMemory& memory,const NativeStorageMemoryBinding& binding) noexcept {
 using E=NativeCatalogRootsLeaseError;NativeCatalogRootsPreparedResult out;
 try {
  const auto* p=disk::FindCanonicalFilespacePageProfile(maximum_profile);if(!p)return out;
  auto granted=memory.CreateArena(binding,NativeCatalogRootsWorkspaceBytes(maximum_profile),p->page_size_bytes);
  out.memory_error=granted.error;out.allocation_status=granted.backing.status;
  out.allocation_diagnostic=std::move(granted.backing.diagnostic);
  if(!granted.ok()){out.error=E::memory_failure;return out;}
  NativeCatalogRootsPreparedMemory prepared;prepared.arena_=std::move(granted.arena);
  const auto array=[&]<class T>(usize n)->std::span<T>{
   static_assert(std::is_trivially_destructible_v<T>);
   const auto block=prepared.arena_.Allocate(n*sizeof(T),alignof(T));
   if(!block.ok())throw std::bad_alloc();
   auto* data=static_cast<T*>(block.pointer);std::uninitialized_value_construct_n(data,n);return {data,n};
  };
  const auto images=prepared.arena_.Allocate(2*p->page_size_bytes,p->page_size_bytes);
  if(!images.ok()){out.error=E::resource_exhausted;return out;}
  prepared.images_={static_cast<byte*>(images.pointer),images.bytes};prepared.maximum_page_bytes_=p->page_size_bytes;
  prepared.zero_roots_=array.template operator()<disk::FilespaceRootReference>(32);
  prepared.references_=array.template operator()<page::NativeCatalogRootReference>(12);
  prepared.decoded_=array.template operator()<NativeReadCatalogRootImage>(2);
  out.workspace=std::move(prepared);out.error=E::none;
 }catch(const std::bad_alloc&){out.error=E::resource_exhausted;}
  catch(const std::length_error&){out.error=E::resource_exhausted;}
  catch(const std::system_error&){out.error=E::lock_failure;}
 return out;
}
struct NativeCatalogRootsLeaseReadResult {
 NativeCatalogRootsLeaseError error=NativeCatalogRootsLeaseError::invalid_request;
 disk::FilespaceBootstrapError bootstrap_error=disk::FilespaceBootstrapError::none;
 disk::FilespacePageZeroError page_zero_error=disk::FilespacePageZeroError::none;
 disk::NativeCommonPageHeaderError header_error=disk::NativeCommonPageHeaderError::none;
 page::NativeCatalogRootError catalog_error=page::NativeCatalogRootError::none;
 core::platform::Status io_status;
 core::platform::DiagnosticRecord io_diagnostic;
 u64 physical_bytes_read=0,retained_image_bytes=0;
 std::span<const NativeReadCatalogRootImage> catalogs;
 usize feature_root_index=0;
 bool ok() const noexcept{return error==NativeCatalogRootsLeaseError::none&&!catalogs.empty();}
};
// Both roots are resolved from the privately admitted checkpoint, never from
// caller-provided PageRefs. This verifies root membership and committed creators,
// not relation/index coverage, row visibility, family/security or publication.
class NativeCatalogRootsLeaseReader {
 public:
 static NativeCatalogRootsLeaseReadResult Read(const NativeSelectedCheckpointMemoryLease& lease,
     u64 maximum_additional_image_bytes,NativeCatalogRootsPreparedMemory& backing) noexcept {
  using E=NativeCatalogRootsLeaseError;NativeCatalogRootsLeaseReadResult out;
  try {
   lease.CheckThread();
   if(!backing||!maximum_additional_image_bytes||!lease.selection_.checkpoint_inventory.checkpoint)return out;
   const auto& pair=lease.selection_.checkpoint_inventory;const auto& cp=*pair.checkpoint;
   if(backing.arena_.binding().database_uuid!=cp.header.database_uuid)return out;
   if(cp.flags&4u){out.error=E::cluster_requires_authority;return out;}
   const auto catalog=std::find_if(cp.roots.begin(),cp.roots.end(),[](const auto& r){return r.role==5;});
   const auto feature=std::find_if(cp.roots.begin(),cp.roots.end(),[](const auto& r){return r.role==9;});
   if(catalog==cp.roots.end()||feature==cp.roots.end()){out.error=E::binding_mismatch;return out;}
   usize count=0,feature_index=0;u64 retained=0;
   for(const auto* target:{&*catalog,&*feature}){
    if(target==&*feature&&target->page==catalog->page&&target->object_uuid==catalog->object_uuid&&
        target->page_type==catalog->page_type&&target->sha256==catalog->sha256)continue;
    const auto& ref=target->page;const auto* p=disk::FindCanonicalFilespacePageProfile(ref.page_size_profile_uuid);
    if(!p||target->page_type!=5||!ref.page_number||!ref.page_generation){out.error=E::binding_mismatch;return out;}
    const auto size=p->page_size_bytes;
    if(size>backing.maximum_page_bytes_||size>maximum_additional_image_bytes-retained){out.error=E::resource_exhausted;return out;}
    const auto file=std::find_if(lease.devices_.begin(),lease.devices_.end(),[&](const auto& f){
      return f.filespace_uuid==ref.filespace_uuid&&f.page_size_profile_uuid==ref.page_size_profile_uuid;});
    if(file==lease.devices_.end()){out.error=E::binding_mismatch;return out;}
    if(ref.page_number>std::numeric_limits<u64>::max()/size||
        !disk::CheckFileDeviceExtent(ref.page_number*size,size).ok()){out.error=E::binding_mismatch;return out;}
    auto& device=*file->device;auto& batch=lease.batches_[file-lease.devices_.begin()];
    auto image=backing.images_.subspan(count*backing.maximum_page_bytes_,size);
    const auto read=[&](u64 at,void* bytes,usize n){
      auto r=batch.ReadAt(at,bytes,n);out.physical_bytes_read+=r.bytes_transferred;
      out.io_status=r.status;out.io_diagnostic=std::move(r.diagnostic);return r.ok()&&r.bytes_transferred==n;};
    const disk::FilespaceBootstrapBinding binding{cp.header.database_uuid,ref.filespace_uuid,ref.page_size_profile_uuid};
    disk::SerializedFilespaceBootstrap prefix{};
    if(!read(0,prefix.data(),prefix.size())){out.error=E::io_failure;return out;}
    const auto bootstrap=disk::DecodeFilespaceBootstrap(prefix.data(),prefix.size(),&binding);
    if(!bootstrap.ok()){out.error=E::bootstrap_failure;out.bootstrap_error=bootstrap.error;return out;}
    auto before=device.Size();out.io_status=before.status;out.io_diagnostic=std::move(before.diagnostic);
    if(!before.ok()){out.error=E::io_failure;return out;}
    if(!read(0,image.data(),size)){out.error=E::io_failure;return out;}
    if(!std::equal(prefix.begin(),prefix.end(),image.begin())){
      out.error=E::page_zero_failure;out.page_zero_error=disk::FilespacePageZeroError::probe_changed;return out;}
    const auto zero=disk::DecodeFilespacePageZeroInto(image,backing.zero_roots_,&binding);
    if(!zero.ok()){out.error=E::page_zero_failure;out.page_zero_error=zero.error;return out;}
    if(zero.record->total_pages>std::numeric_limits<u64>::max()/size||before.size_bytes!=zero.record->total_pages*size||
        ref.page_number>=zero.record->total_pages||zero.record->bootstrap.filespace_role>4){
      out.error=E::page_zero_failure;out.page_zero_error=disk::FilespacePageZeroError::invalid_capacity;return out;}
    if(zero.record->bootstrap.flags&disk::FilespaceBootstrapFlag::payload_encrypted){out.error=E::encrypted_requires_authority;return out;}
    if(zero.record->bootstrap.flags&disk::FilespaceBootstrapFlag::cluster_authority_required){out.error=E::cluster_requires_authority;return out;}
    const auto total_pages=zero.record->total_pages;
    if(!read(ref.page_number*size,image.data(),size)){out.error=E::io_failure;return out;}
    auto after=device.Size();out.io_status=after.status;out.io_diagnostic=std::move(after.diagnostic);
    if(!after.ok()){out.error=E::io_failure;return out;}
    if(before.size_bytes!=after.size_bytes){out.error=E::page_zero_failure;out.page_zero_error=disk::FilespacePageZeroError::invalid_capacity;return out;}
    const disk::NativeCommonPageHeaderBinding expected{binding,ref.page_number,ref.page_generation,5,{}};
    const auto header=disk::DecodeNativeCommonPageHeader(image.data(),128,&expected);
    if(!header.ok()){out.error=E::binding_mismatch;out.header_error=header.error;return out;}
    if(header.header->flags&1u){out.error=E::encrypted_requires_authority;return out;}
    if(header.header->flags&2u){out.error=E::cluster_requires_authority;return out;}
    if(header.header->flags&12u){out.error=E::header_policy_requires_authority;return out;}
    auto decoded=page::DecodeNativeCatalogRootInto(image,backing.references_.subspan(count*6,6));
    if(!decoded.ok()){out.error=E::catalog_failure;out.catalog_error=decoded.error;return out;}
    const auto& r=*decoded.root;
    if(r.object_uuid!=target->object_uuid||(target==&*catalog?r.root_kind!=2:(r.root_kind!=2&&r.root_kind!=8))){out.error=E::binding_mismatch;return out;}
    const auto outside=[&](const auto& child){return child.filespace_uuid==ref.filespace_uuid&&child.page_number>=total_pages;};
    if((r.predecessor&&outside(*r.predecessor))||std::any_of(r.roots.begin(),r.roots.end(),[&](const auto& child){return outside(child.page);})){
      out.error=E::binding_mismatch;return out;}
    const auto digest=core::hash::ComputeSha256DigestNative(image.data(),image.size());
    if(!digest.ok()){out.error=E::hash_failure;return out;}
    if(digest.digest!=target->sha256){out.error=E::integrity_failure;return out;}
    const auto& entries=pair.inventory.entries;
    const auto creator=std::lower_bound(entries.begin(),entries.end(),r.creator_local_transaction_id,
      [](const auto& e,u64 n){return e.identity.local_id.value<n;});
    if(creator==entries.end()||creator->identity.local_id.value!=r.creator_local_transaction_id||
        creator->identity.transaction_uuid.value!=r.creator_transaction_uuid){out.error=E::creator_mismatch;return out;}
    if(creator->identity.scope!=transaction::mga::TransactionScope::local_node){out.error=E::cluster_requires_authority;return out;}
    if(!transaction::mga::HasCommittedInventoryOutcome(*creator)){out.error=E::creator_not_committed;return out;}
    backing.decoded_[count]={{r.header,r.root_kind,r.object_uuid,r.creator_transaction_uuid,
      r.creator_local_transaction_id,r.catalog_generation,r.schema_epoch,r.security_epoch,r.resource_epoch,
      r.predecessor,r.predecessor_sha256,r.roots},image,static_cast<usize>(creator-entries.begin())};
    if(target==&*feature)feature_index=count;
    ++count;retained+=size;
   }
   out.catalogs=backing.decoded_.first(count);out.feature_root_index=feature_index;
   out.retained_image_bytes=retained;out.error=E::none;return out;
  }catch(const std::bad_alloc&){out.error=E::resource_exhausted;}
   catch(const std::length_error&){out.error=E::resource_exhausted;}
   catch(const std::system_error&){out.error=E::lock_failure;}
  return out;
 }
};
} // namespace scratchbird::storage::database
