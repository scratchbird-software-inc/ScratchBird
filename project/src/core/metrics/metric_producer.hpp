// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SEARCH_KEY: SB_METRICS_PRODUCER_API

#include "metric_registry.hpp"

#include <initializer_list>
#include <string>
#include <vector>

namespace scratchbird::core::metrics {

MetricLabelSet Labels(std::initializer_list<std::pair<std::string, std::string>> labels);

MetricValidationResult IncrementCounter(const std::string& family,
                                        MetricLabelSet labels,
                                        double delta,
                                        const std::string& producer_owner);
MetricValidationResult SetGauge(const std::string& family,
                                MetricLabelSet labels,
                                double value,
                                const std::string& producer_owner);
MetricValidationResult ObserveHistogram(const std::string& family,
                                        MetricLabelSet labels,
                                        double value,
                                        const std::string& producer_owner);
MetricValidationResult SetState(const std::string& family,
                                MetricLabelSet labels,
                                double value,
                                std::string state_text,
                                const std::string& producer_owner);
MetricValidationResult RejectSample(const std::string& family,
                                    const std::string& reason,
                                    const std::string& producer_owner);

}  // namespace scratchbird::core::metrics
