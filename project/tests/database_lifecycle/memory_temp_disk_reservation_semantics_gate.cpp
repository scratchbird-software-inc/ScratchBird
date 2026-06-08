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
#include <iostream>
#include <limits>
#include <string>
#include <string_view>

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

namespace memory = scratchbird::core::memory;
namespace platform = scratchbird::core::platform;

constexpr std::string_view kAuthorityBoundary =
    "resource_security_evidence_only_not_transaction_finality_row_visibility_security_authorization_recovery_parser_donor_wal_benchmark_optimizer_plan_or_agent_action_authority";

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
  Fail("MMCH_TEMP_DISK_RESERVATION_SEMANTICS requires POSIX tempfile support; Windows is owned by MMCH-043");
#else
  std::string tmpl = "/tmp/";
  tmpl += prefix;
  tmpl += ".XXXXXX";
  std::string writable = tmpl;
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  Require(made != nullptr, "mkdtemp failed for MMCH-041");
  return std::filesystem::path(made);
#endif
}

memory::TempWorkspacePolicy Policy(const std::filesystem::path& root,
                                   memory::TempWorkspaceDiskReservationMode mode) {
  memory::TempWorkspacePolicy policy;
  policy.policy_name = "MMCH_TEMP_DISK_RESERVATION_SEMANTICS";
  policy.root_path = root;
  policy.filespace_quota_bytes = std::numeric_limits<std::uint64_t>::max();
  policy.session_quota_bytes = std::numeric_limits<std::uint64_t>::max();
  policy.transaction_quota_bytes = std::numeric_limits<std::uint64_t>::max();
  policy.statement_quota_bytes = std::numeric_limits<std::uint64_t>::max();
  policy.operation_quota_bytes = std::numeric_limits<std::uint64_t>::max();
  policy.create_root_path = true;
  policy.sparse_file_reservation =
      mode == memory::TempWorkspaceDiskReservationMode::sparse_file;
  policy.disk_reservation_mode = mode;
  policy.require_physical_disk_reservation =
      mode == memory::TempWorkspaceDiskReservationMode::physical_preallocate;
  return policy;
}

memory::TempWorkspaceOwner Owner(std::string suffix) {
  memory::TempWorkspaceOwner owner;
  owner.temp_object_uuid = "disk-" + std::move(suffix);
  owner.database_id = "database-mmch041";
  owner.engine_id = "engine-mmch041";
  owner.session_id = "session-mmch041";
  owner.transaction_id = "txn-mmch041";
  owner.statement_id = "stmt-mmch041";
  owner.operation_id = "op-mmch041";
  owner.policy_generation = 41;
  owner.security_generation = 410;
  owner.resource_budget_reference = "disk-reservation-semantics";
  return owner;
}

memory::TempWorkspaceAllocationRequest Request(std::string suffix,
                                               std::uint64_t bytes) {
  memory::TempWorkspaceAllocationRequest request;
  request.owner = Owner(std::move(suffix));
  request.lifetime = memory::TempWorkspaceLifetime::statement_lifetime;
  request.bytes = bytes;
  request.purpose = "MMCH_TEMP_DISK_RESERVATION_SEMANTICS focused gate";
  return request;
}

std::uint64_t FileSize(const std::filesystem::path& path) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  Require(!ec, "MMCH-041 could not read temp workspace file size");
  return static_cast<std::uint64_t>(size);
}

bool HasDiagnosticArgument(const platform::DiagnosticRecord& diagnostic,
                           std::string_view key,
                           std::string_view value) {
  for (const auto& argument : diagnostic.arguments) {
    if (argument.key == key && argument.value.find(value) != std::string::npos) {
      return true;
    }
  }
  return false;
}

void RequireAuthorityDiagnostic(const platform::DiagnosticRecord& diagnostic) {
  Require(HasDiagnosticArgument(diagnostic, "authority_boundary", kAuthorityBoundary),
          "MMCH-041 diagnostic lacked authority boundary");
  Require(diagnostic.remediation_hint.find("resource and security evidence only") != std::string::npos,
          "MMCH-041 diagnostic remediation drifted authority boundary");
}

void LogicalQuotaOnlyIsDistinct() {
  const auto root = MakeTempDir("sb_mmch041_logical");
  memory::TempWorkspaceLifecycleManager manager(
      Policy(root, memory::TempWorkspaceDiskReservationMode::logical_quota_only));
  auto allocated = manager.AllocateSpillFile(Request("logical", 4096));
  Require(allocated.ok() && allocated.record.has_value(),
          "MMCH-041 logical quota allocation failed");
  const auto& record = *allocated.record;
  Require(record.disk_reservation_evidence.logical_quota_reserved,
          "MMCH-041 logical quota evidence missing");
  Require(record.disk_reservation_evidence.mode ==
              memory::TempWorkspaceDiskReservationMode::logical_quota_only,
          "MMCH-041 logical mode evidence mismatch");
  Require(!record.disk_reservation_evidence.physical_preallocation_attempted,
          "MMCH-041 logical mode attempted physical preallocation");
  Require(!record.disk_reservation_evidence.sparse_file_created,
          "MMCH-041 logical mode created sparse reservation");
  Require(FileSize(record.path) == 0,
          "MMCH-041 logical mode should not claim file-space reservation");
  Require(manager.Snapshot().active_bytes == 4096,
          "MMCH-041 logical quota did not account active bytes");
  Require(record.disk_reservation_evidence.authority_boundary == kAuthorityBoundary,
          "MMCH-041 logical evidence authority boundary missing");
  Require(manager.CleanupOnShutdown().ok(), "MMCH-041 logical cleanup failed");
  std::filesystem::remove_all(root);
}

void SparseReservationIsExplicitlyNotPhysical() {
  const auto root = MakeTempDir("sb_mmch041_sparse");
  memory::TempWorkspaceLifecycleManager manager(
      Policy(root, memory::TempWorkspaceDiskReservationMode::sparse_file));
  auto allocated = manager.AllocateSpillFile(Request("sparse", 8192));
  Require(allocated.ok() && allocated.record.has_value(),
          "MMCH-041 sparse allocation failed");
  const auto& record = *allocated.record;
  Require(record.disk_reservation_evidence.sparse_file_created,
          "MMCH-041 sparse evidence missing");
  Require(record.disk_reservation_evidence.sparse_not_physical_reservation,
          "MMCH-041 sparse mode did not disclaim physical reservation");
  Require(!record.disk_reservation_evidence.physical_preallocation_complete,
          "MMCH-041 sparse mode claimed physical preallocation");
  Require(FileSize(record.path) == 8192,
          "MMCH-041 sparse mode file extent size mismatch");
  Require(record.disk_reservation_evidence.authority_boundary == kAuthorityBoundary,
          "MMCH-041 sparse authority boundary missing");
  Require(manager.CleanupOnShutdown().ok(), "MMCH-041 sparse cleanup failed");
  std::filesystem::remove_all(root);
}

void PhysicalPreallocationIsExplicitAndRequired() {
  const auto root = MakeTempDir("sb_mmch041_physical");
  memory::TempWorkspaceLifecycleManager manager(
      Policy(root, memory::TempWorkspaceDiskReservationMode::physical_preallocate));
  auto allocated = manager.AllocateSpillFile(Request("physical", 4096));
  Require(allocated.ok() && allocated.record.has_value(),
          "MMCH-041 physical preallocation failed");
  const auto& record = *allocated.record;
  Require(record.disk_reservation_evidence.physical_preallocation_required,
          "MMCH-041 physical required evidence missing");
  Require(record.disk_reservation_evidence.physical_preallocation_attempted,
          "MMCH-041 physical attempt evidence missing");
  Require(record.disk_reservation_evidence.physical_preallocation_complete,
          "MMCH-041 physical completion evidence missing");
  Require(!record.disk_reservation_evidence.sparse_not_physical_reservation,
          "MMCH-041 physical mode claimed sparse limitation");
  Require(FileSize(record.path) == 4096,
          "MMCH-041 physical preallocation size mismatch");
  Require(record.disk_reservation_evidence.authority_boundary == kAuthorityBoundary,
          "MMCH-041 physical authority boundary missing");
  Require(manager.CleanupOnShutdown().ok(), "MMCH-041 physical cleanup failed");
  std::filesystem::remove_all(root);
}

void PhysicalReservationFailureFailsClosed() {
  const auto root = MakeTempDir("sb_mmch041_physical_fail");
  memory::TempWorkspaceLifecycleManager manager(
      Policy(root, memory::TempWorkspaceDiskReservationMode::physical_preallocate));
  auto refused = manager.AllocateSpillFile(
      Request("physical-failure", std::numeric_limits<std::uint64_t>::max()));
  Require(!refused.ok(), "MMCH-041 impossible physical preallocation was accepted");
  Require(!refused.record.has_value(),
          "MMCH-041 failed physical preallocation returned a record");
  Require(refused.diagnostic.diagnostic_code == "TEMP_WORKSPACE.SPILL_RESERVE_FAILED",
          "MMCH-041 physical failure diagnostic changed");
  Require(HasDiagnosticArgument(refused.diagnostic,
                                "disk_reservation_mode",
                                "physical_preallocate"),
          "MMCH-041 physical failure diagnostic omitted mode");
  Require(HasDiagnosticArgument(refused.diagnostic,
                                "disk_reservation_failure",
                                "requested reservation exceeds platform file offset range"),
          "MMCH-041 physical failure diagnostic omitted reservation reason");
  RequireAuthorityDiagnostic(refused.diagnostic);
  Require(manager.Snapshot().active_bytes == 0,
          "MMCH-041 failed physical preallocation leaked accounting bytes");
  Require(std::filesystem::is_empty(root),
          "MMCH-041 failed physical preallocation left a temp file");
  std::filesystem::remove_all(root);
}

}  // namespace

int main() {
  std::cout << "MMCH-041 authority_note=temp_disk_reservation_evidence_only;"
               "not_transaction_finality_row_visibility_security_authorization_recovery_parser_donor_wal_benchmark_optimizer_plan_or_agent_action_authority"
            << '\n';
  LogicalQuotaOnlyIsDistinct();
  SparseReservationIsExplicitlyNotPhysical();
  PhysicalPreallocationIsExplicitAndRequired();
  PhysicalReservationFailureFailsClosed();
  return EXIT_SUCCESS;
}
