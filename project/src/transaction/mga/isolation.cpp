// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "isolation.hpp"

#include "uuid.hpp"

#include <algorithm>
#include <utility>
#include <vector>

namespace scratchbird::transaction::mga {
namespace {

using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::UuidKind;

Status IsolationOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::transaction_mga};
}

Status IsolationConflictStatus() {
  return {StatusCode::platform_required_feature_missing,
          Severity::error,
          Subsystem::transaction_mga};
}

bool SameRelation(const SerializableKeyRange& left,
                  const SerializableKeyRange& right) {
  return left.relation_uuid.kind == right.relation_uuid.kind &&
         left.relation_uuid.value == right.relation_uuid.value;
}

bool IsWriteKind(SerializableAccessKind kind) {
  return kind == SerializableAccessKind::insert ||
         kind == SerializableAccessKind::update ||
         kind == SerializableAccessKind::delete_row;
}

bool IsReadKind(SerializableAccessKind kind) {
  return kind == SerializableAccessKind::point_read ||
         kind == SerializableAccessKind::range_read ||
         kind == SerializableAccessKind::predicate_read;
}

bool IsPendingFinality(TransactionState state) {
  return state == TransactionState::active ||
         state == TransactionState::preparing ||
         state == TransactionState::prepared ||
         state == TransactionState::committing;
}

bool IsRecoveryState(TransactionState state) {
  return state == TransactionState::limbo ||
         state == TransactionState::recovering;
}

bool IsValidRelationUuid(const TypedUuid& relation_uuid) {
  return relation_uuid.kind == UuidKind::object &&
         relation_uuid.valid() &&
         scratchbird::core::uuid::IsEngineIdentityUuid(relation_uuid.value);
}

bool BoundsOverlap(const SerializableKeyRange& left,
                   const SerializableKeyRange& right) {
  if (!SameRelation(left, right)) {
    return false;
  }
  if (left.full_relation || right.full_relation) {
    return true;
  }
  if (!left.predicate_digest.empty() || !right.predicate_digest.empty()) {
    return left.predicate_digest.empty() ||
           right.predicate_digest.empty() ||
           left.predicate_digest == right.predicate_digest;
  }
  const bool left_before_right =
      !left.upper_unbounded &&
      !right.lower_unbounded &&
      (left.upper_bound < right.lower_bound ||
       (left.upper_bound == right.lower_bound &&
        (!left.upper_inclusive || !right.lower_inclusive)));
  const bool right_before_left =
      !right.upper_unbounded &&
      !left.lower_unbounded &&
      (right.upper_bound < left.lower_bound ||
       (right.upper_bound == left.lower_bound &&
        (!right.upper_inclusive || !left.lower_inclusive)));
  return !left_before_right && !right_before_left;
}

void AddEvidence(SerializableConflictResult* result,
                 std::string value) {
  result->evidence.push_back(std::move(value));
}

SerializableConflictResult SerializableAdmitted(std::string evidence) {
  SerializableConflictResult result;
  result.status = IsolationOkStatus();
  result.conflict = SerializableConflictKind::none;
  result.retry_class = SerializableRetryClass::none;
  result.admitted = true;
  AddEvidence(&result, std::move(evidence));
  AddEvidence(&result, "serializable.inventory_authority=durable_transaction_inventory");
  AddEvidence(&result, "serializable.parser_or_reference_authority=false");
  return result;
}

SerializableConflictResult SerializableRefused(SerializableConflictKind conflict,
                                               SerializableRetryClass retry_class,
                                               std::string diagnostic_code,
                                               std::string message_key,
                                               std::string detail = {}) {
  SerializableConflictResult result;
  result.status = IsolationConflictStatus();
  result.conflict = conflict;
  result.retry_class = retry_class;
  result.admitted = false;
  result.diagnostic = MakeTransactionSnapshotDiagnostic(result.status,
                                                        std::move(diagnostic_code),
                                                        std::move(message_key),
                                                        std::move(detail));
  AddEvidence(&result, std::string("serializable.conflict=") +
                       SerializableConflictKindName(conflict));
  AddEvidence(&result, std::string("serializable.retry_class=") +
                       SerializableRetryClassName(retry_class));
  AddEvidence(&result, "serializable.inventory_authority=durable_transaction_inventory");
  return result;
}

SerializableConflictResult ValidateSerializableAccess(
    const SerializableAccessRecord& access) {
  if (access.parser_or_reference_authority) {
    return SerializableRefused(
        SerializableConflictKind::external_authority_refused,
        SerializableRetryClass::invalid_request,
        "SB-SNTXN-SERIALIZABLE-EXTERNAL-AUTHORITY-REFUSED",
        "transaction.serializable.external_authority_refused");
  }
  if (!access.durable_inventory_authoritative) {
    return SerializableRefused(
        SerializableConflictKind::inventory_authority_required,
        SerializableRetryClass::invalid_request,
        "SB-SNTXN-SERIALIZABLE-INVENTORY-AUTHORITY-REQUIRED",
        "transaction.serializable.inventory_authority_required");
  }
  if (!access.local_id.valid() ||
      access.sequence == 0 ||
      access.kind == SerializableAccessKind::unknown ||
      (!IsReadKind(access.kind) && !IsWriteKind(access.kind)) ||
      !IsValidRelationUuid(access.range.relation_uuid)) {
    return SerializableRefused(
        SerializableConflictKind::invalid_request,
        SerializableRetryClass::invalid_request,
        "SB-SNTXN-SERIALIZABLE-INVALID-ACCESS",
        "transaction.serializable.invalid_access");
  }
  if (!access.range.full_relation &&
      access.range.predicate_digest.empty() &&
      (access.range.lower_bound.empty() || access.range.upper_bound.empty()) &&
      (!access.range.lower_unbounded || !access.range.upper_unbounded)) {
    return SerializableRefused(
        SerializableConflictKind::invalid_request,
        SerializableRetryClass::invalid_request,
        "SB-SNTXN-SERIALIZABLE-RANGE-INVALID",
        "transaction.serializable.range_invalid");
  }
  return SerializableAdmitted("serializable.access_valid=true");
}

}  // namespace

const char* IsolationLevelName(IsolationLevel level) {
  switch (level) {
    case IsolationLevel::read_committed: return "read_committed";
    case IsolationLevel::repeatable_read: return "repeatable_read";
    case IsolationLevel::serializable: return "serializable";
    case IsolationLevel::reference_compatibility: return "reference_compatibility";
    case IsolationLevel::unknown: return "unknown";
  }
  return "unknown";
}

const char* SerializableAccessKindName(SerializableAccessKind kind) {
  switch (kind) {
    case SerializableAccessKind::point_read: return "point_read";
    case SerializableAccessKind::range_read: return "range_read";
    case SerializableAccessKind::predicate_read: return "predicate_read";
    case SerializableAccessKind::insert: return "insert";
    case SerializableAccessKind::update: return "update";
    case SerializableAccessKind::delete_row: return "delete_row";
    case SerializableAccessKind::unknown: return "unknown";
  }
  return "unknown";
}

const char* SerializableConflictKindName(SerializableConflictKind kind) {
  switch (kind) {
    case SerializableConflictKind::none: return "none";
    case SerializableConflictKind::phantom_insert: return "phantom_insert";
    case SerializableConflictKind::read_write: return "read_write";
    case SerializableConflictKind::write_write: return "write_write";
    case SerializableConflictKind::external_authority_refused:
      return "external_authority_refused";
    case SerializableConflictKind::inventory_authority_required:
      return "inventory_authority_required";
    case SerializableConflictKind::invalid_request: return "invalid_request";
    case SerializableConflictKind::unknown: return "unknown";
  }
  return "unknown";
}

const char* SerializableRetryClassName(SerializableRetryClass retry_class) {
  switch (retry_class) {
    case SerializableRetryClass::none: return "none";
    case SerializableRetryClass::serialization_retry: return "serialization_retry";
    case SerializableRetryClass::wait_for_transaction: return "wait_for_transaction";
    case SerializableRetryClass::recovery_required: return "recovery_required";
    case SerializableRetryClass::invalid_request: return "invalid_request";
  }
  return "invalid_request";
}

IsolationLevelResult ValidateLocalIsolationLevel(IsolationLevel level) {
  IsolationLevelResult result;
  result.level = level;
  result.status = {scratchbird::core::platform::StatusCode::ok,
                   scratchbird::core::platform::Severity::info,
                   scratchbird::core::platform::Subsystem::transaction_mga};
  result.supported = level == IsolationLevel::read_committed ||
                     level == IsolationLevel::repeatable_read ||
                     level == IsolationLevel::serializable;
  if (!result.supported) {
    result.status = {scratchbird::core::platform::StatusCode::platform_required_feature_missing,
                     scratchbird::core::platform::Severity::error,
                     scratchbird::core::platform::Subsystem::transaction_mga};
    result.diagnostic = MakeTransactionSnapshotDiagnostic(result.status,
                                                          "SB-SNTXN-ISOLATION-UNSUPPORTED",
                                                          "transaction.isolation.unsupported",
                                                          IsolationLevelName(level));
  }
  return result;
}

VisibilitySnapshot SnapshotPolicyForIsolation(IsolationLevel level,
                                              const LocalTransactionSnapshot& snapshot) {
  VisibilitySnapshot visibility;
  visibility.reader_transaction = snapshot.reader_transaction;
  visibility.visible_through_local_transaction_id =
      snapshot.transaction_start_visible_through_local_transaction.value;
  visibility.visible_through_local_transaction_id_is_boundary = true;
  visibility.allow_reader_own_uncommitted = true;
  visibility.recovery_context = false;
  if (level == IsolationLevel::read_committed) {
    visibility.visible_through_local_transaction_id = snapshot.visible_through_local_transaction.value;
  }
  return visibility;
}

SerializableKeyRange MakeSerializablePointRange(TypedUuid relation_uuid,
                                                std::string key) {
  SerializableKeyRange range;
  range.relation_uuid = relation_uuid;
  range.lower_bound = key;
  range.upper_bound = std::move(key);
  return range;
}

SerializableKeyRange MakeSerializableBoundedRange(TypedUuid relation_uuid,
                                                  std::string lower_bound,
                                                  std::string upper_bound) {
  SerializableKeyRange range;
  range.relation_uuid = relation_uuid;
  range.lower_bound = std::move(lower_bound);
  range.upper_bound = std::move(upper_bound);
  return range;
}

SerializableKeyRange MakeSerializablePredicateRange(TypedUuid relation_uuid,
                                                    std::string predicate_digest) {
  SerializableKeyRange range;
  range.relation_uuid = relation_uuid;
  range.lower_unbounded = true;
  range.upper_unbounded = true;
  range.full_relation = true;
  range.predicate_digest = std::move(predicate_digest);
  return range;
}

SerializableConflictResult EvaluateSerializableWriteConflict(
    const std::vector<SerializableAccessRecord>& existing_accesses,
    const SerializableAccessRecord& write) {
  const auto valid = ValidateSerializableAccess(write);
  if (!valid.ok()) {
    return valid;
  }
  if (!IsWriteKind(write.kind)) {
    return SerializableRefused(
        SerializableConflictKind::invalid_request,
        SerializableRetryClass::invalid_request,
        "SB-SNTXN-SERIALIZABLE-WRITE-REQUIRED",
        "transaction.serializable.write_required",
        SerializableAccessKindName(write.kind));
  }
  for (const SerializableAccessRecord& existing : existing_accesses) {
    if (existing.local_id.value == write.local_id.value) {
      continue;
    }
    const auto existing_valid = ValidateSerializableAccess(existing);
    if (!existing_valid.ok()) {
      return existing_valid;
    }
    if (!BoundsOverlap(existing.range, write.range)) {
      continue;
    }
    if (IsRecoveryState(existing.transaction_state) ||
        IsRecoveryState(write.transaction_state)) {
      return SerializableRefused(
          SerializableConflictKind::read_write,
          SerializableRetryClass::recovery_required,
          "SB-SNTXN-SERIALIZABLE-RECOVERY-REQUIRED",
          "transaction.serializable.recovery_required");
    }
    if (IsWriteKind(existing.kind)) {
      return SerializableRefused(
          SerializableConflictKind::write_write,
          IsPendingFinality(existing.transaction_state)
              ? SerializableRetryClass::wait_for_transaction
              : SerializableRetryClass::serialization_retry,
          "SB-SNTXN-SERIALIZABLE-WRITE-WRITE-CONFLICT",
          "transaction.serializable.write_write_conflict",
          SerializableAccessKindName(existing.kind));
    }
    if (existing.kind == SerializableAccessKind::predicate_read ||
        existing.kind == SerializableAccessKind::range_read) {
      const SerializableConflictKind conflict =
          write.kind == SerializableAccessKind::insert
              ? SerializableConflictKind::phantom_insert
              : SerializableConflictKind::read_write;
      return SerializableRefused(
          conflict,
          SerializableRetryClass::serialization_retry,
          conflict == SerializableConflictKind::phantom_insert
              ? "SB-SNTXN-SERIALIZABLE-PHANTOM-REFUSED"
              : "SB-SNTXN-SERIALIZABLE-READ-WRITE-CONFLICT",
          conflict == SerializableConflictKind::phantom_insert
              ? "transaction.serializable.phantom_refused"
              : "transaction.serializable.read_write_conflict",
          SerializableAccessKindName(write.kind));
    }
    if (existing.kind == SerializableAccessKind::point_read) {
      return SerializableRefused(
          SerializableConflictKind::read_write,
          SerializableRetryClass::serialization_retry,
          "SB-SNTXN-SERIALIZABLE-READ-WRITE-CONFLICT",
          "transaction.serializable.read_write_conflict",
          SerializableAccessKindName(write.kind));
    }
  }
  return SerializableAdmitted("serializable.write_admitted=true");
}

SerializableConflictResult SerializableConflictTracker::RecordAccess(
    const SerializableAccessRecord& access) {
  const auto valid = ValidateSerializableAccess(access);
  if (!valid.ok()) {
    return valid;
  }
  if (IsWriteKind(access.kind)) {
    return RecordWrite(access);
  }
  accesses_.push_back(access);
  auto result = SerializableAdmitted("serializable.read_recorded=true");
  AddEvidence(&result, std::string("serializable.access_kind=") +
                       SerializableAccessKindName(access.kind));
  return result;
}

SerializableConflictResult SerializableConflictTracker::CheckWrite(
    const SerializableAccessRecord& write) const {
  return EvaluateSerializableWriteConflict(accesses_, write);
}

SerializableConflictResult SerializableConflictTracker::RecordWrite(
    const SerializableAccessRecord& write) {
  auto result = CheckWrite(write);
  if (!result.ok()) {
    return result;
  }
  accesses_.push_back(write);
  AddEvidence(&result, std::string("serializable.access_kind=") +
                       SerializableAccessKindName(write.kind));
  AddEvidence(&result, "serializable.write_recorded=true");
  return result;
}

u64 SerializableConflictTracker::access_count() const {
  return static_cast<u64>(accesses_.size());
}

}  // namespace scratchbird::transaction::mga
