// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "database_format.hpp"
#include "runtime_platform.hpp"

#include <array>
#include <string>

namespace scratchbird::storage::disk {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::Uuid;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

inline constexpr u32 kPageHeaderSerializedBytes = 128;
inline constexpr std::array<byte, 8> kScratchBirdPageMagic = {'S', 'B', 'P', 'G', 'V', '0', '0', '1'};

using SerializedPageHeader = std::array<byte, kPageHeaderSerializedBytes>;

enum class PageType : u32 {
  database_header = 1,
  allocation_map = 2,
  catalog = 3,
  transaction_inventory = 4,
  row_data = 5,
  index_btree = 6,
  index_btree_root = 18,
  index_btree_branch = 19,
  index_btree_leaf = 20,
  index_btree_posting = 21,
  index_hash = 22,
  index_bitmap = 23,
  index_summary = 24,
  index_inverted = 25,
  index_spatial = 26,
  index_vector = 27,
  index_graph = 28,
  index_temporary = 29,
  index_statistics = 30,
  index_special_root = 31,
  blob = 7,
  metrics = 8,
  archive = 9,
  columnar = 10,
  vector = 11,
  graph = 12,
  system_state = 13,
  bootstrap_reserved = 14,
  filespace_directory = 15,
  config_root = 16,
  security_root = 17,
  reserved_local = 1000,
  cluster_decision = 50000,
  cluster_route = 50001,
  cluster_catalog = 50002,
  cluster_transaction = 50003,
  encrypted_opaque = 60000,
  unknown = 0xffffffffu
};

inline constexpr std::array<PageType, 37> kDeclaredPageTypes = {{
    PageType::database_header,
    PageType::allocation_map,
    PageType::catalog,
    PageType::transaction_inventory,
    PageType::row_data,
    PageType::index_btree,
    PageType::index_btree_root,
    PageType::index_btree_branch,
    PageType::index_btree_leaf,
    PageType::index_btree_posting,
    PageType::index_hash,
    PageType::index_bitmap,
    PageType::index_summary,
    PageType::index_inverted,
    PageType::index_spatial,
    PageType::index_vector,
    PageType::index_graph,
    PageType::index_temporary,
    PageType::index_statistics,
    PageType::index_special_root,
    PageType::blob,
    PageType::metrics,
    PageType::archive,
    PageType::columnar,
    PageType::vector,
    PageType::graph,
    PageType::system_state,
    PageType::bootstrap_reserved,
    PageType::filespace_directory,
    PageType::config_root,
    PageType::security_root,
    PageType::reserved_local,
    PageType::cluster_decision,
    PageType::cluster_route,
    PageType::cluster_catalog,
    PageType::cluster_transaction,
    PageType::encrypted_opaque,
}};

enum class PageClassificationKind : u16 {
  supported_local,
  reserved_local,
  cluster_only,
  encrypted_or_opaque,
  unknown_safe,
  invalid_magic,
  invalid_header,
  checksum_mismatch
};

namespace PageHeaderFlag {
inline constexpr u64 encrypted_payload = 1ull << 0;
inline constexpr u64 cluster_only = 1ull << 1;
inline constexpr u64 unknown_safe_read_only = 1ull << 2;
inline constexpr u64 reserved_no_write = 1ull << 3;
}  // namespace PageHeaderFlag

struct PageHeader {
  u32 header_bytes = kPageHeaderSerializedBytes;
  u32 page_size = static_cast<u32>(PageSizeProfile::profile_16k);
  PageType page_type = PageType::unknown;
  ChecksumAlgorithm checksum_algorithm = ChecksumAlgorithm::fnv1a64;
  Uuid database_uuid;
  Uuid filespace_uuid;
  Uuid page_uuid;
  u64 page_number = 0;
  u64 page_generation = 0;
  u64 flags = 0;
  u64 header_checksum = 0;
};

struct PageHeaderResult {
  Status status;
  PageHeader header;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct SerializedPageHeaderResult {
  Status status;
  SerializedPageHeader serialized{};
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct PageClassification {
  Status status;
  PageClassificationKind kind = PageClassificationKind::invalid_header;
  PageType page_type = PageType::unknown;
  bool readable = false;
  bool writable = false;
  bool cluster_authority_required = false;
  bool decryption_required = false;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

const char* PageTypeName(PageType page_type);
const char* PageClassificationKindName(PageClassificationKind kind);
bool IsSupportedLocalPageType(PageType page_type);
bool IsClusterOnlyPageType(PageType page_type);
bool IsPageUuidV7(const Uuid& uuid);
u64 ComputePageHeaderChecksum(const SerializedPageHeader& serialized);
PageHeaderResult ValidatePageHeader(const PageHeader& header);
SerializedPageHeaderResult SerializePageHeader(const PageHeader& header);
PageHeaderResult ParsePageHeader(const SerializedPageHeader& serialized);
PageClassification ClassifyPageHeader(const SerializedPageHeader& serialized);
DiagnosticRecord MakePageHeaderDiagnostic(Status status,
                                          std::string diagnostic_code,
                                          std::string message_key,
                                          std::string detail = {});

}  // namespace scratchbird::storage::disk
