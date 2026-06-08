// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// CLUSTER_CATALOG_MANIFESTS
// CLUSTER_SCHEMA_MANIFESTS
#include "runtime_platform.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::catalog {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::u32;

inline constexpr u32 kClusterCatalogManifestVersionCurrent = 1;

struct ClusterCatalogColumnManifest {
  std::string column_name;
  std::string type_name;
  bool required = true;
  bool uuid_identity = false;
  bool authority_reference = false;
  bool provider_supplied = true;
};

struct ClusterCatalogTableManifest {
  std::string schema_path;
  std::string table_name;
  std::string stable_table_id;
  std::string record_family;
  u32 manifest_version = kClusterCatalogManifestVersionCurrent;
  bool engine_owned = true;
  bool cluster_shared = true;
  bool external_provider_bound = true;
  bool local_runtime_execution_enabled = false;
  bool mutable_by_local_core = false;
  bool uuid_only_identity = true;
  std::vector<ClusterCatalogColumnManifest> columns;
  std::vector<std::string> primary_key_columns;
  std::vector<std::string> required_columns;
};

struct ClusterRoleProfileManifest {
  std::string role_code;
  ClusterCatalogTableManifest table;
};

struct ClusterCatalogManifestSet {
  u32 manifest_version = kClusterCatalogManifestVersionCurrent;
  bool engine_owned = true;
  bool external_provider_required = true;
  bool local_runtime_execution_enabled = false;
  std::vector<ClusterCatalogTableManifest> tables;
  std::vector<ClusterRoleProfileManifest> role_profiles;
};

struct ClusterCatalogManifestValidationResult {
  Status status;
  ClusterCatalogManifestSet manifest;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok(); }
};

struct ClusterCatalogTableValidationResult {
  Status status;
  ClusterCatalogTableManifest table;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok(); }
};

const std::vector<std::string>& BuiltinClusterCatalogRoleCodes();
const std::vector<ClusterCatalogTableManifest>& BuiltinClusterCatalogTableManifests();
const std::vector<ClusterRoleProfileManifest>& BuiltinClusterRoleProfileManifests();
ClusterCatalogManifestSet BuiltinClusterCatalogManifestSet();
std::string ClusterCatalogFullTablePath(const ClusterCatalogTableManifest& table);
const ClusterCatalogColumnManifest* FindClusterCatalogColumn(
    const ClusterCatalogTableManifest& table,
    const std::string& column_name);
ClusterCatalogTableValidationResult ValidateClusterCatalogTableManifest(
    const ClusterCatalogTableManifest& table);
ClusterCatalogTableValidationResult ValidateClusterRoleProfileManifest(
    const ClusterRoleProfileManifest& role_profile);
ClusterCatalogManifestValidationResult ValidateClusterCatalogManifestSet(
    const ClusterCatalogManifestSet& manifest);
DiagnosticRecord MakeClusterCatalogManifestDiagnostic(Status status,
                                                      std::string diagnostic_code,
                                                      std::string message_key,
                                                      std::string detail = {});

}  // namespace scratchbird::core::catalog
