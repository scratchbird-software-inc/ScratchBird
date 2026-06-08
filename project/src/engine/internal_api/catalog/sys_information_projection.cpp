// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/sys_information_projection.hpp"

#include "catalog_index_profile.hpp"
#include "datatype_descriptor.hpp"
#include "datatype_wire_metadata.hpp"

#include <algorithm>
#include <cstdint>
#include <set>
#include <sstream>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

using scratchbird::core::catalog::BuiltinCatalogIndexProfiles;
using scratchbird::core::catalog::CatalogIndexMethodName;
using scratchbird::core::catalog::CatalogIndexProfileHasOrderedNeed;
using scratchbird::core::catalog::CatalogIndexPurposeName;
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

std::string LogicalTypeForProjectionColumn(std::string_view column_name) {
  if (column_name == "ordinal_position" || column_name == "version_number" ||
      column_name == "active_version_number" || column_name == "port" ||
      column_name == "blocker_count" || column_name == "scan_generation" ||
      column_name == "integer_value" || column_name == "truncate_ready_bytes" ||
      column_name == "supported_value" || column_name == "increment" ||
      column_name == "start_value" || column_name == "safe_start_byte" ||
      column_name == "safe_end_byte") {
    return "uint64";
  }
  if (ContainsToken(column_name, "count") || ContainsToken(column_name, "bytes") ||
      ContainsToken(column_name, "epoch") || ContainsToken(column_name, "generation")) {
    return "uint64";
  }
  if (column_name == "enabled" || column_name == "grantable" ||
      column_name == "is_grantable" || column_name == "is_supported" ||
      column_name == "is_default" || column_name == "purged" ||
      column_name == "approval_required" || column_name == "cycle_option") {
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
sys.catalog_readable.object_names,catalog_readable,names_aliases,object_path;object_name;name_language;visibility_state,No UUID columns; names come from centralized resolver.
sys.catalog_readable.object_comments,catalog_readable,comments,object_path;object_name;comment_text;comment_language;visibility_state,No UUID columns; comments come from centralized comment resolver.
sys.catalog_readable.schemas,catalog_readable,schema_browser,schema_path;schema_name;parent_path;comment_text;visibility_state,Local schemas and subschemas.
sys.catalog_readable.relations,catalog_readable,relation_browser,relation_path;relation_name;relation_kind;schema_path;status;comment_text;visibility_state,Tables; views; materialized views; virtual views; relation-like objects.
sys.catalog_readable.columns,catalog_readable,column_grid,relation_path;column_name;ordinal_position;datatype_name;domain_name;is_nullable;comment_text;visibility_state,Preferred rich column metadata source.
sys.catalog_readable.constraints,catalog_readable,constraints,relation_path;constraint_name;constraint_kind;referenced_object_path;status;visibility_state,Constraint browser without UUID exposure.
sys.catalog_readable.indexes,catalog_readable,indexes,index_path;index_name;relation_path;index_family;storage_profile_name;comment_text;visibility_state,Index browser.
sys.catalog_readable.index_columns,catalog_readable,index_columns,index_path;ordinal_position;key_expression;collation_name;operator_class_name;direction,Ordered index keys and expressions.
sys.catalog_readable.datatypes,catalog_readable,datatypes,type_path;type_name;type_family;storage_class;wire_class;status;visibility_state,Canonical datatype descriptors.
sys.catalog_readable.domains,catalog_readable,domains,domain_path;domain_name;base_type_name;domain_kind;status;comment_text;visibility_state,Domains and donor-emulation domains.
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
sys.catalog_readable.filespaces,catalog_readable,storage,filespace_path;filespace_name;role;status;size_metrics;comment_text;visibility_state,Device paths redacted by policy.
sys.catalog_readable.page_families,catalog_readable,storage,page_family_name;page_role;layout_version;status;visibility_state,Page family metadata.
sys.catalog_readable.resources,catalog_readable,resources,resource_path;resource_name;resource_kind;version;status;visibility_state,Timezone; charset; collation; locale; parser; donor resources.
sys.catalog_readable.parser_profiles,catalog_readable,parser_profiles,profile_path;profile_name;parser_family;dialect;cache_policy;resource_epoch;visibility_state,Parser package and dialect profile metadata.
sys.catalog_readable.listeners,catalog_readable,listeners,listener_path;listener_name;network;port;parser_family;status;visibility_state,Listener profiles and parser bindings.
sys.catalog_readable.metrics_catalog,catalog_readable,metrics,metric_name;scope;units;type;retention_policy;redaction_class,Metric catalog metadata.
sys.catalog_readable.diagnostics_catalog,catalog_readable,diagnostics,diagnostic_name;category;severity;redaction_class;support_action,Diagnostic catalog metadata.
sys.catalog_readable.settings,catalog_readable,settings,setting_name;setting_value_display;authority;default_source;redaction_state;visibility_state,Secret values redacted by policy.
sys.catalog_readable.jobs,catalog_readable,jobs,job_path;job_name;job_kind;status;policy_name;metrics_summary;visibility_state,Jobs and maintenance tasks.
sys.catalog_readable.remote_connections,catalog_readable,remote_connections,connection_path;connection_name;connection_kind;endpoint_display;credential_state;visibility_state,Credentials redacted.
sys.catalog_readable.emulation_profiles,catalog_readable,emulation_profiles,profile_path;profile_name;donor_family;parser_binding;udr_binding;status;visibility_state,Donor/emulation profile metadata.
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

void AddField(SysInformationProjectionRow* row, std::string name, std::string value) {
  row->fields.push_back({std::move(name), std::move(value)});
}

std::string TableTypeForObject(const SysInformationCatalogObjectSource& object) {
  if (!object.table_type.empty()) { return object.table_type; }
  if (object.object_class == "view") { return "VIEW"; }
  if (object.object_class == "materialized_view") { return "MATERIALIZED VIEW"; }
  if (object.object_class == "temporary_table") { return "LOCAL TEMPORARY"; }
  if (object.object_class == "virtual_table") { return "VIRTUAL TABLE"; }
  return "BASE TABLE";
}

bool IsTableLikeObject(const SysInformationCatalogObjectSource& object) {
  return object.object_class == "table" || object.object_class == "view" ||
         object.object_class == "materialized_view" ||
         object.object_class == "temporary_table" ||
         object.object_class == "virtual_table";
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
    };
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
    const std::vector<SysInformationFilespaceShrinkReadinessSource>& filespace_shrink_readiness) {
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
      AddField(&row, "commit_action", "");
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

  if (canonical_view_path == "sys.catalog_readable.objects" ||
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
      const std::string object_path = ObjectDisplayPath(resolver_names, context, object, &found_path);
      if (!found_object || !found_path) {
        if (context.strict_mode) {
          return Failure(kSysInformationDiagnosticNameNotFound, object.object_uuid);
        }
        continue;
      }
      const std::string comment = SelectCommentText(
          comments, context, object.object_uuid, object.object_class);
      SysInformationProjectionRow row;
      if (canonical_view_path == "sys.catalog_readable.objects") {
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
