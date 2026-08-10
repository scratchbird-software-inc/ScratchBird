// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "descriptor_value_runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::executor {

inline constexpr const char* kModelTypedExchangeInvalid =
    "SB_MODEL_TYPED_EXCHANGE_INVALID_V1";
inline constexpr const char* kModelDocumentMissingBindingRefused =
    "SB_MODEL_DOCUMENT_MISSING_BINDING_REFUSED_V1";
inline constexpr const char* kModelMgaContextMismatch =
    "SB_MODEL_MGA_CONTEXT_MISMATCH_V1";

struct ModelPropertyDescriptorV1 {
  std::uint16_t abi_version{1};
  std::string property_descriptor_id{"SB_MODEL_PROPERTY_DESCRIPTOR_V1"};
  std::string property_uuid;
  std::string ordering_id{"fixture_order"};
  std::string partitioning_id{"single_local_partition"};
  std::string uniqueness_id{"document_uuid"};
  bool exact{true};
  bool residual_recheck_complete{false};
  bool base_row_mga_recheck_complete{false};
  bool security_recheck_complete{false};
};

struct ModelProviderRowIdentityV1 {
  std::string document_uuid;
  std::string row_uuid;
  std::string vertex_uuid;
  std::string edge_uuid;
  std::string path_uuid;
  std::uint64_t graph_depth{0};
};

struct ModelSourceInputDescriptorV1 {
  std::uint16_t abi_version{1};
  std::string input_descriptor_id{"SB_MODEL_SOURCE_INPUT_DESCRIPTOR_V1"};
  std::string family_id;
  std::string operation_id;
  std::string object_uuid;
  std::uint64_t physical_node_id{0};
  std::string selected_alternative_uuid;
  std::string capability_uuid;
  std::string provider_uuid;
  std::uint64_t provider_generation{0};
  std::string result_handle_uuid;
  std::uint64_t causal_counter_id{0};
  std::vector<std::uint32_t> output_descriptor_ids;
  PhysicalMgaStatementContext mga_statement_context;
  std::string catalog_epoch_uuid;
  std::string security_context_uuid;
  std::string policy_snapshot_uuid;
  std::string resource_contract_uuid;
  std::uint64_t catalog_generation{0};
  std::uint64_t descriptor_generation{0};
  std::uint64_t security_generation{0};
  std::uint64_t policy_generation{0};
  std::uint64_t resource_generation{0};
  std::size_t maximum_rows{0};
  std::size_t maximum_cells{0};
  std::uint64_t maximum_memory_bytes{0};
  bool parser_execution_authority_claimed{false};
  bool transaction_finality_authority_claimed{false};
};

struct ModelProviderBatchV1 {
  std::uint16_t abi_version{1};
  std::string provider_uuid;
  std::uint64_t provider_generation{0};
  std::string result_handle_uuid;
  std::uint64_t causal_counter_id{0};
  std::vector<std::uint32_t> output_descriptor_ids;
  std::vector<ModelProviderRowIdentityV1> ordered_row_identities;
  DescriptorBatch batch;
  ModelPropertyDescriptorV1 properties;
  PhysicalMgaStatementContext mga_statement_context;
  std::string security_receipt_uuid;
  bool residual_recheck_complete{false};
  bool base_row_mga_recheck_complete{false};
  bool security_recheck_complete{false};
  bool provider_visibility_authority_claimed{false};
  bool provider_finality_authority_claimed{false};
};

struct ModelSourceOutputDescriptorV1 {
  std::uint16_t abi_version{1};
  std::string output_descriptor_id{"SB_MODEL_SOURCE_OUTPUT_DESCRIPTOR_V1"};
  std::string family_id;
  std::string operation_id;
  std::string object_uuid;
  std::uint64_t physical_node_id{0};
  std::string selected_alternative_uuid;
  std::string capability_uuid;
  std::string provider_uuid;
  std::uint64_t provider_generation{0};
  std::string result_handle_uuid;
  std::uint64_t causal_counter_id{0};
  std::vector<std::uint32_t> output_descriptor_ids;
  std::vector<ModelProviderRowIdentityV1> ordered_row_identities;
  DescriptorBatch batch;
  ModelPropertyDescriptorV1 properties;
  PhysicalMgaStatementContext mga_statement_context;
  std::string security_receipt_uuid;
  bool exact_exchange_validated{false};
};

struct ModelExchangeResultV1 {
  bool accepted{false};
  bool root_publishable{false};
  ModelSourceOutputDescriptorV1 output;
  std::string diagnostic_id;
  std::string detail;
};

struct ModelInputValidationResultV1 {
  bool accepted{false};
  std::string diagnostic_id;
  std::string detail;
};

ModelInputValidationResultV1 ValidateModelFamilySourceInputV1(
    const ModelSourceInputDescriptorV1& input);

ModelExchangeResultV1 PublishModelFamilyExchangeV1(
    const ModelSourceInputDescriptorV1& input,
    const ModelProviderBatchV1& provider_batch);

}  // namespace scratchbird::engine::executor
