// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_record_codec.hpp"
#include "catalog_value_codec.hpp"
#include "metric_registry.hpp"
#include <optional>
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
const CatalogValueSchema& CatalogMetricLabelSchemaSchema();
CatalogValueEncodeResult EncodeCatalogMetricLabelSchema(const CatalogMetricLabelSchema&);
CatalogMetricLabelSchemaResult DecodeCatalogMetricLabelSchema(std::string_view);
bool IsCatalogMetricLabelSchemaPayload(std::string_view);
bool CatalogMetricLabelSchemaMatchesHeader(const CatalogTypedRecord&);
bool CatalogMetricLabelSchemaMatchesMetadata(const CatalogMetadataVersion&);
bool CatalogMetricLabelSchemaPreservesOrigin(const CatalogMetadataVersion&, const CatalogMetadataVersion&);
// Structural comparison only. The caller still owns actual catalog snapshot,
// visibility, producer/policy admission and publication.
bool CatalogMetricLabelSchemaMatchesDescriptor(const CatalogMetricLabelSchema&,
    const metrics::MetricDescriptorDefinition&, const metrics::MetricDescriptorBinding&);
}  // namespace scratchbird::core::catalog
