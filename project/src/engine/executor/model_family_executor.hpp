// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "model_family_exchange.hpp"
#include "../optimizer/model_family_coordinator.hpp"
#include "../../core/memory/temp_workspace_lifecycle.hpp"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace scratchbird::engine::executor {

struct ModelCapabilityDescriptorV1 {
  std::uint16_t abi_version{1};
  std::string capability_descriptor_id{
      "SB_MODEL_CAPABILITY_DESCRIPTOR_V1"};
  std::string capability_uuid;
  std::string family_id;
  std::string provider_uuid;
  std::uint64_t provider_generation{0};
  std::uint64_t capability_generation{0};
  bool local_scope{true};
  bool available{false};
  bool exact{false};
  bool exact_collection_fallback_available{false};
  bool cancellation_supported{false};
  bool cleanup_supported{false};
  bool residual_recheck_supported{false};
  bool base_row_mga_recheck_supported{false};
  bool security_recheck_supported{false};
  bool provider_visibility_authority_claimed{false};
  bool provider_finality_authority_claimed{false};
};

struct ModelProviderExecutionResultV1 {
  bool ok{false};
  bool data_access_observed{false};
  std::uint64_t rows_examined{0};
  ModelProviderBatchV1 provider_batch;
  std::string diagnostic_id;
  std::string detail;
};

using ModelProviderExecuteCallbackV1 = std::function<
    ModelProviderExecutionResultV1(const ModelSourceInputDescriptorV1&)>;
using ModelCancellationProbeV1 = std::function<bool()>;
using ModelCleanupCallbackV1 = std::function<void()>;

struct ModelFamilyExecutionRequestV1 {
  std::uint16_t abi_version{1};
  ModelSourceInputDescriptorV1 input;
  ModelCapabilityDescriptorV1 capability;
  ModelProviderExecuteCallbackV1 execute_provider;
  ModelCancellationProbeV1 cancellation_requested;
  ModelCleanupCallbackV1 cleanup_provider;
  bool exact_fallback_selected{false};
  bool fault_injected{false};
  bool security_admitted{true};
  std::uint64_t current_catalog_generation{0};
  std::uint64_t current_descriptor_generation{0};
  std::uint64_t current_security_generation{0};
  std::uint64_t current_policy_generation{0};
  std::uint64_t current_resource_generation{0};
  std::uint64_t current_provider_generation{0};
  std::uint64_t current_capability_generation{0};
  PhysicalMgaStatementContext current_mga_statement_context;
};

struct ModelFamilyExecutionResultV1 {
  bool accepted{false};
  bool execution_started{false};
  bool provider_entered{false};
  bool data_access_observed{false};
  std::uint64_t rows_examined{0};
  bool root_published{false};
  bool cleanup_complete{false};
  std::uint32_t cleanup_count{0};
  ModelSourceOutputDescriptorV1 output;
  std::string diagnostic_id;
  std::string detail;
};

ModelFamilyExecutionResultV1 ExecuteModelFamilySourceV1(
    const ModelFamilyExecutionRequestV1& request);

struct ModelFamilyCompositionExecutionLegV1 {
  std::uint16_t lexical_source_ordinal{0};
  ModelFamilyExecutionRequestV1 execution;
  std::function<void()> pause_exchange;
  std::function<void()> resume_exchange;
  std::function<void()> cleanup_exchange;
  std::function<ModelFamilyExecutionResultV1(
      const ModelFamilyExecutionRequestV1&,
      const DescriptorTuple&,
      std::uint64_t)> execute_correlated_provider;
};

struct ModelFamilyRelationalConsumerExecutionResultV1 {
  bool ok{false};
  DescriptorBatch output_batch;
  std::uint64_t rows_examined{0};
  std::uint64_t executed_physical_node_id{0};
  std::uint64_t causal_counter_id{0};
  std::string selected_implementation_uuid;
  PhysicalMgaStatementContext mga_statement_context;
  std::string security_receipt_uuid;
  std::string diagnostic_id;
  std::string detail;
};

struct ModelFamilyCompositionPublicationStateV1 {
  std::uint64_t current_selected_plan_generation{0};
  std::vector<std::uint64_t> current_catalog_generations;
  std::vector<std::uint64_t> current_descriptor_generations;
  std::vector<std::uint64_t> current_security_generations;
  std::vector<std::uint64_t> current_policy_generations;
  std::vector<std::uint64_t> current_resource_generations;
  std::vector<std::uint64_t> current_provider_generations;
  std::vector<std::uint64_t> current_capability_generations;
  std::vector<std::string> current_catalog_snapshot_uuids;
  std::vector<std::string> current_descriptor_snapshot_uuids;
  std::vector<std::string> current_security_context_uuids;
  std::vector<std::string> current_policy_snapshot_uuids;
  std::vector<std::string> current_resource_contract_uuids;
  std::vector<std::string> current_provider_uuids;
  std::vector<std::string> current_capability_uuids;
  PhysicalMgaStatementContext current_mga_statement_context;
  bool security_admitted{false};
};

using ModelFamilyRelationalConsumerExecuteCallbackV1 = std::function<
    ModelFamilyRelationalConsumerExecutionResultV1(
        const scratchbird::engine::optimizer::ModelFamilyRelationalConsumerV1&,
        const DescriptorBatch&,
        const DescriptorBatch&)>;

struct ModelFamilyCompositionExecutionRequestV1 {
  std::uint16_t abi_version{1};
  scratchbird::engine::optimizer::ModelFamilyDependencyCoordinatorResultV1
      admitted_plan;
  std::vector<ModelFamilyCompositionExecutionLegV1> legs;
  ModelFamilyRelationalConsumerExecuteCallbackV1 execute_relational_consumer;
  std::function<void(std::uint64_t)> cleanup_relational_consumer;
  ModelCancellationProbeV1 cancellation_requested;
  std::function<ModelFamilyCompositionPublicationStateV1()>
      revalidate_publication_state;
  scratchbird::core::memory::TempWorkspaceLifecycleManager*
      engine_temp_workspace{nullptr};
  scratchbird::core::memory::TempWorkspaceOwner spill_owner;
  std::string spill_operation_uuid;
  std::string spill_resource_contract_uuid;
  std::uint64_t spill_runtime_generation{1};
  std::uint64_t backpressure_high_watermark_rows{0};
  std::uint64_t backpressure_low_watermark_rows{0};
  std::uint64_t current_selected_plan_generation{0};
  PhysicalMgaStatementContext current_mga_statement_context;
  // The dispatch boundary may retain the per-database MGA inventory guard
  // across admission and execution. Provider callbacks that re-enter MGA
  // storage must then remain on that owning caller thread.
  bool engine_mga_inventory_guard_owned_by_caller{false};
  bool inject_failure_after_first_parallel_leg{false};
};

struct ModelFamilyCompositionExecutionResultV1 {
  bool accepted{false};
  bool execution_started{false};
  bool root_published{false};
  bool no_partial_root{true};
  bool cancellation_fanout_complete{false};
  bool failure_frozen{false};
  bool spill_reserved{false};
  bool spill_io_complete{false};
  bool backpressure_complete{false};
  bool cleanup_complete{false};
  std::uint64_t spill_reserved_bytes{0};
  std::uint64_t spill_cleanup_count{0};
  std::uint64_t provider_cleanup_count{0};
  std::uint64_t provider_entry_count{0};
  std::uint64_t observed_data_access_count{0};
  std::uint64_t exchange_cleanup_count{0};
  std::uint64_t relational_consumer_cleanup_count{0};
  std::uint64_t total_cleanup_count{0};
  std::uint64_t rows_received{0};
  std::uint64_t rows_published{0};
  std::uint64_t cells_received{0};
  std::uint64_t pause_count{0};
  std::uint64_t resume_count{0};
  std::uint64_t visible_dependency_rows{0};
  std::uint64_t right_evaluation_count{0};
  std::vector<std::uint16_t> launched_leg_ordinals;
  std::vector<std::uint16_t> unstarted_leg_ordinals;
  std::vector<std::uint16_t> started_leg_ordinals;
  std::vector<std::uint16_t> cancelled_leg_ordinals;
  std::vector<std::uint16_t> failed_leg_ordinals;
  std::vector<std::uint16_t> completed_leg_ordinals;
  std::vector<std::string> leg_terminal_diagnostic_ids;
  std::vector<std::uint16_t> started_exchange_ordinals;
  std::vector<std::uint64_t> started_relational_consumer_ids;
  std::vector<scratchbird::engine::optimizer::
                  ModelFamilyCoordinatorRuleReceiptV1>
      rule_receipts;
  DescriptorBatch root_output_batch;
  std::string root_publication_receipt_uuid;
  std::string spill_result_hash;
  std::string diagnostic_id;
  std::string detail;
};

ModelFamilyCompositionExecutionResultV1
ExecuteModelFamilyCompositionV1(
    const ModelFamilyCompositionExecutionRequestV1& request);

}  // namespace scratchbird::engine::executor
