// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "binary_uuid_fixture.hpp"
#include "security/security_model.hpp"
#include "uuid.hpp"

#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>

namespace scratchbird::tests {

// Component-level catalog input to the real authorization materializer. This
// is not proof of durable catalog publication, login, or IPC admission. Rights
// are explicit test inputs; trace strings are never converted into authority.
inline void MaterializeComponentAuthorization(
    engine::internal_api::EngineRequestContext& context,
    std::initializer_list<std::string_view> rights,
    std::initializer_list<std::string_view> denies = {}) {
  namespace api = engine::internal_api;
  if (!core::uuid::IsEngineIdentityUuid(context.database_uuid) ||
      !core::uuid::IsEngineIdentityUuid(context.principal_uuid))
    throw std::runtime_error("component authorization requires native owner/principal identities");
  api::DurableAuthorizationState catalog;
  catalog.authority_uuid = context.database_uuid;
  catalog.security_context_generation = 1;
  catalog.security_epoch = context.security_epoch == 0 ? 1 : context.security_epoch;
  catalog.policy_epoch = context.resource_epoch == 0 ? 1 : context.resource_epoch;
  catalog.catalog_generation_id = context.catalog_generation_id == 0 ? 1 : context.catalog_generation_id;
  catalog.principals.push_back({context.principal_uuid, "principal", true, catalog.security_epoch});
  const auto add = [&](std::string_view right, bool deny) {
    catalog.grants.push_back({FixtureUuid(2226, static_cast<std::uint32_t>(catalog.grants.size() + 1)),
        context.principal_uuid, "principal", {}, std::string(right), deny, true, catalog.security_epoch});
  };
  for (auto right : rights) add(right, false);
  for (auto right : denies) add(right, true);
  const auto materialized = api::MaterializeDurableAuthorizationContext(catalog,
      {context.principal_uuid, catalog.security_epoch, catalog.policy_epoch, catalog.catalog_generation_id});
  if (!materialized.ok) throw std::runtime_error("component authorization materialization failed");
  context.authorization_context = materialized.context;
  context.security_epoch = catalog.security_epoch;
  context.catalog_generation_id = catalog.catalog_generation_id;
}
}  // namespace scratchbird::tests
