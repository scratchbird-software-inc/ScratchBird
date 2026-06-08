// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "structured_page_body.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

namespace scratchbird::storage::page {
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
using scratchbird::storage::disk::PageTypeName;
using scratchbird::storage::disk::kPageHeaderSerializedBytes;

inline constexpr std::array<byte, 8> kStructuredPageBodyMagic = {
    'S', 'B', 'S', 'T', 'R', '0', '0', '1'};
inline constexpr u32 kOffsetMagic = 0;
inline constexpr u32 kOffsetHeaderBytes = 8;
inline constexpr u32 kOffsetFormatMajor = 12;
inline constexpr u32 kOffsetFormatMinor = 14;
inline constexpr u32 kOffsetPageType = 16;
inline constexpr u32 kOffsetPageFamily = 20;
inline constexpr u32 kOffsetPageNumber = 24;
inline constexpr u32 kOffsetGeneration = 32;
inline constexpr u32 kOffsetRecordCount = 40;
inline constexpr u32 kOffsetRecordsBytes = 44;
inline constexpr u32 kOffsetRecordsOffset = 48;
inline constexpr u32 kOffsetChecksum = 56;

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::storage_page};
}

Status ErrorStatus() {
  return {StatusCode::platform_required_feature_missing,
          Severity::error,
          Subsystem::storage_page};
}

StructuredPageBodyResult StructuredPageBodyError(std::string diagnostic_code,
                                                 std::string message_key,
                                                 std::string detail = {}) {
  StructuredPageBodyResult result;
  result.status = ErrorStatus();
  result.diagnostic = MakeStructuredPageBodyDiagnostic(result.status,
                                                       std::move(diagnostic_code),
                                                       std::move(message_key),
                                                       std::move(detail));
  return result;
}

void Store32(std::vector<byte>* out, u32 value) {
  value = HostToLittle32(value);
  const auto* ptr = reinterpret_cast<const byte*>(&value);
  out->insert(out->end(), ptr, ptr + sizeof(value));
}

void StoreLittle16At(std::vector<byte>* out, u32 offset, u16 value) {
  value = HostToLittle16(value);
  std::memcpy(out->data() + offset, &value, sizeof(value));
}

void StoreLittle32At(std::vector<byte>* out, u32 offset, u32 value) {
  value = HostToLittle32(value);
  std::memcpy(out->data() + offset, &value, sizeof(value));
}

void StoreLittle64At(std::vector<byte>* out, u32 offset, u64 value) {
  value = HostToLittle64(value);
  std::memcpy(out->data() + offset, &value, sizeof(value));
}

u32 Load32(const std::vector<byte>& in, std::size_t* offset) {
  u32 value = 0;
  std::memcpy(&value, in.data() + *offset, sizeof(value));
  *offset += sizeof(value);
  return LittleToHost32(value);
}

u16 LoadLittle16At(const std::vector<byte>& in, u32 offset) {
  u16 value = 0;
  std::memcpy(&value, in.data() + offset, sizeof(value));
  return LittleToHost16(value);
}

u32 LoadLittle32At(const std::vector<byte>& in, u32 offset) {
  u32 value = 0;
  std::memcpy(&value, in.data() + offset, sizeof(value));
  return LittleToHost32(value);
}

u64 LoadLittle64At(const std::vector<byte>& in, u32 offset) {
  u64 value = 0;
  std::memcpy(&value, in.data() + offset, sizeof(value));
  return LittleToHost64(value);
}

bool SameRecordIdentity(const StructuredPageBodyRecord& left,
                        const StructuredPageBodyRecord& right) {
  return left.record_kind == right.record_kind && left.key == right.key;
}

std::vector<byte> SerializeRecords(
    const std::vector<StructuredPageBodyRecord>& records) {
  std::vector<byte> out;
  for (const auto& record : records) {
    Store32(&out, record.record_kind);
    Store32(&out, record.ordinal);
    Store32(&out, static_cast<u32>(record.key.size()));
    Store32(&out, static_cast<u32>(record.payload.size()));
    out.insert(out.end(), record.key.begin(), record.key.end());
    out.insert(out.end(), record.payload.begin(), record.payload.end());
  }
  return out;
}

}  // namespace

const char* StructuredPageBodyMutationKindName(
    StructuredPageBodyMutationKind kind) {
  switch (kind) {
    case StructuredPageBodyMutationKind::upsert_record: return "upsert_record";
    case StructuredPageBodyMutationKind::delete_record: return "delete_record";
    case StructuredPageBodyMutationKind::clear_records: return "clear_records";
  }
  return "unknown";
}

u64 ComputeStructuredPageBodyChecksum(const std::vector<byte>& record_bytes) {
  u64 hash = 14695981039346656037ull;
  for (byte value : record_bytes) {
    hash ^= static_cast<u64>(value);
    hash *= 1099511628211ull;
  }
  return hash;
}

StructuredPageBodyResult BuildStructuredPageBody(const StructuredPageBody& body,
                                                 u32 page_size) {
  if (page_size <= kPageHeaderSerializedBytes + kStructuredPageBodyHeaderBytes) {
    return StructuredPageBodyError("SB-STRUCTURED-PAGE-BODY-PAGE-SIZE-TOO-SMALL",
                                   "storage.structured_page_body.page_size_too_small",
                                   std::to_string(page_size));
  }
  const auto family = LookupPageFamily(body.page_type);
  if (!family.ok() || family.descriptor.cluster_only ||
      family.descriptor.encrypted_or_opaque || family.descriptor.reserved) {
    return StructuredPageBodyError("SB-STRUCTURED-PAGE-BODY-PAGE-TYPE-REFUSED",
                                   "storage.structured_page_body.page_type_refused",
                                   PageTypeName(body.page_type));
  }
  const PageFamily effective_family =
      body.page_family == PageFamily::unknown ? family.descriptor.family : body.page_family;
  if (effective_family != family.descriptor.family) {
    return StructuredPageBodyError("SB-STRUCTURED-PAGE-BODY-FAMILY-MISMATCH",
                                   "storage.structured_page_body.family_mismatch",
                                   PageFamilyName(effective_family));
  }
  if (body.generation == 0) {
    return StructuredPageBodyError("SB-STRUCTURED-PAGE-BODY-GENERATION-INVALID",
                                   "storage.structured_page_body.generation_invalid");
  }

  const std::vector<byte> records = SerializeRecords(body.records);
  const u32 body_capacity = page_size - kPageHeaderSerializedBytes;
  if (records.size() > body_capacity - kStructuredPageBodyHeaderBytes) {
    return StructuredPageBodyError("SB-STRUCTURED-PAGE-BODY-TOO-LARGE",
                                   "storage.structured_page_body.too_large",
                                   std::to_string(records.size()));
  }

  StructuredPageBodyResult result;
  result.serialized.assign(kStructuredPageBodyHeaderBytes, 0);
  std::copy(kStructuredPageBodyMagic.begin(),
            kStructuredPageBodyMagic.end(),
            result.serialized.begin() + kOffsetMagic);
  StoreLittle32At(&result.serialized,
                  kOffsetHeaderBytes,
                  kStructuredPageBodyHeaderBytes);
  StoreLittle16At(&result.serialized,
                  kOffsetFormatMajor,
                  kStructuredPageBodyFormatMajor);
  StoreLittle16At(&result.serialized,
                  kOffsetFormatMinor,
                  kStructuredPageBodyFormatMinor);
  StoreLittle32At(&result.serialized,
                  kOffsetPageType,
                  static_cast<u32>(body.page_type));
  StoreLittle32At(&result.serialized,
                  kOffsetPageFamily,
                  static_cast<u32>(effective_family));
  StoreLittle64At(&result.serialized, kOffsetPageNumber, body.page_number);
  StoreLittle64At(&result.serialized, kOffsetGeneration, body.generation);
  StoreLittle32At(&result.serialized,
                  kOffsetRecordCount,
                  static_cast<u32>(body.records.size()));
  StoreLittle32At(&result.serialized,
                  kOffsetRecordsBytes,
                  static_cast<u32>(records.size()));
  StoreLittle32At(&result.serialized,
                  kOffsetRecordsOffset,
                  kStructuredPageBodyHeaderBytes);
  StoreLittle64At(&result.serialized,
                  kOffsetChecksum,
                  ComputeStructuredPageBodyChecksum(records));
  result.serialized.insert(result.serialized.end(), records.begin(), records.end());
  result.serialized.resize(body_capacity, 0);
  result.body = body;
  result.body.page_family = effective_family;
  result.status = OkStatus();
  return result;
}

StructuredPageBodyResult ParseStructuredPageBody(
    const std::vector<byte>& serialized) {
  if (serialized.size() < kStructuredPageBodyHeaderBytes) {
    return StructuredPageBodyError("SB-STRUCTURED-PAGE-BODY-TRUNCATED",
                                   "storage.structured_page_body.truncated");
  }
  if (!std::equal(kStructuredPageBodyMagic.begin(),
                  kStructuredPageBodyMagic.end(),
                  serialized.begin() + kOffsetMagic)) {
    return StructuredPageBodyError("SB-STRUCTURED-PAGE-BODY-MAGIC-INVALID",
                                   "storage.structured_page_body.magic_invalid");
  }
  if (LoadLittle32At(serialized, kOffsetHeaderBytes) !=
      kStructuredPageBodyHeaderBytes) {
    return StructuredPageBodyError("SB-STRUCTURED-PAGE-BODY-HEADER-BYTES-INVALID",
                                   "storage.structured_page_body.header_bytes_invalid");
  }
  if (LoadLittle16At(serialized, kOffsetFormatMajor) !=
          kStructuredPageBodyFormatMajor ||
      LoadLittle16At(serialized, kOffsetFormatMinor) >
          kStructuredPageBodyFormatMinor) {
    return StructuredPageBodyError("SB-STRUCTURED-PAGE-BODY-FORMAT-UNSUPPORTED",
                                   "storage.structured_page_body.format_unsupported");
  }

  const u32 records_offset = LoadLittle32At(serialized, kOffsetRecordsOffset);
  const u32 records_bytes = LoadLittle32At(serialized, kOffsetRecordsBytes);
  if (records_offset != kStructuredPageBodyHeaderBytes ||
      records_offset > serialized.size() ||
      records_bytes > serialized.size() - records_offset) {
    return StructuredPageBodyError("SB-STRUCTURED-PAGE-BODY-RECORDS-BOUNDS-INVALID",
                                   "storage.structured_page_body.records_bounds_invalid");
  }
  const u64 expected_checksum = LoadLittle64At(serialized, kOffsetChecksum);
  const std::vector<byte> record_bytes(
      serialized.begin() + static_cast<std::ptrdiff_t>(records_offset),
      serialized.begin() + static_cast<std::ptrdiff_t>(records_offset + records_bytes));
  if (ComputeStructuredPageBodyChecksum(record_bytes) != expected_checksum) {
    return StructuredPageBodyError("SB-STRUCTURED-PAGE-BODY-CHECKSUM-MISMATCH",
                                   "storage.structured_page_body.checksum_mismatch");
  }

  StructuredPageBody body;
  body.page_type = static_cast<PageType>(LoadLittle32At(serialized, kOffsetPageType));
  body.page_family = static_cast<PageFamily>(LoadLittle32At(serialized, kOffsetPageFamily));
  body.page_number = LoadLittle64At(serialized, kOffsetPageNumber);
  body.generation = LoadLittle64At(serialized, kOffsetGeneration);
  const u32 record_count = LoadLittle32At(serialized, kOffsetRecordCount);
  if (body.generation == 0) {
    return StructuredPageBodyError("SB-STRUCTURED-PAGE-BODY-GENERATION-INVALID",
                                   "storage.structured_page_body.generation_invalid");
  }
  const auto family = LookupPageFamily(body.page_type);
  if (!family.ok() || family.descriptor.family != body.page_family) {
    return StructuredPageBodyError("SB-STRUCTURED-PAGE-BODY-FAMILY-MISMATCH",
                                   "storage.structured_page_body.family_mismatch",
                                   PageTypeName(body.page_type));
  }

  std::size_t offset = 0;
  for (u32 i = 0; i < record_count; ++i) {
    if (record_bytes.size() < offset + 16) {
      return StructuredPageBodyError("SB-STRUCTURED-PAGE-BODY-RECORD-TRUNCATED",
                                     "storage.structured_page_body.record_truncated");
    }
    StructuredPageBodyRecord record;
    record.record_kind = Load32(record_bytes, &offset);
    record.ordinal = Load32(record_bytes, &offset);
    const u32 key_size = Load32(record_bytes, &offset);
    const u32 payload_size = Load32(record_bytes, &offset);
    if (record_bytes.size() < offset + key_size ||
        record_bytes.size() - offset - key_size < payload_size) {
      return StructuredPageBodyError("SB-STRUCTURED-PAGE-BODY-RECORD-PAYLOAD-TRUNCATED",
                                     "storage.structured_page_body.record_payload_truncated");
    }
    record.key.assign(record_bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                      record_bytes.begin() + static_cast<std::ptrdiff_t>(offset + key_size));
    offset += key_size;
    record.payload.assign(record_bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                          record_bytes.begin() + static_cast<std::ptrdiff_t>(offset + payload_size));
    offset += payload_size;
    body.records.push_back(std::move(record));
  }
  if (offset != record_bytes.size()) {
    return StructuredPageBodyError("SB-STRUCTURED-PAGE-BODY-TRAILING-RECORD-BYTES",
                                   "storage.structured_page_body.trailing_record_bytes");
  }

  StructuredPageBodyResult result;
  result.status = OkStatus();
  result.body = std::move(body);
  result.serialized = serialized;
  return result;
}

StructuredPageBodyResult ApplyStructuredPageBodyMutation(
    const StructuredPageBody& body,
    const StructuredPageBodyMutation& mutation,
    u32 page_size) {
  StructuredPageBody mutated = body;
  switch (mutation.kind) {
    case StructuredPageBodyMutationKind::upsert_record: {
      auto found = std::find_if(mutated.records.begin(),
                                mutated.records.end(),
                                [&](const StructuredPageBodyRecord& current) {
                                  return SameRecordIdentity(current, mutation.record);
                                });
      if (found == mutated.records.end()) {
        mutated.records.push_back(mutation.record);
      } else {
        *found = mutation.record;
      }
      break;
    }
    case StructuredPageBodyMutationKind::delete_record:
      mutated.records.erase(
          std::remove_if(mutated.records.begin(),
                         mutated.records.end(),
                         [&](const StructuredPageBodyRecord& current) {
                           return SameRecordIdentity(current, mutation.record);
                         }),
          mutated.records.end());
      break;
    case StructuredPageBodyMutationKind::clear_records:
      mutated.records.clear();
      break;
  }
  ++mutated.generation;
  return BuildStructuredPageBody(mutated, page_size);
}

DiagnosticRecord MakeStructuredPageBodyDiagnostic(Status status,
                                                  std::string diagnostic_code,
                                                  std::string message_key,
                                                  std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "storage.structured_page_body");
}

}  // namespace scratchbird::storage::page
