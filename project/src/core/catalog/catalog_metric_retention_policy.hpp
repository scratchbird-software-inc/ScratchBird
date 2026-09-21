// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_record_codec.hpp"
#include "catalog_value_codec.hpp"
#include "metric_retention_policy.hpp"
#include <optional>
#include <string_view>

namespace scratchbird::core::catalog {
// A native family definition, not a template, publication receipt or live lookup.
struct CatalogMetricRetentionPolicy {
  metrics::MetricRetentionPolicy policy;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id = 0;
};
struct CatalogMetricRetentionPolicyResult {
  CatalogValueError error = CatalogValueError::invalid_value;
  std::optional<CatalogMetricRetentionPolicy> record;
  bool ok() const { return error == CatalogValueError::none && record.has_value(); }
};
const CatalogValueSchema& CatalogMetricRetentionPolicySchema();
CatalogValueEncodeResult EncodeCatalogMetricRetentionPolicy(const CatalogMetricRetentionPolicy&);
CatalogMetricRetentionPolicyResult DecodeCatalogMetricRetentionPolicy(std::string_view);
// Schema identification is for family-confusion rejection only, not validation.
bool IsCatalogMetricRetentionPolicyPayload(std::string_view);
bool CatalogMetricRetentionPolicyMatchesHeader(const CatalogTypedRecord&);
bool CatalogMetricRetentionPolicyMatchesMetadata(const CatalogMetadataVersion&);
bool CatalogMetricRetentionPolicyPreservesOrigin(
    const CatalogMetadataVersion& previous, const CatalogMetadataVersion& successor);
}  // namespace scratchbird::core::catalog
