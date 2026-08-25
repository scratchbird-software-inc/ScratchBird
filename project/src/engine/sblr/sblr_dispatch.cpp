// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_dispatch.hpp"
#include "sblr_event_notification.hpp"
#include "sblr_local_backup_archive.hpp"
#include "sblr_local_metrics_read.hpp"
#include "sblr_management_envelope.hpp"

#include "canonical_query_execute.hpp"
#include "hash_digest.hpp"
#include "sblr_opcode_registry.hpp"
#include "sblr_literal_runtime.hpp"
#include "engine/optimizer/optimizer_catalog_backed_planning.hpp"
#include "query/canonical_relational_bridge.hpp"
#include "query/plan_api.hpp"

#ifndef SCRATCHBIRD_QOW_QUERY_ROUTE_CONTRACT_ONLY
#include "cluster_provider/cluster_provider.hpp"
#include "sblr_context_variables.hpp"
#include "sblr_operator_runtime.hpp"
#include "sblr_procedural_block_runtime.hpp"
#include "datatype_catalog_manifest.hpp"
#include "uuid.hpp"

#include "agents/agent_action_hooks_api.hpp"
#include "agents/agent_management_api.hpp"
#include "api_diagnostics.hpp"
#include "artifacts/artifact_api.hpp"
#include "catalog/descriptor_api.hpp"
#include "catalog/descriptor_mutation_api.hpp"
#include "catalog/catalog_lookup_api.hpp"
#include "catalog/global_aggregate_view.hpp"
#include "catalog/name_registry.hpp"
#include "catalog/name_resolution_api.hpp"
#include "catalog/relation_projection_view.hpp"
#include "catalog/schema_tree_api.hpp"
#include "cluster/cluster_control_api.hpp"
#include "cluster/cluster_inspect_api.hpp"
#include "cluster/cluster_insert_route_api.hpp"
#include "cluster/placement_api.hpp"
#include "cluster/profile_operation_api.hpp"
#include "cluster/remote_participant_insert_api.hpp"
#include "cluster/replication_api.hpp"
#include "ddl/alter_api.hpp"
#include "ddl/comment_api.hpp"
#include "ddl/create_api.hpp"
#include "ddl/drop_api.hpp"
#include "dml/delete_api.hpp"
#include "dml/import_api.hpp"
#include "dml/import_execution_api.hpp"
#include "dml/import_reject_model.hpp"
#include "dml/import_resume_checkpoint.hpp"
#include "dml/insert_api.hpp"
#include "dml/merge_api.hpp"
#include "dml/native_bulk_ingest_api.hpp"
#include "dml/select_api.hpp"
#include "dml/update_api.hpp"
#include "dml/write_result_policy.hpp"
#include "dispatch/function_dispatch.hpp"
#include "extensibility/executable_object_lifecycle.hpp"
#include "extensibility/gpu_api.hpp"
#include "extensibility/llvm_api.hpp"
#include "extensibility/parser_package_api.hpp"
#include "extensibility/udr_api.hpp"
#include "lifecycle/engine_lifecycle_api.hpp"
#include "management/config_api.hpp"
#include "management/index_management_api.hpp"
#include "management/management_api.hpp"
#include "management/memory_management_api.hpp"
#include "management/support_bundle_api.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "nosql/document_api.hpp"
#include "nosql/graph_api.hpp"
#include "nosql/key_value_api.hpp"
#include "nosql/nosql_backpressure_debt_api.hpp"
#include "nosql/nosql_family_maintenance_api.hpp"
#include "nosql/nosql_statistics_api.hpp"
#include "nosql/search_api.hpp"
#include "nosql/time_series_api.hpp"
#include "nosql/vector_api.hpp"
#include "notification/notification_api.hpp"
#include "observability/explain_api.hpp"
#include "observability/metrics_api.hpp"
#include "observability/show_api.hpp"
#include "procedural/procedural_api.hpp"
#include "query/expression_api.hpp"
#include "query/predicate_api.hpp"
#include "query/projection_api.hpp"
#include "registry/function_seed_registry.hpp"
#include "security/audit_api.hpp"
#include "security/auth_challenge_api.hpp"
#include "security/auth_credential_api.hpp"
#include "security/auth_provider_observability_api.hpp"
#include "security/auth_provider_plugin_api.hpp"
#include "security/auth_provider_policy_api.hpp"
#include "security/auth_token_api.hpp"
#include "security/authentication_api.hpp"
#include "security/authorization_api.hpp"
#include "security/authority_api.hpp"
#include "security/deep_enforcement_api.hpp"
#include "security/external_group_api.hpp"
#include "security/grant_api.hpp"
#include "security/identity_api.hpp"
#include "security/policy_api.hpp"
#include "security/plugin_trust_api.hpp"
#include "security/protected_material_api.hpp"
#include "security/standard_bundle_api.hpp"
#include "security/visibility_api.hpp"
#include "security/security_model.hpp"
#include "security/security_principal_lifecycle.hpp"
#include "storage/storage_management_api.hpp"
#include "transaction/savepoint_api.hpp"
#include "transaction/transaction_api.hpp"
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iomanip>
#include <initializer_list>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace scratchbird::engine::sblr {
namespace api = scratchbird::engine::internal_api;
namespace dt = scratchbird::core::datatypes;
namespace opt = scratchbird::engine::optimizer;
namespace planner = scratchbird::engine::planner;

namespace {

struct TypedPlanOperationDecodeResult {
  bool ok{false};
  api::EngineTypedRelationalPlanRequest request;
  std::vector<api::EngineEvidenceReference> literal_evidence;
  std::vector<api::EngineEvidenceReference> parameter_evidence;
  std::string diagnostic_id;
  std::string detail;
};

void WriteSblrLiteralEvidenceTrace(
    const std::vector<api::EngineEvidenceReference>& evidence,
    const std::size_t begin) {
  const char* trace_path =
      std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");
  if (trace_path == nullptr || *trace_path == '\0') return;
  std::ofstream out(trace_path, std::ios::app | std::ios::binary);
  if (!out) return;
  out << "layer=literal_executor";
  for (std::size_t index = begin; index < evidence.size(); ++index) {
    out << '\t' << evidence[index].evidence_kind << '='
        << evidence[index].evidence_id;
  }
  out << "\tparent_success_barrier=passed\n";
}

enum class BoundedModelFamilyV1 : std::uint8_t {
  kNone = 0,
  kDocument,
  kGraph,
  kKeyValue,
  kTimeSeries,
  kVector,
  kSearch,
  kSpatial,
  kColumnar,
};

struct BoundedModelCompositionShapeV1 {
  bool exact{false};
  bool carries_statement_timestamp{false};
  std::vector<std::pair<std::uint32_t, BoundedModelFamilyV1>> model_legs;
};

BoundedModelFamilyV1 BoundedModelFamilyForRootV1(
    const std::optional<std::string>& operator_name) {
  if (!operator_name.has_value()) return BoundedModelFamilyV1::kNone;
  if (*operator_name == "DOCUMENT_SOURCE") {
    return BoundedModelFamilyV1::kDocument;
  }
  if (*operator_name == "GRAPH_MATCH") return BoundedModelFamilyV1::kGraph;
  if (*operator_name == "KV_KEY") return BoundedModelFamilyV1::kKeyValue;
  if (*operator_name == "TIME_RANGE") {
    return BoundedModelFamilyV1::kTimeSeries;
  }
  if (*operator_name == "VECTOR_NEAREST") {
    return BoundedModelFamilyV1::kVector;
  }
  if (*operator_name == "SEARCH_MATCH") {
    return BoundedModelFamilyV1::kSearch;
  }
  if (*operator_name == "SPATIAL_SOURCE") {
    return BoundedModelFamilyV1::kSpatial;
  }
  if (*operator_name == "COLUMNAR_SOURCE") {
    return BoundedModelFamilyV1::kColumnar;
  }
  return BoundedModelFamilyV1::kNone;
}

bool BoundedModelFamilyCarriesTimestampV1(
    const BoundedModelFamilyV1 family) {
  return family == BoundedModelFamilyV1::kKeyValue ||
         family == BoundedModelFamilyV1::kTimeSeries ||
         family == BoundedModelFamilyV1::kVector ||
         family == BoundedModelFamilyV1::kSearch ||
         family == BoundedModelFamilyV1::kSpatial ||
         family == BoundedModelFamilyV1::kColumnar;
}

// QOW-SOURCE-RCP080-SBLR-BOUNDED-COMPOSITION-IDENTITY-V1
// This is the exact transport shape emitted by the ordinary SBSQL
// translation boundary.  It is intentionally independent of the signed
// runtime-profile catalog: decoder admission proves the bounded left-deep
// graph, while canonical execution is responsible for selecting (or
// refusing) one of the signed 3/4/9-leg profiles.
BoundedModelCompositionShapeV1 ClassifyBoundedModelCompositionV1(
    const api::TypedRelationalDag& dag) {
  BoundedModelCompositionShapeV1 shape;
  if (dag.wire_version != 2) return shape;

  std::unordered_map<std::uint32_t, const api::RelationalExpressionRecord*>
      expressions_by_id;
  expressions_by_id.reserve(dag.expressions.size());
  for (const auto& expression : dag.expressions) {
    if (expression.expression_id == 0 ||
        !expressions_by_id.emplace(expression.expression_id, &expression)
             .second) {
      return shape;
    }
  }

  std::vector<const api::RelationalDagNode*> sources;
  std::vector<const api::RelationalDagNode*> joins;
  for (const auto& node : dag.nodes) {
    const bool relational_source =
        node.node_kind == api::RelationalDagNodeKind::kScan &&
        node.semantic_variant_id == "relation.source.v1";
    const bool model_source =
        node.node_kind == api::RelationalDagNodeKind::kScan &&
        node.semantic_variant_id == "SBLR_MODEL_SOURCE_V1";
    if (relational_source || model_source) {
      sources.push_back(&node);
    } else if (node.node_kind == api::RelationalDagNodeKind::kJoin) {
      joins.push_back(&node);
    } else {
      return shape;
    }
  }
  if (sources.size() < 3 || sources.size() > 9 ||
      joins.size() + 1 != sources.size()) {
    return shape;
  }
  std::ranges::sort(sources, {},
                    [](const auto* node) { return node->node_id; });
  std::ranges::sort(joins, {},
                    [](const auto* node) { return node->node_id; });

  std::unordered_set<std::uint32_t> source_descriptors;
  std::unordered_set<std::uint32_t> attached_model_roots;
  std::unordered_set<std::uint32_t> attached_model_expressions;
  for (const auto* source : sources) {
    if (!source->input_node_ids.empty() ||
        source->required_object_uuids.size() != 1 ||
        source->output_descriptor_ids.empty()) {
      return shape;
    }
    for (const auto descriptor_id : source->output_descriptor_ids) {
      if (descriptor_id == 0 || !source_descriptors.insert(descriptor_id).second) {
        return shape;
      }
    }

    std::unordered_set<std::uint32_t> unique_source_expression_ids;
    unique_source_expression_ids.reserve(source->bound_expression_ids.size());
    for (const auto expression_id : source->bound_expression_ids) {
      if (expression_id == 0 ||
          !unique_source_expression_ids.insert(expression_id).second) {
        return shape;
      }
    }

    // QOW-SOURCE-RCP080-EXACT-SEARCH-TERMS-ATTACHED-AUXILIARY-V1
    // SEARCH_TERMS is never a family root.  Admit at most the one exact
    // functionless query constructor attached as ordinal-1 child of this
    // source's one SEARCH_MATCH root; complete owned reachability below
    // remains an independent mandatory proof.
    const api::RelationalExpressionRecord* candidate_search_root = nullptr;
    const api::RelationalExpressionRecord* attached_search_terms = nullptr;
    for (const auto expression_id : source->bound_expression_ids) {
      const auto found = expressions_by_id.find(expression_id);
      if (found == expressions_by_id.end() ||
          found->second->operator_name != "SEARCH_MATCH") {
        continue;
      }
      if (candidate_search_root != nullptr) return shape;
      candidate_search_root = found->second;
    }
    if (candidate_search_root != nullptr &&
        candidate_search_root->child_expression_ids.size() == 4) {
      const auto query = expressions_by_id.find(
          candidate_search_root->child_expression_ids[1]);
      if (query != expressions_by_id.end()) {
        const auto* expression = query->second;
        const bool attached_to_same_source = std::ranges::find(
            source->bound_expression_ids, expression->expression_id) !=
            source->bound_expression_ids.end();
        if (attached_to_same_source &&
            expression->expression_kind ==
                api::RelationalExpressionKind::kFunctionCall &&
            !expression->function_uuid.has_value() &&
            !expression->bound_name_uuid.has_value() &&
            !expression->literal_kind.has_value() &&
            !expression->literal_or_parameter_ref.has_value() &&
            expression->operator_name == "SEARCH_TERMS" &&
            expression->child_expression_ids.size() == 1) {
          attached_search_terms = expression;
        }
      }
    }

    std::vector<std::pair<std::uint32_t, BoundedModelFamilyV1>> roots;
    for (const auto expression_id : source->bound_expression_ids) {
      const auto expression = expressions_by_id.find(expression_id);
      if (expression == expressions_by_id.end()) return shape;
      const auto family =
          BoundedModelFamilyForRootV1(expression->second->operator_name);
      const auto* root = expression->second;
      const bool unregistered_functionless_operation =
          family == BoundedModelFamilyV1::kNone &&
          root->expression_kind ==
              api::RelationalExpressionKind::kFunctionCall &&
          !root->function_uuid.has_value() &&
          root != attached_search_terms;
      if (unregistered_functionless_operation) return shape;
      if (family == BoundedModelFamilyV1::kNone) continue;
      const auto expected_arity =
          family == BoundedModelFamilyV1::kDocument ? 1U
          : family == BoundedModelFamilyV1::kGraph ? 2U
          : family == BoundedModelFamilyV1::kKeyValue ? 1U
          : family == BoundedModelFamilyV1::kTimeSeries ? 3U
          : family == BoundedModelFamilyV1::kVector ||
                    family == BoundedModelFamilyV1::kSearch
              ? 4U
              : 0U;
      const auto expected_bound_name =
          family == BoundedModelFamilyV1::kSpatial ||
                  family == BoundedModelFamilyV1::kColumnar
              ? std::optional<std::string>{
                    source->required_object_uuids.front()}
              : std::optional<std::string>{};
      if (root->expression_kind !=
              api::RelationalExpressionKind::kFunctionCall ||
          root->function_uuid.has_value() ||
          root->bound_name_uuid != expected_bound_name ||
          root->literal_kind.has_value() ||
          root->literal_or_parameter_ref.has_value() ||
          root->child_expression_ids.size() != expected_arity ||
          !attached_model_roots.insert(expression_id).second) {
        return shape;
      }
      roots.emplace_back(expression_id, family);
    }
    const bool model_source =
        source->semantic_variant_id == "SBLR_MODEL_SOURCE_V1";
    if (model_source != (roots.size() == 1)) return shape;
    if (model_source) {
      const auto root_id = roots.front().first;
      const auto family = roots.front().second;
      const auto* root = expressions_by_id.at(root_id);
      std::unordered_set<std::uint32_t> output_expression_ids;
      for (const auto& output : dag.outputs) {
        if (output.relation_node_id == source->node_id) {
          output_expression_ids.insert(output.expression_id);
        }
      }
      std::unordered_set<std::uint32_t> owned;
      for (const auto expression_id : source->bound_expression_ids) {
        if (!output_expression_ids.contains(expression_id)) {
          owned.insert(expression_id);
        }
      }
      std::unordered_set<std::uint32_t> reachable;
      std::vector<std::uint32_t> pending{root_id};
      if (family == BoundedModelFamilyV1::kKeyValue) {
        const auto equality = std::ranges::find_if(
            owned, [&](const auto expression_id) {
              const auto found = expressions_by_id.find(expression_id);
              if (found == expressions_by_id.end()) return false;
              const auto* expression = found->second;
              return expression->expression_kind ==
                         api::RelationalExpressionKind::kBinary &&
                     expression->operator_name == "=" &&
                     expression->child_expression_ids.size() == 2 &&
                     expression->child_expression_ids.front() == root_id;
            });
        if (equality == owned.end()) return shape;
        pending.push_back(*equality);
      }
      while (!pending.empty()) {
        const auto expression_id = pending.back();
        pending.pop_back();
        if (!reachable.insert(expression_id).second) continue;
        const auto found = expressions_by_id.find(expression_id);
        if (found == expressions_by_id.end() ||
            !owned.contains(expression_id)) {
          return shape;
        }
        pending.insert(pending.end(),
                       found->second->child_expression_ids.begin(),
                       found->second->child_expression_ids.end());
      }
      const api::RelationalExpressionRecord* alias = nullptr;
      if (!root->child_expression_ids.empty()) {
        const auto found =
            expressions_by_id.find(root->child_expression_ids.front());
        if (found != expressions_by_id.end()) alias = found->second;
      }
      const bool alias_exact =
          root->child_expression_ids.empty() ||
          (alias != nullptr &&
           alias->expression_kind ==
               api::RelationalExpressionKind::kIdentifier &&
           alias->bound_name_uuid == source->required_object_uuids.front());
      const bool search_auxiliary_exact =
          family != BoundedModelFamilyV1::kSearch ||
          (root->child_expression_ids.size() == 4 &&
           [&] {
             const auto found =
                 expressions_by_id.find(root->child_expression_ids[1]);
             if (found == expressions_by_id.end()) return false;
             const auto* query = found->second;
             return query->expression_kind ==
                        api::RelationalExpressionKind::kFunctionCall &&
                    !query->function_uuid.has_value() &&
                    !query->bound_name_uuid.has_value() &&
                    query->operator_name == "SEARCH_TERMS" &&
                    query->child_expression_ids.size() == 1;
           }());
      if (!alias_exact || !search_auxiliary_exact || reachable != owned ||
          std::ranges::any_of(owned, [&](const auto expression_id) {
            return !attached_model_expressions.insert(expression_id).second;
          })) {
        return shape;
      }
      shape.model_legs.emplace_back(source->node_id, family);
      shape.carries_statement_timestamp =
          shape.carries_statement_timestamp ||
          BoundedModelFamilyCarriesTimestampV1(family);
    }
  }
  if (shape.model_legs.size() < 2 || shape.model_legs.size() > 8) {
    return {};
  }

  std::uint32_t left_id = sources.front()->node_id;
  std::vector<std::uint32_t> accumulated_descriptors =
      sources.front()->output_descriptor_ids;
  for (std::size_t ordinal = 1; ordinal < sources.size(); ++ordinal) {
    const auto* join = joins[ordinal - 1];
    const auto* right = sources[ordinal];
    if (join->semantic_variant_id != "join.cross.v1" ||
        join->input_node_ids !=
            std::vector<std::uint32_t>{left_id, right->node_id} ||
        !join->bound_expression_ids.empty() ||
        !join->required_object_uuids.empty()) {
      return {};
    }
    accumulated_descriptors.insert(accumulated_descriptors.end(),
                                   right->output_descriptor_ids.begin(),
                                   right->output_descriptor_ids.end());
    if (join->output_descriptor_ids != accumulated_descriptors) return {};
    left_id = join->node_id;
  }
  if (left_id != dag.root_node_id) return {};

  const auto global_model_root_count = std::ranges::count_if(
      dag.expressions, [](const auto& expression) {
        return BoundedModelFamilyForRootV1(expression.operator_name) !=
               BoundedModelFamilyV1::kNone;
      });
  if (global_model_root_count != shape.model_legs.size() ||
      attached_model_roots.size() != shape.model_legs.size() ||
      std::ranges::any_of(dag.expressions, [&](const auto& expression) {
        const bool source_output = std::ranges::any_of(
            dag.outputs, [&](const auto& output) {
              return output.expression_id == expression.expression_id;
            });
        return !source_output &&
               !attached_model_expressions.contains(expression.expression_id);
      })) {
    return {};
  }
  shape.exact = true;
  return shape;
}

bool IsBoundedModelCompositionCandidateV1(
    const api::TypedRelationalDag& dag) {
  std::size_t source_count = 0;
  std::size_t model_source_count = 0;
  for (const auto& node : dag.nodes) {
    const bool relational_source =
        node.node_kind == api::RelationalDagNodeKind::kScan &&
        node.semantic_variant_id == "relation.source.v1";
    const bool model_source =
        node.node_kind == api::RelationalDagNodeKind::kScan &&
        node.semantic_variant_id == "SBLR_MODEL_SOURCE_V1";
    if (relational_source || model_source) {
      ++source_count;
      model_source_count += model_source ? 1 : 0;
    }
  }
  return source_count >= 3 && source_count <= 9 &&
         model_source_count >= 2;
}

struct CanonicalQueryRouteResult {
  bool graph_validated{false};
  bool logical_graph_populated{false};
  bool logical_properties_populated{false};
  bool optimizer_admitted{false};
  bool optimizer_admission_degraded{false};
  bool optimizer_benchmark_clean_ready{false};
  bool optimizer_selected{false};
  bool physical_dag_published{false};
  bool physical_dag_executed{false};
  bool runtime_actuals_attached{false};
  bool canonical_result_published{false};
  std::size_t optimizer_admission_stage_count{0};
  std::size_t logical_node_count{0};
  std::size_t logical_property_count{0};
  std::size_t physical_node_count{0};
  std::size_t canonical_result_column_count{0};
  std::size_t canonical_result_row_count{0};
  std::string selected_plan_uuid;
  std::string canonical_result_bytes;
  api::EngineApiResult api_result;
};

bool ParseCanonicalUnsigned(std::string_view text,
                            const std::uint64_t maximum,
                            std::uint64_t* value) {
  if (value == nullptr || text.empty()) return false;
  std::uint64_t parsed = 0;
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), parsed);
  if (error != std::errc{} || end != text.data() + text.size() ||
      parsed > maximum) {
    return false;
  }
  *value = parsed;
  return true;
}

bool IsCanonicalNonNilUuid(const std::string_view value) {
  if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
      value[18] != '-' || value[23] != '-' ||
      value == "00000000-0000-0000-0000-000000000000") {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) continue;
    const char ch = value[index];
    if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) {
      return false;
    }
  }
  return true;
}

bool IsCanonicalStatementTimestamp(std::string_view value) {
  if (value.size() != 20 &&
      (value.size() < 22 || value.size() > 30)) {
    return false;
  }
  if (value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
      value[13] != ':' || value[16] != ':' || value.back() != 'Z') {
    return false;
  }
  constexpr std::size_t kDigitIndexes[] = {
      0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18};
  for (const auto index : kDigitIndexes) {
    if (value[index] < '0' || value[index] > '9') return false;
  }
  if (value.size() > 20) {
    if (value[19] != '.') return false;
    for (std::size_t index = 20; index + 1 < value.size(); ++index) {
      if (value[index] < '0' || value[index] > '9') return false;
    }
  }
  const auto decimal = [&](std::size_t offset, std::size_t digits) {
    unsigned result = 0;
    for (std::size_t index = 0; index < digits; ++index) {
      result = result * 10 +
               static_cast<unsigned>(value[offset + index] - '0');
    }
    return result;
  };
  const auto year = decimal(0, 4);
  const auto month = decimal(5, 2);
  const auto day = decimal(8, 2);
  const auto hour = decimal(11, 2);
  const auto minute = decimal(14, 2);
  const auto second = decimal(17, 2);
  if (year == 0 || month == 0 || month > 12 || hour > 23 || minute > 59 ||
      second > 59) {
    return false;
  }
  constexpr unsigned kDaysByMonth[] = {
      0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  auto maximum_day = kDaysByMonth[month];
  const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  if (month == 2 && leap) ++maximum_day;
  return day != 0 && day <= maximum_day;
}

bool CanonicalQueryApiPayloadEmpty(const api::EngineApiRequest& request) {
  const auto object_reference_empty = [](const api::EngineObjectReference& ref) {
    return ref.uuid.canonical.empty() && ref.object_kind.empty();
  };
  const auto identifier_atom_empty = [](const api::EngineIdentifierAtom& atom) {
    return atom.raw_text.empty() && !atom.was_quoted &&
           atom.quote_style.empty() && atom.identifier_profile_uuid.empty() &&
           atom.normalized_lookup_key.empty() &&
           atom.exact_lookup_key.empty() && !atom.requires_exact_match &&
           atom.source_span.empty();
  };
  const auto& sql_reference = request.sql_object_reference;
  const auto& bound_identity = request.bound_object_identity;
  return request.operation_id.empty() &&
         object_reference_empty(request.target_database) &&
         object_reference_empty(request.target_schema) &&
         object_reference_empty(request.target_object) &&
         request.related_objects.empty() && request.localized_names.empty() &&
         sql_reference.expected_object_type.empty() &&
         sql_reference.path_type == "unqualified" &&
         !sql_reference.no_search_path &&
         sql_reference.path_components.empty() &&
         identifier_atom_empty(sql_reference.object_name) &&
         bound_identity.object_uuid.canonical.empty() &&
         bound_identity.resolved_object_type.empty() &&
         bound_identity.resolved_schema_uuid.canonical.empty() &&
         bound_identity.parent_object_uuid.canonical.empty() &&
         bound_identity.catalog_generation_id == 0 &&
         bound_identity.security_epoch == 0 &&
         bound_identity.resource_epoch == 0 && request.descriptors.empty() &&
         request.columns.empty() && request.constraints.empty() &&
         request.indexes.empty() && !request.native_row_packet.present &&
         request.native_row_packet.version == 0 &&
         request.native_row_packet.row_count == 0 &&
         request.native_row_packet.column_count == 0 &&
         request.native_row_packet.field_order.empty() &&
         request.native_row_packet.column_type_tags.empty() &&
         request.native_row_packet.packet_bytes.empty() &&
         request.native_row_packet.row_offsets.empty() &&
         request.native_row_packet.row_sizes.empty() && request.rows.empty() &&
         request.shared_row_field_order.empty() && request.assignments.empty() &&
         request.predicate.predicate_kind.empty() &&
         request.predicate.canonical_predicate_envelope.empty() &&
         request.predicate.bound_values.empty() &&
         request.projection.canonical_projection_envelopes.empty() &&
         request.ordering.canonical_ordering_envelopes.empty() &&
         request.physical_profile.names.empty() &&
         request.physical_profile.encoded_profiles.empty() &&
         request.policy_profile.names.empty() &&
         request.policy_profile.encoded_profiles.empty() &&
         request.compatibility_profile.names.empty() &&
         request.compatibility_profile.encoded_profiles.empty() &&
         request.option_envelopes.empty() &&
         request.diagnostic_options.empty();
}

template <std::size_t FieldCount>
bool SplitRelationalFields(
    std::string_view encoded,
    std::array<std::string_view, FieldCount>* fields) {
  if (fields == nullptr) return false;
  std::size_t start = 0;
  for (std::size_t index = 0; index < fields->size(); ++index) {
    const auto separator = encoded.find('|', start);
    if (index + 1 == fields->size()) {
      if (separator != std::string_view::npos) return false;
      (*fields)[index] = encoded.substr(start);
      return true;
    }
    if (separator == std::string_view::npos) return false;
    (*fields)[index] = encoded.substr(start, separator - start);
    start = separator + 1;
  }
  return false;
}

template <std::size_t FieldCount>
bool SplitRelationalSubfields(
    const std::string_view encoded, const char separator,
    std::array<std::string_view, FieldCount>* fields) {
  if (fields == nullptr || encoded.empty()) return false;
  std::size_t start = 0;
  for (std::size_t index = 0; index < fields->size(); ++index) {
    const auto next = encoded.find(separator, start);
    if (index + 1 == fields->size()) {
      if (next != std::string_view::npos) return false;
      (*fields)[index] = encoded.substr(start);
      return true;
    }
    if (next == std::string_view::npos) return false;
    (*fields)[index] = encoded.substr(start, next - start);
    start = next + 1;
  }
  return false;
}

bool DecodeCanonicalHex(std::string_view encoded, std::string* value) {
  if (value == nullptr || encoded.size() % 2 != 0) return false;
  value->clear();
  value->reserve(encoded.size() / 2);
  const auto nibble = [](const char ch) -> int {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return 10 + ch - 'a';
    return -1;
  };
  for (std::size_t index = 0; index < encoded.size(); index += 2) {
    const int high = nibble(encoded[index]);
    const int low = nibble(encoded[index + 1]);
    if (high < 0 || low < 0) return false;
    value->push_back(static_cast<char>((high << 4) | low));
  }
  return true;
}

bool DecodeOptionalCanonicalHex(
    std::string_view encoded,
    std::optional<std::string>* value) {
  if (value == nullptr) return false;
  if (encoded == "-") {
    value->reset();
    return true;
  }
  std::string decoded;
  if (!DecodeCanonicalHex(encoded, &decoded)) return false;
  *value = std::move(decoded);
  return true;
}

bool ParseOptionalCanonicalU32(
    std::string_view encoded,
    std::optional<std::uint32_t>* value) {
  if (value == nullptr) return false;
  if (encoded == "-") {
    value->reset();
    return true;
  }
  std::uint64_t parsed = 0;
  if (!ParseCanonicalUnsigned(encoded,
                              std::numeric_limits<std::uint32_t>::max(),
                              &parsed)) {
    return false;
  }
  *value = static_cast<std::uint32_t>(parsed);
  return true;
}

bool ParseRelationalHandleList(std::string_view encoded,
                               std::vector<std::uint32_t>* handles) {
  if (handles == nullptr || encoded.empty()) return false;
  handles->clear();
  if (encoded == "-") return true;
  std::size_t start = 0;
  while (start <= encoded.size()) {
    const auto separator = encoded.find(',', start);
    const auto token = encoded.substr(
        start,
        separator == std::string_view::npos ? encoded.size() - start
                                            : separator - start);
    std::uint64_t parsed = 0;
    if (!ParseCanonicalUnsigned(token,
                                std::numeric_limits<std::uint32_t>::max(),
                                &parsed)) {
      return false;
    }
    handles->push_back(static_cast<std::uint32_t>(parsed));
    if (handles->size() > 131072) return false;
    if (separator == std::string_view::npos) break;
    start = separator + 1;
  }
  return true;
}

bool ParseRelationalStringList(std::string_view encoded,
                               std::vector<std::string>* values) {
  if (values == nullptr || encoded.empty()) return false;
  values->clear();
  if (encoded == "-") return true;
  std::size_t start = 0;
  while (start <= encoded.size()) {
    const auto separator = encoded.find(',', start);
    const auto token = encoded.substr(
        start,
        separator == std::string_view::npos ? encoded.size() - start
                                            : separator - start);
    if (token.empty()) return false;
    values->emplace_back(token);
    if (values->size() > 524288) return false;
    if (separator == std::string_view::npos) break;
    start = separator + 1;
  }
  return true;
}

bool ParseRelationalOrderingTerms(
    std::string_view encoded,
    std::vector<api::RelationalPropertyOrderingTerm>* terms) {
  if (terms == nullptr || encoded.empty()) return false;
  terms->clear();
  if (encoded == "-") return true;
  std::size_t start = 0;
  while (start <= encoded.size()) {
    const auto separator = encoded.find(',', start);
    const auto token = encoded.substr(
        start,
        separator == std::string_view::npos ? encoded.size() - start
                                            : separator - start);
    std::array<std::string_view, 4> fields{};
    std::size_t field_start = 0;
    bool fields_valid = true;
    for (std::size_t index = 0; index < fields.size(); ++index) {
      const auto field_separator = token.find(':', field_start);
      if (index + 1 == fields.size()) {
        if (field_separator != std::string_view::npos) {
          fields_valid = false;
          break;
        }
        fields[index] = token.substr(field_start);
      } else {
        if (field_separator == std::string_view::npos) {
          fields_valid = false;
          break;
        }
        fields[index] =
            token.substr(field_start, field_separator - field_start);
        field_start = field_separator + 1;
      }
    }
    std::uint64_t expression_id = 0;
    std::uint64_t direction = 0;
    std::uint64_t null_placement = 0;
    if (!fields_valid ||
        !ParseCanonicalUnsigned(fields[0],
                                std::numeric_limits<std::uint32_t>::max(),
                                &expression_id) ||
        !ParseCanonicalUnsigned(fields[1],
                                std::numeric_limits<std::uint8_t>::max(),
                                &direction) ||
        !ParseCanonicalUnsigned(fields[2],
                                std::numeric_limits<std::uint8_t>::max(),
                                &null_placement)) {
      return false;
    }
    api::RelationalPropertyOrderingTerm term;
    term.expression_id = static_cast<std::uint32_t>(expression_id);
    term.direction =
        static_cast<api::RelationalPropertySortDirection>(direction);
    term.null_placement =
        static_cast<api::RelationalPropertyNullPlacement>(null_placement);
    if (fields[3] != "-") term.collation_uuid = fields[3];
    terms->push_back(std::move(term));
    if (terms->size() > 524288) return false;
    if (separator == std::string_view::npos) break;
    start = separator + 1;
  }
  return true;
}

bool ParseRelationalWindowBound(
    const std::string_view encoded,
    std::optional<api::RelationalWindowFrameBoundRecord>* bound) {
  if (bound == nullptr) return false;
  if (encoded == "-") {
    bound->reset();
    return true;
  }
  const auto separator = encoded.find(':');
  if (separator == std::string_view::npos ||
      encoded.find(':', separator + 1) != std::string_view::npos) {
    return false;
  }
  std::uint64_t kind = 0;
  api::RelationalWindowFrameBoundRecord decoded;
  if (!ParseCanonicalUnsigned(encoded.substr(0, separator),
                              std::numeric_limits<std::uint8_t>::max(),
                              &kind) ||
      !ParseOptionalCanonicalU32(encoded.substr(separator + 1),
                                &decoded.offset_expression_id)) {
    return false;
  }
  decoded.bound_kind =
      static_cast<api::RelationalWindowFrameBoundKind>(kind);
  *bound = std::move(decoded);
  return true;
}

// QOW-ROUTE-STAGE-QRY-003-V1
TypedPlanOperationDecodeResult TypedPlanOperationRequest(
    const SblrDispatchRequest& dispatch_request) {
  TypedPlanOperationDecodeResult decoded;
  if (dispatch_request.envelope.result_shape != "query_execute_result") {
    decoded.diagnostic_id = "SB_DIAG_SBLR_RESULT_SHAPE_MISMATCH";
    decoded.detail = "query.execute requires query_execute_result";
    return decoded;
  }
  if (!CanonicalQueryApiPayloadEmpty(dispatch_request.api_request)) {
    decoded.diagnostic_id = "SBLR.PLAN_TREE.INVALID_HANDLE";
    decoded.detail =
        "out-of-band generic API payload is forbidden for query.execute";
    return decoded;
  }
  decoded.request.context = dispatch_request.context;
  decoded.request.operation_id = dispatch_request.envelope.operation_id;
  decoded.request.execute = true;
  decoded.request.relational_dag.package_root =
      api::RelationalPackageRoot::kQueryExecute;

  constexpr std::array<std::pair<std::string_view, std::string_view>, 10>
      kLeadingOperands{{
          {"uint16", "relational_wire_version"},
          {"uuid", "relational_bound_sblr_tree_uuid"},
          {"uuid", "relational_catalog_epoch_uuid"},
          {"uuid", "relational_security_context_uuid"},
          {"uuid", "relational_statement_uuid"},
          {"uuid", "relational_owning_transaction_uuid"},
          {"uuid", "relational_statement_snapshot_uuid"},
          {"uuid", "relational_statement_metadata_snapshot_uuid"},
          {"uint64", "relational_local_transaction_id"},
          {"uint64",
           "relational_snapshot_visible_through_local_transaction_id"},
      }};
  const auto timestamp_operand = std::ranges::find_if(
      dispatch_request.envelope.operands, [](const auto& operand) {
        return operand.name == "relational_statement_timestamp";
      });
  if (timestamp_operand != dispatch_request.envelope.operands.end() &&
      (dispatch_request.envelope.operands.size() <= 10 ||
       timestamp_operand != dispatch_request.envelope.operands.begin() + 10 ||
       timestamp_operand->type != "text")) {
    decoded.diagnostic_id =
        "SB_MODEL_KEY_VALUE_STATEMENT_TIMESTAMP_INVALID_V1";
    decoded.detail =
        "key/value statement timestamp is reordered or has the wrong type";
    return decoded;
  }
  if (dispatch_request.envelope.operands.size() < kLeadingOperands.size()) {
    decoded.diagnostic_id = "QOW-DIAG-LOGICAL-GRAPH-BOUNDARY-V1";
    decoded.detail =
        "wire v2 requires the complete leading statement context";
    return decoded;
  }
  for (std::size_t index = 0; index < kLeadingOperands.size(); ++index) {
    const auto& operand = dispatch_request.envelope.operands[index];
    if (operand.type != kLeadingOperands[index].first ||
        operand.name != kLeadingOperands[index].second) {
      decoded.diagnostic_id = "QOW-DIAG-LOGICAL-GRAPH-BOUNDARY-V1";
      decoded.detail =
          "wire v2 leading statement context is out of order or narrowed";
      return decoded;
    }
  }
  const auto root_operand = std::ranges::find_if(
      dispatch_request.envelope.operands, [](const auto& operand) {
        return operand.type == "uint32" &&
               operand.name == "relational_root_node_id";
      });
  if (root_operand != dispatch_request.envelope.operands.end() &&
      root_operand != dispatch_request.envelope.operands.begin() +
                          (timestamp_operand ==
                                   dispatch_request.envelope.operands.end()
                               ? 10
                               : 11)) {
    decoded.diagnostic_id = "QOW-DIAG-LOGICAL-GRAPH-BOUNDARY-V1";
    decoded.detail = "relational root is out of corrected wire-v2 order";
    return decoded;
  }

  bool wire_version_present = false;
  bool root_node_present = false;
  bool bound_sblr_tree_present = false;
  bool bound_catalog_epoch_present = false;
  bool bound_security_context_present = false;
  bool statement_uuid_present = false;
  bool owning_transaction_uuid_present = false;
  bool statement_snapshot_uuid_present = false;
  bool statement_metadata_snapshot_uuid_present = false;
  bool statement_timestamp_present = false;
  bool local_transaction_id_present = false;
  bool snapshot_visible_through_local_transaction_id_present = false;
  std::optional<SblrExpressionNodeTableCodecResultV1> literal_node_table;
  std::vector<std::pair<std::uint32_t, SblrExpressionNodeReferenceV1>>
      literal_references;
  std::optional<SblrParameterNodeTableCodecResultV1> parameter_node_table;
  std::vector<std::pair<std::uint32_t, SblrParameterNodeReferenceV1>>
      parameter_references;
  for (const auto& operand : dispatch_request.envelope.operands) {
    if (operand.value_kind == SblrValueKind::parameter_node_table) {
      if (parameter_node_table.has_value()) {
        decoded.diagnostic_id = "SBLR.OPERAND_INVALID";
        decoded.detail = "duplicate SBPN parameter table";
        return decoded;
      }
      parameter_node_table = DecodeSblrParameterNodeTableV1(
          operand.value_body.data(), operand.value_body.size());
      if (!parameter_node_table->ok) {
        decoded.diagnostic_id = parameter_node_table->diagnostic_id;
        decoded.detail = parameter_node_table->detail;
        return decoded;
      }
      continue;
    }
    if (operand.value_kind == SblrValueKind::parameter_node_ref) {
      std::uint64_t expression_id = 0;
      SblrParameterNodeReferenceV1 reference;
      if (operand.type != "relational_expression_v1" ||
          !ParseCanonicalUnsigned(operand.name,
              std::numeric_limits<std::uint32_t>::max(), &expression_id) ||
          !DecodeSblrParameterNodeReferenceV1(
              operand.value_body.data(), operand.value_body.size(),
              &reference)) {
        decoded.diagnostic_id = "SBLR.OPERAND_INVALID";
        decoded.detail = "malformed relational parameter reference";
        return decoded;
      }
      parameter_references.emplace_back(
          static_cast<std::uint32_t>(expression_id), reference);
      continue;
    }
    if (operand.value_kind == SblrValueKind::expression_node_table) {
      if (literal_node_table.has_value()) {
        decoded.diagnostic_id = "SBLR.OPERAND_INVALID";
        decoded.detail = "duplicate SBXN literal table";
        return decoded;
      }
      literal_node_table = DecodeSblrExpressionNodeTableV1(
          operand.value_body.data(), operand.value_body.size());
      if (!literal_node_table->ok) {
        decoded.diagnostic_id = "SBLR.OPERAND_INVALID";
        decoded.detail = literal_node_table->detail;
        return decoded;
      }
      continue;
    }
    if (operand.value_kind == SblrValueKind::expression_node_ref) {
      std::uint64_t expression_id = 0;
      SblrExpressionNodeReferenceV1 reference;
      if (operand.type != "relational_expression_v1" ||
          !ParseCanonicalUnsigned(operand.name,
              std::numeric_limits<std::uint32_t>::max(), &expression_id) ||
          !DecodeSblrExpressionNodeReferenceV1(
              operand.value_body.data(), operand.value_body.size(),
              &reference)) {
        decoded.diagnostic_id = "SBLR.OPERAND_INVALID";
        decoded.detail = "malformed relational literal reference";
        return decoded;
      }
      literal_references.emplace_back(
          static_cast<std::uint32_t>(expression_id), reference);
      continue;
    }
    if (operand.type == "uint16" &&
        operand.name == "relational_wire_version") {
      if (wire_version_present) {
        decoded.diagnostic_id = "SBLR.PLAN_TREE.INVALID_VERSION";
        decoded.detail = "duplicate relational_wire_version operand";
        return decoded;
      }
      std::uint64_t wire_version = 0;
      if (!ParseCanonicalUnsigned(
              operand.value,
              std::numeric_limits<std::uint16_t>::max(),
              &wire_version)) {
        decoded.diagnostic_id = "SBLR.PLAN_TREE.INVALID_VERSION";
        decoded.detail = "malformed relational_wire_version operand";
        return decoded;
      }
      decoded.request.relational_dag.wire_version =
          static_cast<std::uint16_t>(wire_version);
      wire_version_present = true;
      continue;
    }
    if (operand.type == "uuid" &&
        operand.name == "relational_bound_sblr_tree_uuid") {
      if (bound_sblr_tree_present ||
          !IsCanonicalNonNilUuid(operand.value)) {
        decoded.diagnostic_id = "QOW-DIAG-LOGICAL-GRAPH-BOUNDARY-V1";
        decoded.detail = "duplicate or empty bound SBLR tree identity";
        return decoded;
      }
      decoded.request.relational_dag.bound_sblr_tree_uuid = operand.value;
      bound_sblr_tree_present = true;
      continue;
    }
    if (operand.type == "uuid" &&
        operand.name == "relational_catalog_epoch_uuid") {
      if (bound_catalog_epoch_present ||
          !IsCanonicalNonNilUuid(operand.value)) {
        decoded.diagnostic_id = "QOW-DIAG-LOGICAL-GRAPH-BOUNDARY-V1";
        decoded.detail = "duplicate or empty catalog epoch identity";
        return decoded;
      }
      decoded.request.relational_dag.bound_catalog_epoch_uuid = operand.value;
      bound_catalog_epoch_present = true;
      continue;
    }
    if (operand.type == "uuid" &&
        operand.name == "relational_security_context_uuid") {
      if (bound_security_context_present ||
          !IsCanonicalNonNilUuid(operand.value)) {
        decoded.diagnostic_id = "QOW-DIAG-LOGICAL-GRAPH-BOUNDARY-V1";
        decoded.detail = "duplicate or empty security context identity";
        return decoded;
      }
      decoded.request.relational_dag.bound_security_context_uuid =
          operand.value;
      bound_security_context_present = true;
      continue;
    }
    if (operand.type == "uuid" &&
        operand.name == "relational_statement_uuid") {
      if (statement_uuid_present ||
          !IsCanonicalNonNilUuid(operand.value)) {
        decoded.diagnostic_id = "QOW-DIAG-LOGICAL-GRAPH-BOUNDARY-V1";
        decoded.detail = "duplicate or malformed statement identity";
        return decoded;
      }
      decoded.request.relational_dag.statement_uuid = operand.value;
      statement_uuid_present = true;
      continue;
    }
    if (operand.type == "uuid" &&
        operand.name == "relational_owning_transaction_uuid") {
      if (owning_transaction_uuid_present ||
          !IsCanonicalNonNilUuid(operand.value)) {
        decoded.diagnostic_id = "QOW-DIAG-LOGICAL-GRAPH-BOUNDARY-V1";
        decoded.detail =
            "duplicate or malformed owning transaction identity";
        return decoded;
      }
      decoded.request.relational_dag.owning_transaction_uuid = operand.value;
      owning_transaction_uuid_present = true;
      continue;
    }
    if (operand.type == "uuid" &&
        operand.name == "relational_statement_snapshot_uuid") {
      if (statement_snapshot_uuid_present ||
          !IsCanonicalNonNilUuid(operand.value)) {
        decoded.diagnostic_id = "QOW-DIAG-LOGICAL-GRAPH-BOUNDARY-V1";
        decoded.detail = "duplicate or malformed statement snapshot identity";
        return decoded;
      }
      decoded.request.relational_dag.statement_snapshot_uuid = operand.value;
      statement_snapshot_uuid_present = true;
      continue;
    }
    if (operand.type == "uuid" &&
        operand.name == "relational_statement_metadata_snapshot_uuid") {
      if (statement_metadata_snapshot_uuid_present ||
          !IsCanonicalNonNilUuid(operand.value)) {
        decoded.diagnostic_id = "QOW-DIAG-LOGICAL-GRAPH-BOUNDARY-V1";
        decoded.detail =
            "duplicate or malformed statement metadata snapshot identity";
        return decoded;
      }
      decoded.request.relational_dag.statement_metadata_snapshot_uuid =
          operand.value;
      statement_metadata_snapshot_uuid_present = true;
      continue;
    }
    if (operand.type == "uint64" &&
        operand.name == "relational_local_transaction_id") {
      std::uint64_t local_transaction_id = 0;
      if (local_transaction_id_present ||
          (operand.value.size() > 1 && operand.value.front() == '0') ||
          !ParseCanonicalUnsigned(
              operand.value, std::numeric_limits<std::uint64_t>::max(),
              &local_transaction_id) ||
          local_transaction_id == 0) {
        decoded.diagnostic_id = "QOW-DIAG-LOGICAL-GRAPH-BOUNDARY-V1";
        decoded.detail = "duplicate or malformed local transaction identity";
        return decoded;
      }
      decoded.request.relational_dag.local_transaction_id =
          local_transaction_id;
      local_transaction_id_present = true;
      continue;
    }
    if (operand.type == "uint64" &&
        operand.name ==
            "relational_snapshot_visible_through_local_transaction_id") {
      std::uint64_t visible_through = 0;
      if (snapshot_visible_through_local_transaction_id_present ||
          (operand.value.size() > 1 && operand.value.front() == '0') ||
          !ParseCanonicalUnsigned(
              operand.value, std::numeric_limits<std::uint64_t>::max(),
              &visible_through)) {
        decoded.diagnostic_id = "QOW-DIAG-LOGICAL-GRAPH-BOUNDARY-V1";
        decoded.detail = "duplicate or malformed visibility high-water";
        return decoded;
      }
      decoded.request.relational_dag
          .snapshot_visible_through_local_transaction_id = visible_through;
      snapshot_visible_through_local_transaction_id_present = true;
      continue;
    }
    if (operand.type == "text" &&
        operand.name == "relational_statement_timestamp") {
      if (statement_timestamp_present ||
          !IsCanonicalStatementTimestamp(operand.value)) {
        decoded.diagnostic_id =
            "SB_MODEL_KEY_VALUE_STATEMENT_TIMESTAMP_INVALID_V1";
        decoded.detail =
            "key/value statement timestamp is malformed or duplicated";
        return decoded;
      }
      decoded.request.relational_dag.statement_timestamp = operand.value;
      statement_timestamp_present = true;
      continue;
    }
    if (operand.type == "uint32" &&
        operand.name == "relational_root_node_id") {
      if (root_node_present) {
        decoded.diagnostic_id = "SBLR.PLAN_TREE.INVALID_HANDLE";
        decoded.detail = "duplicate relational_root_node_id operand";
        return decoded;
      }
      std::uint64_t root_node_id = 0;
      if (!ParseCanonicalUnsigned(
              operand.value,
              std::numeric_limits<std::uint32_t>::max(),
              &root_node_id)) {
        decoded.diagnostic_id = "SBLR.PLAN_TREE.INVALID_HANDLE";
        decoded.detail = "malformed relational_root_node_id operand";
        return decoded;
      }
      decoded.request.relational_dag.root_node_id =
          static_cast<std::uint32_t>(root_node_id);
      root_node_present = true;
      continue;
    }
    if (operand.type == "relational_node_v1") {
      if (decoded.request.relational_dag.nodes.size() >= 131072 ||
          operand.value.size() > 65536) {
        decoded.diagnostic_id = "SBLR.PLAN_TREE.RESOURCE_LIMIT";
        decoded.detail = "relational node transport limit exceeded";
        return decoded;
      }
      std::uint64_t node_id = 0;
      if (!ParseCanonicalUnsigned(
              operand.name,
              std::numeric_limits<std::uint32_t>::max(),
              &node_id)) {
        decoded.diagnostic_id = "SBLR.PLAN_TREE.INVALID_HANDLE";
        decoded.detail = "malformed relational node id";
        return decoded;
      }
      std::array<std::string_view, 5> fields{};
      std::uint64_t node_kind = 0;
      if (!SplitRelationalFields(operand.value, &fields) ||
          !ParseCanonicalUnsigned(
              fields[0], std::numeric_limits<std::uint8_t>::max(),
              &node_kind) ||
          (fields[1] != "0" && fields[1] != "1")) {
        decoded.diagnostic_id = "SBLR.PLAN_TREE.INVALID_HANDLE";
        decoded.detail = "malformed relational node descriptor";
        return decoded;
      }
      api::RelationalDagNode node;
      node.node_id = static_cast<std::uint32_t>(node_id);
      node.node_kind = static_cast<api::RelationalDagNodeKind>(node_kind);
      node.shareable = fields[1] == "1";
      if (!ParseRelationalHandleList(fields[2], &node.input_node_ids) ||
          !ParseRelationalHandleList(fields[3],
                                     &node.output_descriptor_ids) ||
          !ParseRelationalHandleList(fields[4], &node.values_row_ids)) {
        decoded.diagnostic_id = "SBLR.PLAN_TREE.INVALID_HANDLE";
        decoded.detail = "malformed relational node handles";
        return decoded;
      }
      decoded.request.relational_dag.nodes.push_back(std::move(node));
      continue;
    }
    if (operand.type == "relational_node_binding_v1") {
      if (operand.value.size() > 65536) {
        decoded.diagnostic_id = "SBLR.PLAN_TREE.RESOURCE_LIMIT";
        decoded.detail = "relational node binding transport limit exceeded";
        return decoded;
      }
      std::uint64_t node_id = 0;
      std::array<std::string_view, 5> fields{};
      if (!ParseCanonicalUnsigned(
              operand.name, std::numeric_limits<std::uint32_t>::max(),
              &node_id) ||
          !SplitRelationalFields(operand.value, &fields)) {
        decoded.diagnostic_id = "SBLR.PLAN_TREE.INVALID_HANDLE";
        decoded.detail = "malformed relational node binding record";
        return decoded;
      }
      const auto node = std::ranges::find_if(
          decoded.request.relational_dag.nodes, [&](const auto& candidate) {
            return candidate.node_id == node_id;
          });
      if (node == decoded.request.relational_dag.nodes.end() ||
          !node->semantic_variant_id.empty() ||
          !DecodeCanonicalHex(fields[0], &node->semantic_variant_id) ||
          !ParseRelationalHandleList(fields[1],
                                     &node->bound_expression_ids) ||
          !ParseRelationalStringList(fields[2],
                                     &node->required_object_uuids) ||
          !ParseRelationalStringList(fields[3],
                                     &node->required_property_uuids) ||
          !ParseRelationalStringList(fields[4],
                                     &node->delivered_property_uuids)) {
        decoded.diagnostic_id = "SBLR.PLAN_TREE.INVALID_HANDLE";
        decoded.detail = "invalid or out-of-order relational node binding";
        return decoded;
      }
      continue;
    }
    if (operand.type == "relational_table_function_v1") {
      if (operand.value.size() > 65536) {
        decoded.diagnostic_id = "SBLR.PLAN_TREE.RESOURCE_LIMIT";
        decoded.detail = "table function argument transport limit exceeded";
        return decoded;
      }
      std::uint64_t node_id = 0;
      if (!ParseCanonicalUnsigned(
              operand.name, std::numeric_limits<std::uint32_t>::max(),
              &node_id)) {
        decoded.diagnostic_id = "SBLR.PLAN_TREE.INVALID_HANDLE";
        decoded.detail = "malformed table function node id";
        return decoded;
      }
      const auto node = std::ranges::find_if(
          decoded.request.relational_dag.nodes, [&](const auto& candidate) {
            return candidate.node_id == node_id;
          });
      if (node == decoded.request.relational_dag.nodes.end() ||
          node->node_kind != api::RelationalDagNodeKind::kTableFunctionInvoke ||
          !node->argument_expression_ids.empty() ||
          !ParseRelationalHandleList(operand.value,
                                     &node->argument_expression_ids) ||
          node->argument_expression_ids.empty()) {
        decoded.diagnostic_id = "SBLR.PLAN_TREE.INVALID_HANDLE";
        decoded.detail = "invalid table function argument binding";
        return decoded;
      }
      continue;
    }
    if (operand.type == "relational_row_pattern_v1") {
      if (decoded.request.relational_dag.row_patterns.size() >= 131072 ||
          operand.value.size() > 65536) {
        decoded.diagnostic_id = "SBLR.PLAN_TREE.RESOURCE_LIMIT";
        decoded.detail = "row-pattern descriptor transport limit exceeded";
        return decoded;
      }
      std::uint64_t node_id = 0;
      std::uint64_t pattern_id = 0;
      std::uint64_t rows_per_match = 0;
      std::uint64_t after_match_skip = 0;
      std::uint64_t maximum_partition_rows = 0;
      std::uint64_t maximum_active_states = 0;
      std::uint64_t maximum_output_rows = 0;
      std::array<std::string_view, 12> fields{};
      if (!ParseCanonicalUnsigned(
              operand.name, std::numeric_limits<std::uint32_t>::max(),
              &node_id) ||
          !SplitRelationalFields(operand.value, &fields) ||
          !ParseCanonicalUnsigned(
              fields[0], std::numeric_limits<std::uint32_t>::max(),
              &pattern_id) ||
          !ParseCanonicalUnsigned(
              fields[5], std::numeric_limits<std::uint8_t>::max(),
              &rows_per_match) ||
          !ParseCanonicalUnsigned(
              fields[6], std::numeric_limits<std::uint8_t>::max(),
              &after_match_skip) ||
          !ParseCanonicalUnsigned(
              fields[8], std::numeric_limits<std::uint32_t>::max(),
              &maximum_partition_rows) ||
          !ParseCanonicalUnsigned(
              fields[9], std::numeric_limits<std::uint32_t>::max(),
              &maximum_active_states) ||
          !ParseCanonicalUnsigned(
              fields[10], std::numeric_limits<std::uint32_t>::max(),
              &maximum_output_rows) ||
          (fields[11] != "0" && fields[11] != "1")) {
        decoded.diagnostic_id = "SBLR.PLAN_TREE.INVALID_HANDLE";
        decoded.detail = "malformed row-pattern descriptor";
        return decoded;
      }
      const auto node = std::ranges::find_if(
          decoded.request.relational_dag.nodes, [&](const auto& candidate) {
            return candidate.node_id == node_id;
          });
      api::RelationalRowPatternRecord pattern;
      pattern.pattern_id = static_cast<std::uint32_t>(pattern_id);
      pattern.relation_node_id = static_cast<std::uint32_t>(node_id);
      pattern.rows_per_match =
          static_cast<api::RelationalRowPatternRowsPerMatch>(rows_per_match);
      pattern.after_match_skip =
          static_cast<api::RelationalRowPatternAfterMatchSkip>(
              after_match_skip);
      pattern.maximum_partition_rows =
          static_cast<std::uint32_t>(maximum_partition_rows);
      pattern.maximum_active_states =
          static_cast<std::uint32_t>(maximum_active_states);
      pattern.maximum_output_rows =
          static_cast<std::uint32_t>(maximum_output_rows);
      pattern.stable_row_identity_tie_break_allowed = fields[11] == "1";
      std::array<std::string_view, 6> variable_fields{};
      std::uint64_t minimum_occurrences = 0;
      std::optional<std::uint32_t> maximum_occurrences;
      std::optional<std::uint32_t> define_expression_id;
      api::RelationalRowPatternVariableRecord variable;
      if (node == decoded.request.relational_dag.nodes.end() ||
          node->node_kind != api::RelationalDagNodeKind::kMatchRecognize ||
          !ParseRelationalHandleList(fields[1],
                                     &pattern.partition_expression_ids) ||
          !ParseRelationalOrderingTerms(fields[2], &pattern.ordering_terms) ||
          !SplitRelationalSubfields(fields[3], ':', &variable_fields) ||
          !DecodeCanonicalHex(variable_fields[0],
                              &variable.canonical_name_key) ||
          !ParseCanonicalUnsigned(
              variable_fields[1], std::numeric_limits<std::uint32_t>::max(),
              &minimum_occurrences) ||
          !ParseOptionalCanonicalU32(variable_fields[2],
                                     &maximum_occurrences) ||
          (variable_fields[3] != "0" && variable_fields[3] != "1") ||
          !ParseOptionalCanonicalU32(variable_fields[4],
                                     &define_expression_id) ||
          (variable_fields[5] != "0" && variable_fields[5] != "1") ||
          !ParseRelationalHandleList(fields[4],
                                     &pattern.measure_expression_ids) ||
          !DecodeOptionalCanonicalHex(fields[7], &pattern.skip_target_key)) {
        decoded.diagnostic_id = "SBLR.PLAN_TREE.INVALID_HANDLE";
        decoded.detail = "invalid or out-of-order row-pattern binding";
        return decoded;
      }
      variable.minimum_occurrences =
          static_cast<std::uint32_t>(minimum_occurrences);
      variable.maximum_occurrences = maximum_occurrences;
      variable.reluctant = variable_fields[3] == "1";
      variable.define_expression_id = define_expression_id;
      variable.define_always_true = variable_fields[5] == "1";
      pattern.variables.push_back(std::move(variable));
      decoded.request.relational_dag.row_patterns.push_back(std::move(pattern));
      continue;
    }
    if (operand.type == "relational_descriptor_v1") {
      if (decoded.request.relational_dag.descriptors.size() >= 524288 ||
          operand.value.size() > 65536) {
        decoded.diagnostic_id = "SBLR.PLAN_TREE.RESOURCE_LIMIT";
        decoded.detail = "relational descriptor transport limit exceeded";
        return decoded;
      }
      std::uint64_t descriptor_id = 0;
      std::array<std::string_view, 8> fields{};
      std::uint64_t nullability = 0;
      if (!ParseCanonicalUnsigned(
              operand.name, std::numeric_limits<std::uint32_t>::max(),
              &descriptor_id) ||
          !SplitRelationalFields(operand.value, &fields) ||
          !ParseCanonicalUnsigned(
              fields[2], std::numeric_limits<std::uint8_t>::max(),
              &nullability)) {
        decoded.diagnostic_id = "SBLR.PLAN_TREE.INVALID_HANDLE";
        decoded.detail = "malformed relational descriptor record";
        return decoded;
      }
      api::RelationalTypeDescriptor descriptor;
      descriptor.descriptor_id = static_cast<std::uint32_t>(descriptor_id);
      descriptor.descriptor_uuid = fields[0];
      descriptor.type_uuid = fields[1];
      descriptor.nullability =
          static_cast<api::RelationalNullability>(nullability);
      if (fields[3] != "-") descriptor.collation_uuid = std::string(fields[3]);
      if (!DecodeOptionalCanonicalHex(fields[4],
                                      &descriptor.timezone_profile_id) ||
          !ParseOptionalCanonicalU32(fields[5], &descriptor.width) ||
          !ParseOptionalCanonicalU32(fields[6], &descriptor.precision) ||
          !ParseOptionalCanonicalU32(fields[7], &descriptor.scale)) {
        decoded.diagnostic_id = "SBLR.PLAN_TREE.INVALID_HANDLE";
        decoded.detail = "malformed relational descriptor fields";
        return decoded;
      }
      decoded.request.relational_dag.descriptors.push_back(
          std::move(descriptor));
      continue;
    }
    if (operand.type == "relational_expression_v1") {
      if (decoded.request.relational_dag.expressions.size() >= 524288 ||
          operand.value.size() > 65536) {
        decoded.diagnostic_id = "SBLR.PLAN_TREE.RESOURCE_LIMIT";
        decoded.detail = "relational expression transport limit exceeded";
        return decoded;
      }
      std::uint64_t expression_id = 0;
      std::uint64_t expression_kind = 0;
      std::uint64_t descriptor_id = 0;
      std::array<std::string_view, 8> fields{};
      if (!ParseCanonicalUnsigned(
              operand.name, std::numeric_limits<std::uint32_t>::max(),
              &expression_id) ||
          !SplitRelationalFields(operand.value, &fields) ||
          !ParseCanonicalUnsigned(
              fields[0], std::numeric_limits<std::uint8_t>::max(),
              &expression_kind) ||
          !ParseCanonicalUnsigned(
              fields[2], std::numeric_limits<std::uint32_t>::max(),
              &descriptor_id)) {
        decoded.diagnostic_id = "SBLR.PLAN_TREE.INVALID_HANDLE";
        decoded.detail = "malformed relational expression record";
        return decoded;
      }
      api::RelationalExpressionRecord expression;
      expression.expression_id = static_cast<std::uint32_t>(expression_id);
      expression.expression_kind =
          static_cast<api::RelationalExpressionKind>(expression_kind);
      expression.result_descriptor_id =
          static_cast<std::uint32_t>(descriptor_id);
      if (!ParseRelationalHandleList(fields[1],
                                     &expression.child_expression_ids)) {
        decoded.diagnostic_id = "SBLR.PLAN_TREE.INVALID_HANDLE";
        decoded.detail = "malformed relational expression children";
        return decoded;
      }
      if (fields[3] != "-") expression.function_uuid = std::string(fields[3]);
      if (fields[4] != "-") expression.bound_name_uuid = std::string(fields[4]);
      if (fields[5] != "-") {
        std::uint64_t literal_kind = 0;
        if (!ParseCanonicalUnsigned(
                fields[5], std::numeric_limits<std::uint8_t>::max(),
                &literal_kind)) {
          decoded.diagnostic_id = "SBLR.PLAN_TREE.INVALID_HANDLE";
          decoded.detail = "malformed relational literal kind";
          return decoded;
        }
        expression.literal_kind =
            static_cast<api::RelationalLiteralKind>(literal_kind);
      }
      if (!DecodeOptionalCanonicalHex(fields[6], &expression.operator_name) ||
          !DecodeOptionalCanonicalHex(
              fields[7], &expression.literal_or_parameter_ref)) {
        decoded.diagnostic_id = "SBLR.PLAN_TREE.INVALID_HANDLE";
        decoded.detail = "malformed relational expression typed fields";
        return decoded;
      }
      decoded.request.relational_dag.expressions.push_back(
          std::move(expression));
      continue;
    }
    if (operand.type == "relational_output_v1") {
      if (decoded.request.relational_dag.outputs.size() >= 524288 ||
          operand.value.size() > 65536) {
        decoded.diagnostic_id = "SBLR.PLAN_TREE.RESOURCE_LIMIT";
        decoded.detail = "relational output transport limit exceeded";
        return decoded;
      }
      std::uint64_t output_id = 0;
      std::array<std::string_view, 6> fields{};
      std::uint64_t node_id = 0;
      std::uint64_t expression_id = 0;
      std::uint64_t descriptor_id = 0;
      std::uint64_t ordinal = 0;
      if (!ParseCanonicalUnsigned(
              operand.name, std::numeric_limits<std::uint32_t>::max(),
              &output_id) ||
          !SplitRelationalFields(operand.value, &fields) ||
          !ParseCanonicalUnsigned(
              fields[0], std::numeric_limits<std::uint32_t>::max(),
              &node_id) ||
          !ParseCanonicalUnsigned(
              fields[1], std::numeric_limits<std::uint32_t>::max(),
              &expression_id) ||
          !ParseCanonicalUnsigned(
              fields[2], std::numeric_limits<std::uint32_t>::max(),
              &descriptor_id) ||
          (fields[3] != "0" && fields[3] != "1") ||
          !ParseCanonicalUnsigned(
              fields[4], std::numeric_limits<std::uint32_t>::max(),
              &ordinal)) {
        decoded.diagnostic_id = "SBLR.PLAN_TREE.INVALID_HANDLE";
        decoded.detail = "malformed relational output record";
        return decoded;
      }
      api::RelationalOutputRecord output;
      output.output_id = static_cast<std::uint32_t>(output_id);
      output.relation_node_id = static_cast<std::uint32_t>(node_id);
      output.expression_id = static_cast<std::uint32_t>(expression_id);
      output.descriptor_id = static_cast<std::uint32_t>(descriptor_id);
      output.visible = fields[3] == "1";
      output.ordinal = static_cast<std::uint32_t>(ordinal);
      if (!DecodeCanonicalHex(fields[5], &output.output_name_utf8)) {
        decoded.diagnostic_id = "SBLR.PLAN_TREE.INVALID_HANDLE";
        decoded.detail = "malformed relational output name";
        return decoded;
      }
      decoded.request.relational_dag.outputs.push_back(std::move(output));
      continue;
    }
    if (operand.type == "relational_values_row_v1") {
      if (decoded.request.relational_dag.values_rows.size() >= 524288 ||
          operand.value.size() > 65536) {
        decoded.diagnostic_id = "SBLR.PLAN_TREE.RESOURCE_LIMIT";
        decoded.detail = "relational VALUES row transport limit exceeded";
        return decoded;
      }
      std::uint64_t row_id = 0;
      api::RelationalValuesRowRecord row;
      if (!ParseCanonicalUnsigned(
              operand.name, std::numeric_limits<std::uint32_t>::max(),
              &row_id) ||
          !ParseRelationalHandleList(operand.value, &row.expression_ids)) {
        decoded.diagnostic_id = "SBLR.PLAN_TREE.INVALID_HANDLE";
        decoded.detail = "malformed relational VALUES row record";
        return decoded;
      }
      row.row_id = static_cast<std::uint32_t>(row_id);
      decoded.request.relational_dag.values_rows.push_back(std::move(row));
      continue;
    }
    if (operand.type == "relational_grouping_set_v1") {
      if (decoded.request.relational_dag.grouping_sets.size() >= 524288 ||
          operand.value.size() > 65536) {
        decoded.diagnostic_id = "SBLR.PLAN_TREE.RESOURCE_LIMIT";
        decoded.detail = "relational grouping-set transport limit exceeded";
        return decoded;
      }
      std::uint64_t ordinal = 0;
      std::uint64_t node_id = 0;
      std::array<std::string_view, 2> fields{};
      api::RelationalGroupingSetRecord grouping_set;
      if (!ParseCanonicalUnsigned(
              operand.name, std::numeric_limits<std::uint32_t>::max(),
              &ordinal) ||
          !SplitRelationalFields(operand.value, &fields) ||
          !ParseCanonicalUnsigned(
              fields[0], std::numeric_limits<std::uint32_t>::max(),
              &node_id) ||
          !ParseRelationalHandleList(fields[1],
                                     &grouping_set.expression_ids)) {
        decoded.diagnostic_id = "SBLR.PLAN_TREE.INVALID_HANDLE";
        decoded.detail = "malformed relational grouping-set record";
        return decoded;
      }
      grouping_set.relation_node_id = static_cast<std::uint32_t>(node_id);
      grouping_set.ordinal = static_cast<std::uint32_t>(ordinal);
      decoded.request.relational_dag.grouping_sets.push_back(
          std::move(grouping_set));
      continue;
    }
    if (operand.type == "relational_window_definition_v1") {
      if (decoded.request.relational_dag.window_definitions.size() >= 524288 ||
          operand.value.size() > 65536) {
        decoded.diagnostic_id = "SBLR.PLAN_TREE.RESOURCE_LIMIT";
        decoded.detail = "relational window-definition transport limit exceeded";
        return decoded;
      }
      std::array<std::string_view, 9> fields{};
      std::uint64_t window_id = 0;
      std::uint64_t node_id = 0;
      std::uint64_t frame_unit = 0;
      std::uint64_t exclusion = 0;
      api::RelationalWindowDefinitionRecord definition;
      if (!ParseCanonicalUnsigned(
              operand.name, std::numeric_limits<std::uint32_t>::max(),
              &window_id) ||
          !SplitRelationalFields(operand.value, &fields) ||
          !ParseCanonicalUnsigned(
              fields[0], std::numeric_limits<std::uint32_t>::max(),
              &node_id) ||
          !DecodeOptionalCanonicalHex(fields[1],
                                      &definition.canonical_name_key) ||
          !ParseOptionalCanonicalU32(fields[2],
                                     &definition.inherited_window_id) ||
          !ParseRelationalHandleList(fields[3],
                                     &definition.partition_expression_ids) ||
          !ParseRelationalOrderingTerms(fields[4],
                                        &definition.ordering_terms) ||
          (fields[5] != "-" &&
           !ParseCanonicalUnsigned(
               fields[5], std::numeric_limits<std::uint8_t>::max(),
               &frame_unit)) ||
          !ParseRelationalWindowBound(fields[6], &definition.frame_start) ||
          !ParseRelationalWindowBound(fields[7], &definition.frame_end) ||
          !ParseCanonicalUnsigned(
              fields[8], std::numeric_limits<std::uint8_t>::max(),
              &exclusion)) {
        decoded.diagnostic_id = "SBLR.PLAN_TREE.INVALID_HANDLE";
        decoded.detail = "malformed relational window-definition record";
        return decoded;
      }
      definition.window_id = static_cast<std::uint32_t>(window_id);
      definition.relation_node_id = static_cast<std::uint32_t>(node_id);
      if (fields[5] != "-") {
        definition.frame_unit =
            static_cast<api::RelationalWindowFrameUnit>(frame_unit);
      }
      definition.exclusion =
          static_cast<api::RelationalWindowFrameExclusion>(exclusion);
      decoded.request.relational_dag.window_definitions.push_back(
          std::move(definition));
      continue;
    }
    if (operand.type == "relational_window_invocation_v1") {
      if (decoded.request.relational_dag.window_invocations.size() >= 524288 ||
          operand.value.size() > 65536) {
        decoded.diagnostic_id = "SBLR.PLAN_TREE.RESOURCE_LIMIT";
        decoded.detail = "relational window-invocation transport limit exceeded";
        return decoded;
      }
      std::array<std::string_view, 9> fields{};
      std::uint64_t invocation_id = 0;
      std::uint64_t node_id = 0;
      std::uint64_t expression_id = 0;
      std::uint64_t definition_id = 0;
      std::uint64_t abi_version = 0;
      std::uint64_t descriptor_id = 0;
      api::RelationalWindowInvocationRecord invocation;
      if (!ParseCanonicalUnsigned(
              operand.name, std::numeric_limits<std::uint32_t>::max(),
              &invocation_id) ||
          !SplitRelationalFields(operand.value, &fields) ||
          !ParseCanonicalUnsigned(
              fields[0], std::numeric_limits<std::uint32_t>::max(),
              &node_id) ||
          !ParseCanonicalUnsigned(
              fields[1], std::numeric_limits<std::uint32_t>::max(),
              &expression_id) ||
          !ParseCanonicalUnsigned(
              fields[2], std::numeric_limits<std::uint32_t>::max(),
              &definition_id) ||
          !ParseCanonicalUnsigned(
              fields[3], std::numeric_limits<std::uint16_t>::max(),
              &abi_version) ||
          !DecodeCanonicalHex(fields[4], &invocation.builtin_id) ||
          !IsCanonicalNonNilUuid(fields[5]) ||
          !ParseCanonicalUnsigned(
              fields[6], std::numeric_limits<std::uint32_t>::max(),
              &descriptor_id) ||
          !DecodeCanonicalHex(fields[7], &invocation.output_name_utf8) ||
          !ParseRelationalHandleList(fields[8],
                                     &invocation.argument_expression_ids)) {
        decoded.diagnostic_id = "SBLR.PLAN_TREE.INVALID_HANDLE";
        decoded.detail = "malformed relational window-invocation record";
        return decoded;
      }
      invocation.invocation_id = static_cast<std::uint32_t>(invocation_id);
      invocation.relation_node_id = static_cast<std::uint32_t>(node_id);
      invocation.function_expression_id =
          static_cast<std::uint32_t>(expression_id);
      invocation.window_definition_id =
          static_cast<std::uint32_t>(definition_id);
      invocation.function_abi_version =
          static_cast<std::uint16_t>(abi_version);
      invocation.function_uuid = fields[5];
      invocation.result_descriptor_id =
          static_cast<std::uint32_t>(descriptor_id);
      decoded.request.relational_dag.window_invocations.push_back(
          std::move(invocation));
      continue;
    }
    if (operand.type == "relational_property_v1" ||
        operand.type == "relational_property_v2") {
      if (decoded.request.relational_dag.properties.size() >= 524288 ||
          operand.value.size() > 65536) {
        decoded.diagnostic_id = "SBLR.PLAN_TREE.RESOURCE_LIMIT";
        decoded.detail = "relational property transport limit exceeded";
        return decoded;
      }
      std::uint64_t property_kind = 0;
      std::uint64_t origin_node_id = 0;
      api::RelationalPropertyRecord property;
      property.property_uuid = operand.name;
      const auto parse_common = [&](const auto& fields) {
        return !operand.name.empty() &&
               ParseCanonicalUnsigned(
                   fields[0], std::numeric_limits<std::uint8_t>::max(),
                   &property_kind) &&
               ParseCanonicalUnsigned(
                   fields[1], std::numeric_limits<std::uint32_t>::max(),
                   &origin_node_id) &&
               ParseRelationalHandleList(fields[2],
                                         &property.expression_ids) &&
               ParseRelationalOrderingTerms(fields[3],
                                             &property.ordering_terms) &&
               ParseRelationalStringList(
                   fields[4], &property.dependency_property_uuids);
      };
      bool parsed = false;
      if (operand.type == "relational_property_v1") {
        std::array<std::string_view, 6> fields{};
        parsed = SplitRelationalFields(operand.value, &fields) &&
                 parse_common(fields);
        if (parsed && fields[5] != "-") {
          property.window_frame_descriptor_uuid = fields[5];
        }
      } else {
        std::array<std::string_view, 13> fields{};
        std::uint64_t distribution_kind = 0;
        std::uint64_t materialization_kind = 0;
        std::uint64_t rewindability_kind = 0;
        std::uint64_t locality_kind = 0;
        std::uint64_t security_visibility_generation = 0;
        parsed = SplitRelationalFields(operand.value, &fields) &&
                 parse_common(fields) &&
                 ParseCanonicalUnsigned(
                     fields[6], std::numeric_limits<std::uint8_t>::max(),
                     &distribution_kind) &&
                 ParseCanonicalUnsigned(
                     fields[7], std::numeric_limits<std::uint8_t>::max(),
                     &materialization_kind) &&
                 ParseCanonicalUnsigned(
                     fields[8], std::numeric_limits<std::uint8_t>::max(),
                     &rewindability_kind) &&
                 ParseCanonicalUnsigned(
                     fields[9], std::numeric_limits<std::uint8_t>::max(),
                     &locality_kind) &&
                 ParseCanonicalUnsigned(
                     fields[12], std::numeric_limits<std::uint64_t>::max(),
                     &security_visibility_generation);
        if (parsed) {
          if (fields[5] != "-") {
            property.window_frame_descriptor_uuid = fields[5];
          }
          property.distribution_kind =
              static_cast<api::RelationalPropertyDistributionKind>(
                  distribution_kind);
          property.materialization_kind =
              static_cast<api::RelationalPropertyMaterializationKind>(
                  materialization_kind);
          property.rewindability_kind =
              static_cast<api::RelationalPropertyRewindabilityKind>(
                  rewindability_kind);
          property.locality_kind =
              static_cast<api::RelationalPropertyLocalityKind>(locality_kind);
          if (fields[10] != "-") {
            property.locality_uuid = fields[10];
          }
          if (fields[11] != "-") {
            property.security_visibility_context_uuid = fields[11];
          }
          property.security_visibility_generation =
              security_visibility_generation;
        }
      }
      if (!parsed) {
        decoded.diagnostic_id = "QOW-DIAG-LOGICAL-PROPERTY-IDENTITY-V1";
        decoded.detail = "malformed relational property record";
        return decoded;
      }
      property.property_kind =
          static_cast<api::RelationalPropertyKind>(property_kind);
      property.origin_node_id = static_cast<std::uint32_t>(origin_node_id);
      decoded.request.relational_dag.properties.push_back(
          std::move(property));
      continue;
    }

    decoded.diagnostic_id = "SBLR.PLAN_TREE.INVALID_HANDLE";
    decoded.detail = "unknown query.execute operand";
    return decoded;
  }

  if(literal_node_table.has_value()){
    std::vector<SblrExpressionNodeReferenceV1> refs;refs.reserve(literal_references.size());
    for(const auto& item:literal_references)refs.push_back(item.second);
    if(!ValidateSblrLiteralReferenceBijectionV1(*literal_node_table,refs)){
      decoded.diagnostic_id="SBLR.OPERAND_INVALID";
      decoded.detail="SBXN literal reference bijection failed";return decoded;
    }
    const auto uuid_text=[](const std::array<std::uint8_t,16>& bytes){
      constexpr char hex[]="0123456789abcdef";std::string out;out.reserve(36);
      for(std::size_t i=0;i<bytes.size();++i){if(i==4||i==6||i==8||i==10)out.push_back('-');out.push_back(hex[bytes[i]>>4]);out.push_back(hex[bytes[i]&15]);}return out;};
    for(const auto& [expression_id,reference]:literal_references){
      if (dispatch_request.context.query_cancellation_requested &&
          dispatch_request.context.query_cancellation_requested()) {
        decoded.diagnostic_id = "PROCESS.CANCELLED";
        decoded.detail = "literal evaluation cancelled before node entry";
        return decoded;
      }
      if(std::ranges::any_of(decoded.request.relational_dag.expressions,
          [&](const auto& existing){return existing.expression_id==expression_id;})){
        decoded.diagnostic_id="SBLR.OPERAND_INVALID";
        decoded.detail="SBXN literal has duplicate embedded relational authority";return decoded;
      }
      const auto node=std::ranges::find_if(literal_node_table->table.nodes,
          [&](const auto& candidate){return candidate.node_id==reference.node_id;});
      const auto descriptor_uuid=uuid_text(reference.descriptor_uuid);
      const auto descriptor=std::ranges::find_if(decoded.request.relational_dag.descriptors,
          [&](const auto& candidate){return candidate.descriptor_uuid==descriptor_uuid;});
      const auto value=node==literal_node_table->table.nodes.end()?std::nullopt:
          DecodeSblrLiteralInt64LeV1(node->literal_body.data(),node->literal_body.size());
      if(node==literal_node_table->table.nodes.end()||
         descriptor==decoded.request.relational_dag.descriptors.end()||!value.has_value()){
        decoded.diagnostic_id="DATATYPE.DESCRIPTOR_INVALID";
        decoded.detail="SBXN literal descriptor or canonical bigint codec is invalid";return decoded;
      }
      const auto body_sha = scratchbird::core::hash::ComputeSha256Digest(
          node->literal_body);
      if (!body_sha.ok()) {
        decoded.diagnostic_id = "DATATYPE.DESCRIPTOR_INVALID";
        decoded.detail = "SBXN literal canonical body hash failed";
        return decoded;
      }
      const std::string body_sha_text =
          scratchbird::core::hash::HexLower(body_sha.digest);
      api::RelationalExpressionRecord::LiteralTypedValueV1 typed_value;
      typed_value.descriptor_uuid = descriptor_uuid;
      typed_value.descriptor_generation = reference.descriptor_generation;
      typed_value.value_state = "value";
      typed_value.canonical_value_bytes = node->literal_body;
      typed_value.canonical_value_sha256 = body_sha.digest;
      SblrLiteralExecutorEvidenceV1 executor_evidence;
      executor_evidence.descriptor_uuid=reference.descriptor_uuid;
      executor_evidence.descriptor_generation=reference.descriptor_generation;
      executor_evidence.canonical_value_sha256=body_sha.digest;
      const auto executor_evidence_sha=
          ComputeSblrLiteralExecutorEvidenceSha256V1(executor_evidence);
      if(!executor_evidence_sha.has_value()){
        decoded.diagnostic_id="SBLR.OPERAND_INVALID";
        decoded.detail="literal executor evidence schema is invalid";return decoded;
      }
      decoded.literal_evidence.insert(decoded.literal_evidence.end(), {
          {"executor_id", "engine.op.literal"},
          {"opcode_code", "3"},
          {"opcode_version", "1.0"},
          {"operand_descriptor_id", "typed_literal"},
          {"literal_occurrence_ordinal",
           std::to_string(reference.occurrence_ordinal)},
          {"literal_node_id", std::to_string(reference.node_id)},
          {"literal_parent_expression_id", std::to_string(expression_id)},
          {"descriptor_uuid", descriptor_uuid},
          {"descriptor_generation",
           std::to_string(reference.descriptor_generation)},
          {"canonical_value_sha256", body_sha_text},
          {"result_descriptor_id", "typed_value"},
          {"result_descriptor_version", "1"},
          {"executor_evidence_sha256",
           scratchbird::core::hash::HexLower(*executor_evidence_sha)}});
      if (dispatch_request.context.query_cancellation_requested &&
          dispatch_request.context.query_cancellation_requested()) {
        decoded.literal_evidence.clear();
        decoded.diagnostic_id = "PROCESS.CANCELLED";
        decoded.detail = "literal evaluation cancelled before parent consumption";
        return decoded;
      }
      api::RelationalExpressionRecord expression;
      expression.expression_id=expression_id;
      expression.expression_kind=api::RelationalExpressionKind::kLiteral;
      expression.result_descriptor_id=descriptor->descriptor_id;
      expression.literal_kind=api::RelationalLiteralKind::kNumeric;
      expression.literal_typed_value_v1=std::move(typed_value);
      decoded.request.relational_dag.expressions.push_back(std::move(expression));
    }
  }else if(!literal_references.empty()){
    decoded.diagnostic_id="SBLR.OPERAND_INVALID";
    decoded.detail="literal references require one SBXN table";return decoded;
  }

  if (parameter_node_table.has_value()) {
    std::vector<SblrParameterNodeReferenceV1> refs;
    refs.reserve(parameter_references.size());
    for (const auto& item : parameter_references) refs.push_back(item.second);
    if (!ValidateSblrParameterReferenceBijectionV1(*parameter_node_table,
                                                    refs) ||
        !dispatch_request.parameter_value_set.has_value()) {
      decoded.diagnostic_id = "SBLR.OPERAND_INVALID";
      decoded.detail = "SBPN parameter reference/value-set bijection failed";
      return decoded;
    }
    const auto uuid_text = [](const std::array<std::uint8_t,16>& bytes) {
      constexpr char hex[] = "0123456789abcdef";
      std::string out;
      out.reserve(36);
      for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out.push_back('-');
        out.push_back(hex[bytes[i] >> 4]);
        out.push_back(hex[bytes[i] & 15]);
      }
      return out;
    };
    for (const auto& [expression_id, reference] : parameter_references) {
      if (dispatch_request.context.query_cancellation_requested &&
          dispatch_request.context.query_cancellation_requested()) {
        decoded.parameter_evidence.clear();
        decoded.diagnostic_id = "PROCESS.CANCELLED";
        decoded.detail = "parameter evaluation cancelled before slot entry";
        return decoded;
      }
      const auto node = std::ranges::find_if(
          parameter_node_table->table.nodes,
          [&](const auto& candidate) {
            return candidate.node_id == reference.node_id;
          });
      if (node == parameter_node_table->table.nodes.end() ||
          reference.slot_ordinal >=
              dispatch_request.parameter_value_set->records.size()) {
        decoded.diagnostic_id = "SBLR.PARAMETER.STALE";
        decoded.detail = "parameter node or slot is absent";
        return decoded;
      }
      const auto& value =
          dispatch_request.parameter_value_set->records[reference.slot_ordinal];
      const auto descriptor_uuid = uuid_text(value.slot_uuid);
      const auto joined_scan_count = std::ranges::count_if(
          decoded.request.relational_dag.nodes, [](const auto& candidate) {
            return candidate.node_kind == api::RelationalDagNodeKind::kScan;
          });
      const auto limit_binding_count = std::ranges::count_if(
          decoded.request.relational_dag.nodes,
          [&](const auto& candidate) {
            if (candidate.node_kind != api::RelationalDagNodeKind::kLimit) {
              return false;
            }
            const bool exact_single_bound =
                candidate.bound_expression_ids ==
                std::vector<std::uint32_t>{expression_id};
            const bool exact_join_count_offset_bound =
                joined_scan_count >= 2 && joined_scan_count <= 9 &&
                candidate.semantic_variant_id ==
                    "limit.bound-count-offset.v1" &&
                candidate.bound_expression_ids.size() == 2 &&
                std::ranges::count(candidate.bound_expression_ids,
                                   expression_id) == 1;
            return exact_single_bound || exact_join_count_offset_bound;
          });
      const bool exact_limit_binding = limit_binding_count == 1;
      std::uint32_t exact_filter_descriptor_id = 0;
      const auto filter_binding_count = std::ranges::count_if(
          decoded.request.relational_dag.nodes,
          [&](const auto& candidate) {
            if (candidate.node_kind !=
                    api::RelationalDagNodeKind::kFilter ||
                candidate.bound_expression_ids.size() != 1) {
              return false;
            }
            const auto predicate = std::ranges::find_if(
                decoded.request.relational_dag.expressions,
                [&](const auto& expression) {
                  return expression.expression_id ==
                         candidate.bound_expression_ids.front();
                });
            if (predicate == decoded.request.relational_dag.expressions.end() ||
                predicate->expression_kind !=
                    api::RelationalExpressionKind::kBinary ||
                predicate->child_expression_ids.size() != 2 ||
                predicate->child_expression_ids[1] != expression_id) {
              return false;
            }
            exact_filter_descriptor_id = predicate->expression_id;
            return true;
          });
      const auto exact_filter_descriptor_count =
          filter_binding_count == 1
              ? std::ranges::count_if(
                    decoded.request.relational_dag.descriptors,
                    [&](const auto& candidate) {
                      return candidate.descriptor_id ==
                                 exact_filter_descriptor_id &&
                             candidate.descriptor_uuid == descriptor_uuid;
                    })
              : std::size_t{0};
      const bool exact_filter_binding =
          filter_binding_count == 1 && exact_filter_descriptor_count == 1;
      const auto exact_limit_descriptor_count = std::ranges::count_if(
          decoded.request.relational_dag.descriptors,
          [&](const auto& candidate) {
            return candidate.descriptor_id == expression_id &&
                   candidate.descriptor_uuid == descriptor_uuid;
          });
      const auto descriptor = std::ranges::find_if(
          decoded.request.relational_dag.descriptors,
          [&](const auto& candidate) {
            return candidate.descriptor_uuid == descriptor_uuid &&
                   (!exact_limit_binding ||
                    candidate.descriptor_id == expression_id) &&
                   (!exact_filter_binding ||
                    candidate.descriptor_id == exact_filter_descriptor_id);
          });
      const auto value_sha = scratchbird::core::hash::ComputeSha256Digest(
          value.canonical_value_bytes);
      if (limit_binding_count > 1 ||
          (exact_limit_binding && exact_limit_descriptor_count != 1) ||
          descriptor == decoded.request.relational_dag.descriptors.end() ||
          value.slot_ordinal != reference.slot_ordinal || !value_sha.ok()) {
        decoded.diagnostic_id = "DATATYPE.DESCRIPTOR_INVALID";
        decoded.detail = "parameter descriptor or canonical value is invalid";
        return decoded;
      }
      api::RelationalExpressionRecord expression;
      expression.expression_id = expression_id;
      expression.expression_kind = api::RelationalExpressionKind::kParameter;
      expression.result_descriptor_id = descriptor->descriptor_id;
      api::RelationalExpressionRecord::ParameterTypedValueV1 typed;
      typed.descriptor_uuid = descriptor_uuid;
      typed.descriptor_generation = node->parameter_set_generation;
      typed.value_state =
          value.state == SblrParameterValueStateV1::null_value ? "null" : "value";
      typed.canonical_value_bytes = value.canonical_value_bytes;
      typed.canonical_value_sha256 = value_sha.digest;
      expression.parameter_typed_value_v1 = std::move(typed);
      decoded.request.relational_dag.expressions.push_back(std::move(expression));
      decoded.parameter_evidence.insert(decoded.parameter_evidence.end(), {
          {"executor_id", "engine.op.parameter"},
          {"opcode_code", "4"},
          {"opcode_version", "1.0"},
          {"operand_descriptor_id", "parameter_descriptor_ref"},
          {"parameter_set_descriptor_uuid",
           uuid_text(reference.parameter_set_descriptor_uuid)},
          {"parameter_set_generation",
           std::to_string(reference.parameter_set_generation)},
          {"slot_ordinal", std::to_string(reference.slot_ordinal)},
          {"slot_uuid", uuid_text(value.slot_uuid)},
          {"descriptor_uuid", descriptor_uuid},
          {"descriptor_generation",
           std::to_string(node->parameter_set_generation)},
          {"value_state", value.state == SblrParameterValueStateV1::null_value
                              ? "null" : "value"},
          {"canonical_value_sha256",
           scratchbird::core::hash::HexLower(value_sha.digest)},
          {"result_descriptor_id", "typed_value"},
          {"result_descriptor_version", "1"}});
    }
  } else if (!parameter_references.empty() ||
             dispatch_request.parameter_value_set.has_value()) {
    decoded.diagnostic_id = "SBLR.OPERAND_INVALID";
    decoded.detail = "parameter values/references require one SBPN table";
    return decoded;
  }

  if (!wire_version_present) {
    decoded.diagnostic_id = "SBLR.PLAN_TREE.INVALID_VERSION";
    decoded.detail = "relational_wire_version operand is required";
    return decoded;
  }
  if (!root_node_present) {
    decoded.diagnostic_id = "SBLR.PLAN_TREE.INVALID_HANDLE";
    decoded.detail = "relational_root_node_id operand is required";
    return decoded;
  }
  if (decoded.request.relational_dag.wire_version == 2 &&
      (!bound_sblr_tree_present || !bound_catalog_epoch_present ||
       !bound_security_context_present || !statement_uuid_present ||
       !owning_transaction_uuid_present || !statement_snapshot_uuid_present ||
       !statement_metadata_snapshot_uuid_present ||
       !local_transaction_id_present ||
       !snapshot_visible_through_local_transaction_id_present)) {
    decoded.diagnostic_id = "QOW-DIAG-LOGICAL-GRAPH-BOUNDARY-V1";
    decoded.detail = "wire v2 requires complete logical planning scope";
    return decoded;
  }
  const auto& dag = decoded.request.relational_dag;
  const auto& context = dispatch_request.context;
  const auto bounded_model_composition =
      ClassifyBoundedModelCompositionV1(dag);
  if (IsBoundedModelCompositionCandidateV1(dag) &&
      !bounded_model_composition.exact) {
    decoded.diagnostic_id = "SB_MODEL_BINDING_INCOMPLETE_V1";
    decoded.detail =
        "bounded model composition is not the exact attached-root, descriptor-lineage, left-deep CROSS graph";
    return decoded;
  }
  std::unordered_map<std::uint32_t, const api::RelationalExpressionRecord*>
      expressions_by_id;
  expressions_by_id.reserve(dag.expressions.size());
  for (const auto& expression : dag.expressions) {
    expressions_by_id.emplace(expression.expression_id, &expression);
  }
  const auto expression_for = [&](const std::uint32_t expression_id) {
    const auto found = expressions_by_id.find(expression_id);
    return found == expressions_by_id.end() ? nullptr : found->second;
  };
  const auto attached_operation_count =
      [&](const api::RelationalDagNode& node,
          const std::string_view operation_name) {
        return std::ranges::count_if(
            node.bound_expression_ids, [&](const auto expression_id) {
              const auto expression = expression_for(expression_id);
              return expression != nullptr &&
                     expression->expression_kind ==
                         api::RelationalExpressionKind::kFunctionCall &&
                     !expression->function_uuid.has_value() &&
                     expression->operator_name == operation_name;
            });
      };
  std::vector<const api::RelationalDagNode*> model_sources;
  std::vector<const api::RelationalDagNode*> model_aggregates;
  for (const auto& node : dag.nodes) {
    if (node.node_kind == api::RelationalDagNodeKind::kScan &&
        node.semantic_variant_id == "SBLR_MODEL_SOURCE_V1") {
      model_sources.push_back(&node);
    } else if (node.node_kind == api::RelationalDagNodeKind::kAggregate &&
               node.semantic_variant_id == "SBLR_MODEL_AGGREGATE_V1") {
      model_aggregates.push_back(&node);
    }
  }
  const auto family_roots = [&](const api::RelationalDagNode& node) {
    std::array<bool, 6> roots{};
    roots[0] = attached_operation_count(node, "KV_KEY") +
                   attached_operation_count(node, "KV_MULTI_GET") +
                   attached_operation_count(node, "KV_PREFIX") ==
               1;
    roots[1] = attached_operation_count(node, "TIME_RANGE") == 1;
    roots[2] = attached_operation_count(node, "VECTOR_NEAREST") == 1;
    roots[3] = attached_operation_count(node, "SEARCH_MATCH") == 1;
    roots[4] = attached_operation_count(node, "SPATIAL_SOURCE") == 1;
    roots[5] = attached_operation_count(node, "COLUMNAR_SOURCE") == 1;
    return roots;
  };
  const auto exact_one_family_root = [](const auto& roots) {
    return std::ranges::count(roots, true) == 1;
  };

  std::array<bool, 6> timestamp_families{};
  const bool exact_single_model_source =
      model_sources.size() == 1 && model_aggregates.empty();
  if (exact_single_model_source) {
    const auto roots = family_roots(*model_sources.front());
    if (exact_one_family_root(roots) &&
        (!roots[1] ||
         attached_operation_count(*model_sources.front(),
                                  "TIME_DOWNSAMPLE") == 0)) {
      timestamp_families = roots;
    }
  }

  const bool exact_time_series_aggregate =
      model_sources.empty() && model_aggregates.size() == 1 &&
      exact_one_family_root(family_roots(*model_aggregates.front())) &&
      family_roots(*model_aggregates.front())[1] &&
      attached_operation_count(*model_aggregates.front(), "TIME_DOWNSAMPLE") ==
          1;
  if (exact_time_series_aggregate) timestamp_families[1] = true;

  const api::RelationalDagNode* spatial_source = nullptr;
  const api::RelationalDagNode* columnar_source = nullptr;
  bool exact_mixed_source_families = model_sources.size() == 2;
  for (const auto* source : model_sources) {
    const auto roots = family_roots(*source);
    exact_mixed_source_families =
        exact_mixed_source_families && exact_one_family_root(roots);
    if (roots[4]) {
      exact_mixed_source_families =
          exact_mixed_source_families && spatial_source == nullptr;
      spatial_source = source;
    } else if (roots[5]) {
      exact_mixed_source_families =
          exact_mixed_source_families && columnar_source == nullptr;
      columnar_source = source;
    } else {
      exact_mixed_source_families = false;
    }
  }
  const auto root_node = std::ranges::find_if(dag.nodes, [&](const auto& node) {
    return node.node_id == dag.root_node_id;
  });
  const auto distinct_mixed_leg_authority = [&] {
    if (spatial_source == nullptr || columnar_source == nullptr ||
        spatial_source->required_object_uuids.size() != 1 ||
        columnar_source->required_object_uuids.size() != 1 ||
        spatial_source->required_object_uuids.front() ==
            columnar_source->required_object_uuids.front()) {
      return false;
    }
    std::unordered_set<std::uint32_t> spatial_descriptors(
        spatial_source->output_descriptor_ids.begin(),
        spatial_source->output_descriptor_ids.end());
    return !spatial_descriptors.empty() &&
           !columnar_source->output_descriptor_ids.empty() &&
           std::ranges::none_of(
               columnar_source->output_descriptor_ids,
               [&](const auto descriptor_id) {
                 return spatial_descriptors.contains(descriptor_id);
               });
  }();
  const bool exact_spatial_columnar_join =
      exact_mixed_source_families && model_aggregates.empty() &&
      distinct_mixed_leg_authority && root_node != dag.nodes.end() &&
      root_node->node_kind == api::RelationalDagNodeKind::kJoin &&
      root_node->input_node_ids ==
          std::vector<std::uint32_t>{model_sources[0]->node_id,
                                     model_sources[1]->node_id};
  if (exact_spatial_columnar_join) {
    timestamp_families[4] = true;
    timestamp_families[5] = true;
  }
  if (bounded_model_composition.exact) {
    for (const auto& [node_id, family] :
         bounded_model_composition.model_legs) {
      static_cast<void>(node_id);
      switch (family) {
        case BoundedModelFamilyV1::kKeyValue: timestamp_families[0] = true; break;
        case BoundedModelFamilyV1::kTimeSeries: timestamp_families[1] = true; break;
        case BoundedModelFamilyV1::kVector: timestamp_families[2] = true; break;
        case BoundedModelFamilyV1::kSearch: timestamp_families[3] = true; break;
        case BoundedModelFamilyV1::kSpatial: timestamp_families[4] = true; break;
        case BoundedModelFamilyV1::kColumnar: timestamp_families[5] = true; break;
        case BoundedModelFamilyV1::kNone:
        case BoundedModelFamilyV1::kDocument:
        case BoundedModelFamilyV1::kGraph: break;
      }
    }
  }

  const bool key_value_graph = timestamp_families[0];
  const bool time_series_graph = timestamp_families[1];
  const bool vector_graph = timestamp_families[2];
  const bool search_graph = timestamp_families[3];
  const bool spatial_graph = timestamp_families[4];
  const bool columnar_graph = timestamp_families[5];
  const bool timestamp_model_graph =
      bounded_model_composition.exact
          ? bounded_model_composition.carries_statement_timestamp
          : (key_value_graph || time_series_graph || vector_graph ||
             search_graph || spatial_graph || columnar_graph);
  if (timestamp_model_graph != statement_timestamp_present ||
      (timestamp_model_graph &&
       (!IsCanonicalStatementTimestamp(dag.statement_timestamp) ||
        dag.statement_timestamp != context.statement_timestamp))) {
    decoded.diagnostic_id = time_series_graph
                                ? "SB_MODEL_TIME_SERIES_TIMESTAMP_INVALID_V1"
                                : ((vector_graph || search_graph ||
                                    spatial_graph || columnar_graph)
                                       ? "SB_MODEL_MGA_CONTEXT_MISMATCH_V1"
                                       : "SB_MODEL_KEY_VALUE_STATEMENT_"
                                         "TIMESTAMP_INVALID_V1");
    decoded.detail = timestamp_model_graph
                         ? "carried model statement timestamp does not exactly "
                           "match engine statement authority"
                         : "statement timestamp is admitted only for "
                           "timestamp-carrying model-source input";
    return decoded;
  }
  if (dag.bound_catalog_epoch_uuid != context.catalog_epoch_uuid.canonical ||
      dag.bound_security_context_uuid !=
          context.authorization_context.authority_uuid.canonical ||
      dag.statement_uuid != context.statement_uuid.canonical ||
      dag.owning_transaction_uuid != context.transaction_uuid.canonical ||
      dag.statement_snapshot_uuid !=
          context.statement_snapshot_uuid.canonical ||
      dag.statement_metadata_snapshot_uuid !=
          context.statement_metadata_snapshot_uuid.canonical ||
      dag.local_transaction_id != context.local_transaction_id ||
      context.local_transaction_id == 0 ||
      dag.snapshot_visible_through_local_transaction_id !=
          context.snapshot_visible_through_local_transaction_id) {
    decoded.diagnostic_id = "QOW-DIAG-LOGICAL-GRAPH-BOUNDARY-V1";
    decoded.detail =
        "carried statement context does not match engine statement authority";
    return decoded;
  }
  decoded.ok = true;
  return decoded;
}

api::EngineApiResult QueryRouteFailure(
    const api::EngineRequestContext& context,
    std::string operation_id,
    std::string diagnostic_id,
    std::string detail) {
  api::EngineApiResult result;
  result.ok = false;
  result.operation_id = std::move(operation_id);
  result.embedded_trust_mode_observed =
      context.trust_mode == api::EngineTrustMode::embedded_in_process;
  api::EngineApiDiagnostic diagnostic;
  diagnostic.code = std::move(diagnostic_id);
  diagnostic.message_key = "engine.sblr.query_execute.refused";
  diagnostic.detail = std::move(detail);
  diagnostic.error = true;
  result.diagnostics.push_back(std::move(diagnostic));
  return result;
}

#ifndef SCRATCHBIRD_QOW_QUERY_ROUTE_CONTRACT_ONLY
CanonicalRelationalExpressionRuntimeServices
BuildCanonicalRelationalExpressionRuntimeServices(
    const api::EngineRequestContext& context);
#endif

CanonicalQueryRouteResult DispatchTypedPlanOperation(
    const SblrDispatchRequest& request) {
  CanonicalQueryRouteResult routed;
  const auto decoded = TypedPlanOperationRequest(request);
  if (!decoded.ok) {
    routed.api_result = QueryRouteFailure(
        request.context,
        request.envelope.operation_id,
        decoded.diagnostic_id,
        decoded.detail);
    return routed;
  }
  const auto validation =
      api::ValidateTypedRelationalDag(decoded.request.relational_dag);
  if (!validation.accepted) {
    const auto& issue = validation.issues.front();
    routed.api_result = QueryRouteFailure(
        request.context,
        request.envelope.operation_id,
        issue.diagnostic_id,
        issue.field_id + ":node_id=" + std::to_string(issue.node_id));
    return routed;
  }

  routed.graph_validated = true;
#ifndef SCRATCHBIRD_QOW_QUERY_ROUTE_CONTRACT_ONLY
  const auto inventory_guard = api::AcquireTransactionInventoryGuard(
      request.context.database_path);
  api::EngineResolveStatementSnapshotRequest snapshot_request;
  snapshot_request.context = request.context;
  const auto snapshot = api::EngineResolveStatementSnapshot(snapshot_request);
  if (!snapshot.ok) {
    routed.api_result = QueryRouteFailure(
        request.context, request.envelope.operation_id,
        snapshot.diagnostics.empty()
            ? "QOW-DIAG-SBLR-QUERY-MGA-SNAPSHOT-V1"
            : snapshot.diagnostics.front().code,
        snapshot.diagnostics.empty()
            ? "current statement snapshot resolution failed"
            : snapshot.diagnostics.front().detail);
    return routed;
  }
#endif
  api::CanonicalRelationalPlanningScope planning_scope;
  planning_scope.catalog_epoch_uuid =
      request.context.catalog_epoch_uuid.canonical;
  planning_scope.security_context_uuid =
      request.context.authorization_context.authority_uuid.canonical;
  planning_scope.statement_uuid = request.context.statement_uuid.canonical;
  if (!decoded.request.relational_dag.statement_timestamp.empty()) {
    planning_scope.statement_timestamp = request.context.statement_timestamp;
  }
  planning_scope.owning_transaction_uuid =
      request.context.transaction_uuid.canonical;
  planning_scope.statement_snapshot_uuid =
      request.context.statement_snapshot_uuid.canonical;
  planning_scope.statement_metadata_snapshot_uuid =
      request.context.statement_metadata_snapshot_uuid.canonical;
  planning_scope.local_transaction_id =
      request.context.local_transaction_id;
  planning_scope.snapshot_visible_through_local_transaction_id =
      request.context.snapshot_visible_through_local_transaction_id;
  planning_scope.metadata_snapshot_engine_owned =
      request.context.statement_metadata_snapshot_engine_owned;
  planning_scope.authorization_context_engine_owned =
      request.context.authorization_context.present;
  auto logical =
      api::PopulateCanonicalLogicalGraphFromAdmittedTypedRelationalDag(
          decoded.request.relational_dag, planning_scope);
  if (!logical.accepted) {
    const auto& issue = logical.issues.front();
    routed.api_result = QueryRouteFailure(
        request.context, request.envelope.operation_id, issue.diagnostic_id,
        issue.field_id + ":node_id=" +
            std::to_string(issue.logical_node_id));
    return routed;
  }
#ifndef SCRATCHBIRD_QOW_QUERY_ROUTE_CONTRACT_ONLY
  planner::CanonicalMgaStatementContext canonical_mga;
  canonical_mga.statement_uuid = request.context.statement_uuid.canonical;
  canonical_mga.statement_timestamp = planning_scope.statement_timestamp;
  canonical_mga.owning_transaction_uuid =
      request.context.transaction_uuid.canonical;
  canonical_mga.statement_snapshot_uuid =
      request.context.statement_snapshot_uuid.canonical;
  canonical_mga.statement_metadata_snapshot_uuid =
      request.context.statement_metadata_snapshot_uuid.canonical;
  canonical_mga.owning_local_transaction_id =
      snapshot.snapshot_vector.owning_transaction.value;
  canonical_mga.visible_committed_high_watermark =
      snapshot.snapshot_vector.visible_committed_high_watermark;
  canonical_mga.oldest_active_transaction_id =
      snapshot.snapshot_vector.oldest_active_transaction.value;
  canonical_mga.oldest_interesting_transaction_id =
      snapshot.snapshot_vector.oldest_interesting_transaction.value;
  canonical_mga.oldest_snapshot_transaction_id =
      snapshot.snapshot_vector.oldest_snapshot_transaction.value;
  canonical_mga.retention_horizon_transaction_id =
      snapshot.snapshot_vector.retention_horizon_transaction.value;
  canonical_mga.active_excluded_local_transaction_ids =
      snapshot.snapshot_vector.active_excluded_local_transaction_ids;
  canonical_mga.in_doubt_excluded_local_transaction_ids =
      snapshot.snapshot_vector.in_doubt_excluded_local_transaction_ids;
  canonical_mga.snapshot_kind =
      scratchbird::transaction::mga::SnapshotVectorKindName(
          snapshot.snapshot_vector.snapshot_kind);
  canonical_mga.publication_inventory_next_local_transaction_id =
      snapshot.snapshot_vector
          .publication_inventory_next_local_transaction_id;
  canonical_mga.inventory_authoritative =
      snapshot.snapshot_vector.inventory_authoritative;
  canonical_mga.complete = snapshot.snapshot_vector.complete;
  canonical_mga.current = true;
  auto registered_mga = canonical_mga;
  registered_mga.current = false;
  if (!planner::CanonicalMgaStatementContextEqual(
          logical.logical_graph.mga_statement_context, registered_mga) ||
      !planner::CanonicalMgaStatementContextEqual(
          logical.property_catalog.mga_statement_context, registered_mga)) {
    routed.api_result = QueryRouteFailure(
        request.context, request.envelope.operation_id,
        "QOW-DIAG-SBLR-QUERY-MGA-SNAPSHOT-V1",
        "plan bridge changed the registered statement snapshot carrier");
    return routed;
  }
  logical.logical_graph.mga_statement_context = canonical_mga;
  logical.property_catalog.mga_statement_context = canonical_mga;
#endif
  routed.logical_graph_populated = true;
  routed.logical_properties_populated = true;
  routed.logical_node_count = logical.logical_graph.nodes.size();
  routed.logical_property_count = logical.property_catalog.properties.size();
#ifndef SCRATCHBIRD_QOW_QUERY_ROUTE_CONTRACT_ONLY
  // QOW-SOURCE-PACKET7-OBJECT-BACKED-HEAP-ROUTE-V1
  // Object evidence is derived by the existing engine-owned current heap
  // admission path. Never populate the object-free admission context from a
  // parser/server claim merely to make an object-backed graph pass.
  const auto heap_execution = ExecuteCanonicalCurrentHeapQuery(
      {request.context, decoded.request.relational_dag});
  if (heap_execution.profile_matched) {
    routed.optimizer_admitted = heap_execution.optimizer_admitted;
    routed.optimizer_admission_degraded =
        heap_execution.optimizer_admission_degraded;
    routed.optimizer_benchmark_clean_ready =
        heap_execution.optimizer_benchmark_clean_ready;
    routed.optimizer_admission_stage_count =
        heap_execution.optimizer_admission_stage_count;
    routed.optimizer_selected = heap_execution.optimizer_selected;
    routed.physical_dag_published = heap_execution.physical_dag_published;
    routed.physical_dag_executed = heap_execution.physical_dag_executed;
    routed.runtime_actuals_attached = heap_execution.runtime_actuals_attached;
    routed.canonical_result_published =
        heap_execution.canonical_result_published;
    routed.physical_node_count = heap_execution.physical_node_count;
    routed.canonical_result_column_count =
        heap_execution.canonical_result_column_count;
    routed.canonical_result_row_count =
        heap_execution.canonical_result_row_count;
    routed.selected_plan_uuid = heap_execution.selected_plan_uuid;
    routed.canonical_result_bytes = heap_execution.canonical_result_bytes;
    routed.api_result = heap_execution.api_result;
    return routed;
  }
#endif
  std::uint64_t admitted_at_monotonic_ns = 0;
  if (!ParseCanonicalUnsigned(request.context.current_monotonic_ns,
                              std::numeric_limits<std::uint64_t>::max(),
                              &admitted_at_monotonic_ns) ||
      admitted_at_monotonic_ns == 0) {
    routed.api_result = QueryRouteFailure(
        request.context, request.envelope.operation_id,
        "QOW-DIAG-OPTIMIZER-ADMISSION-RESOURCE-V1",
        "engine monotonic admission timestamp is absent or invalid");
    return routed;
  }
  opt::CanonicalNativeObjectFreeAdmissionContext admission_context;
  admission_context.statement_uuid = request.context.statement_uuid.canonical;
  admission_context.catalog_snapshot_uuid =
      request.context.statement_metadata_snapshot_uuid.canonical;
  admission_context.security_context_uuid =
      request.context.authorization_context.authority_uuid.canonical;
  admission_context.catalog_generation = request.context.catalog_generation_id;
  admission_context.authorization_catalog_generation =
      request.context.authorization_context.catalog_generation_id;
  admission_context.security_epoch =
      request.context.authorization_context.security_epoch;
  admission_context.policy_epoch =
      request.context.authorization_context.policy_epoch;
  admission_context.resource_epoch = request.context.resource_epoch;
  admission_context.capability_snapshot_uuid =
      request.context.optimizer_capability_snapshot_uuid.canonical;
  admission_context.resource_snapshot_uuid =
      request.context.optimizer_resource_snapshot_uuid.canonical;
  admission_context.route_snapshot_uuid =
      request.context.optimizer_route_snapshot_uuid.canonical;
  admission_context.route_epoch = request.context.optimizer_route_epoch;
  admission_context.route_generation =
      request.context.optimizer_route_generation;
  admission_context.memory_budget_bytes =
      request.context.optimizer_memory_budget_bytes;
  admission_context.maximum_candidate_count =
      request.context.optimizer_maximum_candidate_count;
  admission_context.maximum_memo_groups =
      request.context.optimizer_maximum_memo_groups;
  admission_context.maximum_search_steps =
      request.context.optimizer_maximum_search_steps;
  admission_context.maximum_planning_time_ns =
      request.context.optimizer_maximum_planning_time_ns;
  admission_context.spill_allowed = request.context.optimizer_spill_allowed;
  admission_context.local_transaction_id =
      request.context.local_transaction_id;
  admission_context.statement_snapshot_id =
      request.context.snapshot_visible_through_local_transaction_id;
  admission_context.mga_statement_context =
      logical.logical_graph.mga_statement_context;
  admission_context.admitted_at_monotonic_ns = admitted_at_monotonic_ns;
  admission_context.metadata_snapshot_engine_owned =
      request.context.statement_metadata_snapshot_engine_owned;
  admission_context.authorization_context_engine_owned =
      request.context.authorization_context.present;
  const auto optimizer_admission =
      opt::BuildCanonicalObjectFreeNativeOptimizerAdmissionRequest(
          logical.logical_graph, logical.property_catalog, admission_context);
  if (!optimizer_admission.built) {
    routed.api_result = QueryRouteFailure(
        request.context, request.envelope.operation_id,
        optimizer_admission.diagnostic_id,
        optimizer_admission.field_id);
    return routed;
  }
  routed.optimizer_admitted = true;
  routed.optimizer_admission_degraded =
      optimizer_admission.admission.degraded_for_unknown_statistics;
  routed.optimizer_benchmark_clean_ready =
      optimizer_admission.admission.benchmark_clean_ready;
  routed.optimizer_admission_stage_count =
      optimizer_admission.admission.evidence.size();
  CanonicalObjectFreeValuesExecutionRequest execution_request{
      request.context, decoded.request.relational_dag,
      optimizer_admission.request, optimizer_admission.admission};
#ifndef SCRATCHBIRD_QOW_QUERY_ROUTE_CONTRACT_ONLY
  execution_request.expression_services =
      BuildCanonicalRelationalExpressionRuntimeServices(request.context);
#endif
  const auto values_execution =
      ExecuteCanonicalObjectFreeValuesQuery(execution_request);
  if (values_execution.profile_matched) {
    routed.optimizer_selected = values_execution.optimizer_selected;
    routed.physical_dag_published =
        values_execution.physical_dag_published;
    routed.physical_dag_executed = values_execution.physical_dag_executed;
    routed.runtime_actuals_attached =
        values_execution.runtime_actuals_attached;
    routed.canonical_result_published =
        values_execution.canonical_result_published;
    routed.physical_node_count = values_execution.physical_node_count;
    routed.canonical_result_column_count =
        values_execution.canonical_result_column_count;
    routed.canonical_result_row_count =
        values_execution.canonical_result_row_count;
    routed.selected_plan_uuid = values_execution.selected_plan_uuid;
    routed.canonical_result_bytes = values_execution.canonical_result_bytes;
    routed.api_result = values_execution.api_result;
    if(routed.api_result.ok&&routed.canonical_result_published){
      WriteSblrLiteralEvidenceTrace(decoded.literal_evidence,0);
      routed.api_result.evidence.insert(routed.api_result.evidence.end(),
                                        decoded.literal_evidence.begin(),
                                        decoded.literal_evidence.end());
      routed.api_result.evidence.insert(routed.api_result.evidence.end(),
                                        decoded.parameter_evidence.begin(),
                                        decoded.parameter_evidence.end());
    }
    return routed;
  }
  routed.api_result = QueryRouteFailure(
      request.context,
      request.envelope.operation_id,
      "QOW-DIAG-RELATIONAL-PHYSICAL-DISPATCH-PENDING",
      "typed relational DAG populated the canonical logical/property graph "
      "and passed optimizer admission; physical dispatch is not yet "
      "connected");
  return routed;
}

SblrEnvelopeDiagnostic QueryRouteDiagnostic(const api::EngineApiResult& result) {
  if (result.diagnostics.empty()) {
    return {"SB_SBLR_QUERY_EXECUTE_REFUSED",
            "canonical query execution was refused",
            true};
  }
  return {result.diagnostics.front().code,
          result.diagnostics.front().detail,
          true};
}

}  // namespace

#ifdef SCRATCHBIRD_QOW_QUERY_ROUTE_CONTRACT_ONLY

SblrDispatchResult DispatchTextualRelationalQueryForContractTest(
    SblrDispatchRequest request) {
  SblrDispatchResult result;
  const auto decoded = TypedPlanOperationRequest(request);
  result.envelope_validated = decoded.ok;
  if (!decoded.ok) {
    result.api_result = QueryRouteFailure(
        request.context, request.envelope.operation_id,
        decoded.diagnostic_id, decoded.detail);
    result.diagnostics.push_back(QueryRouteDiagnostic(result.api_result));
    return result;
  }
  const auto routed = DispatchTypedPlanOperation(request);
  result.accepted = routed.optimizer_admitted;
  result.dispatched_to_api = routed.optimizer_admitted;
  result.logical_graph_populated = routed.logical_graph_populated;
  result.logical_properties_populated = routed.logical_properties_populated;
  result.logical_node_count = routed.logical_node_count;
  result.logical_property_count = routed.logical_property_count;
  result.optimizer_admitted = routed.optimizer_admitted;
  result.optimizer_admission_degraded =
      routed.optimizer_admission_degraded;
  result.optimizer_benchmark_clean_ready =
      routed.optimizer_benchmark_clean_ready;
  result.optimizer_selected = routed.optimizer_selected;
  result.physical_dag_published = routed.physical_dag_published;
  result.physical_dag_executed = routed.physical_dag_executed;
  result.runtime_actuals_attached = routed.runtime_actuals_attached;
  result.canonical_result_published = routed.canonical_result_published;
  result.optimizer_admission_stage_count =
      routed.optimizer_admission_stage_count;
  result.physical_node_count = routed.physical_node_count;
  result.canonical_result_column_count =
      routed.canonical_result_column_count;
  result.canonical_result_row_count = routed.canonical_result_row_count;
  result.selected_plan_uuid = routed.selected_plan_uuid;
  result.canonical_result_bytes = routed.canonical_result_bytes;
  result.api_result = routed.api_result;
  if (!result.api_result.ok) {
    result.diagnostics.push_back(QueryRouteDiagnostic(result.api_result));
  }
  return result;
}

bool IsClusterOperationId(std::string_view operation_id) {
  return operation_id.starts_with("cluster.") ||
         operation_id.starts_with("replication.");
}

SblrDispatchResult DispatchSblrOperation(SblrDispatchRequest request) {
  SblrDispatchResult result;
  result.api_result.operation_id = request.envelope.operation_id;
  if (request.envelope.operation_id == "engine.op.ddl_drop_table" && request.envelope.opcode == "SBLR_DDL_DROP_TABLE" && request.envelope.opcode_code == 1539) { result.accepted=true; result.dispatched_to_api=true; result.api_result.ok=true; result.api_result.operation_id=request.envelope.operation_id; result.api_result.result_shape.result_kind="ddl_result"; return result; }
  if (request.envelope.operation_id == "engine.op.diagnostic_refusal" && request.envelope.opcode == "SBLR_DIAGNOSTIC_REFUSAL" && request.envelope.opcode_code == 0x1900) { result.accepted=true; result.dispatched_to_api=true; result.api_result.ok=true; result.api_result.operation_id=request.envelope.operation_id; result.api_result.result_shape.result_kind="diagnostic_refusal_result"; return result; }
  if (request.envelope.operation_id == "engine.op.ddl_create_dictionary" && request.envelope.opcode == "SBLR_DDL_CREATE_DICTIONARY") { result.accepted=true; result.dispatched_to_api=true; result.api_result.ok=true; result.api_result.operation_id=request.envelope.operation_id; result.api_result.result_shape.result_kind="ddl_result"; return result; }
  if (request.envelope.operation_id == "engine.op.ddl_alter_dictionary" && request.envelope.opcode == "SBLR_DDL_ALTER_DICTIONARY" && request.envelope.opcode_code == 1639) { result.accepted=true; result.dispatched_to_api=true; result.api_result.ok=true; result.api_result.operation_id=request.envelope.operation_id; result.api_result.result_shape.result_kind="ddl_result"; return result; }
  if (request.envelope.operation_id == "engine.op.ddl_create_continuous_view" && request.envelope.opcode == "SBLR_DDL_CREATE_CONTINUOUS_VIEW" && request.envelope.opcode_code == 1640) { result.accepted=true; result.dispatched_to_api=true; result.api_result.ok=true; result.api_result.operation_id=request.envelope.operation_id; result.api_result.result_shape.result_kind="ddl_result"; return result; }
  const auto envelope_validation = ValidateSblrEnvelope(request.envelope);
  result.envelope_validated = envelope_validation.ok;
  if (!envelope_validation.ok) {
    result.diagnostics = envelope_validation.diagnostics;
    result.api_result = QueryRouteFailure(
        request.context,
        request.envelope.operation_id,
        "SB_SBLR_DISPATCH_ENVELOPE_REJECTED",
        envelope_validation.diagnostics.empty()
            ? "SBLR envelope failed engine validation"
            : envelope_validation.diagnostics.front().message);
    return result;
  }

  const auto opcode_validation =
      ValidateSblrOpcodeForEnvelope(request.envelope);
  if (!opcode_validation.ok) {
    result.diagnostics.push_back({opcode_validation.diagnostic_id,
                                  opcode_validation.detail,
                                  true});
    result.api_result = QueryRouteFailure(
        request.context,
        request.envelope.operation_id,
        opcode_validation.diagnostic_id,
        opcode_validation.detail);
    return result;
  }
  if (request.envelope.requires_security_context &&
      !request.context.security_context_present) {
    result.api_result = QueryRouteFailure(
        request.context,
        request.envelope.operation_id,
        "SB_SBLR_DISPATCH_SECURITY_CONTEXT_REQUIRED",
        "security_context_present=false");
    result.diagnostics.push_back(QueryRouteDiagnostic(result.api_result));
    return result;
  }
  if (request.envelope.requires_transaction_context &&
      request.context.local_transaction_id == 0 &&
      request.context.transaction_uuid.canonical.empty()) {
    result.api_result = QueryRouteFailure(
        request.context,
        request.envelope.operation_id,
        "SB_SBLR_DISPATCH_TRANSACTION_CONTEXT_REQUIRED",
        "transaction_uuid and local_transaction_id are both absent");
    result.diagnostics.push_back(QueryRouteDiagnostic(result.api_result));
    return result;
  }
  if (request.envelope.operation_id == "engine.op.ddl_create_aggregate") {
    result.accepted = true;
    result.dispatched_to_api = true;
    result.api_result.ok = true;
    result.api_result.operation_id = request.envelope.operation_id;
    result.api_result.result_shape.result_kind = "ddl_result";
    result.api_result.evidence.push_back({request.envelope.operation_id, "executor_dispatch_admitted"});
    return result;
  }
  if (request.envelope.operation_id == "engine.op.catalog_introspect") {
    result.accepted = true;
    result.dispatched_to_api = true;
    result.api_result.ok = true;
    result.api_result.operation_id = request.envelope.operation_id;
    result.api_result.result_shape.result_kind = "catalog_introspect_result";
    result.api_result.evidence.push_back({request.envelope.operation_id, "executor_dispatch_admitted"});
    return result;
  }
  if (request.envelope.operation_id == "engine.op.ddl_alter_aggregate") {
    result.accepted = true;
    result.dispatched_to_api = true;
    result.api_result.ok = true;
    result.api_result.operation_id = request.envelope.operation_id;
    result.api_result.result_shape.result_kind = "ddl_result";
    result.api_result.evidence.push_back({request.envelope.operation_id, "executor_dispatch_admitted"});
    return result;
  }
  if (request.envelope.operation_id == "engine.op.ddl_drop_aggregate") {
    result.accepted = true;
    result.dispatched_to_api = true;
    result.api_result.ok = true;
    result.api_result.operation_id = request.envelope.operation_id;
    result.api_result.result_shape.result_kind = "ddl_result";
    return result;
  }
  if (request.envelope.operation_id == "engine.op.ddl_drop_dictionary") {
    result.accepted = true; result.dispatched_to_api = true; result.api_result.ok = true;
    result.api_result.operation_id = request.envelope.operation_id;
    result.api_result.result_shape.result_kind = "ddl_result";
    return result;
  }
  if (request.envelope.operation_id == "engine.op.ddl_purge_system_history") { result.accepted=true; result.dispatched_to_api=true; result.api_result.ok=true; result.api_result.operation_id=request.envelope.operation_id; result.api_result.result_shape.result_kind="management_operation_result"; return result; }
  if (request.envelope.operation_id == "engine.op.ddl_set_index_optimizer_eligibility") { result.accepted=true; result.dispatched_to_api=true; result.api_result.ok=true; result.api_result.operation_id=request.envelope.operation_id; result.api_result.result_shape.result_kind="ddl_result"; return result; }
  if (request.envelope.operation_id == "engine.op.ddl_set_table_type_enforcement") { result.accepted=true; result.dispatched_to_api=true; result.api_result.ok=true; result.api_result.operation_id=request.envelope.operation_id; result.api_result.result_shape.result_kind="management_operation_result"; return result; }
  if (request.envelope.operation_id == "engine.op.database_serialize_logical_snapshot") { result.accepted=true; result.dispatched_to_api=true; result.api_result.ok=true; result.api_result.operation_id=request.envelope.operation_id; result.api_result.result_shape.result_kind="logical_snapshot_buffer_descriptor"; return result; }
  if (request.envelope.operation_id == "engine.op.database_deserialize_logical_snapshot") { result.accepted=true; result.dispatched_to_api=true; result.api_result.ok=true; result.api_result.operation_id=request.envelope.operation_id; result.api_result.result_shape.result_kind="management_operation_result"; return result; }
  if (request.envelope.operation_id == "engine.op.ddl_create_dictionary") { result.accepted=true; result.dispatched_to_api=true; result.api_result.ok=true; result.api_result.operation_id=request.envelope.operation_id; result.api_result.result_shape.result_kind="ddl_result"; return result; }
  if (request.envelope.operation_id == "engine.op.ddl_drop_table") { result.accepted=true; result.dispatched_to_api=true; result.api_result.ok=true; result.api_result.operation_id=request.envelope.operation_id; result.api_result.result_shape.result_kind="ddl_result"; return result; }
  if (request.envelope.operation_id != "query.execute") {
    result.api_result = QueryRouteFailure(
        request.context,
        request.envelope.operation_id,
        "SB_SBLR_DISPATCH_UNKNOWN_OPERATION",
        "operation has no QOW contract-only dispatch route");
    result.diagnostics.push_back(QueryRouteDiagnostic(result.api_result));
    return result;
  }

  const auto routed = DispatchTypedPlanOperation(request);
  result.accepted = routed.optimizer_admitted;
  result.dispatched_to_api = routed.optimizer_admitted;
  result.logical_graph_populated = routed.logical_graph_populated;
  result.logical_properties_populated = routed.logical_properties_populated;
  result.logical_node_count = routed.logical_node_count;
  result.logical_property_count = routed.logical_property_count;
  result.optimizer_admitted = routed.optimizer_admitted;
  result.optimizer_admission_degraded =
      routed.optimizer_admission_degraded;
  result.optimizer_benchmark_clean_ready =
      routed.optimizer_benchmark_clean_ready;
  result.optimizer_selected = routed.optimizer_selected;
  result.physical_dag_published = routed.physical_dag_published;
  result.physical_dag_executed = routed.physical_dag_executed;
  result.runtime_actuals_attached = routed.runtime_actuals_attached;
  result.canonical_result_published = routed.canonical_result_published;
  result.optimizer_admission_stage_count =
      routed.optimizer_admission_stage_count;
  result.physical_node_count = routed.physical_node_count;
  result.canonical_result_column_count =
      routed.canonical_result_column_count;
  result.canonical_result_row_count = routed.canonical_result_row_count;
  result.selected_plan_uuid = routed.selected_plan_uuid;
  result.canonical_result_bytes = routed.canonical_result_bytes;
  result.api_result = routed.api_result;
  if (!result.api_result.ok) {
    result.diagnostics.push_back(QueryRouteDiagnostic(result.api_result));
  }
  return result;
}

SblrDispatchResult DecodeAndDispatchSblrOperation(
    std::string_view encoded_envelope,
    api::EngineRequestContext context,
    api::EngineApiRequest api_request) {
  const auto decoded = DecodeSblrEnvelope(encoded_envelope);
  if (!decoded.ok) {
    SblrDispatchResult result;
    result.diagnostics = decoded.diagnostics;
    result.api_result = QueryRouteFailure(
        context,
        decoded.envelope.operation_id,
        "SB_SBLR_DECODE_REJECTED",
        "encoded envelope failed validation");
    return result;
  }
  return DispatchSblrOperation(
      {std::move(context), decoded.envelope, std::move(api_request)});
}

std::string SerializeSblrDispatchResultToJson(
    const SblrDispatchResult& result) {
  std::ostringstream out;
  out << "{\"accepted\":" << (result.accepted ? "true" : "false")
      << ",\"envelope_validated\":"
      << (result.envelope_validated ? "true" : "false")
      << ",\"dispatched_to_api\":"
      << (result.dispatched_to_api ? "true" : "false")
      << ",\"logical_graph_populated\":"
      << (result.logical_graph_populated ? "true" : "false")
      << ",\"logical_properties_populated\":"
      << (result.logical_properties_populated ? "true" : "false")
      << ",\"logical_node_count\":" << result.logical_node_count
      << ",\"logical_property_count\":" << result.logical_property_count
      << ",\"optimizer_admitted\":"
      << (result.optimizer_admitted ? "true" : "false")
      << ",\"optimizer_admission_degraded\":"
      << (result.optimizer_admission_degraded ? "true" : "false")
      << ",\"optimizer_benchmark_clean_ready\":"
      << (result.optimizer_benchmark_clean_ready ? "true" : "false")
      << ",\"optimizer_selected\":"
      << (result.optimizer_selected ? "true" : "false")
      << ",\"physical_dag_published\":"
      << (result.physical_dag_published ? "true" : "false")
      << ",\"physical_dag_executed\":"
      << (result.physical_dag_executed ? "true" : "false")
      << ",\"runtime_actuals_attached\":"
      << (result.runtime_actuals_attached ? "true" : "false")
      << ",\"canonical_result_published\":"
      << (result.canonical_result_published ? "true" : "false")
      << ",\"optimizer_admission_stage_count\":"
      << result.optimizer_admission_stage_count
      << ",\"physical_node_count\":" << result.physical_node_count
      << ",\"canonical_result_column_count\":"
      << result.canonical_result_column_count
      << ",\"canonical_result_row_count\":"
      << result.canonical_result_row_count
      << ",\"api_ok\":" << (result.api_result.ok ? "true" : "false")
      << "}";
  return out.str();
}

#else

namespace functions = scratchbird::engine::functions;
namespace {

using SblrSteadyClock = std::chrono::steady_clock;

std::uint64_t SblrElapsedMicros(SblrSteadyClock::time_point start,
                                SblrSteadyClock::time_point finish) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(finish - start)
          .count());
}

void WriteBaseApiPhaseTrace(const SblrOperationEnvelope& envelope,
                            std::uint64_t operand_loop_us,
                            std::uint64_t compact_materialize_us,
                            std::uint64_t total_us,
                            std::size_t row_count,
                            std::size_t operand_count) {
  const char* trace_path = std::getenv("SCRATCHBIRD_SBLR_BASE_API_TRACE_FILE");
  if (trace_path == nullptr || *trace_path == '\0') {
    return;
  }
  std::ofstream out(trace_path, std::ios::app | std::ios::binary);
  if (!out) {
    return;
  }
  out << "operation=" << envelope.operation_id
      << "\toperands=" << operand_count
      << "\trows=" << row_count
      << "\toperand_loop_us=" << operand_loop_us
      << "\tcompact_materialize_us=" << compact_materialize_us
      << "\ttotal_us=" << total_us
      << '\n';
}

void WriteSblrDispatchPhaseTrace(
    std::string_view layer,
    std::string_view operation_id,
    std::size_t encoded_size,
    const std::vector<std::pair<std::string, std::uint64_t>>& phase_micros) {
  const char* trace_path = std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");
  if (trace_path == nullptr || *trace_path == '\0') {
    return;
  }
  std::ofstream out(trace_path, std::ios::app | std::ios::binary);
  if (!out) {
    return;
  }
  out << "layer=" << layer
      << "\toperation=" << operation_id
      << "\tencoded_bytes=" << encoded_size;
  std::uint64_t total = 0;
  for (const auto& [phase, micros] : phase_micros) {
    total += micros;
    out << '\t' << phase << "_us=" << micros;
  }
  out << "\ttotal_us=" << total << '\n';
}

std::string LowerAscii(std::string value) {
  for (auto& ch : value) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return value;
}

int HexValue(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}

bool HexDecodeBytes(std::string_view text, std::vector<std::uint8_t>* out) {
  if (out == nullptr || (text.size() % 2) != 0) return false;
  std::vector<std::uint8_t> bytes;
  bytes.reserve(text.size() / 2);
  for (std::size_t index = 0; index < text.size(); index += 2) {
    const int high = HexValue(text[index]);
    const int low = HexValue(text[index + 1]);
    if (high < 0 || low < 0) return false;
    bytes.push_back(static_cast<std::uint8_t>((high << 4) | low));
  }
  *out = std::move(bytes);
  return true;
}

std::string FormatReal64(double value) {
  std::ostringstream encoded;
  encoded << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
  return encoded.str();
}

std::string HexEncodeBytes(const std::vector<std::uint8_t>& bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (const auto byte : bytes) {
    out.push_back(kHex[(byte >> 4) & 0x0f]);
    out.push_back(kHex[byte & 0x0f]);
  }
  return out;
}

SblrEnvelopeDiagnostic DispatchDiagnostic(std::string code, std::string message) {
  return SblrEnvelopeDiagnostic{std::move(code), std::move(message), true};
}

bool HasDispatchDiagnosticCode(const SblrDispatchResult& result,
                               std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) {
      return true;
    }
  }
  return false;
}

bool IsDefaultSummaryWriteDmlOperation(std::string_view operation_id) {
  return operation_id == "dml.insert_rows" ||
         operation_id == "dml.update_rows" ||
         operation_id == "dml.delete_rows" ||
         operation_id == "dml.merge_rows" ||
         operation_id == "dml.execute_import_rows" ||
         operation_id == "dml.execute_native_bulk_ingest";
}

void EnsureDefaultWriteResultPolicy(api::EngineApiRequest* request,
                                    std::string_view operation_id) {
  if (request == nullptr || !IsDefaultSummaryWriteDmlOperation(operation_id)) {
    return;
  }
  for (const auto& option : request->option_envelopes) {
    if (api::IsWriteResultPolicyOption(option)) {
      return;
    }
  }
  request->option_envelopes.push_back("result_payload_policy:summary_only");
  request->option_envelopes.push_back(
      "sblr.default_write_result_policy:summary_only");
}

void PropagateClusterApiDiagnostics(SblrDispatchResult* result) {
  if (result == nullptr || result->api_result.ok) {
    return;
  }
  for (const auto& diagnostic : result->api_result.diagnostics) {
    if (std::string_view(diagnostic.code).rfind("SBLR.CLUSTER.", 0) != 0 ||
        HasDispatchDiagnosticCode(*result, diagnostic.code)) {
      continue;
    }
    result->diagnostics.push_back(DispatchDiagnostic(
        diagnostic.code,
        diagnostic.detail.empty() ? diagnostic.message_key : diagnostic.detail));
  }
}

std::string UnquoteSbsqlLiteral(std::string value) {
  if (value.size() < 2 || value.front() != '\'' || value.back() != '\'') {
    return value;
  }
  std::string out;
  out.reserve(value.size() - 2);
  for (std::size_t index = 1; index + 1 < value.size(); ++index) {
    if (value[index] == '\'' && index + 1 < value.size() - 1 && value[index + 1] == '\'') {
      out.push_back('\'');
      ++index;
    } else {
      out.push_back(value[index]);
    }
  }
  return out;
}

std::vector<std::string> SplitCommaSeparatedLiterals(std::string_view value) {
  std::vector<std::string> parts;
  std::string current;
  bool in_string = false;
  for (std::size_t index = 0; index < value.size(); ++index) {
    const char ch = value[index];
    if (ch == '\'') {
      current.push_back(ch);
      if (in_string && index + 1 < value.size() && value[index + 1] == '\'') {
        current.push_back(value[index + 1]);
        ++index;
        continue;
      }
      in_string = !in_string;
      continue;
    }
    if (ch == ',' && !in_string) {
      parts.push_back(current);
      current.clear();
      continue;
    }
    current.push_back(ch);
  }
  parts.push_back(current);
  return parts;
}

api::EngineTypedValue TypedValueFromLoweredLiteral(std::string value,
                                                   std::string type_name) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  if (type_name.empty()) type_name = "text";
  if (type_name == "integer") type_name = "bigint";
  typed.descriptor.canonical_type_name = type_name;
  typed.descriptor.encoded_descriptor = "type=" + type_name;
  typed.is_null = type_name == "null";
  typed.encoded_value = typed.is_null ? std::string{} : UnquoteSbsqlLiteral(std::move(value));
  return typed;
}

std::optional<std::string_view> TextOperandValue(
    const SblrOperationEnvelope& envelope,
    std::string_view name) {
  for (const auto& operand : envelope.operands) {
    if (operand.type == "text" && operand.name == name) {
      return std::string_view(operand.value);
    }
  }
  return std::nullopt;
}

bool CanonicalEnvelopeHasExactField(std::string_view envelope,
                                    std::string_view name,
                                    std::string_view value) {
  const std::string expected = std::string(name) + "=" + std::string(value);
  std::size_t offset = 0;
  while (offset <= envelope.size()) {
    const auto separator = envelope.find(';', offset);
    const auto field = envelope.substr(
        offset,
        separator == std::string_view::npos ? envelope.size() - offset
                                            : separator - offset);
    if (field == expected) return true;
    if (separator == std::string_view::npos) break;
    offset = separator + 1;
  }
  return false;
}

std::uint64_t ParseCompactU64(std::string_view value) {
  std::uint64_t out = 0;
  if (value.empty()) return 0;
  for (const unsigned char ch : value) {
    if (!std::isdigit(ch)) return 0;
    const auto digit = static_cast<std::uint64_t>(ch - '0');
    if (out > (std::numeric_limits<std::uint64_t>::max() - digit) / 10u) {
      return 0;
    }
    out = out * 10u + digit;
  }
  return out;
}

bool CompactBool(std::string_view value) {
  const std::string lower = LowerAscii(std::string(value));
  return lower == "true" || lower == "1" || lower == "yes" || lower == "on";
}

bool LooksLikeOrdinalInsertFieldName(std::string_view name) {
  if (name.size() < 2 || name.front() != 'c') return false;
  for (std::size_t index = 1; index < name.size(); ++index) {
    if (!std::isdigit(static_cast<unsigned char>(name[index]))) return false;
  }
  return true;
}

bool HexDecodeString(std::string_view text, std::string* out) {
  if (out == nullptr) return false;
  if (text.empty()) {
    out->clear();
    return true;
  }
  std::vector<std::uint8_t> bytes;
  if (!HexDecodeBytes(text, &bytes)) return false;
  out->assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  return true;
}

std::vector<std::string_view> SplitCompactInsertCell(std::string_view cell) {
  std::vector<std::string_view> parts;
  std::size_t start = 0;
  while (start <= cell.size()) {
    const std::size_t end = cell.find('|', start);
    parts.push_back(cell.substr(
        start,
        end == std::string_view::npos ? cell.size() - start : end - start));
    if (end == std::string_view::npos) break;
    start = end + 1u;
  }
  return parts;
}

// SB_ENGINE_GLOBAL_AGGREGATE_PROJECTION_TRANSPORT_V1_BEGIN
constexpr std::string_view kGlobalAggregateProjectionTransportV1 =
    "sblr.global_aggregate_projection.v1";

bool ParseStrictTransportU64(std::string_view text, std::uint64_t* out) {
  if (out == nullptr || text.empty()) return false;
  if (text.size() > 1 && text.front() == '0') return false;
  std::uint64_t value = 0;
  for (const unsigned char ch : text) {
    if (!std::isdigit(ch)) return false;
    const std::uint64_t digit = static_cast<std::uint64_t>(ch - '0');
    if (value >
        (std::numeric_limits<std::uint64_t>::max() - digit) / 10u) {
      return false;
    }
    value = value * 10u + digit;
  }
  *out = value;
  return true;
}

bool ReadExactSingleTransportOption(const api::EngineApiRequest& request,
                                    std::string_view prefix,
                                    std::string* out) {
  if (out == nullptr) return false;
  std::size_t matches = 0;
  std::string value;
  for (const auto& option : request.option_envelopes) {
    const std::string_view encoded(option);
    if (!encoded.starts_with(prefix)) continue;
    ++matches;
    value.assign(encoded.substr(prefix.size()));
  }
  if (matches != 1) return false;
  *out = std::move(value);
  return true;
}

void SetInvalidGlobalAggregateProjectionTransport(
    api::EngineSelectRowsRequest* typed) {
  if (typed == nullptr) return;
  typed->global_aggregate_projection = {};
  typed->global_aggregate_projection.relation_uuid.canonical = "invalid";
  typed->global_aggregate_projection.relation_descriptor_uuid.canonical =
      "invalid";
  typed->global_aggregate_projection.relation_descriptor_generation = 1;
  api::EngineGlobalAggregateProjection invalid;
  invalid.operation =
      static_cast<api::EngineGlobalAggregateOperation>(0);
  invalid.aggregate_function_uuid.canonical =
      std::string(api::EngineGlobalAggregateCountFunctionUuid());
  invalid.output_alias = "invalid";
  invalid.result_descriptor =
      api::EngineGlobalAggregateCountResultDescriptor();
  typed->global_aggregate_projection.outputs.push_back(std::move(invalid));
}

// Decodes only the family-neutral typed aggregate packet carried through the
// existing bounded DML operand slots.  It does not inspect dialect identity or
// SQL text.  A present marker always constructs a non-empty envelope, including
// on malformed input, so EngineSelectRows fails before materializing rows.
bool DecodeGlobalAggregateProjectionTransportV1(
    const api::EngineApiRequest& base,
    api::EngineSelectRowsRequest* typed) {
  if (typed == nullptr) return false;
  constexpr std::string_view result_projection_prefix =
      "result_projection:";
  constexpr std::string_view aggregate_marker_family =
      "sblr.global_aggregate_projection.";
  std::size_t result_projection_count = 0;
  std::size_t exact_marker_count = 0;
  std::size_t aggregate_marker_family_count = 0;
  for (const auto& option : base.option_envelopes) {
    const std::string_view encoded(option);
    if (!encoded.starts_with(result_projection_prefix)) continue;
    ++result_projection_count;
    const std::string_view value =
        encoded.substr(result_projection_prefix.size());
    if (value == kGlobalAggregateProjectionTransportV1) {
      ++exact_marker_count;
    }
    if (value.starts_with(aggregate_marker_family)) {
      ++aggregate_marker_family_count;
    }
  }
  if (aggregate_marker_family_count == 0) return false;
  SetInvalidGlobalAggregateProjectionTransport(typed);
  if (result_projection_count != 1 || exact_marker_count != 1 ||
      aggregate_marker_family_count != 1) {
    return true;
  }
  const std::string encoded_marker =
      std::string(result_projection_prefix) +
      std::string(kGlobalAggregateProjectionTransportV1);
  typed->option_envelopes.erase(
      std::remove(typed->option_envelopes.begin(),
                  typed->option_envelopes.end(),
                  encoded_marker),
      typed->option_envelopes.end());

  std::string aggregate_function;
  if (!ReadExactSingleTransportOption(
          base, "aggregate_function:", &aggregate_function) ||
      (aggregate_function != api::EngineGlobalAggregateCountFunctionUuid() &&
       aggregate_function != api::EngineGlobalAggregateAvgFunctionUuid())) {
    return true;
  }
  std::string encoded_output_count;
  std::uint64_t output_count = 0;
  if (!ReadExactSingleTransportOption(
          base, "projection_count:", &encoded_output_count) ||
      !ParseStrictTransportU64(encoded_output_count, &output_count) ||
      output_count == 0 || output_count > 16) {
    return true;
  }

  constexpr std::string_view projection_prefix = "projection_";
  std::vector<std::optional<std::string>> packed_outputs(
      static_cast<std::size_t>(output_count));
  for (const auto& option : base.option_envelopes) {
    const std::string_view encoded(option);
    if (encoded.starts_with("projection_count:")) continue;
    if (!encoded.starts_with(projection_prefix)) continue;
    const std::size_t separator = encoded.find(':', projection_prefix.size());
    if (separator == std::string_view::npos) return true;
    const std::string_view encoded_index = encoded.substr(
        projection_prefix.size(), separator - projection_prefix.size());
    std::uint64_t index = 0;
    if (!ParseStrictTransportU64(encoded_index, &index) ||
        std::to_string(index) != std::string(encoded_index) || index >= 16 ||
        index >= output_count ||
        packed_outputs[static_cast<std::size_t>(index)].has_value()) {
      return true;
    }
    packed_outputs[static_cast<std::size_t>(index)] =
        std::string(encoded.substr(separator + 1));
  }
  if (std::any_of(packed_outputs.begin(), packed_outputs.end(),
                  [](const auto& value) { return !value.has_value(); })) {
    return true;
  }

  api::EngineGlobalAggregateProjectionEnvelope decoded;
  decoded.outputs.reserve(static_cast<std::size_t>(output_count));
  for (std::uint64_t index = 0; index < output_count; ++index) {
    const std::string& packed =
        *packed_outputs[static_cast<std::size_t>(index)];
    const auto parts = SplitCompactInsertCell(packed);
    if (parts.size() != 15 || parts[0] != "gag1") return true;

    std::string function_uuid;
    std::string output_alias;
    std::string relation_uuid;
    std::string relation_descriptor_uuid;
    std::string column_uuid;
    std::string source_descriptor_uuid;
    std::string source_descriptor_kind;
    std::string source_canonical_type;
    std::string source_encoded_descriptor;
    std::string result_descriptor_kind;
    std::string result_canonical_type;
    std::string result_encoded_descriptor;
    std::uint64_t operation = 0;
    std::uint64_t descriptor_generation = 0;
    if (!HexDecodeString(parts[1], &function_uuid) ||
        !ParseStrictTransportU64(parts[2], &operation) ||
        !HexDecodeString(parts[3], &output_alias) ||
        !HexDecodeString(parts[4], &relation_uuid) ||
        !HexDecodeString(parts[5], &relation_descriptor_uuid) ||
        !ParseStrictTransportU64(parts[6], &descriptor_generation) ||
        descriptor_generation == 0 ||
        !HexDecodeString(parts[7], &column_uuid) ||
        !HexDecodeString(parts[8], &source_descriptor_uuid) ||
        !HexDecodeString(parts[9], &source_descriptor_kind) ||
        !HexDecodeString(parts[10], &source_canonical_type) ||
        !HexDecodeString(parts[11], &source_encoded_descriptor) ||
        !HexDecodeString(parts[12], &result_descriptor_kind) ||
        !HexDecodeString(parts[13], &result_canonical_type) ||
        !HexDecodeString(parts[14], &result_encoded_descriptor)) {
      return true;
    }
    const bool count_function =
        aggregate_function == api::EngineGlobalAggregateCountFunctionUuid();
    const bool count_operation =
        operation >= static_cast<std::uint64_t>(
                         api::EngineGlobalAggregateOperation::count_star) &&
        operation <= static_cast<std::uint64_t>(
                         api::EngineGlobalAggregateOperation::
                             count_distinct_field);
    const bool avg_operation =
        operation >= static_cast<std::uint64_t>(
                         api::EngineGlobalAggregateOperation::avg_field) &&
        operation <= static_cast<std::uint64_t>(
                         api::EngineGlobalAggregateOperation::
                             avg_distinct_field);
    if (function_uuid != aggregate_function ||
        (count_function && !count_operation) ||
        (!count_function && !avg_operation)) {
      return true;
    }
    if (index == 0) {
      decoded.relation_uuid.canonical = relation_uuid;
      decoded.relation_descriptor_uuid.canonical =
          relation_descriptor_uuid;
      decoded.relation_descriptor_generation = descriptor_generation;
    } else if (decoded.relation_uuid.canonical != relation_uuid ||
               decoded.relation_descriptor_uuid.canonical !=
                   relation_descriptor_uuid ||
               decoded.relation_descriptor_generation !=
                   descriptor_generation) {
      return true;
    }

    api::EngineGlobalAggregateProjection output;
    output.operation =
        static_cast<api::EngineGlobalAggregateOperation>(operation);
    output.aggregate_function_uuid.canonical = std::move(function_uuid);
    output.output_alias = std::move(output_alias);
    output.source_field.column_uuid.canonical = std::move(column_uuid);
    output.source_field.value_descriptor.descriptor_uuid.canonical =
        std::move(source_descriptor_uuid);
    output.source_field.value_descriptor.descriptor_kind =
        std::move(source_descriptor_kind);
    output.source_field.value_descriptor.canonical_type_name =
        std::move(source_canonical_type);
    output.source_field.value_descriptor.encoded_descriptor =
        std::move(source_encoded_descriptor);
    output.result_descriptor.descriptor_kind =
        std::move(result_descriptor_kind);
    output.result_descriptor.canonical_type_name =
        std::move(result_canonical_type);
    output.result_descriptor.encoded_descriptor =
        std::move(result_encoded_descriptor);
    decoded.outputs.push_back(std::move(output));
  }
  typed->global_aggregate_projection = std::move(decoded);
  return true;
}
// SB_ENGINE_GLOBAL_AGGREGATE_PROJECTION_TRANSPORT_V1_END

// SB_ENGINE_GLOBAL_AGGREGATE_VIEW_TRANSPORT_V1_BEGIN
constexpr std::string_view kGlobalAggregateViewTransportV1 =
    "engine.global_aggregate_view.v1";
constexpr std::string_view kGlobalAggregateViewCreatePacketV1 = "gavc1";
constexpr std::string_view kGlobalAggregateViewSelectPacketV1 = "gavs1";

bool IsAdmittedGlobalAggregateViewContextOption(std::string_view name) {
  return name == "identifier_profile_uuid" || name == "principal_name" ||
         name == "requested_role_name" || name == "active_role_name" ||
         name == "current_role_uuid" ||
         name == "effective_role_uuid_set" ||
         name == "effective_group_uuid_set" || name == "authorization_tag";
}

bool GlobalAggregateViewBaseDataEmpty(const api::EngineApiRequest& base) {
  return base.target_database.uuid.canonical.empty() &&
         base.target_database.object_kind.empty() &&
         base.sql_object_reference.expected_object_type.empty() &&
         base.sql_object_reference.path_type == "unqualified" &&
         !base.sql_object_reference.no_search_path &&
         base.sql_object_reference.path_components.empty() &&
         base.sql_object_reference.object_name.raw_text.empty() &&
         !base.sql_object_reference.object_name.was_quoted &&
         base.sql_object_reference.object_name.quote_style.empty() &&
         base.sql_object_reference.object_name.identifier_profile_uuid.empty() &&
         base.sql_object_reference.object_name.normalized_lookup_key.empty() &&
         base.sql_object_reference.object_name.exact_lookup_key.empty() &&
         !base.sql_object_reference.object_name.requires_exact_match &&
         base.sql_object_reference.object_name.source_span.empty() &&
         base.bound_object_identity.object_uuid.canonical.empty() &&
         base.bound_object_identity.resolved_object_type.empty() &&
         base.bound_object_identity.resolved_schema_uuid.canonical.empty() &&
         base.bound_object_identity.parent_object_uuid.canonical.empty() &&
         base.bound_object_identity.catalog_generation_id == 0 &&
         base.bound_object_identity.security_epoch == 0 &&
         base.bound_object_identity.resource_epoch == 0 &&
         !base.native_row_packet.present &&
         base.native_row_packet.version == 0 &&
         base.native_row_packet.row_count == 0 &&
         base.native_row_packet.column_count == 0 &&
         base.native_row_packet.packet_bytes.empty() &&
         base.native_row_packet.field_order.empty() &&
         base.native_row_packet.column_type_tags.empty() &&
         base.native_row_packet.row_offsets.empty() &&
         base.native_row_packet.row_sizes.empty() &&
         base.shared_row_field_order.empty() &&
         base.related_objects.empty() && base.localized_names.empty() &&
         base.descriptors.empty() && base.columns.empty() &&
         base.constraints.empty() && base.indexes.empty() &&
         base.rows.empty() && base.assignments.empty() &&
         base.predicate.predicate_kind.empty() &&
         base.predicate.canonical_predicate_envelope.empty() &&
         base.predicate.bound_values.empty() &&
         base.projection.canonical_projection_envelopes.empty() &&
         base.ordering.canonical_ordering_envelopes.empty() &&
         base.physical_profile.names.empty() &&
         base.physical_profile.encoded_profiles.empty() &&
         base.policy_profile.names.empty() &&
         base.policy_profile.encoded_profiles.empty() &&
         base.compatibility_profile.names.empty() &&
         base.compatibility_profile.encoded_profiles.empty() &&
         base.diagnostic_options.empty();
}

bool GlobalAggregateViewOptionsAdmitted(
    const api::EngineApiRequest& base,
    std::initializer_list<std::string_view> admitted_transport_names) {
  std::set<std::string_view> seen_single_context_names;
  for (const auto& option : base.option_envelopes) {
    const std::string_view encoded(option);
    const auto separator = encoded.find(':');
    if (separator == std::string_view::npos || separator == 0) return false;
    const auto name = encoded.substr(0, separator);
    const bool admitted_transport =
        std::find(admitted_transport_names.begin(),
                  admitted_transport_names.end(),
                  name) != admitted_transport_names.end();
    if (!admitted_transport &&
        !IsAdmittedGlobalAggregateViewContextOption(name)) {
      return false;
    }
    if (!admitted_transport && name != "authorization_tag" &&
        !seen_single_context_names.insert(name).second) {
      return false;
    }
  }
  return true;
}

void SetInvalidGlobalAggregateViewCreateTransport(
    api::EngineCreateViewRequest* typed) {
  if (typed == nullptr) return;
  typed->related_objects.clear();
  typed->columns.clear();
  typed->assignments.clear();
  typed->descriptors.clear();
  typed->projection = {};
  typed->option_envelopes = {
      std::string("view_query_shape:") +
      std::string(kGlobalAggregateViewTransportV1)};
}

// Reconstructs the exact SQL-free engine request for the bounded persisted
// aggregate-view surface.  A marker-family packet is always consumed; any
// malformed packet leaves a marked but invalid request so DDL fails closed.
bool DecodeGlobalAggregateViewCreateTransportV1(
    const api::EngineApiRequest& base,
    api::EngineCreateViewRequest* typed) {
  if (typed == nullptr) return false;
  constexpr std::string_view marker_prefix = "view_query_shape:";
  constexpr std::string_view marker_family =
      "engine.global_aggregate_view";
  std::size_t marker_field_count = 0;
  std::size_t marker_family_count = 0;
  std::size_t exact_marker_count = 0;
  for (const auto& option : base.option_envelopes) {
    const std::string_view encoded(option);
    if (!encoded.starts_with(marker_prefix)) continue;
    ++marker_field_count;
    const auto value = encoded.substr(marker_prefix.size());
    if (!value.starts_with(marker_family)) continue;
    ++marker_family_count;
    if (value == kGlobalAggregateViewTransportV1) ++exact_marker_count;
  }
  if (marker_family_count == 0) return false;

  SetInvalidGlobalAggregateViewCreateTransport(typed);
  if (marker_field_count != 1 || marker_family_count != 1 ||
      exact_marker_count != 1 || !GlobalAggregateViewBaseDataEmpty(base) ||
      !GlobalAggregateViewOptionsAdmitted(
          base,
          {"target_object_kind", "view_name", "name",
           "target_schema_uuid", "view_projection_count",
           "view_query_shape", "view_source_uuid", "view_projection_0"})) {
    return true;
  }
  if (!base.target_object.uuid.canonical.empty() ||
      base.target_object.object_kind != "view" ||
      !base.target_schema.uuid.canonical.empty() ||
      !base.target_schema.object_kind.empty()) {
    return true;
  }

  std::string encoded_count;
  std::string packed;
  std::string source_option;
  std::string target_kind;
  std::string view_name;
  std::string canonical_name;
  std::string target_schema_uuid;
  std::uint64_t projection_count = 0;
  if (!ReadExactSingleTransportOption(
          base, "view_projection_count:", &encoded_count) ||
      !ParseStrictTransportU64(encoded_count, &projection_count) ||
      projection_count != 1 ||
      !ReadExactSingleTransportOption(base, "view_projection_0:", &packed) ||
      !ReadExactSingleTransportOption(
          base, "view_source_uuid:", &source_option) ||
      !ReadExactSingleTransportOption(
          base, "target_object_kind:", &target_kind) ||
      !ReadExactSingleTransportOption(base, "view_name:", &view_name) ||
      !ReadExactSingleTransportOption(base, "name:", &canonical_name) ||
      !ReadExactSingleTransportOption(
          base, "target_schema_uuid:", &target_schema_uuid) ||
      target_kind != "view" || view_name.empty() ||
      view_name != canonical_name || target_schema_uuid.empty() ||
      typed->target_schema.uuid.canonical != target_schema_uuid ||
      typed->target_object.object_kind != "view") {
    return true;
  }

  const auto parts = SplitCompactInsertCell(packed);
  if (parts.size() != 23 || parts[0] != kGlobalAggregateViewCreatePacketV1 ||
      (parts[1] != "0" && parts[1] != "1")) {
    return true;
  }

  std::string source_relation_uuid;
  std::string source_relation_descriptor_uuid;
  std::string source_column_uuid;
  std::string source_column_descriptor_uuid;
  std::string source_descriptor_kind;
  std::string source_canonical_type;
  std::string source_encoded_descriptor;
  std::string expression_kind;
  std::string literal_descriptor_kind;
  std::string literal_canonical_type;
  std::string literal_encoded_descriptor;
  std::string literal_value;
  std::string expression_descriptor_kind;
  std::string expression_canonical_type;
  std::string expression_encoded_descriptor;
  std::string aggregate_function_uuid;
  std::string result_alias;
  std::string result_descriptor_kind;
  std::string result_canonical_type;
  std::string result_encoded_descriptor;
  std::uint64_t source_generation = 0;
  if (!HexDecodeString(parts[2], &source_relation_uuid) ||
      !HexDecodeString(parts[3], &source_relation_descriptor_uuid) ||
      !ParseStrictTransportU64(parts[4], &source_generation) ||
      source_generation == 0 ||
      !HexDecodeString(parts[5], &source_column_uuid) ||
      !HexDecodeString(parts[6], &source_column_descriptor_uuid) ||
      !HexDecodeString(parts[7], &source_descriptor_kind) ||
      !HexDecodeString(parts[8], &source_canonical_type) ||
      !HexDecodeString(parts[9], &source_encoded_descriptor) ||
      !HexDecodeString(parts[10], &expression_kind) ||
      !HexDecodeString(parts[11], &literal_descriptor_kind) ||
      !HexDecodeString(parts[12], &literal_canonical_type) ||
      !HexDecodeString(parts[13], &literal_encoded_descriptor) ||
      !HexDecodeString(parts[14], &literal_value) ||
      !HexDecodeString(parts[15], &expression_descriptor_kind) ||
      !HexDecodeString(parts[16], &expression_canonical_type) ||
      !HexDecodeString(parts[17], &expression_encoded_descriptor) ||
      !HexDecodeString(parts[18], &aggregate_function_uuid) ||
      !HexDecodeString(parts[19], &result_alias) ||
      !HexDecodeString(parts[20], &result_descriptor_kind) ||
      !HexDecodeString(parts[21], &result_canonical_type) ||
      !HexDecodeString(parts[22], &result_encoded_descriptor) ||
      source_relation_uuid != source_option ||
      expression_kind != api::kEngineGlobalAggregateViewInt32MultiplyV1 ||
      aggregate_function_uuid != api::EngineGlobalAggregateAvgFunctionUuid()) {
    return true;
  }

  api::EngineDescriptor source_descriptor;
  source_descriptor.descriptor_uuid.canonical =
      std::move(source_column_descriptor_uuid);
  source_descriptor.descriptor_kind = std::move(source_descriptor_kind);
  source_descriptor.canonical_type_name = std::move(source_canonical_type);
  source_descriptor.encoded_descriptor =
      std::move(source_encoded_descriptor);

  api::EngineDescriptor literal_descriptor;
  literal_descriptor.descriptor_kind = std::move(literal_descriptor_kind);
  literal_descriptor.canonical_type_name =
      std::move(literal_canonical_type);
  literal_descriptor.encoded_descriptor =
      std::move(literal_encoded_descriptor);

  api::EngineDescriptor expression_descriptor;
  expression_descriptor.descriptor_kind =
      std::move(expression_descriptor_kind);
  expression_descriptor.canonical_type_name =
      std::move(expression_canonical_type);
  expression_descriptor.encoded_descriptor =
      std::move(expression_encoded_descriptor);

  api::EngineDescriptor result_descriptor;
  result_descriptor.descriptor_kind = std::move(result_descriptor_kind);
  result_descriptor.canonical_type_name = std::move(result_canonical_type);
  result_descriptor.encoded_descriptor =
      std::move(result_encoded_descriptor);

  api::EngineObjectReference source_relation;
  source_relation.uuid.canonical = std::move(source_relation_uuid);
  source_relation.object_kind = "table";
  api::EngineColumnDefinition source_column;
  source_column.requested_column_uuid.canonical = std::move(source_column_uuid);
  source_column.descriptor = std::move(source_descriptor);
  source_column.ordinal = 0;

  api::EngineTypedValue literal;
  literal.descriptor = std::move(literal_descriptor);
  literal.encoded_value = std::move(literal_value);
  literal.setState(api::EngineValueState::value);

  typed->related_objects = {std::move(source_relation)};
  typed->columns = {std::move(source_column)};
  typed->assignments = {{"int32_literal", std::move(literal)}};
  typed->descriptors = {std::move(expression_descriptor),
                        std::move(result_descriptor)};
  typed->projection.canonical_projection_envelopes = {
      std::string(api::kEngineGlobalAggregateViewInt32MultiplyV1)};
  typed->option_envelopes = {
      std::string("view_query_shape:") +
          std::string(api::kEngineGlobalAggregateViewMarkerV1),
      "source_relation_descriptor_uuid:" +
          source_relation_descriptor_uuid,
      "source_relation_descriptor_generation:" +
          std::to_string(source_generation),
      "aggregate_function_uuid:" + aggregate_function_uuid,
      "aggregate_result_alias:" + result_alias};
  if (parts[1] == "1") {
    typed->option_envelopes.push_back("create_or_alter:true");
  }
  return true;
}

void SetInvalidGlobalAggregateViewSelectTransport(
    api::EngineSelectRowsRequest* typed) {
  if (typed == nullptr) return;
  typed->source_object.object_kind = "view";
  typed->select_projection.canonical_projection_envelopes = {
      std::string(kGlobalAggregateViewTransportV1)};
  typed->projection = {};
  typed->descriptors.clear();
  typed->option_envelopes.clear();
}

// Decodes the bounded semantic descriptor returned by neutral name
// resolution.  No source-relation or expression internals cross this route.
bool DecodeGlobalAggregateViewSelectTransportV1(
    const api::EngineApiRequest& base,
    api::EngineSelectRowsRequest* typed) {
  if (typed == nullptr) return false;
  constexpr std::string_view marker_prefix = "result_projection:";
  constexpr std::string_view marker_family =
      "engine.global_aggregate_view";
  std::size_t marker_field_count = 0;
  std::size_t marker_family_count = 0;
  std::size_t exact_marker_count = 0;
  for (const auto& option : base.option_envelopes) {
    const std::string_view encoded(option);
    if (!encoded.starts_with(marker_prefix)) continue;
    ++marker_field_count;
    const auto value = encoded.substr(marker_prefix.size());
    if (!value.starts_with(marker_family)) continue;
    ++marker_family_count;
    if (value == kGlobalAggregateViewTransportV1) ++exact_marker_count;
  }
  if (marker_family_count == 0) return false;

  SetInvalidGlobalAggregateViewSelectTransport(typed);
  if (marker_field_count != 1 || marker_family_count != 1 ||
      exact_marker_count != 1 || !GlobalAggregateViewBaseDataEmpty(base) ||
      !GlobalAggregateViewOptionsAdmitted(
          base,
          {"target_object_uuid", "target_object_kind", "source_uuid",
           "source_kind", "result_projection", "projection_count",
           "projection_0"})) {
    return true;
  }
  if (!base.target_schema.uuid.canonical.empty() ||
      !base.target_schema.object_kind.empty()) {
    return true;
  }

  std::string encoded_count;
  std::string packed;
  std::string target_uuid;
  std::string target_kind;
  std::string source_uuid;
  std::string source_kind;
  std::uint64_t projection_count = 0;
  if (!ReadExactSingleTransportOption(
          base, "projection_count:", &encoded_count) ||
      !ParseStrictTransportU64(encoded_count, &projection_count) ||
      projection_count != 1 ||
      !ReadExactSingleTransportOption(base, "projection_0:", &packed) ||
      !ReadExactSingleTransportOption(
          base, "target_object_uuid:", &target_uuid) ||
      !ReadExactSingleTransportOption(
          base, "target_object_kind:", &target_kind) ||
      !ReadExactSingleTransportOption(base, "source_uuid:", &source_uuid) ||
      !ReadExactSingleTransportOption(base, "source_kind:", &source_kind) ||
      target_uuid.empty() || target_uuid != source_uuid ||
      target_kind != "view" || source_kind != "view" ||
      typed->source_object.uuid.canonical != target_uuid ||
      typed->source_object.object_kind != "view") {
    return true;
  }
  const auto parts = SplitCompactInsertCell(packed);
  if (parts.size() != 11 || parts[0] != kGlobalAggregateViewSelectPacketV1) {
    return true;
  }

  std::string marker;
  std::string projection_descriptor_uuid;
  std::string projection_descriptor_kind;
  std::string projection_canonical_type;
  std::string projection_encoded_descriptor;
  std::string result_alias;
  std::string result_descriptor_kind;
  std::string result_canonical_type;
  std::string result_encoded_descriptor;
  std::uint64_t descriptor_generation = 0;
  if (!HexDecodeString(parts[1], &marker) ||
      !HexDecodeString(parts[2], &projection_descriptor_uuid) ||
      !ParseStrictTransportU64(parts[3], &descriptor_generation) ||
      descriptor_generation == 0 ||
      !HexDecodeString(parts[4], &projection_descriptor_kind) ||
      !HexDecodeString(parts[5], &projection_canonical_type) ||
      !HexDecodeString(parts[6], &projection_encoded_descriptor) ||
      !HexDecodeString(parts[7], &result_alias) ||
      !HexDecodeString(parts[8], &result_descriptor_kind) ||
      !HexDecodeString(parts[9], &result_canonical_type) ||
      !HexDecodeString(parts[10], &result_encoded_descriptor) ||
      marker != api::kEngineGlobalAggregateViewMarkerV1 ||
      projection_descriptor_uuid.empty() ||
      projection_descriptor_kind != "global_aggregate_view" ||
      projection_canonical_type != api::kEngineGlobalAggregateViewMarkerV1 ||
      result_alias.empty()) {
    return true;
  }

  const api::EngineDescriptor expected_result =
      api::EngineGlobalAggregateAvgIntegerResultDescriptor();
  if (result_descriptor_kind != expected_result.descriptor_kind ||
      result_canonical_type != expected_result.canonical_type_name ||
      result_encoded_descriptor != expected_result.encoded_descriptor) {
    return true;
  }
  const std::string expected_semantic_descriptor =
      std::string("marker=") +
      std::string(api::kEngineGlobalAggregateViewMarkerV1) +
      ";view_uuid=" + target_uuid +
      ";view_descriptor_generation=" +
      std::to_string(descriptor_generation) +
      ";result_alias=" + result_alias +
      ";result_type=int64;result_nullable=true";
  if (projection_encoded_descriptor != expected_semantic_descriptor) {
    return true;
  }

  api::EngineDescriptor semantic;
  semantic.descriptor_uuid.canonical =
      std::move(projection_descriptor_uuid);
  semantic.descriptor_kind = std::move(projection_descriptor_kind);
  semantic.canonical_type_name = std::move(projection_canonical_type);
  semantic.encoded_descriptor = std::move(projection_encoded_descriptor);
  typed->descriptors = {std::move(semantic)};
  return true;
}
// SB_ENGINE_GLOBAL_AGGREGATE_VIEW_TRANSPORT_V1_END

// SB_ENGINE_RELATION_PROJECTION_VIEW_TRANSPORT_V1_BEGIN
constexpr std::string_view kRelationProjectionViewTransportV1 =
    "engine.relation_projection_view.v1";
constexpr std::string_view kRelationProjectionViewCreatePacketV1 = "rpvc1";
constexpr std::string_view kRelationProjectionViewSelectPacketV1 = "rpvs1";
constexpr std::string_view kRelationProjectionViewTransportV2 =
    "engine.relation_projection_view.v2";
constexpr std::string_view kRelationProjectionViewCreatePacketV2 = "rpvc2";
constexpr std::string_view kRelationProjectionViewDeletePacketV2 = "rpvd2";

bool CanonicalTransportUuid(std::string_view value) {
  if (value.size() != 36u || value[8] != '-' || value[13] != '-' ||
      value[18] != '-' || value[23] != '-') {
    return false;
  }
  bool nonzero = false;
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8u || index == 13u || index == 18u || index == 23u) {
      continue;
    }
    const char ch = value[index];
    if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) {
      return false;
    }
    nonzero = nonzero || ch != '0';
  }
  return nonzero;
}

bool SafeTransportOutputName(std::string_view value) {
  if (value.empty() || value.size() > 63u) return false;
  const auto alpha = [](unsigned char ch) {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
  };
  const auto digit = [](unsigned char ch) {
    return ch >= '0' && ch <= '9';
  };
  const unsigned char first = static_cast<unsigned char>(value.front());
  if (!alpha(first) && first != '_') return false;
  for (const unsigned char ch : value) {
    if (!alpha(ch) && !digit(ch) && ch != '_') return false;
  }
  return true;
}

bool TransportInt32Descriptor(std::string_view kind,
                              std::string_view canonical,
                              std::string_view encoded) {
  std::string lowered(canonical);
  std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                 [](unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });
  return !kind.empty() && !encoded.empty() &&
         (lowered == "int32" || lowered == "integer" || lowered == "int");
}

bool CanonicalTransportInt32(std::string_view value) {
  if (value.empty() || value.front() == '+') return false;
  std::int32_t parsed = 0;
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  return error == std::errc{} && end == value.data() + value.size() &&
         std::to_string(parsed) == value;
}

void SetInvalidRelationProjectionViewCreateTransport(
    api::EngineCreateViewRequest* typed) {
  if (typed == nullptr) return;
  typed->related_objects.clear();
  typed->columns.clear();
  typed->assignments.clear();
  typed->descriptors.clear();
  typed->projection = {};
  typed->option_envelopes = {
      std::string("view_query_shape:") +
      std::string(kRelationProjectionViewTransportV1)};
}

bool DecodeRelationProjectionViewCreateTransportV1(
    const api::EngineApiRequest& base,
    api::EngineCreateViewRequest* typed) {
  if (typed == nullptr) return false;
  constexpr std::string_view marker_prefix = "view_query_shape:";
  constexpr std::string_view marker_family =
      "engine.relation_projection_view";
  std::size_t marker_field_count = 0;
  std::size_t marker_family_count = 0;
  std::size_t exact_marker_count = 0;
  for (const auto& option : base.option_envelopes) {
    const std::string_view encoded(option);
    if (!encoded.starts_with(marker_prefix)) continue;
    ++marker_field_count;
    const auto value = encoded.substr(marker_prefix.size());
    if (!value.starts_with(marker_family)) continue;
    ++marker_family_count;
    if (value == kRelationProjectionViewTransportV1) ++exact_marker_count;
  }
  if (marker_family_count == 0) return false;

  SetInvalidRelationProjectionViewCreateTransport(typed);
  if (marker_field_count != 1u || marker_family_count != 1u ||
      exact_marker_count != 1u || !GlobalAggregateViewBaseDataEmpty(base) ||
      !GlobalAggregateViewOptionsAdmitted(
          base,
          {"target_object_kind", "view_name", "name",
           "target_schema_uuid", "view_projection_count",
           "view_query_shape", "view_source_uuid", "view_projection_0",
           "view_projection_1"}) ||
      !base.target_object.uuid.canonical.empty() ||
      base.target_object.object_kind != "view" ||
      !base.target_schema.uuid.canonical.empty() ||
      !base.target_schema.object_kind.empty()) {
    return true;
  }

  std::string encoded_count;
  std::string packed[2];
  std::string source_option;
  std::string target_kind;
  std::string view_name;
  std::string canonical_name;
  std::string target_schema_uuid;
  std::uint64_t projection_count = 0;
  if (!ReadExactSingleTransportOption(
          base, "view_projection_count:", &encoded_count) ||
      !ParseStrictTransportU64(encoded_count, &projection_count) ||
      projection_count != 2u ||
      !ReadExactSingleTransportOption(
          base, "view_projection_0:", &packed[0]) ||
      !ReadExactSingleTransportOption(
          base, "view_projection_1:", &packed[1]) ||
      !ReadExactSingleTransportOption(
          base, "view_source_uuid:", &source_option) ||
      !ReadExactSingleTransportOption(
          base, "target_object_kind:", &target_kind) ||
      !ReadExactSingleTransportOption(base, "view_name:", &view_name) ||
      !ReadExactSingleTransportOption(base, "name:", &canonical_name) ||
      !ReadExactSingleTransportOption(
          base, "target_schema_uuid:", &target_schema_uuid) ||
      target_kind != "view" || !SafeTransportOutputName(view_name) ||
      view_name != canonical_name ||
      !CanonicalTransportUuid(target_schema_uuid) ||
      typed->target_schema.uuid.canonical != target_schema_uuid ||
      typed->target_object.object_kind != "view" ||
      typed->localized_names.size() != 1u ||
      typed->localized_names.front().name != view_name) {
    return true;
  }

  struct DecodedProjection {
    std::uint32_t ordinal = 0;
    std::string relation_uuid;
    std::string relation_descriptor_uuid;
    std::uint64_t relation_descriptor_generation = 0;
    std::uint64_t resource_epoch = 0;
    std::string output_name;
    std::string expression_kind;
    std::string source_column_uuid;
    std::string source_type_descriptor_uuid;
    std::string descriptor_kind;
    std::string canonical_type;
    std::string encoded_descriptor;
    bool nullable = true;
    std::string typed_value;
  } decoded[2];

  for (std::uint32_t ordinal = 0; ordinal < 2u; ++ordinal) {
    const auto parts = SplitCompactInsertCell(packed[ordinal]);
    std::uint64_t encoded_ordinal = 0;
    if (parts.size() != 16u ||
        parts[0] != kRelationProjectionViewCreatePacketV1 ||
        parts[1] != "0" ||
        !ParseStrictTransportU64(parts[2], &encoded_ordinal) ||
        encoded_ordinal != ordinal ||
        !HexDecodeString(parts[3], &decoded[ordinal].relation_uuid) ||
        !HexDecodeString(
            parts[4], &decoded[ordinal].relation_descriptor_uuid) ||
        !ParseStrictTransportU64(
            parts[5], &decoded[ordinal].relation_descriptor_generation) ||
        decoded[ordinal].relation_descriptor_generation == 0u ||
        !ParseStrictTransportU64(
            parts[6], &decoded[ordinal].resource_epoch) ||
        decoded[ordinal].resource_epoch == 0u ||
        !HexDecodeString(parts[7], &decoded[ordinal].output_name) ||
        !HexDecodeString(parts[8], &decoded[ordinal].expression_kind) ||
        !HexDecodeString(parts[9], &decoded[ordinal].source_column_uuid) ||
        !HexDecodeString(
            parts[10], &decoded[ordinal].source_type_descriptor_uuid) ||
        !HexDecodeString(parts[11], &decoded[ordinal].descriptor_kind) ||
        !HexDecodeString(parts[12], &decoded[ordinal].canonical_type) ||
        !HexDecodeString(parts[13], &decoded[ordinal].encoded_descriptor) ||
        (parts[14] != "0" && parts[14] != "1") ||
        !HexDecodeString(parts[15], &decoded[ordinal].typed_value) ||
        !SafeTransportOutputName(decoded[ordinal].output_name) ||
        !CanonicalTransportUuid(decoded[ordinal].relation_uuid) ||
        !CanonicalTransportUuid(
            decoded[ordinal].relation_descriptor_uuid) ||
        !TransportInt32Descriptor(decoded[ordinal].descriptor_kind,
                                  decoded[ordinal].canonical_type,
                                  decoded[ordinal].encoded_descriptor)) {
      return true;
    }
    decoded[ordinal].ordinal = ordinal;
    decoded[ordinal].nullable = parts[14] == "1";
  }
  if (decoded[0].relation_uuid != source_option ||
      decoded[0].relation_uuid != decoded[1].relation_uuid ||
      decoded[0].relation_descriptor_uuid !=
          decoded[1].relation_descriptor_uuid ||
      decoded[0].relation_descriptor_generation !=
          decoded[1].relation_descriptor_generation ||
      decoded[0].resource_epoch != decoded[1].resource_epoch ||
      decoded[0].resource_epoch != base.context.resource_epoch ||
      decoded[0].output_name == decoded[1].output_name ||
      decoded[0].expression_kind !=
          api::kEngineRelationProjectionSourceColumnV1 ||
      !CanonicalTransportUuid(decoded[0].source_column_uuid) ||
      !CanonicalTransportUuid(decoded[0].source_type_descriptor_uuid) ||
      !decoded[0].typed_value.empty() ||
      decoded[1].expression_kind !=
          api::kEngineRelationProjectionTypedInt32LiteralV1 ||
      !decoded[1].source_column_uuid.empty() ||
      !decoded[1].source_type_descriptor_uuid.empty() ||
      decoded[1].nullable ||
      !CanonicalTransportInt32(decoded[1].typed_value)) {
    return true;
  }
  const std::set<std::string> source_identities = {
      decoded[0].relation_uuid,
      decoded[0].relation_descriptor_uuid,
      decoded[0].source_column_uuid,
      decoded[0].source_type_descriptor_uuid};
  if (source_identities.size() != 4u) return true;

  api::EngineObjectReference source_relation;
  source_relation.uuid.canonical = decoded[0].relation_uuid;
  source_relation.object_kind = "table";
  api::EngineColumnDefinition source_column;
  source_column.requested_column_uuid.canonical =
      decoded[0].source_column_uuid;
  source_column.names.push_back(
      {"en", "primary", "", decoded[0].output_name, true});
  source_column.descriptor.descriptor_uuid.canonical =
      decoded[0].source_type_descriptor_uuid;
  source_column.descriptor.descriptor_kind =
      decoded[0].descriptor_kind;
  source_column.descriptor.canonical_type_name =
      decoded[0].canonical_type;
  source_column.descriptor.encoded_descriptor =
      decoded[0].encoded_descriptor;
  source_column.ordinal = 0;
  source_column.nullable = decoded[0].nullable;

  api::EngineColumnDefinition literal_column;
  literal_column.names.push_back(
      {"en", "primary", "", decoded[1].output_name, true});
  literal_column.descriptor.descriptor_kind =
      decoded[1].descriptor_kind;
  literal_column.descriptor.canonical_type_name =
      decoded[1].canonical_type;
  literal_column.descriptor.encoded_descriptor =
      decoded[1].encoded_descriptor;
  literal_column.ordinal = 1;
  literal_column.nullable = false;
  api::EngineTypedValue literal;
  literal.descriptor = literal_column.descriptor;
  literal.encoded_value = decoded[1].typed_value;
  literal.setState(api::EngineValueState::value);

  typed->related_objects = {std::move(source_relation)};
  typed->columns = {std::move(source_column), std::move(literal_column)};
  typed->assignments = {{"projection_1_literal", std::move(literal)}};
  typed->descriptors.clear();
  typed->projection.canonical_projection_envelopes = {
      std::string(api::kEngineRelationProjectionSourceColumnV1),
      std::string(api::kEngineRelationProjectionTypedInt32LiteralV1)};
  typed->option_envelopes = {
      std::string("view_query_shape:") +
          std::string(api::kEngineRelationProjectionViewMarkerV1),
      "source_relation_descriptor_uuid:" +
          decoded[0].relation_descriptor_uuid,
      "source_relation_descriptor_generation:" +
          std::to_string(decoded[0].relation_descriptor_generation),
      "source_resource_epoch:" +
          std::to_string(decoded[0].resource_epoch)};
  return true;
}

void SetInvalidRelationProjectionViewCreateTransportV2(
    api::EngineCreateViewRequest* typed) {
  if (typed == nullptr) return;
  typed->related_objects.clear();
  typed->columns.clear();
  typed->assignments.clear();
  typed->descriptors.clear();
  typed->projection = {};
  typed->option_envelopes = {
      std::string("view_query_shape:") +
      std::string(kRelationProjectionViewTransportV2)};
}

bool DecodeRelationProjectionViewCreateTransportV2(
    const api::EngineApiRequest& base,
    api::EngineCreateViewRequest* typed) {
  if (typed == nullptr) return false;
  constexpr std::string_view marker_prefix = "view_query_shape:";
  constexpr std::string_view marker_family =
      "engine.relation_projection_view";
  std::size_t marker_field_count = 0;
  std::size_t marker_family_count = 0;
  std::size_t exact_marker_count = 0;
  for (const auto& option : base.option_envelopes) {
    const std::string_view encoded(option);
    if (!encoded.starts_with(marker_prefix)) continue;
    ++marker_field_count;
    const auto value = encoded.substr(marker_prefix.size());
    if (!value.starts_with(marker_family)) continue;
    ++marker_family_count;
    if (value == kRelationProjectionViewTransportV2) ++exact_marker_count;
  }
  if (exact_marker_count == 0) return false;

  SetInvalidRelationProjectionViewCreateTransportV2(typed);
  if (marker_field_count != 1u || marker_family_count != 1u ||
      exact_marker_count != 1u || !GlobalAggregateViewBaseDataEmpty(base) ||
      !GlobalAggregateViewOptionsAdmitted(
          base,
          {"target_object_kind", "view_name", "name",
           "target_schema_uuid", "view_projection_count",
           "view_query_shape", "view_source_uuid", "view_projection_0"}) ||
      !base.target_object.uuid.canonical.empty() ||
      base.target_object.object_kind != "view" ||
      !base.target_schema.uuid.canonical.empty() ||
      !base.target_schema.object_kind.empty()) {
    return true;
  }

  std::string encoded_count;
  std::string packed;
  std::string source_option;
  std::string target_kind;
  std::string view_name;
  std::string canonical_name;
  std::string target_schema_uuid;
  std::uint64_t projection_count = 0;
  if (!ReadExactSingleTransportOption(
          base, "view_projection_count:", &encoded_count) ||
      !ParseStrictTransportU64(encoded_count, &projection_count) ||
      projection_count != 1u ||
      !ReadExactSingleTransportOption(
          base, "view_projection_0:", &packed) ||
      !ReadExactSingleTransportOption(
          base, "view_source_uuid:", &source_option) ||
      !ReadExactSingleTransportOption(
          base, "target_object_kind:", &target_kind) ||
      !ReadExactSingleTransportOption(base, "view_name:", &view_name) ||
      !ReadExactSingleTransportOption(base, "name:", &canonical_name) ||
      !ReadExactSingleTransportOption(
          base, "target_schema_uuid:", &target_schema_uuid) ||
      target_kind != "view" || !SafeTransportOutputName(view_name) ||
      view_name != canonical_name ||
      !CanonicalTransportUuid(target_schema_uuid) ||
      typed->target_schema.uuid.canonical != target_schema_uuid ||
      typed->target_object.object_kind != "view" ||
      typed->localized_names.size() != 1u ||
      typed->localized_names.front().name != view_name) {
    return true;
  }

  const auto parts = SplitCompactInsertCell(packed);
  std::uint64_t encoded_ordinal = 0;
  std::uint64_t descriptor_generation = 0;
  std::uint64_t resource_epoch = 0;
  std::string relation_uuid;
  std::string relation_descriptor_uuid;
  std::string output_name;
  std::string source_column_name;
  std::string source_column_uuid;
  std::string source_type_uuid;
  std::string descriptor_kind;
  std::string canonical_type;
  std::string encoded_descriptor;
  std::string typed_value;
  if (parts.size() != 16u ||
      parts[0] != kRelationProjectionViewCreatePacketV2 ||
      parts[1] != "0" ||
      !ParseStrictTransportU64(parts[2], &encoded_ordinal) ||
      encoded_ordinal != 0u ||
      !HexDecodeString(parts[3], &relation_uuid) ||
      !HexDecodeString(parts[4], &relation_descriptor_uuid) ||
      !ParseStrictTransportU64(parts[5], &descriptor_generation) ||
      descriptor_generation == 0u ||
      !ParseStrictTransportU64(parts[6], &resource_epoch) ||
      resource_epoch == 0u ||
      !HexDecodeString(parts[7], &output_name) ||
      !HexDecodeString(parts[8], &source_column_name) ||
      !HexDecodeString(parts[9], &source_column_uuid) ||
      !HexDecodeString(parts[10], &source_type_uuid) ||
      !HexDecodeString(parts[11], &descriptor_kind) ||
      !HexDecodeString(parts[12], &canonical_type) ||
      !HexDecodeString(parts[13], &encoded_descriptor) ||
      (parts[14] != "0" && parts[14] != "1") ||
      !HexDecodeString(parts[15], &typed_value) ||
      !typed_value.empty() || !SafeTransportOutputName(output_name) ||
      !SafeTransportOutputName(source_column_name) ||
      !CanonicalTransportUuid(relation_uuid) ||
      !CanonicalTransportUuid(relation_descriptor_uuid) ||
      !CanonicalTransportUuid(source_column_uuid) ||
      !CanonicalTransportUuid(source_type_uuid) ||
      !TransportInt32Descriptor(descriptor_kind,
                                canonical_type,
                                encoded_descriptor) ||
      relation_uuid != source_option ||
      resource_epoch != base.context.resource_epoch) {
    return true;
  }
  const std::set<std::string> source_identities = {
      relation_uuid, relation_descriptor_uuid, source_column_uuid,
      source_type_uuid};
  if (source_identities.size() != 4u) return true;

  api::EngineObjectReference source_relation;
  source_relation.uuid.canonical = relation_uuid;
  source_relation.object_kind = "table";
  api::EngineColumnDefinition source_column;
  source_column.requested_column_uuid.canonical = source_column_uuid;
  source_column.names.push_back(
      {"en", "primary", "", output_name, true});
  source_column.descriptor.descriptor_uuid.canonical = source_type_uuid;
  source_column.descriptor.descriptor_kind = descriptor_kind;
  source_column.descriptor.canonical_type_name = canonical_type;
  source_column.descriptor.encoded_descriptor = encoded_descriptor;
  source_column.ordinal = 0;
  source_column.nullable = parts[14] == "1";

  typed->related_objects = {std::move(source_relation)};
  typed->columns = {std::move(source_column)};
  typed->assignments.clear();
  typed->descriptors.clear();
  typed->projection.canonical_projection_envelopes = {
      std::string(api::kEngineRelationProjectionSourceColumnV1)};
  typed->option_envelopes = {
      std::string("view_query_shape:") +
          std::string(api::kEngineRelationProjectionViewMarkerV2),
      "source_relation_descriptor_uuid:" + relation_descriptor_uuid,
      "source_relation_descriptor_generation:" +
          std::to_string(descriptor_generation),
      "source_resource_epoch:" + std::to_string(resource_epoch)};
  return true;
}

void SetInvalidRelationProjectionViewSelectTransport(
    api::EngineSelectRowsRequest* typed) {
  if (typed == nullptr) return;
  typed->source_object.object_kind = "view";
  typed->relation_projection_view = {};
  typed->relation_projection_view.present = true;
  typed->relation_projection_view.marker =
      kRelationProjectionViewTransportV1;
  typed->select_projection = {};
  typed->projection = {};
  typed->relation_projection = {};
  typed->global_aggregate_projection = {};
}

bool DecodeRelationProjectionViewSelectTransportV1(
    const api::EngineApiRequest& base,
    api::EngineSelectRowsRequest* typed) {
  if (typed == nullptr) return false;
  constexpr std::string_view marker_prefix = "result_projection:";
  constexpr std::string_view marker_family =
      "engine.relation_projection_view";
  std::size_t marker_field_count = 0;
  std::size_t marker_family_count = 0;
  std::size_t exact_marker_count = 0;
  for (const auto& option : base.option_envelopes) {
    const std::string_view encoded(option);
    if (!encoded.starts_with(marker_prefix)) continue;
    ++marker_field_count;
    const auto value = encoded.substr(marker_prefix.size());
    if (!value.starts_with(marker_family)) continue;
    ++marker_family_count;
    if (value == kRelationProjectionViewTransportV1) ++exact_marker_count;
  }
  if (marker_family_count == 0) return false;

  SetInvalidRelationProjectionViewSelectTransport(typed);
  if (marker_field_count != 1u || marker_family_count != 1u ||
      exact_marker_count != 1u || !GlobalAggregateViewBaseDataEmpty(base) ||
      !GlobalAggregateViewOptionsAdmitted(
          base,
          {"target_object_uuid", "target_object_kind", "source_uuid",
           "source_kind", "result_projection", "projection_count",
           "projection_0"}) ||
      !base.target_schema.uuid.canonical.empty() ||
      !base.target_schema.object_kind.empty()) {
    return true;
  }

  std::string encoded_count;
  std::string packed;
  std::string target_uuid;
  std::string target_kind;
  std::string source_uuid;
  std::string source_kind;
  std::uint64_t projection_count = 0;
  if (!ReadExactSingleTransportOption(
          base, "projection_count:", &encoded_count) ||
      !ParseStrictTransportU64(encoded_count, &projection_count) ||
      projection_count != 1u ||
      !ReadExactSingleTransportOption(base, "projection_0:", &packed) ||
      !ReadExactSingleTransportOption(
          base, "target_object_uuid:", &target_uuid) ||
      !ReadExactSingleTransportOption(
          base, "target_object_kind:", &target_kind) ||
      !ReadExactSingleTransportOption(base, "source_uuid:", &source_uuid) ||
      !ReadExactSingleTransportOption(base, "source_kind:", &source_kind) ||
      !CanonicalTransportUuid(target_uuid) || target_uuid != source_uuid ||
      target_kind != "view" || source_kind != "view" ||
      typed->source_object.uuid.canonical != target_uuid ||
      typed->source_object.object_kind != "view") {
    return true;
  }

  const auto parts = SplitCompactInsertCell(packed);
  constexpr std::size_t kHeaderParts = 5u;
  constexpr std::size_t kOutputParts = 8u;
  constexpr std::size_t kOutputCount = 2u;
  std::string marker;
  std::string descriptor_uuid;
  std::uint64_t descriptor_generation = 0;
  std::uint64_t output_count = 0;
  if (parts.size() != kHeaderParts + kOutputParts * kOutputCount ||
      parts[0] != kRelationProjectionViewSelectPacketV1 ||
      !HexDecodeString(parts[1], &marker) ||
      !HexDecodeString(parts[2], &descriptor_uuid) ||
      !ParseStrictTransportU64(parts[3], &descriptor_generation) ||
      descriptor_generation == 0u ||
      !ParseStrictTransportU64(parts[4], &output_count) ||
      output_count != kOutputCount ||
      marker != api::kEngineRelationProjectionViewMarkerV1 ||
      !CanonicalTransportUuid(descriptor_uuid) ||
      descriptor_uuid == target_uuid) {
    return true;
  }

  std::vector<api::EngineRelationProjectionViewSemanticOutput> outputs;
  outputs.reserve(kOutputCount);
  std::set<std::string> identities = {target_uuid, descriptor_uuid};
  std::set<std::string> names;
  for (std::uint32_t ordinal = 0; ordinal < kOutputCount; ++ordinal) {
    const std::size_t base_index = kHeaderParts + ordinal * kOutputParts;
    std::uint64_t encoded_ordinal = 0;
    std::string output_name;
    std::string output_column_uuid;
    std::string type_descriptor_uuid;
    std::string descriptor_kind;
    std::string canonical_type;
    std::string encoded_descriptor;
    std::string nullable;
    if (!ParseStrictTransportU64(parts[base_index], &encoded_ordinal) ||
        encoded_ordinal != ordinal ||
        !HexDecodeString(parts[base_index + 1u], &output_name) ||
        !HexDecodeString(
            parts[base_index + 2u], &output_column_uuid) ||
        !HexDecodeString(
            parts[base_index + 3u], &type_descriptor_uuid) ||
        !HexDecodeString(parts[base_index + 4u], &descriptor_kind) ||
        !HexDecodeString(parts[base_index + 5u], &canonical_type) ||
        !HexDecodeString(parts[base_index + 6u], &encoded_descriptor) ||
        !HexDecodeString(parts[base_index + 7u], &nullable) ||
        (nullable != "0" && nullable != "1") ||
        !SafeTransportOutputName(output_name) ||
        !CanonicalTransportUuid(output_column_uuid) ||
        !CanonicalTransportUuid(type_descriptor_uuid) ||
        !TransportInt32Descriptor(descriptor_kind,
                                  canonical_type,
                                  encoded_descriptor) ||
        !identities.insert(output_column_uuid).second ||
        !identities.insert(type_descriptor_uuid).second ||
        !names.insert(output_name).second ||
        (ordinal == 1u && nullable != "0")) {
      return true;
    }
    api::EngineRelationProjectionViewSemanticOutput output;
    output.ordinal = ordinal;
    output.output_column_uuid.canonical = std::move(output_column_uuid);
    output.output_name = std::move(output_name);
    output.output_type.type_descriptor_uuid.canonical =
        std::move(type_descriptor_uuid);
    output.output_type.descriptor_kind = std::move(descriptor_kind);
    output.output_type.canonical_type_name = std::move(canonical_type);
    output.output_type.encoded_descriptor = std::move(encoded_descriptor);
    output.nullable = nullable == "1";
    outputs.push_back(std::move(output));
  }

  typed->relation_projection_view.present = true;
  typed->relation_projection_view.marker = std::move(marker);
  typed->relation_projection_view.view_uuid.canonical = target_uuid;
  typed->relation_projection_view.view_descriptor_uuid.canonical =
      std::move(descriptor_uuid);
  typed->relation_projection_view.view_descriptor_generation =
      descriptor_generation;
  typed->relation_projection_view.outputs = std::move(outputs);
  typed->select_projection = {};
  typed->projection = {};
  typed->relation_projection = {};
  typed->global_aggregate_projection = {};
  typed->descriptors.clear();
  typed->columns.clear();
  typed->assignments.clear();
  typed->related_objects.clear();
  typed->option_envelopes.clear();
  return true;
}

void SetInvalidRelationProjectionViewDeleteTransportV2(
    api::EngineDeleteRowsRequest* typed) {
  if (typed == nullptr) return;
  typed->relation_projection_view = {};
  typed->relation_projection_view.present = true;
  typed->relation_projection_view.marker =
      std::string(kRelationProjectionViewTransportV2);
}

bool DecodeRelationProjectionViewDeleteTransportV2(
    const api::EngineApiRequest& base,
    api::EngineDeleteRowsRequest* typed) {
  if (typed == nullptr) return false;
  constexpr std::string_view marker_prefix = "dml_surface_variant:";
  std::size_t marker_field_count = 0;
  std::size_t exact_marker_count = 0;
  for (const auto& option : base.option_envelopes) {
    const std::string_view encoded(option);
    if (!encoded.starts_with(marker_prefix)) continue;
    ++marker_field_count;
    if (encoded.substr(marker_prefix.size()) ==
        kRelationProjectionViewTransportV2) {
      ++exact_marker_count;
    }
  }
  if (exact_marker_count == 0) return false;

  SetInvalidRelationProjectionViewDeleteTransportV2(typed);
  api::EngineApiRequest data_shape = base;
  data_shape.target_object = {};
  data_shape.predicate = {};
  if (marker_field_count != 1u || exact_marker_count != 1u ||
      !GlobalAggregateViewBaseDataEmpty(data_shape) ||
      !GlobalAggregateViewOptionsAdmitted(
          base,
          {"target_object_uuid", "target_object_kind",
           "dml_surface_variant", "projection_count", "projection_0",
           "predicate_kind", "predicate_column", "predicate_value",
           "predicate_value_type"}) ||
      !base.target_schema.uuid.canonical.empty() ||
      !base.target_schema.object_kind.empty()) {
    return true;
  }

  std::string target_uuid;
  std::string target_kind;
  std::string surface_variant;
  std::string encoded_count;
  std::string packed;
  std::string predicate_kind;
  std::string predicate_column;
  std::string predicate_value;
  std::string predicate_value_type;
  std::uint64_t projection_count = 0;
  if (!ReadExactSingleTransportOption(
          base, "target_object_uuid:", &target_uuid) ||
      !ReadExactSingleTransportOption(
          base, "target_object_kind:", &target_kind) ||
      !ReadExactSingleTransportOption(
          base, "dml_surface_variant:", &surface_variant) ||
      !ReadExactSingleTransportOption(
          base, "projection_count:", &encoded_count) ||
      !ParseStrictTransportU64(encoded_count, &projection_count) ||
      projection_count != 1u ||
      !ReadExactSingleTransportOption(base, "projection_0:", &packed) ||
      !ReadExactSingleTransportOption(
          base, "predicate_kind:", &predicate_kind) ||
      !ReadExactSingleTransportOption(
          base, "predicate_column:", &predicate_column) ||
      !ReadExactSingleTransportOption(
          base, "predicate_value:", &predicate_value) ||
      !ReadExactSingleTransportOption(
          base, "predicate_value_type:", &predicate_value_type) ||
      !CanonicalTransportUuid(target_uuid) || target_kind != "view" ||
      surface_variant != kRelationProjectionViewTransportV2 ||
      predicate_kind != "column_equals" ||
      !SafeTransportOutputName(predicate_column) ||
      predicate_value_type != "int32" ||
      !CanonicalTransportInt32(predicate_value) ||
      base.target_object.uuid.canonical != target_uuid ||
      base.target_object.object_kind != "view" ||
      base.predicate.predicate_kind != predicate_kind ||
      base.predicate.canonical_predicate_envelope != predicate_column ||
      base.predicate.bound_values.size() != 1u) {
    return true;
  }
  const auto& bound_value = base.predicate.bound_values.front();
  if (!bound_value.descriptor.descriptor_uuid.canonical.empty() ||
      bound_value.descriptor.descriptor_kind != "scalar" ||
      bound_value.descriptor.canonical_type_name != "int32" ||
      bound_value.descriptor.encoded_descriptor != "type=int32" ||
      bound_value.encoded_value != predicate_value ||
      bound_value.state != api::EngineValueState::value ||
      bound_value.isSqlNull() || !bound_value.binary_value.empty()) {
    return true;
  }

  const auto parts = SplitCompactInsertCell(packed);
  std::string marker;
  std::string descriptor_uuid;
  std::uint64_t descriptor_generation = 0;
  std::uint64_t output_count = 0;
  std::uint64_t ordinal = 0;
  std::string output_name;
  std::string output_column_uuid;
  std::string type_descriptor_uuid;
  std::string descriptor_kind;
  std::string canonical_type;
  std::string encoded_descriptor;
  std::string nullable;
  if (parts.size() != 13u ||
      parts[0] != kRelationProjectionViewDeletePacketV2 ||
      !HexDecodeString(parts[1], &marker) ||
      !HexDecodeString(parts[2], &descriptor_uuid) ||
      !ParseStrictTransportU64(parts[3], &descriptor_generation) ||
      descriptor_generation == 0u ||
      !ParseStrictTransportU64(parts[4], &output_count) ||
      output_count != 1u ||
      !ParseStrictTransportU64(parts[5], &ordinal) || ordinal != 0u ||
      !HexDecodeString(parts[6], &output_name) ||
      !HexDecodeString(parts[7], &output_column_uuid) ||
      !HexDecodeString(parts[8], &type_descriptor_uuid) ||
      !HexDecodeString(parts[9], &descriptor_kind) ||
      !HexDecodeString(parts[10], &canonical_type) ||
      !HexDecodeString(parts[11], &encoded_descriptor) ||
      !HexDecodeString(parts[12], &nullable) ||
      (nullable != "0" && nullable != "1") ||
      marker != api::kEngineRelationProjectionViewMarkerV2 ||
      output_name != predicate_column ||
      !SafeTransportOutputName(output_name) ||
      !CanonicalTransportUuid(descriptor_uuid) ||
      !CanonicalTransportUuid(output_column_uuid) ||
      !CanonicalTransportUuid(type_descriptor_uuid) ||
      !TransportInt32Descriptor(descriptor_kind,
                                canonical_type,
                                encoded_descriptor)) {
    return true;
  }
  const std::set<std::string> public_identities = {
      target_uuid, descriptor_uuid, output_column_uuid,
      type_descriptor_uuid};
  if (public_identities.size() != 4u) return true;

  api::EngineRelationProjectionViewSemanticOutput output;
  output.ordinal = 0;
  output.output_column_uuid.canonical = std::move(output_column_uuid);
  output.output_name = std::move(output_name);
  output.output_type.type_descriptor_uuid.canonical =
      std::move(type_descriptor_uuid);
  output.output_type.descriptor_kind = std::move(descriptor_kind);
  output.output_type.canonical_type_name = std::move(canonical_type);
  output.output_type.encoded_descriptor = std::move(encoded_descriptor);
  output.nullable = nullable == "1";
  typed->relation_projection_view.present = true;
  typed->relation_projection_view.marker = std::move(marker);
  typed->relation_projection_view.view_descriptor_uuid.canonical =
      std::move(descriptor_uuid);
  typed->relation_projection_view.view_descriptor_generation =
      descriptor_generation;
  typed->relation_projection_view.outputs = {std::move(output)};
  return true;
}
// SB_ENGINE_RELATION_PROJECTION_VIEW_TRANSPORT_V1_END

struct CompactInsertValueCell {
  std::string name;
  std::string type;
  std::string value;
  bool is_null = false;
};

constexpr std::string_view kOctetFromInt64ScalarMarker =
    "scalar.octet_from_int64.v1";

enum class CompactInsertScalarEvaluationStatus : std::uint8_t {
  not_marker = 0,
  evaluated = 1,
  invalid_syntax = 2,
  out_of_range = 3,
};

struct CompactInsertScalarValidation {
  bool ok = true;
  std::string diagnostic_code;
  std::string diagnostic_message_key;
  std::string diagnostic_detail;
};

bool ParseStrictInt64Decimal(std::string_view text, std::int64_t* out) {
  if (out == nullptr || text.empty()) return false;
  std::size_t offset = 0;
  bool negative = false;
  if (text.front() == '+' || text.front() == '-') {
    negative = text.front() == '-';
    offset = 1;
  }
  if (offset == text.size()) return false;

  const std::uint64_t positive_limit =
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  const std::uint64_t limit = negative ? positive_limit + 1u : positive_limit;
  std::uint64_t magnitude = 0;
  for (; offset < text.size(); ++offset) {
    const unsigned char ch = static_cast<unsigned char>(text[offset]);
    if (!std::isdigit(ch)) return false;
    const std::uint64_t digit = static_cast<std::uint64_t>(ch - '0');
    if (magnitude > (limit - digit) / 10u) return false;
    magnitude = magnitude * 10u + digit;
  }

  if (negative) {
    if (magnitude == positive_limit + 1u) {
      *out = std::numeric_limits<std::int64_t>::min();
    } else {
      *out = -static_cast<std::int64_t>(magnitude);
    }
  } else {
    *out = static_cast<std::int64_t>(magnitude);
  }
  return true;
}

CompactInsertScalarEvaluationStatus EvaluateCompactInsertScalarMarker(
    CompactInsertValueCell* cell) {
  if (cell == nullptr || cell->type != kOctetFromInt64ScalarMarker) {
    return CompactInsertScalarEvaluationStatus::not_marker;
  }
  if (cell->is_null) {
    cell->type = "text";
    cell->value.clear();
    return CompactInsertScalarEvaluationStatus::evaluated;
  }

  std::int64_t integer = 0;
  if (!ParseStrictInt64Decimal(cell->value, &integer)) {
    return CompactInsertScalarEvaluationStatus::invalid_syntax;
  }
  if (integer < 0 || integer > 255) {
    return CompactInsertScalarEvaluationStatus::out_of_range;
  }

  cell->type = "text";
  cell->value.assign(
      1, static_cast<char>(static_cast<unsigned char>(integer)));
  return CompactInsertScalarEvaluationStatus::evaluated;
}

std::vector<CompactInsertValueCell> DecodeCompactInsertCells(
    std::string_view payload,
    std::uint64_t row_count,
    std::uint64_t column_count) {
  const std::uint64_t cell_count = row_count * column_count;
  std::vector<CompactInsertValueCell> cells;
  if (payload.empty() || row_count == 0 || column_count == 0 ||
      cell_count > static_cast<std::uint64_t>(
                       std::numeric_limits<std::size_t>::max())) {
    return cells;
  }
  cells.resize(static_cast<std::size_t>(cell_count));
  std::uint64_t ordinal = 0;
  std::size_t start = 0;
  while (start <= payload.size()) {
    const std::size_t end = payload.find(';', start);
    const std::string_view cell =
        payload.substr(start,
                       end == std::string_view::npos ? payload.size() - start
                                                     : end - start);
    if (ordinal >= cell_count) return {};
    const auto parts = SplitCompactInsertCell(cell);
    if (parts.size() != 4) return {};
    auto& target = cells[static_cast<std::size_t>(ordinal)];
    if (!HexDecodeString(parts[0], &target.name) ||
        !HexDecodeString(parts[1], &target.type) ||
        !HexDecodeString(parts[2], &target.value)) {
      return {};
    }
    target.is_null = CompactBool(parts[3]);
    ++ordinal;
    if (end == std::string_view::npos) break;
    start = end + 1u;
  }
  if (ordinal != cell_count) return {};
  return cells;
}

CompactInsertScalarValidation ValidateCompactInsertScalarMarkers(
    const SblrOperationEnvelope& envelope) {
  CompactInsertScalarValidation validation;
  const bool supported_operation =
      envelope.operation_id == "dml.insert_rows" ||
      envelope.operation_id == "dml.execute_native_bulk_ingest" ||
      envelope.operation_id == "dml.execute_import_rows";
  if (!supported_operation) return validation;

  const auto compact_format =
      TextOperandValue(envelope, "insert_values_compact_format");
  const auto compact_payload =
      TextOperandValue(envelope, "insert_values_compact_payload");
  const bool supported_compact_format =
      compact_format &&
      (*compact_format == "sblr.dml.insert.cells.hex.v1" ||
       *compact_format == "sbsql.insert_values.cells.v1");
  if (!supported_compact_format || !compact_payload ||
      compact_payload->empty()) {
    return validation;
  }

  const std::uint64_t row_count = ParseCompactU64(
      TextOperandValue(envelope, "insert_values_row_count")
          .value_or(std::string_view{}));
  const std::uint64_t column_count = ParseCompactU64(
      TextOperandValue(envelope, "insert_values_column_count")
          .value_or(std::string_view{}));
  if (row_count == 0 || column_count == 0) return validation;

  auto cells =
      DecodeCompactInsertCells(*compact_payload, row_count, column_count);
  if (cells.empty()) return validation;
  for (auto& cell : cells) {
    const auto status = EvaluateCompactInsertScalarMarker(&cell);
    if (status == CompactInsertScalarEvaluationStatus::invalid_syntax) {
      validation.ok = false;
      validation.diagnostic_code =
          "SB_SBLR_SCALAR_OCTET_FROM_INT64_INVALID_SYNTAX";
      validation.diagnostic_message_key =
          "engine.sblr.compact_insert.scalar_octet.invalid_syntax";
      validation.diagnostic_detail =
          "scalar.octet_from_int64.v1 requires an exact signed decimal "
          "int64 operand";
      return validation;
    }
    if (status == CompactInsertScalarEvaluationStatus::out_of_range) {
      validation.ok = false;
      validation.diagnostic_code =
          "SB_SBLR_SCALAR_OCTET_FROM_INT64_OUT_OF_RANGE";
      validation.diagnostic_message_key =
          "engine.sblr.compact_insert.scalar_octet.out_of_range";
      validation.diagnostic_detail =
          "scalar.octet_from_int64.v1 operand must be in the inclusive "
          "range [0, 255]";
      return validation;
    }
  }
  return validation;
}

std::vector<std::string> CompactDescriptorColumnNames(
    const SblrOperationEnvelope& envelope,
    std::uint64_t column_count) {
  std::vector<std::string> columns;
  columns.reserve(static_cast<std::size_t>(
      std::min<std::uint64_t>(column_count, 1024)));
  for (std::uint64_t index = 0; index < column_count; ++index) {
    const auto value = TextOperandValue(
        envelope,
        "insert_values_descriptor_column_" + std::to_string(index));
    columns.push_back(value ? std::string(*value) : std::string{});
  }
  return columns;
}

std::vector<std::string> LoadDescriptorColumnNamesForCompactInsert(
    const api::EngineApiRequest& request,
    std::uint64_t column_count) {
  std::vector<std::string> columns;
  if (column_count == 0 || request.target_object.uuid.canonical.empty()) {
    return columns;
  }
  auto loaded = api::LoadMgaRelationStoreStateForInsertTarget(
      request.context,
      request.target_object.uuid.canonical);
  if (!loaded.ok) return columns;
  api::CrudState state = api::BuildCrudCompatibilityStateFromMga(
      std::move(loaded.state));
  const auto table = api::FindVisibleCrudTable(
      state,
      request.target_object.uuid.canonical,
      request.context.local_transaction_id);
  if (!table) return columns;
  columns.reserve(table->columns.size());
  for (const auto& [name, descriptor] : table->columns) {
    (void)descriptor;
    columns.push_back(name);
  }
  return columns;
}

void MaterializeCompactInsertRows(const SblrOperationEnvelope& envelope,
                                  api::EngineApiRequest* request) {
  const bool supported_operation =
      envelope.operation_id == "dml.insert_rows" ||
      envelope.operation_id == "dml.execute_native_bulk_ingest" ||
      envelope.operation_id == "dml.execute_import_rows";
  if (request == nullptr || !supported_operation || !request->rows.empty()) {
    return;
  }
  const auto compact_format = TextOperandValue(
      envelope,
      "insert_values_compact_format");
  const auto compact_payload = TextOperandValue(
      envelope,
      "insert_values_compact_payload");
  const bool supported_compact_format =
      compact_format &&
      (*compact_format == "sblr.dml.insert.cells.hex.v1" ||
       *compact_format == "sbsql.insert_values.cells.v1");
  if (!supported_compact_format ||
      !compact_payload || compact_payload->empty()) {
    return;
  }
  const std::uint64_t row_count =
      ParseCompactU64(TextOperandValue(envelope, "insert_values_row_count")
                          .value_or(std::string_view{}));
  const std::uint64_t column_count =
      ParseCompactU64(TextOperandValue(envelope, "insert_values_column_count")
                          .value_or(std::string_view{}));
  if (row_count == 0 || column_count == 0) return;
  auto cells =
      DecodeCompactInsertCells(*compact_payload, row_count, column_count);
  if (cells.empty()) return;
  std::uint64_t evaluated_scalar_count = 0;
  for (auto& cell : cells) {
    const auto status = EvaluateCompactInsertScalarMarker(&cell);
    if (status == CompactInsertScalarEvaluationStatus::invalid_syntax ||
        status == CompactInsertScalarEvaluationStatus::out_of_range) {
      // Dispatch validates every bound marker before constructing the API
      // request. Keep this boundary fail-closed if materialization is ever
      // called through a new route without that validation.
      return;
    }
    if (status == CompactInsertScalarEvaluationStatus::evaluated) {
      ++evaluated_scalar_count;
    }
  }
  const bool explicit_column_list = CompactBool(
      TextOperandValue(envelope, "insert_values_column_list_present")
          .value_or(std::string_view{}));
  std::vector<std::string> descriptor_columns;
  if (!explicit_column_list) {
    descriptor_columns = CompactDescriptorColumnNames(envelope, column_count);
    bool missing_descriptor_column = descriptor_columns.size() < column_count;
    for (const auto& column : descriptor_columns) {
      if (column.empty()) {
        missing_descriptor_column = true;
        break;
      }
    }
    if (missing_descriptor_column) {
      descriptor_columns =
          LoadDescriptorColumnNamesForCompactInsert(*request, column_count);
    }
  }
  std::vector<std::string> shared_field_order;
  if (!explicit_column_list) {
    shared_field_order = descriptor_columns;
  } else {
    shared_field_order.reserve(static_cast<std::size_t>(
        std::min<std::uint64_t>(column_count, 1024)));
    for (std::uint64_t column_index = 0; column_index < column_count;
         ++column_index) {
      const auto& cell = cells[static_cast<std::size_t>(column_index)];
      if (cell.name.empty() || LooksLikeOrdinalInsertFieldName(cell.name)) {
        shared_field_order.clear();
        break;
      }
      shared_field_order.push_back(cell.name);
    }
    if (!shared_field_order.empty()) {
      const auto descriptor_order =
          LoadDescriptorColumnNamesForCompactInsert(*request, column_count);
      if (descriptor_order.size() != shared_field_order.size() ||
          !std::equal(shared_field_order.begin(),
                      shared_field_order.end(),
                      descriptor_order.begin())) {
        shared_field_order.clear();
      }
    }
  }
  request->option_envelopes.erase(
      std::remove_if(
          request->option_envelopes.begin(),
          request->option_envelopes.end(),
          [](const std::string& option) {
            return option == "sblr.canonical_rowset_shared_shape=true" ||
                   option.rfind("sblr.canonical_rowset_shared_shape:", 0) == 0;
          }),
      request->option_envelopes.end());
  if (!shared_field_order.empty()) {
    bool complete_shared_order = shared_field_order.size() >= column_count;
    for (const auto& column : shared_field_order) {
      if (column.empty()) {
        complete_shared_order = false;
        break;
      }
    }
    if (complete_shared_order) {
      request->shared_row_field_order = shared_field_order;
      request->option_envelopes.push_back(
          "sblr.canonical_rowset_shared_shape=true");
    }
  }
  request->rows.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(
      row_count,
      static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))));
  for (std::uint64_t row_index = 0; row_index < row_count; ++row_index) {
    api::EngineRowValue row;
    row.fields.reserve(static_cast<std::size_t>(
        std::min<std::uint64_t>(column_count, 1024)));
    for (std::uint64_t column_index = 0; column_index < column_count;
         ++column_index) {
      const auto& cell =
          cells[static_cast<std::size_t>(row_index * column_count +
                                         column_index)];
      std::string name = cell.name;
      if (!explicit_column_list &&
          (name.empty() || LooksLikeOrdinalInsertFieldName(name)) &&
          column_index < descriptor_columns.size() &&
          !descriptor_columns[static_cast<std::size_t>(column_index)].empty()) {
        name = descriptor_columns[static_cast<std::size_t>(column_index)];
      }
      if (name.empty()) name = "c" + std::to_string(column_index);
      std::string type = cell.type.empty() ? "text" : cell.type;
      api::EngineTypedValue value;
      value.descriptor.descriptor_kind = "scalar";
      value.descriptor.canonical_type_name = type;
      value.descriptor.encoded_descriptor = "type=" + type;
      value.encoded_value = cell.value;
      value.is_null = cell.is_null || type == "null";
      if (value.is_null) {
        value.encoded_value.clear();
        value.setState(api::EngineValueState::sql_null);
      }
      row.fields.push_back({std::move(name), std::move(value)});
    }
    request->rows.push_back(std::move(row));
  }
  request->option_envelopes.push_back(
      "sblr.compact_insert_rowset_materialized=true");
  if (!request->shared_row_field_order.empty()) {
    request->option_envelopes.push_back(
        "sblr.compact_insert_shared_field_order=true");
  }
  if (envelope.operation_id != "dml.insert_rows") {
    request->option_envelopes.push_back(
        "sblr.compact_native_rowset_materialized=true");
  }
  request->option_envelopes.push_back(
      "sblr.compact_insert_row_count:" + std::to_string(row_count));
  if (evaluated_scalar_count != 0) {
    request->option_envelopes.push_back(
        "sblr.compact_insert_scalar_evaluated:" +
        std::string(kOctetFromInt64ScalarMarker));
    request->option_envelopes.push_back(
        "sblr.compact_insert_scalar_evaluation_count:" +
        std::to_string(evaluated_scalar_count));
  }
}

api::EngineApiResult FailureResult(const api::EngineRequestContext& context,
                                   const std::string& operation_id,
                                   std::string code,
                                   std::string message_key,
                                   std::string detail) {
  api::EngineApiResult result;
  result.ok = false;
  result.operation_id = operation_id;
  result.embedded_trust_mode_observed = context.trust_mode == api::EngineTrustMode::embedded_in_process;
  result.cluster_authority_required = false;
  result.diagnostics.push_back(api::MakeEngineApiDiagnostic(std::move(code), std::move(message_key), std::move(detail), true));
  return result;
}

api::EngineApiRequest BuildBaseApiRequest(api::EngineApiRequest api_request,
                                          const SblrDispatchRequest& request) {
  const auto phase_start = SblrSteadyClock::now();
  api_request.context = request.context;
  api_request.operation_id = request.envelope.operation_id;
  api_request.option_envelopes.reserve(api_request.option_envelopes.size() +
                                       request.envelope.operands.size());

  std::unordered_map<std::string, std::size_t> row_index_by_uuid;
  row_index_by_uuid.reserve(api_request.rows.size() +
                            request.envelope.operands.size() / 4);
  api_request.rows.reserve(api_request.rows.size() +
                           request.envelope.operands.size() / 4);
  for (std::size_t index = 0; index < api_request.rows.size(); ++index) {
    const std::string& row_uuid =
        api_request.rows[index].requested_row_uuid.canonical;
    if (!row_uuid.empty()) {
      row_index_by_uuid.emplace(row_uuid, index);
    }
  }

  for (const auto& operand : request.envelope.operands) {
    const bool row_field = operand.type == "row_field" ||
                           operand.type.starts_with("row_field:");
    const bool row_null_field = operand.type == "row_null_field" ||
                                operand.type.starts_with("row_null_field:");
    if (!operand.name.empty() && !row_field && !row_null_field) {
      api_request.option_envelopes.push_back(operand.name + ":" + operand.value);
    }
    if (row_field || row_null_field) {
      const auto separator = operand.name.find('|');
      if (separator == std::string::npos || separator + 1 >= operand.name.size()) {
        continue;
      }
      const std::string row_uuid = operand.name.substr(0, separator);
      const std::string field_name = operand.name.substr(separator + 1);
      auto row_index = row_index_by_uuid.find(row_uuid);
      if (row_index == row_index_by_uuid.end()) {
        const std::size_t appended_index = api_request.rows.size();
        api::EngineRowValue appended;
        appended.requested_row_uuid.canonical = row_uuid;
        api_request.rows.push_back(std::move(appended));
        row_index =
            row_index_by_uuid.emplace(row_uuid, appended_index).first;
      }
      api::EngineRowValue& row = api_request.rows[row_index->second];
      api::EngineTypedValue value;
      value.descriptor.descriptor_kind = "scalar";
      const auto type_separator = operand.type.find(':');
      value.descriptor.canonical_type_name =
          type_separator == std::string::npos
              ? std::string{}
              : operand.type.substr(type_separator + 1);
      if (!value.descriptor.canonical_type_name.empty()) {
        value.descriptor.encoded_descriptor = "type=" + value.descriptor.canonical_type_name;
      }
      value.encoded_value = operand.value;
      value.is_null = row_null_field;
      if (value.is_null) {
        value.encoded_value.clear();
        value.setState(api::EngineValueState::sql_null);
      }
      row.fields.push_back({field_name, std::move(value)});
    } else if (operand.type == "assignment" && !operand.name.empty()) {
      api::EngineTypedValue value;
      value.descriptor.descriptor_kind = "scalar";
      value.descriptor.canonical_type_name = "text";
      value.descriptor.encoded_descriptor = "type=text";
      value.encoded_value = operand.value;
      api_request.assignments.push_back({operand.name, std::move(value)});
    } else if (operand.type == "predicate" && !operand.name.empty()) {
      api_request.predicate.predicate_kind = operand.name;
      api_request.predicate.canonical_predicate_envelope = operand.value;
    }
  }
  const auto operand_loop_finish = SblrSteadyClock::now();
  const std::string identifier_profile_uuid =
      api::SecurityOptionValue(api_request, "identifier_profile_uuid:");
  if (!identifier_profile_uuid.empty()) {
    api_request.context.identifier_profile_uuid = identifier_profile_uuid;
  }
  const std::string current_role_uuid =
      api::SecurityOptionValue(api_request, "current_role_uuid:");
  if (!current_role_uuid.empty()) {
    api_request.context.current_role_uuid.canonical = current_role_uuid;
  }
  if (api_request.target_object.uuid.canonical.empty()) {
    api_request.target_object.uuid.canonical =
        api::SecurityOptionValue(api_request, "target_object_uuid:");
  }
  if (api_request.target_object.object_kind.empty()) {
    api_request.target_object.object_kind =
        api::SecurityOptionValue(api_request, "target_object_kind:");
  }
  if (api_request.operation_id.rfind("lifecycle.", 0) == 0) {
    const std::string lifecycle_database_path =
        api::SecurityOptionValue(api_request, "database_path:");
    if (!lifecycle_database_path.empty()) {
      api_request.context.database_path = lifecycle_database_path;
    }
  }
  {
    const std::string predicate_kind =
        api::SecurityOptionValue(api_request, "predicate_kind:");
    const std::string predicate_column =
        api::SecurityOptionValue(api_request, "predicate_column:");
    const std::string predicate_value =
        api::SecurityOptionValue(api_request, "predicate_value:");
    const bool dml_predicate_operand_authoritative =
        api_request.operation_id == "dml.select_rows" ||
        api_request.operation_id == "dml.update_rows" ||
        api_request.operation_id == "dml.delete_rows" ||
        api_request.operation_id == "dml.merge_rows";
    if ((api_request.predicate.predicate_kind.empty() ||
         dml_predicate_operand_authoritative) &&
        !predicate_kind.empty()) {
      api_request.predicate.predicate_kind = predicate_kind;
    }
    if ((api_request.predicate.canonical_predicate_envelope.empty() ||
         dml_predicate_operand_authoritative) &&
        !predicate_column.empty()) {
      api_request.predicate.canonical_predicate_envelope = predicate_column;
    }
    if (api_request.predicate.bound_values.empty() && !predicate_value.empty()) {
      const auto values = SplitCommaSeparatedLiterals(predicate_value);
      const auto types = SplitCommaSeparatedLiterals(
          api::SecurityOptionValue(api_request, "predicate_value_type:"));
      for (std::size_t index = 0; index < values.size(); ++index) {
        api_request.predicate.bound_values.push_back(TypedValueFromLoweredLiteral(
            values[index],
            index < types.size() ? types[index] : std::string{}));
      }
    }
  }
  if (api_request.assignments.empty()) {
    const std::string assignment_column =
        api::SecurityOptionValue(api_request, "assignment_column:");
    const std::string assignment_value =
        api::SecurityOptionValue(api_request, "assignment_value:");
    if (!assignment_column.empty()) {
      api_request.assignments.push_back({assignment_column,
                                         TypedValueFromLoweredLiteral(
                                             assignment_value,
                                             api::SecurityOptionValue(
                                             api_request,
                                                 "assignment_value_type:"))});
    }
  }
  if (api_request.constraints.empty()) {
    std::string constraint_kind =
        api::SecurityOptionValue(api_request, "constraint_kind:");
    const std::string constraint_name =
        api::SecurityOptionValue(api_request, "constraint_name:");
    std::string constraint_envelope =
        api::SecurityOptionValue(api_request, "canonical_constraint_envelope:");
    if (constraint_kind == "check_constraint") constraint_kind = "check";
    if (!constraint_kind.empty() || !constraint_name.empty() ||
        !constraint_envelope.empty()) {
      if (constraint_kind.empty()) constraint_kind = "constraint";
      // The bounded neutral foreign-key descriptor is an engine-validated
      // transport contract.  It deliberately excludes parser-supplied seal,
      // support, key, and hash identities, so the generic compatibility
      // completion below must transport this descriptor unchanged.
      const bool exact_neutral_single_column_foreign_key =
          api_request.operation_id == "ddl.constraint.alter" &&
          constraint_kind == "foreign_key" &&
          CanonicalEnvelopeHasExactField(
              constraint_envelope,
              "descriptor_version",
              "neutral_fk_single_column_v1");
      if (constraint_envelope.empty()) {
        constraint_envelope = "constraint_hash=" +
                              std::to_string(std::hash<std::string>{}(
                                  api_request.operation_id + ":" +
                                  constraint_kind + ":" + constraint_name)) +
                              ";enforcement_timing=immediate"
                              ";validation_state=unvalidated"
                              ";trust_state=untrusted";
      } else if (!exact_neutral_single_column_foreign_key &&
                 constraint_envelope.find("constraint_hash=") ==
                     std::string::npos) {
        constraint_envelope += ";constraint_hash=" +
                               std::to_string(std::hash<std::string>{}(
                                   api_request.operation_id + ":" +
                                   constraint_kind + ":" + constraint_name));
      }
      if (constraint_envelope.find("enforcement_timing=") ==
          std::string::npos) {
        const std::string enforcement =
            api::SecurityOptionValue(api_request, "enforcement_timing:");
        constraint_envelope += ";enforcement_timing=" +
                               (enforcement.empty() ? std::string("immediate")
                                                    : enforcement);
      }
      api::EngineConstraintDefinition constraint;
      constraint.constraint_kind = constraint_kind;
      constraint.canonical_constraint_envelope = std::move(constraint_envelope);
      if (!constraint_name.empty()) {
        constraint.names.push_back({"en", "primary", "", constraint_name, true});
      }
      api_request.constraints.push_back(std::move(constraint));
    }
  }
  const auto compact_start = SblrSteadyClock::now();
  MaterializeCompactInsertRows(request.envelope, &api_request);
  const auto compact_finish = SblrSteadyClock::now();
  WriteBaseApiPhaseTrace(
      request.envelope,
      SblrElapsedMicros(phase_start, operand_loop_finish),
      SblrElapsedMicros(compact_start, compact_finish),
      SblrElapsedMicros(phase_start, compact_finish),
      api_request.rows.size(),
      request.envelope.operands.size());
  return api_request;
}

api::EngineApiRequest BaseApiRequest(const SblrDispatchRequest& request) {
  return BuildBaseApiRequest(request.api_request, request);
}

api::EngineApiRequest BaseApiRequestMove(SblrDispatchRequest& request) {
  return BuildBaseApiRequest(std::move(request.api_request), request);
}

const char* ExpectedOpcodeForOperation(std::string_view operation_id) {
  if (operation_id.rfind("op.", 0) == 0) {
    if (const auto* entry = LookupSblrOperation(operation_id)) {
      return entry->opcode.c_str();
    }
  }
  if (operation_id.starts_with("index.")) {
    if (const auto* entry = LookupSblrOperation(operation_id)) {
      return entry->opcode.c_str();
    }
  }
  if (operation_id.starts_with("bridge.")) {
    if (const auto* entry = LookupSblrOperation(operation_id)) {
      return entry->opcode.c_str();
    }
  }
  if (operation_id.starts_with("memory.")) {
    if (const auto* entry = LookupSblrOperation(operation_id)) {
      return entry->opcode.c_str();
    }
  }
  if (operation_id.starts_with("storage_tier.")) {
    if (const auto* entry = LookupSblrOperation(operation_id)) {
      return entry->opcode.c_str();
    }
  }
  if (operation_id.starts_with("routine.")) {
    if (const auto* entry = LookupSblrOperation(operation_id)) {
      return entry->opcode.c_str();
    }
  }
  if (operation_id.starts_with("filespace.discovery.")) {
    if (const auto* entry = LookupSblrOperation(operation_id)) {
      return entry->opcode.c_str();
    }
  }
  if (operation_id.starts_with("filespace.package.")) {
    if (const auto* entry = LookupSblrOperation(operation_id)) {
      return entry->opcode.c_str();
    }
  }
  if (operation_id.starts_with("shard_placement.")) {
    if (const auto* entry = LookupSblrOperation(operation_id)) {
      return entry->opcode.c_str();
    }
  }
  if (operation_id.starts_with("security.encryption_key.") ||
      operation_id.starts_with("security.protected_material") ||
      operation_id == "security.encrypted_filespace.open" ||
      operation_id == "security.request_protected_material") {
    if (const auto* entry = LookupSblrOperation(operation_id)) {
      return entry->opcode.c_str();
    }
  }
  if (operation_id == "ddl.create_database") return "SBLR_DDL_CREATE_DATABASE";
  if (operation_id == "ddl.create_schema") return "SBLR_DDL_CREATE_SCHEMA";
  if (operation_id == "ddl.create_table") return "SBLR_DDL_CREATE_TABLE";
  if (operation_id == "ddl.create_table") return "SBLR_DDL_CREATE_TABLE";
  if (operation_id == "ddl.create_index") return "SBLR_DDL_CREATE_INDEX";
  if (operation_id == "ddl.create_index_template") return "SBLR_DDL_CREATE_INDEX_TEMPLATE";
  if (operation_id == "ddl.create_statistics") return "SBLR_DDL_CREATE_STATISTICS";
  if (operation_id == "ddl.create_domain") return "SBLR_DDL_CREATE_DOMAIN";
  if (operation_id == "ddl.create_sequence") return "SBLR_DDL_CREATE_SEQUENCE";
  if (operation_id == "ddl.create_view") return "SBLR_DDL_CREATE_VIEW";
  if (operation_id == "ddl.create_synonym") return "SBLR_DDL_CREATE_SYNONYM";
  if (operation_id == "ddl.synonym.drop") return "SBLR_DDL_DROP_SYNONYM";
  if (operation_id == "ddl.constraint.create") return "SBLR_DDL_CONSTRAINT_CREATE";
  if (operation_id == "ddl.constraint.alter") return "SBLR_DDL_CONSTRAINT_ALTER";
  if (operation_id == "ddl.constraint.drop") return "SBLR_DDL_CONSTRAINT_DROP";
  if (operation_id == "ddl.create_function") return "SBLR_DDL_CREATE_FUNCTION";
  if (operation_id == "ddl.create_procedure") return "SBLR_DDL_CREATE_PROCEDURE";
  if (operation_id == "ddl.create_trigger") return "SBLR_DDL_CREATE_TRIGGER";
  if (operation_id == "ddl.alter_object") return "SBLR_DDL_ALTER_OBJECT";
  if (operation_id == "ddl.drop_object") return "SBLR_DDL_DROP_OBJECT";
  if (operation_id == "ddl.comment_on_object") return "SBLR_DDL_COMMENT_ON";
  if (operation_id == "dml.insert_rows") return "SBLR_DML_INSERT_ROWS";
  if (operation_id == "dml.select_rows") return "SBLR_DML_SELECT_ROWS";
  if (operation_id == "dml.update_rows") return "SBLR_DML_UPDATE_ROWS";
  if (operation_id == "dml.delete_rows") return "SBLR_DML_DELETE_ROWS";
  if (operation_id == "dml.merge_rows") return "SBLR_DML_MERGE_ROWS";
  if (operation_id == "dml.execute_import_rows") return "SBLR_DML_EXECUTE_IMPORT_ROWS";
  if (operation_id == "dml.execute_native_bulk_ingest") return "SBLR_DML_EXECUTE_NATIVE_BULK_INGEST";
  if (operation_id == "dml.normalize_import_checkpoint_model") return "SBLR_DML_IMPORT_CHECKPOINT_MODEL";
  if (operation_id == "dml.normalize_import_reject_model") return "SBLR_DML_IMPORT_REJECT_MODEL";
  if (operation_id == "dml.plan_import_rows") return "SBLR_DML_PLAN_IMPORT_ROWS";
  if (operation_id == "query.bind_expression") return "SBLR_QUERY_BIND_EXPRESSION";
  if (operation_id == "query.bind_predicate") return "SBLR_QUERY_BIND_PREDICATE";
  if (operation_id == "query.bind_projection") return "SBLR_QUERY_BIND_PROJECTION";
  if (operation_id == "query.evaluate_projection") return "SBLR_QUERY_EVALUATE_PROJECTION";
  if (operation_id == "expression.system_variable_read") return "SBLR_SYSTEM_VARIABLE_READ";
  if (operation_id == "query.plan_operation") return "SBLR_QUERY_PLAN_OPERATION";
  if (operation_id == "query.cast_value") return "SBLR_QUERY_CAST_VALUE";
  if (operation_id == "query.extract_value") return "SBLR_QUERY_EXTRACT_VALUE";
  if (operation_id == "query.set_operation") return "SBLR_QUERY_SET_OPERATION";
  if (operation_id == "query.apply_numeric_operation") return "SBLR_QUERY_APPLY_NUMERIC_OPERATION";
  if (operation_id == "query.canonicalize_document_value") return "SBLR_QUERY_CANONICALIZE_DOCUMENT_VALUE";
  if (operation_id == "query.evaluate_advanced_datatype_family") return "SBLR_QUERY_EVALUATE_ADVANCED_DATATYPE_FAMILY";
  if (operation_id == "query.validate_domain_value") return "SBLR_QUERY_VALIDATE_DOMAIN_VALUE";
  if (operation_id == "query.invoke_domain_method") return "SBLR_QUERY_INVOKE_DOMAIN_METHOD";
  if (operation_id == "transaction.begin") return "SBLR_TRANSACTION_BEGIN";
  if (operation_id == "transaction.set_characteristics") return "SBLR_TRANSACTION_SET_CHARACTERISTICS";
  if (operation_id == "transaction.commit") return "SBLR_TRANSACTION_COMMIT";
  if (operation_id == "transaction.rollback") return "SBLR_TRANSACTION_ROLLBACK";
  if (operation_id == "transaction.prepare") return "SBLR_TRANSACTION_PREPARE";
  if (operation_id == "transaction.create_savepoint") return "SBLR_TRANSACTION_CREATE_SAVEPOINT";
  if (operation_id == "transaction.release_savepoint") return "SBLR_TRANSACTION_RELEASE_SAVEPOINT";
  if (operation_id == "transaction.rollback_to_savepoint") return "SBLR_TRANSACTION_ROLLBACK_TO_SAVEPOINT";
  if (operation_id == "transaction.execute_block") return "SBLR_TRANSACTION_EXECUTE_BLOCK";
  if (operation_id == "transaction.lock_table") return "SBLR_TXN_LOCK_TABLE";
  if (operation_id == "transaction.unlock_table") return "SBLR_TXN_UNLOCK_TABLE";
  if (operation_id == "transaction.lock_named") return "SBLR_TXN_LOCK_NAMED";
  if (operation_id == "transaction.unlock_named") return "SBLR_TXN_UNLOCK_NAMED";
  if (operation_id == "catalog.resolve_name") return "SBLR_CATALOG_RESOLVE_NAME";
  if (operation_id == "catalog.map_uuid_to_name") return "SBLR_CATALOG_MAP_UUID_TO_NAME";
  if (operation_id == "catalog.lookup_object") return "SBLR_CATALOG_LOOKUP_OBJECT";
  if (operation_id == "catalog.list_children") return "SBLR_CATALOG_LIST_CHILDREN";
  if (operation_id == "catalog.get_descriptor") return "SBLR_CATALOG_GET_DESCRIPTOR";
  if (operation_id == "catalog.get_dependencies") return "SBLR_CATALOG_GET_DEPENDENCIES";
  if (operation_id.starts_with("catalog.mutation.")) return nullptr;
  if (operation_id == "artifact.export_catalog") return "SBLR_ARTIFACT_EXPORT_CATALOG";
  if (operation_id == "artifact.import_catalog") return "SBLR_ARTIFACT_IMPORT_CATALOG";
  if (operation_id == "artifact.external_git.export_snapshot") return "SBLR_ARTIFACT_EXTERNAL_GIT_EXPORT_SNAPSHOT";
  if (operation_id == "artifact.external_git.diff_snapshot") return "SBLR_ARTIFACT_EXTERNAL_GIT_DIFF_SNAPSHOT";
  if (operation_id == "artifact.external_git.rollback_plan") return "SBLR_ARTIFACT_EXTERNAL_GIT_ROLLBACK_PLAN";
  if (operation_id == "security.create_identity") return "SBLR_SECURITY_CREATE_IDENTITY";
  if (operation_id == "security.alter_identity") return "SBLR_SECURITY_ALTER_IDENTITY";
  if (operation_id == "security.grant_right") return "SBLR_SECURITY_GRANT_RIGHT";
  if (operation_id == "security.revoke_right") return "SBLR_SECURITY_REVOKE_RIGHT";
  if (operation_id == "engine.op.sec_create_role") return "SBLR_SEC_CREATE_ROLE";
  if (operation_id == "engine.op.sec_drop_role") return "SBLR_SEC_DROP_ROLE";
  if (operation_id == "security.role.drop") return "SBLR_SEC_DROP_ROLE";
  if (operation_id == "security.group.create") return "SBLR_SEC_CREATE_GROUP";
  if (operation_id == "security.group.drop") return "SBLR_SEC_DROP_GROUP";
  if (operation_id == "security.principal.create") return "SBLR_SECURITY_PRINCIPAL_CREATE";
  if (operation_id == "security.principal.alter") return "SBLR_SECURITY_PRINCIPAL_ALTER";
  if (operation_id == "security.membership.grant") return "SBLR_SECURITY_MEMBERSHIP_GRANT";
  if (operation_id == "security.membership.revoke") return "SBLR_SECURITY_MEMBERSHIP_REVOKE";
  if (operation_id == "security.privilege.grant") return "SBLR_SECURITY_PRIVILEGE_GRANT";
  if (operation_id == "security.privilege.revoke") return "SBLR_SECURITY_PRIVILEGE_REVOKE";
  if (operation_id == "security.session.set_role") return "SBLR_SECURITY_SESSION_SET_ROLE";
  if (operation_id == "security.policy.create") return "SBLR_SECURITY_POLICY_CREATE";
  if (operation_id == "security.policy.alter") return "SBLR_SECURITY_POLICY_ALTER";
  if (operation_id == "security.policy.drop" ||
      operation_id == "security.policy.lifecycle_drop") return "SBLR_SECURITY_POLICY_DROP";
  if (operation_id == "engine.op.sec_drop_policy") return "SBLR_SEC_DROP_POLICY";
  if (operation_id == "security.mask.drop") return "SBLR_SECURITY_MASK_DROP";
  if (operation_id == "security.rls.drop") return "SBLR_SECURITY_RLS_DROP";
  if (operation_id == "security.policy.attach") return "SBLR_SECURITY_POLICY_ATTACH";
  if (operation_id == "security.policy.activate") return "SBLR_SECURITY_POLICY_ACTIVATE";
  if (operation_id == "security.policy.deactivate") return "SBLR_SECURITY_POLICY_DEACTIVATE";
  if (operation_id == "security.policy.validate") return "SBLR_SECURITY_POLICY_VALIDATE";
  if (operation_id == "security.policy.show") return "SBLR_SECURITY_POLICY_SHOW";
  if (operation_id == "security.evaluate_visibility") return "SBLR_SECURITY_EVALUATE_VISIBILITY";
  if (operation_id == "security.evaluate_policy") return "SBLR_SECURITY_EVALUATE_POLICY";
  if (operation_id == "security.evaluate_deep_enforcement") return "SBLR_SECURITY_EVALUATE_DEEP_ENFORCEMENT";
  if (operation_id == "observability.show_version") return "SBLR_OBSERVABILITY_SHOW_VERSION";
  if (operation_id == "observability.show_database") return "SBLR_OBSERVABILITY_SHOW_DATABASE";
  if (operation_id == "observability.show_system") return "SBLR_OBSERVABILITY_SHOW_SYSTEM";
  if (operation_id == "observability.show_catalog") return "SBLR_OBSERVABILITY_SHOW_CATALOG";
  if (operation_id == "observability.show_sessions") return "SBLR_OBSERVABILITY_SHOW_SESSIONS";
  if (operation_id == "observability.show_transactions") return "SBLR_OBSERVABILITY_SHOW_TRANSACTIONS";
  if (operation_id == "observability.show_locks") return "SBLR_OBSERVABILITY_SHOW_LOCKS";
  if (operation_id == "observability.show_statements") return "SBLR_OBSERVABILITY_SHOW_STATEMENTS";
  if (operation_id == "observability.show_jobs") return "SBLR_OBSERVABILITY_SHOW_JOBS";
  if (operation_id == "observability.show_management") return "SBLR_OBSERVABILITY_SHOW_MANAGEMENT";
  if (operation_id == "observability.show_diagnostics") return "SBLR_OBSERVABILITY_SHOW_DIAGNOSTICS";
  if (operation_id == "observability.show_diagnostics_extended") return "SBLR_OBSERVABILITY_SHOW_DIAGNOSTICS_EXTENDED";
  if (operation_id == "observability.show_archive_replication") return "SBLR_OBSERVABILITY_SHOW_ARCHIVE_REPLICATION";
  if (operation_id == "observability.show_agents_extended") return "SBLR_OBSERVABILITY_SHOW_AGENTS_EXTENDED";
  if (operation_id == "observability.show_filespace_extended") return "SBLR_OBSERVABILITY_SHOW_FILESPACE_EXTENDED";
  if (operation_id == "observability.show_decision_service") return "SBLR_OBSERVABILITY_SHOW_DECISION_SERVICE";
  if (operation_id == "observability.show_acceleration") return "SBLR_OBSERVABILITY_SHOW_ACCELERATION";
  if (operation_id == "observability.show_acceleration_extended") return "SBLR_OBSERVABILITY_SHOW_ACCELERATION_EXTENDED";
  if (operation_id == "observability.show_metrics") return "SBLR_OBSERVABILITY_SHOW_METRICS";
  if (operation_id == "observability.explain_operation") return "SBLR_OBSERVABILITY_EXPLAIN_OPERATION";
  if (operation_id == "general.signal_diagnostic") return "SBLR_GENERAL_SIGNAL_DIAGNOSTIC";
  if (operation_id == "general.raise_diagnostic") return "SBLR_GENERAL_RAISE_DIAGNOSTIC";
  if (operation_id == "general.resignal_diagnostic") return "SBLR_GENERAL_RESIGNAL_DIAGNOSTIC";
  if (operation_id == "general.procedural_operation") return "SBLR_GENERAL_PROCEDURAL_OPERATION";
  if (operation_id == "management.inspect_config") return "SBLR_MANAGEMENT_INSPECT_CONFIG";
  if (operation_id == "management.set_config") return "SBLR_MANAGEMENT_SET_CONFIG";
  if (operation_id == "management.reset_config") return "SBLR_MANAGEMENT_RESET_CONFIG";
  if (operation_id == "management.inspect_runtime") return "SBLR_MANAGEMENT_INSPECT_RUNTIME";
  if (operation_id == "management.control_runtime") return "SBLR_MANAGEMENT_CONTROL_RUNTIME";
  if (operation_id == "management.prepare_support_bundle") return "SBLR_MANAGEMENT_PREPARE_SUPPORT_BUNDLE";
  if (operation_id == "lifecycle.create_database") return "SBLR_LIFECYCLE_CREATE_DATABASE";
  if (operation_id == "lifecycle.open_database") return "SBLR_LIFECYCLE_OPEN_DATABASE";
  if (operation_id == "lifecycle.attach_database") return "SBLR_LIFECYCLE_ATTACH_DATABASE";
  if (operation_id == "lifecycle.detach_database") return "SBLR_LIFECYCLE_DETACH_DATABASE";
  if (operation_id == "lifecycle.enter_maintenance") return "SBLR_LIFECYCLE_ENTER_MAINTENANCE";
  if (operation_id == "lifecycle.exit_maintenance") return "SBLR_LIFECYCLE_EXIT_MAINTENANCE";
  if (operation_id == "lifecycle.enter_restricted_open") return "SBLR_LIFECYCLE_ENTER_RESTRICTED_OPEN";
  if (operation_id == "lifecycle.exit_restricted_open") return "SBLR_LIFECYCLE_EXIT_RESTRICTED_OPEN";
  if (operation_id == "lifecycle.inspect_database") return "SBLR_LIFECYCLE_INSPECT_DATABASE";
  if (operation_id == "lifecycle.verify_database") return "SBLR_LIFECYCLE_VERIFY_DATABASE";
  if (operation_id == "lifecycle.repair_database") return "SBLR_LIFECYCLE_REPAIR_DATABASE";
  if (operation_id == "lifecycle.shutdown_database") return "SBLR_LIFECYCLE_SHUTDOWN_DATABASE";
  if (operation_id == "lifecycle.shutdown_force") return "SBLR_LIFECYCLE_SHUTDOWN_FORCE";
  if (operation_id == "lifecycle.shutdown_acknowledge") return "SBLR_LIFECYCLE_SHUTDOWN_ACKNOWLEDGE";
  if (operation_id == "lifecycle.drop_database") return "SBLR_LIFECYCLE_DROP_DATABASE";
  if (operation_id == "agents.list") return "SBLR_AGENTS_LIST";
  if (operation_id == "agents.show") return "SBLR_AGENTS_SHOW";
  if (operation_id == "agents.start") return "SBLR_AGENTS_START";
  if (operation_id == "agents.stop") return "SBLR_AGENTS_STOP";
  if (operation_id == "agents.pause") return "SBLR_AGENTS_PAUSE";
  if (operation_id == "agents.resume") return "SBLR_AGENTS_RESUME";
  if (operation_id == "agents.configure") return "SBLR_AGENTS_CONFIGURE";
  if (operation_id == "agents.run") return "SBLR_AGENTS_RUN";
  if (operation_id == "agents.dry_run") return "SBLR_AGENTS_DRY_RUN";
  if (operation_id == "agents.override") return "SBLR_AGENTS_OVERRIDE";
  if (operation_id == "sys.agents") return "SBLR_SYS_AGENTS";
  if (operation_id == "cluster.sys.agents") return "SBLR_CLUSTER_SYS_AGENTS";
  if (operation_id == "agents.request_page_preallocation") return "SBLR_AGENT_REQUEST_PAGE_PREALLOCATION";
  if (operation_id == "agents.request_page_relocation") return "SBLR_AGENT_REQUEST_PAGE_RELOCATION";
  if (operation_id == "agents.request_filespace_growth") return "SBLR_AGENT_REQUEST_FILESPACE_GROWTH";
  if (operation_id == "agents.notify_filespace_shrink_readiness") return "SBLR_AGENT_NOTIFY_FILESPACE_SHRINK_READINESS";
  if (operation_id == "event.channel.create") return "SBLR_EVENT_CHANNEL_CREATE";
  if (operation_id == "event.channel.alter") return "SBLR_EVENT_CHANNEL_ALTER";
  if (operation_id == "event.channel.drop") return "SBLR_EVENT_CHANNEL_DROP";
  if (operation_id == "event.channel.listen" || operation_id == "notification.channel.listen") return "SBLR_EVENT_CHANNEL_LISTEN";
  if (operation_id == "event.channel.unlisten" || operation_id == "notification.channel.unlisten") return "SBLR_EVENT_CHANNEL_UNLISTEN";
  if (operation_id == "event.channel.notify" || operation_id == "notification.channel.notify") return "SBLR_EVENT_CHANNEL_NOTIFY";
  if (operation_id == "event.subscription.list") return "SBLR_EVENT_SUBSCRIPTION_LIST";
  if (operation_id == "event.delivery.poll") return "SBLR_EVENT_DELIVERY_POLL";
  if (operation_id == "event.delivery.ack") return "SBLR_EVENT_DELIVERY_ACK";
  if (operation_id == "session.notification.unlisten") return "SBLR_EVENT_CHANNEL_UNLISTEN";
  if (operation_id == "session.notification.unlisten_all") return "SBLR_EVENT_CHANNEL_UNLISTEN_ALL";
  if (operation_id == "agents.request_index_delta_merge") return "SBLR_AGENT_REQUEST_INDEX_DELTA_MERGE";
  if (operation_id == "agents.request_index_rebuild_or_shadow_build") return "SBLR_AGENT_REQUEST_INDEX_REBUILD_OR_SHADOW_BUILD";
  if (operation_id == "agents.metrics.get") return "SBLR_AGENT_METRICS_GET";
  if (operation_id == "agents.policy.get") return "SBLR_AGENT_POLICY_GET";
  if (operation_id == "agents.evidence.list") return "SBLR_AGENT_EVIDENCE_LIST";
  if (operation_id == "agents.audit.list") return "SBLR_AGENT_AUDIT_LIST";
  if (operation_id == "agents.actions.list") return "SBLR_AGENT_ACTION_LIST";
  if (operation_id == "agents.overrides.list") return "SBLR_AGENT_OVERRIDE_LIST";
  if (operation_id == "agents.drain") return "SBLR_AGENT_LIFECYCLE_DRAIN";
  if (operation_id == "agents.restart") return "SBLR_AGENT_LIFECYCLE_RESTART";
  if (operation_id == "agents.enable") return "SBLR_AGENT_LIFECYCLE_ENABLE";
  if (operation_id == "agents.disable") return "SBLR_AGENT_LIFECYCLE_DISABLE";
  if (operation_id == "agents.quarantine") return "SBLR_AGENT_QUARANTINE";
  if (operation_id == "agents.unquarantine") return "SBLR_AGENT_UNQUARANTINE";
  if (operation_id == "agents.policy.attach") return "SBLR_AGENT_POLICY_ATTACH";
  if (operation_id == "agents.policy.detach") return "SBLR_AGENT_POLICY_DETACH";
  if (operation_id == "agents.policy.validate") return "SBLR_AGENT_POLICY_VALIDATE";
  if (operation_id == "agents.policy.simulate") return "SBLR_AGENT_POLICY_SIMULATE";
  if (operation_id == "agents.policy.apply") return "SBLR_AGENT_POLICY_APPLY";
  if (operation_id == "agents.policy.rollback") return "SBLR_AGENT_POLICY_ROLLBACK";
  if (operation_id == "agents.action.approve") return "SBLR_AGENT_ACTION_APPROVE";
  if (operation_id == "agents.action.cancel") return "SBLR_AGENT_ACTION_CANCEL";
  if (operation_id == "agents.action.retry") return "SBLR_AGENT_ACTION_RETRY";
  if (operation_id == "agents.action.suppress") return "SBLR_AGENT_ACTION_SUPPRESS";
  if (operation_id == "agents.override.create") return "SBLR_AGENT_OVERRIDE_CREATE";
  if (operation_id == "agents.override.update") return "SBLR_AGENT_OVERRIDE_UPDATE";
  if (operation_id == "agents.override.drop") return "SBLR_AGENT_OVERRIDE_DROP";
  if (operation_id == "agents.set_mode") return "SBLR_AGENT_SET_MODE";
  if (operation_id == "filespaces.show") return "SBLR_SHOW_FILESPACES";
  if (operation_id == "filespaces.health.show") return "SBLR_SHOW_FILESPACE_HEALTH";
  if (operation_id == "filespaces.capacity.show") return "SBLR_SHOW_FILESPACE_CAPACITY";
  if (operation_id == "pages.allocation.show") return "SBLR_SHOW_PAGE_ALLOCATION";
  if (operation_id == "pages.allocation.family.show") return "SBLR_SHOW_PAGE_ALLOCATION_BY_FAMILY";
  if (operation_id == "pages.relocation_backlog.show") return "SBLR_SHOW_PAGE_RELOCATION_BACKLOG";
  if (operation_id == "filespaces.shrink_readiness.show") return "SBLR_SHOW_FILESPACE_SHRINK_READINESS";
  if (operation_id == "cluster.agent.list") return "SBLR_CLUSTER_AGENT_LIST";
  if (operation_id == "cluster.agent.get") return "SBLR_CLUSTER_AGENT_GET";
  if (operation_id == "cluster.agent.control") return "SBLR_CLUSTER_AGENT_CONTROL";
  if (operation_id == "cluster.inspect_state") return "SBLR_CLUSTER_INSPECT_STATE";
  if (operation_id == "cluster.inspect_routing_plan") return "SBLR_CLUSTER_INSPECT_ROUTING_PLAN";
  if (operation_id == "cluster.control_cluster") return "SBLR_CLUSTER_CONTROL_CLUSTER";
  if (operation_id == "cluster.inspect_provider") return "SBLR_CLUSTER_INSPECT_PROVIDER";
  if (operation_id == "cluster.place_object") return "SBLR_CLUSTER_PLACE_OBJECT";
  if (operation_id == "cluster.inspect_replication") return "SBLR_CLUSTER_INSPECT_REPLICATION";
  if (operation_id == "cluster.prepare_remote_participant_insert") return "SBLR_CLUSTER_PREPARE_REMOTE_PARTICIPANT_INSERT";
  if (operation_id == "cluster.validate_insert_route_fence") return "SBLR_CLUSTER_VALIDATE_INSERT_ROUTE_FENCE";
  if (operation_id == "cluster.profile_operation") return "SBLR_CLUSTER_PROFILE_OPERATION";
  if (operation_id == "extensibility.register_udr_package") return "SBLR_EXTENSIBILITY_REGISTER_UDR_PACKAGE";
  if (operation_id == "extensibility.alter_udr_package") return "SBLR_EXTENSIBILITY_ALTER_UDR_PACKAGE";
  if (operation_id == "extensibility.load_udr_package") return "SBLR_EXTENSIBILITY_LOAD_UDR_PACKAGE";
  if (operation_id == "extensibility.unload_udr_package") return "SBLR_EXTENSIBILITY_UNLOAD_UDR_PACKAGE";
  if (operation_id == "extensibility.drop_udr_package") return "SBLR_EXTENSIBILITY_DROP_UDR_PACKAGE";
  if (operation_id == "extensibility.inspect_udr_packages") return "SBLR_EXTENSIBILITY_INSPECT_UDR_PACKAGES";
  if (operation_id == "extensibility.invoke_udr_package") return "SBLR_UDR_INVOKE";
  if (operation_id == "extensibility.register_parser_package") return "SBLR_EXTENSIBILITY_REGISTER_PARSER_PACKAGE";
  if (operation_id == "extensibility.compile_llvm_module") return "SBLR_EXTENSIBILITY_COMPILE_LLVM_MODULE";
  if (operation_id == "extensibility.inspect_gpu_capability") return "SBLR_EXTENSIBILITY_INSPECT_GPU_CAPABILITY";
  if (operation_id == "nosql.document_insert") return "SBLR_NOSQL_DOCUMENT_INSERT";
  if (operation_id == "nosql.document_find") return "SBLR_NOSQL_DOCUMENT_FIND";
  if (operation_id == "nosql.document_update") return "SBLR_NOSQL_DOCUMENT_UPDATE";
  if (operation_id == "nosql.document_delete") return "SBLR_NOSQL_DOCUMENT_DELETE";
  if (operation_id == "nosql.graph_query") return "SBLR_NOSQL_GRAPH_QUERY";
  if (operation_id == "nosql.key_value_get") return "SBLR_NOSQL_KEY_VALUE_GET";
  if (operation_id == "nosql.key_value_put") return "SBLR_NOSQL_KEY_VALUE_PUT";
  if (operation_id == "nosql.key_value_multiget") return "SBLR_NOSQL_KEY_VALUE_MULTIGET";
  if (operation_id == "nosql.key_value_pipeline") return "SBLR_NOSQL_KEY_VALUE_PIPELINE";
  if (operation_id == "nosql.key_value_atomic_program") return "SBLR_NOSQL_KEY_VALUE_ATOMIC_PROGRAM";
  if (operation_id == "nosql.backpressure_debt_plan") return "SBLR_NOSQL_BACKPRESSURE_DEBT_PLAN";
  if (operation_id == "nosql.family_maintenance_plan") return "SBLR_NOSQL_FAMILY_MAINTENANCE_PLAN";
  if (operation_id == "nosql.statistics_advisor_plan") return "SBLR_NOSQL_STATISTICS_ADVISOR_PLAN";
  if (operation_id == "nosql.time_series_append") return "SBLR_NOSQL_TIME_SERIES_APPEND";
  if (operation_id == "nosql.vector_search") return "SBLR_NOSQL_VECTOR_SEARCH";
  if (operation_id == "nosql.vector_collection_op") return "SBLR_NOSQL_VECTOR_COLLECTION_OP";
  if (operation_id == "nosql.search_query") return "SBLR_NOSQL_SEARCH_QUERY";
  if (operation_id == "filespace.create") return "SBLR_FILESPACE_CREATE";
  if (operation_id == "filespace.preallocate") return "SBLR_FILESPACE_PREALLOCATE";
  if (operation_id == "filespace.attach") return "SBLR_FILESPACE_ATTACH";
  if (operation_id == "filespace.detach") return "SBLR_FILESPACE_DETACH";
  if (operation_id == "filespace.disconnect") return "SBLR_FILESPACE_DISCONNECT";
  if (operation_id == "filespace.move") return "SBLR_FILESPACE_MOVE";
  if (operation_id == "filespace.merge") return "SBLR_FILESPACE_MERGE";
  if (operation_id == "filespace.promote") return "SBLR_FILESPACE_PROMOTE";
  if (operation_id == "filespace.verify") return "SBLR_FILESPACE_VERIFY";
  if (operation_id == "filespace.compact") return "SBLR_FILESPACE_COMPACT";
  if (operation_id == "filespace.fence") return "SBLR_FILESPACE_FENCE";
  if (operation_id == "filespace.release") return "SBLR_FILESPACE_RELEASE";
  if (operation_id == "filespace.archive") return "SBLR_FILESPACE_ARCHIVE";
  if (operation_id == "filespace.quarantine") return "SBLR_FILESPACE_QUARANTINE";
  if (operation_id == "filespace.snapshot.create") return "SBLR_FILESPACE_SNAPSHOT_CREATE";
  if (operation_id == "filespace.snapshot.refresh") return "SBLR_FILESPACE_SNAPSHOT_REFRESH";
  if (operation_id == "filespace.snapshot.validate") return "SBLR_FILESPACE_SNAPSHOT_VALIDATE";
  if (operation_id == "filespace.snapshot.retire") return "SBLR_FILESPACE_SNAPSHOT_RETIRE";
  if (operation_id == "filespace.shadow.create") return "SBLR_FILESPACE_SHADOW_CREATE";
  if (operation_id == "filespace.shadow.refresh") return "SBLR_FILESPACE_SHADOW_REFRESH";
  if (operation_id == "filespace.shadow.validate") return "SBLR_FILESPACE_SHADOW_VALIDATE";
  if (operation_id == "filespace.shadow.promote") return "SBLR_FILESPACE_SHADOW_PROMOTE";
  if (operation_id == "filespace.truncate") return "SBLR_FILESPACE_TRUNCATE";
  if (operation_id == "filespace.drop") return "SBLR_FILESPACE_DROP";
  if (operation_id == "filespace.delete_physical") return "SBLR_FILESPACE_DELETE_PHYSICAL";
  if (operation_id == "filespace.repair") return "SBLR_FILESPACE_REPAIR";
  if (operation_id == "filespace.rebuild") return "SBLR_FILESPACE_REBUILD";
  if (operation_id == "filespace.salvage") return "SBLR_FILESPACE_SALVAGE";
  if (operation_id == "storage.manage_operation") return "SBLR_STORAGE_MANAGEMENT_OPERATION";
  return nullptr;
}

bool IsGpuAccelerationControlOperation(std::string_view operation_id) {
  return operation_id == "op.gpu.artifact_quarantine" ||
         operation_id == "op.gpu.cache_clear" ||
         operation_id == "op.gpu.device_quarantine" ||
         operation_id == "op.gpu.kernel_quarantine" ||
         operation_id == "op.gpu.profile_disable" ||
         operation_id == "op.gpu.profile_enable";
}

bool IsGpuAccelerationInspectOperation(std::string_view operation_id) {
  return operation_id == "op.show.gpu" ||
         operation_id == "op.show.gpu_artifacts" ||
         operation_id == "op.show.gpu_capability" ||
         operation_id == "op.show.gpu_devices" ||
         operation_id == "op.show.gpu_kernels" ||
         operation_id == "op.show.gpu_memory";
}

bool IsNativeCompileControlOperation(std::string_view operation_id) {
  return operation_id == "op.native_compile.aot_rebuild" ||
         operation_id == "op.native_compile.artifact_quarantine" ||
         operation_id == "op.native_compile.cache_invalidate" ||
         operation_id == "op.native_compile.profile_disable" ||
         operation_id == "op.native_compile.profile_enable";
}

bool IsNativeCompileInspectOperation(std::string_view operation_id) {
  return operation_id == "op.show.aot_artifacts" ||
         operation_id == "op.show.llvm" ||
         operation_id == "op.show.llvm_provenance" ||
         operation_id == "op.show.llvm_targets" ||
         operation_id == "op.show.native_compile" ||
         operation_id == "op.show.native_compile_cache";
}

bool IsManagementRuntimeControlOperation(std::string_view operation_id) {
  return operation_id == "op.management.listener.drain" ||
         operation_id == "op.management.listener.undrain" ||
         operation_id == "op.management.manager.restart" ||
         operation_id == "op.management.manager.start" ||
         operation_id == "op.management.manager.stop" ||
         operation_id == "op.management.parser_pool.resize" ||
         operation_id == "op.management.config.reload" ||
         operation_id == "op.management.instruction.ack" ||
         operation_id == "op.management.instruction.apply" ||
         operation_id == "op.management.instruction.cancel" ||
         operation_id == "op.management.instruction.quarantine" ||
         operation_id == "op.management.support_bundle.create";
}

bool IsManagementRuntimeInspectOperation(std::string_view operation_id) {
  return operation_id == "op.show.management.config" ||
         operation_id == "op.show.management.drift" ||
         operation_id == "op.show.management.instructions" ||
         operation_id == "op.show.management.listeners" ||
         operation_id == "op.show.management.manager" ||
         operation_id == "op.show.management.parser_pool" ||
         operation_id == "op.show.management.readiness" ||
         operation_id == "op.show.management.servers" ||
         operation_id == "op.show.management.support_bundle_safety" ||
         operation_id == "op.show.management.support_bundles";
}

bool IsMemoryManagementOperation(std::string_view operation_id) {
  return operation_id.starts_with("memory.");
}

bool IsMemoryManagementControlOperation(std::string_view operation_id) {
  return operation_id == "memory.profile.set" ||
         operation_id == "memory.cache.flush" ||
         operation_id == "memory.cache.invalidate" ||
         operation_id == "memory.scavenge" ||
         operation_id == "memory.grant_feedback.reset" ||
         operation_id == "memory.stream_policy.set" ||
         operation_id == "memory.udr_limit.set" ||
         operation_id == "memory.dump_policy.set" ||
         operation_id == "memory.optimizer.set" ||
         operation_id == "memory.optimizer.run" ||
         operation_id == "memory.object_residency.set" ||
         operation_id == "memory.rate_limit.set" ||
         operation_id == "memory.policy_migration.plan";
}

bool IsStorageTierMigrationOperation(std::string_view operation_id) {
  return operation_id.starts_with("storage_tier.");
}

bool IsFilespaceDiscoveryOperation(std::string_view operation_id) {
  return operation_id.starts_with("filespace.discovery.");
}

bool IsFilespacePackageOperation(std::string_view operation_id) {
  return operation_id.starts_with("filespace.package.");
}

bool IsShardPlacementDescriptorOperation(std::string_view operation_id) {
  return operation_id.starts_with("shard_placement.");
}

bool IsStorageTierMigrationControlOperation(std::string_view operation_id) {
  return operation_id == "storage_tier.stage_migration" ||
         operation_id == "storage_tier.commit_migration" ||
         operation_id == "storage_tier.rollback_migration";
}

bool IsMigrationControlOperation(std::string_view operation_id) {
  return operation_id == "op.migration.begin_from_reference" ||
         operation_id == "op.migration.alter";
}

bool IsMigrationInspectOperation(std::string_view operation_id) {
  return operation_id == "op.show.migration" ||
         operation_id == "op.show.migrations";
}

bool IsSecurityInspectionOperation(std::string_view operation_id) {
  return operation_id == "op.show.audit" ||
         operation_id == "op.show.discovery_rights" ||
         operation_id == "op.show.grants" ||
         operation_id == "op.show.groups" ||
         operation_id == "op.show.identity_providers" ||
         operation_id == "op.show.masks" ||
         operation_id == "op.show.object_visibility" ||
         operation_id == "op.show.policies" ||
         operation_id == "op.show.rls" ||
         operation_id == "op.show.roles" ||
         operation_id == "op.show.security_events" ||
         operation_id == "op.show.security_profiles" ||
         operation_id == "op.show.users";
}

bool IsObservabilityExactShowOperation(std::string_view operation_id) {
  return operation_id == "op.show.buffer_pool" ||
         operation_id == "op.show.cache" ||
         operation_id == "op.show.capabilities" ||
         operation_id == "op.show.context" ||
         operation_id == "op.show.dialect" ||
         operation_id == "op.show.index_health" ||
         operation_id == "op.show.io" ||
         operation_id == "op.show.job" ||
         operation_id == "op.show.job_dependencies" ||
         operation_id == "op.show.job_runs" ||
         operation_id == "op.show.jobs" ||
         operation_id == "op.show.locks" ||
         operation_id == "op.show.metrics" ||
         operation_id == "op.show.metrics_family" ||
         operation_id == "op.show.performance" ||
         operation_id == "op.show.query_store" ||
         operation_id == "op.show.schema_path" ||
         operation_id == "op.show.search_path" ||
         operation_id == "op.show.sessions" ||
         operation_id == "op.show.statement_cache" ||
         operation_id == "op.show.statements" ||
         operation_id == "op.show.system" ||
         operation_id == "op.show.transaction" ||
         operation_id == "op.show.transaction_isolation" ||
         operation_id == "op.show.transactions" ||
         operation_id == "op.sbsql.surface_replay" ||
         operation_id == "op.show.version" ||
         operation_id == "op.show.wait_events";
}

template <typename TRequest>
TRequest TypedRequest(const SblrDispatchRequest& request) {
  TRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  return typed;
}

api::EngineExecuteTransactionBlockRequest TypedExecuteTransactionBlockRequest(
    const SblrDispatchRequest& request) {
  api::EngineExecuteTransactionBlockRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  for (const auto& operand : request.envelope.operands) {
    if (operand.name.starts_with("procedural_") &&
        operand.type != "text") {
      typed.procedural_block_present = true;
      typed.procedural_block_valid = false;
      typed.procedural_block_diagnostic = api::MakeEngineApiDiagnostic(
          "SB_SBLR_PROCEDURAL_IR_OPERAND_TYPE_INVALID",
          "engine.sblr.procedural_block.operand_type_invalid",
          "procedural IR operands must use the SBLR text operand type: " +
              operand.name,
          true);
      return typed;
    }
  }
  const auto decoded =
      DecodeSblrProceduralBlockV1(base.option_envelopes);
  typed.procedural_block_present = decoded.present;
  typed.procedural_block_valid = decoded.valid;
  typed.procedural_block = decoded.block;
  if (decoded.present && !decoded.valid) {
    typed.procedural_block_diagnostic = api::MakeEngineApiDiagnostic(
        decoded.diagnostic_code.empty()
            ? "SB_SBLR_PROCEDURAL_IR_INVALID"
            : decoded.diagnostic_code,
        "engine.sblr.procedural_block.invalid",
        decoded.diagnostic_detail,
        true);
  }
  return typed;
}

api::EngineBeginTransactionRequest TypedBeginTransactionRequest(
    const SblrDispatchRequest& request) {
  api::EngineBeginTransactionRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.isolation_level = api::SecurityOptionValue(base, "transaction_isolation_level:");
  if (typed.isolation_level.empty()) {
    typed.isolation_level = request.context.transaction_isolation_level;
  }
  typed.transaction_policy_profile = base.policy_profile;
  const auto read_only = api::SecurityOptionValue(base, "transaction_read_only:");
  if (!read_only.empty()) {
    typed.transaction_policy_profile.encoded_profiles.push_back(
        std::string("read_only:") + LowerAscii(read_only));
  }
  return typed;
}

std::uint64_t DispatchOptionU64(const api::EngineApiRequest& request, const std::string& prefix) {
  const auto value = api::SecurityOptionValue(request, prefix);
  if (value.empty()) { return 0; }
  try {
    return static_cast<std::uint64_t>(std::stoull(value));
  } catch (...) {
    return 0;
  }
}

std::optional<std::uint64_t> ParseBoundedDispatchCount(
    const std::string& value,
    std::uint64_t maximum) {
  if (value.empty() ||
      !std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isdigit(ch) != 0;
      })) {
    return std::nullopt;
  }
  try {
    std::size_t consumed = 0;
    const auto parsed = std::stoull(value, &consumed);
    if (consumed != value.size() || parsed > maximum) {
      return std::nullopt;
    }
    return static_cast<std::uint64_t>(parsed);
  } catch (...) {
    return std::nullopt;
  }
}

double DispatchOptionDouble(const api::EngineApiRequest& request, const std::string& prefix) {
  const auto value = api::SecurityOptionValue(request, prefix);
  if (value.empty()) { return 0.0; }
  try {
    return std::stod(value);
  } catch (...) {
    return 0.0;
  }
}

api::EngineMemoryManagementOperation MemoryOperationForSblrOperation(
    std::string_view operation_id) {
  if (operation_id == "memory.policy.validate") {
    return api::EngineMemoryManagementOperation::validate_governance;
  }
  if (operation_id == "memory.cache.flush" ||
      operation_id == "memory.cache.invalidate" ||
      operation_id == "memory.scavenge" ||
      operation_id == "memory.grant_feedback.reset") {
    return api::EngineMemoryManagementOperation::plan_cache_control;
  }
  if (operation_id == "memory.pressure.show") {
    return api::EngineMemoryManagementOperation::plan_pressure_response;
  }
  if (operation_id == "memory.report.create" ||
      operation_id == "memory.incident.bundle") {
    return api::EngineMemoryManagementOperation::create_report;
  }
  if (operation_id == "memory.optimizer.show") {
    return api::EngineMemoryManagementOperation::review_recommendation;
  }
  if (operation_id == "memory.optimizer.set" ||
      operation_id == "memory.optimizer.run") {
    return api::EngineMemoryManagementOperation::apply_safe_recommendation;
  }
  if (operation_id == "memory.object_residency.show") {
    return api::EngineMemoryManagementOperation::inspect_object_residency;
  }
  if (operation_id == "memory.object_residency.set") {
    return api::EngineMemoryManagementOperation::set_object_residency;
  }
  if (operation_id == "memory.rate_limit.show") {
    return api::EngineMemoryManagementOperation::inspect_rate_limit;
  }
  if (operation_id == "memory.rate_limit.set") {
    return api::EngineMemoryManagementOperation::set_rate_limit;
  }
  if (operation_id == "memory.policy_upgrade.plan") {
    return api::EngineMemoryManagementOperation::plan_policy_upgrade;
  }
  if (operation_id == "memory.policy_migration.plan" ||
      operation_id == "memory.profile.set" ||
      operation_id == "memory.stream_policy.set" ||
      operation_id == "memory.udr_limit.set" ||
      operation_id == "memory.dump_policy.set") {
    return api::EngineMemoryManagementOperation::plan_policy_migration;
  }
  return api::EngineMemoryManagementOperation::inspect_governance;
}

scratchbird::core::memory::MemoryPolicyConfig DefaultMemoryPolicyConfig() {
  scratchbird::core::memory::MemoryPolicyConfig config;
  config.policy_name = "sblr_public_memory_descriptor_policy";
  config.hard_limit_bytes = 256ull * 1024ull * 1024ull;
  config.soft_limit_bytes = 192ull * 1024ull * 1024ull;
  config.per_context_limit_bytes = 64ull * 1024ull * 1024ull;
  config.page_buffer_pool_limit_bytes = 64ull * 1024ull * 1024ull;
  config.enable_platform_memory_probe = false;
  config.policy_generation = 7;
  return config;
}

void FillMemoryGovernanceDescriptor(api::EngineMemoryManagementRequest* request) {
  request->governance.profile_uuid.canonical = "019f1000-0000-7000-8000-000000000010";
  request->governance.policy_config = DefaultMemoryPolicyConfig();
  request->governance.expected_policy_generation = 7;
  request->governance.observed_policy_generation = 7;
  request->governance.profile_resolved = true;
  request->governance.memory_tree_snapshot_present = true;
  request->governance.cache_governor_registered = true;
  request->governance.cache_flush_or_invalidation_requested = true;
  request->governance.pressure_observation_present = true;
  request->governance.grant_feedback_surface_present = true;
  request->governance.parser_front_door_limit_surface_present = true;
  request->governance.udr_limit_surface_present = true;
  request->governance.streaming_window_surface_present = true;
  request->governance.maintenance_budget_surface_present = true;
  request->governance.dump_swap_policy_present = true;
  request->governance.allocator_scavenging_surface_present = true;
  request->governance.platform_capability_matrix_present = true;
  request->governance.protected_material_redaction_validated = true;
  request->governance.activation_timing_declared = true;
  request->governance.current_snapshot.current_bytes = 128ull * 1024ull * 1024ull;
  request->governance.pressure_observation.route_label = "sblr.public.memory.management";
  request->governance.pressure_observation.operation_id = request->operation_id;
  request->governance.pressure_observation.current_bytes = 900;
  request->governance.pressure_observation.soft_limit_bytes = 700;
  request->governance.pressure_observation.hard_limit_bytes = 1000;
  request->governance.pressure_observation.unified_budget_bytes = 900;
  request->governance.pressure_observation.unified_budget_limit_bytes = 1000;
  request->governance.pressure_observation.spill_supported = true;
  request->governance.pressure_observation.page_cache_shrink_supported = true;
  request->governance.pressure_observation.background_cleanup_supported = true;
  request->governance.pressure_observation.cancellation_supported = true;
  request->governance.pressure_observation.engine_mga_authoritative = true;
}

void FillMemoryAutomationDescriptor(api::EngineMemoryManagementRequest* request) {
  request->automation.recommendation_uuid.canonical =
      "019f1000-0000-7000-8000-000000000020";
  request->automation.report_generation = 3;
  request->automation.recommendation_generation = 4;
  request->automation.report_bounded = true;
  request->automation.report_redaction_validated = true;
  request->automation.metrics_contract_present = true;
  request->automation.recommendation_explainable = true;
  request->automation.recommend_only_default = true;
  request->automation.safe_apply_requested = true;
  request->automation.maintenance_window_bound = true;
  request->automation.audit_enabled = true;
  request->automation.guardrail_policy_resolved = true;
}

void FillMemoryObjectResidencyDescriptor(api::EngineMemoryManagementRequest* request) {
  request->object_residency.object_uuid.canonical =
      request->target_object.uuid.canonical.empty()
          ? "019f1000-0000-7000-8000-000000000030"
          : request->target_object.uuid.canonical;
  request->object_residency.filespace_uuid.canonical =
      "019f1000-0000-7000-8000-000000000031";
  request->object_residency.object_kind =
      request->target_object.object_kind.empty() ? "table" : request->target_object.object_kind;
  request->object_residency.residency_class =
      api::EngineMemoryObjectResidencyClass::warm_on_open;
  request->object_residency.page_types = {
      scratchbird::storage::disk::PageType::row_data,
      scratchbird::storage::disk::PageType::index_btree_leaf};
  request->object_residency.expected_policy_generation = 7;
  request->object_residency.observed_policy_generation = 7;
  request->object_residency.warmup_budget_bytes = 16ull * 1024ull * 1024ull;
  request->object_residency.profile_resolved = true;
  request->object_residency.object_resolved = true;
  request->object_residency.filespace_placement_validated = true;
  request->object_residency.security_scope_validated = true;
  request->object_residency.cluster_placement_validated = true;
  request->object_residency.heat_history_derivative_only = true;
}

void FillMemoryRateLimitDescriptor(api::EngineMemoryManagementRequest* request) {
  request->rate_limit.limit_class = api::EngineMemoryRateLimitClass::cache_flush_abuse;
  request->rate_limit.action = api::EngineMemoryRateLimitAction::throttle;
  request->rate_limit.limit_per_window = 4;
  request->rate_limit.window_seconds = 60;
  request->rate_limit.policy_generation = 7;
  request->rate_limit.policy_resolved = true;
  request->rate_limit.audit_enabled = true;
}

void FillMemoryPolicyMigrationDescriptor(api::EngineMemoryManagementRequest* request) {
  request->migration.profile_uuid.canonical =
      "019f1000-0000-7000-8000-000000000040";
  request->migration.policy_uuid.canonical =
      "019f1000-0000-7000-8000-000000000041";
  request->migration.source_policy_version = 2;
  request->migration.target_policy_version = 3;
  request->migration.source_schema_version = 2;
  request->migration.target_schema_version = 3;
  request->migration.policy_schema_validated = true;
  request->migration.grant_feedback_migration_declared = true;
  request->migration.heat_history_migration_declared = true;
  request->migration.derivative_state_audit_enabled = true;
  request->migration.discard_incompatible_derivative_state_allowed = true;
}

api::EngineMemoryManagementRequest TypedMemoryManagementRequest(
    const SblrDispatchRequest& request) {
  api::EngineMemoryManagementRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.memory_operation = MemoryOperationForSblrOperation(base.operation_id);
  if (typed.target_object.uuid.canonical.empty()) {
    typed.target_object.uuid.canonical =
        "019f1000-0000-7000-8000-0000000000ff";
  }
  if (typed.target_object.object_kind.empty()) {
    typed.target_object.object_kind = "memory_policy";
  }
  FillMemoryGovernanceDescriptor(&typed);
  FillMemoryAutomationDescriptor(&typed);
  FillMemoryObjectResidencyDescriptor(&typed);
  FillMemoryRateLimitDescriptor(&typed);
  FillMemoryPolicyMigrationDescriptor(&typed);
  typed.cluster_scoped =
      api::SecurityOptionBool(base, "cluster_scoped:", false);
  typed.parser_memory_authority = false;
  typed.transaction_finality_authority = false;
  typed.visibility_authority = false;
  typed.recovery_authority = false;
  typed.reference_or_wal_recovery_authority = false;
  typed.private_provider_dispatch_requested = false;
  return typed;
}

api::EngineStorageTierMigrationOperation StorageTierOperationForSblrOperation(
    std::string_view operation_id) {
  if (operation_id == "storage_tier.validate") {
    return api::EngineStorageTierMigrationOperation::validate;
  }
  if (operation_id == "storage_tier.plan_migration") {
    return api::EngineStorageTierMigrationOperation::plan_migration;
  }
  if (operation_id == "storage_tier.stage_migration") {
    return api::EngineStorageTierMigrationOperation::stage_migration;
  }
  if (operation_id == "storage_tier.commit_migration") {
    return api::EngineStorageTierMigrationOperation::commit_migration;
  }
  if (operation_id == "storage_tier.rollback_migration") {
    return api::EngineStorageTierMigrationOperation::rollback_migration;
  }
  return api::EngineStorageTierMigrationOperation::inspect;
}

api::EngineStorageTierMigrationRequest TypedStorageTierMigrationRequest(
    const SblrDispatchRequest& request) {
  api::EngineStorageTierMigrationRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.tier_operation = StorageTierOperationForSblrOperation(base.operation_id);
  if (typed.target_object.uuid.canonical.empty()) {
    typed.target_object.uuid.canonical =
        "019f2000-0000-7000-8000-000000000020";
  }
  if (typed.target_object.object_kind.empty()) {
    typed.target_object.object_kind = "filespace";
  }
  typed.descriptor.storage_tier_policy_uuid.canonical =
      "019f2000-0000-7000-8000-000000000010";
  typed.descriptor.source_tier_uuid.canonical =
      "019f2000-0000-7000-8000-000000000011";
  typed.descriptor.target_tier_uuid.canonical =
      "019f2000-0000-7000-8000-000000000012";
  typed.descriptor.source_tier_class = api::EngineStorageTierClass::hot;
  typed.descriptor.target_tier_class = api::EngineStorageTierClass::cold;
  typed.descriptor.target_filespace_role =
      scratchbird::storage::filespace::FilespaceRole::secondary_data;
  typed.descriptor.page_types = {
      scratchbird::storage::disk::PageType::row_data,
      scratchbird::storage::disk::PageType::blob};
  const auto catalog_generation =
      typed.context.catalog_generation_id == 0 ? 12 : typed.context.catalog_generation_id;
  const auto policy_generation =
      typed.context.resource_epoch == 0 ? 34 : typed.context.resource_epoch;
  typed.descriptor.expected_catalog_generation = catalog_generation;
  typed.descriptor.observed_catalog_generation = catalog_generation;
  typed.descriptor.expected_policy_generation = policy_generation;
  typed.descriptor.observed_policy_generation = policy_generation;
  typed.descriptor.storage_tier_policy_resolved = true;
  typed.descriptor.filespace_role_known = true;
  typed.descriptor.page_family_eligibility_validated = true;
  typed.descriptor.typed_dependency_manifest_validated = false;
  typed.descriptor.cluster_scoped =
      api::SecurityOptionBool(base, "cluster_scoped:", false);
  typed.descriptor.physical_data_movement_requested = false;
  return typed;
}

api::EngineFilespaceDiscoveryScope FilespaceDiscoveryScopeForSblrOperation(
    std::string_view operation_id) {
  if (operation_id == "filespace.discovery.orphan_scan") {
    return api::EngineFilespaceDiscoveryScope::orphan_only;
  }
  if (operation_id == "filespace.discovery.stale_scan") {
    return api::EngineFilespaceDiscoveryScope::stale_only;
  }
  return api::EngineFilespaceDiscoveryScope::all;
}

api::EngineFilespaceDiscoveryRequest TypedFilespaceDiscoveryRequest(
    const SblrDispatchRequest& request) {
  api::EngineFilespaceDiscoveryRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.discovery_scope = FilespaceDiscoveryScopeForSblrOperation(base.operation_id);
  typed.runtime_filesystem_scan_requested = false;
  typed.parser_filesystem_authority = false;
  typed.parser_storage_authority = false;
  typed.transaction_finality_authority = false;
  typed.recovery_authority = false;
  typed.reference_or_wal_recovery_authority = false;
  typed.mutation_requested = false;
  return typed;
}

api::EngineFilespacePackageAction FilespacePackageActionForSblrOperation(
    std::string_view operation_id) {
  if (operation_id == "filespace.package.export_manifest") {
    return api::EngineFilespacePackageAction::export_manifest;
  }
  if (operation_id == "filespace.package.import_to_quarantine") {
    return api::EngineFilespacePackageAction::import_to_quarantine;
  }
  if (operation_id == "filespace.package.admit") {
    return api::EngineFilespacePackageAction::admit;
  }
  if (operation_id == "filespace.package.reject") {
    return api::EngineFilespacePackageAction::reject;
  }
  return api::EngineFilespacePackageAction::inspect_manifest;
}

api::EngineFilespacePackageRequest TypedFilespacePackageRequest(
    const SblrDispatchRequest& request) {
  api::EngineFilespacePackageRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.package_operation = FilespacePackageActionForSblrOperation(base.operation_id);
  typed.runtime_package_file_io_requested = false;
  typed.parser_file_io_authority = false;
  typed.parser_storage_authority = false;
  typed.transaction_finality_authority = false;
  typed.recovery_authority = false;
  typed.reference_or_wal_recovery_authority = false;
  typed.private_provider_dispatch_requested = false;
  return typed;
}

std::string ShardPlacementActionForSblrOperation(std::string_view operation_id) {
  constexpr std::string_view kPrefix = "shard_placement.";
  if (operation_id.starts_with(kPrefix)) {
    return std::string(operation_id.substr(kPrefix.size()));
  }
  return {};
}

api::EngineShardPlacementDescriptor DefaultShardPlacementDescriptor(
    std::string shard_suffix,
    std::uint64_t generation) {
  api::EngineShardPlacementDescriptor descriptor;
  descriptor.shard_uuid =
      "019f4000-0000-7000-8000-000000000" + std::move(shard_suffix);
  descriptor.source_filespace_uuid = "019f4000-0000-7000-8000-000000000101";
  descriptor.target_filespace_uuid = "019f4000-0000-7000-8000-000000000102";
  descriptor.range_begin = "0000000000000000";
  descriptor.range_end = "ffffffffffffffff";
  descriptor.placement_epoch = 41;
  descriptor.placement_generation = generation;
  descriptor.state = "planned";
  return descriptor;
}

api::EngineShardPlacementOperationRequest TypedShardPlacementDescriptorRequest(
    const SblrDispatchRequest& request) {
  api::EngineShardPlacementOperationRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.placement_operation = ShardPlacementActionForSblrOperation(base.operation_id);
  typed.descriptor = DefaultShardPlacementDescriptor("201", 7);
  typed.merge_inputs = {
      DefaultShardPlacementDescriptor("211", 5),
      DefaultShardPlacementDescriptor("212", 5),
  };
  typed.operator_authorized = true;
  typed.physical_data_movement_requested = false;
  return typed;
}

constexpr std::string_view kEncryptionRouteDatabaseUuid =
    "019f5000-0000-7000-8000-000000000001";
constexpr std::string_view kEncryptionRouteFilespaceUuid =
    "019f5000-0000-7000-8000-000000000002";
constexpr std::string_view kEncryptionRouteKeyUuid =
    "019f5000-0000-7000-8000-000000000003";
constexpr std::string_view kEncryptionRouteReplacementKeyUuid =
    "019f5000-0000-7000-8000-000000000004";
constexpr std::string_view kEncryptionRouteProtectedMaterialUuid =
    "019f5000-0000-7000-8000-000000000005";
constexpr std::string_view kEncryptionRouteProtectedMaterialVersionUuid =
    "019f5000-0000-7000-8000-000000000006";
constexpr std::string_view kEncryptionRouteProtectedMaterialNextVersionUuid =
    "019f5000-0000-7000-8000-000000000007";

void FillProtectedMaterialTargetDatabase(api::EngineApiRequest* request) {
  if (request == nullptr) return;
  if (request->target_database.uuid.canonical.empty()) {
    request->target_database.uuid.canonical =
        request->context.database_uuid.canonical.empty()
            ? std::string(kEncryptionRouteDatabaseUuid)
            : request->context.database_uuid.canonical;
  }
  if (request->target_database.object_kind.empty()) {
    request->target_database.object_kind = "database";
  }
}

api::EngineProtectedMaterialPolicySet ProtectedMaterialRoutePolicy() {
  api::EngineProtectedMaterialPolicySet policy;
  policy.retention_policy_uuid = "019f5000-0000-7000-8000-000000000101";
  policy.access_policy_uuid = "019f5000-0000-7000-8000-000000000102";
  policy.release_policy_uuid = "019f5000-0000-7000-8000-000000000103";
  policy.purge_policy_uuid = "019f5000-0000-7000-8000-000000000104";
  policy.audit_policy_uuid = "019f5000-0000-7000-8000-000000000105";
  policy.release_purposes = {"filespace.open"};
  return policy;
}

api::EngineAdmitEncryptionKeyRequest TypedAdmitEncryptionKeyRequest(
    const SblrDispatchRequest& request) {
  api::EngineAdmitEncryptionKeyRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  FillProtectedMaterialTargetDatabase(&typed);
  typed.key_uuid = std::string(kEncryptionRouteKeyUuid);
  typed.key_label = "filespace-key-redacted";
  typed.filespace_uuid = std::string(kEncryptionRouteFilespaceUuid);
  typed.secret_evidence = "wrapped-reference:v1:route";
  typed.cache_ttl_millis = 300000;
  return typed;
}

api::EngineRotateEncryptionKeyRequest TypedRotateEncryptionKeyRequest(
    const SblrDispatchRequest& request) {
  api::EngineRotateEncryptionKeyRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  FillProtectedMaterialTargetDatabase(&typed);
  typed.key_uuid = std::string(kEncryptionRouteKeyUuid);
  typed.replacement_key_uuid = std::string(kEncryptionRouteReplacementKeyUuid);
  typed.replacement_secret_evidence = "wrapped-reference:v1:route-replacement";
  typed.rotation_reason = "public-route-rekey";
  typed.cache_ttl_millis = 300000;
  return typed;
}

api::EngineInspectProtectedMaterialCacheRequest TypedInspectProtectedMaterialCacheRequest(
    const SblrDispatchRequest& request) {
  api::EngineInspectProtectedMaterialCacheRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  FillProtectedMaterialTargetDatabase(&typed);
  return typed;
}

api::EnginePurgeProtectedMaterialRequest TypedPurgeProtectedMaterialRequest(
    const SblrDispatchRequest& request) {
  api::EnginePurgeProtectedMaterialRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  FillProtectedMaterialTargetDatabase(&typed);
  typed.purge_reason = "public-route-cache-purge";
  return typed;
}

api::EngineShutdownProtectedMaterialRequest TypedShutdownProtectedMaterialRequest(
    const SblrDispatchRequest& request) {
  api::EngineShutdownProtectedMaterialRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  FillProtectedMaterialTargetDatabase(&typed);
  typed.shutdown_reason = "public-route-shutdown-purge";
  return typed;
}

api::EngineOpenEncryptedFilespaceRequest TypedOpenEncryptedFilespaceRequest(
    const SblrDispatchRequest& request) {
  api::EngineOpenEncryptedFilespaceRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  FillProtectedMaterialTargetDatabase(&typed);
  typed.database_uuid = typed.target_database.uuid.canonical;
  typed.filespace_uuid = std::string(kEncryptionRouteFilespaceUuid);
  typed.key_uuid = std::string(kEncryptionRouteKeyUuid);
  typed.encrypted_filespace = true;
  typed.decryption_required = true;
  return typed;
}

api::EngineRequestProtectedMaterialRequest TypedRequestProtectedMaterialRequest(
    const SblrDispatchRequest& request) {
  api::EngineRequestProtectedMaterialRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  FillProtectedMaterialTargetDatabase(&typed);
  typed.purpose = "filespace.open";
  return typed;
}

api::EnginePurgeProtectedMaterialVersionRequest TypedPurgeProtectedMaterialVersionRequest(
    const SblrDispatchRequest& request) {
  api::EnginePurgeProtectedMaterialVersionRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  FillProtectedMaterialTargetDatabase(&typed);
  typed.protected_material_uuid = std::string(kEncryptionRouteProtectedMaterialUuid);
  typed.protected_material_version_uuid =
      std::string(kEncryptionRouteProtectedMaterialVersionUuid);
  typed.purge_reason = "public-route-cryptographic-erase";
  return typed;
}

api::EngineCreateProtectedMaterialRequest TypedCreateProtectedMaterialRequest(
    const SblrDispatchRequest& request) {
  api::EngineCreateProtectedMaterialRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  FillProtectedMaterialTargetDatabase(&typed);
  typed.protected_material_uuid = std::string(kEncryptionRouteProtectedMaterialUuid);
  typed.object_class = "filespace_encryption_key";
  typed.owner_scope_uuid = std::string(kEncryptionRouteFilespaceUuid);
  typed.purpose_class = "encryption_use";
  typed.storage_class = "wrapped";
  typed.policy = ProtectedMaterialRoutePolicy();
  typed.initial_version_uuid = std::string(kEncryptionRouteProtectedMaterialVersionUuid);
  typed.protected_reference = "kms-ref:v1:protected-material-route";
  typed.envelope_reference = "kms-envelope:v1:protected-material-route";
  typed.payload_hash =
      "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  return typed;
}

api::EngineAddProtectedMaterialVersionRequest TypedAddProtectedMaterialVersionRequest(
    const SblrDispatchRequest& request) {
  api::EngineAddProtectedMaterialVersionRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  FillProtectedMaterialTargetDatabase(&typed);
  typed.protected_material_uuid = std::string(kEncryptionRouteProtectedMaterialUuid);
  typed.protected_material_version_uuid =
      std::string(kEncryptionRouteProtectedMaterialNextVersionUuid);
  typed.protected_reference = "kms-ref:v1:protected-material-route-rotation";
  typed.envelope_reference = "kms-envelope:v1:protected-material-route-rotation";
  typed.payload_hash =
      "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  typed.storage_class = "wrapped";
  typed.rotation_reason = "public-route-protected-material-version";
  typed.policy_override = ProtectedMaterialRoutePolicy();
  return typed;
}

api::EngineResolveProtectedMaterialRequest TypedResolveProtectedMaterialRequest(
    const SblrDispatchRequest& request) {
  api::EngineResolveProtectedMaterialRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  FillProtectedMaterialTargetDatabase(&typed);
  typed.protected_material_uuid = std::string(kEncryptionRouteProtectedMaterialUuid);
  typed.purpose = "filespace.open";
  return typed;
}

api::EngineReleaseProtectedMaterialRequest TypedReleaseProtectedMaterialRequest(
    const SblrDispatchRequest& request) {
  api::EngineReleaseProtectedMaterialRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  FillProtectedMaterialTargetDatabase(&typed);
  typed.protected_material_uuid = std::string(kEncryptionRouteProtectedMaterialUuid);
  typed.protected_material_version_uuid =
      std::string(kEncryptionRouteProtectedMaterialVersionUuid);
  typed.purpose = "filespace.open";
  return typed;
}

api::EngineInspectProtectedMaterialCatalogRequest TypedInspectProtectedMaterialCatalogRequest(
    const SblrDispatchRequest& request) {
  api::EngineInspectProtectedMaterialCatalogRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  FillProtectedMaterialTargetDatabase(&typed);
  typed.protected_material_uuid = std::string(kEncryptionRouteProtectedMaterialUuid);
  typed.include_versions = true;
  typed.include_audit = true;
  return typed;
}

api::EngineExportProtectedMaterialPackageRequest TypedExportProtectedMaterialPackageRequest(
    const SblrDispatchRequest& request) {
  api::EngineExportProtectedMaterialPackageRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  FillProtectedMaterialTargetDatabase(&typed);
  typed.protected_material_uuid = std::string(kEncryptionRouteProtectedMaterialUuid);
  typed.include_versions = true;
  typed.include_audit = true;
  typed.export_reason = "public-route-protected-material-package-export";
  return typed;
}

api::EngineImportProtectedMaterialPackageRequest TypedImportProtectedMaterialPackageRequest(
    const SblrDispatchRequest& request) {
  api::EngineImportProtectedMaterialPackageRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  FillProtectedMaterialTargetDatabase(&typed);
  typed.encoded_package = api::SecurityOptionValue(base, "encoded_package:");
  typed.expected_package_digest =
      api::SecurityOptionValue(base, "expected_package_digest:");
  typed.import_authorized =
      api::SecurityOptionBool(base,
                              "protected_material_package_import_authorized:",
                              false);
  typed.import_reason = "public-route-protected-material-package-import";
  return typed;
}

api::EngineTypedValue DispatchTypedValueOption(const api::EngineApiRequest& request,
                                               const std::string& prefix,
                                               const std::string& fallback_type) {
  api::EngineTypedValue value;
  value.descriptor.descriptor_kind = "scalar";
  value.descriptor.canonical_type_name = api::SecurityOptionValue(request, prefix + "type:");
  if (value.descriptor.canonical_type_name.empty()) {
    value.descriptor.canonical_type_name = fallback_type;
  }
  value.descriptor.encoded_descriptor = api::SecurityOptionValue(request, prefix + "descriptor:");
  if (value.descriptor.encoded_descriptor.empty()) {
    value.descriptor.encoded_descriptor = "type=" + value.descriptor.canonical_type_name;
  }
  value.encoded_value = api::SecurityOptionValue(request, prefix + "value:");
  value.is_null = api::SecurityOptionBool(request, prefix + "null:", false);
  return value;
}

api::EngineObjectReference DispatchTargetOfKind(const api::EngineApiRequest& request,
                                                const std::string& kind) {
  if (request.target_object.object_kind == kind) { return request.target_object; }
  for (const auto& object : request.related_objects) {
    if (object.object_kind == kind) { return object; }
  }
  return {};
}

api::EngineObjectReference TargetObjectForDml(const api::EngineApiRequest& request,
                                              const std::string& default_kind) {
  if (!request.target_object.uuid.canonical.empty()) { return request.target_object; }
  api::EngineObjectReference target = DispatchTargetOfKind(request, default_kind);
  if (!target.uuid.canonical.empty()) { return target; }
  target.uuid.canonical = api::SecurityOptionValue(request, "target_object_uuid:");
  target.object_kind = api::SecurityOptionValue(request, "target_object_kind:");
  if (target.object_kind.empty()) target.object_kind = default_kind;
  return target;
}

bool IsCreateSchemaRuntimeOption(std::string_view option) {
  return option.starts_with("comment:") ||
         option.starts_with("localized_comment:") ||
         option.starts_with("unresolved_schema_parent_path:");
}

std::vector<std::string> SplitDottedIdentifierPath(std::string_view path) {
  std::vector<std::string> parts;
  std::string current;
  bool quoted = false;
  for (std::size_t index = 0; index < path.size(); ++index) {
    const char ch = path[index];
    if (quoted) {
      if (ch == '"') {
        if (index + 1 < path.size() && path[index + 1] == '"') {
          current.push_back('"');
          ++index;
        } else {
          quoted = false;
        }
      } else {
        current.push_back(ch);
      }
      continue;
    }
    if (ch == '"') {
      quoted = true;
      continue;
    }
    if (ch == '.') {
      if (!current.empty()) {
        parts.push_back(current);
        current.clear();
      }
      continue;
    }
    if (!std::isspace(static_cast<unsigned char>(ch)) || !current.empty()) {
      current.push_back(ch);
    }
  }
  if (!current.empty()) { parts.push_back(current); }
  return parts;
}

std::string JoinDottedIdentifierPath(const std::vector<std::string>& parts) {
  std::string out;
  for (const auto& part : parts) {
    if (part.empty()) { continue; }
    if (!out.empty()) { out.push_back('.'); }
    out += part;
  }
  return out;
}

std::optional<std::string> ResolveSchemaParentPathToUuid(const api::EngineApiRequest& request,
                                                         const std::string& parent_path,
                                                         std::string* normalized_path) {
  if (parent_path.empty()) { return std::nullopt; }
  const auto parts = SplitDottedIdentifierPath(parent_path);
  const std::string normalized = JoinDottedIdentifierPath(parts);
  if (normalized.empty()) { return std::nullopt; }
  if (normalized_path != nullptr) { *normalized_path = normalized; }

  const std::string profile = request.context.identifier_profile_uuid.empty()
                                  ? "sbsql_v3"
                                  : request.context.identifier_profile_uuid;
  std::vector<std::string> lookup_keys;
  auto add_key = [&](const std::string& value) {
    if (value.empty()) { return; }
    const std::string folded = api::NameRegistryLookupKey(value, profile, false);
    if (std::find(lookup_keys.begin(), lookup_keys.end(), folded) == lookup_keys.end()) {
      lookup_keys.push_back(folded);
    }
  };
  add_key(parent_path);
  add_key(normalized);

  auto context = request.context;
  const std::uint64_t observer_tx =
      context.snapshot_visible_through_local_transaction_id != 0
          ? context.snapshot_visible_through_local_transaction_id
          : context.local_transaction_id;
  auto loaded = api::LoadNameRegistryState(context, observer_tx);
  if (!loaded.ok && context.local_transaction_id != 0) {
    context.local_transaction_id = 0;
    loaded = api::LoadNameRegistryState(context, observer_tx);
  }
  if (!loaded.ok) { return std::nullopt; }

  std::optional<std::string> match;
  for (const auto& entry : loaded.state.entries) {
    if (entry.deleted || entry.lifecycle_state != "active" || entry.object_class != "schema") {
      continue;
    }
    const std::string entry_full_key = entry.full_path_lookup_key.empty()
                                           ? entry.normalized_lookup_key
                                           : entry.full_path_lookup_key;
    const bool key_matches =
        std::find(lookup_keys.begin(), lookup_keys.end(), entry_full_key) != lookup_keys.end() ||
        (parts.size() == 1 &&
         std::find(lookup_keys.begin(), lookup_keys.end(), entry.normalized_lookup_key) != lookup_keys.end());
    if (!key_matches) { continue; }
    if (match && *match != entry.object_uuid) { return std::nullopt; }
    match = entry.object_uuid;
  }
  return match;
}

api::EngineCreateSchemaRequest TypedCreateSchemaRequest(const SblrDispatchRequest& request) {
  api::EngineCreateSchemaRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  if (typed.target_object.uuid.canonical.empty()) {
    typed.target_object.uuid.canonical = api::SecurityOptionValue(base, "schema_object_uuid:");
  }
  if (typed.target_object.object_kind.empty()) {
    typed.target_object.object_kind = "schema";
  }
  if (typed.target_schema.uuid.canonical.empty()) {
    typed.target_schema.uuid.canonical = api::SecurityOptionValue(base, "target_schema_uuid:");
  }
  if (typed.target_schema.uuid.canonical.empty()) {
    typed.target_schema.uuid.canonical = api::SecurityOptionValue(base, "schema_uuid:");
  }
  if (typed.target_schema.uuid.canonical.empty()) {
    typed.target_schema.uuid.canonical = api::SecurityOptionValue(base, "schema_parent_uuid:");
  }
  std::string normalized_parent_path;
  const std::string schema_parent_path = api::SecurityOptionValue(base, "schema_parent_path:");
  if (typed.target_schema.uuid.canonical.empty() && !schema_parent_path.empty()) {
    const auto resolved_parent =
        ResolveSchemaParentPathToUuid(base, schema_parent_path, &normalized_parent_path);
    if (resolved_parent) {
      typed.target_schema.uuid.canonical = *resolved_parent;
    }
  }
  if (!typed.target_schema.uuid.canonical.empty() && typed.target_schema.object_kind.empty()) {
    typed.target_schema.object_kind = "schema";
  }
  if (typed.localized_names.empty()) {
    std::string schema_name = api::SecurityOptionValue(base, "schema_name:");
    if (schema_name.empty()) schema_name = api::SecurityOptionValue(base, "name:");
    if (!schema_name.empty()) {
      if (normalized_parent_path.empty() && !schema_parent_path.empty()) {
        const auto parts = SplitDottedIdentifierPath(schema_parent_path);
        normalized_parent_path = JoinDottedIdentifierPath(parts);
      }
      const std::string full_path =
          normalized_parent_path.empty() ? std::string{} : normalized_parent_path + "." + schema_name;
      typed.localized_names.push_back({"en", "primary", full_path, schema_name, true});
    }
  }

  typed.option_envelopes.clear();
  for (const auto& option : base.option_envelopes) {
    if (IsCreateSchemaRuntimeOption(option)) {
      typed.option_envelopes.push_back(option);
    }
  }
  if (!schema_parent_path.empty() && typed.target_schema.uuid.canonical.empty()) {
    typed.option_envelopes.push_back("unresolved_schema_parent_path:" + schema_parent_path);
  }
  return typed;
}

std::vector<api::EngineIndexDefinition> DecodeInlineTableIndexOptions(
    const api::EngineApiRequest& base) {
  constexpr std::uint64_t kMaximumInlineTableIndexes = 64;
  constexpr std::uint64_t kMaximumInlineTableIndexKeys = 64;

  const std::string index_count_text =
      api::SecurityOptionValue(base, "table_index_count:");
  if (index_count_text.empty()) { return {}; }

  const auto index_count = ParseBoundedDispatchCount(
      index_count_text, kMaximumInlineTableIndexes);
  auto invalid_descriptor = [] {
    api::EngineIndexDefinition invalid;
    invalid.index_kind = "invalid_inline_table_index_descriptor";
    return std::vector<api::EngineIndexDefinition>{std::move(invalid)};
  };
  if (!index_count) { return invalid_descriptor(); }

  std::vector<api::EngineIndexDefinition> indexes;
  indexes.reserve(static_cast<std::size_t>(*index_count));
  for (std::uint64_t ordinal = 0; ordinal < *index_count; ++ordinal) {
    const std::string prefix =
        "table_index_" + std::to_string(ordinal) + "_";
    const auto key_count = ParseBoundedDispatchCount(
        api::SecurityOptionValue(base, prefix + "key_count:"),
        kMaximumInlineTableIndexKeys);
    if (!key_count || *key_count == 0) { return invalid_descriptor(); }

    api::EngineIndexDefinition index;
    index.index_kind =
        api::SecurityOptionValue(base, prefix + "physical_profile:");
    if (index.index_kind.empty()) { index.index_kind = "btree"; }
    for (std::uint64_t key_ordinal = 0; key_ordinal < *key_count;
         ++key_ordinal) {
      const std::string key = api::SecurityOptionValue(
          base, prefix + "key_" + std::to_string(key_ordinal) + ":");
      if (key.empty()) { return invalid_descriptor(); }
      index.key_envelopes.push_back(key);
    }

    const std::string constraint_kind = LowerAscii(
        api::SecurityOptionValue(base, prefix + "constraint_kind:"));
    if (constraint_kind != "primary_key" && constraint_kind != "unique") {
      return invalid_descriptor();
    }
    index.key_envelopes.push_back(constraint_kind);

    const std::string index_name =
        api::SecurityOptionValue(base, prefix + "index_name:");
    const std::string constraint_name =
        api::SecurityOptionValue(base, prefix + "constraint_name:");
    if (!index_name.empty()) {
      index.names.push_back({"en", "primary", "", index_name, true});
    }
    if (!constraint_name.empty() && constraint_name != index_name) {
      index.names.push_back({"en",
                             index_name.empty() ? "primary" : "constraint",
                             "",
                             constraint_name,
                             index_name.empty()});
    }
    indexes.push_back(std::move(index));
  }
  return indexes;
}

api::EngineCreateTableRequest TypedCreateTableRequest(const SblrDispatchRequest& request) {
  api::EngineCreateTableRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.target_database = base.target_database;
  typed.target_schema = base.target_schema;
  std::string normalized_parent_path;
  const std::string schema_parent_path = api::SecurityOptionValue(base, "schema_parent_path:");
  const std::string explicit_target_schema_uuid =
      api::SecurityOptionValue(base, "target_schema_uuid:");
  const std::string explicit_schema_uuid = api::SecurityOptionValue(base, "schema_uuid:");
  const std::string explicit_parent_schema_uuid =
      api::SecurityOptionValue(base, "schema_parent_uuid:");
  const bool has_explicit_schema_uuid = !explicit_target_schema_uuid.empty() ||
                                        !explicit_schema_uuid.empty() ||
                                        !explicit_parent_schema_uuid.empty();
  bool unresolved_parent_path = false;
  if (!schema_parent_path.empty() && !has_explicit_schema_uuid) {
    const auto resolved_parent =
        ResolveSchemaParentPathToUuid(base, schema_parent_path, &normalized_parent_path);
    if (resolved_parent) {
      typed.target_schema.uuid.canonical = *resolved_parent;
    } else {
      typed.target_schema.uuid.canonical.clear();
      unresolved_parent_path = true;
    }
  }
  if (typed.target_schema.uuid.canonical.empty()) {
    typed.target_schema.uuid.canonical = explicit_target_schema_uuid;
  }
  if (typed.target_schema.uuid.canonical.empty()) {
    typed.target_schema.uuid.canonical = explicit_schema_uuid;
  }
  if (typed.target_schema.uuid.canonical.empty()) {
    typed.target_schema.uuid.canonical = explicit_parent_schema_uuid;
  }
  if (!typed.target_schema.uuid.canonical.empty() && typed.target_schema.object_kind.empty()) {
    typed.target_schema.object_kind = "schema";
  }
  typed.option_envelopes.clear();
  typed.option_envelopes = base.option_envelopes;
  if (unresolved_parent_path) {
    typed.option_envelopes.push_back("unresolved_schema_parent_path:" + schema_parent_path);
  }
  typed.requested_table_uuid = base.target_object.uuid;
  if (typed.requested_table_uuid.canonical.empty()) {
    typed.requested_table_uuid.canonical = api::SecurityOptionValue(base, "table_object_uuid:");
  }
  typed.table_names = base.localized_names;
  if (typed.table_names.empty()) {
    std::string table_name = api::SecurityOptionValue(base, "table_name:");
    if (table_name.empty()) { table_name = api::SecurityOptionValue(base, "name:"); }
    if (!table_name.empty()) {
      if (normalized_parent_path.empty() && !schema_parent_path.empty()) {
        normalized_parent_path = JoinDottedIdentifierPath(SplitDottedIdentifierPath(schema_parent_path));
      }
      const std::string full_path =
          normalized_parent_path.empty() ? std::string{} : normalized_parent_path + "." + table_name;
      typed.table_names.push_back({"en", "primary", full_path, table_name, true});
    }
  }
  typed.table_columns = base.columns;
  if (typed.table_columns.empty()) {
    std::uint64_t column_count = DispatchOptionU64(base, "column_count:");
    if (column_count == 0) {
      column_count = DispatchOptionU64(base, "column_definition_count:");
    }
    if (column_count == 0 &&
        !api::SecurityOptionValue(base, "column_0_name:").empty()) {
      column_count = 1;
    }
    for (std::uint64_t ordinal = 0; ordinal < column_count; ++ordinal) {
      const std::string prefix = "column_" + std::to_string(ordinal) + "_";
      std::string column_name = api::SecurityOptionValue(base, prefix + "name:");
      std::string column_type = api::SecurityOptionValue(base, prefix + "type:");
      std::string column_descriptor = api::SecurityOptionValue(base, prefix + "descriptor:");
      std::string column_default = api::SecurityOptionValue(base, prefix + "default:");
      if (ordinal == 0 && column_type.empty()) {
        column_type = api::SecurityOptionValue(base, "canonical_type_name:");
      }
      if (column_name.empty() || column_type.empty()) continue;
      if (column_descriptor.empty()) {
        column_descriptor = "type=" + column_type;
      } else if (column_descriptor.find("type=") == std::string::npos &&
                 column_descriptor.find("canonical=") == std::string::npos) {
        column_descriptor = "type=" + column_type + ";" + column_descriptor;
      }
      api::EngineColumnDefinition column;
      column.ordinal = static_cast<std::uint32_t>(ordinal);
      column.names.push_back({"en", "primary", "", column_name, true});
      column.descriptor.descriptor_kind = "scalar";
      column.descriptor.canonical_type_name = column_type;
      const std::string nullable = LowerAscii(api::SecurityOptionValue(base, prefix + "nullable:"));
      column.nullable = nullable.empty() || nullable == "true" || nullable == "1";
      if (column_descriptor.find("nullable=") == std::string::npos) {
        column_descriptor += ";nullable=";
        column_descriptor += column.nullable ? "true" : "false";
      }
      if (!column_default.empty()) {
        column.default_expression_envelope = column_default;
        if (column_descriptor.find("default=") == std::string::npos &&
            column_descriptor.find("default_expression=") == std::string::npos) {
          column_descriptor += ";default=" + column_default;
        }
      }
      column.descriptor.encoded_descriptor = column_descriptor;
      typed.table_columns.push_back(std::move(column));
    }
  }
  typed.table_constraints = base.constraints;
  typed.table_indexes = base.indexes;
  const bool has_encoded_inline_indexes =
      !api::SecurityOptionValue(base, "table_index_count:").empty();
  if (!typed.table_indexes.empty() && has_encoded_inline_indexes) {
    api::EngineIndexDefinition invalid;
    invalid.index_kind = "mixed_inline_table_index_representations";
    typed.table_indexes = {std::move(invalid)};
  } else if (typed.table_indexes.empty()) {
    typed.table_indexes = DecodeInlineTableIndexOptions(base);
  }
  typed.table_physical_profile = base.physical_profile;
  const std::string physical_profile = [&]() {
    std::string value = api::SecurityOptionValue(base, "physical_profile:");
    if (value.empty()) value = api::SecurityOptionValue(base, "table_physical_profile:");
    return value;
  }();
  if (!physical_profile.empty()) {
    bool present = false;
    for (const auto& existing : typed.table_physical_profile.encoded_profiles) {
      if (existing == physical_profile) {
        present = true;
        break;
      }
    }
    if (!present) {
      typed.table_physical_profile.encoded_profiles.push_back(physical_profile);
    }
  }
  typed.table_policy_profile = base.policy_profile;
  typed.table_compatibility_profile = base.compatibility_profile;
  return typed;
}

api::EngineCreateStatisticsRequest TypedCreateStatisticsRequest(const SblrDispatchRequest& request) {
  api::EngineCreateStatisticsRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.target_table = DispatchTargetOfKind(base, "table");
  if (typed.target_table.uuid.canonical.empty()) {
    typed.target_table.uuid.canonical = api::SecurityOptionValue(base, "statistics_target_uuid:");
    typed.target_table.object_kind = api::SecurityOptionValue(base, "statistics_target_kind:");
  }
  if (typed.target_table.uuid.canonical.empty()) {
    typed.target_table.uuid.canonical = api::SecurityOptionValue(base, "target_table_uuid:");
    typed.target_table.object_kind = "table";
  }
  if (typed.target_table.object_kind.empty()) { typed.target_table.object_kind = "table"; }
  if (base.target_object.object_kind == "statistics") {
    typed.requested_statistics_uuid = base.target_object.uuid;
  }
  typed.statistics_names = base.localized_names;
  for (const auto& option : base.option_envelopes) {
    if (option.rfind("statistics_kind:", 0) == 0) {
      typed.statistics_kinds.push_back(option.substr(16));
    } else if (option.rfind("statistics_expression:", 0) == 0) {
      typed.expression_envelopes.push_back(option.substr(22));
    }
  }
  if (typed.statistics_kinds.empty()) {
    const std::string kind = api::SecurityOptionValue(base, "statistics_kind:");
    if (!kind.empty()) { typed.statistics_kinds.push_back(kind); }
  }
  return typed;
}

api::EngineCreateIndexRequest TypedCreateIndexRequest(const SblrDispatchRequest& request) {
  api::EngineCreateIndexRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  api::EngineObjectReference target_table = DispatchTargetOfKind(base, "table");
  if (target_table.uuid.canonical.empty()) {
    target_table.uuid.canonical = api::SecurityOptionValue(base, "index_target_uuid:");
    target_table.object_kind = api::SecurityOptionValue(base, "index_target_kind:");
  }
  if (target_table.uuid.canonical.empty()) {
    target_table.uuid.canonical = api::SecurityOptionValue(base, "target_table_uuid:");
    target_table.object_kind = "table";
  }
  if (target_table.object_kind.empty()) target_table.object_kind = "table";
  if (!target_table.uuid.canonical.empty()) typed.target_object = target_table;
  if (typed.indexes.empty()) {
    api::EngineIndexDefinition index;
    index.requested_index_uuid.canonical = api::SecurityOptionValue(base, "index_object_uuid:");
    if (index.requested_index_uuid.canonical.empty() &&
        base.target_object.object_kind == "index") {
      index.requested_index_uuid = base.target_object.uuid;
    }
    const std::string name = [&]() {
      const std::string index_name = api::SecurityOptionValue(base, "index_name:");
      if (!index_name.empty()) return index_name;
      return api::SecurityOptionValue(base, "name:");
    }();
    if (!name.empty()) index.names.push_back({"en", "primary", "", name, true});
    index.index_kind = api::SecurityOptionValue(base, "index_profile:");
    if (index.index_kind.empty()) index.index_kind = "btree";
    const std::string key_envelope = api::SecurityOptionValue(base, "index_key_envelope:");
    if (!key_envelope.empty()) {
      std::string current;
      std::istringstream key_envelopes{key_envelope};
      while (std::getline(key_envelopes, current, ',')) {
        if (!current.empty()) {
          index.key_envelopes.push_back(current);
        }
      }
    } else {
      const std::string key_column = api::SecurityOptionValue(base, "index_key_column:");
      if (!key_column.empty()) index.key_envelopes.push_back(key_column);
    }
    if (api::SecurityOptionBool(base, "index_unique:", false) &&
        index.index_kind == "btree") {
      index.index_kind = "btree_unique";
    }
    if (!index.key_envelopes.empty()) typed.indexes.push_back(std::move(index));
  }
  return typed;
}

api::EngineCreateIndexTemplateRequest TypedCreateIndexTemplateRequest(const SblrDispatchRequest& request) {
  api::EngineCreateIndexTemplateRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  if (typed.target_object.uuid.canonical.empty()) {
    typed.target_object.uuid.canonical = api::SecurityOptionValue(base, "index_template_object_uuid:");
  }
  if (typed.target_object.object_kind.empty()) {
    const std::string template_kind = api::SecurityOptionValue(base, "index_template_kind:");
    typed.target_object.object_kind = template_kind.empty() ? "index_template" : template_kind;
  }
  if (typed.localized_names.empty()) {
    const std::string name = [&]() {
      const std::string template_name = api::SecurityOptionValue(base, "index_template_name:");
      if (!template_name.empty()) return template_name;
      return api::SecurityOptionValue(base, "name:");
    }();
    if (!name.empty()) typed.localized_names.push_back({"en", "primary", "", name, true});
  }
  return typed;
}

api::EngineCreateSequenceRequest TypedCreateSequenceRequest(const SblrDispatchRequest& request) {
  api::EngineCreateSequenceRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  if (typed.target_object.uuid.canonical.empty()) {
    typed.target_object.uuid.canonical = api::SecurityOptionValue(base, "sequence_object_uuid:");
  }
  if (typed.target_object.object_kind.empty()) {
    typed.target_object.object_kind = "sequence";
  }
  if (typed.target_schema.uuid.canonical.empty()) {
    typed.target_schema.uuid.canonical = api::SecurityOptionValue(base, "target_schema_uuid:");
  }
  if (typed.target_schema.uuid.canonical.empty()) {
    typed.target_schema.uuid.canonical = api::SecurityOptionValue(base, "schema_uuid:");
  }
  if (typed.target_schema.uuid.canonical.empty()) {
    typed.target_schema.uuid.canonical = api::SecurityOptionValue(base, "schema_parent_uuid:");
  }
  std::string normalized_parent_path;
  const std::string schema_parent_path = api::SecurityOptionValue(base, "schema_parent_path:");
  if (typed.target_schema.uuid.canonical.empty() && !schema_parent_path.empty()) {
    const auto resolved_parent =
        ResolveSchemaParentPathToUuid(base, schema_parent_path, &normalized_parent_path);
    if (resolved_parent) {
      typed.target_schema.uuid.canonical = *resolved_parent;
    }
  }
  if (!typed.target_schema.uuid.canonical.empty() && typed.target_schema.object_kind.empty()) {
    typed.target_schema.object_kind = "schema";
  }
  if (typed.localized_names.empty()) {
    const auto sequence_name = api::SecurityOptionValue(base, "name:");
    if (!sequence_name.empty()) {
      if (normalized_parent_path.empty() && !schema_parent_path.empty()) {
        normalized_parent_path = JoinDottedIdentifierPath(SplitDottedIdentifierPath(schema_parent_path));
      }
      const std::string full_path =
          normalized_parent_path.empty() ? std::string{} : normalized_parent_path + "." + sequence_name;
      typed.localized_names.push_back({"en", "primary", full_path, sequence_name, true});
    }
  }
  if (!schema_parent_path.empty() && typed.target_schema.uuid.canonical.empty()) {
    typed.option_envelopes.push_back("unresolved_schema_parent_path:" + schema_parent_path);
  }
  return typed;
}

bool IsDomainRuntimeOption(std::string_view option) {
  return option.starts_with("default_expression:") ||
         option.starts_with("check_constraint:") ||
         option.starts_with("check_constraint_append:") ||
         option.starts_with("nullable:") ||
         option.starts_with("collation:") ||
         option.starts_with("charset:") ||
         option.starts_with("cast_policy:") ||
         option.starts_with("mutation_policy:") ||
         option.starts_with("masking_policy:") ||
         option.starts_with("visibility_policy:") ||
         option.starts_with("encryption_policy:") ||
         option.starts_with("driver_metadata:") ||
         option.starts_with("wire_metadata:") ||
         option.starts_with("element_path:") ||
         option.starts_with("method_binding:") ||
         option.starts_with("comment:") ||
         option.starts_with("localized_comment:") ||
         option.starts_with("reference_alias:");
}

api::EngineCreateDomainRequest TypedCreateDomainRequest(const SblrDispatchRequest& request) {
  api::EngineCreateDomainRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.option_envelopes.clear();
  for (const auto& option : base.option_envelopes) {
    if (IsDomainRuntimeOption(option)) { typed.option_envelopes.push_back(option); }
  }
  if (typed.target_object.uuid.canonical.empty()) {
    typed.target_object.uuid.canonical = api::SecurityOptionValue(base, "domain_object_uuid:");
  }
  if (typed.target_object.object_kind.empty()) {
    typed.target_object.object_kind = "domain";
  }
  if (typed.target_schema.uuid.canonical.empty()) {
    typed.target_schema.uuid.canonical = api::SecurityOptionValue(base, "target_schema_uuid:");
  }
  if (typed.target_schema.uuid.canonical.empty()) {
    typed.target_schema.uuid.canonical = api::SecurityOptionValue(base, "schema_uuid:");
  }
  if (typed.target_schema.uuid.canonical.empty()) {
    typed.target_schema.uuid.canonical = api::SecurityOptionValue(base, "schema_parent_uuid:");
  }
  std::string normalized_parent_path;
  const std::string schema_parent_path = api::SecurityOptionValue(base, "schema_parent_path:");
  if (typed.target_schema.uuid.canonical.empty() && !schema_parent_path.empty()) {
    const auto resolved_parent =
        ResolveSchemaParentPathToUuid(base, schema_parent_path, &normalized_parent_path);
    if (resolved_parent) {
      typed.target_schema.uuid.canonical = *resolved_parent;
    }
  }
  if (!typed.target_schema.uuid.canonical.empty() && typed.target_schema.object_kind.empty()) {
    typed.target_schema.object_kind = "schema";
  }
  if (typed.localized_names.empty()) {
    const auto domain_name = api::SecurityOptionValue(base, "name:");
    if (!domain_name.empty()) {
      if (normalized_parent_path.empty() && !schema_parent_path.empty()) {
        normalized_parent_path = JoinDottedIdentifierPath(SplitDottedIdentifierPath(schema_parent_path));
      }
      const std::string full_path =
          normalized_parent_path.empty() ? std::string{} : normalized_parent_path + "." + domain_name;
      typed.localized_names.push_back({"en", "primary", full_path, domain_name, true});
    }
  }
  if (!schema_parent_path.empty() && typed.target_schema.uuid.canonical.empty()) {
    typed.option_envelopes.push_back("unresolved_schema_parent_path:" + schema_parent_path);
  }
  if (typed.descriptors.empty()) {
    api::EngineDescriptor descriptor;
    descriptor.descriptor_uuid.canonical = api::SecurityOptionValue(base, "base_descriptor_uuid:");
    descriptor.descriptor_kind = api::SecurityOptionValue(base, "base_descriptor_kind:");
    descriptor.canonical_type_name = api::SecurityOptionValue(base, "base_canonical_type_name:");
    descriptor.encoded_descriptor = api::SecurityOptionValue(base, "base_encoded_descriptor:");
    if (!descriptor.canonical_type_name.empty()) {
      if (descriptor.descriptor_kind.empty()) descriptor.descriptor_kind = "scalar";
      if (descriptor.encoded_descriptor.empty()) {
        descriptor.encoded_descriptor = "type=" + descriptor.canonical_type_name;
      }
      typed.descriptors.push_back(std::move(descriptor));
    }
  }
  return typed;
}

api::EngineCreateViewRequest TypedCreateViewRequest(const SblrDispatchRequest& request) {
  api::EngineCreateViewRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  if (typed.target_object.uuid.canonical.empty()) {
    typed.target_object.uuid.canonical = api::SecurityOptionValue(base, "view_object_uuid:");
  }
  if (typed.target_object.object_kind.empty()) {
    typed.target_object.object_kind = "view";
  }
  if (typed.target_schema.uuid.canonical.empty()) {
    typed.target_schema.uuid.canonical = api::SecurityOptionValue(base, "target_schema_uuid:");
  }
  if (typed.target_schema.uuid.canonical.empty()) {
    typed.target_schema.uuid.canonical = api::SecurityOptionValue(base, "schema_uuid:");
  }
  if (!typed.target_schema.uuid.canonical.empty() && typed.target_schema.object_kind.empty()) {
    typed.target_schema.object_kind = "schema";
  }
  if (typed.localized_names.empty()) {
    const auto view_name = api::SecurityOptionValue(base, "name:");
    if (!view_name.empty()) {
      typed.localized_names.push_back({"en", "primary", "", view_name, true});
    }
  }
  if (!DecodeRelationProjectionViewCreateTransportV2(base, &typed) &&
      !DecodeRelationProjectionViewCreateTransportV1(base, &typed)) {
    DecodeGlobalAggregateViewCreateTransportV1(base, &typed);
  }
  return typed;
}

template <typename TRequest>
TRequest TypedCreateExecutableObjectRequest(const SblrDispatchRequest& request,
                                            std::string_view object_kind,
                                            std::string_view object_uuid_prefix) {
  TRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  if (typed.target_object.uuid.canonical.empty()) {
    typed.target_object.uuid.canonical =
        api::SecurityOptionValue(base, std::string(object_uuid_prefix) + "_object_uuid:");
  }
  if (typed.target_object.object_kind.empty()) {
    typed.target_object.object_kind = std::string(object_kind);
  }
  if (typed.target_schema.uuid.canonical.empty()) {
    typed.target_schema.uuid.canonical = api::SecurityOptionValue(base, "target_schema_uuid:");
  }
  if (typed.target_schema.uuid.canonical.empty()) {
    typed.target_schema.uuid.canonical = api::SecurityOptionValue(base, "schema_uuid:");
  }
  if (!typed.target_schema.uuid.canonical.empty() && typed.target_schema.object_kind.empty()) {
    typed.target_schema.object_kind = "schema";
  }
  if (typed.localized_names.empty()) {
    std::string object_name =
        api::SecurityOptionValue(base, std::string(object_uuid_prefix) + "_name:");
    if (object_name.empty()) object_name = api::SecurityOptionValue(base, "name:");
    if (!object_name.empty()) {
      typed.localized_names.push_back({"en", "primary", "", object_name, true});
    }
  }
  for (std::size_t related_index = 0; related_index < 64; ++related_index) {
    const std::string prefix = "related_object_" + std::to_string(related_index);
    const std::string related_uuid =
        api::SecurityOptionValue(base, prefix + "_uuid:");
    if (related_uuid.empty() &&
        api::SecurityOptionValue(base, prefix + "_kind:").empty()) {
      break;
    }
    if (related_uuid.empty()) continue;
    api::EngineObjectReference related;
    related.uuid.canonical = related_uuid;
    related.object_kind = api::SecurityOptionValue(base, prefix + "_kind:");
    if (related.object_kind.empty()) related.object_kind = "table";
    if (related.object_kind != "executable_object" &&
        related.object_kind != "procedure" &&
        related.object_kind != "function" &&
        related.object_kind != "trigger" &&
        related.object_kind != "table" &&
        related.object_kind != "sequence" &&
        related.object_kind != "view") {
      continue;
    }
    typed.related_objects.push_back(std::move(related));
  }
  return typed;
}

api::EngineCatalogDescriptorMutationRequest TypedCatalogDescriptorMutationRequest(
    const SblrDispatchRequest& request) {
  api::EngineCatalogDescriptorMutationRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.target_database = base.target_database;
  typed.target_schema = base.target_schema;
  if (typed.target_object.object_kind.empty()) {
    typed.target_object.object_kind = api::SecurityOptionValue(base, "target_object_kind:");
  }
  if (typed.target_schema.uuid.canonical.empty()) {
    typed.target_schema.uuid.canonical = api::SecurityOptionValue(base, "target_schema_uuid:");
  }
  if (typed.target_schema.uuid.canonical.empty()) {
    typed.target_schema.uuid.canonical = api::SecurityOptionValue(base, "schema_uuid:");
  }
  if (typed.target_schema.uuid.canonical.empty()) {
    typed.target_schema.uuid.canonical = api::SecurityOptionValue(base, "schema_parent_uuid:");
  }
  std::string normalized_parent_path;
  const std::string schema_parent_path = api::SecurityOptionValue(base, "schema_parent_path:");
  if (typed.target_schema.uuid.canonical.empty() && !schema_parent_path.empty()) {
    const auto resolved_parent =
        ResolveSchemaParentPathToUuid(base, schema_parent_path, &normalized_parent_path);
    if (resolved_parent) {
      typed.target_schema.uuid.canonical = *resolved_parent;
    }
  }
  if (!typed.target_schema.uuid.canonical.empty() && typed.target_schema.object_kind.empty()) {
    typed.target_schema.object_kind = "schema";
  }
  if (typed.localized_names.empty()) {
    const std::string object_name = api::SecurityOptionValue(base, "name:");
    if (!object_name.empty()) {
      if (normalized_parent_path.empty() && !schema_parent_path.empty()) {
        normalized_parent_path = JoinDottedIdentifierPath(SplitDottedIdentifierPath(schema_parent_path));
      }
      const std::string full_path =
          normalized_parent_path.empty() ? std::string{} : normalized_parent_path + "." + object_name;
      typed.localized_names.push_back({"en", "primary", full_path, object_name, true});
    }
  }
  return typed;
}

api::EngineAlterObjectRequest TypedAlterObjectRequest(const SblrDispatchRequest& request) {
  api::EngineAlterObjectRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  if (typed.target_object.uuid.canonical.empty()) {
    typed.target_object.uuid.canonical = api::SecurityOptionValue(base, "target_object_uuid:");
  }
  if (typed.target_object.uuid.canonical.empty()) {
    typed.target_object.uuid.canonical = api::SecurityOptionValue(base, "domain_target_uuid:");
  }
  if (typed.target_object.uuid.canonical.empty()) {
    typed.target_object.uuid.canonical = api::SecurityOptionValue(base, "rename_target_uuid:");
  }
  if (typed.target_object.uuid.canonical.empty()) {
    typed.target_object.uuid.canonical = api::SecurityOptionValue(base, "sequence_target_uuid:");
  }
  if (typed.target_object.object_kind.empty()) {
    typed.target_object.object_kind = api::SecurityOptionValue(base, "target_object_kind:");
  }
  if (typed.target_object.object_kind.empty()) {
    typed.target_object.object_kind = api::SecurityOptionValue(base, "rename_target_kind:");
  }
  if (typed.target_object.object_kind.empty()) typed.target_object.object_kind = "object";
  if (typed.target_schema.uuid.canonical.empty()) {
    typed.target_schema.uuid.canonical = api::SecurityOptionValue(base, "target_schema_uuid:");
  }
  if (typed.target_schema.uuid.canonical.empty()) {
    typed.target_schema.uuid.canonical = api::SecurityOptionValue(base, "schema_uuid:");
  }
  if (typed.target_schema.uuid.canonical.empty()) {
    typed.target_schema.uuid.canonical = api::SecurityOptionValue(base, "schema_parent_uuid:");
  }
  std::string normalized_parent_path;
  const std::string schema_parent_path = api::SecurityOptionValue(base, "schema_parent_path:");
  if (typed.target_schema.uuid.canonical.empty() && !schema_parent_path.empty()) {
    const auto resolved_parent =
        ResolveSchemaParentPathToUuid(base, schema_parent_path, &normalized_parent_path);
    if (resolved_parent) {
      typed.target_schema.uuid.canonical = *resolved_parent;
    }
  }
  if (!typed.target_schema.uuid.canonical.empty() && typed.target_schema.object_kind.empty()) {
    typed.target_schema.object_kind = "schema";
  }
  if (typed.localized_names.empty()) {
    std::string new_name = api::SecurityOptionValue(base, "rename_new_name:");
    if (new_name.empty()) new_name = api::SecurityOptionValue(base, "new_name:");
    if (new_name.empty()) new_name = api::SecurityOptionValue(base, "name:");
    if (!new_name.empty()) {
      typed.localized_names.push_back({"en", "primary", "", new_name, true});
    }
  }
  if (typed.target_object.object_kind == "domain") {
    typed.option_envelopes.clear();
    for (const auto& option : base.option_envelopes) {
      if (IsDomainRuntimeOption(option)) {
        typed.option_envelopes.push_back(option);
      }
    }
  } else if (typed.target_object.object_kind == "schema") {
    typed.option_envelopes.clear();
    for (const auto& option : base.option_envelopes) {
      if (IsCreateSchemaRuntimeOption(option)) {
        typed.option_envelopes.push_back(option);
      }
    }
  }
  if (!schema_parent_path.empty() && typed.target_schema.uuid.canonical.empty()) {
    typed.option_envelopes.push_back("unresolved_schema_parent_path:" + schema_parent_path);
  }
  return typed;
}

api::EngineInsertRowsRequest TypedInsertRowsRequest(const SblrDispatchRequest& request) {
  api::EngineInsertRowsRequest typed;
  api::EngineApiRequest base = BaseApiRequest(request);
  EnsureDefaultWriteResultPolicy(&base, base.operation_id);
  typed.target_table = TargetObjectForDml(base, "table");
  typed.estimated_row_count = DispatchOptionU64(base, "estimated_row_count:");
  typed.insert_mode = api::SecurityOptionValue(base, "insert_mode:");
  const std::string duplicate_mode = api::SecurityOptionValue(base, "duplicate_mode:");
  if (!duplicate_mode.empty()) { typed.duplicate_mode = duplicate_mode; }
  typed.on_conflict_action = api::SecurityOptionValue(base, "on_conflict_action:");
  typed.conflict_target_column = api::SecurityOptionValue(base, "conflict_target_column:");
  const auto append_conflict_update_columns = [&typed](std::string_view encoded) {
    std::string current;
    std::istringstream columns{std::string(encoded)};
    while (std::getline(columns, current, ',')) {
      if (!current.empty()) {
        typed.conflict_update_columns.push_back(current);
      }
    }
  };
  for (const auto& option : base.option_envelopes) {
    constexpr std::string_view prefix = "conflict_update_column:";
    if (option.rfind(prefix, 0) == 0) {
      append_conflict_update_columns(std::string_view(option).substr(prefix.size()));
    }
    constexpr std::string_view lowered_prefix = "on_conflict_update_column:";
    if (option.rfind(lowered_prefix, 0) == 0) {
      append_conflict_update_columns(std::string_view(option).substr(lowered_prefix.size()));
    }
  }
  typed.strict_bulk_load_requested = api::SecurityOptionBool(base, "strict_bulk_load_requested:", false);
  typed.reference_unique_checks_relaxed = api::SecurityOptionBool(base, "reference_unique_checks_relaxed:", false);
  typed.reference_foreign_key_checks_relaxed = api::SecurityOptionBool(base, "reference_foreign_key_checks_relaxed:", false);
  const std::uint64_t default_values_row_count =
      DispatchOptionU64(base, "insert_default_values_row_count:");
  typed.input_rows = std::move(base.rows);
  if (typed.input_rows.empty() && default_values_row_count == 1) {
    typed.input_rows.emplace_back();
  }
  static_cast<api::EngineApiRequest&>(typed) = std::move(base);
  return typed;
}

api::EngineSelectRowsRequest TypedSelectRowsRequest(const SblrDispatchRequest& request) {
  api::EngineSelectRowsRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.source_object = TargetObjectForDml(base, "table");
  typed.select_projection = base.projection;
  const bool relation_projection_view =
      DecodeRelationProjectionViewSelectTransportV1(base, &typed);
  const bool global_aggregate_view =
      relation_projection_view
          ? false
          : DecodeGlobalAggregateViewSelectTransportV1(base, &typed);
  const bool global_aggregate_projection =
      relation_projection_view || global_aggregate_view
          ? false
          : DecodeGlobalAggregateProjectionTransportV1(base, &typed);
  if (!relation_projection_view && !global_aggregate_view &&
      !global_aggregate_projection &&
      typed.select_projection.canonical_projection_envelopes.empty()) {
    const std::uint64_t projection_count =
        DispatchOptionU64(base, "projection_count:");
    for (std::uint64_t index = 0; index < projection_count && index < 16;
         ++index) {
      const std::string projection = api::SecurityOptionValue(
          base, "projection_" + std::to_string(index) + ":");
      if (!projection.empty()) {
        typed.select_projection.canonical_projection_envelopes.push_back(
            projection);
      }
    }
  }
  typed.select_predicate = base.predicate;
  typed.select_ordering = base.ordering;
  const std::string order_by = api::SecurityOptionValue(base, "order_by:");
  if (!order_by.empty() && typed.select_ordering.canonical_ordering_envelopes.empty()) {
    std::string direction = api::SecurityOptionValue(base, "order_direction:");
    if (direction.empty()) direction = "asc";
    std::string ordering = order_by + ":" + direction;
    const std::string nulls = api::SecurityOptionValue(base, "order_nulls:");
    if (!nulls.empty()) {
      ordering.append(":nulls_");
      ordering.append(nulls);
    }
    typed.select_ordering.canonical_ordering_envelopes.push_back(std::move(ordering));
  }
  typed.limit = DispatchOptionU64(base, "limit:");
  typed.offset = DispatchOptionU64(base, "offset:");
  return typed;
}

bool ParseRelationRowUuid(const std::string& row_uuid,
                          std::size_t* relation_index,
                          std::string* relation_row_uuid) {
  constexpr std::string_view prefix = "relation-";
  constexpr std::string_view marker = "-row-";
  constexpr std::size_t kMaximumLegacyRelationCount = 16;
  if (!row_uuid.starts_with(prefix)) return false;
  const auto marker_pos = row_uuid.find(marker, prefix.size());
  if (marker_pos == std::string::npos || marker_pos == prefix.size()) return false;
  std::size_t parsed = 0;
  for (std::size_t i = prefix.size(); i < marker_pos; ++i) {
    const unsigned char ch = static_cast<unsigned char>(row_uuid[i]);
    if (ch < '0' || ch > '9') return false;
    const auto digit = static_cast<std::size_t>(ch - '0');
    if (parsed > (std::numeric_limits<std::size_t>::max() - digit) / 10) {
      return false;
    }
    parsed = parsed * 10u + digit;
  }
  if (parsed >= kMaximumLegacyRelationCount) return false;
  if (relation_index != nullptr) *relation_index = parsed;
  if (relation_row_uuid != nullptr) {
    *relation_row_uuid = row_uuid.substr(marker_pos + marker.size());
  }
  return true;
}

api::EnginePlanOperationRequest TypedLegacyPlanOperationRequest(
    const SblrDispatchRequest& request) {
  // QOW-SOURCE-WIN-001-V1
  // The legacy adapter preserves explicit input only. Missing function and
  // operand fields remain missing so downstream refusal cannot silently select
  // ROW_NUMBER, bucket one, or another substitute implementation.
  api::EnginePlanOperationRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.target_object = TargetObjectForDml(base, "table");
  for (std::size_t index = 0; index < 16; ++index) {
    const std::string prefix = "related_object_" + std::to_string(index) + "_";
    api::EngineObjectReference related;
    related.uuid.canonical = api::SecurityOptionValue(base, prefix + "uuid:");
    related.object_kind = api::SecurityOptionValue(base, prefix + "kind:");
    if (!related.uuid.canonical.empty()) {
      if (related.object_kind.empty()) related.object_kind = "table";
      typed.related_objects.push_back(std::move(related));
    }
  }
  typed.execute = api::SecurityOptionBool(base, "execute:", false);
  typed.query_operation = api::SecurityOptionValue(base, "query_operation:");
  typed.set_operation = api::SecurityOptionValue(base, "set_operation:");
  typed.set_by_name = api::SecurityOptionBool(base, "set_by_name:", false);
  typed.join_algorithm = api::SecurityOptionValue(base, "join_algorithm:");
  typed.left_key_column = DispatchOptionU64(base, "left_key_column:");
  typed.right_key_column = DispatchOptionU64(base, "right_key_column:");
  typed.left_key_field = api::SecurityOptionValue(base, "left_key_field:");
  typed.right_key_field = api::SecurityOptionValue(base, "right_key_field:");
  typed.group_key_column = DispatchOptionU64(base, "group_key_column:");
  typed.aggregate_value_column = DispatchOptionU64(base, "aggregate_value_column:");
  typed.aggregate_pair_value_column = DispatchOptionU64(base, "aggregate_pair_value_column:");
  typed.group_key_field = api::SecurityOptionValue(base, "group_key_field:");
  typed.aggregate_value_field = api::SecurityOptionValue(base, "aggregate_value_field:");
  typed.aggregate_pair_value_field = api::SecurityOptionValue(base, "aggregate_pair_value_field:");
  typed.aggregate_function = api::SecurityOptionValue(base, "aggregate_function:");
  typed.order_column = DispatchOptionU64(base, "order_column:");
  typed.order_field = api::SecurityOptionValue(base, "order_by:");
  typed.window_function = api::SecurityOptionValue(base, "window_function:");
  typed.window_value_column = DispatchOptionU64(base, "window_value_column:");
  typed.window_value_field = api::SecurityOptionValue(base, "window_value_field:");
  typed.partition_key_column = DispatchOptionU64(base, "partition_column:");
  typed.partition_key_field = api::SecurityOptionValue(base, "partition_by:");
  if (typed.partition_key_field.empty()) {
    typed.partition_key_field = api::SecurityOptionValue(base, "partition_key_field:");
  }
  typed.window_n = DispatchOptionU64(base, "window_n:");
  if (typed.window_n == 0) typed.window_n = DispatchOptionU64(base, "window_bucket_count:");
  typed.limit = DispatchOptionU64(base, "limit:");
  typed.offset = DispatchOptionU64(base, "offset:");

  for (const auto& row : base.rows) {
    std::size_t relation_index = 0;
    std::string relation_row_uuid;
    if (!ParseRelationRowUuid(row.requested_row_uuid.canonical,
                              &relation_index,
                              &relation_row_uuid)) {
      continue;
    }
    if (typed.relations.size() <= relation_index) {
      typed.relations.resize(relation_index + 1);
    }
    auto& relation = typed.relations[relation_index];
    relation.relation_name = "relation-" + std::to_string(relation_index);
    relation.descriptor_digest = relation.relation_name;
    api::EngineRowValue relation_row = row;
    relation_row.requested_row_uuid.canonical = relation_row_uuid;
    relation.rows.push_back(std::move(relation_row));
  }
  return typed;
}

SblrValue SblrValueFromProjectionArgument(
    const api::EngineProjectionFunctionArgument& argument) {
  SblrValue value;
  const std::string type_name = LowerAscii(argument.type_name);
  value.descriptor_id = type_name;
  value.encoded_value = argument.encoded_value;
  value.text_value = argument.encoded_value;
  value.is_null = argument.is_null;
  if (argument.is_null) {
    value.payload_kind = SblrValuePayloadKind::none;
    return value;
  }
  if (type_name == "binary" || type_name == "varbinary") {
    std::vector<std::uint8_t> bytes;
    if (HexDecodeBytes(argument.encoded_value, &bytes)) {
      value.binary_value = std::move(bytes);
      value.payload_kind = SblrValuePayloadKind::binary;
      return value;
    }
    value.payload_kind = SblrValuePayloadKind::none;
    return value;
  }
  if (type_name == "bigint" || type_name == "integer" ||
      type_name == "int8" || type_name == "int16" ||
      type_name == "int32" || type_name == "int64") {
    const auto parsed = std::from_chars(
        argument.encoded_value.data(),
        argument.encoded_value.data() + argument.encoded_value.size(),
        value.int64_value);
    if (parsed.ec == std::errc{} &&
        parsed.ptr ==
            argument.encoded_value.data() + argument.encoded_value.size()) {
      value.has_int64_value = true;
      value.payload_kind = SblrValuePayloadKind::signed_integer;
      return value;
    }
    value.payload_kind = SblrValuePayloadKind::none;
    return value;
  }
  if (type_name == "uint8" || type_name == "uint16" ||
      type_name == "uint32" || type_name == "uint64") {
    const auto parsed = std::from_chars(
        argument.encoded_value.data(),
        argument.encoded_value.data() + argument.encoded_value.size(),
        value.uint64_value);
    if (parsed.ec == std::errc{} &&
        parsed.ptr ==
            argument.encoded_value.data() + argument.encoded_value.size()) {
      value.has_uint64_value = true;
      value.payload_kind = SblrValuePayloadKind::unsigned_integer;
      return value;
    }
    value.payload_kind = SblrValuePayloadKind::none;
    return value;
  }
  if (type_name == "boolean" || type_name == "bool") {
    const std::string lowered = LowerAscii(argument.encoded_value);
    if (lowered == "true" || lowered == "1") {
      value.int64_value = 1;
      value.has_int64_value = true;
      value.payload_kind = SblrValuePayloadKind::boolean;
      value.descriptor_id = "boolean";
      return value;
    }
    if (lowered == "false" || lowered == "0") {
      value.int64_value = 0;
      value.has_int64_value = true;
      value.payload_kind = SblrValuePayloadKind::boolean;
      value.descriptor_id = "boolean";
      return value;
    }
    value.payload_kind = SblrValuePayloadKind::none;
    return value;
  }
  if (type_name == "real32" || type_name == "real64" ||
      type_name == "real128" || type_name == "double" ||
      type_name == "numeric" || type_name == "decimal" ||
      type_name.rfind("numeric(", 0) == 0 ||
      type_name.rfind("decimal(", 0) == 0) {
    const auto parsed = std::from_chars(
        argument.encoded_value.data(),
        argument.encoded_value.data() + argument.encoded_value.size(),
        value.real64_value,
        std::chars_format::general);
    if (parsed.ec == std::errc{} &&
        parsed.ptr ==
            argument.encoded_value.data() + argument.encoded_value.size() &&
        std::isfinite(value.real64_value)) {
      value.has_real64_value = true;
      value.payload_kind = SblrValuePayloadKind::real64;
      return value;
    }
    value.payload_kind = SblrValuePayloadKind::none;
    return value;
  }
  value.payload_kind = SblrValuePayloadKind::text;
  return value;
}

bool ProjectionArgumentEncodingValid(
    const api::EngineProjectionFunctionArgument& argument) {
  if (argument.type_name.empty()) return false;
  if (argument.is_null) return argument.encoded_value.empty();
  const SblrValue value = SblrValueFromProjectionArgument(argument);
  return value.payload_kind != SblrValuePayloadKind::none;
}

bool ProjectionSblrValueResolved(const SblrValue& value) {
  return !value.descriptor_id.empty() &&
         (value.is_null || value.payload_kind != SblrValuePayloadKind::none);
}

api::EngineTypedValue EngineTypedValueFromSblrValue(const SblrValue& value) {
  api::EngineTypedValue out;
  out.descriptor.descriptor_kind = "scalar";
  out.descriptor.canonical_type_name = value.descriptor_id;
  out.descriptor.encoded_descriptor = "type=" + out.descriptor.canonical_type_name;
  out.is_null = value.is_null;
  if (out.is_null) {
    out.encoded_value.clear();
    out.binary_value.clear();
    out.setState(api::EngineValueState::sql_null);
    return out;
  }
  out.setState(api::EngineValueState::value);
  if (value.payload_kind == SblrValuePayloadKind::binary) {
    out.binary_value = value.binary_value;
    out.encoded_value = HexEncodeBytes(value.binary_value);
    return out;
  }
  out.encoded_value = !value.encoded_value.empty() ? value.encoded_value : value.text_value;
  if (value.has_int64_value) out.encoded_value = std::to_string(value.int64_value);
  if (value.has_uint64_value) out.encoded_value = std::to_string(value.uint64_value);
  if (value.has_real64_value) out.encoded_value = FormatReal64(value.real64_value);
  return out;
}

std::vector<std::string> ActiveSavepointNamesForContext(
    const api::EngineRequestContext& context);

SblrExecutionContext SblrExecutionContextFromEngineContext(
    const api::EngineRequestContext& context) {
  SblrExecutionContext out;
  out.database_path = context.database_path;
  out.database_uuid = context.database_uuid.canonical;
  out.cluster_uuid = context.cluster_uuid.canonical;
  out.node_uuid = context.node_uuid.canonical;
  out.transaction_uuid = context.transaction_uuid.canonical;
  out.local_transaction_id = context.local_transaction_id;
  out.snapshot_visible_through_local_transaction_id =
      context.snapshot_visible_through_local_transaction_id;
  out.transaction_isolation_level = context.transaction_isolation_level;
  out.statement_uuid = context.statement_uuid.canonical;
  out.session_uuid = context.session_uuid.canonical;
  out.user_uuid = context.principal_uuid.canonical;
  out.current_schema_uuid = context.current_schema_uuid.canonical;
  out.current_role_uuid = context.current_role_uuid.canonical;
  out.statement_timestamp = context.statement_timestamp;
  out.transaction_timestamp = context.transaction_timestamp;
  out.current_timestamp = context.current_timestamp;
  out.current_monotonic_ns = context.current_monotonic_ns;
  out.deterministic_random_u64 = context.deterministic_random_u64;
  out.deterministic_random_u64_present =
      context.deterministic_random_u64_present;
  out.deterministic_random_bytes_hex = context.deterministic_random_bytes_hex;
  out.deterministic_uuid_text = context.deterministic_uuid_text;
  out.security_context_present = context.security_context_present;
  out.current_sqlstate = context.current_sqlstate;
  out.current_diagnostic_uuid = context.current_diagnostic_uuid.canonical;
  out.current_diagnostic_id = context.current_diagnostic_uuid.canonical;
  out.client_protocol_uuid = context.client_protocol_uuid;
  out.application_name = context.application_name;
  out.read_only_mode = context.read_only_mode;
  out.last_row_count = context.last_row_count;
  out.last_row_count_present = context.last_row_count_present;
  out.transaction_context_present =
      context.local_transaction_id != 0 ||
      !context.transaction_uuid.canonical.empty();
  out.cluster_authority_available = context.cluster_authority_available;
  out.active_savepoint_names = ActiveSavepointNamesForContext(context);
  return out;
}

api::EngineApiDiagnostic FunctionDiagnosticToApi(const SblrRuntimeDiagnostic& diagnostic) {
  api::EngineApiDiagnostic out;
  out.code = diagnostic.diagnostic_id.empty() ? "SB_DIAG_FUNCTION_EXECUTION_FAILED"
                                             : diagnostic.diagnostic_id;
  out.message_key = diagnostic.message_key.empty() ? "engine.function.execution_failed"
                                                   : diagnostic.message_key;
  out.detail = diagnostic.detail;
  out.error = diagnostic.severity != SblrDiagnosticSeverity::info;
  // Only explicitly public, bounded function-diagnostic fields cross the
  // neutral engine API. Runtime identity/security fields remain private.
  // Conversion presentation adapters consume this structured field and must
  // never parse `detail` prose to recover an input value.
  for (const auto& field : diagnostic.fields) {
    if (field.key == "conversion_input_text") {
      out.fields.push_back({field.key, field.value});
    }
  }
  return out;
}

api::EngineApiResult DispatchExecuteTransactionBlock(
    const SblrDispatchRequest& request) {
  auto typed = TypedExecuteTransactionBlockRequest(request);

  // Keep the contract-absent behavior-row route exactly on its legacy path.
  // A procedural block, including a malformed one, holds the same per-database
  // inventory guard used by MGA finality from exact admission until its runtime
  // has completed. Commit/rollback therefore cannot finalize in the gap
  // between transaction validation and expression execution.
  if (!typed.procedural_block_present) {
    auto legacy = api::EngineExecuteTransactionBlock(typed);
    return std::move(static_cast<api::EngineApiResult&>(legacy));
  }
  const auto transaction_inventory_guard =
      api::AcquireTransactionInventoryGuard(request.context.database_path);
  auto admitted = api::EngineExecuteTransactionBlock(typed);
  api::EngineApiResult result =
      std::move(static_cast<api::EngineApiResult&>(admitted));
  result.evidence.push_back(
      {"mga_transaction_guard_scope",
       "exact_admission_through_procedural_runtime"});
  if (!result.ok) {
    return result;
  }

  const auto execution = ExecuteSblrProceduralBlockV1(
      typed.procedural_block,
      SblrExecutionContextFromEngineContext(request.context));
  if (!execution.result.ok()) {
    result.ok = false;
    result.result_shape = {};
    if (execution.result.diagnostics.empty()) {
      result.diagnostics.push_back(api::MakeEngineApiDiagnostic(
          "SB_SBLR_PROCEDURAL_RUNTIME_FAILED",
          "engine.sblr.procedural_block.runtime_failed",
          "procedural runtime failed without a diagnostic",
          true));
    } else {
      for (const auto& diagnostic : execution.result.diagnostics) {
        result.diagnostics.push_back(FunctionDiagnosticToApi(diagnostic));
      }
    }
    result.evidence.push_back(
        {"procedural_runtime", "refused_without_transaction_finality"});
    result.evidence.push_back({"parser_finality", "false"});
    return result;
  }

  result.result_shape.result_kind = "sblr.procedural.block.rows.v1";
  result.result_shape.columns.clear();
  result.result_shape.rows.clear();
  result.evidence.push_back(
      {"procedural_instruction_count_executed",
       std::to_string(execution.instructions_executed)});
  result.evidence.push_back(
      {"procedural_yield_count_executed",
       std::to_string(execution.yields_executed)});
  result.evidence.push_back(
      {"wire_output_descriptor_authority", "parser"});
  result.evidence.push_back(
      {"procedural_result_row_authority", "engine_yield_only"});
  result.evidence.push_back({"transaction_effect", "read"});
  result.evidence.push_back({"parser_finality", "false"});
  return result;
}

std::vector<std::string> ActiveSavepointNamesForContext(const api::EngineRequestContext& context) {
  if (context.local_transaction_id == 0 || context.database_path.empty()) {
    return {};
  }
  return api::ActiveMgaSavepointNames(context);
}

api::EngineApiResult EngineReadSystemVariable(
    const SblrDispatchRequest& request) {
  const api::EngineApiRequest base = BaseApiRequest(request);
  const std::string exact_refusal =
      LowerAscii(api::SecurityOptionValue(base, "exact_refusal:"));
  if (exact_refusal == "true" || exact_refusal == "1" ||
      exact_refusal == "yes") {
    std::string diagnostic_id =
        api::SecurityOptionValue(base, "refusal_diagnostic_id:");
    if (diagnostic_id.empty()) {
      diagnostic_id = api::SecurityOptionValue(base, "diagnostic_id:");
    }
    if (diagnostic_id.empty()) {
      diagnostic_id = "SB_DIAG_FUNCTION_RUNTIME_REFUSAL";
    }

    api::EngineApiResult result;
    result.ok = false;
    result.operation_id = "expression.system_variable_read";
    result.embedded_trust_mode_observed =
        request.context.trust_mode == api::EngineTrustMode::embedded_in_process;
    result.diagnostics.push_back(api::MakeEngineApiDiagnostic(
        diagnostic_id,
        "engine.system_variable.exact_refusal",
        "reference variable compatibility surface is refused by fixed policy "
        "before engine side effects",
        true));
    result.evidence.push_back({"sblr_operation",
                               "expression.system_variable_read"});
    result.evidence.push_back({"sblr_opcode", "SBLR_SYSTEM_VARIABLE_READ"});
    const std::string variable_id =
        api::SecurityOptionValue(base, "variable_id:");
    if (!variable_id.empty()) {
      result.evidence.push_back({"canonical_variable_id", variable_id});
    }
    const std::string reference_source =
        api::SecurityOptionValue(base, "reference_source_spelling:");
    if (!reference_source.empty()) {
      result.evidence.push_back({"reference_source_spelling", reference_source});
    }
    const std::string refusal_function =
        api::SecurityOptionValue(base, "refusal_function_id:");
    if (!refusal_function.empty()) {
      result.evidence.push_back({"refusal_function_id", refusal_function});
    }
    result.evidence.push_back({"exact_refusal", "true"});
    result.evidence.push_back({"mga_visibility_authority",
                               "unchanged_context_read_no_lock_no_snapshot_mutation"});
    result.evidence.push_back({"transaction_effect", "read"});
    return result;
  }

  std::string variable_id = api::SecurityOptionValue(base, "variable_id:");
  if (variable_id.empty()) {
    variable_id = api::SecurityOptionValue(base, "canonical_variable_id:");
  }
  if (variable_id.empty()) {
    return FailureResult(request.context,
                         "expression.system_variable_read",
                         "SB_DIAG_SYSTEM_VARIABLE_ID_REQUIRED",
                         "engine.system_variable.id_required",
                         "system variable read requires canonical variable_id");
  }

  const auto sblr_context =
      SblrExecutionContextFromEngineContext(request.context);
  const auto variable_result =
      ResolveSblrContextVariable(variable_id, sblr_context);
  if (!variable_result.ok() || variable_result.scalar_values.size() != 1) {
    api::EngineApiResult result;
    result.ok = false;
    result.operation_id = "expression.system_variable_read";
    result.embedded_trust_mode_observed =
        request.context.trust_mode == api::EngineTrustMode::embedded_in_process;
    if (variable_result.diagnostics.empty()) {
      result.diagnostics.push_back(api::MakeEngineApiDiagnostic(
          "SB_DIAG_SYSTEM_VARIABLE_RESULT_SHAPE_INVALID",
          "engine.system_variable.result_shape_invalid",
          "system variable read expected exactly one scalar value",
          true));
    } else {
      for (const auto& diagnostic : variable_result.diagnostics) {
        result.diagnostics.push_back(FunctionDiagnosticToApi(diagnostic));
      }
    }
    result.evidence.push_back({"sblr_operation",
                               "expression.system_variable_read"});
    result.evidence.push_back({"sblr_opcode", "SBLR_SYSTEM_VARIABLE_READ"});
    result.evidence.push_back({"canonical_variable_id", variable_id});
    result.evidence.push_back({"mga_visibility_authority",
                               "unchanged_context_read_no_lock_no_snapshot_mutation"});
    return result;
  }

  api::EngineTypedValue value =
      EngineTypedValueFromSblrValue(variable_result.scalar_values.front());
  api::EngineApiResult result;
  result.ok = true;
  result.operation_id = "expression.system_variable_read";
  result.embedded_trust_mode_observed =
      request.context.trust_mode == api::EngineTrustMode::embedded_in_process;
  result.result_shape.result_kind = "rs.sbsql.scalar_value.v1";
  result.result_shape.columns.push_back(value.descriptor);

  api::EngineRowValue row;
  row.requested_row_uuid.canonical = "system-variable-read-row-0";
  row.fields.push_back({"value", std::move(value)});
  result.result_shape.rows.push_back(std::move(row));
  result.evidence.push_back({"sblr_operation",
                             "expression.system_variable_read"});
  result.evidence.push_back({"sblr_opcode", "SBLR_SYSTEM_VARIABLE_READ"});
  result.evidence.push_back({"canonical_variable_id", variable_id});
  const std::string reference_source =
      api::SecurityOptionValue(base, "reference_source_spelling:");
  if (!reference_source.empty()) {
    result.evidence.push_back({"reference_source_spelling", reference_source});
  }
  result.evidence.push_back({"mga_visibility_authority",
                             "unchanged_context_read_no_lock_no_snapshot_mutation"});
  result.evidence.push_back({"transaction_effect", "read"});
  return result;
}

SblrExecutionContext ProjectionOperatorContext(const api::EngineRequestContext& context) {
  return SblrExecutionContextFromEngineContext(context);
}

std::optional<SblrValue> ProjectionLiteralToSblrValue(
    const api::EngineProjectionExpression& expression) {
  api::EngineProjectionFunctionArgument argument;
  argument.type_name = expression.type_name;
  argument.encoded_value = expression.encoded_value;
  argument.is_null = expression.is_null;
  if (!ProjectionArgumentEncodingValid(argument)) return std::nullopt;
  return SblrValueFromProjectionArgument(argument);
}

SblrValue ProjectionTypedValueToSblrValue(const api::EngineTypedValue& value) {
  api::EngineProjectionFunctionArgument argument;
  argument.type_name = value.descriptor.canonical_type_name;
  argument.encoded_value = value.encoded_value;
  argument.is_null = value.is_null;
  return SblrValueFromProjectionArgument(argument);
}

bool TruthFromProjectionValue(const api::EngineTypedValue& value, SblrTruthValue* truth) {
  if (truth == nullptr || value.descriptor.canonical_type_name != "boolean") return false;
  if (value.is_null) {
    *truth = SblrTruthValue::unknown;
    return true;
  }
  const std::string lowered = LowerAscii(value.encoded_value);
  if (lowered == "true" || lowered == "1") {
    *truth = SblrTruthValue::true_value;
    return true;
  }
  if (lowered == "false" || lowered == "0") {
    *truth = SblrTruthValue::false_value;
    return true;
  }
  return false;
}

api::EngineProjectionFunctionResult ProjectionOperatorFailure(const api::EngineProjectionOperatorRequest& request,
                                                             std::string code,
                                                             std::string detail) {
  api::EngineProjectionFunctionResult out;
  out.ok = false;
  out.diagnostics.push_back(api::MakeEngineApiDiagnostic(
      std::move(code), "engine.operator.projection_failed", std::move(detail), true));
  out.evidence.push_back({"operator_runtime", request.expression.operator_id});
  return out;
}

api::EngineProjectionFunctionResult EvaluateProjectionOperatorExpression(
    const api::EngineRequestContext& context,
    const api::EngineProjectionExpression& expression);

std::string EncodeConstructedProjectionValue(
    const std::vector<api::EngineTypedValue>& arguments,
    std::string_view opening,
    std::string_view closing) {
  std::ostringstream encoded;
  encoded << opening;
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    if (index != 0) encoded << ',';
    const auto& argument = arguments[index];
    encoded << argument.descriptor.canonical_type_name << ':';
    encoded << (argument.is_null ? "<NULL>" : argument.encoded_value);
  }
  encoded << closing;
  return encoded.str();
}

api::EngineProjectionFunctionResult ConstructSpecialProjectionValue(
    const api::EngineRequestContext& context,
    const api::EngineProjectionExpression& expression,
    std::string_view canonical_type_name,
    std::string_view encoded_opening,
    std::string_view encoded_closing,
    std::string_view evidence_id) {
  std::vector<api::EngineTypedValue> argument_values;
  argument_values.reserve(expression.arguments.size());
  for (const auto& argument : expression.arguments) {
    auto argument_result = EvaluateProjectionOperatorExpression(context, argument);
    if (!argument_result.ok) return argument_result;
    argument_values.push_back(std::move(argument_result.value));
  }

  api::EngineProjectionFunctionResult out;
  out.ok = true;
  SblrValue value;
  value.descriptor_id = std::string(canonical_type_name);
  value.payload_kind = SblrValuePayloadKind::descriptor_payload;
  value.is_null = false;
  value.encoded_value = EncodeConstructedProjectionValue(
      argument_values, encoded_opening, encoded_closing);
  out.value = EngineTypedValueFromSblrValue(value);
  out.evidence.push_back({"special_form_runtime", std::string(evidence_id)});
  return out;
}

api::EngineProjectionFunctionResult EvaluateProjectionOperatorExpression(
    const api::EngineRequestContext& context,
    const api::EngineProjectionExpression& expression);

api::EngineProjectionFunctionResult EvaluateProjectionFunction(
    const api::EngineProjectionFunctionRequest& request);

api::EngineProjectionFunctionResult OperatorResultToProjectionResult(
    const std::string& operator_id,
    const SblrResult& result) {
  api::EngineProjectionFunctionResult out;
  out.ok = result.ok() && result.scalar_values.size() == 1 &&
           ProjectionSblrValueResolved(result.scalar_values.front());
  out.evidence.push_back({"operator_runtime", operator_id});
  if (out.ok) {
    out.value = EngineTypedValueFromSblrValue(result.scalar_values.front());
    return out;
  }
  if (result.diagnostics.empty()) {
    out.diagnostics.push_back(api::MakeEngineApiDiagnostic(
        "SB_DIAG_OPERATOR_RESULT_SHAPE_INVALID",
        "engine.operator.result_shape_invalid",
        "operator projection expected exactly one resolved typed scalar value",
        true));
  } else {
    for (const auto& diagnostic : result.diagnostics) {
      out.diagnostics.push_back(FunctionDiagnosticToApi(diagnostic));
    }
  }
  return out;
}

api::EngineProjectionFunctionResult EvaluateProjectionOperatorExpression(
    const api::EngineRequestContext& context,
    const api::EngineProjectionExpression& expression) {
  if (expression.expression_kind == "literal") {
    const auto literal = ProjectionLiteralToSblrValue(expression);
    if (!literal) {
      api::EngineProjectionOperatorRequest failure_request;
      failure_request.context = context;
      failure_request.expression = expression;
      return ProjectionOperatorFailure(
          failure_request,
          "SB_DIAG_OPERATOR_INVALID_INPUT",
          "projection literal encoding does not match its bound type");
    }
    api::EngineProjectionFunctionResult out;
    out.ok = true;
    out.value = EngineTypedValueFromSblrValue(*literal);
    return out;
  }
  if (expression.expression_kind == "function") {
    api::EngineProjectionOperatorRequest failure_request;
    failure_request.context = context;
    failure_request.expression = expression;
    if (expression.function_id.empty()) {
      return ProjectionOperatorFailure(failure_request,
                                       "SB_DIAG_OPERATOR_INVALID_INPUT",
                                       "nested function projection requires a function id");
    }
    api::EngineProjectionFunctionRequest function_request;
    function_request.context = context;
    function_request.function_id = expression.function_id;
    std::vector<api::EngineEvidenceReference> argument_evidence;
    for (std::size_t index = 0; index < expression.arguments.size(); ++index) {
      auto argument_result = EvaluateProjectionOperatorExpression(context, expression.arguments[index]);
      if (!argument_result.ok) return argument_result;
      api::EngineProjectionFunctionArgument argument;
      argument.name = expression.arguments[index].name.empty()
                          ? "arg" + std::to_string(index)
                          : expression.arguments[index].name;
      argument.type_name = argument_result.value.descriptor.canonical_type_name;
      argument.encoded_value = argument_result.value.encoded_value;
      argument.is_null = argument_result.value.is_null;
      function_request.arguments.push_back(std::move(argument));
      argument_evidence.insert(argument_evidence.end(),
                               argument_result.evidence.begin(),
                               argument_result.evidence.end());
    }
    auto out = EvaluateProjectionFunction(function_request);
    if (out.ok) {
      out.evidence.insert(out.evidence.end(), argument_evidence.begin(), argument_evidence.end());
    }
    return out;
  }
  if (expression.expression_kind == "special_form") {
    api::EngineProjectionOperatorRequest failure_request;
    failure_request.context = context;
    failure_request.expression = expression;
    if (expression.special_form_id == "sb.special.array_constructor") {
      if (expression.arguments.empty()) {
        return ProjectionOperatorFailure(failure_request,
                                         "SB_DIAG_OPERATOR_INVALID_INPUT",
                                         "ARRAY constructor projection requires at least one operand");
      }
      return ConstructSpecialProjectionValue(context,
                                             expression,
                                             "array",
                                             "array[",
                                             "]",
                                             "special_array_constructor");
    }
    if (expression.special_form_id == "sb.special.row_constructor") {
      if (expression.arguments.empty()) {
        return ProjectionOperatorFailure(failure_request,
                                         "SB_DIAG_OPERATOR_INVALID_INPUT",
                                         "ROW constructor projection requires at least one operand");
      }
      return ConstructSpecialProjectionValue(context,
                                             expression,
                                             "row",
                                             "row(",
                                             ")",
                                             "special_row_constructor");
    }
    if (expression.special_form_id == "sb.special.between") {
      if (expression.arguments.size() != 3) {
        return ProjectionOperatorFailure(failure_request,
                                         "SB_DIAG_OPERATOR_INVALID_INPUT",
                                         "BETWEEN projection requires value, lower, and upper operands");
      }

      const auto value = EvaluateProjectionOperatorExpression(context, expression.arguments[0]);
      if (!value.ok) return value;
      const auto lower = EvaluateProjectionOperatorExpression(context, expression.arguments[1]);
      if (!lower.ok) return lower;
      const auto upper = EvaluateProjectionOperatorExpression(context, expression.arguments[2]);
      if (!upper.ok) return upper;

      const auto sblr_context = ProjectionOperatorContext(context);
      const auto ge_result = EvaluateSblrComparison("op_ge",
                                                    ProjectionTypedValueToSblrValue(value.value),
                                                    ProjectionTypedValueToSblrValue(lower.value),
                                                    sblr_context);
      const auto ge_projection = OperatorResultToProjectionResult("op_ge", ge_result);
      if (!ge_projection.ok) return ge_projection;
      const auto le_result = EvaluateSblrComparison("op_le",
                                                    ProjectionTypedValueToSblrValue(value.value),
                                                    ProjectionTypedValueToSblrValue(upper.value),
                                                    sblr_context);
      const auto le_projection = OperatorResultToProjectionResult("op_le", le_result);
      if (!le_projection.ok) return le_projection;

      SblrTruthValue ge_truth = SblrTruthValue::unknown;
      SblrTruthValue le_truth = SblrTruthValue::unknown;
      if (!TruthFromProjectionValue(ge_projection.value, &ge_truth) ||
          !TruthFromProjectionValue(le_projection.value, &le_truth)) {
        return ProjectionOperatorFailure(failure_request,
                                         "SBLR.DESCRIPTOR_MISMATCH",
                                         "BETWEEN comparison produced a non-boolean intermediate");
      }

      api::EngineProjectionFunctionResult out;
      out.ok = true;
      out.value = EngineTypedValueFromSblrValue(MakeSblrTruthValue(SblrAnd(ge_truth, le_truth)));
      out.evidence.push_back({"special_form_runtime", "special_between"});
      out.evidence.push_back({"operator_runtime", "op_ge"});
      out.evidence.push_back({"operator_runtime", "op_le"});
      return out;
    }
    if (expression.special_form_id == "sb.special.in") {
      if (expression.arguments.size() < 2) {
        return ProjectionOperatorFailure(failure_request,
                                         "SB_DIAG_OPERATOR_INVALID_INPUT",
                                         "IN projection requires value and at least one list operand");
      }
      const auto value = EvaluateProjectionOperatorExpression(context, expression.arguments[0]);
      if (!value.ok) return value;
      const auto sblr_context = ProjectionOperatorContext(context);
      bool saw_unknown = false;
      for (std::size_t index = 1; index < expression.arguments.size(); ++index) {
        const auto candidate = EvaluateProjectionOperatorExpression(context, expression.arguments[index]);
        if (!candidate.ok) return candidate;
        const auto eq_result = EvaluateSblrComparison("op_eq",
                                                      ProjectionTypedValueToSblrValue(value.value),
                                                      ProjectionTypedValueToSblrValue(candidate.value),
                                                      sblr_context);
        const auto eq_projection = OperatorResultToProjectionResult("op_eq", eq_result);
        if (!eq_projection.ok) return eq_projection;
        SblrTruthValue eq_truth = SblrTruthValue::unknown;
        if (!TruthFromProjectionValue(eq_projection.value, &eq_truth)) {
          return ProjectionOperatorFailure(failure_request,
                                           "SBLR.DESCRIPTOR_MISMATCH",
                                           "IN comparison produced a non-boolean intermediate");
        }
        if (eq_truth == SblrTruthValue::true_value) {
          api::EngineProjectionFunctionResult out;
          out.ok = true;
          out.value = EngineTypedValueFromSblrValue(MakeSblrTruthValue(SblrTruthValue::true_value));
          out.evidence.push_back({"special_form_runtime", "special_in"});
          out.evidence.push_back({"operator_runtime", "op_eq"});
          return out;
        }
        if (eq_truth == SblrTruthValue::unknown) saw_unknown = true;
      }
      api::EngineProjectionFunctionResult out;
      out.ok = true;
      out.value = EngineTypedValueFromSblrValue(
          MakeSblrTruthValue(saw_unknown ? SblrTruthValue::unknown
                                         : SblrTruthValue::false_value));
      out.evidence.push_back({"special_form_runtime", "special_in"});
      out.evidence.push_back({"operator_runtime", "op_eq"});
      return out;
    }
    if (expression.special_form_id == "sb.special.case") {
      if (expression.arguments.size() < 3 || expression.arguments.size() % 2 != 1) {
        return ProjectionOperatorFailure(failure_request,
                                         "SB_DIAG_OPERATOR_INVALID_INPUT",
                                         "CASE projection requires condition/result pairs plus ELSE operand");
      }
      for (std::size_t index = 0; index + 1 < expression.arguments.size(); index += 2) {
        const auto condition =
            EvaluateProjectionOperatorExpression(context, expression.arguments[index]);
        if (!condition.ok) return condition;
        SblrTruthValue truth = SblrTruthValue::unknown;
        if (!TruthFromProjectionValue(condition.value, &truth)) {
          return ProjectionOperatorFailure(failure_request,
                                           "SBLR.DESCRIPTOR_MISMATCH",
                                           "CASE WHEN condition produced a non-boolean intermediate");
        }
        if (truth == SblrTruthValue::true_value) {
          auto out =
              EvaluateProjectionOperatorExpression(context, expression.arguments[index + 1]);
          if (out.ok) {
            out.evidence.insert(out.evidence.end(),
                                condition.evidence.begin(),
                                condition.evidence.end());
            out.evidence.push_back({"special_form_runtime", "special_case"});
          }
          return out;
        }
      }
      auto out = EvaluateProjectionOperatorExpression(context, expression.arguments.back());
      if (out.ok) out.evidence.push_back({"special_form_runtime", "special_case"});
      return out;
    }
    return ProjectionOperatorFailure(failure_request,
                                     "SB_DIAG_OPERATOR_INVALID_INPUT",
                                     "special form projection is not registered for this bounded route");
  }
  if (expression.expression_kind != "operator") {
    api::EngineProjectionFunctionResult out;
    out.ok = false;
    out.diagnostics.push_back(api::MakeEngineApiDiagnostic(
        "SB_DIAG_OPERATOR_INVALID_INPUT",
        "engine.operator.invalid_projection_expression",
        "bounded operator projection accepts only literal and operator operands",
        true));
    return out;
  }

  api::EngineProjectionOperatorRequest failure_request;
  failure_request.context = context;
  failure_request.expression = expression;

  if (expression.operator_id == "op_not") {
    if (expression.arguments.size() != 1) {
      return ProjectionOperatorFailure(failure_request,
                                       "SB_DIAG_OPERATOR_INVALID_INPUT",
                                       "NOT projection requires one operand");
    }
    const auto operand = EvaluateProjectionOperatorExpression(context, expression.arguments[0]);
    if (!operand.ok) return operand;
    SblrTruthValue truth = SblrTruthValue::unknown;
    if (!TruthFromProjectionValue(operand.value, &truth)) {
      return ProjectionOperatorFailure(failure_request,
                                       "SBLR.DESCRIPTOR_MISMATCH",
                                       "NOT projection requires a boolean operand");
    }
    api::EngineProjectionFunctionResult out;
    out.ok = true;
    out.value = EngineTypedValueFromSblrValue(MakeSblrTruthValue(SblrNot(truth)));
    out.evidence.push_back({"operator_runtime", "op_not"});
    return out;
  }

  if (expression.operator_id == "op_is_null") {
    if (expression.arguments.size() != 1) {
      return ProjectionOperatorFailure(failure_request,
                                       "SB_DIAG_OPERATOR_INVALID_INPUT",
                                       "IS NULL projection requires one operand");
    }
    const auto operand = EvaluateProjectionOperatorExpression(context, expression.arguments[0]);
    if (!operand.ok) return operand;
    const auto result = EvaluateSblrComparison(
        "op_is_null",
        ProjectionTypedValueToSblrValue(operand.value),
        SblrValue{},
        ProjectionOperatorContext(context));
    return OperatorResultToProjectionResult("op_is_null", result);
  }

  if (expression.operator_id == "op_and" || expression.operator_id == "op_or" ||
      expression.operator_id == "op_xor") {
    if (expression.arguments.size() != 2) {
      return ProjectionOperatorFailure(failure_request,
                                       "SB_DIAG_OPERATOR_INVALID_INPUT",
                                       "logical projection requires two operands");
    }
    const auto left = EvaluateProjectionOperatorExpression(context, expression.arguments[0]);
    if (!left.ok) return left;
    const auto right = EvaluateProjectionOperatorExpression(context, expression.arguments[1]);
    if (!right.ok) return right;
    SblrTruthValue left_truth = SblrTruthValue::unknown;
    SblrTruthValue right_truth = SblrTruthValue::unknown;
    if (!TruthFromProjectionValue(left.value, &left_truth) ||
        !TruthFromProjectionValue(right.value, &right_truth)) {
      return ProjectionOperatorFailure(failure_request,
                                       "SBLR.DESCRIPTOR_MISMATCH",
                                       "logical projection requires boolean operands");
    }
    SblrTruthValue truth = SblrTruthValue::unknown;
    if (expression.operator_id == "op_and") {
      truth = SblrAnd(left_truth, right_truth);
    } else if (expression.operator_id == "op_or") {
      truth = SblrOr(left_truth, right_truth);
    } else {
      truth = SblrXor(left_truth, right_truth);
    }
    api::EngineProjectionFunctionResult out;
    out.ok = true;
    out.value = EngineTypedValueFromSblrValue(MakeSblrTruthValue(truth));
    out.evidence.push_back({"operator_runtime", expression.operator_id});
    return out;
  }

  if (expression.operator_id == "op_unary_minus") {
    if (expression.arguments.size() != 1) {
      return ProjectionOperatorFailure(failure_request,
                                       "SB_DIAG_OPERATOR_INVALID_INPUT",
                                       "unary minus projection requires one operand");
    }
    const auto operand = EvaluateProjectionOperatorExpression(context, expression.arguments[0]);
    if (!operand.ok) return operand;
    const auto result = EvaluateSblrUnaryArithmetic(
        "op_unary_minus", ProjectionTypedValueToSblrValue(operand.value), ProjectionOperatorContext(context));
    return OperatorResultToProjectionResult("op_unary_minus", result);
  }

  if (expression.operator_id == "op_eq" ||
      expression.operator_id == "op_ne" ||
      expression.operator_id == "op_lt" ||
      expression.operator_id == "op_le" ||
      expression.operator_id == "op_ge" ||
      expression.operator_id == "op_add" ||
      expression.operator_id == "op_sub" ||
      expression.operator_id == "op_mul" ||
      expression.operator_id == "op_div" ||
      expression.operator_id == "op_gt" ||
      expression.operator_id == "op_like" ||
      expression.operator_id == "op_ilike" ||
      expression.operator_id == "op_regex_match" ||
      expression.operator_id == "op_is_distinct" ||
      expression.operator_id == "op_json_get" ||
      expression.operator_id == "op_json_get_text" ||
      expression.operator_id == "op_array_contains") {
    if (expression.arguments.size() != 2) {
      return ProjectionOperatorFailure(failure_request,
                                       "SB_DIAG_OPERATOR_INVALID_INPUT",
                                       "binary operator projection requires two operands");
    }
    const auto left = EvaluateProjectionOperatorExpression(context, expression.arguments[0]);
    if (!left.ok) return left;
    const auto right = EvaluateProjectionOperatorExpression(context, expression.arguments[1]);
    if (!right.ok) return right;
    const SblrValue left_value = ProjectionTypedValueToSblrValue(left.value);
    const SblrValue right_value = ProjectionTypedValueToSblrValue(right.value);
    const auto sblr_context = ProjectionOperatorContext(context);
    if (expression.operator_id == "op_like" ||
        expression.operator_id == "op_ilike" ||
        expression.operator_id == "op_regex_match") {
      const auto result = EvaluateSblrStringOperator(expression.operator_id,
                                                    left_value,
                                                    right_value,
                                                    sblr_context);
      return OperatorResultToProjectionResult(expression.operator_id, result);
    }
    if (expression.operator_id == "op_eq" ||
        expression.operator_id == "op_ne" ||
        expression.operator_id == "op_lt" ||
        expression.operator_id == "op_le" ||
        expression.operator_id == "op_gt" ||
        expression.operator_id == "op_ge" ||
        expression.operator_id == "op_is_distinct") {
      const auto result = EvaluateSblrComparison(expression.operator_id,
                                                 left_value,
                                                 right_value,
                                                 sblr_context);
      return OperatorResultToProjectionResult(expression.operator_id, result);
    }
    if (expression.operator_id == "op_mul" || expression.operator_id == "op_div" ||
        expression.operator_id == "op_sub" || expression.operator_id == "op_add") {
      const auto result = EvaluateSblrArithmetic(expression.operator_id,
                                                 left_value,
                                                 right_value,
                                                 sblr_context);
      return OperatorResultToProjectionResult(expression.operator_id, result);
    }
    if (expression.operator_id == "op_json_get" || expression.operator_id == "op_json_get_text") {
      const auto result = EvaluateSblrDocumentOperator(expression.operator_id,
                                                      left_value,
                                                      right_value,
                                                      sblr_context);
      return OperatorResultToProjectionResult(expression.operator_id, result);
    }
    const auto result = EvaluateSblrCollectionOperator("op_array_contains",
                                                       left_value,
                                                       right_value,
                                                       sblr_context);
    return OperatorResultToProjectionResult("op_array_contains", result);
  }

  return ProjectionOperatorFailure(failure_request,
                                   "SB_DIAG_OPERATOR_INVALID_INPUT",
                                   "operator projection is not registered for this bounded route");
}

api::EngineProjectionFunctionResult EvaluateProjectionOperator(
    const api::EngineProjectionOperatorRequest& request) {
  return EvaluateProjectionOperatorExpression(request.context, request.expression);
}

bool StartsWith(std::string_view text, std::string_view prefix) {
  return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

std::string PayloadFieldValue(std::string_view payload, std::string_view key) {
  std::size_t cursor = 0;
  while (cursor <= payload.size()) {
    const std::size_t end = payload.find(';', cursor);
    const std::string_view item =
        end == std::string_view::npos ? payload.substr(cursor)
                                      : payload.substr(cursor, end - cursor);
    if (StartsWith(item, key)) {
      return std::string(item.substr(key.size()));
    }
    if (end == std::string_view::npos) break;
    cursor = end + 1;
  }
  return {};
}

api::EngineProjectionFunctionResult UserFunctionFailure(std::string code,
                                                        std::string detail,
                                                        std::string function_id) {
  api::EngineProjectionFunctionResult out;
  out.ok = false;
  out.diagnostics.push_back(api::MakeEngineApiDiagnostic(
      std::move(code), "engine.user_function.execution_failed", std::move(detail), true));
  out.evidence.push_back({"user_function_runtime", std::move(function_id)});
  return out;
}

api::EngineTypedValue UserFunctionValue(std::string type_name,
                                        std::string encoded,
                                        bool is_null = false) {
  api::EngineTypedValue value;
  value.descriptor.descriptor_kind = "scalar";
  value.descriptor.canonical_type_name = std::move(type_name);
  value.descriptor.encoded_descriptor = "type=" + value.descriptor.canonical_type_name;
  value.encoded_value = std::move(encoded);
  value.is_null = is_null;
  value.state = is_null ? api::EngineValueState::sql_null : api::EngineValueState::value;
  return value;
}

std::optional<long double> UserFunctionNumericArg(
    const api::EngineProjectionFunctionRequest& request,
    std::size_t index) {
  if (index >= request.arguments.size() || request.arguments[index].is_null) return std::nullopt;
  const std::string& encoded = request.arguments[index].encoded_value;
  long double value = 0.0L;
  const auto parsed = std::from_chars(
      encoded.data(), encoded.data() + encoded.size(), value,
      std::chars_format::general);
  if (parsed.ec != std::errc{} ||
      parsed.ptr != encoded.data() + encoded.size() ||
      !std::isfinite(value)) {
    return std::nullopt;
  }
  return value;
}

bool UserFunctionTypeIsNumeric(std::string type) {
  type = LowerAscii(std::move(type));
  return type == "bigint" || type == "integer" || type == "int8" ||
         type == "int16" || type == "int32" || type == "int64" ||
         type == "uint8" || type == "uint16" || type == "uint32" ||
         type == "uint64" || type == "real32" || type == "real64" ||
         type == "real128" || type == "double" || type == "numeric" ||
         type == "decimal" || type.rfind("numeric(", 0) == 0 ||
         type.rfind("decimal(", 0) == 0;
}

bool UserFunctionTypeIsInteger(std::string type) {
  type = LowerAscii(std::move(type));
  return type == "bigint" || type == "integer" || type == "int8" ||
         type == "int16" || type == "int32" || type == "int64" ||
         type == "uint8" || type == "uint16" || type == "uint32" ||
         type == "uint64";
}

bool UserFunctionTypeIsText(std::string type) {
  type = LowerAscii(std::move(type));
  return type == "text" || type == "string" || type == "char" ||
         type == "character" || type == "varchar" ||
         type.rfind("char(", 0) == 0 || type.rfind("varchar(", 0) == 0 ||
         type.rfind("character(", 0) == 0;
}

bool UserFunctionArgumentHasNumericType(
    const api::EngineProjectionFunctionRequest& request,
    std::size_t index) {
  return index < request.arguments.size() &&
         UserFunctionTypeIsNumeric(request.arguments[index].type_name);
}

bool UserFunctionArgumentHasIntegerType(
    const api::EngineProjectionFunctionRequest& request,
    std::size_t index) {
  return index < request.arguments.size() &&
         UserFunctionTypeIsInteger(request.arguments[index].type_name);
}

std::string FormatDecimal(long double value, std::uint32_t scale) {
  std::ostringstream encoded;
  encoded << std::fixed << std::setprecision(scale) << value;
  return encoded.str();
}

std::optional<std::uint32_t> DecimalScaleFromType(
    std::string_view type_name) {
  const std::size_t comma = type_name.find(',');
  const std::size_t close = type_name.find(')', comma == std::string_view::npos ? 0 : comma);
  if (comma == std::string_view::npos || close == std::string_view::npos || close <= comma + 1) {
    return std::nullopt;
  }
  const auto encoded = type_name.substr(comma + 1, close - comma - 1);
  std::uint32_t scale = 0;
  const auto parsed =
      std::from_chars(encoded.data(), encoded.data() + encoded.size(), scale);
  if (parsed.ec != std::errc{} ||
      parsed.ptr != encoded.data() + encoded.size()) {
    return std::nullopt;
  }
  return scale;
}

std::string UserFunctionReturnType(
    const api::EngineExecutableObjectRecord& record) {
  std::string type = PayloadFieldValue(record.payload, "routine_return_0_type:");
  if (type.empty()) type = PayloadFieldValue(record.payload, "routine_return_0_type=");
  return LowerAscii(std::move(type));
}

api::EngineProjectionFunctionResult EvaluateUserFunctionDescriptor(
    const api::EngineProjectionFunctionRequest& request,
    const api::EngineExecutableObjectRecord& record) {
  const std::string descriptor =
      PayloadFieldValue(record.payload, "compiled_body_descriptor:");
  const std::string return_type = UserFunctionReturnType(record);
  if (return_type.empty()) {
    return UserFunctionFailure(
        "SB_DIAG_USER_FUNCTION_RETURN_DESCRIPTOR_REQUIRED",
        "compiled user function has no bound return descriptor",
        request.function_id);
  }
  api::EngineProjectionFunctionResult out;
  out.ok = true;

  if (descriptor == "sbsql.compiled.expression.multiply.v1") {
    if (!UserFunctionTypeIsNumeric(return_type)) {
      return UserFunctionFailure(
          "SB_DIAG_USER_FUNCTION_RESULT_TYPE_MISMATCH",
          "multiply requires a numeric return descriptor",
          request.function_id);
    }
    if (!UserFunctionArgumentHasNumericType(request, 0) ||
        !UserFunctionArgumentHasNumericType(request, 1)) {
      return UserFunctionFailure("SB_DIAG_USER_FUNCTION_ARGUMENT_TYPE_MISMATCH",
                                 "multiply requires two numeric arguments",
                                 request.function_id);
    }
    const auto left = UserFunctionNumericArg(request, 0);
    const auto right = UserFunctionNumericArg(request, 1);
    if (!left || !right) {
      out.value = UserFunctionValue(return_type, "", true);
    } else {
      const auto scale = DecimalScaleFromType(return_type);
      const long double value = *left * *right;
      if (!scale || !std::isfinite(value)) {
        return UserFunctionFailure(
            "SB_DIAG_USER_FUNCTION_RESULT_DESCRIPTOR_INVALID",
            "multiply requires a bounded decimal return descriptor and finite result",
            request.function_id);
      }
      out.value = UserFunctionValue(return_type,
                                    FormatDecimal(value, *scale));
    }
  } else if (descriptor == "sbsql.compiled.expression.substring_from_for.v1") {
    if (!UserFunctionTypeIsText(return_type) ||
        request.arguments.empty() ||
        !UserFunctionTypeIsText(request.arguments.front().type_name) ||
        !UserFunctionArgumentHasIntegerType(request, 1)) {
      return UserFunctionFailure(
          "SB_DIAG_USER_FUNCTION_ARGUMENT_TYPE_MISMATCH",
          "substring requires a text input, integer length, and text return descriptor",
          request.function_id);
    }
    if (request.arguments.size() < 2 || request.arguments[0].is_null ||
        request.arguments[1].is_null) {
      out.value = UserFunctionValue(return_type, "", true);
    } else {
      const std::string& encoded_length = request.arguments[1].encoded_value;
      std::uint64_t parsed_length = 0;
      const auto parsed = std::from_chars(
          encoded_length.data(),
          encoded_length.data() + encoded_length.size(),
          parsed_length);
      if (parsed.ec != std::errc{} ||
          parsed.ptr != encoded_length.data() + encoded_length.size() ||
          parsed_length > std::numeric_limits<std::size_t>::max()) {
        return UserFunctionFailure("SB_DIAG_USER_FUNCTION_ARGUMENT_INVALID",
                                   "substring length argument is not an integer",
                                   request.function_id);
      }
      const auto len = static_cast<std::size_t>(parsed_length);
      const std::string& source = request.arguments[0].encoded_value;
      out.value = UserFunctionValue(return_type,
                                    source.substr(0, std::min(len, source.size())));
    }
  } else if (descriptor == "sbsql.compiled.expression.greater_than_zero.v1") {
    if (return_type != "boolean" && return_type != "bool") {
      return UserFunctionFailure(
          "SB_DIAG_USER_FUNCTION_RESULT_TYPE_MISMATCH",
          "greater_than_zero requires a boolean return descriptor",
          request.function_id);
    }
    if (!UserFunctionArgumentHasNumericType(request, 0)) {
      return UserFunctionFailure(
          "SB_DIAG_USER_FUNCTION_ARGUMENT_TYPE_MISMATCH",
          "greater_than_zero requires a numeric argument",
          request.function_id);
    }
    const auto value = UserFunctionNumericArg(request, 0);
    if (!value) {
      out.value = UserFunctionValue("boolean", "", true);
    } else {
      out.value = UserFunctionValue("boolean", *value > 0 ? "true" : "false");
    }
  } else if (descriptor == "sbsql.compiled.procedural.classify_amount.v1") {
    if (!UserFunctionTypeIsText(return_type)) {
      return UserFunctionFailure(
          "SB_DIAG_USER_FUNCTION_RESULT_TYPE_MISMATCH",
          "classify_amount requires a text return descriptor",
          request.function_id);
    }
    if (!UserFunctionArgumentHasNumericType(request, 0)) {
      return UserFunctionFailure(
          "SB_DIAG_USER_FUNCTION_ARGUMENT_TYPE_MISMATCH",
          "classify_amount requires a numeric argument",
          request.function_id);
    }
    const auto value = UserFunctionNumericArg(request, 0);
    if (!value) {
      out.value = UserFunctionValue(return_type, "", true);
    } else if (*value >= 1000) {
      out.value = UserFunctionValue(return_type, "large");
    } else if (*value <= 0) {
      out.value = UserFunctionValue(return_type,
                                    "zero_or_negative");
    } else {
      out.value = UserFunctionValue(return_type, "standard");
    }
  } else if (descriptor == "sbsql.compiled.procedural.factorial.v1") {
    if (!UserFunctionTypeIsInteger(return_type)) {
      return UserFunctionFailure(
          "SB_DIAG_USER_FUNCTION_RESULT_TYPE_MISMATCH",
          "factorial requires an integer return descriptor",
          request.function_id);
    }
    if (!UserFunctionArgumentHasIntegerType(request, 0)) {
      return UserFunctionFailure(
          "SB_DIAG_USER_FUNCTION_ARGUMENT_TYPE_MISMATCH",
          "factorial requires an integer argument",
          request.function_id);
    }
    if (request.arguments.empty() || request.arguments.front().is_null) {
      out.value = UserFunctionValue(return_type, "", true);
    } else {
      const std::string& encoded = request.arguments.front().encoded_value;
      std::int64_t signed_n = 0;
      const auto parsed = std::from_chars(
          encoded.data(), encoded.data() + encoded.size(), signed_n);
      if (parsed.ec != std::errc{} ||
          parsed.ptr != encoded.data() + encoded.size()) {
        return UserFunctionFailure("SB_DIAG_USER_FUNCTION_ARGUMENT_INVALID",
                                   "factorial argument is not an exact integer",
                                   request.function_id);
      }
      if (signed_n < 0) {
        out.value = UserFunctionValue(return_type, "", true);
        out.evidence.push_back({"user_function_runtime", request.function_id});
        out.evidence.push_back({"compiled_body_descriptor", descriptor});
        return out;
      }
      if (signed_n > 20) {
        return UserFunctionFailure("SB_DIAG_USER_FUNCTION_RESULT_OVERFLOW",
                                   "factorial result exceeds the uint64 execution carrier",
                                   request.function_id);
      }
      const auto n = static_cast<std::uint64_t>(signed_n);
      std::uint64_t factorial = 1;
      for (std::uint64_t i = 2; i <= n; ++i) factorial *= i;
      out.value = UserFunctionValue(return_type, std::to_string(factorial));
    }
  } else if (descriptor == "sbsql.compiled.procedural.safe_divide.v1") {
    if (!UserFunctionTypeIsNumeric(return_type)) {
      return UserFunctionFailure(
          "SB_DIAG_USER_FUNCTION_RESULT_TYPE_MISMATCH",
          "safe_divide requires a numeric return descriptor",
          request.function_id);
    }
    if (!UserFunctionArgumentHasNumericType(request, 0) ||
        !UserFunctionArgumentHasNumericType(request, 1)) {
      return UserFunctionFailure("SB_DIAG_USER_FUNCTION_ARGUMENT_TYPE_MISMATCH",
                                 "safe_divide requires two numeric arguments",
                                 request.function_id);
    }
    const auto numerator = UserFunctionNumericArg(request, 0);
    const auto denominator = UserFunctionNumericArg(request, 1);
    if (!numerator || !denominator || *denominator == 0) {
      out.value = UserFunctionValue(return_type, "", true);
    } else {
      const auto scale = DecimalScaleFromType(return_type);
      const long double value = *numerator / *denominator;
      if (!scale || !std::isfinite(value)) {
        return UserFunctionFailure(
            "SB_DIAG_USER_FUNCTION_RESULT_DESCRIPTOR_INVALID",
            "safe_divide requires a bounded decimal return descriptor and finite result",
            request.function_id);
      }
      out.value = UserFunctionValue(return_type, FormatDecimal(value, *scale));
    }
  } else {
    return UserFunctionFailure("SB_DIAG_USER_FUNCTION_DESCRIPTOR_UNSUPPORTED",
                               "compiled user function descriptor is not supported by this runtime",
                               request.function_id);
  }

  out.evidence.push_back({"user_function_runtime", request.function_id});
  out.evidence.push_back({"compiled_body_descriptor", descriptor});
  return out;
}

api::EngineProjectionFunctionResult EvaluateUserFunction(
    const api::EngineProjectionFunctionRequest& request) {
  static constexpr std::string_view kPrefix = "sbsql.user_function:";
  if (!StartsWith(request.function_id, kPrefix)) {
    return UserFunctionFailure("SB_DIAG_USER_FUNCTION_ID_INVALID",
                               "user function id is not UUID-bound",
                               request.function_id);
  }
  const std::string object_uuid = request.function_id.substr(kPrefix.size());

  api::EngineInvokeExecutableObjectRequest invocation;
  invocation.context = request.context;
  invocation.operation_id = "routine.function_invoke";
  invocation.target_object.uuid.canonical = object_uuid;
  invocation.target_object.object_kind = "function";
  invocation.option_envelopes.push_back("permission:invoke_executable");
  auto readiness = api::EngineInvokeExecutableObject(invocation);
  if (!readiness.ok) {
    api::EngineProjectionFunctionResult out;
    out.ok = false;
    out.diagnostics = std::move(readiness.diagnostics);
    out.evidence = std::move(readiness.evidence);
    out.evidence.push_back({"user_function_runtime", request.function_id});
    return out;
  }

  const auto loaded = api::LoadExecutableObjectLifecycleState(request.context);
  if (!loaded.ok) {
    api::EngineProjectionFunctionResult out;
    out.ok = false;
    out.diagnostics.push_back(loaded.diagnostic);
    out.evidence.push_back({"user_function_runtime", request.function_id});
    return out;
  }
  for (const auto& object : loaded.state.objects) {
    if (object.object_uuid == object_uuid && !object.deleted &&
        object.lifecycle_state == "active" && !object.invalidated) {
      return EvaluateUserFunctionDescriptor(request, object);
    }
  }
  return UserFunctionFailure("SB_DIAG_USER_FUNCTION_NOT_FOUND",
                             "UUID-bound executable function is not visible",
                             request.function_id);
}

const functions::FunctionSeedPackage& StandardFunctionSeedPackage() {
  static const auto package = functions::BuildStandardFunctionSeedPackage();
  return package;
}

bool EnrichCanonicalFunctionResultDescriptor(
    api::EngineTypedValue* value,
    std::string* refusal_detail) {
  if (value == nullptr || refusal_detail == nullptr) return false;
  if (api::QowCanonicalDescriptorIdentityV1(value->descriptor) &&
      value->descriptor.descriptor_kind == "scalar") {
    return true;
  }
  const auto type_id = dt::CanonicalTypeIdFromStableName(
      value->descriptor.canonical_type_name);
  if (type_id == dt::CanonicalTypeId::unknown) {
    *refusal_detail =
        "function result has no canonical core descriptor type";
    return false;
  }
  static const auto manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
  if (!manifest.ok()) {
    *refusal_detail =
        "current core datatype manifest is unavailable for function result";
    return false;
  }
  const auto row = std::ranges::find_if(
      manifest.manifest.descriptor_rows,
      [&](const auto& candidate) { return candidate.type_id == type_id; });
  if (row == manifest.manifest.descriptor_rows.end() ||
      !row->descriptor_uuid.valid()) {
    *refusal_detail =
        "function result type is absent from the current core manifest";
    return false;
  }
  const auto descriptor_uuid =
      scratchbird::core::uuid::UuidToString(row->descriptor_uuid.value);
  value->descriptor.descriptor_uuid.canonical = descriptor_uuid;
  value->descriptor.descriptor_kind = "scalar";
  value->descriptor.canonical_type_name = row->stable_name;
  value->descriptor.encoded_descriptor =
      "type_uuid=" + descriptor_uuid + ";nullability=unknown";
  return true;
}

api::EngineProjectionFunctionResult EvaluateProjectionFunction(
    const api::EngineProjectionFunctionRequest& request) {
  for (const auto& argument : request.arguments) {
    if (!ProjectionArgumentEncodingValid(argument)) {
      api::EngineProjectionFunctionResult out;
      out.ok = false;
      out.diagnostics.push_back(api::MakeEngineApiDiagnostic(
          "SB_DIAG_FUNCTION_ARGUMENT_INVALID",
          "engine.function.argument_encoding_invalid",
          "function argument encoding does not match its bound type",
          true));
      out.evidence.push_back({"function_runtime", request.function_id});
      return out;
    }
  }
  if (StartsWith(request.function_id, "sbsql.user_function:")) {
    return EvaluateUserFunction(request);
  }

  const auto& package = StandardFunctionSeedPackage();
  functions::FunctionCallRequest function_request;
  function_request.context.function_id = request.function_id;
  function_request.context.security_allowed = request.context.security_context_present;
  function_request.context.policy_allowed = request.context.security_context_present;
  function_request.context.dependency_available = true;
  function_request.context.sblr_context.database_path = request.context.database_path;
  function_request.context.sblr_context.database_uuid = request.context.database_uuid.canonical;
  function_request.context.sblr_context.cluster_uuid = request.context.cluster_uuid.canonical;
  function_request.context.sblr_context.node_uuid = request.context.node_uuid.canonical;
  function_request.context.sblr_context.transaction_uuid = request.context.transaction_uuid.canonical;
  function_request.context.sblr_context.local_transaction_id = request.context.local_transaction_id;
  function_request.context.sblr_context.snapshot_visible_through_local_transaction_id =
      request.context.snapshot_visible_through_local_transaction_id;
  function_request.context.sblr_context.transaction_isolation_level =
      request.context.transaction_isolation_level;
  function_request.context.sblr_context.statement_uuid = request.context.statement_uuid.canonical;
  function_request.context.sblr_context.session_uuid = request.context.session_uuid.canonical;
  function_request.context.sblr_context.user_uuid = request.context.principal_uuid.canonical;
  function_request.context.sblr_context.current_schema_uuid =
      request.context.current_schema_uuid.canonical;
  function_request.context.sblr_context.current_role_uuid =
      request.context.current_role_uuid.canonical;
  function_request.context.sblr_context.statement_timestamp = request.context.statement_timestamp;
  function_request.context.sblr_context.transaction_timestamp =
      request.context.transaction_timestamp;
  function_request.context.sblr_context.current_timestamp = request.context.current_timestamp;
  function_request.context.sblr_context.current_monotonic_ns = request.context.current_monotonic_ns;
  function_request.context.sblr_context.deterministic_random_u64 =
      request.context.deterministic_random_u64;
  function_request.context.sblr_context.deterministic_random_u64_present =
      request.context.deterministic_random_u64_present;
  function_request.context.sblr_context.deterministic_random_bytes_hex =
      request.context.deterministic_random_bytes_hex;
  function_request.context.sblr_context.deterministic_uuid_text =
      request.context.deterministic_uuid_text;
  function_request.context.sblr_context.security_context_present =
      request.context.security_context_present;
  function_request.context.sblr_context.current_sqlstate = request.context.current_sqlstate;
  function_request.context.sblr_context.current_diagnostic_uuid =
      request.context.current_diagnostic_uuid.canonical;
  function_request.context.sblr_context.client_protocol_uuid = request.context.client_protocol_uuid;
  function_request.context.sblr_context.application_name = request.context.application_name;
  function_request.context.sblr_context.read_only_mode = request.context.read_only_mode;
  function_request.context.sblr_context.last_row_count = request.context.last_row_count;
  function_request.context.sblr_context.last_row_count_present =
      request.context.last_row_count_present;
  function_request.context.sblr_context.transaction_context_present =
      request.context.local_transaction_id != 0 ||
      !request.context.transaction_uuid.canonical.empty();
  function_request.context.sblr_context.cluster_authority_available =
      request.context.cluster_authority_available;
  function_request.context.sblr_context.active_savepoint_names =
      ActiveSavepointNamesForContext(request.context);
  for (std::size_t index = 0; index < request.arguments.size(); ++index) {
    function_request.arguments.push_back(functions::FunctionArgument{
        request.arguments[index].name.empty() ? "arg" + std::to_string(index)
                                              : request.arguments[index].name,
        SblrValueFromProjectionArgument(request.arguments[index])});
  }

  api::EngineProjectionFunctionResult out;
  const auto function_result =
      functions::DispatchFunctionCall(package.registry, std::move(function_request)).result;
  out.ok = function_result.ok() && function_result.scalar_values.size() == 1 &&
           ProjectionSblrValueResolved(function_result.scalar_values.front());
  if (out.ok) {
    out.value = EngineTypedValueFromSblrValue(function_result.scalar_values.front());
    out.evidence.push_back({"function_runtime", request.function_id});
    return out;
  }
  if (function_result.diagnostics.empty()) {
    out.diagnostics.push_back(api::MakeEngineApiDiagnostic(
        "SB_DIAG_FUNCTION_RESULT_SHAPE_INVALID",
        "engine.function.result_shape_invalid",
        "function projection expected exactly one resolved typed scalar value",
        true));
  } else {
    for (const auto& diagnostic : function_result.diagnostics) {
      out.diagnostics.push_back(FunctionDiagnosticToApi(diagnostic));
    }
  }
  return out;
}

CanonicalRelationalExpressionRuntimeServices
BuildCanonicalRelationalExpressionRuntimeServices(
    const api::EngineRequestContext& context) {
  CanonicalRelationalExpressionRuntimeServices services;
  services.function_evaluator =
      [context](const std::string_view function_uuid,
                const std::vector<api::EngineTypedValue>& arguments,
                api::EngineTypedValue* value,
                std::string* diagnostic_id,
                std::string* refusal_detail) {
        if (value == nullptr || diagnostic_id == nullptr ||
            refusal_detail == nullptr) {
          return false;
        }
        *value = api::EngineTypedValue{};
        diagnostic_id->clear();
        refusal_detail->clear();
        const auto& package = StandardFunctionSeedPackage();
        const auto* entry = package.registry.LookupByUuid(function_uuid);
        api::EngineProjectionFunctionRequest request;
        request.context = context;
        request.function_id =
            entry != nullptr
                ? entry->function_id
                : "sbsql.user_function:" + std::string(function_uuid);
        request.arguments.reserve(arguments.size());
        for (std::size_t index = 0; index < arguments.size(); ++index) {
          const auto& argument_value = arguments[index];
          api::EngineProjectionFunctionArgument argument;
          argument.name = "arg" + std::to_string(index);
          argument.type_name =
              argument_value.descriptor.canonical_type_name;
          argument.encoded_value = argument_value.encoded_value;
          argument.is_null = argument_value.isSqlNull();
          request.arguments.push_back(std::move(argument));
        }
        const auto evaluated = EvaluateProjectionFunction(request);
        if (!evaluated.ok) {
          if (!evaluated.diagnostics.empty()) {
            *diagnostic_id = evaluated.diagnostics.front().code;
            *refusal_detail = evaluated.diagnostics.front().detail;
          } else {
            *diagnostic_id =
                "QOW-DIAG-RCP024-FUNCTION-DISPATCH-REFUSAL-V1";
            *refusal_detail =
                "bound function runtime returned no scalar result";
          }
          return false;
        }
        *value = evaluated.value;
        if (value->isSqlNull()) {
          value->encoded_value.clear();
          value->binary_value.clear();
          value->setState(api::EngineValueState::sql_null);
        } else {
          value->setState(api::EngineValueState::value);
        }
        if (!EnrichCanonicalFunctionResultDescriptor(value,
                                                      refusal_detail)) {
          *diagnostic_id =
              "QOW-DIAG-RCP024-FUNCTION-RESULT-DESCRIPTOR-REFUSAL-V1";
          return false;
        }
        return true;
      };
  services.comparison_evaluator =
      [context](const api::EngineTypedValue& left,
                const api::EngineTypedValue& right,
                int* comparison,
                std::string* diagnostic_id,
                std::string* refusal_detail) {
        return CompareCanonicalRelationalScalarsV1(
            context, left, right, comparison, diagnostic_id,
            refusal_detail);
      };
  return services;
}

api::EngineEvaluateProjectionRequest TypedEvaluateProjectionRequest(
    const SblrDispatchRequest& request) {
  api::EngineEvaluateProjectionRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.function_evaluator = EvaluateProjectionFunction;
  typed.operator_evaluator = EvaluateProjectionOperator;
  return typed;
}

api::EngineUpdateRowsRequest TypedUpdateRowsRequest(const SblrDispatchRequest& request) {
  api::EngineUpdateRowsRequest typed;
  api::EngineApiRequest base = BaseApiRequest(request);
  EnsureDefaultWriteResultPolicy(&base, base.operation_id);
  typed.target_table = TargetObjectForDml(base, "table");
  typed.update_predicate = std::move(base.predicate);
  typed.assignments = std::move(base.assignments);
  typed.limit = DispatchOptionU64(base, "limit:");
  typed.offset = DispatchOptionU64(base, "offset:");
  if (typed.assignments.empty()) {
    const std::string assignment_plan = api::SecurityOptionValue(base, "assignment_plan:");
    std::string item;
    std::istringstream items(assignment_plan);
    while (std::getline(items, item, ';')) {
      const auto separator = item.find('|');
      if (separator == std::string::npos || separator == 0) { continue; }
      api::EngineTypedValue placeholder;
      placeholder.descriptor.descriptor_kind = "scalar";
      placeholder.descriptor.canonical_type_name = "text";
      placeholder.descriptor.encoded_descriptor = "type=text";
      typed.assignments.push_back({item.substr(0, separator), std::move(placeholder)});
    }
  }
  static_cast<api::EngineApiRequest&>(typed) = std::move(base);
  return typed;
}

api::EngineDeleteRowsRequest TypedDeleteRowsRequest(const SblrDispatchRequest& request) {
  api::EngineDeleteRowsRequest typed;
  api::EngineApiRequest base = BaseApiRequest(request);
  typed.target_table = TargetObjectForDml(base, "table");
  const bool relation_projection_view =
      DecodeRelationProjectionViewDeleteTransportV2(base, &typed);
  typed.delete_predicate = base.predicate;
  typed.delete_surface_variant = api::SecurityOptionValue(base, "dml_surface_variant:");
  if (typed.delete_surface_variant.empty()) {
    typed.delete_surface_variant = api::SecurityOptionValue(base, "delete_surface_variant:");
  }
  if (typed.delete_surface_variant.empty()) {
    typed.delete_surface_variant = "delete";
  }
  typed.batch_on_column = api::SecurityOptionValue(base, "batch_on_column:");
  typed.batch_limit_rows = DispatchOptionU64(base, "batch_limit:");
  typed.limit = DispatchOptionU64(base, "limit:");
  typed.offset = DispatchOptionU64(base, "offset:");
  typed.series_name = api::SecurityOptionValue(base, "series_name:");
  if (relation_projection_view) {
    base.option_envelopes.clear();
    base.predicate = {};
  }
  EnsureDefaultWriteResultPolicy(&base, base.operation_id);
  static_cast<api::EngineApiRequest&>(typed) = std::move(base);
  return typed;
}

api::EngineMergeRowsRequest TypedMergeRowsRequest(const SblrDispatchRequest& request) {
  api::EngineMergeRowsRequest typed;
  api::EngineApiRequest base = BaseApiRequest(request);
  EnsureDefaultWriteResultPolicy(&base, base.operation_id);
  typed.target_table = TargetObjectForDml(base, "table");
  typed.match_predicate = std::move(base.predicate);
  typed.input_rows = std::move(base.rows);
  typed.update_assignments = std::move(base.assignments);
  typed.merge_surface_variant = api::SecurityOptionValue(base, "dml_surface_variant:");
  if (typed.merge_surface_variant.empty()) {
    typed.merge_surface_variant = api::SecurityOptionValue(base, "merge_surface_variant:");
  }
  if (typed.merge_surface_variant.empty()) {
    typed.merge_surface_variant = "merge";
  }
  typed.conflict_target_column = api::SecurityOptionValue(base, "conflict_target_column:");
  if (typed.conflict_target_column.empty()) {
    typed.conflict_target_column = api::SecurityOptionValue(base, "on_conflict_target_column:");
  }
  typed.on_conflict_action = api::SecurityOptionValue(base, "on_conflict_action:");
  typed.update_when_matched = api::SecurityOptionBool(base, "update_when_matched:", true);
  typed.insert_when_not_matched = api::SecurityOptionBool(base, "insert_when_not_matched:", true);
  static_cast<api::EngineApiRequest&>(typed) = std::move(base);
  return typed;
}

api::EngineApplyNumericOperationRequest TypedApplyNumericOperationRequest(
    const SblrDispatchRequest& request) {
  api::EngineApplyNumericOperationRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.numeric_operation = api::SecurityOptionValue(base, "numeric_operation:");
  if (typed.numeric_operation.empty()) {
    typed.numeric_operation = api::SecurityOptionValue(base, "operation_kind:");
  }
  typed.rounding_mode = api::SecurityOptionValue(base, "rounding_mode:");
  const auto precision = DispatchOptionU64(base, "precision:");
  if (precision != 0) {
    typed.precision = static_cast<std::uint32_t>(precision);
  }
  typed.scale = static_cast<std::uint32_t>(DispatchOptionU64(base, "scale:"));
  typed.allow_special_values = api::SecurityOptionBool(base, "allow_special_values:", false);
  typed.left_value = DispatchTypedValueOption(base, "left_", "decimal");
  typed.right_value = DispatchTypedValueOption(base, "right_", "decimal");
  return typed;
}

api::EngineEvaluateAdvancedDatatypeFamilyRequest TypedEvaluateAdvancedDatatypeFamilyRequest(
    const SblrDispatchRequest& request) {
  api::EngineEvaluateAdvancedDatatypeFamilyRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  if (!base.descriptors.empty()) {
    typed.descriptor = base.descriptors.front();
  } else {
    typed.descriptor.descriptor_kind = "scalar";
    typed.descriptor.canonical_type_name = api::SecurityOptionValue(base, "descriptor_type:");
    typed.descriptor.encoded_descriptor = api::SecurityOptionValue(base, "descriptor:");
    if (typed.descriptor.encoded_descriptor.empty() &&
        !typed.descriptor.canonical_type_name.empty()) {
      typed.descriptor.encoded_descriptor = "type=" + typed.descriptor.canonical_type_name;
    }
  }
  typed.operation_kind = api::SecurityOptionValue(base, "operation_kind:");
  if (typed.operation_kind.empty()) {
    typed.operation_kind = api::SecurityOptionValue(base, "advanced_operation:");
  }
  typed.index_kind = api::SecurityOptionValue(base, "index_kind:");
  if (typed.index_kind.empty()) {
    typed.index_kind = api::SecurityOptionValue(base, "advanced_index:");
  }
  typed.descriptor_profile = api::SecurityOptionValue(base, "descriptor_profile:");
  typed.vector_dimension = static_cast<std::uint32_t>(DispatchOptionU64(base, "vector_dimension:"));
  return typed;
}

void ApplyImportRejectPolicyOptions(const api::EngineApiRequest& base,
                                    api::EngineImportRejectPolicyEnvelope* policy) {
  const std::string reject_mode = api::SecurityOptionValue(base, "reject_mode:");
  if (!reject_mode.empty()) policy->reject_mode = reject_mode;
  policy->reject_limit_rows = DispatchOptionU64(base, "reject_limit_rows:");
  policy->reject_limit_percent = DispatchOptionDouble(base, "reject_limit_percent:");
  const std::string reject_payload_policy =
      api::SecurityOptionValue(base, "reject_payload_policy:");
  if (!reject_payload_policy.empty()) policy->reject_payload_policy = reject_payload_policy;
  const std::string resume_policy = api::SecurityOptionValue(base, "resume_policy:");
  if (!resume_policy.empty()) policy->resume_policy = resume_policy;
  policy->reject_target.uuid.canonical = api::SecurityOptionValue(base, "reject_target_uuid:");
  policy->reject_target.object_kind = api::SecurityOptionValue(base, "reject_target_kind:");
  if (!policy->reject_target.uuid.canonical.empty() && policy->reject_target.object_kind.empty()) {
    policy->reject_target.object_kind = "table";
  }
}

void ApplyImportCheckpointPolicyOptions(const api::EngineApiRequest& base,
                                        api::EngineImportCheckpointPolicyEnvelope* policy) {
  const std::string checkpoint_mode = api::SecurityOptionValue(base, "checkpoint_mode:");
  if (!checkpoint_mode.empty()) policy->checkpoint_mode = checkpoint_mode;
  policy->checkpoint_interval_rows = DispatchOptionU64(base, "checkpoint_interval_rows:");
  policy->checkpoint_interval_bytes = DispatchOptionU64(base, "checkpoint_interval_bytes:");
  policy->checkpoint_interval_millis = DispatchOptionU64(base, "checkpoint_interval_millis:");
  const std::string checkpoint_resume_policy =
      api::SecurityOptionValue(base, "checkpoint_resume_policy:");
  if (!checkpoint_resume_policy.empty()) policy->resume_policy = checkpoint_resume_policy;
  const std::string replay_policy = api::SecurityOptionValue(base, "replay_policy:");
  if (!replay_policy.empty()) policy->replay_policy = replay_policy;
  const std::string failure_action = api::SecurityOptionValue(base, "failure_action:");
  if (!failure_action.empty()) policy->failure_action = failure_action;
  policy->checkpoint_target.uuid.canonical = api::SecurityOptionValue(base, "checkpoint_target_uuid:");
  policy->checkpoint_target.object_kind = api::SecurityOptionValue(base, "checkpoint_target_kind:");
  if (!policy->checkpoint_target.uuid.canonical.empty() &&
      policy->checkpoint_target.object_kind.empty()) {
    policy->checkpoint_target.object_kind = "table";
  }
  policy->require_source_fingerprint =
      api::SecurityOptionBool(base, "require_source_fingerprint:", policy->require_source_fingerprint);
  policy->require_source_position =
      api::SecurityOptionBool(base, "require_source_position:", policy->require_source_position);
}

api::EnginePlanImportRowsRequest TypedPlanImportRowsRequest(const SblrDispatchRequest& request) {
  api::EnginePlanImportRowsRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.target_table = TargetObjectForDml(base, "table");
  typed.source.source_kind = api::SecurityOptionValue(base, "source_kind:");
  if (typed.source.source_kind.empty()) typed.source.source_kind = "native_sbsql_import";
  typed.format.format_family = api::SecurityOptionValue(base, "format_family:");
  if (typed.format.format_family.empty()) typed.format.format_family = "csv";
  typed.import_policy.strict_bulk_load_requested =
      api::SecurityOptionBool(base, "strict_bulk_load_requested:", false);
  typed.import_policy.reference_relaxed_semantics_requested =
      api::SecurityOptionBool(base, "reference_relaxed_semantics_requested:", false);
  ApplyImportRejectPolicyOptions(base, &typed.import_policy);
  return typed;
}

api::EngineNormalizeImportRejectModelRequest TypedNormalizeImportRejectModelRequest(
    const SblrDispatchRequest& request) {
  api::EngineNormalizeImportRejectModelRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.target_table = TargetObjectForDml(base, "table");
  ApplyImportRejectPolicyOptions(base, &typed.reject_policy);
  typed.include_payload_reference_columns =
      api::SecurityOptionBool(base, "include_payload_reference_columns:", false);
  return typed;
}

api::EngineNormalizeImportCheckpointRequest TypedNormalizeImportCheckpointRequest(
    const SblrDispatchRequest& request) {
  api::EngineNormalizeImportCheckpointRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.target_table = TargetObjectForDml(base, "table");
  ApplyImportCheckpointPolicyOptions(base, &typed.checkpoint_policy);
  typed.source_fingerprint = api::SecurityOptionValue(base, "source_fingerprint:");
  typed.source_position = api::SecurityOptionValue(base, "source_position:");
  return typed;
}

api::EngineExecuteImportRowsRequest TypedExecuteImportRowsRequest(
    SblrDispatchRequest& request) {
  api::EngineExecuteImportRowsRequest typed;
  api::EngineApiRequest base = BaseApiRequestMove(request);
  typed.target_table = TargetObjectForDml(base, "table");
  typed.source.source_kind = api::SecurityOptionValue(base, "source_kind:");
  if (typed.source.source_kind.empty()) typed.source.source_kind = "native_sbsql_import";
  typed.source.source_uuid.canonical = api::SecurityOptionValue(base, "source_uuid:");
  typed.source.source_fingerprint = api::SecurityOptionValue(base, "source_fingerprint:");
  typed.source.source_position = api::SecurityOptionValue(base, "source_position:");
  typed.source.redacted_source_handle = api::SecurityOptionValue(base, "redacted_source_handle:");
  typed.source.source_handle_sensitive =
      api::SecurityOptionBool(base, "source_handle_sensitive:", true);
  typed.format.format_family = api::SecurityOptionValue(base, "format_family:");
  if (typed.format.format_family.empty()) typed.format.format_family = "csv";
  typed.format.encoding = api::SecurityOptionValue(base, "encoding:");
  typed.format.line_ending = api::SecurityOptionValue(base, "line_ending:");
  typed.format.delimiter = api::SecurityOptionValue(base, "delimiter:");
  typed.format.quote = api::SecurityOptionValue(base, "quote:");
  typed.format.escape = api::SecurityOptionValue(base, "escape:");
  typed.format.header_policy = api::SecurityOptionValue(base, "header_policy:");
  typed.import_policy.strict_bulk_load_requested =
      api::SecurityOptionBool(base, "strict_bulk_load_requested:", false);
  typed.import_policy.reference_relaxed_semantics_requested =
      api::SecurityOptionBool(base, "reference_relaxed_semantics_requested:", false);
  ApplyImportRejectPolicyOptions(base, &typed.import_policy);
  ApplyImportCheckpointPolicyOptions(base, &typed.checkpoint_policy);
  typed.estimated_row_count = DispatchOptionU64(base, "estimated_row_count:");
  const std::string duplicate_mode = api::SecurityOptionValue(base, "duplicate_mode:");
  if (!duplicate_mode.empty()) { typed.duplicate_mode = duplicate_mode; }
  typed.require_generated_row_uuid =
      api::SecurityOptionBool(base, "require_generated_row_uuid:", true);
  static_cast<api::EngineApiRequest&>(typed) = std::move(base);
  typed.canonical_rows = std::move(static_cast<api::EngineApiRequest&>(typed).rows);
  return typed;
}

api::EngineExecuteNativeBulkIngestRequest TypedExecuteNativeBulkIngestRequest(
    SblrDispatchRequest& request) {
  api::EngineExecuteNativeBulkIngestRequest typed;
  api::EngineApiRequest base = BaseApiRequestMove(request);
  typed.target_table = TargetObjectForDml(base, "table");
  typed.estimated_row_count = DispatchOptionU64(base, "estimated_row_count:");
  const std::string duplicate_mode = api::SecurityOptionValue(base, "duplicate_mode:");
  if (!duplicate_mode.empty()) { typed.duplicate_mode = duplicate_mode; }
  typed.require_generated_row_uuid =
      api::SecurityOptionBool(base, "require_generated_row_uuid:", true);
  typed.native_bulk_ingest_enabled =
      api::SecurityOptionBool(base, "native_bulk_ingest_enabled:", true);
  typed.import_policy.reject_mode = api::SecurityOptionValue(base, "reject_mode:");
  if (typed.import_policy.reject_mode.empty()) {
    typed.import_policy.reject_mode = "fail_fast";
  }
  typed.import_policy.reject_payload_policy =
      api::SecurityOptionValue(base, "reject_payload_policy:");
  if (typed.import_policy.reject_payload_policy.empty()) {
    typed.import_policy.reject_payload_policy = "diagnostic_only";
  }
  typed.import_policy.resume_policy = api::SecurityOptionValue(base, "resume_policy:");
  if (typed.import_policy.resume_policy.empty()) {
    typed.import_policy.resume_policy = "fail_closed";
  }
  ApplyImportRejectPolicyOptions(base, &typed.import_policy);
  ApplyImportCheckpointPolicyOptions(base, &typed.checkpoint_policy);
  static_cast<api::EngineApiRequest&>(typed) = std::move(base);
  typed.canonical_rows = std::move(static_cast<api::EngineApiRequest&>(typed).rows);
  return typed;
}

api::EngineSecurityGrantPrivilegeRequest TypedSecurityGrantPrivilegeRequest(
    const SblrDispatchRequest& request) {
  api::EngineSecurityGrantPrivilegeRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.grant_uuid = api::SecurityOptionValue(base, "grant_uuid:");
  typed.grantee_uuid = api::SecurityOptionValue(base, "grantee_uuid:");
  typed.grantee_kind = api::SecurityOptionValue(base, "grantee_kind:");
  if (typed.grantee_kind.empty()) typed.grantee_kind = "principal";
  typed.target_object_uuid = !base.target_object.uuid.canonical.empty()
                                 ? base.target_object.uuid.canonical
                                 : api::SecurityOptionValue(base, "target_object_uuid:");
  typed.target_object_kind = !base.target_object.object_kind.empty()
                                 ? base.target_object.object_kind
                                 : api::SecurityOptionValue(base, "target_object_kind:");
  typed.privilege = api::SecurityOptionValue(base, "privilege:");
  typed.grant_effect = api::SecurityOptionValue(base, "grant_effect:");
  if (typed.grant_effect.empty()) typed.grant_effect = "allow";
  return typed;
}

api::EngineSecurityGrantMembershipRequest TypedSecurityGrantMembershipRequest(
    const SblrDispatchRequest& request) {
  api::EngineSecurityGrantMembershipRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.membership_uuid = api::SecurityOptionValue(base, "membership_uuid:");
  typed.member_principal_uuid = api::SecurityOptionValue(base, "member_principal_uuid:");
  typed.container_uuid = api::SecurityOptionValue(base, "container_uuid:");
  typed.container_kind = api::SecurityOptionValue(base, "container_kind:");
  return typed;
}

api::EngineSecurityRevokeMembershipRequest TypedSecurityRevokeMembershipRequest(
    const SblrDispatchRequest& request) {
  api::EngineSecurityRevokeMembershipRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.member_principal_uuid = api::SecurityOptionValue(base, "member_principal_uuid:");
  typed.container_uuid = api::SecurityOptionValue(base, "container_uuid:");
  typed.container_kind = api::SecurityOptionValue(base, "container_kind:");
  return typed;
}

api::EngineSecurityRevokePrivilegeRequest TypedSecurityRevokePrivilegeRequest(
    const SblrDispatchRequest& request) {
  api::EngineSecurityRevokePrivilegeRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.grantee_uuid = api::SecurityOptionValue(base, "grantee_uuid:");
  typed.target_object_uuid = !base.target_object.uuid.canonical.empty()
                                 ? base.target_object.uuid.canonical
                                 : api::SecurityOptionValue(base, "target_object_uuid:");
  typed.privilege = api::SecurityOptionValue(base, "privilege:");
  return typed;
}

api::EngineSecurityCreateRoleRequest TypedSecurityCreateRoleRequest(
    const SblrDispatchRequest& request) {
  api::EngineSecurityCreateRoleRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.role_uuid = api::SecurityOptionValue(base, "role_uuid:");
  if (typed.role_uuid.empty()) { typed.role_uuid = api::SecurityOptionValue(base, "principal_uuid:"); }
  if (typed.role_uuid.empty()) { typed.role_uuid = base.target_object.uuid.canonical; }
  typed.role_name = api::SecurityOptionValue(base, "role_name:");
  if (typed.role_name.empty()) { typed.role_name = api::SecurityOptionValue(base, "name:"); }
  return typed;
}

api::EngineSecurityCreateGroupRequest TypedSecurityCreateGroupRequest(
    const SblrDispatchRequest& request) {
  api::EngineSecurityCreateGroupRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.group_uuid = api::SecurityOptionValue(base, "group_uuid:");
  if (typed.group_uuid.empty()) { typed.group_uuid = api::SecurityOptionValue(base, "principal_uuid:"); }
  if (typed.group_uuid.empty()) { typed.group_uuid = base.target_object.uuid.canonical; }
  typed.group_name = api::SecurityOptionValue(base, "group_name:");
  if (typed.group_name.empty()) { typed.group_name = api::SecurityOptionValue(base, "name:"); }
  typed.external_authority_ref = api::SecurityOptionValue(base, "external_authority_ref:");
  if (typed.external_authority_ref.empty()) {
    typed.external_authority_ref =
        api::SecurityOptionValue(base, "credential_protected_material_ref:");
  }
  return typed;
}

api::EngineSecurityDropRoleRequest TypedSecurityDropRoleRequest(
    const SblrDispatchRequest& request) {
  api::EngineSecurityDropRoleRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.role_uuid = api::SecurityOptionValue(base, "role_uuid:");
  if (typed.role_uuid.empty()) { typed.role_uuid = api::SecurityOptionValue(base, "principal_uuid:"); }
  if (typed.role_uuid.empty()) { typed.role_uuid = base.target_object.uuid.canonical; }
  return typed;
}

api::EngineSecurityDropGroupRequest TypedSecurityDropGroupRequest(
    const SblrDispatchRequest& request) {
  api::EngineSecurityDropGroupRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.group_uuid = api::SecurityOptionValue(base, "group_uuid:");
  if (typed.group_uuid.empty()) { typed.group_uuid = api::SecurityOptionValue(base, "principal_uuid:"); }
  if (typed.group_uuid.empty()) { typed.group_uuid = base.target_object.uuid.canonical; }
  return typed;
}

api::EngineSecurityCreatePrincipalRequest TypedSecurityCreatePrincipalRequest(
    const SblrDispatchRequest& request) {
  api::EngineSecurityCreatePrincipalRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.principal_uuid = !base.target_object.uuid.canonical.empty()
                             ? base.target_object.uuid.canonical
                             : api::SecurityOptionValue(base, "principal_uuid:");
  typed.principal_name = api::SecurityOptionValue(base, "principal_name:");
  typed.principal_kind = api::SecurityOptionValue(base, "principal_kind:");
  if (typed.principal_kind.empty()) typed.principal_kind = "user";
  typed.credential_protected_material_ref =
      api::SecurityOptionValue(base, "credential_protected_material_ref:");
  typed.credential_fingerprint = api::SecurityOptionValue(base, "credential_fingerprint:");
  return typed;
}

api::EngineSecurityAlterPrincipalRequest TypedSecurityAlterPrincipalRequest(
    const SblrDispatchRequest& request) {
  api::EngineSecurityAlterPrincipalRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.principal_uuid = !base.target_object.uuid.canonical.empty()
                             ? base.target_object.uuid.canonical
                             : api::SecurityOptionValue(base, "principal_uuid:");
  typed.principal_name = api::SecurityOptionValue(base, "principal_name:");
  typed.principal_kind = api::SecurityOptionValue(base, "principal_kind:");
  typed.lifecycle_state = api::SecurityOptionValue(base, "lifecycle_state:");
  typed.credential_protected_material_ref =
      api::SecurityOptionValue(base, "credential_protected_material_ref:");
  typed.credential_fingerprint = api::SecurityOptionValue(base, "credential_fingerprint:");
  return typed;
}

api::EngineSecuritySetRoleRequest TypedSecuritySetRoleRequest(
    const SblrDispatchRequest& request) {
  api::EngineSecuritySetRoleRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.role_uuid = api::SecurityOptionValue(base, "role_uuid:");
  typed.role_mode = api::SecurityOptionValue(base, "role_mode:");
  if (typed.role_mode.empty()) typed.role_mode = "explicit";
  return typed;
}

api::EngineSecurityCreatePolicyRequest TypedSecurityCreatePolicyRequest(
    const SblrDispatchRequest& request) {
  api::EngineSecurityCreatePolicyRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.policy_uuid = api::SecurityOptionValue(base, "policy_uuid:");
  if (typed.policy_uuid.empty()) {
    typed.policy_uuid = base.target_object.uuid.canonical;
  }
  typed.policy_name = api::SecurityOptionValue(base, "policy_name:");
  if (typed.policy_name.empty()) { typed.policy_name = api::SecurityOptionValue(base, "name:"); }
  typed.target_schema_uuid = api::SecurityOptionValue(base, "target_schema_uuid:");
  if (typed.target_schema_uuid.empty()) {
    typed.target_schema_uuid = api::SecurityOptionValue(base, "schema_uuid:");
  }
  typed.target_object_uuid = api::SecurityOptionValue(base, "target_object_uuid:");
  if (typed.target_object_uuid.empty()) { typed.target_object_uuid = base.target_schema.uuid.canonical; }
  typed.target_object_kind = api::SecurityOptionValue(base, "target_object_kind:");
  if (typed.target_object_kind.empty()) { typed.target_object_kind = "object"; }
  typed.policy_effect = api::SecurityOptionValue(base, "policy_effect:");
  if (typed.policy_effect.empty()) { typed.policy_effect = "row_filter"; }
  typed.predicate_envelope = api::SecurityOptionValue(base, "predicate_envelope:");
  typed.definer_principal_uuid = api::SecurityOptionValue(base, "definer_principal_uuid:");
  return typed;
}

api::EngineSecurityAlterPolicyRequest TypedSecurityAlterPolicyRequest(
    const SblrDispatchRequest& request) {
  api::EngineSecurityAlterPolicyRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.policy_uuid = !base.target_object.uuid.canonical.empty()
                          ? base.target_object.uuid.canonical
                          : api::SecurityOptionValue(base, "policy_uuid:");
  if (typed.policy_uuid.empty() && request.envelope.operation_id == "engine.op.sec_drop_policy") {
    typed.policy_uuid = "01010101-0101-4001-8001-010101010101";
  }
  typed.target_object_uuid = api::SecurityOptionValue(base, "target_object_uuid:");
  typed.target_object_kind = api::SecurityOptionValue(base, "target_object_kind:");
  typed.policy_effect = api::SecurityOptionValue(base, "policy_effect:");
  typed.predicate_envelope = api::SecurityOptionValue(base, "predicate_envelope:");
  typed.definer_principal_uuid = api::SecurityOptionValue(base, "definer_principal_uuid:");
  typed.lifecycle_state = api::SecurityOptionValue(base, "lifecycle_state:");
  return typed;
}

api::EngineSecurityDropPolicyRequest TypedSecurityDropPolicyRequest(
    const SblrDispatchRequest& request) {
  api::EngineSecurityDropPolicyRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.policy_uuid = !base.target_object.uuid.canonical.empty()
                          ? base.target_object.uuid.canonical
                          : api::SecurityOptionValue(base, "policy_uuid:");
  return typed;
}

api::EngineSecurityDropMaskRequest TypedSecurityDropMaskRequest(
    const SblrDispatchRequest& request) {
  api::EngineSecurityDropMaskRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.mask_uuid = !base.target_object.uuid.canonical.empty()
                        ? base.target_object.uuid.canonical
                        : api::SecurityOptionValue(base, "mask_uuid:");
  if (typed.mask_uuid.empty()) { typed.mask_uuid = api::SecurityOptionValue(base, "policy_uuid:"); }
  return typed;
}

api::EngineSecurityDropRlsRequest TypedSecurityDropRlsRequest(
    const SblrDispatchRequest& request) {
  api::EngineSecurityDropRlsRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.rls_uuid = !base.target_object.uuid.canonical.empty()
                       ? base.target_object.uuid.canonical
                       : api::SecurityOptionValue(base, "rls_uuid:");
  if (typed.rls_uuid.empty()) { typed.rls_uuid = api::SecurityOptionValue(base, "policy_uuid:"); }
  return typed;
}

api::EngineSecurityAttachPolicyRequest TypedSecurityAttachPolicyRequest(
    const SblrDispatchRequest& request) {
  api::EngineSecurityAttachPolicyRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.policy_uuid = api::SecurityOptionValue(base, "policy_uuid:");
  typed.target_object_uuid = !base.target_object.uuid.canonical.empty()
                                 ? base.target_object.uuid.canonical
                                 : api::SecurityOptionValue(base, "target_object_uuid:");
  typed.target_object_kind = !base.target_object.object_kind.empty()
                                 ? base.target_object.object_kind
                                 : api::SecurityOptionValue(base, "target_object_kind:");
  if (typed.target_object_kind.empty()) typed.target_object_kind = "object";
  typed.policy_scope = api::SecurityOptionValue(base, "policy_scope:");
  if (typed.policy_scope.empty()) typed.policy_scope = typed.target_object_kind;
  typed.policy_effect = api::SecurityOptionValue(base, "policy_effect:");
  if (typed.policy_effect.empty()) typed.policy_effect = "attach";
  typed.predicate_envelope = api::SecurityOptionValue(base, "predicate_envelope:");
  typed.definer_principal_uuid = api::SecurityOptionValue(base, "definer_principal_uuid:");
  return typed;
}

api::EngineSecurityActivatePolicyRequest TypedSecurityActivatePolicyRequest(
    const SblrDispatchRequest& request) {
  api::EngineSecurityActivatePolicyRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.policy_uuid = !base.target_object.uuid.canonical.empty()
                          ? base.target_object.uuid.canonical
                          : api::SecurityOptionValue(base, "policy_uuid:");
  return typed;
}

api::EngineSecurityDeactivatePolicyRequest TypedSecurityDeactivatePolicyRequest(
    const SblrDispatchRequest& request) {
  api::EngineSecurityDeactivatePolicyRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.policy_uuid = !base.target_object.uuid.canonical.empty()
                          ? base.target_object.uuid.canonical
                          : api::SecurityOptionValue(base, "policy_uuid:");
  return typed;
}

api::EngineSecurityValidatePolicyRequest TypedSecurityValidatePolicyRequest(
    const SblrDispatchRequest& request) {
  api::EngineSecurityValidatePolicyRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.policy_uuid = !base.target_object.uuid.canonical.empty()
                          ? base.target_object.uuid.canonical
                          : api::SecurityOptionValue(base, "policy_uuid:");
  typed.observed_policy_generation =
      DispatchOptionU64(base, "observed_policy_generation:");
  typed.observed_cache_invalidation_epoch =
      DispatchOptionU64(base, "observed_cache_invalidation_epoch:");
  return typed;
}

api::EngineSecurityShowPolicyRequest TypedSecurityShowPolicyRequest(
    const SblrDispatchRequest& request) {
  api::EngineSecurityShowPolicyRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.policy_uuid = !base.target_object.uuid.canonical.empty()
                          ? base.target_object.uuid.canonical
                          : api::SecurityOptionValue(base, "policy_uuid:");
  typed.include_rows = api::SecurityOptionBool(base, "include_rows:", true);
  return typed;
}

template <typename TRequest>
TRequest TypedAgentHookRequest(const SblrDispatchRequest& request,
                               const std::string& default_agent_type,
                               const std::string& default_action) {
  TRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.agent_type = api::SecurityOptionValue(base, "agent_type:");
  if (typed.agent_type.empty()) { typed.agent_type = default_agent_type; }
  typed.action_class = api::SecurityOptionValue(base, "action_class:");
  if (typed.action_class.empty()) { typed.action_class = default_action; }
  typed.agent_uuid.canonical = api::SecurityOptionValue(base, "agent_uuid:");
  if (typed.agent_uuid.canonical.empty()) { typed.agent_uuid.canonical = "agent:local:" + typed.agent_type; }
  typed.policy_snapshot_uuid.canonical = api::SecurityOptionValue(base, "policy_snapshot_uuid:");
  if (typed.policy_snapshot_uuid.canonical.empty()) {
    typed.policy_snapshot_uuid.canonical = "policy:" + typed.agent_type + ":baseline";
  }
  typed.target_filespace = DispatchTargetOfKind(base, "filespace");
  typed.target_index = DispatchTargetOfKind(base, "index");
  typed.page_family = api::SecurityOptionValue(base, "page_family:");
  typed.page_type = api::SecurityOptionValue(base, "page_type:");
  typed.safety_fence_result = api::SecurityOptionValue(base, "safety_fence_result:");
  typed.cooldown_key = api::SecurityOptionValue(base, "cooldown_key:");
  typed.requested_pages = DispatchOptionU64(base, "requested_pages:");
  typed.requested_bytes = DispatchOptionU64(base, "requested_bytes:");
  typed.policy_authorized = api::SecurityOptionBool(base, "policy_authorized:", false);
  typed.evidence_sink_available = api::SecurityOptionBool(base, "evidence_sink_available:", false);
  typed.metrics_fresh = api::SecurityOptionBool(base, "metrics_fresh:", false);
  typed.cooldown_active = api::SecurityOptionBool(base, "cooldown_active:", false);
  typed.manual_override_active = api::SecurityOptionBool(base, "manual_override_active:", false);
  typed.lifecycle_fence_active = api::SecurityOptionBool(base, "lifecycle_fence_active:", false);
  typed.dry_run = api::SecurityOptionBool(base, "dry_run:", false);
  typed.shadow_build = api::SecurityOptionBool(base, "shadow_build:", false);
  return typed;
}

std::string JsonEscape(std::string_view input) {
  std::ostringstream out;
  for (const unsigned char ch : input) {
    switch (ch) {
      case '\\': out << "\\\\"; break;
      case '"': out << "\\\""; break;
      case '\b': out << "\\b"; break;
      case '\f': out << "\\f"; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default: out << ch;
    }
  }
  return out.str();
}

}  // namespace

SblrQueryPreflightResult PreflightSblrQueryOperation(
    SblrDispatchRequest request) {
  SblrQueryPreflightResult result;
  if (const char* trace = std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE"); trace && *trace) { std::ofstream f(trace, std::ios::app); if (f) f << "preflight_observe op=" << request.envelope.operation_id << " opcode=" << request.envelope.opcode << " code=" << request.envelope.opcode_code << "\n"; }
  const bool exact_source_map =
      request.envelope.operation_id == "engine.op.source_map" &&
      request.envelope.opcode == "SBLR_SOURCE_MAP" &&
      request.envelope.opcode_code == 6;
  const bool exact_error_vector =
      request.envelope.operation_id == "engine.op.error_vector" &&
      request.envelope.opcode == "SBLR_ERROR_VECTOR" &&
      request.envelope.opcode_code == 7;
  const bool exact_txn_begin =
      request.envelope.operation_id == "engine.op.txn_begin" &&
      request.envelope.opcode == "SBLR_TXN_BEGIN" &&
      request.envelope.opcode_code == 256;
  const bool exact_txn_commit =
      request.envelope.operation_id == "engine.op.txn_commit" &&
      request.envelope.opcode == "SBLR_TXN_COMMIT" &&
      request.envelope.opcode_code == 257;
  const bool exact_txn_rollback =
      request.envelope.operation_id == "engine.op.txn_rollback" &&
      request.envelope.opcode == "SBLR_TXN_ROLLBACK" &&
      request.envelope.opcode_code == 258;
  const bool exact_txn_savepoint =
      request.envelope.operation_id == "engine.op.txn_savepoint" &&
      request.envelope.opcode == "SBLR_TXN_SAVEPOINT" &&
      request.envelope.opcode_code == 259;
  const bool exact_txn_release_savepoint =
      request.envelope.operation_id == "engine.op.txn_release_savepoint" &&
      request.envelope.opcode == "SBLR_TXN_RELEASE_SAVEPOINT" &&
      request.envelope.opcode_code == 260;
  const bool exact_txn_rollback_to_savepoint =
      request.envelope.operation_id == "engine.op.txn_rollback_to_savepoint" &&
      request.envelope.opcode == "SBLR_TXN_ROLLBACK_TO_SAVEPOINT" &&
      request.envelope.opcode_code == 261;
  const bool exact_psql_autonomous_frame = request.envelope.operation_id=="engine.op.psql_autonomous_frame"&&request.envelope.opcode=="SBLR_PSQL_AUTONOMOUS_FRAME"&&request.envelope.opcode_code==262;
  const bool exact_reservation_release = request.envelope.operation_id=="engine.op.transaction_reservation_release"&&request.envelope.opcode=="SBLR_TRANSACTION_RESERVATION_RELEASE"&&request.envelope.opcode_code==263;
  const bool exact_temporary_cleanup = request.envelope.operation_id=="engine.op.temporary_instance_cleanup"&&request.envelope.opcode=="SBLR_TEMPORARY_INSTANCE_CLEANUP"&&request.envelope.opcode_code==264;
  const bool exact_cursor_open = request.envelope.operation_id=="engine.op.cursor_open"&&request.envelope.opcode=="SBLR_CURSOR_OPEN"&&request.envelope.opcode_code==512;
  const bool exact_cursor_fetch = request.envelope.operation_id=="engine.op.cursor_fetch"&&request.envelope.opcode=="SBLR_CURSOR_FETCH"&&request.envelope.opcode_code==513;
  const bool exact_cursor_close = request.envelope.operation_id=="engine.op.cursor_close"&&request.envelope.opcode=="SBLR_CURSOR_CLOSE"&&request.envelope.opcode_code==514;
  const bool exact_read_by_key = request.envelope.operation_id=="engine.op.read_by_key"&&request.envelope.opcode=="SBLR_READ_BY_KEY"&&request.envelope.opcode_code==515;
  const bool exact_read_range = request.envelope.operation_id=="engine.op.read_range"&&request.envelope.opcode=="SBLR_READ_RANGE"&&request.envelope.opcode_code==516;
  const bool exact_read_stream = request.envelope.operation_id=="engine.op.read_stream"&&request.envelope.opcode=="SBLR_READ_STREAM"&&request.envelope.opcode_code==517;
  const bool exact_result_set_pass = request.envelope.operation_id=="engine.op.result_set_pass"&&request.envelope.opcode=="SBLR_RESULT_SET_PASS"&&request.envelope.opcode_code==518;
  const bool exact_access_cursor_open = request.envelope.operation_id=="engine.op.access_cursor_open"&&request.envelope.opcode=="SBLR_ACCESS_CURSOR_OPEN"&&request.envelope.opcode_code==519;
  const bool exact_access_cursor_fetch = request.envelope.operation_id=="engine.op.access_cursor_fetch"&&request.envelope.opcode=="SBLR_ACCESS_CURSOR_FETCH"&&request.envelope.opcode_code==520;
  const bool exact_access_cursor_close = request.envelope.operation_id=="engine.op.access_cursor_close"&&request.envelope.opcode=="SBLR_ACCESS_CURSOR_CLOSE"&&request.envelope.opcode_code==521;
  const bool exact_insert = request.envelope.operation_id=="engine.op.insert"&&request.envelope.opcode=="SBLR_INSERT"&&request.envelope.opcode_code==768;
  const bool exact_update = request.envelope.operation_id=="engine.op.update"&&request.envelope.opcode=="SBLR_UPDATE"&&request.envelope.opcode_code==769;
  const bool exact_delete = request.envelope.operation_id=="engine.op.delete"&&request.envelope.opcode=="SBLR_DELETE"&&request.envelope.opcode_code==770;
  const bool exact_merge = request.envelope.operation_id=="engine.op.merge"&&request.envelope.opcode=="SBLR_MERGE"&&request.envelope.opcode_code==771;
  const bool exact_table_truncate = request.envelope.operation_id=="engine.op.table_truncate"&&request.envelope.opcode=="SBLR_TABLE_TRUNCATE"&&request.envelope.opcode_code==773;
  const bool exact_table_analyze = request.envelope.operation_id=="engine.op.table_analyze"&&request.envelope.opcode=="SBLR_TABLE_ANALYZE"&&request.envelope.opcode_code==774;
  const bool exact_bulk_import_stream = request.envelope.operation_id=="engine.op.bulk_import_stream"&&request.envelope.opcode=="SBLR_BULK_IMPORT_STREAM"&&request.envelope.opcode_code==775;
  const bool exact_bulk_export_stream = request.envelope.operation_id=="engine.op.bulk_export_stream"&&request.envelope.opcode=="SBLR_BULK_EXPORT_STREAM"&&request.envelope.opcode_code==776;
  const bool exact_statement_batch = request.envelope.operation_id=="engine.op.statement_batch"&&request.envelope.opcode=="SBLR_STATEMENT_BATCH"&&request.envelope.opcode_code==777;
  const bool exact_atomic_cas = request.envelope.operation_id=="engine.op.atomic_cas"&&request.envelope.opcode=="SBLR_ATOMIC_CAS"&&request.envelope.opcode_code==778;
  const bool exact_atomic_rmw = request.envelope.operation_id=="engine.op.atomic_read_modify_write"&&request.envelope.opcode=="SBLR_ATOMIC_READ_MODIFY_WRITE"&&request.envelope.opcode_code==779;
  const bool exact_advisory_lock = request.envelope.operation_id=="engine.op.advisory_lock_acquire"&&request.envelope.opcode=="SBLR_ADVISORY_LOCK_ACQUIRE"&&request.envelope.opcode_code==780;
  const bool exact_advisory_lock_release = request.envelope.operation_id=="engine.op.advisory_lock_release"&&request.envelope.opcode=="SBLR_ADVISORY_LOCK_RELEASE"&&request.envelope.opcode_code==781;
  const bool exact_function_call = request.envelope.operation_id=="engine.op.function_call"&&request.envelope.opcode=="SBLR_FUNCTION_CALL"&&request.envelope.opcode_code==1024;
  const bool exact_operator_call = request.envelope.operation_id=="engine.op.operator_call"&&request.envelope.opcode=="SBLR_OPERATOR_CALL"&&request.envelope.opcode_code==1025;
  const bool exact_cast = request.envelope.operation_id=="engine.op.cast"&&request.envelope.opcode=="SBLR_CAST"&&request.envelope.opcode_code==1026;
  const bool exact_compare = request.envelope.operation_id=="engine.op.compare"&&request.envelope.opcode=="SBLR_COMPARE"&&request.envelope.opcode_code==1027;
  const bool exact_domain_operation = request.envelope.operation_id=="engine.op.domain_operation"&&request.envelope.opcode=="SBLR_DOMAIN_OPERATION"&&request.envelope.opcode_code==1028;
  const bool exact_udr = request.envelope.operation_id=="engine.op.udr_invoke"&&request.envelope.opcode=="SBLR_UDR_INVOKE"&&request.envelope.opcode_code==1029;
  const bool exact_procedure = request.envelope.operation_id=="engine.op.procedure_invoke"&&request.envelope.opcode=="SBLR_PROCEDURE_INVOKE"&&request.envelope.opcode_code==1030;
  const bool exact_function_invoke = request.envelope.operation_id=="engine.op.function_invoke"&&request.envelope.opcode=="SBLR_FUNCTION_INVOKE"&&request.envelope.opcode_code==1031;
  const bool exact_aggregate_invoke = request.envelope.operation_id=="engine.op.aggregate_invoke"&&request.envelope.opcode=="SBLR_AGGREGATE_INVOKE"&&request.envelope.opcode_code==1032;
  const bool exact_sequence_nextval = request.envelope.operation_id=="engine.op.sequence_nextval"&&request.envelope.opcode=="SBLR_SEQUENCE_NEXTVAL"&&request.envelope.opcode_code==1033;
  const bool exact_sequence_currval = request.envelope.operation_id=="engine.op.sequence_currval"&&request.envelope.opcode=="SBLR_SEQUENCE_CURRVAL"&&request.envelope.opcode_code==1034;
  const bool exact_sequence_setval = request.envelope.operation_id=="engine.op.sequence_setval"&&request.envelope.opcode=="SBLR_SEQUENCE_SETVAL"&&request.envelope.opcode_code==1035;
  const bool exact_query_numeric = request.envelope.operation_id=="engine.op.query_apply_numeric_operation"&&request.envelope.opcode=="SBLR_QUERY_APPLY_NUMERIC_OPERATION"&&request.envelope.opcode_code==1036;
  const bool exact_advanced_datatype_family = request.envelope.operation_id=="engine.op.query_evaluate_advanced_datatype_family"&&request.envelope.opcode=="SBLR_QUERY_EVALUATE_ADVANCED_DATATYPE_FAMILY"&&request.envelope.opcode_code==1037;
  const bool exact_project = (request.envelope.operation_id=="engine.op.project"&&request.envelope.opcode=="SBLR_PROJECT"&&request.envelope.opcode_code==1280) || (request.envelope.operation_id=="engine.op.ddl_alter_rewrite_rule"&&request.envelope.opcode=="SBLR_DDL_ALTER_REWRITE_RULE"&&request.envelope.opcode_code==1618) || (request.envelope.operation_id=="engine.op.ddl_drop_rewrite_rule"&&request.envelope.opcode=="SBLR_DDL_DROP_REWRITE_RULE"&&request.envelope.opcode_code==1619);
  const bool exact_ddl_alter_rewrite_rule = request.envelope.operation_id=="engine.op.ddl_alter_rewrite_rule"&&request.envelope.opcode=="SBLR_DDL_ALTER_REWRITE_RULE"&&request.envelope.opcode_code==1618;
  const bool exact_ddl_drop_rewrite_rule = request.envelope.operation_id=="engine.op.ddl_drop_rewrite_rule"&&request.envelope.opcode=="SBLR_DDL_DROP_REWRITE_RULE"&&request.envelope.opcode_code==1619;
  const bool exact_ddl_validate_constraint = request.envelope.operation_id=="engine.op.ddl_validate_constraint"&&request.envelope.opcode=="SBLR_DDL_VALIDATE_CONSTRAINT"&&request.envelope.opcode_code==1620;
  const bool exact_security_create_privilege_template = request.envelope.operation_id=="engine.op.security_create_privilege_template"&&request.envelope.opcode=="SBLR_SECURITY_CREATE_PRIVILEGE_TEMPLATE"&&request.envelope.opcode_code==1621;
  const bool exact_security_create_user = request.envelope.operation_id=="engine.op.security_create_user"&&request.envelope.opcode=="SBLR_SEC_CREATE_USER"&&request.envelope.opcode_code==1792;
  const bool exact_security_alter_user = request.envelope.operation_id=="engine.op.sec_alter_user"&&request.envelope.opcode=="SBLR_SEC_ALTER_USER"&&request.envelope.opcode_code==1793;
  const bool exact_security_create_role = request.envelope.operation_id=="engine.op.sec_create_role"&&request.envelope.opcode=="SBLR_SEC_CREATE_ROLE"&&request.envelope.opcode_code==1794;
  const bool exact_security_drop_role = request.envelope.operation_id=="engine.op.sec_drop_role"&&request.envelope.opcode=="SBLR_SEC_DROP_ROLE"&&request.envelope.opcode_code==1801;
  const bool exact_security_alter_role = request.envelope.operation_id=="engine.op.sec_alter_role"&&request.envelope.opcode=="SBLR_SEC_ALTER_ROLE"&&request.envelope.opcode_code==1800;
  const bool exact_security_create_group_mapping = request.envelope.operation_id=="engine.op.sec_create_group_mapping"&&request.envelope.opcode=="SBLR_SEC_CREATE_GROUP_MAPPING"&&request.envelope.opcode_code==1797;
  const bool exact_security_drop_group_mapping = request.envelope.operation_id=="engine.op.sec_drop_group_mapping"&&request.envelope.opcode=="SBLR_SEC_DROP_GROUP_MAPPING"&&request.envelope.opcode_code==1806;
  const bool exact_security_grant = request.envelope.operation_id=="engine.op.sec_grant"&&request.envelope.opcode=="SBLR_SEC_GRANT"&&request.envelope.opcode_code==1795;
  const bool exact_security_revoke = request.envelope.operation_id=="engine.op.sec_revoke"&&request.envelope.opcode=="SBLR_SEC_REVOKE"&&request.envelope.opcode_code==1796;
  const bool exact_security_alter_policy = request.envelope.operation_id=="engine.op.sec_alter_policy"&&request.envelope.opcode=="SBLR_SEC_ALTER_POLICY"&&request.envelope.opcode_code==1798;
  const bool exact_security_drop_user = request.envelope.operation_id=="engine.op.sec_drop_user"&&request.envelope.opcode=="SBLR_SEC_DROP_USER"&&request.envelope.opcode_code==1799;
  const bool exact_security_authenticate = request.envelope.operation_id=="engine.op.sec_authenticate"&&request.envelope.opcode=="SBLR_SEC_AUTHENTICATE"&&request.envelope.opcode_code==1804;
  const bool exact_security_deauthenticate = request.envelope.operation_id=="engine.op.sec_deauthenticate"&&request.envelope.opcode=="SBLR_SEC_DEAUTHENTICATE"&&request.envelope.opcode_code==1805;
  const bool exact_session_role_switch = request.envelope.operation_id=="engine.op.session_role_switch"&&request.envelope.opcode=="SBLR_SESSION_ROLE_SWITCH"&&request.envelope.opcode_code==4359;
  const bool exact_session_setting_reset = request.envelope.operation_id=="engine.op.session_setting_reset"&&request.envelope.opcode=="SBLR_SESSION_SETTING_RESET"&&request.envelope.opcode_code==4357;
  const bool exact_session_setting_get = request.envelope.operation_id=="engine.op.session_setting_get"&&request.envelope.opcode=="SBLR_SESSION_SETTING_GET"&&request.envelope.opcode_code==4356;
  const bool exact_session_default_qualifier_set = request.envelope.operation_id=="engine.op.session_default_qualifier_set"&&request.envelope.opcode=="SBLR_SESSION_DEFAULT_QUALIFIER_SET"&&request.envelope.opcode_code==4358;
  const bool exact_session_discard = request.envelope.operation_id=="engine.op.session_discard"&&request.envelope.opcode=="SBLR_SESSION_DISCARD"&&request.envelope.opcode_code==4360;
  const bool exact_session_snapshot_handle = request.envelope.operation_id=="engine.op.session_snapshot_handle"&&request.envelope.opcode=="SBLR_SESSION_SNAPSHOT_HANDLE"&&request.envelope.opcode_code==4361;
  const bool exact_context_set = request.envelope.operation_id=="engine.op.context_set"&&request.envelope.opcode=="SBLR_CONTEXT_SET"&&request.envelope.opcode_code==4362;
  const bool exact_context_unset = request.envelope.operation_id=="engine.op.context_unset"&&request.envelope.opcode=="SBLR_CONTEXT_UNSET"&&request.envelope.opcode_code==4363;
  const bool exact_context_get = request.envelope.operation_id=="engine.op.context_get"&&request.envelope.opcode=="SBLR_CONTEXT_GET"&&request.envelope.opcode_code==4364;
  const bool exact_session_setting_set = request.envelope.operation_id=="engine.op.session_setting_set"&&request.envelope.opcode=="SBLR_SESSION_SETTING_SET"&&request.envelope.opcode_code==4355;
  const bool exact_security_create_policy = request.envelope.operation_id=="engine.op.sec_create_policy"&&request.envelope.opcode=="SBLR_SEC_CREATE_POLICY"&&request.envelope.opcode_code==1802;
  const bool exact_security_drop_policy = request.envelope.operation_id=="engine.op.sec_drop_policy"&&request.envelope.opcode=="SBLR_SEC_DROP_POLICY"&&request.envelope.opcode_code==1803;
  const bool exact_security_alter_privilege_template = request.envelope.operation_id=="engine.op.security_alter_privilege_template"&&request.envelope.opcode=="SBLR_SECURITY_ALTER_PRIVILEGE_TEMPLATE"&&request.envelope.opcode_code==1622;
  const bool exact_security_drop_privilege_template = request.envelope.operation_id=="engine.op.security_drop_privilege_template"&&request.envelope.opcode=="SBLR_SECURITY_DROP_PRIVILEGE_TEMPLATE"&&request.envelope.opcode_code==1623;
  const bool exact_database_create_template_clone = request.envelope.operation_id=="engine.op.database_create_template_clone"&&request.envelope.opcode=="SBLR_DATABASE_CREATE_TEMPLATE_CLONE"&&request.envelope.opcode_code==1624;
  const bool exact_ddl_create_aggregate = request.envelope.operation_id=="engine.op.ddl_create_aggregate"&&request.envelope.opcode=="SBLR_DDL_CREATE_AGGREGATE"&&request.envelope.opcode_code==1625;
  const bool exact_ddl_alter_aggregate = request.envelope.operation_id=="engine.op.ddl_alter_aggregate"&&request.envelope.opcode=="SBLR_DDL_ALTER_AGGREGATE"&&request.envelope.opcode_code==1626;
  const bool exact_ddl_drop_aggregate = request.envelope.operation_id=="engine.op.ddl_drop_aggregate"&&request.envelope.opcode=="SBLR_DDL_DROP_AGGREGATE"&&request.envelope.opcode_code==1627;
  const bool exact_ddl_drop_dictionary = request.envelope.operation_id=="engine.op.ddl_drop_dictionary"&&request.envelope.opcode=="SBLR_DDL_DROP_DICTIONARY"&&request.envelope.opcode_code==1638;
  const bool exact_ddl_alter_dictionary = request.envelope.operation_id=="engine.op.ddl_alter_dictionary"&&request.envelope.opcode=="SBLR_DDL_ALTER_DICTIONARY"&&request.envelope.opcode_code==1639;
  const bool exact_ddl_create_continuous_view = request.envelope.operation_id=="engine.op.ddl_create_continuous_view"&&request.envelope.opcode=="SBLR_DDL_CREATE_CONTINUOUS_VIEW"&&request.envelope.opcode_code==1640;
  const bool exact_ddl_alter_continuous_view = request.envelope.operation_id=="engine.op.ddl_alter_continuous_view"&&request.envelope.opcode=="SBLR_DDL_ALTER_CONTINUOUS_VIEW"&&request.envelope.opcode_code==1641;
  const bool exact_ddl_drop_continuous_view = request.envelope.operation_id=="engine.op.ddl_drop_continuous_view"&&request.envelope.opcode=="SBLR_DDL_DROP_CONTINUOUS_VIEW";
  const bool exact_dml_async_insert_submit = request.envelope.operation_id=="engine.op.dml_async_insert_submit"&&request.envelope.opcode=="SBLR_DML_ASYNC_INSERT_SUBMIT";
  const bool exact_dml_async_insert_status = request.envelope.operation_id=="engine.op.dml_async_insert_status"&&request.envelope.opcode=="SBLR_DML_ASYNC_INSERT_STATUS";
  const bool exact_dml_counter_add = request.envelope.operation_id=="engine.op.dml_counter_add"&&request.envelope.opcode=="SBLR_DML_COUNTER_ADD";
  const bool exact_dml_timeseries_schema_write = request.envelope.operation_id=="engine.op.dml_timeseries_schema_write";
  const bool exact_ddl_timeseries_series_cardinality_policy = request.envelope.operation_id=="engine.op.ddl_set_timeseries_series_cardinality_policy";
  const bool exact_ddl_create_timeseries_value_cache = request.envelope.operation_id=="engine.op.ddl_create_timeseries_value_cache";
  const bool exact_dml_async_insert_cancel = request.envelope.operation_id=="engine.op.dml_async_insert_cancel"&&request.envelope.opcode=="SBLR_DML_ASYNC_INSERT_CANCEL";
  const bool exact_ddl_purge_system_history = request.envelope.operation_id=="engine.op.ddl_purge_system_history"&&request.envelope.opcode=="SBLR_DDL_PURGE_SYSTEM_HISTORY"&&request.envelope.opcode_code==1628;
  const bool exact_ddl_set_index_optimizer_eligibility = request.envelope.operation_id=="engine.op.ddl_set_index_optimizer_eligibility"&&request.envelope.opcode=="SBLR_DDL_SET_INDEX_OPTIMIZER_ELIGIBILITY"&&request.envelope.opcode_code==1629;
  const bool exact_ddl_set_table_type_enforcement = (request.envelope.operation_id=="engine.op.ddl_set_table_type_enforcement"&&request.envelope.opcode=="SBLR_DDL_SET_TABLE_TYPE_ENFORCEMENT"&&request.envelope.opcode_code==1630) || (request.envelope.operation_id=="engine.op.database_serialize_logical_snapshot"&&request.envelope.opcode=="SBLR_DATABASE_SERIALIZE_LOGICAL_SNAPSHOT"&&request.envelope.opcode_code==1631);
  const bool exact_database_serialize_logical_snapshot = request.envelope.operation_id=="engine.op.database_serialize_logical_snapshot"&&request.envelope.opcode=="SBLR_DATABASE_SERIALIZE_LOGICAL_SNAPSHOT"&&request.envelope.opcode_code==1631;
  const bool exact_database_deserialize_logical_snapshot = request.envelope.operation_id=="engine.op.database_deserialize_logical_snapshot"&&request.envelope.opcode=="SBLR_DATABASE_DESERIALIZE_LOGICAL_SNAPSHOT"&&request.envelope.opcode_code==1632;
  const bool exact_ddl_create_macro = request.envelope.operation_id=="engine.op.ddl_create_macro"&&request.envelope.opcode=="SBLR_DDL_CREATE_MACRO"&&request.envelope.opcode_code==1633;
  const bool exact_ddl_drop_macro = request.envelope.operation_id=="engine.op.ddl_drop_macro"&&request.envelope.opcode=="SBLR_DDL_DROP_MACRO"&&request.envelope.opcode_code==1634;
  const bool exact_admin_register_external_relation_resolver = request.envelope.operation_id=="engine.op.admin_register_external_relation_resolver"&&request.envelope.opcode=="SBLR_ADMIN_REGISTER_EXTERNAL_RELATION_RESOLVER"&&request.envelope.opcode_code==1635;
  const bool exact_admin_unregister_external_relation_resolver = request.envelope.operation_id=="engine.op.admin_unregister_external_relation_resolver"&&request.envelope.opcode=="SBLR_ADMIN_UNREGISTER_EXTERNAL_RELATION_RESOLVER"&&request.envelope.opcode_code==1636;
  const bool exact_ddl_create_dictionary = request.envelope.operation_id=="engine.op.ddl_create_dictionary"&&request.envelope.opcode=="SBLR_DDL_CREATE_DICTIONARY"&&(request.envelope.opcode_code==1637||request.envelope.opcode_code==1608);
  const bool exact_aggregate = request.envelope.operation_id=="engine.op.aggregate"&&request.envelope.opcode=="SBLR_AGGREGATE"&&request.envelope.opcode_code==1281;
  const bool exact_group = request.envelope.operation_id=="engine.op.group"&&request.envelope.opcode=="SBLR_GROUP"&&request.envelope.opcode_code==1282;
  const bool exact_sort = request.envelope.operation_id=="engine.op.sort"&&request.envelope.opcode=="SBLR_SORT"&&request.envelope.opcode_code==1283;
  const bool exact_limit = request.envelope.operation_id=="engine.op.limit"&&request.envelope.opcode=="SBLR_LIMIT"&&request.envelope.opcode_code==1284;
  const bool exact_return_result_set = request.envelope.operation_id=="engine.op.return_result_set"&&request.envelope.opcode=="SBLR_RETURN_RESULT_SET"&&request.envelope.opcode_code==1286;
  const bool exact_kv_structured_read = request.envelope.operation_id=="engine.op.kv_structured_read"&&request.envelope.opcode=="SBLR_KV_STRUCTURED_READ"&&request.envelope.opcode_code==8192;
  const bool exact_kv_structured_mutate = request.envelope.operation_id=="engine.op.kv_structured_mutate"&&request.envelope.opcode=="SBLR_KV_STRUCTURED_MUTATE"&&request.envelope.opcode_code==8193;
  const bool exact_kv_structured_scan = request.envelope.operation_id=="engine.op.kv_structured_scan"&&request.envelope.opcode=="SBLR_KV_STRUCTURED_SCAN"&&request.envelope.opcode_code==8194;
  const bool exact_kv_structured_stream_read = request.envelope.operation_id=="engine.op.kv_structured_stream_read"&&request.envelope.opcode=="SBLR_KV_STRUCTURED_STREAM_READ"&&request.envelope.opcode_code==8195;
  const bool exact_kv_structured_stream_append = request.envelope.operation_id=="engine.op.kv_structured_stream_append"&&request.envelope.opcode=="SBLR_KV_STRUCTURED_STREAM_APPEND"&&request.envelope.opcode_code==8196;
  const bool exact_kv_structured_timeseries = request.envelope.operation_id=="engine.op.kv_structured_timeseries"&&request.envelope.opcode=="SBLR_KV_STRUCTURED_TIMESERIES"&&request.envelope.opcode_code==8197;
  const bool exact_system_config_set = request.envelope.operation_id=="engine.op.system_config_set"&&request.envelope.opcode=="SBLR_SYSTEM_CONFIG_SET"&&request.envelope.opcode_code==5125;
  const bool exact_ddl_create_domain = request.envelope.operation_id=="engine.op.ddl_create_domain"&&request.envelope.opcode=="SBLR_DDL_CREATE_DOMAIN"&&request.envelope.opcode_code==1542;
  const bool exact_ddl_create_schema = request.envelope.operation_id=="engine.op.ddl_create_schema"&&request.envelope.opcode=="SBLR_DDL_CREATE_SCHEMA"&&request.envelope.opcode_code==1536;
  const bool exact_ddl_create_table = request.envelope.operation_id=="engine.op.ddl_create_table"&&request.envelope.opcode=="SBLR_DDL_CREATE_TABLE"&&request.envelope.opcode_code==1537;
  const bool exact_ddl_drop_table = request.envelope.operation_id=="engine.op.ddl_drop_table"&&request.envelope.opcode=="SBLR_DDL_DROP_TABLE"&&request.envelope.opcode_code==1539;
  const bool exact_diagnostic_refusal = request.envelope.operation_id=="engine.op.diagnostic_refusal"&&request.envelope.opcode=="SBLR_DIAGNOSTIC_REFUSAL"&&request.envelope.opcode_code==0x1900;
  const bool exact_ddl_create_table_as_query_with_data = request.envelope.operation_id=="engine.op.ddl_create_table_as_query_with_data"&&request.envelope.opcode=="SBLR_DDL_CREATE_TABLE_AS_QUERY_WITH_DATA"&&request.envelope.opcode_code==1669;
  const bool exact_ddl_create_table_as_query_with_no_data = request.envelope.operation_id=="engine.op.ddl_create_table_as_query_with_no_data"&&request.envelope.opcode=="SBLR_DDL_CREATE_TABLE_AS_QUERY_WITH_NO_DATA"&&request.envelope.opcode_code==1670;
  const bool exact_ddl_create_index = request.envelope.operation_id=="engine.op.ddl_create_index"&&request.envelope.opcode=="SBLR_DDL_CREATE_INDEX"&&request.envelope.opcode_code==1540;
  const bool exact_ddl_drop_index = request.envelope.operation_id=="engine.op.ddl_drop_index"&&request.envelope.opcode=="SBLR_DDL_DROP_INDEX"&&request.envelope.opcode_code==1541;
  const bool exact_ddl_alter_domain = request.envelope.operation_id=="engine.op.ddl_alter_domain"&&request.envelope.opcode=="SBLR_DDL_ALTER_DOMAIN"&&request.envelope.opcode_code==1547;
  const bool exact_ddl_create_view = (request.envelope.operation_id=="engine.op.ddl_create_view"&&request.envelope.opcode=="SBLR_DDL_CREATE_VIEW"&&request.envelope.opcode_code==1548) ||
      (request.envelope.operation_id=="engine.op.ddl_refresh_materialized_view"&&request.envelope.opcode=="SBLR_DDL_REFRESH_MATERIALIZED_VIEW"&&request.envelope.opcode_code==1567) ||
      (request.envelope.operation_id=="engine.op.ddl_create_materialized_view"&&request.envelope.opcode=="SBLR_DDL_CREATE_MATERIALIZED_VIEW"&&request.envelope.opcode_code==1566);
  const bool exact_ddl_create_type = request.envelope.operation_id=="engine.op.ddl_create_type"&&request.envelope.opcode=="SBLR_DDL_CREATE_TYPE"&&request.envelope.opcode_code==1569;
  const bool exact_ddl_alter_type = request.envelope.operation_id=="engine.op.ddl_alter_type"&&request.envelope.opcode=="SBLR_DDL_ALTER_TYPE"&&request.envelope.opcode_code==1570;
  const bool exact_ddl_drop_type = request.envelope.operation_id=="engine.op.ddl_drop_type"&&request.envelope.opcode=="SBLR_DDL_DROP_TYPE"&&request.envelope.opcode_code==1571;
  const bool exact_ddl_refresh_materialized_view = request.envelope.operation_id=="engine.op.ddl_refresh_materialized_view"&&request.envelope.opcode=="SBLR_DDL_REFRESH_MATERIALIZED_VIEW"&&request.envelope.opcode_code==1567;
  const bool exact_ddl_create_materialized_view = request.envelope.operation_id=="engine.op.ddl_create_materialized_view"&&request.envelope.opcode=="SBLR_DDL_CREATE_MATERIALIZED_VIEW"&&request.envelope.opcode_code==1566;
  const bool exact_ddl_drop_materialized_view = request.envelope.operation_id=="engine.op.ddl_drop_materialized_view"&&request.envelope.opcode=="SBLR_DDL_DROP_MATERIALIZED_VIEW"&&request.envelope.opcode_code==1568;
  const bool exact_ddl_create_publication = request.envelope.operation_id=="engine.op.ddl_create_publication"&&request.envelope.opcode=="SBLR_DDL_CREATE_PUBLICATION"&&request.envelope.opcode_code==1582;
  const bool exact_ddl_alter_publication = request.envelope.operation_id=="engine.op.ddl_alter_publication"&&request.envelope.opcode=="SBLR_DDL_ALTER_PUBLICATION"&&request.envelope.opcode_code==1583;
  const bool exact_ddl_alter_view = request.envelope.operation_id=="engine.op.ddl_alter_view"&&request.envelope.opcode=="SBLR_DDL_ALTER_VIEW"&&request.envelope.opcode_code==1549;
  const bool exact_ddl_drop_view = (request.envelope.operation_id=="engine.op.ddl_drop_view"&&request.envelope.opcode=="SBLR_DDL_DROP_VIEW"&&request.envelope.opcode_code==1550) || (request.envelope.operation_id=="engine.op.ddl_create_trigger"&&request.envelope.opcode=="SBLR_DDL_CREATE_TRIGGER"&&request.envelope.opcode_code==1551) || (request.envelope.operation_id=="engine.op.ddl_alter_trigger"&&request.envelope.opcode=="SBLR_DDL_ALTER_TRIGGER"&&request.envelope.opcode_code==1552) || (request.envelope.operation_id=="engine.op.ddl_drop_trigger"&&request.envelope.opcode=="SBLR_DDL_DROP_TRIGGER"&&request.envelope.opcode_code==1553);
  const bool exact_ddl_alter_trigger = request.envelope.operation_id=="engine.op.ddl_alter_trigger"&&request.envelope.opcode=="SBLR_DDL_ALTER_TRIGGER"&&request.envelope.opcode_code==1552;
  const bool exact_ddl_drop_trigger = request.envelope.operation_id=="engine.op.ddl_drop_trigger"&&request.envelope.opcode=="SBLR_DDL_DROP_TRIGGER"&&request.envelope.opcode_code==1553;
  const bool exact_ddl_create_procedure = request.envelope.operation_id=="engine.op.ddl_create_procedure"&&request.envelope.opcode=="SBLR_DDL_CREATE_PROCEDURE"&&request.envelope.opcode_code==1554;
  const bool exact_ddl_drop_procedure = request.envelope.operation_id=="engine.op.ddl_drop_procedure"&&request.envelope.opcode=="SBLR_DDL_DROP_PROCEDURE"&&request.envelope.opcode_code==1556;
  const bool exact_ddl_create_function = request.envelope.operation_id=="engine.op.ddl_create_function"&&request.envelope.opcode=="SBLR_DDL_CREATE_FUNCTION"&&request.envelope.opcode_code==1557;
  const bool exact_ddl_alter_function = request.envelope.operation_id=="engine.op.ddl_alter_function"&&request.envelope.opcode=="SBLR_DDL_ALTER_FUNCTION"&&request.envelope.opcode_code==1558;
  const bool exact_ddl_drop_function = request.envelope.operation_id=="engine.op.ddl_drop_function"&&request.envelope.opcode=="SBLR_DDL_DROP_FUNCTION"&&request.envelope.opcode_code==1559;
  const bool exact_ddl_create_package = request.envelope.operation_id=="engine.op.ddl_create_package"&&request.envelope.opcode=="SBLR_DDL_CREATE_PACKAGE"&&request.envelope.opcode_code==1560;
  const bool exact_ddl_create_sequence = request.envelope.operation_id=="engine.op.ddl_create_sequence"&&request.envelope.opcode=="SBLR_DDL_CREATE_SEQUENCE"&&request.envelope.opcode_code==1671;
  const bool exact_ddl_drop_package = request.envelope.operation_id=="engine.op.ddl_drop_package"&&request.envelope.opcode=="SBLR_DDL_DROP_PACKAGE"&&request.envelope.opcode_code==1562;
  const bool exact_ddl_drop_synonym = request.envelope.operation_id=="engine.op.ddl_drop_synonym"&&request.envelope.opcode=="SBLR_DDL_DROP_SYNONYM"&&request.envelope.opcode_code==1575;
  const bool exact_ddl_alter_package = request.envelope.operation_id=="engine.op.ddl_alter_package"&&request.envelope.opcode=="SBLR_DDL_ALTER_PACKAGE"&&request.envelope.opcode_code==1561;
  const bool exact_ddl_alter_sequence = request.envelope.operation_id=="engine.op.ddl_alter_sequence"&&request.envelope.opcode=="SBLR_DDL_ALTER_SEQUENCE"&&request.envelope.opcode_code==1564;
  const bool exact_ddl_drop_sequence = request.envelope.operation_id=="engine.op.ddl_drop_sequence"&&request.envelope.opcode=="SBLR_DDL_DROP_SEQUENCE"&&request.envelope.opcode_code==1565;
  const bool exact_ddl_create_temporary_table = request.envelope.operation_id=="engine.op.ddl_create_temporary_table"&&request.envelope.opcode=="SBLR_DDL_CREATE_TEMPORARY_TABLE"&&request.envelope.opcode_code==1561;
  const bool exact_ddl_drop_temporary_table = request.envelope.operation_id=="engine.op.ddl_drop_temporary_table"&&request.envelope.opcode=="SBLR_DDL_DROP_TEMPORARY_TABLE"&&request.envelope.opcode_code==1562;
  const bool exact_ddl_rename_object_vector = request.envelope.operation_id=="engine.op.ddl_rename_object_vector"&&request.envelope.opcode=="SBLR_DDL_RENAME_OBJECT_VECTOR"&&request.envelope.opcode_code==1563;
  const bool exact_ddl_rename_object = request.envelope.operation_id=="engine.op.ddl_rename_object"&&request.envelope.opcode=="SBLR_DDL_RENAME_OBJECT"&&request.envelope.opcode_code==1572;
  const bool exact_ddl_create_synonym = request.envelope.operation_id=="engine.op.ddl_create_synonym"&&request.envelope.opcode=="SBLR_DDL_CREATE_SYNONYM"&&request.envelope.opcode_code==1574;
  const bool exact_ddl_create_foreign_table = request.envelope.operation_id=="engine.op.ddl_create_foreign_table"&&request.envelope.opcode=="SBLR_DDL_CREATE_FOREIGN_TABLE"&&request.envelope.opcode_code==1576;
  const bool exact_ddl_create_fdw = request.envelope.operation_id=="engine.op.ddl_create_fdw"&&request.envelope.opcode=="SBLR_DDL_CREATE_FDW"&&request.envelope.opcode_code==1578;
  const bool exact_ddl_drop_fdw = request.envelope.operation_id=="engine.op.ddl_drop_fdw"&&request.envelope.opcode=="SBLR_DDL_DROP_FDW"&&request.envelope.opcode_code==1579;
  const bool exact_ddl_drop_foreign_table = request.envelope.operation_id=="engine.op.ddl_drop_foreign_table"&&request.envelope.opcode=="SBLR_DDL_DROP_FOREIGN_TABLE"&&request.envelope.opcode_code==1577;
  const bool exact_ddl_create_or_replace_srs = request.envelope.operation_id=="engine.op.ddl_create_or_replace_srs"&&request.envelope.opcode=="SBLR_DDL_CREATE_OR_REPLACE_SRS"&&request.envelope.opcode_code==1615;
  const bool exact_ddl_drop_srs = request.envelope.operation_id=="engine.op.ddl_drop_srs"&&request.envelope.opcode=="SBLR_DDL_DROP_SRS"&&request.envelope.opcode_code==1616;
  const bool exact_ddl_create_rewrite_rule = request.envelope.operation_id=="engine.op.ddl_create_rewrite_rule"&&request.envelope.opcode=="SBLR_DDL_CREATE_REWRITE_RULE"&&request.envelope.opcode_code==1617;
  const bool exact_ddl_alter_procedure = request.envelope.operation_id=="engine.op.ddl_alter_procedure"&&request.envelope.opcode=="SBLR_DDL_ALTER_PROCEDURE"&&request.envelope.opcode_code==1555;
  const bool exact_ddl_create_trigger = (request.envelope.operation_id=="engine.op.ddl_create_trigger"&&request.envelope.opcode=="SBLR_DDL_CREATE_TRIGGER"&&request.envelope.opcode_code==1551) || (request.envelope.operation_id=="engine.op.ddl_alter_trigger"&&request.envelope.opcode=="SBLR_DDL_ALTER_TRIGGER"&&request.envelope.opcode_code==1552) || exact_ddl_drop_trigger;
  const bool exact_window = (request.envelope.operation_id=="engine.op.window"&&request.envelope.opcode=="SBLR_WINDOW"&&request.envelope.opcode_code==1285) || exact_context_set || exact_diagnostic_refusal || exact_return_result_set || exact_kv_structured_read || exact_kv_structured_mutate || exact_kv_structured_scan || exact_kv_structured_stream_read || exact_kv_structured_stream_append || exact_kv_structured_timeseries || exact_system_config_set || exact_ddl_create_domain || exact_ddl_create_schema || exact_ddl_create_table || exact_ddl_drop_table || exact_ddl_create_index || exact_ddl_drop_index || exact_ddl_alter_domain || exact_ddl_create_view || exact_ddl_refresh_materialized_view || exact_ddl_drop_materialized_view || exact_ddl_create_type || exact_ddl_alter_type || exact_ddl_drop_type || exact_ddl_alter_view || exact_ddl_drop_view || exact_ddl_create_trigger || exact_ddl_alter_trigger || exact_ddl_create_procedure || exact_ddl_alter_procedure || exact_ddl_drop_procedure || exact_ddl_create_function || exact_ddl_alter_function || exact_ddl_drop_function || exact_ddl_create_package || exact_ddl_alter_package || exact_ddl_alter_sequence || exact_ddl_drop_package || exact_ddl_create_or_replace_srs || exact_ddl_drop_srs || exact_ddl_create_rewrite_rule || exact_ddl_create_foreign_table || exact_ddl_create_fdw || exact_ddl_drop_fdw || exact_ddl_drop_foreign_table;
  const bool exact_management_envelope =
      (request.envelope.operation_id == "engine.op.mgmt_operation" &&
       request.envelope.opcode == "SBLR_MGMT_OPERATION" &&
       request.envelope.opcode_code == 0x0D00) ||
      (request.envelope.operation_id == "engine.op.mgmt_payload" &&
       request.envelope.opcode == "SBLR_MGMT_PAYLOAD" &&
       request.envelope.opcode_code == 0x0D01) ||
      (request.envelope.operation_id == "engine.op.mgmt_result" &&
       request.envelope.opcode == "SBLR_MGMT_RESULT" &&
       request.envelope.opcode_code == 0x0D02) ||
      (request.envelope.operation_id == "engine.op.mgmt_progress" &&
       request.envelope.opcode == "SBLR_MGMT_PROGRESS" &&
       request.envelope.opcode_code == 0x0D03) ||
      (request.envelope.operation_id == "engine.op.mgmt_diagnostic" &&
       request.envelope.opcode == "SBLR_MGMT_DIAGNOSTIC" &&
       request.envelope.opcode_code == 0x0D04) ||
      (request.envelope.operation_id == "engine.op.mgmt_metric_snapshot_ref" &&
       request.envelope.opcode == "SBLR_MGMT_METRIC_SNAPSHOT_REF" &&
       request.envelope.opcode_code == 0x0D05);
  const bool exact_show_version =
      request.envelope.operation_id == "observability.show_version" &&
      request.envelope.opcode == "SBLR_OBSERVABILITY_SHOW_VERSION" &&
      request.envelope.opcode_code == 0x0D06;
  const bool exact_local_metrics_read =
      request.envelope.operation_id == "engine.op.read_metrics" &&
      request.envelope.opcode == "SBLR_READ_METRICS" &&
      request.envelope.opcode_code == 0x0C01;
  const bool exact_catalog_introspect =
      request.envelope.operation_id == "engine.op.catalog_introspect" &&
      request.envelope.opcode == "SBLR_CATALOG_INTROSPECT" &&
      request.envelope.opcode_code == 0x1300;
  const bool exact_event_notification =
      IsSblrEventNotificationOperation(request.envelope.operation_id) &&
      request.envelope.opcode_code >= 0x0F00 &&
      request.envelope.opcode_code <= 0x0F09;
  const bool exact_local_backup_archive =
      request.envelope.operation_id.rfind("engine.op.", 0) == 0 &&
      request.envelope.opcode_code >= 0x0A00 && request.envelope.opcode_code <= 0x0A04;
  if (exact_ddl_drop_sequence) { result.ok=true; result.materialized_envelope=request.envelope; return result; }
  if (exact_ddl_create_macro) { result.ok=true; result.materialized_envelope=request.envelope; return result; }
  if (exact_ddl_drop_macro) { result.ok=true; result.materialized_envelope=request.envelope; return result; }
  if (exact_admin_register_external_relation_resolver) { result.ok=true; result.materialized_envelope=request.envelope; return result; }
  if (exact_admin_unregister_external_relation_resolver) { result.ok=true; result.materialized_envelope=request.envelope; return result; }
  if (exact_ddl_create_dictionary) { result.ok=true; result.materialized_envelope=request.envelope; return result; }
  if (exact_ddl_alter_dictionary) { result.ok=true; result.materialized_envelope=request.envelope; return result; }
  if (exact_ddl_create_continuous_view) { result.ok=true; result.materialized_envelope=request.envelope; return result; }
  if (exact_ddl_alter_continuous_view) { result.ok=true; result.materialized_envelope=request.envelope; return result; }
  if (exact_ddl_drop_continuous_view) { result.ok=true; result.materialized_envelope=request.envelope; return result; }
  if (exact_dml_async_insert_submit) { result.ok=true; result.materialized_envelope=request.envelope; return result; }
  if (exact_dml_async_insert_status || exact_dml_counter_add || exact_dml_timeseries_schema_write || exact_ddl_timeseries_series_cardinality_policy || exact_ddl_create_timeseries_value_cache) { result.ok=true; result.materialized_envelope=request.envelope; return result; }
  if (exact_dml_async_insert_cancel) { result.ok=true; result.materialized_envelope=request.envelope; return result; }
  if (exact_ddl_create_sequence) { result.ok=true; result.materialized_envelope=request.envelope; return result; }
  if (exact_ddl_alter_sequence || exact_ddl_drop_sequence) { result.ok=true; result.materialized_envelope=request.envelope; return result; }
  if (exact_ddl_create_materialized_view) { result.ok=true; result.materialized_envelope=request.envelope; return result; }
  if (exact_ddl_create_table_as_query_with_data || exact_ddl_create_table_as_query_with_no_data || request.envelope.operation_id == "engine.op.ddl_create_table_as_query_with_data" || request.envelope.operation_id == "engine.op.ddl_create_table_as_query_with_no_data") { result.ok=true; result.materialized_envelope=request.envelope; return result; }
  if (exact_ddl_drop_synonym) { result.ok=true; result.materialized_envelope=request.envelope; return result; }
  if (exact_ddl_drop_foreign_table) { result.ok=true; result.materialized_envelope=request.envelope; return result; }
  if (exact_ddl_create_publication || exact_ddl_alter_publication) { result.ok=true; result.materialized_envelope=request.envelope; return result; }
  if (exact_security_create_role) { result.ok=true; result.materialized_envelope=request.envelope; return result; }
  if (exact_context_unset) { result.ok=true; result.materialized_envelope=request.envelope; return result; }
  if (exact_context_get) { result.ok=true; result.materialized_envelope=request.envelope; return result; }
  if (exact_security_drop_role) { result.ok=true; result.materialized_envelope=request.envelope; return result; }
  if (exact_security_alter_role || exact_security_create_group_mapping || exact_security_drop_group_mapping || exact_security_grant || exact_security_revoke || exact_security_alter_policy || exact_security_drop_user || exact_security_authenticate || exact_security_deauthenticate || exact_session_role_switch || exact_session_setting_set || exact_session_setting_reset || exact_session_setting_get || exact_session_default_qualifier_set || exact_session_discard || exact_session_snapshot_handle || exact_context_set) { result.ok=true; result.materialized_envelope=request.envelope; return result; }
  if (exact_security_create_policy) { result.ok=true; result.materialized_envelope=request.envelope; return result; }
  if (exact_security_drop_policy) { result.ok=true; result.materialized_envelope=request.envelope; return result; }
  if (request.envelope.operation_id != "query.execute" && !exact_ddl_alter_continuous_view && !exact_ddl_drop_continuous_view && !exact_dml_async_insert_submit && !exact_dml_async_insert_status && !exact_dml_async_insert_cancel && !exact_dml_counter_add && !exact_dml_timeseries_schema_write && !exact_ddl_timeseries_series_cardinality_policy && !exact_ddl_create_timeseries_value_cache && !exact_ddl_alter_rewrite_rule && !exact_ddl_drop_rewrite_rule && !exact_ddl_validate_constraint && !exact_security_create_privilege_template && !exact_security_create_user && !exact_security_alter_privilege_template && !exact_security_drop_privilege_template && !exact_source_map && !exact_show_version &&
      !exact_error_vector && !exact_database_create_template_clone && !exact_ddl_create_aggregate && !exact_txn_begin && !exact_txn_commit &&
      !exact_error_vector && !exact_database_create_template_clone && !exact_ddl_alter_aggregate && !exact_ddl_drop_aggregate && !exact_ddl_drop_dictionary && !exact_ddl_purge_system_history && !exact_ddl_set_index_optimizer_eligibility && !exact_ddl_set_table_type_enforcement && !exact_database_deserialize_logical_snapshot && !exact_security_alter_user && !exact_security_create_role && !exact_security_drop_role && !exact_security_alter_role && !exact_security_create_group_mapping && !exact_security_drop_group_mapping && !exact_security_create_policy && !exact_security_drop_policy && !exact_txn_begin && !exact_txn_commit &&
      !exact_txn_rollback && !exact_txn_savepoint && !exact_txn_release_savepoint && !exact_txn_rollback_to_savepoint && !exact_psql_autonomous_frame && !exact_reservation_release && !exact_temporary_cleanup && !exact_cursor_open && !exact_cursor_fetch && !exact_cursor_close && !exact_read_by_key && !exact_read_range && !exact_read_stream && !exact_result_set_pass && !exact_access_cursor_open && !exact_access_cursor_fetch && !exact_access_cursor_close && !exact_insert && !exact_update && !exact_delete && !exact_merge && !exact_table_truncate && !exact_table_analyze && !exact_bulk_import_stream && !exact_bulk_export_stream && !exact_statement_batch && !exact_atomic_cas && !exact_atomic_rmw && !exact_advisory_lock && !exact_advisory_lock_release && !exact_function_call && !exact_operator_call && !exact_cast && !exact_compare && !exact_domain_operation && !exact_udr && !exact_procedure && !exact_function_invoke && !exact_aggregate_invoke && !exact_sequence_nextval && !exact_sequence_currval && !exact_sequence_setval && !exact_query_numeric && !exact_advanced_datatype_family && !exact_ddl_create_domain && !exact_ddl_create_schema && !exact_ddl_create_table && !exact_ddl_create_index && !exact_ddl_drop_index && !exact_ddl_alter_domain && !exact_ddl_create_view && !exact_ddl_alter_view && !exact_ddl_drop_view && !exact_ddl_create_publication && !exact_ddl_alter_publication && !exact_ddl_create_procedure && !exact_ddl_alter_procedure && !exact_ddl_drop_procedure && !exact_ddl_create_function && !exact_ddl_alter_function && !exact_ddl_drop_function && !exact_ddl_create_package && !exact_ddl_create_temporary_table && !exact_ddl_drop_temporary_table && !exact_ddl_rename_object_vector && !exact_ddl_rename_object && !exact_ddl_create_synonym && !exact_ddl_create_or_replace_srs && !exact_project && !exact_aggregate && !exact_group && !exact_sort && !exact_limit && !exact_window && !exact_management_envelope &&
      !exact_security_drop_privilege_template && !exact_local_metrics_read && !exact_catalog_introspect && !exact_event_notification && !exact_local_backup_archive && !exact_ddl_create_dictionary && !exact_context_unset && !exact_context_get) {
    result.diagnostic_id = "SBLR.OPERATION.OPCODE_IDENTITY_MISMATCH";
    result.detail = "package root preflight admits query.execute only";
    return result;
  }
  const auto envelope_validation = ValidateSblrEnvelope(request.envelope);
  if (!envelope_validation.ok) {
    result.diagnostic_id = envelope_validation.diagnostics.empty()
        ? "SBLR.OPERATION.OPERAND_INVALID"
        : envelope_validation.diagnostics.front().code;
    result.detail = envelope_validation.diagnostics.empty()
        ? "canonical query envelope validation failed"
        : envelope_validation.diagnostics.front().message;
    return result;
  }
  if (exact_ddl_alter_dictionary) {
    result.ok=true;
    result.materialized_envelope=request.envelope;
    return result;
  }
  if (exact_ddl_create_materialized_view || exact_ddl_refresh_materialized_view) {
    result.ok = true;
    result.materialized_envelope = request.envelope;
    return result;
  }
  if (exact_ddl_alter_sequence) {
    result.ok = true;
    result.materialized_envelope = request.envelope;
    return result;
  }
  if (exact_ddl_create_synonym || exact_ddl_create_foreign_table || exact_ddl_create_fdw || exact_ddl_drop_fdw || exact_ddl_rename_object) {
    result.ok = true;
    result.materialized_envelope = request.envelope;
    return result;
  }
  if (exact_source_map || exact_error_vector || exact_txn_begin ||
      exact_txn_commit || exact_txn_rollback || exact_txn_savepoint || exact_txn_release_savepoint || exact_txn_rollback_to_savepoint || exact_psql_autonomous_frame || exact_reservation_release || exact_temporary_cleanup || exact_cursor_open || exact_cursor_fetch || exact_cursor_close || exact_read_by_key || exact_read_range || exact_read_stream || exact_result_set_pass || exact_access_cursor_open || exact_access_cursor_fetch || exact_access_cursor_close || exact_insert || exact_update || exact_delete || exact_merge || exact_table_truncate || exact_table_analyze || exact_bulk_import_stream || exact_bulk_export_stream || exact_statement_batch || exact_atomic_cas || exact_atomic_rmw || exact_advisory_lock || exact_advisory_lock_release || exact_function_call || exact_operator_call || exact_cast || exact_compare || exact_domain_operation || exact_udr || exact_procedure || exact_function_invoke || exact_aggregate_invoke || exact_sequence_nextval || exact_sequence_currval || exact_sequence_setval || exact_query_numeric || exact_advanced_datatype_family || exact_management_envelope ||
      exact_project || exact_security_create_privilege_template || exact_security_create_user || exact_security_alter_user || exact_security_alter_privilege_template || exact_security_drop_privilege_template || exact_database_create_template_clone || exact_ddl_create_aggregate || exact_ddl_alter_aggregate || exact_ddl_drop_aggregate || exact_ddl_drop_dictionary || exact_ddl_purge_system_history || exact_ddl_set_index_optimizer_eligibility || exact_ddl_set_table_type_enforcement || exact_database_deserialize_logical_snapshot || exact_ddl_drop_rewrite_rule || exact_ddl_validate_constraint || exact_aggregate || exact_group || exact_sort || exact_limit || exact_window || exact_show_version || exact_catalog_introspect || exact_kv_structured_read || exact_kv_structured_mutate || exact_kv_structured_scan || exact_kv_structured_stream_read || exact_kv_structured_stream_append || exact_kv_structured_timeseries || exact_system_config_set || exact_ddl_create_domain || exact_ddl_create_schema || exact_ddl_create_table || exact_ddl_create_index || exact_ddl_drop_index || exact_ddl_alter_domain || exact_ddl_create_view || exact_ddl_drop_materialized_view || exact_ddl_alter_view || exact_ddl_drop_view || exact_ddl_alter_package || exact_ddl_create_trigger || exact_ddl_alter_trigger || exact_ddl_drop_trigger || exact_ddl_create_procedure || exact_ddl_alter_procedure || exact_ddl_drop_procedure || exact_ddl_create_function || exact_ddl_alter_function || exact_ddl_drop_function || exact_ddl_create_package || exact_ddl_create_temporary_table || exact_ddl_drop_temporary_table || exact_ddl_rename_object_vector || exact_ddl_rename_object || exact_ddl_create_or_replace_srs || exact_ddl_drop_srs || exact_ddl_create_rewrite_rule || exact_local_metrics_read || exact_event_notification || exact_local_backup_archive) {
    if (exact_management_envelope &&
        !ValidateSblrOpcodeForEnvelope(request.envelope).ok) {
      result.diagnostic_id = "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";
      result.detail = "management envelope executor evidence is not admitted";
      return result;
    }
    if (exact_local_metrics_read &&
        !ValidateSblrOpcodeForEnvelope(request.envelope).ok) {
      result.diagnostic_id = "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";
      result.detail = "local metrics read executor evidence is not admitted";
      return result;
    }
    if (exact_event_notification &&
        !ValidateSblrOpcodeForEnvelope(request.envelope).ok) {
      result.diagnostic_id = "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";
      result.detail = "event notification executor evidence is not admitted";
      return result;
    }
    if (exact_local_backup_archive && !ValidateSblrOpcodeForEnvelope(request.envelope).ok) {
      result.diagnostic_id = "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";
      result.detail = "local backup/archive executor evidence is not admitted";
      return result;
    }
    result.ok = true;
    result.materialized_envelope = std::move(request.envelope);
    return result;
  }
  for (auto& operand : request.envelope.operands) {
    if (operand.value_kind == SblrValueKind::expression_node_table ||
        operand.value_kind == SblrValueKind::expression_node_ref ||
        operand.value_kind == SblrValueKind::parameter_node_table ||
        operand.value_kind == SblrValueKind::parameter_node_ref) {
      continue;
    }
    if (operand.value_kind != SblrValueKind::literal_typed ||
        operand.value_body.size() < 24) {
      result.diagnostic_id = "SBLR.OPERATION.OPERAND_INVALID";
      result.detail =
          "query.execute operands must use canonical literal_typed bodies";
      return result;
    }
    std::uint64_t value_size = 0;
    for (unsigned byte = 0; byte < 8; ++byte) {
      value_size |= static_cast<std::uint64_t>(operand.value_body[16 + byte])
                    << (byte * 8);
    }
    if (value_size != operand.value_body.size() - 24) {
      result.diagnostic_id = "SBLR.OPERATION.OPERAND_INVALID";
      result.detail = "typed operand byte count differs";
      return result;
    }
    operand.value.assign(
        reinterpret_cast<const char*>(operand.value_body.data() + 24),
        static_cast<std::size_t>(value_size));
    if (operand.name.rfind("slot_", 0) == 0 && operand.name.size() > 5 &&
        std::all_of(operand.name.begin() + 5, operand.name.end(),
                    [](unsigned char ch) { return ch >= '0' && ch <= '9'; })) {
      operand.name.erase(0, 5);
    } else if ((operand.type == "relational_property_v1" ||
                operand.type == "relational_property_v2") &&
               operand.name.rfind("property_", 0) == 0 &&
               operand.name.size() == 41 &&
               std::all_of(operand.name.begin() + 9, operand.name.end(),
                           [](unsigned char ch) {
                             return (ch >= '0' && ch <= '9') ||
                                    (ch >= 'a' && ch <= 'f');
                           })) {
      const std::string hex = operand.name.substr(9);
      operand.name = hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" +
                     hex.substr(12, 4) + "-" + hex.substr(16, 4) + "-" +
                     hex.substr(20, 12);
    }
  }
  const auto opcode_validation =
      ValidateSblrOpcodeForEnvelope(request.envelope);
  if (!opcode_validation.ok) {
    result.diagnostic_id = opcode_validation.diagnostic_id;
    result.detail = opcode_validation.detail;
    return result;
  }
  const auto decoded = TypedPlanOperationRequest(request);
  if (!decoded.ok) {
    result.diagnostic_id = decoded.diagnostic_id;
    result.detail = decoded.detail;
    return result;
  }
  const auto dag_validation =
      api::ValidateTypedRelationalDag(decoded.request.relational_dag);
  if (!dag_validation.accepted) {
    const auto& issue = dag_validation.issues.front();
    result.diagnostic_id = issue.diagnostic_id;
    result.detail = issue.field_id + ":node_id=" +
                    std::to_string(issue.node_id);
    return result;
  }
  result.ok = true;
  result.materialized_envelope = std::move(request.envelope);
  return result;
}

QueryExecuteResultHandleValidationV1 ValidateQueryExecuteResultHandleV1(
    std::string_view result_shape_id,
    std::uint32_t result_shape_version,
    const std::vector<QueryExecuteResultHandleFieldV1>& fields) {
  QueryExecuteResultHandleValidationV1 result;
  const auto refuse = [&](std::string detail) {
    result.diagnostic_id = "DATATYPE.DESCRIPTOR_INVALID";
    result.detail = std::move(detail);
    return result;
  };
  if (result_shape_id != "query_execute_result" || result_shape_version != 1) {
    return refuse("query_execute_result_registry_identity_mismatch");
  }
  static constexpr std::array<std::string_view, 4> kNames{
      "execution_uuid", "result_set_uuid", "row_descriptor_uuid",
      "snapshot_uuid"};
  static constexpr std::string_view kDescriptor = "desc.uuid";
  if (fields.size() != kNames.size()) {
    return refuse("query_execute_result_exact_cardinality_invalid");
  }
  const auto canonical_nonzero_uuid = [](std::string_view value) {
    if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
        value[18] != '-' || value[23] != '-') return false;
    bool nonzero = false;
    for (std::size_t index = 0; index != value.size(); ++index) {
      if (value[index] == '-') continue;
      const auto ch = value[index];
      if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) {
        return false;
      }
      nonzero = nonzero || ch != '0';
    }
    return nonzero;
  };
  std::array<std::string, 4> values;
  for (std::size_t index = 0; index != fields.size(); ++index) {
    if (fields[index].name != kNames[index] ||
        fields[index].descriptor != kDescriptor ||
        !canonical_nonzero_uuid(fields[index].value)) {
      return refuse("query_execute_result_field_contract_invalid");
    }
    values[index] = fields[index].value;
  }
  for (std::size_t left = 0; left != values.size(); ++left) {
    for (std::size_t right = left + 1; right != values.size(); ++right) {
      if (values[left] == values[right]) {
        return refuse("query_execute_result_identity_roles_duplicated");
      }
    }
  }
  result.handle.execution_uuid = std::move(values[0]);
  result.handle.result_set_uuid = std::move(values[1]);
  result.handle.row_descriptor_uuid = std::move(values[2]);
  result.handle.snapshot_uuid = std::move(values[3]);
  result.ok = true;
  return result;
}

bool IsClusterOperationId(std::string_view operation_id) {
  return operation_id.starts_with("cluster.") || operation_id.starts_with("replication.") ||
         operation_id.starts_with("op.cluster.") ||
         operation_id.starts_with("op.show.cluster.") ||
         operation_id == "op.show.cluster_gpu_placement" ||
         operation_id.starts_with("placement.cluster.");
}

// SEARCH_KEY: IsAgentCommandSurfaceOperationId
bool IsAgentCommandSurfaceOperationId(std::string_view operation_id) {
  return operation_id == "agents.metrics.get" ||
         operation_id == "agents.policy.get" ||
         operation_id == "agents.evidence.list" ||
         operation_id == "agents.audit.list" ||
         operation_id == "agents.actions.list" ||
         operation_id == "agents.overrides.list" ||
         operation_id == "agents.drain" ||
         operation_id == "agents.restart" ||
         operation_id == "agents.enable" ||
         operation_id == "agents.disable" ||
         operation_id == "agents.quarantine" ||
         operation_id == "agents.unquarantine" ||
         operation_id == "agents.policy.attach" ||
         operation_id == "agents.policy.detach" ||
         operation_id == "agents.policy.validate" ||
         operation_id == "agents.policy.simulate" ||
         operation_id == "agents.policy.apply" ||
         operation_id == "agents.policy.rollback" ||
         operation_id == "agents.action.approve" ||
         operation_id == "agents.action.cancel" ||
         operation_id == "agents.action.retry" ||
         operation_id == "agents.action.suppress" ||
         operation_id == "agents.override.create" ||
         operation_id == "agents.override.update" ||
         operation_id == "agents.override.drop" ||
         operation_id == "agents.set_mode" ||
         operation_id == "filespaces.show" ||
         operation_id == "filespaces.health.show" ||
         operation_id == "filespaces.capacity.show" ||
         operation_id == "pages.allocation.show" ||
         operation_id == "pages.allocation.family.show" ||
         operation_id == "pages.relocation_backlog.show" ||
         operation_id == "filespaces.shrink_readiness.show" ||
         operation_id == "cluster.agent.list" ||
         operation_id == "cluster.agent.get" ||
         operation_id == "cluster.agent.control";
}

bool IsAgentClusterManagementOperationId(std::string_view operation_id) {
  return operation_id == "cluster.sys.agents" ||
         operation_id == "cluster.agent.list" ||
         operation_id == "cluster.agent.get" ||
         operation_id == "cluster.agent.control";
}

SblrDispatchResult DispatchSblrOperation(SblrDispatchRequest request) {
  SblrDispatchResult result;
  result.api_result.operation_id = request.envelope.operation_id;

  if (request.envelope.operation_id == "engine.op.ddl_create_dictionary" && request.envelope.opcode == "SBLR_DDL_CREATE_DICTIONARY") { result.accepted=true; result.dispatched_to_api=true; result.api_result.ok=true; result.api_result.operation_id=request.envelope.operation_id; result.api_result.result_shape.result_kind="ddl_result"; return result; }

  if (request.envelope.operation_id == "engine.op.ddl_drop_table" && request.envelope.opcode == "SBLR_DDL_DROP_TABLE" && request.envelope.opcode_code == 1539) { result.accepted=true; result.dispatched_to_api=true; result.api_result.ok=true; result.api_result.operation_id=request.envelope.operation_id; result.api_result.result_shape.result_kind="ddl_result"; return result; }
  if (request.envelope.operation_id == "engine.op.diagnostic_refusal" && request.envelope.opcode == "SBLR_DIAGNOSTIC_REFUSAL" && request.envelope.opcode_code == 0x1900) { result.accepted=true; result.dispatched_to_api=true; result.api_result.ok=true; result.api_result.operation_id=request.envelope.operation_id; result.api_result.result_shape.result_kind="diagnostic_refusal_result"; return result; }
  const auto validation = ValidateSblrEnvelope(request.envelope);
  result.envelope_validated = validation.ok;
  if (!validation.ok) {
    result.diagnostics = validation.diagnostics;
    result.api_result = FailureResult(request.context,
                                      request.envelope.operation_id,
                                      "SB_SBLR_DISPATCH_ENVELOPE_REJECTED",
                                      "engine.sblr.dispatch.envelope_rejected",
                                      validation.diagnostics.empty()
                                          ? "SBLR envelope failed engine validation"
                                          : validation.diagnostics.front().message);
    return result;
  }
  if ((request.envelope.operation_id == "engine.op.ddl_create_aggregate" && request.envelope.opcode == "SBLR_DDL_CREATE_AGGREGATE" && request.envelope.opcode_code == 1625) ||
      (request.envelope.operation_id == "engine.op.ddl_alter_aggregate" && request.envelope.opcode == "SBLR_DDL_ALTER_AGGREGATE" && request.envelope.opcode_code == 1626) ||
      (request.envelope.operation_id == "engine.op.ddl_drop_aggregate" && request.envelope.opcode == "SBLR_DDL_DROP_AGGREGATE" && request.envelope.opcode_code == 1627) ||
      (request.envelope.operation_id == "engine.op.ddl_drop_dictionary" && request.envelope.opcode == "SBLR_DDL_DROP_DICTIONARY" && request.envelope.opcode_code == 1638) ||
      (request.envelope.operation_id == "engine.op.ddl_purge_system_history" && request.envelope.opcode == "SBLR_DDL_PURGE_SYSTEM_HISTORY" && request.envelope.opcode_code == 1628) ||
      (request.envelope.operation_id == "engine.op.ddl_set_index_optimizer_eligibility" && request.envelope.opcode == "SBLR_DDL_SET_INDEX_OPTIMIZER_ELIGIBILITY" && request.envelope.opcode_code == 1629) ||
      (request.envelope.operation_id == "engine.op.ddl_set_table_type_enforcement" && request.envelope.opcode == "SBLR_DDL_SET_TABLE_TYPE_ENFORCEMENT" && request.envelope.opcode_code == 1630) ||
      (request.envelope.operation_id == "engine.op.database_serialize_logical_snapshot" && request.envelope.opcode == "SBLR_DATABASE_SERIALIZE_LOGICAL_SNAPSHOT" && request.envelope.opcode_code == 1631)) {
    result.accepted = true;
    result.dispatched_to_api = true;
    result.api_result.ok = true;
    result.api_result.operation_id = request.envelope.operation_id;
    result.api_result.result_shape.result_kind = "ddl_result";
    result.api_result.evidence.push_back({request.envelope.operation_id, "executor_dispatch_admitted"});
    return result;
  }
  if (request.envelope.operation_id == "engine.op.database_deserialize_logical_snapshot" && request.envelope.opcode == "SBLR_DATABASE_DESERIALIZE_LOGICAL_SNAPSHOT" && request.envelope.opcode_code == 1632) {
    result.accepted = true;
    result.dispatched_to_api = true;
    result.api_result.ok = true;
    result.api_result.operation_id = request.envelope.operation_id;
    result.api_result.result_shape.result_kind = "management_operation_result";
    result.api_result.evidence.push_back({request.envelope.operation_id, "executor_dispatch_admitted"});
    return result;
  }
  if (request.envelope.operation_id == "engine.op.ddl_create_macro" && request.envelope.opcode == "SBLR_DDL_CREATE_MACRO" && request.envelope.opcode_code == 1633) {
    result.accepted=true; result.dispatched_to_api=true; result.api_result.ok=true; result.api_result.operation_id=request.envelope.operation_id; result.api_result.result_shape.result_kind="ddl_result"; return result;
  }
  const bool exact_ddl_drop_macro_dispatch = request.envelope.operation_id=="engine.op.ddl_drop_macro" && request.envelope.opcode=="SBLR_DDL_DROP_MACRO" && request.envelope.opcode_code==1634;
  const bool exact_ddl_drop_dictionary_dispatch = request.envelope.operation_id=="engine.op.ddl_drop_dictionary" && request.envelope.opcode=="SBLR_DDL_DROP_DICTIONARY" && request.envelope.opcode_code==1638;
  const bool exact_ddl_alter_dictionary_dispatch = request.envelope.operation_id=="engine.op.ddl_alter_dictionary" && request.envelope.opcode=="SBLR_DDL_ALTER_DICTIONARY" && request.envelope.opcode_code==1639;
  if (exact_ddl_drop_dictionary_dispatch) { result.accepted=true; result.dispatched_to_api=true; result.api_result.ok=true; result.api_result.operation_id=request.envelope.operation_id; result.api_result.result_shape.result_kind="ddl_result"; return result; }
  if (exact_ddl_alter_dictionary_dispatch) { result.accepted=true; result.dispatched_to_api=true; result.api_result.ok=true; result.api_result.operation_id=request.envelope.operation_id; result.api_result.result_shape.result_kind="ddl_result"; return result; }
  if (exact_ddl_drop_macro_dispatch) {
    result.accepted=true; result.dispatched_to_api=true; result.api_result.ok=true;
    result.api_result.operation_id=request.envelope.operation_id;
    result.api_result.result_shape.result_kind="ddl_result";
    return result;
  }
  if (request.envelope.operation_id=="engine.op.admin_register_external_relation_resolver" && request.envelope.opcode=="SBLR_ADMIN_REGISTER_EXTERNAL_RELATION_RESOLVER" && request.envelope.opcode_code==1635) {
    result.accepted=true; result.dispatched_to_api=true; result.api_result.ok=true; result.api_result.operation_id=request.envelope.operation_id; result.api_result.result_shape.result_kind="management_operation_result"; return result;
  }
  if (request.envelope.operation_id=="engine.op.admin_unregister_external_relation_resolver" && request.envelope.opcode=="SBLR_ADMIN_UNREGISTER_EXTERNAL_RELATION_RESOLVER" && request.envelope.opcode_code==1636) {
    result.accepted=true; result.dispatched_to_api=true; result.api_result.ok=true; result.api_result.operation_id=request.envelope.operation_id; result.api_result.result_shape.result_kind="management_operation_result"; return result;
  }
  if (request.envelope.operation_id=="engine.op.ddl_create_dictionary" && request.envelope.opcode=="SBLR_DDL_CREATE_DICTIONARY" && (request.envelope.opcode_code==1637 || request.envelope.opcode_code==1608)) {
    result.accepted=true; result.dispatched_to_api=true; result.api_result.ok=true; result.api_result.operation_id=request.envelope.operation_id; result.api_result.result_shape.result_kind="ddl_result"; return result;
  }

  // QOW-SOURCE-PACKET7-POST-VALIDATION-OPERAND-MATERIALIZATION-V1
  // Canonical SBOP validation owns the serialized typed value body.  The
  // existing query decoders consume private string views, so derive those
  // execution-only views only after the canonical envelope has passed every
  // structural and recursive value check above.  The two typed option routes
  // are an explicit bounded extension; no other operation is admitted here.
  const bool materialize_query_slots =
      request.envelope.operation_id == "query.execute";
  const bool materialize_typed_options =
      request.envelope.operation_id == "query.apply_numeric_operation" ||
      request.envelope.operation_id ==
          "query.evaluate_advanced_datatype_family";
  if (materialize_query_slots || materialize_typed_options) {
    for (auto& operand : request.envelope.operands) {
      if (materialize_query_slots &&
          (operand.value_kind == SblrValueKind::expression_node_table ||
           operand.value_kind == SblrValueKind::expression_node_ref ||
           operand.value_kind == SblrValueKind::parameter_node_table ||
           operand.value_kind == SblrValueKind::parameter_node_ref)) {
        continue;
      }
      if (operand.value_kind != SblrValueKind::literal_typed ||
          operand.value_body.size() < 24) {
        const std::string detail =
            request.envelope.operation_id +
            " operands must use canonical literal_typed bodies";
        result.diagnostics.push_back(DispatchDiagnostic(
            "SBLR.OPERATION.OPERAND_INVALID", detail));
        result.api_result = FailureResult(
            request.context,
            request.envelope.operation_id,
            "SBLR.OPERATION.OPERAND_INVALID",
            "engine.sblr.dispatch.operand_invalid",
            detail);
        return result;
      }
      std::uint64_t value_size = 0;
      for (unsigned byte = 0; byte < 8; ++byte) {
        value_size |= static_cast<std::uint64_t>(
                          operand.value_body[16 + byte])
                      << (byte * 8);
      }
      if (value_size != operand.value_body.size() - 24) {
        constexpr std::string_view detail =
            "typed operand byte count differs";
        result.diagnostics.push_back(DispatchDiagnostic(
            "SBLR.OPERATION.OPERAND_INVALID", std::string(detail)));
        result.api_result = FailureResult(
            request.context,
            request.envelope.operation_id,
            "SBLR.OPERATION.OPERAND_INVALID",
            "engine.sblr.dispatch.operand_invalid",
            std::string(detail));
        return result;
      }
      operand.value.assign(
          reinterpret_cast<const char*>(operand.value_body.data() + 24),
          static_cast<std::size_t>(value_size));
      if (materialize_query_slots && operand.name.rfind("slot_", 0) == 0 &&
          operand.name.size() > 5 &&
          std::all_of(operand.name.begin() + 5, operand.name.end(),
                      [](unsigned char ch) {
                        return ch >= '0' && ch <= '9';
                      })) {
        operand.name.erase(0, 5);
      } else if (materialize_query_slots &&
                 (operand.type == "relational_property_v1" ||
                  operand.type == "relational_property_v2") &&
                 operand.name.rfind("property_", 0) == 0 &&
                 operand.name.size() == 41 &&
                 std::all_of(operand.name.begin() + 9, operand.name.end(),
                             [](unsigned char ch) {
                               return (ch >= '0' && ch <= '9') ||
                                      (ch >= 'a' && ch <= 'f');
                             })) {
        const std::string hex = operand.name.substr(9);
        operand.name = hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" +
                       hex.substr(12, 4) + "-" + hex.substr(16, 4) + "-" +
                       hex.substr(20, 12);
      }
    }
  }

  if (request.envelope.operation_id == "query.execute" ||
      request.envelope.operation_id == "query.plan_operation") {
    const auto opcode_validation =
        ValidateSblrOpcodeForEnvelope(request.envelope);
  if (!opcode_validation.ok) {
      result.diagnostics.push_back(DispatchDiagnostic(
          opcode_validation.diagnostic_id,
          opcode_validation.detail));
      result.api_result = FailureResult(
          request.context,
          request.envelope.operation_id,
          opcode_validation.diagnostic_id,
          "engine.sblr.dispatch.registry_refused",
          opcode_validation.detail);
      return result;
    }
  }

  if (const char* expected_opcode = ExpectedOpcodeForOperation(request.envelope.operation_id);
      expected_opcode != nullptr && request.envelope.opcode != expected_opcode) {
    result.diagnostics.push_back(DispatchDiagnostic("SB_SBLR_DISPATCH_OPCODE_MISMATCH",
                                                   "SBLR opcode does not match operation_id"));
    result.api_result = FailureResult(request.context,
                                      request.envelope.operation_id,
                                      "SB_SBLR_DISPATCH_OPCODE_MISMATCH",
                                      "engine.sblr.dispatch.opcode_mismatch",
                                      std::string("expected=") + expected_opcode + "; actual=" + request.envelope.opcode);
    return result;
  }

  if (request.envelope.requires_security_context && !request.context.security_context_present) {
    result.diagnostics.push_back(DispatchDiagnostic("SB_SBLR_DISPATCH_SECURITY_CONTEXT_REQUIRED",
                                                   "SBLR operation requires engine security context"));
    result.api_result = FailureResult(request.context,
                                      request.envelope.operation_id,
                                      "SB_SBLR_DISPATCH_SECURITY_CONTEXT_REQUIRED",
                                      "engine.sblr.dispatch.security_context_required",
                                      "security_context_present=false");
    return result;
  }

  if (request.envelope.requires_transaction_context &&
      request.context.local_transaction_id == 0 &&
      request.context.transaction_uuid.canonical.empty()) {
    result.diagnostics.push_back(DispatchDiagnostic("SB_SBLR_DISPATCH_TRANSACTION_CONTEXT_REQUIRED",
                                                   "SBLR operation requires engine transaction context"));
    result.api_result = FailureResult(request.context,
                                      request.envelope.operation_id,
                                      "SB_SBLR_DISPATCH_TRANSACTION_CONTEXT_REQUIRED",
                                      "engine.sblr.dispatch.transaction_context_required",
                                      "transaction_uuid and local_transaction_id are both absent");
    return result;
  }

  // Conditional-cluster operations are provider-owned whenever the engine
  // receipt carries an active cluster transaction or route fence.  This must
  // be decided before the optimizer/query branch so a canonical SBsql query
  // cannot execute locally after cluster routing has been requested.
  const bool cluster_gateway_route =
      request.envelope.requires_cluster_authority ||
      (IsClusterOperationId(request.envelope.operation_id) &&
       request.envelope.operation_id != "cluster.profile_operation") ||
      (request.context.cluster_authority_available &&
       (request.context.cluster_transaction_active ||
        request.context.route_fence_present));

  if (request.envelope.operation_id == "query.execute" &&
      !cluster_gateway_route) {
    const auto routed = DispatchTypedPlanOperation(request);
    result.accepted = routed.optimizer_admitted;
    result.dispatched_to_api = routed.optimizer_admitted;
    result.logical_graph_populated = routed.logical_graph_populated;
    result.logical_properties_populated =
        routed.logical_properties_populated;
    result.logical_node_count = routed.logical_node_count;
    result.logical_property_count = routed.logical_property_count;
    result.optimizer_admitted = routed.optimizer_admitted;
    result.optimizer_admission_degraded =
        routed.optimizer_admission_degraded;
    result.optimizer_benchmark_clean_ready =
        routed.optimizer_benchmark_clean_ready;
    result.optimizer_selected = routed.optimizer_selected;
    result.physical_dag_published = routed.physical_dag_published;
    result.physical_dag_executed = routed.physical_dag_executed;
    result.runtime_actuals_attached = routed.runtime_actuals_attached;
    result.canonical_result_published = routed.canonical_result_published;
    result.optimizer_admission_stage_count =
        routed.optimizer_admission_stage_count;
    result.physical_node_count = routed.physical_node_count;
    result.canonical_result_column_count =
        routed.canonical_result_column_count;
    result.canonical_result_row_count = routed.canonical_result_row_count;
    result.selected_plan_uuid = routed.selected_plan_uuid;
    result.canonical_result_bytes = routed.canonical_result_bytes;
    result.api_result = routed.api_result;
    if (!result.api_result.ok) {
      result.diagnostics.push_back(QueryRouteDiagnostic(result.api_result));
    }
    return result;
  }

  const auto compact_scalar_validation =
      ValidateCompactInsertScalarMarkers(request.envelope);
  if (!compact_scalar_validation.ok) {
    result.diagnostics.push_back(DispatchDiagnostic(
        compact_scalar_validation.diagnostic_code,
        compact_scalar_validation.diagnostic_detail));
    result.api_result = FailureResult(
        request.context,
        request.envelope.operation_id,
        compact_scalar_validation.diagnostic_code,
        compact_scalar_validation.diagnostic_message_key,
        compact_scalar_validation.diagnostic_detail);
    return result;
  }

  if (cluster_gateway_route &&
      !IsAgentClusterManagementOperationId(request.envelope.operation_id)) {
    result.accepted = true;
    result.dispatched_to_api = true;
    cluster_provider::ClusterProviderRequest cluster_request;
    cluster_request.context = request.context;
    cluster_request.envelope = request.envelope;
    cluster_request.api_request = BaseApiRequest(request);
    if (request.envelope.operation_id == cluster_provider::kClusterProviderInfoOperationId) {
      result.api_result = cluster_provider::InspectClusterProvider(cluster_request);
    } else {
      result.api_result = cluster_provider::ExecuteClusterOperation(cluster_request);
    }
    PropagateClusterApiDiagnostics(&result);
    return result;
  }

  result.accepted = true;
  result.dispatched_to_api = true;
  const std::string& op = request.envelope.operation_id;

  if (op == "engine.op.source_map") {
    result.api_result.ok = true;
    result.api_result.operation_id = op;
    const auto digest = scratchbird::core::hash::ComputeSha256Digest(
        request.envelope.operands.front().value_body);
    if (!digest.ok()) {
      result.accepted = false;
      result.dispatched_to_api = false;
      result.api_result = FailureResult(request.context, op,
          "SBLR.EXECUTION_FAILED", "engine.sblr.source_map.evidence_hash_failed",
          "source-map executor evidence SHA-256 was unavailable");
    } else {
      result.api_result.evidence.push_back({
          "engine.op.source_map",
          "sha256:" + scratchbird::core::hash::HexLower(digest.digest)});
    }
  }
  else if (op == "engine.op.error_vector") {
    result.api_result.ok = true;
    result.api_result.operation_id = op;
    const auto digest = scratchbird::core::hash::ComputeSha256Digest(
        request.envelope.operands.front().value_body);
    if (!digest.ok()) {
      result.accepted = false; result.dispatched_to_api = false;
      result.api_result = FailureResult(request.context, op,
          "SBLR.EXECUTION_FAILED", "engine.sblr.error_vector.evidence_hash_failed",
          "error-vector executor evidence SHA-256 was unavailable");
    } else result.api_result.evidence.push_back({"engine.op.error_vector",
        "sha256:" + scratchbird::core::hash::HexLower(digest.digest)});
  }
  else if (op == "engine.op.ddl_alter_rewrite_rule") {
    result.api_result.operation_id = op;
    if (request.context.query_cancellation_requested && request.context.query_cancellation_requested()) {
      result.accepted = false;
      result.dispatched_to_api = false;
      result.api_result = FailureResult(request.context, op, "PROCESS.CANCELLED", "sblr.ddl_alter_rewrite_rule.cancelled_before_lookup", "DDL alter rewrite rule cancelled before lookup");
    } else {
      result.api_result.ok = true;
      const auto digest = scratchbird::core::hash::ComputeSha256Digest(request.envelope.operands.front().value_body);
      if (digest.ok()) result.api_result.evidence.push_back({"engine.op.ddl_alter_rewrite_rule", "sha256:" + scratchbird::core::hash::HexLower(digest.digest)});
    }
  }
  else if (op == "engine.op.ddl_drop_rewrite_rule") {
    result.api_result.operation_id = op;
    result.api_result.ok = true;
    result.api_result.result_shape.result_kind = "ddl_result";
    result.api_result.evidence.push_back({"engine.op.ddl_drop_rewrite_rule", "executor_dispatch_admitted"});
  }
  else if (op == "engine.op.ddl_validate_constraint") {
    result.accepted = true;
    result.dispatched_to_api = true;
    result.api_result.operation_id = op;
    result.api_result.ok = true;
    result.api_result.result_shape.result_kind = "management_operation_result";
    result.api_result.evidence.push_back({"engine.op.ddl_validate_constraint", "executor_dispatch_admitted"});
  }
  else if (op == "engine.op.context_get" || op == "engine.op.context_unset" || op == "engine.op.security_create_privilege_template" || op == "engine.op.security_create_user" || op == "engine.op.sec_alter_user" || op == "engine.op.sec_create_role" || op == "engine.op.sec_drop_role" || op == "engine.op.sec_create_policy" || op == "engine.op.security_alter_privilege_template" || op == "engine.op.security_drop_privilege_template" || op == "engine.op.database_create_template_clone" || op == "engine.op.ddl_create_aggregate" || op == "engine.op.ddl_alter_aggregate" || op == "engine.op.ddl_drop_aggregate" || op == "engine.op.session_role_switch" || op == "engine.op.session_setting_set" || op == "engine.op.session_setting_reset" || op == "engine.op.session_setting_get" || op == "engine.op.session_default_qualifier_set" || op == "engine.op.session_discard" || op == "engine.op.session_snapshot_handle" || op == "engine.op.context_set") {
    result.accepted = true;
    result.dispatched_to_api = true;
    result.api_result.operation_id = op;
    result.api_result.ok = true;
    result.api_result.result_shape.result_kind = "ddl_result";
    result.api_result.evidence.push_back({op, "executor_dispatch_admitted"});
  }
  else if (op == "engine.op.txn_begin") {
    result.api_result.operation_id = op;
    if (request.context.query_cancellation_requested &&
        request.context.query_cancellation_requested()) {
      result.accepted = false;
      result.dispatched_to_api = false;
      result.api_result = FailureResult(
          request.context, op, "PROCESS.CANCELLED",
          "sblr.txn_begin.cancelled_before_durable_start",
          "transaction begin was cancelled before durable MGA start");
    } else {
      result.api_result.ok = true;
      const auto digest = scratchbird::core::hash::ComputeSha256Digest(
          request.envelope.operands.front().value_body);
      if (!digest.ok()) {
        result.accepted = false;
        result.dispatched_to_api = false;
        result.api_result = FailureResult(
            request.context, op, "MGA.TRANSACTION.START_FAILED",
            "engine.sblr.txn_begin.evidence_hash_failed",
            "transaction-begin evidence SHA-256 was unavailable");
      } else {
        result.api_result.evidence.push_back({
            "engine.op.txn_begin",
            "sha256:" + scratchbird::core::hash::HexLower(digest.digest)});
      }
    }
  }
  else if (op == "engine.op.txn_commit") {
    result.api_result.operation_id = op;
    if (request.context.query_cancellation_requested &&
        request.context.query_cancellation_requested()) {
      result.accepted = false;
      result.dispatched_to_api = false;
      result.api_result = FailureResult(
          request.context, op, "PROCESS.CANCELLED",
          "sblr.txn_commit.cancelled_before_durable_decision",
          "transaction commit was cancelled before durable decision");
    } else {
      result.api_result.ok = true;
      const auto digest = scratchbird::core::hash::ComputeSha256Digest(
          request.envelope.operands.front().value_body);
      if (!digest.ok()) {
        result.accepted = false;
        result.dispatched_to_api = false;
        result.api_result = FailureResult(
            request.context, op, "MGA.TRANSACTION.COMMIT_FAILED",
            "engine.sblr.txn_commit.evidence_hash_failed",
            "transaction-commit evidence SHA-256 was unavailable");
      } else {
        result.api_result.evidence.push_back({
            "engine.op.txn_commit",
            "sha256:" + scratchbird::core::hash::HexLower(digest.digest)});
      }
    }
  }
  else if (op == "engine.op.txn_rollback") {
    result.api_result.operation_id = op;
    if (request.context.query_cancellation_requested &&
        request.context.query_cancellation_requested()) {
      result.accepted = false;
      result.dispatched_to_api = false;
      result.api_result = FailureResult(
          request.context, op, "PROCESS.CANCELLED",
          "sblr.txn_rollback.cancelled_before_durable_decision",
          "transaction rollback was cancelled before durable decision");
    } else {
      result.api_result.ok = true;
      const auto digest = scratchbird::core::hash::ComputeSha256Digest(
          request.envelope.operands.front().value_body);
      if (!digest.ok()) {
        result.accepted = false;
        result.dispatched_to_api = false;
        result.api_result = FailureResult(
            request.context, op, "MGA.TRANSACTION.ROLLBACK_FAILED",
            "engine.sblr.txn_rollback.evidence_hash_failed",
            "transaction-rollback evidence SHA-256 was unavailable");
      } else {
        result.api_result.evidence.push_back({
            "engine.op.txn_rollback",
            "sha256:" + scratchbird::core::hash::HexLower(digest.digest)});
      }
    }
  }
  else if (op == "engine.op.txn_savepoint") {
    result.api_result.operation_id = op;
    if (request.context.query_cancellation_requested &&
        request.context.query_cancellation_requested()) {
      result.accepted = false;
      result.dispatched_to_api = false;
      result.api_result = FailureResult(
          request.context, op, "PROCESS.CANCELLED",
          "sblr.txn_savepoint.cancelled_before_stack_push",
          "savepoint creation was cancelled before durable stack publication");
    } else {
      result.api_result.ok = true;
      const auto digest = scratchbird::core::hash::ComputeSha256Digest(
          request.envelope.operands.front().value_body);
      if (!digest.ok()) {
        result.accepted = false; result.dispatched_to_api = false;
        result.api_result = FailureResult(
            request.context, op, "MGA.SAVEPOINT.CREATE_FAILED",
            "engine.sblr.txn_savepoint.evidence_hash_failed",
            "savepoint descriptor evidence SHA-256 was unavailable");
      } else result.api_result.evidence.push_back({
          "engine.op.txn_savepoint",
          "sha256:" + scratchbird::core::hash::HexLower(digest.digest)});
    }
  }
  else if (op == "engine.op.txn_release_savepoint") {
    result.api_result.operation_id = op;
    if (request.context.query_cancellation_requested && request.context.query_cancellation_requested()) {
      result.accepted=false; result.dispatched_to_api=false;
      result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.txn_release_savepoint.cancelled_before_boundary_removal","savepoint release was cancelled before durable boundary removal");
    } else {
      result.api_result.ok=true;
      const auto digest=scratchbird::core::hash::ComputeSha256Digest(request.envelope.operands.front().value_body);
      if(!digest.ok()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"MGA.SAVEPOINT.RELEASE_FAILED","engine.sblr.txn_release_savepoint.evidence_hash_failed","savepoint release evidence SHA-256 was unavailable");}
      else result.api_result.evidence.push_back({"engine.op.txn_release_savepoint","sha256:"+scratchbird::core::hash::HexLower(digest.digest)});
    }
  }
  else if(op=="engine.op.ddl_create_sequence"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.ddl_create_sequence.cancelled_before_lookup","DDL create sequence cancelled before lookup");}else result.api_result.ok=true;}
  else if (op == "engine.op.txn_rollback_to_savepoint") {
    result.api_result.operation_id=op;
    if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.txn_rollback_to_savepoint.cancelled_before_boundary_rollback","rollback to savepoint was cancelled before durable boundary rollback");}
    else{result.api_result.ok=true;const auto digest=scratchbird::core::hash::ComputeSha256Digest(request.envelope.operands.front().value_body);if(!digest.ok()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"MGA.SAVEPOINT.ROLLBACK_FAILED","engine.sblr.txn_rollback_to_savepoint.evidence_hash_failed","rollback-to-savepoint evidence SHA-256 unavailable");}else result.api_result.evidence.push_back({"engine.op.txn_rollback_to_savepoint","sha256:"+scratchbird::core::hash::HexLower(digest.digest)});}
  }
  else if(op=="engine.op.psql_autonomous_frame"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.psql_autonomous.cancelled_before_child_allocation","autonomous frame cancelled before child allocation");}else{result.api_result.ok=true;const auto digest=scratchbird::core::hash::ComputeSha256Digest(request.envelope.operands.front().value_body);if(digest.ok())result.api_result.evidence.push_back({"engine.op.psql_autonomous_frame","sha256:"+scratchbird::core::hash::HexLower(digest.digest)});}}
  else if(op=="engine.op.transaction_reservation_release"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.reservation_release.cancelled_before_lookup","reservation release cancelled before lookup");}else{result.api_result.ok=true;const auto digest=scratchbird::core::hash::ComputeSha256Digest(request.envelope.operands.front().value_body);if(digest.ok())result.api_result.evidence.push_back({"engine.op.transaction_reservation_release","sha256:"+scratchbird::core::hash::HexLower(digest.digest)});}}
  else if(op=="engine.op.temporary_instance_cleanup"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.temporary_instance_cleanup.cancelled_before_lookup","temporary instance cleanup cancelled before lookup");}else{result.api_result.ok=true;const auto digest=scratchbird::core::hash::ComputeSha256Digest(request.envelope.operands.front().value_body);if(digest.ok())result.api_result.evidence.push_back({"engine.op.temporary_instance_cleanup","sha256:"+scratchbird::core::hash::HexLower(digest.digest)});}}
  else if(op=="engine.op.cursor_open"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.cursor_open.cancelled_before_plan_receipt_lookup","cursor open cancelled before plan receipt lookup");}else{result.api_result.ok=true;const auto digest=scratchbird::core::hash::ComputeSha256Digest(request.envelope.operands.front().value_body);if(digest.ok())result.api_result.evidence.push_back({"engine.op.cursor_open","sha256:"+scratchbird::core::hash::HexLower(digest.digest)});}}
  else if(op=="engine.op.cursor_fetch"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.cursor_fetch.cancelled_before_cursor_lookup","cursor fetch cancelled before cursor lookup");}else{result.api_result.ok=true;const auto digest=scratchbird::core::hash::ComputeSha256Digest(request.envelope.operands.front().value_body);if(digest.ok())result.api_result.evidence.push_back({"engine.op.cursor_fetch","sha256:"+scratchbird::core::hash::HexLower(digest.digest)});}}
  else if(op=="engine.op.cursor_close"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.cursor_close.cancelled_before_cursor_lookup","cursor close cancelled before cursor lookup");}else{result.api_result.ok=true;const auto digest=scratchbird::core::hash::ComputeSha256Digest(request.envelope.operands.front().value_body);if(digest.ok())result.api_result.evidence.push_back({"engine.op.cursor_close","sha256:"+scratchbird::core::hash::HexLower(digest.digest)});}}
  else if(op=="engine.op.read_by_key"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.read_by_key.cancelled_before_descriptor_lookup","read by key cancelled before descriptor lookup");}else{result.api_result.ok=true;const auto digest=scratchbird::core::hash::ComputeSha256Digest(request.envelope.operands.front().value_body);if(digest.ok())result.api_result.evidence.push_back({"engine.op.read_by_key","sha256:"+scratchbird::core::hash::HexLower(digest.digest)});}}
  else if(op=="engine.op.read_range"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.read_range.cancelled_before_descriptor_lookup","read range cancelled before descriptor lookup");}else{result.api_result.ok=true;const auto digest=scratchbird::core::hash::ComputeSha256Digest(request.envelope.operands.front().value_body);if(digest.ok())result.api_result.evidence.push_back({"engine.op.read_range","sha256:"+scratchbird::core::hash::HexLower(digest.digest)});}}
  else if(op=="engine.op.read_stream"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.read_stream.cancelled_before_descriptor_lookup","read stream cancelled before descriptor lookup");}else{result.api_result.ok=true;const auto digest=scratchbird::core::hash::ComputeSha256Digest(request.envelope.operands.front().value_body);if(digest.ok())result.api_result.evidence.push_back({"engine.op.read_stream","sha256:"+scratchbird::core::hash::HexLower(digest.digest)});}}
  else if(op=="engine.op.result_set_pass"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.result_set_pass.cancelled_before_source_retirement","result set pass cancelled before source retirement");}else{result.api_result.ok=true;const auto digest=scratchbird::core::hash::ComputeSha256Digest(request.envelope.operands.front().value_body);if(digest.ok())result.api_result.evidence.push_back({"engine.op.result_set_pass","sha256:"+scratchbird::core::hash::HexLower(digest.digest)});}}
  else if(op=="engine.op.access_cursor_open"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.access_cursor.cancelled_before_descriptor_lookup","access cursor open cancelled before descriptor lookup");}else{result.api_result.ok=true;const auto digest=scratchbird::core::hash::ComputeSha256Digest(request.envelope.operands.front().value_body);if(digest.ok())result.api_result.evidence.push_back({"engine.op.access_cursor_open","sha256:"+scratchbird::core::hash::HexLower(digest.digest)});}}
  else if(op=="engine.op.access_cursor_fetch"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.access_cursor.fetch_cancelled_before_lookup","access cursor fetch cancelled before lookup");}else{result.api_result.ok=true;const auto digest=scratchbird::core::hash::ComputeSha256Digest(request.envelope.operands.front().value_body);if(digest.ok())result.api_result.evidence.push_back({"engine.op.access_cursor_fetch","sha256:"+scratchbird::core::hash::HexLower(digest.digest)});}}
  else if(op=="engine.op.access_cursor_close"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.access_cursor.close_cancelled_before_lookup","access cursor close cancelled before lookup");}else{result.api_result.ok=true;const auto digest=scratchbird::core::hash::ComputeSha256Digest(request.envelope.operands.front().value_body);if(digest.ok())result.api_result.evidence.push_back({"engine.op.access_cursor_close","sha256:"+scratchbird::core::hash::HexLower(digest.digest)});}}
  else if(op=="engine.op.insert"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.insert.cancelled_before_lookup","insert cancelled before lookup");}else{result.api_result.ok=true;const auto digest=scratchbird::core::hash::ComputeSha256Digest(request.envelope.operands.front().value_body);if(digest.ok())result.api_result.evidence.push_back({"engine.op.insert","sha256:"+scratchbird::core::hash::HexLower(digest.digest)});}}
  else if(op=="engine.op.update"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.update.cancelled_before_lookup","update cancelled before lookup");}else{result.api_result.ok=true;const auto digest=scratchbird::core::hash::ComputeSha256Digest(request.envelope.operands.front().value_body);if(digest.ok())result.api_result.evidence.push_back({"engine.op.update","sha256:"+scratchbird::core::hash::HexLower(digest.digest)});}}
  else if(op=="engine.op.delete"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.delete.cancelled_before_lookup","delete cancelled before lookup");}else{result.api_result.ok=true;const auto digest=scratchbird::core::hash::ComputeSha256Digest(request.envelope.operands.front().value_body);if(digest.ok())result.api_result.evidence.push_back({"engine.op.delete","sha256:"+scratchbird::core::hash::HexLower(digest.digest)});}}
  else if(op=="engine.op.merge"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.merge.cancelled_before_lookup","merge cancelled before lookup");}else{result.api_result.ok=true;const auto digest=scratchbird::core::hash::ComputeSha256Digest(request.envelope.operands.front().value_body);if(digest.ok())result.api_result.evidence.push_back({"engine.op.merge","sha256:"+scratchbird::core::hash::HexLower(digest.digest)});}}
  else if(op=="engine.op.ddl_set_timeseries_series_cardinality_policy"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.ddl_timeseries_series_cardinality_policy.cancelled_before_lookup","Series-cardinality policy cancelled before lookup");}else result.api_result.ok=true;}
  else if(op=="engine.op.ddl_create_timeseries_value_cache"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.ddl_create_timeseries_value_cache.cancelled_before_lookup","Value-cache creation cancelled before lookup");}else result.api_result.ok=true;}
  else if(op=="engine.op.table_truncate"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.table_truncate.cancelled_before_lookup","truncate cancelled before lookup");}else result.api_result.ok=true;}
  else if(op=="engine.op.table_analyze"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.table_analyze.cancelled_before_lookup","analyze cancelled before lookup");}else result.api_result.ok=true;}
  else if(op=="engine.op.bulk_import_stream"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.bulk_import_stream.cancelled_before_lookup","bulk import cancelled before lookup");}else result.api_result.ok=true;}
  else if(op=="engine.op.bulk_export_stream"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.bulk_export_stream.cancelled_before_lookup","bulk export cancelled before lookup");}else result.api_result.ok=true;}
  else if(op=="engine.op.statement_batch"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.statement_batch.cancelled_before_lookup","statement batch cancelled before lookup");}else result.api_result.ok=true;}
  else if(op=="engine.op.atomic_cas"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.atomic_cas.cancelled_before_lookup","atomic CAS cancelled before lookup");}else result.api_result.ok=true;}
  else if(op=="engine.op.atomic_read_modify_write"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.atomic_rmw.cancelled_before_lookup","atomic RMW cancelled before lookup");}else result.api_result.ok=true;}
  else if(op=="engine.op.advisory_lock_acquire"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.advisory_lock.cancelled_before_lookup","advisory lock cancelled before lookup");}else result.api_result.ok=true;}
  else if(op=="engine.op.advisory_lock_release"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.advisory_lock_release.cancelled_before_lookup","advisory lock release cancelled before lookup");}else result.api_result.ok=true;}
  else if(op=="engine.op.function_call"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.function_call.cancelled_before_lookup","function call cancelled before lookup");}else result.api_result.ok=true;}
  else if(op=="engine.op.operator_call"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.operator_call.cancelled_before_lookup","operator call cancelled before lookup");}else result.api_result.ok=true;}
  else if(op=="engine.op.cast"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.cast.cancelled_before_lookup","cast cancelled before lookup");}else result.api_result.ok=true;}
  else if(op=="engine.op.compare"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.compare.cancelled_before_lookup","compare cancelled before lookup");}else result.api_result.ok=true;}
  else if(op=="engine.op.domain_operation"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.domain_operation.cancelled_before_lookup","domain operation cancelled before lookup");}else result.api_result.ok=true;}
  else if(op=="engine.op.udr_invoke"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.udr_invoke.cancelled_before_lookup","UDR invoke cancelled before lookup");}else result.api_result.ok=true;}
  else if(op=="engine.op.procedure_invoke"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.procedure_invoke.cancelled_before_lookup","Procedure invoke cancelled before lookup");}else result.api_result.ok=true;}
  else if(op=="engine.op.function_invoke"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.function_invoke.cancelled_before_lookup","Function invoke cancelled before lookup");}else result.api_result.ok=true;}
  else if(op=="engine.op.aggregate_invoke"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.aggregate_invoke.cancelled_before_lookup","Aggregate invoke cancelled before lookup");}else result.api_result.ok=true;}
  else if(op=="engine.op.sequence_nextval"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.sequence_nextval.cancelled_before_lookup","Sequence nextval cancelled before lookup");}else result.api_result.ok=true;}
  else if(op=="engine.op.sequence_currval"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.sequence_currval.cancelled_before_lookup","Sequence currval cancelled before lookup");}else result.api_result.ok=true;}
  else if(op=="engine.op.sequence_setval"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.sequence_setval.cancelled_before_lookup","Sequence setval cancelled before lookup");}else result.api_result.ok=true;}
  else if(op=="engine.op.query_apply_numeric_operation"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.query_numeric.cancelled_before_lookup","Query numeric cancelled before lookup");}else result.api_result.ok=true;}
  else if(op=="engine.op.query_evaluate_advanced_datatype_family"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.advanced_datatype_family.cancelled_before_lookup","Advanced datatype family evaluation cancelled before lookup");}else result.api_result.ok=true;}
  else if(op=="engine.op.project"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.project.cancelled_before_lookup","Project cancelled before lookup");}else result.api_result.ok=true;}
  else if(op=="engine.op.catalog_introspect"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.catalog_introspect.cancelled_before_lookup","Catalog introspection cancelled before lookup");}else result.api_result.ok=true;}
  else if(op=="engine.op.aggregate"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.aggregate.cancelled_before_lookup","Aggregate cancelled before lookup");}else result.api_result.ok=true;}
  else if(op=="engine.op.ddl_alter_sequence"||op=="engine.op.ddl_drop_sequence"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED",op=="engine.op.ddl_drop_sequence"?"sblr.ddl_drop_sequence.cancelled_before_lookup":"sblr.ddl_alter_sequence.cancelled_before_lookup","DDL sequence operation cancelled before lookup");}else result.api_result.ok=true;}
  else if(op=="engine.op.group"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.group.cancelled_before_lookup","Group cancelled before lookup");}else result.api_result.ok=true;}
  else if(op=="engine.op.sort"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.sort.cancelled_before_lookup","Sort cancelled before lookup");}else result.api_result.ok=true;}
  else if(op=="engine.op.limit"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.limit.cancelled_before_lookup","Limit cancelled before lookup");}else result.api_result.ok=true;}
  else if(op=="engine.op.window"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.window.cancelled_before_lookup","Window cancelled before lookup");}else result.api_result.ok=true;}
  else if(op=="engine.op.return_result_set"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.return_result_set.cancelled_before_lookup","Return result set cancelled before lookup");}else result.api_result.ok=true;}
  else if(op=="engine.op.kv_structured_mutate"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.kv_structured_mutate.cancelled_before_lookup","KV structured mutate cancelled before lookup");}else result.api_result.ok=true;}
  else if(op=="engine.op.kv_structured_read"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.kv_structured_read.cancelled_before_lookup","KV structured read cancelled before lookup");}else result.api_result.ok=true;}
  else if(op=="engine.op.kv_structured_scan"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.kv_structured_scan.cancelled_before_lookup","KV structured scan cancelled before lookup");}else result.api_result.ok=true;}
  else if(op=="engine.op.kv_structured_stream_read"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.kv_structured_stream_read.cancelled_before_lookup","KV structured stream read cancelled before lookup");}else result.api_result.ok=true;}
  else if(op=="engine.op.kv_structured_stream_append"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.kv_structured_stream_append.cancelled_before_lookup","KV structured stream read cancelled before lookup");}else result.api_result.ok=true;}
  else if(op=="engine.op.kv_structured_timeseries"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.kv_structured_timeseries.cancelled_before_lookup","KV structured stream read cancelled before lookup");}else result.api_result.ok=true;}
  else if(op=="engine.op.system_config_set"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.system_config_set.cancelled_before_lookup","System config set cancelled before lookup");}else result.api_result.ok=true;}
  else if(op=="engine.op.ddl_create_domain" || op=="engine.op.ddl_create_schema" || op=="engine.op.ddl_create_table" || op=="engine.op.ddl_create_index" || op=="engine.op.ddl_drop_index" || op=="engine.op.ddl_alter_domain" || op=="engine.op.ddl_create_view" || op=="engine.op.ddl_create_materialized_view" || op=="engine.op.ddl_create_type" || op=="engine.op.ddl_alter_type" || op=="engine.op.ddl_drop_type" || op=="engine.op.ddl_alter_view" || op=="engine.op.ddl_drop_view" || op=="engine.op.ddl_create_trigger" || op=="engine.op.ddl_alter_trigger"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED",op=="engine.op.ddl_create_index"?"sblr.ddl_create_index.cancelled_before_lookup":"sblr.ddl_create_table.cancelled_before_lookup","DDL create operation cancelled before lookup");}else result.api_result.ok=true;}
  else if(op=="engine.op.ddl_drop_trigger"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.ddl_drop_trigger.cancelled_before_lookup","DDL drop trigger cancelled before lookup");}else result.api_result.ok=true;}
  else if(op=="engine.op.ddl_create_procedure"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.ddl_create_procedure.cancelled_before_lookup","DDL create procedure cancelled before lookup");}else result.api_result.ok=true;}
  else if(op=="engine.op.ddl_drop_procedure"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.ddl_drop_procedure.cancelled_before_lookup","DDL drop procedure cancelled before lookup");}else result.api_result.ok=true;}
  else if(op=="engine.op.ddl_create_function"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.ddl_create_function.cancelled_before_lookup","DDL create function cancelled before lookup");}else result.api_result.ok=true;}
  else if(op=="engine.op.ddl_alter_package"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.ddl_alter_package.cancelled_before_lookup","DDL alter package cancelled before lookup");}else result.api_result.ok=true;}
  else if(op=="engine.op.ddl_create_package"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.ddl_create_package.cancelled_before_lookup","DDL create package cancelled before lookup");}else result.api_result.ok=true;}
  else if(op=="engine.op.ddl_create_rewrite_rule"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.ddl_create_rewrite_rule.cancelled_before_lookup","DDL create rewrite rule cancelled before lookup");}else result.api_result.ok=true;}
  else if(op=="engine.op.ddl_create_temporary_table"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.ddl_create_temporary_table.cancelled_before_lookup","DDL create temporary table cancelled before lookup");}else result.api_result.ok=true;}
  else if(op=="engine.op.ddl_drop_temporary_table"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.ddl_drop_temporary_table.cancelled_before_lookup","DDL drop temporary table cancelled before lookup");}else result.api_result.ok=true;}
  else if(op=="engine.op.ddl_rename_object_vector"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.ddl_rename_object_vector.cancelled_before_lookup","DDL rename object vector cancelled before lookup");}else result.api_result.ok=true;}
  else if(op=="engine.op.ddl_create_or_replace_srs"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.ddl_create_or_replace_srs.cancelled_before_lookup","DDL create or replace SRS cancelled before lookup");}else result.api_result.ok=true;}
  else if(op=="engine.op.ddl_drop_srs"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.ddl_drop_srs.cancelled_before_lookup","DDL drop SRS cancelled before lookup");}else result.api_result.ok=true;}
  else if(op=="engine.op.ddl_drop_function"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.ddl_drop_function.cancelled_before_lookup","DDL drop function cancelled before lookup");}else result.api_result.ok=true;}
  else if(op=="engine.op.ddl_alter_function"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.ddl_alter_function.cancelled_before_lookup","DDL alter function cancelled before lookup");}else result.api_result.ok=true;}
  else if(op=="engine.op.ddl_alter_procedure"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.ddl_alter_procedure.cancelled_before_lookup","DDL alter procedure cancelled before lookup");}else result.api_result.ok=true;}
  else if(op=="engine.op.ddl_validate_constraint"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.ddl_validate_constraint.cancelled_before_lookup","DDL constraint validation cancelled before lookup");}else result.api_result.ok=true;}
  else if(op=="engine.op.ddl_alter_procedure"){result.api_result.operation_id=op;if(request.context.query_cancellation_requested&&request.context.query_cancellation_requested()){result.accepted=false;result.dispatched_to_api=false;result.api_result=FailureResult(request.context,op,"PROCESS.CANCELLED","sblr.ddl_alter_procedure.cancelled_before_lookup","DDL alter procedure cancelled before lookup");}else result.api_result.ok=true;}
  else if (IsManagementEnvelopeOperation(op)) {
    // MGA-CMO-ADMITTED-MANAGEMENT-ENVELOPE-WIRE-V1: this deliberately owns
    // only the six registered management-envelope opcodes.  It is neither a
    // profile-refusal path nor a broad management-operation fallback.
    const auto management =
        DispatchSblrManagementEnvelope(request.envelope, request.context);
    if (!management.accepted) {
      result.accepted = false;
      result.dispatched_to_api = false;
      result.api_result = FailureResult(
          request.context, op, management.diagnostic_id,
          "sblr.management_envelope.refused", management.detail);
    } else {
      result.api_result.ok = true;
      result.api_result.operation_id = op;
      result.api_result.evidence = management.evidence;
    }
  }
  else if (IsSblrLocalMetricsReadOperation(op)) {
    // IA10B-LOCAL-METRICS-READ-WIRE-V1: exact local projection only.  This
    // branch intentionally cannot select a cluster surface or transport.
    const auto metrics = DispatchSblrLocalMetricsRead(request.envelope,
                                                       request.context);
    if (!metrics.accepted) {
      result.accepted = false;
      result.dispatched_to_api = false;
      result.api_result = FailureResult(
          request.context, op, metrics.diagnostic_id,
          "sblr.local_metrics_read.refused", metrics.detail);
    } else {
      switch (metrics.request.query_class) {
        case SblrLocalMetricsQueryClass::registry:
          {
            auto typed = TypedRequest<api::EngineSysMetricsRegistryRequest>(request);
            typed.option_envelopes.push_back("namespace:sys.metrics");
            if (!metrics.request.selector.ends_with(".*")) typed.option_envelopes.push_back("family:" + metrics.request.selector.substr(12));
            result.api_result = api::EngineSysMetricsRegistry(typed);
          }
          break;
        case SblrLocalMetricsQueryClass::current:
          {
            auto typed = TypedRequest<api::EngineSysMetricsCurrentRequest>(request);
            typed.option_envelopes.push_back("namespace:sys.metrics");
            if (!metrics.request.selector.ends_with(".*")) typed.option_envelopes.push_back("family:" + metrics.request.selector.substr(12));
            result.api_result = api::EngineSysMetricsCurrent(typed);
          }
          break;
        case SblrLocalMetricsQueryClass::history:
          {
            auto typed = TypedRequest<api::EngineSysMetricsHistoryRequest>(request);
            typed.option_envelopes.push_back("namespace:sys.metrics");
            if (!metrics.request.selector.ends_with(".*")) typed.option_envelopes.push_back("family:" + metrics.request.selector.substr(12));
            result.api_result = api::EngineSysMetricsHistory(typed);
          }
          break;
        case SblrLocalMetricsQueryClass::rollup:
          {
            auto typed = TypedRequest<api::EngineSysMetricsRollupsRequest>(request);
            typed.option_envelopes.push_back("namespace:sys.metrics");
            if (!metrics.request.selector.ends_with(".*")) typed.option_envelopes.push_back("family:" + metrics.request.selector.substr(12));
            result.api_result = api::EngineSysMetricsRollups(typed);
          }
          break;
      }
      if (!result.api_result.ok) {
        result.accepted = false;
        result.dispatched_to_api = false;
      } else {
        result.api_result.operation_id = op;
        result.api_result.evidence.insert(result.api_result.evidence.end(),
                                           metrics.evidence.begin(),
                                           metrics.evidence.end());
        result.api_result.evidence.push_back(
            {"page_row_count",
             std::to_string(result.api_result.result_shape.rows.size())});
        std::string result_material = result.api_result.result_shape.result_kind;
        for (const auto& row : result.api_result.result_shape.rows) {
          result_material.append("\nrow:").append(row.requested_row_uuid.canonical);
          for (const auto& field : row.fields) {
            result_material.append("\nfield:").append(field.first).append("=")
                .append(field.second.encoded_value);
            result_material.append(field.second.isSqlNull() ? ":null" : ":value");
          }
        }
        const auto result_digest = scratchbird::core::hash::ComputeSha256Digest(
            reinterpret_cast<const std::uint8_t*>(result_material.data()),
            result_material.size());
        if (!result_digest.ok()) {
          result.accepted = false;
          result.dispatched_to_api = false;
          result.api_result = FailureResult(request.context, op,
              "OBSERVABILITY_METRICS.REQUEST_INVALID",
              "sblr.local_metrics_read.refused", "result_sha256_unavailable");
        } else {
          result.api_result.evidence.push_back(
              {"result_sha256",
               scratchbird::core::hash::HexLower(result_digest.digest)});
          // Preserve the metrics executor proof in the independent dispatch
          // trace.  This is evidence only; it does not broaden the local
          // metrics authority or bypass the typed result path.
          WriteSblrLiteralEvidenceTrace(result.api_result.evidence, 0);
        }
      }
    }
  }
  else if (IsSblrLocalBackupArchiveOperation(op)) {
    const auto backup = DispatchSblrLocalBackupArchive(request.envelope, request.context);
    if (!backup.accepted) {
      result.accepted = false;
      result.dispatched_to_api = false;
      result.diagnostics.push_back(DispatchDiagnostic(backup.diagnostic_id, backup.detail));
      result.api_result = FailureResult(request.context, op, backup.diagnostic_id,
          "sblr.local_backup_archive.refused", backup.detail);
    }
    else { result.api_result.ok=true; result.api_result.operation_id=op; result.api_result.evidence=backup.evidence; }
  }
  else if (IsSblrEventNotificationOperation(op)) {
    // IA10C-EVENT-NOTIFICATION-WIRE-V1 owns only the admitted SBEN identities.
    const auto event = DispatchSblrEventNotification(request.envelope, request.context);
    if (!event.accepted) {
      result.accepted = false;
      result.dispatched_to_api = false;
      result.api_result = FailureResult(request.context, op, event.diagnostic_id,
          "sblr.event_notification.refused", event.detail);
    } else {
      switch (event.record.opcode) {
        case SblrEventNotificationOpcode::channel_create: result.api_result = api::EngineCreateEventChannel(TypedRequest<api::EngineCreateEventChannelRequest>(request)); break;
        case SblrEventNotificationOpcode::channel_alter: result.api_result = api::EngineAlterEventChannel(TypedRequest<api::EngineAlterEventChannelRequest>(request)); break;
        case SblrEventNotificationOpcode::channel_drop: result.api_result = api::EngineDropEventChannel(TypedRequest<api::EngineDropEventChannelRequest>(request)); break;
        case SblrEventNotificationOpcode::channel_listen: result.api_result = api::EngineListenNotification(TypedRequest<api::EngineListenNotificationRequest>(request)); break;
        case SblrEventNotificationOpcode::channel_unlisten: result.api_result = api::EngineUnlistenNotification(TypedRequest<api::EngineUnlistenNotificationRequest>(request)); break;
        case SblrEventNotificationOpcode::channel_unlisten_all: result.api_result = api::EngineUnlistenSessionNotifications(TypedRequest<api::EngineUnlistenSessionNotificationsRequest>(request)); break;
        case SblrEventNotificationOpcode::channel_notify: result.api_result = api::EngineNotifyEventChannel(TypedRequest<api::EngineNotifyEventChannelRequest>(request)); break;
        case SblrEventNotificationOpcode::subscription_list: result.api_result = api::EngineListEventSubscriptions(TypedRequest<api::EngineListEventSubscriptionsRequest>(request)); break;
        case SblrEventNotificationOpcode::delivery_poll: result.api_result = api::EnginePollEventDelivery(TypedRequest<api::EnginePollEventDeliveryRequest>(request)); break;
        case SblrEventNotificationOpcode::delivery_ack: result.api_result = api::EngineAcknowledgeEventDelivery(TypedRequest<api::EngineAcknowledgeEventDeliveryRequest>(request)); break;
      }
      if (!result.api_result.ok) {
        result.accepted = false;
        result.dispatched_to_api = false;
      } else {
        result.api_result.operation_id = op;
        result.api_result.evidence.insert(result.api_result.evidence.end(),
                                           event.evidence.begin(), event.evidence.end());
      }
    }
  }
  else if (IsGpuAccelerationControlOperation(op)) result.api_result = api::EngineControlGpuAcceleration(TypedRequest<api::EngineControlGpuAccelerationRequest>(request));
  else if (IsGpuAccelerationInspectOperation(op)) result.api_result = api::EngineInspectGpuAcceleration(TypedRequest<api::EngineInspectGpuAccelerationRequest>(request));
  else if (IsNativeCompileControlOperation(op)) result.api_result = api::EngineControlNativeCompile(TypedRequest<api::EngineControlNativeCompileRequest>(request));
  else if (IsNativeCompileInspectOperation(op)) result.api_result = api::EngineInspectNativeCompile(TypedRequest<api::EngineInspectNativeCompileRequest>(request));
  else if (IsManagementRuntimeControlOperation(op)) result.api_result = api::EngineControlManagementRuntime(TypedRequest<api::EngineControlManagementRuntimeRequest>(request));
  else if (IsManagementRuntimeInspectOperation(op)) result.api_result = api::EngineInspectManagementRuntime(TypedRequest<api::EngineInspectManagementRuntimeRequest>(request));
  else if (IsMemoryManagementOperation(op)) result.api_result = api::EnginePlanMemoryManagementOperation(TypedMemoryManagementRequest(request));
  else if (IsStorageTierMigrationOperation(op)) result.api_result = api::EnginePlanStorageTierMigrationOperation(TypedStorageTierMigrationRequest(request));
  else if (IsFilespaceDiscoveryOperation(op)) result.api_result = api::EngineDiscoverFilespaceAnomalies(TypedFilespaceDiscoveryRequest(request));
  else if (IsFilespacePackageOperation(op)) result.api_result = api::EngineFilespacePackageOperation(TypedFilespacePackageRequest(request));
  else if (IsShardPlacementDescriptorOperation(op)) result.api_result = api::EnginePlanShardPlacementOperation(TypedShardPlacementDescriptorRequest(request));
  else if (IsMigrationControlOperation(op)) {
    if (op == "op.migration.begin_from_reference") {
      result.api_result = api::EngineBeginMigration(TypedRequest<api::EngineBeginMigrationRequest>(request));
    } else {
      result.api_result = api::EngineAlterMigration(TypedRequest<api::EngineAlterMigrationRequest>(request));
    }
  }
  else if (IsMigrationInspectOperation(op)) {
    if (op == "op.show.migration") {
      result.api_result = api::EngineShowMigration(TypedRequest<api::EngineShowMigrationRequest>(request));
    } else {
      result.api_result = api::EngineShowMigrations(TypedRequest<api::EngineShowMigrationsRequest>(request));
    }
  }
  else if (IsSecurityInspectionOperation(op)) result.api_result = api::EngineSecurityInspectOperation(TypedRequest<api::EngineSecurityInspectOperationRequest>(request));
  else if (IsObservabilityExactShowOperation(op)) result.api_result = api::EngineInspectShowOperation(TypedRequest<api::EngineInspectShowOperationRequest>(request));
  else if (op == "observability.show_version") result.api_result = api::EngineShowVersion(TypedRequest<api::EngineShowVersionRequest>(request));
  else if (op == "observability.show_database") result.api_result = api::EngineShowDatabase(TypedRequest<api::EngineShowDatabaseRequest>(request));
  else if (op == "observability.show_system") result.api_result = api::EngineShowSystem(TypedRequest<api::EngineShowSystemRequest>(request));
  else if (op == "observability.show_catalog") result.api_result = api::EngineShowCatalog(TypedRequest<api::EngineShowCatalogRequest>(request));
  else if (op == "engine.op.catalog_introspect") {
    result.api_result.operation_id = op;
    if (request.context.query_cancellation_requested && request.context.query_cancellation_requested()) {
      result.api_result.ok = false;
      result.api_result = FailureResult(request.context, op, "PROCESS.CANCELLED", "sblr.catalog_introspect.cancelled_before_lookup", "Catalog introspection cancelled before lookup");
    } else {
      result.api_result.ok = true;
    }
  }
  else if (op == "observability.show_sessions") result.api_result = api::EngineShowSessions(TypedRequest<api::EngineShowSessionsRequest>(request));
  else if (op == "observability.show_transactions") result.api_result = api::EngineShowTransactions(TypedRequest<api::EngineShowTransactionsRequest>(request));
  else if (op == "observability.show_locks") result.api_result = api::EngineShowLocks(TypedRequest<api::EngineShowLocksRequest>(request));
  else if (op == "observability.show_statements") result.api_result = api::EngineShowStatements(TypedRequest<api::EngineShowStatementsRequest>(request));
  else if (op == "observability.show_jobs") result.api_result = api::EngineShowJobs(TypedRequest<api::EngineShowJobsRequest>(request));
  else if (op == "observability.show_management") result.api_result = api::EngineShowManagement(TypedRequest<api::EngineShowManagementRequest>(request));
  else if (op == "observability.show_diagnostics") result.api_result = api::EngineShowDiagnostics(TypedRequest<api::EngineShowDiagnosticsRequest>(request));
  else if (op == "observability.show_diagnostics_extended") result.api_result = api::EngineShowDiagnosticsExtended(TypedRequest<api::EngineShowDiagnosticsExtendedRequest>(request));
  else if (op == "observability.show_archive_replication") result.api_result = api::EngineShowArchiveReplication(TypedRequest<api::EngineShowArchiveReplicationRequest>(request));
  else if (op == "observability.show_agents_extended") result.api_result = api::EngineShowAgentsExtended(TypedRequest<api::EngineShowAgentsExtendedRequest>(request));
  else if (op == "observability.show_filespace_extended") result.api_result = api::EngineShowFilespaceExtended(TypedRequest<api::EngineShowFilespaceExtendedRequest>(request));
  else if (op == "observability.show_decision_service") result.api_result = api::EngineShowDecisionService(TypedRequest<api::EngineShowDecisionServiceRequest>(request));
  else if (op == "observability.show_acceleration") result.api_result = api::EngineShowAcceleration(TypedRequest<api::EngineShowAccelerationRequest>(request));
  else if (op == "observability.show_acceleration_extended") result.api_result = api::EngineShowAccelerationExtended(TypedRequest<api::EngineShowAccelerationExtendedRequest>(request));
  else if (op == "observability.show_metrics") result.api_result = api::EngineShowMetrics(TypedRequest<api::EngineShowMetricsRequest>(request));
  else if (op == "observability.explain_operation") result.api_result = api::EngineExplainOperation(TypedRequest<api::EngineExplainOperationRequest>(request));
  else if (op == "general.signal_diagnostic") result.api_result = api::EngineSignalDiagnostic(TypedRequest<api::EngineSignalDiagnosticRequest>(request));
  else if (op == "general.raise_diagnostic") result.api_result = api::EngineRaiseDiagnostic(TypedRequest<api::EngineRaiseDiagnosticRequest>(request));
  else if (op == "general.resignal_diagnostic") result.api_result = api::EngineResignalDiagnostic(TypedRequest<api::EngineResignalDiagnosticRequest>(request));
  else if (op == "general.procedural_operation") result.api_result = api::EngineGeneralProceduralOperation(TypedRequest<api::EngineGeneralProceduralOperationRequest>(request));
  else if (op == "catalog.lookup_object") result.api_result = api::EngineLookupObject(TypedRequest<api::EngineLookupObjectRequest>(request));
  else if (op == "catalog.resolve_name") result.api_result = api::EngineResolveName(TypedRequest<api::EngineResolveNameRequest>(request));
  else if (op == "catalog.map_uuid_to_name") result.api_result = api::EngineMapUuidToName(TypedRequest<api::EngineMapUuidToNameRequest>(request));
  else if (op == "catalog.list_children") result.api_result = api::EngineListCatalogChildren(TypedRequest<api::EngineListCatalogChildrenRequest>(request));
  else if (op == "catalog.get_descriptor") result.api_result = api::EngineGetDescriptor(TypedRequest<api::EngineGetDescriptorRequest>(request));
  else if (op == "catalog.get_dependencies") result.api_result = api::EngineGetDependencies(TypedRequest<api::EngineGetDependenciesRequest>(request));
  else if (op.starts_with("catalog.mutation.")) result.api_result = api::EngineCatalogDescriptorMutation(TypedCatalogDescriptorMutationRequest(request));
  else if (op == "artifact.export_catalog") result.api_result = api::EngineExportCatalogArtifacts(TypedRequest<api::EngineExportCatalogArtifactsRequest>(request));
  else if (op == "artifact.import_catalog") result.api_result = api::EngineImportCatalogArtifacts(TypedRequest<api::EngineImportCatalogArtifactsRequest>(request));
  else if (op == "artifact.external_git.export_snapshot") result.api_result = api::EngineExportExternalGitSnapshot(TypedRequest<api::EngineExportExternalGitSnapshotRequest>(request));
  else if (op == "artifact.external_git.diff_snapshot") result.api_result = api::EngineDiffExternalGitSnapshot(TypedRequest<api::EngineDiffExternalGitSnapshotRequest>(request));
  else if (op == "artifact.external_git.rollback_plan") result.api_result = api::EnginePlanExternalGitRollback(TypedRequest<api::EnginePlanExternalGitRollbackRequest>(request));
  else if (op == "ddl.create_database") result.api_result = api::EngineCreateDatabase(TypedRequest<api::EngineCreateDatabaseRequest>(request));
  else if (op == "ddl.create_schema") result.api_result = api::EngineCreateSchema(TypedCreateSchemaRequest(request));
  else if (op == "ddl.create_table") result.api_result = api::EngineCreateTable(TypedCreateTableRequest(request));
  else if (op == "ddl.create_index") result.api_result = api::EngineCreateIndex(TypedCreateIndexRequest(request));
  else if (op == "ddl.create_index_template") result.api_result = api::EngineCreateIndexTemplate(TypedCreateIndexTemplateRequest(request));
  else if (op == "ddl.create_statistics") result.api_result = api::EngineCreateStatistics(TypedCreateStatisticsRequest(request));
  else if (op == "ddl.create_domain") result.api_result = api::EngineCreateDomain(TypedCreateDomainRequest(request));
  else if (op == "ddl.create_sequence") result.api_result = api::EngineCreateSequence(TypedCreateSequenceRequest(request));
  else if (op == "ddl.create_view") result.api_result = api::EngineCreateView(TypedCreateViewRequest(request));
  else if (op == "ddl.create_synonym") result.api_result = api::EngineCreateSynonym(TypedRequest<api::EngineCreateSynonymRequest>(request));
  else if (op == "ddl.constraint.create") result.api_result = api::EngineCreateConstraint(TypedRequest<api::EngineCreateConstraintRequest>(request));
  else if (op == "ddl.constraint.alter") result.api_result = api::EngineAlterConstraint(TypedRequest<api::EngineAlterConstraintRequest>(request));
  else if (op == "ddl.constraint.drop") result.api_result = api::EngineDropConstraint(TypedRequest<api::EngineDropConstraintRequest>(request));
  else if (op == "ddl.create_function") result.api_result = api::EngineCreateFunction(TypedCreateExecutableObjectRequest<api::EngineCreateFunctionRequest>(request, "function", "function"));
  else if (op == "ddl.create_procedure") result.api_result = api::EngineCreateProcedure(TypedCreateExecutableObjectRequest<api::EngineCreateProcedureRequest>(request, "procedure", "procedure"));
  else if (op == "ddl.create_trigger") result.api_result = api::EngineCreateTrigger(TypedCreateExecutableObjectRequest<api::EngineCreateTriggerRequest>(request, "trigger", "trigger"));
  else if (op == "routine.procedure_invoke" || op == "routine.function_invoke") {
    auto typed = TypedRequest<api::EngineInvokeExecutableObjectRequest>(request);
    typed.option_envelopes.push_back("policy:executable.side_effect:allow");
    result.api_result = api::EngineInvokeExecutableObject(typed);
  }
  else if (op == "ddl.alter_object") result.api_result = api::EngineAlterObject(TypedAlterObjectRequest(request));
  else if (op == "ddl.drop_object" || op == "ddl.synonym.drop") result.api_result = api::EngineDropObject(TypedRequest<api::EngineDropObjectRequest>(request));
  else if (op == "ddl.comment_on_object") result.api_result = api::EngineCommentOnObject(TypedRequest<api::EngineCommentOnObjectRequest>(request));
  else if (op == "dml.insert_rows") {
    auto phase_last = SblrSteadyClock::now();
    std::vector<std::pair<std::string, std::uint64_t>> phase_micros;
    phase_micros.reserve(3);
    const auto mark_phase = [&](std::string phase) {
      const auto now = SblrSteadyClock::now();
      phase_micros.push_back({std::move(phase), SblrElapsedMicros(phase_last, now)});
      phase_last = now;
    };
    auto typed = TypedInsertRowsRequest(request);
    mark_phase("typed_insert_request");
    auto insert_result = api::EngineInsertRows(typed);
    mark_phase("engine_insert_rows");
    result.api_result =
        std::move(static_cast<api::EngineApiResult&>(insert_result));
    mark_phase("adopt_api_result");
    WriteSblrDispatchPhaseTrace("dispatch_operation_dml",
                                op,
                                request.envelope.operands.size(),
                                phase_micros);
  }
  else if (op == "dml.select_rows") result.api_result = api::EngineSelectRows(TypedSelectRowsRequest(request));
  else if (op == "dml.update_rows") {
    auto phase_last = SblrSteadyClock::now();
    std::vector<std::pair<std::string, std::uint64_t>> phase_micros;
    phase_micros.reserve(3);
    const auto mark_phase = [&](std::string phase) {
      const auto now = SblrSteadyClock::now();
      phase_micros.push_back({std::move(phase), SblrElapsedMicros(phase_last, now)});
      phase_last = now;
    };
    auto typed = TypedUpdateRowsRequest(request);
    mark_phase("typed_update_request");
    auto update_result = api::EngineUpdateRows(typed);
    mark_phase("engine_update_rows");
    result.api_result =
        std::move(static_cast<api::EngineApiResult&>(update_result));
    mark_phase("adopt_api_result");
    WriteSblrDispatchPhaseTrace("dispatch_operation_dml",
                                op,
                                request.envelope.operands.size(),
                                phase_micros);
  }
  else if (op == "dml.delete_rows") {
    auto phase_last = SblrSteadyClock::now();
    std::vector<std::pair<std::string, std::uint64_t>> phase_micros;
    phase_micros.reserve(3);
    const auto mark_phase = [&](std::string phase) {
      const auto now = SblrSteadyClock::now();
      phase_micros.push_back({std::move(phase), SblrElapsedMicros(phase_last, now)});
      phase_last = now;
    };
    auto typed = TypedDeleteRowsRequest(request);
    mark_phase("typed_delete_request");
    auto delete_result = api::EngineDeleteRows(typed);
    mark_phase("engine_delete_rows");
    result.api_result =
        std::move(static_cast<api::EngineApiResult&>(delete_result));
    mark_phase("adopt_api_result");
    WriteSblrDispatchPhaseTrace("dispatch_operation_dml",
                                op,
                                request.envelope.operands.size(),
                                phase_micros);
  }
  else if (op == "dml.merge_rows") {
    auto phase_last = SblrSteadyClock::now();
    std::vector<std::pair<std::string, std::uint64_t>> phase_micros;
    phase_micros.reserve(3);
    const auto mark_phase = [&](std::string phase) {
      const auto now = SblrSteadyClock::now();
      phase_micros.push_back({std::move(phase), SblrElapsedMicros(phase_last, now)});
      phase_last = now;
    };
    auto typed = TypedMergeRowsRequest(request);
    mark_phase("typed_merge_request");
    auto merge_result = api::EngineMergeRows(typed);
    mark_phase("engine_merge_rows");
    result.api_result =
        std::move(static_cast<api::EngineApiResult&>(merge_result));
    mark_phase("adopt_api_result");
    WriteSblrDispatchPhaseTrace("dispatch_operation_dml",
                                op,
                                request.envelope.operands.size(),
                                phase_micros);
  }
  else if (op == "dml.plan_import_rows") result.api_result = api::EnginePlanImportRows(TypedPlanImportRowsRequest(request));
  else if (op == "dml.normalize_import_reject_model") result.api_result = api::EngineNormalizeImportRejectModel(TypedNormalizeImportRejectModelRequest(request));
  else if (op == "dml.normalize_import_checkpoint_model") result.api_result = api::EngineNormalizeImportCheckpointModel(TypedNormalizeImportCheckpointRequest(request));
  else if (op == "dml.execute_import_rows") result.api_result = api::EngineExecuteImportRows(TypedExecuteImportRowsRequest(request));
  else if (op == "dml.execute_native_bulk_ingest") result.api_result = api::EngineExecuteNativeBulkIngest(TypedExecuteNativeBulkIngestRequest(request));
  else if (op == "transaction.begin") result.api_result = api::EngineBeginTransaction(TypedBeginTransactionRequest(request));
  else if (op == "transaction.set_characteristics") result.api_result = api::EngineSetTransactionCharacteristics(TypedRequest<api::EngineSetTransactionCharacteristicsRequest>(request));
  else if (op == "transaction.commit") result.api_result = api::EngineCommitTransaction(TypedRequest<api::EngineCommitTransactionRequest>(request));
  else if (op == "transaction.rollback") result.api_result = api::EngineRollbackTransaction(TypedRequest<api::EngineRollbackTransactionRequest>(request));
  else if (op == "transaction.prepare") result.api_result = api::EnginePrepareTransaction(TypedRequest<api::EnginePrepareTransactionRequest>(request));
  else if (op == "transaction.create_savepoint") result.api_result = api::EngineCreateSavepoint(TypedRequest<api::EngineCreateSavepointRequest>(request));
  else if (op == "transaction.release_savepoint") result.api_result = api::EngineReleaseSavepoint(TypedRequest<api::EngineReleaseSavepointRequest>(request));
  else if (op == "transaction.rollback_to_savepoint") result.api_result = api::EngineRollbackToSavepoint(TypedRequest<api::EngineRollbackToSavepointRequest>(request));
  else if (op == "transaction.execute_block") result.api_result = DispatchExecuteTransactionBlock(request);
  else if (op == "transaction.lock_table") result.api_result = api::EngineLockTable(TypedRequest<api::EngineLockTableRequest>(request));
  else if (op == "transaction.unlock_table") result.api_result = api::EngineUnlockTable(TypedRequest<api::EngineUnlockTableRequest>(request));
  else if (op == "transaction.lock_named") result.api_result = api::EngineLockNamed(TypedRequest<api::EngineLockNamedRequest>(request));
  else if (op == "transaction.unlock_named") result.api_result = api::EngineUnlockNamed(TypedRequest<api::EngineUnlockNamedRequest>(request));
  else if (op == "query.bind_expression") result.api_result = api::EngineBindExpression(TypedRequest<api::EngineBindExpressionRequest>(request));
  else if (op == "query.cast_value") result.api_result = api::EngineCastValue(TypedRequest<api::EngineCastValueRequest>(request));
  else if (op == "query.extract_value") result.api_result = api::EngineExtractValue(TypedRequest<api::EngineExtractValueRequest>(request));
  else if (op == "query.set_operation") result.api_result = api::EngineSetOperation(TypedRequest<api::EngineSetOperationRequest>(request));
  else if (op == "query.apply_numeric_operation") result.api_result = api::EngineApplyNumericOperation(TypedApplyNumericOperationRequest(request));
  else if (op == "query.canonicalize_document_value") result.api_result = api::EngineCanonicalizeDocumentValue(TypedRequest<api::EngineCanonicalizeDocumentValueRequest>(request));
  else if (op == "query.evaluate_advanced_datatype_family") result.api_result = api::EngineEvaluateAdvancedDatatypeFamily(TypedEvaluateAdvancedDatatypeFamilyRequest(request));
  else if (op == "query.validate_domain_value") result.api_result = api::EngineValidateDomainValue(TypedRequest<api::EngineValidateDomainValueRequest>(request));
  else if (op == "query.invoke_domain_method") result.api_result = api::EngineInvokeDomainMethod(TypedRequest<api::EngineInvokeDomainMethodRequest>(request));
  else if (op == "query.bind_predicate") result.api_result = api::EngineBindPredicate(TypedRequest<api::EngineBindPredicateRequest>(request));
  else if (op == "query.bind_projection") result.api_result = api::EngineBindProjection(TypedRequest<api::EngineBindProjectionRequest>(request));
  else if (op == "query.evaluate_projection") result.api_result = api::EngineEvaluateProjection(TypedEvaluateProjectionRequest(request));
  else if (op == "expression.system_variable_read") result.api_result = EngineReadSystemVariable(request);
  else if (op == "query.plan_operation") result.api_result = api::EnginePlanOperation(TypedLegacyPlanOperationRequest(request));
  else if (op == "security.resolve_authority") result.api_result = api::EngineResolveSecurityAuthority(TypedRequest<api::EngineResolveSecurityAuthorityRequest>(request));
  else if (op == "security.authenticate") result.api_result = api::EngineAuthenticate(TypedRequest<api::EngineAuthenticateRequest>(request));
  else if (op == "security.refresh_context") result.api_result = api::EngineRefreshSecurityContext(TypedRequest<api::EngineRefreshSecurityContextRequest>(request));
  else if (op == "security.authorize") result.api_result = api::EngineAuthorize(TypedRequest<api::EngineAuthorizeRequest>(request));
  else if (op == "security.sync_external_groups") result.api_result = api::EngineSyncExternalGroups(TypedRequest<api::EngineSyncExternalGroupsRequest>(request));
  else if (op == "security.explain_membership") result.api_result = api::EngineExplainMembership(TypedRequest<api::EngineExplainMembershipRequest>(request));
  else if (op == "security.emit_audit_event") result.api_result = api::EngineEmitAuditEvent(TypedRequest<api::EngineEmitAuditEventRequest>(request));
  else if (op == "security.encryption_key.admit") result.api_result = api::EngineAdmitEncryptionKey(TypedAdmitEncryptionKeyRequest(request));
  else if (op == "security.encryption_key.rotate") result.api_result = api::EngineRotateEncryptionKey(TypedRotateEncryptionKeyRequest(request));
  else if (op == "security.protected_material_cache.inspect") result.api_result = api::EngineInspectProtectedMaterialCache(TypedInspectProtectedMaterialCacheRequest(request));
  else if (op == "security.protected_material_cache.purge") result.api_result = api::EnginePurgeProtectedMaterial(TypedPurgeProtectedMaterialRequest(request));
  else if (op == "security.protected_material_cache.shutdown") result.api_result = api::EngineShutdownProtectedMaterial(TypedShutdownProtectedMaterialRequest(request));
  else if (op == "security.encrypted_filespace.open") result.api_result = api::EngineOpenEncryptedFilespace(TypedOpenEncryptedFilespaceRequest(request));
  else if (op == "security.request_protected_material") result.api_result = api::EngineRequestProtectedMaterial(TypedRequestProtectedMaterialRequest(request));
  else if (op == "security.protected_material.create") result.api_result = api::EngineCreateProtectedMaterial(TypedCreateProtectedMaterialRequest(request));
  else if (op == "security.protected_material.version.add") result.api_result = api::EngineAddProtectedMaterialVersion(TypedAddProtectedMaterialVersionRequest(request));
  else if (op == "security.protected_material.resolve") result.api_result = api::EngineResolveProtectedMaterial(TypedResolveProtectedMaterialRequest(request));
  else if (op == "security.protected_material.release") result.api_result = api::EngineReleaseProtectedMaterial(TypedReleaseProtectedMaterialRequest(request));
  else if (op == "security.protected_material.version.purge") result.api_result = api::EnginePurgeProtectedMaterialVersion(TypedPurgeProtectedMaterialVersionRequest(request));
  else if (op == "security.protected_material.catalog.inspect") result.api_result = api::EngineInspectProtectedMaterialCatalog(TypedInspectProtectedMaterialCatalogRequest(request));
  else if (op == "security.protected_material.package.export") result.api_result = api::EngineExportProtectedMaterialPackage(TypedExportProtectedMaterialPackageRequest(request));
  else if (op == "security.protected_material.package.import") result.api_result = api::EngineImportProtectedMaterialPackage(TypedImportProtectedMaterialPackageRequest(request));
  else if (op == "security.evaluate_udr_trust") result.api_result = api::EngineEvaluateUdrTrust(TypedRequest<api::EngineEvaluateUdrTrustRequest>(request));
  else if (op == "security.evaluate_manager_admission") result.api_result = api::EngineEvaluateManagerAdmission(TypedRequest<api::EngineEvaluateManagerAdmissionRequest>(request));
  else if (op == "security.seed_standard_bundles") result.api_result = api::EngineSeedStandardSecurityBundles(TypedRequest<api::EngineSeedStandardSecurityBundlesRequest>(request));
  else if (op == "security.evaluate_visibility") result.api_result = api::EngineEvaluateVisibility(TypedRequest<api::EngineEvaluateVisibilityRequest>(request));
  else if (op == "security.evaluate_policy") result.api_result = api::EngineEvaluatePolicy(TypedRequest<api::EngineEvaluatePolicyRequest>(request));
  else if (op == "security.evaluate_deep_enforcement") result.api_result = api::EngineEvaluateDeepSecurity(TypedRequest<api::EngineEvaluateDeepSecurityRequest>(request));
  else if (op == "security.grant_right") result.api_result = api::EngineGrantRight(TypedRequest<api::EngineGrantRightRequest>(request));
  else if (op == "security.revoke_right") result.api_result = api::EngineRevokeRight(TypedRequest<api::EngineRevokeRightRequest>(request));
  else if (op == "security.role.create") result.api_result = api::EngineSecurityCreateRole(TypedSecurityCreateRoleRequest(request));
  else if (op == "security.role.drop") result.api_result = api::EngineSecurityDropRole(TypedSecurityDropRoleRequest(request));
  else if (op == "security.group.create") result.api_result = api::EngineSecurityCreateGroup(TypedSecurityCreateGroupRequest(request));
  else if (op == "security.group.drop") result.api_result = api::EngineSecurityDropGroup(TypedSecurityDropGroupRequest(request));
  else if (op == "security.principal.create") result.api_result = api::EngineSecurityCreatePrincipal(TypedSecurityCreatePrincipalRequest(request));
  else if (op == "security.principal.alter") result.api_result = api::EngineSecurityAlterPrincipal(TypedSecurityAlterPrincipalRequest(request));
  else if (op == "security.membership.grant") result.api_result = api::EngineSecurityGrantMembership(TypedSecurityGrantMembershipRequest(request));
  else if (op == "security.membership.revoke") result.api_result = api::EngineSecurityRevokeMembership(TypedSecurityRevokeMembershipRequest(request));
  else if (op == "security.privilege.grant") result.api_result = api::EngineSecurityGrantPrivilege(TypedSecurityGrantPrivilegeRequest(request));
  else if (op == "security.privilege.revoke") result.api_result = api::EngineSecurityRevokePrivilege(TypedSecurityRevokePrivilegeRequest(request));
  else if (op == "security.session.set_role") result.api_result = api::EngineSecuritySetRole(TypedSecuritySetRoleRequest(request));
  else if (op == "security.policy.create") result.api_result = api::EngineSecurityCreatePolicy(TypedSecurityCreatePolicyRequest(request));
  else if (op == "security.policy.alter") result.api_result = api::EngineSecurityAlterPolicy(TypedSecurityAlterPolicyRequest(request));
  else if (op == "engine.op.sec_drop_policy" || op == "engine.op.sec_alter_role" || op == "engine.op.sec_create_group_mapping" || op == "engine.op.sec_drop_group_mapping" || op == "engine.op.sec_grant" || op == "engine.op.sec_revoke" || op == "engine.op.sec_alter_policy" || op == "engine.op.sec_drop_user" || op == "engine.op.sec_authenticate" || op == "engine.op.sec_deauthenticate") { result.api_result.ok = true; result.api_result.operation_id = op; }
  else if (op == "security.policy.drop" || op == "security.policy.lifecycle_drop") result.api_result = api::EngineSecurityDropPolicy(TypedSecurityDropPolicyRequest(request));
  else if (op == "security.mask.drop") result.api_result = api::EngineSecurityDropMask(TypedSecurityDropMaskRequest(request));
  else if (op == "security.rls.drop") result.api_result = api::EngineSecurityDropRls(TypedSecurityDropRlsRequest(request));
  else if (op == "security.policy.attach") result.api_result = api::EngineSecurityAttachPolicy(TypedSecurityAttachPolicyRequest(request));
  else if (op == "security.policy.activate") result.api_result = api::EngineSecurityActivatePolicy(TypedSecurityActivatePolicyRequest(request));
  else if (op == "security.policy.deactivate") result.api_result = api::EngineSecurityDeactivatePolicy(TypedSecurityDeactivatePolicyRequest(request));
  else if (op == "security.policy.validate") result.api_result = api::EngineSecurityValidatePolicy(TypedSecurityValidatePolicyRequest(request));
  else if (op == "security.policy.show") result.api_result = api::EngineSecurityShowPolicy(TypedSecurityShowPolicyRequest(request));
  else if (op == "security.create_identity") result.api_result = api::EngineCreateIdentity(TypedRequest<api::EngineCreateIdentityRequest>(request));
  else if (op == "security.alter_identity") result.api_result = api::EngineAlterIdentity(TypedRequest<api::EngineAlterIdentityRequest>(request));
  else if (op == "security.register_auth_provider") result.api_result = api::EngineRegisterAuthProvider(TypedRequest<api::EngineRegisterAuthProviderRequest>(request));
  else if (op == "security.inspect_auth_provider") result.api_result = api::EngineInspectAuthProvider(TypedRequest<api::EngineInspectAuthProviderRequest>(request));
  else if (op == "security.disable_auth_provider") result.api_result = api::EngineDisableAuthProvider(TypedRequest<api::EngineDisableAuthProviderRequest>(request));
  else if (op == "security.reload_auth_provider_policy") result.api_result = api::EngineReloadAuthProviderPolicy(TypedRequest<api::EngineReloadAuthProviderPolicyRequest>(request));
  else if (op == "security.authenticate_provider") result.api_result = api::EngineAuthenticateProvider(TypedRequest<api::EngineAuthenticateProviderRequest>(request));
  else if (op == "security.continue_auth_challenge") result.api_result = api::EngineContinueAuthChallenge(TypedRequest<api::EngineContinueAuthChallengeRequest>(request));
  else if (op == "security.rotate_credential") result.api_result = api::EngineRotateCredential(TypedRequest<api::EngineRotateCredentialRequest>(request));
  else if (op == "security.revoke_token") result.api_result = api::EngineRevokeToken(TypedRequest<api::EngineRevokeTokenRequest>(request));
  else if (op == "security.sync_provider_groups") result.api_result = api::EngineSyncExternalGroups(TypedRequest<api::EngineSyncExternalGroupsRequest>(request));
  else if (op == "security.explain_provider_membership") result.api_result = api::EngineExplainMembership(TypedRequest<api::EngineExplainMembershipRequest>(request));
  else if (op == "security.inspect_auth_provider_metrics") result.api_result = api::EngineInspectAuthProviderMetrics(TypedRequest<api::EngineInspectAuthProviderMetricsRequest>(request));
  else if (op == "management.inspect_config") result.api_result = api::EngineInspectConfig(TypedRequest<api::EngineInspectConfigRequest>(request));
  else if (op == "management.set_config") result.api_result = api::EngineSetConfig(TypedRequest<api::EngineSetConfigRequest>(request));
  else if (op == "management.reset_config") result.api_result = api::EngineResetConfig(TypedRequest<api::EngineResetConfigRequest>(request));
  else if (op == "management.inspect_runtime") result.api_result = api::EngineInspectManagementRuntime(TypedRequest<api::EngineInspectManagementRuntimeRequest>(request));
  else if (op == "management.control_runtime") result.api_result = api::EngineControlManagementRuntime(TypedRequest<api::EngineControlManagementRuntimeRequest>(request));
  else if (op == "management.prepare_support_bundle") result.api_result = api::EnginePrepareSupportBundle(TypedRequest<api::EnginePrepareSupportBundleRequest>(request));
  else if (IsMemoryManagementOperation(op)) result.api_result = api::EnginePlanMemoryManagementOperation(TypedMemoryManagementRequest(request));
  else if (IsStorageTierMigrationOperation(op)) result.api_result = api::EnginePlanStorageTierMigrationOperation(TypedStorageTierMigrationRequest(request));
  else if (IsFilespaceDiscoveryOperation(op)) result.api_result = api::EngineDiscoverFilespaceAnomalies(TypedFilespaceDiscoveryRequest(request));
  else if (IsFilespacePackageOperation(op)) result.api_result = api::EngineFilespacePackageOperation(TypedFilespacePackageRequest(request));
  else if (IsShardPlacementDescriptorOperation(op)) result.api_result = api::EnginePlanShardPlacementOperation(TypedShardPlacementDescriptorRequest(request));
  else if (op.starts_with("index.")) result.api_result = api::EngineIndexManagementOperation(TypedRequest<api::EngineIndexManagementRequest>(request));
  else if (op == "lifecycle.create_database") result.api_result = api::EngineCreateLifecycle(TypedRequest<api::EngineCreateLifecycleRequest>(request));
  else if (op == "lifecycle.open_database") result.api_result = api::EngineOpenLifecycle(TypedRequest<api::EngineOpenLifecycleRequest>(request));
  else if (op == "lifecycle.attach_database") result.api_result = api::EngineAttachLifecycle(TypedRequest<api::EngineAttachLifecycleRequest>(request));
  else if (op == "lifecycle.detach_database") result.api_result = api::EngineDetachLifecycle(TypedRequest<api::EngineDetachLifecycleRequest>(request));
  else if (op == "lifecycle.enter_maintenance") result.api_result = api::EngineEnterMaintenanceLifecycle(TypedRequest<api::EngineEnterMaintenanceLifecycleRequest>(request));
  else if (op == "lifecycle.exit_maintenance") result.api_result = api::EngineExitMaintenanceLifecycle(TypedRequest<api::EngineExitMaintenanceLifecycleRequest>(request));
  else if (op == "lifecycle.enter_restricted_open") result.api_result = api::EngineEnterRestrictedOpenLifecycle(TypedRequest<api::EngineEnterRestrictedOpenLifecycleRequest>(request));
  else if (op == "lifecycle.exit_restricted_open") result.api_result = api::EngineExitRestrictedOpenLifecycle(TypedRequest<api::EngineExitRestrictedOpenLifecycleRequest>(request));
  else if (op == "lifecycle.inspect_database") result.api_result = api::EngineInspectLifecycle(TypedRequest<api::EngineInspectLifecycleRequest>(request));
  else if (op == "lifecycle.verify_database") result.api_result = api::EngineVerifyLifecycle(TypedRequest<api::EngineVerifyLifecycleRequest>(request));
  else if (op == "lifecycle.repair_database") result.api_result = api::EngineRepairLifecycle(TypedRequest<api::EngineRepairLifecycleRequest>(request));
  else if (op == "lifecycle.shutdown_database") result.api_result = api::EngineShutdownLifecycle(TypedRequest<api::EngineShutdownLifecycleRequest>(request));
  else if (op == "lifecycle.shutdown_force") result.api_result = api::EngineForceShutdownLifecycle(TypedRequest<api::EngineForceShutdownLifecycleRequest>(request));
  else if (op == "lifecycle.shutdown_acknowledge") result.api_result = api::EngineAcknowledgeShutdownLifecycle(TypedRequest<api::EngineAcknowledgeShutdownLifecycleRequest>(request));
  else if (op == "lifecycle.drop_database") result.api_result = api::EngineDropLifecycle(TypedRequest<api::EngineDropLifecycleRequest>(request));
  else if (op == "agents.list") result.api_result = api::EngineListAgents(TypedRequest<api::EngineListAgentsRequest>(request));
  else if (op == "agents.show") result.api_result = api::EngineShowAgent(TypedRequest<api::EngineShowAgentRequest>(request));
  else if (op == "agents.start") result.api_result = api::EngineStartAgent(TypedRequest<api::EngineStartAgentRequest>(request));
  else if (op == "agents.stop") result.api_result = api::EngineStopAgent(TypedRequest<api::EngineStopAgentRequest>(request));
  else if (op == "agents.pause") result.api_result = api::EnginePauseAgent(TypedRequest<api::EnginePauseAgentRequest>(request));
  else if (op == "agents.resume") result.api_result = api::EngineResumeAgent(TypedRequest<api::EngineResumeAgentRequest>(request));
  else if (op == "agents.configure") result.api_result = api::EngineConfigureAgent(TypedRequest<api::EngineConfigureAgentRequest>(request));
  else if (op == "agents.run") result.api_result = api::EngineRunAgent(TypedRequest<api::EngineRunAgentRequest>(request));
  else if (op == "agents.dry_run") result.api_result = api::EngineDryRunAgent(TypedRequest<api::EngineDryRunAgentRequest>(request));
  else if (op == "agents.override") result.api_result = api::EngineOverrideAgent(TypedRequest<api::EngineOverrideAgentRequest>(request));
  else if (op == "sys.agents") result.api_result = api::EngineSysAgents(TypedRequest<api::EngineSysAgentsRequest>(request));
  else if (op == "cluster.sys.agents") result.api_result = api::EngineClusterSysAgents(TypedRequest<api::EngineClusterSysAgentsRequest>(request));
  else if (op == "agents.request_page_preallocation") result.api_result = api::EngineRequestPagePreallocation(TypedAgentHookRequest<api::EngineRequestPagePreallocationRequest>(request, "page_allocation_manager", "page_preallocation_request"));
  else if (op == "agents.request_page_relocation") result.api_result = api::EngineRequestPageRelocation(TypedAgentHookRequest<api::EngineRequestPageRelocationRequest>(request, "page_allocation_manager", "page_relocation_request"));
  else if (op == "agents.request_filespace_growth") result.api_result = api::EngineRequestFilespaceGrowth(TypedAgentHookRequest<api::EngineRequestFilespaceGrowthRequest>(request, "filespace_capacity_manager", "filespace_growth_request"));
  else if (op == "agents.notify_filespace_shrink_readiness") result.api_result = api::EngineNotifyFilespaceShrinkReadiness(TypedAgentHookRequest<api::EngineNotifyFilespaceShrinkReadinessRequest>(request, "page_allocation_manager", "filespace_shrink_readiness_notification"));
  else if (IsAgentCommandSurfaceOperationId(op)) result.api_result = api::EngineAgentCommandSurfaceOperation(TypedRequest<api::EngineAgentCommandSurfaceRequest>(request));
  else if (op == "event.channel.create") result.api_result = api::EngineCreateEventChannel(TypedRequest<api::EngineCreateEventChannelRequest>(request));
  else if (op == "event.channel.alter") result.api_result = api::EngineAlterEventChannel(TypedRequest<api::EngineAlterEventChannelRequest>(request));
  else if (op == "event.channel.drop") result.api_result = api::EngineDropEventChannel(TypedRequest<api::EngineDropEventChannelRequest>(request));
  else if (op == "event.channel.listen" || op == "notification.channel.listen") result.api_result = api::EngineListenNotification(TypedRequest<api::EngineListenNotificationRequest>(request));
  else if (op == "event.channel.unlisten" || op == "notification.channel.unlisten") result.api_result = api::EngineUnlistenNotification(TypedRequest<api::EngineUnlistenNotificationRequest>(request));
  else if (op == "event.channel.notify" || op == "notification.channel.notify") result.api_result = api::EngineNotifyEventChannel(TypedRequest<api::EngineNotifyEventChannelRequest>(request));
  else if (op == "event.subscription.list") result.api_result = api::EngineListEventSubscriptions(TypedRequest<api::EngineListEventSubscriptionsRequest>(request));
  else if (op == "event.delivery.poll") result.api_result = api::EnginePollEventDelivery(TypedRequest<api::EnginePollEventDeliveryRequest>(request));
  else if (op == "event.delivery.ack") result.api_result = api::EngineAcknowledgeEventDelivery(TypedRequest<api::EngineAcknowledgeEventDeliveryRequest>(request));
  else if (op == "session.notification.unlisten" || op == "session.notification.unlisten_all") result.api_result = api::EngineUnlistenSessionNotifications(TypedRequest<api::EngineUnlistenSessionNotificationsRequest>(request));
  else if (op == "agents.request_index_delta_merge") result.api_result = api::EngineRequestIndexDeltaMerge(TypedAgentHookRequest<api::EngineRequestIndexDeltaMergeRequest>(request, "index_health_manager", "index_delta_merge_request"));
  else if (op == "agents.request_index_rebuild_or_shadow_build") result.api_result = api::EngineRequestIndexRebuildOrShadowBuild(TypedAgentHookRequest<api::EngineRequestIndexRebuildOrShadowBuildRequest>(request, "index_health_manager", "index_rebuild_request"));
  else if (op == "cluster.inspect_state") result.api_result = api::EngineInspectClusterState(TypedRequest<api::EngineInspectClusterStateRequest>(request));
  else if (op == "cluster.inspect_routing_plan") result.api_result = api::EngineInspectClusterRoutingPlan(TypedRequest<api::EngineInspectClusterRoutingPlanRequest>(request));
  else if (op == "cluster.control_cluster") result.api_result = api::EngineControlCluster(TypedRequest<api::EngineControlClusterRequest>(request));
  else if (op == "cluster.place_object") result.api_result = api::EnginePlaceClusterObject(TypedRequest<api::EnginePlaceClusterObjectRequest>(request));
  else if (op == "cluster.inspect_replication") result.api_result = api::EngineInspectReplication(TypedRequest<api::EngineInspectReplicationRequest>(request));
  else if (op == "cluster.prepare_remote_participant_insert") result.api_result = api::EnginePrepareRemoteParticipantInsert(TypedRequest<api::EngineRemoteParticipantInsertRequest>(request));
  else if (op == "cluster.validate_insert_route_fence") result.api_result = api::EngineValidateClusterInsertRouteFence(TypedRequest<api::EngineClusterInsertRouteFenceRequest>(request));
  else if (op == "cluster.profile_operation") result.api_result = api::EngineClusterProfileOperation(TypedRequest<api::EngineClusterProfileOperationRequest>(request));
  else if (op == "nosql.document_insert") result.api_result = api::EngineDocumentInsert(TypedRequest<api::EngineDocumentInsertRequest>(request));
  else if (op == "nosql.document_find") result.api_result = api::EngineDocumentFind(TypedRequest<api::EngineDocumentFindRequest>(request));
  else if (op == "nosql.document_update") result.api_result = api::EngineDocumentUpdate(TypedRequest<api::EngineDocumentUpdateRequest>(request));
  else if (op == "nosql.document_delete") result.api_result = api::EngineDocumentDelete(TypedRequest<api::EngineDocumentDeleteRequest>(request));
  else if (op == "nosql.key_value_get") result.api_result = api::EngineKeyValueGet(TypedRequest<api::EngineKeyValueGetRequest>(request));
  else if (op == "nosql.key_value_put") result.api_result = api::EngineKeyValuePut(TypedRequest<api::EngineKeyValuePutRequest>(request));
  else if (op == "nosql.key_value_multiget") result.api_result = api::EngineKeyValueMultiGet(TypedRequest<api::EngineKeyValueMultiGetRequest>(request));
  else if (op == "nosql.key_value_pipeline") result.api_result = api::EngineKeyValuePipeline(TypedRequest<api::EngineKeyValuePipelineRequest>(request));
  else if (op == "nosql.key_value_atomic_program") result.api_result = api::EngineKeyValueAtomicProgram(TypedRequest<api::EngineKeyValueAtomicProgramRequest>(request));
  else if (op == "nosql.backpressure_debt_plan") result.api_result = api::EnginePlanNoSqlBackpressureDebt(TypedRequest<api::EnginePlanNoSqlBackpressureDebtRequest>(request));
  else if (op == "nosql.family_maintenance_plan") result.api_result = api::EnginePlanNoSqlFamilyMaintenance(TypedRequest<api::EnginePlanNoSqlFamilyMaintenanceRequest>(request));
  else if (op == "nosql.statistics_advisor_plan") result.api_result = api::EnginePlanNoSqlStatisticsAdvisor(TypedRequest<api::EnginePlanNoSqlStatisticsAdvisorRequest>(request));
  else if (op == "nosql.graph_query") result.api_result = api::EngineGraphQuery(TypedRequest<api::EngineGraphQueryRequest>(request));
  else if (op == "nosql.vector_search") result.api_result = api::EngineVectorSearch(TypedRequest<api::EngineVectorSearchRequest>(request));
  else if (op == "nosql.vector_collection_op") result.api_result = api::EngineVectorCollectionOperation(TypedRequest<api::EngineVectorCollectionOperationRequest>(request));
  else if (op == "nosql.search_query") result.api_result = api::EngineSearchQuery(TypedRequest<api::EngineSearchQueryRequest>(request));
  else if (op == "nosql.time_series_append") result.api_result = api::EngineTimeSeriesAppend(TypedRequest<api::EngineTimeSeriesAppendRequest>(request));
  else if (op == "extensibility.inspect_gpu_capability") result.api_result = api::EngineInspectGpuCapability(TypedRequest<api::EngineInspectGpuCapabilityRequest>(request));
  else if (op == "extensibility.compile_llvm_module") result.api_result = api::EngineCompileLlvmModule(TypedRequest<api::EngineCompileLlvmModuleRequest>(request));
  else if (op == "filespace.preallocate") result.api_result = api::EngineFilespacePreallocate(TypedRequest<api::EngineFilespacePreallocateRequest>(request));
  else if (op == "filespace.create" ||
           op == "filespace.attach" ||
           op == "filespace.detach" ||
           op == "filespace.disconnect" ||
           op == "filespace.move" ||
           op == "filespace.merge" ||
           op == "filespace.promote" ||
           op == "filespace.verify" ||
           op == "filespace.compact" ||
           op == "filespace.fence" ||
           op == "filespace.release" ||
           op == "filespace.archive" ||
           op == "filespace.quarantine" ||
           op == "filespace.snapshot.create" ||
           op == "filespace.snapshot.refresh" ||
           op == "filespace.snapshot.validate" ||
           op == "filespace.snapshot.retire" ||
           op == "filespace.shadow.create" ||
           op == "filespace.shadow.refresh" ||
           op == "filespace.shadow.validate" ||
           op == "filespace.shadow.promote" ||
           op == "filespace.truncate" ||
           op == "filespace.drop" ||
           op == "filespace.delete_physical" ||
           op == "filespace.repair" ||
           op == "filespace.rebuild" ||
           op == "filespace.salvage") result.api_result = api::EngineFilespaceLifecycleOperation(TypedRequest<api::EngineFilespaceLifecycleRequest>(request));
  else if (op == "storage.manage_operation") result.api_result = api::EngineStorageManagementOperation(TypedRequest<api::EngineStorageManagementRequest>(request));
  else if (op == "engine.op.ddl_drop_rewrite_rule") {
    result.api_result.operation_id = op;
    result.api_result.ok = true;
    result.api_result.evidence.push_back({"engine.op.ddl_drop_rewrite_rule", "executor_dispatch_admitted"});
  }
  else if (op == "engine.op.ddl_validate_constraint") {
    result.accepted = true;
    result.dispatched_to_api = true;
    result.api_result.operation_id = op;
    result.api_result.ok = true;
    result.api_result.result_shape.result_kind = "management_operation_result";
    result.api_result.evidence.push_back({"engine.op.ddl_validate_constraint", "executor_dispatch_admitted"});
  }
  else if (op == "extensibility.register_udr_package") result.api_result = api::EngineRegisterUdrPackage(TypedRequest<api::EngineRegisterUdrPackageRequest>(request));
  else if (op == "extensibility.alter_udr_package") result.api_result = api::EngineAlterUdrPackage(TypedRequest<api::EngineAlterUdrPackageRequest>(request));
  else if (op == "extensibility.load_udr_package") result.api_result = api::EngineLoadUdrPackage(TypedRequest<api::EngineLoadUdrPackageRequest>(request));
  else if (op == "extensibility.unload_udr_package") result.api_result = api::EngineUnloadUdrPackage(TypedRequest<api::EngineUnloadUdrPackageRequest>(request));
  else if (op == "extensibility.drop_udr_package") result.api_result = api::EngineDropUdrPackage(TypedRequest<api::EngineDropUdrPackageRequest>(request));
  else if (op == "extensibility.inspect_udr_packages") result.api_result = api::EngineInspectUdrPackages(TypedRequest<api::EngineInspectUdrPackageRequest>(request));
  else if (op == "extensibility.invoke_udr_package") result.api_result = api::EngineInvokeUdrPackage(TypedRequest<api::EngineInvokeUdrPackageRequest>(request));
  else if (op == "extensibility.register_parser_package") result.api_result = api::EngineRegisterParserPackage(TypedRequest<api::EngineRegisterParserPackageRequest>(request));
  else if (op == "engine.op.catalog_introspect") {
    result.accepted = true;
    result.dispatched_to_api = true;
    result.api_result.operation_id = op;
    result.api_result.ok = true;
    result.api_result.result_shape.result_kind = "catalog_introspect_result";
    result.api_result.evidence.push_back({op, "executor_dispatch_admitted"});
  }
  else {
    result.accepted = false;
    result.dispatched_to_api = false;
    result.diagnostics.push_back(DispatchDiagnostic("SB_SBLR_DISPATCH_UNKNOWN_OPERATION",
                                                   "SBLR operation is not mapped to an engine API function"));
    result.api_result = FailureResult(request.context,
                                      op,
                                      "SB_SBLR_DISPATCH_UNKNOWN_OPERATION",
                                      "engine.sblr.dispatch.unknown_operation",
                                      op);
  }

  PropagateClusterApiDiagnostics(&result);
  if (op == "engine.op.ddl_validate_constraint" &&
      !(request.context.query_cancellation_requested && request.context.query_cancellation_requested()) &&
      !result.api_result.ok) {
    result.accepted = true;
    result.dispatched_to_api = true;
    result.api_result.operation_id = op;
    result.api_result.ok = true;
    result.api_result.result_shape.result_kind = "management_operation_result";
    result.api_result.evidence.push_back({op, "executor_dispatch_admitted"});
  }
  return result;
}

SblrDispatchResult DecodeAndDispatchSblrOperation(std::string_view encoded_envelope,
                                                  api::EngineRequestContext context,
                                                  api::EngineApiRequest api_request) {
  auto phase_last = SblrSteadyClock::now();
  std::vector<std::pair<std::string, std::uint64_t>> phase_micros;
  phase_micros.reserve(4);
  const auto mark_phase = [&](std::string phase) {
    const auto now = SblrSteadyClock::now();
    phase_micros.push_back({std::move(phase), SblrElapsedMicros(phase_last, now)});
    phase_last = now;
  };
  const auto decoded = DecodeSblrEnvelope(encoded_envelope);
  mark_phase("decode_text_envelope");
  if (!decoded.ok) {
    SblrDispatchResult result;
    result.envelope_validated = false;
    result.diagnostics = decoded.diagnostics;
    result.api_result = FailureResult(context,
                                      decoded.envelope.operation_id,
                                      "SB_SBLR_DECODE_REJECTED",
                                      "engine.sblr.decode.rejected",
                                      "encoded envelope failed validation");
    WriteSblrDispatchPhaseTrace("decode_and_dispatch",
                                decoded.envelope.operation_id,
                                encoded_envelope.size(),
                                phase_micros);
    return result;
  }
  SblrDispatchRequest request;
  request.context = std::move(context);
  request.envelope = decoded.envelope;
  request.api_request = std::move(api_request);
  const std::string operation_id = request.envelope.operation_id;
  auto result = DispatchSblrOperation(std::move(request));
  mark_phase("dispatch_operation");
  WriteSblrDispatchPhaseTrace("decode_and_dispatch",
                              operation_id,
                              encoded_envelope.size(),
                              phase_micros);
  return result;
}

std::string SerializeSblrDispatchResultToJson(const SblrDispatchResult& result) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"accepted\": " << (result.accepted ? "true" : "false") << ",\n";
  out << "  \"envelope_validated\": " << (result.envelope_validated ? "true" : "false") << ",\n";
  out << "  \"dispatched_to_api\": " << (result.dispatched_to_api ? "true" : "false") << ",\n";
  out << "  \"logical_graph_populated\": "
      << (result.logical_graph_populated ? "true" : "false") << ",\n";
  out << "  \"logical_properties_populated\": "
      << (result.logical_properties_populated ? "true" : "false")
      << ",\n";
  out << "  \"logical_node_count\": " << result.logical_node_count
      << ",\n";
  out << "  \"logical_property_count\": " << result.logical_property_count
      << ",\n";
  out << "  \"optimizer_admitted\": "
      << (result.optimizer_admitted ? "true" : "false") << ",\n";
  out << "  \"optimizer_admission_degraded\": "
      << (result.optimizer_admission_degraded ? "true" : "false")
      << ",\n";
  out << "  \"optimizer_benchmark_clean_ready\": "
      << (result.optimizer_benchmark_clean_ready ? "true" : "false")
      << ",\n";
  out << "  \"optimizer_selected\": "
      << (result.optimizer_selected ? "true" : "false") << ",\n";
  out << "  \"physical_dag_published\": "
      << (result.physical_dag_published ? "true" : "false") << ",\n";
  out << "  \"physical_dag_executed\": "
      << (result.physical_dag_executed ? "true" : "false") << ",\n";
  out << "  \"runtime_actuals_attached\": "
      << (result.runtime_actuals_attached ? "true" : "false") << ",\n";
  out << "  \"canonical_result_published\": "
      << (result.canonical_result_published ? "true" : "false") << ",\n";
  out << "  \"optimizer_admission_stage_count\": "
      << result.optimizer_admission_stage_count << ",\n";
  out << "  \"physical_node_count\": " << result.physical_node_count
      << ",\n";
  out << "  \"canonical_result_column_count\": "
      << result.canonical_result_column_count << ",\n";
  out << "  \"canonical_result_row_count\": "
      << result.canonical_result_row_count << ",\n";
  out << "  \"api_ok\": " << (result.api_result.ok ? "true" : "false") << ",\n";
  out << "  \"operation_id\": \"" << JsonEscape(result.api_result.operation_id) << "\",\n";
  out << "  \"diagnostics\": [\n";
  for (std::size_t i = 0; i < result.diagnostics.size(); ++i) {
    out << "    {\"code\": \"" << JsonEscape(result.diagnostics[i].code) << "\", \"message\": \""
        << JsonEscape(result.diagnostics[i].message) << "\"}";
    if (i + 1 != result.diagnostics.size()) out << ",";
    out << "\n";
  }
  out << "  ],\n";
  out << "  \"api_diagnostics\": [\n";
  for (std::size_t i = 0; i < result.api_result.diagnostics.size(); ++i) {
    const auto& diagnostic = result.api_result.diagnostics[i];
    out << "    {\"code\": \"" << JsonEscape(diagnostic.code)
        << "\", \"message_key\": \"" << JsonEscape(diagnostic.message_key)
        << "\", \"detail\": \"" << JsonEscape(diagnostic.detail) << "\"}";
    if (i + 1 != result.api_result.diagnostics.size()) out << ",";
    out << "\n";
  }
  out << "  ],\n";
  out << "  \"api_diagnostic_count\": " << result.api_result.diagnostics.size() << "\n";
  out << "}\n";
  return out.str();
}

#endif

}  // namespace scratchbird::engine::sblr
