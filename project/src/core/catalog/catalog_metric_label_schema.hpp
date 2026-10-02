// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_record_codec.hpp"
#include "catalog_value_codec.hpp"
#include "metric_registry.hpp"
#include <optional>
#include <iterator>
#include <string_view>

namespace scratchbird::core::catalog {
struct CatalogMetricLabelSchema {
  Uuid label_schema_uuid;
  u64 generation = 0;
  std::vector<metrics::MetricLabelDescriptor> labels;
  bool cluster_only = false;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id = 0;
};
struct CatalogMetricLabelSchemaResult {
  CatalogValueError error = CatalogValueError::invalid_value;
  std::optional<CatalogMetricLabelSchema> record;
  bool ok() const { return error == CatalogValueError::none && record.has_value(); }
};
struct CatalogMetricLabelView {
  std::string_view key;
  bool required = false;
  bool sensitive = false;
  metrics::MetricLabelType value_type = metrics::MetricLabelType::text;
};
struct CatalogMetricLabelSchemaViewResult;
struct CatalogMetricDescriptorViewResult;
// Constructed only by complete schema validation. Iterators and returned keys
// borrow immutable input; retain its owner and actual memory grant during use.
class CatalogMetricLabelSequenceView {
 public:
  class Iterator {
   public:
    using value_type = CatalogMetricLabelView;
    using difference_type = std::ptrdiff_t;
    using iterator_category = std::forward_iterator_tag;
    Iterator() = default;
    value_type operator*() const;
    Iterator& operator++();
    Iterator operator++(int) { auto old = *this; ++*this; return old; }
    bool operator==(const Iterator&) const = default;
   private:
    friend class CatalogMetricLabelSequenceView;
    Iterator(const byte* key, const byte* flags, const byte* types)
        : key_(key), flags_(flags), types_(types) {}
    const byte* key_ = nullptr;
    const byte* flags_ = nullptr;
    const byte* types_ = nullptr;
  };
  CatalogMetricLabelSequenceView() = default;
  std::size_t size() const { return flags_.size(); }
  bool empty() const { return flags_.empty(); }
  Iterator begin() const { return {keys_.data(), flags_.data(), types_.data()}; }
  Iterator end() const {
    return {keys_.empty() ? keys_.data() : keys_.data() + keys_.size(),
            flags_.empty() ? flags_.data() : flags_.data() + flags_.size(),
            types_.empty() ? types_.data() : types_.data() + types_.size()};
  }
 private:
  friend CatalogMetricLabelSchemaViewResult DecodeCatalogMetricLabelSchemaView(std::string_view);
  friend CatalogMetricDescriptorViewResult DecodeCatalogMetricDescriptorView(std::string_view);
  CatalogMetricLabelSequenceView(std::span<const byte> keys,
      std::span<const byte> flags, std::span<const byte> types)
      : keys_(keys), flags_(flags), types_(types) {}
  std::span<const byte> keys_, flags_, types_;
};
struct CatalogMetricLabelSchemaView {
  Uuid label_schema_uuid;
  u64 generation = 0;
  CatalogMetricLabelSequenceView labels;
  bool cluster_only = false;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id = 0;
};
struct CatalogMetricLabelSchemaViewResult {
  CatalogValueError error = CatalogValueError::invalid_value;
  std::optional<CatalogMetricLabelSchemaView> record;
  bool ok() const { return error == CatalogValueError::none && record.has_value(); }
};
CatalogMetricLabelSchemaViewResult DecodeCatalogMetricLabelSchemaView(std::string_view);
const CatalogValueSchema& CatalogMetricLabelSchemaSchema();
CatalogValueEncodeResult EncodeCatalogMetricLabelSchema(const CatalogMetricLabelSchema&);
CatalogMetricLabelSchemaResult DecodeCatalogMetricLabelSchema(std::string_view);
bool IsCatalogMetricLabelSchemaPayload(std::string_view);
bool CatalogMetricLabelSchemaMatchesHeader(const CatalogTypedRecord&);
bool CatalogMetricLabelSchemaMatchesHeader(const CatalogTypedRecordView&);
bool CatalogMetricLabelSchemaMatchesMetadata(const CatalogMetadataVersion&);
bool CatalogMetricLabelSchemaPreservesOrigin(const CatalogMetadataVersion&, const CatalogMetadataVersion&);
// Structural comparison only. The caller still owns actual catalog snapshot,
// visibility, producer/policy admission and publication.
bool CatalogMetricLabelSchemaMatchesDescriptor(const CatalogMetricLabelSchema&,
    const metrics::MetricDescriptorDefinition&, const metrics::MetricDescriptorBinding&);
bool CatalogMetricLabelSchemaMatchesMetadata(const CatalogMetadataVersionView&);
bool CatalogMetricLabelSchemaPreservesOrigin(const CatalogMetadataVersionView&, const CatalogMetadataVersionView&);
}  // namespace scratchbird::core::catalog
