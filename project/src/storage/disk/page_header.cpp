// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "page_header.hpp"

#include "metric_producer.hpp"

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

namespace scratchbird::storage::disk {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
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

constexpr u32 kOffsetMagic = 0;
constexpr u32 kOffsetHeaderBytes = 8;
constexpr u32 kOffsetPageSize = 12;
constexpr u32 kOffsetPageType = 16;
constexpr u32 kOffsetChecksumAlgorithm = 20;
constexpr u32 kOffsetDatabaseUuid = 24;
constexpr u32 kOffsetFilespaceUuid = 40;
constexpr u32 kOffsetPageUuid = 56;
constexpr u32 kOffsetPageNumber = 72;
constexpr u32 kOffsetPageGeneration = 80;
constexpr u32 kOffsetFlags = 88;
constexpr u32 kOffsetHeaderChecksum = 96;

Status PageOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::storage_disk};
}

Status PageErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::storage_disk};
}

Status PageWarningStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::warning, Subsystem::storage_disk};
}

void RecordPageMetric(const char* family, const char* classification, const char* reason) {
  (void)scratchbird::core::metrics::IncrementCounter(
      family,
      scratchbird::core::metrics::Labels({{"component", "storage.page_header"}, {"classification", classification},
                                          {"reason", reason}}),
      1.0,
      family == std::string("sb_storage_checksum_failures_total") ? "storage_page" : "storage_page");
}

void Store16(SerializedPageHeader* serialized, u32 offset, u16 value) {
  const u16 stored = HostToLittle16(value);
  std::memcpy(serialized->data() + offset, &stored, sizeof(stored));
}

void Store32(SerializedPageHeader* serialized, u32 offset, u32 value) {
  const u32 stored = HostToLittle32(value);
  std::memcpy(serialized->data() + offset, &stored, sizeof(stored));
}

void Store64(SerializedPageHeader* serialized, u32 offset, u64 value) {
  const u64 stored = HostToLittle64(value);
  std::memcpy(serialized->data() + offset, &stored, sizeof(stored));
}

u16 Load16(const SerializedPageHeader& serialized, u32 offset) {
  u16 value = 0;
  std::memcpy(&value, serialized.data() + offset, sizeof(value));
  return LittleToHost16(value);
}

u32 Load32(const SerializedPageHeader& serialized, u32 offset) {
  u32 value = 0;
  std::memcpy(&value, serialized.data() + offset, sizeof(value));
  return LittleToHost32(value);
}

u64 Load64(const SerializedPageHeader& serialized, u32 offset) {
  u64 value = 0;
  std::memcpy(&value, serialized.data() + offset, sizeof(value));
  return LittleToHost64(value);
}

SerializedPageHeader ZeroChecksumField(SerializedPageHeader serialized) {
  Store64(&serialized, kOffsetHeaderChecksum, 0);
  return serialized;
}

PageHeaderResult HeaderError(std::string diagnostic_code,
                             std::string message_key,
                             std::string detail = {}) {
  PageHeaderResult result;
  result.status = PageErrorStatus();
  result.diagnostic = MakePageHeaderDiagnostic(result.status,
                                               std::move(diagnostic_code),
                                               std::move(message_key),
                                               std::move(detail));
  return result;
}

SerializedPageHeaderResult SerializedHeaderError(std::string diagnostic_code,
                                                 std::string message_key,
                                                 std::string detail = {}) {
  SerializedPageHeaderResult result;
  result.status = PageErrorStatus();
  result.diagnostic = MakePageHeaderDiagnostic(result.status,
                                               std::move(diagnostic_code),
                                               std::move(message_key),
                                               std::move(detail));
  return result;
}

PageClassification Classification(PageClassificationKind kind,
                                  PageType page_type,
                                  bool readable,
                                  bool writable,
                                  bool cluster_authority_required,
                                  bool decryption_required,
                                  Status status,
                                  std::string diagnostic_code,
                                  std::string message_key) {
  PageClassification classification;
  classification.status = status;
  classification.kind = kind;
  classification.page_type = page_type;
  classification.readable = readable;
  classification.writable = writable;
  classification.cluster_authority_required = cluster_authority_required;
  classification.decryption_required = decryption_required;
  if (!diagnostic_code.empty()) {
    classification.diagnostic = MakePageHeaderDiagnostic(status,
                                                         std::move(diagnostic_code),
                                                         std::move(message_key),
                                                         PageTypeName(page_type));
  }
  return classification;
}

}  // namespace

const char* PageTypeName(PageType page_type) {
  switch (page_type) {
    case PageType::database_header: return "database_header";
    case PageType::allocation_map: return "allocation_map";
    case PageType::catalog: return "catalog";
    case PageType::transaction_inventory: return "transaction_inventory";
    case PageType::row_data: return "row_data";
    case PageType::index_btree: return "index_btree";
    case PageType::index_btree_root: return "index_btree_root";
    case PageType::index_btree_branch: return "index_btree_branch";
    case PageType::index_btree_leaf: return "index_btree_leaf";
    case PageType::index_btree_posting: return "index_btree_posting";
    case PageType::index_hash: return "index_hash";
    case PageType::index_bitmap: return "index_bitmap";
    case PageType::index_summary: return "index_summary";
    case PageType::index_inverted: return "index_inverted";
    case PageType::index_spatial: return "index_spatial";
    case PageType::index_vector: return "index_vector";
    case PageType::index_graph: return "index_graph";
    case PageType::index_temporary: return "index_temporary";
    case PageType::index_statistics: return "index_statistics";
    case PageType::index_special_root: return "index_special_root";
    case PageType::blob: return "blob";
    case PageType::metrics: return "metrics";
    case PageType::archive: return "archive";
    case PageType::columnar: return "columnar";
    case PageType::vector: return "vector";
    case PageType::graph: return "graph";
    case PageType::system_state: return "system_state";
    case PageType::bootstrap_reserved: return "bootstrap_reserved";
    case PageType::filespace_directory: return "filespace_directory";
    case PageType::config_root: return "config_root";
    case PageType::security_root: return "security_root";
    case PageType::reserved_local: return "reserved_local";
    case PageType::cluster_decision: return "cluster_decision";
    case PageType::cluster_route: return "cluster_route";
    case PageType::cluster_catalog: return "cluster_catalog";
    case PageType::cluster_transaction: return "cluster_transaction";
    case PageType::encrypted_opaque: return "encrypted_opaque";
    case PageType::unknown: return "unknown";
  }
  return "unknown";
}

const char* PageClassificationKindName(PageClassificationKind kind) {
  switch (kind) {
    case PageClassificationKind::supported_local: return "supported_local";
    case PageClassificationKind::reserved_local: return "reserved_local";
    case PageClassificationKind::cluster_only: return "cluster_only";
    case PageClassificationKind::encrypted_or_opaque: return "encrypted_or_opaque";
    case PageClassificationKind::unknown_safe: return "unknown_safe";
    case PageClassificationKind::invalid_magic: return "invalid_magic";
    case PageClassificationKind::invalid_header: return "invalid_header";
    case PageClassificationKind::checksum_mismatch: return "checksum_mismatch";
  }
  return "unknown";
}

bool IsSupportedLocalPageType(PageType page_type) {
  switch (page_type) {
    case PageType::database_header:
    case PageType::allocation_map:
    case PageType::catalog:
    case PageType::transaction_inventory:
    case PageType::row_data:
    case PageType::index_btree:
    case PageType::index_btree_root:
    case PageType::index_btree_branch:
    case PageType::index_btree_leaf:
    case PageType::index_btree_posting:
    case PageType::index_hash:
    case PageType::index_bitmap:
    case PageType::index_summary:
    case PageType::index_inverted:
    case PageType::index_spatial:
    case PageType::index_vector:
    case PageType::index_graph:
    case PageType::index_temporary:
    case PageType::index_statistics:
    case PageType::index_special_root:
    case PageType::blob:
    case PageType::metrics:
    case PageType::archive:
    case PageType::columnar:
    case PageType::vector:
    case PageType::graph:
    case PageType::system_state:
    case PageType::bootstrap_reserved:
    case PageType::filespace_directory:
    case PageType::config_root:
    case PageType::security_root:
      return true;
    default:
      return false;
  }
}

bool IsClusterOnlyPageType(PageType page_type) {
  switch (page_type) {
    case PageType::cluster_decision:
    case PageType::cluster_route:
    case PageType::cluster_catalog:
    case PageType::cluster_transaction:
      return true;
    default:
      return false;
  }
}

bool IsPageUuidV7(const Uuid& uuid) {
  return !uuid.is_nil() &&
         ((uuid.bytes[8] & 0xc0u) == 0x80u) &&
         (((uuid.bytes[6] >> 4) & 0x0fu) == 7u);
}

u64 ComputePageHeaderChecksum(const SerializedPageHeader& serialized) {
  const SerializedPageHeader normalized = ZeroChecksumField(serialized);
  u64 hash = 1469598103934665603ull;
  for (byte value : normalized) {
    hash ^= value;
    hash *= 1099511628211ull;
  }
  return hash;
}

PageHeaderResult ValidatePageHeader(const PageHeader& header) {
  if (header.header_bytes != kPageHeaderSerializedBytes) {
    return HeaderError("SB-STORAGE-PAGE-HEADER-SIZE-INVALID",
                       "storage.page.header_size_invalid",
                       std::to_string(header.header_bytes));
  }
  if (!IsSupportedDatabasePageSize(header.page_size)) {
    return HeaderError("SB-STORAGE-PAGE-SIZE-INVALID",
                       "storage.page.page_size_invalid",
                       std::to_string(header.page_size));
  }
  if (!IsDatabaseUuidV7(header.database_uuid)) {
    return HeaderError("SB-STORAGE-PAGE-DATABASE-UUID-NOT-V7",
                       "storage.page.database_uuid_not_v7");
  }
  if (!IsPageUuidV7(header.filespace_uuid)) {
    return HeaderError("SB-STORAGE-PAGE-FILESPACE-UUID-NOT-V7",
                       "storage.page.filespace_uuid_not_v7");
  }
  if (!IsPageUuidV7(header.page_uuid)) {
    return HeaderError("SB-STORAGE-PAGE-UUID-NOT-V7",
                       "storage.page.page_uuid_not_v7");
  }
  if (header.checksum_algorithm != ChecksumAlgorithm::none &&
      header.checksum_algorithm != ChecksumAlgorithm::fnv1a64) {
    return HeaderError("SB-STORAGE-PAGE-CHECKSUM-UNKNOWN",
                       "storage.page.checksum_unknown",
                       std::to_string(static_cast<u16>(header.checksum_algorithm)));
  }

  PageHeaderResult result;
  result.status = PageOkStatus();
  result.header = header;
  return result;
}

SerializedPageHeaderResult SerializePageHeader(const PageHeader& header) {
  PageHeaderResult validated = ValidatePageHeader(header);
  if (!validated.ok()) {
    SerializedPageHeaderResult result;
    result.status = validated.status;
    result.diagnostic = validated.diagnostic;
    return result;
  }

  SerializedPageHeaderResult result;
  result.status = PageOkStatus();
  SerializedPageHeader serialized{};
  std::copy(kScratchBirdPageMagic.begin(), kScratchBirdPageMagic.end(), serialized.begin() + kOffsetMagic);
  Store32(&serialized, kOffsetHeaderBytes, header.header_bytes);
  Store32(&serialized, kOffsetPageSize, header.page_size);
  Store32(&serialized, kOffsetPageType, static_cast<u32>(header.page_type));
  Store16(&serialized, kOffsetChecksumAlgorithm, static_cast<u16>(header.checksum_algorithm));
  std::copy(header.database_uuid.bytes.begin(), header.database_uuid.bytes.end(), serialized.begin() + kOffsetDatabaseUuid);
  std::copy(header.filespace_uuid.bytes.begin(), header.filespace_uuid.bytes.end(), serialized.begin() + kOffsetFilespaceUuid);
  std::copy(header.page_uuid.bytes.begin(), header.page_uuid.bytes.end(), serialized.begin() + kOffsetPageUuid);
  Store64(&serialized, kOffsetPageNumber, header.page_number);
  Store64(&serialized, kOffsetPageGeneration, header.page_generation);
  Store64(&serialized, kOffsetFlags, header.flags);
  const u64 checksum = header.checksum_algorithm == ChecksumAlgorithm::none ? 0 : ComputePageHeaderChecksum(serialized);
  Store64(&serialized, kOffsetHeaderChecksum, checksum);
  result.serialized = serialized;
  return result;
}

PageHeaderResult ParsePageHeader(const SerializedPageHeader& serialized) {
  if (!std::equal(kScratchBirdPageMagic.begin(), kScratchBirdPageMagic.end(), serialized.begin() + kOffsetMagic)) {
    return HeaderError("SB-STORAGE-PAGE-MAGIC-INVALID",
                       "storage.page.magic_invalid");
  }

  PageHeader header;
  header.header_bytes = Load32(serialized, kOffsetHeaderBytes);
  header.page_size = Load32(serialized, kOffsetPageSize);
  header.page_type = static_cast<PageType>(Load32(serialized, kOffsetPageType));
  header.checksum_algorithm = static_cast<ChecksumAlgorithm>(Load16(serialized, kOffsetChecksumAlgorithm));
  std::copy(serialized.begin() + kOffsetDatabaseUuid,
            serialized.begin() + kOffsetDatabaseUuid + header.database_uuid.bytes.size(),
            header.database_uuid.bytes.begin());
  std::copy(serialized.begin() + kOffsetFilespaceUuid,
            serialized.begin() + kOffsetFilespaceUuid + header.filespace_uuid.bytes.size(),
            header.filespace_uuid.bytes.begin());
  std::copy(serialized.begin() + kOffsetPageUuid,
            serialized.begin() + kOffsetPageUuid + header.page_uuid.bytes.size(),
            header.page_uuid.bytes.begin());
  header.page_number = Load64(serialized, kOffsetPageNumber);
  header.page_generation = Load64(serialized, kOffsetPageGeneration);
  header.flags = Load64(serialized, kOffsetFlags);
  header.header_checksum = Load64(serialized, kOffsetHeaderChecksum);

  PageHeaderResult validated = ValidatePageHeader(header);
  if (!validated.ok()) {
    return validated;
  }

  if (header.checksum_algorithm == ChecksumAlgorithm::fnv1a64) {
    const u64 expected = ComputePageHeaderChecksum(serialized);
    if (header.header_checksum != expected) {
      RecordPageMetric("sb_storage_checksum_failures_total", "checksum_mismatch", "header_checksum_mismatch");
      return HeaderError("SB-STORAGE-PAGE-HEADER-CHECKSUM-MISMATCH",
                         "storage.page.header_checksum_mismatch");
    }
  }

  validated.header = header;
  return validated;
}

PageClassification ClassifyPageHeader(const SerializedPageHeader& serialized) {
  if (!std::equal(kScratchBirdPageMagic.begin(), kScratchBirdPageMagic.end(), serialized.begin() + kOffsetMagic)) {
    RecordPageMetric("sb_storage_unknown_pages_total", "invalid_magic", "magic_invalid");
    return Classification(PageClassificationKind::invalid_magic,
                          PageType::unknown,
                          false,
                          false,
                          false,
                          false,
                          PageErrorStatus(),
                          "SB-STORAGE-PAGE-MAGIC-INVALID",
                          "storage.page.magic_invalid");
  }

  PageHeaderResult parsed = ParsePageHeader(serialized);
  if (!parsed.ok()) {
    const std::string code = parsed.diagnostic.diagnostic_code;
    const PageClassificationKind kind = code == "SB-STORAGE-PAGE-HEADER-CHECKSUM-MISMATCH"
                                            ? PageClassificationKind::checksum_mismatch
                                            : PageClassificationKind::invalid_header;
    if (kind != PageClassificationKind::checksum_mismatch) {
      RecordPageMetric("sb_storage_unknown_pages_total", PageClassificationKindName(kind), parsed.diagnostic.diagnostic_code.c_str());
    }
    return Classification(kind,
                          PageType::unknown,
                          false,
                          false,
                          false,
                          false,
                          parsed.status,
                          parsed.diagnostic.diagnostic_code,
                          parsed.diagnostic.message_key);
  }

  const PageHeader& header = parsed.header;
  if ((header.flags & PageHeaderFlag::encrypted_payload) != 0 || header.page_type == PageType::encrypted_opaque) {
    return Classification(PageClassificationKind::encrypted_or_opaque,
                          header.page_type,
                          true,
                          false,
                          false,
                          true,
                          PageWarningStatus(),
                          "SB-STORAGE-PAGE-ENCRYPTED-OPAQUE",
                          "storage.page.encrypted_opaque");
  }

  if (IsClusterOnlyPageType(header.page_type) || (header.flags & PageHeaderFlag::cluster_only) != 0) {
    return Classification(PageClassificationKind::cluster_only,
                          header.page_type,
                          true,
                          false,
                          true,
                          false,
                          PageWarningStatus(),
                          "SB-STORAGE-PAGE-CLUSTER-ONLY",
                          "storage.page.cluster_only");
  }

  if (IsSupportedLocalPageType(header.page_type)) {
    return Classification(PageClassificationKind::supported_local,
                          header.page_type,
                          true,
                          true,
                          false,
                          false,
                          PageOkStatus(),
                          {},
                          {});
  }

  if (header.page_type == PageType::reserved_local || (header.flags & PageHeaderFlag::reserved_no_write) != 0) {
    return Classification(PageClassificationKind::reserved_local,
                          header.page_type,
                          true,
                          false,
                          false,
                          false,
                          PageWarningStatus(),
                          "SB-STORAGE-PAGE-RESERVED-LOCAL",
                          "storage.page.reserved_local");
  }

  if ((header.flags & PageHeaderFlag::unknown_safe_read_only) != 0) {
    RecordPageMetric("sb_storage_unknown_pages_total", "unknown_safe", "unknown_safe_read_only");
    return Classification(PageClassificationKind::unknown_safe,
                          header.page_type,
                          true,
                          false,
                          false,
                          false,
                          PageWarningStatus(),
                          "SB-STORAGE-PAGE-UNKNOWN-SAFE-READ-ONLY",
                          "storage.page.unknown_safe_read_only");
  }

  RecordPageMetric("sb_storage_unknown_pages_total", "unknown_unsafe", "unknown_unsafe");
  return Classification(PageClassificationKind::invalid_header,
                        header.page_type,
                        false,
                        false,
                        false,
                        false,
                        PageErrorStatus(),
                        "SB-STORAGE-PAGE-UNKNOWN-UNSAFE",
                        "storage.page.unknown_unsafe");
}

DiagnosticRecord MakePageHeaderDiagnostic(Status status,
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
                        "storage.page_header");
}

}  // namespace scratchbird::storage::disk
