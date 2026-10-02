// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_common_page_header.hpp"
#include "filespace_page_zero.hpp"
#include <array>
#include <optional>
#include <span>
#include <vector>

namespace scratchbird::storage::page {
using scratchbird::core::platform::Uuid;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::u64;
struct NativeFilespaceAllocationRoot {
  disk::NativePageReference page;
  Uuid object_uuid;
  std::array<byte,32> sha256{};
  u64 map_generation=0,capacity_generation=0;
  bool operator==(const NativeFilespaceAllocationRoot&) const = default;
};
struct NativeFilespaceDirectoryRecord {
  disk::FilespaceBootstrap bootstrap;
  Uuid locator_uuid;
  Uuid page_zero_uuid;
  u64 page_zero_generation = 0;
  u64 root_set_generation = 0;
  u64 total_pages = 0;
  u64 verification_epoch = 0;
  std::optional<disk::NativePageReference> operation;
  std::optional<NativeFilespaceAllocationRoot> allocation_root{};
};
template<class Records> struct NativeFilespaceDirectoryData {
  disk::NativeCommonPageHeader header;
  Uuid object_uuid;
  u64 directory_generation = 0;
  Uuid creator_transaction_uuid;
  u64 creator_local_transaction_id = 0;
  u64 total_records = 0;
  u64 first_record = 0;
  std::optional<disk::NativePageReference> next;
  std::array<byte,32> next_sha256{};
  Records records;
  Uuid creator_operation_uuid{};
};
using NativeFilespaceDirectory=NativeFilespaceDirectoryData<std::vector<NativeFilespaceDirectoryRecord>>;
using NativeFilespaceDirectoryView=NativeFilespaceDirectoryData<std::span<NativeFilespaceDirectoryRecord>>;
enum class NativeDirectoryError {
  none, invalid_header, invalid_family, invalid_record, invalid_reference,
  invalid_integrity, hash_failure, resource_exhausted, invalid_filespace,
  binding_mismatch, chain_mismatch, io_failure, physical_extent_changed, invalid_backing
};
struct NativeFilespaceDirectoryResult {
  NativeDirectoryError error = NativeDirectoryError::invalid_family;
  std::optional<NativeFilespaceDirectory> directory;
  std::vector<byte> bytes;
  bool ok() const noexcept { return error==NativeDirectoryError::none && directory.has_value(); }
};
NativeFilespaceDirectoryResult EncodeNativeFilespaceDirectory(const NativeFilespaceDirectory&) noexcept;
NativeFilespaceDirectoryResult DecodeNativeFilespaceDirectory(const std::vector<byte>&) noexcept;
struct NativeFilespaceDirectoryViewResult {
  NativeDirectoryError error=NativeDirectoryError::invalid_family;
  std::optional<NativeFilespaceDirectoryView> directory;
  bool ok() const noexcept {return error==NativeDirectoryError::none&&directory.has_value();}
};
// Each decoded record also requires one caller-owned UUID for uniqueness
// validation. Scratch is not retained by the view; records must outlive it.
// Input/records/scratch must be disjoint. On refusal no usable view is returned.
NativeFilespaceDirectoryViewResult DecodeNativeFilespaceDirectoryInto(std::span<const byte>,
    std::span<NativeFilespaceDirectoryRecord>,std::span<Uuid> uniqueness_scratch) noexcept;
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
struct NativeHistoricalFilespaceImage {
  Uuid filespace_uuid;
  std::vector<byte> page_zero;
};
// Verification inputs only. The owning publication reader must authenticate
// the retained images/root digest; this is not a current directory or capacity
// grant. The ceiling includes supplied page-zero images and bootstrap probes.
NativeFilespaceDirectoryChainResult ReadNativeFilespaceDirectoryAtHistoricalRootFromOpenDevices(
    const Uuid& database_uuid, const std::vector<disk::NativeFilespaceDevice>&,
    const disk::FilespaceRootReference& head, const std::array<byte, 32>& root_sha256,
    const std::vector<NativeHistoricalFilespaceImage>&,
    u64 maximum_retained_image_bytes) noexcept;
}  // namespace scratchbird::storage::page
