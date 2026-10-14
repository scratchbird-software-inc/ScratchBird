// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "api_diagnostics.hpp"
#include "local_transaction_store.hpp"
#include "identity_publication_fence.hpp"
#include "uuid.hpp"

namespace scratchbird::engine::internal_api {

// Current write admission, not statement visibility. The caller retains its
// operation owner across admission and effects; a retained read snapshot must
// never conceal a later rollback-only fence or transaction finality.
inline EngineApiDiagnostic ValidateCurrentMutatingTransactionAuthority(
    const EngineRequestContext& context, const std::string& operation_id) {
  if (context.database_path.empty())
    return MakeInvalidRequestDiagnostic(operation_id, "database_path_required");
  if (context.local_transaction_id == 0)
    return MakeInvalidRequestDiagnostic(operation_id, "local_transaction_id_required");
  if (!core::uuid::IsEngineIdentityUuid(context.database_uuid) ||
      !core::uuid::IsEngineIdentityUuid(context.transaction_uuid))
    return MakeInvalidRequestDiagnostic(operation_id, "binary_transaction_owner_required");
  // Acquire the current native snapshot directly. The copy-returning loader
  // uses the same admission but duplicates the entire transaction history.
  // This is not the potentially stale snapshot retained in request.context.
  const auto loaded = storage::database::AcquireLocalTransactionInventorySnapshot(context.database_path);
  if (!loaded.ok())
    return MakeEngineApiDiagnosticFromNative(loaded.diagnostic,
        loaded.diagnostic.diagnostic_code.empty() ? "SB-MGA-TXN-INV-LOAD-FAILED" : loaded.diagnostic.diagnostic_code,
        loaded.diagnostic.message_key.empty() ? "mga.transaction_inventory.load_failed" : loaded.diagnostic.message_key,
        loaded.diagnostic.remediation_hint);
  if (!loaded.snapshot || !loaded.snapshot->inventory.publication_base ||
      loaded.snapshot->inventory.publication_base->database_uuid != context.database_uuid)
    return MakeInvalidRequestDiagnostic(operation_id, "database_identity_mismatch");
  const auto owner = transaction::mga::LookupLocalTransaction(loaded.snapshot->inventory,
      transaction::mga::MakeLocalTransactionId(context.local_transaction_id));
  if (!owner.ok())
    return MakeEngineApiDiagnosticFromNative(owner.diagnostic,
        owner.diagnostic.diagnostic_code.empty() ? "SB-MGA-TXN-INV-LOOKUP-FAILED" : owner.diagnostic.diagnostic_code,
        owner.diagnostic.message_key.empty() ? "mga.transaction_inventory.lookup_failed" : owner.diagnostic.message_key,
        owner.diagnostic.remediation_hint);
  if (!owner.entry.identity.valid() || owner.entry.identity.transaction_uuid.value != context.transaction_uuid)
    return MakeInvalidRequestDiagnostic(operation_id, "transaction_identity_mismatch");
  if (owner.entry.state != transaction::mga::TransactionState::active)
    return MakeInvalidRequestDiagnostic(operation_id, "active_local_transaction_required");
  if (owner.entry.rollback_only &&
      (!context.identity_publication_fence ||
       !context.identity_publication_fence->Allows(context.database_path, context.database_uuid,
           context.local_transaction_id, context.transaction_uuid, loaded.snapshot->inventory)))
    return MakeInvalidRequestDiagnostic(operation_id, "transaction_rollback_only");
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

} // namespace scratchbird::engine::internal_api
