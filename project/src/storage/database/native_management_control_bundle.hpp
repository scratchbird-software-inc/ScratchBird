// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_allocation_map.hpp"

namespace scratchbird::storage::database {
using core::platform::Uuid;
using core::platform::byte;
using core::platform::u64;
struct NativeManagementControlBundleRoot {
  disk::NativePageReference first;
  Uuid object_uuid,operation_uuid;
  u64 map_count=0,page_count=0;
  std::array<byte,32> aggregate_sha256{},first_page_sha256{};
  bool operator==(const NativeManagementControlBundleRoot&) const=default;
};
enum class NativeManagementControlBundleError {
  none,invalid_request,invalid_identity,invalid_extent,invalid_header,
  invalid_allocation,invalid_integrity,binding_mismatch,resource_exhausted,
  hash_failure,io_failure,bootstrap_failure,encrypted_requires_authority,
  cluster_requires_authority
};
struct NativeManagementControlBundleImage {
  NativeManagementControlBundleError error=NativeManagementControlBundleError::invalid_request;
  std::optional<NativeManagementControlBundleRoot> root;
  std::vector<std::vector<byte>> pages;
  bool ok() const noexcept{return error==NativeManagementControlBundleError::none&&root.has_value();}
};
struct NativeManagementControlBundleRead {
  NativeManagementControlBundleError error=NativeManagementControlBundleError::invalid_request;
  std::vector<std::vector<byte>> allocation_images;
  std::vector<disk::NativeCommonPageHeader> page_headers;
  u64 total_pages=0;
  bool ok() const noexcept{return error==NativeManagementControlBundleError::none&&!allocation_images.empty()&&!page_headers.empty()&&total_pages;}
};
// Complete immutable reconstruction input; not physical allocation, history,
// selection, kernel authorization or operation completion authority.
NativeManagementControlBundleError ValidateNativeManagementControlBundleRoot(
  const NativeManagementControlBundleRoot&,const Uuid& database,const Uuid& bootstrap,u64 budget) noexcept;
NativeManagementControlBundleImage EncodeNativeManagementControlBundle(
  const std::vector<std::vector<byte>>& allocation_images,const Uuid& database,
  const Uuid& bootstrap,const Uuid& object,const Uuid& attempt,
  const std::vector<disk::NativeCommonPageHeader>& headers,u64 budget) noexcept;
NativeManagementControlBundleRead DecodeNativeManagementControlBundle(
  const std::vector<std::vector<byte>>& pages,const NativeManagementControlBundleRoot&,
  const Uuid& database,const Uuid& bootstrap,u64 budget) noexcept;
NativeManagementControlBundleRead ReadNativeManagementControlBundleFromOpenDevice(
  const disk::NativeFilespaceDevice&,const NativeManagementControlBundleRoot&,
  const Uuid& database,const Uuid& bootstrap,u64 budget) noexcept;
} // namespace scratchbird::storage::database
