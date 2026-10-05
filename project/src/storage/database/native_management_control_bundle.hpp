// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_allocation_map.hpp"
#include "disk_device.hpp"

namespace scratchbird::storage::database {
using core::platform::Uuid;
using core::platform::byte;
using core::platform::u64;
struct NativeManagementControlBundleRoot {
  disk::NativePageReference first;
  Uuid object_uuid,operation_uuid;
  u64 map_count=0,page_count=0;
  std::array<byte,32> aggregate_sha256{},first_page_sha256{};
  u64 inventory_count=0;
  u64 directory_count=0,payload_bytes=0;
  u64 growth_image_count=0;
  bool operator==(const NativeManagementControlBundleRoot&) const=default;
};
enum class NativeManagementControlBundleError {
  none,invalid_request,invalid_identity,invalid_extent,invalid_header,
  invalid_allocation,invalid_integrity,binding_mismatch,resource_exhausted,
  hash_failure,io_failure,bootstrap_failure,encrypted_requires_authority,
  cluster_requires_authority,physical_extent_changed,invalid_workspace
};
struct NativeManagementControlBundleImage {
  NativeManagementControlBundleError error=NativeManagementControlBundleError::invalid_request;
  std::optional<NativeManagementControlBundleRoot> root;
  std::vector<std::vector<byte>> pages;
  bool ok() const noexcept{return error==NativeManagementControlBundleError::none&&root.has_value();}
};
struct NativeManagementControlBundleEncoding {
  NativeManagementControlBundleError error=NativeManagementControlBundleError::invalid_request;
  std::optional<NativeManagementControlBundleRoot> root;
  std::span<const byte> pages;
  std::size_t backing_bytes_used=0;
  bool ok() const noexcept{return error==NativeManagementControlBundleError::none&&root.has_value();}
};
// Complete immutable construction. All validation metadata and payload use
// backing; ordered physical pages use output. Both entire regions must be
// disjoint from each other and every input region. No implicit heap fallback,
// actual memory grant, source admission or publication permission is supplied.
NativeManagementControlBundleEncoding EncodeNativeManagementControlBundleInto(
  std::span<const std::span<const byte>> allocation_images,const Uuid& database,
  const Uuid& bootstrap,const Uuid& object,const Uuid& attempt,
  std::span<const disk::NativeCommonPageHeader> headers,u64 budget,
  std::span<byte> output,std::span<byte> backing,
  std::span<const std::span<const byte>> inventory_images={},
  std::span<const std::span<const byte>> directory_images={},
  std::span<const std::span<const byte>> growth_images={}) noexcept;
struct NativeManagementControlBundleRead {
  NativeManagementControlBundleError error=NativeManagementControlBundleError::invalid_request;
  std::vector<std::vector<byte>> allocation_images;
  std::vector<disk::NativeCommonPageHeader> page_headers;
  u64 total_pages=0;
  std::vector<std::vector<byte>> inventory_images;
  std::vector<std::vector<byte>> directory_images;
  // Exact canonical before/after page-zero images; never recovery authority.
  std::vector<std::vector<byte>> growth_images;
  bool ok() const noexcept{return error==NativeManagementControlBundleError::none&&!allocation_images.empty()&&!page_headers.empty()&&total_pages;}
};

struct NativeManagementControlBundleViewRead {
  NativeManagementControlBundleError error=NativeManagementControlBundleError::invalid_request;
  std::span<const std::span<const byte>> allocation_images;
  std::span<const disk::NativeCommonPageHeader> page_headers;
  u64 total_pages=0;
  std::span<const std::span<const byte>> inventory_images,directory_images,growth_images;
  std::size_t backing_bytes_used=0;
  // Actual-file reads retain exact physical chunks in the same backing.
  // Pure decoding does not claim observation of physical storage.
  std::span<const std::span<const byte>> physical_images;
  bool ok() const noexcept{return error==NativeManagementControlBundleError::none&&!allocation_images.empty()&&!page_headers.empty()&&total_pages;}
};
// Complete shared validation of every bundle/image family. The supplied buffer
// owns all retained payload/descriptors/headers and all temporary decoded values,
// uniqueness containers and inventory validation scratch. No heap fallback.
// Backing must outlive the result and must not overlap any complete input region,
// its descriptor array or identities/root. Failure exposes no usable prefix;
// backing contents are unspecified. Used bytes include alignment and temporary
// allocations, not a memory grant or recovery/publication authorization.
NativeManagementControlBundleViewRead DecodeNativeManagementControlBundleInto(
  std::span<const std::span<const byte>>,const NativeManagementControlBundleRoot&,
  const Uuid& database,const Uuid& bootstrap,u64 budget,std::span<byte> backing) noexcept;


enum class NativeManagementControlReadContext {current,historical_before,historical_result};
struct NativeManagementControlBundleDeviceRead {
  NativeManagementControlBundleViewRead bundle;
  core::platform::Status io_status;
  core::platform::DiagnosticRecord io_diagnostic;
  u64 physical_bytes_read=0;
  bool ok() const noexcept{return bundle.ok();}
};
// Complete current or explicitly retained historical observation, not selection
// or recovery authority. The caller supplies backing and a batch bound to the
// exact device. Construct that batch before every enclosing source fence and
// retain it until all such fences release. This function holds the device guard
// throughout its complete read but does not flush the caller's observations.
// Retain the actual last I/O status/diagnostic and all transferred bytes even
// when validation fails. These observations do not authorize physical effects.
NativeManagementControlBundleDeviceRead ReadNativeManagementControlBundleInto(
  const disk::NativeFilespaceDevice&,const NativeManagementControlBundleRoot&,
  const Uuid& database,const Uuid& bootstrap,u64 budget,NativeManagementControlReadContext,
  std::span<const byte> retained_page_zero,disk::FileDevice::ReadLatencyBatch&,
  std::span<byte> backing) noexcept;
// Complete immutable reconstruction input; not physical allocation, history,
// selection, kernel authorization or operation completion authority.
NativeManagementControlBundleError ValidateNativeManagementControlBundleRoot(
  const NativeManagementControlBundleRoot&,const Uuid& database,const Uuid& bootstrap,u64 budget) noexcept;
NativeManagementControlBundleImage EncodeNativeManagementControlBundle(
  const std::vector<std::vector<byte>>& allocation_images,const Uuid& database,
  const Uuid& bootstrap,const Uuid& object,const Uuid& attempt,
  const std::vector<disk::NativeCommonPageHeader>& headers,u64 budget,
  const std::vector<std::vector<byte>>& inventory_images={},
  const std::vector<std::vector<byte>>& directory_images={},
  const std::vector<std::vector<byte>>& growth_images={}) noexcept;
NativeManagementControlBundleRead DecodeNativeManagementControlBundle(
  const std::vector<std::vector<byte>>& pages,const NativeManagementControlBundleRoot&,
  const Uuid& database,const Uuid& bootstrap,u64 budget) noexcept;
NativeManagementControlBundleRead ReadNativeManagementControlBundleFromOpenDevice(
  const disk::NativeFilespaceDevice&,const NativeManagementControlBundleRoot&,
  const Uuid& database,const Uuid& bootstrap,u64 budget) noexcept;
// Read-only original-image context. The caller must authenticate the retained
// image/root against the original publication before admitting any effects.
// Can read an already anchored bundle with a torn current mutable metadata body.
NativeManagementControlBundleRead ReadNativeManagementControlBundleAtHistoricalPageZeroFromOpenDevice(
  const disk::NativeFilespaceDevice&,const NativeManagementControlBundleRoot&,
  const Uuid& database,const Uuid& bootstrap,
  const std::vector<byte>& retained_page_zero,u64 budget) noexcept;
// Reverse-history counterpart: exact primary result context, not an original
// execution grant. For primary growth the bundle's after image must match and
// its original placement must still fit its retained before capacity.
NativeManagementControlBundleRead ReadNativeManagementControlBundleAtHistoricalResultFromOpenDevice(
  const disk::NativeFilespaceDevice&,const NativeManagementControlBundleRoot&,
  const Uuid& database,const Uuid& bootstrap,
  const std::vector<byte>& retained_result_page_zero,u64 budget) noexcept;
} // namespace scratchbird::storage::database
