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
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <unistd.h>

namespace {

namespace memory = scratchbird::core::memory;
namespace platform = scratchbird::core::platform;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

std::filesystem::path MakeTempDir() {
  std::string tmpl = "/tmp/sb_dblc013x_temp_workspace.XXXXXX";
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  Require(made != nullptr, "mkdtemp failed for DBLC-013X temp workspace test");
  return std::filesystem::path(made);
}

memory::TempWorkspacePolicy Policy(const std::filesystem::path& root, std::uint64_t quota = 4096) {
  memory::TempWorkspacePolicy policy;
  policy.policy_name = "dblc013x_conformance";
  policy.root_path = root;
  policy.filespace_quota_bytes = quota;
  policy.session_quota_bytes = quota;
  policy.transaction_quota_bytes = quota;
  policy.statement_quota_bytes = quota;
  policy.operation_quota_bytes = quota;
  policy.create_root_path = true;
  policy.sparse_file_reservation = true;
  return policy;
}

memory::TempWorkspaceOwner Owner(std::string suffix) {
  memory::TempWorkspaceOwner owner;
  owner.temp_object_uuid = "temp-" + suffix;
  owner.database_id = "database-a";
  owner.engine_id = "engine-a";
  owner.session_id = "session-a";
  owner.transaction_id = "txn-a";
  owner.statement_id = "stmt-a";
  owner.operation_id = "op-a";
  owner.policy_generation = 7;
  owner.security_generation = 9;
  owner.snapshot_boundary = "snapshot-inventory-generation-11";
  owner.metadata_boundary = "metadata-generation-13";
  owner.resource_budget_reference = "budget-a";
  return owner;
}

memory::TempWorkspaceAllocationRequest Request(std::string suffix,
                                               memory::TempWorkspaceLifetime lifetime,
                                               std::uint64_t bytes) {
  memory::TempWorkspaceAllocationRequest request;
  request.owner = Owner(std::move(suffix));
  request.lifetime = lifetime;
  request.bytes = bytes;
  request.purpose = "DBLC-013X conformance";
  return request;
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
}

void TestAllocationQuotaAndDiagnostics() {
  const auto root = MakeTempDir();
  memory::TempWorkspaceLifecycleManager manager(Policy(root, 1024));

  auto reservation = manager.ReserveTempFilespace(
      Request("reserve", memory::TempWorkspaceLifetime::statement_lifetime, 128));
  Require(reservation.ok() && reservation.record.has_value(), "temp filespace reservation failed");
  Require(std::filesystem::exists(reservation.record->path), "reserved temp filespace file was not created");

  auto sort = manager.AllocateSortSpill(
      Request("sort", memory::TempWorkspaceLifetime::statement_lifetime, 256));
  Require(sort.ok() && sort.record.has_value(), "sort spill allocation failed");
  Require(sort.record->storage_class == memory::TempStorageClass::sort_workspace,
          "sort spill did not use sort workspace class");

  auto hash = manager.AllocateHashSpill(
      Request("hash", memory::TempWorkspaceLifetime::statement_lifetime, 256));
  Require(hash.ok() && hash.record.has_value(), "hash spill allocation failed");
  Require(hash.record->storage_class == memory::TempStorageClass::hash_workspace,
          "hash spill did not use hash workspace class");

  auto denied = manager.AllocateSpillFile(
      Request("denied", memory::TempWorkspaceLifetime::statement_lifetime, 512));
  Require(!denied.ok(), "quota denial unexpectedly succeeded");
  RequireValidDiagnostic(denied.diagnostic, "TEMP_WORKSPACE.QUOTA_DENIED", "quota denial");
  Require(manager.Snapshot().quota_denial_count == 1, "quota denial metric was not recorded");

  const auto cleaned = manager.CleanupOnShutdown();
  Require(cleaned.ok() && cleaned.cleaned_count == 3, "shutdown cleanup did not remove all quota test records");
  Require(!std::filesystem::exists(reservation.record->path), "reservation path survived shutdown cleanup");
  std::filesystem::remove_all(root);
}

void TestCommitRollbackDisconnectAndShutdownCleanup() {
  const auto root = MakeTempDir();
  memory::TempWorkspaceLifecycleManager manager(Policy(root));

  auto commit = manager.AllocateSpillFile(
      Request("commit", memory::TempWorkspaceLifetime::transaction_lifetime, 128));
  Require(commit.ok() && commit.record.has_value(), "commit cleanup setup allocation failed");

  auto refused = manager.CleanupOnCommit("txn-a", memory::TempTransactionOutcomeEvidence::rolled_back);
  Require(!refused.ok(), "commit cleanup accepted rollback evidence");
  RequireValidDiagnostic(refused.diagnostic, "TEMP_WORKSPACE.OUTCOME_EVIDENCE_REQUIRED",
                         "commit evidence refusal");
  Require(manager.Find(commit.record->allocation_id).has_value(),
          "commit cleanup without commit evidence removed a workspace");

  auto commit_cleaned = manager.CleanupOnCommit("txn-a", memory::TempTransactionOutcomeEvidence::committed);
  Require(commit_cleaned.ok() && commit_cleaned.cleaned_count == 1, "commit cleanup failed");
  Require(!std::filesystem::exists(commit.record->path), "commit cleanup left spill file behind");

  auto rollback = manager.AllocateSpillFile(
      Request("rollback", memory::TempWorkspaceLifetime::transaction_lifetime, 128));
  Require(rollback.ok() && rollback.record.has_value(), "rollback cleanup setup allocation failed");
  auto rollback_cleaned = manager.CleanupOnRollback("txn-a", memory::TempTransactionOutcomeEvidence::rolled_back);
  Require(rollback_cleaned.ok() && rollback_cleaned.cleaned_count == 1, "rollback cleanup failed");
  Require(!std::filesystem::exists(rollback.record->path), "rollback cleanup left spill file behind");

  auto disconnect = manager.AllocateSpillFile(
      Request("disconnect", memory::TempWorkspaceLifetime::session_lifetime, 128));
  Require(disconnect.ok() && disconnect.record.has_value(), "disconnect cleanup setup allocation failed");
  auto disconnect_cleaned = manager.CleanupOnDisconnect("session-a");
  Require(disconnect_cleaned.ok() && disconnect_cleaned.cleaned_count == 1, "disconnect cleanup failed");
  Require(!std::filesystem::exists(disconnect.record->path), "disconnect cleanup left spill file behind");

  auto shutdown = manager.AllocateSpillFile(
      Request("shutdown", memory::TempWorkspaceLifetime::session_lifetime, 128));
  Require(shutdown.ok() && shutdown.record.has_value(), "shutdown cleanup setup allocation failed");
  auto shutdown_cleaned = manager.CleanupOnShutdown();
  Require(shutdown_cleaned.ok() && shutdown_cleaned.cleaned_count == 1, "shutdown cleanup failed");
  Require(manager.Snapshot().active_bytes == 0, "cleanup accounting leaked active bytes");
  std::filesystem::remove_all(root);
}

void TestRecoveryCleanupAndRefusal() {
  const auto root = MakeTempDir();
  memory::TempWorkspaceLifecycleManager manager(Policy(root));

  auto evidence_required_request =
      Request("recovery-evidence", memory::TempWorkspaceLifetime::transaction_lifetime, 128);
  evidence_required_request.evidence_required_before_discard = true;
  auto evidence_required = manager.AllocateSpillFile(std::move(evidence_required_request));
  Require(evidence_required.ok() && evidence_required.record.has_value(),
          "recovery evidence setup allocation failed");

  memory::TempWorkspaceRecoveryEvidence missing_evidence;
  auto refused = manager.CleanupRecoverySafe(missing_evidence);
  Require(!refused.ok() && refused.refused_count == 1, "recovery cleanup did not refuse missing evidence");
  RequireValidDiagnostic(refused.diagnostic, "TEMP_WORKSPACE.RECOVERY_CLEANUP_REFUSED",
                         "recovery cleanup refusal");
  Require(manager.Find(evidence_required.record->allocation_id).has_value(),
          "recovery cleanup removed evidence-required workspace without authority");

  memory::TempWorkspaceRecoveryEvidence outcome_evidence;
  outcome_evidence.engine_recovery_authority = true;
  outcome_evidence.transaction_outcome = memory::TempTransactionOutcomeEvidence::rolled_back;
  auto recovery_cleaned = manager.CleanupRecoverySafe(outcome_evidence);
  Require(recovery_cleaned.ok() && recovery_cleaned.cleaned_count == 1,
          "recovery cleanup with outcome evidence failed");
  Require(!std::filesystem::exists(evidence_required.record->path),
          "recovery cleanup left discard-after-evidence file behind");

  auto resumable_request = Request("operation-resume", memory::TempWorkspaceLifetime::operation_lifetime, 128);
  resumable_request.durable_operation_owned = true;
  auto resumable = manager.AllocateSpillFile(std::move(resumable_request));
  Require(resumable.ok() && resumable.record.has_value(), "operation-owned recovery setup failed");
  memory::TempWorkspaceRecoveryEvidence operation_evidence;
  operation_evidence.engine_recovery_authority = true;
  operation_evidence.operation_envelope_present = true;
  auto classified = manager.ClassifyForRecovery(resumable.record->allocation_id, operation_evidence);
  Require(classified.ok() &&
              classified.recovery_class == memory::TempRecoveryClass::operation_owned_resume,
          "operation-owned recovery was not classified for resume");
  auto recovery_refused = manager.CleanupRecoverySafe(operation_evidence);
  Require(!recovery_refused.ok() && recovery_refused.refused_count == 1,
          "recovery cleanup removed operation-owned resumable scratch");

  auto corrupt = manager.ClassifyForRecovery(resumable.record->allocation_id,
                                             memory::TempWorkspaceRecoveryEvidence{
                                                 memory::TempTransactionOutcomeEvidence::none,
                                                 true,
                                                 true,
                                                 false,
                                                 false});
  Require(corrupt.ok() && corrupt.recovery_class == memory::TempRecoveryClass::quarantine_required,
          "integrity failure did not classify as quarantine required");
  Require(manager.Find(resumable.record->allocation_id)->state == memory::TempWorkspaceState::quarantined,
          "quarantined recovery record did not update state");

  std::filesystem::remove_all(root);
}

}  // namespace

int main() {
  TestAllocationQuotaAndDiagnostics();
  TestCommitRollbackDisconnectAndShutdownCleanup();
  TestRecoveryCleanupAndRefusal();
  return EXIT_SUCCESS;
}
