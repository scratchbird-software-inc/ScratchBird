// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "extensibility/gpu_api.hpp"

#include "behavior_support/api_behavior_store.hpp"
#include "extensibility/extensibility_support.hpp"
#include "gpu_acceleration.hpp"
#include "runtime_capabilities.hpp"

#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {
namespace {

namespace gpu = scratchbird::engine::gpu_acceleration;

constexpr const char* kOperation = "extensibility.inspect_gpu_capability";

bool Contains(const std::string& value, const std::string& token) {
  return value.find(token) != std::string::npos;
}

bool StartsWith(const std::string& value, const std::string& prefix) {
  return value.rfind(prefix, 0) == 0;
}

bool HasOptionToken(const EngineApiRequest& request, const std::string& token) {
  for (const auto& option : request.option_envelopes) {
    if (Contains(option, token)) { return true; }
  }
  return false;
}

std::string OptionValue(const EngineApiRequest& request, const std::string& prefix) {
  for (const auto& option : request.option_envelopes) {
    if (StartsWith(option, prefix)) { return option.substr(prefix.size()); }
  }
  return {};
}

bool RequestsAuthorityBypass(const EngineApiRequest& request) {
  return HasOptionToken(request, "bypass_mga") ||
         HasOptionToken(request, "bypass_sblr") ||
         HasOptionToken(request, "bypass_catalog") ||
         HasOptionToken(request, "bypass_uuid_catalog") ||
         HasOptionToken(request, "bypass_security") ||
         HasOptionToken(request, "bypass_transaction") ||
         HasOptionToken(request, "gpu_authority");
}

bool RequestsForbiddenGpuInput(const EngineApiRequest& request) {
  return HasOptionToken(request, "raw_sql") ||
         HasOptionToken(request, "donor_text") ||
         HasOptionToken(request, "parser_ast") ||
         HasOptionToken(request, "client_protocol_frame");
}

bool RequestsUnsafeGpuStorageAccess(const EngineApiRequest& request) {
  return HasOptionToken(request, "direct_storage") ||
         HasOptionToken(request, "direct_catalog_mutation") ||
         HasOptionToken(request, "raw_page") ||
         HasOptionToken(request, "direct_page_access") ||
         HasOptionToken(request, "direct_catalog_access");
}

bool RequestsGpuExecution(const EngineApiRequest& request) {
  return HasOptionToken(request, "enable_gpu_execution") ||
         HasOptionToken(request, "planner_select_gpu") ||
         HasOptionToken(request, "gpu_profile:active") ||
         HasOptionToken(request, "execute_on_gpu");
}

std::string RequestedWorkload(const EngineApiRequest& request) {
  std::string workload = OptionValue(request, "workload:");
  if (workload.empty()) { workload = OptionValue(request, "profile_workload:"); }
  return workload.empty() ? "inspect" : workload;
}

struct GpuProviderState {
  bool available = false;
  std::string provider = "missing";
  std::vector<std::string> rows;
};

GpuProviderState DetectGpuProvider(const EngineApiRequest& request) {
  GpuProviderState state;
  const auto simulated = OptionValue(request, "simulate_gpu_provider:");
  if (!simulated.empty()) {
    state.available = true;
    state.provider = simulated;
    state.rows.push_back(simulated + ":present:simulated_for_profile_probe");
    return state;
  }
  const auto manifest = scratchbird::core::platform::DetectRuntimeCapabilities();
  for (const auto& capability : manifest.capabilities) {
    if (capability.key == "gpu.cuda" || capability.key == "gpu.hip" || capability.key == "gpu.opencl") {
      const bool present = capability.state == scratchbird::core::platform::CapabilityState::present;
      if (present && !state.available) {
        state.available = true;
        state.provider = capability.key;
      }
      state.rows.push_back(capability.key + ":" +
                           scratchbird::core::platform::CapabilityStateName(capability.state) + ":" +
                           capability.provider);
    }
  }
  return state;
}

std::vector<double> ParseDoubleList(const std::string& encoded) {
  std::vector<double> values;
  std::stringstream in(encoded);
  std::string item;
  while (std::getline(in, item, ',')) {
    if (item.empty()) {
      continue;
    }
    char* end = nullptr;
    const double value = std::strtod(item.c_str(), &end);
    if (end != item.c_str()) {
      values.push_back(value);
    }
  }
  return values;
}

double ParseDoubleOption(const EngineApiRequest& request,
                         const std::string& prefix,
                         double fallback) {
  const auto value = OptionValue(request, prefix);
  if (value.empty()) {
    return fallback;
  }
  char* end = nullptr;
  const double parsed = std::strtod(value.c_str(), &end);
  return end == value.c_str() ? fallback : parsed;
}

std::uint64_t ParseU64Option(const EngineApiRequest& request,
                             const std::string& prefix,
                             std::uint64_t fallback) {
  const auto value = OptionValue(request, prefix);
  if (value.empty()) {
    return fallback;
  }
  char* end = nullptr;
  const unsigned long long parsed = std::strtoull(value.c_str(), &end, 10);
  return end == value.c_str() ? fallback : static_cast<std::uint64_t>(parsed);
}

gpu::GpuPolicyProfile RequestedPolicyProfile(const EngineApiRequest& request,
                                             bool execution_requested) {
  auto profile = OptionValue(request, "gpu_profile:");
  if (profile.empty()) {
    profile = OptionValue(request, "gpu_accel_profile:");
  }
  if (profile.empty() && execution_requested) {
    return gpu::GpuPolicyProfile::required_for_declared_workload;
  }
  return gpu::ParseGpuPolicyProfile(profile);
}

gpu::GpuAccelerationRequest BuildGpuAccelerationRequest(const EngineApiRequest& request,
                                                        const GpuProviderState& provider,
                                                        const std::string& workload,
                                                        bool execution_requested) {
  gpu::GpuAccelerationRequest gpu_request;
  gpu_request.execution_requested = execution_requested;
  gpu_request.provider_available = provider.available;
  gpu_request.provider = provider.provider;
  gpu_request.backend = provider.provider;
  gpu_request.security_context_present = request.context.security_context_present;
  gpu_request.cluster_authority_available = request.context.cluster_authority_available;
  gpu_request.cluster_dispatch_requested = EngineExtensionRequestsClusterAuthority(request);
  gpu_request.authority_bypass_requested = RequestsAuthorityBypass(request);
  gpu_request.direct_page_or_catalog_access_requested = RequestsUnsafeGpuStorageAccess(request);
  gpu_request.raw_client_or_parser_input_requested = RequestsForbiddenGpuInput(request);
  gpu_request.workload = workload;
  gpu_request.policy_profile = RequestedPolicyProfile(request, execution_requested);
  gpu_request.batch_operation = gpu::ParseGpuBatchOperation(OptionValue(request, "batch_operation:"));
  gpu_request.values = ParseDoubleList(OptionValue(request, "batch_values:"));
  gpu_request.rhs_values = ParseDoubleList(OptionValue(request, "batch_rhs:"));
  gpu_request.scale = ParseDoubleOption(request, "batch_scale:", 1.0);
  gpu_request.device_memory_budget_bytes = ParseU64Option(request, "device_memory_budget_bytes:", gpu_request.device_memory_budget_bytes);
  gpu_request.pinned_host_memory_budget_bytes = ParseU64Option(request, "pinned_host_memory_budget_bytes:", gpu_request.pinned_host_memory_budget_bytes);
  gpu_request.approximate_declared = HasOptionToken(request, "gpu_numeric:approximate_declared") ||
                                    HasOptionToken(request, "exactness:approximate_declared");
  return gpu_request;
}

EngineApiDiagnostic GpuDiagnostic(const std::string& code, const std::string& detail) {
  return MakeEngineApiDiagnostic(code, "engine.extensibility.gpu", detail, true);
}

EngineInspectGpuCapabilityResult GpuFailure(const EngineRequestContext& context,
                                            const std::string& code,
                                            const std::string& detail) {
  return MakeApiBehaviorDiagnostic<EngineInspectGpuCapabilityResult>(
      context,
      kOperation,
      GpuDiagnostic(code, detail));
}

void AddGpuRows(EngineApiResult* result,
                const GpuProviderState& provider,
                const gpu::GpuAccelerationResult& gpu_result,
                const std::string& workload,
                const std::string& activation_state,
                const std::string& planner_state) {
  AddApiBehaviorRow(result, {{"gpu_execution_authority", "accelerator_only_cpu_engine_plan_remains_authority"},
                             {"capability_probe", "local_runtime_capability_manifest"},
                             {"implicit_execution", "false"},
                             {"planner_may_select_gpu", planner_state},
                             {"correctness_authority", "cpu_equivalent_engine_plan"},
                             {"requested_workload", workload},
                             {"provider", provider.provider},
                             {"activation_state", activation_state},
                             {"gpu_effective_path", gpu::GpuEffectivePathName(gpu_result.effective_path)},
                             {"gpu_policy_cache_key", gpu_result.cache_key_hash},
                             {"gpu_transfer_bytes", std::to_string(gpu_result.transfer_bytes)},
                             {"gpu_memory_bytes", std::to_string(gpu_result.device_memory_bytes)},
                             {"gpu_batch_scalar_result", std::to_string(gpu_result.scalar_value)}});
  if (!gpu_result.output_values.empty()) {
    std::string encoded;
    for (double value : gpu_result.output_values) {
      if (!encoded.empty()) {
        encoded += ",";
      }
      encoded += std::to_string(value);
    }
    AddApiBehaviorRow(result, {{"gpu_batch_output_values", encoded}});
  }
  for (const auto& row : provider.rows) {
    AddApiBehaviorRow(result, {{"gpu_provider_state", row}});
  }
}

void AddGpuEvidence(EngineApiResult* result,
                    const std::string& behavior,
                    const std::string& activation_state,
                    const std::string& provider) {
  AddApiBehaviorEvidence(result, "gpu_capability", "inspected");
  AddApiBehaviorEvidence(result, "gpu_runtime", activation_state);
  AddApiBehaviorEvidence(result, "gpu_provider", provider.empty() ? "missing" : provider);
  AddApiBehaviorEvidence(result, "gpu_metrics", "capability_event_emitted");
  AddApiBehaviorEvidence(result, "execution_boundary", "gpu_never_transaction_security_visibility_authority");
  AddEngineExtensionEvidence(result, "gpu", behavior);
}

std::string OperationIdOr(const EngineApiRequest& request, const std::string& fallback) {
  return request.operation_id.empty() ? fallback : request.operation_id;
}

std::string ResultShapeContract(const EngineApiRequest& request, const std::string& fallback) {
  const auto result_shape = OptionValue(request, "result_shape_contract:");
  return result_shape.empty() ? fallback : result_shape;
}

void AddGpuOperationResult(EngineApiResult* result,
                           const EngineApiRequest& request,
                           const std::string& operation_id,
                           const std::string& api_function,
                           const std::string& route_kind,
                           const std::string& result_shape) {
  AddApiBehaviorEvidence(result, "public_sbsql_operation", operation_id);
  AddApiBehaviorEvidence(result, "engine_api_function", api_function);
  AddApiBehaviorEvidence(result, "parser_executes_sql", "false");
  AddApiBehaviorEvidence(result, "cluster_provider_dispatch", "false");
  AddApiBehaviorEvidence(result, "private_cluster_execution", "false");
  AddApiBehaviorEvidence(result, "result_shape_contract", result_shape);
  AddApiBehaviorRow(result,
                    {{"operation_id", operation_id},
                     {"result_shape", result_shape},
                     {"route_kind", route_kind},
                     {"target_ref_kind", OptionValue(request, "target_ref_kind:")},
                     {"target_ref_visible", OptionValue(request, "target_ref:").empty() ? "false" : "true"},
                     {"security_epoch", std::to_string(request.context.security_epoch)},
                     {"resource_epoch", std::to_string(request.context.resource_epoch)}});
  result->result_shape.result_kind = result_shape;
}

}  // namespace

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_EXTENSIBILITY_GPU_API_BEHAVIOR
EngineInspectGpuCapabilityResult EngineInspectGpuCapability(const EngineInspectGpuCapabilityRequest& request) {
  if (EngineExtensionRequestsControl(request) && !request.context.security_context_present) {
    return EngineExtensionSecurityRequired<EngineInspectGpuCapabilityResult>(request, kOperation);
  }
  if (!request.context.cluster_authority_available && EngineExtensionRequestsClusterAuthority(request)) {
    return EngineExtensionClusterAuthorityUnavailable<EngineInspectGpuCapabilityResult>(request, kOperation);
  }
  const bool execution_requested = RequestsGpuExecution(request);
  const auto workload = RequestedWorkload(request);
  const auto provider = DetectGpuProvider(request);
  const auto gpu_request = BuildGpuAccelerationRequest(request, provider, workload, execution_requested);
  const auto gpu_result = gpu::EvaluateGpuAcceleration(gpu_request);

  if (!gpu_result.ok) {
    if (gpu_result.diagnostic_code == "GPU.SECURITY_CONTEXT_REQUIRED") {
      return EngineExtensionSecurityRequired<EngineInspectGpuCapabilityResult>(request, kOperation);
    }
    if (gpu_result.diagnostic_code == "GPU.CLUSTER_PLACEMENT_UNAVAILABLE") {
      return EngineExtensionClusterAuthorityUnavailable<EngineInspectGpuCapabilityResult>(request, kOperation);
    }
    if (gpu_result.diagnostic_code == "GPU.WORKLOAD_UNSUPPORTED") {
      return GpuFailure(request.context, "SB_ENGINE_API_GPU_WORKLOAD_UNSUPPORTED", gpu_result.diagnostic_detail);
    }
    if (gpu_result.diagnostic_code == "GPU.BACKEND_UNAVAILABLE") {
      return GpuFailure(request.context, "SB_ENGINE_API_GPU_PROVIDER_UNAVAILABLE", gpu_result.diagnostic_detail);
    }
    if (gpu_result.diagnostic_code == "GPU.AUTHORITY_BYPASS_REFUSED") {
      return GpuFailure(request.context, "SB_ENGINE_API_GPU_AUTHORITY_BYPASS_REFUSED", gpu_result.diagnostic_detail);
    }
    return GpuFailure(request.context, "SB_ENGINE_API_" + gpu_result.diagnostic_code, gpu_result.diagnostic_detail);
  }

  auto result = MakeApiBehaviorSuccess<EngineInspectGpuCapabilityResult>(request.context, kOperation);
  const std::string activation = gpu_result.activation_state;
  const std::string planner = gpu_result.planner_may_select_gpu ? "policy_checked_provider_available" :
                              (request.context.security_context_present ? "policy_checked_inactive" : "false");
  AddGpuRows(&result, provider, gpu_result, workload, activation, planner);
  AddGpuEvidence(&result,
                 execution_requested ? (gpu_result.fallback_used ? "cpu_fallback_accelerator_policy" : "profile_active_accelerator_only") :
                                       "inspect_only_no_implicit_execution",
                 activation,
                 provider.provider);
  for (const auto& [kind, id] : gpu_result.evidence) {
    AddApiBehaviorEvidence(&result, kind, id);
  }
  return result;
}

EngineControlGpuAccelerationResult EngineControlGpuAcceleration(
    const EngineControlGpuAccelerationRequest& request) {
  const std::string operation_id = OperationIdOr(request, "gpu.acceleration.control");
  if (!request.context.security_context_present) {
    return MakeApiBehaviorDiagnostic<EngineControlGpuAccelerationResult>(
        request.context,
        operation_id,
        GpuDiagnostic("SB_ENGINE_API_GPU_SECURITY_CONTEXT_REQUIRED",
                      "gpu_acceleration_control_requires_security_context"));
  }
  if (!request.context.cluster_authority_available &&
      EngineExtensionRequestsClusterAuthority(request)) {
    return MakeApiBehaviorDiagnostic<EngineControlGpuAccelerationResult>(
        request.context,
        operation_id,
        GpuDiagnostic("SB_ENGINE_API_GPU_CLUSTER_AUTHORITY_UNAVAILABLE",
                      "gpu_acceleration_control_does_not_use_private_cluster_dispatch"));
  }
  auto result =
      MakeApiBehaviorSuccess<EngineControlGpuAccelerationResult>(request.context, operation_id);
  AddGpuOperationResult(&result,
                        request,
                        operation_id,
                        "EngineControlGpuAcceleration",
                        "gpu_acceleration_control",
                        ResultShapeContract(request, "rs.acceleration.control.v1"));
  return result;
}

EngineInspectGpuAccelerationResult EngineInspectGpuAcceleration(
    const EngineInspectGpuAccelerationRequest& request) {
  const std::string operation_id = OperationIdOr(request, "gpu.acceleration.inspect");
  auto result =
      MakeApiBehaviorSuccess<EngineInspectGpuAccelerationResult>(request.context, operation_id);
  AddGpuOperationResult(&result,
                        request,
                        operation_id,
                        "EngineInspectGpuAcceleration",
                        "gpu_acceleration_inspect",
                        ResultShapeContract(request, "rs.show.gpu.v1"));
  return result;
}

}  // namespace scratchbird::engine::internal_api
