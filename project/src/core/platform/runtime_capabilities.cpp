// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "runtime_capabilities.hpp"

#include "scratchbird/core/platform/noncluster_engine_profile.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace scratchbird::core::platform {
namespace {

RuntimeCapability MakeCapability(std::string key,
                                 std::string description,
                                 CapabilityRequirement requirement,
                                 CapabilityState state,
                                 std::string provider,
                                 std::string diagnostic_code) {
  RuntimeCapability capability;
  capability.key = std::move(key);
  capability.description = std::move(description);
  capability.requirement = requirement;
  capability.state = state;
  capability.provider = std::move(provider);
  capability.diagnostic_code = std::move(diagnostic_code);
  return capability;
}

CapabilityState PresentIf(bool present) {
  return present ? CapabilityState::present : CapabilityState::absent;
}

CapabilityRequirement MandatoryForReleaseComplete() {
  return SCRATCHBIRD_NONCLUSTER_ENGINE_RELEASE_COMPLETE
             ? CapabilityRequirement::mandatory
             : CapabilityRequirement::optional;
}

std::string BoolName(bool value) {
  return value ? "true" : "false";
}

bool ContainsString(const std::vector<std::string>& values,
                    const std::string& value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

void AddCompatibilityEvidence(RuntimeCompatibilityResult* result,
                              const RuntimeCompatibilityDescriptor& descriptor) {
  result->evidence.push_back("runtime_compatibility.route=odf107");
  result->evidence.push_back("runtime_compatibility.route_id=" +
                             descriptor.route_id);
  result->evidence.push_back("runtime_compatibility.source_component=" +
                             descriptor.source_component);
  result->evidence.push_back("runtime_compatibility.action=" +
                             std::string(RuntimeCompatibilityActionName(
                                 result->action)));
  result->evidence.push_back("runtime_compatibility.required_endian=" +
                             std::string(RuntimeEndianName(
                                 descriptor.required_endian)));
  result->evidence.push_back("runtime_compatibility.provider_endian=" +
                             std::string(RuntimeEndianName(
                                 descriptor.provider_endian)));
  result->evidence.push_back("runtime_compatibility.required_alignment=" +
                             std::to_string(descriptor.required_alignment));
  result->evidence.push_back("runtime_compatibility.provider_alignment=" +
                             std::to_string(descriptor.provider_alignment));
  result->evidence.push_back("runtime_compatibility.required_engine_abi_id=" +
                             descriptor.required_engine_abi_id);
  result->evidence.push_back("runtime_compatibility.provider_engine_abi_id=" +
                             descriptor.provider_engine_abi_id);
  result->evidence.push_back("runtime_compatibility.required_architecture=" +
                             descriptor.required_architecture);
  result->evidence.push_back("runtime_compatibility.provider_architecture=" +
                             descriptor.provider_architecture);
  result->evidence.push_back(
      "runtime_compatibility.required_runtime_identity_id=" +
      descriptor.required_runtime_identity_id);
  result->evidence.push_back(
      "runtime_compatibility.provider_runtime_identity_id=" +
      descriptor.provider_runtime_identity_id);
  result->evidence.push_back(
      "runtime_compatibility.required_runtime_generation=" +
      std::to_string(descriptor.required_runtime_generation));
  result->evidence.push_back(
      "runtime_compatibility.provider_runtime_generation=" +
      std::to_string(descriptor.provider_runtime_generation));
  result->evidence.push_back(
      "runtime_compatibility.required_compatibility_epoch=" +
      std::to_string(descriptor.required_compatibility_epoch));
  result->evidence.push_back(
      "runtime_compatibility.provider_compatibility_epoch=" +
      std::to_string(descriptor.provider_compatibility_epoch));
  result->evidence.push_back(
      "runtime_compatibility.deterministic_scalar_fallback_required=" +
      BoolName(descriptor.deterministic_scalar_fallback_required));
  result->evidence.push_back(
      "runtime_compatibility.deterministic_scalar_fallback_available=" +
      BoolName(descriptor.deterministic_scalar_fallback_available));
  result->evidence.push_back(
      "runtime_compatibility.accelerator_requested=" +
      BoolName(descriptor.accelerator_requested));
  for (const auto& feature : descriptor.required_cpu_features) {
    result->evidence.push_back("runtime_compatibility.required_cpu_feature=" +
                               feature);
  }
  for (const auto& feature : descriptor.provider_cpu_features) {
    result->evidence.push_back("runtime_compatibility.provider_cpu_feature=" +
                               feature);
  }
  for (const auto& capability :
       descriptor.required_accelerator_capabilities) {
    result->evidence.push_back(
        "runtime_compatibility.required_accelerator_capability=" +
        capability);
  }
  for (const auto& capability :
       descriptor.provider_accelerator_capabilities) {
    result->evidence.push_back(
        "runtime_compatibility.provider_accelerator_capability=" +
        capability);
  }
}

RuntimeCompatibilityResult MakeCompatibilityResult(
    RuntimeCompatibilityDescriptor descriptor,
    RuntimeCompatibilityAction action,
    std::string diagnostic_code,
    std::string diagnostic_detail) {
  RuntimeCompatibilityResult result;
  result.action = action;
  result.ok = action == RuntimeCompatibilityAction::admit;
  result.fallback_required =
      action == RuntimeCompatibilityAction::exact_scalar_fallback;
  result.fail_closed = action == RuntimeCompatibilityAction::fail_closed;
  result.status = result.fail_closed
                      ? Status{StatusCode::platform_required_feature_missing,
                               Severity::error,
                               Subsystem::platform}
                      : Status{StatusCode::ok, Severity::info,
                               Subsystem::platform};
  result.diagnostic_code = std::move(diagnostic_code);
  result.diagnostic_detail = std::move(diagnostic_detail);
  AddCompatibilityEvidence(&result, descriptor);
  if (!result.ok) {
    result.evidence.push_back("runtime_compatibility.mismatch_reason=" +
                              result.diagnostic_code);
  }
  result.diagnostic =
      MakeDiagnostic(result.status.code,
                     result.status.severity,
                     result.status.subsystem,
                     result.diagnostic_code,
                     result.ok ? "runtime.compatibility.admitted"
                               : (result.fail_closed
                                      ? "runtime.compatibility.refused"
                                      : "runtime.compatibility.fallback"),
                     {{"route_id", descriptor.route_id},
                      {"detail", result.diagnostic_detail},
                      {"action", RuntimeCompatibilityActionName(action)}},
                     {},
                     descriptor.source_component.empty()
                         ? "core.platform.runtime_compatibility"
                         : descriptor.source_component,
                     result.ok ? std::string{}
                               : result.diagnostic_detail);
  return result;
}

RuntimeCompatibilityAction MismatchAction(
    const RuntimeCompatibilityDescriptor& descriptor) {
  if (descriptor.fail_closed_on_mismatch ||
      (descriptor.deterministic_scalar_fallback_required &&
       !descriptor.deterministic_scalar_fallback_available)) {
    return RuntimeCompatibilityAction::fail_closed;
  }
  return RuntimeCompatibilityAction::exact_scalar_fallback;
}

}  // namespace

bool RuntimeCapabilityManifest::mandatory_ok() const {
  for (const RuntimeCapability& capability : capabilities) {
    if (capability.requirement == CapabilityRequirement::mandatory &&
        capability.state != CapabilityState::present) {
      return false;
    }
  }
  return true;
}

const char* CapabilityRequirementName(CapabilityRequirement requirement) {
  switch (requirement) {
    case CapabilityRequirement::mandatory: return "mandatory";
    case CapabilityRequirement::optional: return "optional";
  }
  return "unknown";
}

const char* CapabilityStateName(CapabilityState state) {
  switch (state) {
    case CapabilityState::present: return "present";
    case CapabilityState::absent: return "absent";
    case CapabilityState::disabled: return "disabled";
    case CapabilityState::unknown: return "unknown";
  }
  return "unknown";
}

const char* RuntimeEndianName(RuntimeEndian endian) {
  switch (endian) {
    case RuntimeEndian::little:
      return "little";
    case RuntimeEndian::big:
      return "big";
    case RuntimeEndian::unknown:
      return "unknown";
  }
  return "unknown";
}

const char* RuntimeCompatibilityActionName(
    RuntimeCompatibilityAction action) {
  switch (action) {
    case RuntimeCompatibilityAction::admit:
      return "admit";
    case RuntimeCompatibilityAction::exact_scalar_fallback:
      return "exact_scalar_fallback";
    case RuntimeCompatibilityAction::fail_closed:
      return "fail_closed";
  }
  return "fail_closed";
}

RuntimeEndian CurrentRuntimeEndian() {
  return kHostEndian == HostEndian::little ? RuntimeEndian::little
                                           : RuntimeEndian::big;
}

std::string CurrentRuntimeArchitecture() {
#if defined(__x86_64__) || defined(_M_X64)
  return "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
  return "aarch64";
#elif defined(__arm__) || defined(_M_ARM)
  return "arm";
#elif defined(__i386__) || defined(_M_IX86)
  return "x86";
#elif defined(__powerpc64__) || defined(__ppc64__)
  return "ppc64";
#elif defined(__riscv) && (__riscv_xlen == 64)
  return "riscv64";
#else
  return "unknown";
#endif
}

std::string CurrentRuntimeIdentityId() {
  return "sb_runtime." + CurrentRuntimeArchitecture() + "." +
         RuntimeEndianName(CurrentRuntimeEndian());
}

u64 CurrentRuntimeCompatibilityGeneration() {
  return 107;
}

u64 CurrentRuntimeCompatibilityEpoch() {
  return 1;
}

std::vector<std::string> CurrentRuntimeCpuFeatures() {
  std::vector<std::string> features = {
      "scalar_exact",
      "deterministic_scalar_fallback",
      "memcpy_unaligned_load_store",
  };
  features.push_back(CurrentRuntimeArchitecture());
  features.push_back(kHostEndian == HostEndian::little ? "little_endian"
                                                       : "big_endian");
#if defined(__SSE2__) || defined(_M_X64) || \
    (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
  features.push_back("sse2");
#endif
#if defined(__SSSE3__)
  features.push_back("ssse3");
#endif
#if defined(__SSE4_2__)
  features.push_back("sse4_2");
#endif
#if defined(__AVX__)
  features.push_back("avx");
#endif
#if defined(__AVX2__)
  features.push_back("avx2");
#endif
#if defined(__AVX512F__)
  features.push_back("avx512f");
#endif
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
  features.push_back("neon");
#endif
  return features;
}

RuntimeCompatibilityDescriptor CurrentRuntimeCompatibilityDescriptor(
    std::string route_id) {
  RuntimeCompatibilityDescriptor descriptor;
  descriptor.route_id = std::move(route_id);
  descriptor.source_component = "core.platform.runtime_compatibility";
  descriptor.required_endian = CurrentRuntimeEndian();
  descriptor.provider_endian = CurrentRuntimeEndian();
  descriptor.required_alignment = 1;
  descriptor.provider_alignment = alignof(std::max_align_t);
  descriptor.required_engine_abi_id = "sb_engine_abi_v3";
  descriptor.provider_engine_abi_id = "sb_engine_abi_v3";
  descriptor.required_architecture = CurrentRuntimeArchitecture();
  descriptor.provider_architecture = CurrentRuntimeArchitecture();
  descriptor.required_runtime_identity_id = CurrentRuntimeIdentityId();
  descriptor.provider_runtime_identity_id = CurrentRuntimeIdentityId();
  descriptor.required_runtime_generation =
      CurrentRuntimeCompatibilityGeneration();
  descriptor.provider_runtime_generation =
      CurrentRuntimeCompatibilityGeneration();
  descriptor.required_compatibility_epoch =
      CurrentRuntimeCompatibilityEpoch();
  descriptor.provider_compatibility_epoch =
      CurrentRuntimeCompatibilityEpoch();
  descriptor.provider_cpu_features = CurrentRuntimeCpuFeatures();
  descriptor.deterministic_scalar_fallback_available = true;
  return descriptor;
}

RuntimeCompatibilityResult NegotiateRuntimeCompatibility(
    RuntimeCompatibilityDescriptor descriptor) {
  if (descriptor.route_id.empty()) {
    descriptor.route_id = "runtime.compatibility";
  }
  if (descriptor.source_component.empty()) {
    descriptor.source_component = "core.platform.runtime_compatibility";
  }
  if (descriptor.required_endian == RuntimeEndian::unknown) {
    descriptor.required_endian = CurrentRuntimeEndian();
  }
  if (descriptor.provider_endian == RuntimeEndian::unknown) {
    descriptor.provider_endian = CurrentRuntimeEndian();
  }
  if (descriptor.required_alignment == 0) {
    descriptor.required_alignment = 1;
  }
  if (descriptor.provider_alignment == 0) {
    descriptor.provider_alignment = alignof(std::max_align_t);
  }
  if (descriptor.required_engine_abi_id.empty()) {
    descriptor.required_engine_abi_id = "sb_engine_abi_v3";
  }
  if (descriptor.provider_engine_abi_id.empty()) {
    descriptor.provider_engine_abi_id = "sb_engine_abi_v3";
  }
  if (descriptor.required_architecture.empty()) {
    descriptor.required_architecture = CurrentRuntimeArchitecture();
  }
  if (descriptor.provider_architecture.empty()) {
    descriptor.provider_architecture = CurrentRuntimeArchitecture();
  }
  if (descriptor.required_runtime_identity_id.empty()) {
    descriptor.required_runtime_identity_id = CurrentRuntimeIdentityId();
  }
  if (descriptor.provider_runtime_identity_id.empty()) {
    descriptor.provider_runtime_identity_id = CurrentRuntimeIdentityId();
  }
  if (descriptor.required_runtime_generation == 0) {
    descriptor.required_runtime_generation =
        CurrentRuntimeCompatibilityGeneration();
  }
  if (descriptor.provider_runtime_generation == 0) {
    descriptor.provider_runtime_generation =
        CurrentRuntimeCompatibilityGeneration();
  }
  if (descriptor.required_compatibility_epoch == 0) {
    descriptor.required_compatibility_epoch =
        CurrentRuntimeCompatibilityEpoch();
  }
  if (descriptor.provider_compatibility_epoch == 0) {
    descriptor.provider_compatibility_epoch =
        CurrentRuntimeCompatibilityEpoch();
  }
  if (descriptor.provider_cpu_features.empty()) {
    descriptor.provider_cpu_features = CurrentRuntimeCpuFeatures();
  }

  auto mismatch = [&](std::string code, std::string detail) {
    return MakeCompatibilityResult(descriptor, MismatchAction(descriptor),
                                   std::move(code), std::move(detail));
  };

  if (descriptor.required_endian != descriptor.provider_endian) {
    return mismatch("SB_RUNTIME_COMPATIBILITY.ENDIAN_MISMATCH",
                    "runtime_endian_mismatch");
  }
  if (descriptor.required_alignment > descriptor.provider_alignment) {
    return mismatch("SB_RUNTIME_COMPATIBILITY.ALIGNMENT_MISMATCH",
                    "runtime_alignment_mismatch");
  }
  if (descriptor.required_engine_abi_id != descriptor.provider_engine_abi_id) {
    return mismatch("SB_RUNTIME_COMPATIBILITY.ABI_MISMATCH",
                    "engine_abi_mismatch");
  }
  if (descriptor.required_architecture != descriptor.provider_architecture) {
    return mismatch("SB_RUNTIME_COMPATIBILITY.ARCHITECTURE_MISMATCH",
                    "runtime_architecture_mismatch");
  }
  if (descriptor.required_runtime_identity_id !=
      descriptor.provider_runtime_identity_id) {
    return mismatch("SB_RUNTIME_COMPATIBILITY.RUNTIME_IDENTITY_MISMATCH",
                    "runtime_identity_mismatch");
  }
  if (descriptor.required_runtime_generation !=
      descriptor.provider_runtime_generation ||
      descriptor.required_compatibility_epoch !=
          descriptor.provider_compatibility_epoch) {
    return mismatch("SB_RUNTIME_COMPATIBILITY.RUNTIME_GENERATION_MISMATCH",
                    "runtime_generation_or_epoch_mismatch");
  }
  for (const auto& feature : descriptor.required_cpu_features) {
    if (!ContainsString(descriptor.provider_cpu_features, feature)) {
      return mismatch("SB_RUNTIME_COMPATIBILITY.CPU_FEATURE_MISMATCH",
                      "missing_cpu_feature:" + feature);
    }
  }
  for (const auto& capability :
       descriptor.required_accelerator_capabilities) {
    if (!ContainsString(descriptor.provider_accelerator_capabilities,
                        capability)) {
      return mismatch(
          "SB_RUNTIME_COMPATIBILITY.ACCELERATOR_CAPABILITY_MISMATCH",
          "missing_accelerator_capability:" + capability);
    }
  }
  if (descriptor.deterministic_scalar_fallback_required &&
      !descriptor.deterministic_scalar_fallback_available) {
    return MakeCompatibilityResult(
        descriptor, RuntimeCompatibilityAction::fail_closed,
        "SB_RUNTIME_COMPATIBILITY.SCALAR_FALLBACK_UNAVAILABLE",
        "deterministic_scalar_fallback_required");
  }
  return MakeCompatibilityResult(descriptor,
                                 RuntimeCompatibilityAction::admit,
                                 "SB_RUNTIME_COMPATIBILITY.OK",
                                 "runtime_compatibility_admitted");
}

RuntimeCapabilityManifest DetectRuntimeCapabilities() {
  RuntimeCapabilityManifest manifest;

  manifest.capabilities.push_back(MakeCapability(
      "profile.noncluster_engine.release_complete",
      "release-complete non-cluster engine build profile",
      MandatoryForReleaseComplete(),
      SCRATCHBIRD_NONCLUSTER_ENGINE_RELEASE_COMPLETE ? CapabilityState::present : CapabilityState::disabled,
      SCRATCHBIRD_NONCLUSTER_ENGINE_PROFILE,
      "CAPABILITY.NONCLUSTER_RELEASE_COMPLETE_PROFILE_INACTIVE"));

  manifest.capabilities.push_back(MakeCapability(
      "numeric.int128",
      "compiler/library support for signed 128-bit integer operations",
      CapabilityRequirement::mandatory,
      PresentIf(kCompilerHasInt128),
      kCompilerHasInt128 ? "compiler::__int128_t" : "missing",
      "CAPABILITY.NUMERIC_INT128_MISSING"));

  manifest.capabilities.push_back(MakeCapability(
      "numeric.uint128",
      "compiler/library support for unsigned 128-bit integer operations",
      CapabilityRequirement::mandatory,
      PresentIf(kCompilerHasInt128),
      kCompilerHasInt128 ? "compiler::__uint128_t" : "missing",
      "CAPABILITY.NUMERIC_UINT128_MISSING"));

  manifest.capabilities.push_back(MakeCapability(
      "numeric.decimal",
      "mandatory deterministic exact decimal support up to 38 digits",
      CapabilityRequirement::mandatory,
      PresentIf(kCompilerHasInt128),
      kCompilerHasInt128 ? "sbl_numeric.decimal128" : "missing",
      "CAPABILITY.NUMERIC_DECIMAL_MISSING"));

  manifest.capabilities.push_back(MakeCapability(
      "numeric.decimal_float",
      "mandatory decimal-float canonicalization and special-value support",
      CapabilityRequirement::mandatory,
      PresentIf(kCompilerHasInt128),
      kCompilerHasInt128 ? "sbl_numeric.decfloat128" : "missing",
      "CAPABILITY.NUMERIC_DECFLOAT_MISSING"));

#if defined(SCRATCHBIRD_HAS_REAL128_LIBRARY) && SCRATCHBIRD_HAS_REAL128_LIBRARY
  constexpr bool kHasReal128Library = true;
#else
  constexpr bool kHasReal128Library = false;
#endif
  manifest.capabilities.push_back(MakeCapability(
      "numeric.real128",
      "mandatory real128 floating-point library support",
      MandatoryForReleaseComplete(),
      PresentIf(kHasReal128Library),
#if defined(SCRATCHBIRD_REAL128_PROVIDER)
      SCRATCHBIRD_REAL128_PROVIDER,
#else
      kHasReal128Library ? "configured" : "missing",
#endif
      "CAPABILITY.NUMERIC_REAL128_MISSING"));

#if defined(SCRATCHBIRD_HAS_LLVM) && SCRATCHBIRD_HAS_LLVM
  constexpr bool kHasLlvm = true;
#else
  constexpr bool kHasLlvm = false;
#endif
#if defined(SCRATCHBIRD_LLVM_LIBRARY_FOUND) && SCRATCHBIRD_LLVM_LIBRARY_FOUND
  constexpr bool kLlvmLibraryFound = true;
#else
  constexpr bool kLlvmLibraryFound = false;
#endif
#if defined(SCRATCHBIRD_LLVM_VERSION_OK) && SCRATCHBIRD_LLVM_VERSION_OK
  constexpr bool kLlvmVersionOk = true;
#else
  constexpr bool kLlvmVersionOk = false;
#endif
  manifest.capabilities.push_back(MakeCapability(
      "compiler.llvm",
      "mandatory LLVM library support for JIT and AOT infrastructure",
      MandatoryForReleaseComplete(),
      PresentIf(kHasLlvm),
#if defined(SCRATCHBIRD_LLVM_PROVIDER)
      SCRATCHBIRD_LLVM_PROVIDER,
#else
      kHasLlvm ? "configured" : "missing",
#endif
      (kLlvmLibraryFound && !kLlvmVersionOk) ? "CAPABILITY.LLVM_VERSION_UNSUPPORTED" : "CAPABILITY.LLVM_MISSING"));

#if defined(SCRATCHBIRD_HAS_CUDA) && SCRATCHBIRD_HAS_CUDA
  constexpr bool kHasCuda = true;
#else
  constexpr bool kHasCuda = false;
#endif
  manifest.capabilities.push_back(MakeCapability(
      "gpu.cuda",
      "optional NVIDIA CUDA GPU execution provider",
      CapabilityRequirement::optional,
      PresentIf(kHasCuda),
      kHasCuda ? "cuda" : "missing",
      "CAPABILITY.GPU_CUDA_UNAVAILABLE"));

#if defined(SCRATCHBIRD_HAS_HIP) && SCRATCHBIRD_HAS_HIP
  constexpr bool kHasHip = true;
#else
  constexpr bool kHasHip = false;
#endif
  manifest.capabilities.push_back(MakeCapability(
      "gpu.hip",
      "optional AMD HIP GPU execution provider",
      CapabilityRequirement::optional,
      PresentIf(kHasHip),
      kHasHip ? "hip" : "missing",
      "CAPABILITY.GPU_HIP_UNAVAILABLE"));

#if defined(SCRATCHBIRD_HAS_OPENCL) && SCRATCHBIRD_HAS_OPENCL
  constexpr bool kHasOpenCl = true;
#else
  constexpr bool kHasOpenCl = false;
#endif
  manifest.capabilities.push_back(MakeCapability(
      "gpu.opencl",
      "optional OpenCL GPU execution provider",
      CapabilityRequirement::optional,
      PresentIf(kHasOpenCl),
      kHasOpenCl ? "opencl" : "missing",
      "CAPABILITY.GPU_OPENCL_UNAVAILABLE"));

  return manifest;
}

RuntimeCapabilityCheck CheckMandatoryRuntimeCapabilities() {
  RuntimeCapabilityCheck check;
  check.manifest = DetectRuntimeCapabilities();
  check.status = {StatusCode::ok, Severity::info, Subsystem::platform};

  for (const RuntimeCapability& capability : check.manifest.capabilities) {
    if (capability.requirement == CapabilityRequirement::mandatory &&
        capability.state != CapabilityState::present) {
      check.diagnostics.push_back(MakeCapabilityDiagnostic(capability));
    }
  }

  if (!check.diagnostics.empty()) {
    check.status = {StatusCode::platform_required_feature_missing,
                    Severity::fatal,
                    Subsystem::platform};
  }

  return check;
}

DiagnosticRecord MakeCapabilityDiagnostic(const RuntimeCapability& capability) {
  return MakeDiagnostic(StatusCode::platform_required_feature_missing,
                        Severity::fatal,
                        Subsystem::platform,
                        capability.diagnostic_code,
                        "runtime.capability.mandatory_missing",
                        {{"capability", capability.key},
                         {"requirement", CapabilityRequirementName(capability.requirement)},
                         {"state", CapabilityStateName(capability.state)},
                         {"provider", capability.provider}},
                        {},
                        "core.platform.capabilities");
}

}  // namespace scratchbird::core::platform
