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

bool DescriptorOrderTermsCarrierIsExactDefault(
    const std::vector<CanonicalDescriptorOrderTerm>& order_terms) {
  const std::vector<CanonicalDescriptorOrderTerm> empty;
  return order_terms.empty() && order_terms.capacity() == empty.capacity();
}

bool StringCarrierIsExactDefault(const std::string& value) {
  const std::string empty;
  return value.empty() && value.capacity() == empty.capacity();
}

bool CanonicalExecutionMgaAuthorityCarrierIsExactDefault(
    const CanonicalExecutionMgaAuthority& authority) {
  const CanonicalExecutionMgaAuthority empty;
  const auto exact_empty_string = [](const std::string& value,
                                     const std::string& baseline) {
    return value.empty() && value.capacity() == baseline.capacity();
  };
  const auto& context = authority.statement_context;
  const auto& empty_context = empty.statement_context;
  return authority.origin == empty.origin && !authority.resolve_current &&
         PhysicalMgaStatementContextEqual(context, empty_context) &&
         exact_empty_string(context.statement_uuid,
                            empty_context.statement_uuid) &&
         exact_empty_string(context.owning_transaction_uuid,
                            empty_context.owning_transaction_uuid) &&
         exact_empty_string(context.statement_snapshot_uuid,
                            empty_context.statement_snapshot_uuid) &&
         exact_empty_string(context.statement_metadata_snapshot_uuid,
                            empty_context.statement_metadata_snapshot_uuid) &&
         exact_empty_string(context.snapshot_kind,
                            empty_context.snapshot_kind) &&
         exact_empty_string(context.statement_timestamp,
                            empty_context.statement_timestamp) &&
         context.active_excluded_local_transaction_ids.empty() &&
         context.active_excluded_local_transaction_ids.capacity() ==
             empty_context.active_excluded_local_transaction_ids.capacity() &&
         context.in_doubt_excluded_local_transaction_ids.empty() &&
         context.in_doubt_excluded_local_transaction_ids.capacity() ==
             empty_context.in_doubt_excluded_local_transaction_ids.capacity();
}

bool SortReceiptRequestCarriersAreExactDefault(
    const CanonicalDescriptorSortRequest& request) {
  const CanonicalDescriptorSortRequest empty;
  return TypedPhysicalNodeDagCarrierIsExactDefault(request.physical_dag) &&
         request.selected_physical_node_id ==
             empty.selected_physical_node_id &&
         DescriptorBatchCarrierIsExactDefault(request.input_batch) &&
         DescriptorOrderTermsCarrierIsExactDefault(request.order_terms) &&
         StringCarrierIsExactDefault(
             request.deterministic_tie_evidence_uuid) &&
         request.maximum_pair_comparisons ==
             empty.maximum_pair_comparisons &&
         CanonicalExecutionMgaAuthorityCarrierIsExactDefault(
             request.mga_authority);
}

bool CanonicalDescriptorBatchMemoryBytes(const DescriptorBatch& batch,
                                         std::uint64_t* bytes) {
  if (bytes == nullptr) return false;
  *bytes = 1;
  for (const auto& row : batch.rows) {
    for (const auto& value : row.values) {
      if (value.encoded_value.size() >
          std::numeric_limits<std::uint64_t>::max() - *bytes) {
        return false;
      }
      *bytes += value.encoded_value.size();
      if (value.binary_value.size() >
          std::numeric_limits<std::uint64_t>::max() - *bytes) {
        return false;
      }
      *bytes += value.binary_value.size();
    }
  }
  return true;
}

bool CanonicalDescriptorBatchRangeMemoryBytes(
    const DescriptorBatch& batch,
    const std::size_t first_row,
    const std::size_t row_count,
    std::uint64_t* bytes) {
  if (bytes == nullptr || first_row > batch.rows.size() ||
      row_count > batch.rows.size() - first_row) {
    return false;
  }
  *bytes = 1;
  for (std::size_t row_index = first_row;
       row_index < first_row + row_count; ++row_index) {
    for (const auto& value : batch.rows[row_index].values) {
      if (value.encoded_value.size() >
          std::numeric_limits<std::uint64_t>::max() - *bytes) {
        return false;
      }
      *bytes += value.encoded_value.size();
      if (value.binary_value.size() >
          std::numeric_limits<std::uint64_t>::max() - *bytes) {
        return false;
      }
      *bytes += value.binary_value.size();
    }
  }
  return true;
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
enum class DescriptorLimitExecutionRoute : std::uint8_t {
  limit = 1,
  fetch_first_rows_only,
};

constexpr std::string_view DescriptorLimitImplementationId(
    const DescriptorLimitExecutionRoute route) {
  switch (route) {
    case DescriptorLimitExecutionRoute::limit:
      return "limit.typed.v1";
    case DescriptorLimitExecutionRoute::fetch_first_rows_only:
      return "fetch.native.rows-only.v1";
  }
  return {};
}

CanonicalDescriptorLimitResult ExecuteCanonicalDescriptorLimitBound(
    const CanonicalDescriptorLimitRequest& request,
    const TypedPhysicalNodeDag& execution_dag,
    const DescriptorBatch& execution_input_batch,
    const DescriptorLimitExecutionRoute execution_route,
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
  const auto expected_implementation_id =
      DescriptorLimitImplementationId(execution_route);
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
      selected_node->implementation_id != expected_implementation_id ||
      selected_node->input_physical_node_ids.size() != 1) {
    return refuse(Refusal(
        "QOW-DIAG-QRY-007-SORT-LIMIT-PHYSICAL-ROUTE-V1",
        "descriptor LIMIT/FETCH requires its selected root canonical "
        "implementation"));
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

  const auto row_count = execution_input_batch.rows.size();
  const auto offset = request.offset > row_count
                          ? row_count
                          : static_cast<std::size_t>(request.offset);
  const auto remaining = row_count - offset;
  const auto take = request.limit > remaining
                        ? remaining
                        : static_cast<std::size_t>(request.limit);

  std::uint64_t input_memory_bytes = 0;
  std::uint64_t output_memory_bytes = 0;
  if (!CanonicalDescriptorBatchMemoryBytes(execution_input_batch,
                                           &input_memory_bytes) ||
      !CanonicalDescriptorBatchRangeMemoryBytes(
          execution_input_batch, offset, take, &output_memory_bytes) ||
      selected_node->memory_bytes_required == 0 ||
      selected_node->memory_bytes_required > execution_dag.memory_budget_bytes ||
      selected_node->memory_bytes_required >
          static_cast<std::uint64_t>(
              std::numeric_limits<std::size_t>::max())) {
    return refuse(Refusal(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        "limit memory grant or runtime payload accounting is invalid"));
  }
  auto remaining_memory_bytes = selected_node->memory_bytes_required;
  const auto charge = [&](const std::uint64_t bytes) {
    if (bytes > remaining_memory_bytes) return false;
    remaining_memory_bytes -= bytes;
    return true;
  };
  if (!charge(input_memory_bytes) || !charge(output_memory_bytes)) {
    return refuse(Refusal(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        "limit runtime materialization exceeds the selected node memory grant"));
  }

  result.output_batch.columns = execution_input_batch.columns;
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
      request, request.physical_dag, request.input_batch,
      DescriptorLimitExecutionRoute::limit, false);
}

CanonicalDescriptorLimitResult ExecuteCanonicalDescriptorLimit(
    const CanonicalDescriptorLimitRequest& request,
    const TypedPhysicalNodeDag& borrowed_execution_dag,
    const DescriptorBatch& borrowed_input_batch) {
  return ExecuteCanonicalDescriptorLimitBound(
      request, borrowed_execution_dag, borrowed_input_batch,
      DescriptorLimitExecutionRoute::limit, true);
}

// QOW-SOURCE-QRY-010-FETCH-TOP-PROFILE-V1
// The native SBSQL development profile admits only FETCH FIRST <bound count>
// ROWS ONLY.  WITH TIES and donor TOP variants stay explicit refusals rather
// than silently degrading to an ordinary limit.
namespace {
CanonicalDescriptorFetchProfileResult
ExecuteCanonicalDescriptorFetchProfileBound(
    const CanonicalDescriptorFetchProfileRequest& request,
    const TypedPhysicalNodeDag& execution_dag,
    const DescriptorBatch& execution_input_batch,
    const bool borrowed_execution_carriers) {
  CanonicalDescriptorFetchProfileResult result;
  if (borrowed_execution_carriers &&
      (!TypedPhysicalNodeDagCarrierIsExactDefault(request.physical_dag) ||
       !DescriptorBatchCarrierIsExactDefault(request.input_batch))) {
    result.diagnostic.ok = false;
    result.diagnostic.diagnostic_code =
        "QOW-DIAG-QRY-010-FETCH-TOP-PROFILE-REFUSAL-V1";
    result.diagnostic.detail =
        "FETCH request carries conflicting owned execution carriers";
    return result;
  }
  const auto entry_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, execution_dag);
  if (!entry_authority.ok) {
    result.diagnostic = entry_authority;
    return result;
  }
  if (request.form !=
          CanonicalFetchTopProfileForm::fetch_first_rows_only ||
      !request.row_count_is_bound) {
    result.diagnostic.ok = false;
    result.diagnostic.diagnostic_code =
        "QOW-DIAG-QRY-010-FETCH-TOP-PROFILE-REFUSAL-V1";
    result.diagnostic.detail =
        "only bound native FETCH FIRST ROWS ONLY is admitted";
    return result;
  }

  CanonicalDescriptorLimitRequest limit_request;
  limit_request.selected_physical_node_id =
      request.selected_physical_node_id;
  limit_request.limit = request.row_count;
  limit_request.offset = request.offset;
  limit_request.mga_authority = request.mga_authority;
  auto limited = ExecuteCanonicalDescriptorLimitBound(
      limit_request, execution_dag, execution_input_batch,
      DescriptorLimitExecutionRoute::fetch_first_rows_only, true);
  if (limited.diagnostic.ok &&
      !PhysicalMgaStatementContextEqual(
          limited.mga_statement_context,
          request.mga_authority.statement_context)) {
    limited.diagnostic.ok = false;
    limited.diagnostic.diagnostic_code =
        "QOW-DIAG-QRY-010-FETCH-TOP-MGA-V1";
    limited.diagnostic.detail =
        "FETCH nested limit returned a different MGA statement context";
  }
  if (limited.diagnostic.ok) {
    const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
        request.mga_authority, execution_dag);
    if (!result_authority.ok) limited.diagnostic = result_authority;
  }
  if (!limited.diagnostic.ok) {
    result.diagnostic = std::move(limited.diagnostic);
    return result;
  }
  result.diagnostic = std::move(limited.diagnostic);
  result.output_batch = std::move(limited.output_batch);
  result.selected_plan_uuid = std::move(limited.selected_plan_uuid);
  result.executed_physical_node_id = limited.executed_physical_node_id;
  result.causal_counter_id = limited.causal_counter_id;
  if (result.diagnostic.ok) {
    result.mga_statement_context = request.mga_authority.statement_context;
  }
  return result;
}
}  // namespace

CanonicalDescriptorFetchProfileResult ExecuteCanonicalDescriptorFetchProfile(
    const CanonicalDescriptorFetchProfileRequest& request) {
  return ExecuteCanonicalDescriptorFetchProfileBound(
      request, request.physical_dag, request.input_batch, false);
}

CanonicalDescriptorFetchProfileResult ExecuteCanonicalDescriptorFetchProfile(
    const CanonicalDescriptorFetchProfileRequest& request,
    const TypedPhysicalNodeDag& borrowed_execution_dag,
    const DescriptorBatch& borrowed_input_batch) {
  return ExecuteCanonicalDescriptorFetchProfileBound(
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

  std::uint64_t input_memory_bytes = 0;
  if (!CanonicalDescriptorBatchMemoryBytes(execution_input_batch,
                                           &input_memory_bytes) ||
      selected_node->memory_bytes_required == 0 ||
      selected_node->memory_bytes_required > execution_dag.memory_budget_bytes ||
      selected_node->memory_bytes_required >
          static_cast<std::uint64_t>(
              std::numeric_limits<std::size_t>::max()) ||
      execution_input_batch.columns.size() >
          std::numeric_limits<std::uint64_t>::max() /
              sizeof(std::uint8_t) ||
      execution_input_batch.rows.size() >
          std::numeric_limits<std::uint64_t>::max() /
              sizeof(std::size_t)) {
    return refuse(Refusal(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        "query DISTINCT memory grant or runtime payload accounting is invalid"));
  }
  auto remaining_memory_bytes = selected_node->memory_bytes_required;
  const auto charge = [&](const std::uint64_t bytes) {
    if (bytes > remaining_memory_bytes) return false;
    remaining_memory_bytes -= bytes;
    return true;
  };
  const auto coverage_memory_bytes =
      static_cast<std::uint64_t>(execution_input_batch.columns.size()) *
      sizeof(std::uint8_t);
  const auto representative_memory_bytes =
      static_cast<std::uint64_t>(execution_input_batch.rows.size()) *
      sizeof(std::size_t);
  if (!charge(input_memory_bytes) || !charge(coverage_memory_bytes) ||
      !charge(representative_memory_bytes)) {
    return refuse(Refusal(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        "query DISTINCT runtime state exceeds the selected node memory grant"));
  }

  // A byte per descriptor makes the admitted coverage carrier deterministic;
  // vector<bool> has implementation-defined packed storage and cannot support
  // an exact producer-memory receipt.
  std::vector<std::uint8_t> covered(execution_input_batch.columns.size(), 0);
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

  std::vector<std::size_t> representative_rows(
      execution_input_batch.rows.size(), 0);
  std::size_t representative_count = 0;
  std::size_t eliminated = 0;
  for (std::size_t row = 0; row < execution_input_batch.rows.size(); ++row) {
    bool duplicate = false;
    for (std::size_t representative_index = 0;
         representative_index < representative_count;
         ++representative_index) {
      const auto representative =
          representative_rows[representative_index];
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
      representative_rows[representative_count++] = row;
    }
  }

  DescriptorBatch output;
  output.columns = execution_input_batch.columns;
  std::uint64_t output_memory_bytes = 1;
  for (std::size_t representative_index = 0;
       representative_index < representative_count;
       ++representative_index) {
    const auto row = representative_rows[representative_index];
    for (const auto& value : execution_input_batch.rows[row].values) {
      if (value.encoded_value.size() >
              std::numeric_limits<std::uint64_t>::max() -
                  output_memory_bytes) {
        return refuse(Refusal(
            "SBLR.PLAN_TREE.RESOURCE_LIMIT",
            "query DISTINCT output payload accounting overflowed"));
      }
      output_memory_bytes += value.encoded_value.size();
      if (value.binary_value.size() >
          std::numeric_limits<std::uint64_t>::max() -
              output_memory_bytes) {
        return refuse(Refusal(
            "SBLR.PLAN_TREE.RESOURCE_LIMIT",
            "query DISTINCT output payload accounting overflowed"));
      }
      output_memory_bytes += value.binary_value.size();
    }
  }
  if (!charge(output_memory_bytes)) {
    return refuse(Refusal(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        "query DISTINCT output exceeds the selected node memory grant"));
  }
  output.rows.reserve(representative_count);
  for (std::size_t representative_index = 0;
       representative_index < representative_count;
       ++representative_index) {
    const auto row = representative_rows[representative_index];
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
namespace {
CanonicalDescriptorSortResult ExecuteCanonicalDescriptorSortBound(
    const TypedPhysicalNodeDag& execution_dag,
    const CanonicalExecutionMgaAuthority& execution_mga_authority,
    const std::uint64_t selected_physical_node_id,
    const std::size_t maximum_pair_comparisons,
    const DescriptorBatch& execution_input_batch,
    const DescriptorBatch& execution_order_batch,
    const std::vector<CanonicalDescriptorOrderTerm>& execution_order_terms,
    const std::string& execution_deterministic_tie_evidence_uuid,
    const bool separate_order_key_batch) {
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

  const TypedPhysicalNodeDag* physical_dag = &execution_dag;
  const CanonicalExecutionMgaAuthority* mga_authority =
      &execution_mga_authority;
  const DescriptorBatch* input_batch = &execution_input_batch;
  const DescriptorBatch* order_batch = &execution_order_batch;
  const std::vector<CanonicalDescriptorOrderTerm>* order_terms =
      &execution_order_terms;
  const std::string* deterministic_tie_evidence_uuid =
      &execution_deterministic_tie_evidence_uuid;

  const auto authority_validation = RevalidateCanonicalExecutionMgaAuthority(
      *mga_authority, *physical_dag);
  if (!authority_validation.ok) {
    return refuse(authority_validation);
  }

  const PhysicalNodeRecord* selected_node = nullptr;
  const PhysicalNodeRecord* input_node = nullptr;
  const std::string_view expected_implementation_id =
      separate_order_key_batch ? "sort.typed.expression-row.v1"
                               : "sort.typed.terms.v1";
  for (const auto& node : physical_dag->nodes) {
    if (node.physical_node_id == selected_physical_node_id) {
      selected_node = &node;
    }
  }
  if (selected_physical_node_id == 0 ||
      selected_physical_node_id != physical_dag->root_physical_node_id ||
      selected_node == nullptr ||
      selected_node->node_kind != PhysicalNodeKind::kSort ||
      selected_node->implementation_id != expected_implementation_id ||
      selected_node->input_physical_node_ids.size() != 1) {
    return order_refusal(
        "descriptor order requires one selected root canonical sort "
        "implementation");
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
  if (separate_order_key_batch) {
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

  std::uint64_t input_memory_bytes = 0;
  std::uint64_t order_memory_bytes = 0;
  if (!CanonicalDescriptorBatchMemoryBytes(*input_batch,
                                           &input_memory_bytes) ||
      (separate_order_key_batch &&
       !CanonicalDescriptorBatchMemoryBytes(*order_batch,
                                            &order_memory_bytes)) ||
      selected_node->memory_bytes_required == 0 ||
      selected_node->memory_bytes_required > physical_dag->memory_budget_bytes ||
      selected_node->memory_bytes_required >
          static_cast<std::uint64_t>(
              std::numeric_limits<std::size_t>::max()) ||
      row_count > std::numeric_limits<std::uint64_t>::max() /
                      sizeof(std::size_t)) {
    return refuse(Refusal(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        "sort memory grant or runtime payload accounting is invalid"));
  }
  auto remaining_memory_bytes = selected_node->memory_bytes_required;
  const auto charge = [&](const std::uint64_t bytes) {
    if (bytes > remaining_memory_bytes) return false;
    remaining_memory_bytes -= bytes;
    return true;
  };
  const auto matrix_memory_bytes = static_cast<std::uint64_t>(matrix_size);
  const auto row_order_memory_bytes =
      static_cast<std::uint64_t>(row_count) * sizeof(std::size_t);
  if (!charge(input_memory_bytes) || !charge(input_memory_bytes) ||
      (separate_order_key_batch && !charge(order_memory_bytes)) ||
      !charge(matrix_memory_bytes) || !charge(row_order_memory_bytes)) {
    return refuse(Refusal(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        "sort runtime materialization exceeds the selected node memory grant"));
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
  std::sort(row_order.begin(), row_order.end(),
            [&](const std::size_t left, const std::size_t right) {
              const auto comparison =
                  comparisons[left * row_count + right];
              return comparison < 0 || (comparison == 0 && left < right);
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
}  // namespace

CanonicalDescriptorSortResult ExecuteCanonicalDescriptorSort(
    const CanonicalDescriptorSortRequest& request) {
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
  bool separate_order_key_batch = false;
  if (request.order_key_receipt != nullptr) {
    const auto& receipt = *request.order_key_receipt;
    if (!SortReceiptRequestCarriersAreExactDefault(request) ||
        !receipt.exact_current_revalidated_before_issue_ ||
        receipt.borrowed_execution_carriers_ ||
        receipt.physical_dag_.nodes.empty() ||
        receipt.selected_physical_node_id_ == 0 ||
        receipt.maximum_pair_comparisons_ == 0 ||
        receipt.maximum_order_key_batch_bytes_ == 0 ||
        receipt.ordering_property_uuid_.empty()) {
      CanonicalDescriptorSortResult result;
      result.diagnostic = Refusal(
          "QOW-DIAG-QRY-010-ORDER-REFUSAL-V1",
          "expression order-key receipt does not bind this execution");
      return result;
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
    separate_order_key_batch = true;
  }
  return ExecuteCanonicalDescriptorSortBound(
      *physical_dag, *mga_authority, selected_physical_node_id,
      maximum_pair_comparisons, *input_batch, *order_batch, *order_terms,
      *deterministic_tie_evidence_uuid, separate_order_key_batch);
}

CanonicalDescriptorSortResult ExecuteCanonicalDescriptorSort(
    const CanonicalDescriptorSortRequest& request,
    const TypedPhysicalNodeDag& borrowed_execution_dag,
    const DescriptorBatch& borrowed_input_batch) {
  if (request.order_key_receipt != nullptr) {
    const auto& receipt = *request.order_key_receipt;
    if (!SortReceiptRequestCarriersAreExactDefault(request) ||
        !receipt.exact_current_revalidated_before_issue_ ||
        !receipt.borrowed_execution_carriers_ ||
        !TypedPhysicalNodeDagCarrierIsExactDefault(receipt.physical_dag_) ||
        !DescriptorBatchCarrierIsExactDefault(receipt.input_batch_) ||
        receipt.selected_physical_node_id_ == 0 ||
        receipt.maximum_pair_comparisons_ == 0 ||
        receipt.maximum_order_key_batch_bytes_ == 0 ||
        receipt.ordering_property_uuid_.empty()) {
      CanonicalDescriptorSortResult result;
      result.diagnostic = Refusal(
          "QOW-DIAG-QRY-010-ORDER-REFUSAL-V1",
          "borrowed expression order-key receipt does not bind this execution");
      return result;
    }
    return ExecuteCanonicalDescriptorSortBound(
        borrowed_execution_dag, receipt.mga_authority_,
        receipt.selected_physical_node_id_,
        receipt.maximum_pair_comparisons_, borrowed_input_batch,
        receipt.order_key_batch_, receipt.order_terms_,
        receipt.deterministic_tie_evidence_uuid_, true);
  }
  if (!TypedPhysicalNodeDagCarrierIsExactDefault(request.physical_dag) ||
      !DescriptorBatchCarrierIsExactDefault(request.input_batch) ||
      request.order_key_receipt != nullptr) {
    CanonicalDescriptorSortResult result;
    result.diagnostic = Refusal(
        "QOW-DIAG-QRY-010-ORDER-REFUSAL-V1",
        "descriptor order request carries conflicting owned execution carriers");
    return result;
  }
  return ExecuteCanonicalDescriptorSortBound(
      borrowed_execution_dag, request.mga_authority,
      request.selected_physical_node_id, request.maximum_pair_comparisons,
      borrowed_input_batch, borrowed_input_batch, request.order_terms,
      request.deterministic_tie_evidence_uuid, false);
}

CanonicalDescriptorSortResult ExecuteCanonicalDescriptorSort(
    const CanonicalDescriptorSortRequest& request,
    const TypedPhysicalNodeDag& borrowed_execution_dag,
    const DescriptorBatch& borrowed_input_batch,
    const std::vector<CanonicalDescriptorOrderTerm>& borrowed_order_terms,
    const std::string& borrowed_deterministic_tie_evidence_uuid) {
  if (!TypedPhysicalNodeDagCarrierIsExactDefault(request.physical_dag) ||
      !DescriptorBatchCarrierIsExactDefault(request.input_batch) ||
      !DescriptorOrderTermsCarrierIsExactDefault(request.order_terms) ||
      !StringCarrierIsExactDefault(
          request.deterministic_tie_evidence_uuid) ||
      request.order_key_receipt != nullptr) {
    CanonicalDescriptorSortResult result;
    result.diagnostic = Refusal(
        "QOW-DIAG-QRY-010-ORDER-REFUSAL-V1",
        "descriptor order request carries conflicting owned execution or semantic carriers");
    return result;
  }
  return ExecuteCanonicalDescriptorSortBound(
      borrowed_execution_dag, request.mga_authority,
      request.selected_physical_node_id, request.maximum_pair_comparisons,
      borrowed_input_batch, borrowed_input_batch, borrowed_order_terms,
      borrowed_deterministic_tie_evidence_uuid, false);
}

}  // namespace scratchbird::engine::executor
