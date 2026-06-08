// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "transaction_lock.hpp"

#include "metric_producer.hpp"

#include <algorithm>
#include <utility>
#include <vector>

namespace scratchbird::transaction::mga {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status LockOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::transaction_mga};
}

Status LockErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::transaction_mga};
}

TransactionLockResult LockError(TransactionLockDecision decision,
                                std::string diagnostic_code,
                                std::string message_key,
                                std::string detail = {}) {
  TransactionLockResult result;
  result.status = LockErrorStatus();
  result.decision = decision;
  result.diagnostic = MakeTransactionLockDiagnostic(result.status,
                                                    std::move(diagnostic_code),
                                                    std::move(message_key),
                                                    std::move(detail));
  return result;
}

void PublishLockDecisionMetric(TransactionLockDecision decision) {
  (void)scratchbird::core::metrics::IncrementCounter(
      "sb_tx_lock_decisions_total",
      scratchbird::core::metrics::Labels({{"component", "transaction.mga.lock"},
                                          {"decision", TransactionLockDecisionName(decision)}}),
      1.0,
      "transaction_mga_lock");
}

void PublishLockGaugeCounts(u64 held_locks, u64 waiters) {
  (void)scratchbird::core::metrics::SetGauge(
      "sb_tx_locks_held",
      scratchbird::core::metrics::Labels({{"component", "transaction.mga.lock"}}),
      static_cast<double>(held_locks),
      "transaction_mga_lock");
  (void)scratchbird::core::metrics::SetGauge(
      "sb_tx_lock_waiters",
      scratchbird::core::metrics::Labels({{"component", "transaction.mga.lock"}}),
      static_cast<double>(waiters),
      "transaction_mga_lock");
}

bool OwnsLock(const LocalTransactionLockTable::LockRecord& record, LocalTransactionId owner) {
  return std::any_of(record.owners.begin(), record.owners.end(), [owner](LocalTransactionId held) {
    return held.value == owner.value;
  });
}

void AddOwner(LocalTransactionLockTable::LockRecord* record, LocalTransactionId owner) {
  if (!OwnsLock(*record, owner)) { record->owners.push_back(owner); }
}

void RemoveOwner(LocalTransactionLockTable::LockRecord* record, LocalTransactionId owner) {
  record->owners.erase(std::remove_if(record->owners.begin(),
                                      record->owners.end(),
                                      [owner](LocalTransactionId held) { return held.value == owner.value; }),
                       record->owners.end());
}

bool CompatibleWithExisting(const LocalTransactionLockTable::LockRecord& record,
                            LocalTransactionId requester,
                            TransactionLockMode requested_mode) {
  if (record.owners.empty()) { return true; }
  if (record.mode == TransactionLockMode::shared && requested_mode == TransactionLockMode::shared) { return true; }
  return record.owners.size() == 1 && record.owners.front().value == requester.value;
}

LocalTransactionId FirstBlockingOwner(const LocalTransactionLockTable::LockRecord& record,
                                      LocalTransactionId requester) {
  for (LocalTransactionId owner : record.owners) {
    if (owner.value != requester.value) { return owner; }
  }
  return record.owners.empty() ? LocalTransactionId{} : record.owners.front();
}

bool HasDeadlockPath(const std::map<std::string, LocalTransactionLockTable::LockRecord>& locks,
                     const std::map<u64, LocalTransactionLockTable::WaitingRecord>& waiting,
                     LocalTransactionId from,
                     LocalTransactionId target,
                     u64 depth = 0) {
  if (!from.valid() || !target.valid() || depth > locks.size() + 1) { return false; }
  if (from.value == target.value) { return true; }
  const auto wait = waiting.find(from.value);
  if (wait == waiting.end()) { return false; }
  const auto lock = locks.find(wait->second.request.resource_key);
  if (lock == locks.end()) { return false; }
  for (LocalTransactionId owner : lock->second.owners) {
    if (HasDeadlockPath(locks, waiting, owner, target, depth + 1)) { return true; }
  }
  return false;
}

const LocalTransactionLockTable::WaitingRecord* EarliestPriorWaiterForResource(
    const std::map<u64, LocalTransactionLockTable::WaitingRecord>& waiting,
    const std::string& resource_key,
    LocalTransactionId requester,
    u64 requester_wait_sequence) {
  const LocalTransactionLockTable::WaitingRecord* earliest = nullptr;
  for (const auto& [_, wait] : waiting) {
    (void)_;
    if (wait.request.resource_key != resource_key ||
        wait.request.requester.value == requester.value) {
      continue;
    }
    if (requester_wait_sequence != 0 &&
        wait.wait_sequence >= requester_wait_sequence) {
      continue;
    }
    if (earliest == nullptr || wait.wait_sequence < earliest->wait_sequence) {
      earliest = &wait;
    }
  }
  return earliest;
}

bool DeadlineExpired(const TransactionWaitPolicy& policy) {
  if (policy.no_wait || policy.timeout_millis == 0) { return true; }
  if (policy.wait_start_unix_epoch_millis == 0 || policy.now_unix_epoch_millis == 0) { return false; }
  if (policy.now_unix_epoch_millis <= policy.wait_start_unix_epoch_millis) { return false; }
  return policy.now_unix_epoch_millis - policy.wait_start_unix_epoch_millis >= policy.timeout_millis;
}

u64 WaitElapsedMillis(const TransactionWaitPolicy& policy) {
  if (policy.wait_start_unix_epoch_millis == 0 || policy.now_unix_epoch_millis == 0 ||
      policy.now_unix_epoch_millis <= policy.wait_start_unix_epoch_millis) {
    return 0;
  }
  return policy.now_unix_epoch_millis - policy.wait_start_unix_epoch_millis;
}

u64 RetryAfterMillis(const TransactionWaitPolicy& policy) {
  if (policy.timeout_millis == 0) { return 0; }
  const u64 elapsed = WaitElapsedMillis(policy);
  return elapsed >= policy.timeout_millis ? 0 : policy.timeout_millis - elapsed;
}

}  // namespace

void LocalTransactionLockTable::SetAdmissionPolicy(AdmissionPolicy policy) {
  std::lock_guard<std::mutex> lock(mutex_);
  admission_policy_ = policy;
}

TransactionLockResult LocalTransactionLockTable::Acquire(TransactionLockRequest request) {
  std::lock_guard<std::mutex> lock(mutex_);
  return AcquireInternal(std::move(request), 0);
}

TransactionLockResult LocalTransactionLockTable::AcquireInternal(TransactionLockRequest request,
                                                                 u64 preserved_wait_sequence) {
  if (!request.requester.valid() || request.resource_key.empty() || request.mode == TransactionLockMode::unknown ||
      !request.transaction_active) {
    auto result = LockError(TransactionLockDecision::invalid_request,
                            "SB-SNTXN-LOCK-INVALID-REQUEST",
                            "transaction.lock.invalid_request");
    PublishLockDecisionMetric(result.decision);
    return result;
  }
  if (admission_policy_.max_held_locks != 0 && locks_.size() >= admission_policy_.max_held_locks &&
      locks_.find(request.resource_key) == locks_.end()) {
    auto result = LockError(TransactionLockDecision::admission_refused,
                            "SB-SNTXN-LOCK-ADMISSION-REFUSED",
                            "transaction.lock.admission_refused",
                            "max_held_locks");
    PublishLockDecisionMetric(result.decision);
    return result;
  }
  auto found = locks_.find(request.resource_key);
  if (found == locks_.end()) {
    LockRecord record;
    record.mode = request.mode;
    record.owners.push_back(request.requester);
    locks_[request.resource_key] = std::move(record);
    waiting_.erase(request.requester.value);
    TransactionLockResult result;
    result.status = LockOkStatus();
    result.decision = TransactionLockDecision::granted;
    PublishLockDecisionMetric(result.decision);
    PublishLockGaugeCounts(held_lock_count_unlocked(), waiting_lock_count_unlocked());
    return result;
  }
  if (OwnsLock(found->second, request.requester) &&
      (found->second.mode == TransactionLockMode::exclusive ||
       found->second.mode == request.mode ||
       CompatibleWithExisting(found->second, request.requester, request.mode))) {
    if (request.mode == TransactionLockMode::exclusive) { found->second.mode = TransactionLockMode::exclusive; }
    waiting_.erase(request.requester.value);
    TransactionLockResult result;
    result.status = LockOkStatus();
    result.decision = TransactionLockDecision::already_owned;
    PublishLockDecisionMetric(result.decision);
    PublishLockGaugeCounts(held_lock_count_unlocked(), waiting_lock_count_unlocked());
    return result;
  }

  const WaitingRecord* prior_waiter =
      OwnsLock(found->second, request.requester)
          ? nullptr
          : EarliestPriorWaiterForResource(waiting_,
                                           request.resource_key,
                                           request.requester,
                                           preserved_wait_sequence);
  if (prior_waiter != nullptr) {
    const LocalTransactionId blocker = prior_waiter->request.requester;
    if (DeadlineExpired(request.wait_policy)) {
      TransactionLockResult result = LockError(TransactionLockDecision::timeout,
                                               "SB-SNTXN-LOCK-FAIRNESS-TIMEOUT",
                                               "transaction.lock.fairness_timeout",
                                               request.resource_key);
      result.blocking_transaction = blocker;
      result.wait_elapsed_millis = WaitElapsedMillis(request.wait_policy);
      PublishLockDecisionMetric(result.decision);
      return result;
    }
    if (!admission_policy_.accepting_new_waits ||
        (admission_policy_.max_waiters != 0 && waiting_.size() >= admission_policy_.max_waiters)) {
      auto result = LockError(TransactionLockDecision::admission_refused,
                              "SB-SNTXN-LOCK-ADMISSION-REFUSED",
                              "transaction.lock.admission_refused",
                              "fairness_wait_admission");
      result.blocking_transaction = blocker;
      PublishLockDecisionMetric(result.decision);
      return result;
    }
    const u64 wait_sequence =
        preserved_wait_sequence == 0 ? next_wait_sequence_++ : preserved_wait_sequence;
    const u64 retry_count =
        preserved_wait_sequence == 0 ? 0 : prior_waiter->retry_count + 1;
    waiting_[request.requester.value] = {request, blocker, wait_sequence, retry_count};
    TransactionLockResult result;
    result.status = LockErrorStatus();
    result.decision = TransactionLockDecision::wait_required;
    result.blocking_transaction = blocker;
    result.retry_after_millis = RetryAfterMillis(request.wait_policy);
    result.wait_elapsed_millis = WaitElapsedMillis(request.wait_policy);
    result.diagnostic = MakeTransactionLockDiagnostic(result.status,
                                                      "SB-SNTXN-LOCK-FAIRNESS-WAIT",
                                                      "transaction.lock.fairness_wait",
                                                      request.resource_key);
    PublishLockDecisionMetric(result.decision);
    PublishLockGaugeCounts(held_lock_count_unlocked(), waiting_lock_count_unlocked());
    return result;
  }

  if (CompatibleWithExisting(found->second, request.requester, request.mode)) {
    if (request.mode == TransactionLockMode::exclusive) { found->second.mode = TransactionLockMode::exclusive; }
    AddOwner(&found->second, request.requester);
    waiting_.erase(request.requester.value);
    TransactionLockResult result;
    result.status = LockOkStatus();
    result.decision = TransactionLockDecision::granted;
    PublishLockDecisionMetric(result.decision);
    PublishLockGaugeCounts(held_lock_count_unlocked(), waiting_lock_count_unlocked());
    return result;
  }

  const LocalTransactionId blocker = FirstBlockingOwner(found->second, request.requester);
  if (HasDeadlockPath(locks_, waiting_, blocker, request.requester)) {
    TransactionLockResult result = LockError(TransactionLockDecision::deadlock_detected,
                                             "SB-SNTXN-DEADLOCK-DETECTED",
                                             "transaction.lock.deadlock_detected",
                                             request.resource_key);
    result.blocking_transaction = blocker;
    PublishLockDecisionMetric(result.decision);
    return result;
  }

  if (DeadlineExpired(request.wait_policy)) {
    TransactionLockResult result = LockError(TransactionLockDecision::timeout,
                                             "SB-SNTXN-LOCK-TIMEOUT",
                                             "transaction.lock.timeout",
                                             request.resource_key);
    result.blocking_transaction = blocker;
    result.wait_elapsed_millis = WaitElapsedMillis(request.wait_policy);
    PublishLockDecisionMetric(result.decision);
    return result;
  }

  if (!admission_policy_.accepting_new_waits ||
      (admission_policy_.max_waiters != 0 && waiting_.size() >= admission_policy_.max_waiters)) {
    auto result = LockError(TransactionLockDecision::admission_refused,
                            "SB-SNTXN-LOCK-ADMISSION-REFUSED",
                            "transaction.lock.admission_refused",
                            "wait_admission");
    result.blocking_transaction = blocker;
    PublishLockDecisionMetric(result.decision);
    return result;
  }

  const u64 wait_sequence =
      preserved_wait_sequence == 0 ? next_wait_sequence_++ : preserved_wait_sequence;
  u64 retry_count = 0;
  if (preserved_wait_sequence != 0) {
    retry_count = 1;
  }
  waiting_[request.requester.value] = {request, blocker, wait_sequence, retry_count};

  TransactionLockResult result;
  result.status = LockErrorStatus();
  result.decision = TransactionLockDecision::wait_required;
  result.blocking_transaction = blocker;
  result.retry_after_millis = RetryAfterMillis(request.wait_policy);
  result.wait_elapsed_millis = WaitElapsedMillis(request.wait_policy);
  result.diagnostic = MakeTransactionLockDiagnostic(result.status,
                                                    "SB-SNTXN-LOCK-WAIT-REQUIRED",
                                                    "transaction.lock.wait_required",
                                                    request.resource_key);
  PublishLockDecisionMetric(result.decision);
  PublishLockGaugeCounts(held_lock_count_unlocked(), waiting_lock_count_unlocked());
  return result;
}

TransactionLockResult LocalTransactionLockTable::Retry(LocalTransactionId requester) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto wait = waiting_.find(requester.value);
  if (wait == waiting_.end()) {
    auto result = LockError(TransactionLockDecision::invalid_request,
                            "SB-SNTXN-LOCK-INVALID-RETRY",
                            "transaction.lock.invalid_retry");
    PublishLockDecisionMetric(result.decision);
    return result;
  }
  TransactionLockRequest request = wait->second.request;
  const u64 preserved_wait_sequence = wait->second.wait_sequence;
  waiting_.erase(wait);
  return AcquireInternal(std::move(request), preserved_wait_sequence);
}

TransactionLockResult LocalTransactionLockTable::Release(LocalTransactionId owner, std::string resource_key) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto found = locks_.find(resource_key);
  if (found == locks_.end() || !OwnsLock(found->second, owner)) {
    auto result = LockError(TransactionLockDecision::invalid_request,
                            "SB-SNTXN-LOCK-INVALID-RELEASE",
                            "transaction.lock.invalid_release",
                            resource_key);
    PublishLockDecisionMetric(result.decision);
    return result;
  }
  RemoveOwner(&found->second, owner);
  if (found->second.owners.empty()) { locks_.erase(found); }
  for (auto it = waiting_.begin(); it != waiting_.end();) {
    if (it->first == owner.value) {
      it = waiting_.erase(it);
    } else {
      ++it;
    }
  }
  TransactionLockResult result;
  result.status = LockOkStatus();
  result.decision = TransactionLockDecision::granted;
  PublishLockDecisionMetric(result.decision);
  PublishLockGaugeCounts(held_lock_count_unlocked(), waiting_lock_count_unlocked());
  return result;
}

u64 LocalTransactionLockTable::ReleaseAll(LocalTransactionId owner) {
  std::lock_guard<std::mutex> lock(mutex_);
  u64 released = 0;
  for (auto it = locks_.begin(); it != locks_.end();) {
    if (OwnsLock(it->second, owner)) {
      RemoveOwner(&it->second, owner);
      ++released;
    }
    if (it->second.owners.empty()) {
      it = locks_.erase(it);
    } else {
      ++it;
    }
  }
  waiting_.erase(owner.value);
  PublishLockGaugeCounts(held_lock_count_unlocked(), waiting_lock_count_unlocked());
  return released;
}

u64 LocalTransactionLockTable::held_lock_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return held_lock_count_unlocked();
}

u64 LocalTransactionLockTable::waiting_lock_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return waiting_lock_count_unlocked();
}

u64 LocalTransactionLockTable::held_lock_count_unlocked() const {
  return static_cast<u64>(locks_.size());
}

u64 LocalTransactionLockTable::waiting_lock_count_unlocked() const {
  return static_cast<u64>(waiting_.size());
}

const char* TransactionLockDecisionName(TransactionLockDecision decision) {
  switch (decision) {
    case TransactionLockDecision::granted: return "granted";
    case TransactionLockDecision::already_owned: return "already_owned";
    case TransactionLockDecision::wait_required: return "wait_required";
    case TransactionLockDecision::timeout: return "timeout";
    case TransactionLockDecision::deadlock_detected: return "deadlock_detected";
    case TransactionLockDecision::admission_refused: return "admission_refused";
    case TransactionLockDecision::invalid_request: return "invalid_request";
  }
  return "invalid_request";
}

DiagnosticRecord MakeTransactionLockDiagnostic(Status status,
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
                        "transaction.mga.lock");
}

}  // namespace scratchbird::transaction::mga
