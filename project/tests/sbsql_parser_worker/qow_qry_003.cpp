// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_dispatch.hpp"
#include "sblr_opcode_registry.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <utility>

namespace sblr = scratchbird::engine::sblr;
namespace api = scratchbird::engine::internal_api;

namespace {

bool Require(const bool condition, const std::string_view message) {
  if (!condition) std::cerr << message << '\n';
  return condition;
}

bool HasDispatchDiagnostic(const sblr::SblrDispatchResult& result,
                           const std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

bool HasApiDiagnostic(const sblr::SblrDispatchResult& result,
                      const std::string_view code) {
  for (const auto& diagnostic : result.api_result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

api::EngineRequestContext Context() {
  api::EngineRequestContext context;
  context.security_context_present = true;
  context.statement_uuid.canonical =
      "019f0000-0000-7120-8000-000000000303";
  context.local_transaction_id = 37;
  context.snapshot_visible_through_local_transaction_id = 35;
  context.statement_metadata_snapshot_engine_owned = true;
  context.statement_metadata_snapshot_uuid.canonical =
      "019f0000-0000-7100-8000-000000000303";
  context.authorization_context.present = true;
  context.authorization_context.authority_uuid.canonical =
      "019f0000-0000-7110-8000-000000000304";
  context.catalog_generation_id = 303;
  context.security_epoch = 304;
  context.resource_epoch = 305;
  context.optimizer_capability_snapshot_uuid.canonical =
      "019f0000-0000-7200-8000-000000006001";
  context.optimizer_resource_snapshot_uuid.canonical =
      "019f0000-0000-7200-8000-000000006002";
  context.optimizer_route_snapshot_uuid.canonical =
      "019f0000-0000-7200-8000-000000006003";
  context.optimizer_route_epoch = 306;
  context.optimizer_route_generation = 307;
  context.optimizer_memory_budget_bytes = 64 * 1024 * 1024;
  context.optimizer_maximum_candidate_count = 131072;
  context.optimizer_maximum_memo_groups = 131072;
  context.optimizer_maximum_search_steps = 1048576;
  context.optimizer_maximum_planning_time_ns = 5'000'000'000;
  context.optimizer_spill_allowed = true;
  context.current_monotonic_ns = "303000";
  context.authorization_context.security_epoch = 304;
  context.authorization_context.policy_epoch = 305;
  context.authorization_context.catalog_generation_id = 303;
  return context;
}

sblr::SblrOperationEnvelope QueryEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope(
      "query.execute", "SBLR_QUERY_EXECUTE", "qow.qry.003");
  envelope.result_shape = "query_execute_result";
  envelope.requires_transaction_context = true;
  envelope.operands = {
      {"uint16", "relational_wire_version", "2"},
      {"uuid", "relational_bound_sblr_tree_uuid",
       "019f0000-0000-7000-8000-000000000300"},
      {"uuid", "relational_catalog_epoch_uuid",
       "019f0000-0000-7100-8000-000000000303"},
      {"uuid", "relational_security_context_uuid",
       "019f0000-0000-7110-8000-000000000304"},
      {"uint32", "relational_root_node_id", "1"},
      {"relational_descriptor_v1", "1",
       "019f0000-0000-7200-8000-000000000301|"
       "019f0000-0000-7300-8000-000000000302|1|-|-|-|-|-"},
      {"relational_expression_v1", "1", "1|-|1|-|-|1|-|31"},
      {"relational_output_v1", "1", "1|1|1|1|0|636f6c756d6e5f31"},
      {"relational_values_row_v1", "1", "1"},
      {"relational_node_v1", "1", "13|0|-|1|1"},
      {"relational_node_binding_v1", "1",
       "76616c7565732e6c69746572616c2d7461626c652e7631|1|-|-|-"},
  };
  return envelope;
}

bool ValidateRegistryClosure() {
  const auto* canonical = sblr::LookupSblrOperation("query.execute");
  const auto* legacy = sblr::LookupSblrOperation("query.plan_operation");
  bool passed = true;
  passed &= Require(canonical != nullptr,
                    "canonical query.execute registry row is absent");
  passed &= Require(canonical != nullptr &&
                        canonical->opcode == "SBLR_QUERY_EXECUTE" &&
                        canonical->support ==
                            sblr::SblrOpcodeSupport::implemented &&
                        canonical->requires_transaction_context,
                    "canonical query.execute registry contract differs");
  passed &= Require(legacy != nullptr,
                    "legacy query.plan_operation registry row is absent");
  passed &= Require(legacy != nullptr &&
                        legacy->support ==
                            sblr::SblrOpcodeSupport::deprecated_refusal &&
                        legacy->refusal_diagnostic ==
                            "QOW-DIAG-RELATIONAL-ROOT-NONCANONICAL",
                    "legacy query root is not fail-closed in the registry");
  return passed;
}

bool ValidateCanonicalDispatchSeam() {
  sblr::SblrDispatchRequest request;
  request.context = Context();
  request.envelope = QueryEnvelope();
  const auto result = sblr::DispatchSblrOperation(std::move(request));

  bool passed = true;
  passed &= Require(result.envelope_validated,
                    "canonical query envelope was not validated");
  passed &= Require(result.accepted && result.dispatched_to_api,
                    "validated typed DAG did not reach the API dispatch seam");
  passed &= Require(result.logical_graph_populated &&
                        result.logical_properties_populated &&
                        result.optimizer_admitted &&
                        result.optimizer_admission_stage_count == 8 &&
                        result.logical_node_count == 1 &&
                        result.logical_property_count == 0,
                    "typed DAG did not populate the canonical logical graph");
  passed &= Require(
      result.optimizer_selected && result.physical_dag_published &&
          result.physical_dag_executed && result.runtime_actuals_attached &&
          result.canonical_result_published && result.api_result.ok &&
          result.physical_node_count == 1 &&
          result.canonical_result_column_count == 1 &&
          result.canonical_result_row_count == 1 &&
          result.api_result.result_shape.rows.size() == 1 &&
          result.api_result.result_shape.rows[0].fields.size() == 1 &&
          result.api_result.result_shape.rows[0].fields[0].second.encoded_value ==
              "1" &&
          result.diagnostics.empty(),
      "canonical literal VALUES did not traverse the physical/result spine");
  return passed;
}

bool ValidateDagAndTransportRefusal() {
  auto dangling = QueryEnvelope();
  dangling.operands[9].value = "3|0|99|1|-";
  auto dangling_result = sblr::DispatchSblrOperation(
      {Context(), std::move(dangling), {}});

  auto unknown = QueryEnvelope();
  unknown.operands.push_back({"text", "silent_default", "forbidden"});
  auto unknown_result = sblr::DispatchSblrOperation(
      {Context(), std::move(unknown), {}});

  auto malformed_expression = QueryEnvelope();
  malformed_expression.operands[6].value = "1|-|1|-|-|1|-|not_hex";
  auto malformed_expression_result = sblr::DispatchSblrOperation(
      {Context(), std::move(malformed_expression), {}});

  auto wrong_shape = QueryEnvelope();
  wrong_shape.result_shape = "engine.api.result.v1";
  auto shape_result = sblr::DispatchSblrOperation(
      {Context(), std::move(wrong_shape), {}});

  auto side_channel = QueryEnvelope();
  api::EngineApiRequest generic_payload;
  generic_payload.option_envelopes.push_back("query_operation:scan");
  auto side_channel_result = sblr::DispatchSblrOperation(
      {Context(), std::move(side_channel), std::move(generic_payload)});

  bool passed = true;
  passed &= Require(!dangling_result.accepted &&
                        !dangling_result.dispatched_to_api,
                    "dangling typed DAG reached physical dispatch");
  passed &= Require(HasApiDiagnostic(
                        dangling_result, "SBLR.PLAN_TREE.INVALID_HANDLE"),
                    "dangling DAG diagnostic differs");
  passed &= Require(!unknown_result.accepted &&
                        !unknown_result.dispatched_to_api,
                    "unknown query.execute operand was silently accepted");
  passed &= Require(HasApiDiagnostic(
                        unknown_result, "SBLR.PLAN_TREE.INVALID_HANDLE"),
                    "unknown-operand diagnostic differs");
  passed &= Require(
      !malformed_expression_result.accepted &&
          HasApiDiagnostic(malformed_expression_result,
                           "SBLR.PLAN_TREE.INVALID_HANDLE"),
      "malformed typed scalar record was accepted");
  passed &= Require(
      !shape_result.accepted &&
          HasApiDiagnostic(shape_result,
                           "SB_DIAG_SBLR_RESULT_SHAPE_MISMATCH"),
      "noncanonical query result shape was accepted");
  passed &= Require(
      !side_channel_result.accepted &&
          HasApiDiagnostic(side_channel_result,
                           "SBLR.PLAN_TREE.INVALID_HANDLE"),
      "out-of-band flat API payload bypassed the typed DAG route");
  return passed;
}

bool ValidateRegistryAndAuthorityRefusal() {
  auto mismatch = QueryEnvelope();
  mismatch.opcode = "SBLR_QUERY_PLAN_OPERATION";
  const auto mismatch_result = sblr::DispatchSblrOperation(
      {Context(), std::move(mismatch), {}});

  auto missing_declaration = QueryEnvelope();
  missing_declaration.requires_transaction_context = false;
  const auto declaration_result = sblr::DispatchSblrOperation(
      {Context(), std::move(missing_declaration), {}});

  auto missing_authority = QueryEnvelope();
  auto context = Context();
  context.local_transaction_id = 0;
  const auto authority_result = sblr::DispatchSblrOperation(
      {std::move(context), std::move(missing_authority), {}});

  auto legacy = sblr::MakeSblrEnvelope(
      "query.plan_operation", "SBLR_QUERY_PLAN_OPERATION", "qow.legacy");
  legacy.operands.push_back({"uint16", "legacy_wire_version", "1"});
  const auto legacy_result = sblr::DispatchSblrOperation(
      {Context(), std::move(legacy), {}});

  bool passed = true;
  passed &= Require(
      HasDispatchDiagnostic(mismatch_result,
                            "SB_DIAG_SBLR_OPCODE_MISMATCH"),
      "registry opcode mismatch did not reach dispatch diagnostics");
  passed &= Require(
      HasDispatchDiagnostic(declaration_result,
                            "SB_DIAG_SBLR_TRANSACTION_CONTEXT_REQUIRED"),
      "missing MGA-context declaration was not refused by the registry");
  passed &= Require(
      HasDispatchDiagnostic(authority_result,
                            "SB_SBLR_DISPATCH_TRANSACTION_CONTEXT_REQUIRED"),
      "missing engine MGA context was not refused by dispatch");
  passed &= Require(
      HasDispatchDiagnostic(legacy_result,
                            "QOW-DIAG-RELATIONAL-ROOT-NONCANONICAL") &&
          !legacy_result.accepted && !legacy_result.dispatched_to_api,
      "legacy relational root did not fail closed before execution");
  return passed;
}

bool ValidateDecodePath() {
  const auto encoded = sblr::EncodeSblrEnvelope(QueryEnvelope());
  const auto result =
      sblr::DecodeAndDispatchSblrOperation(encoded, Context());
  bool passed = true;
  passed &= Require(result.envelope_validated && result.accepted &&
                        result.dispatched_to_api,
                    "decoded canonical query did not reach the typed seam");
  passed &= Require(result.optimizer_selected &&
                        result.physical_dag_published &&
                        result.physical_dag_executed &&
                        result.runtime_actuals_attached &&
                        result.canonical_result_published &&
                        result.api_result.ok &&
                        result.canonical_result_row_count == 1,
                    "decoded canonical VALUES query did not complete the live spine");
  return passed;
}

}  // namespace

// QOW-ROUTE-STAGE-QRY-003-V1
// QOW-TEST-QRY-003-V1
int main() {
  bool passed = true;
  passed &= ValidateRegistryClosure();
  passed &= ValidateCanonicalDispatchSeam();
  passed &= ValidateDagAndTransportRefusal();
  passed &= ValidateRegistryAndAuthorityRefusal();
  passed &= ValidateDecodePath();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
