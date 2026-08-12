// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "model_family_exchange.hpp"

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
  PhysicalMgaStatementContext current_mga_statement_context;
};

struct ModelFamilyExecutionResultV1 {
  bool accepted{false};
  bool execution_started{false};
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

}  // namespace scratchbird::engine::executor
