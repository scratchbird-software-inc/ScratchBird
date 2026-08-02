// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "query/canonical_heap_optimizer_admission.hpp"

#include "mga_relation_store/mga_relation_store.hpp"
#include "query/canonical_relational_bridge.hpp"
#include "security/security_model.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <optional>
#include <ranges>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

namespace opt = scratchbird::engine::optimizer;

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

bool ParsePositiveU64(const std::string_view text, std::uint64_t* value) {
  if (value == nullptr || text.empty()) return false;
  std::uint64_t parsed = 0;
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), parsed);
  if (error != std::errc{} || end != text.data() + text.size() ||
      parsed == 0) {
    return false;
  }
  *value = parsed;
  return true;
}

std::optional<std::string> ExactDescriptorField(
    const std::string_view descriptor,
    const std::string_view field_name) {
  const std::string prefix = std::string(field_name) + "=";
  std::optional<std::string> value;
  std::size_t offset = 0;
  while (offset <= descriptor.size()) {
    const auto next = descriptor.find(';', offset);
    const auto end = next == std::string_view::npos ? descriptor.size() : next;
    const auto field = descriptor.substr(offset, end - offset);
    if (field.rfind(prefix, 0) == 0) {
      if (value.has_value() || field.size() == prefix.size()) {
        return std::nullopt;
      }
      value = std::string(field.substr(prefix.size()));
    }
    if (next == std::string_view::npos) break;
    offset = next + 1;
  }
  return value;
}

bool ExactOptionalDescriptorFieldMatches(
    const std::string_view descriptor,
    const std::string_view field_name,
    const std::optional<std::string>& expected) {
  const std::string prefix = std::string(field_name) + "=";
  std::size_t matches = 0;
  std::string_view actual;
  std::size_t offset = 0;
  while (offset <= descriptor.size()) {
    const auto next = descriptor.find(';', offset);
    const auto end = next == std::string_view::npos ? descriptor.size() : next;
    const auto field = descriptor.substr(offset, end - offset);
    if (field.rfind(prefix, 0) == 0) {
      ++matches;
      actual = field.substr(prefix.size());
    }
    if (next == std::string_view::npos) break;
    offset = next + 1;
  }
  if (!expected.has_value()) return matches == 0;
  return matches == 1 && !actual.empty() && actual == *expected;
}

bool ExactNullabilityCarrierMatches(const std::string_view descriptor,
                                    const bool expected_nullable) {
  std::optional<bool> admitted;
  std::size_t canonical_count = 0;
  std::size_t storage_count = 0;
  std::size_t offset = 0;
  while (offset <= descriptor.size()) {
    const auto next = descriptor.find(';', offset);
    const auto end = next == std::string_view::npos ? descriptor.size() : next;
    const auto field = descriptor.substr(offset, end - offset);
    std::optional<bool> current;
    if (field.rfind("nullability=", 0) == 0) {
      ++canonical_count;
      const auto value = field.substr(std::string_view("nullability=").size());
      if (value == "nullable") {
        current = true;
      } else if (value == "non_null") {
        current = false;
      } else {
        return false;
      }
    } else if (field.rfind("nullable=", 0) == 0) {
      ++storage_count;
      const auto value = field.substr(std::string_view("nullable=").size());
      if (value == "true") {
        current = true;
      } else if (value == "false") {
        current = false;
      } else {
        return false;
      }
    }
    if (current.has_value()) {
      if (admitted.has_value() && *admitted != *current) return false;
      admitted = current;
    }
    if (next == std::string_view::npos) break;
    offset = next + 1;
  }
  return canonical_count <= 1 && storage_count <= 1 && admitted.has_value() &&
         *admitted == expected_nullable;
}

CanonicalHeapOptimizerAdmissionResult Refuse(std::string diagnostic_id,
                                             std::string field_id) {
  CanonicalHeapOptimizerAdmissionResult result;
  result.issue.diagnostic_id = std::move(diagnostic_id);
  result.issue.field_id = std::move(field_id);
  return result;
}

}  // namespace

CanonicalHeapOptimizerAdmissionResult
BuildCanonicalCurrentHeapOptimizerAdmission(
    const CanonicalHeapOptimizerAdmissionRequest& request) {
  const auto& context = request.context;
  const auto& relational = request.relational_dag;

  std::uint64_t admitted_at_monotonic_ns = 0;
  if (context.database_path.empty() ||
      !IsCanonicalUuid(context.database_uuid.canonical) ||
      !IsCanonicalUuid(context.statement_uuid.canonical) ||
      !IsCanonicalUuid(context.transaction_uuid.canonical) ||
      !IsCanonicalUuid(context.statement_snapshot_uuid.canonical) ||
      !IsCanonicalUuid(context.catalog_epoch_uuid.canonical) ||
      context.local_transaction_id == 0 ||
      !context.statement_metadata_snapshot_engine_owned ||
      !IsCanonicalUuid(context.statement_metadata_snapshot_uuid.canonical) ||
      !context.security_context_present ||
      !context.authorization_context.present ||
      !IsCanonicalUuid(context.authorization_context.authority_uuid.canonical) ||
      context.authorization_context.principal_uuid.canonical !=
          context.principal_uuid.canonical ||
      context.catalog_generation_id == 0 || context.security_epoch == 0 ||
      context.resource_epoch == 0 ||
      context.authorization_context.catalog_generation_id !=
          context.catalog_generation_id ||
      context.authorization_context.security_epoch != context.security_epoch ||
      context.authorization_context.policy_epoch == 0 ||
      !IsCanonicalUuid(context.optimizer_capability_snapshot_uuid.canonical) ||
      !IsCanonicalUuid(context.optimizer_resource_snapshot_uuid.canonical) ||
      !IsCanonicalUuid(context.optimizer_route_snapshot_uuid.canonical) ||
      context.optimizer_route_epoch == 0 ||
      context.optimizer_route_generation == 0 ||
      context.optimizer_memory_budget_bytes == 0 ||
      context.optimizer_maximum_candidate_count == 0 ||
      context.optimizer_maximum_memo_groups == 0 ||
      context.optimizer_maximum_search_steps == 0 ||
      context.optimizer_maximum_planning_time_ns == 0 ||
      !ParsePositiveU64(context.current_monotonic_ns,
                        &admitted_at_monotonic_ns)) {
    return Refuse("QOW-DIAG-QRY-004-HEAP-OPTIMIZER-CONTEXT-V1",
                  "engine_planning_context");
  }
  if (!context.prepared_metadata_required_object_uuid.canonical.empty() ||
      context.prepared_metadata_required_executable_generation != 0 ||
      context.prepared_metadata_required_metadata_epoch != 0) {
    return Refuse("QOW-DIAG-QRY-004-HEAP-OPTIMIZER-SCOPE-V1",
                  "prepared_or_cached_execution");
  }

  const auto validation = ValidateTypedRelationalDag(relational);
  if (!validation.accepted) {
    const auto& issue = validation.issues.front();
    return Refuse(issue.diagnostic_id,
                  issue.field_id + ":node_id=" +
                      std::to_string(issue.node_id));
  }
  if (relational.wire_version != 2 || relational.nodes.size() != 1 ||
      relational.root_node_id != relational.nodes.front().node_id) {
    return Refuse("QOW-DIAG-QRY-004-HEAP-OPTIMIZER-PROFILE-V1",
                  "one_leaf_root");
  }
  const auto& node = relational.nodes.front();
  if (node.node_kind != RelationalDagNodeKind::kScan ||
      node.semantic_variant_id != "relation.source.v1" ||
      !node.input_node_ids.empty() || node.shareable ||
      node.required_object_uuids.size() != 1 ||
      !IsCanonicalUuid(node.required_object_uuids.front()) ||
      node.output_descriptor_ids.empty() ||
      node.output_descriptor_ids.size() != node.bound_expression_ids.size() ||
      !node.values_row_ids.empty() || !node.required_property_uuids.empty() ||
      !node.delivered_property_uuids.empty() ||
      !relational.values_rows.empty() || !relational.grouping_sets.empty() ||
      !relational.properties.empty()) {
    return Refuse("QOW-DIAG-QRY-004-HEAP-OPTIMIZER-PROFILE-V1",
                  "relation_source_leaf");
  }
  const std::size_t width = node.output_descriptor_ids.size();
  if (relational.outputs.size() != width ||
      relational.expressions.size() != width ||
      relational.descriptors.size() != width) {
    return Refuse("QOW-DIAG-QRY-004-HEAP-OPTIMIZER-BINDING-V1",
                  "complete_visible_width");
  }

  CanonicalRelationalPlanningScope planning_scope;
  planning_scope.catalog_epoch_uuid =
      context.catalog_epoch_uuid.canonical;
  planning_scope.security_context_uuid =
      context.authorization_context.authority_uuid.canonical;
  planning_scope.statement_uuid = context.statement_uuid.canonical;
  planning_scope.owning_transaction_uuid = context.transaction_uuid.canonical;
  planning_scope.statement_snapshot_uuid =
      context.statement_snapshot_uuid.canonical;
  planning_scope.statement_metadata_snapshot_uuid =
      context.statement_metadata_snapshot_uuid.canonical;
  planning_scope.local_transaction_id = context.local_transaction_id;
  planning_scope.snapshot_visible_through_local_transaction_id =
      context.snapshot_visible_through_local_transaction_id;
  planning_scope.metadata_snapshot_engine_owned =
      context.statement_metadata_snapshot_engine_owned;
  planning_scope.authorization_context_engine_owned =
      context.authorization_context.present;
  const auto logical =
      PopulateCanonicalLogicalGraphFromAdmittedTypedRelationalDag(
          relational, planning_scope);
  if (!logical.accepted || !logical.property_catalog.properties.empty()) {
    if (!logical.issues.empty()) {
      return Refuse(logical.issues.front().diagnostic_id,
                    logical.issues.front().field_id);
    }
    return Refuse("QOW-DIAG-QRY-004-HEAP-OPTIMIZER-PROFILE-V1",
                  "object_source_properties");
  }

  const std::string& relation_uuid = node.required_object_uuids.front();
  const auto temporary =
      CheckMgaTemporaryTableVisibility(context, relation_uuid);
  if (!temporary.ok || !temporary.table_visible ||
      temporary.known_temporary || temporary.table.temporary) {
    return Refuse("QOW-DIAG-QRY-004-HEAP-OPTIMIZER-RELATION-V1",
                  temporary.ok ? "current_non_temporary_relation"
                               : temporary.diagnostic.detail);
  }
  const auto loaded = LoadMgaRelationStorageDescriptor(context, relation_uuid);
  if (!loaded.ok) {
    return Refuse("QOW-DIAG-QRY-004-HEAP-OPTIMIZER-DESCRIPTOR-V1",
                  loaded.diagnostic.detail);
  }
  const auto& persisted = loaded.descriptor;
  if (persisted.relation_uuid.canonical != relation_uuid ||
      persisted.database_uuid.canonical != context.database_uuid.canonical ||
      persisted.relation_kind != "table" ||
      persisted.storage_profile != "local_mga_rowstore_v1" ||
      !IsCanonicalUuid(persisted.descriptor_uuid.canonical) ||
      persisted.descriptor_generation == 0 ||
      (persisted.descriptor_status != "production_descriptor" &&
       persisted.descriptor_status != "metadata_bridge_vetted_descriptor") ||
      persisted.columns.size() != width) {
    return Refuse("QOW-DIAG-QRY-004-HEAP-OPTIMIZER-DESCRIPTOR-V1",
                  "current_persisted_local_heap_descriptor");
  }

  std::unordered_set<std::uint32_t> output_ids;
  std::unordered_set<std::uint32_t> expression_ids;
  std::unordered_set<std::uint32_t> descriptor_ids;
  std::unordered_set<std::string> column_uuids;
  std::unordered_set<std::string> column_names;
  for (std::size_t ordinal = 0; ordinal < width; ++ordinal) {
    const auto& output = relational.outputs[ordinal];
    const auto& expression = relational.expressions[ordinal];
    const auto& descriptor = relational.descriptors[ordinal];
    const auto& column = persisted.columns[ordinal];
    const auto persisted_type_uuid = ExactDescriptorField(
        column.value_descriptor.encoded_descriptor, "type_uuid");
    const bool nullable =
        descriptor.nullability == RelationalNullability::kNullable;
    if (output.relation_node_id != node.node_id || !output.visible ||
        output.ordinal != ordinal || output.output_id == 0 ||
        !output_ids.insert(output.output_id).second ||
        output.descriptor_id != node.output_descriptor_ids[ordinal] ||
        expression.expression_id != output.expression_id ||
        !expression_ids.insert(expression.expression_id).second ||
        node.bound_expression_ids[ordinal] != expression.expression_id ||
        expression.expression_kind != RelationalExpressionKind::kIdentifier ||
        !expression.child_expression_ids.empty() ||
        expression.result_descriptor_id != output.descriptor_id ||
        !expression.bound_name_uuid.has_value() ||
        !IsCanonicalUuid(*expression.bound_name_uuid) ||
        expression.function_uuid.has_value() || expression.literal_kind.has_value() ||
        expression.operator_name.has_value() ||
        expression.literal_or_parameter_ref.has_value() ||
        descriptor.descriptor_id != output.descriptor_id ||
        !descriptor_ids.insert(descriptor.descriptor_id).second ||
        !IsCanonicalUuid(descriptor.descriptor_uuid) ||
        !IsCanonicalUuid(descriptor.type_uuid) ||
        descriptor.nullability == RelationalNullability::kUnknown ||
        (descriptor.collation_uuid.has_value() &&
         !IsCanonicalUuid(*descriptor.collation_uuid)) ||
        (descriptor.timezone_profile_id.has_value() &&
         descriptor.timezone_profile_id->empty()) ||
        column.ordinal != ordinal ||
        !IsCanonicalUuid(column.column_uuid.canonical) ||
        !column_uuids.insert(column.column_uuid.canonical).second ||
        column.column_uuid.canonical != *expression.bound_name_uuid ||
        column.canonical_name_key.empty() ||
        !column_names.insert(column.canonical_name_key).second ||
        output.output_name_utf8 != column.canonical_name_key ||
        column.value_descriptor.descriptor_uuid.canonical !=
            descriptor.descriptor_uuid ||
        column.value_descriptor.encoded_descriptor.empty() ||
        column.value_descriptor.canonical_type_name.empty() ||
        !persisted_type_uuid.has_value() ||
        !IsCanonicalUuid(*persisted_type_uuid) ||
        *persisted_type_uuid != descriptor.type_uuid ||
        column.nullable != nullable ||
        !ExactNullabilityCarrierMatches(
            column.value_descriptor.encoded_descriptor, nullable) ||
        (descriptor.collation_uuid.has_value()
             ? column.collation_uuid != *descriptor.collation_uuid
             : !column.collation_uuid.empty()) ||
        !ExactOptionalDescriptorFieldMatches(
            column.value_descriptor.encoded_descriptor, "collation_uuid",
            descriptor.collation_uuid) ||
        !ExactOptionalDescriptorFieldMatches(
            column.value_descriptor.encoded_descriptor,
            "timezone_profile_id", descriptor.timezone_profile_id)) {
      return Refuse("QOW-DIAG-QRY-004-HEAP-OPTIMIZER-BINDING-V1",
                    "persisted_ordinal_descriptor_binding");
    }
  }

  const auto authorization = EvaluateMaterializedAuthorization(
      context, context.authorization_context, "SELECT", relation_uuid);
  if (!authorization.authorized || authorization.denied ||
      authorization.policy_recheck_required ||
      !authorization.diagnostics.empty()) {
    return Refuse("QOW-DIAG-QRY-004-HEAP-OPTIMIZER-SECURITY-V1",
                  authorization.diagnostics.empty()
                      ? "select_authorization"
                      : authorization.diagnostics.front().detail);
  }

  opt::CanonicalNativeObjectAdmissionContext admission_context;
  admission_context.statement_uuid = context.statement_uuid.canonical;
  admission_context.catalog_snapshot_uuid =
      context.statement_metadata_snapshot_uuid.canonical;
  admission_context.security_context_uuid =
      context.authorization_context.authority_uuid.canonical;
  admission_context.catalog_generation = context.catalog_generation_id;
  admission_context.authorization_catalog_generation =
      context.authorization_context.catalog_generation_id;
  admission_context.security_epoch =
      context.authorization_context.security_epoch;
  admission_context.policy_epoch =
      context.authorization_context.policy_epoch;
  admission_context.resource_epoch = context.resource_epoch;
  admission_context.capability_snapshot_uuid =
      context.optimizer_capability_snapshot_uuid.canonical;
  admission_context.resource_snapshot_uuid =
      context.optimizer_resource_snapshot_uuid.canonical;
  admission_context.route_snapshot_uuid =
      context.optimizer_route_snapshot_uuid.canonical;
  admission_context.route_epoch = context.optimizer_route_epoch;
  admission_context.route_generation = context.optimizer_route_generation;
  admission_context.memory_budget_bytes =
      context.optimizer_memory_budget_bytes;
  admission_context.maximum_candidate_count =
      context.optimizer_maximum_candidate_count;
  admission_context.maximum_memo_groups =
      context.optimizer_maximum_memo_groups;
  admission_context.maximum_search_steps =
      context.optimizer_maximum_search_steps;
  admission_context.maximum_planning_time_ns =
      context.optimizer_maximum_planning_time_ns;
  admission_context.spill_allowed = context.optimizer_spill_allowed;
  admission_context.local_transaction_id = context.local_transaction_id;
  admission_context.statement_snapshot_id =
      context.snapshot_visible_through_local_transaction_id;
  admission_context.admitted_at_monotonic_ns = admitted_at_monotonic_ns;
  admission_context.metadata_snapshot_engine_owned = true;
  admission_context.authorization_context_engine_owned = true;
  admission_context.catalog_object_uuids = {relation_uuid};
  admission_context.authorized_object_uuids = {relation_uuid};
  admission_context.catalog_object_evidence_engine_owned = true;
  admission_context.authorization_object_evidence_engine_owned = true;

  auto built = opt::BuildCanonicalObjectAwareNativeOptimizerAdmissionRequest(
      logical.logical_graph, logical.property_catalog, admission_context);
  if (!built.built || !built.admission.admitted ||
      !built.admission.planning_allowed ||
      !built.admission.degraded_for_unknown_statistics ||
      built.admission.benchmark_clean_ready ||
      built.admission.data_access_allowed ||
      built.admission.evidence.size() != 8 ||
      built.request.statistics.node_estimates.size() != 1) {
    return Refuse(
        built.diagnostic_id.empty()
            ? "QOW-DIAG-QRY-004-HEAP-OPTIMIZER-ADMISSION-V1"
            : built.diagnostic_id,
        built.field_id.empty() ? "one_source_unknown_statistics_admission"
                               : built.field_id);
  }

  CanonicalHeapOptimizerAdmissionResult result;
  result.built = true;
  result.request = std::move(built.request);
  result.admission = std::move(built.admission);
  result.current_relation_descriptor_uuid =
      persisted.descriptor_uuid.canonical;
  result.current_relation_descriptor_generation =
      persisted.descriptor_generation;
  return result;
}

}  // namespace scratchbird::engine::internal_api
