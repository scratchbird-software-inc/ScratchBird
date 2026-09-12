// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "behavior_support/api_behavior_store.hpp"
#include "uuid.hpp"

namespace scratchbird::engine::internal_api {
namespace {

void RefuseBinding(EngineApiResult& result, const char* reason) {
  auto diagnostics = result.diagnostics;
  diagnostics.push_back(MakeEngineApiDiagnostic(
      "CATALOG.INVALID_INPUT", "catalog.ddl.result_identity_invalid", reason));
  // All fallible work precedes the refusal commit. Preserve owner diagnostics.
  result.diagnostics.swap(diagnostics);
  result.ok = false;
}

}  // namespace

void AddDdlPublicationResult(EngineApiResult* result,
                             const std::string& operation_id,
                             const std::string& object_kind,
                             const EngineUuid& object_uuid,
                             const EngineUuid& catalog_row_uuid,
                             const std::string& /*invalidation_scope*/) {
  // SEARCH_KEY: SB_ENGINE_DDL_RESULT_IDENTITY_NOT_PUBLICATION_RECEIPT
  // The old invalidation-scope argument is not an executed invalidation receipt.
  // No rows, evidence flags, generations or newly issued UUIDs are fabricated.
  if (result == nullptr || !result->ok) return;
  const auto valid = [](const EngineUuid& id) {
    return scratchbird::core::uuid::IsEngineIdentityUuid(id);
  };
  if (!valid(object_uuid) ||
      (!catalog_row_uuid.is_nil() && !valid(catalog_row_uuid)) ||
      (!result->catalog_row_uuid.is_nil() && !valid(result->catalog_row_uuid)) ||
      (!result->primary_object.uuid.is_nil() &&
       result->primary_object.uuid != object_uuid)) {
    RefuseBinding(*result, "ddl_result_identity_invalid");
    return;
  }
  if ((!operation_id.empty() && !result->operation_id.empty() &&
       operation_id != result->operation_id) ||
      (!object_kind.empty() && !result->primary_object.object_kind.empty() &&
       object_kind != result->primary_object.object_kind) ||
      (!catalog_row_uuid.is_nil() && !result->catalog_row_uuid.is_nil() &&
       catalog_row_uuid != result->catalog_row_uuid)) {
    RefuseBinding(*result, "ddl_result_identity_mismatch");
    return;
  }

  auto operation = operation_id.empty() ? result->operation_id : operation_id;
  auto kind = object_kind.empty() ? result->primary_object.object_kind : object_kind;
  if (operation.empty() || kind.empty()) {
    RefuseBinding(*result, "ddl_result_identity_labels_missing");
    return;
  }
  const auto row = catalog_row_uuid.is_nil() ? result->catalog_row_uuid : catalog_row_uuid;
  // Swaps and fixed-width copies cannot fail after staging. This is identity
  // binding only; the caller's owning mutation/transaction remains authority.
  result->operation_id.swap(operation);
  result->primary_object.object_kind.swap(kind);
  result->primary_object.uuid = object_uuid;
  result->catalog_row_uuid = row;
}

}  // namespace scratchbird::engine::internal_api
