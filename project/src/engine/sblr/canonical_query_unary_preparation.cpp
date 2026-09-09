// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_query_aggregate_registration.hpp"
#include "canonical_query_correlated_registration.hpp"
#include "canonical_query_descriptor_support.hpp"
#include "canonical_query_node_composition.hpp"
#include "canonical_query_object_free_composition_support.hpp"
#include "canonical_query_predicate_support.hpp"
#include "canonical_query_projection_registration.hpp"
#include "canonical_query_recursive_registration.hpp"
#include "canonical_query_runtime_memory_support.hpp"
#include "canonical_query_scalar_support.hpp"
#include "canonical_query_set_registration.hpp"
#include "canonical_query_sort_registration.hpp"
#include "canonical_relational_expression.hpp"
#include "catalog/name_resolution_api.hpp"
#include "datatype_catalog_manifest.hpp"
#include "datatype_operations.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace scratchbird::engine::sblr {
namespace api = scratchbird::engine::internal_api;
namespace dt = scratchbird::core::datatypes;
namespace exec = scratchbird::engine::executor;
namespace plan = scratchbird::engine::planner;

// SEARCH_KEY: SB_ENGINE_CANONICAL_QUERY_UNARY_PREPARATION_AUTHORITY
// Single-input descriptor, expression, order/equality, row-bound, and result
// preparation over supplied typed input; resource lookup remains engine-owned.
// Owns no plan selection, physical DAG dispatch, relation-store access,
// snapshot construction, transaction finality, or public route selection.

namespace {

using PreparedProjectExpression = LiveProjectExpressionRegistration;

LiveProjectRegistrationProfile MakeLiveProjectRegistrationProfile(
    const PreparedProjectRoot& prepared_root) {
  LiveProjectRegistrationProfile profile;
  profile.expression_projection = prepared_root.expression_projection;
  profile.projected_columns = prepared_root.projected_columns;
  profile.expressions = prepared_root.expressions;
  profile.expression_output_columns =
      prepared_root.expression_output_batch.columns;
  return profile;
}

bool PrepareCanonicalSortOrderTerm(
    const api::EngineRequestContext& context,
    const plan::CanonicalLogicalPropertyOrderingTerm& logical_term,
    const exec::ExecutorColumnDescriptor& column,
    const std::size_t column_ordinal,
    exec::CanonicalDescriptorOrderTerm* term,
    std::string* detail) {
  if (term == nullptr || detail == nullptr) return false;
  *term = {};
  detail->clear();
  term->column = column_ordinal;
  term->expression_descriptor_id = column.descriptor_id;
  term->direction =
      logical_term.direction ==
              plan::CanonicalLogicalPropertySortDirection::kAscending
          ? exec::CanonicalDescriptorOrderDirection::ascending
          : exec::CanonicalDescriptorOrderDirection::descending;
  term->null_placement =
      logical_term.null_placement ==
              plan::CanonicalLogicalPropertyNullPlacement::kNullsFirst
          ? exec::CanonicalDescriptorNullPlacement::first
          : exec::CanonicalDescriptorNullPlacement::last;
  term->collation_uuid = logical_term.collation_uuid;

  if (dt::CanonicalTypeIdFromStableName(
          column.descriptor.canonical_type_name) ==
      dt::CanonicalTypeId::character) {
#if defined(SCRATCHBIRD_QOW_QUERY_ROUTE_CONTRACT_ONLY)
    *term = {};
    *detail =
        "sort character ordering requires the production engine resource "
        "catalog";
    return false;
#else
    api::EngineUuid collation_uuid;
    collation_uuid.canonical = term->collation_uuid;
    const auto resolved = api::LookupEngineResourceDescriptorByUuid(
        context, collation_uuid, "collation");
    if (!resolved.ok || !resolved.resource_descriptor.present ||
        resolved.resource_descriptor.resource_uuid.canonical !=
            term->collation_uuid) {
      *term = {};
      *detail =
          "sort character ordering lacks current engine collation authority: " +
          (resolved.diagnostic.code.empty()
               ? std::string("CATALOG.RESOURCE.DESCRIPTOR_INVALID")
               : resolved.diagnostic.code);
      return false;
    }
    term->resource_epoch = resolved.resource_descriptor.resource_epoch;
    term->collation_epoch = resolved.resource_descriptor.family_epoch;
    term->text_seed.active = true;
    term->text_seed.seed_pack_name =
        resolved.resource_descriptor.seed_pack_name;
    term->text_seed.seed_pack_version =
        resolved.resource_descriptor.seed_pack_version;
    term->text_seed.charset_name =
        resolved.resource_descriptor.parent_canonical_name;
    term->text_seed.collation_name =
        resolved.resource_descriptor.canonical_name;
    term->text_seed.collation_case_insensitive =
        resolved.resource_descriptor.case_insensitive;
    term->text_seed.collation_accent_insensitive =
        resolved.resource_descriptor.accent_insensitive;
#endif
  } else if ((column.descriptor.canonical_type_name == "time" ||
              column.descriptor.canonical_type_name == "timestamp") &&
             column.descriptor.encoded_descriptor.find(
                 "timezone_profile_id=") != std::string::npos) {
    if (!BindTimezoneOrderAuthority(context, term, detail)) {
      *term = {};
      return false;
    }
  }
  const auto validation =
      exec::ValidateCanonicalDescriptorOrderTerm(*term, column);
  if (!validation.ok) {
    *term = {};
    *detail = validation.detail;
    return false;
  }
  return true;
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

    const auto& column = input.batch.columns[ordinal->second];
    exec::CanonicalDescriptorOrderTerm term;
    if (!PrepareCanonicalSortOrderTerm(
            context, logical_term, column, ordinal->second, &term,
            &result.detail)) {
      result.order_terms.clear();
      return result;
    }
    result.order_terms.push_back(std::move(term));
  }

  result.result_bindings = input.result_bindings;
  result.ordering_property_uuid = property.property_uuid;
  result.ok = true;
  return result;
}

PreparedDistinctRoot PrepareQueryDistinctRoot(
    const api::EngineRequestContext& context,
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalRelationalNode& distinct_node,
    const plan::CanonicalLogicalRelationalNode& input_node,
    const MaterializedValues& input) {
  PreparedDistinctRoot result;
  if (distinct_node.output_descriptor_ids.empty() ||
      distinct_node.output_descriptor_ids != input_node.output_descriptor_ids ||
      distinct_node.bound_expression_ids.size() !=
          distinct_node.output_descriptor_ids.size() ||
      input.result_bindings.size() != input.batch.columns.size() ||
      input_node.output_descriptor_ids.size() != input.batch.columns.size()) {
    result.detail =
        "query DISTINCT does not cover and preserve its projected schema";
    return result;
  }
  if (std::ranges::any_of(dag.outputs, [&](const auto& output) {
        return output.relation_node_id == distinct_node.logical_node_id;
      })) {
    result.detail =
        "query DISTINCT output lineage is not admitted by this profile";
    return result;
  }

  std::unordered_map<std::uint32_t, const api::RelationalExpressionRecord*>
      expressions;
  std::unordered_map<std::uint32_t, const api::RelationalTypeDescriptor*>
      descriptors;
  for (const auto& expression : dag.expressions) {
    expressions.emplace(expression.expression_id, &expression);
  }
  for (const auto& descriptor : dag.descriptors) {
    descriptors.emplace(descriptor.descriptor_id, &descriptor);
  }
  std::unordered_set<std::uint32_t> covered_descriptors;
  for (const auto expression_id : distinct_node.bound_expression_ids) {
    const auto expression = expressions.find(expression_id);
    if (expression == expressions.end() ||
        std::ranges::find(input_node.bound_expression_ids, expression_id) ==
            input_node.bound_expression_ids.end() ||
        std::ranges::find(distinct_node.output_descriptor_ids,
                          expression->second->result_descriptor_id) ==
            distinct_node.output_descriptor_ids.end() ||
        !covered_descriptors
             .insert(expression->second->result_descriptor_id)
             .second) {
      result.detail =
          "query DISTINCT expression coverage is unresolved or duplicated";
      return result;
    }
  }

  for (std::size_t column = 0; column < input.batch.columns.size(); ++column) {
    const auto descriptor_id = distinct_node.output_descriptor_ids[column];
    const auto descriptor = descriptors.find(descriptor_id);
    if (descriptor == descriptors.end() ||
        !covered_descriptors.contains(descriptor_id)) {
      result.equality_terms.clear();
      result.detail = "query DISTINCT descriptor coverage is incomplete";
      return result;
    }
    exec::CanonicalDescriptorOrderTerm term;
    term.column = column;
    term.expression_descriptor_id = descriptor_id;
    term.direction = exec::CanonicalDescriptorOrderDirection::ascending;
    term.null_placement = exec::CanonicalDescriptorNullPlacement::first;
    if (dt::CanonicalTypeIdFromStableName(
            input.batch.columns[column].descriptor.canonical_type_name) ==
        dt::CanonicalTypeId::character) {
      if (!descriptor->second->collation_uuid.has_value()) {
        result.equality_terms.clear();
        result.detail =
            "query DISTINCT character equality lacks a bound collation";
        return result;
      }
      term.collation_uuid = *descriptor->second->collation_uuid;
#if defined(SCRATCHBIRD_QOW_QUERY_ROUTE_CONTRACT_ONLY)
      result.equality_terms.clear();
      result.detail =
          "query DISTINCT character equality requires the production engine "
          "resource catalog";
      return result;
#else
      api::EngineUuid collation_uuid;
      collation_uuid.canonical = term.collation_uuid;
      const auto resolved = api::LookupEngineResourceDescriptorByUuid(
          context, collation_uuid, "collation");
      if (!resolved.ok || !resolved.resource_descriptor.present ||
          resolved.resource_descriptor.resource_uuid.canonical !=
              term.collation_uuid) {
        result.equality_terms.clear();
        result.detail =
            "query DISTINCT character equality lacks current engine "
            "collation authority";
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
    } else if ((input.batch.columns[column].descriptor.canonical_type_name ==
                    "time" ||
                input.batch.columns[column].descriptor.canonical_type_name ==
                    "timestamp") &&
               input.batch.columns[column].descriptor.encoded_descriptor.find(
                   "timezone_profile_id=") != std::string::npos) {
      if (!BindTimezoneOrderAuthority(context, &term, &result.detail)) {
        result.equality_terms.clear();
        return result;
      }
    }
    const auto validation = exec::ValidateCanonicalDescriptorOrderTerm(
        term, input.batch.columns[column]);
    if (!validation.ok) {
      result.equality_terms.clear();
      result.detail = validation.detail;
      return result;
    }
    result.equality_terms.push_back(std::move(term));
  }

  result.result_bindings = input.result_bindings;
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

bool PrepareInputRowBinding(
    const api::TypedRelationalDag& dag,
    const std::uint32_t root_expression_id,
    const std::vector<std::uint32_t>& input_descriptor_ids,
    CanonicalRelationalExpressionRowBinding* row_binding,
    std::string* detail,
    std::uint64_t* preparation_peak_structural_bytes = nullptr) {
  if (preparation_peak_structural_bytes != nullptr) {
    *preparation_peak_structural_bytes = 0;
  }
  if (row_binding == nullptr || detail == nullptr ||
      root_expression_id == 0 || input_descriptor_ids.empty()) {
    if (detail != nullptr) {
      *detail = "row expression binding request is incomplete";
    }
    return false;
  }
  *row_binding = {};
  row_binding->row_descriptor_ids = input_descriptor_ids;

  std::unordered_map<std::uint32_t, std::size_t> input_ordinals;
  for (std::size_t ordinal = 0; ordinal < input_descriptor_ids.size();
       ++ordinal) {
    if (!input_ordinals.emplace(input_descriptor_ids[ordinal], ordinal)
             .second) {
      *row_binding = {};
      *detail = "row expression input descriptor identity is ambiguous";
      return false;
    }
  }
  std::unordered_map<std::uint32_t,
                     const api::RelationalExpressionRecord*>
      expressions;
  for (const auto& expression : dag.expressions) {
    expressions.emplace(expression.expression_id, &expression);
  }
  std::unordered_set<std::uint32_t> reachable;
  std::vector<std::uint32_t> pending{root_expression_id};
  while (!pending.empty()) {
    const auto expression_id = pending.back();
    pending.pop_back();
    if (!reachable.insert(expression_id).second) continue;
    const auto expression = expressions.find(expression_id);
    if (expression == expressions.end()) {
      *row_binding = {};
      *detail = "row expression has a dangling expression child";
      return false;
    }
    if (expression->second->expression_kind ==
        api::RelationalExpressionKind::kIdentifier) {
      const auto ordinal =
          input_ordinals.find(expression->second->result_descriptor_id);
      if (ordinal == input_ordinals.end()) {
        *row_binding = {};
        *detail = "row expression identifier is not supplied by its input";
        return false;
      }
      row_binding->slots.push_back(
          {expression_id, expression->second->result_descriptor_id,
           ordinal->second,
           CanonicalRelationalExpressionRowSlotKind::input_identifier});
    }
    pending.insert(pending.end(),
                   expression->second->child_expression_ids.begin(),
                   expression->second->child_expression_ids.end());
  }
  if (preparation_peak_structural_bytes != nullptr) {
    std::uint64_t bytes = sizeof(*row_binding);
    std::uint64_t allocation = 0;
    const auto add_array = [&](const std::uint64_t count,
                               const std::uint64_t element_bytes) {
      return CheckedMultiply(count, element_bytes, &allocation) &&
             CheckedAdd(bytes, allocation, &bytes);
    };
    // This is the exact live container cohort retained by this implementation
    // under the engine logical-memory convention: bucket arrays, conservative
    // node/link storage, vector capacities, and the surviving row binding.
    constexpr std::uint64_t kNodeLinks = 6 * sizeof(void*);
    if (!add_array(row_binding->row_descriptor_ids.capacity(),
                   sizeof(std::uint32_t)) ||
        !add_array(row_binding->slots.capacity(),
                   sizeof(CanonicalRelationalExpressionRowSlotBinding)) ||
        !add_array(input_ordinals.bucket_count(), sizeof(void*)) ||
        !add_array(input_ordinals.size(),
                   sizeof(std::pair<const std::uint32_t, std::size_t>) +
                       kNodeLinks) ||
        !add_array(expressions.bucket_count(), sizeof(void*)) ||
        !add_array(
            expressions.size(),
            sizeof(std::pair<
                const std::uint32_t,
                const api::RelationalExpressionRecord*>) + kNodeLinks) ||
        !add_array(reachable.bucket_count(), sizeof(void*)) ||
        !add_array(reachable.size(), sizeof(std::uint32_t) + kNodeLinks) ||
        !add_array(pending.capacity(), sizeof(std::uint32_t))) {
      *row_binding = {};
      *detail = "row expression binding memory receipt overflowed";
      return false;
    }
    *preparation_peak_structural_bytes = bytes;
  }
  return true;
}


PreparedSortRoot PrepareExpressionSortRoot(
    const api::EngineRequestContext& context,
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalPropertyCatalog& properties,
    const plan::CanonicalLogicalRelationalNode& root,
    const plan::CanonicalLogicalRelationalNode& input_node,
    const MaterializedValues& input,
    const CanonicalRelationalExpressionRuntimeServices& expression_services) {
  PreparedSortRoot result;
  if (root.output_descriptor_ids.empty() ||
      root.output_descriptor_ids != input_node.output_descriptor_ids ||
      input.result_bindings.size() != input.batch.columns.size() ||
      input_node.output_descriptor_ids.size() != input.batch.columns.size()) {
    result.detail =
        "expression SORT output does not preserve its bound input schema";
    return result;
  }
  if (std::ranges::any_of(dag.outputs, [&](const auto& output) {
        return output.relation_node_id == root.logical_node_id;
      })) {
    result.detail =
        "expression SORT root output lineage is not admitted by this profile";
    return result;
  }
  if (properties.properties.size() != 1 ||
      root.required_property_uuids.size() != 1 ||
      root.delivered_property_uuids.size() != 1 ||
      root.required_property_uuids.front() !=
          root.delivered_property_uuids.front()) {
    result.detail =
        "expression SORT requires exactly one enforced ordering property";
    return result;
  }
  const auto& property = properties.properties.front();
  if (property.property_uuid != root.required_property_uuids.front() ||
      property.property_kind !=
          plan::CanonicalLogicalPropertyKind::kOrdering ||
      property.origin_logical_node_id != root.logical_node_id ||
      property.ordering_terms.empty() ||
      root.bound_expression_ids.size() != property.ordering_terms.size()) {
    result.detail =
        "expression SORT ordering property or expression coverage is invalid";
    return result;
  }

  std::unordered_map<std::uint32_t, const api::RelationalExpressionRecord*>
      expression_records;
  std::unordered_map<std::uint32_t, const api::RelationalTypeDescriptor*>
      descriptors;
  std::unordered_set<std::uint32_t> materialized_descriptor_ids(
      input_node.output_descriptor_ids.begin(),
      input_node.output_descriptor_ids.end());
  for (const auto& expression : dag.expressions) {
    expression_records.emplace(expression.expression_id, &expression);
  }
  for (const auto& descriptor : dag.descriptors) {
    descriptors.emplace(descriptor.descriptor_id, &descriptor);
  }

  CanonicalRelationalExpressionRuntime runtime(dag, expression_services);
  std::vector<api::EngineTypedValue> descriptor_only_input_row;
  if (input.batch.rows.empty()) {
    descriptor_only_input_row.reserve(input.batch.columns.size());
    for (const auto& column : input.batch.columns) {
      api::EngineTypedValue value;
      value.descriptor = column.descriptor;
      value.state = api::EngineValueState::value;
      value.is_null = false;
      descriptor_only_input_row.push_back(std::move(value));
    }
  }
  const auto& type_inference_row =
      input.batch.rows.empty() ? descriptor_only_input_row
                               : input.batch.rows.front().values;
  for (std::size_t ordinal = 0; ordinal < property.ordering_terms.size();
       ++ordinal) {
    const auto& logical_term = property.ordering_terms[ordinal];
    if (root.bound_expression_ids[ordinal] != logical_term.expression_id) {
      result.detail =
          "expression SORT bound-expression order differs from its property";
      return result;
    }
    const auto expression =
        expression_records.find(logical_term.expression_id);
    if (expression == expression_records.end()) {
      result.detail = "expression SORT term is unresolved";
      return result;
    }
    const auto descriptor =
        descriptors.find(expression->second->result_descriptor_id);
    if (descriptor == descriptors.end() ||
        descriptor->second->nullability ==
            api::RelationalNullability::kUnknown ||
        !materialized_descriptor_ids
             .insert(expression->second->result_descriptor_id)
             .second) {
      result.detail =
          "expression SORT result descriptor is absent, ambiguous, or "
          "unresolved";
      return result;
    }

    PreparedSortExpression prepared_expression;
    prepared_expression.expression_id = logical_term.expression_id;
    if (!PrepareInputRowBinding(
            dag, prepared_expression.expression_id,
            input_node.output_descriptor_ids,
            &prepared_expression.row_binding, &result.detail)) {
      return result;
    }
    const bool row_independent =
        prepared_expression.row_binding.slots.empty();
    const bool inferred =
        row_independent
            ? runtime.InferType(
                  prepared_expression.expression_id, std::nullopt,
                  &prepared_expression.expected_type, &result.detail)
            : runtime.InferTypeForConsumer(
                  prepared_expression.expression_id,
                  prepared_expression.row_binding, type_inference_row,
                  api::EngineCanonicalExpressionConsumer::projection,
                  &prepared_expression.expected_type, &result.detail);
    if (!inferred ||
        prepared_expression.expected_type.empty() ||
        prepared_expression.expected_type == "null") {
      if (result.detail.empty()) {
        result.detail = "expression SORT result type is unresolved";
      }
      return result;
    }
    const auto type_id = dt::CanonicalTypeIdFromStableName(
        prepared_expression.expected_type);
    if (type_id == dt::CanonicalTypeId::unknown ||
        (descriptor->second->collation_uuid.has_value() &&
         type_id != dt::CanonicalTypeId::character) ||
        (descriptor->second->timezone_profile_id.has_value() &&
         prepared_expression.expected_type != "timestamp")) {
      result.detail =
          "expression SORT descriptor metadata contradicts its result type";
      return result;
    }
    api::EngineTypedValue first_value;
    const bool evaluated =
        row_independent
            ? runtime.EvaluateForConsumer(
                  prepared_expression.expression_id,
                  prepared_expression.expected_type,
                  api::EngineCanonicalExpressionConsumer::projection,
                  &first_value, &result.detail)
            : runtime.EvaluateForConsumer(
                  prepared_expression.expression_id,
                  prepared_expression.expected_type,
                  prepared_expression.row_binding, type_inference_row,
                  api::EngineCanonicalExpressionConsumer::projection,
                  &first_value, &result.detail);
    if (!evaluated ||
        first_value.descriptor.canonical_type_name !=
            prepared_expression.expected_type) {
      if (result.detail.empty()) {
        result.detail = "expression SORT first value has an invalid type";
      }
      return result;
    }
    prepared_expression.materialized_column = {
        "__sort_expression_" +
            std::to_string(prepared_expression.expression_id),
        first_value.descriptor,
        descriptor->second->nullability ==
            api::RelationalNullability::kNullable,
        descriptor->second->descriptor_id};
    if (prepared_expression.row_binding.slots.empty()) {
      prepared_expression.row_independent_value = first_value;
    }
    exec::CanonicalDescriptorOrderTerm term;
    if (!PrepareCanonicalSortOrderTerm(
            context, logical_term,
            prepared_expression.materialized_column,
            input.batch.columns.size() + result.expressions.size(), &term,
            &result.detail)) {
      return result;
    }
    result.order_terms.push_back(std::move(term));
    result.expressions.push_back(std::move(prepared_expression));
  }

  if (!input.batch.rows.empty() &&
      !MaterializeExpressionSortBatch(
          dag, result.expressions, input.batch, expression_services,
          &result.expression_input_batch, &result.detail)) {
    result.order_terms.clear();
    result.expressions.clear();
    return result;
  }
  result.expression_ordering = true;
  result.result_bindings = input.result_bindings;
  result.ordering_property_uuid = property.property_uuid;
  result.ok = true;
  return result;
}


PreparedProjectRoot PrepareExpressionProjectRoot(
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalRelationalNode& root,
    const plan::CanonicalLogicalRelationalNode& input_node,
    const MaterializedValues& input,
    const CanonicalRelationalExpressionRuntimeServices& expression_services) {
  PreparedProjectRoot result;
  if (root.output_descriptor_ids.empty() ||
      root.bound_expression_ids.size() != root.output_descriptor_ids.size() ||
      input.result_bindings.size() != input.batch.columns.size() ||
      input_node.output_descriptor_ids.size() != input.batch.columns.size()) {
    result.detail =
        "expression PROJECT input, output, or expression coverage is incomplete";
    return result;
  }

  std::vector<const api::RelationalOutputRecord*> outputs;
  for (const auto& output : dag.outputs) {
    if (output.relation_node_id == root.logical_node_id) {
      outputs.push_back(&output);
    }
  }
  std::ranges::sort(outputs, {}, &api::RelationalOutputRecord::ordinal);
  if (outputs.size() != root.output_descriptor_ids.size()) {
    result.detail = "expression PROJECT root output lineage is incomplete";
    return result;
  }

  std::unordered_map<std::uint32_t, const api::RelationalTypeDescriptor*>
      descriptors;
  std::unordered_map<std::uint32_t,
                     const api::RelationalExpressionRecord*>
      expression_records;
  for (const auto& descriptor : dag.descriptors) {
    descriptors.emplace(descriptor.descriptor_id, &descriptor);
  }
  for (const auto& expression : dag.expressions) {
    expression_records.emplace(expression.expression_id, &expression);
  }

  CanonicalRelationalExpressionRuntime runtime(dag, expression_services);
  std::vector<api::EngineTypedValue> descriptor_only_input_row;
  if (input.batch.rows.empty()) {
    descriptor_only_input_row.reserve(input.batch.columns.size());
    for (const auto& column : input.batch.columns) {
      api::EngineTypedValue value;
      value.descriptor = column.descriptor;
      value.state = api::EngineValueState::value;
      value.is_null = false;
      descriptor_only_input_row.push_back(std::move(value));
    }
  }
  const auto& type_inference_row =
      input.batch.rows.empty() ? descriptor_only_input_row
                               : input.batch.rows.front().values;
  std::vector<exec::ExecutorColumnDescriptor> output_columns;
  output_columns.reserve(outputs.size());
  std::size_t published_ordinal = 0;
  for (std::size_t ordinal = 0; ordinal < outputs.size(); ++ordinal) {
    const auto expression_id = root.bound_expression_ids[ordinal];
    const auto expression = expression_records.find(expression_id);
    const auto descriptor = descriptors.find(root.output_descriptor_ids[ordinal]);
    const auto* output = outputs[ordinal];
    if (expression == expression_records.end() ||
        descriptor == descriptors.end() || output->ordinal != ordinal ||
        output->expression_id != expression_id ||
        output->descriptor_id != root.output_descriptor_ids[ordinal] ||
        expression->second->result_descriptor_id !=
            root.output_descriptor_ids[ordinal] ||
        output->output_name_utf8.empty() ||
        descriptor->second->nullability ==
            api::RelationalNullability::kUnknown) {
      result.detail =
          "expression PROJECT output expression or descriptor binding is invalid";
      return result;
    }

    PreparedProjectExpression prepared_expression;
    prepared_expression.expression_id = expression_id;
    if (!PrepareInputRowBinding(
            dag, expression_id, input_node.output_descriptor_ids,
            &prepared_expression.row_binding, &result.detail) ||
        !runtime.InferTypeForConsumer(
            expression_id, prepared_expression.row_binding,
            type_inference_row,
            api::EngineCanonicalExpressionConsumer::projection,
            &prepared_expression.expected_type, &result.detail) ||
        prepared_expression.expected_type.empty() ||
        prepared_expression.expected_type == "null") {
      if (result.detail.empty()) {
        result.detail = "expression PROJECT result type is unresolved";
      }
      return result;
    }
    const auto type_id = dt::CanonicalTypeIdFromStableName(
        prepared_expression.expected_type);
    if (type_id == dt::CanonicalTypeId::unknown ||
        (descriptor->second->collation_uuid.has_value() &&
         type_id != dt::CanonicalTypeId::character) ||
        (descriptor->second->timezone_profile_id.has_value() &&
         prepared_expression.expected_type != "timestamp")) {
      result.detail =
          "expression PROJECT descriptor metadata contradicts its result type";
      return result;
    }
    api::EngineDescriptor output_descriptor;
    if (!input.batch.rows.empty()) {
      api::EngineTypedValue first_value;
      if (!runtime.EvaluateForConsumer(
              expression_id, prepared_expression.expected_type,
              prepared_expression.row_binding,
              input.batch.rows.front().values,
              api::EngineCanonicalExpressionConsumer::projection,
              &first_value, &result.detail)) {
        return result;
      }
      output_descriptor = std::move(first_value.descriptor);
    } else {
      const auto& source = *descriptor->second;
      output_descriptor.descriptor_uuid.canonical = source.descriptor_uuid;
      output_descriptor.descriptor_kind = "scalar";
      output_descriptor.canonical_type_name =
          prepared_expression.expected_type;
      output_descriptor.encoded_descriptor =
          "type_uuid=" + source.type_uuid + ";nullability=" +
          (source.nullability == api::RelationalNullability::kNullable
               ? "nullable"
               : "non_null");
      if (source.collation_uuid.has_value()) {
        output_descriptor.encoded_descriptor +=
            ";collation_uuid=" + *source.collation_uuid;
      }
      if (source.timezone_profile_id.has_value()) {
        output_descriptor.encoded_descriptor +=
            ";timezone_profile_id=" + *source.timezone_profile_id;
      }
      if (source.width.has_value()) {
        output_descriptor.encoded_descriptor +=
            ";width=" + std::to_string(*source.width);
      }
      if (source.precision.has_value()) {
        output_descriptor.encoded_descriptor +=
            ";precision=" + std::to_string(*source.precision);
      }
      if (source.scale.has_value()) {
        output_descriptor.encoded_descriptor +=
            ";scale=" + std::to_string(*source.scale);
      }
    }
    output_columns.push_back(
        {output->output_name_utf8, std::move(output_descriptor),
         descriptor->second->nullability ==
             api::RelationalNullability::kNullable,
         descriptor->second->descriptor_id});
    result.expressions.push_back(std::move(prepared_expression));

    exec::CanonicalResultColumnBinding binding;
    binding.physical_column_ordinal = ordinal;
    binding.visible = output->visible;
    if (binding.visible) {
      binding.published_descriptor = exec::CanonicalResultColumnDescriptor{
          static_cast<std::uint32_t>(published_ordinal++),
          output->output_name_utf8,
          descriptor->second->descriptor_uuid,
          descriptor->second->type_uuid,
          ResultNullability(descriptor->second->nullability),
          descriptor->second->collation_uuid,
          descriptor->second->timezone_profile_id};
    }
    result.result_bindings.push_back(std::move(binding));
  }

  if (!MaterializeExpressionProjectBatch(
          dag, result.expressions, output_columns, input.batch,
          expression_services, &result.expression_output_batch,
          &result.detail)) {
    result.expressions.clear();
    result.result_bindings.clear();
    return result;
  }
  result.expression_projection = true;
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
  if (root.bound_expression_ids.size() != 1) {
    result.detail = "filter predicate root identity is not exact";
    return result;
  }
  result.predicate_expression_id = root.bound_expression_ids.front();
  if (!PrepareInputRowBinding(
          dag, result.predicate_expression_id,
          input_node.output_descriptor_ids, &result.predicate_row_binding,
          &result.detail)) {
    return result;
  }
  result.result_bindings = input.result_bindings;
  result.ok = true;
  return result;
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
  if (!runtime->EvaluateForConsumer(
          expression_id, "int64",
          api::EngineCanonicalExpressionConsumer::projection, &value,
          refusal_detail)) {
    return false;
  }
  std::int64_t decoded = 0;
  if (!DecodeCanonicalInt64Scalar(value, &decoded, refusal_detail)) {
    return false;
  }
  if (decoded < 0) {
    *refusal_detail = "row bound is negative or outside exact int64 admission";
    return false;
  }
  *row_bound = static_cast<std::uint64_t>(decoded);
  return true;
}

}  // namespace

PreparedFilterRoot PrepareFilterRootForComposition(
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalRelationalNode& root,
    const plan::CanonicalLogicalRelationalNode& input_node,
    const MaterializedValues& input) {
  return PrepareFilterRoot(dag, root, input_node, input);
}

PreparedProjectRoot PrepareExpressionProjectRootForComposition(
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalRelationalNode& root,
    const plan::CanonicalLogicalRelationalNode& input_node,
    const MaterializedValues& input,
    const CanonicalRelationalExpressionRuntimeServices& expression_services) {
  return PrepareExpressionProjectRoot(
      dag, root, input_node, input, expression_services);
}

PreparedProjectRoot PrepareDescriptorDirectProjectRootForComposition(
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalRelationalNode& root,
    const plan::CanonicalLogicalRelationalNode& input_node,
    const MaterializedValues& input) {
  return PrepareDescriptorDirectProjectRoot(dag, root, input_node, input);
}





PreparedDistinctRoot PrepareQueryDistinctRootForComposition(
    const api::EngineRequestContext& context,
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalRelationalNode& distinct_node,
    const plan::CanonicalLogicalRelationalNode& input_node,
    const MaterializedValues& input) {
  return PrepareQueryDistinctRoot(
      context, dag, distinct_node, input_node, input);
}

PreparedLimitRoot PrepareLimitRootForComposition(
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalRelationalNode& root,
    const plan::CanonicalLogicalRelationalNode& input_node,
    const MaterializedValues& input) {
  return PrepareLimitRoot(dag, root, input_node, input);
}

bool PrepareCanonicalSortOrderTermForComposition(
    const api::EngineRequestContext& context,
    const plan::CanonicalLogicalPropertyOrderingTerm& logical_term,
    const exec::ExecutorColumnDescriptor& column,
    const std::size_t column_ordinal,
    exec::CanonicalDescriptorOrderTerm* term,
    std::string* detail) {
  return PrepareCanonicalSortOrderTerm(context, logical_term, column,
                                       column_ordinal, term, detail);
}

PreparedSortRoot PrepareSortRootForComposition(
    const api::EngineRequestContext& context,
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalPropertyCatalog& properties,
    const plan::CanonicalLogicalRelationalNode& root,
    const plan::CanonicalLogicalRelationalNode& input_node,
    const MaterializedValues& input) {
  return PrepareSortRoot(context, dag, properties, root, input_node, input);
}

PreparedSortRoot PrepareExpressionSortRootForComposition(
    const api::EngineRequestContext& context,
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalPropertyCatalog& properties,
    const plan::CanonicalLogicalRelationalNode& root,
    const plan::CanonicalLogicalRelationalNode& input_node,
    const MaterializedValues& input,
    const CanonicalRelationalExpressionRuntimeServices& expression_services) {
  return PrepareExpressionSortRoot(
      context, dag, properties, root, input_node, input, expression_services);
}

bool EvaluateNonNegativeRowBoundForComposition(
    CanonicalRelationalExpressionRuntime* runtime,
    const std::uint32_t expression_id,
    std::uint64_t* value,
    std::string* detail) {
  return EvaluateNonNegativeRowBound(runtime, expression_id, value, detail);
}

LiveProjectRegistrationProfile
MakeLiveProjectRegistrationProfileForComposition(
    const PreparedProjectRoot& prepared_root) {
  return MakeLiveProjectRegistrationProfile(prepared_root);
}

bool PrepareInputRowBindingForComposition(
    const api::TypedRelationalDag& dag,
    const std::uint32_t root_expression_id,
    const std::vector<std::uint32_t>& input_descriptor_ids,
    CanonicalRelationalExpressionRowBinding* row_binding,
    std::string* detail,
    std::uint64_t* preparation_peak_structural_bytes) {
  return PrepareInputRowBinding(
      dag, root_expression_id, input_descriptor_ids, row_binding, detail,
      preparation_peak_structural_bytes);
}

}  // namespace scratchbird::engine::sblr
