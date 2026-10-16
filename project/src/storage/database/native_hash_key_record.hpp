// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "disk_device.hpp"
#include "native_hash_key_envelope.hpp"

namespace scratchbird::storage::database {

// Physical encrypted-record effects only, NOT an admitted key, current
// generation, authorization, allocation, catalog publication or MGA finality.
// Selected standalone custody-provider file/device/slot ownership and all
// admission/source fences remain with the actual caller. This byte-record
// mechanism is NOT a filespace-page allocator or a native-page-image writer.
enum class NativeHashKeyRecordError {
    none, invalid_record, invalid_extent, io_failure, slot_not_allocated,
    conflicting_record, readback_mismatch, extent_changed, synchronization_not_performed
};
struct NativeHashKeyRecordEffects {
    std::uint64_t read_calls = 0, read_bytes_observed = 0;
    std::uint64_t write_calls = 0, write_bytes_observed = 0;
    bool exact_existing_record = false;
    bool sync_attempted = false, sync_completed = false, readback_verified = false;
    // An existing original or a native-attempted write needs reconciliation on
    // error, even when no transferred bytes were observed. An API-level refusal
    // before any native write does not manufacture physical uncertainty.
    bool original_or_write_may_remain = false;
};
struct NativeHashKeyRecordResult {
    NativeHashKeyRecordError error = NativeHashKeyRecordError::invalid_record;
    core::crypto::NativeHashKeyEnvelopeErrorV1 envelope_error =
        core::crypto::NativeHashKeyEnvelopeErrorV1::none;
    disk::BoundedIoResult last_io;
    NativeHashKeyRecordEffects effects;
    bool ok() const noexcept { return error == NativeHashKeyRecordError::none; }
};
// Immutable original ciphertext retry only. Conflicting nonzero slots refuse
// without overwrite, extension or a fresh identity. A prior partial write is a
// conflicting unresolved record, not an empty slot. Complete sync + readback
// verify this physical record only; the owner separately authenticates it and
// coherently publishes/selects the actual custody/catalog/MGA reference.
NativeHashKeyRecordResult StageNativeHashKeyRecordOnOpenDevice(
    disk::FileDevice&, std::uint64_t offset,
    const core::crypto::NativeHashKeyBindingV1&,
    const core::crypto::NativeHashKeyEnvelopeV1&);

struct NativeHashKeyRecordReadResult : NativeHashKeyRecordResult {
    // Whole ciphertext returned only on success, never a failed read prefix.
    core::crypto::NativeHashKeyEnvelopeV1 record{};
};
// No open/close, write, sync, key reconstruction or permissions are performed.
// Structural success is not authentication; use the admitted protected root
// and exact private plaintext staging to Open the selected original afterward.
NativeHashKeyRecordReadResult ReadNativeHashKeyRecordFromOpenDevice(
    disk::FileDevice&, std::uint64_t offset,
    const core::crypto::NativeHashKeyBindingV1&);

}  // namespace scratchbird::storage::database
