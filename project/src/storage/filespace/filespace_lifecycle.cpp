// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "filespace_lifecycle.hpp"

#include "disk_device.hpp"
#include "filespace_header.hpp"
#include "metric_contracts.hpp"
#include "metric_producer.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef UuidToString
#undef UuidToString
#endif
#endif

namespace scratchbird::storage::filespace {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::uuid::IsEngineIdentityUuid;

Status FilespaceOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::storage_disk};
}

Status FilespaceErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::storage_disk};
}

bool IsTypedEngineIdentity(const TypedUuid& uuid, UuidKind kind) {
  return uuid.kind == kind && uuid.valid() && IsEngineIdentityUuid(uuid.value);
}

bool AddOverflow(u64 left, u64 right, u64* value) {
  if (value == nullptr) {
    return true;
  }
  if (left > std::numeric_limits<u64>::max() - right) {
    return true;
  }
  *value = left + right;
  return false;
}

bool CapacityWindowValid(u64 total_pages,
                         u64 free_pages,
                         u64 preallocated_pages,
                         u64 allocation_root_page) {
  if (total_pages == 0 ||
      free_pages > total_pages ||
      preallocated_pages > total_pages) {
    return false;
  }
  u64 free_and_preallocated = 0;
  if (AddOverflow(free_pages, preallocated_pages, &free_and_preallocated) ||
      free_and_preallocated > total_pages) {
    return false;
  }
  return allocation_root_page == 0 || allocation_root_page < total_pages;
}

bool RequestHasCapacityConstraint(const FilespaceOperationRequest& request) {
  return request.physical_filespace_id != 0 ||
         request.total_pages != 0 ||
         request.free_pages != 0 ||
         request.preallocated_pages != 0 ||
         request.allocation_root_page != 0 ||
         request.header_generation != 0 ||
         request.writer_identity_uuid.valid();
}

bool RequestMatchesPhysicalHeader(const FilespaceOperationRequest& request,
                                  const PhysicalFilespaceHeader& header) {
  if (request.physical_filespace_id != 0 &&
      request.physical_filespace_id != header.physical_filespace_id) {
    return false;
  }
  if (request.total_pages != 0 && request.total_pages != header.total_pages) {
    return false;
  }
  if (request.free_pages != 0 && request.free_pages != header.free_pages) {
    return false;
  }
  if (request.preallocated_pages != 0 &&
      request.preallocated_pages != header.preallocated_pages) {
    return false;
  }
  if (request.allocation_root_page != 0 &&
      request.allocation_root_page != header.allocation_root_page) {
    return false;
  }
  if (request.header_generation != 0 &&
      request.header_generation != header.header_generation) {
    return false;
  }
  if (request.writer_identity_uuid.valid() &&
      request.writer_identity_uuid.value != header.writer_identity_uuid.value) {
    return false;
  }
  return true;
}

void ApplyPhysicalHeaderMetadata(FilespaceDescriptor* descriptor,
                                 const PhysicalFilespaceHeader& header) {
  if (descriptor == nullptr) {
    return;
  }
  descriptor->physical_filespace_id = header.physical_filespace_id;
  descriptor->total_pages = header.total_pages;
  descriptor->free_pages = header.free_pages;
  descriptor->preallocated_pages = header.preallocated_pages;
  descriptor->allocation_root_page = header.allocation_root_page;
  descriptor->header_generation = header.header_generation;
  descriptor->writer_identity_uuid = header.writer_identity_uuid;
}

void ApplyRequestCapacityMetadata(FilespaceDescriptor* descriptor,
                                  const FilespaceOperationRequest& request) {
  if (descriptor == nullptr) {
    return;
  }
  descriptor->physical_filespace_id = request.physical_filespace_id;
  descriptor->total_pages = request.total_pages == 0 ? 1 : request.total_pages;
  descriptor->free_pages = request.free_pages;
  descriptor->preallocated_pages = request.preallocated_pages;
  descriptor->allocation_root_page = request.allocation_root_page;
  descriptor->header_generation = request.header_generation == 0 ? 1 : request.header_generation;
  descriptor->writer_identity_uuid = request.writer_identity_uuid;
}

void EmitLifecycleMetric(const char* operation, const char* result, const char* reason) {
  (void)scratchbird::core::metrics::IncrementCounter(
      "sb_storage_filespace_lifecycle_total",
      scratchbird::core::metrics::Labels({{"component", "storage.filespace"},
                                          {"operation", operation},
                                          {"result", result},
                                          {"reason", reason}}),
      1.0,
      "storage_filespace");
}

void EmitPinMetric(const FilespaceDescriptor& descriptor) {
  (void)scratchbird::core::metrics::SetGauge(
      "sb_storage_filespace_active_pins",
      scratchbird::core::metrics::Labels({{"component", "storage.filespace"},
                                          {"role", FilespaceRoleName(descriptor.role)},
                                          {"state", FilespaceStateName(descriptor.state)}}),
      static_cast<double>(ActivePinCount(descriptor)),
      "storage_filespace");
}

double FilespaceRoleStateValue(FilespaceRole role) {
  switch (role) {
    case FilespaceRole::active_primary: return 1.0;
    case FilespaceRole::primary_shadow: return 2.0;
    case FilespaceRole::primary_candidate: return 3.0;
    case FilespaceRole::secondary_data:
    case FilespaceRole::secondary_index:
    case FilespaceRole::secondary_overflow:
    case FilespaceRole::secondary_history:
    case FilespaceRole::secondary_shard:
    case FilespaceRole::temporary:
    case FilespaceRole::import_candidate:
    case FilespaceRole::drop_pending:
    case FilespaceRole::forbidden:
      return 4.0;
    case FilespaceRole::archive_history:
    case FilespaceRole::archive_log:
    case FilespaceRole::archive_detached:
      return 5.0;
    case FilespaceRole::primary_snapshot:
      return 2.0;
    case FilespaceRole::unknown:
      return 0.0;
  }
  return 0.0;
}

double FilespaceHealthStateValue(FilespaceState state) {
  switch (state) {
    case FilespaceState::online:
    case FilespaceState::read_only:
      return 1.0;
    case FilespaceState::maintenance:
    case FilespaceState::moving:
    case FilespaceState::relocating_objects:
    case FilespaceState::promoting:
    case FilespaceState::demoting:
    case FilespaceState::detaching:
    case FilespaceState::drop_pending:
      return 2.0;
    case FilespaceState::quarantine:
      return 3.0;
    case FilespaceState::deleted:
    case FilespaceState::forbidden:
      return 4.0;
    case FilespaceState::absent:
    case FilespaceState::detached:
    case FilespaceState::archived:
    case FilespaceState::creating:
    case FilespaceState::initializing:
      return 0.0;
  }
  return 0.0;
}

void EmitFilespaceAuthorityMetrics(const FilespaceDescriptor& descriptor) {
  const std::string database_uuid = scratchbird::core::uuid::UuidToString(descriptor.database_uuid.value);
  const std::string filespace_uuid = scratchbird::core::uuid::UuidToString(descriptor.filespace_uuid.value);
  const std::string role = FilespaceRoleName(descriptor.role);
  const std::string state = FilespaceStateName(descriptor.state);
  (void)scratchbird::core::metrics::PublishFilespaceRoleState(FilespaceRoleStateValue(descriptor.role),
                                                              role,
                                                              database_uuid,
                                                              filespace_uuid,
                                                              {},
                                                              role,
                                                              "file");
  (void)scratchbird::core::metrics::PublishFilespaceHealthState(FilespaceHealthStateValue(descriptor.state),
                                                                state,
                                                                database_uuid,
                                                                filespace_uuid,
                                                                {},
                                                                role,
                                                                "file");
}

FilespaceOperationResult ErrorResult(std::string diagnostic_code,
                                     std::string message_key,
                                     const FilespaceOperationRequest& request,
                                     std::string detail = {}) {
  EmitLifecycleMetric(FilespaceOperationName(request.operation), "error", diagnostic_code.c_str());
  FilespaceOperationResult result;
  result.status = FilespaceErrorStatus();
  result.diagnostic = MakeFilespaceDiagnostic(result.status,
                                              std::move(diagnostic_code),
                                              std::move(message_key),
                                              std::move(detail));
  return result;
}

FilespaceOperationResult SuccessResult(FilespaceDescriptor descriptor,
                                       FilespaceEvidenceRecord evidence,
                                       bool durable_state_changed,
                                       bool metrics_emitted) {
  FilespaceOperationResult result;
  result.status = FilespaceOkStatus();
  result.descriptor = std::move(descriptor);
  result.evidence = std::move(evidence);
  result.durable_state_changed = durable_state_changed;
  result.cache_invalidation_required = durable_state_changed;
  result.metrics_emitted = metrics_emitted;
  return result;
}

FilespaceClassDecision FilespaceClassError(std::string diagnostic_code,
                                           std::string message_key,
                                           std::string detail = {}) {
  FilespaceClassDecision result;
  result.status = FilespaceErrorStatus();
  result.admitted = false;
  result.filespace_class = FilespaceClass::forbidden;
  result.diagnostic = MakeFilespaceDiagnostic(result.status,
                                              std::move(diagnostic_code),
                                              std::move(message_key),
                                              std::move(detail));
  result.evidence.push_back("filespace_class=forbidden");
  result.evidence.push_back("filespace_class_admitted=false");
  result.evidence.push_back("finality_authority=false");
  result.evidence.push_back("visibility_authority=false");
  result.evidence.push_back("mga_visibility_authority=durable_transaction_inventory");
  return result;
}

FilespaceDescriptor* FindMutable(FilespaceRegistry* registry, const TypedUuid& filespace_uuid) {
  if (registry == nullptr) {
    return nullptr;
  }
  for (FilespaceDescriptor& descriptor : registry->filespaces) {
    if (descriptor.filespace_uuid.value == filespace_uuid.value) {
      return &descriptor;
    }
  }
  return nullptr;
}

const FilespaceDescriptor* FindPrimary(const FilespaceRegistry& registry) {
  for (const FilespaceDescriptor& descriptor : registry.filespaces) {
    if (descriptor.role == FilespaceRole::active_primary &&
        descriptor.state != FilespaceState::detached &&
        descriptor.state != FilespaceState::dropped) {
      return &descriptor;
    }
  }
  return nullptr;
}

bool IsPrimaryCapableRole(FilespaceRole role) {
  return role == FilespaceRole::active_primary ||
         role == FilespaceRole::primary_shadow ||
         role == FilespaceRole::primary_snapshot ||
         role == FilespaceRole::primary_candidate;
}

bool IsArchiveRole(FilespaceRole role) {
  return role == FilespaceRole::archive_history ||
         role == FilespaceRole::archive_log ||
         role == FilespaceRole::archive_detached;
}

struct FilespaceClassDescriptor {
  FilespaceObjectClass object_class = FilespaceObjectClass::unspecified;
  FilespaceClass filespace_class = FilespaceClass::unknown;
  FilespaceRole role = FilespaceRole::unknown;
  const char* page_family = "";
  const char* reason = "";
};

FilespaceClassDescriptor DescriptorForObjectClass(FilespaceObjectClass object_class) {
  switch (object_class) {
    case FilespaceObjectClass::hot_row:
      return {object_class,
              FilespaceClass::hot_row,
              FilespaceRole::secondary_data,
              "data",
              "hot_row_mutable_data_pages"};
    case FilespaceObjectClass::cold_row:
      return {object_class,
              FilespaceClass::cold_row,
              FilespaceRole::secondary_history,
              "data",
              "cold_row_retention_pages"};
    case FilespaceObjectClass::secondary_delta_ledger:
      return {object_class,
              FilespaceClass::secondary_delta_ledger,
              FilespaceRole::secondary_index,
              "index",
              "secondary_delta_ledger_append_pages"};
    case FilespaceObjectClass::exact_index:
      return {object_class,
              FilespaceClass::exact_index,
              FilespaceRole::secondary_index,
              "index",
              "exact_index_lookup_pages"};
    case FilespaceObjectClass::immutable_generation:
      return {object_class,
              FilespaceClass::immutable_generation,
              FilespaceRole::secondary_history,
              "index",
              "immutable_generation_published_pages"};
    case FilespaceObjectClass::large_blob:
      return {object_class,
              FilespaceClass::large_blob,
              FilespaceRole::secondary_overflow,
              "blob",
              "large_value_blob_payload_pages"};
    case FilespaceObjectClass::temp_spill:
      return {object_class,
              FilespaceClass::temp_spill,
              FilespaceRole::temporary,
              "overflow",
              "temporary_spill_pages"};
    case FilespaceObjectClass::backup_stream:
      return {object_class,
              FilespaceClass::backup_stream,
              FilespaceRole::archive_log,
              "archive",
              "backup_stream_pages"};
    case FilespaceObjectClass::unspecified:
      break;
  }
  return {};
}

bool PageFamilyCompatibleWithClass(const std::string& page_family,
                                   const FilespaceClassDescriptor& descriptor) {
  if (page_family.empty()) {
    return true;
  }
  switch (descriptor.filespace_class) {
    case FilespaceClass::hot_row:
    case FilespaceClass::cold_row:
      return page_family == "data" || page_family == "row_data";
    case FilespaceClass::secondary_delta_ledger:
    case FilespaceClass::exact_index:
    case FilespaceClass::immutable_generation:
      return page_family == "index" || page_family == "index_btree" ||
             page_family == "index_btree_root" ||
             page_family == "index_btree_branch" ||
             page_family == "index_btree_leaf" ||
             page_family == "index_btree_posting" ||
             page_family == "index_hash" || page_family == "index_bitmap" ||
             page_family == "index_summary" ||
             page_family == "index_inverted" ||
             page_family == "index_spatial" ||
             page_family == "index_vector" ||
             page_family == "index_graph" ||
             page_family == "index_statistics" ||
             page_family == "index_special_root" ||
             page_family == "catalog" ||
             page_family == "config_root" ||
             page_family == "security_root" ||
             page_family == "allocation" ||
             page_family == "allocation_map" ||
             page_family == "startup" ||
             page_family == "database_header" ||
             page_family == "system_state" ||
             page_family == "bootstrap_reserved" ||
             page_family == "filespace_directory" ||
             page_family == "transaction" ||
             page_family == "transaction_inventory" ||
             page_family == "metrics";
    case FilespaceClass::large_blob:
      return page_family == "blob" || page_family == "overflow" ||
             page_family == "toast" || page_family == "columnar" ||
             page_family == "vector" || page_family == "graph";
    case FilespaceClass::temp_spill:
      return page_family == "overflow" || page_family == "toast" ||
             page_family == "index_temporary";
    case FilespaceClass::backup_stream:
      return page_family == "archive" || page_family == "blob" ||
             page_family == "overflow";
    case FilespaceClass::unknown:
    case FilespaceClass::forbidden:
      break;
  }
  return false;
}

void ApplyRoleDefaults(FilespaceDescriptor* descriptor) {
  descriptor->startup_authority = descriptor->role == FilespaceRole::active_primary;
  descriptor->catalog_persistence_owner = descriptor->role == FilespaceRole::active_primary;
  descriptor->filespace_manifest_owner = descriptor->role == FilespaceRole::active_primary;
  descriptor->recovery_evidence_owner = descriptor->role == FilespaceRole::active_primary;
  descriptor->archive_owner = IsArchiveRole(descriptor->role);
  descriptor->history_owner = descriptor->role == FilespaceRole::secondary_history ||
                              descriptor->role == FilespaceRole::archive_history;
  if (descriptor->role == FilespaceRole::active_primary) {
    descriptor->read_only = false;
    descriptor->state = FilespaceState::attached;
    descriptor->active = true;
  } else if (descriptor->role == FilespaceRole::primary_shadow ||
             descriptor->role == FilespaceRole::primary_snapshot ||
             descriptor->role == FilespaceRole::archive_history ||
             descriptor->role == FilespaceRole::archive_log ||
             descriptor->role == FilespaceRole::archive_detached ||
             descriptor->role == FilespaceRole::import_candidate ||
             descriptor->role == FilespaceRole::drop_pending ||
             descriptor->role == FilespaceRole::forbidden) {
    descriptor->read_only = true;
  }
}

FilespaceEvidenceRecord Evidence(FilespaceRegistry* registry,
                                 const FilespaceOperationRequest& request,
                                 const FilespaceDescriptor& before,
                                 const FilespaceDescriptor& after,
                                 std::string diagnostic_code,
                                 bool durable_state_changed) {
  FilespaceEvidenceRecord evidence;
  evidence.sequence = registry == nullptr ? 0 : registry->next_evidence_sequence++;
  evidence.operation = request.operation;
  evidence.filespace_uuid = request.filespace_uuid;
  evidence.previous_state = before.state;
  evidence.new_state = after.state;
  evidence.previous_role = before.role;
  evidence.new_role = after.role;
  evidence.reason = request.reason;
  evidence.diagnostic_code = std::move(diagnostic_code);
  evidence.durable_state_changed = durable_state_changed;
  if (registry != nullptr && request.policy.evidence_before_success) {
    registry->evidence.push_back(evidence);
  }
  return evidence;
}

bool HasActivePins(const FilespaceDescriptor& descriptor) {
  return ActivePinCount(descriptor) != 0;
}

FilespaceOperationResult ValidateRequest(const FilespaceOperationRequest& request) {
  if (!IsTypedEngineIdentity(request.database_uuid, UuidKind::database)) {
    return ErrorResult("SB-FILESPACE-LIFECYCLE-DATABASE-UUID-MUST-BE-V7",
                       "storage.filespace.database_uuid_must_be_v7",
                       request);
  }
  if (!IsTypedEngineIdentity(request.filespace_uuid, UuidKind::filespace)) {
    return ErrorResult("SB-FILESPACE-LIFECYCLE-FILESPACE-UUID-MUST-BE-V7",
                       "storage.filespace.filespace_uuid_must_be_v7",
                       request);
  }
  if (!scratchbird::storage::disk::IsSupportedDatabasePageSize(request.page_size)) {
    return ErrorResult("SB-FILESPACE-LIFECYCLE-PAGE-SIZE-INVALID",
                       "storage.filespace.page_size_invalid",
                       request,
                       std::to_string(request.page_size));
  }
  const bool request_writer_required =
      request.operation == FilespaceOperation::create_filespace ||
      (request.operation == FilespaceOperation::attach_filespace &&
       (!request.policy.require_physical_header_for_attach ||
        request.role == FilespaceRole::active_primary));
  if (request_writer_required &&
      !IsTypedEngineIdentity(request.writer_identity_uuid, UuidKind::object)) {
    return ErrorResult("SB-FILESPACE-LIFECYCLE-WRITER-UUID-MUST-BE-V7",
                       "storage.filespace.writer_uuid_must_be_v7",
                       request);
  }
  if (request.writer_identity_uuid.valid() &&
      !IsTypedEngineIdentity(request.writer_identity_uuid, UuidKind::object)) {
    return ErrorResult("SB-FILESPACE-LIFECYCLE-WRITER-UUID-MUST-BE-V7",
                       "storage.filespace.writer_uuid_must_be_v7",
                       request);
  }
  if (request.operation == FilespaceOperation::create_filespace) {
    const u64 total_pages = request.total_pages == 0 ? 1 : request.total_pages;
    if (!CapacityWindowValid(total_pages,
                             request.free_pages,
                             request.preallocated_pages,
                             request.allocation_root_page)) {
      return ErrorResult("SB-FILESPACE-LIFECYCLE-CAPACITY-WINDOW-INVALID",
                         "storage.filespace.capacity_window_invalid",
                         request);
    }
  } else if (RequestHasCapacityConstraint(request) && request.total_pages != 0 &&
             !CapacityWindowValid(request.total_pages,
                                  request.free_pages,
                                  request.preallocated_pages,
                                  request.allocation_root_page)) {
    return ErrorResult("SB-FILESPACE-LIFECYCLE-CAPACITY-WINDOW-INVALID",
                       "storage.filespace.capacity_window_invalid",
                       request);
  }
  if ((request.operation == FilespaceOperation::create_filespace ||
       request.operation == FilespaceOperation::attach_filespace) &&
      request.path.empty()) {
    return ErrorResult("SB-FILESPACE-LIFECYCLE-PATH-REQUIRED",
                       "storage.filespace.path_required",
                       request);
  }

  FilespaceOperationResult result;
  result.status = FilespaceOkStatus();
  return result;
}

std::string EscapeField(std::string value) {
  for (char& ch : value) {
    if (ch == '\n' || ch == '\r' || ch == '|') {
      ch = ' ';
    }
  }
  return value;
}

std::string Row(const std::vector<std::string>& fields) {
  std::string row;
  for (std::size_t index = 0; index < fields.size(); ++index) {
    if (index != 0) {
      row += '|';
    }
    row += EscapeField(fields[index]);
  }
  row += '\n';
  return row;
}

u16 ParseU16(const std::string& value) {
  return static_cast<u16>(std::strtoul(value.c_str(), nullptr, 10));
}

u32 ParseU32(const std::string& value) {
  return static_cast<u32>(std::strtoul(value.c_str(), nullptr, 10));
}

u64 ParseU64(const std::string& value) {
  return static_cast<u64>(std::strtoull(value.c_str(), nullptr, 10));
}

bool ParseStrictU64(const std::string& value, u64* parsed) {
  if (parsed == nullptr || value.empty()) {
    return false;
  }
  u64 accumulator = 0;
  for (char ch : value) {
    if (ch < '0' || ch > '9') {
      return false;
    }
    const u64 digit = static_cast<u64>(ch - '0');
    if (accumulator > (std::numeric_limits<u64>::max() - digit) / 10) {
      return false;
    }
    accumulator = accumulator * 10 + digit;
  }
  *parsed = accumulator;
  return true;
}

std::vector<std::string> SplitRow(const std::string& row) {
  std::vector<std::string> fields;
  std::stringstream stream(row);
  std::string field;
  while (std::getline(stream, field, '|')) {
    fields.push_back(field);
  }
  return fields;
}

FilespaceRole ParseRole(u16 value) {
  switch (static_cast<FilespaceRole>(value)) {
    case FilespaceRole::active_primary:
    case FilespaceRole::primary_shadow:
    case FilespaceRole::primary_snapshot:
    case FilespaceRole::primary_candidate:
    case FilespaceRole::secondary_data:
    case FilespaceRole::secondary_index:
    case FilespaceRole::secondary_overflow:
    case FilespaceRole::secondary_history:
    case FilespaceRole::secondary_shard:
    case FilespaceRole::archive_history:
    case FilespaceRole::archive_log:
    case FilespaceRole::archive_detached:
    case FilespaceRole::temporary:
    case FilespaceRole::import_candidate:
    case FilespaceRole::drop_pending:
    case FilespaceRole::forbidden:
      return static_cast<FilespaceRole>(value);
    default:
      return FilespaceRole::unknown;
  }
}

FilespaceState ParseState(u16 value) {
  switch (static_cast<FilespaceState>(value)) {
    case FilespaceState::online:
    case FilespaceState::read_only:
    case FilespaceState::detached:
    case FilespaceState::archived:
    case FilespaceState::deleted:
    case FilespaceState::creating:
    case FilespaceState::initializing:
    case FilespaceState::maintenance:
    case FilespaceState::moving:
    case FilespaceState::relocating_objects:
    case FilespaceState::promoting:
    case FilespaceState::demoting:
    case FilespaceState::detaching:
    case FilespaceState::drop_pending:
    case FilespaceState::quarantine:
    case FilespaceState::forbidden:
      return static_cast<FilespaceState>(value);
    default:
      return FilespaceState::absent;
  }
}

FilespacePinKind ParsePinKind(u16 value) {
  switch (static_cast<FilespacePinKind>(value)) {
    case FilespacePinKind::page_owner:
    case FilespacePinKind::transaction:
    case FilespacePinKind::backup:
    case FilespacePinKind::archive:
    case FilespacePinKind::catalog:
    case FilespacePinKind::external:
      return static_cast<FilespacePinKind>(value);
  }
  return FilespacePinKind::external;
}

FilespaceOperation ParseOperation(u16 value) {
  switch (static_cast<FilespaceOperation>(value)) {
    case FilespaceOperation::create_filespace:
    case FilespaceOperation::attach_filespace:
    case FilespaceOperation::detach_filespace:
    case FilespaceOperation::promote_filespace:
    case FilespaceOperation::demote_filespace:
    case FilespaceOperation::set_read_only:
    case FilespaceOperation::set_read_write:
    case FilespaceOperation::assign_archive_owner:
    case FilespaceOperation::assign_history_owner:
    case FilespaceOperation::pin_filespace:
    case FilespaceOperation::unpin_filespace:
    case FilespaceOperation::verify_filespace:
    case FilespaceOperation::compact_filespace:
    case FilespaceOperation::truncate_filespace:
    case FilespaceOperation::drop_filespace:
      return static_cast<FilespaceOperation>(value);
  }
  return FilespaceOperation::verify_filespace;
}

constexpr const char* kFilespaceManifestHeader =
    "scratchbird.filespace.registry.manifest.v1";
constexpr const char* kFilespaceManifestChecksumAlgorithm =
    "fnv1a64-filespace-registry-v1";
constexpr u64 kFnv1a64OffsetBasis = 14695981039346656037ull;
constexpr u64 kFnv1a64Prime = 1099511628211ull;

std::string Hex64(u64 value) {
  std::ostringstream stream;
  stream << std::hex << std::setfill('0') << std::setw(16) << value;
  return stream.str();
}

std::string FilespaceManifestChecksum(const std::string& payload) {
  u64 hash = kFnv1a64OffsetBasis;
  for (unsigned char ch : payload) {
    hash ^= static_cast<u64>(ch);
    hash *= kFnv1a64Prime;
  }
  return Hex64(hash);
}

std::filesystem::path FilespaceManifestTempPath(const std::filesystem::path& path) {
  return std::filesystem::path(path.string() + ".tmp");
}

bool ReplaceFileAtomically(const std::filesystem::path& temp_path,
                           const std::filesystem::path& target_path,
                           std::string* detail) {
#if defined(_WIN32)
  if (::MoveFileExW(temp_path.wstring().c_str(),
                    target_path.wstring().c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
    return true;
  }
  if (detail != nullptr) {
    *detail = "windows_error_" + std::to_string(::GetLastError());
  }
  return false;
#else
  std::error_code ec;
  std::filesystem::rename(temp_path, target_path, ec);
  if (!ec) {
    return true;
  }
  if (detail != nullptr) {
    *detail = ec.message();
  }
  return false;
#endif
}

FilespaceRegistryManifestResult ManifestWriteError(FilespaceRegistryManifestResult result,
                                                   std::string diagnostic_code,
                                                   std::string message_key,
                                                   std::string detail = {}) {
  result.status = FilespaceErrorStatus();
  result.diagnostic = MakeFilespaceDiagnostic(result.status,
                                              std::move(diagnostic_code),
                                              std::move(message_key),
                                              std::move(detail));
  return result;
}

FilespaceRegistryManifestLoadResult ManifestLoadError(FilespaceRegistryManifestLoadResult result,
                                                      std::string diagnostic_code,
                                                      std::string message_key,
                                                      std::string detail = {}) {
  result.status = FilespaceErrorStatus();
  result.diagnostic = MakeFilespaceDiagnostic(result.status,
                                              std::move(diagnostic_code),
                                              std::move(message_key),
                                              std::move(detail));
  return result;
}

}  // namespace

const char* FilespaceRoleName(FilespaceRole role) {
  switch (role) {
    case FilespaceRole::unknown: return "unknown";
    case FilespaceRole::active_primary: return "active_primary";
    case FilespaceRole::primary_shadow: return "primary_shadow";
    case FilespaceRole::primary_snapshot: return "primary_snapshot";
    case FilespaceRole::primary_candidate: return "primary_candidate";
    case FilespaceRole::secondary_data: return "secondary_data";
    case FilespaceRole::secondary_index: return "secondary_index";
    case FilespaceRole::secondary_overflow: return "secondary_overflow";
    case FilespaceRole::secondary_history: return "secondary_history";
    case FilespaceRole::secondary_shard: return "secondary_shard";
    case FilespaceRole::archive_history: return "archive_history";
    case FilespaceRole::archive_log: return "archive_log";
    case FilespaceRole::archive_detached: return "archive_detached";
    case FilespaceRole::temporary: return "temporary";
    case FilespaceRole::import_candidate: return "import_candidate";
    case FilespaceRole::drop_pending: return "drop_pending";
    case FilespaceRole::forbidden: return "forbidden";
  }
  return "unknown";
}

const char* FilespaceStateName(FilespaceState state) {
  switch (state) {
    case FilespaceState::absent: return "absent";
    case FilespaceState::online: return "online";
    case FilespaceState::read_only: return "read_only";
    case FilespaceState::detached: return "detached";
    case FilespaceState::archived: return "archived";
    case FilespaceState::deleted: return "deleted";
    case FilespaceState::creating: return "creating";
    case FilespaceState::initializing: return "initializing";
    case FilespaceState::maintenance: return "maintenance";
    case FilespaceState::moving: return "moving";
    case FilespaceState::relocating_objects: return "relocating_objects";
    case FilespaceState::promoting: return "promoting";
    case FilespaceState::demoting: return "demoting";
    case FilespaceState::detaching: return "detaching";
    case FilespaceState::drop_pending: return "drop_pending";
    case FilespaceState::quarantine: return "quarantine";
    case FilespaceState::forbidden: return "forbidden";
  }
  return "unknown";
}

const char* FilespaceOperationName(FilespaceOperation operation) {
  switch (operation) {
    case FilespaceOperation::create_filespace: return "create_filespace";
    case FilespaceOperation::attach_filespace: return "attach_filespace";
    case FilespaceOperation::detach_filespace: return "detach_filespace";
    case FilespaceOperation::promote_filespace: return "promote_filespace";
    case FilespaceOperation::demote_filespace: return "demote_filespace";
    case FilespaceOperation::set_read_only: return "set_read_only";
    case FilespaceOperation::set_read_write: return "set_read_write";
    case FilespaceOperation::assign_archive_owner: return "assign_archive_owner";
    case FilespaceOperation::assign_history_owner: return "assign_history_owner";
    case FilespaceOperation::pin_filespace: return "pin_filespace";
    case FilespaceOperation::unpin_filespace: return "unpin_filespace";
    case FilespaceOperation::verify_filespace: return "verify_filespace";
    case FilespaceOperation::compact_filespace: return "compact_filespace";
    case FilespaceOperation::truncate_filespace: return "truncate_filespace";
    case FilespaceOperation::drop_filespace: return "drop_filespace";
  }
  return "unknown";
}

const char* FilespacePinKindName(FilespacePinKind kind) {
  switch (kind) {
    case FilespacePinKind::page_owner: return "page_owner";
    case FilespacePinKind::transaction: return "transaction";
    case FilespacePinKind::backup: return "backup";
    case FilespacePinKind::archive: return "archive";
    case FilespacePinKind::catalog: return "catalog";
    case FilespacePinKind::external: return "external";
  }
  return "unknown";
}

const char* FilespaceObjectClassName(FilespaceObjectClass object_class) {
  switch (object_class) {
    case FilespaceObjectClass::unspecified: return "unspecified";
    case FilespaceObjectClass::hot_row: return "hot_row";
    case FilespaceObjectClass::cold_row: return "cold_row";
    case FilespaceObjectClass::secondary_delta_ledger: return "secondary_delta_ledger";
    case FilespaceObjectClass::exact_index: return "exact_index";
    case FilespaceObjectClass::immutable_generation: return "immutable_generation";
    case FilespaceObjectClass::large_blob: return "large_blob";
    case FilespaceObjectClass::temp_spill: return "temp_spill";
    case FilespaceObjectClass::backup_stream: return "backup_stream";
  }
  return "unknown";
}

const char* FilespaceClassName(FilespaceClass filespace_class) {
  switch (filespace_class) {
    case FilespaceClass::unknown: return "unknown";
    case FilespaceClass::hot_row: return "hot_row";
    case FilespaceClass::cold_row: return "cold_row";
    case FilespaceClass::secondary_delta_ledger: return "secondary_delta_ledger";
    case FilespaceClass::exact_index: return "exact_index";
    case FilespaceClass::immutable_generation: return "immutable_generation";
    case FilespaceClass::large_blob: return "large_blob";
    case FilespaceClass::temp_spill: return "temp_spill";
    case FilespaceClass::backup_stream: return "backup_stream";
    case FilespaceClass::forbidden: return "forbidden";
  }
  return "unknown";
}

u64 ActivePinCount(const FilespaceDescriptor& descriptor) {
  u64 total = 0;
  for (const FilespacePin& pin : descriptor.pins) {
    total += pin.count;
  }
  return total;
}

FilespaceObjectClass DefaultFilespaceObjectClassForPageFamily(const std::string& page_family) {
  if (page_family == "data" || page_family == "row_data") {
    return FilespaceObjectClass::hot_row;
  }
  if (page_family == "index" || page_family == "index_btree" ||
      page_family == "index_btree_root" || page_family == "index_btree_branch" ||
      page_family == "index_btree_leaf" || page_family == "index_btree_posting" ||
      page_family == "index_hash" || page_family == "index_bitmap" ||
      page_family == "index_summary" || page_family == "index_inverted" ||
      page_family == "index_spatial" || page_family == "index_vector" ||
      page_family == "index_graph" || page_family == "index_statistics" ||
      page_family == "index_special_root") {
    return FilespaceObjectClass::exact_index;
  }
  if (page_family == "blob" || page_family == "overflow" || page_family == "toast") {
    return FilespaceObjectClass::large_blob;
  }
  if (page_family == "columnar" || page_family == "vector" ||
      page_family == "graph") {
    return FilespaceObjectClass::large_blob;
  }
  if (page_family == "archive") {
    return FilespaceObjectClass::backup_stream;
  }
  if (page_family == "catalog" || page_family == "config_root" ||
      page_family == "security_root" || page_family == "allocation" ||
      page_family == "allocation_map" || page_family == "startup" ||
      page_family == "database_header" || page_family == "system_state" ||
      page_family == "bootstrap_reserved" ||
      page_family == "filespace_directory" || page_family == "transaction" ||
      page_family == "transaction_inventory" || page_family == "metrics") {
    return FilespaceObjectClass::immutable_generation;
  }
  return FilespaceObjectClass::hot_row;
}

FilespaceClassDecision ResolveFilespaceClass(const FilespaceClassRequest& request) {
  if (!IsTypedEngineIdentity(request.database_uuid, UuidKind::database)) {
    return FilespaceClassError("SB-FILESPACE-CLASS-DATABASE-UUID-INVALID",
                               "storage.filespace_class.database_uuid_invalid");
  }
  if (!IsTypedEngineIdentity(request.filespace_uuid, UuidKind::filespace)) {
    return FilespaceClassError("SB-FILESPACE-CLASS-FILESPACE-UUID-INVALID",
                               "storage.filespace_class.filespace_uuid_invalid");
  }
  FilespaceObjectClass object_class = request.object_class;
  if (object_class == FilespaceObjectClass::unspecified) {
    object_class = DefaultFilespaceObjectClassForPageFamily(request.page_family);
  }
  if (request.explicit_object_class &&
      !IsTypedEngineIdentity(request.owner_object_uuid, UuidKind::object)) {
    return FilespaceClassError("SB-FILESPACE-CLASS-OWNER-OBJECT-UUID-INVALID",
                               "storage.filespace_class.owner_object_uuid_invalid",
                               FilespaceObjectClassName(object_class));
  }

  const FilespaceClassDescriptor descriptor = DescriptorForObjectClass(object_class);
  if (descriptor.filespace_class == FilespaceClass::unknown) {
    return FilespaceClassError("SB-FILESPACE-CLASS-OBJECT-CLASS-UNKNOWN",
                               "storage.filespace_class.object_class_unknown",
                               FilespaceObjectClassName(object_class));
  }
  if (!PageFamilyCompatibleWithClass(request.page_family, descriptor)) {
    return FilespaceClassError("SB-FILESPACE-CLASS-PAGE-FAMILY-INCOMPATIBLE",
                               "storage.filespace_class.page_family_incompatible",
                               request.page_family + ":" +
                                   FilespaceObjectClassName(object_class));
  }

  FilespaceClassDecision result;
  result.status = FilespaceOkStatus();
  result.admitted = true;
  result.object_class = object_class;
  result.filespace_class = descriptor.filespace_class;
  result.recommended_role = descriptor.role;
  result.page_family = request.page_family.empty() ? descriptor.page_family : request.page_family;
  result.diagnostic = MakeFilespaceDiagnostic(result.status,
                                              "SB-FILESPACE-CLASS-RESOLVED",
                                              "storage.filespace_class.resolved",
                                              descriptor.reason);
  result.evidence.push_back(std::string("object_class=") + FilespaceObjectClassName(object_class));
  result.evidence.push_back(std::string("filespace_class=") + FilespaceClassName(descriptor.filespace_class));
  result.evidence.push_back(std::string("filespace_role=") + FilespaceRoleName(descriptor.role));
  result.evidence.push_back(std::string("page_family=") + result.page_family);
  result.evidence.push_back(std::string("selection_reason=") + descriptor.reason);
  result.evidence.push_back("filespace_class_admitted=true");
  result.evidence.push_back("finality_authority=false");
  result.evidence.push_back("visibility_authority=false");
  result.evidence.push_back("uuid_ordering_finality_authority=false");
  result.evidence.push_back("mga_visibility_authority=durable_transaction_inventory");
  result.evidence.push_back("timestamp_lifecycle_ordering_finality_authority=false");
  return result;
}

FilespaceOperationResult ApplyFilespaceOperation(FilespaceRegistry* registry,
                                                 const FilespaceOperationRequest& request) {
  if (registry == nullptr) {
    return ErrorResult("SB-FILESPACE-LIFECYCLE-REGISTRY-NULL",
                       "storage.filespace.registry_null",
                       request);
  }
  const auto request_valid = ValidateRequest(request);
  if (!request_valid.ok()) {
    return request_valid;
  }

  FilespaceDescriptor* descriptor = FindMutable(registry, request.filespace_uuid);
  const FilespaceDescriptor before = descriptor == nullptr ? FilespaceDescriptor{} : *descriptor;
  FilespaceDescriptor after = before;
  bool durable_state_changed = false;

  switch (request.operation) {
    case FilespaceOperation::create_filespace:
    case FilespaceOperation::attach_filespace: {
      if (descriptor != nullptr && descriptor->state != FilespaceState::detached && descriptor->state != FilespaceState::dropped) {
        return ErrorResult("SB-FILESPACE-LIFECYCLE-ALREADY-ATTACHED",
                           "storage.filespace.already_attached",
                           request);
      }
      if (request.role == FilespaceRole::active_primary && FindPrimary(*registry) != nullptr) {
        return ErrorResult("SB-FILESPACE-LIFECYCLE-PRIMARY-ALREADY-EXISTS",
                           "storage.filespace.primary_already_exists",
                           request);
      }
      PhysicalFilespaceHeaderResult physical;
      const bool should_read_physical =
          request.operation == FilespaceOperation::attach_filespace &&
          request.policy.require_physical_header_for_attach &&
          request.role != FilespaceRole::active_primary;
      if (should_read_physical) {
        physical = ReadPhysicalFilespaceHeader(request.path);
        if (!physical.ok()) {
          return ErrorResult("SB-FILESPACE-LIFECYCLE-ATTACH-PHYSICAL-HEADER-INVALID",
                             "storage.filespace.attach_physical_header_invalid",
                             request,
                             physical.diagnostic.diagnostic_code);
        }
        if (!(physical.header.database_uuid.value == request.database_uuid.value) ||
            !(physical.header.filespace_uuid.value == request.filespace_uuid.value) ||
            physical.header.role != request.role ||
            physical.header.page_size != request.page_size) {
          return ErrorResult("SB-FILESPACE-LIFECYCLE-ATTACH-PHYSICAL-HEADER-MISMATCH",
                             "storage.filespace.attach_physical_header_mismatch",
                             request);
        }
        if (!RequestMatchesPhysicalHeader(request, physical.header)) {
          return ErrorResult("SB-FILESPACE-LIFECYCLE-ATTACH-PHYSICAL-HEADER-MISMATCH",
                             "storage.filespace.attach_physical_header_mismatch",
                             request);
        }
      }
      after.database_uuid = request.database_uuid;
      after.filespace_uuid = request.filespace_uuid;
      after.path = request.path;
      after.role = request.role;
      after.state = FilespaceState::attached;
      after.page_size = request.page_size;
      after.read_only = false;
      after.active = true;
      after.first_filespace = registry->filespaces.empty();
      ApplyRoleDefaults(&after);
      if (should_read_physical) {
        ApplyPhysicalHeaderMetadata(&after, physical.header);
      } else {
        ApplyRequestCapacityMetadata(&after, request);
      }
      ++after.generation;
      if (descriptor == nullptr) {
        registry->filespaces.push_back(after);
        descriptor = &registry->filespaces.back();
      } else {
        *descriptor = after;
      }
      durable_state_changed = true;
      break;
    }
    case FilespaceOperation::detach_filespace:
      if (descriptor == nullptr || descriptor->state == FilespaceState::dropped) {
        return ErrorResult("SB-FILESPACE-LIFECYCLE-NOT-FOUND",
                           "storage.filespace.not_found",
                           request);
      }
      if (descriptor->role == FilespaceRole::active_primary && !request.policy.allow_primary_detach) {
        return ErrorResult("SB-FILESPACE-LIFECYCLE-PRIMARY-DETACH-FORBIDDEN",
                           "storage.filespace.primary_detach_forbidden",
                           request);
      }
      if (request.policy.require_no_active_pins_for_detach && HasActivePins(*descriptor)) {
        return ErrorResult("SB-FILESPACE-LIFECYCLE-DETACH-PINNED",
                           "storage.filespace.detach_pinned",
                           request,
                           std::to_string(ActivePinCount(*descriptor)));
      }
      after.state = FilespaceState::detached;
      after.active = false;
      after.read_only = true;
      ++after.generation;
      *descriptor = after;
      durable_state_changed = true;
      break;
    case FilespaceOperation::promote_filespace:
      if (descriptor == nullptr || descriptor->state == FilespaceState::dropped) {
        return ErrorResult("SB-FILESPACE-LIFECYCLE-NOT-FOUND",
                           "storage.filespace.not_found",
                           request);
      }
      if (!request.policy.allow_promotion) {
        return ErrorResult("SB-FILESPACE-LIFECYCLE-PROMOTE-FORBIDDEN",
                           "storage.filespace.promote_forbidden",
                           request);
      }
      if (!IsPrimaryCapableRole(descriptor->role) || descriptor->role == FilespaceRole::primary_snapshot) {
        return ErrorResult("SB-FILESPACE-LIFECYCLE-PROMOTE-ROLE-INVALID",
                           "storage.filespace.promote_role_invalid",
                           request,
                           FilespaceRoleName(descriptor->role));
      }
      if (request.policy.require_no_active_pins_for_promote && HasActivePins(*descriptor)) {
        return ErrorResult("SB-FILESPACE-LIFECYCLE-PROMOTE-PINNED",
                           "storage.filespace.promote_pinned",
                           request,
                           std::to_string(ActivePinCount(*descriptor)));
      }
      if (request.policy.require_physical_header_for_promote) {
        const auto physical = ReadPhysicalFilespaceHeader(descriptor->path);
        if (!physical.ok()) {
          return ErrorResult("SB-FILESPACE-LIFECYCLE-PROMOTE-PHYSICAL-HEADER-INVALID",
                             "storage.filespace.promote_physical_header_invalid",
                             request,
                             physical.diagnostic.diagnostic_code);
        }
        if (!(physical.header.database_uuid.value == request.database_uuid.value) ||
            !(physical.header.filespace_uuid.value == request.filespace_uuid.value) ||
            (physical.header.role != FilespaceRole::primary_candidate &&
             physical.header.role != FilespaceRole::primary_shadow &&
             physical.header.role != FilespaceRole::active_primary)) {
          return ErrorResult("SB-FILESPACE-LIFECYCLE-PROMOTE-PHYSICAL-HEADER-MISMATCH",
                             "storage.filespace.promote_physical_header_mismatch",
                             request);
        }
      }
      if (!request.policy.allow_primary_replacement) {
        const FilespaceDescriptor* primary = FindPrimary(*registry);
        if (primary != nullptr && primary->filespace_uuid.value != request.filespace_uuid.value) {
          return ErrorResult("SB-FILESPACE-LIFECYCLE-PRIMARY-ALREADY-EXISTS",
                             "storage.filespace.primary_already_exists",
                             request);
        }
      } else {
        for (FilespaceDescriptor& candidate : registry->filespaces) {
          if (candidate.role == FilespaceRole::active_primary &&
              candidate.filespace_uuid.value != request.filespace_uuid.value) {
            candidate.role = FilespaceRole::primary_shadow;
            candidate.startup_authority = false;
            candidate.catalog_persistence_owner = false;
            candidate.filespace_manifest_owner = false;
            candidate.recovery_evidence_owner = false;
            candidate.read_only = true;
            ++candidate.generation;
          }
        }
      }
      if (request.policy.require_physical_header_for_promote) {
        PhysicalFilespaceHeader promoted_header;
        promoted_header.database_uuid = descriptor->database_uuid;
        promoted_header.filespace_uuid = descriptor->filespace_uuid;
        promoted_header.role = FilespaceRole::active_primary;
        promoted_header.state = FilespaceState::online;
        promoted_header.page_size = descriptor->page_size;
        promoted_header.physical_filespace_id = descriptor->physical_filespace_id;
        promoted_header.total_pages = descriptor->total_pages;
        promoted_header.free_pages = descriptor->free_pages;
        promoted_header.preallocated_pages = descriptor->preallocated_pages;
        promoted_header.allocation_root_page = descriptor->allocation_root_page;
        promoted_header.header_generation = descriptor->header_generation + 1;
        promoted_header.writer_identity_uuid = request.writer_identity_uuid.valid()
                                                   ? request.writer_identity_uuid
                                                   : descriptor->writer_identity_uuid;
        promoted_header.creation_operation_uuid = request.reason;
        const auto write_promoted_header = WritePhysicalFilespaceHeader(descriptor->path, promoted_header, true);
        if (!write_promoted_header.ok()) {
          return ErrorResult("SB-FILESPACE-LIFECYCLE-PROMOTE-PHYSICAL-HEADER-WRITE-FAILED",
                             "storage.filespace.promote_physical_header_write_failed",
                             request,
                             write_promoted_header.diagnostic.diagnostic_code);
        }
      }
      after.role = FilespaceRole::active_primary;
      after.state = FilespaceState::attached;
      after.active = true;
      after.read_only = false;
      after.startup_authority = true;
      after.catalog_persistence_owner = true;
      after.filespace_manifest_owner = true;
      after.recovery_evidence_owner = true;
      if (request.policy.require_physical_header_for_promote) {
        ++after.header_generation;
        if (request.writer_identity_uuid.valid()) {
          after.writer_identity_uuid = request.writer_identity_uuid;
        }
      }
      ++after.generation;
      *descriptor = after;
      durable_state_changed = true;
      break;
    case FilespaceOperation::demote_filespace:
      if (descriptor == nullptr || descriptor->state == FilespaceState::dropped) {
        return ErrorResult("SB-FILESPACE-LIFECYCLE-NOT-FOUND",
                           "storage.filespace.not_found",
                           request);
      }
      after.role = FilespaceRole::primary_shadow;
      after.startup_authority = false;
      after.catalog_persistence_owner = false;
      after.filespace_manifest_owner = false;
      after.recovery_evidence_owner = false;
      after.read_only = true;
      ++after.generation;
      *descriptor = after;
      durable_state_changed = true;
      break;
    case FilespaceOperation::set_read_only:
      if (descriptor == nullptr || descriptor->state == FilespaceState::dropped) {
        return ErrorResult("SB-FILESPACE-LIFECYCLE-NOT-FOUND",
                           "storage.filespace.not_found",
                           request);
      }
      after.state = FilespaceState::read_only;
      after.read_only = true;
      ++after.generation;
      *descriptor = after;
      durable_state_changed = true;
      break;
    case FilespaceOperation::set_read_write:
      if (descriptor == nullptr || descriptor->state == FilespaceState::dropped) {
        return ErrorResult("SB-FILESPACE-LIFECYCLE-NOT-FOUND",
                           "storage.filespace.not_found",
                           request);
      }
      if (descriptor->state == FilespaceState::archived || IsArchiveRole(descriptor->role)) {
        return ErrorResult("SB-FILESPACE-LIFECYCLE-ARCHIVE-READ-WRITE-FORBIDDEN",
                           "storage.filespace.archive_read_write_forbidden",
                           request);
      }
      after.state = FilespaceState::attached;
      after.read_only = false;
      ++after.generation;
      *descriptor = after;
      durable_state_changed = true;
      break;
    case FilespaceOperation::assign_archive_owner:
      if (descriptor == nullptr || descriptor->state == FilespaceState::dropped) {
        return ErrorResult("SB-FILESPACE-LIFECYCLE-NOT-FOUND",
                           "storage.filespace.not_found",
                           request);
      }
      if (!request.policy.allow_archive_owner_assignment) {
        return ErrorResult("SB-FILESPACE-LIFECYCLE-ARCHIVE-OWNER-FORBIDDEN",
                           "storage.filespace.archive_owner_forbidden",
                           request);
      }
      if (descriptor->role == FilespaceRole::active_primary) {
        return ErrorResult("SB-FILESPACE-LIFECYCLE-ACTIVE-PRIMARY-ARCHIVE-FORBIDDEN",
                           "storage.filespace.active_primary_archive_forbidden",
                           request);
      }
      after.role = FilespaceRole::archive_history;
      after.state = FilespaceState::archived;
      after.read_only = true;
      after.archive_owner = true;
      after.active = true;
      ++after.generation;
      *descriptor = after;
      durable_state_changed = true;
      break;
    case FilespaceOperation::assign_history_owner:
      if (descriptor == nullptr || descriptor->state == FilespaceState::dropped) {
        return ErrorResult("SB-FILESPACE-LIFECYCLE-NOT-FOUND",
                           "storage.filespace.not_found",
                           request);
      }
      if (!request.policy.allow_history_owner_assignment) {
        return ErrorResult("SB-FILESPACE-LIFECYCLE-HISTORY-OWNER-FORBIDDEN",
                           "storage.filespace.history_owner_forbidden",
                           request);
      }
      after.role = FilespaceRole::secondary_history;
      after.history_owner = true;
      after.active = true;
      ++after.generation;
      *descriptor = after;
      durable_state_changed = true;
      break;
    case FilespaceOperation::pin_filespace:
      if (descriptor == nullptr || descriptor->state == FilespaceState::dropped) {
        return ErrorResult("SB-FILESPACE-LIFECYCLE-NOT-FOUND",
                           "storage.filespace.not_found",
                           request);
      }
      if (request.pin_count == 0 || request.pin_owner.empty()) {
        return ErrorResult("SB-FILESPACE-LIFECYCLE-PIN-INVALID",
                           "storage.filespace.pin_invalid",
                           request);
      }
      after.pins = descriptor->pins;
      after.pins.push_back({request.pin_kind, request.pin_owner, request.pin_count});
      *descriptor = after;
      durable_state_changed = true;
      break;
    case FilespaceOperation::unpin_filespace:
      if (descriptor == nullptr || descriptor->state == FilespaceState::dropped) {
        return ErrorResult("SB-FILESPACE-LIFECYCLE-NOT-FOUND",
                           "storage.filespace.not_found",
                           request);
      }
      after.pins = descriptor->pins;
      after.pins.erase(std::remove_if(after.pins.begin(),
                                      after.pins.end(),
                                      [&](const FilespacePin& pin) {
                                        return pin.kind == request.pin_kind && pin.owner == request.pin_owner;
                                      }),
                       after.pins.end());
      *descriptor = after;
      durable_state_changed = true;
      break;
    case FilespaceOperation::verify_filespace:
    case FilespaceOperation::compact_filespace:
    case FilespaceOperation::truncate_filespace:
      if (descriptor == nullptr || descriptor->state == FilespaceState::dropped) {
        return ErrorResult("SB-FILESPACE-LIFECYCLE-NOT-FOUND",
                           "storage.filespace.not_found",
                           request);
      }
      break;
    case FilespaceOperation::drop_filespace:
      if (descriptor == nullptr || descriptor->state == FilespaceState::dropped) {
        return ErrorResult("SB-FILESPACE-LIFECYCLE-NOT-FOUND",
                           "storage.filespace.not_found",
                           request);
      }
      if (request.policy.require_no_active_pins_for_drop && HasActivePins(*descriptor)) {
        return ErrorResult("SB-FILESPACE-LIFECYCLE-DROP-PINNED",
                           "storage.filespace.drop_pinned",
                           request,
                           std::to_string(ActivePinCount(*descriptor)));
      }
      after.state = FilespaceState::dropped;
      after.active = false;
      after.read_only = true;
      ++after.generation;
      *descriptor = after;
      durable_state_changed = true;
      break;
  }

  const FilespaceEvidenceRecord evidence = Evidence(registry,
                                                    request,
                                                    before,
                                                    after,
                                                    "SB-FILESPACE-LIFECYCLE-OK",
                                                    durable_state_changed);
  EmitLifecycleMetric(FilespaceOperationName(request.operation), "ok", "ok");
  EmitPinMetric(after);
  EmitFilespaceAuthorityMetrics(after);
  return SuccessResult(after, evidence, durable_state_changed, true);
}

FilespaceSerializeResult SerializeFilespaceRegistry(const FilespaceRegistry& registry) {
  FilespaceSerializeResult result;
  result.status = FilespaceOkStatus();
  result.payload += Row({"scratchbird.filespace.registry.v2", std::to_string(registry.next_evidence_sequence)});
  for (const FilespaceDescriptor& descriptor : registry.filespaces) {
    if (!CapacityWindowValid(descriptor.total_pages,
                             descriptor.free_pages,
                             descriptor.preallocated_pages,
                             descriptor.allocation_root_page)) {
      result.status = FilespaceErrorStatus();
      result.diagnostic = MakeFilespaceDiagnostic(result.status,
                                                  "SB-FILESPACE-LIFECYCLE-SERIALIZE-CAPACITY-WINDOW-INVALID",
                                                  "storage.filespace.serialize_capacity_window_invalid");
      return result;
    }
    if (descriptor.header_generation == 0) {
      result.status = FilespaceErrorStatus();
      result.diagnostic = MakeFilespaceDiagnostic(result.status,
                                                  "SB-FILESPACE-LIFECYCLE-SERIALIZE-GENERATION-INVALID",
                                                  "storage.filespace.serialize_generation_invalid");
      return result;
    }
    if (!IsTypedEngineIdentity(descriptor.writer_identity_uuid, UuidKind::object)) {
      result.status = FilespaceErrorStatus();
      result.diagnostic = MakeFilespaceDiagnostic(result.status,
                                                  "SB-FILESPACE-LIFECYCLE-SERIALIZE-WRITER-UUID-INVALID",
                                                  "storage.filespace.serialize_writer_uuid_invalid");
      return result;
    }
    result.payload += Row({"F",
                           std::to_string(static_cast<u16>(descriptor.role)),
                           std::to_string(static_cast<u16>(descriptor.state)),
                           std::to_string(descriptor.page_size),
                           std::to_string(descriptor.generation),
                           descriptor.path,
                           descriptor.read_only ? "1" : "0",
                           descriptor.archive_owner ? "1" : "0",
                           descriptor.history_owner ? "1" : "0",
                           descriptor.startup_authority ? "1" : "0",
                           descriptor.catalog_persistence_owner ? "1" : "0",
                           descriptor.filespace_manifest_owner ? "1" : "0",
                           descriptor.recovery_evidence_owner ? "1" : "0",
                           descriptor.first_filespace ? "1" : "0",
                           descriptor.active ? "1" : "0",
                           scratchbird::core::uuid::UuidToString(descriptor.database_uuid.value),
                           scratchbird::core::uuid::UuidToString(descriptor.filespace_uuid.value),
                           std::to_string(descriptor.physical_filespace_id),
                           std::to_string(descriptor.total_pages),
                           std::to_string(descriptor.free_pages),
                           std::to_string(descriptor.preallocated_pages),
                           std::to_string(descriptor.allocation_root_page),
                           std::to_string(descriptor.header_generation),
                           scratchbird::core::uuid::UuidToString(descriptor.writer_identity_uuid.value)});
    for (const FilespacePin& pin : descriptor.pins) {
      result.payload += Row({"P",
                             scratchbird::core::uuid::UuidToString(descriptor.filespace_uuid.value),
                             std::to_string(static_cast<u16>(pin.kind)),
                             pin.owner,
                             std::to_string(pin.count)});
    }
  }
  for (const FilespaceEvidenceRecord& evidence : registry.evidence) {
    result.payload += Row({"E",
                           std::to_string(evidence.sequence),
                           std::to_string(static_cast<u16>(evidence.operation)),
                           scratchbird::core::uuid::UuidToString(evidence.filespace_uuid.value),
                           std::to_string(static_cast<u16>(evidence.previous_state)),
                           std::to_string(static_cast<u16>(evidence.new_state)),
                           std::to_string(static_cast<u16>(evidence.previous_role)),
                           std::to_string(static_cast<u16>(evidence.new_role)),
                           evidence.reason,
                           evidence.diagnostic_code,
                           evidence.durable_state_changed ? "1" : "0"});
  }
  return result;
}

FilespaceParseResult ParseFilespaceRegistry(const std::string& payload) {
  FilespaceParseResult result;
  result.status = FilespaceOkStatus();

  std::stringstream stream(payload);
  std::string row;
  bool header_seen = false;
  bool registry_v2 = false;
  std::map<std::string, std::size_t> uuid_to_index;
  while (std::getline(stream, row)) {
    if (row.empty()) {
      continue;
    }
    const auto fields = SplitRow(row);
    if (!header_seen) {
      if (fields.size() != 2 ||
          (fields[0] != "scratchbird.filespace.registry.v1" &&
           fields[0] != "scratchbird.filespace.registry.v2")) {
        result.status = FilespaceErrorStatus();
        result.diagnostic = MakeFilespaceDiagnostic(result.status,
                                                    "SB-FILESPACE-LIFECYCLE-PARSE-HEADER-INVALID",
                                                    "storage.filespace.parse_header_invalid");
        return result;
      }
      registry_v2 = fields[0] == "scratchbird.filespace.registry.v2";
      result.registry.next_evidence_sequence = ParseU64(fields[1]);
      header_seen = true;
      continue;
    }
    if (fields.empty()) {
      continue;
    }
    if (fields[0] == "F" &&
        ((!registry_v2 && fields.size() == 17) ||
         (registry_v2 && fields.size() == 24))) {
      FilespaceDescriptor descriptor;
      descriptor.role = ParseRole(ParseU16(fields[1]));
      descriptor.state = ParseState(ParseU16(fields[2]));
      descriptor.page_size = ParseU32(fields[3]);
      descriptor.generation = ParseU64(fields[4]);
      descriptor.path = fields[5];
      descriptor.read_only = fields[6] == "1";
      descriptor.archive_owner = fields[7] == "1";
      descriptor.history_owner = fields[8] == "1";
      descriptor.startup_authority = fields[9] == "1";
      descriptor.catalog_persistence_owner = fields[10] == "1";
      descriptor.filespace_manifest_owner = fields[11] == "1";
      descriptor.recovery_evidence_owner = fields[12] == "1";
      descriptor.first_filespace = fields[13] == "1";
      descriptor.active = fields[14] == "1";
      const auto database_uuid = scratchbird::core::uuid::ParseTypedUuid(UuidKind::database, fields[15]);
      const auto filespace_uuid = scratchbird::core::uuid::ParseTypedUuid(UuidKind::filespace, fields[16]);
      if (!database_uuid.ok() || !filespace_uuid.ok()) {
        result.status = FilespaceErrorStatus();
        result.diagnostic = MakeFilespaceDiagnostic(result.status,
                                                    "SB-FILESPACE-LIFECYCLE-PARSE-UUID-INVALID",
                                                    "storage.filespace.parse_uuid_invalid");
        return result;
      }
      descriptor.database_uuid = database_uuid.value;
      descriptor.filespace_uuid = filespace_uuid.value;
      if (registry_v2) {
        descriptor.physical_filespace_id = ParseU16(fields[17]);
        descriptor.total_pages = ParseU64(fields[18]);
        descriptor.free_pages = ParseU64(fields[19]);
        descriptor.preallocated_pages = ParseU64(fields[20]);
        descriptor.allocation_root_page = ParseU64(fields[21]);
        descriptor.header_generation = ParseU64(fields[22]);
        const auto writer_uuid = scratchbird::core::uuid::ParseTypedUuid(UuidKind::object, fields[23]);
        if (!writer_uuid.ok()) {
          result.status = FilespaceErrorStatus();
          result.diagnostic = MakeFilespaceDiagnostic(result.status,
                                                      "SB-FILESPACE-LIFECYCLE-PARSE-WRITER-UUID-INVALID",
                                                      "storage.filespace.parse_writer_uuid_invalid");
          return result;
        }
        descriptor.writer_identity_uuid = writer_uuid.value;
        if (!CapacityWindowValid(descriptor.total_pages,
                                 descriptor.free_pages,
                                 descriptor.preallocated_pages,
                                 descriptor.allocation_root_page)) {
          result.status = FilespaceErrorStatus();
          result.diagnostic = MakeFilespaceDiagnostic(result.status,
                                                      "SB-FILESPACE-LIFECYCLE-PARSE-CAPACITY-WINDOW-INVALID",
                                                      "storage.filespace.parse_capacity_window_invalid");
          return result;
        }
        if (descriptor.header_generation == 0) {
          result.status = FilespaceErrorStatus();
          result.diagnostic = MakeFilespaceDiagnostic(result.status,
                                                      "SB-FILESPACE-LIFECYCLE-PARSE-GENERATION-INVALID",
                                                      "storage.filespace.parse_generation_invalid");
          return result;
        }
      }
      uuid_to_index[fields[16]] = result.registry.filespaces.size();
      result.registry.filespaces.push_back(std::move(descriptor));
    } else if (fields[0] == "P" && fields.size() == 5) {
      const auto found = uuid_to_index.find(fields[1]);
      if (found == uuid_to_index.end()) {
        result.status = FilespaceErrorStatus();
        result.diagnostic = MakeFilespaceDiagnostic(result.status,
                                                    "SB-FILESPACE-LIFECYCLE-PARSE-PIN-ORPHANED",
                                                    "storage.filespace.parse_pin_orphaned");
        return result;
      }
      result.registry.filespaces[found->second].pins.push_back({ParsePinKind(ParseU16(fields[2])), fields[3], ParseU64(fields[4])});
    } else if (fields[0] == "E" && fields.size() == 11) {
      FilespaceEvidenceRecord evidence;
      evidence.sequence = ParseU64(fields[1]);
      evidence.operation = ParseOperation(ParseU16(fields[2]));
      const auto filespace_uuid = scratchbird::core::uuid::ParseTypedUuid(UuidKind::filespace, fields[3]);
      if (!filespace_uuid.ok()) {
        result.status = FilespaceErrorStatus();
        result.diagnostic = MakeFilespaceDiagnostic(result.status,
                                                    "SB-FILESPACE-LIFECYCLE-PARSE-EVIDENCE-UUID-INVALID",
                                                    "storage.filespace.parse_evidence_uuid_invalid");
        return result;
      }
      evidence.filespace_uuid = filespace_uuid.value;
      evidence.previous_state = ParseState(ParseU16(fields[4]));
      evidence.new_state = ParseState(ParseU16(fields[5]));
      evidence.previous_role = ParseRole(ParseU16(fields[6]));
      evidence.new_role = ParseRole(ParseU16(fields[7]));
      evidence.reason = fields[8];
      evidence.diagnostic_code = fields[9];
      evidence.durable_state_changed = fields[10] == "1";
      result.registry.evidence.push_back(std::move(evidence));
    } else {
      result.status = FilespaceErrorStatus();
      result.diagnostic = MakeFilespaceDiagnostic(result.status,
                                                  "SB-FILESPACE-LIFECYCLE-PARSE-ROW-INVALID",
                                                  "storage.filespace.parse_row_invalid");
      return result;
    }
  }

  if (!header_seen) {
    result.status = FilespaceErrorStatus();
    result.diagnostic = MakeFilespaceDiagnostic(result.status,
                                                "SB-FILESPACE-LIFECYCLE-PARSE-HEADER-MISSING",
                                                "storage.filespace.parse_header_missing");
  }
  return result;
}

FilespaceRegistryManifestResult PersistFilespaceRegistryManifest(
    const FilespaceRegistry& registry,
    const FilespaceRegistryManifestWriteRequest& request) {
  FilespaceRegistryManifestResult result;
  result.status = FilespaceOkStatus();
  result.generation = request.generation;

  if (request.path.empty()) {
    return ManifestWriteError(std::move(result),
                              "SB-FILESPACE-REGISTRY-MANIFEST-PATH-MISSING",
                              "storage.filespace.registry_manifest.path_missing");
  }
  if (request.generation == 0) {
    return ManifestWriteError(std::move(result),
                              "SB-FILESPACE-REGISTRY-MANIFEST-GENERATION-INVALID",
                              "storage.filespace.registry_manifest.generation_invalid");
  }
  if (!IsTypedEngineIdentity(request.writer_identity_uuid, UuidKind::object)) {
    return ManifestWriteError(std::move(result),
                              "SB-FILESPACE-REGISTRY-MANIFEST-WRITER-UUID-INVALID",
                              "storage.filespace.registry_manifest.writer_uuid_invalid");
  }

  const auto serialized = SerializeFilespaceRegistry(registry);
  if (!serialized.ok()) {
    result.status = serialized.status;
    result.diagnostic = serialized.diagnostic;
    return result;
  }
  result.checksum = FilespaceManifestChecksum(serialized.payload);

  std::error_code ec;
  const std::filesystem::path parent = request.path.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent, ec);
    if (ec) {
      return ManifestWriteError(std::move(result),
                                "SB-FILESPACE-REGISTRY-MANIFEST-PARENT-CREATE-FAILED",
                                "storage.filespace.registry_manifest.parent_create_failed",
                                ec.message());
    }
  }

  const auto temp_path = FilespaceManifestTempPath(request.path);
  if (std::filesystem::exists(temp_path, ec)) {
    if (!request.remove_stale_temp) {
      return ManifestWriteError(std::move(result),
                                "SB-FILESPACE-REGISTRY-MANIFEST-STALE-TEMP-REFUSED",
                                "storage.filespace.registry_manifest.stale_temp_refused",
                                temp_path.string());
    }
    std::filesystem::remove(temp_path, ec);
    if (ec) {
      return ManifestWriteError(std::move(result),
                                "SB-FILESPACE-REGISTRY-MANIFEST-STALE-TEMP-REMOVE-FAILED",
                                "storage.filespace.registry_manifest.stale_temp_remove_failed",
                                ec.message());
    }
    result.stale_temp_removed = true;
    const auto parent_sync = scratchbird::storage::disk::SyncParentDirectoryPath(temp_path.string());
    if (!parent_sync.ok()) {
      result.status = parent_sync.status;
      result.diagnostic = parent_sync.diagnostic;
      return result;
    }
  }

  {
    std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
    if (!out) {
      return ManifestWriteError(std::move(result),
                                "SB-FILESPACE-REGISTRY-MANIFEST-TEMP-OPEN-FAILED",
                                "storage.filespace.registry_manifest.temp_open_failed",
                                temp_path.string());
    }
    out << kFilespaceManifestHeader << '|'
        << request.generation << '|'
        << scratchbird::core::uuid::UuidToString(request.writer_identity_uuid.value) << '|'
        << kFilespaceManifestChecksumAlgorithm << '|'
        << result.checksum << '\n'
        << serialized.payload;
    out.close();
    if (!out) {
      return ManifestWriteError(std::move(result),
                                "SB-FILESPACE-REGISTRY-MANIFEST-TEMP-WRITE-FAILED",
                                "storage.filespace.registry_manifest.temp_write_failed",
                                temp_path.string());
    }
    result.payload_written = true;
  }

  const auto file_sync = scratchbird::storage::disk::SyncFilesystemPath(temp_path.string(), true);
  if (!file_sync.ok()) {
    result.status = file_sync.status;
    result.diagnostic = file_sync.diagnostic;
    return result;
  }
  result.file_synced = true;

  std::string replace_error;
  if (!ReplaceFileAtomically(temp_path, request.path, &replace_error)) {
    return ManifestWriteError(std::move(result),
                              "SB-FILESPACE-REGISTRY-MANIFEST-RENAME-FAILED",
                              "storage.filespace.registry_manifest.rename_failed",
                              replace_error);
  }
  result.renamed = true;

  const auto parent_sync = scratchbird::storage::disk::SyncParentDirectoryPath(request.path.string());
  if (!parent_sync.ok()) {
    result.status = parent_sync.status;
    result.diagnostic = parent_sync.diagnostic;
    return result;
  }
  result.parent_synced = true;
  return result;
}

FilespaceRegistryManifestLoadResult LoadFilespaceRegistryManifest(
    const FilespaceRegistryManifestLoadRequest& request) {
  FilespaceRegistryManifestLoadResult result;
  result.status = FilespaceOkStatus();

  if (request.path.empty()) {
    return ManifestLoadError(std::move(result),
                             "SB-FILESPACE-REGISTRY-MANIFEST-LOAD-PATH-MISSING",
                             "storage.filespace.registry_manifest.load_path_missing");
  }

  std::error_code ec;
  const auto temp_path = FilespaceManifestTempPath(request.path);
  if (std::filesystem::exists(temp_path, ec)) {
    if (!request.remove_stale_temp) {
      return ManifestLoadError(std::move(result),
                               "SB-FILESPACE-REGISTRY-MANIFEST-LOAD-STALE-TEMP-REFUSED",
                               "storage.filespace.registry_manifest.load_stale_temp_refused",
                               temp_path.string());
    }
    std::filesystem::remove(temp_path, ec);
    if (ec) {
      return ManifestLoadError(std::move(result),
                               "SB-FILESPACE-REGISTRY-MANIFEST-LOAD-STALE-TEMP-REMOVE-FAILED",
                               "storage.filespace.registry_manifest.load_stale_temp_remove_failed",
                               ec.message());
    }
    result.stale_temp_removed = true;
    const auto parent_sync = scratchbird::storage::disk::SyncParentDirectoryPath(temp_path.string());
    if (!parent_sync.ok()) {
      result.status = parent_sync.status;
      result.diagnostic = parent_sync.diagnostic;
      return result;
    }
  }

  std::ifstream in(request.path, std::ios::binary);
  if (!in) {
    return ManifestLoadError(std::move(result),
                             "SB-FILESPACE-REGISTRY-MANIFEST-LOAD-OPEN-FAILED",
                             "storage.filespace.registry_manifest.load_open_failed",
                             request.path.string());
  }

  std::string header;
  std::getline(in, header);
  if (!in && header.empty()) {
    return ManifestLoadError(std::move(result),
                             "SB-FILESPACE-REGISTRY-MANIFEST-LOAD-HEADER-MISSING",
                             "storage.filespace.registry_manifest.load_header_missing");
  }
  std::ostringstream payload_stream;
  payload_stream << in.rdbuf();
  const std::string payload = payload_stream.str();

  const auto fields = SplitRow(header);
  if (fields.size() != 5 || fields[0] != kFilespaceManifestHeader) {
    return ManifestLoadError(std::move(result),
                             "SB-FILESPACE-REGISTRY-MANIFEST-LOAD-HEADER-INVALID",
                             "storage.filespace.registry_manifest.load_header_invalid");
  }
  if (!ParseStrictU64(fields[1], &result.generation) || result.generation == 0) {
    return ManifestLoadError(std::move(result),
                             "SB-FILESPACE-REGISTRY-MANIFEST-LOAD-GENERATION-INVALID",
                             "storage.filespace.registry_manifest.load_generation_invalid",
                             fields[1]);
  }
  const auto writer_uuid = scratchbird::core::uuid::ParseTypedUuid(UuidKind::object, fields[2]);
  if (!writer_uuid.ok() || !IsTypedEngineIdentity(writer_uuid.value, UuidKind::object)) {
    return ManifestLoadError(std::move(result),
                             "SB-FILESPACE-REGISTRY-MANIFEST-LOAD-WRITER-UUID-INVALID",
                             "storage.filespace.registry_manifest.load_writer_uuid_invalid",
                             fields[2]);
  }
  result.writer_identity_uuid = writer_uuid.value;
  if (fields[3] != kFilespaceManifestChecksumAlgorithm) {
    return ManifestLoadError(std::move(result),
                             "SB-FILESPACE-REGISTRY-MANIFEST-LOAD-CHECKSUM-ALGORITHM-INVALID",
                             "storage.filespace.registry_manifest.load_checksum_algorithm_invalid",
                             fields[3]);
  }
  result.checksum = fields[4];
  const std::string observed_checksum = FilespaceManifestChecksum(payload);
  if (observed_checksum != result.checksum) {
    return ManifestLoadError(std::move(result),
                             "SB-FILESPACE-REGISTRY-MANIFEST-LOAD-CHECKSUM-MISMATCH",
                             "storage.filespace.registry_manifest.load_checksum_mismatch",
                             observed_checksum);
  }
  result.checksum_verified = true;

  if (request.expected_writer_identity_uuid.valid()) {
    if (!IsTypedEngineIdentity(request.expected_writer_identity_uuid, UuidKind::object) ||
        request.expected_writer_identity_uuid.value != result.writer_identity_uuid.value) {
      return ManifestLoadError(std::move(result),
                               "SB-FILESPACE-REGISTRY-MANIFEST-LOAD-WRITER-UUID-MISMATCH",
                               "storage.filespace.registry_manifest.load_writer_uuid_mismatch",
                               fields[2]);
    }
  }
  result.writer_identity_verified = true;

  const auto parsed = ParseFilespaceRegistry(payload);
  if (!parsed.ok()) {
    result.status = parsed.status;
    result.diagnostic = parsed.diagnostic;
    return result;
  }
  result.registry = parsed.registry;
  return result;
}

DiagnosticRecord MakeFilespaceDiagnostic(Status status,
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
                        "storage.filespace.lifecycle");
}

}  // namespace scratchbird::storage::filespace
