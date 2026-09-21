// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_metric_binding.hpp"
#include "uuid.hpp"
#include <new>
#include <stdexcept>
#include <utility>

namespace scratchbird::core::catalog {
namespace {
using E = CatalogMetricBindingError;
const CatalogMetadataVersion* Find(std::span<const CatalogMetricRowView> rows,
    const Uuid& identity, u64 generation, CatalogRecordKind kind, E& error) {
  const CatalogMetricRowView* selected = nullptr;
  for (const auto& row : rows) {
    if (!row.metadata) { error = E::invalid_catalog; return nullptr; }
    if (row.metadata->record.header.object_uuid.value != identity) continue;
    if (selected) { error = E::duplicate_identity; return nullptr; }
    selected = &row;
  }
  if (!selected) { error = E::missing_object; return nullptr; }
  const auto& m = *selected->metadata;
  if (selected->provisional || m.record.header.deleted ||
      m.lifecycle != CatalogObjectLifecycle::active || m.status != CatalogObjectStatus::active) {
    error = E::inactive_object; return nullptr;
  }
  if (m.authority_scope != CatalogAuthorityScope::local) { error = E::nonlocal_scope; return nullptr; }
  if (m.definition_version != generation) { error = E::stale_generation; return nullptr; }
  if (m.record.header.kind != kind) { error = E::wrong_family; return nullptr; }
  if (!EncodeCatalogMetadataVersion(m).ok()) { error = E::invalid_catalog; return nullptr; }
  return &m;
}
bool Bind(std::span<const CatalogMetricRowView> rows, const Uuid& identity, u64 generation,
          CatalogMetricDependencyBinding& result, E& error) {
  const auto* metadata = Find(rows,identity,generation,CatalogRecordKind::metric_descriptor,error);
  if (!metadata) return false;
  auto descriptor = DecodeCatalogMetricDescriptor(metadata->record.payload);
  if (!descriptor.ok()) { error = E::invalid_catalog; return false; }
  result.descriptor_metadata = *metadata;
  result.descriptor = std::move(*descriptor.record);
  const auto& d = result.descriptor.definition; const auto& b = result.descriptor.binding;
  if (d.cluster_only) { error = E::nonlocal_scope; return false; }
  if (b.label_schema_generation) {
    const auto* schema = Find(rows,b.label_schema_uuid,b.label_schema_generation,CatalogRecordKind::metric_label_schema,error);
    if (!schema) return false;
    auto labels = DecodeCatalogMetricLabelSchema(schema->record.payload);
    if (!labels.ok()) { error = E::invalid_catalog; return false; }
    if (!CatalogMetricLabelSchemaMatchesDescriptor(*labels.record,d,b)) { error = E::label_mismatch; return false; }
    result.label_metadata = *schema;
    result.labels = std::move(labels.record);
  }
  const auto* policy = Find(rows,b.retention_policy_uuid,b.retention_policy_generation,CatalogRecordKind::policy,error);
  if (!policy) return false;
  auto retention = DecodeCatalogMetricRetentionPolicy(policy->record.payload);
  if (!retention.ok() || !CatalogMetricRetentionPolicyMatchesMetadata(*policy)) { error = E::wrong_family; return false; }
  if (retention.record->policy.scope == "cluster") { error = E::nonlocal_scope; return false; }
  result.retention_metadata = *policy;
  result.retention = std::move(*retention.record);
  const auto* visibility = Find(rows,b.visibility_policy_uuid,b.visibility_policy_generation,CatalogRecordKind::policy,error);
  if (!visibility) return false;
  result.visibility_policy = *visibility;
  return true;
}
}  // namespace
CatalogMetricBindingResult ResolveLocalCatalogMetricBindings(
    std::span<const CatalogMetricRowView> rows, const Uuid& metric_uuid, u64 generation) noexcept {
  if (!uuid::IsEngineIdentityUuid(metric_uuid) || !generation) return {};
  try {
    E error = E::none;
    CatalogMetricBindingSet result;
    if (!Bind(rows,metric_uuid,generation,result.metric,error)) return {error,{}};
    if (result.metric.descriptor.definition.type == metrics::MetricType::rate) {
      const auto& b = result.metric.descriptor.binding;
      CatalogMetricDependencyBinding source;
      if (!Bind(rows,b.rate_source_counter_uuid,b.rate_source_counter_generation,source,error)) return {error,{}};
      if (source.descriptor.definition.type != metrics::MetricType::counter) return {E::source_not_counter,{}};
      result.source_counter = std::move(source);
    }
    return {E::none,std::move(result)};
  } catch (const std::bad_alloc&) { return {E::resource_exhausted,{}}; }
    catch (const std::length_error&) { return {E::resource_exhausted,{}}; }
    catch (...) { return {E::invalid_catalog,{}}; }
}
}  // namespace scratchbird::core::catalog
