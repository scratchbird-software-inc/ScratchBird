// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "catalog/catalog_journal_publication.hpp"
#include "catalog/catalog_table_storage_creation.hpp"
#include "catalog/catalog_comment_publication.hpp"
#include "catalog/catalog_view_storage_creation.hpp"
#include "mga_relation_store/mga_metadata_preparation.hpp"
#include "mga_relation_store/mga_publication_callback_scope.hpp"
#include "transaction/mutation_authority.hpp"
#include "dml/mutation_savepoint_capability.hpp"
#include "memory.hpp"

#include <array>
#include <algorithm>
#include <limits>
#include <cstring>

namespace scratchbird::engine::internal_api {

struct PreparedCatalogJournalRecord {
  CatalogJournalKind kind;
  ApiBehaviorRecord record;
};

inline EngineApiDiagnostic ValidateCatalogResolverJournalFormat(
    const EngineRequestContext& context, const std::string& operation) {
  std::error_code error;
  const bool exists = std::filesystem::exists(context.database_path + ".sb.name_events.v2", error);
  if (error || (!exists && std::filesystem::exists(context.database_path + ".sb.api_events", error)) || error)
    return MakeInvalidRequestDiagnostic(operation, "legacy_name_format_unsupported");
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

// Complete low-level catalog publication. The caller prepares authorization,
// every definition and its successful result before entry. This function owns
// the native execution fence; ordinary writers cannot enter through that fence.
// Destruction never clears an exclusion after failure or allocation unwinding.
inline EngineApiDiagnostic PublishPreparedCatalogJournalBatch(
    const EngineRequestContext&, std::string_view, std::vector<PreparedCatalogJournalRecord>);
inline EngineApiDiagnostic PublishPreparedCatalogJournalBatch(
    const CatalogTableCreationCustody&, const EngineRequestContext&, std::string_view,
    std::vector<PreparedCatalogJournalRecord>, PreparedCatalogTableStorage&);

class CatalogJournalBatchPublisher final {
 private:
  friend EngineApiDiagnostic PublishPreparedCatalogJournalBatch(
      const EngineRequestContext&, std::string_view, std::vector<PreparedCatalogJournalRecord>);
  friend EngineApiDiagnostic PublishPreparedCatalogJournalBatch(
      const CatalogTableCreationCustody&, const EngineRequestContext&, std::string_view,
      std::vector<PreparedCatalogJournalRecord>, PreparedCatalogTableStorage&);
  friend EngineApiDiagnostic PublishPreparedCatalogJournalBatch(
      const CatalogCommentPublicationCustody&, const EngineRequestContext&, std::string_view,
      std::vector<PreparedCatalogJournalRecord>);
  friend EngineApiDiagnostic PublishPreparedCatalogJournalBatch(
      const CatalogViewCreationCustody&, const EngineRequestContext&, std::string_view,
      std::vector<PreparedCatalogJournalRecord>);
  static EngineApiDiagnostic Publish(
    const EngineRequestContext& context, std::string_view operation,
    std::vector<PreparedCatalogJournalRecord> records,
    PreparedCatalogTableStorage* storage, const CatalogCommentPublicationCustody* comment_custody = nullptr,
    const CatalogViewCreationCustody* view_custody = nullptr) {
  const auto refused = [&](std::string detail) {
    return MakeInvalidRequestDiagnostic(std::string(operation), std::move(detail));
  };
  if (records.empty()) return refused("catalog_batch_records_required");
  const auto owner = AcquireTransactionInventoryGuard(context.database_path);
  if (storage) {
    const auto savepoint = AdmitMgaSavepointProducer(context, MgaMutationProducer::catalog_mutation);
    if (savepoint.error) return savepoint;
  }
  const auto authority = ValidateCurrentMutatingTransactionAuthority(context, std::string(operation));
  if (authority.error) return authority;
  const auto current = storage::database::AcquireLocalTransactionInventorySnapshot(context.database_path);
  if (!current.ok()) return refused("catalog_batch_inventory_unavailable");
  const std::array<std::string, 3> paths{
      context.database_path + ".sb.catalog_object_events",
      context.database_path + ".sb.name_events.v2",
      context.database_path + ".sb.mga_relation_metadata"};
  std::array<bool, 3> touched{};
  bool owns_comment_mutation = false;
  bool owns_view_definition = false;
  touched[2] = storage != nullptr;
  for (const auto& item : records) {
    const auto& record = item.record;
    if (record.creator_tx != context.local_transaction_id || record.event_sequence != 0)
      return refused("catalog_publication_context_invalid");
    switch (item.kind) {
      case CatalogJournalKind::lifecycle: {
        CatalogLifecycleRecord decoded;
        if (!DecodeCatalogLifecycleFrame(record, &decoded))
          return refused("catalog_publication_record_invalid");
        if (std::holds_alternative<EngineCatalogCommentRecord>(decoded))
          owns_comment_mutation = true;
        if (const auto* object = std::get_if<EngineCatalogObjectRecord>(&decoded);
            object && object->object_kind == "view" && !object->deleted && object->payload.starts_with("SBVIEW02")) {
          if (!view_custody || object->object_uuid != view_custody->source().view_uuid ||
              object->payload != view_custody->payload()) return refused("view_owned_publication_required");
          owns_view_definition = true;
        }
        touched[0] = true;
        break;
      }
      case CatalogJournalKind::resolver_names: {
        NameRegistryEntry decoded;
        if (record.object_kind == "name_registry.retire") {
          if (!core::uuid::IsEngineIdentityUuid(record.object_uuid) ||
              record.target_object_uuid != record.object_uuid || !record.payload.empty() ||
              record.state != "retired" || !record.target_schema_uuid.is_nil() ||
              !record.target_database_uuid.is_nil() || !record.default_name.empty() || record.deleted)
            return refused("catalog_publication_record_invalid");
        } else if (record.object_kind != "name_registry.entry" || record.state != "active" ||
            !record.target_database_uuid.is_nil() || !record.default_name.empty() || record.deleted ||
            !DecodeNameRegistryEntry(record.payload, &decoded) ||
            decoded.name_entry_uuid != record.object_uuid ||
            decoded.object_uuid != record.target_object_uuid ||
            decoded.scope_uuid != record.target_schema_uuid ||
            decoded.creator_tx != record.creator_tx)
          return refused("catalog_publication_record_invalid");
        touched[1] = true;
        break;
      }
      default: return refused("catalog_publication_kind_invalid");
    }
  }
  if (owns_comment_mutation) {
    if (!comment_custody) return refused("comment_owned_publication_required");
    const auto readmit = comment_custody->Readmit();
    if (readmit.error) return readmit;
    const auto savepoint = AdmitMgaSavepointProducer(context, MgaMutationProducer::catalog_mutation);
    if (savepoint.error) return savepoint;
  }
  if (owns_view_definition) {
    const auto readmit = view_custody->Readmit();
    if (readmit.error) return readmit;
    const auto savepoint = AdmitMgaSavepointProducer(context, MgaMutationProducer::catalog_mutation);
    if (savepoint.error) return savepoint;
  }
  for (std::size_t i = 0; i != paths.size(); ++i) {
    if (!touched[i]) continue;
    std::error_code error;
    const auto entry = std::filesystem::symlink_status(paths[i], error);
    const bool absent = entry.type() == std::filesystem::file_type::not_found &&
        (!error || error == std::errc::no_such_file_or_directory);
    if (!absent && (error || entry.type() != std::filesystem::file_type::regular))
      return refused("catalog_journal_destination_invalid");
  }
  if (touched[1]) {
    const auto format = ValidateCatalogResolverJournalFormat(context, std::string(operation));
    if (format.error) return format;
  }
  std::size_t storage_count = 0;
  if (storage) {
    if (storage->indexes.size() == std::numeric_limits<std::size_t>::max())
      return refused("catalog_batch_size_overflow");
    storage_count = storage->indexes.size() + 1;
  }
  if (records.size() > std::numeric_limits<std::size_t>::max() - storage_count)
    return refused("catalog_batch_size_overflow");
  const auto record_count = records.size() + storage_count;
  std::vector<std::string> allocator_lines;
  const auto reservation = ReserveEventSequenceRange(context, "relation_metadata",
      context.database_path + ".sb.mga_relation_metadata", record_count,
      [&] { return ScanNextMetadataEventSequence(context); }, &allocator_lines);
  if (!reservation.ok) return reservation.diagnostic;
  catalog_publication_detail::UnpublishedReservation unpublished{reservation};
  std::vector<std::string> frames;
  frames.reserve(record_count);
  std::vector<CatalogJournalKind> kinds;
  kinds.reserve(record_count);
  std::size_t frame_bytes = 0;
  for (std::size_t i = 0; i != records.size(); ++i) {
    records[i].record.event_sequence = reservation.first + i;
    std::string frame;
    if (!EncodeApiBehaviorRecord(records[i].record, &frame))
      return refused("catalog_journal_frame_encoding_failed");
    if (frame.size() > std::numeric_limits<std::size_t>::max() - frame_bytes)
      return refused("catalog_batch_size_overflow");
    frame_bytes += frame.size();
    frames.push_back(std::move(frame));
    kinds.push_back(records[i].kind);
  }
  if (storage) {
    for (std::size_t i = 0; i != storage->indexes.size(); ++i) {
      std::string frame;
      const auto prepared = PrepareMgaIndexMetadataFrame(context, storage->indexes[i],
          reservation.first + records.size() + i, &frame);
      if (prepared.error) return prepared;
      if (frame.size() > std::numeric_limits<std::size_t>::max() - frame_bytes)
        return refused("catalog_batch_size_overflow");
      frame_bytes += frame.size(); frames.push_back(std::move(frame));
      kinds.push_back(CatalogJournalKind::relation_metadata);
    }
    EngineRequestContext descriptor_context = context;
    descriptor_context.current_schema_uuid = storage->schema_uuid;
    MgaPreparedSealedTableMetadata material;
    const auto prepared = PrepareMgaSealedTableMetadata(descriptor_context, storage->table, storage->indexes,
        reservation.first + records.size() + storage->indexes.size(), &material);
    if (prepared.error) return prepared;
    if (material.frame.size() > std::numeric_limits<std::size_t>::max() - frame_bytes)
      return refused("catalog_batch_size_overflow");
    frame_bytes += material.frame.size(); frames.push_back(std::move(material.frame));
    kinds.push_back(CatalogJournalKind::relation_metadata);
    storage->result.effective_table_descriptor.descriptor_uuid = material.descriptor.descriptor_uuid;
    storage->result.effective_table_descriptor.descriptor_kind = "mga_relation_storage_descriptor";
    storage->result.effective_table_descriptor.canonical_type_name = "table";
    storage->result.evidence.push_back({"mga_relation_descriptor", material.descriptor.descriptor_uuid});
    AddDdlPublicationResult(&storage->result, "ddl.create_table", "table", storage->table.table_uuid,
        storage->result.catalog_row_uuid, "mga_relation_metadata");
    if (!storage->result.ok) return refused("catalog_table_result_binding_invalid");
  }
  namespace memory = core::memory;
  memory::MemoryTag tag;
  tag.subsystem = core::platform::Subsystem::catalog;
  tag.category = memory::MemoryCategory::core_runtime;
  tag.lifetime = memory::MemoryLifetime::statement;
  tag.purpose = "catalog_journal_batch";
  tag.callsite = std::string(operation);
  tag.binary_ownership[memory::MemoryBinaryScopeKind::owner] = context.database_uuid.bytes;
  tag.binary_ownership[memory::MemoryBinaryScopeKind::context] = context.transaction_uuid.bytes;
  tag.binary_ownership[memory::MemoryBinaryScopeKind::database] = context.database_uuid.bytes;
  auto image = memory::DefaultMemoryManager().AllocateScoped(frame_bytes, alignof(std::max_align_t), std::move(tag));
  if (!image.ok()) return MakeEngineApiDiagnosticFromNative(image.diagnostic,
      "SB_ENGINE_API_INVALID_REQUEST", "engine.api.invalid_request", std::string(operation) + ":catalog_batch_memory_admission_failed");
  struct FrameLocation { std::size_t offset, bytes; CatalogJournalKind kind; };
  std::vector<FrameLocation> locations;
  locations.reserve(frames.size());
  auto* bytes = static_cast<char*>(image.allocation.data());
  std::size_t offset = 0;
  for (std::size_t i = 0; i != frames.size(); ++i) {
    std::memcpy(bytes + offset, frames[i].data(), frames[i].size());
    locations.push_back({offset, frames[i].size(), kinds[i]});
    offset += frames[i].size();
  }
  // The publication image is the real managed allocation. Discard duplicate
  // staging payloads before entering the durable exclusion/effects phase.
  std::vector<std::string>{}.swap(frames);
  std::vector<PreparedCatalogJournalRecord>{}.swap(records);
  std::vector<CatalogJournalKind>{}.swap(kinds);
  // Prepare success before marking: allocation failure after release must not
  // turn an acknowledged complete operation into a fabricated partial result.
  auto success = MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  // Cancellation callbacks can re-enter engine APIs. They are observations,
  // not retained authority; re-admit the actual native writer after the last
  // callback and before marking or publishing any allocator bytes.
  const auto callback = CheckMgaPublicationCallback(context, "catalog_batch_cancelled_before_publication");
  if (callback.error) return callback;
  if (storage || owns_comment_mutation || owns_view_definition) {
    const auto savepoint = AdmitMgaSavepointProducer(context, MgaMutationProducer::catalog_mutation);
    if (savepoint.error) return savepoint;
  }
  const auto final_authority = ValidateCurrentMutatingTransactionAuthority(context, std::string(operation));
  if (final_authority.error) return final_authority;
  if (owns_comment_mutation) {
    const auto readmit = comment_custody->Readmit();
    if (readmit.error) return readmit;
  }
  if (owns_view_definition) {
    const auto readmit = view_custody->Readmit();
    if (readmit.error) return readmit;
  }
  auto marked = transaction::mga::MarkLocalTransactionRollbackOnly(
      current.snapshot->inventory, transaction::mga::MakeLocalTransactionId(context.local_transaction_id));
  if (!marked.ok()) return MakeEngineApiDiagnosticFromNative(marked.diagnostic,
      "SB_ENGINE_API_INVALID_REQUEST", "engine.api.invalid_request", std::string(operation) + ":catalog_batch_mark_failed");
  auto fenced = storage::database::PersistLocalTransactionInventoryToDatabase(
      context.database_path, std::move(marked.inventory));
  const auto unresolved = [&](std::string phase, const core::platform::DiagnosticRecord* cause = nullptr) {
    auto result = cause ? MakeEngineApiDiagnosticFromNative(*cause,
        "SB_ENGINE_API_INVALID_REQUEST", "engine.api.invalid_request", std::string(operation) + ":" + phase)
        : refused(phase);
    result.identity_fields.emplace_back("database_uuid", context.database_uuid);
    result.identity_fields.emplace_back("transaction_uuid", context.transaction_uuid);
    result.fields.push_back({"local_transaction_id", std::to_string(context.local_transaction_id)});
    result.fields.push_back({"publication_phase", std::move(phase)});
    result.fields.push_back({"completion", "reconcile_native_inventory"});
    // Re-read actual authority after an uncertain acknowledgement. A valid
    // completed release can exist despite an error, and mark failure need not
    // have established a fence. Never substitute a newer base for release.
    const auto observed = storage::database::AcquireStrongLocalTransactionInventorySnapshot(context.database_path);
    std::string exclusion = "unknown";
    if (observed.ok() && observed.snapshot->inventory.publication_base &&
        observed.snapshot->inventory.publication_base->database_uuid == context.database_uuid) {
      const auto entry = transaction::mga::LookupLocalTransaction(observed.snapshot->inventory,
          transaction::mga::MakeLocalTransactionId(context.local_transaction_id));
      if (entry.ok() && entry.entry.identity.transaction_uuid.value == context.transaction_uuid)
        exclusion = entry.entry.rollback_only ? "retained" : "absent";
    }
    result.fields.push_back({"observed_native_commit_exclusion", std::move(exclusion)});
    return result;
  };
  if (!fenced.ok()) return unresolved("catalog_batch_mark_publication_failed", &fenced.diagnostic);
  unpublished.attempted = true;
  if (!AppendDeferredEventSequenceAllocatorLines(context, &allocator_lines, nullptr))
    return unresolved("catalog_journal_allocator_append_failed");
  const auto allocator_sync = storage::disk::SyncFilesystemPath(reservation.allocator_path, true);
  if (!allocator_sync.ok()) return unresolved("catalog_journal_allocator_sync_failed", &allocator_sync.diagnostic);
  const auto allocator_parent = storage::disk::SyncParentDirectoryPath(reservation.allocator_path);
  if (!allocator_parent.ok()) return unresolved("catalog_journal_allocator_directory_sync_failed", &allocator_parent.diagnostic);
  for (std::size_t i = 0; i != paths.size(); ++i) {
    if (!touched[i]) continue;
    std::ofstream out(paths[i], std::ios::binary | std::ios::app);
    if (!out) return unresolved("catalog_journal_open_failed");
    for (const auto& frame : locations) {
      if (static_cast<std::size_t>(frame.kind) != i) continue;
      out.write(bytes + frame.offset, static_cast<std::streamsize>(frame.bytes));
      if (!out) return unresolved("catalog_journal_append_failed");
    }
    out.flush();
    out.close();
    if (!out) return unresolved("catalog_journal_append_failed");
    const auto sync = storage::disk::SyncFilesystemPath(paths[i], true);
    if (!sync.ok()) return unresolved("catalog_journal_sync_failed", &sync.diagnostic);
    const auto parent = storage::disk::SyncParentDirectoryPath(paths[i]);
    if (!parent.ok()) return unresolved("catalog_journal_directory_sync_failed", &parent.diagnostic);
  }
  // Only this invocation's returned publication base can release its fence.
  // Reloading a newer inventory to get around a stale base would transfer
  // ownership and potentially clear another operation's exclusion.
  auto completed = std::move(fenced.inventory);
  auto entry = std::find_if(completed.entries.begin(), completed.entries.end(), [&](const auto& candidate) {
    return candidate.identity.local_id.value == context.local_transaction_id;
  });
  if (entry == completed.entries.end() || entry->identity.transaction_uuid.value != context.transaction_uuid ||
      entry->state != transaction::mga::TransactionState::active || !entry->rollback_only)
    return unresolved("catalog_batch_release_identity_mismatch");
  entry->rollback_only = false;
  const auto released = storage::database::PersistLocalTransactionInventoryToDatabase(context.database_path, std::move(completed));
  if (!released.ok()) return unresolved("catalog_batch_release_publication_failed", &released.diagnostic);
  return success;
}
};

inline EngineApiDiagnostic PublishPreparedCatalogJournalBatch(
    const EngineRequestContext& context, std::string_view operation,
    std::vector<PreparedCatalogJournalRecord> records) {
  return CatalogJournalBatchPublisher::Publish(context, operation, std::move(records), nullptr);
}

inline EngineApiDiagnostic PublishPreparedCatalogJournalBatch(
    const CatalogTableCreationCustody&, const EngineRequestContext& context, std::string_view operation,
    std::vector<PreparedCatalogJournalRecord> records, PreparedCatalogTableStorage& storage) {
  return CatalogJournalBatchPublisher::Publish(context, operation, std::move(records), &storage);
}

inline EngineApiDiagnostic PublishPreparedCatalogJournalBatch(
    const CatalogCommentPublicationCustody& custody, const EngineRequestContext& context,
    std::string_view operation, std::vector<PreparedCatalogJournalRecord> records) {
  return CatalogJournalBatchPublisher::Publish(context, operation, std::move(records), nullptr, &custody);
}

inline EngineApiDiagnostic PublishPreparedCatalogJournalBatch(
    const CatalogViewCreationCustody& custody, const EngineRequestContext& context,
    std::string_view operation, std::vector<PreparedCatalogJournalRecord> records) {
  return CatalogJournalBatchPublisher::Publish(context, operation, std::move(records), nullptr, nullptr, &custody);
}

} // namespace scratchbird::engine::internal_api
