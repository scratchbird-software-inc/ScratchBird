// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SB-INSERT-FILESPACE-GROWTH-ANCHOR
// SEARCH_KEY: SB_FILESPACE_CAPACITY_GROWTH_LEDGER
#include "filespace_growth.hpp"

#include "filespace_header.hpp"
#include "metric_contracts.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace scratchbird::storage::filespace {
namespace {

using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::uuid::IsEngineIdentityUuid;
using scratchbird::core::uuid::UuidToString;

Status GrowthOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::storage_disk};
}

Status GrowthErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::storage_disk};
}

bool SameUuid(const TypedUuid& left, const TypedUuid& right) {
  return left.kind == right.kind && left.value == right.value;
}

bool IsTypedEngineIdentity(const TypedUuid& uuid, UuidKind kind) {
  return uuid.kind == kind && uuid.valid() && IsEngineIdentityUuid(uuid.value);
}

bool AddWouldOverflow(u64 left, u64 right) {
  return right > std::numeric_limits<u64>::max() - left;
}

bool MultiplyWouldOverflow(u64 left, u64 right) {
  return left != 0 && right > std::numeric_limits<u64>::max() / left;
}

bool IsKnownInsertPageFamily(const std::string& page_family) {
  return page_family == "row_data" ||
         page_family == "catalog" ||
         page_family == "index" ||
         page_family == "overflow" ||
         page_family == "toast" ||
         page_family == "blob" ||
         page_family == "metrics" ||
         page_family == "archive" ||
         page_family == "columnar" ||
         page_family == "vector" ||
         page_family == "graph" ||
         page_family == "allocation" ||
         page_family == "startup" ||
         page_family == "transaction_inventory";
}

bool IsFilespaceWritableForInsertGrowth(const FilespaceDescriptor& descriptor) {
  return descriptor.active &&
         !descriptor.read_only &&
         (descriptor.state == FilespaceState::online || descriptor.state == FilespaceState::attached) &&
         descriptor.state != FilespaceState::quarantine &&
         descriptor.state != FilespaceState::forbidden &&
         descriptor.state != FilespaceState::drop_pending &&
         descriptor.state != FilespaceState::deleted;
}

bool IsFilespaceOnlineForPreallocation(const FilespaceDescriptor& descriptor) {
  return descriptor.active &&
         !descriptor.read_only &&
         (descriptor.state == FilespaceState::online || descriptor.state == FilespaceState::attached);
}

const FilespaceDescriptor* FindGrowthTarget(const FilespaceRegistry& registry,
                                            const InsertFilespaceGrowthRequest& request) {
  if (request.filespace_uuid.valid()) {
    const auto found = std::find_if(registry.filespaces.begin(),
                                    registry.filespaces.end(),
                                    [&](const FilespaceDescriptor& descriptor) {
                                      return SameUuid(descriptor.filespace_uuid, request.filespace_uuid);
                                    });
    return found == registry.filespaces.end() ? nullptr : &(*found);
  }

  const auto found = std::find_if(registry.filespaces.begin(),
                                  registry.filespaces.end(),
                                  [&](const FilespaceDescriptor& descriptor) {
                                    return SameUuid(descriptor.database_uuid, request.database_uuid) &&
                                           descriptor.role == request.filespace_role &&
                                           IsFilespaceWritableForInsertGrowth(descriptor);
                                  });
  return found == registry.filespaces.end() ? nullptr : &(*found);
}

InsertFilespaceGrowthEntry* FindMutableGrowthOperation(FilespaceGrowthLedger* ledger,
                                                       const TypedUuid& growth_operation_id) {
  if (ledger == nullptr) {
    return nullptr;
  }
  const auto found = std::find_if(ledger->operations.begin(),
                                  ledger->operations.end(),
                                  [&](const InsertFilespaceGrowthEntry& operation) {
                                    return SameUuid(operation.growth_operation_id, growth_operation_id);
                                  });
  return found == ledger->operations.end() ? nullptr : &(*found);
}

InsertFilespaceGrowthWaitPolicy WaitPolicyForUrgency(InsertFilespaceGrowthUrgency urgency) {
  switch (urgency) {
    case InsertFilespaceGrowthUrgency::background:
      return InsertFilespaceGrowthWaitPolicy::background_only;
    case InsertFilespaceGrowthUrgency::normal:
      return InsertFilespaceGrowthWaitPolicy::bounded_wait;
    case InsertFilespaceGrowthUrgency::high:
    case InsertFilespaceGrowthUrgency::critical:
      return InsertFilespaceGrowthWaitPolicy::no_wait;
  }
  return InsertFilespaceGrowthWaitPolicy::refused;
}

TypedUuid GrowthOperationId(FilespaceGrowthLedger* ledger) {
  const auto generated = scratchbird::core::uuid::GenerateEngineIdentityV7(
      UuidKind::object,
      ledger == nullptr ? 1 : ledger->next_evidence_sequence);
  return generated.ok() ? generated.value : TypedUuid{};
}

TypedUuid PreallocationOperationId(FilespaceGrowthLedger* ledger) {
  const auto generated = scratchbird::core::uuid::GenerateEngineIdentityV7(
      UuidKind::object,
      ledger == nullptr ? 1 : ledger->next_evidence_sequence + 1000000);
  return generated.ok() ? generated.value : TypedUuid{};
}

TypedUuid PhysicalGrowthOperationId(FilespaceGrowthLedger* ledger) {
  const auto generated = scratchbird::core::uuid::GenerateEngineIdentityV7(
      UuidKind::object,
      ledger == nullptr ? 1 : ledger->next_evidence_sequence + 2000000);
  return generated.ok() ? generated.value : TypedUuid{};
}

InsertFilespaceGrowthEvidenceRecord BuildEvidence(FilespaceGrowthLedger* ledger,
                                                  const InsertFilespaceGrowthEntry& entry,
                                                  std::string action,
                                                  InsertFilespaceGrowthState previous_state,
                                                  InsertFilespaceGrowthState new_state,
                                                  std::string diagnostic_code,
                                                  std::string reason,
                                                  bool durable_state_changed) {
  InsertFilespaceGrowthEvidenceRecord evidence;
  evidence.sequence = ledger == nullptr ? 0 : ledger->next_evidence_sequence++;
  evidence.action = std::move(action);
  evidence.growth_operation_id = entry.growth_operation_id;
  evidence.database_uuid = entry.database_uuid;
  evidence.filespace_uuid = entry.filespace_uuid;
  evidence.filespace_role = entry.filespace_role;
  evidence.page_family = entry.page_family;
  evidence.requested_page_count = entry.requested_page_count;
  evidence.predicted_insert_pressure_pages = entry.predicted_insert_pressure_pages;
  evidence.expected_usable_pages = entry.expected_usable_pages;
  evidence.urgency_class = entry.urgency_class;
  evidence.wait_policy = entry.wait_policy;
  evidence.previous_state = previous_state;
  evidence.new_state = new_state;
  evidence.diagnostic_code = std::move(diagnostic_code);
  evidence.reason = std::move(reason);
  evidence.durable_state_changed = durable_state_changed;
  return evidence;
}

FilespacePreallocationEvidenceRecord BuildPreallocationEvidence(
    FilespaceGrowthLedger* ledger,
    const FilespacePreallocationEntry& entry,
    std::string action,
    FilespacePreallocationState previous_state,
    FilespacePreallocationState new_state,
    std::string diagnostic_code,
    std::string reason,
    bool durable_state_changed,
    bool evidence_before_success) {
  FilespacePreallocationEvidenceRecord evidence;
  evidence.sequence = ledger == nullptr ? 0 : ledger->next_evidence_sequence++;
  evidence.action = std::move(action);
  evidence.request_uuid = entry.request_uuid;
  evidence.preallocation_operation_id = entry.preallocation_operation_id;
  evidence.database_uuid = entry.database_uuid;
  evidence.filespace_uuid = entry.filespace_uuid;
  evidence.policy_uuid = entry.policy_uuid;
  evidence.file_member_uuid = entry.file_member_uuid;
  evidence.transaction_uuid = entry.transaction_uuid;
  evidence.filespace_role = entry.filespace_role;
  evidence.previous_state = previous_state;
  evidence.new_state = new_state;
  evidence.requested_page_count = entry.requested_page_count;
  evidence.preallocated_page_count = entry.preallocated_page_count;
  evidence.bytes_preallocated = entry.bytes_preallocated;
  evidence.policy_generation = entry.policy_generation;
  evidence.catalog_generation = entry.catalog_generation;
  evidence.diagnostic_code = std::move(diagnostic_code);
  evidence.reason = std::move(reason);
  evidence.durable_state_changed = durable_state_changed;
  evidence.evidence_before_success = evidence_before_success;
  return evidence;
}

FilespacePhysicalGrowthEvidenceRecord BuildPhysicalGrowthEvidence(
    FilespaceGrowthLedger* ledger,
    const FilespacePhysicalGrowthEntry& entry,
    std::string action,
    FilespacePhysicalGrowthState previous_state,
    FilespacePhysicalGrowthState new_state,
    std::string diagnostic_code,
    std::string reason,
    bool durable_state_changed,
    bool evidence_before_success) {
  FilespacePhysicalGrowthEvidenceRecord evidence;
  evidence.sequence = ledger == nullptr ? 0 : ledger->next_evidence_sequence++;
  evidence.action = std::move(action);
  evidence.request_uuid = entry.request_uuid;
  evidence.growth_operation_id = entry.growth_operation_id;
  evidence.database_uuid = entry.database_uuid;
  evidence.filespace_uuid = entry.filespace_uuid;
  evidence.policy_uuid = entry.policy_uuid;
  evidence.file_member_uuid = entry.file_member_uuid;
  evidence.transaction_uuid = entry.transaction_uuid;
  evidence.filespace_role = entry.filespace_role;
  evidence.caller_mode = entry.caller_mode;
  evidence.previous_state = previous_state;
  evidence.new_state = new_state;
  evidence.requested_growth_pages = entry.requested_growth_pages;
  evidence.grown_page_count = entry.grown_page_count;
  evidence.bytes_grown = entry.bytes_grown;
  evidence.physical_file_size_before_bytes = entry.physical_file_size_before_bytes;
  evidence.physical_file_size_after_bytes = entry.physical_file_size_after_bytes;
  evidence.physical_file_expected_size_after_bytes =
      entry.physical_file_expected_size_after_bytes;
  evidence.extent_preallocation_offset_bytes =
      entry.extent_preallocation_offset_bytes;
  evidence.extent_preallocation_bytes = entry.extent_preallocation_bytes;
  evidence.extent_preallocation_strategy =
      entry.extent_preallocation_strategy;
  evidence.extent_preallocation_fallback_reason =
      entry.extent_preallocation_fallback_reason;
  evidence.policy_generation = entry.policy_generation;
  evidence.catalog_generation = entry.catalog_generation;
  evidence.diagnostic_code = std::move(diagnostic_code);
  evidence.reason = std::move(reason);
  evidence.extent_preallocation_attempted =
      entry.extent_preallocation_attempted;
  evidence.extent_preallocation_succeeded =
      entry.extent_preallocation_succeeded;
  evidence.extent_preallocation_fallback_used =
      entry.extent_preallocation_fallback_used;
  evidence.physical_extension_required = entry.physical_extension_required;
  evidence.physical_extension_completed = entry.physical_extension_completed;
  evidence.physical_extension_synced = entry.physical_extension_synced;
  evidence.physical_header_updated = entry.physical_header_updated;
  evidence.metadata_commit_after_physical_extension =
      entry.metadata_commit_after_physical_extension;
  evidence.durable_state_changed = durable_state_changed;
  evidence.evidence_before_success = evidence_before_success;
  return evidence;
}

const FilespaceDescriptor* FindPreallocationTarget(const FilespaceRegistry& registry,
                                                   const TypedUuid& filespace_uuid) {
  const auto found = std::find_if(registry.filespaces.begin(),
                                  registry.filespaces.end(),
                                  [&](const FilespaceDescriptor& descriptor) {
                                    return SameUuid(descriptor.filespace_uuid, filespace_uuid);
                                  });
  return found == registry.filespaces.end() ? nullptr : &(*found);
}

FilespacePreallocationEntry* FindMutablePreallocationByRequest(FilespaceGrowthLedger* ledger,
                                                               const TypedUuid& request_uuid) {
  if (ledger == nullptr || !request_uuid.valid()) {
    return nullptr;
  }
  const auto found = std::find_if(ledger->preallocation_operations.begin(),
                                  ledger->preallocation_operations.end(),
                                  [&](const FilespacePreallocationEntry& operation) {
                                    return SameUuid(operation.request_uuid, request_uuid);
                                  });
  return found == ledger->preallocation_operations.end() ? nullptr : &(*found);
}

FilespacePhysicalGrowthEntry* FindMutablePhysicalGrowthByRequest(FilespaceGrowthLedger* ledger,
                                                                 const TypedUuid& request_uuid) {
  if (ledger == nullptr || !request_uuid.valid()) {
    return nullptr;
  }
  const auto found = std::find_if(ledger->physical_growth_operations.begin(),
                                  ledger->physical_growth_operations.end(),
                                  [&](const FilespacePhysicalGrowthEntry& operation) {
                                    return SameUuid(operation.request_uuid, request_uuid);
                                  });
  return found == ledger->physical_growth_operations.end() ? nullptr : &(*found);
}

FilespaceMemberCapacityWindow* FindMutableMemberCapacityWindow(
    FilespaceGrowthLedger* ledger,
    const TypedUuid& filespace_uuid,
    const TypedUuid& file_member_uuid) {
  if (ledger == nullptr) {
    return nullptr;
  }
  const auto found = std::find_if(ledger->member_capacity_windows.begin(),
                                  ledger->member_capacity_windows.end(),
                                  [&](const FilespaceMemberCapacityWindow& window) {
                                    return SameUuid(window.filespace_uuid, filespace_uuid) &&
                                           SameUuid(window.file_member_uuid, file_member_uuid);
                                  });
  return found == ledger->member_capacity_windows.end() ? nullptr : &(*found);
}

const FilespacePreallocatedExtentMetadata* FindPreallocatedExtentByOperation(
    const FilespaceGrowthLedger& ledger,
    const TypedUuid& preallocation_operation_id) {
  const auto found = std::find_if(ledger.preallocated_extents.begin(),
                                  ledger.preallocated_extents.end(),
                                  [&](const FilespacePreallocatedExtentMetadata& extent) {
                                    return SameUuid(extent.preallocation_operation_id,
                                                    preallocation_operation_id);
                                  });
  return found == ledger.preallocated_extents.end() ? nullptr : &(*found);
}

const FilespaceMemberCapacityWindow* FindMemberCapacityWindow(
    const FilespaceGrowthLedger& ledger,
    const TypedUuid& filespace_uuid,
    const TypedUuid& file_member_uuid) {
  const auto found = std::find_if(ledger.member_capacity_windows.begin(),
                                  ledger.member_capacity_windows.end(),
                                  [&](const FilespaceMemberCapacityWindow& window) {
                                    return SameUuid(window.filespace_uuid, filespace_uuid) &&
                                           SameUuid(window.file_member_uuid, file_member_uuid);
                                  });
  return found == ledger.member_capacity_windows.end() ? nullptr : &(*found);
}

bool HasPhysicalGrowthAuthorization(const FilespacePhysicalGrowthRequest& request) {
  if (!request.authorization.action_approval) {
    return false;
  }

  switch (request.caller_mode) {
    case FilespacePhysicalGrowthCallerMode::filespace_capacity_manager:
      return request.authorization.obs_agent_control_right &&
             request.authorization.filespace_lifecycle_right;
    case FilespacePhysicalGrowthCallerMode::direct_sysarch:
      return request.authorization.storage_filespace_control_right;
    case FilespacePhysicalGrowthCallerMode::unknown:
      return false;
  }
  return false;
}

InsertFilespaceGrowthResult Refuse(FilespaceGrowthLedger* ledger,
                                   const InsertFilespaceGrowthRequest& request,
                                   const FilespaceDescriptor* descriptor,
                                   std::string diagnostic_code,
                                   std::string message_key,
                                   std::string detail) {
  InsertFilespaceGrowthResult result;
  result.status = GrowthErrorStatus();
  result.admitted = false;
  result.wait_policy = InsertFilespaceGrowthWaitPolicy::refused;

  InsertFilespaceGrowthEntry entry;
  entry.growth_operation_id = GrowthOperationId(ledger);
  entry.database_uuid = request.database_uuid;
  entry.filespace_uuid = descriptor == nullptr ? request.filespace_uuid : descriptor->filespace_uuid;
  entry.policy_uuid = request.policy_uuid;
  entry.filespace_role = descriptor == nullptr ? request.filespace_role : descriptor->role;
  entry.page_family = request.page_family;
  entry.requested_page_count = request.requested_page_count;
  entry.predicted_insert_pressure_pages = request.predicted_insert_pressure_pages;
  entry.expected_usable_pages = 0;
  entry.urgency_class = request.urgency_class;
  entry.wait_policy = InsertFilespaceGrowthWaitPolicy::refused;
  entry.state = InsertFilespaceGrowthState::refused;

  result.growth_operation_id = entry.growth_operation_id;
  result.operation = entry;
  result.evidence = BuildEvidence(ledger,
                                  entry,
                                  "refuse_insert_filespace_growth",
                                  InsertFilespaceGrowthState::absent,
                                  InsertFilespaceGrowthState::refused,
                                  diagnostic_code,
                                  detail,
                                  false);
  result.diagnostic = MakeFilespaceGrowthDiagnostic(result.status,
                                                   std::move(diagnostic_code),
                                                   std::move(message_key),
                                                   std::move(detail));
  if (ledger != nullptr) {
    ledger->evidence.push_back(result.evidence);
  }
  if (entry.filespace_uuid.valid()) {
    (void)scratchbird::core::metrics::RecordFilespaceAgentCapacityRequest(UuidToString(entry.filespace_uuid.value),
                                                                          "insert_growth",
                                                                          result.diagnostic.diagnostic_code);
  }
  return result;
}

FilespacePhysicalGrowthResult RefusePhysicalGrowth(const FilespacePhysicalGrowthRequest& request,
                                                   const FilespaceDescriptor* descriptor,
                                                   std::string diagnostic_code,
                                                   std::string message_key,
                                                   std::string detail) {
  FilespacePhysicalGrowthResult result;
  result.status = GrowthErrorStatus();

  FilespacePhysicalGrowthEntry entry;
  entry.request_uuid = request.request_uuid;
  entry.growth_operation_id = PhysicalGrowthOperationId(nullptr);
  entry.database_uuid = request.database_uuid;
  entry.filespace_uuid = descriptor == nullptr ? request.filespace_uuid : descriptor->filespace_uuid;
  entry.policy_uuid = request.policy_uuid;
  entry.storage_profile_uuid = request.storage_profile_uuid;
  entry.file_member_uuid = request.member_capacity.file_member_uuid;
  entry.transaction_uuid = request.transaction_context.transaction_uuid;
  entry.transaction_number = request.transaction_context.transaction_number;
  entry.filespace_role = descriptor == nullptr ? FilespaceRole::unknown : descriptor->role;
  entry.caller_mode = request.caller_mode;
  entry.state = FilespacePhysicalGrowthState::refused;
  entry.requested_growth_pages = request.requested_growth_pages;
  entry.page_size_bytes = request.page_size_bytes;
  if (!AddWouldOverflow(request.member_capacity.current_page_count,
                        request.member_capacity.preallocated_page_count)) {
    entry.member_physical_page_count_before =
        request.member_capacity.current_page_count +
        request.member_capacity.preallocated_page_count;
    entry.member_physical_page_count_after = entry.member_physical_page_count_before;
  }
  entry.member_preallocated_pages_before = request.member_capacity.preallocated_page_count;
  entry.member_preallocated_pages_after = request.member_capacity.preallocated_page_count;
  entry.member_maximum_page_count = request.member_capacity.maximum_page_count;
  entry.policy_generation = request.policy_generation;
  entry.catalog_generation = request.catalog_generation;
  entry.reserve_growth_as_preallocated = request.reserve_growth_as_preallocated;

  result.operation = entry;
  result.evidence = BuildPhysicalGrowthEvidence(nullptr,
                                                entry,
                                                "refuse_filespace_physical_growth",
                                                FilespacePhysicalGrowthState::absent,
                                                FilespacePhysicalGrowthState::refused,
                                                diagnostic_code,
                                                detail,
                                                false,
                                                false);
  result.diagnostic = MakeFilespacePhysicalGrowthDiagnostic(result.status,
                                                           std::move(diagnostic_code),
                                                           std::move(message_key),
                                                           std::move(detail));
  return result;
}

FilespacePreallocationResult RefusePreallocation(FilespaceGrowthLedger* ledger,
                                                 const FilespacePreallocationRequest& request,
                                                 const FilespaceDescriptor* descriptor,
                                                 std::string diagnostic_code,
                                                 std::string message_key,
                                                 std::string detail) {
  FilespacePreallocationResult result;
  result.status = GrowthErrorStatus();

  FilespacePreallocationEntry entry;
  entry.request_uuid = request.request_uuid;
  entry.preallocation_operation_id = PreallocationOperationId(ledger);
  entry.database_uuid = request.database_uuid;
  entry.filespace_uuid = descriptor == nullptr ? request.filespace_uuid : descriptor->filespace_uuid;
  entry.policy_uuid = request.policy_uuid;
  entry.storage_profile_uuid = request.storage_profile_uuid;
  entry.file_member_uuid = request.member_capacity.file_member_uuid;
  entry.transaction_uuid = request.transaction_context.transaction_uuid;
  entry.transaction_number = request.transaction_context.transaction_number;
  entry.filespace_role = descriptor == nullptr ? FilespaceRole::unknown : descriptor->role;
  entry.state = FilespacePreallocationState::refused;
  entry.requested_page_count = request.requested_page_count;
  entry.page_size_bytes = request.page_size_bytes;
  entry.member_page_count_before = request.member_capacity.current_page_count;
  entry.member_page_count_after = request.member_capacity.current_page_count;
  entry.member_preallocated_pages_before = request.member_capacity.preallocated_page_count;
  entry.member_preallocated_pages_after = request.member_capacity.preallocated_page_count;
  entry.member_maximum_page_count = request.member_capacity.maximum_page_count;
  entry.policy_generation = request.policy_generation;
  entry.catalog_generation = request.catalog_generation;

  result.operation = entry;
  result.evidence = BuildPreallocationEvidence(nullptr,
                                               entry,
                                               "refuse_filespace_preallocate",
                                               FilespacePreallocationState::absent,
                                               FilespacePreallocationState::refused,
                                               diagnostic_code,
                                               detail,
                                               false,
                                               false);
  result.diagnostic = MakeFilespacePreallocationDiagnostic(result.status,
                                                          std::move(diagnostic_code),
                                                          std::move(message_key),
                                                          std::move(detail));
  if (entry.filespace_uuid.valid()) {
    (void)scratchbird::core::metrics::RecordFilespaceAgentCapacityRequest(
        UuidToString(entry.filespace_uuid.value),
        "preallocate",
        result.diagnostic.diagnostic_code);
  }
  return result;
}

FilespaceGrowthMutationResult GrowthMutationRefuse(FilespaceGrowthLedger* ledger,
                                                   const CompleteFilespaceGrowthRequest& request,
                                                   std::string diagnostic_code,
                                                   std::string message_key,
                                                   std::string detail) {
  FilespaceGrowthMutationResult result;
  result.status = GrowthErrorStatus();
  result.diagnostic = MakeFilespaceGrowthDiagnostic(result.status,
                                                   std::move(diagnostic_code),
                                                   std::move(message_key),
                                                   std::move(detail));
  if (ledger != nullptr) {
    InsertFilespaceGrowthEntry evidence_entry;
    evidence_entry.growth_operation_id = request.growth_operation_id;
    result.evidence = BuildEvidence(ledger,
                                    evidence_entry,
                                    "complete_insert_filespace_growth_refused",
                                    InsertFilespaceGrowthState::absent,
                                    InsertFilespaceGrowthState::absent,
                                    result.diagnostic.diagnostic_code,
                                    result.diagnostic.message_key,
                                    false);
    ledger->evidence.push_back(result.evidence);
  }
  return result;
}

}  // namespace

const char* InsertFilespaceGrowthUrgencyName(InsertFilespaceGrowthUrgency urgency) {
  switch (urgency) {
    case InsertFilespaceGrowthUrgency::background:
      return "background";
    case InsertFilespaceGrowthUrgency::normal:
      return "normal";
    case InsertFilespaceGrowthUrgency::high:
      return "high";
    case InsertFilespaceGrowthUrgency::critical:
      return "critical";
  }
  return "unknown";
}

const char* InsertFilespaceGrowthWaitPolicyName(InsertFilespaceGrowthWaitPolicy policy) {
  switch (policy) {
    case InsertFilespaceGrowthWaitPolicy::no_wait:
      return "no_wait";
    case InsertFilespaceGrowthWaitPolicy::bounded_wait:
      return "bounded_wait";
    case InsertFilespaceGrowthWaitPolicy::background_only:
      return "background_only";
    case InsertFilespaceGrowthWaitPolicy::refused:
      return "refused";
  }
  return "unknown";
}

const char* InsertFilespaceGrowthStateName(InsertFilespaceGrowthState state) {
  switch (state) {
    case InsertFilespaceGrowthState::absent:
      return "absent";
    case InsertFilespaceGrowthState::admitted_pending_allocation:
      return "admitted_pending_allocation";
    case InsertFilespaceGrowthState::allocation_complete:
      return "allocation_complete";
    case InsertFilespaceGrowthState::refused:
      return "refused";
    case InsertFilespaceGrowthState::quarantine:
      return "quarantine";
  }
  return "unknown";
}

const char* FilespaceGrowthRecoveryActionName(FilespaceGrowthRecoveryAction action) {
  switch (action) {
    case FilespaceGrowthRecoveryAction::no_action:
      return "no_action";
    case FilespaceGrowthRecoveryAction::complete:
      return "complete";
    case FilespaceGrowthRecoveryAction::roll_back:
      return "roll_back";
    case FilespaceGrowthRecoveryAction::quarantine:
      return "quarantine";
    case FilespaceGrowthRecoveryAction::fail_closed:
      return "fail_closed";
  }
  return "unknown";
}

const char* FilespacePreallocationStateName(FilespacePreallocationState state) {
  switch (state) {
    case FilespacePreallocationState::absent:
      return "absent";
    case FilespacePreallocationState::completed:
      return "completed";
    case FilespacePreallocationState::refused:
      return "refused";
    case FilespacePreallocationState::quarantine:
      return "quarantine";
  }
  return "unknown";
}

const char* FilespacePreallocationRecoveryActionName(FilespacePreallocationRecoveryAction action) {
  switch (action) {
    case FilespacePreallocationRecoveryAction::no_action:
      return "no_action";
    case FilespacePreallocationRecoveryAction::retain_reserved_free:
      return "retain_reserved_free";
    case FilespacePreallocationRecoveryAction::quarantine:
      return "quarantine";
    case FilespacePreallocationRecoveryAction::fail_closed:
      return "fail_closed";
  }
  return "unknown";
}

const char* FilespacePhysicalGrowthCallerModeName(FilespacePhysicalGrowthCallerMode mode) {
  switch (mode) {
    case FilespacePhysicalGrowthCallerMode::filespace_capacity_manager:
      return "filespace_capacity_manager";
    case FilespacePhysicalGrowthCallerMode::direct_sysarch:
      return "direct_sysarch";
    case FilespacePhysicalGrowthCallerMode::unknown:
      return "unknown";
  }
  return "unknown";
}

const char* FilespacePhysicalGrowthStateName(FilespacePhysicalGrowthState state) {
  switch (state) {
    case FilespacePhysicalGrowthState::absent:
      return "absent";
    case FilespacePhysicalGrowthState::completed:
      return "completed";
    case FilespacePhysicalGrowthState::refused:
      return "refused";
    case FilespacePhysicalGrowthState::quarantine:
      return "quarantine";
  }
  return "unknown";
}

const char* FilespacePhysicalGrowthRecoveryActionName(FilespacePhysicalGrowthRecoveryAction action) {
  switch (action) {
    case FilespacePhysicalGrowthRecoveryAction::no_action:
      return "no_action";
    case FilespacePhysicalGrowthRecoveryAction::retain_physical_growth:
      return "retain_physical_growth";
    case FilespacePhysicalGrowthRecoveryAction::quarantine:
      return "quarantine";
    case FilespacePhysicalGrowthRecoveryAction::fail_closed:
      return "fail_closed";
  }
  return "unknown";
}

InsertFilespaceGrowthResult RequestInsertFilespaceGrowth(FilespaceGrowthLedger* ledger,
                                                         const FilespaceRegistry& registry,
                                                         const InsertFilespaceGrowthRequest& request) {
  if (ledger == nullptr) {
    return Refuse(nullptr,
                  request,
                  nullptr,
                  "insert_filespace_growth_missing_ledger",
                  "storage.filespace.insert_growth.missing_ledger",
                  "filespace growth ledger is required");
  }
  if (!request.database_uuid.valid()) {
    return Refuse(ledger,
                  request,
                  nullptr,
                  "insert_filespace_growth_invalid_database_uuid",
                  "storage.filespace.insert_growth.invalid_database_uuid",
                  "database_uuid must be a valid engine UUID");
  }
  if (!IsKnownInsertPageFamily(request.page_family)) {
    return Refuse(ledger,
                  request,
                  nullptr,
                  "insert_filespace_growth_unknown_page_family",
                  "storage.filespace.insert_growth.unknown_page_family",
                  request.page_family);
  }
  if (request.requested_page_count == 0) {
    return Refuse(ledger,
                  request,
                  nullptr,
                  "insert_filespace_growth_zero_pages",
                  "storage.filespace.insert_growth.zero_pages",
                  "requested_page_count must be non-zero");
  }

  const FilespaceDescriptor* descriptor = FindGrowthTarget(registry, request);
  if (descriptor == nullptr) {
    return Refuse(ledger,
                  request,
                  nullptr,
                  "insert_filespace_growth_no_filespace",
                  "storage.filespace.insert_growth.no_filespace",
                  "no writable filespace matched the request");
  }
  if (!SameUuid(descriptor->database_uuid, request.database_uuid)) {
    return Refuse(ledger,
                  request,
                  descriptor,
                  "insert_filespace_growth_database_mismatch",
                  "storage.filespace.insert_growth.database_mismatch",
                  "filespace database UUID does not match request database UUID");
  }
  if (!IsFilespaceWritableForInsertGrowth(*descriptor)) {
    return Refuse(ledger,
                  request,
                  descriptor,
                  "insert_filespace_growth_filespace_unavailable",
                  "storage.filespace.insert_growth.filespace_unavailable",
                  FilespaceStateName(descriptor->state));
  }

  InsertFilespaceGrowthEntry entry;
  entry.growth_operation_id = GrowthOperationId(ledger);
  entry.database_uuid = request.database_uuid;
  entry.filespace_uuid = descriptor->filespace_uuid;
  entry.policy_uuid = request.policy_uuid;
  entry.filespace_role = descriptor->role;
  entry.page_family = request.page_family;
  entry.requested_page_count = request.requested_page_count;
  entry.predicted_insert_pressure_pages = request.predicted_insert_pressure_pages;
  entry.expected_usable_pages = request.requested_page_count + request.predicted_insert_pressure_pages;
  entry.urgency_class = request.urgency_class;
  entry.wait_policy = WaitPolicyForUrgency(request.urgency_class);
  entry.state = InsertFilespaceGrowthState::admitted_pending_allocation;

  InsertFilespaceGrowthResult result;
  result.status = GrowthOkStatus();
  result.admitted = true;
  result.growth_operation_id = entry.growth_operation_id;
  result.expected_usable_pages = entry.expected_usable_pages;
  result.wait_policy = entry.wait_policy;
  result.operation = entry;
  result.evidence = BuildEvidence(ledger,
                                  entry,
                                  "admit_insert_filespace_growth",
                                  InsertFilespaceGrowthState::absent,
                                  InsertFilespaceGrowthState::admitted_pending_allocation,
                                  "ok",
                                  "growth admitted",
                                  true);
  result.diagnostic = MakeFilespaceGrowthDiagnostic(result.status,
                                                   "ok",
                                                   "storage.filespace.insert_growth.admitted",
                                                   "growth admitted");

  ledger->operations.push_back(entry);
  ledger->evidence.push_back(result.evidence);
  (void)scratchbird::core::metrics::RecordFilespaceAgentCapacityRequest(UuidToString(entry.filespace_uuid.value),
                                                                        "insert_growth",
                                                                        "admitted");
  return result;
}

FilespacePreallocationResult PreallocateFilespace(FilespaceGrowthLedger* ledger,
                                                  const FilespaceRegistry& registry,
                                                  const FilespacePreallocationRequest& request) {
  if (ledger == nullptr) {
    return RefusePreallocation(nullptr,
                               request,
                               nullptr,
                               "filespace_preallocate_missing_ledger",
                               "storage.filespace.preallocate.missing_ledger",
                               "filespace preallocation ledger is required");
  }
  if (!request.evidence_store_present) {
    return RefusePreallocation(ledger,
                               request,
                               nullptr,
                               "filespace_preallocate_missing_evidence_store",
                               "storage.filespace.preallocate.missing_evidence_store",
                               "durable preallocation evidence store is required");
  }
  if (!request.evidence_before_success) {
    return RefusePreallocation(ledger,
                               request,
                               nullptr,
                               "filespace_preallocate_evidence_before_success_required",
                               "storage.filespace.preallocate.evidence_before_success_required",
                               "preallocation success requires evidence-before-success");
  }
  if (!IsTypedEngineIdentity(request.request_uuid, UuidKind::object)) {
    return RefusePreallocation(ledger,
                               request,
                               nullptr,
                               "filespace_preallocate_invalid_request_uuid",
                               "storage.filespace.preallocate.invalid_request_uuid",
                               "request_uuid must be a valid engine object UUID");
  }

  if (!IsTypedEngineIdentity(request.database_uuid, UuidKind::database)) {
    return RefusePreallocation(ledger,
                               request,
                               nullptr,
                               "filespace_preallocate_invalid_database_uuid",
                               "storage.filespace.preallocate.invalid_database_uuid",
                               "database_uuid must be a valid engine database UUID");
  }
  if (!IsTypedEngineIdentity(request.filespace_uuid, UuidKind::filespace)) {
    return RefusePreallocation(ledger,
                               request,
                               nullptr,
                               "filespace_preallocate_invalid_filespace_uuid",
                               "storage.filespace.preallocate.invalid_filespace_uuid",
                               "filespace_uuid must be a valid engine filespace UUID");
  }
  if (!IsTypedEngineIdentity(request.policy_uuid, UuidKind::object)) {
    return RefusePreallocation(ledger,
                               request,
                               nullptr,
                               "filespace_preallocate_invalid_policy_uuid",
                               "storage.filespace.preallocate.invalid_policy_uuid",
                               "policy_uuid must be a valid engine object UUID");
  }
  if (!IsTypedEngineIdentity(request.storage_profile_uuid, UuidKind::object)) {
    return RefusePreallocation(ledger,
                               request,
                               nullptr,
                               "filespace_preallocate_invalid_storage_profile_uuid",
                               "storage.filespace.preallocate.invalid_storage_profile_uuid",
                               "storage_profile_uuid must be a valid engine object UUID");
  }
  if (request.requested_page_count == 0) {
    return RefusePreallocation(ledger,
                               request,
                               nullptr,
                               "filespace_preallocate_zero_pages",
                               "storage.filespace.preallocate.zero_pages",
                               "requested_page_count must be non-zero");
  }
  if (request.page_size_bytes == 0 || request.page_size_bytes % 4096 != 0) {
    return RefusePreallocation(ledger,
                               request,
                               nullptr,
                               "filespace_preallocate_invalid_page_size",
                               "storage.filespace.preallocate.invalid_page_size",
                               "page_size_bytes must identify an admitted storage page profile");
  }
  if (request.policy_generation == 0 || request.observed_policy_generation == 0 ||
      request.policy_generation != request.observed_policy_generation) {
    return RefusePreallocation(ledger,
                               request,
                               nullptr,
                               "filespace_preallocate_stale_policy_generation",
                               "storage.filespace.preallocate.stale_policy_generation",
                               "policy generation input is missing or stale");
  }
  if (request.catalog_generation == 0 || request.observed_catalog_generation == 0 ||
      request.catalog_generation != request.observed_catalog_generation) {
    return RefusePreallocation(ledger,
                               request,
                               nullptr,
                               "filespace_preallocate_stale_catalog_generation",
                               "storage.filespace.preallocate.stale_catalog_generation",
                               "catalog generation input is missing or stale");
  }
  if (request.require_mga_transaction_context &&
      (!request.transaction_context.present ||
       !IsTypedEngineIdentity(request.transaction_context.transaction_uuid, UuidKind::transaction) ||
       request.transaction_context.transaction_number == 0 ||
       !request.transaction_context.durable_inventory_admitted ||
       !request.transaction_context.write_intent ||
       !request.transaction_context.durability_fence_satisfied)) {
    return RefusePreallocation(ledger,
                               request,
                               nullptr,
                               "filespace_preallocate_invalid_transaction_context",
                               "storage.filespace.preallocate.invalid_transaction_context",
                               "durable MGA transaction context is required for preallocation metadata changes");
  }

  const FilespaceDescriptor* descriptor = FindPreallocationTarget(registry, request.filespace_uuid);
  if (descriptor == nullptr) {
    return RefusePreallocation(ledger,
                               request,
                               nullptr,
                               "filespace_preallocate_no_filespace",
                               "storage.filespace.preallocate.no_filespace",
                               "target filespace was not found");
  }
  if (!SameUuid(descriptor->database_uuid, request.database_uuid)) {
    return RefusePreallocation(ledger,
                               request,
                               descriptor,
                               "filespace_preallocate_database_mismatch",
                               "storage.filespace.preallocate.database_mismatch",
                               "filespace database UUID does not match request database UUID");
  }
  if (descriptor->page_size != request.page_size_bytes) {
    return RefusePreallocation(ledger,
                               request,
                               descriptor,
                               "filespace_preallocate_page_size_mismatch",
                               "storage.filespace.preallocate.page_size_mismatch",
                               "request page size does not match filespace page profile");
  }
  if (descriptor->read_only || descriptor->state == FilespaceState::read_only) {
    return RefusePreallocation(ledger,
                               request,
                               descriptor,
                               "filespace_preallocate_read_only",
                               "storage.filespace.preallocate.read_only",
                               "target filespace is read-only");
  }
  if (descriptor->state == FilespaceState::quarantine) {
    return RefusePreallocation(ledger,
                               request,
                               descriptor,
                               "filespace_preallocate_quarantine",
                               "storage.filespace.preallocate.quarantine",
                               "target filespace is quarantined");
  }
  if (descriptor->role == FilespaceRole::forbidden || descriptor->state == FilespaceState::forbidden) {
    return RefusePreallocation(ledger,
                               request,
                               descriptor,
                               "filespace_preallocate_forbidden",
                               "storage.filespace.preallocate.forbidden",
                               "target filespace is forbidden");
  }
  if (descriptor->role == FilespaceRole::drop_pending ||
      descriptor->state == FilespaceState::drop_pending ||
      descriptor->state == FilespaceState::deleted) {
    return RefusePreallocation(ledger,
                               request,
                               descriptor,
                               "filespace_preallocate_drop_pending",
                               "storage.filespace.preallocate.drop_pending",
                               "target filespace is drop-pending or deleted");
  }
  if (!IsFilespaceOnlineForPreallocation(*descriptor)) {
    return RefusePreallocation(ledger,
                               request,
                               descriptor,
                               "filespace_preallocate_filespace_unavailable",
                               "storage.filespace.preallocate.filespace_unavailable",
                               FilespaceStateName(descriptor->state));
  }

  const auto& member = request.member_capacity;
  if (!member.present || !member.explicit_capacity_context ||
      !IsTypedEngineIdentity(member.file_member_uuid, UuidKind::object)) {
    return RefusePreallocation(ledger,
                               request,
                               descriptor,
                               "filespace_preallocate_missing_physical_member_context",
                               "storage.filespace.preallocate.missing_physical_member_context",
                               "explicit storage file/member capacity context is required");
  }
  if (!member.online || !member.writable) {
    return RefusePreallocation(ledger,
                               request,
                               descriptor,
                               "filespace_preallocate_member_unavailable",
                               "storage.filespace.preallocate.member_unavailable",
                               "storage file/member is not online and writable");
  }
  if (member.maximum_page_count == 0 ||
      member.current_page_count > member.maximum_page_count ||
      member.preallocated_page_count > member.maximum_page_count ||
      AddWouldOverflow(member.current_page_count, member.preallocated_page_count) ||
      AddWouldOverflow(member.current_page_count + member.preallocated_page_count,
                       request.requested_page_count) ||
      member.current_page_count + member.preallocated_page_count + request.requested_page_count >
          member.maximum_page_count) {
    return RefusePreallocation(ledger,
                               request,
                               descriptor,
                               "filespace_preallocate_insufficient_capacity",
                               "storage.filespace.preallocate.insufficient_capacity",
                               "storage file/member capacity window cannot satisfy requested pages");
  }
  if (MultiplyWouldOverflow(request.requested_page_count, request.page_size_bytes)) {
    return RefusePreallocation(ledger,
                               request,
                               descriptor,
                               "filespace_preallocate_capacity_overflow",
                               "storage.filespace.preallocate.capacity_overflow",
                               "requested pages overflow byte accounting");
  }

  if (auto* existing = FindMutablePreallocationByRequest(ledger, request.request_uuid);
      existing != nullptr && existing->state == FilespacePreallocationState::completed) {
    FilespacePreallocationResult result;
    result.status = GrowthOkStatus();
    result.preallocated = true;
    result.duplicate_request = true;
    result.operation = *existing;
    if (const auto* extent = FindPreallocatedExtentByOperation(
            *ledger, existing->preallocation_operation_id);
        extent != nullptr) {
      result.extent = *extent;
    }
    if (const auto* window = FindMemberCapacityWindow(*ledger,
                                                      existing->filespace_uuid,
                                                      existing->file_member_uuid);
        window != nullptr) {
      result.member_capacity_window = *window;
    }
    result.evidence = BuildPreallocationEvidence(ledger,
                                                 *existing,
                                                 "filespace_preallocate_idempotent_replay",
                                                 FilespacePreallocationState::completed,
                                                 FilespacePreallocationState::completed,
                                                 "ok",
                                                 "duplicate request returned existing preallocation",
                                                 false,
                                                 true);
    ledger->preallocation_evidence.push_back(result.evidence);
    result.diagnostic = MakeFilespacePreallocationDiagnostic(
        result.status,
        "ok",
        "storage.filespace.preallocate.idempotent_replay",
        "duplicate request returned existing preallocation");
    return result;
  }

  FilespacePreallocationEntry entry;
  entry.request_uuid = request.request_uuid;
  entry.preallocation_operation_id = PreallocationOperationId(ledger);
  entry.database_uuid = request.database_uuid;
  entry.filespace_uuid = descriptor->filespace_uuid;
  entry.policy_uuid = request.policy_uuid;
  entry.storage_profile_uuid = request.storage_profile_uuid;
  entry.file_member_uuid = member.file_member_uuid;
  entry.transaction_uuid = request.transaction_context.transaction_uuid;
  entry.filespace_role = descriptor->role;
  entry.state = FilespacePreallocationState::completed;
  entry.transaction_number = request.transaction_context.transaction_number;
  entry.requested_page_count = request.requested_page_count;
  entry.preallocated_page_count = request.requested_page_count;
  entry.bytes_preallocated = request.requested_page_count * request.page_size_bytes;
  entry.page_size_bytes = request.page_size_bytes;
  entry.start_page_number = member.start_page_number + member.current_page_count + member.preallocated_page_count;
  entry.member_page_count_before = member.current_page_count;
  entry.member_page_count_after = member.current_page_count + member.preallocated_page_count + request.requested_page_count;
  entry.member_preallocated_pages_before = member.preallocated_page_count;
  entry.member_preallocated_pages_after = member.preallocated_page_count + request.requested_page_count;
  entry.member_maximum_page_count = member.maximum_page_count;
  entry.policy_generation = request.policy_generation;
  entry.catalog_generation = request.catalog_generation;
  entry.durable_state_changed = true;
  entry.cache_invalidation_required = true;

  FilespacePreallocatedExtentMetadata extent;
  extent.request_uuid = entry.request_uuid;
  extent.preallocation_operation_id = entry.preallocation_operation_id;
  extent.database_uuid = entry.database_uuid;
  extent.filespace_uuid = entry.filespace_uuid;
  extent.policy_uuid = entry.policy_uuid;
  extent.storage_profile_uuid = entry.storage_profile_uuid;
  extent.file_member_uuid = entry.file_member_uuid;
  extent.transaction_uuid = entry.transaction_uuid;
  extent.transaction_number = entry.transaction_number;
  extent.start_page_number = entry.start_page_number;
  extent.page_count = entry.preallocated_page_count;
  extent.page_size_bytes = entry.page_size_bytes;
  extent.bytes_preallocated = entry.bytes_preallocated;
  extent.policy_generation = entry.policy_generation;
  extent.catalog_generation = entry.catalog_generation;

  FilespaceMemberCapacityWindow window;
  window.filespace_uuid = entry.filespace_uuid;
  window.file_member_uuid = entry.file_member_uuid;
  window.start_page_number = member.start_page_number;
  window.logical_page_count = member.current_page_count;
  window.physical_page_count = entry.member_page_count_after;
  window.preallocated_page_count = entry.member_preallocated_pages_after;
  window.maximum_page_count = member.maximum_page_count;

  const auto evidence = BuildPreallocationEvidence(ledger,
                                                   entry,
                                                   "filespace_preallocate_commit",
                                                   FilespacePreallocationState::absent,
                                                   FilespacePreallocationState::completed,
                                                   "ok",
                                                   request.reason.empty() ? "filespace preallocated" : request.reason,
                                                   true,
                                                   true);
  ledger->preallocation_evidence.push_back(evidence);
  ledger->preallocation_operations.push_back(entry);
  ledger->preallocated_extents.push_back(extent);
  if (auto* existing_window = FindMutableMemberCapacityWindow(ledger, entry.filespace_uuid, entry.file_member_uuid);
      existing_window != nullptr) {
    *existing_window = window;
  } else {
    ledger->member_capacity_windows.push_back(window);
  }

  const std::string database_uuid = UuidToString(entry.database_uuid.value);
  const std::string filespace_uuid = UuidToString(entry.filespace_uuid.value);
  const std::string role = FilespaceRoleName(entry.filespace_role);
  const bool reserved_metric_ok =
      scratchbird::core::metrics::PublishFilespaceReservedBytes(
          static_cast<double>(entry.bytes_preallocated),
          database_uuid,
          filespace_uuid,
          {},
          role,
          "file",
          "preallocated").ok;
  const bool request_metric_ok =
      scratchbird::core::metrics::RecordFilespaceAgentCapacityRequest(
          filespace_uuid,
          "preallocate",
          "completed").ok;
  entry.metrics_emitted = reserved_metric_ok && request_metric_ok;
  ledger->preallocation_operations.back().metrics_emitted = entry.metrics_emitted;

  FilespacePreallocationResult result;
  result.status = GrowthOkStatus();
  result.preallocated = true;
  result.durable_state_changed = true;
  result.cache_invalidation_required = true;
  result.metrics_emitted = entry.metrics_emitted;
  result.operation = entry;
  result.extent = extent;
  result.member_capacity_window = window;
  result.evidence = evidence;
  result.diagnostic = MakeFilespacePreallocationDiagnostic(result.status,
                                                          "ok",
                                                          "storage.filespace.preallocate.completed",
                                                          request.reason.empty() ? "filespace preallocated"
                                                                                 : request.reason);
  return result;
}

FilespacePhysicalGrowthResult ExecuteFilespacePhysicalGrowth(
    FilespaceGrowthLedger* ledger,
    const FilespaceRegistry& registry,
    const FilespacePhysicalGrowthRequest& request) {
  if (ledger == nullptr) {
    return RefusePhysicalGrowth(request,
                                nullptr,
                                "filespace_growth_missing_ledger",
                                "storage.filespace.growth.missing_ledger",
                                "filespace physical growth ledger is required");
  }
  if (!request.evidence_store_present) {
    return RefusePhysicalGrowth(request,
                                nullptr,
                                "filespace_growth_missing_evidence_store",
                                "storage.filespace.growth.missing_evidence_store",
                                "durable filespace growth evidence store is required");
  }
  if (!request.evidence_before_success) {
    return RefusePhysicalGrowth(request,
                                nullptr,
                                "filespace_growth_evidence_before_success_required",
                                "storage.filespace.growth.evidence_before_success_required",
                                "filespace growth success requires evidence-before-success");
  }
  if (!request.engine_owned_authority) {
    return RefusePhysicalGrowth(request,
                                nullptr,
                                "filespace_growth_non_engine_authority",
                                "storage.filespace.growth.non_engine_authority",
                                "filespace physical growth requires engine-owned storage authority");
  }
  if (request.caller_mode == FilespacePhysicalGrowthCallerMode::unknown) {
    return RefusePhysicalGrowth(request,
                                nullptr,
                                "filespace_growth_invalid_caller_mode",
                                "storage.filespace.growth.invalid_caller_mode",
                                "caller mode must be filespace_capacity_manager or direct_sysarch");
  }
  if (!request.policy_expand_allowed) {
    return RefusePhysicalGrowth(request,
                                nullptr,
                                "filespace_growth_policy_denied",
                                "storage.filespace.growth.policy_denied",
                                "filespace capacity policy does not allow live expansion");
  }
  if (!HasPhysicalGrowthAuthorization(request)) {
    return RefusePhysicalGrowth(request,
                                nullptr,
                                "filespace_growth_permission_denied",
                                "storage.filespace.growth.permission_denied",
                                "caller lacks required live growth right or approval");
  }
  if (!IsTypedEngineIdentity(request.request_uuid, UuidKind::object)) {
    return RefusePhysicalGrowth(request,
                                nullptr,
                                "filespace_growth_invalid_request_uuid",
                                "storage.filespace.growth.invalid_request_uuid",
                                "request_uuid must be a valid engine object UUID");
  }
  if (!IsTypedEngineIdentity(request.database_uuid, UuidKind::database)) {
    return RefusePhysicalGrowth(request,
                                nullptr,
                                "filespace_growth_invalid_database_uuid",
                                "storage.filespace.growth.invalid_database_uuid",
                                "database_uuid must be a valid engine database UUID");
  }
  if (!IsTypedEngineIdentity(request.filespace_uuid, UuidKind::filespace)) {
    return RefusePhysicalGrowth(request,
                                nullptr,
                                "filespace_growth_invalid_filespace_uuid",
                                "storage.filespace.growth.invalid_filespace_uuid",
                                "filespace_uuid must be a valid engine filespace UUID");
  }
  if (!IsTypedEngineIdentity(request.policy_uuid, UuidKind::object)) {
    return RefusePhysicalGrowth(request,
                                nullptr,
                                "filespace_growth_invalid_policy_uuid",
                                "storage.filespace.growth.invalid_policy_uuid",
                                "policy_uuid must be a valid engine object UUID");
  }
  if (!IsTypedEngineIdentity(request.storage_profile_uuid, UuidKind::object)) {
    return RefusePhysicalGrowth(request,
                                nullptr,
                                "filespace_growth_invalid_storage_profile_uuid",
                                "storage.filespace.growth.invalid_storage_profile_uuid",
                                "storage_profile_uuid must be a valid engine object UUID");
  }
  if (request.requested_growth_pages == 0) {
    return RefusePhysicalGrowth(request,
                                nullptr,
                                "filespace_growth_zero_pages",
                                "storage.filespace.growth.zero_pages",
                                "requested_growth_pages must be non-zero");
  }
  if (request.page_size_bytes == 0 || request.page_size_bytes % 4096 != 0) {
    return RefusePhysicalGrowth(request,
                                nullptr,
                                "filespace_growth_invalid_page_size",
                                "storage.filespace.growth.invalid_page_size",
                                "page_size_bytes must identify an admitted storage page profile");
  }
  if (request.policy_generation == 0 || request.observed_policy_generation == 0 ||
      request.policy_generation != request.observed_policy_generation) {
    return RefusePhysicalGrowth(request,
                                nullptr,
                                "filespace_growth_stale_policy_generation",
                                "storage.filespace.growth.stale_policy_generation",
                                "policy generation input is missing or stale");
  }
  if (request.catalog_generation == 0 || request.observed_catalog_generation == 0 ||
      request.catalog_generation != request.observed_catalog_generation) {
    return RefusePhysicalGrowth(request,
                                nullptr,
                                "filespace_growth_stale_catalog_generation",
                                "storage.filespace.growth.stale_catalog_generation",
                                "catalog generation input is missing or stale");
  }
  if (request.require_mga_transaction_context &&
      (!request.transaction_context.present ||
       !IsTypedEngineIdentity(request.transaction_context.transaction_uuid, UuidKind::transaction) ||
       request.transaction_context.transaction_number == 0 ||
       !request.transaction_context.durable_inventory_admitted ||
       !request.transaction_context.write_intent ||
       !request.transaction_context.durability_fence_satisfied)) {
    return RefusePhysicalGrowth(request,
                                nullptr,
                                "filespace_growth_invalid_transaction_context",
                                "storage.filespace.growth.invalid_transaction_context",
                                "durable MGA transaction context is required for filespace growth metadata changes");
  }

  const FilespaceDescriptor* descriptor = FindPreallocationTarget(registry, request.filespace_uuid);
  if (descriptor == nullptr) {
    return RefusePhysicalGrowth(request,
                                nullptr,
                                "filespace_growth_no_filespace",
                                "storage.filespace.growth.no_filespace",
                                "target filespace was not found");
  }
  if (!SameUuid(descriptor->database_uuid, request.database_uuid)) {
    return RefusePhysicalGrowth(request,
                                descriptor,
                                "filespace_growth_database_mismatch",
                                "storage.filespace.growth.database_mismatch",
                                "filespace database UUID does not match request database UUID");
  }
  if (descriptor->page_size != request.page_size_bytes) {
    return RefusePhysicalGrowth(request,
                                descriptor,
                                "filespace_growth_page_size_mismatch",
                                "storage.filespace.growth.page_size_mismatch",
                                "request page size does not match filespace page profile");
  }
  if (descriptor->read_only || descriptor->state == FilespaceState::read_only) {
    return RefusePhysicalGrowth(request,
                                descriptor,
                                "filespace_growth_read_only",
                                "storage.filespace.growth.read_only",
                                "target filespace is read-only");
  }
  if (descriptor->state == FilespaceState::quarantine) {
    return RefusePhysicalGrowth(request,
                                descriptor,
                                "filespace_growth_quarantine",
                                "storage.filespace.growth.quarantine",
                                "target filespace is quarantined");
  }
  if (descriptor->role == FilespaceRole::forbidden || descriptor->state == FilespaceState::forbidden) {
    return RefusePhysicalGrowth(request,
                                descriptor,
                                "filespace_growth_forbidden",
                                "storage.filespace.growth.forbidden",
                                "target filespace is forbidden");
  }
  if (descriptor->role == FilespaceRole::drop_pending ||
      descriptor->state == FilespaceState::drop_pending ||
      descriptor->state == FilespaceState::deleted) {
    return RefusePhysicalGrowth(request,
                                descriptor,
                                "filespace_growth_drop_pending",
                                "storage.filespace.growth.drop_pending",
                                "target filespace is drop-pending or deleted");
  }
  if (!IsFilespaceOnlineForPreallocation(*descriptor)) {
    return RefusePhysicalGrowth(request,
                                descriptor,
                                "filespace_growth_filespace_unavailable",
                                "storage.filespace.growth.filespace_unavailable",
                                FilespaceStateName(descriptor->state));
  }

  const auto& member = request.member_capacity;
  if (!member.present || !member.explicit_capacity_context ||
      !IsTypedEngineIdentity(member.file_member_uuid, UuidKind::object)) {
    return RefusePhysicalGrowth(request,
                                descriptor,
                                "filespace_growth_missing_physical_member_context",
                                "storage.filespace.growth.missing_physical_member_context",
                                "explicit storage file/member capacity context is required");
  }
  if (!member.online || !member.writable) {
    return RefusePhysicalGrowth(request,
                                descriptor,
                                "filespace_growth_member_unavailable",
                                "storage.filespace.growth.member_unavailable",
                                "storage file/member is not online and writable");
  }
  if (member.physical_path.empty()) {
    return RefusePhysicalGrowth(request,
                                descriptor,
                                "filespace_growth_missing_physical_member_path",
                                "storage.filespace.growth.missing_physical_member_path",
                                "explicit storage file/member path is required for durable physical growth");
  }
  if (member.maximum_page_count == 0 ||
      member.current_page_count > member.maximum_page_count ||
      member.preallocated_page_count > member.maximum_page_count ||
      AddWouldOverflow(member.current_page_count, member.preallocated_page_count)) {
    return RefusePhysicalGrowth(request,
                                descriptor,
                                "filespace_growth_missing_physical_member_context",
                                "storage.filespace.growth.missing_physical_member_context",
                                "storage file/member physical capacity context is inconsistent");
  }
  const u64 physical_page_count_before = member.current_page_count + member.preallocated_page_count;
  if (AddWouldOverflow(physical_page_count_before, request.requested_growth_pages) ||
      AddWouldOverflow(member.start_page_number, physical_page_count_before)) {
    return RefusePhysicalGrowth(request,
                                descriptor,
                                "filespace_growth_capacity_overflow",
                                "storage.filespace.growth.capacity_overflow",
                                "requested pages overflow physical capacity accounting");
  }
  if (physical_page_count_before + request.requested_growth_pages > member.maximum_page_count) {
    return RefusePhysicalGrowth(request,
                                descriptor,
                                "filespace_growth_insufficient_capacity",
                                "storage.filespace.growth.insufficient_capacity",
                                "storage file/member extendable capacity cannot satisfy requested pages");
  }
  if (MultiplyWouldOverflow(request.requested_growth_pages, request.page_size_bytes)) {
    return RefusePhysicalGrowth(request,
                                descriptor,
                                "filespace_growth_capacity_overflow",
                                "storage.filespace.growth.capacity_overflow",
                                "requested pages overflow byte accounting");
  }

  if (auto* existing = FindMutablePhysicalGrowthByRequest(ledger, request.request_uuid);
      existing != nullptr && existing->state == FilespacePhysicalGrowthState::completed) {
    FilespacePhysicalGrowthResult result;
    result.status = GrowthOkStatus();
    result.grown = true;
    result.duplicate_request = true;
    result.operation = *existing;
    result.durable_state_changed = false;
    result.cache_invalidation_required = false;
    result.metrics_emitted = existing->metrics_emitted;
    result.allocated_logical_pages = existing->allocated_logical_pages;
    result.page_allocation_authority_bypassed = existing->page_allocation_authority_bypassed;
    if (existing->reserve_growth_as_preallocated) {
      if (const auto* extent = FindPreallocatedExtentByOperation(*ledger, existing->growth_operation_id);
          extent != nullptr) {
        result.reserved_free_extent = *extent;
      }
    }
    if (const auto* window = FindMemberCapacityWindow(*ledger,
                                                      existing->filespace_uuid,
                                                      existing->file_member_uuid);
        window != nullptr) {
      result.member_capacity_window = *window;
    }
    result.evidence = BuildPhysicalGrowthEvidence(
        ledger,
        *existing,
        "filespace_physical_growth_idempotent_replay",
        FilespacePhysicalGrowthState::completed,
        FilespacePhysicalGrowthState::completed,
        "ok",
        "duplicate request returned existing filespace growth",
        false,
        true);
    ledger->physical_growth_evidence.push_back(result.evidence);
    result.diagnostic = MakeFilespacePhysicalGrowthDiagnostic(
        result.status,
        "ok",
        "storage.filespace.growth.idempotent_replay",
        "duplicate request returned existing filespace growth");
    return result;
  }

  FilespacePhysicalGrowthEntry entry;
  entry.request_uuid = request.request_uuid;
  entry.growth_operation_id = PhysicalGrowthOperationId(ledger);
  entry.database_uuid = request.database_uuid;
  entry.filespace_uuid = descriptor->filespace_uuid;
  entry.policy_uuid = request.policy_uuid;
  entry.storage_profile_uuid = request.storage_profile_uuid;
  entry.file_member_uuid = member.file_member_uuid;
  entry.transaction_uuid = request.transaction_context.transaction_uuid;
  entry.filespace_role = descriptor->role;
  entry.caller_mode = request.caller_mode;
  entry.state = FilespacePhysicalGrowthState::completed;
  entry.transaction_number = request.transaction_context.transaction_number;
  entry.requested_growth_pages = request.requested_growth_pages;
  entry.grown_page_count = request.requested_growth_pages;
  entry.bytes_grown = request.requested_growth_pages * request.page_size_bytes;
  entry.page_size_bytes = request.page_size_bytes;
  entry.growth_start_page_number = member.start_page_number + physical_page_count_before;
  entry.member_physical_page_count_before = physical_page_count_before;
  entry.member_physical_page_count_after = physical_page_count_before + request.requested_growth_pages;
  entry.member_preallocated_pages_before = member.preallocated_page_count;
  entry.member_preallocated_pages_after =
      member.preallocated_page_count +
      (request.reserve_growth_as_preallocated ? request.requested_growth_pages : 0);
  entry.member_maximum_page_count = member.maximum_page_count;
  entry.physical_extension_required = true;
  entry.policy_generation = request.policy_generation;
  entry.catalog_generation = request.catalog_generation;
  entry.reserve_growth_as_preallocated = request.reserve_growth_as_preallocated;
  entry.durable_state_changed = true;
  entry.cache_invalidation_required = true;

  FilespaceMemberCapacityWindow window;
  window.filespace_uuid = entry.filespace_uuid;
  window.file_member_uuid = entry.file_member_uuid;
  window.start_page_number = member.start_page_number;
  window.logical_page_count = member.current_page_count;
  window.physical_page_count = entry.member_physical_page_count_after;
  window.preallocated_page_count = entry.member_preallocated_pages_after;
  window.maximum_page_count = member.maximum_page_count;

  FilespacePreallocatedExtentMetadata extent;
  if (entry.reserve_growth_as_preallocated) {
    extent.request_uuid = entry.request_uuid;
    extent.preallocation_operation_id = entry.growth_operation_id;
    extent.database_uuid = entry.database_uuid;
    extent.filespace_uuid = entry.filespace_uuid;
    extent.policy_uuid = entry.policy_uuid;
    extent.storage_profile_uuid = entry.storage_profile_uuid;
    extent.file_member_uuid = entry.file_member_uuid;
    extent.transaction_uuid = entry.transaction_uuid;
    extent.transaction_number = entry.transaction_number;
    extent.start_page_number = entry.growth_start_page_number;
    extent.page_count = entry.grown_page_count;
    extent.page_size_bytes = entry.page_size_bytes;
    extent.bytes_preallocated = entry.bytes_grown;
    extent.policy_generation = entry.policy_generation;
    extent.catalog_generation = entry.catalog_generation;
  }

  const auto physical_growth = ExtendPhysicalFilespaceCapacity(
      member.physical_path,
      entry.member_physical_page_count_before,
      entry.member_preallocated_pages_before,
      entry.grown_page_count,
      entry.reserve_growth_as_preallocated);
  if (!physical_growth.ok()) {
    return RefusePhysicalGrowth(request,
                                descriptor,
                                physical_growth.diagnostic.diagnostic_code.empty()
                                    ? "filespace_growth_physical_extension_failed"
                                    : physical_growth.diagnostic.diagnostic_code,
                                physical_growth.diagnostic.message_key.empty()
                                    ? "storage.filespace.growth.physical_extension_failed"
                                    : physical_growth.diagnostic.message_key,
                                "physical filespace member extension failed before metadata commit");
  }
  entry.physical_file_size_before_bytes = physical_growth.file_size_before_bytes;
  entry.physical_file_size_after_bytes = physical_growth.file_size_after_bytes;
  entry.physical_file_expected_size_after_bytes =
      physical_growth.expected_capacity_after_bytes;
  entry.extent_preallocation_offset_bytes =
      physical_growth.extent_preallocation_offset_bytes;
  entry.extent_preallocation_bytes =
      physical_growth.extent_preallocation_bytes;
  entry.extent_preallocation_strategy =
      physical_growth.extent_preallocation_strategy;
  entry.extent_preallocation_fallback_reason =
      physical_growth.extent_preallocation_fallback_reason;
  entry.extent_preallocation_attempted =
      physical_growth.extent_preallocation_attempted;
  entry.extent_preallocation_succeeded =
      physical_growth.extent_preallocation_succeeded;
  entry.extent_preallocation_fallback_used =
      physical_growth.extent_preallocation_fallback_used;
  entry.physical_extension_completed = physical_growth.physical_extension_completed;
  entry.physical_extension_synced = physical_growth.physical_extension_synced;
  entry.physical_header_updated = physical_growth.header_updated;
  entry.metadata_commit_after_physical_extension =
      entry.physical_extension_completed &&
      entry.physical_extension_synced &&
      entry.physical_header_updated;

  const auto evidence = BuildPhysicalGrowthEvidence(
      ledger,
      entry,
      "filespace_physical_growth_commit",
      FilespacePhysicalGrowthState::absent,
      FilespacePhysicalGrowthState::completed,
      "ok",
      request.reason.empty() ? "filespace physical capacity grown" : request.reason,
      true,
      true);
  ledger->physical_growth_evidence.push_back(evidence);
  ledger->physical_growth_operations.push_back(entry);
  if (entry.reserve_growth_as_preallocated) {
    ledger->preallocated_extents.push_back(extent);
  }
  if (auto* existing_window = FindMutableMemberCapacityWindow(ledger, entry.filespace_uuid, entry.file_member_uuid);
      existing_window != nullptr) {
    *existing_window = window;
  } else {
    ledger->member_capacity_windows.push_back(window);
  }

  const std::string database_uuid = UuidToString(entry.database_uuid.value);
  const std::string filespace_uuid = UuidToString(entry.filespace_uuid.value);
  const std::string role = FilespaceRoleName(entry.filespace_role);
  const bool capacity_metric_ok =
      scratchbird::core::metrics::PublishFilespaceCapacitySnapshot(
          static_cast<double>(entry.member_physical_page_count_after) *
              static_cast<double>(entry.page_size_bytes),
          static_cast<double>(window.logical_page_count) *
              static_cast<double>(entry.page_size_bytes),
          static_cast<double>(entry.member_preallocated_pages_after) *
              static_cast<double>(entry.page_size_bytes),
          database_uuid,
          filespace_uuid,
          {},
          role,
          "file").ok;
  const bool request_metric_ok =
      scratchbird::core::metrics::RecordFilespaceAgentCapacityRequest(
          filespace_uuid,
          "physical_growth",
          FilespacePhysicalGrowthCallerModeName(entry.caller_mode)).ok;
  entry.metrics_emitted = capacity_metric_ok && request_metric_ok;
  ledger->physical_growth_operations.back().metrics_emitted = entry.metrics_emitted;

  FilespacePhysicalGrowthResult result;
  result.status = GrowthOkStatus();
  result.grown = true;
  result.durable_state_changed = true;
  result.cache_invalidation_required = true;
  result.metrics_emitted = entry.metrics_emitted;
  result.allocated_logical_pages = false;
  result.page_allocation_authority_bypassed = false;
  result.operation = entry;
  result.reserved_free_extent = extent;
  result.member_capacity_window = window;
  result.evidence = evidence;
  result.diagnostic = MakeFilespacePhysicalGrowthDiagnostic(
      result.status,
      "ok",
      "storage.filespace.growth.completed",
      request.reason.empty() ? "filespace physical capacity grown" : request.reason);
  return result;
}

FilespaceGrowthMutationResult CompleteInsertFilespaceGrowth(FilespaceGrowthLedger* ledger,
                                                            const CompleteFilespaceGrowthRequest& request) {
  if (ledger == nullptr) {
    return GrowthMutationRefuse(nullptr,
                                request,
                                "insert_filespace_growth_missing_ledger",
                                "storage.filespace.insert_growth.missing_ledger",
                                "filespace growth ledger is required");
  }
  if (!request.growth_operation_id.valid()) {
    return GrowthMutationRefuse(ledger,
                                request,
                                "insert_filespace_growth_invalid_operation_id",
                                "storage.filespace.insert_growth.invalid_operation_id",
                                "growth_operation_id must be valid");
  }

  auto* operation = FindMutableGrowthOperation(ledger, request.growth_operation_id);
  if (operation == nullptr) {
    return GrowthMutationRefuse(ledger,
                                request,
                                "insert_filespace_growth_operation_not_found",
                                "storage.filespace.insert_growth.operation_not_found",
                                "growth operation was not found");
  }
  if (operation->state == InsertFilespaceGrowthState::allocation_complete) {
    FilespaceGrowthMutationResult result;
    result.status = GrowthOkStatus();
    result.changed = false;
    result.operation = *operation;
    result.evidence = BuildEvidence(ledger,
                                    *operation,
                                    "complete_insert_filespace_growth_noop",
                                    InsertFilespaceGrowthState::allocation_complete,
                                    InsertFilespaceGrowthState::allocation_complete,
                                    "ok",
                                    "already complete",
                                    false);
    result.diagnostic = MakeFilespaceGrowthDiagnostic(result.status,
                                                     "ok",
                                                     "storage.filespace.insert_growth.already_complete",
                                                     "already complete");
    ledger->evidence.push_back(result.evidence);
    return result;
  }
  if (operation->state != InsertFilespaceGrowthState::admitted_pending_allocation) {
    return GrowthMutationRefuse(ledger,
                                request,
                                "insert_filespace_growth_not_completable",
                                "storage.filespace.insert_growth.not_completable",
                                InsertFilespaceGrowthStateName(operation->state));
  }
  if (request.actual_usable_pages != 0 && request.actual_usable_pages < operation->expected_usable_pages) {
    return GrowthMutationRefuse(ledger,
                                request,
                                "insert_filespace_growth_actual_pages_below_expected",
                                "storage.filespace.insert_growth.actual_pages_below_expected",
                                "actual_usable_pages is below admitted expected_usable_pages");
  }

  const auto previous_state = operation->state;
  operation->state = InsertFilespaceGrowthState::allocation_complete;
  if (request.actual_usable_pages != 0) {
    operation->expected_usable_pages = request.actual_usable_pages;
  }

  FilespaceGrowthMutationResult result;
  result.status = GrowthOkStatus();
  result.changed = true;
  result.operation = *operation;
  result.evidence = BuildEvidence(ledger,
                                  *operation,
                                  "complete_insert_filespace_growth",
                                  previous_state,
                                  InsertFilespaceGrowthState::allocation_complete,
                                  "ok",
                                  request.reason.empty() ? "allocation complete" : request.reason,
                                  true);
  result.diagnostic = MakeFilespaceGrowthDiagnostic(result.status,
                                                   "ok",
                                                   "storage.filespace.insert_growth.completed",
                                                   request.reason.empty() ? "allocation complete" : request.reason);
  ledger->evidence.push_back(result.evidence);
  (void)scratchbird::core::metrics::RecordFilespaceAgentCapacityRequest(UuidToString(operation->filespace_uuid.value),
                                                                        "insert_growth",
                                                                        "allocation_complete");
  return result;
}

FilespaceGrowthRecoveryClassification ClassifyFilespaceGrowthForRecovery(
    const InsertFilespaceGrowthEntry& operation) {
  FilespaceGrowthRecoveryClassification classification;
  classification.growth_operation_id = operation.growth_operation_id;
  classification.observed_state = operation.state;

  switch (operation.state) {
    case InsertFilespaceGrowthState::admitted_pending_allocation:
      classification.action = FilespaceGrowthRecoveryAction::complete;
      classification.fail_closed = false;
      classification.stable_reason = "durable growth admission must complete allocation or be retried idempotently";
      break;
    case InsertFilespaceGrowthState::allocation_complete:
    case InsertFilespaceGrowthState::refused:
    case InsertFilespaceGrowthState::absent:
      classification.action = FilespaceGrowthRecoveryAction::no_action;
      classification.fail_closed = false;
      classification.stable_reason = "state has no restart mutation";
      break;
    case InsertFilespaceGrowthState::quarantine:
      classification.action = FilespaceGrowthRecoveryAction::fail_closed;
      classification.fail_closed = true;
      classification.stable_reason = "quarantined growth operation blocks automatic restart";
      break;
  }
  return classification;
}

FilespaceGrowthRecoveryResult ClassifyFilespaceGrowthLedgerForRecovery(const FilespaceGrowthLedger& ledger) {
  FilespaceGrowthRecoveryResult result;
  result.status = GrowthOkStatus();
  result.diagnostic = MakeFilespaceGrowthDiagnostic(result.status,
                                                   "ok",
                                                   "storage.filespace.insert_growth.recovery_classified",
                                                   "filespace growth ledger classified");
  result.classifications.reserve(ledger.operations.size());
  for (const auto& operation : ledger.operations) {
    result.classifications.push_back(ClassifyFilespaceGrowthForRecovery(operation));
  }
  return result;
}

const InsertFilespaceGrowthEntry* FindFilespaceGrowthOperation(const FilespaceGrowthLedger& ledger,
                                                               const TypedUuid& growth_operation_id) {
  const auto found = std::find_if(ledger.operations.begin(),
                                  ledger.operations.end(),
                                  [&](const InsertFilespaceGrowthEntry& operation) {
                                    return SameUuid(operation.growth_operation_id, growth_operation_id);
                                  });
  return found == ledger.operations.end() ? nullptr : &(*found);
}

FilespacePreallocationRecoveryClassification ClassifyFilespacePreallocationForRecovery(
    const FilespacePreallocationEntry& operation) {
  FilespacePreallocationRecoveryClassification classification;
  classification.preallocation_operation_id = operation.preallocation_operation_id;
  classification.observed_state = operation.state;

  switch (operation.state) {
    case FilespacePreallocationState::completed:
      classification.action = FilespacePreallocationRecoveryAction::retain_reserved_free;
      classification.fail_closed = false;
      classification.stable_reason = "completed preallocation retains reserved-free extent metadata";
      break;
    case FilespacePreallocationState::refused:
    case FilespacePreallocationState::absent:
      classification.action = FilespacePreallocationRecoveryAction::no_action;
      classification.fail_closed = false;
      classification.stable_reason = "state has no durable preallocation mutation";
      break;
    case FilespacePreallocationState::quarantine:
      classification.action = FilespacePreallocationRecoveryAction::fail_closed;
      classification.fail_closed = true;
      classification.stable_reason = "quarantined preallocation blocks automatic restart";
      break;
  }
  return classification;
}

FilespacePreallocationRecoveryResult ClassifyFilespacePreallocationLedgerForRecovery(
    const FilespaceGrowthLedger& ledger) {
  FilespacePreallocationRecoveryResult result;
  result.status = GrowthOkStatus();
  result.diagnostic = MakeFilespacePreallocationDiagnostic(
      result.status,
      "ok",
      "storage.filespace.preallocate.recovery_classified",
      "filespace preallocation ledger classified");
  result.classifications.reserve(ledger.preallocation_operations.size());
  for (const auto& operation : ledger.preallocation_operations) {
    result.classifications.push_back(ClassifyFilespacePreallocationForRecovery(operation));
  }
  return result;
}

const FilespacePreallocationEntry* FindFilespacePreallocationOperation(
    const FilespaceGrowthLedger& ledger,
    const TypedUuid& preallocation_operation_id) {
  const auto found = std::find_if(ledger.preallocation_operations.begin(),
                                  ledger.preallocation_operations.end(),
                                  [&](const FilespacePreallocationEntry& operation) {
                                    return SameUuid(operation.preallocation_operation_id,
                                                    preallocation_operation_id);
                                  });
  return found == ledger.preallocation_operations.end() ? nullptr : &(*found);
}

FilespacePhysicalGrowthRecoveryClassification ClassifyFilespacePhysicalGrowthForRecovery(
    const FilespacePhysicalGrowthEntry& operation) {
  FilespacePhysicalGrowthRecoveryClassification classification;
  classification.growth_operation_id = operation.growth_operation_id;
  classification.observed_state = operation.state;

  switch (operation.state) {
    case FilespacePhysicalGrowthState::completed:
      if (!operation.physical_extension_required ||
          !operation.physical_extension_completed ||
          !operation.physical_extension_synced ||
          !operation.physical_header_updated ||
          !operation.metadata_commit_after_physical_extension ||
          operation.extent_preallocation_bytes == 0 ||
          operation.extent_preallocation_strategy.empty() ||
          !operation.extent_preallocation_attempted ||
          (!operation.extent_preallocation_succeeded &&
           !operation.extent_preallocation_fallback_used) ||
          operation.physical_file_size_after_bytes <
              operation.physical_file_size_before_bytes ||
          operation.extent_preallocation_bytes !=
              (operation.physical_file_size_after_bytes -
               operation.physical_file_size_before_bytes) ||
          operation.physical_file_size_after_bytes !=
              operation.physical_file_expected_size_after_bytes) {
        classification.action = FilespacePhysicalGrowthRecoveryAction::fail_closed;
        classification.fail_closed = true;
        classification.stable_reason =
            "completed filespace growth lacks durable physical extension proof";
      } else {
        classification.action = FilespacePhysicalGrowthRecoveryAction::retain_physical_growth;
        classification.fail_closed = false;
        classification.stable_reason =
            "completed filespace growth retains physically extended member capacity";
      }
      break;
    case FilespacePhysicalGrowthState::refused:
    case FilespacePhysicalGrowthState::absent:
      classification.action = FilespacePhysicalGrowthRecoveryAction::no_action;
      classification.fail_closed = false;
      classification.stable_reason = "state has no durable filespace growth mutation";
      break;
    case FilespacePhysicalGrowthState::quarantine:
      classification.action = FilespacePhysicalGrowthRecoveryAction::fail_closed;
      classification.fail_closed = true;
      classification.stable_reason = "quarantined filespace growth blocks automatic restart";
      break;
  }
  return classification;
}

FilespacePhysicalGrowthRecoveryResult ClassifyFilespacePhysicalGrowthLedgerForRecovery(
    const FilespaceGrowthLedger& ledger) {
  FilespacePhysicalGrowthRecoveryResult result;
  result.status = GrowthOkStatus();
  result.diagnostic = MakeFilespacePhysicalGrowthDiagnostic(
      result.status,
      "ok",
      "storage.filespace.growth.recovery_classified",
      "filespace physical growth ledger classified");
  result.classifications.reserve(ledger.physical_growth_operations.size());
  for (const auto& operation : ledger.physical_growth_operations) {
    result.classifications.push_back(ClassifyFilespacePhysicalGrowthForRecovery(operation));
  }
  return result;
}

const FilespacePhysicalGrowthEntry* FindFilespacePhysicalGrowthOperation(
    const FilespaceGrowthLedger& ledger,
    const TypedUuid& growth_operation_id) {
  const auto found = std::find_if(ledger.physical_growth_operations.begin(),
                                  ledger.physical_growth_operations.end(),
                                  [&](const FilespacePhysicalGrowthEntry& operation) {
                                    return SameUuid(operation.growth_operation_id,
                                                    growth_operation_id);
                                  });
  return found == ledger.physical_growth_operations.end() ? nullptr : &(*found);
}

DiagnosticRecord MakeFilespaceGrowthDiagnostic(Status status,
                                               std::string diagnostic_code,
                                               std::string message_key,
                                               std::string detail) {
  std::vector<scratchbird::core::platform::DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return scratchbird::core::platform::MakeDiagnostic(status.code,
                                                     status.severity,
                                                     status.subsystem,
                                                     std::move(diagnostic_code),
                                                     std::move(message_key),
                                                     std::move(arguments),
                                                     {},
                                                     "storage.filespace.insert_growth",
                                                     status.ok() ? "" : "refuse insert filespace growth and retry only after filespace authority is available");
}

DiagnosticRecord MakeFilespacePreallocationDiagnostic(Status status,
                                                      std::string diagnostic_code,
                                                      std::string message_key,
                                                      std::string detail) {
  std::vector<scratchbird::core::platform::DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return scratchbird::core::platform::MakeDiagnostic(
      status.code,
      status.severity,
      status.subsystem,
      std::move(diagnostic_code),
      std::move(message_key),
      std::move(arguments),
      {},
      "storage.filespace.preallocate",
      status.ok() ? "" : "refuse filespace preallocation and retry only after storage authority is available");
}

DiagnosticRecord MakeFilespacePhysicalGrowthDiagnostic(Status status,
                                                       std::string diagnostic_code,
                                                       std::string message_key,
                                                       std::string detail) {
  std::vector<scratchbird::core::platform::DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return scratchbird::core::platform::MakeDiagnostic(
      status.code,
      status.severity,
      status.subsystem,
      std::move(diagnostic_code),
      std::move(message_key),
      std::move(arguments),
      {},
      "storage.filespace.growth",
      status.ok() ? "" : "refuse filespace physical growth and retry only after storage authority is available");
}

}  // namespace scratchbird::storage::filespace
