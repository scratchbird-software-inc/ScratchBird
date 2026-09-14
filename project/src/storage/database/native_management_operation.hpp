// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "runtime_platform.hpp"
#include <array>
#include <optional>
#include <string>
#include <vector>

namespace scratchbird::storage::database {
using core::platform::Uuid;
using core::platform::byte;
using core::platform::u16;
using core::platform::u32;
using core::platform::u64;
enum class NativeManagementState : u16 {
  created=1,authorized,planned,running,pausing,paused,cancelling,blocked,
  committing,failed_retryable,recovering,orphaned,cancelled,completed,failed_terminal
};
enum class NativeManagementStepState : u16 {
  pending=1,ready,running,blocked,compensating,failed_retryable,completed,
  skipped,compensated,failed_terminal
};
enum class NativeManagementScope : u16 { local_database=1,local_node,cluster };
enum class NativeManagementRestart : u16 { resume=1,roll_forward,compensate,mark_retryable,mark_terminal,operator_review };
enum class NativeManagementMutation : u16 { none=0,page,catalog,filespace,index,archive,backup,stream,security,cluster,evidence,metric };
enum class NativeManagementCompensation : u16 { none=0,retry,compensate,operator_review,terminal_failure };
enum class NativeManagementRecovery : u16 { classify=1,retry,roll_forward,compensate,mark_retryable,mark_terminal,orphan };
struct NativeManagementStep {
  Uuid uuid,operation_uuid,family_uuid,target_uuid,started_at,completed_at;
  Uuid evidence_uuid,metric_evidence_uuid,diagnostic_uuid,boundary_uuid;
  std::array<byte,32> precondition_sha256{},postcondition_sha256{};
  u32 ordinal=0;
  std::string idempotency_key;
  NativeManagementStepState state=NativeManagementStepState::pending;
  NativeManagementMutation mutation=NativeManagementMutation::none;
  NativeManagementCompensation compensation=NativeManagementCompensation::none;
  NativeManagementRecovery recovery=NativeManagementRecovery::classify;
  bool evidence_required=false,metrics_required=false,idempotent=false;
  bool operator==(const NativeManagementStep&) const=default;
};
struct NativeManagementOperation {
  Uuid database_uuid,bootstrap_uuid,uuid,descriptor_uuid,family_uuid,target_type_uuid,target_uuid;
  Uuid initiator_uuid,request_context_uuid,policy_snapshot_uuid,security_snapshot_uuid,phase_uuid;
  Uuid boundary_uuid,created_at,updated_at,terminal_at,resource_plan_uuid,lock_plan_uuid;
  Uuid result_uuid,diagnostic_uuid,evidence_uuid,metric_evidence_uuid,cluster_uuid;
  std::array<byte,32> normalized_request_sha256{};
  // Catalog, configuration, security, cluster epoch. Present zero != absent.
  std::array<std::optional<u64>,4> generation_guards{};
  u64 revision=1;
  std::string idempotency_key;
  NativeManagementState state=NativeManagementState::created;
  NativeManagementScope scope=NativeManagementScope::local_node;
  u16 initiator_kind=0;
  NativeManagementRestart restart=NativeManagementRestart::resume;
  bool evidence_required=false,metrics_required=false;
  std::vector<NativeManagementStep> steps;
  bool operator==(const NativeManagementOperation&) const=default;
};
enum class NativeManagementOperationError {
  none,invalid_header,invalid_identity,invalid_record,invalid_utf8,
  invalid_integrity,invalid_transition,immutable_field,resource_exhausted,hash_failure
};
struct NativeManagementOperationImage {
  NativeManagementOperationError error=NativeManagementOperationError::invalid_record;
  std::optional<NativeManagementOperation> record;
  std::vector<byte> bytes;
  // Hash of ALL stored bytes including the embedded seal.
  std::array<byte,32> sha256{};
  bool ok() const noexcept {return error==NativeManagementOperationError::none&&record.has_value();}
};
// Record shape/common history only. No authentication, native storage,
// checkpoint selection, operation outcome or client completion is granted.
NativeManagementOperationError ValidateNativeManagementOperation(const NativeManagementOperation&) noexcept;
NativeManagementOperationError ValidateNativeManagementOperationEvolution(
  const NativeManagementOperation&,const NativeManagementOperation&) noexcept;
NativeManagementOperationImage EncodeNativeManagementOperation(const NativeManagementOperation&,u64 maximum_encoded_bytes) noexcept;
NativeManagementOperationImage DecodeNativeManagementOperation(const std::vector<byte>&,u64 maximum_encoded_bytes) noexcept;
} // namespace scratchbird::storage::database
