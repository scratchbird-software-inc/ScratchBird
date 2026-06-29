// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/descriptor_mutation_api.hpp"

#include "api_diagnostics.hpp"
#include "behavior_support/api_behavior_store.hpp"
#include "catalog/name_registry.hpp"
#include "catalog/pinned_descriptor_cache.hpp"

namespace scratchbird::engine::internal_api {
namespace {

std::string OptionValue(const EngineApiRequest& request, const std::string& prefix) {
  for (const auto& option : request.option_envelopes) {
    if (option.rfind(prefix, 0) == 0) return option.substr(prefix.size());
  }
  return {};
}

std::string MutationObjectKind(const EngineApiRequest& request) {
  if (!request.target_object.object_kind.empty()) return request.target_object.object_kind;
  const auto from_option = OptionValue(request, "target_object_kind:");
  return from_option.empty() ? "catalog_descriptor" : from_option;
}

bool DescriptorUsesRootNameScope(const std::string& object_kind) {
  return object_kind == "filespace" || object_kind == "filespace_agent";
}

std::string DescriptorNameScopeUuid(const EngineCatalogDescriptorMutationRequest& request,
                                    const std::string& object_kind) {
  if (!request.target_schema.uuid.canonical.empty()) {
    return request.target_schema.uuid.canonical;
  }
  if (DescriptorUsesRootNameScope(object_kind)) {
    return {};
  }
  return request.target_object.uuid.canonical;
}

}  // namespace

EngineCatalogDescriptorMutationResult EngineCatalogDescriptorMutation(
    const EngineCatalogDescriptorMutationRequest& request) {
  const std::string operation_id =
      request.operation_id.empty() ? "catalog.mutation.descriptor" : request.operation_id;
  if (request.context.read_only_mode) {
    return MakeApiBehaviorDiagnostic<EngineCatalogDescriptorMutationResult>(
        request.context,
        operation_id,
        MakeInvalidRequestDiagnostic(operation_id, "catalog_descriptor_mutation_read_only_context"));
  }

  if (operation_id == "catalog.mutation.refresh_materialized_view") {
    if (request.target_object.uuid.canonical.empty()) {
      return MakeApiBehaviorDiagnostic<EngineCatalogDescriptorMutationResult>(
          request.context,
          operation_id,
          MakeInvalidRequestDiagnostic(operation_id, "catalog_descriptor_target_uuid_required"));
    }
    auto result = MakeApiBehaviorSuccess<EngineCatalogDescriptorMutationResult>(
        request.context, operation_id);
    result.primary_object.uuid.canonical = request.target_object.uuid.canonical;
    result.primary_object.object_kind =
        request.target_object.object_kind.empty() ? "materialized_view"
                                                  : request.target_object.object_kind;
    AddApiBehaviorEvidence(&result, "catalog_descriptor_mutation", operation_id);
    AddApiBehaviorEvidence(&result, "materialized_view_refresh", result.primary_object.uuid.canonical);
    AddApiBehaviorEvidence(&result, "mga_catalog_commit",
                           std::to_string(request.context.local_transaction_id));
    AddApiBehaviorEvidence(&result, "security_context",
                           request.context.security_context_present ? "present" : "missing");
    auto invalidation = CatalogPinnedDescriptorInvalidationEventForMutation(
        "ddl_catalog_mutation",
        result.primary_object.uuid.canonical,
        request.context.catalog_generation_id);
    invalidation.reason = "materialized_view_refresh";
    const auto invalidated = GlobalCatalogPinnedDescriptorCache().Invalidate(invalidation);
    AddApiBehaviorEvidence(&result,
                           "catalog_pinned_descriptor_cache_invalidated",
                           std::to_string(invalidated.invalidated_entries.size()));
    AddApiBehaviorRow(&result,
                      {{"operation_id", operation_id},
                       {"object_uuid", result.primary_object.uuid.canonical},
                       {"object_kind", result.primary_object.object_kind},
                       {"catalog_authority", OptionValue(request, "catalog_authority:")},
                       {"descriptor_ref", OptionValue(request, "descriptor_ref:")},
                       {"refresh_generation_visible", "true"},
                       {"descriptor_replaced", "false"},
                       {"mga_catalog_commit_required", "true"},
                       {"parser_executes_sql", "false"}});
    AddDdlPublicationResult(&result,
                            operation_id,
                            result.primary_object.object_kind,
                            result.primary_object.uuid.canonical);
    return result;
  }

  const std::string object_kind = MutationObjectKind(request);
  auto result = PersistedRecordResult<EngineCatalogDescriptorMutationResult>(
      request,
      operation_id,
      object_kind,
      true,
      "descriptor_mutation_committed");
  if (!result.ok) return result;

  const std::string fallback_name = ApiBehaviorPrimaryName(request, object_kind);
  const std::string scope_uuid = DescriptorNameScopeUuid(request, object_kind);
  const auto name_appended = PersistNameRegistryEntriesForObject(request.context,
                                                                operation_id,
                                                                result.primary_object.uuid.canonical,
                                                                result.primary_object.object_kind,
                                                                scope_uuid,
                                                                request.localized_names,
                                                                fallback_name);
  if (name_appended.error) {
    return MakeApiBehaviorDiagnostic<EngineCatalogDescriptorMutationResult>(
        request.context, operation_id, name_appended);
  }

  AddApiBehaviorEvidence(&result, "catalog_descriptor_mutation", operation_id);
  AddApiBehaviorEvidence(&result, "name_registry", result.primary_object.uuid.canonical);
  AddApiBehaviorEvidence(&result, "mga_catalog_commit", std::to_string(request.context.local_transaction_id));
  AddApiBehaviorEvidence(&result, "security_context", request.context.security_context_present ? "present" : "missing");
  auto invalidation = CatalogPinnedDescriptorInvalidationEventForMutation(
      "ddl_catalog_mutation",
      result.primary_object.uuid.canonical,
      request.context.catalog_generation_id);
  invalidation.reason = "ddl_catalog_mutation";
  const auto invalidated = GlobalCatalogPinnedDescriptorCache().Invalidate(invalidation);
  AddApiBehaviorEvidence(&result,
                         "catalog_pinned_descriptor_cache_invalidated",
                         std::to_string(invalidated.invalidated_entries.size()));
  AddApiBehaviorRow(&result,
                    {{"operation_id", operation_id},
                     {"object_uuid", result.primary_object.uuid.canonical},
                     {"object_kind", result.primary_object.object_kind},
                     {"catalog_authority", OptionValue(request, "catalog_authority:")},
                     {"descriptor_ref", OptionValue(request, "descriptor_ref:")},
                     {"mga_catalog_commit_required", "true"},
                     {"parser_executes_sql", "false"}});
  return result;
}

}  // namespace scratchbird::engine::internal_api
