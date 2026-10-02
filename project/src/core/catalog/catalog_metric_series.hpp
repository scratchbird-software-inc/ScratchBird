// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_record_codec.hpp"
#include "catalog_value_codec.hpp"
#include "metric_history.hpp"
#include <optional>
#include <iterator>
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
struct CatalogMetricSeriesLabelView {
  std::string_view key;
  metrics::MetricLabelType type = metrics::MetricLabelType::text;
  std::variant<std::string_view,Uuid> value;
  bool operator==(const CatalogMetricSeriesLabelView&) const = default;
};
struct CatalogMetricSeriesViewResult;
// Only complete decode constructs a nonempty sequence. Retain the immutable
// input owner and its actual memory grant during every iterator/view use.
class CatalogMetricSeriesLabelsView {
 public:
  class Iterator {
   public:
    using value_type = CatalogMetricSeriesLabelView;
    using difference_type = std::ptrdiff_t;
    using iterator_category = std::forward_iterator_tag;
    Iterator() = default;
    value_type operator*() const;
    Iterator& operator++();
    Iterator operator++(int) { auto old=*this; ++*this; return old; }
    bool operator==(const Iterator&) const = default;
   private:
    friend class CatalogMetricSeriesLabelsView;
    Iterator(const byte* keys,const byte* types,const byte* values)
        : keys_(keys),types_(types),values_(values) {}
    const byte* keys_=nullptr;
    const byte* types_=nullptr;
    const byte* values_=nullptr;
  };
  CatalogMetricSeriesLabelsView() = default;
  std::size_t size() const { return types_.size(); }
  bool empty() const { return types_.empty(); }
  Iterator begin() const { return {keys_.data(),types_.data(),values_.data()}; }
  Iterator end() const {
    return {keys_.empty()?keys_.data():keys_.data()+keys_.size(),
        types_.empty()?types_.data():types_.data()+types_.size(),
        values_.empty()?values_.data():values_.data()+values_.size()};
  }
  bool operator==(const CatalogMetricSeriesLabelsView&) const;
 private:
  friend CatalogMetricSeriesViewResult DecodeCatalogMetricSeriesView(std::string_view);
  CatalogMetricSeriesLabelsView(std::span<const byte> keys,std::span<const byte> types,
      std::span<const byte> values) : keys_(keys),types_(types),values_(values) {}
  std::span<const byte> keys_,types_,values_;
};
struct CatalogMetricSeriesView {
  Uuid series_uuid;
  u64 generation = 0;
  metrics::MetricHistoryBinding binding;
  CatalogMetricSeriesLabelsView labels;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id = 0;
};
struct CatalogMetricSeriesViewResult {
  CatalogValueError error = CatalogValueError::invalid_value;
  std::optional<CatalogMetricSeriesView> record;
  bool ok() const { return error == CatalogValueError::none && record.has_value(); }
};
CatalogMetricSeriesViewResult DecodeCatalogMetricSeriesView(std::string_view);
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
