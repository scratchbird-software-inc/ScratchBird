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
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace {

namespace memory = scratchbird::core::memory;

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
#if defined(_WIN32)
  Fail("MMCH_TEMP_WORKSPACE_CRASH_RECOVERY requires POSIX temp directory support; Windows is owned by MMCH-043");
#else
  std::string tmpl = "/tmp/sb_mmch042_temp_recovery.XXXXXX";
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  Require(made != nullptr, "mkdtemp failed for MMCH-042");
  return std::filesystem::path(made);
#endif
}

memory::TempWorkspacePolicy Policy(const std::filesystem::path& root) {
  memory::TempWorkspacePolicy policy;
  policy.policy_name = "MMCH_TEMP_WORKSPACE_CRASH_RECOVERY";
  policy.root_path = root;
  policy.filespace_quota_bytes = 8192;
  policy.session_quota_bytes = 8192;
  policy.transaction_quota_bytes = 8192;
  policy.statement_quota_bytes = 8192;
  policy.operation_quota_bytes = 8192;
  policy.create_root_path = true;
  policy.sparse_file_reservation = true;
  policy.disk_reservation_mode = memory::TempWorkspaceDiskReservationMode::sparse_file;
  return policy;
}

memory::TempWorkspaceOwner Owner(std::string suffix) {
  memory::TempWorkspaceOwner owner;
  owner.temp_object_uuid = "crash-" + std::move(suffix);
  owner.database_id = "database-mmch042";
  owner.engine_id = "engine-mmch042";
  owner.session_id = "session-mmch042";
  owner.transaction_id = "txn-mmch042";
  owner.statement_id = "stmt-mmch042";
  owner.operation_id = "op-mmch042";
  owner.policy_generation = 42;
  owner.security_generation = 420;
  owner.resource_budget_reference = "crash-recovery-budget";
  return owner;
}

memory::TempWorkspaceAllocationRequest Request(
    std::string suffix,
    memory::TempWorkspaceLifetime lifetime) {
  memory::TempWorkspaceAllocationRequest request;
  request.owner = Owner(std::move(suffix));
  request.lifetime = lifetime;
  request.bytes = 128;
  request.purpose = "MMCH_TEMP_WORKSPACE_CRASH_RECOVERY focused gate";
  return request;
}

void SeedCrashState(const std::filesystem::path& root,
                    std::string* evidence_required_id,
                    std::string* durable_id,
                    std::string* legal_id) {
  memory::TempWorkspaceLifecycleManager manager(Policy(root));

  auto ordinary = manager.AllocateSpillFile(
      Request("ordinary", memory::TempWorkspaceLifetime::statement_lifetime));
  Require(ordinary.ok() && ordinary.record.has_value(),
          "MMCH-042 ordinary setup allocation failed");

  auto evidence_request =
      Request("evidence-required", memory::TempWorkspaceLifetime::transaction_lifetime);
  evidence_request.evidence_required_before_discard = true;
  auto evidence_required = manager.AllocateSpillFile(std::move(evidence_request));
  Require(evidence_required.ok() && evidence_required.record.has_value(),
          "MMCH-042 evidence-required setup allocation failed");
  *evidence_required_id = evidence_required.record->allocation_id;

  auto durable_request =
      Request("durable-operation", memory::TempWorkspaceLifetime::operation_lifetime);
  durable_request.durable_operation_owned = true;
  durable_request.recovery_resume_supported = true;
  auto durable = manager.AllocateSpillFile(std::move(durable_request));
  Require(durable.ok() && durable.record.has_value(),
          "MMCH-042 durable setup allocation failed");
  *durable_id = durable.record->allocation_id;

  auto legal_request =
      Request("legal-hold", memory::TempWorkspaceLifetime::session_lifetime);
  legal_request.legal_hold = true;
  legal_request.administrator_review_required = true;
  auto legal = manager.AllocateSpillFile(std::move(legal_request));
  Require(legal.ok() && legal.record.has_value(),
          "MMCH-042 legal hold setup allocation failed");
  *legal_id = legal.record->allocation_id;

  Require(manager.ActiveRecords().size() == 4,
          "MMCH-042 setup did not create four active records");
}

void ReopenRecoverAndRefuseCorrectly() {
  const auto root = MakeTempDir();
  std::string evidence_required_id;
  std::string durable_id;
  std::string legal_id;
  SeedCrashState(root, &evidence_required_id, &durable_id, &legal_id);

  memory::TempWorkspaceLifecycleManager reopened(Policy(root));
  Require(reopened.ActiveRecords().size() == 4,
          "MMCH-042 reopened manager did not load manifest records");
  Require(reopened.Snapshot().active_bytes == 512,
          "MMCH-042 reopened accounting did not reconstruct active bytes");

  memory::TempWorkspaceRecoveryEvidence missing;
  missing.leaked_after_crash = true;
  auto first_cleanup = reopened.CleanupRecoverySafe(missing);
  Require(!first_cleanup.ok() &&
              first_cleanup.cleaned_count == 1 &&
              first_cleanup.refused_count == 3,
          "MMCH-042 recovery cleanup did not clean ordinary and refuse protected records");
  Require(reopened.ActiveRecords().size() == 3,
          "MMCH-042 manifest cleanup did not retain protected records");

  memory::TempWorkspaceLifecycleManager after_first_cleanup(Policy(root));
  Require(after_first_cleanup.ActiveRecords().size() == 3,
          "MMCH-042 manifest did not persist post-cleanup protected records");
  Require(after_first_cleanup.Find(evidence_required_id).has_value(),
          "MMCH-042 evidence-required record missing after restart");
  Require(after_first_cleanup.Find(durable_id).has_value(),
          "MMCH-042 durable record missing after restart");
  Require(after_first_cleanup.Find(legal_id).has_value(),
          "MMCH-042 legal hold record missing after restart");

  memory::TempWorkspaceRecoveryEvidence authority;
  authority.engine_recovery_authority = true;
  authority.operation_envelope_present = true;
  authority.transaction_outcome = memory::TempTransactionOutcomeEvidence::rolled_back;
  auto durable_class = after_first_cleanup.ClassifyForRecovery(durable_id, authority);
  Require(durable_class.ok() &&
              durable_class.recovery_class == memory::TempRecoveryClass::operation_owned_resume,
          "MMCH-042 durable operation did not classify for resume with operation evidence");
  auto second_cleanup = after_first_cleanup.CleanupRecoverySafe(authority);
  Require(!second_cleanup.ok() &&
              second_cleanup.cleaned_count == 1 &&
              second_cleanup.refused_count == 2,
          "MMCH-042 authoritative cleanup did not clean evidence-required and retain durable/legal records");

  memory::TempWorkspaceLifecycleManager final_reopen(Policy(root));
  Require(final_reopen.ActiveRecords().size() == 2,
          "MMCH-042 final reopen did not retain durable/legal records");
  auto corrupt = final_reopen.ClassifyForRecovery(
      durable_id,
      memory::TempWorkspaceRecoveryEvidence{
          memory::TempTransactionOutcomeEvidence::none,
          true,
          true,
          false,
          false});
  Require(corrupt.ok() &&
              corrupt.recovery_class == memory::TempRecoveryClass::quarantine_required,
          "MMCH-042 integrity failure did not quarantine durable record");
  Require(final_reopen.Find(legal_id)->state == memory::TempWorkspaceState::review_required,
          "MMCH-042 legal hold record did not remain review-required");

  std::filesystem::remove_all(root);
}

}  // namespace

int main() {
  std::cout << "MMCH-042 authority_note=temp_workspace_recovery_evidence_only;"
               "not_transaction_finality_row_visibility_security_authorization_recovery_parser_donor_wal_benchmark_optimizer_plan_or_agent_action_authority"
            << '\n';
  ReopenRecoverAndRefuseCorrectly();
  return EXIT_SUCCESS;
}
