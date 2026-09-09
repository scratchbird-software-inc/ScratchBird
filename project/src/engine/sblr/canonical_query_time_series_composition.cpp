// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_query_time_series_composition.hpp"

#include "canonical_query_aggregate_composition.hpp"
#include "canonical_query_aggregate_registration.hpp"
#include "canonical_query_descriptor_support.hpp"
#include "canonical_query_filter_registration.hpp"
#include "canonical_query_join_registration.hpp"
#include "canonical_query_node_composition.hpp"
#include "canonical_query_object_free_composition_support.hpp"
#include "canonical_query_object_free_profile.hpp"
#include "canonical_query_physical_registration.hpp"
#include "canonical_query_recursive_registration.hpp"
#include "canonical_query_relational_registration.hpp"
#include "canonical_query_runtime_memory_support.hpp"
#include "canonical_query_scalar_support.hpp"
#include "canonical_query_set_composition.hpp"
#include "canonical_query_set_registration.hpp"
#include "canonical_query_sort_registration.hpp"
#include "canonical_query_time_series_endpoint.hpp"
#include "canonical_query_window_registration.hpp"
#include "canonical_relational_expression.hpp"

#include "catalog/name_resolution_api.hpp"
#include "datatype_catalog_manifest.hpp"
#include "datatype_operations.hpp"
#include "engine/executor/executor_foundation.hpp"
#include "engine/internal_api/mga_relation_store/mga_relation_store.hpp"
#include "engine/optimizer/model_family_profile_factory.hpp"
#include "engine/optimizer/optimizer_contract.hpp"
#include "engine/optimizer/relational_planner.hpp"
#include "hash_digest.hpp"
#include "nosql/nosql_provider_generation_store.hpp"
#include "nosql/time_series_api.hpp"
#include "query/canonical_heap_optimizer_admission.hpp"
#include "query/canonical_relational_bridge.hpp"
#include "query/expression_api.hpp"
#include "security/security_model.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <sstream>
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
namespace opt = scratchbird::engine::optimizer;
namespace plan = scratchbird::engine::planner;

namespace {

// SEARCH_KEY: SB_ENGINE_CANONICAL_QUERY_TIME_SERIES_COMPOSITION_AUTHORITY
// Owns the admitted production time-series source and its bounded relational
// composition. It borrows and revalidates engine-issued MGA statement context;
// it cannot create snapshots or publish transaction finality.

constexpr std::string_view kValuesImplementationId =
    "values.materialize.canonical.v1";

}  // namespace

// QOW-SOURCE-RCP-076-TIME-SERIES-CANONICAL-QUERY-ROUTE-V1
CanonicalObjectFreeValuesExecutionResult ExecuteCanonicalTimeSeriesFamilyQuery(
    const CanonicalCurrentHeapExecutionRequest& input,
    Rcp079CapturedModelLegV1* leg_capture) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto& dag = input.relational_dag;
  const auto source = std::ranges::find_if(dag.nodes, [](const auto& node) {
    return node.semantic_variant_id == "SBLR_MODEL_SOURCE_V1" ||
           node.semantic_variant_id == "SBLR_MODEL_AGGREGATE_V1";
  });
  const bool time_expression = std::ranges::any_of(
      dag.expressions, [](const auto& expression) {
        return expression.operator_name == "TIME_RANGE" ||
               expression.operator_name == "TIME_BUCKET" ||
               expression.operator_name == "TIME_DOWNSAMPLE";
      });
  if (source == dag.nodes.end() || !time_expression) return result;
  result.profile_matched = true;
  CanonicalObjectFreeValuesExecutionRequest response_context;
  response_context.context = input.context;
  response_context.relational_dag = dag;
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
    result.api_result = Failure(response_context, std::move(diagnostic_id),
                                std::move(detail));
    result.api_result.evidence.push_back(
        {"canonical.time_series_source_execution_started", "false"});
    result.api_result.evidence.push_back(
        {"canonical.time_series_source_data_access_observed", "false"});
    result.api_result.evidence.push_back(
        {"canonical.time_series_cleanup_count", "0"});
    result.api_result.evidence.push_back(
        {"canonical.time_series_cleanup_complete", "false"});
    return result;
  };
  const auto replace_source_receipt =
      [&](const bool execution_started, const bool data_access_observed,
          const std::uint32_t cleanup_count, const bool cleanup_complete) {
        std::erase_if(result.api_result.evidence, [](const auto& evidence) {
          return evidence.evidence_kind ==
                     "canonical.time_series_source_execution_started" ||
                 evidence.evidence_kind ==
                     "canonical.time_series_source_data_access_observed" ||
                 evidence.evidence_kind ==
                     "canonical.time_series_cleanup_count" ||
                 evidence.evidence_kind ==
                     "canonical.time_series_cleanup_complete";
        });
        result.api_result.evidence.push_back(
            {"canonical.time_series_source_execution_started",
             execution_started ? "true" : "false"});
        result.api_result.evidence.push_back(
            {"canonical.time_series_source_data_access_observed",
             data_access_observed ? "true" : "false"});
        result.api_result.evidence.push_back(
            {"canonical.time_series_cleanup_count",
             std::to_string(cleanup_count)});
        result.api_result.evidence.push_back(
            {"canonical.time_series_cleanup_complete",
             cleanup_complete ? "true" : "false"});
      };

  // The admitted typed DAG is bound to the engine-issued statement/MGA
  // boundary.  Check that boundary before generic graph validation so a
  // substituted snapshot can never be reported as an ordinary graph-shape
  // failure (and, more importantly, can never reach planning or access).
  if (dag.statement_uuid != input.context.statement_uuid.canonical ||
      dag.statement_timestamp != input.context.statement_timestamp ||
      dag.owning_transaction_uuid !=
          input.context.transaction_uuid.canonical ||
      dag.statement_snapshot_uuid !=
          input.context.statement_snapshot_uuid.canonical ||
      dag.statement_metadata_snapshot_uuid !=
          input.context.statement_metadata_snapshot_uuid.canonical ||
      dag.local_transaction_id != input.context.local_transaction_id ||
      dag.snapshot_visible_through_local_transaction_id !=
          input.context.snapshot_visible_through_local_transaction_id) {
    return refuse("SB_MODEL_MGA_CONTEXT_MISMATCH_V1",
                  "time-series typed DAG statement/MGA boundary changed");
  }
  if (dag.bound_catalog_epoch_uuid !=
      input.context.catalog_epoch_uuid.canonical) {
    return refuse("SB_MODEL_CATALOG_GENERATION_STALE_V1",
                  "time-series typed DAG bound catalog epoch changed");
  }
  if (dag.bound_security_context_uuid !=
      input.context.authorization_context.authority_uuid.canonical) {
    return refuse("SB_MODEL_SECURITY_ADMISSION_REFUSED_V1",
                  "time-series typed DAG bound security context changed");
  }

  const auto validation = api::ValidateTypedRelationalDag(dag);
  if (!validation.accepted || !validation.issues.empty()) {
    return refuse(validation.issues.empty()
                      ? "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1"
                      : validation.issues.front().diagnostic_id,
                  validation.issues.empty()
                      ? "time-series typed DAG validation failed"
                      : validation.issues.front().field_id);
  }
  const auto typed_node_for = [&](const std::uint32_t node_id) {
    return std::ranges::find_if(dag.nodes, [&](const auto& node) {
      return node.node_id == node_id;
    });
  };
  const api::RelationalDagNode* recursive_root = nullptr;
  const api::RelationalDagNode* recursive_term = nullptr;
  const api::RelationalDagNode* set_root = nullptr;
  const api::RelationalDagNode* set_values = nullptr;
  const api::RelationalDagNode* mixed_join = nullptr;
  const api::RelationalDagNode* asof_join = nullptr;
  const api::RelationalDagNode* relational_scan = nullptr;
  LiveSetOperationProfile time_series_set_profile;
  std::uint32_t composition_root_node_id = dag.root_node_id;
  const auto requested_root = typed_node_for(dag.root_node_id);
  if (requested_root != dag.nodes.end() &&
      requested_root->node_kind ==
          api::RelationalDagNodeKind::kSetOperation) {
    time_series_set_profile = ResolveLiveSetOperationProfileForComposition(
        requested_root->semantic_variant_id);
    if (!time_series_set_profile.matched ||
        time_series_set_profile.operation !=
            exec::CanonicalSetOperationKind::kUnion ||
        time_series_set_profile.quantifier !=
            exec::CanonicalSetOperationQuantifier::kAll ||
        time_series_set_profile.alignment !=
            exec::CanonicalSetOperationAlignment::kOrdinal ||
        time_series_set_profile.type_profile !=
            exec::CanonicalSetOperationTypeProfile::kExact ||
        time_series_set_profile.equality_profile !=
            exec::CanonicalSetOperationEqualityProfile::kExactTyped ||
        requested_root->input_node_ids.size() != 2 ||
        requested_root->input_node_ids[0] ==
            requested_root->input_node_ids[1] ||
        !requested_root->bound_expression_ids.empty() ||
        !requested_root->required_object_uuids.empty() ||
        !requested_root->required_property_uuids.empty() ||
        !requested_root->delivered_property_uuids.empty()) {
      return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                    "time-series set root is not exact ordinal UNION ALL");
    }
    const auto left = typed_node_for(requested_root->input_node_ids[0]);
    const auto right = typed_node_for(requested_root->input_node_ids[1]);
    if (left == dag.nodes.end() || right == dag.nodes.end() ||
        right->node_kind != api::RelationalDagNodeKind::kValues ||
        right->semantic_variant_id != "values.literal-table.v1" ||
        !right->input_node_ids.empty() || right->values_row_ids.empty() ||
        !right->bound_expression_ids.empty() ||
        !right->required_object_uuids.empty() ||
        !right->required_property_uuids.empty() ||
        !right->delivered_property_uuids.empty() ||
        requested_root->output_descriptor_ids !=
            left->output_descriptor_ids ||
        requested_root->output_descriptor_ids !=
            right->output_descriptor_ids) {
      return refuse(
          "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
          "time-series UNION ALL right VALUES input or schema is not exact");
    }
    set_root = &*requested_root;
    set_values = &*right;
    composition_root_node_id = left->node_id;
  } else if (requested_root != dag.nodes.end() &&
             requested_root->node_kind ==
                 api::RelationalDagNodeKind::kRecursiveCte) {
    const auto recursive_profile = MatchLiveRecursiveCteProfileForComposition(
        requested_root->semantic_variant_id);
    if (!recursive_profile.matched || recursive_profile.search_cycle ||
        recursive_profile.union_mode !=
            exec::CanonicalRecursiveCteUnionMode::kAll ||
        requested_root->input_node_ids.size() != 2 ||
        requested_root->input_node_ids[0] ==
            requested_root->input_node_ids[1] ||
        requested_root->bound_expression_ids.size() != 1 ||
        !requested_root->required_object_uuids.empty() ||
        !requested_root->required_property_uuids.empty() ||
        !requested_root->delivered_property_uuids.empty()) {
      return refuse(
          "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
          "time-series recursive root is not exact bounded UNION ALL");
    }
    const auto anchor = typed_node_for(requested_root->input_node_ids[0]);
    const auto term = typed_node_for(requested_root->input_node_ids[1]);
    if (anchor == dag.nodes.end() || term == dag.nodes.end() ||
        anchor->node_kind != api::RelationalDagNodeKind::kAggregate ||
        term->node_kind != api::RelationalDagNodeKind::kCte ||
        term->semantic_variant_id !=
            "cte.recursive-term-int64-increment.v1" ||
        !term->input_node_ids.empty() ||
        !term->bound_expression_ids.empty() ||
        !term->required_object_uuids.empty() ||
        !term->required_property_uuids.empty() ||
        !term->delivered_property_uuids.empty() ||
        anchor->output_descriptor_ids != term->output_descriptor_ids ||
        anchor->output_descriptor_ids !=
            requested_root->output_descriptor_ids) {
      return refuse(
          "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
          "time-series recursive anchor, term, or schema is not exact");
    }
    recursive_root = &*requested_root;
    recursive_term = &*term;
    composition_root_node_id = anchor->node_id;
  }
  std::vector<const api::RelationalDagNode*> consumer_chain;
  std::unordered_set<std::uint32_t> reachable_node_ids;
  if (recursive_root != nullptr) {
    reachable_node_ids.insert(recursive_root->node_id);
    reachable_node_ids.insert(recursive_term->node_id);
  }
  if (set_root != nullptr) {
    reachable_node_ids.insert(set_root->node_id);
    reachable_node_ids.insert(set_values->node_id);
  }
  auto chain_node = typed_node_for(composition_root_node_id);
  while (chain_node != dag.nodes.end() &&
         chain_node->node_id != source->node_id) {
    if (!reachable_node_ids.insert(chain_node->node_id).second) {
      return refuse("SBLR.PLAN_TREE.INVALID_HANDLE",
                    "time-series downstream composition contains a cycle");
    }
    if (chain_node->node_kind == api::RelationalDagNodeKind::kJoin) {
      if (mixed_join != nullptr || asof_join != nullptr ||
          !consumer_chain.empty() ||
          chain_node->input_node_ids.size() != 2 ||
          chain_node->input_node_ids[0] == chain_node->input_node_ids[1] ||
          std::ranges::find(chain_node->input_node_ids, source->node_id) ==
              chain_node->input_node_ids.end()) {
        return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                      "time-series mixed JOIN shape is not exact");
      }
      const auto other_node_id =
          chain_node->input_node_ids[0] == source->node_id
              ? chain_node->input_node_ids[1]
              : chain_node->input_node_ids[0];
      const auto other = typed_node_for(other_node_id);
      if (other == dag.nodes.end() ||
          other->node_kind != api::RelationalDagNodeKind::kScan ||
          other->semantic_variant_id == "SBLR_MODEL_SOURCE_V1" ||
          !other->input_node_ids.empty() ||
          other->required_object_uuids.size() != 1 ||
          !reachable_node_ids.insert(other->node_id).second) {
        return refuse(
            "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
            "time-series JOIN relational input is not one bound heap scan");
      }
      const bool asof_semantic =
          chain_node->semantic_variant_id == "join.asof.left.v1" ||
          chain_node->semantic_variant_id == "join.asof.inner.v1";
      if (asof_semantic) {
        asof_join = &*chain_node;
      } else {
        mixed_join = &*chain_node;
      }
      relational_scan = &*other;
      break;
    }
    if (chain_node->input_node_ids.size() != 1 ||
        (chain_node->node_kind != api::RelationalDagNodeKind::kFilter &&
         chain_node->node_kind != api::RelationalDagNodeKind::kProject &&
         chain_node->node_kind != api::RelationalDagNodeKind::kSort &&
         chain_node->node_kind != api::RelationalDagNodeKind::kWindow &&
         chain_node->node_kind != api::RelationalDagNodeKind::kAggregate &&
         chain_node->node_kind != api::RelationalDagNodeKind::kCte &&
         chain_node->node_kind != api::RelationalDagNodeKind::kLimit)) {
      return refuse(
          "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
          "time-series downstream composition is not a supported canonical chain");
    }
    consumer_chain.push_back(&*chain_node);
    chain_node = typed_node_for(chain_node->input_node_ids.front());
  }
  if (mixed_join == nullptr && asof_join == nullptr &&
      chain_node == dag.nodes.end()) {
    return refuse("SBLR.PLAN_TREE.INVALID_HANDLE",
                  "time-series downstream composition does not reach its source");
  }
  if (!reachable_node_ids.insert(source->node_id).second ||
      reachable_node_ids.size() != dag.nodes.size()) {
    return refuse("SBLR.PLAN_TREE.INVALID_HANDLE",
                  "time-series downstream composition is disconnected");
  }
  std::ranges::reverse(consumer_chain);
  const bool standalone_time_series_source =
      consumer_chain.empty() && set_root == nullptr &&
      recursive_root == nullptr && mixed_join == nullptr &&
      asof_join == nullptr;
  result.optimizer_admitted = true;
  const auto expression_named = [&](const std::string_view name) {
    return std::ranges::find_if(dag.expressions, [&](const auto& expression) {
      return expression.operator_name == name;
    });
  };
  const auto range = expression_named("TIME_RANGE");
  const auto bucket = expression_named("TIME_BUCKET");
  const auto downsample = expression_named("TIME_DOWNSAMPLE");
  const bool bucket_operation = bucket != dag.expressions.end();
  const bool downsample_operation = downsample != dag.expressions.end();
  if (range == dag.expressions.end() ||
      source->required_object_uuids.size() != 1 ||
      range->child_expression_ids.size() != 3 ||
      (bucket_operation && bucket->child_expression_ids.size() != 2) ||
      (downsample_operation && downsample->child_expression_ids.size() != 3)) {
    return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                  "time-series source operation is incomplete");
  }
  const auto expression_for = [&](const std::uint32_t expression_id) {
    return std::ranges::find_if(dag.expressions, [&](const auto& expression) {
      return expression.expression_id == expression_id;
    });
  };
  const auto start_expression = expression_for(range->child_expression_ids[1]);
  const auto end_expression = expression_for(range->child_expression_ids[2]);
  if (start_expression == dag.expressions.end() ||
      end_expression == dag.expressions.end() ||
      !start_expression->literal_or_parameter_ref.has_value() ||
      !end_expression->literal_or_parameter_ref.has_value()) {
    return refuse("SB_MODEL_TIME_SERIES_RANGE_INVALID_V1",
                  "time-series range endpoint literal is absent");
  }
  const auto start_descriptor =
      std::ranges::find_if(dag.descriptors, [&](const auto& descriptor) {
        return descriptor.descriptor_id ==
               start_expression->result_descriptor_id;
      });
  const auto end_descriptor =
      std::ranges::find_if(dag.descriptors, [&](const auto& descriptor) {
        return descriptor.descriptor_id ==
               end_expression->result_descriptor_id;
      });
  if (start_descriptor == dag.descriptors.end() ||
      end_descriptor == dag.descriptors.end()) {
    return refuse("SB_MODEL_TIME_SERIES_TIMESTAMP_INVALID_V1",
                  "time-series range descriptor is absent");
  }
  api::EngineTypedValue range_start;
  range_start.descriptor.descriptor_uuid.canonical =
      start_descriptor->descriptor_uuid;
  range_start.descriptor.descriptor_kind = "scalar";
  range_start.descriptor.canonical_type_name = "timestamp_tz";
  range_start.descriptor.encoded_descriptor = "timezone_profile_id=UTC";
  range_start.encoded_value = *start_expression->literal_or_parameter_ref;
  range_start.setState(api::EngineValueState::value);
  auto range_end = range_start;
  range_end.descriptor.descriptor_uuid.canonical =
      end_descriptor->descriptor_uuid;
  range_end.encoded_value = *end_expression->literal_or_parameter_ref;
  std::int64_t canonical_range_start_ns = 0;
  std::int64_t canonical_range_end_ns = 0;
  if (!canonical_query_execute_detail::ParseTimeSeriesEndpointNsV1(
          range_start.encoded_value, &canonical_range_start_ns) ||
      !canonical_query_execute_detail::ParseTimeSeriesEndpointNsV1(
          range_end.encoded_value, &canonical_range_end_ns)) {
    return refuse("SB_MODEL_TIME_SERIES_TIMESTAMP_INVALID_V1",
                  "time-series range endpoint is malformed or out of range");
  }
  if (canonical_range_start_ns > canonical_range_end_ns) {
    return refuse("SB_MODEL_TIME_SERIES_RANGE_INVALID_V1",
                  "time-series range start exceeds its end");
  }

  api::EngineBoundTimeSeriesAggregateV1 aggregate =
      api::EngineBoundTimeSeriesAggregateV1::kNone;
  std::int64_t bucket_interval_ns = 0;
  const auto parse_interval = [](const std::string_view encoded,
                                 std::int64_t* interval_ns) {
    if (interval_ns == nullptr || encoded.empty()) return false;
    if (std::ranges::all_of(encoded, [](const char ch) {
          return ch >= '0' && ch <= '9';
        })) {
      const auto parsed = std::from_chars(
          encoded.data(), encoded.data() + encoded.size(), *interval_ns);
      return parsed.ec == std::errc{} &&
             parsed.ptr == encoded.data() + encoded.size() &&
             *interval_ns > 0;
    }
    if (!encoded.starts_with('P')) return false;
    std::size_t position = 1;
    bool in_time = false;
    bool component_seen = false;
    int last_order = -1;
    __int128 total = 0;
    while (position < encoded.size()) {
      if (encoded[position] == 'T') {
        if (in_time) return false;
        in_time = true;
        ++position;
        if (position == encoded.size()) return false;
        continue;
      }
      const auto whole_start = position;
      while (position < encoded.size() && encoded[position] >= '0' &&
             encoded[position] <= '9') {
        ++position;
      }
      if (whole_start == position) return false;
      std::uint64_t whole = 0;
      const auto parsed = std::from_chars(
          encoded.data() + whole_start, encoded.data() + position, whole);
      if (parsed.ec != std::errc{} ||
          parsed.ptr != encoded.data() + position) {
        return false;
      }
      std::uint64_t fraction = 0;
      bool fractional = false;
      if (position < encoded.size() && encoded[position] == '.') {
        fractional = true;
        const auto fraction_start = ++position;
        while (position < encoded.size() && encoded[position] >= '0' &&
               encoded[position] <= '9') {
          ++position;
        }
        const auto digits = position - fraction_start;
        if (digits == 0 || digits > 9) return false;
        const auto fraction_parse = std::from_chars(
            encoded.data() + fraction_start, encoded.data() + position,
            fraction);
        if (fraction_parse.ec != std::errc{} ||
            fraction_parse.ptr != encoded.data() + position) {
          return false;
        }
        for (std::size_t index = digits; index < 9; ++index) fraction *= 10;
      }
      if (position == encoded.size()) return false;
      const auto designator = encoded[position++];
      int order = -1;
      std::uint64_t seconds_multiplier = 0;
      if (!in_time && designator == 'D') {
        order = 0;
        seconds_multiplier = 86'400;
      } else if (in_time && designator == 'H') {
        order = 1;
        seconds_multiplier = 3'600;
      } else if (in_time && designator == 'M') {
        order = 2;
        seconds_multiplier = 60;
      } else if (in_time && designator == 'S') {
        order = 3;
        seconds_multiplier = 1;
      } else {
        return false;
      }
      if (order <= last_order || (fractional && designator != 'S')) {
        return false;
      }
      last_order = order;
      component_seen = true;
      total += static_cast<__int128>(whole) * seconds_multiplier *
               1'000'000'000;
      if (designator == 'S') total += fraction;
      if (total > std::numeric_limits<std::int64_t>::max()) return false;
    }
    if (!component_seen || total <= 0) return false;
    *interval_ns = static_cast<std::int64_t>(total);
    return true;
  };
  if (bucket_operation) {
    const auto interval_expression =
        expression_for(bucket->child_expression_ids[0]);
    if (interval_expression == dag.expressions.end() ||
        !interval_expression->literal_or_parameter_ref.has_value() ||
        !parse_interval(*interval_expression->literal_or_parameter_ref,
                        &bucket_interval_ns)) {
      return refuse("SB_MODEL_TIME_SERIES_INTERVAL_INVALID_V1",
                    "TIME_BUCKET interval is not positive fixed nanoseconds");
    }
  }
  if (downsample_operation) {
    const auto aggregate_expression =
        expression_for(downsample->child_expression_ids[0]);
    const auto interval_expression =
        expression_for(downsample->child_expression_ids[1]);
    if (aggregate_expression == dag.expressions.end() ||
        interval_expression == dag.expressions.end() ||
        !aggregate_expression->literal_or_parameter_ref.has_value() ||
        !interval_expression->literal_or_parameter_ref.has_value()) {
      return refuse("SB_MODEL_TIME_SERIES_AGGREGATE_REFUSED_V1",
                    "time-series aggregate or interval literal is absent");
    }
    const auto aggregate_id = *aggregate_expression->literal_or_parameter_ref;
    if (aggregate_id == "COUNT") {
      aggregate = api::EngineBoundTimeSeriesAggregateV1::kCount;
    } else if (aggregate_id == "SUM") {
      aggregate = api::EngineBoundTimeSeriesAggregateV1::kSum;
    } else if (aggregate_id == "MIN") {
      aggregate = api::EngineBoundTimeSeriesAggregateV1::kMin;
    } else if (aggregate_id == "MAX") {
      aggregate = api::EngineBoundTimeSeriesAggregateV1::kMax;
    } else if (aggregate_id == "AVG") {
      aggregate = api::EngineBoundTimeSeriesAggregateV1::kAvg;
    } else {
      return refuse("SB_MODEL_TIME_SERIES_AGGREGATE_REFUSED_V1",
                    "time-series aggregate ID is outside the closed set");
    }
    std::int64_t downsample_interval_ns = 0;
    if (!parse_interval(*interval_expression->literal_or_parameter_ref,
                        &downsample_interval_ns) ||
        (bucket_operation &&
         downsample_interval_ns != bucket_interval_ns)) {
      return refuse("SB_MODEL_TIME_SERIES_INTERVAL_INVALID_V1",
                    "time-series intervals are invalid or do not match");
    }
    bucket_interval_ns = downsample_interval_ns;
  }

  std::vector<const api::RelationalOutputRecord*> outputs;
  for (const auto& output : dag.outputs) {
    if (output.relation_node_id == source->node_id) outputs.push_back(&output);
  }
  std::ranges::sort(outputs, {}, &api::RelationalOutputRecord::ordinal);
  const std::size_t expected_width =
      downsample_operation ? 7 : (bucket_operation ? 1 : 6);
  static constexpr std::array<std::string_view, 6> kRawNames{
      "row_uuid", "series_uuid", "metric_uuid", "point_timestamp", "tags",
      "value"};
  static constexpr std::array<std::string_view, 6> kRawTypes{
      "uuid", "uuid", "uuid", "timestamp_tz", "text", "real64"};
  static constexpr std::array<std::string_view, 7> kDownsampleNames{
      "series_uuid", "metric_uuid", "bucket_start", "bucket_end", "tags",
      "sample_count", "aggregate_value"};
  if (outputs.size() != expected_width ||
      source->output_descriptor_ids.size() != expected_width) {
    return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                  "time-series public output width is not exact");
  }
  std::vector<exec::ExecutorColumnDescriptor> public_columns;
  public_columns.reserve(expected_width);
  for (std::size_t ordinal = 0; ordinal < expected_width; ++ordinal) {
    const auto descriptor = std::ranges::find_if(
        dag.descriptors, [&](const auto& candidate) {
          return candidate.descriptor_id == outputs[ordinal]->descriptor_id;
        });
    const auto expected_name = downsample_operation
                                   ? kDownsampleNames[ordinal]
                                   : bucket_operation
                                         ? std::string_view("bucket_start")
                                         : kRawNames[ordinal];
    const auto expected_type =
        downsample_operation
            ? (ordinal < 2
                   ? std::string_view("uuid")
                   : ordinal < 4
                         ? std::string_view("timestamp_tz")
                         : ordinal == 4
                               ? std::string_view("text")
                               : ordinal == 5 || aggregate ==
                                                         api::EngineBoundTimeSeriesAggregateV1::kCount
                                     ? std::string_view("int64")
                                     : std::string_view("real64"))
            : bucket_operation ? std::string_view("timestamp_tz")
                               : kRawTypes[ordinal];
    const auto expected_timezone =
        expected_type == "timestamp_tz"
            ? std::optional<std::string>("UTC")
            : std::optional<std::string>{};
    if (descriptor == dag.descriptors.end() || !outputs[ordinal]->visible ||
        outputs[ordinal]->ordinal != ordinal ||
        outputs[ordinal]->output_name_utf8 != expected_name ||
        outputs[ordinal]->descriptor_id != source->output_descriptor_ids[ordinal] ||
        descriptor->nullability != api::RelationalNullability::kNonNull ||
        descriptor->collation_uuid.has_value() ||
        descriptor->timezone_profile_id != expected_timezone ||
        descriptor->width.has_value() || descriptor->precision.has_value() ||
        descriptor->scale.has_value()) {
      return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                    "time-series public output descriptor was substituted");
    }
    api::EngineDescriptor engine_descriptor;
    engine_descriptor.descriptor_uuid.canonical = descriptor->descriptor_uuid;
    engine_descriptor.descriptor_kind = "scalar";
    engine_descriptor.canonical_type_name = std::string(expected_type);
    engine_descriptor.encoded_descriptor =
        "type_uuid=" + descriptor->type_uuid + ";nullability=non_null";
    if (descriptor->timezone_profile_id.has_value()) {
      engine_descriptor.encoded_descriptor +=
          ";timezone_profile_id=" + *descriptor->timezone_profile_id;
    }
    public_columns.push_back(
        {std::string(expected_name), engine_descriptor, false,
         descriptor->descriptor_id});
  }

  api::EngineResolveStatementSnapshotRequest snapshot_request;
  snapshot_request.context = input.context;
  const auto snapshot = api::EngineResolveStatementSnapshot(snapshot_request);
  if (!snapshot.ok) {
    return refuse("SB_MODEL_MGA_CONTEXT_MISMATCH_V1",
                  "current engine MGA statement snapshot is unavailable");
  }
  auto mga = PhysicalMgaContextFromResolvedSnapshot(
      input.context, snapshot.snapshot_vector);
  mga.statement_timestamp = input.context.statement_timestamp;
  if (!exec::PhysicalMgaStatementContextValid(mga)) {
    return refuse("SB_MODEL_TIME_SERIES_TIMESTAMP_INVALID_V1",
                  "engine-issued time-series statement timestamp is invalid");
  }
  const auto object_uuid = source->required_object_uuids.front();
  const auto authorization = api::EvaluateMaterializedAuthorization(
      input.context, input.context.authorization_context, "SELECT", object_uuid);
  if (!authorization.authorized || authorization.denied ||
      authorization.policy_recheck_required ||
      !authorization.diagnostics.empty()) {
    return refuse("SB_MODEL_SECURITY_ADMISSION_REFUSED_V1",
                  "time-series SELECT authorization was refused");
  }
  const auto loaded_relation =
      api::LoadMgaRelationStorageDescriptor(input.context, object_uuid);
  if (!loaded_relation.ok) {
    return refuse("SB_MODEL_TIME_SERIES_EXACT_FALLBACK_UNAVAILABLE_V1",
                  loaded_relation.diagnostic.detail.empty()
                      ? "persistent time-series relation is unavailable"
                      : loaded_relation.diagnostic.detail);
  }
  const auto& persisted = loaded_relation.descriptor;
  if (persisted.relation_uuid.canonical != object_uuid ||
      persisted.database_uuid.canonical != input.context.database_uuid.canonical ||
      persisted.schema_uuid.canonical.empty() ||
      persisted.relation_kind != "table" ||
      persisted.storage_profile != "local_mga_rowstore_v1" ||
      persisted.descriptor_uuid.canonical.empty() ||
      persisted.descriptor_generation == 0) {
    return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                  "persistent time-series descriptor is invalid");
  }
  if (!api::ExactTimeSeriesStorageDescriptorV1(persisted)) {
    return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                  "persistent time-series descriptor type binding is not exact");
  }
  const auto core_type_uuid = [&](const std::string_view stable_name) {
    return ExactCanonicalCoreDatatypeTypeUuidV1(stable_name);
  };
  const auto alias_expression = expression_for(range->child_expression_ids[0]);
  const auto exact_range_descriptor = [&](const auto descriptor) {
    return descriptor != dag.descriptors.end() &&
           descriptor->descriptor_uuid ==
               persisted.columns[1].value_descriptor.descriptor_uuid.canonical &&
           descriptor->type_uuid == core_type_uuid("timestamp") &&
           descriptor->nullability == api::RelationalNullability::kNonNull &&
           descriptor->timezone_profile_id == std::optional<std::string>("UTC");
  };
  if (alias_expression == dag.expressions.end() ||
      alias_expression->expression_kind !=
          api::RelationalExpressionKind::kIdentifier ||
      alias_expression->bound_name_uuid != object_uuid ||
      !exact_range_descriptor(start_descriptor) ||
      !exact_range_descriptor(end_descriptor)) {
    return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                  "TIME_RANGE binding or endpoint descriptor was substituted");
  }
  const auto interval_expression =
      bucket_operation
          ? expression_for(bucket->child_expression_ids[0])
          : downsample_operation
                ? expression_for(downsample->child_expression_ids[1])
                : dag.expressions.end();
  const auto interval_descriptor =
      interval_expression == dag.expressions.end()
          ? dag.descriptors.end()
          : std::ranges::find_if(
                dag.descriptors, [&](const auto& candidate_descriptor) {
                  return candidate_descriptor.descriptor_id ==
                         interval_expression->result_descriptor_id;
                });
  if ((bucket_operation || downsample_operation) &&
      (interval_descriptor == dag.descriptors.end() ||
       !CanonicalUuidText(interval_descriptor->descriptor_uuid) ||
       interval_descriptor->descriptor_uuid ==
           interval_descriptor->type_uuid ||
       interval_descriptor->type_uuid != core_type_uuid("interval") ||
       interval_descriptor->nullability !=
           api::RelationalNullability::kNonNull)) {
    return refuse("SB_MODEL_TIME_SERIES_INTERVAL_INVALID_V1",
                  "time-series interval descriptor was substituted");
  }
  const auto sample_count_descriptor =
      downsample_operation && outputs.size() > 5
          ? std::ranges::find_if(
                dag.descriptors, [&](const auto& candidate_descriptor) {
                  return candidate_descriptor.descriptor_id ==
                         outputs[5]->descriptor_id;
                })
          : dag.descriptors.end();
  if (downsample_operation &&
      (sample_count_descriptor == dag.descriptors.end() ||
       !CanonicalUuidText(sample_count_descriptor->descriptor_uuid) ||
       sample_count_descriptor->descriptor_uuid ==
           sample_count_descriptor->type_uuid ||
       sample_count_descriptor->type_uuid != core_type_uuid("int64") ||
       sample_count_descriptor->nullability !=
           api::RelationalNullability::kNonNull)) {
    return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                  "time-series sample-count descriptor was substituted");
  }
  if (downsample_operation) {
    const auto aggregate_expression =
        expression_for(downsample->child_expression_ids[0]);
    const auto aggregate_descriptor =
        aggregate_expression == dag.expressions.end()
            ? dag.descriptors.end()
            : std::ranges::find_if(
                  dag.descriptors, [&](const auto& candidate_descriptor) {
                    return candidate_descriptor.descriptor_id ==
                           aggregate_expression->result_descriptor_id;
                  });
    if (aggregate_expression == dag.expressions.end() ||
        aggregate_descriptor == dag.descriptors.end() ||
        aggregate_descriptor->descriptor_uuid !=
            persisted.columns[2].value_descriptor.descriptor_uuid.canonical ||
        aggregate_descriptor->type_uuid != core_type_uuid("character")) {
      return refuse("SB_MODEL_TIME_SERIES_AGGREGATE_REFUSED_V1",
                    "time-series aggregate ID descriptor was substituted");
    }
  }
  using TimeSeriesDescriptorIdentity = std::pair<std::string, std::string>;
  std::vector<TimeSeriesDescriptorIdentity> expected_source_descriptors;
  const auto append_source_descriptor =
      [&](const std::string& descriptor_uuid,
          const std::string_view stable_type_name) {
        expected_source_descriptors.emplace_back(
            descriptor_uuid, core_type_uuid(stable_type_name));
      };
  const auto range_result_descriptor = std::ranges::find_if(
      dag.descriptors, [&](const auto& descriptor) {
        return descriptor.descriptor_id == range->result_descriptor_id;
      });
  if (range_result_descriptor == dag.descriptors.end() ||
      !CanonicalUuidText(range_result_descriptor->descriptor_uuid) ||
      range_result_descriptor->type_uuid != core_type_uuid("boolean")) {
    return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                  "time-series range truth descriptor is not exact");
  }
  append_source_descriptor(persisted.descriptor_uuid.canonical, "uuid");
  append_source_descriptor(persisted.schema_uuid.canonical, "uuid");
  append_source_descriptor(
      persisted.columns[0].value_descriptor.descriptor_uuid.canonical,
      "uuid");
  append_source_descriptor(
      persisted.columns[1].value_descriptor.descriptor_uuid.canonical,
      "timestamp");
  if (downsample_operation) {
    append_source_descriptor(persisted.columns[1].column_uuid.canonical,
                             "timestamp");
  }
  append_source_descriptor(
      persisted.columns[2].value_descriptor.descriptor_uuid.canonical,
      "character");
  if (downsample_operation) {
    append_source_descriptor(sample_count_descriptor->descriptor_uuid,
                             "int64");
  }
  if (bucket_operation || downsample_operation) {
    append_source_descriptor(interval_descriptor->descriptor_uuid,
                             "interval");
  }
  append_source_descriptor(range_result_descriptor->descriptor_uuid,
                           "boolean");
  append_source_descriptor(
      persisted.columns[3].value_descriptor.descriptor_uuid.canonical,
      "real64");
  if (downsample_operation &&
      aggregate == api::EngineBoundTimeSeriesAggregateV1::kCount) {
    append_source_descriptor(persisted.columns[3].column_uuid.canonical,
                             "int64");
  }
  std::unordered_set<std::uint32_t> source_expression_closure;
  std::vector<std::uint32_t> source_expression_worklist =
      source->bound_expression_ids;
  while (!source_expression_worklist.empty()) {
    const auto expression_id = source_expression_worklist.back();
    source_expression_worklist.pop_back();
    if (!source_expression_closure.insert(expression_id).second) continue;
    const auto expression = expression_for(expression_id);
    if (expression == dag.expressions.end()) {
      return refuse("SBLR.PLAN_TREE.INVALID_HANDLE",
                    "time-series source expression closure is incomplete");
    }
    source_expression_worklist.insert(
        source_expression_worklist.end(),
        expression->child_expression_ids.begin(),
        expression->child_expression_ids.end());
  }
  std::unordered_set<std::uint32_t> source_descriptor_ids(
      source->output_descriptor_ids.begin(),
      source->output_descriptor_ids.end());
  for (const auto expression_id : source_expression_closure) {
    source_descriptor_ids.insert(
        expression_for(expression_id)->result_descriptor_id);
  }
  const auto hidden_descriptor_count =
      (bucket_operation || downsample_operation) ? 1u : 0u;
  const auto expected_identity = [&](const auto& descriptor) {
    return std::ranges::any_of(
        expected_source_descriptors, [&](const auto& expected) {
          return descriptor.descriptor_uuid == expected.first &&
                 descriptor.type_uuid == expected.second;
        });
  };
  bool exact_source_descriptor_cohort =
      source_descriptor_ids.size() + hidden_descriptor_count ==
      expected_source_descriptors.size();
  for (const auto descriptor_id : source_descriptor_ids) {
    const auto descriptor = std::ranges::find_if(
        dag.descriptors, [&](const auto& candidate_descriptor) {
          return candidate_descriptor.descriptor_id == descriptor_id;
        });
    exact_source_descriptor_cohort =
        exact_source_descriptor_cohort && descriptor != dag.descriptors.end() &&
        expected_identity(*descriptor);
  }
  for (const auto& expected : expected_source_descriptors) {
    const auto descriptor = std::ranges::find_if(
        dag.descriptors, [&](const auto& candidate_descriptor) {
          return candidate_descriptor.descriptor_uuid == expected.first &&
                 candidate_descriptor.type_uuid == expected.second;
        });
    const bool hidden_row_identity =
        hidden_descriptor_count == 1 &&
        expected.first == persisted.descriptor_uuid.canonical;
    exact_source_descriptor_cohort =
        exact_source_descriptor_cohort && descriptor != dag.descriptors.end() &&
        (source_descriptor_ids.contains(descriptor->descriptor_id) !=
         hidden_row_identity);
  }
  if (!exact_source_descriptor_cohort) {
    return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                  "time-series exact source descriptor cohort differs from storage");
  }
  std::vector<std::string> expected_public_descriptor_uuids;
  std::vector<std::string_view> expected_public_core_types;
  if (downsample_operation) {
    expected_public_descriptor_uuids = {
        persisted.schema_uuid.canonical,
        persisted.columns[0].value_descriptor.descriptor_uuid.canonical,
        persisted.columns[1].value_descriptor.descriptor_uuid.canonical,
        persisted.columns[1].column_uuid.canonical,
        persisted.columns[2].value_descriptor.descriptor_uuid.canonical,
        sample_count_descriptor->descriptor_uuid,
        aggregate == api::EngineBoundTimeSeriesAggregateV1::kCount
            ? persisted.columns[3].column_uuid.canonical
            : persisted.columns[3].value_descriptor.descriptor_uuid.canonical};
    expected_public_core_types = {
        "uuid", "uuid", "timestamp", "timestamp", "character", "int64",
        aggregate == api::EngineBoundTimeSeriesAggregateV1::kCount
            ? std::string_view("int64")
            : std::string_view("real64")};
  } else if (bucket_operation) {
    expected_public_descriptor_uuids = {
        persisted.columns[1].value_descriptor.descriptor_uuid.canonical};
    expected_public_core_types = {"timestamp"};
  } else {
    expected_public_descriptor_uuids = {
        persisted.descriptor_uuid.canonical,
        persisted.schema_uuid.canonical,
        persisted.columns[0].value_descriptor.descriptor_uuid.canonical,
        persisted.columns[1].value_descriptor.descriptor_uuid.canonical,
        persisted.columns[2].value_descriptor.descriptor_uuid.canonical,
        persisted.columns[3].value_descriptor.descriptor_uuid.canonical};
    expected_public_core_types = {
        "uuid", "uuid", "uuid", "timestamp", "character", "real64"};
  }
  bool exact_public_cohort =
      outputs.size() == expected_public_descriptor_uuids.size();
  for (std::size_t ordinal = 0;
       exact_public_cohort && ordinal < outputs.size(); ++ordinal) {
    const auto descriptor = std::ranges::find_if(
        dag.descriptors, [&](const auto& candidate_descriptor) {
          return candidate_descriptor.descriptor_id ==
                 outputs[ordinal]->descriptor_id;
        });
    exact_public_cohort =
        descriptor != dag.descriptors.end() &&
        descriptor->descriptor_uuid == expected_public_descriptor_uuids[ordinal] &&
        descriptor->type_uuid ==
            core_type_uuid(expected_public_core_types[ordinal]) &&
        !descriptor->collation_uuid.has_value() &&
        descriptor->timezone_profile_id ==
            (expected_public_core_types[ordinal] == "timestamp"
                 ? std::optional<std::string>("UTC")
                 : std::optional<std::string>{}) &&
        !descriptor->width.has_value() && !descriptor->precision.has_value() &&
        !descriptor->scale.has_value();
  }
  if (!exact_public_cohort) {
    return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                  "time-series public descriptor cohort differs from storage");
  }
  if (!downsample_operation && !bucket_operation) {
    const std::array<std::string, 6> expected_bound_names{
        persisted.descriptor_uuid.canonical,
        persisted.schema_uuid.canonical,
        persisted.columns[0].column_uuid.canonical,
        persisted.columns[1].column_uuid.canonical,
        persisted.columns[2].column_uuid.canonical,
        persisted.columns[3].column_uuid.canonical};
    for (std::size_t ordinal = 0; ordinal < outputs.size(); ++ordinal) {
      const auto expression = expression_for(outputs[ordinal]->expression_id);
      if (expression == dag.expressions.end() ||
          expression->expression_kind !=
              api::RelationalExpressionKind::kIdentifier ||
          expression->bound_name_uuid != expected_bound_names[ordinal]) {
        return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                      "time-series raw bound column identity was substituted");
      }
    }
  }
  if (bucket_operation) {
    const auto timestamp_expression =
        expression_for(bucket->child_expression_ids[1]);
    const auto timestamp_descriptor =
        timestamp_expression == dag.expressions.end()
            ? dag.descriptors.end()
            : std::ranges::find_if(
                  dag.descriptors, [&](const auto& candidate_descriptor) {
                    return candidate_descriptor.descriptor_id ==
                           timestamp_expression->result_descriptor_id;
                  });
    if (timestamp_expression == dag.expressions.end() ||
        timestamp_descriptor == dag.descriptors.end() ||
        timestamp_expression->expression_kind !=
            api::RelationalExpressionKind::kIdentifier ||
        timestamp_expression->bound_name_uuid !=
            persisted.columns[1].column_uuid.canonical ||
        timestamp_descriptor->descriptor_uuid !=
            persisted.columns[1].value_descriptor.descriptor_uuid.canonical ||
        timestamp_descriptor->type_uuid != core_type_uuid("timestamp")) {
      return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                    "TIME_BUCKET point timestamp binding was substituted");
    }
  }
  if (downsample_operation) {
    const std::array<std::string, 7> expected_bound_names{
        persisted.schema_uuid.canonical,
        persisted.columns[0].column_uuid.canonical,
        persisted.columns[1].column_uuid.canonical,
        persisted.columns[1].column_uuid.canonical,
        persisted.columns[2].column_uuid.canonical,
        persisted.relation_uuid.canonical,
        aggregate == api::EngineBoundTimeSeriesAggregateV1::kCount
            ? persisted.relation_uuid.canonical
            : persisted.columns[3].column_uuid.canonical};
    for (std::size_t ordinal = 0; ordinal < outputs.size(); ++ordinal) {
      const auto expression = expression_for(outputs[ordinal]->expression_id);
      if (expression == dag.expressions.end() ||
          expression->expression_kind !=
              api::RelationalExpressionKind::kIdentifier ||
          expression->bound_name_uuid != expected_bound_names[ordinal]) {
        return refuse(
            "SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
            "time-series downsample output binding was substituted");
      }
    }
    const auto value_expression =
        expression_for(downsample->child_expression_ids[2]);
    const auto value_descriptor =
        value_expression == dag.expressions.end()
            ? dag.descriptors.end()
            : std::ranges::find_if(
                  dag.descriptors, [&](const auto& candidate_descriptor) {
                    return candidate_descriptor.descriptor_id ==
                           value_expression->result_descriptor_id;
                  });
    if (value_expression == dag.expressions.end() ||
        value_descriptor == dag.descriptors.end() ||
        value_expression->expression_kind !=
            api::RelationalExpressionKind::kIdentifier ||
        value_expression->bound_name_uuid !=
            persisted.columns[3].column_uuid.canonical ||
        value_descriptor->descriptor_uuid !=
            persisted.columns[3].value_descriptor.descriptor_uuid.canonical ||
        value_descriptor->type_uuid != core_type_uuid("real64")) {
      return refuse("SB_MODEL_TIME_SERIES_AGGREGATE_REFUSED_V1",
                    "TIME_DOWNSAMPLE value binding was substituted");
    }
  }
  std::vector<std::string> admitted_object_uuids{object_uuid};
  if (mixed_join != nullptr || asof_join != nullptr) {
    const auto relational_object_uuid =
        relational_scan->required_object_uuids.front();
    const auto relational_authorization =
        api::EvaluateMaterializedAuthorization(
            input.context, input.context.authorization_context, "SELECT",
            relational_object_uuid);
    const auto relational_relation = api::LoadMgaRelationStorageDescriptor(
        input.context, relational_object_uuid);
    if (!relational_authorization.authorized ||
        relational_authorization.denied ||
        relational_authorization.policy_recheck_required ||
        !relational_authorization.diagnostics.empty() ||
        !relational_relation.ok ||
        relational_relation.descriptor.relation_uuid.canonical !=
            relational_object_uuid ||
        relational_relation.descriptor.database_uuid.canonical !=
            input.context.database_uuid.canonical ||
        relational_relation.descriptor.descriptor_uuid.canonical.empty() ||
        relational_relation.descriptor.descriptor_generation == 0) {
      return refuse(
          "SB_MODEL_SECURITY_ADMISSION_REFUSED_V1",
          "time-series mixed JOIN relational object was not admitted");
    }
    admitted_object_uuids.push_back(relational_object_uuid);
  }

  api::CanonicalRelationalPlanningScope planning_scope;
  planning_scope.catalog_epoch_uuid = input.context.catalog_epoch_uuid.canonical;
  planning_scope.security_context_uuid =
      input.context.authorization_context.authority_uuid.canonical;
  planning_scope.statement_uuid = input.context.statement_uuid.canonical;
  planning_scope.statement_timestamp = input.context.statement_timestamp;
  planning_scope.owning_transaction_uuid =
      input.context.transaction_uuid.canonical;
  planning_scope.statement_snapshot_uuid =
      input.context.statement_snapshot_uuid.canonical;
  planning_scope.statement_metadata_snapshot_uuid =
      input.context.statement_metadata_snapshot_uuid.canonical;
  planning_scope.local_transaction_id = input.context.local_transaction_id;
  planning_scope.snapshot_visible_through_local_transaction_id =
      input.context.snapshot_visible_through_local_transaction_id;
  planning_scope.metadata_snapshot_engine_owned =
      input.context.statement_metadata_snapshot_engine_owned;
  planning_scope.authorization_context_engine_owned =
      input.context.authorization_context.present;
  auto logical =
      api::PopulateCanonicalLogicalGraphFromAdmittedTypedRelationalDag(
          dag, planning_scope);
  const auto logical_source = std::ranges::find_if(
      logical.logical_graph.nodes, [&](const auto& node) {
        return node.logical_node_id == source->node_id;
      });
  if (!logical.accepted || logical_source == logical.logical_graph.nodes.end() ||
      logical_source->model_family_identity !=
          plan::CanonicalLogicalModelFamilyIdentity::kTimeSeries) {
    return refuse(logical.issues.empty()
                      ? "QOW-DIAG-OPTIMIZER-ADMISSION-BOUND-REQUEST-V1"
                      : logical.issues.front().diagnostic_id,
                  logical.issues.empty() ? "time-series logical bridge refused"
                                         : logical.issues.front().field_id);
  }

  const auto identity_scope =
      dag.bound_sblr_tree_uuid + ":" + input.context.statement_uuid.canonical;
  const auto provider_generations =
      api::ListNoSqlProviderGenerations(input.context);
  std::vector<const api::EngineNoSqlProviderGenerationMetadata*>
      current_preferred_generations;
  for (const auto& metadata : provider_generations) {
    if (!metadata.time_series_rollup_candidate_present ||
        api::ValidateTimeSeriesRollupCapabilityBindingV1(metadata)) {
      continue;
    }
    const bool same_time_series_collection =
        metadata.family == api::EngineNoSqlProviderFamily::kTimeSeries &&
        metadata.collection_uuid == object_uuid &&
        metadata.database_identity ==
            api::EngineNoSqlProviderDatabaseIdentity(input.context) &&
        metadata.database_uuid == input.context.database_uuid.canonical;
    if (!same_time_series_collection) {
      return refuse("SB_MODEL_PROVIDER_GENERATION_STALE_V1",
                    "time-series persisted rollup binding was substituted");
    }
  }
  for (const auto& metadata : provider_generations) {
    const bool same_time_series_collection =
        metadata.family == api::EngineNoSqlProviderFamily::kTimeSeries &&
        metadata.collection_uuid == object_uuid &&
        metadata.database_identity ==
            api::EngineNoSqlProviderDatabaseIdentity(input.context) &&
        metadata.database_uuid == input.context.database_uuid.canonical;
    if (!same_time_series_collection) continue;

    const bool default_rollup_carrier =
        !metadata.time_series_rollup_candidate_present &&
        metadata.time_series_rollup_capability_uuid.empty() &&
        metadata.time_series_rollup_generation == 0 &&
        metadata.time_series_visible_late_arrival_generation == 0 &&
        metadata.time_series_rollup_interval_ns == 0 &&
        metadata.time_series_rollup_exactness_attestation_state.empty() &&
        metadata.time_series_rollup_statement_snapshot_uuid.empty() &&
        metadata.time_series_rollup_statement_metadata_snapshot_uuid.empty() &&
        metadata.time_series_rollup_owning_transaction_uuid.empty() &&
        metadata.time_series_rollup_local_transaction_id == 0 &&
        metadata
                .time_series_rollup_snapshot_visible_through_local_transaction_id ==
            0 &&
        metadata.time_series_rollup_security_context_uuid.empty() &&
        metadata.time_series_rollup_catalog_epoch_uuid.empty() &&
        !metadata.time_series_rollup_exact_residual_recheck_required &&
        !metadata.time_series_rollup_base_row_mga_recheck_required &&
        !metadata.time_series_rollup_security_recheck_required;
    if (!metadata.time_series_rollup_candidate_present &&
        !default_rollup_carrier) {
      return refuse("SB_MODEL_PROVIDER_GENERATION_STALE_V1",
                    "time-series rollup carrier was partially activated");
    }
    if (metadata.time_series_rollup_candidate_present) {
      if (!CanonicalUuidText(metadata.time_series_rollup_capability_uuid) ||
          metadata.time_series_rollup_capability_uuid ==
              metadata.generation_uuid ||
          metadata.time_series_rollup_generation == 0 ||
          metadata.time_series_visible_late_arrival_generation == 0 ||
          metadata.time_series_rollup_generation >
              metadata.time_series_visible_late_arrival_generation ||
          metadata.time_series_rollup_interval_ns <= 0 ||
          metadata.time_series_rollup_exactness_attestation_state !=
              "TIME_SERIES_ROLLUP_SECTION_8_EXACT_V1" ||
          !metadata.time_series_rollup_exact_residual_recheck_required ||
          !metadata.time_series_rollup_base_row_mga_recheck_required) {
        return refuse("SB_MODEL_PROVIDER_GENERATION_STALE_V1",
                      "time-series persisted rollup capability is invalid");
      }
      if (metadata.time_series_rollup_interval_ns != bucket_interval_ns) {
        return refuse("SB_MODEL_PROVIDER_GENERATION_STALE_V1",
                      "time-series persisted rollup interval was substituted");
      }
      if (metadata.time_series_rollup_statement_snapshot_uuid !=
              input.context.statement_snapshot_uuid.canonical ||
          metadata.time_series_rollup_statement_metadata_snapshot_uuid !=
              input.context.statement_metadata_snapshot_uuid.canonical ||
          metadata.time_series_rollup_owning_transaction_uuid !=
              input.context.transaction_uuid.canonical ||
          metadata.time_series_rollup_local_transaction_id !=
              input.context.local_transaction_id ||
          metadata
                  .time_series_rollup_snapshot_visible_through_local_transaction_id !=
              input.context.snapshot_visible_through_local_transaction_id) {
        return refuse("SB_MODEL_MGA_CONTEXT_MISMATCH_V1",
                      "time-series persisted rollup MGA cohort changed");
      }
      if (!metadata.time_series_rollup_security_recheck_required ||
          metadata.time_series_rollup_security_context_uuid !=
              input.context.authorization_context.authority_uuid.canonical ||
          metadata.security_epoch !=
              std::max<std::uint64_t>(1, input.context.security_epoch) ||
          metadata.redaction_epoch !=
              std::max<std::uint64_t>(1, input.context.security_epoch)) {
        return refuse("SB_MODEL_SECURITY_ADMISSION_REFUSED_V1",
                      "time-series persisted rollup security cohort changed");
      }
      if (metadata.time_series_rollup_catalog_epoch_uuid !=
              input.context.catalog_epoch_uuid.canonical ||
          metadata.catalog_epoch !=
              std::max<std::uint64_t>(
                  1, input.context.catalog_generation_id) ||
          metadata.descriptor_epoch !=
              std::max<std::uint64_t>(1, input.context.resource_epoch)) {
        return refuse("SB_MODEL_CATALOG_GENERATION_STALE_V1",
                      "time-series persisted rollup catalog cohort changed");
      }
      if (!api::ValidateTimeSeriesRollupCapabilityBindingV1(metadata)) {
        return refuse("SB_MODEL_PROVIDER_GENERATION_STALE_V1",
                      "time-series persisted rollup binding was substituted");
      }
    }
    if (CanonicalUuidText(metadata.provider_id) &&
        CanonicalUuidText(metadata.generation_uuid) &&
        metadata.generation_id != 0 &&
        metadata.descriptor_epoch ==
            std::max<std::uint64_t>(1, input.context.resource_epoch) &&
        metadata.security_epoch ==
            std::max<std::uint64_t>(1, input.context.security_epoch) &&
        metadata.redaction_epoch ==
            std::max<std::uint64_t>(1, input.context.security_epoch) &&
        metadata.catalog_epoch ==
            std::max<std::uint64_t>(1, input.context.catalog_generation_id) &&
        metadata.publish_state == "published" &&
        metadata.validation_state == "validated" &&
        !metadata.provider_claims_transaction_finality_authority &&
        !metadata.provider_claims_visibility_authority) {
      current_preferred_generations.push_back(&metadata);
    }
  }
  const bool preferred_generation_current =
      current_preferred_generations.size() == 1;
  const auto* rollup_candidate =
      preferred_generation_current && downsample_operation &&
              current_preferred_generations.front()
                  ->time_series_rollup_candidate_present
          ? current_preferred_generations.front()
          : nullptr;
  const bool rollup_candidate_stale =
      rollup_candidate != nullptr &&
      rollup_candidate->time_series_rollup_generation <
          rollup_candidate->time_series_visible_late_arrival_generation;
  const auto provider_uuid =
      preferred_generation_current
          ? current_preferred_generations.front()->provider_id
          : DerivedCanonicalUuid(identity_scope,
                                 "time-series.unavailable-provider");
  const auto capability_uuid =
      preferred_generation_current
          ? current_preferred_generations.front()->generation_uuid
          : DerivedCanonicalUuid(identity_scope,
                                 "time-series.unavailable-capability");
  const auto fallback_provider_uuid =
      DerivedCanonicalUuid(identity_scope, "time-series.fallback-provider");
  const auto fallback_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "time-series.fallback-capability");
  const auto generation =
      std::max<std::uint64_t>(1, input.context.catalog_generation_id);
  opt::ModelFamilyCoordinatorRequestV1 planning;
  planning.family_id = "time_series";
  planning.operation_id = downsample_operation ? "TIME_SERIES_DOWNSAMPLE"
                                                : "TIME_SERIES_RANGE_READ";
  planning.logical_operator_id = "LOGICAL_TIME_SERIES_SOURCE_V1";
  planning.logical_node_id = source->node_id;
  planning.object_uuid = object_uuid;
  planning.output_descriptor_ids = source->output_descriptor_ids;
  planning.mga_statement_context = mga;
  planning.bound_sblr_tree_uuid = dag.bound_sblr_tree_uuid;
  planning.catalog_epoch_uuid = input.context.catalog_epoch_uuid.canonical;
  planning.security_context_uuid =
      input.context.authorization_context.authority_uuid.canonical;
  planning.capability_snapshot_uuid =
      input.context.optimizer_capability_snapshot_uuid.canonical;
  planning.resource_snapshot_uuid =
      input.context.optimizer_resource_snapshot_uuid.canonical;
  planning.statistics_snapshot_uuid =
      DerivedCanonicalUuid(identity_scope, "time-series.statistics-snapshot");
  planning.route_snapshot_uuid =
      input.context.optimizer_route_snapshot_uuid.canonical;
  planning.catalog_generation = generation;
  planning.current_catalog_generation = generation;
  planning.security_epoch = std::max<std::uint64_t>(1, input.context.security_epoch);
  planning.policy_epoch = std::max<std::uint64_t>(
      1, input.context.authorization_context.policy_epoch);
  planning.resource_epoch = std::max<std::uint64_t>(1, input.context.resource_epoch);
  planning.statistics_generation = generation;
  planning.route_epoch = input.context.optimizer_route_epoch;
  planning.route_generation = input.context.optimizer_route_generation;
  planning.memory_budget_bytes = input.context.optimizer_memory_budget_bytes;
  planning.security_admitted = input.context.security_context_present &&
                               input.context.authorization_context.present;
  const auto preferred_provider_generation =
      preferred_generation_current
          ? current_preferred_generations.front()->generation_id
          : std::max<std::uint64_t>(1, planning.route_generation);
  // A current generation is necessary but not sufficient to execute a
  // rollup.  RCP-076 authorizes persisted candidate admission and exact raw
  // reconstruction, not rollup materialization.  Until a distinct execution
  // proof exists, every admitted rollup candidate coordinates to the exact
  // engine-owned fallback instead of masquerading as the preferred raw path.
  const auto preferred_available =
      preferred_generation_current && rollup_candidate == nullptr;
  // The exact engine-owned bucket-store fallback has its own frozen route
  // generation.  It is deliberately independent from both the storage
  // descriptor generation and any externally published provider generation.
  const auto fallback_provider_generation =
      std::max<std::uint64_t>(1, planning.route_generation);
  std::vector<opt::ModelFamilyCapabilitySnapshotV1> alternatives;
  alternatives.push_back(MakeModelFamilyCapabilitySnapshotForCompositionV1(
      planning, identity_scope + ".time-series.native",
      opt::ModelFamilyAlternativeRouteClassV1::kNative, provider_uuid,
      capability_uuid, preferred_provider_generation, preferred_available, 1,
      1, std::max<std::uint64_t>(1, planning.memory_budget_bytes / 2)));
  alternatives.push_back(MakeModelFamilyCapabilitySnapshotForCompositionV1(
      planning, identity_scope + ".time-series.fallback",
      opt::ModelFamilyAlternativeRouteClassV1::kExactCollectionFallback,
      fallback_provider_uuid, fallback_capability_uuid,
      fallback_provider_generation, true, 2, 2,
      std::max<std::uint64_t>(1, planning.memory_budget_bytes / 2)));
  const auto planned = PlanCanonicalModelFamilySourceForCompositionV1(
      planning, identity_scope + ".time-series.inventory",
      std::move(alternatives));
  if (!planned.accepted || !planned.selected || !planned.data_access_allowed ||
      !planned.optimizer_owned_enumeration ||
      planned.exact_fallback_selected == preferred_available ||
      planned.selected_candidate.provider_uuid !=
          (preferred_available ? provider_uuid : fallback_provider_uuid) ||
      planned.selected_candidate.capability_uuid !=
          (preferred_available ? capability_uuid : fallback_capability_uuid) ||
      planned.selected_candidate.provider_generation !=
          (preferred_available ? preferred_provider_generation
                               : fallback_provider_generation) ||
      planned.physical_dag.nodes.size() != 1 ||
      planned.physical_dag.nodes.front().implementation_id !=
          "physical_time_series_range_scan_v1") {
    return refuse(planned.diagnostic_id.empty()
                      ? "SB_MODEL_TIME_SERIES_EXACT_FALLBACK_UNAVAILABLE_V1"
                      : planned.diagnostic_id,
                  planned.detail.empty()
                      ? "time-series exact candidate was not selected"
                      : planned.detail);
  }
  result.optimizer_selected = true;
  result.physical_dag_published = true;
  result.physical_node_count = planned.physical_dag.nodes.size();
  result.selected_plan_uuid = planned.physical_dag.selected_plan_uuid;

  api::EngineBoundTimeSeriesReadRequestV1 provider_request;
  provider_request.context = input.context;
  provider_request.operation =
      downsample_operation
          ? api::EngineBoundTimeSeriesReadOperationV1::kBucketDownsample
          : api::EngineBoundTimeSeriesReadOperationV1::kRangeRead;
  provider_request.aggregate = aggregate;
  provider_request.object_uuid = object_uuid;
  provider_request.range_start = range_start;
  provider_request.range_end = range_end;
  // TIME_BUCKET is a scalar projection over raw range rows.  Its interval is
  // retained by the post-read projection and must not turn the provider
  // request into a downsample-shaped access request.
  provider_request.bucket_interval_ns =
      downsample_operation ? bucket_interval_ns : 0;
  provider_request.expected_descriptor_uuid =
      persisted.descriptor_uuid.canonical;
  provider_request.expected_descriptor_generation =
      persisted.descriptor_generation;
  provider_request.selected_alternative_uuid =
      planned.selected_candidate.alternative_uuid;
  provider_request.capability_uuid =
      planned.selected_candidate.capability_uuid;
  provider_request.provider_uuid = planned.selected_candidate.provider_uuid;
  provider_request.provider_generation =
      planned.selected_candidate.provider_generation;
  provider_request.exact_fallback_selected =
      planned.exact_fallback_selected;
  provider_request.rollup_candidate_selected = rollup_candidate != nullptr;
  provider_request.rollup_generation =
      rollup_candidate == nullptr
          ? 0
          : rollup_candidate->time_series_rollup_generation;
  provider_request.visible_late_arrival_generation =
      rollup_candidate == nullptr
          ? 0
          : rollup_candidate->time_series_visible_late_arrival_generation;
  const auto scanned_row_bound =
      input.context.optimizer_maximum_search_steps;
  const auto output_row_bound =
      input.context.optimizer_maximum_candidate_count;
  const auto group_bound = input.context.optimizer_maximum_memo_groups;
  if (scanned_row_bound == 0 || output_row_bound == 0 || group_bound == 0 ||
      planning.memory_budget_bytes == 0 ||
      scanned_row_bound > std::numeric_limits<std::size_t>::max() ||
      output_row_bound > std::numeric_limits<std::size_t>::max() ||
      group_bound > std::numeric_limits<std::size_t>::max()) {
    return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                  "time-series optimizer resource cohort is absent");
  }
  provider_request.maximum_scanned_row_versions =
      static_cast<std::size_t>(scanned_row_bound);
  const auto selected_source_memory_grant =
      planned.selected_candidate.cost.memory_bytes_required;
  if (selected_source_memory_grant == 0 ||
      selected_source_memory_grant > planning.memory_budget_bytes) {
    return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "time-series selected source memory grant is invalid");
  }
  provider_request.maximum_decoded_bytes =
      selected_source_memory_grant;
  provider_request.maximum_output_rows =
      static_cast<std::size_t>(output_row_bound);
  provider_request.maximum_groups =
      static_cast<std::size_t>(group_bound);
  provider_request.maximum_tag_bytes =
      std::max<std::uint64_t>(1, planning.memory_budget_bytes / 4);
  provider_request.maximum_result_bytes =
      selected_source_memory_grant;
  provider_request.maximum_memory_bytes = selected_source_memory_grant;
  const auto time_series_cancellation_probe_failed =
      std::make_shared<std::atomic_bool>(false);
  const auto raw_time_series_cancellation =
      input.context.query_cancellation_requested;
  const std::function<bool()> time_series_cancellation_requested =
      [raw_time_series_cancellation,
       time_series_cancellation_probe_failed]() noexcept {
        if (!raw_time_series_cancellation) return false;
        try {
          return raw_time_series_cancellation();
        } catch (...) {
          time_series_cancellation_probe_failed->store(
              true, std::memory_order_relaxed);
          return true;
        }
      };
  provider_request.cancellation_requested =
      time_series_cancellation_requested;

  exec::ModelSourceInputDescriptorV1 source_input;
  source_input.family_id = "time_series";
  source_input.operation_id =
      bucket_operation && !downsample_operation
          ? "TIME_SERIES_BUCKET"
          : planning.operation_id;
  source_input.object_uuid = object_uuid;
  source_input.physical_node_id =
      planned.physical_dag.nodes.front().physical_node_id;
  source_input.selected_alternative_uuid =
      planned.selected_candidate.alternative_uuid;
  source_input.capability_uuid = planned.selected_candidate.capability_uuid;
  source_input.provider_uuid = planned.selected_candidate.provider_uuid;
  source_input.provider_generation =
      planned.selected_candidate.provider_generation;
  source_input.exact_fallback_selected = planned.exact_fallback_selected;
  source_input.result_handle_uuid =
      DerivedCanonicalUuid(identity_scope, "time-series.result-handle");
  source_input.causal_counter_id =
      planned.physical_dag.nodes.front().causal_counter_id;
  source_input.output_descriptor_ids = source->output_descriptor_ids;
  source_input.mga_statement_context = mga;
  source_input.catalog_epoch_uuid = input.context.catalog_epoch_uuid.canonical;
  source_input.security_context_uuid =
      input.context.authorization_context.authority_uuid.canonical;
  source_input.policy_snapshot_uuid =
      DerivedCanonicalUuid(identity_scope, "time-series.policy-snapshot");
  source_input.resource_contract_uuid =
      DerivedCanonicalUuid(identity_scope, "time-series.resource-contract");
  source_input.catalog_generation = generation;
  source_input.descriptor_generation = persisted.descriptor_generation;
  source_input.security_generation = planning.security_epoch;
  source_input.policy_generation = planning.policy_epoch;
  source_input.resource_generation = planning.resource_epoch;
  source_input.maximum_rows = provider_request.maximum_output_rows;
  std::uint64_t source_maximum_cells = 0;
  if (!CheckedMultiply(source_input.maximum_rows, expected_width,
                       &source_maximum_cells) ||
      source_maximum_cells > std::numeric_limits<std::size_t>::max()) {
    return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                  "time-series source cell bound overflowed");
  }
  source_input.maximum_cells =
      static_cast<std::size_t>(source_maximum_cells);
  source_input.maximum_memory_bytes = selected_source_memory_grant;
  const auto append_rollup_evidence =
      [&](api::EngineApiResult* api_result) {
        if (api_result == nullptr || rollup_candidate == nullptr) return;
        api_result->evidence.push_back(
            {"canonical.time_series_rollup_candidate",
             rollup_candidate->time_series_rollup_capability_uuid});
        api_result->evidence.push_back(
            {"canonical.time_series_rollup_generation",
             std::to_string(
                 rollup_candidate->time_series_rollup_generation)});
        api_result->evidence.push_back(
            {"canonical.time_series_visible_late_arrival_generation",
             std::to_string(rollup_candidate
                                ->time_series_visible_late_arrival_generation)});
        api_result->evidence.push_back(
            {"canonical.time_series_rollup_rejection",
             rollup_candidate_stale ? "stale_generation"
                                    : "equivalence_unproved"});
        api_result->evidence.push_back(
            {"canonical.time_series_rollup_request_derivation",
             "engine_loaded_provider_generation_metadata"});
      };

  std::optional<CanonicalObjectFreeValuesExecutionRequest>
      composition_planning_request;
  std::optional<LivePhysicalPlanningResult> composition_physical;
  std::optional<PreparedFilterRoot> composition_filter;
  std::optional<PreparedProjectRoot> composition_project;
  std::optional<PreparedSortRoot> composition_sort;
  std::optional<exec::ExecutorColumnDescriptor> composition_row_number;
  std::optional<PreparedGlobalAggregateRoot> composition_count_star;
  std::optional<PreparedLimitRoot> composition_limit;
  std::optional<PreparedLiveSetNode> composition_set;
  MaterializedValues composition_set_values;
  std::optional<PreparedRecursiveCteRoot> composition_recursive_cte;
  bool composition_nonrecursive_cte = false;
  MaterializedValues composition_state;
  std::string composition_filter_capability_uuid;
  std::string composition_project_capability_uuid;
  std::string composition_sort_capability_uuid;
  std::string composition_window_capability_uuid;
  std::string composition_window_order_evidence_uuid;
  std::string composition_aggregate_capability_uuid;
  std::string composition_cte_capability_uuid;
  std::string composition_cte_implementation_id;
  std::string composition_limit_capability_uuid;
  std::string composition_limit_implementation_id;
  std::string composition_set_values_capability_uuid;
  std::string composition_set_root_capability_uuid;
  std::string composition_recursive_term_capability_uuid;
  std::string composition_recursive_root_capability_uuid;
  std::optional<exec::CanonicalAcceptedJoinKind> composition_mixed_join_kind;
  std::string composition_mixed_join_component;
  std::string composition_mixed_join_operation;
  std::string composition_relational_scan_capability_uuid;
  std::string composition_mixed_join_capability_uuid;
  CanonicalRelationalExpressionRowBinding
      composition_mixed_join_predicate_binding;
  std::string composition_asof_join_capability_uuid;
  std::string composition_asof_join_implementation_id;
  std::string composition_asof_transformation_receipt;
  exec::CanonicalTimeSeriesAsofInputBindingV1 composition_asof_left_binding;
  exec::CanonicalTimeSeriesAsofInputBindingV1 composition_asof_right_binding;
  std::int64_t composition_asof_tolerance_ns = 0;
  std::uint64_t composition_asof_maximum_comparisons = 0;
  std::size_t composition_asof_maximum_output_rows = 0;
  bool composition_asof_left_outer = false;
  std::uint64_t composition_limit_count = 0;
  std::uint64_t composition_limit_offset = 0;
  bool composition_fetch_first_rows_only = false;
  // Standalone and composed time-series forms share one canonical optimizer
  // and execution spine. Keep this scope so the preparation-only state does
  // not escape into the provider callback below.
  {
    plan::CanonicalMgaStatementContext current_logical_mga;
    current_logical_mga.statement_uuid = mga.statement_uuid;
    current_logical_mga.statement_timestamp = mga.statement_timestamp;
    current_logical_mga.owning_transaction_uuid =
        mga.owning_transaction_uuid;
    current_logical_mga.statement_snapshot_uuid =
        mga.statement_snapshot_uuid;
    current_logical_mga.statement_metadata_snapshot_uuid =
        mga.statement_metadata_snapshot_uuid;
    current_logical_mga.owning_local_transaction_id =
        mga.owning_local_transaction_id;
    current_logical_mga.visible_committed_high_watermark =
        mga.visible_committed_high_watermark;
    current_logical_mga.oldest_active_transaction_id =
        mga.oldest_active_transaction_id;
    current_logical_mga.oldest_interesting_transaction_id =
        mga.oldest_interesting_transaction_id;
    current_logical_mga.oldest_snapshot_transaction_id =
        mga.oldest_snapshot_transaction_id;
    current_logical_mga.retention_horizon_transaction_id =
        mga.retention_horizon_transaction_id;
    current_logical_mga.active_excluded_local_transaction_ids =
        mga.active_excluded_local_transaction_ids;
    current_logical_mga.in_doubt_excluded_local_transaction_ids =
        mga.in_doubt_excluded_local_transaction_ids;
    current_logical_mga.snapshot_kind = mga.snapshot_kind;
    current_logical_mga.publication_inventory_next_local_transaction_id =
        mga.publication_inventory_next_local_transaction_id;
    current_logical_mga.inventory_authoritative = mga.inventory_authoritative;
    current_logical_mga.complete = mga.complete;
    current_logical_mga.current = true;
    auto registered_logical_mga = current_logical_mga;
    registered_logical_mga.current = false;
    if (!plan::CanonicalMgaStatementContextEqual(
            logical.logical_graph.mga_statement_context,
            registered_logical_mga) ||
        !plan::CanonicalMgaStatementContextEqual(
            logical.property_catalog.mga_statement_context,
            registered_logical_mga)) {
      return refuse("SB_MODEL_MGA_CONTEXT_MISMATCH_V1",
                    "time-series logical bridge MGA cohort changed");
    }
    logical.logical_graph.mga_statement_context = current_logical_mga;
    logical.property_catalog.mga_statement_context = current_logical_mga;

    opt::CanonicalNativeObjectAdmissionContext admission_context;
    admission_context.statement_uuid =
        input.context.statement_uuid.canonical;
    admission_context.catalog_snapshot_uuid =
        input.context.statement_metadata_snapshot_uuid.canonical;
    admission_context.security_context_uuid =
        input.context.authorization_context.authority_uuid.canonical;
    admission_context.catalog_generation =
        input.context.catalog_generation_id;
    admission_context.authorization_catalog_generation =
        input.context.authorization_context.catalog_generation_id;
    admission_context.security_epoch =
        input.context.authorization_context.security_epoch;
    admission_context.policy_epoch =
        input.context.authorization_context.policy_epoch;
    admission_context.resource_epoch = input.context.resource_epoch;
    admission_context.capability_snapshot_uuid =
        input.context.optimizer_capability_snapshot_uuid.canonical;
    admission_context.resource_snapshot_uuid =
        input.context.optimizer_resource_snapshot_uuid.canonical;
    admission_context.route_snapshot_uuid =
        input.context.optimizer_route_snapshot_uuid.canonical;
    admission_context.route_epoch = input.context.optimizer_route_epoch;
    admission_context.route_generation =
        input.context.optimizer_route_generation;
    admission_context.memory_budget_bytes =
        input.context.optimizer_memory_budget_bytes;
    admission_context.maximum_candidate_count =
        input.context.optimizer_maximum_candidate_count;
    admission_context.maximum_memo_groups =
        input.context.optimizer_maximum_memo_groups;
    admission_context.maximum_search_steps =
        input.context.optimizer_maximum_search_steps;
    admission_context.maximum_planning_time_ns =
        input.context.optimizer_maximum_planning_time_ns;
    admission_context.spill_allowed = input.context.optimizer_spill_allowed;
    admission_context.local_transaction_id =
        input.context.local_transaction_id;
    admission_context.statement_snapshot_id =
        input.context.snapshot_visible_through_local_transaction_id;
    admission_context.mga_statement_context = current_logical_mga;
    const auto monotonic_parse = std::from_chars(
        input.context.current_monotonic_ns.data(),
        input.context.current_monotonic_ns.data() +
            input.context.current_monotonic_ns.size(),
        admission_context.admitted_at_monotonic_ns);
    if (input.context.current_monotonic_ns.empty() ||
        monotonic_parse.ec != std::errc{} ||
        monotonic_parse.ptr != input.context.current_monotonic_ns.data() +
                                   input.context.current_monotonic_ns.size() ||
        admission_context.admitted_at_monotonic_ns == 0) {
      return refuse("QOW-DIAG-OPTIMIZER-ADMISSION-BOUND-REQUEST-V1",
                    "time-series optimizer monotonic authority is absent");
    }
    admission_context.metadata_snapshot_engine_owned = true;
    admission_context.authorization_context_engine_owned = true;
    admission_context.catalog_object_uuids = admitted_object_uuids;
    admission_context.authorized_object_uuids = admitted_object_uuids;
    admission_context.catalog_object_evidence_engine_owned = true;
    admission_context.authorization_object_evidence_engine_owned = true;
    auto canonical_admission =
        opt::BuildCanonicalObjectAwareNativeOptimizerAdmissionRequest(
            logical.logical_graph, logical.property_catalog,
            admission_context);
    if (!canonical_admission.built ||
        !canonical_admission.admission.admitted ||
        !canonical_admission.admission.planning_allowed ||
        canonical_admission.admission.data_access_allowed ||
        canonical_admission.admission.evidence.size() != 8) {
      return refuse(
          canonical_admission.diagnostic_id.empty()
              ? "QOW-DIAG-OPTIMIZER-ADMISSION-BOUND-REQUEST-V1"
              : canonical_admission.diagnostic_id,
          canonical_admission.field_id.empty()
              ? "time-series canonical optimizer admission was refused"
              : canonical_admission.field_id);
    }
    result.optimizer_admitted = true;
    composition_planning_request.emplace(
        CanonicalObjectFreeValuesExecutionRequest{
            input.context, dag, canonical_admission.request,
            canonical_admission.admission});
    composition_planning_request->expression_services.comparison_evaluator =
        [context = input.context](const api::EngineTypedValue& left,
                                  const api::EngineTypedValue& right,
                                  int* comparison,
                                  std::string* diagnostic_id,
                                  std::string* refusal_detail) {
          return CompareCanonicalRelationalScalarsV1(
              context, left, right, comparison, diagnostic_id,
              refusal_detail);
        };

    composition_state.ok = true;
    composition_state.batch.columns = public_columns;
    // The model exchange preserves the exact public TIMESTAMP_TZ carrier.
    // Relational expressions consume its canonical core `timestamp` type;
    // normalize only this copied adapter schema after verifying the retained
    // timezone carrier. UUID, type UUID, nullability, and timezone metadata
    // remain unchanged.
    for (auto& column : composition_state.batch.columns) {
      if (column.descriptor.canonical_type_name != "timestamp_tz") continue;
      if (column.descriptor.encoded_descriptor.find(
              "timezone_profile_id=") == std::string::npos) {
        return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                      "time-series timestamp timezone carrier is absent");
      }
      column.descriptor.canonical_type_name = "timestamp";
    }
    for (std::size_t ordinal = 0; ordinal < outputs.size(); ++ordinal) {
      const auto descriptor = std::ranges::find_if(
          dag.descriptors, [&](const auto& candidate_descriptor) {
            return candidate_descriptor.descriptor_id ==
                   outputs[ordinal]->descriptor_id;
          });
      if (descriptor == dag.descriptors.end()) {
        return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                      "time-series source publication descriptor is absent");
      }
      exec::CanonicalResultColumnBinding binding;
      binding.physical_column_ordinal = ordinal;
      binding.visible = outputs[ordinal]->visible;
      if (binding.visible) {
        binding.published_descriptor =
            exec::CanonicalResultColumnDescriptor{
                static_cast<std::uint32_t>(ordinal),
                outputs[ordinal]->output_name_utf8,
                descriptor->descriptor_uuid, descriptor->type_uuid,
                ResultNullability(descriptor->nullability),
                descriptor->collation_uuid,
                descriptor->timezone_profile_id};
      }
      composition_state.result_bindings.push_back(std::move(binding));
    }
    const auto source_batch_validation =
        exec::ValidateCanonicalDescriptorBatch(
            composition_state.batch, source->output_descriptor_ids);
    if (!source_batch_validation.ok) {
      return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                    source_batch_validation.detail);
    }

    std::vector<LivePhysicalNodeProfile> profiles;
    LivePhysicalNodeProfile source_profile;
    source_profile.logical_node_id = source->node_id;
    source_profile.implementation_id =
        "physical_time_series_range_scan_v1";
    source_profile.capability_uuid =
        planned.selected_candidate.capability_uuid;
    source_profile.logical_node_kind = logical_source->node_kind;
    source_profile.physical_node_kind =
        downsample_operation ? exec::PhysicalNodeKind::kAggregate
                             : exec::PhysicalNodeKind::kScan;
    source_profile.transformation_rule_id =
        "canonical.time-series.scan.v1";
    source_profile.estimated_rows = 1;
    source_profile.memory_bytes_required = std::max<std::uint64_t>(
        1, planned.selected_candidate.cost.memory_bytes_required);
    source_profile.page_read_sequential_units = 1;
    source_profile.mga_visibility_checks_expected = 1;
    source_profile.storage_read_capable = true;
    source_profile.mga_visibility_capable = true;
    source_profile.residual_predicate_required = true;
    source_profile.storage_recheck_required = true;
    source_profile.compatibility_profile_id = "time_series.local.v1";
    profiles.push_back(std::move(source_profile));

    const auto logical_node_for = [&](const std::uint32_t node_id) {
      return std::ranges::find_if(
          canonical_admission.request.logical_graph.nodes,
          [&](const auto& node) { return node.logical_node_id == node_id; });
    };
    const auto descriptor_for = [&](const std::uint32_t descriptor_id) {
      return std::ranges::find_if(dag.descriptors, [&](const auto& descriptor) {
        return descriptor.descriptor_id == descriptor_id;
      });
    };
    const auto core_manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
    if (!core_manifest.ok()) {
      return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                    "current core datatype catalog is unavailable");
    }
    const auto type_uuid_for = [&](const std::string_view stable_name) {
      return ExactCanonicalCoreDatatypeTypeUuidV1(stable_name);
    };
    auto previous_logical = logical_node_for(source->node_id);
    std::unordered_set<api::RelationalDagNodeKind> consumer_kinds;
    for (const auto* consumer : consumer_chain) {
      const auto logical_consumer = logical_node_for(consumer->node_id);
      if (previous_logical ==
              canonical_admission.request.logical_graph.nodes.end() ||
          logical_consumer ==
              canonical_admission.request.logical_graph.nodes.end() ||
          consumer->input_node_ids !=
              std::vector<std::uint32_t>{previous_logical->logical_node_id} ||
          !consumer_kinds.insert(consumer->node_kind).second) {
        return refuse("SBLR.PLAN_TREE.INVALID_HANDLE",
                      "time-series consumer identity or ordering is invalid");
      }
      LivePhysicalNodeProfile profile;
      profile.logical_node_id = consumer->node_id;
      profile.logical_node_kind = logical_consumer->node_kind;
      profile.estimated_rows = 65536;
      profile.memory_bytes_required = 1;
      profile.minimum_input_count = 1;
      profile.maximum_input_count = 1;
      profile.runtime_peak_from_callback_batches = true;
      if (consumer->node_kind == api::RelationalDagNodeKind::kFilter) {
        if (consumer->semantic_variant_id != "filter.where.v1") {
          return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                        "time-series FILTER semantic is not canonical");
        }
        auto prepared = PrepareFilterRootForComposition(
            dag, *logical_consumer, *previous_logical, composition_state);
        if (!prepared.ok) {
          return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                        prepared.detail);
        }
        composition_filter = std::move(prepared);
        composition_filter_capability_uuid = DerivedCanonicalUuid(
            identity_scope, "time-series.filter.capability");
        profile.implementation_id = "filter.3vl.row.v1";
        profile.capability_uuid = composition_filter_capability_uuid;
        profile.physical_node_kind = exec::PhysicalNodeKind::kFilter;
        profile.transformation_rule_id =
            "canonical.time-series.filter.3vl.v1";
        profile.memory_bytes_required = planning.memory_budget_bytes;
      } else if (consumer->node_kind ==
                 api::RelationalDagNodeKind::kProject) {
        if (consumer->semantic_variant_id != "project.select-list.v1" ||
            consumer->bound_expression_ids.empty()) {
          return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                        "time-series PROJECT semantic is not canonical");
        }
        auto prepared = PrepareExpressionProjectRootForComposition(
            dag, *logical_consumer, *previous_logical, composition_state,
            composition_planning_request->expression_services);
        if (!prepared.ok) {
          return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                        prepared.detail);
        }
        std::unordered_set<std::size_t> selected_source_ordinals;
        prepared.projected_columns.reserve(prepared.expressions.size());
        for (std::size_t ordinal = 0;
             ordinal < prepared.expressions.size(); ++ordinal) {
          const auto& selected = prepared.expressions[ordinal];
          const auto expression = std::ranges::find_if(
              dag.expressions, [&](const auto& candidate) {
                return candidate.expression_id == selected.expression_id;
              });
          if (expression == dag.expressions.end() ||
              expression->expression_kind !=
                  api::RelationalExpressionKind::kIdentifier ||
              selected.row_binding.slots.size() != 1 ||
              selected.row_binding.slots.front().row_ordinal >=
                  previous_logical->output_descriptor_ids.size() ||
              selected.row_binding.slots.front().descriptor_id !=
                  consumer->output_descriptor_ids[ordinal] ||
              previous_logical->output_descriptor_ids
                      [selected.row_binding.slots.front().row_ordinal] !=
                  consumer->output_descriptor_ids[ordinal] ||
              !selected_source_ordinals
                   .insert(selected.row_binding.slots.front().row_ordinal)
                   .second) {
            return refuse(
                "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                "time-series PROJECT is not an exact descriptor-direct "
                "identifier selection");
          }
          prepared.projected_columns.push_back(
              selected.row_binding.slots.front().row_ordinal);
        }
        prepared.expression_projection = false;
        composition_state.batch = prepared.expression_output_batch;
        composition_state.result_bindings = prepared.result_bindings;
        composition_project = std::move(prepared);
        composition_project_capability_uuid = DerivedCanonicalUuid(
            identity_scope, "time-series.project.capability");
        profile.implementation_id = "project.descriptor-direct.v1";
        profile.capability_uuid = composition_project_capability_uuid;
        profile.physical_node_kind = exec::PhysicalNodeKind::kProject;
        profile.transformation_rule_id =
            "canonical.time-series.project.descriptor-direct.v1";
        profile.memory_bytes_required = planning.memory_budget_bytes;
      } else if (consumer->node_kind ==
                 api::RelationalDagNodeKind::kSort) {
        if (consumer->semantic_variant_id != "sort.required-order.v1") {
          return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                        "time-series SORT semantic is not canonical");
        }
        auto sort_properties =
            canonical_admission.request.logical_properties;
        std::erase_if(sort_properties.properties, [&](const auto& property) {
          return std::ranges::find(
                     logical_consumer->required_property_uuids,
                     property.property_uuid) ==
                 logical_consumer->required_property_uuids.end();
        });
        auto prepared = PrepareSortRootForComposition(
            input.context, dag, sort_properties, *logical_consumer,
            *previous_logical, composition_state);
        if (!prepared.ok) {
          return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                        prepared.detail);
        }
        composition_sort = std::move(prepared);
        composition_sort_capability_uuid = DerivedCanonicalUuid(
            identity_scope, "time-series.sort.capability");
        profile.implementation_id = "sort.typed.terms.v1";
        profile.capability_uuid = composition_sort_capability_uuid;
        profile.physical_node_kind = exec::PhysicalNodeKind::kSort;
        profile.transformation_rule_id =
            "canonical.time-series.sort.required-order.v1";
        profile.delivered_property_uuids =
            logical_consumer->delivered_property_uuids;
        profile.supported_property_kinds = {
            plan::CanonicalLogicalPropertyKind::kOrdering};
        profile.memory_bytes_required = planning.memory_budget_bytes;
      } else if (consumer->node_kind ==
                 api::RelationalDagNodeKind::kWindow) {
        if (!composition_sort.has_value()) {
          return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                        "time-series ROW_NUMBER binding is not exact");
        }
        const auto row_number =
            PrepareGlobalRowNumberWindowBindingForComposition(
            dag, canonical_admission.request.logical_properties, *consumer,
            *logical_consumer, *previous_logical, *composition_sort,
            composition_state.batch.columns.size(),
            composition_state.result_bindings.size(), type_uuid_for("int64"),
            "time-series");
        if (!row_number.ok) {
          return refuse(row_number.diagnostic_id, row_number.detail);
        }
        const auto* output_descriptor = row_number.result_descriptor;
        const auto& window_outputs = row_number.outputs;
        api::EngineDescriptor descriptor;
        descriptor.descriptor_uuid.canonical =
            output_descriptor->descriptor_uuid;
        descriptor.descriptor_kind = "scalar";
        descriptor.canonical_type_name = "int64";
        descriptor.encoded_descriptor =
            "type_uuid=" + output_descriptor->type_uuid +
            ";nullability=non_null";
        exec::ExecutorColumnDescriptor row_number_column{
            window_outputs.back()->output_name_utf8, descriptor, false,
            output_descriptor->descriptor_id};
        if (!api::QowCanonicalDescriptorIdentityV1(descriptor)) {
          return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                        "time-series ROW_NUMBER descriptor is invalid");
        }
        exec::CanonicalResultColumnBinding row_number_binding;
        row_number_binding.physical_column_ordinal =
            composition_state.batch.columns.size();
        row_number_binding.visible = true;
        row_number_binding.published_descriptor =
            exec::CanonicalResultColumnDescriptor{
                static_cast<std::uint32_t>(
                    composition_state.batch.columns.size()),
                row_number_column.stable_name,
                output_descriptor->descriptor_uuid,
                output_descriptor->type_uuid,
                exec::CanonicalResultNullability::kNonNull,
                std::nullopt, std::nullopt};
        composition_state.batch.columns.push_back(row_number_column);
        composition_state.result_bindings.push_back(
            std::move(row_number_binding));
        composition_row_number = std::move(row_number_column);
        composition_window_capability_uuid = DerivedCanonicalUuid(
            identity_scope, "time-series.window.row-number.capability");
        composition_window_order_evidence_uuid = DerivedCanonicalUuid(
            identity_scope + ":" +
                composition_sort->ordering_property_uuid,
            "time-series.window.deterministic-order");
        profile.implementation_id = "window.row-number.v1";
        profile.capability_uuid = composition_window_capability_uuid;
        profile.physical_node_kind = exec::PhysicalNodeKind::kWindow;
        profile.transformation_rule_id =
            "canonical.time-series.window.row-number.v1";
        profile.required_property_uuids =
            logical_consumer->required_property_uuids;
        profile.delivered_property_uuids =
            logical_consumer->delivered_property_uuids;
        profile.supported_property_kinds = {
            plan::CanonicalLogicalPropertyKind::kOrdering,
            plan::CanonicalLogicalPropertyKind::kWindow};
        profile.memory_bytes_required = planning.memory_budget_bytes;
      } else if (consumer->node_kind ==
                 api::RelationalDagNodeKind::kAggregate) {
        const auto aggregate_profile =
            MatchLiveUnaryAggregateExpressionProfileForComposition(
                consumer->semantic_variant_id);
        if (!aggregate_profile.matched ||
            !aggregate_profile.count_star ||
            aggregate_profile.function !=
                exec::CanonicalAggregateFunction::count ||
            aggregate_profile.distinct || aggregate_profile.has_filter) {
          return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                        "time-series aggregate is not exact global COUNT(*)");
        }
        const auto count_descriptor = descriptor_for(
            consumer->output_descriptor_ids.empty()
                ? 0
                : consumer->output_descriptor_ids.front());
        const auto int64_type_uuid = type_uuid_for("int64");
        if (consumer->output_descriptor_ids.size() != 1 ||
            count_descriptor == dag.descriptors.end() ||
            int64_type_uuid.empty() ||
            count_descriptor->type_uuid != int64_type_uuid ||
            count_descriptor->nullability !=
                api::RelationalNullability::kNonNull ||
            count_descriptor->collation_uuid.has_value() ||
            count_descriptor->timezone_profile_id.has_value() ||
            count_descriptor->width.has_value() ||
            count_descriptor->precision.has_value() ||
            count_descriptor->scale.has_value()) {
          return refuse(
              "SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
              "time-series COUNT(*) result descriptor is not canonical int64");
        }
        auto prepared = PrepareGlobalAggregateRootForComposition(
            dag, *logical_consumer, *previous_logical, composition_state,
            aggregate_profile.function, true, false, false);
        if (!prepared.ok) {
          return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                        prepared.detail);
        }
        composition_state.batch.columns = {prepared.result_column};
        composition_state.result_bindings = prepared.result_bindings;
        composition_count_star = std::move(prepared);
        composition_aggregate_capability_uuid = DerivedCanonicalUuid(
            identity_scope, "time-series.count-star.capability");
        profile.implementation_id = "aggregate.count-star.v1";
        profile.capability_uuid = composition_aggregate_capability_uuid;
        profile.physical_node_kind = exec::PhysicalNodeKind::kAggregate;
        profile.transformation_rule_id =
            aggregate_profile.transformation_id;
        profile.estimated_rows = 1;
        profile.memory_bytes_required = planning.memory_budget_bytes;
      } else if (consumer->node_kind == api::RelationalDagNodeKind::kCte) {
        if (consumer->semantic_variant_id != "cte.bound.v1" ||
            !consumer->bound_expression_ids.empty() ||
            !consumer->required_object_uuids.empty() ||
            !consumer->required_property_uuids.empty() ||
            !consumer->delivered_property_uuids.empty() ||
            consumer->output_descriptor_ids !=
                previous_logical->output_descriptor_ids ||
            logical_consumer->output_descriptor_ids !=
                previous_logical->output_descriptor_ids ||
            composition_state.batch.columns.size() !=
                consumer->output_descriptor_ids.size()) {
          return refuse(
              "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
              "time-series nonrecursive CTE is not an exact "
              "schema-preserving bound CTE");
        }
        const auto validated = exec::ValidateCanonicalDescriptorBatch(
            composition_state.batch, consumer->output_descriptor_ids);
        if (!validated.ok) {
          return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                        "time-series nonrecursive CTE input: " +
                            validated.detail);
        }
        composition_nonrecursive_cte = true;
        composition_cte_implementation_id =
            consumer->shareable ? "cte.bound.materialize.typed.v1"
                                : "cte.bound.inline.typed.v1";
        composition_cte_capability_uuid = DerivedCanonicalUuid(
            identity_scope,
            consumer->shareable
                ? "time-series.cte.materialize.capability"
                : "time-series.cte.inline.capability");
        profile.implementation_id = composition_cte_implementation_id;
        profile.capability_uuid = composition_cte_capability_uuid;
        profile.physical_node_kind = exec::PhysicalNodeKind::kCte;
        profile.transformation_rule_id =
            consumer->shareable
                ? "canonical.time-series.cte.materialize.v1"
                : "canonical.time-series.cte.inline.v1";
        profile.memory_bytes_required = planning.memory_budget_bytes;
        profile.runtime_auxiliary_from_first_input_batch =
            consumer->shareable;
      } else if (consumer->node_kind ==
                 api::RelationalDagNodeKind::kLimit) {
        const auto expected_arity =
            consumer->semantic_variant_id == "limit.bound-count.v1" ? 1U
                                                                    : 2U;
        composition_fetch_first_rows_only =
            consumer->semantic_variant_id ==
            "fetch.first-rows-only-offset.v1";
        if ((consumer->semantic_variant_id != "limit.bound-count.v1" &&
             consumer->semantic_variant_id !=
                 "limit.bound-count-offset.v1" &&
             !composition_fetch_first_rows_only) ||
            consumer->bound_expression_ids.size() != expected_arity) {
          return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                        "time-series LIMIT/FETCH semantic is not canonical");
        }
        CanonicalRelationalExpressionRuntime runtime(
            dag, composition_planning_request->expression_services);
        std::string detail;
        if (!EvaluateNonNegativeRowBoundForComposition(
                &runtime, consumer->bound_expression_ids.front(),
                &composition_limit_count, &detail) ||
            (expected_arity == 2 &&
             !EvaluateNonNegativeRowBoundForComposition(
                 &runtime, consumer->bound_expression_ids.back(),
                 &composition_limit_offset, &detail))) {
          return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                        detail.empty()
                            ? "time-series LIMIT/FETCH bound is invalid"
                            : detail);
        }
        auto prepared = PrepareLimitRootForComposition(
            dag, *logical_consumer, *previous_logical, composition_state);
        if (!prepared.ok) {
          return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                        prepared.detail);
        }
        composition_limit = std::move(prepared);
        composition_limit_implementation_id =
            composition_fetch_first_rows_only
                ? "fetch.native.rows-only.v1"
                : "limit.typed.v1";
        composition_limit_capability_uuid = DerivedCanonicalUuid(
            identity_scope, "time-series.limit.capability");
        profile.implementation_id = composition_limit_implementation_id;
        profile.capability_uuid = composition_limit_capability_uuid;
        profile.physical_node_kind = exec::PhysicalNodeKind::kLimit;
        profile.transformation_rule_id =
            composition_fetch_first_rows_only
                ? "canonical.time-series.fetch.first-rows-only.v1"
                : "canonical.time-series.limit.bound-count-offset.v1";
        profile.memory_bytes_required = planning.memory_budget_bytes;
      } else {
        return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                      "time-series consumer kind is not prepared");
      }
      profiles.push_back(std::move(profile));
      previous_logical = logical_consumer;
    }
    constexpr std::size_t kTimeSeriesCompositionRowBound = 65536;
    if (set_root != nullptr) {
      const auto logical_values = logical_node_for(set_values->node_id);
      const auto logical_root = logical_node_for(set_root->node_id);
      if (logical_values ==
              canonical_admission.request.logical_graph.nodes.end() ||
          logical_root ==
              canonical_admission.request.logical_graph.nodes.end() ||
          logical_values->node_kind !=
              plan::CanonicalLogicalRelationalNodeKind::kValues ||
          logical_values->semantic_variant_id != "values.literal-table.v1" ||
          !logical_values->input_logical_node_ids.empty() ||
          logical_root->node_kind !=
              plan::CanonicalLogicalRelationalNodeKind::kSetOperation ||
          logical_root->semantic_variant_id != "set-operation.union-all.v1" ||
          logical_root->input_logical_node_ids !=
              std::vector<std::uint32_t>{previous_logical->logical_node_id,
                                         logical_values->logical_node_id}) {
        return refuse(
            "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
            "time-series UNION ALL logical input identity is not exact");
      }
      auto right = MaterializeValues(
          dag, *logical_values,
          composition_planning_request->expression_services);
      if (!right.ok) {
        return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                      "time-series UNION ALL right VALUES: " + right.detail);
      }
      if (right.batch.rows.empty() ||
          right.batch.rows.size() >= kTimeSeriesCompositionRowBound) {
        return refuse(
            "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
            "time-series UNION ALL leaves no bounded source row budget");
      }
      auto prepared = PrepareSetOperationRootForComposition(
          input.context, dag, *logical_root, composition_state, right,
          time_series_set_profile);
      if (!prepared.ok) {
        return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                      "time-series UNION ALL: " + prepared.detail);
      }
      std::uint64_t comparison_bound = 0;
      std::uint64_t collation_comparison_count = 0;
      if (!BoundSetOperationEqualityComparisonsForComposition(
              prepared, time_series_set_profile,
              kTimeSeriesCompositionRowBound, &comparison_bound,
              &collation_comparison_count) ||
          comparison_bound > std::numeric_limits<std::size_t>::max()) {
        return refuse(
            "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
            "time-series UNION ALL comparison bound overflowed");
      }
      std::uint64_t values_memory = 1;
      if (!AddBatchMemoryBytes(right.batch, &values_memory) ||
          values_memory > planning.memory_budget_bytes) {
        return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                      "time-series UNION ALL VALUES memory bound was exceeded");
      }
      composition_set_values_capability_uuid = DerivedCanonicalUuid(
          identity_scope, "time-series.set-values.capability");
      composition_set_root_capability_uuid = DerivedCanonicalUuid(
          identity_scope, "time-series.set-union-all.capability");

      LivePhysicalNodeProfile values_profile;
      values_profile.logical_node_id = logical_values->logical_node_id;
      values_profile.implementation_id = std::string(kValuesImplementationId);
      values_profile.capability_uuid =
          composition_set_values_capability_uuid;
      values_profile.logical_node_kind = logical_values->node_kind;
      values_profile.physical_node_kind = exec::PhysicalNodeKind::kValues;
      values_profile.transformation_rule_id =
          "canonical.time-series.set-values.materialize.v1";
      values_profile.estimated_rows = right.batch.rows.size();
      values_profile.memory_bytes_required = values_memory;
      profiles.push_back(std::move(values_profile));

      LivePhysicalNodeProfile set_profile;
      set_profile.logical_node_id = logical_root->logical_node_id;
      set_profile.implementation_id =
          time_series_set_profile.implementation_id;
      set_profile.capability_uuid = composition_set_root_capability_uuid;
      set_profile.logical_node_kind = logical_root->node_kind;
      set_profile.physical_node_kind = exec::PhysicalNodeKind::kSetOperation;
      set_profile.transformation_rule_id =
          "canonical.time-series.set.union-all.ordinal.v1";
      set_profile.estimated_rows = kTimeSeriesCompositionRowBound;
      set_profile.memory_bytes_required = planning.memory_budget_bytes;
      set_profile.minimum_input_count = 2;
      set_profile.maximum_input_count = 2;
      set_profile.runtime_peak_from_callback_batches = true;
      profiles.push_back(std::move(set_profile));

      composition_state.batch.columns = prepared.result_columns;
      composition_state.result_bindings = prepared.result_bindings;
      composition_set = PreparedLiveSetNode{
          time_series_set_profile, std::move(prepared),
          kTimeSeriesCompositionRowBound,
          std::max<std::size_t>(
              1, static_cast<std::size_t>(comparison_bound))};
      composition_set_values = std::move(right);
      previous_logical = logical_root;
    }
    if (recursive_root != nullptr) {
      const auto logical_term = logical_node_for(recursive_term->node_id);
      const auto logical_root = logical_node_for(recursive_root->node_id);
      if (logical_term ==
              canonical_admission.request.logical_graph.nodes.end() ||
          logical_root ==
              canonical_admission.request.logical_graph.nodes.end() ||
          !logical_term->input_logical_node_ids.empty() ||
          logical_root->input_logical_node_ids !=
              std::vector<std::uint32_t>{previous_logical->logical_node_id,
                                         logical_term->logical_node_id} ||
          composition_state.batch.columns.size() != 1 ||
          composition_state.batch.columns.front().descriptor
                  .canonical_type_name != "int64") {
        return refuse(
            "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
            "time-series recursive logical branch is not an exact int64 anchor and empty term");
      }
      CanonicalRelationalExpressionRuntime runtime(
          dag, composition_planning_request->expression_services);
      std::uint64_t upper_bound = 0;
      std::string detail;
      if (!EvaluateNonNegativeRowBoundForComposition(
              &runtime, recursive_root->bound_expression_ids.front(),
              &upper_bound, &detail) ||
          upper_bound >= kTimeSeriesCompositionRowBound ||
          upper_bound >= static_cast<std::uint64_t>(
                             std::numeric_limits<std::size_t>::max())) {
        return refuse(
            "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
            detail.empty()
                ? "time-series recursive upper bound exceeds its admitted work bound"
                : detail);
      }
      PreparedRecursiveCteRoot prepared;
      prepared.profile = MatchLiveRecursiveCteProfileForComposition(
          recursive_root->semantic_variant_id);
      prepared.anchor_columns = composition_state.batch.columns;
      prepared.term = PrepareLiveRecursiveCteTerm(
          prepared.profile, prepared.anchor_columns,
          static_cast<std::int64_t>(upper_bound));
      if (!BindPreparedRecursiveCteCardinality(
              &prepared, profiles, previous_logical->logical_node_id,
              upper_bound, kTimeSeriesCompositionRowBound)) {
        return refuse(
            "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
            "time-series recursive cardinality bound is invalid");
      }
      if (!BindPreparedRecursiveCtePeakMemory(
              &prepared, planning.memory_budget_bytes)) {
        return refuse(
            "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
            "time-series recursive payload peak exceeds its admitted memory budget");
      }
      composition_recursive_cte = prepared;
      composition_recursive_term_capability_uuid = DerivedCanonicalUuid(
          identity_scope, "time-series.recursive-term.capability");
      composition_recursive_root_capability_uuid = DerivedCanonicalUuid(
          identity_scope, "time-series.recursive-root.capability");

      LivePhysicalNodeProfile term_profile;
      term_profile.logical_node_id = recursive_term->node_id;
      term_profile.implementation_id =
          "cte.recursive-term.int64-increment.typed.v1";
      term_profile.capability_uuid =
          composition_recursive_term_capability_uuid;
      term_profile.logical_node_kind = logical_term->node_kind;
      term_profile.physical_node_kind = exec::PhysicalNodeKind::kCte;
      term_profile.transformation_rule_id =
          "canonical.cte.recursive-term-int64-increment.v1";
      term_profile.memory_bytes_required = 1;
      profiles.push_back(std::move(term_profile));

      LivePhysicalNodeProfile root_profile;
      root_profile.logical_node_id = recursive_root->node_id;
      root_profile.implementation_id = prepared.profile.implementation_id;
      root_profile.capability_uuid =
          composition_recursive_root_capability_uuid;
      root_profile.logical_node_kind = logical_root->node_kind;
      root_profile.physical_node_kind = exec::PhysicalNodeKind::kRecursiveCte;
      root_profile.transformation_rule_id = prepared.profile.transformation_id;
      root_profile.estimated_rows = prepared.maximum_result_row_count;
      root_profile.memory_bytes_required =
          prepared.planned_peak_memory_bytes;
      root_profile.minimum_input_count = 2;
      root_profile.maximum_input_count = 2;
      profiles.push_back(std::move(root_profile));
      previous_logical = logical_root;
    }
    if (mixed_join != nullptr) {
      const auto relational_logical =
          logical_node_for(relational_scan->node_id);
      const auto join_logical = logical_node_for(mixed_join->node_id);
      if (relational_logical ==
              canonical_admission.request.logical_graph.nodes.end() ||
          join_logical ==
              canonical_admission.request.logical_graph.nodes.end() ||
          join_logical->input_logical_node_ids !=
              mixed_join->input_node_ids) {
        return refuse("SBLR.PLAN_TREE.INVALID_HANDLE",
                      "time-series mixed JOIN logical identity is incomplete");
      }
      if (mixed_join->semantic_variant_id == "join.cross.v1") {
        composition_mixed_join_kind =
            exec::CanonicalAcceptedJoinKind::kCross;
        composition_mixed_join_component = "cross";
        composition_mixed_join_operation = "CROSS JOIN";
      } else if (mixed_join->semantic_variant_id == "join.inner.v1") {
        composition_mixed_join_kind =
            exec::CanonicalAcceptedJoinKind::kInner;
        composition_mixed_join_component = "inner";
        composition_mixed_join_operation = "INNER JOIN";
      } else if (mixed_join->semantic_variant_id ==
                 "join.left-outer.v1") {
        composition_mixed_join_kind =
            exec::CanonicalAcceptedJoinKind::kLeftOuter;
        composition_mixed_join_component = "left-outer";
        composition_mixed_join_operation = "LEFT OUTER JOIN";
      } else if (mixed_join->semantic_variant_id ==
                 "join.right-outer.v1") {
        composition_mixed_join_kind =
            exec::CanonicalAcceptedJoinKind::kRightOuter;
        composition_mixed_join_component = "right-outer";
        composition_mixed_join_operation = "RIGHT OUTER JOIN";
      } else if (mixed_join->semantic_variant_id ==
                 "join.full-outer.v1") {
        composition_mixed_join_kind =
            exec::CanonicalAcceptedJoinKind::kFullOuter;
        composition_mixed_join_component = "full-outer";
        composition_mixed_join_operation = "FULL OUTER JOIN";
      } else if (mixed_join->semantic_variant_id ==
                 "join.left-semi.v1") {
        composition_mixed_join_kind =
            exec::CanonicalAcceptedJoinKind::kLeftSemi;
        composition_mixed_join_component = "left-semi";
        composition_mixed_join_operation = "LEFT SEMI JOIN";
      } else if (mixed_join->semantic_variant_id ==
                 "join.left-anti.v1") {
        composition_mixed_join_kind =
            exec::CanonicalAcceptedJoinKind::kLeftAnti;
        composition_mixed_join_component = "left-anti";
        composition_mixed_join_operation = "LEFT ANTI JOIN";
      } else {
        return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                      "time-series mixed JOIN kind is not canonical");
      }
      const auto left_input_node =
          typed_node_for(mixed_join->input_node_ids.front());
      if (left_input_node == dag.nodes.end() ||
          ((*composition_mixed_join_kind ==
                exec::CanonicalAcceptedJoinKind::kLeftSemi ||
            *composition_mixed_join_kind ==
                exec::CanonicalAcceptedJoinKind::kLeftAnti) &&
           mixed_join->output_descriptor_ids !=
               left_input_node->output_descriptor_ids)) {
        return refuse(
            "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
            "time-series mixed JOIN left input or semi/anti schema is invalid");
      }
      const bool predicate_join =
          *composition_mixed_join_kind !=
          exec::CanonicalAcceptedJoinKind::kCross;
      if (mixed_join->bound_expression_ids.size() !=
          static_cast<std::size_t>(predicate_join)) {
        return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                      "time-series mixed JOIN predicate arity is invalid");
      }
      if (predicate_join) {
        std::vector<std::uint32_t> predicate_descriptors;
        for (const auto input_node_id : mixed_join->input_node_ids) {
          const auto input_node = typed_node_for(input_node_id);
          predicate_descriptors.insert(
              predicate_descriptors.end(),
              input_node->output_descriptor_ids.begin(),
              input_node->output_descriptor_ids.end());
        }
        std::string detail;
        if (!PrepareInputRowBindingForComposition(
                dag, mixed_join->bound_expression_ids.front(),
                predicate_descriptors,
                &composition_mixed_join_predicate_binding, &detail)) {
          return refuse(
              "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
              detail.empty()
                  ? "time-series mixed JOIN predicate is not bound"
                  : detail);
        }
      }
      composition_relational_scan_capability_uuid = DerivedCanonicalUuid(
          identity_scope, "time-series.mixed-join.heap-scan.capability");
      composition_mixed_join_capability_uuid = DerivedCanonicalUuid(
          identity_scope, "time-series.mixed-join.capability");
      LivePhysicalNodeProfile heap_profile;
      heap_profile.logical_node_id = relational_scan->node_id;
      heap_profile.implementation_id = "scan.heap.v1";
      heap_profile.capability_uuid =
          composition_relational_scan_capability_uuid;
      heap_profile.logical_node_kind =
          plan::CanonicalLogicalRelationalNodeKind::kRelationSource;
      heap_profile.physical_node_kind = exec::PhysicalNodeKind::kScan;
      heap_profile.transformation_rule_id =
          "canonical.time-series.mixed-join.heap-scan.v1";
      heap_profile.estimated_rows = 1;
      heap_profile.memory_bytes_required =
          input.context.optimizer_memory_budget_bytes;
      heap_profile.page_read_sequential_units = 1;
      heap_profile.mga_visibility_checks_expected = 1;
      heap_profile.storage_read_capable = true;
      heap_profile.mga_visibility_capable = true;
      profiles.push_back(std::move(heap_profile));
      LivePhysicalNodeProfile join_profile;
      join_profile.logical_node_id = mixed_join->node_id;
      join_profile.implementation_id =
          "join." + composition_mixed_join_component +
          ".3vl.nested.v1";
      join_profile.capability_uuid =
          composition_mixed_join_capability_uuid;
      join_profile.logical_node_kind =
          plan::CanonicalLogicalRelationalNodeKind::kJoin;
      join_profile.physical_node_kind = exec::PhysicalNodeKind::kJoin;
      join_profile.transformation_rule_id =
          "canonical.time-series.mixed-join." +
          composition_mixed_join_component + ".v1";
      join_profile.estimated_rows = kTimeSeriesCompositionRowBound;
      join_profile.memory_bytes_required = planning.memory_budget_bytes;
      join_profile.minimum_input_count = 2;
      join_profile.maximum_input_count = 2;
      join_profile.runtime_peak_from_callback_batches = true;
      profiles.push_back(std::move(join_profile));

      composition_state.batch.columns.clear();
      composition_state.result_bindings.clear();
      std::vector<const api::RelationalOutputRecord*> join_outputs;
      for (const auto& output : dag.outputs) {
        if (output.relation_node_id == mixed_join->node_id) {
          join_outputs.push_back(&output);
        }
      }
      std::ranges::sort(join_outputs, {},
                        &api::RelationalOutputRecord::ordinal);
      if (join_outputs.size() !=
          mixed_join->output_descriptor_ids.size()) {
        return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                      "time-series mixed JOIN result coverage is incomplete");
      }
      for (std::size_t ordinal = 0; ordinal < join_outputs.size();
           ++ordinal) {
        const auto descriptor =
            descriptor_for(join_outputs[ordinal]->descriptor_id);
        if (descriptor == dag.descriptors.end() ||
            join_outputs[ordinal]->ordinal != ordinal ||
            join_outputs[ordinal]->descriptor_id !=
                mixed_join->output_descriptor_ids[ordinal]) {
          return refuse(
              "SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
              "time-series mixed JOIN result descriptor is unresolved");
        }
        exec::CanonicalResultColumnBinding binding;
        binding.physical_column_ordinal = ordinal;
        binding.visible = join_outputs[ordinal]->visible;
        if (binding.visible) {
          const auto left_width =
              left_input_node->output_descriptor_ids.size();
          const bool null_extended_output =
              ((*composition_mixed_join_kind ==
                    exec::CanonicalAcceptedJoinKind::kRightOuter ||
                *composition_mixed_join_kind ==
                    exec::CanonicalAcceptedJoinKind::kFullOuter) &&
               ordinal < left_width) ||
              ((*composition_mixed_join_kind ==
                    exec::CanonicalAcceptedJoinKind::kLeftOuter ||
                *composition_mixed_join_kind ==
                    exec::CanonicalAcceptedJoinKind::kFullOuter) &&
               ordinal >= left_width);
          binding.published_descriptor =
              exec::CanonicalResultColumnDescriptor{
                  static_cast<std::uint32_t>(ordinal),
                  join_outputs[ordinal]->output_name_utf8,
                  descriptor->descriptor_uuid, descriptor->type_uuid,
                  null_extended_output
                      ? exec::CanonicalResultNullability::kNullable
                      : ResultNullability(descriptor->nullability),
                  descriptor->collation_uuid,
                  descriptor->timezone_profile_id};
        }
        composition_state.result_bindings.push_back(std::move(binding));
      }
    }
    if (asof_join != nullptr) {
      const auto relational_logical =
          logical_node_for(relational_scan->node_id);
      const auto join_logical = logical_node_for(asof_join->node_id);
      const auto left_input = typed_node_for(asof_join->input_node_ids[0]);
      const auto right_input = typed_node_for(asof_join->input_node_ids[1]);
      if (relational_logical ==
              canonical_admission.request.logical_graph.nodes.end() ||
          join_logical ==
              canonical_admission.request.logical_graph.nodes.end() ||
          left_input == dag.nodes.end() || right_input == dag.nodes.end() ||
          join_logical->input_logical_node_ids != asof_join->input_node_ids ||
          asof_join->input_node_ids.size() != 2 ||
          asof_join->output_descriptor_ids.size() !=
              left_input->output_descriptor_ids.size() +
                  right_input->output_descriptor_ids.size()) {
        return refuse("SBLR.PLAN_TREE.INVALID_HANDLE",
                      "time-series ASOF JOIN logical identity is incomplete");
      }
      composition_asof_left_outer =
          asof_join->semantic_variant_id == "join.asof.left.v1";
      if (!composition_asof_left_outer &&
          asof_join->semantic_variant_id != "join.asof.inner.v1") {
        return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                      "time-series ASOF JOIN disposition is not canonical");
      }
      if (bucket_operation && !downsample_operation) {
        return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                      "bucket-only time-series output is not an ASOF input");
      }
      const auto bind_asof_input =
          [&](const api::RelationalDagNode& input_node,
              const bool time_series_input,
              exec::CanonicalTimeSeriesAsofInputBindingV1* binding) {
            if (binding == nullptr) return false;
            std::vector<const api::RelationalOutputRecord*> input_outputs;
            for (const auto& output : dag.outputs) {
              if (output.relation_node_id == input_node.node_id) {
                input_outputs.push_back(&output);
              }
            }
            std::ranges::sort(input_outputs, {},
                              &api::RelationalOutputRecord::ordinal);
            if (input_outputs.size() !=
                input_node.output_descriptor_ids.size()) {
              return false;
            }
            for (std::size_t ordinal = 0; ordinal < input_outputs.size();
                 ++ordinal) {
              const auto* output = input_outputs[ordinal];
              const auto expression = expression_for(output->expression_id);
              const auto descriptor = descriptor_for(output->descriptor_id);
              if (output->ordinal != ordinal ||
                  output->descriptor_id !=
                      input_node.output_descriptor_ids[ordinal] ||
                  expression == dag.expressions.end() ||
                  descriptor == dag.descriptors.end() ||
                  expression->expression_kind !=
                      api::RelationalExpressionKind::kIdentifier ||
                  expression->result_descriptor_id != output->descriptor_id ||
                  std::ranges::find(input_node.bound_expression_ids,
                                    output->expression_id) ==
                      input_node.bound_expression_ids.end()) {
                return false;
              }
            }
            const auto unique_named = [&](const std::string_view name) {
              const api::RelationalOutputRecord* found = nullptr;
              for (const auto* output : input_outputs) {
                if (output->output_name_utf8 != name) continue;
                if (found != nullptr) return static_cast<
                    const api::RelationalOutputRecord*>(nullptr);
                found = output;
              }
              return found;
            };
            const auto* metric = unique_named("metric_uuid");
            const auto* tags = unique_named("tags");
            const auto* timestamp = unique_named(
                time_series_input
                    ? (downsample_operation ? "bucket_start"
                                            : "point_timestamp")
                    : "event_timestamp");
            if (metric == nullptr || tags == nullptr || timestamp == nullptr) {
              return false;
            }
            binding->metric_expression_id = metric->expression_id;
            binding->tags_expression_id = tags->expression_id;
            binding->timestamp_expression_id = timestamp->expression_id;
            binding->metric_descriptor_id = metric->descriptor_id;
            binding->tags_descriptor_id = tags->descriptor_id;
            binding->timestamp_descriptor_id = timestamp->descriptor_id;
            binding->metric_column_ordinal = metric->ordinal;
            binding->tags_column_ordinal = tags->ordinal;
            binding->timestamp_column_ordinal = timestamp->ordinal;
            binding->raw_time_series =
                time_series_input && !downsample_operation;
            binding->downsample_time_series =
                time_series_input && downsample_operation;
            if (binding->raw_time_series) {
              const auto* row_uuid = unique_named("row_uuid");
              if (row_uuid == nullptr) return false;
              binding->row_uuid_expression_id = row_uuid->expression_id;
              binding->row_uuid_descriptor_id = row_uuid->descriptor_id;
              binding->row_uuid_column_ordinal = row_uuid->ordinal;
            }
            return true;
          };
      if (!bind_asof_input(*left_input,
                           left_input->node_id == source->node_id,
                           &composition_asof_left_binding) ||
          !bind_asof_input(*right_input,
                           right_input->node_id == source->node_id,
                           &composition_asof_right_binding)) {
        return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                      "time-series ASOF key binding is incomplete");
      }
      if (asof_join->bound_expression_ids.size() != 7) {
        return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                      "time-series ASOF bound key/tolerance arity is invalid");
      }
      const std::array<std::uint32_t, 6> exact_key_expressions{
          composition_asof_left_binding.metric_expression_id,
          composition_asof_left_binding.tags_expression_id,
          composition_asof_left_binding.timestamp_expression_id,
          composition_asof_right_binding.metric_expression_id,
          composition_asof_right_binding.tags_expression_id,
          composition_asof_right_binding.timestamp_expression_id};
      if (!std::equal(exact_key_expressions.begin(),
                      exact_key_expressions.end(),
                      asof_join->bound_expression_ids.begin())) {
        return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                      "time-series ASOF key receipt was substituted");
      }
      CanonicalRelationalExpressionRuntime tolerance_runtime(
          dag, composition_planning_request->expression_services);
      std::uint64_t tolerance = 0;
      std::string tolerance_detail;
      if (!EvaluateNonNegativeRowBoundForComposition(
              &tolerance_runtime, asof_join->bound_expression_ids.back(),
              &tolerance, &tolerance_detail) ||
          tolerance >
              static_cast<std::uint64_t>(
                  std::numeric_limits<std::int64_t>::max())) {
        return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                      tolerance_detail.empty()
                          ? "time-series ASOF tolerance is outside exact int64"
                          : tolerance_detail);
      }
      composition_asof_tolerance_ns =
          static_cast<std::int64_t>(tolerance);
      const auto optimizer_row_bound = std::min<std::uint64_t>(
          input.context.optimizer_maximum_candidate_count,
          std::numeric_limits<std::size_t>::max());
      const auto left_bound = left_input->node_id == source->node_id
                                  ? source_input.maximum_rows
                                  : optimizer_row_bound;
      const auto right_bound = right_input->node_id == source->node_id
                                   ? source_input.maximum_rows
                                   : optimizer_row_bound;
      std::uint64_t pair_work = 0;
      std::uint64_t uniqueness_work = 0;
      if (left_bound == 0 || right_bound == 0 ||
          !CheckedMultiply(left_bound, right_bound, &pair_work) ||
          (right_bound > 1 &&
           (!CheckedMultiply(right_bound, right_bound - 1,
                             &uniqueness_work))) ||
          (right_bound > 1 && (uniqueness_work /= 2,
                               pair_work >
                                   std::numeric_limits<std::uint64_t>::max() -
                                       uniqueness_work))) {
        return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                      "time-series ASOF work receipt overflowed");
      }
      composition_asof_maximum_comparisons = pair_work + uniqueness_work;
      if (left_bound > std::numeric_limits<std::size_t>::max()) {
        return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                      "time-series ASOF output bound overflowed");
      }
      composition_asof_maximum_output_rows =
          static_cast<std::size_t>(left_bound);
      composition_relational_scan_capability_uuid = DerivedCanonicalUuid(
          identity_scope, "time-series.asof-join.heap-scan.capability");
      composition_asof_join_capability_uuid = DerivedCanonicalUuid(
          identity_scope, "time-series.asof-join.capability");
      composition_asof_join_implementation_id =
          composition_asof_left_outer ? "join.asof.left.typed.v1"
                                      : "join.asof.inner.typed.v1";

      LivePhysicalNodeProfile heap_profile;
      heap_profile.logical_node_id = relational_scan->node_id;
      heap_profile.implementation_id = "scan.heap.v1";
      heap_profile.capability_uuid =
          composition_relational_scan_capability_uuid;
      heap_profile.logical_node_kind =
          plan::CanonicalLogicalRelationalNodeKind::kRelationSource;
      heap_profile.physical_node_kind = exec::PhysicalNodeKind::kScan;
      heap_profile.transformation_rule_id =
          "canonical.time-series.asof-join.heap-scan.v1";
      heap_profile.estimated_rows = 1;
      heap_profile.memory_bytes_required =
          input.context.optimizer_memory_budget_bytes;
      heap_profile.page_read_sequential_units = 1;
      heap_profile.mga_visibility_checks_expected = 1;
      heap_profile.storage_read_capable = true;
      heap_profile.mga_visibility_capable = true;
      profiles.push_back(std::move(heap_profile));

      exec::CanonicalTimeSeriesAsofJoinRequestV1 receipt;
      receipt.left_binding = composition_asof_left_binding;
      receipt.right_binding = composition_asof_right_binding;
      receipt.tolerance_ns = composition_asof_tolerance_ns;
      receipt.left_outer = composition_asof_left_outer;
      receipt.maximum_output_rows = composition_asof_maximum_output_rows;
      receipt.maximum_comparisons =
          composition_asof_maximum_comparisons;
      LivePhysicalNodeProfile join_profile;
      join_profile.logical_node_id = asof_join->node_id;
      join_profile.implementation_id =
          composition_asof_join_implementation_id;
      join_profile.capability_uuid =
          composition_asof_join_capability_uuid;
      join_profile.logical_node_kind =
          plan::CanonicalLogicalRelationalNodeKind::kJoin;
      join_profile.physical_node_kind = exec::PhysicalNodeKind::kJoin;
      composition_asof_transformation_receipt =
          exec::CanonicalTimeSeriesAsofTransformationReceiptV1(receipt);
      join_profile.transformation_rule_id =
          composition_asof_transformation_receipt;
      join_profile.estimated_rows = composition_asof_maximum_output_rows;
      join_profile.memory_bytes_required = planning.memory_budget_bytes;
      join_profile.minimum_input_count = 2;
      join_profile.maximum_input_count = 2;
      join_profile.runtime_peak_from_callback_batches = true;
      profiles.push_back(std::move(join_profile));

      composition_state.batch.columns.clear();
      composition_state.result_bindings.clear();
      std::vector<const api::RelationalOutputRecord*> join_outputs;
      for (const auto& output : dag.outputs) {
        if (output.relation_node_id == asof_join->node_id) {
          join_outputs.push_back(&output);
        }
      }
      std::ranges::sort(join_outputs, {},
                        &api::RelationalOutputRecord::ordinal);
      std::vector<std::uint32_t> concatenated_descriptors =
          left_input->output_descriptor_ids;
      concatenated_descriptors.insert(concatenated_descriptors.end(),
                                      right_input->output_descriptor_ids.begin(),
                                      right_input->output_descriptor_ids.end());
      if (join_outputs.size() != concatenated_descriptors.size() ||
          asof_join->output_descriptor_ids != concatenated_descriptors) {
        return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                      "time-series ASOF output concatenation is incomplete");
      }
      const auto left_width = left_input->output_descriptor_ids.size();
      for (std::size_t ordinal = 0; ordinal < join_outputs.size(); ++ordinal) {
        const auto descriptor =
            descriptor_for(join_outputs[ordinal]->descriptor_id);
        if (descriptor == dag.descriptors.end() ||
            join_outputs[ordinal]->ordinal != ordinal ||
            join_outputs[ordinal]->descriptor_id !=
                concatenated_descriptors[ordinal]) {
          return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                        "time-series ASOF result descriptor is unresolved");
        }
        exec::CanonicalResultColumnBinding binding;
        binding.physical_column_ordinal = ordinal;
        binding.visible = join_outputs[ordinal]->visible;
        if (binding.visible) {
          const bool null_extended =
              composition_asof_left_outer && ordinal >= left_width;
          binding.published_descriptor =
              exec::CanonicalResultColumnDescriptor{
                  static_cast<std::uint32_t>(ordinal),
                  join_outputs[ordinal]->output_name_utf8,
                  descriptor->descriptor_uuid, descriptor->type_uuid,
                  null_extended
                      ? exec::CanonicalResultNullability::kNullable
                      : ResultNullability(descriptor->nullability),
                  descriptor->collation_uuid,
                  descriptor->timezone_profile_id};
        }
        composition_state.result_bindings.push_back(std::move(binding));
      }
    }
    if (!CompleteLiveRuntimeMemoryReceipts(&profiles)) {
      return refuse("QOW-DIAG-OPT-017-REFUSAL-V1",
                    "time-series runtime memory receipt is incomplete");
    }
    composition_physical = PlanAndPublishLivePhysicalDag(
        *composition_planning_request, profiles,
        "time-series-composition.selected-plan", "time-series composition",
        "time_series.local.v1");
    if (!composition_physical->ok) {
      return refuse(
          composition_physical->diagnostic_id.empty()
              ? "QOW-DIAG-OPTIMIZER-PHYSICAL-PUBLICATION-V1"
              : composition_physical->diagnostic_id,
          composition_physical->detail.empty()
              ? "time-series combined physical DAG was not published"
              : composition_physical->detail);
    }
    const auto physical_source = std::ranges::find_if(
        composition_physical->physical_dag.nodes, [&](const auto& node) {
          return node.relational_node_id == source->node_id;
        });
    if (composition_physical->physical_dag.nodes.size() != profiles.size() ||
        composition_physical->physical_dag.root_physical_node_id !=
            dag.root_node_id ||
        physical_source == composition_physical->physical_dag.nodes.end() ||
        physical_source->implementation_id !=
            "physical_time_series_range_scan_v1" ||
        physical_source->memory_bytes_required !=
            selected_source_memory_grant) {
      return refuse("QOW-DIAG-OPTIMIZER-PHYSICAL-PUBLICATION-V1",
                    "time-series combined physical root or source changed");
    }
    provider_request.selected_alternative_uuid =
        physical_source->selected_alternative_uuid;
    provider_request.capability_uuid =
        physical_source->executor_capability_uuid;
    source_input.physical_node_id = physical_source->physical_node_id;
    source_input.selected_alternative_uuid =
        physical_source->selected_alternative_uuid;
    source_input.capability_uuid =
        physical_source->executor_capability_uuid;
    source_input.causal_counter_id = physical_source->causal_counter_id;
    result.optimizer_selected = true;
    result.physical_dag_published = true;
    result.optimizer_admission_stage_count =
        canonical_admission.admission.evidence.size();
    result.physical_node_count =
        composition_physical->physical_dag.nodes.size();
    result.selected_plan_uuid =
        composition_physical->physical_dag.selected_plan_uuid;
  }

  exec::ModelFamilyExecutionRequestV1 execution_request;
  execution_request.input = source_input;
  execution_request.capability.capability_uuid = source_input.capability_uuid;
  execution_request.capability.family_id = "time_series";
  execution_request.capability.provider_uuid = source_input.provider_uuid;
  execution_request.capability.provider_generation =
      source_input.provider_generation;
  execution_request.capability.available = true;
  execution_request.capability.exact = true;
  execution_request.capability.exact_collection_fallback_available = true;
  execution_request.capability.cancellation_supported = true;
  execution_request.capability.cleanup_supported = true;
  execution_request.capability.residual_recheck_supported = true;
  execution_request.capability.base_row_mga_recheck_supported = true;
  execution_request.capability.security_recheck_supported = true;
  execution_request.exact_fallback_selected =
      source_input.exact_fallback_selected;
  execution_request.cancellation_requested =
      provider_request.cancellation_requested;
  execution_request.cleanup_provider = [] {};
  execution_request.security_admitted = planning.security_admitted;
  execution_request.current_catalog_generation = generation;
  execution_request.current_descriptor_generation =
      persisted.descriptor_generation;
  execution_request.current_security_generation = planning.security_epoch;
  execution_request.current_policy_generation = planning.policy_epoch;
  execution_request.current_resource_generation = planning.resource_epoch;
  execution_request.current_provider_generation =
      source_input.provider_generation;
  execution_request.current_mga_statement_context = mga;
  const auto property_uuid =
      DerivedCanonicalUuid(identity_scope, "time-series.property");
  const auto security_receipt_uuid =
      DerivedCanonicalUuid(identity_scope, "time-series.security-receipt");
  const auto time_series_runtime_memory_receipt =
      std::make_shared<Rcp079ColumnarSourceRuntimeMemoryReceiptV1>();
  execution_request.execute_provider =
      [provider_request, public_columns, property_uuid,
       security_receipt_uuid, aggregate, bucket_operation,
       bucket_interval_ns, time_series_cancellation_requested,
       time_series_cancellation_probe_failed,
       time_series_runtime_memory_receipt](
          const exec::ModelSourceInputDescriptorV1& runtime_input) mutable {
        exec::ModelProviderExecutionResultV1 provider;
        const auto fail = [&](std::string diagnostic, std::string detail) {
          provider.diagnostic_id = std::move(diagnostic);
          provider.detail = std::move(detail);
          return provider;
        };
        const auto cancellation_diagnostic = [&] {
          return time_series_cancellation_probe_failed->load(
                     std::memory_order_relaxed)
                     ? "SB_MODEL_COORDINATOR_LEG_FAILED_V1"
                     : "SB_MODEL_EXECUTION_CANCELLED_V1";
        };
        *time_series_runtime_memory_receipt = {};
        try {
        if (runtime_input.family_id != "time_series" ||
            runtime_input.object_uuid != provider_request.object_uuid ||
            runtime_input.descriptor_generation !=
                provider_request.expected_descriptor_generation ||
            runtime_input.maximum_rows == 0 ||
            runtime_input.maximum_cells == 0 || public_columns.empty() ||
            runtime_input.maximum_memory_bytes == 0 ||
            runtime_input.maximum_rows > provider_request.maximum_output_rows ||
            runtime_input.maximum_memory_bytes >
                provider_request.maximum_memory_bytes) {
          return fail("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                      "time-series runtime provider grant is invalid");
        }
        const auto runtime_row_bound = std::min<std::uint64_t>(
            runtime_input.maximum_rows,
            runtime_input.maximum_cells / public_columns.size());
        if (runtime_row_bound == 0) {
          return fail("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                      "time-series runtime provider cell grant is invalid");
        }
        auto runtime_provider_request = provider_request;
        runtime_provider_request.selected_alternative_uuid =
            runtime_input.selected_alternative_uuid;
        runtime_provider_request.capability_uuid =
            runtime_input.capability_uuid;
        runtime_provider_request.provider_uuid = runtime_input.provider_uuid;
        runtime_provider_request.provider_generation =
            runtime_input.provider_generation;
        runtime_provider_request.exact_fallback_selected =
            runtime_input.exact_fallback_selected;
        // The physical row/cell grant bounds rows published by this leg.  It
        // must not narrow the separately planned scan-version bound: MGA may
        // need to inspect historical, invisible, or tombstoned versions to
        // produce a much smaller visible result.
        runtime_provider_request.maximum_decoded_bytes =
            runtime_input.maximum_memory_bytes;
        runtime_provider_request.maximum_output_rows =
            runtime_row_bound;
        runtime_provider_request.maximum_groups = std::min(
            runtime_provider_request.maximum_groups,
            runtime_row_bound);
        runtime_provider_request.maximum_tag_bytes = std::min(
            runtime_provider_request.maximum_tag_bytes,
            runtime_input.maximum_memory_bytes);
        runtime_provider_request.maximum_result_bytes =
            runtime_input.maximum_memory_bytes;
        runtime_provider_request.maximum_memory_bytes =
            runtime_input.maximum_memory_bytes;
        if (time_series_cancellation_requested()) {
          return fail(cancellation_diagnostic(),
                      "time-series execution was cancelled before provider access");
        }
        const auto read =
            api::EngineBoundTimeSeriesReadV1(runtime_provider_request);
        provider.data_access_observed = read.data_access_observed;
        provider.rows_examined = read.scanned_row_version_count;
        if (time_series_cancellation_probe_failed->load(
                std::memory_order_relaxed) ||
            read.cancellation_probe_failed) {
          return fail("SB_MODEL_COORDINATOR_LEG_FAILED_V1",
                      "time-series cancellation probe failed");
        }
        if (!read.ok) {
          provider.diagnostic_id = read.diagnostics.empty()
                                       ? "SB_MODEL_COORDINATOR_LEG_FAILED_V1"
                                       : read.diagnostics.front().code;
          provider.detail = read.diagnostics.empty()
                                ? "engine-bound time-series read failed"
                                : read.diagnostics.front().detail;
          return provider;
        }
        const bool exact_fallback = runtime_input.exact_fallback_selected;
        const auto expected_access_path =
            exact_fallback ? "TIME_SERIES_BUCKET_STORE_SCAN_EXACT_V1"
                           : "time_series.local.v1";
        const auto expected_ordering =
            runtime_input.operation_id == "TIME_SERIES_DOWNSAMPLE"
                ? "series_metric_tags_bucket_start_ascending_v1"
                : "series_metric_timestamp_tags_row_ascending_v1";
        const auto selected_output_count =
            read.rows.size() + read.downsample_rows.size();
        if (!read.data_access_observed || !read.diagnostics.empty() ||
            !read.unsupported_features.empty() ||
            read.cancellation_observed ||
            read.memory_grant_bytes != runtime_input.maximum_memory_bytes ||
            !read.memory_receipt_complete ||
            read.current_live_memory_bytes > read.peak_live_memory_bytes ||
            read.peak_live_memory_bytes > read.memory_grant_bytes ||
            read.selected_access_path_id != expected_access_path ||
            read.preferred_access_invocation_count !=
                static_cast<std::uint64_t>(!exact_fallback) ||
            read.exact_fallback_access_invocation_count !=
                static_cast<std::uint64_t>(exact_fallback) ||
            read.exact_fallback_observed != exact_fallback ||
            read.rollup_observed ||
            !read.rollup_equivalence_recheck_complete ||
            !read.residual_recheck_complete ||
            !read.base_row_mga_recheck_complete ||
            !read.security_recheck_complete ||
            read.scanned_row_version_count >
                runtime_provider_request.maximum_scanned_row_versions ||
            (runtime_input.operation_id != "TIME_SERIES_DOWNSAMPLE" &&
             read.selected_visible_row_count >
                 runtime_provider_request.maximum_output_rows) ||
            selected_output_count >
                runtime_provider_request.maximum_output_rows ||
            read.result_byte_count >
                runtime_provider_request.maximum_result_bytes ||
            read.ordering_id != expected_ordering ||
            read.descriptor_uuid !=
                runtime_provider_request.expected_descriptor_uuid ||
            read.selected_alternative_uuid !=
                runtime_input.selected_alternative_uuid ||
            read.capability_uuid != runtime_input.capability_uuid ||
            read.provider_uuid != runtime_input.provider_uuid ||
            read.provider_generation != runtime_input.provider_generation ||
            read.descriptor_generation != runtime_input.descriptor_generation ||
            read.exact_fallback_observed !=
                runtime_input.exact_fallback_selected ||
            (runtime_input.operation_id == "TIME_SERIES_DOWNSAMPLE"
                 ? (!read.rows.empty() ||
                    (read.selected_visible_row_count != 0 &&
                     read.downsample_rows.empty()) ||
                    (!read.downsample_rows.empty() &&
                     read.downsample_rows.size() >
                         read.selected_visible_row_count))
                 : (!read.downsample_rows.empty() ||
                    read.rows.size() !=
                        read.selected_visible_row_count))) {
          return fail("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                      "time-series provider execution receipt changed");
        }
        const auto real_text = [](const double value) {
          if (value == 0.0) return std::string("0");
          std::array<char, 64> encoded{};
          const auto converted = std::to_chars(
              encoded.data(), encoded.data() + encoded.size(), value,
              std::chars_format::general,
              std::numeric_limits<double>::max_digits10);
          return converted.ec == std::errc{}
                     ? std::string(encoded.data(), converted.ptr)
                     : std::string{};
        };
        std::uint64_t bridge_bytes = 0;
        const auto account_bridge = [&](const std::uint64_t bytes) {
          return bytes <= std::numeric_limits<std::uint64_t>::max() -
                              bridge_bytes &&
                 (bridge_bytes += bytes) <=
                     runtime_input.maximum_memory_bytes;
        };
        const auto account_string = [&](const std::string_view value) {
          return value.size() != std::numeric_limits<std::size_t>::max() &&
                 account_bridge(static_cast<std::uint64_t>(value.size()) + 1);
        };
        const auto account_descriptor =
            [&](const api::EngineDescriptor& descriptor) {
              return account_string(descriptor.descriptor_uuid.canonical) &&
                     account_string(descriptor.descriptor_kind) &&
                     account_string(descriptor.canonical_type_name) &&
                     account_string(descriptor.encoded_descriptor);
            };
        const bool bridge_initial_ok =
            account_bridge(read.current_live_memory_bytes) &&
            account_bridge(sizeof(exec::ModelProviderBatchV1));
        const std::size_t provider_row_count =
            read.rows.size() + read.downsample_rows.size();
        std::uint64_t vector_bytes = 0;
        bool bridge_preflight_ok =
            bridge_initial_ok &&
            CheckedMultiply(public_columns.size(),
                            sizeof(exec::ExecutorColumnDescriptor),
                            &vector_bytes) &&
            account_bridge(vector_bytes) &&
            CheckedMultiply(provider_row_count, sizeof(exec::DescriptorTuple),
                            &vector_bytes) &&
            account_bridge(vector_bytes) &&
            CheckedMultiply(provider_row_count,
                            sizeof(exec::ModelProviderRowIdentityV1),
                            &vector_bytes) &&
            account_bridge(vector_bytes) &&
            CheckedMultiply(provider_row_count, public_columns.size(),
                            &vector_bytes) &&
            CheckedMultiply(vector_bytes,
                            sizeof(api::EngineTypedValue), &vector_bytes) &&
            account_bridge(vector_bytes);
        for (const auto& column : public_columns) {
          if (time_series_cancellation_requested()) {
            return fail(cancellation_diagnostic(),
                        "time-series provider descriptor preflight was cancelled");
          }
          bridge_preflight_ok =
              bridge_preflight_ok && account_string(column.stable_name) &&
              account_descriptor(column.descriptor);
        }
        for (const auto& row : read.rows) {
          if (time_series_cancellation_requested()) {
            return fail(cancellation_diagnostic(),
                        "time-series provider raw-row preflight was cancelled");
          }
          bridge_preflight_ok =
              bridge_preflight_ok && account_string(row.row_uuid) &&
              account_string(row.series_uuid) &&
              account_string(row.metric_uuid) &&
              account_string(row.point_timestamp) &&
              account_string(row.tags) && account_bridge(65) &&
              account_string(row.row_uuid) && account_string(row.series_uuid) &&
              account_string(row.metric_uuid) && account_string(row.tags) &&
              account_bridge(1 + 64);
          for (const auto& column : public_columns) {
            bridge_preflight_ok =
                bridge_preflight_ok && account_descriptor(column.descriptor);
          }
        }
        for (const auto& row : read.downsample_rows) {
          if (time_series_cancellation_requested()) {
            return fail(
                cancellation_diagnostic(),
                "time-series provider aggregate-row preflight was cancelled");
          }
          bridge_preflight_ok =
              bridge_preflight_ok && account_string(row.series_uuid) &&
              account_string(row.metric_uuid) &&
              account_string(row.bucket_start) &&
              account_string(row.bucket_end) && account_string(row.tags) &&
              account_bridge(21 + 65) && account_string(row.series_uuid) &&
              account_string(row.metric_uuid) && account_string(row.tags) &&
              account_bridge(32 + 21 + 65);
          for (const auto& column : public_columns) {
            bridge_preflight_ok =
                bridge_preflight_ok && account_descriptor(column.descriptor);
          }
        }
        bridge_preflight_ok =
            bridge_preflight_ok && account_string(runtime_input.provider_uuid) &&
            account_string(runtime_input.selected_alternative_uuid) &&
            account_string(runtime_input.capability_uuid) &&
            account_string(runtime_input.result_handle_uuid) &&
            account_string(property_uuid) &&
            account_string(security_receipt_uuid);
        if (!bridge_preflight_ok) {
          provider.diagnostic_id = "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1";
          provider.detail =
              "time-series provider batch construction exceeded its memory bound";
          return provider;
        }
        auto& batch = provider.provider_batch;
        batch.provider_uuid = runtime_input.provider_uuid;
        batch.provider_generation = runtime_input.provider_generation;
        batch.selected_alternative_uuid =
            runtime_input.selected_alternative_uuid;
        batch.capability_uuid = runtime_input.capability_uuid;
        batch.exact_fallback_selected =
            runtime_input.exact_fallback_selected;
        batch.result_handle_uuid = runtime_input.result_handle_uuid;
        batch.causal_counter_id = runtime_input.causal_counter_id;
        batch.output_descriptor_ids = runtime_input.output_descriptor_ids;
        batch.batch.columns.reserve(public_columns.size());
        batch.batch.columns.insert(batch.batch.columns.end(),
                                   public_columns.begin(),
                                   public_columns.end());
        batch.batch.rows.reserve(provider_row_count);
        batch.ordered_row_identities.reserve(provider_row_count);
        batch.mga_statement_context = runtime_input.mga_statement_context;
        batch.security_receipt_uuid = security_receipt_uuid;
        batch.properties.property_uuid = property_uuid;
        batch.properties.ordering_id = read.ordering_id;
        batch.properties.partitioning_id = "single_local_partition";
        batch.properties.uniqueness_id =
            runtime_input.operation_id != "TIME_SERIES_DOWNSAMPLE"
                ? "row_uuid"
                : "series_metric_tags_bucket_v1";
        batch.properties.exact = true;
        batch.properties.residual_recheck_complete =
            read.residual_recheck_complete;
        batch.properties.base_row_mga_recheck_complete =
            read.base_row_mga_recheck_complete;
        batch.properties.security_recheck_complete =
            read.security_recheck_complete;
        batch.residual_recheck_complete = read.residual_recheck_complete;
        batch.base_row_mga_recheck_complete =
            read.base_row_mga_recheck_complete;
        batch.security_recheck_complete = read.security_recheck_complete;
        const auto append_tuple = [&](const auto& encoded) {
          exec::DescriptorTuple tuple;
          tuple.values.reserve(encoded.size());
          for (std::size_t ordinal = 0; ordinal < encoded.size(); ++ordinal) {
            api::EngineTypedValue value;
            value.descriptor = public_columns[ordinal].descriptor;
            value.encoded_value = encoded[ordinal];
            value.setState(api::EngineValueState::value);
            tuple.values.push_back(std::move(value));
          }
          batch.batch.rows.push_back(std::move(tuple));
        };
        for (const auto& row : read.rows) {
          if (time_series_cancellation_requested()) {
            return fail(cancellation_diagnostic(),
                        "time-series provider raw-row publication was cancelled");
          }
          api::EngineApiI64 projected_bucket_start_ns = 0;
          std::string raw_payload;
          if (bucket_operation) {
            std::string bucket_start;
            if (!api::EngineExactTimeSeriesBucketStartV1(
                    row.point_timestamp_ns, bucket_interval_ns,
                    &projected_bucket_start_ns, &bucket_start)) {
              provider.diagnostic_id =
                  "SB_MODEL_TIME_SERIES_INTERVAL_INVALID_V1";
              provider.detail =
                  "TIME_BUCKET exact mathematical floor projection failed";
              return provider;
            }
            append_tuple(
                std::array<std::string_view, 1>{bucket_start});
          } else {
            raw_payload = real_text(row.value);
            if (raw_payload.empty()) {
              provider.diagnostic_id =
                  "SB_MODEL_TIME_SERIES_VALUE_INVALID_V1";
              provider.detail =
                  "time-series raw REAL64 publication encoding failed";
              return provider;
            }
            append_tuple(std::array<std::string_view, 6>{
                row.row_uuid, row.series_uuid, row.metric_uuid,
                row.point_timestamp, row.tags, raw_payload});
          }
          exec::ModelProviderRowIdentityV1 identity;
          identity.row_uuid = row.row_uuid;
          identity.series_uuid = row.series_uuid;
          identity.metric_uuid = row.metric_uuid;
          identity.tags = row.tags;
          identity.point_timestamp_ns = row.point_timestamp_ns;
          identity.bucket_start_ns = projected_bucket_start_ns;
          if (!bucket_operation) {
            identity.time_series_payload_kind = "raw.real64.v1";
            identity.time_series_raw_value = std::move(raw_payload);
          }
          batch.ordered_row_identities.push_back(std::move(identity));
        }
        for (const auto& row : read.downsample_rows) {
          if (time_series_cancellation_requested()) {
            return fail(
                cancellation_diagnostic(),
                "time-series provider aggregate publication was cancelled");
          }
          const auto sample_count = std::to_string(row.sample_count);
          const auto aggregate_value =
              aggregate == api::EngineBoundTimeSeriesAggregateV1::kCount
                  ? std::to_string(row.aggregate_count)
                  : real_text(row.aggregate_value);
          if (aggregate_value.empty()) {
            provider.diagnostic_id =
                "SB_MODEL_TIME_SERIES_VALUE_INVALID_V1";
            provider.detail =
                "time-series aggregate publication encoding failed";
            return provider;
          }
          append_tuple(std::array<std::string_view, 7>{
              row.series_uuid, row.metric_uuid, row.bucket_start,
              row.bucket_end, row.tags, sample_count, aggregate_value});
          exec::ModelProviderRowIdentityV1 identity;
          identity.series_uuid = row.series_uuid;
          identity.metric_uuid = row.metric_uuid;
          identity.tags = row.tags;
          identity.bucket_start_ns = row.bucket_start_ns;
          switch (aggregate) {
            case api::EngineBoundTimeSeriesAggregateV1::kCount:
              identity.time_series_payload_kind =
                  "downsample.count.int64.v1";
              break;
            case api::EngineBoundTimeSeriesAggregateV1::kSum:
              identity.time_series_payload_kind =
                  "downsample.sum.real64.v1";
              break;
            case api::EngineBoundTimeSeriesAggregateV1::kMin:
              identity.time_series_payload_kind =
                  "downsample.min.real64.v1";
              break;
            case api::EngineBoundTimeSeriesAggregateV1::kMax:
              identity.time_series_payload_kind =
                  "downsample.max.real64.v1";
              break;
            case api::EngineBoundTimeSeriesAggregateV1::kAvg:
              identity.time_series_payload_kind =
                  "downsample.avg.real64.v1";
              break;
            case api::EngineBoundTimeSeriesAggregateV1::kNone:
              break;
          }
          identity.time_series_sample_count = sample_count;
          identity.time_series_aggregate_value = aggregate_value;
          batch.ordered_row_identities.push_back(std::move(identity));
        }
        if (time_series_cancellation_requested()) {
          return fail(cancellation_diagnostic(),
                      "time-series provider publication was cancelled");
        }
        const auto provider_memory =
            Rcp079ModelProviderBatchLogicalMemoryBytesV1(batch);
        const auto output_memory =
            Rcp079ProjectedModelSourceOutputLogicalMemoryBytesV1(
                runtime_input, batch);
        std::uint64_t uniqueness_memory =
            3 * sizeof(std::unordered_set<std::string>);
        std::uint64_t maximum_tag_temporary = 0;
        constexpr std::uint64_t kSetNodeOverhead =
            sizeof(std::string) + 4 * sizeof(void*) + 64;
        bool exchange_memory_complete = true;
        for (const auto& identity : batch.ordered_row_identities) {
          if (time_series_cancellation_requested()) {
            return fail(
                cancellation_diagnostic(),
                "time-series exchange memory receipt was cancelled");
          }
          if (runtime_input.operation_id != "TIME_SERIES_DOWNSAMPLE") {
            std::uint64_t node_memory = kSetNodeOverhead;
            exchange_memory_complete =
                exchange_memory_complete &&
                CheckedAdd(node_memory, identity.row_uuid.size(),
                           &node_memory) &&
                CheckedAdd(uniqueness_memory, node_memory,
                           &uniqueness_memory);
          }
          std::uint64_t tag_temporary = 3 * sizeof(std::string);
          std::uint64_t repeated_tag_bytes = 0;
          exchange_memory_complete =
              exchange_memory_complete &&
              CheckedAdd(static_cast<std::uint64_t>(identity.tags.size()), 1,
                         &repeated_tag_bytes) &&
              CheckedMultiply(repeated_tag_bytes, std::uint64_t{3},
                              &repeated_tag_bytes) &&
              CheckedAdd(tag_temporary, repeated_tag_bytes,
                         &tag_temporary);
          maximum_tag_temporary =
              std::max(maximum_tag_temporary, tag_temporary);
        }
        std::uint64_t provider_construction_peak = 0;
        std::uint64_t exchange_validation_peak = 0;
        std::uint64_t exchange_output_peak = 0;
        if (!provider_memory.has_value() || !output_memory.has_value() ||
            !exchange_memory_complete ||
            !CheckedAdd(read.current_live_memory_bytes, *provider_memory,
                        &provider_construction_peak) ||
            !CheckedAdd(*provider_memory, uniqueness_memory,
                        &exchange_validation_peak) ||
            !CheckedAdd(exchange_validation_peak, maximum_tag_temporary,
                        &exchange_validation_peak) ||
            !CheckedAdd(*provider_memory, *output_memory,
                        &exchange_output_peak)) {
          return fail("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                      "time-series live-memory receipt overflowed");
        }
        time_series_runtime_memory_receipt->provider_logical_memory_bytes =
            *provider_memory;
        time_series_runtime_memory_receipt->peak_live_memory_bytes =
            std::max({read.peak_live_memory_bytes,
                      provider_construction_peak,
                      exchange_validation_peak, exchange_output_peak});
        time_series_runtime_memory_receipt->memory_grant_bytes =
            runtime_input.maximum_memory_bytes;
        time_series_runtime_memory_receipt->complete =
            time_series_runtime_memory_receipt->peak_live_memory_bytes <=
            time_series_runtime_memory_receipt->memory_grant_bytes;
        if (!time_series_runtime_memory_receipt->complete) {
          return fail(
              "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
              "time-series provider/exchange peak exceeded the selected grant");
        }
        provider.ok = true;
        return provider;
        } catch (const std::bad_alloc&) {
          return fail("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                      "time-series provider allocation was refused");
        } catch (const std::length_error&) {
          return fail("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                      "time-series provider allocation length was refused");
        } catch (const std::exception& exception) {
          return fail("SB_MODEL_COORDINATOR_LEG_FAILED_V1",
                      std::string("time-series provider threw: ") +
                          exception.what());
        } catch (...) {
          return fail("SB_MODEL_COORDINATOR_LEG_FAILED_V1",
                      "time-series provider threw a non-standard exception");
        }
      };
  CaptureRcp079ModelLegV1(
      leg_capture, source->node_id, "time_series",
      "physical_time_series_range_scan_v1",
      "canonical.time-series.scan.v1", "time_series.local.v1",
      persisted.descriptor_uuid.canonical, persisted.descriptor_generation,
      downsample_operation
          ? plan::CanonicalLogicalRelationalNodeKind::kAggregate
          : plan::CanonicalLogicalRelationalNodeKind::kRelationSource,
      downsample_operation ? exec::PhysicalNodeKind::kAggregate
                           : exec::PhysicalNodeKind::kScan,
      execution_request);
  if (leg_capture != nullptr) {
    leg_capture->exact_output_columns = public_columns;
    leg_capture->columnar_runtime_memory_receipt =
        time_series_runtime_memory_receipt;
    leg_capture->cancellation_probe_failed =
        time_series_cancellation_probe_failed;
  }
  if (leg_capture != nullptr) return result;
  {
    struct TimeSeriesSourceExecutionReceipt {
      bool execution_started{false};
      bool data_access_observed{false};
      bool cleanup_complete{false};
      std::uint32_t cleanup_count{0};
    };
    const auto source_execution_receipt =
        std::make_shared<TimeSeriesSourceExecutionReceipt>();
    exec::CanonicalPhysicalExecutorRegistration source_registration;
    source_registration.node_kind =
        downsample_operation ? exec::PhysicalNodeKind::kAggregate
                             : exec::PhysicalNodeKind::kScan;
    source_registration.implementation_id =
        "physical_time_series_range_scan_v1";
    source_registration.executor_capability_uuid =
        source_input.capability_uuid;
    source_registration.executor_capability_abi_version = 1;
    source_registration.engine_owned = true;
    source_registration.accepts_optimizer_publication_v2 = true;
    source_registration.publishes_runtime_observation_v1 = true;
    source_registration.execute =
        [execution_request, source_execution_receipt,
         persisted_descriptor_uuid = persisted.descriptor_uuid.canonical,
         public_columns, property_uuid, security_receipt_uuid,
         time_series_runtime_memory_receipt,
         time_series_cancellation_probe_failed,
         standalone_time_series_source,
         maximum_scanned_row_versions =
             provider_request.maximum_scanned_row_versions](
            const exec::TypedPhysicalNodeDag& selected_dag,
            const exec::PhysicalNodeRecord& selected_node,
            const std::vector<exec::CanonicalPhysicalDispatchInput>& inputs) {
          exec::CanonicalPhysicalDispatchStepResult step;
          step.selected_plan_uuid = selected_dag.selected_plan_uuid;
          step.executed_physical_node_id = selected_node.physical_node_id;
          step.causal_counter_id = selected_node.causal_counter_id;
          step.output_descriptor_ids = selected_node.output_descriptor_ids;
          step.mga_statement_context = selected_dag.mga_statement_context;
          step.authority.engine_mga_snapshot_bound = true;
          step.data_access_observation_known = true;
          try {
          if (!inputs.empty() || selected_dag.abi_version != 2 ||
              selected_node.memory_bytes_required == 0 ||
              selected_node.memory_bytes_required >
                  selected_dag.memory_budget_bytes ||
              selected_node.memory_bytes_required !=
                  execution_request.input.maximum_memory_bytes ||
              selected_node.implementation_id !=
                  "physical_time_series_range_scan_v1" ||
              selected_node.selected_alternative_uuid !=
                  execution_request.input.selected_alternative_uuid ||
              selected_node.executor_capability_uuid !=
                  execution_request.input.capability_uuid ||
              selected_node.physical_node_id !=
                  execution_request.input.physical_node_id ||
              selected_node.causal_counter_id !=
                  execution_request.input.causal_counter_id ||
              selected_node.output_descriptor_ids !=
                  execution_request.input.output_descriptor_ids ||
              !exec::PhysicalMgaStatementContextEqual(
                  selected_dag.mga_statement_context,
                  execution_request.input.mga_statement_context)) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "SB_MODEL_TYPED_EXCHANGE_INVALID_V1";
            step.diagnostic.detail =
                "selected time-series physical source identity changed";
            return step;
          }
          auto executed =
              exec::ExecuteModelFamilySourceV1(execution_request);
          source_execution_receipt->execution_started =
              executed.execution_started;
          source_execution_receipt->data_access_observed =
              executed.data_access_observed;
          source_execution_receipt->cleanup_complete =
              executed.cleanup_complete;
          source_execution_receipt->cleanup_count = executed.cleanup_count;
          step.data_access_observed = executed.data_access_observed;
          if (time_series_cancellation_probe_failed->load(
                  std::memory_order_relaxed)) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "SB_MODEL_COORDINATOR_LEG_FAILED_V1";
            step.diagnostic.detail =
                "time-series cancellation probe failed";
            return step;
          }
          if (executed.diagnostic_id == "SB_MODEL_EXECUTION_CANCELLED_V1") {
            const exec::PhysicalAdmissionEvidence* cancellation_policy =
                nullptr;
            for (const auto& evidence : selected_dag.admission_evidence) {
              if (evidence.stage !=
                  exec::PhysicalAdmissionStage::kPolicyCapability) {
                continue;
              }
              if (cancellation_policy != nullptr) {
                cancellation_policy = nullptr;
                break;
              }
              cancellation_policy = &evidence;
            }
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code = executed.diagnostic_id;
            step.diagnostic.detail = executed.detail;
            step.cancellation_observed = true;
            step.transient_state_cleanup_proven =
                executed.cleanup_complete && executed.cleanup_count == 1;
            step.cancellation_evidence_uuid =
                cancellation_policy == nullptr
                    ? std::string{}
                    : cancellation_policy->evidence_uuid;
            return step;
          }
          const auto& output = executed.output;
          const auto& expected = execution_request.input;
          const auto expected_ordering =
              expected.operation_id == "TIME_SERIES_DOWNSAMPLE"
                  ? "series_metric_tags_bucket_start_ascending_v1"
                  : "series_metric_timestamp_tags_row_ascending_v1";
          const auto expected_uniqueness =
              expected.operation_id == "TIME_SERIES_DOWNSAMPLE"
                  ? "series_metric_tags_bucket_v1"
                  : "row_uuid";
          const bool execution_completed =
              executed.accepted && executed.execution_started &&
              executed.provider_entered && executed.data_access_observed &&
              executed.root_published && executed.cleanup_complete &&
              executed.cleanup_count == 1 &&
              executed.rows_examined <= maximum_scanned_row_versions;
          const bool exact_output_received =
              output.abi_version == 1 &&
              output.output_descriptor_id ==
                  "SB_MODEL_SOURCE_OUTPUT_DESCRIPTOR_V1" &&
              output.exact_exchange_validated &&
              output.family_id == expected.family_id &&
              output.operation_ids == expected.operation_ids &&
              output.operation_id == expected.operation_id &&
              output.object_uuid == expected.object_uuid &&
              output.physical_node_id == selected_node.physical_node_id &&
              output.selected_alternative_uuid ==
                  selected_node.selected_alternative_uuid &&
              output.capability_uuid ==
                  selected_node.executor_capability_uuid &&
              output.provider_uuid == expected.provider_uuid &&
              output.provider_generation == expected.provider_generation &&
              output.result_handle_uuid == expected.result_handle_uuid &&
              output.causal_counter_id == selected_node.causal_counter_id &&
              output.output_descriptor_ids ==
                  selected_node.output_descriptor_ids &&
              output.exact_fallback_selected ==
                  expected.exact_fallback_selected &&
              exec::PhysicalMgaStatementContextEqual(
                  output.mga_statement_context,
                  expected.mga_statement_context) &&
              output.security_receipt_uuid == security_receipt_uuid &&
              output.multimodel_common_statement_context ==
                  expected.multimodel_common_statement_context &&
              output.multimodel_composition_receipt_uuid ==
                  expected.multimodel_composition_receipt_uuid &&
              output.multimodel_lexical_source_ordinal ==
                  expected.multimodel_lexical_source_ordinal &&
              output.multimodel_composition_arity ==
                  expected.multimodel_composition_arity &&
              output.properties.abi_version == 1 &&
              output.properties.property_descriptor_id ==
                  "SB_MODEL_PROPERTY_DESCRIPTOR_V1" &&
              output.properties.property_uuid == property_uuid &&
              output.properties.ordering_id == expected_ordering &&
              output.properties.partitioning_id ==
                  "single_local_partition" &&
              output.properties.uniqueness_id == expected_uniqueness &&
              output.properties.exact &&
              output.properties.residual_recheck_complete &&
              output.properties.base_row_mga_recheck_complete &&
              output.properties.security_recheck_complete &&
              output.ordered_row_identities.size() ==
                  output.batch.rows.size() &&
              output.batch.rows.size() <= expected.maximum_rows &&
              output.batch.columns.size() == public_columns.size() &&
              std::ranges::equal(
                  output.batch.columns, public_columns,
                  [](const auto& actual, const auto& expected_column) {
                    return Rcp079ExactExecutorColumnV1(actual,
                                                       expected_column);
                  });
          if (!execution_completed || !exact_output_received) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                execution_completed
                    ? "SB_MODEL_TYPED_EXCHANGE_INVALID_V1"
                    : executed.diagnostic_id.empty()
                    ? "SB_MODEL_COORDINATOR_LEG_FAILED_V1"
                    : executed.diagnostic_id;
            step.diagnostic.detail =
                execution_completed
                    ? "time-series source output receipt changed"
                    : executed.detail.empty()
                    ? "time-series source execution did not complete"
                    : executed.detail;
            return step;
          }
          std::uint64_t current_memory_bytes = 0;
          if (!time_series_runtime_memory_receipt->complete ||
              time_series_runtime_memory_receipt
                      ->provider_logical_memory_bytes == 0 ||
              time_series_runtime_memory_receipt->memory_grant_bytes !=
                  selected_node.memory_bytes_required ||
              time_series_runtime_memory_receipt->peak_live_memory_bytes >
                  time_series_runtime_memory_receipt->memory_grant_bytes ||
              !RuntimeMaterializedBatchMemoryBytes(
                  executed.output.batch, &current_memory_bytes) ||
              current_memory_bytes >
                  time_series_runtime_memory_receipt
                      ->peak_live_memory_bytes) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1";
            step.diagnostic.detail =
                "time-series runtime memory receipt is incomplete";
            return step;
          }
          PublishRuntimeMemoryObservation(
              &step, current_memory_bytes,
              time_series_runtime_memory_receipt->peak_live_memory_bytes);
          if (!standalone_time_series_source) {
            for (auto& column : executed.output.batch.columns) {
              if (column.descriptor.canonical_type_name == "timestamp_tz") {
                column.descriptor.canonical_type_name = "timestamp";
              }
            }
            for (auto& row : executed.output.batch.rows) {
              for (auto& value : row.values) {
                if (value.descriptor.canonical_type_name == "timestamp_tz") {
                  value.descriptor.canonical_type_name = "timestamp";
                }
              }
            }
          }
          step.result_handle_id = selected_node.physical_node_id;
          step.output_row_count = executed.output.batch.rows.size();
          step.rows_examined = executed.rows_examined;
          step.current_relation_descriptor_uuid = persisted_descriptor_uuid;
          step.current_relation_descriptor_generation =
              execution_request.input.descriptor_generation;
          step.materialized_output_batch = std::move(executed.output.batch);
          return step;
          } catch (const std::bad_alloc&) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1";
            step.diagnostic.detail =
                "time-series selected source allocation was refused";
            return step;
          } catch (const std::length_error&) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1";
            step.diagnostic.detail =
                "time-series selected source allocation length was refused";
            return step;
          } catch (const std::exception& exception) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "SB_MODEL_COORDINATOR_LEG_FAILED_V1";
            step.diagnostic.detail =
                std::string("time-series selected source threw: ") +
                exception.what();
            return step;
          } catch (...) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "SB_MODEL_COORDINATOR_LEG_FAILED_V1";
            step.diagnostic.detail =
                "time-series selected source threw a non-standard exception";
            return step;
          }
        };

    std::size_t maximum_columns = public_columns.size();
    for (const auto& node : dag.nodes) {
      maximum_columns =
          std::max(maximum_columns, node.output_descriptor_ids.size());
    }
    std::uint64_t maximum_cells = 0;
    if (maximum_columns == 0 ||
        !CheckedMultiply(source_input.maximum_rows, maximum_columns,
                         &maximum_cells) ||
        maximum_cells > std::numeric_limits<std::size_t>::max()) {
      return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                    "time-series composition cell bound overflowed");
    }
    exec::CanonicalHeapPhysicalRegistrationResult mixed_heap_registration;
    std::size_t mixed_join_pair_bound = 0;
    const auto optimizer_row_bound = static_cast<std::size_t>(
        std::min<std::uint64_t>(
            input.context.optimizer_maximum_candidate_count,
            std::numeric_limits<std::size_t>::max()));
    if (mixed_join != nullptr || asof_join != nullptr) {
      const auto maximum_scanned_row_versions = static_cast<std::size_t>(
          std::min<std::uint64_t>(
              std::min(input.context.optimizer_maximum_search_steps,
                       input.context.optimizer_maximum_candidate_count),
              std::numeric_limits<std::size_t>::max()));
      const auto maximum_decoded_bytes = static_cast<std::size_t>(
          std::min<std::uint64_t>(
              input.context.optimizer_memory_budget_bytes,
              std::numeric_limits<std::size_t>::max()));
      std::uint64_t pair_bound = 0;
      if (maximum_scanned_row_versions == 0 ||
          maximum_decoded_bytes == 0 || optimizer_row_bound == 0 ||
          !CheckedMultiply(source_input.maximum_rows, optimizer_row_bound,
                           &pair_bound) ||
          pair_bound > std::numeric_limits<std::size_t>::max()) {
        return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                      "time-series JOIN bounds are absent or overflow");
      }
      mixed_join_pair_bound = static_cast<std::size_t>(pair_bound);
      auto heap_relational_dag = dag;
      heap_relational_dag.statement_timestamp.clear();
      heap_relational_dag.root_node_id = relational_scan->node_id;
      std::erase_if(heap_relational_dag.nodes, [&](const auto& node) {
        return node.node_id != relational_scan->node_id;
      });
      std::erase_if(
          heap_relational_dag.descriptors, [&](const auto& descriptor) {
            return std::ranges::find(
                       relational_scan->output_descriptor_ids,
                       descriptor.descriptor_id) ==
                   relational_scan->output_descriptor_ids.end();
          });
      std::erase_if(
          heap_relational_dag.expressions, [&](const auto& expression) {
            return std::ranges::find(
                       relational_scan->bound_expression_ids,
                       expression.expression_id) ==
                   relational_scan->bound_expression_ids.end();
          });
      std::erase_if(heap_relational_dag.outputs, [&](const auto& output) {
        return output.relation_node_id != relational_scan->node_id;
      });
      exec::CanonicalHeapPhysicalDagDispatchRequest heap_request;
      heap_request.context = &input.context;
      heap_request.relational_dag = &heap_relational_dag;
      heap_request.physical_dag = composition_physical->physical_dag;
      std::erase_if(heap_request.physical_dag.nodes, [&](const auto& node) {
        return node.relational_node_id != relational_scan->node_id;
      });
      if (heap_request.physical_dag.nodes.size() != 1) {
        return refuse(
            "SB_MODEL_COORDINATOR_LEG_FAILED_V1",
            "time-series JOIN selected heap node is absent");
      }
      heap_request.physical_dag.root_physical_node_id =
          heap_request.physical_dag.nodes.front().physical_node_id;
      heap_request.physical_dag.mga_statement_context.statement_timestamp
          .clear();
      for (auto& node : heap_request.physical_dag.nodes) {
        node.mga_statement_context.statement_timestamp.clear();
      }
      heap_request.maximum_scanned_row_versions =
          maximum_scanned_row_versions;
      heap_request.maximum_decoded_bytes = maximum_decoded_bytes;
      heap_request.maximum_output_rows = optimizer_row_bound;
      heap_request.maximum_output_columns = maximum_columns;
      heap_request.maximum_output_cells =
          static_cast<std::size_t>(maximum_cells);
      heap_request.cancellation_requested =
          input.context.query_cancellation_requested
              ? input.context.query_cancellation_requested
              : std::function<bool()>([] { return false; });
      mixed_heap_registration =
          exec::BuildCanonicalHeapPhysicalRegistration(heap_request);
      if (!mixed_heap_registration.diagnostic.ok ||
          !mixed_heap_registration.registration.has_value()) {
        return refuse(
            mixed_heap_registration.diagnostic.diagnostic_code.empty()
                ? "SB_MODEL_COORDINATOR_LEG_FAILED_V1"
                : mixed_heap_registration.diagnostic.diagnostic_code,
            mixed_heap_registration.diagnostic.detail.empty()
                ? "time-series mixed JOIN heap executor was refused"
                : mixed_heap_registration.diagnostic.detail);
      }
      auto registration =
          std::move(*mixed_heap_registration.registration);
      auto execute_heap = std::move(registration.execute);
      registration.execute =
          [execute_heap = std::move(execute_heap)](
              const exec::TypedPhysicalNodeDag& selected_dag,
              const exec::PhysicalNodeRecord& selected_node,
              const std::vector<exec::CanonicalPhysicalDispatchInput>& inputs)
              mutable {
            auto heap_dag = selected_dag;
            std::erase_if(heap_dag.nodes, [&](const auto& node) {
              return node.physical_node_id !=
                     selected_node.physical_node_id;
            });
            if (heap_dag.nodes.size() != 1) {
              exec::CanonicalPhysicalDispatchStepResult step;
              step.diagnostic.ok = false;
              step.diagnostic.diagnostic_code =
                  "SB_MODEL_MGA_CONTEXT_MISMATCH_V1";
              step.diagnostic.detail =
                "time-series JOIN heap node identity changed";
              return step;
            }
            heap_dag.root_physical_node_id =
                heap_dag.nodes.front().physical_node_id;
            heap_dag.mga_statement_context.statement_timestamp.clear();
            for (auto& node : heap_dag.nodes) {
              node.mga_statement_context.statement_timestamp.clear();
            }
            const auto heap_node = std::ranges::find_if(
                heap_dag.nodes, [&](const auto& node) {
                  return node.physical_node_id ==
                         selected_node.physical_node_id;
                });
            if (heap_node == heap_dag.nodes.end()) {
              exec::CanonicalPhysicalDispatchStepResult step;
              step.diagnostic.ok = false;
              step.diagnostic.diagnostic_code =
                  "SB_MODEL_MGA_CONTEXT_MISMATCH_V1";
              step.diagnostic.detail =
                  "time-series JOIN heap node identity changed";
              return step;
            }
            heap_node->dispatcher_callback_memory_limit_bytes =
                selected_node.dispatcher_callback_memory_limit_bytes;
            auto step = execute_heap(heap_dag, *heap_node, inputs);
            if (step.diagnostic.ok) {
              step.mga_statement_context = selected_dag.mga_statement_context;
            }
            return step;
          };
      mixed_heap_registration.registration = std::move(registration);
    }
    api::CanonicalOptimizerSelectedExecutionRequest selected;
    selected.selected_physical_dag = composition_physical->physical_dag;
    selected.pre_access_statistics_snapshot_uuid =
        composition_physical->physical_dag.statistics_snapshot_uuid;
    selected.mga_authority = BuildCanonicalExecutionMgaAuthority(
        input.context, composition_physical->physical_dag);
    selected.runtime_limits.maximum_rows_per_batch =
        source_input.maximum_rows;
    selected.runtime_limits.maximum_columns_per_batch = maximum_columns;
    selected.runtime_limits.maximum_cells_per_batch =
        static_cast<std::size_t>(maximum_cells);
    selected.runtime_limits.maximum_total_materialized_rows =
        source_input.maximum_rows;
    selected.runtime_limits.maximum_total_materialized_cells =
        static_cast<std::size_t>(maximum_cells);
    selected.cancellation_requested =
        raw_time_series_cancellation
            ? raw_time_series_cancellation
            : std::function<bool()>([] { return false; });
    selected.available_executors.push_back(std::move(source_registration));
    if (composition_set.has_value()) {
      std::unordered_map<std::uint64_t, exec::DescriptorBatch> values_batches;
      values_batches.emplace(set_values->node_id,
                             std::move(composition_set_values.batch));
      selected.available_executors.push_back(MakeLiveValuesRegistration(
          std::move(values_batches), composition_set_values_capability_uuid,
          "QOW-DIAG-RELATIONAL-LIVE-SET-VALUES-V1",
          "time-series UNION ALL"));
      std::unordered_map<std::uint64_t, PreparedLiveSetNode>
          prepared_set_nodes;
      prepared_set_nodes.emplace(set_root->node_id, *composition_set);
      selected.available_executors.push_back(
          MakeLiveSetOperationRegistration(
              MakeLiveSetRegistrationProfilesForComposition(prepared_set_nodes),
              time_series_set_profile.implementation_id,
              composition_set_root_capability_uuid, input.context));
    }
    if (mixed_join != nullptr) {
      selected.available_executors.push_back(
          std::move(*mixed_heap_registration.registration));
      selected.available_executors.push_back(MakeLiveJoinRegistration(
          "join." + composition_mixed_join_component +
              ".3vl.nested.v1",
          composition_mixed_join_capability_uuid, {}, mixed_join_pair_bound,
          optimizer_row_bound, *composition_mixed_join_kind,
          "time-series mixed " + composition_mixed_join_operation,
          input.context, true,
          *composition_mixed_join_kind ==
                  exec::CanonicalAcceptedJoinKind::kCross
              ? 0
              : mixed_join->bound_expression_ids.front(),
          composition_mixed_join_predicate_binding, dag,
          composition_planning_request->expression_services, {},
          std::nullopt, &dag, &input.context,
          &selected.mga_authority));
    }
    if (asof_join != nullptr) {
      selected.available_executors.push_back(
          std::move(*mixed_heap_registration.registration));
      exec::CanonicalPhysicalExecutorRegistration asof_registration;
      asof_registration.node_kind = exec::PhysicalNodeKind::kJoin;
      asof_registration.implementation_id =
          composition_asof_join_implementation_id;
      asof_registration.executor_capability_uuid =
          composition_asof_join_capability_uuid;
      asof_registration.executor_capability_abi_version = 1;
      asof_registration.engine_owned = true;
      asof_registration.accepts_optimizer_publication_v2 = true;
      asof_registration.honors_dispatcher_memory_limit_v1 = true;
      asof_registration.retained_live_memory_bytes_v1 =
          sizeof(exec::CanonicalTimeSeriesAsofInputBindingV1) * 2 +
          sizeof(api::EngineRequestContext) + 8 * sizeof(void*) + 512;
      asof_registration.execute =
          [left_binding = composition_asof_left_binding,
           right_binding = composition_asof_right_binding,
           tolerance_ns = composition_asof_tolerance_ns,
           maximum_comparisons = composition_asof_maximum_comparisons,
           maximum_output_rows = composition_asof_maximum_output_rows,
           left_outer = composition_asof_left_outer,
           bound_transformation_receipt =
               composition_asof_transformation_receipt,
           execution_context = input.context,
           borrowed_mga_authority = &selected.mga_authority](
              const exec::TypedPhysicalNodeDag& selected_dag,
              const exec::PhysicalNodeRecord& selected_node,
              const std::vector<exec::CanonicalPhysicalDispatchInput>& inputs) {
            exec::CanonicalPhysicalDispatchStepResult step;
            step.selected_plan_uuid = selected_dag.selected_plan_uuid;
            step.executed_physical_node_id = selected_node.physical_node_id;
            step.causal_counter_id = selected_node.causal_counter_id;
            step.output_descriptor_ids = selected_node.output_descriptor_ids;
            step.mga_statement_context = selected_dag.mga_statement_context;
            step.authority.engine_mga_snapshot_bound = true;
            step.data_access_observation_known = true;
            step.data_access_observed = false;
            if (selected_dag.abi_version != 2 || inputs.size() != 2 ||
                selected_node.node_kind != exec::PhysicalNodeKind::kJoin ||
                selected_node.input_physical_node_ids.size() != 2 ||
                inputs[0].physical_node_id !=
                    selected_node.input_physical_node_ids[0] ||
                inputs[1].physical_node_id !=
                    selected_node.input_physical_node_ids[1] ||
                !inputs[0].materialized_output_batch.has_value() ||
                !inputs[1].materialized_output_batch.has_value()) {
              step.diagnostic.ok = false;
              step.diagnostic.diagnostic_code =
                  "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1";
              step.diagnostic.detail =
                  "selected time-series ASOF input identity changed";
              return step;
            }
            const auto& left_input_batch =
                *inputs[0].materialized_output_batch;
            const auto& right_input_batch =
                *inputs[1].materialized_output_batch;
            const auto* cancellation_policy =
                FindLiveCancellationPolicy(selected_dag);
            if (cancellation_policy == nullptr) {
              step.diagnostic.ok = false;
              step.diagnostic.diagnostic_code =
                  "QOW-DIAG-RELATIONAL-LIVE-ASOF-CANCELLATION-POLICY-V1";
              step.diagnostic.detail =
                  "time-series ASOF bridge requires one exact cancellation "
                  "policy evidence row";
              return step;
            }
            const auto cancellation_requested =
                execution_context.query_cancellation_requested
                    ? execution_context.query_cancellation_requested
                    : std::function<bool()>([] { return false; });
            auto cancellation_state = LiveCancellationProbeState::kRunning;
            const std::function<bool()> guarded_cancellation_requested =
                [&]() noexcept {
              return PollLiveCancellationProbe(cancellation_requested,
                                               &cancellation_state);
            };
            const auto cancelled = [&]() {
              return guarded_cancellation_requested();
            };
            const auto bind_cancellation = [&](const char* detail) {
              exec::DescriptorRuntimeDiagnostic diagnostic;
              diagnostic.ok = false;
              diagnostic.diagnostic_code =
                  cancellation_state ==
                          LiveCancellationProbeState::kProbeFailed
                      ? "SB_MODEL_COORDINATOR_LEG_FAILED_V1"
                      : "SB_MODEL_EXECUTION_CANCELLED_V1";
              diagnostic.detail =
                  cancellation_state ==
                          LiveCancellationProbeState::kProbeFailed
                      ? "time-series ASOF bridge cancellation probe failed"
                      : detail;
              BindLiveCancellationFailure(std::move(diagnostic),
                                          cancellation_policy, &step);
            };
            if (cancelled()) {
              bind_cancellation(
                  "time-series ASOF bridge was cancelled before preflight");
              return step;
            }
            std::uint64_t asof_auxiliary_memory_bytes =
                sizeof(exec::CanonicalTimeSeriesAsofJoinRequestV1);
            if (!CheckedAdd(asof_auxiliary_memory_bytes,
                            bound_transformation_receipt.size() + 1,
                            &asof_auxiliary_memory_bytes)) {
              step.diagnostic.ok = false;
              step.diagnostic.diagnostic_code =
                  "SBLR.PLAN_TREE.RESOURCE_LIMIT";
              step.diagnostic.detail =
                  "time-series ASOF semantic receipt size overflowed";
              return step;
            }
            std::uint64_t asof_auxiliary_component_bytes = 0;
            if (!CheckedMultiply(
                    left_input_batch.rows.size(),
                    sizeof(exec::CanonicalTimeSeriesAsofKeyV1),
                    &asof_auxiliary_component_bytes) ||
                !CheckedAdd(asof_auxiliary_memory_bytes,
                            asof_auxiliary_component_bytes,
                            &asof_auxiliary_memory_bytes) ||
                !CheckedMultiply(
                    right_input_batch.rows.size(),
                    sizeof(exec::CanonicalTimeSeriesAsofKeyV1) +
                        sizeof(std::string),
                    &asof_auxiliary_component_bytes) ||
                !CheckedAdd(asof_auxiliary_memory_bytes,
                            asof_auxiliary_component_bytes,
                            &asof_auxiliary_memory_bytes) ||
                !CheckedMultiply(left_input_batch.rows.size(),
                                 sizeof(std::int64_t),
                                 &asof_auxiliary_component_bytes) ||
                !CheckedAdd(asof_auxiliary_memory_bytes,
                            asof_auxiliary_component_bytes,
                            &asof_auxiliary_memory_bytes)) {
              step.diagnostic.ok = false;
              step.diagnostic.diagnostic_code =
                  "SBLR.PLAN_TREE.RESOURCE_LIMIT";
              step.diagnostic.detail =
                  "time-series ASOF bridge workspace bound overflowed";
              return step;
            }
            exec::TypedPhysicalNodeDag scoped_execution_dag;
            std::size_t callback_memory_bound = 0;
            std::string callback_memory_detail;
            if (!BuildStrictBinaryOperatorLocalPhysicalDag(
                    selected_dag, selected_node, left_input_batch,
                    right_input_batch, asof_auxiliary_memory_bytes,
                    &scoped_execution_dag, &callback_memory_bound,
                    &callback_memory_detail)) {
              step.diagnostic.ok = false;
              step.diagnostic.diagnostic_code =
                  "SBLR.PLAN_TREE.RESOURCE_LIMIT";
              step.diagnostic.detail =
                  "time-series ASOF bridge " + callback_memory_detail;
              return step;
            }
            std::uint64_t bridge_peak_bytes =
                sizeof(exec::CanonicalTimeSeriesAsofJoinRequestV1);
            const auto account = [&](const std::uint64_t bytes) {
              return bytes <= std::numeric_limits<std::uint64_t>::max() -
                                  bridge_peak_bytes &&
                     (bridge_peak_bytes += bytes) <=
                         callback_memory_bound;
            };
            const auto account_string = [&](const std::string_view value) {
              return value.size() <
                         std::numeric_limits<std::uint64_t>::max() &&
                     account(static_cast<std::uint64_t>(value.size()) + 1);
            };
            const auto account_descriptor =
                [&](const api::EngineDescriptor& descriptor) {
                  return account_string(
                             descriptor.descriptor_uuid.canonical) &&
                         account_string(descriptor.descriptor_kind) &&
                         account_string(descriptor.canonical_type_name) &&
                         account_string(descriptor.encoded_descriptor);
                };
            bool preflight_cancelled = false;
            const auto account_batch_copy =
                [&](const exec::DescriptorBatch& batch) {
                  std::uint64_t bytes = 0;
                  bool ok =
                      CheckedMultiply(batch.columns.size(),
                                      sizeof(exec::ExecutorColumnDescriptor),
                                      &bytes) &&
                      account(bytes) &&
                      CheckedMultiply(batch.rows.size(),
                                      sizeof(exec::DescriptorTuple), &bytes) &&
                      account(bytes);
                  for (const auto& column : batch.columns) {
                    if (cancelled()) {
                      preflight_cancelled = true;
                      return false;
                    }
                    ok = ok && account_string(column.stable_name) &&
                         account_descriptor(column.descriptor);
                  }
                  for (const auto& row : batch.rows) {
                    if (cancelled()) {
                      preflight_cancelled = true;
                      return false;
                    }
                    ok = ok &&
                         CheckedMultiply(row.values.size(),
                                         sizeof(api::EngineTypedValue),
                                         &bytes) &&
                         account(bytes);
                    for (const auto& value : row.values) {
                      ok = ok && account_descriptor(value.descriptor) &&
                           account_string(value.encoded_value) &&
                           account(value.binary_value.size());
                    }
                  }
                  return ok;
                };
            const auto account_binding_keys =
                [&](const exec::DescriptorBatch& batch,
                    const exec::CanonicalTimeSeriesAsofInputBindingV1& binding,
                    const bool right_input) {
                  std::uint64_t bytes = 0;
                  bool ok = CheckedMultiply(
                                batch.rows.size(),
                                sizeof(exec::CanonicalTimeSeriesAsofKeyV1),
                                &bytes) &&
                            account(bytes);
                  if (right_input && binding.raw_time_series) {
                    ok = ok &&
                         CheckedMultiply(batch.rows.size(), sizeof(std::string),
                                         &bytes) &&
                         account(bytes);
                  }
                  for (const auto& row : batch.rows) {
                    if (cancelled()) {
                      preflight_cancelled = true;
                      return false;
                    }
                    if (binding.metric_column_ordinal >= row.values.size() ||
                        binding.tags_column_ordinal >= row.values.size() ||
                        binding.timestamp_column_ordinal >=
                            row.values.size() ||
                        (binding.raw_time_series &&
                         binding.row_uuid_column_ordinal >=
                             row.values.size())) {
                      return false;
                    }
                    ok = ok &&
                         account_string(
                             row.values[binding.metric_column_ordinal]
                                 .encoded_value) &&
                         account_string(
                             row.values[binding.tags_column_ordinal]
                                 .encoded_value);
                    if (right_input && binding.raw_time_series) {
                      ok = ok && account_string(
                                     row.values[binding.row_uuid_column_ordinal]
                                         .encoded_value);
                    }
                  }
                  return ok;
                };
            std::uint64_t dag_vector_bytes = 0;
            const auto account_string_vector =
                [&](const std::vector<std::string>& values) {
                  bool ok = CheckedMultiply(values.size(), sizeof(std::string),
                                            &dag_vector_bytes) &&
                            account(dag_vector_bytes);
                  for (const auto& value : values) {
                    ok = ok && account_string(value);
                  }
                  return ok;
                };
            const auto account_mga =
                [&](const exec::PhysicalMgaStatementContext& mga) {
                  return account_string(mga.statement_uuid) &&
                         account_string(mga.statement_timestamp) &&
                         account_string(mga.owning_transaction_uuid) &&
                         account_string(mga.statement_snapshot_uuid) &&
                         account_string(
                             mga.statement_metadata_snapshot_uuid) &&
                         account_string(mga.snapshot_kind) &&
                         CheckedMultiply(
                             mga.active_excluded_local_transaction_ids.size(),
                             sizeof(std::uint64_t), &dag_vector_bytes) &&
                         account(dag_vector_bytes) &&
                         CheckedMultiply(
                             mga.in_doubt_excluded_local_transaction_ids.size(),
                             sizeof(std::uint64_t), &dag_vector_bytes) &&
                         account(dag_vector_bytes);
                };
            bool dag_copy_ok =
                account_string(selected_dag.selected_plan_uuid) &&
                account_string(selected_dag.bound_sblr_tree_uuid) &&
                account_string(selected_dag.catalog_epoch_uuid) &&
                account_string(selected_dag.security_context_uuid) &&
                account_string(selected_dag.capability_snapshot_uuid) &&
                account_string(selected_dag.resource_snapshot_uuid) &&
                account_string(selected_dag.statistics_snapshot_uuid) &&
                account_string(selected_dag.route_snapshot_uuid) &&
                account_string(selected_dag.selected_plan_signature) &&
                account_mga(selected_dag.mga_statement_context);
            dag_copy_ok =
                dag_copy_ok &&
                CheckedMultiply(selected_dag.nodes.size(),
                                sizeof(exec::PhysicalNodeRecord),
                                &dag_vector_bytes) &&
                account(dag_vector_bytes) &&
                CheckedMultiply(selected_dag.admission_evidence.size(),
                                sizeof(exec::PhysicalAdmissionEvidence),
                                &dag_vector_bytes) &&
                account(dag_vector_bytes);
            for (const auto& evidence : selected_dag.admission_evidence) {
              dag_copy_ok = dag_copy_ok &&
                            account_string(evidence.evidence_uuid);
            }
            for (const auto& node : selected_dag.nodes) {
              if (cancelled()) {
                preflight_cancelled = true;
                dag_copy_ok = false;
                break;
              }
              dag_copy_ok =
                  dag_copy_ok && account_string(node.implementation_id) &&
                  account_string(node.selected_alternative_uuid) &&
                  account_string(node.executor_capability_uuid) &&
                  account_string(node.cost_vector_uuid) &&
                  account_string(node.logical_semantic_variant_id) &&
                  account_string(node.transformation_uuid) &&
                  account_string(node.transformation_rule_id) &&
                  account_string(node.retained_cost.cost_vector_uuid) &&
                  account_string(
                      node.retained_cost.calibration_profile_uuid) &&
                  account_string(
                      node.retained_cost.scalarization_policy_id) &&
                  account_string_vector(node.required_property_uuids) &&
                  account_string_vector(node.delivered_property_uuids) &&
                  account_string_vector(node.enforced_property_uuids) &&
                  account_mga(node.mga_statement_context) &&
                  CheckedMultiply(node.input_physical_node_ids.size(),
                                  sizeof(std::uint64_t), &dag_vector_bytes) &&
                  account(dag_vector_bytes) &&
                  CheckedMultiply(node.output_descriptor_ids.size(),
                                  sizeof(std::uint32_t), &dag_vector_bytes) &&
                  account(dag_vector_bytes);
            }
            const auto account_output_peak = [&]() {
              std::uint64_t bytes = 0;
              const auto output_width = left_input_batch.columns.size() +
                                        right_input_batch.columns.size();
              bool ok =
                  CheckedMultiply(output_width,
                                  sizeof(exec::ExecutorColumnDescriptor),
                                  &bytes) &&
                  account(bytes) &&
                  CheckedMultiply(left_input_batch.rows.size(),
                                  sizeof(exec::DescriptorTuple), &bytes) &&
                  account(bytes) &&
                  CheckedMultiply(left_input_batch.rows.size(), output_width,
                                  &bytes) &&
                  CheckedMultiply(bytes, sizeof(api::EngineTypedValue),
                                  &bytes) &&
                  account(bytes) &&
                  CheckedMultiply(left_input_batch.rows.size(),
                                  sizeof(std::int64_t), &bytes) &&
                  account(bytes);
              for (const auto& column : left_input_batch.columns) {
                ok = ok && account_string(column.stable_name) &&
                     account_descriptor(column.descriptor);
              }
              for (const auto& column : right_input_batch.columns) {
                ok = ok && account_string(column.stable_name) &&
                     account_descriptor(column.descriptor) &&
                     (!left_outer || account(32));
              }
              const auto measured_string = [](const std::string_view value,
                                              std::uint64_t* measured) {
                return measured != nullptr &&
                       value.size() <
                           std::numeric_limits<std::uint64_t>::max() &&
                       CheckedAdd(*measured,
                                  static_cast<std::uint64_t>(value.size()) + 1,
                                  measured);
              };
              const auto measured_descriptor =
                  [&](const api::EngineDescriptor& descriptor,
                      std::uint64_t* measured) {
                    return measured_string(
                               descriptor.descriptor_uuid.canonical,
                               measured) &&
                           measured_string(descriptor.descriptor_kind,
                                           measured) &&
                           measured_string(descriptor.canonical_type_name,
                                           measured) &&
                           measured_string(descriptor.encoded_descriptor,
                                           measured);
                  };
              const auto measured_value =
                  [&](const api::EngineTypedValue& value,
                      std::uint64_t* measured) {
                    return measured_descriptor(value.descriptor, measured) &&
                           measured_string(value.encoded_value, measured) &&
                           CheckedAdd(*measured, value.binary_value.size(),
                                      measured);
                  };
              std::uint64_t unmatched_right_dynamic = 0;
              for (const auto& column : right_input_batch.columns) {
                ok = ok && measured_descriptor(column.descriptor,
                                                &unmatched_right_dynamic) &&
                     (!left_outer ||
                      CheckedAdd(unmatched_right_dynamic, 32,
                                 &unmatched_right_dynamic));
              }
              std::uint64_t maximum_right_dynamic =
                  unmatched_right_dynamic;
              for (const auto& row : right_input_batch.rows) {
                if (cancelled()) {
                  preflight_cancelled = true;
                  return false;
                }
                std::uint64_t row_dynamic = 0;
                for (const auto& value : row.values) {
                  ok = ok && measured_value(value, &row_dynamic);
                }
                maximum_right_dynamic =
                    std::max(maximum_right_dynamic, row_dynamic);
              }
              for (const auto& row : left_input_batch.rows) {
                if (cancelled()) {
                  preflight_cancelled = true;
                  return false;
                }
                for (const auto& value : row.values) {
                  std::uint64_t value_dynamic = 0;
                  ok = ok && measured_value(value, &value_dynamic) &&
                       account(value_dynamic);
                }
                ok = ok && account(maximum_right_dynamic);
              }
              const auto& mga = selected_dag.mga_statement_context;
              ok = ok && account_string(selected_dag.selected_plan_uuid) &&
                   account_string(mga.statement_uuid) &&
                   account_string(mga.statement_timestamp) &&
                   account_string(mga.owning_transaction_uuid) &&
                   account_string(mga.statement_snapshot_uuid) &&
                   account_string(mga.statement_metadata_snapshot_uuid) &&
                   account_string(mga.snapshot_kind) &&
                   CheckedMultiply(
                       mga.active_excluded_local_transaction_ids.size(),
                       sizeof(std::uint64_t), &bytes) &&
                   account(bytes) &&
                   CheckedMultiply(
                       mga.in_doubt_excluded_local_transaction_ids.size(),
                       sizeof(std::uint64_t), &bytes) &&
                   account(bytes);
              return ok;
            };
            const bool bridge_preflight_ok =
                callback_memory_bound != 0 && dag_copy_ok &&
                // Dispatcher inputs remain live while request-owned copies,
                // ASOF keys, and the maximum result coexist.
                account_batch_copy(left_input_batch) &&
                account_batch_copy(right_input_batch) &&
                account_batch_copy(left_input_batch) &&
                account_batch_copy(right_input_batch) &&
                account_binding_keys(left_input_batch, left_binding, false) &&
                account_binding_keys(right_input_batch, right_binding, true) &&
                account_output_peak();
            if (!bridge_preflight_ok) {
              if (preflight_cancelled) {
                bind_cancellation(
                    "time-series ASOF bridge was cancelled during preflight");
              } else {
                step.diagnostic.ok = false;
                step.diagnostic.diagnostic_code =
                    "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1";
                step.diagnostic.detail =
                    "time-series ASOF combined input/copy/key/result peak "
                    "exceeded the selected memory bound";
              }
              return step;
            }
            exec::CanonicalTimeSeriesAsofJoinRequestV1 request;
            try {
              request.physical_dag = selected_dag;
              request.selected_physical_node_id =
                  selected_node.physical_node_id;
              request.left_batch = left_input_batch;
              request.right_batch = right_input_batch;
              request.left_binding = left_binding;
              request.right_binding = right_binding;
              request.bound_transformation_receipt =
                  bound_transformation_receipt;
              request.tolerance_ns = tolerance_ns;
              request.left_outer = left_outer;
              request.right_is_time_series_raw =
                  right_binding.raw_time_series;
              request.maximum_output_rows = maximum_output_rows;
              request.maximum_comparisons = maximum_comparisons;
              request.maximum_memory_bytes = selected_dag.memory_budget_bytes;
              request.mga_authority = *borrowed_mga_authority;
              request.cancellation_requested = cancellation_requested;
            const auto append_keys =
                [&](const exec::DescriptorBatch& batch,
                    const exec::CanonicalTimeSeriesAsofInputBindingV1& binding,
                    const bool right_input,
                    std::vector<exec::CanonicalTimeSeriesAsofKeyV1>* keys,
                    std::vector<std::string>* right_ties) {
                  if (keys == nullptr || right_ties == nullptr) return false;
                  keys->reserve(batch.rows.size());
                  if (right_input && binding.raw_time_series) {
                    right_ties->reserve(batch.rows.size());
                  }
                  for (const auto& row : batch.rows) {
                    if (cancelled()) {
                      preflight_cancelled = true;
                      return false;
                    }
                    if (binding.metric_column_ordinal >= row.values.size() ||
                        binding.tags_column_ordinal >= row.values.size() ||
                        binding.timestamp_column_ordinal >=
                            row.values.size() ||
                        (binding.raw_time_series &&
                         binding.row_uuid_column_ordinal >=
                             row.values.size())) {
                      return false;
                    }
                    std::int64_t timestamp = 0;
                    if (!exec::ParseCanonicalTimeSeriesTimestampNsV1(
                            row.values[binding.timestamp_column_ordinal]
                                .encoded_value,
                            &timestamp)) {
                      return false;
                    }
                    keys->push_back(
                        {row.values[binding.metric_column_ordinal]
                             .encoded_value,
                         row.values[binding.tags_column_ordinal]
                             .encoded_value,
                         timestamp});
                    if (right_input && binding.raw_time_series) {
                      right_ties->push_back(
                          row.values[binding.row_uuid_column_ordinal]
                              .encoded_value);
                    }
                  }
                  return true;
                };
            if (!append_keys(request.left_batch, request.left_binding, false,
                             &request.left_keys,
                             &request.right_tie_break_row_uuids) ||
                !append_keys(request.right_batch, request.right_binding, true,
                             &request.right_keys,
                             &request.right_tie_break_row_uuids)) {
              if (preflight_cancelled) {
                bind_cancellation(
                    "time-series ASOF bridge was cancelled during key "
                    "construction");
              } else {
                step.diagnostic.ok = false;
                step.diagnostic.diagnostic_code =
                    "SB_MODEL_TIME_SERIES_IDENTITY_INVALID_V1";
                step.diagnostic.detail =
                    "selected time-series ASOF bound key could not be "
                    "decoded";
              }
              return step;
            }
            if (cancelled()) {
              bind_cancellation(
                  "time-series ASOF bridge was cancelled before execution");
              return step;
            }
            auto executed =
                exec::ExecuteCanonicalTimeSeriesAsofJoinV1(request);
            if (!executed.diagnostic.ok) {
              BindLiveCancellationFailure(std::move(executed.diagnostic),
                                          cancellation_policy, &step);
              return step;
            }
            if (executed.selected_plan_uuid !=
                    selected_dag.selected_plan_uuid ||
                executed.executed_physical_node_id !=
                    selected_node.physical_node_id ||
                executed.causal_counter_id != selected_node.causal_counter_id ||
                !exec::PhysicalMgaStatementContextEqual(
                    executed.mga_statement_context,
                    selected_dag.mga_statement_context)) {
              step.diagnostic.ok = false;
              step.diagnostic.diagnostic_code =
                  "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1";
              step.diagnostic.detail =
                  "time-series ASOF execution receipt changed";
              return step;
            }
            step.result_handle_id = selected_node.physical_node_id;
            step.input_row_count = request.left_batch.rows.size() +
                                   request.right_batch.rows.size();
            step.output_row_count = executed.output_batch.rows.size();
            step.rows_examined = step.input_row_count;
            step.materialized_output_batch =
                std::move(executed.output_batch);
            return step;
            } catch (const std::bad_alloc&) {
              step.diagnostic.ok = false;
              step.diagnostic.diagnostic_code =
                  "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1";
              step.diagnostic.detail =
                  "time-series ASOF bridge allocation was refused";
              return step;
            }
          };
      selected.available_executors.push_back(std::move(asof_registration));
    }
    if (composition_filter.has_value()) {
      selected.available_executors.push_back(
          MakeLiveHeapFilterRegistration(
              composition_filter->predicate_expression_id,
              composition_filter->predicate_row_binding, dag,
              composition_planning_request->expression_services,
              composition_filter_capability_uuid, source_input.maximum_rows,
              input.context,
              api::EngineCanonicalExpressionConsumer::filter,
              api::EnginePredicateConsumer::filter, &dag, &input.context,
              &selected.mga_authority));
    }
    if (composition_project.has_value()) {
      selected.available_executors.push_back(
          MakeLiveHeapProjectRegistration(
              composition_project->projected_columns,
              composition_project_capability_uuid,
              source_input.maximum_rows, {}, &input.context,
              &selected.mga_authority));
    }
    if (composition_sort.has_value()) {
      std::uint64_t comparison_bound = 0;
      if (!CheckedMultiply(source_input.maximum_rows,
                           source_input.maximum_rows, &comparison_bound) ||
          comparison_bound > std::numeric_limits<std::size_t>::max()) {
        return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                      "time-series SORT comparison bound overflowed");
      }
      const auto tie_uuid = DerivedCanonicalUuid(
          identity_scope + ":" + composition_sort->ordering_property_uuid,
          "time-series.sort.deterministic-tie");
      selected.available_executors.push_back(MakeLiveSortRegistration(
          composition_sort->order_terms, tie_uuid,
          composition_sort_capability_uuid, source_input.maximum_rows,
          std::max<std::size_t>(
              1, static_cast<std::size_t>(comparison_bound)),
          {}, &input.context, &selected.mga_authority));
    }
    if (composition_row_number.has_value()) {
      selected.available_executors.push_back(MakeLiveRowNumberRegistration(
          *composition_row_number, composition_window_order_evidence_uuid,
          composition_window_capability_uuid, source_input.maximum_rows,
          {}, &input.context, &selected.mga_authority));
    }
    if (composition_nonrecursive_cte) {
      selected.available_executors.push_back(
          MakeLiveNonrecursiveCteRegistration(
              composition_cte_implementation_id,
              composition_cte_capability_uuid, source_input.maximum_rows,
              {}, &input.context, &selected.mga_authority));
    }
    if (composition_count_star.has_value()) {
      selected.available_executors.push_back(MakeLiveCountStarRegistration(
          composition_count_star->result_column,
          composition_aggregate_capability_uuid, source_input.maximum_rows,
          input.context, &input.context, &selected.mga_authority));
    }
    if (composition_recursive_cte.has_value()) {
      selected.available_executors.push_back(
          MakeLiveRecursiveCteTermRegistration(
              composition_recursive_cte->term,
              composition_recursive_term_capability_uuid, input.context));
      selected.available_executors.push_back(MakeLiveRecursiveCteRegistration(
          *composition_recursive_cte,
          composition_recursive_term_capability_uuid,
          composition_recursive_root_capability_uuid, input.context));
    }
    if (composition_limit.has_value()) {
      selected.available_executors.push_back(MakeLiveLimitRegistration(
          composition_limit_implementation_id,
          composition_limit_capability_uuid, composition_limit_count,
          composition_limit_offset, composition_fetch_first_rows_only,
          source_input.maximum_rows, {}, &input.context,
          &selected.mga_authority));
    }
    selected.engine_execution_authorized = true;
    selected.result_publication_request.statement_uuid =
        input.context.statement_uuid.canonical;
    selected.result_publication_request.invocation_mode =
        exec::CanonicalResultInvocationMode::kDirect;
    selected.result_publication_request.execution_attempt_uuid =
        DerivedCanonicalUuid(identity_scope + ":" +
                                 input.context.current_monotonic_ns,
                             "time-series-composition.execution-attempt");
    selected.result_publication_request.result_kind =
        exec::CanonicalResultKind::kRows;
    selected.result_publication_request.transaction_effect_evidence_uuid =
        DerivedCanonicalUuid(
            identity_scope + ":" +
                std::to_string(input.context.local_transaction_id) + ":" +
                std::to_string(input.context
                                   .snapshot_visible_through_local_transaction_id),
            "time-series-composition.transaction-effect-unchanged");
    selected.result_publication_request.maximum_row_count =
        source_input.maximum_rows;
    selected.result_publication_request.column_bindings =
        composition_state.result_bindings;
    const auto execution = ExecuteSelectedCanonicalObjectFreeDag(
        input.context, selected,
        composition_physical->ordinary_runtime_memory_receipts);
    const auto root = std::ranges::find_if(
        composition_physical->physical_dag.nodes, [&](const auto& node) {
          return node.physical_node_id ==
                 composition_physical->physical_dag.root_physical_node_id;
        });
    if (!execution.accepted || !execution.exact_selected_nodes_executed ||
        !execution.causal_counters_attached ||
        !execution.canonical_result_published ||
        !execution.data_access_observed ||
        !execution.runtime_actuals.accepted || !execution.issues.empty() ||
        root == composition_physical->physical_dag.nodes.end() ||
        root->relational_node_id != dag.root_node_id ||
        execution.dispatch.executed_steps.size() !=
            composition_physical->physical_dag.nodes.size() ||
        execution.dispatch.executed_root_physical_node_id !=
            root->physical_node_id ||
        execution.dispatch.selected_plan_uuid !=
            composition_physical->physical_dag.selected_plan_uuid ||
        execution.result_publication.envelope.column_descriptors.size() !=
            composition_state.result_bindings.size()) {
      const bool cancellation_observed =
          execution.cancellation_observed ||
          execution.dispatch.diagnostic.diagnostic_code ==
              "SB_MODEL_EXECUTION_CANCELLED_V1" ||
          (!execution.issues.empty() &&
           execution.issues.front().diagnostic_id ==
               "SB_MODEL_EXECUTION_CANCELLED_V1");
      const std::string cancellation_detail =
          !execution.dispatch.diagnostic.detail.empty()
              ? execution.dispatch.diagnostic.detail
              : !execution.issues.empty()
                    ? execution.issues.front().field_id
                    : "time-series combined execution cancellation detail "
                      "was absent";
      refuse(cancellation_observed
                 ? "SB_MODEL_EXECUTION_CANCELLED_V1"
                 : execution.issues.empty()
                       ? "SB_MODEL_COORDINATOR_LEG_FAILED_V1"
                       : execution.issues.front().diagnostic_id,
             cancellation_observed
                 ? cancellation_detail
                 : execution.issues.empty()
                       ? "time-series combined physical execution did not "
                         "complete"
                       : execution.issues.front().field_id);
      if (source_execution_receipt->execution_started) {
        replace_source_receipt(
            true, source_execution_receipt->data_access_observed,
            source_execution_receipt->cleanup_count,
            source_execution_receipt->cleanup_complete);
      }
      if (cancellation_observed) {
        result.api_result.evidence.push_back(
            {"canonical.time_series_cancellation_checkpoint",
             cancellation_detail});
      }
      append_rollup_evidence(&result.api_result);
      return result;
    }
    result.physical_dag_executed = true;
    result.runtime_actuals_attached = true;
    result.canonical_result_published = true;
    result.canonical_result_column_count =
        execution.result_publication.envelope.column_descriptors.size();
    result.canonical_result_row_count =
        execution.result_publication.row_stream.rows.size();
    result.canonical_result_bytes =
        execution.result_publication.canonical_envelope_bytes;
    result.api_result =
        SuccessfulApiResult(*composition_planning_request, execution);
    result.api_result.evidence.push_back(
        {"canonical.model_route",
         "SBSQL_TIME_SERIES_SOURCE_TO_CANONICAL_RELATIONAL_ROOT_V1"});
    result.api_result.evidence.push_back(
        {"canonical.model_search_family", "time_series.local.v1"});
    result.api_result.evidence.push_back(
        {"canonical.time_series_operation", source_input.operation_id});
    result.api_result.evidence.push_back(
        {"canonical.time_series_composition_root",
         std::to_string(dag.root_node_id)});
    result.api_result.evidence.push_back(
        {"canonical.time_series_provider_route",
         source_input.exact_fallback_selected
             ? "TIME_SERIES_BUCKET_STORE_SCAN_EXACT_V1"
             : "TIME_SERIES_PREFERRED_PROVIDER_V1"});
    result.api_result.evidence.push_back(
        {"canonical.time_series_provider_generation",
         std::to_string(source_input.provider_generation)});
    result.api_result.evidence.push_back(
        {"canonical.selected_plan", result.selected_plan_uuid});
    result.api_result.evidence.push_back(
        {"canonical.time_series_root_causal_counter",
         std::to_string(execution.dispatch.root_causal_counter_id)});
    replace_source_receipt(source_execution_receipt->execution_started,
                           source_execution_receipt->data_access_observed,
                           source_execution_receipt->cleanup_count,
                           source_execution_receipt->cleanup_complete);
    append_rollup_evidence(&result.api_result);
    return result;
  }
}


}  // namespace scratchbird::engine::sblr
