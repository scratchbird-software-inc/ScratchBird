// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "physical_mga_cow_store.hpp"
#include "catalog_schema_definition.hpp"
#include "catalog_metric_retention_policy.hpp"
#include "catalog_metric_descriptor.hpp"
#include "catalog_metric_label_schema.hpp"
#include "catalog_metric_series.hpp"

#include "database_format.hpp"
#include "disk_device.hpp"
#include "local_transaction_store.hpp"
#include "page_header.hpp"
#include "page_manager.hpp"
#include "startup_state.hpp"
#include "uuid.hpp"
#include "hash_digest_parts.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
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
using scratchbird::transaction::mga::EvaluateVersionEffectVisibility;
using scratchbird::transaction::mga::kInvalidLocalTransactionId;
using scratchbird::transaction::mga::LocalTransactionId;
using scratchbird::transaction::mga::LocalTransactionInventory;
using scratchbird::transaction::mga::LookupLocalTransaction;
using scratchbird::transaction::mga::MakeLocalTransactionId;
using scratchbird::transaction::mga::PlanLocalCopyOnWriteMutationForTransaction;
using scratchbird::transaction::mga::ValidateCopyOnWriteTransactionState;
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

bool ReserveRetainedCatalogObject(
    std::map<scratchbird::core::platform::Uuid,scratchbird::core::platform::Uuid>& reservations,
    const scratchbird::core::catalog::CatalogMetadataVersion& metadata,
    const TransactionInventoryEntry& creator) {
  const auto outcome=scratchbird::transaction::mga::InventoryVisibilityState(creator);
  if (outcome==TransactionState::rolled_back || outcome==TransactionState::failed_terminal) return true;
  const auto& header=metadata.record.header;
  const auto [reservation,inserted]=reservations.emplace(header.object_uuid.value,header.row_uuid.value);
  return inserted || reservation->second==header.row_uuid.value;
}

using PhysicalCowSteadyClock = std::chrono::steady_clock;

u64 PhysicalCowElapsedMicros(PhysicalCowSteadyClock::time_point start,
                             PhysicalCowSteadyClock::time_point finish) {
  return static_cast<u64>(
      std::chrono::duration_cast<std::chrono::microseconds>(finish - start)
          .count());
}

void WritePhysicalCowBatchPhaseTrace(
    u64 row_count,
    u64 pages_written,
    const std::vector<std::pair<std::string, u64>>& phase_micros) {
  const char* trace_path =
      std::getenv("SCRATCHBIRD_PHYSICAL_MGA_COW_PHASE_TRACE_FILE");
  if (trace_path == nullptr || *trace_path == '\0') {
    return;
  }
  std::ofstream out(trace_path, std::ios::app | std::ios::binary);
  if (!out) {
    return;
  }
  out << "operation=storage.physical_mga_cow.batch"
      << "\trows=" << row_count
      << "\tpages_written=" << pages_written;
  u64 total = 0;
  for (const auto& [phase, micros] : phase_micros) {
    total += micros;
    out << '\t' << phase << "_us=" << micros;
  }
  out << "\ttotal_us=" << total << '\n';
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

PhysicalMgaCowFinalizeResult ValidateFinalization(
    const PhysicalMgaCowFinalization& request) {
  if (!request.transaction.local_id.valid() ||
      !IsTypedEngineIdentity(request.transaction.transaction_uuid, UuidKind::transaction) ||
      request.transaction.scope != scratchbird::transaction::mga::TransactionScope::local_node ||
      (request.decision != PhysicalMgaCowFinalizeDecision::commit &&
       request.decision != PhysicalMgaCowFinalizeDecision::rollback) ||
      request.final_unix_epoch_millis == 0)
    return ErrorResult<PhysicalMgaCowFinalizeResult>("CATALOG.INVALID_INPUT",
        "storage.physical_mga_cow.finalization_input_invalid");
  PhysicalMgaCowFinalizeResult result;
  result.status = CowStoreOkStatus();
  return result;
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
  const auto startup=ReadStartupStatePageBody(device,parsed_header.header.page_size);
  if (!startup.ok()) return Propagate<DatabaseContextResult>(startup.status,startup.diagnostic);
  const auto startup_header=ReadDevicePageHeader(device,parsed_header.header.page_size,
      kSystemStatePageNumber,ReadWritePolicy(parsed_header.header.page_size));
  if (!startup_header.ok()) return Propagate<DatabaseContextResult>(startup_header.status,startup_header.diagnostic);
  const auto parsed_startup=ParsePageHeader(startup_header.serialized);
  if (!parsed_startup.ok()) return Propagate<DatabaseContextResult>(parsed_startup.status,parsed_startup.diagnostic);
  if (startup.state.database_uuid.kind!=UuidKind::database ||
      startup.state.database_uuid.value!=parsed_header.header.database_uuid ||
      startup.state.page_size!=parsed_header.header.page_size ||
      !IsTypedEngineIdentity(startup.state.first_filespace_uuid,UuidKind::filespace) ||
      parsed_inventory_header.header.filespace_uuid!=startup.state.first_filespace_uuid.value ||
      parsed_inventory_header.header.page_number!=kTransactionInventoryPageNumber ||
      !parsed_inventory_header.header.page_generation ||
      !scratchbird::core::uuid::IsEngineIdentityUuid(parsed_inventory_header.header.page_uuid) ||
      parsed_startup.header.page_type!=PageType::system_state ||
      parsed_startup.header.database_uuid!=parsed_header.header.database_uuid ||
      parsed_startup.header.filespace_uuid!=startup.state.first_filespace_uuid.value ||
      parsed_startup.header.page_number!=kSystemStatePageNumber ||
      parsed_startup.header.page_size!=parsed_header.header.page_size ||
      !parsed_startup.header.page_generation ||
      !scratchbird::core::uuid::IsEngineIdentityUuid(parsed_startup.header.page_uuid))
    return ErrorResult<DatabaseContextResult>("CATALOG.INVALID_INPUT",
        "storage.physical_mga_cow.inventory_filespace_binding_invalid");

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

scratchbird::core::catalog::CatalogNameVersionBinding NativeNameBinding(
    const TypedUuid& database_uuid, const TypedUuid& filespace_uuid, u64 page_number,
    const RowDataRecord& row, const scratchbird::core::catalog::CatalogMetadataVersion& metadata) {
  scratchbird::core::catalog::CatalogNameVersionBinding binding;
  binding.database_uuid=database_uuid; binding.filespace_uuid=filespace_uuid;
  binding.row_uuid=row.row_uuid; binding.version_uuid={UuidKind::row,row.version_uuid};
  binding.catalog_object_uuid=metadata.record.header.object_uuid;
  binding.creating_transaction_uuid=row.transaction_uuid;
  binding.page_id=page_number; binding.slot_id=row.stable_slot_id;
  binding.storage_generation=row.storage_generation; binding.version_sequence=row.row_version;
  binding.creating_transaction_number=row.local_transaction_id;
  binding.catalog_generation=metadata.catalog_generation;
  return binding;
}

scratchbird::core::catalog::CatalogNameEnvelopeDecodeResult DecodeNativeName(
    const TypedUuid& database_uuid, const TypedUuid& filespace_uuid, u64 page_number,
    const RowDataRecord& row, const scratchbird::core::catalog::CatalogMetadataVersion& metadata) {
  namespace catalog=scratchbird::core::catalog;
  auto result=catalog::DecodeCatalogNameEnvelope(
      std::vector<scratchbird::core::platform::byte>(metadata.record.payload.begin(),metadata.record.payload.end()),
      NativeNameBinding(database_uuid,filespace_uuid,page_number,row,metadata));
  if (result.ok() && !catalog::CatalogNamePayloadMatchesMetadata(result.record->payload,metadata))
    return {catalog::CatalogNameEnvelopeError::binding_mismatch,{}};
  return result;
}

PhysicalMgaCowMutationResult MaterializeNativeName(const PhysicalMgaCowMutation& request,
    const DatabaseContextResult& context, RowDataRecord& row) {
  if (!request.catalog_name) { PhysicalMgaCowMutationResult result; result.status=CowStoreOkStatus(); return result; }
  namespace catalog=scratchbird::core::catalog;
  auto metadata=request.catalog_name->metadata;
  const auto name=catalog::EncodeCatalogNameEnvelope({NativeNameBinding(context.page_context.database_uuid,
      context.page_context.filespace_uuid,request.page_number,row,metadata),request.catalog_name->payload});
  if (!name.ok()) return ErrorResult<PhysicalMgaCowMutationResult>("CATALOG.INVALID_INPUT", "catalog.native_name.resident_binding_invalid");
  metadata.record.payload.assign(name.bytes.begin(),name.bytes.end());
  const auto encoded=catalog::EncodeCatalogMetadataVersion(metadata);
  if (!encoded.ok()) return Propagate<PhysicalMgaCowMutationResult>(encoded.status,encoded.diagnostic);
  scratchbird::core::datatypes::DatatypeBinaryValue cell;
  cell.type_id=scratchbird::core::datatypes::CanonicalTypeId::binary; cell.payload=encoded.bytes;
  row.cells={{1,std::move(cell)}};
  PhysicalMgaCowMutationResult result; result.status=CowStoreOkStatus(); return result;
}

PhysicalMgaCowMutationResult ValidateMutationRequest(
    const PhysicalMgaCowMutation& request, const std::string& database_path) {
  auto common =
      ValidateCommonRequest<PhysicalMgaCowMutationResult>(database_path,
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
  if (request.predecessor_page_number != 0 &&
      request.predecessor_page_number >= request.page_number) {
    return ErrorResult<PhysicalMgaCowMutationResult>(
        "SB-PHYSICAL-MGA-COW-PREDECESSOR-PAGE-INVALID",
        "storage.physical_mga_cow.predecessor_page_invalid",
        "predecessor=" + std::to_string(request.predecessor_page_number) +
            " page=" + std::to_string(request.page_number));
  }
  if ((request.kind == PhysicalMgaCowMutationKind::insert ||
       request.kind == PhysicalMgaCowMutationKind::update) &&
      request.cells.empty() && !request.catalog_name) {
    return ErrorResult<PhysicalMgaCowMutationResult>(
        "SB-PHYSICAL-MGA-COW-PAYLOAD-REQUIRED",
        "storage.physical_mga_cow.payload_required");
  }
  if (request.catalog_name) {
    const auto& name=*request.catalog_name;
    if (!request.cells.empty() || !name.metadata.record.payload.empty() ||
        request.kind == PhysicalMgaCowMutationKind::delete_row || !request.use_existing_transaction ||
        !SameUuid(name.metadata.record.header.row_uuid,request.row_uuid) ||
        !SameUuid(name.metadata.creator_transaction_uuid,request.transaction_uuid) ||
        name.metadata.creator_local_transaction_id != request.existing_local_transaction_id.value ||
        name.metadata.authority_scope == scratchbird::core::catalog::CatalogAuthorityScope::cluster ||
        !scratchbird::core::catalog::CatalogNamePayloadMatchesMetadata(name.payload,name.metadata) ||
        !scratchbird::core::catalog::EncodeCatalogMetadataVersion(name.metadata).ok())
      return ErrorResult<PhysicalMgaCowMutationResult>("CATALOG.INVALID_INPUT", "catalog.native_name.mutation_binding_invalid");
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
    if (scratchbird::transaction::mga::HasCommittedInventoryOutcome(entry) &&
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
    case TransactionState::archived:
      return RowVersionState::unknown;
  }
  return RowVersionState::unknown;
}

RowVersionMetadata MetadataForRow(const RowDataRecord& row,
                                  const TransactionInventoryEntry& entry) {
  RowVersionMetadata metadata;
  metadata.identity.row.row_uuid = row.row_uuid;
  metadata.identity.creator_transaction = entry.identity;
  metadata.identity.version_sequence = row.row_version;
  metadata.identity.version_uuid = row.version_uuid;
  metadata.chain.previous_version_uuid = {UuidKind::row, row.previous_version_uuid};
  metadata.chain.next_version_uuid = {UuidKind::row, row.next_version_uuid};
  metadata.chain.previous_version_sequence = row.previous_row_version;
  metadata.chain.next_version_sequence = row.next_row_version;
  const auto state = scratchbird::transaction::mga::InventoryVisibilityState(entry);
  metadata.state = RowStateForEntry(row, state);
  metadata.creator_transaction_state = state;
  metadata.creator_commit_sequence = entry.commit_sequence;
  metadata.payload_present = !row.cells.empty();
  return metadata;
}

std::optional<DiagnosticRecord> ValidateRowCreators(
    const RowDataPageBody& body, const LocalTransactionInventory& inventory) {
  for (const auto& row : body.rows) {
    const auto creator = LookupLocalTransaction(inventory, MakeLocalTransactionId(row.local_transaction_id));
    if (!creator.ok()) return creator.diagnostic;
    if (!SameUuid(creator.entry.identity.transaction_uuid, row.transaction_uuid))
      return MakePhysicalMgaCowDiagnostic(CowStoreErrorStatus(), "CATALOG.INVALID_INPUT",
          "storage.physical_mga_cow.creator_identity_mismatch");
  }
  return std::nullopt;
}

PhysicalMgaCowMutationResult ReadRowDataPage(FileDevice* device,
                                             const DatabaseContextResult& context,
                                             const PhysicalMgaCowMutation& request,
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
    body->next_page_number = request.predecessor_page_number;
    PhysicalMgaCowMutationResult result;
    result.status = CowStoreOkStatus();
    result.row_page = *body;
    return result;
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
  const auto outer = ParsePageHeader(header.serialized);
  if (!outer.ok()) return Propagate<PhysicalMgaCowMutationResult>(outer.status, outer.diagnostic);
  if (outer.header.database_uuid != context.page_context.database_uuid.value ||
      outer.header.filespace_uuid != context.page_context.filespace_uuid.value ||
      outer.header.page_number != request.page_number ||
      outer.header.page_generation != parsed.body.page_generation) {
    return ErrorResult<PhysicalMgaCowMutationResult>("CATALOG.INVALID_INPUT",
        "storage.physical_mga_cow.page_identity_mismatch");
  }
  *body = parsed.body;
  PhysicalMgaCowMutationResult result;
  result.status = CowStoreOkStatus();
  result.row_page = *body;
  return result;
}

RowDataPageBody MakeEmptyRowDataPageBody(
    const TypedUuid& relation_uuid,
    u64 page_number,
    u64 predecessor_page_number) {
  RowDataPageBody body;
  body.relation_uuid = relation_uuid;
  body.segment_id = 1;
  body.segment_generation = 1;
  body.page_number = page_number;
  body.page_generation = 1;
  body.compaction_generation = 1;
  body.next_page_number = predecessor_page_number;
  return body;
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

scratchbird::core::uuid::TypedUuidResult IssuePhysicalIdentity(UuidKind kind) {
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  // The generator rejects out-of-range time and reports entropy failures.
  // Accepted database/cluster clock policy remains the owning admission layer.
  return scratchbird::core::uuid::GenerateDurableEngineIdentityV7(
      kind, static_cast<u64>(now));
}

struct PreparedRowDataPage {
  PhysicalMgaCowMutationResult result;
  u64 offset = 0;
  std::vector<scratchbird::core::platform::byte> image;
};

PreparedRowDataPage PrepareRowDataPage(const DatabaseContextResult& context,
                                      RowDataPageBody body) {
  body.page_generation = std::max<u64>(1, body.page_generation);
  body.compaction_generation = std::max<u64>(body.compaction_generation,
                                             body.page_generation);
  auto built = BuildRowDataPageBodyOwned(std::move(body), context.page_size);
  if (!built.ok()) {
    return {Propagate<PhysicalMgaCowMutationResult>(built.status, built.diagnostic)};
  }
  const auto page_uuid = IssuePhysicalIdentity(UuidKind::page);
  if (!page_uuid.ok()) {
    return {Propagate<PhysicalMgaCowMutationResult>(page_uuid.status, page_uuid.diagnostic)};
  }
  ManagedPageHeaderRequest header_request;
  header_request.context = context.page_context;
  header_request.page_type = PageType::row_data;
  header_request.page_uuid = page_uuid.value;
  header_request.page_number = built.body.page_number;
  header_request.page_generation = built.body.page_generation;
  const auto header = BuildManagedPageHeader(header_request);
  if (!header.ok()) {
    return {Propagate<PhysicalMgaCowMutationResult>(header.status, header.diagnostic)};
  }
  const auto page_offset = CheckedPageOffset(context.page_size,
                                             built.body.page_number);
  if (!page_offset.ok()) {
    return {Propagate<PhysicalMgaCowMutationResult>(page_offset.status, page_offset.diagnostic)};
  }
  // No independently published header generation before the body write.
  // A complete image is still not an atomic-sector or crash-recovery claim.
  std::vector<scratchbird::core::platform::byte> image(context.page_size);
  if (header.serialized.size() > image.size() ||
      built.serialized.size() != image.size() - header.serialized.size()) {
    return {ErrorResult<PhysicalMgaCowMutationResult>("CATALOG.INVALID_INPUT",
        "storage.physical_mga_cow.page_image_extent_invalid")};
  }
  std::copy(header.serialized.begin(), header.serialized.end(), image.begin());
  std::copy(built.serialized.begin(), built.serialized.end(),
            image.begin() + header.serialized.size());
  PhysicalMgaCowMutationResult result;
  result.status = CowStoreOkStatus();
  result.row_page = std::move(built.body);
  result.page_uuid = page_uuid.value;
  result.page_generation = result.row_page.page_generation;
  return {std::move(result), page_offset.offset, std::move(image)};
}

PhysicalMgaCowMutationResult WriteRowDataPage(FileDevice* device,
                                              const DatabaseContextResult& context,
                                              RowDataPageBody body,
                                              bool sync_after_write = true) {
  auto prepared = PrepareRowDataPage(context, std::move(body));
  if (!prepared.result.ok()) return std::move(prepared.result);
  const auto written = device->WriteAt(prepared.offset, prepared.image.data(), prepared.image.size());
  if (!written.ok()) {
    return Propagate<PhysicalMgaCowMutationResult>(written.status, written.diagnostic);
  }
  if (sync_after_write) {
    const auto sync = device->Sync();
    if (!sync.ok()) {
      return Propagate<PhysicalMgaCowMutationResult>(sync.status,
                                                     sync.diagnostic);
    }
  }
  return std::move(prepared.result);
}

BaseRowSelection SelectBaseRow(const RowDataPageBody& body,
                               const LocalTransactionInventory& inventory,
                               const TransactionInventoryEntry& writer,
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
  VisibilitySnapshot snapshot = LatestCommittedSnapshot(inventory);
  snapshot.reader_transaction = writer.identity.local_id;
  snapshot.allow_reader_own_uncommitted = true;
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
    if (!SameUuid(entry.entry.identity.transaction_uuid, candidate.second.transaction_uuid)) {
      if (blocked != nullptr) *blocked = true;
      if (diagnostic != nullptr) *diagnostic = MakePhysicalMgaCowDiagnostic(
          CowStoreErrorStatus(), "CATALOG.INVALID_INPUT",
          "storage.physical_mga_cow.creator_identity_mismatch");
      return selection;
    }
    if (entry.entry.identity.local_id.value != writer.identity.local_id.value &&
        (entry.entry.state == TransactionState::active ||
        entry.entry.state == TransactionState::preparing ||
        entry.entry.state == TransactionState::prepared ||
        entry.entry.state == TransactionState::committing)) {
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
    const auto visible = EvaluateVersionEffectVisibility(MetadataForRow(candidate.second,
                                                          entry.entry),
                                            snapshot);
    if (visible.decision == VisibilityDecision::visible) {
      if (candidate.second.deleted) return selection;
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
  u32 largest = 0;
  for (const RowDataRecord& row : body.rows) {
    largest = std::max(largest, row.stable_slot_id);
  }
  return largest == std::numeric_limits<u32>::max() ? 0 : largest + 1;
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
    case PhysicalMgaCowFinalizeDecision::invalid: return "invalid";
  }
  return "unknown";
}

PhysicalMgaCowMutationResult WritePhysicalMgaCowUnpublishedMutation(
    const PhysicalMgaCowMutationRequest& request) {
  const auto valid = ValidateMutationRequest(request, request.database_path);
  if (!valid.ok()) {
    return valid;
  }

  FileDevice device;
  const auto open = device.Open(request.database_path, FileOpenMode::open_existing);
  if (!open.ok()) {
    return Propagate<PhysicalMgaCowMutationResult>(open.status, open.diagnostic);
  }
  return WritePhysicalMgaCowUnpublishedMutationToOpenDevice(device, request);
}

PhysicalMgaCowMutationResult WritePhysicalMgaCowUnpublishedMutationToOpenDevice(
    FileDevice& device, const PhysicalMgaCowMutation& request) {
  const auto operation_guard = device.AcquireOperationGuard();
  const auto valid = ValidateMutationRequest(request, device.path());
  if (!valid.ok()) {
    return valid;
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

  LocalTransactionId owned_transaction;
  scratchbird::transaction::mga::TransactionIdentity owned_identity;
  const auto perform = [&]() -> PhysicalMgaCowMutationResult {
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
    const auto admission = ValidateCopyOnWriteTransactionState(existing.entry);
    if (!admission.ok()) {
      return Propagate<PhysicalMgaCowMutationResult>(admission.status, admission.diagnostic);
    }
    active_entry = existing.entry;
  } else {
    const auto begin = BeginLocalTransaction(loaded_inventory.inventory,
                                             request.transaction_uuid,
                                             request.begin_unix_epoch_millis);
    if (!begin.ok()) {
      return Propagate<PhysicalMgaCowMutationResult>(begin.status, begin.diagnostic);
    }
    owned_transaction = begin.entry.identity.local_id;
    owned_identity = begin.entry.identity;
    const auto persisted_active =
        PersistLocalTransactionInventoryToOpenDevice(&device,
                                                     context.page_size,
                                                     begin.inventory);
    if (!persisted_active.ok()) {
      return Propagate<PhysicalMgaCowMutationResult>(persisted_active.status,
                                                     persisted_active.diagnostic);
    }
    active_inventory = persisted_active.inventory;
    active_entry = begin.entry;
  }

  RowDataPageBody row_page;
  auto read_page = ReadRowDataPage(&device, context, request, &row_page);
  if (!read_page.ok()) {
    return read_page;
  }
  if (!row_page.rows.empty() && row_page.page_generation == std::numeric_limits<u64>::max())
    return ErrorResult<PhysicalMgaCowMutationResult>("CATALOG.INVALID_INPUT",
        "storage.physical_mga_cow.page_generation_exhausted");
  row_page.page_generation += row_page.rows.empty() ? 0 : 1;
  if (const auto failure = ValidateRowCreators(row_page, active_inventory))
    return Propagate<PhysicalMgaCowMutationResult>(failure->status, *failure);

  bool blocked = false;
  DiagnosticRecord blocked_diagnostic;
  const BaseRowSelection base = SelectBaseRow(row_page,
                                              active_inventory,
                                              active_entry,
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
  if (new_sequence == 0) {
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

  const auto version_uuid = IssuePhysicalIdentity(UuidKind::row);
  if (!version_uuid.ok())
    return Propagate<PhysicalMgaCowMutationResult>(version_uuid.status, version_uuid.diagnostic);
  scratchbird::core::platform::Uuid previous_version_uuid;
  if (base.found) {
    previous_version_uuid = base.row.version_uuid;
    row_page.rows[base.index].next_version_uuid = version_uuid.value.value;
    row_page.rows[base.index].next_row_version = new_sequence;
  } else if (base.max_row_version != 0) {
    for (RowDataRecord& row : row_page.rows) {
      if (SameUuid(row.row_uuid, request.row_uuid) &&
          row.row_version == base.max_row_version) {
        previous_version_uuid = row.version_uuid;
        row.next_version_uuid = version_uuid.value.value;
        row.next_row_version = new_sequence;
      }
    }
  }

  RowDataRecord new_row;
  new_row.storage_generation = row_page.page_generation;
  new_row.version_uuid = version_uuid.value.value;
  new_row.previous_version_uuid = previous_version_uuid;
  new_row.row_uuid = request.row_uuid;
  new_row.transaction_uuid = request.transaction_uuid;
  new_row.local_transaction_id = active_entry.identity.local_id.value;
  new_row.stable_slot_id = base.found ? base.row.stable_slot_id
                                      : (request.stable_slot_id == 0
                                             ? NextStableSlotId(row_page)
                                             : request.stable_slot_id);
  new_row.row_version = new_sequence;
  if (new_row.stable_slot_id == 0)
    return ErrorResult<PhysicalMgaCowMutationResult>("CATALOG.INVALID_INPUT",
        "storage.physical_mga_cow.slot_identity_exhausted");
  new_row.previous_row_version = base.found ? base.row.row_version
                                            : base.max_row_version;
  new_row.next_row_version = 0;
  new_row.deleted = request.kind == PhysicalMgaCowMutationKind::delete_row;
  if (!new_row.deleted) {
    new_row.cells = request.cells;
  }
  if (request.catalog_name) {
    const auto materialized=MaterializeNativeName(request,context,new_row);
    if (!materialized.ok()) return materialized;
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
  result.page_uuid = written.page_uuid;
  result.page_generation = written.page_generation;
  result.evidence.push_back("physical_mga_cow.row_page_written=true");
  result.evidence.push_back(
      request.use_existing_transaction
          ? "physical_mga_cow.existing_active_transaction_verified=true"
          : "physical_mga_cow.inventory_active_persisted_before_row_page=true");
  result.evidence.push_back("physical_mga_cow.visibility_published_by_inventory=false");
  result.evidence.push_back(std::string("physical_mga_cow.kind=") +
                            PhysicalMgaCowMutationKindName(request.kind));
  return result;
  };
  PhysicalMgaCowMutationResult operation;
  std::exception_ptr pending_exception;
  try {
    operation = perform();
  } catch (...) {
    pending_exception = std::current_exception();
  }
  if ((!operation.ok() || pending_exception) && owned_transaction.valid()) {
    const auto compensate = [&]() -> PhysicalMgaCowMutationResult {
      const auto current = LoadLocalTransactionInventoryFromOpenDevice(&device, context.page_size);
      if (!current.ok())
        return Propagate<PhysicalMgaCowMutationResult>(current.status, current.diagnostic);
      const auto found = std::find_if(current.inventory.entries.begin(), current.inventory.entries.end(),
          [&](const auto& entry) { return entry.identity.local_id.value == owned_transaction.value; });
      if (found == current.inventory.entries.end()) {
        PhysicalMgaCowMutationResult absent; absent.status = CowStoreOkStatus();
        absent.evidence.push_back("physical_mga_cow.failed_owned_transaction_not_published=true");
        return absent;
      }
      if (!SameUuid(found->identity.transaction_uuid, request.transaction_uuid))
        return ErrorResult<PhysicalMgaCowMutationResult>("CATALOG.INVALID_INPUT",
            "storage.physical_mga_cow.failed_transaction_identity_mismatch");
      const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count();
      const auto rolled = RollbackLocalTransaction(current.inventory, owned_transaction, static_cast<u64>(millis));
      if (!rolled.ok())
        return Propagate<PhysicalMgaCowMutationResult>(rolled.status, rolled.diagnostic);
      const auto persisted = PersistLocalTransactionInventoryToOpenDevice(&device, context.page_size, rolled.inventory);
      if (!persisted.ok())
        return Propagate<PhysicalMgaCowMutationResult>(persisted.status, persisted.diagnostic);
      PhysicalMgaCowMutationResult compensated; compensated.status = CowStoreOkStatus();
      compensated.evidence.push_back("physical_mga_cow.failed_owned_transaction_rolled_back=true");
      return compensated;
    };
    auto compensation = compensate();
    if (!compensation.ok()) {
      compensation.unresolved_owned_transaction = owned_identity;
      compensation.diagnostic.arguments.push_back({"mutation_failure_code", operation.diagnostic.diagnostic_code});
      compensation.diagnostic.arguments.push_back({"mutation_failure_key", operation.diagnostic.message_key});
      if (pending_exception)
        compensation.diagnostic.arguments.push_back({"mutation_exception", "propagation_interrupted_by_rollback_failure"});
      return compensation;
    }
    operation.evidence.insert(operation.evidence.end(), compensation.evidence.begin(), compensation.evidence.end());
  }
  if (pending_exception) std::rethrow_exception(pending_exception);
  return operation;
}

PhysicalMgaCowMutationBatchResult WritePhysicalMgaCowUnpublishedMutationBatch(
    PhysicalMgaCowMutationBatchRequest request) {
  if (request.mutations.empty()) {
    return ErrorResult<PhysicalMgaCowMutationBatchResult>(
        "SB-PHYSICAL-MGA-COW-BATCH-EMPTY",
        "storage.physical_mga_cow.batch_empty");
  }
  const auto& path = request.mutations.front().database_path;
  for (const auto& mutation : request.mutations) {
    const auto valid = ValidateMutationRequest(mutation, mutation.database_path);
    if (!valid.ok()) {
      return Propagate<PhysicalMgaCowMutationBatchResult>(valid.status, valid.diagnostic);
    }
    if (mutation.database_path != path) {
      return ErrorResult<PhysicalMgaCowMutationBatchResult>(
          "SB-PHYSICAL-MGA-COW-BATCH-SCOPE-MISMATCH",
          "storage.physical_mga_cow.batch_scope_mismatch");
    }
  }
  FileDevice device;
  const auto open = device.Open(path, FileOpenMode::open_existing);
  if (!open.ok()) {
    return Propagate<PhysicalMgaCowMutationBatchResult>(open.status, open.diagnostic);
  }
  PhysicalMgaCowMutationBatch batch;
  batch.sync_after_batch = request.sync_after_batch;
  batch.engine_generated_unique_insert_rows = request.engine_generated_unique_insert_rows;
  batch.mutations.reserve(request.mutations.size());
  for (auto& mutation : request.mutations) {
    batch.mutations.push_back(std::move(static_cast<PhysicalMgaCowMutation&>(mutation)));
  }
  auto result = WritePhysicalMgaCowUnpublishedMutationBatchToOpenDevice(device, std::move(batch));
  if (result.ok()) {
    result.evidence.push_back("physical_mga_cow.batch_database_open_once=true");
  }
  return result;
}

PhysicalMgaCowMutationBatchResult WritePhysicalMgaCowUnpublishedMutationBatchToOpenDevice(
    FileDevice& device, PhysicalMgaCowMutationBatch request) {
  const auto operation_guard = device.AcquireOperationGuard();
  const auto trace_start = PhysicalCowSteadyClock::now();
  auto trace_last = trace_start;
  std::vector<std::pair<std::string, u64>> phase_micros;
  phase_micros.reserve(10);
  const auto mark_phase = [&](std::string phase) {
    const auto now = PhysicalCowSteadyClock::now();
    phase_micros.push_back(
        {std::move(phase), PhysicalCowElapsedMicros(trace_last, now)});
    trace_last = now;
  };

  if (request.mutations.empty()) {
    return ErrorResult<PhysicalMgaCowMutationBatchResult>(
        "SB-PHYSICAL-MGA-COW-BATCH-EMPTY",
        "storage.physical_mga_cow.batch_empty");
  }

  const auto first_valid = ValidateMutationRequest(request.mutations.front(), device.path());
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
  mark_phase("validate_first");

  const auto context = LoadDatabaseContext(&device);
  if (!context.ok()) {
    return Propagate<PhysicalMgaCowMutationBatchResult>(context.status,
                                                        context.diagnostic);
  }
  mark_phase("load_database_context");
  const auto loaded_inventory =
      LoadLocalTransactionInventoryFromOpenDevice(&device, context.page_size);
  if (!loaded_inventory.ok()) {
    return Propagate<PhysicalMgaCowMutationBatchResult>(
        loaded_inventory.status,
        loaded_inventory.diagnostic);
  }
  mark_phase("load_transaction_inventory");

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
  const auto admission = ValidateCopyOnWriteTransactionState(existing.entry);
  if (!admission.ok()) {
    return Propagate<PhysicalMgaCowMutationBatchResult>(admission.status, admission.diagnostic);
  }
  const TransactionInventoryEntry active_entry = existing.entry;
  mark_phase("lookup_transaction");
  const auto initial_device_size = device.Size();
  if (!initial_device_size.ok()) {
    return Propagate<PhysicalMgaCowMutationBatchResult>(
        initial_device_size.status,
        initial_device_size.diagnostic);
  }
  mark_phase("initial_device_size");

  std::map<u64, RowDataPageBody> page_cache;
  std::vector<PhysicalMgaCowRowReceipt> row_receipts;
  bool used_empty_page_insert_fast_path = false;
  row_receipts.reserve(request.mutations.size());
  const auto retain_receipt = [&](const PhysicalMgaCowMutation& mutation, const RowDataRecord& row) {
    PhysicalMgaCowRowReceipt receipt;
    receipt.database_uuid = context.page_context.database_uuid;
    receipt.filespace_uuid = context.page_context.filespace_uuid;
    receipt.relation_uuid = mutation.relation_uuid; receipt.row_uuid = row.row_uuid;
    receipt.creator = active_entry.identity;
    receipt.version_uuid = row.version_uuid; receipt.previous_version_uuid = row.previous_version_uuid;
    receipt.page_number = mutation.page_number; receipt.row_version = row.row_version;
    receipt.storage_generation = row.storage_generation;
    receipt.stable_slot_id = row.stable_slot_id; receipt.deleted = row.deleted;
    row_receipts.push_back(std::move(receipt));
  };
  std::map<u64, bool> page_empty_on_load;
  std::map<u64, std::set<std::array<scratchbird::core::platform::byte, 16>>>
      page_insert_row_uuids;
  for (auto& mutation_request : request.mutations) {
    const auto valid = ValidateMutationRequest(mutation_request, device.path());
    if (!valid.ok()) {
      return Propagate<PhysicalMgaCowMutationBatchResult>(valid.status,
                                                          valid.diagnostic);
    }
    if (!(mutation_request.transaction_uuid.value ==
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
      const auto page_offset =
          CheckedPageOffset(context.page_size, mutation_request.page_number);
      if (!page_offset.ok()) {
        return Propagate<PhysicalMgaCowMutationBatchResult>(
            page_offset.status,
            page_offset.diagnostic);
      }
      if (initial_device_size.size_bytes <= page_offset.offset) {
        loaded_page = MakeEmptyRowDataPageBody(mutation_request.relation_uuid,
                                               mutation_request.page_number,
                                               mutation_request
                                                   .predecessor_page_number);
      } else {
        auto read_page = ReadRowDataPage(&device,
                                         context,
                                         mutation_request,
                                         &loaded_page);
        if (!read_page.ok()) {
          return Propagate<PhysicalMgaCowMutationBatchResult>(
              read_page.status,
              read_page.diagnostic);
        }
      }
      if (!loaded_page.rows.empty()) {
        if (loaded_page.page_generation == std::numeric_limits<u64>::max())
          return ErrorResult<PhysicalMgaCowMutationBatchResult>("CATALOG.INVALID_INPUT",
              "storage.physical_mga_cow.page_generation_exhausted");
        if (const auto failure = ValidateRowCreators(loaded_page, loaded_inventory.inventory))
          return Propagate<PhysicalMgaCowMutationBatchResult>(failure->status, *failure);
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
      used_empty_page_insert_fast_path = true;
      if (!request.engine_generated_unique_insert_rows) {
        auto& inserted_rows =
            page_insert_row_uuids[mutation_request.page_number];
        if (!inserted_rows.insert(mutation_request.row_uuid.value.bytes).second) {
          return ErrorResult<PhysicalMgaCowMutationBatchResult>(
              "SB-PHYSICAL-MGA-COW-DUPLICATE-VISIBLE-ROW",
              "storage.physical_mga_cow.duplicate_visible_row");
        }
      }

      const auto version_uuid = IssuePhysicalIdentity(UuidKind::row);
      if (!version_uuid.ok())
        return Propagate<PhysicalMgaCowMutationBatchResult>(version_uuid.status, version_uuid.diagnostic);
      RowDataRecord new_row;
      new_row.storage_generation = row_page.page_generation;
      new_row.version_uuid = version_uuid.value.value;
      new_row.row_uuid = mutation_request.row_uuid;
      new_row.transaction_uuid = mutation_request.transaction_uuid;
      new_row.local_transaction_id = active_entry.identity.local_id.value;
      new_row.stable_slot_id = mutation_request.stable_slot_id;
      new_row.row_version = 1;
      new_row.previous_row_version = 0;
      new_row.next_row_version = 0;
      new_row.deleted = false;
      new_row.cells = std::move(mutation_request.cells);
      if (mutation_request.catalog_name) {
        const auto materialized=MaterializeNativeName(mutation_request,context,new_row);
        if (!materialized.ok()) return Propagate<PhysicalMgaCowMutationBatchResult>(materialized.status,materialized.diagnostic);
      }
      retain_receipt(mutation_request, new_row);
      row_page.rows.push_back(std::move(new_row));
      continue;
    }

    bool blocked = false;
    DiagnosticRecord blocked_diagnostic;
    const BaseRowSelection base = SelectBaseRow(row_page,
                                                active_inventory,
                                                active_entry,
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
    if (new_sequence == 0) {
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

    const auto version_uuid = IssuePhysicalIdentity(UuidKind::row);
    if (!version_uuid.ok())
      return Propagate<PhysicalMgaCowMutationBatchResult>(version_uuid.status, version_uuid.diagnostic);
    scratchbird::core::platform::Uuid previous_version_uuid;
    if (base.found) {
      previous_version_uuid = base.row.version_uuid;
      row_page.rows[base.index].next_version_uuid = version_uuid.value.value;
      row_page.rows[base.index].next_row_version = new_sequence;
    } else if (base.max_row_version != 0) {
      for (RowDataRecord& row : row_page.rows) {
        if (SameUuid(row.row_uuid, mutation_request.row_uuid) &&
            row.row_version == base.max_row_version) {
          previous_version_uuid = row.version_uuid;
          row.next_version_uuid = version_uuid.value.value;
          row.next_row_version = new_sequence;
        }
      }
    }

    RowDataRecord new_row;
    new_row.storage_generation = row_page.page_generation;
    new_row.version_uuid = version_uuid.value.value;
    new_row.previous_version_uuid = previous_version_uuid;
    new_row.row_uuid = mutation_request.row_uuid;
    new_row.transaction_uuid = mutation_request.transaction_uuid;
    new_row.local_transaction_id = active_entry.identity.local_id.value;
    new_row.stable_slot_id = base.found
                                 ? base.row.stable_slot_id
                                 : (mutation_request.stable_slot_id == 0
                                        ? NextStableSlotId(row_page)
                                        : mutation_request.stable_slot_id);
    new_row.row_version = new_sequence;
    if (new_row.stable_slot_id == 0)
      return ErrorResult<PhysicalMgaCowMutationBatchResult>("CATALOG.INVALID_INPUT",
          "storage.physical_mga_cow.slot_identity_exhausted");
    new_row.previous_row_version = base.found ? base.row.row_version
                                              : base.max_row_version;
    new_row.next_row_version = 0;
    new_row.deleted =
        mutation_request.kind == PhysicalMgaCowMutationKind::delete_row;
    if (!new_row.deleted) {
      new_row.cells = std::move(mutation_request.cells);
    }
    if (mutation_request.catalog_name) {
      const auto materialized=MaterializeNativeName(mutation_request,context,new_row);
      if (!materialized.ok()) return Propagate<PhysicalMgaCowMutationBatchResult>(materialized.status,materialized.diagnostic);
    }
    retain_receipt(mutation_request, new_row);
    row_page.rows.push_back(std::move(new_row));
  }
  mark_phase("stage_page_mutations");

  std::vector<PreparedRowDataPage> prepared_pages;
  std::map<u64, std::pair<TypedUuid, u64>> page_locations;
  prepared_pages.reserve(page_cache.size());
  for (auto& [page_number, row_page] : page_cache) {
    (void)page_number;
    auto prepared = PrepareRowDataPage(context, std::move(row_page));
    if (!prepared.result.ok())
      return Propagate<PhysicalMgaCowMutationBatchResult>(prepared.result.status, prepared.result.diagnostic);
    page_locations.emplace(page_number, std::make_pair(prepared.result.page_uuid, prepared.result.page_generation));
    // Batch publication needs the complete image, not a duplicate decoded page.
    prepared.result.row_page = {};
    prepared_pages.push_back(std::move(prepared));
  }
  mark_phase("prepare_page_images");
  for (auto& receipt : row_receipts) {
    const auto& location = page_locations.at(receipt.page_number);
    receipt.page_uuid = location.first; receipt.page_generation = location.second;
  }

  // Persist inability to commit BEFORE the first page write. If I/O, allocation
  // or the process fails during this batch, no surviving prefix can be committed
  // later. Only this invocation's exact loaded base may release its marker after
  // every image is complete. This is inventory authority, not a sidecar WAL.
  const auto marked = scratchbird::transaction::mga::MarkLocalTransactionRollbackOnly(
      active_inventory, active_entry.identity.local_id);
  if (!marked.ok()) return Propagate<PhysicalMgaCowMutationBatchResult>(marked.status, marked.diagnostic);
  const auto guarded = PersistLocalTransactionInventoryToOpenDevice(&device, context.page_size, marked.inventory);
  const auto unresolved = [&](Status status, DiagnosticRecord diagnostic) {
    auto failed = Propagate<PhysicalMgaCowMutationBatchResult>(status, std::move(diagnostic));
    failed.unresolved_mutation_transaction = active_entry.identity;
    return failed;
  };
  if (!guarded.ok()) return unresolved(guarded.status, guarded.diagnostic);

  PhysicalMgaCowMutationBatchResult result;
  result.status = CowStoreOkStatus();
  for (const auto& prepared : prepared_pages) {
    const auto written = device.WriteAt(prepared.offset, prepared.image.data(), prepared.image.size());
    if (!written.ok()) {
      return unresolved(written.status, written.diagnostic);
    }
    ++result.pages_written;
  }
  mark_phase("write_pages");
  if (request.sync_after_batch) {
    const auto sync = device.Sync();
    if (!sync.ok()) {
      return unresolved(sync.status, sync.diagnostic);
    }
  }
  mark_phase("sync");
  result.written_rows = static_cast<u64>(request.mutations.size());
  result.evidence.push_back("physical_mga_cow.batch=true");
  result.evidence.push_back("physical_mga_cow.batch_borrowed_device=true");
  result.evidence.push_back("physical_mga_cow.batch_native_inventory_commit_guard=true");
  result.evidence.push_back(
      request.engine_generated_unique_insert_rows
          ? "physical_mga_cow.engine_generated_unique_insert_rows=true"
          : "physical_mga_cow.engine_generated_unique_insert_rows=false");
  result.evidence.push_back("physical_mga_cow.existing_active_transaction_verified=true");
  result.evidence.push_back(std::string("physical_mga_cow.batch_sync_after_pages=") +
                            (request.sync_after_batch ? "true" : "false"));
  result.evidence.push_back("physical_mga_cow.batch_inventory_guard_publication_sync=true");
  result.evidence.push_back("physical_mga_cow.visibility_published_by_inventory=false");
  result.evidence.push_back(std::string("physical_mga_cow.empty_page_insert_fast_path=") +
      (used_empty_page_insert_fast_path ? "true" : "false"));
  result.evidence.push_back("physical_mga_cow.batch_written_rows=" +
                            std::to_string(result.written_rows));
  result.evidence.push_back("physical_mga_cow.batch_pages_written=" +
                            std::to_string(result.pages_written));
  WritePhysicalCowBatchPhaseTrace(result.written_rows,
                                  result.pages_written,
                                  phase_micros);
  auto completed_inventory = guarded.inventory;
  auto completed_entry = std::find_if(completed_inventory.entries.begin(), completed_inventory.entries.end(),
      [&](const auto& entry) { return entry.identity.local_id.value == active_entry.identity.local_id.value; });
  if (completed_entry == completed_inventory.entries.end() ||
      !SameUuid(completed_entry->identity.transaction_uuid, active_entry.identity.transaction_uuid) ||
      completed_entry->state != TransactionState::active || !completed_entry->rollback_only)
    return unresolved(CowStoreErrorStatus(), MakePhysicalMgaCowDiagnostic(CowStoreErrorStatus(),
        "CATALOG.INVALID_INPUT", "storage.physical_mga_cow.batch_guard_identity_mismatch"));
  completed_entry->rollback_only = false;
  const auto released = PersistLocalTransactionInventoryToOpenDevice(&device, context.page_size, completed_inventory);
  if (!released.ok()) return unresolved(released.status, released.diagnostic);
  result.row_receipts = std::move(row_receipts);
  return result;
}

PhysicalMgaCowFinalizeResult FinalizePhysicalMgaCowTransaction(
    const PhysicalMgaCowFinalizeRequest& request) {
  const auto valid = ValidateFinalization(request);
  if (!valid.ok()) return valid;
  if (request.database_path.empty()) {
    return ErrorResult<PhysicalMgaCowFinalizeResult>(
        "SB-PHYSICAL-MGA-COW-PATH-REQUIRED",
        "storage.physical_mga_cow.path_required");
  }
  FileDevice device;
  const auto open = device.Open(request.database_path, FileOpenMode::open_existing);
  if (!open.ok()) {
    return Propagate<PhysicalMgaCowFinalizeResult>(open.status, open.diagnostic);
  }
  return FinalizePhysicalMgaCowTransactionToOpenDevice(device, request);
}

PhysicalMgaCowFinalizeResult FinalizePhysicalMgaCowTransactionToOpenDevice(
    FileDevice& device, const PhysicalMgaCowFinalization& request) {
  const auto operation_guard = device.AcquireOperationGuard();
  const auto valid = ValidateFinalization(request);
  if (!valid.ok()) return valid;
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

  const auto current = LookupLocalTransaction(loaded_inventory.inventory,
      request.transaction.local_id);
  if (!current.ok() ||
      !SameUuid(current.entry.identity.transaction_uuid, request.transaction.transaction_uuid) ||
      current.entry.identity.scope != request.transaction.scope)
    return ErrorResult<PhysicalMgaCowFinalizeResult>("CATALOG.INVALID_INPUT",
        "storage.physical_mga_cow.finalization_identity_mismatch");

  const auto finalized =
      request.decision == PhysicalMgaCowFinalizeDecision::commit
          ? CommitLocalTransaction(loaded_inventory.inventory,
                                   request.transaction.local_id,
                                   request.final_unix_epoch_millis)
          : RollbackLocalTransaction(loaded_inventory.inventory,
                                     request.transaction.local_id,
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
  result.inventory = persisted.inventory;
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
  return ReadPhysicalMgaCowRowsFromOpenDevice(
      device, request.relation_uuid, request.page_number,
      request.visibility_snapshot, request.use_latest_committed_snapshot, request.reader_identity,
      request.snapshot_pin);
}

PhysicalMgaCowReadResult ReadPhysicalMgaCowRowsFromOpenDevice(
    FileDevice& device,
    const TypedUuid& relation_uuid,
    u64 page_number,
    const VisibilitySnapshot& visibility_snapshot,
    bool use_latest_committed_snapshot,
    const scratchbird::transaction::mga::TransactionIdentity& reader_identity,
    const scratchbird::transaction::mga::PublishedSnapshotPin* snapshot_pin) {
  const auto operation_guard = device.AcquireOperationGuard();
  PhysicalMgaCowReadRequest request;
  request.database_path = device.path();
  request.relation_uuid = relation_uuid;
  request.page_number = page_number;
  request.visibility_snapshot = visibility_snapshot;
  request.reader_identity = reader_identity;
  request.use_latest_committed_snapshot = use_latest_committed_snapshot;
  request.snapshot_pin = snapshot_pin;
  const auto common = ValidateCommonRequest<PhysicalMgaCowReadResult>(
      request.database_path, request.relation_uuid, request.page_number);
  if (!common.ok()) {
    return common;
  }
  const bool reader_supplied = reader_identity.local_id.value != 0 ||
      reader_identity.transaction_uuid.kind != UuidKind::unknown ||
      !reader_identity.transaction_uuid.value.is_nil() ||
      reader_identity.scope != scratchbird::transaction::mga::TransactionScope::unknown;
  if ((reader_supplied &&
       (!reader_identity.local_id.valid() ||
        (!snapshot_pin && reader_identity.local_id.value != visibility_snapshot.reader_transaction.value) ||
        !IsTypedEngineIdentity(reader_identity.transaction_uuid, UuidKind::transaction) ||
        reader_identity.scope != scratchbird::transaction::mga::TransactionScope::local_node)) ||
      (!reader_supplied && visibility_snapshot.reader_transaction.value != 0))
    return ErrorResult<PhysicalMgaCowReadResult>("CATALOG.INVALID_INPUT",
        "storage.physical_mga_cow.reader_identity_invalid");
  if (snapshot_pin && (!reader_supplied || use_latest_committed_snapshot ||
      visibility_snapshot.reader_transaction.value != 0 ||
      visibility_snapshot.visible_through_local_transaction_id != 0 ||
      visibility_snapshot.visible_through_local_transaction_id_is_boundary ||
      !visibility_snapshot.allow_reader_own_uncommitted || visibility_snapshot.recovery_context ||
      visibility_snapshot.visible_through_commit_sequence != 0 ||
      visibility_snapshot.visible_through_commit_sequence_is_boundary ||
      !visibility_snapshot.active_excluded_local_transaction_ids.empty() ||
      !visibility_snapshot.in_doubt_excluded_local_transaction_ids.empty()))
    return ErrorResult<PhysicalMgaCowReadResult>("CATALOG.INVALID_INPUT",
        "storage.physical_mga_cow.snapshot_pin_request_invalid");
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

  if (reader_supplied) {
    const auto reader = LookupLocalTransaction(loaded_inventory.inventory, reader_identity.local_id);
    if (!reader.ok() ||
        !SameUuid(reader.entry.identity.transaction_uuid, reader_identity.transaction_uuid) ||
        reader.entry.identity.scope != reader_identity.scope)
      return ErrorResult<PhysicalMgaCowReadResult>("CATALOG.INVALID_INPUT",
          "storage.physical_mga_cow.reader_identity_mismatch");
  }

  VisibilitySnapshot snapshot = request.visibility_snapshot;
  if (snapshot_pin) {
    const auto resolved = snapshot_pin->Resolve();
    if (!resolved.ok())
      return Propagate<PhysicalMgaCowReadResult>(resolved.status, resolved.diagnostic);
    const auto& descriptor = resolved.descriptor;
    const auto reader = LookupLocalTransaction(loaded_inventory.inventory, reader_identity.local_id);
    if (!SameUuid(descriptor.owning_transaction_uuid, reader_identity.transaction_uuid) ||
        descriptor.owning_transaction.value != reader_identity.local_id.value ||
        loaded_inventory.inventory.next_local_transaction_id <
            descriptor.publication_inventory_next_local_transaction_id ||
        !reader.ok() || (reader.entry.state != TransactionState::active &&
                         reader.entry.state != TransactionState::read_only_active))
      return ErrorResult<PhysicalMgaCowReadResult>("CATALOG.INVALID_INPUT",
          "storage.physical_mga_cow.snapshot_pin_owner_mismatch");
    snapshot.reader_transaction = descriptor.owning_transaction;
    snapshot.visible_through_local_transaction_id = descriptor.visible_committed_high_watermark;
    snapshot.visible_through_local_transaction_id_is_boundary = true;
    snapshot.active_excluded_local_transaction_ids = descriptor.active_excluded_local_transaction_ids;
    snapshot.in_doubt_excluded_local_transaction_ids = descriptor.in_doubt_excluded_local_transaction_ids;
  } else if (request.use_latest_committed_snapshot) {
    snapshot = LatestCommittedSnapshot(loaded_inventory.inventory);
  }

  RowDataPageBody row_page;
  const auto read_page = ReadRowDataPageForRead(&device, context, request, &row_page);
  if (!read_page.ok()) {
    return read_page;
  }

  PhysicalMgaCowReadResult result;
  result.status = CowStoreOkStatus();
  result.inventory = loaded_inventory.inventory;
  result.row_page = row_page;
  result.evidence.push_back("physical_mga_cow.read_visibility_authority=durable_transaction_inventory");
  result.evidence.push_back("physical_mga_cow.row_page_finality_authority=false");

  std::map<std::array<scratchbird::core::platform::byte, 16>,
           std::vector<RowDataRecord>> by_row;
  for (const RowDataRecord& row : row_page.rows) {
    const auto creator = LookupLocalTransaction(loaded_inventory.inventory,
        MakeLocalTransactionId(row.local_transaction_id));
    if (!creator.ok())
      return Propagate<PhysicalMgaCowReadResult>(creator.status, creator.diagnostic);
    if (!SameUuid(creator.entry.identity.transaction_uuid, row.transaction_uuid))
      return ErrorResult<PhysicalMgaCowReadResult>("CATALOG.INVALID_INPUT",
          "storage.physical_mga_cow.creator_identity_mismatch");
    auto metadata = MetadataForRow(row, creator.entry);
    const auto valid_metadata = scratchbird::transaction::mga::ValidateRowVersionMetadata(metadata);
    if (!valid_metadata.ok())
      return Propagate<PhysicalMgaCowReadResult>(valid_metadata.status, valid_metadata.diagnostic);
    result.version_metadata.push_back(std::move(metadata));
    by_row[row.row_uuid.value.bytes].push_back(row);
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
      const auto visibility = EvaluateVersionEffectVisibility(observed.metadata, snapshot);
      if (!visibility.ok() && visibility.decision != VisibilityDecision::wait_for_transaction &&
          visibility.decision != VisibilityDecision::requires_recovery)
        return Propagate<PhysicalMgaCowReadResult>(visibility.status, visibility.diagnostic);
      observed.decision = visibility.decision;
      if (observed.metadata.creator_transaction_state == TransactionState::rolled_back ||
          observed.metadata.creator_transaction_state == TransactionState::failed_terminal) {
        ++result.rolled_back_version_count;
        result.rows.push_back(std::move(observed));
        continue;
      }
      if (row.deleted && observed.decision == VisibilityDecision::visible) {
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

namespace {
struct DecodedNativeCatalogRows {
  Status status;
  DiagnosticRecord diagnostic;
  std::map<scratchbird::core::platform::Uuid,
      scratchbird::core::catalog::CatalogMetadataVersion> metadata;
  std::map<scratchbird::core::platform::Uuid,scratchbird::core::catalog::CatalogNamePayload> names;
  bool ok() const { return status.ok(); }
};
DecodedNativeCatalogRows DecodeNativeCatalogRows(const RowDataPageBody& body,
    const TypedUuid& database_uuid, const TypedUuid& filespace_uuid) {
  namespace catalog = scratchbird::core::catalog;
  DecodedNativeCatalogRows decoded;
  for (const auto& row : body.rows) {
    if (row.deleted || row.cells.size() != 1 || row.cells[0].column_ordinal != 1 ||
        row.cells[0].value.type_id != scratchbird::core::datatypes::CanonicalTypeId::binary ||
        row.cells[0].value.is_null || row.cells[0].value.payload_is_toast_reference)
      return ErrorResult<DecodedNativeCatalogRows>("CATALOG.INVALID_INPUT", "catalog.native_version.cell_shape_invalid");
    const auto value = catalog::DecodeCatalogMetadataVersion(row.cells[0].value.payload);
    if (!value.ok()) return Propagate<DecodedNativeCatalogRows>(value.status, value.diagnostic);
    if (!SameUuid(value.record.record.header.row_uuid, row.row_uuid) ||
        !SameUuid(value.record.creator_transaction_uuid, row.transaction_uuid) ||
        value.record.creator_local_transaction_id != row.local_transaction_id)
      return ErrorResult<DecodedNativeCatalogRows>("CATALOG.INVALID_INPUT", "catalog.native_version.mga_binding_invalid");
    if (!decoded.metadata.emplace(row.version_uuid, value.record).second)
      return ErrorResult<DecodedNativeCatalogRows>("CATALOG.INVALID_INPUT", "catalog.native_version.version_duplicate");
    if (value.record.record.header.kind == catalog::CatalogRecordKind::localized_name) {
      auto name=DecodeNativeName(database_uuid,filespace_uuid,body.page_number,row,value.record);
      if (!name.ok()) return ErrorResult<DecodedNativeCatalogRows>("CATALOG.INVALID_INPUT", "catalog.native_name.resident_binding_invalid");
      decoded.names.emplace(row.version_uuid,std::move(name.record->payload));
    }
  }
  for (const auto& row : body.rows) {
    const auto prior=decoded.metadata.find(row.previous_version_uuid);
    if (prior!=decoded.metadata.end() && !catalog::CatalogSchemaDefinitionPreservesOrigin(
        prior->second,decoded.metadata.at(row.version_uuid)))
      return ErrorResult<DecodedNativeCatalogRows>("CATALOG.INVALID_INPUT", "catalog.native_version.schema_origin_changed");
    if (prior!=decoded.metadata.end()) {
      if (!catalog::CatalogMetricSeriesPreservesOrigin(prior->second,decoded.metadata.at(row.version_uuid)))
        return ErrorResult<DecodedNativeCatalogRows>("CATALOG.INVALID_INPUT", "catalog.native_version.metric_series_origin_changed");
      if (!catalog::CatalogMetricLabelSchemaPreservesOrigin(prior->second,decoded.metadata.at(row.version_uuid)))
        return ErrorResult<DecodedNativeCatalogRows>("CATALOG.INVALID_INPUT", "catalog.native_version.metric_label_schema_origin_changed");
      if (!catalog::CatalogMetricDescriptorPreservesOrigin(prior->second,decoded.metadata.at(row.version_uuid)))
        return ErrorResult<DecodedNativeCatalogRows>("CATALOG.INVALID_INPUT", "catalog.native_version.metric_descriptor_origin_changed");
      if (!catalog::CatalogMetricRetentionPolicyPreservesOrigin(
          prior->second,decoded.metadata.at(row.version_uuid)))
        return ErrorResult<DecodedNativeCatalogRows>("CATALOG.INVALID_INPUT", "catalog.native_version.metric_retention_origin_changed");
      const auto before=decoded.names.find(row.previous_version_uuid),after=decoded.names.find(row.version_uuid);
      if ((before!=decoded.names.end() || after!=decoded.names.end()) &&
          (before==decoded.names.end() || after==decoded.names.end() ||
           !catalog::CatalogNamePayloadPreservesIdentity(before->second,after->second)))
        return ErrorResult<DecodedNativeCatalogRows>("CATALOG.INVALID_INPUT", "catalog.native_name.origin_changed");
    }
  }
  decoded.status=CowStoreOkStatus(); return decoded;
}
using LeafError=NativeCatalogLeafError;
NativeCatalogLeafResult LeafFailure(LeafError error) { return {error,std::nullopt,{},{}}; }
LeafError LeafMetadataError(const DecodedNativeCatalogRows& decoded) {
  return decoded.diagnostic.diagnostic_code=="SB-CORE-HASH-SHA256-FAILED"
      ?LeafError::hash_failure:LeafError::invalid_metadata;
}
bool ValidLeafBinding(const scratchbird::storage::disk::NativeCommonPageHeader& header,
                      const RowDataPageBody& body) {
  return header.page_number==body.page_number
      && header.page_generation==body.page_generation && !body.next_page_number
      && body.compaction_generation
      && std::all_of(body.rows.begin(),body.rows.end(),[](const auto& row) { return row.stable_slot_id!=0; });
}
scratchbird::core::hash::HashDigestResult LeafDigest(const std::vector<scratchbird::core::platform::byte>& image) {
  const std::array<scratchbird::core::platform::byte,32> zero{};
  const scratchbird::core::hash::HashDigestSegment parts[]={{image.data(),image.size()-32},{zero.data(),zero.size()}};
  return scratchbird::core::hash::ComputeSha256DigestParts(parts,2);
}
}  // namespace

NativeCatalogLeafResult EncodeNativeCatalogLeaf(const NativeCatalogLeafPage& page) noexcept {
  try {
    const auto header=scratchbird::storage::disk::EncodeNativeCommonPageHeader(page.header);
    if (!header.ok() || page.header.page_type!=6) return LeafFailure(LeafError::invalid_header);
    if (!ValidLeafBinding(page.header,page.body)) return LeafFailure(LeafError::invalid_body);
    auto body=BuildRowDataPageBody(page.body,page.header.page_size_bytes-32);
    if (!body.ok()) return LeafFailure(LeafError::invalid_body);
    auto decoded=DecodeNativeCatalogRows(body.body,{UuidKind::database,page.header.database_uuid},
        {UuidKind::filespace,page.header.filespace_uuid});
    if (!decoded.ok()) return LeafFailure(LeafMetadataError(decoded));
    std::vector<scratchbird::core::platform::byte> bytes(page.header.page_size_bytes,0);
    std::copy(header.bytes->begin(),header.bytes->end(),bytes.begin());
    std::copy(body.serialized.begin(),body.serialized.end(),bytes.begin()+128);
    const auto digest=LeafDigest(bytes); if (!digest.ok()) return LeafFailure(LeafError::hash_failure);
    std::copy(digest.digest.begin(),digest.digest.end(),bytes.end()-32);
    return {LeafError::none,NativeCatalogLeafPage{page.header,std::move(body.body)},std::move(decoded.metadata),std::move(bytes)};
  } catch (const std::bad_alloc&) { return LeafFailure(LeafError::resource_exhausted); }
    catch (const std::length_error&) { return LeafFailure(LeafError::resource_exhausted); }
    catch (...) { return LeafFailure(LeafError::invalid_body); }
}

NativeCatalogLeafResult DecodeNativeCatalogLeaf(const std::vector<scratchbird::core::platform::byte>& bytes) noexcept {
  try {
    const auto header=scratchbird::storage::disk::DecodeNativeCommonPageHeader(bytes.data(),std::min<std::size_t>(bytes.size(),128));
    if (!header.ok() || header.header->page_type!=6 || bytes.size()!=header.header->page_size_bytes)
      return LeafFailure(LeafError::invalid_header);
    const auto digest=LeafDigest(bytes); if (!digest.ok()) return LeafFailure(LeafError::hash_failure);
    if (!std::equal(digest.digest.begin(),digest.digest.end(),bytes.end()-32)) return LeafFailure(LeafError::invalid_integrity);
    std::vector<scratchbird::core::platform::byte> body_bytes(bytes.begin()+128,bytes.end()-32);
    auto body=ParseRowDataPageBody(body_bytes,header.header->page_number);
    if (!body.ok() || !ValidLeafBinding(*header.header,body.body)) return LeafFailure(LeafError::invalid_body);
    // The generic row reader also supports other owners. The catalog family
    // requires the exact canonical body, including its zero unused region.
    const auto canonical=BuildRowDataPageBody(body.body,header.header->page_size_bytes-32);
    if (!canonical.ok() || canonical.serialized!=body_bytes) return LeafFailure(LeafError::invalid_body);
    auto decoded=DecodeNativeCatalogRows(body.body,{UuidKind::database,header.header->database_uuid},
        {UuidKind::filespace,header.header->filespace_uuid});
    if (!decoded.ok()) return LeafFailure(LeafMetadataError(decoded));
    return {LeafError::none,NativeCatalogLeafPage{*header.header,std::move(body.body)},std::move(decoded.metadata),bytes};
  } catch (const std::bad_alloc&) { return LeafFailure(LeafError::resource_exhausted); }
    catch (const std::length_error&) { return LeafFailure(LeafError::resource_exhausted); }
    catch (...) { return LeafFailure(LeafError::invalid_body); }
}

NativeCatalogLeafResult ReadNativeCatalogLeafFromOpenDevice(
    FileDevice& device,const scratchbird::core::platform::Uuid& database_uuid,
    const scratchbird::storage::page::NativeCatalogRootReference& ref) noexcept {
  namespace disk=scratchbird::storage::disk;
  try {
    const auto& p=ref.page; const auto* profile=disk::FindCanonicalFilespacePageProfile(p.page_size_profile_uuid);
    if (!scratchbird::core::uuid::IsEngineIdentityUuid(database_uuid)
        || !scratchbird::core::uuid::IsEngineIdentityUuid(ref.object_uuid)
        || !scratchbird::core::uuid::IsEngineIdentityUuid(p.filespace_uuid)
        || ref.role<1 || ref.role>6 || ref.page_type!=6 || !profile || !p.page_number || !p.page_generation
        || p.page_number>=std::numeric_limits<u64>::max()/profile->page_size_bytes
        || !disk::CheckFileDeviceExtent(p.page_number*profile->page_size_bytes,profile->page_size_bytes).ok())
      return LeafFailure(LeafError::binding_mismatch);
    const auto guard=device.AcquireOperationGuard();
    const disk::FilespaceBootstrapBinding binding{database_uuid,p.filespace_uuid,p.page_size_profile_uuid};
    const auto zero=disk::ReadFilespacePageZeroFromOpenDevice(device,&binding);
    if (!zero.ok()) {
      if (zero.error==disk::FilespacePageZeroError::resource_exhausted) return LeafFailure(LeafError::resource_exhausted);
      if (zero.error==disk::FilespacePageZeroError::hash_provider_failure) return LeafFailure(LeafError::hash_failure);
      if (zero.error==disk::FilespacePageZeroError::io_failure) return LeafFailure(LeafError::io_failure);
      return LeafFailure(LeafError::invalid_filespace);
    }
    if (zero.record->bootstrap.filespace_role>5 || p.page_number>=zero.record->total_pages)
      return LeafFailure(LeafError::invalid_filespace);
    if (zero.record->bootstrap.flags & disk::FilespaceBootstrapFlag::payload_encrypted)
      return LeafFailure(LeafError::encrypted_requires_crypto_authority);
    std::vector<scratchbird::core::platform::byte> bytes(profile->page_size_bytes);
    const auto io=device.ReadAt(p.page_number*profile->page_size_bytes,bytes.data(),bytes.size());
    if (!io.ok() || io.bytes_transferred!=bytes.size()) return LeafFailure(LeafError::io_failure);
    const disk::NativeCommonPageHeaderBinding expected{binding,p.page_number,p.page_generation,6,std::nullopt};
    const auto header=disk::DecodeNativeCommonPageHeader(bytes.data(),128,&expected);
    if (!header.ok()) return LeafFailure(LeafError::binding_mismatch);
    if (header.header->flags & 1u) return LeafFailure(LeafError::encrypted_requires_crypto_authority);
    if (header.header->flags & 2u) return LeafFailure(LeafError::cluster_requires_authority);
    if (header.header->flags & 12u) return LeafFailure(LeafError::header_policy_requires_authority);
    auto result=DecodeNativeCatalogLeaf(bytes); if (!result.ok()) return result;
    if (result.page->body.relation_uuid.value!=ref.object_uuid) return LeafFailure(LeafError::binding_mismatch);
    return result;
  } catch (const std::bad_alloc&) { return LeafFailure(LeafError::resource_exhausted); }
    catch (const std::length_error&) { return LeafFailure(LeafError::resource_exhausted); }
    catch (...) { return LeafFailure(LeafError::io_failure); }
}

NativeCatalogRelationImageResult ReadNativeCatalogRelationImagesFromOpenDevices(
    const scratchbird::core::platform::Uuid& database_uuid,
    const std::vector<scratchbird::storage::disk::NativeFilespaceDevice>& devices,
    const scratchbird::storage::page::NativeCatalogRootReference& head,
    const NativeCatalogRelationBinding& binding,u64 maximum_retained_image_bytes) noexcept {
  namespace disk=scratchbird::storage::disk;namespace page=scratchbird::storage::page;
  using Uuid=scratchbird::core::platform::Uuid;using E=NativeCatalogRelationError;
  const auto fail=[](E e){NativeCatalogRelationImageResult r;r.error=e;return r;};
  const auto v7=[](const auto& id){return scratchbird::core::uuid::IsEngineIdentityUuid(id);};
  try {
    if(!v7(database_uuid)||!v7(binding.relation_uuid)||!v7(head.object_uuid)||head.role<1||head.role>6
      ||!v7(head.page.filespace_uuid)||!head.page.page_number||!head.page.page_generation
      ||!disk::FindCanonicalFilespacePageProfile(head.page.page_size_profile_uuid)||devices.empty()||!maximum_retained_image_bytes
      ||(head.page_type!=6&&head.page_type!=0x200))return fail(E::invalid_reference);
    if(head.page_type==6?(binding.index_dependencies.has_value()||head.object_uuid!=binding.relation_uuid):
        (!binding.index_dependencies||binding.index_dependencies->index_uuid!=head.object_uuid))return fail(E::binding_mismatch);
    auto ordered=devices;std::sort(ordered.begin(),ordered.end(),[](const auto& a,const auto& b){return a.filespace_uuid<b.filespace_uuid;});
    for(std::size_t i=0;i<ordered.size();++i){const auto& fs=ordered[i];
      if(!v7(fs.filespace_uuid)||!disk::FindCanonicalFilespacePageProfile(fs.page_size_profile_uuid)||!fs.device
        ||(i&&fs.filespace_uuid==ordered[i-1].filespace_uuid))return fail(E::invalid_filespace);
      for(std::size_t j=0;j<i;++j)if(fs.device==ordered[j].device)return fail(E::invalid_filespace);}
    std::vector<std::unique_lock<std::recursive_mutex>> guards;guards.reserve(ordered.size());
    for(const auto& fs:ordered)guards.push_back(fs.device->AcquireOperationGuard());
    for(const auto& fs:ordered){const disk::FilespaceBootstrapBinding expected{database_uuid,fs.filespace_uuid,fs.page_size_profile_uuid};
      const auto zero=disk::ReadFilespacePageZeroFromOpenDevice(*fs.device,&expected);
      if(!zero.ok()){
        if(zero.error==disk::FilespacePageZeroError::resource_exhausted)return fail(E::resource_exhausted);
        if(zero.error==disk::FilespacePageZeroError::hash_provider_failure)return fail(E::hash_failure);
        if(zero.error==disk::FilespacePageZeroError::io_failure)return fail(E::io_failure);
        return fail(E::invalid_filespace);}}
    NativeCatalogRelationImageResult result;
    std::set<std::pair<Uuid,u64>> navigation_slots;std::set<Uuid> page_ids;
    if(head.page_type==0x200){auto tree=page::ReadNativeBtreeTreeFromOpenDevices(database_uuid,ordered,head.page,*binding.index_dependencies,maximum_retained_image_bytes);
      if(!tree.ok()){auto error=fail(E::tree_failure);error.tree_error=tree.error;return error;}
      for(const auto& node:tree.pages){const auto& h=node.page->header;navigation_slots.emplace(h.filespace_uuid,h.page_number);page_ids.insert(h.page_uuid);}
      result.retained_image_bytes=tree.retained_image_bytes;result.index=std::move(tree);}
    std::map<std::pair<Uuid,u64>,std::size_t> catalog_slots;
    std::map<Uuid,std::pair<std::size_t,std::size_t>> versions;
    E failure=E::none;NativeCatalogLeafError leaf_error=NativeCatalogLeafError::none;
    const auto load=[&](const disk::NativePageReference& ref)->std::optional<std::size_t>{
      const auto slot=std::pair{ref.filespace_uuid,ref.page_number};
      if(navigation_slots.contains(slot)){failure=E::invalid_locator;return {};}
      const auto prior=catalog_slots.find(slot);
      if(prior!=catalog_slots.end()){const auto& h=result.catalogs[prior->second].page->header;
        if(h.page_generation!=ref.page_generation||h.page_size_profile_uuid!=ref.page_size_profile_uuid){failure=E::binding_mismatch;return {};}
        return prior->second;}
      const auto fs=std::lower_bound(ordered.begin(),ordered.end(),ref.filespace_uuid,[](const auto& a,const auto& id){return a.filespace_uuid<id;});
      if(fs==ordered.end()||fs->filespace_uuid!=ref.filespace_uuid||fs->page_size_profile_uuid!=ref.page_size_profile_uuid){failure=E::invalid_filespace;return {};}
      const auto* profile=disk::FindCanonicalFilespacePageProfile(ref.page_size_profile_uuid);
      if(!profile||profile->page_size_bytes>maximum_retained_image_bytes-result.retained_image_bytes){failure=E::resource_exhausted;return {};}
      auto leaf=ReadNativeCatalogLeafFromOpenDevice(*fs->device,database_uuid,{head.role,6,ref,binding.relation_uuid});
      if(!leaf.ok()){failure=E::leaf_failure;leaf_error=leaf.error;return {};}
      if(!page_ids.insert(leaf.page->header.page_uuid).second){failure=E::duplicate_identity;return {};}
      const auto index=result.catalogs.size();
      for(std::size_t i=0;i<leaf.page->body.rows.size();++i)if(!versions.emplace(leaf.page->body.rows[i].version_uuid,std::pair{index,i}).second){failure=E::duplicate_identity;return {};}
      catalog_slots.emplace(slot,index);result.retained_image_bytes+=leaf.bytes.size();result.catalogs.push_back(std::move(leaf));return index;};
    const auto load_error=[&]{auto error=fail(failure);error.leaf_error=leaf_error;return error;};
    if(!result.index){const auto leaf=load(head.page);if(!leaf)return load_error();
      for(std::size_t i=0;i<result.catalogs[*leaf].page->body.rows.size();++i)result.bindings.push_back({{},*leaf,i});}
    else for(const auto node_index:result.index->leaves){const auto& node=*result.index->pages[node_index].page;
      for(std::size_t i=0;i<node.cells.size();++i){const auto& cell=node.cells[i];
        if(!cell.base_page)return fail(E::invalid_locator);const auto leaf=load(*cell.base_page);if(!leaf)return load_error();
        const auto found=versions.find(cell.key.version_uuid);
        if(found==versions.end()||found->second.first!=*leaf)return fail(E::invalid_locator);
        const auto row_index=found->second.second;const auto& row=result.catalogs[*leaf].page->body.rows[row_index];
        if(row.row_uuid.value!=cell.key.row_uuid)return fail(E::binding_mismatch);
        result.bindings.push_back({NativeCatalogIndexEntryLocation{node_index,i},*leaf,row_index});}}
    result.error=E::none;return result;
  }catch(const std::bad_alloc&){return fail(E::resource_exhausted);}catch(const std::length_error&){return fail(E::resource_exhausted);}catch(...){return fail(E::io_failure);}
}

NativeCheckpointCatalogRelationResult ReadNativeCheckpointCatalogRelationFromOpenDevices(
    const scratchbird::core::platform::Uuid& database_uuid,
    const std::vector<scratchbird::storage::disk::NativeFilespaceDevice>& devices,
    const scratchbird::storage::disk::FilespaceRootReference& checkpoint,
    u16 catalog_selector, u16 relation_role, const NativeCatalogRelationBinding& binding,
    u64 maximum_retained_image_bytes) noexcept {
  namespace disk = scratchbird::storage::disk;
  namespace mga = scratchbird::transaction::mga;
  using E = NativeCheckpointCatalogRelationError;
  const auto fail = [](E error) { NativeCheckpointCatalogRelationResult r; r.error = error; return r; };
  try {
    if ((catalog_selector != 2 && catalog_selector != 8) || relation_role < 1 || relation_role > 6 ||
        !maximum_retained_image_bytes || devices.empty() ||
        !scratchbird::core::uuid::IsEngineIdentityUuid(database_uuid)) return fail(E::invalid_reference);
    auto ordered = devices;
    std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) { return a.filespace_uuid < b.filespace_uuid; });
    for (std::size_t i = 0; i < ordered.size(); ++i) {
      const auto& fs = ordered[i];
      if (!fs.device || !scratchbird::core::uuid::IsEngineIdentityUuid(fs.filespace_uuid) ||
          !disk::FindCanonicalFilespacePageProfile(fs.page_size_profile_uuid) ||
          (i && ordered[i-1].filespace_uuid == fs.filespace_uuid)) return fail(E::invalid_filespace);
      for (std::size_t j = 0; j < i; ++j)
        if (ordered[j].device == fs.device) return fail(E::invalid_filespace);
    }
    std::vector<std::unique_lock<std::recursive_mutex>> guards;
    guards.reserve(ordered.size());
    for (const auto& fs : ordered) guards.push_back(fs.device->AcquireOperationGuard());

    NativeCheckpointCatalogRelationResult result;
    result.checkpoint = VerifyNativeCheckpointCatalogRootsFromOpenDevices(
        database_uuid, ordered, checkpoint, maximum_retained_image_bytes);
    if (!result.checkpoint.ok()) {
      auto error = fail(E::checkpoint_failure);
      error.checkpoint.error = result.checkpoint.error;
      error.checkpoint.catalog_error = result.checkpoint.catalog_error;
      error.checkpoint.checkpoint_inventory.error = result.checkpoint.checkpoint_inventory.error;
      error.checkpoint.checkpoint_inventory.inventory_error = result.checkpoint.checkpoint_inventory.inventory_error;
      return error;
    }
    const auto catalog_index = catalog_selector == 2 ? 0 : result.checkpoint.feature_root_index;
    const auto& catalog = *result.checkpoint.catalogs[catalog_index].root;
    const auto head = std::find_if(catalog.roots.begin(), catalog.roots.end(),
        [&](const auto& candidate) { return candidate.role == relation_role; });
    if (head == catalog.roots.end()) return fail(E::missing_relation);
    const auto consumed = result.checkpoint.retained_image_bytes;
    if (consumed >= maximum_retained_image_bytes) return fail(E::resource_exhausted);
    result.relation = ReadNativeCatalogRelationImagesFromOpenDevices(
        database_uuid, ordered, *head, binding, maximum_retained_image_bytes - consumed);
    if (!result.relation.ok()) {
      auto error = fail(E::relation_failure);
      error.relation.error = result.relation.error;
      error.relation.tree_error = result.relation.tree_error;
      error.relation.leaf_error = result.relation.leaf_error;
      return error;
    }
    const auto& entries = result.checkpoint.checkpoint_inventory.inventory.entries;
    std::map<u64, std::size_t> entries_by_number;
    for (std::size_t i = 0; i < entries.size(); ++i)
      entries_by_number.emplace(entries[i].identity.local_id.value, i);
    E creator_error = E::none;
    const auto creator = [&](u64 number, const auto& uuid) -> std::optional<std::size_t> {
      const auto found = entries_by_number.find(number);
      if (found == entries_by_number.end() || entries[found->second].identity.transaction_uuid.value != uuid) {
        creator_error = E::creator_mismatch; return {};
      }
      if (entries[found->second].identity.scope != mga::TransactionScope::local_node) {
        creator_error = E::cluster_requires_authority; return {};
      }
      return found->second;
    };
    if (result.relation.index) {
      for (const auto& image : result.relation.index->pages) {
        const auto entry = creator(image.page->creator_local_transaction_id, image.page->creator_transaction_uuid);
        if (!entry) return fail(creator_error);
        result.navigation_creator_entries.push_back(*entry);
      }
    }
    for (std::size_t p = 0; p < result.relation.catalogs.size(); ++p) {
      const auto& image = result.relation.catalogs[p];
      for (std::size_t r = 0; r < image.page->body.rows.size(); ++r) {
        const auto& row = image.page->body.rows[r];
        const auto& metadata = image.metadata.at(row.version_uuid);
        if (metadata.authority_scope == scratchbird::core::catalog::CatalogAuthorityScope::cluster)
          return fail(E::cluster_requires_authority);
        const auto entry = creator(row.local_transaction_id, row.transaction_uuid.value);
        if (!entry) return fail(creator_error);
        result.row_creators.push_back({p, r, *entry});
      }
    }
    result.retained_image_bytes = consumed + result.relation.retained_image_bytes;
    result.error = E::none;
    return result;
  } catch (const std::bad_alloc&) { return fail(E::resource_exhausted); }
    catch (const std::length_error&) { return fail(E::resource_exhausted); }
    catch (...) { return fail(E::io_failure); }
}

NativePinnedCatalogReadResult ReadNativePinnedCatalogVersionsFromOpenDevices(
    const scratchbird::core::platform::Uuid& database_uuid,
    const std::vector<scratchbird::storage::disk::NativeFilespaceDevice>& devices,
    const scratchbird::storage::disk::FilespaceRootReference& checkpoint,
    u16 catalog_selector, u16 relation_role, const NativeCatalogRelationBinding& binding,
    const scratchbird::transaction::mga::TransactionIdentity& reader,
    const scratchbird::transaction::mga::PublishedSnapshotPin& pin,
    u64 maximum_retained_image_bytes) noexcept {
  namespace mga = scratchbird::transaction::mga;
  namespace catalog = scratchbird::core::catalog;
  namespace disk = scratchbird::storage::disk;
  using Uuid = scratchbird::core::platform::Uuid;
  using E = NativePinnedCatalogReadError;
  const auto fail = [](E error) { NativePinnedCatalogReadResult r; r.error=error; return r; };
  try {
    if (!reader.valid() || reader.scope!=mga::TransactionScope::local_node ||
        !IsTypedEngineIdentity(reader.transaction_uuid, UuidKind::transaction)) return fail(E::invalid_reader);
    const auto captured = pin.Resolve();
    if (!captured.ok()) { auto r=fail(E::snapshot_failure); r.diagnostic=captured.diagnostic; return r; }
    const auto& snapshot = captured.descriptor;
    if (!SameUuid(snapshot.owning_transaction_uuid, reader.transaction_uuid) ||
        snapshot.owning_transaction.value!=reader.local_id.value) return fail(E::reader_mismatch);

    auto ordered=devices;
    std::sort(ordered.begin(),ordered.end(),[](const auto& a,const auto& b){return a.filespace_uuid<b.filespace_uuid;});
    for (std::size_t i=0;i<ordered.size();++i) {
      const auto& fs=ordered[i];
      if (!fs.device || !scratchbird::core::uuid::IsEngineIdentityUuid(fs.filespace_uuid) ||
          !disk::FindCanonicalFilespacePageProfile(fs.page_size_profile_uuid) ||
          (i && ordered[i-1].filespace_uuid==fs.filespace_uuid)) return fail(E::invalid_filespace);
      for (std::size_t j=0;j<i;++j) if (ordered[j].device==fs.device) return fail(E::invalid_filespace);
    }
    std::vector<std::unique_lock<std::recursive_mutex>> guards;
    guards.reserve(ordered.size());
    for (const auto& fs:ordered) guards.push_back(fs.device->AcquireOperationGuard());
    NativePinnedCatalogReadResult result;
    result.source=ReadNativeCheckpointCatalogRelationFromOpenDevices(database_uuid,ordered,checkpoint,
        catalog_selector,relation_role,binding,maximum_retained_image_bytes);
    if (!result.source.ok()) { auto r=fail(E::source_failure); r.source=std::move(result.source); return r; }
    const auto& inventory=result.source.checkpoint.checkpoint_inventory.inventory;
    const auto owner=LookupLocalTransaction(inventory,reader.local_id);
    if (!owner.ok() || !SameUuid(owner.entry.identity.transaction_uuid,reader.transaction_uuid) ||
        owner.entry.identity.scope!=reader.scope ||
        (owner.entry.state!=TransactionState::active && owner.entry.state!=TransactionState::read_only_active) ||
        inventory.next_local_transaction_id<snapshot.publication_inventory_next_local_transaction_id)
      return fail(E::reader_mismatch);
    VisibilitySnapshot visibility;
    visibility.reader_transaction=reader.local_id;
    visibility.visible_through_local_transaction_id=snapshot.visible_committed_high_watermark;
    visibility.visible_through_local_transaction_id_is_boundary=true;
    visibility.active_excluded_local_transaction_ids=snapshot.active_excluded_local_transaction_ids;
    visibility.in_doubt_excluded_local_transaction_ids=snapshot.in_doubt_excluded_local_transaction_ids;

    const auto& sources=result.source.row_creators;
    const auto row_at=[&](std::size_t index)->const RowDataRecord& {
      const auto& source=sources[index];
      return result.source.relation.catalogs[source.catalog_page_index].page->body.rows[source.catalog_row_index];
    };
    const auto metadata_at=[&](std::size_t index)->const catalog::CatalogMetadataVersion& {
      return result.source.relation.catalogs[sources[index].catalog_page_index].metadata.at(row_at(index).version_uuid);
    };
    const auto name_at=[&](std::size_t index) {
      const auto& leaf=*result.source.relation.catalogs[sources[index].catalog_page_index].page;
      return DecodeNativeName({UuidKind::database,leaf.header.database_uuid},
          {UuidKind::filespace,leaf.header.filespace_uuid},leaf.body.page_number,row_at(index),metadata_at(index));
    };
    std::map<Uuid,std::map<u64,std::size_t,std::greater<u64>>> by_row;
    std::map<Uuid,std::size_t> by_version;
    std::map<Uuid,Uuid> object_reservations;
    for (std::size_t i=0;i<sources.size();++i) {
      const auto& row=row_at(i); auto& versions=by_row[row.row_uuid.value];
      if (!versions.empty()) {
        const auto& prior=metadata_at(versions.begin()->second);
        const auto& current=metadata_at(i);
        if (!SameUuid(prior.record.header.object_uuid,current.record.header.object_uuid) ||
            prior.record.header.kind!=current.record.header.kind) return fail(E::invalid_chain);
      }
      if (!versions.emplace(row.row_version,i).second || !by_version.emplace(row.version_uuid,i).second)
        return fail(E::invalid_chain);
      const auto validated=ValidateRowVersionMetadata(MetadataForRow(row,inventory.entries[sources[i].inventory_entry_index]));
      if (!validated.ok()) { auto r=fail(E::visibility_failure); r.diagnostic=validated.diagnostic; return r; }
      // Visibility and index membership cannot release a catalog identity.
      // All retained versions participate, except creators proven to have
      // rolled back or failed terminally, including their archived outcomes.
      if (!ReserveRetainedCatalogObject(object_reservations,metadata_at(i),
          inventory.entries[sources[i].inventory_entry_index])) return fail(E::duplicate_identity);
    }
    for (std::size_t i=0;i<sources.size();++i) {
      const auto& row=row_at(i);
      if (!row.previous_row_version) continue;
      const auto previous=by_version.find(row.previous_version_uuid);
      if (previous!=by_version.end() && (row_at(previous->second).row_uuid.value!=row.row_uuid.value ||
          row_at(previous->second).row_version!=row.previous_row_version)) return fail(E::invalid_chain);
      if (previous!=by_version.end()) {
        const auto& before=metadata_at(previous->second);
        const auto& after=metadata_at(i);
        if (after.record.header.kind == catalog::CatalogRecordKind::localized_name) {
          const auto old_name=name_at(previous->second),new_name=name_at(i);
          if (!old_name.ok() || !new_name.ok() ||
              !catalog::CatalogNamePayloadPreservesIdentity(old_name.record->payload,new_name.record->payload))
            return fail(E::invalid_chain);
        }
        if (!catalog::CatalogSchemaDefinitionPreservesOrigin(before,after) ||
            !catalog::CatalogMetricRetentionPolicyPreservesOrigin(before,after) ||
            !catalog::CatalogMetricDescriptorPreservesOrigin(before,after) ||
            !catalog::CatalogMetricSeriesPreservesOrigin(before,after) ||
            !catalog::CatalogMetricLabelSchemaPreservesOrigin(before,after) ||
            before.definition_version==std::numeric_limits<u64>::max() || after.definition_version!=before.definition_version+1 ||
            after.schema_epoch<before.schema_epoch || after.security_epoch<before.security_epoch || after.resource_epoch<before.resource_epoch ||
            after.catalog_generation<before.catalog_generation || after.dependency_generation<before.dependency_generation ||
            after.invalidation_generation<before.invalidation_generation) return fail(E::invalid_chain);
      }
      const auto& versions=by_row.at(row.row_uuid.value);
      const auto sequence=versions.find(row.previous_row_version);
      if (sequence!=versions.end() && row_at(sequence->second).version_uuid!=row.previous_version_uuid)
        return fail(E::invalid_chain);
    }
    std::set<Uuid> candidates;
    for (const auto& bound:result.source.relation.bindings)
      candidates.insert(result.source.relation.catalogs[bound.catalog_page_index].page->body.rows[bound.catalog_row_index].row_uuid.value);
    for (const auto& row_uuid:candidates) {
      for (const auto& [sequence,index]:by_row.at(row_uuid)) {
        (void)sequence;
        const auto& row=row_at(index);
        const auto& creator=inventory.entries[sources[index].inventory_entry_index];
        const auto decision=EvaluateVersionEffectVisibility(MetadataForRow(row,creator),visibility);
        if (decision.decision==VisibilityDecision::requires_recovery) {
          auto r=fail(E::requires_recovery); r.diagnostic=decision.diagnostic; return r;
        }
        if (!decision.ok() && decision.decision!=VisibilityDecision::wait_for_transaction) {
          auto r=fail(E::visibility_failure); r.diagnostic=decision.diagnostic; return r;
        }
        result.observations.push_back({index,decision.decision});
        if (decision.decision!=VisibilityDecision::visible) {
          if (row.previous_row_version && !by_version.contains(row.previous_version_uuid)) return fail(E::missing_version);
          continue;
        }
        NativeCatalogVersionRow selected;
        selected.metadata=metadata_at(index); selected.version_uuid=row.version_uuid;
        selected.previous_version_uuid=row.previous_version_uuid;
        if (selected.metadata.record.header.kind == catalog::CatalogRecordKind::localized_name) {
          auto name=name_at(index); if (!name.ok()) return fail(E::invalid_chain);
          selected.name_payload=std::move(name.record->payload);
        }
        selected.provisional=!mga::HasCommittedInventoryOutcome(creator);
        selected.effective_lifecycle=selected.metadata.lifecycle;
        selected.effective_status=selected.metadata.status;
        if (selected.provisional) {
          selected.effective_status=catalog::CatalogObjectStatus::proposed;
          selected.effective_lifecycle=selected.metadata.record.header.deleted?catalog::CatalogObjectLifecycle::dropping:
              selected.metadata.definition_version==1?catalog::CatalogObjectLifecycle::creating:catalog::CatalogObjectLifecycle::altering;
        }
        result.rows.push_back(std::move(selected));
        break;
      }
    }
    const auto final_pin=pin.Resolve();
    if (!final_pin.ok()) { auto r=fail(E::snapshot_failure); r.diagnostic=final_pin.diagnostic; return r; }
    result.snapshot_uuid=snapshot.snapshot_uuid.value;
    result.error=E::none;
    return result;
  } catch (const std::bad_alloc&) { return fail(E::resource_exhausted); }
    catch (const std::length_error&) { return fail(E::resource_exhausted); }
    catch (...) { return fail(E::io_failure); }
}

NativeMetricCatalogReadResult ReadLocalNativeMetricCatalogFromOpenDevices(
    const scratchbird::core::platform::Uuid& database_uuid,
    const std::vector<scratchbird::storage::disk::NativeFilespaceDevice>& devices,
    const scratchbird::storage::disk::FilespaceRootReference& checkpoint,
    u16 catalog_selector, u16 relation_role, const NativeCatalogRelationBinding& relation,
    const scratchbird::transaction::mga::TransactionIdentity& reader,
    const scratchbird::transaction::mga::PublishedSnapshotPin& pin,
    const scratchbird::core::platform::Uuid& metric_uuid, u64 descriptor_generation,
    u64 maximum_retained_image_bytes) noexcept {
  using E = NativeMetricCatalogReadError;
  const auto fail = [](E error) { NativeMetricCatalogReadResult r; r.error=error; return r; };
  if (!scratchbird::core::uuid::IsEngineIdentityUuid(metric_uuid) || !descriptor_generation)
    return fail(E::invalid_request);
  try {
    NativeMetricCatalogReadResult result;
    result.source=ReadNativePinnedCatalogVersionsFromOpenDevices(database_uuid,devices,checkpoint,
        catalog_selector,relation_role,relation,reader,pin,maximum_retained_image_bytes);
    if (!result.source.ok()) { result.error=E::source_failure; return result; }
    std::vector<scratchbird::core::catalog::CatalogMetricRowView> views;
    views.reserve(result.source.rows.size());
    for (const auto& row : result.source.rows)
      views.push_back({&row.metadata,row.provisional});
    result.dependencies=scratchbird::core::catalog::ResolveLocalCatalogMetricBindings(
        views,metric_uuid,descriptor_generation);
    if (!result.dependencies.ok()) { result.error=E::dependency_failure; return result; }
    const auto final_pin=pin.Resolve();
    if (!final_pin.ok() || final_pin.descriptor.snapshot_uuid.value!=result.source.snapshot_uuid) {
      result.dependencies={};
      result.diagnostic=final_pin.diagnostic;
      result.error=E::snapshot_failure;
      return result;
    }
    result.error=E::none;
    return result;
  } catch (const std::bad_alloc&) { return fail(E::resource_exhausted); }
    catch (const std::length_error&) { return fail(E::resource_exhausted); }
    catch (...) { return fail(E::read_failure); }
}

NativeCatalogVersionReadResult ReadNativeCatalogVersionsFromOpenDevice(
    FileDevice& device, const TypedUuid& relation_uuid, u64 page_number,
    const VisibilitySnapshot& snapshot, bool latest_committed,
    const scratchbird::transaction::mga::TransactionIdentity& reader_identity,
    const scratchbird::transaction::mga::PublishedSnapshotPin* snapshot_pin) {
  namespace catalog = scratchbird::core::catalog;
  const auto guard = device.AcquireOperationGuard();
  const auto native = ReadPhysicalMgaCowRowsFromOpenDevice(device, relation_uuid,
      page_number, snapshot, latest_committed, reader_identity, snapshot_pin);
  if (!native.ok()) return Propagate<NativeCatalogVersionReadResult>(native.status, native.diagnostic);
  const auto context=LoadDatabaseContext(&device);
  if (!context.ok()) return Propagate<NativeCatalogVersionReadResult>(context.status,context.diagnostic);
  const auto catalog_rows=DecodeNativeCatalogRows(native.row_page,context.page_context.database_uuid,
      context.page_context.filespace_uuid);
  if (!catalog_rows.ok()) return Propagate<NativeCatalogVersionReadResult>(catalog_rows.status,catalog_rows.diagnostic);
  const auto& decoded=catalog_rows.metadata;
  if (native.recovery_required_count != 0)
    return ErrorResult<NativeCatalogVersionReadResult>("SB-ROW-VISIBILITY-REQUIRES-RECOVERY", "catalog.native_version.recovery_required");
  std::map<scratchbird::core::platform::Uuid,scratchbird::core::platform::Uuid> object_reservations;
  for (const auto& row : native.row_page.rows) {
    const auto creator=LookupLocalTransaction(native.inventory,MakeLocalTransactionId(row.local_transaction_id));
    if (!creator.ok()) return Propagate<NativeCatalogVersionReadResult>(creator.status,creator.diagnostic);
    if (!ReserveRetainedCatalogObject(object_reservations,decoded.at(row.version_uuid),creator.entry))
      return ErrorResult<NativeCatalogVersionReadResult>("CATALOG.INVALID_INPUT", "catalog.native_version.object_row_collision");
  }
  NativeCatalogVersionReadResult result;
  for (const auto& row : native.visible_rows) {
    const auto found = decoded.find(row.version_uuid);
    if (found == decoded.end())
      return ErrorResult<NativeCatalogVersionReadResult>("CATALOG.INVALID_INPUT", "catalog.native_version.version_missing");
    const auto creator = LookupLocalTransaction(native.inventory, MakeLocalTransactionId(row.local_transaction_id));
    if (!creator.ok()) return Propagate<NativeCatalogVersionReadResult>(creator.status, creator.diagnostic);
    NativeCatalogVersionRow visible;
    visible.metadata = found->second;
    visible.version_uuid = row.version_uuid; visible.previous_version_uuid = row.previous_version_uuid;
    if (const auto name=catalog_rows.names.find(row.version_uuid); name!=catalog_rows.names.end())
      visible.name_payload=name->second;
    visible.provisional = !scratchbird::transaction::mga::HasCommittedInventoryOutcome(creator.entry);
    visible.effective_lifecycle = visible.metadata.lifecycle;
    visible.effective_status = visible.metadata.status;
    if (visible.provisional) {
      visible.effective_status = catalog::CatalogObjectStatus::proposed;
      visible.effective_lifecycle = visible.metadata.record.header.deleted ? catalog::CatalogObjectLifecycle::dropping :
          visible.metadata.definition_version == 1 ? catalog::CatalogObjectLifecycle::creating : catalog::CatalogObjectLifecycle::altering;
    }
    result.rows.push_back(std::move(visible));
  }
  result.status = CowStoreOkStatus(); return result;
}

namespace {
struct PreparedNativeCatalogMutation {
  Status status;
  DiagnosticRecord diagnostic;
  PhysicalMgaCowMutation mutation;
  bool ok() const { return status.ok(); }
};

PreparedNativeCatalogMutation PrepareNativeCatalogVersion(
    FileDevice& device, const NativeCatalogVersionMutation& request) {
  namespace catalog = scratchbird::core::catalog;
  const auto guard = device.AcquireOperationGuard();
  const auto encoded = catalog::EncodeCatalogMetadataVersion(request.metadata);
  if (!encoded.ok()) return Propagate<PreparedNativeCatalogMutation>(encoded.status, encoded.diagnostic);
  const bool is_name=request.metadata.record.header.kind == catalog::CatalogRecordKind::localized_name;
  if (is_name != request.name_payload.has_value() || (is_name &&
      (!request.metadata.record.payload.empty() ||
       !catalog::CatalogNamePayloadMatchesMetadata(*request.name_payload,request.metadata))))
    return ErrorResult<PreparedNativeCatalogMutation>("CATALOG.INVALID_INPUT", "catalog.native_name.definition_required");
  if (!request.transaction.valid() || request.transaction.scope != scratchbird::transaction::mga::TransactionScope::local_node ||
      request.metadata.authority_scope == catalog::CatalogAuthorityScope::cluster ||
      !SameUuid(request.transaction.transaction_uuid, request.metadata.creator_transaction_uuid) ||
      request.transaction.local_id.value != request.metadata.creator_local_transaction_id ||
      (!request.expected_version_uuid.is_nil() && !scratchbird::core::uuid::IsEngineIdentityUuid(request.expected_version_uuid)))
    return ErrorResult<PreparedNativeCatalogMutation>("CATALOG.INVALID_INPUT", "catalog.native_version.writer_binding_invalid");
  VisibilitySnapshot snapshot;
  snapshot.reader_transaction = request.transaction.local_id;
  const auto current = ReadNativeCatalogVersionsFromOpenDevice(device, request.relation_uuid,
      request.page_number, snapshot, false, request.transaction);
  if (!current.ok()) return Propagate<PreparedNativeCatalogMutation>(current.status, current.diagnostic);
  // Visibility is not uniqueness authority: another transaction's active or
  // prepared row is hidden from this reader but still reserves its object UUID.
  // The device guard spans inspection and staging. Global catalog placement
  // must additionally enforce the same identity across pages/relations.
  const auto retained = ReadPhysicalMgaCowRowsFromOpenDevice(device, request.relation_uuid,
      request.page_number, snapshot, false, request.transaction);
  if (!retained.ok()) return Propagate<PreparedNativeCatalogMutation>(retained.status, retained.diagnostic);
  for (const auto& row : retained.row_page.rows) {
    if (row.row_uuid.value == request.metadata.record.header.row_uuid.value) continue;
    const auto creator = LookupLocalTransaction(retained.inventory, MakeLocalTransactionId(row.local_transaction_id));
    if (!creator.ok()) return Propagate<PreparedNativeCatalogMutation>(creator.status, creator.diagnostic);
    const auto outcome = scratchbird::transaction::mga::InventoryVisibilityState(creator.entry);
    if (outcome == scratchbird::transaction::mga::TransactionState::rolled_back ||
        outcome == scratchbird::transaction::mga::TransactionState::failed_terminal) continue;
    const auto candidate = catalog::DecodeCatalogMetadataVersion(row.cells[0].value.payload);
    if (!candidate.ok()) return Propagate<PreparedNativeCatalogMutation>(candidate.status, candidate.diagnostic);
    if (candidate.record.record.header.object_uuid.value == request.metadata.record.header.object_uuid.value)
      return ErrorResult<PreparedNativeCatalogMutation>("CATALOG.INVALID_INPUT", "catalog.native_version.object_row_collision");
  }
  const NativeCatalogVersionRow* previous = nullptr;
  for (const auto& row : current.rows) {
    if (row.metadata.record.header.row_uuid.value == request.metadata.record.header.row_uuid.value) previous = &row;
    else if (row.metadata.record.header.object_uuid.value == request.metadata.record.header.object_uuid.value)
      return ErrorResult<PreparedNativeCatalogMutation>("CATALOG.INVALID_INPUT", "catalog.native_version.object_row_collision");
  }
  if (request.expected_version_uuid.is_nil()) {
    if (previous || request.metadata.definition_version != 1 || request.metadata.record.header.deleted)
      return ErrorResult<PreparedNativeCatalogMutation>("CATALOG.INVALID_INPUT", "catalog.native_version.create_precondition");
  } else {
    if (!previous || previous->version_uuid != request.expected_version_uuid ||
        previous->metadata.record.header.object_uuid.value != request.metadata.record.header.object_uuid.value ||
        previous->metadata.record.header.kind != request.metadata.record.header.kind ||
        previous->metadata.record.header.deleted ||
        !catalog::CatalogSchemaDefinitionPreservesOrigin(previous->metadata,request.metadata) ||
        !catalog::CatalogMetricRetentionPolicyPreservesOrigin(previous->metadata,request.metadata) ||
        !catalog::CatalogMetricDescriptorPreservesOrigin(previous->metadata,request.metadata) ||
        !catalog::CatalogMetricSeriesPreservesOrigin(previous->metadata,request.metadata) ||
        !catalog::CatalogMetricLabelSchemaPreservesOrigin(previous->metadata,request.metadata) ||
        (is_name && (!previous->name_payload ||
         !catalog::CatalogNamePayloadPreservesIdentity(*previous->name_payload,*request.name_payload))) ||
        previous->metadata.definition_version == std::numeric_limits<u64>::max() ||
        request.metadata.definition_version != previous->metadata.definition_version + 1 ||
        request.metadata.schema_epoch < previous->metadata.schema_epoch ||
        request.metadata.security_epoch < previous->metadata.security_epoch ||
        request.metadata.resource_epoch < previous->metadata.resource_epoch ||
        request.metadata.catalog_generation < previous->metadata.catalog_generation ||
        request.metadata.dependency_generation < previous->metadata.dependency_generation ||
        request.metadata.invalidation_generation < previous->metadata.invalidation_generation)
      return ErrorResult<PreparedNativeCatalogMutation>("CATALOG.DEFINITION_VERSION_STALE", "catalog.native_version.replace_precondition");
  }
  PhysicalMgaCowMutation mutation;
  mutation.relation_uuid = request.relation_uuid; mutation.row_uuid = request.metadata.record.header.row_uuid;
  mutation.page_number = request.page_number; mutation.transaction_uuid = request.transaction.transaction_uuid;
  mutation.existing_local_transaction_id = request.transaction.local_id; mutation.use_existing_transaction = true;
  // Retirement is itself versioned metadata. Preserve its creator/audit fields
  // in an ordinary successor, not a payload-free physical DELETE marker.
  mutation.kind = previous ? PhysicalMgaCowMutationKind::update : PhysicalMgaCowMutationKind::insert;
  scratchbird::core::datatypes::DatatypeBinaryValue cell;
  cell.type_id = scratchbird::core::datatypes::CanonicalTypeId::binary; cell.payload = encoded.bytes;
  mutation.cells.push_back({1, std::move(cell)});
  if (is_name) {
    mutation.cells.clear();
    mutation.catalog_name=NativeCatalogNameMaterialization{request.metadata,*request.name_payload};
  }
  PreparedNativeCatalogMutation prepared;
  prepared.status = CowStoreOkStatus(); prepared.mutation = std::move(mutation);
  return prepared;
}

}  // namespace

PhysicalMgaCowMutationResult WriteNativeCatalogVersionToOpenDevice(
    FileDevice& device, const NativeCatalogVersionMutation& request) {
  const auto guard = device.AcquireOperationGuard();
  auto prepared = PrepareNativeCatalogVersion(device, request);
  if (!prepared.ok()) return Propagate<PhysicalMgaCowMutationResult>(prepared.status, prepared.diagnostic);
  return WritePhysicalMgaCowUnpublishedMutationToOpenDevice(device, prepared.mutation);
}

PhysicalMgaCowMutationBatchResult WriteNativeCatalogVersionsToOpenDevice(
    FileDevice& device, const std::vector<NativeCatalogVersionMutation>& requests) {
  const auto guard = device.AcquireOperationGuard();
  if (requests.empty())
    return ErrorResult<PhysicalMgaCowMutationBatchResult>("CATALOG.INVALID_INPUT", "catalog.native_version.batch_empty");
  const auto& owner = requests.front().transaction;
  std::set<scratchbird::core::platform::Uuid> rows, objects;
  PhysicalMgaCowMutationBatch batch;
  batch.mutations.reserve(requests.size());
  for (const auto& request : requests) {
    if (!SameUuid(request.transaction.transaction_uuid, owner.transaction_uuid) ||
        request.transaction.local_id.value != owner.local_id.value || request.transaction.scope != owner.scope ||
        !rows.insert(request.metadata.record.header.row_uuid.value).second ||
        !objects.insert(request.metadata.record.header.object_uuid.value).second)
      return ErrorResult<PhysicalMgaCowMutationBatchResult>("CATALOG.INVALID_INPUT", "catalog.native_version.batch_scope_or_identity_collision");
    auto prepared = PrepareNativeCatalogVersion(device, request);
    if (!prepared.ok()) return Propagate<PhysicalMgaCowMutationBatchResult>(prepared.status, prepared.diagnostic);
    batch.mutations.push_back(std::move(prepared.mutation));
  }
  return WritePhysicalMgaCowUnpublishedMutationBatchToOpenDevice(device, std::move(batch));
}

}  // namespace scratchbird::storage::database
