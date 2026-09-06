// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "dml/insert_physical_integration.hpp"
#include "crud_support/crud_store.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api::dml::detail {

// SEARCH_KEY: SB_ENGINE_DIRECT_BULK_GENERATED_PROJECTION_AUTHORITY
// Owns deterministic generated-column projection for direct INSERT SELECT.
// It produces values only; relation mutation and MGA publication remain with
// the canonical transactional relation-store facade.
enum class DirectGeneratedProjectionKind {
  unsupported,
  counter,
  literal,
  mod,
  prefix_counter,
  prefix_counter_offset,
  case_zero_literal_else_literal,
  case_zero_literal_else_prefix_counter_offset,
  cast_divide,
  counter_multiply,
  mod_equals
};

struct DirectScaledDecimalOperand {
  std::uint64_t value = 0;
  int scale = 0;
};

struct DirectGeneratedProjectionPlan {
  std::string column_name;
  std::string descriptor;
  std::string type_name;
  std::vector<std::string> parts;
  DirectGeneratedProjectionKind kind =
      DirectGeneratedProjectionKind::unsupported;
  std::string literal_value;
  std::string alternate_literal_value;
  std::string prefix;
  std::uint64_t modulus = 0;
  std::uint64_t expected = 0;
  long long offset = 0;
  long double factor = 0.0L;
  int scale = 0;
  bool has_scaled_operand = false;
  DirectScaledDecimalOperand scaled_operand;
  bool has_exact_scaled_result_multiplier = false;
  std::uint64_t exact_scaled_result_multiplier = 0;
  std::uint64_t output_scale_factor = 0;
};

struct DirectGeneratedCounterPlan {
  bool requested = false;
  bool ok = false;
  std::string failure_reason;
  std::uint64_t start = 0;
  std::uint64_t step = 0;
  std::uint64_t limit = 0;
  std::uint64_t row_count = 0;
  std::vector<DirectGeneratedProjectionPlan> projections;
};

bool DirectGeneratedCounterEnvelopeRequested(
    const DirectPhysicalBulkAppendRequest& request);
std::string DirectGeneratedProjectionValue(
    const DirectGeneratedProjectionPlan& plan,
    std::uint64_t counter);
DirectGeneratedCounterPlan DirectBuildGeneratedCounterPlan(
    const DirectPhysicalBulkAppendRequest& request,
    const CrudTableRecord& table);
std::size_t DirectRequestRowCount(
    const DirectPhysicalBulkAppendRequest& request,
    const DirectGeneratedCounterPlan& generated);

}  // namespace scratchbird::engine::internal_api::dml::detail
