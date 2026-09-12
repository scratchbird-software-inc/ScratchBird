// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "hierarchical_memory_budget_ledger.hpp"
#include "runtime_platform.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace scratchbird::core::memory {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::u64;
using TempWorkspaceUuid = scratchbird::core::platform::Uuid;

enum class TempStorageClass {
  memory_workspace,
  spill_file,
  temporary_page_space,
  temporary_relation,
  temporary_index,
  materialized_result,
  cursor_backing_store,
  sort_workspace,
  hash_workspace,
  bulk_dml_staging,
  backup_restore_scratch,
  archive_package_scratch,
  verification_scratch,
  udr_workspace,
  parser_workspace
};

enum class TempWorkspaceLifetime {
  statement_lifetime,
  cursor_lifetime,
  result_set_lifetime,
  savepoint_lifetime,
  transaction_lifetime,
  session_lifetime,
  operation_lifetime,
  scheduler_task_lifetime,
  recovery_lifetime,
  administrator_review_lifetime
};

enum class TempTransactionOutcomeEvidence {
  none,
  committed,
  rolled_back,
  in_doubt,
  recovery_required
};

enum class TempRecoveryClass {
  discard_safe,
  discard_after_evidence,
  resume_required,
  operation_owned_resume,
  review_required,
  quarantine_required,
  leaked_cleanup_required,
  cleanup_refused
};

enum class TempCleanupReason {
  statement_end,
  commit,
  rollback,
  disconnect,
  shutdown,
  recovery,
  operation_complete,
  administrator
};

enum class TempWorkspaceState {
  active,
  cleaned,
  cleanup_refused,
  cleanup_failed,
  quarantined,
  review_required,
  // Durable cleanup intent. A missing file is admissible only in this state;
  // the exact owner and reservation remain until cleanup finishes.
  cleanup_pending
};

// MMCH_TEMP_DISK_RESERVATION_SEMANTICS
enum class TempWorkspaceDiskReservationMode {
  logical_quota_only,
  sparse_file,
  physical_preallocate
};

struct TempWorkspacePolicy {
  std::string policy_name = "engine_temp_workspace_default";
  TempWorkspaceUuid database_uuid{};
  TempWorkspaceUuid engine_uuid{};
  // MMCH_MEMORY_METADATA_OPEN_UPGRADE_COMPATIBILITY
  // Temp workspace metadata versions gate open/upgrade decisions. They do not
  // make temp metadata a recovery or transaction authority.
  u64 metadata_format_version = 3;
  u64 manifest_generation = 1;
  std::string manifest_writer_identity;
  std::filesystem::path root_path;
  u64 filespace_quota_bytes = 0;
  u64 session_quota_bytes = 0;
  u64 transaction_quota_bytes = 0;
  u64 statement_quota_bytes = 0;
  u64 operation_quota_bytes = 0;
  bool create_root_path = true;
  bool require_existing_root_path = false;
  bool sparse_file_reservation = true;
  TempWorkspaceDiskReservationMode disk_reservation_mode =
      TempWorkspaceDiskReservationMode::sparse_file;
  bool require_physical_disk_reservation = false;
  bool cleanup_files_on_release = true;
  HierarchicalMemoryBudgetLedger* reservation_ledger = nullptr;
  bool require_ceic_011_reservation = false;
  MemoryCategory reservation_category = MemoryCategory::executor_query_reserved;
  std::string reservation_memory_class = "temp_spill_workspace";
  HierarchicalMemoryBudgetProvenance reservation_provenance;
};

struct TempWorkspaceOwner {
  TempWorkspaceUuid temp_object_uuid{};
  TempWorkspaceUuid database_id{};
  TempWorkspaceUuid engine_id{};
  TempWorkspaceUuid session_id{};
  TempWorkspaceUuid transaction_id{};
  TempWorkspaceUuid statement_id{};
  TempWorkspaceUuid cursor_id{};
  TempWorkspaceUuid result_set_id{};
  TempWorkspaceUuid operation_id{};
  TempWorkspaceUuid scheduler_task_id{};
  u64 policy_generation = 0;
  u64 security_generation = 0;
  TempWorkspaceUuid snapshot_boundary{};
  TempWorkspaceUuid metadata_boundary{};
  TempWorkspaceUuid resource_budget_reference{};
  bool operator==(const TempWorkspaceOwner&) const = default;
};

struct TempWorkspaceAllocationRequest {
  TempStorageClass storage_class = TempStorageClass::spill_file;
  TempWorkspaceLifetime lifetime = TempWorkspaceLifetime::statement_lifetime;
  TempWorkspaceOwner owner;
  u64 bytes = 0;
  std::string purpose;
  bool cluster_temp_workspace_requested = false;
  bool durable_operation_owned = false;
  bool recovery_resume_supported = false;
  bool evidence_required_before_discard = false;
  bool administrator_review_required = false;
  bool legal_hold = false;
};

struct TempWorkspaceRecoveryEvidence {
  TempTransactionOutcomeEvidence transaction_outcome = TempTransactionOutcomeEvidence::none;
  bool engine_recovery_authority = false;
  bool operation_envelope_present = false;
  bool integrity_verified = true;
  bool leaked_after_crash = false;
};

struct TempWorkspaceSecurityEvidence {
  bool random_unguessable_name = false;
  bool exclusive_create_no_overwrite = false;
  bool owner_only_permissions = false;
  bool nofollow_or_platform_equivalent = false;
  bool hardlink_refusal_checked = false;
  std::string platform_semantics;
  std::string authority_boundary;
};

struct TempWorkspaceDiskReservationEvidence {
  TempWorkspaceDiskReservationMode mode =
      TempWorkspaceDiskReservationMode::sparse_file;
  bool logical_quota_reserved = false;
  bool sparse_file_created = false;
  bool sparse_not_physical_reservation = false;
  bool physical_preallocation_attempted = false;
  bool physical_preallocation_complete = false;
  bool physical_preallocation_required = false;
  bool disk_full_or_reservation_failure = false;
  u64 requested_bytes = 0;
  u64 file_size_bytes = 0;
  std::string platform_semantics;
  std::string failure_reason;
  std::string authority_boundary;
};

struct TempWorkspaceBudgetReservationEvidence {
  bool internal_logical_quota_checked = false;
  bool internal_logical_quota_reserved = false;
  bool ceic_011_reservation_applicable = false;
  bool ceic_011_reservation_required = false;
  bool ceic_011_reservation_requested = false;
  bool ceic_011_reservation_granted = false;
  bool ceic_011_reservation_committed = false;
  bool ceic_011_reservation_released = false;
  u64 requested_bytes = 0;
  MemoryCategory category = MemoryCategory::unknown;
  std::string memory_class;
  HierarchicalMemoryReservationToken token;
  std::vector<HierarchicalMemoryScopeRef> scope_chain;
  std::string ledger_model;
  std::string failure_reason;
  std::string authority_boundary;
};

// MMCH_TEMP_WORKSPACE_CROSS_PLATFORM
struct TempWorkspacePlatformSecurityCapabilities {
  std::string platform_name;
  std::string secure_random_provider;
  std::string secure_root_semantics;
  std::string secure_file_semantics;
  std::string disk_reservation_semantics;
  bool secure_random_supported = false;
  bool exclusive_create_supported = false;
  bool owner_only_permissions_supported = false;
  bool nofollow_or_platform_equivalent_supported = false;
  bool hardlink_or_reparse_refusal_supported = false;
  bool physical_preallocation_supported = false;
  bool cleanup_supported = true;
  std::vector<std::string> production_supported_platforms;
  std::vector<std::string> evidence;
};

struct TempWorkspaceRecord {
  std::string allocation_id;
  TempStorageClass storage_class = TempStorageClass::spill_file;
  TempWorkspaceLifetime lifetime = TempWorkspaceLifetime::statement_lifetime;
  TempWorkspaceOwner owner;
  u64 reserved_bytes = 0;
  std::filesystem::path path;
  TempWorkspaceState state = TempWorkspaceState::active;
  TempRecoveryClass recovery_class = TempRecoveryClass::discard_safe;
  std::string purpose;
  bool durable_operation_owned = false;
  bool recovery_resume_supported = false;
  bool evidence_required_before_discard = false;
  bool administrator_review_required = false;
  bool legal_hold = false;
  TempWorkspaceSecurityEvidence security_evidence;
  TempWorkspaceDiskReservationEvidence disk_reservation_evidence;
  TempWorkspaceBudgetReservationEvidence budget_reservation_evidence;
};

struct TempWorkspaceAccountingSnapshot {
  u64 active_bytes = 0;
  u64 peak_bytes = 0;
  u64 allocation_count = 0;
  u64 cleanup_count = 0;
  u64 quota_denial_count = 0;
  u64 cleanup_refusal_count = 0;
  u64 recovery_classification_count = 0;
  u64 ceic_011_reservation_grant_count = 0;
  u64 ceic_011_reservation_refusal_count = 0;
  u64 ceic_011_reservation_release_count = 0;
  u64 ceic_011_reservation_release_failure_count = 0;
  std::map<TempWorkspaceUuid, u64> session_bytes;
  std::map<TempWorkspaceUuid, u64> transaction_bytes;
  std::map<TempWorkspaceUuid, u64> statement_bytes;
  std::map<TempWorkspaceUuid, u64> operation_bytes;
};

struct TempWorkspaceResult {
  Status status;
  DiagnosticRecord diagnostic;
  std::optional<TempWorkspaceRecord> record;

  bool ok() const { return status.ok(); }
};

struct TempWorkspaceCleanupResult {
  Status status;
  DiagnosticRecord diagnostic;
  u64 cleaned_count = 0;
  u64 refused_count = 0;
  u64 failed_count = 0;

  bool ok() const { return status.ok() && failed_count == 0 && refused_count == 0; }
};

struct TempWorkspaceRecoveryResult {
  Status status;
  DiagnosticRecord diagnostic;
  TempRecoveryClass recovery_class = TempRecoveryClass::cleanup_refused;

  bool ok() const { return status.ok(); }
};

class TempWorkspaceLifecycleManager {
 public:
  explicit TempWorkspaceLifecycleManager(TempWorkspacePolicy policy);
  TempWorkspaceLifecycleManager(const TempWorkspaceLifecycleManager&) = delete;
  TempWorkspaceLifecycleManager& operator=(const TempWorkspaceLifecycleManager&) = delete;

  TempWorkspaceResult ReserveTempFilespace(TempWorkspaceAllocationRequest request);
  TempWorkspaceResult AllocateSpillFile(TempWorkspaceAllocationRequest request);
  TempWorkspaceResult AllocateSortSpill(TempWorkspaceAllocationRequest request);
  TempWorkspaceResult AllocateHashSpill(TempWorkspaceAllocationRequest request);

  TempWorkspaceCleanupResult CleanupOnCommit(const TempWorkspaceUuid& transaction_id,
                                             TempTransactionOutcomeEvidence evidence);
  TempWorkspaceCleanupResult CleanupOnRollback(const TempWorkspaceUuid& transaction_id,
                                               TempTransactionOutcomeEvidence evidence);
  TempWorkspaceCleanupResult CleanupOnDisconnect(const TempWorkspaceUuid& session_id);
  TempWorkspaceCleanupResult CleanupOnShutdown();
  TempWorkspaceCleanupResult CleanupOperation(const TempWorkspaceUuid& operation_id);
  TempWorkspaceCleanupResult CleanupRecoverySafe(const TempWorkspaceRecoveryEvidence& evidence);

  TempWorkspaceRecoveryResult ClassifyForRecovery(const std::string& allocation_id,
                                                  const TempWorkspaceRecoveryEvidence& evidence);
  TempWorkspaceRecoveryResult ClassifyRecordForRecovery(const TempWorkspaceRecord& record,
                                                        const TempWorkspaceRecoveryEvidence& evidence) const;

  std::optional<TempWorkspaceRecord> Find(const std::string& allocation_id) const;
  std::vector<TempWorkspaceRecord> ActiveRecords() const;
  TempWorkspaceAccountingSnapshot Snapshot() const;
  const TempWorkspacePolicy& policy() const;

 private:
  struct QuotaCheck {
    bool allowed = true;
    std::string dimension;
    u64 limit = 0;
    u64 current = 0;
  };

  struct BudgetReservationResult {
    bool ok = true;
    Status status;
    DiagnosticRecord diagnostic;
    TempWorkspaceBudgetReservationEvidence evidence;
  };

  TempWorkspaceResult Allocate(TempWorkspaceAllocationRequest request, TempStorageClass storage_class);
  QuotaCheck CheckQuotaLocked(const TempWorkspaceAllocationRequest& request) const;
  BudgetReservationResult ReserveBudgetLocked(const TempWorkspaceAllocationRequest& request,
                                              TempStorageClass storage_class) const;
  bool CommitBudgetReservationLocked(TempWorkspaceBudgetReservationEvidence* evidence,
                                     const TempWorkspaceOwner& owner,
                                     DiagnosticRecord* diagnostic) const;
  bool ReleaseBudgetReservationLocked(const TempWorkspaceRecord& record,
                                      TempCleanupReason reason,
                                      DiagnosticRecord* diagnostic);
  TempWorkspaceCleanupResult CleanupWhereLocked(TempCleanupReason reason,
                                                TempTransactionOutcomeEvidence evidence,
                                                const TempWorkspaceUuid& scope_id);
  bool RecordMatchesCleanupScope(const TempWorkspaceRecord& record,
                                 TempCleanupReason reason,
                                 const TempWorkspaceUuid& scope_id) const;
  bool CleanupRequiresOutcome(TempCleanupReason reason) const;
  bool CleanupOutcomeMatches(TempCleanupReason reason, TempTransactionOutcomeEvidence evidence) const;
  bool ProtectedFromOrdinaryCleanup(const TempWorkspaceRecord& record) const;
  bool PrepareCleanupIntentLocked(TempWorkspaceRecord& record, DiagnosticRecord* diagnostic);
  bool RemoveRecordFile(const TempWorkspaceRecord& record, DiagnosticRecord* diagnostic) const;
  void AddAccountingLocked(const TempWorkspaceRecord& record);
  void RemoveAccountingLocked(const TempWorkspaceRecord& record);
  bool LoadManifestFromDisk();
  bool PersistManifestLocked(DiagnosticRecord* diagnostic, bool* published = nullptr);
  std::filesystem::path ManifestPath() const;
  std::filesystem::path ManifestPathForVersion(u64 version) const;
  std::optional<TempWorkspaceRecord> ParseManifestRecord(const std::string& bytes) const;
  std::string SerializeManifestRecord(const TempWorkspaceRecord& record) const;
  std::optional<std::string> NextAllocationIdLocked(const TempWorkspaceAllocationRequest& request,
                                                    std::string* error) const;
  std::filesystem::path PathForAllocationLocked(const std::string& allocation_id,
                                                const TempWorkspaceAllocationRequest& request) const;
  Status TempStatus(scratchbird::core::platform::StatusCode code, Severity severity) const;
  DiagnosticRecord MakeDiagnostic(Status status,
                                  std::string diagnostic_code,
                                  std::string message_key,
                                  const TempWorkspaceOwner& owner,
                                  const std::vector<scratchbird::core::platform::DiagnosticArgument>& extra = {}) const;

  TempWorkspacePolicy policy_;
  std::string root_path_validation_error_;
  mutable std::mutex mutex_;
  TempWorkspaceAccountingSnapshot accounting_;
  std::map<std::string, TempWorkspaceRecord> active_;
  u64 manifest_generation_ = 0;
};

const char* TempStorageClassName(TempStorageClass value);
const char* TempWorkspaceLifetimeName(TempWorkspaceLifetime value);
const char* TempTransactionOutcomeEvidenceName(TempTransactionOutcomeEvidence value);
const char* TempRecoveryClassName(TempRecoveryClass value);
const char* TempCleanupReasonName(TempCleanupReason value);
const char* TempWorkspaceStateName(TempWorkspaceState value);
const char* TempWorkspaceDiskReservationModeName(TempWorkspaceDiskReservationMode value);
TempWorkspacePlatformSecurityCapabilities CurrentTempWorkspacePlatformSecurityCapabilities();

}  // namespace scratchbird::core::memory
