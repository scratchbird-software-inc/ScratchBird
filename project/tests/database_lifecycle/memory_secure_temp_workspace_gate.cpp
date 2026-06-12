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
#include <set>
#include <string>
#include <string_view>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

namespace memory = scratchbird::core::memory;
namespace platform = scratchbird::core::platform;

constexpr std::string_view kAuthorityBoundary =
    "resource_security_evidence_only_not_transaction_finality_row_visibility_security_authorization_recovery_parser_reference_wal_benchmark_optimizer_plan_or_agent_action_authority";

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

std::filesystem::path MakeTempDir(std::string_view prefix) {
#if defined(_WIN32)
  (void)prefix;
  Fail("MMCH_SECURE_TEMP_WORKSPACE requires platform secure tempfile support; Windows gate is owned by MMCH-043");
#else
  std::string tmpl = "/tmp/";
  tmpl += prefix;
  tmpl += ".XXXXXX";
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  Require(made != nullptr, "mkdtemp failed for MMCH_SECURE_TEMP_WORKSPACE gate");
  return std::filesystem::path(made);
#endif
}

memory::TempWorkspacePolicy Policy(const std::filesystem::path& root) {
  memory::TempWorkspacePolicy policy;
  policy.policy_name = "MMCH_SECURE_TEMP_WORKSPACE";
  policy.root_path = root;
  policy.filespace_quota_bytes = 1024 * 1024;
  policy.session_quota_bytes = 1024 * 1024;
  policy.transaction_quota_bytes = 1024 * 1024;
  policy.statement_quota_bytes = 1024 * 1024;
  policy.operation_quota_bytes = 1024 * 1024;
  policy.create_root_path = true;
  policy.sparse_file_reservation = true;
  return policy;
}

memory::TempWorkspaceOwner Owner(std::string suffix) {
  memory::TempWorkspaceOwner owner;
  owner.temp_object_uuid = "temp-" + std::move(suffix);
  owner.database_id = "database-mmch040";
  owner.engine_id = "engine-mmch040";
  owner.session_id = "session-mmch040";
  owner.transaction_id = "txn-mmch040";
  owner.statement_id = "stmt-mmch040";
  owner.operation_id = "op-mmch040";
  owner.policy_generation = 40;
  owner.security_generation = 400;
  owner.snapshot_boundary = "snapshot-boundary-not-authority";
  owner.metadata_boundary = "metadata-boundary-not-authority";
  owner.resource_budget_reference = "memory-security-gate";
  return owner;
}

memory::TempWorkspaceAllocationRequest Request(std::string suffix, std::uint64_t bytes = 4096) {
  memory::TempWorkspaceAllocationRequest request;
  request.owner = Owner(std::move(suffix));
  request.lifetime = memory::TempWorkspaceLifetime::statement_lifetime;
  request.bytes = bytes;
  request.purpose = "MMCH_SECURE_TEMP_WORKSPACE focused gate";
  return request;
}

bool HasDiagnosticArgument(const platform::DiagnosticRecord& diagnostic,
                           std::string_view key,
                           std::string_view value) {
  for (const auto& argument : diagnostic.arguments) {
    if (argument.key == key && argument.value == value) {
      return true;
    }
  }
  return false;
}

void RequireValidDiagnostic(const platform::DiagnosticRecord& diagnostic,
                            std::string_view code,
                            std::string_view context) {
  Require(diagnostic.diagnostic_code == code, std::string(context) + " diagnostic code mismatch");
  const auto validation = platform::ValidateDiagnosticRecord(diagnostic);
  if (!validation.ok()) {
    std::cerr << diagnostic.diagnostic_code << " " << diagnostic.message_key << '\n';
  }
  Require(validation.ok(), std::string(context) + " diagnostic did not validate");
  Require(HasDiagnosticArgument(diagnostic, "authority_boundary", kAuthorityBoundary),
          std::string(context) + " diagnostic lacked authority boundary");
  Require(diagnostic.remediation_hint.find("resource and security evidence only") != std::string::npos,
          std::string(context) + " diagnostic remediation did not preserve authority boundary");
}

bool IsHexToken(std::string_view token) {
  if (token.size() != 32) return false;
  for (char c : token) {
    const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    if (!hex) return false;
  }
  return true;
}

std::string RandomTokenFromFileName(const std::filesystem::path& path) {
  const std::string name = path.filename().string();
  Require(name.rfind("tw-", 0) == 0, "secure temp workspace filename did not use tw prefix");
  Require(name.size() > 35, "secure temp workspace filename too short for random token");
  const std::string token = name.substr(3, 32);
  Require(IsHexToken(token), "secure temp workspace filename did not contain 128-bit hex token");
  Require(name.size() == 35 || name[35] == '-', "secure temp workspace token was not dash-delimited");
  return token;
}

void RequireOwnerOnlyFile(const std::filesystem::path& path) {
#if defined(_WIN32)
  (void)path;
  Fail("MMCH_SECURE_TEMP_WORKSPACE owner-only permission evidence is unsupported on this platform");
#else
  struct stat st {};
  Require(::lstat(path.c_str(), &st) == 0, "created secure temp workspace path could not be stated");
  Require(S_ISREG(st.st_mode), "created secure temp workspace path was not a regular file");
  Require((st.st_mode & 0777) == (S_IRUSR | S_IWUSR),
          "created secure temp workspace file was not owner-only 0600");
  Require(st.st_nlink == 1, "created secure temp workspace file had extra hardlinks");
#endif
}

void TestRandomExclusiveOwnerOnlyEvidence() {
  const auto root = MakeTempDir("sb_mmch040_secure_random");
  memory::TempWorkspaceLifecycleManager manager(Policy(root));

  std::set<std::string> file_names;
  std::set<std::string> random_tokens;
  std::vector<std::filesystem::path> paths;

  for (int i = 0; i < 24; ++i) {
    auto allocation = manager.AllocateSpillFile(Request("random-" + std::to_string(i), 2048));
    Require(allocation.ok() && allocation.record.has_value(), "secure temp workspace allocation failed");
    const auto& record = *allocation.record;
    Require(record.path.parent_path() == root, "secure temp workspace escaped policy root");
    Require(record.path.filename().string().find("tw-" + std::to_string(i + 1) + "-") != 0,
            "secure temp workspace filename retained old sequential shape");
    Require(file_names.insert(record.path.filename().string()).second,
            "secure temp workspace reused a filename");
    Require(random_tokens.insert(RandomTokenFromFileName(record.path)).second,
            "secure temp workspace reused a random token");
    Require(record.security_evidence.random_unguessable_name,
            "record did not carry random-name evidence");
    Require(record.security_evidence.exclusive_create_no_overwrite,
            "record did not carry exclusive-create evidence");
    Require(record.security_evidence.owner_only_permissions,
            "record did not carry owner-only permission evidence");
    Require(record.security_evidence.nofollow_or_platform_equivalent,
            "record did not carry no-follow evidence");
    Require(record.security_evidence.hardlink_refusal_checked,
            "record did not carry hardlink-refusal evidence");
    Require(record.security_evidence.authority_boundary == kAuthorityBoundary,
            "record security evidence did not preserve authority boundary");
    RequireOwnerOnlyFile(record.path);
    paths.push_back(record.path);
  }

  const auto cleaned = manager.CleanupOnShutdown();
  Require(cleaned.ok() && cleaned.cleaned_count == paths.size(),
          "shutdown cleanup did not remove all secure temp workspace records");
  Require(manager.Snapshot().active_bytes == 0, "secure temp workspace cleanup leaked accounting bytes");
  for (const auto& path : paths) {
    Require(!std::filesystem::exists(path), "secure temp workspace cleanup left an active file");
  }
  std::filesystem::remove_all(root);
}

void TestPrecreatedPredictableTargetsAreNotOverwritten() {
  const auto root = MakeTempDir("sb_mmch040_precreated");
  const auto victim = root / "victim";
  {
    std::ofstream out(victim);
    out << "victim-content";
  }
  const auto old_predictable = root / "tw-1-temp-precreated-spill_file.spill";
  {
    std::ofstream out(old_predictable);
    out << "attacker-content";
  }
#if !defined(_WIN32)
  const auto old_predictable_symlink = root / "tw-2-temp-precreated-symlink-spill_file.spill";
  Require(::symlink(victim.c_str(), old_predictable_symlink.c_str()) == 0,
          "failed to create predictable symlink target for secure temp workspace gate");
#endif

  memory::TempWorkspaceLifecycleManager manager(Policy(root));
  auto allocation = manager.AllocateSpillFile(Request("precreated", 1024));
  Require(allocation.ok() && allocation.record.has_value(),
          "random secure temp workspace allocation unexpectedly failed near precreated target");
  Require(allocation.record->path != old_predictable,
          "secure temp workspace selected an old predictable precreated path");

  std::ifstream old_file(old_predictable);
  std::string old_contents;
  old_file >> old_contents;
  Require(old_contents == "attacker-content", "precreated predictable file was overwritten");

  std::ifstream victim_file(victim);
  std::string victim_contents;
  victim_file >> victim_contents;
  Require(victim_contents == "victim-content", "precreated symlink victim was overwritten");

  const auto cleaned = manager.CleanupOnShutdown();
  Require(cleaned.ok() && cleaned.cleaned_count == 1, "cleanup failed for precreated-target allocation");
  Require(std::filesystem::exists(old_predictable), "cleanup removed unrelated precreated file");
  Require(std::filesystem::exists(victim), "cleanup removed unrelated symlink victim");
  std::filesystem::remove_all(root);
}

void TestSymlinkedWorkspaceRootFailsClosed() {
#if defined(_WIN32)
  Fail("MMCH_SECURE_TEMP_WORKSPACE symlink root refusal is unsupported on this platform");
#else
  const auto parent = MakeTempDir("sb_mmch040_symlink_parent");
  const auto real_root = parent / "real-root";
  const auto symlink_root = parent / "symlink-root";
  std::filesystem::create_directory(real_root);
  Require(::symlink(real_root.c_str(), symlink_root.c_str()) == 0,
          "failed to create symlinked workspace root");

  memory::TempWorkspaceLifecycleManager manager(Policy(symlink_root));
  auto refused = manager.AllocateSpillFile(Request("symlink-root", 1024));
  Require(!refused.ok(), "symlinked secure temp workspace root was accepted");
  Require(!refused.record.has_value(), "symlinked secure temp workspace root returned a record");
  RequireValidDiagnostic(refused.diagnostic, "TEMP_WORKSPACE.ROOT_UNSAFE", "symlink root refusal");
  Require(std::filesystem::is_empty(real_root), "symlinked root refusal created a file in the target root");
  std::filesystem::remove_all(parent);
#endif
}

}  // namespace

int main() {
  TestRandomExclusiveOwnerOnlyEvidence();
  TestPrecreatedPredictableTargetsAreNotOverwritten();
  TestSymlinkedWorkspaceRootFailsClosed();
  return EXIT_SUCCESS;
}
