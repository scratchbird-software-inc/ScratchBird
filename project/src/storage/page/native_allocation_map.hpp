// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "native_common_page_header.hpp"
#include "filespace_page_zero.hpp"
#include <array>
#include <optional>
#include <vector>

namespace scratchbird::storage::page {
using scratchbird::core::platform::Uuid;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

// NATIVE-ALLOCATION-BITMAP-IMAGE-001. These are native wire states, not an
// implicit conversion from a prototype allocation ledger or a reuse grant.
enum class NativeAllocationState : byte {
  free = 0, reserved = 1, allocated = 2, reusable_pending_mga = 3,
  reusable_free = 4, compacting = 5, quarantined = 6, preallocated = 7
};
struct NativeAllocationRecord {
  u64 page_number = 0;
  Uuid allocation_uuid;
  Uuid page_uuid;
  Uuid owner_uuid;
  Uuid creator_transaction_uuid;
  u64 creator_local_transaction_id = 0;
  u64 page_generation = 0;
  u64 reuse_horizon = 0;
  u32 page_type = 0;
  // Exactly one creator: transaction UUID + positive local number, or this
  // operation UUID with nil transaction UUID and local number zero. This is
  // lineage, not evidence of an authorized/durable operation.
  Uuid creator_operation_uuid;
  bool operator==(const NativeAllocationRecord&) const = default;
};
struct NativeAllocationMap {
  disk::NativeCommonPageHeader header;
  Uuid object_uuid;
  u64 map_generation = 0;
  u64 capacity_generation = 0;
  u64 total_pages = 0;
  u64 first_page = 0;
  Uuid creator_transaction_uuid;
  u64 creator_local_transaction_id = 0;
  std::optional<disk::NativePageReference> next;
  std::array<byte, 32> next_sha256{};
  std::vector<NativeAllocationState> states;
  std::vector<NativeAllocationRecord> records;
  Uuid creator_operation_uuid;
};
enum class NativeAllocationError {
  none, invalid_header, invalid_family, invalid_identity, invalid_range,
  invalid_state, invalid_record, invalid_reference, invalid_integrity,
  hash_failure, resource_exhausted, invalid_filespace, binding_mismatch,
  chain_mismatch, physical_owner_mismatch, counter_mismatch, io_failure,
  cluster_requires_authority
};
struct NativeAllocationMapResult {
  NativeAllocationError error = NativeAllocationError::invalid_family;
  std::optional<NativeAllocationMap> map;
  std::vector<byte> bytes;
  bool ok() const noexcept { return error == NativeAllocationError::none && map.has_value(); }
};
NativeAllocationMapResult EncodeNativeAllocationMap(const NativeAllocationMap&) noexcept;
NativeAllocationMapResult DecodeNativeAllocationMap(const std::vector<byte>&) noexcept;

struct NativeAllocationChainResult {
  NativeAllocationError error = NativeAllocationError::invalid_family;
  std::vector<NativeAllocationMapResult> pages;
  std::array<u64, 8> state_counts{};
  u64 retained_image_bytes = 0;
  bool ok() const noexcept { return error == NativeAllocationError::none && !pages.empty(); }
};
// Read the actual page-zero allocation root under the retained device guard.
// Full chain/image and control-page binding, NOT checkpoint selection, retention
// authority, a free-page grant or completed mutation/publication.
NativeAllocationChainResult ReadNativeAllocationChainFromOpenDevice(
    disk::FileDevice&, const disk::FilespaceBootstrapBinding&,
    u64 maximum_retained_image_bytes) noexcept;
// Exact selected root, independently hash-bound by the checkpoint owner.
// Actual map counts replace stale bootstrap counters; no reuse/publication grant.
NativeAllocationChainResult ReadNativeAllocationChainAtRootFromOpenDevice(
    disk::FileDevice&,const disk::FilespaceBootstrapBinding&,
    const disk::FilespaceRootReference&,u64 maximum_retained_image_bytes) noexcept;
}  // namespace scratchbird::storage::page
