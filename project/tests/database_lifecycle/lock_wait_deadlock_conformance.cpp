// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "lock_wait_lifecycle.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace mga = scratchbird::transaction::mga;

constexpr const char* kDatabaseUuid = "019e13f0-0000-7000-8000-000000000001";
constexpr const char* kTableUuid = "019e13f0-0000-7000-8000-000000000002";
constexpr const char* kRecordA = "019e13f0-0000-7000-8000-000000000101";
constexpr const char* kRecordB = "019e13f0-0000-7000-8000-000000000102";
constexpr const char* kOwner1 = "019e13f0-0000-7000-8000-000000000201";
constexpr const char* kOwner2 = "019e13f0-0000-7000-8000-000000000202";
constexpr const char* kOwner3 = "019e13f0-0000-7000-8000-000000000203";
constexpr const char* kOwner4 = "019e13f0-0000-7000-8000-000000000204";
constexpr const char* kTx1 = "019e13f0-0000-7000-8000-000000000301";
constexpr const char* kTx2 = "019e13f0-0000-7000-8000-000000000302";
constexpr const char* kTx3 = "019e13f0-0000-7000-8000-000000000303";
constexpr const char* kTx4 = "019e13f0-0000-7000-8000-000000000304";

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

bool HasDiagnosticArgument(const scratchbird::core::platform::DiagnosticRecord& diagnostic,
                           std::string_view key,
                           std::string_view value) {
  for (const auto& arg : diagnostic.arguments) {
    if (arg.key == key && arg.value == value) { return true; }
  }
  return false;
}

mga::MGALockRequest Request(std::string request_id,
                            std::string owner_uuid,
                            std::string tx_uuid,
                            std::string record_uuid,
                            mga::MGALockMode mode,
                            mga::MGAWaitPolicy wait_policy,
                            std::uint64_t requested_at_millis = 1000,
                            std::uint64_t timeout_millis = 100,
                            std::uint64_t durable_work_completed = 10,
                            std::uint64_t transaction_age_millis = 100) {
  mga::MGALockRequest request;
  request.request_id = std::move(request_id);
  request.owner_kind = mga::MGALockOwnerKind::transaction;
  request.owner_uuid = std::move(owner_uuid);
  request.transaction_uuid = std::move(tx_uuid);
  request.scope_kind = mga::MGALockScopeKind::record_lineage;
  request.scope_uuid = record_uuid;
  request.record_uuid = std::move(record_uuid);
  request.table_uuid = kTableUuid;
  request.database_uuid = kDatabaseUuid;
  request.mode = mode;
  request.wait_policy = wait_policy;
  request.timeout_millis = timeout_millis;
  request.priority_class = mga::MGAPriorityClass::normal;
  request.policy_epoch = 1;
  request.requested_at_millis = requested_at_millis;
  request.durable_work_completed = durable_work_completed;
  request.transaction_age_millis = transaction_age_millis;
  return request;
}

void TestCompatibleAndIncompatibleLocks() {
  mga::MGALockWaitLifecycle lifecycle;
  const auto read1 = lifecycle.Acquire(Request("019e13f0-0000-7000-8000-000000001001",
                                               kOwner1,
                                               kTx1,
                                               kRecordA,
                                               mga::MGALockMode::shared_read,
                                               mga::MGAWaitPolicy::no_wait),
                                      1000);
  const auto read2 = lifecycle.Acquire(Request("019e13f0-0000-7000-8000-000000001002",
                                               kOwner2,
                                               kTx2,
                                               kRecordA,
                                               mga::MGALockMode::shared_read,
                                               mga::MGAWaitPolicy::no_wait),
                                      1000);
  const auto writer = lifecycle.Acquire(Request("019e13f0-0000-7000-8000-000000001003",
                                                kOwner3,
                                                kTx3,
                                                kRecordA,
                                                mga::MGALockMode::exclusive_write,
                                                mga::MGAWaitPolicy::wait_timeout),
                                       1000);
  Require(read1.decision == mga::MGALockLifecycleDecision::granted,
          "first compatible shared_read was not granted");
  Require(read2.decision == mga::MGALockLifecycleDecision::granted,
          "second compatible shared_read was not granted");
  Require(writer.decision == mga::MGALockLifecycleDecision::wait_queued &&
              writer.wait.blocked_by_grant_ids.size() == 2,
          "exclusive_write did not wait behind incompatible shared_read grants");
  Require(writer.diagnostic.diagnostic_code == "diag.mga.lock.write_intent_conflict",
          "wait diagnostic code mismatch");
}

void TestBoundedTimeoutAndCancel() {
  mga::MGALockWaitLifecycle lifecycle;
  const auto holder = lifecycle.Acquire(Request("019e13f0-0000-7000-8000-000000002001",
                                                kOwner1,
                                                kTx1,
                                                kRecordA,
                                                mga::MGALockMode::exclusive_write,
                                                mga::MGAWaitPolicy::no_wait),
                                       1000);
  const auto waiter = lifecycle.Acquire(Request("019e13f0-0000-7000-8000-000000002002",
                                                kOwner2,
                                                kTx2,
                                                kRecordA,
                                                mga::MGALockMode::exclusive_write,
                                                mga::MGAWaitPolicy::wait_timeout,
                                                1000,
                                                25),
                                       1000);
  const auto expired = lifecycle.ProcessTimeouts(1025);
  Require(holder.ok() && waiter.decision == mga::MGALockLifecycleDecision::wait_queued,
          "timeout fixture did not create holder and waiter");
  Require(expired.size() == 1 &&
              expired.front().decision == mga::MGALockLifecycleDecision::wait_timeout &&
              expired.front().diagnostic.diagnostic_code == "diag.mga.lock.wait_timeout" &&
              HasDiagnosticArgument(expired.front().diagnostic,
                                    "required_action",
                                    "return_timeout_transaction_state_unchanged"),
          "bounded wait timeout diagnostic mismatch");
  Require(lifecycle.wait_count() == 0 && lifecycle.grant_count() == 1,
          "timeout changed held grants or left expired wait queued");

  const auto waiter2 = lifecycle.Acquire(Request("019e13f0-0000-7000-8000-000000002003",
                                                 kOwner3,
                                                 kTx3,
                                                 kRecordA,
                                                 mga::MGALockMode::exclusive_write,
                                                 mga::MGAWaitPolicy::wait_timeout,
                                                 1030,
                                                 100),
                                        1030);
  const auto cancelled = lifecycle.CancelWait(waiter2.wait.wait_id, 1031);
  Require(cancelled.decision == mga::MGALockLifecycleDecision::wait_cancelled &&
              cancelled.diagnostic.diagnostic_code == "diag.mga.lock.admission_rejected_policy" &&
              lifecycle.wait_count() == 0 && lifecycle.grant_count() == 1,
          "cancel wait did not clean only the wait");
}

void TestDisconnectCleanupGrantsNextWaiter() {
  mga::MGALockWaitLifecycle lifecycle;
  const auto holder = lifecycle.Acquire(Request("019e13f0-0000-7000-8000-000000003001",
                                                kOwner1,
                                                kTx1,
                                                kRecordA,
                                                mga::MGALockMode::exclusive_write,
                                                mga::MGAWaitPolicy::no_wait),
                                       1000);
  const auto waiter = lifecycle.Acquire(Request("019e13f0-0000-7000-8000-000000003002",
                                                kOwner2,
                                                kTx2,
                                                kRecordA,
                                                mga::MGALockMode::exclusive_write,
                                                mga::MGAWaitPolicy::wait_timeout),
                                       1000);
  const auto cleanup = lifecycle.DisconnectOwner(kOwner1, 1005);
  Require(holder.ok() && waiter.decision == mga::MGALockLifecycleDecision::wait_queued,
          "disconnect fixture did not establish wait");
  Require(cleanup.decision == mga::MGALockLifecycleDecision::owner_disconnected &&
              cleanup.cleanup_count == 1 &&
              lifecycle.wait_count() == 0 &&
              lifecycle.grant_count() == 1 &&
              lifecycle.grants().front().owner_uuid == kOwner2,
          "disconnect cleanup did not release owner and grant queued waiter");
}

void TestDeadlockCycleVictimDiagnostics() {
  mga::MGALockWaitLifecycle lifecycle;
  const auto a = lifecycle.Acquire(Request("019e13f0-0000-7000-8000-000000004001",
                                           kOwner1,
                                           kTx1,
                                           kRecordA,
                                           mga::MGALockMode::exclusive_write,
                                           mga::MGAWaitPolicy::no_wait),
                                  1000);
  const auto b = lifecycle.Acquire(Request("019e13f0-0000-7000-8000-000000004002",
                                           kOwner2,
                                           kTx2,
                                           kRecordB,
                                           mga::MGALockMode::exclusive_write,
                                           mga::MGAWaitPolicy::no_wait),
                                  1000);
  auto wait_a = Request("019e13f0-0000-7000-8000-000000004003",
                        kOwner1,
                        kTx1,
                        kRecordB,
                        mga::MGALockMode::exclusive_write,
                        mga::MGAWaitPolicy::wait_with_deadlock_detection,
                        1010,
                        100,
                        50,
                        500);
  auto wait_b = Request("019e13f0-0000-7000-8000-000000004004",
                        kOwner2,
                        kTx2,
                        kRecordA,
                        mga::MGALockMode::exclusive_write,
                        mga::MGAWaitPolicy::wait_with_deadlock_detection,
                        1010,
                        100,
                        5,
                        100);
  const auto queued_a = lifecycle.Acquire(wait_a, 1010);
  const auto victim = lifecycle.Acquire(wait_b, 1010);
  Require(a.ok() && b.ok() && queued_a.decision == mga::MGALockLifecycleDecision::wait_queued,
          "deadlock fixture did not establish first wait edge");
  Require(victim.decision == mga::MGALockLifecycleDecision::deadlock_victim &&
              victim.victim_owner_uuid == kOwner2 &&
              victim.diagnostic.diagnostic_code == "diag.mga.lock.deadlock_victim" &&
              lifecycle.deadlock_history().size() == 1 &&
              lifecycle.deadlock_history().front().diagnostic.diagnostic_code ==
                  "diag.mga.lock.deadlock_detected",
          "deadlock victim selection or diagnostic mismatch");
  Require(lifecycle.wait_count() == 1 && lifecycle.grant_count() == 2,
          "deadlock victim handling changed unrelated grants or waits");
}

void TestShutdownCleanupAndNoClusterPath() {
  mga::MGALockWaitLifecycle lifecycle;
  const auto grant = lifecycle.Acquire(Request("019e13f0-0000-7000-8000-000000005001",
                                               kOwner1,
                                               kTx1,
                                               kRecordA,
                                               mga::MGALockMode::exclusive_write,
                                               mga::MGAWaitPolicy::no_wait),
                                      1000);
  const auto wait = lifecycle.Acquire(Request("019e13f0-0000-7000-8000-000000005002",
                                              kOwner2,
                                              kTx2,
                                              kRecordA,
                                              mga::MGALockMode::exclusive_write,
                                              mga::MGAWaitPolicy::wait_timeout),
                                     1000);
  const auto shutdown = lifecycle.Shutdown(1010);
  Require(grant.ok() && wait.decision == mga::MGALockLifecycleDecision::wait_queued,
          "shutdown fixture did not establish grant and wait");
  Require(shutdown.decision == mga::MGALockLifecycleDecision::shutdown_cleanup &&
              shutdown.cleanup_count == 2 &&
              lifecycle.grant_count() == 0 &&
              lifecycle.wait_count() == 0,
          "shutdown cleanup did not clear volatile lock state");

  mga::MGALockWaitLifecycle standalone;
  auto cluster = Request("019e13f0-0000-7000-8000-000000005003",
                         kOwner4,
                         kTx4,
                         kRecordA,
                         mga::MGALockMode::cluster_prepare,
                         mga::MGAWaitPolicy::wait_timeout);
  cluster.scope_kind = mga::MGALockScopeKind::cluster_transaction_identifier;
  cluster.scope_uuid = "019e13f0-0000-7000-8000-000000000401";
  cluster.record_uuid = "none";
  cluster.table_uuid = "none";
  cluster.cluster_uuid = "none";
  cluster.cluster_exists = false;
  const auto rejected = standalone.Acquire(cluster, 1000);
  Require(rejected.decision == mga::MGALockLifecycleDecision::cluster_absent &&
              rejected.diagnostic.diagnostic_code == "diag.mga.lock.cluster_absent" &&
              rejected.diagnostic.status.subsystem ==
                  scratchbird::core::platform::Subsystem::transaction_mga &&
              standalone.grant_count() == 0 &&
              standalone.wait_count() == 0 &&
              standalone.metrics().cluster_state_records == 0,
          "standalone cluster lock request did not fail closed without cluster state");
}

}  // namespace

int main() {
  TestCompatibleAndIncompatibleLocks();
  TestBoundedTimeoutAndCancel();
  TestDisconnectCleanupGrantsNextWaiter();
  TestDeadlockCycleVictimDiagnostics();
  TestShutdownCleanupAndNoClusterPath();
  std::cout << "lock_wait_deadlock_conformance=passed\n";
  return EXIT_SUCCESS;
}
