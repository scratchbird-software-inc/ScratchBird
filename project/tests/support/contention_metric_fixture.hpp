// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "metric_projection_fixture.hpp"
#include "metric_contracts.hpp"

namespace scratchbird::tests {
inline core::metrics::MetricLabelSet ContentionMetricLabels(
    const core::metrics::LockLatchContentionSample& sample) {
  return {{"component", "runtime.contention"}, {"subsystem", sample.subsystem},
          {"wait_class", sample.wait_class}, {"evidence_surface", sample.evidence_surface}};
}

// Explicit component admission only; never installs runtime/catalog authority
// or manufactures the observations that the real producer must publish.
inline void AdmitContentionMetricFixture(MetricProjectionFixture& fixture,
    const core::metrics::LockLatchContentionSample& sample) {
  for (const auto* family : {"sb_lock_latch_contention_wait_total",
                             "sb_lock_latch_contention_wait_microseconds"})
    fixture.Admit(family, ContentionMetricLabels(sample));
}
}  // namespace scratchbird::tests
