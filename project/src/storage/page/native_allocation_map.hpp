// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "native_common_page_header.hpp"
#include "filespace_page_zero.hpp"
#include "disk_device.hpp"
#include <array>
#include <optional>
#include <span>
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
template<class States,class Records> struct NativeAllocationMapData {
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
  States states;
  Records records;
  Uuid creator_operation_uuid;
};
using NativeAllocationMap=NativeAllocationMapData<std::vector<NativeAllocationState>,
  std::vector<NativeAllocationRecord>>;
using NativeAllocationMapView=NativeAllocationMapData<std::span<NativeAllocationState>,
  std::span<NativeAllocationRecord>>;
enum class NativeAllocationError {
  none, invalid_header, invalid_family, invalid_identity, invalid_range,
  invalid_state, invalid_record, invalid_reference, invalid_integrity,
  hash_failure, resource_exhausted, invalid_filespace, binding_mismatch,
  chain_mismatch, physical_owner_mismatch, counter_mismatch, io_failure,
  cluster_requires_authority, physical_extent_changed, invalid_workspace
};
struct NativeAllocationMapResult {
  NativeAllocationError error = NativeAllocationError::invalid_family;
  std::optional<NativeAllocationMap> map;
  std::vector<byte> bytes;
  bool ok() const noexcept { return error == NativeAllocationError::none && map.has_value(); }
};
NativeAllocationMapResult EncodeNativeAllocationMap(const NativeAllocationMap&) noexcept;
NativeAllocationMapResult DecodeNativeAllocationMap(const std::vector<byte>&) noexcept;
struct NativeAllocationMapViewResult {
  NativeAllocationError error=NativeAllocationError::invalid_family;
  std::optional<NativeAllocationMapView> map;
  bool ok() const noexcept {return error==NativeAllocationError::none&&map.has_value();}
};
// No image copy or metadata allocation. Caller-owned decoded buffers outlive
// this view and must not overlap the input or each other. On failure there is
// no usable view; scratch contents are unspecified, never a decoded prefix.
// This validates an image, not its allocation chain or current-root authority.
NativeAllocationMapViewResult DecodeNativeAllocationMapInto(std::span<const byte>,
    std::span<NativeAllocationState>,std::span<NativeAllocationRecord>) noexcept;

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
// STORAGE-NATIVE-HISTORICAL-ALLOCATION-READ-001. Verification of retained
// immutable history, NOT current capacity, recovery selection or mutation
// authority. The owner must bind the historical page-zero image and root digest
// to its actual publication. Extra physical bytes never become free pages.
// The image ceiling includes historical page zero, bootstrap probe and maps.
NativeAllocationChainResult ReadNativeAllocationChainAtHistoricalRootFromOpenDevice(
    disk::FileDevice&, const disk::FilespaceBootstrapBinding&,
    const disk::FilespaceRootReference&, const std::array<byte, 32>& root_sha256,
    const std::vector<byte>& historical_page_zero,
    u64 maximum_retained_image_bytes) noexcept;

enum class NativeAllocationChainReadContext { bootstrap, selected, historical };
struct NativeAllocationChainPageView {
  NativeAllocationMapView map;
  std::span<const byte> image;
};
struct NativeAllocationChainView {
  NativeAllocationError error=NativeAllocationError::invalid_reference;
  std::span<const NativeAllocationChainPageView> pages;
  std::array<u64,8> state_counts{};
  u64 retained_image_bytes=0;
  std::size_t backing_bytes_used=0;
  bool ok() const noexcept {return error==NativeAllocationError::none&&!pages.empty();}
};
struct NativeAllocationChainDeviceRead {
  NativeAllocationChainView chain;
  core::platform::Status io_status;
  core::platform::DiagnosticRecord io_diagnostic;
  u64 physical_bytes_read=0;
  bool ok() const noexcept {return chain.ok();}
};
// Complete bootstrap / explicit current / historical chain, sharing all owning
// reader validation. Every image, decoded array, index and retained descriptor
// lives in backing. The exact-device batch must outlive ALL enclosing fences.
// All backing is disjoint from input descriptors, historical bytes, device and
// batch objects. Failure withholds the entire chain while retaining actual I/O
// diagnostics/bytes. No capacity, creator admission, selection or reuse grant.
NativeAllocationChainDeviceRead ReadNativeAllocationChainInto(
    disk::FileDevice&,const disk::FilespaceBootstrapBinding&,
    u64 maximum_retained_image_bytes,NativeAllocationChainReadContext,
    const disk::FilespaceRootReference*,const std::array<byte,32>* root_sha256,
    std::span<const byte> historical_page_zero,
    disk::FileDevice::ReadLatencyBatch&,std::span<byte> backing) noexcept;

}  // namespace scratchbird::storage::page
