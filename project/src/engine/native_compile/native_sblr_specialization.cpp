// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "native_sblr_specialization.hpp"

#include <algorithm>
#include <exception>
#include <string_view>
#include <utility>

namespace scratchbird::engine::native_compile {
namespace {
namespace agents = scratchbird::core::agents;
namespace platform = scratchbird::core::platform;

bool KnownKind(NativeSblrSpecializationKind kind) {
  switch (kind) {
    case NativeSblrSpecializationKind::kPredicate:
    case NativeSblrSpecializationKind::kProjection:
    case NativeSblrSpecializationKind::kRowDecode:
    case NativeSblrSpecializationKind::kPathExtraction:
    case NativeSblrSpecializationKind::kDistanceScoring:
    case NativeSblrSpecializationKind::kAggregate:
      return true;
    case NativeSblrSpecializationKind::kUnknown:
      return false;
  }
  return false;
}

void Add(std::vector<std::string>* evidence, std::string value) {
  evidence->push_back(std::move(value));
}

void AppendGovernanceEvidence(
    NativeSblrSpecializationResult* result,
    const agents::ResourceGovernanceAdmissionResult& governance) {
  result->evidence.insert(result->evidence.end(), governance.evidence.begin(),
                          governance.evidence.end());
  Add(&result->evidence, "native_sblr.resource_governance_action=" +
                             std::string(agents::ResourceGovernanceActionName(
                                 governance.action)));
}

void AppendCompatibilityEvidence(
    NativeSblrSpecializationResult* result,
    const platform::RuntimeCompatibilityResult& compatibility) {
  result->evidence.insert(result->evidence.end(),
                          compatibility.evidence.begin(),
                          compatibility.evidence.end());
}

platform::RuntimeCompatibilityDescriptor NativeRuntimeCompatibility(
    const NativeSblrSpecializationRequest& request) {
  auto descriptor = request.capabilities.runtime_compatibility;
  if (descriptor.route_id.empty()) {
    descriptor = platform::CurrentRuntimeCompatibilityDescriptor(
        "engine.native_sblr_specialization");
  }
  descriptor.route_id = descriptor.route_id.empty()
                            ? "engine.native_sblr_specialization"
                            : descriptor.route_id;
  descriptor.source_component = "engine.native_sblr_specialization";
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
  return descriptor;
}

NativeSblrSpecializationResult Refuse(std::string code,
                                      std::string detail) {
  NativeSblrSpecializationResult result;
  result.ok = false;
  result.fail_closed = true;
  result.route = NativeSblrSpecializationRoute::kRefused;
  result.diagnostic_code = std::move(code);
  result.diagnostic_detail = std::move(detail);
  Add(&result.evidence, "native_sblr.route=refused");
  Add(&result.evidence, "native_sblr.fail_closed=true");
  Add(&result.evidence, "native_sblr.accelerator_only=true");
  Add(&result.evidence, "mga_finality_authority=engine_transaction_inventory");
  Add(&result.evidence, "parser_or_reference_authority=false");
  Add(&result.evidence, "provider_transaction_finality_authority=false");
  Add(&result.evidence, "provider_visibility_authority=false");
  Add(&result.evidence, "provider_security_policy_authority=false");
  Add(&result.evidence, "provider_redaction_policy_authority=false");
  Add(&result.evidence,
      "write_after_stream_finality_or_recovery_authority=false");
  return result;
}

NativeSblrSpecializationResult ScalarFallback(
    const NativeSblrSpecializationRequest& request,
    NativeSblrValueBatch values,
    std::string code,
    std::string detail) {
  NativeSblrSpecializationResult result;
  result.ok = true;
  result.scalar_fallback_used = true;
  result.route = NativeSblrSpecializationRoute::kScalarFallback;
  result.values = std::move(values);
  result.diagnostic_code = std::move(code);
  result.diagnostic_detail = std::move(detail);
  Add(&result.evidence, "native_sblr.route=scalar_fallback");
  Add(&result.evidence, std::string("native_sblr.kind=") +
                            NativeSblrSpecializationKindName(request.kind));
  Add(&result.evidence, "native_sblr.scalar_reference=exact");
  Add(&result.evidence, "native_sblr.accelerator_only=true");
  Add(&result.evidence, "mga_finality_authority=engine_transaction_inventory");
  Add(&result.evidence, "parser_or_reference_authority=false");
  Add(&result.evidence, "provider_transaction_finality_authority=false");
  Add(&result.evidence, "provider_visibility_authority=false");
  Add(&result.evidence, "provider_security_policy_authority=false");
  Add(&result.evidence, "provider_redaction_policy_authority=false");
  Add(&result.evidence,
      "write_after_stream_finality_or_recovery_authority=false");
  return result;
}

NativeSblrSpecializationResult NativeSuccess(
    const NativeSblrSpecializationRequest& request,
    NativeSblrValueBatch values,
    const NativeSblrCompileOutcome& compile,
    const NativeSblrKernelOutcome& kernel) {
  NativeSblrSpecializationResult result;
  result.ok = true;
  result.native_used = true;
  result.route = NativeSblrSpecializationRoute::kNative;
  result.values = std::move(values);
  result.diagnostic_code = "SB_NATIVE_SBLR.OK";
  result.diagnostic_detail = "native_sblr_specialization_selected";
  Add(&result.evidence, "native_sblr.route=native");
  Add(&result.evidence, std::string("native_sblr.kind=") +
                            NativeSblrSpecializationKindName(request.kind));
  Add(&result.evidence, "native_sblr.stable_template_id=" +
                            request.identity.stable_template_id);
  Add(&result.evidence, "native_sblr.template_generation=" +
                            std::to_string(request.identity.template_generation));
  Add(&result.evidence, "native_sblr.provider_id=" +
                            request.provider.manifest.provider_id);
  Add(&result.evidence, "native_sblr.kernel_id=" + compile.kernel_id);
  Add(&result.evidence, "native_sblr.kernel_runtime_id=" + kernel.kernel_id);
  Add(&result.evidence, "native_sblr.scalar_equivalence=verified");
  Add(&result.evidence, "native_sblr.accelerator_only=true");
  Add(&result.evidence, "mga_finality_authority=engine_transaction_inventory");
  Add(&result.evidence, "parser_or_reference_authority=false");
  Add(&result.evidence, "provider_transaction_finality_authority=false");
  Add(&result.evidence, "provider_visibility_authority=false");
  Add(&result.evidence, "provider_security_policy_authority=false");
  Add(&result.evidence, "provider_redaction_policy_authority=false");
  Add(&result.evidence,
      "write_after_stream_finality_or_recovery_authority=false");
  return result;
}

bool ContainsKind(const std::vector<NativeSblrSpecializationKind>& kinds,
                  NativeSblrSpecializationKind kind) {
  return std::find(kinds.begin(), kinds.end(), kind) != kinds.end();
}

bool ContainsString(const std::vector<std::string>& values,
                    const std::string& value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

bool HasRequiredCapabilities(const NativeSblrCapabilityBinding& required,
                             const NativeSblrProviderManifest& provider) {
  if (!required.capability_manifest_safe) {
    return false;
  }
  if (!required.required_engine_abi_id.empty() &&
      provider.engine_abi_id != required.required_engine_abi_id) {
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
  return true;
}

bool UnsafeProviderAuthority(const NativeSblrProviderManifest& provider) {
  return !provider.safe_to_execute ||
         provider.claims_transaction_finality_authority ||
         provider.claims_visibility_authority ||
         provider.claims_security_policy_authority ||
         provider.claims_redaction_policy_authority ||
         provider.claims_parser_or_reference_authority;
}

bool HotEnough(const NativeSblrHotness& hotness) {
  const bool invocation_hot =
      hotness.minimum_invocations != 0 &&
      hotness.observed_invocations >= hotness.minimum_invocations;
  const bool row_hot = hotness.minimum_rows != 0 &&
                       hotness.observed_rows >= hotness.minimum_rows;
  const bool expression_hot =
      hotness.minimum_expressions != 0 &&
      hotness.observed_expressions >= hotness.minimum_expressions;
  return invocation_hot || row_hot || expression_hot;
}

NativeSblrValueBatch ExecuteScalar(
    const NativeSblrSpecializationRequest& request,
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

NativeSblrSpecializationResult ValidateInput(
    const NativeSblrSpecializationRequest& request) {
  if (!KnownKind(request.kind)) {
    return Refuse("SB_NATIVE_SBLR.CORRUPT_INPUT_REFUSED",
                  "corrupt_or_unknown_specialization_kind");
  }
  if (request.identity.stable_template_id.empty() ||
      request.identity.sblr_digest.empty() ||
      request.identity.template_generation == 0 ||
      request.identity.expected_template_generation == 0 ||
      request.input.row_count != request.input.values.size()) {
    return Refuse("SB_NATIVE_SBLR.CORRUPT_INPUT_REFUSED",
                  "stable_template_identity_or_input_batch_invalid");
  }
  if (!request.scalar_reference) {
    return Refuse("SB_NATIVE_SBLR.SCALAR_REFERENCE_REQUIRED",
                  "exact_scalar_reference_required");
  }
  return {};
}

bool SameValues(const NativeSblrValueBatch& left,
                const NativeSblrValueBatch& right) {
  return left.values == right.values;
}

}  // namespace

const char* NativeSblrSpecializationKindName(
    NativeSblrSpecializationKind kind) {
  switch (kind) {
    case NativeSblrSpecializationKind::kPredicate:
      return "predicate";
    case NativeSblrSpecializationKind::kProjection:
      return "projection";
    case NativeSblrSpecializationKind::kRowDecode:
      return "row_decode";
    case NativeSblrSpecializationKind::kPathExtraction:
      return "path_extraction";
    case NativeSblrSpecializationKind::kDistanceScoring:
      return "distance_scoring";
    case NativeSblrSpecializationKind::kAggregate:
      return "aggregate";
    case NativeSblrSpecializationKind::kUnknown:
      break;
  }
  return "unknown";
}

const char* NativeSblrSpecializationRouteName(
    NativeSblrSpecializationRoute route) {
  switch (route) {
    case NativeSblrSpecializationRoute::kScalarFallback:
      return "scalar_fallback";
    case NativeSblrSpecializationRoute::kNative:
      return "native";
    case NativeSblrSpecializationRoute::kRefused:
      return "refused";
  }
  return "refused";
}

NativeSblrSpecializationResult ExecuteNativeSblrSpecialization(
    const NativeSblrSpecializationRequest& request) {
  auto input_check = ValidateInput(request);
  if (input_check.fail_closed) {
    return input_check;
  }

  bool scalar_ok = false;
  auto scalar_values = ExecuteScalar(request, &scalar_ok);
  if (!scalar_ok) {
    return Refuse("SB_NATIVE_SBLR.SCALAR_REFERENCE_REQUIRED",
                  "exact_scalar_reference_failed");
  }

  auto governance_request = request.resource_governance;
  governance_request.expected_family =
      agents::ResourceGovernanceFamily::kPreparedNativeSpecialization;
  const auto governance = agents::AdmitResourceGovernance(governance_request);
  if (governance.action == agents::ResourceGovernanceAction::kFailClosed) {
    auto result = Refuse("SB_NATIVE_SBLR.ODF106_QUOTA_REFUSED",
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
        "SB_NATIVE_SBLR.ODF106_EXACT_SCALAR_FALLBACK",
        governance.diagnostic_code);
    AppendGovernanceEvidence(&result, governance);
    return result;
  }

  auto append_governance = [&](NativeSblrSpecializationResult result) {
    AppendGovernanceEvidence(&result, governance);
    return result;
  };

  if (request.identity.template_generation !=
      request.identity.expected_template_generation) {
    return append_governance(ScalarFallback(
        request, std::move(scalar_values),
        "SB_NATIVE_SBLR.STALE_TEMPLATE_GENERATION_FALLBACK",
        "stable_template_generation_mismatch"));
  }
  if (!HotEnough(request.hotness)) {
    return append_governance(ScalarFallback(
        request, std::move(scalar_values),
        "SB_NATIVE_SBLR.COLD_TEMPLATE_FALLBACK",
        "template_not_hot_enough_for_native_specialization"));
  }
  if (request.epochs.security_epoch != request.epochs.expected_security_epoch ||
      request.epochs.redaction_epoch != request.epochs.expected_redaction_epoch) {
    return append_governance(ScalarFallback(
        request, std::move(scalar_values),
        "SB_NATIVE_SBLR.UNSTABLE_SECURITY_REDACTION_EPOCH_FALLBACK",
        "security_or_redaction_epoch_changed"));
  }
  if (request.cancellation_requested || request.quota_exhausted) {
    return append_governance(ScalarFallback(
        request, std::move(scalar_values),
        "SB_NATIVE_SBLR.CANCELLED_OR_QUOTA_FALLBACK",
        "native_specialization_cancelled_or_quota_exhausted"));
  }
  if (request.provider.manifest.provider_id.empty() ||
      !request.provider.compile || !request.provider.run) {
    return append_governance(ScalarFallback(
        request, std::move(scalar_values),
        "SB_NATIVE_SBLR.PROVIDER_UNSUPPORTED_FALLBACK",
        "safe_native_provider_missing"));
  }
  if (UnsafeProviderAuthority(request.provider.manifest)) {
    return append_governance(
        Refuse("SB_NATIVE_SBLR.UNSAFE_PROVIDER_AUTHORITY",
               "native_provider_claims_forbidden_authority"));
  }
  if (!ContainsKind(request.provider.manifest.supported_kinds, request.kind)) {
    return append_governance(ScalarFallback(
        request, std::move(scalar_values),
        "SB_NATIVE_SBLR.UNSUPPORTED_KIND_FALLBACK",
        "specialization_kind_not_supported_by_provider"));
  }
  const auto compatibility = platform::NegotiateRuntimeCompatibility(
      NativeRuntimeCompatibility(request));
  const bool explicit_runtime_descriptor =
      !request.capabilities.runtime_compatibility.route_id.empty();
  auto append_compatibility = [&](NativeSblrSpecializationResult result) {
    AppendGovernanceEvidence(&result, governance);
    AppendCompatibilityEvidence(&result, compatibility);
    return result;
  };
  if (compatibility.action ==
      platform::RuntimeCompatibilityAction::exact_scalar_fallback) {
    return append_compatibility(ScalarFallback(
        request, std::move(scalar_values),
        explicit_runtime_descriptor
            ? "SB_NATIVE_SBLR.ODF107_RUNTIME_COMPATIBILITY_FALLBACK"
            : "SB_NATIVE_SBLR.ABI_CPU_CAPABILITY_MISMATCH_FALLBACK",
        compatibility.diagnostic_code));
  }
  if (compatibility.action ==
      platform::RuntimeCompatibilityAction::fail_closed) {
    return append_compatibility(Refuse(
        "SB_NATIVE_SBLR.ODF107_RUNTIME_COMPATIBILITY_REFUSED",
        compatibility.diagnostic_code));
  }
  if (!HasRequiredCapabilities(request.capabilities,
                               request.provider.manifest)) {
    return append_compatibility(ScalarFallback(
        request, std::move(scalar_values),
        "SB_NATIVE_SBLR.ABI_CPU_CAPABILITY_MISMATCH_FALLBACK",
        "native_provider_abi_cpu_or_capability_mismatch"));
  }

  NativeSblrCompileOutcome compile;
  try {
    compile = request.provider.compile(request);
  } catch (const std::exception& ex) {
    return append_compatibility(ScalarFallback(
        request, std::move(scalar_values),
        "SB_NATIVE_SBLR.COMPILE_FAILED_FALLBACK",
        std::string("native_compile_provider_exception:") + ex.what()));
  } catch (...) {
    return append_compatibility(ScalarFallback(
        request, std::move(scalar_values),
        "SB_NATIVE_SBLR.COMPILE_FAILED_FALLBACK",
        "native_compile_provider_exception"));
  }
  if (compile.cancelled_or_quota) {
    return append_compatibility(ScalarFallback(
        request, std::move(scalar_values),
        "SB_NATIVE_SBLR.CANCELLED_OR_QUOTA_FALLBACK",
        compile.diagnostic_detail.empty()
            ? "native_compile_cancelled_or_quota_exhausted"
            : compile.diagnostic_detail));
  }
  if (!compile.ok || compile.kernel_id.empty()) {
    return append_compatibility(ScalarFallback(
        request, std::move(scalar_values),
        "SB_NATIVE_SBLR.COMPILE_FAILED_FALLBACK",
        compile.diagnostic_detail.empty() ? "native_compile_failed"
                                          : compile.diagnostic_detail));
  }

  NativeSblrKernelOutcome kernel;
  try {
    kernel = request.provider.run(request, compile);
  } catch (const std::exception& ex) {
    return append_compatibility(Refuse(
        "SB_NATIVE_SBLR.RUNTIME_MISMATCH_REFUSED",
        std::string("native_kernel_provider_exception:") + ex.what()));
  } catch (...) {
    return append_compatibility(Refuse(
        "SB_NATIVE_SBLR.RUNTIME_MISMATCH_REFUSED",
        "native_kernel_provider_exception"));
  }
  if (kernel.cancelled_or_quota) {
    return append_compatibility(ScalarFallback(
        request, std::move(scalar_values),
        "SB_NATIVE_SBLR.CANCELLED_OR_QUOTA_FALLBACK",
        kernel.diagnostic_detail.empty()
            ? "native_runtime_cancelled_or_quota_exhausted"
            : kernel.diagnostic_detail));
  }
  if (!kernel.ok) {
    return append_compatibility(Refuse(
        "SB_NATIVE_SBLR.RUNTIME_MISMATCH_REFUSED",
        kernel.diagnostic_detail.empty() ? "native_kernel_runtime_failed"
                                         : kernel.diagnostic_detail));
  }
  if (kernel.kernel_id != compile.kernel_id ||
      kernel.template_generation != request.identity.template_generation ||
      kernel.security_epoch != request.epochs.security_epoch ||
      kernel.redaction_epoch != request.epochs.redaction_epoch) {
    return append_compatibility(Refuse(
        "SB_NATIVE_SBLR.RUNTIME_MISMATCH_REFUSED",
        "native_kernel_identity_or_epoch_mismatch"));
  }
  if (!SameValues(kernel.values, scalar_values)) {
    return append_compatibility(
        Refuse("SB_NATIVE_SBLR.RESULT_MISMATCH_REFUSED",
               "native_result_does_not_match_exact_scalar_reference"));
  }

  return append_compatibility(
      NativeSuccess(request, std::move(kernel.values), compile, kernel));
}

}  // namespace scratchbird::engine::native_compile
