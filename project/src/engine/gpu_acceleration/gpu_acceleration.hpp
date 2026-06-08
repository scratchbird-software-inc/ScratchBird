// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SEARCH_KEY: SB_ENGINE_GPU_ACCELERATION_SUBSYSTEM
// Engine-owned GPU acceleration admission and materialized-batch execution
// seam. GPU is never semantic, security, transaction, MGA, catalog, cleanup,
// recovery, parser, or cluster-decision authority.

#include "runtime_capabilities.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace scratchbird::engine::gpu_acceleration {

enum class GpuPolicyProfile {
  disabled,
  optional_batch,
  required_for_declared_workload,
  cluster_optional,
  cluster_required,
  dev_kernel_debug,
};

enum class GpuEffectivePath {
  inspect_only,
  cpu_fallback,
  gpu_provider_admitted,
  refused,
};

enum class GpuBatchOperation {
  none,
  filter_positive,
  project_scale,
  aggregate_sum,
  vector_dot,
};

struct GpuAccelerationRequest {
  bool execution_requested = false;
  bool provider_available = false;
  bool security_context_present = false;
  bool cluster_authority_available = false;
  bool cluster_dispatch_requested = false;
  bool authority_bypass_requested = false;
  bool direct_page_or_catalog_access_requested = false;
  bool raw_client_or_parser_input_requested = false;
  bool deterministic_equivalence_required = true;
  bool approximate_declared = false;
  std::string workload = "inspect";
  std::string provider = "missing";
  std::string backend = "none";
  GpuPolicyProfile policy_profile = GpuPolicyProfile::disabled;
  GpuBatchOperation batch_operation = GpuBatchOperation::none;
  std::vector<double> values;
  std::vector<double> rhs_values;
  double scale = 1.0;
  std::uint64_t device_memory_budget_bytes = 64U * 1024U * 1024U;
  std::uint64_t pinned_host_memory_budget_bytes = 16U * 1024U * 1024U;
  scratchbird::core::platform::RuntimeCompatibilityDescriptor
      runtime_compatibility;
};

struct GpuAccelerationResult {
  bool ok = false;
  std::string diagnostic_code;
  std::string diagnostic_detail;
  GpuEffectivePath effective_path = GpuEffectivePath::refused;
  bool fallback_used = false;
  bool planner_may_select_gpu = false;
  std::string activation_state = "inspect_only";
  std::string cache_key_hash;
  std::uint64_t transfer_bytes = 0;
  std::uint64_t device_memory_bytes = 0;
  std::vector<double> output_values;
  double scalar_value = 0.0;
  std::vector<std::pair<std::string, std::string>> evidence;
};

GpuAccelerationResult EvaluateGpuAcceleration(const GpuAccelerationRequest& request);

bool GpuWorkloadSupported(const std::string& workload);
const char* GpuPolicyProfileName(GpuPolicyProfile profile);
const char* GpuEffectivePathName(GpuEffectivePath path);
const char* GpuBatchOperationName(GpuBatchOperation operation);
GpuPolicyProfile ParseGpuPolicyProfile(const std::string& profile);
GpuBatchOperation ParseGpuBatchOperation(const std::string& operation);

}  // namespace scratchbird::engine::gpu_acceleration
