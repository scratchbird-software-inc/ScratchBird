// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "metric_visibility_policy.hpp"
#include "security_model.hpp"
#include "catalog_metric_descriptor.hpp"
#include "catalog_metric_visibility_policy.hpp"
#include "uuid.hpp"

namespace scratchbird::engine::internal_api {
namespace {
namespace c = core::catalog;
bool ActiveLocal(const c::CatalogMetadataVersion& m) {
  return !m.record.header.deleted && m.lifecycle == c::CatalogObjectLifecycle::active &&
      m.status == c::CatalogObjectStatus::active && m.authority_scope == c::CatalogAuthorityScope::local &&
      c::EncodeCatalogMetadataVersion(m).ok();
}
bool Granted(const MaterializedAuthorizationDecision& d) {
  return d.authorized && !d.denied && !d.policy_recheck_required;
}
}
LocalMetricVisibilityDecision EvaluateLocalMetricVisibility(
    const EngineRequestContext& context, const c::CatalogMetadataVersion& descriptor,
    const c::CatalogMetadataVersion& policy) noexcept {
  try {
    if (!core::uuid::IsEngineIdentityUuid(context.database_uuid) ||
        context.authorization_context.authority_uuid != context.database_uuid ||
        !ActiveLocal(descriptor) || !ActiveLocal(policy) ||
        !c::CatalogMetricDescriptorMatchesMetadata(descriptor) ||
        !c::CatalogMetricVisibilityPolicyMatchesMetadata(policy)) return {};
    const auto decoded_descriptor = c::DecodeCatalogMetricDescriptor(descriptor.record.payload);
    const auto decoded_policy = c::DecodeCatalogMetricVisibilityPolicy(policy.record.payload);
    if (!decoded_descriptor.ok() || !decoded_policy.ok()) return {};
    const auto& d = *decoded_descriptor.record;
    const auto& p = *decoded_policy.record;
    if (d.definition.cluster_only || p.database_uuid != context.database_uuid ||
        p.metric_uuid != d.binding.metric_uuid || p.policy_uuid != d.binding.visibility_policy_uuid ||
        p.generation != d.binding.visibility_policy_generation ||
        p.read_right != d.definition.security_family || !IsKnownSecurityRight(p.read_right) ||
        (!p.sensitive_read_right.empty() && !IsKnownSecurityRight(p.sensitive_read_right))) return {};
    if (!Granted(EvaluateMaterializedAuthorization(context, context.authorization_context,
                                                   p.read_right, p.metric_uuid))) return {};
    const bool sensitive = !p.sensitive_read_right.empty() &&
        Granted(EvaluateMaterializedAuthorization(context, context.authorization_context,
                                                  p.sensitive_read_right, p.metric_uuid));
    return {true, sensitive, {}};
  } catch (...) {
    // No allocation, corruption, or evaluator exception may publish partial access.
    return {};
  }
}
}  // namespace scratchbird::engine::internal_api
