// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "metric_contracts.hpp"
#include "metric_export.hpp"
#include "metric_registry.hpp"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace metrics = scratchbird::core::metrics;

// SEARCH_KEY: ODFR_CONTENTION_TELEMETRY_GATE

namespace {

bool Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
}

bool HasLabel(const metrics::MetricLabelSet& labels,
              const std::string& key,
              const std::string& value) {
  return std::find_if(labels.begin(), labels.end(), [&](const metrics::MetricLabel& label) {
    return label.key == key && label.value == value;
  }) != labels.end();
}

bool HasCurrentValue(const std::vector<metrics::MetricValue>& values,
                     const std::string& family,
                     const std::string& wait_class,
                     const std::string& evidence_surface) {
  return std::find_if(values.begin(), values.end(), [&](const metrics::MetricValue& value) {
    return value.family == family && HasLabel(value.labels, "wait_class", wait_class) &&
           HasLabel(value.labels, "evidence_surface", evidence_surface);
  }) != values.end();
}

bool ContainsText(const std::string& text, const std::string& expected) {
  return text.find(expected) != std::string::npos;
}

bool RequiredWaitClassesAreStable() {
  const auto required = metrics::LockLatchContentionRequiredWaitClasses();
  const std::vector<std::string> expected = {
      "page_latch",
      "index_leaf_split",
      "index_rightmost_leaf",
      "buffer_pin",
      "descriptor_cache_lock",
      "delta_ledger_merge",
      "background_agent_interference",
      "ipc_queue"};
  for (const auto& wait_class : expected) {
    if (!Require(std::find(required.begin(), required.end(), wait_class) != required.end(),
                 "missing wait class: " + wait_class)) {
      return false;
    }
  }
  return Require(required.size() == expected.size(), "unexpected contention wait class added");
}

bool RecordsBenchmarkAndSupportBundleSurfaces() {
  const std::vector<metrics::LockLatchContentionSample> samples = {
      {3, 30, "storage.page", "page_latch", "benchmark"},
      {2, 25, "index.btree", "index_leaf_split", "benchmark"},
      {1, 12, "index.btree", "index_rightmost_leaf", "support_bundle"},
      {5, 40, "buffer.manager", "buffer_pin", "support_bundle"},
      {4, 36, "catalog.descriptor_cache", "descriptor_cache_lock", "benchmark"},
      {2, 18, "index.delta_ledger", "delta_ledger_merge", "support_bundle"},
      {1, 9, "agents.background", "background_agent_interference", "benchmark"},
      {7, 77, "ipc.queue", "ipc_queue", "support_bundle"}};

  for (const auto& sample : samples) {
    const auto status = metrics::RecordLockLatchContentionWait(sample);
    if (!Require(status.ok, "contention sample rejected: " + status.diagnostic_code + ":" +
                                status.detail)) {
      return false;
    }
  }

  const auto current = metrics::DefaultMetricRegistry().SnapshotCurrent();
  for (const auto& sample : samples) {
    if (!Require(HasCurrentValue(current,
                                 "sb_lock_latch_contention_wait_total",
                                 sample.wait_class,
                                 sample.evidence_surface),
                 "missing wait count surface for " + sample.wait_class) ||
        !Require(HasCurrentValue(current,
                                 "sb_lock_latch_contention_wait_microseconds",
                                 sample.wait_class,
                                 sample.evidence_surface),
                 "missing wait timing surface for " + sample.wait_class)) {
      return false;
    }
  }

  const auto exported = metrics::ExportOpenMetrics(metrics::DefaultMetricRegistry(), true);
  return Require(ContainsText(exported, "sb_lock_latch_contention_wait_total"),
                 "OpenMetrics export missing contention wait count family") &&
         Require(ContainsText(exported, "sb_lock_latch_contention_wait_microseconds"),
                 "OpenMetrics export missing contention wait timing family") &&
         Require(ContainsText(exported, "wait_class=\"page_latch\""),
                 "OpenMetrics export missing page latch wait class") &&
         Require(ContainsText(exported, "wait_class=\"ipc_queue\""),
                 "OpenMetrics export missing IPC queue wait class") &&
         Require(ContainsText(exported, "evidence_surface=\"support_bundle\""),
                 "OpenMetrics export missing support-bundle evidence surface") &&
         Require(ContainsText(exported, "evidence_surface=\"benchmark\""),
                 "OpenMetrics export missing benchmark evidence surface");
}

bool RejectsInvalidContentionLabels() {
  metrics::LockLatchContentionSample sample;
  sample.wait_count = 1;
  sample.wait_microseconds = 1;
  sample.subsystem = "parser.finality";
  sample.wait_class = "parser_finality_wait";
  sample.evidence_surface = "benchmark";
  const auto status = metrics::RecordLockLatchContentionWait(sample);
  return Require(!status.ok, "unknown wait class was accepted") &&
         Require(status.diagnostic_code == "SB-METRICS-CONTRACT-WAIT-CLASS-UNKNOWN",
                 "unknown wait class diagnostic mismatch");
}

}  // namespace

int main() {
  if (!RequiredWaitClassesAreStable()) return 1;
  if (!RecordsBenchmarkAndSupportBundleSurfaces()) return 1;
  if (!RejectsInvalidContentionLabels()) return 1;
  return 0;
}
