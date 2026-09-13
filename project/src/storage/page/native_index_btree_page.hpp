// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "filespace_page_zero.hpp"
#include "native_common_page_header.hpp"
#include <array>
#include <optional>
#include <vector>

namespace scratchbird::storage::page {
using scratchbird::core::platform::Uuid;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

// NATIVE-BTREE-PAGE-IMAGE-001. Exact catalog-owned dependency binding, not a
// caller assertion that dependencies, key encoding or visibility were checked.
struct NativeBtreeDependencies {
  Uuid index_uuid;
  u64 descriptor_generation = 0;
  u64 storage_generation = 0;
  Uuid key_profile_uuid;
  Uuid visibility_profile_uuid;
  Uuid dependency_map_uuid;
  std::array<byte,32> dependency_map_sha256{};
  bool operator==(const NativeBtreeDependencies&) const = default;
};
struct NativeBtreeKey {
  std::vector<byte> encoded_key;
  Uuid row_uuid;
  Uuid version_uuid;
  bool operator==(const NativeBtreeKey&) const = default;
};
struct NativeBtreeCell {
  NativeBtreeKey key;
  bool deleted = false;
  std::optional<disk::NativePageReference> child;
  std::optional<disk::NativePageReference> base_page;
};
struct NativeBtreePage {
  disk::NativeCommonPageHeader header;
  NativeBtreeDependencies dependencies;
  Uuid creator_transaction_uuid;
  u64 creator_local_transaction_id = 0;
  u16 maintenance_state = 0;
  u16 tree_level = 0;
  std::optional<disk::NativePageReference> parent,left,right,first_child;
  std::optional<NativeBtreeKey> low_fence,high_fence;
  std::vector<NativeBtreeCell> cells;
};
enum class NativeBtreeError {
  none, invalid_header, invalid_family, invalid_dependencies, invalid_reference,
  invalid_order, invalid_fence, invalid_integrity, hash_failure, resource_exhausted,
  invalid_filespace, binding_mismatch, io_failure, encrypted_requires_crypto_authority,
  cluster_requires_authority, header_policy_requires_authority
};
struct NativeBtreePageResult {
  NativeBtreeError error = NativeBtreeError::invalid_family;
  std::optional<NativeBtreePage> page;
  std::vector<byte> bytes;
  bool ok() const noexcept { return error==NativeBtreeError::none&&page.has_value(); }
};
int CompareNativeBtreeKeys(const NativeBtreeKey&,const NativeBtreeKey&) noexcept;
NativeBtreePageResult EncodeNativeBtreePage(const NativeBtreePage&) noexcept;
NativeBtreePageResult DecodeNativeBtreePage(const std::vector<byte>&) noexcept;
// Borrow the retained owner. This verifies only the exact image/dependency
// binding, not the dependency-map contents, tree, creator outcome or serving.
NativeBtreePageResult ReadNativeBtreePageFromOpenDevice(
  disk::FileDevice&,const Uuid& database_uuid,const disk::NativePageReference&,
  u32 page_type,const NativeBtreeDependencies&) noexcept;
} // namespace scratchbird::storage::page
