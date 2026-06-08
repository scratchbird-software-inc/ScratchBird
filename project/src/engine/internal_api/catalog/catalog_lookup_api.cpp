// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/catalog_lookup_api.hpp"

#include "behavior_support/api_behavior_store.hpp"
#include "catalog/catalog_object_lifecycle.hpp"
#include "catalog/name_registry.hpp"
#include "catalog/schema_tree_api.hpp"
#include "crud_support/crud_store.hpp"
#include "domain_support/domain_store.hpp"
#include "mga_relation_store/mga_relation_store.hpp"

namespace scratchbird::engine::internal_api {

namespace {

std::string PublicDisplayNameForObject(const EngineApiRequest& request,
                                       const std::string& object_uuid,
                                       const std::string& object_kind) {
  const auto mapped = MapNameRegistryUuidToNamePublic(request, object_uuid, object_kind);
  if (mapped.ok) { return mapped.entry.display_name; }
  return {};
}

}  // namespace

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_CATALOG_CATALOG_LOOKUP_API_BEHAVIOR
EngineLookupObjectResult EngineLookupObject(const EngineLookupObjectRequest& request) {
  if (request.target_object.uuid.canonical.empty() && request.localized_names.empty()) {
    return MakeApiBehaviorDiagnostic<EngineLookupObjectResult>(request.context, "catalog.lookup_object", MakeInvalidRequestDiagnostic("catalog.lookup_object", "target_object_uuid_required"));
  }
  if (!request.localized_names.empty() && request.target_object.uuid.canonical.empty()) {
    const auto resolved = ResolveNameRegistryPublic(request, "schema");
    if (!resolved.ok) {
      return MakeApiBehaviorDiagnostic<EngineLookupObjectResult>(request.context, "catalog.lookup_object", resolved.diagnostic);
    }
    if (resolved.matches.size() > 1) {
      return MakeApiBehaviorDiagnostic<EngineLookupObjectResult>(
          request.context,
          "catalog.lookup_object",
          MakeInvalidRequestDiagnostic("catalog.lookup_object", "schema_path_ambiguous"));
    }
    if (resolved.matches.size() == 1) {
      auto result = MakeApiBehaviorSuccess<EngineLookupObjectResult>(request.context, "catalog.lookup_object");
      result.primary_object.uuid.canonical = resolved.matches.front().object_uuid;
      result.primary_object.object_kind = "schema";
      AddApiBehaviorRow(&result, {{"object_uuid", resolved.matches.front().object_uuid},
                                  {"object_kind", "schema"},
                                  {"name", resolved.matches.front().display_name},
                                  {"payload", "name_registry=true"}});
      AddApiBehaviorEvidence(&result, "name_resolved_to_uuid", resolved.matches.front().object_uuid);
      return result;
    }
  }
  if (const auto schema = FindVisibleSchemaTreeRecord(request.context,
                                                     request.target_object.uuid.canonical,
                                                     request.context.local_transaction_id)) {
    const std::string display_name = PublicDisplayNameForObject(request, schema->schema_uuid, "schema");
    auto result = MakeApiBehaviorSuccess<EngineLookupObjectResult>(request.context, "catalog.lookup_object");
    result.primary_object.uuid.canonical = schema->schema_uuid;
    result.primary_object.object_kind = "schema";
    AddApiBehaviorRow(&result, {{"object_uuid", schema->schema_uuid},
                                {"object_kind", "schema"},
                                {"name", display_name},
                                {"payload", schema->payload}});
    return result;
  }
  const auto record = FindVisibleApiBehaviorRecord(request.context, request.target_object.uuid.canonical, request.context.local_transaction_id);
  auto result = MakeApiBehaviorSuccess<EngineLookupObjectResult>(request.context, "catalog.lookup_object");
  if (record) {
    const std::string display_name = PublicDisplayNameForObject(request, record->object_uuid, record->object_kind);
    result.primary_object.uuid.canonical = record->object_uuid;
    result.primary_object.object_kind = record->object_kind;
    AddApiBehaviorRow(&result, {{"object_uuid", record->object_uuid}, {"object_kind", record->object_kind}, {"name", display_name}});
    return result;
  }
  const auto crud = LoadCrudState(request.context);
  if (crud.ok) {
    if (const auto table = FindVisibleCrudTable(crud.state, request.target_object.uuid.canonical, request.context.local_transaction_id)) {
      const std::string display_name = PublicDisplayNameForObject(request, table->table_uuid, "table");
      result.primary_object.uuid.canonical = table->table_uuid;
      result.primary_object.object_kind = "table";
      AddApiBehaviorRow(&result, {{"object_uuid", table->table_uuid}, {"object_kind", "table"}, {"name", display_name}});
      return result;
    }
  }
  const auto mga_relations = LoadMgaRelationStoreState(request.context);
  if (mga_relations.ok) {
    const CrudState mga_crud = BuildCrudCompatibilityStateFromMga(mga_relations.state);
    if (const auto table = FindVisibleCrudTable(mga_crud, request.target_object.uuid.canonical, request.context.local_transaction_id)) {
      const std::string display_name = PublicDisplayNameForObject(request, table->table_uuid, "table");
      result.primary_object.uuid.canonical = table->table_uuid;
      result.primary_object.object_kind = "table";
      AddApiBehaviorRow(&result, {{"object_uuid", table->table_uuid},
                                  {"object_kind", "table"},
                                  {"name", display_name},
                                  {"payload", "mga_relation_metadata=true"}});
      return result;
    }
    for (const auto& table : mga_crud.tables) {
      if (!CrudCreatorVisible(mga_crud, table.creator_tx, table.event_sequence, request.context.local_transaction_id)) {
        continue;
      }
      for (const auto& index : VisibleCrudIndexesForTable(mga_crud, table.table_uuid, request.context.local_transaction_id)) {
        if (index.index_uuid == request.target_object.uuid.canonical) {
          const std::string display_name = PublicDisplayNameForObject(request, index.index_uuid, "index");
          result.primary_object.uuid.canonical = index.index_uuid;
          result.primary_object.object_kind = "index";
          AddApiBehaviorRow(&result, {{"object_uuid", index.index_uuid},
                                      {"object_kind", "index"},
                                      {"name", display_name},
                                      {"payload", "mga_relation_metadata=true"}});
          return result;
        }
      }
    }
  }
  if (const auto domain = FindVisibleDomain(request.context, request.target_object.uuid.canonical, request.context.local_transaction_id)) {
    const std::string display_name = PublicDisplayNameForObject(request, domain->domain_uuid, "domain");
    result.primary_object.uuid.canonical = domain->domain_uuid;
    result.primary_object.object_kind = "domain";
    AddApiBehaviorRow(&result, {{"object_uuid", domain->domain_uuid}, {"object_kind", "domain"}, {"name", display_name}});
    return result;
  }
  return MakeApiBehaviorDiagnostic<EngineLookupObjectResult>(request.context, "catalog.lookup_object", MakeInvalidRequestDiagnostic("catalog.lookup_object", "object_not_visible"));
}

EngineGetDependenciesResult EngineGetDependencies(const EngineGetDependenciesRequest& request) {
  auto result = MakeApiBehaviorSuccess<EngineGetDependenciesResult>(request.context, "catalog.get_dependencies");
  const auto catalog_objects = LoadCatalogObjectLifecycleState(request.context);
  if (catalog_objects.ok) {
    for (const auto& dependency : catalog_objects.state.dependencies) {
      if (!request.target_object.uuid.canonical.empty() &&
          dependency.source_uuid != request.target_object.uuid.canonical &&
          dependency.dependency_uuid != request.target_object.uuid.canonical) {
        continue;
      }
      AddApiBehaviorRow(&result, {{"source_uuid", dependency.source_uuid},
                                  {"dependency_uuid", dependency.dependency_uuid},
                                  {"dependency_kind", dependency.dependency_kind},
                                  {"source_kind", dependency.source_kind}});
    }
  }
  if (request.target_object.object_kind == "schema" && !request.target_object.uuid.canonical.empty()) {
    for (const auto& schema : VisibleSchemaTreeRecords(request.context, request.context.local_transaction_id)) {
      if (schema.parent_schema_uuid == request.target_object.uuid.canonical) {
        const std::string display_name = PublicDisplayNameForObject(request, schema.schema_uuid, "schema");
        AddApiBehaviorRow(&result, {{"source_uuid", request.target_object.uuid.canonical},
                                    {"dependency_uuid", schema.schema_uuid},
                                    {"dependency_kind", "child_schema"},
                                    {"name", display_name}});
      }
    }
    const auto domains = LoadDomainState(request.context);
    if (domains.ok) {
      for (const auto& domain : domains.domains) {
        const auto visible = FindVisibleDomain(request.context, domain.domain_uuid, request.context.local_transaction_id);
        if (visible && visible->schema_uuid == request.target_object.uuid.canonical) {
          const std::string display_name = PublicDisplayNameForObject(request, visible->domain_uuid, "domain");
          AddApiBehaviorRow(&result, {{"source_uuid", request.target_object.uuid.canonical},
                                      {"dependency_uuid", visible->domain_uuid},
                                      {"dependency_kind", "domain"},
                                      {"name", display_name}});
        }
      }
    }
  }
  for (const auto& object : request.related_objects) {
    AddApiBehaviorRow(&result, {{"source_uuid", request.target_object.uuid.canonical}, {"dependency_uuid", object.uuid.canonical}, {"dependency_kind", object.object_kind}});
  }
  AddApiBehaviorEvidence(&result, "dependency_scan", std::to_string(request.related_objects.size()));
  return result;
}

}  // namespace scratchbird::engine::internal_api
