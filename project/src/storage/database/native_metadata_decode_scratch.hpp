// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
// Internal payload backing shared by complete native metadata readers. No
// admission or upstream fallback is supplied by the bounded resource.
#include "native_allocation_map.hpp"
#include "transaction_inventory_page.hpp"
#include "native_filespace_directory.hpp"
#include "native_management_operation.hpp"
#include "database_dirty_manifest.hpp"
#include <memory_resource>
#include <memory>
#include <limits>
#include <algorithm>

namespace scratchbird::storage::database::detail {
using namespace core::platform;
using Image=std::span<const byte>;
// The bounded resource has no upstream allocator. Every payload byte,
// container node, decoded value and sort scratch byte comes from this buffer.
class NativeMetadataMemory final:public std::pmr::memory_resource {
  std::span<byte> bytes_;
  std::size_t used_=0;
  void* do_allocate(std::size_t bytes,std::size_t alignment) override {
    void* start=bytes_.data()?bytes_.data()+used_:nullptr;
    auto remaining=bytes_.size()-used_;
    if(!std::align(alignment,bytes,start,remaining))throw std::bad_alloc();
    const auto offset=static_cast<byte*>(start)-bytes_.data();
    used_=offset+bytes;return start;
  }
  void do_deallocate(void*,std::size_t,std::size_t) override {}
  bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {return this==&other;}
public:
  explicit NativeMetadataMemory(std::span<byte> bytes):bytes_(bytes){}
  std::size_t used() const noexcept{return used_;}
};
struct NativeMetadataScratch {
  std::pmr::memory_resource* resource;
  template<class T> std::span<T> Array(std::size_t count){
    static_assert(std::is_trivially_destructible_v<T>);
    if(!count)return {};
    if(count>std::numeric_limits<std::size_t>::max()/sizeof(T))throw std::bad_alloc();
    auto* out=static_cast<T*>(resource->allocate(count*sizeof(T),alignof(T)));
    std::uninitialized_value_construct_n(out,count);return {out,count};
  }
  auto Map(Image raw){
    const auto available=raw.size()>=384?raw.size()-384:0;
    const auto states=raw.size()>=384?std::min<u64>(LoadLittle64(raw.data()+192),u64{available}*2):0;
    const auto records=raw.size()>=384?std::min<std::size_t>(LoadLittle32(raw.data()+308),available/128):0;
    return page::DecodeNativeAllocationMapInto(raw,Array<page::NativeAllocationState>(states),Array<page::NativeAllocationRecord>(records));
  }
  auto Inventory(Image raw){
    const auto count=raw.size()>=384?std::min<std::size_t>(LoadLittle32(raw.data()+184),(raw.size()-384)/72):0;
    return page::DecodeNativeTransactionInventoryPageInto(raw,Array<transaction::mga::TransactionInventoryEntry>(count),
      Array<std::size_t>(count),Array<byte>(count));
  }
  auto Directory(Image raw){
    const auto count=raw.size()>=384?std::min<std::size_t>(LoadLittle32(raw.data()+208),(raw.size()-384)/192):0;
    return page::DecodeNativeFilespaceDirectoryInto(raw,Array<page::NativeFilespaceDirectoryRecord>(count),Array<Uuid>(count));
  }
  auto PageZero(Image raw){return disk::DecodeFilespacePageZeroInto(raw,Array<disk::FilespaceRootReference>(32));}
  auto Checkpoint(Image raw){return DecodeNativeCheckpointRootInto(raw,Array<NativeCheckpointRootReference>(16));}
  NativeManagementOperationViewWorkspace OperationWorkspace(Image raw){
    const auto count=raw.size()>=512?std::min<std::size_t>(LoadLittle32(raw.data()+456),(raw.size()-512)/256):0;
    return {Array<NativeManagementStepView>(count),Array<Uuid>(2*count+7)};
  }
  template<class T> std::span<T> Copy(std::span<const T> input){
    auto out=Array<T>(input.size());std::copy(input.begin(),input.end(),out.begin());return out;
  }
};
// Keep ordinary owning callers' allocation sites visible to the existing
// process allocation/fault instrumentation. The bounded API never uses this.
class NativeMetadataHeapMemory final:public std::pmr::memory_resource {
  void* do_allocate(std::size_t size,std::size_t alignment) override {
    return alignment<=alignof(std::max_align_t)?::operator new(size):
      ::operator new(size,std::align_val_t{alignment});
  }
  void do_deallocate(void* p,std::size_t,std::size_t alignment) override {
    if(alignment<=alignof(std::max_align_t))::operator delete(p);
    else ::operator delete(p,std::align_val_t{alignment});
  }
  bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {return this==&other;}
};

template<class Read,class Size>
disk::FilespacePageZeroViewResult ReadNativeMetadataPageZero(
    const disk::FilespaceBootstrapBinding& binding,NativeMetadataScratch& scratch,Read read,Size extent,Image& image){
  using P=disk::FilespacePageZeroError;
  const auto fail=[](P error){return disk::FilespacePageZeroViewResult{error,{}};};
  const auto bootstrap_error=[&](disk::FilespaceBootstrapError e){
    return fail(e==disk::FilespaceBootstrapError::hash_provider_failure?P::hash_provider_failure:
      e==disk::FilespaceBootstrapError::resource_exhausted?P::resource_exhausted:P::invalid_bootstrap);};
  auto prefix=scratch.Array<byte>(disk::kFilespaceBootstrapBytes);
  const auto probe=read(0,prefix.data(),prefix.size());if(!probe.ok()||probe.bytes_transferred!=prefix.size())return fail(P::io_failure);
  const auto decoded=disk::DecodeFilespaceBootstrap(prefix.data(),prefix.size(),&binding);
  if(!decoded.ok())return bootstrap_error(decoded.error);
  const auto preamble=disk::EncodeFilespaceBootstrap(*decoded.preamble);
  if(!preamble.ok())return bootstrap_error(preamble.error);
  const auto before=extent();const auto size=decoded.preamble->page_size_bytes;
  if(!before.ok())return fail(P::io_failure);
  if(before.size_bytes<size||before.size_bytes%size)return fail(P::invalid_capacity);
  auto bytes=scratch.Array<byte>(size);const auto io=read(0,bytes.data(),bytes.size());
  if(!io.ok()||io.bytes_transferred!=bytes.size())return fail(P::io_failure);
  if(!std::equal(preamble.bytes->begin(),preamble.bytes->end(),bytes.begin()))return fail(P::probe_changed);
  auto result=disk::DecodeFilespacePageZeroInto(bytes,scratch.Array<disk::FilespaceRootReference>(32),&binding);
  if(!result.ok())return result;
  const auto after=extent();if(!after.ok())return fail(P::io_failure);
  if(before.size_bytes!=after.size_bytes||after.size_bytes!=result.record->total_pages*size)return fail(P::invalid_capacity);
  image=bytes;return result;
}

} // namespace scratchbird::storage::database::detail
