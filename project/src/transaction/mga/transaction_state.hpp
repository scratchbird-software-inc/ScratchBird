// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "runtime_platform.hpp"
#include "uuid.hpp"

#include <string>
#include <vector>

namespace scratchbird::transaction::mga {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u64;

inline constexpr u64 kInvalidLocalTransactionId = 0;

enum class TransactionScope : u16 {
  local_node,
  cluster_global,
  unknown
};

enum class TransactionState : u16 {
  none,
  created,
  active,
  preparing,
  prepared,
  committing,
  committed,
  rolling_back,
  rolled_back,
  limbo,
  recovering,
  failed_terminal,
  archived,
  read_only_active
};

enum class TransactionTransitionClass : u16 {
  user,
  coordinator,
  recovery,
  cleanup,
  invalid
};

struct LocalTransactionId {
  u64 value = kInvalidLocalTransactionId;

  constexpr bool valid() const {
    return value != kInvalidLocalTransactionId;
  }
};

struct TransactionIdentity {
  LocalTransactionId local_id;
  TypedUuid transaction_uuid;
  TransactionScope scope = TransactionScope::unknown;

  constexpr bool valid() const {
    return local_id.valid() && transaction_uuid.kind == UuidKind::transaction && transaction_uuid.valid() &&
           scope != TransactionScope::unknown;
  }
};

struct TransactionStateTransition {
  TransactionState from = TransactionState::none;
  TransactionState to = TransactionState::none;
  TransactionTransitionClass transition_class = TransactionTransitionClass::invalid;
  bool recovery_only = false;
  const char* stable_name = "invalid";
};

struct TransactionIdentityResult {
  Status status;
  TransactionIdentity identity;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct TransactionTransitionResult {
  Status status;
  bool allowed = false;
  TransactionStateTransition transition;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok() && allowed;
  }
};

const char* TransactionScopeName(TransactionScope scope);
const char* TransactionStateName(TransactionState state);
const char* TransactionTransitionClassName(TransactionTransitionClass transition_class);
LocalTransactionId MakeLocalTransactionId(u64 value);
bool IsTerminalTransactionState(TransactionState state);
const std::vector<TransactionStateTransition>& BuiltinTransactionStateTransitions();
TransactionIdentityResult MakeTransactionIdentity(LocalTransactionId local_id,
                                                  TypedUuid transaction_uuid,
                                                  TransactionScope scope);
TransactionIdentityResult ValidateTransactionIdentity(const TransactionIdentity& identity);
TransactionTransitionResult CheckTransactionStateTransition(TransactionState from,
                                                           TransactionState to,
                                                           bool recovery_context = false);
DiagnosticRecord MakeTransactionDiagnostic(Status status,
                                           std::string diagnostic_code,
                                           std::string message_key,
                                           std::string detail = {});

}  // namespace scratchbird::transaction::mga
