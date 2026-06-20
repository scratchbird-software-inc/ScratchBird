// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ddl/drop_api.hpp"

#include "catalog/catalog_object_lifecycle.hpp"
#include "catalog/name_registry.hpp"
#include "crud_support/crud_store.hpp"
#include "domain_support/domain_store.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "behavior_support/api_behavior_store.hpp"

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_DDL_DROP_API_BEHAVIOR
EngineDropObjectResult EngineDropObject(const EngineDropObjectRequest& request) {
  const std::string kind = request.target_object.object_kind.empty() ? "object" : request.target_object.object_kind;
  if (kind == "synonym") {
    EngineCatalogDropObjectRequest catalog_request;
    static_cast<EngineApiRequest&>(catalog_request) = request;
    catalog_request.operation_id = "ddl.drop_object";
    catalog_request.target_object.object_kind = "synonym";
    const auto dropped = EngineCatalogDropObject(catalog_request);
    EngineDropObjectResult result;
    result.ok = dropped.ok;
    result.operation_id = "ddl.drop_object";
    result.diagnostics = dropped.diagnostics;
    result.unsupported_features = dropped.unsupported_features;
    result.evidence = dropped.evidence;
    result.result_shape = dropped.result_shape;
    result.primary_object = dropped.primary_object;
    result.catalog_row_uuid = dropped.catalog_row_uuid;
    result.transaction_uuid = dropped.transaction_uuid;
    result.local_transaction_id = dropped.local_transaction_id;
    result.embedded_trust_mode_observed = dropped.embedded_trust_mode_observed;
    result.cluster_authority_required = dropped.cluster_authority_required;
    if (result.ok) {
      result.evidence.push_back({"ddl_catalog_route", "sys.catalog.synonym"});
      AddDdlPublicationResult(&result,
                              "ddl.drop_object",
                              "synonym",
                              result.primary_object.uuid.canonical,
                              result.catalog_row_uuid.canonical,
                              "catalog.synonym");
    }
    return result;
  }
  if (kind == "domain") {
    if (request.context.local_transaction_id == 0) {
      return MakeCrudDiagnosticResult<EngineDropObjectResult>(
          request.context,
          "ddl.drop_object",
          MakeInvalidRequestDiagnostic("ddl.drop_object", "local_transaction_id_required"));
    }
    if (request.target_object.uuid.canonical.empty()) {
      return MakeCrudDiagnosticResult<EngineDropObjectResult>(
          request.context,
          "ddl.drop_object",
          MakeInvalidRequestDiagnostic("ddl.drop_object", "target_domain_uuid_required"));
    }
    if (!FindVisibleDomain(request.context, request.target_object.uuid.canonical, request.context.local_transaction_id)) {
      return MakeCrudDiagnosticResult<EngineDropObjectResult>(
          request.context,
          "ddl.drop_object",
          MakeInvalidRequestDiagnostic("ddl.drop_object", "target_domain_not_visible"));
    }
    if (DomainHasCrudDependencies(request.context, request.target_object.uuid.canonical, request.context.local_transaction_id)) {
      return MakeCrudDiagnosticResult<EngineDropObjectResult>(
          request.context,
          "ddl.drop_object",
          MakeInvalidRequestDiagnostic("ddl.drop_object", "domain_has_dependencies"));
    }
    const auto appended = AppendDomainEvent(
        request.context,
        MakeDomainDropEvent(request.context.local_transaction_id, request.target_object.uuid.canonical));
    if (appended.error) {
      return MakeCrudDiagnosticResult<EngineDropObjectResult>(request.context, "ddl.drop_object", appended);
    }
    const auto retired = RetireNameRegistryEntriesForObject(request.context,
                                                           "ddl.drop_object",
                                                           request.target_object.uuid.canonical);
    if (retired.error) {
      return MakeCrudDiagnosticResult<EngineDropObjectResult>(request.context, "ddl.drop_object", retired);
    }
    auto result = MakeCrudSuccessResult<EngineDropObjectResult>(request.context, "ddl.drop_object");
    result.primary_object = request.target_object;
    result.evidence.push_back({"domain_event", "domain_drop"});
    AddDdlPublicationResult(&result,
                            "ddl.drop_object",
                            "domain",
                            request.target_object.uuid.canonical,
                            result.catalog_row_uuid.canonical,
                            "domain_event");
    return result;
  }
  if (kind == "table" || kind == "relation") {
    const std::string object_uuid = request.target_object.uuid.canonical;
    auto temporary_drop = DropMgaTemporaryTable(request.context, object_uuid);
    if (!temporary_drop.ok) {
      if (temporary_drop.target_was_temporary) {
        return MakeCrudDiagnosticResult<EngineDropObjectResult>(
            request.context,
            "ddl.drop_object",
            temporary_drop.diagnostic);
      }
    } else if (temporary_drop.target_was_temporary) {
      const auto retired = RetireNameRegistryEntriesForObject(
          request.context,
          "ddl.drop_object",
          object_uuid);
      if (retired.error) {
        return MakeCrudDiagnosticResult<EngineDropObjectResult>(
            request.context,
            "ddl.drop_object",
            retired);
      }
      auto result = MakeCrudSuccessResult<EngineDropObjectResult>(
          request.context,
          "ddl.drop_object");
      result.primary_object = request.target_object;
      if (result.primary_object.object_kind.empty()) {
        result.primary_object.object_kind = "table";
      }
      result.evidence.push_back({"name_registry_retired", object_uuid});
      result.evidence.push_back({"temporary_metadata_retired",
                                 temporary_drop.metadata_retired ? "true" : "false"});
      result.evidence.push_back({"temporary_object_scope",
                                 temporary_drop.temporary_scope});
      result.evidence.push_back({"temporary_drop_deleted_rows",
                                 std::to_string(temporary_drop.deleted_row_count)});
      result.evidence.push_back(
          {"temporary_drop_reclaimed_large_values",
           std::to_string(temporary_drop.reclaimed_large_value_count)});
      AddDdlPublicationResult(&result,
                              "ddl.drop_object",
                              result.primary_object.object_kind,
                              object_uuid,
                              result.catalog_row_uuid.canonical,
                              "temporary_relation_metadata");
      return result;
    }
  }
  auto result = PersistedRecordResult<EngineDropObjectResult>(request, "ddl.drop_object", kind, true, "dropped", true);
  if (!result.ok) { return result; }
  const std::string object_uuid = request.target_object.uuid.canonical.empty()
                                      ? result.primary_object.uuid.canonical
                                      : request.target_object.uuid.canonical;
  const auto retired = RetireNameRegistryEntriesForObject(request.context, "ddl.drop_object", object_uuid);
  if (retired.error) {
    return MakeCrudDiagnosticResult<EngineDropObjectResult>(request.context, "ddl.drop_object", retired);
  }
  result.evidence.push_back({"name_registry_retired", object_uuid});
  AddDdlPublicationResult(&result,
                          "ddl.drop_object",
                          kind,
                          object_uuid,
                          result.catalog_row_uuid.canonical,
                          kind);
  return result;
}

EngineDropConstraintResult EngineDropConstraint(const EngineDropConstraintRequest& request) {
  constexpr const char* kOperation = "ddl.constraint.drop";
  EngineCatalogDropObjectRequest catalog_request;
  static_cast<EngineApiRequest&>(catalog_request) = request;
  catalog_request.operation_id = kOperation;
  catalog_request.target_object.object_kind = "constraint";
  const auto dropped = EngineCatalogDropObject(catalog_request);

  EngineDropConstraintResult result;
  result.ok = dropped.ok;
  result.operation_id = kOperation;
  result.diagnostics = dropped.diagnostics;
  result.unsupported_features = dropped.unsupported_features;
  result.evidence = dropped.evidence;
  result.result_shape = dropped.result_shape;
  result.primary_object = dropped.primary_object;
  result.catalog_row_uuid = dropped.catalog_row_uuid;
  result.transaction_uuid = dropped.transaction_uuid;
  result.local_transaction_id = dropped.local_transaction_id;
  result.embedded_trust_mode_observed = dropped.embedded_trust_mode_observed;
  result.cluster_authority_required = dropped.cluster_authority_required;
  if (result.ok) {
    result.evidence.push_back({"ddl_catalog_route", "sys.constraint_descriptor"});
    AddDdlPublicationResult(&result,
                            kOperation,
                            "constraint",
                            result.primary_object.uuid.canonical,
                            result.catalog_row_uuid.canonical,
                            "constraint_descriptor");
  }
  return result;
}

}  // namespace scratchbird::engine::internal_api
