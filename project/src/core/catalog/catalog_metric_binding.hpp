// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_metric_descriptor.hpp"
#include "catalog_metric_label_schema.hpp"
#include "catalog_metric_retention_policy.hpp"
#include <span>

namespace scratchbird::core::catalog {
// Views over one already-selected catalog snapshot, not caller authorization.
// Only the native open-device owner establishes visibility/commit provenance.
struct CatalogMetricRowView {
  const CatalogMetadataVersion* metadata = nullptr;
  bool provisional = true;
};
struct CatalogMetricDependencyBinding {
  CatalogMetadataVersion descriptor_metadata;
  CatalogMetricDescriptor descriptor;
  std::optional<CatalogMetadataVersion> label_metadata;
  std::optional<CatalogMetricLabelSchema> labels;
  CatalogMetadataVersion retention_metadata;
  CatalogMetricRetentionPolicy retention;
  // Opaque to metric selection. The security owner must still interpret and
  // authorize this exact retained policy; its presence grants no permission.
  CatalogMetadataVersion visibility_policy;
};
struct CatalogMetricBindingSet {
  CatalogMetricDependencyBinding metric;
  std::optional<CatalogMetricDependencyBinding> source_counter;
};
enum class CatalogMetricBindingError {
  none, invalid_request, invalid_catalog, duplicate_identity, missing_object,
  stale_generation, inactive_object, wrong_family, nonlocal_scope,
  label_mismatch, source_not_counter, resource_exhausted
};
struct CatalogMetricBindingResult {
  CatalogMetricBindingError error = CatalogMetricBindingError::invalid_request;
  std::optional<CatalogMetricBindingSet> binding;
  bool ok() const { return error == CatalogMetricBindingError::none && binding.has_value(); }
};
// Complete structural dependency selection, not live authority or activation.
// No family/path/name lookup and no compiled/default policy substitution.
CatalogMetricBindingResult ResolveLocalCatalogMetricBindings(
    std::span<const CatalogMetricRowView>, const Uuid& metric_uuid, u64 generation) noexcept;
}  // namespace scratchbird::core::catalog
