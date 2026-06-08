// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cluster_descriptor_manifest.hpp"
#include "cluster_metric_descriptors.hpp"

// PUBLIC_CLUSTER_DESCRIPTOR_GATE

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace catalog = scratchbird::core::catalog;
namespace metrics = scratchbird::core::metrics;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool HasRequiredColumn(const catalog::ClusterCatalogTableManifest& table,
                       std::string_view column_name) {
  const auto* column =
      catalog::FindClusterCatalogColumn(table, std::string(column_name));
  return column != nullptr && column->required;
}

bool HasRequiredLabel(const metrics::ClusterMetricDescriptorManifest& descriptor,
                      std::string_view key) {
  return std::any_of(descriptor.labels.begin(),
                     descriptor.labels.end(),
                     [key](const metrics::MetricLabelDescriptor& label) {
                       return label.key == key && label.required;
                     });
}

catalog::ClusterCatalogColumnManifest Column(std::string column_name,
                                             std::string type_name) {
  catalog::ClusterCatalogColumnManifest column;
  column.column_name = std::move(column_name);
  column.type_name = std::move(type_name);
  column.required = true;
  column.provider_supplied = true;
  return column;
}

template <typename Predicate>
void RemoveColumns(catalog::ClusterCatalogTableManifest* table,
                   Predicate predicate) {
  table->columns.erase(std::remove_if(table->columns.begin(),
                                      table->columns.end(),
                                      predicate),
                       table->columns.end());
  table->required_columns.erase(
      std::remove_if(table->required_columns.begin(),
                     table->required_columns.end(),
                     [&predicate](const std::string& column_name) {
                       catalog::ClusterCatalogColumnManifest column;
                       column.column_name = column_name;
                       return predicate(column);
                     }),
      table->required_columns.end());
}

void RequireDescriptor(
    const catalog::ClusterDescriptorManifest& descriptor,
    catalog::ClusterDescriptorCategory category,
    std::string_view table_path,
    const std::vector<std::string>& required_columns) {
  Require(descriptor.category == category, "cluster descriptor category mismatch");
  Require(catalog::ClusterCatalogFullTablePath(descriptor.table) == table_path,
          "cluster descriptor table path mismatch");
  Require(descriptor.external_provider_owned,
          "cluster descriptor was not external-provider-owned");
  Require(descriptor.descriptor_only, "cluster descriptor was not descriptor-only");
  Require(!descriptor.local_runtime_execution_enabled,
          "cluster descriptor enabled local runtime execution");
  Require(!descriptor.mutable_by_local_core,
          "cluster descriptor allowed local core mutation");
  Require(descriptor.authority_provenance_required,
          "cluster descriptor did not require authority provenance");
  Require(descriptor.transaction_inventory_remains_finality_authority,
          "cluster descriptor displaced MGA transaction inventory authority");
  Require(HasRequiredColumn(descriptor.table, "authority_provenance_uuid"),
          "cluster descriptor missing authority provenance UUID");
  Require(HasRequiredColumn(descriptor.table, "provider_record_digest"),
          "cluster descriptor missing provider digest");

  for (const auto& column_name : required_columns) {
    Require(HasRequiredColumn(descriptor.table, column_name),
            "cluster descriptor missing required column");
  }

  const auto validated = catalog::ValidateClusterDescriptorManifest(descriptor);
  Require(validated.ok(), "cluster descriptor did not validate");
}

void TestCatalogDescriptorShapes() {
  const auto manifest = catalog::BuiltinClusterDescriptorManifestSet();
  const auto validated =
      catalog::ValidateClusterDescriptorManifestSet(manifest);
  Require(validated.ok(), "built-in cluster descriptor manifest did not validate");
  Require(manifest.external_provider_required,
          "cluster descriptor set did not require external provider");
  Require(!manifest.local_runtime_execution_enabled,
          "cluster descriptor set enabled local runtime execution");
  Require(!manifest.mutable_by_local_core,
          "cluster descriptor set allowed local mutation");
  Require(manifest.transaction_inventory_remains_finality_authority,
          "cluster descriptor set displaced MGA authority");
  Require(manifest.descriptors.size() ==
              catalog::RequiredClusterDescriptorCodes().size(),
          "cluster descriptor count mismatch");

  const std::map<std::string,
                 std::pair<catalog::ClusterDescriptorCategory,
                           std::vector<std::string>>>
      required = {
          {"decision_service_config",
           {catalog::ClusterDescriptorCategory::decision,
            {"decision_service_uuid", "decision_policy_uuid",
             "quorum_profile_uuid", "provider_abi_epoch"}}},
          {"decision_request",
           {catalog::ClusterDescriptorCategory::decision,
            {"request_uuid", "source_node_uuid", "target_node_uuid",
             "route_uuid", "fence_token_uuid", "request_epoch"}}},
          {"decision_proof_projection",
           {catalog::ClusterDescriptorCategory::decision,
            {"decision_proof_uuid", "decision_request_uuid", "proof_digest",
             "projection_generation", "invalidation_epoch"}}},
          {"route_publish",
           {catalog::ClusterDescriptorCategory::route,
            {"route_uuid", "source_node_uuid", "target_node_uuid",
             "route_profile_uuid", "route_epoch", "route_generation"}}},
          {"route_binding",
           {catalog::ClusterDescriptorCategory::route,
            {"route_uuid", "shard_uuid", "owner_node_uuid",
             "participant_node_uuid", "binding_generation"}}},
          {"fence_token_state",
           {catalog::ClusterDescriptorCategory::fence,
            {"route_uuid", "node_uuid", "fence_epoch", "fence_generation",
             "expiry_epoch"}}},
          {"shard_placement",
           {catalog::ClusterDescriptorCategory::topology,
            {"shard_uuid", "page_family_uuid", "primary_node_uuid",
             "replica_set_uuid", "placement_generation"}}},
          {"replica_topology",
           {catalog::ClusterDescriptorCategory::topology,
            {"shard_uuid", "replica_set_uuid", "replica_node_uuid",
             "replica_role_code", "topology_generation"}}},
          {"cleanup_low_water_mark",
           {catalog::ClusterDescriptorCategory::cleanup,
            {"page_family_uuid", "cleanup_epoch", "cleanup_generation",
             "low_water_transaction_number", "limbo_blocker_count"}}},
          {"limbo_reconciliation",
           {catalog::ClusterDescriptorCategory::cleanup,
            {"transaction_uuid", "participant_uuid", "reconciliation_epoch",
             "finality_state_code", "decision_proof_uuid"}}},
          {"cluster_security_binding",
           {catalog::ClusterDescriptorCategory::security,
            {"node_binding_uuid", "principal_uuid", "security_policy_uuid",
             "credential_material_uuid", "role_uuid", "security_epoch"}}},
          {"cluster_metric_profile_binding",
           {catalog::ClusterDescriptorCategory::metrics,
            {"metric_profile_uuid", "metric_family_code",
             "producer_profile_uuid", "retention_policy_uuid",
             "redaction_policy_uuid", "metric_epoch"}}},
          {"authority_provenance",
           {catalog::ClusterDescriptorCategory::authority_provenance,
            {"provider_evidence_uuid", "provider_epoch",
             "provider_generation", "source_manifest_digest",
             "signature_envelope_uuid", "compatibility_digest"}}},
      };

  for (const auto& code : catalog::RequiredClusterDescriptorCodes()) {
    const auto* descriptor = catalog::FindClusterDescriptorManifest(code);
    Require(descriptor != nullptr, "required cluster descriptor missing");
    const auto expected = required.find(code);
    Require(expected != required.end(), "required descriptor expectation missing");
    RequireDescriptor(*descriptor,
                      expected->second.first,
                      "cluster.sys." +
                          std::string(expected->second.first ==
                                                  catalog::ClusterDescriptorCategory::security
                                              ? "security."
                                              : expected->second.first ==
                                                        catalog::ClusterDescriptorCategory::metrics
                                                    ? "metrics."
                                                    : "catalog.") +
                          descriptor->table.table_name,
                      expected->second.second);
  }
}

void TestMetricDescriptorShapes() {
  const auto descriptors = metrics::BuiltinClusterMetricDescriptorManifests();
  const auto validated =
      metrics::ValidateClusterMetricDescriptorManifestSet(descriptors);
  Require(validated.ok, "built-in cluster metric descriptors did not validate");
  Require(descriptors.size() == metrics::RequiredClusterMetricFamilies().size(),
          "cluster metric descriptor count mismatch");

  for (const auto& family : metrics::RequiredClusterMetricFamilies()) {
    const auto* descriptor = metrics::FindClusterMetricDescriptorManifest(family);
    Require(descriptor != nullptr, "required cluster metric missing");
    Require(descriptor->cluster_only, "cluster metric was not cluster-only");
    Require(descriptor->external_provider_bound,
            "cluster metric was not external-provider-bound");
    Require(!descriptor->local_runtime_execution_enabled,
            "cluster metric enabled local runtime execution");
    Require(descriptor->producer_owner == "external_cluster_provider",
            "cluster metric producer owner was not external provider");
    Require(descriptor->readiness ==
                metrics::MetricReadiness::contract_ready_unwired,
            "cluster metric descriptor was wired as local runtime");
    Require(HasRequiredLabel(*descriptor, "cluster_uuid"),
            "cluster metric missing cluster UUID label");
    Require(HasRequiredLabel(*descriptor, "external_provider_uuid"),
            "cluster metric missing external provider label");
    Require(HasRequiredLabel(*descriptor, "authority_provenance_uuid"),
            "cluster metric missing authority provenance label");

    const auto metric = metrics::ToMetricDescriptor(*descriptor);
    Require(metric.cluster_only, "converted cluster metric lost cluster-only flag");
    Require(metric.visibility == metrics::MetricVisibilityScope::cluster,
            "converted cluster metric visibility was not cluster");
    Require(metric.readiness == metrics::MetricReadiness::contract_ready_unwired,
            "converted cluster metric became runtime-ready");
  }
}

void TestCatalogDescriptorRefusals() {
  auto local_execution = *catalog::FindClusterDescriptorManifest("route_publish");
  local_execution.local_runtime_execution_enabled = true;
  const auto local_execution_result =
      catalog::ValidateClusterDescriptorManifest(local_execution);
  Require(!local_execution_result.ok() &&
              local_execution_result.diagnostic.diagnostic_code ==
                  "SB-CLUSTER-DESCRIPTOR-LOCAL-EXECUTION-REFUSED",
          "cluster descriptor accepted local execution");

  auto authority_refused =
      *catalog::FindClusterDescriptorManifest("decision_request");
  authority_refused.external_provider_owned = false;
  const auto authority_result =
      catalog::ValidateClusterDescriptorManifest(authority_refused);
  Require(!authority_result.ok() &&
              authority_result.diagnostic.diagnostic_code ==
                  "SB-CLUSTER-DESCRIPTOR-AUTHORITY-REFUSED",
          "cluster descriptor accepted local authority");

  auto missing_provenance =
      *catalog::FindClusterDescriptorManifest("authority_provenance");
  RemoveColumns(&missing_provenance.table,
                [](const catalog::ClusterCatalogColumnManifest& column) {
                  return column.column_name == "authority_provenance_uuid";
                });
  const auto missing_provenance_result =
      catalog::ValidateClusterDescriptorManifest(missing_provenance);
  Require(!missing_provenance_result.ok() &&
              missing_provenance_result.diagnostic.diagnostic_code ==
                  "SB-CLUSTER-DESCRIPTOR-PROVENANCE-REQUIRED",
          "cluster descriptor accepted missing authority provenance");

  auto name_column = *catalog::FindClusterDescriptorManifest("route_binding");
  name_column.table.columns.push_back(Column("route_name", "text"));
  name_column.table.required_columns.push_back("route_name");
  const auto name_column_result =
      catalog::ValidateClusterDescriptorManifest(name_column);
  Require(!name_column_result.ok() &&
              name_column_result.diagnostic.diagnostic_code ==
                  "SB-CLUSTER-CATALOG-MANIFEST-NAME-COLUMN-REFUSED",
          "cluster descriptor accepted user-layer name column");

  auto missing = catalog::BuiltinClusterDescriptorManifestSet();
  missing.descriptors.pop_back();
  const auto missing_result =
      catalog::ValidateClusterDescriptorManifestSet(missing);
  Require(!missing_result.ok() &&
              missing_result.diagnostic.diagnostic_code ==
                  "SB-CLUSTER-DESCRIPTOR-REQUIRED-MISSING",
          "cluster descriptor set accepted missing required descriptor");
}

void TestMetricDescriptorRefusals() {
  auto local_execution = metrics::BuiltinClusterMetricDescriptorManifests().front();
  local_execution.local_runtime_execution_enabled = true;
  const auto local_result =
      metrics::ValidateClusterMetricDescriptorManifest(local_execution);
  Require(!local_result.ok &&
              local_result.diagnostic_code ==
                  "SB-CLUSTER-METRIC-DESCRIPTOR-LOCAL-EXECUTION-REFUSED",
          "cluster metric descriptor accepted local execution");

  auto local_authority = metrics::BuiltinClusterMetricDescriptorManifests().front();
  local_authority.cluster_only = false;
  const auto authority_result =
      metrics::ValidateClusterMetricDescriptorManifest(local_authority);
  Require(!authority_result.ok &&
              authority_result.diagnostic_code ==
                  "SB-CLUSTER-METRIC-DESCRIPTOR-AUTHORITY-REFUSED",
          "cluster metric descriptor accepted non-cluster authority");

  auto missing_label = metrics::BuiltinClusterMetricDescriptorManifests().front();
  missing_label.labels.erase(
      std::remove_if(missing_label.labels.begin(),
                     missing_label.labels.end(),
                     [](const metrics::MetricLabelDescriptor& label) {
                       return label.key == "authority_provenance_uuid";
                     }),
      missing_label.labels.end());
  const auto missing_label_result =
      metrics::ValidateClusterMetricDescriptorManifest(missing_label);
  Require(!missing_label_result.ok &&
              missing_label_result.diagnostic_code ==
                  "SB-CLUSTER-METRIC-DESCRIPTOR-LABEL-REQUIRED",
          "cluster metric descriptor accepted missing provenance label");
}

}  // namespace

int main() {
  TestCatalogDescriptorShapes();
  TestMetricDescriptorShapes();
  TestCatalogDescriptorRefusals();
  TestMetricDescriptorRefusals();
  return EXIT_SUCCESS;
}
