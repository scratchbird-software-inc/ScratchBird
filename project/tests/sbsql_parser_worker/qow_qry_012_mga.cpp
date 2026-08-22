// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "descriptor_value_runtime.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace exec = scratchbird::engine::executor;
namespace api = scratchbird::engine::internal_api;

namespace {

constexpr std::uint64_t kOwnerLocalTransactionId =
    0xffff'ffff'ffff'ff00ULL;
constexpr std::uint64_t kOldestActiveLocalTransactionId =
    0xffff'ffff'ffff'fee8ULL;
constexpr std::uint64_t kRetentionHorizonLocalTransactionId =
    0xffff'ffff'ffff'fed0ULL;
constexpr std::uint64_t kInDoubtLocalTransactionId =
    0xffff'ffff'ffff'fef0ULL;
constexpr std::uint64_t kInventoryNextLocalTransactionId =
    0xffff'ffff'ffff'fff0ULL;

bool Require(const bool condition, const std::string_view detail) {
  if (!condition) {
    std::cerr << "QOW-TEST-QRY-012-MGA-V1: " << detail << '\n';
  }
  return condition;
}

api::EngineDescriptor Descriptor(const std::string& descriptor_uuid,
                                 const std::string& type_uuid) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = descriptor_uuid;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "int64";
  descriptor.encoded_descriptor =
      "type_uuid=" + type_uuid + ";nullability=non_null";
  return descriptor;
}

api::EngineTypedValue Value(const api::EngineDescriptor& descriptor,
                            const std::string& encoded) {
  api::EngineTypedValue value;
  value.descriptor = descriptor;
  value.encoded_value = encoded;
  value.state = api::EngineValueState::value;
  return value;
}

exec::PhysicalMgaStatementContext JoinMgaContext() {
  return {
      "019f0000-0000-7200-8000-000000002431",
      "019f0000-0000-7200-8000-000000002432",
      "019f0000-0000-7200-8000-000000002414",
      "019f0000-0000-7200-8000-000000002433",
      kOwnerLocalTransactionId,
      0,
      kOldestActiveLocalTransactionId,
      kRetentionHorizonLocalTransactionId,
      kRetentionHorizonLocalTransactionId,
      kRetentionHorizonLocalTransactionId,
      {kOldestActiveLocalTransactionId, kOwnerLocalTransactionId},
      {kInDoubtLocalTransactionId},
      "statement_stable",
      kInventoryNextLocalTransactionId,
      true,
      true,
      true,
  };
}

exec::CanonicalExecutionMgaAuthority JoinMgaAuthority(
    const exec::PhysicalMgaStatementContext& context = JoinMgaContext()) {
  exec::CanonicalExecutionMgaAuthority authority;
  authority.statement_context = context;
  authority.origin = exec::CanonicalMgaAuthorityOrigin::kClosureTestSeam;
  const auto current = authority.statement_context;
  authority.resolve_current = [current] {
    exec::CanonicalMgaCurrentResolution resolution;
    resolution.statement_context = current;
    return resolution;
  };
  return authority;
}

void SetStatementContext(
    exec::TypedPhysicalNodeDag* dag,
    const exec::PhysicalMgaStatementContext& context) {
  dag->mga_statement_context = context;
  for (auto& node : dag->nodes) node.mga_statement_context = context;
}

exec::CanonicalExecutionMgaAuthority BindPhysicalAbiV2(
    exec::TypedPhysicalNodeDag* dag,
    const exec::PhysicalMgaStatementContext& context = JoinMgaContext()) {
  dag->abi_version = 2;
  dag->local_transaction_id = context.owning_local_transaction_id;
  dag->statement_snapshot_id = 0;
  dag->bound_sblr_tree_uuid = dag->admission_evidence.at(0).evidence_uuid;
  dag->catalog_epoch_uuid = dag->admission_evidence.at(1).evidence_uuid;
  dag->security_context_uuid = dag->admission_evidence.at(2).evidence_uuid;
  dag->capability_snapshot_uuid = dag->admission_evidence.at(4).evidence_uuid;
  dag->resource_snapshot_uuid = dag->admission_evidence.at(5).evidence_uuid;
  dag->statistics_snapshot_uuid = dag->admission_evidence.at(6).evidence_uuid;
  dag->route_snapshot_uuid = dag->admission_evidence.at(7).evidence_uuid;
  dag->catalog_generation = 1;
  dag->security_epoch = 1;
  dag->policy_epoch = 1;
  dag->resource_epoch = 1;
  dag->statistics_generation = 1;
  dag->route_epoch = 1;
  dag->route_generation = 1;
  dag->memory_budget_bytes = 32ULL * 1024ULL * 1024ULL;
  dag->optimizer_published = true;
  dag->immutable_node_identity_validated = true;
  dag->capability_validated_before_access = true;
  SetStatementContext(dag, context);
  for (auto& node : dag->nodes) {
    node.selected_alternative_uuid =
        "019f0000-0000-7200-8000-00000000e554";
    node.executor_capability_uuid =
        "019f0000-0000-7200-8000-00000000e555";
    node.executor_capability_abi_version = 1;
    node.cost_vector_uuid =
        "019f0000-0000-7200-8000-00000000e556";
    node.memory_bytes_required = 32ULL * 1024ULL * 1024ULL;
    node.engine_capability_validated = true;
  }
  return JoinMgaAuthority(context);
}

void BindJoinContext(exec::CanonicalJoinMgaRequest* request,
                     const exec::PhysicalMgaStatementContext& context) {
  auto& dag = request->strategy_request.residual_request.key_request
                  .physical_dag;
  SetStatementContext(&dag, context);
  dag.local_transaction_id = context.owning_local_transaction_id;
  dag.statement_snapshot_id = context.visible_committed_high_watermark;
  request->mga_authority = JoinMgaAuthority(context);
  request->strategy_request.residual_request.key_request.mga_authority =
      request->mga_authority;
}

bool EmptyJoinFailure(const exec::CanonicalJoinMgaResult& result) {
  return !result.diagnostic.ok && !result.mga_boundary_proven &&
         result.output_batch.columns.empty() &&
         result.output_batch.rows.empty() &&
         result.candidate_pair_count == 0 &&
         result.visible_pair_count == 0 && result.selected_plan_uuid.empty() &&
         result.executed_physical_node_id == 0 &&
         !exec::PhysicalMgaStatementContextValid(
             result.mga_statement_context);
}

exec::CanonicalJoinMgaRequest Request() {
  const auto left_key = Descriptor(
      "019f0000-0000-7200-8000-000000002401",
      "019f0000-0000-7300-8000-000000002402");
  const auto left_payload = Descriptor(
      "019f0000-0000-7200-8000-000000002403",
      "019f0000-0000-7300-8000-000000002404");
  const auto right_key = Descriptor(
      "019f0000-0000-7200-8000-000000002405",
      "019f0000-0000-7300-8000-000000002406");
  const auto right_payload = Descriptor(
      "019f0000-0000-7200-8000-000000002407",
      "019f0000-0000-7300-8000-000000002408");

  exec::CanonicalJoinMgaRequest request;
  auto& residual = request.strategy_request.residual_request;
  auto& key = residual.key_request;
  key.physical_dag.selected_plan_uuid =
      "019f0000-0000-7200-8000-000000002409";
  key.physical_dag.root_physical_node_id = 2403;
  key.physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000002411"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000002412"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000002413"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000002414"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000002415"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000002416"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000002417"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000002418"},
  };
  key.physical_dag.nodes = {
      {.physical_node_id = 2401,
       .relational_node_id = 2401,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.left.typed.v1",
       .output_descriptor_ids = {2401, 2402},
       .causal_counter_id = 24001},
      {.physical_node_id = 2402,
       .relational_node_id = 2402,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.right.typed.v1",
       .output_descriptor_ids = {2403, 2404},
       .causal_counter_id = 24002},
      {.physical_node_id = 2403,
       .relational_node_id = 2403,
       .node_kind = exec::PhysicalNodeKind::kJoin,
       .implementation_id = "join.hash-inner.int64-equality.v1",
       .input_physical_node_ids = {2401, 2402},
       .output_descriptor_ids = {2401, 2402, 2403, 2404},
       .causal_counter_id = 24003},
  };
  key.selected_physical_node_id = 2403;
  key.left_batch = exec::MakeDescriptorBatch(
      {{"left_key", left_key, false, 2401},
       {"left_payload", left_payload, false, 2402}},
      {{{Value(left_key, "1"), Value(left_payload, "10")}},
       {{Value(left_key, "01"), Value(left_payload, "11")}},
       {{Value(left_key, "2"), Value(left_payload, "12")}}});
  key.right_batch = exec::MakeDescriptorBatch(
      {{"right_key", right_key, false, 2403},
       {"right_payload", right_payload, false, 2404}},
      {{{Value(right_key, "1"), Value(right_payload, "20")}},
       {{Value(right_key, "2"), Value(right_payload, "21")}},
       {{Value(right_key, "01"), Value(right_payload, "22")}}});
  key.key_terms = {
      {.left_column = 0,
       .left_expression_descriptor_id = 2401,
       .right_column = 0,
       .right_expression_descriptor_id = 2403},
  };
  using Truth = api::EngineSqlTruthValue;
  residual.residual_truth_values = {
      Truth::true_value,  Truth::true_value,  Truth::false_value,
      Truth::false_value, Truth::true_value,  Truth::true_value,
      Truth::true_value,  Truth::true_value,  Truth::true_value,
  };

  request.transaction_inventory_id = kInventoryNextLocalTransactionId;
  request.mga_authority = BindPhysicalAbiV2(&key.physical_dag);
  key.mga_authority = request.mga_authority;
  request.transaction_inventory_evidence_uuid =
      "019f0000-0000-7200-8000-000000002419";
  request.candidate_evidence = {
      {.pair_index = 0,
       .left_creator_local_transaction_id = kOwnerLocalTransactionId,
       .right_creator_local_transaction_id = kOwnerLocalTransactionId,
       .left_row_version_id = 24101,
       .right_row_version_id = 24201,
       .left_visibility = exec::CanonicalMgaVisibilityDecision::kVisible,
       .right_visibility = exec::CanonicalMgaVisibilityDecision::kVisible,
       .security_decision = exec::CanonicalMgaSecurityDecision::kAllowed,
       .index_candidate_generation = 7,
       .current_index_generation = 7,
       .exact_key_recheck = Truth::true_value,
       .engine_evidence_uuid =
           "019f0000-0000-7200-8000-000000002421"},
      {.pair_index = 5,
       .left_creator_local_transaction_id = kOwnerLocalTransactionId,
       .right_creator_local_transaction_id = kOwnerLocalTransactionId,
       .left_row_version_id = 24102,
       .right_row_version_id = 24202,
       .left_visibility = exec::CanonicalMgaVisibilityDecision::kInvisible,
       .right_visibility = exec::CanonicalMgaVisibilityDecision::kVisible,
       .security_decision = exec::CanonicalMgaSecurityDecision::kAllowed,
       .index_candidate_generation = 7,
       .current_index_generation = 7,
       .exact_key_recheck = Truth::true_value,
       .engine_evidence_uuid =
           "019f0000-0000-7200-8000-000000002422"},
      {.pair_index = 7,
       .left_creator_local_transaction_id = kOwnerLocalTransactionId,
       .right_creator_local_transaction_id = kOwnerLocalTransactionId,
       .left_row_version_id = 24103,
       .right_row_version_id = 24203,
       .left_visibility = exec::CanonicalMgaVisibilityDecision::kVisible,
       .right_visibility = exec::CanonicalMgaVisibilityDecision::kVisible,
       .security_decision = exec::CanonicalMgaSecurityDecision::kDenied,
       .index_candidate_generation = 7,
       .current_index_generation = 7,
       .exact_key_recheck = Truth::true_value,
       .engine_evidence_uuid =
           "019f0000-0000-7200-8000-000000002423"},
  };
  return request;
}

exec::CanonicalJoinMgaInputRowEvidence RowEvidence(
    const std::size_t row_index, const std::uint64_t row_version_id,
    const std::string& evidence_uuid) {
  exec::CanonicalJoinMgaInputRowEvidence evidence;
  evidence.row_index = row_index;
  evidence.creator_local_transaction_id = kOwnerLocalTransactionId;
  evidence.row_version_id = row_version_id;
  evidence.visibility = exec::CanonicalMgaVisibilityDecision::kVisible;
  evidence.security_decision = exec::CanonicalMgaSecurityDecision::kAllowed;
  evidence.candidate_generation = 7;
  evidence.current_generation = 7;
  evidence.engine_evidence_uuid = evidence_uuid;
  return evidence;
}

exec::CanonicalJoinMgaRequest InputEvidenceRequest(
    const exec::CanonicalAcceptedJoinKind kind) {
  auto request = Request();
  request.input_row_evidence_profile = true;
  request.left_row_evidence = {
      RowEvidence(0, 24101, "019f0000-0000-7200-8000-000000002431"),
      RowEvidence(1, 24102, "019f0000-0000-7200-8000-000000002432"),
      RowEvidence(2, 24103, "019f0000-0000-7200-8000-000000002433"),
  };
  request.right_row_evidence = {
      RowEvidence(0, 24201, "019f0000-0000-7200-8000-000000002434"),
      RowEvidence(1, 24202, "019f0000-0000-7200-8000-000000002435"),
      RowEvidence(2, 24203, "019f0000-0000-7200-8000-000000002436"),
  };
  request.candidate_evidence[0].left_row_version_id = 24101;
  request.candidate_evidence[0].right_row_version_id = 24201;
  request.candidate_evidence[0].left_visibility =
      exec::CanonicalMgaVisibilityDecision::kVisible;
  request.candidate_evidence[0].security_decision =
      exec::CanonicalMgaSecurityDecision::kAllowed;
  request.candidate_evidence[1].left_row_version_id = 24102;
  request.candidate_evidence[1].right_row_version_id = 24203;
  request.candidate_evidence[1].left_visibility =
      exec::CanonicalMgaVisibilityDecision::kVisible;
  request.candidate_evidence[1].security_decision =
      exec::CanonicalMgaSecurityDecision::kAllowed;
  request.candidate_evidence[2].left_row_version_id = 24103;
  request.candidate_evidence[2].right_row_version_id = 24202;
  request.candidate_evidence[2].security_decision =
      exec::CanonicalMgaSecurityDecision::kAllowed;
  request.strategy_request.join_kind = kind;
  auto& implementation = request.strategy_request.residual_request.key_request
                             .physical_dag.nodes.back().implementation_id;
  switch (kind) {
    case exec::CanonicalAcceptedJoinKind::kInner:
      break;
    case exec::CanonicalAcceptedJoinKind::kLeftOuter:
      implementation = "join.hash-left-outer.int64-equality.v1";
      break;
    case exec::CanonicalAcceptedJoinKind::kRightOuter:
      implementation = "join.hash-right-outer.int64-equality.v1";
      break;
    case exec::CanonicalAcceptedJoinKind::kFullOuter:
      implementation = "join.hash-full-outer.int64-equality.v1";
      break;
    case exec::CanonicalAcceptedJoinKind::kLeftSemi:
      implementation = "join.hash-left-semi.int64-equality.v1";
      break;
    case exec::CanonicalAcceptedJoinKind::kLeftAnti:
      implementation = "join.hash-left-anti.int64-equality.v1";
      break;
    case exec::CanonicalAcceptedJoinKind::kCross:
      break;
  }
  return request;
}

void SelectTypedCompositeProfile(exec::CanonicalJoinMgaRequest* request) {
  if (request == nullptr) return;
  auto& strategy = request->strategy_request;
  auto& key = strategy.residual_request.key_request;
  auto left_real = key.left_batch.columns[0].descriptor;
  auto right_real = key.right_batch.columns[0].descriptor;
  auto left_boolean = key.left_batch.columns[1].descriptor;
  auto right_boolean = key.right_batch.columns[1].descriptor;
  left_real.canonical_type_name = "real64";
  right_real.canonical_type_name = "real64";
  left_boolean.canonical_type_name = "boolean";
  right_boolean.canonical_type_name = "boolean";
  key.left_batch.columns[0].descriptor = left_real;
  key.right_batch.columns[0].descriptor = right_real;
  key.left_batch.columns[1].descriptor = left_boolean;
  key.right_batch.columns[1].descriptor = right_boolean;
  const std::vector<std::string> left_real_values = {"1.5", "1.50", "2.5"};
  const std::vector<std::string> right_real_values = {"1.50", "2.5", "1.5"};
  const std::vector<std::string> left_boolean_values = {"true", "false",
                                                        "true"};
  const std::vector<std::string> right_boolean_values = {"true", "true",
                                                         "false"};
  for (std::size_t row = 0; row < key.left_batch.rows.size(); ++row) {
    key.left_batch.rows[row].values[0] =
        Value(left_real, left_real_values[row]);
    key.left_batch.rows[row].values[1] =
        Value(left_boolean, left_boolean_values[row]);
  }
  for (std::size_t row = 0; row < key.right_batch.rows.size(); ++row) {
    key.right_batch.rows[row].values[0] =
        Value(right_real, right_real_values[row]);
    key.right_batch.rows[row].values[1] =
        Value(right_boolean, right_boolean_values[row]);
  }
  key.key_terms.push_back(
      {.left_column = 1,
       .left_expression_descriptor_id = 2402,
       .right_column = 1,
       .right_expression_descriptor_id = 2404});
  strategy.strategy =
      exec::CanonicalJoinStrategyKind::kHashTypedCompositeEquality;
  const auto kind = strategy.join_kind;
  std::string_view join_kind = "inner";
  if (kind == exec::CanonicalAcceptedJoinKind::kLeftOuter) {
    join_kind = "left-outer";
  }
  key.physical_dag.nodes.back().implementation_id =
      "join.hash-" + std::string(join_kind) +
      ".typed-composite-equality.v1";
}

void RemovePairFive(exec::CanonicalJoinMgaRequest* request) {
  request->candidate_evidence.erase(request->candidate_evidence.begin() + 1);
}

bool ValidateJoinCreatorExclusions() {
  bool passed = true;
  auto excluded = JoinMgaContext();
  excluded.oldest_active_transaction_id = 2401;
  excluded.oldest_interesting_transaction_id = 2401;
  excluded.oldest_snapshot_transaction_id = 2401;
  excluded.retention_horizon_transaction_id = 2401;
  excluded.active_excluded_local_transaction_ids = {
      2401, kOwnerLocalTransactionId};
  excluded.in_doubt_excluded_local_transaction_ids = {2402};

  auto request = Request();
  BindJoinContext(&request, excluded);
  request.candidate_evidence[0].left_creator_local_transaction_id = 2401;
  auto result = exec::ExecuteCanonicalJoinMgaBoundary(request);
  passed &= Require(
      EmptyJoinFailure(result),
      "visible pair creator bypassed the captured active exclusion");

  request = Request();
  BindJoinContext(&request, excluded);
  request.candidate_evidence[0].right_creator_local_transaction_id = 2402;
  result = exec::ExecuteCanonicalJoinMgaBoundary(request);
  passed &= Require(
      EmptyJoinFailure(result),
      "visible pair creator bypassed the captured in-doubt exclusion");

  request = InputEvidenceRequest(exec::CanonicalAcceptedJoinKind::kLeftOuter);
  BindJoinContext(&request, excluded);
  request.left_row_evidence[0].creator_local_transaction_id = 2401;
  result = exec::ExecuteCanonicalJoinMgaBoundary(request);
  passed &= Require(
      EmptyJoinFailure(result),
      "visible input-row creator bypassed the captured active exclusion");

  request = InputEvidenceRequest(exec::CanonicalAcceptedJoinKind::kLeftOuter);
  BindJoinContext(&request, excluded);
  request.right_row_evidence[0].creator_local_transaction_id = 2402;
  result = exec::ExecuteCanonicalJoinMgaBoundary(request);
  passed &= Require(
      EmptyJoinFailure(result),
      "visible input-row creator bypassed the captured in-doubt exclusion");

  auto committed = JoinMgaContext();
  committed.visible_committed_high_watermark = 2403;
  request = Request();
  BindJoinContext(&request, committed);
  request.candidate_evidence[0].left_creator_local_transaction_id = 2403;
  request.candidate_evidence[0].right_creator_local_transaction_id = 2403;
  result = exec::ExecuteCanonicalJoinMgaBoundary(request);
  passed &= Require(
      result.diagnostic.ok && result.mga_boundary_proven &&
          result.visible_pair_count == 1 &&
          result.output_batch.rows.size() == 1 &&
          exec::PhysicalMgaStatementContextEqual(
              result.mga_statement_context, committed),
      "committed non-excluded join creators at high-water were refused: " +
          result.diagnostic.detail);

  auto zero = JoinMgaContext();
  zero.visible_committed_high_watermark = 0;
  request = Request();
  BindJoinContext(&request, zero);
  request.candidate_evidence[0].left_creator_local_transaction_id = 2403;
  result = exec::ExecuteCanonicalJoinMgaBoundary(request);
  passed &= Require(
      EmptyJoinFailure(result),
      "zero high-water admitted a non-owner join creator or leaked output");
  return passed;
}

// QOW-TEST-QRY-012-MGA-V1
// QOW-TEST-QRY-012-MGA-V2
bool ValidateJoinMgaBoundary() {
  bool passed = true;
  auto result = exec::ExecuteCanonicalJoinMgaBoundary(Request());
  passed &= Require(
      result.diagnostic.ok && result.mga_boundary_proven &&
          result.candidate_pair_count == 3 && result.visible_pair_count == 1 &&
          result.visibility_filtered_pair_count == 1 &&
          result.security_filtered_pair_count == 1 &&
          result.output_batch.rows.size() == 1 &&
          result.output_batch.rows[0].values[1].encoded_value == "10" &&
          result.output_batch.rows[0].values[3].encoded_value == "20" &&
          result.executed_physical_node_id == 2403 &&
          result.mga_statement_context
                  .visible_committed_high_watermark == 0 &&
          exec::PhysicalMgaStatementContextEqual(
              result.mga_statement_context, JoinMgaContext()),
      "MGA boundary produced the wrong visible and secured join output");

  auto request = Request();
  SelectTypedCompositeProfile(&request);
  result = exec::ExecuteCanonicalJoinMgaBoundary(request);
  passed &= Require(
      result.diagnostic.ok && result.mga_boundary_proven &&
          result.candidate_pair_count == 3 && result.visible_pair_count == 1 &&
          result.output_batch.rows.size() == 1 &&
          result.output_batch.rows[0].values[0].encoded_value == "1.5" &&
          result.output_batch.rows[0].values[1].encoded_value == "true" &&
          result.output_batch.rows[0].values[2].encoded_value == "1.50" &&
          result.output_batch.rows[0].values[3].encoded_value == "true",
      "typed composite join keys did not survive MGA exact recheck");

  request = InputEvidenceRequest(
      exec::CanonicalAcceptedJoinKind::kLeftOuter);
  SelectTypedCompositeProfile(&request);
  request.right_row_evidence[2].visibility =
      exec::CanonicalMgaVisibilityDecision::kInvisible;
  RemovePairFive(&request);
  result = exec::ExecuteCanonicalJoinMgaBoundary(request);
  passed &= Require(
      result.diagnostic.ok && result.mga_boundary_proven &&
          result.candidate_pair_count == 2 && result.visible_pair_count == 2 &&
          result.visibility_filtered_right_row_count == 1 &&
          result.output_batch.rows.size() == 3,
      "typed composite LEFT OUTER join lost MGA input-row semantics");

  request = Request();
  request.candidate_evidence[1].index_candidate_generation = 6;
  result = exec::ExecuteCanonicalJoinMgaBoundary(request);
  passed &= Require(!result.diagnostic.ok && !result.mga_boundary_proven &&
                        result.output_batch.rows.empty() &&
                        result.candidate_pair_count == 0,
                    "stale index generation published join output");

  request = Request();
  request.candidate_evidence[0].exact_key_recheck =
      api::EngineSqlTruthValue::false_value;
  result = exec::ExecuteCanonicalJoinMgaBoundary(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "failed exact key recheck published a candidate");

  request = Request();
  request.candidate_evidence[0].left_visibility =
      exec::CanonicalMgaVisibilityDecision::kIndeterminate;
  result = exec::ExecuteCanonicalJoinMgaBoundary(request);
  passed &= Require(!result.diagnostic.ok,
                    "indeterminate MGA visibility was accepted");

  request = Request();
  request.candidate_evidence[0].security_decision =
      exec::CanonicalMgaSecurityDecision::kIndeterminate;
  result = exec::ExecuteCanonicalJoinMgaBoundary(request);
  passed &= Require(!result.diagnostic.ok,
                    "indeterminate security decision was accepted");

  request = Request();
  request.mga_authority.statement_context.statement_snapshot_uuid =
      "019f0000-0000-7200-8000-000000009999";
  result = exec::ExecuteCanonicalJoinMgaBoundary(request);
  passed &= Require(!result.diagnostic.ok,
                    "inventory snapshot drift was accepted");

  request = Request();
  request.candidate_evidence.pop_back();
  result = exec::ExecuteCanonicalJoinMgaBoundary(request);
  passed &= Require(!result.diagnostic.ok,
                    "missing candidate MGA evidence was accepted");

  request = Request();
  request.candidate_evidence[1].pair_index = 0;
  result = exec::ExecuteCanonicalJoinMgaBoundary(request);
  passed &= Require(!result.diagnostic.ok,
                    "candidate physical identity drift was accepted");

  request = Request();
  request.maximum_boundary_rechecks = 2;
  result = exec::ExecuteCanonicalJoinMgaBoundary(request);
  passed &= Require(!result.diagnostic.ok,
                    "MGA boundary recheck bound was exceeded");

  request = Request();
  request.strategy_request.join_kind =
      exec::CanonicalAcceptedJoinKind::kLeftOuter;
  request.strategy_request.residual_request.key_request.physical_dag.nodes
      .back()
      .implementation_id = "join.hash-left-outer.int64-equality.v1";
  result = exec::ExecuteCanonicalJoinMgaBoundary(request);
  passed &= Require(!result.diagnostic.ok && !result.mga_boundary_proven &&
                        result.output_batch.rows.empty(),
                    "non-inner output bypassed its absent MGA evidence model");

  request = InputEvidenceRequest(
      exec::CanonicalAcceptedJoinKind::kLeftOuter);
  request.right_row_evidence[2].visibility =
      exec::CanonicalMgaVisibilityDecision::kInvisible;
  RemovePairFive(&request);
  result = exec::ExecuteCanonicalJoinMgaBoundary(request);
  passed &= Require(
      result.diagnostic.ok && result.mga_boundary_proven &&
          result.candidate_pair_count == 2 && result.visible_pair_count == 2 &&
          result.visible_left_row_count == 3 &&
          result.visible_right_row_count == 2 &&
          result.visibility_filtered_right_row_count == 1 &&
          result.output_batch.rows.size() == 3 &&
          result.output_batch.rows[0].values[3].encoded_value == "20" &&
          result.output_batch.rows[1].values[1].encoded_value == "11" &&
          result.output_batch.rows[1].values[2].state ==
              api::EngineValueState::sql_null &&
          result.output_batch.rows[2].values[3].encoded_value == "21",
      "LEFT OUTER MGA input filtering did not recompute unmatched output");

  request = InputEvidenceRequest(
      exec::CanonicalAcceptedJoinKind::kRightOuter);
  request.left_row_evidence[1].visibility =
      exec::CanonicalMgaVisibilityDecision::kInvisible;
  RemovePairFive(&request);
  result = exec::ExecuteCanonicalJoinMgaBoundary(request);
  passed &= Require(
      result.diagnostic.ok && result.visible_left_row_count == 2 &&
          result.visibility_filtered_left_row_count == 1 &&
          result.output_batch.rows.size() == 3 &&
          result.output_batch.rows[2].values[0].state ==
              api::EngineValueState::sql_null &&
          result.output_batch.rows[2].values[3].encoded_value == "22",
      "RIGHT OUTER MGA input filtering did not retain unmatched right rows");

  request = InputEvidenceRequest(
      exec::CanonicalAcceptedJoinKind::kFullOuter);
  request.left_row_evidence[1].visibility =
      exec::CanonicalMgaVisibilityDecision::kInvisible;
  RemovePairFive(&request);
  result = exec::ExecuteCanonicalJoinMgaBoundary(request);
  passed &= Require(
      result.diagnostic.ok && result.output_batch.rows.size() == 3 &&
          result.output_batch.rows[2].values[0].state ==
              api::EngineValueState::sql_null &&
          result.output_batch.rows[2].values[3].encoded_value == "22",
      "FULL OUTER MGA input filtering lost unmatched-side semantics");

  request = InputEvidenceRequest(
      exec::CanonicalAcceptedJoinKind::kLeftSemi);
  result = exec::ExecuteCanonicalJoinMgaBoundary(request);
  passed &= Require(
      result.diagnostic.ok && result.candidate_pair_count == 3 &&
          result.output_batch.columns.size() == 2 &&
          result.output_batch.rows.size() == 3 &&
          result.output_batch.rows[2].values[1].encoded_value == "12",
      "LEFT SEMI MGA evidence did not publish each matched left row once");

  request = InputEvidenceRequest(
      exec::CanonicalAcceptedJoinKind::kLeftAnti);
  request.right_row_evidence[2].visibility =
      exec::CanonicalMgaVisibilityDecision::kInvisible;
  RemovePairFive(&request);
  result = exec::ExecuteCanonicalJoinMgaBoundary(request);
  passed &= Require(
      result.diagnostic.ok && result.candidate_pair_count == 2 &&
          result.output_batch.columns.size() == 2 &&
          result.output_batch.rows.size() == 1 &&
          result.output_batch.rows[0].values[1].encoded_value == "11",
      "LEFT ANTI MGA input filtering did not recompute unmatched left rows");

  request = InputEvidenceRequest(
      exec::CanonicalAcceptedJoinKind::kLeftOuter);
  request.right_row_evidence[2].security_decision =
      exec::CanonicalMgaSecurityDecision::kDenied;
  RemovePairFive(&request);
  result = exec::ExecuteCanonicalJoinMgaBoundary(request);
  passed &= Require(result.diagnostic.ok &&
                        result.security_filtered_right_row_count == 1 &&
                        result.output_batch.rows.size() == 3 &&
                        result.output_batch.rows[1].values[2].state ==
                            api::EngineValueState::sql_null,
                    "row security filtering did not precede outer semantics");

  request = InputEvidenceRequest(
      exec::CanonicalAcceptedJoinKind::kLeftOuter);
  request.right_row_evidence.pop_back();
  result = exec::ExecuteCanonicalJoinMgaBoundary(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "missing input-row evidence was accepted");

  request = InputEvidenceRequest(
      exec::CanonicalAcceptedJoinKind::kLeftOuter);
  request.left_row_evidence[0].creator_local_transaction_id = 9999;
  result = exec::ExecuteCanonicalJoinMgaBoundary(request);
  passed &= Require(!result.diagnostic.ok,
                    "input-row creator drift was accepted");

  request = InputEvidenceRequest(
      exec::CanonicalAcceptedJoinKind::kLeftOuter);
  request.left_row_evidence[1].visibility =
      exec::CanonicalMgaVisibilityDecision::kInvisible;
  request.strategy_request.residual_request.key_request.left_batch.rows[1]
      .values[0]
      .encoded_value = "bad";
  RemovePairFive(&request);
  result = exec::ExecuteCanonicalJoinMgaBoundary(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "invisible row hid malformed typed input");

  request = InputEvidenceRequest(
      exec::CanonicalAcceptedJoinKind::kLeftOuter);
  request.right_row_evidence[2].visibility =
      exec::CanonicalMgaVisibilityDecision::kInvisible;
  request.strategy_request.residual_request.residual_truth_values[5] =
      api::EngineSqlTruthValue::unspecified;
  RemovePairFive(&request);
  result = exec::ExecuteCanonicalJoinMgaBoundary(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "invisible row hid invalid residual truth state");

  request = InputEvidenceRequest(
      exec::CanonicalAcceptedJoinKind::kLeftOuter);
  request.right_row_evidence[0].visibility =
      exec::CanonicalMgaVisibilityDecision::kIndeterminate;
  result = exec::ExecuteCanonicalJoinMgaBoundary(request);
  passed &= Require(!result.diagnostic.ok,
                    "indeterminate input-row visibility was accepted");

  request = InputEvidenceRequest(
      exec::CanonicalAcceptedJoinKind::kLeftOuter);
  request.candidate_evidence[0].left_row_version_id = 9999;
  result = exec::ExecuteCanonicalJoinMgaBoundary(request);
  passed &= Require(!result.diagnostic.ok,
                    "matched pair row-version drift was accepted");

  request = InputEvidenceRequest(
      exec::CanonicalAcceptedJoinKind::kLeftOuter);
  request.maximum_boundary_rechecks = 8;
  result = exec::ExecuteCanonicalJoinMgaBoundary(request);
  passed &= Require(!result.diagnostic.ok,
                    "combined row/pair evidence bound was exceeded");

  request = Request();
  request.strategy_request.residual_request.key_request.physical_dag
      .local_transaction_id = 0;
  result = exec::ExecuteCanonicalJoinMgaBoundary(request);
  passed &= Require(EmptyJoinFailure(result),
                    "join MGA boundary accepted a missing transaction");

  request = Request();
  request.strategy_request.residual_request.key_request.physical_dag
      .mga_statement_context.in_doubt_excluded_local_transaction_ids = {
      kOldestActiveLocalTransactionId};
  result = exec::ExecuteCanonicalJoinMgaBoundary(request);
  passed &= Require(EmptyJoinFailure(result),
                    "overlapping MGA exclusions reached boundary access");

  request = Request();
  request.strategy_request.residual_request.key_request.physical_dag
      .mga_statement_context.owning_local_transaction_id = 2404;
  result = exec::ExecuteCanonicalJoinMgaBoundary(request);
  passed &= Require(EmptyJoinFailure(result),
                    "narrowed owner alias reached boundary access");

  request = Request();
  request.mga_authority.resolve_current = [context = JoinMgaContext()] {
    exec::CanonicalMgaCurrentResolution current;
    current.statement_context = context;
    current.statement_context.statement_uuid =
        "019f0000-0000-7200-8000-00000000e557";
    return current;
  };
  result = exec::ExecuteCanonicalJoinMgaBoundary(request);
  passed &= Require(EmptyJoinFailure(result),
                    "cross-statement current resolution reached boundary access");

  request = Request();
  request.strategy_request.residual_request.key_request.left_batch.rows.clear();
  request.strategy_request.residual_request.residual_truth_values.clear();
  request.candidate_evidence.clear();
  result = exec::ExecuteCanonicalJoinMgaBoundary(request);
  passed &= Require(result.diagnostic.ok && result.mga_boundary_proven &&
                        result.candidate_pair_count == 0 &&
                        result.output_batch.rows.empty(),
                    "empty join invented MGA boundary candidates");
  return passed;
}

}  // namespace

int main() {
  return ValidateJoinMgaBoundary() && ValidateJoinCreatorExclusions()
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
