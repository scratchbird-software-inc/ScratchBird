// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace scratchbird::listener {

struct ListenerMetricSnapshot {
  std::map<std::string, std::uint64_t> counters;
  std::map<std::string, double> gauges;
};

class ListenerMetrics {
 public:
  void Increment(std::string name, std::uint64_t delta = 1);
  void SetGauge(std::string name, double value);
  void RecordAgentRuntimeEvidence(std::string result_state, std::string diagnostic_code);
  ListenerMetricSnapshot Snapshot() const;
  std::string ToJson() const;

 private:
  mutable std::mutex mutex_;
  std::map<std::string, std::uint64_t> counters_;
  std::map<std::string, double> gauges_;
};

} // namespace scratchbird::listener
