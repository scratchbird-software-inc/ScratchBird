// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/name_resolution_api.hpp"

#include "behavior_support/api_behavior_store.hpp"
#include "catalog/catalog_object_lifecycle.hpp"
#include "catalog/name_registry.hpp"
#include "crud_support/crud_store.hpp"
#include "domain_support/domain_store.hpp"

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_CATALOG_NAME_RESOLUTION_API_BEHAVIOR
EngineResolveNameResult EngineResolveName(const EngineResolveNameRequest& request) {
  EngineCatalogResolveObjectNameRequest catalog_request;
  static_cast<EngineApiRequest&>(catalog_request) = static_cast<const EngineApiRequest&>(request);
  const auto catalog_resolved = EngineCatalogResolveObjectName(catalog_request);
  if (catalog_resolved.ok) {
    auto result = MakeApiBehaviorSuccess<EngineResolveNameResult>(request.context, "catalog.resolve_name");
    result.primary_object = catalog_resolved.primary_object;
    result.bound_object_identity = catalog_resolved.bound_object_identity;
    result.result_shape = catalog_resolved.result_shape;
    result.evidence = catalog_resolved.evidence;
    AddApiBehaviorEvidence(&result, "catalog_object_lifecycle_resolver", "SBCATOBJ1");
    return result;
  }
  if (!catalog_resolved.diagnostics.empty() &&
      catalog_resolved.diagnostics.front().code.rfind("CATALOG.SYNONYM_", 0) == 0) {
    return MakeApiBehaviorDiagnostic<EngineResolveNameResult>(
        request.context,
        "catalog.resolve_name",
        catalog_resolved.diagnostics.front());
  }
  const auto resolved = ResolveNameRegistryPublic(request, request.sql_object_reference.expected_object_type);
  if (!resolved.ok) {
    return MakeApiBehaviorDiagnostic<EngineResolveNameResult>(request.context, "catalog.resolve_name", resolved.diagnostic);
  }
  auto result = MakeApiBehaviorSuccess<EngineResolveNameResult>(request.context, "catalog.resolve_name");
  if (resolved.matches.size() > 1) {
    return MakeApiBehaviorDiagnostic<EngineResolveNameResult>(
        request.context,
        "catalog.resolve_name",
        MakeEngineApiDiagnostic("CATALOG.NAME.AMBIGUOUS", "catalog.name.ambiguous", "ambiguous_name"));
  }
  const auto& match = resolved.matches.front();
  result.primary_object.uuid.canonical = match.object_uuid;
  result.primary_object.object_kind = match.object_class;
  result.bound_object_identity.object_uuid.canonical = match.object_uuid;
  result.bound_object_identity.resolved_object_type = match.object_class;
  result.bound_object_identity.resolved_schema_uuid.canonical = match.scope_uuid;
  result.bound_object_identity.parent_object_uuid.canonical = match.parent_object_uuid;
  result.bound_object_identity.catalog_generation_id = match.catalog_generation_id;
  result.bound_object_identity.security_epoch = request.context.security_epoch;
  result.bound_object_identity.resource_epoch = match.resource_epoch;
  AddApiBehaviorRow(&result, {{"object_uuid", match.object_uuid},
                              {"object_kind", match.object_class},
                              {"name", match.display_name},
                              {"scope_uuid", match.scope_uuid},
                              {"identifier_profile_uuid", match.identifier_profile_uuid},
                              {"language_tag", match.language_tag}});
  AddApiBehaviorEvidence(&result, "name_resolution", match.normalized_lookup_key);
  AddApiBehaviorEvidence(&result, "name_entry", match.name_entry_uuid);
  return result;
}

EngineMapUuidToNameResult EngineMapUuidToName(const EngineMapUuidToNameRequest& request) {
  const auto mapped = MapNameRegistryUuidToNamePublic(request,
                                                     request.target_object.uuid.canonical,
                                                     request.target_object.object_kind);
  if (!mapped.ok) {
    return MakeApiBehaviorDiagnostic<EngineMapUuidToNameResult>(
        request.context,
        "catalog.map_uuid_to_name",
        mapped.diagnostic);
  }
  auto result = MakeApiBehaviorSuccess<EngineMapUuidToNameResult>(request.context, "catalog.map_uuid_to_name");
  result.primary_object.uuid.canonical = mapped.entry.object_uuid;
  result.primary_object.object_kind = mapped.entry.object_class;
  result.bound_object_identity.object_uuid.canonical = mapped.entry.object_uuid;
  result.bound_object_identity.resolved_object_type = mapped.entry.object_class;
  result.bound_object_identity.resolved_schema_uuid.canonical = mapped.entry.scope_uuid;
  result.bound_object_identity.parent_object_uuid.canonical = mapped.entry.parent_object_uuid;
  result.bound_object_identity.catalog_generation_id = mapped.entry.catalog_generation_id;
  result.bound_object_identity.security_epoch = request.context.security_epoch;
  result.bound_object_identity.resource_epoch = mapped.entry.resource_epoch;
  AddApiBehaviorRow(&result, {{"object_uuid", mapped.entry.object_uuid},
                              {"object_kind", mapped.entry.object_class},
                              {"name", mapped.entry.display_name},
                              {"raw_name_text", mapped.entry.raw_name_text},
                              {"scope_uuid", mapped.entry.scope_uuid},
                              {"identifier_profile_uuid", mapped.entry.identifier_profile_uuid},
                              {"language_tag", mapped.entry.language_tag},
                              {"name_class", mapped.entry.name_class},
                              {"was_quoted", mapped.entry.was_quoted ? "true" : "false"},
                              {"requires_exact_match", mapped.entry.requires_exact_match ? "true" : "false"}});
  AddApiBehaviorEvidence(&result, "uuid_to_name", mapped.entry.object_uuid);
  AddApiBehaviorEvidence(&result, "name_entry", mapped.entry.name_entry_uuid);
  return result;
}

}  // namespace scratchbird::engine::internal_api
