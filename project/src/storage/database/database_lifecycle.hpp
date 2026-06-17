// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-DB-LIFECYCLE-ANCHOR
// SEARCH_KEY: CONFIG_POLICY_MIGRATION
#include "database_format.hpp"
#include "disk_device.hpp"
#include "agent_engine_lifecycle.hpp"
#include "memory.hpp"
#include "page_cache.hpp"
#include "resource_seed_pack.hpp"
#include "runtime_platform.hpp"
#include "startup_state.hpp"
#include "transaction_horizon.hpp"
#include "transaction_inventory.hpp"
#include "uuid.hpp"

#include <string>
#include <vector>

namespace scratchbird::storage::database {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;
using scratchbird::storage::disk::DatabaseHeader;

inline constexpr u64 kDatabaseHeaderPageNumber = 0;

struct DatabaseCreateConfig {
  std::string path;
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  u32 page_size = static_cast<u32>(scratchbird::storage::disk::PageSizeProfile::profile_16k);
  u64 creation_unix_epoch_millis = 0;
  u64 feature_flags = 0;
  u64 compatibility_flags = scratchbird::storage::disk::DatabaseCompatibilityFlag::public_node_safe_header_open |
                            scratchbird::storage::disk::DatabaseCompatibilityFlag::unknown_page_safe_classification_required;
  std::string resource_seed_pack_root;
  bool require_resource_seed_pack = true;
  bool allow_minimal_resource_bootstrap = false;
  std::string policy_seed_pack_root;
  bool require_policy_seed_pack = false;
  bool allow_overwrite = false;
};

struct PolicySeedPackDescriptor {
  std::string policy_pack_id;
  std::string policy_pack_uuid;
  std::string policy_pack_version;
  std::string manifest_relative_path;
  std::string content_sha256;
  bool create_time_only = true;
  bool post_create_filesystem_authority = false;
  bool local_password_only = true;
};

struct PolicySeedPackCatalogImage {
  bool active = false;
  std::string policy_pack_id;
  std::string policy_pack_uuid;
  std::string policy_pack_version;
  std::string content_sha256;
  std::string signature_status;
  u32 schema_version = 0;
  u32 policy_generation = 0;
  bool create_time_only = true;
  bool post_create_filesystem_authority = false;
  bool local_password_only = true;
  bool materialized_inside_create_transaction = false;
  bool requires_mga_catalog_commit = false;
  u32 security_provider_records = 0;
  u32 enabled_local_password_provider_records = 0;
  u32 external_provider_disabled_records = 0;
  u32 role_records = 0;
  u32 group_records = 0;
  u32 group_membership_records = 0;
  u32 grant_records = 0;
  u32 policy_profile_records = 0;
  u32 default_policy_records = 0;
  std::vector<std::string> policy_profile_areas;
};

struct DatabaseOpenConfig {
  std::string path;
  bool cluster_authority_available = false;
  bool decryption_available = false;
  bool read_only = false;
  bool suppress_background_agents = false;
  std::string migration_plan_id;
  std::string expected_resource_seed_pack_name;
  std::string expected_resource_seed_pack_version;
  std::string expected_resource_seed_pack_content_hash;
};

enum class DatabaseLifecyclePhase : u16 {
  none,
  created,
  opened,
  closed,
  maintenance,
  restricted_open,
  inspected,
  verified,
  repaired,
  dropped,
  quarantined,
  failed
};

enum class DatabaseOpenCompatibilityClass : u16 {
  current,
  supported_migration,
  upgrade_required,
  read_only_compatible,
  unsupported,
  unsupported_old,
  unsupported_new,
  downgrade_refused,
  newer_than_supported_refused,
  ambiguous_identity_refused,
  missing_migration_plan_refused,
  migration_required_without_plan_refused
};

struct DatabaseArtifactVersionCompatibilityRequest {
  std::string artifact_kind;
  u32 format_major = 0;
  u32 format_minor = 0;
  u32 min_supported_major = 0;
  u32 min_supported_minor = 0;
  u32 current_major = 0;
  u32 current_minor = 0;
  u32 max_supported_major = 0;
  u32 max_supported_minor = 0;
  bool identity_proven = true;
  bool downgrade_requested = false;
  bool migration_plan_required = false;
  std::string migration_plan_id;
};

struct DatabaseArtifactCompatibilityResult {
  Status status;
  DatabaseOpenCompatibilityClass compatibility_class =
      DatabaseOpenCompatibilityClass::unsupported;
  bool migration_required = false;
  bool read_only_compatible = false;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct DatabaseCatalogMigrationEvidence {
  u32 database_catalog_record_count = 1;
  u32 active_primary_filespace_record_count = 1;
  bool database_uuid_matches_header = true;
  bool filespace_uuid_matches_startup = true;
  u32 database_catalog_manifest_format_version = 1;
  u32 filespace_catalog_manifest_format_version = 1;
  u32 filespace_resource_seed_manifest_format_version = 1;
  u32 resource_seed_manifest_format_version = 1;
  std::string migration_plan_id;
};

struct ClusterCatalogCompatibilityEvidenceEntry {
  std::string table_path;
  u32 schema_version = 1;
  u32 codec_version = 1;
  bool identity_proven = true;
  bool downgrade_requested = false;
};

struct ClusterCatalogCompatibilityEvidence {
  bool cluster_structures_present = false;
  bool external_provider_available = false;
  std::string migration_plan_id;
  std::vector<ClusterCatalogCompatibilityEvidenceEntry> entries;
};

struct DatabaseLifecycleState {
  std::string path;
  DatabaseHeader header;
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  DatabaseLifecyclePhase phase = DatabaseLifecyclePhase::none;
  bool cluster_structures_present = false;
  bool cluster_authority_active = false;
  bool decryption_available = false;
  bool safe_unknown_page_handling_required = true;
  bool read_only_open = false;
  DatabaseOpenCompatibilityClass database_open_compatibility_class =
      DatabaseOpenCompatibilityClass::unsupported;
  scratchbird::storage::disk::DiskHealthSnapshot disk_health;
  bool storage_partial_success_possible = false;
  std::string storage_partial_success_phase;
  std::string storage_failure_diagnostic_code;
  bool resource_seed_catalog_present = false;
  scratchbird::core::resources::ResourceSeedCatalogImage resource_seed_catalog;
  bool policy_seed_catalog_present = false;
  PolicySeedPackCatalogImage policy_seed_catalog;
  bool typed_catalog_records_present = false;
  u32 typed_catalog_record_count = 0;
  std::vector<std::string> typed_catalog_record_kinds;
  scratchbird::core::memory::MemoryAccountingSnapshot memory_accounting;
  scratchbird::transaction::mga::LocalTransactionInventory local_transaction_inventory;
  scratchbird::transaction::mga::LocalTransactionHorizons local_transaction_horizons;
  bool local_transaction_inventory_present = false;
  StartupStateRecord startup_state;
  bool startup_state_present = false;
  bool write_admission_fenced = true;
  std::string startup_recovery_classification;
  std::string startup_owner_token;
  bool engine_agent_health_present = false;
  scratchbird::core::agents::DatabaseEngineAgentHealthPublication engine_agent_health;
  std::string engine_agent_health_json;
  bool cache_checkpoint_present = false;
  scratchbird::storage::page::PageCacheCheckpointPublication cache_checkpoint;
  std::string cache_checkpoint_json;
};

struct DatabaseLifecycleResult {
  Status status;
  DatabaseLifecycleState state;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct DatabaseLifecycleOperationConfig {
  std::string path;
  bool cluster_authority_available = false;
  bool decryption_available = false;
  std::string operation_uuid;
  std::string actor_uuid;
  bool write_evidence = true;
};

struct DatabaseLifecycleRepairConfig {
  std::string path;
  bool cluster_authority_available = false;
  bool decryption_available = false;
  std::string operation_uuid;
  std::string actor_uuid;
  std::string repair_plan_id;
  std::string expected_database_uuid;
  std::string expected_filespace_uuid;
  bool repair_admission_proven = false;
  bool allow_mutation = false;
};

struct DatabaseDropConfig {
  std::string path;
  bool cluster_authority_available = false;
  bool decryption_available = false;
  std::string operation_uuid;
  std::string actor_uuid;
  std::string drop_mode = "logical";
  std::string expected_database_uuid;
  std::string expected_filespace_uuid;
  bool drop_safety_preconditions = false;
  bool session_drain_complete = false;
  bool ownership_release_verified = false;
  bool retention_policy_satisfied = false;
  bool backup_coverage_verified = false;
  bool legal_hold_clear = false;
  bool allow_physical_delete = false;
  bool allow_quarantine = false;
};

const char* DatabaseLifecyclePhaseName(DatabaseLifecyclePhase phase);
const char* DatabaseOpenCompatibilityClassName(DatabaseOpenCompatibilityClass compatibility_class);
DatabaseArtifactCompatibilityResult ClassifyDatabaseArtifactVersionCompatibility(
    const DatabaseArtifactVersionCompatibilityRequest& request);
DatabaseArtifactCompatibilityResult ClassifyDatabaseCatalogMigrationEvidence(
    const DatabaseCatalogMigrationEvidence& evidence);
DatabaseArtifactCompatibilityResult CheckClusterCatalogCompatibilityEvidence(
    const ClusterCatalogCompatibilityEvidence& evidence);
DatabaseOpenCompatibilityClass ClassifyDatabaseOpenCompatibility(
    const DatabaseHeader& header,
    bool read_only_requested,
    const std::string& migration_plan_id = {});
PolicySeedPackDescriptor DefaultPolicyPackDescriptor();
DatabaseLifecycleResult CreateDatabaseFile(const DatabaseCreateConfig& config);
DatabaseLifecycleResult OpenDatabaseFile(const DatabaseOpenConfig& config);
DatabaseLifecycleResult EnterDatabaseMaintenanceMode(const DatabaseLifecycleOperationConfig& config);
DatabaseLifecycleResult ExitDatabaseMaintenanceMode(const DatabaseLifecycleOperationConfig& config);
DatabaseLifecycleResult EnterDatabaseRestrictedOpenMode(const DatabaseLifecycleOperationConfig& config);
DatabaseLifecycleResult ExitDatabaseRestrictedOpenMode(const DatabaseLifecycleOperationConfig& config);
DatabaseLifecycleResult InspectDatabaseLifecycle(const DatabaseLifecycleOperationConfig& config);
DatabaseLifecycleResult VerifyDatabaseLifecycle(const DatabaseLifecycleOperationConfig& config);
DatabaseLifecycleResult RepairDatabaseLifecycle(const DatabaseLifecycleRepairConfig& config);
DatabaseLifecycleResult DropDatabaseLifecycle(const DatabaseDropConfig& config);
StartupWriteResult MarkDatabaseCleanShutdown(const std::string& path);
DiagnosticRecord MakeDatabaseLifecycleDiagnostic(Status status,
                                                std::string diagnostic_code,
                                                std::string message_key,
                                                std::string path = {},
                                                std::string detail = {});

}  // namespace scratchbird::storage::database
