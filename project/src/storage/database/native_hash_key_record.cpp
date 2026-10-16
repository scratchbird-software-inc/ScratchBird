// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_hash_key_record.hpp"
#include <algorithm>

namespace scratchbird::storage::database {
namespace {
using E = NativeHashKeyRecordError;
using core::crypto::NativeHashKeyEnvelopeV1;
using core::crypto::NativeHashKeyBindingV1;
using core::crypto::NativeHashKeyEnvelopeErrorV1;
constexpr auto kBytes = core::crypto::kNativeHashKeyEnvelopeBytesV1;
bool Extent(std::uint64_t at) noexcept {
    return disk::CheckFileDeviceExtentNative(at, kBytes) == disk::BoundedIoError::none;
}
bool Read(disk::FileDevice::BoundedIoBatch& io, std::uint64_t at,
          NativeHashKeyEnvelopeV1& bytes, NativeHashKeyRecordResult& r) {
    ++r.effects.read_calls;
    r.last_io = io.ReadAt(at, bytes.data(), bytes.size());
    r.effects.read_bytes_observed += r.last_io.bytes_transferred;
    if (!r.last_io.ok()) { r.error = E::io_failure; return false; }
    return true;
}
bool Allocated(disk::FileDevice::BoundedIoBatch& io, std::uint64_t at,
               NativeHashKeyRecordResult& r, std::uint64_t& size) {
    r.last_io = io.Size();
    if (!r.last_io.ok()) { r.error = E::io_failure; return false; }
    size = r.last_io.size_bytes;
    if (size < at + kBytes) { r.error = E::slot_not_allocated; return false; }
    return true;
}
bool SameExtent(disk::FileDevice::BoundedIoBatch& io, std::uint64_t size,
                NativeHashKeyRecordResult& r) {
    r.last_io = io.Size();
    if (!r.last_io.ok()) { r.error = E::io_failure; return false; }
    if (r.last_io.size_bytes != size) { r.error = E::extent_changed; return false; }
    return true;
}
}  // namespace

NativeHashKeyRecordResult StageNativeHashKeyRecordOnOpenDevice(
    disk::FileDevice& device, std::uint64_t at, const NativeHashKeyBindingV1& binding,
    const NativeHashKeyEnvelopeV1& record) {
    NativeHashKeyRecordResult r;
    const NativeHashKeyEnvelopeV1 original = record;
    r.envelope_error = core::crypto::InspectNativeHashKeyEnvelopeV1(binding, {original.data(), original.size()});
    if (r.envelope_error != NativeHashKeyEnvelopeErrorV1::none) return r;
    if (!Extent(at)) { r.error = E::invalid_extent; return r; }
    // Bound the ciphertext to this exact attempt BEFORE acquiring any source
    // fence. No caller-owned bytes are reread as write/recheck input afterward.
    disk::FileDevice::BoundedIoBatch io(device); // Observations publish AFTER guard.
    auto guard = device.AcquireOperationGuard();
    std::uint64_t size = 0;
    if (!Allocated(io, at, r, size)) return r;
    NativeHashKeyEnvelopeV1 previous{};
    if (!Read(io, at, previous, r)) return r;
    if (previous == original) {
        r.effects.exact_existing_record = true;
        r.effects.original_or_write_may_remain = true;
    } else if (!std::all_of(previous.begin(), previous.end(), [](auto b) { return b == 0; })) {
        r.error = E::conflicting_record;
        r.effects.original_or_write_may_remain = true;
        return r;
    } else {
        if (!SameExtent(io, size, r)) return r;
        ++r.effects.write_calls;
        r.last_io = io.WriteAt(at, original.data(), original.size());
        r.effects.original_or_write_may_remain = r.last_io.native_attempted;
        r.effects.write_bytes_observed += r.last_io.bytes_transferred;
        if (!r.last_io.ok()) { r.error = E::io_failure; return r; }
    }
    r.effects.sync_attempted = true;
    r.last_io = io.Sync();
    if (!r.last_io.ok()) { r.error = E::io_failure; return r; }
    r.effects.sync_completed = r.last_io.synchronized;
    if (!r.effects.sync_completed) {
        // A read-only Sync is an explicit successful no-op in FileDevice,
        // not proof that this operation synchronized the existing record.
        r.error = E::synchronization_not_performed;
        return r;
    }
    NativeHashKeyEnvelopeV1 checked{};
    if (!Read(io, at, checked, r)) return r;
    if (checked != original) { r.error = E::readback_mismatch; return r; }
    r.effects.readback_verified = true;
    if (!SameExtent(io, size, r)) return r;
    r.error = E::none;
    return r;
}

NativeHashKeyRecordReadResult ReadNativeHashKeyRecordFromOpenDevice(
    disk::FileDevice& device, std::uint64_t at, const NativeHashKeyBindingV1& binding) {
    NativeHashKeyRecordReadResult r;
    if (!core::crypto::ValidNativeHashKeyBindingV1(binding)) {
        r.envelope_error = NativeHashKeyEnvelopeErrorV1::invalid_binding;
        return r;
    }
    if (!Extent(at)) { r.error = E::invalid_extent; return r; }
    disk::FileDevice::BoundedIoBatch io(device);
    auto guard = device.AcquireOperationGuard();
    std::uint64_t size = 0;
    if (!Allocated(io, at, r, size)) return r;
    NativeHashKeyEnvelopeV1 candidate{};
    if (!Read(io, at, candidate, r)) return r;
    r.envelope_error = core::crypto::InspectNativeHashKeyEnvelopeV1(binding, {candidate.data(), candidate.size()});
    if (r.envelope_error != NativeHashKeyEnvelopeErrorV1::none) return r;
    if (!SameExtent(io, size, r)) return r;
    r.error = E::none;
    r.record = candidate;
    return r;
}
}  // namespace scratchbird::storage::database
