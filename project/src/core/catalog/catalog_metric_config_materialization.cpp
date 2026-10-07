// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_metric_config_materialization.hpp"
#include "uuid.hpp"
#include <array>
#include <new>

namespace scratchbird::core::catalog {
namespace {
// Resource preflight, not a replacement for the complete native codecs. Bound
// every owned sequence and its aggregate payload before copying caller inputs.
bool Bounded(const metrics::MetricDescriptorDefinition& d) noexcept {
  if (d.labels.size()>1024 || d.aliases.size()>1024 ||
      d.histogram_buckets.size()>4096 || d.enum_values.size()>4096) return false;
  // MetricScalar also has a text alternative. Numeric bounds/buckets never
  // permit it, and rejecting it here prevents an unbounded hidden string copy.
  for(const auto* bound:{&d.min_value,&d.max_value})
    if(*bound&&std::holds_alternative<std::string>(**bound))return false;
  for(const auto& bucket:d.histogram_buckets)
    if(std::holds_alternative<std::string>(bucket))return false;
  std::size_t remaining=kCatalogValueBlockMaxBytes;
  const auto take=[&](std::size_t n) {if(n>remaining)return false;remaining-=n;return true;};
  const auto text=[&](std::string_view v,std::size_t limit) {return v.size()<=limit&&take(v.size());};
  if (!text(d.family,4096)||!text(d.namespace_path,4096)||!text(d.producer_owner,4096)||
      !text(d.help,16384)||!text(d.security_family,4096)) return false;
  // This is a conservative lower bound on encoded bytes, not an exact fit
  // claim. Element-count limits above independently bound vector overhead.
  if (!take(d.enum_values.size()*8)||!take(d.histogram_buckets.size()*8)) return false;
  for(const auto& label:d.labels)if(!text(label.key,4096))return false;
  for(const auto& alias:d.aliases)if(!text(alias,4096))return false;
  return true;
}
bool Bounded(const config::MetricPolicyDefinition& p) noexcept {
  const auto& r=p.retention;
  return r.policy_name.size()<=4096&&r.edit_right.size()<=4096&&r.default_admin_group.size()<=4096&&
      r.scope.size()<=8&&r.overflow_behavior.size()<=31&&r.rollup_grains.size()<=4&&p.read_right.size()<=128;
}
}
CatalogMetricMaterializationResult MaterializeConfiguredLocalMetricDefinitions(
    const config::MetricPolicyConfig& configuration, const metrics::MetricDescriptorDefinition& definition,
    const metrics::MetricDescriptorBinding& binding, const Uuid& database_uuid,
    const TypedUuid& transaction_uuid, u64 transaction_id,
    const config::MetricPolicyDefinition* governing) noexcept {
  using E = CatalogMetricMaterializationError;
  const auto fail = [](E error) { return CatalogMetricMaterializationResult{error, {}}; };
  try {
    if (!Bounded(definition)) return fail(E::invalid_definition);
    if (definition.cluster_only || definition.visibility == metrics::MetricVisibilityScope::cluster)
      return fail(E::nonlocal_scope);
    const bool labels = !uuid::IsNilUuid(binding.label_schema_uuid);
    const std::array ids{database_uuid, transaction_uuid.value, binding.metric_uuid,
        binding.retention_policy_uuid, binding.visibility_policy_uuid, binding.label_schema_uuid};
    const auto count = ids.size() - (labels ? 0 : 1);
    if (transaction_uuid.kind != UuidKind::transaction || !transaction_id)
      return fail(E::invalid_identity);
    for (std::size_t i = 0; i < count; ++i) {
      if (!uuid::IsEngineIdentityUuid(ids[i])) return fail(E::invalid_identity);
      for (std::size_t j = 0; j < i; ++j) if (ids[i] == ids[j]) return fail(E::invalid_identity);
      if (!uuid::IsNilUuid(binding.rate_source_counter_uuid) && ids[i] == binding.rate_source_counter_uuid)
        return fail(E::invalid_identity);
    }
    if (binding.descriptor_generation != 1 || binding.retention_policy_generation != 1 ||
        binding.visibility_policy_generation != 1 || binding.label_schema_generation != (labels ? 1 : 0))
      return fail(E::invalid_generation);
    const auto found=configuration.families.find(definition.family);
    const auto* input=governing?governing:(found==configuration.families.end()?nullptr:&found->second);
    if (input&&!Bounded(*input)) return fail(E::invalid_definition);
    const auto selected = config::SelectMetricPolicyDefinition(configuration, definition.family, governing);
    if (!selected.ok()) return fail(E::policy_not_selected);
    if (selected.definition->read_right != definition.security_family)
      return fail(E::policy_mismatch);
    CatalogMetricDefinitionBundle out;
    out.descriptor = {definition, binding, transaction_uuid, transaction_id};
    if (!EncodeCatalogMetricDescriptor(out.descriptor).ok()) return fail(E::invalid_definition);
    if (labels) {
      out.labels = CatalogMetricLabelSchema{binding.label_schema_uuid, 1, definition.labels,
          false, transaction_uuid, transaction_id};
      if (!EncodeCatalogMetricLabelSchema(*out.labels).ok()) return fail(E::invalid_definition);
    }
    static_cast<metrics::MetricRetentionPolicyDefinition&>(out.retention.policy) = selected.definition->retention;
    out.retention.policy.policy_uuid = binding.retention_policy_uuid;
    out.retention.policy.generation = 1;
    out.retention.origin_transaction_uuid = transaction_uuid;
    out.retention.origin_local_transaction_id = transaction_id;
    out.visibility = {binding.visibility_policy_uuid, 1, database_uuid, binding.metric_uuid,
        selected.definition->read_right, {}, transaction_uuid, transaction_id};
    if (!EncodeCatalogMetricRetentionPolicy(out.retention).ok() ||
        !EncodeCatalogMetricVisibilityPolicy(out.visibility).ok()) return fail(E::invalid_definition);
    return {E::none, std::move(out)};
  } catch (const std::bad_alloc&) {
    return fail(E::resource_exhausted);
  } catch (...) {
    return fail(E::invalid_definition);
  }
}
}  // namespace scratchbird::core::catalog
