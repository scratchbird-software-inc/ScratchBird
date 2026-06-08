// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SPDX-License-Identifier: Apache-2.0
// Copyright (c) ScratchBird contributors
//
// PUBLIC_CLUSTER_CATALOG_READINESS_SUITE

#include "agent_cluster_boundary.hpp"
#include "catalog_page.hpp"
#include "cluster_catalog_manifest.hpp"
#include "cluster_catalog_record_codec.hpp"
#include "cluster_descriptor_manifest.hpp"
#include "cluster_metric_descriptors.hpp"
#include "cluster_provider/cluster_provider.hpp"
#include "cluster_schema_gating.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace agents = scratchbird::core::agents;
namespace api = scratchbird::engine::internal_api;
namespace catalog = scratchbird::core::catalog;
namespace cluster_provider = scratchbird::engine::cluster_provider;
namespace metrics = scratchbird::core::metrics;
namespace page = scratchbird::storage::page;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool Contains(const std::vector<std::string>& values, std::string_view value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

std::string Sanitize(std::string value) {
  for (char& ch : value) {
    if (ch == '.') {
      ch = '_';
    }
  }
  return value;
}

std::string MakeUuid(UuidKind kind, std::uint64_t offset) {
  const auto generated =
      uuid::GenerateDurableEngineIdentityV7(kind, 1720000000000ull + offset);
  Require(generated.ok(), "failed to generate durable engine UUID");
  return uuid::UuidToString(generated.value.value);
}

bool HasRequiredColumn(const catalog::ClusterCatalogTableManifest& table,
                       std::string_view column_name) {
  const auto* column =
      catalog::FindClusterCatalogColumn(table, std::string(column_name));
  return column != nullptr && column->required;
}

bool HasRequiredColumn(
    const catalog::ClusterCacheProjectionManifest& projection,
    std::string_view column_name) {
  const auto* column = catalog::FindClusterCacheProjectionColumn(
      projection, std::string(column_name));
  return column != nullptr && column->required;
}

bool HasRequiredLabel(
    const metrics::ClusterMetricDescriptorManifest& descriptor,
    std::string_view key) {
  return std::any_of(descriptor.labels.begin(),
                     descriptor.labels.end(),
                     [key](const metrics::MetricLabelDescriptor& label) {
                       return label.key == key && label.required;
                     });
}

void RequireUuidOnlyEngineAuthorityTable(
    const catalog::ClusterCatalogTableManifest& table,
    std::string_view full_path) {
  Require(catalog::ClusterCatalogFullTablePath(table) == full_path,
          "cluster catalog table path mismatch");
  Require(table.engine_owned, "cluster catalog table was not engine-owned");
  Require(table.cluster_shared, "cluster catalog table was not cluster-shared");
  Require(table.external_provider_bound,
          "cluster catalog table was not external-provider-bound");
  Require(!table.local_runtime_execution_enabled,
          "cluster catalog table enabled local runtime execution");
  Require(!table.mutable_by_local_core,
          "cluster catalog table allowed local core mutation");
  Require(table.uuid_only_identity,
          "cluster catalog table did not use UUID-only identity");
  Require(table.primary_key_columns.size() == 1,
          "cluster catalog table did not expose one primary UUID column");
  Require(HasRequiredColumn(table, table.primary_key_columns.front()),
          "cluster catalog primary UUID column was not required");
  Require(HasRequiredColumn(table, "status"),
          "cluster catalog table missing required status column");
  Require(HasRequiredColumn(table, "provider_record_digest"),
          "cluster catalog table missing provider digest column");
  Require(table.required_columns.size() == table.columns.size(),
          "cluster catalog table required-column inventory drifted");

  for (const auto& column : table.columns) {
    Require(column.column_name.find("name") == std::string::npos,
            "cluster catalog table embedded user-layer name text");
    Require(column.column_name.find("comment") == std::string::npos,
            "cluster catalog table embedded comment text");
    Require(column.column_name.find("description") == std::string::npos,
            "cluster catalog table embedded description text");
    Require(column.type_name != "property_bag",
            "cluster catalog table used a property bag");
    Require(column.type_name != "json" && column.type_name != "jsonb",
            "cluster catalog table used untyped JSON");
  }
}

std::set<std::string> ExpectedClusterTablePaths() {
  return {
      "cluster.sys.catalog.node",
      "cluster.sys.catalog.role",
      "cluster.sys.catalog.capability",
      "cluster.sys.catalog.filespace",
      "cluster.sys.catalog.page_family",
      "cluster.sys.catalog.route",
      "cluster.sys.catalog.route_decision",
      "cluster.sys.catalog.fence_token",
      "cluster.sys.catalog.shard_topology",
      "cluster.sys.security.node_binding",
      "cluster.sys.metrics.node_metric_profile",
  };
}

std::set<std::string> ActualCatalogSourcePaths() {
  std::set<std::string> paths;
  for (const auto& table : catalog::BuiltinClusterCatalogTableManifests()) {
    paths.insert(catalog::ClusterCatalogFullTablePath(table));
  }
  for (const auto& role_profile :
       catalog::BuiltinClusterRoleProfileManifests()) {
    paths.insert(catalog::ClusterCatalogFullTablePath(role_profile.table));
  }
  return paths;
}

void TestManifestSchemaAndProjectionClosure() {
  const auto manifest = catalog::BuiltinClusterCatalogManifestSet();
  const auto manifest_result =
      catalog::ValidateClusterCatalogManifestSet(manifest);
  Require(manifest_result.ok(), "cluster catalog manifest set did not validate");
  Require(manifest.engine_owned,
          "cluster catalog manifest set was not engine-owned");
  Require(manifest.external_provider_required,
          "cluster catalog manifest set did not require an external provider");
  Require(!manifest.local_runtime_execution_enabled,
          "cluster catalog manifest set enabled local execution");
  Require(manifest.tables.size() == ExpectedClusterTablePaths().size(),
          "cluster catalog manifest table count changed");
  Require(manifest.role_profiles.size() ==
              catalog::BuiltinClusterCatalogRoleCodes().size(),
          "cluster role profile manifest count changed");

  std::set<std::string> table_paths;
  for (const auto& table : manifest.tables) {
    const std::string full_path = catalog::ClusterCatalogFullTablePath(table);
    table_paths.insert(full_path);
    RequireUuidOnlyEngineAuthorityTable(table, full_path);
  }
  Require(table_paths == ExpectedClusterTablePaths(),
          "cluster catalog manifest exact table set changed");

  std::set<std::string> role_paths;
  for (const auto& role_profile : manifest.role_profiles) {
    Require(Contains(catalog::BuiltinClusterCatalogRoleCodes(),
                     role_profile.role_code),
            "cluster role profile used an unexpected role code");
    const std::string full_path =
        catalog::ClusterCatalogFullTablePath(role_profile.table);
    Require(full_path ==
                "cluster.sys.catalog.node_role_profile_" +
                    role_profile.role_code,
            "cluster role profile table path mismatch");
    RequireUuidOnlyEngineAuthorityTable(role_profile.table, full_path);
    role_paths.insert(full_path);
  }
  Require(role_paths.size() == manifest.role_profiles.size(),
          "cluster role profile paths were not unique");

  catalog::ClusterSchemaRootRequest standalone;
  const auto standalone_decision =
      catalog::EvaluateClusterSchemaRootAccess(standalone);
  Require(!standalone_decision.ok(),
          "standalone cluster schema root was unexpectedly present");
  Require(!standalone_decision.schema_present,
          "standalone cluster schema root claimed schema presence");
  Require(standalone_decision.failed_closed,
          "standalone cluster schema root did not fail closed");
  Require(standalone_decision.diagnostic.diagnostic_code ==
              catalog::kClusterSchemaAbsentDiagnosticCode,
          "standalone cluster schema root diagnostic changed");

  catalog::ClusterSchemaRootRequest joined;
  joined.joined_cluster_catalog_state = true;
  joined.cluster_authority_available = true;
  joined.external_provider_available = true;
  const auto joined_decision =
      catalog::EvaluateClusterSchemaRootAccess(joined);
  Require(joined_decision.ok(),
          "external-provider cluster schema root was absent");
  Require(joined_decision.schema_present,
          "external-provider cluster schema root did not report presence");
  Require(!joined_decision.local_runtime_execution_enabled,
          "external-provider cluster schema root enabled local execution");
  Require(Contains(joined_decision.schema_paths, "cluster.sys.catalog"),
          "cluster schema root missing catalog path");
  Require(Contains(joined_decision.schema_paths, "cluster.sys.security"),
          "cluster schema root missing security path");
  Require(Contains(joined_decision.schema_paths, "cluster.sys.metrics"),
          "cluster schema root missing metrics path");

  const auto projection_set =
      catalog::BuiltinClusterCacheProjectionManifestSet();
  const auto projection_result =
      catalog::ValidateClusterCacheProjectionManifestSet(projection_set);
  Require(projection_result.ok(),
          "cluster cache projection manifest set did not validate");
  Require(projection_set.projection_only,
          "cluster cache projection set was not projection-only");
  Require(projection_set.source_authority_required,
          "cluster cache projection set did not require source authority");
  Require(!projection_set.local_runtime_execution_enabled,
          "cluster cache projection set enabled local execution");

  const auto expected_sources = ActualCatalogSourcePaths();
  Require(projection_set.projections.size() == expected_sources.size(),
          "cluster cache projection count did not match source manifests");
  std::set<std::string> projection_sources;
  for (const auto& projection : projection_set.projections) {
    Require(projection.schema_path == "sys.catalog.cluster_cache",
            "cluster cache projection used the wrong schema path");
    Require(!projection.cluster_authority,
            "cluster cache projection claimed cluster authority");
    Require(projection.projection_only,
            "cluster cache projection was not projection-only");
    Require(projection.source_authority_required,
            "cluster cache projection did not require source authority");
    Require(!projection.local_runtime_execution_enabled,
            "cluster cache projection enabled local execution");
    Require(HasRequiredColumn(projection, "projection_uuid"),
            "cluster cache projection missing projection UUID");
    Require(HasRequiredColumn(projection, "source_record_uuid"),
            "cluster cache projection missing source record UUID");
    Require(HasRequiredColumn(projection, "source_authority_epoch"),
            "cluster cache projection missing source authority epoch");
    Require(HasRequiredColumn(projection, "source_generation"),
            "cluster cache projection missing source generation");
    Require(HasRequiredColumn(projection, "source_digest"),
            "cluster cache projection missing source digest");
    Require(HasRequiredColumn(projection, "invalidation_epoch"),
            "cluster cache projection missing invalidation epoch");
    Require(HasRequiredColumn(projection, "status"),
            "cluster cache projection missing status");
    Require(!catalog::ClusterCacheProjectionFullTablePath(projection).empty(),
            "cluster cache projection table path was empty");
    projection_sources.insert(projection.source_cluster_table_path);
  }
  Require(projection_sources == expected_sources,
          "cluster cache projections did not cover every source manifest");
}

std::string ValueForColumn(const catalog::ClusterCatalogTableManifest& table,
                           const catalog::ClusterCatalogColumnManifest& column,
                           std::uint64_t ordinal) {
  if (column.type_name == "uuid") {
    const UuidKind kind =
        column.column_name == "cluster_uuid" ? UuidKind::cluster
                                             : UuidKind::object;
    return MakeUuid(kind, ordinal);
  }
  if (column.type_name == "uint64") {
    return std::to_string(1000 + ordinal);
  }
  if (column.type_name == "status_code") {
    return "active";
  }
  if (column.column_name == "role_code" &&
      table.record_family == "node_role_profile") {
    const std::string prefix = "node_role_profile_";
    Require(table.table_name.rfind(prefix, 0) == 0,
            "role profile table name did not include role prefix");
    return table.table_name.substr(prefix.size());
  }
  if (column.column_name == "role_code") {
    return "storage";
  }
  if (column.type_name == "digest") {
    return "digest-" + Sanitize(table.stable_table_id) + "-" +
           std::to_string(ordinal);
  }
  return Sanitize(column.column_name) + "_" + std::to_string(ordinal);
}

catalog::ClusterCatalogRecord RecordForTable(
    const catalog::ClusterCatalogTableManifest& table,
    std::uint64_t base_ordinal) {
  catalog::ClusterCatalogRecord record;
  record.table_path = catalog::ClusterCatalogFullTablePath(table);
  record.codec_version = catalog::kClusterCatalogRecordCodecVersionCurrent;
  record.schema_version = table.manifest_version;
  std::uint64_t ordinal = base_ordinal;
  for (const auto& column : table.columns) {
    record.fields.push_back(
        {column.column_name, ValueForColumn(table, column, ordinal++)});
  }
  return record;
}

std::vector<catalog::ClusterCatalogRecord> BuiltinRecords() {
  std::vector<catalog::ClusterCatalogRecord> records;
  std::uint64_t ordinal = 1;
  for (const auto& table : catalog::BuiltinClusterCatalogTableManifests()) {
    records.push_back(RecordForTable(table, ordinal));
    ordinal += 100;
  }
  for (const auto& role_profile :
       catalog::BuiltinClusterRoleProfileManifests()) {
    records.push_back(RecordForTable(role_profile.table, ordinal));
    ordinal += 100;
  }
  return records;
}

catalog::ClusterCatalogNameResolverRow NameResolverFor(
    const catalog::ClusterCatalogRecord& record,
    std::uint64_t ordinal) {
  const std::string object_name =
      Sanitize(record.table_path) + "_" + std::to_string(ordinal);
  catalog::ClusterCatalogNameResolverRow row;
  row.row_uuid = MakeUuid(UuidKind::row, 10000 + ordinal);
  row.target_record_uuid = catalog::ClusterCatalogRecordPrimaryUuidValue(record);
  row.target_table_path = record.table_path;
  row.language_tag = "und";
  row.identifier_profile_uuid = MakeUuid(UuidKind::object, 11000 + ordinal);
  row.name_class = "system_identifier";
  row.raw_name_text = object_name;
  row.display_name = object_name;
  row.normalized_lookup_key = object_name;
  row.exact_lookup_key = object_name;
  row.full_path_lookup_key = record.table_path + "." + object_name;
  row.catalog_generation = 1;
  return row;
}

catalog::ClusterCatalogCommentResolverRow CommentResolverFor(
    const catalog::ClusterCatalogRecord& record,
    std::uint64_t ordinal) {
  catalog::ClusterCatalogCommentResolverRow row;
  row.row_uuid = MakeUuid(UuidKind::row, 20000 + ordinal);
  row.comment_uuid = MakeUuid(UuidKind::object, 21000 + ordinal);
  row.target_record_uuid = catalog::ClusterCatalogRecordPrimaryUuidValue(record);
  row.target_table_path = record.table_path;
  row.language_tag = "und";
  row.comment_text = "cluster catalog public readiness proof " +
                     std::to_string(ordinal);
  row.catalog_generation = 1;
  return row;
}

catalog::ClusterCatalogRecordSet ValidRecordSet() {
  catalog::ClusterCatalogRecordSet set;
  set.records = BuiltinRecords();
  std::uint64_t ordinal = 1;
  for (const auto& record : set.records) {
    set.name_resolver_rows.push_back(NameResolverFor(record, ordinal));
    set.comment_resolver_rows.push_back(CommentResolverFor(record, ordinal));
    ++ordinal;
  }
  return set;
}

void TestCodecResolverAndPageRecordClosure() {
  const auto records = BuiltinRecords();
  Require(records.size() == ActualCatalogSourcePaths().size(),
          "cluster catalog record fixture did not cover every source manifest");

  std::vector<page::CatalogPageRow> rows;
  std::uint32_t ordinal = 1;
  for (const auto& record : records) {
    const auto encoded = catalog::EncodeClusterCatalogRecord(record);
    Require(encoded.ok(), "cluster catalog record failed to encode");
    const auto decoded = catalog::DecodeClusterCatalogRecord(encoded.encoded);
    Require(decoded.ok(), "cluster catalog record failed to decode");
    Require(decoded.record.table_path == record.table_path,
            "cluster catalog codec changed table path");
    Require(decoded.record.fields.size() == record.fields.size(),
            "cluster catalog codec changed field count");
    Require(catalog::ClusterCatalogRecordPrimaryUuidValue(decoded.record) ==
                catalog::ClusterCatalogRecordPrimaryUuidValue(record),
            "cluster catalog codec changed primary UUID");

    const auto row = catalog::EncodeClusterCatalogRecordPageRow(
        record, ordinal++);
    Require(row.ok(), "cluster catalog page row failed to encode");
    rows.push_back(row.row);
  }

  const auto pages = page::BuildCatalogPageSet(rows, 8192, 40, 80);
  Require(pages.ok(), "cluster catalog records did not serialize to pages");
  std::size_t decoded_rows = 0;
  for (const auto& serialized : pages.pages) {
    const auto parsed =
        page::ParseCatalogPageBody(serialized.body, serialized.page_number);
    Require(parsed.ok(), "cluster catalog page body did not parse");
    for (const auto& row : parsed.body.rows) {
      Require(row.kind == page::CatalogPageRowKind::cluster_catalog_record,
              "cluster catalog page row kind changed");
      Require(catalog::DecodeClusterCatalogRecordPageRow(row).ok(),
              "cluster catalog page row did not decode");
      ++decoded_rows;
    }
  }
  Require(decoded_rows == records.size(),
          "cluster catalog page roundtrip lost records");

  const auto valid_set = ValidRecordSet();
  const auto valid_result =
      catalog::ValidateClusterCatalogRecordSet(valid_set);
  Require(valid_result.ok(), "cluster catalog record set was refused");
  Require(valid_result.accepted_record_uuids.size() == valid_set.records.size(),
          "cluster catalog record set did not accept every record");

  auto missing_names = valid_set;
  missing_names.name_resolver_rows.clear();
  const auto missing_name_result =
      catalog::ValidateClusterCatalogRecordSet(missing_names);
  Require(!missing_name_result.ok() &&
              missing_name_result.diagnostic.diagnostic_code ==
                  "SB-CLUSTER-CATALOG-RECORD-RESOLVER-MISSING",
          "cluster catalog records did not require resolver rows");

  auto missing_comments = valid_set;
  missing_comments.comment_resolver_rows.clear();
  const auto missing_comment_result =
      catalog::ValidateClusterCatalogRecordSet(missing_comments);
  Require(!missing_comment_result.ok() &&
              missing_comment_result.diagnostic.diagnostic_code ==
                  "SB-CLUSTER-CATALOG-RECORD-COMMENT-MISSING",
          "cluster catalog records did not require comment rows");

  auto embedded_name = records.front();
  embedded_name.fields.push_back({"display_name", "cluster node"});
  const auto embedded_name_result =
      catalog::EncodeClusterCatalogRecord(embedded_name);
  Require(!embedded_name_result.ok() &&
              embedded_name_result.diagnostic.diagnostic_code ==
                  "SB-CLUSTER-CATALOG-RECORD-USER-TEXT-REFUSED",
          "cluster catalog codec accepted embedded user-layer name text");

  auto property_bag = records.front();
  property_bag.fields.push_back({"role_properties", "k=v"});
  const auto property_bag_result =
      catalog::EncodeClusterCatalogRecord(property_bag);
  Require(!property_bag_result.ok() &&
              property_bag_result.diagnostic.diagnostic_code ==
                  "SB-CLUSTER-CATALOG-RECORD-PROPERTY-BAG-REFUSED",
          "cluster catalog codec accepted a property bag");
}

void TestDescriptorMetricAndAuthorityClosure() {
  const auto descriptor_set = catalog::BuiltinClusterDescriptorManifestSet();
  const auto descriptor_result =
      catalog::ValidateClusterDescriptorManifestSet(descriptor_set);
  Require(descriptor_result.ok(),
          "cluster descriptor manifest set did not validate");
  Require(descriptor_set.external_provider_required,
          "cluster descriptor set did not require an external provider");
  Require(!descriptor_set.local_runtime_execution_enabled,
          "cluster descriptor set enabled local execution");
  Require(!descriptor_set.mutable_by_local_core,
          "cluster descriptor set allowed local mutation");
  Require(descriptor_set.transaction_inventory_remains_finality_authority,
          "cluster descriptor set displaced MGA transaction inventory");
  Require(descriptor_set.descriptors.size() ==
              catalog::RequiredClusterDescriptorCodes().size(),
          "cluster descriptor count changed");

  std::set<catalog::ClusterDescriptorCategory> categories;
  for (const auto& descriptor : descriptor_set.descriptors) {
    categories.insert(descriptor.category);
    Require(descriptor.external_provider_owned,
            "cluster descriptor was not external-provider-owned");
    Require(descriptor.descriptor_only,
            "cluster descriptor was not descriptor-only");
    Require(!descriptor.local_runtime_execution_enabled,
            "cluster descriptor enabled local execution");
    Require(!descriptor.mutable_by_local_core,
            "cluster descriptor allowed local mutation");
    Require(descriptor.authority_provenance_required,
            "cluster descriptor did not require authority provenance");
    Require(descriptor.transaction_inventory_remains_finality_authority,
            "cluster descriptor displaced MGA transaction inventory");
    Require(HasRequiredColumn(descriptor.table,
                              "authority_provenance_uuid"),
            "cluster descriptor missing authority provenance UUID");
    Require(HasRequiredColumn(descriptor.table, "provider_record_digest"),
            "cluster descriptor missing provider digest");
    Require(catalog::ValidateClusterDescriptorManifest(descriptor).ok(),
            "cluster descriptor failed standalone validation");
  }
  Require(categories.count(catalog::ClusterDescriptorCategory::decision) == 1,
          "cluster decision descriptors missing");
  Require(categories.count(catalog::ClusterDescriptorCategory::route) == 1,
          "cluster route descriptors missing");
  Require(categories.count(catalog::ClusterDescriptorCategory::fence) == 1,
          "cluster fence descriptors missing");
  Require(categories.count(catalog::ClusterDescriptorCategory::topology) == 1,
          "cluster topology descriptors missing");
  Require(categories.count(catalog::ClusterDescriptorCategory::cleanup) == 1,
          "cluster cleanup descriptors missing");
  Require(categories.count(catalog::ClusterDescriptorCategory::security) == 1,
          "cluster security descriptors missing");
  Require(categories.count(catalog::ClusterDescriptorCategory::metrics) == 1,
          "cluster metric descriptors missing");
  Require(categories.count(
              catalog::ClusterDescriptorCategory::authority_provenance) == 1,
          "cluster authority provenance descriptors missing");

  for (std::string_view code : {"decision_request",
                                "route_publish",
                                "fence_token_state",
                                "shard_placement",
                                "cleanup_low_water_mark",
                                "limbo_reconciliation",
                                "cluster_security_binding",
                                "cluster_metric_profile_binding",
                                "authority_provenance"}) {
    Require(catalog::FindClusterDescriptorManifest(std::string(code)) != nullptr,
            "required cluster descriptor was missing");
  }

  const auto metric_descriptors =
      metrics::BuiltinClusterMetricDescriptorManifests();
  const auto metric_result =
      metrics::ValidateClusterMetricDescriptorManifestSet(metric_descriptors);
  Require(metric_result.ok,
          "cluster metric descriptor manifest set did not validate");
  Require(metric_descriptors.size() ==
              metrics::RequiredClusterMetricFamilies().size(),
          "cluster metric descriptor count changed");
  for (const auto& family : metrics::RequiredClusterMetricFamilies()) {
    const auto* descriptor =
        metrics::FindClusterMetricDescriptorManifest(family);
    Require(descriptor != nullptr,
            "required cluster metric descriptor was missing");
    Require(descriptor->cluster_only,
            "cluster metric descriptor was not cluster-only");
    Require(descriptor->external_provider_bound,
            "cluster metric descriptor was not external-provider-bound");
    Require(!descriptor->local_runtime_execution_enabled,
            "cluster metric descriptor enabled local execution");
    Require(descriptor->producer_owner == "external_cluster_provider",
            "cluster metric descriptor producer was not external provider");
    Require(descriptor->readiness ==
                metrics::MetricReadiness::contract_ready_unwired,
            "cluster metric descriptor was wired as a local runtime metric");
    Require(HasRequiredLabel(*descriptor, "cluster_uuid"),
            "cluster metric descriptor missing cluster UUID label");
    Require(HasRequiredLabel(*descriptor, "external_provider_uuid"),
            "cluster metric descriptor missing provider label");
    Require(HasRequiredLabel(*descriptor, "authority_provenance_uuid"),
            "cluster metric descriptor missing provenance label");
  }
  Require(metrics::FindClusterMetricDescriptorManifest(
              "sb_cluster_provider_boundary_refusal_total") != nullptr,
          "cluster provider boundary refusal metric descriptor missing");
}

std::string ProviderRowField(const api::EngineRowValue& row,
                             std::string_view name) {
  for (const auto& [field_name, value] : row.fields) {
    if (field_name == name) {
      return value.encoded_value;
    }
  }
  Fail("cluster provider info field missing");
}

bool HasDiagnostic(const api::EngineApiResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) {
      return true;
    }
  }
  return false;
}

bool HasUnsupportedFeature(const api::EngineApiResult& result,
                           std::string_view feature) {
  for (const auto& unsupported : result.unsupported_features) {
    if (unsupported.feature == feature && !unsupported.reason.empty()) {
      return true;
    }
  }
  return false;
}

bool HasAgentDiagnostic(const agents::AgentClusterBoundaryResult& result,
                        std::string_view code) {
  return std::find(result.diagnostic_codes.begin(),
                   result.diagnostic_codes.end(),
                   code) != result.diagnostic_codes.end();
}

api::EngineRequestContext EngineContext() {
  api::EngineRequestContext context;
  context.security_context_present = true;
  context.cluster_authority_available = true;
  context.database_uuid.canonical = "database:public-cluster-readiness-pcr098";
  context.cluster_uuid.canonical = "cluster:public-cluster-readiness-pcr098";
  context.principal_uuid.canonical =
      "principal:public-cluster-readiness-pcr098";
  context.trace_tags.push_back("public_cluster_catalog_readiness_suite");
  return context;
}

cluster_provider::ClusterProviderRequest ProviderRequest(
    std::string operation_id) {
  cluster_provider::ClusterProviderRequest request;
  request.context = EngineContext();
  request.envelope.operation_id = std::move(operation_id);
  request.envelope.opcode = "SBLR_PUBLIC_CLUSTER_CATALOG_READINESS_SUITE";
  request.envelope.trace_key = "public-cluster-catalog-readiness-suite";
  request.envelope.requires_security_context = true;
  request.envelope.requires_cluster_authority = true;
  request.envelope.contains_sql_text = false;
  request.api_request.context = request.context;
  request.api_request.operation_id = request.envelope.operation_id;
  return request;
}

agents::AgentRuntimeContext AgentContext() {
  agents::AgentRuntimeContext context;
  context.security_context_present = true;
  context.cluster_authority_available = true;
  context.standalone_edition = false;
  context.principal_uuid = "principal:public-cluster-readiness-pcr098";
  context.database_uuid = "database:public-cluster-readiness-pcr098";
  context.cluster_uuid = "cluster:public-cluster-readiness-pcr098";
  context.trace_tags.push_back("public_cluster_catalog_readiness_suite");
  return context;
}

void TestExternalProviderFailClosedClosure() {
  const auto info = cluster_provider::DescribeClusterProvider();
  const std::string provider_type(info.provider_type);
  const std::string support_status(info.support_status);
  Require(provider_type == "no_cluster" || provider_type == "compile_link_stub",
          "public cluster readiness suite linked an executable in-tree provider");
  Require(support_status == "not_enabled" ||
              support_status == "compile_link_only",
          "public cluster provider advertised executable support");
  Require(!info.supports_execution,
          "public cluster provider reported execution support");
  Require(!cluster_provider::ClusterProviderSupportsExecution(),
          "cluster provider support helper reported execution support");

  auto inspect_request = ProviderRequest(
      std::string(cluster_provider::kClusterProviderInfoOperationId));
  const auto inspect =
      cluster_provider::InspectClusterProvider(inspect_request);
  Require(inspect.ok, "cluster provider inspect route failed");
  Require(inspect.result_shape.rows.size() == 1,
          "cluster provider inspect route did not emit one info row");
  Require(ProviderRowField(inspect.result_shape.rows.front(),
                           "provider_type") == provider_type,
          "cluster provider inspect row changed provider type");
  Require(ProviderRowField(inspect.result_shape.rows.front(),
                           "support_status") == support_status,
          "cluster provider inspect row changed support status");
  Require(ProviderRowField(inspect.result_shape.rows.front(),
                           "supports_execution") == "false",
          "cluster provider inspect row claimed execution support");

  const auto executed = cluster_provider::ExecuteClusterOperation(
      ProviderRequest("cluster.public_catalog_readiness"));
  Require(!executed.ok,
          "in-tree cluster provider accepted readiness execution");
  Require(executed.cluster_authority_required,
          "in-tree cluster provider did not require cluster authority");
  Require(executed.result_shape.rows.empty(),
          "in-tree cluster provider emitted mutable result rows");
  if (provider_type == "no_cluster") {
    Require(HasDiagnostic(executed,
                          cluster_provider::kClusterSupportNotEnabledCode),
            "no_cluster provider did not publish fail-closed diagnostic");
    Require(HasUnsupportedFeature(executed, "cluster.provider"),
            "no_cluster provider did not publish unsupported feature");
  } else {
    Require(HasDiagnostic(
                executed,
                cluster_provider::kClusterHandshakeStubCompileLinkOnlyCode),
            "compile-link stub did not publish compile-link-only diagnostic");
    Require(HasUnsupportedFeature(executed, "cluster.provider.stub"),
            "compile-link stub did not publish unsupported feature");
  }

  const auto route = agents::RouteAgentClusterProviderBoundary(
      AgentContext(), "agent.cluster.public_pcr098", true);
  Require(route.provider_called,
          "agent cluster boundary did not call the provider boundary");
  Require(!route.ok, "agent cluster boundary accepted in-tree provider");
  Require(route.cluster_path_failed_closed,
          "agent cluster boundary did not fail closed");
  Require(route.external_provider_required,
          "agent cluster boundary did not require an external provider");
  Require(HasAgentDiagnostic(
              route, agents::kAgentClusterExternalProviderRequiredCode),
          "agent cluster boundary missing external-provider diagnostic");

  agents::AgentClusterLeaseState state;
  agents::AgentClusterLeaseRequest lease_request;
  lease_request.surface = agents::AgentClusterLeaseSurface::acquire_lease;
  lease_request.agent_type_id = "cluster_scheduler_manager";
  lease_request.instance_uuid = "public-cluster-readiness-instance";
  lease_request.now_microseconds = 100;
  lease_request.lease_duration_microseconds = 500;
  lease_request.production_live_path = true;
  const auto lease = agents::ApplyAgentClusterLeaseSurface(
      AgentContext(), lease_request, &state);
  Require(!lease.ok, "agent cluster lease accepted in-tree provider");
  Require(lease.cluster_path_failed_closed,
          "agent cluster lease did not fail closed");
  Require(lease.external_provider_required,
          "agent cluster lease did not require an external provider");
  Require(state.state == agents::AgentClusterLeadershipState::follower,
          "agent cluster lease mutated local leadership state");
}

}  // namespace

int main() {
  TestManifestSchemaAndProjectionClosure();
  TestCodecResolverAndPageRecordClosure();
  TestDescriptorMetricAndAuthorityClosure();
  TestExternalProviderFailClosedClosure();
  return EXIT_SUCCESS;
}
