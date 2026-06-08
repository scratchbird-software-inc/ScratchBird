// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/sys_information_projection.hpp"

#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace info = scratchbird::engine::internal_api;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) { Fail(message); }
}

void RequireOk(const info::SysInformationProjectionResult& result,
               std::string_view message) {
  if (!result.ok) {
    std::cerr << result.diagnostic_code << " " << result.diagnostic_detail << '\n';
  }
  Require(result.ok, message);
}

void RequireValidationOk(const info::SysInformationProjectionValidationResult& result,
                         std::string_view message) {
  if (!result.ok) {
    for (const auto& code : result.diagnostic_codes) {
      std::cerr << code << '\n';
    }
  }
  Require(result.ok, message);
}

std::string Field(const info::SysInformationProjectionRow& row, std::string_view name) {
  for (const auto& field : row.fields) {
    if (field.first == name) { return field.second; }
  }
  return {};
}

bool RowContainsFieldValue(const info::SysInformationProjectionResult& result,
                           std::string_view field_name,
                           std::string_view value) {
  for (const auto& row : result.rows) {
    if (Field(row, field_name) == value) { return true; }
  }
  return false;
}

void RequireNoUuidColumnsOrValues(const info::SysInformationProjectionResult& result) {
  for (const auto& row : result.rows) {
    for (const auto& field : row.fields) {
      Require(!info::SysInformationProjectionColumnNameExposesUuid(field.first),
              "sys.information projection exposed a UUID-shaped column");
      Require(field.second.find("uuid-") == std::string::npos,
              "sys.information projection exposed a raw UUID-shaped value");
      Require(field.second.find("object-") == std::string::npos,
              "sys.information projection exposed an internal object token");
    }
  }
}

info::SysInformationProjectionContext Context() {
  info::SysInformationProjectionContext context;
  context.catalog_display_name = "CustomerDB";
  context.session_language = "fr-CA";
  context.default_language = "en";
  context.visible_catalog_generation_id = 4;
  context.strict_mode = false;
  context.cluster_authority_available = false;
  return context;
}

std::vector<info::SysInformationCatalogObjectSource> CatalogObjects() {
  return {
      {.object_uuid = "schema-app",
       .object_class = "schema",
       .schema_uuid = "",
       .catalog_generation_id = 1,
       .created_local_transaction_id = 1},
      {.object_uuid = "table-customers",
       .object_class = "table",
       .schema_uuid = "schema-app",
       .table_type = "BASE TABLE",
       .catalog_generation_id = 2,
       .created_local_transaction_id = 2},
      {.object_uuid = "view-open-orders",
       .object_class = "view",
       .schema_uuid = "schema-app",
       .table_type = "VIEW",
       .catalog_generation_id = 3,
       .created_local_transaction_id = 3},
      {.object_uuid = "table-hidden",
       .object_class = "table",
       .schema_uuid = "schema-app",
       .table_type = "BASE TABLE",
       .catalog_generation_id = 3,
       .created_local_transaction_id = 3,
       .hidden = true},
      {.object_uuid = "table-future",
       .object_class = "table",
       .schema_uuid = "schema-app",
       .table_type = "BASE TABLE",
       .catalog_generation_id = 7,
       .created_local_transaction_id = 7},
      {.object_uuid = "table-cluster-route",
       .object_class = "table",
       .schema_uuid = "schema-app",
       .table_type = "BASE TABLE",
       .catalog_generation_id = 3,
       .created_local_transaction_id = 3,
       .cluster_path = true},
  };
}

std::vector<info::SysInformationResolverNameSource> ResolverNames() {
  return {
      {.object_uuid = "schema-app",
       .object_class = "schema",
       .language_tag = "en",
       .name_class = "primary",
       .display_name = "app",
       .raw_name_text = "raw_schema_name_must_not_leak",
       .normalized_lookup_key = "APP",
       .catalog_generation_id = 1},
      {.object_uuid = "table-customers",
       .object_class = "table",
       .scope_uuid = "schema-app",
       .language_tag = "en",
       .name_class = "primary",
       .display_name = "customers",
       .raw_name_text = "raw_customers_name_must_not_leak",
       .normalized_lookup_key = "CUSTOMERS",
       .catalog_generation_id = 2},
      {.object_uuid = "view-open-orders",
       .object_class = "view",
       .scope_uuid = "schema-app",
       .language_tag = "fr-CA",
       .name_class = "primary",
       .display_name = "commandes_ouvertes",
       .raw_name_text = "raw_open_orders_name_must_not_leak",
       .normalized_lookup_key = "COMMANDES_OUVERTES",
       .catalog_generation_id = 3},
      {.object_uuid = "table-hidden",
       .object_class = "table",
       .scope_uuid = "schema-app",
       .language_tag = "en",
       .name_class = "primary",
       .display_name = "hidden_table",
       .catalog_generation_id = 3},
      {.object_uuid = "table-future",
       .object_class = "table",
       .scope_uuid = "schema-app",
       .language_tag = "en",
       .name_class = "primary",
       .display_name = "future_table",
       .catalog_generation_id = 7},
      {.object_uuid = "table-cluster-route",
       .object_class = "table",
       .scope_uuid = "schema-app",
       .language_tag = "en",
       .name_class = "primary",
       .display_name = "cluster_route",
       .catalog_generation_id = 3},
  };
}

std::vector<info::SysInformationCommentSource> Comments() {
  return {
      {.object_uuid = "table-customers",
       .object_class = "table",
       .language_tag = "en",
       .comment_text = "Customer master data",
       .catalog_generation_id = 2},
      {.object_uuid = "view-open-orders",
       .object_class = "view",
       .language_tag = "fr-CA",
       .comment_text = "Commandes ouvertes visibles",
       .catalog_generation_id = 3},
  };
}

std::vector<info::SysInformationColumnSource> Columns() {
  return {
      {.relation_object_uuid = "table-customers",
       .schema_uuid = "schema-app",
       .column_name = "customer_id",
       .ordinal_position = 1,
       .datatype_name = "int64",
       .domain_name = "",
       .is_nullable = "NO",
       .comment_text = "Stable customer identifier",
       .catalog_generation_id = 3},
      {.relation_object_uuid = "table-hidden",
       .schema_uuid = "schema-app",
       .column_name = "hidden_value",
       .ordinal_position = 1,
       .datatype_name = "text",
       .catalog_generation_id = 3},
  };
}

std::vector<info::SysInformationSettingSource> Settings() {
  return {
      {.setting_name = "metadata.visibility.strict_mode",
       .setting_value_display = "false",
       .authority = "engine",
       .default_source = "compiled_default",
       .redaction_state = "not_redacted",
       .visibility_state = "visible",
       .catalog_generation_id = 1},
      {.setting_name = "secret.backend.token",
       .setting_value_display = "<redacted>",
       .authority = "engine",
       .default_source = "runtime_secret",
       .redaction_state = "redacted",
       .visibility_state = "security_redacted",
       .catalog_generation_id = 1},
  };
}

std::vector<info::SysInformationFrontendAgentSource> FrontendAgents() {
  return {
      {.agent_name = "memory_governor",
       .agent_type_id = "memory_governor",
       .scope_kind = "local",
       .state = "running",
       .health_state = "healthy",
       .enabled = "YES",
       .policy_name = "default_memory_policy",
       .catalog_generation_id = 1},
  };
}

std::vector<info::SysInformationProtectedMaterialSource> ProtectedMaterials() {
  return {
      {.material_path = "security.materials.kms_root",
       .material_name = "kms_root",
       .purpose_class = "cloud_ops_use",
       .storage_class = "wrapped",
       .lifecycle_state = "active",
       .active_version_number = "2",
       .retention_policy_name = "retain_current",
       .access_policy_name = "security_admin_only",
       .release_policy_name = "purpose_bound_release",
       .purge_policy_name = "retention_checked_purge",
       .audit_policy_name = "audit_required",
       .visibility_state = "security_redacted",
       .catalog_generation_id = 1},
  };
}

std::vector<info::SysInformationProtectedMaterialVersionSource> ProtectedMaterialVersions() {
  return {
      {.material_path = "security.materials.kms_root",
       .material_name = "kms_root",
       .version_number = "2",
       .storage_class = "wrapped",
       .rotation_state = "current",
       .valid_from_state = "visible",
       .valid_until_state = "",
       .retention_state = "active",
       .purged = "NO",
       .payload_hash_present = "YES",
       .audit_state = "audit_present",
       .catalog_generation_id = 1},
  };
}

std::vector<std::string> ClusterPacketPaths() {
  return {
      "cluster.sys.catalog_readable.objects",
      "cluster.sys.catalog_readable.object_names",
      "cluster.sys.catalog_readable.object_comments",
      "cluster.sys.catalog_readable.nodes",
      "cluster.sys.catalog_readable.node_roles",
      "cluster.sys.catalog_readable.node_role_profiles",
      "cluster.sys.catalog_readable.routes",
      "cluster.sys.catalog_readable.shards",
      "cluster.sys.catalog_readable.cluster_tables",
      "cluster.sys.catalog_readable.transactions",
      "cluster.sys.catalog_readable.limbo",
      "cluster.sys.catalog_readable.write_intent_segments",
      "cluster.sys.catalog_readable.reconciliation_cases",
      "cluster.sys.catalog_readable.merge_policies",
      "cluster.sys.catalog_readable.security_domains",
      "cluster.sys.catalog_readable.key_share_groups",
      "cluster.sys.catalog_readable.filespaces",
      "cluster.sys.catalog_readable.resources",
      "cluster.sys.catalog_readable.parser_listener_gateways",
      "cluster.sys.catalog_readable.metrics_catalog",
      "cluster.sys.catalog_readable.diagnostics_catalog",
      "cluster.sys.frontend.agents",
      "cluster.sys.frontend.agent_actions",
      "cluster.sys.frontend.agent_evidence",
      "cluster.sys.frontend.agent_metric_dependencies",
  };
}

void TestBuiltinProjectionDefinitionsValidate() {
  RequireValidationOk(info::ValidateBuiltinSysInformationProjectionDefinitions(),
                      "built-in sys.information projection definitions failed validation");

  std::map<std::string, int> families;
  for (const auto& definition : info::BuiltinSysInformationProjectionDefinitions()) {
    Require(definition.view_scope == "local",
            "local frontend packet projection did not record local scope");
    Require(!definition.ui_area.empty(),
            "local frontend packet projection did not record UI area");
    Require(!definition.key_columns.empty(),
            "local frontend packet projection did not record key columns");
    Require(!definition.description.empty(),
            "local frontend packet projection did not record description");
    ++families[info::SysInformationProjectionFamilyName(definition.family)];
    Require(definition.cluster_path_fail_closed,
            "sys.information projection did not fail closed for cluster paths");
    Require(definition.authorization_filter_required,
            "sys.information projection lacked authorization filter requirement");
    Require(definition.mga_snapshot_visibility_required,
            "sys.information projection lacked MGA snapshot visibility requirement");
    Require(!definition.exposes_internal_uuid,
            "sys.information projection exposed internal UUID authority");
    for (const auto& column : definition.columns) {
      if (column.exposes_internal_uuid) {
        Require(info::SysInformationProjectionColumnNameExposesUuid(column.column_name),
                "sys.information explicit UUID metadata did not name a UUID column");
        Require(column.logical_type == "uuid",
                "sys.information explicit UUID metadata was not resolver-sourced uuid");
      } else {
        Require(!info::SysInformationProjectionColumnNameExposesUuid(column.column_name),
                "sys.information column name exposed UUID authority");
      }
    }
  }
  Require(info::BuiltinSysInformationProjectionDefinitions().size() == 98,
          "frontend catalog packet local projection count drifted");
  Require(families["catalog_readable"] == 42,
          "catalog_readable frontend packet count drifted");
  Require(families["standard_information_schema"] == 30,
          "information_schema frontend packet count drifted");
  Require(families["scratchbird_extension"] == 16,
          "information extension frontend packet count drifted");
  Require(families["frontend_projection"] == 10,
          "frontend projection packet count drifted");
}

void TestAllLocalPacketViewsAreQueryable() {
  for (const auto& definition : info::BuiltinSysInformationProjectionDefinitions()) {
    const auto result = info::BuildSysInformationProjection(
        definition.view_path,
        Context(),
        {},
        {},
        {},
        {},
        {},
        Settings(),
        FrontendAgents(),
        ProtectedMaterials(),
        ProtectedMaterialVersions());
    RequireOk(result, std::string("local frontend packet view was not queryable: ") +
                          definition.view_path);
    RequireNoUuidColumnsOrValues(result);
  }
}

void TestClusterPacketViewsFailClosed() {
  for (const auto& path : ClusterPacketPaths()) {
    const auto result = info::BuildSysInformationProjection(
        path, Context(), CatalogObjects(), ResolverNames());
    Require(!result.ok, "cluster frontend packet view did not fail closed");
    Require(result.diagnostic_code == info::kSysInformationDiagnosticClusterScopeForbidden,
            "cluster frontend packet view used wrong diagnostic");
  }
}

void TestTablesProjectionJoinsResolverAndFilters() {
  const auto result = info::BuildSysInformationProjection(
      "sys.information_schema.tables", Context(), CatalogObjects(), ResolverNames());
  RequireOk(result, "tables projection failed");
  Require(result.rows.size() == 2, "tables projection did not apply security/MGA/cluster filters");
  Require(RowContainsFieldValue(result, "table_name", "customers"),
          "tables projection did not use default-language resolver fallback");
  Require(RowContainsFieldValue(result, "table_name", "commandes_ouvertes"),
          "tables projection did not use session-language resolver name");
  Require(RowContainsFieldValue(result, "table_schema", "app"),
          "tables projection did not join schema name through resolver");
  Require(!RowContainsFieldValue(result, "table_name", "hidden_table"),
          "tables projection exposed hidden metadata");
  Require(!RowContainsFieldValue(result, "table_name", "future_table"),
          "tables projection ignored MGA catalog generation visibility");
  Require(!RowContainsFieldValue(result, "table_name", "cluster_route"),
          "tables projection exposed cluster-only metadata in standalone scope");
  RequireNoUuidColumnsOrValues(result);

  for (const auto& row : result.rows) {
    Require(Field(row, "table_name") != "raw_customers_name_must_not_leak",
            "tables projection leaked raw resolver text");
    Require(Field(row, "table_name") != "raw_open_orders_name_must_not_leak",
            "tables projection leaked raw resolver text");
  }
}

void TestSchemataProjectionUsesResolver() {
  const auto result = info::BuildSysInformationProjection(
      "sys.information_schema.schemata", Context(), CatalogObjects(), ResolverNames());
  RequireOk(result, "schemata projection failed");
  Require(result.rows.size() == 1, "schemata projection returned wrong row count");
  Require(Field(result.rows.front(), "catalog_name") == "CustomerDB",
          "schemata projection returned wrong catalog display name");
  Require(Field(result.rows.front(), "schema_name") == "app",
          "schemata projection did not use resolver-backed schema name");
  RequireNoUuidColumnsOrValues(result);
}

void TestScratchBirdExtensionViewsHideUuidAndUseResolvers() {
  const auto paths = info::BuildSysInformationProjection(
      "sys.information_schema.scratchbird_object_paths",
      Context(),
      CatalogObjects(),
      ResolverNames());
  RequireOk(paths, "scratchbird object paths projection failed");
  Require(RowContainsFieldValue(paths, "object_path", "app.customers"),
          "object paths projection did not compose resolver-backed path");
  Require(RowContainsFieldValue(paths, "object_path", "app.commandes_ouvertes"),
          "object paths projection did not compose localized resolver-backed path");
  RequireNoUuidColumnsOrValues(paths);

  const auto comments = info::BuildSysInformationProjection(
      "sys.information_schema.scratchbird_object_comments",
      Context(),
      CatalogObjects(),
      ResolverNames(),
      Comments());
  RequireOk(comments, "scratchbird object comments projection failed");
  Require(RowContainsFieldValue(comments, "comment_text", "Customer master data"),
          "comment projection did not join centralized comment source");
  Require(RowContainsFieldValue(comments, "comment_text", "Commandes ouvertes visibles"),
          "comment projection did not use session-language comment fallback");
  RequireNoUuidColumnsOrValues(comments);
}

void TestIndexProfileProjection() {
  const auto result = info::BuildSysInformationProjection(
      "sys.information_schema.scratchbird_index_profiles",
      Context(),
      {},
      {});
  RequireOk(result, "scratchbird index profile projection failed");
  Require(RowContainsFieldValue(result, "profile_name", "sys_catalog_object_identity_uuid_hash"),
          "index profile projection omitted UUID hash profile");
  Require(RowContainsFieldValue(result, "access_method", "hash_equality"),
          "index profile projection omitted hash equality access method");
  Require(RowContainsFieldValue(result, "access_method", "btree_ordered"),
          "index profile projection omitted ordered B-tree access method");
  Require(RowContainsFieldValue(result, "resolver_boundary", "identity_resolver"),
          "index profile projection omitted resolver boundary metadata");
  RequireNoUuidColumnsOrValues(result);
}

void TestReadableCatalogProjectionRows() {
  const auto objects = info::BuildSysInformationProjection(
      "sys.catalog_readable.objects",
      Context(),
      CatalogObjects(),
      ResolverNames(),
      Comments());
  RequireOk(objects, "catalog_readable objects projection failed");
  Require(RowContainsFieldValue(objects, "object_path", "app.customers"),
          "catalog_readable objects did not build readable object path");
  Require(RowContainsFieldValue(objects, "comment_text", "Customer master data"),
          "catalog_readable objects did not join comments");
  RequireNoUuidColumnsOrValues(objects);

  const auto relations = info::BuildSysInformationProjection(
      "sys.catalog_readable.relations",
      Context(),
      CatalogObjects(),
      ResolverNames(),
      Comments());
  RequireOk(relations, "catalog_readable relations projection failed");
  Require(RowContainsFieldValue(relations, "relation_path", "app.customers"),
          "catalog_readable relations did not build relation path");
  Require(!RowContainsFieldValue(relations, "relation_name", "hidden_table"),
          "catalog_readable relations exposed hidden relation");
  RequireNoUuidColumnsOrValues(relations);

  const auto columns = info::BuildSysInformationProjection(
      "sys.catalog_readable.columns",
      Context(),
      CatalogObjects(),
      ResolverNames(),
      Comments(),
      {},
      Columns());
  RequireOk(columns, "catalog_readable columns projection failed");
  Require(RowContainsFieldValue(columns, "column_name", "customer_id"),
          "catalog_readable columns did not project visible column");
  Require(!RowContainsFieldValue(columns, "column_name", "hidden_value"),
          "catalog_readable columns exposed hidden relation column");
  RequireNoUuidColumnsOrValues(columns);

  const auto datatypes = info::BuildSysInformationProjection(
      "sys.catalog_readable.datatypes", Context(), {}, {});
  RequireOk(datatypes, "catalog_readable datatypes projection failed");
  Require(RowContainsFieldValue(datatypes, "type_name", "int64"),
          "catalog_readable datatypes omitted builtin int64");
  RequireNoUuidColumnsOrValues(datatypes);

  const auto indexes = info::BuildSysInformationProjection(
      "sys.catalog_readable.indexes", Context(), {}, {});
  RequireOk(indexes, "catalog_readable indexes projection failed");
  Require(RowContainsFieldValue(indexes, "index_name", "sys_catalog_object_identity_uuid_hash"),
          "catalog_readable indexes omitted catalog profile evidence");
  RequireNoUuidColumnsOrValues(indexes);
}

void TestInformationColumnsProjectionRows() {
  const auto result = info::BuildSysInformationProjection(
      "sys.information_schema.columns",
      Context(),
      CatalogObjects(),
      ResolverNames(),
      Comments(),
      {},
      Columns());
  RequireOk(result, "sys.information columns projection failed");
  Require(RowContainsFieldValue(result, "table_name", "customers"),
          "sys.information columns did not join table name");
  Require(RowContainsFieldValue(result, "column_name", "customer_id"),
          "sys.information columns did not project visible column");
  Require(RowContainsFieldValue(result, "data_type", "int64"),
          "sys.information columns did not project datatype");
  RequireNoUuidColumnsOrValues(result);
}

void TestSettingsAgentsAndProtectedMaterialRows() {
  const auto settings = info::BuildSysInformationProjection(
      "sys.catalog_readable.settings",
      Context(),
      {},
      {},
      {},
      {},
      {},
      Settings());
  RequireOk(settings, "catalog_readable settings projection failed");
  Require(RowContainsFieldValue(settings, "setting_value_display", "<redacted>"),
          "catalog_readable settings did not project redacted display value");
  Require(RowContainsFieldValue(settings, "redaction_state", "redacted"),
          "catalog_readable settings did not project redaction state");
  RequireNoUuidColumnsOrValues(settings);

  const auto agents = info::BuildSysInformationProjection(
      "sys.frontend.agents",
      Context(),
      {},
      {},
      {},
      {},
      {},
      {},
      FrontendAgents());
  RequireOk(agents, "frontend agents projection failed");
  Require(RowContainsFieldValue(agents, "agent_name", "memory_governor"),
          "frontend agents did not project agent name");
  Require(RowContainsFieldValue(agents, "policy_name", "default_memory_policy"),
          "frontend agents did not project policy name");
  RequireNoUuidColumnsOrValues(agents);

  const auto materials = info::BuildSysInformationProjection(
      "sys.information.scratchbird_protected_material",
      Context(),
      {},
      {},
      {},
      {},
      {},
      {},
      {},
      ProtectedMaterials());
  RequireOk(materials, "protected material projection failed");
  Require(RowContainsFieldValue(materials, "material_name", "kms_root"),
          "protected material projection did not project redacted material name");
  Require(RowContainsFieldValue(materials, "visibility_state", "security_redacted"),
          "protected material projection did not project visibility state");
  RequireNoUuidColumnsOrValues(materials);

  const auto versions = info::BuildSysInformationProjection(
      "sys.information.scratchbird_protected_material_versions",
      Context(),
      {},
      {},
      {},
      {},
      {},
      {},
      {},
      {},
      ProtectedMaterialVersions());
  RequireOk(versions, "protected material version projection failed");
  Require(RowContainsFieldValue(versions, "version_number", "2"),
          "protected material version projection did not project version number");
  Require(RowContainsFieldValue(versions, "payload_hash_present", "YES"),
          "protected material version projection did not redact payload hash value");
  RequireNoUuidColumnsOrValues(versions);
}

void TestMissingNameStrictModeFailsClosed() {
  auto objects = CatalogObjects();
  objects.push_back({.object_uuid = "table-no-visible-name",
                     .object_class = "table",
                     .schema_uuid = "schema-app",
                     .table_type = "BASE TABLE",
                     .catalog_generation_id = 3,
                     .created_local_transaction_id = 3});
  auto context = Context();
  context.strict_mode = true;

  const auto result = info::BuildSysInformationProjection(
      "sys.information_schema.tables", context, objects, ResolverNames());
  Require(!result.ok, "strict missing resolver name did not fail closed");
  Require(result.diagnostic_code == info::kSysInformationDiagnosticNameNotFound,
          "strict missing resolver name used wrong diagnostic");
}

void TestClusterAndUnsupportedPathsFailClosed() {
  const auto cluster_result = info::BuildSysInformationProjection(
      "cluster.sys.information_schema.tables", Context(), CatalogObjects(), ResolverNames());
  Require(!cluster_result.ok, "cluster sys.information path did not fail closed");
  Require(cluster_result.diagnostic_code == info::kSysInformationDiagnosticClusterScopeForbidden,
          "cluster sys.information failure used wrong diagnostic");

  const auto unsupported = info::BuildSysInformationProjection(
      "sys.information_schema.not_a_catalog_view", Context(), CatalogObjects(), ResolverNames());
  Require(!unsupported.ok, "unsupported projection did not fail closed");
  Require(unsupported.diagnostic_code == info::kSysInformationDiagnosticViewUnsupported,
          "unsupported projection used wrong diagnostic");
}

}  // namespace

int main() {
  TestBuiltinProjectionDefinitionsValidate();
  TestAllLocalPacketViewsAreQueryable();
  TestClusterPacketViewsFailClosed();
  TestTablesProjectionJoinsResolverAndFilters();
  TestSchemataProjectionUsesResolver();
  TestScratchBirdExtensionViewsHideUuidAndUseResolvers();
  TestIndexProfileProjection();
  TestReadableCatalogProjectionRows();
  TestInformationColumnsProjectionRows();
  TestSettingsAgentsAndProtectedMaterialRows();
  TestMissingNameStrictModeFailsClosed();
  TestClusterAndUnsupportedPathsFailClosed();
  return EXIT_SUCCESS;
}
