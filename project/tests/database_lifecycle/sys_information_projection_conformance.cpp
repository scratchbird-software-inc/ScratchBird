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

std::size_t RequireRowIndex(const info::SysInformationProjectionResult& result,
                            std::string_view field_name,
                            std::string_view value,
                            std::string_view message) {
  for (std::size_t index = 0; index < result.rows.size(); ++index) {
    if (Field(result.rows[index], field_name) == value) { return index; }
  }
  Fail(message);
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
      {.object_uuid = "schema-sys",
       .object_class = "schema",
       .schema_uuid = "",
       .catalog_generation_id = 1,
       .created_local_transaction_id = 1},
      {.object_uuid = "schema-sys-catalog",
       .object_class = "schema",
       .schema_uuid = "",
       .parent_object_uuid = "schema-sys",
       .catalog_generation_id = 1,
       .created_local_transaction_id = 1},
      {.object_uuid = "schema-sys-catalog-readable",
       .object_class = "schema",
       .schema_uuid = "",
       .parent_object_uuid = "schema-sys",
       .catalog_generation_id = 1,
       .created_local_transaction_id = 1},
      {.object_uuid = "table-sys-catalog-identity",
       .object_class = "table",
       .schema_uuid = "schema-sys-catalog",
       .parent_object_uuid = "schema-sys-catalog",
       .table_type = "SYSTEM TABLE",
       .catalog_generation_id = 1,
       .created_local_transaction_id = 1},
      {.object_uuid = "view-sys-readable-nav-tree",
       .object_class = "view",
       .schema_uuid = "schema-sys-catalog-readable",
       .parent_object_uuid = "schema-sys-catalog-readable",
       .table_type = "SYSTEM VIEW",
       .catalog_generation_id = 1,
       .created_local_transaction_id = 1},
      {.object_uuid = "schema-app",
       .object_class = "schema",
       .schema_uuid = "",
       .catalog_generation_id = 1,
       .created_local_transaction_id = 1},
      {.object_uuid = "schema-reporting",
       .object_class = "schema",
       .schema_uuid = "",
       .parent_object_uuid = "schema-app",
       .catalog_generation_id = 2,
       .created_local_transaction_id = 2},
      {.object_uuid = "schema-emulated",
       .object_class = "schema",
       .schema_uuid = "",
       .catalog_generation_id = 1,
       .created_local_transaction_id = 1},
      {.object_uuid = "schema-remote",
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
      {.object_uuid = "procedure-refresh-customers",
       .object_class = "procedure",
       .schema_uuid = "schema-app",
       .catalog_generation_id = 3,
       .created_local_transaction_id = 3},
      {.object_uuid = "function-customer-score",
       .object_class = "function",
       .schema_uuid = "schema-app",
       .catalog_generation_id = 3,
       .created_local_transaction_id = 3},
      {.object_uuid = "package-customer-admin",
       .object_class = "package",
       .schema_uuid = "schema-app",
       .catalog_generation_id = 3,
       .created_local_transaction_id = 3},
      {.object_uuid = "sequence-customer-id",
       .object_class = "sequence",
       .schema_uuid = "schema-app",
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
      {.object_uuid = "user-alice",
       .object_class = "user",
       .catalog_generation_id = 2,
       .created_local_transaction_id = 2},
      {.object_uuid = "group-engineering",
       .object_class = "group",
       .catalog_generation_id = 2,
       .created_local_transaction_id = 2},
      {.object_uuid = "role-sysarch",
       .object_class = "role",
       .catalog_generation_id = 2,
       .created_local_transaction_id = 2},
      {.object_uuid = "membership-alice-engineering-user-group",
       .object_class = "security_user_group_membership",
       .schema_uuid = "group-engineering",
       .parent_object_uuid = "user-alice",
       .table_type = "group",
       .catalog_generation_id = 2,
       .created_local_transaction_id = 2},
      {.object_uuid = "membership-alice-engineering-group-user",
       .object_class = "security_group_user_membership",
       .schema_uuid = "user-alice",
       .parent_object_uuid = "group-engineering",
       .table_type = "user",
       .catalog_generation_id = 2,
       .created_local_transaction_id = 2},
      {.object_uuid = "membership-alice-sysarch-user-role",
       .object_class = "security_user_role_membership",
       .schema_uuid = "role-sysarch",
       .parent_object_uuid = "user-alice",
       .table_type = "role",
       .catalog_generation_id = 2,
       .created_local_transaction_id = 2},
      {.object_uuid = "membership-alice-sysarch-role-user",
       .object_class = "security_role_user_membership",
       .schema_uuid = "user-alice",
       .parent_object_uuid = "role-sysarch",
       .table_type = "user",
       .catalog_generation_id = 2,
       .created_local_transaction_id = 2},
      {.object_uuid = "membership-engineering-sysarch-group-role",
       .object_class = "security_group_role_membership",
       .schema_uuid = "role-sysarch",
       .parent_object_uuid = "group-engineering",
       .table_type = "role",
       .catalog_generation_id = 2,
       .created_local_transaction_id = 2},
      {.object_uuid = "membership-engineering-sysarch-role-group",
       .object_class = "security_role_group_membership",
       .schema_uuid = "group-engineering",
       .parent_object_uuid = "role-sysarch",
       .table_type = "group",
       .catalog_generation_id = 2,
       .created_local_transaction_id = 2},
      {.object_uuid = "grant-alice-select",
       .object_class = "security_grant",
       .schema_uuid = "table-customers",
       .parent_object_uuid = "user-alice",
       .table_type = "principal",
       .catalog_generation_id = 2,
       .created_local_transaction_id = 2},
      {.object_uuid = "policy-default-security",
       .object_class = "security_policy",
       .catalog_generation_id = 2,
       .created_local_transaction_id = 2},
      {.object_uuid = "config-general-security",
       .object_class = "security_configuration",
       .catalog_generation_id = 2,
       .created_local_transaction_id = 2},
  };
}

std::vector<info::SysInformationResolverNameSource> ResolverNames() {
  return {
      {.object_uuid = "schema-sys",
       .object_class = "schema",
       .language_tag = "en",
       .name_class = "primary",
       .display_name = "sys",
       .normalized_lookup_key = "SYS",
       .catalog_generation_id = 1},
      {.object_uuid = "schema-sys-catalog",
       .object_class = "schema",
       .scope_uuid = "schema-sys",
       .language_tag = "en",
       .name_class = "primary",
       .display_name = "catalog",
       .normalized_lookup_key = "CATALOG",
       .catalog_generation_id = 1},
      {.object_uuid = "schema-sys-catalog-readable",
       .object_class = "schema",
       .scope_uuid = "schema-sys",
       .language_tag = "en",
       .name_class = "primary",
       .display_name = "catalog_readable",
       .normalized_lookup_key = "CATALOG_READABLE",
       .catalog_generation_id = 1},
      {.object_uuid = "table-sys-catalog-identity",
       .object_class = "table",
       .scope_uuid = "schema-sys-catalog",
       .language_tag = "en",
       .name_class = "primary",
       .display_name = "object_identity",
       .normalized_lookup_key = "OBJECT_IDENTITY",
       .catalog_generation_id = 1},
      {.object_uuid = "view-sys-readable-nav-tree",
       .object_class = "view",
       .scope_uuid = "schema-sys-catalog-readable",
       .language_tag = "en",
       .name_class = "primary",
       .display_name = "navigator_tree",
       .normalized_lookup_key = "NAVIGATOR_TREE",
       .catalog_generation_id = 1},
      {.object_uuid = "schema-app",
       .object_class = "schema",
       .language_tag = "en",
       .name_class = "primary",
       .display_name = "app",
       .raw_name_text = "raw_schema_name_must_not_leak",
       .normalized_lookup_key = "APP",
       .catalog_generation_id = 1},
      {.object_uuid = "schema-reporting",
       .object_class = "schema",
       .scope_uuid = "schema-app",
       .language_tag = "en",
       .name_class = "primary",
       .display_name = "reporting",
       .normalized_lookup_key = "REPORTING",
       .catalog_generation_id = 2},
      {.object_uuid = "schema-emulated",
       .object_class = "schema",
       .language_tag = "en",
       .name_class = "primary",
       .display_name = "emulated",
       .normalized_lookup_key = "EMULATED",
       .catalog_generation_id = 1},
      {.object_uuid = "schema-remote",
       .object_class = "schema",
       .language_tag = "en",
       .name_class = "primary",
       .display_name = "remote",
       .normalized_lookup_key = "REMOTE",
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
      {.object_uuid = "procedure-refresh-customers",
       .object_class = "procedure",
       .scope_uuid = "schema-app",
       .language_tag = "en",
       .name_class = "primary",
       .display_name = "refresh_customers",
       .normalized_lookup_key = "REFRESH_CUSTOMERS",
       .catalog_generation_id = 3},
      {.object_uuid = "function-customer-score",
       .object_class = "function",
       .scope_uuid = "schema-app",
       .language_tag = "en",
       .name_class = "primary",
       .display_name = "customer_score",
       .normalized_lookup_key = "CUSTOMER_SCORE",
       .catalog_generation_id = 3},
      {.object_uuid = "package-customer-admin",
       .object_class = "package",
       .scope_uuid = "schema-app",
       .language_tag = "en",
       .name_class = "primary",
       .display_name = "customer_admin",
       .normalized_lookup_key = "CUSTOMER_ADMIN",
       .catalog_generation_id = 3},
      {.object_uuid = "sequence-customer-id",
       .object_class = "sequence",
       .scope_uuid = "schema-app",
       .language_tag = "en",
       .name_class = "primary",
       .display_name = "customer_id_seq",
       .normalized_lookup_key = "CUSTOMER_ID_SEQ",
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
      {.object_uuid = "user-alice",
       .object_class = "user",
       .language_tag = "en",
       .name_class = "primary",
       .display_name = "alice",
       .catalog_generation_id = 2},
      {.object_uuid = "group-engineering",
       .object_class = "group",
       .language_tag = "en",
       .name_class = "primary",
       .display_name = "engineering",
       .catalog_generation_id = 2},
      {.object_uuid = "role-sysarch",
       .object_class = "role",
       .language_tag = "en",
       .name_class = "primary",
       .display_name = "sysarch",
       .catalog_generation_id = 2},
      {.object_uuid = "membership-alice-engineering-user-group",
       .object_class = "security_user_group_membership",
       .language_tag = "en",
       .name_class = "primary",
       .display_name = "engineering",
       .catalog_generation_id = 2},
      {.object_uuid = "membership-alice-engineering-group-user",
       .object_class = "security_group_user_membership",
       .language_tag = "en",
       .name_class = "primary",
       .display_name = "alice",
       .catalog_generation_id = 2},
      {.object_uuid = "membership-alice-sysarch-user-role",
       .object_class = "security_user_role_membership",
       .language_tag = "en",
       .name_class = "primary",
       .display_name = "sysarch",
       .catalog_generation_id = 2},
      {.object_uuid = "membership-alice-sysarch-role-user",
       .object_class = "security_role_user_membership",
       .language_tag = "en",
       .name_class = "primary",
       .display_name = "alice",
       .catalog_generation_id = 2},
      {.object_uuid = "membership-engineering-sysarch-group-role",
       .object_class = "security_group_role_membership",
       .language_tag = "en",
       .name_class = "primary",
       .display_name = "sysarch",
       .catalog_generation_id = 2},
      {.object_uuid = "membership-engineering-sysarch-role-group",
       .object_class = "security_role_group_membership",
       .language_tag = "en",
       .name_class = "primary",
       .display_name = "engineering",
       .catalog_generation_id = 2},
      {.object_uuid = "grant-alice-select",
       .object_class = "security_grant",
       .language_tag = "en",
       .name_class = "primary",
       .display_name = "SELECT",
       .catalog_generation_id = 2},
      {.object_uuid = "policy-default-security",
       .object_class = "security_policy",
       .language_tag = "en",
       .name_class = "primary",
       .display_name = "default_security_policy",
       .catalog_generation_id = 2},
      {.object_uuid = "config-general-security",
       .object_class = "security_configuration",
       .language_tag = "en",
       .name_class = "primary",
       .display_name = "general_security_configuration",
       .catalog_generation_id = 2},
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

std::vector<info::SysInformationAgentPolicySource> AgentPolicies() {
  return {
      {.agent_uuid = "agent-memory-governor",
       .agent_ref = "memory_governor",
       .policy_uuid = "policy-default-memory",
       .policy_ref = "default_memory_policy",
       .policy_name = "default_memory_policy",
       .policy_family = "memory",
       .version_uuid = "policy-default-memory-v1",
       .version_ref = "default_memory_policy:v1",
       .active_state = "active",
       .validation_state = "valid",
       .attached_at = "2026-06-01T00:00:00Z",
       .attached_by = "sysarch",
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
  Require(info::BuiltinSysInformationProjectionDefinitions().size() == 119,
          "frontend catalog packet local projection count drifted");
  Require(families["catalog_readable"] == 62,
          "catalog_readable frontend packet count drifted");
  Require(families["standard_information_schema"] == 30,
          "information_schema frontend packet count drifted");
  Require(families["scratchbird_extension"] == 16,
          "information extension frontend packet count drifted");
  Require(families["frontend_projection"] == 11,
          "frontend projection packet count drifted");
  const auto* object_identity = info::FindSysInformationProjectionDefinition(
      "sys.catalog.object_identity");
  Require(object_identity != nullptr,
          "physical sys.catalog.object_identity table projection was not registered");
  bool has_object_uuid_column = false;
  for (const auto& column : object_identity->columns) {
    if (column.column_name == "object_uuid" && column.logical_type == "uuid" &&
        column.exposes_internal_uuid) {
      has_object_uuid_column = true;
      break;
    }
  }
  Require(has_object_uuid_column,
          "physical sys.catalog.object_identity did not preserve raw UUID column metadata");
  Require(info::FindSysInformationProjectionDefinition("sys.key_descriptor") != nullptr,
          "physical sys.key_descriptor table projection was not registered");
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
  Require(result.rows.size() >= 4, "tables projection omitted visible user or system relations");
  Require(RowContainsFieldValue(result, "table_name", "customers"),
          "tables projection did not use default-language resolver fallback");
  Require(RowContainsFieldValue(result, "table_name", "commandes_ouvertes"),
          "tables projection did not use session-language resolver name");
  Require(RowContainsFieldValue(result, "table_name", "object_identity"),
          "tables projection omitted visible system catalog table");
  Require(RowContainsFieldValue(result, "table_name", "navigator_tree"),
          "tables projection omitted visible system catalog view");
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
  Require(result.rows.size() >= 7, "schemata projection omitted visible physical schemas");
  Require(Field(result.rows.front(), "catalog_name") == "CustomerDB",
          "schemata projection returned wrong catalog display name");
  Require(RowContainsFieldValue(result, "schema_name", "sys"),
          "schemata projection did not use resolver-backed system schema name");
  Require(RowContainsFieldValue(result, "schema_name", "catalog"),
          "schemata projection did not use resolver-backed system catalog schema name");
  Require(RowContainsFieldValue(result, "schema_name", "catalog_readable"),
          "schemata projection did not use resolver-backed readable catalog schema name");
  Require(RowContainsFieldValue(result, "schema_name", "app"),
          "schemata projection did not use resolver-backed root schema name");
  Require(RowContainsFieldValue(result, "schema_name", "reporting"),
          "schemata projection did not use resolver-backed child schema name");
  Require(RowContainsFieldValue(result, "schema_name", "emulated"),
          "schemata projection hid empty contextual schema metadata");
  Require(RowContainsFieldValue(result, "schema_name", "remote"),
          "schemata projection hid empty contextual schema metadata");
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

  const auto object_tree = info::BuildSysInformationProjection(
      "sys.catalog_readable.object_tree",
      Context(),
      CatalogObjects(),
      ResolverNames(),
      Comments());
  RequireOk(object_tree, "catalog_readable object tree projection failed");
  Require(RowContainsFieldValue(object_tree, "object_path", "app"),
          "catalog_readable object tree omitted root schema path");
  Require(RowContainsFieldValue(object_tree, "object_path", "app.customers"),
          "catalog_readable object tree did not build child object path");
  Require(RowContainsFieldValue(object_tree, "parent_path", "app"),
          "catalog_readable object tree did not expose parent display path");
  Require(RowContainsFieldValue(object_tree, "object_kind", "table"),
          "catalog_readable object tree omitted table object kind");
  Require(!RowContainsFieldValue(object_tree, "object_name", "hidden_table"),
          "catalog_readable object tree exposed hidden object");
  Require(!RowContainsFieldValue(object_tree, "object_name", "future_table"),
          "catalog_readable object tree ignored MGA catalog generation visibility");
  RequireNoUuidColumnsOrValues(object_tree);

  const auto navigator_tree = info::BuildSysInformationProjection(
      "sys.catalog_readable.navigator_tree",
      Context(),
      CatalogObjects(),
      ResolverNames(),
      Comments(),
      {},
      Columns());
  RequireOk(navigator_tree, "catalog_readable navigator tree projection failed");
  Require(RowContainsFieldValue(navigator_tree, "node_name", "CustomerDB"),
          "navigator tree omitted database root node");
  Require(RowContainsFieldValue(navigator_tree, "node_role", "database.management"),
          "navigator tree omitted management pseudo group");
  Require(RowContainsFieldValue(navigator_tree, "node_role", "database.filespaces"),
          "navigator tree omitted filespaces pseudo group");
  Require(RowContainsFieldValue(navigator_tree, "node_role", "schema"),
          "navigator tree omitted physical schema nodes");
  Require(RowContainsFieldValue(navigator_tree, "node_role", "table"),
          "navigator tree omitted physical table nodes");
  Require(RowContainsFieldValue(navigator_tree, "node_role", "view"),
          "navigator tree omitted physical view nodes");
  Require(RowContainsFieldValue(navigator_tree, "node_role", "security.users"),
          "navigator tree omitted security users pseudo group");
  Require(RowContainsFieldValue(navigator_tree, "node_role", "security.user"),
          "navigator tree omitted security user object");
  Require(RowContainsFieldValue(navigator_tree, "node_role", "security.user.groups"),
          "navigator tree omitted user groups pseudo group");
  Require(RowContainsFieldValue(navigator_tree, "node_role", "security.user.group"),
          "navigator tree omitted user group membership object");
  Require(RowContainsFieldValue(navigator_tree, "node_role", "security.user.role"),
          "navigator tree omitted user role membership object");
  Require(RowContainsFieldValue(navigator_tree, "node_role", "security.user.grant"),
          "navigator tree omitted user direct grant object");
  Require(RowContainsFieldValue(navigator_tree, "node_role", "security.group"),
          "navigator tree omitted security group object");
  Require(RowContainsFieldValue(navigator_tree, "node_role", "security.group.user"),
          "navigator tree omitted group user membership object");
  Require(RowContainsFieldValue(navigator_tree, "node_role", "security.group.role"),
          "navigator tree omitted group role membership object");
  Require(RowContainsFieldValue(navigator_tree, "node_role", "security.role"),
          "navigator tree omitted security role object");
  Require(RowContainsFieldValue(navigator_tree, "node_role", "security.role.user"),
          "navigator tree omitted role user membership object");
  Require(RowContainsFieldValue(navigator_tree, "node_role", "security.role.group"),
          "navigator tree omitted role group membership object");
  Require(RowContainsFieldValue(navigator_tree, "node_role", "security.policy"),
          "navigator tree omitted security policy object");
  Require(RowContainsFieldValue(navigator_tree, "node_role", "security.configuration"),
          "navigator tree omitted security configuration object");
  Require(RowContainsFieldValue(navigator_tree, "node_name", "Programmability"),
          "navigator tree omitted programmability pseudo group");
  Require(RowContainsFieldValue(navigator_tree, "node_name", "Triggers"),
          "navigator tree omitted triggers pseudo group");
  Require(RowContainsFieldValue(navigator_tree, "node_name", "refresh_customers"),
          "navigator tree omitted procedure under physical or projected branch");
  Require(RowContainsFieldValue(navigator_tree, "node_name", "customer_score"),
          "navigator tree omitted function under physical or projected branch");
  Require(RowContainsFieldValue(navigator_tree, "node_name", "customer_admin"),
          "navigator tree omitted package under physical or projected branch");
  Require(RowContainsFieldValue(navigator_tree, "node_name", "customer_id_seq"),
          "navigator tree omitted sequence under physical or projected branch");
  Require(!RowContainsFieldValue(navigator_tree, "node_name", "hidden_table"),
          "navigator tree exposed hidden object");
  Require(!RowContainsFieldValue(navigator_tree, "node_name", "future_table"),
          "navigator tree ignored MGA catalog generation visibility");
  Require(!RowContainsFieldValue(navigator_tree, "node_path", "CustomerDB/emulated"),
          "navigator tree exposed empty emulated root schema");
  Require(!RowContainsFieldValue(navigator_tree, "node_path", "CustomerDB/remote"),
          "navigator tree exposed empty remote root schema");
  Require(!RowContainsFieldValue(navigator_tree, "node_path", "CustomerDB/Security"),
          "navigator tree exposed security pseudo group at root");
  Require(!RowContainsFieldValue(navigator_tree, "node_path", "CustomerDB/Programmability"),
          "navigator tree exposed programmability pseudo group at root");
  Require(!RowContainsFieldValue(navigator_tree, "node_path", "CustomerDB/Domains"),
          "navigator tree exposed domains pseudo group at root");
  Require(!RowContainsFieldValue(navigator_tree, "node_path", "CustomerDB/Jobs"),
          "navigator tree exposed jobs pseudo group at root");
  Require(!RowContainsFieldValue(navigator_tree, "node_path", "CustomerDB/Diagnostics / Metrics"),
          "navigator tree exposed diagnostics pseudo group at root");
  Require(!RowContainsFieldValue(navigator_tree, "node_path", "CustomerDB/Triggers"),
          "navigator tree exposed triggers pseudo group at root");
  Require(!RowContainsFieldValue(navigator_tree, "node_path", "CustomerDB/File-spaces"),
          "navigator tree exposed filespaces pseudo group at root");

  const auto database_index = RequireRowIndex(navigator_tree,
                                             "node_path",
                                              "CustomerDB",
                                              "navigator tree omitted database path");
  const auto management_index = RequireRowIndex(navigator_tree,
                                               "node_path",
                                               "CustomerDB/Management",
                                               "navigator tree omitted management path");
  const auto security_user_index = RequireRowIndex(navigator_tree,
                                                  "node_path",
                                                  "CustomerDB/Management/Security/users/alice",
                                                  "navigator tree omitted security user path");
  const auto security_user_groups_index = RequireRowIndex(
      navigator_tree,
      "node_path",
      "CustomerDB/Management/Security/users/alice/groups",
      "navigator tree omitted security user groups path");
  const auto security_user_group_membership_index = RequireRowIndex(
      navigator_tree,
      "node_path",
      "CustomerDB/Management/Security/users/alice/groups/engineering",
      "navigator tree omitted security user group membership path");
  const auto security_user_role_membership_index = RequireRowIndex(
      navigator_tree,
      "node_path",
      "CustomerDB/Management/Security/users/alice/roles/sysarch",
      "navigator tree omitted security user role membership path");
  const auto security_user_grant_index = RequireRowIndex(
      navigator_tree,
      "node_path",
      "CustomerDB/Management/Security/users/alice/grants/SELECT",
      "navigator tree omitted security user direct grant path");
  const auto security_group_index = RequireRowIndex(navigator_tree,
                                                   "node_path",
                                                   "CustomerDB/Management/Security/groups/engineering",
                                                   "navigator tree omitted security group path");
  const auto security_group_user_index = RequireRowIndex(
      navigator_tree,
      "node_path",
      "CustomerDB/Management/Security/groups/engineering/users/alice",
      "navigator tree omitted security group user membership path");
  const auto security_group_role_index = RequireRowIndex(
      navigator_tree,
      "node_path",
      "CustomerDB/Management/Security/groups/engineering/roles/sysarch",
      "navigator tree omitted security group role membership path");
  const auto security_group_grants_index = RequireRowIndex(
      navigator_tree,
      "node_path",
      "CustomerDB/Management/Security/groups/engineering/grants",
      "navigator tree omitted security group grants path");
  const auto security_role_index = RequireRowIndex(navigator_tree,
                                                  "node_path",
                                                  "CustomerDB/Management/Security/roles/sysarch",
                                                  "navigator tree omitted security role path");
  const auto security_role_users_index = RequireRowIndex(
      navigator_tree,
      "node_path",
      "CustomerDB/Management/Security/roles/sysarch/users",
      "navigator tree omitted security role users path");
  const auto security_role_user_index = RequireRowIndex(
      navigator_tree,
      "node_path",
      "CustomerDB/Management/Security/roles/sysarch/users/alice",
      "navigator tree omitted security role user membership path");
  const auto security_role_group_index = RequireRowIndex(
      navigator_tree,
      "node_path",
      "CustomerDB/Management/Security/roles/sysarch/groups/engineering",
      "navigator tree omitted security role group membership path");
  const auto security_policy_index = RequireRowIndex(
      navigator_tree,
      "node_path",
      "CustomerDB/Management/Security/policies/default_security_policy",
      "navigator tree omitted security policy path");
  const auto security_configuration_index = RequireRowIndex(
      navigator_tree,
      "node_path",
      "CustomerDB/Management/Security/configurations/general_security_configuration",
      "navigator tree omitted security configuration path");
  Require(security_user_index < security_user_groups_index &&
              security_user_groups_index < security_user_group_membership_index &&
              security_user_index < security_user_role_membership_index &&
              security_user_index < security_user_grant_index &&
              security_group_index < security_group_user_index &&
              security_group_index < security_group_role_index &&
              security_group_index < security_group_grants_index &&
              security_role_index < security_role_users_index &&
              security_role_users_index < security_role_user_index &&
              security_role_index < security_role_group_index &&
              security_policy_index < security_configuration_index,
          "navigator tree did not parent security management paths correctly");
  const auto db_triggers_index = RequireRowIndex(navigator_tree,
                                                "node_path",
                                                "CustomerDB/Management/Triggers",
                                                "navigator tree omitted database triggers path");
  const auto filespaces_index = RequireRowIndex(navigator_tree,
                                               "node_path",
                                               "CustomerDB/Management/File-spaces",
                                               "navigator tree omitted filespaces path");
  const auto app_index = RequireRowIndex(navigator_tree,
                                        "node_path",
                                        "CustomerDB/app",
                                        "navigator tree omitted app schema path");
  Require(database_index < management_index && management_index < db_triggers_index &&
              db_triggers_index < filespaces_index &&
              filespaces_index < app_index,
          "navigator tree did not emit database folders in standard order");

  const auto sys_index = RequireRowIndex(navigator_tree,
                                        "node_path",
                                        "CustomerDB/sys",
                                        "navigator tree omitted sys physical root path");
  const auto sys_catalog_index = RequireRowIndex(navigator_tree,
                                                "node_path",
                                                "CustomerDB/sys/catalog",
                                                "navigator tree omitted sys catalog physical child path");
  const auto sys_catalog_table_index = RequireRowIndex(
      navigator_tree,
      "node_path",
      "CustomerDB/sys/catalog/object_identity",
      "navigator tree omitted sys catalog physical table path");
  const auto sys_catalog_readable_index = RequireRowIndex(
      navigator_tree,
      "node_path",
      "CustomerDB/sys/catalog_readable",
      "navigator tree omitted sys catalog_readable physical child path");
  const auto sys_catalog_readable_view_index = RequireRowIndex(
      navigator_tree,
      "node_path",
      "CustomerDB/sys/catalog_readable/navigator_tree",
      "navigator tree omitted sys catalog_readable physical view path");
  Require(sys_index < sys_catalog_index && sys_catalog_index < sys_catalog_table_index &&
              sys_index < sys_catalog_readable_index &&
              sys_catalog_readable_index < sys_catalog_readable_view_index,
          "navigator tree did not parent sys physical children directly");
  Require(!RowContainsFieldValue(navigator_tree, "node_path", "CustomerDB/sys/Schemas"),
          "navigator tree grouped physical sys children under a pseudo Schemas folder");
  Require(!RowContainsFieldValue(navigator_tree, "node_path", "CustomerDB/app/Schemas"),
          "navigator tree grouped physical app children under a pseudo Schemas folder");

  const auto reporting_index = RequireRowIndex(navigator_tree,
                                              "node_path",
                                              "CustomerDB/app/reporting",
                                              "navigator tree omitted direct child schema path");
  const auto table_index = RequireRowIndex(navigator_tree,
                                          "node_path",
                                          "CustomerDB/app/customers",
                                          "navigator tree omitted direct table path");
  const auto view_index = RequireRowIndex(navigator_tree,
                                         "node_path",
                                         "CustomerDB/app/commandes_ouvertes",
                                         "navigator tree omitted direct view path");
  Require(!RowContainsFieldValue(navigator_tree,
                                 "node_path",
                                 "CustomerDB/app/Tables/customers/Columns"),
          "navigator tree grouped physical table children under a pseudo Tables folder");
  Require(app_index < reporting_index && reporting_index < table_index &&
              table_index < view_index,
          "navigator tree did not emit direct physical schema children in standard order");

  const auto programmability_index = RequireRowIndex(navigator_tree,
                                                    "node_path",
                                                    "CustomerDB/Management/Programmability",
                                                    "navigator tree omitted root programmability path");
  const auto programmability_procedures_index = RequireRowIndex(
      navigator_tree,
      "node_path",
      "CustomerDB/Management/Programmability/procedures",
      "navigator tree omitted root programmability procedures group");
  const auto& programmability = navigator_tree.rows[programmability_procedures_index];
  const std::string programmability_node_id = Field(programmability, "node_id");
  const auto procedure_index = RequireRowIndex(navigator_tree,
                                              "node_path",
                                              "CustomerDB/Management/Programmability/procedures/refresh_customers",
                                              "navigator tree omitted projected procedure path");
  const auto programmability_functions_index = RequireRowIndex(
      navigator_tree,
      "node_path",
      "CustomerDB/Management/Programmability/functions",
      "navigator tree omitted root programmability functions group");
  const auto function_index = RequireRowIndex(navigator_tree,
                                             "node_path",
                                             "CustomerDB/Management/Programmability/functions/customer_score",
                                             "navigator tree omitted projected function path");
  const auto programmability_packages_index = RequireRowIndex(
      navigator_tree,
      "node_path",
      "CustomerDB/Management/Programmability/packages",
      "navigator tree omitted root programmability packages group");
  const auto package_index = RequireRowIndex(navigator_tree,
                                            "node_path",
                                            "CustomerDB/Management/Programmability/packages/customer_admin",
                                            "navigator tree omitted projected package path");
  const auto programmability_sequences_index = RequireRowIndex(
      navigator_tree,
      "node_path",
      "CustomerDB/Management/Programmability/sequences",
      "navigator tree omitted root programmability sequences group");
  const auto sequence_index = RequireRowIndex(navigator_tree,
                                             "node_path",
                                             "CustomerDB/Management/Programmability/sequences/customer_id_seq",
                                             "navigator tree omitted projected sequence path");
  Require(Field(navigator_tree.rows[procedure_index], "parent_node_id") == programmability_node_id,
          "navigator tree did not parent procedure under projected programmability procedures");
  Require(programmability_index < programmability_procedures_index &&
              programmability_procedures_index < procedure_index &&
              procedure_index < programmability_functions_index &&
              programmability_functions_index < function_index &&
              function_index < programmability_packages_index &&
              programmability_packages_index < package_index &&
              package_index < programmability_sequences_index &&
              programmability_sequences_index < sequence_index,
          "navigator tree did not order projected programmability groups and children");
  RequireNoUuidColumnsOrValues(navigator_tree);

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

  const auto configuration_settings = info::BuildSysInformationProjection(
      "sys.configuration.settings",
      Context(),
      {},
      {},
      {},
      {},
      {},
      Settings());
  RequireOk(configuration_settings, "sys.configuration settings projection failed");
  Require(RowContainsFieldValue(configuration_settings, "setting_name", "metadata.visibility.strict_mode"),
          "sys.configuration settings did not project setting name");
  Require(RowContainsFieldValue(configuration_settings, "effective_state", "security_redacted"),
          "sys.configuration settings did not project effective state");

  const auto configuration_effective_settings = info::BuildSysInformationProjection(
      "sys.configuration.effective_settings",
      Context(),
      {},
      {},
      {},
      {},
      {},
      Settings());
  RequireOk(configuration_effective_settings, "sys.configuration effective settings projection failed");
  Require(RowContainsFieldValue(configuration_effective_settings, "resolved_from", "engine:runtime_secret"),
          "sys.configuration effective settings did not project resolution source");

  const auto configuration_profiles = info::BuildSysInformationProjection(
      "sys.configuration.profiles",
      Context(),
      {},
      {});
  RequireOk(configuration_profiles, "sys.configuration profiles projection failed");

  const auto configuration_policy_bindings = info::BuildSysInformationProjection(
      "sys.configuration.policy_bindings",
      Context(),
      {},
      {},
      {},
      {},
      {},
      {},
      {},
      {},
      {},
      {},
      {},
      AgentPolicies());
  RequireOk(configuration_policy_bindings, "sys.configuration policy bindings projection failed");
  Require(RowContainsFieldValue(configuration_policy_bindings, "policy_uuid", "policy-default-memory"),
          "sys.configuration policy bindings did not project policy UUID");
  Require(RowContainsFieldValue(configuration_policy_bindings, "binding_state", "active"),
          "sys.configuration policy bindings did not project binding state");

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

void TestParserDialectProjectionRows() {
  const auto dialects = info::BuildSysInformationProjection(
      "sys.parser.dialects",
      Context(),
      {},
      {});
  RequireOk(dialects, "parser dialect projection failed");
  Require(RowContainsFieldValue(dialects, "dialect_name", "SBsql"),
          "parser dialect projection omitted SBsql");
  Require(RowContainsFieldValue(dialects, "compatibility_state", "supported"),
          "parser dialect projection did not advertise supported SBsql state");
  RequireNoUuidColumnsOrValues(dialects);
}

void TestSessionRoleProjectionRows() {
  auto context = Context();
  context.principal_name = "alice";
  context.principal_uuid = "principal-alice";
  context.requested_role_name = "sysarch";
  context.active_role_name = "sysarch";
  context.active_role_uuid = "role-sysarch";
  context.effective_role_names.push_back("sysarch");
  context.effective_role_uuids.push_back("role-sysarch");
  context.effective_group_uuids.push_back("group-public");

  const auto enabled_roles = info::BuildSysInformationProjection(
      "sys.information.enabled_roles",
      context,
      {},
      {});
  RequireOk(enabled_roles, "enabled roles projection failed");
  Require(RowContainsFieldValue(enabled_roles, "role_name", "sysarch"),
          "enabled roles projection omitted active sysarch role");
  Require(RowContainsFieldValue(enabled_roles, "is_default", "YES"),
          "enabled roles projection did not mark active role as default");
  Require(RowContainsFieldValue(enabled_roles, "enabled_by", "requested_role"),
          "enabled roles projection did not record requested role source");
  RequireNoUuidColumnsOrValues(enabled_roles);

  const auto applicable_roles = info::BuildSysInformationProjection(
      "sys.information.applicable_roles",
      context,
      {},
      {});
  RequireOk(applicable_roles, "applicable roles projection failed");
  Require(RowContainsFieldValue(applicable_roles, "grantee", "alice"),
          "applicable roles projection omitted effective user");
  Require(RowContainsFieldValue(applicable_roles, "role_name", "sysarch"),
          "applicable roles projection omitted sysarch membership");
  Require(RowContainsFieldValue(applicable_roles, "is_grantable", "NO"),
          "applicable roles projection did not mark membership non-grantable");
  RequireNoUuidColumnsOrValues(applicable_roles);
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
  TestParserDialectProjectionRows();
  TestSessionRoleProjectionRows();
  TestMissingNameStrictModeFailsClosed();
  TestClusterAndUnsupportedPathsFailClosed();
  return EXIT_SUCCESS;
}
