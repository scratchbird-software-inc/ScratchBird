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

bool DescriptorBatchCarrierIsExactDefault(const DescriptorBatch& batch) {
  const DescriptorBatch empty;
  return batch.columns.empty() &&
         batch.columns.capacity() == empty.columns.capacity() &&
         batch.rows.empty() && batch.rows.capacity() == empty.rows.capacity();
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

bool TimezoneSeedAbsent(
    const scratchbird::core::datatypes::TimezoneSeedAuthority& seed) {
  return !seed.active && seed.seed_pack_name.empty() &&
         seed.seed_pack_version.empty() && seed.content_hash.empty() &&
         seed.timezone_records == 0 &&
         seed.timezone_transition_records == 0 &&
         seed.timezone_leap_second_records == 0 &&
         seed.timezone_names.empty();
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
          right.descriptor.canonical_type_name) {
    *refusal_detail = "order operands do not share a supported scalar encoding";
    return false;
  }
  const auto canonical_state = [](const auto& value) {
    if (value.state ==
        scratchbird::engine::internal_api::EngineValueState::sql_null) {
      return value.is_null && value.encoded_value.empty() &&
             value.binary_value.empty();
    }
    return value.state ==
               scratchbird::engine::internal_api::EngineValueState::value &&
           !value.is_null;
  };
  if (!canonical_state(left) || !canonical_state(right)) {
    *refusal_detail =
        "order operand carries a malformed NULL or non-value sentinel";
    return false;
  }
  const bool carries_binary_payload =
      !left.binary_value.empty() || !right.binary_value.empty();
  if (carries_binary_payload && type_id != dt::CanonicalTypeId::binary) {
    *refusal_detail =
        "order operand carries binary payload for a non-binary type";
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
  std::string left_encoded = left.encoded_value;
  std::string right_encoded = right.encoded_value;
  if (!has_null && type_id == dt::CanonicalTypeId::binary) {
    if (!left.binary_value.empty()) {
      left_encoded.assign(
          reinterpret_cast<const char*>(left.binary_value.data()),
          left.binary_value.size());
    }
    if (!right.binary_value.empty()) {
      right_encoded.assign(
          reinterpret_cast<const char*>(right.binary_value.data()),
          right.binary_value.size());
    }
  }
  const auto timezone_profile = DescriptorField(
      left.descriptor.encoded_descriptor, "timezone_profile_id");
  bool timezone_normalized = false;
  if (!has_null && !timezone_profile.empty()) {
    const auto normalize = [&](const auto& value,
                               std::string* normalized_value) {
      dt::ReferenceTemporalWireProfileRequest request;
      request.reference_engine = "scratchbird_native";
      request.reference_type_or_family =
          value.descriptor.canonical_type_name;
      request.wire_profile = timezone_profile;
      request.encoded_value = value.encoded_value;
      request.timezone_seed = term.timezone_seed;
      const auto normalized = dt::ValidateReferenceTemporalWireProfile(
          request);
      if (!normalized.ok() || normalized.canonical_type_id != type_id) {
        *refusal_detail = normalized.diagnostic.diagnostic_code.empty()
                              ? "timezone order normalization refused"
                              : normalized.diagnostic.diagnostic_code;
        return false;
      }
      *normalized_value = normalized.normalized_value;
      return true;
    };
    if (!normalize(left, &left_encoded) ||
        !normalize(right, &right_encoded)) {
      return false;
    }
    timezone_normalized = true;
  }
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
      request.left.encoded_value = left_encoded;
      request.right.type_id = type_id;
      request.right.encoded_value = right_encoded;
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
      if (timezone_normalized) {
        *comparison = left_encoded < right_encoded
                          ? -1
                          : (left_encoded > right_encoded ? 1 : 0);
        *comparison = *comparison < 0 ? -1 : (*comparison > 0 ? 1 : 0);
        if (term.direction ==
            CanonicalDescriptorOrderDirection::descending) {
          *comparison = -*comparison;
        }
        return true;
      }
      const auto validate = [type_id](const std::string& encoded_value) {
        dt::DatatypeCastRequest request;
        request.value.type_id = type_id;
        request.value.encoded_value = encoded_value;
        request.target_type_id = type_id;
        request.explicit_cast = true;
        return dt::CastDatatypeValue(request);
      };
      const auto left_checked = validate(left_encoded);
      const auto right_checked = validate(right_encoded);
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
    if (term.timezone_epoch != 0 ||
        !TimezoneSeedAbsent(term.timezone_seed)) {
      return Refusal("QOW-DIAG-QRY-010-ORDER-REFUSAL-V1",
                     "character order term carries timezone authority");
    }
  } else if ((type_id == dt::CanonicalTypeId::time ||
              type_id == dt::CanonicalTypeId::timestamp) &&
             !DescriptorField(column.descriptor.encoded_descriptor,
                              "timezone_profile_id")
                  .empty()) {
    const auto timezone_profile = DescriptorField(
        column.descriptor.encoded_descriptor, "timezone_profile_id");
    const bool profile_matches_type =
        (type_id == dt::CanonicalTypeId::timestamp &&
         timezone_profile == "timestamp_timezone_profile") ||
        (type_id == dt::CanonicalTypeId::time &&
         timezone_profile == "time_timezone_profile");
    const auto& seed = term.timezone_seed;
    if (!profile_matches_type || !term.collation_uuid.empty() ||
        term.resource_epoch == 0 || term.timezone_epoch == 0 ||
        term.collation_epoch != 0 || !TextSeedAbsent(term.text_seed) ||
        !seed.active || seed.seed_pack_name.empty() ||
        seed.seed_pack_version.empty() || seed.content_hash.empty() ||
        seed.timezone_records == 0 || seed.timezone_names.empty()) {
      return Refusal(
          "QOW-DIAG-QRY-010-ORDER-REFUSAL-V1",
          "temporal order lacks bound timezone resource authority");
    }
  } else if (!term.collation_uuid.empty() || term.resource_epoch != 0 ||
             term.collation_epoch != 0 || !TextSeedAbsent(term.text_seed) ||
             term.timezone_epoch != 0 ||
             !TimezoneSeedAbsent(term.timezone_seed)) {
    return Refusal("QOW-DIAG-QRY-010-ORDER-REFUSAL-V1",
                   "order term carries authority for a different datatype");
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
namespace {
CanonicalDescriptorLimitResult ExecuteCanonicalDescriptorLimitBound(
    const CanonicalDescriptorLimitRequest& request,
    const TypedPhysicalNodeDag& execution_dag,
    const DescriptorBatch& execution_input_batch,
    const bool borrowed_execution_carriers) {
  CanonicalDescriptorLimitResult result;
  const auto refuse = [&](DescriptorRuntimeDiagnostic diagnostic) {
    result.diagnostic = std::move(diagnostic);
    result.output_batch = {};
    return result;
  };
  if (borrowed_execution_carriers &&
      (!TypedPhysicalNodeDagCarrierIsExactDefault(request.physical_dag) ||
       !DescriptorBatchCarrierIsExactDefault(request.input_batch))) {
    return refuse(Refusal(
        "QOW-DIAG-QRY-007-SORT-LIMIT-PHYSICAL-ROUTE-V1",
        "descriptor limit request carries conflicting owned execution carriers"));
  }
  const auto authority_validation = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, execution_dag);
  if (!authority_validation.ok) {
    return refuse(authority_validation);
  }

  const PhysicalNodeRecord* selected_node = nullptr;
  const PhysicalNodeRecord* input_node = nullptr;
  for (const auto& node : execution_dag.nodes) {
    if (node.physical_node_id == request.selected_physical_node_id) {
      selected_node = &node;
    }
  }
  if (request.selected_physical_node_id == 0 ||
      request.selected_physical_node_id !=
          execution_dag.root_physical_node_id ||
      selected_node == nullptr ||
      selected_node->node_kind != PhysicalNodeKind::kLimit ||
      selected_node->input_physical_node_ids.size() != 1) {
    return refuse(Refusal(
        "QOW-DIAG-QRY-007-SORT-LIMIT-PHYSICAL-ROUTE-V1",
        "descriptor limit requires one selected root limit node"));
  }
  for (const auto& node : execution_dag.nodes) {
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
      execution_input_batch, input_node->output_descriptor_ids);
  if (!input_validation.ok) return refuse(std::move(input_validation));

  result.output_batch.columns = execution_input_batch.columns;
  const auto row_count = execution_input_batch.rows.size();
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
      execution_input_batch.rows.begin() +
          static_cast<std::ptrdiff_t>(offset),
      execution_input_batch.rows.begin() +
          static_cast<std::ptrdiff_t>(offset + take));

  auto output_validation = ValidateCanonicalDescriptorBatch(
      result.output_batch, selected_node->output_descriptor_ids);
  if (!output_validation.ok) return refuse(std::move(output_validation));
  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, execution_dag);
  if (!result_authority.ok) return refuse(result_authority);
  result.diagnostic = {};
  result.selected_plan_uuid = execution_dag.selected_plan_uuid;
  result.executed_physical_node_id = selected_node->physical_node_id;
  result.causal_counter_id = selected_node->causal_counter_id;
  result.mga_statement_context = request.mga_authority.statement_context;
  return result;
}
}  // namespace

CanonicalDescriptorLimitResult ExecuteCanonicalDescriptorLimit(
    const CanonicalDescriptorLimitRequest& request) {
  return ExecuteCanonicalDescriptorLimitBound(
      request, request.physical_dag, request.input_batch, false);
}

CanonicalDescriptorLimitResult ExecuteCanonicalDescriptorLimit(
    const CanonicalDescriptorLimitRequest& request,
    const TypedPhysicalNodeDag& borrowed_execution_dag,
    const DescriptorBatch& borrowed_input_batch) {
  return ExecuteCanonicalDescriptorLimitBound(
      request, borrowed_execution_dag, borrowed_input_batch, true);
}

// QOW-SOURCE-QRY-010-DISTINCT-COMPOSITION-V1
// Query DISTINCT is the optimizer's distinct-aggregate physical operation.
// Equality is evaluated over every projected descriptor using the same typed
// comparator, SQL NULL equality, and catalog-bound collation authority as
// canonical ordering. Every value validates before duplicate representatives
// are materialized, so a later duplicate cannot hide malformed input.
namespace {
CanonicalDescriptorDistinctResult ExecuteCanonicalDescriptorDistinctBound(
    const CanonicalDescriptorDistinctRequest& request,
    const TypedPhysicalNodeDag& execution_dag,
    const DescriptorBatch& execution_input_batch,
    const bool borrowed_execution_carriers) {
  CanonicalDescriptorDistinctResult result;
  const auto refuse = [&](DescriptorRuntimeDiagnostic diagnostic) {
    result = {};
    result.diagnostic = std::move(diagnostic);
    return result;
  };
  const auto distinct_refusal = [&](std::string detail) {
    return refuse(Refusal("QOW-DIAG-QRY-010-DISTINCT-REFUSAL-V1",
                          std::move(detail)));
  };

  if (borrowed_execution_carriers &&
      (!TypedPhysicalNodeDagCarrierIsExactDefault(request.physical_dag) ||
       !DescriptorBatchCarrierIsExactDefault(request.input_batch))) {
    return distinct_refusal(
        "query DISTINCT request carries conflicting owned execution carriers");
  }

  const auto authority_validation = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, execution_dag);
  if (!authority_validation.ok) return refuse(authority_validation);

  const PhysicalNodeRecord* selected_node = nullptr;
  const PhysicalNodeRecord* input_node = nullptr;
  for (const auto& node : execution_dag.nodes) {
    if (node.physical_node_id == request.selected_physical_node_id) {
      selected_node = &node;
    }
  }
  if (request.selected_physical_node_id == 0 ||
      request.selected_physical_node_id !=
          execution_dag.root_physical_node_id ||
      selected_node == nullptr ||
      selected_node->node_kind != PhysicalNodeKind::kAggregate ||
      selected_node->implementation_id !=
          "aggregate.query-distinct.typed.v1" ||
      selected_node->input_physical_node_ids.size() != 1) {
    return distinct_refusal(
        "query DISTINCT requires one selected root distinct-aggregate node");
  }
  for (const auto& node : execution_dag.nodes) {
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
                          "query DISTINCT does not preserve its input schema"));
  }
  auto validation = ValidateCanonicalDescriptorBatch(
      execution_input_batch, input_node->output_descriptor_ids);
  if (!validation.ok) return refuse(std::move(validation));
  if (request.equality_terms.size() != execution_input_batch.columns.size() ||
      request.equality_terms.empty() ||
      request.maximum_value_comparisons == 0) {
    return distinct_refusal(
        "query DISTINCT requires one bounded equality term per output column");
  }

  std::vector<bool> covered(execution_input_batch.columns.size(), false);
  for (const auto& term : request.equality_terms) {
    if (term.column >= execution_input_batch.columns.size() ||
        covered[term.column] ||
        term.direction != CanonicalDescriptorOrderDirection::ascending ||
        term.null_placement != CanonicalDescriptorNullPlacement::first) {
      return distinct_refusal(
          "query DISTINCT equality term coverage or canonical form is invalid");
    }
    const auto term_validation = ValidateCanonicalDescriptorOrderTerm(
        term, execution_input_batch.columns[term.column]);
    if (!term_validation.ok) return distinct_refusal(term_validation.detail);
    covered[term.column] = true;
  }

  std::size_t comparison_count = 0;
  const auto equal_rows = [&](const DescriptorTuple& left,
                              const DescriptorTuple& right,
                              bool* equal,
                              std::string* detail) {
    if (equal == nullptr || detail == nullptr) return false;
    *equal = true;
    for (const auto& term : request.equality_terms) {
      if (comparison_count >= request.maximum_value_comparisons) {
        *detail = "query DISTINCT value comparison bound was exceeded";
        return false;
      }
      ++comparison_count;
      const auto compared = CompareCanonicalDescriptorOrderValues(
          left.values[term.column], right.values[term.column], term);
      if (!compared.diagnostic.ok) {
        *detail = compared.diagnostic.detail;
        return false;
      }
      if (compared.comparison != 0) {
        *equal = false;
        return true;
      }
    }
    return true;
  };

  // Validate every typed value independently before deciding membership.
  for (const auto& row : execution_input_batch.rows) {
    bool equal = false;
    std::string detail;
    if (!equal_rows(row, row, &equal, &detail) || !equal) {
      return distinct_refusal(detail.empty()
                                  ? "query DISTINCT self comparison failed"
                                  : std::move(detail));
    }
  }

  std::vector<std::size_t> representative_rows;
  representative_rows.reserve(execution_input_batch.rows.size());
  std::size_t eliminated = 0;
  for (std::size_t row = 0; row < execution_input_batch.rows.size(); ++row) {
    bool duplicate = false;
    for (const auto representative : representative_rows) {
      bool equal = false;
      std::string detail;
      if (!equal_rows(execution_input_batch.rows[row],
                      execution_input_batch.rows[representative], &equal,
                      &detail)) {
        return distinct_refusal(std::move(detail));
      }
      if (equal) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) {
      ++eliminated;
    } else {
      representative_rows.push_back(row);
    }
  }

  DescriptorBatch output;
  output.columns = execution_input_batch.columns;
  output.rows.reserve(representative_rows.size());
  for (const auto row : representative_rows) {
    output.rows.push_back(execution_input_batch.rows[row]);
  }
  validation = ValidateCanonicalDescriptorBatch(
      output, selected_node->output_descriptor_ids);
  if (!validation.ok) return refuse(std::move(validation));
  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, execution_dag);
  if (!result_authority.ok) return refuse(result_authority);

  result.diagnostic = {};
  result.output_batch = std::move(output);
  result.eliminated_duplicate_row_count = eliminated;
  result.value_comparison_count = comparison_count;
  result.selected_plan_uuid = execution_dag.selected_plan_uuid;
  result.executed_physical_node_id = selected_node->physical_node_id;
  result.causal_counter_id = selected_node->causal_counter_id;
  result.mga_statement_context = request.mga_authority.statement_context;
  return result;
}
}  // namespace

CanonicalDescriptorDistinctResult ExecuteCanonicalDescriptorDistinct(
    const CanonicalDescriptorDistinctRequest& request) {
  return ExecuteCanonicalDescriptorDistinctBound(
      request, request.physical_dag, request.input_batch, false);
}

CanonicalDescriptorDistinctResult ExecuteCanonicalDescriptorDistinct(
    const CanonicalDescriptorDistinctRequest& request,
    const TypedPhysicalNodeDag& borrowed_execution_dag,
    const DescriptorBatch& borrowed_input_batch) {
  return ExecuteCanonicalDescriptorDistinctBound(
      request, borrowed_execution_dag, borrowed_input_batch, true);
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

  const TypedPhysicalNodeDag* physical_dag = &request.physical_dag;
  const CanonicalExecutionMgaAuthority* mga_authority =
      &request.mga_authority;
  std::uint64_t selected_physical_node_id =
      request.selected_physical_node_id;
  std::size_t maximum_pair_comparisons =
      request.maximum_pair_comparisons;
  const DescriptorBatch* input_batch = &request.input_batch;
  const DescriptorBatch* order_batch = &request.input_batch;
  const std::vector<CanonicalDescriptorOrderTerm>* order_terms =
      &request.order_terms;
  const std::string* deterministic_tie_evidence_uuid =
      &request.deterministic_tie_evidence_uuid;
  if (request.order_key_receipt != nullptr) {
    const auto& receipt = *request.order_key_receipt;
    if (!request.physical_dag.nodes.empty() ||
        !request.physical_dag.selected_plan_uuid.empty() ||
        request.selected_physical_node_id != 0 ||
        !request.input_batch.columns.empty() ||
        !request.input_batch.rows.empty() || !request.order_terms.empty() ||
        !request.deterministic_tie_evidence_uuid.empty() ||
        !receipt.exact_current_revalidated_before_issue_ ||
        receipt.physical_dag_.nodes.empty() ||
        receipt.selected_physical_node_id_ == 0 ||
        receipt.maximum_pair_comparisons_ == 0 ||
        receipt.maximum_order_key_batch_bytes_ == 0 ||
        receipt.ordering_property_uuid_.empty() ||
        request.mga_authority.origin != CanonicalMgaAuthorityOrigin::kMissing ||
        static_cast<bool>(request.mga_authority.resolve_current)) {
      return order_refusal(
          "expression order-key receipt does not bind this execution");
    }
    physical_dag = &receipt.physical_dag_;
    mga_authority = &receipt.mga_authority_;
    selected_physical_node_id = receipt.selected_physical_node_id_;
    maximum_pair_comparisons = receipt.maximum_pair_comparisons_;
    input_batch = &receipt.input_batch_;
    order_batch = &receipt.order_key_batch_;
    order_terms = &receipt.order_terms_;
    deterministic_tie_evidence_uuid =
        &receipt.deterministic_tie_evidence_uuid_;
  }

  const auto authority_validation = RevalidateCanonicalExecutionMgaAuthority(
      *mga_authority, *physical_dag);
  if (!authority_validation.ok) {
    return refuse(authority_validation);
  }

  const PhysicalNodeRecord* selected_node = nullptr;
  const PhysicalNodeRecord* input_node = nullptr;
  for (const auto& node : physical_dag->nodes) {
    if (node.physical_node_id == selected_physical_node_id) {
      selected_node = &node;
    }
  }
  if (selected_physical_node_id == 0 ||
      selected_physical_node_id != physical_dag->root_physical_node_id ||
      selected_node == nullptr ||
      selected_node->node_kind != PhysicalNodeKind::kSort ||
      selected_node->input_physical_node_ids.size() != 1) {
    return order_refusal(
        "descriptor order requires one selected root sort node");
  }
  for (const auto& node : physical_dag->nodes) {
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
      *input_batch, input_node->output_descriptor_ids);
  if (!input_validation.ok) return refuse(std::move(input_validation));
  if (request.order_key_receipt != nullptr) {
    if (order_batch->rows.size() != input_batch->rows.size()) {
      return order_refusal(
          "materialized order-key cardinality differs from the input");
    }
    std::vector<std::uint32_t> order_descriptor_ids;
    order_descriptor_ids.reserve(order_batch->columns.size());
    for (const auto& column : order_batch->columns) {
      order_descriptor_ids.push_back(column.descriptor_id);
    }
    auto order_validation = ValidateCanonicalDescriptorBatch(
        *order_batch, order_descriptor_ids);
    if (!order_validation.ok) return refuse(std::move(order_validation));
  }
  if (order_terms->empty() ||
      !IsCanonicalUuid(*deterministic_tie_evidence_uuid) ||
      *deterministic_tie_evidence_uuid ==
          "00000000-0000-0000-0000-000000000000") {
    return order_refusal(
        "bound order terms and deterministic tie evidence are required");
  }

  for (const auto& term : *order_terms) {
    if (term.column >= order_batch->columns.size()) {
      return order_refusal("order term column is outside the input schema");
    }
    const auto& column = order_batch->columns[term.column];
    const auto validation =
        ValidateCanonicalDescriptorOrderTerm(term, column);
    if (!validation.ok) {
      return order_refusal(validation.detail);
    }
  }

  const auto row_count = input_batch->rows.size();
  if (maximum_pair_comparisons == 0 ||
      (row_count != 0 &&
       row_count >
           std::numeric_limits<std::size_t>::max() / row_count)) {
    return order_refusal("order comparison resource bound overflowed");
  }
  const auto matrix_size = row_count * row_count;
  if (matrix_size > maximum_pair_comparisons) {
    return order_refusal("order comparison resource bound was exceeded");
  }

  std::vector<std::int8_t> comparisons(matrix_size, 0);
  for (std::size_t row = 0; row < row_count; ++row) {
    for (const auto& term : *order_terms) {
      const auto& value = order_batch->rows[row].values[term.column];
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
      for (const auto& term : *order_terms) {
        const auto& left_value =
            order_batch->rows[left].values[term.column];
        const auto& right_value =
            order_batch->rows[right].values[term.column];
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

  result.output_batch.columns = input_batch->columns;
  result.output_batch.rows.reserve(row_count);
  for (const auto row : row_order) {
    result.output_batch.rows.push_back(input_batch->rows[row]);
  }
  auto output_validation = ValidateCanonicalDescriptorBatch(
      result.output_batch, selected_node->output_descriptor_ids);
  if (!output_validation.ok) return refuse(std::move(output_validation));
  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      *mga_authority, *physical_dag);
  if (!result_authority.ok) return refuse(result_authority);

  result.diagnostic = {};
  result.selected_plan_uuid = physical_dag->selected_plan_uuid;
  result.executed_physical_node_id = selected_node->physical_node_id;
  result.causal_counter_id = selected_node->causal_counter_id;
  result.mga_statement_context = mga_authority->statement_context;
  return result;
}

}  // namespace scratchbird::engine::executor
