// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "metric_producer.hpp"

#include <utility>

namespace scratchbird::core::metrics {

MetricLabelSet Labels(std::initializer_list<std::pair<std::string, std::string>> labels) {
  MetricLabelSet out;
  for (const auto& label : labels) {
    if (!label.first.empty() && !label.second.empty()) {
      out.push_back({label.first, label.second});
    }
  }
  return out;
}

MetricValidationResult IncrementCounter(const std::string& family,
                                        MetricLabelSet labels,
                                        double delta,
                                        const std::string& producer_owner) {
  return DefaultMetricRegistry().IncrementCounter(family, std::move(labels), delta, producer_owner);
}

MetricValidationResult SetGauge(const std::string& family,
                                MetricLabelSet labels,
                                double value,
                                const std::string& producer_owner) {
  return DefaultMetricRegistry().SetGauge(family, std::move(labels), value, producer_owner);
}

MetricValidationResult ObserveHistogram(const std::string& family,
                                        MetricLabelSet labels,
                                        double value,
                                        const std::string& producer_owner) {
  return DefaultMetricRegistry().ObserveHistogram(family, std::move(labels), value, producer_owner);
}

MetricValidationResult SetState(const std::string& family,
                                MetricLabelSet labels,
                                double value,
                                std::string state_text,
                                const std::string& producer_owner) {
  return DefaultMetricRegistry().SetState(family, std::move(labels), value, std::move(state_text), producer_owner);
}

MetricValidationResult RejectSample(const std::string& family,
                                    const std::string& reason,
                                    const std::string& producer_owner) {
  (void)producer_owner;
  return DefaultMetricRegistry().IncrementCounter("sb_metric_samples_rejected_total",
                                                  Labels({{"metric_family", family}, {"reason", reason}}),
                                                  1.0,
                                                  "metrics_runtime");
}

}  // namespace scratchbird::core::metrics
