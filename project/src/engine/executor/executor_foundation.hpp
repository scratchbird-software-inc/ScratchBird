// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::executor {

struct Tuple {
  std::vector<std::int64_t> values;
};

struct Batch {
  std::string descriptor_digest;
  std::vector<Tuple> rows;
};

struct OperatorDiagnostic {
  bool ok = true;
  std::string diagnostic_code = "SB_EXECUTOR_OK";
};

struct OperatorCatalogEntry {
  std::string operator_id;
  std::string family;
  bool descriptor_required = true;
  bool storage_backed = false;
  bool can_materialize = false;
};

enum class Int64ComparisonOperator {
  kGreaterThan,
  kGreaterThanOrEqual,
  kLessThan,
  kLessThanOrEqual,
  kEqual,
  kNotEqual,
};

Batch MakeBatch(std::string descriptor_digest, std::vector<Tuple> rows);
OperatorDiagnostic ValidateBatch(const Batch& batch);
std::int64_t EvalAdd(std::int64_t lhs, std::int64_t rhs);
std::int64_t EvalMultiply(std::int64_t lhs, std::int64_t rhs);
Batch FilterByInt64Comparison(const Batch& input,
                              std::size_t column,
                              Int64ComparisonOperator op,
                              std::int64_t threshold);
Batch FilterGreaterThan(const Batch& input, std::size_t column, std::int64_t threshold);
Batch ProjectColumns(const Batch& input, const std::vector<std::size_t>& columns);
Batch SortByColumn(const Batch& input, std::size_t column, bool ascending);
Batch LimitOffset(const Batch& input, std::size_t limit, std::size_t offset);
Batch AggregateSumByKey(const Batch& input, std::size_t key_column, std::size_t value_column);
Batch NestedLoopJoinEqual(const Batch& left, const Batch& right, std::size_t left_column, std::size_t right_column);
Batch HashJoinEqual(const Batch& left, const Batch& right, std::size_t left_column, std::size_t right_column);
Batch MergeJoinEqual(const Batch& left_sorted, const Batch& right_sorted, std::size_t left_column, std::size_t right_column);
Batch AddRowNumberWindow(const Batch& input, std::size_t order_column);
Batch AddRankWindow(const Batch& input, std::size_t order_column);
Batch AddDenseRankWindow(const Batch& input, std::size_t order_column);
Batch AddPartitionCountWindow(const Batch& input, std::size_t partition_column);
Batch AddNtileWindow(const Batch& input, std::size_t order_column, std::int64_t bucket_count);
Batch AddLagWindow(const Batch& input, std::size_t order_column, std::size_t value_column);
Batch AddLeadWindow(const Batch& input, std::size_t order_column, std::size_t value_column);
Batch AddFirstValueWindow(const Batch& input, std::size_t order_column, std::size_t value_column);
Batch AddLastValueWindow(const Batch& input, std::size_t order_column, std::size_t value_column);
Batch MaterializeCte(const Batch& input);
std::int64_t ScalarSubqueryFirstValue(const Batch& input, std::size_t column);
Batch SetUnionDistinct(const Batch& left, const Batch& right);
Batch SetIntersectDistinct(const Batch& left, const Batch& right);
Batch SetExceptDistinct(const Batch& left, const Batch& right);
Batch SetUnionAll(const Batch& left, const Batch& right);
Batch SetIntersectAll(const Batch& left, const Batch& right);
Batch SetExceptAll(const Batch& left, const Batch& right);
std::vector<OperatorCatalogEntry> Stage6OperatorCatalog();
bool ValidateOperatorCatalog(const std::vector<OperatorCatalogEntry>& catalog, std::vector<std::string>* errors);

}  // namespace scratchbird::engine::executor
