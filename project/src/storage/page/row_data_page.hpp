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
#include "runtime_platform.hpp"
#include "uuid.hpp"

#include <string>
#include <vector>

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

struct RowDataCell {
  u16 column_ordinal = 0;
  DatatypeBinaryValue value;
};

struct RowDataRecord {
  TypedUuid row_uuid;
  TypedUuid transaction_uuid;
  u64 local_transaction_id = 0;
  u32 internal_row_ordinal = 0;
  u32 stable_slot_id = 0;
  u32 row_version = 1;
  u64 previous_row_version = 0;
  u64 next_row_version = 0;
  bool deleted = false;
  std::vector<RowDataCell> cells;
};

struct RowDataSlot {
  u32 stable_slot_id = 0;
  u32 row_offset = 0;
  u32 row_bytes = 0;
  u64 row_checksum = 0;
  bool deleted = false;
};

struct RowDataPageBody {
  TypedUuid relation_uuid;
  u64 segment_id = 0;
  u64 segment_generation = 0;
  u64 page_number = 0;
  u64 page_generation = 0;
  u64 compaction_generation = 0;
  u64 next_page_number = 0;
  u32 free_space_offset = 0;
  u32 free_space_bytes = 0;
  std::vector<RowDataRecord> rows;
  std::vector<RowDataSlot> slots;
};

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

  bool ok() const {
    return status.ok();
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
RowDataPageResult ParseRowDataPageBody(const std::vector<byte>& serialized, u64 page_number);
DiagnosticRecord MakeRowDataPageDiagnostic(Status status,
                                           std::string diagnostic_code,
                                           std::string message_key,
                                           std::string detail = {});

}  // namespace scratchbird::storage::page
