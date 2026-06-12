// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_page_family.hpp"

#include <cstring>

namespace scratchbird::storage::page {
namespace {
using scratchbird::core::platform::HostToLittle16;
using scratchbird::core::platform::HostToLittle32;
using scratchbird::core::platform::HostToLittle64;
using scratchbird::core::platform::LittleToHost16;
using scratchbird::core::platform::LittleToHost32;
using scratchbird::core::platform::LittleToHost64;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status OkStatus() { return {StatusCode::ok, Severity::info, Subsystem::storage_page}; }
Status ErrorStatus() { return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::storage_page}; }

void Store16(std::vector<byte>* out, std::size_t offset, u16 value) {
  value = HostToLittle16(value);
  std::memcpy(out->data() + offset, &value, sizeof(value));
}
void Store32(std::vector<byte>* out, std::size_t offset, u32 value) {
  value = HostToLittle32(value);
  std::memcpy(out->data() + offset, &value, sizeof(value));
}
void Store64(std::vector<byte>* out, std::size_t offset, u64 value) {
  value = HostToLittle64(value);
  std::memcpy(out->data() + offset, &value, sizeof(value));
}
u16 Load16(const std::vector<byte>& in, std::size_t offset) {
  u16 value = 0;
  std::memcpy(&value, in.data() + offset, sizeof(value));
  return LittleToHost16(value);
}
u32 Load32(const std::vector<byte>& in, std::size_t offset) {
  u32 value = 0;
  std::memcpy(&value, in.data() + offset, sizeof(value));
  return LittleToHost32(value);
}
u64 Load64(const std::vector<byte>& in, std::size_t offset) {
  u64 value = 0;
  std::memcpy(&value, in.data() + offset, sizeof(value));
  return LittleToHost64(value);
}
void StoreUuid(std::vector<byte>* out, std::size_t offset, const TypedUuid& uuid) {
  (*out)[offset] = static_cast<byte>(uuid.kind);
  std::memcpy(out->data() + offset + 1, uuid.value.bytes.data(), uuid.value.bytes.size());
}
TypedUuid LoadUuid(const std::vector<byte>& in, std::size_t offset) {
  TypedUuid uuid;
  uuid.kind = static_cast<scratchbird::core::platform::UuidKind>(in[offset]);
  std::memcpy(uuid.value.bytes.data(), in.data() + offset + 1, uuid.value.bytes.size());
  return uuid;
}
}  // namespace

const char* IndexPageFamilyKindName(IndexPageFamilyKind family) {
  switch (family) {
    case IndexPageFamilyKind::btree: return "btree";
    case IndexPageFamilyKind::hash: return "hash";
    case IndexPageFamilyKind::bitmap: return "bitmap";
    case IndexPageFamilyKind::summary: return "summary";
    case IndexPageFamilyKind::inverted: return "inverted";
    case IndexPageFamilyKind::spatial: return "spatial";
    case IndexPageFamilyKind::vector: return "vector";
    case IndexPageFamilyKind::graph: return "graph";
    case IndexPageFamilyKind::temporary_work: return "temporary_work";
    case IndexPageFamilyKind::statistics: return "statistics";
    case IndexPageFamilyKind::unknown: return "unknown";
  }
  return "unknown";
}

IndexPageFamilyKind IndexPageFamilyKindForPageType(PageType page_type) {
  switch (page_type) {
    case PageType::index_btree:
    case PageType::index_btree_root:
    case PageType::index_btree_branch:
    case PageType::index_btree_leaf:
    case PageType::index_btree_posting: return IndexPageFamilyKind::btree;
    case PageType::index_hash: return IndexPageFamilyKind::hash;
    case PageType::index_bitmap: return IndexPageFamilyKind::bitmap;
    case PageType::index_summary: return IndexPageFamilyKind::summary;
    case PageType::index_inverted: return IndexPageFamilyKind::inverted;
    case PageType::index_spatial: return IndexPageFamilyKind::spatial;
    case PageType::index_vector: return IndexPageFamilyKind::vector;
    case PageType::index_graph: return IndexPageFamilyKind::graph;
    case PageType::index_temporary: return IndexPageFamilyKind::temporary_work;
    case PageType::index_statistics: return IndexPageFamilyKind::statistics;
    default: return IndexPageFamilyKind::unknown;
  }
}

PageType RootPageTypeForIndexFamilyKind(IndexPageFamilyKind family) {
  switch (family) {
    case IndexPageFamilyKind::btree: return PageType::index_btree_root;
    case IndexPageFamilyKind::hash: return PageType::index_hash;
    case IndexPageFamilyKind::bitmap: return PageType::index_bitmap;
    case IndexPageFamilyKind::summary: return PageType::index_summary;
    case IndexPageFamilyKind::inverted: return PageType::index_inverted;
    case IndexPageFamilyKind::spatial: return PageType::index_spatial;
    case IndexPageFamilyKind::vector: return PageType::index_vector;
    case IndexPageFamilyKind::graph: return PageType::index_graph;
    case IndexPageFamilyKind::temporary_work: return PageType::index_temporary;
    case IndexPageFamilyKind::statistics: return PageType::index_statistics;
    case IndexPageFamilyKind::unknown: return PageType::index_special_root;
  }
  return PageType::unknown;
}

IndexPageFamilyHeaderResult BuildIndexPageFamilyHeader(const IndexPageFamilyHeader& header) {
  IndexPageFamilyHeaderResult result;
  if (!header.index_object_uuid.valid() || !header.family_uuid.valid() || header.family == IndexPageFamilyKind::unknown) {
    result.status = ErrorStatus();
    result.diagnostic = MakeIndexPageFamilyDiagnostic(result.status,
                                                      "SB_DIAG_PAGE_INDEX_SPECIAL_HEADER_INVALID",
                                                      "index.page.special_header_invalid");
    return result;
  }
  result.status = OkStatus();
  result.header = header;
  result.serialized.assign(kIndexPageFamilyHeaderBytes, 0);
  StoreUuid(&result.serialized, 0, header.index_object_uuid);
  StoreUuid(&result.serialized, 17, header.family_uuid);
  Store64(&result.serialized, 40, header.resource_epoch);
  Store64(&result.serialized, 48, header.mutation_epoch);
  Store64(&result.serialized, 56, header.logical_page_number);
  Store64(&result.serialized, 64, header.right_sibling_page_number);
  Store32(&result.serialized, 72, header.layout_version);
  Store16(&result.serialized, 76, static_cast<u16>(header.family));
  Store32(&result.serialized, 80, static_cast<u32>(header.page_type));
  return result;
}

IndexPageFamilyHeaderResult ParseIndexPageFamilyHeader(const std::vector<byte>& serialized) {
  IndexPageFamilyHeaderResult result;
  if (serialized.size() < kIndexPageFamilyHeaderBytes) {
    result.status = ErrorStatus();
    result.diagnostic = MakeIndexPageFamilyDiagnostic(result.status,
                                                      "SB_DIAG_PAGE_INDEX_SPECIAL_HEADER_INVALID",
                                                      "index.page.special_header_truncated");
    return result;
  }
  result.status = OkStatus();
  result.serialized.assign(serialized.begin(), serialized.begin() + kIndexPageFamilyHeaderBytes);
  result.header.index_object_uuid = LoadUuid(serialized, 0);
  result.header.family_uuid = LoadUuid(serialized, 17);
  result.header.resource_epoch = Load64(serialized, 40);
  result.header.mutation_epoch = Load64(serialized, 48);
  result.header.logical_page_number = Load64(serialized, 56);
  result.header.right_sibling_page_number = Load64(serialized, 64);
  result.header.layout_version = Load32(serialized, 72);
  result.header.family = static_cast<IndexPageFamilyKind>(Load16(serialized, 76));
  result.header.page_type = static_cast<PageType>(Load32(serialized, 80));
  if (result.header.family == IndexPageFamilyKind::unknown || !result.header.index_object_uuid.valid() || !result.header.family_uuid.valid()) {
    result.status = ErrorStatus();
    result.diagnostic = MakeIndexPageFamilyDiagnostic(result.status,
                                                      "SB_DIAG_PAGE_INDEX_SPECIAL_HEADER_INVALID",
                                                      "index.page.special_header_invalid");
  }
  return result;
}

IndexSpecialHeaderResult BuildIndexSpecialHeader(const IndexSpecialHeader& header) {
  return BuildIndexPageFamilyHeader(header);
}

IndexSpecialHeaderResult ParseIndexSpecialHeader(const std::vector<byte>& serialized) {
  return ParseIndexPageFamilyHeader(serialized);
}

DiagnosticRecord MakeIndexPageFamilyDiagnostic(Status status, std::string diagnostic_code,
                                               std::string message_key, std::string detail) {
  return MakeDiagnostic(status.code, status.severity, status.subsystem, std::move(diagnostic_code),
                        std::move(message_key), detail.empty() ? std::vector<scratchbird::core::platform::DiagnosticArgument>{}
                                                               : std::vector<scratchbird::core::platform::DiagnosticArgument>{{"detail", std::move(detail)}},
                        {}, "storage.page.index_family");
}

}  // namespace scratchbird::storage::page
