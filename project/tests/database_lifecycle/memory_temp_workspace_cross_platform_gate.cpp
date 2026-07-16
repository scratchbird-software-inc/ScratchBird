// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "temp_workspace_lifecycle.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
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

#if defined(_WIN32)
void ProveWindowsExtendedLengthWorkspaceLifecycle() {
  std::error_code ec;
  const auto base = std::filesystem::temp_directory_path() /
                    "scratchbird-mmch043-long-path";
  std::filesystem::remove_all(base, ec);
  ec.clear();
  std::filesystem::create_directories(base, ec);
  Require(!ec, "MMCH-043 could not create Windows long-path test base");

  auto root = base;
  int segment = 0;
  while (root.native().size() < 225) {
    root /= "s" + std::to_string(segment++) + std::string(5, 'x');
  }
  Require(root.native().size() < 240,
          "MMCH-043 Windows long-path test root exceeded directory limit");
  std::filesystem::create_directories(root, ec);
  Require(!ec, "MMCH-043 could not create Windows long-path test root");

  mem::TempWorkspacePolicy policy;
  policy.policy_name = "mmch043_windows_extended_path";
  policy.root_path = std::filesystem::path(
      std::wstring(LR"(\\?\)") + root.native());
  policy.filespace_quota_bytes = 4096;
  policy.session_quota_bytes = 4096;
  policy.transaction_quota_bytes = 4096;
  policy.statement_quota_bytes = 4096;
  policy.operation_quota_bytes = 4096;

  mem::TempWorkspaceAllocationRequest request;
  request.bytes = 1024;
  request.purpose = "MMCH-043 Windows extended path allocation";
  request.owner.temp_object_uuid = "mmch043-" + std::string(80, 'o');
  request.owner.database_id = "mmch043-db";
  request.owner.engine_id = "mmch043-engine";
  request.owner.session_id = "mmch043-session";
  request.owner.transaction_id = "mmch043-transaction";
  request.owner.statement_id = "mmch043-statement";
  request.owner.operation_id = "mmch043-operation";

  std::filesystem::path logical_spill_path;
  {
    mem::TempWorkspaceLifecycleManager manager(policy);
    Require(manager.policy().root_path == root.lexically_normal(),
            "MMCH-043 extended prefix was not removed from logical policy root");
    const auto allocation = manager.AllocateSpillFile(request);
    if (!allocation.ok()) {
      std::cerr << "MMCH-043 Windows long-path allocation diagnostic="
                << allocation.diagnostic.diagnostic_code << '\n';
      for (const auto& argument : allocation.diagnostic.arguments) {
        std::cerr << "MMCH-043 Windows long-path allocation "
                  << argument.key << '=' << argument.value << '\n';
      }
    }
    Require(allocation.ok() && allocation.record.has_value(),
            "MMCH-043 Windows extended-length spill allocation failed");
    constexpr std::size_t kWindowsLegacyMaxPath = 260;
    Require(allocation.record->path.native().size() > kWindowsLegacyMaxPath,
            "MMCH-043 Windows spill regression path did not exceed MAX_PATH");
    Require(allocation.record->path.native().rfind(LR"(\\?\)", 0) != 0,
            "MMCH-043 API-only extended prefix leaked into logical record path");
    logical_spill_path = allocation.record->path;
  }

  mem::TempWorkspaceLifecycleManager reopened(policy);
  Require(reopened.policy().root_path == root.lexically_normal(),
          "MMCH-043 extended prefix leaked into reopened logical policy root");
  const auto reopened_records = reopened.ActiveRecords();
  Require(reopened_records.size() == 1 &&
              reopened_records.front().path == logical_spill_path,
          "MMCH-043 Windows long-path manifest did not reopen logical spill path");
  Require(reopened_records.front().path.native().rfind(LR"(\\?\)", 0) != 0,
          "MMCH-043 API-only extended prefix leaked through manifest reload");
  const auto cleanup = reopened.CleanupOnShutdown();
  Require(cleanup.ok() && cleanup.cleaned_count == 1,
          "MMCH-043 Windows extended-length spill cleanup failed");
  Require(std::filesystem::is_empty(root, ec) && !ec,
          "MMCH-043 Windows long-path manifest or spill survived cleanup");

  const auto original_cwd = std::filesystem::current_path();
  std::filesystem::current_path(base, ec);
  Require(!ec, "MMCH-043 could not set relative-root construction directory");
  auto relative_policy = policy;
  relative_policy.policy_name = "mmch043_windows_relative_path";
  relative_policy.root_path = "relative-root";
  mem::TempWorkspaceLifecycleManager relative_manager(relative_policy);
  const auto frozen_relative_root =
      (base / "relative-root").lexically_normal();
  Require(relative_manager.policy().root_path == frozen_relative_root,
          "MMCH-043 relative logical root was not frozen as absolute");
  std::filesystem::current_path(original_cwd, ec);
  Require(!ec, "MMCH-043 could not restore construction directory");
  request.owner.temp_object_uuid = "mmch043-relative";
  const auto relative_allocation = relative_manager.AllocateSpillFile(request);
  Require(relative_allocation.ok() && relative_allocation.record.has_value() &&
              relative_allocation.record->path.parent_path() ==
                  frozen_relative_root,
          "MMCH-043 frozen relative root changed after current-directory change");
  const auto relative_cleanup = relative_manager.CleanupOnShutdown();
  Require(relative_cleanup.ok() && relative_cleanup.cleaned_count == 1,
          "MMCH-043 frozen relative-root cleanup failed");

  auto device_policy = policy;
  device_policy.policy_name = "mmch043_windows_device_rejection";
  device_policy.root_path = LR"(\\.\C:\scratchbird-mmch043-device)";
  const auto sentinel =
      base / ".scratchbird_temp_workspace_manifest.v2";
  {
    std::ofstream out(sentinel, std::ios::binary | std::ios::trunc);
    out << "mmch043-device-rejection-sentinel\n";
    Require(out.good(), "MMCH-043 could not write device rejection sentinel");
  }
  std::filesystem::current_path(base, ec);
  Require(!ec, "MMCH-043 could not set device rejection sentinel directory");
  mem::TempWorkspaceLifecycleManager device_manager(device_policy);
  const auto device_allocation = device_manager.AllocateSpillFile(request);
  Require(!device_allocation.ok() &&
              device_allocation.diagnostic.diagnostic_code ==
                  "TEMP_WORKSPACE.ROOT_UNSAFE" &&
              device_manager.policy().root_path.empty(),
          "MMCH-043 Windows device namespace did not fail closed");
  for (const auto& argument : device_allocation.diagnostic.arguments) {
    Require(argument.value.find(R"(\\.\)") == std::string::npos &&
                argument.value.find(R"(\\?\)") == std::string::npos,
            "MMCH-043 rejected API namespace leaked into diagnostics");
  }
  const auto device_cleanup = device_manager.CleanupOnShutdown();
  Require(!device_cleanup.ok(),
          "MMCH-043 invalid device namespace cleanup did not fail closed");
  std::filesystem::current_path(original_cwd, ec);
  Require(!ec, "MMCH-043 could not restore device rejection directory");
  std::ifstream sentinel_in(sentinel, std::ios::binary);
  std::string sentinel_body;
  std::getline(sentinel_in, sentinel_body);
  Require(sentinel_body == "mmch043-device-rejection-sentinel",
          "MMCH-043 invalid namespace cleanup touched the CWD manifest");

  std::filesystem::remove_all(base, ec);
  Require(!ec, "MMCH-043 could not remove Windows long-path test root");
}
#endif

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
              "temp_workspace.platform_authority_scope=evidence_only_not_transaction_finality_row_visibility_security_authorization_recovery_parser_reference_wal_benchmark_optimizer_plan_or_agent_action_authority"),
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
#if defined(_WIN32)
  ProveWindowsExtendedLengthWorkspaceLifecycle();
#endif
  return EXIT_SUCCESS;
}
