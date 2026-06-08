// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "cluster_catalog_record_codec.hpp"
#include "filespace_lifecycle.hpp"
#include "transaction_cleanup.hpp"

#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

enum class BackupArchiveLifecycleOperation {
  logical_backup,
  physical_backup,
  logical_restore,
  physical_restore,
  delta_package,
  delta_apply,
  shadow_snapshot
};

struct BackupArchiveLifecycleAdmission {
  BackupArchiveLifecycleOperation operation = BackupArchiveLifecycleOperation::logical_backup;
  bool admitted = false;
  bool snapshot_hold_required = false;
  bool snapshot_hold_acquired = false;
  bool filespace_hold_required = false;
  bool filespace_hold_acquired = false;
  bool shutdown_blocker_registered = false;
  bool drop_blocker_registered = false;
  bool legal_retention_satisfied = false;
  bool restore_inspection_open = false;
  bool recovery_classification_verified = false;
  bool live_file_shortcut_refused = true;
  bool engine_owned_path = true;
  bool lifecycle_policy_present = true;
  std::string lifecycle_policy_ref = "backup.archive.default_policy.v1";
  EngineApiDiagnostic diagnostic;
  std::vector<EngineEvidenceReference> evidence;
};

const char* BackupArchiveLifecycleOperationName(BackupArchiveLifecycleOperation operation);
BackupArchiveLifecycleAdmission EvaluateBackupArchiveLifecycleAdmission(
    const EngineApiRequest& request,
    BackupArchiveLifecycleOperation operation);

// SEARCH_KEY: DISASTER_RECOVERY_DRILLS
// SEARCH_KEY: SB_ENGINE_INTERNAL_API_BACKUP_ARCHIVE_LOGICAL_BACKUP
struct EngineStartLogicalBackupRequest : EngineApiRequest {};
struct EngineStartLogicalBackupResult : EngineApiResult {
  EngineUuid backup_uuid;
  EngineUuid snapshot_uuid;
  EngineApiU64 snapshot_visible_through_local_transaction_id = 0;
  EngineApiU64 table_count = 0;
  EngineApiU64 row_count = 0;
  EngineApiU64 index_count = 0;
  std::string manifest_uri;
};
EngineStartLogicalBackupResult EngineStartLogicalBackup(const EngineStartLogicalBackupRequest& request);

struct EngineStartPhysicalBackupRequest : EngineApiRequest {};
struct EngineStartPhysicalBackupResult : EngineApiResult {
  EngineUuid backup_uuid;
  std::string manifest_uri;
  std::string image_uri;
  EngineApiU64 image_bytes = 0;
};
EngineStartPhysicalBackupResult EngineStartPhysicalBackup(const EngineStartPhysicalBackupRequest& request);

struct EngineRestorePhysicalBackupRequest : EngineApiRequest {};
struct EngineRestorePhysicalBackupResult : EngineApiResult {
  EngineUuid restore_uuid;
  EngineUuid source_backup_uuid;
  std::string source_manifest_uri;
  std::string restored_database_path;
  EngineApiU64 image_bytes = 0;
};
EngineRestorePhysicalBackupResult EngineRestorePhysicalBackup(const EngineRestorePhysicalBackupRequest& request);

struct EnginePackageDeltaStreamRequest : EngineApiRequest {};
struct EnginePackageDeltaStreamResult : EngineApiResult {
  EngineUuid delta_uuid;
  EngineApiU64 start_transaction_id = 0;
  EngineApiU64 end_transaction_id = 0;
  EngineApiU64 row_count = 0;
  EngineApiU64 table_count = 0;
  std::string delta_manifest_uri;
};
EnginePackageDeltaStreamResult EnginePackageDeltaStream(const EnginePackageDeltaStreamRequest& request);

struct EngineApplyDeltaStreamRequest : EngineApiRequest {};
struct EngineApplyDeltaStreamResult : EngineApiResult {
  EngineUuid apply_uuid;
  EngineUuid delta_uuid;
  EngineApiU64 pitr_target_transaction_id = 0;
  EngineApiU64 applied_row_count = 0;
  EngineApiU64 applied_table_count = 0;
  std::string pitr_restore_point_name;
  std::string delta_manifest_uri;
};
EngineApplyDeltaStreamResult EngineApplyDeltaStream(const EngineApplyDeltaStreamRequest& request);

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_BACKUP_FORWARD_SESSION
struct EngineStartBackupForwardSessionRequest : EngineApiRequest {
  EngineUuid base_backup_uuid;
  EngineApiU64 base_snapshot_visible_through_local_transaction_id = 0;
  std::string source_manifest_uri;
  std::string filespace_uuid;
  std::string timeline_uuid;
  std::string fork_uuid;
  bool write_after_requested = true;
};

struct EngineStartBackupForwardSessionResult : EngineApiResult {
  EngineUuid session_uuid;
  EngineUuid base_backup_uuid;
  EngineApiU64 selected_start_transaction_id = 0;
  std::string source_manifest_uri;
  std::string filespace_uuid;
  std::string timeline_uuid;
  std::string fork_uuid;
  std::string ledger_uri;
  bool write_after_requested = false;
  bool write_after_recovery_authority = false;
  bool transaction_finality_authority = false;
};

EngineStartBackupForwardSessionResult EngineStartBackupForwardSession(
    const EngineStartBackupForwardSessionRequest& request);

struct EngineFinishBackupForwardSessionRequest : EngineApiRequest {
  EngineUuid session_uuid;
  EngineUuid base_backup_uuid;
  EngineApiU64 selected_start_transaction_id = 0;
  EngineApiU64 finish_transaction_id = 0;
  EngineApiU64 expected_previous_end_transaction_id = 0;
  std::string source_manifest_uri;
  std::string delta_manifest_uri;
  std::string filespace_uuid;
  std::string timeline_uuid;
  std::string fork_uuid;
  bool write_after_requested = true;
};

struct EngineFinishBackupForwardSessionResult : EngineApiResult {
  EngineUuid session_uuid;
  EngineUuid base_backup_uuid;
  EngineUuid delta_uuid;
  EngineApiU64 selected_start_transaction_id = 0;
  EngineApiU64 finish_transaction_id = 0;
  EngineApiU64 packaged_row_count = 0;
  EngineApiU64 packaged_table_count = 0;
  std::string source_manifest_uri;
  std::string delta_manifest_uri;
  std::string filespace_uuid;
  std::string timeline_uuid;
  std::string fork_uuid;
  std::string ledger_uri;
  bool coverage_contiguous = false;
  bool write_after_segment_immutable = false;
  bool write_after_recovery_authority = false;
  bool cluster_recovery_authority = false;
  bool transaction_finality_authority = false;
};

EngineFinishBackupForwardSessionResult EngineFinishBackupForwardSession(
    const EngineFinishBackupForwardSessionRequest& request);

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_MULTI_HORIZON_DISPOSAL_GUARD
struct EngineEvaluateHistoryDisposalMultiHorizonRequest : EngineApiRequest {
  EngineApiU64 disposable_start_transaction_id = 0;
  EngineApiU64 disposable_end_transaction_id = 0;
  std::string filespace_uuid;
  std::string archive_manifest_uri;
  std::string write_after_segment_uri;
  bool physical_reclaim_requested = true;
  bool archive_deletion_requested = true;
  bool write_after_truncation_requested = true;

  bool reader_horizon_authoritative = false;
  EngineApiU64 reader_horizon_transaction_id = 0;
  bool writer_horizon_authoritative = false;
  EngineApiU64 writer_horizon_transaction_id = 0;
  bool parser_snapshot_horizon_authoritative = false;
  EngineApiU64 parser_snapshot_horizon_transaction_id = 0;
  bool backup_forward_horizon_authoritative = false;
  EngineApiU64 backup_forward_horizon_transaction_id = 0;
  bool archive_horizon_authoritative = false;
  EngineApiU64 archive_horizon_transaction_id = 0;
  bool legal_hold_horizon_authoritative = false;
  EngineApiU64 legal_hold_horizon_transaction_id = 0;
  bool legal_hold_active = false;
  bool detached_filespace_horizon_authoritative = false;
  EngineApiU64 detached_filespace_horizon_transaction_id = 0;
  bool detached_filespace_hold_active = false;
  bool stable_checkpoint_horizon_authoritative = false;
  EngineApiU64 stable_checkpoint_horizon_transaction_id = 0;
  bool local_durable_horizon_authoritative = false;
  EngineApiU64 local_durable_horizon_transaction_id = 0;
  bool restore_reachability_horizon_authoritative = false;
  EngineApiU64 restore_reachability_horizon_transaction_id = 0;
};

struct EngineEvaluateHistoryDisposalMultiHorizonResult : EngineApiResult {
  EngineUuid decision_uuid;
  EngineApiU64 disposable_start_transaction_id = 0;
  EngineApiU64 disposable_end_transaction_id = 0;
  std::string filespace_uuid;
  std::string archive_manifest_uri;
  std::string write_after_segment_uri;
  std::string blocking_horizon_kind;
  EngineApiU64 blocking_horizon_transaction_id = 0;
  bool disposal_authorized = false;
  bool physical_reclaim_authorized = false;
  bool archive_deletion_authorized = false;
  bool write_after_truncation_authorized = false;
  bool fail_closed = true;
  bool mutation_performed = false;
  bool transaction_finality_authority = false;
  bool write_after_recovery_authority = false;
  bool cluster_recovery_authority = false;
};

EngineEvaluateHistoryDisposalMultiHorizonResult
EngineEvaluateHistoryDisposalMultiHorizon(
    const EngineEvaluateHistoryDisposalMultiHorizonRequest& request);

// SEARCH_KEY: CLUSTER_CATALOG_BACKUP_RESTORE
enum class ClusterCatalogTransferOperation {
  backup,
  restore,
  attach,
  import,
  public_export
};

enum class ClusterCatalogIdentityDisposition {
  preserve,
  remap,
  reject,
  quarantine
};

struct ClusterCatalogIdentityTransferRule {
  std::string table_path;
  std::string source_record_uuid;
  std::string target_record_uuid;
  ClusterCatalogIdentityDisposition disposition =
      ClusterCatalogIdentityDisposition::preserve;
  bool explicit_remap_authorized = false;
  bool no_uuid_reuse_proven = false;
  bool resolver_evidence_proven = false;
  bool comment_evidence_proven = false;
  bool security_binding_proven = false;
  bool projection_integrity_proven = false;
  bool provider_digest_verified = false;
  std::string disposition_reason;
};

struct ClusterCatalogIdentityTransferDecision {
  std::string table_path;
  std::string source_record_uuid;
  std::string target_record_uuid;
  ClusterCatalogIdentityDisposition disposition =
      ClusterCatalogIdentityDisposition::preserve;
  bool accepted = false;
  bool mutation_performed = false;
  bool identity_preserved = false;
  bool identity_remapped = false;
  bool identity_rejected = false;
  bool identity_quarantined = false;
  std::string diagnostic_code;
};

struct EngineEvaluateClusterCatalogBackupRestoreIdentityRequest
    : EngineApiRequest {
  ClusterCatalogTransferOperation transfer_operation =
      ClusterCatalogTransferOperation::backup;
  scratchbird::core::catalog::ClusterCatalogRecordSet record_set;
  std::vector<ClusterCatalogIdentityTransferRule> identity_rules;
  bool cluster_catalog_present = false;
  bool joined_cluster_catalog_state = false;
  bool external_provider_available = false;
  bool public_export_manifest_clean = true;
};

struct EngineEvaluateClusterCatalogBackupRestoreIdentityResult
    : EngineApiResult {
  ClusterCatalogTransferOperation transfer_operation =
      ClusterCatalogTransferOperation::backup;
  EngineApiU64 preserved_count = 0;
  EngineApiU64 remapped_count = 0;
  EngineApiU64 rejected_count = 0;
  EngineApiU64 quarantined_count = 0;
  bool fail_closed = true;
  bool mutation_performed = false;
  bool local_runtime_execution_enabled = false;
  bool cluster_recovery_authority = false;
  bool resolver_comment_security_projection_proven = false;
  std::vector<ClusterCatalogIdentityTransferDecision> decisions;
};

const char* ClusterCatalogTransferOperationName(
    ClusterCatalogTransferOperation operation);
const char* ClusterCatalogIdentityDispositionName(
    ClusterCatalogIdentityDisposition disposition);
EngineEvaluateClusterCatalogBackupRestoreIdentityResult
EngineEvaluateClusterCatalogBackupRestoreIdentityPolicy(
    const EngineEvaluateClusterCatalogBackupRestoreIdentityRequest& request);

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_BACKUP_UPDATE_VERIFIED_COVERAGE
struct EngineUpdateBackupFromVerifiedCoverageRequest : EngineApiRequest {
  EngineUuid backup_uuid;
  EngineApiU64 last_covered_transaction_id = 0;
  EngineApiU64 target_end_transaction_id = 0;
  std::string base_manifest_uri;
  std::string update_manifest_uri;
  std::string filespace_uuid;
  std::vector<std::string> verified_segment_manifest_uris;
  std::string idempotency_key;
};

struct EngineUpdateBackupFromVerifiedCoverageResult : EngineApiResult {
  EngineUuid update_uuid;
  EngineUuid backup_uuid;
  EngineApiU64 base_coverage_end_transaction_id = 0;
  EngineApiU64 reused_coverage_end_transaction_id = 0;
  EngineApiU64 target_end_transaction_id = 0;
  EngineApiU64 packaged_start_transaction_id = 0;
  EngineApiU64 packaged_end_transaction_id = 0;
  EngineApiU64 reused_segment_count = 0;
  EngineApiU64 packaged_row_count = 0;
  EngineApiU64 packaged_table_count = 0;
  std::string base_manifest_uri;
  std::string update_manifest_uri;
  bool historical_coverage_reused = false;
  bool idempotent_noop = false;
  bool coverage_contiguous = false;
  bool gap_detected = false;
  bool overlap_detected = false;
  bool transaction_finality_authority = false;
  bool write_after_recovery_authority = false;
  bool cluster_recovery_authority = false;
};

EngineUpdateBackupFromVerifiedCoverageResult
EngineUpdateBackupFromVerifiedCoverage(
    const EngineUpdateBackupFromVerifiedCoverageRequest& request);

struct EngineRestoreLogicalBackupRequest : EngineApiRequest {};
struct EngineRestoreLogicalBackupResult : EngineApiResult {
  EngineUuid restore_uuid;
  EngineUuid source_backup_uuid;
  EngineApiU64 restored_table_count = 0;
  EngineApiU64 restored_row_count = 0;
  EngineApiU64 restored_index_count = 0;
  std::string source_manifest_uri;
};
EngineRestoreLogicalBackupResult EngineRestoreLogicalBackup(const EngineRestoreLogicalBackupRequest& request);

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_ARCHIVE_BEFORE_RECLAIM
struct EngineArchiveRetainedHistoryRecord {
  scratchbird::transaction::mga::RowVersionMetadata metadata;
  std::string table_uuid;
  std::string payload_digest;
  std::string retention_class;
  std::string retention_policy_ref;
  std::string key_lineage_id;
};

struct EngineArchiveRetainedHistoryBeforeReclaimRequest : EngineApiRequest {
  scratchbird::storage::filespace::FilespaceDescriptor archive_filespace;
  std::vector<EngineArchiveRetainedHistoryRecord> retained_history;
  EngineApiU64 authoritative_cleanup_horizon_local_transaction_id = 0;
  bool engine_mga_authoritative = false;
  bool cleanup_horizon_authoritative = false;
  bool local_archive_filespace_header_verified = false;
  bool retention_policy_installed = false;
  bool legal_hold_active = false;
  bool manifest_reachability_required = true;
  EngineApiU64 max_row_versions_to_archive = 0;
};

struct EngineArchiveRetainedHistoryBeforeReclaimResult : EngineApiResult {
  EngineUuid archive_uuid;
  EngineUuid archive_filespace_uuid;
  EngineApiU64 archived_row_version_count = 0;
  EngineApiU64 movement_record_count = 0;
  EngineApiU64 manifest_checksum = 0;
  EngineApiU64 manifest_bytes = 0;
  std::string manifest_uri;
  bool local_archive_filespace_bound = false;
  bool manifest_written = false;
  bool manifest_verified = false;
  bool hot_reclaim_authorized = false;
  bool cluster_route_refused = false;
  bool transaction_finality_authority = false;
  std::vector<scratchbird::transaction::mga::LocalCleanupReclaimEvidenceRecord>
      reclaim_evidence_records;
};

EngineArchiveRetainedHistoryBeforeReclaimResult
EngineArchiveRetainedHistoryBeforeReclaim(
    const EngineArchiveRetainedHistoryBeforeReclaimRequest& request);

}  // namespace scratchbird::engine::internal_api
