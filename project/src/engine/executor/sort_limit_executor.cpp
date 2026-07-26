// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "descriptor_value_runtime.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <numeric>
#include <string_view>
#include <utility>

namespace scratchbird::engine::executor {
namespace {

DescriptorRuntimeDiagnostic Refusal(std::string code,
                                    std::string detail = {}) {
  DescriptorRuntimeDiagnostic diagnostic;
  diagnostic.ok = false;
  diagnostic.diagnostic_code = std::move(code);
  diagnostic.detail = std::move(detail);
  return diagnostic;
}

bool IsCanonicalUuid(const std::string_view value) {
  if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
      value[18] != '-' || value[23] != '-') {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) continue;
    const auto ch = static_cast<unsigned char>(value[index]);
    if (!std::isxdigit(ch) || std::isupper(ch)) return false;
  }
  return true;
}

std::string DescriptorField(const std::string& descriptor,
                            const std::string& key) {
  const std::string prefix = key + "=";
  std::string value;
  bool found = false;
  std::size_t start = 0;
  while (start <= descriptor.size()) {
    const auto end = descriptor.find(';', start);
    const auto field = descriptor.substr(
        start, end == std::string::npos ? std::string::npos : end - start);
    if (field.rfind(prefix, 0) == 0) {
      if (found) return {};
      value = field.substr(prefix.size());
      found = true;
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return value;
}

bool DescriptorU32(const std::string& descriptor,
                   const std::string& key,
                   std::uint32_t* value) {
  if (value == nullptr) return false;
  const auto field = DescriptorField(descriptor, key);
  if (field.empty() || (field.size() > 1 && field.front() == '0')) return false;
  std::uint64_t parsed = 0;
  for (const auto ch : field) {
    if (ch < '0' || ch > '9') return false;
    parsed = parsed * 10u + static_cast<unsigned>(ch - '0');
    if (parsed > std::numeric_limits<std::uint32_t>::max()) return false;
  }
  *value = static_cast<std::uint32_t>(parsed);
  return true;
}

bool TextSeedAbsent(
    const scratchbird::core::datatypes::DatatypeTextSeedAuthority& seed) {
  return !seed.active && seed.seed_pack_name.empty() &&
         seed.seed_pack_version.empty() && seed.charset_name.empty() &&
         seed.collation_name.empty() &&
         !seed.collation_case_insensitive &&
         !seed.collation_accent_insensitive;
}

bool CompareOrderValues(
    const scratchbird::engine::internal_api::EngineTypedValue& left,
    const scratchbird::engine::internal_api::EngineTypedValue& right,
    const CanonicalDescriptorOrderTerm& term,
    int* comparison,
    std::string* refusal_detail) {
  namespace dt = scratchbird::core::datatypes;
  if (comparison == nullptr || refusal_detail == nullptr) return false;
  *comparison = 0;
  refusal_detail->clear();
  const auto type_id =
      dt::CanonicalTypeIdFromStableName(left.descriptor.canonical_type_name);
  if (type_id == dt::CanonicalTypeId::unknown ||
      left.descriptor.canonical_type_name !=
          right.descriptor.canonical_type_name ||
      !left.binary_value.empty() || !right.binary_value.empty()) {
    *refusal_detail = "order operands do not share a supported scalar encoding";
    return false;
  }
  const auto null_ordering =
      term.null_placement == CanonicalDescriptorNullPlacement::first
          ? dt::DatatypeNullOrdering::nulls_first
          : dt::DatatypeNullOrdering::nulls_last;
  const bool has_null =
      left.state ==
          scratchbird::engine::internal_api::EngineValueState::sql_null ||
      right.state ==
          scratchbird::engine::internal_api::EngineValueState::sql_null;
  if (has_null) {
    dt::DatatypeComparisonRequest request;
    request.left.type_id = type_id;
    request.left.is_null =
        left.state ==
        scratchbird::engine::internal_api::EngineValueState::sql_null;
    request.right.type_id = type_id;
    request.right.is_null =
        right.state ==
        scratchbird::engine::internal_api::EngineValueState::sql_null;
    request.null_ordering = null_ordering;
    const auto compared = dt::CompareDatatypeValues(request);
    if (!compared.ok()) {
      *refusal_detail = compared.diagnostic.diagnostic_code;
      return false;
    }
    *comparison = compared.comparison;
  } else {
    const bool runtime_numeric =
        type_id == dt::CanonicalTypeId::decimal ||
        type_id == dt::CanonicalTypeId::decimal_float ||
        type_id == dt::CanonicalTypeId::int128 ||
        type_id == dt::CanonicalTypeId::uint128 ||
        type_id == dt::CanonicalTypeId::real128;
    if (runtime_numeric) {
      dt::DatatypeNumericOperationRequest request;
      request.operation = dt::DatatypeNumericOperationKind::compare;
      request.type_id = type_id;
      request.left.type_id = type_id;
      request.left.encoded_value = left.encoded_value;
      request.right.type_id = type_id;
      request.right.encoded_value = right.encoded_value;
      request.context.precision = 38;
      request.context.scale = 0;
      if (type_id == dt::CanonicalTypeId::decimal ||
          type_id == dt::CanonicalTypeId::decimal_float) {
        std::uint32_t precision = 0;
        std::uint32_t scale = 0;
        if (!DescriptorU32(left.descriptor.encoded_descriptor, "precision",
                           &precision) ||
            !DescriptorU32(left.descriptor.encoded_descriptor, "scale",
                           &scale)) {
          *refusal_detail =
              "order descriptor precision or scale is not bound";
          return false;
        }
        request.context.precision = precision;
        request.context.scale = scale;
      } else {
        std::uint32_t width = 0;
        if (!DescriptorU32(left.descriptor.encoded_descriptor, "width",
                           &width) ||
            width != 128) {
          *refusal_detail = "128-bit order descriptor width is invalid";
          return false;
        }
      }
      const auto numeric = dt::ApplyNumericOperation(request);
      if (!numeric.ok()) {
        *refusal_detail = numeric.diagnostic.diagnostic_code;
        return false;
      }
      *comparison = numeric.comparison;
    } else {
      const auto validate = [type_id](const auto& value) {
        dt::DatatypeCastRequest request;
        request.value.type_id = type_id;
        request.value.encoded_value = value.encoded_value;
        request.target_type_id = type_id;
        request.explicit_cast = true;
        return dt::CastDatatypeValue(request);
      };
      const auto left_checked = validate(left);
      const auto right_checked = validate(right);
      if (!left_checked.ok() || !right_checked.ok()) {
        *refusal_detail = "order operand encoding is invalid";
        return false;
      }
      dt::DatatypeComparisonRequest request;
      request.left = left_checked.value;
      request.right = right_checked.value;
      request.null_ordering = null_ordering;
      if (type_id == dt::CanonicalTypeId::character) {
        request.case_insensitive_character_compare =
            term.text_seed.collation_case_insensitive;
        request.text_seed = term.text_seed;
      }
      const auto compared = dt::CompareDatatypeValues(request);
      if (!compared.ok()) {
        *refusal_detail = compared.diagnostic.diagnostic_code;
        return false;
      }
      *comparison = compared.comparison;
    }
  }
  *comparison = *comparison < 0 ? -1 : (*comparison > 0 ? 1 : 0);
  if (!has_null &&
      term.direction == CanonicalDescriptorOrderDirection::descending) {
    *comparison = -*comparison;
  }
  return true;
}

}  // namespace

DescriptorRuntimeDiagnostic ValidateCanonicalDescriptorOrderTerm(
    const CanonicalDescriptorOrderTerm& term,
    const ExecutorColumnDescriptor& column) {
  namespace dt = scratchbird::core::datatypes;
  if (term.expression_descriptor_id == 0 ||
      term.expression_descriptor_id != column.descriptor_id) {
    return Refusal("QOW-DIAG-QRY-010-ORDER-REFUSAL-V1",
                   "order expression descriptor handle is unresolved");
  }
  if ((term.direction != CanonicalDescriptorOrderDirection::ascending &&
       term.direction != CanonicalDescriptorOrderDirection::descending) ||
      (term.null_placement != CanonicalDescriptorNullPlacement::first &&
       term.null_placement != CanonicalDescriptorNullPlacement::last)) {
    return Refusal("QOW-DIAG-QRY-010-ORDER-REFUSAL-V1",
                   "order direction or NULL placement is invalid");
  }
  const auto type_id = dt::CanonicalTypeIdFromStableName(
      column.descriptor.canonical_type_name);
  if (type_id == dt::CanonicalTypeId::unknown) {
    return Refusal("QOW-DIAG-QRY-010-ORDER-REFUSAL-V1",
                   "order expression type is unknown");
  }
  if (type_id == dt::CanonicalTypeId::character) {
    const auto descriptor_collation = DescriptorField(
        column.descriptor.encoded_descriptor, "collation_uuid");
    const auto& seed = term.text_seed;
    if (!IsCanonicalUuid(term.collation_uuid) ||
        descriptor_collation != term.collation_uuid ||
        term.resource_epoch == 0 || term.collation_epoch == 0 ||
        !seed.active || seed.seed_pack_name.empty() ||
        seed.seed_pack_version.empty() || seed.charset_name.empty() ||
        seed.collation_name.empty()) {
      return Refusal(
          "QOW-DIAG-QRY-010-ORDER-REFUSAL-V1",
          "character order lacks bound collation resource authority");
    }
  } else if (!term.collation_uuid.empty() || term.resource_epoch != 0 ||
             term.collation_epoch != 0 || !TextSeedAbsent(term.text_seed)) {
    return Refusal("QOW-DIAG-QRY-010-ORDER-REFUSAL-V1",
                   "non-character order term carries text collation authority");
  }
  return {};
}

CanonicalDescriptorOrderComparisonResult CompareCanonicalDescriptorOrderValues(
    const scratchbird::engine::internal_api::EngineTypedValue& left,
    const scratchbird::engine::internal_api::EngineTypedValue& right,
    const CanonicalDescriptorOrderTerm& term) {
  CanonicalDescriptorOrderComparisonResult result;
  std::string detail;
  if (!CompareOrderValues(left, right, term, &result.comparison, &detail)) {
    result = {};
    result.diagnostic = Refusal("QOW-DIAG-QRY-010-ORDER-REFUSAL-V1",
                                std::move(detail));
  }
  return result;
}

// QOW-SOURCE-QRY-007-SORT-LIMIT-V1
// First canonical implementation in this module: a typed LIMIT/OFFSET node.
// Typed ORDER BY terms are deliberately left to QRY-010; this entry does not
// fall back to the legacy one-integer-key sorter.
CanonicalDescriptorLimitResult ExecuteCanonicalDescriptorLimit(
    const CanonicalDescriptorLimitRequest& request) {
  CanonicalDescriptorLimitResult result;
  const auto refuse = [&](DescriptorRuntimeDiagnostic diagnostic) {
    result.diagnostic = std::move(diagnostic);
    result.output_batch = {};
    return result;
  };
  const auto dag_validation =
      ValidateTypedPhysicalNodeDag(request.physical_dag);
  if (!dag_validation.accepted) {
    const auto& issue = dag_validation.issues.front();
    return refuse(Refusal(issue.diagnostic_id, issue.field_id));
  }
  if (request.selected_physical_node_id == 0 ||
      request.selected_physical_node_id !=
          request.physical_dag.root_physical_node_id) {
    return refuse(Refusal("SBLR.PLAN_TREE.INVALID_HANDLE",
                          "selected limit node is not the root"));
  }

  const PhysicalNodeRecord* selected_node = nullptr;
  const PhysicalNodeRecord* input_node = nullptr;
  for (const auto& node : request.physical_dag.nodes) {
    if (node.physical_node_id == request.selected_physical_node_id) {
      selected_node = &node;
    }
  }
  if (selected_node == nullptr ||
      selected_node->node_kind != PhysicalNodeKind::kLimit ||
      selected_node->input_physical_node_ids.size() != 1) {
    return refuse(Refusal(
        "QOW-DIAG-QRY-007-SORT-LIMIT-PHYSICAL-ROUTE-V1",
        "descriptor limit requires one selected limit node"));
  }
  for (const auto& node : request.physical_dag.nodes) {
    if (node.physical_node_id ==
        selected_node->input_physical_node_ids.front()) {
      input_node = &node;
      break;
    }
  }
  if (input_node == nullptr ||
      selected_node->output_descriptor_ids !=
          input_node->output_descriptor_ids) {
    return refuse(Refusal("SBLR.PLAN_TREE.INVALID_HANDLE",
                          "limit schema does not preserve input handles"));
  }
  auto input_validation = ValidateCanonicalDescriptorBatch(
      request.input_batch, input_node->output_descriptor_ids);
  if (!input_validation.ok) return refuse(std::move(input_validation));

  result.output_batch.columns = request.input_batch.columns;
  const auto row_count = request.input_batch.rows.size();
  const auto offset = request.offset > row_count
                          ? row_count
                          : static_cast<std::size_t>(request.offset);
  const auto remaining = row_count - offset;
  const auto take = request.limit > remaining
                        ? remaining
                        : static_cast<std::size_t>(request.limit);
  result.output_batch.rows.reserve(take);
  result.output_batch.rows.insert(
      result.output_batch.rows.end(),
      request.input_batch.rows.begin() + static_cast<std::ptrdiff_t>(offset),
      request.input_batch.rows.begin() +
          static_cast<std::ptrdiff_t>(offset + take));

  auto output_validation = ValidateCanonicalDescriptorBatch(
      result.output_batch, selected_node->output_descriptor_ids);
  if (!output_validation.ok) return refuse(std::move(output_validation));
  result.diagnostic = {};
  result.selected_plan_uuid = request.physical_dag.selected_plan_uuid;
  result.executed_physical_node_id = selected_node->physical_node_id;
  result.causal_counter_id = selected_node->causal_counter_id;
  return result;
}

// QOW-SOURCE-QRY-010-V1
// Execute an already-bound physical ORDER BY node.  Every descriptor handle,
// collation authority, operand encoding, and row-pair comparison is validated
// before the first output row is materialized.
CanonicalDescriptorSortResult ExecuteCanonicalDescriptorSort(
    const CanonicalDescriptorSortRequest& request) {
  CanonicalDescriptorSortResult result;
  const auto refuse = [&](DescriptorRuntimeDiagnostic diagnostic) {
    result.diagnostic = std::move(diagnostic);
    result.output_batch = {};
    return result;
  };
  const auto order_refusal = [&](std::string detail) {
    return refuse(Refusal("QOW-DIAG-QRY-010-ORDER-REFUSAL-V1",
                          std::move(detail)));
  };

  const auto dag_validation =
      ValidateTypedPhysicalNodeDag(request.physical_dag);
  if (!dag_validation.accepted) {
    const auto& issue = dag_validation.issues.front();
    return refuse(Refusal(issue.diagnostic_id, issue.field_id));
  }
  if (request.selected_physical_node_id == 0 ||
      request.selected_physical_node_id !=
          request.physical_dag.root_physical_node_id) {
    return refuse(Refusal("SBLR.PLAN_TREE.INVALID_HANDLE",
                          "selected sort node is not the root"));
  }

  const PhysicalNodeRecord* selected_node = nullptr;
  const PhysicalNodeRecord* input_node = nullptr;
  for (const auto& node : request.physical_dag.nodes) {
    if (node.physical_node_id == request.selected_physical_node_id) {
      selected_node = &node;
    }
  }
  if (selected_node == nullptr ||
      selected_node->node_kind != PhysicalNodeKind::kSort ||
      selected_node->input_physical_node_ids.size() != 1) {
    return order_refusal("descriptor order requires one selected sort node");
  }
  for (const auto& node : request.physical_dag.nodes) {
    if (node.physical_node_id ==
        selected_node->input_physical_node_ids.front()) {
      input_node = &node;
      break;
    }
  }
  if (input_node == nullptr ||
      selected_node->output_descriptor_ids !=
          input_node->output_descriptor_ids) {
    return refuse(Refusal("SBLR.PLAN_TREE.INVALID_HANDLE",
                          "sort schema does not preserve input handles"));
  }
  auto input_validation = ValidateCanonicalDescriptorBatch(
      request.input_batch, input_node->output_descriptor_ids);
  if (!input_validation.ok) return refuse(std::move(input_validation));
  if (request.order_terms.empty() ||
      !IsCanonicalUuid(request.deterministic_tie_evidence_uuid)) {
    return order_refusal(
        "bound order terms and deterministic tie evidence are required");
  }

  for (const auto& term : request.order_terms) {
    if (term.column >= request.input_batch.columns.size()) {
      return order_refusal("order term column is outside the input schema");
    }
    const auto& column = request.input_batch.columns[term.column];
    const auto validation =
        ValidateCanonicalDescriptorOrderTerm(term, column);
    if (!validation.ok) {
      return order_refusal(validation.detail);
    }
  }

  const auto row_count = request.input_batch.rows.size();
  if (request.maximum_pair_comparisons == 0 ||
      (row_count != 0 &&
       row_count >
           std::numeric_limits<std::size_t>::max() / row_count)) {
    return order_refusal("order comparison resource bound overflowed");
  }
  const auto matrix_size = row_count * row_count;
  if (matrix_size > request.maximum_pair_comparisons) {
    return order_refusal("order comparison resource bound was exceeded");
  }

  std::vector<std::int8_t> comparisons(matrix_size, 0);
  for (std::size_t row = 0; row < row_count; ++row) {
    for (const auto& term : request.order_terms) {
      const auto& value = request.input_batch.rows[row].values[term.column];
      const auto compared =
          CompareCanonicalDescriptorOrderValues(value, value, term);
      if (!compared.diagnostic.ok) {
        return order_refusal("order operand refusal: " +
                             compared.diagnostic.detail);
      }
    }
  }
  for (std::size_t left = 0; left < row_count; ++left) {
    for (std::size_t right = left + 1; right < row_count; ++right) {
      int comparison = 0;
      for (const auto& term : request.order_terms) {
        const auto& left_value =
            request.input_batch.rows[left].values[term.column];
        const auto& right_value =
            request.input_batch.rows[right].values[term.column];
        const auto compared = CompareCanonicalDescriptorOrderValues(
            left_value, right_value, term);
        if (!compared.diagnostic.ok) {
          return order_refusal("order comparison refusal: " +
                               compared.diagnostic.detail);
        }
        comparison = compared.comparison;
        if (comparison != 0) break;
      }
      comparisons[left * row_count + right] =
          static_cast<std::int8_t>(comparison);
      comparisons[right * row_count + left] =
          static_cast<std::int8_t>(-comparison);
    }
  }

  std::vector<std::size_t> row_order(row_count);
  std::iota(row_order.begin(), row_order.end(), 0);
  std::stable_sort(row_order.begin(), row_order.end(),
                   [&](const std::size_t left, const std::size_t right) {
                     return comparisons[left * row_count + right] < 0;
                   });

  result.output_batch.columns = request.input_batch.columns;
  result.output_batch.rows.reserve(row_count);
  for (const auto row : row_order) {
    result.output_batch.rows.push_back(request.input_batch.rows[row]);
  }
  auto output_validation = ValidateCanonicalDescriptorBatch(
      result.output_batch, selected_node->output_descriptor_ids);
  if (!output_validation.ok) return refuse(std::move(output_validation));

  result.diagnostic = {};
  result.selected_plan_uuid = request.physical_dag.selected_plan_uuid;
  result.executed_physical_node_id = selected_node->physical_node_id;
  result.causal_counter_id = selected_node->causal_counter_id;
  return result;
}

}  // namespace scratchbird::engine::executor
