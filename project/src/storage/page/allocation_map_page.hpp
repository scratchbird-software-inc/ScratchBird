// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-ALLOCATION-MAP-PAGE-BODY-ANCHOR
// Durable allocation-map page bodies record filespace capacity generations and
// page extents. The format is little-endian and validated fail-closed before a
// page body is admitted by the engine.

#include "page_allocation_lifecycle.hpp"
#include "page_header.hpp"
#include "page_registry.hpp"
#include "runtime_platform.hpp"

#include <array>
#include <string>
#include <vector>

namespace scratchbird::storage::page {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;
using scratchbird::storage::disk::PageType;

inline constexpr std::array<byte, 8> kAllocationMapPageBodyMagic = {
    'S', 'B', 'A', 'L', 'M', '0', '0', '1'};
inline constexpr u32 kAllocationMapPageBodyHeaderBytes = 192;
inline constexpr u32 kAllocationMapExtentRecordBytes = 96;
inline constexpr u16 kAllocationMapPageBodyFormatMajor = 1;
inline constexpr u16 kAllocationMapPageBodyFormatMinor = 0;

enum class AllocationMapPageBodyMutationKind : u16 {
  replace_extent_state = 1
};

struct AllocationMapExtent {
  u64 start_page = 0;
  u64 page_count = 0;
  PageAllocationLifecycleState state = PageAllocationLifecycleState::free;
  PageType page_type = PageType::unknown;
  PageFamily page_family = PageFamily::unknown;
  u32 extent_flags = 0;
  u64 page_generation = 0;
  u64 reusable_after_local_transaction_id = 0;
  TypedUuid allocation_uuid;
  TypedUuid owner_object_uuid;
  TypedUuid creator_transaction_uuid;
};

struct AllocationMapPageBody {
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  TypedUuid file_member_uuid;
  u64 allocation_map_page_number = 0;
  u64 map_generation = 1;
  u64 capacity_generation = 1;
  u32 page_size_bytes = 0;
  u64 filespace_start_page = 0;
  u64 total_pages = 0;
  std::vector<AllocationMapExtent> extents;
};

struct AllocationMapPageBodyCounts {
  u64 free_pages = 0;
  u64 reserved_pages = 0;
  u64 allocated_pages = 0;
  u64 reusable_pending_mga_pages = 0;
  u64 quarantined_pages = 0;
  u64 total_counted_pages = 0;
};

struct AllocationMapPageBodyValidationResult {
  Status status;
  AllocationMapPageBodyCounts counts;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct AllocationMapPageBodyResult {
  Status status;
  AllocationMapPageBody body;
  AllocationMapPageBodyValidationResult validation;
  std::vector<byte> serialized;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct AllocationMapPageBodyMutation {
  AllocationMapPageBodyMutationKind kind =
      AllocationMapPageBodyMutationKind::replace_extent_state;
  AllocationMapExtent extent;
};

const char* AllocationMapPageBodyMutationKindName(
    AllocationMapPageBodyMutationKind kind);
u64 ComputeAllocationMapPageBodyChecksum(
    const std::vector<byte>& extent_bytes);
AllocationMapPageBodyValidationResult ValidateAllocationMapPageBody(
    const AllocationMapPageBody& body);
AllocationMapPageBodyResult BuildAllocationMapPageBody(
    const AllocationMapPageBody& body,
    u32 page_size);
AllocationMapPageBodyResult ParseAllocationMapPageBody(
    const std::vector<byte>& serialized);
AllocationMapPageBodyResult ApplyAllocationMapPageBodyMutation(
    const AllocationMapPageBody& body,
    const AllocationMapPageBodyMutation& mutation,
    u32 page_size);
AllocationMapPageBodyResult RebuildAllocationMapPageBody(
    const AllocationMapPageBody& sparse_body,
    u32 page_size);
DiagnosticRecord MakeAllocationMapPageBodyDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail = {});

}  // namespace scratchbird::storage::page
