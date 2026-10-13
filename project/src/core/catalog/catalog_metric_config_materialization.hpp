// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_metric_descriptor.hpp"
#include "catalog_metric_retention_policy.hpp"
#include "catalog_metric_visibility_policy.hpp"
#include "../config/metric_policy_config.hpp"
#include <vector>

namespace scratchbird::core::catalog {
// First-version payloads only: no common metadata, names, publication, grants,
// producer readiness, series or observations are manufactured by this builder.
// The owning catalog supplies issuer-allocated identities and retained inputs,
// admits references/security and publishes all records as one native operation.
struct CatalogMetricDefinitionBundle {
  CatalogMetricDescriptor descriptor;
  std::optional<CatalogMetricLabelSchema> labels;
  CatalogMetricRetentionPolicy retention;
  CatalogMetricVisibilityPolicy visibility;
};
enum class CatalogMetricMaterializationError {
  none, invalid_identity, invalid_generation, invalid_definition,
  policy_not_selected, policy_mismatch, nonlocal_scope, resource_exhausted
};
struct CatalogMetricMaterializationResult {
  CatalogMetricMaterializationError error = CatalogMetricMaterializationError::invalid_definition;
  std::optional<CatalogMetricDefinitionBundle> definitions;
  bool ok() const { return error == CatalogMetricMaterializationError::none && definitions.has_value(); }
};
// Exact family selection, never registry enumeration or a runtime fallback.
// Governing family policy overrides the complete file definition. It is owning
// catalog input, not an authorization boolean. Rights vocabulary and authority
// are checked by that owner; this boundary checks exact descriptor agreement.
CatalogMetricMaterializationResult MaterializeConfiguredLocalMetricDefinitions(
    const config::MetricPolicyConfig&, const metrics::MetricDescriptorDefinition&,
    const metrics::MetricDescriptorBinding&, const Uuid& database_uuid,
    const TypedUuid& origin_transaction_uuid, u64 origin_local_transaction_id,
    const config::MetricPolicyDefinition* governing_definition = nullptr) noexcept;

// Borrowed compiled definitions and owner-issued binary identities. A batch
// covers every selected local family exactly once; no registry enumeration,
// identity generation, observation, grant or publication is implied.
struct CatalogConfiguredMetricInput {
  const metrics::MetricDescriptorDefinition* definition = nullptr;
  metrics::MetricDescriptorBinding binding;
};
struct CatalogMetricBatchMaterializationResult {
  CatalogMetricMaterializationError error = CatalogMetricMaterializationError::invalid_definition;
  std::optional<std::vector<CatalogMetricDefinitionBundle>> definitions;
  bool ok() const { return error == CatalogMetricMaterializationError::none && definitions.has_value(); }
};
// Resource/identity/family preflight the entire borrowed batch (including
// cross-family identity custody) before making any owning payload copies.
// Complete native semantic codec checks follow. On every failure return no partial
// bundle. Input and result allocations remain the owning catalog's admission
// responsibility; this is not a storage or memory grant.
CatalogMetricBatchMaterializationResult MaterializeConfiguredLocalMetricBatch(
    const config::MetricPolicyConfig&, const std::vector<CatalogConfiguredMetricInput>&,
    const Uuid& database_uuid, const TypedUuid& origin_transaction_uuid,
    u64 origin_local_transaction_id) noexcept;
}  // namespace scratchbird::core::catalog
