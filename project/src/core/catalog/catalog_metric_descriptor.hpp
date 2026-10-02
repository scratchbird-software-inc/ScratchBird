// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_record_codec.hpp"
#include "catalog_value_codec.hpp"
#include "metric_registry.hpp"
#include "catalog_metric_label_schema.hpp"
#include <optional>
#include <string_view>

namespace scratchbird::core::catalog {
// Durable definition only. Live policy, producer and schema activation is not
// inferred from valid bytes and no runtime readiness is serialized here.
struct CatalogMetricDescriptor {
  metrics::MetricDescriptorDefinition definition;
  metrics::MetricDescriptorBinding binding;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id = 0;
};
struct CatalogMetricDescriptorResult {
  CatalogValueError error = CatalogValueError::invalid_value;
  std::optional<CatalogMetricDescriptor> record;
  bool ok() const { return error == CatalogValueError::none && record.has_value(); }
};
struct CatalogMetricDescriptorViewResult;
// Nonempty sequences can only be constructed by the complete descriptor
// decoder. Every view borrows immutable input, never another view's storage.
// Keep the input owner and its actual memory grant alive through all access.
class CatalogMetricNumericSequenceView {
 public:
  CatalogMetricNumericSequenceView() = default;
  std::size_t size() const { return count_; }
  bool empty() const { return count_ == 0; }
  metrics::MetricScalar operator[](std::size_t index) const;
 private:
  friend CatalogMetricDescriptorViewResult DecodeCatalogMetricDescriptorView(std::string_view);
  CatalogMetricNumericSequenceView(std::span<const byte> bytes,
      metrics::MetricScalarType type, std::size_t count) : bytes_(bytes), type_(type), count_(count) {}
  std::span<const byte> bytes_;
  metrics::MetricScalarType type_ = metrics::MetricScalarType::invalid;
  std::size_t count_ = 0;
};
class CatalogMetricEnumSequenceView {
 public:
  CatalogMetricEnumSequenceView() = default;
  std::size_t size() const { return bytes_.size()/8; }
  bool empty() const { return bytes_.empty(); }
  u64 operator[](std::size_t index) const { return platform::LoadLittle64(bytes_.data()+8*index); }
 private:
  friend CatalogMetricDescriptorViewResult DecodeCatalogMetricDescriptorView(std::string_view);
  explicit CatalogMetricEnumSequenceView(std::span<const byte> bytes) : bytes_(bytes) {}
  std::span<const byte> bytes_;
};
class CatalogMetricAliasSequenceView {
 public:
  class Iterator {
   public:
    using value_type = std::string_view;
    using difference_type = std::ptrdiff_t;
    using iterator_category = std::forward_iterator_tag;
    Iterator() = default;
    value_type operator*() const {
      return {reinterpret_cast<const char*>(at_+4),platform::LoadLittle32(at_)};
    }
    Iterator& operator++() { at_ += 4+platform::LoadLittle32(at_); return *this; }
    Iterator operator++(int) { auto old=*this; ++*this; return old; }
    bool operator==(const Iterator&) const = default;
   private:
    friend class CatalogMetricAliasSequenceView;
    explicit Iterator(const byte* at) : at_(at) {}
    const byte* at_ = nullptr;
  };
  CatalogMetricAliasSequenceView() = default;
  std::size_t size() const { return count_; }
  bool empty() const { return count_==0; }
  Iterator begin() const { return Iterator(bytes_.data()); }
  Iterator end() const { return Iterator(bytes_.empty()?bytes_.data():bytes_.data()+bytes_.size()); }
 private:
  friend CatalogMetricDescriptorViewResult DecodeCatalogMetricDescriptorView(std::string_view);
  CatalogMetricAliasSequenceView(std::span<const byte> bytes,std::size_t count) : bytes_(bytes),count_(count) {}
  std::span<const byte> bytes_;
  std::size_t count_ = 0;
};
struct CatalogMetricDescriptorDefinitionView {
  std::string_view family, namespace_path, help, producer_owner, security_family;
  metrics::MetricType type = metrics::MetricType::counter;
  metrics::MetricUnit unit = metrics::MetricUnit::none;
  metrics::MetricVisibilityScope visibility = metrics::MetricVisibilityScope::family;
  bool cluster_only = false;
  CatalogMetricLabelSequenceView labels;
  CatalogMetricAliasSequenceView aliases;
  CatalogMetricNumericSequenceView histogram_buckets;
  bool histogram_cumulative = true;
  metrics::MetricScalarType value_type = metrics::MetricScalarType::invalid;
  std::optional<metrics::MetricScalar> min_value, max_value;
  CatalogMetricEnumSequenceView enum_values;
  u64 rate_window_nanoseconds = 0;
};
struct CatalogMetricDescriptorView {
  CatalogMetricDescriptorDefinitionView definition;
  metrics::MetricDescriptorBinding binding;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id = 0;
};
struct CatalogMetricDescriptorViewResult {
  CatalogValueError error = CatalogValueError::invalid_value;
  std::optional<CatalogMetricDescriptorView> record;
  bool ok() const { return error==CatalogValueError::none && record.has_value(); }
};
CatalogMetricDescriptorViewResult DecodeCatalogMetricDescriptorView(std::string_view);
const CatalogValueSchema& CatalogMetricDescriptorSchema();
CatalogValueEncodeResult EncodeCatalogMetricDescriptor(const CatalogMetricDescriptor&);
CatalogMetricDescriptorResult DecodeCatalogMetricDescriptor(std::string_view);
bool IsCatalogMetricDescriptorPayload(std::string_view);
bool CatalogMetricDescriptorMatchesHeader(const CatalogTypedRecord&);
bool CatalogMetricDescriptorMatchesHeader(const CatalogTypedRecordView&);
bool CatalogMetricDescriptorMatchesMetadata(const CatalogMetadataVersion&);
bool CatalogMetricDescriptorPreservesOrigin(const CatalogMetadataVersion&, const CatalogMetadataVersion&);
bool CatalogMetricDescriptorMatchesMetadata(const CatalogMetadataVersionView&);
bool CatalogMetricDescriptorPreservesOrigin(const CatalogMetadataVersionView&, const CatalogMetadataVersionView&);
}  // namespace scratchbird::core::catalog
