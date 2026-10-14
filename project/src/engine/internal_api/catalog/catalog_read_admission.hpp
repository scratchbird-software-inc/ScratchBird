// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "api_diagnostics.hpp"
#include "local_transaction_store.hpp"
#include "identity_publication_fence.hpp"

namespace scratchbird::engine::internal_api {

// An unresolved mutation may retain physically valid catalog records. They
// cannot be consumed as an ordinary owning transaction's usable catalog.
// Callers hold the inventory owner across this gate and complete projection.
inline EngineApiDiagnostic ValidateCatalogReadCommitExclusion(
    const EngineRequestContext& context, std::uint64_t observer,
    const std::string& operation) {
  if (observer != 0) {
    const auto current = storage::database::AcquireLocalTransactionInventorySnapshot(context.database_path);
    if (!current.ok()) return MakeEngineApiDiagnosticFromNative(current.diagnostic,
        "SB_ENGINE_API_INVALID_REQUEST", "engine.api.invalid_request", operation + ":current_inventory_unavailable");
    const auto entry = transaction::mga::LookupLocalTransaction(current.snapshot->inventory,
        transaction::mga::MakeLocalTransactionId(observer));
    if (entry.ok() && entry.entry.rollback_only &&
        (observer != context.local_transaction_id || !context.identity_publication_fence ||
         !context.identity_publication_fence->Allows(context.database_path, context.database_uuid,
             context.local_transaction_id, context.transaction_uuid, current.snapshot->inventory)))
      return MakeInvalidRequestDiagnostic(operation, "transaction_rollback_only");
  }
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

} // namespace scratchbird::engine::internal_api
