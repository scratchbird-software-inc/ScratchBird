// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "metric_page_cache_definitions.hpp"

namespace scratchbird::core::metrics {
std::vector<MetricDescriptorDefinition> PageCacheContextMetricDefinitions() {
  struct Row { const char* family; MetricType type; MetricUnit unit; const char* help; };
  static constexpr Row rows[] = {
      {"sb_page_cache_context_resident_pages", MetricType::gauge, MetricUnit::pages,
       "Resident page-cache pages by IO context."},
      {"sb_page_cache_context_resident_bytes", MetricType::gauge, MetricUnit::bytes,
       "Resident page-cache bytes by IO context."},
      {"sb_page_cache_context_pinned_pages", MetricType::gauge, MetricUnit::pages,
       "Pinned page-cache pages by IO context."},
      {"sb_page_cache_context_dirty_pages", MetricType::gauge, MetricUnit::pages,
       "Dirty page-cache pages by IO context."},
      {"sb_page_cache_context_admissions_total", MetricType::counter, MetricUnit::events,
       "Page-cache admissions by IO context."},
      {"sb_page_cache_context_reuses_total", MetricType::counter, MetricUnit::events,
       "Page-cache ring or slot reuses by IO context."},
      {"sb_page_cache_context_evictions_total", MetricType::counter, MetricUnit::events,
       "Page-cache evictions by owning IO context."},
      {"sb_page_cache_context_protected_normal_hot_skips_total", MetricType::counter, MetricUnit::events,
       "Normal hot page eviction skips caused by scan-resistant IO contexts."},
      {"sb_page_cache_context_refusals_total", MetricType::counter, MetricUnit::events,
       "Page-cache scan-lane admissions refused by bounded ring or budget pressure."},
  };
  std::vector<MetricDescriptorDefinition> definitions;
  definitions.reserve(std::size(rows));
  for (const auto& row : rows) {
    MetricDescriptorDefinition definition;
    definition.family = row.family;
    definition.type = row.type;
    definition.unit = row.unit;
    definition.value_type = MetricScalarType::uint64;
    definition.namespace_path = "sys.metrics.storage.pages.cache";
    definition.help = row.help;
    definition.producer_owner = "storage_page";
    definition.security_family = "OBS_METRICS_READ_FAMILY";
    definition.visibility = MetricVisibilityScope::family;
    definition.labels = {{"component", true},
        {"database_uuid", true, false, MetricLabelType::system_uuid},
        {"filespace_uuid", true, false, MetricLabelType::system_uuid},
        {"page_family", true}, {"context", true}, {"result", true}, {"reason", true}};
    definitions.push_back(std::move(definition));
  }
  return definitions;
}
}  // namespace scratchbird::core::metrics
