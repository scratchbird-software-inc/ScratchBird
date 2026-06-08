// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "resource_governance_admission.hpp"
#include "runtime_capabilities.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace scratchbird::engine::gpu_acceleration {

// SEARCH_KEY: SB_SCORING_KERNEL_ACCELERATION_ODF_105
// SIMD/GPU scoring kernels are optional accelerators over materialized,
// authorized executor batches. Exact scalar executor evaluation remains the
// authority for results, transaction finality, MGA visibility, security,
// redaction, parser boundaries, donor compatibility, recovery, pages, and
// catalog state.

enum class ScoringKernelKind {
  kUnknown = 0,
  kVectorDistance,
  kBm25,
  kBitmapIntersection,
  kTimeAggregate,
  kJsonPath,
  kGraphMembership,
};

enum class ScoringKernelRoute {
  kScalarFallback,
  kAccelerator,
  kRefused,
};

struct ScoringKernelDescriptor {
  ScoringKernelKind kind = ScoringKernelKind::kUnknown;
  std::string descriptor_id;
  std::string kernel_digest;
  std::uint64_t descriptor_generation = 0;
  std::uint64_t expected_descriptor_generation = 0;
  bool deterministic_equivalence_required = true;
  bool deterministic_equivalence_provable = true;
};

struct ScoringKernelCapabilityBinding {
  std::string required_engine_abi_id = "sb_engine_abi_v3";
  std::string required_runtime_identity_id;
  std::string required_architecture;
  std::vector<std::string> required_cpu_capabilities;
  std::vector<std::string> required_gpu_capabilities;
  bool capability_manifest_safe = true;
  scratchbird::core::platform::RuntimeCompatibilityDescriptor
      runtime_compatibility;
};

struct ScoringKernelInputBatch {
  std::string batch_id;
  std::uint64_t row_count = 0;
  std::uint64_t materialized_memory_bytes = 0;

  std::vector<double> lhs_values;
  std::vector<double> rhs_values;
  std::vector<double> time_values;
  std::vector<double> document_lengths;

  std::vector<std::uint64_t> bitmap_left;
  std::vector<std::uint64_t> bitmap_right;
  std::vector<std::uint64_t> graph_sources;
  std::vector<std::uint64_t> graph_targets;
  std::vector<std::uint64_t> query_sources;
  std::vector<std::uint64_t> query_targets;

  std::vector<std::string> json_documents;
  std::vector<std::string> json_path_tokens;

  bool materialized_authorized_batch = true;
  bool safe_materialized_batch = true;
  bool raw_client_or_parser_input = false;
  bool direct_page_or_catalog_access = false;
};

struct ScoringKernelValueBatch {
  std::vector<double> double_values;
  std::vector<std::int64_t> integer_values;
  std::vector<std::string> string_values;
};

using ScoringKernelScalarReference =
    std::function<ScoringKernelValueBatch(const ScoringKernelInputBatch&)>;

struct ScoringKernelProviderManifest {
  std::string provider_id;
  std::string engine_abi_id = "sb_engine_abi_v3";
  std::string runtime_identity_id;
  std::string architecture;
  std::vector<std::string> cpu_capabilities;
  std::vector<std::string> gpu_capabilities;
  std::vector<ScoringKernelKind> supported_kinds;

  bool safe_to_execute = true;
  bool claims_transaction_finality_authority = false;
  bool claims_visibility_authority = false;
  bool claims_security_policy_authority = false;
  bool claims_redaction_policy_authority = false;
  bool claims_parser_or_donor_authority = false;
  bool claims_recovery_authority = false;
  bool claims_page_or_catalog_authority = false;
};

struct ScoringKernelProviderOutcome {
  bool ok = false;
  bool cancelled_or_quota = false;
  std::string diagnostic_code;
  std::string diagnostic_detail;
  std::string kernel_id;
  std::uint64_t descriptor_generation = 0;
  std::string runtime_identity_id;
  ScoringKernelValueBatch values;
};

struct ScoringKernelRequest;

using ScoringKernelProviderRun =
    std::function<ScoringKernelProviderOutcome(const ScoringKernelRequest&)>;

struct ScoringKernelProvider {
  ScoringKernelProviderManifest manifest;
  ScoringKernelProviderRun run;
};

struct ScoringKernelRequest {
  ScoringKernelDescriptor descriptor;
  ScoringKernelCapabilityBinding capabilities;
  ScoringKernelInputBatch input;
  ScoringKernelScalarReference scalar_reference;
  ScoringKernelProvider provider;
  std::uint64_t max_rows = 0;
  std::uint64_t max_memory_bytes = 0;
  std::uint64_t minimum_rows_for_acceleration = 0;
  bool cancellation_requested = false;
  bool quota_exhausted = false;
  scratchbird::core::agents::ResourceGovernanceAdmissionRequest
      resource_governance;
};

struct ScoringKernelResult {
  bool ok = false;
  bool fail_closed = false;
  bool accelerator_used = false;
  bool scalar_fallback_used = false;
  ScoringKernelRoute route = ScoringKernelRoute::kRefused;
  ScoringKernelValueBatch values;
  std::string diagnostic_code;
  std::string diagnostic_detail;
  std::vector<std::string> evidence;
};

const char* ScoringKernelKindName(ScoringKernelKind kind);
const char* ScoringKernelRouteName(ScoringKernelRoute route);

ScoringKernelResult ExecuteScoringKernelAcceleration(
    const ScoringKernelRequest& request);

}  // namespace scratchbird::engine::gpu_acceleration
