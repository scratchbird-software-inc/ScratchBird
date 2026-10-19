// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "api_types.hpp"
#include "uuid.hpp"
#include <string_view>

namespace scratchbird::engine::internal_api {

// Structural validity of an already-materialized identity only. This does not
// authenticate, reload current authority, activate a provider or grant a right.
// Empty means these identity/epoch fields agree; callers retain their actual
// security/configuration source fences through admission and publication.
inline std::string_view MaterializedAuthorizationIdentityFailure(
    const EngineRequestContext& request,
    const EngineMaterializedAuthorizationContext& authority) noexcept {
  const auto valid=[](const EngineUuid& id){return core::uuid::IsEngineIdentityUuid(id);};
  if (!request.security_context_present) return "security_context_missing";
  if (!authority.present) return "authorization_context_missing";
  if (!valid(request.principal_uuid) || !valid(authority.principal_uuid))
    return "principal_identity_invalid";
  if (request.principal_uuid != authority.principal_uuid) return "principal_mismatch";
  if (!valid(authority.authority_uuid)) return "authority_identity_invalid";
  if (!authority.security_context_generation) return "security_context_generation_missing";
  if (!authority.security_epoch) return "security_epoch_missing";
  if (!authority.policy_epoch) return "policy_epoch_missing";
  if (!authority.catalog_generation_id) return "catalog_generation_missing";
  if (request.security_epoch && request.security_epoch != authority.security_epoch)
    return "security_epoch_mismatch";
  if (request.catalog_generation_id && request.catalog_generation_id != authority.catalog_generation_id)
    return "catalog_generation_mismatch";
  return {};
}

}  // namespace scratchbird::engine::internal_api
