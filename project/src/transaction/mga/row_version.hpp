// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "runtime_platform.hpp"
#include "transaction_state.hpp"
#include "uuid.hpp"

#include <string>

namespace scratchbird::transaction::mga {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u64;

inline constexpr u64 kInvalidRowVersionSequence = 0;

enum class RowVersionState : u16 {
  unknown,
  uncommitted,
  prepared,
  committed,
  rolled_back,
  delete_marker,
  limbo,
  recovery_required
};

enum class VisibilityDecision : u16 {
  visible,
  invisible,
  wait_for_transaction,
  requires_recovery,
  unknown
};

enum class HotStableRowHeadDecisionKind : u16 {
  refused,
  page_local_hot,
  stable_row_head_indirection,
  ordinary_index_rewrite
};

struct RowIdentity {
  TypedUuid row_uuid;

  constexpr bool valid() const {
    return row_uuid.kind == UuidKind::row && row_uuid.valid();
  }
};

struct RowVersionChainLinks {
  TypedUuid previous_version_uuid;
  TypedUuid next_version_uuid;
  u64 previous_version_sequence = kInvalidRowVersionSequence;
  u64 next_version_sequence = kInvalidRowVersionSequence;

  constexpr bool has_previous() const {
    return previous_version_sequence != kInvalidRowVersionSequence;
  }

  constexpr bool has_next() const {
    return next_version_sequence != kInvalidRowVersionSequence;
  }
};

struct RowVersionIdentity {
  RowIdentity row;
  TransactionIdentity creator_transaction;
  u64 version_sequence = kInvalidRowVersionSequence;

  constexpr bool valid() const {
    return row.valid() && creator_transaction.valid() && version_sequence != kInvalidRowVersionSequence;
  }
};

struct RowVersionMetadata {
  RowVersionIdentity identity;
  RowVersionChainLinks chain;
  LocalTransactionId successor_transaction_local_id;
  RowVersionState state = RowVersionState::unknown;
  TransactionState creator_transaction_state = TransactionState::none;
  bool payload_present = false;
};

struct VisibilitySnapshot {
  LocalTransactionId reader_transaction;
  u64 visible_through_local_transaction_id = kInvalidLocalTransactionId;
  bool visible_through_local_transaction_id_is_boundary = false;
  bool allow_reader_own_uncommitted = true;
  bool recovery_context = false;
};

struct RowIdentityResult {
  Status status;
  RowIdentity identity;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct RowVersionIdentityResult {
  Status status;
  RowVersionIdentity identity;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct RowVersionMetadataResult {
  Status status;
  RowVersionMetadata metadata;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct VisibilityResult {
  Status status;
  VisibilityDecision decision = VisibilityDecision::unknown;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct HotStableRowHeadProofInput {
  RowVersionMetadata old_visible_version;
  RowVersionMetadata new_version;
  TypedUuid old_version_uuid;
  TypedUuid new_previous_version_uuid;
  VisibilitySnapshot visibility_snapshot;
  bool exact_index_keys_unchanged = false;
  bool same_page_budget_available = false;
  bool parser_or_donor_authority = false;
};

struct HotStableRowHeadDecisionResult {
  Status status;
  HotStableRowHeadDecisionKind decision = HotStableRowHeadDecisionKind::refused;
  bool proof_accepted = false;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

const char* RowVersionStateName(RowVersionState state);
const char* VisibilityDecisionName(VisibilityDecision decision);
const char* HotStableRowHeadDecisionName(HotStableRowHeadDecisionKind decision);
RowIdentityResult MakeRowIdentity(TypedUuid row_uuid);
RowIdentityResult ValidateRowIdentity(const RowIdentity& identity);
RowVersionIdentityResult MakeRowVersionIdentity(RowIdentity row,
                                                TransactionIdentity creator_transaction,
                                                u64 version_sequence);
RowVersionIdentityResult ValidateRowVersionIdentity(const RowVersionIdentity& identity);
RowVersionMetadataResult ValidateRowVersionMetadata(const RowVersionMetadata& metadata);
VisibilityResult EvaluateVisibility(const RowVersionMetadata& metadata,
                                    const VisibilitySnapshot& snapshot);
HotStableRowHeadDecisionResult EvaluateHotStableRowHeadDecision(
    const HotStableRowHeadProofInput& input);
DiagnosticRecord MakeRowVersionDiagnostic(Status status,
                                          std::string diagnostic_code,
                                          std::string message_key,
                                          std::string detail = {});

}  // namespace scratchbird::transaction::mga
