// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-ROW-DATA-PAGE-ANCHOR
#include "datatype_binary.hpp"
#include "datatype_binary_view.hpp"
#include "datatype_time.hpp"
#include "datatype_timestamp_diagnostic.hpp"
#include "runtime_platform.hpp"
#include "uuid.hpp"
#include "row_external_value_locator.hpp"

#include <string>
#include <optional>
#include <span>
#include <vector>
#include <memory>

namespace scratchbird::core::datatypes {
struct DateValidatedProfileHandleV3;
struct TimeValidatedProfileHandleV3;
struct TimestampValidatedProfileHandleV3;
}

namespace scratchbird::storage::page {

using scratchbird::core::datatypes::DatatypeBinaryValue;
using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

inline constexpr u32 kRowDataPageBodyHeaderBytes = 96;

// Synchronous, borrowed receiving schema. The owner supplies profiles from the
// admitted relation, not from a cell's enum or payload. This component validates
// representation and relation binding; it does not grant catalog/MGA authority.
// Exactly one profile per column, in strictly increasing ordinal order. Profiles
// and the span must remain immutable and alive throughout the call. Returned
// owning rows retain no borrowed schema, so a rebuild needs explicit re-binding.
struct RowDataTemporalColumnBinding {
  u16 column_ordinal = 0;
  bool null_allowed = false;
  std::shared_ptr<const core::datatypes::DateValidatedProfileHandleV3> date;
  std::shared_ptr<const core::datatypes::TimeValidatedProfileHandleV3> time;
  std::shared_ptr<const core::datatypes::TimestampValidatedProfileHandleV3> timestamp;
};
struct RowDataTemporalReceiver {
  TypedUuid relation_uuid;
  std::span<const RowDataTemporalColumnBinding> columns;
};
// Lifetime owner for operations which retain work beyond the submitting call.
struct RowDataTemporalSchema {
  TypedUuid relation_uuid;
  std::vector<RowDataTemporalColumnBinding> columns;
  RowDataTemporalReceiver receiver() const noexcept { return {relation_uuid, columns}; }
};

struct RowDataCell {
  u16 column_ordinal = 0;
  DatatypeBinaryValue value;
  // Exactly one representation: a locator requires an empty default value.
  std::optional<RowExternalValueLocator> external_value;
};

template<class Cells>
struct BasicRowDataRecord {
  TypedUuid row_uuid;
  // Issued by the physical mutation owner, never derived from the row/sequence.
  scratchbird::core::platform::Uuid version_uuid;
  scratchbird::core::platform::Uuid previous_version_uuid;
  scratchbird::core::platform::Uuid next_version_uuid;
  TypedUuid transaction_uuid;
  u64 local_transaction_id = 0;
  u32 internal_row_ordinal = 0;
  u32 stable_slot_id = 0;
  u64 row_version = 1;
  // Native page generation at admission of this version's current residency.
  // Preserved when later versions change the surrounding page image.
  u64 storage_generation = 0;
  u64 previous_row_version = 0;
  u64 next_row_version = 0;
  bool deleted = false;
  Cells cells;
};
using RowDataRecord = BasicRowDataRecord<std::vector<RowDataCell>>;
struct RowDataCellView {
  u16 column_ordinal = 0;
  scratchbird::core::datatypes::DatatypeBinaryValueView value;
};
using RowDataRecordView = BasicRowDataRecord<std::span<const RowDataCellView>>;

struct RowDataSlot {
  u32 stable_slot_id = 0;
  u32 row_offset = 0;
  u32 row_bytes = 0;
  u64 row_checksum = 0;
  bool deleted = false;
};

template<class Rows, class Slots>
struct BasicRowDataPageBody {
  TypedUuid relation_uuid;
  u64 segment_id = 0;
  u64 segment_generation = 0;
  u64 page_number = 0;
  u64 page_generation = 0;
  u64 compaction_generation = 0;
  u64 next_page_number = 0;
  u32 free_space_offset = 0;
  u32 free_space_bytes = 0;
  Rows rows;
  Slots slots;
};
using RowDataPageBody = BasicRowDataPageBody<std::vector<RowDataRecord>, std::vector<RowDataSlot>>;
using RowDataPageView = BasicRowDataPageBody<std::span<const RowDataRecordView>, std::span<const RowDataSlot>>;

struct DenseRowOrdinalScope {
  TypedUuid relation_uuid;
  u64 segment_id = 0;
  u64 segment_generation = 0;
  u64 page_number = 0;
  u64 page_generation = 0;
};

struct DenseRowOrdinalLocator {
  DenseRowOrdinalScope scope;
  u32 internal_row_ordinal = 0;
  TypedUuid row_uuid;
  scratchbird::core::platform::Uuid version_uuid;
  TypedUuid transaction_uuid;
  u64 local_transaction_id = 0;
  bool durable_mga_inventory_authority_available = false;
  bool normal_mga_visibility_authority_available = false;
};

struct DenseRowOrdinalValidation {
  bool accepted = false;
  bool fail_closed_to_uuid_mga_lookup = true;
  bool durable_mga_inventory_remains_authority = true;
  bool ordinal_is_visibility_or_finality_authority = false;
  std::string refusal_reason;
  DenseRowOrdinalLocator locator;
  RowDataRecord row;
  std::vector<std::string> evidence;
};

struct RowDataPageResult {
  Status status;
  RowDataPageBody body;
  std::vector<byte> serialized;
  DiagnosticRecord diagnostic;
  // Present only for the canonical catalog-cell policy. Keep the producer's
  // native arguments intact; the owning DiagnosticRecord cannot represent all
  // of them. A failure never publishes body or serialized prefixes.
  std::optional<scratchbird::core::datatypes::DatatypeBinaryDiagnosticView>
      binary_diagnostic;
  // Preserve native typed arguments when a TIME receiver refuses a cell.
  std::optional<scratchbird::core::datatypes::TimeDiagnosticFactV3> time_diagnostic;
  std::optional<scratchbird::core::datatypes::TimestampDiagnosticFactV3> timestamp_diagnostic;

  bool ok() const {
    return status.ok();
  }
  bool resource_failure() const noexcept {
    using Code = scratchbird::core::platform::StatusCode;
    return status.code == Code::memory_allocation_failed ||
           status.code == Code::memory_limit_exceeded;
  }
};

u64 ComputeRowDataPageChecksum(const std::vector<byte>& body);
void AssignDenseInternalRowOrdinals(RowDataPageBody* body);
DenseRowOrdinalScope MakeDenseRowOrdinalScope(const RowDataPageBody& body);
DenseRowOrdinalLocator MakeDenseRowOrdinalLocator(const DenseRowOrdinalScope& scope,
                                                  const RowDataRecord& row,
                                                  bool durable_mga_inventory_authority_available,
                                                  bool normal_mga_visibility_authority_available);
DenseRowOrdinalValidation ValidateDenseRowOrdinalLocator(const RowDataPageBody& body,
                                                         const DenseRowOrdinalLocator& locator);
RowDataPageResult BuildRowDataPageBody(const RowDataPageBody& body, u32 page_size);
RowDataPageResult BuildRowDataPageBodyOwned(RowDataPageBody body, u32 page_size);
RowDataPageResult BuildRowDataPageBody(const RowDataPageBody& body, u32 page_size,
                                    const RowDataTemporalReceiver& receiver);
RowDataPageResult BuildRowDataPageBodyOwned(RowDataPageBody body, u32 page_size,
                                         const RowDataTemporalReceiver& receiver);
RowDataPageResult ParseRowDataPageBody(const std::vector<byte>& serialized, u64 page_number);
RowDataPageResult ParseRowDataPageBody(const std::vector<byte>& serialized, u64 page_number,
                                    const RowDataTemporalReceiver& receiver);
// Same row framing/identity/checksum admission as the general reader, but every
// cell must use the datatype owner's direct, non-NULL binary component profile.
// Context is reporting-only and must come from the containing admitted schema.
// This remains an owning row API, not whole-page allocation-free admission.
RowDataPageResult ParseRowDataPageBodyWithCanonicalBinaryCells(
    const std::vector<byte>& serialized, u64 page_number,
    const scratchbird::core::datatypes::DatatypeBinaryDiagnosticContextV1& context);
// Own the decoded rows, not a duplicate of the containing image. The input
// remains immutable for the call; returned rows are independent of its lifetime.
// serialized is intentionally empty on success. These are not grant-backed or
// allocation-free row containers; framing and diagnostics share the old reader.
RowDataPageResult ParseRowDataPageRows(std::span<const byte> serialized, u64 page_number);
RowDataPageResult ParseRowDataPageRows(std::span<const byte> serialized, u64 page_number,
                                    const RowDataTemporalReceiver& receiver);
RowDataPageResult ParseRowDataPageRowsWithCanonicalBinaryCells(
    std::span<const byte> serialized, u64 page_number,
    const scratchbird::core::datatypes::DatatypeBinaryDiagnosticContextV1& context);

// Caller-owned native arrays and hash-index scratch. No datatype payload is
// copied: successful cells borrow immutable input bytes. All backing must remain
// alive and disjoint from input through the returned view's last use. The owner
// supplies actual admitted memory; this component never grants it.
struct RowDataPageViewWorkspace {
  std::span<RowDataRecordView> rows;
  std::span<RowDataCellView> cells;
  std::span<RowDataSlot> slots;
  std::span<u32> version_index;
  std::span<u32> sequence_index;
};
struct RowDataPageViewRequirements {
  std::size_t rows = 0, cells = 0, index_slots = 0;
};
RowDataPageViewRequirements RowDataPageViewBackingRequirements(std::size_t image_bytes) noexcept;
// Fixed row diagnostics retain literal identities and an optional numeric
// detail. The owning legacy adapter alone renders that detail as text.
struct RowDataPageDiagnosticView {
  std::string_view diagnostic_code, message_key;
  std::optional<u64> detail;
  std::string_view source_component;
};
enum class RowDataPageViewError { none, row_failure, binary_failure, insufficient_workspace, invalid_workspace };
struct RowDataPageViewResult {
  RowDataPageViewError error = RowDataPageViewError::row_failure;
  Status status;
  RowDataPageView body;
  RowDataPageDiagnosticView diagnostic;
  std::optional<scratchbird::core::datatypes::DatatypeBinaryDiagnosticView> binary_diagnostic;
  bool ok() const noexcept { return error == RowDataPageViewError::none && status.ok(); }
};
RowDataPageViewResult ParseRowDataPageWithCanonicalBinaryCellsInto(
    std::span<const byte> input, u64 page_number, RowDataPageViewWorkspace workspace,
    const scratchbird::core::datatypes::DatatypeBinaryDiagnosticContextV1& context) noexcept;

DiagnosticRecord MakeRowDataPageDiagnostic(Status status,
                                           std::string diagnostic_code,
                                           std::string message_key,
                                           std::string detail = {});

}  // namespace scratchbird::storage::page
