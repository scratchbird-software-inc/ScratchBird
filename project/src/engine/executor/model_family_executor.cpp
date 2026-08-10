// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "model_family_executor.hpp"

#include <cctype>
#include <utility>

namespace scratchbird::engine::executor {
namespace {

bool CanonicalUuid(const std::string_view value) {
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

}  // namespace

ModelFamilyExecutionResultV1 ExecuteModelFamilySourceV1(
    const ModelFamilyExecutionRequestV1& request) {
  // QOW-SOURCE-CES05-MODEL-FAMILY-EXECUTOR-V1
  ModelFamilyExecutionResultV1 result;
  const auto refuse = [&](const char* diagnostic, std::string detail) {
    result.accepted = false;
    result.root_published = false;
    result.diagnostic_id = diagnostic;
    result.detail = std::move(detail);
  };

  const auto input_validation = ValidateModelFamilySourceInputV1(request.input);
  if (!input_validation.accepted) {
    refuse(input_validation.diagnostic_id.c_str(), input_validation.detail);
    return result;
  }
  const bool graph_family = request.input.family_id == "graph";
  const bool key_value_family = request.input.family_id == "key_value";
  if (request.current_catalog_generation != request.input.catalog_generation) {
    refuse("SB_MODEL_CATALOG_GENERATION_STALE_V1",
           "model-family catalog generation changed before provider access");
    return result;
  }
  if (request.current_descriptor_generation !=
      request.input.descriptor_generation) {
    refuse(kModelTypedExchangeInvalid,
           "model-family descriptor generation changed before provider access");
    return result;
  }
  if (request.current_security_generation !=
          request.input.security_generation ||
      request.current_policy_generation != request.input.policy_generation ||
      request.current_resource_generation !=
          request.input.resource_generation) {
    refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
           "model-family authority generation changed before access");
    return result;
  }
  if (request.current_provider_generation !=
      request.input.provider_generation) {
    refuse("SB_MODEL_PROVIDER_GENERATION_STALE_V1",
           "model-family provider generation changed before access");
    return result;
  }

  if (request.abi_version != 1 || request.capability.abi_version != 1 ||
      request.capability.capability_descriptor_id !=
          "SB_MODEL_CAPABILITY_DESCRIPTOR_V1" ||
      request.capability.family_id != request.input.family_id ||
      (request.capability.family_id != "document" &&
       request.capability.family_id != "graph" &&
       request.capability.family_id != "key_value") ||
      request.capability.capability_uuid !=
          request.input.capability_uuid ||
      request.capability.provider_uuid != request.input.provider_uuid ||
      request.capability.provider_generation !=
          request.input.provider_generation ||
      !CanonicalUuid(request.capability.capability_uuid) ||
      !request.capability.local_scope || !request.capability.available ||
      !request.capability.exact ||
      !request.capability.cancellation_supported ||
      !request.capability.cleanup_supported ||
      !request.capability.residual_recheck_supported ||
      !request.capability.base_row_mga_recheck_supported ||
      !request.capability.security_recheck_supported ||
      request.capability.provider_visibility_authority_claimed ||
      request.capability.provider_finality_authority_claimed ||
      (key_value_family &&
       request.exact_fallback_selected !=
           request.input.exact_fallback_selected) ||
      !request.execute_provider || !request.cancellation_requested ||
      !request.cleanup_provider) {
    refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
           "model-family executor capability is unavailable or semantically invalid");
    return result;
  }
  if (request.exact_fallback_selected &&
      !request.capability.exact_collection_fallback_available) {
    refuse(graph_family
               ? "SB_MODEL_GRAPH_EXACT_FALLBACK_UNAVAILABLE_V1"
               : key_value_family
                     ? "SB_MODEL_KEY_VALUE_EXACT_FALLBACK_UNAVAILABLE_V1"
                     : "SB_MODEL_DOCUMENT_EXACT_FALLBACK_UNAVAILABLE_V1",
           "the exact model-family fallback is unavailable");
    return result;
  }
  if (!request.security_admitted) {
    refuse("SB_MODEL_SECURITY_ADMISSION_REFUSED_V1",
           "model-family provider access was not admitted by engine security");
    return result;
  }
  if (!PhysicalMgaStatementContextValid(
          request.current_mga_statement_context) ||
      !PhysicalMgaStatementContextEqual(
          request.input.mga_statement_context,
          request.current_mga_statement_context)) {
    refuse(kModelMgaContextMismatch,
           "current engine MGA statement context differs from the selected input");
    return result;
  }
  if (request.input.maximum_memory_bytes == 0 ||
      request.input.maximum_rows == 0 || request.input.maximum_cells == 0) {
    refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
           "model-family executor has no bounded resource admission");
    return result;
  }
  try {
    if (request.cancellation_requested()) {
      refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
             "model-family execution was cancelled before data access");
      return result;
    }
  } catch (...) {
    refuse("SB_MODEL_COORDINATOR_LEG_FAILED_V1",
           "model-family cancellation probe failed before data access");
    return result;
  }

  result.execution_started = true;
  bool cleanup_attempted = false;
  const auto cleanup_once = [&]() noexcept {
    if (cleanup_attempted) return result.cleanup_complete;
    cleanup_attempted = true;
    result.cleanup_count = 1;
    try {
      request.cleanup_provider();
      result.cleanup_complete = true;
    } catch (...) {
      result.cleanup_complete = false;
      result.accepted = false;
      result.root_published = false;
      result.diagnostic_id = "SB_MODEL_COORDINATOR_LEG_FAILED_V1";
      result.detail = "model-family provider cleanup failed";
    }
    return result.cleanup_complete;
  };

  if (request.fault_injected) {
    refuse("SB_MODEL_COORDINATOR_LEG_FAILED_V1",
           "model-family provider leg failed before exchange");
    cleanup_once();
    return result;
  }
  ModelProviderExecutionResultV1 provider;
  try {
    provider = request.execute_provider(request.input);
  } catch (...) {
    refuse("SB_MODEL_COORDINATOR_LEG_FAILED_V1",
           "model-family provider threw during execution");
    cleanup_once();
    return result;
  }
  result.data_access_observed = provider.data_access_observed;
  result.rows_examined = provider.rows_examined;
  if (!provider.ok) {
    refuse(provider.diagnostic_id.empty()
               ? "SB_MODEL_COORDINATOR_LEG_FAILED_V1"
               : provider.diagnostic_id.c_str(),
           provider.detail.empty() ? provider.diagnostic_id
                                   : std::move(provider.detail));
    cleanup_once();
    return result;
  }
  try {
    if (request.cancellation_requested()) {
      refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
             "model-family execution was cancelled after provider access");
      cleanup_once();
      return result;
    }
  } catch (...) {
    refuse("SB_MODEL_COORDINATOR_LEG_FAILED_V1",
           "model-family cancellation probe failed after provider access");
    cleanup_once();
    return result;
  }
  auto exchange = PublishModelFamilyExchangeV1(request.input,
                                                provider.provider_batch);
  if (!exchange.accepted) {
    refuse(exchange.diagnostic_id.c_str(), std::move(exchange.detail));
    cleanup_once();
    return result;
  }
  try {
    if (request.cancellation_requested()) {
      refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
             "model-family execution was cancelled before root publication");
      cleanup_once();
      return result;
    }
  } catch (...) {
    refuse("SB_MODEL_COORDINATOR_LEG_FAILED_V1",
           "model-family cancellation probe failed before root publication");
    cleanup_once();
    return result;
  }

  result.accepted = true;
  result.root_published = exchange.root_publishable;
  result.output = std::move(exchange.output);
  if (!cleanup_once()) {
    result.accepted = false;
    result.root_published = false;
  }
  return result;
}

}  // namespace scratchbird::engine::executor
