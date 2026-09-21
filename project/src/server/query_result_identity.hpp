// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "session_registry.hpp"
#include "uuid.hpp"

namespace scratchbird::server {

// Adopt an engine-owned result identity cohort without UUID text conversion.
// The caller must obtain these values from the live typed result-handle reader;
// byte validation here cannot establish registry or receipt ownership.
// Validate all four first: refusal must never partially rebind an existing cursor.
inline bool AdoptQueryResultIdentities(
    const core::platform::Uuid& execution,
    const core::platform::Uuid& result_set,
    const core::platform::Uuid& row_descriptor,
    const core::platform::Uuid& snapshot,
    ServerCursorRecord* cursor) {
  if (cursor == nullptr || !core::uuid::IsEngineIdentityUuid(execution) ||
      !core::uuid::IsEngineIdentityUuid(result_set) ||
      !core::uuid::IsEngineIdentityUuid(row_descriptor) ||
      !core::uuid::IsEngineIdentityUuid(snapshot)) return false;
  static_assert(noexcept(cursor->execution_uuid = execution.bytes));
  static_assert(noexcept(cursor->result_set_uuid = result_set.bytes));
  static_assert(noexcept(cursor->row_descriptor_uuid = row_descriptor.bytes));
  static_assert(noexcept(cursor->snapshot_uuid = snapshot.bytes));
  cursor->execution_uuid = execution.bytes;
  cursor->result_set_uuid = result_set.bytes;
  cursor->row_descriptor_uuid = row_descriptor.bytes;
  cursor->snapshot_uuid = snapshot.bytes;
  return true;
}

inline bool AdoptQueryResultIdentities(
    const server_engine_bridge::StatementQueryExecuteResultHandleView& handle,
    ServerCursorRecord* cursor) {
  return AdoptQueryResultIdentities(handle.execution_uuid, handle.result_set_uuid,
                                   handle.row_descriptor_uuid, handle.snapshot_uuid, cursor);
}

inline bool AdoptQueryResultIdentities(
    const server_engine_bridge::StatementCatalogIntrospectResultHandleView& handle,
    ServerCursorRecord* cursor) {
  return AdoptQueryResultIdentities(handle.request_uuid, handle.result_set_uuid,
                                   handle.row_descriptor_uuid, handle.snapshot_uuid, cursor);
}

}  // namespace scratchbird::server
