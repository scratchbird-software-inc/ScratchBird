// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_query_execute.hpp"

#include "canonical_relational_expression.hpp"

#include "catalog/name_resolution_api.hpp"
#include "engine/optimizer/optimizer_contract.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <optional>
#include <ranges>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace scratchbird::engine::sblr {
namespace api = scratchbird::engine::internal_api;
namespace exec = scratchbird::engine::executor;
namespace opt = scratchbird::engine::optimizer;
namespace plan = scratchbird::engine::planner;
namespace {

constexpr std::string_view kValuesImplementationId =
    "values.materialize.canonical.v1";

std::uint64_t Fnv1a64(const std::string_view value,
                      std::uint64_t hash = 14695981039346656037ull) {
  for (const auto byte : value) {
    hash ^= static_cast<std::uint8_t>(byte);
    hash *= 1099511628211ull;
  }
  return hash;
}

std::string DerivedCanonicalUuid(const std::string_view scope,
                                 const std::string_view purpose) {
  const auto first = Fnv1a64(purpose, Fnv1a64(scope));
  const auto second = Fnv1a64(scope, Fnv1a64(purpose));
  std::array<std::uint8_t, 16> bytes{};
  for (std::size_t index = 0; index < 8; ++index) {
    bytes[index] = static_cast<std::uint8_t>(first >> ((7 - index) * 8));
    bytes[8 + index] =
        static_cast<std::uint8_t>(second >> ((7 - index) * 8));
  }
  bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0f) | 0x50);
  bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3f) | 0x80);
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10) out << '-';
    out << std::setw(2) << static_cast<unsigned>(bytes[index]);
  }
  return out.str();
}

api::EngineApiResult Failure(const CanonicalObjectFreeValuesExecutionRequest& request,
                             std::string diagnostic_id,
                             std::string detail) {
  api::EngineApiResult result;
  result.operation_id = "query.execute";
  result.local_transaction_id = request.context.local_transaction_id;
  result.transaction_uuid = request.context.transaction_uuid;
  result.embedded_trust_mode_observed =
      request.context.trust_mode == api::EngineTrustMode::embedded_in_process;
  api::EngineApiDiagnostic diagnostic;
  diagnostic.code = std::move(diagnostic_id);
  diagnostic.message_key = "engine.sblr.query_execute.refused";
  diagnostic.detail = std::move(detail);
  diagnostic.error = true;
  result.diagnostics.push_back(std::move(diagnostic));
  return result;
}

exec::CanonicalResultNullability ResultNullability(
    const api::RelationalNullability nullability) {
  switch (nullability) {
    case api::RelationalNullability::kNonNull:
      return exec::CanonicalResultNullability::kNonNull;
    case api::RelationalNullability::kNullable:
      return exec::CanonicalResultNullability::kNullable;
    case api::RelationalNullability::kUnknown:
      return exec::CanonicalResultNullability::kUnknown;
  }
  return exec::CanonicalResultNullability::kUnknown;
}

struct MaterializedValues {
  bool ok{false};
  exec::DescriptorBatch batch;
  std::vector<exec::CanonicalResultColumnBinding> result_bindings;
  std::string detail;
};

struct PreparedSetOperationRoot {
  bool ok{false};
  std::vector<exec::ExecutorColumnDescriptor> result_columns;
  std::vector<exec::CanonicalResultColumnBinding> result_bindings;
  std::string detail;
};

struct PreparedInnerJoinRoot {
  bool ok{false};
  std::vector<exec::CanonicalResultColumnBinding> result_bindings;
  std::string detail;
};

struct PreparedFilterRoot {
  bool ok{false};
  std::vector<exec::CanonicalResultColumnBinding> result_bindings;
  std::string detail;
};

struct PreparedProjectRoot {
  bool ok{false};
  std::vector<std::size_t> projected_columns;
  std::vector<exec::CanonicalResultColumnBinding> result_bindings;
  std::string detail;
};

struct PreparedLimitRoot {
  bool ok{false};
  std::vector<exec::CanonicalResultColumnBinding> result_bindings;
  std::string detail;
};

struct PreparedSortRoot {
  bool ok{false};
  std::vector<exec::CanonicalDescriptorOrderTerm> order_terms;
  std::vector<exec::CanonicalResultColumnBinding> result_bindings;
  std::string ordering_property_uuid;
  std::string detail;
};

struct PreparedGlobalAggregateRoot {
  bool ok{false};
  bool count_star{false};
  std::vector<std::size_t> value_columns;
  std::vector<std::uint32_t> value_descriptor_ids;
  std::vector<api::EngineTypedValue> direct_arguments;
  std::vector<exec::CanonicalDescriptorOrderTerm> aggregate_order_terms;
  std::string aggregate_separator{","};
  exec::CanonicalListaggOverflowMode listagg_overflow_mode =
      exec::CanonicalListaggOverflowMode::none;
  std::size_t listagg_max_output_bytes{0};
  std::string listagg_truncation_indicator{"..."};
  bool listagg_with_count{true};
  exec::CanonicalAggregateDescriptor aggregate_descriptor;
  exec::ExecutorColumnDescriptor result_column;
  std::vector<exec::CanonicalResultColumnBinding> result_bindings;
  std::string detail;
};

PreparedGlobalAggregateRoot PrepareGlobalAggregateRoot(
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalRelationalNode& root,
    const plan::CanonicalLogicalRelationalNode& input_node,
    const MaterializedValues& input,
    const exec::CanonicalAggregateFunction function,
    const bool count_star) {
  PreparedGlobalAggregateRoot result;
  result.count_star = count_star;
  const bool is_count = function == exec::CanonicalAggregateFunction::count;
  const bool is_sum = function == exec::CanonicalAggregateFunction::sum;
  const bool is_avg = function == exec::CanonicalAggregateFunction::avg;
  const bool is_min = function == exec::CanonicalAggregateFunction::min;
  const bool is_max = function == exec::CanonicalAggregateFunction::max;
  const bool is_bool_and =
      function == exec::CanonicalAggregateFunction::bool_and;
  const bool is_bool_or =
      function == exec::CanonicalAggregateFunction::bool_or;
  const bool is_every = function == exec::CanonicalAggregateFunction::every;
  const bool is_boolean = is_bool_and || is_bool_or || is_every;
  const bool is_stddev_pop =
      function == exec::CanonicalAggregateFunction::stddev_pop;
  const bool is_variance_pop =
      function == exec::CanonicalAggregateFunction::variance_pop;
  const bool is_stddev =
      function == exec::CanonicalAggregateFunction::stddev;
  const bool is_variance =
      function == exec::CanonicalAggregateFunction::variance;
  const bool is_stddev_samp =
      function == exec::CanonicalAggregateFunction::stddev_samp;
  const bool is_variance_samp =
      function == exec::CanonicalAggregateFunction::variance_samp;
  const bool is_statistical =
      is_stddev_pop || is_variance_pop || is_stddev || is_variance ||
      is_stddev_samp || is_variance_samp;
  const bool is_regr_count =
      function == exec::CanonicalAggregateFunction::regr_count;
  const bool is_pair_statistical =
      function == exec::CanonicalAggregateFunction::corr ||
      function == exec::CanonicalAggregateFunction::covar_pop ||
      function == exec::CanonicalAggregateFunction::covar_samp ||
      is_regr_count ||
      function == exec::CanonicalAggregateFunction::regr_avgx ||
      function == exec::CanonicalAggregateFunction::regr_avgy ||
      function == exec::CanonicalAggregateFunction::regr_intercept ||
      function == exec::CanonicalAggregateFunction::regr_r2 ||
      function == exec::CanonicalAggregateFunction::regr_slope ||
      function == exec::CanonicalAggregateFunction::regr_sxx ||
      function == exec::CanonicalAggregateFunction::regr_sxy ||
      function == exec::CanonicalAggregateFunction::regr_syy;
  const bool is_string_agg =
      function == exec::CanonicalAggregateFunction::string_agg;
  const bool is_ordered_string_agg =
      is_string_agg &&
      root.semantic_variant_id ==
          "aggregate.global-string-agg-ordered-expression.v1";
  const bool is_listagg =
      function == exec::CanonicalAggregateFunction::listagg;
  const bool is_listagg_ordered =
      is_listagg &&
      root.semantic_variant_id ==
          "aggregate.global-listagg-ordered-expression.v1";
  const bool is_listagg_overflow_error =
      is_listagg &&
      root.semantic_variant_id ==
          "aggregate.global-listagg-ordered-overflow-error-expression.v1";
  const bool is_listagg_overflow_truncate =
      is_listagg &&
      root.semantic_variant_id ==
          "aggregate.global-listagg-ordered-overflow-truncate-expression.v1";
  const bool is_listagg_profile =
      is_listagg_ordered || is_listagg_overflow_error ||
      is_listagg_overflow_truncate;
  const bool is_array_agg =
      function == exec::CanonicalAggregateFunction::array_agg;
  const bool is_json_agg =
      function == exec::CanonicalAggregateFunction::json_agg;
  const bool is_json_object_agg =
      function == exec::CanonicalAggregateFunction::json_object_agg;
  const bool is_ordered_single_collection = is_array_agg || is_json_agg;
  const bool is_ordered_collection =
      is_ordered_single_collection || is_json_object_agg;
  const bool is_hypothetical_rank =
      function == exec::CanonicalAggregateFunction::rank;
  const bool is_hypothetical_dense_rank =
      function == exec::CanonicalAggregateFunction::dense_rank;
  const bool is_hypothetical_percent_rank =
      function == exec::CanonicalAggregateFunction::percent_rank;
  const bool is_hypothetical_cume_dist =
      function == exec::CanonicalAggregateFunction::cume_dist;
  const bool is_hypothetical =
      is_hypothetical_rank || is_hypothetical_dense_rank ||
      is_hypothetical_percent_rank || is_hypothetical_cume_dist;
  const bool is_mode = function == exec::CanonicalAggregateFunction::mode;
  const bool is_percentile_cont =
      function == exec::CanonicalAggregateFunction::percentile_cont;
  const bool is_percentile_disc =
      function == exec::CanonicalAggregateFunction::percentile_disc;
  const bool is_exact_percentile = is_percentile_cont || is_percentile_disc;
  const bool is_ordered_set =
      is_hypothetical || is_mode || is_exact_percentile;
  if ((!is_count && !is_sum && !is_avg && !is_min && !is_max &&
       !is_boolean && !is_statistical && !is_pair_statistical &&
       !is_string_agg && !is_listagg_profile && !is_ordered_collection &&
       !is_ordered_set) ||
      (count_star && !is_count)) {
    result.detail = "global aggregate function profile is not admitted";
    return result;
  }
  if (root.output_descriptor_ids.size() != 1 ||
      root.bound_expression_ids.size() != 1 ||
      input.result_bindings.size() != input.batch.columns.size() ||
      input_node.output_descriptor_ids.size() != input.batch.columns.size() ||
      std::ranges::find(input_node.output_descriptor_ids,
                        root.output_descriptor_ids.front()) !=
          input_node.output_descriptor_ids.end()) {
    result.detail =
        "global aggregate input or output descriptor coverage is unresolved";
    return result;
  }

  const api::RelationalOutputRecord* output = nullptr;
  for (const auto& candidate : dag.outputs) {
    if (candidate.relation_node_id != root.logical_node_id) continue;
    if (output != nullptr) {
      result.detail = "global aggregate requires exactly one bound output";
      return result;
    }
    output = &candidate;
  }
  if (output == nullptr || output->ordinal != 0 || !output->visible ||
      output->output_name_utf8.empty() ||
      output->descriptor_id != root.output_descriptor_ids.front() ||
      output->expression_id != root.bound_expression_ids.front()) {
    result.detail = "global aggregate output lineage is not exact";
    return result;
  }

  const auto expression = std::ranges::find_if(
      dag.expressions, [&](const auto& candidate) {
        return candidate.expression_id == root.bound_expression_ids.front();
      });
  const auto descriptor = std::ranges::find_if(
      dag.descriptors, [&](const auto& candidate) {
        return candidate.descriptor_id == root.output_descriptor_ids.front();
      });
  const auto registry = exec::CanonicalAggregateRuntimeRegistryV1();
  const auto aggregate = std::ranges::find_if(
      registry, [&](const auto& candidate) {
        return candidate.function == function;
      });
  std::size_t expected_argument_count = 1;
  if (count_star) {
    expected_argument_count = 0;
  } else if (is_listagg_overflow_truncate) {
    expected_argument_count = 6;
  } else if (is_listagg_overflow_error) {
    expected_argument_count = 4;
  } else if (is_listagg_ordered) {
    expected_argument_count = 3;
  } else if (is_hypothetical || is_exact_percentile) {
    expected_argument_count = 2;
  } else if (is_json_object_agg || is_ordered_string_agg) {
    expected_argument_count = 3;
  } else if (is_pair_statistical || is_string_agg ||
             is_ordered_single_collection) {
    expected_argument_count = 2;
  }
  if (expression == dag.expressions.end() ||
      descriptor == dag.descriptors.end() || aggregate == registry.end() ||
      !aggregate->executable ||
      expression->expression_kind !=
          api::RelationalExpressionKind::kFunctionCall ||
      expression->child_expression_ids.size() != expected_argument_count ||
      expression->result_descriptor_id != descriptor->descriptor_id ||
      !expression->function_uuid.has_value() ||
      *expression->function_uuid != aggregate->function_uuid ||
      expression->bound_name_uuid.has_value() ||
      expression->literal_kind.has_value() ||
      expression->operator_name.has_value() ||
      expression->literal_or_parameter_ref.has_value()) {
    result.detail =
        "global aggregate function identity or argument binding is invalid";
    return result;
  }

  if (!count_star) {
    CanonicalRelationalExpressionRuntime expression_runtime(dag);
    for (std::size_t argument_ordinal = 0;
         argument_ordinal < expression->child_expression_ids.size();
         ++argument_ordinal) {
      const auto child_expression_id =
          expression->child_expression_ids[argument_ordinal];
      const auto argument = std::ranges::find_if(
          dag.expressions, [&](const auto& candidate) {
            return candidate.expression_id == child_expression_id;
          });
      if ((is_hypothetical || is_exact_percentile) &&
          argument_ordinal == 0) {
        const auto direct_descriptor = std::ranges::find_if(
            dag.descriptors, [&](const auto& candidate) {
              return argument != dag.expressions.end() &&
                     candidate.descriptor_id == argument->result_descriptor_id;
            });
        const bool exact_literal =
            argument != dag.expressions.end() &&
            argument->expression_kind ==
                api::RelationalExpressionKind::kLiteral &&
            argument->child_expression_ids.empty() &&
            !argument->bound_name_uuid.has_value() &&
            !argument->function_uuid.has_value() &&
            argument->literal_kind == api::RelationalLiteralKind::kNumeric &&
            !argument->operator_name.has_value() &&
            argument->literal_or_parameter_ref.has_value() &&
            direct_descriptor != dag.descriptors.end() &&
            direct_descriptor->nullability ==
                api::RelationalNullability::kNonNull &&
            !direct_descriptor->collation_uuid.has_value() &&
            !direct_descriptor->timezone_profile_id.has_value() &&
            !direct_descriptor->width.has_value() &&
            !direct_descriptor->precision.has_value() &&
            !direct_descriptor->scale.has_value() &&
            argument->result_descriptor_id !=
                root.output_descriptor_ids.front() &&
            std::ranges::find(input_node.output_descriptor_ids,
                              argument->result_descriptor_id) ==
                input_node.output_descriptor_ids.end();
        if (!exact_literal) {
          result.detail =
              "global ordered-set direct argument must be one standalone, "
              "unqualified, non-NULL canonical numeric literal";
          return result;
        }
        api::EngineTypedValue direct_argument;
        std::string direct_detail;
        const std::string_view direct_type =
            is_exact_percentile ? std::string_view("real64")
                                : std::string_view("int64");
        if (!expression_runtime.Evaluate(child_expression_id, direct_type,
                                         &direct_argument, &direct_detail) ||
            direct_argument.state != api::EngineValueState::value ||
            direct_argument.is_null ||
            direct_argument.descriptor.canonical_type_name != direct_type) {
          result.detail =
              is_exact_percentile
                  ? "global exact percentile fraction must be a canonical "
                    "real64 literal"
                  : "global hypothetical-set direct argument must be a "
                    "canonical int64 literal";
          if (!direct_detail.empty()) result.detail += ": " + direct_detail;
          return result;
        }
        result.direct_arguments.push_back(std::move(direct_argument));
        continue;
      }
      if ((is_string_agg || is_listagg_profile) && argument_ordinal == 1) {
        const auto separator_descriptor = std::ranges::find_if(
            dag.descriptors, [&](const auto& candidate) {
              return argument != dag.expressions.end() &&
                     candidate.descriptor_id ==
                         argument->result_descriptor_id;
            });
        api::EngineTypedValue separator;
        std::string separator_detail;
        if (argument == dag.expressions.end() ||
            argument->expression_kind !=
                api::RelationalExpressionKind::kLiteral ||
            !argument->child_expression_ids.empty() ||
            argument->bound_name_uuid.has_value() ||
            argument->function_uuid.has_value() ||
            !argument->literal_kind.has_value() ||
            argument->operator_name.has_value() ||
            !argument->literal_or_parameter_ref.has_value() ||
            separator_descriptor == dag.descriptors.end() ||
            separator_descriptor->nullability !=
                api::RelationalNullability::kNonNull ||
            separator_descriptor->collation_uuid.has_value() ||
            separator_descriptor->timezone_profile_id.has_value() ||
            separator_descriptor->width.has_value() ||
            separator_descriptor->precision.has_value() ||
            separator_descriptor->scale.has_value() ||
            argument->result_descriptor_id ==
                root.output_descriptor_ids.front() ||
            std::ranges::find(input_node.output_descriptor_ids,
                              argument->result_descriptor_id) !=
                input_node.output_descriptor_ids.end() ||
            !expression_runtime.Evaluate(child_expression_id, "text",
                                         &separator, &separator_detail) ||
            separator.state != api::EngineValueState::value ||
            separator.is_null ||
            separator.descriptor.canonical_type_name != "text") {
          result.detail =
              "global STRING_AGG/LISTAGG separator must be one standalone, "
              "unqualified, non-NULL canonical text literal";
          if (!separator_detail.empty()) {
            result.detail += ": " + separator_detail;
          }
          return result;
        }
        result.aggregate_separator = separator.encoded_value;
        continue;
      }
      if (is_listagg_profile && argument_ordinal >= 3) {
        const auto option_descriptor = std::ranges::find_if(
            dag.descriptors, [&](const auto& candidate) {
              return argument != dag.expressions.end() &&
                     candidate.descriptor_id == argument->result_descriptor_id;
            });
        const bool exact_literal =
            argument != dag.expressions.end() &&
            argument->expression_kind ==
                api::RelationalExpressionKind::kLiteral &&
            argument->child_expression_ids.empty() &&
            !argument->bound_name_uuid.has_value() &&
            !argument->function_uuid.has_value() &&
            argument->literal_kind.has_value() &&
            !argument->operator_name.has_value() &&
            argument->literal_or_parameter_ref.has_value() &&
            option_descriptor != dag.descriptors.end() &&
            option_descriptor->nullability ==
                api::RelationalNullability::kNonNull &&
            !option_descriptor->collation_uuid.has_value() &&
            !option_descriptor->timezone_profile_id.has_value() &&
            !option_descriptor->width.has_value() &&
            !option_descriptor->precision.has_value() &&
            !option_descriptor->scale.has_value() &&
            argument->result_descriptor_id !=
                root.output_descriptor_ids.front() &&
            std::ranges::find(input_node.output_descriptor_ids,
                              argument->result_descriptor_id) ==
                input_node.output_descriptor_ids.end();
        if (!exact_literal) {
          result.detail =
              "global LISTAGG overflow options must be standalone, "
              "unqualified, non-NULL canonical literals";
          return result;
        }
        api::EngineTypedValue option;
        std::string option_detail;
        if (argument_ordinal == 3) {
          if (!expression_runtime.Evaluate(child_expression_id, "int64",
                                           &option, &option_detail) ||
              option.state != api::EngineValueState::value || option.is_null ||
              option.descriptor.canonical_type_name != "int64") {
            result.detail =
                "global LISTAGG overflow bound must be a positive canonical "
                "int64 literal";
            if (!option_detail.empty()) result.detail += ": " + option_detail;
            return result;
          }
          std::int64_t decoded = 0;
          const auto [end, error] = std::from_chars(
              option.encoded_value.data(),
              option.encoded_value.data() + option.encoded_value.size(),
              decoded);
          if (error != std::errc{} ||
              end != option.encoded_value.data() + option.encoded_value.size() ||
              decoded <= 0 ||
              static_cast<std::uint64_t>(decoded) >
                  std::numeric_limits<std::size_t>::max()) {
            result.detail =
                "global LISTAGG overflow bound must be a positive canonical "
                "int64 literal";
            return result;
          }
          result.listagg_max_output_bytes =
              static_cast<std::size_t>(decoded);
          continue;
        }
        if (argument_ordinal == 4) {
          if (!expression_runtime.Evaluate(child_expression_id, "text", &option,
                                           &option_detail) ||
              option.state != api::EngineValueState::value || option.is_null ||
              option.descriptor.canonical_type_name != "text") {
            result.detail =
                "global LISTAGG truncation indicator must be a canonical "
                "text literal";
            if (!option_detail.empty()) result.detail += ": " + option_detail;
            return result;
          }
          result.listagg_truncation_indicator = option.encoded_value;
          continue;
        }
        if (argument_ordinal == 5) {
          if (!expression_runtime.Evaluate(child_expression_id, "boolean",
                                           &option, &option_detail) ||
              option.state != api::EngineValueState::value || option.is_null ||
              option.descriptor.canonical_type_name != "boolean" ||
              (option.encoded_value != "true" &&
               option.encoded_value != "false")) {
            result.detail =
                "global LISTAGG WITH/WITHOUT COUNT option must be a canonical "
                "boolean literal";
            if (!option_detail.empty()) result.detail += ": " + option_detail;
            return result;
          }
          result.listagg_with_count = option.encoded_value == "true";
          continue;
        }
      }
      if (argument == dag.expressions.end() ||
          argument->expression_kind !=
              api::RelationalExpressionKind::kIdentifier ||
          !argument->child_expression_ids.empty() ||
          !argument->bound_name_uuid.has_value() ||
          argument->function_uuid.has_value() ||
          argument->literal_kind.has_value() ||
          argument->operator_name.has_value() ||
          argument->literal_or_parameter_ref.has_value()) {
        result.detail =
            "global aggregate expression arguments are not exact bound input "
            "identifiers";
        return result;
      }
      const auto input_descriptor = std::ranges::find(
          input_node.output_descriptor_ids, argument->result_descriptor_id);
      if (input_descriptor == input_node.output_descriptor_ids.end() ||
          std::ranges::count(input_node.output_descriptor_ids,
                             argument->result_descriptor_id) != 1) {
        result.detail =
            "global aggregate expression descriptor is not uniquely supplied "
            "by its input";
        return result;
      }
      const auto value_column = static_cast<std::size_t>(
          std::distance(input_node.output_descriptor_ids.begin(),
                        input_descriptor));
      if (value_column >= input.batch.columns.size() ||
          input.batch.columns[value_column].descriptor_id !=
              argument->result_descriptor_id) {
        result.detail =
            "global aggregate expression input ordinal is not "
            "descriptor-exact";
        return result;
      }
      const auto input_type =
          input.batch.columns[value_column].descriptor.canonical_type_name;
      const bool is_order_argument =
          (is_ordered_string_agg && argument_ordinal == 2) ||
          (is_listagg_profile && argument_ordinal == 2) ||
          (is_ordered_single_collection && argument_ordinal == 1) ||
          (is_json_object_agg && argument_ordinal == 2) ||
          (is_mode && argument_ordinal == 0) ||
          ((is_hypothetical || is_exact_percentile) &&
           argument_ordinal == 1);
      if (is_order_argument) {
        if (input_type != "int64") {
          result.detail = is_ordered_set
                              ? "global ordered-set value/order input must be "
                                "a canonical int64 column"
                              : "global aggregate order input must be a "
                                "canonical int64 column";
          return result;
        }
        exec::CanonicalDescriptorOrderTerm order_term;
        order_term.column = value_column;
        order_term.expression_descriptor_id = argument->result_descriptor_id;
        order_term.direction =
            exec::CanonicalDescriptorOrderDirection::ascending;
        order_term.null_placement =
            exec::CanonicalDescriptorNullPlacement::last;
        const auto validation = exec::ValidateCanonicalDescriptorOrderTerm(
            order_term, input.batch.columns[value_column]);
        if (!validation.ok) {
          result.detail = validation.detail;
          return result;
        }
        result.aggregate_order_terms.push_back(std::move(order_term));
        if (is_ordered_set) {
          result.value_columns.push_back(value_column);
          result.value_descriptor_ids.push_back(
              argument->result_descriptor_id);
        }
        continue;
      }
      result.value_columns.push_back(value_column);
      result.value_descriptor_ids.push_back(argument->result_descriptor_id);
      if ((is_sum || is_avg || is_min || is_max || is_statistical ||
           is_pair_statistical) &&
          input_type != "int64") {
        if (is_sum) {
          result.detail = "global SUM input must be a canonical int64 column";
        } else if (is_avg) {
          result.detail = "global AVG input must be a canonical int64 column";
        } else if (is_statistical) {
          result.detail =
              "global unary statistical input must be a canonical int64 "
              "column";
        } else if (is_pair_statistical) {
          result.detail =
              "global pair statistical inputs must be canonical int64 "
              "columns";
        } else {
          result.detail =
              "global MIN/MAX input must be a canonical int64 column";
        }
        return result;
      }
      if (is_boolean && input_type != "boolean") {
        result.detail =
            "global BOOL_AND/BOOL_OR/EVERY input must be a canonical boolean "
            "column";
        return result;
      }
      if (is_string_agg && input_type != "text") {
        result.detail =
            "global STRING_AGG input must be a canonical text column";
        return result;
      }
      if (is_listagg_profile && input_type != "text") {
        result.detail =
            "global LISTAGG input must be a canonical text column";
        return result;
      }
      if (is_ordered_single_collection && input_type != "text") {
        result.detail =
            "global ARRAY_AGG/JSON_AGG input must be a canonical text column";
        return result;
      }
      if (is_json_object_agg && argument_ordinal == 0 &&
          input_type != "text") {
        result.detail =
            "global JSON_OBJECT_AGG key must be a canonical text column";
        return result;
      }
      if (is_json_object_agg && argument_ordinal == 1 &&
          input_type != "int64") {
        result.detail =
            "global JSON_OBJECT_AGG value must be a canonical int64 column";
        return result;
      }
    }
  }
  const bool result_nullable =
      is_sum || is_avg || is_min || is_max || is_boolean || is_statistical ||
      (is_pair_statistical && !is_regr_count) || is_string_agg ||
      is_listagg_profile || is_ordered_collection || is_mode ||
      is_exact_percentile;
  const auto expected_nullability =
      result_nullable ? api::RelationalNullability::kNullable
                      : api::RelationalNullability::kNonNull;
  if (descriptor->nullability != expected_nullability ||
      descriptor->collation_uuid.has_value() ||
      descriptor->timezone_profile_id.has_value() ||
      descriptor->width.has_value() || descriptor->precision.has_value() ||
      descriptor->scale.has_value()) {
    if (is_sum) {
      result.detail =
          "global SUM result must be an unqualified nullable int64";
    } else if (is_avg) {
      result.detail =
          "global AVG result must be an unqualified nullable real64";
    } else if (is_min || is_max) {
      result.detail =
          "global MIN/MAX result must be an unqualified nullable int64";
    } else if (is_boolean) {
      result.detail =
          "global BOOL_AND/BOOL_OR/EVERY result must be an unqualified "
          "nullable boolean";
    } else if (is_statistical) {
      result.detail =
          "global unary statistical result must be an unqualified nullable "
          "real64";
    } else if (is_pair_statistical) {
      result.detail =
          is_regr_count
              ? "global REGR_COUNT result must be an unqualified non-null "
                "int64"
              : "global pair statistical result must be an unqualified "
                "nullable real64";
    } else if (is_string_agg) {
      result.detail =
          "global STRING_AGG result must be an unqualified nullable text";
    } else if (is_listagg_profile) {
      result.detail =
          "global LISTAGG result must be an unqualified nullable text";
    } else if (is_array_agg) {
      result.detail =
          "global ARRAY_AGG result must be an unqualified nullable "
          "list<text nullable>";
    } else if (is_json_agg) {
      result.detail =
          "global JSON_AGG result must be an unqualified nullable json";
    } else if (is_json_object_agg) {
      result.detail =
          "global JSON_OBJECT_AGG result must be an unqualified nullable "
          "json";
    } else if (is_mode) {
      result.detail =
          "global MODE result must be an unqualified nullable int64";
    } else if (is_exact_percentile) {
      result.detail =
          "global exact percentile result must be an unqualified nullable "
          "real64";
    } else if (is_hypothetical_rank || is_hypothetical_dense_rank) {
      result.detail =
          "global hypothetical RANK/DENSE_RANK result must be an unqualified "
          "non-null int64";
    } else if (is_hypothetical_percent_rank ||
               is_hypothetical_cume_dist) {
      result.detail =
          "global hypothetical PERCENT_RANK/CUME_DIST result must be an "
          "unqualified non-null real64";
    } else {
      result.detail =
          "global COUNT result must be an unqualified non-null int64";
    }
    return result;
  }

  result.aggregate_descriptor =
      {aggregate->abi_version, aggregate->function, aggregate->builtin_id,
       aggregate->function_uuid, count_star};
  api::EngineDescriptor engine_descriptor;
  engine_descriptor.descriptor_uuid.canonical = descriptor->descriptor_uuid;
  engine_descriptor.descriptor_kind = "scalar";
  if (is_array_agg) {
    engine_descriptor.canonical_type_name = "list<text nullable>";
  } else if (is_json_agg || is_json_object_agg) {
    engine_descriptor.canonical_type_name = "json";
  } else if (is_string_agg || is_listagg_profile) {
    engine_descriptor.canonical_type_name = "text";
  } else if (is_avg || is_statistical ||
             (is_pair_statistical && !is_regr_count) ||
             is_exact_percentile || is_hypothetical_percent_rank ||
             is_hypothetical_cume_dist) {
    engine_descriptor.canonical_type_name = "real64";
  } else if (is_boolean) {
    engine_descriptor.canonical_type_name = "boolean";
  } else {
    engine_descriptor.canonical_type_name = "int64";
  }
  engine_descriptor.encoded_descriptor =
      "type_uuid=" + descriptor->type_uuid + ";nullability=" +
      (result_nullable ? "nullable" : "non_null");
  result.result_column = {output->output_name_utf8, engine_descriptor,
                          result_nullable, descriptor->descriptor_id};
  exec::CanonicalResultColumnBinding binding;
  binding.physical_column_ordinal = 0;
  binding.visible = true;
  binding.published_descriptor = exec::CanonicalResultColumnDescriptor{
      0,
      output->output_name_utf8,
      descriptor->descriptor_uuid,
      descriptor->type_uuid,
      result_nullable ? exec::CanonicalResultNullability::kNullable
                      : exec::CanonicalResultNullability::kNonNull,
      std::nullopt,
      std::nullopt};
  result.result_bindings.push_back(std::move(binding));
  if (is_listagg_overflow_error) {
    result.listagg_overflow_mode =
        exec::CanonicalListaggOverflowMode::error;
  } else if (is_listagg_overflow_truncate) {
    result.listagg_overflow_mode =
        exec::CanonicalListaggOverflowMode::truncate;
  }
  result.ok = true;
  return result;
}

PreparedSortRoot PrepareSortRoot(
    const api::EngineRequestContext& context,
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalPropertyCatalog& properties,
    const plan::CanonicalLogicalRelationalNode& root,
    const plan::CanonicalLogicalRelationalNode& input_node,
    const MaterializedValues& input) {
  PreparedSortRoot result;
  if (root.output_descriptor_ids.empty() ||
      root.output_descriptor_ids != input_node.output_descriptor_ids ||
      input.result_bindings.size() != input.batch.columns.size() ||
      input_node.output_descriptor_ids.size() != input.batch.columns.size()) {
    result.detail = "sort output does not preserve its bound input schema";
    return result;
  }
  if (std::ranges::any_of(dag.outputs, [&](const auto& output) {
        return output.relation_node_id == root.logical_node_id;
      })) {
    result.detail = "sort root output lineage is not admitted by this profile";
    return result;
  }
  if (properties.properties.size() != 1 ||
      root.required_property_uuids.size() != 1 ||
      root.delivered_property_uuids.size() != 1 ||
      root.required_property_uuids.front() !=
          root.delivered_property_uuids.front()) {
    result.detail =
        "sort requires exactly one enforced ordering property";
    return result;
  }
  const auto& property = properties.properties.front();
  if (property.property_uuid != root.required_property_uuids.front() ||
      property.property_kind !=
          plan::CanonicalLogicalPropertyKind::kOrdering ||
      property.origin_logical_node_id != root.logical_node_id ||
      property.ordering_terms.empty()) {
    result.detail = "sort ordering property identity or origin is unresolved";
    return result;
  }

  std::unordered_map<std::uint32_t, const api::RelationalExpressionRecord*>
      expressions;
  for (const auto& expression : dag.expressions) {
    expressions.emplace(expression.expression_id, &expression);
  }
  std::unordered_map<std::uint32_t, std::size_t> input_ordinals;
  for (std::size_t ordinal = 0;
       ordinal < input_node.output_descriptor_ids.size(); ++ordinal) {
    input_ordinals.emplace(input_node.output_descriptor_ids[ordinal], ordinal);
  }
  std::unordered_set<std::uint32_t> bound_order_expressions(
      root.bound_expression_ids.begin(), root.bound_expression_ids.end());
  if (bound_order_expressions.size() != property.ordering_terms.size()) {
    result.detail =
        "sort bound-expression coverage differs from its ordering terms";
    return result;
  }

  for (const auto& logical_term : property.ordering_terms) {
    const auto expression = expressions.find(logical_term.expression_id);
    if (expression == expressions.end() ||
        !bound_order_expressions.contains(logical_term.expression_id) ||
        std::ranges::find(input_node.bound_expression_ids,
                          logical_term.expression_id) ==
            input_node.bound_expression_ids.end()) {
      result.order_terms.clear();
      result.detail = "sort ordering expression is not bound to its input";
      return result;
    }
    const auto ordinal =
        input_ordinals.find(expression->second->result_descriptor_id);
    if (ordinal == input_ordinals.end()) {
      result.order_terms.clear();
      result.detail =
          "sort ordering expression does not resolve to an input descriptor";
      return result;
    }

    exec::CanonicalDescriptorOrderTerm term;
    term.column = ordinal->second;
    term.expression_descriptor_id = expression->second->result_descriptor_id;
    term.direction =
        logical_term.direction ==
                plan::CanonicalLogicalPropertySortDirection::kAscending
            ? exec::CanonicalDescriptorOrderDirection::ascending
            : exec::CanonicalDescriptorOrderDirection::descending;
    term.null_placement =
        logical_term.null_placement ==
                plan::CanonicalLogicalPropertyNullPlacement::kNullsFirst
            ? exec::CanonicalDescriptorNullPlacement::first
            : exec::CanonicalDescriptorNullPlacement::last;
    term.collation_uuid = logical_term.collation_uuid;

    const auto& column = input.batch.columns[term.column];
    if (column.descriptor.canonical_type_name == "text") {
#if defined(SCRATCHBIRD_QOW_QUERY_ROUTE_CONTRACT_ONLY)
      result.order_terms.clear();
      result.detail =
          "sort character ordering requires the production engine resource "
          "catalog";
      return result;
#else
      api::EngineUuid collation_uuid;
      collation_uuid.canonical = term.collation_uuid;
      const auto resolved = api::LookupEngineResourceDescriptorByUuid(
          context, collation_uuid, "collation");
      if (!resolved.ok || !resolved.resource_descriptor.present ||
          resolved.resource_descriptor.resource_uuid.canonical !=
              term.collation_uuid) {
        result.order_terms.clear();
        result.detail =
            "sort character ordering lacks current engine collation "
            "authority: " +
            (resolved.diagnostic.code.empty()
                 ? std::string("CATALOG.RESOURCE.DESCRIPTOR_INVALID")
                 : resolved.diagnostic.code);
        return result;
      }
      term.resource_epoch = resolved.resource_descriptor.resource_epoch;
      term.collation_epoch = resolved.resource_descriptor.family_epoch;
      term.text_seed.active = true;
      term.text_seed.seed_pack_name =
          resolved.resource_descriptor.seed_pack_name;
      term.text_seed.seed_pack_version =
          resolved.resource_descriptor.seed_pack_version;
      term.text_seed.charset_name =
          resolved.resource_descriptor.parent_canonical_name;
      term.text_seed.collation_name =
          resolved.resource_descriptor.canonical_name;
      term.text_seed.collation_case_insensitive =
          resolved.resource_descriptor.case_insensitive;
      term.text_seed.collation_accent_insensitive =
          resolved.resource_descriptor.accent_insensitive;
#endif
    }
    const auto validation =
        exec::ValidateCanonicalDescriptorOrderTerm(term, column);
    if (!validation.ok) {
      result.order_terms.clear();
      result.detail = validation.detail;
      return result;
    }
    result.order_terms.push_back(std::move(term));
  }

  result.result_bindings = input.result_bindings;
  result.ordering_property_uuid = property.property_uuid;
  result.ok = true;
  return result;
}

PreparedLimitRoot PrepareLimitRoot(
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalRelationalNode& root,
    const plan::CanonicalLogicalRelationalNode& input_node,
    const MaterializedValues& input) {
  PreparedLimitRoot result;
  if (root.output_descriptor_ids.empty() ||
      root.output_descriptor_ids != input_node.output_descriptor_ids ||
      input.result_bindings.size() != input.batch.columns.size()) {
    result.detail = "limit output does not preserve its bound input schema";
    return result;
  }
  if (std::ranges::any_of(dag.outputs, [&](const auto& output) {
        return output.relation_node_id == root.logical_node_id;
      })) {
    result.detail = "limit root output lineage is not admitted by this profile";
    return result;
  }
  result.result_bindings = input.result_bindings;
  result.ok = true;
  return result;
}

PreparedProjectRoot PrepareDescriptorDirectProjectRoot(
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalRelationalNode& root,
    const plan::CanonicalLogicalRelationalNode& input_node,
    const MaterializedValues& input) {
  PreparedProjectRoot result;
  if (root.output_descriptor_ids.empty() ||
      input.result_bindings.size() != input.batch.columns.size() ||
      input_node.output_descriptor_ids.size() != input.batch.columns.size()) {
    result.detail = "project input or output descriptor coverage is incomplete";
    return result;
  }
  if (std::ranges::any_of(dag.outputs, [&](const auto& output) {
        return output.relation_node_id == root.logical_node_id;
      })) {
    result.detail =
        "descriptor-direct project does not admit root output lineage";
    return result;
  }

  std::unordered_map<std::uint32_t, std::size_t> input_ordinals;
  for (std::size_t ordinal = 0;
       ordinal < input_node.output_descriptor_ids.size(); ++ordinal) {
    input_ordinals.emplace(input_node.output_descriptor_ids[ordinal], ordinal);
  }
  std::size_t published_ordinal = 0;
  for (std::size_t output_ordinal = 0;
       output_ordinal < root.output_descriptor_ids.size(); ++output_ordinal) {
    const auto source =
        input_ordinals.find(root.output_descriptor_ids[output_ordinal]);
    if (source == input_ordinals.end()) {
      result.projected_columns.clear();
      result.result_bindings.clear();
      result.detail =
          "descriptor-direct project output is not an input column";
      return result;
    }
    result.projected_columns.push_back(source->second);
    auto binding = input.result_bindings[source->second];
    binding.physical_column_ordinal = output_ordinal;
    if (binding.visible) {
      if (!binding.published_descriptor.has_value()) {
        result.projected_columns.clear();
        result.result_bindings.clear();
        result.detail = "project visible result binding is incomplete";
        return result;
      }
      binding.published_descriptor->ordinal =
          static_cast<std::uint32_t>(published_ordinal++);
    }
    result.result_bindings.push_back(std::move(binding));
  }
  result.ok = true;
  return result;
}

PreparedFilterRoot PrepareFilterRoot(
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalRelationalNode& root,
    const plan::CanonicalLogicalRelationalNode& input_node,
    const MaterializedValues& input) {
  PreparedFilterRoot result;
  if (root.output_descriptor_ids.empty() ||
      root.output_descriptor_ids != input_node.output_descriptor_ids ||
      input.result_bindings.size() != input.batch.columns.size()) {
    result.detail = "filter output does not preserve its bound input schema";
    return result;
  }
  if (std::ranges::any_of(dag.outputs, [&](const auto& output) {
        return output.relation_node_id == root.logical_node_id;
      })) {
    result.detail = "filter root output lineage is not admitted by this profile";
    return result;
  }
  result.result_bindings = input.result_bindings;
  result.ok = true;
  return result;
}

PreparedInnerJoinRoot PrepareInnerJoinRoot(
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalRelationalNode& root,
    const plan::CanonicalLogicalRelationalNode& left_node,
    const plan::CanonicalLogicalRelationalNode& right_node,
    const MaterializedValues& left,
    const MaterializedValues& right) {
  PreparedInnerJoinRoot result;
  std::vector<std::uint32_t> concatenated_descriptors =
      left_node.output_descriptor_ids;
  concatenated_descriptors.insert(concatenated_descriptors.end(),
                                  right_node.output_descriptor_ids.begin(),
                                  right_node.output_descriptor_ids.end());
  if (root.output_descriptor_ids.empty() ||
      root.output_descriptor_ids != concatenated_descriptors ||
      left.result_bindings.size() != left.batch.columns.size() ||
      right.result_bindings.size() != right.batch.columns.size()) {
    result.detail = "inner-join output does not concatenate both bound inputs";
    return result;
  }
  if (std::ranges::any_of(dag.outputs, [&](const auto& output) {
        return output.relation_node_id == root.logical_node_id;
      })) {
    result.detail =
        "inner-join root output lineage is not admitted by this profile";
    return result;
  }

  std::size_t published_ordinal = 0;
  const auto append_bindings =
      [&](const std::vector<exec::CanonicalResultColumnBinding>& bindings,
          const std::size_t physical_base) {
    for (const auto& source : bindings) {
      auto binding = source;
      binding.physical_column_ordinal += physical_base;
      if (binding.visible) {
        if (!binding.published_descriptor.has_value()) return false;
        binding.published_descriptor->ordinal =
            static_cast<std::uint32_t>(published_ordinal++);
      }
      result.result_bindings.push_back(std::move(binding));
    }
    return true;
  };
  if (!append_bindings(left.result_bindings, 0) ||
      !append_bindings(right.result_bindings, left.batch.columns.size())) {
    result.result_bindings.clear();
    result.detail = "inner-join visible result binding is incomplete";
    return result;
  }
  result.ok = true;
  return result;
}

PreparedSetOperationRoot PrepareOrdinalSetOperationRoot(
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalRelationalNode& root,
    const MaterializedValues& left,
    const MaterializedValues& right) {
  PreparedSetOperationRoot result;
  if (root.output_descriptor_ids.empty() ||
      left.batch.columns.size() != root.output_descriptor_ids.size() ||
      right.batch.columns.size() != root.output_descriptor_ids.size() ||
      left.result_bindings.size() != root.output_descriptor_ids.size()) {
    result.detail = "set-operation input/result arity is inconsistent";
    return result;
  }
  if (std::ranges::any_of(dag.outputs, [&](const auto& output) {
        return output.relation_node_id == root.logical_node_id;
      })) {
    result.detail =
        "set-operation root output lineage is not admitted by this ordinal profile";
    return result;
  }

  std::unordered_map<std::uint32_t, const api::RelationalTypeDescriptor*>
      descriptors;
  for (const auto& descriptor : dag.descriptors) {
    descriptors.emplace(descriptor.descriptor_id, &descriptor);
  }

  std::size_t published_ordinal = 0;
  for (std::size_t column = 0; column < root.output_descriptor_ids.size();
       ++column) {
    const auto descriptor = descriptors.find(root.output_descriptor_ids[column]);
    const auto& left_column = left.batch.columns[column];
    const auto& right_column = right.batch.columns[column];
    if (descriptor == descriptors.end() ||
        descriptor->second->nullability == api::RelationalNullability::kUnknown ||
        descriptor->second->collation_uuid.has_value() ||
        descriptor->second->timezone_profile_id.has_value() ||
        left_column.descriptor.canonical_type_name.empty() ||
        left_column.descriptor.canonical_type_name !=
            right_column.descriptor.canonical_type_name) {
      result.detail =
          "set-operation exact ordinal descriptor reconciliation is unresolved";
      return result;
    }

    api::EngineDescriptor engine_descriptor;
    engine_descriptor.descriptor_uuid.canonical =
        descriptor->second->descriptor_uuid;
    engine_descriptor.descriptor_kind = "scalar";
    engine_descriptor.canonical_type_name =
        left_column.descriptor.canonical_type_name;
    engine_descriptor.encoded_descriptor =
        "type_uuid=" + descriptor->second->type_uuid + ";nullability=" +
        (descriptor->second->nullability ==
                 api::RelationalNullability::kNullable
             ? "nullable"
             : "non_null");
    if (descriptor->second->width.has_value()) {
      engine_descriptor.encoded_descriptor +=
          ";width=" + std::to_string(*descriptor->second->width);
    }
    if (descriptor->second->precision.has_value()) {
      engine_descriptor.encoded_descriptor +=
          ";precision=" + std::to_string(*descriptor->second->precision);
    }
    if (descriptor->second->scale.has_value()) {
      engine_descriptor.encoded_descriptor +=
          ";scale=" + std::to_string(*descriptor->second->scale);
    }
    if (engine_descriptor.encoded_descriptor !=
            left_column.descriptor.encoded_descriptor ||
        engine_descriptor.encoded_descriptor !=
            right_column.descriptor.encoded_descriptor ||
        (descriptor->second->nullability ==
             api::RelationalNullability::kNullable) !=
            (left_column.nullable || right_column.nullable)) {
      result.detail =
          "set-operation exact ordinal descriptors do not share one bound type";
      return result;
    }

    result.result_columns.push_back(
        {left_column.stable_name, engine_descriptor,
         descriptor->second->nullability ==
             api::RelationalNullability::kNullable,
         descriptor->second->descriptor_id});
    auto binding = left.result_bindings[column];
    if (binding.visible) {
      binding.published_descriptor = exec::CanonicalResultColumnDescriptor{
          static_cast<std::uint32_t>(published_ordinal++),
          left_column.stable_name,
          descriptor->second->descriptor_uuid,
          descriptor->second->type_uuid,
          ResultNullability(descriptor->second->nullability),
          std::nullopt,
          std::nullopt};
    }
    result.result_bindings.push_back(std::move(binding));
  }
  result.ok = true;
  return result;
}

bool AddBatchMemoryBytes(const exec::DescriptorBatch& batch,
                         std::uint64_t* memory_bytes) {
  if (memory_bytes == nullptr) return false;
  for (const auto& row : batch.rows) {
    for (const auto& value : row.values) {
      if (value.encoded_value.size() >
          std::numeric_limits<std::uint64_t>::max() - *memory_bytes) {
        return false;
      }
      *memory_bytes += value.encoded_value.size();
    }
  }
  return true;
}

bool CheckedAdd(const std::uint64_t left, const std::uint64_t right,
                std::uint64_t* result) {
  if (result == nullptr ||
      right > std::numeric_limits<std::uint64_t>::max() - left) {
    return false;
  }
  *result = left + right;
  return true;
}

bool CheckedMultiply(const std::uint64_t left, const std::uint64_t right,
                     std::uint64_t* result) {
  if (result == nullptr ||
      (left != 0 && right >
                        std::numeric_limits<std::uint64_t>::max() / left)) {
    return false;
  }
  *result = left * right;
  return true;
}

bool EvaluateNonNegativeRowBound(
    CanonicalRelationalExpressionRuntime* runtime,
    const std::uint32_t expression_id,
    std::uint64_t* row_bound,
    std::string* refusal_detail) {
  if (runtime == nullptr || row_bound == nullptr || refusal_detail == nullptr) {
    return false;
  }
  api::EngineTypedValue value;
  if (!runtime->Evaluate(expression_id, "int64", &value, refusal_detail)) {
    return false;
  }
  if (value.state != api::EngineValueState::value || value.is_null ||
      value.descriptor.canonical_type_name != "int64" ||
      value.encoded_value.empty()) {
    *refusal_detail = "row bound is not a non-NULL canonical int64 value";
    return false;
  }
  std::int64_t decoded = 0;
  const auto [end, error] = std::from_chars(
      value.encoded_value.data(),
      value.encoded_value.data() + value.encoded_value.size(), decoded);
  if (error != std::errc{} ||
      end != value.encoded_value.data() + value.encoded_value.size() ||
      decoded < 0) {
    *refusal_detail = "row bound is negative or outside exact int64 admission";
    return false;
  }
  *row_bound = static_cast<std::uint64_t>(decoded);
  return true;
}

MaterializedValues MaterializeValues(
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalRelationalNode& logical_node) {
  MaterializedValues result;
  const auto node_it = std::ranges::find_if(
      dag.nodes, [&](const auto& node) {
        return node.node_id == logical_node.logical_node_id;
      });
  if (node_it == dag.nodes.end() ||
      node_it->node_kind != api::RelationalDagNodeKind::kValues ||
      !node_it->input_node_ids.empty() || node_it->values_row_ids.empty() ||
      node_it->output_descriptor_ids.empty()) {
    result.detail = "live VALUES root shape is incomplete";
    return result;
  }

  std::unordered_map<std::uint32_t, const api::RelationalTypeDescriptor*>
      descriptors;
  std::unordered_map<std::uint32_t, const api::RelationalExpressionRecord*>
      expressions;
  std::unordered_map<std::uint32_t, const api::RelationalValuesRowRecord*> rows;
  for (const auto& descriptor : dag.descriptors) {
    descriptors.emplace(descriptor.descriptor_id, &descriptor);
  }
  for (const auto& expression : dag.expressions) {
    expressions.emplace(expression.expression_id, &expression);
  }
  for (const auto& row : dag.values_rows) rows.emplace(row.row_id, &row);
  CanonicalRelationalExpressionRuntime expression_runtime(dag);

  std::vector<const api::RelationalOutputRecord*> outputs;
  for (const auto& output : dag.outputs) {
    if (output.relation_node_id == node_it->node_id) outputs.push_back(&output);
  }
  std::ranges::sort(outputs, {}, &api::RelationalOutputRecord::ordinal);
  if (outputs.size() != node_it->output_descriptor_ids.size()) {
    result.detail = "live VALUES result output coverage is incomplete";
    return result;
  }

  std::vector<std::string> type_names(node_it->output_descriptor_ids.size());
  for (const auto row_id : node_it->values_row_ids) {
    const auto row = rows.find(row_id);
    if (row == rows.end() ||
        row->second->expression_ids.size() != type_names.size()) {
      result.detail = "live VALUES row width is inconsistent";
      return result;
    }
    for (std::size_t column = 0; column < type_names.size(); ++column) {
      const auto expression =
          expressions.find(row->second->expression_ids[column]);
      if (expression == expressions.end() ||
          expression->second->result_descriptor_id !=
              node_it->output_descriptor_ids[column]) {
        result.detail =
            "live VALUES expression result descriptor is not column-bound";
        return result;
      }
      std::string type_name;
      if (!expression_runtime.InferType(expression->first, std::nullopt,
                                        &type_name, &result.detail)) {
        return result;
      }
      if (type_name == "null") continue;
      if (!type_names[column].empty() && type_names[column] != type_name) {
        result.detail = "live VALUES column has unreconciled literal types";
        return result;
      }
      type_names[column] = type_name;
    }
  }

  for (const auto row_id : node_it->values_row_ids) {
    const auto* row = rows.at(row_id);
    for (std::size_t column = 0; column < type_names.size(); ++column) {
      std::string reconciled_type;
      if (type_names[column].empty() ||
          !expression_runtime.InferType(row->expression_ids[column],
                                        type_names[column], &reconciled_type,
                                        &result.detail)) {
        if (result.detail.empty()) {
          result.detail = "live VALUES column type is unresolved";
        }
        return result;
      }
    }
  }

  std::size_t published_ordinal = 0;
  for (std::size_t column = 0; column < type_names.size(); ++column) {
    const auto descriptor =
        descriptors.find(node_it->output_descriptor_ids[column]);
    if (descriptor == descriptors.end() || type_names[column].empty() ||
        descriptor->second->nullability ==
            api::RelationalNullability::kUnknown ||
        (descriptor->second->collation_uuid.has_value() &&
         type_names[column] != "text") ||
        (descriptor->second->timezone_profile_id.has_value() &&
         type_names[column] != "timestamp") ||
        outputs[column]->ordinal != column ||
        outputs[column]->descriptor_id !=
            node_it->output_descriptor_ids[column] ||
        outputs[column]->output_name_utf8.empty()) {
      result.detail = "live VALUES descriptor or output binding is unresolved";
      return result;
    }
    api::EngineDescriptor engine_descriptor;
    engine_descriptor.descriptor_uuid.canonical =
        descriptor->second->descriptor_uuid;
    engine_descriptor.descriptor_kind = "scalar";
    engine_descriptor.canonical_type_name = type_names[column];
    engine_descriptor.encoded_descriptor =
        "type_uuid=" + descriptor->second->type_uuid + ";nullability=" +
        (descriptor->second->nullability ==
                 api::RelationalNullability::kNullable
             ? "nullable"
             : "non_null");
    if (descriptor->second->collation_uuid.has_value()) {
      engine_descriptor.encoded_descriptor +=
          ";collation_uuid=" + *descriptor->second->collation_uuid;
    }
    if (descriptor->second->timezone_profile_id.has_value()) {
      engine_descriptor.encoded_descriptor +=
          ";timezone_profile_id=" + *descriptor->second->timezone_profile_id;
    }
    if (descriptor->second->width.has_value()) {
      engine_descriptor.encoded_descriptor +=
          ";width=" + std::to_string(*descriptor->second->width);
    }
    if (descriptor->second->precision.has_value()) {
      engine_descriptor.encoded_descriptor +=
          ";precision=" + std::to_string(*descriptor->second->precision);
    }
    if (descriptor->second->scale.has_value()) {
      engine_descriptor.encoded_descriptor +=
          ";scale=" + std::to_string(*descriptor->second->scale);
    }
    result.batch.columns.push_back(
        {outputs[column]->output_name_utf8, engine_descriptor,
         descriptor->second->nullability ==
             api::RelationalNullability::kNullable,
         descriptor->second->descriptor_id});

    exec::CanonicalResultColumnBinding binding;
    binding.physical_column_ordinal = column;
    binding.visible = outputs[column]->visible;
    if (binding.visible) {
      binding.published_descriptor = exec::CanonicalResultColumnDescriptor{
          static_cast<std::uint32_t>(published_ordinal++),
          outputs[column]->output_name_utf8,
          descriptor->second->descriptor_uuid,
          descriptor->second->type_uuid,
          ResultNullability(descriptor->second->nullability),
          descriptor->second->collation_uuid,
          descriptor->second->timezone_profile_id};
    }
    result.result_bindings.push_back(std::move(binding));
  }

  result.batch.rows.reserve(node_it->values_row_ids.size());
  for (const auto row_id : node_it->values_row_ids) {
    const auto* row = rows.at(row_id);
    exec::DescriptorTuple tuple;
    tuple.values.reserve(row->expression_ids.size());
    for (std::size_t column = 0; column < row->expression_ids.size(); ++column) {
      api::EngineTypedValue value;
      if (!expression_runtime.Evaluate(row->expression_ids[column],
                                       type_names[column], &value,
                                       &result.detail)) {
        result.batch = {};
        result.result_bindings.clear();
        return result;
      }
      tuple.values.push_back(std::move(value));
    }
    result.batch.rows.push_back(std::move(tuple));
  }
  const auto canonical = exec::ValidateCanonicalDescriptorBatch(
      result.batch, node_it->output_descriptor_ids);
  const auto values = exec::ValidateDescriptorBatch(result.batch);
  if (!canonical.ok || !values.ok) {
    result.batch = {};
    result.result_bindings.clear();
    result.detail = !canonical.ok ? canonical.diagnostic_code + ":" + canonical.detail
                                  : values.diagnostic_code + ":" + values.detail;
    return result;
  }
  result.ok = true;
  return result;
}

api::EngineApiResult SuccessfulApiResult(
    const CanonicalObjectFreeValuesExecutionRequest& request,
    const api::CanonicalOptimizerSelectedExecutionResult& execution) {
  api::EngineApiResult result;
  result.ok = true;
  result.operation_id = "query.execute";
  result.result_shape.result_kind = "rows";
  result.local_transaction_id = request.context.local_transaction_id;
  result.transaction_uuid = request.context.transaction_uuid;
  result.embedded_trust_mode_observed =
      request.context.trust_mode == api::EngineTrustMode::embedded_in_process;
  for (const auto& column : execution.result_publication.row_stream.columns) {
    result.result_shape.columns.push_back(column.descriptor);
  }
  for (const auto& row : execution.result_publication.row_stream.rows) {
    api::EngineRowValue api_row;
    for (std::size_t column = 0; column < row.values.size(); ++column) {
      api_row.fields.emplace_back(
          execution.result_publication.envelope.column_descriptors[column]
              .name_utf8,
          row.values[column]);
    }
    result.result_shape.rows.push_back(std::move(api_row));
  }
  result.evidence.push_back(
      {"canonical.selected_plan",
       execution.dispatch.selected_plan_uuid});
  result.evidence.push_back(
      {"canonical.result_abi", "QOW-RESULT-DIAGNOSTIC-ABI-V1"});
  return result;
}

struct LivePhysicalNodeProfile {
  std::uint32_t logical_node_id{0};
  std::string implementation_id;
  std::string capability_uuid;
  plan::CanonicalLogicalRelationalNodeKind logical_node_kind{
      plan::CanonicalLogicalRelationalNodeKind::kValues};
  exec::PhysicalNodeKind physical_node_kind{exec::PhysicalNodeKind::kValues};
  std::string transformation_rule_id;
  std::uint64_t estimated_rows{0};
  std::uint64_t memory_bytes_required{0};
  std::size_t minimum_input_count{0};
  std::size_t maximum_input_count{0};
  std::vector<std::string> required_property_uuids;
  std::vector<std::string> delivered_property_uuids;
  std::vector<plan::CanonicalLogicalPropertyKind> supported_property_kinds;
};

struct LivePhysicalPlanningResult {
  bool ok{false};
  exec::TypedPhysicalNodeDag physical_dag;
  std::string diagnostic_id;
  std::string detail;
};

LivePhysicalPlanningResult PlanAndPublishLivePhysicalDag(
    const CanonicalObjectFreeValuesExecutionRequest& request,
    const std::vector<LivePhysicalNodeProfile>& profiles,
    const std::string_view selected_plan_purpose,
    const std::string_view operation_name) {
  LivePhysicalPlanningResult result;
  const auto& graph = request.optimizer_request.logical_graph;
  if (profiles.size() != graph.nodes.size()) {
    result.diagnostic_id = "QOW-DIAG-OPTIMIZER-SEARCH-NO-PLAN-V1";
    result.detail = std::string(operation_name) +
                    " live profile does not cover every logical node";
    return result;
  }

  const auto identity_scope =
      graph.bound_sblr_tree_uuid + ":" + request.context.statement_uuid.canonical;
  const auto calibration_uuid =
      DerivedCanonicalUuid(identity_scope, "relational.calibration");
  plan::CanonicalPhysicalAlternativeCatalog alternatives;
  alternatives.bound_sblr_tree_uuid = graph.bound_sblr_tree_uuid;
  alternatives.catalog_epoch_uuid = graph.catalog_epoch_uuid;
  alternatives.security_context_uuid = graph.security_context_uuid;
  alternatives.local_transaction_id = graph.local_transaction_id;
  alternatives.statement_snapshot_id = graph.statement_snapshot_id;

  std::unordered_set<std::uint32_t> covered_nodes;
  std::unordered_set<std::string> published_capabilities;
  std::vector<opt::CanonicalOptimizerSearchCandidateInput> candidates;
  opt::CanonicalExecutorCapabilityCatalog capabilities;
  capabilities.capability_snapshot_uuid =
      request.optimizer_admission.capability_snapshot_uuid;
  capabilities.policy_epoch = request.optimizer_admission.policy_epoch;
  capabilities.engine_owned = true;
  for (const auto& profile : profiles) {
    const auto node = std::ranges::find_if(graph.nodes, [&](const auto& item) {
      return item.logical_node_id == profile.logical_node_id;
    });
    if (profile.logical_node_id == 0 || node == graph.nodes.end() ||
        node->node_kind != profile.logical_node_kind ||
        profile.implementation_id.empty() || profile.capability_uuid.empty() ||
        profile.transformation_rule_id.empty() ||
        profile.memory_bytes_required == 0 ||
        !covered_nodes.insert(profile.logical_node_id).second) {
      result.diagnostic_id = "QOW-DIAG-OPTIMIZER-SEARCH-NO-PLAN-V1";
      result.detail = std::string(operation_name) +
                      " live node profile is incomplete or inconsistent";
      return result;
    }
    const auto suffix = std::to_string(profile.logical_node_id);
    const auto alternative_uuid =
        DerivedCanonicalUuid(identity_scope, "alternative." + suffix);
    alternatives.alternatives.push_back(
        {alternative_uuid,
         profile.logical_node_id,
         profile.implementation_id,
         profile.capability_uuid,
         node->output_descriptor_ids,
         true,
         {},
         profile.required_property_uuids,
         profile.delivered_property_uuids});

    opt::CanonicalOptimizerSearchCandidateInput candidate;
    candidate.alternative_uuid = alternative_uuid;
    candidate.transformation_uuid =
        DerivedCanonicalUuid(identity_scope, "transformation." + suffix);
    candidate.transformation_rule_id = profile.transformation_rule_id;
    candidate.bound_sblr_tree_uuid = graph.bound_sblr_tree_uuid;
    candidate.statistics_snapshot_uuid =
        request.optimizer_admission.statistics_snapshot_uuid;
    candidate.statistics_generation =
        request.optimizer_admission.statistics_generation;
    candidate.model_family_id = "relational.local.v1";
    candidate.cost_terms.cost_vector_uuid =
        DerivedCanonicalUuid(identity_scope, "cost-vector." + suffix);
    candidate.cost_terms.calibration_profile_uuid = calibration_uuid;
    candidate.cost_terms.cpu_units =
        std::max<std::uint64_t>(1, profile.estimated_rows);
    candidate.cost_terms.memory_bytes_required =
        profile.memory_bytes_required;
    candidate.cost_terms.confidence = opt::CostConfidence::kHigh;
    candidate.semantic_preserving = true;
    candidate.derived_from_admitted_statistics = true;
    candidate.engine_coster_owned = true;
    candidates.push_back(std::move(candidate));

    if (published_capabilities.insert(profile.capability_uuid).second) {
      opt::CanonicalExecutorCapabilityRecord capability;
      capability.capability_uuid = profile.capability_uuid;
      capability.capability_abi_version = 1;
      capability.implementation_id = profile.implementation_id;
      capability.logical_node_kind = profile.logical_node_kind;
      capability.physical_node_kind = profile.physical_node_kind;
      capability.minimum_input_count = profile.minimum_input_count;
      capability.maximum_input_count = profile.maximum_input_count;
      capability.maximum_memory_bytes =
          request.optimizer_request.resource.memory_budget_bytes;
      capability.supported_property_kinds =
          profile.supported_property_kinds;
      capability.spill_supported = false;
      capability.available = true;
      capability.engine_owned = true;
      capabilities.capabilities.push_back(std::move(capability));
    }
  }

  opt::CanonicalOptimizerSearchPolicy search_policy;
  search_policy.maximum_exhaustive_plan_count = 1;
  search_policy.bounded_beam_width = 1;
  search_policy.deterministic_step_cost_ns = 1;
  search_policy.engine_owned = true;
  const auto search = opt::SearchCanonicalRelationalMemo(
      request.optimizer_request, request.optimizer_admission, alternatives,
      candidates, search_policy);
  if (!search.accepted || !search.selected || !search.issues.empty()) {
    result.diagnostic_id =
        search.issues.empty() ? "QOW-DIAG-OPTIMIZER-SEARCH-NO-PLAN-V1"
                              : search.issues.front().diagnostic_id;
    result.detail = search.issues.empty()
                        ? std::string(operation_name) +
                              " live search returned no selected plan"
                        : search.issues.front().field_id;
    return result;
  }

  opt::CanonicalOptimizerPhysicalPublicationIdentity publication_identity;
  publication_identity.selected_plan_uuid =
      DerivedCanonicalUuid(identity_scope, selected_plan_purpose);
  publication_identity.first_causal_counter_id = 1;
  publication_identity.engine_owned = true;
  const auto publication = opt::PublishCanonicalPhysicalDag(
      request.optimizer_request, request.optimizer_admission, alternatives,
      search, capabilities, publication_identity);
  if (!publication.accepted || !publication.published ||
      !publication.issues.empty()) {
    result.diagnostic_id =
        publication.issues.empty()
            ? "QOW-DIAG-OPTIMIZER-PHYSICAL-PUBLICATION-V1"
            : publication.issues.front().diagnostic_id;
    result.detail = publication.issues.empty()
                        ? std::string(operation_name) +
                              " live physical DAG was not published"
                        : publication.issues.front().field_id;
    return result;
  }
  result.ok = true;
  result.physical_dag = publication.physical_dag;
  return result;
}

exec::CanonicalPhysicalExecutorRegistration MakeLiveValuesRegistration(
    std::unordered_map<std::uint64_t, exec::DescriptorBatch> batches,
    std::string capability_uuid,
    std::string diagnostic_id,
    std::string operation_name) {
  exec::CanonicalPhysicalExecutorRegistration registration;
  registration.node_kind = exec::PhysicalNodeKind::kValues;
  registration.implementation_id = std::string(kValuesImplementationId);
  registration.executor_capability_uuid = std::move(capability_uuid);
  registration.executor_capability_abi_version = 1;
  registration.engine_owned = true;
  registration.accepts_optimizer_publication_v2 = true;
  registration.execute =
      [batches = std::move(batches), diagnostic_id = std::move(diagnostic_id),
       operation_name = std::move(operation_name)](
          const exec::TypedPhysicalNodeDag& dag,
          const exec::PhysicalNodeRecord& node,
          const std::vector<exec::CanonicalPhysicalDispatchInput>& inputs) {
        exec::CanonicalPhysicalDispatchStepResult step;
        step.selected_plan_uuid = dag.selected_plan_uuid;
        step.executed_physical_node_id = node.physical_node_id;
        step.causal_counter_id = node.causal_counter_id;
        step.output_descriptor_ids = node.output_descriptor_ids;
        step.authority.engine_mga_snapshot_bound = true;
        const auto batch = batches.find(node.relational_node_id);
        if (!inputs.empty() || batch == batches.end()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code = diagnostic_id;
          step.diagnostic.detail = operation_name +
                                   " VALUES executor input or payload identity differs";
          return step;
        }
        step.result_handle_id = node.physical_node_id;
        step.output_row_count = batch->second.rows.size();
        step.rows_examined = batch->second.rows.size();
        step.materialized_output_batch = batch->second;
        return step;
      };
  return registration;
}

CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalObjectFreeUnionAllQuery(
    const CanonicalObjectFreeValuesExecutionRequest& request) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto& graph = request.optimizer_request.logical_graph;
  const auto root = std::ranges::find_if(graph.nodes, [&](const auto& node) {
    return node.logical_node_id == graph.root_logical_node_id;
  });
  if (graph.nodes.size() != 3 || root == graph.nodes.end() ||
      root->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kSetOperation ||
      root->semantic_variant_id != "set-operation.union-all.v1" ||
      root->input_logical_node_ids.size() != 2 ||
      root->input_logical_node_ids[0] == root->input_logical_node_ids[1] ||
      !root->bound_expression_ids.empty() ||
      !request.optimizer_request.logical_properties.properties.empty()) {
    return result;
  }
  const auto find_node = [&](const std::uint32_t node_id) {
    return std::ranges::find_if(graph.nodes, [&](const auto& node) {
      return node.logical_node_id == node_id;
    });
  };
  const auto left_node = find_node(root->input_logical_node_ids[0]);
  const auto right_node = find_node(root->input_logical_node_ids[1]);
  if (left_node == graph.nodes.end() || right_node == graph.nodes.end() ||
      left_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kValues ||
      right_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kValues ||
      left_node->semantic_variant_id != "values.literal-table.v1" ||
      right_node->semantic_variant_id != "values.literal-table.v1") {
    return result;
  }
  for (const auto& node : graph.nodes) {
    const bool values =
        node.node_kind == plan::CanonicalLogicalRelationalNodeKind::kValues;
    if (!node.required_object_uuids.empty() ||
        !node.required_property_uuids.empty() ||
        !node.delivered_property_uuids.empty() ||
        (values && !node.input_logical_node_ids.empty())) {
      return result;
    }
  }
  result.profile_matched = true;
  const auto refuse = [&](std::string diagnostic_id, std::string detail) {
    result.optimizer_selected = false;
    result.physical_dag_published = false;
    result.physical_dag_executed = false;
    result.runtime_actuals_attached = false;
    result.canonical_result_published = false;
    result.physical_node_count = 0;
    result.canonical_result_column_count = 0;
    result.canonical_result_row_count = 0;
    result.selected_plan_uuid.clear();
    result.canonical_result_bytes.clear();
    result.api_result =
        Failure(request, std::move(diagnostic_id), std::move(detail));
    return result;
  };
  if (!request.optimizer_admission.admitted ||
      !request.optimizer_admission.planning_allowed) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-SET-ADMISSION-V1",
                  "live UNION ALL execution lacks optimizer admission");
  }

  auto left = MaterializeValues(request.relational_dag, *left_node);
  if (!left.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-SET-PAYLOAD-V1",
                  "left VALUES: " + left.detail);
  }
  auto right = MaterializeValues(request.relational_dag, *right_node);
  if (!right.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-SET-PAYLOAD-V1",
                  "right VALUES: " + right.detail);
  }
  auto prepared_root = PrepareOrdinalSetOperationRoot(
      request.relational_dag, *root, left, right);
  if (!prepared_root.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-SET-PAYLOAD-V1",
                  prepared_root.detail);
  }

  if (right.batch.rows.size() >
      std::numeric_limits<std::size_t>::max() - left.batch.rows.size()) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "live UNION ALL row bound overflowed");
  }
  const auto set_output_row_bound =
      left.batch.rows.size() + right.batch.rows.size();

  std::uint64_t memory_bytes = 1;
  if (!AddBatchMemoryBytes(left.batch, &memory_bytes) ||
      !AddBatchMemoryBytes(right.batch, &memory_bytes)) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "live UNION ALL materialization size overflowed");
  }
  if (memory_bytes > request.optimizer_request.resource.memory_budget_bytes) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "live UNION ALL inputs exceed the admitted memory budget");
  }

  const auto identity_scope =
      graph.bound_sblr_tree_uuid + ":" + request.context.statement_uuid.canonical;
  const auto values_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "values.capability");
  const auto set_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "union-all.capability");
  std::vector<LivePhysicalNodeProfile> profiles;
  for (const auto& node : graph.nodes) {
    const bool values =
        node.node_kind == plan::CanonicalLogicalRelationalNodeKind::kValues;
    std::uint64_t node_rows = set_output_row_bound;
    std::uint64_t node_memory = memory_bytes;
    if (node.logical_node_id == left_node->logical_node_id) {
      node_rows = left.batch.rows.size();
      node_memory = 1;
      if (!AddBatchMemoryBytes(left.batch, &node_memory)) {
        return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                      "left VALUES cost size overflowed");
      }
    } else if (node.logical_node_id == right_node->logical_node_id) {
      node_rows = right.batch.rows.size();
      node_memory = 1;
      if (!AddBatchMemoryBytes(right.batch, &node_memory)) {
        return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                      "right VALUES cost size overflowed");
      }
    }
    profiles.push_back(
        {node.logical_node_id,
         values ? std::string(kValuesImplementationId)
                : std::string("setop.union-all.ordinal.typed.v1"),
         values ? values_capability_uuid : set_capability_uuid,
         node.node_kind,
         values ? exec::PhysicalNodeKind::kValues
                : exec::PhysicalNodeKind::kSetOperation,
         values ? "canonical.values.materialize.v1"
                : "canonical.setop.union-all.ordinal.v1",
         node_rows,
         node_memory,
         values ? 0U : 2U,
         values ? 0U : 2U});
  }
  const auto planning = PlanAndPublishLivePhysicalDag(
      request, profiles, "set.selected-plan", "UNION ALL");
  if (!planning.ok) {
    return refuse(planning.diagnostic_id, planning.detail);
  }
  result.optimizer_selected = true;
  result.physical_dag_published = true;
  result.physical_node_count = planning.physical_dag.nodes.size();
  result.selected_plan_uuid = planning.physical_dag.selected_plan_uuid;

  std::unordered_map<std::uint64_t, exec::DescriptorBatch> values_batches;
  values_batches.emplace(left_node->logical_node_id, std::move(left.batch));
  values_batches.emplace(right_node->logical_node_id, std::move(right.batch));
  auto values_registration = MakeLiveValuesRegistration(
      std::move(values_batches), values_capability_uuid,
      "QOW-DIAG-RELATIONAL-LIVE-SET-VALUES-V1", "UNION ALL");

  exec::CanonicalPhysicalExecutorRegistration set_registration;
  set_registration.node_kind = exec::PhysicalNodeKind::kSetOperation;
  set_registration.implementation_id = "setop.union-all.ordinal.typed.v1";
  set_registration.executor_capability_uuid = set_capability_uuid;
  set_registration.executor_capability_abi_version = 1;
  set_registration.engine_owned = true;
  set_registration.accepts_optimizer_publication_v2 = true;
  set_registration.execute =
      [result_columns = prepared_root.result_columns, set_output_row_bound](
          const exec::TypedPhysicalNodeDag& dag,
          const exec::PhysicalNodeRecord& node,
          const std::vector<exec::CanonicalPhysicalDispatchInput>& inputs) {
        exec::CanonicalPhysicalDispatchStepResult step;
        step.selected_plan_uuid = dag.selected_plan_uuid;
        step.executed_physical_node_id = node.physical_node_id;
        step.causal_counter_id = node.causal_counter_id;
        step.output_descriptor_ids = node.output_descriptor_ids;
        step.authority.engine_mga_snapshot_bound = true;
        if (inputs.size() != 2 ||
            !inputs[0].materialized_output_batch.has_value() ||
            !inputs[1].materialized_output_batch.has_value()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-SET-INPUT-V1";
          step.diagnostic.detail =
              "UNION ALL executor did not receive two typed input batches";
          return step;
        }
        exec::CanonicalSetOperationAllRequest set_request;
        set_request.physical_dag = dag;
        set_request.selected_physical_node_id = node.physical_node_id;
        set_request.left_batch = *inputs[0].materialized_output_batch;
        set_request.right_batch = *inputs[1].materialized_output_batch;
        set_request.result_columns = result_columns;
        set_request.operation = exec::CanonicalSetOperationKind::kUnion;
        set_request.alignment = exec::CanonicalSetOperationAlignment::kOrdinal;
        set_request.quantifier = exec::CanonicalSetOperationQuantifier::kAll;
        set_request.equality_profile =
            exec::CanonicalSetOperationEqualityProfile::kExactTyped;
        set_request.type_profile =
            exec::CanonicalSetOperationTypeProfile::kExact;
        set_request.maximum_output_row_count =
            std::max<std::size_t>(1, set_output_row_bound);
        const auto set_result =
            exec::ExecuteCanonicalSetOperationAll(set_request);
        if (!set_result.diagnostic.ok) {
          step.diagnostic = set_result.diagnostic;
          return step;
        }
        step.result_handle_id = node.physical_node_id;
        step.input_row_count = set_result.left_input_row_count +
                               set_result.right_input_row_count;
        step.rows_examined = step.input_row_count;
        step.output_row_count = set_result.output_batch.rows.size();
        step.materialized_output_batch = set_result.output_batch;
        return step;
      };

  api::CanonicalOptimizerSelectedExecutionRequest execution_request;
  execution_request.selected_physical_dag = planning.physical_dag;
  execution_request.pre_access_statistics_snapshot_uuid =
      planning.physical_dag.statistics_snapshot_uuid;
  execution_request.inventory_local_transaction_id =
      planning.physical_dag.local_transaction_id;
  execution_request.inventory_statement_snapshot_id =
      planning.physical_dag.statement_snapshot_id;
  execution_request.available_executors.push_back(
      std::move(values_registration));
  execution_request.available_executors.push_back(std::move(set_registration));
  execution_request.engine_execution_authorized = true;
  execution_request.result_publication_request.statement_uuid =
      request.context.statement_uuid.canonical;
  execution_request.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" + request.context.current_monotonic_ns,
          "set.execution-attempt");
  execution_request.result_publication_request.transaction_effect_evidence_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" +
              std::to_string(request.context.local_transaction_id) + ":" +
              std::to_string(
                  request.context.snapshot_visible_through_local_transaction_id),
          "set.transaction-effect-unchanged");
  execution_request.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  execution_request.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  execution_request.result_publication_request.column_bindings =
      std::move(prepared_root.result_bindings);
  execution_request.result_publication_request.maximum_row_count =
      std::max<std::size_t>(1, set_output_row_bound);

  const auto execution =
      api::ExecuteCanonicalOptimizerSelectedDag(execution_request);
  if (!execution.accepted || !execution.exact_selected_nodes_executed ||
      !execution.causal_counters_attached ||
      !execution.canonical_result_published || !execution.issues.empty()) {
    return refuse(
        execution.issues.empty()
            ? "QOW-DIAG-RELATIONAL-LIVE-SET-EXECUTION-V1"
            : execution.issues.front().diagnostic_id,
        execution.issues.empty()
            ? "live UNION ALL selected DAG was not completed"
            : execution.issues.front().field_id);
  }
  result.physical_dag_executed = true;
  result.runtime_actuals_attached = execution.runtime_actuals.accepted;
  result.canonical_result_published = execution.result_publication.published;
  result.canonical_result_column_count =
      execution.result_publication.envelope.column_descriptors.size();
  result.canonical_result_row_count =
      execution.result_publication.row_stream.rows.size();
  result.canonical_result_bytes =
      execution.result_publication.canonical_envelope_bytes;
  result.api_result = SuccessfulApiResult(request, execution);
  return result;
}

CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalObjectFreeInnerJoinQuery(
    const CanonicalObjectFreeValuesExecutionRequest& request) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto& graph = request.optimizer_request.logical_graph;
  const auto root = std::ranges::find_if(graph.nodes, [&](const auto& node) {
    return node.logical_node_id == graph.root_logical_node_id;
  });
  if (graph.nodes.size() != 3 || root == graph.nodes.end() ||
      root->node_kind != plan::CanonicalLogicalRelationalNodeKind::kJoin ||
      root->semantic_variant_id != "join.inner.v1" ||
      root->input_logical_node_ids.size() != 2 ||
      root->input_logical_node_ids[0] == root->input_logical_node_ids[1] ||
      root->bound_expression_ids.size() != 1 ||
      !request.optimizer_request.logical_properties.properties.empty()) {
    return result;
  }
  const auto find_node = [&](const std::uint32_t node_id) {
    return std::ranges::find_if(graph.nodes, [&](const auto& node) {
      return node.logical_node_id == node_id;
    });
  };
  const auto left_node = find_node(root->input_logical_node_ids[0]);
  const auto right_node = find_node(root->input_logical_node_ids[1]);
  if (left_node == graph.nodes.end() || right_node == graph.nodes.end() ||
      left_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kValues ||
      right_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kValues ||
      left_node->semantic_variant_id != "values.literal-table.v1" ||
      right_node->semantic_variant_id != "values.literal-table.v1") {
    return result;
  }
  for (const auto& node : graph.nodes) {
    const bool values =
        node.node_kind == plan::CanonicalLogicalRelationalNodeKind::kValues;
    if (!node.required_object_uuids.empty() ||
        !node.required_property_uuids.empty() ||
        !node.delivered_property_uuids.empty() ||
        (values && !node.input_logical_node_ids.empty())) {
      return result;
    }
  }
  result.profile_matched = true;
  const auto refuse = [&](std::string diagnostic_id, std::string detail) {
    result.optimizer_selected = false;
    result.physical_dag_published = false;
    result.physical_dag_executed = false;
    result.runtime_actuals_attached = false;
    result.canonical_result_published = false;
    result.physical_node_count = 0;
    result.canonical_result_column_count = 0;
    result.canonical_result_row_count = 0;
    result.selected_plan_uuid.clear();
    result.canonical_result_bytes.clear();
    result.api_result =
        Failure(request, std::move(diagnostic_id), std::move(detail));
    return result;
  };
  if (!request.optimizer_admission.admitted ||
      !request.optimizer_admission.planning_allowed) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-JOIN-ADMISSION-V1",
                  "live INNER JOIN execution lacks optimizer admission");
  }

  auto left = MaterializeValues(request.relational_dag, *left_node);
  if (!left.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-JOIN-PAYLOAD-V1",
                  "left VALUES: " + left.detail);
  }
  auto right = MaterializeValues(request.relational_dag, *right_node);
  if (!right.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-JOIN-PAYLOAD-V1",
                  "right VALUES: " + right.detail);
  }
  auto prepared_root = PrepareInnerJoinRoot(
      request.relational_dag, *root, *left_node, *right_node, left, right);
  if (!prepared_root.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-JOIN-PAYLOAD-V1",
                  prepared_root.detail);
  }

  api::EngineSqlTruthValue predicate_truth =
      api::EngineSqlTruthValue::unknown;
  std::string predicate_detail;
  CanonicalRelationalExpressionRuntime expression_runtime(
      request.relational_dag);
  if (!expression_runtime.EvaluatePredicate(root->bound_expression_ids.front(),
                                            &predicate_truth,
                                            &predicate_detail)) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-JOIN-PAYLOAD-V1",
                  "INNER JOIN predicate: " + predicate_detail);
  }

  const auto left_count = left.batch.rows.size();
  const auto right_count = right.batch.rows.size();
  if (left_count != 0 &&
      right_count > std::numeric_limits<std::size_t>::max() / left_count) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "live INNER JOIN pair cardinality overflowed");
  }
  const auto pair_count = left_count * right_count;
  const auto output_row_bound =
      predicate_truth == api::EngineSqlTruthValue::true_value ? pair_count : 0;

  std::uint64_t left_memory = 1;
  std::uint64_t right_memory = 1;
  if (!AddBatchMemoryBytes(left.batch, &left_memory) ||
      !AddBatchMemoryBytes(right.batch, &right_memory)) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "live INNER JOIN input size overflowed");
  }
  std::uint64_t total_memory = 0;
  std::uint64_t predicate_memory = 0;
  if (!CheckedAdd(left_memory, right_memory, &total_memory) ||
      !CheckedMultiply(pair_count, sizeof(api::EngineSqlTruthValue),
                       &predicate_memory) ||
      !CheckedAdd(total_memory, predicate_memory, &total_memory)) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "live INNER JOIN predicate state size overflowed");
  }
  if (output_row_bound != 0) {
    std::uint64_t repeated_left = 0;
    std::uint64_t repeated_right = 0;
    std::uint64_t output_memory = 0;
    if (!CheckedMultiply(left_memory, right_count, &repeated_left) ||
        !CheckedMultiply(right_memory, left_count, &repeated_right) ||
        !CheckedAdd(repeated_left, repeated_right, &output_memory) ||
        !CheckedAdd(output_memory, pair_count, &output_memory) ||
        !CheckedAdd(total_memory, output_memory, &total_memory)) {
      return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                    "live INNER JOIN output size overflowed");
    }
  }
  if (total_memory >
      request.optimizer_request.resource.memory_budget_bytes) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "live INNER JOIN exceeds the admitted memory budget");
  }

  const auto identity_scope =
      graph.bound_sblr_tree_uuid + ":" + request.context.statement_uuid.canonical;
  const auto values_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "values.capability");
  const auto join_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "inner-join.capability");
  std::vector<LivePhysicalNodeProfile> profiles;
  for (const auto& node : graph.nodes) {
    const bool values =
        node.node_kind == plan::CanonicalLogicalRelationalNodeKind::kValues;
    std::uint64_t node_rows = pair_count;
    std::uint64_t node_memory = total_memory;
    if (node.logical_node_id == left_node->logical_node_id) {
      node_rows = left_count;
      node_memory = left_memory;
    } else if (node.logical_node_id == right_node->logical_node_id) {
      node_rows = right_count;
      node_memory = right_memory;
    }
    profiles.push_back(
        {node.logical_node_id,
         values ? std::string(kValuesImplementationId)
                : std::string("join.inner.3vl.nested.v1"),
         values ? values_capability_uuid : join_capability_uuid,
         node.node_kind,
         values ? exec::PhysicalNodeKind::kValues
                : exec::PhysicalNodeKind::kJoin,
         values ? "canonical.values.materialize.v1"
                : "canonical.join.inner.3vl.nested.v1",
         node_rows,
         node_memory,
         values ? 0U : 2U,
         values ? 0U : 2U});
  }
  const auto planning = PlanAndPublishLivePhysicalDag(
      request, profiles, "join.selected-plan", "INNER JOIN");
  if (!planning.ok) {
    return refuse(planning.diagnostic_id, planning.detail);
  }
  result.optimizer_selected = true;
  result.physical_dag_published = true;
  result.physical_node_count = planning.physical_dag.nodes.size();
  result.selected_plan_uuid = planning.physical_dag.selected_plan_uuid;

  std::unordered_map<std::uint64_t, exec::DescriptorBatch> values_batches;
  values_batches.emplace(left_node->logical_node_id, std::move(left.batch));
  values_batches.emplace(right_node->logical_node_id, std::move(right.batch));
  auto values_registration = MakeLiveValuesRegistration(
      std::move(values_batches), values_capability_uuid,
      "QOW-DIAG-RELATIONAL-LIVE-JOIN-VALUES-V1", "INNER JOIN");

  exec::CanonicalPhysicalExecutorRegistration join_registration;
  join_registration.node_kind = exec::PhysicalNodeKind::kJoin;
  join_registration.implementation_id = "join.inner.3vl.nested.v1";
  join_registration.executor_capability_uuid = join_capability_uuid;
  join_registration.executor_capability_abi_version = 1;
  join_registration.engine_owned = true;
  join_registration.accepts_optimizer_publication_v2 = true;
  join_registration.execute =
      [predicate_truth, pair_count](
          const exec::TypedPhysicalNodeDag& dag,
          const exec::PhysicalNodeRecord& node,
          const std::vector<exec::CanonicalPhysicalDispatchInput>& inputs) {
        exec::CanonicalPhysicalDispatchStepResult step;
        step.selected_plan_uuid = dag.selected_plan_uuid;
        step.executed_physical_node_id = node.physical_node_id;
        step.causal_counter_id = node.causal_counter_id;
        step.output_descriptor_ids = node.output_descriptor_ids;
        step.authority.engine_mga_snapshot_bound = true;
        if (inputs.size() != 2 ||
            !inputs[0].materialized_output_batch.has_value() ||
            !inputs[1].materialized_output_batch.has_value()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-JOIN-INPUT-V1";
          step.diagnostic.detail =
              "INNER JOIN executor did not receive two typed input batches";
          return step;
        }
        const auto& left_batch = *inputs[0].materialized_output_batch;
        const auto& right_batch = *inputs[1].materialized_output_batch;
        if (left_batch.rows.size() != 0 &&
            right_batch.rows.size() >
                std::numeric_limits<std::size_t>::max() /
                    left_batch.rows.size()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-JOIN-INPUT-V1";
          step.diagnostic.detail = "INNER JOIN pair cardinality overflowed";
          return step;
        }
        const auto actual_pair_count =
            left_batch.rows.size() * right_batch.rows.size();
        if (actual_pair_count != pair_count ||
            right_batch.rows.size() >
                std::numeric_limits<std::size_t>::max() -
                    left_batch.rows.size()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-JOIN-INPUT-V1";
          step.diagnostic.detail =
              "INNER JOIN input cardinality differs or overflows its "
              "selected cost";
          return step;
        }
        exec::CanonicalDescriptorInnerJoinRequest join_request;
        join_request.physical_dag = dag;
        join_request.selected_physical_node_id = node.physical_node_id;
        join_request.left_batch = left_batch;
        join_request.right_batch = right_batch;
        join_request.pair_truth_values.assign(pair_count, predicate_truth);
        join_request.consumer = api::EnginePredicateConsumer::join_on;
        const auto join_result =
            exec::ExecuteCanonicalDescriptorInnerJoin(join_request);
        if (!join_result.diagnostic.ok) {
          step.diagnostic = join_result.diagnostic;
          return step;
        }
        step.result_handle_id = node.physical_node_id;
        step.input_row_count =
            left_batch.rows.size() + right_batch.rows.size();
        step.rows_examined = pair_count;
        step.output_row_count = join_result.output_batch.rows.size();
        step.materialized_output_batch = join_result.output_batch;
        return step;
      };

  api::CanonicalOptimizerSelectedExecutionRequest execution_request;
  execution_request.selected_physical_dag = planning.physical_dag;
  execution_request.pre_access_statistics_snapshot_uuid =
      planning.physical_dag.statistics_snapshot_uuid;
  execution_request.inventory_local_transaction_id =
      planning.physical_dag.local_transaction_id;
  execution_request.inventory_statement_snapshot_id =
      planning.physical_dag.statement_snapshot_id;
  execution_request.available_executors.push_back(
      std::move(values_registration));
  execution_request.available_executors.push_back(std::move(join_registration));
  execution_request.engine_execution_authorized = true;
  execution_request.result_publication_request.statement_uuid =
      request.context.statement_uuid.canonical;
  execution_request.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" + request.context.current_monotonic_ns,
          "join.execution-attempt");
  execution_request.result_publication_request.transaction_effect_evidence_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" +
              std::to_string(request.context.local_transaction_id) + ":" +
              std::to_string(
                  request.context.snapshot_visible_through_local_transaction_id),
          "join.transaction-effect-unchanged");
  execution_request.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  execution_request.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  execution_request.result_publication_request.column_bindings =
      std::move(prepared_root.result_bindings);
  execution_request.result_publication_request.maximum_row_count =
      std::max<std::size_t>(1, output_row_bound);

  const auto execution =
      api::ExecuteCanonicalOptimizerSelectedDag(execution_request);
  if (!execution.accepted || !execution.exact_selected_nodes_executed ||
      !execution.causal_counters_attached ||
      !execution.canonical_result_published || !execution.issues.empty()) {
    return refuse(
        execution.issues.empty()
            ? "QOW-DIAG-RELATIONAL-LIVE-JOIN-EXECUTION-V1"
            : execution.issues.front().diagnostic_id,
        execution.issues.empty()
            ? "live INNER JOIN selected DAG was not completed"
            : execution.issues.front().field_id);
  }
  result.physical_dag_executed = true;
  result.runtime_actuals_attached = execution.runtime_actuals.accepted;
  result.canonical_result_published = execution.result_publication.published;
  result.canonical_result_column_count =
      execution.result_publication.envelope.column_descriptors.size();
  result.canonical_result_row_count =
      execution.result_publication.row_stream.rows.size();
  result.canonical_result_bytes =
      execution.result_publication.canonical_envelope_bytes;
  result.api_result = SuccessfulApiResult(request, execution);
  return result;
}

CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalObjectFreeFilterQuery(
    const CanonicalObjectFreeValuesExecutionRequest& request) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto& graph = request.optimizer_request.logical_graph;
  const auto root = std::ranges::find_if(graph.nodes, [&](const auto& node) {
    return node.logical_node_id == graph.root_logical_node_id;
  });
  if (graph.nodes.size() != 2 || root == graph.nodes.end() ||
      root->node_kind != plan::CanonicalLogicalRelationalNodeKind::kFilter ||
      root->semantic_variant_id != "filter.where.v1" ||
      root->input_logical_node_ids.size() != 1 ||
      root->bound_expression_ids.size() != 1 ||
      !request.optimizer_request.logical_properties.properties.empty()) {
    return result;
  }
  const auto input_node =
      std::ranges::find_if(graph.nodes, [&](const auto& node) {
        return node.logical_node_id == root->input_logical_node_ids.front();
      });
  if (input_node == graph.nodes.end() || input_node == root ||
      input_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kValues ||
      input_node->semantic_variant_id != "values.literal-table.v1" ||
      !input_node->input_logical_node_ids.empty()) {
    return result;
  }
  for (const auto& node : graph.nodes) {
    if (!node.required_object_uuids.empty() ||
        !node.required_property_uuids.empty() ||
        !node.delivered_property_uuids.empty()) {
      return result;
    }
  }
  result.profile_matched = true;
  const auto refuse = [&](std::string diagnostic_id, std::string detail) {
    result.optimizer_selected = false;
    result.physical_dag_published = false;
    result.physical_dag_executed = false;
    result.runtime_actuals_attached = false;
    result.canonical_result_published = false;
    result.physical_node_count = 0;
    result.canonical_result_column_count = 0;
    result.canonical_result_row_count = 0;
    result.selected_plan_uuid.clear();
    result.canonical_result_bytes.clear();
    result.api_result =
        Failure(request, std::move(diagnostic_id), std::move(detail));
    return result;
  };
  if (!request.optimizer_admission.admitted ||
      !request.optimizer_admission.planning_allowed) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-FILTER-ADMISSION-V1",
                  "live FILTER execution lacks optimizer admission");
  }

  auto input = MaterializeValues(request.relational_dag, *input_node);
  if (!input.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-FILTER-PAYLOAD-V1",
                  "FILTER input VALUES: " + input.detail);
  }
  auto prepared_root = PrepareFilterRoot(request.relational_dag, *root,
                                         *input_node, input);
  if (!prepared_root.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-FILTER-PAYLOAD-V1",
                  prepared_root.detail);
  }

  api::EngineSqlTruthValue predicate_truth =
      api::EngineSqlTruthValue::unknown;
  std::string predicate_detail;
  CanonicalRelationalExpressionRuntime expression_runtime(
      request.relational_dag);
  if (!expression_runtime.EvaluatePredicate(root->bound_expression_ids.front(),
                                            &predicate_truth,
                                            &predicate_detail)) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-FILTER-PAYLOAD-V1",
                  "FILTER predicate: " + predicate_detail);
  }

  const auto input_row_count = input.batch.rows.size();
  const auto output_row_bound =
      predicate_truth == api::EngineSqlTruthValue::true_value
          ? input_row_count
          : 0;
  std::uint64_t input_memory = 1;
  if (!AddBatchMemoryBytes(input.batch, &input_memory)) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "live FILTER input size overflowed");
  }
  std::uint64_t predicate_memory = 0;
  std::uint64_t total_memory = 0;
  if (!CheckedMultiply(input_row_count, sizeof(api::EngineSqlTruthValue),
                       &predicate_memory) ||
      !CheckedAdd(input_memory, predicate_memory, &total_memory) ||
      (output_row_bound != 0 &&
       !CheckedAdd(total_memory, input_memory, &total_memory))) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "live FILTER predicate or output size overflowed");
  }
  if (total_memory >
      request.optimizer_request.resource.memory_budget_bytes) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "live FILTER exceeds the admitted memory budget");
  }

  const auto identity_scope =
      graph.bound_sblr_tree_uuid + ":" + request.context.statement_uuid.canonical;
  const auto values_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "values.capability");
  const auto filter_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "filter.capability");
  const std::vector<LivePhysicalNodeProfile> profiles = {
      {input_node->logical_node_id,
       std::string(kValuesImplementationId),
       values_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kValues,
       exec::PhysicalNodeKind::kValues,
       "canonical.values.materialize.v1",
       input_row_count,
       input_memory,
       0,
       0},
      {root->logical_node_id,
       "filter.3vl.row.v1",
       filter_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kFilter,
       exec::PhysicalNodeKind::kFilter,
       "canonical.filter.3vl.row.v1",
       input_row_count,
       total_memory,
       1,
       1}};
  const auto planning = PlanAndPublishLivePhysicalDag(
      request, profiles, "filter.selected-plan", "FILTER");
  if (!planning.ok) {
    return refuse(planning.diagnostic_id, planning.detail);
  }
  result.optimizer_selected = true;
  result.physical_dag_published = true;
  result.physical_node_count = planning.physical_dag.nodes.size();
  result.selected_plan_uuid = planning.physical_dag.selected_plan_uuid;

  std::unordered_map<std::uint64_t, exec::DescriptorBatch> values_batches;
  values_batches.emplace(input_node->logical_node_id, std::move(input.batch));
  auto values_registration = MakeLiveValuesRegistration(
      std::move(values_batches), values_capability_uuid,
      "QOW-DIAG-RELATIONAL-LIVE-FILTER-VALUES-V1", "FILTER");

  exec::CanonicalPhysicalExecutorRegistration filter_registration;
  filter_registration.node_kind = exec::PhysicalNodeKind::kFilter;
  filter_registration.implementation_id = "filter.3vl.row.v1";
  filter_registration.executor_capability_uuid = filter_capability_uuid;
  filter_registration.executor_capability_abi_version = 1;
  filter_registration.engine_owned = true;
  filter_registration.accepts_optimizer_publication_v2 = true;
  filter_registration.execute =
      [predicate_truth, input_row_count](
          const exec::TypedPhysicalNodeDag& dag,
          const exec::PhysicalNodeRecord& node,
          const std::vector<exec::CanonicalPhysicalDispatchInput>& inputs) {
        exec::CanonicalPhysicalDispatchStepResult step;
        step.selected_plan_uuid = dag.selected_plan_uuid;
        step.executed_physical_node_id = node.physical_node_id;
        step.causal_counter_id = node.causal_counter_id;
        step.output_descriptor_ids = node.output_descriptor_ids;
        step.authority.engine_mga_snapshot_bound = true;
        if (inputs.size() != 1 ||
            !inputs.front().materialized_output_batch.has_value()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-FILTER-INPUT-V1";
          step.diagnostic.detail =
              "FILTER executor did not receive one typed input batch";
          return step;
        }
        const auto& input_batch = *inputs.front().materialized_output_batch;
        if (input_batch.rows.size() != input_row_count) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-FILTER-INPUT-V1";
          step.diagnostic.detail =
              "FILTER input cardinality differs from its selected cost";
          return step;
        }
        exec::CanonicalDescriptorFilterRequest filter_request;
        filter_request.physical_dag = dag;
        filter_request.selected_physical_node_id = node.physical_node_id;
        filter_request.input_batch = input_batch;
        filter_request.row_truth_values.assign(input_row_count,
                                               predicate_truth);
        filter_request.consumer = api::EnginePredicateConsumer::filter;
        const auto filter_result =
            exec::ExecuteCanonicalDescriptorFilter(filter_request);
        if (!filter_result.diagnostic.ok) {
          step.diagnostic = filter_result.diagnostic;
          return step;
        }
        step.result_handle_id = node.physical_node_id;
        step.input_row_count = input_batch.rows.size();
        step.rows_examined = input_batch.rows.size();
        step.output_row_count = filter_result.output_batch.rows.size();
        step.materialized_output_batch = filter_result.output_batch;
        return step;
      };

  api::CanonicalOptimizerSelectedExecutionRequest execution_request;
  execution_request.selected_physical_dag = planning.physical_dag;
  execution_request.pre_access_statistics_snapshot_uuid =
      planning.physical_dag.statistics_snapshot_uuid;
  execution_request.inventory_local_transaction_id =
      planning.physical_dag.local_transaction_id;
  execution_request.inventory_statement_snapshot_id =
      planning.physical_dag.statement_snapshot_id;
  execution_request.available_executors.push_back(
      std::move(values_registration));
  execution_request.available_executors.push_back(
      std::move(filter_registration));
  execution_request.engine_execution_authorized = true;
  execution_request.result_publication_request.statement_uuid =
      request.context.statement_uuid.canonical;
  execution_request.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" + request.context.current_monotonic_ns,
          "filter.execution-attempt");
  execution_request.result_publication_request.transaction_effect_evidence_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" +
              std::to_string(request.context.local_transaction_id) + ":" +
              std::to_string(
                  request.context.snapshot_visible_through_local_transaction_id),
          "filter.transaction-effect-unchanged");
  execution_request.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  execution_request.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  execution_request.result_publication_request.column_bindings =
      std::move(prepared_root.result_bindings);
  execution_request.result_publication_request.maximum_row_count =
      std::max<std::size_t>(1, output_row_bound);

  const auto execution =
      api::ExecuteCanonicalOptimizerSelectedDag(execution_request);
  if (!execution.accepted || !execution.exact_selected_nodes_executed ||
      !execution.causal_counters_attached ||
      !execution.canonical_result_published || !execution.issues.empty()) {
    return refuse(
        execution.issues.empty()
            ? "QOW-DIAG-RELATIONAL-LIVE-FILTER-EXECUTION-V1"
            : execution.issues.front().diagnostic_id,
        execution.issues.empty()
            ? "live FILTER selected DAG was not completed"
            : execution.issues.front().field_id);
  }
  result.physical_dag_executed = true;
  result.runtime_actuals_attached = execution.runtime_actuals.accepted;
  result.canonical_result_published = execution.result_publication.published;
  result.canonical_result_column_count =
      execution.result_publication.envelope.column_descriptors.size();
  result.canonical_result_row_count =
      execution.result_publication.row_stream.rows.size();
  result.canonical_result_bytes =
      execution.result_publication.canonical_envelope_bytes;
  result.api_result = SuccessfulApiResult(request, execution);
  return result;
}

CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalObjectFreeProjectQuery(
    const CanonicalObjectFreeValuesExecutionRequest& request) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto& graph = request.optimizer_request.logical_graph;
  const auto root = std::ranges::find_if(graph.nodes, [&](const auto& node) {
    return node.logical_node_id == graph.root_logical_node_id;
  });
  if (graph.nodes.size() != 2 || root == graph.nodes.end() ||
      root->node_kind != plan::CanonicalLogicalRelationalNodeKind::kProject ||
      root->semantic_variant_id != "project.select-list.v1" ||
      root->input_logical_node_ids.size() != 1 ||
      !request.optimizer_request.logical_properties.properties.empty()) {
    return result;
  }
  const auto input_node =
      std::ranges::find_if(graph.nodes, [&](const auto& node) {
        return node.logical_node_id == root->input_logical_node_ids.front();
      });
  if (input_node == graph.nodes.end() || input_node == root ||
      input_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kValues ||
      input_node->semantic_variant_id != "values.literal-table.v1" ||
      !input_node->input_logical_node_ids.empty()) {
    return result;
  }
  for (const auto& node : graph.nodes) {
    if (!node.required_object_uuids.empty() ||
        !node.required_property_uuids.empty() ||
        !node.delivered_property_uuids.empty()) {
      return result;
    }
  }
  result.profile_matched = true;
  const auto refuse = [&](std::string diagnostic_id, std::string detail) {
    result.optimizer_selected = false;
    result.physical_dag_published = false;
    result.physical_dag_executed = false;
    result.runtime_actuals_attached = false;
    result.canonical_result_published = false;
    result.physical_node_count = 0;
    result.canonical_result_column_count = 0;
    result.canonical_result_row_count = 0;
    result.selected_plan_uuid.clear();
    result.canonical_result_bytes.clear();
    result.api_result =
        Failure(request, std::move(diagnostic_id), std::move(detail));
    return result;
  };
  if (!request.optimizer_admission.admitted ||
      !request.optimizer_admission.planning_allowed) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-PROJECT-ADMISSION-V1",
                  "live PROJECT execution lacks optimizer admission");
  }
  if (!root->bound_expression_ids.empty()) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-PROJECT-PAYLOAD-V1",
                  "descriptor-direct PROJECT does not admit bound expressions");
  }

  auto input = MaterializeValues(request.relational_dag, *input_node);
  if (!input.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-PROJECT-PAYLOAD-V1",
                  "PROJECT input VALUES: " + input.detail);
  }
  auto prepared_root = PrepareDescriptorDirectProjectRoot(
      request.relational_dag, *root, *input_node, input);
  if (!prepared_root.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-PROJECT-PAYLOAD-V1",
                  prepared_root.detail);
  }

  const auto input_row_count = input.batch.rows.size();
  std::uint64_t input_memory = 1;
  std::uint64_t total_memory = 0;
  if (!AddBatchMemoryBytes(input.batch, &input_memory) ||
      !CheckedAdd(input_memory, input_memory, &total_memory)) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "live PROJECT input or output size overflowed");
  }
  if (total_memory >
      request.optimizer_request.resource.memory_budget_bytes) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "live PROJECT exceeds the admitted memory budget");
  }

  const auto identity_scope =
      graph.bound_sblr_tree_uuid + ":" + request.context.statement_uuid.canonical;
  const auto values_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "values.capability");
  const auto project_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "project.capability");
  const std::vector<LivePhysicalNodeProfile> profiles = {
      {input_node->logical_node_id,
       std::string(kValuesImplementationId),
       values_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kValues,
       exec::PhysicalNodeKind::kValues,
       "canonical.values.materialize.v1",
       input_row_count,
       input_memory,
       0,
       0},
      {root->logical_node_id,
       "project.typed.row.v1",
       project_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kProject,
       exec::PhysicalNodeKind::kProject,
       "canonical.project.descriptor-direct.v1",
       input_row_count,
       total_memory,
       1,
       1}};
  const auto planning = PlanAndPublishLivePhysicalDag(
      request, profiles, "project.selected-plan", "PROJECT");
  if (!planning.ok) {
    return refuse(planning.diagnostic_id, planning.detail);
  }
  result.optimizer_selected = true;
  result.physical_dag_published = true;
  result.physical_node_count = planning.physical_dag.nodes.size();
  result.selected_plan_uuid = planning.physical_dag.selected_plan_uuid;

  std::unordered_map<std::uint64_t, exec::DescriptorBatch> values_batches;
  values_batches.emplace(input_node->logical_node_id, std::move(input.batch));
  auto values_registration = MakeLiveValuesRegistration(
      std::move(values_batches), values_capability_uuid,
      "QOW-DIAG-RELATIONAL-LIVE-PROJECT-VALUES-V1", "PROJECT");

  exec::CanonicalPhysicalExecutorRegistration project_registration;
  project_registration.node_kind = exec::PhysicalNodeKind::kProject;
  project_registration.implementation_id = "project.typed.row.v1";
  project_registration.executor_capability_uuid = project_capability_uuid;
  project_registration.executor_capability_abi_version = 1;
  project_registration.engine_owned = true;
  project_registration.accepts_optimizer_publication_v2 = true;
  project_registration.execute =
      [projected_columns = prepared_root.projected_columns, input_row_count](
          const exec::TypedPhysicalNodeDag& dag,
          const exec::PhysicalNodeRecord& node,
          const std::vector<exec::CanonicalPhysicalDispatchInput>& inputs) {
        exec::CanonicalPhysicalDispatchStepResult step;
        step.selected_plan_uuid = dag.selected_plan_uuid;
        step.executed_physical_node_id = node.physical_node_id;
        step.causal_counter_id = node.causal_counter_id;
        step.output_descriptor_ids = node.output_descriptor_ids;
        step.authority.engine_mga_snapshot_bound = true;
        if (inputs.size() != 1 ||
            !inputs.front().materialized_output_batch.has_value()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-PROJECT-INPUT-V1";
          step.diagnostic.detail =
              "PROJECT executor did not receive one typed input batch";
          return step;
        }
        const auto& input_batch = *inputs.front().materialized_output_batch;
        if (input_batch.rows.size() != input_row_count) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-PROJECT-INPUT-V1";
          step.diagnostic.detail =
              "PROJECT input cardinality differs from its selected cost";
          return step;
        }
        exec::CanonicalDescriptorProjectionRequest project_request;
        project_request.physical_dag = dag;
        project_request.selected_physical_node_id = node.physical_node_id;
        project_request.input_batch = input_batch;
        project_request.projected_columns = projected_columns;
        const auto project_result =
            exec::ExecuteCanonicalDescriptorProjection(project_request);
        if (!project_result.diagnostic.ok) {
          step.diagnostic = project_result.diagnostic;
          return step;
        }
        step.result_handle_id = node.physical_node_id;
        step.input_row_count = input_batch.rows.size();
        step.rows_examined = input_batch.rows.size();
        step.output_row_count = project_result.output_batch.rows.size();
        step.materialized_output_batch = project_result.output_batch;
        return step;
      };

  api::CanonicalOptimizerSelectedExecutionRequest execution_request;
  execution_request.selected_physical_dag = planning.physical_dag;
  execution_request.pre_access_statistics_snapshot_uuid =
      planning.physical_dag.statistics_snapshot_uuid;
  execution_request.inventory_local_transaction_id =
      planning.physical_dag.local_transaction_id;
  execution_request.inventory_statement_snapshot_id =
      planning.physical_dag.statement_snapshot_id;
  execution_request.available_executors.push_back(
      std::move(values_registration));
  execution_request.available_executors.push_back(
      std::move(project_registration));
  execution_request.engine_execution_authorized = true;
  execution_request.result_publication_request.statement_uuid =
      request.context.statement_uuid.canonical;
  execution_request.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" + request.context.current_monotonic_ns,
          "project.execution-attempt");
  execution_request.result_publication_request.transaction_effect_evidence_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" +
              std::to_string(request.context.local_transaction_id) + ":" +
              std::to_string(
                  request.context.snapshot_visible_through_local_transaction_id),
          "project.transaction-effect-unchanged");
  execution_request.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  execution_request.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  execution_request.result_publication_request.column_bindings =
      std::move(prepared_root.result_bindings);
  execution_request.result_publication_request.maximum_row_count =
      std::max<std::size_t>(1, input_row_count);

  const auto execution =
      api::ExecuteCanonicalOptimizerSelectedDag(execution_request);
  if (!execution.accepted || !execution.exact_selected_nodes_executed ||
      !execution.causal_counters_attached ||
      !execution.canonical_result_published || !execution.issues.empty()) {
    return refuse(
        execution.issues.empty()
            ? "QOW-DIAG-RELATIONAL-LIVE-PROJECT-EXECUTION-V1"
            : execution.issues.front().diagnostic_id,
        execution.issues.empty()
            ? "live PROJECT selected DAG was not completed"
            : execution.issues.front().field_id);
  }
  result.physical_dag_executed = true;
  result.runtime_actuals_attached = execution.runtime_actuals.accepted;
  result.canonical_result_published = execution.result_publication.published;
  result.canonical_result_column_count =
      execution.result_publication.envelope.column_descriptors.size();
  result.canonical_result_row_count =
      execution.result_publication.row_stream.rows.size();
  result.canonical_result_bytes =
      execution.result_publication.canonical_envelope_bytes;
  result.api_result = SuccessfulApiResult(request, execution);
  return result;
}

CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalObjectFreeGlobalAggregateQuery(
    const CanonicalObjectFreeValuesExecutionRequest& request) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto& graph = request.optimizer_request.logical_graph;
  const auto root = std::ranges::find_if(graph.nodes, [&](const auto& node) {
    return node.logical_node_id == graph.root_logical_node_id;
  });
  if (graph.nodes.size() != 2 || root == graph.nodes.end() ||
      root->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kAggregate ||
      root->input_logical_node_ids.size() != 1 ||
      root->bound_expression_ids.size() != 1 ||
      !request.optimizer_request.logical_properties.properties.empty()) {
    return result;
  }
  const bool count_star =
      root->semantic_variant_id == "aggregate.global-count-star.v1";
  const bool count_expression =
      root->semantic_variant_id == "aggregate.global-count-expression.v1";
  const bool sum_expression =
      root->semantic_variant_id == "aggregate.global-sum-expression.v1";
  const bool avg_expression =
      root->semantic_variant_id == "aggregate.global-avg-expression.v1";
  const bool min_expression =
      root->semantic_variant_id == "aggregate.global-min-expression.v1";
  const bool max_expression =
      root->semantic_variant_id == "aggregate.global-max-expression.v1";
  const bool bool_and_expression =
      root->semantic_variant_id == "aggregate.global-bool-and-expression.v1";
  const bool bool_or_expression =
      root->semantic_variant_id == "aggregate.global-bool-or-expression.v1";
  const bool every_expression =
      root->semantic_variant_id == "aggregate.global-every-expression.v1";
  const bool unordered_string_agg_expression =
      root->semantic_variant_id ==
      "aggregate.global-string-agg-expression.v1";
  const bool ordered_string_agg_expression =
      root->semantic_variant_id ==
      "aggregate.global-string-agg-ordered-expression.v1";
  const bool string_agg_expression =
      unordered_string_agg_expression || ordered_string_agg_expression;
  const bool listagg_ordered_expression =
      root->semantic_variant_id ==
      "aggregate.global-listagg-ordered-expression.v1";
  const bool listagg_overflow_error_expression =
      root->semantic_variant_id ==
      "aggregate.global-listagg-ordered-overflow-error-expression.v1";
  const bool listagg_overflow_truncate_expression =
      root->semantic_variant_id ==
      "aggregate.global-listagg-ordered-overflow-truncate-expression.v1";
  const bool listagg_expression =
      listagg_ordered_expression || listagg_overflow_error_expression ||
      listagg_overflow_truncate_expression;
  const bool array_agg_expression =
      root->semantic_variant_id ==
      "aggregate.global-array-agg-ordered-expression.v1";
  const bool json_agg_expression =
      root->semantic_variant_id ==
      "aggregate.global-json-agg-ordered-expression.v1";
  const bool json_object_agg_expression =
      root->semantic_variant_id ==
      "aggregate.global-json-object-agg-ordered-expression.v1";
  const bool ordered_single_collection_expression =
      array_agg_expression || json_agg_expression;
  const bool ordered_collection_expression =
      ordered_single_collection_expression || json_object_agg_expression;
  const bool stddev_pop_expression =
      root->semantic_variant_id ==
      "aggregate.global-stddev-pop-expression.v1";
  const bool variance_pop_expression =
      root->semantic_variant_id ==
      "aggregate.global-variance-pop-expression.v1";
  const bool stddev_expression =
      root->semantic_variant_id == "aggregate.global-stddev-expression.v1";
  const bool variance_expression =
      root->semantic_variant_id == "aggregate.global-variance-expression.v1";
  const bool stddev_samp_expression =
      root->semantic_variant_id ==
      "aggregate.global-stddev-samp-expression.v1";
  const bool variance_samp_expression =
      root->semantic_variant_id ==
      "aggregate.global-variance-samp-expression.v1";
  const bool statistical_expression =
      stddev_pop_expression || variance_pop_expression || stddev_expression ||
      variance_expression || stddev_samp_expression ||
      variance_samp_expression;
  struct PairStatisticalExpressionProfile {
    std::string_view semantic_variant;
    exec::CanonicalAggregateFunction function;
    std::string_view transformation_id;
  };
  static constexpr std::array<PairStatisticalExpressionProfile, 12>
      kPairStatisticalExpressionProfiles = {{
          {"aggregate.global-corr-expression.v1",
           exec::CanonicalAggregateFunction::corr,
           "canonical.aggregate.global-corr-expression.v1"},
          {"aggregate.global-covar-pop-expression.v1",
           exec::CanonicalAggregateFunction::covar_pop,
           "canonical.aggregate.global-covar-pop-expression.v1"},
          {"aggregate.global-covar-samp-expression.v1",
           exec::CanonicalAggregateFunction::covar_samp,
           "canonical.aggregate.global-covar-samp-expression.v1"},
          {"aggregate.global-regr-count-expression.v1",
           exec::CanonicalAggregateFunction::regr_count,
           "canonical.aggregate.global-regr-count-expression.v1"},
          {"aggregate.global-regr-avgx-expression.v1",
           exec::CanonicalAggregateFunction::regr_avgx,
           "canonical.aggregate.global-regr-avgx-expression.v1"},
          {"aggregate.global-regr-avgy-expression.v1",
           exec::CanonicalAggregateFunction::regr_avgy,
           "canonical.aggregate.global-regr-avgy-expression.v1"},
          {"aggregate.global-regr-intercept-expression.v1",
           exec::CanonicalAggregateFunction::regr_intercept,
           "canonical.aggregate.global-regr-intercept-expression.v1"},
          {"aggregate.global-regr-r2-expression.v1",
           exec::CanonicalAggregateFunction::regr_r2,
           "canonical.aggregate.global-regr-r2-expression.v1"},
          {"aggregate.global-regr-slope-expression.v1",
           exec::CanonicalAggregateFunction::regr_slope,
           "canonical.aggregate.global-regr-slope-expression.v1"},
          {"aggregate.global-regr-sxx-expression.v1",
           exec::CanonicalAggregateFunction::regr_sxx,
           "canonical.aggregate.global-regr-sxx-expression.v1"},
          {"aggregate.global-regr-sxy-expression.v1",
           exec::CanonicalAggregateFunction::regr_sxy,
           "canonical.aggregate.global-regr-sxy-expression.v1"},
          {"aggregate.global-regr-syy-expression.v1",
           exec::CanonicalAggregateFunction::regr_syy,
           "canonical.aggregate.global-regr-syy-expression.v1"},
      }};
  const auto pair_statistical_profile = std::ranges::find_if(
      kPairStatisticalExpressionProfiles, [&](const auto& profile) {
        return root->semantic_variant_id == profile.semantic_variant;
      });
  const bool pair_statistical_expression =
      pair_statistical_profile != kPairStatisticalExpressionProfiles.end();
  struct OrderedSetExpressionProfile {
    std::string_view semantic_variant;
    exec::CanonicalAggregateFunction function;
    std::string_view transformation_id;
  };
  static constexpr std::array<OrderedSetExpressionProfile, 7>
      kOrderedSetExpressionProfiles = {{
          {"aggregate.global-rank-hypothetical-expression.v1",
           exec::CanonicalAggregateFunction::rank,
           "canonical.aggregate.global-rank-hypothetical-expression.v1"},
          {"aggregate.global-dense-rank-hypothetical-expression.v1",
           exec::CanonicalAggregateFunction::dense_rank,
           "canonical.aggregate.global-dense-rank-hypothetical-expression.v1"},
          {"aggregate.global-percent-rank-hypothetical-expression.v1",
           exec::CanonicalAggregateFunction::percent_rank,
           "canonical.aggregate.global-percent-rank-hypothetical-expression.v1"},
          {"aggregate.global-cume-dist-hypothetical-expression.v1",
           exec::CanonicalAggregateFunction::cume_dist,
           "canonical.aggregate.global-cume-dist-hypothetical-expression.v1"},
          {"aggregate.global-mode-ordered-expression.v1",
           exec::CanonicalAggregateFunction::mode,
           "canonical.aggregate.global-mode-ordered-expression.v1"},
          {"aggregate.global-percentile-cont-ordered-expression.v1",
           exec::CanonicalAggregateFunction::percentile_cont,
           "canonical.aggregate.global-percentile-cont-ordered-expression.v1"},
          {"aggregate.global-percentile-disc-ordered-expression.v1",
           exec::CanonicalAggregateFunction::percentile_disc,
           "canonical.aggregate.global-percentile-disc-ordered-expression.v1"},
      }};
  const auto ordered_set_profile = std::ranges::find_if(
      kOrderedSetExpressionProfiles, [&](const auto& profile) {
        return root->semantic_variant_id == profile.semantic_variant;
      });
  const bool ordered_set_expression =
      ordered_set_profile != kOrderedSetExpressionProfiles.end();
  if (!count_star && !count_expression && !sum_expression &&
      !avg_expression && !min_expression && !max_expression &&
      !bool_and_expression && !bool_or_expression && !every_expression &&
      !string_agg_expression && !listagg_expression &&
      !ordered_collection_expression &&
      !statistical_expression &&
      !pair_statistical_expression && !ordered_set_expression) {
    return result;
  }
  auto aggregate_function = exec::CanonicalAggregateFunction::count;
  if (sum_expression) aggregate_function = exec::CanonicalAggregateFunction::sum;
  if (avg_expression) aggregate_function = exec::CanonicalAggregateFunction::avg;
  if (min_expression) aggregate_function = exec::CanonicalAggregateFunction::min;
  if (max_expression) aggregate_function = exec::CanonicalAggregateFunction::max;
  if (bool_and_expression) {
    aggregate_function = exec::CanonicalAggregateFunction::bool_and;
  }
  if (bool_or_expression) {
    aggregate_function = exec::CanonicalAggregateFunction::bool_or;
  }
  if (every_expression) {
    aggregate_function = exec::CanonicalAggregateFunction::every;
  }
  if (string_agg_expression) {
    aggregate_function = exec::CanonicalAggregateFunction::string_agg;
  }
  if (listagg_expression) {
    aggregate_function = exec::CanonicalAggregateFunction::listagg;
  }
  if (array_agg_expression) {
    aggregate_function = exec::CanonicalAggregateFunction::array_agg;
  }
  if (json_agg_expression) {
    aggregate_function = exec::CanonicalAggregateFunction::json_agg;
  }
  if (json_object_agg_expression) {
    aggregate_function = exec::CanonicalAggregateFunction::json_object_agg;
  }
  if (stddev_pop_expression) {
    aggregate_function = exec::CanonicalAggregateFunction::stddev_pop;
  }
  if (variance_pop_expression) {
    aggregate_function = exec::CanonicalAggregateFunction::variance_pop;
  }
  if (stddev_expression) {
    aggregate_function = exec::CanonicalAggregateFunction::stddev;
  }
  if (variance_expression) {
    aggregate_function = exec::CanonicalAggregateFunction::variance;
  }
  if (stddev_samp_expression) {
    aggregate_function = exec::CanonicalAggregateFunction::stddev_samp;
  }
  if (variance_samp_expression) {
    aggregate_function = exec::CanonicalAggregateFunction::variance_samp;
  }
  if (pair_statistical_expression) {
    aggregate_function = pair_statistical_profile->function;
  }
  if (ordered_set_expression) {
    aggregate_function = ordered_set_profile->function;
  }
  const auto input_node =
      std::ranges::find_if(graph.nodes, [&](const auto& node) {
        return node.logical_node_id == root->input_logical_node_ids.front();
      });
  if (input_node == graph.nodes.end() || input_node == root ||
      input_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kValues ||
      input_node->semantic_variant_id != "values.literal-table.v1" ||
      !input_node->input_logical_node_ids.empty()) {
    return result;
  }
  for (const auto& node : graph.nodes) {
    if (!node.required_object_uuids.empty() ||
        !node.required_property_uuids.empty() ||
        !node.delivered_property_uuids.empty()) {
      return result;
    }
  }
  result.profile_matched = true;
  const auto refuse = [&](std::string diagnostic_id, std::string detail) {
    result.optimizer_selected = false;
    result.physical_dag_published = false;
    result.physical_dag_executed = false;
    result.runtime_actuals_attached = false;
    result.canonical_result_published = false;
    result.physical_node_count = 0;
    result.canonical_result_column_count = 0;
    result.canonical_result_row_count = 0;
    result.selected_plan_uuid.clear();
    result.canonical_result_bytes.clear();
    result.api_result =
        Failure(request, std::move(diagnostic_id), std::move(detail));
    return result;
  };
  if (!request.optimizer_admission.admitted ||
      !request.optimizer_admission.planning_allowed) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-ADMISSION-V1",
                  "live global aggregate execution lacks optimizer admission");
  }

  auto input = MaterializeValues(request.relational_dag, *input_node);
  if (!input.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-PAYLOAD-V1",
                  "global aggregate input VALUES: " + input.detail);
  }
  auto prepared_root = PrepareGlobalAggregateRoot(
      request.relational_dag, *root, *input_node, input, aggregate_function,
      count_star);
  if (!prepared_root.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-PAYLOAD-V1",
                  prepared_root.detail);
  }

  const auto input_row_count = input.batch.rows.size();
  std::uint64_t input_memory = 1;
  std::uint64_t total_memory = 0;
  constexpr std::uint64_t kIntegerAggregateResultMemory =
      std::numeric_limits<std::int64_t>::digits10 + 2;
  constexpr std::uint64_t kRealAggregateResultMemory = 64;
  constexpr std::uint64_t kBooleanAggregateResultMemory = 5;
  const bool pair_real_result =
      pair_statistical_expression &&
      aggregate_function != exec::CanonicalAggregateFunction::regr_count;
  const bool ordered_set_real_result =
      aggregate_function == exec::CanonicalAggregateFunction::percent_rank ||
      aggregate_function == exec::CanonicalAggregateFunction::cume_dist ||
      aggregate_function == exec::CanonicalAggregateFunction::percentile_cont ||
      aggregate_function == exec::CanonicalAggregateFunction::percentile_disc;
  std::uint64_t aggregate_result_memory =
      (avg_expression || statistical_expression || pair_real_result ||
       ordered_set_real_result)
          ? kRealAggregateResultMemory
          : ((bool_and_expression || bool_or_expression || every_expression)
                 ? kBooleanAggregateResultMemory
                 : kIntegerAggregateResultMemory);
  if (!AddBatchMemoryBytes(input.batch, &input_memory)) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "live global aggregate input size overflowed");
  }
  if (string_agg_expression || listagg_expression) {
    std::uint64_t separator_memory = 0;
    aggregate_result_memory = input_memory;
    if (!CheckedMultiply(
            static_cast<std::uint64_t>(input_row_count),
            static_cast<std::uint64_t>(
                prepared_root.aggregate_separator.size()),
            &separator_memory) ||
        !CheckedAdd(aggregate_result_memory, separator_memory,
                    &aggregate_result_memory)) {
      return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                    "live STRING_AGG/LISTAGG result size overflowed");
    }
    if (ordered_string_agg_expression || listagg_expression) {
      std::uint64_t row_overhead_memory = 0;
      if (!CheckedMultiply(static_cast<std::uint64_t>(input_row_count), 64U,
                           &row_overhead_memory) ||
          !CheckedAdd(aggregate_result_memory, row_overhead_memory,
                      &aggregate_result_memory)) {
        return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                      "live ordered STRING_AGG/LISTAGG state size overflowed");
      }
    }
  }
  if (ordered_collection_expression) {
    std::uint64_t expanded_input_memory = 0;
    std::uint64_t row_overhead_memory = 0;
    const std::uint64_t input_expansion =
        (json_agg_expression || json_object_agg_expression) ? 6U : 1U;
    if (!CheckedMultiply(input_memory, input_expansion,
                         &expanded_input_memory) ||
        !CheckedMultiply(static_cast<std::uint64_t>(input_row_count), 64U,
                         &row_overhead_memory) ||
        !CheckedAdd(expanded_input_memory, row_overhead_memory,
                    &aggregate_result_memory) ||
        !CheckedAdd(aggregate_result_memory, 2U,
                    &aggregate_result_memory)) {
      return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                    "live ordered collection aggregate result size "
                    "overflowed");
    }
  }
  if (ordered_set_expression) {
    std::uint64_t ordered_state_memory = 0;
    if (!CheckedMultiply(static_cast<std::uint64_t>(input_row_count), 64U,
                         &ordered_state_memory) ||
        !CheckedAdd(aggregate_result_memory, ordered_state_memory,
                    &aggregate_result_memory)) {
      return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                    "live ordered-set aggregate state size overflowed");
    }
  }
  if (!CheckedAdd(input_memory, aggregate_result_memory, &total_memory)) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "live global aggregate input or result size overflowed");
  }
  if (total_memory >
      request.optimizer_request.resource.memory_budget_bytes) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "live global aggregate exceeds the admitted memory budget");
  }

  const std::string aggregate_implementation_id =
      count_star ? "aggregate.count-star.v1" : "aggregate.registry-core.v1";
  std::string aggregate_transformation_id;
  if (count_star) {
    aggregate_transformation_id =
        "canonical.aggregate.global-count-star.v1";
  } else if (count_expression) {
    aggregate_transformation_id =
        "canonical.aggregate.global-count-expression.v1";
  } else if (sum_expression) {
    aggregate_transformation_id =
        "canonical.aggregate.global-sum-expression.v1";
  } else if (avg_expression) {
    aggregate_transformation_id =
        "canonical.aggregate.global-avg-expression.v1";
  } else if (min_expression) {
    aggregate_transformation_id =
        "canonical.aggregate.global-min-expression.v1";
  } else if (max_expression) {
    aggregate_transformation_id =
        "canonical.aggregate.global-max-expression.v1";
  } else if (bool_and_expression) {
    aggregate_transformation_id =
        "canonical.aggregate.global-bool-and-expression.v1";
  } else if (bool_or_expression) {
    aggregate_transformation_id =
        "canonical.aggregate.global-bool-or-expression.v1";
  } else if (every_expression) {
    aggregate_transformation_id =
        "canonical.aggregate.global-every-expression.v1";
  } else if (ordered_string_agg_expression) {
    aggregate_transformation_id =
        "canonical.aggregate.global-string-agg-ordered-expression.v1";
  } else if (listagg_overflow_error_expression) {
    aggregate_transformation_id =
        "canonical.aggregate.global-listagg-ordered-overflow-error-expression.v1";
  } else if (listagg_overflow_truncate_expression) {
    aggregate_transformation_id =
        "canonical.aggregate.global-listagg-ordered-overflow-truncate-expression.v1";
  } else if (listagg_ordered_expression) {
    aggregate_transformation_id =
        "canonical.aggregate.global-listagg-ordered-expression.v1";
  } else if (string_agg_expression) {
    aggregate_transformation_id =
        "canonical.aggregate.global-string-agg-expression.v1";
  } else if (array_agg_expression) {
    aggregate_transformation_id =
        "canonical.aggregate.global-array-agg-ordered-expression.v1";
  } else if (json_agg_expression) {
    aggregate_transformation_id =
        "canonical.aggregate.global-json-agg-ordered-expression.v1";
  } else if (json_object_agg_expression) {
    aggregate_transformation_id =
        "canonical.aggregate.global-json-object-agg-ordered-expression.v1";
  } else if (stddev_pop_expression) {
    aggregate_transformation_id =
        "canonical.aggregate.global-stddev-pop-expression.v1";
  } else if (variance_pop_expression) {
    aggregate_transformation_id =
        "canonical.aggregate.global-variance-pop-expression.v1";
  } else if (stddev_expression) {
    aggregate_transformation_id =
        "canonical.aggregate.global-stddev-expression.v1";
  } else if (variance_expression) {
    aggregate_transformation_id =
        "canonical.aggregate.global-variance-expression.v1";
  } else if (stddev_samp_expression) {
    aggregate_transformation_id =
        "canonical.aggregate.global-stddev-samp-expression.v1";
  } else if (pair_statistical_expression) {
    aggregate_transformation_id =
        std::string(pair_statistical_profile->transformation_id);
  } else if (ordered_set_expression) {
    aggregate_transformation_id =
        std::string(ordered_set_profile->transformation_id);
  } else {
    aggregate_transformation_id =
        "canonical.aggregate.global-variance-samp-expression.v1";
  }

  const auto identity_scope =
      graph.bound_sblr_tree_uuid + ":" + request.context.statement_uuid.canonical;
  const auto values_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "values.capability");
  const auto aggregate_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "aggregate.capability");
  const std::vector<LivePhysicalNodeProfile> profiles = {
      {input_node->logical_node_id,
       std::string(kValuesImplementationId),
       values_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kValues,
       exec::PhysicalNodeKind::kValues,
       "canonical.values.materialize.v1",
       input_row_count,
       input_memory,
       0,
       0},
      {root->logical_node_id,
       aggregate_implementation_id,
       aggregate_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kAggregate,
       exec::PhysicalNodeKind::kAggregate,
       aggregate_transformation_id,
       input_row_count,
       total_memory,
       1,
       1}};
  const auto planning = PlanAndPublishLivePhysicalDag(
      request, profiles, "aggregate.selected-plan", "AGGREGATE");
  if (!planning.ok) {
    return refuse(planning.diagnostic_id, planning.detail);
  }
  result.optimizer_selected = true;
  result.physical_dag_published = true;
  result.physical_node_count = planning.physical_dag.nodes.size();
  result.selected_plan_uuid = planning.physical_dag.selected_plan_uuid;

  std::unordered_map<std::uint64_t, exec::DescriptorBatch> values_batches;
  values_batches.emplace(input_node->logical_node_id, std::move(input.batch));
  auto values_registration = MakeLiveValuesRegistration(
      std::move(values_batches), values_capability_uuid,
      "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-VALUES-V1", "AGGREGATE");

  exec::CanonicalPhysicalExecutorRegistration aggregate_registration;
  aggregate_registration.node_kind = exec::PhysicalNodeKind::kAggregate;
  aggregate_registration.implementation_id = aggregate_implementation_id;
  aggregate_registration.executor_capability_uuid =
      aggregate_capability_uuid;
  aggregate_registration.executor_capability_abi_version = 1;
  aggregate_registration.engine_owned = true;
  aggregate_registration.accepts_optimizer_publication_v2 = true;
  aggregate_registration.execute =
      [aggregate_descriptor = prepared_root.aggregate_descriptor,
       result_column = prepared_root.result_column,
       count_star = prepared_root.count_star,
       value_columns = prepared_root.value_columns,
       value_descriptor_ids = prepared_root.value_descriptor_ids,
       direct_arguments = prepared_root.direct_arguments,
       aggregate_order_terms = prepared_root.aggregate_order_terms,
       aggregate_separator = prepared_root.aggregate_separator,
       listagg_overflow_mode = prepared_root.listagg_overflow_mode,
       listagg_max_output_bytes = prepared_root.listagg_max_output_bytes,
       listagg_truncation_indicator =
           prepared_root.listagg_truncation_indicator,
       listagg_with_count = prepared_root.listagg_with_count,
       input_row_count](
          const exec::TypedPhysicalNodeDag& dag,
          const exec::PhysicalNodeRecord& node,
          const std::vector<exec::CanonicalPhysicalDispatchInput>& inputs) {
        exec::CanonicalPhysicalDispatchStepResult step;
        step.selected_plan_uuid = dag.selected_plan_uuid;
        step.executed_physical_node_id = node.physical_node_id;
        step.causal_counter_id = node.causal_counter_id;
        step.output_descriptor_ids = node.output_descriptor_ids;
        step.authority.engine_mga_snapshot_bound = true;
        if (inputs.size() != 1 ||
            !inputs.front().materialized_output_batch.has_value()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-INPUT-V1";
          step.diagnostic.detail =
              "global aggregate executor did not receive one typed input batch";
          return step;
        }
        const auto& input_batch = *inputs.front().materialized_output_batch;
        if (input_batch.rows.size() != input_row_count) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-INPUT-V1";
          step.diagnostic.detail =
              "global aggregate input cardinality differs from its selected cost";
          return step;
        }
        exec::DescriptorBatch output_batch;
        if (count_star) {
          exec::CanonicalDescriptorCountRequest aggregate_request;
          aggregate_request.physical_dag = dag;
          aggregate_request.selected_physical_node_id = node.physical_node_id;
          aggregate_request.input_batch = input_batch;
          aggregate_request.count_column = result_column;
          const auto aggregate_result =
              exec::ExecuteCanonicalDescriptorCountStar(aggregate_request);
          if (!aggregate_result.diagnostic.ok) {
            step.diagnostic = aggregate_result.diagnostic;
            return step;
          }
          output_batch = aggregate_result.output_batch;
        } else {
          exec::CanonicalAggregateRuntimeRequest aggregate_request;
          aggregate_request.physical_dag = dag;
          aggregate_request.selected_physical_node_id = node.physical_node_id;
          aggregate_request.descriptor = aggregate_descriptor;
          aggregate_request.input_batch = input_batch;
          aggregate_request.value_columns = value_columns;
          aggregate_request.value_expression_descriptor_ids =
              value_descriptor_ids;
          aggregate_request.direct_arguments = direct_arguments;
          aggregate_request.result_column = result_column;
          aggregate_request.aggregate_order_terms = aggregate_order_terms;
          aggregate_request.aggregate_separator = aggregate_separator;
          aggregate_request.listagg_overflow_mode = listagg_overflow_mode;
          aggregate_request.listagg_max_output_bytes =
              listagg_max_output_bytes;
          aggregate_request.listagg_truncation_indicator =
              listagg_truncation_indicator;
          aggregate_request.listagg_with_count = listagg_with_count;
          aggregate_request.forced_strategy =
              exec::CanonicalAggregateExecutionStrategy::serial;
          const auto aggregate_result =
              exec::ExecuteCanonicalAggregateRuntime(aggregate_request);
          if (!aggregate_result.diagnostic.ok) {
            step.diagnostic = aggregate_result.diagnostic;
            return step;
          }
          step.authority = aggregate_result.authority;
          output_batch = aggregate_result.output_batch;
        }
        step.result_handle_id = node.physical_node_id;
        step.input_row_count = input_batch.rows.size();
        step.rows_examined = input_batch.rows.size();
        step.output_row_count = output_batch.rows.size();
        step.materialized_output_batch = std::move(output_batch);
        return step;
      };

  api::CanonicalOptimizerSelectedExecutionRequest execution_request;
  execution_request.selected_physical_dag = planning.physical_dag;
  execution_request.pre_access_statistics_snapshot_uuid =
      planning.physical_dag.statistics_snapshot_uuid;
  execution_request.inventory_local_transaction_id =
      planning.physical_dag.local_transaction_id;
  execution_request.inventory_statement_snapshot_id =
      planning.physical_dag.statement_snapshot_id;
  execution_request.available_executors.push_back(
      std::move(values_registration));
  execution_request.available_executors.push_back(
      std::move(aggregate_registration));
  execution_request.engine_execution_authorized = true;
  execution_request.result_publication_request.statement_uuid =
      request.context.statement_uuid.canonical;
  execution_request.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" + request.context.current_monotonic_ns,
          "aggregate.execution-attempt");
  execution_request.result_publication_request.transaction_effect_evidence_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" +
              std::to_string(request.context.local_transaction_id) + ":" +
              std::to_string(
                  request.context.snapshot_visible_through_local_transaction_id),
          "aggregate.transaction-effect-unchanged");
  execution_request.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  execution_request.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  execution_request.result_publication_request.column_bindings =
      std::move(prepared_root.result_bindings);
  execution_request.result_publication_request.maximum_row_count = 1;

  const auto execution =
      api::ExecuteCanonicalOptimizerSelectedDag(execution_request);
  if (!execution.accepted || !execution.exact_selected_nodes_executed ||
      !execution.causal_counters_attached ||
      !execution.canonical_result_published || !execution.issues.empty()) {
    return refuse(
        execution.issues.empty()
            ? "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-EXECUTION-V1"
            : execution.issues.front().diagnostic_id,
        execution.issues.empty()
            ? "live global aggregate selected DAG was not completed"
            : execution.issues.front().field_id);
  }
  result.physical_dag_executed = true;
  result.runtime_actuals_attached = execution.runtime_actuals.accepted;
  result.canonical_result_published = execution.result_publication.published;
  result.canonical_result_column_count =
      execution.result_publication.envelope.column_descriptors.size();
  result.canonical_result_row_count =
      execution.result_publication.row_stream.rows.size();
  result.canonical_result_bytes =
      execution.result_publication.canonical_envelope_bytes;
  result.api_result = SuccessfulApiResult(request, execution);
  return result;
}

CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalObjectFreeLimitQuery(
    const CanonicalObjectFreeValuesExecutionRequest& request) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto& graph = request.optimizer_request.logical_graph;
  const auto root = std::ranges::find_if(graph.nodes, [&](const auto& node) {
    return node.logical_node_id == graph.root_logical_node_id;
  });
  if (graph.nodes.size() != 2 || root == graph.nodes.end() ||
      root->node_kind != plan::CanonicalLogicalRelationalNodeKind::kLimit ||
      root->semantic_variant_id != "limit.bound-count.v1" ||
      root->input_logical_node_ids.size() != 1 ||
      !request.optimizer_request.logical_properties.properties.empty()) {
    return result;
  }
  const auto input_node =
      std::ranges::find_if(graph.nodes, [&](const auto& node) {
        return node.logical_node_id == root->input_logical_node_ids.front();
      });
  if (input_node == graph.nodes.end() || input_node == root ||
      input_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kValues ||
      input_node->semantic_variant_id != "values.literal-table.v1" ||
      !input_node->input_logical_node_ids.empty()) {
    return result;
  }
  for (const auto& node : graph.nodes) {
    if (!node.required_object_uuids.empty() ||
        !node.required_property_uuids.empty() ||
        !node.delivered_property_uuids.empty()) {
      return result;
    }
  }
  result.profile_matched = true;
  const auto refuse = [&](std::string diagnostic_id, std::string detail) {
    result.optimizer_selected = false;
    result.physical_dag_published = false;
    result.physical_dag_executed = false;
    result.runtime_actuals_attached = false;
    result.canonical_result_published = false;
    result.physical_node_count = 0;
    result.canonical_result_column_count = 0;
    result.canonical_result_row_count = 0;
    result.selected_plan_uuid.clear();
    result.canonical_result_bytes.clear();
    result.api_result =
        Failure(request, std::move(diagnostic_id), std::move(detail));
    return result;
  };
  if (!request.optimizer_admission.admitted ||
      !request.optimizer_admission.planning_allowed) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-LIMIT-ADMISSION-V1",
                  "live LIMIT execution lacks optimizer admission");
  }
  if (root->bound_expression_ids.size() != 1) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-LIMIT-PAYLOAD-V1",
                  "bound-count LIMIT requires exactly one expression");
  }

  auto input = MaterializeValues(request.relational_dag, *input_node);
  if (!input.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-LIMIT-PAYLOAD-V1",
                  "LIMIT input VALUES: " + input.detail);
  }
  auto prepared_root = PrepareLimitRoot(request.relational_dag, *root,
                                        *input_node, input);
  if (!prepared_root.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-LIMIT-PAYLOAD-V1",
                  prepared_root.detail);
  }
  std::uint64_t row_limit = 0;
  std::string bound_detail;
  CanonicalRelationalExpressionRuntime expression_runtime(
      request.relational_dag);
  if (!EvaluateNonNegativeRowBound(
          &expression_runtime, root->bound_expression_ids.front(),
          &row_limit, &bound_detail)) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-LIMIT-PAYLOAD-V1",
                  "LIMIT count: " + bound_detail);
  }

  const auto input_row_count = input.batch.rows.size();
  const auto output_row_bound =
      row_limit > input_row_count
          ? input_row_count
          : static_cast<std::size_t>(row_limit);
  std::uint64_t input_memory = 1;
  std::uint64_t total_memory = 0;
  if (!AddBatchMemoryBytes(input.batch, &input_memory) ||
      !CheckedAdd(input_memory, output_row_bound == 0 ? 0 : input_memory,
                  &total_memory)) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "live LIMIT input or output size overflowed");
  }
  if (total_memory >
      request.optimizer_request.resource.memory_budget_bytes) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "live LIMIT exceeds the admitted memory budget");
  }

  const auto identity_scope =
      graph.bound_sblr_tree_uuid + ":" + request.context.statement_uuid.canonical;
  const auto values_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "values.capability");
  const auto limit_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "limit.capability");
  const std::vector<LivePhysicalNodeProfile> profiles = {
      {input_node->logical_node_id,
       std::string(kValuesImplementationId),
       values_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kValues,
       exec::PhysicalNodeKind::kValues,
       "canonical.values.materialize.v1",
       input_row_count,
       input_memory,
       0,
       0},
      {root->logical_node_id,
       "limit.typed.v1",
       limit_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kLimit,
       exec::PhysicalNodeKind::kLimit,
       "canonical.limit.bound-count.v1",
       output_row_bound,
       total_memory,
       1,
       1}};
  const auto planning = PlanAndPublishLivePhysicalDag(
      request, profiles, "limit.selected-plan", "LIMIT");
  if (!planning.ok) {
    return refuse(planning.diagnostic_id, planning.detail);
  }
  result.optimizer_selected = true;
  result.physical_dag_published = true;
  result.physical_node_count = planning.physical_dag.nodes.size();
  result.selected_plan_uuid = planning.physical_dag.selected_plan_uuid;

  std::unordered_map<std::uint64_t, exec::DescriptorBatch> values_batches;
  values_batches.emplace(input_node->logical_node_id, std::move(input.batch));
  auto values_registration = MakeLiveValuesRegistration(
      std::move(values_batches), values_capability_uuid,
      "QOW-DIAG-RELATIONAL-LIVE-LIMIT-VALUES-V1", "LIMIT");

  exec::CanonicalPhysicalExecutorRegistration limit_registration;
  limit_registration.node_kind = exec::PhysicalNodeKind::kLimit;
  limit_registration.implementation_id = "limit.typed.v1";
  limit_registration.executor_capability_uuid = limit_capability_uuid;
  limit_registration.executor_capability_abi_version = 1;
  limit_registration.engine_owned = true;
  limit_registration.accepts_optimizer_publication_v2 = true;
  limit_registration.execute =
      [row_limit, input_row_count](
          const exec::TypedPhysicalNodeDag& dag,
          const exec::PhysicalNodeRecord& node,
          const std::vector<exec::CanonicalPhysicalDispatchInput>& inputs) {
        exec::CanonicalPhysicalDispatchStepResult step;
        step.selected_plan_uuid = dag.selected_plan_uuid;
        step.executed_physical_node_id = node.physical_node_id;
        step.causal_counter_id = node.causal_counter_id;
        step.output_descriptor_ids = node.output_descriptor_ids;
        step.authority.engine_mga_snapshot_bound = true;
        if (inputs.size() != 1 ||
            !inputs.front().materialized_output_batch.has_value()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-LIMIT-INPUT-V1";
          step.diagnostic.detail =
              "LIMIT executor did not receive one typed input batch";
          return step;
        }
        const auto& input_batch = *inputs.front().materialized_output_batch;
        if (input_batch.rows.size() != input_row_count) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-LIMIT-INPUT-V1";
          step.diagnostic.detail =
              "LIMIT input cardinality differs from its selected cost";
          return step;
        }
        exec::CanonicalDescriptorLimitRequest limit_request;
        limit_request.physical_dag = dag;
        limit_request.selected_physical_node_id = node.physical_node_id;
        limit_request.input_batch = input_batch;
        limit_request.limit = row_limit;
        limit_request.offset = 0;
        const auto limit_result =
            exec::ExecuteCanonicalDescriptorLimit(limit_request);
        if (!limit_result.diagnostic.ok) {
          step.diagnostic = limit_result.diagnostic;
          return step;
        }
        step.result_handle_id = node.physical_node_id;
        step.input_row_count = input_batch.rows.size();
        step.rows_examined = limit_result.output_batch.rows.size();
        step.output_row_count = limit_result.output_batch.rows.size();
        step.materialized_output_batch = limit_result.output_batch;
        return step;
      };

  api::CanonicalOptimizerSelectedExecutionRequest execution_request;
  execution_request.selected_physical_dag = planning.physical_dag;
  execution_request.pre_access_statistics_snapshot_uuid =
      planning.physical_dag.statistics_snapshot_uuid;
  execution_request.inventory_local_transaction_id =
      planning.physical_dag.local_transaction_id;
  execution_request.inventory_statement_snapshot_id =
      planning.physical_dag.statement_snapshot_id;
  execution_request.available_executors.push_back(
      std::move(values_registration));
  execution_request.available_executors.push_back(
      std::move(limit_registration));
  execution_request.engine_execution_authorized = true;
  execution_request.result_publication_request.statement_uuid =
      request.context.statement_uuid.canonical;
  execution_request.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" + request.context.current_monotonic_ns,
          "limit.execution-attempt");
  execution_request.result_publication_request.transaction_effect_evidence_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" +
              std::to_string(request.context.local_transaction_id) + ":" +
              std::to_string(
                  request.context.snapshot_visible_through_local_transaction_id),
          "limit.transaction-effect-unchanged");
  execution_request.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  execution_request.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  execution_request.result_publication_request.column_bindings =
      std::move(prepared_root.result_bindings);
  execution_request.result_publication_request.maximum_row_count =
      std::max<std::size_t>(1, output_row_bound);

  const auto execution =
      api::ExecuteCanonicalOptimizerSelectedDag(execution_request);
  if (!execution.accepted || !execution.exact_selected_nodes_executed ||
      !execution.causal_counters_attached ||
      !execution.canonical_result_published || !execution.issues.empty()) {
    return refuse(
        execution.issues.empty()
            ? "QOW-DIAG-RELATIONAL-LIVE-LIMIT-EXECUTION-V1"
            : execution.issues.front().diagnostic_id,
        execution.issues.empty()
            ? "live LIMIT selected DAG was not completed"
            : execution.issues.front().field_id);
  }
  result.physical_dag_executed = true;
  result.runtime_actuals_attached = execution.runtime_actuals.accepted;
  result.canonical_result_published = execution.result_publication.published;
  result.canonical_result_column_count =
      execution.result_publication.envelope.column_descriptors.size();
  result.canonical_result_row_count =
      execution.result_publication.row_stream.rows.size();
  result.canonical_result_bytes =
      execution.result_publication.canonical_envelope_bytes;
  result.api_result = SuccessfulApiResult(request, execution);
  return result;
}

CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalObjectFreeSortQuery(
    const CanonicalObjectFreeValuesExecutionRequest& request) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto& graph = request.optimizer_request.logical_graph;
  const auto root = std::ranges::find_if(graph.nodes, [&](const auto& node) {
    return node.logical_node_id == graph.root_logical_node_id;
  });
  if (graph.nodes.size() != 2 || root == graph.nodes.end() ||
      root->node_kind != plan::CanonicalLogicalRelationalNodeKind::kSort ||
      root->semantic_variant_id != "sort.required-order.v1" ||
      root->input_logical_node_ids.size() != 1) {
    return result;
  }
  const auto input_node =
      std::ranges::find_if(graph.nodes, [&](const auto& node) {
        return node.logical_node_id == root->input_logical_node_ids.front();
      });
  if (input_node == graph.nodes.end() || input_node == root ||
      input_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kValues ||
      input_node->semantic_variant_id != "values.literal-table.v1" ||
      !input_node->input_logical_node_ids.empty()) {
    return result;
  }
  for (const auto& node : graph.nodes) {
    if (!node.required_object_uuids.empty() ||
        (node.logical_node_id == input_node->logical_node_id &&
         (!node.required_property_uuids.empty() ||
          !node.delivered_property_uuids.empty()))) {
      return result;
    }
  }
  result.profile_matched = true;
  const auto refuse = [&](std::string diagnostic_id, std::string detail) {
    result.optimizer_selected = false;
    result.physical_dag_published = false;
    result.physical_dag_executed = false;
    result.runtime_actuals_attached = false;
    result.canonical_result_published = false;
    result.physical_node_count = 0;
    result.canonical_result_column_count = 0;
    result.canonical_result_row_count = 0;
    result.selected_plan_uuid.clear();
    result.canonical_result_bytes.clear();
    result.api_result =
        Failure(request, std::move(diagnostic_id), std::move(detail));
    return result;
  };
  if (!request.optimizer_admission.admitted ||
      !request.optimizer_admission.planning_allowed) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-SORT-ADMISSION-V1",
                  "live SORT execution lacks optimizer admission");
  }

  auto input = MaterializeValues(request.relational_dag, *input_node);
  if (!input.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-SORT-PAYLOAD-V1",
                  "SORT input VALUES: " + input.detail);
  }
  auto prepared_root = PrepareSortRoot(
      request.context, request.relational_dag,
      request.optimizer_request.logical_properties, *root, *input_node,
      input);
  if (!prepared_root.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-SORT-PAYLOAD-V1",
                  prepared_root.detail);
  }

  const auto input_row_count = input.batch.rows.size();
  std::uint64_t input_memory = 1;
  std::uint64_t comparison_count = 0;
  std::uint64_t row_order_memory = 0;
  std::uint64_t total_memory = 0;
  if (!AddBatchMemoryBytes(input.batch, &input_memory) ||
      !CheckedMultiply(input_row_count, input_row_count,
                       &comparison_count) ||
      !CheckedMultiply(input_row_count, sizeof(std::size_t),
                       &row_order_memory) ||
      !CheckedAdd(input_memory, input_memory, &total_memory) ||
      !CheckedAdd(total_memory, comparison_count, &total_memory) ||
      !CheckedAdd(total_memory, row_order_memory, &total_memory) ||
      comparison_count > std::numeric_limits<std::size_t>::max()) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "live SORT comparison or materialization size overflowed");
  }
  if (total_memory >
      request.optimizer_request.resource.memory_budget_bytes) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "live SORT exceeds the admitted memory budget");
  }

  const auto identity_scope =
      graph.bound_sblr_tree_uuid + ":" + request.context.statement_uuid.canonical;
  const auto values_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "values.capability");
  const auto sort_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "sort.capability");
  const auto deterministic_tie_evidence_uuid = DerivedCanonicalUuid(
      identity_scope + ":" + prepared_root.ordering_property_uuid,
      "sort.deterministic-tie");
  const std::vector<LivePhysicalNodeProfile> profiles = {
      {input_node->logical_node_id,
       std::string(kValuesImplementationId),
       values_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kValues,
       exec::PhysicalNodeKind::kValues,
       "canonical.values.materialize.v1",
       input_row_count,
       input_memory,
       0,
       0},
      {root->logical_node_id,
       "sort.typed.terms.v1",
       sort_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kSort,
       exec::PhysicalNodeKind::kSort,
       "canonical.sort.typed.terms.v1",
       input_row_count,
       total_memory,
       1,
       1,
       {},
       {prepared_root.ordering_property_uuid},
       {plan::CanonicalLogicalPropertyKind::kOrdering}}};
  const auto planning = PlanAndPublishLivePhysicalDag(
      request, profiles, "sort.selected-plan", "SORT");
  if (!planning.ok) {
    return refuse(planning.diagnostic_id, planning.detail);
  }
  result.optimizer_selected = true;
  result.physical_dag_published = true;
  result.physical_node_count = planning.physical_dag.nodes.size();
  result.selected_plan_uuid = planning.physical_dag.selected_plan_uuid;

  std::unordered_map<std::uint64_t, exec::DescriptorBatch> values_batches;
  values_batches.emplace(input_node->logical_node_id, std::move(input.batch));
  auto values_registration = MakeLiveValuesRegistration(
      std::move(values_batches), values_capability_uuid,
      "QOW-DIAG-RELATIONAL-LIVE-SORT-VALUES-V1", "SORT");

  exec::CanonicalPhysicalExecutorRegistration sort_registration;
  sort_registration.node_kind = exec::PhysicalNodeKind::kSort;
  sort_registration.implementation_id = "sort.typed.terms.v1";
  sort_registration.executor_capability_uuid = sort_capability_uuid;
  sort_registration.executor_capability_abi_version = 1;
  sort_registration.engine_owned = true;
  sort_registration.accepts_optimizer_publication_v2 = true;
  sort_registration.execute =
      [order_terms = prepared_root.order_terms,
       deterministic_tie_evidence_uuid, input_row_count,
       maximum_pair_comparisons =
           std::max<std::size_t>(1, static_cast<std::size_t>(comparison_count))](
          const exec::TypedPhysicalNodeDag& dag,
          const exec::PhysicalNodeRecord& node,
          const std::vector<exec::CanonicalPhysicalDispatchInput>& inputs) {
        exec::CanonicalPhysicalDispatchStepResult step;
        step.selected_plan_uuid = dag.selected_plan_uuid;
        step.executed_physical_node_id = node.physical_node_id;
        step.causal_counter_id = node.causal_counter_id;
        step.output_descriptor_ids = node.output_descriptor_ids;
        step.authority.engine_mga_snapshot_bound = true;
        if (inputs.size() != 1 ||
            !inputs.front().materialized_output_batch.has_value()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-SORT-INPUT-V1";
          step.diagnostic.detail =
              "SORT executor did not receive one typed input batch";
          return step;
        }
        const auto& input_batch = *inputs.front().materialized_output_batch;
        if (input_batch.rows.size() != input_row_count) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-SORT-INPUT-V1";
          step.diagnostic.detail =
              "SORT input cardinality differs from its selected cost";
          return step;
        }
        exec::CanonicalDescriptorSortRequest sort_request;
        sort_request.physical_dag = dag;
        sort_request.selected_physical_node_id = node.physical_node_id;
        sort_request.input_batch = input_batch;
        sort_request.order_terms = order_terms;
        sort_request.deterministic_tie_evidence_uuid =
            deterministic_tie_evidence_uuid;
        sort_request.maximum_pair_comparisons = maximum_pair_comparisons;
        const auto sort_result =
            exec::ExecuteCanonicalDescriptorSort(sort_request);
        if (!sort_result.diagnostic.ok) {
          step.diagnostic = sort_result.diagnostic;
          return step;
        }
        step.result_handle_id = node.physical_node_id;
        step.input_row_count = input_batch.rows.size();
        step.rows_examined = input_batch.rows.size();
        step.output_row_count = sort_result.output_batch.rows.size();
        step.materialized_output_batch = sort_result.output_batch;
        return step;
      };

  api::CanonicalOptimizerSelectedExecutionRequest execution_request;
  execution_request.selected_physical_dag = planning.physical_dag;
  execution_request.pre_access_statistics_snapshot_uuid =
      planning.physical_dag.statistics_snapshot_uuid;
  execution_request.inventory_local_transaction_id =
      planning.physical_dag.local_transaction_id;
  execution_request.inventory_statement_snapshot_id =
      planning.physical_dag.statement_snapshot_id;
  execution_request.available_executors.push_back(
      std::move(values_registration));
  execution_request.available_executors.push_back(
      std::move(sort_registration));
  execution_request.engine_execution_authorized = true;
  execution_request.result_publication_request.statement_uuid =
      request.context.statement_uuid.canonical;
  execution_request.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" + request.context.current_monotonic_ns,
          "sort.execution-attempt");
  execution_request.result_publication_request.transaction_effect_evidence_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" +
              std::to_string(request.context.local_transaction_id) + ":" +
              std::to_string(
                  request.context.snapshot_visible_through_local_transaction_id),
          "sort.transaction-effect-unchanged");
  execution_request.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  execution_request.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  execution_request.result_publication_request.column_bindings =
      std::move(prepared_root.result_bindings);
  execution_request.result_publication_request.maximum_row_count =
      std::max<std::size_t>(1, input_row_count);

  const auto execution =
      api::ExecuteCanonicalOptimizerSelectedDag(execution_request);
  if (!execution.accepted || !execution.exact_selected_nodes_executed ||
      !execution.causal_counters_attached ||
      !execution.canonical_result_published || !execution.issues.empty()) {
    return refuse(
        execution.issues.empty()
            ? "QOW-DIAG-RELATIONAL-LIVE-SORT-EXECUTION-V1"
            : execution.issues.front().diagnostic_id,
        execution.issues.empty()
            ? "live SORT selected DAG was not completed"
            : execution.issues.front().field_id);
  }
  result.physical_dag_executed = true;
  result.runtime_actuals_attached = execution.runtime_actuals.accepted;
  result.canonical_result_published = execution.result_publication.published;
  result.canonical_result_column_count =
      execution.result_publication.envelope.column_descriptors.size();
  result.canonical_result_row_count =
      execution.result_publication.row_stream.rows.size();
  result.canonical_result_bytes =
      execution.result_publication.canonical_envelope_bytes;
  result.api_result = SuccessfulApiResult(request, execution);
  return result;
}

}  // namespace

CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalObjectFreeValuesQuery(
    const CanonicalObjectFreeValuesExecutionRequest& request) {
  auto aggregate = ExecuteCanonicalObjectFreeGlobalAggregateQuery(request);
  if (aggregate.profile_matched) return aggregate;
  auto sort = ExecuteCanonicalObjectFreeSortQuery(request);
  if (sort.profile_matched) return sort;
  auto limit = ExecuteCanonicalObjectFreeLimitQuery(request);
  if (limit.profile_matched) return limit;
  auto project = ExecuteCanonicalObjectFreeProjectQuery(request);
  if (project.profile_matched) return project;
  auto filter = ExecuteCanonicalObjectFreeFilterQuery(request);
  if (filter.profile_matched) return filter;
  auto inner_join = ExecuteCanonicalObjectFreeInnerJoinQuery(request);
  if (inner_join.profile_matched) return inner_join;
  auto set_operation = ExecuteCanonicalObjectFreeUnionAllQuery(request);
  if (set_operation.profile_matched) return set_operation;
  CanonicalObjectFreeValuesExecutionResult result;
  const auto refuse = [&](std::string diagnostic_id, std::string detail) {
    result.optimizer_selected = false;
    result.physical_dag_published = false;
    result.physical_dag_executed = false;
    result.runtime_actuals_attached = false;
    result.canonical_result_published = false;
    result.physical_node_count = 0;
    result.canonical_result_column_count = 0;
    result.canonical_result_row_count = 0;
    result.selected_plan_uuid.clear();
    result.canonical_result_bytes.clear();
    result.api_result = Failure(request, std::move(diagnostic_id),
                                std::move(detail));
    return result;
  };

  const auto& graph = request.optimizer_request.logical_graph;
  if (graph.nodes.size() != 1 ||
      graph.root_logical_node_id != graph.nodes.front().logical_node_id ||
      graph.nodes.front().node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kValues ||
      graph.nodes.front().semantic_variant_id != "values.literal-table.v1" ||
      !graph.nodes.front().input_logical_node_ids.empty() ||
      !graph.nodes.front().required_object_uuids.empty() ||
      !graph.nodes.front().required_property_uuids.empty() ||
      !graph.nodes.front().delivered_property_uuids.empty() ||
      !request.optimizer_request.logical_properties.properties.empty()) {
    return result;
  }
  result.profile_matched = true;
  if (!request.optimizer_admission.admitted ||
      !request.optimizer_admission.planning_allowed) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-VALUES-ADMISSION-V1",
                  "live VALUES execution lacks optimizer admission");
  }

  auto materialized = MaterializeValues(request.relational_dag,
                                        graph.nodes.front());
  if (!materialized.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-VALUES-PAYLOAD-V1",
                  materialized.detail);
  }

  const auto identity_scope =
      graph.bound_sblr_tree_uuid + ":" + request.context.statement_uuid.canonical;
  const auto alternative_uuid =
      DerivedCanonicalUuid(identity_scope, "values.alternative");
  const auto capability_uuid =
      DerivedCanonicalUuid(identity_scope, "values.capability");
  const auto transformation_uuid =
      DerivedCanonicalUuid(identity_scope, "values.transformation");
  const auto cost_vector_uuid =
      DerivedCanonicalUuid(identity_scope, "values.cost-vector");
  const auto calibration_uuid =
      DerivedCanonicalUuid(identity_scope, "values.calibration");

  plan::CanonicalPhysicalAlternativeCatalog alternatives;
  alternatives.bound_sblr_tree_uuid = graph.bound_sblr_tree_uuid;
  alternatives.catalog_epoch_uuid = graph.catalog_epoch_uuid;
  alternatives.security_context_uuid = graph.security_context_uuid;
  alternatives.local_transaction_id = graph.local_transaction_id;
  alternatives.statement_snapshot_id = graph.statement_snapshot_id;
  alternatives.alternatives.push_back(
      {alternative_uuid,
       graph.nodes.front().logical_node_id,
       std::string(kValuesImplementationId),
       capability_uuid,
       graph.nodes.front().output_descriptor_ids,
       true,
       {},
       {},
       {}});

  std::uint64_t memory_bytes = 1;
  for (const auto& row : materialized.batch.rows) {
    for (const auto& value : row.values) {
      if (value.encoded_value.size() >
          std::numeric_limits<std::uint64_t>::max() - memory_bytes) {
        return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                      "live VALUES materialization size overflowed");
      }
      memory_bytes += value.encoded_value.size();
    }
  }
  if (memory_bytes > request.optimizer_request.resource.memory_budget_bytes) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "live VALUES materialization exceeds the admitted memory budget");
  }

  opt::CanonicalOptimizerSearchCandidateInput candidate;
  candidate.alternative_uuid = alternative_uuid;
  candidate.transformation_uuid = transformation_uuid;
  candidate.transformation_rule_id = "canonical.values.materialize.v1";
  candidate.bound_sblr_tree_uuid = graph.bound_sblr_tree_uuid;
  candidate.statistics_snapshot_uuid =
      request.optimizer_admission.statistics_snapshot_uuid;
  candidate.statistics_generation =
      request.optimizer_admission.statistics_generation;
  candidate.model_family_id = "relational.local.v1";
  candidate.cost_terms.cost_vector_uuid = cost_vector_uuid;
  candidate.cost_terms.calibration_profile_uuid = calibration_uuid;
  candidate.cost_terms.cpu_units = materialized.batch.rows.size();
  candidate.cost_terms.memory_bytes_required = memory_bytes;
  candidate.cost_terms.confidence = opt::CostConfidence::kExact;
  candidate.semantic_preserving = true;
  candidate.derived_from_admitted_statistics = true;
  candidate.engine_coster_owned = true;

  opt::CanonicalOptimizerSearchPolicy search_policy;
  search_policy.maximum_exhaustive_plan_count = 1;
  search_policy.bounded_beam_width = 1;
  search_policy.deterministic_step_cost_ns = 1;
  search_policy.engine_owned = true;
  const auto search = opt::SearchCanonicalRelationalMemo(
      request.optimizer_request, request.optimizer_admission, alternatives,
      {candidate}, search_policy);
  if (!search.accepted || !search.selected || !search.issues.empty()) {
    const auto diagnostic = search.issues.empty()
                                ? "QOW-DIAG-OPTIMIZER-SEARCH-NO-PLAN-V1"
                                : search.issues.front().diagnostic_id;
    const auto detail = search.issues.empty()
                            ? "live VALUES search returned no selected plan"
                            : search.issues.front().field_id;
    return refuse(diagnostic, detail);
  }
  result.optimizer_selected = true;

  opt::CanonicalExecutorCapabilityCatalog capabilities;
  capabilities.capability_snapshot_uuid =
      request.optimizer_admission.capability_snapshot_uuid;
  capabilities.policy_epoch = request.optimizer_admission.policy_epoch;
  capabilities.engine_owned = true;
  opt::CanonicalExecutorCapabilityRecord capability;
  capability.capability_uuid = capability_uuid;
  capability.capability_abi_version = 1;
  capability.implementation_id = kValuesImplementationId;
  capability.logical_node_kind =
      plan::CanonicalLogicalRelationalNodeKind::kValues;
  capability.physical_node_kind = exec::PhysicalNodeKind::kValues;
  capability.maximum_memory_bytes =
      request.optimizer_request.resource.memory_budget_bytes;
  capability.spill_supported = false;
  capability.available = true;
  capability.engine_owned = true;
  capabilities.capabilities.push_back(std::move(capability));

  opt::CanonicalOptimizerPhysicalPublicationIdentity publication_identity;
  publication_identity.selected_plan_uuid =
      DerivedCanonicalUuid(identity_scope, "values.selected-plan");
  publication_identity.first_causal_counter_id = 1;
  publication_identity.engine_owned = true;
  const auto publication = opt::PublishCanonicalPhysicalDag(
      request.optimizer_request, request.optimizer_admission, alternatives,
      search, capabilities, publication_identity);
  if (!publication.accepted || !publication.published ||
      !publication.issues.empty()) {
    const auto diagnostic = publication.issues.empty()
                                ? "QOW-DIAG-OPTIMIZER-PHYSICAL-PUBLICATION-V1"
                                : publication.issues.front().diagnostic_id;
    const auto detail = publication.issues.empty()
                            ? "live VALUES physical DAG was not published"
                            : publication.issues.front().field_id;
    return refuse(diagnostic, detail);
  }
  result.physical_dag_published = true;
  result.physical_node_count = publication.physical_dag.nodes.size();
  result.selected_plan_uuid = publication.physical_dag.selected_plan_uuid;

  exec::CanonicalPhysicalExecutorRegistration registration;
  registration.node_kind = exec::PhysicalNodeKind::kValues;
  registration.implementation_id = kValuesImplementationId;
  registration.executor_capability_uuid = capability_uuid;
  registration.executor_capability_abi_version = 1;
  registration.engine_owned = true;
  registration.accepts_optimizer_publication_v2 = true;
  registration.execute =
      [batch = materialized.batch](const exec::TypedPhysicalNodeDag& dag,
                                   const exec::PhysicalNodeRecord& node,
                                   const std::vector<
                                       exec::CanonicalPhysicalDispatchInput>&
                                       inputs) {
        exec::CanonicalPhysicalDispatchStepResult step;
        step.selected_plan_uuid = dag.selected_plan_uuid;
        step.executed_physical_node_id = node.physical_node_id;
        step.causal_counter_id = node.causal_counter_id;
        step.output_descriptor_ids = node.output_descriptor_ids;
        step.authority.engine_mga_snapshot_bound = true;
        if (!inputs.empty()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-VALUES-INPUT-V1";
          step.diagnostic.detail = "VALUES executor received an input edge";
          return step;
        }
        step.result_handle_id = node.physical_node_id;
        step.output_row_count = batch.rows.size();
        step.rows_examined = batch.rows.size();
        step.materialized_output_batch = batch;
        return step;
      };

  api::CanonicalOptimizerSelectedExecutionRequest execution_request;
  execution_request.selected_physical_dag = publication.physical_dag;
  execution_request.pre_access_statistics_snapshot_uuid =
      publication.physical_dag.statistics_snapshot_uuid;
  execution_request.inventory_local_transaction_id =
      publication.physical_dag.local_transaction_id;
  execution_request.inventory_statement_snapshot_id =
      publication.physical_dag.statement_snapshot_id;
  execution_request.available_executors.push_back(std::move(registration));
  execution_request.engine_execution_authorized = true;
  execution_request.result_publication_request.statement_uuid =
      request.context.statement_uuid.canonical;
  execution_request.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" + request.context.current_monotonic_ns,
          "values.execution-attempt");
  execution_request.result_publication_request.transaction_effect_evidence_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" +
              std::to_string(request.context.local_transaction_id) + ":" +
              std::to_string(
                  request.context.snapshot_visible_through_local_transaction_id),
          "values.transaction-effect-unchanged");
  execution_request.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  execution_request.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  execution_request.result_publication_request.column_bindings =
      std::move(materialized.result_bindings);
  execution_request.result_publication_request.maximum_row_count =
      std::max<std::size_t>(1, materialized.batch.rows.size());

  const auto execution =
      api::ExecuteCanonicalOptimizerSelectedDag(execution_request);
  if (!execution.accepted || !execution.exact_selected_nodes_executed ||
      !execution.causal_counters_attached ||
      !execution.canonical_result_published || !execution.issues.empty()) {
    const auto diagnostic = execution.issues.empty()
                                ? "QOW-DIAG-RELATIONAL-LIVE-VALUES-EXECUTION-V1"
                                : execution.issues.front().diagnostic_id;
    const auto detail = execution.issues.empty()
                            ? "live VALUES selected DAG was not completed"
                            : execution.issues.front().field_id;
    return refuse(diagnostic, detail);
  }
  result.physical_dag_executed = true;
  result.runtime_actuals_attached = execution.runtime_actuals.accepted;
  result.canonical_result_published =
      execution.result_publication.published;
  result.canonical_result_column_count =
      execution.result_publication.envelope.column_descriptors.size();
  result.canonical_result_row_count =
      execution.result_publication.row_stream.rows.size();
  result.canonical_result_bytes =
      execution.result_publication.canonical_envelope_bytes;
  result.api_result = SuccessfulApiResult(request, execution);
  return result;
}

}  // namespace scratchbird::engine::sblr
