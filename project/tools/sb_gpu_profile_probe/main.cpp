// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "extensibility/gpu_api.hpp"

#include <iostream>
#include <string>

using namespace scratchbird::engine::internal_api;

namespace {

EngineRequestContext Context(bool secure) {
  EngineRequestContext context;
  context.trust_mode = EngineTrustMode::embedded_in_process;
  context.security_context_present = secure;
  context.request_id = secure ? "gpu-profile-secure" : "gpu-profile-open";
  context.database_path = "/tmp/sb_gpu_profile_probe.sbdb";
  context.database_uuid.canonical = "00000000-0000-7000-8000-000000001801";
  context.session_uuid.canonical = secure ? "00000000-0000-7000-8000-000000001802" : "00000000-0000-7000-8000-000000001803";
  return context;
}

bool HasDiagnosticCode(const EngineApiResult& result, const std::string& code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) { return true; }
  }
  return false;
}

bool HasEvidence(const EngineApiResult& result, const std::string& kind, const std::string& id = {}) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && (id.empty() || evidence.evidence_id == id)) { return true; }
  }
  return false;
}

bool RowFieldEquals(const EngineApiResult& result, const std::string& field_name, const std::string& expected) {
  for (const auto& row : result.result_shape.rows) {
    for (const auto& field : row.fields) {
      if (field.first == field_name && field.second.encoded_value == expected) { return true; }
    }
  }
  return false;
}

EngineInspectGpuCapabilityRequest EnableRequest(bool secure, const std::string& workload) {
  EngineInspectGpuCapabilityRequest request;
  request.context = Context(secure);
  request.option_envelopes.push_back("enable_gpu_execution:true");
  request.option_envelopes.push_back("workload:" + workload);
  return request;
}

void PrintBool(const std::string& name, bool value, bool comma) {
  std::cout << "  \"" << name << "\": " << (value ? "true" : "false") << (comma ? "," : "") << "\n";
}

}  // namespace

int main() {
  EngineInspectGpuCapabilityRequest inspect;
  inspect.context = Context(false);
  const auto inspect_result = EngineInspectGpuCapability(inspect);

  const auto insecure_control_result = EngineInspectGpuCapability(EnableRequest(false, "vector"));

  auto unavailable = EnableRequest(true, "vector");
  const auto unavailable_result = EngineInspectGpuCapability(unavailable);

  auto unsupported = EnableRequest(true, "oltp_row_update");
  unsupported.option_envelopes.push_back("simulate_gpu_provider:cuda");
  const auto unsupported_result = EngineInspectGpuCapability(unsupported);

  auto bypass = EnableRequest(true, "vector");
  bypass.option_envelopes.push_back("simulate_gpu_provider:cuda");
  bypass.option_envelopes.push_back("gpu_authority:true");
  const auto bypass_result = EngineInspectGpuCapability(bypass);

  auto active = EnableRequest(true, "vector");
  active.option_envelopes.push_back("simulate_gpu_provider:cuda");
  const auto active_result = EngineInspectGpuCapability(active);

  auto optional_fallback = EnableRequest(true, "aggregate");
  optional_fallback.option_envelopes.push_back("gpu_profile:optional_batch");
  optional_fallback.option_envelopes.push_back("batch_operation:aggregate_sum");
  optional_fallback.option_envelopes.push_back("batch_values:1,2,3");
  const auto optional_fallback_result = EngineInspectGpuCapability(optional_fallback);

  auto provider_batch = EnableRequest(true, "vector");
  provider_batch.option_envelopes.push_back("simulate_gpu_provider:cuda");
  provider_batch.option_envelopes.push_back("batch_operation:vector_dot");
  provider_batch.option_envelopes.push_back("batch_values:1,2,3");
  provider_batch.option_envelopes.push_back("batch_rhs:4,5,6");
  const auto provider_batch_result = EngineInspectGpuCapability(provider_batch);

  auto budget = EnableRequest(true, "vector");
  budget.option_envelopes.push_back("simulate_gpu_provider:cuda");
  budget.option_envelopes.push_back("batch_operation:vector_dot");
  budget.option_envelopes.push_back("batch_values:1,2,3");
  budget.option_envelopes.push_back("batch_rhs:4,5,6");
  budget.option_envelopes.push_back("device_memory_budget_bytes:8");
  const auto budget_result = EngineInspectGpuCapability(budget);

  auto cluster = EnableRequest(true, "vector");
  cluster.option_envelopes.push_back("simulate_gpu_provider:cuda");
  cluster.option_envelopes.push_back("cluster_gpu_dispatch:true");
  const auto cluster_result = EngineInspectGpuCapability(cluster);

  const bool inspect_ok = inspect_result.ok && HasEvidence(inspect_result, "extension_behavior", "inspect_only_no_implicit_execution") && RowFieldEquals(inspect_result, "implicit_execution", "false");
  const bool insecure_denied = !insecure_control_result.ok && HasDiagnosticCode(insecure_control_result, "SB_ENGINE_API_SECURITY_CONTEXT_REQUIRED");
  const bool unavailable_denied = !unavailable_result.ok && HasDiagnosticCode(unavailable_result, "SB_ENGINE_API_GPU_PROVIDER_UNAVAILABLE");
  const bool unsupported_denied = !unsupported_result.ok && HasDiagnosticCode(unsupported_result, "SB_ENGINE_API_GPU_WORKLOAD_UNSUPPORTED");
  const bool bypass_denied = !bypass_result.ok && HasDiagnosticCode(bypass_result, "SB_ENGINE_API_GPU_AUTHORITY_BYPASS_REFUSED");
  const bool active_ok = active_result.ok && HasEvidence(active_result, "extension_behavior", "profile_active_accelerator_only") && RowFieldEquals(active_result, "planner_may_select_gpu", "policy_checked_provider_available") && RowFieldEquals(active_result, "correctness_authority", "cpu_equivalent_engine_plan");
  const bool optional_fallback_ok = optional_fallback_result.ok &&
                                    HasEvidence(optional_fallback_result, "extension_behavior", "cpu_fallback_accelerator_policy") &&
                                    RowFieldEquals(optional_fallback_result, "gpu_effective_path", "cpu_fallback") &&
                                    RowFieldEquals(optional_fallback_result, "gpu_batch_scalar_result", "6.000000");
  const bool provider_batch_ok = provider_batch_result.ok &&
                                 HasEvidence(provider_batch_result, "gpu_cpu_equivalence", "reference_batch_result") &&
                                 RowFieldEquals(provider_batch_result, "gpu_effective_path", "gpu_provider_admitted") &&
                                 RowFieldEquals(provider_batch_result, "gpu_batch_scalar_result", "32.000000");
  const bool budget_denied = !budget_result.ok &&
                             HasDiagnosticCode(budget_result, "SB_ENGINE_API_GPU.DEVICE_MEMORY_POLICY_VIOLATION");
  const bool cluster_denied = !cluster_result.ok && cluster_result.cluster_authority_required;

  const bool ok = inspect_ok && insecure_denied && unavailable_denied && unsupported_denied && bypass_denied && active_ok && optional_fallback_ok && provider_batch_ok && budget_denied && cluster_denied;

  std::cout << "{\n";
  PrintBool("ok", ok, true);
  PrintBool("inspect_ok", inspect_ok, true);
  PrintBool("insecure_denied", insecure_denied, true);
  PrintBool("unavailable_denied", unavailable_denied, true);
  PrintBool("unsupported_denied", unsupported_denied, true);
  PrintBool("bypass_denied", bypass_denied, true);
  PrintBool("active_ok", active_ok, true);
  PrintBool("optional_fallback_ok", optional_fallback_ok, true);
  PrintBool("provider_batch_ok", provider_batch_ok, true);
  PrintBool("budget_denied", budget_denied, true);
  PrintBool("cluster_denied", cluster_denied, false);
  std::cout << "}\n";
  return ok ? 0 : 1;
}
