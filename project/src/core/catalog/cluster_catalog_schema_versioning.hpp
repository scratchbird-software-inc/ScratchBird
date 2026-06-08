// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// CLUSTER_CATALOG_SCHEMA_VERSIONING
#include "cluster_catalog_manifest.hpp"
#include "cluster_catalog_record_codec.hpp"
#include "runtime_platform.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::core::catalog {

enum class ClusterCatalogSchemaFamilyKind {
  catalog_table,
  role_profile,
  projection_cache
};

enum class ClusterCatalogCompatibilityClass {
  current,
  unsupported_old_schema,
  unsupported_new_schema,
  unsupported_codec,
  external_provider_required,
  ambiguous_identity_refused,
  downgrade_refused,
  migration_plan_missing,
  migration_unsupported
};

struct ClusterCatalogSchemaVersionProfile {
  std::string family_id;
  std::string table_path;
  std::string stable_source_id;
  ClusterCatalogSchemaFamilyKind family_kind =
      ClusterCatalogSchemaFamilyKind::catalog_table;
  u32 schema_version_current = kClusterCatalogManifestVersionCurrent;
  u32 schema_version_min_supported = kClusterCatalogManifestVersionCurrent;
  u32 schema_version_max_supported = kClusterCatalogManifestVersionCurrent;
  u32 codec_version_current = kClusterCatalogRecordCodecVersionCurrent;
  u32 codec_version_min_supported = kClusterCatalogRecordCodecVersionMinSupported;
  u32 codec_version_max_supported = kClusterCatalogRecordCodecVersionMaxSupported;
  bool external_provider_bound = true;
  bool local_runtime_execution_enabled = false;
  bool mutable_by_local_core = false;
  bool migration_supported = false;
  bool fail_closed_without_external_provider = true;
};

struct ClusterCatalogCompatibilityRequest {
  std::string table_path;
  u32 schema_version = kClusterCatalogManifestVersionCurrent;
  u32 codec_version = kClusterCatalogRecordCodecVersionCurrent;
  bool external_provider_available = false;
  bool identity_proven = true;
  bool downgrade_requested = false;
};

struct ClusterCatalogMigrationPlanRequest {
  std::string table_path;
  u32 from_schema_version = kClusterCatalogManifestVersionCurrent;
  u32 to_schema_version = kClusterCatalogManifestVersionCurrent;
  bool external_provider_available = false;
  bool identity_proven = true;
  std::string migration_plan_id;
};

struct ClusterCatalogCompatibilityResult {
  Status status;
  ClusterCatalogCompatibilityClass compatibility_class =
      ClusterCatalogCompatibilityClass::current;
  bool migration_required = false;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok(); }
};

struct ClusterCatalogSchemaVersionValidationIssue {
  std::string diagnostic_code;
  std::string detail;
};

struct ClusterCatalogSchemaVersionValidationResult {
  bool ok = true;
  std::vector<ClusterCatalogSchemaVersionValidationIssue> issues;
};

const char* ClusterCatalogSchemaFamilyKindName(
    ClusterCatalogSchemaFamilyKind family_kind);
const char* ClusterCatalogCompatibilityClassName(
    ClusterCatalogCompatibilityClass compatibility_class);
const std::vector<ClusterCatalogSchemaVersionProfile>&
BuiltinClusterCatalogSchemaVersionProfiles();
const ClusterCatalogSchemaVersionProfile*
FindClusterCatalogSchemaVersionProfile(std::string_view table_path);
ClusterCatalogCompatibilityResult EvaluateClusterCatalogCompatibility(
    const ClusterCatalogCompatibilityRequest& request);
ClusterCatalogCompatibilityResult ValidateClusterCatalogMigrationPlan(
    const ClusterCatalogMigrationPlanRequest& request);
ClusterCatalogSchemaVersionValidationResult
ValidateBuiltinClusterCatalogSchemaVersionProfiles();
DiagnosticRecord MakeClusterCatalogSchemaVersionDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail = {});

}  // namespace scratchbird::core::catalog
