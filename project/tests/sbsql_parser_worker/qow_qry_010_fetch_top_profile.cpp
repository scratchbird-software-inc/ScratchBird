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
#include <limits>
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
    std::cerr << "QOW-TEST-QRY-010-FETCH-TOP-PROFILE-V1: " << detail << '\n';
  }
  return condition;
}

exec::PhysicalMgaStatementContext StatementContext(
    const std::string& statement_snapshot_uuid) {
  return {
      "019f0000-0000-7200-8000-00000000fe01",
      "019f0000-0000-7200-8000-00000000fe02",
      statement_snapshot_uuid,
      "019f0000-0000-7200-8000-00000000fe03",
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

void SetStatementContext(
    exec::TypedPhysicalNodeDag* dag,
    const exec::PhysicalMgaStatementContext& context) {
  dag->mga_statement_context = context;
  for (auto& node : dag->nodes) node.mga_statement_context = context;
}

exec::CanonicalExecutionMgaAuthority BindPhysicalAbiV2(
    exec::TypedPhysicalNodeDag* dag) {
  dag->abi_version = 2;
  dag->local_transaction_id = kOwnerLocalTransactionId;
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
  dag->memory_budget_bytes = 4096;
  dag->optimizer_published = true;
  dag->immutable_node_identity_validated = true;
  dag->capability_validated_before_access = true;
  const auto context = StatementContext(
      dag->admission_evidence.at(3).evidence_uuid);
  SetStatementContext(dag, context);
  for (auto& node : dag->nodes) {
    node.selected_alternative_uuid =
        "019f0000-0000-7200-8000-00000000fe04";
    node.executor_capability_uuid =
        "019f0000-0000-7200-8000-00000000fe05";
    node.executor_capability_abi_version = 1;
    node.cost_vector_uuid =
        "019f0000-0000-7200-8000-00000000fe06";
    node.memory_bytes_required = 1;
    node.engine_capability_validated = true;
  }
  exec::CanonicalExecutionMgaAuthority authority;
  authority.statement_context = context;
  authority.origin = exec::CanonicalMgaAuthorityOrigin::kClosureTestSeam;
  authority.resolve_current = [context] {
    exec::CanonicalMgaCurrentResolution current;
    current.statement_context = context;
    return current;
  };
  return authority;
}

api::EngineDescriptor Descriptor() {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical =
      "019f0000-0000-7200-8000-000000001101";
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "int64";
  descriptor.encoded_descriptor =
      "type_uuid=019f0000-0000-7300-8000-000000001102;"
      "nullability=non_null";
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

exec::CanonicalDescriptorFetchProfileRequest Request() {
  const auto descriptor = Descriptor();
  exec::CanonicalDescriptorFetchProfileRequest request;
  request.physical_dag.selected_plan_uuid =
      "019f0000-0000-7200-8000-000000001103";
  request.physical_dag.root_physical_node_id = 1102;
  request.physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000001111"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000001112"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000001113"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000001114"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000001115"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000001116"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000001117"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000001118"},
  };
  request.physical_dag.nodes = {
      {.physical_node_id = 1101,
       .relational_node_id = 1101,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.typed.v1",
       .output_descriptor_ids = {1101},
       .causal_counter_id = 11001},
      {.physical_node_id = 1102,
       .relational_node_id = 1102,
       .node_kind = exec::PhysicalNodeKind::kLimit,
       .implementation_id = "fetch.native.rows-only.v1",
       .input_physical_node_ids = {1101},
       .output_descriptor_ids = {1101},
       .causal_counter_id = 11002},
  };
  request.selected_physical_node_id = 1102;
  request.input_batch = exec::MakeDescriptorBatch(
      {{"value", descriptor, false, 1101}},
      {{{Value(descriptor, "1")}},
       {{Value(descriptor, "2")}},
       {{Value(descriptor, "3")}},
       {{Value(descriptor, "4")}}});
  request.form = exec::CanonicalFetchTopProfileForm::fetch_first_rows_only;
  request.row_count = 2;
  request.offset = 1;
  request.row_count_is_bound = true;
  request.mga_authority = BindPhysicalAbiV2(&request.physical_dag);
  return request;
}

// QOW-TEST-QRY-010-FETCH-TOP-PROFILE-V1
bool ValidateFetchFirstProfile() {
  bool passed = true;
  auto result = exec::ExecuteCanonicalDescriptorFetchProfile(Request());
  passed &= Require(result.diagnostic.ok &&
                        result.executed_physical_node_id == 1102 &&
                        result.causal_counter_id == 11002 &&
                        result.mga_statement_context
                                .visible_committed_high_watermark == 0 &&
                        exec::PhysicalMgaStatementContextEqual(
                            result.mga_statement_context,
                            Request().mga_authority.statement_context),
                    "native FETCH FIRST profile did not execute limit node");
  passed &= Require(result.output_batch.rows.size() == 2 &&
                        result.output_batch.rows[0].values[0].encoded_value ==
                            "2" &&
                        result.output_batch.rows[1].values[0].encoded_value ==
                            "3",
                    "FETCH FIRST count or preceding offset was not preserved");

  auto request = Request();
  request.row_count = 0;
  result = exec::ExecuteCanonicalDescriptorFetchProfile(request);
  passed &= Require(result.diagnostic.ok && result.output_batch.rows.empty(),
                    "FETCH FIRST zero returned rows");

  request = Request();
  request.row_count = std::numeric_limits<std::uint64_t>::max();
  result = exec::ExecuteCanonicalDescriptorFetchProfile(request);
  passed &= Require(result.diagnostic.ok &&
                        result.output_batch.rows.size() == 3,
                    "maximum FETCH FIRST count overflowed row bounds");

  request = Request();
  request.input_batch.rows.clear();
  result = exec::ExecuteCanonicalDescriptorFetchProfile(request);
  passed &= Require(result.diagnostic.ok && result.output_batch.rows.empty() &&
                        result.output_batch.columns.size() == 1,
                    "empty FETCH FIRST input lost its descriptor table");

  for (const auto form : {
           exec::CanonicalFetchTopProfileForm::fetch_first_rows_with_ties,
           exec::CanonicalFetchTopProfileForm::top_rows,
           exec::CanonicalFetchTopProfileForm::top_percent,
           exec::CanonicalFetchTopProfileForm::top_rows_with_ties,
           static_cast<exec::CanonicalFetchTopProfileForm>(255),
       }) {
    request = Request();
    request.form = form;
    result = exec::ExecuteCanonicalDescriptorFetchProfile(request);
    passed &= Require(
        !result.diagnostic.ok && result.output_batch.rows.empty() &&
            result.diagnostic.diagnostic_code ==
                "QOW-DIAG-QRY-010-FETCH-TOP-PROFILE-REFUSAL-V1",
        "omitted FETCH/TOP form did not use canonical refusal");
  }

  request = Request();
  request.row_count_is_bound = false;
  result = exec::ExecuteCanonicalDescriptorFetchProfile(request);
  passed &= Require(
      !result.diagnostic.ok && result.output_batch.rows.empty() &&
          result.diagnostic.diagnostic_code ==
              "QOW-DIAG-QRY-010-FETCH-TOP-PROFILE-REFUSAL-V1",
      "unbound FETCH FIRST count was accepted");

  request = Request();
  auto nil = request.physical_dag.mga_statement_context;
  nil.statement_uuid = "00000000-0000-0000-0000-000000000000";
  SetStatementContext(&request.physical_dag, nil);
  request.mga_authority.statement_context = nil;
  result = exec::ExecuteCanonicalDescriptorFetchProfile(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "nil statement identity reached FETCH access");

  request = Request();
  auto malformed = request.physical_dag.mga_statement_context;
  malformed.owning_transaction_uuid = "not-a-canonical-uuid";
  SetStatementContext(&request.physical_dag, malformed);
  request.mga_authority.statement_context = malformed;
  result = exec::ExecuteCanonicalDescriptorFetchProfile(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "malformed transaction identity reached FETCH access");

  request = Request();
  std::swap(request.physical_dag.admission_evidence[0],
            request.physical_dag.admission_evidence[1]);
  result = exec::ExecuteCanonicalDescriptorFetchProfile(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "reordered publication evidence reached FETCH access");

  request = Request();
  request.physical_dag.catalog_epoch_uuid =
      request.physical_dag.mga_statement_context
          .statement_metadata_snapshot_uuid;
  request.physical_dag.admission_evidence[1].evidence_uuid =
      request.physical_dag.catalog_epoch_uuid;
  result = exec::ExecuteCanonicalDescriptorFetchProfile(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "catalog epoch was conflated with metadata snapshot");
  return passed;
}

}  // namespace

int main() {
  return ValidateFetchFirstProfile() ? EXIT_SUCCESS : EXIT_FAILURE;
}
