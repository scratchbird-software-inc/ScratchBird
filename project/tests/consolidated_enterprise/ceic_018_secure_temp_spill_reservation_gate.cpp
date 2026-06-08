// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// CEIC-018 focused validation for secure temp spill creation and CEIC-011
// reservation integration.
#include "temp_workspace_lifecycle.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
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

void RequireAuthorityDiagnostic(const platform::DiagnosticRecord& diagnostic,
                                std::string_view context) {
  Require(HasDiagnosticArgument(diagnostic, "authority_boundary", kAuthorityBoundary),
          std::string(context) + " diagnostic lacked CEIC-018 authority boundary");
  Require(diagnostic.remediation_hint.find("resource and security evidence only") !=
              std::string::npos,
          std::string(context) + " diagnostic remediation overclaimed authority");
}

std::filesystem::path MakeTempDir(std::string_view prefix) {
#if defined(_WIN32)
  (void)prefix;
  Fail("CEIC-018 POSIX secure temp path gate is not enabled on this platform");
#else
  std::string tmpl = "/tmp/";
  tmpl += prefix;
  tmpl += ".XXXXXX";
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  Require(made != nullptr, "CEIC-018 mkdtemp failed");
  return std::filesystem::path(made);
#endif
}

memory::HierarchicalMemoryBudgetProvenance RuntimeProvenance() {
  memory::HierarchicalMemoryBudgetProvenance provenance;
  provenance.source = memory::HierarchicalMemoryBudgetProvenanceSource::runtime_policy;
  provenance.source_label = "ceic_018_secure_temp_spill_reservation_gate";
  return provenance;
}

memory::HierarchicalMemoryScopeRef Scope(memory::HierarchicalMemoryScopeKind kind,
                                         std::string id) {
  return {kind, std::move(id)};
}

void SetBudget(memory::HierarchicalMemoryBudgetLedger* ledger,
               memory::HierarchicalMemoryScopeRef scope,
               std::uint64_t hard_limit_bytes) {
  memory::HierarchicalMemoryBudget budget;
  budget.scope = std::move(scope);
  budget.hard_limit_bytes = hard_limit_bytes;
  budget.provenance = RuntimeProvenance();
  Require(ledger->SetBudget(std::move(budget)).ok(), "CEIC-018 budget setup failed");
}

memory::TempWorkspacePolicy Policy(
    const std::filesystem::path& root,
    memory::TempWorkspaceDiskReservationMode mode =
        memory::TempWorkspaceDiskReservationMode::sparse_file,
    memory::HierarchicalMemoryBudgetLedger* ledger = nullptr,
    std::uint64_t quota = 1024 * 1024) {
  memory::TempWorkspacePolicy policy;
  policy.policy_name = "CEIC-018";
  policy.root_path = root;
  policy.filespace_quota_bytes = quota;
  policy.session_quota_bytes = quota;
  policy.transaction_quota_bytes = quota;
  policy.statement_quota_bytes = quota;
  policy.operation_quota_bytes = quota;
  policy.create_root_path = true;
  policy.sparse_file_reservation =
      mode == memory::TempWorkspaceDiskReservationMode::sparse_file;
  policy.disk_reservation_mode = mode;
  policy.require_physical_disk_reservation =
      mode == memory::TempWorkspaceDiskReservationMode::physical_preallocate;
  policy.reservation_ledger = ledger;
  policy.require_ceic_011_reservation = ledger != nullptr;
  policy.reservation_category = memory::MemoryCategory::executor_query_reserved;
  policy.reservation_memory_class = "ceic_018.temp_spill_workspace";
  policy.reservation_provenance = RuntimeProvenance();
  return policy;
}

memory::TempWorkspaceOwner Owner(std::string suffix) {
  memory::TempWorkspaceOwner owner;
  owner.temp_object_uuid = "ceic018-" + std::move(suffix);
  owner.database_id = "database-ceic018";
  owner.engine_id = "engine-ceic018";
  owner.session_id = "session-ceic018";
  owner.transaction_id = "txn-ceic018";
  owner.statement_id = "stmt-ceic018";
  owner.operation_id = "op-ceic018";
  owner.policy_generation = 18;
  owner.security_generation = 180;
  owner.snapshot_boundary = "mga-snapshot-not-temp-authority";
  owner.metadata_boundary = "metadata-boundary-not-recovery-authority";
  owner.resource_budget_reference = "ceic-011-ledger";
  return owner;
}

memory::TempWorkspaceAllocationRequest Request(
    std::string suffix,
    std::uint64_t bytes = 4096,
    memory::TempWorkspaceLifetime lifetime =
        memory::TempWorkspaceLifetime::statement_lifetime) {
  memory::TempWorkspaceAllocationRequest request;
  request.owner = Owner(std::move(suffix));
  request.lifetime = lifetime;
  request.bytes = bytes;
  request.purpose = "CEIC-018 secure temp spill reservation gate";
  return request;
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
  Require(name.rfind("tw-", 0) == 0, "CEIC-018 filename did not use tw prefix");
  Require(name.size() > 35, "CEIC-018 filename too short for random token");
  const std::string token = name.substr(3, 32);
  Require(IsHexToken(token), "CEIC-018 filename did not carry 128-bit hex token");
  return token;
}

std::uint64_t FileSize(const std::filesystem::path& path) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  Require(!ec, "CEIC-018 could not read temp file size");
  return static_cast<std::uint64_t>(size);
}

void RequireOwnerOnlyRegularFile(const std::filesystem::path& path) {
#if defined(_WIN32)
  (void)path;
  Fail("CEIC-018 owner-only POSIX permission check is not enabled on this platform");
#else
  struct stat st {};
  Require(::lstat(path.c_str(), &st) == 0, "CEIC-018 lstat failed");
  Require(S_ISREG(st.st_mode), "CEIC-018 temp path was not a regular file");
  Require((st.st_mode & 0777) == (S_IRUSR | S_IWUSR),
          "CEIC-018 temp file was not owner-only 0600");
  Require(st.st_nlink == 1, "CEIC-018 temp file hardlink count was not one");
#endif
}

void SecureRandomExclusiveCreateAndPermissions() {
  const auto root = MakeTempDir("sb_ceic018_secure");
  memory::HierarchicalMemoryBudgetLedger ledger;
  memory::TempWorkspaceLifecycleManager manager(Policy(root, memory::TempWorkspaceDiskReservationMode::sparse_file, &ledger));

  std::set<std::string> names;
  std::set<std::string> tokens;
  std::vector<std::filesystem::path> paths;
  for (int i = 0; i < 8; ++i) {
    auto allocated = manager.AllocateSpillFile(Request("secure-" + std::to_string(i), 2048));
    Require(allocated.ok() && allocated.record.has_value(),
            "CEIC-018 secure spill allocation failed");
    const auto& record = *allocated.record;
    Require(record.path.parent_path() == root, "CEIC-018 temp path escaped root");
    Require(names.insert(record.path.filename().string()).second,
            "CEIC-018 secure temp filename was reused");
    Require(tokens.insert(RandomTokenFromFileName(record.path)).second,
            "CEIC-018 secure temp random token was reused");
    Require(record.security_evidence.random_unguessable_name,
            "CEIC-018 random-name evidence missing");
    Require(record.security_evidence.exclusive_create_no_overwrite,
            "CEIC-018 exclusive-create evidence missing");
    Require(record.security_evidence.owner_only_permissions,
            "CEIC-018 owner-only evidence missing");
    Require(record.security_evidence.nofollow_or_platform_equivalent,
            "CEIC-018 no-follow evidence missing");
    Require(record.security_evidence.hardlink_refusal_checked,
            "CEIC-018 hardlink evidence missing");
    Require(record.security_evidence.authority_boundary == kAuthorityBoundary,
            "CEIC-018 security authority boundary missing");
    Require(record.budget_reservation_evidence.ceic_011_reservation_requested,
            "CEIC-018 CEIC-011 reservation request evidence missing");
    Require(record.budget_reservation_evidence.ceic_011_reservation_granted,
            "CEIC-018 CEIC-011 reservation grant evidence missing");
    Require(record.budget_reservation_evidence.ceic_011_reservation_committed,
            "CEIC-018 CEIC-011 reservation commit evidence missing");
    Require(record.budget_reservation_evidence.ledger_model ==
                "hierarchical_memory_budget_ledger",
            "CEIC-018 CEIC-011 ledger model evidence missing");
    RequireOwnerOnlyRegularFile(record.path);
    paths.push_back(record.path);
  }

  Require(ledger.Snapshot().current_bytes == 8 * 2048,
          "CEIC-018 ledger did not track active temp bytes");
  const auto cleaned = manager.CleanupOnShutdown();
  Require(cleaned.ok() && cleaned.cleaned_count == paths.size(),
          "CEIC-018 shutdown cleanup failed");
  Require(ledger.Snapshot().current_bytes == 0,
          "CEIC-018 ledger leaked bytes after shutdown cleanup");
  Require(manager.Snapshot().ceic_011_reservation_release_count == paths.size(),
          "CEIC-018 temp manager did not record CEIC-011 releases");
  for (const auto& path : paths) {
    Require(!std::filesystem::exists(path), "CEIC-018 cleanup left temp file");
  }
  std::filesystem::remove_all(root);
}

void PrecreatedTargetsAndSymlinkRootsFailClosed() {
  const auto root = MakeTempDir("sb_ceic018_precreated");
  const auto victim = root / "victim";
  {
    std::ofstream out(victim);
    out << "victim-content";
  }
  const auto old_predictable = root / "tw-1-ceic018-precreated-spill_file.spill";
  {
    std::ofstream out(old_predictable);
    out << "attacker-content";
  }
#if !defined(_WIN32)
  const auto old_predictable_symlink = root / "tw-2-ceic018-symlink-spill_file.spill";
  Require(::symlink(victim.c_str(), old_predictable_symlink.c_str()) == 0,
          "CEIC-018 failed to create symlink probe");
#endif

  memory::HierarchicalMemoryBudgetLedger ledger;
  memory::TempWorkspaceLifecycleManager manager(Policy(root, memory::TempWorkspaceDiskReservationMode::sparse_file, &ledger));
  auto allocated = manager.AllocateSpillFile(Request("precreated", 1024));
  Require(allocated.ok() && allocated.record.has_value(),
          "CEIC-018 random allocation failed near precreated target");
  Require(allocated.record->path != old_predictable,
          "CEIC-018 selected old predictable target");
  {
    std::ifstream in(old_predictable);
    std::string value;
    in >> value;
    Require(value == "attacker-content", "CEIC-018 overwrote precreated file");
  }
  {
    std::ifstream in(victim);
    std::string value;
    in >> value;
    Require(value == "victim-content", "CEIC-018 overwrote symlink victim");
  }
  Require(manager.CleanupOnShutdown().ok(), "CEIC-018 precreated cleanup failed");
  Require(std::filesystem::exists(old_predictable),
          "CEIC-018 cleanup removed unrelated precreated file");

#if !defined(_WIN32)
  const auto parent = MakeTempDir("sb_ceic018_symlink_parent");
  const auto real_root = parent / "real-root";
  const auto symlink_root = parent / "symlink-root";
  std::filesystem::create_directory(real_root);
  Require(::symlink(real_root.c_str(), symlink_root.c_str()) == 0,
          "CEIC-018 failed to create symlink root probe");
  memory::TempWorkspaceLifecycleManager symlink_manager(Policy(symlink_root, memory::TempWorkspaceDiskReservationMode::sparse_file, &ledger));
  auto refused = symlink_manager.AllocateSpillFile(Request("symlink-root", 1024));
  Require(!refused.ok() && !refused.record.has_value(),
          "CEIC-018 accepted symlink workspace root");
  Require(refused.diagnostic.diagnostic_code == "TEMP_WORKSPACE.ROOT_UNSAFE",
          "CEIC-018 symlink root diagnostic changed");
  RequireAuthorityDiagnostic(refused.diagnostic, "symlink root");
  Require(std::filesystem::is_empty(real_root),
          "CEIC-018 symlink root refusal created a target file");
  std::filesystem::remove_all(parent);
#endif

  std::filesystem::remove_all(root);
}

void ReservationModesAndQuotaRefusals() {
  {
    const auto root = MakeTempDir("sb_ceic018_logical");
    memory::HierarchicalMemoryBudgetLedger ledger;
    memory::TempWorkspaceLifecycleManager manager(
        Policy(root, memory::TempWorkspaceDiskReservationMode::logical_quota_only, &ledger));
    auto allocated = manager.AllocateSpillFile(Request("logical", 4096));
    Require(allocated.ok() && allocated.record.has_value(),
            "CEIC-018 logical allocation failed");
    const auto& evidence = allocated.record->disk_reservation_evidence;
    Require(evidence.logical_quota_reserved, "CEIC-018 logical quota evidence missing");
    Require(!evidence.sparse_file_created, "CEIC-018 logical mode created sparse evidence");
    Require(!evidence.physical_preallocation_attempted,
            "CEIC-018 logical mode attempted physical preallocation");
    Require(FileSize(allocated.record->path) == 0,
            "CEIC-018 logical mode claimed file-space reservation");
    Require(manager.CleanupOnShutdown().ok(), "CEIC-018 logical cleanup failed");
    std::filesystem::remove_all(root);
  }

  {
    const auto root = MakeTempDir("sb_ceic018_sparse");
    memory::HierarchicalMemoryBudgetLedger ledger;
    memory::TempWorkspaceLifecycleManager manager(
        Policy(root, memory::TempWorkspaceDiskReservationMode::sparse_file, &ledger));
    auto allocated = manager.AllocateSpillFile(Request("sparse", 8192));
    Require(allocated.ok() && allocated.record.has_value(),
            "CEIC-018 sparse allocation failed");
    const auto& evidence = allocated.record->disk_reservation_evidence;
    Require(evidence.sparse_file_created, "CEIC-018 sparse evidence missing");
    Require(evidence.sparse_not_physical_reservation,
            "CEIC-018 sparse mode did not disclaim physical reservation");
    Require(!evidence.physical_preallocation_complete,
            "CEIC-018 sparse mode claimed physical preallocation");
    Require(FileSize(allocated.record->path) == 8192,
            "CEIC-018 sparse file size mismatch");
    Require(manager.CleanupOnShutdown().ok(), "CEIC-018 sparse cleanup failed");
    std::filesystem::remove_all(root);
  }

  {
    const auto root = MakeTempDir("sb_ceic018_physical");
    memory::HierarchicalMemoryBudgetLedger ledger;
    memory::TempWorkspaceLifecycleManager manager(
        Policy(root, memory::TempWorkspaceDiskReservationMode::physical_preallocate, &ledger));
    auto allocated = manager.AllocateSpillFile(Request("physical", 4096));
    Require(allocated.ok() && allocated.record.has_value(),
            "CEIC-018 physical preallocation failed");
    const auto& evidence = allocated.record->disk_reservation_evidence;
    Require(evidence.physical_preallocation_required,
            "CEIC-018 physical required evidence missing");
    Require(evidence.physical_preallocation_attempted,
            "CEIC-018 physical attempt evidence missing");
    Require(evidence.physical_preallocation_complete,
            "CEIC-018 physical complete evidence missing");
    Require(FileSize(allocated.record->path) == 4096,
            "CEIC-018 physical file size mismatch");
    Require(manager.CleanupOnShutdown().ok(), "CEIC-018 physical cleanup failed");
    std::filesystem::remove_all(root);
  }

  {
    const auto root = MakeTempDir("sb_ceic018_physical_fail");
    memory::HierarchicalMemoryBudgetLedger ledger;
    memory::TempWorkspaceLifecycleManager manager(
        Policy(root,
               memory::TempWorkspaceDiskReservationMode::physical_preallocate,
               &ledger,
               std::numeric_limits<std::uint64_t>::max()));
    auto refused = manager.AllocateSpillFile(
        Request("physical-fail", std::numeric_limits<std::uint64_t>::max()));
    Require(!refused.ok() && !refused.record.has_value(),
            "CEIC-018 impossible physical reservation succeeded");
    Require(refused.diagnostic.diagnostic_code == "TEMP_WORKSPACE.SPILL_RESERVE_FAILED",
            "CEIC-018 physical failure diagnostic changed");
    RequireAuthorityDiagnostic(refused.diagnostic, "physical failure");
    Require(ledger.Snapshot().current_bytes == 0,
            "CEIC-018 physical failure leaked CEIC-011 bytes");
    Require(std::filesystem::is_empty(root),
            "CEIC-018 physical failure left a temp file");
    std::filesystem::remove_all(root);
  }

  {
    const auto root = MakeTempDir("sb_ceic018_quota");
    memory::HierarchicalMemoryBudgetLedger ledger;
    memory::TempWorkspaceLifecycleManager internal_quota_manager(
        Policy(root, memory::TempWorkspaceDiskReservationMode::sparse_file, &ledger, 1024));
    auto refused = internal_quota_manager.AllocateSpillFile(Request("internal-quota", 2048));
    Require(!refused.ok() && refused.diagnostic.diagnostic_code == "TEMP_WORKSPACE.QUOTA_DENIED",
            "CEIC-018 internal quota refusal failed");
    Require(ledger.Snapshot().current_bytes == 0,
            "CEIC-018 internal quota refusal reached CEIC-011 ledger");
    std::filesystem::remove_all(root);
  }

  {
    const auto root = MakeTempDir("sb_ceic018_ledger_quota");
    memory::HierarchicalMemoryBudgetLedger ledger;
    SetBudget(&ledger, Scope(memory::HierarchicalMemoryScopeKind::process, "engine-ceic018"), 1024);
    memory::TempWorkspaceLifecycleManager manager(
        Policy(root, memory::TempWorkspaceDiskReservationMode::sparse_file, &ledger));
    auto refused = manager.AllocateSpillFile(Request("ledger-quota", 2048));
    Require(!refused.ok(), "CEIC-018 CEIC-011 quota refusal succeeded");
    Require(refused.diagnostic.diagnostic_code == "SB-MEMORY-BUDGET-HARD-LIMIT-REFUSED",
            "CEIC-018 CEIC-011 quota diagnostic changed");
    Require(manager.Snapshot().ceic_011_reservation_refusal_count == 1,
            "CEIC-018 manager did not count CEIC-011 refusal");
    Require(ledger.Snapshot().hard_limit_refusal_count == 1,
            "CEIC-018 ledger did not count hard-limit refusal");
    Require(std::filesystem::is_empty(root), "CEIC-018 CEIC-011 refusal created a file");
    std::filesystem::remove_all(root);
  }

  {
    const auto root = MakeTempDir("sb_ceic018_missing_ledger");
    auto policy = Policy(root);
    policy.require_ceic_011_reservation = true;
    policy.reservation_ledger = nullptr;
    memory::TempWorkspaceLifecycleManager manager(policy);
    auto refused = manager.AllocateSpillFile(Request("missing-ledger", 1024));
    Require(!refused.ok() &&
                refused.diagnostic.diagnostic_code ==
                    "TEMP_WORKSPACE.CEIC011_RESERVATION_REQUIRED",
            "CEIC-018 missing CEIC-011 ledger did not fail closed");
    RequireAuthorityDiagnostic(refused.diagnostic, "missing CEIC-011 ledger");
    std::filesystem::remove_all(root);
  }
}

void CleanupScopesReleaseLedger() {
  const auto root = MakeTempDir("sb_ceic018_cleanup");
  memory::HierarchicalMemoryBudgetLedger ledger;
  memory::TempWorkspaceLifecycleManager manager(Policy(root, memory::TempWorkspaceDiskReservationMode::sparse_file, &ledger));

  auto commit = manager.AllocateSpillFile(
      Request("commit", 1024, memory::TempWorkspaceLifetime::transaction_lifetime));
  auto operation = manager.AllocateSpillFile(
      Request("operation", 1024, memory::TempWorkspaceLifetime::operation_lifetime));
  auto shutdown = manager.AllocateSpillFile(
      Request("shutdown", 1024, memory::TempWorkspaceLifetime::session_lifetime));
  Require(commit.ok() && operation.ok() && shutdown.ok(),
          "CEIC-018 cleanup setup allocation failed");
  Require(ledger.Snapshot().current_bytes == 3072,
          "CEIC-018 cleanup setup ledger bytes mismatch");

  auto bad_commit = manager.CleanupOnCommit(
      "txn-ceic018", memory::TempTransactionOutcomeEvidence::rolled_back);
  Require(!bad_commit.ok() && bad_commit.refused_count != 0,
          "CEIC-018 commit cleanup accepted rollback evidence");
  Require(ledger.Snapshot().current_bytes == 3072,
          "CEIC-018 bad commit evidence released ledger bytes");

  auto commit_cleaned = manager.CleanupOnCommit(
      "txn-ceic018", memory::TempTransactionOutcomeEvidence::committed);
  Require(commit_cleaned.ok() && commit_cleaned.cleaned_count == 1,
          "CEIC-018 commit cleanup failed");
  auto operation_cleaned = manager.CleanupOperation("op-ceic018");
  Require(operation_cleaned.ok() && operation_cleaned.cleaned_count == 1,
          "CEIC-018 operation cleanup failed");
  auto shutdown_cleaned = manager.CleanupOnShutdown();
  Require(shutdown_cleaned.ok() && shutdown_cleaned.cleaned_count == 1,
          "CEIC-018 shutdown cleanup failed");
  Require(ledger.Snapshot().current_bytes == 0,
          "CEIC-018 cleanup leaked CEIC-011 bytes");
  Require(manager.Snapshot().active_bytes == 0,
          "CEIC-018 cleanup leaked temp active bytes");
  std::filesystem::remove_all(root);
}

void RecoveryClassificationIsDeterministic() {
  const auto root = MakeTempDir("sb_ceic018_recovery");
  memory::HierarchicalMemoryBudgetLedger ledger;
  {
    memory::TempWorkspaceLifecycleManager manager(Policy(root, memory::TempWorkspaceDiskReservationMode::sparse_file, &ledger));
    auto ordinary = manager.AllocateSpillFile(
        Request("ordinary", 128, memory::TempWorkspaceLifetime::statement_lifetime));
    auto resumable_request =
        Request("resumable", 128, memory::TempWorkspaceLifetime::operation_lifetime);
    resumable_request.durable_operation_owned = true;
    resumable_request.recovery_resume_supported = true;
    auto resumable = manager.AllocateSpillFile(std::move(resumable_request));
    auto resume_required_request =
        Request("resume-required", 128, memory::TempWorkspaceLifetime::recovery_lifetime);
    resume_required_request.recovery_resume_supported = true;
    auto resume_required = manager.AllocateSpillFile(std::move(resume_required_request));
    auto legal_request =
        Request("legal", 128, memory::TempWorkspaceLifetime::session_lifetime);
    legal_request.legal_hold = true;
    auto legal = manager.AllocateSpillFile(std::move(legal_request));
    auto admin_request =
        Request("admin", 128, memory::TempWorkspaceLifetime::administrator_review_lifetime);
    admin_request.administrator_review_required = true;
    auto admin = manager.AllocateSpillFile(std::move(admin_request));
    Require(ordinary.ok() && resumable.ok() && resume_required.ok() && legal.ok() && admin.ok(),
            "CEIC-018 recovery setup allocation failed");
  }

  memory::TempWorkspaceLifecycleManager reopened(Policy(root));
  Require(reopened.ActiveRecords().size() == 5,
          "CEIC-018 recovery manifest reload count mismatch");
  memory::TempWorkspaceRecoveryEvidence leaked;
  leaked.leaked_after_crash = true;
  auto first_cleanup = reopened.CleanupRecoverySafe(leaked);
  Require(!first_cleanup.ok() && first_cleanup.cleaned_count == 1 &&
              first_cleanup.refused_count == 4,
          "CEIC-018 leaked cleanup classification mismatch");

  const auto records = reopened.ActiveRecords();
  std::string durable_id;
  std::string resume_id;
  std::string legal_id;
  std::string admin_id;
  for (const auto& record : records) {
    if (record.durable_operation_owned) {
      durable_id = record.allocation_id;
    } else if (record.recovery_resume_supported) {
      resume_id = record.allocation_id;
    } else if (record.legal_hold) {
      legal_id = record.allocation_id;
    } else if (record.administrator_review_required) {
      admin_id = record.allocation_id;
    }
  }
  Require(!durable_id.empty() && !resume_id.empty() && !legal_id.empty() && !admin_id.empty(),
          "CEIC-018 recovery setup records were not found");

  memory::TempWorkspaceRecoveryEvidence authority;
  authority.engine_recovery_authority = true;
  authority.operation_envelope_present = true;
  auto durable_class = reopened.ClassifyForRecovery(durable_id, authority);
  Require(durable_class.ok() &&
              durable_class.recovery_class == memory::TempRecoveryClass::operation_owned_resume,
          "CEIC-018 operation-owned record did not classify resumable");
  auto resume_class = reopened.ClassifyForRecovery(resume_id, authority);
  Require(resume_class.ok() &&
              resume_class.recovery_class == memory::TempRecoveryClass::resume_required,
          "CEIC-018 recovery-lifetime record did not require resume");
  auto legal_class = reopened.ClassifyForRecovery(legal_id, authority);
  auto admin_class = reopened.ClassifyForRecovery(admin_id, authority);
  Require(legal_class.ok() &&
              legal_class.recovery_class == memory::TempRecoveryClass::review_required,
          "CEIC-018 legal hold was not review-required");
  Require(admin_class.ok() &&
              admin_class.recovery_class == memory::TempRecoveryClass::review_required,
          "CEIC-018 admin-review record was not review-required");

  auto cleanup = reopened.CleanupRecoverySafe(authority);
  Require(!cleanup.ok() && cleanup.cleaned_count == 0 && cleanup.refused_count == 4,
          "CEIC-018 recovery cleanup removed protected/resumable records");
  std::filesystem::remove_all(root);
}

void AuthorityAndClusterBoundariesFailClosed() {
  const auto root = MakeTempDir("sb_ceic018_authority");
  memory::HierarchicalMemoryBudgetLedger ledger;
  memory::TempWorkspaceLifecycleManager manager(Policy(root, memory::TempWorkspaceDiskReservationMode::sparse_file, &ledger));
  auto request = Request("cluster", 1024);
  request.cluster_temp_workspace_requested = true;
  auto refused = manager.AllocateSpillFile(std::move(request));
  Require(!refused.ok() &&
              refused.diagnostic.diagnostic_code == "TEMP_WORKSPACE.CLUSTER_OUT_OF_SCOPE",
          "CEIC-018 cluster temp request did not fail closed");
  RequireAuthorityDiagnostic(refused.diagnostic, "cluster boundary");

  auto allocated = manager.AllocateSpillFile(Request("authority", 1024));
  Require(allocated.ok() && allocated.record.has_value(),
          "CEIC-018 authority allocation failed");
  const auto& record = *allocated.record;
  Require(record.security_evidence.authority_boundary == kAuthorityBoundary,
          "CEIC-018 security evidence overclaimed authority");
  Require(record.disk_reservation_evidence.authority_boundary == kAuthorityBoundary,
          "CEIC-018 disk evidence overclaimed authority");
  Require(record.budget_reservation_evidence.authority_boundary == kAuthorityBoundary,
          "CEIC-018 budget evidence overclaimed authority");
  Require(record.budget_reservation_evidence.authority_boundary.find("transaction_finality") !=
              std::string::npos,
          "CEIC-018 authority boundary missing transaction finality");
  Require(record.budget_reservation_evidence.authority_boundary.find("row_visibility") !=
              std::string::npos,
          "CEIC-018 authority boundary missing row visibility");
  Require(record.budget_reservation_evidence.authority_boundary.find("security_authorization") !=
              std::string::npos,
          "CEIC-018 authority boundary missing security/authorization");
  Require(record.budget_reservation_evidence.authority_boundary.find("recovery") !=
              std::string::npos,
          "CEIC-018 authority boundary missing recovery");
  Require(record.budget_reservation_evidence.authority_boundary.find("parser") !=
              std::string::npos,
          "CEIC-018 authority boundary missing parser");
  Require(record.budget_reservation_evidence.authority_boundary.find("donor") !=
              std::string::npos,
          "CEIC-018 authority boundary missing donor");
  Require(record.budget_reservation_evidence.authority_boundary.find("wal") != std::string::npos,
          "CEIC-018 authority boundary missing WAL");
  Require(record.budget_reservation_evidence.authority_boundary.find("benchmark") !=
              std::string::npos,
          "CEIC-018 authority boundary missing benchmark");
  Require(record.budget_reservation_evidence.authority_boundary.find("optimizer_plan") !=
              std::string::npos,
          "CEIC-018 authority boundary missing optimizer plan");
  Require(record.budget_reservation_evidence.authority_boundary.find("agent_action") !=
              std::string::npos,
          "CEIC-018 authority boundary missing agent action");
  Require(manager.CleanupOnShutdown().ok(), "CEIC-018 authority cleanup failed");
  std::filesystem::remove_all(root);
}

}  // namespace

int main() {
  std::cout << "CEIC-018 authority_note=temp_spill_reservation_evidence_only;"
               "not_transaction_finality_row_visibility_security_authorization_recovery_"
               "parser_donor_wal_benchmark_optimizer_plan_or_agent_action_authority\n";
  SecureRandomExclusiveCreateAndPermissions();
  PrecreatedTargetsAndSymlinkRootsFailClosed();
  ReservationModesAndQuotaRefusals();
  CleanupScopesReleaseLedger();
  RecoveryClassificationIsDeterministic();
  AuthorityAndClusterBoundariesFailClosed();
  return EXIT_SUCCESS;
}
