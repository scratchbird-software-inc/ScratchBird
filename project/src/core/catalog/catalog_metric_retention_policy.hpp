// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_record_codec.hpp"
#include "catalog_value_codec.hpp"
#include "metric_retention_policy.hpp"
#include <optional>
#include <array>
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

// Validated native payload, not a selected policy or cleanup permission. Text
// borrows immutable caller input; grains are inline so copying/moving a result
// never leaves a span pointing into the old result. Retain the input grant.
struct CatalogMetricRetentionPolicyView {
  Uuid policy_uuid;
  u64 generation = 0;
  std::string_view policy_name;
  std::string_view scope;
  metrics::MetricRetentionMode mode = metrics::MetricRetentionMode::current_only;
  u64 raw_retention_seconds = 0, rollup_retention_seconds = 0;
  std::array<metrics::MetricRollupGrain, 4> rollup_grains{};
  std::size_t rollup_grain_count = 0;
  u64 purge_batch_limit = 0, max_cardinality = 0;
  std::string_view overflow_behavior, edit_right, default_admin_group;
  bool evidence_required = false;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id = 0;
};
struct CatalogMetricRetentionPolicyViewResult {
  CatalogValueError error = CatalogValueError::invalid_value;
  std::optional<CatalogMetricRetentionPolicyView> record;
  bool ok() const { return error == CatalogValueError::none && record.has_value(); }
};
CatalogMetricRetentionPolicyViewResult DecodeCatalogMetricRetentionPolicyView(std::string_view);
const CatalogValueSchema& CatalogMetricRetentionPolicySchema();
CatalogValueEncodeResult EncodeCatalogMetricRetentionPolicy(const CatalogMetricRetentionPolicy&);
CatalogMetricRetentionPolicyResult DecodeCatalogMetricRetentionPolicy(std::string_view);
// Schema identification is for family-confusion rejection only, not validation.
bool IsCatalogMetricRetentionPolicyPayload(std::string_view);
bool CatalogMetricRetentionPolicyMatchesHeader(const CatalogTypedRecord&);
bool CatalogMetricRetentionPolicyMatchesHeader(const CatalogTypedRecordView&);
bool CatalogMetricRetentionPolicyMatchesMetadata(const CatalogMetadataVersion&);
bool CatalogMetricRetentionPolicyPreservesOrigin(
    const CatalogMetadataVersion& previous, const CatalogMetadataVersion& successor);
}  // namespace scratchbird::core::catalog
