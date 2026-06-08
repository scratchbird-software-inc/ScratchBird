// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cluster_descriptor_manifest.hpp"

#include <algorithm>
#include <set>
#include <utility>
#include <vector>

namespace scratchbird::core::catalog {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status DescriptorOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::catalog};
}

Status DescriptorErrorStatus() {
  return {StatusCode::platform_required_feature_missing,
          Severity::error,
          Subsystem::catalog};
}

ClusterCatalogColumnManifest Column(std::string column_name,
                                    std::string type_name,
                                    bool uuid_identity = false,
                                    bool authority_reference = false) {
  ClusterCatalogColumnManifest column;
  column.column_name = std::move(column_name);
  column.type_name = std::move(type_name);
  column.required = true;
  column.uuid_identity = uuid_identity;
  column.authority_reference = authority_reference;
  column.provider_supplied = true;
  return column;
}

std::vector<std::string> ColumnNames(
    const std::vector<ClusterCatalogColumnManifest>& columns) {
  std::vector<std::string> names;
  names.reserve(columns.size());
  for (const auto& column : columns) {
    names.push_back(column.column_name);
  }
  return names;
}

std::vector<ClusterCatalogColumnManifest> CommonColumns(
    std::string primary_uuid_column) {
  return {
      Column(std::move(primary_uuid_column), "uuid", true),
      Column("cluster_uuid", "uuid", false, true),
      Column("external_provider_uuid", "uuid", false, true),
      Column("provider_catalog_uuid", "uuid", false, true),
      Column("authority_provenance_uuid", "uuid", false, true),
      Column("status", "status_code"),
      Column("catalog_epoch", "uint64"),
      Column("catalog_generation", "uint64"),
      Column("provider_record_digest", "digest")};
}

ClusterCatalogTableManifest Table(
    std::string schema_path,
    std::string table_name,
    std::string stable_table_id,
    std::string record_family,
    std::string primary_uuid_column,
    std::vector<ClusterCatalogColumnManifest> specific_columns) {
  std::vector<ClusterCatalogColumnManifest> columns =
      CommonColumns(primary_uuid_column);
  columns.insert(columns.end(),
                 specific_columns.begin(),
                 specific_columns.end());

  ClusterCatalogTableManifest table;
  table.schema_path = std::move(schema_path);
  table.table_name = std::move(table_name);
  table.stable_table_id = std::move(stable_table_id);
  table.record_family = std::move(record_family);
  table.manifest_version = kClusterCatalogManifestVersionCurrent;
  table.engine_owned = true;
  table.cluster_shared = true;
  table.external_provider_bound = true;
  table.local_runtime_execution_enabled = false;
  table.mutable_by_local_core = false;
  table.uuid_only_identity = true;
  table.required_columns = ColumnNames(columns);
  table.columns = std::move(columns);
  table.primary_key_columns = {std::move(primary_uuid_column)};
  return table;
}

ClusterDescriptorManifest Descriptor(
    std::string descriptor_code,
    ClusterDescriptorCategory category,
    ClusterCatalogTableManifest table) {
  ClusterDescriptorManifest descriptor;
  descriptor.descriptor_code = std::move(descriptor_code);
  descriptor.category = category;
  descriptor.table = std::move(table);
  descriptor.external_provider_owned = true;
  descriptor.descriptor_only = true;
  descriptor.local_runtime_execution_enabled = false;
  descriptor.mutable_by_local_core = false;
  descriptor.authority_provenance_required = true;
  descriptor.transaction_inventory_remains_finality_authority = true;
  return descriptor;
}

ClusterDescriptorValidationResult DescriptorError(
    std::string diagnostic_code,
    std::string message_key,
    std::string detail = {}) {
  ClusterDescriptorValidationResult result;
  result.status = DescriptorErrorStatus();
  result.diagnostic = MakeClusterDescriptorManifestDiagnostic(
      result.status,
      std::move(diagnostic_code),
      std::move(message_key),
      std::move(detail));
  return result;
}

ClusterDescriptorSetValidationResult SetError(std::string diagnostic_code,
                                              std::string message_key,
                                              std::string detail = {}) {
  ClusterDescriptorSetValidationResult result;
  result.status = DescriptorErrorStatus();
  result.diagnostic = MakeClusterDescriptorManifestDiagnostic(
      result.status,
      std::move(diagnostic_code),
      std::move(message_key),
      std::move(detail));
  return result;
}

bool HasRequiredColumn(const ClusterCatalogTableManifest& table,
                       const std::string& column_name) {
  const auto* column = FindClusterCatalogColumn(table, column_name);
  return column != nullptr && column->required;
}

}  // namespace

const char* ClusterDescriptorCategoryName(
    ClusterDescriptorCategory category) {
  switch (category) {
    case ClusterDescriptorCategory::decision:
      return "decision";
    case ClusterDescriptorCategory::route:
      return "route";
    case ClusterDescriptorCategory::fence:
      return "fence";
    case ClusterDescriptorCategory::topology:
      return "topology";
    case ClusterDescriptorCategory::cleanup:
      return "cleanup";
    case ClusterDescriptorCategory::security:
      return "security";
    case ClusterDescriptorCategory::metrics:
      return "metrics";
    case ClusterDescriptorCategory::authority_provenance:
      return "authority_provenance";
  }
  return "unknown";
}

const std::vector<std::string>& RequiredClusterDescriptorCodes() {
  static const std::vector<std::string> codes = {
      "decision_service_config",
      "decision_request",
      "decision_proof_projection",
      "route_publish",
      "route_binding",
      "fence_token_state",
      "shard_placement",
      "replica_topology",
      "cleanup_low_water_mark",
      "limbo_reconciliation",
      "cluster_security_binding",
      "cluster_metric_profile_binding",
      "authority_provenance"};
  return codes;
}

const std::vector<ClusterDescriptorManifest>&
BuiltinClusterDescriptorManifests() {
  static const std::vector<ClusterDescriptorManifest> descriptors = {
      Descriptor("decision_service_config",
                 ClusterDescriptorCategory::decision,
                 Table("cluster.sys.catalog",
                       "decision_service_config",
                       "cluster_descriptor.decision_service_config",
                       "decision_service_config",
                       "decision_service_config_uuid",
                       {Column("decision_service_uuid", "uuid", false, true),
                        Column("decision_policy_uuid", "uuid", false, true),
                        Column("quorum_profile_uuid", "uuid", false, true),
                        Column("provider_abi_epoch", "uint64")})),
      Descriptor("decision_request",
                 ClusterDescriptorCategory::decision,
                 Table("cluster.sys.catalog",
                       "decision_request",
                       "cluster_descriptor.decision_request",
                       "decision_request",
                       "decision_request_uuid",
                       {Column("request_uuid", "uuid", false, true),
                        Column("source_node_uuid", "uuid", false, true),
                        Column("target_node_uuid", "uuid", false, true),
                        Column("route_uuid", "uuid", false, true),
                        Column("fence_token_uuid", "uuid", false, true),
                        Column("request_epoch", "uint64")})),
      Descriptor("decision_proof_projection",
                 ClusterDescriptorCategory::decision,
                 Table("cluster.sys.catalog",
                       "decision_proof_projection",
                       "cluster_descriptor.decision_proof_projection",
                       "decision_proof_projection",
                       "decision_proof_projection_uuid",
                       {Column("decision_proof_uuid", "uuid", false, true),
                        Column("decision_request_uuid", "uuid", false, true),
                        Column("proof_digest", "digest"),
                        Column("projection_generation", "uint64"),
                        Column("invalidation_epoch", "uint64")})),
      Descriptor("route_publish",
                 ClusterDescriptorCategory::route,
                 Table("cluster.sys.catalog",
                       "route_publish",
                       "cluster_descriptor.route_publish",
                       "route_publish",
                       "route_publish_uuid",
                       {Column("route_uuid", "uuid", false, true),
                        Column("source_node_uuid", "uuid", false, true),
                        Column("target_node_uuid", "uuid", false, true),
                        Column("route_profile_uuid", "uuid", false, true),
                        Column("route_epoch", "uint64"),
                        Column("route_generation", "uint64")})),
      Descriptor("route_binding",
                 ClusterDescriptorCategory::route,
                 Table("cluster.sys.catalog",
                       "route_binding",
                       "cluster_descriptor.route_binding",
                       "route_binding",
                       "route_binding_uuid",
                       {Column("route_uuid", "uuid", false, true),
                        Column("shard_uuid", "uuid", false, true),
                        Column("owner_node_uuid", "uuid", false, true),
                        Column("participant_node_uuid", "uuid", false, true),
                        Column("binding_generation", "uint64")})),
      Descriptor("fence_token_state",
                 ClusterDescriptorCategory::fence,
                 Table("cluster.sys.catalog",
                       "fence_token_state",
                       "cluster_descriptor.fence_token_state",
                       "fence_token_state",
                       "fence_token_uuid",
                       {Column("route_uuid", "uuid", false, true),
                        Column("node_uuid", "uuid", false, true),
                        Column("fence_epoch", "uint64"),
                        Column("fence_generation", "uint64"),
                        Column("expiry_epoch", "uint64")})),
      Descriptor("shard_placement",
                 ClusterDescriptorCategory::topology,
                 Table("cluster.sys.catalog",
                       "shard_placement",
                       "cluster_descriptor.shard_placement",
                       "shard_placement",
                       "shard_placement_uuid",
                       {Column("shard_uuid", "uuid", false, true),
                        Column("page_family_uuid", "uuid", false, true),
                        Column("primary_node_uuid", "uuid", false, true),
                        Column("replica_set_uuid", "uuid", false, true),
                        Column("placement_generation", "uint64")})),
      Descriptor("replica_topology",
                 ClusterDescriptorCategory::topology,
                 Table("cluster.sys.catalog",
                       "replica_topology",
                       "cluster_descriptor.replica_topology",
                       "replica_topology",
                       "replica_topology_uuid",
                       {Column("shard_uuid", "uuid", false, true),
                        Column("replica_set_uuid", "uuid", false, true),
                        Column("replica_node_uuid", "uuid", false, true),
                        Column("replica_role_code", "replica_role_code"),
                        Column("topology_generation", "uint64")})),
      Descriptor("cleanup_low_water_mark",
                 ClusterDescriptorCategory::cleanup,
                 Table("cluster.sys.catalog",
                       "cleanup_low_water_mark",
                       "cluster_descriptor.cleanup_low_water_mark",
                       "cleanup_low_water_mark",
                       "cleanup_low_water_uuid",
                       {Column("page_family_uuid", "uuid", false, true),
                        Column("cleanup_epoch", "uint64"),
                        Column("cleanup_generation", "uint64"),
                        Column("low_water_transaction_number", "uint64"),
                        Column("limbo_blocker_count", "uint64")})),
      Descriptor("limbo_reconciliation",
                 ClusterDescriptorCategory::cleanup,
                 Table("cluster.sys.catalog",
                       "limbo_reconciliation",
                       "cluster_descriptor.limbo_reconciliation",
                       "limbo_reconciliation",
                       "limbo_reconciliation_uuid",
                       {Column("transaction_uuid", "uuid", false, true),
                        Column("participant_uuid", "uuid", false, true),
                        Column("reconciliation_epoch", "uint64"),
                        Column("finality_state_code", "finality_state_code"),
                        Column("decision_proof_uuid", "uuid", false, true)})),
      Descriptor("cluster_security_binding",
                 ClusterDescriptorCategory::security,
                 Table("cluster.sys.security",
                       "binding_authority",
                       "cluster_descriptor.cluster_security_binding",
                       "cluster_security_binding",
                       "security_binding_uuid",
                       {Column("node_binding_uuid", "uuid", false, true),
                        Column("principal_uuid", "uuid", false, true),
                        Column("security_policy_uuid", "uuid", false, true),
                        Column("credential_material_uuid", "uuid", false, true),
                        Column("role_uuid", "uuid", false, true),
                        Column("security_epoch", "uint64")})),
      Descriptor("cluster_metric_profile_binding",
                 ClusterDescriptorCategory::metrics,
                 Table("cluster.sys.metrics",
                       "metric_profile_binding",
                       "cluster_descriptor.cluster_metric_profile_binding",
                       "cluster_metric_profile_binding",
                       "metric_binding_uuid",
                       {Column("metric_profile_uuid", "uuid", false, true),
                        Column("metric_family_code", "metric_family_code"),
                        Column("producer_profile_uuid", "uuid", false, true),
                        Column("retention_policy_uuid", "uuid", false, true),
                        Column("redaction_policy_uuid", "uuid", false, true),
                        Column("metric_epoch", "uint64")})),
      Descriptor("authority_provenance",
                 ClusterDescriptorCategory::authority_provenance,
                 Table("cluster.sys.catalog",
                       "authority_provenance",
                       "cluster_descriptor.authority_provenance",
                       "authority_provenance",
                       "authority_provenance_record_uuid",
                       {Column("provider_evidence_uuid", "uuid", false, true),
                        Column("provider_epoch", "uint64"),
                        Column("provider_generation", "uint64"),
                        Column("source_manifest_digest", "digest"),
                        Column("signature_envelope_uuid", "uuid", false, true),
                        Column("compatibility_digest", "digest")})),
  };
  return descriptors;
}

ClusterDescriptorManifestSet BuiltinClusterDescriptorManifestSet() {
  ClusterDescriptorManifestSet manifest;
  manifest.external_provider_required = true;
  manifest.local_runtime_execution_enabled = false;
  manifest.mutable_by_local_core = false;
  manifest.transaction_inventory_remains_finality_authority = true;
  manifest.descriptors = BuiltinClusterDescriptorManifests();
  return manifest;
}

const ClusterDescriptorManifest* FindClusterDescriptorManifest(
    const std::string& descriptor_code) {
  for (const auto& descriptor : BuiltinClusterDescriptorManifests()) {
    if (descriptor.descriptor_code == descriptor_code) {
      return &descriptor;
    }
  }
  return nullptr;
}

ClusterDescriptorValidationResult ValidateClusterDescriptorManifest(
    const ClusterDescriptorManifest& descriptor) {
  if (descriptor.descriptor_code.empty() ||
      descriptor.table.schema_path.empty() ||
      descriptor.table.table_name.empty()) {
    return DescriptorError("SB-CLUSTER-DESCRIPTOR-INCOMPLETE",
                           "catalog.cluster_descriptor.incomplete",
                           descriptor.descriptor_code);
  }
  if (!descriptor.external_provider_owned || !descriptor.descriptor_only ||
      !descriptor.authority_provenance_required ||
      !descriptor.transaction_inventory_remains_finality_authority) {
    return DescriptorError("SB-CLUSTER-DESCRIPTOR-AUTHORITY-REFUSED",
                           "catalog.cluster_descriptor.authority_refused",
                           descriptor.descriptor_code);
  }
  if (descriptor.local_runtime_execution_enabled ||
      descriptor.mutable_by_local_core) {
    return DescriptorError("SB-CLUSTER-DESCRIPTOR-LOCAL-EXECUTION-REFUSED",
                           "catalog.cluster_descriptor.local_execution_refused",
                           descriptor.descriptor_code);
  }
  if (!HasRequiredColumn(descriptor.table, "authority_provenance_uuid") ||
      !HasRequiredColumn(descriptor.table, "provider_record_digest")) {
    return DescriptorError("SB-CLUSTER-DESCRIPTOR-PROVENANCE-REQUIRED",
                           "catalog.cluster_descriptor.provenance_required",
                           descriptor.descriptor_code);
  }

  const auto table_result =
      ValidateClusterCatalogTableManifest(descriptor.table);
  if (!table_result.ok()) {
    ClusterDescriptorValidationResult result;
    result.status = table_result.status;
    result.diagnostic = table_result.diagnostic;
    return result;
  }

  ClusterDescriptorValidationResult result;
  result.status = DescriptorOkStatus();
  result.descriptor = descriptor;
  return result;
}

ClusterDescriptorSetValidationResult ValidateClusterDescriptorManifestSet(
    const ClusterDescriptorManifestSet& manifest) {
  if (!manifest.external_provider_required ||
      manifest.local_runtime_execution_enabled ||
      manifest.mutable_by_local_core ||
      !manifest.transaction_inventory_remains_finality_authority) {
    return SetError("SB-CLUSTER-DESCRIPTOR-SET-AUTHORITY-REFUSED",
                    "catalog.cluster_descriptor.set_authority_refused");
  }

  std::set<std::string> descriptor_codes;
  std::set<std::string> table_paths;
  for (const auto& descriptor : manifest.descriptors) {
    const auto descriptor_result =
        ValidateClusterDescriptorManifest(descriptor);
    if (!descriptor_result.ok()) {
      ClusterDescriptorSetValidationResult result;
      result.status = descriptor_result.status;
      result.diagnostic = descriptor_result.diagnostic;
      return result;
    }
    if (!descriptor_codes.insert(descriptor.descriptor_code).second) {
      return SetError("SB-CLUSTER-DESCRIPTOR-DUPLICATE",
                      "catalog.cluster_descriptor.duplicate",
                      descriptor.descriptor_code);
    }
    const std::string table_path =
        ClusterCatalogFullTablePath(descriptor.table);
    if (!table_paths.insert(table_path).second) {
      return SetError("SB-CLUSTER-DESCRIPTOR-TABLE-DUPLICATE",
                      "catalog.cluster_descriptor.table_duplicate",
                      table_path);
    }
  }

  for (const std::string& code : RequiredClusterDescriptorCodes()) {
    if (descriptor_codes.count(code) == 0) {
      return SetError("SB-CLUSTER-DESCRIPTOR-REQUIRED-MISSING",
                      "catalog.cluster_descriptor.required_missing",
                      code);
    }
  }

  ClusterDescriptorSetValidationResult result;
  result.status = DescriptorOkStatus();
  result.manifest = manifest;
  return result;
}

DiagnosticRecord MakeClusterDescriptorManifestDiagnostic(
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
                        "core.catalog.cluster_descriptor_manifest");
}

}  // namespace scratchbird::core::catalog
