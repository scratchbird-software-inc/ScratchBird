// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "dml/delete_recovery_authority.hpp"
#include "api_diagnostics.hpp"
#include "local_transaction_store.hpp"
#include "mga_relation_store/mga_savepoint_store.hpp"
#include "mga_relation_store/mga_savepoint_marker_codec.hpp"
#include "transaction_inventory.hpp"
#include "uuid.hpp"
#include <algorithm>

namespace scratchbird::engine::internal_api {
namespace {
namespace w = scratchbird::wire;
namespace mga = scratchbird::transaction::mga;
bool Matches(const std::string& text, const w::TypedUpdateUuid& binary) {
  const auto parsed = scratchbird::core::uuid::ParseUuid(text);
  return parsed.ok() && !parsed.value.is_nil() &&
      std::equal(binary.begin(), binary.end(), parsed.value.bytes.begin());
}
std::string Text(const w::TypedUpdateUuid& binary) {
  scratchbird::core::platform::Uuid uuid;
  std::copy(binary.begin(), binary.end(), uuid.bytes.begin());
  return scratchbird::core::uuid::UuidToString(uuid);
}
}

EngineDmlDeleteRecoveryObservationV1 ObserveDmlDeleteRecoveryAuthorityV1(
    const EngineRequestContext& context,
    std::span<const std::vector<std::uint8_t>> chain) {
  EngineDmlDeleteRecoveryObservationV1 out;
  const auto refuse = [&](const char* detail, const char* code = "DML.DELETE_FAILED") {
    out.ok = false;
    out.disposition = EngineDmlDeleteRecoveryDispositionV1::refused;
    out.diagnostic = MakeEngineApiDiagnostic(code, "sblr.dml_delete_rows.recovery_authority", detail, true);
    return out;
  };
  if (context.database_path.empty() || !context.local_transaction_id || chain.empty() || chain.size() > 4)
    return refuse("complete_bounded_chain_required");
  if (context.cluster_transaction_active || context.route_fence_present)
    return refuse("local_recovery_only", "SBLR.OPERATION_UNSUPPORTED");
  w::TypedDeleteJournalRecord head;
  bool first = true;
  for (const auto& bytes : chain) {
    w::TypedDeleteJournalRecord next;
    w::TypedDeleteCarrierError error;
    if (!w::DecodeAndValidateTypedDeleteJournal(bytes, first ? nullptr : &head, &next, &error))
      return refuse("durable_chain_invalid");
    head = std::move(next);
    first = false;
  }
  const auto& d = head.descriptor;
  if (!Matches(context.database_uuid, head.database_uuid) ||
      !Matches(context.transaction_uuid, head.owning_transaction_uuid) ||
      context.local_transaction_id != head.owning_local_transaction_id ||
      !Matches(context.statement_receipt_uuid, head.authenticated_statement_receipt_uuid) ||
      !Matches(context.statement_snapshot_uuid, d.statement_snapshot_uuid) ||
      !Matches(context.statement_metadata_snapshot_uuid, d.catalog_snapshot_uuid) ||
      context.catalog_generation_id != d.catalog_generation ||
      context.datatype_registry_generation != d.datatype_registry_generation)
    return refuse("durable_owner_context_mismatch", "MGA.TRANSACTION.STALE");
  // Load authoritative inventory, not a caller projection of transaction state.
  const auto inventory = scratchbird::storage::database::AcquireStrongLocalTransactionInventorySnapshot(context.database_path);
  if (!inventory.ok()) return refuse("inventory_unavailable");
  const auto marker_generation = CurrentMgaSavepointAuthorityGeneration(context);
  const auto transaction = mga::LookupLocalTransaction(inventory.snapshot->inventory,
      mga::MakeLocalTransactionId(context.local_transaction_id));
  if (!transaction.ok() || !Matches(
          scratchbird::core::uuid::UuidToString(transaction.entry.identity.transaction_uuid.value),
          head.owning_transaction_uuid)) return refuse("inventory_owner_mismatch", "MGA.TRANSACTION.STALE");
  using D = EngineDmlDeleteRecoveryDispositionV1;
  using S = w::TypedUpdateJournalState;
  auto disposition = D::refused;
  if (transaction.entry.state == mga::TransactionState::rolled_back) {
    disposition = D::transaction_rolled_back;
  } else if (transaction.entry.state != mga::TransactionState::active &&
             transaction.entry.state != mga::TransactionState::committed) {
    return refuse("inventory_requires_transaction_recovery", "MGA.TRANSACTION.STALE");
  } else if (head.lifecycle_state == S::bound) {
    disposition = D::abandon_unexecuted;
  } else if (head.lifecycle_state == S::aborted) {
    if (head.statement_savepoint_generation) {
      const auto marker = ObserveMgaSavepointMarker(context,
          MgaSavepointUuidKey(Text(head.statement_savepoint_uuid)), head.statement_savepoint_generation);
      if (!marker.ok || (!marker.rolled_back && marker.lifecycle != MgaSavepointMarkerLifecycle::invalidated))
        return refuse("aborted_chain_without_MGA_rewind");
    }
    disposition = D::aborted;
  } else {
    const auto marker = ObserveMgaSavepointMarker(context,
        MgaSavepointUuidKey(Text(head.statement_savepoint_uuid)), head.statement_savepoint_generation);
    if (!marker.ok || marker.lifecycle == MgaSavepointMarkerLifecycle::missing)
      return refuse("exact_savepoint_history_unavailable");
    const bool released = marker.lifecycle == MgaSavepointMarkerLifecycle::released && !marker.rolled_back;
    const bool rewound = marker.rolled_back || marker.lifecycle == MgaSavepointMarkerLifecycle::invalidated;
    if (head.lifecycle_state == S::published) {
      if (!released) return refuse("published_chain_without_exact_MGA_release");
      disposition = D::published;
    } else if (head.lifecycle_state == S::prepared && released) {
      disposition = D::publish_prepared_result;
    } else if (released) {
      return refuse("released_statement_without_prepared_result");
    } else if (rewound) {
      disposition = D::statement_already_rewound;
    } else {
      disposition = D::rollback_statement;
    }
    if (transaction.entry.state == mga::TransactionState::committed &&
        disposition != D::published && disposition != D::publish_prepared_result)
      return refuse("committed_inventory_has_unpublished_statement");
  }
  if (marker_generation != CurrentMgaSavepointAuthorityGeneration(context) ||
      !scratchbird::storage::database::RevalidateLocalTransactionInventorySnapshot(*inventory.snapshot).ok())
    return refuse("MGA_authority_changed_during_observation", "MGA.TRANSACTION.STALE");
  out.ok = true;
  out.disposition = disposition;
  out.head = std::move(head);
  out.diagnostic = MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  return out;
}
}  // namespace scratchbird::engine::internal_api
