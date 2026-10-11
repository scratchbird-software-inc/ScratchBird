// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "context_namespace_access.hpp"
#include "catalog_context_namespace.hpp"
#include "security_model.hpp"
#include "uuid.hpp"

namespace scratchbird::engine::internal_api {
bool EvaluateContextNamespaceAccess(const EngineRequestContext& context,
    const core::catalog::CatalogMetadataVersionView& metadata,
    const EngineUuid& expected, std::uint64_t generation,
    ContextNamespaceAccess access) noexcept {
  namespace c = core::catalog;
  try {
    if ((access != ContextNamespaceAccess::read && access != ContextNamespaceAccess::write) ||
        !core::uuid::IsEngineIdentityUuid(expected) || !generation ||
        !core::uuid::IsEngineIdentityUuid(context.database_uuid) ||
        !context.catalog_generation_id || !context.security_epoch ||
        metadata.catalog_generation > context.catalog_generation_id ||
        metadata.security_epoch > context.security_epoch ||
        context.authorization_context.authority_uuid != context.database_uuid ||
        metadata.record.header.deleted || metadata.lifecycle != c::CatalogObjectLifecycle::active ||
        metadata.status != c::CatalogObjectStatus::active ||
        c::ValidateCatalogMetadataVersionView(metadata) ||
        !c::CatalogContextNamespaceMatchesMetadata(metadata)) return false;
    const auto definition = c::DecodeCatalogContextNamespace(metadata.record.payload);
    const auto& ns = *definition.record;
    if (ns.database_uuid != context.database_uuid || ns.namespace_uuid != expected ||
        ns.generation != generation) return false;
    // No trace-tag fallback, ownership shortcut, or same-name substitution.
    // Explicit denies and policy rechecks retain their shared security meaning.
    const auto decision = EvaluateMaterializedAuthorization(context, context.authorization_context,
        access == ContextNamespaceAccess::read ? "CONTEXT_READ" : "CONTEXT_WRITE", expected);
    return decision.authorized && !decision.denied && !decision.policy_recheck_required;
  } catch (...) {
    return false;
  }
}
}  // namespace scratchbird::engine::internal_api
