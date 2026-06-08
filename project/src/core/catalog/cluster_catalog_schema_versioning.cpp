// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cluster_catalog_schema_versioning.hpp"

#include "cluster_schema_gating.hpp"

#include <set>
#include <string_view>
#include <utility>

namespace scratchbird::core::catalog {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status VersionOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::catalog};
}

Status VersionErrorStatus() {
  return {StatusCode::platform_required_feature_missing,
          Severity::error,
          Subsystem::catalog};
}

std::string SanitizeId(std::string value) {
  for (char& ch : value) {
    if (ch == '.') {
      ch = '_';
    }
  }
  return value;
}

ClusterCatalogSchemaVersionProfile Profile(
    std::string family_id,
    std::string table_path,
    std::string stable_source_id,
    ClusterCatalogSchemaFamilyKind family_kind) {
  ClusterCatalogSchemaVersionProfile profile;
  profile.family_id = std::move(family_id);
  profile.table_path = std::move(table_path);
  profile.stable_source_id = std::move(stable_source_id);
  profile.family_kind = family_kind;
  profile.schema_version_current = kClusterCatalogManifestVersionCurrent;
  profile.schema_version_min_supported = kClusterCatalogManifestVersionCurrent;
  profile.schema_version_max_supported = kClusterCatalogManifestVersionCurrent;
  profile.codec_version_current = kClusterCatalogRecordCodecVersionCurrent;
  profile.codec_version_min_supported =
      kClusterCatalogRecordCodecVersionMinSupported;
  profile.codec_version_max_supported =
      kClusterCatalogRecordCodecVersionMaxSupported;
  profile.external_provider_bound = true;
  profile.local_runtime_execution_enabled = false;
  profile.mutable_by_local_core = false;
  profile.migration_supported = false;
  profile.fail_closed_without_external_provider = true;
  return profile;
}

ClusterCatalogCompatibilityResult VersionError(
    ClusterCatalogCompatibilityClass compatibility_class,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail) {
  ClusterCatalogCompatibilityResult result;
  result.status = VersionErrorStatus();
  result.compatibility_class = compatibility_class;
  result.migration_required =
      compatibility_class ==
          ClusterCatalogCompatibilityClass::migration_plan_missing ||
      compatibility_class ==
          ClusterCatalogCompatibilityClass::migration_unsupported;
  result.diagnostic = MakeClusterCatalogSchemaVersionDiagnostic(
      result.status,
      std::move(diagnostic_code),
      std::move(message_key),
      std::move(detail));
  return result;
}

ClusterCatalogCompatibilityResult VersionOk(bool migration_required = false) {
  ClusterCatalogCompatibilityResult result;
  result.status = VersionOkStatus();
  result.compatibility_class = ClusterCatalogCompatibilityClass::current;
  result.migration_required = migration_required;
  return result;
}

void AddIssue(ClusterCatalogSchemaVersionValidationResult* result,
              std::string diagnostic_code,
              std::string detail) {
  result->ok = false;
  result->issues.push_back({std::move(diagnostic_code), std::move(detail)});
}

std::string VersionDetail(const std::string& table_path,
                          u32 schema_version,
                          u32 codec_version) {
  return table_path + " schema=" + std::to_string(schema_version) +
         " codec=" + std::to_string(codec_version);
}

bool StableSourceIdExists(const ClusterCatalogSchemaVersionProfile& profile) {
  for (const auto& table : BuiltinClusterCatalogTableManifests()) {
    if (ClusterCatalogFullTablePath(table) == profile.table_path) {
      return table.stable_table_id == profile.stable_source_id &&
             table.manifest_version == profile.schema_version_current;
    }
  }
  for (const auto& role_profile : BuiltinClusterRoleProfileManifests()) {
    if (ClusterCatalogFullTablePath(role_profile.table) == profile.table_path) {
      return role_profile.table.stable_table_id == profile.stable_source_id &&
             role_profile.table.manifest_version ==
                 profile.schema_version_current;
    }
  }
  for (const auto& projection : BuiltinClusterCacheProjectionManifests()) {
    if (ClusterCacheProjectionFullTablePath(projection) == profile.table_path) {
      return projection.stable_projection_id == profile.stable_source_id;
    }
  }
  return false;
}

}  // namespace

const char* ClusterCatalogSchemaFamilyKindName(
    ClusterCatalogSchemaFamilyKind family_kind) {
  switch (family_kind) {
    case ClusterCatalogSchemaFamilyKind::catalog_table:
      return "catalog_table";
    case ClusterCatalogSchemaFamilyKind::role_profile:
      return "role_profile";
    case ClusterCatalogSchemaFamilyKind::projection_cache:
      return "projection_cache";
  }
  return "unknown";
}

const char* ClusterCatalogCompatibilityClassName(
    ClusterCatalogCompatibilityClass compatibility_class) {
  switch (compatibility_class) {
    case ClusterCatalogCompatibilityClass::current:
      return "current";
    case ClusterCatalogCompatibilityClass::unsupported_old_schema:
      return "unsupported-old-schema";
    case ClusterCatalogCompatibilityClass::unsupported_new_schema:
      return "unsupported-new-schema";
    case ClusterCatalogCompatibilityClass::unsupported_codec:
      return "unsupported-codec";
    case ClusterCatalogCompatibilityClass::external_provider_required:
      return "external-provider-required";
    case ClusterCatalogCompatibilityClass::ambiguous_identity_refused:
      return "ambiguous-identity-refused";
    case ClusterCatalogCompatibilityClass::downgrade_refused:
      return "downgrade-refused";
    case ClusterCatalogCompatibilityClass::migration_plan_missing:
      return "migration-plan-missing";
    case ClusterCatalogCompatibilityClass::migration_unsupported:
      return "migration-unsupported";
  }
  return "unknown";
}

const std::vector<ClusterCatalogSchemaVersionProfile>&
BuiltinClusterCatalogSchemaVersionProfiles() {
  static const std::vector<ClusterCatalogSchemaVersionProfile> profiles = [] {
    std::vector<ClusterCatalogSchemaVersionProfile> out;
    for (const auto& table : BuiltinClusterCatalogTableManifests()) {
      const std::string path = ClusterCatalogFullTablePath(table);
      out.push_back(Profile("cluster_schema." + SanitizeId(path),
                            path,
                            table.stable_table_id,
                            ClusterCatalogSchemaFamilyKind::catalog_table));
    }
    for (const auto& role_profile : BuiltinClusterRoleProfileManifests()) {
      const std::string path =
          ClusterCatalogFullTablePath(role_profile.table);
      out.push_back(Profile("cluster_schema." + SanitizeId(path),
                            path,
                            role_profile.table.stable_table_id,
                            ClusterCatalogSchemaFamilyKind::role_profile));
    }
    for (const auto& projection : BuiltinClusterCacheProjectionManifests()) {
      const std::string path =
          ClusterCacheProjectionFullTablePath(projection);
      out.push_back(Profile("cluster_schema." + SanitizeId(path),
                            path,
                            projection.stable_projection_id,
                            ClusterCatalogSchemaFamilyKind::projection_cache));
    }
    return out;
  }();
  return profiles;
}

const ClusterCatalogSchemaVersionProfile*
FindClusterCatalogSchemaVersionProfile(std::string_view table_path) {
  for (const auto& profile : BuiltinClusterCatalogSchemaVersionProfiles()) {
    if (std::string_view(profile.table_path) == table_path) {
      return &profile;
    }
  }
  return nullptr;
}

ClusterCatalogCompatibilityResult EvaluateClusterCatalogCompatibility(
    const ClusterCatalogCompatibilityRequest& request) {
  const auto* profile = FindClusterCatalogSchemaVersionProfile(
      request.table_path);
  if (profile == nullptr) {
    return VersionError(
        ClusterCatalogCompatibilityClass::unsupported_new_schema,
        "SB-CLUSTER-CATALOG-SCHEMA-FAMILY-UNKNOWN",
        "catalog.cluster_schema_version.family_unknown",
        request.table_path);
  }
  if (!request.identity_proven) {
    return VersionError(
        ClusterCatalogCompatibilityClass::ambiguous_identity_refused,
        "SB-CLUSTER-CATALOG-SCHEMA-IDENTITY-AMBIGUOUS",
        "catalog.cluster_schema_version.identity_ambiguous",
        request.table_path);
  }
  if (request.downgrade_requested) {
    return VersionError(
        ClusterCatalogCompatibilityClass::downgrade_refused,
        "SB-CLUSTER-CATALOG-SCHEMA-DOWNGRADE-REFUSED",
        "catalog.cluster_schema_version.downgrade_refused",
        request.table_path);
  }
  if (!request.external_provider_available) {
    return VersionError(
        ClusterCatalogCompatibilityClass::external_provider_required,
        "SB-CLUSTER-CATALOG-SCHEMA-EXTERNAL-PROVIDER-REQUIRED",
        "catalog.cluster_schema_version.external_provider_required",
        request.table_path);
  }
  if (request.schema_version < profile->schema_version_min_supported) {
    return VersionError(
        ClusterCatalogCompatibilityClass::unsupported_old_schema,
        "SB-CLUSTER-CATALOG-SCHEMA-UNSUPPORTED-OLD",
        "catalog.cluster_schema_version.unsupported_old",
        VersionDetail(request.table_path,
                      request.schema_version,
                      request.codec_version));
  }
  if (request.schema_version > profile->schema_version_max_supported) {
    return VersionError(
        ClusterCatalogCompatibilityClass::unsupported_new_schema,
        "SB-CLUSTER-CATALOG-SCHEMA-UNSUPPORTED-NEW",
        "catalog.cluster_schema_version.unsupported_new",
        VersionDetail(request.table_path,
                      request.schema_version,
                      request.codec_version));
  }
  if (request.codec_version < profile->codec_version_min_supported ||
      request.codec_version > profile->codec_version_max_supported) {
    return VersionError(
        ClusterCatalogCompatibilityClass::unsupported_codec,
        "SB-CLUSTER-CATALOG-CODEC-UNSUPPORTED",
        "catalog.cluster_schema_version.codec_unsupported",
        VersionDetail(request.table_path,
                      request.schema_version,
                      request.codec_version));
  }
  return VersionOk();
}

ClusterCatalogCompatibilityResult ValidateClusterCatalogMigrationPlan(
    const ClusterCatalogMigrationPlanRequest& request) {
  const auto* profile = FindClusterCatalogSchemaVersionProfile(
      request.table_path);
  if (profile == nullptr) {
    return VersionError(
        ClusterCatalogCompatibilityClass::unsupported_new_schema,
        "SB-CLUSTER-CATALOG-SCHEMA-FAMILY-UNKNOWN",
        "catalog.cluster_schema_version.family_unknown",
        request.table_path);
  }
  if (!request.identity_proven) {
    return VersionError(
        ClusterCatalogCompatibilityClass::ambiguous_identity_refused,
        "SB-CLUSTER-CATALOG-SCHEMA-IDENTITY-AMBIGUOUS",
        "catalog.cluster_schema_version.identity_ambiguous",
        request.table_path);
  }
  if (!request.external_provider_available) {
    return VersionError(
        ClusterCatalogCompatibilityClass::external_provider_required,
        "SB-CLUSTER-CATALOG-SCHEMA-EXTERNAL-PROVIDER-REQUIRED",
        "catalog.cluster_schema_version.external_provider_required",
        request.table_path);
  }
  if (request.from_schema_version == request.to_schema_version &&
      request.to_schema_version == profile->schema_version_current) {
    return VersionOk();
  }
  if (request.migration_plan_id.empty()) {
    return VersionError(
        ClusterCatalogCompatibilityClass::migration_plan_missing,
        "SB-CLUSTER-CATALOG-MIGRATION-PLAN-MISSING",
        "catalog.cluster_schema_version.migration_plan_missing",
        request.table_path);
  }
  return VersionError(
      ClusterCatalogCompatibilityClass::migration_unsupported,
      "SB-CLUSTER-CATALOG-MIGRATION-UNSUPPORTED",
      "catalog.cluster_schema_version.migration_unsupported",
      request.table_path + " plan=" + request.migration_plan_id);
}

ClusterCatalogSchemaVersionValidationResult
ValidateBuiltinClusterCatalogSchemaVersionProfiles() {
  ClusterCatalogSchemaVersionValidationResult result;
  std::set<std::string> table_paths;
  for (const auto& profile : BuiltinClusterCatalogSchemaVersionProfiles()) {
    if (profile.family_id.empty() || profile.table_path.empty() ||
        profile.stable_source_id.empty()) {
      AddIssue(&result,
               "SB-CLUSTER-CATALOG-SCHEMA-PROFILE-INCOMPLETE",
               profile.table_path);
    }
    if (!table_paths.insert(profile.table_path).second) {
      AddIssue(&result,
               "SB-CLUSTER-CATALOG-SCHEMA-PROFILE-DUPLICATE",
               profile.table_path);
    }
    if (!StableSourceIdExists(profile)) {
      AddIssue(&result,
               "SB-CLUSTER-CATALOG-SCHEMA-SOURCE-MISMATCH",
               profile.table_path);
    }
    if (!profile.external_provider_bound ||
        !profile.fail_closed_without_external_provider ||
        profile.local_runtime_execution_enabled ||
        profile.mutable_by_local_core) {
      AddIssue(&result,
               "SB-CLUSTER-CATALOG-SCHEMA-LOCAL-AUTHORITY-REFUSED",
               profile.table_path);
    }
    if (profile.schema_version_current != kClusterCatalogManifestVersionCurrent ||
        profile.schema_version_min_supported !=
            kClusterCatalogManifestVersionCurrent ||
        profile.schema_version_max_supported !=
            kClusterCatalogManifestVersionCurrent ||
        profile.codec_version_current !=
            kClusterCatalogRecordCodecVersionCurrent ||
        profile.codec_version_min_supported !=
            kClusterCatalogRecordCodecVersionMinSupported ||
        profile.codec_version_max_supported !=
            kClusterCatalogRecordCodecVersionMaxSupported) {
      AddIssue(&result,
               "SB-CLUSTER-CATALOG-SCHEMA-VERSION-DRIFT",
               profile.table_path);
    }
    if (profile.migration_supported) {
      AddIssue(&result,
               "SB-CLUSTER-CATALOG-SCHEMA-MIGRATION-OVERCLAIM",
               profile.table_path);
    }
  }
  return result;
}

DiagnosticRecord MakeClusterCatalogSchemaVersionDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail) {
  std::vector<DiagnosticArgument> arguments;
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
                        "core.catalog.cluster_schema_versioning");
}

}  // namespace scratchbird::core::catalog
