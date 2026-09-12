// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "filespace_bootstrap.hpp"
#include <vector>

namespace scratchbird::storage::disk {
using scratchbird::core::platform::u64;
// NATIVE_FILESPACE_PAGE_ZERO_V1; never a prototype PageType enum cast.
struct FilespaceRootReference {
  u16 kind = 0;
  u32 page_type = 0;
  Uuid filespace_uuid;
  u64 page_number = 0;
  u64 page_generation = 0;
  Uuid page_size_profile_uuid;
  Uuid object_uuid;
};
struct FilespacePageZero {
  FilespaceBootstrap bootstrap;
  Uuid page_uuid;
  Uuid creation_operation_uuid;
  Uuid writer_identity_uuid;
  u64 page_generation = 0;
  u64 root_set_generation = 0;
  u64 total_pages = 0;
  u64 free_pages = 0;
  u64 preallocated_pages = 0;
  u64 creation_utc_millis = 0;
  std::vector<FilespaceRootReference> roots;
};
enum class FilespacePageZeroError {
  none, invalid_bootstrap, invalid_common_header, invalid_family,
  integrity_mismatch, hash_provider_failure, invalid_capacity,
  invalid_root_directory, required_root_missing, probe_changed,
  device_not_open, io_failure, resource_exhausted
};
struct FilespacePageZeroDecodeResult {
  FilespacePageZeroError error = FilespacePageZeroError::invalid_family;
  std::optional<FilespacePageZero> record;
  bool ok() const noexcept { return error == FilespacePageZeroError::none && record.has_value(); }
};
struct FilespacePageZeroEncodeResult {
  FilespacePageZeroError error = FilespacePageZeroError::invalid_family;
  std::optional<std::vector<byte>> bytes;
  bool ok() const noexcept { return error == FilespacePageZeroError::none && bytes.has_value(); }
};
u32 CanonicalPageZeroRootPageType(u16 root_kind) noexcept;
FilespacePageZeroEncodeResult EncodeFilespacePageZero(const FilespacePageZero&) noexcept;
FilespacePageZeroDecodeResult DecodeFilespacePageZero(
    const byte*, std::size_t, const FilespaceBootstrapBinding* expected = nullptr) noexcept;
// Complete image/metadata validation, not target-root resolution or serving
// admission. The caller must verify actual roots and MGA recovery authority.
FilespacePageZeroDecodeResult ReadFilespacePageZeroFromOpenDevice(
    FileDevice&, const FilespaceBootstrapBinding* expected = nullptr) noexcept;
}  // namespace scratchbird::storage::disk
