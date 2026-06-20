// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/sys_information_projection.hpp"

#include "catalog_index_profile.hpp"
#include "catalog/sbsql_language_elements_catalog.hpp"
#include "datatype_descriptor.hpp"
#include "datatype_wire_metadata.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

using scratchbird::core::catalog::BuiltinCatalogIndexProfiles;
using scratchbird::core::catalog::BuiltinCatalogTableProfiles;
using scratchbird::core::catalog::CatalogPathIsClusterScoped;
using scratchbird::core::catalog::CatalogIndexMethodName;
using scratchbird::core::catalog::CatalogIndexProfileHasOrderedNeed;
using scratchbird::core::catalog::CatalogIndexPurposeName;
using scratchbird::core::catalog::CatalogTableProfile;
namespace dt = scratchbird::core::datatypes;

SysInformationProjectionColumn Column(std::string column_name,
                                      std::string logical_type,
                                      bool nullable = false,
                                      bool resolver_backed_name = false,
                                      bool comment_backed_text = false,
                                      bool exposes_internal_uuid = false,
                                      bool scratchbird_extension = false) {
  SysInformationProjectionColumn column;
  column.column_name = std::move(column_name);
  column.logical_type = std::move(logical_type);
  column.nullable = nullable;
  column.resolver_backed_name = resolver_backed_name;
  column.comment_backed_text = comment_backed_text;
  column.exposes_internal_uuid = exposes_internal_uuid;
  column.scratchbird_extension = scratchbird_extension;
  return column;
}

SysInformationProjectionColumn ExplicitUuidColumn(std::string column_name,
                                                  bool nullable = false) {
  return Column(std::move(column_name),
                "uuid",
                nullable,
                false,
                false,
                true,
                false);
}

SysInformationProjectionDefinition Definition(
    std::string view_path,
    SysInformationProjectionFamily family,
    std::vector<SysInformationProjectionColumn> columns,
    std::vector<SysInformationSourceKind> source_kinds,
    bool resolver_join_required = true,
    bool comment_join_required = false,
    std::string ui_area = {},
    std::vector<std::string> key_columns = {},
    std::string description = {}) {
  SysInformationProjectionDefinition definition;
  definition.view_path = std::move(view_path);
  definition.view_scope = "local";
  definition.ui_area = std::move(ui_area);
  definition.key_columns = std::move(key_columns);
  definition.description = std::move(description);
  definition.family = family;
  definition.columns = std::move(columns);
  definition.source_kinds = std::move(source_kinds);
  definition.resolver_join_required = resolver_join_required;
  definition.comment_join_required = comment_join_required;
  definition.authorization_filter_required = true;
  definition.redaction_required = true;
  definition.language_fallback_required = resolver_join_required || comment_join_required;
  definition.mga_snapshot_visibility_required = true;
  definition.exposes_internal_uuid = false;
  definition.cluster_path_fail_closed = true;
  return definition;
}

void AddDiagnostic(SysInformationProjectionValidationResult* result, std::string code) {
  result->ok = false;
  result->diagnostic_codes.push_back(std::move(code));
}

SysInformationProjectionResult Failure(std::string code, std::string detail) {
  SysInformationProjectionResult result;
  result.ok = false;
  result.diagnostic_code = std::move(code);
  result.diagnostic_detail = std::move(detail);
  return result;
}

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool EqualsInsensitiveAscii(std::string_view left, std::string_view right) {
  if (left.size() != right.size()) { return false; }
  for (std::size_t i = 0; i < left.size(); ++i) {
    char l = left[i];
    char r = right[i];
    if (l >= 'A' && l <= 'Z') { l = static_cast<char>(l - 'A' + 'a'); }
    if (r >= 'A' && r <= 'Z') { r = static_cast<char>(r - 'A' + 'a'); }
    if (l != r) { return false; }
  }
  return true;
}

std::vector<std::string> SplitSemicolonList(std::string_view value) {
  std::vector<std::string> out;
  while (!value.empty()) {
    const std::size_t pos = value.find(';');
    const std::string_view token = pos == std::string_view::npos ? value : value.substr(0, pos);
    if (!token.empty()) { out.emplace_back(token); }
    if (pos == std::string_view::npos) { break; }
    value.remove_prefix(pos + 1);
  }
  return out;
}

bool ContainsToken(std::string_view value, std::string_view token) {
  return value.find(token) != std::string_view::npos;
}

std::string ProjectionToken(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  bool previous_underscore = false;
  for (const unsigned char ch : value) {
    char next = '_';
    if (std::isalnum(ch)) {
      next = static_cast<char>(std::tolower(ch));
    }
    if (next == '_') {
      if (previous_underscore) {
        continue;
      }
      previous_underscore = true;
    } else {
      previous_underscore = false;
    }
    out.push_back(next);
  }
  while (!out.empty() && out.back() == '_') {
    out.pop_back();
  }
  return out.empty() ? "unspecified" : out;
}

void AddUniqueText(std::vector<std::string>* values, std::string value) {
  if (values == nullptr || value.empty()) { return; }
  if (std::find(values->begin(), values->end(), value) == values->end()) {
    values->push_back(std::move(value));
  }
}

std::vector<std::string> EffectiveRoleUuids(const SysInformationProjectionContext& context) {
  std::vector<std::string> roles;
  for (const auto& role_uuid : context.effective_role_uuids) {
    AddUniqueText(&roles, role_uuid);
  }
  AddUniqueText(&roles, context.active_role_uuid);
  return roles;
}

std::string RoleDisplayNameAt(const SysInformationProjectionContext& context,
                              std::size_t index,
                              std::string_view role_uuid) {
  if (!context.active_role_uuid.empty() &&
      EqualsInsensitiveAscii(context.active_role_uuid, role_uuid)) {
    if (!context.active_role_name.empty()) { return context.active_role_name; }
    if (!context.requested_role_name.empty()) { return context.requested_role_name; }
  }
  if (index < context.effective_role_names.size() &&
      !context.effective_role_names[index].empty()) {
    return context.effective_role_names[index];
  }
  return index == 0 ? "active_role" : "effective_role_" + std::to_string(index + 1);
}

std::string PrincipalDisplayName(const SysInformationProjectionContext& context) {
  if (!context.principal_name.empty()) { return context.principal_name; }
  return "current_user";
}

std::string LogicalTypeForProjectionColumn(std::string_view column_name) {
  if (column_name == "ordinal_position" || column_name == "version_number" ||
      column_name == "active_version_number" || column_name == "port" ||
      column_name == "blocker_count" || column_name == "scan_generation" ||
      column_name == "integer_value" || column_name == "truncate_ready_bytes" ||
      column_name == "supported_value" || column_name == "increment" ||
      column_name == "start_value" || column_name == "safe_start_byte" ||
      column_name == "safe_end_byte" || column_name == "depth" ||
      column_name == "sort_group" || column_name == "sort_ordinal") {
    return "uint64";
  }
  if (ContainsToken(column_name, "count") || ContainsToken(column_name, "bytes") ||
      ContainsToken(column_name, "epoch") || ContainsToken(column_name, "generation")) {
    return "uint64";
  }
  if (column_name == "enabled" || column_name == "grantable" ||
      column_name == "is_grantable" || column_name == "is_supported" ||
      column_name == "is_default" || column_name == "purged" ||
      column_name == "approval_required" || column_name == "cycle_option" ||
      column_name == "is_virtual" || column_name == "is_expandable") {
    return "yes_no";
  }
  return "text";
}

SysInformationProjectionFamily PacketFamily(std::string_view family) {
  if (family == "information_schema") {
    return SysInformationProjectionFamily::standard_information_schema;
  }
  if (family == "information_extension") {
    return SysInformationProjectionFamily::scratchbird_extension;
  }
  if (family == "catalog_readable") {
    return SysInformationProjectionFamily::catalog_readable;
  }
  return SysInformationProjectionFamily::frontend_projection;
}

std::vector<SysInformationProjectionColumn> PacketColumns(std::string_view key_columns) {
  std::vector<SysInformationProjectionColumn> columns;
  for (const auto& column_name : SplitSemicolonList(key_columns)) {
    const bool resolver_backed = ContainsToken(column_name, "name") ||
                                 ContainsToken(column_name, "path") ||
                                 ContainsToken(column_name, "schema");
    const bool comment_backed = ContainsToken(column_name, "comment");
    columns.push_back(Column(column_name,
                             LogicalTypeForProjectionColumn(column_name),
                             true,
                             resolver_backed,
                             comment_backed,
                             false,
                             true));
  }
  return columns;
}

bool ColumnNameExposesUuidByConvention(std::string_view column_name) {
  static constexpr std::string_view kToken = "_uuid";
  if (column_name == "uuid") { return true; }
  if (column_name.size() >= kToken.size() &&
      column_name.substr(column_name.size() - kToken.size()) == kToken) {
    return true;
  }
  return column_name.find("uuid_") != std::string_view::npos;
}

std::vector<SysInformationProjectionColumn> CatalogTableColumns(
    const CatalogTableProfile& table) {
  std::vector<SysInformationProjectionColumn> columns;
  for (const auto& column_profile : table.columns) {
    const bool exposes_uuid =
        ColumnNameExposesUuidByConvention(column_profile.column_name);
    columns.push_back(Column(column_profile.column_name,
                             exposes_uuid ? "uuid" : LogicalTypeForProjectionColumn(column_profile.column_name),
                             column_profile.nullable,
                             false,
                             false,
                             exposes_uuid,
                             false));
  }
  return columns;
}

std::vector<std::string> CatalogTableKeyColumns(const CatalogTableProfile& table) {
  std::vector<std::string> keys;
  for (const auto& column_profile : table.columns) {
    if (keys.size() >= 4) { break; }
    keys.push_back(column_profile.column_name);
  }
  return keys;
}

struct ProjectionPacketRow {
  std::string view_path;
  std::string family;
  std::string ui_area;
  std::string key_columns;
  std::string description;
};

std::vector<std::string> SplitCsvLine(std::string_view line) {
  std::vector<std::string> fields;
  while (true) {
    const std::size_t pos = line.find(',');
    fields.emplace_back(pos == std::string_view::npos ? line : line.substr(0, pos));
    if (pos == std::string_view::npos) { break; }
    line.remove_prefix(pos + 1);
  }
  return fields;
}

const std::vector<ProjectionPacketRow>& LocalFrontendProjectionPacketRows() {
  static const std::vector<ProjectionPacketRow> rows = [] {
    static constexpr std::string_view kRows = R"CSV(sys.catalog_readable.objects,catalog_readable,object_browser,object_path;object_name;object_kind;parent_path;status;comment_text;visibility_state,Primary ScratchBird-native object tree view.
sys.catalog_readable.object_tree,catalog_readable,object_browser,object_id;parent_object_id;object_path;object_name;object_kind;parent_path;depth;schema_path;status;visibility_state,Authorization-filtered parent-child tree for drivers and tools.
sys.catalog_readable.navigator_tree,catalog_readable,object_browser,node_id;parent_node_id;object_id;parent_object_id;node_path;node_name;node_kind;node_role;object_kind;object_path;schema_path;parent_path;depth;sort_group;sort_ordinal;is_virtual;is_expandable;status;visibility_state,Authorization-filtered typed navigator tree with standard UI folders for drivers and tools.
sys.catalog_readable.object_names,catalog_readable,names_aliases,object_path;object_name;name_language;visibility_state,No UUID columns; names come from centralized resolver.
sys.catalog_readable.object_comments,catalog_readable,comments,object_path;object_name;comment_text;comment_language;visibility_state,No UUID columns; comments come from centralized comment resolver.
sys.catalog_readable.schemas,catalog_readable,schema_browser,schema_path;schema_name;parent_path;comment_text;visibility_state,Local schemas and subschemas.
sys.catalog_readable.relations,catalog_readable,relation_browser,relation_path;relation_name;relation_kind;schema_path;status;comment_text;visibility_state,Tables; views; materialized views; virtual views; relation-like objects.
sys.catalog_readable.columns,catalog_readable,column_grid,relation_path;column_name;ordinal_position;datatype_name;domain_name;is_nullable;comment_text;visibility_state,Preferred rich column metadata source.
sys.catalog_readable.constraints,catalog_readable,constraints,relation_path;constraint_name;constraint_kind;referenced_object_path;status;visibility_state,Constraint browser without UUID exposure.
sys.catalog_readable.indexes,catalog_readable,indexes,index_path;index_name;relation_path;index_family;storage_profile_name;comment_text;visibility_state,Index browser.
sys.catalog_readable.index_columns,catalog_readable,index_columns,index_path;ordinal_position;key_expression;collation_name;operator_class_name;direction,Ordered index keys and expressions.
sys.catalog_readable.datatypes,catalog_readable,datatypes,type_path;type_name;type_family;storage_class;wire_class;status;visibility_state,Canonical datatype descriptors.
sys.catalog_readable.domains,catalog_readable,domains,domain_path;domain_name;base_type_name;domain_kind;status;comment_text;visibility_state,Domains and reference-emulation domains.
sys.catalog_readable.domain_elements,catalog_readable,domain_elements,domain_path;element_name;ordinal_position;element_type_name;visibility_state,Compound domain element display.
sys.catalog_readable.casts,catalog_readable,casts,source_type_name;target_type_name;cast_class;lossiness;status;visibility_state,Cast rule browser.
sys.catalog_readable.operations,catalog_readable,functions_operators,operation_path;operation_name;operation_kind;argument_display;result_type_name;status;visibility_state,Functions; operators; aggregates; windows; domain operations.
sys.catalog_readable.procedures,catalog_readable,procedures,procedure_path;procedure_name;result_shape;status;comment_text;visibility_state,Procedure metadata.
sys.catalog_readable.triggers,catalog_readable,triggers,trigger_path;target_path;timing;event;status;visibility_state,Trigger metadata.
sys.catalog_readable.packages,catalog_readable,packages,package_path;package_name;package_kind;status;comment_text;visibility_state,Package/module metadata.
sys.catalog_readable.udrs,catalog_readable,udr_packages,udr_path;udr_name;language;signature;policy_status;comment_text;visibility_state,C++ UDR package metadata.
sys.catalog_readable.security_subjects,catalog_readable,security,subject_path;subject_name;subject_kind;status;visibility_state,Users; roles; groups; service identities.
sys.catalog_readable.privileges,catalog_readable,privileges,subject_path;object_path;privilege_name;grant_state;grantable;visibility_state,Effective visible privileges.
sys.catalog_readable.policies,catalog_readable,policies,policy_path;policy_name;policy_kind;status;comment_text;visibility_state,Policy browser.
sys.security.roles,frontend_projection,security,role_name;status;visibility_state,Frontend-safe role projection.
sys.security.principals,frontend_projection,security,principal_name;principal_kind;status;visibility_state,Frontend-safe users groups roles and service identities projection.
sys.security.policies,frontend_projection,security,policy_name;policy_kind;status;visibility_state,Frontend-safe security policy projection.
sys.security.masks,frontend_projection,security,mask_name;target_path;status;visibility_state,Frontend-safe column mask projection.
sys.security.rls,frontend_projection,security,rls_name;target_path;status;visibility_state,Frontend-safe row-level-security projection.
sys.catalog_readable.filespaces,catalog_readable,storage,filespace_path;filespace_name;role;status;size_metrics;comment_text;visibility_state,Device paths redacted by policy.
sys.catalog_readable.page_families,catalog_readable,storage,page_family_name;page_role;layout_version;status;visibility_state,Page family metadata.
sys.catalog_readable.resources,catalog_readable,resources,resource_path;resource_name;resource_kind;version;status;visibility_state,Timezone; charset; collation; locale; parser; reference resources.
sys.catalog_readable.parser_profiles,catalog_readable,parser_profiles,profile_path;profile_name;parser_family;dialect;cache_policy;resource_epoch;visibility_state,Parser package and dialect profile metadata.
sys.catalog_readable.listeners,catalog_readable,listeners,listener_path;listener_name;network;port;parser_family;status;visibility_state,Listener profiles and parser bindings.
sys.catalog_readable.metrics_catalog,catalog_readable,metrics,metric_name;scope;units;type;retention_policy;redaction_class,Metric catalog metadata.
sys.catalog_readable.diagnostics_catalog,catalog_readable,diagnostics,diagnostic_name;category;severity;redaction_class;support_action,Diagnostic catalog metadata.
sys.catalog_readable.settings,catalog_readable,settings,setting_name;setting_value_display;authority;default_source;redaction_state;visibility_state,Secret values redacted by policy.
sys.catalog_readable.jobs,catalog_readable,jobs,job_path;job_name;job_kind;status;policy_name;metrics_summary;visibility_state,Jobs and maintenance tasks.
sys.catalog_readable.remote_connections,catalog_readable,remote_connections,connection_path;connection_name;connection_kind;endpoint_display;credential_state;visibility_state,Credentials redacted.
sys.catalog_readable.emulation_profiles,catalog_readable,emulation_profiles,profile_path;profile_name;reference_family;parser_binding;udr_binding;status;visibility_state,Reference/emulation profile metadata.
sys.parser.dialects,frontend_projection,parser_profiles,dialect_name;base_dialect;compatibility_state;parser_family,Frontend-safe parser dialect inventory for drivers and management tools.
sys.parser.language_elements,frontend_projection,parser_profiles,surface_id;canonical_name;element_kind;surface_kind;family;sblr_operation_family;support_state;release_status;predictive_state;keyword_text;keyword_class,Frontend-safe SBSQL language element manifest for drivers; predictive text; and conformance tests.
sys.information.schemata,information_schema,driver_metadata,catalog_name;schema_name;schema_owner;sql_path,Standard-compatible schema metadata.
sys.information.tables,information_schema,driver_metadata,table_catalog;table_schema;table_name;table_type;is_insertable_into,Standard-compatible table metadata.
sys.information.views,information_schema,driver_metadata,table_catalog;table_schema;table_name;view_definition,View metadata redacted by policy.
sys.information.columns,information_schema,driver_metadata,table_schema;table_name;column_name;ordinal_position;data_type;is_nullable;column_default,Standard-compatible column metadata.
sys.information.domains,information_schema,driver_metadata,domain_schema;domain_name;data_type,Standard-compatible domain metadata.
sys.information.domain_constraints,information_schema,driver_metadata,constraint_schema;constraint_name;domain_schema;domain_name,Domain constraint metadata.
sys.information.check_constraints,information_schema,driver_metadata,constraint_schema;constraint_name;check_clause,Check constraint metadata with redacted clauses by policy.
sys.information.table_constraints,information_schema,driver_metadata,constraint_schema;constraint_name;table_schema;table_name;constraint_type,Table constraint metadata.
sys.information.key_column_usage,information_schema,driver_metadata,constraint_schema;constraint_name;table_schema;table_name;column_name;ordinal_position,Key column participation.
sys.information.referential_constraints,information_schema,driver_metadata,constraint_schema;constraint_name;unique_constraint_schema;unique_constraint_name;update_rule;delete_rule,Foreign key relationship metadata.
sys.information.constraint_column_usage,information_schema,driver_metadata,constraint_schema;constraint_name;table_schema;table_name;column_name,Constraint column usage metadata.
sys.information.constraint_table_usage,information_schema,driver_metadata,constraint_schema;constraint_name;table_schema;table_name,Constraint table usage metadata.
sys.information.routines,information_schema,driver_metadata,routine_schema;routine_name;routine_type;data_type,Routine metadata.
sys.information.parameters,information_schema,driver_metadata,specific_schema;specific_name;parameter_name;ordinal_position;parameter_mode;data_type,Routine parameter metadata.
sys.information.sequences,information_schema,driver_metadata,sequence_schema;sequence_name;data_type;start_value;increment;cycle_option,Sequence generator and identity-provider metadata.
sys.information.triggers,information_schema,driver_metadata,trigger_schema;trigger_name;event_object_table;action_timing;event_manipulation,Trigger metadata.
sys.information.character_sets,information_schema,driver_metadata,character_set_schema;character_set_name;default_collate_name,Character set resource metadata.
sys.information.collations,information_schema,driver_metadata,collation_schema;collation_name;character_set_name,Collation resource metadata.
sys.information.collation_character_set_applicability,information_schema,driver_metadata,collation_schema;collation_name;character_set_schema;character_set_name,Collation to character set applicability.
sys.information.table_privileges,information_schema,privileges,grantor;grantee;table_schema;table_name;privilege_type;is_grantable,Visible table privileges.
sys.information.column_privileges,information_schema,privileges,grantor;grantee;table_schema;table_name;column_name;privilege_type;is_grantable,Visible column privileges.
sys.information.routine_privileges,information_schema,privileges,grantor;grantee;routine_schema;routine_name;privilege_type;is_grantable,Visible routine privileges.
sys.information.usage_privileges,information_schema,privileges,grantor;grantee;object_schema;object_name;object_type;privilege_type,Visible usage privileges.
sys.information.role_table_grants,information_schema,privileges,grantor;grantee;role_name;table_schema;table_name;privilege_type,Role-mediated table grants.
sys.information.enabled_roles,information_schema,security,role_name;is_default;enabled_by,Session-enabled roles.
sys.information.applicable_roles,information_schema,security,grantee;role_name;is_grantable,Roles applicable to the effective user.
sys.information.sql_features,information_schema,compatibility,feature_id;feature_name;is_supported,Active profile feature flags.
sys.information.sql_implementation_info,information_schema,compatibility,implementation_info_id;implementation_info_name;integer_value;character_value,Safe implementation metadata.
sys.information.sql_parts,information_schema,compatibility,feature_id;feature_name;is_supported,SQL parts supported by the active profile.
sys.information.sql_sizing,information_schema,compatibility,sizing_id;sizing_name;supported_value,Size limits and implementation bounds.
sys.information.scratchbird_object_comments,information_extension,comments,object_path;object_name;comment_text;comment_language,ScratchBird extension view.
sys.information.scratchbird_object_paths,information_extension,object_paths,object_path;object_name;object_kind,ScratchBird extension view.
sys.information.scratchbird_datatype_descriptors,information_extension,datatypes,type_catalog;type_schema;type_name;canonical_type_family;driver_family;support_state,Driver-safe datatype descriptor metadata.
sys.information.scratchbird_domain_elements,information_extension,domain_elements,domain_name;element_name;ordinal_position;element_type_name,Domain element extension metadata.
sys.information.scratchbird_index_profiles,information_extension,indexes,index_name;index_family;support_state,ScratchBird extension index profile metadata.
sys.information.scratchbird_filespaces,information_extension,storage,filespace_name;role;status,Filespace extension metadata.
sys.information.scratchbird_page_families,information_extension,storage,page_family_name;page_role;layout_version;status,Page family extension metadata.
sys.information.scratchbird_policies,information_extension,policies,policy_name;policy_class;status,Policy extension metadata.
sys.information.scratchbird_parser_profiles,information_extension,parser_profiles,profile_name;parser_family;dialect;status,Parser profile extension metadata.
sys.information.scratchbird_metrics_catalog,information_extension,metrics,metric_name;units;type;retention_policy;redaction_class,Metric catalog extension.
sys.information.scratchbird_diagnostics_catalog,information_extension,diagnostics,diagnostic_name;severity;category;redaction_class;operator_action,Diagnostic catalog extension.
sys.information.scratchbird_finality_modes,information_extension,transactions,finality_label;visibility_meaning,Finality label metadata.
sys.information.scratchbird_read_replicas,information_extension,replication,replica_name;source_path;state;lag_display;visibility_state,Read replica display metadata.
sys.information.scratchbird_cache_profiles,information_extension,cache,cache_profile_name;cache_class;invalidation_behavior;status,Cache profile metadata.
sys.information.scratchbird_protected_material,information_extension,protected_material,material_path;material_name;purpose_class;lifecycle_state;visibility_state,Redacted protected material metadata.
sys.information.scratchbird_protected_material_versions,information_extension,protected_material,material_path;material_name;version_number;rotation_state;retention_state;purged,Redacted protected material version metadata.
sys.frontend.agents,frontend_projection,agents,agent_name;agent_type_id;scope_kind;state;health_state;enabled;policy_name,Frontend-safe projection over sys.agents that resolves or hides UUID authority fields.
sys.frontend.agent_metric_dependencies,frontend_projection,agents,agent_name;metric_family;namespace;quality_state;fail_behavior,Frontend-safe projection over sys.agent_metric_dependencies.
sys.frontend.agent_policies,frontend_projection,agents,agent_name;policy_name;policy_family;active_state;validation_state,Frontend-safe projection over sys.agent_policies.
sys.frontend.agent_actions,frontend_projection,agents,action_ref;agent_name;action_id;state;risk_class;approval_required,Frontend-safe projection over sys.agent_actions.
sys.frontend.agent_overrides,frontend_projection,agents,override_ref;target_name;scope_path;suppression_class;state,Frontend-safe projection over sys.agent_overrides.
sys.frontend.agent_evidence,frontend_projection,agents,evidence_ref;agent_name;evidence_type;redaction_class;created_at,Frontend-safe projection over sys.agent_evidence.
sys.frontend.agent_audit,frontend_projection,agents,audit_ref;evidence_ref;actor_name;command_name;result_state;created_at,Frontend-safe projection over sys.agent_audit.
sys.frontend.filespace_capacity_agent_state,frontend_projection,storage_agents,agent_name;filespace_name;policy_name;mode;last_recommendation_code,Frontend-safe projection over filespace capacity agent state.
sys.frontend.page_allocation_agent_state,frontend_projection,storage_agents,agent_name;filespace_name;page_family;page_type;mode;last_shrink_ready_state,Frontend-safe projection over page allocation agent state.
sys.frontend.filespace_shrink_readiness,frontend_projection,storage_agents,filespace_name;truncate_ready_bytes;blocker_count;readiness_state;scan_generation,Frontend-safe projection over filespace shrink readiness.)CSV";
    std::vector<ProjectionPacketRow> out;
    std::istringstream stream{std::string(kRows)};
    std::string line;
    while (std::getline(stream, line)) {
      if (line.empty()) { continue; }
      const auto fields = SplitCsvLine(line);
      if (fields.size() == 5) {
        out.push_back({fields[0], fields[1], fields[2], fields[3], fields[4]});
      }
    }
    return out;
  }();
  return rows;
}

bool ProjectionSourceVisible(bool hidden,
                             std::uint64_t catalog_generation_id,
                             const SysInformationProjectionContext& context) {
  if (hidden) { return false; }
  if (context.visible_catalog_generation_id != 0 &&
      catalog_generation_id > context.visible_catalog_generation_id) {
    return false;
  }
  return true;
}

bool HasTemporaryVisibilityDescriptor(const SysInformationCatalogObjectSource& object) {
  return object.temporary || !object.temporary_scope.empty() ||
         !object.temporary_session_uuid.empty() ||
         !object.on_commit_action.empty();
}

bool TemporaryMetadataVisible(const SysInformationCatalogObjectSource& object,
                              const SysInformationProjectionContext& context) {
  if (!HasTemporaryVisibilityDescriptor(object)) { return true; }
  if (!object.temporary) { return false; }
  if (object.temporary_scope == "global") { return true; }
  if (object.temporary_scope == "private") {
    return !context.session_uuid.empty() &&
           object.temporary_session_uuid == context.session_uuid;
  }
  return false;
}

bool ObjectVisible(const SysInformationCatalogObjectSource& object,
                   const SysInformationProjectionContext& context) {
  if (object.hidden) { return false; }
  if (object.cluster_path && !context.cluster_authority_available) { return false; }
  if (context.visible_catalog_generation_id != 0 &&
      object.catalog_generation_id > context.visible_catalog_generation_id) {
    return false;
  }
  if (object.dropped_local_transaction_id != 0 &&
      context.visible_catalog_generation_id != 0 &&
      object.dropped_local_transaction_id <= context.visible_catalog_generation_id) {
    return false;
  }
  if (!TemporaryMetadataVisible(object, context)) { return false; }
  return true;
}

bool ResolverVisible(const SysInformationResolverNameSource& name,
                     const SysInformationProjectionContext& context) {
  if (name.hidden) { return false; }
  if (context.visible_catalog_generation_id != 0 &&
      name.catalog_generation_id > context.visible_catalog_generation_id) {
    return false;
  }
  return !name.display_name.empty();
}

std::vector<std::string> LanguageOrder(const SysInformationProjectionContext& context) {
  std::vector<std::string> languages;
  auto push = [&languages](std::string value) {
    if (value.empty()) { return; }
    if (std::find(languages.begin(), languages.end(), value) == languages.end()) {
      languages.push_back(std::move(value));
    }
  };
  push(context.session_language);
  push(context.default_language);
  push("und");
  return languages;
}

const SysInformationCatalogObjectSource* FindObject(
    const std::vector<SysInformationCatalogObjectSource>& catalog_objects,
    const std::string& object_uuid,
    const SysInformationProjectionContext& context) {
  for (const auto& object : catalog_objects) {
    if (object.object_uuid == object_uuid && ObjectVisible(object, context)) {
      return &object;
    }
  }
  return nullptr;
}

std::string SelectResolverDisplayName(
    const std::vector<SysInformationResolverNameSource>& resolver_names,
    const SysInformationProjectionContext& context,
    const std::string& object_uuid,
    const std::string& object_class,
    bool* found) {
  *found = false;
  const auto languages = LanguageOrder(context);
  for (const auto& language : languages) {
    for (const auto& name : resolver_names) {
      if (name.object_uuid != object_uuid) { continue; }
      if (!object_class.empty() && name.object_class != object_class) { continue; }
      if (name.language_tag != language) { continue; }
      if (name.name_class != "primary" && name.name_class != "compatibility" &&
          name.name_class != "alias") {
        continue;
      }
      if (!ResolverVisible(name, context)) { continue; }
      *found = true;
      return name.display_name;
    }
  }
  return {};
}

std::string SelectCommentText(const std::vector<SysInformationCommentSource>& comments,
                              const SysInformationProjectionContext& context,
                              const std::string& object_uuid,
                              const std::string& object_class) {
  const auto languages = LanguageOrder(context);
  for (const auto& language : languages) {
    for (const auto& comment : comments) {
      if (comment.object_uuid != object_uuid) { continue; }
      if (!object_class.empty() && comment.object_class != object_class) { continue; }
      if (comment.language_tag != language) { continue; }
      if (comment.hidden) { continue; }
      if (context.visible_catalog_generation_id != 0 &&
          comment.catalog_generation_id > context.visible_catalog_generation_id) {
        continue;
      }
      return comment.comment_text;
    }
  }
  return {};
}

std::string ObjectDisplayName(const std::vector<SysInformationResolverNameSource>& resolver_names,
                              const SysInformationProjectionContext& context,
                              const SysInformationCatalogObjectSource& object,
                              bool* found) {
  return SelectResolverDisplayName(resolver_names,
                                   context,
                                   object.object_uuid,
                                   object.object_class,
                                   found);
}

std::string SchemaDisplayName(const std::vector<SysInformationResolverNameSource>& resolver_names,
                              const SysInformationProjectionContext& context,
                              const SysInformationCatalogObjectSource& object,
                              bool* found) {
  if (object.schema_uuid.empty()) {
    *found = true;
    return {};
  }
  return SelectResolverDisplayName(resolver_names, context, object.schema_uuid, "schema", found);
}

std::string ObjectDisplayPath(const std::vector<SysInformationResolverNameSource>& resolver_names,
                              const SysInformationProjectionContext& context,
                              const SysInformationCatalogObjectSource& object,
                              bool* found) {
  bool found_object = false;
  bool found_schema = false;
  const std::string object_name = ObjectDisplayName(resolver_names, context, object, &found_object);
  const std::string schema_name = SchemaDisplayName(resolver_names, context, object, &found_schema);
  *found = found_object && found_schema;
  if (!*found) { return {}; }
  return schema_name.empty() ? object_name : schema_name + "." + object_name;
}

std::string TreeParentObjectId(const SysInformationCatalogObjectSource& object) {
  if (!object.parent_object_uuid.empty()) { return object.parent_object_uuid; }
  if (object.object_class != "schema") { return object.schema_uuid; }
  return {};
}

bool AppendTreePathParts(const std::vector<SysInformationCatalogObjectSource>& catalog_objects,
                         const std::vector<SysInformationResolverNameSource>& resolver_names,
                         const SysInformationProjectionContext& context,
                         const SysInformationCatalogObjectSource& object,
                         std::vector<std::string>* parts,
                         std::uint32_t depth = 0) {
  if (parts == nullptr || depth > 64) { return false; }
  bool found_name = false;
  const std::string object_name = ObjectDisplayName(resolver_names, context, object, &found_name);
  if (!found_name) { return false; }

  const std::string parent_id = TreeParentObjectId(object);
  if (!parent_id.empty()) {
    const auto* parent = FindObject(catalog_objects, parent_id, context);
    if (parent == nullptr ||
        !AppendTreePathParts(catalog_objects, resolver_names, context, *parent, parts, depth + 1)) {
      return false;
    }
  }
  parts->push_back(object_name);
  return true;
}

std::string JoinPathParts(const std::vector<std::string>& parts) {
  std::string out;
  for (const auto& part : parts) {
    if (!out.empty()) { out.push_back('.'); }
    out += part;
  }
  return out;
}

std::string TreeDisplayPath(const std::vector<SysInformationCatalogObjectSource>& catalog_objects,
                            const std::vector<SysInformationResolverNameSource>& resolver_names,
                            const SysInformationProjectionContext& context,
                            const SysInformationCatalogObjectSource& object,
                            bool* found) {
  std::vector<std::string> parts;
  *found = AppendTreePathParts(catalog_objects, resolver_names, context, object, &parts);
  return *found ? JoinPathParts(parts) : std::string{};
}

std::string TreeParentPath(const std::vector<SysInformationCatalogObjectSource>& catalog_objects,
                           const std::vector<SysInformationResolverNameSource>& resolver_names,
                           const SysInformationProjectionContext& context,
                           const SysInformationCatalogObjectSource& object,
                           bool* found) {
  const std::string parent_id = TreeParentObjectId(object);
  if (parent_id.empty()) {
    *found = true;
    return {};
  }
  const auto* parent = FindObject(catalog_objects, parent_id, context);
  if (parent == nullptr) {
    *found = false;
    return {};
  }
  return TreeDisplayPath(catalog_objects, resolver_names, context, *parent, found);
}

std::uint32_t TreeDepth(const std::vector<SysInformationCatalogObjectSource>& catalog_objects,
                        const SysInformationProjectionContext& context,
                        const SysInformationCatalogObjectSource& object) {
  std::uint32_t depth = 0;
  std::string cursor = TreeParentObjectId(object);
  while (!cursor.empty() && depth < 64) {
    const auto* parent = FindObject(catalog_objects, cursor, context);
    if (parent == nullptr) { break; }
    ++depth;
    cursor = TreeParentObjectId(*parent);
  }
  return depth;
}

void AddField(SysInformationProjectionRow* row, std::string name, std::string value) {
  row->fields.push_back({std::move(name), std::move(value)});
}

std::string TableTypeForObject(const SysInformationCatalogObjectSource& object) {
  if (object.temporary || object.object_class == "temporary_table") {
    if (object.temporary_scope == "global") { return "GLOBAL TEMPORARY"; }
    if (!object.table_type.empty() && ContainsToken(object.table_type, "TEMPORARY")) {
      return object.table_type;
    }
    return "LOCAL TEMPORARY";
  }
  if (!object.table_type.empty()) { return object.table_type; }
  if (object.object_class == "view") { return "VIEW"; }
  if (object.object_class == "materialized_view") { return "MATERIALIZED VIEW"; }
  if (object.object_class == "virtual_table") { return "VIRTUAL TABLE"; }
  return "BASE TABLE";
}

std::string CommitActionForObject(const SysInformationCatalogObjectSource& object) {
  if (!object.temporary && object.object_class != "temporary_table") { return {}; }
  if (object.on_commit_action == "delete_rows") { return "DELETE ROWS"; }
  if (object.on_commit_action == "preserve_rows") { return "PRESERVE ROWS"; }
  return {};
}

bool IsTableLikeObject(const SysInformationCatalogObjectSource& object) {
  return object.object_class == "table" || object.object_class == "view" ||
         object.object_class == "materialized_view" ||
         object.object_class == "temporary_table" ||
         object.object_class == "virtual_table";
}

bool IsSchemaObject(const SysInformationCatalogObjectSource& object) {
  return object.object_class == "schema";
}

bool IsTableNavigatorObject(const SysInformationCatalogObjectSource& object) {
  return object.object_class == "table" ||
         object.object_class == "temporary_table" ||
         object.object_class == "virtual_table" ||
         object.object_class == "materialized_view";
}

bool IsViewNavigatorObject(const SysInformationCatalogObjectSource& object) {
  return object.object_class == "view";
}

bool IsIndexNavigatorObject(const SysInformationCatalogObjectSource& object) {
  return object.object_class == "index";
}

bool IsRoutineNavigatorObject(const SysInformationCatalogObjectSource& object) {
  return object.object_class == "procedure" || object.object_class == "function";
}

bool IsProgrammabilityObject(const SysInformationCatalogObjectSource& object) {
  return object.object_class == "procedure" ||
         object.object_class == "function" ||
         object.object_class == "package" ||
         object.object_class == "sequence";
}

bool IsSecurityUserObject(const SysInformationCatalogObjectSource& object) {
  return object.object_class == "user" || object.object_class == "principal";
}

bool IsSecurityGroupObject(const SysInformationCatalogObjectSource& object) {
  return object.object_class == "group";
}

bool IsSecurityRoleObject(const SysInformationCatalogObjectSource& object) {
  return object.object_class == "role";
}

bool IsSecurityPolicyObject(const SysInformationCatalogObjectSource& object) {
  return object.object_class == "policy" ||
         object.object_class == "security_policy";
}

bool IsSecurityConfigurationObject(const SysInformationCatalogObjectSource& object) {
  return object.object_class == "configuration" ||
         object.object_class == "security_configuration";
}

bool IsSecurityGrantObject(const SysInformationCatalogObjectSource& object) {
  return object.object_class == "grant" ||
         object.object_class == "security_grant";
}

bool IsSecurityUserGroupMembershipObject(const SysInformationCatalogObjectSource& object) {
  return object.object_class == "security_user_group_membership";
}

bool IsSecurityUserRoleMembershipObject(const SysInformationCatalogObjectSource& object) {
  return object.object_class == "security_user_role_membership";
}

bool IsSecurityGroupUserMembershipObject(const SysInformationCatalogObjectSource& object) {
  return object.object_class == "security_group_user_membership";
}

bool IsSecurityGroupRoleMembershipObject(const SysInformationCatalogObjectSource& object) {
  return object.object_class == "security_group_role_membership";
}

bool IsSecurityRoleUserMembershipObject(const SysInformationCatalogObjectSource& object) {
  return object.object_class == "security_role_user_membership";
}

bool IsSecurityRoleGroupMembershipObject(const SysInformationCatalogObjectSource& object) {
  return object.object_class == "security_role_group_membership";
}

std::uint32_t SchemaRootSortGroup(std::string_view path) {
  if (path == "sys") { return 1000; }
  if (path == "users") { return 1100; }
  if (path == "emulated") { return 1200; }
  if (path == "remote") { return 1300; }
  if (path == "app") { return 1400; }
  if (path == "cluster") { return 1500; }
  return 9000;
}

bool IsContextualSchemaRoot(std::string_view path) {
  return path == "cluster" || path == "emulated" || path == "remote";
}

std::uint32_t SchemaChildSortGroup(const SysInformationCatalogObjectSource& object) {
  if (IsSchemaObject(object)) { return 10; }
  if (IsTableNavigatorObject(object)) { return 20; }
  if (IsViewNavigatorObject(object)) { return 30; }
  if (IsProgrammabilityObject(object)) {
    if (object.object_class == "procedure") { return 41; }
    if (object.object_class == "function") { return 42; }
    if (object.object_class == "package") { return 43; }
    if (object.object_class == "sequence") { return 44; }
  }
  return 90;
}

std::uint32_t ProgrammabilityChildSortGroup(const SysInformationCatalogObjectSource& object) {
  if (object.object_class == "procedure") { return 10; }
  if (object.object_class == "function") { return 20; }
  if (object.object_class == "package") { return 30; }
  if (object.object_class == "sequence") { return 40; }
  return 90;
}

std::string NavigatorObjectNodeId(const SysInformationCatalogObjectSource& object) {
  return object.object_class + ":" + object.object_uuid;
}

std::string NavigatorFolderNodeId(std::string_view parent_node_id,
                                  std::string_view folder_name) {
  return "folder:" + std::string(parent_node_id) + ":" + std::string(folder_name);
}

std::string NavigatorChildPath(std::string_view parent_path, std::string_view child_name) {
  if (parent_path.empty()) { return std::string(child_name); }
  return std::string(parent_path) + "/" + std::string(child_name);
}

std::string NavigatorProjectedObjectNodeId(std::string_view parent_node_id,
                                           std::string_view node_role,
                                           const SysInformationCatalogObjectSource& object) {
  return "projection:" + std::string(parent_node_id) + ":" + std::string(node_role) +
         ":" + object.object_uuid;
}

void AddNavigatorRow(SysInformationProjectionResult* result,
                     std::string node_id,
                     std::string parent_node_id,
                     std::string object_id,
                     std::string parent_object_id,
                     std::string node_path,
                     std::string node_name,
                     std::string node_kind,
                     std::string node_role,
                     std::string object_kind,
                     std::string object_path,
                     std::string schema_path,
                     std::string parent_path,
                     std::uint32_t depth,
                     std::uint32_t sort_group,
                     std::uint64_t sort_ordinal,
                     bool is_virtual,
                     bool is_expandable) {
  SysInformationProjectionRow row;
  AddField(&row, "node_id", std::move(node_id));
  AddField(&row, "parent_node_id", std::move(parent_node_id));
  AddField(&row, "object_id", std::move(object_id));
  AddField(&row, "parent_object_id", std::move(parent_object_id));
  AddField(&row, "node_path", std::move(node_path));
  AddField(&row, "node_name", std::move(node_name));
  AddField(&row, "node_kind", std::move(node_kind));
  AddField(&row, "node_role", std::move(node_role));
  AddField(&row, "object_kind", std::move(object_kind));
  AddField(&row, "object_path", std::move(object_path));
  AddField(&row, "schema_path", std::move(schema_path));
  AddField(&row, "parent_path", std::move(parent_path));
  AddField(&row, "depth", std::to_string(depth));
  AddField(&row, "sort_group", std::to_string(sort_group));
  AddField(&row, "sort_ordinal", std::to_string(sort_ordinal));
  AddField(&row, "is_virtual", is_virtual ? "YES" : "NO");
  AddField(&row, "is_expandable", is_expandable ? "YES" : "NO");
  AddField(&row, "status", "active");
  AddField(&row, "visibility_state", "visible");
  result->rows.push_back(std::move(row));
}

struct NavigatorObjectRow {
  const SysInformationCatalogObjectSource* object = nullptr;
  std::string object_name;
  std::string object_path;
  std::string parent_path;
  std::string parent_object_uuid;
  std::string schema_path;
  std::uint32_t depth = 0;
  std::uint32_t sort_group = 0;
};

std::string NavigatorObjectRole(const NavigatorObjectRow& nav) {
  if (nav.object == nullptr) { return {}; }
  if (IsSchemaObject(*nav.object) && nav.parent_path.empty() && nav.object_path == "cluster") {
    return "cluster";
  }
  return nav.object->object_class;
}

bool IsSecurityManagementObject(const SysInformationCatalogObjectSource& object) {
  return IsSecurityUserObject(object) ||
         IsSecurityGroupObject(object) ||
         IsSecurityRoleObject(object) ||
         IsSecurityPolicyObject(object) ||
         IsSecurityConfigurationObject(object) ||
         IsSecurityGrantObject(object) ||
         IsSecurityUserGroupMembershipObject(object) ||
         IsSecurityUserRoleMembershipObject(object) ||
         IsSecurityGroupUserMembershipObject(object) ||
         IsSecurityGroupRoleMembershipObject(object) ||
         IsSecurityRoleUserMembershipObject(object) ||
         IsSecurityRoleGroupMembershipObject(object);
}

std::string ObjectSchemaTreePath(const std::vector<SysInformationCatalogObjectSource>& catalog_objects,
                                 const std::vector<SysInformationResolverNameSource>& resolver_names,
                                 const SysInformationProjectionContext& context,
                                 const SysInformationCatalogObjectSource& object,
                                 std::string_view fallback_parent_path) {
  if (IsSchemaObject(object)) { return std::string(fallback_parent_path); }
  if (object.schema_uuid.empty()) { return std::string(fallback_parent_path); }
  const auto* schema = FindObject(catalog_objects, object.schema_uuid, context);
  if (schema == nullptr) { return std::string(fallback_parent_path); }
  bool found_schema_path = false;
  const std::string schema_path =
      TreeDisplayPath(catalog_objects, resolver_names, context, *schema, &found_schema_path);
  return found_schema_path ? schema_path : std::string(fallback_parent_path);
}

std::vector<NavigatorObjectRow> VisibleNavigatorObjects(
    const std::vector<SysInformationCatalogObjectSource>& catalog_objects,
    const std::vector<SysInformationResolverNameSource>& resolver_names,
    const SysInformationProjectionContext& context) {
  std::vector<NavigatorObjectRow> out;
  for (const auto& object : catalog_objects) {
    if (!ObjectVisible(object, context)) { continue; }
    bool found_name = false;
    bool found_path = false;
    bool found_parent = false;
    const std::string object_name = ObjectDisplayName(resolver_names, context, object, &found_name);
    const std::string object_path =
        TreeDisplayPath(catalog_objects, resolver_names, context, object, &found_path);
    const std::string parent_path =
        TreeParentPath(catalog_objects, resolver_names, context, object, &found_parent);
    if (!found_name || !found_path || !found_parent) { continue; }

    NavigatorObjectRow row;
    row.object = &object;
    row.object_name = object_name;
    row.object_path = object_path;
    row.parent_path = parent_path;
    row.parent_object_uuid = TreeParentObjectId(object);
    row.schema_path = IsSchemaObject(object)
                          ? object_path
                          : ObjectSchemaTreePath(catalog_objects,
                                                 resolver_names,
                                                 context,
                                                 object,
                                                 parent_path);
    row.depth = TreeDepth(catalog_objects, context, object);
    row.sort_group = row.parent_path.empty()
                         ? SchemaRootSortGroup(row.object_path)
                         : SchemaChildSortGroup(object);
    out.push_back(std::move(row));
  }
  std::sort(out.begin(), out.end(), [](const auto& left, const auto& right) {
    if (left.parent_path != right.parent_path) { return left.parent_path < right.parent_path; }
    if (left.sort_group != right.sort_group) { return left.sort_group < right.sort_group; }
    return left.object_path < right.object_path;
  });
  return out;
}

void SortNavigatorRows(std::vector<const NavigatorObjectRow*>* rows) {
  std::sort(rows->begin(), rows->end(), [](const auto* left, const auto* right) {
    if (left->sort_group != right->sort_group) { return left->sort_group < right->sort_group; }
    return left->object_path < right->object_path;
  });
}

SysInformationProjectionResult BuildNavigatorTreeProjection(
    const SysInformationProjectionContext& context,
    const std::vector<SysInformationCatalogObjectSource>& catalog_objects,
    const std::vector<SysInformationResolverNameSource>& resolver_names,
    const std::vector<SysInformationColumnSource>& columns) {
  SysInformationProjectionResult result;
  (void)columns;
  std::uint64_t ordinal = 0;
  const std::string database_name =
      context.catalog_display_name.empty() ? "Database" : context.catalog_display_name;
  const std::string database_node_id = "database";
  AddNavigatorRow(&result,
                  database_node_id,
                  "",
                  "",
                  "",
                  database_name,
                  database_name,
                  "database",
                  "database",
                  "",
                  "",
                  "",
                  "",
                  0,
                  0,
                  ++ordinal,
                  true,
                  true);

  auto emit_folder = [&](std::string_view parent_node_id,
                         std::string_view parent_object_id,
                         std::string_view parent_node_path,
                         std::string_view schema_path,
                         std::string_view role,
                         std::string_view label,
                         std::uint32_t depth,
                         std::uint32_t sort_group,
                         bool expandable = true) {
    const std::string folder_id = NavigatorFolderNodeId(parent_node_id, role);
    AddNavigatorRow(&result,
                    folder_id,
                    std::string(parent_node_id),
                    "",
                    std::string(parent_object_id),
                    NavigatorChildPath(parent_node_path, label),
                    std::string(label),
                    "folder",
                    std::string(role),
                    "",
                    "",
                    std::string(schema_path),
                    std::string(parent_node_path),
                    depth,
                    sort_group,
                    ++ordinal,
                    true,
                    expandable);
    return folder_id;
  };

  const auto nav_objects = VisibleNavigatorObjects(catalog_objects, resolver_names, context);
  std::map<std::string, const NavigatorObjectRow*> by_object_uuid;
  std::map<std::string, std::vector<const NavigatorObjectRow*>> children_by_parent_uuid;
  for (const auto& row : nav_objects) {
    if (row.object == nullptr) { continue; }
    by_object_uuid[row.object->object_uuid] = &row;
    children_by_parent_uuid[row.parent_object_uuid].push_back(&row);
  }
  for (auto& entry : children_by_parent_uuid) {
    SortNavigatorRows(&entry.second);
  }

  const std::string management_id =
      emit_folder(database_node_id, "", database_name, "", "database.management", "Management", 1, 100);
  const std::string management_path = NavigatorChildPath(database_name, "Management");

  const std::string security_id =
      emit_folder(management_id, "", management_path, "", "database.security", "Security", 2, 10);
  const std::string security_path = NavigatorChildPath(management_path, "Security");
  const std::string security_users_id =
      emit_folder(security_id, "", security_path, "", "security.users", "users", 3, 10);
  const std::string security_groups_id =
      emit_folder(security_id, "", security_path, "", "security.groups", "groups", 3, 20);
  const std::string security_roles_id =
      emit_folder(security_id, "", security_path, "", "security.roles", "roles", 3, 30);
  const std::string security_policies_id =
      emit_folder(security_id, "", security_path, "", "security.policies", "policies", 3, 40);
  const std::string security_configurations_id =
      emit_folder(security_id,
                  "",
                  security_path,
                  "",
                  "security.configurations",
                  "configurations",
                  3,
                  50);

  auto children_matching =
      [&](std::string_view parent_uuid,
          const std::function<bool(const SysInformationCatalogObjectSource&)>& predicate) {
        std::vector<const NavigatorObjectRow*> out;
        const auto found = children_by_parent_uuid.find(std::string(parent_uuid));
        if (found == children_by_parent_uuid.end()) { return out; }
        for (const auto* child : found->second) {
          if (child == nullptr || child->object == nullptr) { continue; }
          if (predicate(*child->object)) { out.push_back(child); }
        }
        SortNavigatorRows(&out);
        return out;
      };

  auto emit_security_child_object =
      [&](const NavigatorObjectRow& nav,
          std::string_view parent_node_id,
          std::string_view parent_node_path,
          std::string_view node_role,
          std::string_view object_kind,
          std::uint32_t depth,
          std::uint32_t sort_group) {
        const auto& object = *nav.object;
        AddNavigatorRow(&result,
                        NavigatorObjectNodeId(object),
                        std::string(parent_node_id),
                        object.object_uuid,
                        nav.parent_object_uuid,
                        NavigatorChildPath(parent_node_path, nav.object_name),
                        nav.object_name,
                        object.object_class,
                        std::string(node_role),
                        object_kind.empty() ? object.object_class : std::string(object_kind),
                        nav.object_path,
                        "",
                        std::string(parent_node_path),
                        depth,
                        sort_group,
                        ++ordinal,
                        false,
                        false);
      };

  auto emit_security_object =
      [&](const NavigatorObjectRow& nav,
          std::string_view parent_node_id,
          std::string_view parent_node_path,
          std::string_view node_role,
          std::string_view object_path_prefix,
          std::uint32_t sort_group,
          bool membership_folders) {
    const auto& object = *nav.object;
    const std::string node_id = NavigatorObjectNodeId(object);
    const std::string node_path = NavigatorChildPath(parent_node_path, nav.object_name);
    const std::string object_path =
        std::string(object_path_prefix) + "." + nav.object_name;
    AddNavigatorRow(&result,
                    node_id,
                    std::string(parent_node_id),
                    object.object_uuid,
                    nav.parent_object_uuid,
                    node_path,
                    nav.object_name,
                    object.object_class,
                    std::string(node_role),
                    object.object_class,
                    object_path,
                    "",
                    std::string(parent_node_path),
                    4,
                    sort_group,
                    ++ordinal,
                    false,
                    membership_folders);
    if (!membership_folders) { return; }
    if (node_role == "security.user") {
      const std::string groups_id =
          emit_folder(node_id, object.object_uuid, node_path, "", "security.user.groups", "groups", 5, 10);
      const std::string roles_id =
          emit_folder(node_id, object.object_uuid, node_path, "", "security.user.roles", "roles", 5, 20);
      const std::string grants_id =
          emit_folder(node_id, object.object_uuid, node_path, "", "security.user.grants", "grants", 5, 30);
      const std::string groups_path = NavigatorChildPath(node_path, "groups");
      const std::string roles_path = NavigatorChildPath(node_path, "roles");
      const std::string grants_path = NavigatorChildPath(node_path, "grants");
      for (const auto* child : children_matching(object.object_uuid, IsSecurityUserGroupMembershipObject)) {
        emit_security_child_object(*child, groups_id, groups_path, "security.user.group", "group", 6, 10);
      }
      for (const auto* child : children_matching(object.object_uuid, IsSecurityUserRoleMembershipObject)) {
        emit_security_child_object(*child, roles_id, roles_path, "security.user.role", "role", 6, 10);
      }
      for (const auto* child : children_matching(object.object_uuid, IsSecurityGrantObject)) {
        emit_security_child_object(*child, grants_id, grants_path, "security.user.grant", "grant", 6, 10);
      }
    } else if (node_role == "security.group") {
      const std::string users_id =
          emit_folder(node_id, object.object_uuid, node_path, "", "security.group.users", "users", 5, 10);
      const std::string roles_id =
          emit_folder(node_id, object.object_uuid, node_path, "", "security.group.roles", "roles", 5, 20);
      const std::string grants_id =
          emit_folder(node_id, object.object_uuid, node_path, "", "security.group.grants", "grants", 5, 30);
      const std::string users_path = NavigatorChildPath(node_path, "users");
      const std::string roles_path = NavigatorChildPath(node_path, "roles");
      const std::string grants_path = NavigatorChildPath(node_path, "grants");
      for (const auto* child : children_matching(object.object_uuid, IsSecurityGroupUserMembershipObject)) {
        emit_security_child_object(*child, users_id, users_path, "security.group.user", "user", 6, 10);
      }
      for (const auto* child : children_matching(object.object_uuid, IsSecurityGroupRoleMembershipObject)) {
        emit_security_child_object(*child, roles_id, roles_path, "security.group.role", "role", 6, 10);
      }
      for (const auto* child : children_matching(object.object_uuid, IsSecurityGrantObject)) {
        emit_security_child_object(*child, grants_id, grants_path, "security.group.grant", "grant", 6, 10);
      }
    } else if (node_role == "security.role") {
      const std::string users_id =
          emit_folder(node_id, object.object_uuid, node_path, "", "security.role.users", "users", 5, 10);
      const std::string groups_id =
          emit_folder(node_id, object.object_uuid, node_path, "", "security.role.groups", "groups", 5, 20);
      const std::string grants_id =
          emit_folder(node_id, object.object_uuid, node_path, "", "security.role.grants", "grants", 5, 30);
      const std::string users_path = NavigatorChildPath(node_path, "users");
      const std::string groups_path = NavigatorChildPath(node_path, "groups");
      const std::string grants_path = NavigatorChildPath(node_path, "grants");
      for (const auto* child : children_matching(object.object_uuid, IsSecurityRoleUserMembershipObject)) {
        emit_security_child_object(*child, users_id, users_path, "security.role.user", "user", 6, 10);
      }
      for (const auto* child : children_matching(object.object_uuid, IsSecurityRoleGroupMembershipObject)) {
        emit_security_child_object(*child, groups_id, groups_path, "security.role.group", "group", 6, 10);
      }
      for (const auto* child : children_matching(object.object_uuid, IsSecurityGrantObject)) {
        emit_security_child_object(*child, grants_id, grants_path, "security.role.grant", "grant", 6, 10);
      }
    }
  };

  const std::string security_users_path = NavigatorChildPath(security_path, "users");
  const std::string security_groups_path = NavigatorChildPath(security_path, "groups");
  const std::string security_roles_path = NavigatorChildPath(security_path, "roles");
  const std::string security_policies_path = NavigatorChildPath(security_path, "policies");
  const std::string security_configurations_path =
      NavigatorChildPath(security_path, "configurations");
  for (const auto& nav : nav_objects) {
    if (nav.object == nullptr) { continue; }
    if (IsSecurityUserObject(*nav.object)) {
      emit_security_object(nav,
                           security_users_id,
                           security_users_path,
                           "security.user",
                           "security.users",
                           10,
                           true);
    } else if (IsSecurityGroupObject(*nav.object)) {
      emit_security_object(nav,
                           security_groups_id,
                           security_groups_path,
                           "security.group",
                           "security.groups",
                           10,
                           true);
    } else if (IsSecurityRoleObject(*nav.object)) {
      emit_security_object(nav,
                           security_roles_id,
                           security_roles_path,
                           "security.role",
                           "security.roles",
                           10,
                           true);
    } else if (IsSecurityPolicyObject(*nav.object)) {
      emit_security_object(nav,
                           security_policies_id,
                           security_policies_path,
                           "security.policy",
                           "security.policies",
                           10,
                           false);
    } else if (IsSecurityConfigurationObject(*nav.object)) {
      emit_security_object(nav,
                           security_configurations_id,
                           security_configurations_path,
                           "security.configuration",
                           "security.configurations",
                           10,
                           false);
    }
  }

  const std::string programmability_id =
      emit_folder(management_id,
                  "",
                  management_path,
                  "",
                  "database.programmability",
                  "Programmability",
                  2,
                  20);
  const std::string programmability_path = NavigatorChildPath(management_path, "Programmability");
  const std::string domains_id =
      emit_folder(management_id, "", management_path, "", "database.domains", "Domains", 2, 30);
  const std::string domains_path = NavigatorChildPath(management_path, "Domains");
  emit_folder(management_id, "", management_path, "", "database.agents", "Agents", 2, 40);
  emit_folder(management_id, "", management_path, "", "database.jobs", "Jobs", 2, 50);
  emit_folder(management_id,
              "",
              management_path,
              "",
              "database.diagnostics_metrics",
              "Diagnostics / Metrics",
              2,
              60);
  const std::string triggers_id =
      emit_folder(management_id, "", management_path, "", "database.triggers", "Triggers", 2, 70);
  const std::string triggers_path = NavigatorChildPath(management_path, "Triggers");
  emit_folder(management_id,
              "",
              management_path,
              "",
              "database.filespaces",
              "File-spaces",
              2,
              80);

  auto emit_object =
      [&](const NavigatorObjectRow& nav,
          std::string_view parent_node_id,
          std::string_view parent_node_path,
          std::uint32_t depth,
          std::uint32_t sort_group) {
    const auto& object = *nav.object;
    const std::string node_id = NavigatorObjectNodeId(object);
    const std::string node_path = NavigatorChildPath(parent_node_path, nav.object_name);
    const bool expandable = IsSchemaObject(object) ||
                            IsTableNavigatorObject(object) ||
                            IsViewNavigatorObject(object) ||
                            IsRoutineNavigatorObject(object);
    AddNavigatorRow(&result,
                    node_id,
                    std::string(parent_node_id),
                    object.object_uuid,
                    nav.parent_object_uuid,
                    node_path,
                    nav.object_name,
                    object.object_class,
                    NavigatorObjectRole(nav),
                    object.object_class,
                    nav.object_path,
                    nav.schema_path,
                    std::string(parent_node_path),
                    depth,
                    sort_group,
                    ++ordinal,
                    false,
                    expandable);
    return node_id;
  };

  auto emit_projected_object =
      [&](const NavigatorObjectRow& nav,
          std::string_view parent_node_id,
          std::string_view parent_node_path,
          std::string_view node_role,
          std::uint32_t depth,
          std::uint32_t sort_group,
          bool expandable = false) {
    const auto& object = *nav.object;
    AddNavigatorRow(&result,
                    NavigatorProjectedObjectNodeId(parent_node_id, node_role, object),
                    std::string(parent_node_id),
                    object.object_uuid,
                    nav.parent_object_uuid,
                    NavigatorChildPath(parent_node_path, nav.object_name),
                    nav.object_name,
                    object.object_class,
                    std::string(node_role),
                    object.object_class,
                    nav.object_path,
                    nav.schema_path,
                    std::string(parent_node_path),
                    depth,
                    sort_group,
                    ++ordinal,
                    false,
                    expandable);
  };

  auto emit_projected_group = [&](std::string_view parent_node_id,
                                  std::string_view parent_path,
                                  std::string_view role,
                                  std::string_view label,
                                  const std::function<bool(const SysInformationCatalogObjectSource&)>& predicate,
                                  std::uint32_t sort_group) {
    std::vector<const NavigatorObjectRow*> children;
    for (const auto& nav : nav_objects) {
      if (nav.object == nullptr || !predicate(*nav.object)) { continue; }
      children.push_back(&nav);
    }
    if (children.empty()) { return; }
    SortNavigatorRows(&children);
    const std::string folder_id =
        emit_folder(parent_node_id, "", parent_path, "", role, label, 3, sort_group);
    const std::string folder_path = NavigatorChildPath(parent_path, label);
    for (const auto* child : children) {
      emit_projected_object(*child,
                            folder_id,
                            folder_path,
                            std::string(role) + ".object",
                            4,
                            child->sort_group);
    }
  };

  emit_projected_group(programmability_id,
                       programmability_path,
                       "database.programmability.procedures",
                       "procedures",
                       [](const auto& object) { return object.object_class == "procedure"; },
                       10);
  emit_projected_group(programmability_id,
                       programmability_path,
                       "database.programmability.functions",
                       "functions",
                       [](const auto& object) { return object.object_class == "function"; },
                       20);
  emit_projected_group(programmability_id,
                       programmability_path,
                       "database.programmability.packages",
                       "packages",
                       [](const auto& object) { return object.object_class == "package"; },
                       30);
  emit_projected_group(programmability_id,
                       programmability_path,
                       "database.programmability.sequences",
                       "sequences",
                       [](const auto& object) { return object.object_class == "sequence"; },
                       40);
  emit_projected_group(domains_id,
                       domains_path,
                       "database.domains.domains",
                       "domains",
                       [](const auto& object) { return object.object_class == "domain"; },
                       10);
  emit_projected_group(triggers_id,
                       triggers_path,
                       "database.triggers.triggers",
                       "triggers",
                       [](const auto& object) { return object.object_class == "trigger"; },
                       10);

  std::function<void(const NavigatorObjectRow&,
                     std::string_view,
                     std::string_view,
                     std::uint32_t)> emit_physical;
  emit_physical = [&](const NavigatorObjectRow& nav,
                      std::string_view parent_node_id,
                      std::string_view parent_node_path,
                      std::uint32_t depth) {
    const auto& object = *nav.object;
    const std::string node_id = emit_object(nav, parent_node_id, parent_node_path, depth, nav.sort_group);
    const std::string node_path = NavigatorChildPath(parent_node_path, nav.object_name);

    const auto found = children_by_parent_uuid.find(object.object_uuid);
    if (found == children_by_parent_uuid.end()) { return; }
    for (const auto* child : found->second) {
      if (child == nullptr || child->object == nullptr) { continue; }
      if (IsSecurityManagementObject(*child->object)) { continue; }
      emit_physical(*child, node_id, node_path, depth + 1);
    }
  };

  auto has_visible_children = [&](const NavigatorObjectRow& nav) {
    if (nav.object == nullptr) { return false; }
    const auto found = children_by_parent_uuid.find(nav.object->object_uuid);
    if (found == children_by_parent_uuid.end()) { return false; }
    for (const auto* child : found->second) {
      if (child != nullptr && child->object != nullptr &&
          !IsSecurityManagementObject(*child->object)) {
        return true;
      }
    }
    return false;
  };

  for (const auto* root : children_matching("", IsSchemaObject)) {
    if (root == nullptr || root->object == nullptr) { continue; }
    if (IsContextualSchemaRoot(root->object_path) && !has_visible_children(*root)) {
      continue;
    }
    emit_physical(*root, database_node_id, database_name, 1);
  }

  return result;
}

std::string UIntText(std::uint64_t value) {
  return value == 0 ? std::string{} : std::to_string(value);
}

std::string SignedText(const dt::DatatypeDescriptor& descriptor) {
  if (descriptor.family == dt::TypeFamily::signed_integer ||
      descriptor.family == dt::TypeFamily::real ||
      descriptor.family == dt::TypeFamily::decimal) {
    return "YES";
  }
  if (descriptor.family == dt::TypeFamily::unsigned_integer) { return "NO"; }
  return "UNKNOWN";
}

std::string NumericRadixText(const dt::DatatypeDescriptor& descriptor) {
  if (descriptor.family == dt::TypeFamily::signed_integer ||
      descriptor.family == dt::TypeFamily::unsigned_integer ||
      descriptor.family == dt::TypeFamily::real) {
    return "2";
  }
  if (descriptor.family == dt::TypeFamily::decimal) { return "10"; }
  return {};
}

std::string DisplaySizeText(const dt::DatatypeDescriptor& descriptor) {
  if (descriptor.type_id == dt::CanonicalTypeId::int128) { return "40"; }
  if (descriptor.type_id == dt::CanonicalTypeId::uint128) { return "39"; }
  if (descriptor.type_id == dt::CanonicalTypeId::real128) { return "48"; }
  if (descriptor.type_id == dt::CanonicalTypeId::uuid) { return "36"; }
  return UIntText(descriptor.default_precision != 0 ? descriptor.default_precision
                                                    : descriptor.bit_width);
}

std::vector<SysInformationDatatypeDescriptorSource> BuiltinDatatypeDescriptorSources(
    const SysInformationProjectionContext& context) {
  std::vector<SysInformationDatatypeDescriptorSource> sources;
  for (const auto& descriptor : dt::BuiltinDatatypeDescriptors()) {
    if (descriptor.type_id == dt::CanonicalTypeId::null_type ||
        descriptor.type_id == dt::CanonicalTypeId::unknown) {
      continue;
    }
    const auto wire_type_id = dt::WireTypeIdForCanonicalTypeId(descriptor.type_id);
    SysInformationDatatypeDescriptorSource source;
    source.type_catalog = context.catalog_display_name;
    source.type_schema = "sys";
    source.type_name = descriptor.stable_name;
    source.standard_type_name = dt::CanonicalTypeName(descriptor.type_id);
    source.canonical_type_family = dt::CanonicalWireTypeFamilyName(
        static_cast<dt::CanonicalWireTypeFamily>(wire_type_id.type_family));
    source.canonical_type_code = dt::CanonicalWireTypeCodeName(wire_type_id);
    source.driver_family = "native";
    source.native_type_code = std::to_string(wire_type_id.type_family) + ":" +
                              std::to_string(wire_type_id.type_code);
    source.precision = UIntText(descriptor.default_precision != 0 ? descriptor.default_precision
                                                                  : descriptor.bit_width);
    source.scale = descriptor.default_scale == 0 ? std::string{} : std::to_string(descriptor.default_scale);
    source.display_size = DisplaySizeText(descriptor);
    source.numeric_precision_radix = NumericRadixText(descriptor);
    source.is_nullable = descriptor.nullable_allowed ? "YES" : "NO";
    source.is_signed = SignedText(descriptor);
    source.is_case_sensitive = descriptor.type_id == dt::CanonicalTypeId::character
                                   ? "COLLATION_DEPENDENT"
                                   : "NO";
    const bool opaque_render_only = descriptor.type_id == dt::CanonicalTypeId::opaque_extension;
    source.is_searchable = opaque_render_only ? "NONE" : "BASIC";
    source.is_currency = "NO";
    source.is_auto_increment_capable =
        (descriptor.family == dt::TypeFamily::signed_integer ||
         descriptor.family == dt::TypeFamily::unsigned_integer)
            ? "YES"
            : "NO";
    source.create_params = descriptor.type_id == dt::CanonicalTypeId::decimal
                               ? "precision,scale"
                               : std::string{};
    source.compatibility_class = opaque_render_only ? "render_only" : "native_or_better";
    source.support_state = opaque_render_only ? "unsupported" : "supported";
    if (descriptor.requires_mandatory_library) {
      source.backend_profile = "sbl_numeric:" + descriptor.required_capability_key;
    }
    sources.push_back(std::move(source));
  }
  return sources;
}

bool DatatypeDescriptorVisible(const SysInformationDatatypeDescriptorSource& source,
                               const SysInformationProjectionContext& context) {
  if (source.hidden) { return false; }
  if (context.visible_catalog_generation_id != 0 &&
      source.catalog_generation_id > context.visible_catalog_generation_id) {
    return false;
  }
  return !source.type_name.empty();
}

std::string StableRef(std::string value, std::string fallback) {
  return value.empty() ? std::move(fallback) : std::move(value);
}

std::string VisibleRef(std::string ref,
                       std::string raw_uuid,
                       std::string redacted_value,
                       bool visible = true) {
  if (!visible) {
    return std::move(redacted_value);
  }
  if (!ref.empty()) {
    return std::move(ref);
  }
  return raw_uuid.empty() ? std::string{} : std::move(redacted_value);
}

std::string VisibleUuid(std::string raw_uuid,
                        std::string redacted_value,
                        bool visible = true) {
  if (!visible) {
    return std::move(redacted_value);
  }
  return std::move(raw_uuid);
}

bool IsSupportedProjection(std::string_view view_path) {
  return !SysInformationPathIsClusterScoped(view_path) &&
         FindSysInformationProjectionDefinition(view_path) != nullptr;
}

}  // namespace

const char* SysInformationProjectionFamilyName(SysInformationProjectionFamily family) {
  switch (family) {
    case SysInformationProjectionFamily::standard_information_schema:
      return "standard_information_schema";
    case SysInformationProjectionFamily::scratchbird_extension:
      return "scratchbird_extension";
    case SysInformationProjectionFamily::catalog_readable:
      return "catalog_readable";
    case SysInformationProjectionFamily::frontend_projection:
      return "frontend_projection";
  }
  return "unknown";
}

const char* SysInformationSourceKindName(SysInformationSourceKind source_kind) {
  switch (source_kind) {
    case SysInformationSourceKind::catalog_object_identity: return "catalog_object_identity";
    case SysInformationSourceKind::identity_resolver: return "identity_resolver";
    case SysInformationSourceKind::comment_resolver: return "comment_resolver";
    case SysInformationSourceKind::catalog_index_profile: return "catalog_index_profile";
    case SysInformationSourceKind::security_policy: return "security_policy";
    case SysInformationSourceKind::datatype_descriptor: return "datatype_descriptor";
    case SysInformationSourceKind::routine_parameter_metadata: return "routine_parameter_metadata";
    case SysInformationSourceKind::agent_runtime: return "agent_runtime";
    case SysInformationSourceKind::agent_metric_dependency: return "agent_metric_dependency";
    case SysInformationSourceKind::agent_policy: return "agent_policy";
    case SysInformationSourceKind::agent_action: return "agent_action";
    case SysInformationSourceKind::agent_evidence: return "agent_evidence";
    case SysInformationSourceKind::storage_agent_state: return "storage_agent_state";
    case SysInformationSourceKind::ipar_agent_lifecycle: return "ipar_agent_lifecycle";
    case SysInformationSourceKind::ipar_metric_counter: return "ipar_metric_counter";
    case SysInformationSourceKind::ipar_telemetry_control: return "ipar_telemetry_control";
    case SysInformationSourceKind::ipar_slow_path_reason: return "ipar_slow_path_reason";
    case SysInformationSourceKind::ipar_contention_quota: return "ipar_contention_quota";
  }
  return "unknown";
}

const std::vector<SysInformationProjectionDefinition>& BuiltinSysInformationProjectionDefinitions() {
  static const std::vector<SysInformationProjectionDefinition> definitions = [] {
    std::vector<SysInformationProjectionDefinition> out = {
      Definition("sys.information.schemata",
                 SysInformationProjectionFamily::standard_information_schema,
                 {Column("catalog_name", "text", false, true),
                  Column("schema_name", "text", false, true),
                  Column("schema_owner", "text", true, true),
                  Column("default_character_set_catalog", "text", true, true),
                  Column("default_character_set_schema", "text", true, true),
                  Column("default_character_set_name", "text", true, true),
                  Column("sql_path", "text", true, true)},
                 {SysInformationSourceKind::catalog_object_identity,
                  SysInformationSourceKind::identity_resolver,
                  SysInformationSourceKind::security_policy}),
      Definition("sys.information.tables",
                 SysInformationProjectionFamily::standard_information_schema,
                 {Column("table_catalog", "text", false, true),
                  Column("table_schema", "text", false, true),
                  Column("table_name", "text", false, true),
                  Column("table_type", "text"),
                  Column("self_referencing_column_name", "text", true),
                  Column("reference_generation", "text", true),
                  Column("user_defined_type_catalog", "text", true),
                  Column("user_defined_type_schema", "text", true, true),
                  Column("user_defined_type_name", "text", true, true),
                  Column("is_insertable_into", "yes_no"),
                  Column("is_typed", "yes_no"),
                  Column("commit_action", "text", true)},
                 {SysInformationSourceKind::catalog_object_identity,
                  SysInformationSourceKind::identity_resolver,
                  SysInformationSourceKind::security_policy}),
      Definition("sys.information.columns",
                 SysInformationProjectionFamily::standard_information_schema,
                 {Column("table_catalog", "text", false, true),
                  Column("table_schema", "text", false, true),
                  Column("table_name", "text", false, true),
                  Column("column_name", "text", false, true),
                  Column("ordinal_position", "uint32"),
                  Column("column_default", "text", true),
                  Column("is_nullable", "yes_no"),
                  Column("data_type", "text", false, true),
                  Column("character_maximum_length", "uint64", true),
                  Column("character_octet_length", "uint64", true),
                  Column("numeric_precision", "uint32", true),
                  Column("numeric_precision_radix", "uint32", true),
                  Column("numeric_scale", "uint32", true),
                  Column("datetime_precision", "uint32", true),
                  Column("interval_type", "text", true),
                  Column("interval_precision", "uint32", true),
                  Column("character_set_catalog", "text", true, true),
                  Column("character_set_schema", "text", true, true),
                  Column("character_set_name", "text", true, true),
                  Column("collation_catalog", "text", true, true),
                  Column("collation_schema", "text", true, true),
                  Column("collation_name", "text", true, true),
                  Column("domain_catalog", "text", true, true),
                  Column("domain_schema", "text", true, true),
                  Column("domain_name", "text", true, true),
                  Column("is_identity", "yes_no"),
                  Column("identity_generation", "text", true),
                  Column("identity_start", "text", true),
                  Column("identity_increment", "text", true),
                  Column("identity_maximum", "text", true),
                  Column("identity_minimum", "text", true),
                  Column("identity_cycle", "yes_no"),
                  Column("is_generated", "text"),
                  Column("generation_expression", "text", true),
                  Column("is_updatable", "yes_no")},
                 {SysInformationSourceKind::catalog_object_identity,
                  SysInformationSourceKind::identity_resolver,
                  SysInformationSourceKind::datatype_descriptor,
                  SysInformationSourceKind::security_policy}),
      Definition("sys.information.parameters",
                 SysInformationProjectionFamily::standard_information_schema,
                 {Column("specific_catalog", "text", false, true),
                  Column("specific_schema", "text", false, true),
                  Column("specific_name", "text", false, true),
                  Column("ordinal_position", "uint32"),
                  Column("parameter_mode", "text"),
                  Column("is_result", "yes_no"),
                  Column("as_locator", "yes_no"),
                  Column("parameter_name", "text", true, true),
                  Column("data_type", "text", false, true),
                  Column("character_maximum_length", "uint64", true),
                  Column("character_octet_length", "uint64", true),
                  Column("numeric_precision", "uint32", true),
                  Column("numeric_precision_radix", "uint32", true),
                  Column("numeric_scale", "uint32", true),
                  Column("datetime_precision", "uint32", true),
                  Column("character_set_catalog", "text", true, true),
                  Column("character_set_schema", "text", true, true),
                  Column("character_set_name", "text", true, true),
                  Column("collation_catalog", "text", true, true),
                  Column("collation_schema", "text", true, true),
                  Column("collation_name", "text", true, true),
                  Column("udt_catalog", "text", true, true),
                  Column("udt_schema", "text", true, true),
                  Column("udt_name", "text", true, true),
                  Column("domain_catalog", "text", true, true),
                  Column("domain_schema", "text", true, true),
                  Column("domain_name", "text", true, true),
                  Column("parameter_default", "text", true)},
                 {SysInformationSourceKind::catalog_object_identity,
                  SysInformationSourceKind::identity_resolver,
                  SysInformationSourceKind::datatype_descriptor,
                  SysInformationSourceKind::routine_parameter_metadata,
                  SysInformationSourceKind::security_policy}),
      Definition("sys.information.scratchbird_object_paths",
                 SysInformationProjectionFamily::scratchbird_extension,
                 {Column("object_catalog", "text", false, true, false, false, true),
                  Column("object_schema", "text", true, true, false, false, true),
                  Column("object_name", "text", false, true, false, false, true),
                  Column("object_type", "text", false, false, false, false, true),
                  Column("object_path", "text", false, true, false, false, true)},
                 {SysInformationSourceKind::catalog_object_identity,
                  SysInformationSourceKind::identity_resolver,
                  SysInformationSourceKind::security_policy}),
      Definition("sys.information.scratchbird_object_comments",
                 SysInformationProjectionFamily::scratchbird_extension,
                 {Column("object_catalog", "text", false, true, false, false, true),
                  Column("object_schema", "text", true, true, false, false, true),
                  Column("object_name", "text", false, true, false, false, true),
                  Column("object_type", "text", false, false, false, false, true),
                  Column("comment_text", "text", true, false, true, false, true)},
                 {SysInformationSourceKind::catalog_object_identity,
                  SysInformationSourceKind::identity_resolver,
                  SysInformationSourceKind::comment_resolver,
                  SysInformationSourceKind::security_policy},
                 true,
                 true),
      Definition("sys.information.scratchbird_datatype_descriptors",
                 SysInformationProjectionFamily::scratchbird_extension,
                 {Column("type_catalog", "text", false, false, false, false, true),
                  Column("type_schema", "text", true, false, false, false, true),
                  Column("type_name", "text", false, false, false, false, true),
                  Column("standard_type_name", "text", true, false, false, false, true),
                  Column("canonical_type_family", "text", false, false, false, false, true),
                  Column("canonical_type_code", "text", false, false, false, false, true),
                  Column("driver_family", "text", false, false, false, false, true),
                  Column("native_type_code", "text", true, false, false, false, true),
                  Column("sql_type_code", "text", true, false, false, false, true),
                  Column("precision", "uint64", true, false, false, false, true),
                  Column("scale", "int64", true, false, false, false, true),
                  Column("display_size", "uint64", true, false, false, false, true),
                  Column("character_maximum_length", "uint64", true, false, false, false, true),
                  Column("character_octet_length", "uint64", true, false, false, false, true),
                  Column("numeric_precision_radix", "uint32", true, false, false, false, true),
                  Column("is_nullable", "yes_no_unknown", false, false, false, false, true),
                  Column("is_signed", "yes_no_unknown", false, false, false, false, true),
                  Column("is_case_sensitive", "text", false, false, false, false, true),
                  Column("is_searchable", "text", false, false, false, false, true),
                  Column("is_currency", "yes_no", false, false, false, false, true),
                  Column("is_auto_increment_capable", "yes_no", false, false, false, false, true),
                  Column("literal_prefix", "text", true, false, false, false, true),
                  Column("literal_suffix", "text", true, false, false, false, true),
                  Column("create_params", "text", true, false, false, false, true),
                  Column("compatibility_class", "text", false, false, false, false, true),
                  Column("support_state", "text", false, false, false, false, true),
                  Column("backend_profile", "text", true, false, false, false, true),
                  Column("unsupported_reason", "text", true, false, false, false, true)},
                 {SysInformationSourceKind::datatype_descriptor,
                  SysInformationSourceKind::security_policy},
                 false,
                 false),
      Definition("sys.information.scratchbird_index_profiles",
                 SysInformationProjectionFamily::scratchbird_extension,
                 {Column("profile_name", "text", false, false, false, false, true),
                  Column("catalog_table_path", "text", false, false, false, false, true),
                  Column("access_method", "text", false, false, false, false, true),
                  Column("access_purpose", "text", false, false, false, false, true),
                  Column("is_unique", "yes_no", false, false, false, false, true),
                  Column("is_authoritative", "yes_no", false, false, false, false, true),
                  Column("resolver_boundary", "text", false, false, false, false, true),
                  Column("ordered_access", "yes_no", false, false, false, false, true),
                  Column("group_access", "yes_no", false, false, false, false, true),
                  Column("prefix_access", "yes_no", false, false, false, false, true),
                  Column("generation_access", "yes_no", false, false, false, false, true),
                  Column("history_access", "yes_no", false, false, false, false, true)},
                 {SysInformationSourceKind::catalog_index_profile,
                  SysInformationSourceKind::security_policy},
                 false,
                 false),
      Definition("sys.schemas",
                 SysInformationProjectionFamily::catalog_readable,
                 {Column("schema_id", "uuid", false, false, false, true, true),
                  Column("schema_name", "text", false, true, false, false, true),
                  Column("parent_schema_id", "uuid", true, false, false, true, true),
                  Column("schema_owner", "text", true, true, false, false, true),
                  Column("visibility_state", "text", false, false, false, false, true)},
                 {SysInformationSourceKind::catalog_object_identity,
                  SysInformationSourceKind::identity_resolver,
                  SysInformationSourceKind::security_policy},
                 true,
                 false,
                 "driver_metadata",
                 {"schema_id", "schema_name"},
                 "Driver-safe local schema metadata with stable join identifiers."),
      Definition("sys.tables",
                 SysInformationProjectionFamily::catalog_readable,
                 {Column("table_id", "uuid", false, false, false, true, true),
                  Column("schema_id", "uuid", false, false, false, true, true),
                  Column("table_name", "text", false, true, false, false, true),
                  Column("table_type", "text", false, false, false, false, true),
                  Column("is_insertable_into", "yes_no", false, false, false, false, true),
                  Column("visibility_state", "text", false, false, false, false, true)},
                 {SysInformationSourceKind::catalog_object_identity,
                  SysInformationSourceKind::identity_resolver,
                  SysInformationSourceKind::security_policy},
                 true,
                 false,
                 "driver_metadata",
                 {"table_id", "schema_id", "table_name"},
                 "Driver-safe local table and view metadata with stable join identifiers."),
      Definition("sys.columns",
                 SysInformationProjectionFamily::catalog_readable,
                 {Column("column_id", "text", false, false, false, false, true),
                  Column("table_id", "uuid", false, false, false, true, true),
                  Column("schema_id", "uuid", false, false, false, true, true),
                  Column("column_name", "text", false, false, false, false, true),
                  Column("ordinal_position", "uint32", false, false, false, false, true),
                  Column("data_type", "text", false, false, false, false, true),
                  Column("is_nullable", "yes_no", false, false, false, false, true),
                  Column("column_default", "text", true, false, false, false, true),
                  Column("visibility_state", "text", false, false, false, false, true)},
                 {SysInformationSourceKind::catalog_object_identity,
                  SysInformationSourceKind::identity_resolver,
                  SysInformationSourceKind::datatype_descriptor,
                  SysInformationSourceKind::security_policy},
                 true,
                 false,
                 "driver_metadata",
                 {"table_id", "ordinal_position", "column_name"},
                 "Driver-safe local column metadata with stable table join identifiers."),
      Definition("sys.agents",
                 SysInformationProjectionFamily::catalog_readable,
                 {ExplicitUuidColumn("agent_uuid"),
                  Column("agent_type_id", "text"),
                  Column("scope_kind", "text"),
                  ExplicitUuidColumn("scope_uuid", true),
                  Column("component", "text"),
                  Column("state", "text"),
                  Column("health_state", "text"),
                  Column("enabled", "yes_no"),
                  ExplicitUuidColumn("policy_uuid", true),
                  Column("last_transition_at", "text", true),
                  Column("last_diagnostic_code", "text", true),
                  ExplicitUuidColumn("last_evidence_uuid", true),
                  Column("policy_generation", "uint64", true),
                  Column("queue_depth", "uint64", true),
                  Column("action_backlog", "uint64", true),
                  Column("failure_count", "uint64", true),
                  Column("quarantine_count", "uint64", true),
                  Column("retry_not_before", "text", true),
                  Column("last_decision", "text", true),
                  Column("overhead_budget_units", "uint64", true),
                  Column("diagnostic_redaction_state", "text", true)},
                 {SysInformationSourceKind::agent_runtime,
                  SysInformationSourceKind::security_policy},
                 false,
                 false,
                 "agents",
                 {"agent_type_id", "scope_kind", "state", "health_state", "component"},
                 "Local agent runtime state projection for management UI consumers."),
      Definition("sys.agent_metric_dependencies",
                 SysInformationProjectionFamily::catalog_readable,
                 {ExplicitUuidColumn("agent_uuid"),
                  Column("metric_family", "text"),
                  Column("namespace", "text"),
                  Column("required_or_optional", "text"),
                  Column("freshness_limit", "text"),
                  Column("current_freshness", "text", true),
                  Column("quality_state", "text"),
                  Column("fail_behavior", "text")},
                 {SysInformationSourceKind::agent_metric_dependency,
                  SysInformationSourceKind::security_policy},
                 false,
                 false,
                 "agents",
                 {"agent_uuid", "metric_family", "quality_state"},
                 "Local agent metric dependency freshness and quality projection."),
      Definition("sys.agent_policies",
                 SysInformationProjectionFamily::catalog_readable,
                 {ExplicitUuidColumn("agent_uuid"),
                  ExplicitUuidColumn("policy_uuid"),
                  Column("policy_family", "text"),
                  ExplicitUuidColumn("version_uuid", true),
                  Column("active_state", "text"),
                  Column("validation_state", "text"),
                  Column("attached_at", "text", true),
                  Column("attached_by", "text", true)},
                 {SysInformationSourceKind::agent_policy,
                  SysInformationSourceKind::security_policy},
                 false,
                 false,
                 "agents",
                 {"agent_uuid", "policy_family", "validation_state"},
                 "Local agent policy attachment state without policy body payload."),
      Definition("sys.agent_actions",
                 SysInformationProjectionFamily::catalog_readable,
                 {ExplicitUuidColumn("action_uuid"),
                  ExplicitUuidColumn("agent_uuid"),
                  Column("action_id", "text"),
                  Column("state", "text"),
                  Column("risk_class", "text"),
                  Column("created_at", "text"),
                  Column("expires_at", "text", true),
                  Column("approval_required", "yes_no"),
                  ExplicitUuidColumn("actor_uuid", true),
                  Column("diagnostic_code", "text", true)},
                 {SysInformationSourceKind::agent_action,
                  SysInformationSourceKind::security_policy},
                 false,
                 false,
                 "agents",
                 {"action_id", "state", "risk_class", "agent_uuid"},
                 "Local agent recommended and queued actions with actor redaction."),
      Definition("sys.agent_overrides",
                 SysInformationProjectionFamily::catalog_readable,
                 {ExplicitUuidColumn("override_uuid"),
                  ExplicitUuidColumn("target_uuid"),
                  ExplicitUuidColumn("scope_uuid", true),
                  Column("suppression_class", "text"),
                  Column("starts_at", "text"),
                  Column("expires_at", "text", true),
                  Column("state", "text"),
                  Column("reason_code", "text", true),
                  Column("created_by", "text", true)},
                 {SysInformationSourceKind::agent_action,
                  SysInformationSourceKind::security_policy},
                 false,
                 false,
                 "agents",
                 {"state", "target_uuid", "scope_uuid"},
                 "Local agent override and suppression records with reason text omitted."),
      Definition("sys.agent_evidence",
                 SysInformationProjectionFamily::catalog_readable,
                 {ExplicitUuidColumn("evidence_uuid"),
                  ExplicitUuidColumn("agent_uuid"),
                  Column("evidence_type", "text"),
                  ExplicitUuidColumn("action_uuid", true),
                  Column("redaction_class", "text"),
                  Column("created_at", "text"),
                  ExplicitUuidColumn("actor_uuid", true),
                  Column("payload_digest", "text", true),
                  Column("payload_redacted", "yes_no")},
                 {SysInformationSourceKind::agent_evidence,
                  SysInformationSourceKind::security_policy},
                 false,
                 false,
                 "agents",
                 {"agent_uuid", "evidence_type", "action_uuid"},
                 "Local agent evidence projection with payload redaction state."),
      Definition("sys.agent_audit",
                 SysInformationProjectionFamily::catalog_readable,
                 {ExplicitUuidColumn("audit_uuid"),
                  ExplicitUuidColumn("evidence_uuid", true),
                  ExplicitUuidColumn("actor_uuid", true),
                  Column("command_name", "text"),
                  Column("sblr_operation", "text"),
                  Column("api_call", "text"),
                  Column("result_state", "text"),
                  Column("diagnostic_code", "text", true),
                  Column("created_at", "text")},
                 {SysInformationSourceKind::agent_evidence,
                  SysInformationSourceKind::security_policy},
                 false,
                 false,
                 "agents",
                 {"actor_uuid", "command_name", "result_state"},
                 "Local agent audit projection with redacted actor details."),
      Definition("sys.filespace_capacity_agent_state",
                 SysInformationProjectionFamily::catalog_readable,
                 {ExplicitUuidColumn("agent_uuid"),
                  ExplicitUuidColumn("filespace_uuid"),
                  ExplicitUuidColumn("policy_uuid", true),
                  Column("mode", "text"),
                  Column("health_state", "text"),
                  Column("last_capacity_metric_at", "text", true),
                  Column("last_health_metric_at", "text", true),
                  Column("last_recommendation_code", "text", true),
                  Column("last_refusal_code", "text", true)},
                 {SysInformationSourceKind::storage_agent_state,
                  SysInformationSourceKind::security_policy},
                 false,
                 false,
                 "storage_agents",
                 {"filespace_uuid", "health_state", "mode"},
                 "Filespace capacity agent local state with physical paths omitted."),
      Definition("sys.page_allocation_agent_state",
                 SysInformationProjectionFamily::catalog_readable,
                 {ExplicitUuidColumn("agent_uuid"),
                  ExplicitUuidColumn("filespace_uuid"),
                  Column("page_family", "text"),
                  Column("page_type", "text"),
                  ExplicitUuidColumn("policy_uuid", true),
                  Column("mode", "text"),
                  Column("last_scan_generation", "uint64", true),
                  Column("last_shrink_ready_state", "text", true),
                  Column("last_refusal_code", "text", true)},
                 {SysInformationSourceKind::storage_agent_state,
                  SysInformationSourceKind::security_policy},
                 false,
                 false,
                 "storage_agents",
                 {"filespace_uuid", "page_family", "page_type", "mode"},
                 "Page allocation agent local state with blocker details redacted."),
      Definition("sys.filespace_shrink_readiness",
                 SysInformationProjectionFamily::catalog_readable,
                 {ExplicitUuidColumn("filespace_uuid"),
                  Column("safe_start_byte", "uint64"),
                  Column("safe_end_byte", "uint64"),
                  Column("truncate_ready_bytes", "uint64"),
                  Column("blocker_count", "uint64"),
                  Column("readiness_state", "text"),
                  Column("scan_generation", "uint64"),
                  ExplicitUuidColumn("evidence_uuid", true)},
                 {SysInformationSourceKind::storage_agent_state,
                  SysInformationSourceKind::security_policy},
                 false,
                 false,
                 "storage_agents",
                 {"filespace_uuid", "readiness_state"},
                 "Filespace shrink readiness projection with blocker payload redacted."),
      Definition("sys.ipar.agent_lifecycle",
                 SysInformationProjectionFamily::catalog_readable,
                 {Column("runtime_id", "text"),
                  Column("source_kind", "text"),
                  Column("lifecycle_state", "text"),
                  Column("idle_state", "text"),
                  Column("started", "yes_no"),
                  Column("stopping", "yes_no"),
                  Column("worker_thread_count", "uint64"),
                  Column("background_worker_slots", "uint64"),
                  Column("foreground_reserved_capacity", "uint64"),
                  Column("worker_wake_policy", "text", true),
                  Column("scheduler_ticks", "uint64"),
                  Column("total_worker_ticks", "uint64"),
                  Column("total_actions_accepted", "uint64"),
                  Column("total_actions_refused", "uint64"),
                  Column("total_actions_failed", "uint64"),
                  Column("scheduled_worker_count", "uint64"),
                  Column("min_worker_ticks", "uint64"),
                  Column("max_worker_ticks", "uint64"),
                  Column("starvation_events", "uint64"),
                  Column("last_diagnostic_agent_type_id", "text", true),
                  Column("last_diagnostic_action", "text", true),
                  Column("last_diagnostic_outcome", "text", true),
                  Column("last_diagnostic_code", "text", true),
                  Column("last_diagnostic_detail", "text", true),
                  Column("durable_lease_count", "uint64"),
                  Column("durable_action_backlog_count", "uint64"),
                  Column("durable_replay_pending_action_count", "uint64"),
                  Column("process_rss_kb", "uint64", true),
                  Column("process_vsize_kb", "uint64", true),
                  Column("source_state", "text")},
                 {SysInformationSourceKind::ipar_agent_lifecycle,
                  SysInformationSourceKind::agent_runtime,
                  SysInformationSourceKind::security_policy},
                 false,
                 false,
                 "ipar_observability",
                 {"runtime_id", "lifecycle_state", "idle_state"},
                 "Driver-visible IPAR agent lifecycle and idle-state projection over server runtime snapshots."),
      Definition("sys.ipar.metric_counters",
                 SysInformationProjectionFamily::catalog_readable,
                 {Column("metric_id", "text"),
                  Column("metric_path", "text"),
                  Column("metric_type", "text"),
                  Column("metric_unit", "text"),
                  Column("value", "uint64"),
                  Column("sample_count", "uint64"),
                  Column("label_summary", "text", true),
                  Column("producer", "text", true),
                  Column("source_state", "text")},
                 {SysInformationSourceKind::ipar_metric_counter,
                  SysInformationSourceKind::security_policy},
                 false,
                 false,
                 "ipar_observability",
                 {"metric_id", "metric_path", "producer"},
                 "Driver-visible IPAR metric counters supplied by real metric snapshots."),
      Definition("sys.ipar.telemetry_controls",
                 SysInformationProjectionFamily::catalog_readable,
                 {Column("budget_id", "text"),
                  Column("control_name", "text"),
                  Column("metric_path", "text"),
                  Column("configured_value", "uint64"),
                  Column("observed_value", "uint64"),
                  Column("sample_rate_per_mille", "uint64"),
                  Column("persist_stride", "uint64"),
                  Column("skipped_count", "uint64"),
                  Column("dropped_metric_count", "uint64"),
                  Column("overhead_budget_percent", "text"),
                  Column("source_state", "text")},
                 {SysInformationSourceKind::ipar_telemetry_control,
                  SysInformationSourceKind::security_policy},
                 false,
                 false,
                 "ipar_observability",
                 {"budget_id", "control_name", "metric_path"},
                 "Driver-visible IPAR telemetry overhead and persistence control projection."),
      Definition("sys.ipar.slow_path_reasons",
                 SysInformationProjectionFamily::catalog_readable,
                 {Column("metric_id", "text"),
                  Column("statement_id", "text"),
                  Column("chosen_path", "text"),
                  Column("reason_code", "text"),
                  Column("fallback_count", "uint64"),
                  Column("validation_stage", "text"),
                  Column("driver_visible_message", "text"),
                  Column("diagnostic_code", "text", true),
                  Column("sample_count", "uint64"),
                  Column("source_state", "text"),
                  Column("required_action", "text"),
                  Column("authority_scope", "text"),
                  Column("finality_authority", "yes_no")},
                 {SysInformationSourceKind::ipar_slow_path_reason,
                  SysInformationSourceKind::security_policy},
                 false,
                 false,
                 "ipar_observability",
                 {"statement_id", "chosen_path", "reason_code"},
                 "Driver-visible IPAR slow-path explanation rows with private details omitted."),
      Definition("sys.ipar.contention_quota",
                 SysInformationProjectionFamily::catalog_readable,
                 {Column("row_id", "text"),
                  Column("metric_id", "text"),
                  Column("category", "text"),
                  Column("subject", "text"),
                  Column("metric_path", "text"),
                  Column("metric_type", "text"),
                  Column("metric_unit", "text"),
                  Column("observed_value", "uint64"),
                  Column("limit_value", "uint64"),
                  Column("refusal_count", "uint64"),
                  Column("wait_count", "uint64"),
                  Column("queue_depth", "uint64"),
                  Column("sample_count", "uint64"),
                  Column("producer", "text", true),
                  Column("source_kind", "text"),
                  Column("source_ref", "text", true),
                  Column("provenance", "text"),
                  Column("diagnostic_code", "text", true),
                  Column("source_state", "text")},
                 {SysInformationSourceKind::ipar_contention_quota,
                  SysInformationSourceKind::security_policy},
                 false,
                 false,
                 "ipar_observability",
                 {"category", "subject", "source_kind", "source_ref"},
                 "Driver-visible IPAR contention, quota, and cache rows backed by server observability sources."),
      Definition("sys.configuration.settings",
                 SysInformationProjectionFamily::catalog_readable,
                 {Column("setting_name", "text"),
                  Column("setting_value", "text", true),
                  Column("source_scope", "text"),
                  Column("effective_state", "text")},
                 {SysInformationSourceKind::security_policy},
                 false,
                 false,
                 "settings",
                 {"setting_name", "source_scope", "effective_state"},
                 "Direct configuration setting projection for drivers and management tools."),
      Definition("sys.configuration.profiles",
                 SysInformationProjectionFamily::catalog_readable,
                 {ExplicitUuidColumn("profile_uuid"),
                  Column("profile_name", "text"),
                  Column("profile_kind", "text"),
                  Column("profile_state", "text")},
                 {SysInformationSourceKind::security_policy},
                 false,
                 false,
                 "settings",
                 {"profile_uuid", "profile_name", "profile_state"},
                 "Direct configuration profile projection for drivers and management tools."),
      Definition("sys.configuration.effective_settings",
                 SysInformationProjectionFamily::catalog_readable,
                 {Column("setting_name", "text"),
                  Column("effective_value", "text", true),
                  Column("resolved_from", "text"),
                  Column("policy_hash", "text")},
                 {SysInformationSourceKind::security_policy},
                 false,
                 false,
                 "settings",
                 {"setting_name", "resolved_from", "policy_hash"},
                 "Direct effective configuration projection for drivers and management tools."),
      Definition("sys.configuration.policy_bindings",
                 SysInformationProjectionFamily::catalog_readable,
                 {ExplicitUuidColumn("binding_uuid"),
                  ExplicitUuidColumn("profile_uuid"),
                  ExplicitUuidColumn("policy_uuid"),
                  Column("binding_state", "text")},
                 {SysInformationSourceKind::agent_policy,
                  SysInformationSourceKind::storage_agent_state,
                  SysInformationSourceKind::security_policy},
                 false,
                 false,
                 "settings",
                 {"binding_uuid", "profile_uuid", "policy_uuid", "binding_state"},
                 "Direct configuration policy binding projection for drivers and management tools."),
    };
    for (const auto& table : BuiltinCatalogTableProfiles()) {
      if (table.table_path.empty() || CatalogPathIsClusterScoped(table.table_path)) {
        continue;
      }
      auto existing = std::find_if(out.begin(), out.end(), [&table](const auto& definition) {
        return definition.view_path == table.table_path;
      });
      if (existing != out.end()) { continue; }
      out.push_back(Definition(table.table_path,
                               SysInformationProjectionFamily::catalog_readable,
                               CatalogTableColumns(table),
                               {SysInformationSourceKind::catalog_object_identity,
                                SysInformationSourceKind::security_policy},
                               false,
                               false,
                               "physical_catalog",
                               CatalogTableKeyColumns(table),
                               "Raw physical system catalog table projection."));
    }
    for (const auto& row : LocalFrontendProjectionPacketRows()) {
      auto existing = std::find_if(out.begin(), out.end(), [&row](const auto& definition) {
        return definition.view_path == row.view_path;
      });
      if (existing != out.end()) {
        existing->view_scope = "local";
        existing->ui_area = row.ui_area;
        existing->key_columns = SplitSemicolonList(row.key_columns);
        existing->description = row.description;
        continue;
      }
      auto columns = PacketColumns(row.key_columns);
      bool resolver_required = false;
      bool comment_required = false;
      for (const auto& column : columns) {
        resolver_required = resolver_required || column.resolver_backed_name;
        comment_required = comment_required || column.comment_backed_text;
      }
      std::vector<SysInformationSourceKind> source_kinds{
          SysInformationSourceKind::security_policy};
      if (resolver_required) {
        source_kinds.push_back(SysInformationSourceKind::identity_resolver);
      }
      if (comment_required) {
        source_kinds.push_back(SysInformationSourceKind::comment_resolver);
      }
      out.push_back(Definition(row.view_path,
                               PacketFamily(row.family),
                               std::move(columns),
                               std::move(source_kinds),
                               resolver_required,
                               comment_required,
                               row.ui_area,
                               SplitSemicolonList(row.key_columns),
                               row.description));
    }
    return out;
  }();
  return definitions;
}

std::string SysInformationCanonicalViewPath(std::string_view view_path) {
  static constexpr std::string_view kLegacyPrefix = "sys.information_schema.";
  static constexpr std::string_view kCanonicalPrefix = "sys.information.";
  if (StartsWith(view_path, kLegacyPrefix)) {
    return std::string(kCanonicalPrefix) + std::string(view_path.substr(kLegacyPrefix.size()));
  }
  return std::string(view_path);
}

const SysInformationProjectionDefinition* FindSysInformationProjectionDefinition(
    std::string_view view_path) {
  const std::string canonical = SysInformationCanonicalViewPath(view_path);
  for (const auto& definition : BuiltinSysInformationProjectionDefinitions()) {
    if (definition.view_path == canonical) { return &definition; }
  }
  return nullptr;
}

bool SysInformationPathIsClusterScoped(std::string_view view_path) {
  return StartsWith(view_path, "cluster.") || view_path == "cluster";
}

bool SysInformationProjectionColumnNameExposesUuid(std::string_view column_name) {
  const std::string token = "_uuid";
  if (column_name == "uuid") { return true; }
  if (column_name.size() >= token.size() &&
      column_name.substr(column_name.size() - token.size()) == token) {
    return true;
  }
  return column_name.find("uuid_") != std::string_view::npos;
}

SysInformationProjectionValidationResult ValidateSysInformationProjectionDefinition(
    const SysInformationProjectionDefinition& definition) {
  SysInformationProjectionValidationResult result;
  if (definition.view_path.empty()) {
    AddDiagnostic(&result, kSysInformationDiagnosticInvalidDefinition);
  }
  if (SysInformationPathIsClusterScoped(definition.view_path) || !definition.cluster_path_fail_closed) {
    AddDiagnostic(&result, kSysInformationDiagnosticClusterScopeForbidden);
  }
  if (definition.exposes_internal_uuid) {
    AddDiagnostic(&result, kSysInformationDiagnosticUuidExposed);
  }
  bool has_resolver_source = false;
  bool has_comment_source = false;
  for (const auto source : definition.source_kinds) {
    if (source == SysInformationSourceKind::identity_resolver) { has_resolver_source = true; }
    if (source == SysInformationSourceKind::comment_resolver) { has_comment_source = true; }
  }
  if (definition.resolver_join_required && !has_resolver_source) {
    AddDiagnostic(&result, kSysInformationDiagnosticInvalidDefinition);
  }
  if (definition.comment_join_required && !has_comment_source) {
    AddDiagnostic(&result, kSysInformationDiagnosticInvalidDefinition);
  }
  for (const auto& column : definition.columns) {
    if (column.column_name.empty()) {
      AddDiagnostic(&result, kSysInformationDiagnosticInvalidDefinition);
    }
    if (SysInformationProjectionColumnNameExposesUuid(column.column_name) &&
        !column.exposes_internal_uuid) {
      AddDiagnostic(&result, kSysInformationDiagnosticUuidExposed);
    }
    if (column.resolver_backed_name && !definition.resolver_join_required) {
      AddDiagnostic(&result, kSysInformationDiagnosticInvalidDefinition);
    }
    if (column.comment_backed_text && !definition.comment_join_required) {
      AddDiagnostic(&result, kSysInformationDiagnosticInvalidDefinition);
    }
  }
  return result;
}

SysInformationProjectionValidationResult ValidateBuiltinSysInformationProjectionDefinitions() {
  SysInformationProjectionValidationResult result;
  for (const auto& definition : BuiltinSysInformationProjectionDefinitions()) {
    const auto checked = ValidateSysInformationProjectionDefinition(definition);
    if (!checked.ok) {
      result.ok = false;
      result.diagnostic_codes.insert(result.diagnostic_codes.end(),
                                     checked.diagnostic_codes.begin(),
                                     checked.diagnostic_codes.end());
    }
  }
  if (FindSysInformationProjectionDefinition("sys.information.tables") == nullptr) {
    AddDiagnostic(&result, kSysInformationDiagnosticViewUnsupported);
  }
  if (FindSysInformationProjectionDefinition(
          "sys.information.scratchbird_index_profiles") == nullptr) {
    AddDiagnostic(&result, kSysInformationDiagnosticViewUnsupported);
  }
  if (FindSysInformationProjectionDefinition(
          "sys.information.scratchbird_datatype_descriptors") == nullptr) {
    AddDiagnostic(&result, kSysInformationDiagnosticViewUnsupported);
  }
  if (FindSysInformationProjectionDefinition("sys.information_schema.tables") == nullptr) {
    AddDiagnostic(&result, kSysInformationDiagnosticViewUnsupported);
  }
  return result;
}

SysInformationProjectionResult BuildSysInformationProjection(
    std::string_view view_path,
    const SysInformationProjectionContext& context,
    const std::vector<SysInformationCatalogObjectSource>& catalog_objects,
    const std::vector<SysInformationResolverNameSource>& resolver_names,
    const std::vector<SysInformationCommentSource>& comments,
    const std::vector<SysInformationDatatypeDescriptorSource>& datatype_descriptors,
    const std::vector<SysInformationColumnSource>& columns,
    const std::vector<SysInformationSettingSource>& settings,
    const std::vector<SysInformationFrontendAgentSource>& frontend_agents,
    const std::vector<SysInformationProtectedMaterialSource>& protected_material,
    const std::vector<SysInformationProtectedMaterialVersionSource>& protected_material_versions,
    const std::vector<SysInformationAgentSource>& agents,
    const std::vector<SysInformationAgentMetricDependencySource>& agent_metric_dependencies,
    const std::vector<SysInformationAgentPolicySource>& agent_policies,
    const std::vector<SysInformationAgentActionSource>& agent_actions,
    const std::vector<SysInformationAgentOverrideSource>& agent_overrides,
    const std::vector<SysInformationAgentEvidenceSource>& agent_evidence,
    const std::vector<SysInformationAgentAuditSource>& agent_audit,
    const std::vector<SysInformationFilespaceCapacityAgentStateSource>& filespace_capacity_agent_state,
    const std::vector<SysInformationPageAllocationAgentStateSource>& page_allocation_agent_state,
    const std::vector<SysInformationFilespaceShrinkReadinessSource>& filespace_shrink_readiness,
    const std::vector<SysInformationDomainSource>& domains,
    const std::vector<SysInformationIparAgentLifecycleSource>& ipar_agent_lifecycle,
    const std::vector<SysInformationIparMetricCounterSource>& ipar_metric_counters,
    const std::vector<SysInformationIparTelemetryControlSource>& ipar_telemetry_controls,
    const std::vector<SysInformationIparSlowPathReasonSource>& ipar_slow_path_reasons,
    const std::vector<SysInformationIparContentionQuotaSource>& ipar_contention_quota) {
  if (SysInformationPathIsClusterScoped(view_path)) {
    return Failure(kSysInformationDiagnosticClusterScopeForbidden, std::string(view_path));
  }
  const std::string canonical_view_path = SysInformationCanonicalViewPath(view_path);
  const auto* definition = FindSysInformationProjectionDefinition(canonical_view_path);
  if (definition == nullptr || !IsSupportedProjection(canonical_view_path)) {
    return Failure(kSysInformationDiagnosticViewUnsupported, std::string(view_path));
  }
  const auto definition_check = ValidateSysInformationProjectionDefinition(*definition);
  if (!definition_check.ok) {
    return Failure(definition_check.diagnostic_codes.front(), definition->view_path);
  }

  SysInformationProjectionResult result;

  if (canonical_view_path == "sys.information.schemata") {
    for (const auto& object : catalog_objects) {
      if (object.object_class != "schema" || !ObjectVisible(object, context)) { continue; }
      bool found_schema = false;
      const std::string schema_name = SelectResolverDisplayName(
          resolver_names, context, object.object_uuid, "schema", &found_schema);
      if (!found_schema) {
        if (context.strict_mode) {
          return Failure(kSysInformationDiagnosticNameNotFound, object.object_uuid);
        }
        continue;
      }
      SysInformationProjectionRow row;
      AddField(&row, "catalog_name", context.catalog_display_name);
      AddField(&row, "schema_name", schema_name);
      AddField(&row, "schema_owner", "");
      AddField(&row, "default_character_set_catalog", "");
      AddField(&row, "default_character_set_schema", "");
      AddField(&row, "default_character_set_name", "");
      AddField(&row, "sql_path", "");
      result.rows.push_back(std::move(row));
    }
    return result;
  }

  if (canonical_view_path == "sys.information.tables") {
    for (const auto& object : catalog_objects) {
      if (!IsTableLikeObject(object) || !ObjectVisible(object, context)) { continue; }
      bool found_table = false;
      bool found_schema = false;
      const std::string table_name = SelectResolverDisplayName(
          resolver_names, context, object.object_uuid, object.object_class, &found_table);
      const std::string schema_name = SelectResolverDisplayName(
          resolver_names, context, object.schema_uuid, "schema", &found_schema);
      if (!found_table || !found_schema) {
        if (context.strict_mode) {
          return Failure(kSysInformationDiagnosticNameNotFound,
                         found_table ? object.schema_uuid : object.object_uuid);
        }
        continue;
      }
      SysInformationProjectionRow row;
      AddField(&row, "table_catalog", context.catalog_display_name);
      AddField(&row, "table_schema", schema_name);
      AddField(&row, "table_name", table_name);
      AddField(&row, "table_type", TableTypeForObject(object));
      AddField(&row, "self_referencing_column_name", "");
      AddField(&row, "reference_generation", "");
      AddField(&row, "user_defined_type_catalog", "");
      AddField(&row, "user_defined_type_schema", "");
      AddField(&row, "user_defined_type_name", "");
      AddField(&row, "is_insertable_into", "YES");
      AddField(&row, "is_typed", "NO");
      AddField(&row, "commit_action", CommitActionForObject(object));
      result.rows.push_back(std::move(row));
    }
    return result;
  }

  if (canonical_view_path == "sys.information.views") {
    for (const auto& object : catalog_objects) {
      if (object.object_class != "view" || !ObjectVisible(object, context)) { continue; }
      bool found_view = false;
      bool found_schema = false;
      const std::string view_name = ObjectDisplayName(resolver_names, context, object, &found_view);
      const std::string schema_name = SchemaDisplayName(resolver_names, context, object, &found_schema);
      if (!found_view || !found_schema) {
        if (context.strict_mode) {
          return Failure(kSysInformationDiagnosticNameNotFound,
                         found_view ? object.schema_uuid : object.object_uuid);
        }
        continue;
      }
      SysInformationProjectionRow row;
      AddField(&row, "table_catalog", context.catalog_display_name);
      AddField(&row, "table_schema", schema_name);
      AddField(&row, "table_name", view_name);
      AddField(&row, "view_definition", "");
      result.rows.push_back(std::move(row));
    }
    return result;
  }

  if (canonical_view_path == "sys.information.columns") {
    for (const auto& column : columns) {
      if (!ProjectionSourceVisible(column.hidden, column.catalog_generation_id, context)) { continue; }
      const auto* relation = FindObject(catalog_objects, column.relation_object_uuid, context);
      if (relation == nullptr || !IsTableLikeObject(*relation)) {
        if (context.strict_mode) {
          return Failure(kSysInformationDiagnosticNameNotFound, column.relation_object_uuid);
        }
        continue;
      }
      bool found_relation = false;
      bool found_schema = false;
      const std::string relation_name = ObjectDisplayName(
          resolver_names, context, *relation, &found_relation);
      const std::string schema_name = SchemaDisplayName(
          resolver_names, context, *relation, &found_schema);
      if (!found_relation || !found_schema) {
        if (context.strict_mode) {
          return Failure(kSysInformationDiagnosticNameNotFound,
                         found_relation ? relation->schema_uuid : relation->object_uuid);
        }
        continue;
      }
      SysInformationProjectionRow row;
      AddField(&row, "table_catalog", context.catalog_display_name);
      AddField(&row, "table_schema", schema_name);
      AddField(&row, "table_name", relation_name);
      AddField(&row, "column_name", column.column_name);
      AddField(&row, "ordinal_position", std::to_string(column.ordinal_position));
      AddField(&row, "column_default", column.column_default);
      AddField(&row, "is_nullable", column.is_nullable);
      AddField(&row, "data_type", column.datatype_name);
      AddField(&row, "character_maximum_length", "");
      AddField(&row, "character_octet_length", "");
      AddField(&row, "numeric_precision", "");
      AddField(&row, "numeric_precision_radix", "");
      AddField(&row, "numeric_scale", "");
      AddField(&row, "datetime_precision", "");
      AddField(&row, "interval_type", "");
      AddField(&row, "interval_precision", "");
      AddField(&row, "character_set_catalog", "");
      AddField(&row, "character_set_schema", "");
      AddField(&row, "character_set_name", "");
      AddField(&row, "collation_catalog", "");
      AddField(&row, "collation_schema", "");
      AddField(&row, "collation_name", "");
      AddField(&row, "domain_catalog", column.domain_name.empty() ? "" : context.catalog_display_name);
      AddField(&row, "domain_schema", column.domain_name.empty() ? "" : schema_name);
      AddField(&row, "domain_name", column.domain_name);
      AddField(&row, "is_identity", "NO");
      AddField(&row, "identity_generation", "");
      AddField(&row, "identity_start", "");
      AddField(&row, "identity_increment", "");
      AddField(&row, "identity_maximum", "");
      AddField(&row, "identity_minimum", "");
      AddField(&row, "identity_cycle", "NO");
      AddField(&row, "is_generated", "NEVER");
      AddField(&row, "generation_expression", "");
      AddField(&row, "is_updatable", "YES");
      result.rows.push_back(std::move(row));
    }
    return result;
  }

	  if (canonical_view_path == "sys.information.parameters") {
	    return result;
	  }

	  if (canonical_view_path == "sys.information.sequences") {
	    for (const auto& object : catalog_objects) {
	      if (object.object_class != "sequence" || !ObjectVisible(object, context)) { continue; }
	      bool found_sequence = false;
	      bool found_schema = false;
	      const std::string sequence_name = ObjectDisplayName(
	          resolver_names, context, object, &found_sequence);
	      const std::string schema_name = SchemaDisplayName(
	          resolver_names, context, object, &found_schema);
	      if (!found_sequence || !found_schema) {
	        if (context.strict_mode) {
	          return Failure(kSysInformationDiagnosticNameNotFound,
	                         found_sequence ? object.schema_uuid : object.object_uuid);
	        }
	        continue;
	      }
	      SysInformationProjectionRow row;
	      AddField(&row, "sequence_schema", schema_name);
	      AddField(&row, "sequence_name", sequence_name);
	      AddField(&row, "data_type", "int64");
	      AddField(&row, "start_value", "");
	      AddField(&row, "increment", "");
	      AddField(&row, "cycle_option", "");
	      result.rows.push_back(std::move(row));
	    }
	    return result;
	  }

	  if (canonical_view_path == "sys.schemas") {
	    for (const auto& object : catalog_objects) {
      if (object.object_class != "schema" || !ObjectVisible(object, context)) { continue; }
      bool found_schema = false;
      const std::string schema_name = SelectResolverDisplayName(
          resolver_names, context, object.object_uuid, "schema", &found_schema);
      if (!found_schema) {
        if (context.strict_mode) {
          return Failure(kSysInformationDiagnosticNameNotFound, object.object_uuid);
        }
        continue;
      }
      SysInformationProjectionRow row;
      AddField(&row, "schema_id", object.object_uuid);
      AddField(&row, "schema_name", schema_name);
      AddField(&row, "parent_schema_id", object.parent_object_uuid);
      AddField(&row, "schema_owner", "");
      AddField(&row, "visibility_state", "visible");
      result.rows.push_back(std::move(row));
    }
    return result;
  }

  if (canonical_view_path == "sys.tables") {
    for (const auto& object : catalog_objects) {
      if (!IsTableLikeObject(object) || !ObjectVisible(object, context)) { continue; }
      bool found_table = false;
      const std::string table_name = SelectResolverDisplayName(
          resolver_names, context, object.object_uuid, object.object_class, &found_table);
      if (!found_table) {
        if (context.strict_mode) {
          return Failure(kSysInformationDiagnosticNameNotFound, object.object_uuid);
        }
        continue;
      }
      SysInformationProjectionRow row;
      AddField(&row, "table_id", object.object_uuid);
      AddField(&row, "schema_id", object.schema_uuid);
      AddField(&row, "table_name", table_name);
      AddField(&row, "table_type", TableTypeForObject(object));
      AddField(&row, "is_insertable_into", "YES");
      AddField(&row, "visibility_state", "visible");
      result.rows.push_back(std::move(row));
    }
    return result;
  }

  if (canonical_view_path == "sys.columns") {
    for (const auto& column : columns) {
      if (!ProjectionSourceVisible(column.hidden, column.catalog_generation_id, context)) { continue; }
      const auto* relation = FindObject(catalog_objects, column.relation_object_uuid, context);
      if (relation == nullptr || !IsTableLikeObject(*relation)) {
        if (context.strict_mode) {
          return Failure(kSysInformationDiagnosticNameNotFound, column.relation_object_uuid);
        }
        continue;
      }
      const std::string column_id =
          column.relation_object_uuid + ":" + std::to_string(column.ordinal_position);
      SysInformationProjectionRow row;
      AddField(&row, "column_id", column_id);
      AddField(&row, "table_id", column.relation_object_uuid);
      AddField(&row, "schema_id", relation->schema_uuid.empty() ? column.schema_uuid
                                                                 : relation->schema_uuid);
      AddField(&row, "column_name", column.column_name);
      AddField(&row, "ordinal_position", std::to_string(column.ordinal_position));
      AddField(&row, "data_type", column.datatype_name);
      AddField(&row, "is_nullable", column.is_nullable);
      AddField(&row, "column_default", column.column_default);
      AddField(&row, "visibility_state", "visible");
      result.rows.push_back(std::move(row));
    }
    return result;
  }

  if (canonical_view_path == "sys.catalog.domain_descriptor") {
    for (const auto& domain : domains) {
      if (!ProjectionSourceVisible(domain.hidden, domain.catalog_generation_id, context)) {
        continue;
      }
      const auto* object = FindObject(catalog_objects, domain.domain_uuid, context);
      if (object == nullptr || object->object_class != "domain") {
        if (context.strict_mode) {
          return Failure(kSysInformationDiagnosticNameNotFound, domain.domain_uuid);
        }
        continue;
      }
      SysInformationProjectionRow row;
      AddField(&row, "row_uuid", domain.row_uuid.empty() ? domain.domain_uuid : domain.row_uuid);
      AddField(&row, "domain_uuid", domain.domain_uuid);
      AddField(&row, "schema_uuid", domain.schema_uuid);
      AddField(&row, "source_type_name", domain.source_type_name);
      AddField(&row, "base_type_name", domain.base_type_name);
      AddField(&row, "domain_kind", domain.domain_kind.empty() ? "scalar" : domain.domain_kind);
      AddField(&row, "nullable", domain.nullable);
      AddField(&row, "default_expression_envelope", domain.default_expression_envelope);
      AddField(&row, "check_constraint_envelope", domain.check_constraint_envelope);
      AddField(&row, "catalog_generation_id", std::to_string(domain.catalog_generation_id));
      AddField(&row,
               "created_local_transaction_id",
               std::to_string(domain.created_local_transaction_id));
      AddField(&row, "lifecycle_state", "active");
      result.rows.push_back(std::move(row));
    }
    return result;
  }

  if (canonical_view_path == "sys.information.enabled_roles") {
    const auto role_uuids = EffectiveRoleUuids(context);
    for (std::size_t index = 0; index < role_uuids.size(); ++index) {
      const std::string role_name = RoleDisplayNameAt(context, index, role_uuids[index]);
      if (role_name.empty()) { continue; }
      SysInformationProjectionRow row;
      AddField(&row, "role_name", role_name);
      AddField(&row,
               "is_default",
               !context.active_role_uuid.empty() &&
                       EqualsInsensitiveAscii(context.active_role_uuid, role_uuids[index])
                   ? "YES"
                   : "NO");
      AddField(&row,
               "enabled_by",
               !context.requested_role_name.empty() ? "requested_role" : "session_default");
      result.rows.push_back(std::move(row));
    }
    return result;
  }

  if (canonical_view_path == "sys.information.applicable_roles") {
    const auto role_uuids = EffectiveRoleUuids(context);
    for (std::size_t index = 0; index < role_uuids.size(); ++index) {
      const std::string role_name = RoleDisplayNameAt(context, index, role_uuids[index]);
      if (role_name.empty()) { continue; }
      SysInformationProjectionRow row;
      AddField(&row, "grantee", PrincipalDisplayName(context));
      AddField(&row, "role_name", role_name);
      AddField(&row, "is_grantable", "NO");
      result.rows.push_back(std::move(row));
    }
    return result;
  }

  if (canonical_view_path == "sys.information.domains" ||
      canonical_view_path == "sys.catalog_readable.domains") {
    for (const auto& domain : domains) {
      if (!ProjectionSourceVisible(domain.hidden, domain.catalog_generation_id, context)) {
        continue;
      }
      const auto* object = FindObject(catalog_objects, domain.domain_uuid, context);
      if (object == nullptr || object->object_class != "domain") { continue; }
      bool found_name = false;
      bool found_schema = false;
      const std::string domain_name =
          ObjectDisplayName(resolver_names, context, *object, &found_name);
      const std::string schema_name =
          SchemaDisplayName(resolver_names, context, *object, &found_schema);
      if (!found_name || !found_schema) {
        if (context.strict_mode) {
          return Failure(kSysInformationDiagnosticNameNotFound,
                         found_name ? object->schema_uuid : object->object_uuid);
        }
        continue;
      }
      SysInformationProjectionRow row;
      if (canonical_view_path == "sys.information.domains") {
        AddField(&row, "domain_catalog", context.catalog_display_name);
        AddField(&row, "domain_schema", schema_name);
        AddField(&row, "domain_name", domain_name);
        AddField(&row, "data_type", domain.base_type_name);
      } else {
        AddField(&row, "domain_path", domain.source_type_name);
        AddField(&row, "domain_name", domain_name);
        AddField(&row, "base_type_name", domain.base_type_name);
        AddField(&row, "domain_kind", domain.domain_kind.empty() ? "scalar" : domain.domain_kind);
        AddField(&row, "status", "active");
        AddField(&row, "comment_text",
                 SelectCommentText(comments, context, domain.domain_uuid, "domain"));
        AddField(&row, "visibility_state", "visible");
      }
      result.rows.push_back(std::move(row));
    }
    return result;
  }

  if (canonical_view_path == "sys.information.domain_constraints") {
    for (const auto& domain : domains) {
      if (domain.check_constraint_envelope.empty() ||
          !ProjectionSourceVisible(domain.hidden, domain.catalog_generation_id, context)) {
        continue;
      }
      const auto* object = FindObject(catalog_objects, domain.domain_uuid, context);
      if (object == nullptr || object->object_class != "domain") { continue; }
      bool found_name = false;
      bool found_schema = false;
      const std::string domain_name =
          ObjectDisplayName(resolver_names, context, *object, &found_name);
      const std::string schema_name =
          SchemaDisplayName(resolver_names, context, *object, &found_schema);
      if (!found_name || !found_schema) { continue; }
      SysInformationProjectionRow row;
      AddField(&row, "constraint_schema", schema_name);
      AddField(&row, "constraint_name", domain_name + "_check");
      AddField(&row, "domain_schema", schema_name);
      AddField(&row, "domain_name", domain_name);
      result.rows.push_back(std::move(row));
    }
    return result;
  }

  if (canonical_view_path == "sys.catalog_readable.navigator_tree") {
    return BuildNavigatorTreeProjection(context, catalog_objects, resolver_names, columns);
  }

  if (canonical_view_path == "sys.catalog_readable.objects" ||
      canonical_view_path == "sys.catalog_readable.object_tree" ||
      canonical_view_path == "sys.catalog_readable.schemas" ||
      canonical_view_path == "sys.catalog_readable.relations") {
    for (const auto& object : catalog_objects) {
      if (!ObjectVisible(object, context)) { continue; }
      if (canonical_view_path == "sys.catalog_readable.schemas" &&
          object.object_class != "schema") {
        continue;
      }
      if (canonical_view_path == "sys.catalog_readable.relations" &&
          !IsTableLikeObject(object)) {
        continue;
      }
      bool found_object = false;
      bool found_path = false;
      const std::string object_name = ObjectDisplayName(resolver_names, context, object, &found_object);
      const std::string object_path =
          canonical_view_path == "sys.catalog_readable.object_tree"
              ? TreeDisplayPath(catalog_objects, resolver_names, context, object, &found_path)
              : ObjectDisplayPath(resolver_names, context, object, &found_path);
      if (!found_object || !found_path) {
        if (context.strict_mode) {
          return Failure(kSysInformationDiagnosticNameNotFound, object.object_uuid);
        }
        continue;
      }
      const std::string comment = SelectCommentText(
          comments, context, object.object_uuid, object.object_class);
      SysInformationProjectionRow row;
      if (canonical_view_path == "sys.catalog_readable.object_tree") {
        bool found_parent_path = false;
        const std::string parent_path = TreeParentPath(
            catalog_objects, resolver_names, context, object, &found_parent_path);
        if (!found_parent_path) {
          if (context.strict_mode) {
            return Failure(kSysInformationDiagnosticNameNotFound, TreeParentObjectId(object));
          }
          continue;
        }
        const std::string parent_id = TreeParentObjectId(object);
        AddField(&row, "object_id", object.object_uuid);
        AddField(&row, "parent_object_id", parent_id);
        AddField(&row, "object_path", object_path);
        AddField(&row, "object_name", object_name);
        AddField(&row, "object_kind", object.object_class);
        AddField(&row, "parent_path", parent_path);
        AddField(&row, "depth", std::to_string(TreeDepth(catalog_objects, context, object)));
        AddField(&row, "schema_path", parent_path);
      } else if (canonical_view_path == "sys.catalog_readable.objects") {
        AddField(&row, "object_path", object_path);
        AddField(&row, "object_name", object_name);
        AddField(&row, "object_kind", object.object_class);
        AddField(&row, "parent_path", "");
      } else if (canonical_view_path == "sys.catalog_readable.schemas") {
        AddField(&row, "schema_path", object_path);
        AddField(&row, "schema_name", object_name);
        AddField(&row, "parent_path", "");
      } else {
        bool found_schema = false;
        const std::string schema_name = SchemaDisplayName(resolver_names, context, object, &found_schema);
        AddField(&row, "relation_path", object_path);
        AddField(&row, "relation_name", object_name);
        AddField(&row, "relation_kind", object.object_class);
        AddField(&row, "schema_path", schema_name);
      }
      AddField(&row, "status", "active");
      AddField(&row, "comment_text", comment);
      AddField(&row, "visibility_state", "visible");
      result.rows.push_back(std::move(row));
    }
    return result;
  }

  if (canonical_view_path == "sys.security.roles" ||
      canonical_view_path == "sys.security.principals" ||
      canonical_view_path == "sys.security.policies" ||
      canonical_view_path == "sys.security.masks" ||
      canonical_view_path == "sys.security.rls") {
    for (const auto& object : catalog_objects) {
      if (!ObjectVisible(object, context)) { continue; }
      const bool role_object =
          object.object_class == "role" || object.object_class == "security_role";
      const bool principal_object =
          role_object ||
          object.object_class == "group" ||
          object.object_class == "security_group" ||
          object.object_class == "principal" ||
          object.object_class == "security_principal" ||
          object.object_class == "user";
      const bool policy_object =
          object.object_class == "policy" || object.object_class == "security_policy";
      const bool mask_object =
          object.object_class == "mask" || object.object_class == "security_mask";
      const bool rls_object =
          object.object_class == "rls" || object.object_class == "security_rls";
      if ((canonical_view_path == "sys.security.roles" && !role_object) ||
          (canonical_view_path == "sys.security.principals" && !principal_object) ||
          (canonical_view_path == "sys.security.policies" && !policy_object) ||
          (canonical_view_path == "sys.security.masks" && !mask_object) ||
          (canonical_view_path == "sys.security.rls" && !rls_object)) {
        continue;
      }
      bool found_name = false;
      bool found_path = false;
      const std::string object_name =
          ObjectDisplayName(resolver_names, context, object, &found_name);
      const std::string object_path =
          ObjectDisplayPath(resolver_names, context, object, &found_path);
      if (!found_name) {
        if (context.strict_mode) {
          return Failure(kSysInformationDiagnosticNameNotFound, object.object_uuid);
        }
        continue;
      }
      SysInformationProjectionRow row;
      if (canonical_view_path == "sys.security.roles") {
        AddField(&row, "role_name", object_name);
        AddField(&row, "status", "active");
        AddField(&row, "visibility_state", "visible");
      } else if (canonical_view_path == "sys.security.principals") {
        AddField(&row, "principal_name", object_name);
        AddField(&row, "principal_kind", role_object ? "role" : object.object_class);
        AddField(&row, "status", "active");
        AddField(&row, "visibility_state", "visible");
      } else if (canonical_view_path == "sys.security.policies") {
        AddField(&row, "policy_name", object_name);
        AddField(&row, "policy_kind", object.object_class);
        AddField(&row, "status", "active");
        AddField(&row, "visibility_state", "visible");
      } else if (canonical_view_path == "sys.security.masks") {
        AddField(&row, "mask_name", object_name);
        AddField(&row, "target_path", found_path ? object_path : "");
        AddField(&row, "status", "active");
        AddField(&row, "visibility_state", "visible");
      } else {
        AddField(&row, "rls_name", object_name);
        AddField(&row, "target_path", found_path ? object_path : "");
        AddField(&row, "status", "active");
        AddField(&row, "visibility_state", "visible");
      }
      result.rows.push_back(std::move(row));
    }
    return result;
  }

  if (canonical_view_path == "sys.catalog_readable.filespaces" ||
      canonical_view_path == "sys.information.scratchbird_filespaces") {
    for (const auto& object : catalog_objects) {
      if (object.object_class != "filespace" || !ObjectVisible(object, context)) { continue; }
      bool found_name = false;
      const std::string filespace_name =
          ObjectDisplayName(resolver_names, context, object, &found_name);
      if (!found_name) {
        if (context.strict_mode) {
          return Failure(kSysInformationDiagnosticNameNotFound, object.object_uuid);
        }
        continue;
      }
      SysInformationProjectionRow row;
      if (canonical_view_path == "sys.catalog_readable.filespaces") {
        AddField(&row, "filespace_path", filespace_name);
        AddField(&row, "filespace_name", filespace_name);
        AddField(&row, "role", "database_filespace");
        AddField(&row, "status", "active");
        AddField(&row, "size_metrics", "redacted");
        AddField(&row,
                 "comment_text",
                 SelectCommentText(comments, context, object.object_uuid, object.object_class));
        AddField(&row, "visibility_state", "visible");
      } else {
        AddField(&row, "filespace_name", filespace_name);
        AddField(&row, "role", "database_filespace");
        AddField(&row, "status", "active");
      }
      result.rows.push_back(std::move(row));
    }
    return result;
  }

  if (canonical_view_path == "sys.catalog_readable.object_names") {
    for (const auto& name : resolver_names) {
      if (!ResolverVisible(name, context)) { continue; }
      const auto* object = FindObject(catalog_objects, name.object_uuid, context);
      if (object == nullptr) { continue; }
      bool found_path = false;
      const std::string object_path = ObjectDisplayPath(resolver_names, context, *object, &found_path);
      if (!found_path) { continue; }
      SysInformationProjectionRow row;
      AddField(&row, "object_path", object_path);
      AddField(&row, "object_name", name.display_name);
      AddField(&row, "name_language", name.language_tag);
      AddField(&row, "visibility_state", "visible");
      result.rows.push_back(std::move(row));
    }
    return result;
  }

  if (canonical_view_path == "sys.catalog_readable.object_comments") {
    for (const auto& comment : comments) {
      if (!ProjectionSourceVisible(comment.hidden, comment.catalog_generation_id, context)) { continue; }
      const auto* object = FindObject(catalog_objects, comment.object_uuid, context);
      if (object == nullptr) { continue; }
      bool found_path = false;
      bool found_name = false;
      const std::string object_path = ObjectDisplayPath(resolver_names, context, *object, &found_path);
      const std::string object_name = ObjectDisplayName(resolver_names, context, *object, &found_name);
      if (!found_path || !found_name) { continue; }
      SysInformationProjectionRow row;
      AddField(&row, "object_path", object_path);
      AddField(&row, "object_name", object_name);
      AddField(&row, "comment_text", comment.comment_text);
      AddField(&row, "comment_language", comment.language_tag);
      AddField(&row, "visibility_state", "visible");
      result.rows.push_back(std::move(row));
    }
    return result;
  }

  if (canonical_view_path == "sys.catalog_readable.columns") {
    for (const auto& column : columns) {
      if (!ProjectionSourceVisible(column.hidden, column.catalog_generation_id, context)) { continue; }
      const auto* relation = FindObject(catalog_objects, column.relation_object_uuid, context);
      if (relation == nullptr || !IsTableLikeObject(*relation)) { continue; }
      bool found_path = false;
      const std::string relation_path = ObjectDisplayPath(resolver_names, context, *relation, &found_path);
      if (!found_path) { continue; }
      SysInformationProjectionRow row;
      AddField(&row, "relation_path", relation_path);
      AddField(&row, "column_name", column.column_name);
      AddField(&row, "ordinal_position", std::to_string(column.ordinal_position));
      AddField(&row, "datatype_name", column.datatype_name);
      AddField(&row, "domain_name", column.domain_name);
      AddField(&row, "is_nullable", column.is_nullable);
      AddField(&row, "comment_text", column.comment_text);
      AddField(&row, "visibility_state", "visible");
      result.rows.push_back(std::move(row));
    }
    return result;
  }

  if (canonical_view_path == "sys.catalog_readable.datatypes") {
    const auto builtin_sources = datatype_descriptors.empty()
                                     ? BuiltinDatatypeDescriptorSources(context)
                                     : std::vector<SysInformationDatatypeDescriptorSource>{};
    const auto& sources = datatype_descriptors.empty() ? builtin_sources : datatype_descriptors;
    for (const auto& source : sources) {
      if (!DatatypeDescriptorVisible(source, context)) { continue; }
      SysInformationProjectionRow row;
      AddField(&row, "type_path", source.type_schema.empty()
                                    ? source.type_name
                                    : source.type_schema + "." + source.type_name);
      AddField(&row, "type_name", source.type_name);
      AddField(&row, "type_family", source.canonical_type_family);
      AddField(&row, "storage_class", source.driver_family);
      AddField(&row, "wire_class", source.canonical_type_code);
      AddField(&row, "status", source.support_state);
      AddField(&row, "visibility_state", "visible");
      result.rows.push_back(std::move(row));
    }
    return result;
  }

  if (canonical_view_path == "sys.catalog_readable.indexes") {
    for (const auto& profile : BuiltinCatalogIndexProfiles()) {
      SysInformationProjectionRow row;
      AddField(&row, "index_path", profile.index_name);
      AddField(&row, "index_name", profile.index_name);
      AddField(&row, "relation_path", profile.table_path);
      AddField(&row, "index_family", CatalogIndexMethodName(profile.method));
      AddField(&row, "storage_profile_name", CatalogIndexPurposeName(profile.purpose));
      AddField(&row, "comment_text", "");
      AddField(&row, "visibility_state", "visible");
      result.rows.push_back(std::move(row));
    }
    return result;
  }

  if (canonical_view_path == "sys.catalog_readable.settings") {
    for (const auto& setting : settings) {
      if (!ProjectionSourceVisible(setting.hidden, setting.catalog_generation_id, context)) { continue; }
      SysInformationProjectionRow row;
      AddField(&row, "setting_name", setting.setting_name);
      AddField(&row, "setting_value_display", setting.setting_value_display);
      AddField(&row, "authority", setting.authority);
      AddField(&row, "default_source", setting.default_source);
      AddField(&row, "redaction_state", setting.redaction_state);
      AddField(&row, "visibility_state", setting.visibility_state);
      result.rows.push_back(std::move(row));
    }
    return result;
  }

  if (canonical_view_path == "sys.configuration.settings" ||
      canonical_view_path == "sys.configuration.effective_settings") {
    for (const auto& setting : settings) {
      if (!ProjectionSourceVisible(setting.hidden, setting.catalog_generation_id, context)) { continue; }
      SysInformationProjectionRow row;
      if (canonical_view_path == "sys.configuration.settings") {
        AddField(&row, "setting_name", setting.setting_name);
        AddField(&row, "setting_value", setting.setting_value_display);
        AddField(&row, "source_scope", setting.default_source);
        AddField(&row, "effective_state", setting.visibility_state);
      } else {
        AddField(&row, "setting_name", setting.setting_name);
        AddField(&row, "effective_value", setting.setting_value_display);
        AddField(&row, "resolved_from", setting.authority + ":" + setting.default_source);
        AddField(&row, "policy_hash", setting.redaction_state);
      }
      result.rows.push_back(std::move(row));
    }
    return result;
  }

  if (canonical_view_path == "sys.configuration.profiles") {
    return result;
  }

  if (canonical_view_path == "sys.configuration.policy_bindings") {
    std::size_t ordinal = 1;
    for (const auto& policy : agent_policies) {
      if (!ProjectionSourceVisible(policy.hidden, policy.catalog_generation_id, context)) { continue; }
      SysInformationProjectionRow row;
      AddField(&row,
               "binding_uuid",
               StableRef(policy.version_uuid,
                         "agent_policy_binding_" + std::to_string(ordinal++)));
      AddField(&row, "profile_uuid", StableRef(policy.agent_uuid, policy.agent_ref));
      AddField(&row, "policy_uuid", StableRef(policy.policy_uuid, policy.policy_ref));
      AddField(&row, "binding_state", policy.active_state);
      result.rows.push_back(std::move(row));
    }
    for (const auto& state : filespace_capacity_agent_state) {
      if (!ProjectionSourceVisible(state.hidden, state.catalog_generation_id, context)) { continue; }
      SysInformationProjectionRow row;
      AddField(&row, "binding_uuid", "filespace_capacity_policy_binding_" + std::to_string(ordinal++));
      AddField(&row, "profile_uuid", StableRef(state.agent_uuid, state.agent_ref));
      AddField(&row, "policy_uuid", StableRef(state.policy_uuid, state.policy_ref));
      AddField(&row, "binding_state", state.mode);
      result.rows.push_back(std::move(row));
    }
    for (const auto& state : page_allocation_agent_state) {
      if (!ProjectionSourceVisible(state.hidden, state.catalog_generation_id, context)) { continue; }
      SysInformationProjectionRow row;
      AddField(&row, "binding_uuid", "page_allocation_policy_binding_" + std::to_string(ordinal++));
      AddField(&row, "profile_uuid", StableRef(state.agent_uuid, state.agent_ref));
      AddField(&row, "policy_uuid", StableRef(state.policy_uuid, state.policy_ref));
      AddField(&row, "binding_state", state.mode);
      result.rows.push_back(std::move(row));
    }
    return result;
  }

  if (canonical_view_path == "sys.parser.dialects") {
    SysInformationProjectionRow row;
    AddField(&row, "dialect_name", "SBsql");
    AddField(&row, "base_dialect", "SBsql");
    AddField(&row, "compatibility_state", "supported");
    AddField(&row, "parser_family", "native_sbsql");
    result.rows.push_back(std::move(row));
    return result;
  }

  if (canonical_view_path == "sys.parser.language_elements") {
    for (const auto& element : SbsqlLanguageElementCatalogRows()) {
      SysInformationProjectionRow row;
      AddField(&row, "surface_id", std::string(element.surface_id));
      AddField(&row, "canonical_name", std::string(element.canonical_name));
      AddField(&row, "element_kind", std::string(element.element_kind));
      AddField(&row, "surface_kind", std::string(element.surface_kind));
      AddField(&row, "family", std::string(element.family));
      AddField(&row, "sblr_operation_family", std::string(element.sblr_operation_family));
      AddField(&row, "support_state", std::string(element.support_state));
      AddField(&row, "release_status", std::string(element.release_status));
      AddField(&row, "predictive_state", std::string(element.predictive_state));
      AddField(&row, "keyword_text", std::string(element.keyword_text));
      AddField(&row, "keyword_class", std::string(element.keyword_class));
      result.rows.push_back(std::move(row));
    }
    return result;
  }

  if (canonical_view_path == "sys.agents") {
    for (const auto& agent : agents) {
      if (!ProjectionSourceVisible(agent.hidden, agent.catalog_generation_id, context)) { continue; }
      SysInformationProjectionRow row;
      AddField(&row, "agent_uuid", VisibleUuid(agent.agent_uuid, "<redacted:agent_uuid>"));
      AddField(&row, "agent_type_id", agent.agent_type_id);
      AddField(&row, "scope_kind", agent.scope_kind);
      AddField(&row, "scope_uuid",
               VisibleUuid(agent.scope_uuid, "<redacted:scope_uuid>", agent.scope_visible));
      AddField(&row, "component", agent.component);
      AddField(&row, "state", agent.state);
      AddField(&row, "health_state", agent.health_state);
      AddField(&row, "enabled", agent.enabled);
      AddField(&row, "policy_uuid", VisibleUuid(agent.policy_uuid, "<redacted:policy_uuid>"));
      AddField(&row, "last_transition_at", agent.last_transition_at);
      AddField(&row, "last_diagnostic_code", agent.last_diagnostic_code);
      AddField(&row, "last_evidence_uuid",
               VisibleUuid(agent.last_evidence_uuid, "<redacted:last_evidence_uuid>"));
      AddField(&row, "policy_generation", std::to_string(agent.policy_generation));
      AddField(&row, "queue_depth", std::to_string(agent.queue_depth));
      AddField(&row, "action_backlog", std::to_string(agent.action_backlog));
      AddField(&row, "failure_count", std::to_string(agent.failure_count));
      AddField(&row, "quarantine_count", std::to_string(agent.quarantine_count));
      AddField(&row, "retry_not_before", agent.retry_not_before);
      AddField(&row, "last_decision", agent.last_decision);
      AddField(&row, "overhead_budget_units", std::to_string(agent.overhead_budget_units));
      AddField(&row, "diagnostic_redaction_state", agent.diagnostic_redaction_state);
      result.rows.push_back(std::move(row));
    }
    return result;
  }

  if (canonical_view_path == "sys.agent_metric_dependencies") {
    for (const auto& dependency : agent_metric_dependencies) {
      if (!ProjectionSourceVisible(dependency.hidden, dependency.catalog_generation_id, context)) {
        continue;
      }
      SysInformationProjectionRow row;
      AddField(&row, "agent_uuid",
               VisibleUuid(dependency.agent_uuid, "<redacted:agent_uuid>"));
      AddField(&row, "metric_family", dependency.metric_family);
      AddField(&row, "namespace", dependency.metric_namespace);
      AddField(&row, "required_or_optional", dependency.required_or_optional);
      AddField(&row, "freshness_limit", dependency.freshness_limit);
      AddField(&row, "current_freshness",
               dependency.metric_values_visible ? dependency.current_freshness : "<redacted>");
      AddField(&row, "quality_state", dependency.quality_state);
      AddField(&row, "fail_behavior", dependency.fail_behavior);
      result.rows.push_back(std::move(row));
    }
    return result;
  }

  if (canonical_view_path == "sys.agent_policies") {
    for (const auto& policy : agent_policies) {
      if (!ProjectionSourceVisible(policy.hidden, policy.catalog_generation_id, context)) { continue; }
      SysInformationProjectionRow row;
      AddField(&row, "agent_uuid", VisibleUuid(policy.agent_uuid, "<redacted:agent_uuid>"));
      AddField(&row, "policy_uuid", VisibleUuid(policy.policy_uuid, "<redacted:policy_uuid>"));
      AddField(&row, "policy_family", policy.policy_family);
      AddField(&row, "version_uuid", VisibleUuid(policy.version_uuid, "<redacted:version_uuid>"));
      AddField(&row, "active_state", policy.active_state);
      AddField(&row, "validation_state", policy.validation_state);
      AddField(&row, "attached_at", policy.attached_at);
      AddField(&row, "attached_by", policy.attached_by);
      result.rows.push_back(std::move(row));
    }
    return result;
  }

  if (canonical_view_path == "sys.agent_actions") {
    for (const auto& action : agent_actions) {
      if (!ProjectionSourceVisible(action.hidden, action.catalog_generation_id, context)) { continue; }
      SysInformationProjectionRow row;
      AddField(&row, "action_uuid", VisibleUuid(action.action_uuid, "<redacted:action_uuid>"));
      AddField(&row, "agent_uuid", VisibleUuid(action.agent_uuid, "<redacted:agent_uuid>"));
      AddField(&row, "action_id", action.action_id);
      AddField(&row, "state", action.state);
      AddField(&row, "risk_class", action.risk_class);
      AddField(&row, "created_at", action.created_at);
      AddField(&row, "expires_at", action.expires_at);
      AddField(&row, "approval_required", action.approval_required);
      AddField(&row, "actor_uuid",
               VisibleUuid(action.actor_uuid, "<redacted:actor_uuid>", action.actor_visible));
      AddField(&row, "diagnostic_code", action.diagnostic_code);
      result.rows.push_back(std::move(row));
    }
    return result;
  }

  if (canonical_view_path == "sys.agent_overrides") {
    for (const auto& override_row : agent_overrides) {
      if (!ProjectionSourceVisible(override_row.hidden, override_row.catalog_generation_id, context)) {
        continue;
      }
      SysInformationProjectionRow row;
      AddField(&row, "override_uuid",
               VisibleUuid(override_row.override_uuid, "<redacted:override_uuid>"));
      AddField(&row, "target_uuid",
               VisibleUuid(override_row.target_uuid, "<redacted:target_uuid>"));
      AddField(&row, "scope_uuid",
               VisibleUuid(override_row.scope_uuid,
                           "<redacted:scope_uuid>",
                           override_row.scope_visible));
      AddField(&row, "suppression_class", override_row.suppression_class);
      AddField(&row, "starts_at", override_row.starts_at);
      AddField(&row, "expires_at", override_row.expires_at);
      AddField(&row, "state", override_row.state);
      AddField(&row, "reason_code", override_row.reason_code);
      AddField(&row, "created_by",
               VisibleRef(override_row.created_by_ref,
                          override_row.created_by,
                          "<redacted:created_by>",
                          override_row.actor_visible));
      result.rows.push_back(std::move(row));
    }
    return result;
  }

  if (canonical_view_path == "sys.agent_evidence") {
    for (const auto& evidence : agent_evidence) {
      if (!ProjectionSourceVisible(evidence.hidden, evidence.catalog_generation_id, context)) { continue; }
      SysInformationProjectionRow row;
      AddField(&row, "evidence_uuid",
               VisibleUuid(evidence.evidence_uuid, "<redacted:evidence_uuid>"));
      AddField(&row, "agent_uuid",
               VisibleUuid(evidence.agent_uuid, "<redacted:agent_uuid>"));
      AddField(&row, "evidence_type", evidence.evidence_type);
      AddField(&row, "action_uuid",
               VisibleUuid(evidence.action_uuid, "<redacted:action_uuid>"));
      AddField(&row, "redaction_class", evidence.redaction_class);
      AddField(&row, "created_at", evidence.created_at);
      AddField(&row, "actor_uuid",
               VisibleUuid(evidence.actor_uuid,
                           "<redacted:actor_uuid>",
                           evidence.actor_visible));
      AddField(&row, "payload_digest", evidence.payload_digest);
      AddField(&row, "payload_redacted", evidence.payload_redacted);
      result.rows.push_back(std::move(row));
    }
    return result;
  }

  if (canonical_view_path == "sys.agent_audit") {
    for (const auto& audit : agent_audit) {
      if (!ProjectionSourceVisible(audit.hidden, audit.catalog_generation_id, context)) { continue; }
      SysInformationProjectionRow row;
      AddField(&row, "audit_uuid", VisibleUuid(audit.audit_uuid, "<redacted:audit_uuid>"));
      AddField(&row, "evidence_uuid",
               VisibleUuid(audit.evidence_uuid, "<redacted:evidence_uuid>"));
      AddField(&row, "actor_uuid",
               VisibleUuid(audit.actor_uuid, "<redacted:actor_uuid>", audit.actor_visible));
      AddField(&row, "command_name", audit.command_name);
      AddField(&row, "sblr_operation", audit.sblr_operation);
      AddField(&row, "api_call", audit.api_call);
      AddField(&row, "result_state", audit.result_state);
      AddField(&row, "diagnostic_code", audit.diagnostic_code);
      AddField(&row, "created_at", audit.created_at);
      result.rows.push_back(std::move(row));
    }
    return result;
  }

  if (canonical_view_path == "sys.filespace_capacity_agent_state") {
    for (const auto& state : filespace_capacity_agent_state) {
      if (!ProjectionSourceVisible(state.hidden, state.catalog_generation_id, context)) { continue; }
      SysInformationProjectionRow row;
      AddField(&row, "agent_uuid", VisibleUuid(state.agent_uuid, "<redacted:agent_uuid>"));
      AddField(&row, "filespace_uuid",
               VisibleUuid(state.filespace_uuid, "<redacted:filespace_uuid>"));
      AddField(&row, "policy_uuid", VisibleUuid(state.policy_uuid, "<redacted:policy_uuid>"));
      AddField(&row, "mode", state.mode);
      AddField(&row, "health_state", state.health_state);
      AddField(&row, "last_capacity_metric_at", state.last_capacity_metric_at);
      AddField(&row, "last_health_metric_at", state.last_health_metric_at);
      AddField(&row, "last_recommendation_code", state.last_recommendation_code);
      AddField(&row, "last_refusal_code", state.last_refusal_code);
      result.rows.push_back(std::move(row));
    }
    return result;
  }

  if (canonical_view_path == "sys.page_allocation_agent_state") {
    for (const auto& state : page_allocation_agent_state) {
      if (!ProjectionSourceVisible(state.hidden, state.catalog_generation_id, context)) { continue; }
      SysInformationProjectionRow row;
      AddField(&row, "agent_uuid", VisibleUuid(state.agent_uuid, "<redacted:agent_uuid>"));
      AddField(&row, "filespace_uuid",
               VisibleUuid(state.filespace_uuid, "<redacted:filespace_uuid>"));
      AddField(&row, "page_family", state.page_family);
      AddField(&row, "page_type", state.page_type);
      AddField(&row, "policy_uuid", VisibleUuid(state.policy_uuid, "<redacted:policy_uuid>"));
      AddField(&row, "mode", state.mode);
      AddField(&row, "last_scan_generation", state.last_scan_generation);
      AddField(&row, "last_shrink_ready_state", state.last_shrink_ready_state);
      AddField(&row, "last_refusal_code", state.last_refusal_code);
      result.rows.push_back(std::move(row));
    }
    return result;
  }

  if (canonical_view_path == "sys.filespace_shrink_readiness") {
    for (const auto& readiness : filespace_shrink_readiness) {
      if (!ProjectionSourceVisible(readiness.hidden, readiness.catalog_generation_id, context)) {
        continue;
      }
      SysInformationProjectionRow row;
      AddField(&row, "filespace_uuid",
               VisibleUuid(readiness.filespace_uuid, "<redacted:filespace_uuid>"));
      AddField(&row, "safe_start_byte", readiness.safe_start_byte);
      AddField(&row, "safe_end_byte", readiness.safe_end_byte);
      AddField(&row, "truncate_ready_bytes", readiness.truncate_ready_bytes);
      AddField(&row, "blocker_count", readiness.blocker_count);
      AddField(&row, "readiness_state", readiness.readiness_state);
      AddField(&row, "scan_generation", readiness.scan_generation);
      AddField(&row, "evidence_uuid",
               VisibleUuid(readiness.evidence_uuid, "<redacted:evidence_uuid>"));
      result.rows.push_back(std::move(row));
    }
    return result;
  }

  if (canonical_view_path == "sys.ipar.agent_lifecycle") {
    for (const auto& lifecycle : ipar_agent_lifecycle) {
      if (!ProjectionSourceVisible(lifecycle.hidden, lifecycle.catalog_generation_id, context)) {
        continue;
      }
      SysInformationProjectionRow row;
      AddField(&row, "runtime_id", StableRef(lifecycle.runtime_id, "server_agent_runtime"));
      AddField(&row, "source_kind", lifecycle.source_kind);
      AddField(&row, "lifecycle_state", lifecycle.lifecycle_state);
      AddField(&row, "idle_state", lifecycle.idle_state);
      AddField(&row, "started", lifecycle.started ? "YES" : "NO");
      AddField(&row, "stopping", lifecycle.stopping ? "YES" : "NO");
      AddField(&row, "worker_thread_count", std::to_string(lifecycle.worker_thread_count));
      AddField(&row, "background_worker_slots", std::to_string(lifecycle.background_worker_slots));
      AddField(&row,
               "foreground_reserved_capacity",
               std::to_string(lifecycle.foreground_reserved_capacity));
      AddField(&row, "worker_wake_policy", lifecycle.worker_wake_policy);
      AddField(&row, "scheduler_ticks", std::to_string(lifecycle.scheduler_ticks));
      AddField(&row, "total_worker_ticks", std::to_string(lifecycle.total_worker_ticks));
      AddField(&row,
               "total_actions_accepted",
               std::to_string(lifecycle.total_actions_accepted));
      AddField(&row,
               "total_actions_refused",
               std::to_string(lifecycle.total_actions_refused));
      AddField(&row,
               "total_actions_failed",
               std::to_string(lifecycle.total_actions_failed));
      AddField(&row,
               "scheduled_worker_count",
               std::to_string(lifecycle.scheduled_worker_count));
      AddField(&row, "min_worker_ticks", std::to_string(lifecycle.min_worker_ticks));
      AddField(&row, "max_worker_ticks", std::to_string(lifecycle.max_worker_ticks));
      AddField(&row, "starvation_events", std::to_string(lifecycle.starvation_events));
      AddField(&row,
               "last_diagnostic_agent_type_id",
               lifecycle.last_diagnostic_agent_type_id);
      AddField(&row, "last_diagnostic_action", lifecycle.last_diagnostic_action);
      AddField(&row,
               "last_diagnostic_outcome",
               lifecycle.last_diagnostic_outcome);
      AddField(&row, "last_diagnostic_code", lifecycle.last_diagnostic_code);
      AddField(&row, "last_diagnostic_detail", lifecycle.last_diagnostic_detail);
      AddField(&row, "durable_lease_count", std::to_string(lifecycle.durable_lease_count));
      AddField(&row,
               "durable_action_backlog_count",
               std::to_string(lifecycle.durable_action_backlog_count));
      AddField(&row,
               "durable_replay_pending_action_count",
               std::to_string(lifecycle.durable_replay_pending_action_count));
      AddField(&row, "process_rss_kb", std::to_string(lifecycle.process_rss_kb));
      AddField(&row, "process_vsize_kb", std::to_string(lifecycle.process_vsize_kb));
      AddField(&row, "source_state", lifecycle.source_state);
      result.rows.push_back(std::move(row));
    }
    return result;
  }

  if (canonical_view_path == "sys.ipar.metric_counters") {
    for (const auto& counter : ipar_metric_counters) {
      if (!ProjectionSourceVisible(counter.hidden, counter.catalog_generation_id, context)) {
        continue;
      }
      SysInformationProjectionRow row;
      AddField(&row, "metric_id", counter.metric_id);
      AddField(&row, "metric_path", counter.metric_path);
      AddField(&row, "metric_type", counter.metric_type);
      AddField(&row, "metric_unit", counter.metric_unit);
      AddField(&row, "value", std::to_string(counter.value));
      AddField(&row, "sample_count", std::to_string(counter.sample_count));
      AddField(&row, "label_summary", counter.label_summary);
      AddField(&row, "producer", counter.producer);
      AddField(&row, "source_state", counter.source_state);
      result.rows.push_back(std::move(row));
    }
    return result;
  }

  if (canonical_view_path == "sys.ipar.telemetry_controls") {
    for (const auto& control : ipar_telemetry_controls) {
      if (!ProjectionSourceVisible(control.hidden, control.catalog_generation_id, context)) {
        continue;
      }
      SysInformationProjectionRow row;
      AddField(&row, "budget_id", control.budget_id);
      AddField(&row, "control_name", control.control_name);
      AddField(&row, "metric_path", control.metric_path);
      AddField(&row, "configured_value", std::to_string(control.configured_value));
      AddField(&row, "observed_value", std::to_string(control.observed_value));
      AddField(&row,
               "sample_rate_per_mille",
               std::to_string(control.sample_rate_per_mille));
      AddField(&row, "persist_stride", std::to_string(control.persist_stride));
      AddField(&row, "skipped_count", std::to_string(control.skipped_count));
      AddField(&row,
               "dropped_metric_count",
               std::to_string(control.dropped_metric_count));
      AddField(&row, "overhead_budget_percent", control.overhead_budget_percent);
      AddField(&row, "source_state", control.source_state);
      result.rows.push_back(std::move(row));
    }
    return result;
  }

  if (canonical_view_path == "sys.ipar.slow_path_reasons") {
    for (const auto& reason : ipar_slow_path_reasons) {
      if (!ProjectionSourceVisible(reason.hidden, reason.catalog_generation_id, context)) {
        continue;
      }
      SysInformationProjectionRow row;
      AddField(&row, "metric_id", reason.metric_id);
      AddField(&row, "statement_id", reason.statement_id);
      AddField(&row, "chosen_path", reason.chosen_path);
      AddField(&row, "reason_code", reason.reason_code);
      AddField(&row, "fallback_count", std::to_string(reason.fallback_count));
      AddField(&row, "validation_stage", reason.validation_stage);
      AddField(&row, "driver_visible_message", reason.driver_visible_message);
      AddField(&row, "diagnostic_code", reason.diagnostic_code);
      AddField(&row, "sample_count", std::to_string(reason.sample_count));
      AddField(&row, "source_state", reason.source_state);
      AddField(&row,
               "required_action",
               "inspect_" + ProjectionToken(reason.validation_stage) +
                   "_reason_" + ProjectionToken(reason.reason_code));
      AddField(&row,
               "authority_scope",
               "diagnostic_evidence_only_not_finality_visibility_security_recovery_or_parser_authority");
      AddField(&row, "finality_authority", "NO");
      result.rows.push_back(std::move(row));
    }
    return result;
  }

  if (canonical_view_path == "sys.ipar.contention_quota") {
    for (const auto& source : ipar_contention_quota) {
      if (!ProjectionSourceVisible(source.hidden, source.catalog_generation_id, context)) {
        continue;
      }
      SysInformationProjectionRow row;
      AddField(&row, "row_id", source.row_id);
      AddField(&row, "metric_id", source.metric_id);
      AddField(&row, "category", source.category);
      AddField(&row, "subject", source.subject);
      AddField(&row, "metric_path", source.metric_path);
      AddField(&row, "metric_type", source.metric_type);
      AddField(&row, "metric_unit", source.metric_unit);
      AddField(&row, "observed_value", std::to_string(source.observed_value));
      AddField(&row, "limit_value", std::to_string(source.limit_value));
      AddField(&row, "refusal_count", std::to_string(source.refusal_count));
      AddField(&row, "wait_count", std::to_string(source.wait_count));
      AddField(&row, "queue_depth", std::to_string(source.queue_depth));
      AddField(&row, "sample_count", std::to_string(source.sample_count));
      AddField(&row, "producer", source.producer);
      AddField(&row, "source_kind", source.source_kind);
      AddField(&row, "source_ref", source.source_ref);
      AddField(&row, "provenance", source.provenance);
      AddField(&row, "diagnostic_code", source.diagnostic_code);
      AddField(&row, "source_state", source.source_state);
      result.rows.push_back(std::move(row));
    }
    return result;
  }

  if (canonical_view_path == "sys.frontend.agents") {
    if (!frontend_agents.empty()) {
      for (const auto& agent : frontend_agents) {
        if (!ProjectionSourceVisible(agent.hidden, agent.catalog_generation_id, context)) { continue; }
        SysInformationProjectionRow row;
        AddField(&row, "agent_name", agent.agent_name);
        AddField(&row, "agent_type_id", agent.agent_type_id);
        AddField(&row, "scope_kind", agent.scope_kind);
        AddField(&row, "state", agent.state);
        AddField(&row, "health_state", agent.health_state);
        AddField(&row, "enabled", agent.enabled);
        AddField(&row, "policy_name", agent.policy_name);
        result.rows.push_back(std::move(row));
      }
    } else {
      for (const auto& agent : agents) {
        if (!ProjectionSourceVisible(agent.hidden, agent.catalog_generation_id, context)) { continue; }
        SysInformationProjectionRow row;
        AddField(&row, "agent_name", StableRef(agent.agent_name, agent.agent_type_id));
        AddField(&row, "agent_type_id", agent.agent_type_id);
        AddField(&row, "scope_kind", agent.scope_kind);
        AddField(&row, "state", agent.state);
        AddField(&row, "health_state", agent.health_state);
        AddField(&row, "enabled", agent.enabled);
        AddField(&row, "policy_name", StableRef(agent.policy_name, agent.policy_ref));
        result.rows.push_back(std::move(row));
      }
    }
    return result;
  }

  if (canonical_view_path == "sys.frontend.agent_metric_dependencies") {
    for (const auto& dependency : agent_metric_dependencies) {
      if (!ProjectionSourceVisible(dependency.hidden, dependency.catalog_generation_id, context)) {
        continue;
      }
      SysInformationProjectionRow row;
      AddField(&row, "agent_name", StableRef(dependency.agent_ref, "<redacted:agent>"));
      AddField(&row, "metric_family", dependency.metric_family);
      AddField(&row, "namespace", dependency.metric_namespace);
      AddField(&row, "quality_state", dependency.quality_state);
      AddField(&row, "fail_behavior", dependency.fail_behavior);
      result.rows.push_back(std::move(row));
    }
    return result;
  }

  if (canonical_view_path == "sys.frontend.agent_policies") {
    for (const auto& policy : agent_policies) {
      if (!ProjectionSourceVisible(policy.hidden, policy.catalog_generation_id, context)) { continue; }
      SysInformationProjectionRow row;
      AddField(&row, "agent_name", StableRef(policy.agent_ref, "<redacted:agent>"));
      AddField(&row, "policy_name", StableRef(policy.policy_name, policy.policy_ref));
      AddField(&row, "policy_family", policy.policy_family);
      AddField(&row, "active_state", policy.active_state);
      AddField(&row, "validation_state", policy.validation_state);
      result.rows.push_back(std::move(row));
    }
    return result;
  }

  if (canonical_view_path == "sys.frontend.agent_actions") {
    for (const auto& action : agent_actions) {
      if (!ProjectionSourceVisible(action.hidden, action.catalog_generation_id, context)) { continue; }
      SysInformationProjectionRow row;
      AddField(&row, "action_ref", StableRef(action.action_ref, "<redacted:action>"));
      AddField(&row, "agent_name", StableRef(action.agent_ref, "<redacted:agent>"));
      AddField(&row, "action_id", action.action_id);
      AddField(&row, "state", action.state);
      AddField(&row, "risk_class", action.risk_class);
      AddField(&row, "approval_required", action.approval_required);
      result.rows.push_back(std::move(row));
    }
    return result;
  }

  if (canonical_view_path == "sys.frontend.agent_overrides") {
    for (const auto& override_row : agent_overrides) {
      if (!ProjectionSourceVisible(override_row.hidden, override_row.catalog_generation_id, context)) {
        continue;
      }
      SysInformationProjectionRow row;
      AddField(&row, "override_ref", StableRef(override_row.override_ref, "<redacted:override>"));
      AddField(&row, "target_name", StableRef(override_row.target_ref, "<redacted:target>"));
      AddField(&row, "scope_path", StableRef(override_row.scope_ref, "<redacted:scope>"));
      AddField(&row, "suppression_class", override_row.suppression_class);
      AddField(&row, "state", override_row.state);
      result.rows.push_back(std::move(row));
    }
    return result;
  }

  if (canonical_view_path == "sys.frontend.agent_evidence") {
    for (const auto& evidence : agent_evidence) {
      if (!ProjectionSourceVisible(evidence.hidden, evidence.catalog_generation_id, context)) { continue; }
      SysInformationProjectionRow row;
      AddField(&row, "evidence_ref", StableRef(evidence.evidence_ref, "<redacted:evidence>"));
      AddField(&row, "agent_name", StableRef(evidence.agent_ref, "<redacted:agent>"));
      AddField(&row, "evidence_type", evidence.evidence_type);
      AddField(&row, "redaction_class", evidence.redaction_class);
      AddField(&row, "created_at", evidence.created_at);
      result.rows.push_back(std::move(row));
    }
    return result;
  }

  if (canonical_view_path == "sys.frontend.agent_audit") {
    for (const auto& audit : agent_audit) {
      if (!ProjectionSourceVisible(audit.hidden, audit.catalog_generation_id, context)) { continue; }
      SysInformationProjectionRow row;
      AddField(&row, "audit_ref", StableRef(audit.audit_ref, "<redacted:audit>"));
      AddField(&row, "evidence_ref", StableRef(audit.evidence_ref, "<redacted:evidence>"));
      AddField(&row, "actor_name", audit.actor_visible ? StableRef(audit.actor_ref, "") : "<redacted:actor>");
      AddField(&row, "command_name", audit.command_name);
      AddField(&row, "result_state", audit.result_state);
      AddField(&row, "created_at", audit.created_at);
      result.rows.push_back(std::move(row));
    }
    return result;
  }

  if (canonical_view_path == "sys.frontend.filespace_capacity_agent_state") {
    for (const auto& state : filespace_capacity_agent_state) {
      if (!ProjectionSourceVisible(state.hidden, state.catalog_generation_id, context)) { continue; }
      SysInformationProjectionRow row;
      AddField(&row, "agent_name", StableRef(state.agent_ref, "<redacted:agent>"));
      AddField(&row, "filespace_name", StableRef(state.filespace_ref, "<redacted:filespace>"));
      AddField(&row, "policy_name", StableRef(state.policy_ref, "<redacted:policy>"));
      AddField(&row, "mode", state.mode);
      AddField(&row, "last_recommendation_code", state.last_recommendation_code);
      result.rows.push_back(std::move(row));
    }
    return result;
  }

  if (canonical_view_path == "sys.frontend.page_allocation_agent_state") {
    for (const auto& state : page_allocation_agent_state) {
      if (!ProjectionSourceVisible(state.hidden, state.catalog_generation_id, context)) { continue; }
      SysInformationProjectionRow row;
      AddField(&row, "agent_name", StableRef(state.agent_ref, "<redacted:agent>"));
      AddField(&row, "filespace_name", StableRef(state.filespace_ref, "<redacted:filespace>"));
      AddField(&row, "page_family", state.page_family);
      AddField(&row, "page_type", state.page_type);
      AddField(&row, "mode", state.mode);
      AddField(&row, "last_shrink_ready_state", state.last_shrink_ready_state);
      result.rows.push_back(std::move(row));
    }
    return result;
  }

  if (canonical_view_path == "sys.frontend.filespace_shrink_readiness") {
    for (const auto& readiness : filespace_shrink_readiness) {
      if (!ProjectionSourceVisible(readiness.hidden, readiness.catalog_generation_id, context)) {
        continue;
      }
      SysInformationProjectionRow row;
      AddField(&row, "filespace_name", StableRef(readiness.filespace_ref, "<redacted:filespace>"));
      AddField(&row, "truncate_ready_bytes", readiness.truncate_ready_bytes);
      AddField(&row, "blocker_count", readiness.blocker_count);
      AddField(&row, "readiness_state", readiness.readiness_state);
      AddField(&row, "scan_generation", readiness.scan_generation);
      result.rows.push_back(std::move(row));
    }
    return result;
  }

  if (canonical_view_path == "sys.information.scratchbird_protected_material") {
    for (const auto& material : protected_material) {
      if (!ProjectionSourceVisible(material.hidden, material.catalog_generation_id, context)) { continue; }
      SysInformationProjectionRow row;
      AddField(&row, "material_catalog", context.catalog_display_name);
      AddField(&row, "material_path", material.material_path);
      AddField(&row, "material_name", material.material_name);
      AddField(&row, "purpose_class", material.purpose_class);
      AddField(&row, "storage_class", material.storage_class);
      AddField(&row, "lifecycle_state", material.lifecycle_state);
      AddField(&row, "active_version_number", material.active_version_number);
      AddField(&row, "retention_policy_name", material.retention_policy_name);
      AddField(&row, "access_policy_name", material.access_policy_name);
      AddField(&row, "release_policy_name", material.release_policy_name);
      AddField(&row, "purge_policy_name", material.purge_policy_name);
      AddField(&row, "audit_policy_name", material.audit_policy_name);
      AddField(&row, "visibility_state", material.visibility_state);
      result.rows.push_back(std::move(row));
    }
    return result;
  }

  if (canonical_view_path == "sys.information.scratchbird_protected_material_versions") {
    for (const auto& version : protected_material_versions) {
      if (!ProjectionSourceVisible(version.hidden, version.catalog_generation_id, context)) { continue; }
      SysInformationProjectionRow row;
      AddField(&row, "material_catalog", context.catalog_display_name);
      AddField(&row, "material_path", version.material_path);
      AddField(&row, "material_name", version.material_name);
      AddField(&row, "version_number", version.version_number);
      AddField(&row, "storage_class", version.storage_class);
      AddField(&row, "rotation_state", version.rotation_state);
      AddField(&row, "valid_from_state", version.valid_from_state);
      AddField(&row, "valid_until_state", version.valid_until_state);
      AddField(&row, "retention_state", version.retention_state);
      AddField(&row, "purged", version.purged);
      AddField(&row, "payload_hash_present", version.payload_hash_present);
      AddField(&row, "audit_state", version.audit_state);
      result.rows.push_back(std::move(row));
    }
    return result;
  }

  if (canonical_view_path == "sys.information.scratchbird_object_paths" ||
      canonical_view_path == "sys.information.scratchbird_object_comments") {
    for (const auto& object : catalog_objects) {
      if (!ObjectVisible(object, context)) { continue; }
      bool found_object = false;
      bool found_schema = true;
      const std::string object_name = SelectResolverDisplayName(
          resolver_names, context, object.object_uuid, object.object_class, &found_object);
      std::string schema_name;
      if (!object.schema_uuid.empty()) {
        schema_name = SelectResolverDisplayName(
            resolver_names, context, object.schema_uuid, "schema", &found_schema);
      }
      if (!found_object || !found_schema) {
        if (context.strict_mode) {
          return Failure(kSysInformationDiagnosticNameNotFound,
                         found_object ? object.schema_uuid : object.object_uuid);
        }
        continue;
      }
      const std::string path = schema_name.empty() ? object_name : schema_name + "." + object_name;
      SysInformationProjectionRow row;
      AddField(&row, "object_catalog", context.catalog_display_name);
      AddField(&row, "object_schema", schema_name);
      AddField(&row, "object_name", object_name);
      AddField(&row, "object_type", object.object_class);
      if (canonical_view_path == "sys.information.scratchbird_object_paths") {
        AddField(&row, "object_path", path);
      } else {
        AddField(&row, "comment_text",
                 SelectCommentText(comments, context, object.object_uuid, object.object_class));
      }
      result.rows.push_back(std::move(row));
    }
    return result;
  }

  if (canonical_view_path == "sys.information.scratchbird_datatype_descriptors") {
    const auto builtin_sources = datatype_descriptors.empty()
                                     ? BuiltinDatatypeDescriptorSources(context)
                                     : std::vector<SysInformationDatatypeDescriptorSource>{};
    const auto& sources = datatype_descriptors.empty() ? builtin_sources : datatype_descriptors;
    for (const auto& source : sources) {
      if (!DatatypeDescriptorVisible(source, context)) { continue; }
      SysInformationProjectionRow row;
      AddField(&row, "type_catalog",
               source.type_catalog.empty() ? context.catalog_display_name : source.type_catalog);
      AddField(&row, "type_schema", source.type_schema);
      AddField(&row, "type_name", source.type_name);
      AddField(&row, "standard_type_name", source.standard_type_name);
      AddField(&row, "canonical_type_family", source.canonical_type_family);
      AddField(&row, "canonical_type_code", source.canonical_type_code);
      AddField(&row, "driver_family", source.driver_family);
      AddField(&row, "native_type_code", source.native_type_code);
      AddField(&row, "sql_type_code", source.sql_type_code);
      AddField(&row, "precision", source.precision);
      AddField(&row, "scale", source.scale);
      AddField(&row, "display_size", source.display_size);
      AddField(&row, "character_maximum_length", source.character_maximum_length);
      AddField(&row, "character_octet_length", source.character_octet_length);
      AddField(&row, "numeric_precision_radix", source.numeric_precision_radix);
      AddField(&row, "is_nullable", source.is_nullable);
      AddField(&row, "is_signed", source.is_signed);
      AddField(&row, "is_case_sensitive", source.is_case_sensitive);
      AddField(&row, "is_searchable", source.is_searchable);
      AddField(&row, "is_currency", source.is_currency);
      AddField(&row, "is_auto_increment_capable", source.is_auto_increment_capable);
      AddField(&row, "literal_prefix", source.literal_prefix);
      AddField(&row, "literal_suffix", source.literal_suffix);
      AddField(&row, "create_params", source.create_params);
      AddField(&row, "compatibility_class", source.compatibility_class);
      AddField(&row, "support_state", source.support_state);
      AddField(&row, "backend_profile", source.backend_profile);
      AddField(&row, "unsupported_reason", source.unsupported_reason);
      result.rows.push_back(std::move(row));
    }
    return result;
  }

  if (canonical_view_path == "sys.information.scratchbird_index_profiles") {
    for (const auto& profile : BuiltinCatalogIndexProfiles()) {
      if (!profile.cluster_path_fail_closed) {
        return Failure(kSysInformationDiagnosticClusterScopeForbidden, profile.index_name);
      }
      SysInformationProjectionRow row;
      AddField(&row, "profile_name", profile.index_name);
      AddField(&row, "catalog_table_path", profile.table_path);
      AddField(&row, "access_method", CatalogIndexMethodName(profile.method));
      AddField(&row, "access_purpose", CatalogIndexPurposeName(profile.purpose));
      AddField(&row, "is_unique", profile.unique ? "YES" : "NO");
      AddField(&row, "is_authoritative", profile.authoritative ? "YES" : "NO");
      AddField(&row, "resolver_boundary", profile.authority_boundary);
      AddField(&row, "ordered_access", profile.supports_ordered_scan ? "YES" : "NO");
      AddField(&row, "group_access", profile.supports_group_scan ? "YES" : "NO");
      AddField(&row, "prefix_access", profile.supports_prefix_scan ? "YES" : "NO");
      AddField(&row, "generation_access",
               profile.supports_catalog_generation_visibility ? "YES" : "NO");
      AddField(&row, "history_access", profile.supports_transaction_history ? "YES" : "NO");
      if (profile.method == scratchbird::core::catalog::CatalogIndexMethod::btree_ordered &&
          !CatalogIndexProfileHasOrderedNeed(profile)) {
        return Failure(kSysInformationDiagnosticInvalidDefinition, profile.index_name);
      }
      result.rows.push_back(std::move(row));
    }
    return result;
  }

  return result;
}

}  // namespace scratchbird::engine::internal_api
