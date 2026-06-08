// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-INSERT-FILESPACE-GROWTH-ANCHOR
#include "filespace_lifecycle.hpp"
#include "runtime_platform.hpp"
#include "uuid.hpp"

#include <string>
#include <vector>

namespace scratchbird::storage::filespace {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

enum class InsertFilespaceGrowthUrgency : u32 {
  background,
  normal,
  high,
  critical
};

enum class InsertFilespaceGrowthWaitPolicy : u32 {
  no_wait,
  bounded_wait,
  background_only,
  refused
};

enum class InsertFilespaceGrowthState : u32 {
  absent,
  admitted_pending_allocation,
  allocation_complete,
  refused,
  quarantine
};

enum class FilespaceGrowthRecoveryAction : u32 {
  no_action,
  complete,
  roll_back,
  quarantine,
  fail_closed
};

enum class FilespacePreallocationState : u32 {
  absent,
  completed,
  refused,
  quarantine
};

enum class FilespacePreallocationRecoveryAction : u32 {
  no_action,
  retain_reserved_free,
  quarantine,
  fail_closed
};

enum class FilespacePhysicalGrowthCallerMode : u32 {
  unknown,
  filespace_capacity_manager,
  direct_sysarch
};

enum class FilespacePhysicalGrowthState : u32 {
  absent,
  completed,
  refused,
  quarantine
};

enum class FilespacePhysicalGrowthRecoveryAction : u32 {
  no_action,
  retain_physical_growth,
  quarantine,
  fail_closed
};

struct FilespacePreallocationTransactionContext {
  bool present = false;
  TypedUuid transaction_uuid;
  u64 transaction_number = 0;
  bool durable_inventory_admitted = false;
  bool write_intent = false;
  bool durability_fence_satisfied = false;
};

struct FilespaceStorageMemberCapacityContext {
  bool present = false;
  bool explicit_capacity_context = false;
  TypedUuid file_member_uuid;
  u64 start_page_number = 0;
  u64 current_page_count = 0;
  u64 preallocated_page_count = 0;
  u64 maximum_page_count = 0;
  std::string physical_path;
  bool online = false;
  bool writable = false;
};

struct FilespacePhysicalGrowthAuthorizationContext {
  bool obs_agent_control_right = false;
  bool filespace_lifecycle_right = false;
  bool storage_filespace_control_right = false;
  bool action_approval = false;
};

struct FilespacePreallocationRequest {
  TypedUuid request_uuid;
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  TypedUuid policy_uuid;
  TypedUuid storage_profile_uuid;
  u64 requested_page_count = 0;
  u32 page_size_bytes = 0;
  u64 policy_generation = 0;
  u64 observed_policy_generation = 0;
  u64 catalog_generation = 0;
  u64 observed_catalog_generation = 0;
  FilespaceStorageMemberCapacityContext member_capacity;
  FilespacePreallocationTransactionContext transaction_context;
  bool evidence_store_present = false;
  bool evidence_before_success = true;
  bool require_mga_transaction_context = true;
  std::string reason;
};

struct FilespacePhysicalGrowthRequest {
  TypedUuid request_uuid;
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  TypedUuid policy_uuid;
  TypedUuid storage_profile_uuid;
  u64 requested_growth_pages = 0;
  u32 page_size_bytes = 0;
  u64 policy_generation = 0;
  u64 observed_policy_generation = 0;
  u64 catalog_generation = 0;
  u64 observed_catalog_generation = 0;
  FilespacePhysicalGrowthCallerMode caller_mode = FilespacePhysicalGrowthCallerMode::unknown;
  FilespacePhysicalGrowthAuthorizationContext authorization;
  FilespaceStorageMemberCapacityContext member_capacity;
  FilespacePreallocationTransactionContext transaction_context;
  bool evidence_store_present = false;
  bool evidence_before_success = true;
  bool policy_expand_allowed = false;
  bool engine_owned_authority = false;
  bool require_mga_transaction_context = true;
  bool reserve_growth_as_preallocated = false;
  std::string reason;
};

struct InsertFilespaceGrowthRequest {
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  FilespaceRole filespace_role = FilespaceRole::secondary_data;
  std::string page_family;
  u64 requested_page_count = 0;
  InsertFilespaceGrowthUrgency urgency_class = InsertFilespaceGrowthUrgency::normal;
  u64 predicted_insert_pressure_pages = 0;
  TypedUuid policy_uuid;
};

struct InsertFilespaceGrowthEvidenceRecord {
  u64 sequence = 0;
  std::string action;
  TypedUuid growth_operation_id;
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  FilespaceRole filespace_role = FilespaceRole::unknown;
  std::string page_family;
  u64 requested_page_count = 0;
  u64 predicted_insert_pressure_pages = 0;
  u64 expected_usable_pages = 0;
  InsertFilespaceGrowthUrgency urgency_class = InsertFilespaceGrowthUrgency::normal;
  InsertFilespaceGrowthWaitPolicy wait_policy = InsertFilespaceGrowthWaitPolicy::refused;
  InsertFilespaceGrowthState previous_state = InsertFilespaceGrowthState::absent;
  InsertFilespaceGrowthState new_state = InsertFilespaceGrowthState::absent;
  std::string reason;
  std::string diagnostic_code;
  bool durable_state_changed = false;
};

struct InsertFilespaceGrowthEntry {
  TypedUuid growth_operation_id;
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  TypedUuid policy_uuid;
  FilespaceRole filespace_role = FilespaceRole::unknown;
  std::string page_family;
  u64 requested_page_count = 0;
  u64 predicted_insert_pressure_pages = 0;
  u64 expected_usable_pages = 0;
  InsertFilespaceGrowthUrgency urgency_class = InsertFilespaceGrowthUrgency::normal;
  InsertFilespaceGrowthWaitPolicy wait_policy = InsertFilespaceGrowthWaitPolicy::refused;
  InsertFilespaceGrowthState state = InsertFilespaceGrowthState::absent;
};

struct FilespacePreallocatedExtentMetadata {
  TypedUuid request_uuid;
  TypedUuid preallocation_operation_id;
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  TypedUuid policy_uuid;
  TypedUuid storage_profile_uuid;
  TypedUuid file_member_uuid;
  TypedUuid transaction_uuid;
  u64 transaction_number = 0;
  u64 start_page_number = 0;
  u64 page_count = 0;
  u64 page_size_bytes = 0;
  u64 bytes_preallocated = 0;
  u64 policy_generation = 0;
  u64 catalog_generation = 0;
};

struct FilespaceMemberCapacityWindow {
  TypedUuid filespace_uuid;
  TypedUuid file_member_uuid;
  u64 start_page_number = 0;
  u64 logical_page_count = 0;
  u64 physical_page_count = 0;
  u64 preallocated_page_count = 0;
  u64 maximum_page_count = 0;
};

struct FilespacePreallocationEntry {
  TypedUuid request_uuid;
  TypedUuid preallocation_operation_id;
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  TypedUuid policy_uuid;
  TypedUuid storage_profile_uuid;
  TypedUuid file_member_uuid;
  TypedUuid transaction_uuid;
  FilespaceRole filespace_role = FilespaceRole::unknown;
  FilespacePreallocationState state = FilespacePreallocationState::absent;
  u64 transaction_number = 0;
  u64 requested_page_count = 0;
  u64 preallocated_page_count = 0;
  u64 bytes_preallocated = 0;
  u64 page_size_bytes = 0;
  u64 start_page_number = 0;
  u64 member_page_count_before = 0;
  u64 member_page_count_after = 0;
  u64 member_preallocated_pages_before = 0;
  u64 member_preallocated_pages_after = 0;
  u64 member_maximum_page_count = 0;
  u64 policy_generation = 0;
  u64 catalog_generation = 0;
  bool durable_state_changed = false;
  bool cache_invalidation_required = false;
  bool metrics_emitted = false;
};

struct FilespacePhysicalGrowthEntry {
  TypedUuid request_uuid;
  TypedUuid growth_operation_id;
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  TypedUuid policy_uuid;
  TypedUuid storage_profile_uuid;
  TypedUuid file_member_uuid;
  TypedUuid transaction_uuid;
  FilespaceRole filespace_role = FilespaceRole::unknown;
  FilespacePhysicalGrowthCallerMode caller_mode = FilespacePhysicalGrowthCallerMode::unknown;
  FilespacePhysicalGrowthState state = FilespacePhysicalGrowthState::absent;
  u64 transaction_number = 0;
  u64 requested_growth_pages = 0;
  u64 grown_page_count = 0;
  u64 bytes_grown = 0;
  u64 page_size_bytes = 0;
  u64 growth_start_page_number = 0;
  u64 member_physical_page_count_before = 0;
  u64 member_physical_page_count_after = 0;
  u64 member_preallocated_pages_before = 0;
  u64 member_preallocated_pages_after = 0;
  u64 member_maximum_page_count = 0;
  u64 physical_file_size_before_bytes = 0;
  u64 physical_file_size_after_bytes = 0;
  u64 physical_file_expected_size_after_bytes = 0;
  u64 policy_generation = 0;
  u64 catalog_generation = 0;
  bool reserve_growth_as_preallocated = false;
  bool physical_extension_required = false;
  bool physical_extension_completed = false;
  bool physical_extension_synced = false;
  bool physical_header_updated = false;
  bool metadata_commit_after_physical_extension = false;
  bool durable_state_changed = false;
  bool cache_invalidation_required = false;
  bool metrics_emitted = false;
  bool allocated_logical_pages = false;
  bool page_allocation_authority_bypassed = false;
};

struct FilespacePreallocationEvidenceRecord {
  u64 sequence = 0;
  std::string action;
  TypedUuid request_uuid;
  TypedUuid preallocation_operation_id;
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  TypedUuid policy_uuid;
  TypedUuid file_member_uuid;
  TypedUuid transaction_uuid;
  FilespaceRole filespace_role = FilespaceRole::unknown;
  FilespacePreallocationState previous_state = FilespacePreallocationState::absent;
  FilespacePreallocationState new_state = FilespacePreallocationState::absent;
  u64 requested_page_count = 0;
  u64 preallocated_page_count = 0;
  u64 bytes_preallocated = 0;
  u64 policy_generation = 0;
  u64 catalog_generation = 0;
  std::string reason;
  std::string diagnostic_code;
  bool durable_state_changed = false;
  bool evidence_before_success = false;
};

struct FilespacePhysicalGrowthEvidenceRecord {
  u64 sequence = 0;
  std::string action;
  TypedUuid request_uuid;
  TypedUuid growth_operation_id;
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  TypedUuid policy_uuid;
  TypedUuid file_member_uuid;
  TypedUuid transaction_uuid;
  FilespaceRole filespace_role = FilespaceRole::unknown;
  FilespacePhysicalGrowthCallerMode caller_mode = FilespacePhysicalGrowthCallerMode::unknown;
  FilespacePhysicalGrowthState previous_state = FilespacePhysicalGrowthState::absent;
  FilespacePhysicalGrowthState new_state = FilespacePhysicalGrowthState::absent;
  u64 requested_growth_pages = 0;
  u64 grown_page_count = 0;
  u64 bytes_grown = 0;
  u64 physical_file_size_before_bytes = 0;
  u64 physical_file_size_after_bytes = 0;
  u64 physical_file_expected_size_after_bytes = 0;
  u64 policy_generation = 0;
  u64 catalog_generation = 0;
  std::string reason;
  std::string diagnostic_code;
  bool physical_extension_required = false;
  bool physical_extension_completed = false;
  bool physical_extension_synced = false;
  bool physical_header_updated = false;
  bool metadata_commit_after_physical_extension = false;
  bool durable_state_changed = false;
  bool evidence_before_success = false;
};

struct FilespaceGrowthLedger {
  std::vector<InsertFilespaceGrowthEntry> operations;
  std::vector<InsertFilespaceGrowthEvidenceRecord> evidence;
  std::vector<FilespacePreallocationEntry> preallocation_operations;
  std::vector<FilespacePreallocationEvidenceRecord> preallocation_evidence;
  std::vector<FilespacePreallocatedExtentMetadata> preallocated_extents;
  std::vector<FilespaceMemberCapacityWindow> member_capacity_windows;
  std::vector<FilespacePhysicalGrowthEntry> physical_growth_operations;
  std::vector<FilespacePhysicalGrowthEvidenceRecord> physical_growth_evidence;
  u64 next_evidence_sequence = 1;
};

struct InsertFilespaceGrowthResult {
  Status status;
  bool admitted = false;
  TypedUuid growth_operation_id;
  u64 expected_usable_pages = 0;
  InsertFilespaceGrowthWaitPolicy wait_policy = InsertFilespaceGrowthWaitPolicy::refused;
  InsertFilespaceGrowthEntry operation;
  InsertFilespaceGrowthEvidenceRecord evidence;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && admitted; }
};

struct CompleteFilespaceGrowthRequest {
  TypedUuid growth_operation_id;
  u64 actual_usable_pages = 0;
  std::string reason;
};

struct FilespaceGrowthMutationResult {
  Status status;
  bool changed = false;
  InsertFilespaceGrowthEntry operation;
  InsertFilespaceGrowthEvidenceRecord evidence;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok(); }
};

struct FilespaceGrowthRecoveryClassification {
  TypedUuid growth_operation_id;
  InsertFilespaceGrowthState observed_state = InsertFilespaceGrowthState::absent;
  FilespaceGrowthRecoveryAction action = FilespaceGrowthRecoveryAction::fail_closed;
  bool fail_closed = false;
  std::string stable_reason;
};

struct FilespaceGrowthRecoveryResult {
  Status status;
  std::vector<FilespaceGrowthRecoveryClassification> classifications;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok(); }
};

struct FilespacePreallocationResult {
  Status status;
  bool preallocated = false;
  bool duplicate_request = false;
  bool durable_state_changed = false;
  bool cache_invalidation_required = false;
  bool metrics_emitted = false;
  FilespacePreallocationEntry operation;
  FilespacePreallocatedExtentMetadata extent;
  FilespaceMemberCapacityWindow member_capacity_window;
  FilespacePreallocationEvidenceRecord evidence;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && preallocated; }
};

struct FilespacePhysicalGrowthResult {
  Status status;
  bool grown = false;
  bool duplicate_request = false;
  bool durable_state_changed = false;
  bool cache_invalidation_required = false;
  bool metrics_emitted = false;
  bool allocated_logical_pages = false;
  bool page_allocation_authority_bypassed = false;
  FilespacePhysicalGrowthEntry operation;
  FilespacePreallocatedExtentMetadata reserved_free_extent;
  FilespaceMemberCapacityWindow member_capacity_window;
  FilespacePhysicalGrowthEvidenceRecord evidence;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && grown; }
};

struct FilespacePreallocationRecoveryClassification {
  TypedUuid preallocation_operation_id;
  FilespacePreallocationState observed_state = FilespacePreallocationState::absent;
  FilespacePreallocationRecoveryAction action = FilespacePreallocationRecoveryAction::fail_closed;
  bool fail_closed = false;
  std::string stable_reason;
};

struct FilespacePreallocationRecoveryResult {
  Status status;
  std::vector<FilespacePreallocationRecoveryClassification> classifications;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok(); }
};

struct FilespacePhysicalGrowthRecoveryClassification {
  TypedUuid growth_operation_id;
  FilespacePhysicalGrowthState observed_state = FilespacePhysicalGrowthState::absent;
  FilespacePhysicalGrowthRecoveryAction action = FilespacePhysicalGrowthRecoveryAction::fail_closed;
  bool fail_closed = false;
  std::string stable_reason;
};

struct FilespacePhysicalGrowthRecoveryResult {
  Status status;
  std::vector<FilespacePhysicalGrowthRecoveryClassification> classifications;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok(); }
};

const char* InsertFilespaceGrowthUrgencyName(InsertFilespaceGrowthUrgency urgency);
const char* InsertFilespaceGrowthWaitPolicyName(InsertFilespaceGrowthWaitPolicy policy);
const char* InsertFilespaceGrowthStateName(InsertFilespaceGrowthState state);
const char* FilespaceGrowthRecoveryActionName(FilespaceGrowthRecoveryAction action);
const char* FilespacePreallocationStateName(FilespacePreallocationState state);
const char* FilespacePreallocationRecoveryActionName(FilespacePreallocationRecoveryAction action);
const char* FilespacePhysicalGrowthCallerModeName(FilespacePhysicalGrowthCallerMode mode);
const char* FilespacePhysicalGrowthStateName(FilespacePhysicalGrowthState state);
const char* FilespacePhysicalGrowthRecoveryActionName(FilespacePhysicalGrowthRecoveryAction action);

InsertFilespaceGrowthResult RequestInsertFilespaceGrowth(FilespaceGrowthLedger* ledger,
                                                         const FilespaceRegistry& registry,
                                                         const InsertFilespaceGrowthRequest& request);
FilespacePreallocationResult PreallocateFilespace(FilespaceGrowthLedger* ledger,
                                                  const FilespaceRegistry& registry,
                                                  const FilespacePreallocationRequest& request);
FilespacePhysicalGrowthResult ExecuteFilespacePhysicalGrowth(
    FilespaceGrowthLedger* ledger,
    const FilespaceRegistry& registry,
    const FilespacePhysicalGrowthRequest& request);
FilespaceGrowthMutationResult CompleteInsertFilespaceGrowth(FilespaceGrowthLedger* ledger,
                                                            const CompleteFilespaceGrowthRequest& request);
FilespaceGrowthRecoveryClassification ClassifyFilespaceGrowthForRecovery(
    const InsertFilespaceGrowthEntry& operation);
FilespaceGrowthRecoveryResult ClassifyFilespaceGrowthLedgerForRecovery(const FilespaceGrowthLedger& ledger);
FilespacePreallocationRecoveryClassification ClassifyFilespacePreallocationForRecovery(
    const FilespacePreallocationEntry& operation);
FilespacePreallocationRecoveryResult ClassifyFilespacePreallocationLedgerForRecovery(
    const FilespaceGrowthLedger& ledger);
FilespacePhysicalGrowthRecoveryClassification ClassifyFilespacePhysicalGrowthForRecovery(
    const FilespacePhysicalGrowthEntry& operation);
FilespacePhysicalGrowthRecoveryResult ClassifyFilespacePhysicalGrowthLedgerForRecovery(
    const FilespaceGrowthLedger& ledger);
const InsertFilespaceGrowthEntry* FindFilespaceGrowthOperation(const FilespaceGrowthLedger& ledger,
                                                               const TypedUuid& growth_operation_id);
const FilespacePreallocationEntry* FindFilespacePreallocationOperation(
    const FilespaceGrowthLedger& ledger,
    const TypedUuid& preallocation_operation_id);
const FilespacePhysicalGrowthEntry* FindFilespacePhysicalGrowthOperation(
    const FilespaceGrowthLedger& ledger,
    const TypedUuid& growth_operation_id);
DiagnosticRecord MakeFilespaceGrowthDiagnostic(Status status,
                                               std::string diagnostic_code,
                                               std::string message_key,
                                               std::string detail = {});
DiagnosticRecord MakeFilespacePreallocationDiagnostic(Status status,
                                                      std::string diagnostic_code,
                                                      std::string message_key,
                                                      std::string detail = {});
DiagnosticRecord MakeFilespacePhysicalGrowthDiagnostic(Status status,
                                                       std::string diagnostic_code,
                                                       std::string message_key,
                                                       std::string detail = {});

}  // namespace scratchbird::storage::filespace
