// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "scoring_kernel_acceleration.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <string>
#include <utility>

namespace scratchbird::engine::gpu_acceleration {
namespace {
namespace agents = scratchbird::core::agents;
namespace platform = scratchbird::core::platform;

void Add(std::vector<std::string>* evidence, std::string value) {
  evidence->push_back(std::move(value));
}

bool KnownKind(ScoringKernelKind kind) {
  switch (kind) {
    case ScoringKernelKind::kVectorDistance:
    case ScoringKernelKind::kBm25:
    case ScoringKernelKind::kBitmapIntersection:
    case ScoringKernelKind::kTimeAggregate:
    case ScoringKernelKind::kJsonPath:
    case ScoringKernelKind::kGraphMembership:
      return true;
    case ScoringKernelKind::kUnknown:
      return false;
  }
  return false;
}

bool ContainsKind(const std::vector<ScoringKernelKind>& kinds,
                  ScoringKernelKind kind) {
  return std::find(kinds.begin(), kinds.end(), kind) != kinds.end();
}

bool ContainsString(const std::vector<std::string>& values,
                    const std::string& value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

bool SameDoubles(const std::vector<double>& left,
                 const std::vector<double>& right) {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t i = 0; i < left.size(); ++i) {
    if (std::isnan(left[i]) || std::isnan(right[i]) ||
        std::abs(left[i] - right[i]) > 0.0) {
      return false;
    }
  }
  return true;
}

bool SameValues(const ScoringKernelValueBatch& left,
                const ScoringKernelValueBatch& right) {
  return SameDoubles(left.double_values, right.double_values) &&
         left.integer_values == right.integer_values &&
         left.string_values == right.string_values;
}

ScoringKernelResult Refuse(std::string code, std::string detail) {
  ScoringKernelResult result;
  result.ok = false;
  result.fail_closed = true;
  result.route = ScoringKernelRoute::kRefused;
  result.diagnostic_code = std::move(code);
  result.diagnostic_detail = std::move(detail);
  Add(&result.evidence, "scoring_kernel.route=refused");
  Add(&result.evidence, "scoring_kernel.fail_closed=true");
  Add(&result.evidence, "scoring_kernel.accelerator_only=true");
  Add(&result.evidence, "mga_finality_authority=engine_transaction_inventory");
  Add(&result.evidence, "parser_or_reference_authority=false");
  Add(&result.evidence, "provider_transaction_finality_authority=false");
  Add(&result.evidence, "provider_visibility_authority=false");
  Add(&result.evidence, "provider_security_policy_authority=false");
  Add(&result.evidence, "provider_redaction_policy_authority=false");
  Add(&result.evidence, "provider_recovery_authority=false");
  Add(&result.evidence, "provider_page_or_catalog_authority=false");
  return result;
}

void AppendGovernanceEvidence(
    ScoringKernelResult* result,
    const agents::ResourceGovernanceAdmissionResult& governance) {
  result->evidence.insert(result->evidence.end(), governance.evidence.begin(),
                          governance.evidence.end());
  Add(&result->evidence, "scoring_kernel.resource_governance_action=" +
                             std::string(agents::ResourceGovernanceActionName(
                                 governance.action)));
}

void AppendCompatibilityEvidence(
    ScoringKernelResult* result,
    const platform::RuntimeCompatibilityResult& compatibility) {
  result->evidence.insert(result->evidence.end(),
                          compatibility.evidence.begin(),
                          compatibility.evidence.end());
}

platform::RuntimeCompatibilityDescriptor ScoringRuntimeCompatibility(
    const ScoringKernelRequest& request) {
  auto descriptor = request.capabilities.runtime_compatibility;
  if (descriptor.route_id.empty()) {
    descriptor = platform::CurrentRuntimeCompatibilityDescriptor(
        "engine.scoring_kernel_acceleration");
  }
  descriptor.route_id = descriptor.route_id.empty()
                            ? "engine.scoring_kernel_acceleration"
                            : descriptor.route_id;
  descriptor.source_component = "engine.scoring_kernel_acceleration";
  descriptor.accelerator_requested = true;
  descriptor.deterministic_scalar_fallback_available = true;
  if (!request.capabilities.required_engine_abi_id.empty()) {
    descriptor.required_engine_abi_id =
        request.capabilities.required_engine_abi_id;
  }
  if (!request.provider.manifest.engine_abi_id.empty()) {
    descriptor.provider_engine_abi_id =
        request.provider.manifest.engine_abi_id;
  }
  if (!request.capabilities.required_runtime_identity_id.empty()) {
    descriptor.required_runtime_identity_id =
        request.capabilities.required_runtime_identity_id;
  }
  if (!request.provider.manifest.runtime_identity_id.empty()) {
    descriptor.provider_runtime_identity_id =
        request.provider.manifest.runtime_identity_id;
  }
  if (!request.capabilities.required_architecture.empty()) {
    descriptor.required_architecture =
        request.capabilities.required_architecture;
  }
  if (!request.provider.manifest.architecture.empty()) {
    descriptor.provider_architecture = request.provider.manifest.architecture;
  }
  if (!request.capabilities.required_cpu_capabilities.empty()) {
    descriptor.required_cpu_features =
        request.capabilities.required_cpu_capabilities;
  }
  if (!request.provider.manifest.cpu_capabilities.empty()) {
    descriptor.provider_cpu_features =
        request.provider.manifest.cpu_capabilities;
  }
  if (!request.capabilities.required_gpu_capabilities.empty()) {
    descriptor.required_accelerator_capabilities =
        request.capabilities.required_gpu_capabilities;
  }
  if (!request.provider.manifest.gpu_capabilities.empty()) {
    descriptor.provider_accelerator_capabilities =
        request.provider.manifest.gpu_capabilities;
  }
  return descriptor;
}

ScoringKernelResult ScalarFallback(const ScoringKernelRequest& request,
                                   ScoringKernelValueBatch values,
                                   std::string code,
                                   std::string detail) {
  ScoringKernelResult result;
  result.ok = true;
  result.scalar_fallback_used = true;
  result.route = ScoringKernelRoute::kScalarFallback;
  result.values = std::move(values);
  result.diagnostic_code = std::move(code);
  result.diagnostic_detail = std::move(detail);
  Add(&result.evidence, "scoring_kernel.route=scalar_fallback");
  Add(&result.evidence, std::string("scoring_kernel.kind=") +
                            ScoringKernelKindName(request.descriptor.kind));
  Add(&result.evidence, "scoring_kernel.scalar_reference=exact");
  Add(&result.evidence, "scoring_kernel.accelerator_only=true");
  Add(&result.evidence, "mga_finality_authority=engine_transaction_inventory");
  Add(&result.evidence, "parser_or_reference_authority=false");
  Add(&result.evidence, "provider_transaction_finality_authority=false");
  Add(&result.evidence, "provider_visibility_authority=false");
  Add(&result.evidence, "provider_security_policy_authority=false");
  Add(&result.evidence, "provider_redaction_policy_authority=false");
  Add(&result.evidence, "provider_recovery_authority=false");
  Add(&result.evidence, "provider_page_or_catalog_authority=false");
  return result;
}

ScoringKernelResult AcceleratorSuccess(
    const ScoringKernelRequest& request,
    ScoringKernelValueBatch values,
    const ScoringKernelProviderOutcome& outcome) {
  ScoringKernelResult result;
  result.ok = true;
  result.accelerator_used = true;
  result.route = ScoringKernelRoute::kAccelerator;
  result.values = std::move(values);
  result.diagnostic_code = "SB_SCORING_KERNEL.OK";
  result.diagnostic_detail = "optional_scoring_kernel_accelerator_selected";
  Add(&result.evidence, "scoring_kernel.route=accelerator");
  Add(&result.evidence, std::string("scoring_kernel.kind=") +
                            ScoringKernelKindName(request.descriptor.kind));
  Add(&result.evidence, "scoring_kernel.descriptor_id=" +
                            request.descriptor.descriptor_id);
  Add(&result.evidence, "scoring_kernel.provider_id=" +
                            request.provider.manifest.provider_id);
  Add(&result.evidence, "scoring_kernel.kernel_id=" + outcome.kernel_id);
  Add(&result.evidence, "scoring_kernel.scalar_equivalence=verified");
  Add(&result.evidence, "scoring_kernel.accelerator_only=true");
  Add(&result.evidence, "mga_finality_authority=engine_transaction_inventory");
  Add(&result.evidence, "parser_or_reference_authority=false");
  Add(&result.evidence, "provider_transaction_finality_authority=false");
  Add(&result.evidence, "provider_visibility_authority=false");
  Add(&result.evidence, "provider_security_policy_authority=false");
  Add(&result.evidence, "provider_redaction_policy_authority=false");
  Add(&result.evidence, "provider_recovery_authority=false");
  Add(&result.evidence, "provider_page_or_catalog_authority=false");
  return result;
}

bool UnsafeProviderAuthority(const ScoringKernelProviderManifest& manifest) {
  return !manifest.safe_to_execute ||
         manifest.claims_transaction_finality_authority ||
         manifest.claims_visibility_authority ||
         manifest.claims_security_policy_authority ||
         manifest.claims_redaction_policy_authority ||
         manifest.claims_parser_or_reference_authority ||
         manifest.claims_recovery_authority ||
         manifest.claims_page_or_catalog_authority;
}

bool HasRequiredCapabilities(const ScoringKernelCapabilityBinding& required,
                             const ScoringKernelProviderManifest& provider) {
  if (!required.capability_manifest_safe) {
    return false;
  }
  if (!required.required_engine_abi_id.empty() &&
      provider.engine_abi_id != required.required_engine_abi_id) {
    return false;
  }
  if (!required.required_runtime_identity_id.empty() &&
      provider.runtime_identity_id != required.required_runtime_identity_id) {
    return false;
  }
  if (!required.required_architecture.empty() &&
      provider.architecture != required.required_architecture) {
    return false;
  }
  for (const auto& capability : required.required_cpu_capabilities) {
    if (!ContainsString(provider.cpu_capabilities, capability)) {
      return false;
    }
  }
  for (const auto& capability : required.required_gpu_capabilities) {
    if (!ContainsString(provider.gpu_capabilities, capability)) {
      return false;
    }
  }
  return true;
}

std::uint64_t VectorBytes(std::size_t count, std::size_t element_size) {
  return static_cast<std::uint64_t>(count) *
         static_cast<std::uint64_t>(element_size);
}

std::uint64_t MinimumBatchBytes(const ScoringKernelInputBatch& input) {
  std::uint64_t bytes = 0;
  bytes += VectorBytes(input.lhs_values.size(), sizeof(double));
  bytes += VectorBytes(input.rhs_values.size(), sizeof(double));
  bytes += VectorBytes(input.time_values.size(), sizeof(double));
  bytes += VectorBytes(input.document_lengths.size(), sizeof(double));
  bytes += VectorBytes(input.bitmap_left.size(), sizeof(std::uint64_t));
  bytes += VectorBytes(input.bitmap_right.size(), sizeof(std::uint64_t));
  bytes += VectorBytes(input.graph_sources.size(), sizeof(std::uint64_t));
  bytes += VectorBytes(input.graph_targets.size(), sizeof(std::uint64_t));
  bytes += VectorBytes(input.query_sources.size(), sizeof(std::uint64_t));
  bytes += VectorBytes(input.query_targets.size(), sizeof(std::uint64_t));
  for (const auto& value : input.json_documents) {
    bytes += static_cast<std::uint64_t>(value.size());
  }
  for (const auto& value : input.json_path_tokens) {
    bytes += static_cast<std::uint64_t>(value.size());
  }
  return bytes;
}

bool ValidKindBatchShape(ScoringKernelKind kind,
                         const ScoringKernelInputBatch& input) {
  switch (kind) {
    case ScoringKernelKind::kVectorDistance:
      return input.row_count == input.lhs_values.size() &&
             input.lhs_values.size() == input.rhs_values.size();
    case ScoringKernelKind::kBm25:
      return input.row_count == input.lhs_values.size() &&
             input.lhs_values.size() == input.rhs_values.size() &&
             (input.document_lengths.empty() ||
              input.document_lengths.size() == input.row_count);
    case ScoringKernelKind::kBitmapIntersection:
      return input.row_count ==
             input.bitmap_left.size() + input.bitmap_right.size();
    case ScoringKernelKind::kTimeAggregate:
      return input.row_count == input.time_values.size();
    case ScoringKernelKind::kJsonPath:
      return input.row_count == input.json_documents.size() &&
             !input.json_path_tokens.empty();
    case ScoringKernelKind::kGraphMembership:
      return input.graph_sources.size() == input.graph_targets.size() &&
             input.query_sources.size() == input.query_targets.size() &&
             input.row_count == input.query_sources.size();
    case ScoringKernelKind::kUnknown:
      return false;
  }
  return false;
}

ScoringKernelResult ValidateInput(const ScoringKernelRequest& request) {
  const auto& descriptor = request.descriptor;
  if (!KnownKind(descriptor.kind) || descriptor.descriptor_id.empty() ||
      descriptor.kernel_digest.empty() ||
      descriptor.descriptor_generation == 0 ||
      descriptor.expected_descriptor_generation == 0 ||
      descriptor.descriptor_generation !=
          descriptor.expected_descriptor_generation) {
    return Refuse("SB_SCORING_KERNEL.CORRUPT_DESCRIPTOR_REFUSED",
                  "kernel_descriptor_invalid_or_stale");
  }
  if (!descriptor.deterministic_equivalence_required ||
      !descriptor.deterministic_equivalence_provable) {
    return Refuse("SB_SCORING_KERNEL.DETERMINISM_REQUIRED",
                  "deterministic_scalar_equivalence_required");
  }
  if (!request.scalar_reference) {
    return Refuse("SB_SCORING_KERNEL.SCALAR_REFERENCE_REQUIRED",
                  "exact_scalar_reference_required");
  }
  if (request.input.direct_page_or_catalog_access) {
    return Refuse("SB_SCORING_KERNEL.UNSAFE_PAGE_CATALOG_ACCESS",
                  "accelerator_requires_executor_materialized_batch");
  }
  if (request.input.raw_client_or_parser_input) {
    return Refuse("SB_SCORING_KERNEL.RAW_INPUT_REFUSED",
                  "accelerator_requires_validated_sblr_or_internal_kernel_input");
  }
  if (!request.input.materialized_authorized_batch ||
      !request.input.safe_materialized_batch ||
      request.input.batch_id.empty() || request.input.row_count == 0 ||
      !ValidKindBatchShape(descriptor.kind, request.input)) {
    return Refuse("SB_SCORING_KERNEL.CORRUPT_INPUT_REFUSED",
                  "materialized_authorized_batch_invalid");
  }
  if (request.input.materialized_memory_bytes < MinimumBatchBytes(request.input)) {
    return Refuse("SB_SCORING_KERNEL.CORRUPT_INPUT_REFUSED",
                  "materialized_batch_memory_accounting_invalid");
  }
  return {};
}

ScoringKernelValueBatch ExecuteScalar(const ScoringKernelRequest& request,
                                      bool* ok) {
  *ok = false;
  if (!request.scalar_reference) {
    return {};
  }
  try {
    auto values = request.scalar_reference(request.input);
    *ok = true;
    return values;
  } catch (const std::exception&) {
    return {};
  } catch (...) {
    return {};
  }
}

bool QuotaExceeded(const ScoringKernelRequest& request) {
  return (request.max_rows != 0 && request.input.row_count > request.max_rows) ||
         (request.max_memory_bytes != 0 &&
          request.input.materialized_memory_bytes > request.max_memory_bytes);
}

}  // namespace

const char* ScoringKernelKindName(ScoringKernelKind kind) {
  switch (kind) {
    case ScoringKernelKind::kVectorDistance:
      return "vector_distance";
    case ScoringKernelKind::kBm25:
      return "bm25";
    case ScoringKernelKind::kBitmapIntersection:
      return "bitmap_intersection";
    case ScoringKernelKind::kTimeAggregate:
      return "time_aggregate";
    case ScoringKernelKind::kJsonPath:
      return "json_path";
    case ScoringKernelKind::kGraphMembership:
      return "graph_membership";
    case ScoringKernelKind::kUnknown:
      break;
  }
  return "unknown";
}

const char* ScoringKernelRouteName(ScoringKernelRoute route) {
  switch (route) {
    case ScoringKernelRoute::kScalarFallback:
      return "scalar_fallback";
    case ScoringKernelRoute::kAccelerator:
      return "accelerator";
    case ScoringKernelRoute::kRefused:
      return "refused";
  }
  return "refused";
}

ScoringKernelResult ExecuteScoringKernelAcceleration(
    const ScoringKernelRequest& request) {
  auto input_check = ValidateInput(request);
  if (input_check.fail_closed) {
    return input_check;
  }

  bool scalar_ok = false;
  auto scalar_values = ExecuteScalar(request, &scalar_ok);
  if (!scalar_ok) {
    return Refuse("SB_SCORING_KERNEL.SCALAR_REFERENCE_REQUIRED",
                  "exact_scalar_reference_failed");
  }

  auto governance_request = request.resource_governance;
  governance_request.expected_family =
      agents::ResourceGovernanceFamily::kScoringKernelAccelerator;
  const auto governance = agents::AdmitResourceGovernance(governance_request);
  if (governance.action == agents::ResourceGovernanceAction::kFailClosed) {
    auto result = Refuse("SB_SCORING_KERNEL.ODF106_QUOTA_REFUSED",
                         governance.diagnostic_code);
    AppendGovernanceEvidence(&result, governance);
    return result;
  }
  if (governance.action == agents::ResourceGovernanceAction::kCancel ||
      governance.action ==
          agents::ResourceGovernanceAction::kExactScalarFallback ||
      governance.action == agents::ResourceGovernanceAction::kSlowdownDegrade) {
    auto result = ScalarFallback(
        request, std::move(scalar_values),
        "SB_SCORING_KERNEL.ODF106_EXACT_SCALAR_FALLBACK",
        governance.diagnostic_code);
    AppendGovernanceEvidence(&result, governance);
    return result;
  }

  auto append_governance = [&](ScoringKernelResult result) {
    AppendGovernanceEvidence(&result, governance);
    return result;
  };

  if (request.cancellation_requested || request.quota_exhausted ||
      QuotaExceeded(request)) {
    return append_governance(ScalarFallback(
        request, std::move(scalar_values),
        "SB_SCORING_KERNEL.CANCELLED_OR_QUOTA_FALLBACK",
        "scoring_kernel_cancelled_or_quota_exhausted"));
  }
  if (request.minimum_rows_for_acceleration != 0 &&
      request.input.row_count < request.minimum_rows_for_acceleration) {
    return append_governance(ScalarFallback(
        request, std::move(scalar_values),
        "SB_SCORING_KERNEL.COLD_BATCH_FALLBACK",
        "materialized_batch_not_large_enough_for_acceleration"));
  }
  if (request.provider.manifest.provider_id.empty() || !request.provider.run) {
    return append_governance(ScalarFallback(
        request, std::move(scalar_values),
        "SB_SCORING_KERNEL.PROVIDER_MISSING_FALLBACK",
        "optional_scoring_kernel_provider_missing"));
  }
  if (UnsafeProviderAuthority(request.provider.manifest)) {
    return append_governance(Refuse(
        "SB_SCORING_KERNEL.UNSAFE_PROVIDER_AUTHORITY",
        "scoring_kernel_provider_claims_forbidden_authority"));
  }
  if (!ContainsKind(request.provider.manifest.supported_kinds,
                    request.descriptor.kind)) {
    return append_governance(ScalarFallback(
        request, std::move(scalar_values),
        "SB_SCORING_KERNEL.UNSUPPORTED_KIND_FALLBACK",
        "scoring_kernel_kind_not_supported_by_provider"));
  }
  const auto compatibility = platform::NegotiateRuntimeCompatibility(
      ScoringRuntimeCompatibility(request));
  const bool explicit_runtime_descriptor =
      !request.capabilities.runtime_compatibility.route_id.empty();
  auto append_compatibility = [&](ScoringKernelResult result) {
    AppendGovernanceEvidence(&result, governance);
    AppendCompatibilityEvidence(&result, compatibility);
    return result;
  };
  if (compatibility.action ==
      platform::RuntimeCompatibilityAction::exact_scalar_fallback) {
    return append_compatibility(ScalarFallback(
        request, std::move(scalar_values),
        explicit_runtime_descriptor
            ? "SB_SCORING_KERNEL.ODF107_RUNTIME_COMPATIBILITY_FALLBACK"
            : "SB_SCORING_KERNEL.CAPABILITY_MISMATCH_FALLBACK",
        compatibility.diagnostic_code));
  }
  if (compatibility.action ==
      platform::RuntimeCompatibilityAction::fail_closed) {
    return append_compatibility(Refuse(
        "SB_SCORING_KERNEL.ODF107_RUNTIME_COMPATIBILITY_REFUSED",
        compatibility.diagnostic_code));
  }
  if (!HasRequiredCapabilities(request.capabilities,
                               request.provider.manifest)) {
    return append_compatibility(ScalarFallback(
        request, std::move(scalar_values),
        "SB_SCORING_KERNEL.CAPABILITY_MISMATCH_FALLBACK",
        "scoring_kernel_provider_abi_cpu_gpu_or_capability_mismatch"));
  }

  ScoringKernelProviderOutcome outcome;
  try {
    outcome = request.provider.run(request);
  } catch (const std::exception& ex) {
    return append_compatibility(Refuse(
        "SB_SCORING_KERNEL.RUNTIME_MISMATCH_REFUSED",
        std::string("scoring_kernel_provider_exception:") + ex.what()));
  } catch (...) {
    return append_compatibility(Refuse(
        "SB_SCORING_KERNEL.RUNTIME_MISMATCH_REFUSED",
        "scoring_kernel_provider_exception"));
  }
  if (outcome.cancelled_or_quota) {
    return append_compatibility(ScalarFallback(
        request, std::move(scalar_values),
        "SB_SCORING_KERNEL.CANCELLED_OR_QUOTA_FALLBACK",
        outcome.diagnostic_detail.empty()
            ? "scoring_kernel_runtime_cancelled_or_quota"
            : outcome.diagnostic_detail));
  }
  if (!outcome.ok || outcome.kernel_id.empty()) {
    return append_compatibility(ScalarFallback(
        request, std::move(scalar_values),
        "SB_SCORING_KERNEL.PROVIDER_RUNTIME_FALLBACK",
        outcome.diagnostic_detail.empty()
            ? "scoring_kernel_provider_runtime_failed"
            : outcome.diagnostic_detail));
  }
  if (outcome.descriptor_generation !=
          request.descriptor.descriptor_generation ||
      outcome.runtime_identity_id !=
          request.provider.manifest.runtime_identity_id ||
      (!request.capabilities.required_runtime_identity_id.empty() &&
       outcome.runtime_identity_id !=
           request.capabilities.required_runtime_identity_id)) {
    return append_compatibility(Refuse(
        "SB_SCORING_KERNEL.RUNTIME_IDENTITY_MISMATCH_REFUSED",
        "scoring_kernel_runtime_identity_or_generation_mismatch"));
  }
  if (!SameValues(outcome.values, scalar_values)) {
    return append_compatibility(Refuse(
        "SB_SCORING_KERNEL.RESULT_MISMATCH_REFUSED",
        "scoring_kernel_result_does_not_match_exact_scalar_reference"));
  }

  return append_compatibility(
      AcceleratorSuccess(request, std::move(outcome.values), outcome));
}

}  // namespace scratchbird::engine::gpu_acceleration
