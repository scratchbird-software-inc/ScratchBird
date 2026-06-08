// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "transaction_metrics.hpp"

#include "metric_producer.hpp"

namespace scratchbird::transaction::mga {
namespace {

void Counter(const char* family, const char* operation) {
  (void)scratchbird::core::metrics::IncrementCounter(
      family,
      scratchbird::core::metrics::Labels({{"component", "transaction.mga"}, {"operation", operation}, {"result", "ok"}}),
      1.0,
      "transaction_mga");
}

void ActiveGauge(u64 value) {
  (void)scratchbird::core::metrics::SetGauge(
      "sb_tx_active_transactions",
      scratchbird::core::metrics::Labels({{"component", "transaction.mga"}}),
      static_cast<double>(value),
      "transaction_mga");
}

}  // namespace

void TransactionMetrics::RecordBegin() {
  ++snapshot_.begin_total;
  ++snapshot_.active_current;
  Counter("sb_tx_begin_total", "begin");
  ActiveGauge(snapshot_.active_current);
}

void TransactionMetrics::RecordCommit() {
  ++snapshot_.commit_total;
  if (snapshot_.active_current != 0) {
    --snapshot_.active_current;
  }
  Counter("sb_tx_commit_total", "commit");
  ActiveGauge(snapshot_.active_current);
}

void TransactionMetrics::RecordRollback() {
  ++snapshot_.rollback_total;
  if (snapshot_.active_current != 0) {
    --snapshot_.active_current;
  }
  Counter("sb_tx_rollback_total", "rollback");
  ActiveGauge(snapshot_.active_current);
}

void TransactionMetrics::RecordAbort() {
  ++snapshot_.abort_total;
  if (snapshot_.active_current != 0) {
    --snapshot_.active_current;
  }
  Counter("sb_tx_abort_total", "abort");
  ActiveGauge(snapshot_.active_current);
}

void TransactionMetrics::RecordFailure() {
  ++snapshot_.failure_total;
  Counter("sb_tx_failure_total", "failure");
}

void TransactionMetrics::RecordClusterFailClosed() {
  ++snapshot_.cluster_fail_closed_total;
  Counter("sb_tx_cluster_fail_closed_total", "cluster_fail_closed");
}

TransactionMetricsSnapshot TransactionMetrics::Snapshot() const {
  return snapshot_;
}

std::vector<TransactionMetricSample> TransactionMetrics::MetricSamples() const {
  return {
      {"sb_tx_begin_total", snapshot_.begin_total},
      {"sb_tx_commit_total", snapshot_.commit_total},
      {"sb_tx_rollback_total", snapshot_.rollback_total},
      {"sb_tx_abort_total", snapshot_.abort_total},
      {"sb_tx_active_transactions", snapshot_.active_current},
      {"sb_tx_failure_total", snapshot_.failure_total},
      {"sb_tx_cluster_fail_closed_total", snapshot_.cluster_fail_closed_total},
  };
}

}  // namespace scratchbird::transaction::mga
