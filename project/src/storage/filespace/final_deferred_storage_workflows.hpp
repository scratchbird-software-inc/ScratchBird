// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace scratchbird::storage::filespace {

// MDF-001-FINAL-DEFERRED-FILESPACE-RELOCATION
// MDF-002-FINAL-DEFERRED-CORE-PAGE-BODY-RELOCATION
// MDF-003-FINAL-DEFERRED-SNAPSHOT-SHADOW-PHYSICAL
// MDF-004-FINAL-DEFERRED-PHYSICAL-REENCRYPTION
// MDF-005-FINAL-DEFERRED-REPAIR-REBUILD-SALVAGE-BODIES
// MDF-006-FINAL-DEFERRED-DURABLE-REPAIR-SWEEP
// MDF-007-FINAL-DEFERRED-FILESPACE-PACKAGE-COMPLETION
// MDF-008-FINAL-DEFERRED-SHARD-PLACEMENT-EXECUTION
// MDF-010-FINAL-DEFERRED-STORAGE-TIER-EXECUTION

struct FinalDeferredWorkflowResult {
  bool ok = false;
  std::string diagnostic_code;
  std::uint64_t bytes_processed = 0;
  std::uint64_t source_digest = 0;
  std::uint64_t target_digest = 0;
  bool digest_verified = false;
  bool durable_state_changed = false;
  bool recovery_required = false;
  bool cache_invalidated = false;
  std::vector<std::string> evidence;
};

struct FilespaceRelocationRequest {
  std::filesystem::path source_path;
  std::filesystem::path target_path;
  std::filesystem::path journal_path;
  std::uint64_t expected_digest = 0;
  std::uint32_t active_pin_count = 0;
  bool mga_transaction_authority = true;
  bool legal_hold = false;
  bool retention_released = true;
  bool simulate_interruption_after_stage = false;
};

struct CorePageBody {
  std::uint64_t page_id = 0;
  std::vector<std::uint8_t> body;
  std::uint64_t expected_digest = 0;
};

struct CorePageRelocationRequest {
  std::vector<CorePageBody> pages;
  std::filesystem::path target_directory;
  bool root_authority_current = true;
  bool mga_transaction_authority = true;
  bool simulate_interruption = false;
};

struct SnapshotShadowPackageRequest {
  std::vector<std::filesystem::path> source_files;
  std::filesystem::path package_directory;
  std::string provider_name;
  std::string kms_manifest_id;
  bool provider_bound = true;
  bool kms_manifest_admitted = true;
  bool checkpoint_complete = true;
  bool restore_publication_fenced = true;
  bool parser_recovery_authority_claim = false;
};

struct PhysicalReencryptionRequest {
  std::filesystem::path source_path;
  std::filesystem::path target_path;
  std::uint64_t old_key_id = 0;
  std::uint64_t new_key_id = 0;
  bool protected_material_unsealed = true;
  bool legal_hold = false;
  bool retention_released = true;
  bool allow_plaintext_diagnostic = false;
};

enum class RepairBodyOperation {
  repair,
  rebuild,
  salvage
};

struct RepairBodyPlan {
  RepairBodyOperation operation = RepairBodyOperation::repair;
  std::filesystem::path source_path;
  std::filesystem::path mirror_path;
  std::filesystem::path output_path;
  std::filesystem::path quarantine_path;
  std::uint64_t expected_source_digest = 0;
  bool mga_transaction_authority = true;
  bool require_quarantine_for_salvage = true;
};

struct DurableSweepAction {
  std::filesystem::path target_path;
  bool legal_hold = false;
  bool retention_released = true;
  bool reachable = false;
};

struct DurableSweepRequest {
  std::filesystem::path state_path;
  std::vector<DurableSweepAction> actions;
  bool persist_before_delete = true;
  bool mga_transaction_authority = true;
};

struct PackageTransferRequest {
  std::vector<std::filesystem::path> member_files;
  std::filesystem::path package_directory;
  std::filesystem::path restore_directory;
  bool encrypted_manifest_admitted = true;
  bool parser_restore_authority_claim = false;
};

struct ShardPlacementRequest {
  std::string operation;
  std::filesystem::path state_path;
  std::uint64_t local_transaction_number = 0;
  bool cluster_authority_available = true;
  bool provider_authorized = true;
  bool standalone_cluster_surrogate = false;
};

enum class StorageTierPhase {
  stage,
  commit,
  rollback
};

struct StorageTierMigrationRequest {
  std::filesystem::path source_path;
  std::filesystem::path target_path;
  std::filesystem::path journal_path;
  StorageTierPhase phase = StorageTierPhase::stage;
  bool durable_envelope = true;
  bool cache_epoch_current = true;
  bool inject_digest_mismatch = false;
};

std::uint64_t ComputeFinalDeferredBytesDigest(const std::vector<std::uint8_t>& bytes);
std::uint64_t ComputeFinalDeferredFileDigest(const std::filesystem::path& path);

FinalDeferredWorkflowResult RelocateFilespaceBytes(const FilespaceRelocationRequest& request);
FinalDeferredWorkflowResult RecoverFilespaceRelocation(const std::filesystem::path& journal_path);
FinalDeferredWorkflowResult RelocateCorePageBodies(const CorePageRelocationRequest& request);
FinalDeferredWorkflowResult BuildSnapshotShadowPackage(const SnapshotShadowPackageRequest& request);
FinalDeferredWorkflowResult RewritePhysicalEncryptionProfile(const PhysicalReencryptionRequest& request);
FinalDeferredWorkflowResult ExecuteRepairBodyPlan(const RepairBodyPlan& plan);
FinalDeferredWorkflowResult PersistAndRunDurableSweep(const DurableSweepRequest& request);
FinalDeferredWorkflowResult ResumeDurableSweep(const std::filesystem::path& state_path);
FinalDeferredWorkflowResult TransferEncryptedPackageMembers(const PackageTransferRequest& request);
FinalDeferredWorkflowResult ExecuteShardPlacementOperation(const ShardPlacementRequest& request);
FinalDeferredWorkflowResult ExecuteStorageTierOperation(const StorageTierMigrationRequest& request);

}  // namespace scratchbird::storage::filespace
