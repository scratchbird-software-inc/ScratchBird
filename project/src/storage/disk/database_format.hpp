// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

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

inline constexpr u32 kScratchBirdDatabaseFormatMajor = 1;
inline constexpr u32 kScratchBirdDatabaseFormatMinor = 0;
inline constexpr u32 kDatabaseHeaderSerializedBytes = 512;
inline constexpr std::array<byte, 8> kScratchBirdDatabaseMagic = {'S', 'B', 'D', 'B', 'V', '0', '0', '1'};

using SerializedDatabaseHeader = std::array<byte, kDatabaseHeaderSerializedBytes>;

enum class PageSizeProfile : u32 {
  profile_4k = 4096,
  profile_8k = 8192,
  profile_16k = 16384,
  profile_32k = 32768,
  profile_64k = 65536,
  profile_128k = 131072
};

inline constexpr std::array<u32, 6> kSupportedDatabasePageSizes = {
    static_cast<u32>(PageSizeProfile::profile_4k),
    static_cast<u32>(PageSizeProfile::profile_8k),
    static_cast<u32>(PageSizeProfile::profile_16k),
    static_cast<u32>(PageSizeProfile::profile_32k),
    static_cast<u32>(PageSizeProfile::profile_64k),
    static_cast<u32>(PageSizeProfile::profile_128k),
};

enum class ChecksumAlgorithm : u16 {
  none = 0,
  fnv1a64 = 1
};

namespace DatabaseFeatureFlag {
inline constexpr u64 encrypted_database = 1ull << 0;
inline constexpr u64 cluster_structures_present = 1ull << 1;
inline constexpr u64 variable_page_size_profile = 1ull << 2;
inline constexpr u64 compatibility_uuid_mapping_present = 1ull << 3;
}  // namespace DatabaseFeatureFlag

namespace DatabaseCompatibilityFlag {
inline constexpr u64 public_node_safe_header_open = 1ull << 0;
inline constexpr u64 requires_cluster_authority = 1ull << 1;
inline constexpr u64 requires_decryption_password = 1ull << 2;
inline constexpr u64 unknown_page_safe_classification_required = 1ull << 3;
}  // namespace DatabaseCompatibilityFlag

struct DatabaseHeader {
  u32 format_major = kScratchBirdDatabaseFormatMajor;
  u32 format_minor = kScratchBirdDatabaseFormatMinor;
  u32 header_bytes = kDatabaseHeaderSerializedBytes;
  u32 page_size = static_cast<u32>(PageSizeProfile::profile_16k);
  ChecksumAlgorithm checksum_algorithm = ChecksumAlgorithm::fnv1a64;
  Uuid database_uuid;
  u64 creation_unix_epoch_millis = 0;
  u64 feature_flags = 0;
  u64 compatibility_flags = DatabaseCompatibilityFlag::public_node_safe_header_open;
  u64 header_checksum = 0;
};

struct DatabaseHeaderResult {
  Status status;
  DatabaseHeader header;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct SerializedDatabaseHeaderResult {
  Status status;
  SerializedDatabaseHeader serialized{};
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

bool IsSupportedDatabasePageSize(u32 page_size);
bool IsDatabaseUuidV7(const Uuid& uuid);
PageSizeProfile PageSizeProfileFor(u32 page_size);
const char* ChecksumAlgorithmName(ChecksumAlgorithm algorithm);
u64 ComputeDatabaseHeaderChecksum(const SerializedDatabaseHeader& serialized);
DatabaseHeaderResult MakeDatabaseHeader(Uuid database_uuid,
                                        u32 page_size,
                                        u64 creation_unix_epoch_millis,
                                        u64 feature_flags,
                                        u64 compatibility_flags);
DatabaseHeaderResult ValidateDatabaseHeader(const DatabaseHeader& header);
SerializedDatabaseHeaderResult SerializeDatabaseHeader(const DatabaseHeader& header);
DatabaseHeaderResult ParseDatabaseHeader(const SerializedDatabaseHeader& serialized);
DiagnosticRecord MakeDatabaseFormatDiagnostic(Status status,
                                              std::string diagnostic_code,
                                              std::string message_key,
                                              std::string detail = {});

}  // namespace scratchbird::storage::disk
