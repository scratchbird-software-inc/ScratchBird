// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_index_btree_page.hpp"
#include "native_storage_memory.hpp"
#include "disk_device.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <limits>
#include <memory>
#include <system_error>
#include <type_traits>

namespace scratchbird::storage::database {
struct NativeBtreeTreeMemoryLimits {
  std::size_t maximum_pages=0,maximum_retained_image_bytes=0;
};
enum class NativeBtreeTreeMemoryError {
  none, invalid_request, memory_binding_failure, memory_allocation_failure,
  tree_failure, resource_exhausted, lock_failure
};
struct NativeBtreeTreeMemoryResult {
  NativeBtreeTreeMemoryError error=NativeBtreeTreeMemoryError::invalid_request;
  NativeStorageMemoryError memory_error=NativeStorageMemoryError::none;
  page::NativeBtreeError tree_error=page::NativeBtreeError::none;
  disk::FilespaceBootstrapError bootstrap_error=disk::FilespaceBootstrapError::none;
  disk::FilespacePageZeroError page_zero_error=disk::FilespacePageZeroError::none;
  core::platform::Status allocation_status,io_status;
  core::platform::DiagnosticRecord allocation_diagnostic,io_diagnostic;
  core::platform::u64 physical_bytes_read=0,retained_image_bytes=0;
  NativeStorageArena arena;
  std::span<const page::NativeBtreePageViewResult> pages;
  std::span<const std::size_t> leaves;
  bool ok() const noexcept {return error==NativeBtreeTreeMemoryError::none&&bool(arena)&&!pages.empty()&&!leaves.empty();}
};
namespace btree_memory_detail {
using core::platform::Uuid;
using core::platform::u64;
using core::platform::byte;
using Lock=std::unique_lock<std::recursive_mutex>;
using Batch=disk::FileDevice::ReadLatencyBatch;
constexpr auto missing=std::numeric_limits<std::size_t>::max();
struct Pending {
  disk::NativePageReference ref;
  std::size_t parent=missing;
  core::platform::u16 level=0;
  std::optional<page::NativeBtreeKeyView> low,high;
};
struct File {
  disk::NativeFilespaceDevice source;
  u64 total_pages=0;
  core::platform::u32 size=0,flags=0,role=0;
};
// Explicit construction counts cover partial lock acquisition. Unlock every
// device before observations publish, then release backing in the caller.
struct Fences {
  Lock* locks=nullptr;Batch* batches=nullptr;
  std::size_t locked=0,observations=0;
  ~Fences(){while(locked)std::destroy_at(locks+--locked);
    while(observations)std::destroy_at(batches+--observations);}
};
inline u64 Hash(const Uuid& id,u64 number=0) noexcept {
  u64 h=14695981039346656037ULL;
  for(auto b:id.bytes){h^=b;h*=1099511628211ULL;}
  for(unsigned i=0;i<8;++i){h^=(number>>(8*i))&255;h*=1099511628211ULL;}
  return h;
}
inline std::size_t LargestProfile(std::span<const disk::NativeFilespaceDevice> files) noexcept {
  std::size_t largest=0;
  for(const auto& f:files){const auto* p=disk::FindCanonicalFilespacePageProfile(f.page_size_profile_uuid);
    if(!p)return 0;largest=std::max(largest,static_cast<std::size_t>(p->page_size_bytes));}
  return largest;
}
} // namespace btree_memory_detail

// Complete physical backing, not a retained-image estimate. A zero result means
// invalid/overflowing limits. Sparse trees still retain the admitted full charge.
inline std::size_t NativeBtreeTreeWorkspaceBytes(std::span<const disk::NativeFilespaceDevice> files,
    NativeBtreeTreeMemoryLimits limits) noexcept {
  using namespace btree_memory_detail;
  const auto largest=LargestProfile(files),n=limits.maximum_pages;
  if(!largest||!n||!limits.maximum_retained_image_bytes||n>(missing-1)/2)return 0;
  std::size_t bytes=0;
  const auto add=[&](std::size_t count,std::size_t width,std::size_t alignment){
    if(count>(missing-(alignment-1))/width)return false;
    const auto amount=count*width+alignment-1;
    if(amount>missing-bytes)return false;bytes+=amount;return true;};
  const auto array=[&]<class T>(std::size_t count){return add(count,sizeof(T),alignof(T));};
  if(!array.template operator()<File>(files.size())||!array.template operator()<Lock>(files.size())||
     !array.template operator()<Batch>(files.size())||!array.template operator()<Pending>(n)||
     !array.template operator()<page::NativeBtreePageViewResult>(n)||
     !array.template operator()<std::size_t>(n)||!array.template operator()<std::size_t>(std::min(n,std::size_t{65536}))||
     !array.template operator()<std::size_t>(2*n+1)||!array.template operator()<std::size_t>(2*n+1)||
     !array.template operator()<page::NativeBtreeCellView>(limits.maximum_retained_image_bytes/149)||
     !array.template operator()<page::NativeBtreeReferenceSlot>(page::NativeBtreePageViewBackingRequirements(largest).reference_slots)||
     !array.template operator()<disk::FilespaceRootReference>(32)||
     !add(largest,1,largest)||!add(limits.maximum_retained_image_bytes,1,largest))return 0;
  return bytes;
}

// NATIVE-BTREE-TREE-MEMORY-001. No outer device fence is permitted: backing is
// admitted before binary-ordered cohort guards. This proves navigation only,
// never selected-root, dependency-map, creator visibility or storage permission.
inline NativeBtreeTreeMemoryResult ReadNativeBtreeTreeWithMemoryFromOpenDevices(
    const core::platform::Uuid& database,std::span<const disk::NativeFilespaceDevice> supplied,
    const disk::NativePageReference& root,const page::NativeBtreeDependencies& dependencies,
    NativeBtreeTreeMemoryLimits limits,NativeStorageMemory& memory,
    const NativeStorageMemoryBinding& binding) noexcept {
  using namespace btree_memory_detail;
  using E=NativeBtreeTreeMemoryError;using T=page::NativeBtreeError;
  NativeBtreeTreeMemoryResult out;
  const auto fail=[&](T e){out.error=e==T::resource_exhausted?E::resource_exhausted:E::tree_failure;out.tree_error=e;};
  try {
    out.memory_error=memory.CheckBinding(binding);
    if(out.memory_error==NativeStorageMemoryError::none&&binding.database_uuid!=database)
      out.memory_error=NativeStorageMemoryError::invalid_binding;
    if(out.memory_error!=NativeStorageMemoryError::none){out.error=E::memory_binding_failure;return out;}
    if(!core::uuid::IsEngineIdentityUuid(database)||!core::uuid::IsEngineIdentityUuid(root.filespace_uuid)||
       !root.page_number||!root.page_generation||!disk::FindCanonicalFilespacePageProfile(root.page_size_profile_uuid)||supplied.empty())return out;
    if(!page::NativeBtreeDependenciesValid(dependencies)){fail(T::invalid_dependencies);return out;}
    for(std::size_t i=0;i<supplied.size();++i){const auto& f=supplied[i];
      if(!core::uuid::IsEngineIdentityUuid(f.filespace_uuid)||!f.device||
          !disk::FindCanonicalFilespacePageProfile(f.page_size_profile_uuid)){fail(T::invalid_filespace);return out;}
      for(std::size_t j=0;j<i;++j)if(supplied[j].device==f.device||supplied[j].filespace_uuid==f.filespace_uuid){
        fail(T::invalid_filespace);return out;}
    }
    const auto bytes=NativeBtreeTreeWorkspaceBytes(supplied,limits),largest=LargestProfile(supplied);
    if(!bytes)return out;
    auto payload=memory.CreateArena(binding,bytes,largest);
    out.memory_error=payload.error;out.allocation_status=payload.backing.status;
    out.allocation_diagnostic=std::move(payload.backing.diagnostic);
    if(!payload.ok()){out.error=E::memory_allocation_failure;return out;}
    bool allocated=true;
    const auto raw=[&]<class A>(std::size_t count)->std::span<A>{
      const auto block=payload.arena.Allocate(count*sizeof(A),alignof(A));
      if(!block.ok()){allocated=false;return {};}
      return {static_cast<A*>(block.pointer),count};};
    const auto array=[&]<class A>(std::size_t count){
      static_assert(std::is_trivially_destructible_v<A>);
      auto s=raw.template operator()<A>(count);std::uninitialized_value_construct_n(s.data(),s.size());return s;};
    auto files=array.template operator()<File>(supplied.size());
    auto locks=raw.template operator()<Lock>(supplied.size());auto batches=raw.template operator()<Batch>(supplied.size());
    auto pending=array.template operator()<Pending>(limits.maximum_pages);
    auto pages=array.template operator()<page::NativeBtreePageViewResult>(limits.maximum_pages);
    auto leaves=array.template operator()<std::size_t>(limits.maximum_pages);
    auto levels=array.template operator()<std::size_t>(std::min(limits.maximum_pages,std::size_t{65536}));
    auto slots=array.template operator()<std::size_t>(2*limits.maximum_pages+1);
    auto identities=array.template operator()<std::size_t>(2*limits.maximum_pages+1);
    auto cells=array.template operator()<page::NativeBtreeCellView>(limits.maximum_retained_image_bytes/149);
    auto references=array.template operator()<page::NativeBtreeReferenceSlot>(page::NativeBtreePageViewBackingRequirements(largest).reference_slots);
    auto roots=array.template operator()<disk::FilespaceRootReference>(32);
    const auto zero_bytes=payload.arena.Allocate(largest,largest);
    const auto image_bytes=payload.arena.Allocate(limits.maximum_retained_image_bytes,largest);
    if(!allocated||!zero_bytes.ok()||!image_bytes.ok()){fail(T::resource_exhausted);return out;}
    for(std::size_t i=0;i<files.size();++i)files[i].source=supplied[i];
    std::sort(files.begin(),files.end(),[](const auto& a,const auto& b){return a.source.filespace_uuid<b.source.filespace_uuid;});
    std::fill(slots.begin(),slots.end(),missing);std::fill(identities.begin(),identities.end(),missing);
    std::fill(levels.begin(),levels.end(),missing);
    Fences fences{locks.data(),batches.data()};
    for(auto& f:files){std::construct_at(batches.data()+fences.observations,*f.source.device);++fences.observations;}
    for(auto& f:files){std::construct_at(locks.data()+fences.locked,f.source.device->AcquireOperationGuard());++fences.locked;}
    const auto read=[&](std::size_t i,u64 offset,void* data,std::size_t size){
      auto io=batches[i].ReadAt(offset,data,size);out.physical_bytes_read+=io.bytes_transferred;
      out.io_status=io.status;out.io_diagnostic=std::move(io.diagnostic);return io.ok()&&io.bytes_transferred==size;};
    for(std::size_t i=0;i<files.size();++i){auto& f=files[i];
      const disk::FilespaceBootstrapBinding expected{database,f.source.filespace_uuid,f.source.page_size_profile_uuid};
      disk::SerializedFilespaceBootstrap prefix{};
      if(!read(i,0,prefix.data(),prefix.size())){fail(T::io_failure);return out;}
      const auto bootstrap=disk::DecodeFilespaceBootstrap(prefix.data(),prefix.size(),&expected);
      if(!bootstrap.ok()){out.bootstrap_error=bootstrap.error;fail(T::invalid_filespace);return out;}
      f.size=bootstrap.preamble->page_size_bytes;
      if(!read(i,0,zero_bytes.pointer,f.size)){fail(T::io_failure);return out;}
      const std::span<const byte> image{static_cast<const byte*>(zero_bytes.pointer),f.size};
      if(!std::equal(prefix.begin(),prefix.end(),image.begin())){
        out.page_zero_error=disk::FilespacePageZeroError::probe_changed;fail(T::invalid_filespace);return out;}
      const auto zero=disk::DecodeFilespacePageZeroInto(image,roots,&expected);
      if(!zero.ok()){out.page_zero_error=zero.error;
        fail(zero.error==disk::FilespacePageZeroError::hash_provider_failure?T::hash_failure:T::invalid_filespace);return out;}
      f.total_pages=zero.record->total_pages;f.role=zero.record->bootstrap.filespace_role;f.flags=zero.record->bootstrap.flags;
      auto size=f.source.device->Size();out.io_status=size.status;out.io_diagnostic=std::move(size.diagnostic);
      if(!size.ok()){fail(T::io_failure);return out;}
      if(size.size_bytes!=f.total_pages*f.size){out.page_zero_error=disk::FilespacePageZeroError::invalid_capacity;fail(T::invalid_filespace);return out;}
    }
    const auto filespace=[&](const disk::NativePageReference& ref){
      const auto f=std::lower_bound(files.begin(),files.end(),ref.filespace_uuid,[](const auto& a,const auto& id){return a.source.filespace_uuid<id;});
      return f==files.end()||f->source.filespace_uuid!=ref.filespace_uuid||f->source.page_size_profile_uuid!=ref.page_size_profile_uuid||ref.page_number>=f->total_pages?
        files.size():static_cast<std::size_t>(f-files.begin());};
    std::size_t pending_count=1,count=0,leaf_count=0,image_used=0,cells_used=0;pending[0]={root};
    while(pending_count){const auto next=pending[--pending_count];const auto fs=filespace(next.ref);
      if(fs==files.size()){fail(T::invalid_filespace);return out;}const auto& f=files[fs];
      if(f.role!=5&&f.role!=6){fail(T::invalid_filespace);return out;}
      if(f.flags&disk::FilespaceBootstrapFlag::payload_encrypted){fail(T::encrypted_requires_crypto_authority);return out;}
      if(f.flags&disk::FilespaceBootstrapFlag::cluster_authority_required){fail(T::cluster_requires_authority);return out;}
      auto slot=Hash(next.ref.filespace_uuid,next.ref.page_number)%slots.size();
      while(slots[slot]!=missing){const auto& h=pages[slots[slot]].page->header;
        if(h.filespace_uuid==next.ref.filespace_uuid&&h.page_number==next.ref.page_number){fail(T::tree_reference_mismatch);return out;}
        if(++slot==slots.size())slot=0;}
      if(count==pages.size()||f.size>limits.maximum_retained_image_bytes-image_used){fail(T::resource_exhausted);return out;}
      auto* image=static_cast<byte*>(image_bytes.pointer)+image_used;
      if(!read(fs,next.ref.page_number*f.size,image,f.size)){fail(T::io_failure);return out;}
      const core::platform::u32 type=next.parent==missing?0x200:next.level?0x201:0x202;
      const disk::NativeCommonPageHeaderBinding expected{{database,next.ref.filespace_uuid,next.ref.page_size_profile_uuid},next.ref.page_number,next.ref.page_generation,type,{}};
      const auto common=disk::DecodeNativeCommonPageHeader(image,128,&expected);
      if(!common.ok()){fail(common.error==disk::NativeCommonPageHeaderError::binding_mismatch?T::binding_mismatch:T::invalid_header);return out;}
      if(common.header->flags&1u){fail(T::encrypted_requires_crypto_authority);return out;}
      if(common.header->flags&2u){fail(T::cluster_requires_authority);return out;}
      if(common.header->flags&12u){fail(T::header_policy_requires_authority);return out;}
      const auto required=page::NativeBtreePageViewBackingRequirements(f.size);
      if(required.cells>cells.size()-cells_used){fail(T::resource_exhausted);return out;}
      auto decoded=page::DecodeNativeBtreePageInto({image,f.size},{cells.subspan(cells_used,required.cells),references});
      if(!decoded.ok()){fail(decoded.error);return out;}
      const auto& p=*decoded.page;
      if(p.dependencies!=dependencies){fail(T::binding_mismatch);return out;}
      const auto in_range=[&](const auto& r){return !r||r->filespace_uuid!=next.ref.filespace_uuid||r->page_number<f.total_pages;};
      if(!in_range(p.parent)||!in_range(p.left)||!in_range(p.right)||!in_range(p.first_child)){
        fail(T::invalid_reference);return out;}
      for(const auto& c:p.cells)if(!in_range(c.child)||!in_range(c.base_page)){fail(T::invalid_reference);return out;}
      auto identity=Hash(p.header.page_uuid)%identities.size();
      while(identities[identity]!=missing){if(pages[identities[identity]].page->header.page_uuid==p.header.page_uuid){fail(T::tree_reference_mismatch);return out;}
        if(++identity==identities.size())identity=0;}
      if(p.tree_level>=levels.size()){fail(T::resource_exhausted);return out;}
      const auto previous=levels[p.tree_level];
      const auto topology=page::detail::ValidateNativeBtreeTraversalStep(p,next.parent==missing?nullptr:&*pages[next.parent].page,
        next.level,next.low,next.high,previous==missing?nullptr:&*pages[previous].page);
      if(topology!=T::none){fail(topology);return out;}
      for(const auto& cell:p.cells)if(cell.base_page&&filespace(*cell.base_page)==files.size()){fail(T::invalid_filespace);return out;}
      slots[slot]=identities[identity]=levels[p.tree_level]=count;
      pages[count]=decoded;image_used+=f.size;cells_used+=required.cells;
      const auto index=count++;
      if(!p.tree_level){leaves[leaf_count++]=index;continue;}
      if(p.cells.size()+1>pending.size()-pending_count){fail(T::resource_exhausted);return out;}
      const auto child_level=static_cast<core::platform::u16>(p.tree_level-1);
      for(std::size_t i=p.cells.size();i>0;--i){const auto& c=p.cells[i-1];
        pending[pending_count++]={*c.child,index,child_level,c.key,i==p.cells.size()?p.high_fence:std::optional{p.cells[i].key}};}
      pending[pending_count++]={*p.first_child,index,child_level,p.low_fence,p.cells.front().key};
    }
    for(const auto index:levels)if(index!=missing&&pages[index].page->right){fail(T::tree_sibling_mismatch);return out;}
    out.pages=pages.first(count);out.leaves=leaves.first(leaf_count);out.retained_image_bytes=image_used;
    out.arena=std::move(payload.arena);out.error=E::none;return out;
  }catch(const std::bad_alloc&){out.error=E::resource_exhausted;}
   catch(const std::length_error&){out.error=E::resource_exhausted;}
   catch(const std::system_error&){out.error=E::lock_failure;}
  return out;
}
} // namespace scratchbird::storage::database
