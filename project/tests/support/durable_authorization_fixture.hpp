// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "../../src/storage/database/database_lifecycle.hpp"
#include "../../src/engine/internal_api/security/security_principal_lifecycle.hpp"
#include "../../src/engine/internal_api/security/security_model.hpp"
#include <stdexcept>
namespace scratchbird::tests {
inline void MaterializeBootstrapFixtureAuthorization(
    engine::internal_api::EngineRequestContext& context) {
  namespace api = engine::internal_api;
  const auto bootstrap = storage::database::ReadDatabaseBootstrapSecurityCatalog(context.database_path);
  if (!bootstrap.ok() || !bootstrap.state.present || !bootstrap.state.committed_by_inventory ||
      context.principal_uuid != bootstrap.state.principal_uuid.value)
    throw std::runtime_error("fixture principal is not the durable bootstrap owner");
  const auto loaded = api::LoadSecurityPrincipalLifecycleState(context);
  if (!loaded.ok) throw std::runtime_error("fixture durable security catalog unavailable");
  const auto& lifecycle = loaded.state;
  api::DurableAuthorizationState authority;
  authority.authority_uuid = context.database_uuid;
  authority.security_context_generation = lifecycle.security_context_generation;
  authority.security_epoch = lifecycle.security_generation;
  authority.policy_epoch = lifecycle.policy_generation;
  authority.catalog_generation_id = context.catalog_generation_id;
  authority.engine_owned_sysarch_role_uuid = bootstrap.state.sysarch_role_uuid.value;
  for (const auto& p : lifecycle.principals)
    if (!p.deleted && p.lifecycle_state == "active")
      authority.principals.push_back({p.principal_uuid, "principal", true, authority.security_epoch});
  for (const auto& role : lifecycle.roles)
    if (!role.deleted && role.lifecycle_state == "active")
      authority.roles.push_back({role.role_uuid, true, authority.security_epoch});
  for (const auto& member : lifecycle.memberships)
    if (!member.revoked)
      authority.memberships.push_back({member.member_principal_uuid, "principal",
          member.container_uuid, member.container_kind, true, authority.security_epoch});
  for (const auto& grant : lifecycle.grants)
    if (!grant.revoked)
      authority.grants.push_back({grant.grant_uuid, grant.grantee_uuid, grant.grantee_kind,
          grant.target_object_uuid, grant.privilege, grant.grant_effect == "deny", true,
          authority.security_epoch});
  const auto materialized = api::MaterializeDurableAuthorizationContext(authority,
      {context.principal_uuid, authority.security_epoch, authority.policy_epoch,
       authority.catalog_generation_id});
  if (!materialized.ok) throw std::runtime_error("fixture durable authorization materialization failed");
  context.authorization_context = materialized.context;
  context.security_epoch = authority.security_epoch;
}
}
