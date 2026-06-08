// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "filespace_header.hpp"

#include "disk_device.hpp"
#include "runtime_platform.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <utility>
#include <vector>

namespace scratchbird::storage::filespace {
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
using scratchbird::core::platform::UuidKind;
using scratchbird::core::uuid::IsEngineIdentityUuid;
using scratchbird::core::uuid::ParseTypedUuid;
using scratchbird::core::uuid::UuidToString;
using scratchbird::storage::disk::FileDevice;
using scratchbird::storage::disk::FileOpenMode;

constexpr std::array<unsigned char, 8> kPhysicalHeaderMagic = {'S', 'B', 'F', 'S', 'P', 'H', '0', '1'};
constexpr scratchbird::core::platform::u32 kPhysicalHeaderBytes = 256;
constexpr scratchbird::core::platform::u32 kOffsetMagic = 0;
constexpr scratchbird::core::platform::u32 kOffsetChecksum = 8;
constexpr scratchbird::core::platform::u32 kOffsetDatabaseUuid = 16;
constexpr scratchbird::core::platform::u32 kOffsetFilespaceUuid = 32;
constexpr scratchbird::core::platform::u32 kOffsetRole = 48;
constexpr scratchbird::core::platform::u32 kOffsetState = 50;
constexpr scratchbird::core::platform::u32 kOffsetPageSize = 52;
constexpr scratchbird::core::platform::u32 kOffsetFormatVersion = 56;
constexpr scratchbird::core::platform::u32 kOffsetChecksumProfile = 60;
constexpr scratchbird::core::platform::u32 kOffsetEncryptionProfile = 64;
constexpr scratchbird::core::platform::u32 kOffsetCompatibilityFlags = 72;
constexpr scratchbird::core::platform::u32 kOffsetAttachPolicyFlags = 80;
constexpr scratchbird::core::platform::u32 kOffsetPhysicalFilespaceId = 88;
constexpr scratchbird::core::platform::u32 kOffsetTotalPages = 96;
constexpr scratchbird::core::platform::u32 kOffsetFreePages = 104;
constexpr scratchbird::core::platform::u32 kOffsetPreallocatedPages = 112;
constexpr scratchbird::core::platform::u32 kOffsetAllocationRootPage = 120;
constexpr scratchbird::core::platform::u32 kOffsetHeaderGeneration = 128;
constexpr scratchbird::core::platform::u32 kOffsetWriterIdentityUuid = 136;
constexpr scratchbird::core::platform::u32 kOffsetCreationOperationSize = 152;
constexpr scratchbird::core::platform::u32 kOffsetCreationOperation = 156;
constexpr scratchbird::core::platform::u32 kMaxCreationOperationBytes = 96;
static_assert(kOffsetCreationOperation + kMaxCreationOperationBytes <= kPhysicalHeaderBytes);

Status HeaderOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::storage_disk};
}

Status HeaderErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::storage_disk};
}

PhysicalFilespaceWriteResult WriteError(std::string code, std::string key, std::string detail = {}) {
  PhysicalFilespaceWriteResult result;
  result.status = HeaderErrorStatus();
  result.diagnostic = MakePhysicalFilespaceHeaderDiagnostic(result.status, std::move(code), std::move(key), std::move(detail));
  return result;
}

PhysicalFilespaceHeaderResult ReadError(std::string code, std::string key, std::string detail = {}) {
  PhysicalFilespaceHeaderResult result;
  result.status = HeaderErrorStatus();
  result.diagnostic = MakePhysicalFilespaceHeaderDiagnostic(result.status, std::move(code), std::move(key), std::move(detail));
  return result;
}

PhysicalFilespaceCapacityGrowthResult CapacityGrowthError(std::string code,
                                                          std::string key,
                                                          std::string detail = {}) {
  PhysicalFilespaceCapacityGrowthResult result;
  result.status = HeaderErrorStatus();
  result.diagnostic =
      MakePhysicalFilespaceHeaderDiagnostic(result.status, std::move(code), std::move(key), std::move(detail));
  return result;
}

bool AddOverflow(u64 left, u64 right, u64* value) {
  if (value == nullptr) {
    return true;
  }
  if (left > std::numeric_limits<u64>::max() - right) {
    return true;
  }
  *value = left + right;
  return false;
}

bool CapacityBytes(u64 total_pages, u32 page_size, u64* value) {
  if (value == nullptr || page_size == 0) {
    return false;
  }
  if (total_pages > std::numeric_limits<u64>::max() / static_cast<u64>(page_size)) {
    return false;
  }
  *value = total_pages * static_cast<u64>(page_size);
  return true;
}

PhysicalFilespaceHeaderResult ValidateCapacity(const PhysicalFilespaceHeader& header,
                                               u64* expected_capacity_bytes = nullptr) {
  if (!scratchbird::storage::disk::IsSupportedDatabasePageSize(header.page_size)) {
    return ReadError("SB-FILESPACE-HEADER-PAGE-SIZE-INVALID", "storage.filespace.header.page_size_invalid");
  }
  if (header.total_pages == 0) {
    return ReadError("SB-FILESPACE-HEADER-CAPACITY-INVALID", "storage.filespace.header.capacity_invalid", "total_pages=0");
  }
  u64 capacity_bytes = 0;
  if (!CapacityBytes(header.total_pages, header.page_size, &capacity_bytes)) {
    return ReadError("SB-FILESPACE-HEADER-CAPACITY-OVERFLOW", "storage.filespace.header.capacity_overflow");
  }
  if (header.free_pages > header.total_pages) {
    return ReadError("SB-FILESPACE-HEADER-FREE-PAGES-INVALID", "storage.filespace.header.free_pages_invalid");
  }
  if (header.preallocated_pages > header.total_pages) {
    return ReadError("SB-FILESPACE-HEADER-PREALLOCATED-PAGES-INVALID",
                     "storage.filespace.header.preallocated_pages_invalid");
  }
  u64 free_and_preallocated = 0;
  if (AddOverflow(header.free_pages, header.preallocated_pages, &free_and_preallocated) ||
      free_and_preallocated > header.total_pages) {
    return ReadError("SB-FILESPACE-HEADER-CAPACITY-WINDOW-INVALID",
                     "storage.filespace.header.capacity_window_invalid");
  }
  if (header.allocation_root_page != 0 && header.allocation_root_page >= header.total_pages) {
    return ReadError("SB-FILESPACE-HEADER-ALLOCATION-ROOT-INVALID",
                     "storage.filespace.header.allocation_root_invalid");
  }
  if (header.header_generation == 0) {
    return ReadError("SB-FILESPACE-HEADER-GENERATION-INVALID",
                     "storage.filespace.header.generation_invalid");
  }
  if (!header.writer_identity_uuid.valid() ||
      header.writer_identity_uuid.kind != UuidKind::object ||
      !IsEngineIdentityUuid(header.writer_identity_uuid.value)) {
    return ReadError("SB-FILESPACE-HEADER-WRITER-UUID-INVALID",
                     "storage.filespace.header.writer_uuid_invalid");
  }

  PhysicalFilespaceHeaderResult result;
  result.status = HeaderOkStatus();
  result.header = header;
  result.expected_capacity_bytes = capacity_bytes;
  result.file_size_matches_capacity = true;
  if (expected_capacity_bytes != nullptr) {
    *expected_capacity_bytes = capacity_bytes;
  }
  return result;
}

std::string Escape(std::string value) {
  for (char& ch : value) {
    if (ch == '\n' || ch == '\r') {
      ch = ' ';
    }
  }
  return value;
}

u64 ChecksumBytes(std::vector<unsigned char> bytes) {
  if (bytes.size() > kPhysicalHeaderBytes) {
    bytes.resize(kPhysicalHeaderBytes);
  }
  std::fill(bytes.begin() + kOffsetChecksum, bytes.begin() + kOffsetChecksum + sizeof(u64), 0);
  u64 hash = 1469598103934665603ull;
  for (const unsigned char byte : bytes) {
    hash ^= static_cast<u64>(byte);
    hash *= 1099511628211ull;
  }
  return hash;
}

void Store16(std::vector<unsigned char>* buffer, u32 offset, u16 value) {
  const u16 stored = HostToLittle16(value);
  std::memcpy(buffer->data() + offset, &stored, sizeof(stored));
}

void Store32(std::vector<unsigned char>* buffer, u32 offset, u32 value) {
  const u32 stored = HostToLittle32(value);
  std::memcpy(buffer->data() + offset, &stored, sizeof(stored));
}

void Store64(std::vector<unsigned char>* buffer, u32 offset, u64 value) {
  const u64 stored = HostToLittle64(value);
  std::memcpy(buffer->data() + offset, &stored, sizeof(stored));
}

u16 Load16(const std::vector<unsigned char>& buffer, u32 offset) {
  u16 value = 0;
  std::memcpy(&value, buffer.data() + offset, sizeof(value));
  return LittleToHost16(value);
}

u32 Load32(const std::vector<unsigned char>& buffer, u32 offset) {
  u32 value = 0;
  std::memcpy(&value, buffer.data() + offset, sizeof(value));
  return LittleToHost32(value);
}

u64 Load64(const std::vector<unsigned char>& buffer, u32 offset) {
  u64 value = 0;
  std::memcpy(&value, buffer.data() + offset, sizeof(value));
  return LittleToHost64(value);
}

std::vector<unsigned char> SerializeBinary(const PhysicalFilespaceHeader& header) {
  std::vector<unsigned char> payload(header.page_size, 0);
  std::copy(kPhysicalHeaderMagic.begin(), kPhysicalHeaderMagic.end(), payload.begin() + kOffsetMagic);
  std::copy(header.database_uuid.value.bytes.begin(), header.database_uuid.value.bytes.end(), payload.begin() + kOffsetDatabaseUuid);
  std::copy(header.filespace_uuid.value.bytes.begin(), header.filespace_uuid.value.bytes.end(), payload.begin() + kOffsetFilespaceUuid);
  Store16(&payload, kOffsetRole, static_cast<u16>(header.role));
  Store16(&payload, kOffsetState, static_cast<u16>(header.state));
  Store32(&payload, kOffsetPageSize, header.page_size);
  Store32(&payload, kOffsetFormatVersion, header.format_version);
  Store32(&payload, kOffsetChecksumProfile, header.checksum_profile);
  Store32(&payload, kOffsetEncryptionProfile, header.encryption_profile);
  Store64(&payload, kOffsetCompatibilityFlags, header.compatibility_flags);
  Store64(&payload, kOffsetAttachPolicyFlags, header.attach_policy_flags);
  Store16(&payload, kOffsetPhysicalFilespaceId, header.physical_filespace_id);
  Store64(&payload, kOffsetTotalPages, header.total_pages);
  Store64(&payload, kOffsetFreePages, header.free_pages);
  Store64(&payload, kOffsetPreallocatedPages, header.preallocated_pages);
  Store64(&payload, kOffsetAllocationRootPage, header.allocation_root_page);
  Store64(&payload, kOffsetHeaderGeneration, header.header_generation);
  std::copy(header.writer_identity_uuid.value.bytes.begin(),
            header.writer_identity_uuid.value.bytes.end(),
            payload.begin() + kOffsetWriterIdentityUuid);
  const std::string escaped_creation = Escape(header.creation_operation_uuid);
  const u32 creation_size = static_cast<u32>(std::min<std::size_t>(escaped_creation.size(), kMaxCreationOperationBytes));
  Store32(&payload, kOffsetCreationOperationSize, creation_size);
  std::memcpy(payload.data() + kOffsetCreationOperation, escaped_creation.data(), creation_size);
  Store64(&payload, kOffsetChecksum, ChecksumBytes(payload));
  return payload;
}

PhysicalFilespaceHeaderResult ParseBinaryHeader(const std::vector<unsigned char>& buffer,
                                                u64 file_size_bytes) {
  if (buffer.size() < kPhysicalHeaderBytes) {
    return ReadError("SB-FILESPACE-HEADER-READ-SHORT", "storage.filespace.header.read_short");
  }
  if (!std::equal(kPhysicalHeaderMagic.begin(), kPhysicalHeaderMagic.end(), buffer.begin() + kOffsetMagic)) {
    return ReadError("SB-FILESPACE-HEADER-MAGIC-INVALID", "storage.filespace.header.magic_invalid");
  }
  const u64 stored_checksum = Load64(buffer, kOffsetChecksum);
  if (stored_checksum != ChecksumBytes(buffer)) {
    return ReadError("SB-FILESPACE-HEADER-CHECKSUM-MISMATCH", "storage.filespace.header.checksum_mismatch");
  }
  const u32 page_size = Load32(buffer, kOffsetPageSize);
  if (!scratchbird::storage::disk::IsSupportedDatabasePageSize(page_size)) {
    return ReadError("SB-FILESPACE-HEADER-PAGE-SIZE-INVALID", "storage.filespace.header.page_size_invalid");
  }

  PhysicalFilespaceHeaderResult result;
  result.status = HeaderOkStatus();
  result.header.database_uuid.kind = UuidKind::database;
  result.header.filespace_uuid.kind = UuidKind::filespace;
  std::copy(buffer.begin() + kOffsetDatabaseUuid,
            buffer.begin() + kOffsetDatabaseUuid + result.header.database_uuid.value.bytes.size(),
            result.header.database_uuid.value.bytes.begin());
  std::copy(buffer.begin() + kOffsetFilespaceUuid,
            buffer.begin() + kOffsetFilespaceUuid + result.header.filespace_uuid.value.bytes.size(),
            result.header.filespace_uuid.value.bytes.begin());
  if (!IsEngineIdentityUuid(result.header.database_uuid.value) ||
      !IsEngineIdentityUuid(result.header.filespace_uuid.value)) {
    return ReadError("SB-FILESPACE-HEADER-UUID-INVALID", "storage.filespace.header.uuid_invalid");
  }
  result.header.role = static_cast<FilespaceRole>(Load16(buffer, kOffsetRole));
  result.header.state = static_cast<FilespaceState>(Load16(buffer, kOffsetState));
  result.header.page_size = page_size;
  result.header.format_version = Load32(buffer, kOffsetFormatVersion);
  result.header.checksum_profile = Load32(buffer, kOffsetChecksumProfile);
  result.header.encryption_profile = Load32(buffer, kOffsetEncryptionProfile);
  result.header.compatibility_flags = Load64(buffer, kOffsetCompatibilityFlags);
  result.header.attach_policy_flags = Load64(buffer, kOffsetAttachPolicyFlags);
  result.header.physical_filespace_id = Load16(buffer, kOffsetPhysicalFilespaceId);
  result.header.total_pages = Load64(buffer, kOffsetTotalPages);
  result.header.free_pages = Load64(buffer, kOffsetFreePages);
  result.header.preallocated_pages = Load64(buffer, kOffsetPreallocatedPages);
  result.header.allocation_root_page = Load64(buffer, kOffsetAllocationRootPage);
  result.header.header_generation = Load64(buffer, kOffsetHeaderGeneration);
  result.header.writer_identity_uuid.kind = UuidKind::object;
  std::copy(buffer.begin() + kOffsetWriterIdentityUuid,
            buffer.begin() + kOffsetWriterIdentityUuid + result.header.writer_identity_uuid.value.bytes.size(),
            result.header.writer_identity_uuid.value.bytes.begin());
  const u32 creation_size = std::min(Load32(buffer, kOffsetCreationOperationSize), kMaxCreationOperationBytes);
  result.header.creation_operation_uuid.assign(reinterpret_cast<const char*>(buffer.data() + kOffsetCreationOperation),
                                               creation_size);
  u64 expected_capacity_bytes = 0;
  const auto capacity = ValidateCapacity(result.header, &expected_capacity_bytes);
  if (!capacity.ok()) {
    return capacity;
  }
  result.file_size_bytes = file_size_bytes;
  result.expected_capacity_bytes = expected_capacity_bytes;
  result.file_size_matches_capacity = file_size_bytes == expected_capacity_bytes;
  if (result.header.page_size != 0 && (file_size_bytes % result.header.page_size) != 0) {
    return ReadError("SB-FILESPACE-HEADER-FILE-SIZE-UNALIGNED",
                     "storage.filespace.header.file_size_unaligned",
                     std::to_string(file_size_bytes));
  }
  if (!result.file_size_matches_capacity) {
    return ReadError("SB-FILESPACE-HEADER-FILE-SIZE-CAPACITY-MISMATCH",
                     "storage.filespace.header.file_size_capacity_mismatch",
                     std::to_string(file_size_bytes) + ":" + std::to_string(expected_capacity_bytes));
  }
  return result;
}

std::map<std::string, std::string> ParseFields(const std::string& payload) {
  std::map<std::string, std::string> fields;
  std::stringstream stream(payload);
  std::string line;
  bool header = false;
  while (std::getline(stream, line)) {
    if (!header) {
      if (line == "scratchbird.filespace.header.v1") {
        header = true;
      }
      continue;
    }
    const auto split = line.find('=');
    if (split != std::string::npos) {
      fields[line.substr(0, split)] = line.substr(split + 1);
    }
  }
  return fields;
}

u16 ToU16(const std::string& text) {
  return static_cast<u16>(std::strtoul(text.c_str(), nullptr, 10));
}

u32 ToU32(const std::string& text) {
  return static_cast<u32>(std::strtoul(text.c_str(), nullptr, 10));
}

u64 ToU64(const std::string& text) {
  return static_cast<u64>(std::strtoull(text.c_str(), nullptr, 10));
}

}  // namespace

PhysicalFilespaceWriteResult CreatePhysicalFilespaceFile(const std::string& path,
                                                         const PhysicalFilespaceHeader& header,
                                                         bool allow_overwrite) {
  return WritePhysicalFilespaceHeader(path, header, allow_overwrite);
}

PhysicalFilespaceWriteResult WritePhysicalFilespaceHeader(const std::string& path,
                                                          const PhysicalFilespaceHeader& header,
                                                          bool allow_overwrite) {
  if (path.empty()) {
    return WriteError("SB-FILESPACE-HEADER-PATH-REQUIRED", "storage.filespace.header.path_required");
  }
  if (!header.database_uuid.valid() || !header.filespace_uuid.valid() ||
      header.database_uuid.kind != UuidKind::database ||
      header.filespace_uuid.kind != UuidKind::filespace ||
      !IsEngineIdentityUuid(header.database_uuid.value) ||
      !IsEngineIdentityUuid(header.filespace_uuid.value)) {
    return WriteError("SB-FILESPACE-HEADER-UUID-INVALID", "storage.filespace.header.uuid_invalid");
  }
  if (!scratchbird::storage::disk::IsSupportedDatabasePageSize(header.page_size)) {
    return WriteError("SB-FILESPACE-HEADER-PAGE-SIZE-INVALID", "storage.filespace.header.page_size_invalid");
  }
  u64 expected_capacity_bytes = 0;
  const auto capacity = ValidateCapacity(header, &expected_capacity_bytes);
  if (!capacity.ok()) {
    PhysicalFilespaceWriteResult result;
    result.status = capacity.status;
    result.diagnostic = capacity.diagnostic;
    return result;
  }

  FileDevice device;
  const auto open = device.Open(path, allow_overwrite ? FileOpenMode::create_or_truncate : FileOpenMode::create_new);
  if (!open.ok()) {
    PhysicalFilespaceWriteResult result;
    result.status = open.status;
    result.diagnostic = open.diagnostic;
    return result;
  }
  const auto payload = SerializeBinary(header);
  const auto write = device.WriteAt(0, payload.data(), payload.size());
  if (!write.ok()) {
    PhysicalFilespaceWriteResult result;
    result.status = write.status;
    result.diagnostic = write.diagnostic;
    return result;
  }
  if (expected_capacity_bytes > payload.size()) {
    const unsigned char zero = 0;
    const auto extend = device.WriteAt(expected_capacity_bytes - 1, &zero, sizeof(zero));
    if (!extend.ok()) {
      PhysicalFilespaceWriteResult result;
      result.status = extend.status;
      result.diagnostic = extend.diagnostic;
      return result;
    }
  }
  const auto sync = device.Sync();
  if (!sync.ok()) {
    PhysicalFilespaceWriteResult result;
    result.status = sync.status;
    result.diagnostic = sync.diagnostic;
    return result;
  }
  const auto close = device.Close();
  if (!close.ok()) {
    PhysicalFilespaceWriteResult result;
    result.status = close.status;
    result.diagnostic = close.diagnostic;
    return result;
  }
  PhysicalFilespaceWriteResult result;
  result.status = HeaderOkStatus();
  return result;
}

PhysicalFilespaceCapacityGrowthResult ExtendPhysicalFilespaceCapacity(
    const std::string& path,
    u64 expected_total_pages_before,
    u64 expected_preallocated_pages_before,
    u64 growth_pages,
    bool reserve_growth_as_preallocated,
    u64 header_generation_after) {
  if (path.empty()) {
    return CapacityGrowthError("SB-FILESPACE-HEADER-PATH-REQUIRED",
                               "storage.filespace.header.path_required");
  }
  if (expected_total_pages_before == 0 || growth_pages == 0) {
    return CapacityGrowthError("SB-FILESPACE-HEADER-GROWTH-CAPACITY-INVALID",
                               "storage.filespace.header.growth_capacity_invalid",
                               "expected_total_pages_before and growth_pages must be non-zero");
  }

  const auto before = ReadPhysicalFilespaceHeader(path);
  if (!before.ok()) {
    PhysicalFilespaceCapacityGrowthResult result;
    result.status = before.status;
    result.diagnostic = before.diagnostic;
    return result;
  }
  if (before.header.total_pages != expected_total_pages_before) {
    return CapacityGrowthError("SB-FILESPACE-HEADER-GROWTH-BASE-MISMATCH",
                               "storage.filespace.header.growth_base_mismatch",
                               std::to_string(before.header.total_pages) + ":" +
                                   std::to_string(expected_total_pages_before));
  }
  if (before.header.preallocated_pages != expected_preallocated_pages_before) {
    return CapacityGrowthError("SB-FILESPACE-HEADER-GROWTH-PREALLOCATED-MISMATCH",
                               "storage.filespace.header.growth_preallocated_mismatch",
                               std::to_string(before.header.preallocated_pages) + ":" +
                                   std::to_string(expected_preallocated_pages_before));
  }

  PhysicalFilespaceHeader after = before.header;
  if (AddOverflow(after.total_pages, growth_pages, &after.total_pages)) {
    return CapacityGrowthError("SB-FILESPACE-HEADER-CAPACITY-OVERFLOW",
                               "storage.filespace.header.capacity_overflow");
  }
  if (reserve_growth_as_preallocated) {
    if (AddOverflow(after.preallocated_pages, growth_pages, &after.preallocated_pages)) {
      return CapacityGrowthError("SB-FILESPACE-HEADER-CAPACITY-WINDOW-INVALID",
                                 "storage.filespace.header.capacity_window_invalid",
                                 "preallocated page count overflowed");
    }
  } else if (AddOverflow(after.free_pages, growth_pages, &after.free_pages)) {
    return CapacityGrowthError("SB-FILESPACE-HEADER-CAPACITY-WINDOW-INVALID",
                               "storage.filespace.header.capacity_window_invalid",
                               "free page count overflowed");
  }
  if (header_generation_after == 0) {
    if (before.header.header_generation == std::numeric_limits<u64>::max()) {
      return CapacityGrowthError("SB-FILESPACE-HEADER-GENERATION-OVERFLOW",
                                 "storage.filespace.header.generation_overflow");
    }
    after.header_generation = before.header.header_generation + 1;
  } else if (header_generation_after <= before.header.header_generation) {
    return CapacityGrowthError("SB-FILESPACE-HEADER-GENERATION-STALE",
                               "storage.filespace.header.generation_stale",
                               std::to_string(header_generation_after));
  } else {
    after.header_generation = header_generation_after;
  }

  u64 expected_after_bytes = 0;
  const auto capacity = ValidateCapacity(after, &expected_after_bytes);
  if (!capacity.ok()) {
    PhysicalFilespaceCapacityGrowthResult result;
    result.status = capacity.status;
    result.diagnostic = capacity.diagnostic;
    return result;
  }

  FileDevice device;
  const auto open = device.Open(path, FileOpenMode::open_existing);
  if (!open.ok()) {
    PhysicalFilespaceCapacityGrowthResult result;
    result.status = open.status;
    result.diagnostic = open.diagnostic;
    return result;
  }
  const auto size_before = device.Size();
  if (!size_before.ok()) {
    PhysicalFilespaceCapacityGrowthResult result;
    result.status = size_before.status;
    result.diagnostic = size_before.diagnostic;
    return result;
  }
  if (size_before.size_bytes != before.file_size_bytes) {
    return CapacityGrowthError("SB-FILESPACE-HEADER-GROWTH-CONCURRENT-SIZE-CHANGE",
                               "storage.filespace.header.growth_concurrent_size_change",
                               std::to_string(size_before.size_bytes) + ":" +
                                   std::to_string(before.file_size_bytes));
  }

  PhysicalFilespaceCapacityGrowthResult result;
  result.header_before = before.header;
  result.header_after = after;
  result.file_size_before_bytes = size_before.size_bytes;
  result.expected_capacity_after_bytes = expected_after_bytes;

  if (expected_after_bytes > size_before.size_bytes) {
    const unsigned char zero = 0;
    const auto extend = device.WriteAt(expected_after_bytes - 1, &zero, sizeof(zero));
    if (!extend.ok()) {
      result.status = extend.status;
      result.diagnostic = extend.diagnostic;
      return result;
    }
  }
  result.physical_extension_completed = true;

  const auto extension_sync = device.Sync();
  if (!extension_sync.ok()) {
    result.status = extension_sync.status;
    result.diagnostic = extension_sync.diagnostic;
    return result;
  }
  result.physical_extension_synced = true;

  const auto payload = SerializeBinary(after);
  const auto header_write = device.WriteAt(0, payload.data(), payload.size());
  if (!header_write.ok()) {
    result.status = header_write.status;
    result.diagnostic = header_write.diagnostic;
    return result;
  }
  const auto header_sync = device.Sync();
  if (!header_sync.ok()) {
    result.status = header_sync.status;
    result.diagnostic = header_sync.diagnostic;
    return result;
  }
  result.header_updated = true;

  const auto size_after = device.Size();
  if (!size_after.ok()) {
    result.status = size_after.status;
    result.diagnostic = size_after.diagnostic;
    return result;
  }
  result.file_size_after_bytes = size_after.size_bytes;
  if (result.file_size_after_bytes != expected_after_bytes) {
    return CapacityGrowthError("SB-FILESPACE-HEADER-GROWTH-SIZE-MISMATCH",
                               "storage.filespace.header.growth_size_mismatch",
                               std::to_string(result.file_size_after_bytes) + ":" +
                                   std::to_string(expected_after_bytes));
  }

  const auto close = device.Close();
  if (!close.ok()) {
    result.status = close.status;
    result.diagnostic = close.diagnostic;
    return result;
  }

  const auto verified = ReadPhysicalFilespaceHeader(path);
  if (!verified.ok()) {
    result.status = verified.status;
    result.diagnostic = verified.diagnostic;
    return result;
  }
  if (verified.header.total_pages != after.total_pages ||
      verified.header.preallocated_pages != after.preallocated_pages ||
      verified.header.free_pages != after.free_pages ||
      verified.header.header_generation != after.header_generation ||
      verified.file_size_bytes != expected_after_bytes) {
    return CapacityGrowthError("SB-FILESPACE-HEADER-GROWTH-VERIFY-MISMATCH",
                               "storage.filespace.header.growth_verify_mismatch");
  }

  result.status = HeaderOkStatus();
  return result;
}

PhysicalFilespaceHeaderResult ReadPhysicalFilespaceHeader(const std::string& path) {
  FileDevice device;
  const auto open = device.Open(path, FileOpenMode::open_existing_read_only);
  if (!open.ok()) {
    PhysicalFilespaceHeaderResult result;
    result.status = open.status;
    result.diagnostic = open.diagnostic;
    return result;
  }
  const auto size = device.Size();
  if (!size.ok()) {
    PhysicalFilespaceHeaderResult result;
    result.status = size.status;
    result.diagnostic = size.diagnostic;
    return result;
  }
  std::vector<unsigned char> buffer(kPhysicalHeaderBytes, 0);
  const auto read = device.ReadAt(0, buffer.data(), buffer.size());
  if (!read.ok()) {
    PhysicalFilespaceHeaderResult result;
    result.status = read.status;
    result.diagnostic = read.diagnostic;
    return result;
  }
  return ParseBinaryHeader(buffer, size.size_bytes);
}

PhysicalFilespaceHeaderResult ReadPhysicalFilespaceHeaderOffline(const std::string& path) {
  if (path.empty()) {
    return ReadError("SB-FILESPACE-HEADER-PATH-REQUIRED", "storage.filespace.header.path_required");
  }
  std::error_code ec;
  const auto file_size = std::filesystem::file_size(path, ec);
  if (ec) {
    return ReadError("SB-FILESPACE-HEADER-SIZE-FAILED",
                     "storage.filespace.header.size_failed",
                     path + ":" + ec.message());
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return ReadError("SB-FILESPACE-HEADER-OPEN-FAILED",
                     "storage.filespace.header.open_failed",
                     path);
  }
  std::vector<unsigned char> buffer(kPhysicalHeaderBytes, 0);
  in.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
  if (in.gcount() != static_cast<std::streamsize>(buffer.size())) {
    return ReadError("SB-FILESPACE-HEADER-READ-SHORT",
                     "storage.filespace.header.read_short",
                     std::to_string(in.gcount()));
  }
  return ParseBinaryHeader(buffer, file_size);
}

PhysicalFilespaceHeaderResult ValidatePhysicalFilespaceHeader(const PhysicalFilespaceHeader& expected,
                                                              const PhysicalFilespaceHeader& actual) {
  const auto expected_capacity = ValidateCapacity(expected);
  if (!expected_capacity.ok()) {
    return expected_capacity;
  }
  const auto actual_capacity = ValidateCapacity(actual);
  if (!actual_capacity.ok()) {
    return actual_capacity;
  }
  if (expected.database_uuid.value != actual.database_uuid.value) {
    return ReadError("SB-FILESPACE-HEADER-DATABASE-UUID-MISMATCH", "storage.filespace.header.database_uuid_mismatch");
  }
  if (expected.filespace_uuid.value != actual.filespace_uuid.value) {
    return ReadError("SB-FILESPACE-HEADER-FILESPACE-UUID-MISMATCH", "storage.filespace.header.filespace_uuid_mismatch");
  }
  if (expected.role != actual.role) {
    return ReadError("SB-FILESPACE-HEADER-ROLE-MISMATCH", "storage.filespace.header.role_mismatch");
  }
  if (expected.state != actual.state) {
    return ReadError("SB-FILESPACE-HEADER-STATE-MISMATCH", "storage.filespace.header.state_mismatch");
  }
  if (expected.page_size != actual.page_size) {
    return ReadError("SB-FILESPACE-HEADER-PAGE-SIZE-MISMATCH", "storage.filespace.header.page_size_mismatch");
  }
  if (expected.format_version != actual.format_version) {
    return ReadError("SB-FILESPACE-HEADER-FORMAT-VERSION-MISMATCH", "storage.filespace.header.format_version_mismatch");
  }
  if (expected.checksum_profile != actual.checksum_profile) {
    return ReadError("SB-FILESPACE-HEADER-CHECKSUM-PROFILE-MISMATCH", "storage.filespace.header.checksum_profile_mismatch");
  }
  if (expected.encryption_profile != actual.encryption_profile) {
    return ReadError("SB-FILESPACE-HEADER-ENCRYPTION-PROFILE-MISMATCH", "storage.filespace.header.encryption_profile_mismatch");
  }
  if (expected.compatibility_flags != actual.compatibility_flags) {
    return ReadError("SB-FILESPACE-HEADER-COMPATIBILITY-FLAGS-MISMATCH", "storage.filespace.header.compatibility_flags_mismatch");
  }
  if (expected.attach_policy_flags != actual.attach_policy_flags) {
    return ReadError("SB-FILESPACE-HEADER-ATTACH-POLICY-FLAGS-MISMATCH", "storage.filespace.header.attach_policy_flags_mismatch");
  }
  if (expected.physical_filespace_id != actual.physical_filespace_id) {
    return ReadError("SB-FILESPACE-HEADER-PHYSICAL-ID-MISMATCH", "storage.filespace.header.physical_id_mismatch");
  }
  if (expected.total_pages != actual.total_pages) {
    return ReadError("SB-FILESPACE-HEADER-TOTAL-PAGES-MISMATCH", "storage.filespace.header.total_pages_mismatch");
  }
  if (expected.free_pages != actual.free_pages) {
    return ReadError("SB-FILESPACE-HEADER-FREE-PAGES-MISMATCH", "storage.filespace.header.free_pages_mismatch");
  }
  if (expected.preallocated_pages != actual.preallocated_pages) {
    return ReadError("SB-FILESPACE-HEADER-PREALLOCATED-PAGES-MISMATCH", "storage.filespace.header.preallocated_pages_mismatch");
  }
  if (expected.allocation_root_page != actual.allocation_root_page) {
    return ReadError("SB-FILESPACE-HEADER-ALLOCATION-ROOT-PAGE-MISMATCH", "storage.filespace.header.allocation_root_page_mismatch");
  }
  if (expected.header_generation != actual.header_generation) {
    return ReadError("SB-FILESPACE-HEADER-GENERATION-MISMATCH", "storage.filespace.header.generation_mismatch");
  }
  if (expected.writer_identity_uuid.value != actual.writer_identity_uuid.value) {
    return ReadError("SB-FILESPACE-HEADER-WRITER-UUID-MISMATCH", "storage.filespace.header.writer_uuid_mismatch");
  }
  PhysicalFilespaceHeaderResult result;
  result.status = HeaderOkStatus();
  result.header = actual;
  result.expected_capacity_bytes = actual_capacity.expected_capacity_bytes;
  result.file_size_matches_capacity = true;
  return result;
}

PhysicalFilespaceHeaderResult ValidatePhysicalFilespaceHeader(const PhysicalFilespaceHeader& expected,
                                                              const PhysicalFilespaceHeader& actual,
                                                              u64 file_size_bytes) {
  auto result = ValidatePhysicalFilespaceHeader(expected, actual);
  if (!result.ok()) {
    return result;
  }
  result.file_size_bytes = file_size_bytes;
  result.file_size_matches_capacity = file_size_bytes == result.expected_capacity_bytes;
  if (actual.page_size != 0 && (file_size_bytes % actual.page_size) != 0) {
    return ReadError("SB-FILESPACE-HEADER-FILE-SIZE-UNALIGNED",
                     "storage.filespace.header.file_size_unaligned",
                     std::to_string(file_size_bytes));
  }
  if (!result.file_size_matches_capacity) {
    return ReadError("SB-FILESPACE-HEADER-FILE-SIZE-CAPACITY-MISMATCH",
                     "storage.filespace.header.file_size_capacity_mismatch",
                     std::to_string(file_size_bytes) + ":" + std::to_string(result.expected_capacity_bytes));
  }
  return result;
}

DiagnosticRecord MakePhysicalFilespaceHeaderDiagnostic(Status status,
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
                        "storage.filespace.header");
}

}  // namespace scratchbird::storage::filespace
