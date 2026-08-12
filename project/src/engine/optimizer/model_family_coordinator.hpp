// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "../executor/model_family_executor.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

struct ModelFamilyCostVectorV1 {
  std::string cost_vector_uuid;
  std::uint64_t cpu_units{0};
  std::uint64_t sequential_read_units{0};
  std::uint64_t random_read_units{0};
  std::uint64_t memory_bytes_required{0};
  std::uint64_t uncertainty_penalty{0};
  std::uint64_t risk_penalty{0};
};

struct ModelFamilyCandidateV1 {
  std::string alternative_uuid;
  std::string provider_uuid;
  std::string capability_uuid;
  std::string implementation_id{"physical_document_path_scan_v1"};
  std::uint64_t provider_generation{0};
  bool available{false};
  bool exact{false};
  bool exact_collection_fallback{false};
  bool residual_recheck_required{true};
  bool base_row_mga_recheck_required{true};
  bool security_recheck_required{true};
  bool engine_owned{true};
  bool local_scope{true};
  bool parser_planning_authority_claimed{false};
  bool transaction_finality_authority_claimed{false};
  ModelFamilyCostVectorV1 cost;
};

struct ModelFamilyCoordinatorRequestV1 {
  std::uint16_t abi_version{1};
  std::string family_id;
  // Ordered, distinct operation roots for a multi-stage model leg. The
  // singular field below is only a compatibility projection when the leg
  // has at most one effective operation after its source root.
  std::vector<std::string> operation_ids;
  std::string operation_id;
  std::string logical_operator_id;
  std::uint32_t logical_node_id{0};
  std::string object_uuid;
  std::vector<std::uint32_t> output_descriptor_ids;
  scratchbird::engine::executor::PhysicalMgaStatementContext
      mga_statement_context;
  std::string bound_sblr_tree_uuid;
  std::string catalog_epoch_uuid;
  std::string security_context_uuid;
  std::string capability_snapshot_uuid;
  std::string resource_snapshot_uuid;
  std::string statistics_snapshot_uuid;
  std::string route_snapshot_uuid;
  std::uint64_t catalog_generation{0};
  std::uint64_t current_catalog_generation{0};
  std::uint64_t security_epoch{0};
  std::uint64_t policy_epoch{0};
  std::uint64_t resource_epoch{0};
  std::uint64_t statistics_generation{0};
  std::uint64_t route_epoch{0};
  std::uint64_t route_generation{0};
  std::uint64_t memory_budget_bytes{0};
  bool security_admitted{true};
  bool parser_planning_authority_claimed{false};
  bool transaction_finality_authority_claimed{false};
  std::vector<ModelFamilyCandidateV1> candidates;
};

struct ModelFamilyCoordinatorResultV1 {
  bool accepted{false};
  bool selected{false};
  bool data_access_allowed{false};
  bool deterministic{false};
  bool exact_fallback_selected{false};
  ModelFamilyCandidateV1 selected_candidate;
  scratchbird::engine::executor::TypedPhysicalNodeDag physical_dag;
  std::string logical_operator_id;
  std::string physical_operator_id;
  std::string diagnostic_id;
  std::string detail;
};

ModelFamilyCoordinatorResultV1 CoordinateDocumentFamilySourceV1(
    const ModelFamilyCoordinatorRequestV1& request);

ModelFamilyCoordinatorResultV1 CoordinateKeyValueFamilySourceV1(
    const ModelFamilyCoordinatorRequestV1& request);

ModelFamilyCoordinatorResultV1 CoordinateModelFamilySourceV1(
    const ModelFamilyCoordinatorRequestV1& request);

struct MultilegDescriptorProfileV1 {
  std::uint8_t profile_kind{0};
  std::uint16_t slot{0};
  std::string descriptor_uuid;
  std::string type_uuid;
  bool nullable{false};
};

struct MultilegDescriptorDemandV1 {
  std::uint16_t lexical_source_ordinal{0};
  std::uint16_t field_ordinal{0};
  std::string family_id;
  std::string field_id;
  std::string canonical_type_name;
  bool nullable{false};
  bool derived{true};
  std::string persisted_descriptor_uuid;
  std::string persisted_type_uuid;
};

struct MultilegDescriptorAllocationV1 {
  MultilegDescriptorDemandV1 demand;
  std::string descriptor_uuid;
  std::string type_uuid;
  std::uint8_t profile_kind{0};
  std::uint16_t slot{0};
};

struct MultilegDescriptorAllocationResultV1 {
  bool accepted{false};
  bool preflight_complete{false};
  std::vector<MultilegDescriptorAllocationV1> allocations;
  std::string diagnostic_id;
  std::string detail;
};

MultilegDescriptorAllocationResultV1 AllocateMultilegResultDescriptorsV1(
    const std::vector<MultilegDescriptorProfileV1>& profiles,
    const std::vector<MultilegDescriptorDemandV1>& demands);

// Engine-owned, synchronous dispatch carrier for the exact statement V10
// descriptor suffix. The scope is deliberately thread-local and single-depth:
// a receipt may authorize only its own statement while its query.execute call
// is on the stack, and no profile authority survives that call.
class MultilegDescriptorDispatchScopeV1 {
 public:
  MultilegDescriptorDispatchScopeV1(
      const std::string& statement_uuid,
      const std::vector<MultilegDescriptorProfileV1>& profiles);
  ~MultilegDescriptorDispatchScopeV1();

  MultilegDescriptorDispatchScopeV1(
      const MultilegDescriptorDispatchScopeV1&) = delete;
  MultilegDescriptorDispatchScopeV1& operator=(
      const MultilegDescriptorDispatchScopeV1&) = delete;
  MultilegDescriptorDispatchScopeV1(
      MultilegDescriptorDispatchScopeV1&&) = delete;
  MultilegDescriptorDispatchScopeV1& operator=(
      MultilegDescriptorDispatchScopeV1&&) = delete;

  bool installed() const { return installed_; }
  const std::string& diagnostic_id() const { return diagnostic_id_; }
  const std::string& detail() const { return detail_; }

 private:
  bool installed_{false};
  std::string statement_uuid_;
  std::string diagnostic_id_;
  std::string detail_;
};

struct MultilegDescriptorDispatchLookupV1 {
  bool accepted{false};
  std::vector<MultilegDescriptorProfileV1> profiles;
  std::string diagnostic_id;
  std::string detail;
};

MultilegDescriptorDispatchLookupV1 LookupMultilegDescriptorDispatchScopeV1(
    const std::string& exact_statement_uuid);

struct ModelFamilyCompositionLegV1 {
  std::uint16_t lexical_source_ordinal{0};
  std::string family_id;
  std::string selected_plan_uuid;
  std::uint64_t root_physical_node_id{0};
  scratchbird::engine::executor::PhysicalMgaStatementContext
      mga_statement_context;
  bool selected{false};
  bool exact_recheck_required{true};
  bool security_recheck_required{true};
  bool parser_planning_authority_claimed{false};
  bool transaction_finality_authority_claimed{false};
};

struct ModelFamilyCompositionRequestV1 {
  std::uint16_t abi_version{1};
  std::string composition_profile_id;
  std::vector<ModelFamilyCompositionLegV1> legs;
  std::uint64_t memory_budget_bytes{0};
};

struct ModelFamilyCompositionResultV1 {
  bool accepted{false};
  bool deterministic{false};
  bool descriptor_preflight_required{true};
  bool root_publication_allowed{false};
  bool no_partial_root{true};
  bool empty_root_required{false};
  std::vector<ModelFamilyCompositionLegV1> lexical_legs;
  std::string composition_receipt_uuid;
  std::string lifecycle_contract_id;
  std::string diagnostic_id;
  std::string detail;
};

ModelFamilyCompositionResultV1 CoordinateModelFamilyCompositionV1(
    const ModelFamilyCompositionRequestV1& request);

struct ModelFamilyJoinAdmissionRequestV1 {
  std::uint16_t abi_version{1};
  std::string left_family_id;
  std::string right_family_id;
  std::string join_form_id;
  std::string condition_form_id;
  std::string scenario_profile_id{"JOIN-SCENARIO-BASELINE-V1"};
};

struct ModelFamilyJoinAdmissionResultV1 {
  bool accepted{false};
  bool deterministic{false};
  bool root_publication_allowed{false};
  std::string left_provider_route_id;
  std::string right_provider_route_id;
  std::string relational_consumer_route_id;
  std::string condition_lowering_route_id;
  std::string diagnostic_id;
  std::string detail;
};

// Resolves one cell of the signed 81-direction x 10-join x 5-condition x
// 15-scenario finite model-family join matrix before either leg may access
// data. A refusal never permits a partial canonical root publication.
ModelFamilyJoinAdmissionResultV1 CoordinateModelFamilyJoinAdmissionV1(
    const ModelFamilyJoinAdmissionRequestV1& request);

}  // namespace scratchbird::engine::optimizer
