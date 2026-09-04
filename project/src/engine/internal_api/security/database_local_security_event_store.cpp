// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_local_security_event_store.hpp"

#include "database_format.hpp"
#include "database_local_private_relation_locator.hpp"
#include "database_lifecycle.hpp"
#include "disk_device.hpp"
#include "local_transaction_store.hpp"
#include "page_header.hpp"
#include "physical_mga_cow_store.hpp"
#include "row_data_page.hpp"
#include "security_crypto_policy.hpp"
#include "startup_state.hpp"
#include "transaction/transaction_api.hpp"
#include "transaction_snapshot.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api {
namespace {

namespace core_uuid = scratchbird::core::uuid;
namespace disk = scratchbird::storage::disk;
namespace mga = scratchbird::transaction::mga;
namespace page = scratchbird::storage::page;
namespace storage = scratchbird::storage::database;

using scratchbird::core::datatypes::CanonicalTypeId;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::Uuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::byte;

constexpr std::string_view kLifecycleMagic = "SBSECPL1";
constexpr std::string_view kSuccessorKind = "AUTH_CONTEXT_SUCCESSOR";

struct ParsedBatch {
  Uuid database_uuid;
  Uuid relation_uuid;
  Uuid transaction_uuid;
  Uuid actor_principal_uuid;
  std::uint64_t creator_tx = 0;
  std::uint64_t prior_generation = 0;
  std::uint64_t successor_generation = 0;
  Uuid predecessor_page_uuid;
  std::uint64_t predecessor_page_number = 0;
  std::uint64_t predecessor_page_generation = 0;
  std::vector<std::string> events;
  std::uint64_t page_number = 0;
  Uuid outer_page_uuid;
  std::uint64_t outer_page_generation = 0;
  TypedUuid row_uuid;
};

struct LocatorRuntimeState {
  disk::SerializedDatabaseHeader serialized_header{};
  storage::SerializedDatabaseLocalPrivateSecurityMarkerV1 marker_bytes{};
  storage::DatabaseLocalPrivateSecurityLocatorInspectResultV1 inspected;
  storage::DatabaseLocalPrivateSecurityLocatorVisibilityResultV1 selected;
  mga::LocalTransactionInventory inventory;
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  std::uint32_t page_size = 0;
  std::uint64_t next_page_number = 0;
};

EngineApiDiagnostic OkDiagnostic() {
  EngineApiDiagnostic diagnostic;
  diagnostic.code = "SB_ENGINE_API_OK";
  diagnostic.message_key = "engine.api.ok";
  diagnostic.error = false;
  return diagnostic;
}

EngineApiDiagnostic ErrorDiagnostic(const char* code,
                                    std::string detail,
                                    std::string message_key = {}) {
  EngineApiDiagnostic diagnostic;
  diagnostic.code = code;
  diagnostic.message_key = message_key.empty()
                               ? "security.catalog.private_lifecycle"
                               : std::move(message_key);
  diagnostic.detail = std::move(detail);
  diagnostic.error = true;
  return diagnostic;
}

EngineApiDiagnostic StorageDiagnostic(
    const scratchbird::core::platform::DiagnosticRecord& source,
    const char* fallback_code,
    std::string detail) {
  EngineApiDiagnostic diagnostic;
  diagnostic.code = fallback_code;
  diagnostic.message_key = source.message_key.empty()
                               ? "security.catalog.private_lifecycle.storage"
                               : source.message_key;
  if (!source.diagnostic_code.empty()) {
    if (!detail.empty()) detail.push_back(':');
    detail += source.diagnostic_code;
  }
  diagnostic.detail = std::move(detail);
  diagnostic.error = true;
  return diagnostic;
}

bool SameUuid(const Uuid& left, const Uuid& right) {
  return left == right;
}

bool SameTypedUuid(const TypedUuid& left, const TypedUuid& right) {
  return left.kind == right.kind && SameUuid(left.value, right.value);
}

bool HasTraceTag(const EngineRequestContext& context,
                 std::string_view expected) {
  return std::find(context.trace_tags.begin(), context.trace_tags.end(),
                   expected) != context.trace_tags.end();
}

bool ParseExactU64(std::string_view text, std::uint64_t* value) {
  if (value == nullptr || text.empty()) return false;
  std::uint64_t parsed = 0;
  const auto converted =
      std::from_chars(text.data(), text.data() + text.size(), parsed);
  if (converted.ec != std::errc{} ||
      converted.ptr != text.data() + text.size()) {
    return false;
  }
  *value = parsed;
  return true;
}

std::vector<std::string_view> SplitTabs(std::string_view line) {
  std::vector<std::string_view> parts;
  std::size_t begin = 0;
  while (begin <= line.size()) {
    const std::size_t separator = line.find('\t', begin);
    if (separator == std::string_view::npos) {
      parts.push_back(line.substr(begin));
      break;
    }
    parts.push_back(line.substr(begin, separator - begin));
    begin = separator + 1;
  }
  return parts;
}

bool IsAuthorityKind(std::string_view kind) {
  static constexpr std::array<std::string_view, 7> kKinds = {
      "PRINCIPAL", "ROLE", "GROUP", "MEMBERSHIP", "GRANT", "REVOKE",
      "ROW_POLICY"};
  return std::find(kKinds.begin(), kKinds.end(), kind) != kKinds.end();
}

std::size_t ExactFieldCount(std::string_view kind) {
  if (kind == "PRINCIPAL") return 10;
  if (kind == "ROLE") return 9;
  if (kind == "GROUP") return 9;
  if (kind == "MEMBERSHIP") return 10;
  if (kind == "GRANT") return 13;
  if (kind == "REVOKE") return 8;
  if (kind == "AUDIT") return 10;
  if (kind == "CACHE_INVALIDATE") return 6;
  if (kind == "ROW_POLICY") return 27;
  if (kind == kSuccessorKind) return 5;
  return 0;
}

std::size_t GenerationField(std::string_view kind) {
  if (kind == "PRINCIPAL") return 8;
  if (kind == "ROLE" || kind == "GROUP" || kind == "REVOKE") return 7;
  if (kind == "MEMBERSHIP") return 8;
  if (kind == "GRANT") return 11;
  if (kind == "AUDIT") return 9;
  if (kind == "CACHE_INVALIDATE") return 5;
  if (kind == "ROW_POLICY") return 10;
  return std::numeric_limits<std::size_t>::max();
}

bool ValidateLifecycleLine(std::string_view line,
                           std::string_view expected_kind,
                           std::uint64_t creator_tx,
                           std::uint64_t expected_generation,
                           std::vector<std::string_view>* parts_out) {
  if (line.empty() || line.find('\n') != std::string_view::npos ||
      line.find('\r') != std::string_view::npos) {
    return false;
  }
  auto parts = SplitTabs(line);
  if (parts.size() != ExactFieldCount(expected_kind) ||
      parts[0] != kLifecycleMagic || parts[1] != expected_kind) {
    return false;
  }
  std::uint64_t parsed_tx = 0;
  if (!ParseExactU64(parts[2], &parsed_tx) || parsed_tx != creator_tx) {
    return false;
  }
  const std::size_t generation_field = GenerationField(expected_kind);
  if (generation_field != std::numeric_limits<std::size_t>::max()) {
    std::uint64_t parsed_generation = 0;
    if (!ParseExactU64(parts[generation_field], &parsed_generation) ||
        parsed_generation == 0 || parsed_generation != expected_generation) {
      return false;
    }
  }
  if (parts_out != nullptr) *parts_out = std::move(parts);
  return true;
}

bool ValidateUnsealedEvents(const std::vector<std::string>& events,
                            std::uint64_t creator_tx,
                            std::string_view actor_principal_uuid,
                            std::uint64_t expected_generation,
                            std::string* refusal) {
  auto refuse = [&](std::string reason) {
    if (refusal != nullptr) *refusal = std::move(reason);
    return false;
  };
  if (events.size() != 3) return refuse("exact_authority_audit_cache_batch_required");
  const auto authority_parts = SplitTabs(events[0]);
  if (authority_parts.size() < 2 || !IsAuthorityKind(authority_parts[1])) {
    return refuse("exactly_one_authority_event_required");
  }
  if (!ValidateLifecycleLine(events[0], authority_parts[1], creator_tx,
                             expected_generation, nullptr)) {
    return refuse("authority_event_invalid");
  }
  if (authority_parts[1] == "GRANT" && authority_parts[10] != "allow" &&
      authority_parts[10] != "deny") {
    return refuse("grant_effect_invalid");
  }
  std::vector<std::string_view> audit_parts;
  if (!ValidateLifecycleLine(events[1], "AUDIT", creator_tx,
                             expected_generation, &audit_parts) ||
      audit_parts[5] != actor_principal_uuid || audit_parts[7] != "success") {
    return refuse("audit_event_invalid");
  }
  if (!ValidateLifecycleLine(events[2], "CACHE_INVALIDATE", creator_tx,
                             expected_generation, nullptr)) {
    return refuse("cache_invalidation_event_invalid");
  }
  return true;
}

std::string NewlineTerminated(const std::vector<std::string>& events) {
  std::string payload;
  for (const auto& event : events) {
    payload.append(event);
    payload.push_back('\n');
  }
  return payload;
}

std::string SuccessorEvent(std::uint64_t creator_tx,
                           std::uint64_t generation,
                           const std::vector<std::string>& unsealed_events) {
  return std::string(kLifecycleMagic) + "\t" + std::string(kSuccessorKind) +
         "\t" + std::to_string(creator_tx) + "\t" +
         std::to_string(generation) +
         "\tsecurity-context-successor:v1:sha256:" +
         SecuritySha256Hex(NewlineTerminated(unsealed_events));
}

std::vector<byte> EncodeBatch(const ParsedBatch& batch) {
  storage::DatabaseLocalSecurityBatchEnvelopeV1 carrier;
  carrier.database_uuid = batch.database_uuid;
  carrier.relation_uuid = batch.relation_uuid;
  carrier.transaction_uuid = batch.transaction_uuid;
  carrier.actor_principal_uuid = batch.actor_principal_uuid;
  carrier.creator_local_transaction_id = batch.creator_tx;
  carrier.prior_security_context_generation = batch.prior_generation;
  carrier.successor_security_context_generation = batch.successor_generation;
  carrier.predecessor_page_uuid = batch.predecessor_page_uuid;
  carrier.predecessor_page_number = batch.predecessor_page_number;
  carrier.predecessor_page_generation = batch.predecessor_page_generation;
  carrier.events = batch.events;
  return storage::EncodeDatabaseLocalSecurityBatchEnvelopeV1(carrier);
}

bool DecodeBatch(const std::vector<byte>& encoded,
                 ParsedBatch* batch,
                 std::string* refusal) {
  if (batch == nullptr) return false;
  storage::DatabaseLocalSecurityBatchEnvelopeV1 carrier;
  if (!storage::DecodeDatabaseLocalSecurityBatchEnvelopeV1(
          encoded, &carrier, refusal)) {
    return false;
  }
  batch->database_uuid = carrier.database_uuid;
  batch->relation_uuid = carrier.relation_uuid;
  batch->transaction_uuid = carrier.transaction_uuid;
  batch->actor_principal_uuid = carrier.actor_principal_uuid;
  batch->creator_tx = carrier.creator_local_transaction_id;
  batch->prior_generation = carrier.prior_security_context_generation;
  batch->successor_generation = carrier.successor_security_context_generation;
  batch->predecessor_page_uuid = carrier.predecessor_page_uuid;
  batch->predecessor_page_number = carrier.predecessor_page_number;
  batch->predecessor_page_generation = carrier.predecessor_page_generation;
  batch->events = std::move(carrier.events);
  return true;
}

TypedUuid SecurityRelationUuid() {
  const auto parsed = core_uuid::ParseDurableEngineIdentityUuid(
      UuidKind::object, std::string(kDatabaseLocalSecurityRelationUuidV1));
  return parsed.ok() ? parsed.value : TypedUuid{};
}

bool ExactTransactionIdentity(const EngineRequestContext& context,
                              const mga::LocalTransactionInventory& inventory,
                              bool require_active,
                              mga::TransactionInventoryEntry* entry,
                              EngineApiDiagnostic* diagnostic) {
  if (context.local_transaction_id == 0 ||
      context.transaction_uuid.canonical.empty()) {
    if (diagnostic != nullptr) {
      *diagnostic = ErrorDiagnostic(
          kDatabaseLocalSecurityDiagnosticTransactionRequired,
          "local_transaction_id_and_uuid_required");
    }
    return false;
  }
  const auto transaction_uuid = core_uuid::ParseDurableEngineIdentityUuid(
      UuidKind::transaction, context.transaction_uuid.canonical);
  const auto found = mga::LookupLocalTransaction(
      inventory, mga::MakeLocalTransactionId(context.local_transaction_id));
  if (!transaction_uuid.ok() || !found.ok() ||
      !SameUuid(transaction_uuid.value.value,
                found.entry.identity.transaction_uuid.value) ||
      (require_active && found.entry.state != mga::TransactionState::active)) {
    if (diagnostic != nullptr) {
      *diagnostic = ErrorDiagnostic(
          kDatabaseLocalSecurityDiagnosticTransactionRequired,
          require_active ? "exact_active_inventory_transaction_required"
                         : "exact_inventory_transaction_required");
    }
    return false;
  }
  if (entry != nullptr) *entry = found.entry;
  return true;
}

bool ValidateDecodedBatch(const ParsedBatch& batch,
                          const Uuid& expected_database_uuid,
                          const TypedUuid& expected_relation_uuid,
                          const page::RowDataRecord& row,
                          std::string* refusal) {
  auto refuse = [&](std::string reason) {
    if (refusal != nullptr) *refusal = std::move(reason);
    return false;
  };
  if (!SameUuid(batch.database_uuid, expected_database_uuid) ||
      !SameUuid(batch.relation_uuid, expected_relation_uuid.value) ||
      batch.creator_tx == 0 || batch.prior_generation == 0 ||
      batch.successor_generation == 0 ||
      batch.successor_generation != batch.prior_generation + 1 ||
      row.local_transaction_id != batch.creator_tx ||
      !SameUuid(row.transaction_uuid.value, batch.transaction_uuid) ||
      batch.events.size() != 4) {
    return refuse("batch_identity_or_generation_invalid");
  }
  storage::DatabaseLocalSecurityBatchEnvelopeV1 carrier;
  carrier.database_uuid = batch.database_uuid;
  carrier.relation_uuid = batch.relation_uuid;
  carrier.transaction_uuid = batch.transaction_uuid;
  carrier.actor_principal_uuid = batch.actor_principal_uuid;
  carrier.creator_local_transaction_id = batch.creator_tx;
  carrier.prior_security_context_generation = batch.prior_generation;
  carrier.successor_security_context_generation = batch.successor_generation;
  carrier.predecessor_page_uuid = batch.predecessor_page_uuid;
  carrier.predecessor_page_number = batch.predecessor_page_number;
  carrier.predecessor_page_generation = batch.predecessor_page_generation;
  carrier.events = batch.events;
  return storage::ValidateDatabaseLocalSecurityLifecycleBatchV1(carrier,
                                                                refusal);
}

DatabaseLocalSecurityEventStoreLoadResultV1 LoadUnlocked(
    const EngineRequestContext& context,
    DatabaseLocalSecurityEventVisibilityV1 visibility,
    bool refuse_concurrent_security_writer,
    LocatorRuntimeState* runtime_out) {
  DatabaseLocalSecurityEventStoreLoadResultV1 result;
  const auto bootstrap = storage::ReadDatabaseBootstrapSecurityCatalog(
      context.database_path);
  if (!bootstrap.ok() || bootstrap.state.security_context_generation == 0) {
    result.diagnostic = bootstrap.ok()
                            ? ErrorDiagnostic(
                                  kDatabaseLocalSecurityDiagnosticCorrupt,
                                  "bootstrap_security_context_generation_missing")
                            : StorageDiagnostic(
                                  bootstrap.diagnostic,
                                  kDatabaseLocalSecurityDiagnosticCorrupt,
                                  "bootstrap_security_catalog_invalid");
    return result;
  }
  const auto expected_database = core_uuid::ParseDurableEngineIdentityUuid(
      UuidKind::database, context.database_uuid.canonical);
  if (!expected_database.ok()) {
    result.diagnostic = ErrorDiagnostic(
        kDatabaseLocalSecurityDiagnosticAuthorityRequired,
        "database_uuid_invalid");
    return result;
  }
  const auto loaded_inventory =
      storage::LoadLocalTransactionInventoryFromDatabase(context.database_path);
  if (!loaded_inventory.ok()) {
    result.diagnostic = StorageDiagnostic(
        loaded_inventory.diagnostic, kDatabaseLocalSecurityDiagnosticCorrupt,
        "transaction_inventory_invalid");
    return result;
  }

  mga::VisibilitySnapshot reader_visibility;
  mga::TransactionInventoryEntry reader;
  if (visibility ==
      DatabaseLocalSecurityEventVisibilityV1::include_reader_own_uncommitted) {
    if (!ExactTransactionIdentity(context, loaded_inventory.inventory, true,
                                  &reader, &result.diagnostic)) {
      return result;
    }
    const auto snapshot = mga::CreateLocalTransactionSnapshot(
        loaded_inventory.inventory, reader.identity.local_id);
    if (!snapshot.ok()) {
      result.diagnostic = StorageDiagnostic(
          snapshot.diagnostic, kDatabaseLocalSecurityDiagnosticCorrupt,
          "reader_visibility_snapshot_invalid");
      return result;
    }
    reader_visibility = snapshot.visibility_snapshot;
  }

  disk::FileDevice device;
  const auto opened = device.Open(
      context.database_path, disk::FileOpenMode::open_existing_read_only);
  if (!opened.ok()) {
    result.diagnostic = StorageDiagnostic(
        opened.diagnostic, kDatabaseLocalSecurityDiagnosticCorrupt,
        "database_open_failed");
    return result;
  }
  disk::SerializedDatabaseHeader serialized_header{};
  const auto header_read =
      device.ReadAt(0, serialized_header.data(), serialized_header.size());
  if (!header_read.ok()) {
    result.diagnostic = StorageDiagnostic(
        header_read.diagnostic, kDatabaseLocalSecurityDiagnosticCorrupt,
        "database_header_read_failed");
    return result;
  }
  const auto parsed_header = disk::ParseDatabaseHeader(serialized_header);
  if (!parsed_header.ok() ||
      parsed_header.header.database_uuid != expected_database.value.value ||
      (context.database_page_size_bytes != 0 &&
       context.database_page_size_bytes != parsed_header.header.page_size)) {
    result.diagnostic = ErrorDiagnostic(
        kDatabaseLocalSecurityDiagnosticCorrupt,
        "database_header_identity_mismatch");
    return result;
  }
  const auto size = device.Size();
  if (!size.ok() || size.size_bytes == 0 ||
      size.size_bytes % parsed_header.header.page_size != 0) {
    result.diagnostic = ErrorDiagnostic(
        kDatabaseLocalSecurityDiagnosticCorrupt,
        "database_size_alignment_invalid");
    return result;
  }
  const std::uint64_t page_count =
      size.size_bytes / parsed_header.header.page_size;
  if (page_count <= storage::kDatabaseLocalPrivateSecurityRootPageNumberV1) {
    result.diagnostic = ErrorDiagnostic(
        kDatabaseLocalSecurityDiagnosticCorrupt,
        "database_core_pages_missing");
    return result;
  }
  const auto inventory_offset = disk::CheckDevicePageOffset(
      parsed_header.header.page_size, storage::kTransactionInventoryPageNumber);
  disk::SerializedPageHeader inventory_header_bytes{};
  if (!inventory_offset.ok() ||
      !device
           .ReadAt(inventory_offset.offset, inventory_header_bytes.data(),
                   inventory_header_bytes.size())
           .ok()) {
    result.diagnostic = ErrorDiagnostic(
        kDatabaseLocalSecurityDiagnosticCorrupt,
        "transaction_inventory_page_header_read_failed");
    return result;
  }
  const auto inventory_header = disk::ParsePageHeader(inventory_header_bytes);
  if (!inventory_header.ok() ||
      inventory_header.header.page_type !=
          disk::PageType::transaction_inventory ||
      inventory_header.header.database_uuid != parsed_header.header.database_uuid) {
    result.diagnostic = ErrorDiagnostic(
        kDatabaseLocalSecurityDiagnosticCorrupt,
        "transaction_inventory_page_identity_invalid");
    return result;
  }

  storage::DatabaseLocalPrivateSecurityLocatorInspectRequestV1 inspect_request;
  inspect_request.device = &device;
  inspect_request.marker_bytes =
      parsed_header.header.database_local_private_relation_locator_marker;
  inspect_request.expected_database_uuid = expected_database.value;
  inspect_request.page_size = parsed_header.header.page_size;
  const auto inspected =
      storage::InspectDatabaseLocalPrivateSecurityLocatorV1(inspect_request);
  if (!inspected.ok() ||
      inspected.classification !=
          storage::DatabaseLocalPrivateSecurityLocatorClassV1::sealed_current) {
    result.diagnostic = inspected.ok()
                            ? ErrorDiagnostic(
                                  kDatabaseLocalSecurityDiagnosticCorrupt,
                                  "sealed_locator_required")
                            : StorageDiagnostic(
                                  inspected.diagnostic,
                                  kDatabaseLocalSecurityDiagnosticCorrupt,
                                  "private_locator_invalid");
    return result;
  }

  if (refuse_concurrent_security_writer) {
    for (std::uint32_t slot = 0; slot < 2; ++slot) {
      if (inspected.locator_zero[slot]) {
        continue;
      }
      if (!inspected.locator_shape_valid[slot] ||
          !inspected.locator_digest_valid[slot] ||
          !inspected.locator_context_valid[slot]) {
        result.diagnostic = ErrorDiagnostic(
            kDatabaseLocalSecurityDiagnosticCorrupt,
            "nonzero_malformed_private_locator_candidate");
        return result;
      }
      if (inspected.locators[slot].locator_generation <= 1) continue;
      const auto& candidate = inspected.locators[slot];
      const auto creator = mga::LookupLocalTransaction(
          loaded_inventory.inventory,
          mga::MakeLocalTransactionId(
              candidate.creator_local_transaction_id));
      if (!creator.ok() ||
          creator.entry.identity.transaction_uuid.kind !=
              UuidKind::transaction ||
          creator.entry.identity.transaction_uuid.value !=
              candidate.creator_transaction_uuid) {
        result.diagnostic = ErrorDiagnostic(
            kDatabaseLocalSecurityDiagnosticCorrupt,
            "locator_creator_inventory_identity_invalid");
        return result;
      }
      const bool writer_in_flight =
          creator.entry.state == mga::TransactionState::active ||
          creator.entry.state == mga::TransactionState::preparing ||
          creator.entry.state == mga::TransactionState::prepared ||
          creator.entry.state == mga::TransactionState::committing;
      const bool reader_is_creator =
          visibility == DatabaseLocalSecurityEventVisibilityV1::
                            include_reader_own_uncommitted &&
          reader.identity.local_id.value ==
              candidate.creator_local_transaction_id &&
          reader.identity.transaction_uuid.value ==
              candidate.creator_transaction_uuid;
      if (writer_in_flight && !reader_is_creator) {
        result.diagnostic = ErrorDiagnostic(
            kDatabaseLocalSecurityDiagnosticTransactionRequired,
            "concurrent_security_authority_transaction_active");
        return result;
      }
    }
  }

  TypedUuid filespace_uuid;
  filespace_uuid.kind = UuidKind::filespace;
  filespace_uuid.value = inventory_header.header.filespace_uuid;
  storage::DatabaseLocalPrivateSecurityLocatorVisibilityRequestV1
      visibility_request;
  visibility_request.inspected = &inspected;
  visibility_request.inventory = &loaded_inventory.inventory;
  visibility_request.visibility_snapshot = reader_visibility;
  visibility_request.use_latest_committed_snapshot =
      visibility == DatabaseLocalSecurityEventVisibilityV1::latest_committed;
  if (!visibility_request.use_latest_committed_snapshot) {
    visibility_request.reader_transaction_uuid =
        reader.identity.transaction_uuid;
    visibility_request.reader_local_transaction_id = reader.identity.local_id;
  }
  visibility_request.transaction_inventory_filespace_uuid = filespace_uuid;
  const auto selected =
      storage::SelectDatabaseLocalPrivateSecurityLocatorForVisibilityV1(
          visibility_request);
  if (!selected.ok()) {
    result.diagnostic = StorageDiagnostic(
        selected.diagnostic, kDatabaseLocalSecurityDiagnosticCorrupt,
        "private_locator_visibility_invalid");
    return result;
  }

  const TypedUuid relation_uuid = SecurityRelationUuid();
  const auto& locator = selected.selected_locator;
  if (locator.chain_page_count > page_count ||
      locator.chain_page_count >
          static_cast<std::uint64_t>(
              std::numeric_limits<std::size_t>::max())) {
    result.diagnostic = ErrorDiagnostic(
        kDatabaseLocalSecurityDiagnosticCorrupt,
        "security_chain_count_extent_invalid");
    return result;
  }
  std::vector<ParsedBatch> reverse_batches;
  std::vector<std::uint64_t> reverse_page_numbers;
  reverse_batches.reserve(static_cast<std::size_t>(locator.chain_page_count));
  reverse_page_numbers.reserve(
      static_cast<std::size_t>(locator.chain_page_count));
  std::set<std::uint64_t> visited_pages;
  Uuid expected_page_uuid = locator.head_page_uuid;
  std::uint64_t expected_page_number = locator.head_page_number;
  std::uint64_t expected_page_generation = locator.head_page_generation;
  std::uint64_t observed_extent_min = std::numeric_limits<std::uint64_t>::max();
  std::uint64_t observed_extent_max = 0;
  for (std::uint64_t chain_index = 0;
       chain_index < locator.chain_page_count; ++chain_index) {
    if (expected_page_number <
            storage::kDatabaseLocalPrivateSecurityFirstDataPageNumberV1 ||
        !visited_pages.insert(expected_page_number).second) {
      result.diagnostic = ErrorDiagnostic(
          kDatabaseLocalSecurityDiagnosticCorrupt,
          "security_reverse_chain_cycle_or_page_invalid");
      return result;
    }
    const auto page_offset = disk::CheckDevicePageOffset(
        parsed_header.header.page_size, expected_page_number);
    disk::SerializedPageHeader page_header_bytes{};
    if (!page_offset.ok() ||
        !device
             .ReadAt(page_offset.offset, page_header_bytes.data(),
                     page_header_bytes.size())
             .ok()) {
      result.diagnostic = ErrorDiagnostic(
          kDatabaseLocalSecurityDiagnosticCorrupt,
          "security_chain_page_header_read_failed");
      return result;
    }
    const auto page_header = disk::ParsePageHeader(page_header_bytes);
    if (!page_header.ok() ||
        page_header.header.page_type != disk::PageType::row_data ||
        page_header.header.database_uuid != parsed_header.header.database_uuid ||
        page_header.header.filespace_uuid != filespace_uuid.value ||
        page_header.header.page_uuid != expected_page_uuid ||
        page_header.header.page_number != expected_page_number ||
        page_header.header.page_generation != expected_page_generation) {
      result.diagnostic = ErrorDiagnostic(
          kDatabaseLocalSecurityDiagnosticCorrupt,
          "security_chain_page_identity_invalid");
      return result;
    }

    storage::PhysicalMgaCowReadRequest request;
    request.database_path = context.database_path;
    request.relation_uuid = relation_uuid;
    request.page_number = expected_page_number;
    request.use_latest_committed_snapshot =
        visibility == DatabaseLocalSecurityEventVisibilityV1::latest_committed;
    if (!request.use_latest_committed_snapshot) {
      request.visibility_snapshot = reader_visibility;
    }
    const auto read = storage::ReadPhysicalMgaCowRows(request);
    if (!read.ok() || read.recovery_required_count != 0) {
      result.diagnostic = read.ok()
                              ? ErrorDiagnostic(
                                    kDatabaseLocalSecurityDiagnosticCorrupt,
                                    "security_row_recovery_required")
                              : StorageDiagnostic(
                                    read.diagnostic,
                                    kDatabaseLocalSecurityDiagnosticCorrupt,
                                    "security_row_read_failed");
      return result;
    }
    if (read.rows.size() != 1 || read.visible_rows.size() != 1 ||
        !read.rows[0].visible || read.rows[0].visible_delete_marker ||
        read.visible_delete_marker_count != 0 ||
        read.wait_for_transaction_count != 0 ||
        read.rolled_back_version_count != 0 ||
        read.recovery_required_count != 0) {
      result.diagnostic = ErrorDiagnostic(
          kDatabaseLocalSecurityDiagnosticCorrupt,
          "security_chain_exact_physical_row_cardinality_invalid");
      return result;
    }
    const auto& row = read.visible_rows[0];
    if (row.cells.size() != 1 || row.cells[0].column_ordinal != 1 ||
        row.cells[0].value.type_id != CanonicalTypeId::binary ||
        row.cells[0].value.is_null ||
        row.cells[0].value.payload_is_toast_reference) {
      result.diagnostic = ErrorDiagnostic(
          kDatabaseLocalSecurityDiagnosticCorrupt,
          "security_row_cell_shape_invalid");
      return result;
    }
    ParsedBatch batch;
    batch.page_number = expected_page_number;
    batch.outer_page_uuid = page_header.header.page_uuid;
    batch.outer_page_generation = page_header.header.page_generation;
    batch.row_uuid = row.row_uuid;
    std::string refusal;
    if (!DecodeBatch(row.cells[0].value.payload, &batch, &refusal) ||
        !ValidateDecodedBatch(batch, parsed_header.header.database_uuid,
                              relation_uuid, row, &refusal) ||
        read.row_page.next_page_number != batch.predecessor_page_number) {
      result.diagnostic = ErrorDiagnostic(
          kDatabaseLocalSecurityDiagnosticCorrupt,
          refusal.empty() ? "security_reverse_link_invalid"
                          : std::move(refusal));
      return result;
    }
    const bool final_page = chain_index + 1 == locator.chain_page_count;
    if (final_page != batch.predecessor_page_uuid.is_nil()) {
      result.diagnostic = ErrorDiagnostic(
          kDatabaseLocalSecurityDiagnosticCorrupt,
          "security_chain_tail_predecessor_invalid");
      return result;
    }
    reverse_page_numbers.push_back(expected_page_number);
    reverse_batches.push_back(std::move(batch));
    observed_extent_min =
        std::min(observed_extent_min, expected_page_number);
    observed_extent_max =
        std::max(observed_extent_max, expected_page_number);
    expected_page_uuid = reverse_batches.back().predecessor_page_uuid;
    expected_page_number =
        reverse_batches.back().predecessor_page_number;
    expected_page_generation =
        reverse_batches.back().predecessor_page_generation;
  }

  if (locator.chain_page_count == 0) {
    observed_extent_min = 0;
  } else if (reverse_page_numbers.back() != locator.tail_page_number ||
             reverse_batches.back().page_number != locator.tail_page_number ||
             reverse_batches.back().outer_page_uuid != locator.tail_page_uuid ||
             reverse_batches.back().outer_page_generation !=
                 locator.tail_page_generation) {
    result.diagnostic = ErrorDiagnostic(
        kDatabaseLocalSecurityDiagnosticCorrupt,
        "security_chain_tail_identity_invalid");
    return result;
  }
  if (observed_extent_min != locator.extent_min_page ||
      observed_extent_max != locator.extent_max_page) {
    result.diagnostic = ErrorDiagnostic(
        kDatabaseLocalSecurityDiagnosticCorrupt,
        "security_chain_extent_invalid");
    return result;
  }

  std::reverse(reverse_batches.begin(), reverse_batches.end());
  std::reverse(reverse_page_numbers.begin(), reverse_page_numbers.end());
  std::uint64_t generation = bootstrap.state.security_context_generation;
  for (const auto& batch : reverse_batches) {
    if (generation == std::numeric_limits<std::uint64_t>::max() ||
        batch.prior_generation != generation ||
        batch.successor_generation != generation + 1) {
      result.diagnostic = ErrorDiagnostic(
          kDatabaseLocalSecurityDiagnosticCorrupt,
          "security_context_successor_chain_invalid");
      return result;
    }
    result.state.events.insert(result.state.events.end(), batch.events.begin(),
                               batch.events.end());
    generation = batch.successor_generation;
  }
  if (generation != locator.security_context_generation ||
      (!reverse_batches.empty() && locator.locator_generation > 1 &&
       (locator.creator_local_transaction_id !=
            reverse_batches.back().creator_tx ||
        locator.creator_transaction_uuid !=
            reverse_batches.back().transaction_uuid))) {
    result.diagnostic = ErrorDiagnostic(
        kDatabaseLocalSecurityDiagnosticCorrupt,
        "locator_head_batch_authority_mismatch");
    return result;
  }
  const auto closed = device.Close();
  if (!closed.ok()) {
    result.diagnostic = StorageDiagnostic(
        closed.diagnostic, kDatabaseLocalSecurityDiagnosticCorrupt,
        "database_close_failed");
    return result;
  }

  result.ok = true;
  result.diagnostic = OkDiagnostic();
  result.state.security_context_generation = generation;
  result.state.bootstrap_authority.authenticated = true;
  result.state.bootstrap_authority.principal_present =
      bootstrap.state.present;
  if (bootstrap.state.principal_uuid.valid()) {
    result.state.bootstrap_authority.principal_uuid =
        core_uuid::UuidToString(bootstrap.state.principal_uuid.value);
  }
  result.state.bootstrap_authority.principal_name =
      bootstrap.state.principal_name;
  result.state.bootstrap_authority.credential_fingerprint =
      bootstrap.state.credential_fingerprint;
  if (bootstrap.state.sysarch_role_uuid.valid()) {
    result.state.bootstrap_authority.sysarch_role_uuid =
        core_uuid::UuidToString(bootstrap.state.sysarch_role_uuid.value);
  }
  if (bootstrap.state.membership_uuid.valid()) {
    result.state.bootstrap_authority.membership_uuid =
        core_uuid::UuidToString(bootstrap.state.membership_uuid.value);
  }
  result.state.bootstrap_authority.creator_tx = bootstrap.state.creator_tx;
  result.state.bootstrap_authority.policy_generation =
      bootstrap.state.policy_generation;
  result.state.bootstrap_authority.security_context_generation =
      bootstrap.state.security_context_generation;
  result.state.page_numbers = reverse_page_numbers;
  result.state.database_identity_authenticated = true;
  result.state.page_identity_authenticated = true;
  result.state.durable_inventory_authenticated = true;
  result.state.locator_page_reads = inspected.metrics.locator_page_reads;
  result.state.security_chain_page_reads = locator.chain_page_count;
  result.state.legacy_scan_page_reads = 0;
  result.state.locator_migration_count =
      inspected.metrics.locator_migration_count;
  result.evidence = {
      "database_local_security.private_api=true",
      "database_local_security.public_profile_evidence=false",
      "database_local_security.database_identity_authenticated=true",
      "database_local_security.page_identity_authenticated=true",
      "database_local_security.inventory_visibility_authenticated=true",
      "database_local_security.locator_page_reads=" +
          std::to_string(result.state.locator_page_reads),
      "database_local_security.security_chain_page_reads=" +
          std::to_string(result.state.security_chain_page_reads),
      "database_local_security.legacy_scan_page_reads=0",
      "database_local_security.locator_migration_count=" +
          std::to_string(result.state.locator_migration_count),
      "database_local_security.security_context_generation=" +
          std::to_string(generation),
  };
  if (runtime_out != nullptr) {
    runtime_out->serialized_header = serialized_header;
    runtime_out->marker_bytes = inspect_request.marker_bytes;
    runtime_out->inspected = inspected;
    runtime_out->selected = selected;
    runtime_out->inventory = loaded_inventory.inventory;
    runtime_out->database_uuid = expected_database.value;
    runtime_out->filespace_uuid = filespace_uuid;
    runtime_out->page_size = parsed_header.header.page_size;
    runtime_out->next_page_number = page_count;
  }
  return result;
}

}  // namespace

DatabaseLocalSecurityEventStoreLoadResultV1
LoadDatabaseLocalSecurityEventStoreV1(
    const EngineRequestContext& context,
    DatabaseLocalSecurityEventVisibilityV1 visibility) {
  if (context.database_path.empty()) {
    DatabaseLocalSecurityEventStoreLoadResultV1 result;
    result.diagnostic = ErrorDiagnostic(
        kDatabaseLocalSecurityDiagnosticAuthorityRequired,
        "database_path_required");
    return result;
  }
  const auto inventory_guard =
      AcquireTransactionInventoryGuard(context.database_path);
  return LoadUnlocked(context, visibility, false, nullptr);
}

DatabaseLocalSecurityEventStoreAppendResultV1
AppendDatabaseLocalSecurityEventBatchV1(
    const EngineRequestContext& context,
    std::span<const std::string> unsealed_events) {
  DatabaseLocalSecurityEventStoreAppendResultV1 result;
  if (!context.security_context_present ||
      !HasTraceTag(context,
                   kDatabaseLocalSecurityLifecycleBootstrapAuthorityTagV1)) {
    result.diagnostic = ErrorDiagnostic(
        kDatabaseLocalSecurityDiagnosticAuthorityRequired,
        "authenticated_engine_private_lifecycle_tag_required");
    return result;
  }
  const auto actor = core_uuid::ParseDurableEngineIdentityUuid(
      UuidKind::principal, context.principal_uuid.canonical);
  if (!actor.ok()) {
    result.diagnostic = ErrorDiagnostic(
        kDatabaseLocalSecurityDiagnosticAuthorityRequired,
        "actor_principal_uuid_invalid");
    return result;
  }
  if (context.database_path.empty()) {
    result.diagnostic = ErrorDiagnostic(
        kDatabaseLocalSecurityDiagnosticAuthorityRequired,
        "database_path_required");
    return result;
  }

  const auto inventory_guard =
      AcquireTransactionInventoryGuard(context.database_path);
  const auto loaded_inventory =
      storage::LoadLocalTransactionInventoryFromDatabase(context.database_path);
  if (!loaded_inventory.ok()) {
    result.diagnostic = StorageDiagnostic(
        loaded_inventory.diagnostic, kDatabaseLocalSecurityDiagnosticCorrupt,
        "transaction_inventory_invalid");
    return result;
  }
  mga::TransactionInventoryEntry transaction;
  if (!ExactTransactionIdentity(context, loaded_inventory.inventory, true,
                                &transaction, &result.diagnostic)) {
    return result;
  }
  LocatorRuntimeState locator_runtime;
  const auto loaded = LoadUnlocked(
      context,
      DatabaseLocalSecurityEventVisibilityV1::include_reader_own_uncommitted,
      true, &locator_runtime);
  if (!loaded.ok) {
    result.diagnostic = loaded.diagnostic;
    return result;
  }
  if (loaded.state.security_context_generation ==
      std::numeric_limits<std::uint64_t>::max()) {
    result.diagnostic = ErrorDiagnostic(
        kDatabaseLocalSecurityDiagnosticCorrupt,
        "security_context_generation_exhausted");
    return result;
  }
  if (context.authorization_context.present &&
      (context.authorization_context.principal_uuid.canonical !=
           context.principal_uuid.canonical ||
       context.authorization_context.security_context_generation !=
           loaded.state.security_context_generation)) {
    result.diagnostic = ErrorDiagnostic(
        kDatabaseLocalSecurityDiagnosticAuthorityRequired,
        "materialized_authorization_context_stale_or_mismatched");
    return result;
  }

  std::vector<std::string> events(unsealed_events.begin(),
                                  unsealed_events.end());
  const std::uint64_t successor_generation =
      loaded.state.security_context_generation + 1;
  std::string refusal;
  if (!ValidateUnsealedEvents(events, context.local_transaction_id,
                              context.principal_uuid.canonical,
                              successor_generation, &refusal)) {
    result.diagnostic = ErrorDiagnostic(
        kDatabaseLocalSecurityDiagnosticBatchInvalid, std::move(refusal));
    return result;
  }
  events.push_back(SuccessorEvent(context.local_transaction_id,
                                  successor_generation, events));

  const TypedUuid relation_uuid = SecurityRelationUuid();
  const auto row_uuid = core_uuid::GenerateDurableEngineIdentityV7(
      UuidKind::row,
      static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::system_clock::now().time_since_epoch())
              .count()) +
          successor_generation);
  if (!row_uuid.ok()) {
    result.diagnostic = StorageDiagnostic(
        row_uuid.diagnostic, kDatabaseLocalSecurityDiagnosticWriteFailed,
        "security_batch_row_uuid_generation_failed");
    return result;
  }
  ParsedBatch batch;
  batch.database_uuid = locator_runtime.database_uuid.value;
  batch.relation_uuid = relation_uuid.value;
  batch.transaction_uuid = transaction.identity.transaction_uuid.value;
  batch.actor_principal_uuid = actor.value.value;
  batch.creator_tx = context.local_transaction_id;
  batch.prior_generation = loaded.state.security_context_generation;
  batch.successor_generation = successor_generation;
  batch.predecessor_page_uuid =
      locator_runtime.selected.selected_locator.head_page_uuid;
  batch.predecessor_page_number =
      locator_runtime.selected.selected_locator.head_page_number;
  batch.predecessor_page_generation =
      locator_runtime.selected.selected_locator.head_page_generation;
  batch.events = events;
  const auto encoded = EncodeBatch(batch);
  if (encoded.empty()) {
    result.diagnostic = ErrorDiagnostic(
        kDatabaseLocalSecurityDiagnosticBatchInvalid,
        "security_batch_encode_failed");
    return result;
  }

  page::RowDataCell cell;
  cell.column_ordinal = 1;
  cell.value.type_id = CanonicalTypeId::binary;
  cell.value.payload = encoded;

  storage::PhysicalMgaCowMutationRequest mutation;
  mutation.database_path = context.database_path;
  mutation.relation_uuid = relation_uuid;
  mutation.row_uuid = row_uuid.value;
  mutation.transaction_uuid = transaction.identity.transaction_uuid;
  mutation.existing_local_transaction_id = transaction.identity.local_id;
  mutation.use_existing_transaction = true;
  mutation.kind = storage::PhysicalMgaCowMutationKind::insert;
  mutation.page_number = locator_runtime.next_page_number;
  mutation.predecessor_page_number = batch.predecessor_page_number;
  mutation.begin_unix_epoch_millis = transaction.begin_unix_epoch_millis;
  mutation.stable_slot_id = 1;
  mutation.cells.push_back(std::move(cell));
  const auto written = storage::WritePhysicalMgaCowUnpublishedMutation(mutation);
  if (!written.ok()) {
    result.diagnostic = StorageDiagnostic(
        written.diagnostic, kDatabaseLocalSecurityDiagnosticWriteFailed,
        "security_batch_page_write_failed");
    return result;
  }

  storage::DatabaseLocalPrivateSecurityLocatorSuccessorRequestV1 publication;
  publication.database_path = context.database_path;
  publication.marker_bytes = locator_runtime.marker_bytes;
  publication.expected_database_uuid = locator_runtime.database_uuid;
  publication.page_size = locator_runtime.page_size;
  publication.selected_prior = locator_runtime.selected.selected_locator;
  publication.creator_transaction_uuid = transaction.identity.transaction_uuid;
  publication.creator_local_transaction_id = context.local_transaction_id;
  publication.head_page_uuid = written.page_uuid;
  publication.head_page_number = mutation.page_number;
  publication.head_page_generation = written.page_generation;
  publication.expected_predecessor_page_number =
      batch.predecessor_page_number;
  publication.security_context_generation = successor_generation;
  if (HasTraceTag(
          context,
          "engine.test.private_security_locator_fault.after_locator")) {
    publication.fault_injection_point = "after_locator";
  } else if (HasTraceTag(
                 context,
                 "engine.test.private_security_locator_fault.after_anchor_copy_0")) {
    publication.fault_injection_point = "after_anchor_copy_0";
  } else if (HasTraceTag(
                 context,
                 "engine.test.private_security_locator_fault.readback_mismatch")) {
    publication.fault_injection_point = "locator_readback_mismatch";
  }
  const auto published =
      storage::PublishDatabaseLocalPrivateSecurityLocatorSuccessorV1(
          publication);
  if (!published.ok()) {
    result.diagnostic = StorageDiagnostic(
        published.diagnostic, kDatabaseLocalSecurityDiagnosticWriteFailed,
        "security_locator_publication_failed");
    return result;
  }

  result.ok = true;
  result.diagnostic = OkDiagnostic();
  result.prior_security_context_generation =
      loaded.state.security_context_generation;
  result.security_context_generation = successor_generation;
  result.page_number = mutation.page_number;
  result.sealed_events = std::move(events);
  result.evidence = written.evidence;
  result.evidence.push_back(
      "database_local_security.locator_successor_published=true");
  result.evidence.push_back("database_local_security.private_api=true");
  result.evidence.push_back(
      "database_local_security.public_profile_evidence=false");
  result.evidence.push_back(
      "database_local_security.exact_active_mga_transaction=true");
  result.evidence.push_back(
      "database_local_security.one_authority_event_batch=true");
  result.evidence.push_back(
      "database_local_security.successor_generation=" +
      std::to_string(successor_generation));
  return result;
}

}  // namespace scratchbird::engine::internal_api
