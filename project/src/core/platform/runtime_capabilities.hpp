// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "runtime_platform.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::platform {

enum class CapabilityRequirement : u8 {
  mandatory,
  optional
};

enum class CapabilityState : u8 {
  present,
  absent,
  disabled,
  unknown
};

struct RuntimeCapability {
  std::string key;
  std::string description;
  CapabilityRequirement requirement = CapabilityRequirement::optional;
  CapabilityState state = CapabilityState::unknown;
  std::string provider;
  std::string diagnostic_code;
};

struct RuntimeCapabilityManifest {
  std::vector<RuntimeCapability> capabilities;

  bool mandatory_ok() const;
};

struct RuntimeCapabilityCheck {
  Status status;
  RuntimeCapabilityManifest manifest;
  std::vector<DiagnosticRecord> diagnostics;

  bool ok() const {
    return status.ok() && manifest.mandatory_ok() && diagnostics.empty();
  }
};

enum class RuntimeEndian : u8 {
  unknown,
  little,
  big
};

enum class RuntimeCompatibilityAction : u8 {
  admit,
  exact_scalar_fallback,
  fail_closed
};

struct RuntimeCompatibilityDescriptor {
  std::string route_id;
  std::string source_component;

  RuntimeEndian required_endian = RuntimeEndian::unknown;
  RuntimeEndian provider_endian = RuntimeEndian::unknown;
  u64 required_alignment = 0;
  u64 provider_alignment = 0;

  std::string required_engine_abi_id;
  std::string provider_engine_abi_id;
  std::string required_architecture;
  std::string provider_architecture;
  std::string required_runtime_identity_id;
  std::string provider_runtime_identity_id;
  u64 required_runtime_generation = 0;
  u64 provider_runtime_generation = 0;
  u64 required_compatibility_epoch = 0;
  u64 provider_compatibility_epoch = 0;

  std::vector<std::string> required_cpu_features;
  std::vector<std::string> provider_cpu_features;
  std::vector<std::string> required_accelerator_capabilities;
  std::vector<std::string> provider_accelerator_capabilities;

  bool accelerator_requested = false;
  bool deterministic_scalar_fallback_required = true;
  bool deterministic_scalar_fallback_available = true;
  bool fail_closed_on_mismatch = false;
};

struct RuntimeCompatibilityResult {
  Status status;
  RuntimeCompatibilityAction action = RuntimeCompatibilityAction::fail_closed;
  bool ok = false;
  bool fallback_required = false;
  bool fail_closed = false;
  std::string diagnostic_code;
  std::string diagnostic_detail;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;
};

const char* CapabilityRequirementName(CapabilityRequirement requirement);
const char* CapabilityStateName(CapabilityState state);
RuntimeCapabilityManifest DetectRuntimeCapabilities();
RuntimeCapabilityCheck CheckMandatoryRuntimeCapabilities();
DiagnosticRecord MakeCapabilityDiagnostic(const RuntimeCapability& capability);
const char* RuntimeEndianName(RuntimeEndian endian);
const char* RuntimeCompatibilityActionName(RuntimeCompatibilityAction action);
RuntimeEndian CurrentRuntimeEndian();
std::string CurrentRuntimeArchitecture();
std::string CurrentRuntimeIdentityId();
u64 CurrentRuntimeCompatibilityGeneration();
u64 CurrentRuntimeCompatibilityEpoch();
std::vector<std::string> CurrentRuntimeCpuFeatures();
RuntimeCompatibilityDescriptor CurrentRuntimeCompatibilityDescriptor(
    std::string route_id);
RuntimeCompatibilityResult NegotiateRuntimeCompatibility(
    RuntimeCompatibilityDescriptor descriptor);

}  // namespace scratchbird::core::platform
