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

namespace scratchbird::engine::native_compile {

// SEARCH_KEY: SB_NATIVE_SBLR_SPECIALIZATION_ODF_104
// Native SBLR specialization is an optional accelerator over stable prepared
// templates. Scalar SBLR/executor evaluation remains the exact authority for
// values, diagnostics, MGA visibility, transaction finality, security, and
// redaction policy.

enum class NativeSblrSpecializationKind {
  kUnknown = 0,
  kPredicate,
  kProjection,
  kRowDecode,
  kPathExtraction,
  kDistanceScoring,
  kAggregate,
};

enum class NativeSblrSpecializationRoute {
  kScalarFallback,
  kNative,
  kRefused,
};

struct NativeSblrTemplateIdentity {
  std::string stable_template_id;
  std::string sblr_digest;
  std::string plan_node_id;
  std::uint64_t template_generation = 0;
  std::uint64_t expected_template_generation = 0;
};

struct NativeSblrEpochBinding {
  std::uint64_t security_epoch = 0;
  std::uint64_t expected_security_epoch = 0;
  std::uint64_t redaction_epoch = 0;
  std::uint64_t expected_redaction_epoch = 0;
};

struct NativeSblrHotness {
  std::uint64_t observed_invocations = 0;
  std::uint64_t observed_rows = 0;
  std::uint64_t observed_expressions = 0;
  std::uint64_t minimum_invocations = 0;
  std::uint64_t minimum_rows = 0;
  std::uint64_t minimum_expressions = 0;
};

struct NativeSblrCapabilityBinding {
  std::string required_engine_abi_id = "sb_engine_abi_v3";
  std::string required_architecture;
  std::vector<std::string> required_cpu_capabilities;
  bool capability_manifest_safe = true;
  scratchbird::core::platform::RuntimeCompatibilityDescriptor
      runtime_compatibility;
};

struct NativeSblrInputBatch {
  std::vector<std::int64_t> values;
  std::uint64_t row_count = 0;
};

struct NativeSblrValueBatch {
  std::vector<std::int64_t> values;
};

using NativeSblrScalarReference =
    std::function<NativeSblrValueBatch(const NativeSblrInputBatch&)>;

struct NativeSblrProviderManifest {
  std::string provider_id;
  std::string engine_abi_id = "sb_engine_abi_v3";
  std::string architecture;
  std::vector<std::string> cpu_capabilities;
  std::vector<NativeSblrSpecializationKind> supported_kinds;

  bool safe_to_execute = true;
  bool claims_transaction_finality_authority = false;
  bool claims_visibility_authority = false;
  bool claims_security_policy_authority = false;
  bool claims_redaction_policy_authority = false;
  bool claims_parser_or_reference_authority = false;
};

struct NativeSblrCompileOutcome {
  bool ok = false;
  bool cancelled_or_quota = false;
  std::string kernel_id;
  std::string diagnostic_code;
  std::string diagnostic_detail;
};

struct NativeSblrKernelOutcome {
  bool ok = false;
  bool cancelled_or_quota = false;
  std::string diagnostic_code;
  std::string diagnostic_detail;
  std::string kernel_id;
  std::uint64_t template_generation = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t redaction_epoch = 0;
  NativeSblrValueBatch values;
};

struct NativeSblrSpecializationRequest;

using NativeSblrCompileProvider =
    std::function<NativeSblrCompileOutcome(
        const NativeSblrSpecializationRequest&)>;
using NativeSblrKernelProvider =
    std::function<NativeSblrKernelOutcome(
        const NativeSblrSpecializationRequest&,
        const NativeSblrCompileOutcome&)>;

struct NativeSblrProvider {
  NativeSblrProviderManifest manifest;
  NativeSblrCompileProvider compile;
  NativeSblrKernelProvider run;
};

struct NativeSblrSpecializationRequest {
  NativeSblrSpecializationKind kind = NativeSblrSpecializationKind::kUnknown;
  NativeSblrTemplateIdentity identity;
  NativeSblrEpochBinding epochs;
  NativeSblrHotness hotness;
  NativeSblrCapabilityBinding capabilities;
  NativeSblrInputBatch input;
  NativeSblrScalarReference scalar_reference;
  NativeSblrProvider provider;
  std::string sblr_module_payload;
  bool cancellation_requested = false;
  bool quota_exhausted = false;
  scratchbird::core::agents::ResourceGovernanceAdmissionRequest
      resource_governance;
};

struct NativeSblrSpecializationResult {
  bool ok = false;
  bool fail_closed = false;
  bool native_used = false;
  bool scalar_fallback_used = false;
  NativeSblrSpecializationRoute route =
      NativeSblrSpecializationRoute::kRefused;
  NativeSblrValueBatch values;
  std::string diagnostic_code;
  std::string diagnostic_detail;
  std::vector<std::string> evidence;
};

const char* NativeSblrSpecializationKindName(
    NativeSblrSpecializationKind kind);
const char* NativeSblrSpecializationRouteName(
    NativeSblrSpecializationRoute route);

NativeSblrSpecializationResult ExecuteNativeSblrSpecialization(
    const NativeSblrSpecializationRequest& request);

}  // namespace scratchbird::engine::native_compile
