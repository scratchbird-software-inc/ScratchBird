// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "observability/show_api.hpp"

#include "agent_runtime.hpp"
#include "api_diagnostics.hpp"
#include "behavior_support/api_behavior_store.hpp"
#include "catalog/catalog_object_lifecycle.hpp"
#include "catalog/name_registry.hpp"
#include "catalog/schema_tree_api.hpp"
#include "catalog/sys_information_projection.hpp"
#include "catalog_index_profile.hpp"
#include "crud_support/crud_store.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "observability/performance_optimization_surface.hpp"
#include "security/security_principal_lifecycle.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <string_view>
#include <vector>

namespace scratchbird::engine::internal_api {

namespace {

using scratchbird::core::catalog::BuiltinCatalogTableProfiles;

std::string OptionValue(const EngineApiRequest& request, const std::string& prefix) {
  for (const auto& option : request.option_envelopes) {
    if (option.rfind(prefix, 0) == 0) return option.substr(prefix.size());
  }
  return {};
}

std::vector<std::string> SplitOptionList(std::string_view value) {
  std::vector<std::string> out;
  while (!value.empty()) {
    const std::size_t pos = value.find(';');
    const std::string_view token =
        pos == std::string_view::npos ? value : value.substr(0, pos);
    if (!token.empty()) { out.emplace_back(token); }
    if (pos == std::string_view::npos) { break; }
    value.remove_prefix(pos + 1);
  }
  return out;
}

void PopulateSessionSecurityProjectionContext(
    const EngineShowCatalogRequest& request,
    SysInformationProjectionContext* projection_context) {
  if (projection_context == nullptr) { return; }
  projection_context->principal_uuid = request.context.principal_uuid.canonical;
  projection_context->principal_name = OptionValue(request, "principal_name:");
  projection_context->requested_role_name = OptionValue(request, "requested_role_name:");
  projection_context->active_role_name = OptionValue(request, "active_role_name:");
  if (projection_context->active_role_name.empty()) {
    projection_context->active_role_name = projection_context->requested_role_name;
  }
  projection_context->active_role_uuid = request.context.current_role_uuid.canonical;
  if (projection_context->active_role_uuid.empty()) {
    projection_context->active_role_uuid = OptionValue(request, "current_role_uuid:");
  }
  projection_context->effective_role_uuids =
      SplitOptionList(OptionValue(request, "effective_role_uuid_set:"));
  projection_context->effective_group_uuids =
      SplitOptionList(OptionValue(request, "effective_group_uuid_set:"));
  if (!projection_context->active_role_uuid.empty() &&
      std::find(projection_context->effective_role_uuids.begin(),
                projection_context->effective_role_uuids.end(),
                projection_context->active_role_uuid) ==
          projection_context->effective_role_uuids.end()) {
    projection_context->effective_role_uuids.push_back(projection_context->active_role_uuid);
  }
  if (!projection_context->active_role_name.empty()) {
    projection_context->effective_role_names.push_back(projection_context->active_role_name);
  }
}

std::string DatabaseDisplayName(const EngineRequestContext& context) {
  std::string path = context.database_path;
  while (!path.empty() && (path.back() == '/' || path.back() == '\\')) {
    path.pop_back();
  }
  if (!path.empty()) {
    const auto separator = path.find_last_of("/\\");
    std::string name = separator == std::string::npos ? path : path.substr(separator + 1);
    const auto extension = name.rfind(".sbdb");
    if (extension != std::string::npos && extension + 5 == name.size()) {
      name = name.substr(0, extension);
    }
    if (!name.empty()) { return name; }
  }
  return "ScratchBird";
}

std::string ResultShapeContract(const EngineApiRequest& request, const std::string& fallback) {
  const auto result_shape = OptionValue(request, "result_shape_contract:");
  return result_shape.empty() ? fallback : result_shape;
}

template <typename TResult>
void AddSbsfc080Evidence(TResult* result, const EngineApiRequest& request) {
  const std::string surface_id = OptionValue(request, "sbsfc080_surface_id:");
  if (surface_id.empty()) return;
  const std::string evidence_kind = OptionValue(request, "sbsfc080_runtime_evidence_kind:");
  const std::string evidence_id = OptionValue(request, "sbsfc080_runtime_evidence_id:");
  AddApiBehaviorEvidence(result, evidence_kind.empty() ? "operational_observability_route" : evidence_kind,
                         evidence_id.empty() ? surface_id : evidence_id);
  AddApiBehaviorEvidence(result, "sbsfc080_surface", surface_id);
  AddApiBehaviorEvidence(result, "parser_executes_sql", "false");
  AddApiBehaviorEvidence(result, "cluster_provider_dispatch", "false");
  AddApiBehaviorEvidence(result, "private_cluster_execution", "false");
  AddApiBehaviorEvidence(result, "wal_recovery_authority", "false");
}

template <typename TResult>
void AddPublicExactShowEvidence(TResult* result,
                                const EngineApiRequest& request,
                                const std::string& operation_id,
                                const std::string& fallback_result_shape) {
  const std::string result_shape = ResultShapeContract(request, fallback_result_shape);
  if (OptionValue(request, "result_shape_contract:").empty()) return;
  AddApiBehaviorEvidence(result, "public_sbsql_operation", operation_id);
  AddApiBehaviorEvidence(result, "engine_api_function", "EngineInspectShowOperation");
  AddApiBehaviorEvidence(result, "result_shape_contract", result_shape);
  result->result_shape.result_kind = result_shape;
}

template <typename TResult>
TResult ShowBase(const EngineApiRequest& request,
                 const std::string& operation_id,
                 std::vector<std::pair<std::string, std::string>> fields,
                 const std::string& fallback_result_shape) {
  auto result = MakeApiBehaviorSuccess<TResult>(request.context, operation_id);
  AddApiBehaviorEvidence(&result, "observability", operation_id);
  AddApiBehaviorRow(&result, std::move(fields));
  AddSbsfc080Evidence(&result, request);
  AddPublicExactShowEvidence(&result, request, operation_id, fallback_result_shape);
  return result;
}

std::string OperationIdOr(const EngineApiRequest& request, const std::string& fallback) {
  return request.operation_id.empty() ? fallback : request.operation_id;
}

void AddShowOperationResult(EngineApiResult* result,
                            const EngineApiRequest& request,
                            const std::string& operation_id,
                            const std::string& result_shape) {
  AddApiBehaviorEvidence(result, "public_sbsql_operation", operation_id);
  AddApiBehaviorEvidence(result, "engine_api_function", "EngineInspectShowOperation");
  AddApiBehaviorEvidence(result, "parser_executes_sql", "false");
  AddApiBehaviorEvidence(result, "cluster_provider_dispatch", "false");
  AddApiBehaviorEvidence(result, "private_cluster_execution", "false");
  AddApiBehaviorEvidence(result, "result_shape_contract", result_shape);
  AddApiBehaviorRow(result,
                    {{"operation_id", operation_id},
                     {"result_shape", result_shape},
                     {"route_kind", "observability_show_inspect"},
                     {"target_ref_kind", OptionValue(request, "target_ref_kind:")},
                     {"target_ref_visible", OptionValue(request, "target_ref:").empty() ? "false" : "true"},
                     {"catalog_generation_id", std::to_string(request.context.catalog_generation_id)},
                     {"security_epoch", std::to_string(request.context.security_epoch)},
                     {"resource_epoch", std::to_string(request.context.resource_epoch)}});
  result->result_shape.result_kind = result_shape;
}

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

std::string ParentPath(std::string_view path) {
  const std::size_t dot = path.rfind('.');
  if (dot == std::string_view::npos) { return {}; }
  return std::string(path.substr(0, dot));
}

std::string LeafName(std::string_view path) {
  const std::size_t dot = path.rfind('.');
  if (dot == std::string_view::npos) { return std::string(path); }
  return std::string(path.substr(dot + 1));
}

std::string NameClassForProjection(const std::string& name_class) {
  if (name_class.empty() || name_class == "default") { return "primary"; }
  return name_class;
}

std::string SchemaDisplayPath(const std::vector<EngineSchemaTreeRecord>& schemas,
                              const std::string& schema_uuid,
                              std::map<std::string, std::string>* cache) {
  if (schema_uuid.empty() || cache == nullptr) { return {}; }
  const auto cached = cache->find(schema_uuid);
  if (cached != cache->end()) { return cached->second; }
  const auto found = std::find_if(schemas.begin(), schemas.end(), [&schema_uuid](const auto& schema) {
    return schema.schema_uuid == schema_uuid;
  });
  if (found == schemas.end()) { return {}; }
  for (const auto& name : found->localized_names) {
    if (name.default_name && !name.path.empty()) {
      (*cache)[schema_uuid] = name.path;
      return name.path;
    }
  }
  const std::string leaf = SchemaTreeDefaultName(found->localized_names, found->default_name);
  const std::string parent = SchemaDisplayPath(schemas, found->parent_schema_uuid, cache);
  const std::string path = parent.empty() ? leaf : parent + "." + leaf;
  (*cache)[schema_uuid] = path;
  return path;
}

void AddResolverName(std::vector<SysInformationResolverNameSource>* resolver_names,
                     std::string object_uuid,
                     std::string object_class,
                     std::string scope_uuid,
                     std::string language_tag,
                     std::string name_class,
                     std::string display_name,
                     std::uint64_t catalog_generation_id) {
  if (resolver_names == nullptr || object_uuid.empty() || display_name.empty()) { return; }
  SysInformationResolverNameSource name;
  name.object_uuid = std::move(object_uuid);
  name.object_class = std::move(object_class);
  name.scope_uuid = std::move(scope_uuid);
  name.language_tag = language_tag.empty() ? "en" : std::move(language_tag);
  name.name_class = NameClassForProjection(name_class);
  name.display_name = std::move(display_name);
  name.catalog_generation_id = catalog_generation_id;
  resolver_names->push_back(std::move(name));
}

std::uint64_t CatalogObserverTx(const EngineRequestContext& context) {
  return std::max(context.local_transaction_id,
                  context.snapshot_visible_through_local_transaction_id);
}

EngineRequestContext CatalogReadContext(EngineRequestContext context) {
  context.local_transaction_id = 0;
  return context;
}

void MergeCrudCatalogState(CrudState* base, const CrudState& source) {
  if (base == nullptr) { return; }
  for (const auto& [tx, state] : source.transactions) {
    base->transactions[tx] = state;
    base->max_transaction_id = std::max(base->max_transaction_id, tx);
  }
  for (const auto& table : source.tables) {
    auto existing = std::find_if(base->tables.begin(), base->tables.end(),
                                 [&table](const CrudTableRecord& candidate) {
                                   return candidate.table_uuid == table.table_uuid;
                                 });
    if (existing == base->tables.end()) {
      base->tables.push_back(table);
    } else {
      *existing = table;
    }
  }
  for (const auto& index : source.indexes) {
    auto existing = std::find_if(base->indexes.begin(), base->indexes.end(),
                                 [&index](const CrudIndexRecord& candidate) {
                                   return candidate.index_uuid == index.index_uuid;
                                 });
    if (existing == base->indexes.end()) {
      base->indexes.push_back(index);
    } else {
      *existing = index;
    }
  }
  base->max_event_sequence = std::max(base->max_event_sequence, source.max_event_sequence);
  base->max_sequence = std::max(base->max_sequence, source.max_sequence);
  base->max_index_sequence = std::max(base->max_index_sequence, source.max_index_sequence);
}

CrudState LoadReadableCatalogCrudState(const EngineRequestContext& context) {
  CrudState state;
  const auto crud = LoadCrudState(context);
  if (crud.ok) { state = crud.state; }
  const auto mga_relations = LoadMgaRelationStoreState(context);
  if (mga_relations.ok) {
    MergeCrudCatalogState(&state, BuildCrudCompatibilityStateFromMga(mga_relations.state));
  }
  return state;
}

std::string DisplayTextForNameEntry(const NameRegistryEntry& entry) {
  if (!entry.display_name.empty()) { return entry.display_name; }
  return entry.raw_name_text;
}

void AddNameRegistryResolverNames(
    std::vector<SysInformationResolverNameSource>* resolver_names,
    const NameRegistryState& name_state) {
  for (const auto& entry : name_state.entries) {
    if (entry.deleted || entry.object_uuid.empty()) { continue; }
    AddResolverName(resolver_names,
                    entry.object_uuid,
                    entry.object_class,
                    entry.scope_uuid,
                    entry.language_tag,
                    entry.name_class,
                    DisplayTextForNameEntry(entry),
                    entry.catalog_generation_id == 0 ? entry.creator_tx : entry.catalog_generation_id);
  }
}

const NameRegistryEntry* ScopedNameEntryForObject(
    const std::map<std::string, std::vector<const NameRegistryEntry*>>& names_by_object,
    const std::string& object_uuid,
    const std::string& object_class,
    const std::map<std::string, std::string>& schema_path_by_uuid) {
  const auto found = names_by_object.find(object_uuid);
  if (found == names_by_object.end()) { return nullptr; }
  const NameRegistryEntry* fallback = nullptr;
  for (const auto* entry : found->second) {
    if (entry == nullptr || entry->deleted || entry->object_class != object_class) { continue; }
    if (fallback == nullptr && !DisplayTextForNameEntry(*entry).empty()) { fallback = entry; }
    if (schema_path_by_uuid.find(entry->scope_uuid) == schema_path_by_uuid.end()) { continue; }
    return entry;
  }
  return fallback;
}

std::string TableTypeForCrudTable(const CrudTableRecord& table) {
  if (!table.temporary) { return "BASE TABLE"; }
  if (table.temporary_scope == "global") { return "GLOBAL TEMPORARY"; }
  return "LOCAL TEMPORARY";
}

std::uint64_t CatalogGenerationForNameEntry(const NameRegistryEntry& entry,
                                            std::uint64_t fallback) {
  if (entry.catalog_generation_id != 0) { return entry.catalog_generation_id; }
  return fallback == 0 ? 1 : fallback;
}

void AddSystemViewObject(std::vector<SysInformationCatalogObjectSource>* objects,
                         std::vector<SysInformationResolverNameSource>* resolver_names,
                         const std::map<std::string, std::string>& schema_uuid_by_path,
                         const std::string& view_path) {
  const std::string schema_path = ParentPath(view_path);
  const auto schema = schema_uuid_by_path.find(schema_path);
  if (schema == schema_uuid_by_path.end()) { return; }
  const std::string object_uuid = "sysview:" + view_path;
  SysInformationCatalogObjectSource object;
  object.object_uuid = object_uuid;
  object.object_class = "view";
  object.schema_uuid = schema->second;
  object.parent_object_uuid = schema->second;
  object.table_type = "SYSTEM VIEW";
  object.catalog_generation_id = 1;
  object.created_local_transaction_id = 1;
  objects->push_back(std::move(object));
  AddResolverName(resolver_names,
                  object_uuid,
                  "view",
                  schema->second,
                  "en",
                  "primary",
                  LeafName(view_path),
                  1);
}

void AddSystemTableObject(std::vector<SysInformationCatalogObjectSource>* objects,
                          std::vector<SysInformationResolverNameSource>* resolver_names,
                          const std::map<std::string, std::string>& schema_uuid_by_path,
                          const std::string& table_path) {
  const std::string schema_path = ParentPath(table_path);
  const auto schema = schema_uuid_by_path.find(schema_path);
  if (schema == schema_uuid_by_path.end()) { return; }
  const std::string object_uuid = "systable:" + table_path;
  SysInformationCatalogObjectSource object;
  object.object_uuid = object_uuid;
  object.object_class = "table";
  object.schema_uuid = schema->second;
  object.parent_object_uuid = schema->second;
  object.table_type = "SYSTEM TABLE";
  object.catalog_generation_id = 1;
  object.created_local_transaction_id = 1;
  objects->push_back(std::move(object));
  AddResolverName(resolver_names,
                  object_uuid,
                  "table",
                  schema->second,
                  "en",
                  "primary",
                  LeafName(table_path),
                  1);
}

std::uint64_t SecurityCatalogGeneration(std::uint64_t generation, std::uint64_t creator_tx) {
  (void)generation;
  return creator_tx == 0 ? 1 : creator_tx;
}

void AddSecurityNavigatorObject(std::vector<SysInformationCatalogObjectSource>* objects,
                                std::vector<SysInformationResolverNameSource>* resolver_names,
                                std::string object_uuid,
                                std::string object_class,
                                std::string parent_object_uuid,
                                std::string schema_uuid,
                                std::string table_type,
                                std::string display_name,
                                std::uint64_t catalog_generation_id,
                                std::uint64_t creator_tx) {
  if (objects == nullptr || resolver_names == nullptr ||
      object_uuid.empty() || object_class.empty() || display_name.empty()) {
    return;
  }
  SysInformationCatalogObjectSource object;
  object.object_uuid = object_uuid;
  object.object_class = object_class;
  object.schema_uuid = std::move(schema_uuid);
  object.parent_object_uuid = std::move(parent_object_uuid);
  object.table_type = std::move(table_type);
  object.catalog_generation_id = SecurityCatalogGeneration(catalog_generation_id, creator_tx);
  object.created_local_transaction_id = creator_tx;
  objects->push_back(std::move(object));
  AddResolverName(resolver_names,
                  std::move(object_uuid),
                  std::move(object_class),
                  "",
                  "en",
                  "primary",
                  std::move(display_name),
                  SecurityCatalogGeneration(catalog_generation_id, creator_tx));
}

std::string GrantDisplayName(const EngineSecurityPrivilegeGrantRecord& grant) {
  if (grant.privilege.empty()) { return grant.grant_uuid; }
  if (!grant.target_object_kind.empty()) {
    return grant.privilege + " on " + grant.target_object_kind;
  }
  return grant.privilege;
}

void AddSecurityPrincipalNavigatorObjects(
    std::vector<SysInformationCatalogObjectSource>* objects,
    std::vector<SysInformationResolverNameSource>* resolver_names,
    const EngineSecurityPrincipalLifecycleState& security) {
  std::map<std::string, std::string> principal_names;
  std::map<std::string, std::string> role_names;
  std::map<std::string, std::string> group_names;

  for (const auto& principal : security.principals) {
    if (principal.deleted || principal.principal_uuid.empty() || principal.principal_name.empty()) {
      continue;
    }
    principal_names[principal.principal_uuid] = principal.principal_name;
    AddSecurityNavigatorObject(objects,
                               resolver_names,
                               principal.principal_uuid,
                               principal.principal_kind == "user" ? "user" : "principal",
                               "",
                               "",
                               principal.principal_kind,
                               principal.principal_name,
                               principal.security_generation,
                               principal.creator_tx);
  }
  for (const auto& group : security.groups) {
    if (group.deleted || group.group_uuid.empty() || group.group_name.empty()) { continue; }
    group_names[group.group_uuid] = group.group_name;
    AddSecurityNavigatorObject(objects,
                               resolver_names,
                               group.group_uuid,
                               "group",
                               "",
                               "",
                               "",
                               group.group_name,
                               group.security_generation,
                               group.creator_tx);
  }
  for (const auto& role : security.roles) {
    if (role.deleted || role.role_uuid.empty() || role.role_name.empty()) { continue; }
    role_names[role.role_uuid] = role.role_name;
    AddSecurityNavigatorObject(objects,
                               resolver_names,
                               role.role_uuid,
                               "role",
                               "",
                               "",
                               "",
                               role.role_name,
                               role.security_generation,
                               role.creator_tx);
  }

  for (const auto& membership : security.memberships) {
    if (membership.revoked || membership.membership_uuid.empty() ||
        membership.member_principal_uuid.empty() || membership.container_uuid.empty()) {
      continue;
    }
    const auto principal = principal_names.find(membership.member_principal_uuid);
    if (membership.container_kind == "group") {
      if (principal == principal_names.end()) { continue; }
      const auto group = group_names.find(membership.container_uuid);
      if (group == group_names.end()) { continue; }
      AddSecurityNavigatorObject(objects,
                                 resolver_names,
                                 membership.membership_uuid + ":user_group",
                                 "security_user_group_membership",
                                 membership.member_principal_uuid,
                                 membership.container_uuid,
                                 "group",
                                 group->second,
                                 membership.security_generation,
                                 membership.creator_tx);
      AddSecurityNavigatorObject(objects,
                                 resolver_names,
                                 membership.membership_uuid + ":group_user",
                                 "security_group_user_membership",
                                 membership.container_uuid,
                                 membership.member_principal_uuid,
                                 "user",
                                 principal->second,
                                 membership.security_generation,
                                 membership.creator_tx);
    } else if (membership.container_kind == "role") {
      const auto role = role_names.find(membership.container_uuid);
      if (role == role_names.end()) { continue; }
      if (principal != principal_names.end()) {
        AddSecurityNavigatorObject(objects,
                                   resolver_names,
                                   membership.membership_uuid + ":user_role",
                                   "security_user_role_membership",
                                   membership.member_principal_uuid,
                                   membership.container_uuid,
                                   "role",
                                   role->second,
                                   membership.security_generation,
                                   membership.creator_tx);
        AddSecurityNavigatorObject(objects,
                                   resolver_names,
                                   membership.membership_uuid + ":role_user",
                                   "security_role_user_membership",
                                   membership.container_uuid,
                                   membership.member_principal_uuid,
                                   "user",
                                   principal->second,
                                   membership.security_generation,
                                   membership.creator_tx);
        continue;
      }
      const auto group = group_names.find(membership.member_principal_uuid);
      if (group == group_names.end()) { continue; }
      AddSecurityNavigatorObject(objects,
                                 resolver_names,
                                 membership.membership_uuid + ":group_role",
                                 "security_group_role_membership",
                                 membership.member_principal_uuid,
                                 membership.container_uuid,
                                 "role",
                                 role->second,
                                 membership.security_generation,
                                 membership.creator_tx);
      AddSecurityNavigatorObject(objects,
                                 resolver_names,
                                 membership.membership_uuid + ":role_group",
                                 "security_role_group_membership",
                                 membership.container_uuid,
                                 membership.member_principal_uuid,
                                 "group",
                                 group->second,
                                 membership.security_generation,
                                 membership.creator_tx);
    }
  }

  for (const auto& grant : security.grants) {
    if (grant.revoked || grant.grant_uuid.empty() || grant.grantee_uuid.empty()) { continue; }
    AddSecurityNavigatorObject(objects,
                               resolver_names,
                               grant.grant_uuid,
                               "security_grant",
                               grant.grantee_uuid,
                               grant.target_object_uuid,
                               grant.grantee_kind,
                               GrantDisplayName(grant),
                               grant.security_generation,
                               grant.creator_tx);
  }

  for (const auto& policy : security.row_policies) {
    if (policy.deleted || policy.policy_uuid.empty()) { continue; }
    AddSecurityNavigatorObject(objects,
                               resolver_names,
                               policy.policy_uuid,
                               "security_policy",
                               "",
                               policy.target_object_uuid,
                               policy.target_object_kind,
                               policy.policy_uuid,
                               policy.policy_generation,
                               policy.creator_tx);
  }
}

EngineShowCatalogResult BuildReadableCatalogProjectionResult(const EngineShowCatalogRequest& request) {
  auto result = MakeApiBehaviorSuccess<EngineShowCatalogResult>(
      request.context,
      "observability.show_catalog");

  SysInformationProjectionContext projection_context;
  projection_context.catalog_display_name = DatabaseDisplayName(request.context);
  projection_context.session_language = request.context.language_context.language_tag.empty()
                                             ? "en"
                                             : request.context.language_context.language_tag;
  projection_context.default_language = request.context.language_context.default_language_tag.empty()
                                            ? "en"
                                            : request.context.language_context.default_language_tag;
  projection_context.session_uuid = request.context.session_uuid.canonical;
  projection_context.visible_catalog_generation_id = request.context.catalog_generation_id;
  projection_context.cluster_authority_available = request.context.cluster_authority_available;
  PopulateSessionSecurityProjectionContext(request, &projection_context);
  const EngineRequestContext catalog_read_context = CatalogReadContext(request.context);

  std::vector<SysInformationCatalogObjectSource> objects;
  std::vector<SysInformationResolverNameSource> resolver_names;
  std::vector<SysInformationColumnSource> columns;
  const std::uint64_t observer_tx = CatalogObserverTx(request.context);
  const auto schemas = VisibleSchemaTreeRecords(request.context, observer_tx);
  std::map<std::string, std::string> schema_path_by_uuid;
  std::map<std::string, std::string> schema_uuid_by_path;
  std::set<std::string> object_uuids_in_projection;
  std::set<std::string> column_keys_in_projection;
  for (const auto& schema : schemas) {
    if (schema.schema_uuid.empty()) { continue; }
    const std::string schema_path = SchemaDisplayPath(schemas, schema.schema_uuid, &schema_path_by_uuid);
    if (!schema_path.empty()) { schema_uuid_by_path[schema_path] = schema.schema_uuid; }
    SysInformationCatalogObjectSource object;
    object.object_uuid = schema.schema_uuid;
    object.object_class = "schema";
    object.parent_object_uuid = schema.parent_schema_uuid;
    object.catalog_generation_id = schema.creator_tx == 0 ? 1 : schema.creator_tx;
    object.created_local_transaction_id = schema.creator_tx;
    objects.push_back(std::move(object));
    object_uuids_in_projection.insert(schema.schema_uuid);
    if (schema.localized_names.empty()) {
      AddResolverName(&resolver_names,
                      schema.schema_uuid,
                      "schema",
                      schema.parent_schema_uuid,
                      "en",
                      "primary",
                      schema.default_name,
                      schema.creator_tx);
    } else {
      for (const auto& name : schema.localized_names) {
        AddResolverName(&resolver_names,
                        schema.schema_uuid,
                        "schema",
                        schema.parent_schema_uuid,
                        name.language_tag,
                        name.name_class,
                        name.name.empty() ? schema.default_name : name.name,
                        schema.creator_tx);
      }
    }
  }

  const auto lifecycle = LoadCatalogObjectLifecycleState(request.context);
  if (lifecycle.ok) {
    for (const auto& record : lifecycle.state.objects) {
      if (record.deleted || record.object_uuid.empty()) { continue; }
      SysInformationCatalogObjectSource object;
      object.object_uuid = record.object_uuid;
      object.object_class = record.object_kind.empty() ? "object" : record.object_kind;
      object.schema_uuid = record.schema_uuid;
      object.parent_object_uuid = record.schema_uuid;
      object.table_type = object.object_class == "view" ? "VIEW" : "";
      object.catalog_generation_id = record.metadata_epoch == 0 ? record.creator_tx : record.metadata_epoch;
      object.created_local_transaction_id = record.creator_tx;
      if (object_uuids_in_projection.insert(record.object_uuid).second) {
        objects.push_back(std::move(object));
      }
    }
    for (const auto& name : lifecycle.state.names) {
      if (name.deleted) { continue; }
      AddResolverName(&resolver_names,
                      name.object_uuid,
                      name.object_kind,
                      name.schema_uuid,
                      name.language_tag,
                      name.name_class,
                      name.display_name.empty() ? name.raw_name_text : name.display_name,
                      name.metadata_epoch == 0 ? name.creator_tx : name.metadata_epoch);
    }
    for (const auto& column : lifecycle.state.columns) {
      if (column.deleted || column.owner_object_uuid.empty()) { continue; }
      SysInformationColumnSource source;
      source.relation_object_uuid = column.owner_object_uuid;
      source.column_name = column.column_uuid;
      for (const auto& name : lifecycle.state.names) {
        if (name.deleted || name.object_uuid != column.column_uuid) { continue; }
        if (!name.display_name.empty()) {
          source.column_name = name.display_name;
          break;
        }
        if (!name.raw_name_text.empty()) {
          source.column_name = name.raw_name_text;
          break;
        }
      }
      source.ordinal_position = column.ordinal;
      source.datatype_name = column.canonical_type_name;
      source.is_nullable = column.nullable ? "YES" : "NO";
      source.catalog_generation_id = column.metadata_epoch == 0 ? column.creator_tx : column.metadata_epoch;
      const std::string column_key =
          source.relation_object_uuid + ":" + std::to_string(source.ordinal_position);
      if (column_keys_in_projection.insert(column_key).second) {
        columns.push_back(std::move(source));
      }
    }
  }

  const auto name_registry = LoadNameRegistryState(catalog_read_context, observer_tx);
  std::map<std::string, std::vector<const NameRegistryEntry*>> names_by_object;
  if (name_registry.ok) {
    AddNameRegistryResolverNames(&resolver_names, name_registry.state);
    for (const auto& entry : name_registry.state.entries) {
      if (entry.deleted || entry.object_uuid.empty()) { continue; }
      names_by_object[entry.object_uuid].push_back(&entry);
    }
  }

  const CrudState readable_crud = LoadReadableCatalogCrudState(catalog_read_context);
  std::map<std::string, std::string> table_schema_by_uuid;
  for (const auto& table : readable_crud.tables) {
    if (table.table_uuid.empty() ||
        !CrudCreatorVisible(readable_crud, table.creator_tx, table.event_sequence, observer_tx)) {
      continue;
    }
    const auto* name = ScopedNameEntryForObject(
        names_by_object,
        table.table_uuid,
        "table",
        schema_path_by_uuid);
    if (name == nullptr ||
        schema_path_by_uuid.find(name->scope_uuid) == schema_path_by_uuid.end()) {
      continue;
    }
    table_schema_by_uuid[table.table_uuid] = name->scope_uuid;
    if (object_uuids_in_projection.insert(table.table_uuid).second) {
      SysInformationCatalogObjectSource object;
      object.object_uuid = table.table_uuid;
      object.object_class = "table";
      object.schema_uuid = name->scope_uuid;
      object.parent_object_uuid = name->scope_uuid;
      object.table_type = TableTypeForCrudTable(table);
      object.temporary = table.temporary;
      object.temporary_scope = table.temporary_scope;
      object.temporary_session_uuid = table.temporary_session_uuid;
      object.on_commit_action = table.on_commit_action;
      object.catalog_generation_id = CatalogGenerationForNameEntry(*name, table.creator_tx);
      object.created_local_transaction_id = table.creator_tx;
      objects.push_back(std::move(object));
    }
    std::uint32_t ordinal = 0;
    for (const auto& [column_name, datatype_name] : table.columns) {
      ++ordinal;
      if (column_name.empty()) { continue; }
      const std::string column_key = table.table_uuid + ":" + std::to_string(ordinal);
      if (!column_keys_in_projection.insert(column_key).second) { continue; }
      SysInformationColumnSource source;
      source.relation_object_uuid = table.table_uuid;
      source.schema_uuid = name->scope_uuid;
      source.column_name = column_name;
      source.ordinal_position = ordinal;
      source.datatype_name = datatype_name;
      source.is_nullable = "YES";
      source.catalog_generation_id = CatalogGenerationForNameEntry(*name, table.creator_tx);
      columns.push_back(std::move(source));
    }
  }

  for (const auto& index : readable_crud.indexes) {
    if (index.index_uuid.empty() || index.table_uuid.empty() ||
        !CrudCreatorVisible(readable_crud, index.creator_tx, index.event_sequence, observer_tx)) {
      continue;
    }
    const auto table_schema = table_schema_by_uuid.find(index.table_uuid);
    if (table_schema == table_schema_by_uuid.end()) { continue; }
    if (!object_uuids_in_projection.insert(index.index_uuid).second) { continue; }
    SysInformationCatalogObjectSource object;
    object.object_uuid = index.index_uuid;
    object.object_class = "index";
    object.schema_uuid = table_schema->second;
    object.parent_object_uuid = index.table_uuid;
    object.catalog_generation_id = index.creator_tx == 0 ? 1 : index.creator_tx;
    object.created_local_transaction_id = index.creator_tx;
    objects.push_back(std::move(object));
    if (names_by_object.find(index.index_uuid) == names_by_object.end() && !index.default_name.empty()) {
      AddResolverName(&resolver_names,
                      index.index_uuid,
                      "index",
                      index.table_uuid,
                      "en",
                      "primary",
                      index.default_name,
                      index.creator_tx);
    }
  }

  std::set<std::string> system_tables;
  for (const auto& table : BuiltinCatalogTableProfiles()) {
    if (!table.table_path.empty() &&
        !scratchbird::core::catalog::CatalogPathIsClusterScoped(table.table_path)) {
      system_tables.insert(table.table_path);
    }
  }

  std::set<std::string> system_views;
  for (const auto& definition : BuiltinSysInformationProjectionDefinitions()) {
    if (system_tables.find(definition.view_path) != system_tables.end()) {
      continue;
    }
    system_views.insert(definition.view_path);
    if (StartsWith(definition.view_path, "sys.information.")) {
      system_views.insert("sys.information_schema." +
                          definition.view_path.substr(std::string("sys.information.").size()));
    }
  }
  for (const auto& view_path : system_views) {
    AddSystemViewObject(&objects, &resolver_names, schema_uuid_by_path, view_path);
  }

  for (const auto& table_path : system_tables) {
    AddSystemTableObject(&objects, &resolver_names, schema_uuid_by_path, table_path);
  }

  const auto security = LoadSecurityPrincipalLifecycleState(catalog_read_context);
  if (security.ok) {
    AddSecurityPrincipalNavigatorObjects(&objects, &resolver_names, security.state);
  }

  std::string projection_path = OptionValue(request, "projection:");
  if (projection_path.empty()) {
    projection_path = OptionValue(request, "catalog_projection:");
  }
  if (projection_path.empty()) {
    projection_path = "sys.catalog_readable.object_tree";
  } else if (projection_path == "sys.catalog") {
    projection_path = "sys.catalog_readable.object_tree";
  }

  const auto projection = BuildSysInformationProjection(
      projection_path,
      projection_context,
      objects,
      resolver_names,
      {},
      {},
      columns);
  if (!projection.ok) {
    return MakeApiBehaviorDiagnostic<EngineShowCatalogResult>(
        request.context,
        "observability.show_catalog",
        MakeEngineApiDiagnostic(projection.diagnostic_code,
                                "observability.show_catalog.projection_failed",
                                projection.diagnostic_detail,
                                true));
  }
  for (const auto& row : projection.rows) {
    AddApiBehaviorRow(&result, row.fields);
  }
  AddApiBehaviorEvidence(&result, "catalog_projection", projection_path);
  AddApiBehaviorEvidence(&result, "catalog_rows", std::to_string(result.result_shape.rows.size()));
  result.result_shape.result_kind = projection_path + ".v1";
  return result;
}

}  // namespace

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_OBSERVABILITY_SHOW_API_BEHAVIOR
EngineShowVersionResult EngineShowVersion(const EngineShowVersionRequest& request) {
  return ShowBase<EngineShowVersionResult>(
      request,
      "observability.show_version",
      {{"product", "ScratchBird"}, {"component", "sb_engine"}, {"api", "1.0"}},
      "rs.show.version.v1");
}

EngineShowDatabaseResult EngineShowDatabase(const EngineShowDatabaseRequest& request) {
  return ShowBase<EngineShowDatabaseResult>(
      request,
      "observability.show_database",
      {{"database_path", request.context.database_path},
       {"database_uuid", request.context.database_uuid.canonical}},
      "rs.show.database.v1");
}

EngineShowSystemResult EngineShowSystem(const EngineShowSystemRequest& request) {
  return ShowBase<EngineShowSystemResult>(
      request,
      "observability.show_system",
      {{"cluster_authority", request.context.cluster_authority_available ? "active" : "inactive"},
       {"trust_mode", request.context.trust_mode == EngineTrustMode::embedded_in_process
                          ? "embedded"
                          : "server_isolated"}},
      "rs.show.system.v1");
}

EngineShowCatalogResult EngineShowCatalog(const EngineShowCatalogRequest& request) {
  const std::string projection = OptionValue(request, "projection:");
  const std::string catalog_projection = OptionValue(request, "catalog_projection:");
  const std::string requested_projection =
      projection.empty() ? catalog_projection : projection;
  const std::string canonical_projection =
      requested_projection == "sys.catalog"
          ? "sys.catalog_readable.object_tree"
          : SysInformationCanonicalViewPath(requested_projection);
  if (!requested_projection.empty() &&
      FindSysInformationProjectionDefinition(canonical_projection) != nullptr) {
    return BuildReadableCatalogProjectionResult(request);
  }

  auto result = MakeApiBehaviorSuccess<EngineShowCatalogResult>(request.context, "observability.show_catalog");
  const auto crud = LoadCrudState(request.context);
  if (crud.ok) {
    for (const auto& table : crud.state.tables) {
      if (CrudCreatorVisible(crud.state, table.creator_tx, table.event_sequence, request.context.local_transaction_id)) {
        AddApiBehaviorRow(&result, {{"object_uuid", table.table_uuid}, {"object_kind", "table"}, {"name", table.default_name}});
      }
    }
  }
  for (const auto& record : VisibleApiBehaviorRecords(request.context, {}, request.context.local_transaction_id)) {
    AddApiBehaviorRow(&result, {{"object_uuid", record.object_uuid}, {"object_kind", record.object_kind}, {"name", record.default_name}});
  }
  AddApiBehaviorEvidence(&result, "catalog_rows", std::to_string(result.result_shape.rows.size()));
  return result;
}

EngineShowSessionsResult EngineShowSessions(const EngineShowSessionsRequest& request) {
  return ShowBase<EngineShowSessionsResult>(
      request,
      "observability.show_sessions",
      {{"session_uuid", request.context.session_uuid.canonical},
       {"scope", request.context.security_context_present ? "all_or_self" : "self"}},
      "rs.show.sessions.v1");
}

EngineShowTransactionsResult EngineShowTransactions(const EngineShowTransactionsRequest& request) {
  auto result = MakeApiBehaviorSuccess<EngineShowTransactionsResult>(request.context, "observability.show_transactions");
  const auto crud = LoadCrudState(request.context);
  if (crud.ok) {
    for (const auto& [id, state] : crud.state.transactions) { AddApiBehaviorRow(&result, {{"local_transaction_id", std::to_string(id)}, {"state", state}}); }
  }
  AddApiBehaviorEvidence(&result, "transaction_rows", std::to_string(result.result_shape.rows.size()));
  AddPublicExactShowEvidence(&result, request, "observability.show_transactions", "rs.show.transactions.v1");
  return result;
}

EngineShowLocksResult EngineShowLocks(const EngineShowLocksRequest& request) {
  return ShowBase<EngineShowLocksResult>(
      request,
      "observability.show_locks",
      {{"lock_count", "0"}, {"scope", "local_node"}},
      "rs.show.locks.v1");
}

EngineShowStatementsResult EngineShowStatements(const EngineShowStatementsRequest& request) {
  return ShowBase<EngineShowStatementsResult>(
      request,
      "observability.show_statements",
      {{"statement_count", "0"}, {"scope", "local_node"}},
      "rs.show.statements.v1");
}

EngineShowJobsResult EngineShowJobs(const EngineShowJobsRequest& request) {
  auto result = MakeApiBehaviorSuccess<EngineShowJobsResult>(request.context, "observability.show_jobs");
  AddApiBehaviorEvidence(&result, "observability", "observability.show_jobs");
  AddApiBehaviorEvidence(&result, "jobs_rows", "1");
  AddApiBehaviorRow(&result,
                    {{"job_count", "0"},
                     {"scheduler_scope", "local_node"},
                     {"agent_visibility", request.context.security_context_present ? "authorized" : "self"}});
  AddPublicExactShowEvidence(&result, request, "observability.show_jobs", "rs.show.jobs.v1");
  return result;
}

EngineShowManagementResult EngineShowManagement(const EngineShowManagementRequest& request) {
  const auto surface = request.performance_optimization_snapshot_present
                           ? request.performance_optimization_snapshot
                           : DefaultPerformanceOptimizationSurfaceSnapshot();
  const auto validation =
      ValidatePerformanceOptimizationSurfaceSnapshot(surface);
  if (!validation.ok) {
    return MakeApiBehaviorDiagnostic<EngineShowManagementResult>(
        request.context,
        "observability.show_management",
        MakeEngineApiDiagnostic(validation.diagnostic_code,
                                "observability.performance_optimization.invalid_snapshot",
                                validation.detail,
                                true));
  }
  auto result =
      MakeApiBehaviorSuccess<EngineShowManagementResult>(request.context, "observability.show_management");
  AddApiBehaviorEvidence(&result, "observability", "observability.show_management");
  AddApiBehaviorEvidence(&result, "management_rows", "2");
  AddApiBehaviorEvidence(&result,
                         "management_performance_optimization_surface",
                         PerformanceOptimizationSurfaceSchemaId());
  AddApiBehaviorRow(&result,
                    {{"read_only_mode", request.context.read_only_mode ? "true" : "false"},
                     {"catalog_generation_id", std::to_string(request.context.catalog_generation_id)},
                     {"security_epoch", std::to_string(request.context.security_epoch)},
                     {"resource_epoch", std::to_string(request.context.resource_epoch)},
                     {"performance_optimization_surface", PerformanceOptimizationSurfaceSchemaId()},
                     {"optimization_profile", surface.optimization_profile},
                     {"copy_append_batching_enabled",
                      surface.copy_append_batching_enabled ? "true" : "false"},
                     {"plan_cache_enabled", surface.plan_cache_enabled ? "true" : "false"},
                     {"descriptor_metadata_cache_enabled",
                      surface.descriptor_metadata_cache_enabled ? "true" : "false"},
                     {"statistics_epoch", std::to_string(surface.statistics_epoch)},
                     {"cleanup_horizon_authority_status",
                      surface.cleanup_horizon_authority_status},
                     {"oldest_interesting_transaction_id",
                      std::to_string(surface.oldest_interesting_transaction_id)},
                     {"oldest_active_transaction_id",
                      std::to_string(surface.oldest_active_transaction_id)},
                     {"oldest_snapshot_transaction_id",
                      std::to_string(surface.oldest_snapshot_transaction_id)},
                     {"storage_row_version_backlog_count",
                      std::to_string(surface.storage_row_version_backlog_count)},
                     {"index_delta_backlog_count",
                      std::to_string(surface.index_delta_backlog_count)},
                     {"index_garbage_backlog_count",
                      std::to_string(surface.index_garbage_backlog_count)},
                     {"agent_worker_status", surface.agent_worker_status},
                     {"last_agent_decision", surface.last_agent_decision},
                     {"secondary_index_state", surface.secondary_index_state},
                     {"shadow_index_state", surface.shadow_index_state},
                     {"summary_index_state", surface.summary_index_state},
                     {"specialized_index_state", surface.specialized_index_state},
                     {"exact_refusal_diagnostic_code",
                      surface.exact_refusal_diagnostic_code},
                     {"exact_refusal_message_vector",
                      surface.exact_refusal_message_vector},
                     {"support_bundle_completeness_state",
                      surface.support_bundle_completeness_state},
                     {"resource_governor_state", surface.resource_governor_state},
                     {"odf108_surface_ready", "true"},
                     {"odf108_selected_path_count",
                      std::to_string(surface.odf108_selected_paths.size())},
                     {"odf108_feature_gate_count",
                      std::to_string(surface.odf108_feature_gates.size())},
                     {"odf108_fallback_reason_count",
                      std::to_string(surface.odf108_fallbacks.size())},
                     {"odf108_quota_state_count",
                      std::to_string(surface.odf108_quotas.size())},
                     {"odf108_runtime_compatibility_count",
                      std::to_string(
                          surface.odf108_runtime_compatibility.size())},
                     {"odf108_rebuild_state_count",
                      std::to_string(surface.odf108_rebuild_states.size())},
                     {"odf108_exact_refusal_count",
                      std::to_string(surface.odf108_exact_refusals.size())},
                     {"parser_finality_authority",
                      surface.parser_finality_authority ? "true" : "false"},
                     {"reference_finality_authority",
                      surface.reference_finality_authority ? "true" : "false"},
                     {"wal_recovery_authority",
                      surface.wal_recovery_authority ? "true" : "false"}});
  AddPerformanceOptimizationSurfaceRow(&result, surface);
  return result;
}

EngineShowDiagnosticsResult EngineShowDiagnostics(const EngineShowDiagnosticsRequest& request) {
  auto result =
      MakeApiBehaviorSuccess<EngineShowDiagnosticsResult>(request.context, "observability.show_diagnostics");
  AddApiBehaviorEvidence(&result, "observability", "observability.show_diagnostics");
  AddApiBehaviorEvidence(&result, "diagnostic_rows", "1");
  AddApiBehaviorRow(&result,
                    {{"current_sqlstate", request.context.current_sqlstate},
                     {"diagnostic_uuid", request.context.current_diagnostic_uuid.canonical},
                     {"statement_uuid", request.context.statement_uuid.canonical}});
  AddSbsfc080Evidence(&result, request);
  return result;
}

EngineShowDiagnosticsExtendedResult EngineShowDiagnosticsExtended(
    const EngineShowDiagnosticsExtendedRequest& request) {
  auto result = MakeApiBehaviorSuccess<EngineShowDiagnosticsExtendedResult>(
      request.context, "observability.show_diagnostics_extended");
  AddApiBehaviorEvidence(&result, "observability", "observability.show_diagnostics_extended");
  AddApiBehaviorEvidence(&result, "diagnostic_extended_rows", "1");
  AddApiBehaviorRow(&result,
                    {{"current_sqlstate", request.context.current_sqlstate},
                     {"diagnostic_uuid", request.context.current_diagnostic_uuid.canonical},
                     {"statement_uuid", request.context.statement_uuid.canonical},
                     {"request_id", request.context.request_id},
                     {"trace_tag_count", std::to_string(request.context.trace_tags.size())}});
  AddSbsfc080Evidence(&result, request);
  return result;
}

EngineShowArchiveReplicationResult EngineShowArchiveReplication(
    const EngineShowArchiveReplicationRequest& request) {
  auto result = MakeApiBehaviorSuccess<EngineShowArchiveReplicationResult>(
      request.context, "observability.show_archive_replication");
  AddApiBehaviorEvidence(&result, "observability", "observability.show_archive_replication");
  AddApiBehaviorEvidence(&result, "archive_replication_rows", "1");
  AddApiBehaviorRow(&result,
                    {{"database_uuid", request.context.database_uuid.canonical},
                     {"archive_mode", "local_mga_inventory"},
                     {"replication_channels", "0"},
                     {"cluster_authority", request.context.cluster_authority_available ? "active" : "inactive"}});
  return result;
}

EngineShowAgentsExtendedResult EngineShowAgentsExtended(
    const EngineShowAgentsExtendedRequest& request) {
  auto result = MakeApiBehaviorSuccess<EngineShowAgentsExtendedResult>(
      request.context, "observability.show_agents_extended");
  const auto& registry = scratchbird::core::agents::CanonicalAgentRegistry();
  AddApiBehaviorEvidence(&result, "observability", "observability.show_agents_extended");
  AddApiBehaviorEvidence(&result, "agent_registry", "canonical");
  AddApiBehaviorEvidence(&result, "agent_count", std::to_string(registry.size()));
  for (const auto& agent : registry) {
    AddApiBehaviorRow(&result,
                      {{"agent_type", agent.type_id},
                       {"deployment", scratchbird::core::agents::AgentDeploymentName(agent.deployment)},
                       {"scope", agent.scope},
                       {"authority", scratchbird::core::agents::AgentAuthorityClassName(agent.authority)},
                       {"cluster_only", agent.cluster_only ? "true" : "false"},
                       {"cluster_authority",
                        request.context.cluster_authority_available ? "active" : "inactive"}});
  }
  if (registry.empty()) {
    AddApiBehaviorRow(&result,
                      {{"agent_type", "none"},
                       {"deployment", "local"},
                       {"scope", "local_node"},
                       {"authority", "engine"},
                       {"cluster_only", "false"},
                       {"cluster_authority",
                        request.context.cluster_authority_available ? "active" : "inactive"}});
  }
  return result;
}

EngineShowFilespaceExtendedResult EngineShowFilespaceExtended(
    const EngineShowFilespaceExtendedRequest& request) {
  auto result = MakeApiBehaviorSuccess<EngineShowFilespaceExtendedResult>(
      request.context, "observability.show_filespace_extended");
  AddApiBehaviorEvidence(&result, "observability", "observability.show_filespace_extended");
  AddApiBehaviorEvidence(&result, "filespace_rows", "1");
  AddApiBehaviorRow(&result,
                    {{"database_uuid", request.context.database_uuid.canonical},
                     {"database_path", request.context.database_path},
                     {"filespace_scope", "primary_database"},
                     {"mga_finality_authority", "local_transaction_inventory"},
                     {"storage_visibility", "engine_owned"},
                     {"cluster_authority",
                      request.context.cluster_authority_available ? "active" : "inactive"}});
  return result;
}

EngineShowDecisionServiceResult EngineShowDecisionService(
    const EngineShowDecisionServiceRequest& request) {
  auto result = MakeApiBehaviorSuccess<EngineShowDecisionServiceResult>(
      request.context, "observability.show_decision_service");
  AddApiBehaviorEvidence(&result, "observability", "observability.show_decision_service");
  AddApiBehaviorEvidence(&result, "decision_service_rows", "1");
  AddApiBehaviorRow(&result,
                    {{"database_uuid", request.context.database_uuid.canonical},
                     {"cluster_uuid", request.context.cluster_uuid.canonical},
                     {"decision_service_scope",
                      request.context.cluster_authority_available ? "cluster_provider" : "local_node"},
                     {"decision_service_state",
                      request.context.cluster_authority_available ? "provider_available" : "not_enabled"},
                     {"provider_boundary", "compile_gated_cluster_provider"},
                     {"engine_mode",
                      request.context.cluster_authority_available ? "cluster_enabled" : "standalone"}});
  return result;
}

EngineShowAccelerationResult EngineShowAcceleration(const EngineShowAccelerationRequest& request) {
  auto result = MakeApiBehaviorSuccess<EngineShowAccelerationResult>(
      request.context, "observability.show_acceleration");
  AddApiBehaviorEvidence(&result, "observability", "observability.show_acceleration");
  AddApiBehaviorEvidence(&result, "acceleration_rows", "1");
  AddApiBehaviorRow(&result,
                    {{"provider_count", "0"},
                     {"runtime_mode", "interpreted_sblr"},
                     {"node_uuid", request.context.node_uuid.canonical}});
  AddSbsfc080Evidence(&result, request);
  return result;
}

EngineShowAccelerationExtendedResult EngineShowAccelerationExtended(
    const EngineShowAccelerationExtendedRequest& request) {
  auto result = MakeApiBehaviorSuccess<EngineShowAccelerationExtendedResult>(
      request.context, "observability.show_acceleration_extended");
  AddApiBehaviorEvidence(&result, "observability", "observability.show_acceleration_extended");
  AddApiBehaviorEvidence(&result, "acceleration_extended_rows", "1");
  AddApiBehaviorRow(&result,
                    {{"provider_count", "0"},
                     {"runtime_mode", "interpreted_sblr"},
                     {"llvm_module_count", "0"},
                     {"gpu_queue_count", "0"},
                     {"node_uuid", request.context.node_uuid.canonical}});
  AddSbsfc080Evidence(&result, request);
  return result;
}

EngineInspectShowOperationResult EngineInspectShowOperation(
    const EngineInspectShowOperationRequest& request) {
  const std::string operation_id = OperationIdOr(request, "observability.show_operation");
  auto result =
      MakeApiBehaviorSuccess<EngineInspectShowOperationResult>(request.context, operation_id);
  AddShowOperationResult(&result,
                         request,
                         operation_id,
                         ResultShapeContract(request, "rs.show.context.v1"));
  return result;
}

}  // namespace scratchbird::engine::internal_api
