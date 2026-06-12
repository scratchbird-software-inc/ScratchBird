// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"

#include "bootstrap_schema_roots.hpp"
#include "catalog_record_codec.hpp"
#include "cluster_catalog_schema_versioning.hpp"
#include "agent_engine_lifecycle.hpp"
#include "catalog_page.hpp"
#include "database_dirty_manifest.hpp"
#include "datatype_descriptor.hpp"
#include "memory.hpp"
#include "metric_registry.hpp"
#include "metric_retention_policy.hpp"
#include "page_cache.hpp"
#include "page_manager.hpp"
#include "local_transaction_store.hpp"
#include "transaction_inventory_page.hpp"
#include "transaction_recovery.hpp"

#include <array>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <iterator>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

#include <openssl/sha.h>

namespace scratchbird::storage::database {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::u16;
using scratchbird::core::catalog::CatalogRecordKind;
using scratchbird::core::catalog::CatalogRecordKindName;
using scratchbird::core::catalog::CatalogTypedRecord;
using scratchbird::core::catalog::ClusterCatalogCompatibilityClass;
using scratchbird::core::catalog::ClusterCatalogCompatibilityRequest;
using scratchbird::core::catalog::DecodeCatalogTypedRecord;
using scratchbird::core::catalog::EncodeCatalogTypedRecord;
using scratchbird::core::catalog::EvaluateClusterCatalogCompatibility;
using scratchbird::core::catalog::kBootstrapCatalogTransactionId;
using scratchbird::core::catalog::kClusterUserHomePolicyRoot;
using scratchbird::core::catalog::kLocalEmulatedSchemaPath;
using scratchbird::core::catalog::kLocalRemoteSchemaPath;
using scratchbird::core::catalog::kLocalSysAgentsSchemaPath;
using scratchbird::core::catalog::kLocalSysAuditSchemaPath;
using scratchbird::core::catalog::kLocalSysCatalogReadableSchemaPath;
using scratchbird::core::catalog::kLocalSysCatalogSchemaPath;
using scratchbird::core::catalog::kLocalSysCompatibilitySchemaPath;
using scratchbird::core::catalog::kLocalSysConfigurationSchemaPath;
using scratchbird::core::catalog::kLocalSysDiagnosticsSchemaPath;
using scratchbird::core::catalog::kLocalSysFnSchemaPath;
using scratchbird::core::catalog::kLocalSysInformationTrueSchemaPath;
using scratchbird::core::catalog::kLocalSysInformationSchemaPath;
using scratchbird::core::catalog::kLocalSysManagementSchemaPath;
using scratchbird::core::catalog::kLocalSysMetricsSchemaPath;
using scratchbird::core::catalog::kLocalSysMgaSchemaPath;
using scratchbird::core::catalog::kLocalSysParserSchemaPath;
using scratchbird::core::catalog::kLocalSysSchemaPath;
using scratchbird::core::catalog::kLocalSysSecuritySchemaPath;
using scratchbird::core::catalog::kLocalSysStorageSchemaPath;
using scratchbird::core::catalog::kLocalSysUdrSchemaPath;
using scratchbird::core::catalog::kLocalUserHomePolicyName;
using scratchbird::core::catalog::kLocalUserHomePublicPath;
using scratchbird::core::catalog::kLocalUserHomePolicyRoot;
using scratchbird::core::catalog::kLocalUsersPublicSchemaPath;
using scratchbird::core::catalog::kLocalUsersSchemaPath;
using scratchbird::core::catalog::LookupCatalogRecordDescriptor;
using scratchbird::core::datatypes::BuiltinDatatypeDescriptors;
using scratchbird::core::datatypes::CanonicalTypeName;
using scratchbird::core::datatypes::TypeFamilyName;
using scratchbird::core::datatypes::TypeWidthClassName;
using scratchbird::core::metrics::BaselineMetricRetentionPolicies;
using scratchbird::core::metrics::DefaultMetricRegistry;
using scratchbird::core::metrics::DefaultMetricRetentionPolicyForDescriptor;
using scratchbird::core::metrics::MetricDescriptor;
using scratchbird::core::metrics::MetricReadinessName;
using scratchbird::core::metrics::MetricRetentionModeName;
using scratchbird::core::metrics::MetricRollupGrainName;
using scratchbird::core::metrics::MetricTypeName;
using scratchbird::core::metrics::MetricUnitName;
using scratchbird::core::metrics::MetricVisibilityScopeName;
using scratchbird::core::agents::AgentActivationProfileName;
using scratchbird::core::agents::AgentAuthorityClassName;
using scratchbird::core::agents::AgentCatalogRecordLayouts;
using scratchbird::core::agents::AgentDeploymentName;
using scratchbird::core::agents::AgentFeatureAvailability;
using scratchbird::core::agents::AgentFeatureAvailabilityName;
using scratchbird::core::agents::AgentLifecycleMode;
using scratchbird::core::agents::AgentLifecycleModeName;
using scratchbird::core::agents::AgentPersistenceUsesScratchBirdStorageAuthority;
using scratchbird::core::agents::AgentRuntimeContext;
using scratchbird::core::agents::BaselinePolicyForAgent;
using scratchbird::core::agents::CanonicalAgentRegistry;
using scratchbird::core::agents::DatabaseEngineAgentHealthPublication;
using scratchbird::core::agents::DatabaseEngineAgentInput;
using scratchbird::core::agents::DatabaseEngineAgentLifecycleState;
using scratchbird::core::agents::DeterministicAgentRuntimeObjectUuidFromKey;
using scratchbird::core::agents::EffectiveActivationForLifecycle;
using scratchbird::core::agents::EvaluateAgentFeatureAvailability;
using scratchbird::core::agents::SerializeDatabaseEngineAgentHealthJson;
using scratchbird::core::agents::StartDatabaseEngineLifecycleAgent;
using scratchbird::core::agents::StopDatabaseEngineLifecycleAgent;
using scratchbird::core::agents::ValidateCanonicalAgentRegistry;
using scratchbird::core::agents::ValidateAgentPolicy;
using scratchbird::core::memory::ConfigureDefaultMemoryManagerForFixture;
using scratchbird::core::memory::DefaultMemoryManager;
using scratchbird::core::memory::DefaultMemoryManagerState;
using scratchbird::core::memory::DefaultLocalEngineMemoryPolicy;
using scratchbird::core::memory::MemoryCategory;
using scratchbird::core::memory::MemoryLifetime;
using scratchbird::core::memory::MemoryTag;
using scratchbird::core::resources::LoadResourceSeedPack;
using scratchbird::core::resources::ResourceSeedAlias;
using scratchbird::core::resources::ResourceSeedArtifact;
using scratchbird::core::resources::ResourceSeedCatalogImage;
using scratchbird::core::resources::ResourceSeedFamily;
using scratchbird::core::resources::ResourceSeedFamilyName;
using scratchbird::core::resources::ResourceSeedLoadConfig;
using scratchbird::core::resources::ValidateResourceSeedCatalogImage;
using scratchbird::transaction::mga::ComputeLocalTransactionHorizons;
using scratchbird::transaction::mga::ApplyLocalTransactionInventoryRecovery;
using scratchbird::transaction::mga::BeginLocalTransaction;
using scratchbird::transaction::mga::ClassifyLocalTransactionInventoryForRecovery;
using scratchbird::transaction::mga::CommitLocalTransaction;
using scratchbird::transaction::mga::LocalTransactionHorizons;
using scratchbird::transaction::mga::LocalTransactionInventory;
using scratchbird::transaction::mga::LookupLocalTransaction;
using scratchbird::transaction::mga::MakeLocalTransactionId;
using scratchbird::transaction::mga::MakeEmptyLocalTransactionInventory;
using scratchbird::transaction::mga::TransactionState;
using scratchbird::core::uuid::GenerateEngineIdentityV7;
using scratchbird::core::uuid::IsEngineIdentityUuid;
using scratchbird::core::uuid::ParseTypedUuid;
using scratchbird::storage::disk::FileDevice;
using scratchbird::storage::disk::FileOpenMode;
using scratchbird::storage::disk::CheckDiskDeviceHealth;
using scratchbird::storage::disk::DiskAccessMode;
using scratchbird::storage::disk::DiskChecksumPolicy;
using scratchbird::storage::disk::DiskDevicePolicy;
using scratchbird::storage::disk::DiskFsyncPolicy;
using scratchbird::storage::disk::DiskHealthSnapshot;
using scratchbird::storage::disk::ReadDevicePageHeader;
using scratchbird::storage::disk::SyncFileDeviceWithPolicy;
using scratchbird::storage::disk::UnknownPagePolicy;
using scratchbird::storage::disk::kPageHeaderSerializedBytes;
using scratchbird::storage::disk::MakeDatabaseHeader;
using scratchbird::storage::disk::PageType;
using scratchbird::storage::disk::ParsePageHeader;
using scratchbird::storage::disk::ParseDatabaseHeader;
using scratchbird::storage::disk::SerializeDatabaseHeader;
using scratchbird::storage::disk::SerializedDatabaseHeader;
using scratchbird::storage::page::BuildManagedPageHeader;
using scratchbird::storage::page::BuildCatalogPageSet;
using scratchbird::storage::page::CatalogPageRow;
using scratchbird::storage::page::CatalogPageRowKind;
using scratchbird::storage::page::AllocateManagedPageBuffer;
using scratchbird::storage::page::CheckpointPageCacheLifecycle;
using scratchbird::storage::page::ParseCatalogPageBody;
using scratchbird::storage::page::ManagedPageHeaderRequest;
using scratchbird::storage::page::MarkPageCacheEntryDirty;
using scratchbird::storage::page::PageCacheCheckpointMode;
using scratchbird::storage::page::PageCacheCheckpointPublication;
using scratchbird::storage::page::PageCacheEntry;
using scratchbird::storage::page::PageCacheLedger;
using scratchbird::storage::page::PageCacheLifecycleInput;
using scratchbird::storage::page::PageCacheLifecycleState;
using scratchbird::storage::page::PageCachePolicy;
using scratchbird::storage::page::PageManagerContext;
using scratchbird::storage::page::CheckedPageBodyOffset;
using scratchbird::storage::page::CheckedPageOffset;
using scratchbird::storage::page::SerializePageCacheCheckpointJson;
using scratchbird::storage::page::ShutdownFlushPageCacheLifecycle;
using scratchbird::storage::page::StartPageCacheLifecycle;
using scratchbird::storage::page::BuildTransactionInventoryPageBody;
using scratchbird::storage::page::ParseTransactionInventoryPageBody;
using scratchbird::storage::page::TransactionInventoryPageBody;
using scratchbird::storage::database::LoadLocalTransactionInventoryFromOpenDevice;
using scratchbird::storage::database::PersistLocalTransactionInventoryToOpenDevice;

constexpr u64 kFirstOpenActivationLocalTransactionId = 2;
constexpr u32 kDatabaseCatalogManifestFormatCurrent = 1;
constexpr u32 kDatabaseCatalogManifestFormatMinSupported = 1;
constexpr u32 kDatabaseCatalogManifestFormatMaxSupported = 1;
constexpr u32 kResourceSeedManifestFormatCurrent = 1;
constexpr u32 kResourceSeedManifestFormatMinSupported = 1;
constexpr u32 kResourceSeedManifestFormatMaxSupported = 1;

Status DatabaseLifecycleOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::storage_disk};
}

Status DatabaseLifecycleErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::storage_disk};
}

StartupWriteResult StartupLifecycleOkStatus() {
  StartupWriteResult result;
  result.status = DatabaseLifecycleOkStatus();
  return result;
}

StartupWriteResult StartupLifecycleError(std::string diagnostic_code,
                                         std::string message_key,
                                         std::string path = {},
                                         std::string detail = {}) {
  StartupWriteResult result;
  result.status = DatabaseLifecycleErrorStatus();
  result.diagnostic = MakeDatabaseLifecycleDiagnostic(result.status,
                                                      std::move(diagnostic_code),
                                                      std::move(message_key),
                                                      std::move(path),
                                                      std::move(detail));
  return result;
}

StartupWriteResult StartupLifecyclePropagate(Status status, DiagnosticRecord diagnostic) {
  StartupWriteResult result;
  result.status = status;
  result.diagnostic = std::move(diagnostic);
  return result;
}

bool IsTypedEngineIdentity(const TypedUuid& uuid, UuidKind kind) {
  return uuid.kind == kind && uuid.valid() && IsEngineIdentityUuid(uuid.value);
}

u64 CurrentUnixEpochMillis() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

DatabaseLifecycleResult LifecycleError(std::string diagnostic_code,
                                       std::string message_key,
                                       std::string path = {},
                                       std::string detail = {}) {
  DatabaseLifecycleResult result;
  result.status = DatabaseLifecycleErrorStatus();
  result.state.phase = DatabaseLifecyclePhase::failed;
  result.diagnostic = MakeDatabaseLifecycleDiagnostic(result.status,
                                                      std::move(diagnostic_code),
                                                      std::move(message_key),
                                                      std::move(path),
                                                      std::move(detail));
  return result;
}

DatabaseLifecycleResult PropagateDiagnostic(Status status,
                                            DiagnosticRecord diagnostic,
                                            DatabaseLifecyclePhase phase = DatabaseLifecyclePhase::failed) {
  DatabaseLifecycleResult result;
  result.status = status;
  result.state.phase = phase;
  result.diagnostic = std::move(diagnostic);
  return result;
}

DatabaseLifecycleResult PropagateStorageFailure(Status status,
                                                DiagnosticRecord diagnostic,
                                                std::string path,
                                                std::string phase_name,
                                                bool partial_success_possible) {
  DatabaseLifecycleResult result;
  result.status = status;
  result.state.phase = DatabaseLifecyclePhase::failed;
  result.state.path = std::move(path);
  result.state.storage_partial_success_possible = partial_success_possible;
  result.state.storage_partial_success_phase = std::move(phase_name);
  result.state.storage_failure_diagnostic_code = diagnostic.diagnostic_code;
  result.diagnostic = std::move(diagnostic);
  return result;
}

DatabaseLifecycleResult MarkStoragePartial(DatabaseLifecycleResult failure,
                                           const std::string& path,
                                           const std::string& phase_name) {
  failure.state.phase = DatabaseLifecyclePhase::failed;
  failure.state.path = path;
  failure.state.storage_partial_success_possible = true;
  failure.state.storage_partial_success_phase = phase_name;
  if (failure.state.storage_failure_diagnostic_code.empty()) {
    failure.state.storage_failure_diagnostic_code = failure.diagnostic.diagnostic_code;
  }
  return failure;
}

DiskDevicePolicy LifecycleDiskPolicy(u32 page_size, bool read_only, bool require_size_alignment) {
  DiskDevicePolicy policy;
  policy.page_size = page_size;
  policy.access_mode = read_only ? DiskAccessMode::read_only : DiskAccessMode::read_write;
  policy.fsync_policy = read_only ? DiskFsyncPolicy::never : DiskFsyncPolicy::after_mutation;
  policy.checksum_policy = DiskChecksumPolicy::require_valid;
  policy.unknown_page_policy = UnknownPagePolicy::reject_all;
  policy.require_size_alignment = require_size_alignment;
  return policy;
}

bool ResourceSeedPackMismatch(const ResourceSeedCatalogImage& image,
                              const DatabaseOpenConfig& config,
                              std::string* detail) {
  auto mismatch = [detail](std::string field,
                           const std::string& actual,
                           const std::string& expected) {
    if (expected.empty() || actual == expected) {
      return false;
    }
    if (detail != nullptr && detail->empty()) {
      *detail = std::move(field) + "=" + actual + " expected=" + expected;
    }
    return true;
  };

  bool mismatched = false;
  mismatched |= mismatch("seed_pack_name",
                         image.seed_pack_name,
                         config.expected_resource_seed_pack_name);
  mismatched |= mismatch("seed_pack_version",
                         image.seed_pack_version,
                         config.expected_resource_seed_pack_version);
  mismatched |= mismatch("seed_pack_content_hash",
                         image.content_hash,
                         config.expected_resource_seed_pack_content_hash);
  return mismatched;
}

DatabaseLifecycleResult ValidateCorePageHeaders(FileDevice* device,
                                                u32 page_size,
                                                const DiskDevicePolicy& policy) {
  const std::array<std::pair<PageType, u64>, 5> expected = {{
      {PageType::system_state, kSystemStatePageNumber},
      {PageType::catalog, kCatalogPageNumber},
      {PageType::allocation_map, kAllocationMapPageNumber},
      {PageType::transaction_inventory, kTransactionInventoryPageNumber},
      {PageType::bootstrap_reserved, kBootstrapReservedPageNumber},
  }};

  for (const auto& entry : expected) {
    const auto page = ReadDevicePageHeader(device, page_size, entry.second, policy);
    if (!page.ok()) {
      return PropagateDiagnostic(page.status, page.diagnostic);
    }
    if (page.classification.page_type != entry.first) {
      return LifecycleError("SB-DB-LIFECYCLE-CORE-PAGE-TYPE-MISMATCH",
                            "storage.database_lifecycle.core_page_type_mismatch",
                            device == nullptr ? std::string{} : device->path(),
                            std::to_string(entry.second));
    }
  }

  DatabaseLifecycleResult result;
  result.status = DatabaseLifecycleOkStatus();
  return result;
}

DatabaseLifecycleResult ValidateStartupPageIdentities(FileDevice* device,
                                                      u32 page_size,
                                                      const DiskDevicePolicy& policy,
                                                      const TypedUuid& database_uuid,
                                                      const TypedUuid& first_filespace_uuid) {
  const std::array<u64, 5> fixed_pages = {{
      kSystemStatePageNumber,
      kCatalogPageNumber,
      kAllocationMapPageNumber,
      kTransactionInventoryPageNumber,
      kBootstrapReservedPageNumber,
  }};
  for (const u64 page_number : fixed_pages) {
    const auto page = ReadDevicePageHeader(device, page_size, page_number, policy);
    if (!page.ok()) {
      return PropagateDiagnostic(page.status, page.diagnostic);
    }
    const auto parsed = ParsePageHeader(page.serialized);
    if (!parsed.ok()) {
      return PropagateDiagnostic(parsed.status, parsed.diagnostic);
    }
    if (!(parsed.header.database_uuid == database_uuid.value)) {
      return LifecycleError("SB-DB-LIFECYCLE-STARTUP-PAGE-DATABASE-UUID-MISMATCH",
                            "storage.database_lifecycle.startup_page_database_uuid_mismatch",
                            device == nullptr ? std::string{} : device->path(),
                            std::to_string(page_number));
    }
    if (!(parsed.header.filespace_uuid == first_filespace_uuid.value)) {
      return LifecycleError("SB-DB-LIFECYCLE-STARTUP-PAGE-FILESPACE-UUID-MISMATCH",
                            "storage.database_lifecycle.startup_page_filespace_uuid_mismatch",
                            device == nullptr ? std::string{} : device->path(),
                            std::to_string(page_number));
    }
  }

  DatabaseLifecycleResult result;
  result.status = DatabaseLifecycleOkStatus();
  return result;
}

std::map<std::string, std::string> ParseKeyValuePayload(const std::string& payload);
u32 ParseU32Field(const std::map<std::string, std::string>& fields, const std::string& key);

DatabaseLifecycleResult ValidateFilespaceCatalogManifest(const std::vector<CatalogPageRow>& rows,
                                                         const TypedUuid& database_uuid,
                                                         const TypedUuid& first_filespace_uuid,
                                                         const std::string& path) {
  u32 active_primary_count = 0;
  bool first_filespace_found = false;
  const std::string expected_database_uuid =
      scratchbird::core::uuid::UuidToString(database_uuid.value);
  const std::string expected_filespace_uuid =
      scratchbird::core::uuid::UuidToString(first_filespace_uuid.value);

  const auto require_field = [&](const std::map<std::string, std::string>& fields,
                                 const char* key,
                                 const char* expected) -> DatabaseLifecycleResult {
    const auto found = fields.find(key);
    if (found == fields.end()) {
      return LifecycleError("SB-DB-LIFECYCLE-FILESPACE-MANIFEST-FIELD-MISSING",
                            "storage.database_lifecycle.filespace_manifest_field_missing",
                            path,
                            key);
    }
    if (found->second != expected) {
      return LifecycleError("SB-DB-LIFECYCLE-FILESPACE-MANIFEST-FIELD-MISMATCH",
                            "storage.database_lifecycle.filespace_manifest_field_mismatch",
                            path,
                            std::string(key) + "=" + found->second);
    }
    DatabaseLifecycleResult ok;
    ok.status = DatabaseLifecycleOkStatus();
    return ok;
  };

  for (const CatalogPageRow& row : rows) {
    if (row.kind != CatalogPageRowKind::typed_catalog_record) {
      continue;
    }
    const auto decoded = DecodeCatalogTypedRecord(row);
    if (!decoded.ok()) {
      return PropagateDiagnostic(decoded.status, decoded.diagnostic);
    }
    if (decoded.record.header.kind != CatalogRecordKind::filespace) {
      continue;
    }

    const auto fields = ParseKeyValuePayload(decoded.record.payload);
    const auto role = fields.find("filespace_role");
    if (role != fields.end() && role->second == "active_primary") {
      ++active_primary_count;
    }

    const auto filespace_uuid = fields.find("filespace_uuid");
    if (filespace_uuid == fields.end() || filespace_uuid->second != expected_filespace_uuid) {
      continue;
    }
    first_filespace_found = true;

    for (const auto& required : {
             std::pair<const char*, const char*>{"database_uuid", expected_database_uuid.c_str()},
             {"filespace_uuid", expected_filespace_uuid.c_str()},
             {"filespace_role", "active_primary"},
             {"first_filespace", "1"},
             {"startup_authority", "1"},
             {"catalog_persistence_owner", "1"},
             {"filespace_manifest_owner", "1"},
             {"recovery_evidence_owner", "1"},
             {"read_only", "0"},
             {"state", "online"},
             {"physical_filespace_id", "0"},
             {"lifecycle_generation", "1"},
             {"filespace_manifest_generation", "1"},
             {"catalog_manifest_format_version", "1"},
             {"resource_seed_manifest_format_version", "1"},
             {"registered_txn", "1"},
             {"last_lifecycle_transaction", "1"},
             {"uuid_source", "fresh_uuidv7"},
             {"header_database_uuid_match_required", "1"},
             {"header_filespace_uuid_match_required", "1"},
             {"startup_state_coupled", "1"},
             {"page_header_coupled", "1"},
             {"open_validate_header", "1"},
             {"attach_admission_validate_header", "1"},
             {"transaction_admission_validate_filespace", "1"},
             {"maintenance_validate_header", "1"},
             {"verify_repair_validate_header", "1"},
             {"shutdown_validate_header", "1"},
             {"recovery_validate_header", "1"},
             {"drop_requires_database_lifecycle", "1"},
             {"quarantine_on_ambiguous", "1"},
             {"state_change_evidence_before_success", "1"},
             {"mga_visibility_required", "1"},
             {"path_is_locator_not_identity", "1"},
             {"duplicate_identity_refusal", "1"},
             {"stale_identity_refusal", "1"},
         }) {
      const auto required_result = require_field(fields, required.first, required.second);
      if (!required_result.ok()) {
        return required_result;
      }
    }
  }

  if (active_primary_count != 1) {
    return LifecycleError("SB-DB-LIFECYCLE-FILESPACE-ACTIVE-PRIMARY-COUNT-INVALID",
                          "storage.database_lifecycle.filespace_active_primary_count_invalid",
                          path,
                          std::to_string(active_primary_count));
  }
  if (!first_filespace_found) {
    return LifecycleError("SB-DB-LIFECYCLE-FIRST-FILESPACE-MANIFEST-MISSING",
                          "storage.database_lifecycle.first_filespace_manifest_missing",
                          path,
                          expected_filespace_uuid);
  }

  DatabaseLifecycleResult result;
  result.status = DatabaseLifecycleOkStatus();
  return result;
}

DatabaseLifecycleResult ValidateCatalogMigrationEvidence(const std::vector<CatalogPageRow>& rows,
                                                         const TypedUuid& database_uuid,
                                                         const TypedUuid& first_filespace_uuid,
                                                         const std::string& migration_plan_id,
                                                         const std::string& path) {
  DatabaseCatalogMigrationEvidence evidence;
  evidence.database_catalog_record_count = 0;
  evidence.active_primary_filespace_record_count = 0;
  evidence.database_uuid_matches_header = false;
  evidence.filespace_uuid_matches_startup = false;
  evidence.migration_plan_id = migration_plan_id;
  const std::string expected_database_uuid =
      scratchbird::core::uuid::UuidToString(database_uuid.value);
  const std::string expected_filespace_uuid =
      scratchbird::core::uuid::UuidToString(first_filespace_uuid.value);
  bool saw_resource_seed_manifest = false;

  for (const CatalogPageRow& row : rows) {
    if (row.kind == CatalogPageRowKind::resource_seed_pack) {
      const auto fields = ParseKeyValuePayload(row.payload);
      evidence.resource_seed_manifest_format_version =
          ParseU32Field(fields, "resource_seed_manifest_format_version");
      saw_resource_seed_manifest = true;
      continue;
    }
    if (row.kind != CatalogPageRowKind::typed_catalog_record) {
      continue;
    }
    const auto decoded = DecodeCatalogTypedRecord(row);
    if (!decoded.ok()) {
      return PropagateDiagnostic(decoded.status, decoded.diagnostic);
    }
    const auto fields = ParseKeyValuePayload(decoded.record.payload);
    if (decoded.record.header.kind == CatalogRecordKind::database) {
      ++evidence.database_catalog_record_count;
      const auto catalog_database_uuid = fields.find("database_uuid");
      evidence.database_uuid_matches_header =
          catalog_database_uuid != fields.end() &&
          catalog_database_uuid->second == expected_database_uuid;
      evidence.database_catalog_manifest_format_version =
          ParseU32Field(fields, "catalog_manifest_format_version");
    } else if (decoded.record.header.kind == CatalogRecordKind::filespace) {
      const auto role = fields.find("filespace_role");
      if (role != fields.end() && role->second == "active_primary") {
        ++evidence.active_primary_filespace_record_count;
      }
      const auto catalog_database_uuid = fields.find("database_uuid");
      const auto catalog_filespace_uuid = fields.find("filespace_uuid");
      if (catalog_database_uuid != fields.end() &&
          catalog_database_uuid->second == expected_database_uuid &&
          catalog_filespace_uuid != fields.end() &&
          catalog_filespace_uuid->second == expected_filespace_uuid) {
        evidence.filespace_uuid_matches_startup = true;
        evidence.filespace_catalog_manifest_format_version =
            ParseU32Field(fields, "catalog_manifest_format_version");
        evidence.filespace_resource_seed_manifest_format_version =
            ParseU32Field(fields, "resource_seed_manifest_format_version");
      }
    }
  }

  if (!saw_resource_seed_manifest) {
    evidence.resource_seed_manifest_format_version = 0;
  }

  const auto classified = ClassifyDatabaseCatalogMigrationEvidence(evidence);
  if (!classified.ok()) {
    DatabaseLifecycleResult result;
    result.status = classified.status;
    result.state.phase = DatabaseLifecyclePhase::failed;
    result.state.path = path;
    result.state.database_open_compatibility_class = classified.compatibility_class;
    result.diagnostic = classified.diagnostic;
    if (!result.diagnostic.arguments.empty()) {
      result.diagnostic.arguments.push_back({"path", path});
    }
    return result;
  }

  DatabaseLifecycleResult result;
  result.status = DatabaseLifecycleOkStatus();
  return result;
}

DatabaseLifecycleResult WriteInitialPageHeader(FileDevice* device,
                                               const PageManagerContext& context,
                                               PageType page_type,
                                               u64 page_number,
                                               u64 creation_unix_epoch_millis);

std::string EscapeField(std::string value) {
  for (char& ch : value) {
    if (ch == '\n' || ch == '\r' || ch == '|') {
      ch = ' ';
    }
  }
  return value;
}

std::string KeyValuePayload(const std::vector<std::pair<std::string, std::string>>& fields) {
  std::string result;
  for (const auto& field : fields) {
    result += field.first;
    result += '=';
    result += EscapeField(field.second);
    result += '\n';
  }
  return result;
}

std::map<std::string, std::string> ParseKeyValuePayload(const std::string& payload) {
  std::map<std::string, std::string> result;
  std::stringstream stream(payload);
  std::string line;
  while (std::getline(stream, line)) {
    std::stringstream tokens(line);
    std::string token;
    while (tokens >> token) {
      const std::size_t split = token.find('=');
      if (split == std::string::npos) {
        continue;
      }
      result[token.substr(0, split)] = token.substr(split + 1);
    }
  }
  return result;
}

struct DirtyManifestOpenClassification {
  Status status;
  DirtyObjectManifest manifest;
  DirtyManifestRecoveryResult recovery;
  DiagnosticRecord diagnostic;
  bool manifest_present = false;
  bool recovery_evidence_required = false;

  bool ok() const {
    return status.ok();
  }
};

std::string DirtyManifestPathForDatabase(const std::string& database_path) {
  return database_path + ".dirty.manifest";
}

std::string DirtyRecoveryEvidencePathForDatabase(const std::string& database_path) {
  return database_path + ".recovery.evidence";
}

DirtyManifestOpenClassification DirtyManifestClassificationOk() {
  DirtyManifestOpenClassification result;
  result.status = DatabaseLifecycleOkStatus();
  return result;
}

DirtyManifestOpenClassification DirtyManifestClassificationError(Status status,
                                                                 DiagnosticRecord diagnostic) {
  DirtyManifestOpenClassification result;
  result.status = status;
  result.diagnostic = std::move(diagnostic);
  return result;
}

DirtyManifestOpenClassification ClassifyDirtyManifestSidecarForOpen(const std::string& database_path,
                                                                    const StartupStateRecord& startup_state) {
  if (!startup_state.startup_dirty) {
    return DirtyManifestClassificationOk();
  }

  const std::string manifest_path = DirtyManifestPathForDatabase(database_path);
  std::error_code stat_error;
  if (!std::filesystem::exists(manifest_path, stat_error)) {
    return DirtyManifestClassificationOk();
  }
  if (stat_error || !std::filesystem::is_regular_file(manifest_path, stat_error)) {
    return DirtyManifestClassificationError(
        DatabaseLifecycleErrorStatus(),
        MakeDatabaseLifecycleDiagnostic(DatabaseLifecycleErrorStatus(),
                                        "SB-DB-LIFECYCLE-DIRTY-MANIFEST-UNREADABLE",
                                        "storage.database_lifecycle.dirty_manifest_unreadable",
                                        database_path,
                                        manifest_path));
  }

  std::ifstream input(manifest_path, std::ios::binary);
  if (!input) {
    return DirtyManifestClassificationError(
        DatabaseLifecycleErrorStatus(),
        MakeDatabaseLifecycleDiagnostic(DatabaseLifecycleErrorStatus(),
                                        "SB-DB-LIFECYCLE-DIRTY-MANIFEST-UNREADABLE",
                                        "storage.database_lifecycle.dirty_manifest_unreadable",
                                        database_path,
                                        manifest_path));
  }
  const std::string serialized((std::istreambuf_iterator<char>(input)),
                               std::istreambuf_iterator<char>());
  const auto parsed = ParseDirtyObjectManifest(serialized);
  if (!parsed.ok()) {
    return DirtyManifestClassificationError(parsed.status, parsed.diagnostic);
  }
  const auto recovery = ClassifyDirtyObjectManifestForRecovery(parsed.manifest);
  if (!recovery.ok()) {
    return DirtyManifestClassificationError(recovery.status, recovery.diagnostic);
  }
  if (parsed.manifest.checkpoint_generation > startup_state.checkpoint_generation) {
    return DirtyManifestClassificationError(
        DatabaseLifecycleErrorStatus(),
        MakeDatabaseLifecycleDiagnostic(DatabaseLifecycleErrorStatus(),
                                        "SB-DB-LIFECYCLE-RECOVERY-UNPROVEN-MANIFEST",
                                        "storage.database_lifecycle.recovery_unproven_manifest",
                                        database_path,
                                        manifest_path));
  }
  if (recovery.quarantine_required) {
    return DirtyManifestClassificationError(
        DatabaseLifecycleErrorStatus(),
        MakeDatabaseLifecycleDiagnostic(DatabaseLifecycleErrorStatus(),
                                        "SB-DB-LIFECYCLE-RECOVERY-QUARANTINE-REQUIRED",
                                        "storage.database_lifecycle.recovery_quarantine_required",
                                        database_path,
                                        manifest_path));
  }

  DirtyManifestOpenClassification result;
  result.status = DatabaseLifecycleOkStatus();
  result.manifest_present = true;
  result.recovery_evidence_required = recovery.rebuild_by_scan_required ||
                                      !recovery.classifications.empty();
  result.manifest = parsed.manifest;
  result.recovery = recovery;
  return result;
}

u64 StableRecoveryTextChecksum(const std::string& value) {
  u64 checksum = 1469598103934665603ull;
  for (unsigned char c : value) {
    checksum ^= static_cast<u64>(c);
    checksum *= 1099511628211ull;
  }
  return checksum;
}

DirtyObjectKind DirtyObjectKindForPageType(PageType page_type) {
  switch (page_type) {
    case PageType::database_header:
      return DirtyObjectKind::database_header;
    case PageType::system_state:
      return DirtyObjectKind::startup_state;
    case PageType::transaction_inventory:
      return DirtyObjectKind::transaction_inventory;
    case PageType::catalog:
      return DirtyObjectKind::catalog_page;
    case PageType::allocation_map:
      return DirtyObjectKind::allocation_map;
    case PageType::row_data:
      return DirtyObjectKind::row_data_page;
    case PageType::index_btree:
    case PageType::index_btree_root:
    case PageType::index_btree_branch:
    case PageType::index_btree_leaf:
    case PageType::index_btree_posting:
    case PageType::index_hash:
    case PageType::index_bitmap:
    case PageType::index_summary:
    case PageType::index_inverted:
    case PageType::index_spatial:
    case PageType::index_vector:
    case PageType::index_graph:
    case PageType::index_temporary:
    case PageType::index_statistics:
    case PageType::index_special_root:
      return DirtyObjectKind::index_page;
    case PageType::metrics:
      return DirtyObjectKind::metric_history;
    case PageType::filespace_directory:
      return DirtyObjectKind::filespace_header;
    default:
      return DirtyObjectKind::unknown;
  }
}

DatabaseLifecycleResult EmitDirtyManifestForCheckpoint(const std::string& database_path,
                                                       const StartupStateRecord& startup_state,
                                                       const std::vector<PageCacheEntry>& entries) {
  const u64 local_transaction_id = startup_state.clean_shutdown_local_transaction_id != 0
      ? startup_state.clean_shutdown_local_transaction_id
      : startup_state.last_lifecycle_local_transaction_id;
  if (database_path.empty() || startup_state.checkpoint_generation == 0 || local_transaction_id == 0) {
    return LifecycleError("SB-DB-LIFECYCLE-DIRTY-MANIFEST-EVIDENCE-MISSING",
                          "storage.database_lifecycle.dirty_manifest_evidence_missing",
                          database_path);
  }

  DirtyObjectManifest manifest;
  manifest.format_version = kDirtyObjectManifestFormatVersion;
  manifest.checkpoint_generation = startup_state.checkpoint_generation;
  manifest.completed = true;
  manifest.classification_only = true;

  for (const auto& page : entries) {
    DirtyObjectManifestEntry entry;
    entry.kind = DirtyObjectKindForPageType(page.page_type);
    if (entry.kind == DirtyObjectKind::unknown) {
      continue;
    }
    entry.object_uuid = page.page_uuid;
    entry.page_number = page.page_number;
    entry.page_generation = page.page_generation;
    const std::string object_material =
        std::to_string(page.page_number) + ":" +
        std::to_string(page.page_generation) + ":" +
        std::to_string(static_cast<u32>(page.page_type)) + ":" +
        scratchbird::core::uuid::UuidToString(page.page_uuid.value);
    entry.object_checksum = StableRecoveryTextChecksum(object_material);
    entry.local_transaction_id = local_transaction_id;
    entry.operation_envelope_checksum = StableRecoveryTextChecksum(
        "checkpoint:" + std::to_string(startup_state.checkpoint_generation) +
        ":page:" + std::to_string(page.page_number) +
        ":tx:" + std::to_string(local_transaction_id));
    entry.transaction_evidence_checksum = StableRecoveryTextChecksum(
        "transaction_inventory:" + std::to_string(local_transaction_id) +
        ":lifecycle:" + std::to_string(startup_state.lifecycle_generation));
    entry.dirty = true;
    entry.authoritative = true;
    manifest.entries.push_back(std::move(entry));
  }

  const auto built = BuildDirtyObjectManifest(manifest);
  if (!built.ok()) {
    return PropagateDiagnostic(built.status, built.diagnostic);
  }
  std::ofstream output(DirtyManifestPathForDatabase(database_path), std::ios::binary | std::ios::trunc);
  if (!output) {
    return LifecycleError("SB-DB-LIFECYCLE-DIRTY-MANIFEST-WRITE-FAILED",
                          "storage.database_lifecycle.dirty_manifest_write_failed",
                          database_path);
  }
  output << built.serialized;
  output.flush();
  if (!output) {
    return LifecycleError("SB-DB-LIFECYCLE-DIRTY-MANIFEST-WRITE-FAILED",
                          "storage.database_lifecycle.dirty_manifest_write_failed",
                          database_path);
  }

  DatabaseLifecycleResult result;
  result.status = DatabaseLifecycleOkStatus();
  return result;
}

DatabaseLifecycleResult PersistDirtyManifestRecoveryEvidenceForOpen(
    const std::string& database_path,
    const DirtyManifestOpenClassification& dirty_manifest) {
  if (!dirty_manifest.manifest_present || !dirty_manifest.recovery_evidence_required) {
    DatabaseLifecycleResult result;
    result.status = DatabaseLifecycleOkStatus();
    return result;
  }
  const auto recovery_run_uuid = GenerateEngineIdentityV7(UuidKind::object, CurrentUnixEpochMillis());
  if (!recovery_run_uuid.ok()) {
    return PropagateDiagnostic(recovery_run_uuid.status, recovery_run_uuid.diagnostic);
  }
  const auto evidence = PersistDirtyManifestRecoveryRunEvidence(
      DirtyRecoveryEvidencePathForDatabase(database_path),
      dirty_manifest.manifest,
      dirty_manifest.recovery,
      scratchbird::core::uuid::UuidToString(recovery_run_uuid.value.value));
  if (!evidence.ok()) {
    return PropagateDiagnostic(evidence.status, evidence.diagnostic);
  }

  DatabaseLifecycleResult result;
  result.status = DatabaseLifecycleOkStatus();
  return result;
}

u32 ParseU32Field(const std::map<std::string, std::string>& fields, const std::string& key) {
  const auto found = fields.find(key);
  if (found == fields.end()) {
    return 0;
  }
  return static_cast<u32>(std::strtoul(found->second.c_str(), nullptr, 10));
}

u64 ParseU64Field(const std::map<std::string, std::string>& fields, const std::string& key) {
  const auto found = fields.find(key);
  if (found == fields.end()) {
    return 0;
  }
  return static_cast<u64>(std::strtoull(found->second.c_str(), nullptr, 10));
}

ResourceSeedFamily ParseResourceFamilyName(const std::string& value) {
  for (ResourceSeedFamily family : {ResourceSeedFamily::charset,
                                    ResourceSeedFamily::charset_mapping,
                                    ResourceSeedFamily::charset_mapping_schema,
                                    ResourceSeedFamily::collation,
                                    ResourceSeedFamily::locale,
                                    ResourceSeedFamily::uca,
                                    ResourceSeedFamily::uca_manifest,
                                    ResourceSeedFamily::i18n_version,
                                    ResourceSeedFamily::timezone_version,
                                    ResourceSeedFamily::timezone_source,
                                    ResourceSeedFamily::timezone_tables,
                                    ResourceSeedFamily::timezone_leaps,
                                    ResourceSeedFamily::timezone_archives}) {
    if (value == ResourceSeedFamilyName(family)) {
      return family;
    }
  }
  return ResourceSeedFamily::unknown;
}

CatalogPageRow Row(CatalogPageRowKind kind, u32 ordinal, std::string payload) {
  CatalogPageRow row;
  row.kind = kind;
  row.ordinal = ordinal;
  row.payload = std::move(payload);
  return row;
}

std::string JoinStrings(const std::vector<std::string>& values, const char* separator = ",") {
  std::string joined;
  bool first = true;
  for (const auto& value : values) {
    if (!first) {
      joined += separator;
    }
    joined += value;
    first = false;
  }
  return joined;
}

std::string JoinMetricLabelKeys(const MetricDescriptor& descriptor, bool sensitive) {
  std::vector<std::string> keys;
  for (const auto& label : descriptor.labels) {
    if (label.sensitive == sensitive) {
      keys.push_back(label.required ? label.key + ":required" : label.key);
    }
  }
  return JoinStrings(keys);
}

std::string JoinMetricBuckets(const MetricDescriptor& descriptor) {
  std::vector<std::string> buckets;
  for (const double bucket : descriptor.histogram_buckets) {
    std::ostringstream stream;
    stream << bucket;
    buckets.push_back(stream.str());
  }
  return JoinStrings(buckets);
}

struct CatalogRowsBuildResult {
  Status status;
  std::vector<CatalogPageRow> rows;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct DatabaseCatalogSummaryResult {
  Status status;
  u32 typed_record_count = 0;
  std::vector<std::string> typed_record_kinds;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

CatalogRowsBuildResult CatalogRowsBuildError(Status status, DiagnosticRecord diagnostic) {
  CatalogRowsBuildResult result;
  result.status = status;
  result.diagnostic = std::move(diagnostic);
  return result;
}

DatabaseCatalogSummaryResult DatabaseCatalogSummaryError(Status status, DiagnosticRecord diagnostic) {
  DatabaseCatalogSummaryResult result;
  result.status = status;
  result.diagnostic = std::move(diagnostic);
  return result;
}

struct PolicyProviderSeed {
  std::string provider_uuid;
  std::string provider_family;
  std::string authority;
  std::string credential_verifier_policy;
  bool enabled_by_default = false;
  bool external_provider = false;
};

struct PolicyRoleSeed {
  std::string role_uuid;
  std::string role_code;
  std::string description;
  std::string default_assignment;
};

struct PolicyGroupSeed {
  std::string group_uuid;
  std::string group_code;
  std::string description;
};

struct PolicyGroupMembershipSeed {
  std::string member_uuid;
  std::string member_kind;
  std::string parent_uuid;
  std::string parent_kind;
};

struct PolicyGrantSeed {
  std::string grant_uuid;
  std::string subject_uuid;
  std::string subject_kind;
  std::string target_uuid;
  std::string right;
  std::string effect;
};

struct PolicyProfileSeed {
  std::string profile_uuid;
  std::string area;
  std::string mode;
};

struct LoadedPolicySeedPack {
  PolicySeedPackCatalogImage image;
  std::vector<PolicyProviderSeed> providers;
  std::vector<PolicyRoleSeed> roles;
  std::vector<PolicyGroupSeed> groups;
  std::vector<PolicyGroupMembershipSeed> memberships;
  std::vector<PolicyGrantSeed> grants;
  std::vector<PolicyProfileSeed> profiles;
};

struct PolicyPackLoadResult {
  Status status;
  LoadedPolicySeedPack pack;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

PolicyPackLoadResult PolicyPackLoadError(std::string diagnostic_code,
                                         std::string detail) {
  PolicyPackLoadResult result;
  result.status = DatabaseLifecycleErrorStatus();
  result.diagnostic = MakeDatabaseLifecycleDiagnostic(result.status,
                                                      std::move(diagnostic_code),
                                                      "storage.database_lifecycle.policy_pack_invalid",
                                                      {},
                                                      std::move(detail));
  return result;
}

PolicyPackLoadResult PolicyPackLoadOk(LoadedPolicySeedPack pack) {
  PolicyPackLoadResult result;
  result.status = DatabaseLifecycleOkStatus();
  result.pack = std::move(pack);
  return result;
}

std::string ReadTextFileOrEmpty(const std::filesystem::path& path, bool* ok) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    *ok = false;
    return {};
  }
  *ok = true;
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

std::string HexDigest(const unsigned char* digest, std::size_t size) {
  std::ostringstream stream;
  stream << std::hex << std::setfill('0');
  for (std::size_t i = 0; i < size; ++i) {
    stream << std::setw(2) << static_cast<unsigned int>(digest[i]);
  }
  return stream.str();
}

std::string Sha256Hex(const std::string& payload) {
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(payload.data()), payload.size(), digest);
  return HexDigest(digest, SHA256_DIGEST_LENGTH);
}

bool IsSafePackRelativePath(const std::string& rel_path) {
  if (rel_path.empty()) {
    return false;
  }
  const std::filesystem::path path(rel_path);
  if (path.is_absolute()) {
    return false;
  }
  for (const auto& part : path) {
    if (part == "..") {
      return false;
    }
  }
  return true;
}

bool ExtractStringField(const std::string& object, const std::string& key, std::string* value) {
  const std::regex pattern("\"" + key + "\"\\s*:\\s*\"([^\"]*)\"");
  std::smatch match;
  if (!std::regex_search(object, match, pattern)) {
    return false;
  }
  *value = match[1].str();
  return true;
}

bool ExtractU32Field(const std::string& object, const std::string& key, u32* value) {
  const std::regex pattern("\"" + key + "\"\\s*:\\s*([0-9]+)");
  std::smatch match;
  if (!std::regex_search(object, match, pattern)) {
    return false;
  }
  *value = static_cast<u32>(std::stoul(match[1].str()));
  return true;
}

bool ExtractBoolField(const std::string& object, const std::string& key, bool* value) {
  const std::regex pattern("\"" + key + "\"\\s*:\\s*(true|false)");
  std::smatch match;
  if (!std::regex_search(object, match, pattern)) {
    return false;
  }
  *value = match[1].str() == "true";
  return true;
}

bool ExtractArrayBody(const std::string& text, const std::string& key, std::string* body) {
  const std::string token = "\"" + key + "\"";
  const std::size_t key_pos = text.find(token);
  if (key_pos == std::string::npos) {
    return false;
  }
  const std::size_t start = text.find('[', key_pos + token.size());
  if (start == std::string::npos) {
    return false;
  }
  bool in_string = false;
  bool escaped = false;
  int depth = 0;
  std::size_t body_start = std::string::npos;
  for (std::size_t i = start; i < text.size(); ++i) {
    const char ch = text[i];
    if (in_string) {
      if (escaped) {
        escaped = false;
      } else if (ch == '\\') {
        escaped = true;
      } else if (ch == '"') {
        in_string = false;
      }
      continue;
    }
    if (ch == '"') {
      in_string = true;
      continue;
    }
    if (ch == '[') {
      ++depth;
      if (depth == 1) {
        body_start = i + 1;
      }
      continue;
    }
    if (ch == ']') {
      --depth;
      if (depth == 0 && body_start != std::string::npos) {
        *body = text.substr(body_start, i - body_start);
        return true;
      }
    }
  }
  return false;
}

std::vector<std::string> SplitTopLevelObjects(const std::string& array_body) {
  std::vector<std::string> objects;
  bool in_string = false;
  bool escaped = false;
  int depth = 0;
  std::size_t object_start = std::string::npos;
  for (std::size_t i = 0; i < array_body.size(); ++i) {
    const char ch = array_body[i];
    if (in_string) {
      if (escaped) {
        escaped = false;
      } else if (ch == '\\') {
        escaped = true;
      } else if (ch == '"') {
        in_string = false;
      }
      continue;
    }
    if (ch == '"') {
      in_string = true;
      continue;
    }
    if (ch == '{') {
      if (depth == 0) {
        object_start = i;
      }
      ++depth;
      continue;
    }
    if (ch == '}') {
      --depth;
      if (depth == 0 && object_start != std::string::npos) {
        objects.push_back(array_body.substr(object_start, i - object_start + 1));
        object_start = std::string::npos;
      }
    }
  }
  return objects;
}

bool ValidatePolicyUuid(const std::string& value, UuidKind kind) {
  const auto parsed = ParseTypedUuid(kind, value);
  return parsed.ok();
}

bool ArrayContainsString(const std::string& text,
                         const std::string& array_key,
                         const std::string& value) {
  std::string body;
  if (!ExtractArrayBody(text, array_key, &body)) {
    return false;
  }
  return body.find("\"" + value + "\"") != std::string::npos;
}

bool IsSafePolicyPackId(const std::string& value) {
  if (value.empty() || value.size() > 128) {
    return false;
  }
  for (const char ch : value) {
    const auto uch = static_cast<unsigned char>(ch);
    if (!std::isalnum(uch) && ch != '-' && ch != '_' && ch != '.') {
      return false;
    }
  }
  return value.find("..") == std::string::npos;
}

bool IsSha256Hex(const std::string& value) {
  if (value.size() != 64) {
    return false;
  }
  for (const char ch : value) {
    const auto uch = static_cast<unsigned char>(ch);
    if (!std::isxdigit(uch)) {
      return false;
    }
  }
  return true;
}

const std::set<std::string>& RequiredPolicyProfileAreas() {
  static const std::set<std::string> areas = {
      "agent_policy",
      "cluster_boundary",
      "default_security_posture",
      "diagnostics",
      "index_maintenance",
      "memory_resource_governance",
      "observability",
      "optimizer_statistics_feedback",
      "release_default_configuration",
      "security_provider_selection",
      "standard_roles_groups_grants",
      "storage_filespace_page_policy",
      "transaction_mga_cleanup_archive_backup_forward",
      "unsupported_feature_behavior",
  };
  return areas;
}

bool PolicyProfileAreasExactlyCovered(const std::vector<std::string>& areas) {
  return std::set<std::string>(areas.begin(), areas.end()) == RequiredPolicyProfileAreas();
}

PolicyPackLoadResult LoadSelectedPolicySeedPack(const std::string& pack_root_text) {
  if (pack_root_text.empty()) {
    return PolicyPackLoadError("SB-POLICY-PACK-ROOT-REQUIRED",
                               "policy seed pack root is required");
  }
  const std::filesystem::path pack_root(pack_root_text);
  bool read_ok = false;
  const std::string manifest =
      ReadTextFileOrEmpty(pack_root / "POLICY_PACK_MANIFEST.json", &read_ok);
  if (!read_ok) {
    return PolicyPackLoadError("SB-POLICY-PACK-MANIFEST-MISSING",
                               pack_root_text + "/POLICY_PACK_MANIFEST.json");
  }

  LoadedPolicySeedPack loaded;
  PolicySeedPackCatalogImage& image = loaded.image;
  image.active = true;
  if (!ExtractU32Field(manifest, "schema_version", &image.schema_version) ||
      image.schema_version != 1) {
    return PolicyPackLoadError("SB-POLICY-PACK-SCHEMA-UNSUPPORTED",
                               "schema_version");
  }
  u32 min_schema = 0;
  u32 max_schema = 0;
  if (!ExtractU32Field(manifest, "min_supported_schema_version", &min_schema) ||
      !ExtractU32Field(manifest, "max_supported_schema_version", &max_schema) ||
      min_schema != 1 || max_schema != 1) {
    return PolicyPackLoadError("SB-POLICY-PACK-SCHEMA-UNSUPPORTED",
                               "supported_schema_range");
  }
  std::string content_hash_algorithm;
  if (!ExtractStringField(manifest, "policy_pack_id", &image.policy_pack_id) ||
      !ExtractStringField(manifest, "policy_pack_uuid", &image.policy_pack_uuid) ||
      !ExtractStringField(manifest, "policy_pack_version", &image.policy_pack_version) ||
      !ExtractStringField(manifest, "content_hash_algorithm", &content_hash_algorithm) ||
      !ExtractStringField(manifest, "content_sha256", &image.content_sha256) ||
      !ExtractStringField(manifest, "signature_status", &image.signature_status)) {
    return PolicyPackLoadError("SB-POLICY-PACK-MANIFEST-FIELD-MISSING",
                               "manifest identity/hash/signature fields");
  }
  if (!IsSafePolicyPackId(image.policy_pack_id) ||
      !ValidatePolicyUuid(image.policy_pack_uuid, UuidKind::object) ||
      image.policy_pack_version.empty() ||
      content_hash_algorithm != "sha256" ||
      !IsSha256Hex(image.content_sha256) ||
      image.signature_status != "signature-ready-unsigned") {
    return PolicyPackLoadError("SB-POLICY-PACK-MANIFEST-FIELD-INVALID",
                               "identity/hash/signature policy");
  }
  std::string provenance_source;
  bool private_inputs_required = true;
  bool external_provider_runtime_required = true;
  if (!ExtractStringField(manifest, "source", &provenance_source) ||
      !ExtractBoolField(manifest, "private_inputs_required", &private_inputs_required) ||
      !ExtractBoolField(manifest,
                        "external_provider_runtime_required",
                        &external_provider_runtime_required) ||
      provenance_source != "public-project-tree" ||
      private_inputs_required ||
      external_provider_runtime_required) {
    return PolicyPackLoadError("SB-POLICY-PACK-PROVENANCE-INVALID",
                               "public-project-tree provenance required");
  }
  if (!ExtractBoolField(manifest, "create_time_only", &image.create_time_only) ||
      !ExtractBoolField(manifest, "post_create_filesystem_authority",
                        &image.post_create_filesystem_authority) ||
      !image.create_time_only ||
      image.post_create_filesystem_authority) {
    return PolicyPackLoadError("SB-POLICY-PACK-AUTHORITY-POLICY-INVALID",
                               "create_time_only/post_create_filesystem_authority");
  }
  std::string provider_mode;
  if (!ExtractStringField(manifest, "mode", &provider_mode) ||
      provider_mode != "local-password-only" ||
      !ArrayContainsString(manifest, "enabled_provider_families", "local_password")) {
    return PolicyPackLoadError("SB-POLICY-PACK-PROVIDER-POLICY-INVALID",
                               "local-password-only");
  }
  image.local_password_only = true;

  std::string content_manifest_body;
  if (!ExtractArrayBody(manifest, "content_manifest", &content_manifest_body)) {
    return PolicyPackLoadError("SB-POLICY-PACK-CONTENT-MANIFEST-MISSING",
                               "content_manifest");
  }
  std::string aggregate_payload;
  std::set<std::string> seen_content_paths;
  for (const std::string& object : SplitTopLevelObjects(content_manifest_body)) {
    std::string rel_path;
    std::string expected_sha256;
    if (!ExtractStringField(object, "path", &rel_path) ||
        !ExtractStringField(object, "sha256", &expected_sha256) ||
        !IsSafePackRelativePath(rel_path) ||
        !seen_content_paths.insert(rel_path).second) {
      return PolicyPackLoadError("SB-POLICY-PACK-CONTENT-MANIFEST-INVALID",
                                 rel_path);
    }
    const std::string content = ReadTextFileOrEmpty(pack_root / rel_path, &read_ok);
    if (!read_ok) {
      return PolicyPackLoadError("SB-POLICY-PACK-CONTENT-FILE-MISSING",
                                 rel_path);
    }
    const std::string actual_sha256 = Sha256Hex(content);
    if (actual_sha256 != expected_sha256) {
      return PolicyPackLoadError("SB-POLICY-PACK-CONTENT-HASH-MISMATCH",
                                 rel_path);
    }
    aggregate_payload += rel_path;
    aggregate_payload.push_back('\0');
    aggregate_payload += actual_sha256;
    aggregate_payload.push_back('\n');
  }
  if (seen_content_paths.empty() || Sha256Hex(aggregate_payload) != image.content_sha256) {
    return PolicyPackLoadError("SB-POLICY-PACK-CONTENT-HASH-MISMATCH",
                               "aggregate content_sha256");
  }

  std::string materialization_path;
  if (!ExtractStringField(manifest, "catalog_materialization_metadata", &materialization_path) ||
      !IsSafePackRelativePath(materialization_path)) {
    return PolicyPackLoadError("SB-POLICY-PACK-MATERIALIZATION-MISSING",
                               "catalog_materialization_metadata");
  }
  const std::string materialization =
      ReadTextFileOrEmpty(pack_root / materialization_path, &read_ok);
  if (!read_ok) {
    return PolicyPackLoadError("SB-POLICY-PACK-MATERIALIZATION-MISSING",
                               materialization_path);
  }
  if (!ExtractU32Field(materialization, "policy_generation", &image.policy_generation) ||
      !ExtractBoolField(materialization, "materialize_inside_create_transaction",
                        &image.materialized_inside_create_transaction) ||
      !ExtractBoolField(materialization, "requires_mga_catalog_commit",
                        &image.requires_mga_catalog_commit) ||
      !image.materialized_inside_create_transaction ||
      !image.requires_mga_catalog_commit ||
      !ArrayContainsString(materialization, "catalog_row_families", "PolicyPackRecord") ||
      !ArrayContainsString(materialization, "catalog_row_families", "SecurityProviderRecord") ||
      !ArrayContainsString(materialization, "catalog_row_families", "SecurityRoleRecord") ||
      !ArrayContainsString(materialization, "catalog_row_families", "SecurityGroupRecord") ||
      !ArrayContainsString(materialization, "catalog_row_families", "SecurityGrantRecord") ||
      !ArrayContainsString(materialization, "catalog_row_families", "PolicyProfileRecord")) {
    return PolicyPackLoadError("SB-POLICY-PACK-MATERIALIZATION-INVALID",
                               materialization_path);
  }

  const auto load_content = [&](const char* rel_path) -> std::string {
    return ReadTextFileOrEmpty(pack_root / rel_path, &read_ok);
  };
  const std::string providers_json = load_content("policies/security_providers.json");
  if (!read_ok) { return PolicyPackLoadError("SB-POLICY-PACK-CONTENT-FILE-MISSING", "policies/security_providers.json"); }
  const std::string roles_json = load_content("policies/roles.json");
  if (!read_ok) { return PolicyPackLoadError("SB-POLICY-PACK-CONTENT-FILE-MISSING", "policies/roles.json"); }
  const std::string groups_json = load_content("policies/groups.json");
  if (!read_ok) { return PolicyPackLoadError("SB-POLICY-PACK-CONTENT-FILE-MISSING", "policies/groups.json"); }
  const std::string grants_json = load_content("policies/grants.json");
  if (!read_ok) { return PolicyPackLoadError("SB-POLICY-PACK-CONTENT-FILE-MISSING", "policies/grants.json"); }
  const std::string profiles_json = load_content("policies/policy_profiles.json");
  if (!read_ok) { return PolicyPackLoadError("SB-POLICY-PACK-CONTENT-FILE-MISSING", "policies/policy_profiles.json"); }

  std::string array_body;
  if (!ExtractArrayBody(providers_json, "providers", &array_body)) {
    return PolicyPackLoadError("SB-POLICY-PACK-PROVIDERS-INVALID", "providers");
  }
  for (const std::string& object : SplitTopLevelObjects(array_body)) {
    PolicyProviderSeed provider;
    if (!ExtractStringField(object, "provider_uuid", &provider.provider_uuid) ||
        !ExtractStringField(object, "provider_family", &provider.provider_family) ||
        !ExtractStringField(object, "authority", &provider.authority) ||
        !ExtractBoolField(object, "enabled_by_default", &provider.enabled_by_default) ||
        !ExtractBoolField(object, "external_provider", &provider.external_provider) ||
        !ValidatePolicyUuid(provider.provider_uuid, UuidKind::object)) {
      return PolicyPackLoadError("SB-POLICY-PACK-PROVIDERS-INVALID", "provider");
    }
    ExtractStringField(object, "credential_verifier_policy", &provider.credential_verifier_policy);
    if (provider.provider_family == "local_password") {
      if (!provider.enabled_by_default || provider.external_provider ||
          provider.authority != "durable_catalog_row") {
        return PolicyPackLoadError("SB-POLICY-PACK-PROVIDERS-INVALID",
                                   "local_password");
      }
      ++image.enabled_local_password_provider_records;
    } else if (provider.enabled_by_default || !provider.external_provider ||
               provider.authority != "unsupported_by_default") {
      return PolicyPackLoadError("SB-POLICY-PACK-PROVIDERS-INVALID",
                                 provider.provider_family);
    } else {
      ++image.external_provider_disabled_records;
    }
    loaded.providers.push_back(std::move(provider));
  }
  image.security_provider_records = static_cast<u32>(loaded.providers.size());
  if (image.enabled_local_password_provider_records != 1 ||
      image.external_provider_disabled_records == 0) {
    return PolicyPackLoadError("SB-POLICY-PACK-PROVIDERS-INVALID",
                               "default provider counts");
  }

  if (!ExtractArrayBody(roles_json, "roles", &array_body)) {
    return PolicyPackLoadError("SB-POLICY-PACK-ROLES-INVALID", "roles");
  }
  std::set<std::string> role_uuids;
  for (const std::string& object : SplitTopLevelObjects(array_body)) {
    PolicyRoleSeed role;
    if (!ExtractStringField(object, "role_uuid", &role.role_uuid) ||
        !ExtractStringField(object, "role_code", &role.role_code) ||
        !ValidatePolicyUuid(role.role_uuid, UuidKind::object)) {
      return PolicyPackLoadError("SB-POLICY-PACK-ROLES-INVALID", "role");
    }
    ExtractStringField(object, "description", &role.description);
    ExtractStringField(object, "default_assignment", &role.default_assignment);
    role_uuids.insert(role.role_uuid);
    loaded.roles.push_back(std::move(role));
  }
  image.role_records = static_cast<u32>(loaded.roles.size());

  if (!ExtractArrayBody(groups_json, "groups", &array_body)) {
    return PolicyPackLoadError("SB-POLICY-PACK-GROUPS-INVALID", "groups");
  }
  std::set<std::string> group_uuids;
  for (const std::string& object : SplitTopLevelObjects(array_body)) {
    PolicyGroupSeed group;
    if (!ExtractStringField(object, "group_uuid", &group.group_uuid) ||
        !ExtractStringField(object, "group_code", &group.group_code) ||
        !ValidatePolicyUuid(group.group_uuid, UuidKind::object)) {
      return PolicyPackLoadError("SB-POLICY-PACK-GROUPS-INVALID", "group");
    }
    ExtractStringField(object, "description", &group.description);
    group_uuids.insert(group.group_uuid);
    loaded.groups.push_back(std::move(group));
  }
  image.group_records = static_cast<u32>(loaded.groups.size());

  if (ExtractArrayBody(groups_json, "memberships", &array_body)) {
    for (const std::string& object : SplitTopLevelObjects(array_body)) {
      PolicyGroupMembershipSeed membership;
      if (!ExtractStringField(object, "member_uuid", &membership.member_uuid) ||
          !ExtractStringField(object, "member_kind", &membership.member_kind) ||
          !ExtractStringField(object, "parent_uuid", &membership.parent_uuid) ||
          !ExtractStringField(object, "parent_kind", &membership.parent_kind) ||
          membership.member_kind != "group" ||
          membership.parent_kind != "role" ||
          group_uuids.count(membership.member_uuid) == 0 ||
          role_uuids.count(membership.parent_uuid) == 0) {
        return PolicyPackLoadError("SB-POLICY-PACK-GROUPS-INVALID", "membership");
      }
      loaded.memberships.push_back(std::move(membership));
    }
  }
  image.group_membership_records = static_cast<u32>(loaded.memberships.size());

  if (!ExtractArrayBody(grants_json, "grants", &array_body)) {
    return PolicyPackLoadError("SB-POLICY-PACK-GRANTS-INVALID", "grants");
  }
  for (const std::string& object : SplitTopLevelObjects(array_body)) {
    PolicyGrantSeed grant;
    if (!ExtractStringField(object, "grant_uuid", &grant.grant_uuid) ||
        !ExtractStringField(object, "subject_uuid", &grant.subject_uuid) ||
        !ExtractStringField(object, "subject_kind", &grant.subject_kind) ||
        !ExtractStringField(object, "right", &grant.right) ||
        !ExtractStringField(object, "effect", &grant.effect) ||
        !ValidatePolicyUuid(grant.grant_uuid, UuidKind::row) ||
        (grant.subject_kind != "role" && grant.subject_kind != "group") ||
        (grant.effect != "allow" && grant.effect != "deny")) {
      return PolicyPackLoadError("SB-POLICY-PACK-GRANTS-INVALID", "grant");
    }
    ExtractStringField(object, "target_uuid", &grant.target_uuid);
    const bool subject_known =
        (grant.subject_kind == "role" && role_uuids.count(grant.subject_uuid) != 0) ||
        (grant.subject_kind == "group" && group_uuids.count(grant.subject_uuid) != 0);
    if (!subject_known) {
      return PolicyPackLoadError("SB-POLICY-PACK-GRANTS-INVALID",
                                 grant.subject_uuid);
    }
    loaded.grants.push_back(std::move(grant));
  }
  image.grant_records = static_cast<u32>(loaded.grants.size());

  if (!ExtractArrayBody(profiles_json, "profiles", &array_body)) {
    return PolicyPackLoadError("SB-POLICY-PACK-PROFILES-INVALID", "profiles");
  }
  std::set<std::string> profile_areas;
  for (const std::string& object : SplitTopLevelObjects(array_body)) {
    PolicyProfileSeed profile;
    if (!ExtractStringField(object, "profile_uuid", &profile.profile_uuid) ||
        !ExtractStringField(object, "area", &profile.area) ||
        !ExtractStringField(object, "mode", &profile.mode) ||
        !ValidatePolicyUuid(profile.profile_uuid, UuidKind::object) ||
        !profile_areas.insert(profile.area).second) {
      return PolicyPackLoadError("SB-POLICY-PACK-PROFILES-INVALID", "profile");
    }
    loaded.profiles.push_back(std::move(profile));
  }
  image.policy_profile_records = static_cast<u32>(loaded.profiles.size());
  image.policy_profile_areas.assign(profile_areas.begin(), profile_areas.end());
  if (!PolicyProfileAreasExactlyCovered(image.policy_profile_areas)) {
    return PolicyPackLoadError("SB-POLICY-PACK-PROFILES-UNKNOWN",
                               "policy profile area coverage");
  }
  if (image.role_records < 5 || image.group_records < 6 ||
      image.grant_records == 0 || image.policy_profile_records == 0) {
    return PolicyPackLoadError("SB-POLICY-PACK-MATERIALIZATION-INCOMPLETE",
                               "role/group/grant/profile counts");
  }

  return PolicyPackLoadOk(std::move(loaded));
}

CatalogRowsBuildResult AddTypedCatalogRecord(std::vector<CatalogPageRow>* rows,
                                             CatalogRecordKind kind,
                                             u32* ordinal,
                                             u64 identity_seed,
                                             std::string payload,
                                             TypedUuid parent_object_uuid = {},
                                             TypedUuid explicit_object_uuid = {}) {
  const auto descriptor = LookupCatalogRecordDescriptor(kind);
  if (!descriptor.ok()) {
    return CatalogRowsBuildError(descriptor.status, descriptor.diagnostic);
  }

  const auto row_uuid = GenerateEngineIdentityV7(UuidKind::row, identity_seed);
  if (!row_uuid.ok()) {
    return CatalogRowsBuildError(row_uuid.status, row_uuid.diagnostic);
  }

  CatalogTypedRecord record;
  record.header.kind = kind;
  record.header.row_uuid = row_uuid.value;
  if (descriptor.descriptor.requires_object_uuid) {
    if (explicit_object_uuid.valid()) {
      record.header.object_uuid = explicit_object_uuid;
    } else {
      const auto object_uuid = GenerateEngineIdentityV7(UuidKind::object, identity_seed + 1);
      if (!object_uuid.ok()) {
        return CatalogRowsBuildError(object_uuid.status, object_uuid.diagnostic);
      }
      record.header.object_uuid = object_uuid.value;
    }
  }
  record.header.parent_uuid = parent_object_uuid;
  if (payload.find("creator_tx=") == std::string::npos) {
    payload = std::string("creator_tx=") + std::to_string(kBootstrapCatalogTransactionId) + "\n" + payload;
  }
  record.payload = std::move(payload);

  const auto encoded = EncodeCatalogTypedRecord(record, (*ordinal)++);
  if (!encoded.ok()) {
    return CatalogRowsBuildError(encoded.status, encoded.diagnostic);
  }
  rows->push_back(encoded.row);

  CatalogRowsBuildResult result;
  result.status = DatabaseLifecycleOkStatus();
  return result;
}

CatalogRowsBuildResult AddLocalizedRecordPair(std::vector<CatalogPageRow>* rows,
                                              u32* ordinal,
                                              u64 identity_seed,
                                              TypedUuid target_object_uuid,
                                              std::string language_tag,
                                              std::string path,
                                              std::string name,
                                              std::string comment) {
  const std::string name_payload = KeyValuePayload({{"target_object_uuid", scratchbird::core::uuid::UuidToString(target_object_uuid.value)},
                                                    {"language", language_tag},
                                                    {"path", path},
                                                    {"name", name},
                                                    {"name_class", "default_name"}});
  const auto name_record = AddTypedCatalogRecord(rows,
                                                 CatalogRecordKind::localized_name,
                                                 ordinal,
                                                 identity_seed,
                                                 name_payload,
                                                 target_object_uuid);
  if (!name_record.ok()) {
    return name_record;
  }

  const std::string comment_payload = KeyValuePayload({{"target_object_uuid", scratchbird::core::uuid::UuidToString(target_object_uuid.value)},
                                                       {"language", std::move(language_tag)},
                                                       {"comment", std::move(comment)}});
  return AddTypedCatalogRecord(rows,
                               CatalogRecordKind::localized_comment,
                               ordinal,
                               identity_seed + 2,
                               comment_payload,
                               target_object_uuid);
}

const char* BoolText(bool value) {
  return value ? "1" : "0";
}

CatalogRowsBuildResult ParsePolicyObjectUuid(const std::string& text,
                                             TypedUuid* out) {
  const auto parsed = ParseTypedUuid(UuidKind::object, text);
  if (!parsed.ok()) {
    return CatalogRowsBuildError(parsed.status, parsed.diagnostic);
  }
  *out = parsed.value;
  CatalogRowsBuildResult result;
  result.status = DatabaseLifecycleOkStatus();
  return result;
}

CatalogRowsBuildResult MaterializePolicySeedPackRows(std::vector<CatalogPageRow>* rows,
                                                     u32* ordinal,
                                                     u64 identity_seed,
                                                     TypedUuid database_object_uuid,
                                                     TypedUuid security_parent_uuid,
                                                     const LoadedPolicySeedPack& pack) {
  if (!pack.image.active) {
    CatalogRowsBuildResult result;
    result.status = DatabaseLifecycleOkStatus();
    return result;
  }

  const std::string pack_payload =
      KeyValuePayload({{"policy_pack_id", pack.image.policy_pack_id},
                       {"policy_pack_uuid", pack.image.policy_pack_uuid},
                       {"policy_pack_version", pack.image.policy_pack_version},
                       {"schema_version", std::to_string(pack.image.schema_version)},
                       {"policy_generation", std::to_string(pack.image.policy_generation)},
                       {"content_sha256", pack.image.content_sha256},
                       {"signature_status", pack.image.signature_status},
                       {"create_time_only", BoolText(pack.image.create_time_only)},
                       {"post_create_filesystem_authority", BoolText(pack.image.post_create_filesystem_authority)},
                       {"local_password_only", BoolText(pack.image.local_password_only)},
                       {"materialized_inside_create_transaction",
                        BoolText(pack.image.materialized_inside_create_transaction)},
                       {"requires_mga_catalog_commit", BoolText(pack.image.requires_mga_catalog_commit)},
                       {"security_provider_records", std::to_string(pack.image.security_provider_records)},
                       {"enabled_local_password_provider_records",
                        std::to_string(pack.image.enabled_local_password_provider_records)},
                       {"external_provider_disabled_records",
                        std::to_string(pack.image.external_provider_disabled_records)},
                       {"role_records", std::to_string(pack.image.role_records)},
                       {"group_records", std::to_string(pack.image.group_records)},
                       {"group_membership_records", std::to_string(pack.image.group_membership_records)},
                       {"grant_records", std::to_string(pack.image.grant_records)},
                       {"policy_profile_records", std::to_string(pack.image.policy_profile_records)},
                       {"loaded_at_database_create", "1"},
                       {"created_txn", std::to_string(kBootstrapCatalogTransactionId)},
                       {"uuid_source", "policy_pack_manifest"},
                       {"mga_visibility_required", "1"},
                       {"filesystem_pack_not_post_create_authority", "1"}});
  rows->push_back(Row(CatalogPageRowKind::policy_seed_pack, (*ordinal)++, pack_payload));

  TypedUuid policy_pack_uuid;
  auto parsed_uuid = ParsePolicyObjectUuid(pack.image.policy_pack_uuid, &policy_pack_uuid);
  if (!parsed_uuid.ok()) {
    return parsed_uuid;
  }
  auto typed = AddTypedCatalogRecord(rows,
                                     CatalogRecordKind::policy,
                                     ordinal,
                                     identity_seed,
                                     KeyValuePayload({{"policy_key", "policy_pack." + pack.image.policy_pack_id},
                                                      {"policy_name", pack.image.policy_pack_id},
                                                      {"policy_class", "policy_pack"},
                                                      {"policy_pack_uuid", pack.image.policy_pack_uuid},
                                                      {"policy_pack_version", pack.image.policy_pack_version},
                                                      {"content_sha256", pack.image.content_sha256},
                                                      {"signature_status", pack.image.signature_status},
                                                      {"policy_generation", std::to_string(pack.image.policy_generation)},
                                                      {"created_txn", std::to_string(kBootstrapCatalogTransactionId)},
                                                      {"create_time_only", BoolText(pack.image.create_time_only)},
                                                      {"post_create_filesystem_authority",
                                                       BoolText(pack.image.post_create_filesystem_authority)},
                                                      {"materialized_inside_create_transaction",
                                                       BoolText(pack.image.materialized_inside_create_transaction)},
                                                      {"requires_mga_catalog_commit",
                                                       BoolText(pack.image.requires_mga_catalog_commit)},
                                                      {"loaded_at_database_create", "1"},
                                                      {"engine_owned", "1"}}),
                                     database_object_uuid,
                                     policy_pack_uuid);
  if (!typed.ok()) { return typed; }

  u64 seed = identity_seed + 100;
  for (const auto& provider : pack.providers) {
    TypedUuid provider_uuid;
    parsed_uuid = ParsePolicyObjectUuid(provider.provider_uuid, &provider_uuid);
    if (!parsed_uuid.ok()) { return parsed_uuid; }
    typed = AddTypedCatalogRecord(
        rows,
        CatalogRecordKind::policy,
        ordinal,
        seed,
        KeyValuePayload({{"policy_key", "security.provider." + provider.provider_family},
                         {"policy_name", "security.provider." + provider.provider_family},
                         {"policy_class", "security_provider"},
                         {"provider_uuid", provider.provider_uuid},
                         {"provider_family", provider.provider_family},
                         {"enabled_by_default", BoolText(provider.enabled_by_default)},
                         {"authority", provider.authority},
                         {"credential_verifier_policy", provider.credential_verifier_policy},
                         {"external_provider", BoolText(provider.external_provider)},
                         {"policy_pack_uuid", pack.image.policy_pack_uuid},
                         {"policy_generation", std::to_string(pack.image.policy_generation)},
                         {"created_txn", std::to_string(kBootstrapCatalogTransactionId)},
                         {"loaded_at_database_create", "1"},
                         {"default_provider_mode", "local-password-only"},
                         {"engine_owned", "1"}}),
        security_parent_uuid,
        provider_uuid);
    if (!typed.ok()) { return typed; }
    seed += 4;
  }

  for (const auto& role : pack.roles) {
    TypedUuid role_uuid;
    parsed_uuid = ParsePolicyObjectUuid(role.role_uuid, &role_uuid);
    if (!parsed_uuid.ok()) { return parsed_uuid; }
    typed = AddTypedCatalogRecord(
        rows,
        CatalogRecordKind::role_account,
        ordinal,
        seed,
        KeyValuePayload({{"role_uuid", role.role_uuid},
                         {"role_code", role.role_code},
                         {"description", role.description},
                         {"default_assignment", role.default_assignment},
                         {"policy_pack_uuid", pack.image.policy_pack_uuid},
                         {"policy_generation", std::to_string(pack.image.policy_generation)},
                         {"created_txn", std::to_string(kBootstrapCatalogTransactionId)},
                         {"loaded_at_database_create", "1"},
                         {"identity_authority", "uuid"},
                         {"engine_owned", "1"}}),
        security_parent_uuid,
        role_uuid);
    if (!typed.ok()) { return typed; }
    seed += 4;
  }

  for (const auto& group : pack.groups) {
    TypedUuid group_uuid;
    parsed_uuid = ParsePolicyObjectUuid(group.group_uuid, &group_uuid);
    if (!parsed_uuid.ok()) { return parsed_uuid; }
    typed = AddTypedCatalogRecord(
        rows,
        CatalogRecordKind::group_account,
        ordinal,
        seed,
        KeyValuePayload({{"group_uuid", group.group_uuid},
                         {"group_code", group.group_code},
                         {"description", group.description},
                         {"policy_pack_uuid", pack.image.policy_pack_uuid},
                         {"policy_generation", std::to_string(pack.image.policy_generation)},
                         {"created_txn", std::to_string(kBootstrapCatalogTransactionId)},
                         {"loaded_at_database_create", "1"},
                         {"identity_authority", "uuid"},
                         {"engine_owned", "1"}}),
        security_parent_uuid,
        group_uuid);
    if (!typed.ok()) { return typed; }
    seed += 4;
  }

  for (const auto& membership : pack.memberships) {
    typed = AddTypedCatalogRecord(
        rows,
        CatalogRecordKind::grant_record,
        ordinal,
        seed,
        KeyValuePayload({{"grant_class", "group_membership"},
                         {"member_uuid", membership.member_uuid},
                         {"member_kind", membership.member_kind},
                         {"parent_uuid", membership.parent_uuid},
                         {"parent_kind", membership.parent_kind},
                         {"effect", "allow"},
                         {"policy_pack_uuid", pack.image.policy_pack_uuid},
                         {"policy_generation", std::to_string(pack.image.policy_generation)},
                         {"created_txn", std::to_string(kBootstrapCatalogTransactionId)},
                         {"loaded_at_database_create", "1"},
                         {"identity_authority", "uuid"},
                         {"engine_owned", "1"}}),
        security_parent_uuid);
    if (!typed.ok()) { return typed; }
    seed += 4;
  }

  for (const auto& grant : pack.grants) {
    typed = AddTypedCatalogRecord(
        rows,
        CatalogRecordKind::grant_record,
        ordinal,
        seed,
        KeyValuePayload({{"grant_uuid", grant.grant_uuid},
                         {"grant_class", "privilege"},
                         {"subject_uuid", grant.subject_uuid},
                         {"subject_kind", grant.subject_kind},
                         {"target_uuid", grant.target_uuid},
                         {"right", grant.right},
                         {"effect", grant.effect},
                         {"policy_pack_uuid", pack.image.policy_pack_uuid},
                         {"policy_generation", std::to_string(pack.image.policy_generation)},
                         {"created_txn", std::to_string(kBootstrapCatalogTransactionId)},
                         {"loaded_at_database_create", "1"},
                         {"identity_authority", "uuid"},
                         {"engine_owned", "1"}}),
        security_parent_uuid);
    if (!typed.ok()) { return typed; }
    seed += 4;
  }

  for (const auto& profile : pack.profiles) {
    TypedUuid profile_uuid;
    parsed_uuid = ParsePolicyObjectUuid(profile.profile_uuid, &profile_uuid);
    if (!parsed_uuid.ok()) { return parsed_uuid; }
    typed = AddTypedCatalogRecord(
        rows,
        CatalogRecordKind::policy,
        ordinal,
        seed,
        KeyValuePayload({{"policy_key", "policy.profile." + profile.area},
                         {"policy_name", profile.area},
                         {"policy_class", "policy_profile"},
                         {"profile_uuid", profile.profile_uuid},
                         {"area", profile.area},
                         {"mode", profile.mode},
                         {"policy_pack_uuid", pack.image.policy_pack_uuid},
                         {"policy_generation", std::to_string(pack.image.policy_generation)},
                         {"created_txn", std::to_string(kBootstrapCatalogTransactionId)},
                         {"loaded_at_database_create", "1"},
                         {"identity_authority", "uuid"},
                         {"engine_owned", "1"}}),
        database_object_uuid,
        profile_uuid);
    if (!typed.ok()) { return typed; }
    seed += 4;
  }

  CatalogRowsBuildResult result;
  result.status = DatabaseLifecycleOkStatus();
  return result;
}

struct BootstrapRecordSeed {
  CatalogRecordKind kind = CatalogRecordKind::unknown;
  std::string payload;
};

struct DefaultPolicySeed {
  const char* policy_key;
  const char* default_profile;
  const char* state;
  const char* override_class;
  const char* required_properties;
};

std::string PolicyClassFromKey(const char* policy_key) {
  const std::string key(policy_key == nullptr ? "" : policy_key);
  const std::size_t split = key.find('.');
  return split == std::string::npos ? key : key.substr(0, split);
}

std::vector<BootstrapRecordSeed> DefaultRegistryPolicyRecords() {
  static constexpr std::array<DefaultPolicySeed, 58> kPolicies = {{
      {"policy.catalog.bootstrap", "strict_v1", "enabled", "no_override", "generation_start,missing_policy,unknown_policy,mutation_requires_audit,policy_rows_uuidv7"},
      {"database.identity", "uuidv7_local_v1", "enabled", "no_override", "database_uuid,row_uuid,name_uuid_registry,uuid_order_not_finality"},
      {"database.create.failure_cleanup", "remove_or_quarantine_v1", "enabled", "no_override", "on_tx1_failure,preserve_evidence,guess_identity=false"},
      {"database.bootstrap.tx1", "system_structure_seed_v1", "enabled", "no_override", "tx_number,sys_schema,users_schema,users_public,remote_schema,emulated_schema,cluster_schema"},
      {"database.first_open.tx2_activation", "runtime_activation_v1", "enabled", "no_override", "tx_number,start_agents_after_tx2,start_ipc_after_policy_load,ordinary_work_before_tx2"},
      {"schema.bootstrap.roots", "local_roots_v1", "enabled", "no_override", "roots,cluster_roots,all_objects_uuidv7,common_name_uuid_tables"},
      {"security.authority_selection", "database_local_internal_v1", "enabled", "create_database_only", "authority_class,fallback_security_database,no_security_database_policy,embedded_bootstrap_allowed"},
      {"security.authentication_provider", "engine_hash_provider_v1", "enabled", "security_admin", "plugin_required,cleartext_password_storage,compare_password_hash,provider_failure"},
      {"security.bootstrap_password", "must_change_or_replace_v1", "enabled", "security_admin", "initial_sysarch_equivalent,password_policy,default_password_allowed,hash_only"},
      {"security.authorization_default", "default_deny_explicit_allow_v1", "enabled", "security_admin", "default_action,grant_sources,deny_rules_supported,hidden_object_disclosure"},
      {"security.principal_role_group_seed", "sysarch_public_user_home_v1", "enabled", "no_override", "bootstrap_sysarch_equivalent,public_group,users_public_schema,new_user_home_schema"},
      {"security.user_home_schema", "users_tree_default_v1", "enabled", "create_database_only", "home_root,default_home_path,allow_alternate_home_root,cluster_users"},
      {"security.audit", "security_activity_audit_v1", "enabled", "security_admin", "audit_security_events,audit_policy_mutation,audit_create_database,tamper_evidence,failure_behavior"},
      {"security.redaction", "least_disclosure_v1", "enabled", "security_admin", "client_default,operator_requires_right,protected_material,parser_sql_text"},
      {"security.protected_material", "reference_only_v1", "enabled", "sysarch", "store_secret_material,secret_refs_allowed,key_release_requires_authority,diagnostic_redaction"},
      {"security.encryption_key_admission", "unencrypted_default_key_required_if_encrypted_v1", "enabled", "create_database_only", "database_encryption_default,filespace_key_from_database,encrypted_open_without_key,key_cache_purge_on_shutdown"},
      {"configuration.source_precedence", "compiled_then_durable_v1", "enabled", "no_override", "pre_mount_order,post_mount_durable_wins,silent_fallback_on_invalid,unknown_key"},
      {"configuration.override_reload", "safe_reload_v1", "enabled", "sysarch", "bootstrap_override_after_mount,reload_requires_generation,stale_policy,unsafe_reload"},
      {"resource.seed_i18n", "required_seed_v1", "enabled", "create_database_only", "timezone,charset,collation,locale,missing_resource_blocks_ordinary_open"},
      {"resource.signature_provenance", "unsigned_local_seed_allowed_v1", "enabled", "create_database_only", "pack_signature_required,source_hash_required,version_required,unsupported_version"},
      {"storage.filespace_profile", "single_active_primary_v1", "enabled", "create_database_only", "first_filespace_uuid,active_primary_required,secondary_filespaces,path_not_identity"},
      {"storage.filespace_lifecycle", "strict_identity_v1", "enabled", "sysarch", "stale_missing_duplicate,verify_no_repair_without_authority,drop_active_pins,quarantine_on_ambiguous"},
      {"storage.allocation_freespace_pagemap", "durable_map_v1", "enabled", "no_override", "free_space_map,page_ownership,reusable_space_after_mga_cleanup,physical_order_not_authority"},
      {"lifecycle.ownership_stale_owner", "exclusive_owner_v1", "enabled", "no_override", "single_owner,heartbeat_required,ambiguous_owner,stale_proof_required"},
      {"lifecycle.recovery_dirty_open", "mga_recovery_first_v1", "enabled", "sysarch", "clean_open,dirty_open,ambiguous,corrupt,wal_not_authority"},
      {"lifecycle.maintenance_restricted", "authorized_fence_v1", "enabled", "sysarch", "enter_requires_authority,ordinary_attach_blocked,verify_allowed,repair_requires_explicit_authority"},
      {"lifecycle.shutdown_graceful_drain", "drain_then_close_v1", "enabled", "sysarch", "default_drain_ms,fence_new_work,notify_associated_components,close_after_commit_or_rollback,timeout_without_force"},
      {"lifecycle.shutdown_force", "explicit_force_only_v1", "enabled", "sysarch", "implicit_escalation,terminate_target_database_scope_only,preserve_mga_recovery_evidence,unrelated_database_protected"},
      {"transaction.admission", "engine_mga_admission_v1", "enabled", "no_override", "requires_ownership,requires_security,requires_policy_generation,requires_catalog_snapshot,requires_filespace_valid"},
      {"transaction.default_isolation_snapshot", "read_committed_mga_snapshot_v1", "enabled", "create_database_only", "default_isolation,snapshot_source,security_epoch_captured,catalog_epoch_captured"},
      {"transaction.commit_durability", "inventory_sync_v1", "enabled", "no_override", "commit_authority,success_after_sync_policy,wal_finality,cache_flush_not_finality"},
      {"transaction.rollback_savepoint_limbo", "mga_owned_v1", "enabled", "no_override", "rollback_engine_owned,savepoints_transaction_local,disconnect_unknown_outcome,limbo_requires_recovery_policy"},
      {"transaction.mga_gc_retention", "safe_bounded_cleanup_v1", "enabled", "sysarch", "cleanup_requires_horizon,backup_hold_respected,archive_hold_respected,unknown_outcome_protected,bounded_memory"},
      {"concurrency.lock_wait_deadlock", "bounded_wait_v1", "enabled", "policy_defined", "default_lock_wait_ms,deadlock_detection,disconnect_cleanup,victim_policy"},
      {"cache.checkpoint_preload_flush", "evidence_not_finality_v1", "enabled", "no_override", "preload_after_tx2,flush_on_shutdown,checkpoint_is_clean_close_evidence_only,cache_not_finality"},
      {"backup.archive_restore_snapshot_shadow", "engine_owned_no_live_shortcut_v1", "enabled", "sysarch", "backup_requires_engine_path,live_file_shortcut,restore_inspection_mode,legal_hold_respected"},
      {"workload.resource_quota", "safe_local_default_v1", "enabled", "policy_defined", "max_connections,max_active_requests,max_open_cursors_per_session,memory_pressure"},
      {"temp.spill_workspace", "bounded_cleanup_v1", "enabled", "policy_defined", "temp_catalog_durable,cleanup_on_commit,cleanup_on_rollback,cleanup_on_disconnect,spill_encryption"},
      {"session.disconnect_timeout", "explicit_unknown_outcome_v1", "enabled", "policy_defined", "idle_timeout_ms,statement_timeout_ms,disconnect_does_not_commit,unknown_outcome_message_vector"},
      {"server.route_listener_startup", "local_disabled_until_configured_v1", "enabled", "create_database_only", "network_listener_default,loopback_default,native_port,start_after_security_policy"},
      {"listener.bind_tls_pool", "secure_bind_v1", "enabled", "policy_defined", "tls_required_for_inet,unix_socket_allowed_local,parser_pool_min,parser_pool_max,reuseaddr_policy"},
      {"parser.package_admission", "registered_packages_only_v1", "enabled", "policy_defined", "unregistered_parser,sbsql_profile,reference_profile,parser_auth_authority"},
      {"ipc.frame_auth_backpressure", "authenticated_framed_v1", "enabled", "policy_defined", "max_frame_bytes,malformed_frame,backpressure,endpoint_descriptor_required"},
      {"udr.extension_trust_resource", "cxx_registered_only_v1", "enabled", "policy_defined", "trusted_udr_language,dynamic_load_requires_policy,resource_limits,unload_or_quiesce_required"},
      {"executable.side_effect", "side_effects_disabled_by_default_v1", "enabled", "policy_defined", "routine_side_effects,trigger_side_effects,external_outbox_requires_policy,definer_rights"},
      {"sequence.generator_cache", "bounded_nonfinality_cache_v1", "enabled", "policy_defined", "default_cache_size,cache_not_transaction_finality,crash_gaps_allowed,reference_mapping_requires_profile"},
      {"event.queue_notification", "bounded_volatile_default_v1", "enabled", "policy_defined", "max_payload_bytes,max_queued_events,retention_seconds,overflow_behavior,security_filtering"},
      {"diagnostics.message_vector", "canonical_redacted_v1", "enabled", "no_override", "raw_strings_forbidden,redaction_required,reference_mapping_generic_on_failure,correlation_id_required"},
      {"observability.metrics_log", "local_metrics_enabled_v1", "enabled", "policy_defined", "metrics_enabled,flush_interval_ms,local_root,cluster_metrics_absent_without_cluster,private_paths_redacted"},
      {"support.bundle", "disabled_until_authorized_v1", "enabled", "sysarch", "default_enabled,requires_operator_right,redaction_required,retention_days,protected_material_excluded"},
      {"evidence.retention", "audit_minimum_v1", "enabled", "security_admin", "lifecycle_evidence_days,security_audit_days,diagnostic_evidence_days,legal_hold_overrides_cleanup"},
      {"job.scheduler", "start_after_tx2_v1", "enabled", "policy_defined", "normal_jobs_after_tx2,startup_recovery_jobs_first,maintenance_participation,failure_policy,retry_backoff_ms"},
      {"capability.feature_gate", "installed_enabled_else_fail_closed_v1", "enabled", "sysarch", "unknown_capability,edition_gate_required,parser_profile_requires_package,downgrade_refusal"},
      {"upgrade.migration_refusal", "explicit_supported_only_v1", "enabled", "sysarch", "unknown_format,ambiguous_identity,supported_migration_requires_plan,guess_identity=false"},
      {"admin.management_command_authorization", "sysarch_or_delegated_v1", "enabled", "sysarch", "lifecycle_commands_require_authority,force_shutdown_requires_explicit_right,inspect_redacted_by_default,audit_required"},
      {"reference.emulation_profile", "strict_not_authority_v1", "enabled", "no_override", "reference_sql_exec_inside_engine,unsupported_reference_feature,reference_catalog_overlay_not_authority,cross_dialect_dependency"},
      {"replication.cdc_changefeed_boundary", "disabled_fail_closed_v1", "fail_closed", "cluster_only", "replication_enabled,cdc_enabled,changefeed_enabled,live_ingest_enabled,slot_create,publication_create"},
      {"cluster.boundary_fail_closed", "standalone_fail_closed_v1", "enabled", "cluster_only", "cluster_schema_created,cluster_metrics_created,cluster_routes,cluster_transactions,cluster_agents"},
  }};

  std::vector<BootstrapRecordSeed> records;
  records.reserve(kPolicies.size());
  for (const auto& policy : kPolicies) {
    records.push_back({CatalogRecordKind::policy,
                       KeyValuePayload({{"policy_key", policy.policy_key},
                                        {"policy_name", policy.policy_key},
                                        {"policy_class", PolicyClassFromKey(policy.policy_key)},
                                        {"default_profile", policy.default_profile},
                                        {"state", policy.state},
                                        {"override_class", policy.override_class},
                                        {"required_properties", policy.required_properties},
                                        {"policy_generation", "1"},
                                        {"created_txn", std::to_string(kBootstrapCatalogTransactionId)},
                                        {"tx1_seed_required", "1"},
                                        {"uuid_source", "fresh_uuidv7"},
                                        {"mga_visibility_required", "1"},
                                        {"engine_owned", "1"}})});
  }
  return records;
}

std::vector<BootstrapRecordSeed> DefaultBootstrapCatalogRecords(const DatabaseCreateConfig& config) {
  std::vector<BootstrapRecordSeed> records = DefaultRegistryPolicyRecords();
  const std::array<BootstrapRecordSeed, 15> additional_records = {{
      {CatalogRecordKind::config_profile, KeyValuePayload({{"profile_name", "default_local_node_profile"}, {"scope", "local_database"}, {"unsafe_combinations_fail_closed", "1"}})},
      {CatalogRecordKind::user_account, KeyValuePayload({{"principal_name", "ROOT"}, {"account_class", "break_glass"}, {"enabled", "0"}})},
      {CatalogRecordKind::group_account, KeyValuePayload({{"group_name", "PUBLIC"}, {"ambient_rights", "minimal"}, {"connect_only", "1"}})},
      {CatalogRecordKind::group_account, KeyValuePayload({{"group_name", "DBA"}, {"operational_role", "database_administration"}, {"created_disabled", "1"}})},
      {CatalogRecordKind::role_account, KeyValuePayload({{"role_name", "ROOT_ROLE"}, {"break_glass", "1"}, {"created_disabled", "1"}})},
      {CatalogRecordKind::grant_record, KeyValuePayload({{"grant_name", "PUBLIC_CONNECT_BASELINE"}, {"target", "PUBLIC"}, {"right", "CONNECT"}, {"ambient", "0"}})},
      {CatalogRecordKind::masking_policy, KeyValuePayload({{"policy_name", "default_no_unmask_without_grant"}, {"unmask_requires_explicit_right", "1"}})},
      {CatalogRecordKind::rls_policy, KeyValuePayload({{"policy_name", "default_rls_engine_authority"}, {"parser_authority", "0"}, {"fail_closed", "1"}})},
      {CatalogRecordKind::udr_package, KeyValuePayload({{"package_name", "system_builtin_udr_registry"}, {"trusted_engine_side", "1"}, {"registered", "1"}})},
      {CatalogRecordKind::parser_package, KeyValuePayload({{"package_name", "sbsql_parser_package"}, {"legacy_alias", "native_v3_parser_package"}, {"trusted", "0"}, {"one_instance_per_connection", "1"}})},
      {CatalogRecordKind::sblr_module, KeyValuePayload({{"module_name", "system_bootstrap_sblr"}, {"contains_sql_text", "0"}, {"engine_validated", "1"}})},
      {CatalogRecordKind::storage_descriptor, KeyValuePayload({{"descriptor_name", "default_storage_profile"}, {"page_size", std::to_string(config.page_size)}, {"filespace_uuid", scratchbird::core::uuid::UuidToString(config.filespace_uuid.value)}})},
      {CatalogRecordKind::table_descriptor, KeyValuePayload({{"table_name", "sys.catalog.bootstrap_objects"}, {"system_table", "1"}, {"typed_catalog_records", "1"}})},
      {CatalogRecordKind::index_descriptor, KeyValuePayload({{"index_name", "sys.catalog.bootstrap_object_uuid_idx"}, {"system_index", "1"}, {"key", "object_uuid"}})},
      {CatalogRecordKind::toast_reference, KeyValuePayload({{"reference_name", "resource_seed_large_value_policy"}, {"page_type", "blob"}, {"content_addressed", "1"}})},
  }};
  records.insert(records.end(), additional_records.begin(), additional_records.end());
  return records;
}

CatalogRowsBuildResult BuildCreateCatalogRows(const DatabaseCreateConfig& config,
                                              const ResourceSeedCatalogImage& image,
                                              const LoadedPolicySeedPack& policy_seed_pack) {
  CatalogRowsBuildResult result;
  result.status = DatabaseLifecycleOkStatus();
  u32 ordinal = 1;

  const auto database_object = GenerateEngineIdentityV7(UuidKind::object, config.creation_unix_epoch_millis + 10000);
  const auto filespace_object = GenerateEngineIdentityV7(UuidKind::object, config.creation_unix_epoch_millis + 10002);
  const auto sys_object = GenerateEngineIdentityV7(UuidKind::object, config.creation_unix_epoch_millis + 10020);
  const auto sys_catalog_object = GenerateEngineIdentityV7(UuidKind::object, config.creation_unix_epoch_millis + 10021);
  const auto sys_metrics_object = GenerateEngineIdentityV7(UuidKind::object, config.creation_unix_epoch_millis + 10022);
  const auto sys_agents_object = GenerateEngineIdentityV7(UuidKind::object, config.creation_unix_epoch_millis + 10023);
  const auto sys_security_object = GenerateEngineIdentityV7(UuidKind::object, config.creation_unix_epoch_millis + 10024);
  const auto sys_configuration_object = GenerateEngineIdentityV7(UuidKind::object, config.creation_unix_epoch_millis + 10025);
  const auto sys_management_object = GenerateEngineIdentityV7(UuidKind::object, config.creation_unix_epoch_millis + 10026);
  const auto sys_fn_object = GenerateEngineIdentityV7(UuidKind::object, config.creation_unix_epoch_millis + 10027);
  const auto sys_udr_object = GenerateEngineIdentityV7(UuidKind::object, config.creation_unix_epoch_millis + 10028);
  const auto sys_parser_object = GenerateEngineIdentityV7(UuidKind::object, config.creation_unix_epoch_millis + 10029);
  const auto sys_storage_object = GenerateEngineIdentityV7(UuidKind::object, config.creation_unix_epoch_millis + 10030);
  const auto sys_mga_object = GenerateEngineIdentityV7(UuidKind::object, config.creation_unix_epoch_millis + 10031);
  const auto sys_audit_object = GenerateEngineIdentityV7(UuidKind::object, config.creation_unix_epoch_millis + 10032);
  const auto sys_compatibility_object = GenerateEngineIdentityV7(UuidKind::object, config.creation_unix_epoch_millis + 10033);
  const auto sys_information_object = GenerateEngineIdentityV7(UuidKind::object, config.creation_unix_epoch_millis + 10034);
  const auto sys_information_schema_synonym_object = GenerateEngineIdentityV7(UuidKind::object, config.creation_unix_epoch_millis + 10035);
  const auto sys_catalog_readable_object = GenerateEngineIdentityV7(UuidKind::object, config.creation_unix_epoch_millis + 10036);
  const auto sys_diagnostics_object = GenerateEngineIdentityV7(UuidKind::object, config.creation_unix_epoch_millis + 10037);
  const auto users_object = GenerateEngineIdentityV7(UuidKind::object, config.creation_unix_epoch_millis + 10050);
  const auto users_public_object = GenerateEngineIdentityV7(UuidKind::object, config.creation_unix_epoch_millis + 10051);
  const auto remote_object = GenerateEngineIdentityV7(UuidKind::object, config.creation_unix_epoch_millis + 10060);
  const auto emulated_object = GenerateEngineIdentityV7(UuidKind::object, config.creation_unix_epoch_millis + 10070);
  if (!database_object.ok()) { return CatalogRowsBuildError(database_object.status, database_object.diagnostic); }
  if (!filespace_object.ok()) { return CatalogRowsBuildError(filespace_object.status, filespace_object.diagnostic); }
  if (!sys_object.ok()) { return CatalogRowsBuildError(sys_object.status, sys_object.diagnostic); }
  if (!sys_catalog_object.ok()) { return CatalogRowsBuildError(sys_catalog_object.status, sys_catalog_object.diagnostic); }
  if (!sys_metrics_object.ok()) { return CatalogRowsBuildError(sys_metrics_object.status, sys_metrics_object.diagnostic); }
  if (!sys_agents_object.ok()) { return CatalogRowsBuildError(sys_agents_object.status, sys_agents_object.diagnostic); }
  if (!sys_security_object.ok()) { return CatalogRowsBuildError(sys_security_object.status, sys_security_object.diagnostic); }
  if (!sys_configuration_object.ok()) { return CatalogRowsBuildError(sys_configuration_object.status, sys_configuration_object.diagnostic); }
  if (!sys_management_object.ok()) { return CatalogRowsBuildError(sys_management_object.status, sys_management_object.diagnostic); }
  if (!sys_fn_object.ok()) { return CatalogRowsBuildError(sys_fn_object.status, sys_fn_object.diagnostic); }
  if (!sys_udr_object.ok()) { return CatalogRowsBuildError(sys_udr_object.status, sys_udr_object.diagnostic); }
  if (!sys_parser_object.ok()) { return CatalogRowsBuildError(sys_parser_object.status, sys_parser_object.diagnostic); }
  if (!sys_storage_object.ok()) { return CatalogRowsBuildError(sys_storage_object.status, sys_storage_object.diagnostic); }
  if (!sys_mga_object.ok()) { return CatalogRowsBuildError(sys_mga_object.status, sys_mga_object.diagnostic); }
  if (!sys_audit_object.ok()) { return CatalogRowsBuildError(sys_audit_object.status, sys_audit_object.diagnostic); }
  if (!sys_compatibility_object.ok()) { return CatalogRowsBuildError(sys_compatibility_object.status, sys_compatibility_object.diagnostic); }
  if (!sys_information_object.ok()) { return CatalogRowsBuildError(sys_information_object.status, sys_information_object.diagnostic); }
  if (!sys_information_schema_synonym_object.ok()) { return CatalogRowsBuildError(sys_information_schema_synonym_object.status, sys_information_schema_synonym_object.diagnostic); }
  if (!sys_catalog_readable_object.ok()) { return CatalogRowsBuildError(sys_catalog_readable_object.status, sys_catalog_readable_object.diagnostic); }
  if (!sys_diagnostics_object.ok()) { return CatalogRowsBuildError(sys_diagnostics_object.status, sys_diagnostics_object.diagnostic); }
  if (!users_object.ok()) { return CatalogRowsBuildError(users_object.status, users_object.diagnostic); }
  if (!users_public_object.ok()) { return CatalogRowsBuildError(users_public_object.status, users_public_object.diagnostic); }
  if (!remote_object.ok()) { return CatalogRowsBuildError(remote_object.status, remote_object.diagnostic); }
  if (!emulated_object.ok()) { return CatalogRowsBuildError(emulated_object.status, emulated_object.diagnostic); }

  const std::string database_payload = KeyValuePayload({{"database_uuid", scratchbird::core::uuid::UuidToString(config.database_uuid.value)},
                                                        {"database_header_format_major", std::to_string(scratchbird::storage::disk::kScratchBirdDatabaseFormatMajor)},
                                                        {"database_header_format_minor", std::to_string(scratchbird::storage::disk::kScratchBirdDatabaseFormatMinor)},
                                                        {"catalog_manifest_format_version", std::to_string(kDatabaseCatalogManifestFormatCurrent)},
                                                        {"page_size", std::to_string(config.page_size)},
                                                        {"creation_unix_epoch_millis", std::to_string(config.creation_unix_epoch_millis)},
                                                        {"feature_flags", std::to_string(config.feature_flags)},
                                                        {"compatibility_flags", std::to_string(config.compatibility_flags)}});
  auto typed = AddTypedCatalogRecord(&result.rows,
                                     CatalogRecordKind::database,
                                     &ordinal,
                                     config.creation_unix_epoch_millis + 11000,
                                     database_payload,
                                     {},
                                     database_object.value);
  if (!typed.ok()) { return typed; }

  const std::string filespace_payload = KeyValuePayload({{"filespace_uuid", scratchbird::core::uuid::UuidToString(config.filespace_uuid.value)},
                                                         {"database_uuid", scratchbird::core::uuid::UuidToString(config.database_uuid.value)},
                                                         {"filespace_role", "active_primary"},
                                                         {"first_filespace", "1"},
                                                         {"startup_authority", "1"},
                                                         {"catalog_persistence_owner", "1"},
                                                         {"filespace_manifest_owner", "1"},
                                                         {"recovery_evidence_owner", "1"},
                                                         {"read_only", "0"},
                                                         {"state", "online"},
                                                         {"physical_filespace_id", "0"},
                                                         {"lifecycle_generation", "1"},
                                                         {"filespace_manifest_generation", "1"},
                                                         {"catalog_manifest_format_version", std::to_string(kDatabaseCatalogManifestFormatCurrent)},
                                                         {"resource_seed_manifest_format_version", std::to_string(kResourceSeedManifestFormatCurrent)},
                                                         {"registered_txn", std::to_string(kBootstrapCatalogTransactionId)},
                                                         {"last_lifecycle_transaction", std::to_string(kBootstrapCatalogTransactionId)},
                                                         {"uuid_source", "fresh_uuidv7"},
                                                         {"header_database_uuid_match_required", "1"},
                                                         {"header_filespace_uuid_match_required", "1"},
                                                         {"startup_state_coupled", "1"},
                                                         {"page_header_coupled", "1"},
                                                         {"open_validate_header", "1"},
                                                         {"attach_admission_validate_header", "1"},
                                                         {"transaction_admission_validate_filespace", "1"},
                                                         {"maintenance_validate_header", "1"},
                                                         {"verify_repair_validate_header", "1"},
                                                         {"shutdown_validate_header", "1"},
                                                         {"recovery_validate_header", "1"},
                                                         {"drop_requires_database_lifecycle", "1"},
                                                         {"quarantine_on_ambiguous", "1"},
                                                         {"state_change_evidence_before_success", "1"},
                                                         {"mga_visibility_required", "1"},
                                                         {"path_is_locator_not_identity", "1"},
                                                         {"duplicate_identity_refusal", "1"},
                                                         {"stale_identity_refusal", "1"}});
  typed = AddTypedCatalogRecord(&result.rows,
                                CatalogRecordKind::filespace,
                                &ordinal,
                                config.creation_unix_epoch_millis + 11010,
                                filespace_payload,
                                database_object.value,
                                filespace_object.value);
  if (!typed.ok()) { return typed; }

  struct BootstrapSchema {
    TypedUuid object_uuid;
    TypedUuid parent_uuid;
    const char* path;
    const char* name;
    bool root_schema = false;
  };
  const std::array<BootstrapSchema, 21> schemas = {{
      {sys_object.value, database_object.value, kLocalSysSchemaPath, "sys", true},
      {sys_catalog_object.value, sys_object.value, kLocalSysCatalogSchemaPath, "catalog", false},
      {sys_metrics_object.value, sys_object.value, kLocalSysMetricsSchemaPath, "metrics", false},
      {sys_agents_object.value, sys_object.value, kLocalSysAgentsSchemaPath, "agents", false},
      {sys_security_object.value, sys_object.value, kLocalSysSecuritySchemaPath, "security", false},
      {sys_configuration_object.value, sys_object.value, kLocalSysConfigurationSchemaPath, "configuration", false},
      {sys_management_object.value, sys_object.value, kLocalSysManagementSchemaPath, "management", false},
      {sys_fn_object.value, sys_object.value, kLocalSysFnSchemaPath, "fn", false},
      {sys_udr_object.value, sys_object.value, kLocalSysUdrSchemaPath, "udr", false},
      {sys_parser_object.value, sys_object.value, kLocalSysParserSchemaPath, "parser", false},
      {sys_storage_object.value, sys_object.value, kLocalSysStorageSchemaPath, "storage", false},
      {sys_mga_object.value, sys_object.value, kLocalSysMgaSchemaPath, "mga", false},
      {sys_audit_object.value, sys_object.value, kLocalSysAuditSchemaPath, "audit", false},
      {sys_compatibility_object.value, sys_object.value, kLocalSysCompatibilitySchemaPath, "compatibility", false},
      {sys_information_object.value, sys_object.value, kLocalSysInformationTrueSchemaPath, "information", false},
      {sys_catalog_readable_object.value, sys_object.value, kLocalSysCatalogReadableSchemaPath, "catalog_readable", false},
      {sys_diagnostics_object.value, sys_object.value, kLocalSysDiagnosticsSchemaPath, "diagnostics", false},
      {users_object.value, database_object.value, kLocalUsersSchemaPath, "users", true},
      {users_public_object.value, users_object.value, kLocalUsersPublicSchemaPath, "public", false},
      {remote_object.value, database_object.value, kLocalRemoteSchemaPath, "remote", true},
      {emulated_object.value, database_object.value, kLocalEmulatedSchemaPath, "emulated", true},
  }};
  u64 schema_seed = config.creation_unix_epoch_millis + 11100;
  for (const auto& schema : schemas) {
    const std::string schema_payload = KeyValuePayload({{"schema_object_uuid", scratchbird::core::uuid::UuidToString(schema.object_uuid.value)},
                                                        {"parent_object_uuid", scratchbird::core::uuid::UuidToString(schema.parent_uuid.value)},
                                                        {"path", schema.path},
                                                        {"name", schema.name},
                                                        {"root_schema", schema.root_schema ? "1" : "0"},
                                                        {"local_single_node_scope", "1"},
                                                        {"recursive_schema_tree", "1"}});
    typed = AddTypedCatalogRecord(&result.rows,
                                  CatalogRecordKind::schema,
                                  &ordinal,
                                  schema_seed,
                                  schema_payload,
                                  schema.parent_uuid,
                                  schema.object_uuid);
    if (!typed.ok()) { return typed; }

    const auto localized = AddLocalizedRecordPair(&result.rows,
                                                  &ordinal,
                                                  schema_seed + 1000,
                                                  schema.object_uuid,
                                                  "en",
                                                  schema.path,
                                                  schema.name,
                                                  std::string("ScratchBird bootstrap schema: ") + schema.path);
    if (!localized.ok()) { return localized; }
    schema_seed += 20;
  }

  const std::string information_schema_synonym_payload =
      KeyValuePayload({{"synonym_uuid", scratchbird::core::uuid::UuidToString(sys_information_schema_synonym_object.value.value)},
                       {"parent_object_uuid", scratchbird::core::uuid::UuidToString(sys_object.value.value)},
                       {"target_object_uuid", scratchbird::core::uuid::UuidToString(sys_information_object.value.value)},
                       {"target_object_class", "schema"},
                       {"dependency_kind", "synonym_hard:schema"},
                       {"path", kLocalSysInformationSchemaPath},
                       {"name", "information_schema"},
                       {"canonical_target_path", kLocalSysInformationTrueSchemaPath},
                       {"child_parent_remap_required", "1"},
                       {"second_schema_branch", "0"}});
  typed = AddTypedCatalogRecord(&result.rows,
                                CatalogRecordKind::synonym_descriptor,
                                &ordinal,
                                config.creation_unix_epoch_millis + 11190,
                                information_schema_synonym_payload,
                                sys_object.value,
                                sys_information_schema_synonym_object.value);
  if (!typed.ok()) { return typed; }
  const auto information_schema_synonym_localized =
      AddLocalizedRecordPair(&result.rows,
                             &ordinal,
                             config.creation_unix_epoch_millis + 11194,
                             sys_information_schema_synonym_object.value,
                             "en",
                             kLocalSysInformationSchemaPath,
                             "information_schema",
                             "Legacy SQL information schema synonym for sys.information");
  if (!information_schema_synonym_localized.ok()) { return information_schema_synonym_localized; }

  const auto database_localized = AddLocalizedRecordPair(&result.rows,
                                                        &ordinal,
                                                        config.creation_unix_epoch_millis + 11200,
                                                        database_object.value,
                                                        "en",
                                                        "database",
                                                        "database",
                                                        "ScratchBird bootstrap database object");
  if (!database_localized.ok()) { return database_localized; }

  u64 datatype_seed = config.creation_unix_epoch_millis + 12000;
  for (const auto& descriptor : BuiltinDatatypeDescriptors()) {
    const std::string datatype_payload = KeyValuePayload({{"type_id", std::to_string(static_cast<u32>(descriptor.type_id))},
                                                          {"stable_name", descriptor.stable_name},
                                                          {"canonical_name", CanonicalTypeName(descriptor.type_id)},
                                                          {"family", TypeFamilyName(descriptor.family)},
                                                          {"width_class", TypeWidthClassName(descriptor.width_class)},
                                                          {"bit_width", std::to_string(descriptor.bit_width)},
                                                          {"default_precision", std::to_string(descriptor.default_precision)},
                                                          {"default_scale", std::to_string(descriptor.default_scale)},
                                                          {"descriptor_authoritative", descriptor.descriptor_authoritative ? "1" : "0"},
                                                          {"reference_name_is_alias_only", descriptor.reference_name_is_alias_only ? "1" : "0"}});
    typed = AddTypedCatalogRecord(&result.rows,
                                  CatalogRecordKind::datatype_descriptor,
                                  &ordinal,
                                  datatype_seed,
                                  datatype_payload,
                                  sys_catalog_object.value);
    if (!typed.ok()) { return typed; }
    datatype_seed += 2;
  }

  u64 domain_seed = config.creation_unix_epoch_millis + 13000;
  for (const auto& descriptor : BuiltinDatatypeDescriptors()) {
    const std::string domain_payload = KeyValuePayload({{"domain_name", std::string("SYS_DOMAIN_") + CanonicalTypeName(descriptor.type_id)},
                                                        {"base_type_id", std::to_string(static_cast<u32>(descriptor.type_id))},
                                                        {"base_type_name", CanonicalTypeName(descriptor.type_id)},
                                                        {"system_domain", "1"},
                                                        {"descriptor_authoritative", "1"}});
    typed = AddTypedCatalogRecord(&result.rows,
                                  CatalogRecordKind::domain,
                                  &ordinal,
                                  domain_seed,
                                  domain_payload,
                                  sys_catalog_object.value);
    if (!typed.ok()) { return typed; }
    domain_seed += 2;
  }

  const auto bootstrap_records = DefaultBootstrapCatalogRecords(config);
  u64 bootstrap_seed = config.creation_unix_epoch_millis + 14000;
  for (const auto& bootstrap_record : bootstrap_records) {
    typed = AddTypedCatalogRecord(&result.rows,
                                  bootstrap_record.kind,
                                  &ordinal,
                                  bootstrap_seed,
                                  bootstrap_record.payload,
                                  database_object.value);
    if (!typed.ok()) { return typed; }
    bootstrap_seed += 4;
  }

  const auto policy_seed_rows = MaterializePolicySeedPackRows(&result.rows,
                                                              &ordinal,
                                                              config.creation_unix_epoch_millis + 26000,
                                                              database_object.value,
                                                              sys_security_object.value,
                                                              policy_seed_pack);
  if (!policy_seed_rows.ok()) {
    return policy_seed_rows;
  }

  // SEARCH_KEY: SB_METRICS_HISTORY_BOOTSTRAP_DATABASE_CREATE
  const std::array<std::string, 13> metric_history_surfaces = {{
      "sys.metrics.registry",
      "sys.metrics.descriptors",
      "sys.metrics.current",
      "sys.metrics.labels",
      "sys.metrics.producers",
      "sys.metrics.namespaces",
      "sys.metrics.series",
      "sys.metrics.history",
      "sys.metrics.rollups",
      "sys.metrics.retention_policies",
      "sys.metrics.retention_evidence",
      "sys.metrics.policy_bindings",
      "sys.metrics.bootstrap_seed",
  }};
  for (const auto& surface : metric_history_surfaces) {
    const std::string surface_payload = KeyValuePayload({{"table_name", surface},
                                                         {"system_table", "1"},
                                                         {"schema_path", "sys.metrics"},
                                                         {"metric_history_surface", "1"},
                                                         {"engine_owned", "1"}});
    typed = AddTypedCatalogRecord(&result.rows,
                                  CatalogRecordKind::table_descriptor,
                                  &ordinal,
                                  bootstrap_seed,
                                  surface_payload,
                                  sys_metrics_object.value);
    if (!typed.ok()) { return typed; }
    bootstrap_seed += 4;
  }

  for (const auto& policy : BaselineMetricRetentionPolicies()) {
    if (policy.scope == "cluster") {
      continue;
    }
    std::string grains;
    bool first_grain = true;
    for (const auto grain : policy.rollup_grains) {
      if (!first_grain) {
        grains += ",";
      }
      grains += MetricRollupGrainName(grain);
      first_grain = false;
    }
    const std::string policy_payload = KeyValuePayload({{"policy_name", policy.policy_name},
                                                        {"policy_uuid", policy.policy_uuid},
                                                        {"policy_class", "metric_retention"},
                                                        {"scope", policy.scope},
                                                        {"mode", MetricRetentionModeName(policy.mode)},
                                                        {"raw_retention_seconds", std::to_string(policy.raw_retention_seconds)},
                                                        {"rollup_retention_seconds", std::to_string(policy.rollup_retention_seconds)},
                                                        {"rollup_grains", grains},
                                                        {"purge_batch_limit", std::to_string(policy.purge_batch_limit)},
                                                        {"max_cardinality", std::to_string(policy.max_cardinality)},
                                                        {"overflow_behavior", policy.overflow_behavior},
                                                        {"edit_right", policy.edit_right},
                                                        {"default_admin_group", policy.default_admin_group},
                                                        {"evidence_required", policy.evidence_required ? "1" : "0"},
                                                        {"editable_seed_policy", "1"}});
    typed = AddTypedCatalogRecord(&result.rows,
                                  CatalogRecordKind::policy,
                                  &ordinal,
                                  bootstrap_seed,
                                  policy_payload,
                                  sys_metrics_object.value);
    if (!typed.ok()) { return typed; }
    bootstrap_seed += 4;
  }

  const auto metric_descriptors = DefaultMetricRegistry().Descriptors(false);
  for (const auto& descriptor : metric_descriptors) {
    const auto& retention_policy = DefaultMetricRetentionPolicyForDescriptor(descriptor);
    const std::string descriptor_payload =
        KeyValuePayload({{"metric_family", descriptor.family},
                         {"metric_type", MetricTypeName(descriptor.type)},
                         {"metric_unit", MetricUnitName(descriptor.unit)},
                         {"namespace_path", descriptor.namespace_path},
                         {"producer_owner", descriptor.producer_owner},
                         {"security_family", descriptor.security_family},
                         {"visibility_scope", MetricVisibilityScopeName(descriptor.visibility)},
                         {"readiness", MetricReadinessName(descriptor.readiness)},
                         {"cluster_only", descriptor.cluster_only ? "1" : "0"},
                         {"label_keys", JoinMetricLabelKeys(descriptor, false)},
                         {"sensitive_label_keys", JoinMetricLabelKeys(descriptor, true)},
                         {"aliases", JoinStrings(descriptor.aliases)},
                         {"histogram_buckets", JoinMetricBuckets(descriptor)},
                         {"retention_policy_name", retention_policy.policy_name},
                         {"retention_policy_uuid", retention_policy.policy_uuid},
                         {"seeded_at_database_create", "1"},
                         {"engine_owned", "1"}});
    typed = AddTypedCatalogRecord(&result.rows,
                                  CatalogRecordKind::metric_descriptor,
                                  &ordinal,
                                  bootstrap_seed,
                                  descriptor_payload,
                                  sys_metrics_object.value);
    if (!typed.ok()) { return typed; }
    bootstrap_seed += 4;
  }

  struct MetricCurrentSeed {
    std::string family;
    std::string value;
    std::string state_text;
    std::string label_set;
  };
  const std::string lifecycle_labels =
      std::string("database_uuid:") + scratchbird::core::uuid::UuidToString(config.database_uuid.value) +
      ",filespace_uuid:" + scratchbird::core::uuid::UuidToString(config.filespace_uuid.value) +
      ",component:database_lifecycle,operation:create,result:committed";
  const std::array<MetricCurrentSeed, 6> initial_metric_values = {{
      {"sb_tx_begin_total", "1", "", lifecycle_labels},
      {"sb_tx_commit_total", "1", "", lifecycle_labels},
      {"sb_tx_active_transactions", "0", "", lifecycle_labels},
      {"sb_mga_cleanup_horizon_local_transaction_id", std::to_string(kBootstrapCatalogTransactionId), "", lifecycle_labels},
      {"sb_metric_samples_rejected_total", "0", "", lifecycle_labels},
      {"sb_filespace_used_bytes", "0", "", lifecycle_labels},
  }};
  for (const auto& seed : initial_metric_values) {
    const auto* descriptor = DefaultMetricRegistry().FindDescriptor(seed.family);
    if (descriptor == nullptr || descriptor->cluster_only) {
      continue;
    }
    const std::string current_payload =
        KeyValuePayload({{"metric_family", seed.family},
                         {"metric_type", MetricTypeName(descriptor->type)},
                         {"metric_unit", MetricUnitName(descriptor->unit)},
                         {"namespace_path", descriptor->namespace_path},
                         {"value", seed.value},
                         {"count", "0"},
                         {"sum", "0"},
                         {"state_text", seed.state_text},
                         {"label_set", seed.label_set},
                         {"sample_class", "database_create_bootstrap"},
                         {"creator_local_transaction_id", std::to_string(kBootstrapCatalogTransactionId)},
                         {"seeded_at_database_create", "1"},
                         {"engine_owned", "1"}});
    typed = AddTypedCatalogRecord(&result.rows,
                                  CatalogRecordKind::metric_current_value,
                                  &ordinal,
                                  bootstrap_seed,
                                  current_payload,
                                  sys_metrics_object.value);
    if (!typed.ok()) { return typed; }
    bootstrap_seed += 4;
  }

  // SEARCH_KEY: SB_AGENT_POLICY_BOOTSTRAP_BASELINE
  // New databases receive editable baseline operational-agent policy records.
  // These records are safe by default: no agent is installed as live-action
  // automation without explicit later policy change and approval.
  for (const auto& agent : CanonicalAgentRegistry()) {
    const auto policy = BaselinePolicyForAgent(agent);
    std::string metrics;
    bool first_metric = true;
    for (const auto& metric : policy.required_metric_families) {
      if (!first_metric) {
        metrics += ",";
      }
      metrics += metric;
      first_metric = false;
    }
    const std::string policy_payload =
        KeyValuePayload({{"policy_name", policy.policy_name},
                         {"policy_uuid", policy.policy_uuid},
                         {"policy_class", "operational_agent"},
                         {"agent_type", agent.type_id},
                         {"deployment", AgentDeploymentName(agent.deployment)},
                         {"scope", agent.scope},
                         {"authority", AgentAuthorityClassName(agent.authority)},
                         {"activation", AgentActivationProfileName(policy.activation)},
                         {"allow_live_action", policy.allow_live_action ? "1" : "0"},
                         {"require_manual_approval", policy.require_manual_approval ? "1" : "0"},
                         {"require_dry_run_before_live", policy.require_dry_run_before_live ? "1" : "0"},
                         {"evidence_required", policy.evidence_required ? "1" : "0"},
                         {"explainability_required", policy.explainability_required ? "1" : "0"},
                         {"run_interval_microseconds", std::to_string(policy.run_interval_microseconds)},
                         {"lease_microseconds", std::to_string(policy.lease_microseconds)},
                         {"cooldown_microseconds", std::to_string(policy.cooldown_microseconds)},
                         {"max_runtime_microseconds", std::to_string(policy.max_runtime_microseconds)},
                         {"max_history_query_rows", std::to_string(policy.max_history_query_rows)},
                         {"max_evidence_fanout", std::to_string(policy.max_evidence_fanout)},
                         {"max_label_cardinality", std::to_string(policy.max_label_cardinality)},
                         {"action_budget_per_window", std::to_string(policy.action_budget_per_window)},
                         {"required_metric_families", metrics},
                         {"editable_seed_policy", "1"}});
    typed = AddTypedCatalogRecord(&result.rows,
                                  CatalogRecordKind::policy,
                                  &ordinal,
                                  bootstrap_seed,
                                  policy_payload,
                                  sys_metrics_object.value);
    if (!typed.ok()) { return typed; }
    bootstrap_seed += 4;
  }

  for (const auto& layout : AgentCatalogRecordLayouts()) {
    const std::string layout_payload =
        KeyValuePayload({{"table_name", layout},
                         {"system_table", "1"},
                         {"schema_path", "sys.agents"},
                         {"agent_catalog_layout", "1"},
                         {"engine_owned", "1"}});
    typed = AddTypedCatalogRecord(&result.rows,
                                  CatalogRecordKind::table_descriptor,
                                  &ordinal,
                                  bootstrap_seed,
                                  layout_payload,
                                  sys_agents_object.value);
    if (!typed.ok()) { return typed; }
    bootstrap_seed += 4;
  }

  const std::string seed_pack_payload =
      KeyValuePayload({{"seed_pack_name", image.seed_pack_name},
                       {"resource_seed_manifest_format_version", std::to_string(kResourceSeedManifestFormatCurrent)},
                       {"resource_seed_manifest_supported_min", std::to_string(kResourceSeedManifestFormatMinSupported)},
                       {"resource_seed_manifest_supported_max", std::to_string(kResourceSeedManifestFormatMaxSupported)},
                       {"seed_pack_version", image.seed_pack_version},
                       {"seed_pack_root", image.seed_pack_root},
                       {"manifest_path", image.manifest_path},
                       {"content_hash", image.content_hash},
                       {"i18n_version", image.i18n_version},
                       {"timezone_version", image.timezone_version},
                       {"charset_version", image.charset_version},
                       {"collation_version", image.collation_version},
                       {"locale_version", image.locale_version},
                       {"charset_content_hash", image.charset_content_hash},
                       {"collation_content_hash", image.collation_content_hash},
                       {"locale_content_hash", image.locale_content_hash},
                       {"timezone_content_hash", image.timezone_content_hash},
                       {"active", image.active ? "1" : "0"},
                       {"minimal_bootstrap", image.minimal_bootstrap ? "1" : "0"},
                       {"database_create_ready", image.database_create_ready ? "1" : "0"},
                       {"database_open_ready", image.database_open_ready ? "1" : "0"},
                       {"missing_seed_refusal_required", image.missing_seed_refusal_required ? "1" : "0"},
                       {"unsupported_upgrade_refusal_required", image.unsupported_upgrade_refusal_required ? "1" : "0"},
                       {"resource_bundle_records", std::to_string(image.resource_bundle_records)},
                       {"resource_artifact_records", std::to_string(image.resource_artifact_records)},
                       {"resource_activation_records", std::to_string(image.resource_activation_records)},
                       {"charset_records", std::to_string(image.charset_records)},
                       {"charset_alias_records", std::to_string(image.charset_alias_records)},
                       {"charset_mapping_artifacts", std::to_string(image.charset_mapping_artifacts)},
                       {"collation_records", std::to_string(image.collation_records)},
                       {"collation_tailoring_records", std::to_string(image.collation_tailoring_records)},
                       {"locale_records", std::to_string(image.locale_records)},
                       {"timezone_records", std::to_string(image.timezone_records)},
                       {"timezone_transition_records", std::to_string(image.timezone_transition_records)},
                       {"timezone_leap_second_records", std::to_string(image.timezone_leap_second_records)},
                       {"runtime_cache_invalidation_records", std::to_string(image.runtime_cache_invalidation_records)},
                       {"index_dependency_records", std::to_string(image.index_dependency_records)},
                       {"resource_epoch", std::to_string(image.resource_epoch)},
                       {"charset_epoch", std::to_string(image.charset_epoch)},
                       {"collation_epoch", std::to_string(image.collation_epoch)},
                       {"timezone_epoch", std::to_string(image.timezone_epoch)},
                       {"locale_epoch", std::to_string(image.locale_epoch)},
                       {"runtime_cache_epoch", std::to_string(image.runtime_cache_epoch)},
                       {"resource_alias_records", std::to_string(image.aliases.size())}});

  result.rows.push_back(Row(CatalogPageRowKind::resource_seed_pack, ordinal++, seed_pack_payload));

  const auto resource_bundle_object = GenerateEngineIdentityV7(UuidKind::object, config.creation_unix_epoch_millis + 50000);
  if (!resource_bundle_object.ok()) {
    return CatalogRowsBuildError(resource_bundle_object.status, resource_bundle_object.diagnostic);
  }

  const auto typed_bundle = AddTypedCatalogRecord(&result.rows,
                                                  CatalogRecordKind::resource_bundle,
                                                  &ordinal,
                                                  config.creation_unix_epoch_millis + 50001,
                                                  seed_pack_payload,
                                                  database_object.value,
                                                  resource_bundle_object.value);
  if (!typed_bundle.ok()) {
    return typed_bundle;
  }

  u64 resource_activation_seed = config.creation_unix_epoch_millis + 50020;
  for (const auto& family_version : image.family_versions) {
    const std::string family_payload =
        KeyValuePayload({{"family", ResourceSeedFamilyName(family_version.family)},
                         {"resource_version", family_version.version},
                         {"content_hash", family_version.content_hash},
                         {"activation_epoch", std::to_string(family_version.activation_epoch)},
                         {"resource_epoch", std::to_string(image.resource_epoch)},
                         {"runtime_cache_epoch", std::to_string(image.runtime_cache_epoch)},
                         {"active", family_version.active ? "1" : "0"},
                         {"resource_seed_pack", image.seed_pack_name},
                         {"activation_boundary", "database_create_tx1"},
                         {"cache_invalidation_publication_required", "1"},
                         {"audit_evidence", "resource_activation_record_v1"}});
    result.rows.push_back(Row(CatalogPageRowKind::resource_family_summary, ordinal++, family_payload));
    const auto activation_record = AddTypedCatalogRecord(&result.rows,
                                                         CatalogRecordKind::audit_evidence,
                                                         &ordinal,
                                                         resource_activation_seed,
                                                         family_payload,
                                                         resource_bundle_object.value);
    if (!activation_record.ok()) {
      return activation_record;
    }
    resource_activation_seed += 4;
  }

  for (const ResourceSeedArtifact& artifact : image.artifacts) {
    const std::string artifact_payload =
        KeyValuePayload({{"family", ResourceSeedFamilyName(artifact.family)},
                         {"canonical_path", artifact.canonical_path},
                         {"source_pattern", artifact.source_pattern},
                         {"content_hash", artifact.content_hash},
                         {"content_size_bytes", std::to_string(artifact.content_size_bytes)},
                         {"required_catalog_rows", artifact.required_catalog_rows},
                         {"create_time_action", artifact.create_time_action}});
    result.rows.push_back(Row(CatalogPageRowKind::resource_seed_artifact, ordinal++, artifact_payload));
    const auto typed_artifact = AddTypedCatalogRecord(&result.rows,
                                                      CatalogRecordKind::resource_artifact,
                                                      &ordinal,
                                                      config.creation_unix_epoch_millis + 50100 + ordinal * 2,
                                                      artifact_payload,
                                                      resource_bundle_object.value);
    if (!typed_artifact.ok()) {
      return typed_artifact;
    }
  }

  for (const ResourceSeedAlias& alias : image.aliases) {
    CatalogPageRowKind row_kind = CatalogPageRowKind::resource_family_summary;
    if (alias.family == ResourceSeedFamily::charset) {
      row_kind = CatalogPageRowKind::charset_alias_record;
    } else if (alias.family == ResourceSeedFamily::collation) {
      row_kind = CatalogPageRowKind::collation_record;
    } else if (alias.family == ResourceSeedFamily::timezone_tables ||
               alias.family == ResourceSeedFamily::timezone_source) {
      row_kind = CatalogPageRowKind::timezone_record;
    }
    const std::string alias_payload =
        KeyValuePayload({{"family", ResourceSeedFamilyName(alias.family)},
                         {"alias", alias.alias},
                         {"canonical_name", alias.canonical_name},
                         {"source_path", alias.source_path}});
    result.rows.push_back(Row(row_kind, ordinal++, alias_payload));
  }

  u64 resource_catalog_seed = config.creation_unix_epoch_millis + 70000;
  std::set<std::string> emitted_resource_records;
  for (const ResourceSeedAlias& alias : image.aliases) {
    CatalogRecordKind resource_kind = CatalogRecordKind::unknown;
    if (alias.family == ResourceSeedFamily::charset) {
      resource_kind = alias.alias == alias.canonical_name ? CatalogRecordKind::charset : CatalogRecordKind::charset_alias;
    } else if (alias.family == ResourceSeedFamily::collation) {
      if (alias.alias != alias.canonical_name) {
        continue;
      }
      resource_kind = CatalogRecordKind::collation;
    } else if (alias.family == ResourceSeedFamily::timezone_tables ||
               alias.family == ResourceSeedFamily::timezone_source) {
      resource_kind = CatalogRecordKind::timezone;
    } else {
      continue;
    }

    const std::string dedupe_key =
        std::to_string(static_cast<u16>(resource_kind)) + ":" +
        ResourceSeedFamilyName(alias.family) + ":" + alias.alias + ":" + alias.canonical_name;
    if (!emitted_resource_records.insert(dedupe_key).second) {
      continue;
    }

    const std::string resource_payload =
        KeyValuePayload({{"family", ResourceSeedFamilyName(alias.family)},
                         {"alias", alias.alias},
                         {"canonical_name", alias.canonical_name},
                         {"source_path", alias.source_path},
                         {"resource_seed_pack", image.seed_pack_name},
                         {"resource_seed_version", image.seed_pack_version},
                         {"loaded_at_database_create", "1"},
                         {"engine_owned", "1"}});
    typed = AddTypedCatalogRecord(&result.rows,
                                  resource_kind,
                                  &ordinal,
                                  resource_catalog_seed,
                                  resource_payload,
                                  resource_bundle_object.value);
    if (!typed.ok()) { return typed; }
    resource_catalog_seed += 4;
  }

  for (const ResourceSeedArtifact& artifact : image.artifacts) {
    CatalogRecordKind resource_detail_kind = CatalogRecordKind::unknown;
    if (artifact.family == ResourceSeedFamily::timezone_source ||
        artifact.family == ResourceSeedFamily::timezone_tables) {
      resource_detail_kind = CatalogRecordKind::timezone_transition;
    } else if (artifact.family == ResourceSeedFamily::timezone_leaps) {
      resource_detail_kind = CatalogRecordKind::timezone_leap_second;
    } else if (artifact.family == ResourceSeedFamily::locale ||
               artifact.family == ResourceSeedFamily::uca ||
               artifact.family == ResourceSeedFamily::uca_manifest) {
      resource_detail_kind = CatalogRecordKind::collation_tailoring;
    } else {
      continue;
    }
    const std::string detail_payload =
        KeyValuePayload({{"family", ResourceSeedFamilyName(artifact.family)},
                         {"canonical_path", artifact.canonical_path},
                         {"content_hash", artifact.content_hash},
                         {"content_size_bytes", std::to_string(artifact.content_size_bytes)},
                         {"required_catalog_rows", artifact.required_catalog_rows},
                         {"create_time_action", artifact.create_time_action},
                         {"resource_seed_pack", image.seed_pack_name},
                         {"resource_seed_version", image.seed_pack_version},
                         {"loaded_at_database_create", "1"},
                         {"engine_owned", "1"}});
    typed = AddTypedCatalogRecord(&result.rows,
                                  resource_detail_kind,
                                  &ordinal,
                                  resource_catalog_seed,
                                  detail_payload,
                                  resource_bundle_object.value);
    if (!typed.ok()) { return typed; }
    resource_catalog_seed += 4;
  }

  for (const auto& dependency : image.index_dependencies) {
    std::vector<std::pair<std::string, std::string>> fields = {
        {"index_name", dependency.dependent_artifact_name},
        {"system_index", "1"},
        {"dependent_artifact_class", dependency.dependent_artifact_class},
        {"resource_family", ResourceSeedFamilyName(dependency.family)},
        {"required_resource_version", dependency.required_version},
        {"required_resource_hash", dependency.required_content_hash},
        {"dependency_epoch", std::to_string(dependency.dependency_epoch)},
        {"resource_epoch", std::to_string(image.resource_epoch)},
        {"runtime_cache_epoch", std::to_string(image.runtime_cache_epoch)},
        {"compatibility_proof_required", "1"},
        {"index_rebuild_required_on_epoch_change", "1"},
        {"resource_dependency_evidence", dependency.compatibility_evidence},
        {"resource_seed_pack", image.seed_pack_name},
        {"resource_seed_version", image.seed_pack_version},
        {"loaded_at_database_create", "1"},
        {"engine_owned", "1"},
    };
    if (dependency.dependent_artifact_name == "sys.catalog.resource_seed_text_order_dependency_idx") {
      fields.push_back({"resource_families", "charset,collation,locale"});
      fields.push_back({"charset_version", image.charset_version});
      fields.push_back({"charset_epoch", std::to_string(image.charset_epoch)});
      fields.push_back({"collation_version", image.collation_version});
      fields.push_back({"collation_epoch", std::to_string(image.collation_epoch)});
      fields.push_back({"locale_version", image.locale_version});
      fields.push_back({"locale_epoch", std::to_string(image.locale_epoch)});
      fields.push_back({"key", "family,canonical_name,resource_epoch"});
    } else {
      fields.push_back({"resource_families", "timezone"});
      fields.push_back({"timezone_version", image.timezone_version});
      fields.push_back({"timezone_epoch", std::to_string(image.timezone_epoch)});
      fields.push_back({"key", "timezone_name,activation_epoch"});
    }
    const auto typed_dependency = AddTypedCatalogRecord(&result.rows,
                                                        CatalogRecordKind::index_descriptor,
                                                        &ordinal,
                                                        resource_catalog_seed,
                                                        KeyValuePayload(fields),
                                                        resource_bundle_object.value);
    if (!typed_dependency.ok()) {
      return typed_dependency;
    }
    resource_catalog_seed += 4;
  }

  const std::array<std::pair<CatalogPageRowKind, std::pair<CatalogRecordKind, u32>>, 7> summaries = {{
      {CatalogPageRowKind::charset_record, {CatalogRecordKind::charset, image.charset_records}},
      {CatalogPageRowKind::charset_alias_record, {CatalogRecordKind::charset_alias, image.charset_alias_records}},
      {CatalogPageRowKind::collation_record, {CatalogRecordKind::collation, image.collation_records}},
      {CatalogPageRowKind::collation_tailoring_record, {CatalogRecordKind::collation_tailoring, image.collation_tailoring_records}},
      {CatalogPageRowKind::timezone_record, {CatalogRecordKind::timezone, image.timezone_records}},
      {CatalogPageRowKind::timezone_transition_record, {CatalogRecordKind::timezone_transition, image.timezone_transition_records}},
      {CatalogPageRowKind::timezone_leap_second_record, {CatalogRecordKind::timezone_leap_second, image.timezone_leap_second_records}},
  }};
  for (const auto& summary : summaries) {
    const std::string payload = KeyValuePayload({{"record_count", std::to_string(summary.second.second)}});
    result.rows.push_back(Row(summary.first, ordinal++, payload));
    const auto typed_summary = AddTypedCatalogRecord(&result.rows,
                                                     summary.second.first,
                                                     &ordinal,
                                                     config.creation_unix_epoch_millis + 60000 + ordinal * 2,
                                                     payload,
                                                     resource_bundle_object.value);
    if (!typed_summary.ok()) {
      return typed_summary;
    }
  }
  return result;
}

ResourceSeedCatalogImage BuildResourceImageFromCatalogRows(const std::vector<CatalogPageRow>& rows) {
  ResourceSeedCatalogImage image;
  for (const CatalogPageRow& row : rows) {
    const auto fields = ParseKeyValuePayload(row.payload);
    if (row.kind == CatalogPageRowKind::resource_seed_pack) {
      image.seed_pack_name = fields.count("seed_pack_name") == 0 ? "" : fields.at("seed_pack_name");
      image.seed_pack_version = fields.count("seed_pack_version") == 0 ? "" : fields.at("seed_pack_version");
      image.seed_pack_root = fields.count("seed_pack_root") == 0 ? "" : fields.at("seed_pack_root");
      image.manifest_path = fields.count("manifest_path") == 0 ? "" : fields.at("manifest_path");
      image.content_hash = fields.count("content_hash") == 0 ? "" : fields.at("content_hash");
      image.i18n_version = fields.count("i18n_version") == 0 ? "" : fields.at("i18n_version");
      image.timezone_version = fields.count("timezone_version") == 0 ? "" : fields.at("timezone_version");
      image.charset_version = fields.count("charset_version") == 0 ? "" : fields.at("charset_version");
      image.collation_version = fields.count("collation_version") == 0 ? "" : fields.at("collation_version");
      image.locale_version = fields.count("locale_version") == 0 ? "" : fields.at("locale_version");
      image.charset_content_hash = fields.count("charset_content_hash") == 0 ? "" : fields.at("charset_content_hash");
      image.collation_content_hash = fields.count("collation_content_hash") == 0 ? "" : fields.at("collation_content_hash");
      image.locale_content_hash = fields.count("locale_content_hash") == 0 ? "" : fields.at("locale_content_hash");
      image.timezone_content_hash = fields.count("timezone_content_hash") == 0 ? "" : fields.at("timezone_content_hash");
      image.active = ParseU32Field(fields, "active") != 0;
      image.minimal_bootstrap = ParseU32Field(fields, "minimal_bootstrap") != 0;
      image.database_create_ready = ParseU32Field(fields, "database_create_ready") != 0;
      image.database_open_ready = ParseU32Field(fields, "database_open_ready") != 0;
      image.missing_seed_refusal_required = ParseU32Field(fields, "missing_seed_refusal_required") != 0;
      image.unsupported_upgrade_refusal_required = ParseU32Field(fields, "unsupported_upgrade_refusal_required") != 0;
      image.resource_bundle_records = ParseU32Field(fields, "resource_bundle_records");
      image.resource_artifact_records = ParseU32Field(fields, "resource_artifact_records");
      image.resource_activation_records = ParseU32Field(fields, "resource_activation_records");
      image.charset_records = ParseU32Field(fields, "charset_records");
      image.charset_alias_records = ParseU32Field(fields, "charset_alias_records");
      image.charset_mapping_artifacts = ParseU32Field(fields, "charset_mapping_artifacts");
      image.collation_records = ParseU32Field(fields, "collation_records");
      image.collation_tailoring_records = ParseU32Field(fields, "collation_tailoring_records");
      image.locale_records = ParseU32Field(fields, "locale_records");
      image.timezone_records = ParseU32Field(fields, "timezone_records");
      image.timezone_transition_records = ParseU32Field(fields, "timezone_transition_records");
      image.timezone_leap_second_records = ParseU32Field(fields, "timezone_leap_second_records");
      image.runtime_cache_invalidation_records = ParseU32Field(fields, "runtime_cache_invalidation_records");
      image.index_dependency_records = ParseU32Field(fields, "index_dependency_records");
      image.resource_epoch = ParseU64Field(fields, "resource_epoch");
      image.charset_epoch = ParseU64Field(fields, "charset_epoch");
      image.collation_epoch = ParseU64Field(fields, "collation_epoch");
      image.timezone_epoch = ParseU64Field(fields, "timezone_epoch");
      image.locale_epoch = ParseU64Field(fields, "locale_epoch");
      image.runtime_cache_epoch = ParseU64Field(fields, "runtime_cache_epoch");
      auto add_family = [&image](ResourceSeedFamily family,
                                 const std::string& version,
                                 const std::string& content_hash,
                                 u64 epoch) {
        if (version.empty() || content_hash.empty() || epoch == 0) {
          return;
        }
        scratchbird::core::resources::ResourceSeedFamilyVersion family_version;
        family_version.family = family;
        family_version.version = version;
        family_version.content_hash = content_hash;
        family_version.activation_epoch = epoch;
        family_version.active = true;
        image.family_versions.push_back(std::move(family_version));
      };
      add_family(ResourceSeedFamily::charset,
                 image.charset_version,
                 image.charset_content_hash,
                 image.charset_epoch);
      add_family(ResourceSeedFamily::collation,
                 image.collation_version,
                 image.collation_content_hash,
                 image.collation_epoch);
      add_family(ResourceSeedFamily::locale,
                 image.locale_version,
                 image.locale_content_hash,
                 image.locale_epoch);
      add_family(ResourceSeedFamily::timezone_version,
                 image.timezone_version,
                 image.timezone_content_hash,
                 image.timezone_epoch);
    } else if (row.kind == CatalogPageRowKind::resource_seed_artifact) {
      ResourceSeedArtifact artifact;
      artifact.family = fields.count("family") == 0 ? ResourceSeedFamily::unknown : ParseResourceFamilyName(fields.at("family"));
      artifact.canonical_path = fields.count("canonical_path") == 0 ? "" : fields.at("canonical_path");
      artifact.source_pattern = fields.count("source_pattern") == 0 ? "" : fields.at("source_pattern");
      artifact.content_hash = fields.count("content_hash") == 0 ? "" : fields.at("content_hash");
      artifact.content_size_bytes = ParseU64Field(fields, "content_size_bytes");
      artifact.required_catalog_rows = fields.count("required_catalog_rows") == 0 ? "" : fields.at("required_catalog_rows");
      artifact.create_time_action = fields.count("create_time_action") == 0 ? "" : fields.at("create_time_action");
      artifact.status = scratchbird::core::resources::ResourceSeedArtifactStatus::loaded;
      image.artifacts.push_back(std::move(artifact));
    } else if ((row.kind == CatalogPageRowKind::charset_alias_record ||
                row.kind == CatalogPageRowKind::collation_record ||
                row.kind == CatalogPageRowKind::timezone_record) &&
               fields.count("alias") != 0 && fields.count("canonical_name") != 0) {
      ResourceSeedAlias alias;
      alias.family = fields.count("family") == 0 ? ResourceSeedFamily::unknown : ParseResourceFamilyName(fields.at("family"));
      alias.alias = fields.at("alias");
      alias.canonical_name = fields.at("canonical_name");
      alias.source_path = fields.count("source_path") == 0 ? "" : fields.at("source_path");
      image.aliases.push_back(std::move(alias));
    } else if (row.kind == CatalogPageRowKind::typed_catalog_record) {
      const auto decoded = DecodeCatalogTypedRecord(row);
      if (!decoded.ok() || decoded.record.header.kind != CatalogRecordKind::index_descriptor) {
        continue;
      }
      const auto typed_fields = ParseKeyValuePayload(decoded.record.payload);
      if (typed_fields.count("resource_dependency_evidence") == 0 ||
          typed_fields.count("index_name") == 0 ||
          typed_fields.count("resource_family") == 0) {
        continue;
      }
      scratchbird::core::resources::ResourceSeedIndexDependencyEvidence dependency;
      dependency.dependent_artifact_name = typed_fields.at("index_name");
      dependency.dependent_artifact_class =
          typed_fields.count("dependent_artifact_class") == 0
              ? "index"
              : typed_fields.at("dependent_artifact_class");
      dependency.family = ParseResourceFamilyName(typed_fields.at("resource_family"));
      dependency.required_version =
          typed_fields.count("required_resource_version") == 0
              ? ""
              : typed_fields.at("required_resource_version");
      dependency.required_content_hash =
          typed_fields.count("required_resource_hash") == 0
              ? ""
              : typed_fields.at("required_resource_hash");
      dependency.dependency_epoch = ParseU64Field(typed_fields, "dependency_epoch");
      dependency.compatibility_proven = true;
      dependency.compatibility_evidence = typed_fields.at("resource_dependency_evidence");
      image.index_dependencies.push_back(std::move(dependency));
    }
  }
  if (image.resource_artifact_records == 0) {
    image.resource_artifact_records = static_cast<u32>(image.artifacts.size());
  }
  return image;
}

PolicySeedPackCatalogImage BuildPolicyImageFromCatalogRows(const std::vector<CatalogPageRow>& rows) {
  PolicySeedPackCatalogImage image;
  std::set<std::string> profile_areas;
  for (const CatalogPageRow& row : rows) {
    if (row.kind == CatalogPageRowKind::policy_seed_pack) {
      const auto fields = ParseKeyValuePayload(row.payload);
      image.active = true;
      image.policy_pack_id = fields.count("policy_pack_id") == 0 ? "" : fields.at("policy_pack_id");
      image.policy_pack_uuid = fields.count("policy_pack_uuid") == 0 ? "" : fields.at("policy_pack_uuid");
      image.policy_pack_version =
          fields.count("policy_pack_version") == 0 ? "" : fields.at("policy_pack_version");
      image.content_sha256 = fields.count("content_sha256") == 0 ? "" : fields.at("content_sha256");
      image.signature_status = fields.count("signature_status") == 0 ? "" : fields.at("signature_status");
      image.schema_version = ParseU32Field(fields, "schema_version");
      image.policy_generation = ParseU32Field(fields, "policy_generation");
      image.create_time_only = ParseU32Field(fields, "create_time_only") != 0;
      image.post_create_filesystem_authority =
          ParseU32Field(fields, "post_create_filesystem_authority") != 0;
      image.local_password_only = ParseU32Field(fields, "local_password_only") != 0;
      image.materialized_inside_create_transaction =
          ParseU32Field(fields, "materialized_inside_create_transaction") != 0;
      image.requires_mga_catalog_commit = ParseU32Field(fields, "requires_mga_catalog_commit") != 0;
      image.security_provider_records = ParseU32Field(fields, "security_provider_records");
      image.enabled_local_password_provider_records =
          ParseU32Field(fields, "enabled_local_password_provider_records");
      image.external_provider_disabled_records =
          ParseU32Field(fields, "external_provider_disabled_records");
      image.role_records = ParseU32Field(fields, "role_records");
      image.group_records = ParseU32Field(fields, "group_records");
      image.group_membership_records = ParseU32Field(fields, "group_membership_records");
      image.grant_records = ParseU32Field(fields, "grant_records");
      image.policy_profile_records = ParseU32Field(fields, "policy_profile_records");
      continue;
    }
    if (row.kind != CatalogPageRowKind::typed_catalog_record) {
      continue;
    }
    const auto decoded = DecodeCatalogTypedRecord(row);
    if (!decoded.ok()) {
      continue;
    }
    const auto fields = ParseKeyValuePayload(decoded.record.payload);
    if (decoded.record.header.kind == CatalogRecordKind::policy) {
      const auto policy_class = fields.find("policy_class");
      if (policy_class == fields.end()) {
        continue;
      }
      if (policy_class->second == "security_provider") {
        if (fields.count("provider_family") != 0 &&
            fields.at("provider_family") == "local_password" &&
            ParseU32Field(fields, "enabled_by_default") != 0) {
          image.enabled_local_password_provider_records =
              std::max<u32>(image.enabled_local_password_provider_records, 1);
        }
      } else if (policy_class->second == "policy_profile") {
        const auto area = fields.find("area");
        if (area != fields.end()) {
          profile_areas.insert(area->second);
        }
      }
    }
  }
  if (!profile_areas.empty()) {
    image.policy_profile_areas.assign(profile_areas.begin(), profile_areas.end());
  }
  return image;
}

DatabaseLifecycleResult ValidatePolicySeedCatalogImage(const PolicySeedPackCatalogImage& image,
                                                       const std::string& path) {
  if (!image.active) {
    DatabaseLifecycleResult result;
    result.status = DatabaseLifecycleOkStatus();
    return result;
  }
  if (!IsSafePolicyPackId(image.policy_pack_id) ||
      !ValidatePolicyUuid(image.policy_pack_uuid, UuidKind::object) ||
      image.policy_pack_version.empty() ||
      !IsSha256Hex(image.content_sha256) ||
      image.signature_status != "signature-ready-unsigned" ||
      image.schema_version != 1 ||
      image.policy_generation != 1 ||
      !image.create_time_only ||
      image.post_create_filesystem_authority ||
      !image.local_password_only ||
      !image.materialized_inside_create_transaction ||
      !image.requires_mga_catalog_commit ||
      image.security_provider_records == 0 ||
      image.enabled_local_password_provider_records != 1 ||
      image.role_records < 5 ||
      image.group_records < 6 ||
      image.grant_records == 0 ||
      image.policy_profile_records == 0 ||
      !PolicyProfileAreasExactlyCovered(image.policy_profile_areas)) {
    return LifecycleError("SB-POLICY-PACK-CATALOG-INVALID",
                          "storage.database_lifecycle.policy_pack_catalog_invalid",
                          path,
                          image.policy_pack_id);
  }
  DatabaseLifecycleResult result;
  result.status = DatabaseLifecycleOkStatus();
  return result;
}

DatabaseCatalogSummaryResult BuildTypedCatalogSummary(const std::vector<CatalogPageRow>& rows) {
  DatabaseCatalogSummaryResult result;
  result.status = DatabaseLifecycleOkStatus();
  std::map<std::string, u32> counts;
  for (const CatalogPageRow& row : rows) {
    if (row.kind != CatalogPageRowKind::typed_catalog_record) {
      continue;
    }
    const auto decoded = DecodeCatalogTypedRecord(row);
    if (!decoded.ok()) {
      return DatabaseCatalogSummaryError(decoded.status, decoded.diagnostic);
    }
    ++result.typed_record_count;
    ++counts[CatalogRecordKindName(decoded.record.header.kind)];
  }
  for (const auto& entry : counts) {
    result.typed_record_kinds.push_back(entry.first + "=" + std::to_string(entry.second));
  }
  return result;
}

DatabaseLifecycleResult WriteCatalogPageBodies(FileDevice* device,
                                               const PageManagerContext& page_context,
                                               const std::vector<CatalogPageRow>& rows,
                                               u64 creation_unix_epoch_millis) {
  const auto page_set = BuildCatalogPageSet(rows,
                                            page_context.page_size,
                                            kCatalogPageNumber,
                                            kCatalogOverflowFirstPageNumber);
  if (!page_set.ok()) {
    return PropagateDiagnostic(page_set.status, page_set.diagnostic);
  }

  for (const auto& page : page_set.pages) {
    auto page_buffer = AllocateManagedPageBuffer(page_context,
                                                 PageType::catalog,
                                                 "catalog_page_body_write");
    if (!page_buffer.ok()) {
      return PropagateDiagnostic(page_buffer.status, page_buffer.diagnostic);
    }
    auto* buffer_bytes = static_cast<scratchbird::core::platform::byte*>(page_buffer.buffer.data());
    std::memcpy(buffer_bytes + kPageHeaderSerializedBytes, page.body.data(), page.body.size());

    if (page.page_number != kCatalogPageNumber) {
      const auto page_header = WriteInitialPageHeader(device,
                                                      page_context,
                                                      PageType::catalog,
                                                      page.page_number,
                                                      creation_unix_epoch_millis);
      if (!page_header.ok()) {
        return page_header;
      }
    }

    const auto body_offset = CheckedPageBodyOffset(page_context.page_size,
                                                   page.page_number,
                                                   kPageHeaderSerializedBytes);
    if (!body_offset.ok()) {
      return PropagateDiagnostic(body_offset.status, body_offset.diagnostic);
    }
    const auto write = device->WriteAt(body_offset.offset,
                                       buffer_bytes + kPageHeaderSerializedBytes,
                                       page.body.size());
    if (!write.ok()) {
      return PropagateDiagnostic(write.status, write.diagnostic);
    }
  }

  DatabaseLifecycleResult result;
  result.status = DatabaseLifecycleOkStatus();
  return result;
}

// SB-DATABASE-TXN-INVENTORY-INTEGRATION-ANCHOR
DatabaseLifecycleResult WriteTransactionInventoryPage(FileDevice* device,
                                                      u32 page_size,
                                                      const LocalTransactionInventory& inventory) {
  const auto persisted = PersistLocalTransactionInventoryToOpenDevice(device, page_size, inventory);
  if (!persisted.ok()) {
    return PropagateDiagnostic(persisted.status, persisted.diagnostic);
  }

  DatabaseLifecycleResult result;
  result.status = DatabaseLifecycleOkStatus();
  return result;
}

struct LifecycleTransactionEvidence {
  LocalTransactionInventory inventory;
  LocalTransactionHorizons horizons;
  u64 committed_local_transaction_id = 0;
};

DatabaseLifecycleResult WriteInitialTransactionInventoryPage(FileDevice* device,
                                                            const PageManagerContext& page_context,
                                                            u64 creation_unix_epoch_millis,
                                                            LifecycleTransactionEvidence* evidence) {
  if (evidence == nullptr) {
    return LifecycleError("SB-DB-LIFECYCLE-TXN-ARGUMENT-INVALID",
                          "storage.database_lifecycle.transaction_argument_invalid");
  }
  const auto initialized =
      WriteTransactionInventoryPage(device, page_context.page_size, MakeEmptyLocalTransactionInventory());
  if (!initialized.ok()) { return initialized; }

  const auto bootstrap_tx_uuid =
      GenerateEngineIdentityV7(UuidKind::transaction, creation_unix_epoch_millis + 10001);
  if (!bootstrap_tx_uuid.ok()) {
    return PropagateDiagnostic(bootstrap_tx_uuid.status, bootstrap_tx_uuid.diagnostic);
  }
  auto begun = BeginLocalTransaction(MakeEmptyLocalTransactionInventory(),
                                     bootstrap_tx_uuid.value,
                                     creation_unix_epoch_millis);
  if (!begun.ok()) { return PropagateDiagnostic(begun.status, begun.diagnostic); }
  if (begun.entry.identity.local_id.value != kBootstrapCatalogTransactionId) {
    return LifecycleError("SB-DB-LIFECYCLE-BOOTSTRAP-TX-ID-INVALID",
                          "storage.database_lifecycle.bootstrap_transaction_id_invalid",
                          page_context.database_uuid.valid()
                              ? scratchbird::core::uuid::UuidToString(page_context.database_uuid.value)
                              : std::string{},
                          std::to_string(begun.entry.identity.local_id.value));
  }
  auto committed = CommitLocalTransaction(std::move(begun.inventory),
                                          MakeLocalTransactionId(kBootstrapCatalogTransactionId),
                                          creation_unix_epoch_millis);
  if (!committed.ok()) { return PropagateDiagnostic(committed.status, committed.diagnostic); }
  const auto computed_horizons = ComputeLocalTransactionHorizons(committed.inventory);
  if (!computed_horizons.ok()) {
    return PropagateDiagnostic(computed_horizons.status, computed_horizons.diagnostic);
  }
  const auto written = WriteTransactionInventoryPage(device, page_context.page_size, committed.inventory);
  if (!written.ok()) { return written; }
  evidence->committed_local_transaction_id = kBootstrapCatalogTransactionId;
  evidence->horizons = computed_horizons.horizons;
  evidence->inventory = std::move(committed.inventory);

  DatabaseLifecycleResult result;
  result.status = DatabaseLifecycleOkStatus();
  return result;
}

DatabaseLifecycleResult ReadTransactionInventoryPage(FileDevice* device,
                                                     u32 page_size,
                                                     LocalTransactionInventory* inventory,
                                                     LocalTransactionHorizons* horizons) {
  const auto loaded = LoadLocalTransactionInventoryFromOpenDevice(device, page_size);
  if (!loaded.ok()) { return PropagateDiagnostic(loaded.status, loaded.diagnostic); }
  *inventory = loaded.inventory;
  *horizons = loaded.horizons;

  DatabaseLifecycleResult result;
  result.status = DatabaseLifecycleOkStatus();
  return result;
}

DatabaseLifecycleResult CommitLifecycleTransaction(FileDevice* device,
                                                   u32 page_size,
                                                   LocalTransactionInventory* inventory,
                                                   LocalTransactionHorizons* horizons,
                                                   u64 now_millis,
                                                   u64 expected_local_transaction_id,
                                                   const std::string& path,
                                                   const std::string& invalid_id_code,
                                                   const std::string& invalid_id_key,
                                                   u64* committed_local_transaction_id) {
  if (inventory == nullptr || horizons == nullptr || committed_local_transaction_id == nullptr) {
    return LifecycleError("SB-DB-LIFECYCLE-TXN-ARGUMENT-INVALID",
                          "storage.database_lifecycle.transaction_argument_invalid",
                          path);
  }

  const auto tx_uuid = GenerateEngineIdentityV7(UuidKind::transaction, now_millis);
  if (!tx_uuid.ok()) {
    return PropagateDiagnostic(tx_uuid.status, tx_uuid.diagnostic);
  }
  auto begun = BeginLocalTransaction(*inventory, tx_uuid.value, now_millis);
  if (!begun.ok()) {
    return PropagateDiagnostic(begun.status, begun.diagnostic);
  }
  const u64 local_transaction_id = begun.entry.identity.local_id.value;
  if (expected_local_transaction_id != 0 && local_transaction_id != expected_local_transaction_id) {
    return LifecycleError(invalid_id_code,
                          invalid_id_key,
                          path,
                          std::to_string(local_transaction_id));
  }
  auto committed = CommitLocalTransaction(std::move(begun.inventory),
                                          MakeLocalTransactionId(local_transaction_id),
                                          now_millis);
  if (!committed.ok()) {
    return PropagateDiagnostic(committed.status, committed.diagnostic);
  }
  const auto written = WriteTransactionInventoryPage(device, page_size, committed.inventory);
  if (!written.ok()) {
    return written;
  }
  *inventory = std::move(committed.inventory);
  const auto computed_horizons = ComputeLocalTransactionHorizons(*inventory);
  if (!computed_horizons.ok()) {
    return PropagateDiagnostic(computed_horizons.status, computed_horizons.diagnostic);
  }
  *horizons = computed_horizons.horizons;
  *committed_local_transaction_id = local_transaction_id;

  DatabaseLifecycleResult result;
  result.status = DatabaseLifecycleOkStatus();
  return result;
}

DatabaseLifecycleResult ValidateCommittedLifecycleTransaction(const LocalTransactionInventory& inventory,
                                                              u64 local_transaction_id,
                                                              const std::string& path,
                                                              const std::string& missing_code,
                                                              const std::string& invalid_state_code) {
  const auto lookup = LookupLocalTransaction(inventory, MakeLocalTransactionId(local_transaction_id));
  if (!lookup.ok()) {
    return LifecycleError(missing_code,
                          "storage.database_lifecycle.lifecycle_transaction_missing",
                          path,
                          std::to_string(local_transaction_id));
  }
  if (lookup.entry.state != TransactionState::committed || !lookup.entry.evidence_record_written) {
    return LifecycleError(invalid_state_code,
                          "storage.database_lifecycle.lifecycle_transaction_invalid_state",
                          path,
                          std::to_string(local_transaction_id));
  }

  DatabaseLifecycleResult result;
  result.status = DatabaseLifecycleOkStatus();
  return result;
}

DatabaseLifecycleResult ValidateBootstrapLifecycleEvidence(const StartupStateRecord& startup_state,
                                                           const LocalTransactionInventory& inventory,
                                                           const std::string& path) {
  if (startup_state.bootstrap_local_transaction_id != kBootstrapCatalogTransactionId) {
    return LifecycleError("SB-DB-LIFECYCLE-BOOTSTRAP-EVIDENCE-MISSING",
                          "storage.database_lifecycle.bootstrap_evidence_missing",
                          path,
                          std::to_string(startup_state.bootstrap_local_transaction_id));
  }
  if (!StartupLifecycleEvidencePresent(startup_state,
                                       StartupLifecycleEvidenceFlag::bootstrap_tx1_committed)) {
    return LifecycleError("SB-DB-LIFECYCLE-BOOTSTRAP-EVIDENCE-MISSING",
                          "storage.database_lifecycle.bootstrap_evidence_missing",
                          path,
                          "bootstrap_tx1_committed");
  }
  return ValidateCommittedLifecycleTransaction(inventory,
                                               kBootstrapCatalogTransactionId,
                                               path,
                                               "SB-DB-LIFECYCLE-BOOTSTRAP-TXN-MISSING",
                                               "SB-DB-LIFECYCLE-BOOTSTRAP-TXN-INVALID-STATE");
}

DatabaseLifecycleResult ValidateAgentRuntimeAuthority(const std::string& path, AgentLifecycleMode mode) {
  const auto registry = ValidateCanonicalAgentRegistry();
  if (!registry.ok) {
    return LifecycleError("SB-DB-LIFECYCLE-AGENT-REGISTRY-INVALID",
                          "storage.database_lifecycle.agent_registry_invalid",
                          path,
                          registry.diagnostic_code + ":" + registry.detail);
  }
  if (!AgentPersistenceUsesScratchBirdStorageAuthority()) {
    return LifecycleError("SB-DB-LIFECYCLE-AGENT-STORAGE-AUTHORITY-INVALID",
                          "storage.database_lifecycle.agent_storage_authority_invalid",
                          path);
  }
  AgentRuntimeContext context;
  context.security_context_present = true;
  context.cluster_authority_available = false;
  context.private_features_available = true;
  context.standalone_edition = true;
  context.shutdown_requested = mode == AgentLifecycleMode::shutdown ||
                               mode == AgentLifecycleMode::database_close;
  context.read_only_mode = mode == AgentLifecycleMode::read_only ||
                           mode == AgentLifecycleMode::shutdown ||
                           mode == AgentLifecycleMode::database_close;
  context.trace_tags.push_back(std::string("database_lifecycle:") + AgentLifecycleModeName(mode));

  for (const auto& descriptor : CanonicalAgentRegistry()) {
    const AgentFeatureAvailability availability = EvaluateAgentFeatureAvailability(descriptor, context);
    if (availability == AgentFeatureAvailability::unavailable_cluster_authority ||
        availability == AgentFeatureAvailability::unavailable_edition) {
      continue;
    }
    if (availability != AgentFeatureAvailability::available) {
      return LifecycleError("SB-DB-LIFECYCLE-AGENT-FEATURE-UNAVAILABLE",
                            "storage.database_lifecycle.agent_feature_unavailable",
                            path,
                            descriptor.type_id + ":" + AgentFeatureAvailabilityName(availability));
    }
    auto policy = BaselinePolicyForAgent(descriptor);
    policy.activation = EffectiveActivationForLifecycle(policy.activation, mode);
    const auto policy_status = ValidateAgentPolicy(policy, descriptor);
    if (!policy_status.ok) {
      return LifecycleError("SB-DB-LIFECYCLE-AGENT-POLICY-INVALID",
                            "storage.database_lifecycle.agent_policy_invalid",
                            path,
                            policy_status.diagnostic_code + ":" + policy_status.detail);
    }
  }

  DatabaseLifecycleResult result;
  result.status = DatabaseLifecycleOkStatus();
  return result;
}

DatabaseEngineAgentInput MakeDatabaseEngineAgentInput(TypedUuid database_uuid,
                                                      DatabaseLifecyclePhase phase,
                                                      const StartupStateRecord& startup_state,
                                                      AgentLifecycleMode mode) {
  DatabaseEngineAgentInput input;
  input.database_uuid = scratchbird::core::uuid::UuidToString(database_uuid.value);
  input.engine_instance_uuid =
      DeterministicAgentRuntimeObjectUuidFromKey("database_engine_agent|" + input.database_uuid);
  input.database_lifecycle_state = DatabaseLifecyclePhaseName(phase);
  input.lifecycle_mode = mode;
  input.policy_generation = startup_state.lifecycle_generation == 0 ? 1 : startup_state.lifecycle_generation;
  input.catalog_generation = startup_state.lifecycle_generation == 0 ? 1 : startup_state.lifecycle_generation;
  input.security_generation = startup_state.lifecycle_generation == 0 ? 1 : startup_state.lifecycle_generation;
  input.filespace_generation = startup_state.restart_generation == 0 ? 1 : startup_state.restart_generation;
  input.agent_set_generation = startup_state.runtime_activation_generation == 0
      ? startup_state.lifecycle_generation
      : startup_state.runtime_activation_generation;
  if (input.agent_set_generation == 0) {
    input.agent_set_generation = 1;
  }
  input.health_generation = input.agent_set_generation;
  input.tx1_bootstrap_visible =
      StartupLifecycleEvidencePresent(startup_state,
                                      StartupLifecycleEvidenceFlag::bootstrap_tx1_committed);
  input.tx2_activation_committed =
      startup_state.first_open_activation_local_transaction_id != 0 &&
      StartupLifecycleEvidencePresent(startup_state,
                                      StartupLifecycleEvidenceFlag::first_open_tx2_committed);
  input.startup_admitted = startup_state.runtime_activation_complete &&
                           startup_state.agent_runtime_started &&
                           input.tx2_activation_committed;
  input.shutdown_requested = mode == AgentLifecycleMode::shutdown ||
                             mode == AgentLifecycleMode::database_close;
  input.graceful_shutdown = true;
  input.cluster_authority_available = false;
  input.health_publication_allowed = true;
  input.health_publication_persisted = true;
  input.allow_degraded_service = true;
  return input;
}

DatabaseEngineAgentHealthPublication MakeDatabaseEngineAgentNotStartedHealth(
    TypedUuid database_uuid,
    DatabaseLifecyclePhase phase,
    const StartupStateRecord& startup_state) {
  const auto input = MakeDatabaseEngineAgentInput(database_uuid,
                                                 phase,
                                                 startup_state,
                                                 AgentLifecycleMode::database_open);
  DatabaseEngineAgentHealthPublication health;
  health.database_uuid = input.database_uuid;
  health.engine_instance_uuid = input.engine_instance_uuid;
  health.database_lifecycle_state = input.database_lifecycle_state;
  health.agent_state = DatabaseEngineAgentLifecycleState::not_started;
  health.health_generation = input.health_generation;
  health.policy_generation = input.policy_generation;
  health.catalog_generation = input.catalog_generation;
  health.security_generation = input.security_generation;
  health.filespace_generation = input.filespace_generation;
  health.agent_set_generation = input.agent_set_generation;
  health.ordinary_admission_allowed = false;
  health.cluster_paths_failed_closed = true;
  health.health_publication_redacted = true;
  health.redacted_diagnostics.push_back("ENGINE.AGENT_LIFECYCLE_NOT_STARTED");
  return health;
}

DatabaseEngineAgentHealthPublication BuildDatabaseEngineAgentHealth(TypedUuid database_uuid,
                                                                    DatabaseLifecyclePhase phase,
                                                                    const StartupStateRecord& startup_state) {
  if (!startup_state.agent_runtime_started ||
      !StartupLifecycleEvidencePresent(startup_state,
                                       StartupLifecycleEvidenceFlag::first_open_tx2_committed)) {
    return MakeDatabaseEngineAgentNotStartedHealth(database_uuid, phase, startup_state);
  }
  const auto input = MakeDatabaseEngineAgentInput(database_uuid,
                                                 phase,
                                                 startup_state,
                                                 AgentLifecycleMode::database_open);
  const auto started = StartDatabaseEngineLifecycleAgent(input);
  if (started.ok()) {
    return started.health;
  }
  auto failed = MakeDatabaseEngineAgentNotStartedHealth(database_uuid, phase, startup_state);
  failed.agent_state = DatabaseEngineAgentLifecycleState::failed;
  failed.redacted_diagnostics.push_back(started.status.diagnostic_code);
  return failed;
}

void RecordDatabaseEngineAgentShutdownEvidence(StartupStateRecord* startup_state,
                                               TypedUuid database_uuid) {
  if (startup_state == nullptr) {
    return;
  }
  const auto current = BuildDatabaseEngineAgentHealth(database_uuid,
                                                     DatabaseLifecyclePhase::closed,
                                                     *startup_state);
  auto input = MakeDatabaseEngineAgentInput(database_uuid,
                                           DatabaseLifecyclePhase::closed,
                                           *startup_state,
                                           AgentLifecycleMode::shutdown);
  input.shutdown_requested = true;
  input.graceful_shutdown = true;
  input.health_generation = current.health_generation + 1;
  const auto stopped = StopDatabaseEngineLifecycleAgent(input, current);
  startup_state->completed_phases.push_back(stopped.ok()
      ? "close.database_engine_agent_stopped"
      : "close.database_engine_agent_stop_failed_closed");
  startup_state->agent_runtime_started = false;
}

// SEARCH_KEY: SB_DATABASE_RUNTIME_ACTIVATION_EVIDENCE
void RecordRuntimeActivationEvidence(StartupStateRecord* startup_state,
                                     bool suppress_background_agents) {
  if (startup_state == nullptr) {
    return;
  }
  startup_state->runtime_activation_complete = true;
  startup_state->cache_runtime_started = true;
  startup_state->agent_runtime_started = !suppress_background_agents;
  startup_state->ipc_runtime_started = !suppress_background_agents;
  startup_state->server_runtime_started = !suppress_background_agents;
  if (startup_state->runtime_activation_generation == 0) {
    startup_state->runtime_activation_generation = startup_state->lifecycle_generation;
  }
  startup_state->completed_phases.push_back("open.cache_runtime_started");
  if (suppress_background_agents) {
    startup_state->completed_phases.push_back("open.embedded_background_agents_suppressed");
    startup_state->completed_phases.push_back("open.embedded_ipc_runtime_suppressed");
    startup_state->completed_phases.push_back("open.embedded_server_runtime_suppressed");
  } else {
    startup_state->completed_phases.push_back("open.runtime_agents_started");
    startup_state->completed_phases.push_back("open.ipc_runtime_started");
    startup_state->completed_phases.push_back("open.server_runtime_started");
  }
}

PageCacheLifecycleInput MakePageCacheLifecycleInput(TypedUuid database_uuid,
                                                    const StartupStateRecord& startup_state,
                                                    DatabaseLifecyclePhase phase,
                                                    bool shutdown_requested = false) {
  PageCacheLifecycleInput input;
  input.database_uuid = database_uuid;
  input.filespace_uuid = startup_state.first_filespace_uuid;
  input.database_lifecycle_state = DatabaseLifecyclePhaseName(phase);
  input.policy_generation = startup_state.lifecycle_generation == 0 ? 1 : startup_state.lifecycle_generation;
  input.checkpoint_generation = startup_state.checkpoint_generation == 0 ? 1 : startup_state.checkpoint_generation;
  input.dirty_epoch = startup_state.startup_counter;
  input.tx2_activation_committed =
      startup_state.first_open_activation_local_transaction_id != 0 &&
      StartupLifecycleEvidencePresent(startup_state,
                                      StartupLifecycleEvidenceFlag::first_open_tx2_committed);
  input.cache_runtime_started = startup_state.cache_runtime_started;
  const bool embedded_direct_cache_owner =
      startup_state.runtime_activation_complete &&
      startup_state.cache_runtime_started &&
      !startup_state.agent_runtime_started &&
      !startup_state.ipc_runtime_started &&
      !startup_state.server_runtime_started;
  input.engine_agent_active = startup_state.agent_runtime_started || embedded_direct_cache_owner;
  input.writeback_allowed = true;
  input.checkpoint_allowed = true;
  input.clean_close_requested = shutdown_requested;
  input.shutdown_requested = shutdown_requested;
  input.force = shutdown_requested;
  input.standalone_mode = true;
  input.cluster_authority_available = false;
  return input;
}

DatabaseLifecycleResult BuildCorePageCacheEntries(FileDevice* device,
                                                  u32 page_size,
                                                  const DiskDevicePolicy& policy,
                                                  TypedUuid database_uuid,
                                                  TypedUuid filespace_uuid,
                                                  std::vector<PageCacheEntry>* entries,
                                                  const std::string& path) {
  if (device == nullptr || entries == nullptr) {
    return LifecycleError("SB-DB-LIFECYCLE-CACHE-INPUT-INVALID",
                          "storage.database_lifecycle.cache_input_invalid",
                          path);
  }
  const std::array<std::pair<PageType, u64>, 5> preload_pages = {{
      {PageType::system_state, kSystemStatePageNumber},
      {PageType::catalog, kCatalogPageNumber},
      {PageType::allocation_map, kAllocationMapPageNumber},
      {PageType::transaction_inventory, kTransactionInventoryPageNumber},
      {PageType::bootstrap_reserved, kBootstrapReservedPageNumber},
  }};

  entries->clear();
  for (const auto& [expected_type, page_number] : preload_pages) {
    const auto page = ReadDevicePageHeader(device, page_size, page_number, policy);
    if (!page.ok()) {
      return PropagateDiagnostic(page.status, page.diagnostic);
    }
    const auto parsed = ParsePageHeader(page.serialized);
    if (!parsed.ok()) {
      return PropagateDiagnostic(parsed.status, parsed.diagnostic);
    }
    if (parsed.header.page_type != expected_type ||
        !(parsed.header.database_uuid == database_uuid.value) ||
        !(parsed.header.filespace_uuid == filespace_uuid.value)) {
      return LifecycleError("SB-DB-LIFECYCLE-CACHE-PAGE-IDENTITY-MISMATCH",
                            "storage.database_lifecycle.cache_page_identity_mismatch",
                            path);
    }
    PageCacheEntry entry;
    entry.database_uuid = database_uuid;
    entry.filespace_uuid = filespace_uuid;
    entry.page_uuid.kind = UuidKind::page;
    entry.page_uuid.value = parsed.header.page_uuid;
    entry.page_type = parsed.header.page_type;
    entry.page_number = parsed.header.page_number;
    entry.page_generation = parsed.header.page_generation;
    entry.page_size = parsed.header.page_size;
    entries->push_back(entry);
  }

  DatabaseLifecycleResult result;
  result.status = DatabaseLifecycleOkStatus();
  return result;
}

DatabaseLifecycleResult RecordPageCachePreloadEvidence(FileDevice* device,
                                                       u32 page_size,
                                                       StartupStateRecord* startup_state,
                                                       TypedUuid database_uuid,
                                                       const std::string& path) {
  if (startup_state == nullptr) {
    return LifecycleError("SB-DB-LIFECYCLE-CACHE-INPUT-INVALID",
                          "storage.database_lifecycle.cache_input_invalid",
                          path);
  }
  std::vector<PageCacheEntry> entries;
  const auto build_entries = BuildCorePageCacheEntries(device,
                                                       page_size,
                                                       LifecycleDiskPolicy(page_size, false, true),
                                                       database_uuid,
                                                       startup_state->first_filespace_uuid,
                                                       &entries,
                                                       path);
  if (!build_entries.ok()) {
    return build_entries;
  }

  PageCacheLedger ledger;
  PageCachePolicy policy;
  policy.max_resident_pages = 16;
  policy.max_resident_bytes = 16ull * static_cast<u64>(page_size);
  const auto input = MakePageCacheLifecycleInput(database_uuid,
                                                *startup_state,
                                                DatabaseLifecyclePhase::opened);
  const auto preload = StartPageCacheLifecycle(&ledger, policy, input, entries);
  if (!preload.ok()) {
    return LifecycleError("SB-DB-LIFECYCLE-CACHE-PRELOAD-FAILED",
                          "storage.database_lifecycle.cache_preload_failed",
                          path,
                          preload.diagnostic.diagnostic_code);
  }
  startup_state->durable_evidence_flags |= StartupLifecycleEvidenceFlag::cache_preload_completed;
  startup_state->completed_phases.push_back("open.page_cache_preload_complete");
  startup_state->completed_phases.push_back("open.cache_checkpoint_evidence_not_finality");

  DatabaseLifecycleResult result;
  result.status = DatabaseLifecycleOkStatus();
  return result;
}

DatabaseLifecycleResult RecordPageCacheShutdownFlushEvidence(FileDevice* device,
                                                             u32 page_size,
                                                             StartupStateRecord* startup_state,
                                                             TypedUuid database_uuid,
                                                             const std::string& path) {
  if (startup_state == nullptr) {
    return LifecycleError("SB-DB-LIFECYCLE-CACHE-INPUT-INVALID",
                          "storage.database_lifecycle.cache_input_invalid",
                          path);
  }
  std::vector<PageCacheEntry> entries;
  const auto build_entries = BuildCorePageCacheEntries(device,
                                                       page_size,
                                                       LifecycleDiskPolicy(page_size, false, true),
                                                       database_uuid,
                                                       startup_state->first_filespace_uuid,
                                                       &entries,
                                                       path);
  if (!build_entries.ok()) {
    return build_entries;
  }

  PageCacheLedger ledger;
  PageCachePolicy policy;
  policy.max_resident_pages = 16;
  policy.max_resident_bytes = 16ull * static_cast<u64>(page_size);
  auto input = MakePageCacheLifecycleInput(database_uuid,
                                          *startup_state,
                                          DatabaseLifecyclePhase::closed);
  auto preload_input = input;
  preload_input.shutdown_requested = false;
  preload_input.clean_close_requested = false;
  const auto preload = StartPageCacheLifecycle(&ledger, policy, preload_input, entries);
  if (!preload.ok()) {
    return LifecycleError("SB-DB-LIFECYCLE-CACHE-PRELOAD-FAILED",
                          "storage.database_lifecycle.cache_preload_failed",
                          path,
                          preload.diagnostic.diagnostic_code);
  }
  for (const auto& entry : entries) {
    const auto marked = MarkPageCacheEntryDirty(&ledger, entry.page_uuid, true);
    if (!marked.ok()) {
      return LifecycleError("SB-DB-LIFECYCLE-CACHE-DIRTY-TRACKING-FAILED",
                            "storage.database_lifecycle.cache_dirty_tracking_failed",
                            path,
                            marked.diagnostic.diagnostic_code);
    }
  }
  input.checkpoint_generation = input.checkpoint_generation + 1;
  input.dirty_epoch = ledger.dirty_epoch;
  input.shutdown_requested = true;
  input.clean_close_requested = true;
  const auto shutdown_flush = ShutdownFlushPageCacheLifecycle(&ledger, input);
  if (!shutdown_flush.ok()) {
    return LifecycleError("SB-DB-LIFECYCLE-CACHE-SHUTDOWN-FLUSH-FAILED",
                          "storage.database_lifecycle.cache_shutdown_flush_failed",
                          path,
                          shutdown_flush.diagnostic.diagnostic_code);
  }
  startup_state->checkpoint_generation = input.checkpoint_generation;
  startup_state->durable_evidence_flags |= StartupLifecycleEvidenceFlag::cache_preload_completed |
                                           StartupLifecycleEvidenceFlag::cache_checkpoint_completed |
                                           StartupLifecycleEvidenceFlag::cache_shutdown_flush_completed;
  startup_state->completed_phases.push_back("close.page_cache_dirty_tracking_complete");
  startup_state->completed_phases.push_back("close.page_cache_writeback_complete");
  startup_state->completed_phases.push_back("close.cache_checkpoint_clean_close_evidence_recorded");
  startup_state->completed_phases.push_back("close.cache_shutdown_flush_complete");
  startup_state->completed_phases.push_back("close.cache_checkpoint_not_transaction_finality");
  const auto manifest = EmitDirtyManifestForCheckpoint(path, *startup_state, entries);
  if (!manifest.ok()) {
    return manifest;
  }
  startup_state->completed_phases.push_back("close.dirty_manifest_emitted");

  DatabaseLifecycleResult result;
  result.status = DatabaseLifecycleOkStatus();
  return result;
}

PageCacheCheckpointPublication BuildPageCachePublication(TypedUuid database_uuid,
                                                         DatabaseLifecyclePhase phase,
                                                         const StartupStateRecord& startup_state) {
  PageCacheCheckpointPublication publication;
  publication.database_uuid = scratchbird::core::uuid::UuidToString(database_uuid.value);
  publication.filespace_uuid =
      scratchbird::core::uuid::UuidToString(startup_state.first_filespace_uuid.value);
  publication.database_lifecycle_state = DatabaseLifecyclePhaseName(phase);
  publication.policy_generation = startup_state.lifecycle_generation == 0 ? 1 : startup_state.lifecycle_generation;
  publication.checkpoint_generation = startup_state.checkpoint_generation == 0
      ? 1
      : startup_state.checkpoint_generation;
  publication.dirty_epoch = startup_state.startup_counter;
  publication.cluster_paths_failed_closed = true;
  publication.lifecycle_state = startup_state.cache_runtime_started
      ? PageCacheLifecycleState::active
      : PageCacheLifecycleState::not_started;
  publication.preload_complete =
      StartupLifecycleEvidencePresent(startup_state,
                                      StartupLifecycleEvidenceFlag::cache_preload_completed);
  publication.checkpoint_complete =
      StartupLifecycleEvidencePresent(startup_state,
                                      StartupLifecycleEvidenceFlag::cache_checkpoint_completed);
  publication.clean_close_evidence =
      StartupLifecycleEvidencePresent(startup_state,
                                      StartupLifecycleEvidenceFlag::cache_shutdown_flush_completed);
  publication.shutdown_flush_complete = publication.clean_close_evidence;
  publication.writeback_complete = publication.checkpoint_complete;
  publication.ordinary_admission_allowed = startup_state.cache_runtime_started &&
                                           !startup_state.write_admission_fenced;
  if (publication.shutdown_flush_complete) {
    publication.lifecycle_state = PageCacheLifecycleState::stopped;
    publication.checkpoint_mode = PageCacheCheckpointMode::shutdown_flush;
    publication.ordinary_admission_allowed = false;
    publication.diagnostics.push_back("CACHE.CHECKPOINT_SHUTDOWN_FLUSH_COMPLETE");
  } else if (publication.preload_complete) {
    publication.checkpoint_mode = PageCacheCheckpointMode::try_checkpoint;
    publication.diagnostics.push_back("CACHE.CHECKPOINT_PRELOAD_COMPLETE");
  } else {
    publication.diagnostics.push_back("CACHE.CHECKPOINT_NOT_STARTED");
  }
  return publication;
}

DatabaseLifecycleResult EnsureFirstOpenActivationTransaction(FileDevice* device,
                                                             u32 page_size,
                                                             StartupStateRecord* startup_state,
                                                             LocalTransactionInventory* inventory,
                                                             LocalTransactionHorizons* horizons,
                                                             const std::string& path,
                                                             bool suppress_background_agents) {
  if (startup_state == nullptr || inventory == nullptr || horizons == nullptr) {
    return LifecycleError("SB-DB-LIFECYCLE-ACTIVATION-ARGUMENT-INVALID",
                          "storage.database_lifecycle.activation_argument_invalid",
                          path);
  }

  const auto agent_runtime = ValidateAgentRuntimeAuthority(path, AgentLifecycleMode::database_open);
  if (!agent_runtime.ok()) {
    return agent_runtime;
  }

  if (startup_state->first_open_activation_local_transaction_id != 0) {
    if (!StartupLifecycleEvidencePresent(*startup_state,
                                         StartupLifecycleEvidenceFlag::first_open_tx2_committed)) {
      return LifecycleError("SB-DB-LIFECYCLE-ACTIVATION-EVIDENCE-MISSING",
                            "storage.database_lifecycle.activation_evidence_missing",
                            path,
                            "first_open_tx2_committed");
    }
    const auto validation = ValidateCommittedLifecycleTransaction(
        *inventory,
        startup_state->first_open_activation_local_transaction_id,
        path,
        "SB-DB-LIFECYCLE-ACTIVATION-TXN-MISSING",
        "SB-DB-LIFECYCLE-ACTIVATION-TXN-INVALID-STATE");
    if (validation.ok()) {
      RecordRuntimeActivationEvidence(startup_state, suppress_background_agents);
    }
    return validation;
  }

  const auto existing_tx2 = LookupLocalTransaction(
      *inventory,
      MakeLocalTransactionId(kFirstOpenActivationLocalTransactionId));
  if (existing_tx2.ok()) {
    if (existing_tx2.entry.state != TransactionState::committed ||
        !existing_tx2.entry.evidence_record_written) {
      return LifecycleError("SB-DB-LIFECYCLE-ACTIVATION-TXN-INVALID-STATE",
                            "storage.database_lifecycle.activation_transaction_invalid_state",
                            path,
                            std::to_string(kFirstOpenActivationLocalTransactionId));
    }
    startup_state->first_open_activation_local_transaction_id =
        kFirstOpenActivationLocalTransactionId;
    *startup_state = RecordStartupLifecycleEvidence(
        std::move(*startup_state),
        StartupLifecycleDurablePhase::open_tx2_committed,
        kFirstOpenActivationLocalTransactionId,
        existing_tx2.entry.final_unix_epoch_millis,
        StartupLifecycleEvidenceFlag::first_open_tx2_committed);
    startup_state->completed_phases.push_back("open.first_connection_activation_transaction_recovered");
    startup_state->completed_phases.push_back("open.agent_registry_validated");
    startup_state->completed_phases.push_back("open.agent_lifecycle_mode_validated");
    startup_state->completed_phases.push_back("open.runtime_activation_policy_loaded");
    RecordRuntimeActivationEvidence(startup_state, suppress_background_agents);
    return DatabaseLifecycleResult{DatabaseLifecycleOkStatus(), {}, {}};
  }

  if (inventory->next_local_transaction_id != kFirstOpenActivationLocalTransactionId) {
    return LifecycleError("SB-DB-LIFECYCLE-ACTIVATION-TX-ID-INVALID",
                          "storage.database_lifecycle.activation_transaction_id_invalid",
                          path,
                          std::to_string(inventory->next_local_transaction_id));
  }

  u64 committed_local_transaction_id = 0;
  const u64 activation_unix_epoch_millis = CurrentUnixEpochMillis();
  const auto committed = CommitLifecycleTransaction(
      device,
      page_size,
      inventory,
      horizons,
      activation_unix_epoch_millis,
      kFirstOpenActivationLocalTransactionId,
      path,
      "SB-DB-LIFECYCLE-ACTIVATION-TX-ID-INVALID",
      "storage.database_lifecycle.activation_transaction_id_invalid",
      &committed_local_transaction_id);
  if (!committed.ok()) {
    return committed;
  }

  startup_state->first_open_activation_local_transaction_id = committed_local_transaction_id;
  *startup_state = RecordStartupLifecycleEvidence(
      std::move(*startup_state),
      StartupLifecycleDurablePhase::open_tx2_committed,
      committed_local_transaction_id,
      activation_unix_epoch_millis,
      StartupLifecycleEvidenceFlag::first_open_tx2_committed);
  startup_state->completed_phases.push_back("open.first_connection_activation_transaction_committed");
  startup_state->completed_phases.push_back("open.agent_registry_validated");
  startup_state->completed_phases.push_back("open.agent_lifecycle_mode_validated");
  startup_state->completed_phases.push_back("open.runtime_activation_policy_loaded");
  RecordRuntimeActivationEvidence(startup_state, suppress_background_agents);
  return DatabaseLifecycleResult{DatabaseLifecycleOkStatus(), {}, {}};
}

DatabaseLifecycleResult ReadCatalogPageRows(FileDevice* device,
                                            u32 page_size,
                                            std::vector<CatalogPageRow>* rows) {
  u64 page_number = kCatalogPageNumber;
  u32 visited = 0;
  while (page_number != 0) {
    if (++visited > 1024) {
      return LifecycleError("SB-CATALOG-PAGE-BODY-NEXT-CHAIN-TOO-LONG",
                            "storage.database_lifecycle.catalog_next_chain_too_long");
    }
    scratchbird::core::memory::PageBufferRequest page_buffer_request;
    page_buffer_request.page_size = page_size;
    page_buffer_request.page_count = 1;
    page_buffer_request.tag = MemoryTag{Subsystem::storage_disk,
                                        "catalog_page_body_read",
                                        MemoryCategory::page_buffer,
                                        MemoryLifetime::page_buffer,
                                        "storage.database.lifecycle",
                                        "open"};
    auto page_buffer = DefaultMemoryManager().AllocateScopedPageBuffer(page_buffer_request);
    if (!page_buffer.ok()) {
      return PropagateDiagnostic(page_buffer.status, page_buffer.diagnostic);
    }
    auto* buffer_bytes = static_cast<scratchbird::core::platform::byte*>(page_buffer.buffer.data());
    const auto body_offset = CheckedPageBodyOffset(page_size,
                                                   page_number,
                                                   kPageHeaderSerializedBytes);
    if (!body_offset.ok()) {
      return PropagateDiagnostic(body_offset.status, body_offset.diagnostic);
    }
    const auto read = device->ReadAt(body_offset.offset,
                                     buffer_bytes + kPageHeaderSerializedBytes,
                                     page_size - kPageHeaderSerializedBytes);
    if (!read.ok()) {
      return PropagateDiagnostic(read.status, read.diagnostic);
    }
    std::vector<scratchbird::core::platform::byte> body(buffer_bytes + kPageHeaderSerializedBytes,
                                                        buffer_bytes + page_size);
    const auto parsed = ParseCatalogPageBody(body, page_number);
    if (!parsed.ok()) {
      return PropagateDiagnostic(parsed.status, parsed.diagnostic);
    }
    rows->insert(rows->end(), parsed.body.rows.begin(), parsed.body.rows.end());
    page_number = parsed.body.next_page_number;
  }

  DatabaseLifecycleResult result;
  result.status = DatabaseLifecycleOkStatus();
  return result;
}

StartupRecoveryClassification ClassifyStartupStateForOpen(const StartupStateRecord& state) {
  if (state.clean_shutdown && state.startup_dirty) {
    return StartupRecoveryClassification::restricted_open_required;
  }
  if (state.clean_shutdown && !state.startup_dirty && !state.write_admission_fenced) {
    return StartupRecoveryClassification::clean_checkpoint_path;
  }
  if (!state.clean_shutdown && state.startup_dirty) {
    return StartupRecoveryClassification::repaired_recovery;
  }
  if (state.write_admission_fenced) {
    return StartupRecoveryClassification::fence_writes_until_safe;
  }
  return StartupRecoveryClassification::checkpoint_rebuild_required;
}

DatabaseLifecycleResult ValidateCreateConfig(const DatabaseCreateConfig& config) {
  if (config.path.empty()) {
    return LifecycleError("SB-DB-LIFECYCLE-PATH-REQUIRED",
                          "storage.database_lifecycle.path_required");
  }
  if (!IsTypedEngineIdentity(config.database_uuid, UuidKind::database)) {
    return LifecycleError("SB-DB-LIFECYCLE-DATABASE-UUID-MUST-BE-V7",
                          "storage.database_lifecycle.database_uuid_must_be_v7",
                          config.path);
  }
  if (!IsTypedEngineIdentity(config.filespace_uuid, UuidKind::filespace)) {
    return LifecycleError("SB-DB-LIFECYCLE-FILESPACE-UUID-MUST-BE-V7",
                          "storage.database_lifecycle.filespace_uuid_must_be_v7",
                          config.path);
  }
  if (config.require_resource_seed_pack && config.resource_seed_pack_root.empty()) {
    return LifecycleError("SB_RESOURCE_SEED_MISSING",
                          "storage.database_lifecycle.resource_seed_pack_required",
                          config.path);
  }

  DatabaseLifecycleResult result;
  result.status = DatabaseLifecycleOkStatus();
  return result;
}

DatabaseLifecycleResult WriteInitialPageHeader(FileDevice* device,
                                               const PageManagerContext& context,
                                               PageType page_type,
                                               u64 page_number,
                                               u64 creation_unix_epoch_millis) {
  const auto generated = GenerateEngineIdentityV7(UuidKind::page, creation_unix_epoch_millis + page_number);
  if (!generated.ok()) {
    return PropagateDiagnostic(generated.status, generated.diagnostic);
  }

  ManagedPageHeaderRequest request;
  request.context = context;
  request.page_type = page_type;
  request.page_number = page_number;
  request.page_uuid = generated.value;
  request.page_generation = 1;

  const auto built = BuildManagedPageHeader(request);
  if (!built.ok()) {
    return PropagateDiagnostic(built.status, built.diagnostic);
  }

  auto page_buffer = AllocateManagedPageBuffer(context, page_type, "initial_page_header_write");
  if (!page_buffer.ok()) {
    return PropagateDiagnostic(page_buffer.status, page_buffer.diagnostic);
  }
  auto* buffer_bytes = static_cast<scratchbird::core::platform::byte*>(page_buffer.buffer.data());
  std::memcpy(buffer_bytes, built.serialized.data(), built.serialized.size());

  const auto page_offset = CheckedPageOffset(context.page_size, page_number);
  if (!page_offset.ok()) {
    return PropagateDiagnostic(page_offset.status, page_offset.diagnostic);
  }
  const auto write = device->WriteAt(page_offset.offset,
                                     buffer_bytes,
                                     page_buffer.buffer.size());
  if (!write.ok()) {
    return PropagateDiagnostic(write.status, write.diagnostic);
  }

  DatabaseLifecycleResult result;
  result.status = DatabaseLifecycleOkStatus();
  return result;
}

DatabaseLifecycleState MakeState(const std::string& path,
                                 const DatabaseHeader& header,
                                 TypedUuid database_uuid,
                                 TypedUuid filespace_uuid,
                                 DatabaseLifecyclePhase phase,
                                 bool cluster_authority_available,
                                 bool decryption_available,
                                 bool read_only_open,
                                 DiskHealthSnapshot disk_health,
                                 ResourceSeedCatalogImage resource_seed_catalog = {},
                                 PolicySeedPackCatalogImage policy_seed_catalog = {},
                                 DatabaseCatalogSummaryResult catalog_summary = {},
                                 LocalTransactionInventory local_transaction_inventory = {},
                                 LocalTransactionHorizons local_transaction_horizons = {},
                                 StartupStateRecord startup_state = {},
                                 DatabaseOpenCompatibilityClass compatibility_class =
                                     DatabaseOpenCompatibilityClass::current) {
  DatabaseLifecycleState state;
  state.path = path;
  state.header = header;
  state.database_uuid = database_uuid;
  state.filespace_uuid = filespace_uuid;
  state.phase = phase;
  state.cluster_structures_present =
      (header.feature_flags & scratchbird::storage::disk::DatabaseFeatureFlag::cluster_structures_present) != 0;
  (void)cluster_authority_available;
  state.cluster_authority_active = false;
  state.decryption_available = decryption_available;
  state.read_only_open = read_only_open;
  state.database_open_compatibility_class = compatibility_class;
  state.disk_health = std::move(disk_health);
  state.safe_unknown_page_handling_required =
      (header.compatibility_flags &
       scratchbird::storage::disk::DatabaseCompatibilityFlag::unknown_page_safe_classification_required) != 0;
  state.resource_seed_catalog_present = resource_seed_catalog.active || resource_seed_catalog.minimal_bootstrap;
  state.resource_seed_catalog = std::move(resource_seed_catalog);
  state.policy_seed_catalog_present = policy_seed_catalog.active;
  state.policy_seed_catalog = std::move(policy_seed_catalog);
  state.typed_catalog_records_present = catalog_summary.typed_record_count != 0;
  state.typed_catalog_record_count = catalog_summary.typed_record_count;
  state.typed_catalog_record_kinds = std::move(catalog_summary.typed_record_kinds);
  state.memory_accounting = DefaultMemoryManager().Snapshot();
  state.local_transaction_inventory = std::move(local_transaction_inventory);
  state.local_transaction_horizons = local_transaction_horizons;
  state.local_transaction_inventory_present = local_transaction_horizons.valid;
  state.startup_state = std::move(startup_state);
  state.startup_state_present = state.startup_state.first_filespace_uuid.valid();
  state.write_admission_fenced = read_only_open || state.startup_state.write_admission_fenced;
  state.startup_recovery_classification =
      StartupRecoveryClassificationName(state.startup_state.recovery_classification);
  state.startup_owner_token = state.startup_state.owner_token;
  if (state.startup_state_present) {
    state.engine_agent_health_present = true;
    state.engine_agent_health =
        BuildDatabaseEngineAgentHealth(state.database_uuid, phase, state.startup_state);
    state.engine_agent_health_json =
        SerializeDatabaseEngineAgentHealthJson(state.engine_agent_health, false);
    state.cache_checkpoint_present = true;
    state.cache_checkpoint = BuildPageCachePublication(state.database_uuid, phase, state.startup_state);
    state.cache_checkpoint_json = SerializePageCacheCheckpointJson(state.cache_checkpoint, false);
  }
  return state;
}

int CompareArtifactVersion(u32 left_major, u32 left_minor, u32 right_major, u32 right_minor) {
  if (left_major < right_major) return -1;
  if (left_major > right_major) return 1;
  if (left_minor < right_minor) return -1;
  if (left_minor > right_minor) return 1;
  return 0;
}

std::string ArtifactVersionDetail(const DatabaseArtifactVersionCompatibilityRequest& request) {
  return request.artifact_kind + "=" + std::to_string(request.format_major) + "." +
         std::to_string(request.format_minor) + " supported=" +
         std::to_string(request.min_supported_major) + "." +
         std::to_string(request.min_supported_minor) + ".." +
         std::to_string(request.max_supported_major) + "." +
         std::to_string(request.max_supported_minor) + " current=" +
         std::to_string(request.current_major) + "." +
         std::to_string(request.current_minor);
}

bool IsSupportedDatabaseMigrationPlan(const DatabaseArtifactVersionCompatibilityRequest& request) {
  if (request.migration_plan_id.empty()) {
    return false;
  }
  const std::string expected = request.artifact_kind + "_v" +
      std::to_string(request.format_major) + "_" + std::to_string(request.format_minor) +
      "_to_v" + std::to_string(request.current_major) + "_" +
      std::to_string(request.current_minor) + "_explicit_plan_v1";
  return request.migration_plan_id == expected;
}

DatabaseArtifactCompatibilityResult DatabaseArtifactCompatibilityOk(
    DatabaseOpenCompatibilityClass compatibility_class,
    bool migration_required,
    bool read_only_compatible = false) {
  DatabaseArtifactCompatibilityResult result;
  result.status = DatabaseLifecycleOkStatus();
  result.compatibility_class = compatibility_class;
  result.migration_required = migration_required;
  result.read_only_compatible = read_only_compatible;
  return result;
}

DatabaseArtifactCompatibilityResult DatabaseArtifactCompatibilityError(
    DatabaseOpenCompatibilityClass compatibility_class,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail) {
  DatabaseArtifactCompatibilityResult result;
  result.status = DatabaseLifecycleErrorStatus();
  result.compatibility_class = compatibility_class;
  result.migration_required =
      compatibility_class == DatabaseOpenCompatibilityClass::missing_migration_plan_refused ||
      compatibility_class == DatabaseOpenCompatibilityClass::migration_required_without_plan_refused;
  result.diagnostic = MakeDatabaseLifecycleDiagnostic(result.status,
                                                      std::move(diagnostic_code),
                                                      std::move(message_key),
                                                      {},
                                                      std::move(detail));
  return result;
}

DatabaseOpenConfig ReadOnlyLifecycleOpenConfig(const DatabaseLifecycleOperationConfig& config) {
  DatabaseOpenConfig open;
  open.path = config.path;
  open.cluster_authority_available = config.cluster_authority_available;
  open.decryption_available = config.decryption_available;
  open.read_only = true;
  return open;
}

DatabaseOpenConfig ReadOnlyRepairOpenConfig(const DatabaseLifecycleRepairConfig& config) {
  DatabaseOpenConfig open;
  open.path = config.path;
  open.cluster_authority_available = config.cluster_authority_available;
  open.decryption_available = config.decryption_available;
  open.read_only = true;
  return open;
}

DatabaseLifecycleResult ClassifyReadOnlyLifecycle(const DatabaseOpenConfig& open_config,
                                                  DatabaseLifecyclePhase phase) {
  auto opened = OpenDatabaseFile(open_config);
  if (opened.ok()) {
    opened.state.phase = phase;
  }
  return opened;
}

std::string TypedUuidText(const TypedUuid& uuid) {
  return scratchbird::core::uuid::UuidToString(uuid.value);
}

bool ExpectedIdentityMatches(const DatabaseLifecycleState& state,
                             const std::string& expected_database_uuid,
                             const std::string& expected_filespace_uuid) {
  return !expected_database_uuid.empty() &&
         !expected_filespace_uuid.empty() &&
         expected_database_uuid == TypedUuidText(state.database_uuid) &&
         expected_filespace_uuid == TypedUuidText(state.filespace_uuid);
}

DatabaseLifecycleResult DblcLifecycleError(std::string code,
                                           std::string message_key,
                                           const std::string& path,
                                           std::string detail = {}) {
  return LifecycleError(std::move(code), std::move(message_key), path, std::move(detail));
}

DatabaseLifecycleResult RecordStartupEvidence(const DatabaseLifecycleOperationConfig& config,
                                              DatabaseLifecyclePhase result_phase,
                                              StartupLifecycleDurablePhase durable_phase,
                                              u64 evidence_flags,
                                              bool fence_writes,
                                              bool clear_restricted_state,
                                              StartupRecoveryClassification classification,
                                              const std::string& completed_phase) {
  if (config.path.empty()) {
    return LifecycleError("SB-DB-LIFECYCLE-PATH-REQUIRED",
                          "storage.database_lifecycle.path_required");
  }

  FileDevice device;
  const auto open = device.Open(config.path, FileOpenMode::open_existing);
  if (!open.ok()) {
    return PropagateDiagnostic(open.status, open.diagnostic);
  }

  SerializedDatabaseHeader serialized{};
  const auto read_header = device.ReadAt(0, serialized.data(), serialized.size());
  if (!read_header.ok()) {
    return PropagateDiagnostic(read_header.status, read_header.diagnostic);
  }
  const auto parsed = ParseDatabaseHeader(serialized);
  if (!parsed.ok()) {
    return PropagateDiagnostic(parsed.status, parsed.diagnostic);
  }
  const bool cluster_authority_required =
      (parsed.header.compatibility_flags &
       scratchbird::storage::disk::DatabaseCompatibilityFlag::requires_cluster_authority) != 0 ||
      (parsed.header.feature_flags &
       scratchbird::storage::disk::DatabaseFeatureFlag::cluster_structures_present) != 0;
  if (cluster_authority_required && !config.cluster_authority_available) {
    return LifecycleError("SB-DB-LIFECYCLE-CLUSTER-AUTHORITY-REQUIRED",
                          "storage.database_lifecycle.cluster_authority_required",
                          config.path);
  }
  if (cluster_authority_required) {
    return LifecycleError("SB-DB-LIFECYCLE-CLUSTER-MAPPING-UNAVAILABLE",
                          "storage.database_lifecycle.cluster_mapping_unavailable",
                          config.path);
  }

  auto startup = ReadStartupStatePageBody(&device, parsed.header.page_size);
  if (!startup.ok()) {
    return PropagateDiagnostic(startup.status, startup.diagnostic);
  }
  if (!(startup.state.database_uuid.value == parsed.header.database_uuid)) {
    return LifecycleError("SB-DB-LIFECYCLE-STARTUP-DATABASE-UUID-MISMATCH",
                          "storage.database_lifecycle.startup_database_uuid_mismatch",
                          config.path);
  }

  LocalTransactionInventory local_transaction_inventory;
  LocalTransactionHorizons local_transaction_horizons;
  const auto read_txn_inventory = ReadTransactionInventoryPage(&device,
                                                              parsed.header.page_size,
                                                              &local_transaction_inventory,
                                                              &local_transaction_horizons);
  if (!read_txn_inventory.ok()) {
    return read_txn_inventory;
  }

  u64 committed_local_transaction_id = 0;
  const auto committed = CommitLifecycleTransaction(
      &device,
      parsed.header.page_size,
      &local_transaction_inventory,
      &local_transaction_horizons,
      CurrentUnixEpochMillis(),
      0,
      config.path,
      "SB-DB-LIFECYCLE-MAINTENANCE-TX-ID-INVALID",
      "storage.database_lifecycle.maintenance_transaction_id_invalid",
      &committed_local_transaction_id);
  if (!committed.ok()) {
    return committed;
  }

  startup.state.write_admission_fenced = fence_writes;
  if (clear_restricted_state && startup.state.clean_shutdown && !startup.state.startup_dirty) {
    startup.state.owner_token.clear();
  }
  startup.state.recovery_classification = classification;
  startup.state.completed_phases.push_back(completed_phase);
  startup.state = RecordStartupLifecycleEvidence(std::move(startup.state),
                                                 durable_phase,
                                                 committed_local_transaction_id,
                                                 CurrentUnixEpochMillis(),
                                                 evidence_flags);
  const auto written = WriteStartupStatePageBody(&device, startup.state);
  if (!written.ok()) {
    return PropagateDiagnostic(written.status, written.diagnostic);
  }
  const auto sync = device.Sync();
  if (!sync.ok()) {
    return PropagateDiagnostic(sync.status, sync.diagnostic);
  }
  const auto close = device.Close();
  if (!close.ok()) {
    return PropagateDiagnostic(close.status, close.diagnostic);
  }

  DatabaseOpenConfig read_back;
  read_back.path = config.path;
  read_back.cluster_authority_available = config.cluster_authority_available;
  read_back.decryption_available = config.decryption_available;
  read_back.read_only = true;
  auto result = OpenDatabaseFile(read_back);
  if (result.ok()) {
    result.state.phase = result_phase;
  }
  return result;
}

}  // namespace

DatabaseArtifactCompatibilityResult ClassifyDatabaseArtifactVersionCompatibility(
    const DatabaseArtifactVersionCompatibilityRequest& request) {
  const std::string detail = ArtifactVersionDetail(request);
  if (!request.identity_proven) {
    return DatabaseArtifactCompatibilityError(
        DatabaseOpenCompatibilityClass::ambiguous_identity_refused,
        "ENGINE.DBLC_MIGRATION_AMBIGUOUS_IDENTITY_REFUSED",
        "storage.database_lifecycle.migration_ambiguous_identity_refused",
        detail);
  }
  if (request.downgrade_requested) {
    return DatabaseArtifactCompatibilityError(
        DatabaseOpenCompatibilityClass::downgrade_refused,
        "ENGINE.DBLC_FORMAT_DOWNGRADE_REFUSED",
        "storage.database_lifecycle.format_downgrade_refused",
        detail);
  }
  if (request.migration_plan_required && request.migration_plan_id.empty()) {
    return DatabaseArtifactCompatibilityError(
        DatabaseOpenCompatibilityClass::missing_migration_plan_refused,
        "ENGINE.DBLC_MIGRATION_PLAN_MISSING",
        "storage.database_lifecycle.migration_plan_missing",
        detail);
  }

  if (CompareArtifactVersion(request.format_major,
                             request.format_minor,
                             request.min_supported_major,
                             request.min_supported_minor) < 0) {
    return DatabaseArtifactCompatibilityError(
        DatabaseOpenCompatibilityClass::unsupported_old,
        "ENGINE.DBLC_MIGRATION_UNSUPPORTED_OLD_ARTIFACT",
        "storage.database_lifecycle.migration_unsupported_old_artifact",
        detail);
  }
  if (CompareArtifactVersion(request.format_major,
                             request.format_minor,
                             request.max_supported_major,
                             request.max_supported_minor) > 0) {
    return DatabaseArtifactCompatibilityError(
        DatabaseOpenCompatibilityClass::newer_than_supported_refused,
        "ENGINE.DBLC_FORMAT_NEWER_THAN_SUPPORTED",
        "storage.database_lifecycle.format_newer_than_supported",
        detail);
  }
  const int current_comparison = CompareArtifactVersion(request.format_major,
                                                       request.format_minor,
                                                       request.current_major,
                                                       request.current_minor);
  if (current_comparison == 0) {
    return DatabaseArtifactCompatibilityOk(DatabaseOpenCompatibilityClass::current, false);
  }
  if (current_comparison > 0) {
    return DatabaseArtifactCompatibilityError(
        DatabaseOpenCompatibilityClass::unsupported_new,
        "ENGINE.DBLC_MIGRATION_UNSUPPORTED_NEW_ARTIFACT",
        "storage.database_lifecycle.migration_unsupported_new_artifact",
        detail);
  }
  if (request.migration_plan_id.empty()) {
    return DatabaseArtifactCompatibilityError(
        DatabaseOpenCompatibilityClass::migration_required_without_plan_refused,
        "ENGINE.DBLC_MIGRATION_REQUIRED_WITHOUT_PLAN",
        "storage.database_lifecycle.migration_required_without_plan",
        detail);
  }
  if (!IsSupportedDatabaseMigrationPlan(request)) {
    return DatabaseArtifactCompatibilityError(
        DatabaseOpenCompatibilityClass::missing_migration_plan_refused,
        "ENGINE.DBLC_MIGRATION_PLAN_MISSING",
        "storage.database_lifecycle.migration_plan_missing",
        detail + " plan=" + request.migration_plan_id);
  }
  return DatabaseArtifactCompatibilityOk(DatabaseOpenCompatibilityClass::supported_migration, true);
}

DatabaseArtifactCompatibilityResult ClassifyDatabaseCatalogMigrationEvidence(
    const DatabaseCatalogMigrationEvidence& evidence) {
  if (evidence.database_catalog_record_count != 1 ||
      evidence.active_primary_filespace_record_count != 1 ||
      !evidence.database_uuid_matches_header ||
      !evidence.filespace_uuid_matches_startup) {
    return DatabaseArtifactCompatibilityError(
        DatabaseOpenCompatibilityClass::ambiguous_identity_refused,
        "ENGINE.DBLC_MIGRATION_AMBIGUOUS_IDENTITY_REFUSED",
        "storage.database_lifecycle.migration_ambiguous_identity_refused",
        "database_records=" + std::to_string(evidence.database_catalog_record_count) +
            " active_primary_filespaces=" +
            std::to_string(evidence.active_primary_filespace_record_count));
  }

  const std::array<std::pair<const char*, u32>, 4> versions = {{
      {"database_catalog_manifest", evidence.database_catalog_manifest_format_version},
      {"filespace_catalog_manifest", evidence.filespace_catalog_manifest_format_version},
      {"filespace_resource_seed_manifest", evidence.filespace_resource_seed_manifest_format_version},
      {"resource_seed_manifest", evidence.resource_seed_manifest_format_version},
  }};
  bool migration_required = false;
  for (const auto& version : versions) {
    DatabaseArtifactVersionCompatibilityRequest request;
    request.artifact_kind = version.first;
    request.format_major = version.second;
    request.format_minor = 0;
    request.min_supported_major = 0;
    request.min_supported_minor = 0;
    request.current_major = 1;
    request.current_minor = 0;
    request.max_supported_major = 1;
    request.max_supported_minor = 0;
    request.migration_plan_id = evidence.migration_plan_id;
    const auto classified = ClassifyDatabaseArtifactVersionCompatibility(request);
    if (!classified.ok()) {
      return classified;
    }
    migration_required = migration_required || classified.migration_required;
  }

  return DatabaseArtifactCompatibilityOk(
      migration_required ? DatabaseOpenCompatibilityClass::supported_migration
                         : DatabaseOpenCompatibilityClass::current,
      migration_required);
}

DatabaseArtifactCompatibilityResult CheckClusterCatalogCompatibilityEvidence(
    const ClusterCatalogCompatibilityEvidence& evidence) {
  // CLUSTER_CATALOG_COMPATIBILITY_CHECK
  if (!evidence.cluster_structures_present && evidence.entries.empty()) {
    return DatabaseArtifactCompatibilityOk(
        DatabaseOpenCompatibilityClass::current, false);
  }
  if (!evidence.external_provider_available) {
    return DatabaseArtifactCompatibilityError(
        DatabaseOpenCompatibilityClass::unsupported,
        "ENGINE.DBLC_CLUSTER_CATALOG_EXTERNAL_PROVIDER_REQUIRED",
        "storage.database_lifecycle.cluster_catalog_external_provider_required",
        "cluster catalog compatibility requires external provider evidence");
  }
  if (evidence.cluster_structures_present && evidence.entries.empty()) {
    return DatabaseArtifactCompatibilityError(
        DatabaseOpenCompatibilityClass::ambiguous_identity_refused,
        "ENGINE.DBLC_CLUSTER_CATALOG_COMPATIBILITY_MISSING",
        "storage.database_lifecycle.cluster_catalog_compatibility_missing",
        "cluster structures present without catalog version evidence");
  }

  for (const auto& entry : evidence.entries) {
    ClusterCatalogCompatibilityRequest request;
    request.table_path = entry.table_path;
    request.schema_version = entry.schema_version;
    request.codec_version = entry.codec_version;
    request.external_provider_available = evidence.external_provider_available;
    request.identity_proven = entry.identity_proven;
    request.downgrade_requested = entry.downgrade_requested;
    const auto classified = EvaluateClusterCatalogCompatibility(request);
    if (!classified.ok()) {
      DatabaseOpenCompatibilityClass compatibility_class =
          DatabaseOpenCompatibilityClass::unsupported;
      if (classified.compatibility_class ==
          ClusterCatalogCompatibilityClass::unsupported_old_schema) {
        compatibility_class = DatabaseOpenCompatibilityClass::unsupported_old;
      } else if (
          classified.compatibility_class ==
          ClusterCatalogCompatibilityClass::unsupported_new_schema) {
        compatibility_class = DatabaseOpenCompatibilityClass::unsupported_new;
      } else if (
          classified.compatibility_class ==
          ClusterCatalogCompatibilityClass::unsupported_codec) {
        compatibility_class =
            DatabaseOpenCompatibilityClass::newer_than_supported_refused;
      } else if (
          classified.compatibility_class ==
          ClusterCatalogCompatibilityClass::ambiguous_identity_refused) {
        compatibility_class =
            DatabaseOpenCompatibilityClass::ambiguous_identity_refused;
      } else if (
          classified.compatibility_class ==
          ClusterCatalogCompatibilityClass::downgrade_refused) {
        compatibility_class = DatabaseOpenCompatibilityClass::downgrade_refused;
      }
      return DatabaseArtifactCompatibilityError(
          compatibility_class,
          classified.diagnostic.diagnostic_code,
          classified.diagnostic.message_key,
          entry.table_path);
    }
  }

  (void)evidence.migration_plan_id;
  return DatabaseArtifactCompatibilityOk(
      DatabaseOpenCompatibilityClass::current, false);
}

DatabaseOpenCompatibilityClass ClassifyDatabaseOpenCompatibility(
    const DatabaseHeader& header,
    bool read_only_requested,
    const std::string& migration_plan_id) {
  DatabaseArtifactVersionCompatibilityRequest request;
  request.artifact_kind = "database_header";
  request.format_major = header.format_major;
  request.format_minor = header.format_minor;
  request.min_supported_major = scratchbird::storage::disk::kScratchBirdDatabaseFormatMajor;
  request.min_supported_minor = 0;
  request.current_major = scratchbird::storage::disk::kScratchBirdDatabaseFormatMajor;
  request.current_minor = scratchbird::storage::disk::kScratchBirdDatabaseFormatMinor;
  request.max_supported_major = scratchbird::storage::disk::kScratchBirdDatabaseFormatMajor;
  request.max_supported_minor = scratchbird::storage::disk::kScratchBirdDatabaseFormatMinor;
  request.migration_plan_id = migration_plan_id;

  const auto classified = ClassifyDatabaseArtifactVersionCompatibility(request);
  if (classified.ok()) {
    if (classified.compatibility_class == DatabaseOpenCompatibilityClass::supported_migration) {
      return read_only_requested ? DatabaseOpenCompatibilityClass::read_only_compatible
                                 : DatabaseOpenCompatibilityClass::upgrade_required;
    }
    return classified.compatibility_class;
  }
  if (classified.compatibility_class ==
          DatabaseOpenCompatibilityClass::migration_required_without_plan_refused &&
      read_only_requested) {
    return DatabaseOpenCompatibilityClass::read_only_compatible;
  }
  return classified.compatibility_class;
}

PolicySeedPackDescriptor DefaultPolicyPackDescriptor() {
  // SEARCH_KEY: DEFAULT_POLICY_PACK_IMPORT
  // PCR-128 exposes the public default pack identity. PCR-129 owns validation
  // and materialization inside the create transaction.
  PolicySeedPackDescriptor descriptor;
  descriptor.policy_pack_id = "default-local-password";
  descriptor.policy_pack_uuid = "018f7a10-1280-7000-8000-000000000001";
  descriptor.policy_pack_version = "1.0.0";
  descriptor.manifest_relative_path =
      "resources/policy-packs/default-local-password/POLICY_PACK_MANIFEST.json";
  descriptor.content_sha256 =
      "2046eabdff50fdfe662511734b3d8036e2f4890da89f4d5266cddef5bc840462";
  descriptor.create_time_only = true;
  descriptor.post_create_filesystem_authority = false;
  descriptor.local_password_only = true;
  return descriptor;
}

const char* DatabaseLifecyclePhaseName(DatabaseLifecyclePhase phase) {
  switch (phase) {
    case DatabaseLifecyclePhase::none: return "none";
    case DatabaseLifecyclePhase::created: return "created";
    case DatabaseLifecyclePhase::opened: return "opened";
    case DatabaseLifecyclePhase::closed: return "closed";
    case DatabaseLifecyclePhase::maintenance: return "maintenance";
    case DatabaseLifecyclePhase::restricted_open: return "restricted_open";
    case DatabaseLifecyclePhase::inspected: return "inspected";
    case DatabaseLifecyclePhase::verified: return "verified";
    case DatabaseLifecyclePhase::repaired: return "repaired";
    case DatabaseLifecyclePhase::dropped: return "dropped";
    case DatabaseLifecyclePhase::quarantined: return "quarantined";
    case DatabaseLifecyclePhase::failed: return "failed";
  }
  return "failed";
}

const char* DatabaseOpenCompatibilityClassName(DatabaseOpenCompatibilityClass compatibility_class) {
  switch (compatibility_class) {
    case DatabaseOpenCompatibilityClass::current: return "current";
    case DatabaseOpenCompatibilityClass::supported_migration: return "supported-migration";
    case DatabaseOpenCompatibilityClass::upgrade_required: return "upgrade-required";
    case DatabaseOpenCompatibilityClass::read_only_compatible: return "read-only-compatible";
    case DatabaseOpenCompatibilityClass::unsupported: return "unsupported";
    case DatabaseOpenCompatibilityClass::unsupported_old: return "unsupported-old";
    case DatabaseOpenCompatibilityClass::unsupported_new: return "unsupported-new";
    case DatabaseOpenCompatibilityClass::downgrade_refused: return "downgrade-refused";
    case DatabaseOpenCompatibilityClass::newer_than_supported_refused: return "newer-than-supported-refused";
    case DatabaseOpenCompatibilityClass::ambiguous_identity_refused: return "ambiguous-identity-refused";
    case DatabaseOpenCompatibilityClass::missing_migration_plan_refused: return "missing-migration-plan-refused";
    case DatabaseOpenCompatibilityClass::migration_required_without_plan_refused: return "migration-required-without-plan-refused";
  }
  return "unsupported";
}

DatabaseLifecycleResult CreateDatabaseFile(const DatabaseCreateConfig& config) {
  const auto config_result = ValidateCreateConfig(config);
  if (!config_result.ok()) {
    return config_result;
  }

  if (config.allow_minimal_resource_bootstrap &&
      !DefaultMemoryManagerState().initialized) {
    auto policy = DefaultLocalEngineMemoryPolicy();
    policy.policy_name = "database_lifecycle_minimal_resource_bootstrap";
    const auto configured = ConfigureDefaultMemoryManagerForFixture(
        policy, "database_lifecycle_minimal_resource_bootstrap");
    if (!configured.ok()) {
      return PropagateDiagnostic(configured.status, configured.diagnostic);
    }
  }

  LoadedPolicySeedPack policy_seed_pack;
  if (!config.policy_seed_pack_root.empty()) {
    const auto loaded_policy_pack = LoadSelectedPolicySeedPack(config.policy_seed_pack_root);
    if (!loaded_policy_pack.ok()) {
      return PropagateDiagnostic(loaded_policy_pack.status, loaded_policy_pack.diagnostic);
    }
    policy_seed_pack = loaded_policy_pack.pack;
  } else if (config.require_policy_seed_pack) {
    return LifecycleError("SB-POLICY-PACK-ROOT-REQUIRED",
                          "storage.database_lifecycle.policy_pack_required",
                          config.path);
  }

  auto bootstrap_memory = DefaultMemoryManager().AllocateScoped(
      config.page_size,
      alignof(std::max_align_t),
      MemoryTag{Subsystem::storage_disk,
                "database_create_bootstrap_scratch",
                MemoryCategory::catalog_bootstrap,
                MemoryLifetime::database,
                "storage.database.lifecycle",
                "create"});
  if (!bootstrap_memory.ok()) {
    return PropagateDiagnostic(bootstrap_memory.status, bootstrap_memory.diagnostic);
  }

  const auto header_result = MakeDatabaseHeader(config.database_uuid.value,
                                                config.page_size,
                                                config.creation_unix_epoch_millis,
                                                config.feature_flags,
                                                config.compatibility_flags);
  if (!header_result.ok()) {
    return PropagateDiagnostic(header_result.status, header_result.diagnostic);
  }

  const auto serialized = SerializeDatabaseHeader(header_result.header);
  if (!serialized.ok()) {
    return PropagateDiagnostic(serialized.status, serialized.diagnostic);
  }

  if (config.allow_overwrite) {
    std::error_code ignored;
    std::filesystem::remove(config.path + ".sb.crud_events", ignored);
    std::filesystem::remove(config.path + ".sb.api_events", ignored);
  }

  FileDevice device;
  const auto open = device.Open(config.path,
                                config.allow_overwrite ? FileOpenMode::create_or_truncate : FileOpenMode::create_new);
  if (!open.ok()) {
    return PropagateDiagnostic(open.status, open.diagnostic);
  }
  const auto create_health = CheckDiskDeviceHealth(device,
                                                   LifecycleDiskPolicy(config.page_size, false, false));
  if (!create_health.ok()) {
    return PropagateStorageFailure(create_health.status,
                                   create_health.diagnostic,
                                   config.path,
                                   "create.open",
                                   false);
  }

  const auto write_header = device.WriteAt(0, serialized.serialized.data(), serialized.serialized.size());
  if (!write_header.ok()) {
    return PropagateStorageFailure(write_header.status,
                                   write_header.diagnostic,
                                   config.path,
                                   "create.database_header",
                                   true);
  }

  PageManagerContext page_context;
  page_context.page_size = config.page_size;
  page_context.database_uuid = config.database_uuid;
  page_context.filespace_uuid = config.filespace_uuid;
  page_context.cluster_authority_active = false;

  const std::array<std::pair<PageType, u64>, 5> initial_pages = {{
      {PageType::system_state, kSystemStatePageNumber},
      {PageType::catalog, kCatalogPageNumber},
      {PageType::allocation_map, kAllocationMapPageNumber},
      {PageType::transaction_inventory, kTransactionInventoryPageNumber},
      {PageType::bootstrap_reserved, kBootstrapReservedPageNumber},
  }};

  for (const auto& entry : initial_pages) {
    const auto page_result = WriteInitialPageHeader(&device,
                                                    page_context,
                                                    entry.first,
                                                    entry.second,
                                                    config.creation_unix_epoch_millis);
    if (!page_result.ok()) {
      return MarkStoragePartial(page_result, config.path, "create.page_headers");
    }
  }

  StartupStateRecord initial_startup_state =
      MakeInitialStartupState(config.database_uuid, config.filespace_uuid, config.page_size);
  LifecycleTransactionEvidence bootstrap_evidence;
  const auto write_txn_inventory =
      WriteInitialTransactionInventoryPage(&device,
                                           page_context,
                                           config.creation_unix_epoch_millis,
                                           &bootstrap_evidence);
  if (!write_txn_inventory.ok()) {
    return MarkStoragePartial(write_txn_inventory, config.path, "create.transaction_inventory");
  }
  initial_startup_state.bootstrap_local_transaction_id =
      bootstrap_evidence.committed_local_transaction_id;
  initial_startup_state = RecordStartupLifecycleEvidence(
      std::move(initial_startup_state),
      StartupLifecycleDurablePhase::create_tx1_committed,
      bootstrap_evidence.committed_local_transaction_id,
      config.creation_unix_epoch_millis,
      StartupLifecycleEvidenceFlag::bootstrap_tx1_committed);
  initial_startup_state.completed_phases.push_back("create.bootstrap_transaction_committed");

  ResourceSeedCatalogImage resource_seed_catalog;
  if (!config.resource_seed_pack_root.empty()) {
    ResourceSeedLoadConfig resource_config;
    resource_config.seed_pack_root = config.resource_seed_pack_root;
    resource_config.allow_minimal_bootstrap = config.allow_minimal_resource_bootstrap;
    const auto loaded_resources = LoadResourceSeedPack(resource_config);
    if (!loaded_resources.ok()) {
      return PropagateDiagnostic(loaded_resources.status, loaded_resources.diagnostic);
    }
    resource_seed_catalog = loaded_resources.image;
  } else if (config.allow_minimal_resource_bootstrap) {
    resource_seed_catalog.seed_pack_name = "minimal-bootstrap";
    resource_seed_catalog.seed_pack_version = "0";
    resource_seed_catalog.seed_pack_root = "";
    resource_seed_catalog.manifest_path = "";
    resource_seed_catalog.content_hash = "";
    resource_seed_catalog.active = false;
    resource_seed_catalog.minimal_bootstrap = true;
  }

  if (policy_seed_pack.image.active &&
      !resource_seed_catalog.active &&
      !resource_seed_catalog.minimal_bootstrap) {
    resource_seed_catalog.seed_pack_name = "minimal-bootstrap";
    resource_seed_catalog.seed_pack_version = "0";
    resource_seed_catalog.seed_pack_root = "";
    resource_seed_catalog.manifest_path = "";
    resource_seed_catalog.content_hash = "";
    resource_seed_catalog.active = false;
    resource_seed_catalog.minimal_bootstrap = true;
  }

  if (config.require_resource_seed_pack && !resource_seed_catalog.active) {
    return LifecycleError("SB_RESOURCE_SEED_MISSING",
                          "storage.database_lifecycle.resource_seed_pack_required",
                          config.path);
  }

  DatabaseCatalogSummaryResult created_catalog_summary;
  created_catalog_summary.status = DatabaseLifecycleOkStatus();
  if (resource_seed_catalog.active || resource_seed_catalog.minimal_bootstrap ||
      policy_seed_pack.image.active) {
    const auto catalog_rows = BuildCreateCatalogRows(config,
                                                     resource_seed_catalog,
                                                     policy_seed_pack);
    if (!catalog_rows.ok()) {
      return PropagateDiagnostic(catalog_rows.status, catalog_rows.diagnostic);
    }
    const auto filespace_manifest = ValidateFilespaceCatalogManifest(catalog_rows.rows,
                                                                     config.database_uuid,
                                                                     config.filespace_uuid,
                                                                     config.path);
    if (!filespace_manifest.ok()) {
      return filespace_manifest;
    }
    created_catalog_summary = BuildTypedCatalogSummary(catalog_rows.rows);
    if (!created_catalog_summary.ok()) {
      return PropagateDiagnostic(created_catalog_summary.status, created_catalog_summary.diagnostic);
    }
    const auto write_catalog = WriteCatalogPageBodies(&device,
                                                      page_context,
                                                      catalog_rows.rows,
                                                      config.creation_unix_epoch_millis);
    if (!write_catalog.ok()) {
      return MarkStoragePartial(write_catalog, config.path, "create.catalog_pages");
    }
    initial_startup_state.completed_phases.push_back("create.first_filespace_manifest_registered");
  }

  const auto write_startup_state = WriteStartupStatePageBody(&device, initial_startup_state);
  if (!write_startup_state.ok()) {
    return PropagateDiagnostic(write_startup_state.status, write_startup_state.diagnostic);
  }

  const auto sync = SyncFileDeviceWithPolicy(&device,
                                             LifecycleDiskPolicy(config.page_size, false, true));
  if (!sync.ok()) {
    return PropagateStorageFailure(sync.status,
                                   sync.diagnostic,
                                   config.path,
                                   "create.sync",
                                   true);
  }
  const auto final_create_health = CheckDiskDeviceHealth(device,
                                                         LifecycleDiskPolicy(config.page_size, false, true));
  if (!final_create_health.ok()) {
    return PropagateStorageFailure(final_create_health.status,
                                   final_create_health.diagnostic,
                                   config.path,
                                   "create.final_health",
                                   true);
  }
  const auto close = device.Close();
  if (!close.ok()) {
    return PropagateStorageFailure(close.status,
                                   close.diagnostic,
                                   config.path,
                                   "create.close",
                                   true);
  }

  DatabaseLifecycleResult result;
  result.status = DatabaseLifecycleOkStatus();
  result.state = MakeState(config.path,
                           header_result.header,
                           config.database_uuid,
                           config.filespace_uuid,
                           DatabaseLifecyclePhase::created,
                           false,
                           false,
                           false,
                           final_create_health.snapshot,
                           resource_seed_catalog,
                           policy_seed_pack.image,
                           created_catalog_summary,
                           bootstrap_evidence.inventory,
                           bootstrap_evidence.horizons,
                           initial_startup_state);
  return result;
}

DatabaseLifecycleResult OpenDatabaseFile(const DatabaseOpenConfig& config) {
  if (config.path.empty()) {
    return LifecycleError("SB-DB-LIFECYCLE-OPEN-PATH-REQUIRED",
                          "storage.database_lifecycle.open_path_required");
  }

  FileDevice device;
  const auto open = device.Open(config.path, config.read_only ? FileOpenMode::open_existing_read_only : FileOpenMode::open_existing);
  if (!open.ok()) {
    return PropagateDiagnostic(open.status, open.diagnostic);
  }

  SerializedDatabaseHeader serialized{};
  const auto read = device.ReadAt(0, serialized.data(), serialized.size());
  if (!read.ok()) {
    return PropagateDiagnostic(read.status, read.diagnostic);
  }

  const auto parsed = ParseDatabaseHeader(serialized);
  if (!parsed.ok()) {
    return PropagateDiagnostic(parsed.status, parsed.diagnostic);
  }
  auto compatibility_class =
      ClassifyDatabaseOpenCompatibility(parsed.header, config.read_only, config.migration_plan_id);
  const std::string database_format_detail =
      "database_format=" + std::to_string(parsed.header.format_major) + "." +
      std::to_string(parsed.header.format_minor);
  if (compatibility_class == DatabaseOpenCompatibilityClass::unsupported ||
      compatibility_class == DatabaseOpenCompatibilityClass::unsupported_old ||
      compatibility_class == DatabaseOpenCompatibilityClass::unsupported_new ||
      compatibility_class == DatabaseOpenCompatibilityClass::downgrade_refused ||
      compatibility_class == DatabaseOpenCompatibilityClass::newer_than_supported_refused ||
      compatibility_class == DatabaseOpenCompatibilityClass::missing_migration_plan_refused ||
      compatibility_class == DatabaseOpenCompatibilityClass::migration_required_without_plan_refused ||
      compatibility_class == DatabaseOpenCompatibilityClass::ambiguous_identity_refused) {
    const char* code = "FORMAT.VERSION_UNSUPPORTED";
    const char* message_key = "storage.database_lifecycle.format_version_unsupported";
    if (compatibility_class == DatabaseOpenCompatibilityClass::unsupported_old) {
      code = "ENGINE.DBLC_MIGRATION_UNSUPPORTED_OLD_ARTIFACT";
      message_key = "storage.database_lifecycle.migration_unsupported_old_artifact";
    } else if (compatibility_class == DatabaseOpenCompatibilityClass::unsupported_new) {
      code = "ENGINE.DBLC_MIGRATION_UNSUPPORTED_NEW_ARTIFACT";
      message_key = "storage.database_lifecycle.migration_unsupported_new_artifact";
    } else if (compatibility_class == DatabaseOpenCompatibilityClass::downgrade_refused) {
      code = "ENGINE.DBLC_FORMAT_DOWNGRADE_REFUSED";
      message_key = "storage.database_lifecycle.format_downgrade_refused";
    } else if (compatibility_class == DatabaseOpenCompatibilityClass::newer_than_supported_refused) {
      code = "ENGINE.DBLC_FORMAT_NEWER_THAN_SUPPORTED";
      message_key = "storage.database_lifecycle.format_newer_than_supported";
    } else if (compatibility_class == DatabaseOpenCompatibilityClass::missing_migration_plan_refused) {
      code = "ENGINE.DBLC_MIGRATION_PLAN_MISSING";
      message_key = "storage.database_lifecycle.migration_plan_missing";
    } else if (compatibility_class ==
               DatabaseOpenCompatibilityClass::migration_required_without_plan_refused) {
      code = "ENGINE.DBLC_MIGRATION_REQUIRED_WITHOUT_PLAN";
      message_key = "storage.database_lifecycle.migration_required_without_plan";
    } else if (compatibility_class == DatabaseOpenCompatibilityClass::ambiguous_identity_refused) {
      code = "ENGINE.DBLC_MIGRATION_AMBIGUOUS_IDENTITY_REFUSED";
      message_key = "storage.database_lifecycle.migration_ambiguous_identity_refused";
    }
    return LifecycleError(code, message_key, config.path, database_format_detail);
  }
  if (compatibility_class == DatabaseOpenCompatibilityClass::upgrade_required && !config.read_only) {
    return LifecycleError("FORMAT.UPGRADE_REQUIRED",
                          "storage.database_lifecycle.format_upgrade_required",
                          config.path,
                          database_format_detail);
  }
  const bool cluster_authority_required =
      (parsed.header.compatibility_flags &
       scratchbird::storage::disk::DatabaseCompatibilityFlag::requires_cluster_authority) != 0 ||
      (parsed.header.feature_flags &
       scratchbird::storage::disk::DatabaseFeatureFlag::cluster_structures_present) != 0;
  if (cluster_authority_required) {
    if (!config.cluster_authority_available) {
      return LifecycleError("SB-DB-LIFECYCLE-CLUSTER-AUTHORITY-REQUIRED",
                            "storage.database_lifecycle.cluster_authority_required",
                            config.path);
    }
    return LifecycleError("SB-DB-LIFECYCLE-CLUSTER-MAPPING-UNAVAILABLE",
                          "storage.database_lifecycle.cluster_mapping_unavailable",
                          config.path);
  }
  if ((parsed.header.compatibility_flags &
       scratchbird::storage::disk::DatabaseCompatibilityFlag::requires_decryption_password) != 0 &&
      !config.decryption_available) {
    return LifecycleError("SB-DB-LIFECYCLE-DECRYPTION-REQUIRED",
                          "storage.database_lifecycle.decryption_required",
                          config.path);
  }

  const auto open_policy = LifecycleDiskPolicy(parsed.header.page_size, config.read_only, true);
  const auto open_health = CheckDiskDeviceHealth(device, open_policy);
  if (!open_health.ok()) {
    return PropagateStorageFailure(open_health.status,
                                   open_health.diagnostic,
                                   config.path,
                                   "open.health",
                                   false);
  }
  const auto core_pages = ValidateCorePageHeaders(&device, parsed.header.page_size, open_policy);
  if (!core_pages.ok()) {
    return core_pages;
  }

  TypedUuid database_uuid;
  database_uuid.kind = UuidKind::database;
  database_uuid.value = parsed.header.database_uuid;

  auto startup_state = ReadStartupStatePageBody(&device, parsed.header.page_size);
  if (!startup_state.ok()) {
    return PropagateDiagnostic(startup_state.status, startup_state.diagnostic);
  }
  if (!(startup_state.state.database_uuid.value == database_uuid.value)) {
    return LifecycleError("ENGINE.DBLC_MIGRATION_AMBIGUOUS_IDENTITY_REFUSED",
                          "storage.database_lifecycle.migration_ambiguous_identity_refused",
                          config.path);
  }
  const auto startup_identities = ValidateStartupPageIdentities(&device,
                                                               parsed.header.page_size,
                                                               open_policy,
                                                               database_uuid,
                                                               startup_state.state.first_filespace_uuid);
  if (!startup_identities.ok()) {
    return startup_identities;
  }
  StartupRecoveryClassification classification = ClassifyStartupStateForOpen(startup_state.state);
  startup_state.state.recovery_classification = classification;
  if (!config.read_only &&
      classification != StartupRecoveryClassification::clean_checkpoint_path &&
      classification != StartupRecoveryClassification::repaired_recovery) {
    return LifecycleError("SB-DB-LIFECYCLE-RESTRICTED-OPEN-REQUIRED",
                          "storage.database_lifecycle.restricted_open_required",
                          config.path,
                          StartupRecoveryClassificationName(classification));
  }

  std::vector<CatalogPageRow> catalog_rows;
  ResourceSeedCatalogImage resource_seed_catalog;
  PolicySeedPackCatalogImage policy_seed_catalog;
  const auto read_catalog_rows = ReadCatalogPageRows(&device, parsed.header.page_size, &catalog_rows);
  if (read_catalog_rows.ok()) {
    const auto catalog_migration =
        ValidateCatalogMigrationEvidence(catalog_rows,
                                         database_uuid,
                                         startup_state.state.first_filespace_uuid,
                                         config.migration_plan_id,
                                         config.path);
    if (!catalog_migration.ok()) {
      return catalog_migration;
    }
    resource_seed_catalog = BuildResourceImageFromCatalogRows(catalog_rows);
    policy_seed_catalog = BuildPolicyImageFromCatalogRows(catalog_rows);
  } else {
    return read_catalog_rows;
  }
  const auto filespace_manifest = ValidateFilespaceCatalogManifest(catalog_rows,
                                                                  database_uuid,
                                                                  startup_state.state.first_filespace_uuid,
                                                                  config.path);
  if (!filespace_manifest.ok()) {
    return filespace_manifest;
  }
  const bool seed_catalog_present =
      resource_seed_catalog.active ||
      resource_seed_catalog.minimal_bootstrap ||
      !resource_seed_catalog.artifacts.empty() ||
      !resource_seed_catalog.seed_pack_name.empty() ||
      !resource_seed_catalog.content_hash.empty();
  const bool seed_expectation_present =
      !config.expected_resource_seed_pack_name.empty() ||
      !config.expected_resource_seed_pack_version.empty() ||
      !config.expected_resource_seed_pack_content_hash.empty();
  if (seed_expectation_present && !seed_catalog_present) {
    return LifecycleError("SB_RESOURCE_SEED_MISSING",
                          "storage.database_lifecycle.resource_seed_pack_required",
                          config.path);
  }
  if (seed_catalog_present) {
    const auto resource_seed_validation =
        ValidateResourceSeedCatalogImage(resource_seed_catalog,
                                         resource_seed_catalog.minimal_bootstrap);
    if (!resource_seed_validation.ok()) {
      return PropagateDiagnostic(resource_seed_validation.status, resource_seed_validation.diagnostic);
    }
  }
  const auto policy_seed_validation =
      ValidatePolicySeedCatalogImage(policy_seed_catalog, config.path);
  if (!policy_seed_validation.ok()) {
    return policy_seed_validation;
  }
  auto catalog_summary = BuildTypedCatalogSummary(catalog_rows);
  if (!catalog_summary.ok()) {
    return PropagateDiagnostic(catalog_summary.status, catalog_summary.diagnostic);
  }
  std::string seed_pack_mismatch_detail;
  if (ResourceSeedPackMismatch(resource_seed_catalog, config, &seed_pack_mismatch_detail)) {
    if (!config.read_only) {
      return LifecycleError("FORMAT.UPGRADE_REQUIRED",
                            "storage.database_lifecycle.seed_pack_upgrade_required",
                            config.path,
                            seed_pack_mismatch_detail);
    }
    compatibility_class = DatabaseOpenCompatibilityClass::read_only_compatible;
  }

  const auto dirty_manifest =
      ClassifyDirtyManifestSidecarForOpen(config.path, startup_state.state);
  if (!dirty_manifest.ok()) {
    return PropagateDiagnostic(dirty_manifest.status, dirty_manifest.diagnostic);
  }

  const bool stale_owner_evidence_present =
      startup_state.state.startup_dirty && !startup_state.state.owner_token.empty();
  if (!config.read_only) {
    const u64 dirty_mark_unix_epoch_millis = CurrentUnixEpochMillis();
    startup_state.state = MarkStartupDirty(startup_state.state,
                                           std::string("owner:") + config.path,
                                           classification);
    if (stale_owner_evidence_present) {
      startup_state.state.completed_phases.push_back("open.stale_owner_evidence_classified");
    }
    startup_state.state = RecordStartupLifecycleEvidence(
        std::move(startup_state.state),
        StartupLifecycleDurablePhase::open_dirty_marked,
        0,
        dirty_mark_unix_epoch_millis,
        StartupLifecycleEvidenceFlag::startup_owner_token_persisted);
    const auto dirty_mark = WriteStartupStatePageBody(&device, startup_state.state);
    if (!dirty_mark.ok()) {
      return PropagateDiagnostic(dirty_mark.status, dirty_mark.diagnostic);
    }
  }

  LocalTransactionInventory local_transaction_inventory;
  LocalTransactionHorizons local_transaction_horizons;
  const auto read_txn_inventory = ReadTransactionInventoryPage(&device,
                                                              parsed.header.page_size,
                                                              &local_transaction_inventory,
                                                              &local_transaction_horizons);
  if (!read_txn_inventory.ok()) {
    return read_txn_inventory;
  }
  const auto bootstrap_evidence =
      ValidateBootstrapLifecycleEvidence(startup_state.state,
                                         local_transaction_inventory,
                                         config.path);
  if (!bootstrap_evidence.ok()) {
    return bootstrap_evidence;
  }
  if (startup_state.state.first_open_activation_local_transaction_id != 0) {
    if (!StartupLifecycleEvidencePresent(startup_state.state,
                                         StartupLifecycleEvidenceFlag::first_open_tx2_committed)) {
      return LifecycleError("SB-DB-LIFECYCLE-ACTIVATION-EVIDENCE-MISSING",
                            "storage.database_lifecycle.activation_evidence_missing",
                            config.path,
                            "first_open_tx2_committed");
    }
    const auto activation_evidence = ValidateCommittedLifecycleTransaction(
        local_transaction_inventory,
        startup_state.state.first_open_activation_local_transaction_id,
        config.path,
        "SB-DB-LIFECYCLE-ACTIVATION-TXN-MISSING",
        "SB-DB-LIFECYCLE-ACTIVATION-TXN-INVALID-STATE");
    if (!activation_evidence.ok()) {
      return activation_evidence;
    }
  }
  if (startup_state.state.clean_shutdown_local_transaction_id != 0) {
    if (!StartupLifecycleEvidencePresent(startup_state.state,
                                         StartupLifecycleEvidenceFlag::clean_shutdown_tx_committed)) {
      return LifecycleError("SB-DB-LIFECYCLE-SHUTDOWN-EVIDENCE-MISSING",
                            "storage.database_lifecycle.shutdown_evidence_missing",
                            config.path,
                            "clean_shutdown_tx_committed");
    }
    const auto clean_shutdown_evidence = ValidateCommittedLifecycleTransaction(
        local_transaction_inventory,
        startup_state.state.clean_shutdown_local_transaction_id,
        config.path,
        "SB-DB-LIFECYCLE-SHUTDOWN-TXN-MISSING",
        "SB-DB-LIFECYCLE-SHUTDOWN-TXN-INVALID-STATE");
    if (!clean_shutdown_evidence.ok()) {
      return clean_shutdown_evidence;
    }
  }

  bool transaction_recovery_changed_inventory = false;
  bool transaction_recovery_requires_write_fence = false;
  bool first_open_activation_was_required = false;
  if (config.read_only) {
    const auto recovery_classification = ClassifyLocalTransactionInventoryForRecovery(local_transaction_inventory);
    if (!recovery_classification.ok()) {
      return PropagateDiagnostic(recovery_classification.status, recovery_classification.diagnostic);
    }
    transaction_recovery_requires_write_fence = recovery_classification.write_admission_must_remain_fenced;
  } else {
    const auto recovery = ApplyLocalTransactionInventoryRecovery(local_transaction_inventory, CurrentUnixEpochMillis());
    if (!recovery.ok()) {
      return PropagateDiagnostic(recovery.status, recovery.diagnostic);
    }
    transaction_recovery_changed_inventory = recovery.inventory_changed;
    transaction_recovery_requires_write_fence = recovery.write_admission_must_remain_fenced;
    local_transaction_inventory = recovery.recovered_inventory;
    const auto recovered_horizons = ComputeLocalTransactionHorizons(local_transaction_inventory);
    if (!recovered_horizons.ok()) {
      return PropagateDiagnostic(recovered_horizons.status, recovered_horizons.diagnostic);
    }
    local_transaction_horizons = recovered_horizons.horizons;
    if (transaction_recovery_requires_write_fence) {
      return LifecycleError("SB-MGA-RECOVERY-WRITE-FENCE-REQUIRED",
                            "transaction.recovery.write_fence_required",
                            config.path);
    }
    if (transaction_recovery_changed_inventory) {
      const auto write_recovered_inventory =
          WriteTransactionInventoryPage(&device, parsed.header.page_size, local_transaction_inventory);
      if (!write_recovered_inventory.ok()) {
        return write_recovered_inventory;
      }
    }
    const auto dirty_manifest_evidence =
        PersistDirtyManifestRecoveryEvidenceForOpen(config.path, dirty_manifest);
    if (!dirty_manifest_evidence.ok()) {
      return dirty_manifest_evidence;
    }
    first_open_activation_was_required =
        startup_state.state.first_open_activation_local_transaction_id == 0;
    const auto activation = EnsureFirstOpenActivationTransaction(&device,
                                                                parsed.header.page_size,
                                                                &startup_state.state,
                                                                &local_transaction_inventory,
                                                                &local_transaction_horizons,
                                                                config.path,
                                                                config.suppress_background_agents);
    if (!activation.ok()) {
      return activation;
    }
  }

  if (!config.read_only) {
    startup_state.state.write_admission_fenced = false;
    startup_state.state.config_authority_loaded = true;
    startup_state.state.security_authority_loaded = true;
    startup_state.state.i18n_authority_loaded = true;
    startup_state.state.completed_phases.push_back("open.transaction_inventory_reconciled");
    if (transaction_recovery_changed_inventory) {
      startup_state.state.completed_phases.push_back("open.transaction_inventory_recovery_applied");
    }
    if (dirty_manifest.manifest_present) {
      startup_state.state.completed_phases.push_back("open.dirty_manifest_classified");
      if (dirty_manifest.recovery_evidence_required) {
        startup_state.state.completed_phases.push_back("open.dirty_manifest_recovery_evidence_persisted");
      }
    } else if (classification == StartupRecoveryClassification::repaired_recovery) {
      startup_state.state.completed_phases.push_back("open.dirty_manifest_absent_rebuild_by_scan");
    }
    startup_state.state.completed_phases.push_back("open.first_filespace_manifest_validated");
    startup_state.state.completed_phases.push_back("open.catalog_root_loaded");
    startup_state.state.completed_phases.push_back("open.config_security_i18n_loaded");
    startup_state.state.completed_phases.push_back("open.write_admission_fence_cleared");
    const auto cache_preload = RecordPageCachePreloadEvidence(&device,
                                                              parsed.header.page_size,
                                                              &startup_state.state,
                                                              database_uuid,
                                                              config.path);
    if (!cache_preload.ok()) {
      return cache_preload;
    }
    const u64 activation_local_transaction_id =
        first_open_activation_was_required
            ? startup_state.state.first_open_activation_local_transaction_id
            : 0;
    startup_state.state = RecordStartupLifecycleEvidence(
        std::move(startup_state.state),
        StartupLifecycleDurablePhase::open_ready,
        activation_local_transaction_id,
        CurrentUnixEpochMillis(),
        StartupLifecycleEvidenceFlag::authorities_ready |
            StartupLifecycleEvidenceFlag::first_open_tx2_committed |
            StartupLifecycleEvidenceFlag::cache_preload_completed);
    const auto fence_clear = WriteStartupStatePageBody(&device, startup_state.state);
    if (!fence_clear.ok()) {
      return PropagateDiagnostic(fence_clear.status, fence_clear.diagnostic);
    }
  }

  const auto close = device.Close();
  if (!close.ok()) {
    return PropagateDiagnostic(close.status, close.diagnostic);
  }

  DatabaseLifecycleResult result;
  result.status = DatabaseLifecycleOkStatus();
  result.state = MakeState(config.path,
                           parsed.header,
                           database_uuid,
                           startup_state.state.first_filespace_uuid,
                           DatabaseLifecyclePhase::opened,
                           config.cluster_authority_available,
                           config.decryption_available,
                           config.read_only,
                           open_health.snapshot,
                           resource_seed_catalog,
                           policy_seed_catalog,
                           catalog_summary,
                           local_transaction_inventory,
                           local_transaction_horizons,
                           startup_state.state,
                           compatibility_class);
  return result;
}

DatabaseLifecycleResult EnterDatabaseMaintenanceMode(const DatabaseLifecycleOperationConfig& config) {
  auto classified = ClassifyReadOnlyLifecycle(ReadOnlyLifecycleOpenConfig(config),
                                              DatabaseLifecyclePhase::maintenance);
  if (!classified.ok() || !config.write_evidence) {
    return classified;
  }
  return RecordStartupEvidence(config,
                               DatabaseLifecyclePhase::maintenance,
                               StartupLifecycleDurablePhase::maintenance_entered,
                               StartupLifecycleEvidenceFlag::maintenance_evidence_recorded,
                               true,
                               false,
                               StartupRecoveryClassification::fence_writes_until_safe,
                               "maintenance.entered");
}

DatabaseLifecycleResult ExitDatabaseMaintenanceMode(const DatabaseLifecycleOperationConfig& config) {
  auto classified = ClassifyReadOnlyLifecycle(ReadOnlyLifecycleOpenConfig(config),
                                              DatabaseLifecyclePhase::maintenance);
  if (!classified.ok() || !config.write_evidence) {
    return classified;
  }
  if (classified.state.startup_state.startup_dirty) {
    return DblcLifecycleError("ENGINE.DBLC_REPAIR_REFUSED",
                              "storage.database_lifecycle.maintenance_exit_requires_clean_startup",
                              config.path,
                              "startup_dirty");
  }
  return RecordStartupEvidence(config,
                               DatabaseLifecyclePhase::opened,
                               StartupLifecycleDurablePhase::maintenance_exited,
                               StartupLifecycleEvidenceFlag::maintenance_evidence_recorded,
                               false,
                               true,
                               StartupRecoveryClassification::clean_checkpoint_path,
                               "maintenance.exited");
}

DatabaseLifecycleResult EnterDatabaseRestrictedOpenMode(const DatabaseLifecycleOperationConfig& config) {
  auto classified = ClassifyReadOnlyLifecycle(ReadOnlyLifecycleOpenConfig(config),
                                              DatabaseLifecyclePhase::restricted_open);
  if (!classified.ok() || !config.write_evidence) {
    return classified;
  }
  return RecordStartupEvidence(config,
                               DatabaseLifecyclePhase::restricted_open,
                               StartupLifecycleDurablePhase::restricted_open_entered,
                               StartupLifecycleEvidenceFlag::restricted_open_evidence_recorded,
                               true,
                               false,
                               StartupRecoveryClassification::restricted_open_required,
                               "restricted_open.entered");
}

DatabaseLifecycleResult ExitDatabaseRestrictedOpenMode(const DatabaseLifecycleOperationConfig& config) {
  auto classified = ClassifyReadOnlyLifecycle(ReadOnlyLifecycleOpenConfig(config),
                                              DatabaseLifecyclePhase::restricted_open);
  if (!classified.ok() || !config.write_evidence) {
    return classified;
  }
  if (classified.state.startup_state.startup_dirty) {
    return DblcLifecycleError("ENGINE.DBLC_REPAIR_REFUSED",
                              "storage.database_lifecycle.restricted_exit_requires_clean_startup",
                              config.path,
                              "startup_dirty");
  }
  return RecordStartupEvidence(config,
                               DatabaseLifecyclePhase::opened,
                               StartupLifecycleDurablePhase::restricted_open_exited,
                               StartupLifecycleEvidenceFlag::restricted_open_evidence_recorded,
                               false,
                               true,
                               StartupRecoveryClassification::clean_checkpoint_path,
                               "restricted_open.exited");
}

DatabaseLifecycleResult InspectDatabaseLifecycle(const DatabaseLifecycleOperationConfig& config) {
  return ClassifyReadOnlyLifecycle(ReadOnlyLifecycleOpenConfig(config),
                                   DatabaseLifecyclePhase::inspected);
}

DatabaseLifecycleResult VerifyDatabaseLifecycle(const DatabaseLifecycleOperationConfig& config) {
  auto verified = ClassifyReadOnlyLifecycle(ReadOnlyLifecycleOpenConfig(config),
                                            DatabaseLifecyclePhase::verified);
  if (!verified.ok() || !config.write_evidence) {
    return verified;
  }
  return RecordStartupEvidence(config,
                               DatabaseLifecyclePhase::verified,
                               StartupLifecycleDurablePhase::verify_completed,
                               StartupLifecycleEvidenceFlag::verify_evidence_recorded,
                               verified.state.startup_state.write_admission_fenced,
                               false,
                               verified.state.startup_state.recovery_classification,
                               "verify.completed");
}

DatabaseLifecycleResult RepairDatabaseLifecycle(const DatabaseLifecycleRepairConfig& config) {
  auto verified = ClassifyReadOnlyLifecycle(ReadOnlyRepairOpenConfig(config),
                                            DatabaseLifecyclePhase::verified);
  if (!verified.ok()) {
    if (verified.diagnostic.diagnostic_code.find("CLUSTER") != std::string::npos) {
      return DblcLifecycleError("ENGINE.DBLC_STANDALONE_CLUSTER_FAIL_CLOSED",
                                "storage.database_lifecycle.standalone_cluster_fail_closed",
                                config.path,
                                verified.diagnostic.diagnostic_code);
    }
    return DblcLifecycleError("ENGINE.DBLC_REPAIR_REFUSED",
                              "storage.database_lifecycle.repair_refused",
                              config.path,
                              verified.diagnostic.diagnostic_code.empty()
                                  ? "verify_failed"
                                  : verified.diagnostic.diagnostic_code);
  }
  if (!config.repair_admission_proven || !config.allow_mutation) {
    return DblcLifecycleError("ENGINE.DBLC_REPAIR_REFUSED",
                              "storage.database_lifecycle.repair_refused",
                              config.path,
                              "repair_requires_restricted_or_maintenance_admission_and_mutation_authority");
  }
  if (!ExpectedIdentityMatches(verified.state,
                               config.expected_database_uuid,
                               config.expected_filespace_uuid)) {
    return DblcLifecycleError("ENGINE.DBLC_REPAIR_REFUSED",
                              "storage.database_lifecycle.repair_refused",
                              config.path,
                              "repair_requires_exact_database_and_filespace_uuid_proof");
  }
  if (config.repair_plan_id != "clear_verified_write_fence" &&
      config.repair_plan_id != "record_verified_repair_evidence") {
    return DblcLifecycleError("ENGINE.DBLC_REPAIR_REFUSED",
                              "storage.database_lifecycle.repair_refused",
                              config.path,
                              "unsupported_or_missing_repair_plan_id");
  }

  DatabaseLifecycleOperationConfig operation_config;
  operation_config.path = config.path;
  operation_config.cluster_authority_available = config.cluster_authority_available;
  operation_config.decryption_available = config.decryption_available;
  operation_config.operation_uuid = config.operation_uuid;
  operation_config.actor_uuid = config.actor_uuid;
  operation_config.write_evidence = true;
  const bool clear_write_fence = config.repair_plan_id == "clear_verified_write_fence";
  return RecordStartupEvidence(operation_config,
                               DatabaseLifecyclePhase::repaired,
                               StartupLifecycleDurablePhase::repair_completed,
                               StartupLifecycleEvidenceFlag::repair_evidence_recorded,
                               clear_write_fence ? false : verified.state.startup_state.write_admission_fenced,
                               clear_write_fence,
                               clear_write_fence
                                   ? StartupRecoveryClassification::clean_checkpoint_path
                                   : verified.state.startup_state.recovery_classification,
                               std::string("repair.") + config.repair_plan_id);
}

DatabaseLifecycleResult DropDatabaseLifecycle(const DatabaseDropConfig& config) {
  auto unsafe = [&](std::string detail) {
    return DblcLifecycleError("ENGINE.DBLC_DROP_UNSAFE",
                              "storage.database_lifecycle.drop_unsafe",
                              config.path,
                              std::move(detail));
  };
  if (config.path.empty()) {
    return unsafe("drop_path_required");
  }
  const bool logical = config.drop_mode == "logical" || config.drop_mode == "logical_preserve";
  const bool quarantine = config.drop_mode == "quarantine";
  const bool physical_delete = config.drop_mode == "physical_delete";
  if (!logical && !quarantine && !physical_delete) {
    return unsafe("unsupported_drop_mode");
  }
  if (!config.drop_safety_preconditions ||
      !config.session_drain_complete ||
      !config.ownership_release_verified ||
      !config.retention_policy_satisfied ||
      !config.backup_coverage_verified ||
      !config.legal_hold_clear) {
    return unsafe("drop_requires_drain_ownership_retention_backup_and_legal_hold_proof");
  }
  if (quarantine && !config.allow_quarantine) {
    return unsafe("drop_quarantine_requires_explicit_policy");
  }
  if (physical_delete && !config.allow_physical_delete) {
    return unsafe("drop_physical_delete_requires_explicit_policy");
  }

  FileDevice device;
  const auto open = device.Open(config.path, FileOpenMode::open_existing);
  if (!open.ok()) {
    return PropagateDiagnostic(open.status, open.diagnostic);
  }

  SerializedDatabaseHeader serialized{};
  const auto read_header = device.ReadAt(0, serialized.data(), serialized.size());
  if (!read_header.ok()) {
    return PropagateDiagnostic(read_header.status, read_header.diagnostic);
  }
  const auto parsed_header = ParseDatabaseHeader(serialized);
  if (!parsed_header.ok()) {
    return PropagateDiagnostic(parsed_header.status, parsed_header.diagnostic);
  }

  auto startup_state = ReadStartupStatePageBody(&device, parsed_header.header.page_size);
  if (!startup_state.ok()) {
    return PropagateDiagnostic(startup_state.status, startup_state.diagnostic);
  }
  if (!startup_state.state.clean_shutdown || startup_state.state.startup_dirty) {
    return unsafe("drop_requires_clean_shutdown_final_transaction");
  }
  if (!config.expected_database_uuid.empty() &&
      config.expected_database_uuid != scratchbird::core::uuid::UuidToString(startup_state.state.database_uuid.value)) {
    return unsafe("drop_database_uuid_proof_mismatch");
  }
  if (!config.expected_filespace_uuid.empty() &&
      config.expected_filespace_uuid != scratchbird::core::uuid::UuidToString(startup_state.state.first_filespace_uuid.value)) {
    return unsafe("drop_filespace_uuid_proof_mismatch");
  }

  TypedUuid database_uuid;
  database_uuid.kind = UuidKind::database;
  database_uuid.value = parsed_header.header.database_uuid;
  const auto startup_identities = ValidateStartupPageIdentities(
      &device,
      parsed_header.header.page_size,
      LifecycleDiskPolicy(parsed_header.header.page_size, false, true),
      database_uuid,
      startup_state.state.first_filespace_uuid);
  if (!startup_identities.ok()) {
    return startup_identities;
  }

  std::vector<CatalogPageRow> catalog_rows;
  const auto read_catalog_rows =
      ReadCatalogPageRows(&device, parsed_header.header.page_size, &catalog_rows);
  if (!read_catalog_rows.ok()) {
    return read_catalog_rows;
  }
  const auto filespace_manifest = ValidateFilespaceCatalogManifest(catalog_rows,
                                                                  database_uuid,
                                                                  startup_state.state.first_filespace_uuid,
                                                                  config.path);
  if (!filespace_manifest.ok()) {
    return filespace_manifest;
  }

  LocalTransactionInventory local_transaction_inventory;
  LocalTransactionHorizons local_transaction_horizons;
  const auto read_txn_inventory = ReadTransactionInventoryPage(&device,
                                                              parsed_header.header.page_size,
                                                              &local_transaction_inventory,
                                                              &local_transaction_horizons);
  if (!read_txn_inventory.ok()) {
    return read_txn_inventory;
  }
  const auto bootstrap_evidence =
      ValidateBootstrapLifecycleEvidence(startup_state.state,
                                         local_transaction_inventory,
                                         config.path);
  if (!bootstrap_evidence.ok()) {
    return bootstrap_evidence;
  }
  if (startup_state.state.clean_shutdown_local_transaction_id == 0 ||
      !StartupLifecycleEvidencePresent(startup_state.state,
                                       StartupLifecycleEvidenceFlag::clean_shutdown_tx_committed)) {
    return unsafe("drop_requires_clean_shutdown_transaction_evidence");
  }
  const auto clean_shutdown_evidence = ValidateCommittedLifecycleTransaction(
      local_transaction_inventory,
      startup_state.state.clean_shutdown_local_transaction_id,
      config.path,
      "SB-DB-LIFECYCLE-SHUTDOWN-TXN-MISSING",
      "SB-DB-LIFECYCLE-SHUTDOWN-TXN-INVALID-STATE");
  if (!clean_shutdown_evidence.ok()) {
    return clean_shutdown_evidence;
  }

  u64 committed_local_transaction_id = 0;
  const u64 drop_unix_epoch_millis = CurrentUnixEpochMillis();
  const auto committed = CommitLifecycleTransaction(
      &device,
      parsed_header.header.page_size,
      &local_transaction_inventory,
      &local_transaction_horizons,
      drop_unix_epoch_millis,
      0,
      config.path,
      "SB-DB-LIFECYCLE-DROP-TX-ID-INVALID",
      "storage.database_lifecycle.drop_transaction_id_invalid",
      &committed_local_transaction_id);
  if (!committed.ok()) {
    return committed;
  }

  startup_state.state.write_admission_fenced = true;
  startup_state.state.completed_phases.push_back("drop.safety_preconditions_verified");
  startup_state.state.completed_phases.push_back("drop.filespace_manifest_validated");
  startup_state.state.completed_phases.push_back("drop.evidence_recorded");
  startup_state.state = RecordStartupLifecycleEvidence(
      std::move(startup_state.state),
      StartupLifecycleDurablePhase::drop_evidence_recorded,
      committed_local_transaction_id,
      drop_unix_epoch_millis,
      StartupLifecycleEvidenceFlag::drop_evidence_recorded);
  const auto written = WriteStartupStatePageBody(&device, startup_state.state);
  if (!written.ok()) {
    return PropagateDiagnostic(written.status, written.diagnostic);
  }
  const auto sync = device.Sync();
  if (!sync.ok()) {
    return PropagateDiagnostic(sync.status, sync.diagnostic);
  }
  const auto close = device.Close();
  if (!close.ok()) {
    return PropagateDiagnostic(close.status, close.diagnostic);
  }

  const auto evidence_path = std::filesystem::path(config.path + ".sb.drop_evidence");
  {
    std::ofstream evidence(evidence_path, std::ios::trunc);
    if (!evidence) {
      return unsafe("drop_evidence_sidecar_write_failed");
    }
    evidence << "format=SB_DATABASE_DROP_EVIDENCE_V1\n";
    evidence << "database_uuid=" << scratchbird::core::uuid::UuidToString(database_uuid.value) << "\n";
    evidence << "filespace_uuid="
             << scratchbird::core::uuid::UuidToString(startup_state.state.first_filespace_uuid.value)
             << "\n";
    evidence << "drop_mode=" << config.drop_mode << "\n";
    evidence << "drop_local_transaction_id=" << committed_local_transaction_id << "\n";
    evidence << "operation_uuid=" << config.operation_uuid << "\n";
    evidence << "actor_uuid=" << config.actor_uuid << "\n";
    evidence.flush();
    if (!evidence) {
      return unsafe("drop_evidence_sidecar_flush_failed");
    }
  }

  auto token = config.operation_uuid.empty() ? std::to_string(drop_unix_epoch_millis)
                                             : config.operation_uuid;
  for (char& ch : token) {
    const bool keep = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                      (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
    if (!keep) ch = '_';
  }
  auto move_sidecar = [&](const std::string& suffix, const std::filesystem::path& destination_base) -> bool {
    const auto source = std::filesystem::path(config.path + suffix);
    if (!std::filesystem::exists(source)) return true;
    std::error_code ec;
    std::filesystem::rename(source, destination_base.string() + suffix, ec);
    return !ec;
  };
  auto remove_sidecar = [&](const std::string& suffix) -> bool {
    const auto source = std::filesystem::path(config.path + suffix);
    if (!std::filesystem::exists(source)) return true;
    std::error_code ec;
    std::filesystem::remove(source, ec);
    return !ec;
  };

  DatabaseLifecycleResult result;
  result.status = DatabaseLifecycleOkStatus();
  result.state.path = config.path;
  result.state.header = parsed_header.header;
  result.state.database_uuid = database_uuid;
  result.state.filespace_uuid = startup_state.state.first_filespace_uuid;
  result.state.phase = quarantine ? DatabaseLifecyclePhase::quarantined
                                  : DatabaseLifecyclePhase::dropped;
  result.state.database_open_compatibility_class = DatabaseOpenCompatibilityClass::current;
  result.state.local_transaction_inventory = std::move(local_transaction_inventory);
  result.state.local_transaction_horizons = local_transaction_horizons;
  result.state.local_transaction_inventory_present = true;
  result.state.startup_state = std::move(startup_state.state);
  result.state.startup_state_present = true;
  result.state.write_admission_fenced = true;
  result.state.startup_recovery_classification =
      StartupRecoveryClassificationName(result.state.startup_state.recovery_classification);

  if (quarantine) {
    const auto quarantine_path = std::filesystem::path(config.path + ".sb.quarantine." + token);
    std::error_code ec;
    std::filesystem::rename(config.path, quarantine_path, ec);
    if (ec) {
      return unsafe("drop_quarantine_rename_failed");
    }
    if (!move_sidecar(".sb.crud_events", quarantine_path) ||
        !move_sidecar(".sb.api_events", quarantine_path) ||
        !move_sidecar(".sb.local_password_auth", quarantine_path) ||
        !move_sidecar(".sb.txn_publish", quarantine_path) ||
        !move_sidecar(".sb.txn_publish.tmp", quarantine_path)) {
      return unsafe("drop_quarantine_sidecar_move_failed");
    }
    result.state.path = quarantine_path.string();
  } else if (physical_delete) {
    std::error_code ec;
    std::filesystem::remove(config.path, ec);
    if (ec) {
      return unsafe("drop_physical_delete_failed");
    }
    if (!remove_sidecar(".sb.crud_events") ||
        !remove_sidecar(".sb.api_events") ||
        !remove_sidecar(".sb.local_password_auth") ||
        !remove_sidecar(".sb.txn_publish") ||
        !remove_sidecar(".sb.txn_publish.tmp")) {
      return unsafe("drop_physical_delete_sidecar_failed");
    }
  }

  return result;
}

StartupWriteResult MarkDatabaseCleanShutdown(const std::string& path) {
  if (path.empty()) {
    return StartupLifecycleError("SB-DB-LIFECYCLE-SHUTDOWN-PATH-REQUIRED",
                                 "storage.database_lifecycle.shutdown_path_required");
  }

  FileDevice device;
  const auto open = device.Open(path, FileOpenMode::open_existing);
  if (!open.ok()) {
    return StartupLifecyclePropagate(open.status, open.diagnostic);
  }

  SerializedDatabaseHeader serialized{};
  const auto read_header = device.ReadAt(0, serialized.data(), serialized.size());
  if (!read_header.ok()) {
    return StartupLifecyclePropagate(read_header.status, read_header.diagnostic);
  }
  const auto parsed_header = ParseDatabaseHeader(serialized);
  if (!parsed_header.ok()) {
    return StartupLifecyclePropagate(parsed_header.status, parsed_header.diagnostic);
  }

  auto startup_state = ReadStartupStatePageBody(&device, parsed_header.header.page_size);
  if (!startup_state.ok()) {
    return StartupLifecyclePropagate(startup_state.status, startup_state.diagnostic);
  }
  TypedUuid database_uuid;
  database_uuid.kind = UuidKind::database;
  database_uuid.value = parsed_header.header.database_uuid;
  const auto shutdown_identities = ValidateStartupPageIdentities(
      &device,
      parsed_header.header.page_size,
      LifecycleDiskPolicy(parsed_header.header.page_size, false, true),
      database_uuid,
      startup_state.state.first_filespace_uuid);
  if (!shutdown_identities.ok()) {
    return StartupLifecyclePropagate(shutdown_identities.status, shutdown_identities.diagnostic);
  }
  std::vector<CatalogPageRow> catalog_rows;
  const auto read_catalog_rows = ReadCatalogPageRows(&device, parsed_header.header.page_size, &catalog_rows);
  if (!read_catalog_rows.ok()) {
    return StartupLifecyclePropagate(read_catalog_rows.status, read_catalog_rows.diagnostic);
  }
  const auto filespace_manifest = ValidateFilespaceCatalogManifest(catalog_rows,
                                                                  database_uuid,
                                                                  startup_state.state.first_filespace_uuid,
                                                                  path);
  if (!filespace_manifest.ok()) {
    return StartupLifecyclePropagate(filespace_manifest.status, filespace_manifest.diagnostic);
  }

  LocalTransactionInventory local_transaction_inventory;
  LocalTransactionHorizons local_transaction_horizons;
  const auto read_txn_inventory = ReadTransactionInventoryPage(&device,
                                                              parsed_header.header.page_size,
                                                              &local_transaction_inventory,
                                                              &local_transaction_horizons);
  if (!read_txn_inventory.ok()) {
    return StartupLifecyclePropagate(read_txn_inventory.status, read_txn_inventory.diagnostic);
  }
  const auto bootstrap_evidence =
      ValidateBootstrapLifecycleEvidence(startup_state.state,
                                         local_transaction_inventory,
                                         path);
  if (!bootstrap_evidence.ok()) {
    return StartupLifecyclePropagate(bootstrap_evidence.status, bootstrap_evidence.diagnostic);
  }
  if (startup_state.state.first_open_activation_local_transaction_id != 0) {
    if (!StartupLifecycleEvidencePresent(startup_state.state,
                                         StartupLifecycleEvidenceFlag::first_open_tx2_committed)) {
      return StartupLifecyclePropagate(
          DatabaseLifecycleErrorStatus(),
          MakeDatabaseLifecycleDiagnostic(DatabaseLifecycleErrorStatus(),
                                          "SB-DB-LIFECYCLE-ACTIVATION-EVIDENCE-MISSING",
                                          "storage.database_lifecycle.activation_evidence_missing",
                                          path,
                                          "first_open_tx2_committed"));
    }
    const auto activation_evidence = ValidateCommittedLifecycleTransaction(
        local_transaction_inventory,
        startup_state.state.first_open_activation_local_transaction_id,
        path,
        "SB-DB-LIFECYCLE-ACTIVATION-TXN-MISSING",
        "SB-DB-LIFECYCLE-ACTIVATION-TXN-INVALID-STATE");
    if (!activation_evidence.ok()) {
      return StartupLifecyclePropagate(activation_evidence.status, activation_evidence.diagnostic);
    }
  }

  const bool already_clean = startup_state.state.clean_shutdown &&
                             !startup_state.state.startup_dirty &&
                             !startup_state.state.write_admission_fenced;
  if (!already_clean) {
    const auto agent_runtime = ValidateAgentRuntimeAuthority(path, AgentLifecycleMode::shutdown);
    if (!agent_runtime.ok()) {
      return StartupLifecyclePropagate(agent_runtime.status, agent_runtime.diagnostic);
    }
    u64 committed_local_transaction_id = 0;
    const u64 shutdown_unix_epoch_millis = CurrentUnixEpochMillis();
    const auto committed = CommitLifecycleTransaction(
        &device,
        parsed_header.header.page_size,
        &local_transaction_inventory,
        &local_transaction_horizons,
        shutdown_unix_epoch_millis,
        0,
        path,
        "SB-DB-LIFECYCLE-SHUTDOWN-TX-ID-INVALID",
        "storage.database_lifecycle.shutdown_transaction_id_invalid",
        &committed_local_transaction_id);
    if (!committed.ok()) {
      return StartupLifecyclePropagate(committed.status, committed.diagnostic);
    }
    startup_state.state.clean_shutdown_local_transaction_id = committed_local_transaction_id;
    startup_state.state = RecordStartupLifecycleEvidence(
        std::move(startup_state.state),
        StartupLifecycleDurablePhase::clean_shutdown,
        committed_local_transaction_id,
        shutdown_unix_epoch_millis,
        StartupLifecycleEvidenceFlag::clean_shutdown_tx_committed);
    startup_state.state.completed_phases.push_back("close.final_lifecycle_transaction_committed");
    startup_state.state.completed_phases.push_back("close.agent_runtime_shutdown_policy_validated");
    const auto cache_flush = RecordPageCacheShutdownFlushEvidence(&device,
                                                                  parsed_header.header.page_size,
                                                                  &startup_state.state,
                                                                  database_uuid,
                                                                  path);
    if (!cache_flush.ok()) {
      return StartupLifecyclePropagate(cache_flush.status, cache_flush.diagnostic);
    }
    RecordDatabaseEngineAgentShutdownEvidence(&startup_state.state,
                                              startup_state.state.database_uuid);
  } else if (startup_state.state.clean_shutdown_local_transaction_id != 0 &&
             !StartupLifecycleEvidencePresent(startup_state.state,
                                              StartupLifecycleEvidenceFlag::clean_shutdown_tx_committed)) {
    return StartupLifecyclePropagate(
        DatabaseLifecycleErrorStatus(),
        MakeDatabaseLifecycleDiagnostic(DatabaseLifecycleErrorStatus(),
                                        "SB-DB-LIFECYCLE-SHUTDOWN-EVIDENCE-MISSING",
                                        "storage.database_lifecycle.shutdown_evidence_missing",
                                        path,
                                        "clean_shutdown_tx_committed"));
  }

  const auto clean = MarkStartupClean(std::move(startup_state.state));
  auto write = WriteStartupStatePageBody(&device, clean);
  if (!write.ok()) {
    return write;
  }
  const auto sync = device.Sync();
  if (!sync.ok()) {
    return StartupLifecyclePropagate(sync.status, sync.diagnostic);
  }
  const auto close = device.Close();
  if (!close.ok()) {
    return StartupLifecyclePropagate(close.status, close.diagnostic);
  }
  return StartupLifecycleOkStatus();
}

DiagnosticRecord MakeDatabaseLifecycleDiagnostic(Status status,
                                                std::string diagnostic_code,
                                                std::string message_key,
                                                std::string path,
                                                std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!path.empty()) {
    arguments.push_back({"path", path});
  }
  if (!detail.empty()) {
    arguments.push_back({"detail", detail});
  }

  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "storage.database.lifecycle");
}

}  // namespace scratchbird::storage::database
