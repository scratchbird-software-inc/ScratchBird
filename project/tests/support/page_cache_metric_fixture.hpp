// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "metric_projection_fixture.hpp"

namespace scratchbird::tests {
// Select the exact finite component series before executing real cache work.
// This fixture admits context telemetry only, not global cache gauges or a
// runtime bootstrap. No measurement is emitted by this admission helper.
inline void AdmitPageCacheContextFixture(MetricProjectionFixture& fixture,
    const core::metrics::MetricUuid& database, const core::metrics::MetricUuid& filespace) {
  for (const auto* context : {"normal", "index_read", "bulk_read", "bulk_write",
                              "vacuum_cleanup", "index_build", "strict_bulk_load"}) {
    const auto admit = [&](const char* family, const char* result, const char* reason) {
      fixture.Admit(family, {{"component", "storage.page_cache"},
          {"database_uuid", database}, {"filespace_uuid", filespace}, {"page_family", "all"},
          {"context", context}, {"result", result}, {"reason", reason}});
    };
    for (const auto* family : {"sb_page_cache_context_resident_pages", "sb_page_cache_context_resident_bytes",
                               "sb_page_cache_context_pinned_pages", "sb_page_cache_context_dirty_pages"})
      admit(family, "current", "snapshot");
    admit("sb_page_cache_context_admissions_total", "ok", "admit");
    admit("sb_page_cache_context_reuses_total", "ok", "ring_or_slot_reuse");
    admit("sb_page_cache_context_evictions_total", "evicted", "budget_or_ring");
    admit("sb_page_cache_context_protected_normal_hot_skips_total", "protected", "bulk_context_budget");
    for (const auto* reason : {"normal_hot_protected", "ring_pinned_or_dirty", "budget_pinned_or_dirty"})
      admit("sb_page_cache_context_refusals_total", "refused", reason);
  }
}
}  // namespace scratchbird::tests
