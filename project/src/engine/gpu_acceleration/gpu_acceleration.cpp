// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "gpu_acceleration.hpp"

#include "metric_registry.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace scratchbird::engine::gpu_acceleration {
namespace {

using scratchbird::core::metrics::DefaultMetricRegistry;
using scratchbird::core::metrics::MetricLabelSet;
namespace platform = scratchbird::core::platform;

constexpr const char* kProducer = "gpu_acceleration";

std::string BoolName(bool value) {
  return value ? "true" : "false";
}

std::string ReasonOrNone(const std::string& reason) {
  return reason.empty() ? "none" : reason;
}

MetricLabelSet Labels(const GpuAccelerationRequest& request,
                      const std::string& result,
                      const std::string& reason = {}) {
  return {{"provider_family", request.provider.empty() ? "missing" : request.provider},
          {"workload_class", request.workload.empty() ? "inspect" : request.workload},
          {"result", result},
          {"reason", ReasonOrNone(reason)}};
}

void EmitCommonMetrics(const GpuAccelerationRequest& request,
                       const GpuAccelerationResult& result) {
  auto& registry = DefaultMetricRegistry();
  const std::string result_label = result.ok ? "ok" : "refused";
  (void)registry.SetGauge("sb_gpu_device_available",
                          Labels(request, request.provider_available ? "present" : "missing"),
                          request.provider_available ? 1.0 : 0.0,
                          kProducer);
  if (request.execution_requested) {
    (void)registry.IncrementCounter("sb_gpu_execution_total",
                                    Labels(request, result_label, result.diagnostic_code),
                                    1.0,
                                    kProducer);
  }
  if (result.fallback_used) {
    (void)registry.IncrementCounter("sb_gpu_fallback_total",
                                    Labels(request, "fallback", result.diagnostic_code.empty() ? "policy_optional" : result.diagnostic_code),
                                    1.0,
                                    kProducer);
  }
  if (result.transfer_bytes != 0) {
    (void)registry.IncrementCounter("sb_gpu_transfer_bytes_total",
                                    Labels(request, result_label, "host_device_batch_frame"),
                                    static_cast<double>(result.transfer_bytes),
                                    kProducer);
  }
  (void)registry.SetGauge("sb_gpu_memory_bytes",
                          Labels(request, result_label, result.diagnostic_code),
                          static_cast<double>(result.device_memory_bytes),
                          kProducer);
}

std::string CacheKeyFor(const GpuAccelerationRequest& request) {
  std::ostringstream out;
  out << "gpu-v1|profile=" << GpuPolicyProfileName(request.policy_profile)
      << "|workload=" << request.workload
      << "|provider=" << request.provider
      << "|backend=" << request.backend
      << "|operation=" << GpuBatchOperationName(request.batch_operation)
      << "|deterministic=" << BoolName(request.deterministic_equivalence_required)
      << "|approximate=" << BoolName(request.approximate_declared)
      << "|value_count=" << request.values.size()
      << "|rhs_count=" << request.rhs_values.size()
      << "|scale=" << std::setprecision(17) << request.scale;
  return out.str();
}

GpuAccelerationResult Refuse(const GpuAccelerationRequest& request,
                             std::string code,
                             std::string detail) {
  GpuAccelerationResult result;
  result.ok = false;
  result.diagnostic_code = std::move(code);
  result.diagnostic_detail = std::move(detail);
  result.effective_path = GpuEffectivePath::refused;
  result.activation_state = "refused";
  result.cache_key_hash = CacheKeyFor(request);
  result.evidence.push_back({"gpu_refusal", result.diagnostic_code});
  EmitCommonMetrics(request, result);
  return result;
}

bool ExactWorkloadRequiresCpu(const GpuAccelerationRequest& request) {
  return request.deterministic_equivalence_required &&
         !request.approximate_declared &&
         (request.workload == "aggregate" ||
          request.workload == "columnar_scan" ||
          request.workload == "sort");
}

std::uint64_t BatchBytes(const GpuAccelerationRequest& request) {
  const std::uint64_t lhs = static_cast<std::uint64_t>(request.values.size() * sizeof(double));
  const std::uint64_t rhs = static_cast<std::uint64_t>(request.rhs_values.size() * sizeof(double));
  return lhs + rhs;
}

bool BatchFitsBudget(const GpuAccelerationRequest& request) {
  const std::uint64_t bytes = BatchBytes(request);
  return bytes <= request.device_memory_budget_bytes && bytes <= request.pinned_host_memory_budget_bytes;
}

void ExecuteReferenceBatch(const GpuAccelerationRequest& request,
                           GpuAccelerationResult* result) {
  if (result == nullptr) {
    return;
  }
  switch (request.batch_operation) {
    case GpuBatchOperation::none:
      break;
    case GpuBatchOperation::filter_positive:
      for (double value : request.values) {
        if (value > 0.0) {
          result->output_values.push_back(value);
        }
      }
      break;
    case GpuBatchOperation::project_scale:
      result->output_values.reserve(request.values.size());
      for (double value : request.values) {
        result->output_values.push_back(value * request.scale);
      }
      break;
    case GpuBatchOperation::aggregate_sum:
      result->scalar_value = 0.0;
      for (double value : request.values) {
        result->scalar_value += value;
      }
      break;
    case GpuBatchOperation::vector_dot:
      result->scalar_value = 0.0;
      for (std::size_t i = 0; i < request.values.size(); ++i) {
        result->scalar_value += request.values[i] * request.rhs_values[i];
      }
      break;
  }
}

void AppendCompatibilityEvidence(
    GpuAccelerationResult* result,
    const platform::RuntimeCompatibilityResult& compatibility) {
  for (const auto& item : compatibility.evidence) {
    result->evidence.push_back({"runtime_compatibility", item});
  }
}

platform::RuntimeCompatibilityDescriptor GpuRuntimeCompatibility(
    const GpuAccelerationRequest& request) {
  auto descriptor = request.runtime_compatibility;
  if (descriptor.route_id.empty()) {
    descriptor =
        platform::CurrentRuntimeCompatibilityDescriptor("engine.gpu_acceleration");
  }
  descriptor.route_id =
      descriptor.route_id.empty() ? "engine.gpu_acceleration" : descriptor.route_id;
  descriptor.source_component = "engine.gpu_acceleration";
  descriptor.accelerator_requested = request.execution_requested;
  descriptor.deterministic_scalar_fallback_available = true;
  if (descriptor.provider_accelerator_capabilities.empty() &&
      request.provider_available) {
    descriptor.provider_accelerator_capabilities.push_back(request.provider);
    descriptor.provider_accelerator_capabilities.push_back("deterministic_gpu_batch");
  }
  return descriptor;
}

}  // namespace

const char* GpuPolicyProfileName(GpuPolicyProfile profile) {
  switch (profile) {
    case GpuPolicyProfile::disabled: return "gpu_accel.disabled";
    case GpuPolicyProfile::optional_batch: return "gpu_accel.optional_batch";
    case GpuPolicyProfile::required_for_declared_workload: return "gpu_accel.required_for_declared_workload";
    case GpuPolicyProfile::cluster_optional: return "gpu_accel.cluster_optional";
    case GpuPolicyProfile::cluster_required: return "gpu_accel.cluster_required";
    case GpuPolicyProfile::dev_kernel_debug: return "gpu_accel.dev_kernel_debug";
  }
  return "gpu_accel.unknown";
}

const char* GpuEffectivePathName(GpuEffectivePath path) {
  switch (path) {
    case GpuEffectivePath::inspect_only: return "inspect_only";
    case GpuEffectivePath::cpu_fallback: return "cpu_fallback";
    case GpuEffectivePath::gpu_provider_admitted: return "gpu_provider_admitted";
    case GpuEffectivePath::refused: return "refused";
  }
  return "unknown";
}

const char* GpuBatchOperationName(GpuBatchOperation operation) {
  switch (operation) {
    case GpuBatchOperation::none: return "none";
    case GpuBatchOperation::filter_positive: return "filter_positive";
    case GpuBatchOperation::project_scale: return "project_scale";
    case GpuBatchOperation::aggregate_sum: return "aggregate_sum";
    case GpuBatchOperation::vector_dot: return "vector_dot";
  }
  return "unknown";
}

GpuPolicyProfile ParseGpuPolicyProfile(const std::string& profile) {
  if (profile == "gpu_accel.optional_batch" || profile == "optional_batch") {
    return GpuPolicyProfile::optional_batch;
  }
  if (profile == "gpu_accel.required_for_declared_workload" ||
      profile == "required_for_declared_workload" ||
      profile == "active") {
    return GpuPolicyProfile::required_for_declared_workload;
  }
  if (profile == "gpu_accel.cluster_optional" || profile == "cluster_optional") {
    return GpuPolicyProfile::cluster_optional;
  }
  if (profile == "gpu_accel.cluster_required" || profile == "cluster_required") {
    return GpuPolicyProfile::cluster_required;
  }
  if (profile == "gpu_accel.dev_kernel_debug" || profile == "dev_kernel_debug") {
    return GpuPolicyProfile::dev_kernel_debug;
  }
  return GpuPolicyProfile::disabled;
}

GpuBatchOperation ParseGpuBatchOperation(const std::string& operation) {
  if (operation == "filter_positive") { return GpuBatchOperation::filter_positive; }
  if (operation == "project_scale") { return GpuBatchOperation::project_scale; }
  if (operation == "aggregate_sum") { return GpuBatchOperation::aggregate_sum; }
  if (operation == "vector_dot") { return GpuBatchOperation::vector_dot; }
  return GpuBatchOperation::none;
}

bool GpuWorkloadSupported(const std::string& workload) {
  return workload == "vector" ||
         workload == "search" ||
         workload == "columnar_scan" ||
         workload == "aggregate" ||
         workload == "sort" ||
         workload == "index_build" ||
         workload == "timeseries_transform" ||
         workload == "compression_transform" ||
         workload == "graph_batch";
}

GpuAccelerationResult EvaluateGpuAcceleration(const GpuAccelerationRequest& request) {
  if (request.authority_bypass_requested) {
    return Refuse(request,
                  "GPU.AUTHORITY_BYPASS_REFUSED",
                  "gpu_acceleration_cannot_be_transaction_security_visibility_catalog_mga_or_cluster_authority");
  }
  if (request.direct_page_or_catalog_access_requested) {
    return Refuse(request,
                  "GPU.UNSAFE_PAGE_ACCESS_REFUSED",
                  "gpu_initial_profile_requires_materialized_authorized_batches");
  }
  if (request.raw_client_or_parser_input_requested) {
    return Refuse(request,
                  "GPU.SBLR_ONLY_KERNELS_REQUIRED",
                  "gpu_kernels_must_originate_from_validated_sblr_or_engine_owned_internal_kernels");
  }
  if (!request.cluster_authority_available &&
      (request.cluster_dispatch_requested ||
       request.policy_profile == GpuPolicyProfile::cluster_optional ||
       request.policy_profile == GpuPolicyProfile::cluster_required)) {
    return Refuse(request,
                  "GPU.CLUSTER_PLACEMENT_UNAVAILABLE",
                  "cluster_gpu_dispatch_requires_cluster_authority");
  }
  if (!request.execution_requested) {
    GpuAccelerationResult result;
    result.ok = true;
    result.effective_path = GpuEffectivePath::inspect_only;
    result.activation_state = "inspect_only";
    result.cache_key_hash = CacheKeyFor(request);
    result.device_memory_bytes = 0;
    result.evidence.push_back({"gpu_capability", "inspected"});
    result.evidence.push_back({"gpu_boundary", "inspect_only_no_implicit_execution"});
    const auto compatibility = platform::NegotiateRuntimeCompatibility(
        GpuRuntimeCompatibility(request));
    AppendCompatibilityEvidence(&result, compatibility);
    EmitCommonMetrics(request, result);
    return result;
  }
  if (!request.security_context_present) {
    return Refuse(request,
                  "GPU.SECURITY_CONTEXT_REQUIRED",
                  "gpu_control_requires_engine_security_context");
  }
  if (request.policy_profile == GpuPolicyProfile::disabled) {
    return Refuse(request, "GPU.DISABLED_BY_POLICY", "gpu_execution_disabled_by_policy");
  }
  if (!GpuWorkloadSupported(request.workload)) {
    return Refuse(request, "GPU.WORKLOAD_UNSUPPORTED", "gpu_workload_not_supported:" + request.workload);
  }
  if (request.batch_operation == GpuBatchOperation::vector_dot &&
      request.values.size() != request.rhs_values.size()) {
    return Refuse(request, "GPU.INVALID_ACCELERATION_POLICY", "vector_dot_requires_equal_length_vectors");
  }
  if (!BatchFitsBudget(request)) {
    return Refuse(request, "GPU.DEVICE_MEMORY_POLICY_VIOLATION", "materialized_batch_exceeds_gpu_memory_budget");
  }
  const bool optional_profile =
      request.policy_profile == GpuPolicyProfile::optional_batch ||
      request.policy_profile == GpuPolicyProfile::cluster_optional ||
      request.policy_profile == GpuPolicyProfile::dev_kernel_debug;
  if (!request.provider_available && !optional_profile) {
    return Refuse(request, "GPU.BACKEND_UNAVAILABLE", "gpu_profile_requires_available_provider");
  }
  if (ExactWorkloadRequiresCpu(request) && !optional_profile) {
    return Refuse(request, "GPU.DETERMINISM_NOT_PROVEN", "exact_workload_requires_cpu_equivalence_proof");
  }
  const auto compatibility = platform::NegotiateRuntimeCompatibility(
      GpuRuntimeCompatibility(request));
  if (compatibility.action == platform::RuntimeCompatibilityAction::fail_closed) {
    auto result = Refuse(request,
                         "GPU.RUNTIME_COMPATIBILITY_REFUSED",
                         compatibility.diagnostic_code);
    AppendCompatibilityEvidence(&result, compatibility);
    return result;
  }

  GpuAccelerationResult result;
  result.ok = true;
  result.fallback_used =
      !request.provider_available || ExactWorkloadRequiresCpu(request) ||
      compatibility.action ==
          platform::RuntimeCompatibilityAction::exact_scalar_fallback;
  result.effective_path = result.fallback_used ? GpuEffectivePath::cpu_fallback : GpuEffectivePath::gpu_provider_admitted;
  result.planner_may_select_gpu = request.provider_available && !result.fallback_used;
  result.activation_state = result.fallback_used ? "cpu_fallback_by_policy" : "profile_active";
  result.cache_key_hash = CacheKeyFor(request);
  result.transfer_bytes = BatchBytes(request);
  result.device_memory_bytes = result.transfer_bytes;
  ExecuteReferenceBatch(request, &result);
  result.evidence.push_back({"gpu_execution", GpuEffectivePathName(result.effective_path)});
  result.evidence.push_back({"gpu_cache_key", result.cache_key_hash});
  result.evidence.push_back({"gpu_cpu_equivalence", "reference_batch_result"});
  AppendCompatibilityEvidence(&result, compatibility);
  if (result.fallback_used) {
    result.diagnostic_code =
        compatibility.action ==
                platform::RuntimeCompatibilityAction::exact_scalar_fallback
            ? "GPU.RUNTIME_COMPATIBILITY_FALLBACK"
            : "GPU.FALLBACK_USED";
    result.diagnostic_detail =
        compatibility.action ==
                platform::RuntimeCompatibilityAction::exact_scalar_fallback
            ? compatibility.diagnostic_code
            : "cpu_reference_path_used_by_gpu_policy";
  }
  EmitCommonMetrics(request, result);
  return result;
}

}  // namespace scratchbird::engine::gpu_acceleration
