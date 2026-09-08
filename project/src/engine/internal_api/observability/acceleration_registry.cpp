// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "observability/acceleration_registry.hpp"

#include "hash_digest.hpp"
#include "runtime_capabilities.hpp"

#include <algorithm>
#include <string_view>

namespace scratchbird::engine::internal_api {
namespace {

using scratchbird::core::platform::CapabilityRequirementName;
using scratchbird::core::platform::CapabilityStateName;
using scratchbird::core::platform::RuntimeCapability;

void AppendField(std::string* material, std::string_view field) {
  material->append(std::to_string(field.size()));
  material->push_back(':');
  material->append(field);
}

std::string ProviderClass(std::string_view capability) {
  if (capability == "compiler.llvm") return "compiler";
  if (capability.starts_with("gpu.")) return "gpu";
  return "runtime";
}

std::string RedactedProvider(const RuntimeCapability& capability) {
  const std::string_view state = CapabilityStateName(capability.state);
  if (state == "present") return "engine_detected";
  if (state == "disabled") return "disabled";
  if (state == "absent") return "not_present";
  return "unknown";
}

bool IsAccelerationCapability(std::string_view capability) {
  return capability == "compiler.llvm" || capability.starts_with("gpu.");
}

std::shared_ptr<const EngineAccelerationRegistrySnapshot> BuildSnapshot() {
  auto snapshot = std::make_shared<EngineAccelerationRegistrySnapshot>();
  snapshot->generation =
      scratchbird::core::platform::CurrentRuntimeCompatibilityGeneration();
  snapshot->rows.push_back({"execution.interpreter",
                            "interpreter",
                            "present",
                            "mandatory",
                            "scratchbird_engine",
                            "",
                            snapshot->generation});

  const auto capabilities =
      scratchbird::core::platform::DetectRuntimeCapabilities();
  for (const auto& capability : capabilities.capabilities) {
    if (!IsAccelerationCapability(capability.key)) continue;
    snapshot->rows.push_back(
        {capability.key,
         ProviderClass(capability.key),
         CapabilityStateName(capability.state),
         CapabilityRequirementName(capability.requirement),
         RedactedProvider(capability),
         capability.diagnostic_code,
         snapshot->generation});
  }
  std::sort(snapshot->rows.begin(), snapshot->rows.end(),
            [](const auto& lhs, const auto& rhs) {
              return lhs.provider_id < rhs.provider_id;
            });

  std::string material =
      "ScratchBird.EngineAccelerationRegistrySnapshot.V1";
  AppendField(&material, std::to_string(snapshot->generation));
  AppendField(&material, std::to_string(snapshot->rows.size()));
  for (const auto& row : snapshot->rows) {
    AppendField(&material, row.provider_id);
    AppendField(&material, row.provider_class);
    AppendField(&material, row.state);
    AppendField(&material, row.requirement);
    AppendField(&material, row.redacted_provider);
    AppendField(&material, row.diagnostic_code);
    AppendField(&material, std::to_string(row.generation));
  }
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(
      reinterpret_cast<const scratchbird::core::platform::byte*>(
          material.data()),
      material.size());
  if (digest.ok()) {
    snapshot->evidence_sha256 =
        "sha256:" + scratchbird::core::hash::HexLower(digest.digest);
  }
  return snapshot;
}

}  // namespace

std::shared_ptr<const EngineAccelerationRegistrySnapshot>
LoadEngineAccelerationRegistrySnapshot() {
  static const std::shared_ptr<const EngineAccelerationRegistrySnapshot>
      snapshot = BuildSnapshot();
  return snapshot;
}

}  // namespace scratchbird::engine::internal_api
