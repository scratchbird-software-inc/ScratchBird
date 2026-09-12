// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "engine/internal_api/typed_result_producer_cursor.hpp"
#include "server_engine_bridge/statement_context.hpp"

namespace scratchbird::engine::internal_api {

// Real engine-private owners, not a serializable authority projection. Retain
// before ordinary dispatch releases the public receipt. Retirement prevents
// another public lookup/retention; existing owners survive until result close
// or forced session/transaction revocation. Cancellation and budget ownership
// are separate requirements of TypedResultProducerCursorOpenRequestV1.
struct RetainedStatementResultAuthoritiesV1 {
  std::unique_ptr<TypedResultStatementReceiptHandleV1> statement_receipt;
  std::unique_ptr<TypedResultMgaSnapshotPinHandleV1> snapshot;
};

sb_engine_status_t RetainStatementContextForResultV1(
    server_engine_bridge::StatementContextReceiptHandle receipt,
    const wire::TypedResultUuid& expected_session_uuid,
    const wire::TypedResultUuid& expected_receipt_uuid,
    RetainedStatementResultAuthoritiesV1* out) noexcept;

}  // namespace scratchbird::engine::internal_api
