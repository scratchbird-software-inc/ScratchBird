// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "temp_workspace_lifecycle.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

namespace mem = scratchbird::core::memory;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool EvidenceHas(const std::vector<std::string>& evidence, std::string_view token) {
  for (const auto& row : evidence) {
    if (row.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool PlatformListed(const mem::TempWorkspacePlatformSecurityCapabilities& capabilities,
                    std::string_view platform) {
  for (const auto& supported : capabilities.production_supported_platforms) {
    if (supported == platform) {
      return true;
    }
  }
  return false;
}

}  // namespace

int main() {
  const auto capabilities = mem::CurrentTempWorkspacePlatformSecurityCapabilities();
  std::cout << "MMCH-043 platform=" << capabilities.platform_name
            << " random=" << capabilities.secure_random_provider
            << " file=" << capabilities.secure_file_semantics << '\n';
  Require(EvidenceHas(capabilities.evidence, "MMCH_TEMP_WORKSPACE_CROSS_PLATFORM"),
          "MMCH-043 capability evidence marker missing");
  Require(EvidenceHas(
              capabilities.evidence,
              "temp_workspace.platform_authority_scope=evidence_only_not_transaction_finality_row_visibility_security_authorization_recovery_parser_donor_wal_benchmark_optimizer_plan_or_agent_action_authority"),
          "MMCH-043 authority boundary evidence missing");
  Require(PlatformListed(capabilities, "linux") &&
              PlatformListed(capabilities, "windows") &&
              PlatformListed(capabilities, "macos") &&
              PlatformListed(capabilities, "bsd"),
          "MMCH-043 supported platform inventory incomplete");
  Require(capabilities.secure_random_supported &&
              capabilities.exclusive_create_supported &&
              capabilities.owner_only_permissions_supported &&
              capabilities.nofollow_or_platform_equivalent_supported &&
              capabilities.hardlink_or_reparse_refusal_supported &&
              capabilities.cleanup_supported,
          "MMCH-043 current platform secure temp capabilities incomplete");
#if defined(__linux__)
  Require(capabilities.platform_name == "linux",
          "MMCH-043 Linux platform name mismatch");
  Require(capabilities.secure_random_provider == "getrandom",
          "MMCH-043 Linux secure random provider mismatch");
  Require(capabilities.secure_file_semantics.find("posix_openat") != std::string::npos,
          "MMCH-043 Linux secure file semantics missing openat");
#endif
  return EXIT_SUCCESS;
}
