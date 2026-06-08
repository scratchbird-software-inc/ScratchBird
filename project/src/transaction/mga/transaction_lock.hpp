// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-SNTXN-LOCK-ANCHOR
#include "transaction_inventory.hpp"

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace scratchbird::transaction::mga {

enum class TransactionLockMode : u16 {
  shared,
  exclusive,
  unknown
};

enum class TransactionLockDecision : u16 {
  granted,
  already_owned,
  wait_required,
  timeout,
  deadlock_detected,
  admission_refused,
  invalid_request
};

struct TransactionWaitPolicy {
  u64 timeout_millis = 0;
  bool no_wait = true;
  u64 wait_start_unix_epoch_millis = 0;
  u64 now_unix_epoch_millis = 0;
};

struct TransactionLockRequest {
  LocalTransactionId requester;
  std::string resource_key;
  TransactionLockMode mode = TransactionLockMode::exclusive;
  TransactionWaitPolicy wait_policy;
  bool transaction_active = true;
};

struct TransactionLockResult {
  Status status;
  TransactionLockDecision decision = TransactionLockDecision::invalid_request;
  LocalTransactionId blocking_transaction;
  u64 retry_after_millis = 0;
  u64 wait_elapsed_millis = 0;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok() && (decision == TransactionLockDecision::granted ||
                           decision == TransactionLockDecision::already_owned);
  }
};

class LocalTransactionLockTable {
 public:
  // SEARCH_KEY: SB_MGA_LOCAL_LOCK_TABLE_SHARED_EXCLUSIVE
  struct LockRecord {
    std::vector<LocalTransactionId> owners;
    TransactionLockMode mode = TransactionLockMode::unknown;
  };

  struct AdmissionPolicy {
    bool accepting_new_waits = true;
    u64 max_held_locks = 0;
    u64 max_waiters = 0;
  };

  struct WaitingRecord {
    TransactionLockRequest request;
    LocalTransactionId blocking_transaction;
    u64 wait_sequence = 0;
    u64 retry_count = 0;
  };

  void SetAdmissionPolicy(AdmissionPolicy policy);
  TransactionLockResult Acquire(TransactionLockRequest request);
  TransactionLockResult Retry(LocalTransactionId requester);
  TransactionLockResult Release(LocalTransactionId owner, std::string resource_key);
  u64 ReleaseAll(LocalTransactionId owner);
  u64 held_lock_count() const;
  u64 waiting_lock_count() const;

 private:
  TransactionLockResult AcquireInternal(TransactionLockRequest request, u64 preserved_wait_sequence);
  u64 held_lock_count_unlocked() const;
  u64 waiting_lock_count_unlocked() const;

  mutable std::mutex mutex_;
  std::map<std::string, LockRecord> locks_;
  std::map<u64, WaitingRecord> waiting_;
  AdmissionPolicy admission_policy_;
  u64 next_wait_sequence_ = 1;
};

const char* TransactionLockDecisionName(TransactionLockDecision decision);
DiagnosticRecord MakeTransactionLockDiagnostic(Status status,
                                               std::string diagnostic_code,
                                               std::string message_key,
                                               std::string detail = {});

}  // namespace scratchbird::transaction::mga
