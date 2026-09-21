// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_record_codec.hpp"
#include "catalog_value_codec.hpp"
#include "metric_history.hpp"
#include <optional>
#include <string_view>

namespace scratchbird::core::catalog {
struct CatalogMetricSeriesLabel {
  std::string key;
  metrics::MetricLabelType type = metrics::MetricLabelType::text;
  metrics::MetricLabelValue value;
  bool operator==(const CatalogMetricSeriesLabel&) const = default;
};
struct CatalogMetricSeries {
  Uuid series_uuid;
  u64 generation = 0;
  metrics::MetricHistoryBinding binding;
  std::vector<CatalogMetricSeriesLabel> labels;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id = 0;
};
struct CatalogMetricSeriesResult {
  CatalogValueError error = CatalogValueError::invalid_value;
  std::optional<CatalogMetricSeries> record;
  bool ok() const { return error == CatalogValueError::none && record.has_value(); }
};
const CatalogValueSchema& CatalogMetricSeriesSchema();
CatalogValueEncodeResult EncodeCatalogMetricSeries(const CatalogMetricSeries&);
CatalogMetricSeriesResult DecodeCatalogMetricSeries(std::string_view);
bool IsCatalogMetricSeriesPayload(std::string_view);
bool CatalogMetricSeriesMatchesHeader(const CatalogTypedRecord&);
bool CatalogMetricSeriesMatchesMetadata(const CatalogMetadataVersion&);
bool CatalogMetricSeriesPreservesOrigin(const CatalogMetadataVersion&, const CatalogMetadataVersion&);
// Structural construction from explicitly selected definitions. Never looks
// up a name, issues an identity or grants live activation/security/finality.
metrics::MetricHistoryRecordResult<metrics::MetricSeriesIdentity> BindCatalogMetricSeries(
    const CatalogMetricSeries&, const metrics::MetricDescriptor&, const metrics::MetricRetentionPolicy&);
}  // namespace scratchbird::core::catalog
