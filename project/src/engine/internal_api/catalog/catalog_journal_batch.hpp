// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "catalog/catalog_journal_publication.hpp"
#include "transaction/mutation_authority.hpp"
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
    const EngineRequestContext& context, std::string_view operation,
    std::vector<PreparedCatalogJournalRecord> records) {
  const auto refused = [&](std::string detail) {
    return MakeInvalidRequestDiagnostic(std::string(operation), std::move(detail));
  };
  if (records.empty()) return refused("catalog_batch_records_required");
  const auto owner = AcquireTransactionInventoryGuard(context.database_path);
  const auto authority = ValidateCurrentMutatingTransactionAuthority(context, std::string(operation));
  if (authority.error) return authority;
  const auto current = storage::database::AcquireLocalTransactionInventorySnapshot(context.database_path);
  if (!current.ok()) return refused("catalog_batch_inventory_unavailable");
  const std::array<std::string, 2> paths{
      context.database_path + ".sb.catalog_object_events",
      context.database_path + ".sb.name_events.v2"};
  std::array<bool, 2> touched{};
  for (const auto& item : records) {
    const auto& record = item.record;
    if (record.creator_tx != context.local_transaction_id || record.event_sequence != 0)
      return refused("catalog_publication_context_invalid");
    switch (item.kind) {
      case CatalogJournalKind::lifecycle: {
        CatalogLifecycleRecord decoded;
        if (!DecodeCatalogLifecycleFrame(record, &decoded))
          return refused("catalog_publication_record_invalid");
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
  std::vector<std::string> allocator_lines;
  const auto reservation = ReserveEventSequenceRange(context, "relation_metadata",
      context.database_path + ".sb.mga_relation_metadata", records.size(),
      [&] { return ScanNextMetadataEventSequence(context); }, &allocator_lines);
  if (!reservation.ok) return reservation.diagnostic;
  catalog_publication_detail::UnpublishedReservation unpublished{reservation};
  std::vector<std::string> frames;
  frames.reserve(records.size());
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
    locations.push_back({offset, frames[i].size(), records[i].kind});
    offset += frames[i].size();
  }
  // The publication image is the real managed allocation. Discard duplicate
  // staging payloads before entering the durable exclusion/effects phase.
  std::vector<std::string>{}.swap(frames);
  std::vector<PreparedCatalogJournalRecord>{}.swap(records);
  // Prepare success before marking: allocation failure after release must not
  // turn an acknowledged complete operation into a fabricated partial result.
  auto success = MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
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

} // namespace scratchbird::engine::internal_api
