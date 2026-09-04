// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "datatype_catalog_manifest.hpp"
#include "descriptor_value_runtime.hpp"
#include "sblr_aggregate_window_runtime.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace exec = scratchbird::engine::executor;
namespace api = scratchbird::engine::internal_api;
namespace dt = scratchbird::core::datatypes;
namespace uuid = scratchbird::core::uuid;

namespace {

using scratchbird::engine::sblr::FinalizeSblrAggregateState;
using scratchbird::engine::sblr::InitializeSblrAggregateState;
using scratchbird::engine::sblr::MergeSblrAggregateState;
using scratchbird::engine::sblr::ResolveSblrCanonicalAggregateFunctionUuid;
using scratchbird::engine::sblr::SblrAggregateFinalizeRequest;
using scratchbird::engine::sblr::SblrAggregateOptions;
using scratchbird::engine::sblr::SblrAggregateUpdateRequest;
using scratchbird::engine::sblr::SblrAggregateWindowState;
using scratchbird::engine::sblr::SblrExecutionContext;
using scratchbird::engine::sblr::SblrListAggOverflowMode;
using scratchbird::engine::sblr::SblrStatusCode;
using scratchbird::engine::sblr::SblrValue;
using scratchbird::engine::sblr::SblrValuePayloadKind;
using scratchbird::engine::sblr::UpdateSblrAggregateState;

SblrValue NullValue(std::string descriptor = {}) {
  SblrValue value;
  value.descriptor_id = std::move(descriptor);
  value.is_null = true;
  return value;
}

SblrValue Int64Value(std::int64_t input) {
  SblrValue value;
  value.descriptor_id = "int64";
  value.payload_kind = SblrValuePayloadKind::signed_integer;
  value.is_null = false;
  value.has_int64_value = true;
  value.int64_value = input;
  value.encoded_value = std::to_string(input);
  value.text_value = value.encoded_value;
  return value;
}

SblrValue Real64Value(double input) {
  SblrValue value;
  value.descriptor_id = "real64";
  value.payload_kind = SblrValuePayloadKind::real64;
  value.is_null = false;
  value.has_real64_value = true;
  value.real64_value = input;
  value.encoded_value = std::to_string(input);
  value.text_value = value.encoded_value;
  return value;
}

SblrValue TextValue(std::string input) {
  SblrValue value;
  value.descriptor_id = "text";
  value.payload_kind = SblrValuePayloadKind::text;
  value.is_null = false;
  value.encoded_value = std::move(input);
  value.text_value = value.encoded_value;
  return value;
}

bool HasDiagnostic(const scratchbird::engine::sblr::SblrResult& result, std::string_view id) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.diagnostic_id == id) return true;
  }
  return false;
}

scratchbird::engine::sblr::SblrResult RunAggregate(std::string_view function_id,
                                                   std::string result_descriptor,
                                                   const std::vector<SblrValue>& values,
                                                   const SblrValue* option = nullptr,
                                                   const SblrAggregateOptions* aggregate_options = nullptr) {
  SblrAggregateWindowState state;
  SblrExecutionContext context;
  context.database_uuid = "SBSFC-015-runtime-db";
  context.transaction_uuid = "SBSFC-015-runtime-tx";
  context.transaction_context_present = true;

  auto init = InitializeSblrAggregateState(function_id, std::string(ResolveSblrCanonicalAggregateFunctionUuid(function_id)), std::move(result_descriptor), context, &state);
  if (!init.ok()) return init;

  for (const auto& value : values) {
    SblrAggregateUpdateRequest update;
    update.context = context;
    if (aggregate_options != nullptr) update.options = *aggregate_options;
    update.values.push_back(value);
    if (option != nullptr) update.values.push_back(*option);
    auto update_result = UpdateSblrAggregateState(&state, update);
    if (!update_result.ok()) return update_result;
  }

  SblrAggregateFinalizeRequest finalize;
  finalize.context = context;
  if (aggregate_options != nullptr) finalize.options = *aggregate_options;
  return FinalizeSblrAggregateState(state, finalize);
}

scratchbird::engine::sblr::SblrResult RunAggregateRows(std::string_view function_id,
                                                       std::string result_descriptor,
                                                       const std::vector<std::vector<SblrValue>>& rows) {
  SblrAggregateWindowState state;
  SblrExecutionContext context;
  context.database_uuid = "SBSFC-015-runtime-db";
  context.transaction_uuid = "SBSFC-015-runtime-tx";
  context.transaction_context_present = true;

  auto init = InitializeSblrAggregateState(function_id, std::string(ResolveSblrCanonicalAggregateFunctionUuid(function_id)), std::move(result_descriptor), context, &state);
  if (!init.ok()) return init;

  for (const auto& row : rows) {
    SblrAggregateUpdateRequest update;
    update.context = context;
    update.values = row;
    auto update_result = UpdateSblrAggregateState(&state, update);
    if (!update_result.ok()) return update_result;
  }

  SblrAggregateFinalizeRequest finalize;
  finalize.context = context;
  return FinalizeSblrAggregateState(state, finalize);
}

scratchbird::engine::sblr::SblrResult RunMergedAggregateRows(std::string_view function_id,
                                                             std::string result_descriptor,
                                                             const std::vector<std::vector<SblrValue>>& left_rows,
                                                             const std::vector<std::vector<SblrValue>>& right_rows) {
  SblrAggregateWindowState left_state;
  SblrAggregateWindowState right_state;
  SblrExecutionContext context;
  context.database_uuid = "SBSFC-015-runtime-db";
  context.transaction_uuid = "SBSFC-015-runtime-tx";
  context.transaction_context_present = true;

  auto left_init = InitializeSblrAggregateState(function_id, std::string(ResolveSblrCanonicalAggregateFunctionUuid(function_id)), result_descriptor, context, &left_state);
  if (!left_init.ok()) return left_init;
  auto right_init = InitializeSblrAggregateState(function_id, std::string(ResolveSblrCanonicalAggregateFunctionUuid(function_id)), std::move(result_descriptor), context, &right_state);
  if (!right_init.ok()) return right_init;

  for (const auto& row : left_rows) {
    SblrAggregateUpdateRequest update;
    update.context = context;
    update.values = row;
    auto update_result = UpdateSblrAggregateState(&left_state, update);
    if (!update_result.ok()) return update_result;
  }
  for (const auto& row : right_rows) {
    SblrAggregateUpdateRequest update;
    update.context = context;
    update.values = row;
    auto update_result = UpdateSblrAggregateState(&right_state, update);
    if (!update_result.ok()) return update_result;
  }

  auto merge_result = MergeSblrAggregateState(&left_state, right_state, context);
  if (!merge_result.ok()) return merge_result;

  SblrAggregateFinalizeRequest finalize;
  finalize.context = context;
  return FinalizeSblrAggregateState(left_state, finalize);
}

scratchbird::engine::sblr::SblrResult RunMergedAggregate(std::string_view function_id,
                                                         std::string result_descriptor,
                                                         const std::vector<SblrValue>& left_values,
                                                         const std::vector<SblrValue>& right_values,
                                                         const SblrValue* option = nullptr,
                                                         const SblrAggregateOptions* aggregate_options = nullptr) {
  SblrAggregateWindowState left_state;
  SblrAggregateWindowState right_state;
  SblrExecutionContext context;
  context.database_uuid = "SBSFC-015-runtime-db";
  context.transaction_uuid = "SBSFC-015-runtime-tx";
  context.transaction_context_present = true;

  auto left_init = InitializeSblrAggregateState(function_id, std::string(ResolveSblrCanonicalAggregateFunctionUuid(function_id)), result_descriptor, context, &left_state);
  if (!left_init.ok()) return left_init;
  auto right_init = InitializeSblrAggregateState(function_id, std::string(ResolveSblrCanonicalAggregateFunctionUuid(function_id)), std::move(result_descriptor), context, &right_state);
  if (!right_init.ok()) return right_init;

  auto apply_value = [&](SblrAggregateWindowState* state, const SblrValue& value) {
    SblrAggregateUpdateRequest update;
    update.context = context;
    if (aggregate_options != nullptr) update.options = *aggregate_options;
    update.values.push_back(value);
    if (option != nullptr) update.values.push_back(*option);
    return UpdateSblrAggregateState(state, update);
  };

  for (const auto& value : left_values) {
    auto update_result = apply_value(&left_state, value);
    if (!update_result.ok()) return update_result;
  }
  for (const auto& value : right_values) {
    auto update_result = apply_value(&right_state, value);
    if (!update_result.ok()) return update_result;
  }

  auto merge_result = MergeSblrAggregateState(&left_state, right_state, context);
  if (!merge_result.ok()) return merge_result;

  SblrAggregateFinalizeRequest finalize;
  finalize.context = context;
  if (aggregate_options != nullptr) finalize.options = *aggregate_options;
  return FinalizeSblrAggregateState(left_state, finalize);
}

std::string CoreTypeUuid(const std::string_view stable_name) {
  const auto manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
  if (!manifest.ok()) std::abort();
  const auto found = std::ranges::find_if(
      manifest.manifest.descriptor_rows,
      [&](const auto& row) { return row.stable_name == stable_name; });
  if (found == manifest.manifest.descriptor_rows.end() ||
      !found->descriptor_uuid.valid()) {
    std::abort();
  }
  const auto descriptor_uuid = uuid::UuidToString(found->descriptor_uuid.value);
  const auto identity = dt::LookupDatatypeTypeCodecIdentityV1(
      "019d0000-0000-7000-8000-00000000d701",
      manifest.manifest.catalog_epoch, 1, descriptor_uuid,
      found->descriptor_epoch);
  return identity.ok ? identity.row.type_uuid : descriptor_uuid;
}

std::string CoreAggregateTypeUuid(const std::string_view stable_name) {
  const auto manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
  if (!manifest.ok()) std::abort();
  const auto found = std::ranges::find_if(
      manifest.manifest.descriptor_rows,
      [&](const auto& row) { return row.stable_name == stable_name; });
  if (found == manifest.manifest.descriptor_rows.end() ||
      !found->descriptor_uuid.valid()) {
    std::abort();
  }
  const auto descriptor_uuid = uuid::UuidToString(found->descriptor_uuid.value);
  const auto identity = dt::LookupDatatypeTypeCodecIdentityV1(
      "019d0000-0000-7000-8000-00000000d701",
      manifest.manifest.catalog_epoch, 1, descriptor_uuid,
      found->descriptor_epoch);
  return identity.ok ? identity.row.type_uuid : descriptor_uuid;
}

std::string FixtureUuid(const unsigned value) {
  char buffer[37]{};
  std::snprintf(buffer, sizeof(buffer),
                "019f1500-0000-7600-8000-%012u", value);
  return buffer;
}

api::EngineDescriptor CanonicalDescriptor(const unsigned identity,
                                          const std::string_view type_name,
                                          const bool nullable) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = FixtureUuid(identity);
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = std::string(type_name);
  descriptor.encoded_descriptor =
      "type_uuid=" + CoreTypeUuid(type_name) +
      ";nullability=" + (nullable ? "nullable" : "non_null");
  return descriptor;
}

api::EngineTypedValue CanonicalValue(const api::EngineDescriptor& descriptor,
                                     const std::string& encoded) {
  api::EngineTypedValue value;
  value.descriptor = descriptor;
  value.encoded_value = encoded;
  value.is_null = false;
  value.state = api::EngineValueState::value;
  return value;
}

exec::CanonicalExecutionMgaAuthority BindCanonicalPhysicalDag(
    exec::TypedPhysicalNodeDag* dag) {
  constexpr std::uint64_t kLocal = (std::uint64_t{1} << 32) + 15015;
  constexpr std::uint64_t kHighwater = (std::uint64_t{1} << 32) + 15014;
  dag->abi_version = 2;
  dag->local_transaction_id = kLocal;
  dag->statement_snapshot_id = kHighwater;
  dag->mga_statement_context = {
      FixtureUuid(15101), FixtureUuid(15102), FixtureUuid(15103),
      FixtureUuid(15104), kLocal, kHighwater, kLocal, kLocal, kLocal, kLocal,
      {kLocal}, {}, "statement_stable", kLocal + 1, true, true, true};
  dag->bound_sblr_tree_uuid = FixtureUuid(15110);
  dag->catalog_epoch_uuid = FixtureUuid(15111);
  dag->security_context_uuid = FixtureUuid(15112);
  dag->capability_snapshot_uuid = FixtureUuid(15113);
  dag->resource_snapshot_uuid = FixtureUuid(15114);
  dag->statistics_snapshot_uuid = FixtureUuid(15115);
  dag->route_snapshot_uuid = FixtureUuid(15116);
  dag->catalog_generation = 15121;
  dag->security_epoch = 15122;
  dag->policy_epoch = 15123;
  dag->resource_epoch = 15124;
  dag->statistics_generation = 15125;
  dag->route_epoch = 15126;
  dag->route_generation = 15127;
  dag->memory_budget_bytes = 128U * 1024U * 1024U;
  dag->optimizer_published = true;
  dag->immutable_node_identity_validated = true;
  dag->capability_validated_before_access = true;
  dag->admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       dag->bound_sblr_tree_uuid},
      {exec::PhysicalAdmissionStage::kCatalogEpoch, dag->catalog_epoch_uuid},
      {exec::PhysicalAdmissionStage::kSecurity, dag->security_context_uuid},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       dag->mga_statement_context.statement_snapshot_uuid},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       dag->capability_snapshot_uuid},
      {exec::PhysicalAdmissionStage::kResource, dag->resource_snapshot_uuid},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       dag->statistics_snapshot_uuid},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       dag->route_snapshot_uuid},
  };
  for (std::size_t index = 0; index < dag->nodes.size(); ++index) {
    auto& node = dag->nodes[index];
    node.selected_alternative_uuid =
        FixtureUuid(15200 + static_cast<unsigned>(index) * 3);
    node.executor_capability_uuid =
        FixtureUuid(15201 + static_cast<unsigned>(index) * 3);
    node.executor_capability_abi_version = 1;
    node.cost_vector_uuid =
        FixtureUuid(15202 + static_cast<unsigned>(index) * 3);
    node.memory_bytes_required = 32U * 1024U * 1024U;
    node.engine_capability_validated = true;
    node.mga_statement_context = dag->mga_statement_context;
  }
  exec::CanonicalExecutionMgaAuthority authority;
  authority.statement_context = dag->mga_statement_context;
  authority.origin = exec::CanonicalMgaAuthorityOrigin::kClosureTestSeam;
  const auto current = dag->mga_statement_context;
  authority.resolve_current = [current] {
    exec::CanonicalMgaCurrentResolution resolution;
    resolution.statement_context = current;
    return resolution;
  };
  return authority;
}

struct CanonicalWindowFixture {
  exec::TypedPhysicalNodeDag dag;
  exec::DescriptorBatch batch;
  exec::CanonicalDescriptorOrderTerm order_term;
  exec::ExecutorColumnDescriptor result_column;
  exec::CanonicalExecutionMgaAuthority authority;
  std::string ordering_property_uuid;
  std::string window_property_uuid;
  std::string order_term_binding_evidence_uuid;
  std::string deterministic_order_evidence_uuid;
  std::string window_frame_descriptor_uuid;
  std::string frame_property_binding_evidence_uuid;
};

CanonicalWindowFixture MakeCanonicalWindowFixture(
    const std::vector<std::int64_t>& ordered_values,
    const std::string_view implementation_id,
    const std::string_view result_type,
    const bool result_nullable,
    const bool capability_is_order_term_receipt = true) {
  constexpr std::uint32_t kInputDescriptorId = 15301;
  constexpr std::uint32_t kResultDescriptorId = 15302;
  constexpr std::uint64_t kValuesNodeId = 15311;
  constexpr std::uint64_t kSortNodeId = 15312;
  constexpr std::uint64_t kWindowNodeId = 15313;
  const auto input_descriptor = CanonicalDescriptor(15321, "int64", false);
  const auto result_descriptor =
      CanonicalDescriptor(15322, result_type, result_nullable);

  CanonicalWindowFixture fixture;
  fixture.dag.selected_plan_uuid = FixtureUuid(15330);
  fixture.dag.root_physical_node_id = kWindowNodeId;
  fixture.dag.nodes = {
      {.physical_node_id = kValuesNodeId,
       .relational_node_id = 15311,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.typed.v1",
       .output_descriptor_ids = {kInputDescriptorId},
       .causal_counter_id = 153101},
      {.physical_node_id = kSortNodeId,
       .relational_node_id = 15312,
       .node_kind = exec::PhysicalNodeKind::kSort,
       .implementation_id = "sort.typed.order-proof.v1",
       .input_physical_node_ids = {kValuesNodeId},
       .output_descriptor_ids = {kInputDescriptorId},
       .causal_counter_id = 153102},
      {.physical_node_id = kWindowNodeId,
       .relational_node_id = 15313,
       .node_kind = exec::PhysicalNodeKind::kWindow,
       .implementation_id = std::string(implementation_id),
       .input_physical_node_ids = {kSortNodeId},
       .output_descriptor_ids = {kInputDescriptorId, kResultDescriptorId},
       .causal_counter_id = 153103},
  };
  fixture.authority = BindCanonicalPhysicalDag(&fixture.dag);
  fixture.ordering_property_uuid = FixtureUuid(15331);
  fixture.window_property_uuid = FixtureUuid(15332);
  fixture.deterministic_order_evidence_uuid = FixtureUuid(15333);
  fixture.window_frame_descriptor_uuid = FixtureUuid(15334);
  fixture.frame_property_binding_evidence_uuid = FixtureUuid(15335);
  fixture.dag.nodes[1].delivered_property_uuids = {
      fixture.ordering_property_uuid};
  fixture.dag.nodes[2].required_property_uuids = {
      fixture.ordering_property_uuid};
  fixture.dag.nodes[2].delivered_property_uuids = {
      fixture.ordering_property_uuid, fixture.window_property_uuid};
  fixture.order_term = {
      .column = 0,
      .expression_descriptor_id = kInputDescriptorId,
      .direction = exec::CanonicalDescriptorOrderDirection::ascending,
      .null_placement = exec::CanonicalDescriptorNullPlacement::last,
  };
  std::uint64_t planned_workspace = 0;
  std::uint64_t actual_workspace = 0;
  if (!exec::PlanCanonicalDescriptorOrderTermBindingEvidenceWorkspace(
          fixture.order_term, fixture.ordering_property_uuid,
          &planned_workspace)) {
    std::abort();
  }
  fixture.order_term_binding_evidence_uuid =
      exec::ComputeCanonicalDescriptorOrderTermBindingEvidenceUuid(
          fixture.order_term, fixture.ordering_property_uuid,
          planned_workspace, &actual_workspace);
  if (fixture.order_term_binding_evidence_uuid.empty() ||
      actual_workspace != planned_workspace) {
    std::abort();
  }
  if (capability_is_order_term_receipt) {
    fixture.dag.nodes[2].executor_capability_uuid =
        fixture.order_term_binding_evidence_uuid;
  }
  fixture.batch.columns = {
      {"order_value", input_descriptor, false, kInputDescriptorId}};
  fixture.batch.rows.reserve(ordered_values.size());
  for (const auto value : ordered_values) {
    fixture.batch.rows.push_back(
        {{CanonicalValue(input_descriptor, std::to_string(value))}});
  }
  fixture.result_column = {"window_result", result_descriptor,
                           result_nullable, kResultDescriptorId};
  return fixture;
}

scratchbird::engine::sblr::SblrResult CanonicalFailure(
    const std::string_view diagnostic_id,
    const std::string_view detail) {
  scratchbird::engine::sblr::SblrResult result;
  result.status = SblrStatusCode::execution_failed;
  scratchbird::engine::sblr::SblrRuntimeDiagnostic diagnostic;
  diagnostic.diagnostic_id = std::string(diagnostic_id);
  diagnostic.message_key = "engine.window.canonical_descriptor_refusal";
  diagnostic.detail = std::string(detail);
  result.diagnostics.push_back(std::move(diagnostic));
  return result;
}

scratchbird::engine::sblr::SblrResult CanonicalScalar(SblrValue value) {
  scratchbird::engine::sblr::SblrResult result;
  result.operation_id = "engine.op.query_window_canonical_descriptor";
  result.scalar_values.push_back(std::move(value));
  return result;
}

scratchbird::engine::sblr::SblrResult CanonicalDiagnosticFailure(
    const exec::DescriptorRuntimeDiagnostic& diagnostic) {
  return CanonicalFailure(diagnostic.diagnostic_code, diagnostic.detail);
}

scratchbird::engine::sblr::SblrResult CanonicalNumericScalar(
    const api::EngineTypedValue& value) {
  if (value.state == api::EngineValueState::sql_null || value.is_null) {
    return CanonicalScalar(NullValue(value.descriptor.canonical_type_name));
  }
  try {
    if (value.descriptor.canonical_type_name == "int64") {
      return CanonicalScalar(Int64Value(std::stoll(value.encoded_value)));
    }
    if (value.descriptor.canonical_type_name == "real64") {
      return CanonicalScalar(Real64Value(std::stod(value.encoded_value)));
    }
  } catch (const std::exception&) {
    return CanonicalFailure("QOW-DIAG-WINDOW-RUNTIME-PAYLOAD",
                            "canonical window output is not decodable");
  }
  return CanonicalFailure("QOW-DIAG-WINDOW-RUNTIME-PAYLOAD",
                          "canonical window output type is unsupported");
}

scratchbird::engine::sblr::SblrResult CanonicalWindowOutputAt(
    const exec::DescriptorBatch& output,
    const std::size_t row_index) {
  if (row_index >= output.rows.size() || output.columns.empty() ||
      output.rows[row_index].values.size() != output.columns.size()) {
    return CanonicalFailure("QOW-DIAG-WINDOW-RUNTIME-PAYLOAD",
                            "canonical window output row is missing");
  }
  return CanonicalNumericScalar(output.rows[row_index].values.back());
}

scratchbird::engine::sblr::SblrResult RunCanonicalHypotheticalAggregate(
    const exec::CanonicalAggregateFunction function,
    const std::vector<SblrValue>& values,
    const SblrValue& hypothetical) {
  constexpr std::uint32_t kInputDescriptorId = 15401;
  constexpr std::uint32_t kResultDescriptorId = 15402;
  constexpr std::uint64_t kValuesNodeId = 15411;
  constexpr std::uint64_t kAggregateNodeId = 15412;
  const auto input_descriptor = CanonicalDescriptor(15421, "int64", false);
  const bool integer_result =
      function == exec::CanonicalAggregateFunction::rank ||
      function == exec::CanonicalAggregateFunction::dense_rank;
  const auto result_descriptor = CanonicalDescriptor(
      15422, integer_result ? "int64" : "real64", false);
  auto canonical_result_descriptor = result_descriptor;
  canonical_result_descriptor.encoded_descriptor =
      "type_uuid=" +
      CoreAggregateTypeUuid(integer_result ? "int64" : "real64") +
      ";nullability=non_null";

  exec::CanonicalAggregateRuntimeRequest request;
  request.physical_dag.selected_plan_uuid = FixtureUuid(15430);
  request.physical_dag.root_physical_node_id = kAggregateNodeId;
  request.physical_dag.nodes = {
      {.physical_node_id = kValuesNodeId,
       .relational_node_id = 15411,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.typed.v1",
       .output_descriptor_ids = {kInputDescriptorId},
       .causal_counter_id = 154101},
      {.physical_node_id = kAggregateNodeId,
       .relational_node_id = 15412,
       .node_kind = exec::PhysicalNodeKind::kAggregate,
       .implementation_id = "aggregate.registry-core.v1",
       .input_physical_node_ids = {kValuesNodeId},
       .output_descriptor_ids = {kResultDescriptorId},
       .causal_counter_id = 154102},
  };
  request.mga_authority = BindCanonicalPhysicalDag(&request.physical_dag);
  request.selected_physical_node_id = kAggregateNodeId;
  request.input_batch.columns = {
      {"ordered_value", input_descriptor, false, kInputDescriptorId}};
  for (const auto& value : values) {
    if (value.is_null || !value.has_int64_value) {
      return CanonicalFailure(
          "QOW-DIAG-QRY-011-REGISTRY-VALUE-TYPE-V1",
          "hypothetical-set input must be exact int64");
    }
    request.input_batch.rows.push_back(
        {{CanonicalValue(input_descriptor, std::to_string(value.int64_value))}});
  }
  const auto* entry = exec::LookupCanonicalAggregateByFunctionV1(function);
  if (entry == nullptr || !entry->executable) {
    return CanonicalFailure("QOW-DIAG-QRY-011-REGISTRY-IDENTITY-V1",
                            "hypothetical-set registry entry is unavailable");
  }
  request.descriptor = {entry->abi_version, entry->function,
                        entry->builtin_id, entry->function_uuid, false};
  request.value_columns = {0};
  request.value_expression_descriptor_ids = {kInputDescriptorId};
  request.aggregate_order_terms = {{
      .column = 0,
      .expression_descriptor_id = kInputDescriptorId,
      .direction = exec::CanonicalDescriptorOrderDirection::ascending,
      .null_placement = exec::CanonicalDescriptorNullPlacement::last,
  }};
  if (hypothetical.is_null || !hypothetical.has_int64_value) {
    return CanonicalFailure("QOW-DIAG-QRY-011-REGISTRY-DIRECT-V1",
                            "hypothetical-set direct argument must be int64");
  }
  request.direct_arguments = {CanonicalValue(
      input_descriptor, std::to_string(hypothetical.int64_value))};
  request.result_column = {"hypothetical_result", canonical_result_descriptor, false,
                           kResultDescriptorId};
  const auto result = exec::ExecuteCanonicalAggregateRuntime(request);
  if (!result.diagnostic.ok) return CanonicalDiagnosticFailure(result.diagnostic);
  if (result.output_batch.rows.size() != 1 ||
      result.output_batch.rows.front().values.size() != 1) {
    return CanonicalFailure("QOW-DIAG-QRY-011-REGISTRY-OUTPUT-V1",
                            "hypothetical-set aggregate output is missing");
  }
  return CanonicalNumericScalar(result.output_batch.rows.front().values.front());
}

scratchbird::engine::sblr::SblrResult RunCanonicalPeerRanking(
    const std::string_view builtin_id,
    const std::vector<SblrValue>& values,
    const std::size_t current_row_index) {
  std::string_view implementation_id;
  std::string_view function_uuid;
  std::string_view result_type;
  if (builtin_id == "sb.window.rank") {
    implementation_id = "window.rank.v1";
    function_uuid = "019de5fc-2400-7b94-870d-0dd789ca70ab";
    result_type = "int64";
  } else if (builtin_id == "sb.window.dense_rank") {
    implementation_id = "window.dense-rank.v1";
    function_uuid = "019de5fc-2400-741d-bef0-f079fd3ba494";
    result_type = "int64";
  } else if (builtin_id == "sb.window.percent_rank") {
    implementation_id = "window.percent-rank.v1";
    function_uuid = "019de5fc-2400-7d86-86fe-96f3f27b5dd6";
    result_type = "real64";
  } else {
    implementation_id = "window.cume-dist.v1";
    function_uuid = "019de5fc-2400-721c-be64-2568b64a02b9";
    result_type = "real64";
  }
  std::vector<std::int64_t> ordered;
  ordered.reserve(values.size());
  for (const auto& value : values) {
    if (value.is_null || !value.has_int64_value) {
      return CanonicalFailure("SBLR.PLAN_TREE.INVALID_HANDLE",
                              "peer-ranking input must be exact int64");
    }
    ordered.push_back(value.int64_value);
  }
  auto fixture = MakeCanonicalWindowFixture(
      ordered, implementation_id, result_type, false);
  exec::CanonicalDescriptorPeerRankingRequest request;
  request.physical_dag = std::move(fixture.dag);
  request.selected_physical_node_id = request.physical_dag.root_physical_node_id;
  request.ordered_input_batch = std::move(fixture.batch);
  request.order_term = fixture.order_term;
  request.ranking_column = fixture.result_column;
  request.function_abi_version = 1;
  request.builtin_id = std::string(builtin_id);
  request.function_uuid = std::string(function_uuid);
  request.order_term_binding_evidence_uuid =
      fixture.order_term_binding_evidence_uuid;
  request.deterministic_order_evidence_uuid =
      fixture.deterministic_order_evidence_uuid;
  request.maximum_peer_comparisons =
      std::max<std::size_t>(1, ordered.size());
  request.mga_authority = std::move(fixture.authority);
  const auto result = exec::ExecuteCanonicalDescriptorPeerRanking(request);
  if (!result.diagnostic.ok) return CanonicalDiagnosticFailure(result.diagnostic);
  return CanonicalWindowOutputAt(result.output_batch, current_row_index);
}

scratchbird::engine::sblr::SblrResult RunCanonicalRowNumber(
    const std::size_t row_count,
    const std::size_t current_row_index) {
  std::vector<std::int64_t> ordered(row_count);
  for (std::size_t index = 0; index < row_count; ++index) {
    ordered[index] = static_cast<std::int64_t>(index);
  }
  auto fixture = MakeCanonicalWindowFixture(
      ordered, "window.row-number.v1", "int64", false);
  exec::CanonicalDescriptorRowNumberRequest request;
  request.physical_dag = std::move(fixture.dag);
  request.selected_physical_node_id = request.physical_dag.root_physical_node_id;
  request.ordered_input_batch = std::move(fixture.batch);
  request.row_number_column = fixture.result_column;
  request.deterministic_order_evidence_uuid =
      fixture.deterministic_order_evidence_uuid;
  request.mga_authority = std::move(fixture.authority);
  const auto result = exec::ExecuteCanonicalDescriptorRowNumber(request);
  if (!result.diagnostic.ok) return CanonicalDiagnosticFailure(result.diagnostic);
  return CanonicalWindowOutputAt(result.output_batch, current_row_index);
}

scratchbird::engine::sblr::SblrResult RunCanonicalNtile(
    const std::size_t row_count,
    const std::size_t current_row_index,
    const std::uint64_t bucket_count) {
  std::vector<std::int64_t> ordered(row_count);
  for (std::size_t index = 0; index < row_count; ++index) {
    ordered[index] = static_cast<std::int64_t>(index);
  }
  auto fixture = MakeCanonicalWindowFixture(
      ordered, "window.ntile.v1", "int64", false);
  exec::CanonicalDescriptorNtileRequest request;
  request.physical_dag = std::move(fixture.dag);
  request.selected_physical_node_id = request.physical_dag.root_physical_node_id;
  request.ordered_input_batch = std::move(fixture.batch);
  request.order_term = fixture.order_term;
  request.ntile_column = fixture.result_column;
  const auto bucket_descriptor = CanonicalDescriptor(15323, "int64", false);
  request.bucket_count_operand = CanonicalValue(
      bucket_descriptor, std::to_string(bucket_count));
  request.function_abi_version = 1;
  request.builtin_id = "sb.window.ntile";
  request.function_uuid = "019de5fc-2400-7047-9474-232ca488c094";
  request.order_term_binding_evidence_uuid =
      fixture.order_term_binding_evidence_uuid;
  request.deterministic_order_evidence_uuid =
      fixture.deterministic_order_evidence_uuid;
  request.mga_authority = std::move(fixture.authority);
  const auto result = exec::ExecuteCanonicalDescriptorNtile(request);
  if (!result.diagnostic.ok) return CanonicalDiagnosticFailure(result.diagnostic);
  return CanonicalWindowOutputAt(result.output_batch, current_row_index);
}

scratchbird::engine::sblr::SblrResult RunCanonicalNavigation(
    const std::string_view builtin_id,
    const std::vector<SblrValue>& values,
    const std::size_t current_row_index,
    const std::size_t frame_start_index,
    const std::size_t frame_end_exclusive,
    const std::int64_t offset,
    const std::uint64_t nth,
    const SblrValue* default_value) {
  if (current_row_index >= values.size()) {
    return CanonicalFailure("QOW-DIAG-WINDOW-RUNTIME-PAYLOAD",
                            "current window row is outside the input");
  }
  const bool lag = builtin_id == "sb.window.lag";
  const bool lead = builtin_id == "sb.window.lead";
  const bool first = builtin_id == "sb.window.first_value";
  const bool last = builtin_id == "sb.window.last_value";
  const bool nth_value = builtin_id == "sb.window.nth_value";
  if ((lag || lead) && offset < 0) {
    return CanonicalFailure("QOW-DIAG-WINDOW-OFFSET",
                            "navigation offset must be nonnegative");
  }
  if (nth_value && nth == 0) {
    return CanonicalFailure("QOW-DIAG-WINDOW-NTH",
                            "NTH_VALUE position must be positive");
  }

  std::vector<std::size_t> source_indices;
  std::size_t selected_row = 0;
  if (lag || lead) {
    const bool target_present =
        offset <= static_cast<std::int64_t>(values.size()) &&
        (lag ? static_cast<std::uint64_t>(offset) <= current_row_index
             : static_cast<std::uint64_t>(offset) <
                   values.size() - current_row_index);
    if (target_present) {
      const auto target = lag
                              ? current_row_index -
                                    static_cast<std::size_t>(offset)
                              : current_row_index +
                                    static_cast<std::size_t>(offset);
      if (offset == 0) {
        source_indices = {current_row_index};
        selected_row = 0;
      } else if (lag) {
        source_indices = {target, current_row_index};
        selected_row = 1;
      } else {
        source_indices = {current_row_index, target};
        selected_row = 0;
      }
    } else {
      source_indices = {current_row_index};
      selected_row = 0;
    }
  } else {
    const auto frame_begin = std::min(frame_start_index, values.size());
    const auto requested_end =
        frame_end_exclusive == 0 ? values.size() : frame_end_exclusive;
    const auto frame_end = std::min(requested_end, values.size());
    if (frame_begin >= frame_end) {
      return CanonicalScalar(NullValue(values[current_row_index].descriptor_id));
    }
    for (std::size_t index = frame_begin; index < frame_end; ++index) {
      source_indices.push_back(index);
    }
    selected_row = source_indices.size() - 1;
  }

  std::vector<std::int64_t> proxy_values;
  proxy_values.reserve(source_indices.size());
  for (const auto index : source_indices) {
    proxy_values.push_back(static_cast<std::int64_t>(index));
  }
  std::string_view implementation_id;
  std::string_view function_uuid;
  if (lag) {
    implementation_id = "window.lag.v1";
    function_uuid = "019de5fc-2400-782c-8436-9ac310301738";
  } else if (lead) {
    implementation_id = "window.lead.v1";
    function_uuid = "019de5fc-2400-7a06-bc3c-6747cf5be66f";
  } else if (first) {
    implementation_id = "window.first-value.v1";
    function_uuid = "019de5fc-2400-7264-90fb-d25bd0f806f2";
  } else if (last) {
    implementation_id = "window.last-value.v1";
    function_uuid = "019de5fc-2400-7d23-a5be-7ed3f1a5c3ec";
  } else {
    implementation_id = "window.nth-value.v1";
    function_uuid = "019de5fc-2400-7dc9-80e6-9f2ccf08076f";
  }
  auto fixture = MakeCanonicalWindowFixture(
      proxy_values, implementation_id, "int64", true, false);
  exec::CanonicalDescriptorNavigationWindowRequest request;
  request.physical_dag = std::move(fixture.dag);
  request.selected_physical_node_id = request.physical_dag.root_physical_node_id;
  request.ordered_input_batch = std::move(fixture.batch);
  request.order_term = fixture.order_term;
  request.value_column = 0;
  request.result_column = fixture.result_column;
  if (nth_value) {
    const auto nth_descriptor = CanonicalDescriptor(15324, "int64", false);
    request.nth_value_position_operand =
        CanonicalValue(nth_descriptor, std::to_string(nth));
    request.nth_value_from_first_explicit = true;
    request.nth_value_respect_nulls_explicit = true;
  }
  request.function_abi_version = 1;
  request.builtin_id = std::string(builtin_id);
  request.function_uuid = std::string(function_uuid);
  request.window_frame_descriptor_uuid =
      fixture.window_frame_descriptor_uuid;
  request.order_term_binding_evidence_uuid =
      fixture.order_term_binding_evidence_uuid;
  request.deterministic_order_evidence_uuid =
      fixture.deterministic_order_evidence_uuid;
  request.frame_property_binding_evidence_uuid =
      fixture.frame_property_binding_evidence_uuid;
  request.executor_capability_uuid =
      request.physical_dag.nodes.back().executor_capability_uuid;
  const auto pair_bound = std::max<std::size_t>(
      1, proxy_values.size() * proxy_values.size());
  request.maximum_pair_comparisons = pair_bound;
  request.maximum_effective_row_references = pair_bound;
  request.mga_authority = std::move(fixture.authority);
  const auto result =
      exec::ExecuteCanonicalDescriptorNavigationWindow(request);
  if (!result.diagnostic.ok) return CanonicalDiagnosticFailure(result.diagnostic);
  if (selected_row >= result.output_batch.rows.size() ||
      result.output_batch.rows[selected_row].values.empty()) {
    return CanonicalFailure("QOW-DIAG-WINDOW-RUNTIME-PAYLOAD",
                            "canonical navigation output row is missing");
  }
  const auto& output = result.output_batch.rows[selected_row].values.back();
  if (output.state == api::EngineValueState::sql_null || output.is_null) {
    return default_value == nullptr
               ? CanonicalScalar(NullValue(values[current_row_index].descriptor_id))
               : CanonicalScalar(*default_value);
  }
  std::size_t source_index = 0;
  try {
    source_index = static_cast<std::size_t>(std::stoull(output.encoded_value));
  } catch (const std::exception&) {
    return CanonicalFailure("QOW-DIAG-WINDOW-RUNTIME-PAYLOAD",
                            "canonical navigation proxy is not decodable");
  }
  if (source_index >= values.size()) {
    return CanonicalFailure("QOW-DIAG-WINDOW-RUNTIME-PAYLOAD",
                            "canonical navigation proxy escaped the input");
  }
  return CanonicalScalar(values[source_index]);
}

scratchbird::engine::sblr::SblrResult RunWindow(
    std::string function_id,
    const std::vector<SblrValue>& values,
    const std::size_t current_row_index,
    const std::size_t frame_start_index,
    const std::size_t frame_end_exclusive,
    const std::int64_t offset = 1,
    const std::uint64_t ntile_bucket_count = 1,
    const std::uint64_t nth = 1,
    const SblrValue* default_value = nullptr,
    const std::vector<std::uint64_t>& peer_groups = {}) {
  (void)peer_groups;
  std::ranges::transform(function_id, function_id.begin(),
                         [](const unsigned char value) {
                           return static_cast<char>(std::tolower(value));
                         });
  const bool hypothetical =
      default_value != nullptr &&
      (function_id.starts_with("sb.aggregate.") ||
       function_id.find("withingroup") != std::string::npos);
  if (hypothetical) {
    const auto function =
        function_id.find("dense_rank") != std::string::npos
            ? exec::CanonicalAggregateFunction::dense_rank
            : (function_id.find("percent_rank") != std::string::npos
                   ? exec::CanonicalAggregateFunction::percent_rank
                   : (function_id.find("cume_dist") != std::string::npos
                          ? exec::CanonicalAggregateFunction::cume_dist
                          : exec::CanonicalAggregateFunction::rank));
    return RunCanonicalHypotheticalAggregate(function, values,
                                             *default_value);
  }
  if (function_id == "sb.window.rank" ||
      function_id == "sb.window.dense_rank" ||
      function_id == "sb.window.percent_rank" ||
      function_id == "sb.window.cume_dist") {
    return RunCanonicalPeerRanking(function_id, values, current_row_index);
  }
  if (function_id == "row_number" ||
      function_id == "sb.window.row_number") {
    return RunCanonicalRowNumber(values.size(), current_row_index);
  }
  if (function_id == "ntile" || function_id == "sb.window.ntile") {
    return RunCanonicalNtile(values.size(), current_row_index,
                             ntile_bucket_count);
  }
  if (function_id == "lag") function_id = "sb.window.lag";
  if (function_id == "lead") function_id = "sb.window.lead";
  if (function_id == "first_value") function_id = "sb.window.first_value";
  if (function_id == "last_value") function_id = "sb.window.last_value";
  if (function_id == "nth_value") function_id = "sb.window.nth_value";
  if (function_id == "sb.window.lag" ||
      function_id == "sb.window.lead" ||
      function_id == "sb.window.first_value" ||
      function_id == "sb.window.last_value" ||
      function_id == "sb.window.nth_value") {
    return RunCanonicalNavigation(
        function_id, values, current_row_index, frame_start_index,
        frame_end_exclusive, offset, nth, default_value);
  }
  return CanonicalFailure("QOW-DIAG-WINDOW-FUNCTION-DESCRIPTOR",
                          "window function registry identity is unknown");
}

bool ExpectOkScalar(const scratchbird::engine::sblr::SblrResult& result, std::string_view case_id) {
  if (!result.ok() || result.scalar_values.size() != 1) {
    std::cerr << case_id << ": expected one successful scalar result";
    for (const auto& diagnostic : result.diagnostics) {
      std::cerr << " [" << diagnostic.diagnostic_id << ": "
                << diagnostic.detail << "]";
    }
    std::cerr << "\n";
    return false;
  }
  return true;
}

bool ExpectInt64(std::string_view case_id,
                 const scratchbird::engine::sblr::SblrResult& result,
                 std::int64_t expected) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (value.is_null || !value.has_int64_value || value.int64_value != expected) {
    std::cerr << case_id << ": expected int64 " << expected << ", got " << value.encoded_value << "\n";
    return false;
  }
  return true;
}

bool ExpectReal64(std::string_view case_id,
                  const scratchbird::engine::sblr::SblrResult& result,
                  double expected) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (value.is_null || !value.has_real64_value || std::fabs(value.real64_value - expected) > 1e-12) {
    std::cerr << case_id << ": expected real64 " << expected << ", got " << value.encoded_value << "\n";
    return false;
  }
  return true;
}

bool ExpectBool(std::string_view case_id,
                const scratchbird::engine::sblr::SblrResult& result,
                bool expected) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  const std::int64_t expected_int = expected ? 1 : 0;
  if (value.is_null || value.payload_kind != SblrValuePayloadKind::boolean ||
      !value.has_int64_value || value.int64_value != expected_int) {
    std::cerr << case_id << ": expected boolean " << (expected ? "TRUE" : "FALSE")
              << ", got " << value.encoded_value << "\n";
    return false;
  }
  return true;
}

bool ExpectNull(std::string_view case_id,
                const scratchbird::engine::sblr::SblrResult& result,
                std::string_view expected_descriptor) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (!value.is_null || value.descriptor_id != expected_descriptor) {
    std::cerr << case_id << ": expected NULL " << expected_descriptor
              << ", got " << value.encoded_value << "\n";
    return false;
  }
  return true;
}

bool ExpectText(std::string_view case_id,
                const scratchbird::engine::sblr::SblrResult& result,
                std::string_view expected) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (value.is_null || value.text_value != expected) {
    std::cerr << case_id << ": expected text " << expected << ", got " << value.text_value << "\n";
    return false;
  }
  return true;
}

bool ExpectList(std::string_view case_id,
                const scratchbird::engine::sblr::SblrResult& result,
                std::string_view expected_descriptor,
                std::string_view expected) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (value.is_null || value.descriptor_id != expected_descriptor ||
      value.payload_kind != SblrValuePayloadKind::descriptor_payload ||
      value.text_value != expected) {
    std::cerr << case_id << ": expected list " << expected_descriptor << " "
              << expected << ", got " << value.descriptor_id << " "
              << value.text_value << "\n";
    return false;
  }
  return true;
}

bool ExpectFailure(std::string_view case_id,
                   const scratchbird::engine::sblr::SblrResult& result,
                   std::string_view diagnostic_id) {
  if (result.status == SblrStatusCode::ok || !HasDiagnostic(result, diagnostic_id)) {
    std::cerr << case_id << ": expected diagnostic " << diagnostic_id << "\n";
    return false;
  }
  return true;
}

}  // namespace

int main() {
  bool ok = true;
  const SblrValue top_two = Int64Value(2);
  const SblrValue top_zero = Int64Value(0);
  const SblrValue separator_pipe = TextValue("|");
  const SblrValue fraction_quarter = Real64Value(0.25);
  const SblrValue fraction_three_quarters = Real64Value(0.75);
  const SblrValue invalid_fraction = Real64Value(1.5);
  SblrAggregateOptions listagg_overflow_error;
  listagg_overflow_error.listagg_overflow_mode = SblrListAggOverflowMode::error;
  listagg_overflow_error.listagg_max_output_bytes = 12;
  SblrAggregateOptions listagg_truncate_with_count;
  listagg_truncate_with_count.listagg_overflow_mode = SblrListAggOverflowMode::truncate;
  listagg_truncate_with_count.listagg_max_output_bytes = 12;
  listagg_truncate_with_count.listagg_truncation_indicator = "...";
  listagg_truncate_with_count.listagg_with_count = true;
  SblrAggregateOptions listagg_truncate_without_count;
  listagg_truncate_without_count.listagg_overflow_mode = SblrListAggOverflowMode::truncate;
  listagg_truncate_without_count.listagg_max_output_bytes = 14;
  listagg_truncate_without_count.listagg_truncation_indicator = "...";
  listagg_truncate_without_count.listagg_with_count = false;
  SblrAggregateOptions count_star_options;
  count_star_options.count_nulls = true;

  ok = ExpectInt64("count_star_surface",
                   RunAggregate("count(*)|count([DISTINCT]expr)", "int64",
                                {Int64Value(10), Int64Value(20), NullValue("int64")},
                                nullptr,
                                &count_star_options),
                   3) && ok;

  ok = ExpectInt64("count_name_non_null",
                   RunAggregate("count", "int64",
                                {Int64Value(10), NullValue("int64"), Int64Value(30)}),
                   2) && ok;

  ok = ExpectInt64("count_canonical",
                   RunAggregate("sb.aggregate.count", "int64",
                                {TextValue("a"), TextValue("b")}),
                   2) && ok;

  ok = ExpectReal64("avg_canonical",
                    RunAggregate("sb.aggregate.avg", "real64",
                                 {Int64Value(10), Int64Value(20), NullValue("real64")}),
                    15.0) && ok;

  ok = ExpectReal64("avg_name",
                    RunAggregate("avg", "real64",
                                 {Int64Value(7), Int64Value(13)}),
                    10.0) && ok;

  ok = ExpectInt64("min_expr",
                   RunAggregate("min(expr)", "int64",
                                {Int64Value(12), Int64Value(7), NullValue("int64")}),
                   7) && ok;

  ok = ExpectInt64("min_canonical",
                   RunAggregate("sb.aggregate.min", "int64",
                                {Int64Value(12), Int64Value(13)}),
                   12) && ok;

  ok = ExpectInt64("min_name",
                   RunAggregate("min", "int64",
                                {Int64Value(5), Int64Value(9)}),
                   5) && ok;

  ok = ExpectInt64("max_name",
                   RunAggregate("max", "int64",
                                {Int64Value(5), Int64Value(9)}),
                   9) && ok;

  ok = ExpectInt64("max_expr",
                   RunAggregate("max(expr)", "int64",
                                {Int64Value(12), Int64Value(7), NullValue("int64")}),
                   12) && ok;

  ok = ExpectInt64("max_canonical",
                   RunAggregate("sb.aggregate.max", "int64",
                                {Int64Value(12), Int64Value(13)}),
                   13) && ok;

  ok = ExpectBool("bool_or_signature",
                  RunAggregate("bool_or(boolean)", "boolean",
                               {TextValue("false"), NullValue("boolean"), TextValue("true")}),
                  true) && ok;

  ok = ExpectBool("bool_or_canonical",
                  RunAggregate("sb.aggregate.bool_or", "boolean",
                               {TextValue("false"), Int64Value(0)}),
                  false) && ok;

  ok = ExpectNull("bool_or_name_empty",
                  RunAggregate("bool_or", "boolean", {NullValue("boolean")}),
                  "boolean") && ok;

  ok = ExpectBool("bool_and_name",
                  RunAggregate("bool_and", "boolean",
                               {TextValue("true"), TextValue("true"), NullValue("boolean")}),
                  true) && ok;

  ok = ExpectBool("bool_and_signature",
                  RunAggregate("bool_and(boolean)", "boolean",
                               {TextValue("true"), TextValue("false"), NullValue("boolean")}),
                  false) && ok;

  ok = ExpectNull("bool_and_canonical_empty",
                  RunAggregate("sb.aggregate.bool_and", "boolean", {NullValue("boolean")}),
                  "boolean") && ok;

  ok = ExpectInt64("approx_count_distinct",
                   RunAggregate("approx_count_distinct", "int64",
                                {TextValue("alpha"), TextValue("beta"), TextValue("alpha"), NullValue("text")}),
                   2) && ok;

  ok = ExpectText("approx_top_k",
                  RunAggregate("approx_top_k", "json",
                               {TextValue("b"), TextValue("a"), TextValue("b"), TextValue("c"), TextValue("a"), TextValue("b")},
                               &top_two),
                  R"([{"value":"b","count":3},{"value":"a","count":2}])") && ok;

  ok = ExpectText("top_k_merge_preserves_source_limit",
                  RunMergedAggregate("approx_top_k", "json",
                                     {},
                                     {TextValue("b"), TextValue("a"), TextValue("b"),
                                      TextValue("c"), TextValue("a"), TextValue("b")},
                                     &top_two),
                  R"([{"value":"b","count":3},{"value":"a","count":2}])") && ok;

  ok = ExpectReal64("approx_median_even",
                    RunAggregate("approx_median", "real64",
                                 {Int64Value(10), Int64Value(20), Int64Value(30), Int64Value(40)}),
                    25.0) && ok;

  ok = ExpectReal64("approx_percentile_cont",
                    RunAggregate("approx_percentile_cont", "real64",
                                 {Int64Value(10), Int64Value(20), Int64Value(30), Int64Value(40)},
                                 &fraction_three_quarters),
                    32.5) && ok;

  ok = ExpectReal64("approx_percentile_disc",
                    RunAggregate("approx_percentile_disc", "real64",
                                 {Int64Value(10), Int64Value(20), Int64Value(30), Int64Value(40)},
                                 &fraction_three_quarters),
                    30.0) && ok;

  ok = ExpectReal64("percentile_merge_preserves_source_fraction",
                    RunMergedAggregate("percentile_cont", "real64",
                                       {},
                                       {Int64Value(10), Int64Value(20), Int64Value(30), Int64Value(40)},
                                       &fraction_three_quarters),
                    32.5) && ok;

  ok = ExpectReal64("percentile_cont_exact",
                    RunAggregate("percentile_cont", "real64",
                                 {Int64Value(10), Int64Value(20), Int64Value(30), Int64Value(40)},
                                 &fraction_quarter),
                    17.5) && ok;

  ok = ExpectReal64("percentile_disc_exact",
                    RunAggregate("percentile_disc", "real64",
                                 {Int64Value(10), Int64Value(20), Int64Value(30), Int64Value(40)},
                                 &fraction_quarter),
                    10.0) && ok;

  ok = ExpectReal64("mode_frequency",
                    RunAggregate("mode", "real64",
                                 {Int64Value(5), Int64Value(4), Int64Value(5), Int64Value(4)}),
                    4.0) && ok;

  ok = ExpectText("listagg_separator",
                  RunAggregate("LISTAGG", "text",
                               {TextValue("a"), NullValue("text"), TextValue("b"), TextValue("c")},
                               &separator_pipe),
                  "a|b|c") && ok;

  ok = ExpectFailure("listagg_on_overflow_error",
                     RunAggregate("LISTAGG(expr[,sep][ONOVERFLOW...])WITHINGROUP(ORDERBY...)", "text",
                                  {TextValue("north"), TextValue("east"), TextValue("south")},
                                  &separator_pipe,
                                  &listagg_overflow_error),
                     "SB_DIAG_AGGREGATE_LISTAGG_OVERFLOW") && ok;

  ok = ExpectText("listagg_on_overflow_truncate_with_count",
                  RunAggregate("LISTAGG(expr[,sep][ONOVERFLOW...])WITHINGROUP(ORDERBY...)", "text",
                               {TextValue("north"), TextValue("east"), TextValue("south")},
                               &separator_pipe,
                               &listagg_truncate_with_count),
                  "north|...(2)") && ok;

  ok = ExpectText("listagg_on_overflow_truncate_without_count",
                  RunAggregate("LISTAGG(expr[,sep][ONOVERFLOW...])WITHINGROUP(ORDERBY...)", "text",
                               {TextValue("north"), TextValue("east"), TextValue("south")},
                               &separator_pipe,
                               &listagg_truncate_without_count),
                  "north|east|...") && ok;

  ok = ExpectText("listagg_merge_retains_element_boundaries",
                  RunMergedAggregate("LISTAGG(expr[,sep][ONOVERFLOW...])WITHINGROUP(ORDERBY...)", "text",
                                     {TextValue("north"), TextValue("east")},
                                     {TextValue("south")},
                                     &separator_pipe,
                                     &listagg_truncate_with_count),
                  "north|...(2)") && ok;

  ok = ExpectBool("every_all_true",
                  RunAggregate("sb.aggregate.every", "boolean",
                               {TextValue("true"), NullValue("boolean"), TextValue("yes"), Int64Value(1)}),
                  true) && ok;

  ok = ExpectBool("every_false_present",
                  RunAggregate("every(boolean)", "boolean",
                               {TextValue("true"), TextValue("false"), NullValue("boolean")}),
                  false) && ok;

  ok = ExpectNull("every_empty",
                  RunAggregate("every", "boolean", {}),
                  "boolean") && ok;

  ok = ExpectReal64("stddev_sample",
                    RunAggregate("sb.aggregate.stddev", "real64",
                                 {Int64Value(2), Int64Value(4), Int64Value(4), Int64Value(4),
                                  Int64Value(5), Int64Value(5), Int64Value(7), Int64Value(9),
                                  NullValue("real64")}),
                    2.138089935299395) && ok;

  ok = ExpectReal64("stddev_numeric_signature",
                    RunAggregate("stddev(numeric)", "real64",
                                 {Int64Value(1), Int64Value(2), Int64Value(3)}),
                    1.0) && ok;

  ok = ExpectNull("stddev_singleton",
                  RunAggregate("stddev", "real64", {Int64Value(1), NullValue("real64")}),
                  "real64") && ok;

  ok = ExpectReal64("variance_sample",
                    RunAggregate("sb.aggregate.variance", "real64",
                                 {Int64Value(2), Int64Value(4), Int64Value(4), Int64Value(4),
                                  Int64Value(5), Int64Value(5), Int64Value(7), Int64Value(9),
                                  NullValue("real64")}),
                    4.571428571428571) && ok;

  ok = ExpectReal64("variance_numeric_signature",
                    RunAggregate("variance(numeric)", "real64",
                                 {Int64Value(1), Int64Value(2), Int64Value(3)}),
                    1.0) && ok;

  ok = ExpectNull("variance_empty",
                  RunAggregate("variance", "real64", {}),
                  "real64") && ok;

  ok = ExpectReal64("SBSQL-D4A54D6879E1-stddev_pop",
                    RunAggregate("stddev_pop", "real64",
                                 {Int64Value(2), Int64Value(4), Int64Value(4), Int64Value(4),
                                  Int64Value(5), Int64Value(5), Int64Value(7), Int64Value(9),
                                  NullValue("real64")}),
                    2.0) && ok;

  ok = ExpectReal64("SBSQL-46D54006C21A-stddev_pop_numeric",
                    RunAggregate("stddev_pop(numeric)", "real64",
                                 {Int64Value(1), Int64Value(2), Int64Value(3)}),
                    0.81649658092772603) && ok;

  ok = ExpectReal64("SBSQL-1B1392E72628-stddev_pop_canonical_singleton",
                    RunAggregate("sb.aggregate.stddev_pop", "real64",
                                 {Int64Value(1), NullValue("real64")}),
                    0.0) && ok;

  ok = ExpectNull("stddev_pop_empty",
                  RunAggregate("sb.aggregate.stddev_pop", "real64", {}),
                  "real64") && ok;

  ok = ExpectReal64("SBSQL-1926F7E782F3-variance_pop",
                    RunAggregate("variance_pop", "real64",
                                 {Int64Value(2), Int64Value(4), Int64Value(4), Int64Value(4),
                                  Int64Value(5), Int64Value(5), Int64Value(7), Int64Value(9),
                                  NullValue("real64")}),
                    4.0) && ok;

  ok = ExpectReal64("SBSQL-7CBEA5B27835-variance_pop_numeric",
                    RunAggregate("variance_pop(numeric)", "real64",
                                 {Int64Value(1), Int64Value(2), Int64Value(3)}),
                    2.0 / 3.0) && ok;

  ok = ExpectReal64("SBSQL-F89AE449F324-variance_pop_canonical_singleton",
                    RunAggregate("sb.aggregate.variance_pop", "real64",
                                 {Int64Value(1), NullValue("real64")}),
                    0.0) && ok;

  ok = ExpectNull("variance_pop_empty",
                  RunAggregate("sb.aggregate.variance_pop", "real64", {}),
                  "real64") && ok;

  ok = ExpectReal64("corr_positive",
                    RunAggregateRows("sb.aggregate.corr", "real64",
                                     {{Int64Value(1), Int64Value(1)},
                                      {Int64Value(2), Int64Value(2)},
                                      {Int64Value(3), Int64Value(3)},
                                      {NullValue("real64"), Int64Value(4)},
                                      {Int64Value(5), NullValue("real64")}}),
                    1.0) && ok;

  ok = ExpectReal64("corr_y_x_signature",
                    RunAggregateRows("corr(y,x)", "real64",
                                     {{Int64Value(1), Int64Value(3)},
                                      {Int64Value(2), Int64Value(2)},
                                      {Int64Value(3), Int64Value(1)},
                                      {Int64Value(4), Int64Value(0)}}),
                    -1.0) && ok;

  ok = ExpectReal64("corr_merge_state",
                    RunMergedAggregateRows("corr", "real64",
                                           {{Int64Value(1), Int64Value(1)},
                                            {Int64Value(2), Int64Value(2)}},
                                           {{Int64Value(3), Int64Value(3)},
                                            {Int64Value(4), Int64Value(4)}}),
                    1.0) && ok;

  ok = ExpectNull("corr_zero_variance",
                  RunAggregateRows("corr", "real64",
                                   {{Int64Value(1), Int64Value(5)},
                                    {Int64Value(2), Int64Value(5)},
                                    {Int64Value(3), Int64Value(5)}}),
                  "real64") && ok;

  ok = ExpectFailure("every_invalid_input",
                     RunAggregate("every", "boolean", {TextValue("maybe")}),
                     "SB_DIAG_AGGREGATE_BOOLEAN_INPUT_REQUIRED") && ok;

  ok = ExpectFailure("stddev_invalid_input",
                     RunAggregate("stddev", "real64", {TextValue("not-a-number")}),
                     "SB_DIAG_AGGREGATE_NUMERIC_INPUT_REQUIRED") && ok;

  ok = ExpectFailure("variance_invalid_input",
                     RunAggregate("variance", "real64", {TextValue("not-a-number")}),
                     "SB_DIAG_AGGREGATE_NUMERIC_INPUT_REQUIRED") && ok;

  ok = ExpectFailure("corr_missing_pair",
                     RunAggregateRows("corr", "real64", {{Int64Value(1)}}),
                     "SB_DIAG_AGGREGATE_CORR_PAIR_REQUIRED") && ok;

  ok = ExpectFailure("corr_invalid_input",
                     RunAggregateRows("corr", "real64", {{TextValue("not-a-number"), Int64Value(1)}}),
                     "SB_DIAG_AGGREGATE_NUMERIC_INPUT_REQUIRED") && ok;

  ok = ExpectReal64("SBSQL-53E3A168AD26-stddev_samp",
                    RunAggregate("stddev_samp", "real64",
                                 {Int64Value(2), Int64Value(4), Int64Value(4), Int64Value(4),
                                  Int64Value(5), Int64Value(5), Int64Value(7), Int64Value(9),
                                  NullValue("real64")}),
                    2.138089935299395) && ok;

  ok = ExpectReal64("SBSQL-D155F7EC1FE1-stddev_samp_numeric",
                    RunAggregate("stddev_samp(numeric)", "real64",
                                 {Int64Value(1), Int64Value(2), Int64Value(3)}),
                    1.0) && ok;

  ok = ExpectNull("stddev_samp_singleton",
                  RunAggregate("sb.aggregate.stddev_samp", "real64", {Int64Value(1), NullValue("real64")}),
                  "real64") && ok;

  ok = ExpectReal64("SBSQL-4AF99A06B193-variance_samp",
                    RunAggregate("variance_samp", "real64",
                                 {Int64Value(2), Int64Value(4), Int64Value(4), Int64Value(4),
                                  Int64Value(5), Int64Value(5), Int64Value(7), Int64Value(9),
                                  NullValue("real64")}),
                    4.571428571428571) && ok;

  ok = ExpectReal64("SBSQL-482B2C54BAF1-variance_samp_numeric",
                    RunAggregate("variance_samp(numeric)", "real64",
                                 {Int64Value(1), Int64Value(2), Int64Value(3)}),
                    1.0) && ok;

  ok = ExpectNull("variance_samp_empty",
                  RunAggregate("sb.aggregate.variance_samp", "real64", {}),
                  "real64") && ok;

  ok = ExpectReal64("SBSQL-7D77C331D16C-covar_pop",
                    RunAggregateRows("covar_pop", "real64",
                                     {{Int64Value(1), Int64Value(1)},
                                      {Int64Value(2), Int64Value(2)},
                                      {Int64Value(3), Int64Value(3)},
                                      {NullValue("real64"), Int64Value(4)},
                                      {Int64Value(5), NullValue("real64")}}),
                    2.0 / 3.0) && ok;

  ok = ExpectReal64("SBSQL-E662CB944FC2-covar_pop_y_x",
                    RunAggregateRows("covar_pop(y,x)", "real64",
                                     {{Int64Value(1), Int64Value(3)},
                                      {Int64Value(2), Int64Value(2)},
                                      {Int64Value(3), Int64Value(1)},
                                      {Int64Value(4), Int64Value(0)}}),
                    -1.25) && ok;

  ok = ExpectReal64("covar_pop_singleton",
                    RunAggregateRows("sb.aggregate.covar_pop", "real64",
                                     {{Int64Value(42), Int64Value(7)},
                                      {NullValue("real64"), Int64Value(8)}}),
                    0.0) && ok;

  ok = ExpectReal64("SBSQL-5B5757128C3F-covar_samp",
                    RunAggregateRows("covar_samp", "real64",
                                     {{Int64Value(1), Int64Value(1)},
                                      {Int64Value(2), Int64Value(2)},
                                      {Int64Value(3), Int64Value(3)},
                                      {NullValue("real64"), Int64Value(4)},
                                      {Int64Value(5), NullValue("real64")}}),
                    1.0) && ok;

  ok = ExpectReal64("SBSQL-FC78A3D1CF86-covar_samp_y_x",
                    RunAggregateRows("covar_samp(y,x)", "real64",
                                     {{Int64Value(1), Int64Value(3)},
                                      {Int64Value(2), Int64Value(2)},
                                      {Int64Value(3), Int64Value(1)},
                                      {Int64Value(4), Int64Value(0)}}),
                    -5.0 / 3.0) && ok;

  ok = ExpectNull("covar_samp_singleton",
                  RunAggregateRows("sb.aggregate.covar_samp", "real64",
                                   {{Int64Value(42), Int64Value(7)},
                                    {NullValue("real64"), Int64Value(8)}}),
                  "real64") && ok;

  ok = ExpectReal64("covar_samp_merge_state",
                    RunMergedAggregateRows("covar_samp", "real64",
                                           {{Int64Value(1), Int64Value(1)},
                                            {Int64Value(2), Int64Value(2)}},
                                           {{Int64Value(3), Int64Value(3)},
                                            {Int64Value(4), Int64Value(4)}}),
                    5.0 / 3.0) && ok;

  ok = ExpectInt64("SBSQL-1BBEB1E43F45-regr_count",
                   RunAggregateRows("regr_count", "int64",
                                    {{Int64Value(1), Int64Value(1)},
                                     {Int64Value(2), Int64Value(2)},
                                     {Int64Value(3), Int64Value(3)},
                                     {NullValue("real64"), Int64Value(4)},
                                     {Int64Value(5), NullValue("real64")}}),
                   3) && ok;

  ok = ExpectInt64("SBSQL-3C0839E8B792-regr_count_y_x",
                   RunAggregateRows("regr_count(y,x)", "int64",
                                    {{Int64Value(1), Int64Value(3)},
                                     {Int64Value(2), Int64Value(2)},
                                     {Int64Value(3), Int64Value(1)},
                                     {Int64Value(4), Int64Value(0)}}),
                   4) && ok;

  ok = ExpectInt64("regr_count_empty",
                   RunAggregateRows("sb.aggregate.regr_count", "int64",
                                    {{NullValue("real64"), Int64Value(7)},
                                     {Int64Value(8), NullValue("real64")}}),
                   0) && ok;

  const std::vector<std::vector<SblrValue>> regr_positive_rows = {
      {Int64Value(3), Int64Value(1)},
      {Int64Value(5), Int64Value(2)},
      {Int64Value(7), Int64Value(3)},
      {NullValue("real64"), Int64Value(4)},
      {Int64Value(9), NullValue("real64")},
  };
  const std::vector<std::vector<SblrValue>> regr_negative_rows = {
      {Int64Value(8), Int64Value(1)},
      {Int64Value(6), Int64Value(2)},
      {Int64Value(4), Int64Value(3)},
      {Int64Value(2), Int64Value(4)},
  };
  const std::vector<std::vector<SblrValue>> regr_singleton_rows = {
      {Int64Value(42), Int64Value(7)},
      {NullValue("real64"), Int64Value(8)},
  };
  const std::vector<std::vector<SblrValue>> regr_constant_y_rows = {
      {Int64Value(5), Int64Value(1)},
      {Int64Value(5), Int64Value(2)},
      {Int64Value(5), Int64Value(3)},
  };

  ok = ExpectReal64("SBSQL-7102C019D2CF-regr_avgx",
                    RunAggregateRows("regr_avgx", "real64", regr_positive_rows),
                    2.0) && ok;

  ok = ExpectReal64("SBSQL-54324247868A-regr_avgx_y_x",
                    RunAggregateRows("regr_avgx(y,x)", "real64", regr_negative_rows),
                    2.5) && ok;

  ok = ExpectReal64("SBSQL-DF6313DE4B56-regr_avgy",
                    RunAggregateRows("regr_avgy", "real64", regr_positive_rows),
                    5.0) && ok;

  ok = ExpectReal64("SBSQL-189983EF2867-regr_avgy_y_x",
                    RunAggregateRows("regr_avgy(y,x)", "real64", regr_negative_rows),
                    5.0) && ok;

  ok = ExpectReal64("SBSQL-431925B5EC67-regr_intercept",
                    RunAggregateRows("regr_intercept", "real64", regr_positive_rows),
                    1.0) && ok;

  ok = ExpectReal64("SBSQL-8F9FD6E0E1B0-regr_intercept_y_x",
                    RunAggregateRows("regr_intercept(y,x)", "real64", regr_negative_rows),
                    10.0) && ok;

  ok = ExpectReal64("SBSQL-794AAFE26F38-regr_r2",
                    RunAggregateRows("regr_r2", "real64", regr_positive_rows),
                    1.0) && ok;

  ok = ExpectReal64("SBSQL-BE43021856AE-regr_r2_y_x",
                    RunAggregateRows("regr_r2(y,x)", "real64", regr_negative_rows),
                    1.0) && ok;

  ok = ExpectReal64("regr_r2_constant_y",
                    RunAggregateRows("sb.aggregate.regr_r2", "real64", regr_constant_y_rows),
                    1.0) && ok;

  ok = ExpectReal64("SBSQL-559DFA580089-regr_slope",
                    RunAggregateRows("regr_slope", "real64", regr_positive_rows),
                    2.0) && ok;

  ok = ExpectReal64("SBSQL-BB7BA14B2666-regr_slope_y_x",
                    RunAggregateRows("regr_slope(y,x)", "real64", regr_negative_rows),
                    -2.0) && ok;

  ok = ExpectReal64("SBSQL-C77EA68C577B-regr_sxx",
                    RunAggregateRows("regr_sxx", "real64", regr_positive_rows),
                    2.0) && ok;

  ok = ExpectReal64("SBSQL-D291129F3FD3-regr_sxx_y_x",
                    RunAggregateRows("regr_sxx(y,x)", "real64", regr_negative_rows),
                    5.0) && ok;

  ok = ExpectReal64("regr_sxx_singleton",
                    RunAggregateRows("sb.aggregate.regr_sxx", "real64", regr_singleton_rows),
                    0.0) && ok;

  ok = ExpectReal64("SBSQL-61641209CF6B-regr_sxy",
                    RunAggregateRows("regr_sxy", "real64", regr_positive_rows),
                    4.0) && ok;

  ok = ExpectReal64("SBSQL-1F514A240E49-regr_sxy_y_x",
                    RunAggregateRows("regr_sxy(y,x)", "real64", regr_negative_rows),
                    -10.0) && ok;

  ok = ExpectReal64("SBSQL-1D81FEFFF22A-regr_syy",
                    RunAggregateRows("regr_syy", "real64", regr_positive_rows),
                    8.0) && ok;

  ok = ExpectReal64("SBSQL-9C9BD835BEAF-regr_syy_y_x",
                    RunAggregateRows("regr_syy(y,x)", "real64", regr_negative_rows),
                    20.0) && ok;

  ok = ExpectNull("regr_slope_singleton",
                  RunAggregateRows("sb.aggregate.regr_slope", "real64", regr_singleton_rows),
                  "real64") && ok;

  ok = ExpectFailure("covar_pop_missing_pair",
                     RunAggregateRows("covar_pop", "real64", {{Int64Value(1)}}),
                     "SB_DIAG_AGGREGATE_CORR_PAIR_REQUIRED") && ok;

  ok = ExpectFailure("regr_count_invalid_input",
                     RunAggregateRows("regr_count", "int64", {{TextValue("not-a-number"), Int64Value(1)}}),
                     "SB_DIAG_AGGREGATE_NUMERIC_INPUT_REQUIRED") && ok;

  ok = ExpectText("json_agg_values",
                  RunAggregate("json_agg", "json",
                               {Int64Value(1), TextValue("two"), NullValue("text")}),
                  R"([1,"two",null])") && ok;

  ok = ExpectText("json_agg_orderby_signature",
                  RunAggregate("json_agg(expr[ORDERBY...])", "json",
                               {TextValue("north"), TextValue("east")}),
                  R"(["north","east"])") && ok;

  ok = ExpectText("json_agg_canonical",
                  RunAggregate("sb.aggregate.json_agg", "json",
                               {TextValue("alpha"), NullValue("text")}),
                  R"(["alpha",null])") && ok;

  ok = ExpectNull("json_agg_empty",
                  RunAggregate("sb.aggregate.json_agg", "json", {}),
                  "json") && ok;

  ok = ExpectList("array_agg_values_include_null",
                  RunAggregate("array_agg", "list<text nullable>",
                               {TextValue("north"), NullValue("text"), TextValue("east")}),
                  "list<text nullable>",
                  "list[text:north;NULL;text:east]") && ok;

  ok = ExpectList("array_agg_orderby_signature",
                  RunAggregate("array_agg(expr[ORDERBY...])", "list<text nullable>",
                               {TextValue("north"), TextValue("east")}),
                  "list<text nullable>",
                  "list[text:north;text:east]") && ok;

  ok = ExpectList("array_agg_canonical",
                  RunAggregate("sb.aggregate.array_agg", "list<int64 nullable>",
                               {Int64Value(7), NullValue("int64"), Int64Value(9)}),
                  "list<int64 nullable>",
                  "list[int64:7;NULL;int64:9]") && ok;

  ok = ExpectNull("array_agg_empty",
                  RunAggregate("sb.aggregate.array_agg", "list<any nullable>", {}),
                  "list<any nullable>") && ok;

  ok = ExpectText("json_object_agg_pairs",
                  RunAggregateRows("json_object_agg(key,value)", "json",
                                   {{TextValue("first"), Int64Value(1)},
                                    {TextValue("second"), TextValue("two")}}),
                  R"({"first":1,"second":"two"})") && ok;

  ok = ExpectText("json_object_agg_duplicate_last_key_wins",
                  RunAggregateRows("sb.aggregate.json_object_agg", "json",
                                   {{TextValue("dup"), Int64Value(1)},
                                    {TextValue("other"), NullValue("int64")},
                                    {TextValue("dup"), Int64Value(2)}}),
                  R"({"other":null,"dup":2})") && ok;

  ok = ExpectFailure("json_object_agg_null_key",
                     RunAggregateRows("json_object_agg", "json",
                                      {{NullValue("text"), TextValue("value")}}),
                     "SB_DIAG_AGGREGATE_JSON_OBJECT_KEY_REQUIRED") && ok;

  const std::vector<SblrValue> window_values = {
      TextValue("north"),
      TextValue("east"),
      TextValue("south"),
      TextValue("west"),
      TextValue("zenith"),
  };
  const std::vector<SblrValue> peer_rank_values = {
      Int64Value(10),
      Int64Value(20),
      Int64Value(20),
      Int64Value(30),
      Int64Value(40),
  };
  const std::vector<std::uint64_t> peer_groups = {1, 2, 2, 3, 4};
  const SblrValue hypothetical_25 = Int64Value(25);
  const SblrValue navigation_default = TextValue("fallback");

  ok = ExpectInt64("SBSQL-E33776097240-window-rank-peer",
                   RunWindow("sb.window.rank", peer_rank_values, 2, 0, peer_rank_values.size(),
                             1, 1, 1, nullptr, peer_groups),
                   2) && ok;
  ok = ExpectInt64("SBSQL-73159B932B38-window-rank-canonical-alias",
                   RunWindow("sb.window.rank", peer_rank_values, 2, 0, peer_rank_values.size(),
                             1, 1, 1, nullptr, peer_groups),
                   2) && ok;

  ok = ExpectInt64("SBSQL-E1BCEE3D98B7-window-dense-rank-peer",
                   RunWindow("sb.window.dense_rank", peer_rank_values, 2, 0, peer_rank_values.size(),
                             1, 1, 1, nullptr, peer_groups),
                   2) && ok;
  ok = ExpectInt64("SBSQL-E7B5D653D886-window-dense-rank-canonical-alias",
                   RunWindow("sb.window.dense_rank", peer_rank_values, 2, 0, peer_rank_values.size(),
                             1, 1, 1, nullptr, peer_groups),
                   2) && ok;

  ok = ExpectReal64("SBSQL-513700E3598C-window-percent-rank-peer",
                    RunWindow("sb.window.percent_rank", peer_rank_values, 2, 0, peer_rank_values.size(),
                              1, 1, 1, nullptr, peer_groups),
                    0.25) && ok;
  ok = ExpectReal64("SBSQL-8F46078CCAA2-window-percent-rank-canonical-alias",
                    RunWindow("sb.window.percent_rank", peer_rank_values, 2, 0, peer_rank_values.size(),
                              1, 1, 1, nullptr, peer_groups),
                    0.25) && ok;

  ok = ExpectReal64("SBSQL-F959FD740DD3-window-cume-dist-peer",
                    RunWindow("sb.window.cume_dist", peer_rank_values, 2, 0, peer_rank_values.size(),
                              1, 1, 1, nullptr, peer_groups),
                    0.6) && ok;
  ok = ExpectReal64("SBSQL-3A4D165FF59E-window-cume-dist-canonical-alias",
                    RunWindow("sb.window.cume_dist", peer_rank_values, 2, 0, peer_rank_values.size(),
                              1, 1, 1, nullptr, peer_groups),
                    0.6) && ok;

  ok = ExpectInt64("SBSQL-405DA76744CA-ordered-rank",
                   RunWindow("sb.aggregate.rank", peer_rank_values, 0, 0, peer_rank_values.size(),
                             1, 1, 1, &hypothetical_25),
                   4) && ok;

  ok = ExpectInt64("SBSQL-D9E3FA320510-ordered-dense-rank",
                   RunWindow("sb.aggregate.dense_rank", peer_rank_values, 0, 0, peer_rank_values.size(),
                             1, 1, 1, &hypothetical_25),
                   3) && ok;

  ok = ExpectReal64("SBSQL-443C68E68D9F-ordered-percent-rank",
                    RunWindow("sb.aggregate.percent_rank", peer_rank_values, 0, 0, peer_rank_values.size(),
                              1, 1, 1, &hypothetical_25),
                    0.6) && ok;

  ok = ExpectReal64("SBSQL-63BB74EAD479-ordered-cume-dist",
                    RunWindow("sb.aggregate.cume_dist", peer_rank_values, 0, 0, peer_rank_values.size(),
                              1, 1, 1, &hypothetical_25),
                    2.0 / 3.0) && ok;

  ok = ExpectInt64("SBSQL-6F988BD1E2E0-ordered-rank-within-group",
                   RunWindow("rank(expr)WITHINGROUP(ORDERBYexpr)", peer_rank_values, 0, 0, peer_rank_values.size(),
                             1, 1, 1, &hypothetical_25),
                   4) && ok;

  ok = ExpectInt64("SBSQL-7B0D1EA07215-ordered-dense-rank-within-group",
                   RunWindow("dense_rank(expr)WITHINGROUP(ORDERBYexpr)", peer_rank_values, 0, 0, peer_rank_values.size(),
                             1, 1, 1, &hypothetical_25),
                   3) && ok;

  ok = ExpectReal64("SBSQL-374E6DE31900-ordered-percent-rank-within-group",
                    RunWindow("percent_rank(expr)WITHINGROUP(ORDERBYexpr)", peer_rank_values, 0, 0, peer_rank_values.size(),
                              1, 1, 1, &hypothetical_25),
                    0.6) && ok;

  ok = ExpectReal64("SBSQL-70B39E494FED-ordered-cume-dist-within-group",
                    RunWindow("cume_dist(expr)WITHINGROUP(ORDERBYexpr)", peer_rank_values, 0, 0, peer_rank_values.size(),
                              1, 1, 1, &hypothetical_25),
                    2.0 / 3.0) && ok;

  ok = ExpectInt64("SBSQL-28B6483D8641-row_number",
                   RunWindow("row_number", window_values, 2, 0, window_values.size()),
                   3) && ok;

  ok = ExpectInt64("SBSQL-7A6AFA548A76-row_number-over-lowered",
                   RunWindow("sb.window.row_number", window_values, 3, 0, window_values.size()),
                   4) && ok;

  ok = ExpectInt64("SBSQL-BAF3A91528AA-sb-window-row-number",
                   RunWindow("sb.window.row_number", window_values, 4, 0, window_values.size()),
                   5) && ok;

  ok = ExpectText("SBSQL-C02257DB2BE3-lag",
                  RunWindow("lag", window_values, 3, 0, window_values.size(), 2),
                  "east") && ok;

  ok = ExpectText("SBSQL-35A1ECA35D13-sb-window-lag",
                  RunWindow("sb.window.lag", window_values, 0, 0, window_values.size(), 1, 1, 1, &navigation_default),
                  "fallback") && ok;

  ok = ExpectText("SBSQL-0F7E089AB839-lag-over-lowered",
                  RunWindow("sb.window.lag", window_values, 2, 0, window_values.size(), 1),
                  "east") && ok;

  ok = ExpectText("SBSQL-CD90EEAF7468-lead",
                  RunWindow("lead", window_values, 1, 0, window_values.size(), 2),
                  "west") && ok;

  ok = ExpectText("SBSQL-F14938CD9CF3-sb-window-lead",
                  RunWindow("sb.window.lead", window_values, 4, 0, window_values.size(), 1, 1, 1, &navigation_default),
                  "fallback") && ok;

  ok = ExpectText("SBSQL-F7B4F498213C-lead-over-lowered",
                  RunWindow("sb.window.lead", window_values, 0, 0, window_values.size(), 1),
                  "east") && ok;

  ok = ExpectText("SBSQL-842F61769B34-first-value",
                  RunWindow("first_value", window_values, 3, 1, 4),
                  "east") && ok;

  ok = ExpectText("SBSQL-BDDEB821D132-sb-window-first-value",
                  RunWindow("sb.window.first_value", window_values, 2, 0, window_values.size()),
                  "north") && ok;

  ok = ExpectText("SBSQL-AA6AE730A722-first-value-over-lowered",
                  RunWindow("sb.window.first_value", window_values, 4, 2, window_values.size()),
                  "south") && ok;

  ok = ExpectInt64("SBSQL-E52C3FB97F6C-ntile",
                   RunWindow("ntile",
                             {TextValue("r1"), TextValue("r2"), TextValue("r3"), TextValue("r4"), TextValue("r5"),
                              TextValue("r6"), TextValue("r7"), TextValue("r8"), TextValue("r9"), TextValue("r10")},
                             5, 0, 10, 1, 4),
                   2) && ok;

  ok = ExpectInt64("SBSQL-6412E60ED18E-sb-window-ntile",
                   RunWindow("sb.window.ntile", {TextValue("solo")}, 0, 0, 1, 1, 4),
                   1) && ok;

  ok = ExpectInt64("SBSQL-1EF274EAE8DC-ntile-over-lowered",
                   RunWindow("sb.window.ntile", window_values, 4, 0, window_values.size(), 1, 2),
                   2) && ok;

  ok = ExpectFailure("window_ntile_zero_bucket",
                     RunWindow("ntile", window_values, 0, 0, window_values.size(), 1, 0),
                     "SBLR.PLAN_TREE.INVALID_HANDLE") && ok;

  ok = ExpectText("SBSQL-2D40C15A4E0A-last-value",
                  RunWindow("last_value", window_values, 2, 1, 4),
                  "west") && ok;

  ok = ExpectNull("SBSQL-23AF50D41FEC-sb-window-last-value-all-null",
                  RunWindow("sb.window.last_value", {NullValue("text"), NullValue("text")}, 1, 0, 2),
                  "text") && ok;

  ok = ExpectText("SBSQL-804D99407A3B-last-value-over-lowered",
                  RunWindow("sb.window.last_value", {TextValue("solo")}, 0, 0, 1),
                  "solo") && ok;

  ok = ExpectNull("window_last_value_empty_frame",
                  RunWindow("last_value", window_values, 1, 1, 1),
                  "text") && ok;

  ok = ExpectText("SBSQL-ED86D05F9232-nth-value",
                  RunWindow("nth_value", window_values, 2, 1, window_values.size(), 1, 1, 3),
                  "west") && ok;

  ok = ExpectText("SBSQL-4BC628E8AD6C-sb-window-nth-value",
                  RunWindow("sb.window.nth_value", {TextValue("solo")}, 0, 0, 1, 1, 1, 1),
                  "solo") && ok;

  ok = ExpectText("SBSQL-C97299B0256C-nth-value-over-lowered",
                  RunWindow("sb.window.nth_value", window_values, 3, 0, window_values.size(), 1, 1, 2),
                  "east") && ok;

  ok = ExpectNull("window_nth_value_out_of_frame",
                  RunWindow("nth_value", {TextValue("north"), TextValue("east"), TextValue("south")}, 1, 1, 3, 1, 1, 5),
                  "text") && ok;

  ok = ExpectNull("window_nth_value_all_null",
                  RunWindow("sb.window.nth_value", {NullValue("text"), NullValue("text")}, 1, 0, 2, 1, 1, 2),
                  "text") && ok;

  ok = ExpectInt64("window_row_number",
                   RunWindow("sb.window.row_number", window_values, 2, 0, window_values.size()),
                   3) && ok;

  ok = ExpectInt64("window_ntile",
                   RunWindow("sb.window.ntile", window_values, 3, 0, window_values.size(), 1, 3),
                   2) && ok;

  ok = ExpectText("window_lag_offset",
                  RunWindow("sb.window.lag", window_values, 3, 0, window_values.size(), 2),
                  "east") && ok;

  ok = ExpectText("window_lag_default",
                  RunWindow("sb.window.lag", window_values, 0, 0, window_values.size(), 1, 1, 1, &navigation_default),
                  "fallback") && ok;

  ok = ExpectText("window_lead_offset",
                  RunWindow("sb.window.lead", window_values, 1, 0, window_values.size(), 2),
                  "west") && ok;

  ok = ExpectText("window_lead_default",
                  RunWindow("sb.window.lead", window_values, 4, 0, window_values.size(), 1, 1, 1, &navigation_default),
                  "fallback") && ok;

  ok = ExpectText("window_first_value_frame",
                  RunWindow("sb.window.first_value", window_values, 3, 1, 4),
                  "east") && ok;

  ok = ExpectText("window_last_value_frame",
                  RunWindow("sb.window.last_value", window_values, 2, 1, 4),
                  "west") && ok;

  ok = ExpectText("window_nth_value_frame",
                  RunWindow("sb.window.nth_value", window_values, 2, 1, 5, 1, 1, 3),
                  "west") && ok;

  ok = ExpectFailure("window_nth_value_invalid",
                     RunWindow("sb.window.nth_value", window_values, 2, 0, window_values.size(), 1, 1, 0),
                     "QOW-DIAG-WINDOW-NTH") && ok;

  ok = ExpectFailure("percentile_fraction_invalid",
                     RunAggregate("percentile_cont", "real64",
                                  {Int64Value(10)},
                                  &invalid_fraction),
                     "SB_DIAG_AGGREGATE_PERCENTILE_FRACTION_INVALID") && ok;

  ok = ExpectFailure("top_k_limit_invalid",
                     RunAggregate("approx_top_k", "json",
                                  {TextValue("a")},
                                  &top_zero),
                     "SB_DIAG_AGGREGATE_TOP_K_LIMIT_INVALID") && ok;

  if (!ok) return 1;
  std::cout << "sbsql_sbsfc_015_aggregate_window_runtime_conformance=passed\n";
  return 0;
}
