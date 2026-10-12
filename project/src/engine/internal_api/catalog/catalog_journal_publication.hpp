// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "behavior_support/api_behavior_store.hpp"
#include "catalog/catalog_object_lifecycle_codec.hpp"
#include "catalog/name_registry_codec.hpp"
#include "disk_device.hpp"
#include "local_transaction_store.hpp"
#include "mga_relation_store/mga_event_sequence_allocator.hpp"
#include "transaction/transaction_api.hpp"

#include <filesystem>
#include <fstream>

namespace scratchbird::engine::internal_api {

enum class CatalogJournalKind { lifecycle, resolver_names, relation_metadata };

namespace catalog_publication_detail {
struct UnpublishedReservation {
  const MgaEventSequenceRangeReservation& reservation;
  bool attempted = false;
  ~UnpublishedReservation() {
    // Abandonment must remain allocation-free, including during bad_alloc
    // unwinding. Only an unattempted tail may be returned to the allocator.
    if (!attempted) AbandonDeferredEventSequenceReservation(reservation);
  }
};
} // namespace catalog_publication_detail

// Single-record publication only. The enclosing mutation retains authorization,
// whole-operation ownership and failed-prefix recovery responsibility. This
// primitive is not admission for a catalog producer under an active savepoint.
inline EngineApiDiagnostic AppendSequencedCatalogJournalRecord(
    const EngineRequestContext& context, ApiBehaviorRecord record,
    CatalogJournalKind kind) {
  const auto refused = [&](const std::string& detail) {
    return MakeInvalidRequestDiagnostic(record.operation_id, detail);
  };
  if (context.database_path.empty() || context.local_transaction_id == 0 ||
      !core::uuid::IsEngineIdentityUuid(context.database_uuid) ||
      !core::uuid::IsEngineIdentityUuid(context.transaction_uuid) ||
      record.creator_tx != context.local_transaction_id ||
      record.event_sequence != 0) return refused("catalog_publication_context_invalid");
  const char* suffix = nullptr;
  switch (kind) {
    case CatalogJournalKind::lifecycle: suffix = ".sb.catalog_object_events"; break;
    case CatalogJournalKind::resolver_names: suffix = ".sb.name_events.v2"; break;
    default: return refused("catalog_publication_kind_invalid");
  }
  if (kind == CatalogJournalKind::lifecycle) {
    CatalogLifecycleRecord decoded;
    if (!DecodeCatalogLifecycleFrame(record, &decoded)) return refused("catalog_publication_record_invalid");
  } else if (record.object_kind == "name_registry.retire") {
    if (!core::uuid::IsEngineIdentityUuid(record.object_uuid) ||
        record.target_object_uuid != record.object_uuid || !record.payload.empty() ||
        record.state != "retired" || !record.target_schema_uuid.is_nil() ||
        !record.target_database_uuid.is_nil() || !record.default_name.empty() || record.deleted)
      return refused("catalog_publication_record_invalid");
  } else {
    NameRegistryEntry decoded;
    if (record.object_kind != "name_registry.entry" || record.state != "active" ||
        !record.target_database_uuid.is_nil() || !record.default_name.empty() || record.deleted ||
        !DecodeNameRegistryEntry(record.payload, &decoded) ||
        decoded.name_entry_uuid != record.object_uuid || decoded.object_uuid != record.target_object_uuid ||
        decoded.scope_uuid != record.target_schema_uuid || decoded.creator_tx != record.creator_tx)
      return refused("catalog_publication_record_invalid");
  }
  const auto owner = AcquireTransactionInventoryGuard(context.database_path);
  // Revalidate actual durable authority while holding the same guard as
  // finality. A prior caller check or retained statement snapshot cannot
  // authorize an append after this transaction has committed or rolled back.
  // Acquire current authority without duplicating the entire immutable
  // inventory/horizon history for every individual catalog or name record.
  // The copy-returning loader uses this same native cache and strong fallback.
  const auto inventory = storage::database::AcquireLocalTransactionInventorySnapshot(context.database_path);
  if (!inventory.ok() || !inventory.snapshot->inventory.publication_base ||
      inventory.snapshot->inventory.publication_base->database_uuid != context.database_uuid)
    return refused("catalog_publication_database_authority_invalid");
  const auto admitted_transaction = transaction::mga::LookupLocalTransaction(
      inventory.snapshot->inventory, transaction::mga::MakeLocalTransactionId(context.local_transaction_id));
  if (!admitted_transaction.ok() || !admitted_transaction.entry.identity.valid() ||
      admitted_transaction.entry.identity.transaction_uuid.value != context.transaction_uuid ||
      admitted_transaction.entry.state != transaction::mga::TransactionState::active || admitted_transaction.entry.rollback_only)
    return refused("catalog_publication_transaction_authority_invalid");
  const auto path = context.database_path + suffix;
  std::error_code error;
  const auto entry = std::filesystem::symlink_status(path, error);
  const bool absent = entry.type() == std::filesystem::file_type::not_found &&
      (!error || error == std::errc::no_such_file_or_directory);
  if (!absent && (error || entry.type() != std::filesystem::file_type::regular))
    return refused("catalog_journal_destination_invalid");

  std::vector<std::string> allocator_lines;
  const auto reservation = ReserveEventSequenceRange(context, "relation_metadata",
      context.database_path + ".sb.mga_relation_metadata", 1,
      [&] { return ScanNextMetadataEventSequence(context); }, &allocator_lines);
  if (!reservation.ok) return reservation.diagnostic;
  catalog_publication_detail::UnpublishedReservation unpublished{reservation};
  record.event_sequence = reservation.first;
  std::string bytes;
  if (!EncodeApiBehaviorRecord(record, &bytes)) {
    return refused("catalog_journal_frame_encoding_failed");
  }
  // After an attempted append, never rewind this reservation: failure does
  // not prove an absence of physical effects or durable bytes.
  unpublished.attempted = true;
  if (!AppendDeferredEventSequenceAllocatorLines(context, &allocator_lines, nullptr))
    return refused("catalog_journal_allocator_append_failed");
  const auto allocator_sync = storage::disk::SyncFilesystemPath(reservation.allocator_path, true);
  if (!allocator_sync.ok()) return refused("catalog_journal_allocator_sync_failed:" +
                                            allocator_sync.diagnostic.diagnostic_code);
  const auto allocator_parent = storage::disk::SyncParentDirectoryPath(reservation.allocator_path);
  if (!allocator_parent.ok()) return refused("catalog_journal_allocator_directory_sync_failed:" +
                                              allocator_parent.diagnostic.diagnostic_code);
  std::ofstream out(path, std::ios::binary | std::ios::app);
  if (!out) return refused("catalog_journal_open_failed");
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  out.flush();
  out.close();
  if (!out) return refused("catalog_journal_append_failed");
  const auto journal_sync = storage::disk::SyncFilesystemPath(path, true);
  if (!journal_sync.ok()) return refused("catalog_journal_sync_failed:" + journal_sync.diagnostic.diagnostic_code);
  const auto journal_parent = storage::disk::SyncParentDirectoryPath(path);
  if (!journal_parent.ok()) return refused("catalog_journal_directory_sync_failed:" +
                                            journal_parent.diagnostic.diagnostic_code);
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

} // namespace scratchbird::engine::internal_api
