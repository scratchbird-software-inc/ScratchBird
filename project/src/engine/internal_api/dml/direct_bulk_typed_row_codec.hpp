// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "crud_support/crud_store.hpp"
#include "datatype_operations.hpp"
#include "dml/insert_batch.hpp"
#include "insert_physical_integration.hpp"
#include "physical_mga_cow_store.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api::dml::detail {

// Owns conversion and validation of direct-bulk typed values into physical
// row cells. It does not publish rows or decide transaction finality.

namespace dt = scratchbird::core::datatypes;

struct DirectFixedWidthPayloadValidationColumnPlan {
  std::string column_name;
  std::string canonical_type_name;
  dt::CanonicalTypeId target_type = dt::CanonicalTypeId::unknown;
  bool character_type = false;
  bool inline_fixed = false;
  std::size_t inline_bytes = 0;
};

struct DirectPhysicalTypedCells {
  std::vector<scratchbird::storage::page::RowDataCell> cells;
  std::uint64_t typed_binary_cells = 0;
  std::uint64_t int64_cells = 0;
  std::uint64_t null_cells = 0;
  std::uint64_t character_cells = 0;
  std::map<dt::CanonicalTypeId, std::uint64_t> typed_cell_counts;
};

struct DirectFixedWidthPayloadValidationStats {
  std::uint64_t rows_checked = 0;
  std::uint64_t cells_seen = 0;
  std::uint64_t null_cells = 0;
  std::uint64_t unknown_type_skips = 0;
  std::uint64_t character_type_skips = 0;
  std::uint64_t non_fixed_type_skips = 0;
  std::uint64_t binary_exact_hits = 0;
  std::uint64_t binary_integer_downcast_hits = 0;
  std::uint64_t binary_shape_hits = 0;
  std::uint64_t text_pack_attempts = 0;
  std::uint64_t text_pack_successes = 0;
  std::uint64_t text_pack_failures = 0;
  std::map<dt::CanonicalTypeId, std::uint64_t> cells_by_type;
  std::map<dt::CanonicalTypeId, std::uint64_t> text_pack_attempts_by_type;
};

bool DirectPackTypedPayload(
    dt::CanonicalTypeId target_type,
    const EngineTypedValue& typed,
    std::vector<scratchbird::core::platform::byte>* out);

std::vector<scratchbird::storage::page::RowDataCell> DirectPhysicalCells(
    const std::vector<std::pair<std::string, std::string>>& values);

DirectPhysicalTypedCells DirectPhysicalCellsFromTypedInputRow(
    const EngineRowValue& input_row,
    const InsertRowEncoderPlan& row_encoder_plan,
    const std::vector<DirectFixedWidthPayloadValidationColumnPlan>* column_plan =
        nullptr);

std::vector<DirectFixedWidthPayloadValidationColumnPlan>
BuildDirectFixedWidthPayloadValidationPlan(
    const InsertRowEncoderPlan& row_encoder_plan,
    const EngineRowValue* first_row = nullptr);

std::string DirectFixedWidthTypedPayloadFailure(
    const EngineRowValue& input_row,
    const std::vector<DirectFixedWidthPayloadValidationColumnPlan>& plan,
    DirectFixedWidthPayloadValidationStats* stats = nullptr);

void AddFixedWidthPayloadValidationEvidence(
    const DirectFixedWidthPayloadValidationStats& stats,
    DirectPhysicalBulkAppendResult* result);

}  // namespace scratchbird::engine::internal_api::dml::detail
