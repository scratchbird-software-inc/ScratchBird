// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/update_durable_operation_authority_provider.hpp"

#include "api_diagnostics.hpp"
#include "local_transaction_store.hpp"
#include "transaction_inventory.hpp"
#include "transaction_state.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <string_view>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

using scratchbird::storage::database::LoadLocalTransactionInventoryFromDatabase;
using scratchbird::transaction::mga::LookupLocalTransaction;
using scratchbird::transaction::mga::MakeLocalTransactionId;
using scratchbird::transaction::mga::TransactionState;

EngineApiDiagnostic Diagnostic(std::string code, std::string key,
                               std::string detail = {}) {
  return MakeEngineApiDiagnostic(std::move(code), std::move(key),
                                 std::move(detail), true);
}

bool HasTraceTag(const EngineRequestContext& context, std::string_view tag) {
  return std::find(context.trace_tags.begin(), context.trace_tags.end(), tag) !=
         context.trace_tags.end();
}

bool ExactUuid(std::string_view text) {
  if (text.empty()) return false;
  const auto parsed = scratchbird::core::uuid::ParseUuid(std::string(text));
  return parsed.ok() && !scratchbird::core::uuid::IsNilUuid(parsed.value) &&
         scratchbird::core::uuid::UuidToString(parsed.value) == text;
}

enum class DurableProviderPhaseV1 : std::uint8_t {
  binder = 1,
  consumer = 2,
  recovery = 3,
  consumer_or_recovery = 4,
};

EngineApiDiagnostic ValidateContext(const EngineRequestContext& context,
                                    DurableProviderPhaseV1 phase) {
  const bool binder =
      HasTraceTag(context, "private_dml_update_rows_binder");
  const bool consumer =
      HasTraceTag(context, "private_dml_update_rows_consumer");
  const bool recovery =
      HasTraceTag(context, "private_dml_update_rows_recovery");
  if (!context.security_context_present ||
      !context.statement_metadata_snapshot_engine_owned ||
      (phase == DurableProviderPhaseV1::binder
           ? (!binder || consumer || recovery)
           : phase == DurableProviderPhaseV1::consumer
                 ? (!consumer || binder || recovery)
                 : phase == DurableProviderPhaseV1::recovery
                       ? (!recovery || binder || consumer)
                       : (binder || (consumer == recovery)))) {
    return Diagnostic(kDmlUpdateDurableOperationDiagnosticDenied,
                      "sblr.dml_update_rows.durable_operation_denied");
  }
  if (context.read_only_mode || context.cluster_transaction_active ||
      context.route_fence_present || context.database_path.empty() ||
      context.local_transaction_id == 0 ||
      !ExactUuid(context.database_uuid.canonical) ||
      !ExactUuid(context.transaction_uuid.canonical) ||
      !ExactUuid(context.statement_receipt_uuid.canonical)) {
    return Diagnostic(kDmlUpdateDurableOperationDiagnosticInvalid,
                      "sblr.dml_update_rows.durable_operation_invalid",
                      "authenticated_context_invalid");
  }
  const auto parsed_transaction = scratchbird::core::uuid::ParseTypedUuid(
      scratchbird::core::platform::UuidKind::transaction,
      context.transaction_uuid.canonical);
  if (!parsed_transaction.ok()) {
    return Diagnostic(kDmlUpdateDurableOperationDiagnosticInvalid,
                      "sblr.dml_update_rows.durable_operation_invalid",
                      "transaction_uuid_invalid");
  }
  const auto loaded =
      LoadLocalTransactionInventoryFromDatabase(context.database_path);
  if (!loaded.ok()) {
    return Diagnostic(
        kDmlUpdateDurableOperationDiagnosticStorage,
        "sblr.dml_update_rows.durable_operation_inventory_load_failed",
        loaded.diagnostic.remediation_hint);
  }
  const auto transaction = LookupLocalTransaction(
      loaded.inventory, MakeLocalTransactionId(context.local_transaction_id));
  if (!transaction.ok() ||
      transaction.entry.identity.transaction_uuid.value !=
          parsed_transaction.value.value ||
      (!recovery && transaction.entry.state != TransactionState::active)) {
    return Diagnostic(kDmlUpdateDurableOperationDiagnosticStale,
                      "sblr.dml_update_rows.durable_operation_stale",
                      "active_transaction_inventory_identity_mismatch");
  }
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {},
                                 false);
}

MgaDmlUpdateDurableOperationMutationResultV1 MutationFailure(
    MgaDmlUpdateDurableOperationOutcomeV1 outcome,
    EngineApiDiagnostic diagnostic) {
  MgaDmlUpdateDurableOperationMutationResultV1 result;
  result.outcome = outcome;
  result.diagnostic = std::move(diagnostic);
  return result;
}

MgaDmlUpdateDurableAuthorityReservationResultV1 ReservationFailure(
    MgaDmlUpdateDurableOperationOutcomeV1 outcome,
    EngineApiDiagnostic diagnostic) {
  MgaDmlUpdateDurableAuthorityReservationResultV1 result;
  result.outcome = outcome;
  result.diagnostic = std::move(diagnostic);
  return result;
}

MgaDmlUpdateDurableOperationPrepareResultV1 PrepareFailure(
    MgaDmlUpdateDurableOperationOutcomeV1 outcome,
    EngineApiDiagnostic diagnostic) {
  MgaDmlUpdateDurableOperationPrepareResultV1 result;
  result.outcome = outcome;
  result.diagnostic = std::move(diagnostic);
  return result;
}

MgaDmlUpdateDurableOperationRecoveryResultV1 RecoveryFailure(
    MgaDmlUpdateDurableOperationOutcomeV1 outcome,
    EngineApiDiagnostic diagnostic) {
  MgaDmlUpdateDurableOperationRecoveryResultV1 result;
  result.outcome = outcome;
  result.diagnostic = std::move(diagnostic);
  return result;
}

MgaDmlUpdateDurableOperationOutcomeV1 ValidationOutcome(
    const EngineApiDiagnostic& diagnostic) {
  if (diagnostic.code == kDmlUpdateDurableOperationDiagnosticDenied) {
    return MgaDmlUpdateDurableOperationOutcomeV1::access_denied;
  }
  if (diagnostic.code == kDmlUpdateDurableOperationDiagnosticStale) {
    return MgaDmlUpdateDurableOperationOutcomeV1::stale;
  }
  if (diagnostic.message_key.find("inventory_load_failed") !=
      std::string::npos) {
    return MgaDmlUpdateDurableOperationOutcomeV1::storage_failure;
  }
  return MgaDmlUpdateDurableOperationOutcomeV1::conflict;
}

}  // namespace

MgaDmlUpdateDurableAuthorityReservationResultV1
ReserveDmlUpdateDurableOperationAuthorityV1(
    const EngineDmlUpdateDurableAuthorityReservationRequestV1& request) {
  const auto validated =
      ValidateContext(request.context, DurableProviderPhaseV1::binder);
  if (validated.error) {
    return ReservationFailure(ValidationOutcome(validated), validated);
  }
  return ReserveMgaDmlUpdateDurableOperationAuthorityV1(
      request.context, request.reservation);
}

MgaDmlUpdateDurableOperationMutationResultV1
AbandonDmlUpdateDurableOperationAuthorityReservationV1(
    const EngineDmlUpdateDurableAuthorityAbandonRequestV1& request) {
  const auto validated =
      ValidateContext(request.context, DurableProviderPhaseV1::binder);
  if (validated.error) {
    return MutationFailure(ValidationOutcome(validated), validated);
  }
  return AbandonMgaDmlUpdateDurableOperationAuthorityReservationV1(
      request.context, request.identity);
}

MgaDmlUpdateDurableOperationMutationResultV1
PublishDmlUpdateDurableOperationBoundV1(
    const EngineDmlUpdateDurablePublishBoundRequestV1& request) {
  const auto validated =
      ValidateContext(request.context, DurableProviderPhaseV1::binder);
  if (validated.error) {
    return MutationFailure(ValidationOutcome(validated), validated);
  }
  return PublishMgaDmlUpdateDurableOperationBoundV1(
      request.context, request.publication);
}

MgaDmlUpdateDurableOperationMutationResultV1
AppendDmlUpdateDurableOperationSuccessorV1(
    const EngineDmlUpdateDurableAppendSuccessorRequestV1& request) {
  const auto validated =
      ValidateContext(request.context,
                      DurableProviderPhaseV1::consumer_or_recovery);
  if (validated.error) {
    return MutationFailure(ValidationOutcome(validated), validated);
  }
  return AppendMgaDmlUpdateDurableOperationSuccessorV1(
      request.context, request.append);
}

MgaDmlUpdateDurableOperationPrepareResultV1
PrepareDmlUpdateDurableOperationSuccessorV1(
    const EngineDmlUpdateDurableAppendSuccessorRequestV1& request) {
  const auto validated =
      ValidateContext(request.context, DurableProviderPhaseV1::consumer);
  if (validated.error) {
    return PrepareFailure(ValidationOutcome(validated), validated);
  }
  return PrepareMgaDmlUpdateDurableOperationSuccessorV1(
      request.context, request.append);
}

MgaDmlUpdateDurableOperationMutationResultV1
CommitPreparedDmlUpdateDurableOperationSuccessorV1(
    MgaDmlUpdateDurablePreparedSuccessorV1&& prepared) {
  return CommitPreparedMgaDmlUpdateDurableOperationSuccessorV1(
      std::move(prepared));
}

MgaDmlUpdateDurableOperationMutationResultV1
CancelPreparedDmlUpdateDurableOperationSuccessorV1(
    MgaDmlUpdateDurablePreparedSuccessorV1&& prepared) {
  return CancelPreparedMgaDmlUpdateDurableOperationSuccessorV1(
      std::move(prepared));
}

MgaDmlUpdateDurableOperationRecoveryResultV1
RecoverDmlUpdateDurableOperationChainV1(
    const EngineDmlUpdateDurableRecoverChainRequestV1& request) {
  const auto validated =
      ValidateContext(request.context, DurableProviderPhaseV1::recovery);
  if (validated.error) {
    return RecoveryFailure(ValidationOutcome(validated), validated);
  }
  return RecoverMgaDmlUpdateDurableOperationChainV1(
      request.context, request.lookup);
}

MgaDmlUpdateDurableOperationMutationResultV1
RollbackDmlUpdateStatementFromValidatedDurableAuthorityV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateValidatedDurableAuthorityHandleV1& validated_handle) {
  const auto validated =
      ValidateContext(context, DurableProviderPhaseV1::recovery);
  if (validated.error) {
    return MutationFailure(ValidationOutcome(validated), validated);
  }
  return RollbackMgaDmlUpdateStatementFromValidatedDurableAuthorityV1(
      context, validated_handle);
}

MgaDmlUpdateDurableOperationMutationResultV1
CommitRecoveredDmlUpdateDurableOperationStagedSuccessorV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateValidatedDurableAuthorityHandleV1& validated_handle) {
  const auto validated =
      ValidateContext(context, DurableProviderPhaseV1::recovery);
  if (validated.error) {
    return MutationFailure(ValidationOutcome(validated), validated);
  }
  return CommitRecoveredMgaDmlUpdateDurableOperationStagedSuccessorV1(
      context, validated_handle);
}

}  // namespace scratchbird::engine::internal_api
