// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "datatype_blob_owned_persistence.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <cstring>
#include <new>

namespace scratchbird::core::datatypes {
namespace {
using Failure = BlobOwnedPersistenceFailureV3;
using Resource = memory::ReservationBackedMemoryResource;
void Scrub(byte* data, std::size_t size) noexcept {
  volatile byte* p = data;
  for (std::size_t i = 0; i < size; ++i) p[i] = 0;
}
void ReleaseOwned(Resource& grant, byte* data, std::size_t size, std::size_t alignment) noexcept {
  Scrub(data, size);
  // These exact fields are private custody established by this allocator.
  // An impossible cleanup failure must not masquerade as restored accounting
  // or successful publication. No diagnostic allocation or fabricated retry.
  try { if (!grant.DeallocateNoAlloc(data, size, alignment).ok()) std::terminate(); }
  catch (...) { std::terminate(); }
}
bool Range(std::span<const byte> source) noexcept {
  return source.empty() || (source.data() && source.size() <=
      std::numeric_limits<std::uintptr_t>::max() - reinterpret_cast<std::uintptr_t>(source.data()));
}
bool OverlapsOutput(std::span<const byte> source, const BlobOwnedPersistenceBytesV3& output) noexcept {
  if (source.empty()) return false;
  const auto start = reinterpret_cast<std::uintptr_t>(source.data());
  const auto destination = reinterpret_cast<std::uintptr_t>(&output);
  return destination > std::numeric_limits<std::uintptr_t>::max()-sizeof(output) ||
      (start < destination+sizeof(output) && destination < start+source.size());
}
bool Control(const BlobOwnedPersistenceControlV3& control) noexcept {
  return control.chunk_bytes != 0 && control.chunk_bytes <= 1048576;
}
bool Grant(Resource* grant) {
  if (!grant || !grant->active() || grant->Snapshot().allocated_bytes != 0) return false;
  const auto& r = grant->request();
  if (r.category != memory::MemoryCategory::datatype_payload ||
      !r.owner_id.empty() || !r.operation_id.empty() ||
      !uuid::IsEngineIdentityUuid(platform::Uuid{r.binary_operation_uuid})) return false;
  for (const auto kind : {memory::MemoryBinaryScopeKind::context, memory::MemoryBinaryScopeKind::owner})
    if (!uuid::IsEngineIdentityUuid(platform::Uuid{r.binary_ownership[kind]})) return false;
  for (const auto& scope : r.scope_chain)
    if (!scope.scope_id.empty() || !uuid::IsEngineIdentityUuid(platform::Uuid{scope.binary_scope_uuid}))
      return false;
  return true;
}

// Custody exists before any fail-capable construction/publication. An exact
// private allocation is scrubbed/deallocated under its still-owned grant.
struct Allocation {
  Resource& grant;
  byte* data = nullptr;
  std::size_t bytes = 0, alignment = 0;
  ~Allocation() { Reset(); }
  void Reset() noexcept {
    if (data) { ReleaseOwned(grant, data, bytes, alignment); data = nullptr; }
  }
  bool Acquire(std::size_t count, std::size_t align, BlobOwnedPersistenceResultV3& result) {
    auto allocated = grant.Allocate({count, align, "blob.persistence"});
    if (!allocated.ok()) {
      result.failure = Failure::allocation_failed;
      result.memory_diagnostic = std::move(allocated.diagnostic);
      return false;
    }
    data = static_cast<byte*>(allocated.pointer); bytes = count; alignment = align;
    return true;
  }
};
struct Workspace {
  BlobComponentDigestPassV3 pass;
  BlobComponentHeaderV3 header{};
  // The digest helper's64 bytes of local finalization staging are charged even
  // though their actual lifetime is on that helper's synchronous call stack.
  std::array<byte,64> finalization_accounting{};
};
struct Work {
  Allocation allocation;
  Workspace* workspace = nullptr;
  explicit Work(Resource& grant) : allocation{grant} {}
  ~Work() { Reset(); }
  void Reset() noexcept {
    if (workspace) { std::destroy_at(workspace); workspace = nullptr; }
    allocation.Reset();
  }
  bool Acquire(BlobOwnedPersistenceResultV3& result) {
    if (!allocation.Acquire(sizeof(Workspace), alignof(Workspace), result)) return false;
    workspace = std::construct_at(reinterpret_cast<Workspace*>(allocation.data));
    return true;
  }
};
bool Pass(BlobOwnedPersistenceResultV3& result, BlobComponentPassResultV3 pass) noexcept {
  if (pass.ok()) return true;
  result.failure = Failure::component_failed; result.component = pass; return false;
}
bool Cancel(BlobOwnedPersistenceResultV3& result, const BlobOwnedPersistenceControlV3& control) noexcept {
  if (!control.cancellation.stop_requested()) return false;
  result.failure = Failure::cancelled; return true;
}
bool Copy(std::span<const byte> source, byte* destination,
    const BlobOwnedPersistenceControlV3& control, BlobOwnedPersistenceResultV3& result) noexcept {
  for (std::size_t at = 0; at < source.size();) {
    if (Cancel(result, control)) return false;
    const auto count = std::min(control.chunk_bytes, source.size()-at);
    std::memcpy(destination+at, source.data()+at, count);
    result.copied_bytes += count; at += count;
  }
  return !Cancel(result, control);
}
} // namespace

BlobOwnedPersistenceBytesV3::~BlobOwnedPersistenceBytesV3() noexcept {
  if (allocation_) ReleaseOwned(*resource_, allocation_, allocated_, alignof(std::max_align_t));
}

class BlobOwnedPersistenceCodecV3 final {
  static bool Payload(Allocation& allocation, std::size_t size,
      BlobOwnedPersistenceResultV3& result, std::size_t extra = 0) {
    constexpr auto bookkeeping = sizeof(BlobOwnedPersistenceBytesV3);
    if (extra > std::numeric_limits<std::size_t>::max()-bookkeeping ||
        size > std::numeric_limits<std::size_t>::max()-bookkeeping-extra) {
      result.failure = Failure::invalid_argument; return false;
    }
    return allocation.Acquire(bookkeeping+extra+size, alignof(std::max_align_t), result);
  }
  static void Publish(Allocation& allocation, std::size_t size,
      std::unique_ptr<Resource>& grant, BlobOwnedPersistenceBytesV3& output,
      std::size_t extra = 0) noexcept {
    output.allocation_ = allocation.data; output.allocated_ = allocation.bytes;
    output.data_ = allocation.data+sizeof(BlobOwnedPersistenceBytesV3)+extra; output.size_ = size;
    output.resource_ = std::move(grant); allocation.data = nullptr;
  }
 public:
  static void CopyBytes(std::unique_ptr<Resource>& grant,
      std::span<const byte> source, BlobOwnedPersistenceBytesV3& output,
      const BlobOwnedPersistenceControlV3& control, BlobOwnedPersistenceResultV3& result) {
    if (output.bound()) { result.failure = Failure::output_in_use; return; }
    if (!Control(control) || !Range(source) || OverlapsOutput(source, output)) {
      result.failure = Failure::invalid_argument; return;
    }
    if (Cancel(result, control)) return;
    if (!Grant(grant.get())) { result.failure = Failure::invalid_grant; return; }
    Allocation staging{*grant};
    if (!Payload(staging, source.size(), result)) return;
    if (!Copy(source, staging.data+sizeof(output), control, result)) return;
    Publish(staging, source.size(), grant, output); return;
  }
  static void Encode(const BlobValidatedProfileHandleV3& profile,
      BlobValueStateV3 state, const BlobOwnedPersistenceBytesV3& source,
      std::unique_ptr<Resource>& grant, BlobOwnedPersistenceBytesV3& output,
      const BlobOwnedPersistenceControlV3& control, BlobOwnedPersistenceResultV3& result) {
    if (output.bound() || &source == &output) { result.failure = Failure::output_in_use; return; }
    if (!source.bound() || !Control(control)) { result.failure = Failure::invalid_argument; return; }
    if (!ValidateBlobProfileHandleV3(profile).ok()) {
      (void)Pass(result, {BlobComponentPassFailureV3::profile_invalid}); return;
    }
    if ((state != BlobValueStateV3::value && state != BlobValueStateV3::sql_null) ||
        source.size_ > kBlobMaximumLogicalBytesV3 ||
        (state == BlobValueStateV3::sql_null && source.size_ != 0) ||
        source.size_ > std::numeric_limits<std::size_t>::max()-320) {
      (void)Pass(result, {BlobComponentPassFailureV3::invalid_encode_state}); return;
    }
    if (Cancel(result, control)) return;
    if (!Grant(grant.get())) { result.failure = Failure::invalid_grant; return; }
    Work work{*grant}; if (!work.Acquire(result)) return;
    auto& w = *work.workspace;
    if (!Pass(result, w.pass.BeginEncode(profile, state, source.size_))) return;
    for (std::size_t at = 0; at < source.size_;) {
      if (Cancel(result, control)) return;
      const auto count = std::min(control.chunk_bytes, source.size_-at);
      if (!Pass(result, w.pass.UpdateContent(source.bytes().subspan(at, count)))) return;
      result.digest_bytes += count; at += count;
    }
    if (Cancel(result, control) || !Pass(result, w.pass.FinishEncode(w.header))) return;
    Allocation staging{*grant}; const auto size = 320+source.size_;
    if (!Payload(staging, size, result)) return;
    auto* destination = staging.data+sizeof(output);
    std::memcpy(destination, w.header.data(), w.header.size());
    result.header_bytes_written += w.header.size();
    // Same sealed immutable owner, not an external replay/snapshot assertion.
    if (!Copy(source.bytes(), destination+320, control, result)) return;
    work.Reset();
    if (Cancel(result, control)) return;
    Publish(staging, size, grant, output); return;
  }
  static void Decode(const BlobValidatedProfileHandleV3& profile,
      const BlobOwnedPersistenceBytesV3& source, bool null_allowed,
      std::unique_ptr<Resource>& grant, BlobOwnedPersistenceDecodedV3& output,
      const BlobOwnedPersistenceControlV3& control, BlobOwnedPersistenceResultV3& result) {
    if (output.content_.bound() || &source == &output.content_) {
      result.failure = Failure::output_in_use; return;
    }
    if (!source.bound() || !Control(control)) { result.failure = Failure::invalid_argument; return; }
    if (Cancel(result, control)) return;
    const auto header = source.bytes().first(std::min<std::size_t>(320, source.size_));
    if (!Pass(result, ValidateBlobComponentHeaderNoAllocV3(profile, source.size_, header, null_allowed)))
      return;
    if (!Grant(grant.get())) { result.failure = Failure::invalid_grant; return; }
    Work work{*grant}; if (!work.Acquire(result)) return;
    auto& w = *work.workspace;
    if (!Pass(result, w.pass.BeginDecode(profile, source.size_, header, null_allowed))) return;
    const auto size = source.size_-320;
    Allocation staging{*grant};
    if (!Payload(staging, size, result, sizeof(BlobOwnedPersistenceDecodedV3)-sizeof(output.content_)))
      return;
    auto* destination = staging.data+sizeof(output);
    for (std::size_t at = 0; at < size;) {
      if (Cancel(result, control)) return;
      const auto count = std::min(control.chunk_bytes, size-at);
      const auto chunk = source.bytes().subspan(320+at, count);
      if (!Pass(result, w.pass.UpdateContent(chunk))) return;
      result.digest_bytes += count;
      std::memcpy(destination+at, chunk.data(), count);
      result.copied_bytes += count; at += count;
    }
    if (Cancel(result, control) || !Pass(result, w.pass.FinishDecode()) || Cancel(result, control)) return;
    work.Reset();
    if (Cancel(result, control)) return;
    Publish(staging, size, grant, output.content_, sizeof(output)-sizeof(output.content_));
    output.state_ = static_cast<BlobValueStateV3>(header[24]); return;
  }
};

// No exception can publish a partially constructed output or consume the grant.
// Actual allocator vectors survive as moved data; unrelated exceptions cannot
// masquerade as an encoded-field/integrity failure.
template<class Function>
BlobOwnedPersistenceResultV3 Guard(Function&& function) noexcept {
  BlobOwnedPersistenceResultV3 result;
  try { function(result); }
  catch (const std::bad_alloc&) { result.failure = Failure::allocation_failed; }
  catch (...) { result.failure = Failure::internal_failure; }
  return result;
}
BlobOwnedPersistenceResultV3 CopyBlobPersistenceBytesV3(std::unique_ptr<Resource>& grant,
    std::span<const byte> source, BlobOwnedPersistenceBytesV3& output,
    const BlobOwnedPersistenceControlV3& control) noexcept {
  return Guard([&](auto& result) { BlobOwnedPersistenceCodecV3::CopyBytes(grant, source, output, control, result); });
}
BlobOwnedPersistenceResultV3 EncodeOwnedBlobComponentV3(const BlobValidatedProfileHandleV3& profile,
    BlobValueStateV3 state, const BlobOwnedPersistenceBytesV3& source, std::unique_ptr<Resource>& grant,
    BlobOwnedPersistenceBytesV3& output, const BlobOwnedPersistenceControlV3& control) noexcept {
  return Guard([&](auto& result) { BlobOwnedPersistenceCodecV3::Encode(profile, state, source, grant, output, control, result); });
}
BlobOwnedPersistenceResultV3 DecodeOwnedBlobComponentV3(const BlobValidatedProfileHandleV3& profile,
    const BlobOwnedPersistenceBytesV3& source, bool null_allowed, std::unique_ptr<Resource>& grant,
    BlobOwnedPersistenceDecodedV3& output, const BlobOwnedPersistenceControlV3& control) noexcept {
  return Guard([&](auto& result) { BlobOwnedPersistenceCodecV3::Decode(profile, source, null_allowed, grant, output, control, result); });
}
} // namespace scratchbird::core::datatypes
