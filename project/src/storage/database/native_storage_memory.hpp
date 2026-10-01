// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "filespace_bootstrap.hpp"
#include "reservation_backed_memory_resource.hpp"
#include <cstring>
#include <utility>

namespace scratchbird::storage::database {
// A retained actual shared grant, not storage permission or an image allowance.
// The issuing node's manager/ledger must outlive all workspaces and buffers.
// Payload ownership may outlive its workspace; no public release operation can
// free a buffer still owned by a reader. Revocation prevents new allocations,
// but does not invalidate already owned bytes or erase their accounting.
enum class NativeStorageMemoryError {
  none, invalid_binding, invalid_grant, invalid_profile, resource_exhausted
};
struct NativeStorageMemoryBinding {
  core::platform::Uuid database_uuid, operation_uuid, owner_uuid, context_uuid;
};
class NativeStorageMemory;
struct NativeStorageBufferResult;
class NativeStorageBuffer {
 public:
  NativeStorageBuffer() noexcept = default;
  NativeStorageBuffer(const NativeStorageBuffer&)=delete;
  NativeStorageBuffer& operator=(const NativeStorageBuffer&)=delete;
  NativeStorageBuffer(NativeStorageBuffer&& other) noexcept { Swap(other); }
  NativeStorageBuffer& operator=(NativeStorageBuffer&& other) noexcept {
    if(this!=&other){NativeStorageBuffer old(std::move(other));Swap(old);}return *this;
  }
  ~NativeStorageBuffer(){
    // If a shared provider refuses cleanup it retains its allocation record and
    // charge; final owner destruction retries through that same provider.
    Reset();
  }
  core::platform::Status Reset() {
    if(resource_&&data_){
      const auto status=resource_->DeallocateNoAlloc(data_,size_,alignment_);
      if(!status.ok())return status; // Keep the retryable owner and exact bytes.
    }
    data_=nullptr;size_=alignment_=0;resource_.reset();return {};
  }
  core::platform::byte* data() noexcept {return data_;}
  const core::platform::byte* data() const noexcept {return data_;}
  core::platform::usize size() const noexcept {return size_;}
  explicit operator bool() const noexcept {return data_!=nullptr;}
 private:
  void Swap(NativeStorageBuffer& other) noexcept {
    resource_.swap(other.resource_);std::swap(data_,other.data_);
    std::swap(size_,other.size_);std::swap(alignment_,other.alignment_);
  }
  std::shared_ptr<core::memory::ReservationBackedMemoryResource> resource_;
  core::platform::byte* data_=nullptr;
  core::platform::usize size_=0,alignment_=0;
  friend class NativeStorageMemory;
};
struct NativeStorageBufferResult {
  NativeStorageMemoryError error=NativeStorageMemoryError::invalid_grant;
  core::platform::Status allocation_status{core::platform::StatusCode::memory_invalid_request,
    core::platform::Severity::error,core::platform::Subsystem::memory};
  core::platform::DiagnosticRecord diagnostic;
  NativeStorageBuffer buffer;
  bool ok() const noexcept {return error==NativeStorageMemoryError::none&&bool(buffer);}
};
struct NativeStorageMemoryResult;
class NativeStorageMemory {
 public:
  NativeStorageMemory() noexcept = default;
  NativeStorageMemory(const NativeStorageMemory&)=delete;
  NativeStorageMemory& operator=(const NativeStorageMemory&)=delete;
  NativeStorageMemory(NativeStorageMemory&&) noexcept=default;
  NativeStorageMemory& operator=(NativeStorageMemory&&) noexcept=default;
  NativeStorageMemoryError CheckBinding(const core::platform::Uuid& database,const core::platform::Uuid& operation) const {
    if(!resource_)return NativeStorageMemoryError::invalid_grant;
    if(binding_.database_uuid!=database||binding_.operation_uuid!=operation)return NativeStorageMemoryError::invalid_binding;
    return resource_->active()?NativeStorageMemoryError::none:NativeStorageMemoryError::invalid_grant;
  }
  bool Matches(const core::platform::Uuid& database,const core::platform::Uuid& operation) const {
    return CheckBinding(database,operation)==NativeStorageMemoryError::none;
  }
  const NativeStorageMemoryBinding& binding() const noexcept {return binding_;}
  core::memory::ReservationBackedMemoryResourceSnapshot Snapshot() const {
    return resource_?resource_->Snapshot():core::memory::ReservationBackedMemoryResourceSnapshot{};
  }
  NativeStorageBufferResult AllocatePage(const core::platform::Uuid& profile) noexcept {
    NativeStorageBufferResult out;
    try {
      const auto* p=disk::FindCanonicalFilespacePageProfile(profile);
      if(!p){out.error=NativeStorageMemoryError::invalid_profile;return out;}
      if(!resource_||!resource_->active())return out;
      auto allocated=resource_->Allocate({p->page_size_bytes,p->page_size_bytes,"native storage page buffer"});
      out.allocation_status=allocated.status;out.diagnostic=std::move(allocated.diagnostic);
      if(!allocated.ok()){out.error=NativeStorageMemoryError::resource_exhausted;return out;}
      // No throwing operation may follow physical allocation before ownership.
      out.buffer.resource_=resource_;out.buffer.data_=static_cast<core::platform::byte*>(allocated.pointer);
      out.buffer.size_=allocated.bytes;out.buffer.alignment_=allocated.alignment;
      std::memset(out.buffer.data_,0,out.buffer.size_);
      out.error=NativeStorageMemoryError::none;return out;
    }catch(const std::bad_alloc&){out.error=NativeStorageMemoryError::resource_exhausted;
      out.allocation_status={core::platform::StatusCode::memory_allocation_failed,
        core::platform::Severity::error,core::platform::Subsystem::memory};return out;}
  }
 private:
  NativeStorageMemoryBinding binding_;
  std::shared_ptr<core::memory::ReservationBackedMemoryResource> resource_;
  friend NativeStorageMemoryResult AdoptNativeStorageMemory(
    const NativeStorageMemoryBinding&,std::unique_ptr<core::memory::ReservationBackedMemoryResource>&) noexcept;
};
struct NativeStorageMemoryResult {
  NativeStorageMemoryError error=NativeStorageMemoryError::invalid_grant;
  NativeStorageMemory memory;
  bool ok() const noexcept {return error==NativeStorageMemoryError::none;}
};
// Consume exclusive ownership of an already admitted shared grant on success.
// Failure leaves the caller's original grant/payloads intact; never create a governor or
// trusts observations/permission booleans supplied by the scheduling runtime.
inline NativeStorageMemoryResult AdoptNativeStorageMemory(
    const NativeStorageMemoryBinding& binding,
    std::unique_ptr<core::memory::ReservationBackedMemoryResource>& grant) noexcept {
  namespace m=core::memory;
  NativeStorageMemoryResult out;
  for(const auto& id:{binding.database_uuid,binding.operation_uuid,binding.owner_uuid,binding.context_uuid})
    if(!m::MemorySystemUuidValid(id.bytes)){out.error=NativeStorageMemoryError::invalid_binding;return out;}
  try {
    if(!grant)return out;
    const auto& r=grant->request();
    if(!grant->active()||grant->Snapshot().allocated_bytes||!r.operation_id.empty()||
       r.binary_operation_uuid!=binding.operation_uuid.bytes||
       r.binary_ownership[m::MemoryBinaryScopeKind::database]!=binding.database_uuid.bytes||
       r.binary_ownership[m::MemoryBinaryScopeKind::owner]!=binding.owner_uuid.bytes||
       r.binary_ownership[m::MemoryBinaryScopeKind::context]!=binding.context_uuid.bytes||
       r.category!=m::MemoryCategory::page_buffer||
       r.consumer_kind!=m::ReservationBackedMemoryConsumerKind::background_maintenance)return out;
    out.memory.binding_=binding;out.memory.resource_=std::move(grant);
    out.error=NativeStorageMemoryError::none;return out;
  }catch(const std::bad_alloc&){out.error=NativeStorageMemoryError::resource_exhausted;return out;}
}
} // namespace scratchbird::storage::database
