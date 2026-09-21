// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_record_codec.hpp"
#include "catalog_value_codec.hpp"
#include "metric_registry.hpp"
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
const CatalogValueSchema& CatalogMetricDescriptorSchema();
CatalogValueEncodeResult EncodeCatalogMetricDescriptor(const CatalogMetricDescriptor&);
CatalogMetricDescriptorResult DecodeCatalogMetricDescriptor(std::string_view);
bool IsCatalogMetricDescriptorPayload(std::string_view);
bool CatalogMetricDescriptorMatchesHeader(const CatalogTypedRecord&);
bool CatalogMetricDescriptorMatchesMetadata(const CatalogMetadataVersion&);
bool CatalogMetricDescriptorPreservesOrigin(const CatalogMetadataVersion&, const CatalogMetadataVersion&);
}  // namespace scratchbird::core::catalog
