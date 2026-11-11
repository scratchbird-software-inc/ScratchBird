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
template<class B> struct BasicNativeBtreeKey {
  using Bytes = B;
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
  // A leaf cell's version's creating local transaction number (owner Q38 = 1:
  // the transaction inventory decides visibility without reading the base
  // row; the reader that loads the base row checks it) and its included
  // (covering) payload. Branch cells carry neither.
  u64 creator_local_transaction_id = 0;
  // SEARCH_KEY: SB_NATIVE_BTREE_SUPERSEDING (owner Q42 = 1, 2026-11-09). The
  // local transaction that wrote this version's successor (a rename or drop),
  // stamped into the old version's cells in the same careful-write plan as the
  // successor; 0 when none. Always above the creator (the successor's
  // transaction began after this version's creator committed). A reader decides
  // from the index which version its snapshot sees: the creator's state shows
  // when the version began, this one's when it ended; a rolled-back successor's
  // state hides its stamp, as MGA's back versions do. Leaf cells only. The
  // row-UUID trees are never stamped (they index a row's first version only and
  // successors do not touch them), so an index-only answer from them is not
  // complete for a row with later versions.
  u64 superseding_local_transaction_id = 0;
  typename Key::Bytes payload{};
};
using NativeBtreeCell = BasicNativeBtreeCell<NativeBtreeKey>;
using NativeBtreeCellView = BasicNativeBtreeCell<NativeBtreeKeyView>;
template<class KeyType,class Cells> struct BasicNativeBtreePage {
  using Key = KeyType;
  disk::NativeCommonPageHeader header;
  NativeBtreeDependencies dependencies;
  // The creating transaction; both nil/0 for a system page a later change
  // added (SB_NATIVE_BTREE_SYSTEM_PAGE, owner Q30 = 1).
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
  invalid_workspace,
  key_too_long  // one entry's key and payload exceed a quarter page (no split can fit it)
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
// SEARCH_KEY: SB_CATALOG_SPLIT_LAG (owner Q45 = 1, 2026-11-10; FirebirdSQL
// btr.cpp: the split page is written first, then the original page with its
// sibling pointer, then the parent, and readers walk btr_sibling when a key is
// past a page's end). A split interrupted after the old page's rewrite and
// before the parent's leaves the old page's high fence short of the bound its
// parent gives it, with its right link naming the new page, which no parent
// lists yet. A walk validates such a page against its own high fence and then
// visits the right sibling as an unlisted page of the same parent, between the
// two fences, before the parent's next child; the next child's left link may
// still name the old page (the neighbour's rewrite comes after the old page's).
// The next write into the tree completes the split (the writers' repair).
// Only a right sibling the parent does not list is unlisted: one the parent
// lists makes the short fence a fault (tree_fence_mismatch), as before.
template<class Page> bool NativeBtreeSplitLags(const Page& page,const Page& parent,const std::optional<typename Page::Key>& high) noexcept {
  if(!page.right||!page.high_fence)return false;
  if(high&&CompareNativeBtreeKeys(*page.high_fence,*high)>=0)return false;
  const auto same=[&](const auto& r){return r&&r->filespace_uuid==page.right->filespace_uuid&&r->page_number==page.right->page_number;};
  if(same(parent.first_child))return false;
  for(const auto& c:parent.cells)if(same(c.child))return false;
  return true;
}
template<class Page> NativeBtreeError ValidateNativeBtreeTraversalStep(
    const Page& page,const Page* parent,u16 level,
    const std::optional<typename Page::Key>& low,
    const std::optional<typename Page::Key>& high,const Page* prior,bool prior_unlisted=false) noexcept {
  using E=NativeBtreeError;
  const auto self=[](const Page& p){const auto& h=p.header;
    return disk::NativePageReference{h.filespace_uuid,h.page_number,h.page_generation,h.page_size_profile_uuid};};
  // SB_CATALOG_PARENT_UNCHECKED (owner Q46 = 1, 2026-11-10): a page's parent
  // is the page the walk reached it from; the parent reference a page stores
  // is written but no longer checked. A branch split moves children whose
  // stored references would otherwise all have to change, and no write order
  // keeps them consistent through a crash (FirebirdSQL's pages carry none).
  if(parent){
    if(page.tree_level!=level)return E::tree_level_mismatch;
  }
  if(page.low_fence!=low||page.high_fence!=high)return E::tree_fence_mismatch;
  if(!prior){if(page.left)return E::tree_sibling_mismatch;}
  else {
    // After an unlisted page (SB_CATALOG_SPLIT_LAG) the left link may still
    // name the page before it.
    if(prior->right!=std::optional{self(page)}||
       (page.left!=std::optional{self(*prior)}&&!(prior_unlisted&&page.left==prior->left)))return E::tree_sibling_mismatch;
    if(!page.low_fence||!prior->high_fence||page.low_fence!=prior->high_fence)return E::tree_fence_mismatch;
  }
  return E::none;
}
} // namespace detail
// Empty filespace UUID marks unused scratch; page zero keys profile agreement.
// Positive pages key direct-child uniqueness, independently of generation.
struct NativeBtreeReferenceSlot {
  Uuid filespace_uuid,profile_uuid;
  u64 page_number = 0;
};
struct NativeBtreeViewWorkspace {
  std::span<NativeBtreeCellView> cells;
  std::span<NativeBtreeReferenceSlot> references;
  std::span<byte> keys{};  // the rebuilt prefix-compressed keys
};
struct NativeBtreeViewRequirements {
  std::size_t cells = 0, reference_slots = 0, key_bytes = 0;
};
struct NativeBtreePageViewResult {
  NativeBtreeError error = NativeBtreeError::invalid_family;
  std::optional<NativeBtreePageView> page;
  std::span<const byte> bytes;
  bool ok() const noexcept {return error==NativeBtreeError::none&&page.has_value();}
};
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
// SEARCH_KEY: SB_NATIVE_BTREE_COMPRESSED_PAGE (owner rulings Q37 = 1, Q38 = 1,
// Q40, 2026-11-09). SBBTP002: the native catalog B-tree page with FirebirdSQL-
// style key compression. It replaced SBBTP001 (fixed 144-byte cells plus an
// uncompressed key) for new databases; the common header, family header
// fields, page types, dependency tuple, fences, references and whole-page seal
// (family offset 384) are SBBTP001's; the cells differ:
//   - packed in key order with no per-cell offset table; each stores the
//     bytes its encoded key shares with the previous cell's (a LEB128 count)
//     and the rest (FirebirdSQL btn.h IndexNode prefix/length);
//   - jump nodes as restart points: every kNativeBtreeJumpInterval-th cell
//     stores its whole key (shared count 0) and the page lists those cells'
//     offsets, so a search can start near its key (FirebirdSQL keeps the key
//     in a separate jump node instead; same purpose, slightly less
//     compression at each restart cell);
//   - a leaf cell carries its version's creating local transaction number, an
//     optional superseding one (owner Q42 = 1) and an optional included payload
//     (covering trees, owner Q38 = 1);
//   - a reference to a page in this page's filespace and profile is a fixed
//     page number and generation (16 bytes); others are a full PageRefV2;
//   - when every cell's key is its own 16-byte row UUID (the raw16 row trees)
//     the row UUID is not stored a second time (page flag row_is_key).
// The encoding is canonical: every field has one valid form (the longest
// shared prefix, the compact reference whenever eligible, minimal LEB128, the
// row_is_key flag exactly when every cell qualifies), so encoding a decoded
// page gives back its bytes. Limits: one entry's key plus payload at most a
// quarter page (FirebirdSQL Database.h getMaxIndexKeyLength: key_too_long, which
// no split can cure); the rebuilt keys of a page at most one page
// (resource_exhausted: the page is full), so a reader reserves as many key bytes
// as image bytes. Layout in the spec annotation SB_NATIVE_BTREE_COMPRESSED_PAGE
// (NATIVE-BTREE-PAGE-IMAGE-001).
inline constexpr u16 kNativeBtreeJumpInterval = 16;
// The smallest cell (flags, shared and suffix counts, version UUID, compact
// reference): the most cells any page of a size can hold.
inline constexpr std::size_t kNativeBtreeMinimumCellBytes = 35;
NativeBtreePageResult EncodeNativeBtreePage(const NativeBtreePage&) noexcept;
NativeBtreePageResult DecodeNativeBtreePage(const std::vector<byte>&) noexcept;
// Complete unchanged image admission, not a grant or tree/visibility receipt.
// Allocation-free: keys are rebuilt into workspace.keys, payloads and fences
// borrow the image. All aligned, disjoint backing and immutable input outlive
// every returned view; sized by NativeBtreePageViewBackingRequirements for any
// valid page. Failed calls expose no page prefix; scratch contents then are
// unspecified.
NativeBtreePageViewResult DecodeNativeBtreePageInto(std::span<const byte>,NativeBtreeViewWorkspace) noexcept;
NativeBtreeViewRequirements NativeBtreePageViewBackingRequirements(std::size_t image_bytes) noexcept;
NativeBtreeTreeResult ReadNativeBtreeTreeFromOpenDevices(
  const Uuid& database_uuid,const std::vector<disk::NativeFilespaceDevice>&,
  const disk::NativePageReference& root,const NativeBtreeDependencies&,
  u64 maximum_retained_image_bytes) noexcept;
} // namespace scratchbird::storage::page
