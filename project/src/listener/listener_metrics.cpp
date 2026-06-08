// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "listener_metrics.hpp"

#include <sstream>

#include "listener_diagnostics.hpp"

namespace scratchbird::listener {

void ListenerMetrics::Increment(std::string name, std::uint64_t delta) {
  std::lock_guard lock(mutex_);
  counters_[std::move(name)] += delta;
}

void ListenerMetrics::SetGauge(std::string name, double value) {
  std::lock_guard lock(mutex_);
  gauges_[std::move(name)] = value;
}

void ListenerMetrics::RecordAgentRuntimeEvidence(std::string result_state,
                                                 std::string diagnostic_code) {
  std::lock_guard lock(mutex_);
  counters_["agent_runtime_evidence_total"] += 1;
  counters_["agent_runtime_result_" + std::move(result_state)] += 1;
  if (!diagnostic_code.empty()) {
    counters_["agent_runtime_diagnostic_total"] += 1;
  }
}

ListenerMetricSnapshot ListenerMetrics::Snapshot() const {
  std::lock_guard lock(mutex_);
  return {counters_, gauges_};
}

std::string ListenerMetrics::ToJson() const {
  auto snapshot = Snapshot();
  std::ostringstream out;
  out << "{\"counters\":{";
  bool first = true;
  for (const auto& [name, value] : snapshot.counters) {
    if (!first) out << ',';
    first = false;
    out << '"' << QuoteJson(name) << "\":" << value;
  }
  out << "},\"gauges\":{";
  first = true;
  for (const auto& [name, value] : snapshot.gauges) {
    if (!first) out << ',';
    first = false;
    out << '"' << QuoteJson(name) << "\":" << value;
  }
  out << "}}";
  return out.str();
}

} // namespace scratchbird::listener
