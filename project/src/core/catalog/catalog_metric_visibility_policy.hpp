// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_record_codec.hpp"
#include "catalog_value_codec.hpp"
#include <optional>
#include <string_view>

namespace scratchbird::core::catalog {
// Native local visibility definition, not a grant or activation receipt.
struct CatalogMetricVisibilityPolicy {
  Uuid policy_uuid;
  u64 generation = 0;
  Uuid database_uuid;
  Uuid metric_uuid;
  std::string read_right;
  std::string sensitive_read_right;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id = 0;
};
struct CatalogMetricVisibilityPolicyResult {
  CatalogValueError error = CatalogValueError::invalid_value;
  std::optional<CatalogMetricVisibilityPolicy> record;
  bool ok() const { return error == CatalogValueError::none && record.has_value(); }
};
const CatalogValueSchema& CatalogMetricVisibilityPolicySchema();
CatalogValueEncodeResult EncodeCatalogMetricVisibilityPolicy(const CatalogMetricVisibilityPolicy&);
CatalogMetricVisibilityPolicyResult DecodeCatalogMetricVisibilityPolicy(std::string_view);
bool IsCatalogMetricVisibilityPolicyPayload(std::string_view);
bool CatalogMetricVisibilityPolicyMatchesHeader(const CatalogTypedRecord&);
bool CatalogMetricVisibilityPolicyMatchesMetadata(const CatalogMetadataVersion&);
bool CatalogMetricVisibilityPolicyPreservesOrigin(const CatalogMetadataVersion&, const CatalogMetadataVersion&);
}  // namespace scratchbird::core::catalog
