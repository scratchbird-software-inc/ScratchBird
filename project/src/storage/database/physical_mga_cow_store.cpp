// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "physical_mga_cow_store.hpp"

#include "database_format.hpp"
#include "disk_device.hpp"
#include "local_transaction_store.hpp"
#include "page_header.hpp"
#include "page_manager.hpp"
#include "startup_state.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <utility>

namespace scratchbird::storage::database {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::StoreLittle64;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::UuidKind;
using scratchbird::storage::disk::DiskAccessMode;
using scratchbird::storage::disk::DiskChecksumPolicy;
using scratchbird::storage::disk::DiskDevicePolicy;
using scratchbird::storage::disk::FileDevice;
using scratchbird::storage::disk::FileOpenMode;
using scratchbird::storage::disk::kPageHeaderSerializedBytes;
using scratchbird::storage::disk::PageType;
using scratchbird::storage::disk::ParseDatabaseHeader;
using scratchbird::storage::disk::ParsePageHeader;
using scratchbird::storage::disk::ReadDevicePageHeader;
using scratchbird::storage::disk::SerializedDatabaseHeader;
using scratchbird::storage::disk::UnknownPagePolicy;
using scratchbird::storage::page::BuildManagedPageHeader;
using scratchbird::storage::page::BuildRowDataPageBody;
using scratchbird::storage::page::CheckedPageBodyOffset;
using scratchbird::storage::page::CheckedPageOffset;
using scratchbird::storage::page::ManagedPageHeaderRequest;
using scratchbird::storage::page::PageManagerContext;
using scratchbird::storage::page::ParseRowDataPageBody;
using scratchbird::storage::page::RowDataPageBody;
using scratchbird::storage::page::RowDataRecord;
using scratchbird::transaction::mga::BeginLocalTransaction;
using scratchbird::transaction::mga::CommitLocalTransaction;
using scratchbird::transaction::mga::CopyOnWriteMutationPhase;
using scratchbird::transaction::mga::EvaluateVisibility;
using scratchbird::transaction::mga::kInvalidLocalTransactionId;
using scratchbird::transaction::mga::LocalTransactionId;
using scratchbird::transaction::mga::LocalTransactionInventory;
using scratchbird::transaction::mga::LookupLocalTransaction;
using scratchbird::transaction::mga::MakeLocalTransactionId;
using scratchbird::transaction::mga::PlanLocalCopyOnWriteMutationForTransaction;
using scratchbird::transaction::mga::RollbackLocalTransaction;
using scratchbird::transaction::mga::RowIdentity;
using scratchbird::transaction::mga::RowVersionMetadata;
using scratchbird::transaction::mga::RowVersionState;
using scratchbird::transaction::mga::TransactionInventoryEntry;
using scratchbird::transaction::mga::TransactionState;
using scratchbird::transaction::mga::VisibilityDecision;
using scratchbird::transaction::mga::VisibilitySnapshot;

Status CowStoreOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::storage_page};
}

Status CowStoreErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::storage_page};
}

template <typename Result>
Result ErrorResult(std::string diagnostic_code,
                   std::string message_key,
                   std::string detail = {}) {
  Result result;
  result.status = CowStoreErrorStatus();
  result.diagnostic = MakePhysicalMgaCowDiagnostic(result.status,
                                                   std::move(diagnostic_code),
                                                   std::move(message_key),
                                                   std::move(detail));
  return result;
}

template <typename Result>
Result Propagate(Status status, DiagnosticRecord diagnostic) {
  Result result;
  result.status = status;
  result.diagnostic = std::move(diagnostic);
  return result;
}

struct DatabaseContextResult {
  Status status;
  u32 page_size = 0;
  PageManagerContext page_context;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct BaseRowSelection {
  bool found = false;
  std::size_t index = 0;
  RowDataRecord row;
  u64 max_row_version = 0;
};

bool SameUuid(const TypedUuid& left, const TypedUuid& right) {
  return left.kind == right.kind && left.value == right.value;
}

bool IsTypedEngineIdentity(const TypedUuid& typed, UuidKind kind) {
  return typed.kind == kind &&
         typed.valid() &&
         scratchbird::core::uuid::IsEngineIdentityUuid(typed.value);
}

DiskDevicePolicy ReadWritePolicy(u32 page_size) {
  DiskDevicePolicy policy;
  policy.page_size = page_size;
  policy.access_mode = DiskAccessMode::read_write;
  policy.checksum_policy = DiskChecksumPolicy::require_valid;
  policy.unknown_page_policy = UnknownPagePolicy::reject_all;
  policy.require_open_device = true;
  policy.require_size_alignment = true;
  return policy;
}

DatabaseContextResult LoadDatabaseContext(FileDevice* device) {
  if (device == nullptr) {
    return ErrorResult<DatabaseContextResult>(
        "SB-PHYSICAL-MGA-COW-DEVICE-INVALID",
        "storage.physical_mga_cow.device_invalid");
  }

  SerializedDatabaseHeader header_bytes{};
  const auto read_header =
      device->ReadAt(0, header_bytes.data(), header_bytes.size());
  if (!read_header.ok()) {
    return Propagate<DatabaseContextResult>(read_header.status,
                                            read_header.diagnostic);
  }
  const auto parsed_header = ParseDatabaseHeader(header_bytes);
  if (!parsed_header.ok()) {
    return Propagate<DatabaseContextResult>(parsed_header.status,
                                            parsed_header.diagnostic);
  }

  const auto inventory_header =
      ReadDevicePageHeader(device,
                           parsed_header.header.page_size,
                           kTransactionInventoryPageNumber,
                           ReadWritePolicy(parsed_header.header.page_size));
  if (!inventory_header.ok()) {
    return Propagate<DatabaseContextResult>(inventory_header.status,
                                            inventory_header.diagnostic);
  }
  if (inventory_header.classification.page_type != PageType::transaction_inventory) {
    return ErrorResult<DatabaseContextResult>(
        "SB-PHYSICAL-MGA-COW-INVENTORY-HEADER-INVALID",
        "storage.physical_mga_cow.inventory_header_invalid",
        std::to_string(kTransactionInventoryPageNumber));
  }
  const auto parsed_inventory_header = ParsePageHeader(inventory_header.serialized);
  if (!parsed_inventory_header.ok()) {
    return Propagate<DatabaseContextResult>(parsed_inventory_header.status,
                                            parsed_inventory_header.diagnostic);
  }
  if (!(parsed_inventory_header.header.database_uuid == parsed_header.header.database_uuid)) {
    return ErrorResult<DatabaseContextResult>(
        "SB-PHYSICAL-MGA-COW-DATABASE-UUID-MISMATCH",
        "storage.physical_mga_cow.database_uuid_mismatch");
  }

  DatabaseContextResult result;
  result.status = CowStoreOkStatus();
  result.page_size = parsed_header.header.page_size;
  result.page_context.page_size = parsed_header.header.page_size;
  result.page_context.database_uuid.kind = UuidKind::database;
  result.page_context.database_uuid.value = parsed_header.header.database_uuid;
  result.page_context.filespace_uuid.kind = UuidKind::filespace;
  result.page_context.filespace_uuid.value =
      parsed_inventory_header.header.filespace_uuid;
  result.page_context.cluster_authority_active = false;
  return result;
}

template <typename Result>
Result ValidateCommonRequest(const std::string& path,
                             const TypedUuid& relation_uuid,
                             u64 page_number) {
  if (path.empty()) {
    return ErrorResult<Result>("SB-PHYSICAL-MGA-COW-PATH-REQUIRED",
                               "storage.physical_mga_cow.path_required");
  }
  if (!IsTypedEngineIdentity(relation_uuid, UuidKind::object)) {
    return ErrorResult<Result>(
        "SB-PHYSICAL-MGA-COW-RELATION-UUID-INVALID",
        "storage.physical_mga_cow.relation_uuid_invalid");
  }
  if (page_number < kCatalogOverflowFirstPageNumber) {
    return ErrorResult<Result>(
        "SB-PHYSICAL-MGA-COW-PAGE-NUMBER-RESERVED",
        "storage.physical_mga_cow.page_number_reserved",
        std::to_string(page_number));
  }
  Result result;
  result.status = CowStoreOkStatus();
  return result;
}

PhysicalMgaCowMutationResult ValidateMutationRequest(
    const PhysicalMgaCowMutationRequest& request) {
  auto common =
      ValidateCommonRequest<PhysicalMgaCowMutationResult>(request.database_path,
                                                          request.relation_uuid,
                                                          request.page_number);
  if (!common.ok()) {
    return common;
  }
  if (!IsTypedEngineIdentity(request.row_uuid, UuidKind::row)) {
    return ErrorResult<PhysicalMgaCowMutationResult>(
        "SB-PHYSICAL-MGA-COW-ROW-UUID-INVALID",
        "storage.physical_mga_cow.row_uuid_invalid");
  }
  if (!IsTypedEngineIdentity(request.transaction_uuid, UuidKind::transaction)) {
    return ErrorResult<PhysicalMgaCowMutationResult>(
        "SB-PHYSICAL-MGA-COW-TRANSACTION-UUID-INVALID",
        "storage.physical_mga_cow.transaction_uuid_invalid");
  }
  if (request.use_existing_transaction &&
      !request.existing_local_transaction_id.valid()) {
    return ErrorResult<PhysicalMgaCowMutationResult>(
        "SB-PHYSICAL-MGA-COW-LOCAL-ID-INVALID",
        "storage.physical_mga_cow.local_id_invalid");
  }
  if ((request.kind == PhysicalMgaCowMutationKind::insert ||
       request.kind == PhysicalMgaCowMutationKind::update) &&
      request.cells.empty()) {
    return ErrorResult<PhysicalMgaCowMutationResult>(
        "SB-PHYSICAL-MGA-COW-PAYLOAD-REQUIRED",
        "storage.physical_mga_cow.payload_required");
  }
  return common;
}

scratchbird::transaction::mga::CopyOnWriteMutationKind ToTransactionCowKind(
    PhysicalMgaCowMutationKind kind) {
  switch (kind) {
    case PhysicalMgaCowMutationKind::insert:
      return scratchbird::transaction::mga::CopyOnWriteMutationKind::insert;
    case PhysicalMgaCowMutationKind::update:
      return scratchbird::transaction::mga::CopyOnWriteMutationKind::update;
    case PhysicalMgaCowMutationKind::delete_row:
      return scratchbird::transaction::mga::CopyOnWriteMutationKind::delete_row;
  }
  return scratchbird::transaction::mga::CopyOnWriteMutationKind::unknown;
}

u64 LatestCommittedLocalTransactionId(const LocalTransactionInventory& inventory) {
  u64 latest = kInvalidLocalTransactionId;
  for (const TransactionInventoryEntry& entry : inventory.entries) {
    if ((entry.state == TransactionState::committed ||
         entry.state == TransactionState::archived) &&
        entry.identity.local_id.valid() &&
        entry.identity.local_id.value > latest) {
      latest = entry.identity.local_id.value;
    }
  }
  return latest;
}

VisibilitySnapshot LatestCommittedSnapshot(const LocalTransactionInventory& inventory) {
  VisibilitySnapshot snapshot;
  snapshot.visible_through_local_transaction_id =
      LatestCommittedLocalTransactionId(inventory);
  snapshot.visible_through_local_transaction_id_is_boundary = true;
  snapshot.allow_reader_own_uncommitted = false;
  return snapshot;
}

RowVersionState RowStateForEntry(const RowDataRecord& row,
                                 TransactionState creator_state) {
  if (row.deleted) {
    return RowVersionState::delete_marker;
  }
  switch (creator_state) {
    case TransactionState::committed:
    case TransactionState::archived:
      return RowVersionState::committed;
    case TransactionState::rolled_back:
    case TransactionState::failed_terminal:
      return RowVersionState::rolled_back;
    case TransactionState::prepared:
      return RowVersionState::prepared;
    case TransactionState::limbo:
      return RowVersionState::limbo;
    case TransactionState::recovering:
      return RowVersionState::recovery_required;
    case TransactionState::active:
    case TransactionState::preparing:
    case TransactionState::committing:
    case TransactionState::created:
    case TransactionState::rolling_back:
    case TransactionState::read_only_active:
    case TransactionState::none:
      return RowVersionState::uncommitted;
  }
  return RowVersionState::unknown;
}

RowVersionMetadata MetadataForRow(const RowDataRecord& row,
                                  const TransactionInventoryEntry& entry) {
  RowVersionMetadata metadata;
  metadata.identity.row.row_uuid = row.row_uuid;
  metadata.identity.creator_transaction = entry.identity;
  metadata.identity.version_sequence = row.row_version;
  metadata.chain.previous_version_sequence = row.previous_row_version;
  metadata.chain.next_version_sequence = row.next_row_version;
  metadata.state = RowStateForEntry(row, entry.state);
  metadata.creator_transaction_state = entry.state;
  metadata.payload_present = !row.cells.empty();
  return metadata;
}

PhysicalMgaCowMutationResult ReadRowDataPage(FileDevice* device,
                                             const DatabaseContextResult& context,
                                             const PhysicalMgaCowMutationRequest& request,
                                             RowDataPageBody* body) {
  if (body == nullptr) {
    return ErrorResult<PhysicalMgaCowMutationResult>(
        "SB-PHYSICAL-MGA-COW-ROW-PAGE-INVALID",
        "storage.physical_mga_cow.row_page_invalid");
  }
  const auto page_offset = CheckedPageOffset(context.page_size, request.page_number);
  if (!page_offset.ok()) {
    return Propagate<PhysicalMgaCowMutationResult>(page_offset.status,
                                                   page_offset.diagnostic);
  }
  const auto size = device->Size();
  if (!size.ok()) {
    return Propagate<PhysicalMgaCowMutationResult>(size.status,
                                                   size.diagnostic);
  }
  if (size.size_bytes <= page_offset.offset) {
    body->relation_uuid = request.relation_uuid;
    body->segment_id = 1;
    body->segment_generation = 1;
    body->page_number = request.page_number;
    body->page_generation = 1;
    body->compaction_generation = 1;
    return PhysicalMgaCowMutationResult{CowStoreOkStatus(), {}, {}, {}, *body, {}, {}, {}};
  }
  if (size.size_bytes < page_offset.offset + context.page_size) {
    return ErrorResult<PhysicalMgaCowMutationResult>(
        "SB-PHYSICAL-MGA-COW-ROW-PAGE-PARTIAL",
        "storage.physical_mga_cow.row_page_partial",
        std::to_string(request.page_number));
  }

  const auto header =
      ReadDevicePageHeader(device,
                           context.page_size,
                           request.page_number,
                           ReadWritePolicy(context.page_size));
  if (!header.ok()) {
    return Propagate<PhysicalMgaCowMutationResult>(header.status,
                                                   header.diagnostic);
  }
  if (header.classification.page_type != PageType::row_data) {
    return ErrorResult<PhysicalMgaCowMutationResult>(
        "SB-PHYSICAL-MGA-COW-ROW-PAGE-TYPE-MISMATCH",
        "storage.physical_mga_cow.row_page_type_mismatch",
        std::to_string(request.page_number));
  }
  const auto body_offset = CheckedPageBodyOffset(context.page_size,
                                                 request.page_number,
                                                 kPageHeaderSerializedBytes);
  if (!body_offset.ok()) {
    return Propagate<PhysicalMgaCowMutationResult>(body_offset.status,
                                                   body_offset.diagnostic);
  }
  std::vector<scratchbird::core::platform::byte> serialized(
      context.page_size - kPageHeaderSerializedBytes, 0);
  const auto read_body =
      device->ReadAt(body_offset.offset, serialized.data(), serialized.size());
  if (!read_body.ok()) {
    return Propagate<PhysicalMgaCowMutationResult>(read_body.status,
                                                   read_body.diagnostic);
  }
  const auto parsed = ParseRowDataPageBody(serialized, request.page_number);
  if (!parsed.ok()) {
    return Propagate<PhysicalMgaCowMutationResult>(parsed.status,
                                                   parsed.diagnostic);
  }
  if (!SameUuid(parsed.body.relation_uuid, request.relation_uuid)) {
    return ErrorResult<PhysicalMgaCowMutationResult>(
        "SB-PHYSICAL-MGA-COW-RELATION-MISMATCH",
        "storage.physical_mga_cow.relation_mismatch",
        std::to_string(request.page_number));
  }
  *body = parsed.body;
  return PhysicalMgaCowMutationResult{CowStoreOkStatus(), {}, {}, {}, *body, {}, {}, {}};
}

PhysicalMgaCowReadResult ReadRowDataPageForRead(FileDevice* device,
                                                const DatabaseContextResult& context,
                                                const PhysicalMgaCowReadRequest& request,
                                                RowDataPageBody* body) {
  PhysicalMgaCowMutationRequest mutation_request;
  mutation_request.database_path = request.database_path;
  mutation_request.relation_uuid = request.relation_uuid;
  mutation_request.page_number = request.page_number;
  const auto read = ReadRowDataPage(device, context, mutation_request, body);
  if (!read.ok()) {
    return Propagate<PhysicalMgaCowReadResult>(read.status, read.diagnostic);
  }
  return PhysicalMgaCowReadResult{CowStoreOkStatus(), {}, *body, {}, {}, 0, 0, 0, 0, {}, {}};
}

PhysicalMgaCowMutationResult WriteRowDataPage(FileDevice* device,
                                              const DatabaseContextResult& context,
                                              RowDataPageBody body,
                                              bool sync_after_write = true) {
  body.page_generation = std::max<u64>(1, body.page_generation);
  body.compaction_generation = std::max<u64>(body.compaction_generation,
                                             body.page_generation);
  const auto built = BuildRowDataPageBodyOwned(std::move(body), context.page_size);
  if (!built.ok()) {
    return Propagate<PhysicalMgaCowMutationResult>(built.status,
                                                   built.diagnostic);
  }
  const auto page_uuid = scratchbird::core::uuid::GenerateEngineIdentityV7(
      UuidKind::page,
      1770000000000ull + body.page_number + body.page_generation);
  if (!page_uuid.ok()) {
    return Propagate<PhysicalMgaCowMutationResult>(page_uuid.status,
                                                   page_uuid.diagnostic);
  }
  ManagedPageHeaderRequest header_request;
  header_request.context = context.page_context;
  header_request.page_type = PageType::row_data;
  header_request.page_uuid = page_uuid.value;
  header_request.page_number = body.page_number;
  header_request.page_generation = body.page_generation;
  const auto header = BuildManagedPageHeader(header_request);
  if (!header.ok()) {
    return Propagate<PhysicalMgaCowMutationResult>(header.status,
                                                   header.diagnostic);
  }
  const auto page_offset = CheckedPageOffset(context.page_size, body.page_number);
  if (!page_offset.ok()) {
    return Propagate<PhysicalMgaCowMutationResult>(page_offset.status,
                                                   page_offset.diagnostic);
  }
  const auto write_header = device->WriteAt(page_offset.offset,
                                            header.serialized.data(),
                                            header.serialized.size());
  if (!write_header.ok()) {
    return Propagate<PhysicalMgaCowMutationResult>(write_header.status,
                                                   write_header.diagnostic);
  }
  const auto body_offset = CheckedPageBodyOffset(context.page_size,
                                                 body.page_number,
                                                 kPageHeaderSerializedBytes);
  if (!body_offset.ok()) {
    return Propagate<PhysicalMgaCowMutationResult>(body_offset.status,
                                                   body_offset.diagnostic);
  }
  const auto write_body = device->WriteAt(body_offset.offset,
                                          built.serialized.data(),
                                          built.serialized.size());
  if (!write_body.ok()) {
    return Propagate<PhysicalMgaCowMutationResult>(write_body.status,
                                                   write_body.diagnostic);
  }
  if (sync_after_write) {
    const auto sync = device->Sync();
    if (!sync.ok()) {
      return Propagate<PhysicalMgaCowMutationResult>(sync.status,
                                                     sync.diagnostic);
    }
  }
  PhysicalMgaCowMutationResult result;
  result.status = CowStoreOkStatus();
  result.row_page = built.body;
  return result;
}

BaseRowSelection SelectBaseRow(const RowDataPageBody& body,
                               const LocalTransactionInventory& inventory,
                               const TypedUuid& row_uuid,
                               bool* blocked,
                               DiagnosticRecord* diagnostic) {
  BaseRowSelection selection;
  if (blocked != nullptr) {
    *blocked = false;
  }
  std::vector<std::pair<std::size_t, RowDataRecord>> candidates;
  for (std::size_t index = 0; index < body.rows.size(); ++index) {
    const RowDataRecord& row = body.rows[index];
    if (SameUuid(row.row_uuid, row_uuid)) {
      selection.max_row_version =
          std::max<u64>(selection.max_row_version, row.row_version);
      candidates.push_back({index, row});
    }
  }
  std::sort(candidates.begin(),
            candidates.end(),
            [](const auto& left, const auto& right) {
              return left.second.row_version > right.second.row_version;
            });
  const VisibilitySnapshot snapshot = LatestCommittedSnapshot(inventory);
  for (const auto& candidate : candidates) {
    const auto entry = LookupLocalTransaction(
        inventory,
        MakeLocalTransactionId(candidate.second.local_transaction_id));
    if (!entry.ok()) {
      if (blocked != nullptr) {
        *blocked = true;
      }
      if (diagnostic != nullptr) {
        *diagnostic = entry.diagnostic;
      }
      return selection;
    }
    if (entry.entry.state == TransactionState::active ||
        entry.entry.state == TransactionState::preparing ||
        entry.entry.state == TransactionState::prepared ||
        entry.entry.state == TransactionState::committing) {
      if (blocked != nullptr) {
        *blocked = true;
      }
      if (diagnostic != nullptr) {
        *diagnostic = MakePhysicalMgaCowDiagnostic(
            CowStoreErrorStatus(),
            "SB-PHYSICAL-MGA-COW-BASE-WAITS-FOR-TRANSACTION",
            "storage.physical_mga_cow.base_waits_for_transaction",
            std::to_string(candidate.second.local_transaction_id));
      }
      return selection;
    }
    if (entry.entry.state == TransactionState::rolled_back ||
        entry.entry.state == TransactionState::failed_terminal) {
      continue;
    }
    if (candidate.second.deleted &&
        (entry.entry.state == TransactionState::committed ||
         entry.entry.state == TransactionState::archived) &&
        candidate.second.local_transaction_id <=
            snapshot.visible_through_local_transaction_id) {
      return selection;
    }
    const auto visible = EvaluateVisibility(MetadataForRow(candidate.second,
                                                          entry.entry),
                                            snapshot);
    if (visible.decision == VisibilityDecision::visible) {
      selection.found = true;
      selection.index = candidate.first;
      selection.row = candidate.second;
      return selection;
    }
    if (visible.decision == VisibilityDecision::wait_for_transaction ||
        visible.decision == VisibilityDecision::requires_recovery) {
      if (blocked != nullptr) {
        *blocked = true;
      }
      if (diagnostic != nullptr) {
        *diagnostic = visible.diagnostic;
      }
      return selection;
    }
  }
  return selection;
}

u32 NextStableSlotId(const RowDataPageBody& body) {
  u32 next = 1;
  for (const RowDataRecord& row : body.rows) {
    next = std::max<u32>(next, row.stable_slot_id + 1);
  }
  return next;
}

}  // namespace

const char* PhysicalMgaCowMutationKindName(PhysicalMgaCowMutationKind kind) {
  switch (kind) {
    case PhysicalMgaCowMutationKind::insert: return "insert";
    case PhysicalMgaCowMutationKind::update: return "update";
    case PhysicalMgaCowMutationKind::delete_row: return "delete_row";
  }
  return "unknown";
}

const char* PhysicalMgaCowFinalizeDecisionName(PhysicalMgaCowFinalizeDecision decision) {
  switch (decision) {
    case PhysicalMgaCowFinalizeDecision::commit: return "commit";
    case PhysicalMgaCowFinalizeDecision::rollback: return "rollback";
  }
  return "unknown";
}

PhysicalMgaCowMutationResult WritePhysicalMgaCowUnpublishedMutation(
    const PhysicalMgaCowMutationRequest& request) {
  const auto valid = ValidateMutationRequest(request);
  if (!valid.ok()) {
    return valid;
  }

  FileDevice device;
  const auto open = device.Open(request.database_path, FileOpenMode::open_existing);
  if (!open.ok()) {
    return Propagate<PhysicalMgaCowMutationResult>(open.status, open.diagnostic);
  }
  const auto context = LoadDatabaseContext(&device);
  if (!context.ok()) {
    return Propagate<PhysicalMgaCowMutationResult>(context.status,
                                                   context.diagnostic);
  }
  const auto loaded_inventory =
      LoadLocalTransactionInventoryFromOpenDevice(&device, context.page_size);
  if (!loaded_inventory.ok()) {
    return Propagate<PhysicalMgaCowMutationResult>(loaded_inventory.status,
                                                   loaded_inventory.diagnostic);
  }

  LocalTransactionInventory active_inventory = loaded_inventory.inventory;
  TransactionInventoryEntry active_entry;
  if (request.use_existing_transaction) {
    const auto existing =
        LookupLocalTransaction(active_inventory,
                               request.existing_local_transaction_id);
    if (!existing.ok()) {
      return Propagate<PhysicalMgaCowMutationResult>(existing.status,
                                                     existing.diagnostic);
    }
    if (!(existing.entry.identity.transaction_uuid.value ==
          request.transaction_uuid.value)) {
      return ErrorResult<PhysicalMgaCowMutationResult>(
          "SB-PHYSICAL-MGA-COW-TRANSACTION-UUID-MISMATCH",
          "storage.physical_mga_cow.transaction_uuid_mismatch");
    }
    if (existing.entry.state != TransactionState::active &&
        existing.entry.state != TransactionState::preparing &&
        existing.entry.state != TransactionState::committing) {
      return ErrorResult<PhysicalMgaCowMutationResult>(
          "SB-PHYSICAL-MGA-COW-TRANSACTION-NOT-ACTIVE",
          "storage.physical_mga_cow.transaction_not_active");
    }
    active_entry = existing.entry;
  } else {
    const auto begin = BeginLocalTransaction(loaded_inventory.inventory,
                                             request.transaction_uuid,
                                             request.begin_unix_epoch_millis);
    if (!begin.ok()) {
      return Propagate<PhysicalMgaCowMutationResult>(begin.status, begin.diagnostic);
    }
    const auto persisted_active =
        PersistLocalTransactionInventoryToOpenDevice(&device,
                                                     context.page_size,
                                                     begin.inventory);
    if (!persisted_active.ok()) {
      return Propagate<PhysicalMgaCowMutationResult>(persisted_active.status,
                                                     persisted_active.diagnostic);
    }
    active_inventory = begin.inventory;
    active_entry = begin.entry;
  }

  RowDataPageBody row_page;
  auto read_page = ReadRowDataPage(&device, context, request, &row_page);
  if (!read_page.ok()) {
    return read_page;
  }
  row_page.page_generation += row_page.rows.empty() ? 0 : 1;

  bool blocked = false;
  DiagnosticRecord blocked_diagnostic;
  const BaseRowSelection base = SelectBaseRow(row_page,
                                              active_inventory,
                                              request.row_uuid,
                                              &blocked,
                                              &blocked_diagnostic);
  if (blocked) {
    return Propagate<PhysicalMgaCowMutationResult>(blocked_diagnostic.status,
                                                   blocked_diagnostic);
  }
  if (request.kind == PhysicalMgaCowMutationKind::insert && base.found) {
    return ErrorResult<PhysicalMgaCowMutationResult>(
        "SB-PHYSICAL-MGA-COW-DUPLICATE-VISIBLE-ROW",
        "storage.physical_mga_cow.duplicate_visible_row");
  }
  if ((request.kind == PhysicalMgaCowMutationKind::update ||
       request.kind == PhysicalMgaCowMutationKind::delete_row) &&
      !base.found) {
    return ErrorResult<PhysicalMgaCowMutationResult>(
        "SB-PHYSICAL-MGA-COW-BASE-ROW-REQUIRED",
        "storage.physical_mga_cow.base_row_required",
        PhysicalMgaCowMutationKindName(request.kind));
  }

  const u64 new_sequence = base.max_row_version + 1;
  if (new_sequence == 0 ||
      new_sequence > std::numeric_limits<u32>::max()) {
    return ErrorResult<PhysicalMgaCowMutationResult>(
        "SB-PHYSICAL-MGA-COW-VERSION-SEQUENCE-INVALID",
        "storage.physical_mga_cow.version_sequence_invalid");
  }

  RowIdentity row_identity;
  row_identity.row_uuid = request.row_uuid;
  const auto planned = PlanLocalCopyOnWriteMutationForTransaction(
      active_entry,
      row_identity,
      ToTransactionCowKind(request.kind),
      base.found ? base.row.row_version : 0,
      new_sequence);
  if (!planned.ok()) {
    return Propagate<PhysicalMgaCowMutationResult>(planned.status,
                                                   planned.diagnostic);
  }

  if (base.found) {
    row_page.rows[base.index].next_row_version = new_sequence;
  } else if (base.max_row_version != 0) {
    for (RowDataRecord& row : row_page.rows) {
      if (SameUuid(row.row_uuid, request.row_uuid) &&
          row.row_version == base.max_row_version) {
        row.next_row_version = new_sequence;
      }
    }
  }

  RowDataRecord new_row;
  new_row.row_uuid = request.row_uuid;
  new_row.transaction_uuid = request.transaction_uuid;
  new_row.local_transaction_id = active_entry.identity.local_id.value;
  new_row.stable_slot_id = base.found ? base.row.stable_slot_id
                                      : (request.stable_slot_id == 0
                                             ? NextStableSlotId(row_page)
                                             : request.stable_slot_id);
  new_row.row_version = static_cast<u32>(new_sequence);
  new_row.previous_row_version = base.found ? base.row.row_version
                                            : base.max_row_version;
  new_row.next_row_version = 0;
  new_row.deleted = request.kind == PhysicalMgaCowMutationKind::delete_row;
  if (!new_row.deleted) {
    new_row.cells = request.cells;
  }
  row_page.rows.push_back(new_row);

  const auto written = WriteRowDataPage(&device, context, row_page);
  if (!written.ok()) {
    return written;
  }

  auto mutation = planned.mutation;
  mutation.phase = CopyOnWriteMutationPhase::payload_written_unpublished;
  mutation.evidence_record_written = true;

  PhysicalMgaCowMutationResult result;
  result.status = CowStoreOkStatus();
  result.inventory = active_inventory;
  result.transaction_entry = active_entry;
  result.mutation = mutation;
  result.row_page = written.row_page;
  result.row_version = new_row;
  result.evidence.push_back("physical_mga_cow.row_page_written=true");
  result.evidence.push_back(
      request.use_existing_transaction
          ? "physical_mga_cow.existing_active_transaction_verified=true"
          : "physical_mga_cow.inventory_active_persisted_before_row_page=true");
  result.evidence.push_back("physical_mga_cow.visibility_published_by_inventory=false");
  result.evidence.push_back(std::string("physical_mga_cow.kind=") +
                            PhysicalMgaCowMutationKindName(request.kind));
  return result;
}

PhysicalMgaCowMutationBatchResult WritePhysicalMgaCowUnpublishedMutationBatch(
    const PhysicalMgaCowMutationBatchRequest& request) {
  if (request.mutations.empty()) {
    return ErrorResult<PhysicalMgaCowMutationBatchResult>(
        "SB-PHYSICAL-MGA-COW-BATCH-EMPTY",
        "storage.physical_mga_cow.batch_empty");
  }

  const auto first_valid = ValidateMutationRequest(request.mutations.front());
  if (!first_valid.ok()) {
    return Propagate<PhysicalMgaCowMutationBatchResult>(first_valid.status,
                                                        first_valid.diagnostic);
  }
  const auto& first = request.mutations.front();
  if (!first.use_existing_transaction ||
      !first.existing_local_transaction_id.valid()) {
    return ErrorResult<PhysicalMgaCowMutationBatchResult>(
        "SB-PHYSICAL-MGA-COW-BATCH-EXISTING-TRANSACTION-REQUIRED",
        "storage.physical_mga_cow.batch_existing_transaction_required");
  }

  FileDevice device;
  const auto open = device.Open(first.database_path, FileOpenMode::open_existing);
  if (!open.ok()) {
    return Propagate<PhysicalMgaCowMutationBatchResult>(open.status,
                                                        open.diagnostic);
  }
  const auto context = LoadDatabaseContext(&device);
  if (!context.ok()) {
    return Propagate<PhysicalMgaCowMutationBatchResult>(context.status,
                                                        context.diagnostic);
  }
  const auto loaded_inventory =
      LoadLocalTransactionInventoryFromOpenDevice(&device, context.page_size);
  if (!loaded_inventory.ok()) {
    return Propagate<PhysicalMgaCowMutationBatchResult>(
        loaded_inventory.status,
        loaded_inventory.diagnostic);
  }

  LocalTransactionInventory active_inventory = loaded_inventory.inventory;
  const auto existing =
      LookupLocalTransaction(active_inventory,
                             first.existing_local_transaction_id);
  if (!existing.ok()) {
    return Propagate<PhysicalMgaCowMutationBatchResult>(existing.status,
                                                        existing.diagnostic);
  }
  if (!(existing.entry.identity.transaction_uuid.value ==
        first.transaction_uuid.value)) {
    return ErrorResult<PhysicalMgaCowMutationBatchResult>(
        "SB-PHYSICAL-MGA-COW-TRANSACTION-UUID-MISMATCH",
        "storage.physical_mga_cow.transaction_uuid_mismatch");
  }
  if (existing.entry.state != TransactionState::active &&
      existing.entry.state != TransactionState::preparing &&
      existing.entry.state != TransactionState::committing) {
    return ErrorResult<PhysicalMgaCowMutationBatchResult>(
        "SB-PHYSICAL-MGA-COW-TRANSACTION-NOT-ACTIVE",
        "storage.physical_mga_cow.transaction_not_active");
  }
  const TransactionInventoryEntry active_entry = existing.entry;

  std::map<u64, RowDataPageBody> page_cache;
  std::map<u64, bool> page_empty_on_load;
  std::map<u64, std::set<std::array<scratchbird::core::platform::byte, 16>>>
      page_insert_row_uuids;
  for (const auto& mutation_request : request.mutations) {
    const auto valid = ValidateMutationRequest(mutation_request);
    if (!valid.ok()) {
      return Propagate<PhysicalMgaCowMutationBatchResult>(valid.status,
                                                          valid.diagnostic);
    }
    if (mutation_request.database_path != first.database_path ||
        !(mutation_request.transaction_uuid.value ==
          first.transaction_uuid.value) ||
        !mutation_request.use_existing_transaction ||
        mutation_request.existing_local_transaction_id.value !=
            first.existing_local_transaction_id.value) {
      return ErrorResult<PhysicalMgaCowMutationBatchResult>(
          "SB-PHYSICAL-MGA-COW-BATCH-SCOPE-MISMATCH",
          "storage.physical_mga_cow.batch_scope_mismatch");
    }

    auto page = page_cache.find(mutation_request.page_number);
    if (page == page_cache.end()) {
      RowDataPageBody loaded_page;
      auto read_page = ReadRowDataPage(&device,
                                       context,
                                       mutation_request,
                                       &loaded_page);
      if (!read_page.ok()) {
        return Propagate<PhysicalMgaCowMutationBatchResult>(
            read_page.status,
            read_page.diagnostic);
      }
      if (!loaded_page.rows.empty()) {
        ++loaded_page.page_generation;
      }
      page_empty_on_load.emplace(mutation_request.page_number,
                                 loaded_page.rows.empty());
      page = page_cache.emplace(mutation_request.page_number,
                                std::move(loaded_page)).first;
    } else if (!SameUuid(page->second.relation_uuid,
                         mutation_request.relation_uuid)) {
      return ErrorResult<PhysicalMgaCowMutationBatchResult>(
          "SB-PHYSICAL-MGA-COW-RELATION-MISMATCH",
          "storage.physical_mga_cow.relation_mismatch",
          std::to_string(mutation_request.page_number));
    }

    RowDataPageBody& row_page = page->second;
    const bool page_started_empty =
        page_empty_on_load.find(mutation_request.page_number) !=
            page_empty_on_load.end() &&
        page_empty_on_load[mutation_request.page_number];
    if (page_started_empty &&
        mutation_request.kind == PhysicalMgaCowMutationKind::insert &&
        mutation_request.stable_slot_id != 0) {
      auto& inserted_rows = page_insert_row_uuids[mutation_request.page_number];
      if (!inserted_rows.insert(mutation_request.row_uuid.value.bytes).second) {
        return ErrorResult<PhysicalMgaCowMutationBatchResult>(
            "SB-PHYSICAL-MGA-COW-DUPLICATE-VISIBLE-ROW",
            "storage.physical_mga_cow.duplicate_visible_row");
      }

      RowIdentity row_identity;
      row_identity.row_uuid = mutation_request.row_uuid;
      const auto planned = PlanLocalCopyOnWriteMutationForTransaction(
          active_entry,
          row_identity,
          ToTransactionCowKind(mutation_request.kind),
          0,
          1);
      if (!planned.ok()) {
        return Propagate<PhysicalMgaCowMutationBatchResult>(
            planned.status,
            planned.diagnostic);
      }

      RowDataRecord new_row;
      new_row.row_uuid = mutation_request.row_uuid;
      new_row.transaction_uuid = mutation_request.transaction_uuid;
      new_row.local_transaction_id = active_entry.identity.local_id.value;
      new_row.stable_slot_id = mutation_request.stable_slot_id;
      new_row.row_version = 1;
      new_row.previous_row_version = 0;
      new_row.next_row_version = 0;
      new_row.deleted = false;
      new_row.cells = mutation_request.cells;
      row_page.rows.push_back(std::move(new_row));
      continue;
    }

    bool blocked = false;
    DiagnosticRecord blocked_diagnostic;
    const BaseRowSelection base = SelectBaseRow(row_page,
                                                active_inventory,
                                                mutation_request.row_uuid,
                                                &blocked,
                                                &blocked_diagnostic);
    if (blocked) {
      return Propagate<PhysicalMgaCowMutationBatchResult>(
          blocked_diagnostic.status,
          blocked_diagnostic);
    }
    if (mutation_request.kind == PhysicalMgaCowMutationKind::insert &&
        base.found) {
      return ErrorResult<PhysicalMgaCowMutationBatchResult>(
          "SB-PHYSICAL-MGA-COW-DUPLICATE-VISIBLE-ROW",
          "storage.physical_mga_cow.duplicate_visible_row");
    }
    if ((mutation_request.kind == PhysicalMgaCowMutationKind::update ||
         mutation_request.kind == PhysicalMgaCowMutationKind::delete_row) &&
        !base.found) {
      return ErrorResult<PhysicalMgaCowMutationBatchResult>(
          "SB-PHYSICAL-MGA-COW-BASE-ROW-REQUIRED",
          "storage.physical_mga_cow.base_row_required",
          PhysicalMgaCowMutationKindName(mutation_request.kind));
    }

    const u64 new_sequence = base.max_row_version + 1;
    if (new_sequence == 0 ||
        new_sequence > std::numeric_limits<u32>::max()) {
      return ErrorResult<PhysicalMgaCowMutationBatchResult>(
          "SB-PHYSICAL-MGA-COW-VERSION-SEQUENCE-INVALID",
          "storage.physical_mga_cow.version_sequence_invalid");
    }

    RowIdentity row_identity;
    row_identity.row_uuid = mutation_request.row_uuid;
    const auto planned = PlanLocalCopyOnWriteMutationForTransaction(
        active_entry,
        row_identity,
        ToTransactionCowKind(mutation_request.kind),
        base.found ? base.row.row_version : 0,
        new_sequence);
    if (!planned.ok()) {
      return Propagate<PhysicalMgaCowMutationBatchResult>(
          planned.status,
          planned.diagnostic);
    }

    if (base.found) {
      row_page.rows[base.index].next_row_version = new_sequence;
    } else if (base.max_row_version != 0) {
      for (RowDataRecord& row : row_page.rows) {
        if (SameUuid(row.row_uuid, mutation_request.row_uuid) &&
            row.row_version == base.max_row_version) {
          row.next_row_version = new_sequence;
        }
      }
    }

    RowDataRecord new_row;
    new_row.row_uuid = mutation_request.row_uuid;
    new_row.transaction_uuid = mutation_request.transaction_uuid;
    new_row.local_transaction_id = active_entry.identity.local_id.value;
    new_row.stable_slot_id = base.found
                                 ? base.row.stable_slot_id
                                 : (mutation_request.stable_slot_id == 0
                                        ? NextStableSlotId(row_page)
                                        : mutation_request.stable_slot_id);
    new_row.row_version = static_cast<u32>(new_sequence);
    new_row.previous_row_version = base.found ? base.row.row_version
                                              : base.max_row_version;
    new_row.next_row_version = 0;
    new_row.deleted =
        mutation_request.kind == PhysicalMgaCowMutationKind::delete_row;
    if (!new_row.deleted) {
      new_row.cells = mutation_request.cells;
    }
    row_page.rows.push_back(std::move(new_row));
  }

  PhysicalMgaCowMutationBatchResult result;
  result.status = CowStoreOkStatus();
  for (auto& [page_number, row_page] : page_cache) {
    (void)page_number;
    const auto written = WriteRowDataPage(&device,
                                          context,
                                          std::move(row_page),
                                          false);
    if (!written.ok()) {
      return Propagate<PhysicalMgaCowMutationBatchResult>(written.status,
                                                          written.diagnostic);
    }
    ++result.pages_written;
  }
  if (request.sync_after_batch) {
    const auto sync = device.Sync();
    if (!sync.ok()) {
      return Propagate<PhysicalMgaCowMutationBatchResult>(sync.status,
                                                          sync.diagnostic);
    }
  }
  result.written_rows = static_cast<u64>(request.mutations.size());
  result.evidence.push_back("physical_mga_cow.batch=true");
  result.evidence.push_back("physical_mga_cow.batch_database_open_once=true");
  result.evidence.push_back("physical_mga_cow.batch_inventory_loaded_once=true");
  result.evidence.push_back("physical_mga_cow.batch_sync_after_pages=true");
  result.evidence.push_back("physical_mga_cow.visibility_published_by_inventory=false");
  result.evidence.push_back("physical_mga_cow.batch_written_rows=" +
                            std::to_string(result.written_rows));
  result.evidence.push_back("physical_mga_cow.batch_pages_written=" +
                            std::to_string(result.pages_written));
  return result;
}

PhysicalMgaCowFinalizeResult FinalizePhysicalMgaCowTransaction(
    const PhysicalMgaCowFinalizeRequest& request) {
  if (request.database_path.empty()) {
    return ErrorResult<PhysicalMgaCowFinalizeResult>(
        "SB-PHYSICAL-MGA-COW-PATH-REQUIRED",
        "storage.physical_mga_cow.path_required");
  }
  if (!request.local_transaction_id.valid()) {
    return ErrorResult<PhysicalMgaCowFinalizeResult>(
        "SB-PHYSICAL-MGA-COW-LOCAL-ID-INVALID",
        "storage.physical_mga_cow.local_id_invalid");
  }

  FileDevice device;
  const auto open = device.Open(request.database_path, FileOpenMode::open_existing);
  if (!open.ok()) {
    return Propagate<PhysicalMgaCowFinalizeResult>(open.status, open.diagnostic);
  }
  const auto context = LoadDatabaseContext(&device);
  if (!context.ok()) {
    return Propagate<PhysicalMgaCowFinalizeResult>(context.status,
                                                   context.diagnostic);
  }
  const auto loaded_inventory =
      LoadLocalTransactionInventoryFromOpenDevice(&device, context.page_size);
  if (!loaded_inventory.ok()) {
    return Propagate<PhysicalMgaCowFinalizeResult>(loaded_inventory.status,
                                                   loaded_inventory.diagnostic);
  }

  const auto finalized =
      request.decision == PhysicalMgaCowFinalizeDecision::commit
          ? CommitLocalTransaction(loaded_inventory.inventory,
                                   request.local_transaction_id,
                                   request.final_unix_epoch_millis)
          : RollbackLocalTransaction(loaded_inventory.inventory,
                                     request.local_transaction_id,
                                     request.final_unix_epoch_millis);
  if (!finalized.ok()) {
    return Propagate<PhysicalMgaCowFinalizeResult>(finalized.status,
                                                   finalized.diagnostic);
  }

  const auto persisted =
      PersistLocalTransactionInventoryToOpenDevice(&device,
                                                   context.page_size,
                                                   finalized.inventory);
  if (!persisted.ok()) {
    return Propagate<PhysicalMgaCowFinalizeResult>(persisted.status,
                                                   persisted.diagnostic);
  }

  PhysicalMgaCowFinalizeResult result;
  result.status = CowStoreOkStatus();
  result.inventory = finalized.inventory;
  result.transaction_entry = finalized.entry;
  result.evidence.push_back("physical_mga_cow.visibility_published_by_inventory=true");
  result.evidence.push_back(std::string("physical_mga_cow.finalize=") +
                            PhysicalMgaCowFinalizeDecisionName(request.decision));
  result.evidence.push_back("physical_mga_cow.row_page_rewrite_for_finality=false");
  return result;
}

PhysicalMgaCowReadResult ReadPhysicalMgaCowRows(
    const PhysicalMgaCowReadRequest& request) {
  auto common =
      ValidateCommonRequest<PhysicalMgaCowReadResult>(request.database_path,
                                                      request.relation_uuid,
                                                      request.page_number);
  if (!common.ok()) {
    return common;
  }

  FileDevice device;
  const auto open = device.Open(request.database_path,
                                FileOpenMode::open_existing_read_only);
  if (!open.ok()) {
    return Propagate<PhysicalMgaCowReadResult>(open.status, open.diagnostic);
  }
  const auto context = LoadDatabaseContext(&device);
  if (!context.ok()) {
    return Propagate<PhysicalMgaCowReadResult>(context.status,
                                               context.diagnostic);
  }
  const auto loaded_inventory =
      LoadLocalTransactionInventoryFromOpenDevice(&device, context.page_size);
  if (!loaded_inventory.ok()) {
    return Propagate<PhysicalMgaCowReadResult>(loaded_inventory.status,
                                               loaded_inventory.diagnostic);
  }

  RowDataPageBody row_page;
  const auto read_page = ReadRowDataPageForRead(&device, context, request, &row_page);
  if (!read_page.ok()) {
    return read_page;
  }

  VisibilitySnapshot snapshot = request.visibility_snapshot;
  if (request.use_latest_committed_snapshot) {
    snapshot = LatestCommittedSnapshot(loaded_inventory.inventory);
  }

  PhysicalMgaCowReadResult result;
  result.status = CowStoreOkStatus();
  result.inventory = loaded_inventory.inventory;
  result.row_page = row_page;
  result.evidence.push_back("physical_mga_cow.read_visibility_authority=durable_transaction_inventory");
  result.evidence.push_back("physical_mga_cow.row_page_finality_authority=false");

  std::map<std::string, std::vector<RowDataRecord>> by_row;
  for (const RowDataRecord& row : row_page.rows) {
    by_row[scratchbird::core::uuid::UuidToString(row.row_uuid.value)].push_back(row);
  }
  for (auto& entry : by_row) {
    std::vector<RowDataRecord>& versions = entry.second;
    std::sort(versions.begin(), versions.end(), [](const RowDataRecord& left,
                                                   const RowDataRecord& right) {
      return left.row_version > right.row_version;
    });
    for (const RowDataRecord& row : versions) {
      const auto creator = LookupLocalTransaction(
          loaded_inventory.inventory,
          MakeLocalTransactionId(row.local_transaction_id));
      if (!creator.ok()) {
        ++result.recovery_required_count;
        continue;
      }
      PhysicalMgaCowReadRow observed;
      observed.row = row;
      observed.metadata = MetadataForRow(row, creator.entry);
      observed.decision = EvaluateVisibility(observed.metadata, snapshot).decision;
      if (creator.entry.state == TransactionState::rolled_back ||
          creator.entry.state == TransactionState::failed_terminal) {
        ++result.rolled_back_version_count;
        result.rows.push_back(std::move(observed));
        continue;
      }
      if (row.deleted &&
          (creator.entry.state == TransactionState::committed ||
           creator.entry.state == TransactionState::archived) &&
          row.local_transaction_id <= snapshot.visible_through_local_transaction_id) {
        observed.visible_delete_marker = true;
        ++result.visible_delete_marker_count;
        result.rows.push_back(std::move(observed));
        break;
      }
      if (observed.decision == VisibilityDecision::visible) {
        observed.visible = true;
        result.visible_rows.push_back(row);
        result.rows.push_back(std::move(observed));
        break;
      }
      if (observed.decision == VisibilityDecision::wait_for_transaction) {
        ++result.wait_for_transaction_count;
      } else if (observed.decision == VisibilityDecision::requires_recovery) {
        ++result.recovery_required_count;
      }
      result.rows.push_back(std::move(observed));
    }
  }
  return result;
}

DiagnosticRecord MakePhysicalMgaCowDiagnostic(Status status,
                                              std::string diagnostic_code,
                                              std::string message_key,
                                              std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", detail});
  }
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "storage.database.physical_mga_cow");
}

}  // namespace scratchbird::storage::database
