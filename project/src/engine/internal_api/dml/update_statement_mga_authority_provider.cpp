// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/update_statement_mga_authority_provider.hpp"

#include "api_diagnostics.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <string_view>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

EngineApiDiagnostic Diagnostic(std::string code, std::string key,
                               std::string detail = {}) {
  return MakeEngineApiDiagnostic(std::move(code), std::move(key),
                                 std::move(detail), true);
}

EngineApiDiagnostic Ok() {
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {},
                                 false);
}

bool HasTraceTag(const EngineRequestContext& context, std::string_view tag) {
  return std::find(context.trace_tags.begin(), context.trace_tags.end(), tag) !=
         context.trace_tags.end();
}

bool ExactUuid(std::string_view text) {
  if (text.empty()) return false;
  const auto parsed = scratchbird::core::uuid::ParseUuid(std::string(text));
  return parsed.ok() &&
         !scratchbird::core::uuid::IsNilUuid(parsed.value) &&
         scratchbird::core::uuid::UuidToString(parsed.value) == text;
}

EngineApiDiagnostic ValidateOpenRequest(
    const EngineDmlUpdateStatementMgaAuthorityOpenRequestV1& request,
    bool recovery = false) {
  const auto& context = request.context;
  const bool binder =
      HasTraceTag(context, "private_dml_update_rows_binder");
  const bool consumer =
      HasTraceTag(context, "private_dml_update_rows_consumer");
  const bool recovery_tag =
      HasTraceTag(context, "private_dml_update_rows_recovery");
  if (!context.security_context_present ||
      !context.statement_metadata_snapshot_engine_owned ||
      !context.authorization_context.present ||
      context.authorization_context.authority_uuid.canonical.empty() ||
      context.authorization_context.security_context_generation == 0 ||
      (recovery ? (!recovery_tag || binder || consumer)
                : (!consumer || binder || recovery_tag))) {
    return Diagnostic(kDmlUpdateStatementMgaDiagnosticAccessDenied,
                      "sblr.dml_update_rows.statement_mga_authority_denied");
  }
  if (context.read_only_mode || context.cluster_transaction_active ||
      context.route_fence_present || context.database_path.empty() ||
      context.local_transaction_id == 0 ||
      !ExactUuid(context.database_uuid.canonical) ||
      !ExactUuid(context.transaction_uuid.canonical) ||
      !ExactUuid(context.statement_receipt_uuid.canonical) ||
      !ExactUuid(request.authenticated_statement_receipt_uuid) ||
      request.authenticated_statement_receipt_uuid !=
          context.statement_receipt_uuid.canonical ||
      !ExactUuid(request.operation_uuid) ||
      !ExactUuid(request.descriptor_uuid) ||
      request.descriptor_generation == 0 ||
      !ExactUuid(request.recovery_token_uuid) ||
      request.recovery_generation == 0 ||
      !ExactUuid(request.reserved_publication_barrier_uuid) ||
      request.reserved_publication_barrier_generation == 0 ||
      request.operation_uuid == request.descriptor_uuid ||
      request.operation_uuid == request.recovery_token_uuid ||
      request.operation_uuid == request.reserved_publication_barrier_uuid ||
      request.descriptor_uuid == request.recovery_token_uuid ||
      request.descriptor_uuid == request.reserved_publication_barrier_uuid ||
      request.recovery_token_uuid ==
          request.reserved_publication_barrier_uuid) {
    return Diagnostic(kDmlUpdateStatementMgaDiagnosticInvalid,
                      "sblr.dml_update_rows.statement_mga_authority_invalid");
  }
  return Ok();
}

MgaDmlUpdateStatementSavepointBindingV1 Binding(
    const EngineDmlUpdateStatementMgaAuthorityOpenRequestV1& request) {
  MgaDmlUpdateStatementSavepointBindingV1 binding;
  binding.database_uuid = request.context.database_uuid.canonical;
  binding.owning_transaction_uuid =
      request.context.transaction_uuid.canonical;
  binding.owning_local_transaction_id = request.context.local_transaction_id;
  binding.authenticated_statement_receipt_uuid =
      request.authenticated_statement_receipt_uuid;
  binding.operation_uuid = request.operation_uuid;
  binding.descriptor_uuid = request.descriptor_uuid;
  binding.descriptor_generation = request.descriptor_generation;
  binding.recovery_token_uuid = request.recovery_token_uuid;
  binding.recovery_generation = request.recovery_generation;
  return binding;
}

EngineDmlUpdateStatementMgaAuthorityResultV1 Convert(
    MgaDmlUpdateStatementSavepointAuthorityResultV1 source) {
  EngineDmlUpdateStatementMgaAuthorityResultV1 result;
  result.ok = source.ok;
  result.diagnostic = std::move(source.diagnostic);
  result.authority = std::move(source.authority);
  return result;
}

EngineDmlUpdateStatementMgaAuthorityResultV1 Refused(
    EngineApiDiagnostic diagnostic) {
  EngineDmlUpdateStatementMgaAuthorityResultV1 result;
  result.diagnostic = std::move(diagnostic);
  return result;
}

EngineDmlUpdateStatementMgaAuthorityResultV1 StaleFromValidation(
    const EngineApiDiagnostic& diagnostic) {
  if (!diagnostic.error) {
    EngineDmlUpdateStatementMgaAuthorityResultV1 result;
    result.ok = true;
    result.diagnostic = diagnostic;
    return result;
  }
  if (diagnostic.code == kDmlUpdateStatementMgaDiagnosticAccessDenied) {
    return Refused(diagnostic);
  }
  return Refused(Diagnostic(
      kDmlUpdateStatementMgaDiagnosticStale,
      "sblr.dml_update_rows.statement_mga_authority_stale",
      "current_authority_context_mismatch"));
}

bool CurrentBindingMatchesAdmitted(
    const EngineDmlUpdateStatementMgaAuthorityTransitionRequestV1& request) {
  return Binding(request.current) == request.admitted.binding &&
         request.current.reserved_publication_barrier_uuid ==
             request.admitted.publication_barrier_uuid &&
         request.current.reserved_publication_barrier_generation ==
             request.admitted.publication_barrier_generation;
}

}  // namespace

EngineDmlUpdateStatementMgaAuthorityResultV1
OpenDmlUpdateStatementMgaAuthorityV1(
    const EngineDmlUpdateStatementMgaAuthorityOpenRequestV1& request) {
  const auto validated = ValidateOpenRequest(request);
  if (validated.error) return Refused(validated);
  return Convert(
      CreateMgaDmlUpdateStatementSavepointAuthorityWithReservedBarrierV1(
          request.context, Binding(request),
          request.reserved_publication_barrier_uuid,
          request.reserved_publication_barrier_generation));
}

EngineDmlUpdateStatementMgaAuthorityResultV1
RecoverDmlUpdateStatementMgaAuthorityV1(
    const EngineDmlUpdateStatementMgaAuthorityRecoverRequestV1& request) {
  const auto validated = ValidateOpenRequest(request.current, true);
  if (validated.error) return StaleFromValidation(validated);
  return Convert(RecoverMgaDmlUpdateStatementSavepointAuthorityV1(
      request.current.context, Binding(request.current), request.savepoint_uuid,
      request.savepoint_generation));
}

EngineDmlUpdateStatementMgaAuthorityResultV1
RevalidateDmlUpdateStatementMgaAuthorityV1(
    const EngineDmlUpdateStatementMgaAuthorityTransitionRequestV1& request) {
  const auto validated = ValidateOpenRequest(request.current);
  if (validated.error) return StaleFromValidation(validated);
  if (!CurrentBindingMatchesAdmitted(request)) {
    return Refused(Diagnostic(
        kDmlUpdateStatementMgaDiagnosticStale,
        "sblr.dml_update_rows.statement_mga_authority_stale",
        "cross_authority_transition_refused"));
  }
  return Convert(RevalidateMgaDmlUpdateStatementSavepointAuthorityV1(
      request.current.context, request.admitted));
}

EngineDmlUpdateStatementMgaAuthorityResultV1
RollbackDmlUpdateStatementMgaAuthorityV1(
    const EngineDmlUpdateStatementMgaAuthorityTransitionRequestV1& request) {
  const auto validated = ValidateOpenRequest(request.current);
  if (validated.error) return StaleFromValidation(validated);
  if (!CurrentBindingMatchesAdmitted(request)) {
    return Refused(Diagnostic(
        kDmlUpdateStatementMgaDiagnosticStale,
        "sblr.dml_update_rows.statement_mga_authority_stale",
        "cross_authority_transition_refused"));
  }
  return Convert(RollbackMgaDmlUpdateStatementSavepointAuthorityV1(
      request.current.context, request.admitted));
}

EngineDmlUpdateStatementMgaAuthorityResultV1
ReleaseDmlUpdateStatementMgaAuthorityV1(
    const EngineDmlUpdateStatementMgaAuthorityTransitionRequestV1& request) {
  const auto validated = ValidateOpenRequest(request.current);
  if (validated.error) return StaleFromValidation(validated);
  if (!CurrentBindingMatchesAdmitted(request)) {
    return Refused(Diagnostic(
        kDmlUpdateStatementMgaDiagnosticStale,
        "sblr.dml_update_rows.statement_mga_authority_stale",
        "cross_authority_transition_refused"));
  }
  return Convert(ReleaseMgaDmlUpdateStatementSavepointAuthorityV1(
      request.current.context, request.admitted));
}

}  // namespace scratchbird::engine::internal_api
