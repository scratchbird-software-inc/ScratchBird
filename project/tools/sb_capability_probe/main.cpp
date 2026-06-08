// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SB-CAPABILITY-PROBE-ANCHOR
#include "runtime_capabilities.hpp"

#include <iostream>

namespace {

using scratchbird::core::platform::CapabilityRequirementName;
using scratchbird::core::platform::CapabilityStateName;
using scratchbird::core::platform::CheckMandatoryRuntimeCapabilities;

const char* Bool(bool value) {
  return value ? "true" : "false";
}

}  // namespace

int main() {
  const auto check = CheckMandatoryRuntimeCapabilities();
  std::cout << "{\n";
  std::cout << "  \"ok\": " << Bool(check.ok()) << ",\n";
  std::cout << "  \"mandatory_ok\": " << Bool(check.manifest.mandatory_ok()) << ",\n";
  std::cout << "  \"capabilities\": [\n";
  for (std::size_t index = 0; index < check.manifest.capabilities.size(); ++index) {
    const auto& capability = check.manifest.capabilities[index];
    std::cout << "    {\"key\": \"" << capability.key << "\", "
              << "\"requirement\": \"" << CapabilityRequirementName(capability.requirement) << "\", "
              << "\"state\": \"" << CapabilityStateName(capability.state) << "\", "
              << "\"provider\": \"" << capability.provider << "\"}";
    if (index + 1 != check.manifest.capabilities.size()) {
      std::cout << ",";
    }
    std::cout << "\n";
  }
  std::cout << "  ],\n";
  std::cout << "  \"diagnostics\": " << check.diagnostics.size() << "\n";
  std::cout << "}\n";
  return check.ok() ? 0 : 1;
}
