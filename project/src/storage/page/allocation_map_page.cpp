// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "allocation_map_page.hpp"

#include "database_format.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
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
using scratchbird::core::platform::UuidKind;
using scratchbird::core::uuid::IsEngineIdentityUuid;
using scratchbird::storage::disk::IsSupportedDatabasePageSize;
using scratchbird::storage::disk::kPageHeaderSerializedBytes;

inline constexpr u32 kOffsetMagic = 0;
inline constexpr u32 kOffsetHeaderBytes = 8;
inline constexpr u32 kOffsetFormatMajor = 12;
inline constexpr u32 kOffsetFormatMinor = 14;
inline constexpr u32 kOffsetPageType = 16;
inline constexpr u32 kOffsetPageSize = 20;
inline constexpr u32 kOffsetDatabaseUuid = 24;
inline constexpr u32 kOffsetFilespaceUuid = 40;
inline constexpr u32 kOffsetFileMemberUuid = 56;
inline constexpr u32 kOffsetAllocationMapPageNumber = 72;
inline constexpr u32 kOffsetMapGeneration = 80;
inline constexpr u32 kOffsetCapacityGeneration = 88;
inline constexpr u32 kOffsetFilespaceStartPage = 96;
inline constexpr u32 kOffsetTotalPages = 104;
inline constexpr u32 kOffsetExtentCount = 112;
inline constexpr u32 kOffsetExtentRecordBytes = 116;
inline constexpr u32 kOffsetExtentsOffset = 120;
inline constexpr u32 kOffsetExtentsBytes = 124;
inline constexpr u32 kOffsetFreePages = 128;
inline constexpr u32 kOffsetReservedPages = 136;
inline constexpr u32 kOffsetAllocatedPages = 144;
inline constexpr u32 kOffsetReusablePendingMgaPages = 152;
inline constexpr u32 kOffsetQuarantinedPages = 160;
inline constexpr u32 kOffsetChecksum = 168;

inline constexpr u32 kExtentOffsetStartPage = 0;
inline constexpr u32 kExtentOffsetPageCount = 8;
inline constexpr u32 kExtentOffsetState = 16;
inline constexpr u32 kExtentOffsetPageType = 20;
inline constexpr u32 kExtentOffsetPageFamily = 24;
inline constexpr u32 kExtentOffsetFlags = 28;
inline constexpr u32 kExtentOffsetPageGeneration = 32;
inline constexpr u32 kExtentOffsetReusableAfterLocalTx = 40;
inline constexpr u32 kExtentOffsetAllocationUuid = 48;
inline constexpr u32 kExtentOffsetOwnerObjectUuid = 64;
inline constexpr u32 kExtentOffsetCreatorTransactionUuid = 80;

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::storage_page};
}

Status ErrorStatus() {
  return {StatusCode::platform_required_feature_missing,
          Severity::error,
          Subsystem::storage_page};
}

bool AddWouldOverflow(u64 left, u64 right) {
  return right > std::numeric_limits<u64>::max() - left;
}

bool MultiplyWouldOverflow(u64 left, u64 right) {
  return left != 0 && right > std::numeric_limits<u64>::max() / left;
}

bool IsTypedEngineIdentity(const TypedUuid& uuid, UuidKind kind) {
  return uuid.kind == kind && uuid.valid() && IsEngineIdentityUuid(uuid.value);
}

bool IsOptionalTypedEngineIdentity(const TypedUuid& uuid, UuidKind kind) {
  return !uuid.valid() || IsTypedEngineIdentity(uuid, kind);
}

bool SameTypedUuid(const TypedUuid& left, const TypedUuid& right) {
  return left.kind == right.kind && left.value == right.value;
}

bool ExtentStateRequiresAllocationIdentity(PageAllocationLifecycleState state) {
  switch (state) {
    case PageAllocationLifecycleState::reserved:
    case PageAllocationLifecycleState::allocated:
    case PageAllocationLifecycleState::reusable_pending_mga:
    case PageAllocationLifecycleState::reusable_free:
    case PageAllocationLifecycleState::compacting:
    case PageAllocationLifecycleState::preallocated:
      return true;
    case PageAllocationLifecycleState::free:
    case PageAllocationLifecycleState::quarantined:
      return false;
  }
  return true;
}

bool IsKnownAllocationState(PageAllocationLifecycleState state) {
  switch (state) {
    case PageAllocationLifecycleState::free:
    case PageAllocationLifecycleState::reserved:
    case PageAllocationLifecycleState::allocated:
    case PageAllocationLifecycleState::reusable_pending_mga:
    case PageAllocationLifecycleState::reusable_free:
    case PageAllocationLifecycleState::compacting:
    case PageAllocationLifecycleState::quarantined:
    case PageAllocationLifecycleState::preallocated:
      return true;
  }
  return false;
}

AllocationMapPageBodyValidationResult ValidationError(
    std::string diagnostic_code,
    std::string message_key,
    std::string detail = {}) {
  AllocationMapPageBodyValidationResult result;
  result.status = ErrorStatus();
  result.diagnostic = MakeAllocationMapPageBodyDiagnostic(
      result.status,
      std::move(diagnostic_code),
      std::move(message_key),
      std::move(detail));
  return result;
}

AllocationMapPageBodyResult BodyError(std::string diagnostic_code,
                                      std::string message_key,
                                      std::string detail = {}) {
  AllocationMapPageBodyResult result;
  result.status = ErrorStatus();
  result.diagnostic = MakeAllocationMapPageBodyDiagnostic(
      result.status,
      std::move(diagnostic_code),
      std::move(message_key),
      std::move(detail));
  return result;
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

void StoreUuidAt(std::vector<byte>* out, u32 offset, const TypedUuid& uuid) {
  std::copy(uuid.value.bytes.begin(),
            uuid.value.bytes.end(),
            out->begin() + static_cast<std::ptrdiff_t>(offset));
}

TypedUuid LoadUuidAt(const std::vector<byte>& in, u32 offset, UuidKind kind) {
  TypedUuid uuid;
  uuid.kind = kind;
  std::copy(in.begin() + static_cast<std::ptrdiff_t>(offset),
            in.begin() + static_cast<std::ptrdiff_t>(offset + uuid.value.bytes.size()),
            uuid.value.bytes.begin());
  return uuid;
}

AllocationMapExtent NormalizeFreeExtent(AllocationMapExtent extent) {
  if (extent.state == PageAllocationLifecycleState::free) {
    extent.page_type = PageType::unknown;
    extent.page_family = PageFamily::unknown;
    extent.extent_flags = 0;
    extent.page_generation = 0;
    extent.reusable_after_local_transaction_id = 0;
    extent.allocation_uuid = TypedUuid{};
    extent.owner_object_uuid = TypedUuid{};
    extent.creator_transaction_uuid = TypedUuid{};
  }
  return extent;
}

bool SameMergeIdentity(const AllocationMapExtent& left,
                       const AllocationMapExtent& right) {
  return left.state == right.state &&
         left.page_type == right.page_type &&
         left.page_family == right.page_family &&
         left.extent_flags == right.extent_flags &&
         left.page_generation == right.page_generation &&
         left.reusable_after_local_transaction_id ==
             right.reusable_after_local_transaction_id &&
         SameTypedUuid(left.allocation_uuid, right.allocation_uuid) &&
         SameTypedUuid(left.owner_object_uuid, right.owner_object_uuid) &&
         SameTypedUuid(left.creator_transaction_uuid,
                       right.creator_transaction_uuid);
}

std::vector<AllocationMapExtent> MergeAdjacentExtents(
    std::vector<AllocationMapExtent> extents) {
  for (auto& extent : extents) {
    extent = NormalizeFreeExtent(extent);
  }
  std::sort(extents.begin(),
            extents.end(),
            [](const AllocationMapExtent& left,
               const AllocationMapExtent& right) {
              return left.start_page < right.start_page;
            });

  std::vector<AllocationMapExtent> merged;
  for (const auto& extent : extents) {
    if (merged.empty()) {
      merged.push_back(extent);
      continue;
    }
    auto& last = merged.back();
    if (!AddWouldOverflow(last.start_page, last.page_count) &&
        last.start_page + last.page_count == extent.start_page &&
        SameMergeIdentity(last, extent)) {
      last.page_count += extent.page_count;
      continue;
    }
    merged.push_back(extent);
  }
  return merged;
}

void AccumulateCounts(PageAllocationLifecycleState state,
                      u64 page_count,
                      AllocationMapPageBodyCounts* counts) {
  counts->total_counted_pages += page_count;
  switch (state) {
    case PageAllocationLifecycleState::free:
    case PageAllocationLifecycleState::reusable_free:
      counts->free_pages += page_count;
      break;
    case PageAllocationLifecycleState::reserved:
    case PageAllocationLifecycleState::preallocated:
    case PageAllocationLifecycleState::compacting:
      counts->reserved_pages += page_count;
      break;
    case PageAllocationLifecycleState::allocated:
      counts->allocated_pages += page_count;
      break;
    case PageAllocationLifecycleState::reusable_pending_mga:
      counts->reusable_pending_mga_pages += page_count;
      break;
    case PageAllocationLifecycleState::quarantined:
      counts->quarantined_pages += page_count;
      break;
  }
}

AllocationMapPageBodyValidationResult ValidateExtentMetadata(
    const AllocationMapExtent& extent) {
  if (!IsKnownAllocationState(extent.state)) {
    return ValidationError("SB-ALLOCATION-MAP-PAGE-STATE-UNKNOWN",
                           "storage.allocation_map_page.state_unknown",
                           std::to_string(static_cast<u32>(extent.state)));
  }
  if (ExtentStateRequiresAllocationIdentity(extent.state) &&
      !IsTypedEngineIdentity(extent.allocation_uuid, UuidKind::object)) {
    return ValidationError("SB-ALLOCATION-MAP-PAGE-ALLOCATION-UUID-INVALID",
                           "storage.allocation_map_page.allocation_uuid_invalid");
  }
  if (!IsOptionalTypedEngineIdentity(extent.owner_object_uuid, UuidKind::object)) {
    return ValidationError("SB-ALLOCATION-MAP-PAGE-OWNER-UUID-INVALID",
                           "storage.allocation_map_page.owner_uuid_invalid");
  }
  if (!IsOptionalTypedEngineIdentity(extent.creator_transaction_uuid,
                                     UuidKind::transaction)) {
    return ValidationError("SB-ALLOCATION-MAP-PAGE-CREATOR-TX-UUID-INVALID",
                           "storage.allocation_map_page.creator_tx_uuid_invalid");
  }
  if (extent.page_type != PageType::unknown) {
    const auto family = LookupPageFamily(extent.page_type);
    if (!family.ok() || family.descriptor.family != extent.page_family) {
      return ValidationError("SB-ALLOCATION-MAP-PAGE-FAMILY-MISMATCH",
                             "storage.allocation_map_page.family_mismatch");
    }
  } else if (extent.state != PageAllocationLifecycleState::free &&
             extent.state != PageAllocationLifecycleState::quarantined &&
             extent.page_family == PageFamily::unknown) {
    return ValidationError("SB-ALLOCATION-MAP-PAGE-FAMILY-REQUIRED",
                           "storage.allocation_map_page.family_required");
  }
  if (extent.state == PageAllocationLifecycleState::allocated &&
      extent.page_generation == 0) {
    return ValidationError("SB-ALLOCATION-MAP-PAGE-GENERATION-REQUIRED",
                           "storage.allocation_map_page.page_generation_required");
  }
  return {OkStatus(), {}, {}};
}

std::vector<byte> SerializeExtentRecords(
    const std::vector<AllocationMapExtent>& extents) {
  std::vector<byte> out(extents.size() * kAllocationMapExtentRecordBytes, 0);
  for (std::size_t index = 0; index < extents.size(); ++index) {
    const u32 base = static_cast<u32>(index * kAllocationMapExtentRecordBytes);
    const AllocationMapExtent extent = NormalizeFreeExtent(extents[index]);
    StoreLittle64At(&out, base + kExtentOffsetStartPage, extent.start_page);
    StoreLittle64At(&out, base + kExtentOffsetPageCount, extent.page_count);
    StoreLittle32At(&out,
                    base + kExtentOffsetState,
                    static_cast<u32>(extent.state));
    StoreLittle32At(&out,
                    base + kExtentOffsetPageType,
                    static_cast<u32>(extent.page_type));
    StoreLittle32At(&out,
                    base + kExtentOffsetPageFamily,
                    static_cast<u32>(extent.page_family));
    StoreLittle32At(&out, base + kExtentOffsetFlags, extent.extent_flags);
    StoreLittle64At(&out,
                    base + kExtentOffsetPageGeneration,
                    extent.page_generation);
    StoreLittle64At(&out,
                    base + kExtentOffsetReusableAfterLocalTx,
                    extent.reusable_after_local_transaction_id);
    StoreUuidAt(&out,
                base + kExtentOffsetAllocationUuid,
                extent.allocation_uuid);
    StoreUuidAt(&out,
                base + kExtentOffsetOwnerObjectUuid,
                extent.owner_object_uuid);
    StoreUuidAt(&out,
                base + kExtentOffsetCreatorTransactionUuid,
                extent.creator_transaction_uuid);
  }
  return out;
}

AllocationMapExtent LoadExtentRecord(const std::vector<byte>& in, u32 base) {
  AllocationMapExtent extent;
  extent.start_page = LoadLittle64At(in, base + kExtentOffsetStartPage);
  extent.page_count = LoadLittle64At(in, base + kExtentOffsetPageCount);
  extent.state = static_cast<PageAllocationLifecycleState>(
      LoadLittle32At(in, base + kExtentOffsetState));
  extent.page_type =
      static_cast<PageType>(LoadLittle32At(in, base + kExtentOffsetPageType));
  extent.page_family =
      static_cast<PageFamily>(LoadLittle32At(in, base + kExtentOffsetPageFamily));
  extent.extent_flags = LoadLittle32At(in, base + kExtentOffsetFlags);
  extent.page_generation =
      LoadLittle64At(in, base + kExtentOffsetPageGeneration);
  extent.reusable_after_local_transaction_id =
      LoadLittle64At(in, base + kExtentOffsetReusableAfterLocalTx);
  extent.allocation_uuid =
      LoadUuidAt(in, base + kExtentOffsetAllocationUuid, UuidKind::object);
  extent.owner_object_uuid =
      LoadUuidAt(in, base + kExtentOffsetOwnerObjectUuid, UuidKind::object);
  extent.creator_transaction_uuid = LoadUuidAt(
      in, base + kExtentOffsetCreatorTransactionUuid, UuidKind::transaction);
  return NormalizeFreeExtent(extent);
}

}  // namespace

const char* AllocationMapPageBodyMutationKindName(
    AllocationMapPageBodyMutationKind kind) {
  switch (kind) {
    case AllocationMapPageBodyMutationKind::replace_extent_state:
      return "replace_extent_state";
  }
  return "unknown";
}

u64 ComputeAllocationMapPageBodyChecksum(
    const std::vector<byte>& extent_bytes) {
  u64 hash = 14695981039346656037ull ^ 0x414c4c4f434d4150ull;
  for (byte value : extent_bytes) {
    hash ^= static_cast<u64>(value);
    hash *= 1099511628211ull;
  }
  return hash;
}

AllocationMapPageBodyValidationResult ValidateAllocationMapPageBody(
    const AllocationMapPageBody& body) {
  if (!IsTypedEngineIdentity(body.database_uuid, UuidKind::database)) {
    return ValidationError("SB-ALLOCATION-MAP-PAGE-DATABASE-UUID-INVALID",
                           "storage.allocation_map_page.database_uuid_invalid");
  }
  if (!IsTypedEngineIdentity(body.filespace_uuid, UuidKind::filespace)) {
    return ValidationError("SB-ALLOCATION-MAP-PAGE-FILESPACE-UUID-INVALID",
                           "storage.allocation_map_page.filespace_uuid_invalid");
  }
  if (!IsOptionalTypedEngineIdentity(body.file_member_uuid, UuidKind::object)) {
    return ValidationError("SB-ALLOCATION-MAP-PAGE-FILE-MEMBER-UUID-INVALID",
                           "storage.allocation_map_page.file_member_uuid_invalid");
  }
  if (body.map_generation == 0 || body.capacity_generation == 0) {
    return ValidationError("SB-ALLOCATION-MAP-PAGE-GENERATION-INVALID",
                           "storage.allocation_map_page.generation_invalid");
  }
  if (!IsSupportedDatabasePageSize(body.page_size_bytes)) {
    return ValidationError("SB-ALLOCATION-MAP-PAGE-SIZE-INVALID",
                           "storage.allocation_map_page.page_size_invalid",
                           std::to_string(body.page_size_bytes));
  }
  if (body.total_pages == 0 || body.extents.empty() ||
      AddWouldOverflow(body.filespace_start_page, body.total_pages)) {
    return ValidationError("SB-ALLOCATION-MAP-PAGE-CAPACITY-INVALID",
                           "storage.allocation_map_page.capacity_invalid");
  }
  if (body.extents.size() >
      std::numeric_limits<u32>::max() / kAllocationMapExtentRecordBytes) {
    return ValidationError("SB-ALLOCATION-MAP-PAGE-EXTENT-COUNT-OVERFLOW",
                           "storage.allocation_map_page.extent_count_overflow");
  }

  AllocationMapPageBodyCounts counts;
  u64 expected_start = body.filespace_start_page;
  const u64 filespace_end = body.filespace_start_page + body.total_pages;
  for (const auto& raw_extent : body.extents) {
    const AllocationMapExtent extent = NormalizeFreeExtent(raw_extent);
    if (extent.page_count == 0 ||
        AddWouldOverflow(extent.start_page, extent.page_count)) {
      return ValidationError("SB-ALLOCATION-MAP-PAGE-EXTENT-RANGE-INVALID",
                             "storage.allocation_map_page.extent_range_invalid");
    }
    if (extent.start_page != expected_start) {
      return ValidationError("SB-ALLOCATION-MAP-PAGE-EXTENT-COVERAGE-GAP",
                             "storage.allocation_map_page.extent_coverage_gap",
                             std::to_string(extent.start_page));
    }
    if (extent.start_page + extent.page_count > filespace_end) {
      return ValidationError("SB-ALLOCATION-MAP-PAGE-EXTENT-OUT-OF-RANGE",
                             "storage.allocation_map_page.extent_out_of_range");
    }
    const auto metadata = ValidateExtentMetadata(extent);
    if (!metadata.ok()) {
      return metadata;
    }
    AccumulateCounts(extent.state, extent.page_count, &counts);
    expected_start = extent.start_page + extent.page_count;
  }
  if (expected_start != filespace_end ||
      counts.total_counted_pages != body.total_pages) {
    return ValidationError("SB-ALLOCATION-MAP-PAGE-CAPACITY-COUNT-MISMATCH",
                           "storage.allocation_map_page.capacity_count_mismatch");
  }

  AllocationMapPageBodyValidationResult result;
  result.status = OkStatus();
  result.counts = counts;
  return result;
}

AllocationMapPageBodyResult BuildAllocationMapPageBody(
    const AllocationMapPageBody& body,
    u32 page_size) {
  if (page_size <= kPageHeaderSerializedBytes + kAllocationMapPageBodyHeaderBytes) {
    return BodyError("SB-ALLOCATION-MAP-PAGE-PAGE-SIZE-TOO-SMALL",
                     "storage.allocation_map_page.page_size_too_small",
                     std::to_string(page_size));
  }
  AllocationMapPageBody normalized = body;
  normalized.page_size_bytes = page_size;
  normalized.extents = MergeAdjacentExtents(body.extents);
  const auto validation = ValidateAllocationMapPageBody(normalized);
  if (!validation.ok()) {
    AllocationMapPageBodyResult result;
    result.status = validation.status;
    result.validation = validation;
    result.diagnostic = validation.diagnostic;
    return result;
  }

  const std::vector<byte> extent_bytes = SerializeExtentRecords(normalized.extents);
  const u32 body_capacity = page_size - kPageHeaderSerializedBytes;
  if (extent_bytes.size() > body_capacity - kAllocationMapPageBodyHeaderBytes) {
    return BodyError("SB-ALLOCATION-MAP-PAGE-TOO-LARGE",
                     "storage.allocation_map_page.too_large",
                     std::to_string(extent_bytes.size()));
  }

  AllocationMapPageBodyResult result;
  result.status = OkStatus();
  result.body = normalized;
  result.validation = validation;
  result.serialized.assign(kAllocationMapPageBodyHeaderBytes, 0);
  std::copy(kAllocationMapPageBodyMagic.begin(),
            kAllocationMapPageBodyMagic.end(),
            result.serialized.begin() + kOffsetMagic);
  StoreLittle32At(&result.serialized,
                  kOffsetHeaderBytes,
                  kAllocationMapPageBodyHeaderBytes);
  StoreLittle16At(&result.serialized,
                  kOffsetFormatMajor,
                  kAllocationMapPageBodyFormatMajor);
  StoreLittle16At(&result.serialized,
                  kOffsetFormatMinor,
                  kAllocationMapPageBodyFormatMinor);
  StoreLittle32At(&result.serialized,
                  kOffsetPageType,
                  static_cast<u32>(PageType::allocation_map));
  StoreLittle32At(&result.serialized, kOffsetPageSize, page_size);
  StoreUuidAt(&result.serialized, kOffsetDatabaseUuid, normalized.database_uuid);
  StoreUuidAt(&result.serialized, kOffsetFilespaceUuid, normalized.filespace_uuid);
  StoreUuidAt(&result.serialized,
              kOffsetFileMemberUuid,
              normalized.file_member_uuid);
  StoreLittle64At(&result.serialized,
                  kOffsetAllocationMapPageNumber,
                  normalized.allocation_map_page_number);
  StoreLittle64At(&result.serialized,
                  kOffsetMapGeneration,
                  normalized.map_generation);
  StoreLittle64At(&result.serialized,
                  kOffsetCapacityGeneration,
                  normalized.capacity_generation);
  StoreLittle64At(&result.serialized,
                  kOffsetFilespaceStartPage,
                  normalized.filespace_start_page);
  StoreLittle64At(&result.serialized, kOffsetTotalPages, normalized.total_pages);
  StoreLittle32At(&result.serialized,
                  kOffsetExtentCount,
                  static_cast<u32>(normalized.extents.size()));
  StoreLittle32At(&result.serialized,
                  kOffsetExtentRecordBytes,
                  kAllocationMapExtentRecordBytes);
  StoreLittle32At(&result.serialized,
                  kOffsetExtentsOffset,
                  kAllocationMapPageBodyHeaderBytes);
  StoreLittle32At(&result.serialized,
                  kOffsetExtentsBytes,
                  static_cast<u32>(extent_bytes.size()));
  StoreLittle64At(&result.serialized,
                  kOffsetFreePages,
                  validation.counts.free_pages);
  StoreLittle64At(&result.serialized,
                  kOffsetReservedPages,
                  validation.counts.reserved_pages);
  StoreLittle64At(&result.serialized,
                  kOffsetAllocatedPages,
                  validation.counts.allocated_pages);
  StoreLittle64At(&result.serialized,
                  kOffsetReusablePendingMgaPages,
                  validation.counts.reusable_pending_mga_pages);
  StoreLittle64At(&result.serialized,
                  kOffsetQuarantinedPages,
                  validation.counts.quarantined_pages);
  StoreLittle64At(&result.serialized,
                  kOffsetChecksum,
                  ComputeAllocationMapPageBodyChecksum(extent_bytes));
  result.serialized.insert(result.serialized.end(),
                           extent_bytes.begin(),
                           extent_bytes.end());
  result.serialized.resize(body_capacity, 0);
  return result;
}

AllocationMapPageBodyResult ParseAllocationMapPageBody(
    const std::vector<byte>& serialized) {
  if (serialized.size() < kAllocationMapPageBodyHeaderBytes) {
    return BodyError("SB-ALLOCATION-MAP-PAGE-TRUNCATED",
                     "storage.allocation_map_page.truncated");
  }
  if (!std::equal(kAllocationMapPageBodyMagic.begin(),
                  kAllocationMapPageBodyMagic.end(),
                  serialized.begin() + kOffsetMagic)) {
    return BodyError("SB-ALLOCATION-MAP-PAGE-MAGIC-INVALID",
                     "storage.allocation_map_page.magic_invalid");
  }
  if (LoadLittle32At(serialized, kOffsetHeaderBytes) !=
      kAllocationMapPageBodyHeaderBytes) {
    return BodyError("SB-ALLOCATION-MAP-PAGE-HEADER-BYTES-INVALID",
                     "storage.allocation_map_page.header_bytes_invalid");
  }
  if (LoadLittle16At(serialized, kOffsetFormatMajor) !=
          kAllocationMapPageBodyFormatMajor ||
      LoadLittle16At(serialized, kOffsetFormatMinor) >
          kAllocationMapPageBodyFormatMinor) {
    return BodyError("SB-ALLOCATION-MAP-PAGE-FORMAT-UNSUPPORTED",
                     "storage.allocation_map_page.format_unsupported");
  }
  if (static_cast<PageType>(LoadLittle32At(serialized, kOffsetPageType)) !=
      PageType::allocation_map) {
    return BodyError("SB-ALLOCATION-MAP-PAGE-TYPE-MISMATCH",
                     "storage.allocation_map_page.page_type_mismatch");
  }
  const u32 extent_record_bytes =
      LoadLittle32At(serialized, kOffsetExtentRecordBytes);
  const u32 extent_count = LoadLittle32At(serialized, kOffsetExtentCount);
  const u32 extents_offset = LoadLittle32At(serialized, kOffsetExtentsOffset);
  const u32 extents_bytes = LoadLittle32At(serialized, kOffsetExtentsBytes);
  if (extent_record_bytes != kAllocationMapExtentRecordBytes ||
      extents_offset != kAllocationMapPageBodyHeaderBytes ||
      extents_offset > serialized.size() ||
      extents_bytes > serialized.size() - extents_offset ||
      MultiplyWouldOverflow(extent_count, extent_record_bytes) ||
      static_cast<u64>(extent_count) * extent_record_bytes != extents_bytes) {
    return BodyError("SB-ALLOCATION-MAP-PAGE-EXTENT-BOUNDS-INVALID",
                     "storage.allocation_map_page.extent_bounds_invalid");
  }

  const std::vector<byte> extent_bytes(
      serialized.begin() + static_cast<std::ptrdiff_t>(extents_offset),
      serialized.begin() + static_cast<std::ptrdiff_t>(extents_offset + extents_bytes));
  const u64 expected_checksum = LoadLittle64At(serialized, kOffsetChecksum);
  if (ComputeAllocationMapPageBodyChecksum(extent_bytes) != expected_checksum) {
    return BodyError("SB-ALLOCATION-MAP-PAGE-CHECKSUM-MISMATCH",
                     "storage.allocation_map_page.checksum_mismatch");
  }

  AllocationMapPageBody body;
  body.database_uuid =
      LoadUuidAt(serialized, kOffsetDatabaseUuid, UuidKind::database);
  body.filespace_uuid =
      LoadUuidAt(serialized, kOffsetFilespaceUuid, UuidKind::filespace);
  body.file_member_uuid =
      LoadUuidAt(serialized, kOffsetFileMemberUuid, UuidKind::object);
  body.allocation_map_page_number =
      LoadLittle64At(serialized, kOffsetAllocationMapPageNumber);
  body.map_generation = LoadLittle64At(serialized, kOffsetMapGeneration);
  body.capacity_generation =
      LoadLittle64At(serialized, kOffsetCapacityGeneration);
  body.page_size_bytes = LoadLittle32At(serialized, kOffsetPageSize);
  body.filespace_start_page =
      LoadLittle64At(serialized, kOffsetFilespaceStartPage);
  body.total_pages = LoadLittle64At(serialized, kOffsetTotalPages);
  for (u32 index = 0; index < extent_count; ++index) {
    body.extents.push_back(LoadExtentRecord(extent_bytes,
                                            index * kAllocationMapExtentRecordBytes));
  }

  const auto validation = ValidateAllocationMapPageBody(body);
  if (!validation.ok()) {
    AllocationMapPageBodyResult result;
    result.status = validation.status;
    result.validation = validation;
    result.diagnostic = validation.diagnostic;
    result.body = body;
    return result;
  }
  if (validation.counts.free_pages !=
          LoadLittle64At(serialized, kOffsetFreePages) ||
      validation.counts.reserved_pages !=
          LoadLittle64At(serialized, kOffsetReservedPages) ||
      validation.counts.allocated_pages !=
          LoadLittle64At(serialized, kOffsetAllocatedPages) ||
      validation.counts.reusable_pending_mga_pages !=
          LoadLittle64At(serialized, kOffsetReusablePendingMgaPages) ||
      validation.counts.quarantined_pages !=
          LoadLittle64At(serialized, kOffsetQuarantinedPages)) {
    return BodyError("SB-ALLOCATION-MAP-PAGE-COUNT-MISMATCH",
                     "storage.allocation_map_page.count_mismatch");
  }

  AllocationMapPageBodyResult result;
  result.status = OkStatus();
  result.body = std::move(body);
  result.validation = validation;
  result.serialized = serialized;
  return result;
}

AllocationMapPageBodyResult ApplyAllocationMapPageBodyMutation(
    const AllocationMapPageBody& body,
    const AllocationMapPageBodyMutation& mutation,
    u32 page_size) {
  if (mutation.kind !=
      AllocationMapPageBodyMutationKind::replace_extent_state) {
    return BodyError("SB-ALLOCATION-MAP-PAGE-MUTATION-UNKNOWN",
                     "storage.allocation_map_page.mutation_unknown",
                     AllocationMapPageBodyMutationKindName(mutation.kind));
  }
  const auto validation = ValidateAllocationMapPageBody(body);
  if (!validation.ok()) {
    AllocationMapPageBodyResult result;
    result.status = validation.status;
    result.validation = validation;
    result.diagnostic = validation.diagnostic;
    return result;
  }
  AllocationMapExtent replacement = NormalizeFreeExtent(mutation.extent);
  if (replacement.page_count == 0 ||
      AddWouldOverflow(replacement.start_page, replacement.page_count) ||
      replacement.start_page < body.filespace_start_page ||
      replacement.start_page + replacement.page_count >
          body.filespace_start_page + body.total_pages) {
    return BodyError("SB-ALLOCATION-MAP-PAGE-MUTATION-RANGE-INVALID",
                     "storage.allocation_map_page.mutation_range_invalid");
  }
  const auto extent_metadata = ValidateExtentMetadata(replacement);
  if (!extent_metadata.ok()) {
    AllocationMapPageBodyResult result;
    result.status = extent_metadata.status;
    result.validation = extent_metadata;
    result.diagnostic = extent_metadata.diagnostic;
    return result;
  }

  const u64 mutation_start = replacement.start_page;
  const u64 mutation_end = replacement.start_page + replacement.page_count;
  bool replacement_inserted = false;
  std::vector<AllocationMapExtent> mutated_extents;
  for (const auto& extent : body.extents) {
    const u64 extent_start = extent.start_page;
    const u64 extent_end = extent.start_page + extent.page_count;
    if (extent_end <= mutation_start || extent_start >= mutation_end) {
      mutated_extents.push_back(extent);
      continue;
    }
    if (extent_start < mutation_start) {
      AllocationMapExtent prefix = extent;
      prefix.page_count = mutation_start - extent_start;
      mutated_extents.push_back(prefix);
    }
    if (!replacement_inserted) {
      mutated_extents.push_back(replacement);
      replacement_inserted = true;
    }
    if (extent_end > mutation_end) {
      AllocationMapExtent suffix = extent;
      suffix.start_page = mutation_end;
      suffix.page_count = extent_end - mutation_end;
      mutated_extents.push_back(suffix);
    }
  }
  if (!replacement_inserted) {
    return BodyError("SB-ALLOCATION-MAP-PAGE-MUTATION-RANGE-NOT-COVERED",
                     "storage.allocation_map_page.mutation_range_not_covered");
  }

  AllocationMapPageBody mutated = body;
  mutated.map_generation = body.map_generation + 1;
  mutated.extents = MergeAdjacentExtents(std::move(mutated_extents));
  return BuildAllocationMapPageBody(mutated, page_size);
}

AllocationMapPageBodyResult RebuildAllocationMapPageBody(
    const AllocationMapPageBody& sparse_body,
    u32 page_size) {
  if (!IsTypedEngineIdentity(sparse_body.database_uuid, UuidKind::database) ||
      !IsTypedEngineIdentity(sparse_body.filespace_uuid, UuidKind::filespace) ||
      sparse_body.map_generation == 0 ||
      sparse_body.capacity_generation == 0 ||
      sparse_body.total_pages == 0 ||
      AddWouldOverflow(sparse_body.filespace_start_page,
                       sparse_body.total_pages)) {
    return BodyError("SB-ALLOCATION-MAP-PAGE-REBUILD-METADATA-INVALID",
                     "storage.allocation_map_page.rebuild_metadata_invalid");
  }

  std::vector<AllocationMapExtent> facts = sparse_body.extents;
  for (auto& fact : facts) {
    fact = NormalizeFreeExtent(fact);
    if (fact.page_count == 0 ||
        AddWouldOverflow(fact.start_page, fact.page_count) ||
        fact.start_page < sparse_body.filespace_start_page ||
        fact.start_page + fact.page_count >
            sparse_body.filespace_start_page + sparse_body.total_pages) {
      return BodyError("SB-ALLOCATION-MAP-PAGE-REBUILD-RANGE-INVALID",
                       "storage.allocation_map_page.rebuild_range_invalid");
    }
    const auto metadata = ValidateExtentMetadata(fact);
    if (!metadata.ok()) {
      AllocationMapPageBodyResult result;
      result.status = metadata.status;
      result.validation = metadata;
      result.diagnostic = metadata.diagnostic;
      return result;
    }
  }
  std::sort(facts.begin(),
            facts.end(),
            [](const AllocationMapExtent& left,
               const AllocationMapExtent& right) {
              return left.start_page < right.start_page;
            });

  std::vector<AllocationMapExtent> complete;
  u64 next_page = sparse_body.filespace_start_page;
  const u64 end_page = sparse_body.filespace_start_page + sparse_body.total_pages;
  for (const auto& fact : facts) {
    if (fact.start_page < next_page) {
      return BodyError("SB-ALLOCATION-MAP-PAGE-REBUILD-OVERLAP",
                       "storage.allocation_map_page.rebuild_overlap");
    }
    if (fact.start_page > next_page) {
      complete.push_back({next_page,
                          fact.start_page - next_page,
                          PageAllocationLifecycleState::free});
    }
    complete.push_back(fact);
    next_page = fact.start_page + fact.page_count;
  }
  if (next_page < end_page) {
    complete.push_back({next_page,
                        end_page - next_page,
                        PageAllocationLifecycleState::free});
  }

  AllocationMapPageBody rebuilt = sparse_body;
  rebuilt.page_size_bytes = page_size;
  rebuilt.extents = MergeAdjacentExtents(std::move(complete));
  return BuildAllocationMapPageBody(rebuilt, page_size);
}

DiagnosticRecord MakeAllocationMapPageBodyDiagnostic(
    Status status,
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
                        "storage.page.allocation_map_body");
}

}  // namespace scratchbird::storage::page
