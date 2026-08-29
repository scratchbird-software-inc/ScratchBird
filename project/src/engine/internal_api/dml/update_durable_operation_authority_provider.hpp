// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "mga_relation_store/mga_relation_store.hpp"

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_DML_UPDATE_DURABLE_OPERATION_AUTHORITY_PROVIDER_V1
// Typed private facade over MGA-owned immutable carrier and DUJR durability.
// Carrier semantics are validated by the UPDATE consumer before this API is
// called.  This provider authenticates the current receipt/transaction and
// preserves the supplied bytes without encoding, decoding, or inference.

inline constexpr const char* kDmlUpdateDurableOperationDiagnosticInvalid =
    "SBLR.OPERAND_INVALID";
inline constexpr const char* kDmlUpdateDurableOperationDiagnosticDenied =
    "SECURITY.ACCESS_DENIED";
inline constexpr const char* kDmlUpdateDurableOperationDiagnosticStale =
    "MGA.TRANSACTION.STALE";
inline constexpr const char* kDmlUpdateDurableOperationDiagnosticConflict =
    "DML.UPDATE_FAILED";
inline constexpr const char* kDmlUpdateDurableOperationDiagnosticStorage =
    "DML.UPDATE_FAILED";

struct EngineDmlUpdateDurablePublishBoundRequestV1 {
  EngineRequestContext context;
  MgaDmlUpdateDurablePublishBoundRequestV1 publication;
};

struct EngineDmlUpdateDurableAuthorityReservationRequestV1 {
  EngineRequestContext context;
  MgaDmlUpdateDurableAuthorityReservationRequestV1 reservation;
};

struct EngineDmlUpdateDurableAuthorityAbandonRequestV1 {
  EngineRequestContext context;
  MgaDmlUpdateDurableOperationIdentityV1 identity;
};

struct EngineDmlUpdateDurableAppendSuccessorRequestV1 {
  EngineRequestContext context;
  MgaDmlUpdateDurableAppendSuccessorRequestV1 append;
};

struct EngineDmlUpdateDurableRecoverChainRequestV1 {
  EngineRequestContext context;
  MgaDmlUpdateDurableOperationLookupV1 lookup;
};

MgaDmlUpdateDurableOperationMutationResultV1
PublishDmlUpdateDurableOperationBoundV1(
    const EngineDmlUpdateDurablePublishBoundRequestV1& request);

MgaDmlUpdateDurableAuthorityReservationResultV1
ReserveDmlUpdateDurableOperationAuthorityV1(
    const EngineDmlUpdateDurableAuthorityReservationRequestV1& request);

MgaDmlUpdateDurableOperationMutationResultV1
AbandonDmlUpdateDurableOperationAuthorityReservationV1(
    const EngineDmlUpdateDurableAuthorityAbandonRequestV1& request);

MgaDmlUpdateDurableOperationMutationResultV1
AppendDmlUpdateDurableOperationSuccessorV1(
    const EngineDmlUpdateDurableAppendSuccessorRequestV1& request);

MgaDmlUpdateDurableOperationPrepareResultV1
PrepareDmlUpdateDurableOperationSuccessorV1(
    const EngineDmlUpdateDurableAppendSuccessorRequestV1& request);

MgaDmlUpdateDurableOperationMutationResultV1
CommitPreparedDmlUpdateDurableOperationSuccessorV1(
    MgaDmlUpdateDurablePreparedSuccessorV1&& prepared);

MgaDmlUpdateDurableOperationMutationResultV1
CancelPreparedDmlUpdateDurableOperationSuccessorV1(
    MgaDmlUpdateDurablePreparedSuccessorV1&& prepared);

MgaDmlUpdateDurableOperationRecoveryResultV1
RecoverDmlUpdateDurableOperationChainV1(
    const EngineDmlUpdateDurableRecoverChainRequestV1& request);

MgaDmlUpdateDurableOperationMutationResultV1
RollbackDmlUpdateStatementFromValidatedDurableAuthorityV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateValidatedDurableAuthorityHandleV1& validated_handle);

MgaDmlUpdateDurableOperationMutationResultV1
CommitRecoveredDmlUpdateDurableOperationStagedSuccessorV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateValidatedDurableAuthorityHandleV1& validated_handle);

}  // namespace scratchbird::engine::internal_api
