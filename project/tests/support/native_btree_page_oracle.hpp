// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
//
// SEARCH_KEY: SB_NATIVE_BTREE_COMPRESSED_PAGE
// An independent byte-packing oracle for SBBTP002 catalog B-tree pages (spec
// annotation SB_NATIVE_BTREE_COMPRESSED_PAGE): its own LEB128, shared-prefix and
// layout rules and OpenSSL SHA-256; the production codec supplies only the
// 128-byte common header (another format). Knobs produce the malformed forms
// the decoders must refuse. Shared by native_btree_page_v2_test and
// filespace_page_zero_test. Test support only.
#pragma once
#include "native_common_page_header.hpp"
#include "native_index_btree_page.hpp"

#include <openssl/sha.h>

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>
#include <vector>

namespace scratchbird::tests::btree_oracle {
namespace d = scratchbird::storage::disk;
namespace pg = scratchbird::storage::page;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::u64;
using scratchbird::core::platform::Uuid;
using Bytes = std::vector<byte>;
inline void Require(bool ok, const char* what) {
  if (!ok) throw std::runtime_error(what);
}
struct Knobs {
  int overlong_shared_at = -1;     // cell i's shared count as a two-byte LEB128
  int short_shared_at = -1;        // cell i shares one byte fewer than it could
  bool full_references = false;    // every reference written in full
  bool rows_always = false;        // row UUIDs stored and the flag clear even when they are the keys
  int empty_payload_flag_at = -1;  // cell i's payload flag set with length 0
  int zero_superseding_at = -1;    // cell i's superseding flag set with the number 0
};
inline void Le(Bytes& b, std::size_t at, u64 v, unsigned n) {
  for (unsigned i = 0; i < n; ++i) b[at + i] = static_cast<byte>(v >> (8 * i));
}
inline void Leb(Bytes& out, u64 v) {
  do {
    byte x = static_cast<byte>(v & 0x7f);
    v >>= 7;
    if (v) x |= 0x80;
    out.push_back(x);
  } while (v);
}
inline void Raw(Bytes& out, const Uuid& u) { out.insert(out.end(), u.bytes.begin(), u.bytes.end()); }
inline void Raw64(Bytes& out, u64 v) {
  for (unsigned i = 0; i < 8; ++i) out.push_back(static_cast<byte>(v >> (8 * i)));
}
inline void RefFull(Bytes& out, const d::NativePageReference& r) {
  Raw(out, r.filespace_uuid);
  Raw64(out, r.page_number);
  Raw64(out, r.page_generation);
  Raw(out, r.page_size_profile_uuid);
}
inline void Seal(Bytes& b) {
  std::fill(b.begin() + 512, b.begin() + 544, 0);
  std::array<byte, 32> digest{};
  Require(SHA256(b.data(), b.size(), digest.data()) != nullptr, "independent SHA-256");
  std::copy(digest.begin(), digest.end(), b.begin() + 512);
}
// Owning pages and decoded views alike.
template <class Page> Bytes Pack(const Page& r, const Knobs& k = {}) {
  const std::size_t size = r.header.page_size_bytes;
  Bytes b(size, 0);
  const auto common = d::EncodeNativeCommonPageHeader(r.header);
  Require(common.ok(), "the common header encodes");
  std::copy(common.bytes->begin(), common.bytes->end(), b.begin());
  const bool branch = r.tree_level > 0;
  bool row_is_key = !r.cells.empty() && !k.rows_always;
  for (const auto& c : r.cells)
    if (c.key.encoded_key.size() != 16 ||
        !std::equal(c.key.encoded_key.begin(), c.key.encoded_key.end(), c.key.row_uuid.bytes.begin()))
      row_is_key = false;
  Bytes body;
  std::array<std::pair<u64, u64>, 2> fences{};
  for (int f = 0; f < 2; ++f) {
    const auto& key = f ? r.high_fence : r.low_fence;
    if (!key) continue;
    fences[f] = {640 + body.size(), 40 + key->encoded_key.size()};
    Bytes h(8, 0);
    Le(h, 0, key->encoded_key.size(), 4);
    body.insert(body.end(), h.begin(), h.end());
    Raw(body, key->row_uuid);
    Raw(body, key->version_uuid);
    body.insert(body.end(), key->encoded_key.begin(), key->encoded_key.end());
  }
  const std::size_t jumps = (r.cells.size() + 15) / 16, table = 640 + body.size();
  body.resize(body.size() + 4 * jumps);
  std::vector<u64> restarts;
  u64 expanded = 0;
  for (std::size_t i = 0; i < r.cells.size(); ++i) {
    const auto& c = r.cells[i];
    if (i % 16 == 0) restarts.push_back(640 + body.size());
    // A malformed page (a cell with the other kind's reference, as when a
    // branch is relabelled a leaf) is packed as given, for the decoders to
    // refuse; a cell with no reference at all cannot be represented.
    Require(c.child.has_value() || c.base_page.has_value(), "SBBTP002 cells always carry a reference");
    const auto& ref = branch ? (c.child ? *c.child : *c.base_page) : (c.base_page ? *c.base_page : *c.child);
    const bool compact = !k.full_references && ref.filespace_uuid == r.header.filespace_uuid &&
                         ref.page_size_profile_uuid == r.header.page_size_profile_uuid;
    const bool payload_flag = !c.payload.empty() || int(i) == k.empty_payload_flag_at;
    const bool superseding_flag = c.superseding_local_transaction_id || int(i) == k.zero_superseding_at;
    body.push_back(static_cast<byte>((c.deleted ? 1 : 0) | (compact ? 2 : 0) | (payload_flag ? 4 : 0) | (superseding_flag ? 8 : 0)));
    std::size_t shared = 0;
    if (i % 16) {
      const auto& prev = r.cells[i - 1].key.encoded_key;
      while (shared < prev.size() && shared < c.key.encoded_key.size() && prev[shared] == c.key.encoded_key[shared]) ++shared;
      if (int(i) == k.short_shared_at && shared) --shared;
    }
    if (int(i) == k.overlong_shared_at) {
      body.push_back(static_cast<byte>(0x80 | (shared & 0x7f)));
      body.push_back(static_cast<byte>(shared >> 7));
    } else {
      Leb(body, shared);
    }
    Leb(body, c.key.encoded_key.size() - shared);
    body.insert(body.end(), c.key.encoded_key.begin() + static_cast<std::ptrdiff_t>(shared), c.key.encoded_key.end());
    expanded += c.key.encoded_key.size();
    if (!row_is_key) Raw(body, c.key.row_uuid);
    Raw(body, c.key.version_uuid);
    if (!branch) Leb(body, c.creator_local_transaction_id);
    if (superseding_flag) Leb(body, c.superseding_local_transaction_id);
    if (compact) {
      Raw64(body, ref.page_number);
      Raw64(body, ref.page_generation);
    } else {
      RefFull(body, ref);
    }
    if (payload_flag) {
      Leb(body, c.payload.size());
      body.insert(body.end(), c.payload.begin(), c.payload.end());
    }
  }
  for (std::size_t j = 0; j < jumps; ++j) Le(body, table - 640 + 4 * j, restarts[j], 4);
  Require(640 + body.size() <= size, "the oracle's page fits");
  std::copy(body.begin(), body.end(), b.begin() + 640);
  const std::string magic = "SBBTP002";
  std::copy(magic.begin(), magic.end(), b.begin() + 128);
  Le(b, 136, 2, 2);
  Le(b, 138, 512, 2);
  Le(b, 140, 640 + body.size(), 4);
  const auto& dep = r.dependencies;
  std::copy(dep.index_uuid.bytes.begin(), dep.index_uuid.bytes.end(), b.begin() + 144);
  Le(b, 160, dep.descriptor_generation, 8);
  Le(b, 168, dep.storage_generation, 8);
  std::copy(dep.key_profile_uuid.bytes.begin(), dep.key_profile_uuid.bytes.end(), b.begin() + 176);
  std::copy(dep.visibility_profile_uuid.bytes.begin(), dep.visibility_profile_uuid.bytes.end(), b.begin() + 192);
  std::copy(dep.dependency_map_uuid.bytes.begin(), dep.dependency_map_uuid.bytes.end(), b.begin() + 208);
  std::copy(dep.dependency_map_sha256.begin(), dep.dependency_map_sha256.end(), b.begin() + 224);
  std::copy(r.creator_transaction_uuid.bytes.begin(), r.creator_transaction_uuid.bytes.end(), b.begin() + 256);
  Le(b, 272, r.creator_local_transaction_id, 8);
  Le(b, 280, r.maintenance_state, 2);
  Le(b, 282, r.tree_level, 2);
  Le(b, 284, row_is_key ? 1 : 0, 2);
  Le(b, 286, 16, 2);
  const auto ref_at = [&](std::size_t at, const auto& p) {
    if (!p) return;
    Bytes out;
    RefFull(out, *p);
    std::copy(out.begin(), out.end(), b.begin() + static_cast<std::ptrdiff_t>(at));
  };
  ref_at(288, r.parent);
  ref_at(336, r.left);
  ref_at(384, r.right);
  ref_at(432, r.first_child);
  Le(b, 480, fences[0].first, 4);
  Le(b, 484, fences[0].second, 4);
  Le(b, 488, fences[1].first, 4);
  Le(b, 492, fences[1].second, 4);
  Le(b, 496, r.cells.size(), 4);
  Le(b, 500, std::count_if(r.cells.begin(), r.cells.end(), [](const auto& c) { return c.deleted; }), 4);
  Le(b, 504, table, 4);
  Le(b, 508, jumps, 4);
  Le(b, 544, expanded, 4);
  Seal(b);
  return b;
}

}  // namespace scratchbird::tests::btree_oracle
