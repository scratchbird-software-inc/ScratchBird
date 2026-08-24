// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "../executor/model_family_exchange.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

struct ModelFamilyCostVectorV1 {
  std::string cost_vector_uuid;
  std::string provenance_uuid;
  std::string property_snapshot_uuid;
  std::string calibration_profile_uuid;
  std::string scalarization_policy_id;
  std::uint64_t provenance_generation{0};
  std::uint32_t confidence_basis_points{0};
  std::uint64_t scalar_score{0};
  std::uint64_t startup_units{0};
  std::uint64_t cpu_units{0};
  std::uint64_t sequential_read_units{0};
  std::uint64_t random_read_units{0};
  std::uint64_t page_write_units{0};
  std::uint64_t cache_units{0};
  std::uint64_t memory_bytes_required{0};
  std::uint64_t memory_grant_units{0};
  std::uint64_t spill_units{0};
  std::uint64_t network_units{0};
  std::uint64_t compression_units{0};
  std::uint64_t encryption_units{0};
  std::uint64_t predicate_evaluation_units{0};
  std::uint64_t vector_distance_units{0};
  std::uint64_t text_scoring_units{0};
  std::uint64_t spatial_evaluation_units{0};
  std::uint64_t udr_invocation_units{0};
  std::uint64_t mga_units{0};
  std::uint64_t index_maintenance_units{0};
  std::uint64_t uncertainty_penalty{0};
  std::uint64_t risk_penalty{0};
};

enum class ModelFamilyAlternativeRouteClassV1 : std::uint8_t {
  kNative,
  kExactCollectionFallback,
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
  ModelFamilyAlternativeRouteClassV1 route_class{
      ModelFamilyAlternativeRouteClassV1::kNative};
  std::string candidate_inventory_receipt_uuid;
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
  // Exact pre-admission identity for one leg of a signed RCP-080 common
  // composition. Empty/zero retains the standalone coordinator contract.
  std::string composition_profile_id;
  std::uint16_t composition_lexical_source_ordinal{0};
  std::uint16_t composition_arity{0};
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
  bool optimizer_owned_enumeration{false};
  ModelFamilyCandidateV1 selected_candidate;
  scratchbird::engine::executor::TypedPhysicalNodeDag physical_dag;
  std::string logical_operator_id;
  std::string physical_operator_id;
  std::string diagnostic_id;
  std::string detail;
  std::string candidate_inventory_receipt_uuid;
  std::string selected_cost_explain_json;
};

ModelFamilyCoordinatorResultV1 CoordinateDocumentFamilySourceV1(
    const ModelFamilyCoordinatorRequestV1& request);

ModelFamilyCoordinatorResultV1 CoordinateKeyValueFamilySourceV1(
    const ModelFamilyCoordinatorRequestV1& request);

ModelFamilyCoordinatorResultV1 CoordinateModelFamilySourceV1(
    const ModelFamilyCoordinatorRequestV1& request);

std::string SerializeModelFamilyCostVectorToJsonV1(
    const ModelFamilyCostVectorV1& cost);

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

// RCP-080 complete coordinator ABI. The earlier composition carrier above is
// retained as the admission surface proven by RCP-079. This carrier is the
// execution-ready contract: every dependency is explicit, every selected leg
// retains its family-local decision dimensions, and the coordinator publishes
// a stable schedule without recomputing a cross-family scalar cost.
struct ModelFamilyDependencyEdgeV1 {
  std::uint16_t abi_version{1};
  std::string edge_uuid;
  std::uint16_t producer_lexical_source_ordinal{0};
  std::uint16_t consumer_lexical_source_ordinal{0};
  std::string edge_kind{"data_binding"};
  std::string required_property_uuid;
  std::string delivered_property_uuid;
  std::string descriptor_lineage_uuid;
  std::vector<std::uint32_t> producer_output_descriptor_ids;
  std::vector<std::uint32_t> consumer_input_descriptor_ids;
  std::vector<std::string> producer_output_descriptor_uuids;
  std::vector<std::string> consumer_input_descriptor_uuids;
  bool descriptor_compatible{true};
  bool semantics_authorized{true};
  bool parser_execution_authority_claimed{false};
  bool transaction_finality_authority_claimed{false};
};

struct ModelFamilyDependencyAlternativeV1 {
  std::string alternative_uuid;
  std::string candidate_inventory_receipt_uuid;
  std::string implementation_id;
  std::vector<std::string> operation_ids;
  std::string operation_id;
  std::string operation_scope_receipt_uuid;
  std::string selection_policy_receipt_uuid;
  std::uint64_t authority_approved_comparison_rank{0};
  ModelFamilyCostVectorV1 family_local_cost;
  bool available{false};
  bool exact{false};
  bool exact_fallback{false};
  bool admitted{false};
};

struct ModelFamilyDependencyLegV1 {
  std::uint16_t abi_version{1};
  std::uint16_t lexical_source_ordinal{0};
  std::string physical_node_uuid;
  std::string family_id;
  std::vector<std::string> operation_ids;
  std::string operation_id;
  std::string selected_plan_uuid;
  std::string selected_alternative_uuid;
  std::string provider_uuid;
  std::string capability_uuid;
  std::string delivered_property_uuid;
  std::string bound_object_uuid;
  std::string catalog_snapshot_uuid;
  std::string current_catalog_snapshot_uuid;
  std::string descriptor_snapshot_uuid;
  std::string current_descriptor_snapshot_uuid;
  std::string security_context_uuid;
  std::string current_security_context_uuid;
  std::string policy_snapshot_uuid;
  std::string current_policy_snapshot_uuid;
  std::string resource_contract_uuid;
  std::string current_resource_contract_uuid;
  std::string operation_scope_receipt_uuid;
  std::string selected_alternative_receipt_uuid;
  std::uint64_t root_physical_node_id{0};
  std::vector<std::uint32_t> output_descriptor_ids;
  std::vector<std::string> output_descriptor_uuids;
  ModelFamilyCostVectorV1 family_local_cost;
  std::vector<ModelFamilyDependencyAlternativeV1> candidate_alternatives;
  scratchbird::engine::executor::PhysicalMgaStatementContext
      mga_statement_context;
  std::uint64_t catalog_generation{0};
  std::uint64_t current_catalog_generation{0};
  std::uint64_t descriptor_generation{0};
  std::uint64_t current_descriptor_generation{0};
  std::uint64_t security_generation{0};
  std::uint64_t current_security_generation{0};
  std::uint64_t policy_generation{0};
  std::uint64_t current_policy_generation{0};
  std::uint64_t resource_generation{0};
  std::uint64_t current_resource_generation{0};
  std::uint64_t provider_generation{0};
  std::uint64_t current_provider_generation{0};
  std::uint16_t capability_abi_version{1};
  std::uint64_t capability_generation{0};
  std::uint64_t current_capability_generation{0};
  std::uint64_t memory_grant_bytes{0};
  std::uint64_t exchange_buffer_bytes{0};
  std::uint64_t maximum_rows{0};
  std::uint64_t maximum_columns{0};
  std::uint64_t maximum_cells{0};
  bool selected{false};
  bool security_admitted{false};
  bool capability_admitted{false};
  bool exact{false};
  bool exact_fallback_selected{false};
  bool exact_fallback_available{false};
  bool exact_recheck_required{true};
  bool base_row_mga_recheck_required{true};
  bool security_recheck_required{true};
  bool cleanup_supported{false};
  bool cancellation_supported{false};
  bool parallel_eligible{false};
  bool spill_eligible{false};
  bool local_scope{true};
  bool cluster_scope_required{false};
  bool parser_planning_authority_claimed{false};
  bool transaction_finality_authority_claimed{false};
};

struct ModelFamilyRelationalConsumerV1 {
  std::uint16_t abi_version{1};
  std::string physical_node_uuid;
  std::uint64_t physical_node_id{0};
  std::uint64_t causal_counter_id{0};
  std::string selected_implementation_uuid;
  std::string expected_security_receipt_uuid;
  std::string join_form_id;
  std::vector<std::string> input_physical_node_uuids;
  std::vector<std::uint32_t> input_descriptor_ids;
  std::vector<std::uint32_t> output_descriptor_ids;
  std::vector<std::string> input_descriptor_uuids;
  std::vector<std::string> output_descriptor_uuids;
  scratchbird::engine::executor::PhysicalMgaStatementContext
      mga_statement_context;
  std::uint64_t maximum_rows{0};
  std::uint64_t maximum_columns{0};
  std::uint64_t maximum_cells{0};
  std::uint64_t memory_grant_bytes{0};
  bool canonical_root{false};
  bool exact{false};
  bool cleanup_supported{false};
  bool cancellation_supported{false};
  bool parser_execution_authority_claimed{false};
  bool transaction_finality_authority_claimed{false};
};

struct ModelFamilyCoordinatorRuleReceiptV1 {
  std::string rule_id;
  std::string evidence_id;
  std::string receipt_uuid;
  std::uint64_t causal_counter_id{0};
  bool complete{false};
};

struct ModelFamilyScheduledLegV1 {
  ModelFamilyDependencyLegV1 leg;
  std::string composition_admission_receipt_uuid;
  std::uint16_t composition_arity{0};
  std::vector<std::uint16_t> dependency_ordinals;
  std::uint32_t schedule_wave{0};
  std::uint32_t stable_start_ordinal{0};
  std::uint64_t causal_counter_id{0};
};

struct ModelFamilyDependencyCoordinatorRequestV1 {
  std::uint16_t abi_version{1};
  std::string composition_profile_id;
  std::string bound_sblr_tree_uuid;
  std::uint64_t selected_plan_generation{0};
  std::uint64_t current_selected_plan_generation{0};
  std::string canonical_root_physical_node_uuid;
  std::uint64_t canonical_root_physical_node_id{0};
  std::vector<ModelFamilyDependencyLegV1> legs;
  std::vector<ModelFamilyDependencyEdgeV1> edges;
  std::vector<ModelFamilyRelationalConsumerV1> relational_consumers;
  std::uint64_t statement_memory_budget_bytes{0};
  std::uint64_t maximum_spill_bytes{0};
  std::uint64_t backpressure_high_watermark_rows{0};
  std::uint64_t backpressure_low_watermark_rows{0};
  bool spill_required{false};
  bool engine_temporary_storage_available{false};
  bool spill_cleanup_path_available{false};
  bool cluster_capability_available{false};
  bool signed_short_circuit_enabled{false};
  bool feedback_observation_frozen{false};
  bool feedback_target_is_later_plan{false};
  bool current_plan_mutation_requested{false};
  std::uint64_t feedback_observation_generation{0};
  std::uint64_t feedback_target_plan_generation{0};
};

struct ModelFamilyDependencyCoordinatorResultV1 {
  bool accepted{false};
  bool deterministic{false};
  bool data_access_allowed{false};
  bool root_publication_candidate{false};
  bool no_partial_root{true};
  bool spill_reservation_required{false};
  std::uint64_t admitted_peak_memory_bytes{0};
  std::uint64_t admitted_spill_bytes{0};
  std::uint64_t selected_plan_generation{0};
  std::uint64_t expected_cleanup_component_count{0};
  std::vector<ModelFamilyScheduledLegV1> stable_schedule;
  std::vector<std::vector<std::uint16_t>> parallel_waves;
  std::vector<ModelFamilyDependencyEdgeV1> dependency_edges;
  std::vector<ModelFamilyRelationalConsumerV1> relational_consumers;
  std::vector<ModelFamilyCoordinatorRuleReceiptV1> rule_receipts;
  std::string dependency_dag_receipt_uuid;
  std::string composition_admission_receipt_uuid;
  std::string diagnostic_id;
  std::string detail;
};

ModelFamilyDependencyCoordinatorResultV1
CoordinateModelFamilyDependencyDagV1(
    const ModelFamilyDependencyCoordinatorRequestV1& request);

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
