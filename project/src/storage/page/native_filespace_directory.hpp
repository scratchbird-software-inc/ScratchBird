// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_common_page_header.hpp"
#include "filespace_page_zero.hpp"
#include <array>
#include <optional>
#include <vector>

namespace scratchbird::storage::page {
using scratchbird::core::platform::Uuid;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::u64;
struct NativeFilespaceDirectoryRecord {
  disk::FilespaceBootstrap bootstrap;
  Uuid locator_uuid;
  Uuid page_zero_uuid;
  u64 page_zero_generation = 0;
  u64 root_set_generation = 0;
  u64 total_pages = 0;
  u64 verification_epoch = 0;
  std::optional<disk::NativePageReference> operation;
};
struct NativeFilespaceDirectory {
  disk::NativeCommonPageHeader header;
  Uuid object_uuid;
  u64 directory_generation = 0;
  Uuid creator_transaction_uuid;
  u64 creator_local_transaction_id = 0;
  u64 total_records = 0;
  u64 first_record = 0;
  std::optional<disk::NativePageReference> next;
  std::array<byte,32> next_sha256{};
  std::vector<NativeFilespaceDirectoryRecord> records;
};
enum class NativeDirectoryError {
  none, invalid_header, invalid_family, invalid_record, invalid_reference,
  invalid_integrity, hash_failure, resource_exhausted, invalid_filespace,
  binding_mismatch, chain_mismatch, io_failure
};
struct NativeFilespaceDirectoryResult {
  NativeDirectoryError error = NativeDirectoryError::invalid_family;
  std::optional<NativeFilespaceDirectory> directory;
  std::vector<byte> bytes;
  bool ok() const noexcept { return error==NativeDirectoryError::none && directory.has_value(); }
};
NativeFilespaceDirectoryResult EncodeNativeFilespaceDirectory(const NativeFilespaceDirectory&) noexcept;
NativeFilespaceDirectoryResult DecodeNativeFilespaceDirectory(const std::vector<byte>&) noexcept;
struct NativeFilespaceDirectoryChainResult {
  NativeDirectoryError error = NativeDirectoryError::invalid_family;
  std::vector<NativeFilespaceDirectoryResult> pages;
  u64 retained_image_bytes = 0;
  bool ok() const noexcept { return error==NativeDirectoryError::none && !pages.empty(); }
};
// Actual supplied owned devices only. Not an attachment, locator lookup,
// checkpoint/MGA grant or completed native publication.
NativeFilespaceDirectoryChainResult ReadNativeFilespaceDirectoryFromOpenDevices(
    const Uuid& database_uuid, const std::vector<disk::NativeFilespaceDevice>&,
    const disk::FilespaceRootReference& head, u64 maximum_retained_image_bytes) noexcept;
}  // namespace scratchbird::storage::page
