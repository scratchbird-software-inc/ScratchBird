// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "row_data_page.hpp"

#include "database_format.hpp"
#include "page_header.hpp"

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

namespace scratchbird::storage::page {
namespace {

using scratchbird::core::datatypes::DecodeDatatypeBinaryValue;
using scratchbird::core::datatypes::EncodeDatatypeBinaryValue;
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
using scratchbird::core::platform::UuidKind;
using scratchbird::core::uuid::IsEngineIdentityUuid;
using scratchbird::storage::disk::kPageHeaderSerializedBytes;

inline constexpr byte kRowDataMagic[8] = {'S', 'B', 'R', 'O', 'W', '0', '0', '2'};
inline constexpr byte kRowDataV1Magic[8] = {'S', 'B', 'R', 'O', 'W', '0', '0', '1'};
inline constexpr u32 kOffsetMagic = 0;
inline constexpr u32 kOffsetHeaderBytes = 8;
inline constexpr u32 kOffsetRowCount = 12;
inline constexpr u32 kOffsetBodyBytes = 16;
inline constexpr u32 kOffsetNextPageNumber = 24;
inline constexpr u32 kOffsetBodyChecksum = 32;
inline constexpr u32 kOffsetRelationUuid = 40;
inline constexpr u32 kOffsetPageGeneration = 56;
inline constexpr u32 kOffsetSegmentId = 64;
inline constexpr u32 kOffsetSegmentGeneration = 72;
inline constexpr u32 kOffsetCompactionGeneration = 80;
inline constexpr u32 kOffsetSlotDirectoryOffset = 88;
inline constexpr u32 kOffsetFreeSpaceBytes = 92;

inline constexpr u32 kRowHeaderV1Bytes = 72;
inline constexpr u32 kRowHeaderBytes = 88;
inline constexpr u32 kRowOffsetInternalOrdinal = 48;
inline constexpr u32 kRowOffsetRowBytes = 52;
inline constexpr u32 kRowOffsetRowChecksum = 56;
inline constexpr u32 kRowOffsetStableSlotId = 64;
inline constexpr u32 kRowOffsetPreviousRowVersion = 72;
inline constexpr u32 kRowOffsetNextRowVersion = 80;
inline constexpr u32 kCellHeaderBytes = 16;
inline constexpr u32 kSlotEntryBytes = 24;
inline constexpr u32 kSlotOffsetStableSlotId = 0;
inline constexpr u32 kSlotOffsetRowOffset = 4;
inline constexpr u32 kSlotOffsetRowBytes = 8;
inline constexpr u32 kSlotOffsetFlags = 12;
inline constexpr u32 kSlotOffsetRowChecksum = 16;

namespace RowFlag {
inline constexpr u16 deleted = 1u << 0;
}  // namespace RowFlag

Status RowPageOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::storage_page};
}

Status RowPageErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::storage_page};
}

RowDataPageResult RowPageError(std::string diagnostic_code,
                               std::string message_key,
                               std::string detail = {}) {
  RowDataPageResult result;
  result.status = RowPageErrorStatus();
  result.diagnostic = MakeRowDataPageDiagnostic(result.status,
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

bool IsTypedEngineIdentity(const TypedUuid& uuid, UuidKind kind) {
  return uuid.kind == kind && uuid.valid() && IsEngineIdentityUuid(uuid.value);
}

bool SameTypedUuid(const TypedUuid& left, const TypedUuid& right) {
  return left.kind == right.kind && left.value == right.value;
}

DenseRowOrdinalValidation RowOrdinalRefusal(const RowDataPageBody& body,
                                            const DenseRowOrdinalLocator& locator,
                                            std::string reason) {
  DenseRowOrdinalValidation result;
  result.locator = locator;
  result.refusal_reason = std::move(reason);
  result.evidence = {
      "dense_row_ordinal.accepted=false",
      "dense_row_ordinal.fail_closed_to_uuid_mga_lookup=true",
      "dense_row_ordinal.finality_authority=false",
      "durable_mga_inventory_remains_authority=true",
      "relation_segment_page_scope=" + std::to_string(body.segment_id) + ":" +
          std::to_string(body.segment_generation) + ":" +
          std::to_string(body.page_number) + ":" +
          std::to_string(body.page_generation),
      "refusal_reason=" + result.refusal_reason,
  };
  return result;
}

}  // namespace

u64 ComputeRowDataPageChecksum(const std::vector<byte>& body) {
  std::vector<byte> normalized = body;
  if (normalized.size() >= kOffsetBodyChecksum + sizeof(u64)) {
    StoreLittle64(normalized.data() + kOffsetBodyChecksum, 0);
  }
  return Fnv1a64(normalized.data(), normalized.size());
}

void AssignDenseInternalRowOrdinals(RowDataPageBody* body) {
  if (body == nullptr) {
    return;
  }
  for (std::size_t index = 0; index < body->rows.size(); ++index) {
    body->rows[index].internal_row_ordinal = static_cast<u32>(index + 1);
    if (body->rows[index].stable_slot_id == 0) {
      body->rows[index].stable_slot_id = body->rows[index].internal_row_ordinal;
    }
  }
}

DenseRowOrdinalScope MakeDenseRowOrdinalScope(const RowDataPageBody& body) {
  DenseRowOrdinalScope scope;
  scope.relation_uuid = body.relation_uuid;
  scope.segment_id = body.segment_id;
  scope.segment_generation = body.segment_generation;
  scope.page_number = body.page_number;
  scope.page_generation = body.page_generation;
  return scope;
}

DenseRowOrdinalLocator MakeDenseRowOrdinalLocator(
    const DenseRowOrdinalScope& scope,
    const RowDataRecord& row,
    bool durable_mga_inventory_authority_available,
    bool normal_mga_visibility_authority_available) {
  DenseRowOrdinalLocator locator;
  locator.scope = scope;
  locator.internal_row_ordinal = row.internal_row_ordinal;
  locator.row_uuid = row.row_uuid;
  locator.transaction_uuid = row.transaction_uuid;
  locator.local_transaction_id = row.local_transaction_id;
  locator.durable_mga_inventory_authority_available =
      durable_mga_inventory_authority_available;
  locator.normal_mga_visibility_authority_available =
      normal_mga_visibility_authority_available;
  return locator;
}

DenseRowOrdinalValidation ValidateDenseRowOrdinalLocator(
    const RowDataPageBody& body,
    const DenseRowOrdinalLocator& locator) {
  if (!IsTypedEngineIdentity(body.relation_uuid, UuidKind::object) ||
      !IsTypedEngineIdentity(locator.scope.relation_uuid, UuidKind::object)) {
    return RowOrdinalRefusal(body, locator, "relation_uuid_required");
  }
  if (!SameTypedUuid(body.relation_uuid, locator.scope.relation_uuid)) {
    return RowOrdinalRefusal(body, locator, "relation_mismatch");
  }
  if (body.segment_id == 0 || locator.scope.segment_id != body.segment_id) {
    return RowOrdinalRefusal(body, locator, "segment_mismatch");
  }
  if (body.segment_generation == 0 ||
      locator.scope.segment_generation != body.segment_generation) {
    return RowOrdinalRefusal(body, locator, "segment_generation_mismatch");
  }
  if (body.page_number == 0 || locator.scope.page_number != body.page_number) {
    return RowOrdinalRefusal(body, locator, "page_mismatch");
  }
  if (body.page_generation == 0 ||
      locator.scope.page_generation != body.page_generation) {
    return RowOrdinalRefusal(body, locator, "page_generation_mismatch");
  }
  if (!locator.durable_mga_inventory_authority_available ||
      !locator.normal_mga_visibility_authority_available) {
    return RowOrdinalRefusal(body, locator, "mga_authority_missing");
  }
  if (!IsTypedEngineIdentity(locator.row_uuid, UuidKind::row) ||
      !IsTypedEngineIdentity(locator.transaction_uuid, UuidKind::transaction)) {
    return RowOrdinalRefusal(body, locator, "uuid_evidence_required");
  }
  if (locator.internal_row_ordinal == 0 ||
      locator.internal_row_ordinal > body.rows.size()) {
    return RowOrdinalRefusal(body, locator, "ordinal_out_of_range");
  }

  const RowDataRecord& row =
      body.rows[static_cast<std::size_t>(locator.internal_row_ordinal - 1)];
  if (row.internal_row_ordinal != locator.internal_row_ordinal) {
    return RowOrdinalRefusal(body, locator, "ordinal_slot_mismatch");
  }
  if (!SameTypedUuid(row.row_uuid, locator.row_uuid)) {
    return RowOrdinalRefusal(body, locator, "row_uuid_mismatch");
  }
  if (!SameTypedUuid(row.transaction_uuid, locator.transaction_uuid) ||
      row.local_transaction_id != locator.local_transaction_id) {
    return RowOrdinalRefusal(body, locator, "transaction_evidence_mismatch");
  }

  DenseRowOrdinalValidation result;
  result.accepted = true;
  result.fail_closed_to_uuid_mga_lookup = false;
  result.locator = locator;
  result.row = row;
  result.evidence = {
      "dense_row_ordinal.accepted=true",
      "dense_row_ordinal.scope=relation_segment_page_generation",
      "dense_row_ordinal.uuid_identity_preserved=true",
      "dense_row_ordinal.finality_authority=false",
      "durable_mga_inventory_remains_authority=true",
      "normal_mga_visibility_authority_available=true",
  };
  return result;
}

RowDataPageResult BuildRowDataPageBody(const RowDataPageBody& body, u32 page_size) {
  if (page_size <= kPageHeaderSerializedBytes + kRowDataPageBodyHeaderBytes) {
    return RowPageError("SB-ROW-DATA-PAGE-SIZE-TOO-SMALL",
                        "storage.row_data_page.page_size_too_small",
                        std::to_string(page_size));
  }

  RowDataPageBody body_with_ordinals = body;
  AssignDenseInternalRowOrdinals(&body_with_ordinals);
  if (!IsTypedEngineIdentity(body_with_ordinals.relation_uuid, UuidKind::object)) {
    return RowPageError("SB-ROW-DATA-PAGE-RELATION-UUID-REQUIRED",
                        "storage.row_data_page.relation_uuid_required");
  }
  if (body_with_ordinals.segment_id == 0 ||
      body_with_ordinals.segment_generation == 0 ||
      body_with_ordinals.page_generation == 0) {
    return RowPageError("SB-ROW-DATA-PAGE-ORDINAL-SCOPE-REQUIRED",
                        "storage.row_data_page.ordinal_scope_required");
  }
  if (body_with_ordinals.compaction_generation == 0) {
    body_with_ordinals.compaction_generation = body_with_ordinals.page_generation;
  }

  std::vector<std::vector<byte>> encoded_cells;
  u32 body_bytes = kRowDataPageBodyHeaderBytes;
  for (const RowDataRecord& row : body_with_ordinals.rows) {
    if (!IsTypedEngineIdentity(row.row_uuid, UuidKind::row) ||
        !IsTypedEngineIdentity(row.transaction_uuid, UuidKind::transaction)) {
      return RowPageError("SB-ROW-DATA-PAGE-UUID-MUST-BE-V7",
                          "storage.row_data_page.uuid_must_be_v7");
    }
    if (row.previous_row_version != 0 && row.previous_row_version >= row.row_version) {
      return RowPageError("SB-ROW-DATA-PAGE-LINEAGE-INVALID",
                          "storage.row_data_page.previous_lineage_invalid",
                          std::to_string(row.row_version));
    }
    if (row.next_row_version != 0 && row.next_row_version <= row.row_version) {
      return RowPageError("SB-ROW-DATA-PAGE-LINEAGE-INVALID",
                          "storage.row_data_page.next_lineage_invalid",
                          std::to_string(row.row_version));
    }
    body_bytes += kRowHeaderBytes;
    for (const RowDataCell& cell : row.cells) {
      const auto encoded = EncodeDatatypeBinaryValue(cell.value);
      if (!encoded.ok()) {
        RowDataPageResult result;
        result.status = encoded.status;
        result.diagnostic = encoded.diagnostic;
        return result;
      }
      body_bytes += kCellHeaderBytes + static_cast<u32>(encoded.encoded.size());
      encoded_cells.push_back(encoded.encoded);
    }
  }
  const u32 slot_directory_offset = body_bytes;
  body_bytes += static_cast<u32>(body_with_ordinals.rows.size()) * kSlotEntryBytes;
  if (body_bytes > page_size - kPageHeaderSerializedBytes) {
    return RowPageError("SB-ROW-DATA-PAGE-BODY-TOO-LARGE",
                        "storage.row_data_page.body_too_large",
                        std::to_string(body_bytes));
  }

  RowDataPageResult result;
  result.status = RowPageOkStatus();
  result.body = body_with_ordinals;
  result.serialized.assign(page_size - kPageHeaderSerializedBytes, 0);
  std::memcpy(result.serialized.data() + kOffsetMagic, kRowDataMagic, sizeof(kRowDataMagic));
  StoreLittle32(result.serialized.data() + kOffsetHeaderBytes, kRowDataPageBodyHeaderBytes);
  StoreLittle32(result.serialized.data() + kOffsetRowCount, static_cast<u32>(body_with_ordinals.rows.size()));
  StoreLittle64(result.serialized.data() + kOffsetNextPageNumber, body_with_ordinals.next_page_number);
  std::copy(body_with_ordinals.relation_uuid.value.bytes.begin(),
            body_with_ordinals.relation_uuid.value.bytes.end(),
            result.serialized.begin() + kOffsetRelationUuid);
  StoreLittle64(result.serialized.data() + kOffsetPageGeneration,
                body_with_ordinals.page_generation);
  StoreLittle64(result.serialized.data() + kOffsetSegmentId,
                body_with_ordinals.segment_id);
  StoreLittle64(result.serialized.data() + kOffsetSegmentGeneration,
                body_with_ordinals.segment_generation);
  StoreLittle64(result.serialized.data() + kOffsetCompactionGeneration,
                body_with_ordinals.compaction_generation);
  StoreLittle32(result.serialized.data() + kOffsetSlotDirectoryOffset,
                slot_directory_offset);
  StoreLittle32(result.serialized.data() + kOffsetFreeSpaceBytes,
                static_cast<u32>(result.serialized.size() - body_bytes));

  u32 offset = kRowDataPageBodyHeaderBytes;
  std::size_t cell_index = 0;
  std::vector<RowDataSlot> slots;
  slots.reserve(body_with_ordinals.rows.size());
  for (const RowDataRecord& row : body_with_ordinals.rows) {
    const u32 row_start = offset;
    std::copy(row.row_uuid.value.bytes.begin(), row.row_uuid.value.bytes.end(), result.serialized.begin() + offset);
    std::copy(row.transaction_uuid.value.bytes.begin(), row.transaction_uuid.value.bytes.end(), result.serialized.begin() + offset + 16);
    StoreLittle64(result.serialized.data() + offset + 32, row.local_transaction_id);
    StoreLittle32(result.serialized.data() + offset + 40, row.row_version);
    StoreLittle16(result.serialized.data() + offset + 44, row.deleted ? RowFlag::deleted : 0);
    StoreLittle16(result.serialized.data() + offset + 46, static_cast<u16>(row.cells.size()));
    StoreLittle32(result.serialized.data() + offset + kRowOffsetInternalOrdinal,
                  row.internal_row_ordinal);
    StoreLittle64(result.serialized.data() + offset + kRowOffsetRowChecksum, 0);
    StoreLittle32(result.serialized.data() + offset + kRowOffsetStableSlotId,
                  row.stable_slot_id);
    StoreLittle64(result.serialized.data() + offset + kRowOffsetPreviousRowVersion,
                  row.previous_row_version);
    StoreLittle64(result.serialized.data() + offset + kRowOffsetNextRowVersion,
                  row.next_row_version);
    offset += kRowHeaderBytes;
    for (const RowDataCell& cell : row.cells) {
      const std::vector<byte>& encoded = encoded_cells[cell_index++];
      StoreLittle16(result.serialized.data() + offset, cell.column_ordinal);
      StoreLittle16(result.serialized.data() + offset + 2, 0);
      StoreLittle32(result.serialized.data() + offset + 4, static_cast<u32>(encoded.size()));
      StoreLittle64(result.serialized.data() + offset + 8, Fnv1a64(encoded.data(), encoded.size()));
      offset += kCellHeaderBytes;
      std::copy(encoded.begin(), encoded.end(), result.serialized.begin() + offset);
      offset += static_cast<u32>(encoded.size());
    }
    const u32 row_bytes = offset - row_start;
    StoreLittle32(result.serialized.data() + row_start + kRowOffsetRowBytes,
                  row_bytes);
    const u64 row_checksum = Fnv1a64(result.serialized.data() + row_start, row_bytes);
    StoreLittle64(result.serialized.data() + row_start + kRowOffsetRowChecksum,
                  row_checksum);
    RowDataSlot slot;
    slot.stable_slot_id = row.stable_slot_id;
    slot.row_offset = row_start;
    slot.row_bytes = row_bytes;
    slot.row_checksum = row_checksum;
    slot.deleted = row.deleted;
    slots.push_back(slot);
  }
  for (const RowDataSlot& slot : slots) {
    StoreLittle32(result.serialized.data() + offset + kSlotOffsetStableSlotId,
                  slot.stable_slot_id);
    StoreLittle32(result.serialized.data() + offset + kSlotOffsetRowOffset,
                  slot.row_offset);
    StoreLittle32(result.serialized.data() + offset + kSlotOffsetRowBytes,
                  slot.row_bytes);
    StoreLittle32(result.serialized.data() + offset + kSlotOffsetFlags,
                  slot.deleted ? RowFlag::deleted : 0);
    StoreLittle64(result.serialized.data() + offset + kSlotOffsetRowChecksum,
                  slot.row_checksum);
    offset += kSlotEntryBytes;
  }
  body_with_ordinals.slots = slots;
  body_with_ordinals.free_space_offset = offset;
  body_with_ordinals.free_space_bytes =
      static_cast<u32>(result.serialized.size() - offset);
  result.body = body_with_ordinals;
  StoreLittle32(result.serialized.data() + kOffsetBodyBytes, offset);
  StoreLittle64(result.serialized.data() + kOffsetBodyChecksum, ComputeRowDataPageChecksum(result.serialized));
  return result;
}

RowDataPageResult ParseRowDataPageBody(const std::vector<byte>& serialized, u64 page_number) {
  if (serialized.size() < kRowDataPageBodyHeaderBytes) {
    return RowPageError("SB-ROW-DATA-PAGE-BODY-SHORT",
                        "storage.row_data_page.body_short",
                        std::to_string(page_number));
  }
  const bool row_data_v2 =
      std::memcmp(serialized.data() + kOffsetMagic, kRowDataMagic, sizeof(kRowDataMagic)) == 0;
  const bool row_data_v1 =
      std::memcmp(serialized.data() + kOffsetMagic, kRowDataV1Magic, sizeof(kRowDataV1Magic)) == 0;
  if (!row_data_v2 && !row_data_v1) {
    return RowPageError("SB-ROW-DATA-PAGE-MAGIC-INVALID",
                        "storage.row_data_page.magic_invalid");
  }
  const u32 row_header_bytes = row_data_v2 ? kRowHeaderBytes : kRowHeaderV1Bytes;
  const u32 header_bytes = LoadLittle32(serialized.data() + kOffsetHeaderBytes);
  const u32 row_count = LoadLittle32(serialized.data() + kOffsetRowCount);
  const u32 body_bytes = LoadLittle32(serialized.data() + kOffsetBodyBytes);
  const u32 slot_directory_offset =
      LoadLittle32(serialized.data() + kOffsetSlotDirectoryOffset);
  const u32 free_space_bytes = LoadLittle32(serialized.data() + kOffsetFreeSpaceBytes);
  if (header_bytes != kRowDataPageBodyHeaderBytes || body_bytes > serialized.size()) {
    return RowPageError("SB-ROW-DATA-PAGE-BODY-SIZE-INVALID",
                        "storage.row_data_page.body_size_invalid");
  }
  const u64 slot_bytes = static_cast<u64>(row_count) * kSlotEntryBytes;
  if (slot_directory_offset < kRowDataPageBodyHeaderBytes ||
      static_cast<u64>(slot_directory_offset) + slot_bytes != body_bytes ||
      free_space_bytes != serialized.size() - body_bytes) {
    return RowPageError("SB-ROW-DATA-PAGE-SLOT-DIRECTORY-INVALID",
                        "storage.row_data_page.slot_directory_invalid");
  }
  if (LoadLittle64(serialized.data() + kOffsetBodyChecksum) != ComputeRowDataPageChecksum(serialized)) {
    return RowPageError("SB-ROW-DATA-PAGE-CHECKSUM-MISMATCH",
                        "storage.row_data_page.checksum_mismatch");
  }

  RowDataPageResult result;
  result.status = RowPageOkStatus();
  result.body.page_number = page_number;
  result.body.relation_uuid.kind = UuidKind::object;
  std::copy(serialized.begin() + kOffsetRelationUuid,
            serialized.begin() + kOffsetRelationUuid + 16,
            result.body.relation_uuid.value.bytes.begin());
  result.body.page_generation = LoadLittle64(serialized.data() + kOffsetPageGeneration);
  result.body.segment_id = LoadLittle64(serialized.data() + kOffsetSegmentId);
  result.body.segment_generation =
      LoadLittle64(serialized.data() + kOffsetSegmentGeneration);
  result.body.compaction_generation =
      LoadLittle64(serialized.data() + kOffsetCompactionGeneration);
  result.body.next_page_number = LoadLittle64(serialized.data() + kOffsetNextPageNumber);
  result.body.free_space_offset = body_bytes;
  result.body.free_space_bytes = free_space_bytes;
  result.serialized = serialized;

  u32 offset = kRowDataPageBodyHeaderBytes;
  std::vector<RowDataSlot> expected_slots;
  expected_slots.reserve(row_count);
  for (u32 row_index = 0; row_index < row_count; ++row_index) {
    if (offset + row_header_bytes > body_bytes) {
      return RowPageError("SB-ROW-DATA-PAGE-ROW-SHORT",
                          "storage.row_data_page.row_short",
                          std::to_string(row_index));
    }
    const u32 row_start = offset;
    RowDataRecord row;
    row.row_uuid.kind = UuidKind::row;
    std::copy(serialized.begin() + offset, serialized.begin() + offset + 16, row.row_uuid.value.bytes.begin());
    row.transaction_uuid.kind = UuidKind::transaction;
    std::copy(serialized.begin() + offset + 16, serialized.begin() + offset + 32, row.transaction_uuid.value.bytes.begin());
    row.local_transaction_id = LoadLittle64(serialized.data() + offset + 32);
    row.row_version = LoadLittle32(serialized.data() + offset + 40);
    row.deleted = (LoadLittle16(serialized.data() + offset + 44) & RowFlag::deleted) != 0;
    const u16 cell_count = LoadLittle16(serialized.data() + offset + 46);
    row.internal_row_ordinal = LoadLittle32(serialized.data() + offset + kRowOffsetInternalOrdinal);
    const u32 row_bytes = LoadLittle32(serialized.data() + offset + kRowOffsetRowBytes);
    const u64 row_checksum = LoadLittle64(serialized.data() + offset + kRowOffsetRowChecksum);
    row.stable_slot_id = LoadLittle32(serialized.data() + offset + kRowOffsetStableSlotId);
    if (row_data_v2) {
      row.previous_row_version =
          LoadLittle64(serialized.data() + offset + kRowOffsetPreviousRowVersion);
      row.next_row_version =
          LoadLittle64(serialized.data() + offset + kRowOffsetNextRowVersion);
    }
    if (row.previous_row_version != 0 && row.previous_row_version >= row.row_version) {
      return RowPageError("SB-ROW-DATA-PAGE-LINEAGE-INVALID",
                          "storage.row_data_page.previous_lineage_invalid",
                          std::to_string(row_index));
    }
    if (row.next_row_version != 0 && row.next_row_version <= row.row_version) {
      return RowPageError("SB-ROW-DATA-PAGE-LINEAGE-INVALID",
                          "storage.row_data_page.next_lineage_invalid",
                          std::to_string(row_index));
    }
    offset += row_header_bytes;

    for (u16 cell_index = 0; cell_index < cell_count; ++cell_index) {
      if (offset + kCellHeaderBytes > body_bytes) {
        return RowPageError("SB-ROW-DATA-PAGE-CELL-SHORT",
                            "storage.row_data_page.cell_short",
                            std::to_string(cell_index));
      }
      RowDataCell cell;
      cell.column_ordinal = LoadLittle16(serialized.data() + offset);
      const u32 payload_bytes = LoadLittle32(serialized.data() + offset + 4);
      const u64 payload_checksum = LoadLittle64(serialized.data() + offset + 8);
      offset += kCellHeaderBytes;
      if (offset + payload_bytes > body_bytes) {
        return RowPageError("SB-ROW-DATA-PAGE-CELL-PAYLOAD-SHORT",
                            "storage.row_data_page.cell_payload_short",
                            std::to_string(cell_index));
      }
      std::vector<byte> encoded(serialized.begin() + offset, serialized.begin() + offset + payload_bytes);
      if (payload_checksum != Fnv1a64(encoded.data(), encoded.size())) {
        return RowPageError("SB-ROW-DATA-PAGE-CELL-CHECKSUM-MISMATCH",
                            "storage.row_data_page.cell_checksum_mismatch",
                            std::to_string(cell_index));
      }
      const auto decoded = DecodeDatatypeBinaryValue(encoded);
      if (!decoded.ok()) {
        RowDataPageResult decoded_result;
        decoded_result.status = decoded.status;
        decoded_result.diagnostic = decoded.diagnostic;
        return decoded_result;
      }
      cell.value = decoded.value;
      row.cells.push_back(std::move(cell));
      offset += payload_bytes;
    }
    if (row_bytes != offset - row_start) {
      return RowPageError("SB-ROW-DATA-PAGE-ROW-BYTES-MISMATCH",
                          "storage.row_data_page.row_bytes_mismatch",
                          std::to_string(row_index));
    }
    std::vector<byte> row_image(serialized.begin() + row_start,
                                serialized.begin() + offset);
    StoreLittle64(row_image.data() + kRowOffsetRowChecksum, 0);
    if (row_checksum != Fnv1a64(row_image.data(), row_image.size())) {
      return RowPageError("SB-ROW-DATA-PAGE-ROW-CHECKSUM-MISMATCH",
                          "storage.row_data_page.row_checksum_mismatch",
                          std::to_string(row_index));
    }
    RowDataSlot slot;
    slot.stable_slot_id = row.stable_slot_id;
    slot.row_offset = row_start;
    slot.row_bytes = row_bytes;
    slot.row_checksum = row_checksum;
    slot.deleted = row.deleted;
    expected_slots.push_back(slot);
    result.body.rows.push_back(std::move(row));
  }
  if (offset != slot_directory_offset) {
    return RowPageError("SB-ROW-DATA-PAGE-SLOT-DIRECTORY-OFFSET-MISMATCH",
                        "storage.row_data_page.slot_directory_offset_mismatch",
                        std::to_string(offset));
  }
  for (u32 slot_index = 0; slot_index < row_count; ++slot_index) {
    const RowDataSlot& expected = expected_slots[slot_index];
    RowDataSlot slot;
    slot.stable_slot_id = LoadLittle32(serialized.data() + offset + kSlotOffsetStableSlotId);
    slot.row_offset = LoadLittle32(serialized.data() + offset + kSlotOffsetRowOffset);
    slot.row_bytes = LoadLittle32(serialized.data() + offset + kSlotOffsetRowBytes);
    slot.deleted = (LoadLittle32(serialized.data() + offset + kSlotOffsetFlags) & RowFlag::deleted) != 0;
    slot.row_checksum = LoadLittle64(serialized.data() + offset + kSlotOffsetRowChecksum);
    if (slot.stable_slot_id != expected.stable_slot_id ||
        slot.row_offset != expected.row_offset ||
        slot.row_bytes != expected.row_bytes ||
        slot.deleted != expected.deleted ||
        slot.row_checksum != expected.row_checksum) {
      return RowPageError("SB-ROW-DATA-PAGE-SLOT-DIRECTORY-MISMATCH",
                          "storage.row_data_page.slot_directory_mismatch",
                          std::to_string(slot_index));
    }
    result.body.slots.push_back(slot);
    offset += kSlotEntryBytes;
  }
  return result;
}

DiagnosticRecord MakeRowDataPageDiagnostic(Status status,
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
                        "storage.page.row_data");
}

}  // namespace scratchbird::storage::page
