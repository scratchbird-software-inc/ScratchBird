// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

//
// SBSQL bounded source-layout anchor. Runtime behavior for this family is
// implemented by the active dispatcher, executor, planner, or function modules
// linked beside this translation unit and covered by the corresponding proof
// gates. Keep family-specific growth in this bounded area or in the named
// shared runtime module, not in broad catch-all files.

#include "descriptor_value_runtime.hpp"

#include "datatype_catalog_manifest.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <limits>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace scratchbird::engine::executor {
namespace {

bool IsCanonicalUuid(const std::string_view value) {
  if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
      value[18] != '-' || value[23] != '-' ||
      value == "00000000-0000-0000-0000-000000000000") {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) continue;
    const auto ch = static_cast<unsigned char>(value[index]);
    if (!std::isxdigit(ch) || std::isupper(ch)) return false;
  }
  return true;
}

DescriptorRuntimeDiagnostic TableSubqueryRefusal(std::string detail) {
  DescriptorRuntimeDiagnostic diagnostic;
  diagnostic.ok = false;
  diagnostic.diagnostic_code =
      "QOW-DIAG-QRY-013-TABLE-REFUSAL-V1";
  diagnostic.detail = std::move(detail);
  return diagnostic;
}

bool DescriptorBatchCarrierIsExactDefault(const DescriptorBatch& batch) {
  const DescriptorBatch empty;
  return batch.columns.empty() &&
         batch.columns.capacity() == empty.columns.capacity() &&
         batch.rows.empty() && batch.rows.capacity() == empty.rows.capacity();
}

std::string CanonicalCoreDatatypeUuid(const std::string_view stable_name) {
  static const auto manifest =
      scratchbird::core::datatypes::LoadCurrentCoreDatatypeCatalogManifest();
  if (!manifest.ok()) return {};
  const auto count = std::ranges::count_if(
      manifest.manifest.descriptor_rows,
      [&](const auto& row) { return row.stable_name == stable_name; });
  const auto found = std::ranges::find_if(
      manifest.manifest.descriptor_rows,
      [&](const auto& row) { return row.stable_name == stable_name; });
  return count == 1 && found != manifest.manifest.descriptor_rows.end() &&
                 found->descriptor_uuid.valid()
             ? scratchbird::core::uuid::UuidToString(
                   found->descriptor_uuid.value)
             : std::string{};
}

std::optional<std::string_view> CanonicalDescriptorField(
    const scratchbird::engine::internal_api::EngineDescriptor& descriptor,
    const std::string_view key) {
  const auto prefix = std::string(key) + "=";
  std::optional<std::string_view> result;
  std::size_t begin = 0;
  while (begin <= descriptor.encoded_descriptor.size()) {
    const auto end = descriptor.encoded_descriptor.find(';', begin);
    const auto field = std::string_view(descriptor.encoded_descriptor).substr(
        begin, end == std::string::npos ? std::string::npos : end - begin);
    if (field.starts_with(prefix)) {
      if (result.has_value()) return std::nullopt;
      result = field.substr(prefix.size());
    }
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  return result;
}

bool CanonicalSubqueryBatchMemoryBytes(const DescriptorBatch& batch,
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

struct CorrelatedCancellationPoll {
  DescriptorRuntimeDiagnostic diagnostic;
  bool cancelled = false;
};

CorrelatedCancellationPoll PollCorrelatedCancellation(
    const std::function<bool()>& probe,
    const std::string_view phase) {
  CorrelatedCancellationPoll result;
  if (!probe) return result;
  try {
    if (!probe()) return result;
    result.cancelled = true;
    result.diagnostic = TableSubqueryRefusal(
        "correlated/LATERAL execution cancelled " + std::string(phase));
    result.diagnostic.diagnostic_code =
        "QOW-DIAG-QRY-013-CANCELLATION-REFUSAL-V1";
  } catch (const std::exception& error) {
    result.diagnostic = TableSubqueryRefusal(
        "correlated/LATERAL cancellation probe failed " +
        std::string(phase) + ":" + error.what());
    result.diagnostic.diagnostic_code =
        "QOW-DIAG-QRY-013-CANCELLATION-PROBE-V1";
  } catch (...) {
    result.diagnostic = TableSubqueryRefusal(
        "correlated/LATERAL cancellation probe failed " +
        std::string(phase));
    result.diagnostic.diagnostic_code =
        "QOW-DIAG-QRY-013-CANCELLATION-PROBE-V1";
  }
  return result;
}

bool CorrelatedCancellationEvidenceBound(
    const TypedPhysicalNodeDag& dag,
    const std::function<bool()>& probe,
    const std::string& evidence_uuid) {
  if (!probe) return evidence_uuid.empty();
  const PhysicalAdmissionEvidence* policy = nullptr;
  for (const auto& evidence : dag.admission_evidence) {
    if (evidence.stage != PhysicalAdmissionStage::kPolicyCapability) continue;
    if (policy != nullptr) return false;
    policy = &evidence;
  }
  return policy != nullptr && !evidence_uuid.empty() &&
         policy->evidence_uuid == evidence_uuid;
}

struct CorrelatedBatchValidation {
  DescriptorRuntimeDiagnostic diagnostic;
  std::optional<CorrelatedCancellationPoll> cancellation;
};

CorrelatedBatchValidation ValidateCorrelatedBatch(
    const DescriptorBatch& batch,
    const std::vector<std::uint32_t>& descriptor_ids,
    const std::function<bool()>& cancellation_requested,
    const std::string_view phase) {
  CorrelatedBatchValidation result;
  bool validation_cancelled = false;
  result.diagnostic = ValidateCanonicalDescriptorBatch(
      batch, descriptor_ids,
      [&]() {
        auto poll = PollCorrelatedCancellation(
            cancellation_requested, phase);
        if (poll.diagnostic.ok) return false;
        result.cancellation = std::move(poll);
        return true;
      },
      &validation_cancelled);
  if (!validation_cancelled) result.cancellation.reset();
  return result;
}

}  // namespace

// QOW-SOURCE-QRY-013-TABLE-V1
// Materialize one table-subquery result through the selected canonical
// relational node. The engine-owned physical DAG supplies MGA statement
// context and immutable descriptor handles; parser or donor syntax is never
// consulted here. Validation and the resource bound complete before any
// result batch is published.
namespace {
enum class TableSubqueryExecutionRoute : std::uint8_t {
  table = 1,
  scalar,
  row,
  exists,
  quantified,
};

bool TableSubqueryImplementationMatches(
    const TableSubqueryExecutionRoute route,
    const std::string_view implementation_id) {
  switch (route) {
    case TableSubqueryExecutionRoute::table:
      return implementation_id == "subquery.table.materialize.typed.v1";
    case TableSubqueryExecutionRoute::scalar:
      return implementation_id == "subquery.scalar.cardinality.typed.v1";
    case TableSubqueryExecutionRoute::row:
      return implementation_id == "subquery.row.cardinality.typed.v1";
    case TableSubqueryExecutionRoute::exists:
      return implementation_id == "subquery.exists.typed.v1";
    case TableSubqueryExecutionRoute::quantified:
      return implementation_id == "subquery.quantified.typed.v1" ||
             implementation_id ==
                 "subquery.quantified.int64.typed.v1";
  }
  return false;
}

const PhysicalNodeRecord* FindPhysicalNode(
    const TypedPhysicalNodeDag& execution_dag,
    const std::uint64_t physical_node_id) {
  const auto found = std::ranges::find_if(
      execution_dag.nodes, [&](const auto& node) {
        return node.physical_node_id == physical_node_id;
      });
  return found == execution_dag.nodes.end() ? nullptr : &*found;
}

CanonicalTableSubqueryResult ExecuteCanonicalTableSubqueryBound(
    const CanonicalTableSubqueryRequest& request,
    const TypedPhysicalNodeDag& execution_dag,
    const std::uint64_t scoped_root_physical_node_id,
    const TableSubqueryExecutionRoute execution_route,
    const bool borrowed_execution_dag,
    const DescriptorBatch& execution_input_batch,
    const bool borrowed_input_batch) {
  CanonicalTableSubqueryResult result;
  const auto refuse = [&](std::string detail) {
    result.diagnostic = TableSubqueryRefusal(std::move(detail));
    result.output_batch = {};
    result.materialized_row_count = 0;
    result.selected_plan_uuid.clear();
    result.executed_physical_node_id = 0;
    result.causal_counter_id = 0;
    return result;
  };

  if (borrowed_execution_dag &&
      !TypedPhysicalNodeDagCarrierIsExactDefault(request.physical_dag)) {
    return refuse(
        "table subquery request carries conflicting physical DAG authority");
  }
  if (borrowed_input_batch &&
      !DescriptorBatchCarrierIsExactDefault(request.input_batch)) {
    return refuse(
        "table subquery request carries conflicting input batch ownership");
  }

  const auto authority_validation = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, execution_dag);
  if (!authority_validation.ok)
    return refuse(authority_validation.diagnostic_code + ":" +
                  authority_validation.detail);
  if (request.selected_physical_node_id == 0 ||
      request.selected_physical_node_id !=
          scoped_root_physical_node_id) {
    return refuse("selected table-subquery node is not the physical root");
  }

  const PhysicalNodeRecord* selected_node = nullptr;
  for (const auto& node : execution_dag.nodes) {
    if (node.physical_node_id == request.selected_physical_node_id) {
      selected_node = &node;
      break;
    }
  }
  if (selected_node == nullptr ||
      selected_node->node_kind != PhysicalNodeKind::kSubquery ||
      !TableSubqueryImplementationMatches(
          execution_route, selected_node->implementation_id) ||
      selected_node->input_physical_node_ids.size() != 1) {
    return refuse(
        "table subquery requires its selected canonical implementation");
  }

  const auto input_id = selected_node->input_physical_node_ids.front();
  const PhysicalNodeRecord* input_node = nullptr;
  for (const auto& node : execution_dag.nodes) {
    if (node.physical_node_id == input_id) {
      input_node = &node;
      break;
    }
  }
  if (input_node == nullptr) {
    return refuse("table-subquery relational input is unresolved");
  }
  if (selected_node->output_descriptor_ids !=
      input_node->output_descriptor_ids) {
    return refuse("table-subquery output descriptor handles drifted");
  }

  auto input_validation = ValidateCanonicalDescriptorBatch(
      execution_input_batch, input_node->output_descriptor_ids);
  if (!input_validation.ok) {
    return refuse(input_validation.diagnostic_code + ":" +
                  input_validation.detail);
  }
  if (request.maximum_materialized_row_count == 0 ||
      execution_input_batch.rows.size() >
          request.maximum_materialized_row_count) {
    return refuse("table-subquery materialization row bound was exceeded");
  }

  if (execution_route == TableSubqueryExecutionRoute::table) {
    std::uint64_t input_memory_bytes = 0;
    if (!CanonicalSubqueryBatchMemoryBytes(execution_input_batch,
                                           &input_memory_bytes) ||
        selected_node->memory_bytes_required == 0 ||
        selected_node->memory_bytes_required >
            execution_dag.memory_budget_bytes ||
        selected_node->memory_bytes_required >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::size_t>::max())) {
      auto refusal = refuse(
          "table-subquery memory grant or runtime payload accounting is "
          "invalid");
      refusal.diagnostic.diagnostic_code = "SBLR.PLAN_TREE.RESOURCE_LIMIT";
      return refusal;
    }
    auto remaining_memory_bytes = selected_node->memory_bytes_required;
    const auto charge = [&](const std::uint64_t bytes) {
      if (bytes > remaining_memory_bytes) return false;
      remaining_memory_bytes -= bytes;
      return true;
    };
    if (!charge(input_memory_bytes) || !charge(input_memory_bytes)) {
      auto refusal = refuse(
          "table-subquery materialization exceeds the selected node memory "
          "grant");
      refusal.diagnostic.diagnostic_code = "SBLR.PLAN_TREE.RESOURCE_LIMIT";
      return refusal;
    }
  }

  DescriptorBatch materialized = execution_input_batch;
  auto output_validation = ValidateCanonicalDescriptorBatch(
      materialized, selected_node->output_descriptor_ids);
  if (!output_validation.ok) {
    return refuse(output_validation.diagnostic_code + ":" +
                  output_validation.detail);
  }
  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, execution_dag);
  if (!result_authority.ok)
    return refuse(result_authority.diagnostic_code + ":" +
                  result_authority.detail);

  result.diagnostic = {};
  result.output_batch = std::move(materialized);
  result.materialized_row_count = result.output_batch.rows.size();
  result.selected_plan_uuid = execution_dag.selected_plan_uuid;
  result.executed_physical_node_id = selected_node->physical_node_id;
  result.causal_counter_id = selected_node->causal_counter_id;
  result.mga_statement_context = request.mga_authority.statement_context;
  return result;
}
}  // namespace

CanonicalTableSubqueryResult ExecuteCanonicalTableSubquery(
    const CanonicalTableSubqueryRequest& request) {
  return ExecuteCanonicalTableSubqueryBound(
      request, request.physical_dag,
      request.physical_dag.root_physical_node_id,
      TableSubqueryExecutionRoute::table, false,
      request.input_batch, false);
}

CanonicalTableSubqueryResult ExecuteCanonicalTableSubquery(
    const CanonicalTableSubqueryRequest& request,
    const TypedPhysicalNodeDag& borrowed_execution_dag,
    const std::uint64_t scoped_root_physical_node_id) {
  return ExecuteCanonicalTableSubqueryBound(
      request, borrowed_execution_dag, scoped_root_physical_node_id,
      TableSubqueryExecutionRoute::table, true,
      request.input_batch, false);
}

CanonicalTableSubqueryResult ExecuteCanonicalTableSubquery(
    const CanonicalTableSubqueryRequest& request,
    const TypedPhysicalNodeDag& borrowed_execution_dag,
    const std::uint64_t scoped_root_physical_node_id,
    const DescriptorBatch& borrowed_input_batch) {
  return ExecuteCanonicalTableSubqueryBound(
      request, borrowed_execution_dag, scoped_root_physical_node_id,
      TableSubqueryExecutionRoute::table, true,
      borrowed_input_batch, true);
}

// QOW-SOURCE-QRY-013-SCALAR-V1
// Enforce the scalar-subquery zero/one/many-row contract over the canonical
// table-subquery result. Zero rows produce one typed SQL NULL, one row
// preserves its value, and more than one row fails before any scalar result is
// published.
namespace {
CanonicalScalarSubqueryResult ExecuteCanonicalScalarSubqueryBound(
    const CanonicalScalarSubqueryRequest& request,
    const TypedPhysicalNodeDag& execution_dag,
    const std::uint64_t scoped_root_physical_node_id,
    const DescriptorBatch& execution_input_batch,
    const bool borrowed_execution_carriers) {
  using scratchbird::engine::internal_api::EngineTypedValue;
  using scratchbird::engine::internal_api::EngineValueState;

  CanonicalScalarSubqueryResult result;
  const auto refuse = [&](std::string detail) {
    result.diagnostic.ok = false;
    result.diagnostic.diagnostic_code =
        "QOW-DIAG-QRY-013-SCALAR-REFUSAL-V1";
    result.diagnostic.detail = std::move(detail);
    result.output_batch = {};
    result.source_row_count = 0;
    result.selected_plan_uuid.clear();
    result.executed_physical_node_id = 0;
    result.causal_counter_id = 0;
    return result;
  };

  auto table = ExecuteCanonicalTableSubqueryBound(
      request.table_request, execution_dag, scoped_root_physical_node_id,
      TableSubqueryExecutionRoute::scalar, borrowed_execution_carriers,
      execution_input_batch, borrowed_execution_carriers);
  if (!table.diagnostic.ok) {
    return refuse(table.diagnostic.diagnostic_code + ":" +
                  table.diagnostic.detail);
  }
  if (!PhysicalMgaStatementContextEqual(
          table.mga_statement_context,
          request.table_request.mga_authority.statement_context)) {
    return refuse("scalar table subquery returned a different MGA statement context");
  }
  if (table.output_batch.columns.size() != 1) {
    return refuse("scalar subquery requires exactly one result column");
  }
  const auto* selected_node =
      FindPhysicalNode(execution_dag, scoped_root_physical_node_id);
  if (selected_node == nullptr ||
      !IsCanonicalUuid(selected_node->executor_capability_uuid)) {
    return refuse("scalar subquery capability identity is unresolved");
  }

  const auto& source_column = table.output_batch.columns.front();
  const auto& source_descriptor = source_column.descriptor;
  const auto& result_descriptor = request.result_column.descriptor;
  const auto source_type_uuid =
      CanonicalDescriptorField(source_descriptor, "type_uuid");
  const auto result_type_uuid =
      CanonicalDescriptorField(result_descriptor, "type_uuid");
  if (request.value_expression_descriptor_id == 0 ||
      request.value_expression_descriptor_id != source_column.descriptor_id ||
      request.result_column.descriptor_id != source_column.descriptor_id ||
      request.result_column.stable_name.empty() ||
      !request.result_column.nullable ||
      !source_type_uuid.has_value() || !result_type_uuid.has_value() ||
      !IsCanonicalUuid(*source_type_uuid) ||
      !IsCanonicalUuid(*result_type_uuid) ||
      *source_type_uuid != *result_type_uuid ||
      !IsCanonicalUuid(result_descriptor.descriptor_uuid.canonical) ||
      result_descriptor.descriptor_uuid.canonical ==
          source_descriptor.descriptor_uuid.canonical ||
      result_descriptor.descriptor_uuid.canonical == *source_type_uuid ||
      result_descriptor.descriptor_uuid.canonical ==
          selected_node->executor_capability_uuid ||
      !CanonicalDerivedDescriptorTypeMatches(
          source_descriptor, source_column.nullable, result_descriptor,
          true)) {
    return refuse("scalar result descriptor is not the bound source column");
  }

  DescriptorBatch output;
  output.columns = {request.result_column};
  auto schema_validation = ValidateCanonicalDescriptorBatch(
      output, {request.result_column.descriptor_id});
  if (!schema_validation.ok) {
    return refuse(schema_validation.diagnostic_code + ":" +
                  schema_validation.detail);
  }
  if (table.materialized_row_count > 1) {
    return refuse("scalar subquery produced more than one row");
  }

  EngineTypedValue scalar_value;
  if (table.materialized_row_count == 0) {
    scalar_value.descriptor = request.result_column.descriptor;
    scalar_value.is_null = true;
    scalar_value.state = EngineValueState::sql_null;
  } else {
    scalar_value = table.output_batch.rows.front().values.front();
    scalar_value.descriptor = request.result_column.descriptor;
  }
  output.rows = {{{std::move(scalar_value)}}};
  auto output_validation = ValidateCanonicalDescriptorBatch(
      output, {request.result_column.descriptor_id});
  if (!output_validation.ok) {
    return refuse(output_validation.diagnostic_code + ":" +
                  output_validation.detail);
  }
  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.table_request.mga_authority,
      execution_dag);
  if (!result_authority.ok)
    return refuse(result_authority.diagnostic_code + ":" +
                  result_authority.detail);

  result.diagnostic = {};
  result.output_batch = std::move(output);
  result.source_row_count = table.materialized_row_count;
  result.selected_plan_uuid = std::move(table.selected_plan_uuid);
  result.executed_physical_node_id = table.executed_physical_node_id;
  result.causal_counter_id = table.causal_counter_id;
  result.mga_statement_context =
      request.table_request.mga_authority.statement_context;
  return result;
}

// QOW-SOURCE-QRY-013-ROW-V1
// Enforce the row-subquery cardinality contract over the canonical table
// result. Zero rows produce one typed all-NULL row, one row preserves every
// field, and more than one row fails before any row value is published.
CanonicalRowSubqueryResult ExecuteCanonicalRowSubqueryBound(
    const CanonicalRowSubqueryRequest& request,
    const TypedPhysicalNodeDag& execution_dag,
    const std::uint64_t scoped_root_physical_node_id,
    const DescriptorBatch& execution_input_batch,
    const bool borrowed_execution_carriers) {
  using scratchbird::engine::internal_api::EngineTypedValue;
  using scratchbird::engine::internal_api::EngineValueState;

  CanonicalRowSubqueryResult result;
  const auto refuse = [&](std::string detail) {
    result.diagnostic.ok = false;
    result.diagnostic.diagnostic_code =
        "QOW-DIAG-QRY-013-ROW-REFUSAL-V1";
    result.diagnostic.detail = std::move(detail);
    result.output_batch = {};
    result.source_row_count = 0;
    result.selected_plan_uuid.clear();
    result.executed_physical_node_id = 0;
    result.causal_counter_id = 0;
    return result;
  };

  auto table = ExecuteCanonicalTableSubqueryBound(
      request.table_request, execution_dag, scoped_root_physical_node_id,
      TableSubqueryExecutionRoute::row, borrowed_execution_carriers,
      execution_input_batch, borrowed_execution_carriers);
  if (!table.diagnostic.ok) {
    return refuse(table.diagnostic.diagnostic_code + ":" +
                  table.diagnostic.detail);
  }
  if (!PhysicalMgaStatementContextEqual(
          table.mga_statement_context,
          request.table_request.mga_authority.statement_context)) {
    return refuse("row table subquery returned a different MGA statement context");
  }
  const auto width = table.output_batch.columns.size();
  if (width == 0 || request.row_expression_descriptor_ids.size() != width ||
      request.result_columns.size() != width) {
    return refuse("row subquery result width is not completely bound");
  }

  const auto* selected_node =
      FindPhysicalNode(execution_dag, scoped_root_physical_node_id);
  if (selected_node == nullptr ||
      !IsCanonicalUuid(selected_node->executor_capability_uuid)) {
    return refuse("row subquery capability identity is unresolved");
  }
  std::unordered_set<std::string_view> source_identity_domain;
  source_identity_domain.insert(selected_node->executor_capability_uuid);
  for (const auto& source : table.output_batch.columns) {
    const auto source_type_uuid =
        CanonicalDescriptorField(source.descriptor, "type_uuid");
    if (!IsCanonicalUuid(source.descriptor.descriptor_uuid.canonical) ||
        !source_type_uuid.has_value() ||
        !IsCanonicalUuid(*source_type_uuid)) {
      return refuse("row subquery source identity domain is unresolved");
    }
    source_identity_domain.insert(
        source.descriptor.descriptor_uuid.canonical);
    source_identity_domain.insert(*source_type_uuid);
  }
  std::unordered_set<std::string_view> result_descriptor_uuids;
  std::vector<std::uint32_t> result_descriptor_ids;
  result_descriptor_ids.reserve(width);
  for (std::size_t column = 0; column < width; ++column) {
    const auto& source = table.output_batch.columns[column];
    const auto& bound = request.result_columns[column];
    const auto& source_descriptor = source.descriptor;
    const auto& bound_descriptor = bound.descriptor;
    const auto source_type_uuid =
        CanonicalDescriptorField(source_descriptor, "type_uuid");
    const auto result_type_uuid =
        CanonicalDescriptorField(bound_descriptor, "type_uuid");
    if (request.row_expression_descriptor_ids[column] == 0 ||
        request.row_expression_descriptor_ids[column] !=
            source.descriptor_id ||
        bound.descriptor_id != source.descriptor_id ||
        bound.stable_name.empty() || !bound.nullable ||
        !source_type_uuid.has_value() || !result_type_uuid.has_value() ||
        !IsCanonicalUuid(*source_type_uuid) ||
        !IsCanonicalUuid(*result_type_uuid) ||
        *source_type_uuid != *result_type_uuid ||
        !IsCanonicalUuid(bound_descriptor.descriptor_uuid.canonical) ||
        source_identity_domain.contains(
            bound_descriptor.descriptor_uuid.canonical) ||
        !result_descriptor_uuids
             .insert(bound_descriptor.descriptor_uuid.canonical)
             .second ||
        !CanonicalDerivedDescriptorTypeMatches(
            source_descriptor, source.nullable, bound_descriptor, true)) {
      return refuse("row result descriptor is not the bound source field");
    }
    result_descriptor_ids.push_back(bound.descriptor_id);
  }

  DescriptorBatch output;
  output.columns = request.result_columns;
  auto schema_validation =
      ValidateCanonicalDescriptorBatch(output, result_descriptor_ids);
  if (!schema_validation.ok) {
    return refuse(schema_validation.diagnostic_code + ":" +
                  schema_validation.detail);
  }
  if (table.materialized_row_count > 1) {
    return refuse("row subquery produced more than one row");
  }

  DescriptorTuple row;
  if (table.materialized_row_count == 0) {
    row.values.reserve(width);
    for (const auto& column : request.result_columns) {
      EngineTypedValue null_value;
      null_value.descriptor = column.descriptor;
      null_value.is_null = true;
      null_value.state = EngineValueState::sql_null;
      row.values.push_back(std::move(null_value));
    }
  } else {
    row = table.output_batch.rows.front();
    for (std::size_t column = 0; column < width; ++column) {
      row.values[column].descriptor =
          request.result_columns[column].descriptor;
    }
  }
  output.rows.push_back(std::move(row));
  auto output_validation =
      ValidateCanonicalDescriptorBatch(output, result_descriptor_ids);
  if (!output_validation.ok) {
    return refuse(output_validation.diagnostic_code + ":" +
                  output_validation.detail);
  }
  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.table_request.mga_authority,
      execution_dag);
  if (!result_authority.ok)
    return refuse(result_authority.diagnostic_code + ":" +
                  result_authority.detail);

  result.diagnostic = {};
  result.output_batch = std::move(output);
  result.source_row_count = table.materialized_row_count;
  result.selected_plan_uuid = std::move(table.selected_plan_uuid);
  result.executed_physical_node_id = table.executed_physical_node_id;
  result.causal_counter_id = table.causal_counter_id;
  result.mga_statement_context =
      request.table_request.mga_authority.statement_context;
  return result;
}
}  // namespace

CanonicalScalarSubqueryResult ExecuteCanonicalScalarSubquery(
    const CanonicalScalarSubqueryRequest& request) {
  return ExecuteCanonicalScalarSubqueryBound(
      request, request.table_request.physical_dag,
      request.table_request.physical_dag.root_physical_node_id,
      request.table_request.input_batch, false);
}

CanonicalScalarSubqueryResult ExecuteCanonicalScalarSubquery(
    const CanonicalScalarSubqueryRequest& request,
    const TypedPhysicalNodeDag& borrowed_execution_dag,
    const std::uint64_t scoped_root_physical_node_id,
    const DescriptorBatch& borrowed_input_batch) {
  return ExecuteCanonicalScalarSubqueryBound(
      request, borrowed_execution_dag, scoped_root_physical_node_id,
      borrowed_input_batch, true);
}

CanonicalRowSubqueryResult ExecuteCanonicalRowSubquery(
    const CanonicalRowSubqueryRequest& request) {
  return ExecuteCanonicalRowSubqueryBound(
      request, request.table_request.physical_dag,
      request.table_request.physical_dag.root_physical_node_id,
      request.table_request.input_batch, false);
}

CanonicalRowSubqueryResult ExecuteCanonicalRowSubquery(
    const CanonicalRowSubqueryRequest& request,
    const TypedPhysicalNodeDag& borrowed_execution_dag,
    const std::uint64_t scoped_root_physical_node_id,
    const DescriptorBatch& borrowed_input_batch) {
  return ExecuteCanonicalRowSubqueryBound(
      request, borrowed_execution_dag, scoped_root_physical_node_id,
      borrowed_input_batch, true);
}

// QOW-SOURCE-QRY-013-EXISTS-V1
// Evaluate EXISTS only after the complete canonical table-subquery result has
// validated. The result is always one bound non-NULL boolean row; no input
// value or parser-level predicate can supply existence authority.
CanonicalExistsSubqueryResult ExecuteCanonicalExistsSubquery(
    const CanonicalExistsSubqueryRequest& request) {
  using scratchbird::engine::internal_api::EngineTypedValue;
  using scratchbird::engine::internal_api::EngineValueState;

  CanonicalExistsSubqueryResult result;
  const auto refuse = [&](std::string detail) {
    result.diagnostic.ok = false;
    result.diagnostic.diagnostic_code =
        "QOW-DIAG-QRY-013-EXISTS-REFUSAL-V1";
    result.diagnostic.detail = std::move(detail);
    result.output_batch = {};
    result.source_row_count = 0;
    result.exists = false;
    result.selected_plan_uuid.clear();
    result.executed_physical_node_id = 0;
    result.causal_counter_id = 0;
    return result;
  };

  auto table = ExecuteCanonicalTableSubqueryBound(
      request.table_request, request.table_request.physical_dag,
      request.table_request.physical_dag.root_physical_node_id,
      TableSubqueryExecutionRoute::exists, false,
      request.table_request.input_batch, false);
  if (!table.diagnostic.ok) {
    return refuse(table.diagnostic.diagnostic_code + ":" +
                  table.diagnostic.detail);
  }
  if (!PhysicalMgaStatementContextEqual(
          table.mga_statement_context,
          request.table_request.mga_authority.statement_context)) {
    return refuse("EXISTS table subquery returned a different MGA statement context");
  }
  const auto result_type_uuid = CanonicalDescriptorField(
      request.result_column.descriptor, "type_uuid");
  const auto canonical_boolean_type_uuid =
      CanonicalCoreDatatypeUuid("boolean");
  if (request.exists_expression_descriptor_id == 0 ||
      request.result_column.descriptor_id !=
          request.exists_expression_descriptor_id ||
      request.result_column.stable_name.empty() ||
      request.result_column.nullable ||
      request.result_column.descriptor.descriptor_kind != "scalar" ||
      request.result_column.descriptor.canonical_type_name != "boolean" ||
      !result_type_uuid.has_value() || canonical_boolean_type_uuid.empty() ||
      *result_type_uuid != canonical_boolean_type_uuid ||
      request.result_column.descriptor.descriptor_uuid.canonical ==
          *result_type_uuid) {
    return refuse("EXISTS result is not a bound non-null boolean");
  }

  DescriptorBatch output;
  output.columns = {request.result_column};
  auto schema_validation = ValidateCanonicalDescriptorBatch(
      output, {request.result_column.descriptor_id});
  if (!schema_validation.ok) {
    return refuse(schema_validation.diagnostic_code + ":" +
                  schema_validation.detail);
  }

  const bool exists = table.materialized_row_count != 0;
  EngineTypedValue value;
  value.descriptor = request.result_column.descriptor;
  value.encoded_value = exists ? "true" : "false";
  value.state = EngineValueState::value;
  output.rows = {{{std::move(value)}}};
  auto output_validation = ValidateCanonicalDescriptorBatch(
      output, {request.result_column.descriptor_id});
  if (!output_validation.ok) {
    return refuse(output_validation.diagnostic_code + ":" +
                  output_validation.detail);
  }
  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.table_request.mga_authority,
      request.table_request.physical_dag);
  if (!result_authority.ok)
    return refuse(result_authority.diagnostic_code + ":" +
                  result_authority.detail);

  result.diagnostic = {};
  result.output_batch = std::move(output);
  result.source_row_count = table.materialized_row_count;
  result.exists = exists;
  result.selected_plan_uuid = std::move(table.selected_plan_uuid);
  result.executed_physical_node_id = table.executed_physical_node_id;
  result.causal_counter_id = table.causal_counter_id;
  result.mga_statement_context =
      request.table_request.mga_authority.statement_context;
  return result;
}

// QOW-SOURCE-QRY-013-QUANTIFIED-V1
// Evaluate one descriptor-compatible canonical scalar comparison against a
// one-column canonical table result with SQL ANY/ALL three-valued folding.
// Every operand is decoded before publication, so a decisive early truth
// cannot hide malformed later input. Collated character values remain behind
// the catalog-bound comparison seam and are refused by the shared expression
// runtime when no such authority is present.
CanonicalQuantifiedSubqueryResult ExecuteCanonicalQuantifiedSubquery(
    const CanonicalQuantifiedSubqueryRequest& request) {
  namespace api = scratchbird::engine::internal_api;

  CanonicalQuantifiedSubqueryResult result;
  const auto refuse = [&](std::string detail) {
    result.diagnostic.ok = false;
    result.diagnostic.diagnostic_code =
        "QOW-DIAG-QRY-013-QUANTIFIED-REFUSAL-V1";
    result.diagnostic.detail = std::move(detail);
    result.output_batch = {};
    result.truth_value = api::EngineSqlTruthValue::unspecified;
    result.comparison_count = 0;
    result.selected_plan_uuid.clear();
    result.executed_physical_node_id = 0;
    result.causal_counter_id = 0;
    return result;
  };

  auto table = ExecuteCanonicalTableSubqueryBound(
      request.table_request, request.table_request.physical_dag,
      request.table_request.physical_dag.root_physical_node_id,
      TableSubqueryExecutionRoute::quantified, false,
      request.table_request.input_batch, false);
  if (!table.diagnostic.ok) {
    return refuse(table.diagnostic.diagnostic_code + ":" +
                  table.diagnostic.detail);
  }
  if (!PhysicalMgaStatementContextEqual(
          table.mga_statement_context,
          request.table_request.mga_authority.statement_context)) {
    return refuse("quantified table subquery returned a different MGA statement context");
  }
  if (table.output_batch.columns.size() != 1) {
    return refuse("quantified subquery requires exactly one result column");
  }
  const auto& right_column = table.output_batch.columns.front();
  if (request.right_expression_descriptor_id == 0 ||
      request.right_expression_descriptor_id != right_column.descriptor_id ||
      request.left_operand_column.descriptor_id == 0 ||
      request.left_operand_column.stable_name.empty()) {
    return refuse("quantified comparison operands are not descriptor-bound");
  }
  const bool any = request.quantifier ==
                   CanonicalQuantifiedSubqueryQuantifier::kAny;
  const bool all = request.quantifier ==
                   CanonicalQuantifiedSubqueryQuantifier::kAll;
  if (!any && !all) {
    return refuse("quantified comparison quantifier is not bound");
  }
  using Operation = api::EngineComparisonPredicateOperator;
  switch (request.comparison_operator) {
    case Operation::equal:
    case Operation::not_equal:
    case Operation::less_than:
    case Operation::less_than_or_equal:
    case Operation::greater_than:
    case Operation::greater_than_or_equal:
      break;
    default:
      return refuse("quantified comparison operator is not bound");
  }
  if (request.maximum_comparison_count == 0 ||
      table.materialized_row_count > request.maximum_comparison_count) {
    return refuse("quantified comparison resource bound was exceeded");
  }
  namespace dt = scratchbird::core::datatypes;
  const bool descriptor_bound_comparison =
      dt::CanonicalTypeIdFromStableName(
          request.left_value.descriptor.canonical_type_name) ==
          dt::CanonicalTypeId::character ||
      dt::CanonicalTypeIdFromStableName(
          right_column.descriptor.canonical_type_name) ==
          dt::CanonicalTypeId::character ||
      request.left_value.descriptor.encoded_descriptor.find(
          "timezone_profile_id=") != std::string::npos ||
      right_column.descriptor.encoded_descriptor.find(
          "timezone_profile_id=") != std::string::npos;
  if (request.comparison_authority_engine_owned !=
          descriptor_bound_comparison ||
      (request.comparison_authority_engine_owned &&
       request.precomputed_comparisons.size() !=
           table.materialized_row_count) ||
      (!request.comparison_authority_engine_owned &&
       !request.precomputed_comparisons.empty())) {
    return refuse(
        "quantified comparison authority carrier is not exact");
  }
  const auto result_type_uuid = CanonicalDescriptorField(
      request.result_column.descriptor, "type_uuid");
  const auto canonical_boolean_type_uuid =
      CanonicalCoreDatatypeUuid("boolean");
  if (request.result_expression_descriptor_id == 0 ||
      request.result_column.descriptor_id !=
          request.result_expression_descriptor_id ||
      request.result_column.stable_name.empty() ||
      !request.result_column.nullable ||
      request.result_column.descriptor.descriptor_kind != "scalar" ||
      request.result_column.descriptor.canonical_type_name != "boolean" ||
      !result_type_uuid.has_value() || canonical_boolean_type_uuid.empty() ||
      *result_type_uuid != canonical_boolean_type_uuid ||
      request.result_column.descriptor.descriptor_uuid.canonical ==
          *result_type_uuid) {
    return refuse("quantified result is not a bound nullable boolean");
  }

  DescriptorBatch left_batch;
  left_batch.columns = {request.left_operand_column};
  left_batch.rows = {{{request.left_value}}};
  auto left_validation = ValidateCanonicalDescriptorBatch(
      left_batch, {request.left_operand_column.descriptor_id});
  if (!left_validation.ok) {
    return refuse(left_validation.diagnostic_code + ":" +
                  left_validation.detail);
  }

  api::EngineCanonicalExpressionOperation expression_operation =
      api::EngineCanonicalExpressionOperation::equal;
  if (request.comparison_operator == Operation::not_equal) {
    expression_operation =
        api::EngineCanonicalExpressionOperation::not_equal;
  } else if (request.comparison_operator == Operation::less_than) {
    expression_operation =
        api::EngineCanonicalExpressionOperation::less_than;
  } else if (request.comparison_operator == Operation::less_than_or_equal) {
    expression_operation =
        api::EngineCanonicalExpressionOperation::less_than_or_equal;
  } else if (request.comparison_operator == Operation::greater_than) {
    expression_operation =
        api::EngineCanonicalExpressionOperation::greater_than;
  } else if (request.comparison_operator ==
             Operation::greater_than_or_equal) {
    expression_operation =
        api::EngineCanonicalExpressionOperation::greater_than_or_equal;
  }

  std::vector<api::EngineSqlTruthValue> comparison_truths;
  comparison_truths.reserve(table.materialized_row_count);
  for (std::size_t row_index = 0;
       row_index < table.output_batch.rows.size(); ++row_index) {
    const auto& row = table.output_batch.rows[row_index];
    const auto& right_value = row.values.front();
    api::EngineCanonicalExpressionEvaluationRequest expression_request;
    expression_request.consumer =
        api::EngineCanonicalExpressionConsumer::subquery;
    expression_request.operation = expression_operation;
    expression_request.left_value = request.left_value;
    expression_request.right_value = right_value;
    expression_request.result_descriptor =
        request.result_column.descriptor;
    if (request.comparison_authority_engine_owned) {
      const auto& comparison =
          request.precomputed_comparisons[row_index];
      const bool null_comparison = request.left_value.isSqlNull() ||
                                   right_value.isSqlNull();
      if (comparison.has_value() == null_comparison ||
          (comparison.has_value() &&
           (*comparison < -1 || *comparison > 1))) {
        return refuse(
            "quantified comparison authority row is not canonical");
      }
      expression_request.precomputed_comparison = comparison;
    }
    api::EngineCanonicalExpressionEvaluationResult expression_result;
    std::string expression_detail;
    if (!api::QowEvaluateCanonicalTypedExpressionV1(
            expression_request, &expression_result, &expression_detail)) {
      return refuse("canonical quantified comparison refused: " +
                    expression_detail);
    }
    comparison_truths.push_back(expression_result.truth);
  }

  auto truth = any ? api::EngineSqlTruthValue::false_value
                   : api::EngineSqlTruthValue::true_value;
  for (const auto comparison_truth : comparison_truths) {
    if (any) {
      if (comparison_truth == api::EngineSqlTruthValue::true_value) {
        truth = api::EngineSqlTruthValue::true_value;
      } else if (comparison_truth == api::EngineSqlTruthValue::unknown &&
                 truth == api::EngineSqlTruthValue::false_value) {
        truth = api::EngineSqlTruthValue::unknown;
      }
    } else {
      if (comparison_truth == api::EngineSqlTruthValue::false_value) {
        truth = api::EngineSqlTruthValue::false_value;
      } else if (comparison_truth == api::EngineSqlTruthValue::unknown &&
                 truth == api::EngineSqlTruthValue::true_value) {
        truth = api::EngineSqlTruthValue::unknown;
      }
    }
  }

  DescriptorBatch output;
  output.columns = {request.result_column};
  api::EngineCanonicalExpressionEvaluationRequest truth_request;
  truth_request.consumer =
      api::EngineCanonicalExpressionConsumer::subquery;
  truth_request.operation =
      api::EngineCanonicalExpressionOperation::consume_truth;
  truth_request.input_truth = truth;
  truth_request.result_descriptor = request.result_column.descriptor;
  api::EngineCanonicalExpressionEvaluationResult truth_result;
  std::string truth_detail;
  if (!api::QowEvaluateCanonicalTypedExpressionV1(
          truth_request, &truth_result, &truth_detail)) {
    return refuse("canonical quantified truth publication refused: " +
                  truth_detail);
  }
  output.rows = {{{std::move(truth_result.value)}}};
  auto output_validation = ValidateCanonicalDescriptorBatch(
      output, {request.result_column.descriptor_id});
  if (!output_validation.ok) {
    return refuse(output_validation.diagnostic_code + ":" +
                  output_validation.detail);
  }
  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.table_request.mga_authority,
      request.table_request.physical_dag);
  if (!result_authority.ok)
    return refuse(result_authority.diagnostic_code + ":" +
                  result_authority.detail);

  result.diagnostic = {};
  result.output_batch = std::move(output);
  result.truth_value = truth;
  result.comparison_count = comparison_truths.size();
  result.selected_plan_uuid = std::move(table.selected_plan_uuid);
  result.executed_physical_node_id = table.executed_physical_node_id;
  result.causal_counter_id = table.causal_counter_id;
  result.mga_statement_context =
      request.table_request.mga_authority.statement_context;
  return result;
}

// QOW-SOURCE-QRY-013-CORRELATED-V1
// Bind and execute one descriptor-compatible scalar-equality subquery scope
// for every outer row. The physical root owns both input relations and emits
// the inner descriptor shape; scope results retain physical outer-row identity
// and deterministic inner order without granting the parser execution
// authority. The legacy int64 implementation identity remains an exact alias
// of the shared typed equality route; character and timezone-profile keys
// arrive with an engine-issued comparison carrier.
namespace {
CanonicalCorrelatedSubqueryResult ExecuteCanonicalCorrelatedSubqueryBound(
    const CanonicalCorrelatedSubqueryRequest& request,
    const TypedPhysicalNodeDag& execution_dag,
    const std::uint64_t scoped_root_physical_node_id,
    const bool borrowed_execution_dag) {
  namespace api = scratchbird::engine::internal_api;

  CanonicalCorrelatedSubqueryResult result;
  const auto refuse = [&](std::string detail) {
    result.diagnostic.ok = false;
    result.diagnostic.diagnostic_code =
        "QOW-DIAG-QRY-013-CORRELATED-REFUSAL-V1";
    result.diagnostic.detail = std::move(detail);
    result.scopes.clear();
    result.scope_execution_count = 0;
    result.comparison_count = 0;
    result.result_row_count = 0;
    result.cancellation_observed = false;
    result.transient_state_cleanup_proven = true;
    result.cancellation_evidence_uuid.clear();
    result.selected_plan_uuid.clear();
    result.executed_physical_node_id = 0;
    result.causal_counter_id = 0;
    return result;
  };
  const auto refuse_poll = [&](CorrelatedCancellationPoll poll) {
    auto diagnostic = std::move(poll.diagnostic);
    auto refused = refuse(diagnostic.detail);
    refused.diagnostic = std::move(diagnostic);
    refused.cancellation_observed = poll.cancelled;
    if (poll.cancelled) {
      refused.cancellation_evidence_uuid =
          request.cancellation_evidence_uuid;
    }
    return refused;
  };
  const auto poll_cancellation = [&](const std::string_view phase) {
    return PollCorrelatedCancellation(request.cancellation_requested, phase);
  };

  if (borrowed_execution_dag &&
      !TypedPhysicalNodeDagCarrierIsExactDefault(request.physical_dag)) {
    return refuse(
        "correlated subquery request carries conflicting physical DAG authority");
  }
  const auto authority_validation = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, execution_dag);
  if (!authority_validation.ok)
    return refuse(authority_validation.diagnostic_code + ":" +
                  authority_validation.detail);
  if (!CorrelatedCancellationEvidenceBound(
          execution_dag, request.cancellation_requested,
          request.cancellation_evidence_uuid)) {
    return refuse("correlated cancellation evidence is not bound");
  }
  if (request.selected_physical_node_id == 0 ||
      request.selected_physical_node_id !=
          scoped_root_physical_node_id) {
    return refuse("selected correlated subquery is not the physical root");
  }

  const PhysicalNodeRecord* selected_node = nullptr;
  const PhysicalNodeRecord* outer_node = nullptr;
  const PhysicalNodeRecord* inner_node = nullptr;
  for (const auto& node : execution_dag.nodes) {
    if (node.physical_node_id == request.selected_physical_node_id) {
      selected_node = &node;
      break;
    }
  }
  const bool correlated_implementation =
      selected_node != nullptr &&
      (selected_node->implementation_id ==
           "subquery.correlated.int64-equality.typed.v1" ||
       selected_node->implementation_id ==
           "subquery.correlated.equality.typed.v1");
  if (selected_node == nullptr ||
      selected_node->node_kind != PhysicalNodeKind::kSubquery ||
      !correlated_implementation ||
      selected_node->input_physical_node_ids.size() != 2 ||
      selected_node->input_physical_node_ids[0] ==
          selected_node->input_physical_node_ids[1]) {
    return refuse("correlated subquery physical profile is not bound");
  }
  for (const auto& node : execution_dag.nodes) {
    if (node.physical_node_id ==
        selected_node->input_physical_node_ids[0]) {
      outer_node = &node;
    }
    if (node.physical_node_id ==
        selected_node->input_physical_node_ids[1]) {
      inner_node = &node;
    }
  }
  if (outer_node == nullptr || inner_node == nullptr ||
      selected_node->output_descriptor_ids !=
          inner_node->output_descriptor_ids) {
    return refuse("correlated subquery input or output handles are unresolved");
  }
  const auto& outer_batch = request.borrowed_outer_batch != nullptr
                                ? *request.borrowed_outer_batch
                                : request.outer_batch;
  const auto& inner_batch = request.borrowed_inner_batch != nullptr
                                ? *request.borrowed_inner_batch
                                : request.inner_batch;

  auto outer_validation = ValidateCorrelatedBatch(
      outer_batch, outer_node->output_descriptor_ids,
      request.cancellation_requested,
      "while validating correlated outer rows");
  if (outer_validation.cancellation.has_value()) {
    return refuse_poll(std::move(*outer_validation.cancellation));
  }
  if (!outer_validation.diagnostic.ok) {
    return refuse(outer_validation.diagnostic.diagnostic_code + ":" +
                  outer_validation.diagnostic.detail);
  }
  auto inner_validation = ValidateCorrelatedBatch(
      inner_batch, inner_node->output_descriptor_ids,
      request.cancellation_requested,
      "while validating correlated inner rows");
  if (inner_validation.cancellation.has_value()) {
    return refuse_poll(std::move(*inner_validation.cancellation));
  }
  if (!inner_validation.diagnostic.ok) {
    return refuse(inner_validation.diagnostic.diagnostic_code + ":" +
                  inner_validation.diagnostic.detail);
  }
  if (request.outer_binding_column >= outer_batch.columns.size() ||
      request.inner_reference_column >= inner_batch.columns.size()) {
    return refuse("correlated binding column is outside its scope");
  }
  const auto& outer_column =
      outer_batch.columns[request.outer_binding_column];
  const auto& inner_column =
      inner_batch.columns[request.inner_reference_column];
  if (request.outer_binding_expression_descriptor_id == 0 ||
      request.outer_binding_expression_descriptor_id !=
          outer_column.descriptor_id ||
      request.inner_reference_expression_descriptor_id == 0 ||
      request.inner_reference_expression_descriptor_id !=
          inner_column.descriptor_id) {
    return refuse("correlated binding is not an exact descriptor handle pair");
  }
  namespace dt = scratchbird::core::datatypes;
  const auto outer_type = dt::CanonicalTypeIdFromStableName(
      outer_column.descriptor.canonical_type_name);
  const auto inner_type = dt::CanonicalTypeIdFromStableName(
      inner_column.descriptor.canonical_type_name);
  if (outer_type == dt::CanonicalTypeId::unknown ||
      outer_type != inner_type) {
    return refuse("correlated binding descriptors are not type-compatible");
  }

  const auto outer_count = outer_batch.rows.size();
  const auto inner_count = inner_batch.rows.size();
  if (request.maximum_scope_execution_count == 0 ||
      outer_count > request.maximum_scope_execution_count ||
      request.maximum_comparison_count == 0 ||
      (outer_count != 0 &&
       inner_count > request.maximum_comparison_count / outer_count) ||
      outer_count * inner_count > request.maximum_comparison_count ||
      request.maximum_result_row_count == 0) {
    return refuse("correlated subquery resource bound was exceeded");
  }
  const bool descriptor_bound_comparison =
      outer_type == dt::CanonicalTypeId::character ||
      inner_type == dt::CanonicalTypeId::character ||
      outer_column.descriptor.encoded_descriptor.find(
          "timezone_profile_id=") != std::string::npos ||
      inner_column.descriptor.encoded_descriptor.find(
          "timezone_profile_id=") != std::string::npos;
  if (request.comparison_authority_engine_owned !=
          descriptor_bound_comparison ||
      (request.comparison_authority_engine_owned &&
       request.precomputed_equality_comparisons.size() !=
           outer_count * inner_count) ||
      (!request.comparison_authority_engine_owned &&
       !request.precomputed_equality_comparisons.empty())) {
    return refuse(
        "correlated comparison authority carrier is not exact");
  }
  if (descriptor_bound_comparison) {
    for (std::size_t outer_index = 0; outer_index < outer_count;
         ++outer_index) {
      const auto& outer_value =
          outer_batch.rows[outer_index]
              .values[request.outer_binding_column];
      for (std::size_t inner_index = 0; inner_index < inner_count;
           ++inner_index) {
        const auto authority_cancellation =
            poll_cancellation(
                "while validating correlated comparison authority");
        if (!authority_cancellation.diagnostic.ok) {
          return refuse_poll(authority_cancellation);
        }
        const auto& inner_value =
            inner_batch.rows[inner_index]
                .values[request.inner_reference_column];
        const auto& comparison =
            request.precomputed_equality_comparisons[
                outer_index * inner_count + inner_index];
        const bool null_comparison = outer_value.isSqlNull() ||
                                     inner_value.isSqlNull();
        if (comparison.has_value() == null_comparison ||
            (comparison.has_value() &&
             (*comparison < -1 || *comparison > 1))) {
          return refuse(
              "correlated comparison authority row is not canonical");
        }
      }
    }
  }

  std::optional<CorrelatedCancellationPoll> key_cancellation;
  const auto validate_keys = [&](const DescriptorBatch& batch,
                                 const std::size_t column) {
    for (const auto& row : batch.rows) {
      auto cancellation =
          poll_cancellation("while validating correlated keys");
      if (!cancellation.diagnostic.ok) {
        key_cancellation = std::move(cancellation);
        return std::string{};
      }
      const auto& value = row.values[column];
      if (value.state == api::EngineValueState::sql_null) continue;
      int comparison = 0;
      std::string detail;
      if (!api::QowCompareCanonicalNonCollatedScalarsV1(
              value, value, &comparison, &detail)) {
        return detail;
      }
    }
    return std::string{};
  };
  if (!descriptor_bound_comparison) {
    if (const auto detail =
            validate_keys(outer_batch, request.outer_binding_column);
        !detail.empty()) {
      return refuse("correlated outer key refused: " + detail);
    }
    if (key_cancellation.has_value()) {
      return refuse_poll(std::move(*key_cancellation));
    }
    if (const auto detail =
            validate_keys(inner_batch, request.inner_reference_column);
        !detail.empty()) {
      return refuse("correlated inner key refused: " + detail);
    }
    if (key_cancellation.has_value()) {
      return refuse_poll(std::move(*key_cancellation));
    }
  }

  std::vector<CanonicalCorrelatedScopeResult> scopes;
  scopes.reserve(outer_count);
  std::size_t comparison_count = 0;
  std::size_t result_row_count = 0;
  for (std::size_t outer_index = 0; outer_index < outer_count; ++outer_index) {
    auto cancellation =
        poll_cancellation("before a correlated outer scope");
    if (!cancellation.diagnostic.ok) {
      return refuse_poll(std::move(cancellation));
    }
    CanonicalCorrelatedScopeResult scope;
    scope.outer_row_index = outer_index;
    if (request.retain_bound_outer_values) {
      scope.bound_outer_value =
          outer_batch.rows[outer_index]
              .values[request.outer_binding_column];
    }
    scope.output_batch.columns = inner_batch.columns;
    const auto& outer_value =
        outer_batch.rows[outer_index]
            .values[request.outer_binding_column];
    if (outer_value.state != api::EngineValueState::sql_null) {
      for (std::size_t inner_index = 0; inner_index < inner_count;
           ++inner_index) {
        cancellation =
            poll_cancellation("while evaluating a correlated scope");
        if (!cancellation.diagnostic.ok) {
          return refuse_poll(std::move(cancellation));
        }
        const auto& inner_value =
            inner_batch.rows[inner_index]
                .values[request.inner_reference_column];
        if (inner_value.state == api::EngineValueState::sql_null) continue;
        if (comparison_count == request.maximum_comparison_count) {
          return refuse("correlated subquery comparison bound was exceeded");
        }
        ++comparison_count;
        int comparison = 0;
        std::string detail;
        if (descriptor_bound_comparison) {
          comparison = *request.precomputed_equality_comparisons[
              outer_index * inner_count + inner_index];
        } else if (!api::QowCompareCanonicalNonCollatedScalarsV1(
                       outer_value, inner_value, &comparison, &detail)) {
          return refuse("correlated key comparison refused: " + detail);
        }
        if (comparison == 0) {
          if (result_row_count == request.maximum_result_row_count) {
            return refuse("correlated subquery result bound was exceeded");
          }
          scope.output_batch.rows.push_back(inner_batch.rows[inner_index]);
          ++result_row_count;
        }
      }
    }
    auto scope_validation = ValidateCorrelatedBatch(
        scope.output_batch, selected_node->output_descriptor_ids,
        request.cancellation_requested,
        "while validating a correlated scope result");
    if (scope_validation.cancellation.has_value()) {
      return refuse_poll(std::move(*scope_validation.cancellation));
    }
    if (!scope_validation.diagnostic.ok) {
      return refuse(scope_validation.diagnostic.diagnostic_code + ":" +
                    scope_validation.diagnostic.detail);
    }
    scopes.push_back(std::move(scope));
  }

  auto cancellation =
      poll_cancellation("before correlated result publication");
  if (!cancellation.diagnostic.ok) {
    return refuse_poll(std::move(cancellation));
  }
  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, execution_dag);
  if (!result_authority.ok)
    return refuse(result_authority.diagnostic_code + ":" +
                  result_authority.detail);

  result.diagnostic = {};
  result.scopes = std::move(scopes);
  result.scope_execution_count = outer_count;
  result.comparison_count = comparison_count;
  result.result_row_count = result_row_count;
  result.transient_state_cleanup_proven = true;
  if (request.cancellation_requested) {
    result.cancellation_evidence_uuid = request.cancellation_evidence_uuid;
  }
  result.selected_plan_uuid = execution_dag.selected_plan_uuid;
  result.executed_physical_node_id = selected_node->physical_node_id;
  result.causal_counter_id = selected_node->causal_counter_id;
  result.mga_statement_context = request.mga_authority.statement_context;
  return result;
}
}  // namespace

CanonicalCorrelatedSubqueryResult ExecuteCanonicalCorrelatedSubquery(
    const CanonicalCorrelatedSubqueryRequest& request) {
  return ExecuteCanonicalCorrelatedSubqueryBound(
      request, request.physical_dag,
      request.physical_dag.root_physical_node_id, false);
}

CanonicalCorrelatedSubqueryResult ExecuteCanonicalCorrelatedSubquery(
    const CanonicalCorrelatedSubqueryRequest& request,
    const TypedPhysicalNodeDag& borrowed_execution_dag,
    const std::uint64_t scoped_root_physical_node_id) {
  return ExecuteCanonicalCorrelatedSubqueryBound(
      request, borrowed_execution_dag, scoped_root_physical_node_id, true);
}

// QOW-SOURCE-QRY-013-LATERAL-V1
// QOW-SOURCE-QRY-013-LATERAL-V2
// Bind and execute INNER/LEFT LATERAL and CROSS/OUTER APPLY table expansion by
// consuming proven correlated scopes. The lateral physical plan shares the
// exact engine MGA statement boundary and owns only outer-plus-inner relational
// flattening/null extension; correlation remains authoritative in
// ExecuteCanonicalCorrelatedSubquery.
CanonicalLateralSubqueryResult ExecuteCanonicalLateralSubquery(
    const CanonicalLateralSubqueryRequest& request) {
  CanonicalLateralSubqueryResult result;
  const auto refuse = [&](std::string detail) {
    result.diagnostic.ok = false;
    result.diagnostic.diagnostic_code =
        "QOW-DIAG-QRY-013-LATERAL-REFUSAL-V1";
    result.diagnostic.detail = std::move(detail);
    result.output_batch = {};
    result.form = CanonicalLateralJoinForm::kInnerLateral;
    result.scope_execution_count = 0;
    result.matched_scope_count = 0;
    result.null_extended_outer_row_count = 0;
    result.output_row_count = 0;
    result.cancellation_observed = false;
    result.transient_state_cleanup_proven = true;
    result.cancellation_evidence_uuid.clear();
    result.correlated_plan_uuid.clear();
    result.selected_plan_uuid.clear();
    result.executed_physical_node_id = 0;
    result.causal_counter_id = 0;
    return result;
  };
  const auto refuse_poll = [&](CorrelatedCancellationPoll poll) {
    auto diagnostic = std::move(poll.diagnostic);
    auto refused = refuse(diagnostic.detail);
    refused.diagnostic = std::move(diagnostic);
    refused.cancellation_observed = poll.cancelled;
    if (poll.cancelled) {
      refused.cancellation_evidence_uuid =
          request.cancellation_evidence_uuid;
    }
    return refused;
  };
  const auto poll_cancellation = [&](const std::string_view phase) {
    return PollCorrelatedCancellation(request.cancellation_requested, phase);
  };

  const auto lateral_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, request.physical_dag);
  const auto correlated_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.correlated_request.mga_authority,
      request.correlated_request.physical_dag);
  if (!lateral_authority.ok || !correlated_authority.ok ||
      !PhysicalMgaStatementContextEqual(
          request.mga_authority.statement_context,
          request.correlated_request.mga_authority.statement_context)) {
    return refuse(!lateral_authority.ok
                      ? lateral_authority.diagnostic_code + ":" +
                            lateral_authority.detail
                      : (!correlated_authority.ok
                             ? correlated_authority.diagnostic_code + ":" +
                                   correlated_authority.detail
                             : "LATERAL and correlation MGA statement contexts differ"));
  }
  if (!CorrelatedCancellationEvidenceBound(
          request.physical_dag, request.cancellation_requested,
          request.cancellation_evidence_uuid) ||
      !CorrelatedCancellationEvidenceBound(
          request.correlated_request.physical_dag,
          request.correlated_request.cancellation_requested,
          request.correlated_request.cancellation_evidence_uuid) ||
      static_cast<bool>(request.cancellation_requested) !=
          static_cast<bool>(request.correlated_request
                                .cancellation_requested) ||
      request.cancellation_evidence_uuid !=
          request.correlated_request.cancellation_evidence_uuid) {
    return refuse("LATERAL and correlation cancellation authority differ");
  }
  if (request.selected_physical_node_id == 0 ||
      request.selected_physical_node_id !=
          request.physical_dag.root_physical_node_id) {
    return refuse("selected LATERAL node is not the physical root");
  }
  if (request.physical_dag.local_transaction_id !=
          request.correlated_request.physical_dag.local_transaction_id ||
      request.physical_dag.statement_snapshot_id !=
          request.correlated_request.physical_dag.statement_snapshot_id) {
    return refuse("LATERAL and correlation MGA statement contexts differ");
  }

  const PhysicalNodeRecord* selected_node = nullptr;
  const PhysicalNodeRecord* outer_node = nullptr;
  const PhysicalNodeRecord* subquery_node = nullptr;
  for (const auto& node : request.physical_dag.nodes) {
    if (node.physical_node_id == request.selected_physical_node_id) {
      selected_node = &node;
      break;
    }
  }
  std::string_view expected_implementation;
  bool null_extend_empty_scopes = false;
  switch (request.form) {
    case CanonicalLateralJoinForm::kInnerLateral:
      expected_implementation =
          "join.lateral-inner.correlated.typed.v1";
      break;
    case CanonicalLateralJoinForm::kLeftLateral:
      expected_implementation =
          "join.lateral-left.correlated.typed.v1";
      null_extend_empty_scopes = true;
      break;
    case CanonicalLateralJoinForm::kCrossApply:
      expected_implementation =
          "join.cross-apply.correlated.typed.v1";
      break;
    case CanonicalLateralJoinForm::kOuterApply:
      expected_implementation =
          "join.outer-apply.correlated.typed.v1";
      null_extend_empty_scopes = true;
      break;
    default:
      return refuse("LATERAL/APPLY form is outside the accepted profile");
  }
  if (selected_node == nullptr ||
      selected_node->node_kind != PhysicalNodeKind::kJoin ||
      selected_node->implementation_id != expected_implementation ||
      selected_node->input_physical_node_ids.size() != 2 ||
      selected_node->input_physical_node_ids[0] ==
          selected_node->input_physical_node_ids[1]) {
    return refuse("LATERAL/APPLY physical profile is not exactly bound");
  }
  for (const auto& node : request.physical_dag.nodes) {
    if (node.physical_node_id ==
        selected_node->input_physical_node_ids[0]) {
      outer_node = &node;
    }
    if (node.physical_node_id ==
        selected_node->input_physical_node_ids[1]) {
      subquery_node = &node;
    }
  }
  if (outer_node == nullptr || subquery_node == nullptr ||
      subquery_node->node_kind != PhysicalNodeKind::kSubquery) {
    return refuse("LATERAL outer or correlated input node is unresolved");
  }
  const auto& outer_batch =
      request.correlated_request.borrowed_outer_batch != nullptr
          ? *request.correlated_request.borrowed_outer_batch
          : request.correlated_request.outer_batch;
  const auto& inner_batch =
      request.correlated_request.borrowed_inner_batch != nullptr
          ? *request.correlated_request.borrowed_inner_batch
          : request.correlated_request.inner_batch;

  std::vector<std::uint32_t> outer_descriptor_ids;
  outer_descriptor_ids.reserve(outer_batch.columns.size());
  for (const auto& column : outer_batch.columns) {
    outer_descriptor_ids.push_back(column.descriptor_id);
  }
  std::vector<std::uint32_t> inner_descriptor_ids;
  inner_descriptor_ids.reserve(inner_batch.columns.size());
  for (const auto& column : inner_batch.columns) {
    inner_descriptor_ids.push_back(column.descriptor_id);
  }
  std::vector<std::uint32_t> output_descriptor_ids = outer_descriptor_ids;
  output_descriptor_ids.insert(output_descriptor_ids.end(),
                               inner_descriptor_ids.begin(),
                               inner_descriptor_ids.end());
  if (outer_node->output_descriptor_ids != outer_descriptor_ids ||
      subquery_node->output_descriptor_ids != inner_descriptor_ids ||
      selected_node->output_descriptor_ids != output_descriptor_ids) {
    return refuse("LATERAL physical descriptor handles drifted");
  }
  if (request.maximum_output_row_count == 0) {
    return refuse("LATERAL output resource bound is zero");
  }

  auto cancellation =
      poll_cancellation("before correlated LATERAL execution");
  if (!cancellation.diagnostic.ok) {
    return refuse_poll(std::move(cancellation));
  }

  auto correlated =
      ExecuteCanonicalCorrelatedSubquery(request.correlated_request);
  if (!correlated.diagnostic.ok) {
    auto diagnostic = std::move(correlated.diagnostic);
    auto refused = refuse(diagnostic.detail);
    refused.diagnostic = std::move(diagnostic);
    refused.cancellation_observed = correlated.cancellation_observed;
    refused.transient_state_cleanup_proven =
        correlated.transient_state_cleanup_proven;
    refused.cancellation_evidence_uuid =
        std::move(correlated.cancellation_evidence_uuid);
    return refused;
  }
  if (!PhysicalMgaStatementContextEqual(
          correlated.mga_statement_context,
          request.mga_authority.statement_context)) {
    return refuse("correlated LATERAL input returned a different MGA statement context");
  }
  if (correlated.scopes.size() !=
      outer_batch.rows.size()) {
    return refuse("correlated scopes do not cover the outer relation");
  }
  std::size_t matched_scope_count = 0;
  std::size_t null_extended_outer_row_count = 0;
  for (std::size_t scope_index = 0; scope_index < correlated.scopes.size();
       ++scope_index) {
    cancellation =
        poll_cancellation("while classifying correlated LATERAL scopes");
    if (!cancellation.diagnostic.ok) {
      return refuse_poll(std::move(cancellation));
    }
    const auto& scope = correlated.scopes[scope_index];
    if (scope.outer_row_index != scope_index) {
      return refuse("correlated scope lost canonical outer-row order");
    }
    if (scope.output_batch.rows.empty()) {
      if (null_extend_empty_scopes) ++null_extended_outer_row_count;
    } else {
      ++matched_scope_count;
    }
  }
  if (correlated.result_row_count >
          std::numeric_limits<std::size_t>::max() -
              null_extended_outer_row_count ||
      correlated.result_row_count + null_extended_outer_row_count >
          request.maximum_output_row_count) {
    return refuse("LATERAL output resource bound was exceeded");
  }

  DescriptorBatch output;
  output.columns = outer_batch.columns;
  output.columns.insert(output.columns.end(),
                        inner_batch.columns.begin(),
                        inner_batch.columns.end());
  const auto inner_column_begin = outer_batch.columns.size();
  if (null_extend_empty_scopes) {
    for (std::size_t column = inner_column_begin;
         column < output.columns.size(); ++column) {
      cancellation =
          poll_cancellation("while deriving LATERAL null descriptors");
      if (!cancellation.diagnostic.ok) {
        return refuse_poll(std::move(cancellation));
      }
      output.columns[column].nullable = true;
      if (!DeriveCanonicalNullableDescriptorEncoding(
              &output.columns[column].descriptor)) {
        return refuse(
            "LATERAL null-extended descriptor lacks an exact nullability "
            "carrier");
      }
    }
  }
  output.rows.reserve(correlated.result_row_count +
                      null_extended_outer_row_count);
  for (auto& scope : correlated.scopes) {
    cancellation =
        poll_cancellation("while flattening a correlated LATERAL scope");
    if (!cancellation.diagnostic.ok) {
      return refuse_poll(std::move(cancellation));
    }
    const auto& outer_row =
        outer_batch.rows[scope.outer_row_index];
    if (scope.output_batch.rows.empty() && null_extend_empty_scopes) {
      DescriptorTuple lateral_row;
      lateral_row.values = outer_row.values;
      for (const auto& column : inner_batch.columns) {
        cancellation =
            poll_cancellation("while forming a null-extended LATERAL row");
        if (!cancellation.diagnostic.ok) {
          return refuse_poll(std::move(cancellation));
        }
        scratchbird::engine::internal_api::EngineTypedValue null_value;
        null_value.descriptor = column.descriptor;
        null_value.is_null = true;
        null_value.state = scratchbird::engine::internal_api::
            EngineValueState::sql_null;
        lateral_row.values.push_back(std::move(null_value));
      }
      output.rows.push_back(std::move(lateral_row));
      continue;
    }
    for (auto& inner_row : scope.output_batch.rows) {
      cancellation =
          poll_cancellation("while flattening a LATERAL result row");
      if (!cancellation.diagnostic.ok) {
        return refuse_poll(std::move(cancellation));
      }
      DescriptorTuple lateral_row;
      lateral_row.values = outer_row.values;
      lateral_row.values.insert(lateral_row.values.end(),
                                std::make_move_iterator(
                                    inner_row.values.begin()),
                                std::make_move_iterator(
                                    inner_row.values.end()));
      output.rows.push_back(std::move(lateral_row));
    }
  }
  if (null_extend_empty_scopes) {
    for (auto& row : output.rows) {
      for (std::size_t column = inner_column_begin;
           column < output.columns.size(); ++column) {
        cancellation =
            poll_cancellation("while rebinding LATERAL null descriptors");
        if (!cancellation.diagnostic.ok) {
          return refuse_poll(std::move(cancellation));
        }
        row.values[column].descriptor = output.columns[column].descriptor;
      }
    }
  }
  auto output_validation = ValidateCorrelatedBatch(
      output, output_descriptor_ids, request.cancellation_requested,
      "while validating the LATERAL result");
  if (output_validation.cancellation.has_value()) {
    return refuse_poll(std::move(*output_validation.cancellation));
  }
  if (!output_validation.diagnostic.ok) {
    return refuse(output_validation.diagnostic.diagnostic_code + ":" +
                  output_validation.diagnostic.detail);
  }
  cancellation = poll_cancellation("before LATERAL result publication");
  if (!cancellation.diagnostic.ok) {
    return refuse_poll(std::move(cancellation));
  }
  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, request.physical_dag);
  if (!result_authority.ok)
    return refuse(result_authority.diagnostic_code + ":" +
                  result_authority.detail);

  result.diagnostic = {};
  result.output_batch = std::move(output);
  result.form = request.form;
  result.scope_execution_count = correlated.scope_execution_count;
  result.matched_scope_count = matched_scope_count;
  result.null_extended_outer_row_count = null_extended_outer_row_count;
  result.output_row_count = result.output_batch.rows.size();
  result.transient_state_cleanup_proven = true;
  if (request.cancellation_requested) {
    result.cancellation_evidence_uuid = request.cancellation_evidence_uuid;
  }
  result.correlated_plan_uuid = std::move(correlated.selected_plan_uuid);
  result.selected_plan_uuid = request.physical_dag.selected_plan_uuid;
  result.executed_physical_node_id = selected_node->physical_node_id;
  result.causal_counter_id = selected_node->causal_counter_id;
  result.mga_statement_context = request.mga_authority.statement_context;
  return result;
}

}  // namespace scratchbird::engine::executor
