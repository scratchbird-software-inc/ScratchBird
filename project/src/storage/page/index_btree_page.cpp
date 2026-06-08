// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_btree_page.hpp"

#include "database_format.hpp"
#include "page_header.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <set>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace scratchbird::storage::page {
namespace {

using scratchbird::core::datatypes::DecodeDatatypeBinaryValue;
using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::LoadLittle16;
using scratchbird::core::platform::LoadLittle32;
using scratchbird::core::platform::LoadLittle64;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::StoreLittle16;
using scratchbird::core::platform::StoreLittle32;
using scratchbird::core::platform::StoreLittle64;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::Uuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::uuid::IsEngineIdentityUuid;
using scratchbird::core::uuid::CompareUuid128;
using scratchbird::storage::disk::kPageHeaderSerializedBytes;

inline constexpr std::array<byte, 8> kIndexBtreeMagic = {'S', 'B', 'I', 'D', 'X', '0', '0', '1'};
inline constexpr std::array<byte, 4> kOrderPreservingKeyMagic = {'S', 'B', 'K', 'O'};
inline constexpr std::array<byte, 4> kUnsafeLegacyKeyMagic = {'S', 'B', 'K', '1'};
inline constexpr u32 kOffsetMagic = 0;
inline constexpr u32 kOffsetHeaderBytes = 8;
inline constexpr u32 kOffsetCellCount = 12;
inline constexpr u32 kOffsetBodyBytes = 16;
inline constexpr u32 kOffsetBodyChecksum = 24;
inline constexpr u32 kOffsetIndexUuid = 32;
inline constexpr u32 kOffsetPageKind = 48;
inline constexpr u32 kOffsetTreeLevel = 50;
inline constexpr u32 kOffsetLeftSibling = 56;
inline constexpr u32 kOffsetRightSibling = 64;
inline constexpr u32 kOffsetParentPage = 72;
inline constexpr u32 kOffsetPageNumber = 80;
inline constexpr u32 kOffsetFreeSpaceBytes = 88;

inline constexpr u32 kCellHeaderBytes = 64;
inline constexpr u32 kCellOffsetFlags = 0;
inline constexpr u32 kCellOffsetKeyOrdinal = 2;
inline constexpr u32 kCellOffsetPayloadBytes = 4;
inline constexpr u32 kCellOffsetPayloadChecksum = 8;
inline constexpr u32 kCellOffsetChildPageNumber = 16;
inline constexpr u32 kCellOffsetRowUuid = 24;
inline constexpr u32 kCellOffsetVersionUuid = 40;

namespace CellFlag {
inline constexpr u16 high_key = 1u << 0;
inline constexpr u16 deleted = 1u << 1;
inline constexpr u16 encoded_key = 1u << 2;
}  // namespace CellFlag

Status IndexPageOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::storage_page};
}

Status IndexPageErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::storage_page};
}

IndexBtreePageBodyResult IndexPageError(std::string diagnostic_code,
                                        std::string message_key,
                                        std::string detail = {}) {
  IndexBtreePageBodyResult result;
  result.status = IndexPageErrorStatus();
  result.diagnostic = MakeIndexBtreePageDiagnostic(result.status,
                                                   std::move(diagnostic_code),
                                                   std::move(message_key),
                                                   std::move(detail));
  return result;
}

u64 Fnv1a64(const byte* data, std::size_t size) {
  u64 hash = 1469598103934665603ull;
  for (std::size_t i = 0; i < size; ++i) {
    hash ^= static_cast<u64>(data[i]);
    hash *= 1099511628211ull;
  }
  return hash;
}

bool IsTypedEngineIdentity(const TypedUuid& uuid, UuidKind kind) {
  return uuid.kind == kind && uuid.valid() && IsEngineIdentityUuid(uuid.value);
}

bool IsNilUuid(const Uuid& uuid) {
  return uuid.is_nil();
}

u16 FlagsFor(const IndexBtreeCell& cell) {
  u16 flags = 0;
  if (cell.high_key) {
    flags |= CellFlag::high_key;
  }
  if (cell.deleted) {
    flags |= CellFlag::deleted;
  }
  if (!cell.encoded_key.empty()) {
    flags |= CellFlag::encoded_key;
  }
  return flags;
}

bool IsKnownPageKind(IndexBtreePageKind kind) {
  return kind == IndexBtreePageKind::root ||
         kind == IndexBtreePageKind::internal ||
         kind == IndexBtreePageKind::leaf;
}

bool IsInternalLike(const IndexBtreePageBody& body) {
  return body.page_kind == IndexBtreePageKind::internal ||
         (body.page_kind == IndexBtreePageKind::root && body.tree_level > 0);
}

bool IsLeafLike(const IndexBtreePageBody& body) {
  return body.page_kind == IndexBtreePageKind::leaf ||
         (body.page_kind == IndexBtreePageKind::root && body.tree_level == 0);
}

bool GeneratedRowUuid(const TypedUuid& uuid) {
  return IsTypedEngineIdentity(uuid, UuidKind::row);
}

template <std::size_t N>
bool StartsWithBytes(const std::vector<byte>& bytes, const std::array<byte, N>& magic) {
  return bytes.size() >= magic.size() &&
         std::equal(magic.begin(), magic.end(), bytes.begin());
}

bool IsOrderPreservingEncodedKey(const std::vector<byte>& bytes) {
  return StartsWithBytes(bytes, kOrderPreservingKeyMagic);
}

bool IsUnsafeLegacyEncodedKey(const std::vector<byte>& bytes) {
  return StartsWithBytes(bytes, kUnsafeLegacyKeyMagic);
}

int CompareUnsignedBytes(const std::vector<byte>& left,
                         const std::vector<byte>& right,
                         std::size_t offset) {
  const std::size_t left_size = left.size() > offset ? left.size() - offset : 0;
  const std::size_t right_size = right.size() > offset ? right.size() - offset : 0;
  const std::size_t count = std::min(left_size, right_size);
  for (std::size_t i = 0; i < count; ++i) {
    const byte l = left[offset + i];
    const byte r = right[offset + i];
    if (l < r) {
      return -1;
    }
    if (l > r) {
      return 1;
    }
  }
  return left_size < right_size ? -1 : (right_size < left_size ? 1 : 0);
}

IndexBtreePageBodyResult ValidateEncodedKey(const IndexBtreeCell& cell,
                                            std::size_t cell_index) {
  if (cell.encoded_key.empty()) {
    return IndexPageError("SB-INDEX-BTREE-PAGE-ENCODED-KEY-MISSING",
                          "storage.index_btree_page.encoded_key_missing",
                          std::to_string(cell_index));
  }
  if (IsUnsafeLegacyEncodedKey(cell.encoded_key)) {
    return IndexPageError("SB-INDEX-BTREE-PAGE-UNSAFE-LEGACY-KEY",
                          "storage.index_btree_page.unsafe_legacy_key_refused",
                          std::to_string(cell_index));
  }
  if (!IsOrderPreservingEncodedKey(cell.encoded_key)) {
    return IndexPageError("SB-INDEX-BTREE-PAGE-KEY-ENVELOPE-INVALID",
                          "storage.index_btree_page.key_envelope_invalid",
                          std::to_string(cell_index));
  }
  return {};
}

int CompareIndexBtreeCellsUnchecked(const IndexBtreeCell& left,
                                    const IndexBtreeCell& right) {
  const int key_compare = CompareUnsignedBytes(left.encoded_key,
                                               right.encoded_key,
                                               kOrderPreservingKeyMagic.size());
  if (key_compare != 0) {
    return key_compare;
  }
  const int row_compare = CompareUuid128(left.row_uuid.value, right.row_uuid.value);
  if (row_compare != 0) {
    return row_compare;
  }
  return CompareUuid128(left.version_uuid.value, right.version_uuid.value);
}

IndexBtreePageBodyResult ValidatePhysicalPageBody(const IndexBtreePageBody& body) {
  if (body.page_number == 0) {
    return IndexPageError("SB-INDEX-BTREE-PAGE-NUMBER-REQUIRED",
                          "storage.index_btree_page.page_number_required");
  }
  if (!IsKnownPageKind(body.page_kind)) {
    return IndexPageError("SB-INDEX-BTREE-PAGE-KIND-INVALID",
                          "storage.index_btree_page.kind_invalid",
                          std::to_string(static_cast<u16>(body.page_kind)));
  }
  if (!IsLeafLike(body) && !IsInternalLike(body)) {
    return IndexPageError("SB-INDEX-BTREE-PAGE-LEVEL-KIND-MISMATCH",
                          "storage.index_btree_page.level_kind_mismatch",
                          std::to_string(body.page_number));
  }

  for (std::size_t i = 0; i < body.cells.size(); ++i) {
    const IndexBtreeCell& cell = body.cells[i];
    const auto key_validation = ValidateEncodedKey(cell, i);
    if (!key_validation.ok()) {
      return key_validation;
    }
    if (!GeneratedRowUuid(cell.row_uuid)) {
      return IndexPageError("SB-INDEX-BTREE-PAGE-ROW-UUID-MUST-BE-V7",
                            "storage.index_btree_page.row_uuid_must_be_v7",
                            std::to_string(i));
    }
    if (!GeneratedRowUuid(cell.version_uuid)) {
      return IndexPageError("SB-INDEX-BTREE-PAGE-VERSION-UUID-MUST-BE-V7",
                            "storage.index_btree_page.version_uuid_must_be_v7",
                            std::to_string(i));
    }
    if (IsInternalLike(body)) {
      if (!cell.high_key || cell.child_page_number == 0) {
        return IndexPageError("SB-INDEX-BTREE-PAGE-INTERNAL-FENCE-INVALID",
                              "storage.index_btree_page.internal_fence_invalid",
                              std::to_string(i));
      }
    } else if (cell.high_key || cell.child_page_number != 0) {
      return IndexPageError("SB-INDEX-BTREE-PAGE-LEAF-CELL-INVALID",
                            "storage.index_btree_page.leaf_cell_invalid",
                            std::to_string(i));
    }
    if (i > 0 && CompareIndexBtreeCellsUnchecked(body.cells[i - 1], cell) > 0) {
      return IndexPageError("SB-INDEX-BTREE-PAGE-CELLS-UNSORTED",
                            "storage.index_btree_page.cells_unsorted",
                            std::to_string(i));
    }
  }

  return {};
}

std::vector<byte> PayloadForCell(const IndexBtreeCell& cell) {
  return cell.encoded_key;
}

}  // namespace

const char* IndexBtreePageKindName(IndexBtreePageKind kind) {
  switch (kind) {
    case IndexBtreePageKind::root: return "root";
    case IndexBtreePageKind::internal: return "internal";
    case IndexBtreePageKind::leaf: return "leaf";
    case IndexBtreePageKind::unknown: return "unknown";
  }
  return "unknown";
}

const char* IndexBtreePhysicalScanModeName(IndexBtreePhysicalScanMode mode) {
  switch (mode) {
    case IndexBtreePhysicalScanMode::point: return "point";
    case IndexBtreePhysicalScanMode::range: return "range";
    case IndexBtreePhysicalScanMode::prefix: return "prefix";
    case IndexBtreePhysicalScanMode::ordered: return "ordered";
  }
  return "unknown";
}

const char* IndexBtreePhysicalScanOrderingName(IndexBtreePhysicalScanOrdering ordering) {
  switch (ordering) {
    case IndexBtreePhysicalScanOrdering::forward: return "forward";
    case IndexBtreePhysicalScanOrdering::reverse: return "reverse";
  }
  return "unknown";
}

const char* IndexBtreePhysicalCorruptionClassName(
    IndexBtreePhysicalCorruptionClass corruption_class) {
  switch (corruption_class) {
    case IndexBtreePhysicalCorruptionClass::none: return "none";
    case IndexBtreePhysicalCorruptionClass::checksum: return "checksum";
    case IndexBtreePhysicalCorruptionClass::page: return "page";
    case IndexBtreePhysicalCorruptionClass::parent: return "parent";
    case IndexBtreePhysicalCorruptionClass::fence: return "fence";
    case IndexBtreePhysicalCorruptionClass::sibling: return "sibling";
    case IndexBtreePhysicalCorruptionClass::order: return "order";
    case IndexBtreePhysicalCorruptionClass::duplicate: return "duplicate";
    case IndexBtreePhysicalCorruptionClass::orphan_stale_page_image:
      return "orphan_stale_page_image";
    case IndexBtreePhysicalCorruptionClass::tree: return "tree";
    case IndexBtreePhysicalCorruptionClass::unknown: return "unknown";
  }
  return "unknown";
}

u64 ComputeIndexBtreePageChecksum(const std::vector<byte>& body) {
  std::vector<byte> normalized = body;
  if (normalized.size() >= kOffsetBodyChecksum + sizeof(u64)) {
    StoreLittle64(normalized.data() + kOffsetBodyChecksum, 0);
  }
  return Fnv1a64(normalized.data(), normalized.size());
}

IndexBtreePageBodyResult BuildIndexBtreePageBody(const IndexBtreePageBody& body, u32 page_size) {
  if (page_size <= kPageHeaderSerializedBytes + kIndexBtreePageBodyHeaderBytes) {
    return IndexPageError("SB-INDEX-BTREE-PAGE-SIZE-TOO-SMALL",
                          "storage.index_btree_page.page_size_too_small",
                          std::to_string(page_size));
  }
  if (!IsTypedEngineIdentity(body.index_uuid, UuidKind::object)) {
    return IndexPageError("SB-INDEX-BTREE-PAGE-INDEX-UUID-MUST-BE-V7",
                          "storage.index_btree_page.index_uuid_must_be_v7");
  }
  const auto page_validation = ValidatePhysicalPageBody(body);
  if (!page_validation.ok()) {
    return page_validation;
  }

  std::vector<std::vector<byte>> encoded_keys;
  u32 body_bytes = kIndexBtreePageBodyHeaderBytes;
  for (const IndexBtreeCell& cell : body.cells) {
    const auto payload = PayloadForCell(cell);
    body_bytes += kCellHeaderBytes + static_cast<u32>(payload.size());
    encoded_keys.push_back(payload);
  }
  if (body_bytes > page_size - kPageHeaderSerializedBytes) {
    return IndexPageError("SB-INDEX-BTREE-PAGE-BODY-TOO-LARGE",
                          "storage.index_btree_page.body_too_large",
                          std::to_string(body_bytes));
  }
  const u32 free_space_bytes = page_size - kPageHeaderSerializedBytes - body_bytes;

  IndexBtreePageBodyResult result;
  result.status = IndexPageOkStatus();
  result.body = body;
  result.body.free_space_bytes = free_space_bytes;
  result.serialized.assign(page_size - kPageHeaderSerializedBytes, 0);
  std::copy(kIndexBtreeMagic.begin(), kIndexBtreeMagic.end(), result.serialized.begin() + kOffsetMagic);
  StoreLittle32(result.serialized.data() + kOffsetHeaderBytes, kIndexBtreePageBodyHeaderBytes);
  StoreLittle32(result.serialized.data() + kOffsetCellCount, static_cast<u32>(body.cells.size()));
  std::copy(body.index_uuid.value.bytes.begin(), body.index_uuid.value.bytes.end(), result.serialized.begin() + kOffsetIndexUuid);
  StoreLittle16(result.serialized.data() + kOffsetPageKind, static_cast<u16>(body.page_kind));
  StoreLittle16(result.serialized.data() + kOffsetTreeLevel, body.tree_level);
  StoreLittle64(result.serialized.data() + kOffsetLeftSibling, body.left_sibling_page_number);
  StoreLittle64(result.serialized.data() + kOffsetRightSibling, body.right_sibling_page_number);
  StoreLittle64(result.serialized.data() + kOffsetParentPage, body.parent_page_number);
  StoreLittle64(result.serialized.data() + kOffsetPageNumber, body.page_number);
  StoreLittle32(result.serialized.data() + kOffsetFreeSpaceBytes, free_space_bytes);

  u32 offset = kIndexBtreePageBodyHeaderBytes;
  for (std::size_t i = 0; i < body.cells.size(); ++i) {
    const IndexBtreeCell& cell = body.cells[i];
    const std::vector<byte>& encoded = encoded_keys[i];
    StoreLittle16(result.serialized.data() + offset + kCellOffsetFlags, FlagsFor(cell));
    StoreLittle16(result.serialized.data() + offset + kCellOffsetKeyOrdinal, cell.key_ordinal);
    StoreLittle32(result.serialized.data() + offset + kCellOffsetPayloadBytes, static_cast<u32>(encoded.size()));
    StoreLittle64(result.serialized.data() + offset + kCellOffsetPayloadChecksum, Fnv1a64(encoded.data(), encoded.size()));
    StoreLittle64(result.serialized.data() + offset + kCellOffsetChildPageNumber, cell.child_page_number);
    if (cell.row_uuid.valid()) {
      std::copy(cell.row_uuid.value.bytes.begin(), cell.row_uuid.value.bytes.end(), result.serialized.begin() + offset + kCellOffsetRowUuid);
    }
    if (cell.version_uuid.valid()) {
      std::copy(cell.version_uuid.value.bytes.begin(),
                cell.version_uuid.value.bytes.end(),
                result.serialized.begin() + offset + kCellOffsetVersionUuid);
    }
    offset += kCellHeaderBytes;
    std::copy(encoded.begin(), encoded.end(), result.serialized.begin() + offset);
    offset += static_cast<u32>(encoded.size());
  }

  StoreLittle32(result.serialized.data() + kOffsetBodyBytes, offset);
  StoreLittle64(result.serialized.data() + kOffsetBodyChecksum, ComputeIndexBtreePageChecksum(result.serialized));
  return result;
}

IndexBtreePageBodyResult ParseIndexBtreePageBody(const std::vector<byte>& serialized, u64 page_number) {
  if (serialized.size() < kIndexBtreePageBodyHeaderBytes) {
    return IndexPageError("SB-INDEX-BTREE-PAGE-BODY-SHORT",
                          "storage.index_btree_page.body_short",
                          std::to_string(page_number));
  }
  if (!std::equal(kIndexBtreeMagic.begin(), kIndexBtreeMagic.end(), serialized.begin() + kOffsetMagic)) {
    return IndexPageError("SB-INDEX-BTREE-PAGE-MAGIC-INVALID",
                          "storage.index_btree_page.magic_invalid",
                          std::to_string(page_number));
  }
  if (LoadLittle32(serialized.data() + kOffsetHeaderBytes) != kIndexBtreePageBodyHeaderBytes) {
    return IndexPageError("SB-INDEX-BTREE-PAGE-HEADER-SIZE-INVALID",
                          "storage.index_btree_page.header_size_invalid",
                          std::to_string(page_number));
  }
  if (LoadLittle64(serialized.data() + kOffsetBodyChecksum) != ComputeIndexBtreePageChecksum(serialized)) {
    return IndexPageError("SB-INDEX-BTREE-PAGE-CHECKSUM-MISMATCH",
                          "storage.index_btree_page.checksum_mismatch",
                          std::to_string(page_number));
  }
  const u32 cell_count = LoadLittle32(serialized.data() + kOffsetCellCount);
  const u32 body_bytes = LoadLittle32(serialized.data() + kOffsetBodyBytes);
  if (body_bytes > serialized.size() || body_bytes < kIndexBtreePageBodyHeaderBytes) {
    return IndexPageError("SB-INDEX-BTREE-PAGE-BODY-SIZE-INVALID",
                          "storage.index_btree_page.body_size_invalid",
                          std::to_string(page_number));
  }
  const u32 free_space_bytes = LoadLittle32(serialized.data() + kOffsetFreeSpaceBytes);
  if (free_space_bytes != serialized.size() - body_bytes) {
    return IndexPageError("SB-INDEX-BTREE-PAGE-FREE-SPACE-INVALID",
                          "storage.index_btree_page.free_space_invalid",
                          std::to_string(page_number));
  }

  IndexBtreePageBodyResult result;
  result.status = IndexPageOkStatus();
  result.body.page_number = LoadLittle64(serialized.data() + kOffsetPageNumber);
  if (result.body.page_number == 0) {
    result.body.page_number = page_number;
  }
  if (result.body.page_number != page_number) {
    return IndexPageError("SB-INDEX-BTREE-PAGE-NUMBER-MISMATCH",
                          "storage.index_btree_page.page_number_mismatch",
                          std::to_string(page_number));
  }
  result.body.index_uuid.kind = UuidKind::object;
  std::copy(serialized.begin() + kOffsetIndexUuid,
            serialized.begin() + kOffsetIndexUuid + 16,
            result.body.index_uuid.value.bytes.begin());
  if (!IsTypedEngineIdentity(result.body.index_uuid, UuidKind::object)) {
    return IndexPageError("SB-INDEX-BTREE-PAGE-INDEX-UUID-MUST-BE-V7",
                          "storage.index_btree_page.index_uuid_must_be_v7",
                          std::to_string(page_number));
  }
  result.body.page_kind = static_cast<IndexBtreePageKind>(LoadLittle16(serialized.data() + kOffsetPageKind));
  if (!IsKnownPageKind(result.body.page_kind)) {
    return IndexPageError("SB-INDEX-BTREE-PAGE-KIND-INVALID",
                          "storage.index_btree_page.kind_invalid",
                          std::to_string(page_number));
  }
  result.body.tree_level = LoadLittle16(serialized.data() + kOffsetTreeLevel);
  result.body.left_sibling_page_number = LoadLittle64(serialized.data() + kOffsetLeftSibling);
  result.body.right_sibling_page_number = LoadLittle64(serialized.data() + kOffsetRightSibling);
  result.body.parent_page_number = LoadLittle64(serialized.data() + kOffsetParentPage);
  result.body.free_space_bytes = free_space_bytes;
  result.serialized = serialized;

  u32 offset = kIndexBtreePageBodyHeaderBytes;
  for (u32 cell_index = 0; cell_index < cell_count; ++cell_index) {
    if (offset + kCellHeaderBytes > body_bytes) {
      return IndexPageError("SB-INDEX-BTREE-PAGE-CELL-SHORT",
                            "storage.index_btree_page.cell_short",
                            std::to_string(cell_index));
    }
    IndexBtreeCell cell;
    const u16 flags = LoadLittle16(serialized.data() + offset + kCellOffsetFlags);
    cell.high_key = (flags & CellFlag::high_key) != 0;
    cell.deleted = (flags & CellFlag::deleted) != 0;
    cell.key_ordinal = LoadLittle16(serialized.data() + offset + kCellOffsetKeyOrdinal);
    const u32 payload_bytes = LoadLittle32(serialized.data() + offset + kCellOffsetPayloadBytes);
    const u64 payload_checksum = LoadLittle64(serialized.data() + offset + kCellOffsetPayloadChecksum);
    cell.child_page_number = LoadLittle64(serialized.data() + offset + kCellOffsetChildPageNumber);
    Uuid row_uuid;
    std::copy(serialized.begin() + offset + kCellOffsetRowUuid,
              serialized.begin() + offset + kCellOffsetRowUuid + 16,
              row_uuid.bytes.begin());
    if (!IsNilUuid(row_uuid)) {
      cell.row_uuid.kind = UuidKind::row;
      cell.row_uuid.value = row_uuid;
      if (!IsTypedEngineIdentity(cell.row_uuid, UuidKind::row)) {
        return IndexPageError("SB-INDEX-BTREE-PAGE-ROW-UUID-MUST-BE-V7",
                              "storage.index_btree_page.row_uuid_must_be_v7",
                              std::to_string(cell_index));
      }
    }
    Uuid version_uuid;
    std::copy(serialized.begin() + offset + kCellOffsetVersionUuid,
              serialized.begin() + offset + kCellOffsetVersionUuid + 16,
              version_uuid.bytes.begin());
    if (!IsNilUuid(version_uuid)) {
      cell.version_uuid.kind = UuidKind::row;
      cell.version_uuid.value = version_uuid;
      if (!IsTypedEngineIdentity(cell.version_uuid, UuidKind::row)) {
        return IndexPageError("SB-INDEX-BTREE-PAGE-VERSION-UUID-MUST-BE-V7",
                              "storage.index_btree_page.version_uuid_must_be_v7",
                              std::to_string(cell_index));
      }
    }

    offset += kCellHeaderBytes;
    if (offset + payload_bytes > body_bytes) {
      return IndexPageError("SB-INDEX-BTREE-PAGE-PAYLOAD-SHORT",
                            "storage.index_btree_page.payload_short",
                            std::to_string(cell_index));
    }
    std::vector<byte> encoded(serialized.begin() + offset, serialized.begin() + offset + payload_bytes);
    if (payload_checksum != Fnv1a64(encoded.data(), encoded.size())) {
      return IndexPageError("SB-INDEX-BTREE-PAGE-PAYLOAD-CHECKSUM-MISMATCH",
                            "storage.index_btree_page.payload_checksum_mismatch",
                            std::to_string(cell_index));
    }
    if ((flags & CellFlag::encoded_key) != 0) {
      cell.encoded_key = std::move(encoded);
      const auto key_validation = ValidateEncodedKey(cell, cell_index);
      if (!key_validation.ok()) {
        return key_validation;
      }
    } else {
      const auto decoded = DecodeDatatypeBinaryValue(encoded);
      if (!decoded.ok()) {
        IndexBtreePageBodyResult decoded_result;
        decoded_result.status = decoded.status;
        decoded_result.diagnostic = decoded.diagnostic;
        return decoded_result;
      }
      cell.key_value = decoded.value;
    }
    result.body.cells.push_back(std::move(cell));
    offset += payload_bytes;
  }
  if (offset != body_bytes) {
    return IndexPageError("SB-INDEX-BTREE-PAGE-BODY-TRAILING-BYTES",
                          "storage.index_btree_page.body_trailing_bytes",
                          std::to_string(page_number));
  }
  const auto page_validation = ValidatePhysicalPageBody(result.body);
  if (!page_validation.ok()) {
    return page_validation;
  }
  return result;
}

namespace {

IndexBtreePhysicalTreeResult PhysicalTreeError(std::string diagnostic_code,
                                               std::string message_key,
                                               std::string detail = {}) {
  IndexBtreePhysicalTreeResult result;
  result.status = IndexPageErrorStatus();
  result.diagnostic = MakeIndexBtreePageDiagnostic(result.status,
                                                   std::move(diagnostic_code),
                                                   std::move(message_key),
                                                   std::move(detail));
  return result;
}

IndexBtreePhysicalInsertResult PhysicalInsertError(std::string diagnostic_code,
                                                   std::string message_key,
                                                   std::string detail = {}) {
  IndexBtreePhysicalInsertResult result;
  result.status = IndexPageErrorStatus();
  result.diagnostic = MakeIndexBtreePageDiagnostic(result.status,
                                                   std::move(diagnostic_code),
                                                   std::move(message_key),
                                                   std::move(detail));
  return result;
}

IndexBtreePhysicalInsertResult FromPageError(const IndexBtreePageBodyResult& page_result) {
  IndexBtreePhysicalInsertResult result;
  result.status = page_result.status;
  result.diagnostic = page_result.diagnostic;
  return result;
}

IndexBtreePhysicalDeleteResult PhysicalDeleteError(std::string diagnostic_code,
                                                   std::string message_key,
                                                   std::string detail = {}) {
  IndexBtreePhysicalDeleteResult result;
  result.status = IndexPageErrorStatus();
  result.diagnostic = MakeIndexBtreePageDiagnostic(result.status,
                                                   std::move(diagnostic_code),
                                                   std::move(message_key),
                                                   std::move(detail));
  return result;
}

IndexBtreePhysicalDeleteResult FromPageDeleteError(const IndexBtreePageBodyResult& page_result) {
  IndexBtreePhysicalDeleteResult result;
  result.status = page_result.status;
  result.diagnostic = page_result.diagnostic;
  return result;
}

IndexBtreePhysicalScanResult PhysicalScanError(std::string diagnostic_code,
                                               std::string message_key,
                                               std::string detail = {}) {
  IndexBtreePhysicalScanResult result;
  result.status = IndexPageErrorStatus();
  result.diagnostic = MakeIndexBtreePageDiagnostic(result.status,
                                                   std::move(diagnostic_code),
                                                   std::move(message_key),
                                                   std::move(detail));
  return result;
}

IndexBtreePhysicalScanResult FromPageScanError(const IndexBtreePageBodyResult& page_result) {
  IndexBtreePhysicalScanResult result;
  result.status = page_result.status;
  result.diagnostic = page_result.diagnostic;
  return result;
}

IndexBtreePhysicalUniqueInsertResult PhysicalUniqueInsertError(
    std::string diagnostic_code,
    std::string message_key,
    std::string detail = {}) {
  IndexBtreePhysicalUniqueInsertResult result;
  result.status = IndexPageErrorStatus();
  result.diagnostic = MakeIndexBtreePageDiagnostic(result.status,
                                                   std::move(diagnostic_code),
                                                   std::move(message_key),
                                                   std::move(detail));
  result.evidence = {"unique_atomic_probe_insert=false",
                     "latch_authority=structural_only",
                     "visibility_authority=false",
                     "authorization_authority=false",
                     "transaction_finality_authority=false",
                     "recovery_authority=false"};
  return result;
}

IndexBtreePhysicalUniqueInsertResult FromPageUniqueInsertError(
    const IndexBtreePageBodyResult& page_result) {
  IndexBtreePhysicalUniqueInsertResult result;
  result.status = page_result.status;
  result.diagnostic = page_result.diagnostic;
  result.evidence = {"unique_atomic_probe_insert=false",
                     "latch_authority=structural_only",
                     "visibility_authority=false",
                     "authorization_authority=false",
                     "transaction_finality_authority=false",
                     "recovery_authority=false"};
  return result;
}

IndexBtreePhysicalUniqueInsertResult FromPhysicalInsertError(
    const IndexBtreePhysicalInsertResult& insert_result) {
  IndexBtreePhysicalUniqueInsertResult result;
  result.status = insert_result.status;
  result.diagnostic = insert_result.diagnostic;
  result.insert_result = insert_result;
  result.evidence = insert_result.evidence;
  result.evidence.push_back("unique_atomic_probe_insert=false");
  result.evidence.push_back("visibility_authority=false");
  result.evidence.push_back("authorization_authority=false");
  result.evidence.push_back("transaction_finality_authority=false");
  result.evidence.push_back("recovery_authority=false");
  return result;
}

IndexBtreePhysicalTreeValidationResult PhysicalValidationError(
    std::string diagnostic_code,
    std::string message_key,
    std::string detail = {}) {
  IndexBtreePhysicalTreeValidationResult result;
  result.status = IndexPageErrorStatus();
  result.diagnostic = MakeIndexBtreePageDiagnostic(result.status,
                                                   std::move(diagnostic_code),
                                                   std::move(message_key),
                                                   std::move(detail));
  result.evidence.push_back("structural_validation=false");
  result.evidence.push_back("visibility_authority=false");
  result.evidence.push_back("authorization_authority=false");
  result.evidence.push_back("transaction_finality_authority=false");
  result.evidence.push_back("recovery_authority=false");
  return result;
}

IndexBtreePhysicalTreeImageResult PhysicalImageError(std::string diagnostic_code,
                                                     std::string message_key,
                                                     std::string detail = {}) {
  IndexBtreePhysicalTreeImageResult result;
  result.status = IndexPageErrorStatus();
  result.diagnostic = MakeIndexBtreePageDiagnostic(result.status,
                                                   std::move(diagnostic_code),
                                                   std::move(message_key),
                                                   std::move(detail));
  result.evidence.push_back("serialized_tree_image_exported=false");
  result.evidence.push_back("visibility_authority=false");
  result.evidence.push_back("authorization_authority=false");
  result.evidence.push_back("transaction_finality_authority=false");
  result.evidence.push_back("recovery_authority=false");
  return result;
}

IndexBtreePhysicalTreeReportResult PhysicalReportError(std::string diagnostic_code,
                                                       std::string message_key,
                                                       std::string detail = {}) {
  IndexBtreePhysicalTreeReportResult result;
  result.status = IndexPageErrorStatus();
  result.diagnostic = MakeIndexBtreePageDiagnostic(result.status,
                                                   std::move(diagnostic_code),
                                                   std::move(message_key),
                                                   std::move(detail));
  result.report.status = result.status;
  result.report.diagnostic = result.diagnostic;
  result.report.corruption_class = IndexBtreePhysicalCorruptionClass::unknown;
  result.report.exact_diagnostic_code = result.diagnostic.diagnostic_code;
  result.report.exact_diagnostic_message_key = result.diagnostic.message_key;
  result.report.evidence = {"structural_report=false",
                            "visibility=false",
                            "authorization=false",
                            "transaction_finality=false",
                            "recovery=false",
                            "visibility_authority=false",
                            "authorization_authority=false",
                            "transaction_finality_authority=false",
                            "recovery_authority=false"};
  result.evidence = result.report.evidence;
  return result;
}

IndexBtreePhysicalTreeRebuildResult PhysicalRebuildError(std::string diagnostic_code,
                                                         std::string message_key,
                                                         std::string detail = {}) {
  IndexBtreePhysicalTreeRebuildResult result;
  result.status = IndexPageErrorStatus();
  result.diagnostic = MakeIndexBtreePageDiagnostic(result.status,
                                                   std::move(diagnostic_code),
                                                   std::move(message_key),
                                                   std::move(detail));
  result.evidence = {"structural_rebuild=false",
                     "visibility=false",
                     "authorization=false",
                     "transaction_finality=false",
                     "recovery=false",
                     "visibility_authority=false",
                     "authorization_authority=false",
                     "transaction_finality_authority=false",
                     "recovery_authority=false"};
  return result;
}

IndexBtreePhysicalBulkBuildResult PhysicalBulkBuildError(
    std::string diagnostic_code,
    std::string message_key,
    std::string detail = {}) {
  IndexBtreePhysicalBulkBuildResult result;
  result.status = IndexPageErrorStatus();
  result.diagnostic = MakeIndexBtreePageDiagnostic(result.status,
                                                   std::move(diagnostic_code),
                                                   std::move(message_key),
                                                   std::move(detail));
  result.evidence = {"physical_leaf_pack=false",
                     "branch_levels_built=false",
                     "fence_keys_stored=false",
                     "candidate_root_generation_created=false",
                     "root_publish_authorized=false",
                     "physical_append_authorized=false",
                     "visibility_authority=false",
                     "authorization_authority=false",
                     "transaction_finality_authority=false",
                     "recovery_authority=false"};
  return result;
}

IndexBtreePhysicalTreeRepairResult PhysicalRepairError(
    std::string diagnostic_code,
    std::string message_key,
    IndexBtreePhysicalCorruptionClass corruption_class,
    std::string detail = {}) {
  IndexBtreePhysicalTreeRepairResult result;
  result.status = IndexPageErrorStatus();
  result.diagnostic = MakeIndexBtreePageDiagnostic(result.status,
                                                   std::move(diagnostic_code),
                                                   std::move(message_key),
                                                   std::move(detail));
  result.refused = true;
  result.corruption_class = corruption_class;
  result.evidence = {"structural_repair=false",
                     std::string("corruption_class=") +
                         IndexBtreePhysicalCorruptionClassName(corruption_class),
                     "repair_refused=true",
                     "visibility=false",
                     "authorization=false",
                     "transaction_finality=false",
                     "recovery=false",
                     "visibility_authority=false",
                     "authorization_authority=false",
                     "transaction_finality_authority=false",
                     "recovery_authority=false"};
  return result;
}

std::shared_ptr<std::shared_mutex> LatchForTree(const IndexBtreePhysicalTree* tree) {
  static std::mutex table_mutex;
  static std::unordered_map<const void*, std::weak_ptr<std::shared_mutex>> latch_table;
  std::lock_guard<std::mutex> guard(table_mutex);
  auto& weak_latch = latch_table[tree];
  auto latch = weak_latch.lock();
  if (!latch) {
    latch = std::make_shared<std::shared_mutex>();
    weak_latch = latch;
  }
  return latch;
}

struct SharedTreeLatch {
  explicit SharedTreeLatch(const IndexBtreePhysicalTree* tree)
      : latch(LatchForTree(tree)), lock(*latch) {}

  std::shared_ptr<std::shared_mutex> latch;
  std::shared_lock<std::shared_mutex> lock;
};

struct UniqueTreeLatch {
  explicit UniqueTreeLatch(const IndexBtreePhysicalTree* tree)
      : latch(LatchForTree(tree)), lock(*latch) {}

  std::shared_ptr<std::shared_mutex> latch;
  std::unique_lock<std::shared_mutex> lock;
};

IndexBtreePageBodyResult FetchIndexBtreePhysicalPageUnlocked(
    const IndexBtreePhysicalTree& tree,
    u64 page_number);
IndexBtreePhysicalTreeValidationResult ValidateIndexBtreePhysicalTreeUnlocked(
    const IndexBtreePhysicalTree& tree);
IndexBtreePhysicalTreeReport BuildIndexBtreePhysicalTreeReportUnlocked(
    const IndexBtreePhysicalTree& tree);
IndexBtreePhysicalTreeRebuildResult RebuildIndexBtreePhysicalTreeFromLiveCells(
    const IndexBtreePhysicalTree& source,
    const std::vector<IndexBtreeCell>& live_entries,
    const char* reason);
IndexBtreePhysicalTreeValidationResult CollectRepairLiveEntriesUnlocked(
    const IndexBtreePhysicalTree& tree,
    std::set<u64>* reachable_pages,
    std::vector<IndexBtreeCell>* live_entries,
    u64* tombstone_count);
bool SameLiveEntry(const IndexBtreeCell& left, const IndexBtreeCell& right);

std::optional<std::size_t> FindPageImageIndex(const IndexBtreePhysicalTree& tree,
                                              u64 page_number) {
  for (std::size_t i = 0; i < tree.pages.size(); ++i) {
    if (tree.pages[i].page_number == page_number) {
      return i;
    }
  }
  return std::nullopt;
}

IndexBtreePageBodyResult FetchIndexBtreePhysicalPageUnlocked(
    const IndexBtreePhysicalTree& tree,
    u64 page_number) {
  const auto index = FindPageImageIndex(tree, page_number);
  if (!index.has_value()) {
    return IndexPageError("SB-INDEX-BTREE-PHYSICAL-PAGE-NOT-FOUND",
                          "storage.index_btree_physical.page_not_found",
                          std::to_string(page_number));
  }
  return ParseIndexBtreePageBody(tree.pages[*index].serialized, page_number);
}

IndexBtreePageBodyResult StagePhysicalPage(const IndexBtreePageBody& body,
                                           u32 page_size) {
  auto built = BuildIndexBtreePageBody(body, page_size);
  if (!built.ok()) {
    return built;
  }
  auto parsed = ParseIndexBtreePageBody(built.serialized, body.page_number);
  if (!parsed.ok()) {
    return parsed;
  }
  parsed.serialized = std::move(built.serialized);
  return parsed;
}

void PublishStagedPage(IndexBtreePhysicalTree* tree,
                       const IndexBtreePageBodyResult& staged) {
  const IndexBtreePhysicalPageImage image{staged.body.page_number, staged.serialized};
  const auto existing = FindPageImageIndex(*tree, image.page_number);
  if (existing.has_value()) {
    tree->pages[*existing] = image;
  } else {
    tree->pages.push_back(image);
  }
}

std::optional<IndexBtreeCell> LastLiveCell(const IndexBtreePageBody& page) {
  for (auto it = page.cells.rbegin(); it != page.cells.rend(); ++it) {
    if (!it->deleted) {
      return *it;
    }
  }
  return std::nullopt;
}

std::optional<IndexBtreeCell> FirstLiveCell(const IndexBtreePageBody& page) {
  for (const IndexBtreeCell& cell : page.cells) {
    if (!cell.deleted) {
      return cell;
    }
  }
  return std::nullopt;
}

IndexBtreeCell FenceCellForPage(const IndexBtreePageBody& child) {
  const auto live_cell = LastLiveCell(child);
  IndexBtreeCell fence = live_cell.has_value() ? *live_cell : IndexBtreeCell{};
  fence.child_page_number = child.page_number;
  fence.high_key = true;
  fence.deleted = false;
  return fence;
}

void InsertCellSorted(std::vector<IndexBtreeCell>* cells, IndexBtreeCell cell) {
  const auto position = std::lower_bound(
      cells->begin(),
      cells->end(),
      cell,
      [](const IndexBtreeCell& left, const IndexBtreeCell& right) {
        return CompareIndexBtreeCellsUnchecked(left, right) < 0;
      });
  cells->insert(position, std::move(cell));
}

u64 SelectChildPage(const IndexBtreePageBody& parent, const IndexBtreeCell& key) {
  for (const IndexBtreeCell& fence : parent.cells) {
    if (CompareIndexBtreeCellsUnchecked(key, fence) <= 0) {
      return fence.child_page_number;
    }
  }
  return parent.cells.empty() ? 0 : parent.cells.back().child_page_number;
}

void AddCandidateEvidence(IndexBtreePhysicalInsertResult* result) {
  result->evidence.push_back("latch_authority=structural_only");
  result->evidence.push_back("visibility_authority=false");
  result->evidence.push_back("authorization_authority=false");
  result->evidence.push_back("transaction_finality_authority=false");
  result->evidence.push_back("recovery_authority=false");
}

void AddCandidateEvidence(IndexBtreePhysicalDeleteResult* result) {
  result->evidence.push_back("latch_authority=structural_only");
  result->evidence.push_back("visibility_authority=false");
  result->evidence.push_back("authorization_authority=false");
  result->evidence.push_back("transaction_finality_authority=false");
  result->evidence.push_back("recovery_authority=false");
}

void AddCandidateEvidence(IndexBtreePhysicalScanResult* result) {
  result->evidence.push_back("mga_recheck_required=true");
  result->evidence.push_back("security_recheck_required=true");
  result->evidence.push_back("latch_authority=structural_only");
  result->evidence.push_back("visibility_authority=false");
  result->evidence.push_back("authorization_authority=false");
  result->evidence.push_back("transaction_finality_authority=false");
  result->evidence.push_back("recovery_authority=false");
}

void AddCandidateEvidence(IndexBtreePhysicalUniqueInsertResult* result) {
  result->evidence.push_back("mga_recheck_required=true");
  result->evidence.push_back("security_recheck_required=true");
  result->evidence.push_back("latch_authority=structural_only");
  result->evidence.push_back("visibility_authority=false");
  result->evidence.push_back("authorization_authority=false");
  result->evidence.push_back("transaction_finality_authority=false");
  result->evidence.push_back("recovery_authority=false");
}

void SortCells(std::vector<IndexBtreeCell>* cells) {
  std::sort(cells->begin(),
            cells->end(),
            [](const IndexBtreeCell& left, const IndexBtreeCell& right) {
              return CompareIndexBtreeCellsUnchecked(left, right) < 0;
            });
}

std::vector<u64> ParentPath(std::vector<u64> path) {
  if (!path.empty()) {
    path.pop_back();
  }
  return path;
}

u32 RemoveDeletedCells(IndexBtreePageBody* page) {
  const auto before = page->cells.size();
  page->cells.erase(std::remove_if(page->cells.begin(),
                                   page->cells.end(),
                                   [](const IndexBtreeCell& cell) {
                                     return cell.deleted;
                                   }),
                    page->cells.end());
  return static_cast<u32>(before - page->cells.size());
}

std::size_t LiveCellCount(const IndexBtreePageBody& page) {
  return static_cast<std::size_t>(
      std::count_if(page.cells.begin(),
                    page.cells.end(),
                    [](const IndexBtreeCell& cell) {
                      return !cell.deleted;
                    }));
}

bool HasDeletedCells(const IndexBtreePageBody& page) {
  return LiveCellCount(page) != page.cells.size();
}

bool SameParentAndLevel(const IndexBtreePageBody& left,
                        const IndexBtreePageBody& right) {
  return left.parent_page_number == right.parent_page_number &&
         left.tree_level == right.tree_level &&
         left.page_kind == right.page_kind;
}

bool IsUnderfilledNonRoot(const IndexBtreePageBody& page) {
  return page.page_kind != IndexBtreePageKind::root && LiveCellCount(page) < 2;
}

IndexBtreePhysicalInsertResult StageChildParentUpdates(
    const IndexBtreePhysicalTree& tree,
    const std::vector<IndexBtreeCell>& fences,
    u64 parent_page_number,
    std::vector<IndexBtreePageBodyResult>* staged_pages) {
  for (const IndexBtreeCell& fence : fences) {
    auto child_fetch = FetchIndexBtreePhysicalPageUnlocked(tree, fence.child_page_number);
    if (!child_fetch.ok()) {
      return FromPageError(child_fetch);
    }
    IndexBtreePageBody child = std::move(child_fetch.body);
    child.parent_page_number = parent_page_number;
    auto staged_child = StagePhysicalPage(child, tree.page_size);
    if (!staged_child.ok()) {
      return FromPageError(staged_child);
    }
    staged_pages->push_back(std::move(staged_child));
  }
  IndexBtreePhysicalInsertResult result;
  result.status = IndexPageOkStatus();
  return result;
}

IndexBtreePhysicalInsertResult SplitInternalPage(IndexBtreePhysicalTree* tree,
                                                const std::vector<u64>& path_to_page,
                                                const IndexBtreePageBody& page,
                                                std::vector<IndexBtreeCell> cells,
                                                IndexBtreePhysicalInsertResult result);

IndexBtreePhysicalInsertResult UpdateParentFence(IndexBtreePhysicalTree* tree,
                                                 const std::vector<u64>& path,
                                                 const IndexBtreePageBody& child,
                                                 IndexBtreePhysicalInsertResult result) {
  if (path.empty()) {
    return result;
  }
  auto parent_fetch = FetchIndexBtreePhysicalPageUnlocked(*tree, path.back());
  if (!parent_fetch.ok()) {
    return FromPageError(parent_fetch);
  }
  IndexBtreePageBody parent = std::move(parent_fetch.body);
  bool replaced = false;
  for (IndexBtreeCell& fence : parent.cells) {
    if (fence.child_page_number == child.page_number) {
      fence = FenceCellForPage(child);
      replaced = true;
      break;
    }
  }
  if (!replaced) {
    return PhysicalInsertError("SB-INDEX-BTREE-PHYSICAL-PARENT-FENCE-MISSING",
                               "storage.index_btree_physical.parent_fence_missing",
                               std::to_string(child.page_number));
  }
  SortCells(&parent.cells);
  auto staged_parent = StagePhysicalPage(parent, tree->page_size);
  if (staged_parent.ok()) {
    PublishStagedPage(tree, staged_parent);
    result.evidence.push_back("parent_fence_updated=true");
    return UpdateParentFence(tree, ParentPath(path), parent, std::move(result));
  }
  if (staged_parent.diagnostic.diagnostic_code != "SB-INDEX-BTREE-PAGE-BODY-TOO-LARGE") {
    return FromPageError(staged_parent);
  }
  if (HasDeletedCells(parent)) {
    const u32 removed = RemoveDeletedCells(&parent);
    auto staged_compacted_parent = StagePhysicalPage(parent, tree->page_size);
    if (staged_compacted_parent.ok()) {
      PublishStagedPage(tree, staged_compacted_parent);
      result.evidence.push_back("internal_bottom_up_cleanup_before_split=true");
      result.evidence.push_back("internal_tombstone_cleanup_compacted=" +
                                std::to_string(removed));
      return UpdateParentFence(tree, ParentPath(path), parent, std::move(result));
    }
    if (staged_compacted_parent.diagnostic.diagnostic_code !=
        "SB-INDEX-BTREE-PAGE-BODY-TOO-LARGE") {
      return FromPageError(staged_compacted_parent);
    }
  }
  result.evidence.push_back("parent_fence_update_overflow=true");
  return SplitInternalPage(tree, path, parent, std::move(parent.cells), std::move(result));
}

IndexBtreePhysicalInsertResult PropagateSplit(IndexBtreePhysicalTree* tree,
                                              const std::vector<u64>& path,
                                              const IndexBtreePageBody& left,
                                              const IndexBtreePageBody& right,
                                              IndexBtreePhysicalInsertResult result) {
  if (path.empty()) {
    return PhysicalInsertError("SB-INDEX-BTREE-PHYSICAL-SPLIT-PARENT-MISSING",
                               "storage.index_btree_physical.split_parent_missing",
                               std::to_string(left.page_number));
  }
  auto parent_fetch = FetchIndexBtreePhysicalPageUnlocked(*tree, path.back());
  if (!parent_fetch.ok()) {
    return FromPageError(parent_fetch);
  }
  IndexBtreePageBody parent = std::move(parent_fetch.body);
  const auto old_size = parent.cells.size();
  parent.cells.erase(std::remove_if(parent.cells.begin(),
                                    parent.cells.end(),
                                    [&](const IndexBtreeCell& fence) {
                                      return fence.child_page_number == left.page_number;
                                    }),
                     parent.cells.end());
  if (parent.cells.size() + 1 != old_size) {
    return PhysicalInsertError("SB-INDEX-BTREE-PHYSICAL-PARENT-FENCE-MISSING",
                               "storage.index_btree_physical.parent_fence_missing",
                               std::to_string(left.page_number));
  }
  InsertCellSorted(&parent.cells, FenceCellForPage(left));
  InsertCellSorted(&parent.cells, FenceCellForPage(right));
  auto staged_parent = StagePhysicalPage(parent, tree->page_size);
  if (staged_parent.ok()) {
    PublishStagedPage(tree, staged_parent);
    result.evidence.push_back("parent_separator_updated=true");
    return UpdateParentFence(tree, ParentPath(path), parent, std::move(result));
  }
  if (staged_parent.diagnostic.diagnostic_code != "SB-INDEX-BTREE-PAGE-BODY-TOO-LARGE") {
    return FromPageError(staged_parent);
  }
  if (HasDeletedCells(parent)) {
    const u32 removed = RemoveDeletedCells(&parent);
    auto staged_compacted_parent = StagePhysicalPage(parent, tree->page_size);
    if (staged_compacted_parent.ok()) {
      PublishStagedPage(tree, staged_compacted_parent);
      result.evidence.push_back("internal_bottom_up_cleanup_before_split=true");
      result.evidence.push_back("internal_tombstone_cleanup_compacted=" +
                                std::to_string(removed));
      return UpdateParentFence(tree, ParentPath(path), parent, std::move(result));
    }
    if (staged_compacted_parent.diagnostic.diagnostic_code !=
        "SB-INDEX-BTREE-PAGE-BODY-TOO-LARGE") {
      return FromPageError(staged_compacted_parent);
    }
  }
  result.evidence.push_back("parent_separator_overflow=true");
  return SplitInternalPage(tree, path, parent, std::move(parent.cells), std::move(result));
}

IndexBtreePhysicalInsertResult SplitRootLeaf(IndexBtreePhysicalTree* tree,
                                             const IndexBtreePageBody& root,
                                             std::vector<IndexBtreeCell> cells) {
  const u64 left_page_number = tree->next_page_number++;
  const u64 right_page_number = tree->next_page_number++;
  const std::size_t split = cells.size() / 2;

  IndexBtreePageBody left;
  left.index_uuid = tree->index_uuid;
  left.page_number = left_page_number;
  left.parent_page_number = root.page_number;
  left.right_sibling_page_number = right_page_number;
  left.page_kind = IndexBtreePageKind::leaf;
  left.tree_level = 0;
  left.cells.assign(cells.begin(), cells.begin() + static_cast<std::ptrdiff_t>(split));

  IndexBtreePageBody right;
  right.index_uuid = tree->index_uuid;
  right.page_number = right_page_number;
  right.parent_page_number = root.page_number;
  right.left_sibling_page_number = left_page_number;
  right.page_kind = IndexBtreePageKind::leaf;
  right.tree_level = 0;
  right.cells.assign(cells.begin() + static_cast<std::ptrdiff_t>(split), cells.end());

  IndexBtreePageBody new_root = root;
  new_root.tree_level = 1;
  new_root.left_sibling_page_number = 0;
  new_root.right_sibling_page_number = 0;
  new_root.parent_page_number = 0;
  new_root.cells.clear();
  InsertCellSorted(&new_root.cells, FenceCellForPage(left));
  InsertCellSorted(&new_root.cells, FenceCellForPage(right));

  auto staged_left = StagePhysicalPage(left, tree->page_size);
  if (!staged_left.ok()) {
    return FromPageError(staged_left);
  }
  auto staged_right = StagePhysicalPage(right, tree->page_size);
  if (!staged_right.ok()) {
    return FromPageError(staged_right);
  }
  auto staged_root = StagePhysicalPage(new_root, tree->page_size);
  if (!staged_root.ok()) {
    return FromPageError(staged_root);
  }

  PublishStagedPage(tree, staged_left);
  PublishStagedPage(tree, staged_right);
  PublishStagedPage(tree, staged_root);

  IndexBtreePhysicalInsertResult result;
  result.status = IndexPageOkStatus();
  result.inserted = true;
  result.split_performed = true;
  result.root_split_performed = true;
  result.root_page_number = new_root.page_number;
  result.left_page_number = left.page_number;
  result.right_page_number = right.page_number;
  result.separator_cell = FenceCellForPage(left);
  result.evidence = {"root_split=true",
                     "root_page_stable=true",
                     "root_internal_children=2",
                     "exclusive_latch_acquired=true",
                     "optimistic_descent_validated=true",
                     "latch_authority=structural_only",
                     "visibility_authority=false",
                     "authorization_authority=false",
                     "transaction_finality_authority=false",
                     "recovery_authority=false"};
  return result;
}

IndexBtreePhysicalInsertResult SplitInternalPage(IndexBtreePhysicalTree* tree,
                                                const std::vector<u64>& path_to_page,
                                                const IndexBtreePageBody& page,
                                                std::vector<IndexBtreeCell> cells,
                                                IndexBtreePhysicalInsertResult result) {
  if (cells.size() < 2 || !IsInternalLike(page)) {
    return PhysicalInsertError("SB-INDEX-BTREE-PHYSICAL-INTERNAL-SPLIT-INVALID",
                               "storage.index_btree_physical.internal_split_invalid",
                               std::to_string(page.page_number));
  }
  SortCells(&cells);
  const std::size_t split = cells.size() / 2;

  if (page.page_kind == IndexBtreePageKind::root) {
    const u64 left_page_number = tree->next_page_number++;
    const u64 right_page_number = tree->next_page_number++;

    IndexBtreePageBody left;
    left.index_uuid = tree->index_uuid;
    left.page_number = left_page_number;
    left.parent_page_number = page.page_number;
    left.right_sibling_page_number = right_page_number;
    left.page_kind = IndexBtreePageKind::internal;
    left.tree_level = page.tree_level;
    left.cells.assign(cells.begin(), cells.begin() + static_cast<std::ptrdiff_t>(split));

    IndexBtreePageBody right;
    right.index_uuid = tree->index_uuid;
    right.page_number = right_page_number;
    right.parent_page_number = page.page_number;
    right.left_sibling_page_number = left_page_number;
    right.page_kind = IndexBtreePageKind::internal;
    right.tree_level = page.tree_level;
    right.cells.assign(cells.begin() + static_cast<std::ptrdiff_t>(split), cells.end());

    IndexBtreePageBody new_root = page;
    new_root.parent_page_number = 0;
    new_root.left_sibling_page_number = 0;
    new_root.right_sibling_page_number = 0;
    new_root.tree_level = static_cast<u16>(page.tree_level + 1);
    new_root.cells.clear();
    InsertCellSorted(&new_root.cells, FenceCellForPage(left));
    InsertCellSorted(&new_root.cells, FenceCellForPage(right));

    std::vector<IndexBtreePageBodyResult> staged_child_parent_updates;
    auto child_update = StageChildParentUpdates(*tree,
                                                left.cells,
                                                left.page_number,
                                                &staged_child_parent_updates);
    if (!child_update.ok()) {
      return child_update;
    }
    child_update = StageChildParentUpdates(*tree,
                                           right.cells,
                                           right.page_number,
                                           &staged_child_parent_updates);
    if (!child_update.ok()) {
      return child_update;
    }
    auto staged_left = StagePhysicalPage(left, tree->page_size);
    if (!staged_left.ok()) {
      return FromPageError(staged_left);
    }
    auto staged_right = StagePhysicalPage(right, tree->page_size);
    if (!staged_right.ok()) {
      return FromPageError(staged_right);
    }
    auto staged_root = StagePhysicalPage(new_root, tree->page_size);
    if (!staged_root.ok()) {
      return FromPageError(staged_root);
    }

    for (const auto& staged_child : staged_child_parent_updates) {
      PublishStagedPage(tree, staged_child);
    }
    PublishStagedPage(tree, staged_left);
    PublishStagedPage(tree, staged_right);
    PublishStagedPage(tree, staged_root);

    result.status = IndexPageOkStatus();
    result.inserted = true;
    result.split_performed = true;
    result.root_split_performed = true;
    result.root_page_number = new_root.page_number;
  result.left_page_number = left.page_number;
  result.right_page_number = right.page_number;
  result.separator_cell = FenceCellForPage(left);
  result.evidence.push_back("internal_root_split=true");
  result.evidence.push_back("root_page_stable=true");
  result.evidence.push_back("exclusive_latch_acquired=true");
  result.evidence.push_back("optimistic_descent_validated=true");
  AddCandidateEvidence(&result);
    return result;
  }

  if (path_to_page.empty() || path_to_page.back() != page.page_number) {
    return PhysicalInsertError("SB-INDEX-BTREE-PHYSICAL-INTERNAL-PATH-INVALID",
                               "storage.index_btree_physical.internal_path_invalid",
                               std::to_string(page.page_number));
  }

  const u64 right_page_number = tree->next_page_number++;
  IndexBtreePageBody left = page;
  left.cells.assign(cells.begin(), cells.begin() + static_cast<std::ptrdiff_t>(split));
  left.right_sibling_page_number = right_page_number;

  IndexBtreePageBody right;
  right.index_uuid = tree->index_uuid;
  right.page_number = right_page_number;
  right.parent_page_number = page.parent_page_number;
  right.left_sibling_page_number = page.page_number;
  right.right_sibling_page_number = page.right_sibling_page_number;
  right.page_kind = IndexBtreePageKind::internal;
  right.tree_level = page.tree_level;
  right.cells.assign(cells.begin() + static_cast<std::ptrdiff_t>(split), cells.end());

  std::optional<IndexBtreePageBody> old_right;
  if (page.right_sibling_page_number != 0) {
    auto old_right_fetch = FetchIndexBtreePhysicalPageUnlocked(*tree, page.right_sibling_page_number);
    if (!old_right_fetch.ok()) {
      return FromPageError(old_right_fetch);
    }
    old_right = std::move(old_right_fetch.body);
    old_right->left_sibling_page_number = right_page_number;
  }

  std::vector<IndexBtreePageBodyResult> staged_child_parent_updates;
  auto child_update = StageChildParentUpdates(*tree,
                                              left.cells,
                                              left.page_number,
                                              &staged_child_parent_updates);
  if (!child_update.ok()) {
    return child_update;
  }
  child_update = StageChildParentUpdates(*tree,
                                         right.cells,
                                         right.page_number,
                                         &staged_child_parent_updates);
  if (!child_update.ok()) {
    return child_update;
  }
  auto staged_left = StagePhysicalPage(left, tree->page_size);
  if (!staged_left.ok()) {
    return FromPageError(staged_left);
  }
  auto staged_right = StagePhysicalPage(right, tree->page_size);
  if (!staged_right.ok()) {
    return FromPageError(staged_right);
  }
  std::optional<IndexBtreePageBodyResult> staged_old_right;
  if (old_right.has_value()) {
    staged_old_right = StagePhysicalPage(*old_right, tree->page_size);
    if (!staged_old_right->ok()) {
      return FromPageError(*staged_old_right);
    }
  }

  for (const auto& staged_child : staged_child_parent_updates) {
    PublishStagedPage(tree, staged_child);
  }
  PublishStagedPage(tree, staged_left);
  PublishStagedPage(tree, staged_right);
  if (staged_old_right.has_value()) {
    PublishStagedPage(tree, *staged_old_right);
  }

  result.status = IndexPageOkStatus();
  result.inserted = true;
  result.split_performed = true;
  result.root_page_number = tree->root_page_number;
  result.left_page_number = left.page_number;
  result.right_page_number = right.page_number;
  result.separator_cell = FenceCellForPage(left);
  result.evidence.push_back("internal_split=true");
  result.evidence.push_back("exclusive_latch_acquired=true");
  result.evidence.push_back("optimistic_descent_validated=true");
  return PropagateSplit(tree, ParentPath(path_to_page), left, right, std::move(result));
}

IndexBtreePhysicalInsertResult SplitNonRootLeaf(IndexBtreePhysicalTree* tree,
                                                const std::vector<u64>& path,
                                                const IndexBtreePageBody& leaf,
                                                std::vector<IndexBtreeCell> cells) {
  const u64 right_page_number = tree->next_page_number++;
  const std::size_t split = cells.size() / 2;

  IndexBtreePageBody left = leaf;
  left.cells.assign(cells.begin(), cells.begin() + static_cast<std::ptrdiff_t>(split));
  left.right_sibling_page_number = right_page_number;

  IndexBtreePageBody right;
  right.index_uuid = tree->index_uuid;
  right.page_number = right_page_number;
  right.parent_page_number = leaf.parent_page_number;
  right.left_sibling_page_number = leaf.page_number;
  right.right_sibling_page_number = leaf.right_sibling_page_number;
  right.page_kind = IndexBtreePageKind::leaf;
  right.tree_level = 0;
  right.cells.assign(cells.begin() + static_cast<std::ptrdiff_t>(split), cells.end());

  std::optional<IndexBtreePageBody> old_right;
  if (leaf.right_sibling_page_number != 0) {
    auto old_right_fetch = FetchIndexBtreePhysicalPageUnlocked(*tree, leaf.right_sibling_page_number);
    if (!old_right_fetch.ok()) {
      return FromPageError(old_right_fetch);
    }
    old_right = std::move(old_right_fetch.body);
    old_right->left_sibling_page_number = right_page_number;
  }

  auto staged_left = StagePhysicalPage(left, tree->page_size);
  if (!staged_left.ok()) {
    return FromPageError(staged_left);
  }
  auto staged_right = StagePhysicalPage(right, tree->page_size);
  if (!staged_right.ok()) {
    return FromPageError(staged_right);
  }
  std::optional<IndexBtreePageBodyResult> staged_old_right;
  if (old_right.has_value()) {
    staged_old_right = StagePhysicalPage(*old_right, tree->page_size);
    if (!staged_old_right->ok()) {
      return FromPageError(*staged_old_right);
    }
  }

  PublishStagedPage(tree, staged_left);
  PublishStagedPage(tree, staged_right);
  if (staged_old_right.has_value()) {
    PublishStagedPage(tree, *staged_old_right);
  }

  IndexBtreePhysicalInsertResult result;
  result.status = IndexPageOkStatus();
  result.inserted = true;
  result.split_performed = true;
  result.root_page_number = tree->root_page_number;
  result.left_page_number = left.page_number;
  result.right_page_number = right.page_number;
  result.separator_cell = FenceCellForPage(left);
  result.evidence.push_back("leaf_split=true");
  result.evidence.push_back("exclusive_latch_acquired=true");
  result.evidence.push_back("optimistic_descent_validated=true");
  AddCandidateEvidence(&result);
  return PropagateSplit(tree, path, left, right, std::move(result));
}

bool IsExactLocatorCell(const IndexBtreeCell& left, const IndexBtreeCell& right) {
  return !left.deleted &&
         CompareIndexBtreeCellsUnchecked(left, right) == 0 &&
         left.row_uuid.value == right.row_uuid.value &&
         left.version_uuid.value == right.version_uuid.value;
}

std::optional<std::size_t> FindExactLiveCell(const IndexBtreePageBody& page,
                                             const IndexBtreeCell& cell) {
  for (std::size_t i = 0; i < page.cells.size(); ++i) {
    if (IsExactLocatorCell(page.cells[i], cell)) {
      return i;
    }
  }
  return std::nullopt;
}

IndexBtreePhysicalDeleteResult StagePageForDelete(IndexBtreePhysicalTree* tree,
                                                  const IndexBtreePageBody& page) {
  auto staged = StagePhysicalPage(page, tree->page_size);
  if (!staged.ok()) {
    return FromPageDeleteError(staged);
  }
  PublishStagedPage(tree, staged);
  IndexBtreePhysicalDeleteResult result;
  result.status = IndexPageOkStatus();
  return result;
}

IndexBtreePhysicalDeleteResult CollapseRootToOnlyChild(IndexBtreePhysicalTree* tree,
                                                       const IndexBtreePageBody& root,
                                                       IndexBtreePhysicalDeleteResult result) {
  if (root.page_kind != IndexBtreePageKind::root || root.cells.size() != 1) {
    return result;
  }
  const u64 only_child_page_number = root.cells.front().child_page_number;
  auto child_fetch = FetchIndexBtreePhysicalPageUnlocked(*tree, only_child_page_number);
  if (!child_fetch.ok()) {
    return FromPageDeleteError(child_fetch);
  }
  IndexBtreePageBody collapsed = std::move(child_fetch.body);
  collapsed.page_number = root.page_number;
  collapsed.parent_page_number = 0;
  collapsed.left_sibling_page_number = 0;
  collapsed.right_sibling_page_number = 0;
  collapsed.page_kind = IndexBtreePageKind::root;
  if (collapsed.tree_level > 0) {
    for (IndexBtreeCell& fence : collapsed.cells) {
      auto grandchild_fetch = FetchIndexBtreePhysicalPageUnlocked(*tree, fence.child_page_number);
      if (!grandchild_fetch.ok()) {
        return FromPageDeleteError(grandchild_fetch);
      }
      IndexBtreePageBody grandchild = std::move(grandchild_fetch.body);
      grandchild.parent_page_number = collapsed.page_number;
      auto staged_grandchild = StagePhysicalPage(grandchild, tree->page_size);
      if (!staged_grandchild.ok()) {
        return FromPageDeleteError(staged_grandchild);
      }
      PublishStagedPage(tree, staged_grandchild);
    }
  }
  auto staged_root = StagePhysicalPage(collapsed, tree->page_size);
  if (!staged_root.ok()) {
    return FromPageDeleteError(staged_root);
  }
  PublishStagedPage(tree, staged_root);
  result.root_collapsed = true;
  result.root_page_number = collapsed.page_number;
  result.evidence.push_back("root_collapsed=true");
  result.evidence.push_back("root_height_reduced=true");
  return result;
}

IndexBtreePhysicalDeleteResult UpdateParentAfterLeafMutation(
    IndexBtreePhysicalTree* tree,
    const std::vector<u64>& path,
    const IndexBtreePageBody& child,
    IndexBtreePhysicalDeleteResult result) {
  if (path.empty()) {
    return result;
  }
  auto parent_fetch = FetchIndexBtreePhysicalPageUnlocked(*tree, path.back());
  if (!parent_fetch.ok()) {
    return FromPageDeleteError(parent_fetch);
  }
  IndexBtreePageBody parent = std::move(parent_fetch.body);
  bool replaced = false;
  for (IndexBtreeCell& fence : parent.cells) {
    if (fence.child_page_number == child.page_number) {
      fence = FenceCellForPage(child);
      replaced = true;
      break;
    }
  }
  if (!replaced) {
    return PhysicalDeleteError("SB-INDEX-BTREE-PHYSICAL-PARENT-FENCE-MISSING",
                               "storage.index_btree_physical.parent_fence_missing",
                               std::to_string(child.page_number));
  }
  SortCells(&parent.cells);
  auto staged_parent = StagePhysicalPage(parent, tree->page_size);
  if (!staged_parent.ok()) {
    return FromPageDeleteError(staged_parent);
  }
  PublishStagedPage(tree, staged_parent);
  result.evidence.push_back("parent_fence_updated=true");
  if (parent.page_kind == IndexBtreePageKind::root && parent.cells.size() == 1 &&
      parent.tree_level > 0) {
    return CollapseRootToOnlyChild(tree, parent, std::move(result));
  }
  return UpdateParentAfterLeafMutation(tree, ParentPath(path), parent, std::move(result));
}

IndexBtreePhysicalDeleteResult RemoveChildFenceAndMaybeCollapse(
    IndexBtreePhysicalTree* tree,
    const std::vector<u64>& path,
    const IndexBtreePageBody& kept_child,
    u64 removed_child_page_number,
    IndexBtreePhysicalDeleteResult result) {
  if (path.empty()) {
    return result;
  }
  auto parent_fetch = FetchIndexBtreePhysicalPageUnlocked(*tree, path.back());
  if (!parent_fetch.ok()) {
    return FromPageDeleteError(parent_fetch);
  }
  IndexBtreePageBody parent = std::move(parent_fetch.body);
  const auto before = parent.cells.size();
  parent.cells.erase(std::remove_if(parent.cells.begin(),
                                    parent.cells.end(),
                                    [&](const IndexBtreeCell& fence) {
                                      return fence.child_page_number == removed_child_page_number;
                                    }),
                     parent.cells.end());
  if (parent.cells.size() + 1 != before) {
    return PhysicalDeleteError("SB-INDEX-BTREE-PHYSICAL-PARENT-FENCE-MISSING",
                               "storage.index_btree_physical.parent_fence_missing",
                               std::to_string(removed_child_page_number));
  }
  bool replaced = false;
  for (IndexBtreeCell& fence : parent.cells) {
    if (fence.child_page_number == kept_child.page_number) {
      fence = FenceCellForPage(kept_child);
      replaced = true;
      break;
    }
  }
  if (!replaced) {
    InsertCellSorted(&parent.cells, FenceCellForPage(kept_child));
  }
  SortCells(&parent.cells);
  auto staged_parent = StagePhysicalPage(parent, tree->page_size);
  if (!staged_parent.ok()) {
    return FromPageDeleteError(staged_parent);
  }
  PublishStagedPage(tree, staged_parent);
  result.evidence.push_back("parent_fence_removed=true");
  result.evidence.push_back("parent_fence_updated=true");
  if (parent.page_kind == IndexBtreePageKind::root && parent.cells.size() == 1 &&
      parent.tree_level > 0) {
    return CollapseRootToOnlyChild(tree, parent, std::move(result));
  }
  return UpdateParentAfterLeafMutation(tree, ParentPath(path), parent, std::move(result));
}

IndexBtreePhysicalDeleteResult RebuildLeafDeleteStructuralCompaction(
    IndexBtreePhysicalTree* tree,
    const IndexBtreeCell& deleted_cell,
    u64 deleted_leaf_page_number,
    IndexBtreePhysicalDeleteResult result) {
  std::set<u64> reachable_pages;
  std::vector<IndexBtreeCell> live_entries;
  u64 reachable_tombstone_count = 0;
  auto collected = CollectRepairLiveEntriesUnlocked(*tree,
                                                    &reachable_pages,
                                                    &live_entries,
                                                    &reachable_tombstone_count);
  if (!collected.ok()) {
    return PhysicalDeleteError(collected.diagnostic.diagnostic_code,
                               collected.diagnostic.message_key,
                               "delete_structural_compaction_collect_failed");
  }

  bool removed_deleted_entry = false;
  live_entries.erase(std::remove_if(live_entries.begin(),
                                    live_entries.end(),
                                    [&](const IndexBtreeCell& live_entry) {
                                      if (!removed_deleted_entry &&
                                          SameLiveEntry(live_entry, deleted_cell)) {
                                        removed_deleted_entry = true;
                                        return true;
                                      }
                                      return false;
                                    }),
                     live_entries.end());
  if (!removed_deleted_entry) {
    return PhysicalDeleteError(
        "SB-INDEX-BTREE-PHYSICAL-DELETE-COMPACTION-ENTRY-MISSING",
        "storage.index_btree_physical.delete_compaction_entry_missing",
        std::to_string(deleted_leaf_page_number));
  }

  auto rebuilt = RebuildIndexBtreePhysicalTreeFromLiveCells(
      *tree,
      live_entries,
      "leaf_delete_cross_parent_structural_compaction");
  if (!rebuilt.ok()) {
    return PhysicalDeleteError(rebuilt.diagnostic.diagnostic_code,
                               rebuilt.diagnostic.message_key,
                               "delete_structural_compaction_rebuild_failed");
  }

  *tree = std::move(rebuilt.tree);
  result.structural_rebuild_performed = true;
  result.cleanup_performed = true;
  result.root_page_number = tree->root_page_number;
  result.evidence.push_back("leaf_structural_compaction_rebuild=true");
  result.evidence.push_back("leaf_delete_cross_parent_structural_compaction=true");
  result.evidence.push_back("deleted_live_entry_removed=true");
  result.evidence.push_back("reachable_live_cells_preserved=true");
  result.evidence.push_back("preexisting_tombstones_compacted=" +
                            std::to_string(reachable_tombstone_count));
  result.evidence.push_back("rebuilt_root_page_number=" +
                            std::to_string(tree->root_page_number));
  result.evidence.insert(result.evidence.end(),
                         rebuilt.evidence.begin(),
                         rebuilt.evidence.end());
  return result;
}

IndexBtreePhysicalDeleteResult RebalanceOrMergeLeaf(IndexBtreePhysicalTree* tree,
                                                    const std::vector<u64>& path,
                                                    IndexBtreePageBody leaf,
                                                    const IndexBtreeCell& deleted_cell,
                                                    IndexBtreePhysicalDeleteResult result) {
  RemoveDeletedCells(&leaf);
  if (!IsUnderfilledNonRoot(leaf)) {
    auto staged_leaf = StagePageForDelete(tree, leaf);
    if (!staged_leaf.ok()) {
      return staged_leaf;
    }
    return UpdateParentAfterLeafMutation(tree, path, leaf, std::move(result));
  }

  std::optional<IndexBtreePageBody> left;
  if (leaf.left_sibling_page_number != 0) {
    auto left_fetch = FetchIndexBtreePhysicalPageUnlocked(*tree, leaf.left_sibling_page_number);
    if (!left_fetch.ok()) {
      return FromPageDeleteError(left_fetch);
    }
    left = std::move(left_fetch.body);
    RemoveDeletedCells(&*left);
  }
  std::optional<IndexBtreePageBody> right;
  if (leaf.right_sibling_page_number != 0) {
    auto right_fetch = FetchIndexBtreePhysicalPageUnlocked(*tree, leaf.right_sibling_page_number);
    if (!right_fetch.ok()) {
      return FromPageDeleteError(right_fetch);
    }
    right = std::move(right_fetch.body);
    RemoveDeletedCells(&*right);
  }

  if (left.has_value() && SameParentAndLevel(*left, leaf) && left->cells.size() > 2) {
    leaf.cells.insert(leaf.cells.begin(), left->cells.back());
    left->cells.pop_back();
    SortCells(&leaf.cells);
    SortCells(&left->cells);
    auto staged_left = StagePageForDelete(tree, *left);
    if (!staged_left.ok()) {
      return staged_left;
    }
    auto staged_leaf = StagePageForDelete(tree, leaf);
    if (!staged_leaf.ok()) {
      return staged_leaf;
    }
    result.rebalance_performed = true;
    result.evidence.push_back("leaf_rebalance_from_left=true");
    result = UpdateParentAfterLeafMutation(tree, path, *left, std::move(result));
    if (!result.ok()) {
      return result;
    }
    return UpdateParentAfterLeafMutation(tree, path, leaf, std::move(result));
  }

  if (right.has_value() && SameParentAndLevel(leaf, *right) && right->cells.size() > 2) {
    leaf.cells.push_back(right->cells.front());
    right->cells.erase(right->cells.begin());
    SortCells(&leaf.cells);
    SortCells(&right->cells);
    auto staged_leaf = StagePageForDelete(tree, leaf);
    if (!staged_leaf.ok()) {
      return staged_leaf;
    }
    auto staged_right = StagePageForDelete(tree, *right);
    if (!staged_right.ok()) {
      return staged_right;
    }
    result.rebalance_performed = true;
    result.evidence.push_back("leaf_rebalance_from_right=true");
    result = UpdateParentAfterLeafMutation(tree, path, leaf, std::move(result));
    if (!result.ok()) {
      return result;
    }
    return UpdateParentAfterLeafMutation(tree, path, *right, std::move(result));
  }

  if (left.has_value() && SameParentAndLevel(*left, leaf)) {
    IndexBtreePageBody merged = *left;
    merged.cells.insert(merged.cells.end(), leaf.cells.begin(), leaf.cells.end());
    SortCells(&merged.cells);
    merged.right_sibling_page_number = leaf.right_sibling_page_number;
    auto staged_merged = StagePhysicalPage(merged, tree->page_size);
    if (staged_merged.ok()) {
      std::optional<IndexBtreePageBodyResult> staged_old_right;
      if (leaf.right_sibling_page_number != 0) {
        auto old_right_fetch = FetchIndexBtreePhysicalPageUnlocked(*tree, leaf.right_sibling_page_number);
        if (!old_right_fetch.ok()) {
          return FromPageDeleteError(old_right_fetch);
        }
        IndexBtreePageBody old_right = std::move(old_right_fetch.body);
        old_right.left_sibling_page_number = merged.page_number;
        staged_old_right = StagePhysicalPage(old_right, tree->page_size);
        if (!staged_old_right->ok()) {
          return FromPageDeleteError(*staged_old_right);
        }
      }
      PublishStagedPage(tree, staged_merged);
      if (staged_old_right.has_value()) {
        PublishStagedPage(tree, *staged_old_right);
      }
      result.merge_performed = true;
      result.kept_page_number = merged.page_number;
      result.removed_page_number = leaf.page_number;
      result.evidence.push_back("leaf_merge_into_left=true");
      return RemoveChildFenceAndMaybeCollapse(tree,
                                              path,
                                              merged,
                                              leaf.page_number,
                                              std::move(result));
    }
  }

  if (right.has_value() && SameParentAndLevel(leaf, *right)) {
    IndexBtreePageBody merged = leaf;
    merged.cells.insert(merged.cells.end(), right->cells.begin(), right->cells.end());
    SortCells(&merged.cells);
    merged.right_sibling_page_number = right->right_sibling_page_number;
    auto staged_merged = StagePhysicalPage(merged, tree->page_size);
    if (!staged_merged.ok()) {
      return FromPageDeleteError(staged_merged);
    }
    std::optional<IndexBtreePageBodyResult> staged_old_right;
    if (right->right_sibling_page_number != 0) {
      auto old_right_fetch = FetchIndexBtreePhysicalPageUnlocked(*tree, right->right_sibling_page_number);
      if (!old_right_fetch.ok()) {
        return FromPageDeleteError(old_right_fetch);
      }
      IndexBtreePageBody old_right = std::move(old_right_fetch.body);
      old_right.left_sibling_page_number = merged.page_number;
      staged_old_right = StagePhysicalPage(old_right, tree->page_size);
      if (!staged_old_right->ok()) {
        return FromPageDeleteError(*staged_old_right);
      }
    }
    PublishStagedPage(tree, staged_merged);
    if (staged_old_right.has_value()) {
      PublishStagedPage(tree, *staged_old_right);
    }
    result.merge_performed = true;
    result.kept_page_number = merged.page_number;
    result.removed_page_number = right->page_number;
    result.evidence.push_back("leaf_merge_into_current=true");
    return RemoveChildFenceAndMaybeCollapse(tree,
                                            path,
                                            merged,
                                            right->page_number,
                                            std::move(result));
  }

  auto staged_leaf = StagePhysicalPage(leaf, tree->page_size);
  if (!staged_leaf.ok()) {
    return FromPageDeleteError(staged_leaf);
  }
  return RebuildLeafDeleteStructuralCompaction(tree,
                                               deleted_cell,
                                               leaf.page_number,
                                               std::move(result));
}

int CompareEncodedKeysOnly(const std::vector<byte>& left,
                           const std::vector<byte>& right) {
  return CompareUnsignedBytes(left, right, kOrderPreservingKeyMagic.size());
}

bool EncodedKeyStartsWith(const std::vector<byte>& key,
                          const std::vector<byte>& prefix) {
  return key.size() >= prefix.size() &&
         std::equal(prefix.begin(), prefix.end(), key.begin());
}

struct ScanTraversalAccounting {
  u64 reachable_leaf_pages = 0;
  u64 visited_leaf_pages = 0;
  u64 pruned_leaf_pages = 0;
  u64 pruned_subtrees = 0;
};

IndexBtreePhysicalScanResult ValidateScanKeyBytes(const std::vector<byte>& encoded_key,
                                                  std::string_view detail) {
  IndexBtreeCell cell;
  cell.encoded_key = encoded_key;
  const auto validation = ValidateEncodedKey(cell, 0);
  if (!validation.ok()) {
    return PhysicalScanError(validation.diagnostic.diagnostic_code,
                             validation.diagnostic.message_key,
                             std::string(detail));
  }
  return {};
}

IndexBtreePhysicalScanResult ValidateScanPrefixBytes(
    const std::vector<byte>& encoded_prefix,
    std::string_view detail) {
  if (encoded_prefix.empty()) {
    return PhysicalScanError("SB-INDEX-BTREE-PHYSICAL-SCAN-PREFIX-MISSING",
                             "storage.index_btree_physical.scan_prefix_missing",
                             std::string(detail));
  }
  if (IsUnsafeLegacyEncodedKey(encoded_prefix)) {
    return PhysicalScanError("SB-INDEX-BTREE-PHYSICAL-SCAN-UNSAFE-LEGACY-PREFIX",
                             "storage.index_btree_physical.scan_unsafe_legacy_prefix",
                             std::string(detail));
  }
  if (!IsOrderPreservingEncodedKey(encoded_prefix)) {
    return PhysicalScanError("SB-INDEX-BTREE-PHYSICAL-SCAN-PREFIX-ENVELOPE-INVALID",
                             "storage.index_btree_physical.scan_prefix_envelope_invalid",
                             std::string(detail));
  }
  if (encoded_prefix.size() <= kOrderPreservingKeyMagic.size()) {
    return PhysicalScanError("SB-INDEX-BTREE-PHYSICAL-SCAN-PREFIX-EMPTY",
                             "storage.index_btree_physical.scan_prefix_empty",
                             std::string(detail));
  }
  if (encoded_prefix.size() >= 2 &&
      encoded_prefix[encoded_prefix.size() - 2] == 0x00 &&
      encoded_prefix[encoded_prefix.size() - 1] == 0x00) {
    return PhysicalScanError("SB-INDEX-BTREE-PHYSICAL-SCAN-PREFIX-TERMINATED",
                             "storage.index_btree_physical.scan_prefix_terminated",
                             "prefix scans require generated matcher bytes, not a full encoded key");
  }
  return {};
}

bool FenceHighKeyBelowLowerBound(const IndexBtreePhysicalScanBound& lower_bound,
                                 const IndexBtreeCell& fence) {
  if (lower_bound.unbounded) {
    return false;
  }
  const int compare = CompareEncodedKeysOnly(fence.encoded_key, lower_bound.encoded_key);
  return compare < 0 || (compare == 0 && !lower_bound.inclusive);
}

bool FenceHighKeyEndsAfterUpperBound(const IndexBtreePhysicalScanBound& upper_bound,
                                     const IndexBtreeCell& fence) {
  if (upper_bound.unbounded) {
    return false;
  }
  const int compare = CompareEncodedKeysOnly(fence.encoded_key, upper_bound.encoded_key);
  return compare > 0 || (compare == 0 && !upper_bound.inclusive);
}

int CompareFenceKeyToPrefix(const IndexBtreeCell& fence,
                            const std::vector<byte>& prefix) {
  if (EncodedKeyStartsWith(fence.encoded_key, prefix)) {
    return 0;
  }
  return CompareEncodedKeysOnly(fence.encoded_key, prefix);
}

bool FenceHighKeyBelowScanRequest(const IndexBtreePhysicalScanRequest& request,
                                  const IndexBtreeCell& fence) {
  switch (request.mode) {
    case IndexBtreePhysicalScanMode::point:
      return CompareEncodedKeysOnly(fence.encoded_key, request.point_key) < 0;
    case IndexBtreePhysicalScanMode::range:
      return FenceHighKeyBelowLowerBound(request.lower_bound, fence);
    case IndexBtreePhysicalScanMode::prefix:
      return CompareFenceKeyToPrefix(fence, request.prefix) < 0;
    case IndexBtreePhysicalScanMode::ordered:
      return false;
  }
  return false;
}

bool FenceHighKeyEndsScanRequest(const IndexBtreePhysicalScanRequest& request,
                                 const IndexBtreeCell& fence) {
  switch (request.mode) {
    case IndexBtreePhysicalScanMode::point:
      return CompareEncodedKeysOnly(fence.encoded_key, request.point_key) > 0;
    case IndexBtreePhysicalScanMode::range:
      return FenceHighKeyEndsAfterUpperBound(request.upper_bound, fence);
    case IndexBtreePhysicalScanMode::prefix:
      return CompareFenceKeyToPrefix(fence, request.prefix) > 0;
    case IndexBtreePhysicalScanMode::ordered:
      return false;
  }
  return false;
}

struct PrunedLeafCountResult {
  bool ok = true;
  u64 leaf_pages = 0;
  IndexBtreePhysicalScanResult error;
};

PrunedLeafCountResult CountPrunedLeafPages(const IndexBtreePhysicalTree& tree,
                                           u64 page_number,
                                           u64 expected_parent_page_number,
                                           std::vector<u64>* counted_pages) {
  if (std::find(counted_pages->begin(), counted_pages->end(), page_number) !=
      counted_pages->end()) {
    PrunedLeafCountResult result;
    result.ok = false;
    result.error = PhysicalScanError("SB-INDEX-BTREE-PHYSICAL-SCAN-CYCLE",
                                     "storage.index_btree_physical.scan_cycle",
                                     std::to_string(page_number));
    return result;
  }
  counted_pages->push_back(page_number);
  auto fetched = FetchIndexBtreePhysicalPageUnlocked(tree, page_number);
  if (!fetched.ok()) {
    PrunedLeafCountResult result;
    result.ok = false;
    result.error = FromPageScanError(fetched);
    return result;
  }
  const IndexBtreePageBody& page = fetched.body;
  if (page.index_uuid.value != tree.index_uuid.value) {
    PrunedLeafCountResult result;
    result.ok = false;
    result.error = PhysicalScanError("SB-INDEX-BTREE-PHYSICAL-SCAN-INDEX-MISMATCH",
                                     "storage.index_btree_physical.scan_index_mismatch",
                                     std::to_string(page_number));
    return result;
  }
  if (page.parent_page_number != expected_parent_page_number) {
    PrunedLeafCountResult result;
    result.ok = false;
    result.error = PhysicalScanError("SB-INDEX-BTREE-PHYSICAL-SCAN-PARENT-MISMATCH",
                                     "storage.index_btree_physical.scan_parent_mismatch",
                                     std::to_string(page_number));
    return result;
  }
  PrunedLeafCountResult result;
  if (IsLeafLike(page)) {
    result.leaf_pages = 1;
    return result;
  }
  if (!IsInternalLike(page)) {
    result.ok = false;
    result.error = PhysicalScanError("SB-INDEX-BTREE-PHYSICAL-SCAN-PAGE-KIND-INVALID",
                                     "storage.index_btree_physical.scan_page_kind_invalid",
                                     std::to_string(page_number));
    return result;
  }
  for (const IndexBtreeCell& fence : page.cells) {
    if (!fence.high_key || fence.child_page_number == 0) {
      result.ok = false;
      result.error = PhysicalScanError("SB-INDEX-BTREE-PHYSICAL-SCAN-FENCE-INVALID",
                                       "storage.index_btree_physical.scan_fence_invalid",
                                       std::to_string(page.page_number));
      return result;
    }
    if (page.tree_level == 1) {
      ++result.leaf_pages;
    } else {
      auto child_count = CountPrunedLeafPages(tree,
                                              fence.child_page_number,
                                              page.page_number,
                                              counted_pages);
      if (!child_count.ok) {
        return child_count;
      }
      result.leaf_pages += child_count.leaf_pages;
    }
  }
  return result;
}

IndexBtreePhysicalScanResult AccountPrunedSubtree(const IndexBtreePhysicalTree& tree,
                                                  const IndexBtreePageBody& parent,
                                                  const IndexBtreeCell& fence,
                                                  ScanTraversalAccounting* accounting) {
  u64 leaf_pages = 1;
  if (parent.tree_level > 1) {
    std::vector<u64> counted_pages;
    auto count = CountPrunedLeafPages(tree,
                                      fence.child_page_number,
                                      parent.page_number,
                                      &counted_pages);
    if (!count.ok) {
      return count.error;
    }
    leaf_pages = count.leaf_pages;
  }
  accounting->reachable_leaf_pages += leaf_pages;
  accounting->pruned_leaf_pages += leaf_pages;
  ++accounting->pruned_subtrees;
  return {};
}

IndexBtreePhysicalScanResult CollectPrunedScanLeaves(
    const IndexBtreePhysicalTree& tree,
    const IndexBtreePhysicalScanRequest& request,
    u64 page_number,
    u64 expected_parent_page_number,
    std::vector<u64>* visited_pages,
    std::vector<IndexBtreePageBody>* leaves,
    ScanTraversalAccounting* accounting) {
  if (std::find(visited_pages->begin(), visited_pages->end(), page_number) !=
      visited_pages->end()) {
    return PhysicalScanError("SB-INDEX-BTREE-PHYSICAL-SCAN-CYCLE",
                             "storage.index_btree_physical.scan_cycle",
                             std::to_string(page_number));
  }
  visited_pages->push_back(page_number);

  auto fetched = FetchIndexBtreePhysicalPageUnlocked(tree, page_number);
  if (!fetched.ok()) {
    return FromPageScanError(fetched);
  }
  IndexBtreePageBody page = std::move(fetched.body);
  if (page.index_uuid.value != tree.index_uuid.value) {
    return PhysicalScanError("SB-INDEX-BTREE-PHYSICAL-SCAN-INDEX-MISMATCH",
                             "storage.index_btree_physical.scan_index_mismatch",
                             std::to_string(page_number));
  }
  if (page.parent_page_number != expected_parent_page_number) {
    return PhysicalScanError("SB-INDEX-BTREE-PHYSICAL-SCAN-PARENT-MISMATCH",
                             "storage.index_btree_physical.scan_parent_mismatch",
                             std::to_string(page_number));
  }
  if (IsLeafLike(page)) {
    ++accounting->reachable_leaf_pages;
    ++accounting->visited_leaf_pages;
    leaves->push_back(std::move(page));
    return {};
  }
  if (!IsInternalLike(page)) {
    return PhysicalScanError("SB-INDEX-BTREE-PHYSICAL-SCAN-PAGE-KIND-INVALID",
                             "storage.index_btree_physical.scan_page_kind_invalid",
                             std::to_string(page_number));
  }

  for (std::size_t i = 0; i < page.cells.size(); ++i) {
    const IndexBtreeCell& fence = page.cells[i];
    if (!fence.high_key || fence.child_page_number == 0) {
      return PhysicalScanError("SB-INDEX-BTREE-PHYSICAL-SCAN-FENCE-INVALID",
                               "storage.index_btree_physical.scan_fence_invalid",
                               std::to_string(page.page_number));
    }
    if (request.mode != IndexBtreePhysicalScanMode::ordered &&
        FenceHighKeyBelowScanRequest(request, fence)) {
      auto pruned = AccountPrunedSubtree(tree, page, fence, accounting);
      if (!pruned.ok()) {
        return pruned;
      }
      continue;
    }

    auto child_result = CollectPrunedScanLeaves(tree,
                                                request,
                                                fence.child_page_number,
                                                page.page_number,
                                                visited_pages,
                                                leaves,
                                                accounting);
    if (!child_result.ok()) {
      return child_result;
    }

    if (request.mode != IndexBtreePhysicalScanMode::ordered &&
        FenceHighKeyEndsScanRequest(request, fence)) {
      for (std::size_t j = i + 1; j < page.cells.size(); ++j) {
        auto pruned = AccountPrunedSubtree(tree, page, page.cells[j], accounting);
        if (!pruned.ok()) {
          return pruned;
        }
      }
      break;
    }
  }
  return {};
}

bool ScanBoundAllowsLower(const IndexBtreePhysicalScanBound& bound,
                          const IndexBtreeCell& cell) {
  if (bound.unbounded) {
    return true;
  }
  const int compare = CompareEncodedKeysOnly(cell.encoded_key, bound.encoded_key);
  return compare > 0 || (compare == 0 && bound.inclusive);
}

bool ScanBoundAllowsUpper(const IndexBtreePhysicalScanBound& bound,
                          const IndexBtreeCell& cell) {
  if (bound.unbounded) {
    return true;
  }
  const int compare = CompareEncodedKeysOnly(cell.encoded_key, bound.encoded_key);
  return compare < 0 || (compare == 0 && bound.inclusive);
}

bool ScanRequestIncludesCell(const IndexBtreePhysicalScanRequest& request,
                             const IndexBtreeCell& cell) {
  switch (request.mode) {
    case IndexBtreePhysicalScanMode::point:
      return CompareEncodedKeysOnly(cell.encoded_key, request.point_key) == 0;
    case IndexBtreePhysicalScanMode::range:
    case IndexBtreePhysicalScanMode::ordered:
      return ScanBoundAllowsLower(request.lower_bound, cell) &&
             ScanBoundAllowsUpper(request.upper_bound, cell);
    case IndexBtreePhysicalScanMode::prefix:
      return EncodedKeyStartsWith(cell.encoded_key, request.prefix);
  }
  return false;
}

bool LocatorLess(const IndexBtreePhysicalRowLocator& left,
                 const IndexBtreePhysicalRowLocator& right) {
  const int key_compare = CompareEncodedKeysOnly(left.encoded_key, right.encoded_key);
  if (key_compare != 0) {
    return key_compare < 0;
  }
  const int row_compare = CompareUuid128(left.row_uuid.value, right.row_uuid.value);
  if (row_compare != 0) {
    return row_compare < 0;
  }
  return CompareUuid128(left.version_uuid.value, right.version_uuid.value) < 0;
}

bool SameFenceCell(const IndexBtreeCell& observed, const IndexBtreeCell& expected) {
  return observed.high_key &&
         !observed.deleted &&
         observed.child_page_number == expected.child_page_number &&
         CompareIndexBtreeCellsUnchecked(observed, expected) >= 0;
}

bool SameLiveEntry(const IndexBtreeCell& left, const IndexBtreeCell& right) {
  return left.encoded_key == right.encoded_key &&
         left.row_uuid.value == right.row_uuid.value &&
         left.version_uuid.value == right.version_uuid.value;
}

bool BulkPageAcceptsCell(IndexBtreePageBody body,
                         const IndexBtreeCell& cell,
                         u32 page_size,
                         u32 capacity) {
  if (capacity != 0 && body.cells.size() >= capacity) {
    return false;
  }
  body.cells.push_back(cell);
  return BuildIndexBtreePageBody(body, page_size).ok();
}

IndexBtreePhysicalBulkBuildResult SerializeBulkBuiltPages(
    std::vector<IndexBtreePageBody> pages,
    u64 root_page_number,
    u64 next_page_number,
    u32 page_size,
    TypedUuid index_uuid,
    u64 leaf_page_count,
    u64 branch_level_count) {
  IndexBtreePhysicalBulkBuildResult result;
  result.status = IndexPageOkStatus();
  result.tree.page_size = page_size;
  result.tree.index_uuid = index_uuid;
  result.tree.root_page_number = root_page_number;
  result.tree.next_page_number = next_page_number;

  std::sort(pages.begin(),
            pages.end(),
            [](const IndexBtreePageBody& left,
               const IndexBtreePageBody& right) {
              return left.page_number < right.page_number;
            });
  for (const auto& page : pages) {
    auto built = BuildIndexBtreePageBody(page, page_size);
    if (!built.ok()) {
      return PhysicalBulkBuildError(built.diagnostic.diagnostic_code,
                                    built.diagnostic.message_key,
                                    std::to_string(page.page_number));
    }
    auto parsed = ParseIndexBtreePageBody(built.serialized, page.page_number);
    if (!parsed.ok()) {
      return PhysicalBulkBuildError(parsed.diagnostic.diagnostic_code,
                                    parsed.diagnostic.message_key,
                                    std::to_string(page.page_number));
    }
    result.tree.pages.push_back({page.page_number, std::move(built.serialized)});
  }

  auto validation = ValidateIndexBtreePhysicalTreeUnlocked(result.tree);
  if (!validation.ok()) {
    return PhysicalBulkBuildError(validation.diagnostic.diagnostic_code,
                                  validation.diagnostic.message_key,
                                  "bulk_build_validation_failed");
  }
  result.report = BuildIndexBtreePhysicalTreeReportUnlocked(result.tree);
  if (!result.report.valid) {
    return PhysicalBulkBuildError(result.report.exact_diagnostic_code,
                                  result.report.exact_diagnostic_message_key,
                                  "bulk_build_report_invalid");
  }

  result.physical_leaf_pack = true;
  result.branch_levels_built = branch_level_count > 0;
  result.fence_keys_stored = branch_level_count > 0;
  result.candidate_root_generation_created = true;
  result.leaf_page_count = leaf_page_count;
  result.branch_level_count = branch_level_count;
  result.evidence = {"physical_leaf_pack=true",
                     std::string("branch_levels_built=") +
                         (result.branch_levels_built ? "true" : "false"),
                     std::string("fence_keys_stored=") +
                         (result.fence_keys_stored ? "true" : "false"),
                     "candidate_root_generation_created=true",
                     "root_publish_authorized=false",
                     "physical_append_authorized=false",
                     "sorted_bulk_candidate_tree_validated=true",
                     "retail_per_row_insert_used=false",
                     "visibility_authority=false",
                     "authorization_authority=false",
                     "transaction_finality_authority=false",
                     "recovery_authority=false"};
  result.evidence.push_back("leaf_page_count=" +
                            std::to_string(result.leaf_page_count));
  result.evidence.push_back("branch_level_count=" +
                            std::to_string(result.branch_level_count));
  result.evidence.push_back("root_page_number=" +
                            std::to_string(result.tree.root_page_number));
  result.evidence.insert(result.evidence.end(),
                         validation.evidence.begin(),
                         validation.evidence.end());
  return result;
}

IndexBtreePhysicalCorruptionClass ClassifyIndexBtreeDiagnostic(
    const std::string& diagnostic_code) {
  if (diagnostic_code.empty()) {
    return IndexBtreePhysicalCorruptionClass::none;
  }
  if (diagnostic_code.find("CHECKSUM") != std::string::npos) {
    return IndexBtreePhysicalCorruptionClass::checksum;
  }
  if (diagnostic_code.find("DUPLICATE") != std::string::npos) {
    return IndexBtreePhysicalCorruptionClass::duplicate;
  }
  if (diagnostic_code.find("PARENT") != std::string::npos) {
    return IndexBtreePhysicalCorruptionClass::parent;
  }
  if (diagnostic_code.find("FENCE") != std::string::npos) {
    return IndexBtreePhysicalCorruptionClass::fence;
  }
  if (diagnostic_code.find("SIBLING") != std::string::npos) {
    return IndexBtreePhysicalCorruptionClass::sibling;
  }
  if (diagnostic_code.find("ORDER") != std::string::npos ||
      diagnostic_code.find("UNSORTED") != std::string::npos ||
      diagnostic_code.find("LEAF-RANGE") != std::string::npos ||
      diagnostic_code.find("RANGE-PARTITION") != std::string::npos) {
    return IndexBtreePhysicalCorruptionClass::order;
  }
  if (diagnostic_code.find("PAGE") != std::string::npos ||
      diagnostic_code.find("MAGIC") != std::string::npos ||
      diagnostic_code.find("HEADER") != std::string::npos ||
      diagnostic_code.find("BODY") != std::string::npos ||
      diagnostic_code.find("PAYLOAD") != std::string::npos ||
      diagnostic_code.find("CELL") != std::string::npos ||
      diagnostic_code.find("LEVEL") != std::string::npos ||
      diagnostic_code.find("KIND") != std::string::npos ||
      diagnostic_code.find("INDEX") != std::string::npos ||
      diagnostic_code.find("UUID") != std::string::npos) {
    return IndexBtreePhysicalCorruptionClass::page;
  }
  if (diagnostic_code.find("TREE-INVALID") != std::string::npos ||
      diagnostic_code.find("IMAGE-INVALID") != std::string::npos) {
    return IndexBtreePhysicalCorruptionClass::tree;
  }
  return IndexBtreePhysicalCorruptionClass::unknown;
}

void AddStructuralNonAuthorityEvidence(std::vector<std::string>* evidence) {
  evidence->push_back("structural_only=true");
  evidence->push_back("visibility=false");
  evidence->push_back("authorization=false");
  evidence->push_back("transaction_finality=false");
  evidence->push_back("recovery=false");
  evidence->push_back("visibility_authority=false");
  evidence->push_back("authorization_authority=false");
  evidence->push_back("transaction_finality_authority=false");
  evidence->push_back("recovery_authority=false");
}

IndexBtreePhysicalTreeValidationResult CollectReachableContentUnlocked(
    const IndexBtreePhysicalTree& tree,
    u64 page_number,
    u64 expected_parent_page_number,
    std::set<u64>* reachable_pages,
    std::vector<IndexBtreePageBody>* leaves,
    std::vector<IndexBtreeCell>* live_entries,
    u64* tombstone_count) {
  if (!reachable_pages->insert(page_number).second) {
    return PhysicalValidationError("SB-INDEX-BTREE-PHYSICAL-VALIDATE-CYCLE",
                                   "storage.index_btree_physical.validate_cycle",
                                   std::to_string(page_number));
  }
  auto fetched = FetchIndexBtreePhysicalPageUnlocked(tree, page_number);
  if (!fetched.ok()) {
    return PhysicalValidationError(fetched.diagnostic.diagnostic_code,
                                   fetched.diagnostic.message_key,
                                   std::to_string(page_number));
  }
  const IndexBtreePageBody page = std::move(fetched.body);
  if (page.index_uuid.value != tree.index_uuid.value) {
    return PhysicalValidationError("SB-INDEX-BTREE-PHYSICAL-VALIDATE-INDEX-MISMATCH",
                                   "storage.index_btree_physical.validate_index_mismatch",
                                   std::to_string(page_number));
  }
  if (page.parent_page_number != expected_parent_page_number) {
    return PhysicalValidationError("SB-INDEX-BTREE-PHYSICAL-VALIDATE-PARENT-MISMATCH",
                                   "storage.index_btree_physical.validate_parent_mismatch",
                                   std::to_string(page_number));
  }
  if (IsLeafLike(page)) {
    leaves->push_back(page);
    for (const IndexBtreeCell& cell : page.cells) {
      if (cell.deleted) {
        ++(*tombstone_count);
      } else {
        live_entries->push_back(cell);
      }
    }
    IndexBtreePhysicalTreeValidationResult result;
    result.status = IndexPageOkStatus();
    return result;
  }
  if (!IsInternalLike(page)) {
    return PhysicalValidationError("SB-INDEX-BTREE-PHYSICAL-VALIDATE-PAGE-KIND-INVALID",
                                   "storage.index_btree_physical.validate_page_kind_invalid",
                                   std::to_string(page_number));
  }
  for (const IndexBtreeCell& fence : page.cells) {
    if (!fence.high_key || fence.child_page_number == 0) {
      return PhysicalValidationError("SB-INDEX-BTREE-PHYSICAL-VALIDATE-FENCE-INVALID",
                                     "storage.index_btree_physical.validate_fence_invalid",
                                     std::to_string(page_number));
    }
    auto child_fetch = FetchIndexBtreePhysicalPageUnlocked(tree, fence.child_page_number);
    if (!child_fetch.ok()) {
      return PhysicalValidationError(child_fetch.diagnostic.diagnostic_code,
                                     child_fetch.diagnostic.message_key,
                                     std::to_string(fence.child_page_number));
    }
    if (child_fetch.body.tree_level + 1 != page.tree_level) {
      return PhysicalValidationError("SB-INDEX-BTREE-PHYSICAL-VALIDATE-LEVEL-MISMATCH",
                                     "storage.index_btree_physical.validate_level_mismatch",
                                     std::to_string(fence.child_page_number));
    }
    const IndexBtreeCell expected_fence = FenceCellForPage(child_fetch.body);
    if (!SameFenceCell(fence, expected_fence)) {
      return PhysicalValidationError("SB-INDEX-BTREE-PHYSICAL-VALIDATE-FENCE-MISMATCH",
                                     "storage.index_btree_physical.validate_fence_mismatch",
                                     std::to_string(fence.child_page_number));
    }
    auto child_result = CollectReachableContentUnlocked(tree,
                                                        fence.child_page_number,
                                                        page.page_number,
                                                        reachable_pages,
                                                        leaves,
                                                        live_entries,
                                                        tombstone_count);
    if (!child_result.ok()) {
      return child_result;
    }
  }
  IndexBtreePhysicalTreeValidationResult result;
  result.status = IndexPageOkStatus();
  return result;
}

IndexBtreePageBodyResult ValidateDescentPathUnlocked(
    const IndexBtreePhysicalTree& tree,
    const std::vector<u64>& path,
    const IndexBtreePageBody& leaf,
    const IndexBtreeCell& key) {
  if (!IsLeafLike(leaf)) {
    return IndexPageError("SB-INDEX-BTREE-PHYSICAL-DESCENT-LEAF-INVALID",
                          "storage.index_btree_physical.descent_leaf_invalid",
                          std::to_string(leaf.page_number));
  }
  u64 expected_parent = 0;
  for (std::size_t i = 0; i < path.size(); ++i) {
    const u64 parent_page_number = path[i];
    auto parent_fetch = FetchIndexBtreePhysicalPageUnlocked(tree, parent_page_number);
    if (!parent_fetch.ok()) {
      return parent_fetch;
    }
    const IndexBtreePageBody& parent = parent_fetch.body;
    if (!IsInternalLike(parent) || parent.parent_page_number != expected_parent) {
      return IndexPageError("SB-INDEX-BTREE-PHYSICAL-DESCENT-STALE",
                            "storage.index_btree_physical.descent_stale",
                            std::to_string(parent_page_number));
    }
    const u64 expected_child = (i + 1 < path.size()) ? path[i + 1] : leaf.page_number;
    const u64 selected_child = SelectChildPage(parent, key);
    if (selected_child != expected_child) {
      return IndexPageError("SB-INDEX-BTREE-PHYSICAL-DESCENT-STALE",
                            "storage.index_btree_physical.descent_stale",
                            std::to_string(parent_page_number));
    }
    const bool has_child_fence =
        std::any_of(parent.cells.begin(),
                    parent.cells.end(),
                    [&](const IndexBtreeCell& fence) {
                      return fence.high_key &&
                             fence.child_page_number == expected_child;
                    });
    if (!has_child_fence) {
      return IndexPageError("SB-INDEX-BTREE-PHYSICAL-DESCENT-FENCE-MISSING",
                            "storage.index_btree_physical.descent_fence_missing",
                            std::to_string(expected_child));
    }
    expected_parent = parent_page_number;
  }
  if (leaf.parent_page_number != expected_parent) {
    return IndexPageError("SB-INDEX-BTREE-PHYSICAL-DESCENT-PARENT-MISMATCH",
                          "storage.index_btree_physical.descent_parent_mismatch",
                          std::to_string(leaf.page_number));
  }

  IndexBtreePageBodyResult result;
  result.status = IndexPageOkStatus();
  return result;
}

IndexBtreePhysicalTreeValidationResult ValidateReachablePageUnlocked(
    const IndexBtreePhysicalTree& tree,
    u64 page_number,
    u64 expected_parent_page_number,
    std::set<u64>* reachable_pages,
    std::vector<IndexBtreePageBody>* leaves,
    std::vector<IndexBtreeCell>* live_entries) {
  if (!reachable_pages->insert(page_number).second) {
    return PhysicalValidationError("SB-INDEX-BTREE-PHYSICAL-VALIDATE-CYCLE",
                                   "storage.index_btree_physical.validate_cycle",
                                   std::to_string(page_number));
  }
  auto fetched = FetchIndexBtreePhysicalPageUnlocked(tree, page_number);
  if (!fetched.ok()) {
    return PhysicalValidationError(fetched.diagnostic.diagnostic_code,
                                   fetched.diagnostic.message_key,
                                   std::to_string(page_number));
  }
  const IndexBtreePageBody page = std::move(fetched.body);
  if (page.index_uuid.value != tree.index_uuid.value) {
    return PhysicalValidationError("SB-INDEX-BTREE-PHYSICAL-VALIDATE-INDEX-MISMATCH",
                                   "storage.index_btree_physical.validate_index_mismatch",
                                   std::to_string(page_number));
  }
  if (page.parent_page_number != expected_parent_page_number) {
    return PhysicalValidationError("SB-INDEX-BTREE-PHYSICAL-VALIDATE-PARENT-MISMATCH",
                                   "storage.index_btree_physical.validate_parent_mismatch",
                                   std::to_string(page_number));
  }
  if (IsLeafLike(page)) {
    leaves->push_back(page);
    for (const IndexBtreeCell& cell : page.cells) {
      if (!cell.deleted) {
        live_entries->push_back(cell);
      }
    }
    IndexBtreePhysicalTreeValidationResult result;
    result.status = IndexPageOkStatus();
    return result;
  }
  if (!IsInternalLike(page)) {
    return PhysicalValidationError("SB-INDEX-BTREE-PHYSICAL-VALIDATE-PAGE-KIND-INVALID",
                                   "storage.index_btree_physical.validate_page_kind_invalid",
                                   std::to_string(page_number));
  }
  for (const IndexBtreeCell& fence : page.cells) {
    if (!fence.high_key || fence.child_page_number == 0) {
      return PhysicalValidationError("SB-INDEX-BTREE-PHYSICAL-VALIDATE-FENCE-INVALID",
                                     "storage.index_btree_physical.validate_fence_invalid",
                                     std::to_string(page_number));
    }
    auto child_fetch = FetchIndexBtreePhysicalPageUnlocked(tree, fence.child_page_number);
    if (!child_fetch.ok()) {
      return PhysicalValidationError(child_fetch.diagnostic.diagnostic_code,
                                     child_fetch.diagnostic.message_key,
                                     std::to_string(fence.child_page_number));
    }
    if (child_fetch.body.tree_level + 1 != page.tree_level) {
      return PhysicalValidationError("SB-INDEX-BTREE-PHYSICAL-VALIDATE-LEVEL-MISMATCH",
                                     "storage.index_btree_physical.validate_level_mismatch",
                                     std::to_string(fence.child_page_number));
    }
    const IndexBtreeCell expected_fence = FenceCellForPage(child_fetch.body);
    if (!SameFenceCell(fence, expected_fence)) {
      return PhysicalValidationError("SB-INDEX-BTREE-PHYSICAL-VALIDATE-FENCE-MISMATCH",
                                     "storage.index_btree_physical.validate_fence_mismatch",
                                     std::to_string(fence.child_page_number));
    }
    auto child_result = ValidateReachablePageUnlocked(tree,
                                                      fence.child_page_number,
                                                      page.page_number,
                                                      reachable_pages,
                                                      leaves,
                                                      live_entries);
    if (!child_result.ok()) {
      return child_result;
    }
  }

  IndexBtreePhysicalTreeValidationResult result;
  result.status = IndexPageOkStatus();
  return result;
}

IndexBtreePhysicalTreeValidationResult ValidateIndexBtreePhysicalTreeUnlocked(
    const IndexBtreePhysicalTree& tree) {
  if (tree.root_page_number == 0 || tree.page_size == 0) {
    return PhysicalValidationError("SB-INDEX-BTREE-PHYSICAL-VALIDATE-TREE-INVALID",
                                   "storage.index_btree_physical.validate_tree_invalid");
  }
  if (!IsTypedEngineIdentity(tree.index_uuid, UuidKind::object)) {
    return PhysicalValidationError("SB-INDEX-BTREE-PHYSICAL-VALIDATE-INDEX-UUID-INVALID",
                                   "storage.index_btree_physical.validate_index_uuid_invalid");
  }
  std::set<u64> image_pages;
  for (const IndexBtreePhysicalPageImage& image : tree.pages) {
    if (!image_pages.insert(image.page_number).second) {
      return PhysicalValidationError("SB-INDEX-BTREE-PHYSICAL-VALIDATE-DUPLICATE-PAGE",
                                     "storage.index_btree_physical.validate_duplicate_page",
                                     std::to_string(image.page_number));
    }
    auto parsed = ParseIndexBtreePageBody(image.serialized, image.page_number);
    if (!parsed.ok()) {
      return PhysicalValidationError(parsed.diagnostic.diagnostic_code,
                                     parsed.diagnostic.message_key,
                                     std::to_string(image.page_number));
    }
    if (parsed.body.index_uuid.value != tree.index_uuid.value) {
      return PhysicalValidationError("SB-INDEX-BTREE-PHYSICAL-VALIDATE-INDEX-MISMATCH",
                                     "storage.index_btree_physical.validate_index_mismatch",
                                     std::to_string(image.page_number));
    }
  }

  std::set<u64> reachable_pages;
  std::vector<IndexBtreePageBody> leaves;
  std::vector<IndexBtreeCell> live_entries;
  auto reachable = ValidateReachablePageUnlocked(tree,
                                                 tree.root_page_number,
                                                 0,
                                                 &reachable_pages,
                                                 &leaves,
                                                 &live_entries);
  if (!reachable.ok()) {
    return reachable;
  }

  for (std::size_t i = 0; i < leaves.size(); ++i) {
    const u64 expected_left = i == 0 ? 0 : leaves[i - 1].page_number;
    const u64 expected_right = i + 1 == leaves.size() ? 0 : leaves[i + 1].page_number;
    if (leaves[i].left_sibling_page_number != expected_left ||
        leaves[i].right_sibling_page_number != expected_right) {
      return PhysicalValidationError("SB-INDEX-BTREE-PHYSICAL-VALIDATE-SIBLING-MISMATCH",
                                     "storage.index_btree_physical.validate_sibling_mismatch",
                                     std::to_string(leaves[i].page_number));
    }
  }

  for (std::size_t i = 1; i < leaves.size(); ++i) {
    const auto previous_last = LastLiveCell(leaves[i - 1]);
    const auto current_first = FirstLiveCell(leaves[i]);
    if (previous_last.has_value() && current_first.has_value() &&
        CompareIndexBtreeCellsUnchecked(*previous_last, *current_first) > 0) {
      return PhysicalValidationError("SB-INDEX-BTREE-PHYSICAL-VALIDATE-LEAF-RANGE-PARTITION",
                                     "storage.index_btree_physical.validate_leaf_range_partition",
                                     std::to_string(leaves[i].page_number));
    }
  }

  for (std::size_t i = 1; i < live_entries.size(); ++i) {
    if (CompareIndexBtreeCellsUnchecked(live_entries[i - 1], live_entries[i]) > 0) {
      return PhysicalValidationError("SB-INDEX-BTREE-PHYSICAL-VALIDATE-GLOBAL-LEAF-ORDER",
                                     "storage.index_btree_physical.validate_global_leaf_order",
                                     std::to_string(i));
    }
  }

  std::vector<IndexBtreeCell> sorted_live_entries = live_entries;
  std::sort(sorted_live_entries.begin(),
            sorted_live_entries.end(),
            [](const IndexBtreeCell& left, const IndexBtreeCell& right) {
              return CompareIndexBtreeCellsUnchecked(left, right) < 0;
            });
  for (std::size_t i = 1; i < sorted_live_entries.size(); ++i) {
    if (SameLiveEntry(sorted_live_entries[i - 1], sorted_live_entries[i])) {
      return PhysicalValidationError("SB-INDEX-BTREE-PHYSICAL-VALIDATE-DUPLICATE-LIVE-ENTRY",
                                     "storage.index_btree_physical.validate_duplicate_live_entry",
                                     std::to_string(i));
    }
  }

  IndexBtreePhysicalTreeValidationResult result;
  result.status = IndexPageOkStatus();
  result.reachable_page_count = static_cast<u64>(reachable_pages.size());
  result.reachable_leaf_page_count = static_cast<u64>(leaves.size());
  result.live_entry_count = static_cast<u64>(live_entries.size());
  result.evidence = {"structural_validation=true",
                     "page_image_checksums_verified=true",
                     "page_numbers_verified=true",
                     "parent_child_fences_verified=true",
                     "sibling_order_verified=true",
                     "leaf_range_partition_verified=true",
                     "global_leaf_stream_order_verified=true",
                     "live_entries_no_duplicates=true",
                     "latch_authority=structural_only",
                     "visibility_authority=false",
                     "authorization_authority=false",
                     "transaction_finality_authority=false",
                     "recovery_authority=false"};
  result.evidence.push_back("reachable_page_count=" +
                            std::to_string(result.reachable_page_count));
  result.evidence.push_back("reachable_leaf_page_count=" +
                            std::to_string(result.reachable_leaf_page_count));
  result.evidence.push_back("live_entry_count=" +
                            std::to_string(result.live_entry_count));
  return result;
}

void PopulateIndexBtreeReportRows(IndexBtreePhysicalTreeReport* report) {
  report->support_bundle_rows.clear();
  report->support_bundle_rows.push_back("root_page_number=" +
                                        std::to_string(report->root_page_number));
  report->support_bundle_rows.push_back("page_count=" +
                                        std::to_string(report->page_count));
  report->support_bundle_rows.push_back("reachable_page_count=" +
                                        std::to_string(report->reachable_page_count));
  report->support_bundle_rows.push_back("reachable_leaf_count=" +
                                        std::to_string(report->reachable_leaf_count));
  report->support_bundle_rows.push_back("tuple_live_entry_estimate=" +
                                        std::to_string(report->tuple_live_entry_estimate));
  report->support_bundle_rows.push_back("tombstone_deleted_entry_count=" +
                                        std::to_string(report->tombstone_deleted_entry_count));
  report->support_bundle_rows.push_back("tree_height=" +
                                        std::to_string(report->tree_height));
  report->support_bundle_rows.push_back("page_size=" +
                                        std::to_string(report->page_size));
  report->support_bundle_rows.push_back("next_page_number=" +
                                        std::to_string(report->next_page_number));
  report->support_bundle_rows.push_back(std::string("valid=") +
                                        (report->valid ? "true" : "false"));
  report->support_bundle_rows.push_back(
      std::string("corruption_class=") +
      IndexBtreePhysicalCorruptionClassName(report->corruption_class));
  report->support_bundle_rows.push_back("exact_diagnostic_code=" +
                                        report->exact_diagnostic_code);
  report->support_bundle_rows.push_back("exact_diagnostic_message_key=" +
                                        report->exact_diagnostic_message_key);
  report->support_bundle_rows.push_back("visibility=false");
  report->support_bundle_rows.push_back("authorization=false");
  report->support_bundle_rows.push_back("transaction_finality=false");
  report->support_bundle_rows.push_back("recovery=false");
  report->support_bundle_rows.push_back("visibility_authority=false");
  report->support_bundle_rows.push_back("authorization_authority=false");
  report->support_bundle_rows.push_back("transaction_finality_authority=false");
  report->support_bundle_rows.push_back("recovery_authority=false");
}

IndexBtreePhysicalTreeReport BuildIndexBtreePhysicalTreeReportUnlocked(
    const IndexBtreePhysicalTree& tree) {
  IndexBtreePhysicalTreeReport report;
  report.root_page_number = tree.root_page_number;
  report.page_count = static_cast<u64>(tree.pages.size());
  report.page_size = tree.page_size;
  report.next_page_number = tree.next_page_number;
  report.status = IndexPageOkStatus();
  report.valid = true;
  report.corruption_class = IndexBtreePhysicalCorruptionClass::none;
  report.exact_diagnostic_code = "OK";
  report.exact_diagnostic_message_key = "storage.index_btree_physical.report.valid";

  if (tree.root_page_number == 0 || tree.page_size == 0) {
    report.status = IndexPageErrorStatus();
    report.diagnostic = MakeIndexBtreePageDiagnostic(
        report.status,
        "SB-INDEX-BTREE-PHYSICAL-REPORT-TREE-INVALID",
        "storage.index_btree_physical.report_tree_invalid");
    report.valid = false;
    report.corruption_class = IndexBtreePhysicalCorruptionClass::tree;
    report.exact_diagnostic_code = report.diagnostic.diagnostic_code;
    report.exact_diagnostic_message_key = report.diagnostic.message_key;
    report.evidence = {"structural_report=false"};
    AddStructuralNonAuthorityEvidence(&report.evidence);
    PopulateIndexBtreeReportRows(&report);
    return report;
  }

  auto root_fetch = FetchIndexBtreePhysicalPageUnlocked(tree, tree.root_page_number);
  if (root_fetch.ok()) {
    report.tree_height = static_cast<u64>(root_fetch.body.tree_level) + 1;
  }

  std::set<u64> reachable_pages;
  std::vector<IndexBtreePageBody> leaves;
  std::vector<IndexBtreeCell> live_entries;
  u64 reachable_tombstones = 0;
  auto reachable = CollectReachableContentUnlocked(tree,
                                                   tree.root_page_number,
                                                   0,
                                                   &reachable_pages,
                                                   &leaves,
                                                   &live_entries,
                                                   &reachable_tombstones);
  if (reachable.ok()) {
    report.reachable_page_count = static_cast<u64>(reachable_pages.size());
    report.reachable_leaf_count = static_cast<u64>(leaves.size());
    report.tuple_live_entry_estimate = static_cast<u64>(live_entries.size());
    report.tombstone_deleted_entry_count = reachable_tombstones;
  } else {
    report.status = reachable.status;
    report.diagnostic = reachable.diagnostic;
    report.valid = false;
    report.corruption_class =
        ClassifyIndexBtreeDiagnostic(reachable.diagnostic.diagnostic_code);
    report.exact_diagnostic_code = reachable.diagnostic.diagnostic_code;
    report.exact_diagnostic_message_key = reachable.diagnostic.message_key;
  }

  if (reachable.ok()) {
    std::optional<DiagnosticRecord> orphan_diagnostic;
    for (const IndexBtreePhysicalPageImage& image : tree.pages) {
      if (reachable_pages.find(image.page_number) != reachable_pages.end()) {
        continue;
      }
      auto parsed = ParseIndexBtreePageBody(image.serialized, image.page_number);
      if (!parsed.ok()) {
        orphan_diagnostic = parsed.diagnostic;
        break;
      }
    }
    if (orphan_diagnostic.has_value()) {
      report.status = IndexPageErrorStatus();
      report.diagnostic = MakeIndexBtreePageDiagnostic(
          report.status,
          "SB-INDEX-BTREE-PHYSICAL-REPORT-ORPHAN-STALE-PAGE-IMAGE",
          "storage.index_btree_physical.report_orphan_stale_page_image",
          orphan_diagnostic->diagnostic_code);
      report.valid = false;
      report.corruption_class =
          IndexBtreePhysicalCorruptionClass::orphan_stale_page_image;
      report.exact_diagnostic_code = report.diagnostic.diagnostic_code;
      report.exact_diagnostic_message_key = report.diagnostic.message_key;
    }
  }

  if (report.valid) {
    auto validation = ValidateIndexBtreePhysicalTreeUnlocked(tree);
    if (!validation.ok()) {
      report.status = validation.status;
      report.diagnostic = validation.diagnostic;
      report.valid = false;
      report.corruption_class =
          ClassifyIndexBtreeDiagnostic(validation.diagnostic.diagnostic_code);
      report.exact_diagnostic_code = validation.diagnostic.diagnostic_code;
      report.exact_diagnostic_message_key = validation.diagnostic.message_key;
    }
  }

  report.evidence.push_back(std::string("structural_report=") +
                            (report.valid ? "true" : "false"));
  report.evidence.push_back("support_bundle_rows=true");
  report.evidence.push_back(std::string("corruption_class=") +
                            IndexBtreePhysicalCorruptionClassName(
                                report.corruption_class));
  AddStructuralNonAuthorityEvidence(&report.evidence);
  PopulateIndexBtreeReportRows(&report);
  return report;
}

IndexBtreePhysicalTreeRebuildResult RebuildIndexBtreePhysicalTreeFromLiveCells(
    const IndexBtreePhysicalTree& source,
    const std::vector<IndexBtreeCell>& live_entries,
    const char* reason) {
  auto init = InitializeIndexBtreePhysicalTree(source.index_uuid, source.page_size);
  if (!init.ok()) {
    return PhysicalRebuildError(init.diagnostic.diagnostic_code,
                                init.diagnostic.message_key,
                                "rebuild_initialize_failed");
  }
  IndexBtreePhysicalTree rebuilt = std::move(init.tree);
  std::vector<IndexBtreeCell> sorted_live_entries = live_entries;
  std::sort(sorted_live_entries.begin(),
            sorted_live_entries.end(),
            [](const IndexBtreeCell& left, const IndexBtreeCell& right) {
              return CompareIndexBtreeCellsUnchecked(left, right) < 0;
            });
  for (std::size_t i = 1; i < sorted_live_entries.size(); ++i) {
    if (SameLiveEntry(sorted_live_entries[i - 1], sorted_live_entries[i])) {
      return PhysicalRebuildError(
          "SB-INDEX-BTREE-PHYSICAL-REBUILD-DUPLICATE-LIVE-ENTRY",
          "storage.index_btree_physical.rebuild_duplicate_live_entry",
          std::to_string(i));
    }
  }
  for (IndexBtreeCell cell : sorted_live_entries) {
    cell.high_key = false;
    cell.deleted = false;
    cell.child_page_number = 0;
    IndexBtreePhysicalInsertRequest request;
    request.cell = std::move(cell);
    auto inserted = InsertIndexBtreeCell(&rebuilt, request);
    if (!inserted.ok()) {
      return PhysicalRebuildError(inserted.diagnostic.diagnostic_code,
                                  inserted.diagnostic.message_key,
                                  "rebuild_insert_failed");
    }
  }

  auto validation = ValidateIndexBtreePhysicalTreeUnlocked(rebuilt);
  if (!validation.ok()) {
    return PhysicalRebuildError(validation.diagnostic.diagnostic_code,
                                validation.diagnostic.message_key,
                                "rebuild_validation_failed");
  }
  auto image_result = ExportIndexBtreePhysicalTreeImage(rebuilt);
  if (!image_result.ok()) {
    return PhysicalRebuildError(image_result.diagnostic.diagnostic_code,
                                image_result.diagnostic.message_key,
                                "rebuild_export_failed");
  }

  IndexBtreePhysicalTreeRebuildResult result;
  result.status = IndexPageOkStatus();
  result.tree = std::move(rebuilt);
  result.image = std::move(image_result.image);
  result.report = BuildIndexBtreePhysicalTreeReportUnlocked(result.tree);
  result.rebuilt = true;
  result.evidence = {"structural_rebuild=true",
                     std::string("rebuild_reason=") + reason,
                     "reachable_live_cells_preserved=true",
                     "encoded_keys_preserved=true",
                     "row_version_uuids_preserved=true",
                     "fresh_tree_image_built=true"};
  result.evidence.push_back("source_page_count=" +
                            std::to_string(source.pages.size()));
  result.evidence.push_back("rebuilt_page_count=" +
                            std::to_string(result.tree.pages.size()));
  result.evidence.push_back("rebuilt_root_page_number=" +
                            std::to_string(result.tree.root_page_number));
  AddStructuralNonAuthorityEvidence(&result.evidence);
  return result;
}

IndexBtreePhysicalTreeValidationResult CollectRepairLiveEntriesUnlocked(
    const IndexBtreePhysicalTree& tree,
    std::set<u64>* reachable_pages,
    std::vector<IndexBtreeCell>* live_entries,
    u64* tombstone_count) {
  std::vector<IndexBtreePageBody> leaves;
  return CollectReachableContentUnlocked(tree,
                                         tree.root_page_number,
                                         0,
                                         reachable_pages,
                                         &leaves,
                                         live_entries,
                                         tombstone_count);
}

bool SameRowUuid(const TypedUuid& left, const TypedUuid& right) {
  return left.valid() && right.valid() &&
         CompareUuid128(left.value, right.value) == 0;
}

bool SameVersionUuid(const TypedUuid& left, const TypedUuid& right) {
  return left.valid() && right.valid() &&
         CompareUuid128(left.value, right.value) == 0;
}

bool UniqueSameRowProofValid(const IndexBtreePhysicalUniqueInsertRequest& request) {
  return request.allow_same_row_update &&
         request.same_row_proof_uuid.valid() &&
         SameRowUuid(request.same_row_proof_uuid, request.cell.row_uuid);
}

IndexBtreePhysicalUniqueConflictCandidate UniqueCandidateFromCell(
    const IndexBtreeCell& existing,
    const IndexBtreeCell& incoming,
    u64 leaf_page_number,
    u32 cell_ordinal) {
  IndexBtreePhysicalUniqueConflictCandidate candidate;
  candidate.encoded_key = existing.encoded_key;
  candidate.row_uuid = existing.row_uuid;
  candidate.version_uuid = existing.version_uuid;
  candidate.leaf_page_number = leaf_page_number;
  candidate.cell_ordinal = cell_ordinal;
  candidate.same_key_identity = true;
  candidate.same_row = SameRowUuid(existing.row_uuid, incoming.row_uuid);
  candidate.exact_live_entry =
      candidate.same_row && SameVersionUuid(existing.version_uuid, incoming.version_uuid);
  return candidate;
}

IndexBtreePhysicalUniqueInsertResult CollectUniqueConflictCandidatesUnlocked(
    const IndexBtreePhysicalTree& tree,
    const IndexBtreePhysicalUniqueInsertRequest& request,
    std::vector<IndexBtreePhysicalUniqueConflictCandidate>* candidates) {
  IndexBtreePhysicalScanRequest scan_request;
  scan_request.mode = IndexBtreePhysicalScanMode::point;
  scan_request.point_key = request.cell.encoded_key;

  std::vector<IndexBtreePageBody> leaves;
  std::vector<u64> visited_pages;
  ScanTraversalAccounting accounting;
  auto collected = CollectPrunedScanLeaves(tree,
                                           scan_request,
                                           tree.root_page_number,
                                           0,
                                           &visited_pages,
                                           &leaves,
                                           &accounting);
  if (!collected.ok()) {
    return PhysicalUniqueInsertError(collected.diagnostic.diagnostic_code,
                                     collected.diagnostic.message_key,
                                     "unique_probe_collect_failed");
  }
  for (const IndexBtreePageBody& leaf : leaves) {
    for (std::size_t i = 0; i < leaf.cells.size(); ++i) {
      const IndexBtreeCell& cell = leaf.cells[i];
      if (cell.deleted || cell.high_key) {
        continue;
      }
      if (CompareEncodedKeysOnly(cell.encoded_key, request.cell.encoded_key) == 0) {
        candidates->push_back(UniqueCandidateFromCell(cell,
                                                      request.cell,
                                                      leaf.page_number,
                                                      static_cast<u32>(i)));
      }
    }
  }
  IndexBtreePhysicalUniqueInsertResult result;
  result.status = IndexPageOkStatus();
  result.evidence.push_back("unique_probe_reachable_root_descent=true");
  result.evidence.push_back("unique_probe_same_encoded_key_identity=true");
  result.evidence.push_back("unique_probe_compares_row_uuid=false");
  result.evidence.push_back("unique_probe_compares_version_uuid=false");
  result.evidence.push_back("unique_probe_visited_leaf_pages=" +
                            std::to_string(accounting.visited_leaf_pages));
  result.evidence.push_back("unique_probe_pruned_leaf_pages=" +
                            std::to_string(accounting.pruned_leaf_pages));
  return result;
}

}  // namespace

IndexBtreePhysicalTreeResult InitializeIndexBtreePhysicalTree(TypedUuid index_uuid, u32 page_size) {
  if (!IsTypedEngineIdentity(index_uuid, UuidKind::object)) {
    return PhysicalTreeError("SB-INDEX-BTREE-PHYSICAL-INDEX-UUID-MUST-BE-V7",
                             "storage.index_btree_physical.index_uuid_must_be_v7");
  }
  IndexBtreePhysicalTree tree;
  tree.page_size = page_size;
  tree.index_uuid = index_uuid;
  tree.root_page_number = 1;
  tree.next_page_number = 2;

  IndexBtreePageBody root;
  root.index_uuid = index_uuid;
  root.page_number = tree.root_page_number;
  root.page_kind = IndexBtreePageKind::root;
  root.tree_level = 0;
  const auto staged_root = StagePhysicalPage(root, page_size);
  if (!staged_root.ok()) {
    return PhysicalTreeError(staged_root.diagnostic.diagnostic_code,
                             staged_root.diagnostic.message_key);
  }
  PublishStagedPage(&tree, staged_root);

  IndexBtreePhysicalTreeResult result;
  result.status = IndexPageOkStatus();
  result.tree = std::move(tree);
  result.evidence = {"root_page_initialized=true",
                     "latch_model=address_keyed_shared_mutex",
                     "latch_authority=structural_only",
                     "visibility_authority=false",
                     "authorization_authority=false",
                     "transaction_finality_authority=false",
                     "recovery_authority=false"};
  return result;
}

IndexBtreePhysicalTreeValidationResult ValidateIndexBtreePhysicalTree(
    const IndexBtreePhysicalTree& tree) {
  SharedTreeLatch latch(&tree);
  auto result = ValidateIndexBtreePhysicalTreeUnlocked(tree);
  if (result.ok()) {
    result.evidence.push_back("shared_latch_acquired=true");
    result.evidence.push_back("concurrent_reader_safe=true");
  }
  return result;
}

IndexBtreePhysicalTreeReportResult BuildIndexBtreePhysicalTreeReport(
    const IndexBtreePhysicalTree& tree) {
  SharedTreeLatch latch(&tree);
  IndexBtreePhysicalTreeReportResult result;
  result.report = BuildIndexBtreePhysicalTreeReportUnlocked(tree);
  result.status = result.report.status;
  result.diagnostic = result.report.diagnostic;
  result.evidence = result.report.evidence;
  if (result.report.valid) {
    result.evidence.push_back("shared_latch_acquired=true");
  }
  return result;
}

IndexBtreePhysicalTreeRebuildResult RebuildIndexBtreePhysicalTree(
    const IndexBtreePhysicalTree& tree) {
  SharedTreeLatch latch(&tree);
  auto validation = ValidateIndexBtreePhysicalTreeUnlocked(tree);
  if (!validation.ok()) {
    return PhysicalRebuildError(validation.diagnostic.diagnostic_code,
                                validation.diagnostic.message_key,
                                "source_validation_failed");
  }
  std::set<u64> reachable_pages;
  std::vector<IndexBtreeCell> live_entries;
  u64 tombstone_count = 0;
  auto collected = CollectRepairLiveEntriesUnlocked(tree,
                                                    &reachable_pages,
                                                    &live_entries,
                                                    &tombstone_count);
  if (!collected.ok()) {
    return PhysicalRebuildError(collected.diagnostic.diagnostic_code,
                                collected.diagnostic.message_key,
                                "source_collection_failed");
  }
  return RebuildIndexBtreePhysicalTreeFromLiveCells(tree, live_entries, "explicit_rebuild");
}

IndexBtreePhysicalBulkBuildResult BuildIndexBtreePhysicalBulkLoadedTree(
    const IndexBtreePhysicalBulkBuildRequest& request) {
  if (!IsTypedEngineIdentity(request.index_uuid, UuidKind::object)) {
    return PhysicalBulkBuildError(
        "SB-INDEX-BTREE-BULK-BUILD-INDEX-UUID-MUST-BE-V7",
        "storage.index_btree_bulk_build.index_uuid_must_be_v7");
  }
  if (!request.sorted_order_proof_valid) {
    return PhysicalBulkBuildError(
        "SB-INDEX-BTREE-BULK-BUILD-ORDER-PROOF-INVALID",
        "storage.index_btree_bulk_build.order_proof_invalid");
  }
  if (request.page_size <=
      kPageHeaderSerializedBytes + kIndexBtreePageBodyHeaderBytes) {
    return PhysicalBulkBuildError(
        "SB-INDEX-BTREE-BULK-BUILD-PAGE-SIZE-TOO-SMALL",
        "storage.index_btree_bulk_build.page_size_too_small",
        std::to_string(request.page_size));
  }
  const u32 leaf_capacity =
      request.leaf_entry_capacity == 0 ? 128 : request.leaf_entry_capacity;
  const u32 internal_capacity =
      request.internal_entry_capacity == 0 ? leaf_capacity
                                           : request.internal_entry_capacity;
  if (leaf_capacity == 0 || internal_capacity == 0) {
    return PhysicalBulkBuildError(
        "SB-INDEX-BTREE-BULK-BUILD-CAPACITY-INVALID",
        "storage.index_btree_bulk_build.capacity_invalid");
  }

  for (std::size_t i = 0; i < request.sorted_cells.size(); ++i) {
    const auto& cell = request.sorted_cells[i];
    if (cell.high_key || cell.deleted || cell.child_page_number != 0) {
      return PhysicalBulkBuildError(
          "SB-INDEX-BTREE-BULK-BUILD-LEAF-CELL-INVALID",
          "storage.index_btree_bulk_build.leaf_cell_invalid",
          std::to_string(i));
    }
    if (i > 0 &&
        CompareIndexBtreeCellsUnchecked(request.sorted_cells[i - 1], cell) > 0) {
      return PhysicalBulkBuildError(
          "SB-INDEX-BTREE-BULK-BUILD-CELLS-UNSORTED",
          "storage.index_btree_bulk_build.cells_unsorted",
          std::to_string(i));
    }
  }

  std::vector<IndexBtreePageBody> pages;
  std::vector<std::size_t> current_level;
  u64 next_page_number = 1;

  if (request.sorted_cells.empty()) {
    IndexBtreePageBody root;
    root.index_uuid = request.index_uuid;
    root.page_number = next_page_number++;
    root.page_kind = IndexBtreePageKind::root;
    root.tree_level = 0;
    pages.push_back(std::move(root));
    return SerializeBulkBuiltPages(std::move(pages),
                                   1,
                                   next_page_number,
                                   request.page_size,
                                   request.index_uuid,
                                   1,
                                   0);
  }

  IndexBtreePageBody leaf;
  leaf.index_uuid = request.index_uuid;
  leaf.page_number = next_page_number++;
  leaf.page_kind = IndexBtreePageKind::leaf;
  leaf.tree_level = 0;
  for (const auto& cell : request.sorted_cells) {
    if (!BulkPageAcceptsCell(leaf, cell, request.page_size, leaf_capacity)) {
      if (leaf.cells.empty()) {
        return PhysicalBulkBuildError(
            "SB-INDEX-BTREE-BULK-BUILD-LEAF-CELL-OVERFLOW",
            "storage.index_btree_bulk_build.leaf_cell_overflow");
      }
      pages.push_back(std::move(leaf));
      current_level.push_back(pages.size() - 1);
      leaf = {};
      leaf.index_uuid = request.index_uuid;
      leaf.page_number = next_page_number++;
      leaf.page_kind = IndexBtreePageKind::leaf;
      leaf.tree_level = 0;
      if (!BulkPageAcceptsCell(leaf, cell, request.page_size, leaf_capacity)) {
        return PhysicalBulkBuildError(
            "SB-INDEX-BTREE-BULK-BUILD-LEAF-CELL-OVERFLOW",
            "storage.index_btree_bulk_build.leaf_cell_overflow");
      }
    }
    leaf.cells.push_back(cell);
  }
  pages.push_back(std::move(leaf));
  current_level.push_back(pages.size() - 1);

  for (std::size_t i = 0; i < current_level.size(); ++i) {
    auto& page = pages[current_level[i]];
    page.left_sibling_page_number =
        i == 0 ? 0 : pages[current_level[i - 1]].page_number;
    page.right_sibling_page_number =
        i + 1 == current_level.size() ? 0 : pages[current_level[i + 1]].page_number;
  }

  const u64 leaf_page_count = static_cast<u64>(current_level.size());
  u64 branch_level_count = 0;
  while (current_level.size() > 1) {
    const u16 parent_level =
        static_cast<u16>(pages[current_level.front()].tree_level + 1);
    std::vector<std::size_t> parent_level_indices;
    IndexBtreePageBody parent;
    parent.index_uuid = request.index_uuid;
    parent.page_number = next_page_number++;
    parent.page_kind = IndexBtreePageKind::internal;
    parent.tree_level = parent_level;

    for (const auto child_index : current_level) {
      IndexBtreeCell fence = FenceCellForPage(pages[child_index]);
      if (fence.encoded_key.empty() || fence.child_page_number == 0) {
        return PhysicalBulkBuildError(
            "SB-INDEX-BTREE-BULK-BUILD-FENCE-CELL-INVALID",
            "storage.index_btree_bulk_build.fence_cell_invalid",
            std::to_string(pages[child_index].page_number));
      }
      if (!BulkPageAcceptsCell(parent,
                               fence,
                               request.page_size,
                               internal_capacity)) {
        if (parent.cells.empty()) {
          return PhysicalBulkBuildError(
              "SB-INDEX-BTREE-BULK-BUILD-INTERNAL-CELL-OVERFLOW",
              "storage.index_btree_bulk_build.internal_cell_overflow");
        }
        pages.push_back(std::move(parent));
        parent_level_indices.push_back(pages.size() - 1);
        parent = {};
        parent.index_uuid = request.index_uuid;
        parent.page_number = next_page_number++;
        parent.page_kind = IndexBtreePageKind::internal;
        parent.tree_level = parent_level;
        if (!BulkPageAcceptsCell(parent,
                                 fence,
                                 request.page_size,
                                 internal_capacity)) {
          return PhysicalBulkBuildError(
              "SB-INDEX-BTREE-BULK-BUILD-INTERNAL-CELL-OVERFLOW",
              "storage.index_btree_bulk_build.internal_cell_overflow");
        }
      }
      pages[child_index].parent_page_number = parent.page_number;
      parent.cells.push_back(std::move(fence));
    }

    pages.push_back(std::move(parent));
    parent_level_indices.push_back(pages.size() - 1);
    for (std::size_t i = 0; i < parent_level_indices.size(); ++i) {
      auto& page = pages[parent_level_indices[i]];
      page.left_sibling_page_number =
          i == 0 ? 0 : pages[parent_level_indices[i - 1]].page_number;
      page.right_sibling_page_number =
          i + 1 == parent_level_indices.size()
              ? 0
              : pages[parent_level_indices[i + 1]].page_number;
    }
    current_level = std::move(parent_level_indices);
    ++branch_level_count;
  }

  const std::size_t root_index = current_level.front();
  pages[root_index].page_kind = IndexBtreePageKind::root;
  pages[root_index].parent_page_number = 0;
  pages[root_index].left_sibling_page_number = 0;
  pages[root_index].right_sibling_page_number = 0;
  const u64 root_page_number = pages[root_index].page_number;

  return SerializeBulkBuiltPages(std::move(pages),
                                 root_page_number,
                                 next_page_number,
                                 request.page_size,
                                 request.index_uuid,
                                 leaf_page_count,
                                 branch_level_count);
}

IndexBtreePhysicalTreeRepairResult RepairIndexBtreePhysicalTree(
    const IndexBtreePhysicalTree& tree) {
  SharedTreeLatch latch(&tree);
  IndexBtreePhysicalTreeRepairResult result;
  result.before_report = BuildIndexBtreePhysicalTreeReportUnlocked(tree);
  result.corruption_class = result.before_report.corruption_class;

  const bool has_stale_orphan_pages =
      result.before_report.reachable_page_count < result.before_report.page_count;
  const bool safe_orphan_corruption =
      result.before_report.corruption_class ==
      IndexBtreePhysicalCorruptionClass::orphan_stale_page_image;
  if (result.before_report.valid && !has_stale_orphan_pages) {
    result.status = IndexPageOkStatus();
    result.tree = tree;
    result.image.page_size = tree.page_size;
    result.image.index_uuid = tree.index_uuid;
    result.image.root_page_number = tree.root_page_number;
    result.image.next_page_number = tree.next_page_number;
    result.image.pages = tree.pages;
    result.image.evidence = {"serialized_tree_image_exported=true",
                             "structural_repair=false",
                             "visibility=false",
                             "authorization=false",
                             "transaction_finality=false",
                             "recovery=false",
                             "visibility_authority=false",
                             "authorization_authority=false",
                             "transaction_finality_authority=false",
                             "recovery_authority=false"};
    result.after_report = result.before_report;
    result.evidence = {"structural_repair=false",
                       "repair_needed=false",
                       "repair_refused=false"};
    AddStructuralNonAuthorityEvidence(&result.evidence);
    return result;
  }

  if (!result.before_report.valid && !safe_orphan_corruption) {
    auto refused = PhysicalRepairError(
        result.before_report.exact_diagnostic_code,
        result.before_report.exact_diagnostic_message_key,
        result.before_report.corruption_class,
        "unsafe_structural_repair_refused");
    refused.before_report = result.before_report;
    return refused;
  }

  std::set<u64> reachable_pages;
  std::vector<IndexBtreeCell> live_entries;
  u64 tombstone_count = 0;
  auto collected = CollectRepairLiveEntriesUnlocked(tree,
                                                    &reachable_pages,
                                                    &live_entries,
                                                    &tombstone_count);
  if (!collected.ok()) {
    auto refused = PhysicalRepairError(collected.diagnostic.diagnostic_code,
                                       collected.diagnostic.message_key,
                                       ClassifyIndexBtreeDiagnostic(
                                           collected.diagnostic.diagnostic_code),
                                       "reachable_live_cell_collection_failed");
    refused.before_report = result.before_report;
    return refused;
  }

  auto rebuilt = RebuildIndexBtreePhysicalTreeFromLiveCells(
      tree,
      live_entries,
      has_stale_orphan_pages ? "orphan_stale_page_image" : "safe_repair");
  if (!rebuilt.ok()) {
    auto refused = PhysicalRepairError(rebuilt.diagnostic.diagnostic_code,
                                       rebuilt.diagnostic.message_key,
                                       ClassifyIndexBtreeDiagnostic(
                                           rebuilt.diagnostic.diagnostic_code),
                                       "rebuild_failed");
    refused.before_report = result.before_report;
    return refused;
  }

  result.status = IndexPageOkStatus();
  result.tree = std::move(rebuilt.tree);
  result.image = std::move(rebuilt.image);
  result.after_report = BuildIndexBtreePhysicalTreeReportUnlocked(result.tree);
  result.repaired = true;
  result.refused = false;
  result.evidence = {"structural_repair=true",
                     "repair_refused=false",
                     "reachable_live_cells_rebuilt=true",
                     "encoded_keys_preserved=true",
                     "row_version_uuids_preserved=true"};
  if (has_stale_orphan_pages) {
    result.evidence.push_back("orphan_stale_page_images_removed=true");
  }
  result.evidence.push_back("before_page_count=" +
                            std::to_string(result.before_report.page_count));
  result.evidence.push_back("after_page_count=" +
                            std::to_string(result.after_report.page_count));
  result.evidence.push_back("before_reachable_page_count=" +
                            std::to_string(result.before_report.reachable_page_count));
  result.evidence.push_back("after_reachable_page_count=" +
                            std::to_string(result.after_report.reachable_page_count));
  AddStructuralNonAuthorityEvidence(&result.evidence);
  return result;
}

IndexBtreePhysicalTreeImageResult ExportIndexBtreePhysicalTreeImage(
    const IndexBtreePhysicalTree& tree) {
  SharedTreeLatch latch(&tree);
  auto validation = ValidateIndexBtreePhysicalTreeUnlocked(tree);
  if (!validation.ok()) {
    return PhysicalImageError(validation.diagnostic.diagnostic_code,
                              validation.diagnostic.message_key,
                              "export_validation_failed");
  }
  IndexBtreePhysicalTreeImageResult result;
  result.status = IndexPageOkStatus();
  result.image.page_size = tree.page_size;
  result.image.index_uuid = tree.index_uuid;
  result.image.root_page_number = tree.root_page_number;
  result.image.next_page_number = tree.next_page_number;
  result.image.pages = tree.pages;
  result.image.evidence = {"serialized_tree_image_exported=true",
                           "shared_latch_acquired=true",
                           "page_image_checksums_verified=true",
                           "latch_authority=structural_only",
                           "visibility_authority=false",
                           "authorization_authority=false",
                           "transaction_finality_authority=false",
                           "recovery_authority=false"};
  result.evidence = result.image.evidence;
  return result;
}

IndexBtreePhysicalTreeResult ImportIndexBtreePhysicalTreeImage(
    const IndexBtreePhysicalTreeImage& image) {
  if (image.page_size == 0 || image.root_page_number == 0 || image.pages.empty()) {
    return PhysicalTreeError("SB-INDEX-BTREE-PHYSICAL-IMAGE-INVALID",
                             "storage.index_btree_physical.image_invalid");
  }
  IndexBtreePhysicalTree tree;
  tree.page_size = image.page_size;
  tree.index_uuid = image.index_uuid;
  tree.root_page_number = image.root_page_number;
  tree.next_page_number = image.next_page_number;
  tree.pages = image.pages;
  auto validation = ValidateIndexBtreePhysicalTreeUnlocked(tree);
  if (!validation.ok()) {
    return PhysicalTreeError(validation.diagnostic.diagnostic_code,
                             validation.diagnostic.message_key,
                             "import_validation_failed");
  }
  IndexBtreePhysicalTreeResult result;
  result.status = IndexPageOkStatus();
  result.tree = std::move(tree);
  result.evidence = validation.evidence;
  result.evidence.push_back("serialized_tree_image_imported=true");
  result.evidence.push_back("crash_reopen_style_validation=true");
  return result;
}

IndexBtreePageBodyResult FetchIndexBtreePhysicalPage(const IndexBtreePhysicalTree& tree,
                                                     u64 page_number) {
  SharedTreeLatch latch(&tree);
  return FetchIndexBtreePhysicalPageUnlocked(tree, page_number);
}

static IndexBtreePhysicalInsertResult InsertIndexBtreeCellUnlocked(
    IndexBtreePhysicalTree* tree,
    const IndexBtreePhysicalInsertRequest& request) {
  if (tree == nullptr) {
    return PhysicalInsertError("SB-INDEX-BTREE-PHYSICAL-TREE-INVALID",
                               "storage.index_btree_physical.tree_invalid");
  }
  if (tree->root_page_number == 0) {
    return PhysicalInsertError("SB-INDEX-BTREE-PHYSICAL-TREE-INVALID",
                               "storage.index_btree_physical.tree_invalid");
  }
  IndexBtreePageBody request_probe;
  request_probe.index_uuid = tree->index_uuid;
  request_probe.page_number = tree->root_page_number;
  request_probe.page_kind = IndexBtreePageKind::root;
  request_probe.tree_level = 0;
  request_probe.cells = {request.cell};
  const auto request_validation = ValidatePhysicalPageBody(request_probe);
  if (!request_validation.ok()) {
    return FromPageError(request_validation);
  }

  std::vector<u64> path;
  auto current = FetchIndexBtreePhysicalPageUnlocked(*tree, tree->root_page_number);
  if (!current.ok()) {
    return FromPageError(current);
  }
  while (IsInternalLike(current.body)) {
    path.push_back(current.body.page_number);
    const u64 child_page_number = SelectChildPage(current.body, request.cell);
    if (child_page_number == 0) {
      return PhysicalInsertError("SB-INDEX-BTREE-PHYSICAL-CHILD-MISSING",
                                 "storage.index_btree_physical.child_missing",
                                 std::to_string(current.body.page_number));
    }
    current = FetchIndexBtreePhysicalPageUnlocked(*tree, child_page_number);
    if (!current.ok()) {
      return FromPageError(current);
    }
  }

  IndexBtreePageBody leaf = std::move(current.body);
  const auto descent_validation =
      ValidateDescentPathUnlocked(*tree, path, leaf, request.cell);
  if (!descent_validation.ok()) {
    return FromPageError(descent_validation);
  }
  std::vector<IndexBtreeCell> cells = leaf.cells;
  InsertCellSorted(&cells, request.cell);
  IndexBtreePageBody candidate = leaf;
  candidate.cells = cells;
  auto staged_candidate = StagePhysicalPage(candidate, tree->page_size);
  if (staged_candidate.ok()) {
    PublishStagedPage(tree, staged_candidate);
    IndexBtreePhysicalInsertResult result;
    result.status = IndexPageOkStatus();
    result.inserted = true;
    result.root_page_number = tree->root_page_number;
    result.leaf_page_number = candidate.page_number;
    result.evidence = {"insert_sorted=true",
                       "exclusive_latch_acquired=true",
                       "optimistic_descent_validated=true",
                       "latch_authority=structural_only",
                       "visibility_authority=false",
                       "authorization_authority=false",
                       "transaction_finality_authority=false",
                       "recovery_authority=false"};
    return UpdateParentFence(tree, path, candidate, std::move(result));
  }
  if (staged_candidate.diagnostic.diagnostic_code != "SB-INDEX-BTREE-PAGE-BODY-TOO-LARGE") {
    return FromPageError(staged_candidate);
  }
  if (HasDeletedCells(leaf)) {
    IndexBtreePageBody compacted = leaf;
    const u32 removed = RemoveDeletedCells(&compacted);
    auto staged_compacted = StagePhysicalPage(compacted, tree->page_size);
    if (!staged_compacted.ok()) {
      return FromPageError(staged_compacted);
    }
    PublishStagedPage(tree, staged_compacted);

    std::vector<IndexBtreeCell> compacted_cells = compacted.cells;
    InsertCellSorted(&compacted_cells, request.cell);
    IndexBtreePageBody compacted_candidate = compacted;
    compacted_candidate.cells = std::move(compacted_cells);
    auto staged_compacted_candidate = StagePhysicalPage(compacted_candidate, tree->page_size);
    if (staged_compacted_candidate.ok()) {
      PublishStagedPage(tree, staged_compacted_candidate);
      IndexBtreePhysicalInsertResult result;
      result.status = IndexPageOkStatus();
      result.inserted = true;
      result.root_page_number = tree->root_page_number;
      result.leaf_page_number = compacted_candidate.page_number;
      result.evidence = {"insert_sorted=true",
                         "exclusive_latch_acquired=true",
                         "optimistic_descent_validated=true",
                         "latch_authority=structural_only",
                         "bottom_up_cleanup_before_split=true",
                         "tombstone_cleanup_compacted=" + std::to_string(removed),
                         "free_space_reused_before_split=true",
                         "visibility_authority=false",
                         "authorization_authority=false",
                         "transaction_finality_authority=false",
                         "recovery_authority=false"};
      return UpdateParentFence(tree, path, compacted_candidate, std::move(result));
    }
    if (staged_compacted_candidate.diagnostic.diagnostic_code !=
        "SB-INDEX-BTREE-PAGE-BODY-TOO-LARGE") {
      return FromPageError(staged_compacted_candidate);
    }
    leaf = std::move(compacted);
    cells = leaf.cells;
    InsertCellSorted(&cells, request.cell);
    candidate = leaf;
    candidate.cells = cells;
  }
  if (candidate.cells.size() < 2) {
    return PhysicalInsertError("SB-INDEX-BTREE-PHYSICAL-CELL-TOO-LARGE",
                               "storage.index_btree_physical.cell_too_large",
                               staged_candidate.diagnostic.diagnostic_code);
  }
  if (candidate.page_kind == IndexBtreePageKind::root && candidate.tree_level == 0) {
    return SplitRootLeaf(tree, candidate, std::move(cells));
  }
  return SplitNonRootLeaf(tree, path, candidate, std::move(cells));
}

IndexBtreePhysicalInsertResult InsertIndexBtreeCell(
    IndexBtreePhysicalTree* tree,
    const IndexBtreePhysicalInsertRequest& request) {
  if (tree == nullptr) {
    return PhysicalInsertError("SB-INDEX-BTREE-PHYSICAL-TREE-INVALID",
                               "storage.index_btree_physical.tree_invalid");
  }
  UniqueTreeLatch latch(tree);
  return InsertIndexBtreeCellUnlocked(tree, request);
}

IndexBtreePhysicalUniqueInsertResult InsertUniqueIndexBtreeCell(
    IndexBtreePhysicalTree* tree,
    const IndexBtreePhysicalUniqueInsertRequest& request) {
  if (tree == nullptr) {
    return PhysicalUniqueInsertError("SB-INDEX-BTREE-PHYSICAL-TREE-INVALID",
                                     "storage.index_btree_physical.tree_invalid");
  }
  UniqueTreeLatch latch(tree);
  if (tree->root_page_number == 0) {
    return PhysicalUniqueInsertError("SB-INDEX-BTREE-PHYSICAL-TREE-INVALID",
                                     "storage.index_btree_physical.tree_invalid");
  }

  IndexBtreePhysicalUniqueInsertResult result;
  result.status = IndexPageOkStatus();
  result.evidence = {"unique_atomic_probe_insert=true",
                     "exclusive_latch_acquired=true",
                     "atomic_conflict_probe_insert_latch=structural_exclusive",
                     "latch_authority=structural_only",
                     "visibility_authority=false",
                     "authorization_authority=false",
                     "transaction_finality_authority=false",
                     "recovery_authority=false"};

  if (!request.partial_predicate_participates) {
    result.bypassed_partial_predicate = true;
    result.evidence.push_back("partial_predicate_participates=false");
    result.evidence.push_back("partial_predicate_physical_insert_bypassed=true");
    return result;
  }
  result.evidence.push_back("partial_predicate_participates=true");

  IndexBtreeCell validation_cell = request.cell;
  validation_cell.high_key = false;
  validation_cell.deleted = false;
  validation_cell.child_page_number = 0;
  IndexBtreePageBody request_probe;
  request_probe.index_uuid = tree->index_uuid;
  request_probe.page_number = tree->root_page_number;
  request_probe.page_kind = IndexBtreePageKind::root;
  request_probe.tree_level = 0;
  request_probe.cells = {validation_cell};
  const auto request_validation = ValidatePhysicalPageBody(request_probe);
  if (!request_validation.ok()) {
    return FromPageUniqueInsertError(request_validation);
  }

  const bool nulls_distinct =
      request.null_policy == IndexBtreePhysicalUniqueNullPolicy::nulls_distinct;
  const bool null_exempt = request.incoming_key_has_null && nulls_distinct;
  result.evidence.push_back(std::string("unique_null_policy=") +
                            (nulls_distinct ? "nulls_distinct" : "nulls_not_distinct"));
  result.evidence.push_back(std::string("incoming_key_has_null=") +
                            (request.incoming_key_has_null ? "true" : "false"));
  if (!null_exempt) {
    std::vector<IndexBtreePhysicalUniqueConflictCandidate> candidates;
    auto probe = CollectUniqueConflictCandidatesUnlocked(*tree, request, &candidates);
    if (!probe.ok()) {
      return probe;
    }
    result.evidence.insert(result.evidence.end(), probe.evidence.begin(), probe.evidence.end());
    result.conflict_candidates = std::move(candidates);
  } else {
    result.null_exempt_from_conflict = true;
    result.evidence.push_back("unique_null_exempt_from_conflict=true");
    result.evidence.push_back("unique_probe_skipped_for_nulls_distinct=true");
  }

  if (!result.conflict_candidates.empty()) {
    AddCandidateEvidence(&result);
    result.conflict = true;
    result.evidence.push_back("unique_conflict_candidate_count=" +
                              std::to_string(result.conflict_candidates.size()));
    result.evidence.push_back("unique_conflict_candidates_require_mga_security_recheck=true");
    result.evidence.push_back("unique_conflict_candidate_only=true");
    const bool proof_valid = UniqueSameRowProofValid(request);
    const bool all_same_row =
        std::all_of(result.conflict_candidates.begin(),
                    result.conflict_candidates.end(),
                    [](const IndexBtreePhysicalUniqueConflictCandidate& candidate) {
                      return candidate.same_row;
                    });
    const bool has_exact_live_entry =
        std::any_of(result.conflict_candidates.begin(),
                    result.conflict_candidates.end(),
                    [](const IndexBtreePhysicalUniqueConflictCandidate& candidate) {
                      return candidate.exact_live_entry;
                    });
    if (proof_valid && all_same_row && !has_exact_live_entry) {
      result.conflict = false;
      result.same_row_update_allowed = true;
      result.evidence.push_back("same_row_update_proof=true");
      result.evidence.push_back("same_row_update_allowed=true");
    } else {
      result.evidence.push_back(std::string("same_row_update_proof=") +
                                (proof_valid ? "true" : "false"));
      result.evidence.push_back("same_row_update_allowed=false");
      result.evidence.push_back(std::string("exact_live_entry_duplicate=") +
                                (has_exact_live_entry ? "true" : "false"));
      if (request.active_duplicate_policy ==
          IndexBtreePhysicalUniqueActiveDuplicatePolicy::refuse_candidate) {
        result.conflict_state = IndexBtreePhysicalUniqueConflictState::refuse_candidate;
        result.evidence.push_back("unique_active_duplicate_policy=refuse_candidate");
        result.evidence.push_back("unique_conflict_state=refuse_candidate");
      } else {
        result.conflict_state = IndexBtreePhysicalUniqueConflictState::wait_for_mga;
        result.evidence.push_back("unique_active_duplicate_policy=wait_for_mga");
        result.evidence.push_back("unique_conflict_state=wait_for_mga");
      }
      return result;
    }
  }

  IndexBtreePhysicalInsertRequest insert_request;
  insert_request.cell = validation_cell;
  auto inserted = InsertIndexBtreeCellUnlocked(tree, insert_request);
  result.insert_result = inserted;
  if (!inserted.ok()) {
    return FromPhysicalInsertError(inserted);
  }
  result.inserted = inserted.inserted;
  result.evidence.insert(result.evidence.end(), inserted.evidence.begin(), inserted.evidence.end());
  result.evidence.push_back("unique_physical_insert_performed=true");
  result.evidence.push_back("unique_deferred_reservation_commit_validation=false");
  result.evidence.push_back("unique_rollback_cleanup_authority=false");
  return result;
}

IndexBtreePhysicalDeleteResult DeleteIndexBtreeCell(IndexBtreePhysicalTree* tree,
                                                   const IndexBtreePhysicalDeleteRequest& request) {
  if (tree == nullptr) {
    return PhysicalDeleteError("SB-INDEX-BTREE-PHYSICAL-TREE-INVALID",
                               "storage.index_btree_physical.tree_invalid");
  }
  UniqueTreeLatch latch(tree);
  if (tree->root_page_number == 0) {
    return PhysicalDeleteError("SB-INDEX-BTREE-PHYSICAL-TREE-INVALID",
                               "storage.index_btree_physical.tree_invalid");
  }
  IndexBtreePageBody request_probe;
  request_probe.index_uuid = tree->index_uuid;
  request_probe.page_number = tree->root_page_number;
  request_probe.page_kind = IndexBtreePageKind::root;
  request_probe.tree_level = 0;
  request_probe.cells = {request.cell};
  const auto request_validation = ValidatePhysicalPageBody(request_probe);
  if (!request_validation.ok()) {
    return FromPageDeleteError(request_validation);
  }

  std::vector<u64> path;
  auto current = FetchIndexBtreePhysicalPageUnlocked(*tree, tree->root_page_number);
  if (!current.ok()) {
    return FromPageDeleteError(current);
  }
  while (IsInternalLike(current.body)) {
    path.push_back(current.body.page_number);
    const u64 child_page_number = SelectChildPage(current.body, request.cell);
    if (child_page_number == 0) {
      return PhysicalDeleteError("SB-INDEX-BTREE-PHYSICAL-CHILD-MISSING",
                                 "storage.index_btree_physical.child_missing",
                                 std::to_string(current.body.page_number));
    }
    current = FetchIndexBtreePhysicalPageUnlocked(*tree, child_page_number);
    if (!current.ok()) {
      return FromPageDeleteError(current);
    }
  }

  IndexBtreePageBody leaf = std::move(current.body);
  const auto descent_validation =
      ValidateDescentPathUnlocked(*tree, path, leaf, request.cell);
  if (!descent_validation.ok()) {
    return FromPageDeleteError(descent_validation);
  }
  const auto cell_index = FindExactLiveCell(leaf, request.cell);
  if (!cell_index.has_value()) {
    return PhysicalDeleteError("SB-INDEX-BTREE-PHYSICAL-DELETE-NOT-FOUND",
                               "storage.index_btree_physical.delete_not_found",
                               std::to_string(leaf.page_number));
  }

  leaf.cells[*cell_index].deleted = true;
  IndexBtreePhysicalDeleteResult result;
  result.status = IndexPageOkStatus();
  result.deleted = true;
  result.tombstone_marked = true;
  result.root_page_number = tree->root_page_number;
  result.leaf_page_number = leaf.page_number;
  result.evidence = {"delete_exact_locator=true",
                     "tombstone_marked=true",
                     "exclusive_latch_acquired=true",
                     "optimistic_descent_validated=true"};
  AddCandidateEvidence(&result);

  if (!IsUnderfilledNonRoot(leaf) && LiveCellCount(leaf) >= 2) {
    auto staged_leaf = StagePhysicalPage(leaf, tree->page_size);
    if (!staged_leaf.ok()) {
      return FromPageDeleteError(staged_leaf);
    }
    PublishStagedPage(tree, staged_leaf);
    result.evidence.push_back("delete_cleanup_deferred_until_pressure=true");
    return UpdateParentAfterLeafMutation(tree, path, leaf, std::move(result));
  }

  const u32 removed = RemoveDeletedCells(&leaf);
  result.cleanup_performed = removed > 0;
  result.tombstones_removed += removed;
  result.evidence.push_back("tombstone_cleanup_compacted=" + std::to_string(removed));

  if (leaf.page_kind == IndexBtreePageKind::root) {
    auto staged_root = StagePhysicalPage(leaf, tree->page_size);
    if (!staged_root.ok()) {
      return FromPageDeleteError(staged_root);
    }
    PublishStagedPage(tree, staged_root);
    result.evidence.push_back("root_leaf_cleanup=true");
    return result;
  }

  return RebalanceOrMergeLeaf(tree,
                              path,
                              std::move(leaf),
                              request.cell,
                              std::move(result));
}

IndexBtreePhysicalScanResult ScanIndexBtreePhysicalTree(
    const IndexBtreePhysicalTree& tree,
    const IndexBtreePhysicalScanRequest& request) {
  SharedTreeLatch latch(&tree);
  if (tree.root_page_number == 0) {
    return PhysicalScanError("SB-INDEX-BTREE-PHYSICAL-TREE-INVALID",
                             "storage.index_btree_physical.tree_invalid");
  }
  if (request.ordering != IndexBtreePhysicalScanOrdering::forward &&
      request.ordering != IndexBtreePhysicalScanOrdering::reverse) {
    return PhysicalScanError("SB-INDEX-BTREE-PHYSICAL-SCAN-ORDERING-INVALID",
                             "storage.index_btree_physical.scan_ordering_invalid");
  }
  switch (request.mode) {
    case IndexBtreePhysicalScanMode::point: {
      auto validation = ValidateScanKeyBytes(request.point_key, "point");
      if (!validation.ok()) {
        return validation;
      }
      break;
    }
    case IndexBtreePhysicalScanMode::range:
    case IndexBtreePhysicalScanMode::ordered: {
      if (!request.lower_bound.unbounded) {
        auto validation = ValidateScanKeyBytes(request.lower_bound.encoded_key, "lower_bound");
        if (!validation.ok()) {
          return validation;
        }
      }
      if (!request.upper_bound.unbounded) {
        auto validation = ValidateScanKeyBytes(request.upper_bound.encoded_key, "upper_bound");
        if (!validation.ok()) {
          return validation;
        }
      }
      if (!request.lower_bound.unbounded && !request.upper_bound.unbounded) {
        const int compare =
            CompareEncodedKeysOnly(request.lower_bound.encoded_key, request.upper_bound.encoded_key);
        if (compare > 0) {
          return PhysicalScanError("SB-INDEX-BTREE-PHYSICAL-SCAN-BOUNDS-INVALID",
                                   "storage.index_btree_physical.scan_bounds_invalid");
        }
      }
      break;
    }
    case IndexBtreePhysicalScanMode::prefix: {
      auto validation = ValidateScanPrefixBytes(request.prefix, "prefix");
      if (!validation.ok()) {
        return validation;
      }
      break;
    }
    default:
      return PhysicalScanError("SB-INDEX-BTREE-PHYSICAL-SCAN-MODE-INVALID",
                               "storage.index_btree_physical.scan_mode_invalid");
  }

  std::vector<IndexBtreePageBody> leaves;
  std::vector<u64> visited_pages;
  ScanTraversalAccounting accounting;
  auto collect_result = CollectPrunedScanLeaves(tree,
                                                request,
                                                tree.root_page_number,
                                                0,
                                                &visited_pages,
                                                &leaves,
                                                &accounting);
  if (!collect_result.ok()) {
    return collect_result;
  }

  IndexBtreePhysicalScanResult result;
  result.status = IndexPageOkStatus();
  result.reachable_leaf_pages = accounting.reachable_leaf_pages;
  result.visited_leaf_pages = accounting.visited_leaf_pages;
  result.pruned_leaf_pages = accounting.pruned_leaf_pages;
  result.pruned_subtrees = accounting.pruned_subtrees;
  result.evidence.push_back(std::string("scan_mode=") +
                            IndexBtreePhysicalScanModeName(request.mode));
  result.evidence.push_back(std::string("scan_ordering=") +
                            IndexBtreePhysicalScanOrderingName(request.ordering));
  result.evidence.push_back("shared_latch_acquired=true");
  result.evidence.push_back("concurrent_reader_safe=true");
  result.evidence.push_back("reachable_root_descent=true");
  result.evidence.push_back("orphan_pages_ignored=true");
  result.evidence.push_back("deleted_tombstones_excluded=true");
  result.evidence.push_back(std::string("fence_pruning_enabled=") +
                            (request.mode == IndexBtreePhysicalScanMode::ordered ? "false"
                                                                                  : "true"));
  result.evidence.push_back("reachable_leaf_pages=" +
                            std::to_string(result.reachable_leaf_pages));
  result.evidence.push_back("visited_leaf_pages=" +
                            std::to_string(result.visited_leaf_pages));
  result.evidence.push_back("pruned_leaf_pages=" +
                            std::to_string(result.pruned_leaf_pages));
  result.evidence.push_back("pruned_subtrees=" +
                            std::to_string(result.pruned_subtrees));
  switch (request.mode) {
    case IndexBtreePhysicalScanMode::point:
      result.evidence.push_back("point_fence_pruning=true");
      break;
    case IndexBtreePhysicalScanMode::range:
      result.evidence.push_back("range_fence_pruning=true");
      break;
    case IndexBtreePhysicalScanMode::prefix:
      result.evidence.push_back("prefix_fence_pruning=true");
      break;
    case IndexBtreePhysicalScanMode::ordered:
      result.evidence.push_back("ordered_full_leaf_traversal=true");
      break;
  }
  AddCandidateEvidence(&result);

  for (const IndexBtreePageBody& leaf : leaves) {
    for (std::size_t i = 0; i < leaf.cells.size(); ++i) {
      const IndexBtreeCell& cell = leaf.cells[i];
      if (cell.deleted || cell.high_key || !ScanRequestIncludesCell(request, cell)) {
        continue;
      }
      IndexBtreePhysicalRowLocator locator;
      locator.encoded_key = cell.encoded_key;
      locator.row_uuid = cell.row_uuid;
      locator.version_uuid = cell.version_uuid;
      locator.leaf_page_number = leaf.page_number;
      locator.cell_ordinal = static_cast<u32>(i);
      result.locators.push_back(std::move(locator));
    }
  }

  std::sort(result.locators.begin(), result.locators.end(), LocatorLess);
  result.evidence.push_back("global_ordering=sbko_row_uuid_version_uuid");
  if (request.ordering == IndexBtreePhysicalScanOrdering::reverse) {
    std::reverse(result.locators.begin(), result.locators.end());
    result.evidence.push_back("reverse_order_exact_inverse=true");
  } else {
    result.evidence.push_back("forward_order=true");
  }
  if (request.limit > 0 && result.locators.size() > request.limit) {
    result.locators.resize(static_cast<std::size_t>(request.limit));
    result.evidence.push_back("limit_applied=true");
  }
  if (request.limit > 0) {
    result.evidence.push_back("ordered_limit=" + std::to_string(request.limit));
  }
  result.evidence.push_back("locator_stream_candidate_only=true");
  return result;
}

IndexBtreePhysicalScanResult PointLookupIndexBtreePhysicalTree(
    const IndexBtreePhysicalTree& tree,
    const std::vector<byte>& encoded_key,
    u64 limit) {
  IndexBtreePhysicalScanRequest request;
  request.mode = IndexBtreePhysicalScanMode::point;
  request.point_key = encoded_key;
  request.limit = limit;
  return ScanIndexBtreePhysicalTree(tree, request);
}

IndexBtreePhysicalScanResult RangeScanIndexBtreePhysicalTree(
    const IndexBtreePhysicalTree& tree,
    const IndexBtreePhysicalScanBound& lower_bound,
    const IndexBtreePhysicalScanBound& upper_bound,
    IndexBtreePhysicalScanOrdering ordering,
    u64 limit) {
  IndexBtreePhysicalScanRequest request;
  request.mode = IndexBtreePhysicalScanMode::range;
  request.ordering = ordering;
  request.lower_bound = lower_bound;
  request.upper_bound = upper_bound;
  request.limit = limit;
  return ScanIndexBtreePhysicalTree(tree, request);
}

IndexBtreePhysicalScanResult PrefixScanIndexBtreePhysicalTree(
    const IndexBtreePhysicalTree& tree,
    const std::vector<byte>& encoded_prefix,
    IndexBtreePhysicalScanOrdering ordering,
    u64 limit) {
  IndexBtreePhysicalScanRequest request;
  request.mode = IndexBtreePhysicalScanMode::prefix;
  request.ordering = ordering;
  request.prefix = encoded_prefix;
  request.limit = limit;
  return ScanIndexBtreePhysicalTree(tree, request);
}

IndexBtreePhysicalScanResult OrderedScanIndexBtreePhysicalTree(
    const IndexBtreePhysicalTree& tree,
    IndexBtreePhysicalScanOrdering ordering,
    u64 limit) {
  IndexBtreePhysicalScanRequest request;
  request.mode = IndexBtreePhysicalScanMode::ordered;
  request.ordering = ordering;
  request.limit = limit;
  return ScanIndexBtreePhysicalTree(tree, request);
}

DiagnosticRecord MakeIndexBtreePageDiagnostic(Status status,
                                              std::string diagnostic_code,
                                              std::string message_key,
                                              std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", detail});
  }

  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "storage.page.index_btree");
}

}  // namespace scratchbird::storage::page
