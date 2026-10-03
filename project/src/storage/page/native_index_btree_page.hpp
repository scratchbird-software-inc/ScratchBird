// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "filespace_page_zero.hpp"
#include "native_common_page_header.hpp"
#include <array>
#include <optional>
#include <algorithm>
#include <span>
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
template<class Bytes> struct BasicNativeBtreeKey {
  Bytes encoded_key;
  Uuid row_uuid;
  Uuid version_uuid;
  bool operator==(const BasicNativeBtreeKey& other) const noexcept {
    return row_uuid==other.row_uuid&&version_uuid==other.version_uuid&&
      std::equal(encoded_key.begin(),encoded_key.end(),other.encoded_key.begin(),other.encoded_key.end());
  }
};
using NativeBtreeKey = BasicNativeBtreeKey<std::vector<byte>>;
using NativeBtreeKeyView = BasicNativeBtreeKey<std::span<const byte>>;
template<class Key> struct BasicNativeBtreeCell {
  Key key;
  bool deleted = false;
  std::optional<disk::NativePageReference> child;
  std::optional<disk::NativePageReference> base_page;
};
using NativeBtreeCell = BasicNativeBtreeCell<NativeBtreeKey>;
using NativeBtreeCellView = BasicNativeBtreeCell<NativeBtreeKeyView>;
template<class KeyType,class Cells> struct BasicNativeBtreePage {
  using Key = KeyType;
  disk::NativeCommonPageHeader header;
  NativeBtreeDependencies dependencies;
  Uuid creator_transaction_uuid;
  u64 creator_local_transaction_id = 0;
  u16 maintenance_state = 0;
  u16 tree_level = 0;
  std::optional<disk::NativePageReference> parent,left,right,first_child;
  std::optional<Key> low_fence,high_fence;
  Cells cells;
};
using NativeBtreePage = BasicNativeBtreePage<NativeBtreeKey,std::vector<NativeBtreeCell>>;
using NativeBtreePageView = BasicNativeBtreePage<NativeBtreeKeyView,std::span<const NativeBtreeCellView>>;
enum class NativeBtreeError {
  none, invalid_header, invalid_family, invalid_dependencies, invalid_reference,
  invalid_order, invalid_fence, invalid_integrity, hash_failure, resource_exhausted,
  invalid_filespace, binding_mismatch, io_failure, encrypted_requires_crypto_authority,
  cluster_requires_authority, header_policy_requires_authority,
  tree_reference_mismatch, tree_level_mismatch, tree_fence_mismatch, tree_sibling_mismatch,
  invalid_workspace
};
struct NativeBtreePageResult {
  NativeBtreeError error = NativeBtreeError::invalid_family;
  std::optional<NativeBtreePage> page;
  std::vector<byte> bytes;
  bool ok() const noexcept { return error==NativeBtreeError::none&&page.has_value(); }
};
int CompareNativeBtreeKeys(const NativeBtreeKey&,const NativeBtreeKey&) noexcept;
int CompareNativeBtreeKeys(const NativeBtreeKeyView&,const NativeBtreeKeyView&) noexcept;
bool NativeBtreeDependenciesValid(const NativeBtreeDependencies&) noexcept;
namespace detail {
// Shared whole-tree step: decoded page validity alone cannot establish these
// inherited ranges, parent bindings or cross-parent sibling relationships.
template<class Page> NativeBtreeError ValidateNativeBtreeTraversalStep(
    const Page& page,const Page* parent,u16 level,
    const std::optional<typename Page::Key>& low,
    const std::optional<typename Page::Key>& high,const Page* prior) noexcept {
  using E=NativeBtreeError;
  const auto self=[](const Page& p){const auto& h=p.header;
    return disk::NativePageReference{h.filespace_uuid,h.page_number,h.page_generation,h.page_size_profile_uuid};};
  if(parent){
    if(page.parent!=std::optional{self(*parent)})return E::tree_reference_mismatch;
    if(page.tree_level!=level)return E::tree_level_mismatch;
  }
  if(page.low_fence!=low||page.high_fence!=high)return E::tree_fence_mismatch;
  if(!prior){if(page.left)return E::tree_sibling_mismatch;}
  else {
    if(prior->right!=std::optional{self(page)}||page.left!=std::optional{self(*prior)})return E::tree_sibling_mismatch;
    if(!page.low_fence||!prior->high_fence||page.low_fence!=prior->high_fence)return E::tree_fence_mismatch;
  }
  return E::none;
}
} // namespace detail
NativeBtreePageResult EncodeNativeBtreePage(const NativeBtreePage&) noexcept;
NativeBtreePageResult DecodeNativeBtreePage(const std::vector<byte>&) noexcept;
// Empty filespace UUID marks unused scratch; page zero keys profile agreement.
// Positive pages key direct-child uniqueness, independently of generation.
struct NativeBtreeReferenceSlot {
  Uuid filespace_uuid,profile_uuid;
  u64 page_number = 0;
};
struct NativeBtreeViewWorkspace {
  std::span<NativeBtreeCellView> cells;
  std::span<NativeBtreeReferenceSlot> references;
};
struct NativeBtreeViewRequirements {
  std::size_t cells = 0, reference_slots = 0;
};
NativeBtreeViewRequirements NativeBtreePageViewBackingRequirements(std::size_t image_bytes) noexcept;
struct NativeBtreePageViewResult {
  NativeBtreeError error = NativeBtreeError::invalid_family;
  std::optional<NativeBtreePageView> page;
  std::span<const byte> bytes;
  bool ok() const noexcept {return error==NativeBtreeError::none&&page.has_value();}
};
// Complete unchanged image admission, not a grant or tree/visibility receipt.
// All aligned, disjoint backing and immutable input outlive every returned view.
// Failed calls expose no page prefix; scratch contents then are unspecified.
NativeBtreePageViewResult DecodeNativeBtreePageInto(
  std::span<const byte>,NativeBtreeViewWorkspace) noexcept;
// Borrow the retained owner. This verifies only the exact image/dependency
// binding, not the dependency-map contents, tree, creator outcome or serving.
NativeBtreePageResult ReadNativeBtreePageFromOpenDevice(
  disk::FileDevice&,const Uuid& database_uuid,const disk::NativePageReference&,
  u32 page_type,const NativeBtreeDependencies&) noexcept;
struct NativeBtreeTreeResult {
  NativeBtreeError error = NativeBtreeError::invalid_reference;
  // Preorder images; leaf indexes select them in verified navigation order.
  std::vector<NativeBtreePageResult> pages;
  std::vector<std::size_t> leaves;
  u64 retained_image_bytes = 0;
  bool ok() const noexcept { return error==NativeBtreeError::none&&!pages.empty()&&!leaves.empty(); }
};
// NATIVE-BTREE-RETAINED-TREE-001. Complete navigation-tree verification only;
// not a dependency-map, base-row/MGA, serving-generation or publication receipt.
NativeBtreeTreeResult ReadNativeBtreeTreeFromOpenDevices(
  const Uuid& database_uuid,const std::vector<disk::NativeFilespaceDevice>&,
  const disk::NativePageReference& root,const NativeBtreeDependencies&,
  u64 maximum_retained_image_bytes) noexcept;
} // namespace scratchbird::storage::page
