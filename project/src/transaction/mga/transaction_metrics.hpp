// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-TXN-MEMORY-METRICS-ANCHOR
#include "runtime_platform.hpp"

#include <string>
#include <vector>

namespace scratchbird::transaction::mga {

using scratchbird::core::platform::u64;

struct TransactionMetricsSnapshot {
  u64 begin_total = 0;
  u64 commit_total = 0;
  u64 rollback_total = 0;
  u64 abort_total = 0;
  u64 active_current = 0;
  u64 failure_total = 0;
  u64 cluster_fail_closed_total = 0;
};

struct TransactionMetricSample {
  std::string name;
  u64 value = 0;
};

class TransactionMetrics {
 public:
  void RecordBegin();
  void RecordCommit();
  void RecordRollback();
  void RecordAbort();
  void RecordFailure();
  void RecordClusterFailClosed();
  TransactionMetricsSnapshot Snapshot() const;
  std::vector<TransactionMetricSample> MetricSamples() const;

 private:
  TransactionMetricsSnapshot snapshot_;
};

}  // namespace scratchbird::transaction::mga
