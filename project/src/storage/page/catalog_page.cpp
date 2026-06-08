// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog_page.hpp"

#include "database_format.hpp"
#include "page_header.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>
#include <vector>

namespace scratchbird::storage::page {
namespace {

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

inline constexpr std::array<byte, 8> kCatalogMagic = {'S', 'B', 'C', 'A', 'T', '0', '0', '1'};
inline constexpr u32 kOffsetMagic = 0;
inline constexpr u32 kOffsetFormatMajor = 8;
inline constexpr u32 kOffsetFormatMinor = 10;
inline constexpr u32 kOffsetHeaderBytes = 12;
inline constexpr u32 kOffsetPageSequence = 16;
inline constexpr u32 kOffsetRowCount = 20;
inline constexpr u32 kOffsetBodyBytes = 24;
inline constexpr u32 kOffsetNextPageNumber = 32;
inline constexpr u32 kOffsetBodyChecksum = 40;

inline constexpr u32 kRowHeaderBytes = 20;
inline constexpr u32 kRowOffsetKind = 0;
inline constexpr u32 kRowOffsetFlags = 2;
inline constexpr u32 kRowOffsetOrdinal = 4;
inline constexpr u32 kRowOffsetPayloadBytes = 8;
inline constexpr u32 kRowOffsetPayloadChecksum = 12;

Status CatalogPageOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::storage_page};
}

Status CatalogPageErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::storage_page};
}

CatalogPageBodyResult CatalogPageBodyError(std::string diagnostic_code,
                                           std::string message_key,
                                           std::string detail = {}) {
  CatalogPageBodyResult result;
  result.status = CatalogPageErrorStatus();
  result.diagnostic = MakeCatalogPageDiagnostic(result.status,
                                                std::move(diagnostic_code),
                                                std::move(message_key),
                                                std::move(detail));
  return result;
}

CatalogPageSetResult CatalogPageSetError(std::string diagnostic_code,
                                         std::string message_key,
                                         std::string detail = {}) {
  CatalogPageSetResult result;
  result.status = CatalogPageErrorStatus();
  result.diagnostic = MakeCatalogPageDiagnostic(result.status,
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

u64 Fnv1a64(const std::string& payload) {
  return Fnv1a64(reinterpret_cast<const byte*>(payload.data()), payload.size());
}

std::vector<byte> SerializeBody(const CatalogPageBody& body, u32 page_size) {
  std::vector<byte> result(page_size - scratchbird::storage::disk::kPageHeaderSerializedBytes, 0);
  std::copy(kCatalogMagic.begin(), kCatalogMagic.end(), result.begin() + kOffsetMagic);
  StoreLittle16(result.data() + kOffsetFormatMajor, kCatalogPageBodyFormatMajor);
  StoreLittle16(result.data() + kOffsetFormatMinor, kCatalogPageBodyFormatMinor);
  StoreLittle32(result.data() + kOffsetHeaderBytes, kCatalogPageBodyHeaderBytes);
  StoreLittle32(result.data() + kOffsetPageSequence, body.page_sequence);
  StoreLittle32(result.data() + kOffsetRowCount, static_cast<u32>(body.rows.size()));
  StoreLittle64(result.data() + kOffsetNextPageNumber, body.next_page_number);

  u32 offset = kCatalogPageBodyHeaderBytes;
  for (const CatalogPageRow& row : body.rows) {
    StoreLittle16(result.data() + offset + kRowOffsetKind, static_cast<u16>(row.kind));
    StoreLittle16(result.data() + offset + kRowOffsetFlags, 0);
    StoreLittle32(result.data() + offset + kRowOffsetOrdinal, row.ordinal);
    StoreLittle32(result.data() + offset + kRowOffsetPayloadBytes, static_cast<u32>(row.payload.size()));
    StoreLittle64(result.data() + offset + kRowOffsetPayloadChecksum, Fnv1a64(row.payload));
    offset += kRowHeaderBytes;
    std::memcpy(result.data() + offset, row.payload.data(), row.payload.size());
    offset += static_cast<u32>(row.payload.size());
  }

  StoreLittle32(result.data() + kOffsetBodyBytes, offset);
  StoreLittle64(result.data() + kOffsetBodyChecksum, ComputeCatalogPageBodyChecksum(result));
  return result;
}

u32 SerializedRowBytes(const CatalogPageRow& row) {
  return kRowHeaderBytes + static_cast<u32>(row.payload.size());
}

}  // namespace

const char* CatalogPageRowKindName(CatalogPageRowKind kind) {
  switch (kind) {
    case CatalogPageRowKind::resource_seed_pack: return "resource_seed_pack";
    case CatalogPageRowKind::resource_seed_artifact: return "resource_seed_artifact";
    case CatalogPageRowKind::resource_family_summary: return "resource_family_summary";
    case CatalogPageRowKind::typed_catalog_record: return "typed_catalog_record";
    case CatalogPageRowKind::policy_seed_pack: return "policy_seed_pack";
    case CatalogPageRowKind::charset_record: return "charset_record";
    case CatalogPageRowKind::charset_alias_record: return "charset_alias_record";
    case CatalogPageRowKind::collation_record: return "collation_record";
    case CatalogPageRowKind::collation_tailoring_record: return "collation_tailoring_record";
    case CatalogPageRowKind::timezone_record: return "timezone_record";
    case CatalogPageRowKind::timezone_transition_record: return "timezone_transition_record";
    case CatalogPageRowKind::timezone_leap_second_record: return "timezone_leap_second_record";
    case CatalogPageRowKind::bootstrap_object: return "bootstrap_object";
    case CatalogPageRowKind::cluster_catalog_record: return "cluster_catalog_record";
    case CatalogPageRowKind::unknown: return "unknown";
  }
  return "unknown";
}

u64 ComputeCatalogPageBodyChecksum(const std::vector<byte>& body) {
  std::vector<byte> normalized = body;
  if (normalized.size() >= kOffsetBodyChecksum + sizeof(u64)) {
    StoreLittle64(normalized.data() + kOffsetBodyChecksum, 0);
  }
  return Fnv1a64(normalized.data(), normalized.size());
}

CatalogPageSetResult BuildCatalogPageSet(const std::vector<CatalogPageRow>& rows,
                                         u32 page_size,
                                         u64 first_page_number,
                                         u64 overflow_first_page_number) {
  if (page_size <= scratchbird::storage::disk::kPageHeaderSerializedBytes + kCatalogPageBodyHeaderBytes) {
    return CatalogPageSetError("SB-CATALOG-PAGE-BODY-PAGE-SIZE-TOO-SMALL",
                               "storage.catalog_page_body.page_size_too_small",
                               std::to_string(page_size));
  }
  if (first_page_number == 0 || overflow_first_page_number == 0) {
    return CatalogPageSetError("SB-CATALOG-PAGE-BODY-PAGE-NUMBER-INVALID",
                               "storage.catalog_page_body.page_number_invalid");
  }

  const u32 capacity = page_size - scratchbird::storage::disk::kPageHeaderSerializedBytes;
  std::vector<CatalogPageBody> bodies;
  CatalogPageBody current;
  current.page_sequence = 0;
  current.page_number = first_page_number;
  u32 used = kCatalogPageBodyHeaderBytes;

  for (const CatalogPageRow& row : rows) {
    if (row.kind == CatalogPageRowKind::unknown) {
      return CatalogPageSetError("SB-CATALOG-PAGE-BODY-ROW-KIND-UNKNOWN",
                                 "storage.catalog_page_body.row_kind_unknown");
    }
    const u32 row_bytes = SerializedRowBytes(row);
    if (row_bytes + kCatalogPageBodyHeaderBytes > capacity) {
      return CatalogPageSetError("SB-CATALOG-PAGE-BODY-ROW-TOO-LARGE",
                                 "storage.catalog_page_body.row_too_large",
                                 std::to_string(row.ordinal));
    }
    if (used + row_bytes > capacity) {
      bodies.push_back(std::move(current));
      current = CatalogPageBody{};
      current.page_sequence = static_cast<u32>(bodies.size());
      current.page_number = overflow_first_page_number + current.page_sequence - 1;
      used = kCatalogPageBodyHeaderBytes;
    }
    current.rows.push_back(row);
    used += row_bytes;
  }
  bodies.push_back(std::move(current));

  for (std::size_t i = 0; i < bodies.size(); ++i) {
    bodies[i].next_page_number = (i + 1 < bodies.size()) ? bodies[i + 1].page_number : 0;
  }

  CatalogPageSetResult result;
  result.status = CatalogPageOkStatus();
  for (const CatalogPageBody& body : bodies) {
    SerializedCatalogPageBody serialized;
    serialized.page_number = body.page_number;
    serialized.next_page_number = body.next_page_number;
    serialized.body = SerializeBody(body, page_size);
    result.pages.push_back(std::move(serialized));
  }
  return result;
}

CatalogPageBodyResult ParseCatalogPageBody(const std::vector<byte>& body, u64 page_number) {
  if (body.size() < kCatalogPageBodyHeaderBytes) {
    return CatalogPageBodyError("SB-CATALOG-PAGE-BODY-SHORT",
                                "storage.catalog_page_body.short",
                                std::to_string(page_number));
  }
  if (!std::equal(kCatalogMagic.begin(), kCatalogMagic.end(), body.begin() + kOffsetMagic)) {
    return CatalogPageBodyError("SB-CATALOG-PAGE-BODY-MAGIC-INVALID",
                                "storage.catalog_page_body.magic_invalid",
                                std::to_string(page_number));
  }
  const u16 format_major = LoadLittle16(body.data() + kOffsetFormatMajor);
  const u16 format_minor = LoadLittle16(body.data() + kOffsetFormatMinor);
  if (format_major < kCatalogPageBodyFormatMajorMinSupported ||
      format_major > kCatalogPageBodyFormatMajorMaxSupported ||
      (format_major == kCatalogPageBodyFormatMajor && format_minor > kCatalogPageBodyFormatMinorMaxSupported)) {
    return CatalogPageBodyError("SB-CATALOG-PAGE-BODY-FORMAT-UNSUPPORTED",
                                "storage.catalog_page_body.format_unsupported",
                                std::to_string(page_number));
  }
  if (LoadLittle32(body.data() + kOffsetHeaderBytes) != kCatalogPageBodyHeaderBytes) {
    return CatalogPageBodyError("SB-CATALOG-PAGE-BODY-HEADER-SIZE-INVALID",
                                "storage.catalog_page_body.header_size_invalid",
                                std::to_string(page_number));
  }

  const u64 stored_checksum = LoadLittle64(body.data() + kOffsetBodyChecksum);
  const u64 expected_checksum = ComputeCatalogPageBodyChecksum(body);
  if (stored_checksum != expected_checksum) {
    return CatalogPageBodyError("SB-CATALOG-PAGE-BODY-CHECKSUM-MISMATCH",
                                "storage.catalog_page_body.checksum_mismatch",
                                std::to_string(page_number));
  }

  const u32 row_count = LoadLittle32(body.data() + kOffsetRowCount);
  const u32 body_bytes = LoadLittle32(body.data() + kOffsetBodyBytes);
  if (body_bytes > body.size() || body_bytes < kCatalogPageBodyHeaderBytes) {
    return CatalogPageBodyError("SB-CATALOG-PAGE-BODY-BYTES-INVALID",
                                "storage.catalog_page_body.bytes_invalid",
                                std::to_string(page_number));
  }

  CatalogPageBody parsed;
  parsed.page_sequence = LoadLittle32(body.data() + kOffsetPageSequence);
  parsed.page_number = page_number;
  parsed.next_page_number = LoadLittle64(body.data() + kOffsetNextPageNumber);

  u32 offset = kCatalogPageBodyHeaderBytes;
  for (u32 i = 0; i < row_count; ++i) {
    if (offset + kRowHeaderBytes > body_bytes) {
      return CatalogPageBodyError("SB-CATALOG-PAGE-BODY-ROW-SHORT",
                                  "storage.catalog_page_body.row_short",
                                  std::to_string(page_number));
    }
    CatalogPageRow row;
    row.kind = static_cast<CatalogPageRowKind>(LoadLittle16(body.data() + offset + kRowOffsetKind));
    row.ordinal = LoadLittle32(body.data() + offset + kRowOffsetOrdinal);
    const u32 payload_bytes = LoadLittle32(body.data() + offset + kRowOffsetPayloadBytes);
    const u64 payload_checksum = LoadLittle64(body.data() + offset + kRowOffsetPayloadChecksum);
    offset += kRowHeaderBytes;
    if (offset + payload_bytes > body_bytes) {
      return CatalogPageBodyError("SB-CATALOG-PAGE-BODY-PAYLOAD-SHORT",
                                  "storage.catalog_page_body.payload_short",
                                  std::to_string(page_number));
    }
    row.payload.assign(reinterpret_cast<const char*>(body.data() + offset), payload_bytes);
    if (payload_checksum != Fnv1a64(row.payload)) {
      return CatalogPageBodyError("SB-CATALOG-PAGE-BODY-PAYLOAD-CHECKSUM-MISMATCH",
                                  "storage.catalog_page_body.payload_checksum_mismatch",
                                  std::to_string(row.ordinal));
    }
    parsed.rows.push_back(std::move(row));
    offset += payload_bytes;
  }

  CatalogPageBodyResult result;
  result.status = CatalogPageOkStatus();
  result.body = std::move(parsed);
  return result;
}

DiagnosticRecord MakeCatalogPageDiagnostic(Status status,
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
                        "storage.page.catalog_body");
}

}  // namespace scratchbird::storage::page
