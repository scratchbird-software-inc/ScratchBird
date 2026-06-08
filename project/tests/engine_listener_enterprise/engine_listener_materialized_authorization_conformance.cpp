// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "security/authorization_api.hpp"
#include "security/security_model.hpp"
#include "uuid.hpp"

#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace uuid = scratchbird::core::uuid;

using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::u64;

constexpr u64 kBaseMillis = 1771200200000ull;

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

api::EngineUuid MakeUuid(UuidKind kind, u64 offset) {
  const auto generated =
      uuid::GenerateEngineIdentityV7(kind, kBaseMillis + offset);
  api::EngineUuid out;
  if (generated.ok()) {
    out.canonical = uuid::UuidToString(generated.value.value);
  }
  return out;
}

bool HasDiagnosticCode(const api::EngineApiResult& result,
                       std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) { return true; }
  }
  return false;
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && evidence.evidence_id == id) {
      return true;
    }
  }
  return false;
}

struct FixtureIds {
  api::EngineUuid authority = MakeUuid(UuidKind::object, 1);
  api::EngineUuid database = MakeUuid(UuidKind::database, 2);
  api::EngineUuid principal = MakeUuid(UuidKind::principal, 3);
  api::EngineUuid table = MakeUuid(UuidKind::object, 4);
  api::EngineUuid other_table = MakeUuid(UuidKind::object, 5);
  api::EngineUuid allow_grant = MakeUuid(UuidKind::object, 6);
  api::EngineUuid deny_grant = MakeUuid(UuidKind::object, 7);
};

api::EngineRequestContext BaseContext(const FixtureIds& ids) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = "eler032-materialized-authorization";
  context.database_uuid = ids.database;
  context.principal_uuid = ids.principal;
  context.session_uuid = MakeUuid(UuidKind::session, 20);
  context.security_context_present = true;
  context.catalog_generation_id = 13;
  context.security_epoch = 11;
  context.resource_epoch = 1;
  return context;
}

api::EngineMaterializedAuthorizationContext MaterializedContext(
    const FixtureIds& ids,
    bool include_deny = false) {
  api::EngineMaterializedAuthorizationContext context;
  context.present = true;
  context.authority_uuid = ids.authority;
  context.principal_uuid = ids.principal;
  context.security_epoch = 11;
  context.policy_epoch = 12;
  context.catalog_generation_id = 13;
  context.effective_subjects.push_back({ids.principal, "principal"});
  context.grants.push_back({ids.allow_grant,
                            ids.principal,
                            "principal",
                            ids.table,
                            "SELECT",
                            false,
                            11});
  context.grants.push_back({MakeUuid(UuidKind::object, 8),
                            ids.principal,
                            "principal",
                            {},
                            "SEC_GRANT_ADMIN",
                            false,
                            11});
  if (include_deny) {
    context.grants.push_back({ids.deny_grant,
                              ids.principal,
                              "principal",
                              ids.table,
                              "SELECT",
                              true,
                              11});
  }
  context.evidence_tags.push_back("durable_authorization_context");
  return context;
}

api::EngineAuthorizeRequest AuthorizeRequest(api::EngineRequestContext context,
                                             api::EngineUuid target,
                                             std::string right) {
  api::EngineAuthorizeRequest request;
  request.context = std::move(context);
  request.required_right = std::move(right);
  request.target_database.uuid = request.context.database_uuid;
  request.target_database.object_kind = "database";
  request.target_object.uuid = std::move(target);
  request.target_object.object_kind = "object";
  return request;
}

void ProductionTraceTagsAreNotAuthorization() {
  const FixtureIds ids;
  auto context = BaseContext(ids);
  context.trace_tags = {"right:SELECT", "group:ROOT", "role:ROLE_SECURITY_ADMIN"};
  Require(!api::SecurityTraceAuthorizationFallbackAllowed(context),
          "production context allowed trace authorization fallback");
  Require(!api::SecurityContextHasRight(context, "SELECT", ids.table.canonical),
          "production trace right authorized SELECT without materialized context");
  Require(!api::SecurityContextHasRight(context, "SEC_GRANT_ADMIN"),
          "production trace role authorized admin right without materialized context");

  const auto authorized =
      api::EngineAuthorize(AuthorizeRequest(context, ids.table, "SELECT"));
  Require(!authorized.ok && !authorized.authorized,
          "EngineAuthorize accepted trace tags without materialized context");
  Require(HasDiagnosticCode(authorized, "SECURITY.AUTHORIZATION.CONTEXT_REQUIRED"),
          "missing materialized context diagnostic drifted");
}

void ExplicitBootstrapAndFixtureFallbackAreFenced() {
  const FixtureIds ids;
  auto bootstrap = BaseContext(ids);
  bootstrap.trace_tags = {"security.bootstrap"};
  Require(api::SecurityTraceAuthorizationFallbackAllowed(bootstrap),
          "bootstrap fallback was not explicit");
  Require(api::SecurityContextHasRight(bootstrap, "SELECT", ids.table.canonical),
          "bootstrap context did not authorize helper route");

  auto embedded_without_fixture = BaseContext(ids);
  embedded_without_fixture.trust_mode = api::EngineTrustMode::embedded_in_process;
  embedded_without_fixture.trace_tags = {"right:SELECT"};
  Require(!api::SecurityTraceAuthorizationFallbackAllowed(embedded_without_fixture),
          "embedded context allowed trace fallback without fixture marker");
  Require(!api::SecurityContextHasRight(embedded_without_fixture,
                                        "SELECT",
                                        ids.table.canonical),
          "embedded trace right authorized without fixture marker");

  auto fixture = embedded_without_fixture;
  fixture.trace_tags.push_back("security.fixture_trace_authority");
  Require(api::SecurityTraceAuthorizationFallbackAllowed(fixture),
          "fixture trace fallback marker was not honored");
  Require(api::SecurityContextHasRight(fixture, "SELECT", ids.table.canonical),
          "fixture trace right did not authorize helper route");
}

void MaterializedContextIsAuthority() {
  const FixtureIds ids;
  auto context = BaseContext(ids);
  context.authorization_context = MaterializedContext(ids);
  context.trace_tags = {"deny:SELECT", "security_context:expired"};

  Require(api::SecurityContextHasRight(context, "SELECT", ids.table.canonical),
          "materialized grant did not authorize helper route");
  Require(!api::SecurityContextHasRight(context, "SELECT", ids.other_table.canonical),
          "target-specific materialized grant authorized another object");

  const auto select =
      api::EngineAuthorize(AuthorizeRequest(context, ids.table, "SELECT"));
  Require(select.ok && select.authorized,
          "EngineAuthorize did not accept materialized SELECT grant");
  Require(HasEvidence(select,
                      "authorization_authority",
                      "materialized_authorization_context"),
          "authorization authority evidence missing");

  auto denied_context = BaseContext(ids);
  denied_context.authorization_context = MaterializedContext(ids, true);
  const auto denied =
      api::EngineAuthorize(AuthorizeRequest(denied_context, ids.table, "SELECT"));
  Require(!denied.ok && !denied.authorized,
          "explicit materialized deny did not override allow");
  Require(HasDiagnosticCode(denied, "SECURITY.AUTHORIZATION.DENIED"),
          "explicit deny diagnostic drifted");

  auto stale = BaseContext(ids);
  stale.security_epoch = 12;
  stale.authorization_context = MaterializedContext(ids);
  Require(!api::SecurityContextHasRight(stale, "SELECT", ids.table.canonical),
          "stale request epoch authorized through materialized context");
  const auto stale_authorize =
      api::EngineAuthorize(AuthorizeRequest(stale, ids.table, "SELECT"));
  Require(!stale_authorize.ok && !stale_authorize.authorized,
          "EngineAuthorize accepted stale materialized authorization context");
  Require(HasDiagnosticCode(stale_authorize, "SECURITY.CONTEXT.EXPIRED"),
          "stale authorization diagnostic drifted");
}

}  // namespace

int main() {
  ProductionTraceTagsAreNotAuthorization();
  ExplicitBootstrapAndFixtureFallbackAreFenced();
  MaterializedContextIsAuthority();
  std::cout << "engine_listener_materialized_authorization_conformance=passed\n";
  return EXIT_SUCCESS;
}
