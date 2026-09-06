// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "mga_relation_store/mga_relation_store.hpp"
#include "mga_relation_store/mga_contextual_text_descriptor.hpp"
#include "mga_relation_store/mga_event_sequence_allocator.hpp"
#include "mga_relation_store/mga_heap_runtime_support.hpp"
#include "mga_relation_store/mga_large_value_store.hpp"
#include "mga_relation_store/mga_relation_metadata_store.hpp"
#include "mga_relation_store/mga_row_codec.hpp"
#include "mga_relation_store/mga_row_version_reader.hpp"
#include "mga_relation_store/mga_savepoint_store.hpp"
#include "mga_relation_store/mga_relation_store_internal_support.hpp"

#include "api_diagnostics.hpp"
#include "catalog/name_resolution_api.hpp"
#include "datatype_catalog_manifest.hpp"
#include "descriptor_value_runtime.hpp"
#include "ipar_fault_injection.hpp"
#include "local_transaction_store.hpp"
#include "query/contextual_text_policy_registry_v2.hpp"
#include "query/contextual_text_target_authority_resolver_v2.hpp"
#include "query/plan_api.hpp"
#include "security/security_model.hpp"
#include "transaction/transaction_api.hpp"
#include "transaction_inventory.hpp"
#include "transaction_state.hpp"
#include "typed_update_carrier_codec.hpp"
#include "uuid.hpp"
#include "hash_digest.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>


namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_MGA_VISIBLE_HEAP_READER_IMPLEMENTATION_AUTHORITY

namespace {

constexpr const char* kRowStoreMagic = "SBMGA1";

std::string ScopedRelationSegmentName(const std::string& table_uuid) {
  std::string name;
  name.reserve(table_uuid.size());
  for (const char ch : table_uuid) {
    const bool safe = (ch >= 'a' && ch <= 'z') ||
                      (ch >= 'A' && ch <= 'Z') ||
                      (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
    name.push_back(safe ? ch : '_');
  }
  return name.empty() ? std::string("unknown") : name;
}

std::string ScopedRelationStoreRoot(const EngineRequestContext& context) {
  return context.database_path + ".sb.mga_relation_scope";
}

std::string ScopedRowStorePath(const EngineRequestContext& context,
                               const std::string& table_uuid) {
  return ScopedRelationStoreRoot(context) + "/" +
         ScopedRelationSegmentName(table_uuid) + ".rows";
}

std::string ScopedRowBinaryStorePath(const EngineRequestContext& context,
                                     const std::string& table_uuid) {
  return ScopedRelationStoreRoot(context) + "/" +
         ScopedRelationSegmentName(table_uuid) + ".rows.sbnr";
}

bool FileExistsAndNotEmpty(const std::string& path) {
  std::error_code ignored;
  return std::filesystem::exists(path, ignored) &&
         std::filesystem::file_size(path, ignored) != 0;
}

struct ExistingStoreFileIdentity {
  std::uintmax_t file_size = 0;
  std::int64_t file_mtime_ticks = 0;
  bool ok = false;
};

ExistingStoreFileIdentity ExistingFileIdentity(const std::string& path) {
  ExistingStoreFileIdentity identity;
  std::error_code ignored;
  if (path.empty() || !std::filesystem::exists(path, ignored)) {
    return identity;
  }
  identity.file_size = std::filesystem::file_size(path, ignored);
  if (ignored || identity.file_size == static_cast<std::uintmax_t>(-1)) {
    return {};
  }
  ignored.clear();
  const auto mtime = std::filesystem::last_write_time(path, ignored);
  if (ignored) return {};
  identity.file_mtime_ticks =
      static_cast<std::int64_t>(mtime.time_since_epoch().count());
  identity.ok = true;
  return identity;
}

EngineApiDiagnostic OkDiagnostic() {
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

std::uint64_t ParseU64(const std::string& text,
                       const std::uint64_t fallback = 0) {
  if (text.empty()) return fallback;
  try {
    return static_cast<std::uint64_t>(std::stoull(text));
  } catch (...) {
    return fallback;
  }
}

int HexValue(const char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return 10 + value - 'a';
  if (value >= 'A' && value <= 'F') return 10 + value - 'A';
  return -1;
}

std::string DecodeCrudTextForHeapRead(const std::string& encoded) {
  std::string decoded;
  if ((encoded.size() % 2) != 0) return decoded;
  decoded.reserve(encoded.size() / 2);
  for (std::size_t index = 0; index < encoded.size(); index += 2) {
    const int high = HexValue(encoded[index]);
    const int low = HexValue(encoded[index + 1]);
    if (high < 0 || low < 0) return {};
    decoded.push_back(static_cast<char>((high << 4) | low));
  }
  return decoded;
}

std::vector<std::pair<std::string, std::string>> DecodeCrudPairsForHeapRead(
    const std::string& encoded) {
  std::vector<std::pair<std::string, std::string>> pairs;
  pairs.reserve(8);
  std::size_t start = 0;
  while (start <= encoded.size()) {
    const auto pipe = encoded.find('|', start);
    const std::size_t end = pipe == std::string::npos ? encoded.size() : pipe;
    const auto equals = encoded.find('=', start);
    if (equals != std::string::npos && equals < end) {
      pairs.push_back(
          {DecodeCrudTextForHeapRead(encoded.substr(start, equals - start)),
           DecodeCrudTextForHeapRead(
               encoded.substr(equals + 1, end - equals - 1))});
    }
    if (pipe == std::string::npos) break;
    start = pipe + 1;
  }
  return pairs;
}

}  // namespace

bool AccountHeapReadEngineDescriptorMemory(
    const EngineDescriptor& descriptor, std::uint64_t* total) {
  return AccountHeapReadOwnedString(descriptor.descriptor_uuid.canonical,
                                    total) &&
         AccountHeapReadOwnedString(descriptor.descriptor_kind, total) &&
         AccountHeapReadOwnedString(descriptor.canonical_type_name, total) &&
         AccountHeapReadOwnedString(descriptor.encoded_descriptor, total);
}

std::optional<std::uint64_t> HeapReadStorageDescriptorMemoryBytes(
    const MgaRelationStorageDescriptor& descriptor) {
  std::uint64_t bytes = sizeof(descriptor);
  std::uint64_t allocation_bytes = 0;
  const auto account_uuid = [&](const EngineUuid& uuid) {
    return AccountHeapReadOwnedString(uuid.canonical, &bytes);
  };
  if (!account_uuid(descriptor.descriptor_uuid) ||
      !account_uuid(descriptor.database_uuid) ||
      !account_uuid(descriptor.schema_uuid) ||
      !account_uuid(descriptor.relation_uuid) ||
      !account_uuid(descriptor.primary_filespace_uuid) ||
      !AccountHeapReadOwnedString(descriptor.relation_kind, &bytes) ||
      !AccountHeapReadOwnedString(descriptor.storage_profile, &bytes) ||
      !AccountHeapReadOwnedString(descriptor.row_identity_rule, &bytes) ||
      !AccountHeapReadOwnedString(descriptor.version_identity_rule, &bytes) ||
      !AccountHeapReadOwnedString(descriptor.mutation_rule, &bytes) ||
      !AccountHeapReadOwnedString(descriptor.visibility_rule, &bytes) ||
      !AccountHeapReadOwnedString(descriptor.cleanup_rule, &bytes) ||
      !AccountHeapReadOwnedString(descriptor.recovery_rule, &bytes) ||
      !AccountHeapReadOwnedString(descriptor.descriptor_status, &bytes) ||
      !CheckedHeapReadMemoryMultiply(
          static_cast<std::uint64_t>(descriptor.columns.capacity()),
          sizeof(MgaRelationColumnStorageDescriptor), &allocation_bytes) ||
      !CheckedHeapReadMemoryAdd(allocation_bytes, &bytes) ||
      !CheckedHeapReadMemoryMultiply(
          static_cast<std::uint64_t>(descriptor.indexes.capacity()),
          sizeof(MgaRelationIndexStorageDescriptor), &allocation_bytes) ||
      !CheckedHeapReadMemoryAdd(allocation_bytes, &bytes) ||
      !CheckedHeapReadMemoryMultiply(
          static_cast<std::uint64_t>(
              descriptor.required_evidence_kinds.capacity()),
          sizeof(std::string), &allocation_bytes) ||
      !CheckedHeapReadMemoryAdd(allocation_bytes, &bytes)) {
    return std::nullopt;
  }
  for (const auto& column : descriptor.columns) {
    if (!account_uuid(column.column_uuid) ||
        !AccountHeapReadOwnedString(column.canonical_name_key, &bytes) ||
        !AccountHeapReadEngineDescriptorMemory(column.value_descriptor,
                                               &bytes) ||
        !AccountHeapReadOwnedString(column.storage_class, &bytes) ||
        !AccountHeapReadOwnedString(column.charset_uuid, &bytes) ||
        !AccountHeapReadOwnedString(column.collation_uuid, &bytes) ||
        !AccountHeapReadOwnedString(column.overflow_policy, &bytes)) {
      return std::nullopt;
    }
  }
  for (const auto& index : descriptor.indexes) {
    if (!account_uuid(index.index_uuid) ||
        !AccountHeapReadOwnedString(index.family, &bytes) ||
        !AccountHeapReadOwnedString(index.profile, &bytes) ||
        !AccountHeapReadOwnedString(index.predicate_kind, &bytes) ||
        !AccountHeapReadOwnedString(index.predicate_column, &bytes) ||
        !AccountHeapReadOwnedString(index.predicate_value, &bytes) ||
        !AccountHeapReadOwnedString(index.residency_policy, &bytes) ||
        !CheckedHeapReadMemoryMultiply(
            static_cast<std::uint64_t>(index.key_envelopes.capacity()),
            sizeof(std::string), &allocation_bytes) ||
        !CheckedHeapReadMemoryAdd(allocation_bytes, &bytes) ||
        !CheckedHeapReadMemoryMultiply(
            static_cast<std::uint64_t>(index.include_columns.capacity()),
            sizeof(std::string), &allocation_bytes) ||
        !CheckedHeapReadMemoryAdd(allocation_bytes, &bytes)) {
      return std::nullopt;
    }
    for (const auto& value : index.key_envelopes) {
      if (!AccountHeapReadOwnedString(value, &bytes)) return std::nullopt;
    }
    for (const auto& value : index.include_columns) {
      if (!AccountHeapReadOwnedString(value, &bytes)) return std::nullopt;
    }
  }
  for (const auto& value : descriptor.required_evidence_kinds) {
    if (!AccountHeapReadOwnedString(value, &bytes)) return std::nullopt;
  }
  return bytes;
}

std::optional<std::uint64_t> HeapReadTransactionStateMemoryBytes(
    const std::map<std::uint64_t, std::string>& transactions) {
  constexpr std::uint64_t kMapNodeOverhead = 4 * sizeof(void*);
  std::uint64_t bytes = sizeof(transactions);
  std::uint64_t node_bytes = 0;
  if (!CheckedHeapReadMemoryMultiply(
          static_cast<std::uint64_t>(transactions.size()),
          sizeof(std::pair<const std::uint64_t, std::string>) +
              kMapNodeOverhead,
          &node_bytes) ||
      !CheckedHeapReadMemoryAdd(node_bytes, &bytes)) {
    return std::nullopt;
  }
  for (const auto& [transaction, state] : transactions) {
    (void)transaction;
    if (!AccountHeapReadOwnedString(state, &bytes)) return std::nullopt;
  }
  return bytes;
}

std::optional<std::uint64_t> HeapReadSavepointMemoryBytes(
    const SavepointParsedState& savepoints) {
  constexpr std::uint64_t kMapNodeOverhead = 4 * sizeof(void*);
  std::uint64_t bytes = sizeof(savepoints);
  for (const auto& [transaction, names] : savepoints.active_savepoints) {
    (void)transaction;
    std::uint64_t node_bytes =
        sizeof(std::pair<const std::uint64_t,
                         std::map<std::string, SavepointCutoffs>>) +
        kMapNodeOverhead;
    if (!CheckedHeapReadMemoryAdd(node_bytes, &bytes)) return std::nullopt;
    for (const auto& [name, cutoffs] : names) {
      (void)cutoffs;
      node_bytes =
          sizeof(std::pair<const std::string, SavepointCutoffs>) +
          kMapNodeOverhead;
      if (!CheckedHeapReadMemoryAdd(node_bytes, &bytes) ||
          !AccountHeapReadOwnedString(name, &bytes)) {
        return std::nullopt;
      }
    }
  }
  for (const auto& [transaction, ranges] : savepoints.rollback_ranges) {
    (void)transaction;
    std::uint64_t node_bytes =
        sizeof(std::pair<const std::uint64_t,
                         std::vector<SavepointRollbackRange>>) +
        kMapNodeOverhead;
    std::uint64_t allocation_bytes = 0;
    if (!CheckedHeapReadMemoryAdd(node_bytes, &bytes) ||
        !CheckedHeapReadMemoryMultiply(
            static_cast<std::uint64_t>(ranges.capacity()),
            sizeof(SavepointRollbackRange), &allocation_bytes) ||
        !CheckedHeapReadMemoryAdd(allocation_bytes, &bytes)) {
      return std::nullopt;
    }
  }
  for (const auto& [transaction, ranges] :
       savepoints.normalized_row_rollback_ranges) {
    (void)transaction;
    std::uint64_t node_bytes =
        sizeof(std::pair<
                   const std::uint64_t,
                   std::vector<std::pair<std::uint64_t, std::uint64_t>>>) +
        kMapNodeOverhead;
    std::uint64_t allocation_bytes = 0;
    if (!CheckedHeapReadMemoryAdd(node_bytes, &bytes) ||
        !CheckedHeapReadMemoryMultiply(
            static_cast<std::uint64_t>(ranges.capacity()),
            sizeof(std::pair<std::uint64_t, std::uint64_t>),
            &allocation_bytes) ||
        !CheckedHeapReadMemoryAdd(allocation_bytes, &bytes)) {
      return std::nullopt;
    }
  }
  return bytes;
}

std::optional<std::uint64_t> HeapReadVisibilityMapMemoryBytes(
    const std::unordered_map<std::string, std::size_t>& rows) {
  constexpr std::uint64_t kNodeOverhead = 4 * sizeof(void*);
  std::uint64_t bytes = sizeof(rows);
  std::uint64_t allocation_bytes = 0;
  if (!CheckedHeapReadMemoryMultiply(
          static_cast<std::uint64_t>(rows.bucket_count()), sizeof(void*),
          &allocation_bytes) ||
      !CheckedHeapReadMemoryAdd(allocation_bytes, &bytes) ||
      !CheckedHeapReadMemoryMultiply(
          static_cast<std::uint64_t>(rows.size()),
          sizeof(std::pair<const std::string, std::size_t>) + kNodeOverhead,
          &allocation_bytes) ||
      !CheckedHeapReadMemoryAdd(allocation_bytes, &bytes)) {
    return std::nullopt;
  }
  for (const auto& [uuid, ordinal] : rows) {
    (void)ordinal;
    if (!AccountHeapReadOwnedString(uuid, &bytes)) return std::nullopt;
  }
  return bytes;
}

MgaVisibleHeapRelationReadResult ReadVisibleMgaHeapRelationWithObservation(
    const EngineRequestContext& context,
    const MgaVisibleHeapRelationReadRequest& request,
    HeapReadRuntimeObservation* runtime_observation,
    const PreparedMgaHeapReadAuthority* prepared_authority) {
  MgaVisibleHeapRelationReadResult result;
  const auto refuse = [&](EngineApiDiagnostic diagnostic,
                          const BoundedScopedRowReadControl* control = nullptr,
                          const MgaHeapReadFailureCategoryV1 category =
                              MgaHeapReadFailureCategoryV1::kInvalidRequest) {
    result.ok = false;
    result.diagnostic = std::move(diagnostic);
    result.failure_category = category;
    if (control != nullptr) {
      result.failure_category =
          control->failure_category != MgaHeapReadFailureCategoryV1::kNone
              ? control->failure_category
              : category != MgaHeapReadFailureCategoryV1::kInvalidRequest
                    ? category
                    : MgaHeapReadFailureCategoryV1::kResource;
    }
    result.descriptor = {};
    std::vector<CrudRowVersionRecord>{}.swap(result.visible_rows);
    std::vector<EngineEvidenceReference>{}.swap(result.evidence);
    result.current_relation_base_generation = 0;
    result.current_live_memory_bytes = 0;
    result.memory_grant_bytes = request.maximum_memory_bytes;
    result.memory_receipt_complete = false;
    if (control != nullptr) {
      result.scanned_row_version_count = control->decoded_row_versions;
      result.decoded_byte_count = control->decoded_bytes;
      result.cancellation_observed = control->cancellation_observed;
      result.peak_live_memory_bytes = control->peak_live_memory_bytes;
    }
    return result;
  };
  const auto invalid = [&](std::string detail,
                           const BoundedScopedRowReadControl* control = nullptr,
                           const MgaHeapReadFailureCategoryV1 category =
                               MgaHeapReadFailureCategoryV1::kInvalidRequest) {
    return refuse(MakeInvalidRequestDiagnostic("mga.heap_relation_read",
                                               std::move(detail)),
                  control, category);
  };

  const auto& cancellation_requested =
      request.borrowed_cancellation_requested == nullptr
          ? request.cancellation_requested
          : *request.borrowed_cancellation_requested;
  const auto& relation_uuid = request.borrowed_relation_uuid == nullptr
                                  ? request.relation_uuid
                                  : *request.borrowed_relation_uuid;

  if (relation_uuid.empty()) {
    return invalid("relation_uuid_required");
  }
  if (context.local_transaction_id == 0 ||
      context.transaction_uuid.canonical.empty()) {
    return invalid("exact_active_transaction_and_snapshot_required", nullptr,
                   MgaHeapReadFailureCategoryV1::kMgaContext);
  }
  if (request.maximum_scanned_row_versions == 0 ||
      request.maximum_decoded_bytes == 0 ||
      request.maximum_output_rows == 0) {
    return invalid("nonzero_heap_read_resource_bounds_required", nullptr,
                   MgaHeapReadFailureCategoryV1::kResource);
  }
  if (!cancellation_requested) {
    return invalid("engine_cancellation_probe_required");
  }
  if (cancellation_requested()) {
    result.cancellation_observed = true;
    return invalid("heap_read_cancelled_before_descriptor_load", nullptr,
                   MgaHeapReadFailureCategoryV1::kCancellation);
  }

  const auto inventory_guard =
      AcquireTransactionInventoryGuard(context.database_path);
  BoundedScopedRowReadControl control;
  control.maximum_row_versions = request.maximum_scanned_row_versions;
  control.maximum_bytes = request.maximum_decoded_bytes;
  control.maximum_memory_bytes = request.maximum_memory_bytes;
  control.cancellation_requested = &cancellation_requested;
  control.runtime_observation = runtime_observation;
  scratchbird::transaction::mga::SnapshotVectorDescriptor owned_snapshot;
  std::map<std::uint64_t, std::string> owned_transaction_states;
  const scratchbird::transaction::mga::SnapshotVectorDescriptor*
      snapshot_vector = nullptr;
  const std::map<std::uint64_t, std::string>* transaction_states = nullptr;
  std::uint64_t current_relation_base_generation = 0;
  if (prepared_authority != nullptr) {
    const auto* prepared_statement = prepared_authority->statement.get();
    if (prepared_statement == nullptr ||
        prepared_statement->transaction_states == nullptr ||
        prepared_authority->relation_uuid != relation_uuid ||
        prepared_statement->database_uuid != context.database_uuid.canonical ||
        prepared_statement->statement_uuid != context.statement_uuid.canonical ||
        prepared_statement->transaction_uuid !=
            context.transaction_uuid.canonical ||
        prepared_statement->statement_snapshot_uuid !=
            context.statement_snapshot_uuid.canonical ||
        prepared_statement->statement_metadata_snapshot_uuid !=
            context.statement_metadata_snapshot_uuid.canonical ||
        prepared_statement->catalog_epoch_uuid !=
            context.catalog_epoch_uuid.canonical ||
        prepared_statement->authorization_authority_uuid !=
            context.authorization_context.authority_uuid.canonical ||
        prepared_statement->catalog_generation !=
            context.catalog_generation_id ||
        prepared_statement->security_epoch !=
            context.authorization_context.security_epoch ||
        prepared_statement->policy_epoch !=
            context.authorization_context.policy_epoch ||
        prepared_statement->resource_epoch != context.resource_epoch ||
        prepared_statement->local_transaction_id !=
            context.local_transaction_id ||
        !prepared_statement->snapshot_vector.inventory_authoritative ||
        !prepared_statement->snapshot_vector.complete) {
      return invalid("prepared_heap_read_authority_is_stale", &control,
                     MgaHeapReadFailureCategoryV1::kMgaContext);
    }
    snapshot_vector = &prepared_statement->snapshot_vector;
    transaction_states = prepared_statement->transaction_states.get();
    current_relation_base_generation =
        prepared_authority->current_relation_base_generation;
  } else {
    EngineResolveStatementSnapshotRequest snapshot_request;
    snapshot_request.context = context;
    auto snapshot = EngineResolveStatementSnapshot(snapshot_request);
    if (!snapshot.ok || !snapshot.snapshot_vector.inventory_authoritative ||
        !snapshot.snapshot_vector.complete) {
      return invalid("exact_current_statement_snapshot_vector_required",
                     nullptr, MgaHeapReadFailureCategoryV1::kMgaContext);
    }
    // Retain only the engine-issued visibility carrier.  Request/result
    // diagnostics and identity strings are catalog-authority transients and
    // must not remain live in the operator-local row-carrier phase.
    owned_snapshot = std::move(snapshot.snapshot_vector);
    snapshot_vector = &owned_snapshot;
    auto loaded_descriptor =
        LoadMgaRelationStorageDescriptor(context, relation_uuid);
    if (!loaded_descriptor.ok) {
      return refuse(std::move(loaded_descriptor.diagnostic), nullptr,
                    MgaHeapReadFailureCategoryV1::kCatalog);
    }
    const auto& loaded = loaded_descriptor.descriptor;
    if (loaded.relation_uuid.canonical != relation_uuid ||
        loaded.database_uuid.canonical != context.database_uuid.canonical ||
        loaded.relation_kind != "table" ||
        loaded.storage_profile != "local_mga_rowstore_v1" ||
        loaded.descriptor_uuid.canonical.empty() ||
        loaded.descriptor_generation == 0 ||
        loaded.descriptor_status.empty()) {
      return invalid("current_persisted_local_heap_descriptor_required",
                     nullptr, MgaHeapReadFailureCategoryV1::kCatalog);
    }
    result.descriptor = std::move(loaded_descriptor.descriptor);
    CrudState metadata;
    const auto metadata_loaded = LoadMgaMetadata(&metadata, context);
    if (metadata_loaded.error) {
      return refuse(metadata_loaded, nullptr,
                    MgaHeapReadFailureCategoryV1::kCatalog);
    }
    metadata.transactions.clear();
    metadata.max_transaction_id = 0;
    const auto authority =
        OverlayMgaTransactionAuthorityForStoreModule(context, &metadata,
                                                     true);
    if (authority.error) {
      return refuse(authority, nullptr,
                    MgaHeapReadFailureCategoryV1::kMgaContext);
    }
    FilterMgaRelationMetadataForStoreModule(context, &metadata);
    const auto table = FindVisibleCrudTable(
        metadata, relation_uuid, context.local_transaction_id);
    if (!table) {
      return invalid("heap_relation_not_visible", nullptr,
                     MgaHeapReadFailureCategoryV1::kCatalog);
    }
    const auto temporary_authority =
        ValidateMgaHeapTemporaryRelationAuthorityForStoreModule(context,
                                                                *table);
    if (temporary_authority.error) {
      return refuse(temporary_authority, nullptr,
                    MgaHeapReadFailureCategoryV1::kCatalog);
    }
    current_relation_base_generation = table->event_sequence;
    owned_transaction_states = std::move(metadata.transactions);
    transaction_states = &owned_transaction_states;
  }
  const auto& descriptor = prepared_authority == nullptr
                               ? result.descriptor
                               : prepared_authority->descriptor;
  const auto creator_visible = [&](const std::uint64_t creator) {
    if (creator == 0) return false;
    const auto transaction = transaction_states->find(creator);
    if (transaction == transaction_states->end()) return false;
    if (creator == snapshot_vector->owning_transaction.value) {
      return transaction->second == "active" ||
             transaction->second == "preparing" ||
             transaction->second == "prepared";
    }
    if (transaction->second != "committed" &&
        transaction->second != "archived") {
      return false;
    }
    if (snapshot_vector->visible_committed_high_watermark == 0 ||
        creator > snapshot_vector->visible_committed_high_watermark) {
      return false;
    }
    return !std::binary_search(
               snapshot_vector->active_excluded_local_transaction_ids.begin(),
               snapshot_vector->active_excluded_local_transaction_ids.end(),
               creator) &&
           !std::binary_search(
               snapshot_vector->in_doubt_excluded_local_transaction_ids.begin(),
               snapshot_vector->in_doubt_excluded_local_transaction_ids.end(),
               creator);
  };
  const auto descriptor_memory =
      HeapReadStorageDescriptorMemoryBytes(descriptor);
  const auto transaction_memory = prepared_authority == nullptr
                                      ? HeapReadTransactionStateMemoryBytes(
                                            *transaction_states)
                                      : std::optional<std::uint64_t>{0};
  std::uint64_t authority_memory = sizeof(result);
  std::uint64_t snapshot_vector_bytes = 0;
  if (!descriptor_memory.has_value() || !transaction_memory.has_value() ||
      !CheckedHeapReadMemoryMultiply(
          static_cast<std::uint64_t>(prepared_authority == nullptr
                                         ? snapshot_vector
                                               ->active_excluded_local_transaction_ids
                                               .capacity()
                                         : 0),
          sizeof(std::uint64_t), &snapshot_vector_bytes) ||
      !CheckedHeapReadMemoryAdd(snapshot_vector_bytes, &authority_memory) ||
      !CheckedHeapReadMemoryMultiply(
          static_cast<std::uint64_t>(prepared_authority == nullptr
                                         ? snapshot_vector
                                               ->in_doubt_excluded_local_transaction_ids
                                               .capacity()
                                         : 0),
          sizeof(std::uint64_t), &snapshot_vector_bytes) ||
      !CheckedHeapReadMemoryAdd(snapshot_vector_bytes, &authority_memory) ||
      !CheckedHeapReadMemoryAdd(*descriptor_memory, &authority_memory) ||
      !CheckedHeapReadMemoryAdd(*transaction_memory, &authority_memory)) {
    control.refusal_detail = "heap_read_authority_memory_receipt_overflow";
    return invalid(control.refusal_detail, &control);
  }
  control.retained_parent_memory_bytes = authority_memory;
  if (!ObserveBoundedHeapReadMemory(&control, authority_memory)) {
    return invalid(control.refusal_detail, &control);
  }
  if (prepared_authority != nullptr) {
    result.descriptor = descriptor;
  }
  std::vector<CrudRowVersionRecord> row_versions;
  bool used_segment = false;
  if (!LoadDecodedScopedRowsForTableBounded(context,
                                            relation_uuid,
                                            &control,
                                            &row_versions,
                                            &used_segment)) {
    return invalid(control.refusal_detail.empty()
                       ? "bounded_scoped_heap_read_failed"
                       : control.refusal_detail,
                   &control);
  }
  result.scanned_row_version_count = control.decoded_row_versions;
  result.decoded_byte_count = control.decoded_bytes;
  result.scoped_physical_segment_used = used_segment;

  const auto row_version_memory =
      HeapReadRowVectorMemoryBytes(row_versions);
  if (!row_version_memory.has_value()) {
    control.refusal_detail = "heap_read_visibility_memory_receipt_overflow";
    return invalid(control.refusal_detail, &control);
  }
  std::uint64_t savepoint_parent_memory = authority_memory;
  if (!CheckedHeapReadMemoryAdd(*row_version_memory,
                                &savepoint_parent_memory)) {
    control.refusal_detail = "heap_read_visibility_memory_receipt_overflow";
    return invalid(control.refusal_detail, &control);
  }
  SavepointParsedState savepoints;
  if (!ParseSavepointsBounded(context, &control, savepoint_parent_memory,
                              &savepoints)) {
    return invalid(control.refusal_detail.empty()
                       ? "bounded_savepoint_read_failed"
                       : control.refusal_detail,
                   &control);
  }
  const auto savepoint_memory = HeapReadSavepointMemoryBytes(savepoints);
  std::uint64_t visibility_base_memory = authority_memory;
  if (!savepoint_memory.has_value() ||
      !CheckedHeapReadMemoryAdd(*savepoint_memory,
                                &visibility_base_memory) ||
      !CheckedHeapReadMemoryAdd(*row_version_memory,
                                &visibility_base_memory) ||
      !ObserveBoundedHeapReadMemory(&control, visibility_base_memory)) {
    if (control.refusal_detail.empty()) {
      control.refusal_detail = "heap_read_visibility_memory_receipt_overflow";
    }
    return invalid(control.refusal_detail, &control);
  }
  std::vector<CrudRowVersionRecord> admitted_versions;
  std::uint64_t admitted_reserve_bytes = 0;
  std::uint64_t admitted_preflight_memory = visibility_base_memory;
  if (!CheckedHeapReadMemoryMultiply(
          static_cast<std::uint64_t>(row_versions.size()),
          sizeof(CrudRowVersionRecord), &admitted_reserve_bytes) ||
      !CheckedHeapReadMemoryAdd(sizeof(admitted_versions),
                                &admitted_preflight_memory) ||
      !CheckedHeapReadMemoryAdd(admitted_reserve_bytes,
                                &admitted_preflight_memory) ||
      !ObserveBoundedHeapReadMemory(&control,
                                    admitted_preflight_memory)) {
    if (control.refusal_detail.empty()) {
      control.refusal_detail =
          "heap_read_admission_memory_receipt_overflow";
    }
    return invalid(control.refusal_detail, &control);
  }
  admitted_versions.reserve(row_versions.size());
  for (auto& row : row_versions) {
    if (cancellation_requested()) {
      control.cancellation_observed = true;
      control.refusal_detail = "heap_read_cancelled_during_visibility";
      return invalid(control.refusal_detail, &control);
    }
    if (row.table_uuid != relation_uuid) {
      return invalid("scoped_heap_row_relation_identity_mismatch", &control,
                     MgaHeapReadFailureCategoryV1::kCorruptStorage);
    }
    if (RowEventRolledBackBySavepoint(savepoints,
                                      row.creator_tx,
                                      row.event_sequence)) {
      ++result.invisible_row_version_count;
      continue;
    }
    current_relation_base_generation =
        std::max(current_relation_base_generation, row.event_sequence);
    admitted_versions.push_back(std::move(row));
  }
  {
    const auto current_rows = HeapReadRowVectorMemoryBytes(row_versions);
    const auto admitted_rows =
        HeapReadRowVectorMemoryBytes(admitted_versions);
    std::uint64_t phase_memory = authority_memory;
    if (!current_rows.has_value() || !admitted_rows.has_value() ||
        !CheckedHeapReadMemoryAdd(*savepoint_memory, &phase_memory) ||
        !CheckedHeapReadMemoryAdd(*current_rows, &phase_memory) ||
        !CheckedHeapReadMemoryAdd(*admitted_rows, &phase_memory) ||
        !ObserveBoundedHeapReadMemory(&control, phase_memory)) {
      if (control.refusal_detail.empty()) {
        control.refusal_detail =
            "heap_read_admission_memory_receipt_overflow";
      }
      return invalid(control.refusal_detail, &control);
    }
  }
  {
    const auto current_rows = HeapReadRowVectorMemoryBytes(row_versions);
    const auto admitted_rows =
        HeapReadRowVectorMemoryBytes(admitted_versions);
    const auto chain_index =
        HeapReadVersionIndexProjectionMemoryBytes(admitted_versions);
    std::uint64_t chain_phase_memory = authority_memory;
    if (!current_rows.has_value() || !admitted_rows.has_value() ||
        !chain_index.has_value() ||
        !CheckedHeapReadMemoryAdd(*savepoint_memory,
                                  &chain_phase_memory) ||
        !CheckedHeapReadMemoryAdd(*current_rows, &chain_phase_memory) ||
        !CheckedHeapReadMemoryAdd(*admitted_rows, &chain_phase_memory) ||
        !CheckedHeapReadMemoryAdd(*chain_index, &chain_phase_memory) ||
        !CheckedHeapReadMemoryAdd(512, &chain_phase_memory) ||
        !ObserveBoundedHeapReadMemory(&control, chain_phase_memory)) {
      if (control.refusal_detail.empty()) {
        control.refusal_detail =
            "heap_read_chain_validation_memory_receipt_overflow";
      }
      return invalid(control.refusal_detail, &control);
    }
    const auto chain_status =
        ValidateMgaRowVersionRecordChainsForStoreModule(admitted_versions);
    if (chain_status.error) {
      return refuse(chain_status, &control,
                    MgaHeapReadFailureCategoryV1::kCorruptStorage);
    }
  }
  if (RowsContainLargeValueLocators(admitted_versions)) {
    return invalid("large_value_outside_bounded_inline_heap_profile",
                   &control, MgaHeapReadFailureCategoryV1::kCorruptStorage);
  }

  std::unordered_map<std::string, std::size_t> newest_visible_by_row;
  {
    const auto current_rows = HeapReadRowVectorMemoryBytes(row_versions);
    const auto admitted_rows =
        HeapReadRowVectorMemoryBytes(admitted_versions);
    const auto visibility_projection =
        HeapReadVisibilityMapProjectionMemoryBytes(admitted_versions);
    std::uint64_t phase_memory = authority_memory;
    if (!current_rows.has_value() || !admitted_rows.has_value() ||
        !visibility_projection.has_value() ||
        !CheckedHeapReadMemoryAdd(*savepoint_memory, &phase_memory) ||
        !CheckedHeapReadMemoryAdd(*current_rows, &phase_memory) ||
        !CheckedHeapReadMemoryAdd(*admitted_rows, &phase_memory) ||
        !CheckedHeapReadMemoryAdd(*visibility_projection, &phase_memory) ||
        !ObserveBoundedHeapReadMemory(&control, phase_memory)) {
      if (control.refusal_detail.empty()) {
        control.refusal_detail =
            "heap_read_visibility_map_memory_receipt_overflow";
      }
      return invalid(control.refusal_detail, &control);
    }
  }
  newest_visible_by_row.reserve(admitted_versions.size());
  for (std::size_t index = 0; index < admitted_versions.size(); ++index) {
    if (cancellation_requested()) {
      control.cancellation_observed = true;
      control.refusal_detail = "heap_read_cancelled_during_visibility";
      return invalid(control.refusal_detail, &control);
    }
    const auto& row = admitted_versions[index];
    ++result.visibility_recheck_count;
    if ((!row.temporary_session_uuid.empty() &&
         row.temporary_session_uuid != context.session_uuid.canonical) ||
        !creator_visible(row.creator_tx)) {
      ++result.invisible_row_version_count;
      continue;
    }
    const auto found = newest_visible_by_row.find(row.row_uuid);
    if (found == newest_visible_by_row.end() ||
        admitted_versions[found->second].sequence < row.sequence) {
      newest_visible_by_row[row.row_uuid] = index;
    }
  }
  {
    const auto current_rows = HeapReadRowVectorMemoryBytes(row_versions);
    const auto admitted_rows =
        HeapReadRowVectorMemoryBytes(admitted_versions);
    const auto visibility_map =
        HeapReadVisibilityMapMemoryBytes(newest_visible_by_row);
    std::uint64_t phase_memory = authority_memory;
    if (!current_rows.has_value() || !admitted_rows.has_value() ||
        !visibility_map.has_value() ||
        !CheckedHeapReadMemoryAdd(*savepoint_memory, &phase_memory) ||
        !CheckedHeapReadMemoryAdd(*current_rows, &phase_memory) ||
        !CheckedHeapReadMemoryAdd(*admitted_rows, &phase_memory) ||
        !CheckedHeapReadMemoryAdd(*visibility_map, &phase_memory) ||
        !ObserveBoundedHeapReadMemory(&control, phase_memory)) {
      if (control.refusal_detail.empty()) {
        control.refusal_detail =
            "heap_read_visibility_map_memory_receipt_overflow";
      }
      return invalid(control.refusal_detail, &control);
    }
  }

  std::vector<CrudRowVersionRecord> visible_rows;
  std::uint64_t visible_row_count = 0;
  std::uint64_t visible_projection_bytes = sizeof(visible_rows);
  for (std::size_t index = 0; index < admitted_versions.size(); ++index) {
    const auto& row = admitted_versions[index];
    const auto newest = newest_visible_by_row.find(row.row_uuid);
    if (newest == newest_visible_by_row.end() || newest->second != index ||
        row.deleted) {
      continue;
    }
    if (visible_row_count >= request.maximum_output_rows ||
        !AccountHeapReadRowDynamicMemoryBytes(
            row, &visible_projection_bytes)) {
      return invalid(visible_row_count >= request.maximum_output_rows
                         ? "heap_read_maximum_output_rows_exceeded"
                         : "heap_read_publication_memory_receipt_overflow",
                     &control);
    }
    ++visible_row_count;
  }
  std::uint64_t visible_structural_bytes = 0;
  if (!CheckedHeapReadMemoryMultiply(visible_row_count,
                                     sizeof(CrudRowVersionRecord),
                                     &visible_structural_bytes) ||
      !CheckedHeapReadMemoryAdd(visible_structural_bytes,
                                &visible_projection_bytes)) {
    control.refusal_detail = "heap_read_publication_memory_receipt_overflow";
    return invalid(control.refusal_detail, &control);
  }
  {
    const auto current_rows = HeapReadRowVectorMemoryBytes(row_versions);
    const auto admitted_rows =
        HeapReadRowVectorMemoryBytes(admitted_versions);
    const auto visibility_map =
        HeapReadVisibilityMapMemoryBytes(newest_visible_by_row);
    std::uint64_t phase_memory = authority_memory;
    if (!current_rows.has_value() || !admitted_rows.has_value() ||
        !visibility_map.has_value() ||
        !CheckedHeapReadMemoryAdd(*savepoint_memory, &phase_memory) ||
        !CheckedHeapReadMemoryAdd(*current_rows, &phase_memory) ||
        !CheckedHeapReadMemoryAdd(*admitted_rows, &phase_memory) ||
        !CheckedHeapReadMemoryAdd(*visibility_map, &phase_memory) ||
        !CheckedHeapReadMemoryAdd(visible_projection_bytes,
                                  &phase_memory) ||
        !ObserveBoundedHeapReadMemory(&control, phase_memory)) {
      if (control.refusal_detail.empty()) {
        control.refusal_detail =
            "heap_read_publication_memory_receipt_overflow";
      }
      return invalid(control.refusal_detail, &control);
    }
  }
  visible_rows.reserve(static_cast<std::size_t>(visible_row_count));
  for (std::size_t index = 0; index < admitted_versions.size(); ++index) {
    if (cancellation_requested()) {
      control.cancellation_observed = true;
      control.refusal_detail = "heap_read_cancelled_during_publication";
      return invalid(control.refusal_detail, &control);
    }
    const auto& row = admitted_versions[index];
    const auto newest = newest_visible_by_row.find(row.row_uuid);
    if (newest == newest_visible_by_row.end() || newest->second != index) {
      continue;
    }
    if (row.deleted) {
      ++result.tombstone_row_count;
      continue;
    }
    if (visible_rows.size() >= request.maximum_output_rows) {
      return invalid("heap_read_maximum_output_rows_exceeded", &control);
    }
    visible_rows.push_back(row);
  }
  {
    const auto current_rows = HeapReadRowVectorMemoryBytes(row_versions);
    const auto admitted_rows =
        HeapReadRowVectorMemoryBytes(admitted_versions);
    const auto visibility_map =
        HeapReadVisibilityMapMemoryBytes(newest_visible_by_row);
    const auto published_rows =
        HeapReadRowVectorMemoryBytes(visible_rows);
    std::uint64_t phase_memory = authority_memory;
    if (!current_rows.has_value() || !admitted_rows.has_value() ||
        !visibility_map.has_value() || !published_rows.has_value() ||
        !CheckedHeapReadMemoryAdd(*savepoint_memory, &phase_memory) ||
        !CheckedHeapReadMemoryAdd(*current_rows, &phase_memory) ||
        !CheckedHeapReadMemoryAdd(*admitted_rows, &phase_memory) ||
        !CheckedHeapReadMemoryAdd(*visibility_map, &phase_memory) ||
        !CheckedHeapReadMemoryAdd(*published_rows, &phase_memory) ||
        !ObserveBoundedHeapReadMemory(&control, phase_memory)) {
      if (control.refusal_detail.empty()) {
        control.refusal_detail =
            "heap_read_publication_memory_receipt_overflow";
      }
      return invalid(control.refusal_detail, &control);
    }
  }

  {
    const auto current_rows = HeapReadRowVectorMemoryBytes(row_versions);
    const auto admitted_rows =
        HeapReadRowVectorMemoryBytes(admitted_versions);
    const auto visibility_map =
        HeapReadVisibilityMapMemoryBytes(newest_visible_by_row);
    const auto published_rows = HeapReadRowVectorMemoryBytes(visible_rows);
    std::uint64_t publication_phase_memory = authority_memory;
    constexpr std::uint64_t kResultPublicationEnvelopeBytes =
        6 * sizeof(EngineEvidenceReference) + 2048;
    if (!current_rows.has_value() || !admitted_rows.has_value() ||
        !visibility_map.has_value() || !published_rows.has_value() ||
        !CheckedHeapReadMemoryAdd(*savepoint_memory,
                                  &publication_phase_memory) ||
        !CheckedHeapReadMemoryAdd(*current_rows,
                                  &publication_phase_memory) ||
        !CheckedHeapReadMemoryAdd(*admitted_rows,
                                  &publication_phase_memory) ||
        !CheckedHeapReadMemoryAdd(*visibility_map,
                                  &publication_phase_memory) ||
        !CheckedHeapReadMemoryAdd(*published_rows,
                                  &publication_phase_memory) ||
        !CheckedHeapReadMemoryAdd(kResultPublicationEnvelopeBytes,
                                  &publication_phase_memory) ||
        !ObserveBoundedHeapReadMemory(&control,
                                      publication_phase_memory)) {
      if (control.refusal_detail.empty()) {
        control.refusal_detail =
            "heap_read_result_memory_receipt_overflow";
      }
      return invalid(control.refusal_detail, &control);
    }
  }
  result.evidence.reserve(6);
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  result.visible_rows = std::move(visible_rows);
  result.current_relation_base_generation = current_relation_base_generation;
  result.evidence.push_back(
      {"mga_heap_read_relation_descriptor_uuid",
       descriptor.descriptor_uuid.canonical});
  result.evidence.push_back(
      {"mga_heap_read_relation_descriptor_generation",
       std::to_string(descriptor.descriptor_generation)});
  result.evidence.push_back(
      {"mga_heap_read_relation_base_generation",
       std::to_string(result.current_relation_base_generation)});
  result.evidence.push_back(
      {"mga_heap_read_storage_route", "bounded_scoped_physical_segment"});
  result.evidence.push_back(
      {"mga_heap_read_visibility_authority",
       "durable_transaction_inventory_statement_snapshot"});
  result.evidence.push_back(
      {"mga_heap_read_parser_or_candidate_authority", "false"});
  const auto current_rows =
      HeapReadRowVectorMemoryBytes(result.visible_rows);
  std::uint64_t evidence_bytes = 0;
  std::uint64_t evidence_allocation_bytes = 0;
  if (!current_rows.has_value() ||
      !CheckedHeapReadMemoryMultiply(
          static_cast<std::uint64_t>(result.evidence.capacity()),
          sizeof(EngineEvidenceReference), &evidence_allocation_bytes) ||
      !CheckedHeapReadMemoryAdd(evidence_allocation_bytes,
                                &evidence_bytes)) {
    control.refusal_detail = "heap_read_result_memory_receipt_overflow";
    return invalid(control.refusal_detail, &control);
  }
  for (const auto& evidence : result.evidence) {
    if (!AccountHeapReadOwnedString(evidence.evidence_kind,
                                    &evidence_bytes) ||
        !AccountHeapReadOwnedString(evidence.evidence_id,
                                    &evidence_bytes)) {
      control.refusal_detail = "heap_read_result_memory_receipt_overflow";
      return invalid(control.refusal_detail, &control);
    }
  }
  std::uint64_t current_memory = sizeof(result);
  if (!CheckedHeapReadMemoryAdd(*descriptor_memory, &current_memory) ||
      !CheckedHeapReadMemoryAdd(*current_rows, &current_memory) ||
      !CheckedHeapReadMemoryAdd(evidence_bytes, &current_memory)) {
    control.refusal_detail = "heap_read_result_memory_receipt_overflow";
    return invalid(control.refusal_detail, &control);
  }
  const auto retained_rows = HeapReadRowVectorMemoryBytes(row_versions);
  const auto admitted_rows =
      HeapReadRowVectorMemoryBytes(admitted_versions);
  const auto visibility_map =
      HeapReadVisibilityMapMemoryBytes(newest_visible_by_row);
  std::uint64_t final_phase_memory = authority_memory;
  if (!retained_rows.has_value() || !admitted_rows.has_value() ||
      !visibility_map.has_value() ||
      !CheckedHeapReadMemoryAdd(*savepoint_memory, &final_phase_memory) ||
      !CheckedHeapReadMemoryAdd(*retained_rows, &final_phase_memory) ||
      !CheckedHeapReadMemoryAdd(*admitted_rows, &final_phase_memory) ||
      !CheckedHeapReadMemoryAdd(*visibility_map, &final_phase_memory) ||
      !CheckedHeapReadMemoryAdd(*current_rows, &final_phase_memory) ||
      !CheckedHeapReadMemoryAdd(evidence_bytes, &final_phase_memory) ||
      !ObserveBoundedHeapReadMemory(&control, final_phase_memory)) {
    if (control.refusal_detail.empty()) {
      control.refusal_detail = "heap_read_final_memory_receipt_overflow";
    }
    return invalid(control.refusal_detail, &control);
  }
  result.current_live_memory_bytes = current_memory;
  result.peak_live_memory_bytes = control.peak_live_memory_bytes;
  result.memory_grant_bytes = request.maximum_memory_bytes;
  result.memory_receipt_complete =
      request.maximum_memory_bytes != 0 &&
      result.current_live_memory_bytes <= result.peak_live_memory_bytes &&
      result.peak_live_memory_bytes <= result.memory_grant_bytes;
  if (request.maximum_memory_bytes != 0 &&
      !result.memory_receipt_complete) {
    control.refusal_detail = "heap_read_memory_receipt_incomplete";
    return invalid(control.refusal_detail, &control);
  }
  return result;
}

// QOW-SOURCE-QRY-004-HEAP-MGA-V1
MgaVisibleHeapRelationReadResult ReadVisibleMgaHeapRelation(
    const EngineRequestContext& context,
    const MgaVisibleHeapRelationReadRequest& request) {
  return ReadVisibleMgaHeapRelationWithObservation(context, request, nullptr);
}

namespace {

constexpr std::uint32_t kStreamingCountNoFallback =
    std::numeric_limits<std::uint32_t>::max();
constexpr std::uint64_t kStreamingCountScratchBytes = 128 * 1024;
constexpr std::uint32_t kStreamingCountMaximumMetadataStringBytes =
    64 * 1024;

struct StreamingVisibleSelection {
  std::vector<std::uint8_t> visible_source_ordinals;
  std::string text_path;
  std::string binary_path;
  std::uint64_t text_bytes = 0;
  std::uint64_t binary_bytes = 0;
};

struct StreamingCountIdentity {
  std::array<std::uint8_t, 16> canonical_bytes{};
  std::uint32_t fallback_ordinal = kStreamingCountNoFallback;
};

struct StreamingCountRowVersion {
  StreamingCountIdentity row_uuid;
  StreamingCountIdentity version_uuid;
  StreamingCountIdentity previous_version_uuid;
  std::uint64_t creator_tx = 0;
  std::uint64_t event_sequence = 0;
  std::uint64_t previous_sequence = 0;
  std::uint64_t source_ordinal = 0;
  bool has_previous_version = false;
  bool deleted = false;
  bool temporary_session_visible = false;
  bool creator_visible = false;
};

bool StreamingCountIdentityLess(
    const StreamingCountIdentity& left,
    const StreamingCountIdentity& right,
    const std::vector<std::string>& fallbacks) {
  const bool left_canonical =
      left.fallback_ordinal == kStreamingCountNoFallback;
  const bool right_canonical =
      right.fallback_ordinal == kStreamingCountNoFallback;
  if (left_canonical != right_canonical) return left_canonical;
  if (left_canonical) {
    return std::lexicographical_compare(
        left.canonical_bytes.begin(), left.canonical_bytes.end(),
        right.canonical_bytes.begin(), right.canonical_bytes.end());
  }
  if (left.fallback_ordinal >= fallbacks.size() ||
      right.fallback_ordinal >= fallbacks.size()) {
    return left.fallback_ordinal < right.fallback_ordinal;
  }
  return fallbacks[left.fallback_ordinal] <
         fallbacks[right.fallback_ordinal];
}

bool StreamingCountIdentityEqual(
    const StreamingCountIdentity& left,
    const StreamingCountIdentity& right,
    const std::vector<std::string>& fallbacks) {
  return !StreamingCountIdentityLess(left, right, fallbacks) &&
         !StreamingCountIdentityLess(right, left, fallbacks);
}

std::optional<std::uint64_t> StreamingCountMetadataMemoryBytes(
    const std::vector<StreamingCountRowVersion>& rows,
    const std::vector<std::string>& fallback_identities,
    const SavepointParsedState& savepoints,
    const MgaRelationStorageDescriptor& descriptor,
    const std::uint64_t transient_string_bytes = 0) {
  std::uint64_t bytes = sizeof(MgaVisibleHeapRelationCountResult) +
                        sizeof(rows) + sizeof(fallback_identities) +
                        kStreamingCountScratchBytes;
  std::uint64_t allocation_bytes = 0;
  const auto descriptor_bytes =
      HeapReadStorageDescriptorMemoryBytes(descriptor);
  const auto savepoint_bytes = HeapReadSavepointMemoryBytes(savepoints);
  if (!descriptor_bytes.has_value() || !savepoint_bytes.has_value() ||
      !CheckedHeapReadMemoryMultiply(
          static_cast<std::uint64_t>(rows.capacity()),
          sizeof(StreamingCountRowVersion), &allocation_bytes) ||
      !CheckedHeapReadMemoryAdd(allocation_bytes, &bytes) ||
      !CheckedHeapReadMemoryMultiply(
          static_cast<std::uint64_t>(fallback_identities.capacity()),
          sizeof(std::string), &allocation_bytes) ||
      !CheckedHeapReadMemoryAdd(allocation_bytes, &bytes) ||
      !CheckedHeapReadMemoryAdd(*descriptor_bytes, &bytes) ||
      !CheckedHeapReadMemoryAdd(*savepoint_bytes, &bytes) ||
      !CheckedHeapReadMemoryAdd(transient_string_bytes, &bytes)) {
    return std::nullopt;
  }
  for (const auto& identity : fallback_identities) {
    if (!AccountHeapReadOwnedString(identity, &bytes)) return std::nullopt;
  }
  return bytes;
}

bool ObserveStreamingCountMemory(
    const std::vector<StreamingCountRowVersion>& rows,
    const std::vector<std::string>& fallback_identities,
    const SavepointParsedState& savepoints,
    const MgaRelationStorageDescriptor& descriptor,
    const std::uint64_t maximum_memory_bytes,
    std::uint64_t* peak_memory_bytes,
    std::string* detail,
    const std::uint64_t transient_string_bytes = 0) {
  const auto live = StreamingCountMetadataMemoryBytes(
      rows, fallback_identities, savepoints, descriptor,
      transient_string_bytes);
  if (!live.has_value()) {
    if (detail != nullptr) {
      *detail = "heap_count_memory_receipt_overflow";
    }
    return false;
  }
  if (peak_memory_bytes != nullptr) {
    *peak_memory_bytes = std::max(*peak_memory_bytes, *live);
  }
  if (maximum_memory_bytes == 0 || *live > maximum_memory_bytes) {
    if (detail != nullptr) {
      *detail = "heap_count_maximum_memory_bytes_exceeded";
    }
    return false;
  }
  return true;
}

bool ReserveStreamingCountRows(
    const std::uint64_t additional_rows,
    std::vector<StreamingCountRowVersion>* rows,
    const std::vector<std::string>& fallback_identities,
    const SavepointParsedState& savepoints,
    const MgaRelationStorageDescriptor& descriptor,
    const std::uint64_t maximum_memory_bytes,
    std::uint64_t* peak_memory_bytes,
    std::string* detail) {
  if (rows == nullptr ||
      additional_rows > std::numeric_limits<std::size_t>::max() ||
      static_cast<std::size_t>(additional_rows) >
          std::numeric_limits<std::size_t>::max() - rows->size()) {
    if (detail != nullptr) *detail = "heap_count_row_metadata_overflow";
    return false;
  }
  const auto required = rows->size() +
                        static_cast<std::size_t>(additional_rows);
  if (required > rows->capacity()) {
    std::uint64_t projected_bytes = 0;
    std::uint64_t structural_bytes = 0;
    const auto descriptor_bytes =
        HeapReadStorageDescriptorMemoryBytes(descriptor);
    const auto savepoint_bytes = HeapReadSavepointMemoryBytes(savepoints);
    if (!descriptor_bytes.has_value() || !savepoint_bytes.has_value() ||
        !CheckedHeapReadMemoryMultiply(required,
                                       sizeof(StreamingCountRowVersion),
                                       &structural_bytes) ||
        !CheckedHeapReadMemoryAdd(
            sizeof(MgaVisibleHeapRelationCountResult) + sizeof(*rows) +
                sizeof(fallback_identities) + kStreamingCountScratchBytes,
            &projected_bytes) ||
        !CheckedHeapReadMemoryAdd(structural_bytes, &projected_bytes) ||
        !CheckedHeapReadMemoryAdd(*descriptor_bytes, &projected_bytes) ||
        !CheckedHeapReadMemoryAdd(*savepoint_bytes, &projected_bytes)) {
      if (detail != nullptr) *detail = "heap_count_memory_receipt_overflow";
      return false;
    }
    for (const auto& identity : fallback_identities) {
      if (!AccountHeapReadOwnedString(identity, &projected_bytes)) {
        if (detail != nullptr) *detail = "heap_count_memory_receipt_overflow";
        return false;
      }
    }
    if (projected_bytes > maximum_memory_bytes) {
      if (detail != nullptr) {
        *detail = "heap_count_maximum_memory_bytes_exceeded";
      }
      return false;
    }
    rows->reserve(required);
  }
  return ObserveStreamingCountMemory(
      *rows, fallback_identities, savepoints, descriptor,
      maximum_memory_bytes, peak_memory_bytes, detail);
}

bool ParseStreamingCountIdentity(
    const std::string_view text,
    std::vector<std::string>* fallback_identities,
    StreamingCountIdentity* identity,
    const bool allow_empty,
    const SavepointParsedState& savepoints,
    const MgaRelationStorageDescriptor& descriptor,
    const std::vector<StreamingCountRowVersion>& rows,
    const std::uint64_t maximum_memory_bytes,
    std::uint64_t* peak_memory_bytes,
    std::string* detail) {
  if (fallback_identities == nullptr || identity == nullptr ||
      (!allow_empty && text.empty())) {
    if (detail != nullptr) *detail = "heap_count_row_identity_invalid";
    return false;
  }
  if (text.empty()) {
    *identity = {};
    return true;
  }
  const auto parsed = scratchbird::core::uuid::ParseUuid(std::string(text));
  if (parsed.ok()) {
    std::copy(parsed.value.bytes.begin(), parsed.value.bytes.end(),
              identity->canonical_bytes.begin());
    identity->fallback_ordinal = kStreamingCountNoFallback;
    return true;
  }
  if (fallback_identities->size() >= kStreamingCountNoFallback ||
      text.size() > kStreamingCountMaximumMetadataStringBytes) {
    if (detail != nullptr) *detail = "heap_count_row_identity_invalid";
    return false;
  }
  const std::uint64_t transient =
      static_cast<std::uint64_t>(text.size()) + 1 + sizeof(std::string);
  if (!ObserveStreamingCountMemory(
          rows, *fallback_identities, savepoints, descriptor,
          maximum_memory_bytes, peak_memory_bytes, detail, transient)) {
    return false;
  }
  fallback_identities->emplace_back(text);
  identity->fallback_ordinal = static_cast<std::uint32_t>(
      fallback_identities->size() - 1);
  return ObserveStreamingCountMemory(
      rows, *fallback_identities, savepoints, descriptor,
      maximum_memory_bytes, peak_memory_bytes, detail);
}

class StreamingCountBinaryReader {
 public:
  StreamingCountBinaryReader(
      std::string path, const std::uint64_t authorized_bytes,
      const std::uint64_t maximum_decoded_bytes,
      const std::function<bool()>* cancellation_requested,
      MgaVisibleHeapRelationCountResult* result,
      HeapReadRuntimeObservation* runtime_observation,
      std::string* detail)
      : path_(std::move(path)),
        remaining_(authorized_bytes),
        physical_remaining_(authorized_bytes),
        maximum_decoded_bytes_(maximum_decoded_bytes),
        cancellation_requested_(cancellation_requested),
        result_(result),
        runtime_observation_(runtime_observation),
        detail_(detail) {}

  bool Open() {
    const auto started = std::chrono::steady_clock::now();
    input_.open(path_, std::ios::binary);
    ObserveWait(started);
    if (!input_) return Fail("heap_count_scoped_binary_open_failed");
    return true;
  }

  bool ReadU8(std::uint8_t* value) {
    return ReadExact(reinterpret_cast<char*>(value), 1);
  }

  bool ReadU16(std::uint16_t* value) {
    std::array<std::uint8_t, 2> bytes{};
    if (!ReadExact(reinterpret_cast<char*>(bytes.data()), bytes.size())) {
      return false;
    }
    *value = static_cast<std::uint16_t>(bytes[0]) |
             (static_cast<std::uint16_t>(bytes[1]) << 8U);
    return true;
  }

  bool ReadU32(std::uint32_t* value) {
    std::array<std::uint8_t, 4> bytes{};
    if (!ReadExact(reinterpret_cast<char*>(bytes.data()), bytes.size())) {
      return false;
    }
    *value = 0;
    for (std::size_t index = 0; index < bytes.size(); ++index) {
      *value |= static_cast<std::uint32_t>(bytes[index]) << (index * 8U);
    }
    return true;
  }

  bool ReadU64(std::uint64_t* value) {
    std::array<std::uint8_t, 8> bytes{};
    if (!ReadExact(reinterpret_cast<char*>(bytes.data()), bytes.size())) {
      return false;
    }
    *value = 0;
    for (std::size_t index = 0; index < bytes.size(); ++index) {
      *value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8U);
    }
    return true;
  }

  bool ReadUuid(StreamingCountIdentity* identity) {
    if (identity == nullptr) return Fail("heap_count_row_identity_invalid");
    identity->fallback_ordinal = kStreamingCountNoFallback;
    return ReadExact(
        reinterpret_cast<char*>(identity->canonical_bytes.data()),
        identity->canonical_bytes.size());
  }

  bool ReadUuidText(std::string* value) {
    if (value == nullptr) return Fail("heap_stream_row_identity_invalid");
    scratchbird::core::platform::Uuid uuid;
    if (!ReadExact(reinterpret_cast<char*>(uuid.bytes.data()),
                   uuid.bytes.size())) {
      return false;
    }
    *value = scratchbird::core::uuid::UuidToString(uuid);
    return true;
  }

  bool ReadString(std::string* value, const bool allow_empty) {
    std::uint32_t size = 0;
    if (value == nullptr || !ReadU32(&size) || size > remaining_ ||
        size > kStreamingCountMaximumMetadataStringBytes ||
        (!allow_empty && size == 0)) {
      return Fail("heap_count_binary_string_invalid");
    }
    value->resize(size);
    return size == 0 || ReadExact(value->data(), size);
  }

  bool SkipString(const bool allow_empty) {
    std::uint32_t size = 0;
    return ReadU32(&size) && size <= remaining_ &&
           size <= kStreamingCountMaximumMetadataStringBytes &&
           (allow_empty || size != 0) && Skip(size);
  }

  bool SkipPayloadString() {
    std::uint32_t size = 0;
    return ReadU32(&size) && size <= remaining_ && Skip(size);
  }

  bool ReadPayloadString(std::string* value,
                         const std::uint64_t maximum_payload_bytes) {
    std::uint32_t size = 0;
    if (value == nullptr || !ReadU32(&size) || size > remaining_ ||
        size > maximum_payload_bytes) {
      return Fail("heap_stream_binary_payload_invalid");
    }
    value->resize(size);
    return size == 0 || ReadExact(value->data(), size);
  }

  bool ReadFixedPayload(std::string* value,
                        const std::size_t size,
                        const std::uint64_t maximum_payload_bytes) {
    if (value == nullptr || size > maximum_payload_bytes ||
        size > remaining_) {
      return Fail("heap_stream_binary_payload_invalid");
    }
    value->resize(size);
    return size == 0 || ReadExact(value->data(), size);
  }

  bool Skip(const std::uint64_t bytes) {
    if (bytes > remaining_) return Fail("heap_count_binary_truncated");
    std::array<char, 64 * 1024> scratch{};
    std::uint64_t remaining = bytes;
    while (remaining != 0) {
      const auto chunk = static_cast<std::size_t>(
          std::min<std::uint64_t>(remaining, scratch.size()));
      if (!ReadExact(scratch.data(), chunk)) return false;
      remaining -= chunk;
    }
    return true;
  }

  bool Finish() {
    if (remaining_ != 0) return Fail("heap_count_binary_trailing_bytes");
    const auto started = std::chrono::steady_clock::now();
    const auto next = input_.peek();
    ObserveWait(started);
    if (next != std::char_traits<char>::eof()) {
      return Fail("heap_count_scoped_binary_grew_during_read");
    }
    return !input_.bad() || Fail("heap_count_scoped_binary_read_failed");
  }

  [[nodiscard]] std::uint64_t remaining() const { return remaining_; }
  [[nodiscard]] bool cancellation_observed() const {
    return cancellation_observed_;
  }

 private:
  bool ReadExact(char* destination, const std::size_t bytes) {
    if (destination == nullptr || bytes > remaining_) {
      return Fail("heap_count_binary_truncated");
    }
    std::size_t copied = 0;
    while (copied != bytes) {
      if (buffer_offset_ == buffer_size_ && !FillBuffer()) return false;
      const auto available = buffer_size_ - buffer_offset_;
      const auto chunk = std::min(available, bytes - copied);
      std::memcpy(destination + copied, buffer_.data() + buffer_offset_,
                  chunk);
      buffer_offset_ += chunk;
      copied += chunk;
      remaining_ -= chunk;
      if (result_ == nullptr ||
          result_->decoded_byte_count >
              std::numeric_limits<std::uint64_t>::max() - chunk) {
        return Fail("heap_count_byte_counter_overflow");
      }
      result_->decoded_byte_count += chunk;
      if (result_->decoded_byte_count > maximum_decoded_bytes_) {
        return Fail("heap_count_maximum_decoded_bytes_exceeded");
      }
    }
    return true;
  }

  bool FillBuffer() {
    if (physical_remaining_ == 0) {
      return Fail("heap_count_binary_truncated");
    }
    if (Cancelled()) return false;
    const auto chunk = static_cast<std::size_t>(
        std::min<std::uint64_t>(physical_remaining_, buffer_.size()));
    const auto started = std::chrono::steady_clock::now();
    input_.read(buffer_.data(), static_cast<std::streamsize>(chunk));
    ObserveWait(started);
    if (input_.gcount() != static_cast<std::streamsize>(chunk) ||
        input_.bad()) {
      return Fail("heap_count_scoped_binary_read_failed");
    }
    if (result_ == nullptr ||
        result_->storage_bytes_read >
            std::numeric_limits<std::uint64_t>::max() - chunk) {
      return Fail("heap_count_byte_counter_overflow");
    }
    result_->storage_bytes_read += chunk;
    physical_remaining_ -= chunk;
    buffer_offset_ = 0;
    buffer_size_ = chunk;
    return true;
  }

  bool Cancelled() {
    if (cancellation_requested_ != nullptr && *cancellation_requested_ &&
        (*cancellation_requested_)()) {
      cancellation_observed_ = true;
      if (result_ != nullptr) result_->cancellation_observed = true;
      return Fail("heap_count_cancelled_during_physical_read");
    }
    return false;
  }

  bool Fail(const std::string_view detail) {
    if (detail_ != nullptr && detail_->empty()) detail_->assign(detail);
    return false;
  }

  void ObserveWait(const std::chrono::steady_clock::time_point started) {
    if (runtime_observation_ == nullptr) return;
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - started).count();
    if (elapsed < 0 ||
        static_cast<std::uintmax_t>(elapsed) >
            std::numeric_limits<std::uint64_t>::max() ||
        runtime_observation_->operator_wait_ns >
            std::numeric_limits<std::uint64_t>::max() -
                static_cast<std::uint64_t>(elapsed)) {
      runtime_observation_->complete = false;
      return;
    }
    runtime_observation_->operator_wait_ns +=
        static_cast<std::uint64_t>(elapsed);
  }

  std::string path_;
  std::ifstream input_;
  std::uint64_t remaining_ = 0;
  std::uint64_t physical_remaining_ = 0;
  std::array<char, 64 * 1024> buffer_{};
  std::size_t buffer_offset_ = 0;
  std::size_t buffer_size_ = 0;
  std::uint64_t maximum_decoded_bytes_ = 0;
  const std::function<bool()>* cancellation_requested_ = nullptr;
  MgaVisibleHeapRelationCountResult* result_ = nullptr;
  HeapReadRuntimeObservation* runtime_observation_ = nullptr;
  std::string* detail_ = nullptr;
  bool cancellation_observed_ = false;
};

bool StreamingCountCreatorVisible(
    const std::uint64_t creator,
    const scratchbird::transaction::mga::SnapshotVectorDescriptor& snapshot,
    const std::map<std::uint64_t, std::string>& transaction_states) {
  if (creator == 0) return false;
  const auto transaction = transaction_states.find(creator);
  if (transaction == transaction_states.end()) return false;
  if (creator == snapshot.owning_transaction.value) {
    return transaction->second == "active" ||
           transaction->second == "preparing" ||
           transaction->second == "prepared";
  }
  if (transaction->second != "committed" &&
      transaction->second != "archived") {
    return false;
  }
  if (snapshot.visible_committed_high_watermark == 0 ||
      creator > snapshot.visible_committed_high_watermark) {
    return false;
  }
  return !std::binary_search(
             snapshot.active_excluded_local_transaction_ids.begin(),
             snapshot.active_excluded_local_transaction_ids.end(), creator) &&
         !std::binary_search(
             snapshot.in_doubt_excluded_local_transaction_ids.begin(),
             snapshot.in_doubt_excluded_local_transaction_ids.end(), creator);
}

bool AppendStreamingCountRow(
    StreamingCountRowVersion row,
    const std::string_view table_uuid,
    const std::string_view required_relation_uuid,
    const SavepointParsedState& savepoints,
    const MgaRelationStorageDescriptor& descriptor,
    std::vector<StreamingCountRowVersion>* rows,
    const std::vector<std::string>& fallback_identities,
    const std::uint64_t maximum_memory_bytes,
    std::uint64_t* peak_memory_bytes,
    MgaVisibleHeapRelationCountResult* result,
    std::string* detail) {
  if (rows == nullptr || result == nullptr ||
      table_uuid != required_relation_uuid) {
    if (detail != nullptr) {
      *detail = "scoped_heap_row_relation_identity_mismatch";
    }
    return false;
  }
  if (result->scanned_row_version_count ==
      std::numeric_limits<std::uint64_t>::max()) {
    if (detail != nullptr) *detail = "heap_count_row_counter_overflow";
    return false;
  }
  ++result->scanned_row_version_count;
  row.source_ordinal = result->scanned_row_version_count;
  if (RowEventRolledBackBySavepoint(savepoints, row.creator_tx,
                                    row.event_sequence)) {
    if (result->invisible_row_version_count ==
        std::numeric_limits<std::uint64_t>::max()) {
      if (detail != nullptr) *detail = "heap_count_row_counter_overflow";
      return false;
    }
    ++result->invisible_row_version_count;
    return true;
  }
  result->current_relation_base_generation = std::max(
      result->current_relation_base_generation, row.event_sequence);
  if (!ReserveStreamingCountRows(
          1, rows, fallback_identities, savepoints, descriptor,
          maximum_memory_bytes, peak_memory_bytes, detail)) {
    return false;
  }
  rows->push_back(std::move(row));
  return ObserveStreamingCountMemory(
      *rows, fallback_identities, savepoints, descriptor,
      maximum_memory_bytes, peak_memory_bytes, detail);
}

bool DecodeStreamingCountBinaryFile(
    const std::string& path,
    const std::uint64_t authorized_file_bytes,
    const std::string& relation_uuid,
    const std::string& session_uuid,
    const scratchbird::transaction::mga::SnapshotVectorDescriptor& snapshot,
    const std::map<std::uint64_t, std::string>& transaction_states,
    const SavepointParsedState& savepoints,
    const MgaRelationStorageDescriptor& descriptor,
    const std::uint64_t maximum_decoded_bytes,
    const std::uint64_t maximum_memory_bytes,
    const std::function<bool()>* cancellation_requested,
    std::vector<StreamingCountRowVersion>* rows,
    std::vector<std::string>* fallback_identities,
    std::uint64_t* peak_memory_bytes,
    MgaVisibleHeapRelationCountResult* result,
    HeapReadRuntimeObservation* runtime_observation,
    std::string* detail) {
  if (rows == nullptr || fallback_identities == nullptr || result == nullptr ||
      detail == nullptr) {
    return false;
  }
  StreamingCountBinaryReader reader(
      path, authorized_file_bytes, maximum_decoded_bytes,
      cancellation_requested, result, runtime_observation, detail);
  if (!reader.Open()) return false;
  while (reader.remaining() != 0) {
    std::array<char, 8> magic{};
    std::uint16_t version = 0;
    std::uint16_t flags = 0;
    std::uint32_t column_count = 0;
    std::uint64_t row_count = 0;
    if (!reader.Skip(0) ||
        !reader.ReadU8(reinterpret_cast<std::uint8_t*>(&magic[0])) ||
        !reader.ReadU8(reinterpret_cast<std::uint8_t*>(&magic[1])) ||
        !reader.ReadU8(reinterpret_cast<std::uint8_t*>(&magic[2])) ||
        !reader.ReadU8(reinterpret_cast<std::uint8_t*>(&magic[3])) ||
        !reader.ReadU8(reinterpret_cast<std::uint8_t*>(&magic[4])) ||
        !reader.ReadU8(reinterpret_cast<std::uint8_t*>(&magic[5])) ||
        !reader.ReadU8(reinterpret_cast<std::uint8_t*>(&magic[6])) ||
        !reader.ReadU8(reinterpret_cast<std::uint8_t*>(&magic[7])) ||
        std::string_view(magic.data(), magic.size()) !=
            kScopedRowBinaryBatchMagic ||
        !reader.ReadU16(&version) || !reader.ReadU16(&flags) ||
        !reader.ReadU32(&column_count) || !reader.ReadU64(&row_count) ||
        (version != 1 && version != kScopedRowBinaryLegacyTypedVersion &&
         version != kScopedRowBinaryVersion &&
         version != kScopedRowBinaryNativePacketVersion) ||
        flags != 0 || column_count == 0 || column_count > 4096) {
      if (detail->empty()) *detail = "heap_count_scoped_binary_header_invalid";
      return false;
    }
    for (std::uint32_t column = 0; column < column_count; ++column) {
      if (!reader.SkipString(false)) return false;
    }
    std::vector<std::uint8_t> native_tags;
    if (version == kScopedRowBinaryNativePacketVersion) {
      native_tags.reserve(column_count);
      for (std::uint32_t column = 0; column < column_count; ++column) {
        std::uint8_t tag = 0;
        if (!reader.ReadU8(&tag) ||
            ScopedRowNativePacketTypeName(tag).empty()) {
          if (detail->empty()) {
            *detail = "heap_count_scoped_binary_type_invalid";
          }
          return false;
        }
        native_tags.push_back(tag);
      }
    } else if (version >= kScopedRowBinaryLegacyTypedVersion) {
      for (std::uint32_t column = 0; column < column_count; ++column) {
        if (!reader.SkipString(false)) return false;
      }
    }

    const bool compact_batch = version >= kScopedRowBinaryVersion;
    std::uint64_t compact_first_event_sequence = 0;
    std::uint64_t compact_creator_tx = 0;
    std::string compact_table_uuid;
    std::string compact_temporary_session_uuid;
    std::uint8_t compact_flags = 0;
    if (compact_batch &&
        (!reader.ReadU64(&compact_first_event_sequence) ||
         !reader.ReadU64(&compact_creator_tx) ||
         !reader.ReadString(&compact_table_uuid, false) ||
         !reader.ReadString(&compact_temporary_session_uuid, true) ||
         !reader.ReadU8(&compact_flags) || compact_flags != 0 ||
         compact_table_uuid != relation_uuid ||
         (row_count != 0 &&
          row_count - 1 > std::numeric_limits<std::uint64_t>::max() -
                              compact_first_event_sequence))) {
      if (detail->empty()) {
        *detail = "heap_count_scoped_binary_compact_header_invalid";
      }
      return false;
    }
    const std::uint64_t null_bitmap_bytes =
        (static_cast<std::uint64_t>(column_count) + 7U) / 8U;
    const std::uint64_t minimum_row_bytes =
        (compact_batch ? 32U : 45U) + null_bitmap_bytes;
    if (minimum_row_bytes == 0 ||
        row_count > reader.remaining() / minimum_row_bytes ||
        !ReserveStreamingCountRows(
            row_count, rows, *fallback_identities, savepoints, descriptor,
            maximum_memory_bytes, peak_memory_bytes, detail)) {
      if (detail->empty()) *detail = "heap_count_row_count_exceeds_segment";
      return false;
    }
    std::vector<std::uint8_t> null_bitmap(
        static_cast<std::size_t>(null_bitmap_bytes));
    for (std::uint64_t row_index = 0; row_index < row_count; ++row_index) {
      StreamingCountRowVersion row;
      std::string row_table_uuid;
      std::string row_uuid;
      std::string version_uuid;
      std::string previous_version_uuid;
      std::string temporary_session_uuid;
      std::uint8_t deleted = 0;
      if (compact_batch) {
        row.creator_tx = compact_creator_tx;
        row.event_sequence = compact_first_event_sequence + row_index;
        row_table_uuid = compact_table_uuid;
        row.temporary_session_visible =
            compact_temporary_session_uuid.empty() ||
            compact_temporary_session_uuid == session_uuid;
        if (!reader.ReadUuid(&row.row_uuid) ||
            !reader.ReadUuid(&row.version_uuid)) {
          return false;
        }
      } else {
        if (!reader.ReadU64(&row.creator_tx) ||
            !reader.ReadU64(&row.event_sequence) ||
            !reader.ReadU64(&row.previous_sequence) ||
            !reader.ReadU8(&deleted) ||
            !reader.ReadString(&row_table_uuid, false) ||
            !reader.ReadString(&row_uuid, false) ||
            !reader.ReadString(&version_uuid, false) ||
            !reader.ReadString(&previous_version_uuid, true) ||
            !reader.ReadString(&temporary_session_uuid, true) ||
            !ParseStreamingCountIdentity(
                row_uuid, fallback_identities, &row.row_uuid, false,
                savepoints, descriptor, *rows, maximum_memory_bytes,
                peak_memory_bytes, detail) ||
            !ParseStreamingCountIdentity(
                version_uuid, fallback_identities, &row.version_uuid, false,
                savepoints, descriptor, *rows, maximum_memory_bytes,
                peak_memory_bytes, detail)) {
          return false;
        }
        row.deleted = deleted != 0;
        row.has_previous_version = !previous_version_uuid.empty();
        if (row.has_previous_version &&
            !ParseStreamingCountIdentity(
                previous_version_uuid, fallback_identities,
                &row.previous_version_uuid, false, savepoints, descriptor,
                *rows, maximum_memory_bytes, peak_memory_bytes, detail)) {
          return false;
        }
        row.temporary_session_visible = temporary_session_uuid.empty() ||
                                        temporary_session_uuid == session_uuid;
      }
      row.creator_visible = StreamingCountCreatorVisible(
          row.creator_tx, snapshot, transaction_states);
      for (std::size_t byte = 0; byte < null_bitmap.size(); ++byte) {
        if (!reader.ReadU8(&null_bitmap[byte])) return false;
      }
      for (std::uint32_t column = 0; column < column_count; ++column) {
        const bool is_null =
            (null_bitmap[column / 8U] &
             static_cast<std::uint8_t>(1U << (column % 8U))) != 0;
        if (is_null) continue;
        if (version == kScopedRowBinaryNativePacketVersion) {
          const auto tag = native_tags[column];
          if ((tag == 3 && !reader.Skip(1)) ||
              (tag == 4 && !reader.Skip(4)) ||
              ((tag == 2 || tag == 5 || tag == 6) && !reader.Skip(8)) ||
              ((tag == 1 || tag == 7) && !reader.SkipPayloadString())) {
            return false;
          }
        } else if (!reader.SkipPayloadString()) {
          return false;
        }
      }
      if (!AppendStreamingCountRow(
              std::move(row), row_table_uuid, relation_uuid, savepoints,
              descriptor, rows, *fallback_identities,
              maximum_memory_bytes, peak_memory_bytes, result, detail)) {
        return false;
      }
    }
  }
  if (!reader.Finish()) {
    result->cancellation_observed = reader.cancellation_observed();
    return false;
  }
  return true;
}

std::vector<std::string_view> StreamingCountTextFields(
    const std::string& line) {
  std::vector<std::string_view> fields;
  fields.reserve(12);
  std::size_t begin = 0;
  while (begin <= line.size()) {
    const auto end = line.find('\t', begin);
    fields.emplace_back(line.data() + begin,
                        end == std::string::npos ? line.size() - begin
                                                 : end - begin);
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  return fields;
}

bool DecodeStreamingCountTextFile(
    const std::string& path,
    const std::uint64_t authorized_file_bytes,
    const std::string& relation_uuid,
    const std::string& session_uuid,
    const scratchbird::transaction::mga::SnapshotVectorDescriptor& snapshot,
    const std::map<std::uint64_t, std::string>& transaction_states,
    const SavepointParsedState& savepoints,
    const MgaRelationStorageDescriptor& descriptor,
    const std::uint64_t maximum_decoded_bytes,
    const std::uint64_t maximum_memory_bytes,
    const std::function<bool()>* cancellation_requested,
    std::vector<StreamingCountRowVersion>* rows,
    std::vector<std::string>* fallback_identities,
    std::uint64_t* peak_memory_bytes,
    MgaVisibleHeapRelationCountResult* result,
    HeapReadRuntimeObservation* runtime_observation,
    std::string* detail) {
  if (rows == nullptr || fallback_identities == nullptr || result == nullptr ||
      detail == nullptr) {
    return false;
  }
  const auto open_started = std::chrono::steady_clock::now();
  std::ifstream input(path, std::ios::binary);
  const auto observe_wait = [&](const auto started) {
    if (runtime_observation == nullptr) return;
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - started).count();
    if (elapsed < 0 ||
        static_cast<std::uintmax_t>(elapsed) >
            std::numeric_limits<std::uint64_t>::max() ||
        runtime_observation->operator_wait_ns >
            std::numeric_limits<std::uint64_t>::max() -
                static_cast<std::uint64_t>(elapsed)) {
      runtime_observation->complete = false;
      return;
    }
    runtime_observation->operator_wait_ns +=
        static_cast<std::uint64_t>(elapsed);
  };
  observe_wait(open_started);
  if (!input) {
    *detail = "heap_count_scoped_text_open_failed";
    return false;
  }
  // Retain only fields 0..9 and the optional temporary-session field. Field
  // 10 is the encoded value payload and can be arbitrarily large; COUNT(*)
  // must validate and count row-version metadata without materializing it.
  std::string line_metadata;
  std::uint32_t line_tab_count = 0;
  std::array<char, 64 * 1024> chunk{};
  std::uint64_t remaining = authorized_file_bytes;
  const auto consume_line = [&](const std::string& current) {
    const auto fields = StreamingCountTextFields(current);
    if (fields.size() < 11 || fields[0] != kRowStoreMagic ||
        fields[1] != "ROW_VERSION") {
      return true;
    }
    StreamingCountRowVersion row;
    row.creator_tx = ParseU64(std::string(fields[2]));
    row.event_sequence = ParseU64(std::string(fields[3]));
    row.deleted = fields[7] == "1";
    row.previous_sequence = ParseU64(std::string(fields[9]));
    row.has_previous_version = !fields[8].empty();
    row.temporary_session_visible =
        fields.size() < 12 || fields[11].empty() ||
        fields[11] == session_uuid;
    row.creator_visible = StreamingCountCreatorVisible(
        row.creator_tx, snapshot, transaction_states);
    if (!ParseStreamingCountIdentity(
            fields[5], fallback_identities, &row.row_uuid, false,
            savepoints, descriptor, *rows, maximum_memory_bytes,
            peak_memory_bytes, detail) ||
        !ParseStreamingCountIdentity(
            fields[6], fallback_identities, &row.version_uuid, false,
            savepoints, descriptor, *rows, maximum_memory_bytes,
            peak_memory_bytes, detail) ||
        (row.has_previous_version &&
         !ParseStreamingCountIdentity(
             fields[8], fallback_identities, &row.previous_version_uuid,
             false, savepoints, descriptor, *rows, maximum_memory_bytes,
             peak_memory_bytes, detail))) {
      return false;
    }
    return AppendStreamingCountRow(
        std::move(row), fields[4], relation_uuid, savepoints, descriptor,
        rows, *fallback_identities, maximum_memory_bytes,
        peak_memory_bytes, result, detail);
  };
  while (remaining != 0) {
    if (cancellation_requested != nullptr && *cancellation_requested &&
        (*cancellation_requested)()) {
      result->cancellation_observed = true;
      *detail = "heap_count_cancelled_during_physical_read";
      return false;
    }
    const auto requested = static_cast<std::size_t>(
        std::min<std::uint64_t>(remaining, chunk.size()));
    const auto read_started = std::chrono::steady_clock::now();
    input.read(chunk.data(), static_cast<std::streamsize>(requested));
    observe_wait(read_started);
    if (input.gcount() != static_cast<std::streamsize>(requested) ||
        input.bad()) {
      *detail = "heap_count_scoped_text_read_failed";
      return false;
    }
    remaining -= requested;
    if (result->storage_bytes_read >
            std::numeric_limits<std::uint64_t>::max() - requested ||
        result->decoded_byte_count >
            std::numeric_limits<std::uint64_t>::max() - requested) {
      *detail = "heap_count_byte_counter_overflow";
      return false;
    }
    result->storage_bytes_read += requested;
    result->decoded_byte_count += requested;
    if (result->decoded_byte_count > maximum_decoded_bytes) {
      *detail = "heap_count_maximum_decoded_bytes_exceeded";
      return false;
    }
    for (std::size_t index = 0; index < requested; ++index) {
      const char value = chunk[index];
      if (value == '\n') {
        if (!ObserveStreamingCountMemory(
                *rows, *fallback_identities, savepoints, descriptor,
                maximum_memory_bytes, peak_memory_bytes, detail,
                static_cast<std::uint64_t>(line_metadata.capacity()) + 1) ||
            !consume_line(line_metadata)) {
          return false;
        }
        line_metadata.clear();
        line_tab_count = 0;
        continue;
      }
      const bool payload_byte = line_tab_count == 10 && value != '\t';
      if (payload_byte) continue;
      if (line_metadata.size() >=
          kStreamingCountMaximumMetadataStringBytes) {
        *detail = "heap_count_text_record_exceeds_metadata_bound";
        return false;
      }
      line_metadata.push_back(value);
      if (value == '\t' &&
          line_tab_count != std::numeric_limits<std::uint32_t>::max()) {
        ++line_tab_count;
      }
      if ((index & 4095U) == 0 &&
          !ObserveStreamingCountMemory(
              *rows, *fallback_identities, savepoints, descriptor,
              maximum_memory_bytes, peak_memory_bytes, detail,
              static_cast<std::uint64_t>(line_metadata.capacity()) + 1)) {
        return false;
      }
    }
  }
  if (!line_metadata.empty() && !consume_line(line_metadata)) return false;
  const auto peek_started = std::chrono::steady_clock::now();
  const auto next = input.peek();
  observe_wait(peek_started);
  if (next != std::char_traits<char>::eof()) {
    *detail = "heap_count_scoped_text_grew_during_read";
    return false;
  }
  return !input.bad();
}

bool ValidateAndCountStreamingRows(
    std::vector<StreamingCountRowVersion>* rows,
    const std::vector<std::string>& fallback_identities,
    const std::function<bool()>* cancellation_requested,
    MgaVisibleHeapRelationCountResult* result,
    std::string* detail,
    std::vector<std::uint8_t>* visible_source_ordinals = nullptr) {
  if (rows == nullptr || result == nullptr || detail == nullptr) return false;
  const auto cancelled = [&](const std::string_view phase) {
    if (cancellation_requested != nullptr && *cancellation_requested &&
        (*cancellation_requested)()) {
      result->cancellation_observed = true;
      *detail = "heap_count_cancelled_" + std::string(phase);
      return true;
    }
    return false;
  };
  if (cancelled("before_chain_validation")) return false;
  const auto version_less = [&](const auto& left, const auto& right) {
    if (StreamingCountIdentityLess(left.version_uuid, right.version_uuid,
                                   fallback_identities)) {
      return true;
    }
    if (StreamingCountIdentityLess(right.version_uuid, left.version_uuid,
                                   fallback_identities)) {
      return false;
    }
    return left.source_ordinal < right.source_ordinal;
  };
  std::sort(rows->begin(), rows->end(), version_less);
  for (std::size_t index = 0; index < rows->size(); ++index) {
    if ((index & 1023U) == 0 && cancelled("during_chain_validation")) {
      return false;
    }
    const auto& row = (*rows)[index];
    if (index != 0 && StreamingCountIdentityEqual(
                          (*rows)[index - 1].version_uuid, row.version_uuid,
                          fallback_identities)) {
      *detail = "duplicate_row_version_uuid";
      return false;
    }
    if (!row.has_previous_version) continue;
    const auto previous = std::lower_bound(
        rows->begin(), rows->end(), row,
        [&](const auto& candidate, const auto& sought) {
          return StreamingCountIdentityLess(
              candidate.version_uuid, sought.previous_version_uuid,
              fallback_identities);
        });
    if (previous == rows->end() ||
        !StreamingCountIdentityEqual(
            previous->version_uuid, row.previous_version_uuid,
            fallback_identities)) {
      *detail = "previous_row_version_missing";
      return false;
    }
    if (!StreamingCountIdentityEqual(previous->row_uuid, row.row_uuid,
                                     fallback_identities)) {
      *detail = "previous_row_version_wrong_chain";
      return false;
    }
    if (previous->event_sequence >= row.event_sequence) {
      *detail = "previous_row_version_not_older";
      return false;
    }
    if (row.previous_sequence != 0 &&
        previous->event_sequence != row.previous_sequence) {
      *detail = "previous_row_version_sequence_mismatch";
      return false;
    }
  }
  if (cancelled("before_visibility_projection")) return false;
  const auto row_less = [&](const auto& left, const auto& right) {
    if (StreamingCountIdentityLess(left.row_uuid, right.row_uuid,
                                   fallback_identities)) {
      return true;
    }
    if (StreamingCountIdentityLess(right.row_uuid, left.row_uuid,
                                   fallback_identities)) {
      return false;
    }
    if (left.event_sequence != right.event_sequence) {
      return left.event_sequence < right.event_sequence;
    }
    return left.source_ordinal < right.source_ordinal;
  };
  std::sort(rows->begin(), rows->end(), row_less);
  std::size_t begin = 0;
  while (begin < rows->size()) {
    if ((begin & 1023U) == 0 && cancelled("during_visibility_projection")) {
      return false;
    }
    std::size_t end = begin + 1;
    while (end < rows->size() &&
           StreamingCountIdentityEqual((*rows)[begin].row_uuid,
                                       (*rows)[end].row_uuid,
                                       fallback_identities)) {
      ++end;
    }
    const StreamingCountRowVersion* newest_visible = nullptr;
    for (std::size_t index = begin; index < end; ++index) {
      if (result->visibility_recheck_count ==
          std::numeric_limits<std::uint64_t>::max()) {
        *detail = "heap_count_visibility_counter_overflow";
        return false;
      }
      ++result->visibility_recheck_count;
      const auto& row = (*rows)[index];
      if (!row.temporary_session_visible || !row.creator_visible) {
        if (result->invisible_row_version_count ==
            std::numeric_limits<std::uint64_t>::max()) {
          *detail = "heap_count_visibility_counter_overflow";
          return false;
        }
        ++result->invisible_row_version_count;
        continue;
      }
      if (newest_visible == nullptr ||
          newest_visible->event_sequence < row.event_sequence) {
        newest_visible = &row;
      }
    }
    if (newest_visible != nullptr) {
      if (newest_visible->deleted) {
        if (result->tombstone_row_count ==
            std::numeric_limits<std::uint64_t>::max()) {
          *detail = "heap_count_tombstone_counter_overflow";
          return false;
        }
        ++result->tombstone_row_count;
      } else {
        if (result->visible_row_count ==
            std::numeric_limits<std::uint64_t>::max()) {
          *detail = "heap_count_result_overflow";
          return false;
        }
        ++result->visible_row_count;
        if (visible_source_ordinals != nullptr) {
          if (newest_visible->source_ordinal == 0 ||
              newest_visible->source_ordinal >=
                  visible_source_ordinals->size()) {
            *detail = "heap_count_visible_source_ordinal_invalid";
            return false;
          }
          (*visible_source_ordinals)[newest_visible->source_ordinal] = 1;
        }
      }
    }
    begin = end;
  }
  return !cancelled("before_publication");
}

bool ObserveVisibleStreamMemory(
    const MgaVisibleHeapRelationStreamRequest& request,
    const MgaRelationStorageDescriptor& descriptor,
    const StreamingVisibleSelection& selection,
    const CrudRowVersionRecord* transient_row,
    const std::uint64_t consumer_retained_bytes,
    const std::uint64_t prospective_consumer_growth_bytes,
    std::uint64_t* peak_memory_bytes,
    std::string* detail) {
  if (peak_memory_bytes == nullptr || detail == nullptr) return false;
  const auto descriptor_bytes =
      HeapReadStorageDescriptorMemoryBytes(descriptor);
  std::uint64_t live = sizeof(MgaVisibleHeapRelationStreamResult) +
                       sizeof(selection) +
                       static_cast<std::uint64_t>(
                           selection.visible_source_ordinals.capacity()) +
                       kStreamingCountScratchBytes;
  if (!descriptor_bytes.has_value() ||
      !CheckedHeapReadMemoryAdd(*descriptor_bytes, &live) ||
      !AccountHeapReadOwnedString(selection.text_path, &live) ||
      !AccountHeapReadOwnedString(selection.binary_path, &live) ||
      !CheckedHeapReadMemoryAdd(consumer_retained_bytes, &live) ||
      !CheckedHeapReadMemoryAdd(prospective_consumer_growth_bytes, &live)) {
    *detail = "heap_stream_memory_receipt_overflow";
    return false;
  }
  if (transient_row != nullptr) {
    std::uint64_t row_bytes = sizeof(CrudRowVersionRecord);
    if (!AccountHeapReadRowDynamicMemoryBytes(*transient_row, &row_bytes) ||
        !CheckedHeapReadMemoryAdd(row_bytes, &live)) {
      *detail = "heap_stream_memory_receipt_overflow";
      return false;
    }
  }
  *peak_memory_bytes = std::max(*peak_memory_bytes, live);
  if (request.maximum_memory_bytes == 0 ||
      live > request.maximum_memory_bytes) {
    *detail = "heap_stream_maximum_memory_bytes_exceeded";
    return false;
  }
  return true;
}

bool StreamConsumerMemory(
    const MgaVisibleHeapRelationStreamRequest& request,
    std::uint64_t* bytes,
    std::string* detail) {
  if (bytes == nullptr || detail == nullptr ||
      !request.consumer_retained_memory_bytes) {
    if (detail != nullptr) *detail = "heap_stream_consumer_receipt_required";
    return false;
  }
  try {
    *bytes = request.consumer_retained_memory_bytes();
    return true;
  } catch (...) {
    *detail = "heap_stream_consumer_receipt_probe_failed";
    return false;
  }
}

bool VisibleStreamDeliveryBoundReached(
    const MgaVisibleHeapRelationStreamRequest& request,
    const MgaVisibleHeapRelationStreamResult& result) {
  return request.maximum_delivered_visible_rows.has_value() &&
         result.delivered_row_count >=
             *request.maximum_delivered_visible_rows;
}

bool PublishVisibleStreamSecondPassCounters(
    const MgaVisibleHeapRelationCountResult& phase,
    MgaVisibleHeapRelationStreamResult* result,
    std::string* detail) {
  if (result == nullptr || detail == nullptr ||
      phase.storage_bytes_read >
          std::numeric_limits<std::uint64_t>::max() -
              result->storage_bytes_read ||
      phase.decoded_byte_count >
          std::numeric_limits<std::uint64_t>::max() -
              result->decoded_byte_count ||
      phase.storage_bytes_read >
          std::numeric_limits<std::uint64_t>::max() -
              result->second_pass_storage_bytes_read ||
      phase.decoded_byte_count >
          std::numeric_limits<std::uint64_t>::max() -
              result->second_pass_decoded_byte_count) {
    if (detail != nullptr) *detail = "heap_stream_second_pass_counter_overflow";
    return false;
  }
  result->storage_bytes_read += phase.storage_bytes_read;
  result->decoded_byte_count += phase.decoded_byte_count;
  result->second_pass_storage_bytes_read += phase.storage_bytes_read;
  result->second_pass_decoded_byte_count += phase.decoded_byte_count;
  return true;
}

bool DeliverVisibleStreamRow(
    const MgaVisibleHeapRelationStreamRequest& request,
    const MgaRelationStorageDescriptor& descriptor,
    const StreamingVisibleSelection& selection,
    const std::uint64_t source_ordinal,
    const CrudRowVersionRecord& row,
    MgaVisibleHeapRelationStreamResult* result,
    std::string* detail) {
  if (result == nullptr || detail == nullptr ||
      !request.consume_visible_row) {
    if (detail != nullptr) *detail = "heap_stream_consumer_required";
    return false;
  }
  std::uint64_t before = 0;
  if (!StreamConsumerMemory(request, &before, detail) ||
      !ObserveVisibleStreamMemory(
          request, descriptor, selection, &row, before,
          request.maximum_consumer_growth_bytes_per_row,
          &result->peak_live_memory_bytes, detail)) {
    return false;
  }
  bool accepted = false;
  try {
    accepted = request.consume_visible_row(source_ordinal, row);
  } catch (...) {
    *detail = "heap_stream_consumer_threw";
    return false;
  }
  if (!accepted) {
    *detail = "heap_stream_consumer_refused_row";
    return false;
  }
  std::uint64_t after = 0;
  if (!StreamConsumerMemory(request, &after, detail) || after < before ||
      after - before > request.maximum_consumer_growth_bytes_per_row ||
      !ObserveVisibleStreamMemory(
          request, descriptor, selection, nullptr, after, 0,
          &result->peak_live_memory_bytes, detail)) {
    if (detail->empty()) *detail = "heap_stream_consumer_receipt_invalid";
    return false;
  }
  if (result->delivered_row_count ==
      std::numeric_limits<std::uint64_t>::max()) {
    *detail = "heap_stream_delivered_row_count_overflow";
    return false;
  }
  ++result->delivered_row_count;
  const auto& cancellation_requested =
      request.borrowed_cancellation_requested == nullptr
          ? request.cancellation_requested
          : *request.borrowed_cancellation_requested;
  try {
    if (cancellation_requested && cancellation_requested()) {
      result->cancellation_observed = true;
      *detail = "heap_stream_cancelled_after_row_delivery";
      return false;
    }
  } catch (...) {
    *detail = "heap_stream_cancellation_probe_failed";
    return false;
  }
  return true;
}

bool DecodeVisibleStreamTextFile(
    const MgaVisibleHeapRelationStreamRequest& request,
    const std::string& relation_uuid,
    const StreamingVisibleSelection& selection,
    std::uint64_t* source_ordinal,
    MgaVisibleHeapRelationStreamResult* result,
    std::string* detail) {
  if (source_ordinal == nullptr || result == nullptr || detail == nullptr ||
      selection.text_bytes == 0) {
    return selection.text_bytes == 0;
  }
  MgaVisibleHeapRelationCountResult phase;
  StreamingCountBinaryReader reader(
      selection.text_path, selection.text_bytes,
      request.maximum_decoded_bytes_per_pass,
      request.borrowed_cancellation_requested == nullptr
          ? &request.cancellation_requested
          : request.borrowed_cancellation_requested,
      &phase, nullptr, detail);
  if (!reader.Open()) return false;
  std::string line;
  const auto consume_line = [&]() {
    const auto fields = StreamingCountTextFields(line);
    if (fields.size() < 11 || fields[0] != kRowStoreMagic ||
        fields[1] != "ROW_VERSION") {
      return true;
    }
    if (*source_ordinal == std::numeric_limits<std::uint64_t>::max()) {
      *detail = "heap_stream_source_ordinal_overflow";
      return false;
    }
    ++*source_ordinal;
    if (*source_ordinal >= selection.visible_source_ordinals.size()) {
      *detail = "heap_stream_source_ordinal_out_of_range";
      return false;
    }
    if (fields[4] != relation_uuid) {
      *detail = "heap_stream_relation_identity_mismatch";
      return false;
    }
    if (selection.visible_source_ordinals[*source_ordinal] == 0) return true;
    CrudRowVersionRecord row;
    row.creator_tx = ParseU64(std::string(fields[2]));
    row.event_sequence = ParseU64(std::string(fields[3]));
    row.sequence = row.event_sequence;
    row.table_uuid.assign(fields[4]);
    row.row_uuid.assign(fields[5]);
    row.version_uuid.assign(fields[6]);
    row.deleted = fields[7] == "1";
    row.previous_version_uuid.assign(fields[8]);
    row.previous_sequence = ParseU64(std::string(fields[9]));
    row.values = DecodeCrudPairsForHeapRead(std::string(fields[10]));
    if (fields.size() >= 12) row.temporary_session_uuid.assign(fields[11]);
    return DeliverVisibleStreamRow(request, result->descriptor, selection,
                                   *source_ordinal, row, result, detail);
  };
  while (reader.remaining() != 0) {
    std::uint8_t byte = 0;
    if (!reader.ReadU8(&byte)) return false;
    if (byte == '\n') {
      if (!consume_line()) return false;
      line.clear();
      if (VisibleStreamDeliveryBoundReached(request, *result)) {
        return PublishVisibleStreamSecondPassCounters(phase, result, detail);
      }
      continue;
    }
    std::uint64_t consumer_bytes = 0;
    if (!StreamConsumerMemory(request, &consumer_bytes, detail) ||
        line.size() >= request.maximum_memory_bytes ||
        !ObserveVisibleStreamMemory(
            request, result->descriptor, selection, nullptr,
            consumer_bytes + static_cast<std::uint64_t>(line.size()) + 2,
            0, &result->peak_live_memory_bytes, detail)) {
      if (detail->empty()) *detail = "heap_stream_text_record_exceeds_memory_grant";
      return false;
    }
    line.push_back(static_cast<char>(byte));
  }
  if (!line.empty() && !consume_line()) return false;
  if (!reader.Finish()) {
    result->cancellation_observed = reader.cancellation_observed();
    return false;
  }
  return PublishVisibleStreamSecondPassCounters(phase, result, detail);
}

bool DecodeVisibleStreamBinaryFile(
    const MgaVisibleHeapRelationStreamRequest& request,
    const std::string& relation_uuid,
    const StreamingVisibleSelection& selection,
    std::uint64_t* source_ordinal,
    MgaVisibleHeapRelationStreamResult* result,
    std::string* detail) {
  if (source_ordinal == nullptr || result == nullptr || detail == nullptr ||
      selection.binary_bytes == 0) {
    return selection.binary_bytes == 0;
  }
  MgaVisibleHeapRelationCountResult phase;
  StreamingCountBinaryReader reader(
      selection.binary_path, selection.binary_bytes,
      request.maximum_decoded_bytes_per_pass,
      request.borrowed_cancellation_requested == nullptr
          ? &request.cancellation_requested
          : request.borrowed_cancellation_requested,
      &phase, nullptr, detail);
  if (!reader.Open()) return false;
  while (reader.remaining() != 0) {
    std::array<char, 8> magic{};
    std::uint16_t version = 0;
    std::uint16_t flags = 0;
    std::uint32_t column_count = 0;
    std::uint64_t row_count = 0;
    for (auto& byte : magic) {
      if (!reader.ReadU8(reinterpret_cast<std::uint8_t*>(&byte))) return false;
    }
    if (std::string_view(magic.data(), magic.size()) !=
            kScopedRowBinaryBatchMagic ||
        !reader.ReadU16(&version) || !reader.ReadU16(&flags) ||
        !reader.ReadU32(&column_count) || !reader.ReadU64(&row_count) ||
        (version != 1 && version != kScopedRowBinaryLegacyTypedVersion &&
         version != kScopedRowBinaryVersion &&
         version != kScopedRowBinaryNativePacketVersion) ||
        flags != 0 || column_count == 0 || column_count > 4096) {
      *detail = "heap_stream_scoped_binary_header_invalid";
      return false;
    }
    std::vector<std::string> field_order;
    field_order.reserve(column_count);
    for (std::uint32_t column = 0; column < column_count; ++column) {
      std::string field;
      if (!reader.ReadString(&field, false)) return false;
      field_order.push_back(std::move(field));
    }
    std::vector<std::string> field_types;
    std::vector<std::uint8_t> native_tags;
    field_types.reserve(column_count);
    native_tags.reserve(column_count);
    if (version == kScopedRowBinaryNativePacketVersion) {
      for (std::uint32_t column = 0; column < column_count; ++column) {
        std::uint8_t tag = 0;
        const auto name = reader.ReadU8(&tag)
                              ? ScopedRowNativePacketTypeName(tag)
                              : std::string_view{};
        if (name.empty()) {
          *detail = "heap_stream_scoped_binary_type_invalid";
          return false;
        }
        native_tags.push_back(tag);
        field_types.emplace_back(name);
      }
    } else if (version >= kScopedRowBinaryLegacyTypedVersion) {
      for (std::uint32_t column = 0; column < column_count; ++column) {
        std::string type;
        if (!reader.ReadString(&type, false)) return false;
        field_types.push_back(std::move(type));
      }
    } else {
      field_types.assign(column_count, "text");
    }
    const bool compact = version >= kScopedRowBinaryVersion;
    std::uint64_t compact_first_sequence = 0;
    std::uint64_t compact_creator_tx = 0;
    std::string compact_table_uuid;
    std::string compact_session_uuid;
    std::uint8_t compact_flags = 0;
    if (compact &&
        (!reader.ReadU64(&compact_first_sequence) ||
         !reader.ReadU64(&compact_creator_tx) ||
         !reader.ReadString(&compact_table_uuid, false) ||
         !reader.ReadString(&compact_session_uuid, true) ||
         !reader.ReadU8(&compact_flags) || compact_flags != 0 ||
         compact_table_uuid != relation_uuid ||
         (row_count != 0 &&
          row_count - 1 > std::numeric_limits<std::uint64_t>::max() -
                              compact_first_sequence))) {
      *detail = "heap_stream_scoped_binary_compact_header_invalid";
      return false;
    }
    const std::uint64_t bitmap_bytes =
        (static_cast<std::uint64_t>(column_count) + 7U) / 8U;
    if (bitmap_bytes > std::numeric_limits<std::size_t>::max()) {
      *detail = "heap_stream_null_bitmap_overflow";
      return false;
    }
    std::vector<std::uint8_t> null_bitmap(
        static_cast<std::size_t>(bitmap_bytes));
    for (std::uint64_t row_index = 0; row_index < row_count; ++row_index) {
      if (*source_ordinal == std::numeric_limits<std::uint64_t>::max()) {
        *detail = "heap_stream_source_ordinal_overflow";
        return false;
      }
      ++*source_ordinal;
      if (*source_ordinal >= selection.visible_source_ordinals.size()) {
        *detail = "heap_stream_source_ordinal_out_of_range";
        return false;
      }
      const bool selected =
          selection.visible_source_ordinals[*source_ordinal] != 0;
      CrudRowVersionRecord row;
      std::uint8_t deleted = 0;
      if (compact) {
        row.creator_tx = compact_creator_tx;
        row.event_sequence = compact_first_sequence + row_index;
        row.sequence = row.event_sequence;
        row.table_uuid = compact_table_uuid;
        row.temporary_session_uuid = compact_session_uuid;
        if (selected) {
          if (!reader.ReadUuidText(&row.row_uuid) ||
              !reader.ReadUuidText(&row.version_uuid)) {
            return false;
          }
        } else if (!reader.Skip(32)) {
          return false;
        }
      } else {
        std::string table_uuid;
        std::string row_uuid;
        std::string version_uuid;
        std::string previous_uuid;
        std::string session_uuid;
        if (!reader.ReadU64(&row.creator_tx) ||
            !reader.ReadU64(&row.event_sequence) ||
            !reader.ReadU64(&row.previous_sequence) ||
            !reader.ReadU8(&deleted) ||
            !reader.ReadString(&table_uuid, false) ||
            !reader.ReadString(&row_uuid, false) ||
            !reader.ReadString(&version_uuid, false) ||
            !reader.ReadString(&previous_uuid, true) ||
            !reader.ReadString(&session_uuid, true) ||
            table_uuid != relation_uuid) {
          *detail = "heap_stream_scoped_binary_row_header_invalid";
          return false;
        }
        row.sequence = row.event_sequence;
        row.deleted = deleted != 0;
        if (selected) {
          row.table_uuid = std::move(table_uuid);
          row.row_uuid = std::move(row_uuid);
          row.version_uuid = std::move(version_uuid);
          row.previous_version_uuid = std::move(previous_uuid);
          row.temporary_session_uuid = std::move(session_uuid);
        }
      }
      for (auto& byte : null_bitmap) {
        if (!reader.ReadU8(&byte)) return false;
      }
      if (selected) row.values.reserve(column_count);
      for (std::uint32_t column = 0; column < column_count; ++column) {
        const bool is_null =
            (null_bitmap[column / 8U] &
             static_cast<std::uint8_t>(1U << (column % 8U))) != 0;
        if (is_null) {
          if (selected) row.values.push_back({field_order[column], "<NULL>"});
          continue;
        }
        if (!selected) {
          if (version == kScopedRowBinaryNativePacketVersion) {
            const auto tag = native_tags[column];
            if ((tag == 3 && !reader.Skip(1)) ||
                (tag == 4 && !reader.Skip(4)) ||
                ((tag == 2 || tag == 5 || tag == 6) && !reader.Skip(8)) ||
                ((tag == 1 || tag == 7) && !reader.SkipPayloadString())) {
              return false;
            }
          } else if (!reader.SkipPayloadString()) {
            return false;
          }
          continue;
        }
        std::string payload;
        if (version == kScopedRowBinaryNativePacketVersion) {
          const auto tag = native_tags[column];
          const std::size_t fixed =
              tag == 3 ? 1 : (tag == 4 ? 4 :
                              ((tag == 2 || tag == 5 || tag == 6) ? 8 : 0));
          if (fixed != 0) {
            if (!reader.ReadFixedPayload(
                    &payload, fixed, request.maximum_memory_bytes)) {
              return false;
            }
          } else if (!reader.ReadPayloadString(
                         &payload, request.maximum_memory_bytes)) {
            return false;
          }
        } else if (!reader.ReadPayloadString(
                       &payload, request.maximum_memory_bytes)) {
          return false;
        }
        row.values.push_back(
            {field_order[column],
             ScopedRowBinaryMaterializeValue(field_types[column], payload)});
      }
      if (selected &&
          !DeliverVisibleStreamRow(request, result->descriptor, selection,
                                   *source_ordinal, row, result, detail)) {
        return false;
      }
      if (VisibleStreamDeliveryBoundReached(request, *result)) {
        return PublishVisibleStreamSecondPassCounters(phase, result, detail);
      }
    }
  }
  if (!reader.Finish()) {
    result->cancellation_observed = reader.cancellation_observed();
    return false;
  }
  return PublishVisibleStreamSecondPassCounters(phase, result, detail);
}

}  // namespace

PreparedMgaHeapReadAuthorityCohortResult PrepareMgaHeapReadAuthorities(
    const EngineRequestContext& context,
    const std::span<const std::string> relation_uuids) {
  return PrepareMgaHeapReadAuthoritiesForStoreModule(context,
                                                     relation_uuids);
}

PreparedMgaHeapReadAuthorityCohortResult PrepareMgaHeapReadAuthorities(
    const EngineRequestContext& context,
    const std::span<const std::string> relation_uuids,
    const scratchbird::transaction::mga::SnapshotVectorDescriptor&
        resolved_statement_snapshot) {
  return PrepareMgaHeapReadAuthoritiesForStoreModule(
      context, relation_uuids, &resolved_statement_snapshot);
}

EngineApiDiagnostic RevalidatePreparedMgaHeapReadAuthorityCohort(
    const EngineRequestContext& context,
    const PreparedMgaHeapReadAuthorityCohort& cohort) {
  const auto* statement = cohort.statement.get();
  if (statement == nullptr || statement->transaction_states == nullptr ||
      statement->transaction_inventory_snapshot == nullptr ||
      cohort.relations.empty() ||
      statement->database_uuid != context.database_uuid.canonical ||
      statement->statement_uuid != context.statement_uuid.canonical ||
      statement->transaction_uuid != context.transaction_uuid.canonical ||
      statement->statement_snapshot_uuid !=
          context.statement_snapshot_uuid.canonical ||
      statement->statement_metadata_snapshot_uuid !=
          context.statement_metadata_snapshot_uuid.canonical ||
      statement->catalog_epoch_uuid != context.catalog_epoch_uuid.canonical ||
      statement->authorization_authority_uuid !=
          context.authorization_context.authority_uuid.canonical ||
      statement->catalog_generation != context.catalog_generation_id ||
      statement->security_epoch != context.authorization_context.security_epoch ||
      statement->policy_epoch != context.authorization_context.policy_epoch ||
      statement->resource_epoch != context.resource_epoch ||
      statement->local_transaction_id != context.local_transaction_id ||
      !statement->snapshot_vector.inventory_authoritative ||
      !statement->snapshot_vector.complete) {
    return MakeInvalidRequestDiagnostic(
        "mga.heap_relation_read.revalidate",
        "statement_authority_identity_stale");
  }
  const auto inventory_fence = scratchbird::storage::database::
      RevalidateLocalTransactionInventorySnapshot(
          *statement->transaction_inventory_snapshot);
  if (!inventory_fence.ok()) {
    // Inventory publication is database-wide, while this cohort is scoped to
    // one statement and owning transaction. An unrelated session may replace
    // the journal without invalidating either. Resolve through the exact
    // published snapshot authority on the slow path and compare the immutable
    // statement projection; this retains the execution-time TOCTOU fence
    // without turning unrelated transaction finality into a false conflict.
    EngineResolveStatementSnapshotRequest snapshot_request;
    snapshot_request.context = context;
    const auto current_snapshot =
        EngineResolveStatementSnapshot(snapshot_request);
    if (!current_snapshot.ok || !current_snapshot.snapshot_vector.complete ||
        !current_snapshot.snapshot_vector.inventory_authoritative ||
        current_snapshot.snapshot_vector.snapshot_uuid.kind !=
            statement->snapshot_vector.snapshot_uuid.kind ||
        current_snapshot.snapshot_vector.snapshot_uuid.value !=
            statement->snapshot_vector.snapshot_uuid.value ||
        current_snapshot.snapshot_vector.owning_transaction.value !=
            statement->snapshot_vector.owning_transaction.value ||
        current_snapshot.snapshot_vector.visible_committed_high_watermark !=
            statement->snapshot_vector.visible_committed_high_watermark) {
      return MakeInvalidRequestDiagnostic(
          "mga.heap_relation_read.revalidate",
          "transaction_inventory_statement_authority_stale");
    }
  }
  const auto metadata_identity = ExistingFileIdentity(statement->metadata_path);
  const auto savepoint_identity = ExistingFileIdentity(statement->savepoint_path);
  const auto descriptor_identity = ExistingFileIdentity(statement->descriptor_path);
  if ((metadata_identity.ok ? metadata_identity.file_size : 0) !=
          statement->metadata_file_size ||
      (metadata_identity.ok ? metadata_identity.file_mtime_ticks : 0) !=
          statement->metadata_file_mtime_ticks ||
      (savepoint_identity.ok ? savepoint_identity.file_size : 0) !=
          statement->savepoint_file_size ||
      (savepoint_identity.ok ? savepoint_identity.file_mtime_ticks : 0) !=
          statement->savepoint_file_mtime_ticks ||
      (descriptor_identity.ok ? descriptor_identity.file_size : 0) !=
          statement->descriptor_file_size ||
      (descriptor_identity.ok ? descriptor_identity.file_mtime_ticks : 0) !=
          statement->descriptor_file_mtime_ticks) {
    return MakeInvalidRequestDiagnostic(
        "mga.heap_relation_read.revalidate",
        "catalog_or_mga_file_identity_stale");
  }
  for (const auto& [relation_uuid, relation] : cohort.relations) {
    if (relation == nullptr || relation->statement.get() != statement ||
        relation->relation_uuid != relation_uuid ||
        relation->descriptor.relation_uuid.canonical != relation_uuid ||
        relation->descriptor.database_uuid.canonical !=
            context.database_uuid.canonical ||
        relation->descriptor.descriptor_uuid.canonical.empty() ||
        relation->descriptor.descriptor_generation == 0 ||
        relation->current_relation_base_generation == 0 ||
        (relation->temporary
             ? (relation->temporary_scope == "global"
                    ? (!relation->temporary_session_uuid.empty() ||
                       context.session_uuid.canonical.empty())
                    : (relation->temporary_scope != "private" ||
                       relation->temporary_session_uuid !=
                           context.session_uuid.canonical))
             : (!relation->temporary_scope.empty() ||
                !relation->temporary_session_uuid.empty()))) {
      return MakeInvalidRequestDiagnostic(
          "mga.heap_relation_read.revalidate",
          "relation_authority_identity_stale");
    }
  }
  return OkDiagnostic();
}

static MgaVisibleHeapRelationCountResult CountVisibleMgaHeapRelationObserved(
    const EngineRequestContext& context,
    const MgaVisibleHeapRelationCountRequest& request,
    HeapReadRuntimeObservation* runtime_observation,
    const PreparedMgaHeapReadAuthority* prepared_authority = nullptr,
    StreamingVisibleSelection* visible_selection = nullptr) {
  MgaVisibleHeapRelationCountResult result;
  result.memory_grant_bytes = request.maximum_memory_bytes;
  const auto refuse = [&](std::string detail,
                          const MgaHeapReadFailureCategoryV1 category) {
    if (const char* trace_path =
            std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");
        trace_path != nullptr && *trace_path != '\0') {
      std::ofstream trace(trace_path, std::ios::app | std::ios::binary);
      if (trace) {
        trace << "layer=mga_heap_count\taccepted=false\tcategory="
              << static_cast<unsigned>(category) << "\tdetail=" << detail
              << '\n';
      }
    }
    result.ok = false;
    result.diagnostic = MakeInvalidRequestDiagnostic(
        "mga.heap_relation_count", std::move(detail));
    result.failure_category =
        result.cancellation_observed
            ? MgaHeapReadFailureCategoryV1::kCancellation
            : category;
    result.visible_row_count = 0;
    result.current_live_memory_bytes = 0;
    result.memory_receipt_complete = false;
    return result;
  };
  const auto& cancellation_requested =
      request.borrowed_cancellation_requested == nullptr
          ? request.cancellation_requested
          : *request.borrowed_cancellation_requested;
  const auto& relation_uuid = request.borrowed_relation_uuid == nullptr
                                  ? request.relation_uuid
                                  : *request.borrowed_relation_uuid;
  HeapReadRuntimeObservation owned_runtime_observation;
  auto* const effective_runtime_observation =
      runtime_observation == nullptr ? &owned_runtime_observation
                                     : runtime_observation;
  if (relation_uuid.empty() || context.local_transaction_id == 0 ||
      context.transaction_uuid.canonical.empty()) {
    return refuse("exact_relation_transaction_and_snapshot_required",
                  MgaHeapReadFailureCategoryV1::kMgaContext);
  }
  if (request.maximum_decoded_bytes == 0 ||
      request.maximum_memory_bytes == 0) {
    return refuse("nonzero_heap_count_resource_bounds_required",
                  MgaHeapReadFailureCategoryV1::kResource);
  }
  if (!cancellation_requested) {
    return refuse("engine_cancellation_probe_required",
                  MgaHeapReadFailureCategoryV1::kInvalidRequest);
  }
  if (cancellation_requested()) {
    result.cancellation_observed = true;
    return refuse("heap_count_cancelled_before_descriptor_load",
                  MgaHeapReadFailureCategoryV1::kCancellation);
  }

  const auto inventory_guard =
      AcquireTransactionInventoryGuard(context.database_path);
  PreparedMgaHeapReadAuthority owned_authority;
  if (prepared_authority == nullptr) {
    auto prepared = PrepareMgaHeapReadAuthorityForStoreModule(context,
                                                              relation_uuid);
    if (!prepared.ok) {
      result.diagnostic = std::move(prepared.diagnostic);
      result.failure_category = MgaHeapReadFailureCategoryV1::kCatalog;
      return result;
    }
    owned_authority = std::move(prepared.authority);
    prepared_authority = &owned_authority;
  }
  const auto* prepared_statement = prepared_authority->statement.get();
  if (prepared_statement == nullptr ||
      prepared_statement->transaction_states == nullptr ||
      prepared_authority->relation_uuid != relation_uuid ||
      prepared_statement->database_uuid != context.database_uuid.canonical ||
      prepared_statement->statement_uuid != context.statement_uuid.canonical ||
      prepared_statement->transaction_uuid !=
          context.transaction_uuid.canonical ||
      prepared_statement->statement_snapshot_uuid !=
          context.statement_snapshot_uuid.canonical ||
      prepared_statement->statement_metadata_snapshot_uuid !=
          context.statement_metadata_snapshot_uuid.canonical ||
      prepared_statement->catalog_epoch_uuid !=
          context.catalog_epoch_uuid.canonical ||
      prepared_statement->authorization_authority_uuid !=
          context.authorization_context.authority_uuid.canonical ||
      prepared_statement->catalog_generation !=
          context.catalog_generation_id ||
      prepared_statement->security_epoch !=
          context.authorization_context.security_epoch ||
      prepared_statement->policy_epoch !=
          context.authorization_context.policy_epoch ||
      prepared_statement->resource_epoch != context.resource_epoch ||
      prepared_statement->local_transaction_id !=
          context.local_transaction_id ||
      !prepared_statement->snapshot_vector.inventory_authoritative ||
      !prepared_statement->snapshot_vector.complete) {
    return refuse("prepared_heap_count_authority_is_stale",
                  MgaHeapReadFailureCategoryV1::kMgaContext);
  }
  result.descriptor = prepared_authority->descriptor;
  result.current_relation_base_generation =
      prepared_authority->current_relation_base_generation;

  BoundedScopedRowReadControl savepoint_control;
  savepoint_control.maximum_row_versions =
      std::numeric_limits<std::uint64_t>::max();
  savepoint_control.maximum_bytes = request.maximum_decoded_bytes;
  savepoint_control.maximum_memory_bytes = request.maximum_memory_bytes;
  savepoint_control.cancellation_requested = &cancellation_requested;
  savepoint_control.runtime_observation = effective_runtime_observation;
  SavepointParsedState savepoints;
  const auto descriptor_memory =
      HeapReadStorageDescriptorMemoryBytes(result.descriptor);
  if (!descriptor_memory.has_value()) {
    return refuse("heap_count_authority_memory_receipt_overflow",
                  MgaHeapReadFailureCategoryV1::kResource);
  }
  savepoint_control.retained_parent_memory_bytes =
      sizeof(result) + *descriptor_memory + kStreamingCountScratchBytes;
  if (!ParseSavepointsBounded(
          context, &savepoint_control,
          savepoint_control.retained_parent_memory_bytes, &savepoints)) {
    result.cancellation_observed = savepoint_control.cancellation_observed;
    result.peak_live_memory_bytes = savepoint_control.peak_live_memory_bytes;
    return refuse(savepoint_control.refusal_detail.empty()
                      ? "bounded_savepoint_read_failed"
                      : savepoint_control.refusal_detail,
                  savepoint_control.failure_category ==
                          MgaHeapReadFailureCategoryV1::kNone
                      ? MgaHeapReadFailureCategoryV1::kResource
                      : savepoint_control.failure_category);
  }
  result.storage_bytes_read =
      effective_runtime_observation->storage_bytes_read;

  std::vector<StreamingCountRowVersion> rows;
  std::vector<std::string> fallback_identities;
  std::string detail;
  result.peak_live_memory_bytes = savepoint_control.peak_live_memory_bytes;
  if (!ObserveStreamingCountMemory(
          rows, fallback_identities, savepoints, result.descriptor,
          request.maximum_memory_bytes, &result.peak_live_memory_bytes,
          &detail)) {
    return refuse(detail, MgaHeapReadFailureCategoryV1::kResource);
  }

  const std::string text_path = ScopedRowStorePath(context, relation_uuid);
  const std::string binary_path =
      ScopedRowBinaryStorePath(context, relation_uuid);
  const bool text_exists = FileExistsAndNotEmpty(text_path);
  const bool binary_exists = FileExistsAndNotEmpty(binary_path);
  result.scoped_physical_segment_used = text_exists || binary_exists;
  const auto authorize_file = [&](const std::string& path,
                                  std::uint64_t* bytes) {
    std::error_code ignored;
    const auto size = std::filesystem::file_size(path, ignored);
    if (bytes == nullptr || ignored ||
        size == static_cast<std::uintmax_t>(-1) ||
        size > std::numeric_limits<std::uint64_t>::max()) {
      detail = "heap_count_scoped_segment_size_unavailable";
      return false;
    }
    *bytes = static_cast<std::uint64_t>(size);
    return true;
  };
  std::uint64_t text_bytes = 0;
  std::uint64_t binary_bytes = 0;
  if ((text_exists && !authorize_file(text_path, &text_bytes)) ||
      (binary_exists && !authorize_file(binary_path, &binary_bytes)) ||
      text_bytes > request.maximum_decoded_bytes ||
      binary_bytes > request.maximum_decoded_bytes - text_bytes) {
    return refuse(detail.empty()
                      ? "heap_count_maximum_decoded_bytes_exceeded"
                      : detail,
                  MgaHeapReadFailureCategoryV1::kResource);
  }
  if (visible_selection != nullptr) {
    visible_selection->text_path = text_path;
    visible_selection->binary_path = binary_path;
    visible_selection->text_bytes = text_bytes;
    visible_selection->binary_bytes = binary_bytes;
  }
  if (text_exists &&
      !DecodeStreamingCountTextFile(
          text_path, text_bytes, relation_uuid,
          context.session_uuid.canonical, prepared_statement->snapshot_vector,
          *prepared_statement->transaction_states, savepoints,
          result.descriptor, request.maximum_decoded_bytes,
          request.maximum_memory_bytes, &cancellation_requested, &rows,
          &fallback_identities, &result.peak_live_memory_bytes, &result,
          effective_runtime_observation, &detail)) {
    return refuse(detail.empty() ? "heap_count_scoped_text_decode_failed"
                                 : detail,
                  result.cancellation_observed
                      ? MgaHeapReadFailureCategoryV1::kCancellation
                      : MgaHeapReadFailureCategoryV1::kCorruptStorage);
  }
  if (binary_exists &&
      !DecodeStreamingCountBinaryFile(
          binary_path, binary_bytes, relation_uuid,
          context.session_uuid.canonical, prepared_statement->snapshot_vector,
          *prepared_statement->transaction_states, savepoints,
          result.descriptor, request.maximum_decoded_bytes,
          request.maximum_memory_bytes, &cancellation_requested, &rows,
          &fallback_identities, &result.peak_live_memory_bytes, &result,
          effective_runtime_observation, &detail)) {
    return refuse(detail.empty() ? "heap_count_scoped_binary_decode_failed"
                                 : detail,
                  result.cancellation_observed
                      ? MgaHeapReadFailureCategoryV1::kCancellation
                      : MgaHeapReadFailureCategoryV1::kCorruptStorage);
  }
  if (visible_selection != nullptr) {
    if (result.scanned_row_version_count ==
            std::numeric_limits<std::uint64_t>::max() ||
        result.scanned_row_version_count + 1 >
            std::numeric_limits<std::size_t>::max()) {
      return refuse("heap_count_visible_source_bitmap_overflow",
                    MgaHeapReadFailureCategoryV1::kResource);
    }
    const auto metadata_memory = StreamingCountMetadataMemoryBytes(
        rows, fallback_identities, savepoints, result.descriptor);
    std::uint64_t selection_memory = 0;
    if (!metadata_memory.has_value() ||
        !CheckedHeapReadMemoryAdd(
            result.scanned_row_version_count + 1, &selection_memory) ||
        !CheckedHeapReadMemoryAdd(*metadata_memory, &selection_memory) ||
        selection_memory > request.maximum_memory_bytes) {
      return refuse("heap_count_visible_source_bitmap_exceeds_memory_grant",
                    MgaHeapReadFailureCategoryV1::kResource);
    }
    result.peak_live_memory_bytes =
        std::max(result.peak_live_memory_bytes, selection_memory);
    visible_selection->visible_source_ordinals.assign(
        static_cast<std::size_t>(result.scanned_row_version_count + 1), 0);
  }
  if (!ValidateAndCountStreamingRows(
          &rows, fallback_identities, &cancellation_requested, &result,
          &detail,
          visible_selection == nullptr
              ? nullptr
              : &visible_selection->visible_source_ordinals)) {
    return refuse(detail.empty() ? "heap_count_visibility_failed" : detail,
                  result.cancellation_observed
                      ? MgaHeapReadFailureCategoryV1::kCancellation
                      : MgaHeapReadFailureCategoryV1::kCorruptStorage);
  }
  if (!ObserveStreamingCountMemory(
          rows, fallback_identities, savepoints, result.descriptor,
          request.maximum_memory_bytes, &result.peak_live_memory_bytes,
          &detail)) {
    return refuse(detail, MgaHeapReadFailureCategoryV1::kResource);
  }
  result.ok = true;
  result.failure_category = MgaHeapReadFailureCategoryV1::kNone;
  result.diagnostic = OkDiagnostic();
  result.current_live_memory_bytes = sizeof(result) + *descriptor_memory;
  if (!AccountHeapReadOwnedString(result.diagnostic.code,
                                  &result.current_live_memory_bytes) ||
      !AccountHeapReadOwnedString(result.diagnostic.message_key,
                                  &result.current_live_memory_bytes) ||
      !AccountHeapReadOwnedString(result.diagnostic.detail,
                                  &result.current_live_memory_bytes)) {
    return refuse("heap_count_memory_receipt_overflow",
                  MgaHeapReadFailureCategoryV1::kResource);
  }
  result.peak_live_memory_bytes = std::max(
      result.peak_live_memory_bytes, result.current_live_memory_bytes);
  result.memory_receipt_complete =
      result.current_live_memory_bytes <= result.peak_live_memory_bytes &&
      result.peak_live_memory_bytes <= result.memory_grant_bytes;
  if (!result.memory_receipt_complete) {
    return refuse("heap_count_memory_receipt_incomplete",
                  MgaHeapReadFailureCategoryV1::kResource);
  }
  effective_runtime_observation->storage_bytes_read =
      result.storage_bytes_read;
  effective_runtime_observation->decoded_bytes = result.decoded_byte_count;
  return result;
}

MgaVisibleHeapRelationCountResult CountVisibleMgaHeapRelation(
    const EngineRequestContext& context,
    const MgaVisibleHeapRelationCountRequest& request) {
  return CountVisibleMgaHeapRelationObserved(context, request, nullptr);
}

MgaVisibleHeapRelationCountResult CountVisibleMgaHeapRelationWithObservation(
    const EngineRequestContext& context,
    const MgaVisibleHeapRelationCountRequest& request,
    HeapReadRuntimeObservation* runtime_observation,
    const PreparedMgaHeapReadAuthority* prepared_authority) {
  return CountVisibleMgaHeapRelationObserved(
      context, request, runtime_observation, prepared_authority);
}

MgaVisibleHeapRelationStreamResult StreamVisibleMgaHeapRelation(
    const EngineRequestContext& context,
    const MgaVisibleHeapRelationStreamRequest& request) {
  MgaVisibleHeapRelationStreamResult result;
  result.memory_grant_bytes = request.maximum_memory_bytes;
  const auto refuse = [&](std::string detail,
                          const MgaHeapReadFailureCategoryV1 category) {
    result.ok = false;
    result.diagnostic = MakeInvalidRequestDiagnostic(
        "mga.heap_relation_stream", std::move(detail));
    result.failure_category =
        result.cancellation_observed
            ? MgaHeapReadFailureCategoryV1::kCancellation
            : category;
    result.memory_receipt_complete = false;
    return result;
  };
  const auto& relation_uuid = request.borrowed_relation_uuid == nullptr
                                  ? request.relation_uuid
                                  : *request.borrowed_relation_uuid;
  const auto& cancellation_requested =
      request.borrowed_cancellation_requested == nullptr
          ? request.cancellation_requested
          : *request.borrowed_cancellation_requested;
  if (relation_uuid.empty() || request.maximum_decoded_bytes_per_pass == 0 ||
      request.maximum_memory_bytes == 0 || !cancellation_requested ||
      !request.prepare_consumer_for_visible_rows ||
      !request.consumer_retained_memory_bytes ||
      !request.consume_visible_row) {
    return refuse("complete_stream_identity_bounds_and_callbacks_required",
                  MgaHeapReadFailureCategoryV1::kInvalidRequest);
  }
  if (cancellation_requested()) {
    result.cancellation_observed = true;
    return refuse("heap_stream_cancelled_before_visibility_pass",
                  MgaHeapReadFailureCategoryV1::kCancellation);
  }

  // The same recursive engine inventory guard spans both physical passes.
  // Engine-owned MGA append/finality routes therefore cannot change the
  // authorized segment extents between visibility selection and delivery.
  const auto inventory_guard =
      AcquireTransactionInventoryGuard(context.database_path);
  StreamingVisibleSelection selection;
  MgaVisibleHeapRelationCountRequest count_request;
  count_request.borrowed_relation_uuid = &relation_uuid;
  count_request.maximum_decoded_bytes =
      request.maximum_decoded_bytes_per_pass;
  count_request.maximum_memory_bytes = request.maximum_memory_bytes;
  count_request.borrowed_cancellation_requested = &cancellation_requested;
  auto counted = CountVisibleMgaHeapRelationObserved(
      context, count_request, nullptr, nullptr, &selection);
  result.visible_row_count = counted.visible_row_count;
  result.current_relation_base_generation =
      counted.current_relation_base_generation;
  result.scanned_row_version_count = counted.scanned_row_version_count;
  result.decoded_byte_count = counted.decoded_byte_count;
  result.storage_bytes_read = counted.storage_bytes_read;
  result.visibility_recheck_count = counted.visibility_recheck_count;
  result.invisible_row_version_count = counted.invisible_row_version_count;
  result.tombstone_row_count = counted.tombstone_row_count;
  result.scoped_physical_segment_used =
      counted.scoped_physical_segment_used;
  result.cancellation_observed = counted.cancellation_observed;
  result.peak_live_memory_bytes = counted.peak_live_memory_bytes;
  if (!counted.ok) {
    result.diagnostic = std::move(counted.diagnostic);
    result.failure_category = counted.failure_category;
    return result;
  }
  result.descriptor = std::move(counted.descriptor);
  result.complete_mga_chain_validation = true;

  const auto exact_extent = [](const std::string& path,
                               const std::uint64_t expected) {
    std::error_code ignored;
    const bool exists = std::filesystem::exists(path, ignored);
    if (ignored) return false;
    if (!exists) return expected == 0;
    const auto size = std::filesystem::file_size(path, ignored);
    return !ignored && size != static_cast<std::uintmax_t>(-1) &&
           size <= std::numeric_limits<std::uint64_t>::max() &&
           static_cast<std::uint64_t>(size) == expected;
  };
  if (!exact_extent(selection.text_path, selection.text_bytes) ||
      !exact_extent(selection.binary_path, selection.binary_bytes)) {
    return refuse("heap_stream_scoped_segment_changed_between_passes",
                  MgaHeapReadFailureCategoryV1::kCorruptStorage);
  }
  std::uint64_t consumer_bytes = 0;
  std::string detail;
  auto prepared_request = request;
  bool consumer_prepared = false;
  try {
    consumer_prepared = prepared_request.prepare_consumer_for_visible_rows(
        result.descriptor, result.visible_row_count,
        &prepared_request.maximum_consumer_growth_bytes_per_row);
  } catch (...) {
    return refuse("heap_stream_consumer_preparation_threw",
                  MgaHeapReadFailureCategoryV1::kInvalidRequest);
  }
  if (!consumer_prepared) {
    return refuse("heap_stream_consumer_preparation_refused",
                  MgaHeapReadFailureCategoryV1::kInvalidRequest);
  }
  const auto delivery_target =
      request.maximum_delivered_visible_rows.has_value()
          ? std::min(result.visible_row_count,
                     *request.maximum_delivered_visible_rows)
          : result.visible_row_count;
  if (delivery_target != 0 &&
      prepared_request.maximum_consumer_growth_bytes_per_row == 0) {
    return refuse("heap_stream_consumer_growth_bound_required",
                  MgaHeapReadFailureCategoryV1::kResource);
  }
  if (!StreamConsumerMemory(prepared_request, &consumer_bytes, &detail) ||
      !ObserveVisibleStreamMemory(
          prepared_request, result.descriptor, selection, nullptr,
          consumer_bytes, 0,
          &result.peak_live_memory_bytes, &detail)) {
    return refuse(detail.empty() ? "heap_stream_memory_admission_failed"
                                 : detail,
                  MgaHeapReadFailureCategoryV1::kResource);
  }
  std::uint64_t source_ordinal = 0;
  bool value_pass_ok = true;
  if (delivery_target != 0) {
    value_pass_ok = DecodeVisibleStreamTextFile(
        prepared_request, relation_uuid, selection, &source_ordinal, &result,
        &detail);
    if (value_pass_ok &&
        !VisibleStreamDeliveryBoundReached(prepared_request, result)) {
      value_pass_ok = DecodeVisibleStreamBinaryFile(
          prepared_request, relation_uuid, selection, &source_ordinal, &result,
          &detail);
    }
  }
  result.second_pass_scanned_row_version_count = source_ordinal;
  if (!value_pass_ok) {
    const bool cancelled = result.cancellation_observed ||
                           detail.find("cancelled") != std::string::npos;
    result.cancellation_observed = cancelled;
    const bool resource = detail.find("memory") != std::string::npos ||
                          detail.find("maximum") != std::string::npos ||
                          detail.find("overflow") != std::string::npos;
    return refuse(detail.empty() ? "heap_stream_value_pass_failed" : detail,
                  cancelled
                      ? MgaHeapReadFailureCategoryV1::kCancellation
                      : (resource ? MgaHeapReadFailureCategoryV1::kResource
                                  : MgaHeapReadFailureCategoryV1::kCorruptStorage));
  }
  try {
    if (cancellation_requested()) {
      result.cancellation_observed = true;
      return refuse("heap_stream_cancelled_before_value_publication",
                    MgaHeapReadFailureCategoryV1::kCancellation);
    }
  } catch (...) {
    return refuse("heap_stream_cancellation_probe_failed",
                  MgaHeapReadFailureCategoryV1::kCancellation);
  }
  result.complete_value_delivery =
      result.delivered_row_count == result.visible_row_count;
  result.delivery_stopped_by_bound =
      request.maximum_delivered_visible_rows.has_value() &&
      *request.maximum_delivered_visible_rows < result.visible_row_count &&
      result.delivered_row_count == *request.maximum_delivered_visible_rows;
  if (result.delivered_row_count != delivery_target ||
      source_ordinal > result.scanned_row_version_count ||
      (delivery_target != 0 &&
       !request.maximum_delivered_visible_rows.has_value() &&
       (source_ordinal != result.scanned_row_version_count ||
        !result.complete_value_delivery)) ||
      (!result.complete_value_delivery &&
       !result.delivery_stopped_by_bound)) {
    return refuse("heap_stream_visibility_selection_cardinality_mismatch",
                  MgaHeapReadFailureCategoryV1::kCorruptStorage);
  }
  if (!exact_extent(selection.text_path, selection.text_bytes) ||
      !exact_extent(selection.binary_path, selection.binary_bytes)) {
    return refuse("heap_stream_scoped_segment_changed_during_value_pass",
                  MgaHeapReadFailureCategoryV1::kCorruptStorage);
  }
  result.exact_segment_extent_revalidated = true;
  if (!StreamConsumerMemory(prepared_request, &consumer_bytes, &detail) ||
      !ObserveVisibleStreamMemory(
          prepared_request, result.descriptor, selection, nullptr,
          consumer_bytes, 0,
          &result.peak_live_memory_bytes, &detail)) {
    return refuse(detail.empty() ? "heap_stream_final_memory_receipt_failed"
                                 : detail,
                  MgaHeapReadFailureCategoryV1::kResource);
  }
  const auto descriptor_bytes =
      HeapReadStorageDescriptorMemoryBytes(result.descriptor);
  result.current_live_memory_bytes =
      sizeof(result) + sizeof(selection) +
      static_cast<std::uint64_t>(
          selection.visible_source_ordinals.capacity()) +
      consumer_bytes;
  if (!descriptor_bytes.has_value() ||
      !CheckedHeapReadMemoryAdd(*descriptor_bytes,
                                &result.current_live_memory_bytes) ||
      !AccountHeapReadOwnedString(selection.text_path,
                                  &result.current_live_memory_bytes) ||
      !AccountHeapReadOwnedString(selection.binary_path,
                                  &result.current_live_memory_bytes)) {
    return refuse("heap_stream_final_memory_receipt_overflow",
                  MgaHeapReadFailureCategoryV1::kResource);
  }
  result.peak_live_memory_bytes = std::max(
      result.peak_live_memory_bytes, result.current_live_memory_bytes);
  result.memory_receipt_complete =
      result.current_live_memory_bytes <= result.peak_live_memory_bytes &&
      result.peak_live_memory_bytes <= result.memory_grant_bytes;
  if (!result.memory_receipt_complete) {
    return refuse("heap_stream_final_memory_receipt_incomplete",
                  MgaHeapReadFailureCategoryV1::kResource);
  }
  result.ok = true;
  result.failure_category = MgaHeapReadFailureCategoryV1::kNone;
  result.diagnostic = OkDiagnostic();
  return result;
}


}  // namespace scratchbird::engine::internal_api
