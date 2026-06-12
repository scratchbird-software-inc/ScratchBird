// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "filespace_discovery.hpp"
#include "filespace_lifecycle.hpp"
#include "filespace_package.hpp"
#include "page_header.hpp"

#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_STORAGE_MANAGEMENT_API
struct EngineStorageManagementRequest : EngineApiRequest {};
struct EngineStorageManagementResult : EngineApiResult {};

EngineStorageManagementResult EngineStorageManagementOperation(
    const EngineStorageManagementRequest& request);

struct EngineFilespacePreallocateRequest : EngineApiRequest {};
struct EngineFilespacePreallocateResult : EngineApiResult {};

EngineFilespacePreallocateResult EngineFilespacePreallocate(
    const EngineFilespacePreallocateRequest& request);

struct EngineFilespaceLifecycleRequest : EngineApiRequest {};
struct EngineFilespaceLifecycleResult : EngineApiResult {};

EngineFilespaceLifecycleResult EngineFilespaceLifecycleOperation(
    const EngineFilespaceLifecycleRequest& request);

enum class EngineFilespaceDiscoveryScope {
  all,
  orphan_only,
  stale_only
};

struct EngineFilespaceDiscoveryRequest : EngineStorageManagementRequest {
  EngineFilespaceDiscoveryScope discovery_scope = EngineFilespaceDiscoveryScope::all;
  std::vector<scratchbird::storage::filespace::FilespaceDescriptor> expected_filespaces;
  std::vector<std::string> runtime_scan_paths;
  bool runtime_filesystem_scan_requested = false;
  bool parser_filesystem_authority = false;
  bool parser_storage_authority = false;
  bool transaction_finality_authority = false;
  bool recovery_authority = false;
  bool reference_or_wal_recovery_authority = false;
  bool mutation_requested = false;
  bool execute_quarantine_actions = false;
  bool execute_release_actions = false;
  bool execute_physical_cleanup_actions = false;
  bool physical_header_required_for_quarantine = true;
  bool header_inspection_passed = false;
  bool release_authorized = false;
  bool allow_physical_filespace_delete = false;
  bool physical_delete_retention_satisfied = false;
  bool physical_delete_legal_hold_clear = false;
  bool physical_delete_cleanup_horizon_authoritative = false;
  std::string inspector_uuid;
  std::string release_authority_uuid;
};

struct EngineFilespaceDiscoveryResult : EngineStorageManagementResult {
  EngineFilespaceDiscoveryScope discovery_scope = EngineFilespaceDiscoveryScope::all;
  EngineApiU64 anomaly_count = 0;
  EngineApiU64 quarantine_execution_count = 0;
  EngineApiU64 release_execution_count = 0;
  EngineApiU64 physical_cleanup_execution_count = 0;
  bool quarantine_required = false;
  bool operator_review_required = false;
  bool durable_state_changed = false;
  bool cache_invalidation_required = false;
  bool cleanup_or_quarantine_executed = false;
  bool release_executed = false;
  bool physical_cleanup_executed = false;
  bool physical_file_removed = false;
  bool runtime_filesystem_scan_executed = false;
  bool parser_filesystem_authority = false;
  bool parser_storage_authority = false;
  bool transaction_finality_authority = false;
  bool recovery_authority = false;
  bool reference_or_wal_recovery_authority = false;
  bool private_provider_dispatch = false;
};

const char* EngineFilespaceDiscoveryScopeName(EngineFilespaceDiscoveryScope scope);

EngineFilespaceDiscoveryResult EngineDiscoverFilespaceAnomalies(
    const EngineFilespaceDiscoveryRequest& request);

enum class EngineFilespacePackageAction {
  export_manifest,
  inspect_manifest,
  import_to_quarantine,
  admit,
  reject
};

struct EngineFilespacePackageRequest : EngineStorageManagementRequest {
  EngineFilespacePackageAction package_operation =
      EngineFilespacePackageAction::inspect_manifest;
  std::string package_file_path;
  std::string physical_package_transfer_directory;
  bool runtime_package_file_io_requested = false;
  bool package_file_write_requested = false;
  bool package_file_read_requested = false;
  bool package_file_allow_overwrite = false;
  bool runtime_physical_package_transfer_requested = false;
  bool allow_physical_package_transfer = false;
  bool parser_file_io_authority = false;
  bool parser_storage_authority = false;
  bool transaction_finality_authority = false;
  bool recovery_authority = false;
  bool reference_or_wal_recovery_authority = false;
  bool private_provider_dispatch_requested = false;
};

struct EngineFilespacePackageResult : EngineStorageManagementResult {
  EngineFilespacePackageAction package_operation =
      EngineFilespacePackageAction::inspect_manifest;
  EngineApiU64 member_count = 0;
  EngineApiU64 staged_count = 0;
  EngineApiU64 admitted_count = 0;
  EngineApiU64 rejected_count = 0;
  EngineApiU64 physical_package_member_count = 0;
  EngineApiU64 physical_package_byte_count = 0;
  bool durable_state_changed = false;
  bool cache_invalidation_required = false;
  bool runtime_package_file_io_executed = false;
  bool physical_package_transfer_executed = false;
  bool parser_file_io_authority = false;
  bool parser_storage_authority = false;
  bool transaction_finality_authority = false;
  bool recovery_authority = false;
  bool reference_or_wal_recovery_authority = false;
  bool private_provider_dispatch = false;
};

const char* EngineFilespacePackageOperationName(
    EngineFilespacePackageAction operation);

EngineFilespacePackageResult EngineFilespacePackageOperation(
    const EngineFilespacePackageRequest& request);

enum class EngineStorageTierClass {
  unknown,
  hot,
  warm,
  cold,
  archive,
  nvme,
  ssd,
  hdd,
  custom
};

enum class EngineStorageTierMigrationOperation {
  inspect,
  validate,
  plan_migration,
  stage_migration,
  commit_migration,
  rollback_migration
};

struct EngineStorageTierMigrationDescriptor {
  EngineUuid storage_tier_policy_uuid;
  EngineUuid source_tier_uuid;
  EngineUuid target_tier_uuid;
  std::string source_physical_path;
  std::string target_physical_path;
  EngineStorageTierClass source_tier_class = EngineStorageTierClass::unknown;
  EngineStorageTierClass target_tier_class = EngineStorageTierClass::unknown;
  scratchbird::storage::filespace::FilespaceRole target_filespace_role =
      scratchbird::storage::filespace::FilespaceRole::unknown;
  std::vector<scratchbird::storage::disk::PageType> page_types;
  EngineApiU64 expected_catalog_generation = 0;
  EngineApiU64 observed_catalog_generation = 0;
  EngineApiU64 expected_policy_generation = 0;
  EngineApiU64 observed_policy_generation = 0;
  bool storage_tier_policy_resolved = false;
  bool filespace_role_known = false;
  bool page_family_eligibility_validated = false;
  bool typed_dependency_manifest_validated = false;
  bool cluster_scoped = false;
  bool physical_data_movement_requested = false;
  bool physical_data_movement_authorized = false;
  bool physical_rewrite_plan_validated = false;
  bool backup_export_repair_profile_validated = false;
  bool allow_physical_target_overwrite = false;
};

struct EngineStorageTierMigrationRequest : EngineStorageManagementRequest {
  EngineStorageTierMigrationOperation tier_operation =
      EngineStorageTierMigrationOperation::inspect;
  EngineStorageTierMigrationDescriptor descriptor;
};

struct EngineStorageTierMigrationResult : EngineStorageManagementResult {
  EngineStorageTierMigrationDescriptor descriptor;
  bool durable_state_changed = false;
  bool physical_data_movement_dispatched = false;
  bool physical_digest_verified = false;
  EngineApiU64 physical_data_movement_bytes = 0;
  bool parser_storage_authority = false;
  bool private_provider_dispatch = false;
  bool cache_invalidation_required = false;
};

const char* EngineStorageTierClassName(EngineStorageTierClass tier_class);
const char* EngineStorageTierMigrationOperationName(
    EngineStorageTierMigrationOperation operation);

EngineStorageTierMigrationResult EnginePlanStorageTierMigrationOperation(
    const EngineStorageTierMigrationRequest& request);

}  // namespace scratchbird::engine::internal_api
