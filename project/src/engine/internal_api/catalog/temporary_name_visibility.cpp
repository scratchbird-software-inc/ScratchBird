// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog/temporary_name_visibility.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "api_diagnostics.hpp"
#include "uuid.hpp"

namespace scratchbird::engine::internal_api {
TemporaryNameCandidateClassification ClassifyTemporaryNameCandidate(
    const EngineRequestContext& context, const EngineUuid& object_uuid,
    const MgaTemporaryTableVisibilityResult& visibility) {
  TemporaryNameCandidateClassification result;
  const auto refuse=[&](const char* detail) {
    result.diagnostic=MakeEngineApiDiagnostic(
        "CATALOG.INVALID_INPUT","catalog.name.temporary_visibility_invalid",detail);
    return result;
  };
  if(!visibility.ok || visibility.diagnostic.error) {
    if(!visibility.diagnostic.error || visibility.diagnostic.code.empty())
      return refuse("temporary_table_visibility_source_failed");
    result.diagnostic=visibility.diagnostic;
    return result;
  }
  result.diagnostic=visibility.diagnostic;
  if(!scratchbird::core::uuid::IsEngineIdentityUuid(object_uuid))
    return refuse("temporary_table_object_identity_invalid");
  if(!visibility.table_visible) {
    result.ok=true;
    if(!visibility.known_temporary && !visibility.hidden_by_temporary_visibility)
      result.kind=TemporaryNameCandidateKind::kCatalog;
    return result;
  }
  if(visibility.table.table_uuid!=object_uuid)
    return refuse("temporary_table_object_identity_mismatch");
  const auto& table=visibility.table;
  if(!table.temporary) {
    if(!table.temporary_scope.empty() || !table.temporary_session_uuid.is_nil())
      return refuse("durable_table_namespace_invalid");
    result.ok=true;result.kind=TemporaryNameCandidateKind::kCatalog;return result;
  }
  if(table.temporary_scope=="global" && table.temporary_session_uuid.is_nil()) {
    if(!visibility.visible_to_session)
      return refuse("global_temporary_table_namespace_not_visible");
    result.ok=true;result.kind=TemporaryNameCandidateKind::kCatalog;return result;
  }
  if(table.temporary_scope=="private" &&
      scratchbird::core::uuid::IsEngineIdentityUuid(table.temporary_session_uuid)) {
    if(!scratchbird::core::uuid::IsEngineIdentityUuid(context.session_uuid))
      return refuse("temporary_table_session_namespace_invalid");
    result.ok=true;
    if(table.temporary_session_uuid==context.session_uuid && visibility.visible_to_session)
      result.kind=TemporaryNameCandidateKind::kOwnedPrivate;
    return result;
  }
  return refuse("temporary_table_namespace_invalid");
}
}
