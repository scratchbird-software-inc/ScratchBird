// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "transaction_policy.hpp"

#include "metric_producer.hpp"

#include <utility>
#include <vector>

namespace scratchbird::transaction::mga {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status PolicyOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::transaction_mga};
}

Status PolicyErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::transaction_mga};
}

void RecordPolicyViolation(const char* policy_name, const TransactionInventoryEntry& entry) {
  (void)scratchbird::core::metrics::IncrementCounter(
      "sb_tx_runtime_policy_violation_total",
      scratchbird::core::metrics::Labels({{"component", "transaction.mga"},
                                          {"policy", policy_name},
                                          {"local_transaction_id", std::to_string(entry.identity.local_id.value)}}),
      1.0,
      "transaction_mga");
}

}  // namespace

TransactionRuntimePolicy DefaultLocalTransactionRuntimePolicy() {
  TransactionRuntimePolicy policy;
  policy.max_active_millis = 24ull * 60ull * 60ull * 1000ull;
  policy.max_idle_millis = 60ull * 60ull * 1000ull;
  policy.fail_closed_on_violation = true;
  return policy;
}

TransactionPolicyResult EvaluateTransactionRuntimePolicy(const TransactionInventoryEntry& entry,
                                                         TransactionRuntimePolicy policy,
                                                         u64 now_unix_epoch_millis,
                                                         u64 last_activity_unix_epoch_millis) {
  TransactionPolicyResult result;
  result.status = PolicyOkStatus();
  if (policy.max_active_millis != 0 && now_unix_epoch_millis > entry.begin_unix_epoch_millis &&
      now_unix_epoch_millis - entry.begin_unix_epoch_millis > policy.max_active_millis) {
    result.status = PolicyErrorStatus();
    result.allowed = false;
    RecordPolicyViolation("max_active_millis", entry);
    result.diagnostic = MakeTransactionPolicyDiagnostic(result.status,
                                                        "SB-SNTXN-LONG-RUNNING-POLICY-VIOLATION",
                                                        "transaction.policy.long_running_violation",
                                                        std::to_string(entry.identity.local_id.value));
    return result;
  }
  if (policy.max_idle_millis != 0 && now_unix_epoch_millis > last_activity_unix_epoch_millis &&
      now_unix_epoch_millis - last_activity_unix_epoch_millis > policy.max_idle_millis) {
    result.status = PolicyErrorStatus();
    result.allowed = false;
    RecordPolicyViolation("max_idle_millis", entry);
    result.diagnostic = MakeTransactionPolicyDiagnostic(result.status,
                                                        "SB-SNTXN-IDLE-POLICY-VIOLATION",
                                                        "transaction.policy.idle_violation",
                                                        std::to_string(entry.identity.local_id.value));
  }
  return result;
}

DiagnosticRecord MakeTransactionPolicyDiagnostic(Status status,
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
                        "transaction.mga.policy");
}

}  // namespace scratchbird::transaction::mga
