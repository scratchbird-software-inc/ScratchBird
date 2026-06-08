// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_format.hpp"

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
constexpr u32 kOffsetFormatMajor = 8;
constexpr u32 kOffsetFormatMinor = 12;
constexpr u32 kOffsetHeaderBytes = 16;
constexpr u32 kOffsetPageSize = 20;
constexpr u32 kOffsetChecksumAlgorithm = 24;
constexpr u32 kOffsetDatabaseUuid = 32;
constexpr u32 kOffsetCreationMillis = 48;
constexpr u32 kOffsetFeatureFlags = 56;
constexpr u32 kOffsetCompatibilityFlags = 64;
constexpr u32 kOffsetHeaderChecksum = 72;
constexpr u32 kDatabaseFormatMajorCurrent = kScratchBirdDatabaseFormatMajor;
constexpr u32 kDatabaseFormatMajorMinSupported = kScratchBirdDatabaseFormatMajor;
constexpr u32 kDatabaseFormatMajorMaxSupported = kScratchBirdDatabaseFormatMajor;
constexpr u32 kDatabaseFormatMinorCurrent = kScratchBirdDatabaseFormatMinor;
constexpr u32 kDatabaseFormatMinorMinSupported = 0;
constexpr u32 kDatabaseFormatMinorMaxSupported = kScratchBirdDatabaseFormatMinor;

Status DatabaseOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::storage_disk};
}

Status DatabaseErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::storage_disk};
}

bool IsPowerOfTwo(u32 value) {
  return value != 0 && (value & (value - 1)) == 0;
}

void Store16(SerializedDatabaseHeader* serialized, u32 offset, u16 value) {
  const u16 stored = HostToLittle16(value);
  std::memcpy(serialized->data() + offset, &stored, sizeof(stored));
}

void Store32(SerializedDatabaseHeader* serialized, u32 offset, u32 value) {
  const u32 stored = HostToLittle32(value);
  std::memcpy(serialized->data() + offset, &stored, sizeof(stored));
}

void Store64(SerializedDatabaseHeader* serialized, u32 offset, u64 value) {
  const u64 stored = HostToLittle64(value);
  std::memcpy(serialized->data() + offset, &stored, sizeof(stored));
}

u16 Load16(const SerializedDatabaseHeader& serialized, u32 offset) {
  u16 value = 0;
  std::memcpy(&value, serialized.data() + offset, sizeof(value));
  return LittleToHost16(value);
}

u32 Load32(const SerializedDatabaseHeader& serialized, u32 offset) {
  u32 value = 0;
  std::memcpy(&value, serialized.data() + offset, sizeof(value));
  return LittleToHost32(value);
}

u64 Load64(const SerializedDatabaseHeader& serialized, u32 offset) {
  u64 value = 0;
  std::memcpy(&value, serialized.data() + offset, sizeof(value));
  return LittleToHost64(value);
}

SerializedDatabaseHeader ZeroChecksumField(SerializedDatabaseHeader serialized) {
  Store64(&serialized, kOffsetHeaderChecksum, 0);
  return serialized;
}

DatabaseHeaderResult HeaderError(std::string diagnostic_code,
                                 std::string message_key,
                                 std::string detail = {}) {
  DatabaseHeaderResult result;
  result.status = DatabaseErrorStatus();
  result.diagnostic = MakeDatabaseFormatDiagnostic(result.status,
                                                   std::move(diagnostic_code),
                                                   std::move(message_key),
                                                   std::move(detail));
  return result;
}

SerializedDatabaseHeaderResult SerializedHeaderError(std::string diagnostic_code,
                                                     std::string message_key,
                                                     std::string detail = {}) {
  SerializedDatabaseHeaderResult result;
  result.status = DatabaseErrorStatus();
  result.diagnostic = MakeDatabaseFormatDiagnostic(result.status,
                                                   std::move(diagnostic_code),
                                                   std::move(message_key),
                                                   std::move(detail));
  return result;
}

}  // namespace

bool IsSupportedDatabasePageSize(u32 page_size) {
  return IsPowerOfTwo(page_size) &&
         std::find(kSupportedDatabasePageSizes.begin(),
                   kSupportedDatabasePageSizes.end(),
                   page_size) != kSupportedDatabasePageSizes.end();
}

bool IsDatabaseUuidV7(const Uuid& uuid) {
  return !uuid.is_nil() &&
         ((uuid.bytes[8] & 0xc0u) == 0x80u) &&
         (((uuid.bytes[6] >> 4) & 0x0fu) == 7u);
}

PageSizeProfile PageSizeProfileFor(u32 page_size) {
  return static_cast<PageSizeProfile>(page_size);
}

const char* ChecksumAlgorithmName(ChecksumAlgorithm algorithm) {
  switch (algorithm) {
    case ChecksumAlgorithm::none: return "none";
    case ChecksumAlgorithm::fnv1a64: return "fnv1a64";
  }
  return "unknown";
}

u64 ComputeDatabaseHeaderChecksum(const SerializedDatabaseHeader& serialized) {
  const SerializedDatabaseHeader normalized = ZeroChecksumField(serialized);
  u64 hash = 1469598103934665603ull;
  for (byte value : normalized) {
    hash ^= value;
    hash *= 1099511628211ull;
  }
  return hash;
}

DatabaseHeaderResult MakeDatabaseHeader(Uuid database_uuid,
                                        u32 page_size,
                                        u64 creation_unix_epoch_millis,
                                        u64 feature_flags,
                                        u64 compatibility_flags) {
  DatabaseHeader header;
  header.database_uuid = database_uuid;
  header.page_size = page_size;
  header.creation_unix_epoch_millis = creation_unix_epoch_millis;
  header.feature_flags = feature_flags;
  header.compatibility_flags = compatibility_flags;
  return ValidateDatabaseHeader(header);
}

DatabaseHeaderResult ValidateDatabaseHeader(const DatabaseHeader& header) {
  if (header.format_major < kDatabaseFormatMajorMinSupported) {
    return HeaderError("FORMAT.VERSION_TOO_OLD",
                       "storage.database.format_version_too_old",
                       "database_format_major=" + std::to_string(header.format_major) +
                           " supported_min=" + std::to_string(kDatabaseFormatMajorMinSupported) +
                           " migration_policy=explicit_supported_only");
  }
  if (header.format_major > kDatabaseFormatMajorMaxSupported ||
      (header.format_major == kDatabaseFormatMajorCurrent &&
       header.format_minor > kDatabaseFormatMinorMaxSupported)) {
    return HeaderError("FORMAT.VERSION_UNSUPPORTED",
                       "storage.database.format_version_unsupported",
                       "database_format=" + std::to_string(header.format_major) + "." +
                           std::to_string(header.format_minor) +
                           " supported_max=" + std::to_string(kDatabaseFormatMajorMaxSupported) +
                           "." + std::to_string(kDatabaseFormatMinorMaxSupported) +
                           " newer_than_supported_refusal=1");
  }
  if (header.format_minor < kDatabaseFormatMinorMinSupported) {
    return HeaderError("FORMAT.VERSION_DOWNGRADE_REFUSED",
                       "storage.database.format_version_downgrade_refused",
                       "database_format_minor=" + std::to_string(header.format_minor) +
                           " supported_min_minor=" +
                           std::to_string(kDatabaseFormatMinorMinSupported));
  }
  constexpr u64 kKnownCompatibilityFlags =
      DatabaseCompatibilityFlag::public_node_safe_header_open |
      DatabaseCompatibilityFlag::requires_cluster_authority |
      DatabaseCompatibilityFlag::requires_decryption_password |
      DatabaseCompatibilityFlag::unknown_page_safe_classification_required;
  const u64 unknown_required_flags = header.compatibility_flags & ~kKnownCompatibilityFlags;
  if (unknown_required_flags != 0) {
    return HeaderError("FORMAT.UNKNOWN_REQUIRED_FLAG",
                       "storage.database.unknown_required_compatibility_flag",
                       std::to_string(unknown_required_flags));
  }
  if (header.header_bytes != kDatabaseHeaderSerializedBytes) {
    return HeaderError("SB-STORAGE-DATABASE-HEADER-SIZE-INVALID",
                       "storage.database.header_size_invalid",
                       std::to_string(header.header_bytes));
  }
  if (!IsSupportedDatabasePageSize(header.page_size)) {
    return HeaderError("SB-STORAGE-DATABASE-PAGE-SIZE-INVALID",
                       "storage.database.page_size_invalid",
                       std::to_string(header.page_size));
  }
  if (!IsDatabaseUuidV7(header.database_uuid)) {
    return HeaderError("SB-STORAGE-DATABASE-UUID-NOT-V7",
                       "storage.database.uuid_not_v7");
  }
  if (header.checksum_algorithm != ChecksumAlgorithm::none &&
      header.checksum_algorithm != ChecksumAlgorithm::fnv1a64) {
    return HeaderError("SB-STORAGE-DATABASE-CHECKSUM-UNKNOWN",
                       "storage.database.checksum_unknown",
                       std::to_string(static_cast<u16>(header.checksum_algorithm)));
  }

  DatabaseHeaderResult result;
  result.status = DatabaseOkStatus();
  result.header = header;
  return result;
}

SerializedDatabaseHeaderResult SerializeDatabaseHeader(const DatabaseHeader& header) {
  DatabaseHeaderResult validated = ValidateDatabaseHeader(header);
  if (!validated.ok()) {
    SerializedDatabaseHeaderResult result;
    result.status = validated.status;
    result.diagnostic = validated.diagnostic;
    return result;
  }

  SerializedDatabaseHeaderResult result;
  result.status = DatabaseOkStatus();
  SerializedDatabaseHeader serialized{};
  std::copy(kScratchBirdDatabaseMagic.begin(), kScratchBirdDatabaseMagic.end(), serialized.begin() + kOffsetMagic);
  Store32(&serialized, kOffsetFormatMajor, header.format_major);
  Store32(&serialized, kOffsetFormatMinor, header.format_minor);
  Store32(&serialized, kOffsetHeaderBytes, header.header_bytes);
  Store32(&serialized, kOffsetPageSize, header.page_size);
  Store16(&serialized, kOffsetChecksumAlgorithm, static_cast<u16>(header.checksum_algorithm));
  std::copy(header.database_uuid.bytes.begin(), header.database_uuid.bytes.end(), serialized.begin() + kOffsetDatabaseUuid);
  Store64(&serialized, kOffsetCreationMillis, header.creation_unix_epoch_millis);
  Store64(&serialized, kOffsetFeatureFlags, header.feature_flags);
  Store64(&serialized, kOffsetCompatibilityFlags, header.compatibility_flags);
  const u64 checksum = header.checksum_algorithm == ChecksumAlgorithm::none ? 0 : ComputeDatabaseHeaderChecksum(serialized);
  Store64(&serialized, kOffsetHeaderChecksum, checksum);
  result.serialized = serialized;
  return result;
}

DatabaseHeaderResult ParseDatabaseHeader(const SerializedDatabaseHeader& serialized) {
  if (!std::equal(kScratchBirdDatabaseMagic.begin(), kScratchBirdDatabaseMagic.end(), serialized.begin() + kOffsetMagic)) {
    return HeaderError("SB-STORAGE-DATABASE-MAGIC-INVALID",
                       "storage.database.magic_invalid");
  }

  DatabaseHeader header;
  header.format_major = Load32(serialized, kOffsetFormatMajor);
  header.format_minor = Load32(serialized, kOffsetFormatMinor);
  header.header_bytes = Load32(serialized, kOffsetHeaderBytes);
  header.page_size = Load32(serialized, kOffsetPageSize);
  header.checksum_algorithm = static_cast<ChecksumAlgorithm>(Load16(serialized, kOffsetChecksumAlgorithm));
  std::copy(serialized.begin() + kOffsetDatabaseUuid,
            serialized.begin() + kOffsetDatabaseUuid + header.database_uuid.bytes.size(),
            header.database_uuid.bytes.begin());
  header.creation_unix_epoch_millis = Load64(serialized, kOffsetCreationMillis);
  header.feature_flags = Load64(serialized, kOffsetFeatureFlags);
  header.compatibility_flags = Load64(serialized, kOffsetCompatibilityFlags);
  header.header_checksum = Load64(serialized, kOffsetHeaderChecksum);

  DatabaseHeaderResult validated = ValidateDatabaseHeader(header);
  if (!validated.ok()) {
    return validated;
  }

  if (header.checksum_algorithm == ChecksumAlgorithm::fnv1a64) {
    const u64 expected = ComputeDatabaseHeaderChecksum(serialized);
    if (header.header_checksum != expected) {
      return HeaderError("SB-STORAGE-DATABASE-HEADER-CHECKSUM-MISMATCH",
                         "storage.database.header_checksum_mismatch");
    }
  }

  validated.header = header;
  return validated;
}

DiagnosticRecord MakeDatabaseFormatDiagnostic(Status status,
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
                        "storage.database_format");
}

}  // namespace scratchbird::storage::disk
