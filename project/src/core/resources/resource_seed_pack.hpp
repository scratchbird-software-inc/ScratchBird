// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-RESOURCE-SEED-PACK-ANCHOR
// SEARCH_KEY: WORKLOAD_GOVERNANCE_FAIRNESS
#include "runtime_platform.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::resources {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

enum class ResourceSeedFamily : u16 {
  charset,
  charset_mapping,
  charset_mapping_schema,
  collation,
  locale,
  uca,
  uca_manifest,
  i18n_version,
  timezone_version,
  timezone_source,
  timezone_tables,
  timezone_leaps,
  timezone_archives,
  unknown
};

enum class ResourceSeedArtifactStatus : u16 {
  pending,
  validated,
  loaded,
  rejected,
  ignored_by_profile
};

struct ResourceSeedArtifact {
  ResourceSeedFamily family = ResourceSeedFamily::unknown;
  std::string source_pattern;
  std::string canonical_path;
  std::string required_catalog_rows;
  std::string create_time_action;
  std::string content_hash;
  u64 content_size_bytes = 0;
  ResourceSeedArtifactStatus status = ResourceSeedArtifactStatus::pending;
};

struct ResourceSeedAlias {
  ResourceSeedFamily family = ResourceSeedFamily::unknown;
  std::string alias;
  std::string canonical_name;
  std::string source_path;
};

struct ResourceSeedFamilyVersion {
  ResourceSeedFamily family = ResourceSeedFamily::unknown;
  std::string version;
  std::string content_hash;
  u64 activation_epoch = 0;
  bool active = false;
};

struct ResourceSeedRuntimeCacheEpoch {
  u64 resource_epoch = 0;
  u64 charset_epoch = 0;
  u64 collation_epoch = 0;
  u64 timezone_epoch = 0;
  u64 locale_epoch = 0;
  u64 runtime_cache_epoch = 0;
  bool valid = false;
};

struct ResourceSeedIndexDependencyEvidence {
  std::string dependent_artifact_name;
  std::string dependent_artifact_class = "index";
  ResourceSeedFamily family = ResourceSeedFamily::unknown;
  std::string required_version;
  std::string required_content_hash;
  u64 dependency_epoch = 0;
  bool compatibility_proven = false;
  std::string compatibility_evidence;
};

struct ResourceSeedLoadConfig {
  std::string seed_pack_root;
  bool allow_minimal_bootstrap = false;
  bool require_charset_collation = true;
  bool require_timezone = true;
};

struct ResourceSeedCatalogImage {
  std::string seed_pack_name;
  std::string seed_pack_version;
  std::string seed_pack_root;
  std::string manifest_path;
  std::string content_hash;
  std::string i18n_version;
  std::string timezone_version;
  std::string charset_version;
  std::string collation_version;
  std::string locale_version;
  std::string charset_content_hash;
  std::string collation_content_hash;
  std::string locale_content_hash;
  std::string timezone_content_hash;
  bool active = false;
  bool minimal_bootstrap = false;
  bool database_create_ready = false;
  bool database_open_ready = false;
  bool missing_seed_refusal_required = true;
  bool unsupported_upgrade_refusal_required = true;
  u32 resource_bundle_records = 0;
  u32 resource_artifact_records = 0;
  u32 resource_activation_records = 0;
  u32 charset_records = 0;
  u32 charset_alias_records = 0;
  u32 charset_mapping_artifacts = 0;
  u32 collation_records = 0;
  u32 collation_tailoring_records = 0;
  u32 locale_records = 0;
  u32 timezone_records = 0;
  u32 timezone_transition_records = 0;
  u32 timezone_leap_second_records = 0;
  u32 runtime_cache_invalidation_records = 0;
  u32 index_dependency_records = 0;
  u64 resource_epoch = 0;
  u64 charset_epoch = 0;
  u64 collation_epoch = 0;
  u64 timezone_epoch = 0;
  u64 locale_epoch = 0;
  u64 runtime_cache_epoch = 0;
  std::vector<ResourceSeedFamilyVersion> family_versions;
  std::vector<ResourceSeedIndexDependencyEvidence> index_dependencies;
  std::vector<ResourceSeedArtifact> artifacts;
  std::vector<ResourceSeedAlias> aliases;
};

struct ResourceSeedLifecycleEvaluationResult {
  Status status;
  DiagnosticRecord diagnostic;
  bool cache_epoch_current = false;
  bool runtime_cache_invalidation_required = false;
  bool index_dependency_current = false;
  bool index_rebuild_required = false;
  std::vector<std::string> actions;

  bool ok() const {
    return status.ok();
  }
};

struct ResourceSeedAliasResolutionResult {
  Status status;
  ResourceSeedAlias alias;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct ResourceSeedCatalogImageResult {
  Status status;
  ResourceSeedCatalogImage image;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

const char* ResourceSeedFamilyName(ResourceSeedFamily family);
const char* ResourceSeedArtifactStatusName(ResourceSeedArtifactStatus status);
ResourceSeedCatalogImageResult LoadResourceSeedPack(const ResourceSeedLoadConfig& config);
ResourceSeedCatalogImageResult ValidateResourceSeedCatalogImage(const ResourceSeedCatalogImage& image,
                                                               bool allow_minimal_bootstrap = false);
const ResourceSeedFamilyVersion* FindResourceSeedFamilyVersion(const ResourceSeedCatalogImage& image,
                                                              ResourceSeedFamily family);
std::string ResourceSeedVersionForFamily(const ResourceSeedCatalogImage& image,
                                         ResourceSeedFamily family);
std::string ResourceSeedContentHashForFamily(const ResourceSeedCatalogImage& image,
                                            ResourceSeedFamily family);
u64 ResourceSeedActivationEpochForFamily(const ResourceSeedCatalogImage& image,
                                         ResourceSeedFamily family);
ResourceSeedRuntimeCacheEpoch MakeResourceSeedRuntimeCacheEpoch(const ResourceSeedCatalogImage& image);
ResourceSeedLifecycleEvaluationResult EvaluateResourceSeedRuntimeCache(
    const ResourceSeedCatalogImage& image,
    const ResourceSeedRuntimeCacheEpoch& cache_epoch);
ResourceSeedLifecycleEvaluationResult EvaluateResourceSeedIndexDependency(
    const ResourceSeedCatalogImage& image,
    const ResourceSeedIndexDependencyEvidence& dependency);
ResourceSeedAliasResolutionResult ResolveResourceSeedAlias(const ResourceSeedCatalogImage& image,
                                                           ResourceSeedFamily family,
                                                           const std::string& alias);
DiagnosticRecord MakeResourceSeedDiagnostic(Status status,
                                           std::string diagnostic_code,
                                           std::string message_key,
                                           std::string detail = {});

}  // namespace scratchbird::core::resources
