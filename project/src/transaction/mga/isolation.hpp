// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-SNTXN-ISOLATION-ANCHOR
#include "transaction_snapshot.hpp"

#include <string>
#include <vector>

namespace scratchbird::transaction::mga {

enum class IsolationLevel : u16 {
  read_committed,
  repeatable_read,
  serializable,
  donor_compatibility,
  unknown
};

struct IsolationLevelResult {
  Status status;
  IsolationLevel level = IsolationLevel::unknown;
  bool supported = false;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok() && supported;
  }
};

enum class SerializableAccessKind : u16 {
  point_read,
  range_read,
  predicate_read,
  insert,
  update,
  delete_row,
  unknown
};

enum class SerializableConflictKind : u16 {
  none,
  phantom_insert,
  read_write,
  write_write,
  external_authority_refused,
  inventory_authority_required,
  invalid_request,
  unknown
};

enum class SerializableRetryClass : u16 {
  none,
  serialization_retry,
  wait_for_transaction,
  recovery_required,
  invalid_request
};

struct SerializableKeyRange {
  TypedUuid relation_uuid;
  std::string lower_bound;
  std::string upper_bound;
  bool lower_unbounded = false;
  bool upper_unbounded = false;
  bool lower_inclusive = true;
  bool upper_inclusive = true;
  bool full_relation = false;
  std::string predicate_digest;
};

struct SerializableAccessRecord {
  LocalTransactionId local_id;
  TransactionState transaction_state = TransactionState::none;
  SerializableAccessKind kind = SerializableAccessKind::unknown;
  SerializableKeyRange range;
  u64 sequence = 0;
  bool durable_inventory_authoritative = false;
  bool parser_or_donor_authority = false;
};

struct SerializableConflictResult {
  Status status;
  SerializableConflictKind conflict = SerializableConflictKind::unknown;
  SerializableRetryClass retry_class = SerializableRetryClass::invalid_request;
  bool admitted = false;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;

  bool ok() const {
    return status.ok() && admitted;
  }
};

class SerializableConflictTracker {
 public:
  SerializableConflictResult RecordAccess(const SerializableAccessRecord& access);
  SerializableConflictResult CheckWrite(const SerializableAccessRecord& write) const;
  SerializableConflictResult RecordWrite(const SerializableAccessRecord& write);
  u64 access_count() const;

 private:
  std::vector<SerializableAccessRecord> accesses_;
};

const char* IsolationLevelName(IsolationLevel level);
const char* SerializableAccessKindName(SerializableAccessKind kind);
const char* SerializableConflictKindName(SerializableConflictKind kind);
const char* SerializableRetryClassName(SerializableRetryClass retry_class);
IsolationLevelResult ValidateLocalIsolationLevel(IsolationLevel level);
VisibilitySnapshot SnapshotPolicyForIsolation(IsolationLevel level,
                                              const LocalTransactionSnapshot& snapshot);
SerializableKeyRange MakeSerializablePointRange(TypedUuid relation_uuid,
                                                std::string key);
SerializableKeyRange MakeSerializableBoundedRange(TypedUuid relation_uuid,
                                                  std::string lower_bound,
                                                  std::string upper_bound);
SerializableKeyRange MakeSerializablePredicateRange(TypedUuid relation_uuid,
                                                    std::string predicate_digest);
SerializableConflictResult EvaluateSerializableWriteConflict(
    const std::vector<SerializableAccessRecord>& existing_accesses,
    const SerializableAccessRecord& write);

}  // namespace scratchbird::transaction::mga
