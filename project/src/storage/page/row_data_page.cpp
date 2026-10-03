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
#include <array>
#include <type_traits>
#include <cstring>
#include <limits>
#include <map>
#include <set>
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

inline constexpr byte kRowDataMagic[8] = {'S', 'B', 'R', 'O', 'W', '0', '0', '4'};
inline constexpr byte kRowDataMagicPrefix[5] = {'S', 'B', 'R', 'O', 'W'};
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

inline constexpr u32 kRowHeaderBytes = 144;
inline constexpr u32 kRowOffsetStorageGeneration = 136;
inline constexpr u32 kRowOffsetInternalOrdinal = 52;
inline constexpr u32 kRowOffsetRowBytes = 56;
inline constexpr u32 kRowOffsetRowChecksum = 64;
inline constexpr u32 kRowOffsetStableSlotId = 60;
inline constexpr u32 kRowOffsetPreviousRowVersion = 88;
inline constexpr u32 kRowOffsetNextRowVersion = 96;
inline constexpr u32 kRowOffsetVersionUuid = 72;
inline constexpr u32 kRowOffsetPreviousVersionUuid = 104;
inline constexpr u32 kRowOffsetNextVersionUuid = 120;
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

u64 Fnv1a64WithZeroChecksum(const byte* data, std::size_t size,
                           std::size_t checksum_offset) noexcept {
  const bool complete_field = checksum_offset <= size && size - checksum_offset >= sizeof(u64);
  u64 hash = 1469598103934665603ull;
  for (std::size_t i = 0; i < size; ++i) {
    const byte value = complete_field && i >= checksum_offset &&
        i - checksum_offset < sizeof(u64) ? byte{0} : data[i];
    hash ^= static_cast<u64>(value);
    hash *= 1099511628211ull;
  }
  return hash;
}

bool IsTypedEngineIdentity(const TypedUuid& uuid, UuidKind kind) {
  return uuid.kind == kind && uuid.valid() && IsEngineIdentityUuid(uuid.value);
}

template<class Row>
bool ValidRowIdentity(const Row& row) {
  const auto valid_link = [&](const auto& id, u64 sequence) {
    return sequence == 0 ? id.is_nil()
                        : IsEngineIdentityUuid(id) && id != row.version_uuid &&
                          id != row.row_uuid.value;
  };
  return IsTypedEngineIdentity(row.row_uuid, UuidKind::row) &&
         IsTypedEngineIdentity(row.transaction_uuid, UuidKind::transaction) &&
         IsEngineIdentityUuid(row.version_uuid) &&
         row.version_uuid != row.row_uuid.value &&
         row.local_transaction_id != 0 && row.row_version != 0 && row.storage_generation != 0 &&
         valid_link(row.previous_version_uuid, row.previous_row_version) &&
         valid_link(row.next_version_uuid, row.next_row_version) &&
         (row.previous_row_version == 0 || row.previous_row_version < row.row_version) &&
         (row.next_row_version == 0 || row.next_row_version > row.row_version);
}

bool SameTypedUuid(const TypedUuid& left, const TypedUuid& right) {
  return left.kind == right.kind && left.value == right.value;
}

bool PreviousLinksMatchPage(const RowDataPageBody& body) {
  using Uuid = scratchbird::core::platform::Uuid;
  std::map<Uuid, const RowDataRecord*> by_version;
  std::map<std::pair<Uuid, u64>, Uuid> by_sequence;
  for (const auto& row : body.rows) {
    by_version.emplace(row.version_uuid, &row);
    by_sequence.emplace(std::make_pair(row.row_uuid.value, row.row_version), row.version_uuid);
  }
  for (const auto& row : body.rows) {
    if (row.previous_row_version == 0) continue;
    const auto version = by_version.find(row.previous_version_uuid);
    if (version != by_version.end() &&
        (version->second->row_uuid.value != row.row_uuid.value ||
         version->second->row_version != row.previous_row_version)) return false;
    const auto sequence = by_sequence.find({row.row_uuid.value, row.previous_row_version});
    if (sequence != by_sequence.end() && sequence->second != row.previous_version_uuid) return false;
  }
  // Next-version links remain repairable hints and are not traversal authority.
  return true;
}

bool HasRowDataMagicPrefix(std::span<const byte> serialized) {
  return serialized.size() >= sizeof(kRowDataMagicPrefix) &&
         std::memcmp(serialized.data() + kOffsetMagic,
                     kRowDataMagicPrefix,
                     sizeof(kRowDataMagicPrefix)) == 0;
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
  return Fnv1a64WithZeroChecksum(body.data(), body.size(), kOffsetBodyChecksum);
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
  locator.version_uuid = row.version_uuid;
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
  if (!ValidRowIdentity(row) || row.storage_generation > body.page_generation ||
      !IsEngineIdentityUuid(locator.version_uuid) ||
      row.version_uuid != locator.version_uuid) {
    return RowOrdinalRefusal(body, locator, "version_uuid_mismatch");
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

RowDataPageResult BuildRowDataPageBodyOwned(RowDataPageBody body, u32 page_size) {
  if (page_size <= kPageHeaderSerializedBytes + kRowDataPageBodyHeaderBytes) {
    return RowPageError("SB-ROW-DATA-PAGE-SIZE-TOO-SMALL",
                        "storage.row_data_page.page_size_too_small",
                        std::to_string(page_size));
  }

  RowDataPageBody body_with_ordinals = std::move(body);
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
  u64 body_bytes = kRowDataPageBodyHeaderBytes;
  std::set<scratchbird::core::platform::Uuid> version_ids;
  std::set<std::pair<scratchbird::core::platform::Uuid, u64>> row_sequences;
  for (const RowDataRecord& row : body_with_ordinals.rows) {
    if (row.storage_generation == 0 || row.storage_generation > body_with_ordinals.page_generation)
      return RowPageError("CATALOG.INVALID_INPUT", "storage.row_data_page.storage_generation_invalid");
    if (!ValidRowIdentity(row) ||
        !version_ids.insert(row.version_uuid).second ||
        !row_sequences.emplace(row.row_uuid.value, row.row_version).second) {
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
    if (row.cells.size() > std::numeric_limits<u16>::max()) {
      return RowPageError("CATALOG.INVALID_INPUT", "storage.row_data_page.cell_count_invalid");
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
      if (encoded.encoded.size() > page_size || body_bytes > page_size ||
          kCellHeaderBytes + encoded.encoded.size() > page_size - body_bytes) {
        return RowPageError("SB-ROW-DATA-PAGE-BODY-TOO-LARGE",
                            "storage.row_data_page.body_too_large");
      }
      body_bytes += kCellHeaderBytes + encoded.encoded.size();
      encoded_cells.push_back(encoded.encoded);
    }
  }
  if (body_bytes > page_size) {
    return RowPageError("SB-ROW-DATA-PAGE-BODY-TOO-LARGE", "storage.row_data_page.body_too_large");
  }
  const u32 slot_directory_offset = static_cast<u32>(body_bytes);
  if (!PreviousLinksMatchPage(body_with_ordinals)) {
    return RowPageError("CATALOG.INVALID_INPUT", "storage.row_data_page.previous_identity_mismatch");
  }
  body_bytes += static_cast<u64>(body_with_ordinals.rows.size()) * kSlotEntryBytes;
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
    StoreLittle64(result.serialized.data() + offset + 40, row.row_version);
    StoreLittle16(result.serialized.data() + offset + 48, row.deleted ? RowFlag::deleted : 0);
    StoreLittle16(result.serialized.data() + offset + 50, static_cast<u16>(row.cells.size()));
    StoreLittle32(result.serialized.data() + offset + kRowOffsetInternalOrdinal,
                  row.internal_row_ordinal);
    StoreLittle64(result.serialized.data() + offset + kRowOffsetRowChecksum, 0);
    StoreLittle32(result.serialized.data() + offset + kRowOffsetStableSlotId,
                  row.stable_slot_id);
    StoreLittle64(result.serialized.data() + offset + kRowOffsetPreviousRowVersion,
                  row.previous_row_version);
    StoreLittle64(result.serialized.data() + offset + kRowOffsetNextRowVersion,
                  row.next_row_version);
    std::copy(row.version_uuid.bytes.begin(), row.version_uuid.bytes.end(),
              result.serialized.begin() + offset + kRowOffsetVersionUuid);
    std::copy(row.previous_version_uuid.bytes.begin(), row.previous_version_uuid.bytes.end(),
              result.serialized.begin() + offset + kRowOffsetPreviousVersionUuid);
    std::copy(row.next_version_uuid.bytes.begin(), row.next_version_uuid.bytes.end(),
              result.serialized.begin() + offset + kRowOffsetNextVersionUuid);
    StoreLittle64(result.serialized.data() + offset + kRowOffsetStorageGeneration,
                  row.storage_generation);
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

RowDataPageResult BuildRowDataPageBody(const RowDataPageBody& body, u32 page_size) {
  return BuildRowDataPageBodyOwned(body, page_size);
}

namespace {

template<bool Borrowed>
using ReadResult = std::conditional_t<Borrowed, RowDataPageViewResult, RowDataPageResult>;

template<bool Borrowed>
ReadResult<Borrowed> RowReadError(const char* code, const char* key,
                                 std::optional<u64> detail = {}) {
  if constexpr (Borrowed) {
    RowDataPageViewResult out; out.status=RowPageErrorStatus();
    out.diagnostic={code,key,detail,"storage.page.row_data"}; return out;
  } else {
    return RowPageError(code,key,detail?std::to_string(*detail):std::string{});
  }
}
RowDataPageViewResult RowWorkspaceError(bool invalid=false) noexcept {
  RowDataPageViewResult out; out.error=invalid?RowDataPageViewError::invalid_workspace:RowDataPageViewError::insufficient_workspace;
  out.status={invalid?StatusCode::memory_invalid_request:StatusCode::memory_limit_exceeded,Severity::error,Subsystem::storage_page};
  return out;
}

template<bool Borrowed> struct RowReadState;
template<> struct RowReadState<false> {
  using Row=RowDataRecord;
  using Cell=RowDataCell;
  std::set<scratchbird::core::platform::Uuid> versions;
  std::set<std::pair<scratchbird::core::platform::Uuid,u64>> sequences;
  std::vector<RowDataSlot> slots;
  explicit RowReadState(RowDataPageViewWorkspace) {}
  void Prepare(u32 count) { slots.reserve(count); }
  bool Remember(const Row& row) {
    return versions.insert(row.version_uuid).second&&
      sequences.emplace(row.row_uuid.value,row.row_version).second;
  }
  bool exhausted() const { return false; }
  bool CellValue(Row& row, Cell cell) { row.cells.push_back(std::move(cell));return true; }
  void Finish(RowDataPageBody& body,Row row,RowDataSlot slot) {
    slots.push_back(slot);body.rows.push_back(std::move(row));
  }
  const RowDataSlot& Expected(u32 index) const { return slots[index]; }
  void Slot(RowDataPageBody& body,RowDataSlot slot) { body.slots.push_back(slot); }
  bool Links(const RowDataPageBody& body) const { return PreviousLinksMatchPage(body); }
};

template<> struct RowReadState<true> {
  using Row=RowDataRecordView;
  using Cell=RowDataCellView;
  RowDataPageViewWorkspace workspace;
  std::size_t row_count=0,cell_count=0,cell_start=0;
  bool full=false;
  explicit RowReadState(RowDataPageViewWorkspace supplied):workspace(supplied) {}
  void Prepare(u32) {
    std::fill(workspace.version_index.begin(),workspace.version_index.end(),0);
    std::fill(workspace.sequence_index.begin(),workspace.sequence_index.end(),0);
  }
  static u64 Hash(const scratchbird::core::platform::Uuid& uuid,u64 sequence=0) {
    u64 hash=Fnv1a64(uuid.bytes.data(),uuid.bytes.size());
    for(unsigned i=0;i<8;++i){hash^=(sequence>>(8*i))&255;hash*=1099511628211ull;}
    return hash;
  }
  // Exact equality remains native UUID plus sequence; hash collisions never
  // establish identity. Probe count is bounded by caller-owned index capacity.
  std::optional<std::size_t> Find(std::span<const u32> index,
      const scratchbird::core::platform::Uuid& uuid,u64 sequence,bool version) const {
    if(index.empty())return {};
    const auto start=Hash(uuid,sequence)%index.size();
    for(std::size_t probe=0;probe<index.size();++probe) {
      const auto at=(start+probe)%index.size(); const auto entry=index[at];
      if(!entry)return {};
      const auto& row=workspace.rows[entry-1];
      if(version?row.version_uuid==uuid:
          row.row_uuid.value==uuid&&row.row_version==sequence)return entry-1;
    }
    return {};
  }
  bool Insert(std::span<u32> index,const scratchbird::core::platform::Uuid& uuid,
              u64 sequence,bool version) {
    if(index.empty()){full=true;return false;}
    const auto start=Hash(uuid,sequence)%index.size();
    for(std::size_t probe=0;probe<index.size();++probe) {
      const auto at=(start+probe)%index.size();
      if(!index[at]){index[at]=static_cast<u32>(row_count+1);return true;}
      const auto& row=workspace.rows[index[at]-1];
      if(version?row.version_uuid==uuid:
          row.row_uuid.value==uuid&&row.row_version==sequence)return false;
    }
    full=true;return false;
  }
  bool Remember(const Row& row) {
    if(row_count>=workspace.rows.size()||row_count>=workspace.slots.size()||
       row_count>=std::numeric_limits<u32>::max()){full=true;return false;}
    workspace.rows[row_count]=row;
    return Insert(workspace.version_index,row.version_uuid,0,true)&&
      Insert(workspace.sequence_index,row.row_uuid.value,row.row_version,false);
  }
  bool exhausted() const { return full; }
  bool CellValue(Row&,Cell cell) {
    if(cell_count>=workspace.cells.size()){full=true;return false;}
    workspace.cells[cell_count++]=cell;return true;
  }
  void Finish(RowDataPageView& body,Row row,RowDataSlot slot) {
    row.cells=workspace.cells.subspan(cell_start,cell_count-cell_start);
    cell_start=cell_count;workspace.rows[row_count]=row;workspace.slots[row_count]=slot;
    ++row_count;body.rows=workspace.rows.first(row_count);body.slots=workspace.slots.first(row_count);
  }
  const RowDataSlot& Expected(u32 index) const { return workspace.slots[index]; }
  void Slot(RowDataPageView&,RowDataSlot) {}
  bool Links(const RowDataPageView& body) const {
    for(const auto& row:body.rows) {
      if(!row.previous_row_version)continue;
      const auto version=Find(workspace.version_index,row.previous_version_uuid,0,true);
      if(version&&(workspace.rows[*version].row_uuid.value!=row.row_uuid.value||
          workspace.rows[*version].row_version!=row.previous_row_version))return false;
      const auto sequence=Find(workspace.sequence_index,row.row_uuid.value,row.previous_row_version,false);
      if(sequence&&workspace.rows[*sequence].version_uuid!=row.previous_version_uuid)return false;
    }
    return true;
  }
};

template<bool Borrowed = false>
ReadResult<Borrowed> ParseRowDataPageBodyImpl(
    std::span<const byte> serialized, u64 page_number,
    const scratchbird::core::datatypes::DatatypeBinaryDiagnosticContextV1* binary_context,
    bool retain_serialized = true, RowDataPageViewWorkspace workspace = {}) {
  if (serialized.size() < kRowDataPageBodyHeaderBytes) {
    return RowReadError<Borrowed>("SB-ROW-DATA-PAGE-BODY-SHORT",
                        "storage.row_data_page.body_short",
                        page_number);
  }
  if (std::memcmp(serialized.data() + kOffsetMagic, kRowDataMagic, sizeof(kRowDataMagic)) != 0) {
    if (HasRowDataMagicPrefix(serialized)) {
      return RowReadError<Borrowed>("SB-ROW-DATA-PAGE-FORMAT-UNSUPPORTED",
                          "storage.row_data_page.format_unsupported");
    }
    return RowReadError<Borrowed>("SB-ROW-DATA-PAGE-MAGIC-INVALID",
                        "storage.row_data_page.magic_invalid");
  }
  const u32 row_header_bytes = kRowHeaderBytes;
  const u32 header_bytes = LoadLittle32(serialized.data() + kOffsetHeaderBytes);
  const u32 row_count = LoadLittle32(serialized.data() + kOffsetRowCount);
  const u32 body_bytes = LoadLittle32(serialized.data() + kOffsetBodyBytes);
  const u32 slot_directory_offset =
      LoadLittle32(serialized.data() + kOffsetSlotDirectoryOffset);
  const u32 free_space_bytes = LoadLittle32(serialized.data() + kOffsetFreeSpaceBytes);
  if (header_bytes != kRowDataPageBodyHeaderBytes || body_bytes > serialized.size() ||
      serialized.size() > std::numeric_limits<u32>::max() ||
      LoadLittle32(serialized.data() + 20) != 0) {
    return RowReadError<Borrowed>("SB-ROW-DATA-PAGE-BODY-SIZE-INVALID",
                        "storage.row_data_page.body_size_invalid");
  }
  const u64 slot_bytes = static_cast<u64>(row_count) * kSlotEntryBytes;
  if (slot_directory_offset < kRowDataPageBodyHeaderBytes ||
      static_cast<u64>(slot_directory_offset) + slot_bytes != body_bytes ||
      free_space_bytes != serialized.size() - body_bytes) {
    return RowReadError<Borrowed>("SB-ROW-DATA-PAGE-SLOT-DIRECTORY-INVALID",
                        "storage.row_data_page.slot_directory_invalid");
  }
  if (LoadLittle64(serialized.data() + kOffsetBodyChecksum) !=
      Fnv1a64WithZeroChecksum(serialized.data(), serialized.size(), kOffsetBodyChecksum)) {
    return RowReadError<Borrowed>("SB-ROW-DATA-PAGE-CHECKSUM-MISMATCH",
                        "storage.row_data_page.checksum_mismatch");
  }

  ReadResult<Borrowed> result;
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
  if constexpr (!Borrowed) {
    if (retain_serialized) result.serialized.assign(serialized.begin(), serialized.end());
  }

  if (!IsTypedEngineIdentity(result.body.relation_uuid, UuidKind::object) ||
      result.body.segment_id == 0 || result.body.segment_generation == 0 ||
      result.body.page_generation == 0 || result.body.compaction_generation == 0) {
    return RowReadError<Borrowed>("CATALOG.INVALID_INPUT", "storage.row_data_page.identity_scope_invalid");
  }
  RowReadState<Borrowed> state(workspace);
  u32 offset = kRowDataPageBodyHeaderBytes;
  state.Prepare(row_count);
  for (u32 row_index = 0; row_index < row_count; ++row_index) {
    if (offset > slot_directory_offset || row_header_bytes > slot_directory_offset - offset) {
      return RowReadError<Borrowed>("SB-ROW-DATA-PAGE-ROW-SHORT",
                          "storage.row_data_page.row_short",
                          row_index);
    }
    const u32 row_start = offset;
    typename RowReadState<Borrowed>::Row row;
    row.row_uuid.kind = UuidKind::row;
    std::copy(serialized.begin() + offset, serialized.begin() + offset + 16, row.row_uuid.value.bytes.begin());
    row.transaction_uuid.kind = UuidKind::transaction;
    std::copy(serialized.begin() + offset + 16, serialized.begin() + offset + 32, row.transaction_uuid.value.bytes.begin());
    row.local_transaction_id = LoadLittle64(serialized.data() + offset + 32);
    row.row_version = LoadLittle64(serialized.data() + offset + 40);
    row.storage_generation = LoadLittle64(serialized.data() + offset + kRowOffsetStorageGeneration);
    if (row.storage_generation == 0 || row.storage_generation > result.body.page_generation)
      return RowReadError<Borrowed>("CATALOG.INVALID_INPUT", "storage.row_data_page.storage_generation_invalid");
    const auto row_flags = LoadLittle16(serialized.data() + offset + 48);
    if ((row_flags & ~RowFlag::deleted) != 0) {
      return RowReadError<Borrowed>("CATALOG.INVALID_INPUT", "storage.row_data_page.row_flags_invalid");
    }
    row.deleted = (row_flags & RowFlag::deleted) != 0;
    const u16 cell_count = LoadLittle16(serialized.data() + offset + 50);
    row.internal_row_ordinal = LoadLittle32(serialized.data() + offset + kRowOffsetInternalOrdinal);
    const u32 row_bytes = LoadLittle32(serialized.data() + offset + kRowOffsetRowBytes);
    const u64 row_checksum = LoadLittle64(serialized.data() + offset + kRowOffsetRowChecksum);
    row.stable_slot_id = LoadLittle32(serialized.data() + offset + kRowOffsetStableSlotId);
    row.previous_row_version =
        LoadLittle64(serialized.data() + offset + kRowOffsetPreviousRowVersion);
    row.next_row_version =
        LoadLittle64(serialized.data() + offset + kRowOffsetNextRowVersion);
    std::copy_n(serialized.begin() + offset + kRowOffsetVersionUuid, 16, row.version_uuid.bytes.begin());
    std::copy_n(serialized.begin() + offset + kRowOffsetPreviousVersionUuid, 16, row.previous_version_uuid.bytes.begin());
    std::copy_n(serialized.begin() + offset + kRowOffsetNextVersionUuid, 16, row.next_version_uuid.bytes.begin());
    if (!ValidRowIdentity(row) || row.internal_row_ordinal != row_index + 1 ||
        row.stable_slot_id == 0 || !state.Remember(row) ||
        row_bytes < row_header_bytes || row_bytes > slot_directory_offset - row_start) {
      if constexpr (Borrowed) if (state.exhausted()) return RowWorkspaceError();
      return RowReadError<Borrowed>("CATALOG.INVALID_INPUT", "storage.row_data_page.row_identity_or_extent_invalid");
    }
    const u32 row_end = row_start + row_bytes;
    if (row.previous_row_version != 0 && row.previous_row_version >= row.row_version) {
      return RowReadError<Borrowed>("SB-ROW-DATA-PAGE-LINEAGE-INVALID",
                          "storage.row_data_page.previous_lineage_invalid",
                          row_index);
    }
    if (row.next_row_version != 0 && row.next_row_version <= row.row_version) {
      return RowReadError<Borrowed>("SB-ROW-DATA-PAGE-LINEAGE-INVALID",
                          "storage.row_data_page.next_lineage_invalid",
                          row_index);
    }
    offset += row_header_bytes;

    for (u16 cell_index = 0; cell_index < cell_count; ++cell_index) {
      if (offset > row_end || kCellHeaderBytes > row_end - offset) {
        return RowReadError<Borrowed>("SB-ROW-DATA-PAGE-CELL-SHORT",
                            "storage.row_data_page.cell_short",
                            cell_index);
      }
      typename RowReadState<Borrowed>::Cell cell;
      cell.column_ordinal = LoadLittle16(serialized.data() + offset);
      if (LoadLittle16(serialized.data() + offset + 2) != 0) {
        return RowReadError<Borrowed>("CATALOG.INVALID_INPUT", "storage.row_data_page.cell_reserved_invalid");
      }
      const u32 payload_bytes = LoadLittle32(serialized.data() + offset + 4);
      const u64 payload_checksum = LoadLittle64(serialized.data() + offset + 8);
      offset += kCellHeaderBytes;
      if (payload_bytes > row_end - offset) {
        return RowReadError<Borrowed>("SB-ROW-DATA-PAGE-CELL-PAYLOAD-SHORT",
                            "storage.row_data_page.cell_payload_short",
                            cell_index);
      }
      if (payload_checksum != Fnv1a64(serialized.data() + offset, payload_bytes)) {
        return RowReadError<Borrowed>("SB-ROW-DATA-PAGE-CELL-CHECKSUM-MISMATCH",
                            "storage.row_data_page.cell_checksum_mismatch",
                            cell_index);
      }
      if (binary_context != nullptr) {
        const auto decoded = scratchbird::core::datatypes::DecodeCanonicalBinaryValueViewNoAlloc(
            serialized.data() + offset, payload_bytes, *binary_context);
        if (!decoded.ok()) {
          ReadResult<Borrowed> refused;
          if constexpr (Borrowed) refused.error=RowDataPageViewError::binary_failure;
          refused.status = decoded.status;
          refused.binary_diagnostic = decoded.diagnostic;
          return refused;
        }
        if constexpr (Borrowed) {
          cell.value = decoded.value;
        } else {
          cell.value.type_id = decoded.value.type_id;
          if (decoded.value.payload_bytes != 0) {
            cell.value.payload.assign(decoded.value.payload_data,
                decoded.value.payload_data + decoded.value.payload_bytes);
          }
        }
      } else {
        if constexpr (!Borrowed) {
          std::vector<byte> encoded(serialized.begin() + offset, serialized.begin() + offset + payload_bytes);
          auto decoded = DecodeDatatypeBinaryValue(encoded);
          if (!decoded.ok()) {
            RowDataPageResult decoded_result;
            decoded_result.status = decoded.status;
            decoded_result.diagnostic = std::move(decoded.diagnostic);
            return decoded_result;
          }
          cell.value = std::move(decoded.value);
        }
      }
      if (!state.CellValue(row,std::move(cell))) {
        if constexpr (Borrowed) return RowWorkspaceError();
      }
      offset += payload_bytes;
    }
    if (row_bytes != offset - row_start) {
      return RowReadError<Borrowed>("SB-ROW-DATA-PAGE-ROW-BYTES-MISMATCH",
                          "storage.row_data_page.row_bytes_mismatch",
                          row_index);
    }
    if (row_checksum != Fnv1a64WithZeroChecksum(serialized.data() + row_start,
                                               offset - row_start, kRowOffsetRowChecksum)) {
      return RowReadError<Borrowed>("SB-ROW-DATA-PAGE-ROW-CHECKSUM-MISMATCH",
                          "storage.row_data_page.row_checksum_mismatch",
                          row_index);
    }
    RowDataSlot slot;
    slot.stable_slot_id = row.stable_slot_id;
    slot.row_offset = row_start;
    slot.row_bytes = row_bytes;
    slot.row_checksum = row_checksum;
    slot.deleted = row.deleted;
    state.Finish(result.body,std::move(row),slot);
  }
  if (offset != slot_directory_offset) {
    return RowReadError<Borrowed>("SB-ROW-DATA-PAGE-SLOT-DIRECTORY-OFFSET-MISMATCH",
                        "storage.row_data_page.slot_directory_offset_mismatch",
                        offset);
  }
  for (u32 slot_index = 0; slot_index < row_count; ++slot_index) {
    const RowDataSlot& expected = state.Expected(slot_index);
    RowDataSlot slot;
    slot.stable_slot_id = LoadLittle32(serialized.data() + offset + kSlotOffsetStableSlotId);
    slot.row_offset = LoadLittle32(serialized.data() + offset + kSlotOffsetRowOffset);
    slot.row_bytes = LoadLittle32(serialized.data() + offset + kSlotOffsetRowBytes);
    const u32 slot_flags = LoadLittle32(serialized.data() + offset + kSlotOffsetFlags);
    if ((slot_flags & ~static_cast<u32>(RowFlag::deleted)) != 0) {
      return RowReadError<Borrowed>("CATALOG.INVALID_INPUT", "storage.row_data_page.slot_flags_invalid");
    }
    slot.deleted = (slot_flags & RowFlag::deleted) != 0;
    slot.row_checksum = LoadLittle64(serialized.data() + offset + kSlotOffsetRowChecksum);
    if (slot.stable_slot_id != expected.stable_slot_id ||
        slot.row_offset != expected.row_offset ||
        slot.row_bytes != expected.row_bytes ||
        slot.deleted != expected.deleted ||
        slot.row_checksum != expected.row_checksum) {
      return RowReadError<Borrowed>("SB-ROW-DATA-PAGE-SLOT-DIRECTORY-MISMATCH",
                          "storage.row_data_page.slot_directory_mismatch",
                          slot_index);
    }
    state.Slot(result.body,slot);
    offset += kSlotEntryBytes;
  }
  if (!state.Links(result.body)) {
    return RowReadError<Borrowed>("CATALOG.INVALID_INPUT", "storage.row_data_page.previous_identity_mismatch");
  }
  if constexpr (Borrowed) result.error=RowDataPageViewError::none;
  return result;
}
}  // namespace

RowDataPageResult ParseRowDataPageBody(const std::vector<byte>& serialized, u64 page_number) {
  return ParseRowDataPageBodyImpl(serialized, page_number, nullptr);
}

RowDataPageResult ParseRowDataPageBodyWithCanonicalBinaryCells(
    const std::vector<byte>& serialized, u64 page_number,
    const scratchbird::core::datatypes::DatatypeBinaryDiagnosticContextV1& context) {
  return ParseRowDataPageBodyImpl(serialized, page_number, &context);
}

RowDataPageResult ParseRowDataPageRows(std::span<const byte> serialized, u64 page_number) {
  return ParseRowDataPageBodyImpl(serialized, page_number, nullptr, false);
}

RowDataPageResult ParseRowDataPageRowsWithCanonicalBinaryCells(
    std::span<const byte> serialized, u64 page_number,
    const scratchbird::core::datatypes::DatatypeBinaryDiagnosticContextV1& context) {
  return ParseRowDataPageBodyImpl(serialized, page_number, &context, false);
}


RowDataPageViewRequirements RowDataPageViewBackingRequirements(std::size_t bytes) noexcept {
  RowDataPageViewRequirements out;
  if(bytes<kRowDataPageBodyHeaderBytes||bytes>std::numeric_limits<u32>::max())return out;
  const auto payload=bytes-kRowDataPageBodyHeaderBytes;
  out.rows=payload/(kRowHeaderBytes+kSlotEntryBytes);
  out.cells=payload/(kCellHeaderBytes+32); // Direct binary envelope is at least 32 bytes.
  out.index_slots=out.rows?out.rows*2+1:0;
  return out;
}

RowDataPageViewResult ParseRowDataPageWithCanonicalBinaryCellsInto(
    std::span<const byte> input,u64 page_number,RowDataPageViewWorkspace workspace,
    const scratchbird::core::datatypes::DatatypeBinaryDiagnosticContextV1& context) noexcept {
  // Reject invalid/overlapping backing before the first scratch write.
  struct Range { std::uintptr_t begin=0,end=0; };
  std::array<Range,6> ranges{};
  bool valid=true; std::size_t count=0;
  const auto add=[&](const auto& span) {
    using Item=typename std::remove_reference_t<decltype(span)>::element_type;
    if(span.empty())return;
    if(!span.data()||reinterpret_cast<std::uintptr_t>(span.data())%alignof(Item)||span.size()>std::numeric_limits<std::size_t>::max()/sizeof(Item)){valid=false;return;}
    const auto begin=reinterpret_cast<std::uintptr_t>(span.data());
    const auto bytes=span.size()*sizeof(Item);
    if(begin>std::numeric_limits<std::uintptr_t>::max()-bytes){valid=false;return;}
    ranges[count++]={begin,begin+bytes};
  };
  add(input);add(workspace.rows);add(workspace.cells);add(workspace.slots);
  add(workspace.version_index);add(workspace.sequence_index);
  for(std::size_t i=0;i<count;++i)for(std::size_t j=0;j<i;++j)
    if(ranges[i].begin<ranges[j].end&&ranges[j].begin<ranges[i].end)valid=false;
  if(!valid)return RowWorkspaceError(true);
  return ParseRowDataPageBodyImpl<true>(input,page_number,&context,false,workspace);
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
