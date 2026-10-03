// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "filespace_page_zero.hpp"
#include <memory_resource>
#include <memory>
#include <limits>
#include <algorithm>
#include <type_traits>

// Shared storage-only backing; no database codec or execution dependency.
namespace scratchbird::storage::disk::detail {
using namespace core::platform;
using Image=std::span<const byte>;
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
  explicit NativeMetadataScratch(std::pmr::memory_resource* r):resource(r){}
  template<class T> std::span<T> Array(std::size_t count){
    static_assert(std::is_trivially_destructible_v<T>);
    if(!count)return {};
    if(count>std::numeric_limits<std::size_t>::max()/sizeof(T))throw std::bad_alloc();
    auto* out=static_cast<T*>(resource->allocate(count*sizeof(T),alignof(T)));
    std::uninitialized_value_construct_n(out,count);return {out,count};
  }
  template<class T> std::span<T> Copy(std::span<const T> input){
    auto out=Array<T>(input.size());std::copy(input.begin(),input.end(),out.begin());return out;
  }
};
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


template<class Scratch,class Read,class Size>
disk::FilespacePageZeroViewResult ReadNativeMetadataPageZero(
    const disk::FilespaceBootstrapBinding& binding,Scratch& scratch,Read read,Size extent,Image& image){
  using P=disk::FilespacePageZeroError;
  const auto fail=[](P error){return disk::FilespacePageZeroViewResult{error,{}};};
  const auto bootstrap_error=[&](disk::FilespaceBootstrapError e){
    return fail(e==disk::FilespaceBootstrapError::hash_provider_failure?P::hash_provider_failure:
      e==disk::FilespaceBootstrapError::resource_exhausted?P::resource_exhausted:P::invalid_bootstrap);};
  auto prefix=scratch.template Array<byte>(disk::kFilespaceBootstrapBytes);
  const auto probe=read(0,prefix.data(),prefix.size());if(!probe.ok()||probe.bytes_transferred!=prefix.size())return fail(P::io_failure);
  const auto decoded=disk::DecodeFilespaceBootstrap(prefix.data(),prefix.size(),&binding);
  if(!decoded.ok())return bootstrap_error(decoded.error);
  const auto preamble=disk::EncodeFilespaceBootstrap(*decoded.preamble);
  if(!preamble.ok())return bootstrap_error(preamble.error);
  const auto before=extent();const auto size=decoded.preamble->page_size_bytes;
  if(!before.ok())return fail(P::io_failure);
  if(before.size_bytes<size||before.size_bytes%size)return fail(P::invalid_capacity);
  auto bytes=scratch.template Array<byte>(size);const auto io=read(0,bytes.data(),bytes.size());
  if(!io.ok()||io.bytes_transferred!=bytes.size())return fail(P::io_failure);
  if(!std::equal(preamble.bytes->begin(),preamble.bytes->end(),bytes.begin()))return fail(P::probe_changed);
  auto result=disk::DecodeFilespacePageZeroInto(bytes,scratch.template Array<disk::FilespaceRootReference>(32),&binding);
  if(!result.ok())return result;
  const auto after=extent();if(!after.ok())return fail(P::io_failure);
  if(before.size_bytes!=after.size_bytes||after.size_bytes!=result.record->total_pages*size)return fail(P::invalid_capacity);
  image=bytes;return result;
}


} // namespace scratchbird::storage::disk::detail
