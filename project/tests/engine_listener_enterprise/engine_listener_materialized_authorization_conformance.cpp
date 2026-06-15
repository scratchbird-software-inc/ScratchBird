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
  api::EngineUuid second_principal = MakeUuid(UuidKind::principal, 8);
  api::EngineUuid reporting_role = MakeUuid(UuidKind::object, 9);
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

api::EngineUuid GroupUuid(u64 index) {
  api::EngineUuid out;
  const std::string suffix = std::to_string(1000 + index);
  out.canonical =
      "019f0320-0000-7000-8000-" + std::string(12 - suffix.size(), '0') + suffix;
  return out;
}

api::EngineUuid GrantUuid(u64 index) {
  api::EngineUuid out;
  const std::string suffix = std::to_string(2000 + index);
  out.canonical =
      "019f0320-0000-7000-8000-" + std::string(12 - suffix.size(), '0') + suffix;
  return out;
}

bool HasSubject(const api::EngineMaterializedAuthorizationContext& context,
                const api::EngineUuid& subject_uuid,
                std::string_view subject_kind) {
  for (const auto& subject : context.effective_subjects) {
    if (subject.subject_uuid.canonical == subject_uuid.canonical &&
        subject.subject_kind == subject_kind) {
      return true;
    }
  }
  return false;
}

bool HasDiagnosticCode(const api::DurableAuthorizationMaterializeResult& result,
                       std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) { return true; }
  }
  return false;
}

void DumpDiagnostics(const api::DurableAuthorizationMaterializeResult& result) {
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
  }
}

api::DurableAuthorizationState RecursiveGroupState(const FixtureIds& ids,
                                                   std::size_t group_depth) {
  api::DurableAuthorizationState state;
  state.authority_uuid = ids.authority;
  state.security_epoch = 11;
  state.policy_epoch = 12;
  state.catalog_generation_id = 13;
  state.principals.push_back({ids.principal, "user", true, 11});
  state.principals.push_back({ids.second_principal, "user", true, 11});
  state.roles.push_back({ids.reporting_role, true, 11});

  for (std::size_t index = 0; index < group_depth; ++index) {
    state.groups.push_back({GroupUuid(index), true, 11});
  }
  state.memberships.push_back(
      {ids.principal, "principal", GroupUuid(0), "group", true, 11});
  for (std::size_t index = 0; index + 1 < group_depth; ++index) {
    state.memberships.push_back({GroupUuid(index),
                                 "group",
                                 GroupUuid(index + 1),
                                 "group",
                                 true,
                                 11});
  }
  state.memberships.push_back({GroupUuid(group_depth - 1),
                               "group",
                               ids.reporting_role,
                               "role",
                               true,
                               11});

  state.grants.push_back({GrantUuid(0),
                          GroupUuid(group_depth - 1),
                          "group",
                          ids.table,
                          "SELECT",
                          false,
                          true,
                          11});
  state.grants.push_back({GrantUuid(1),
                          ids.reporting_role,
                          "role",
                          ids.table,
                          "UPDATE",
                          false,
                          true,
                          11});
  state.grants.push_back({GrantUuid(2),
                          ids.principal,
                          "principal",
                          ids.other_table,
                          "DELETE",
                          false,
                          true,
                          11});
  return state;
}

api::DurableAuthorizationMaterializeRequest MaterializeRequest(
    const FixtureIds& ids,
    api::EngineUuid principal = {}) {
  api::DurableAuthorizationMaterializeRequest request;
  request.principal_uuid =
      principal.canonical.empty() ? ids.principal : std::move(principal);
  request.observed_security_epoch = 11;
  request.observed_policy_epoch = 12;
  request.observed_catalog_generation_id = 13;
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

void RecursiveGroupAuthorizationDepthAndVariationsAreEngineOwned() {
  constexpr std::size_t kCertifiedGroupDepth = 128;
  const FixtureIds ids;
  auto state = RecursiveGroupState(ids, kCertifiedGroupDepth);
  const auto materialized =
      api::MaterializeDurableAuthorizationContext(state, MaterializeRequest(ids));
  if (!materialized.ok) { DumpDiagnostics(materialized); }
  Require(materialized.ok, "recursive group materialization failed");
  Require(materialized.context.effective_subjects.size() ==
              kCertifiedGroupDepth + 2,
          "recursive group subject expansion depth drifted");
  Require(HasSubject(materialized.context, ids.principal, "principal"),
          "principal subject missing from materialized context");
  Require(HasSubject(materialized.context, GroupUuid(0), "group"),
          "first group subject missing from materialized context");
  Require(HasSubject(materialized.context,
                     GroupUuid(kCertifiedGroupDepth - 1),
                     "group"),
          "top group subject missing from materialized context");
  Require(HasSubject(materialized.context, ids.reporting_role, "role"),
          "role inherited through group chain missing from materialized context");

  auto context = BaseContext(ids);
  context.authorization_context = materialized.context;
  Require(api::SecurityContextHasRight(context, "SELECT", ids.table.canonical),
          "top-group SELECT grant did not authorize through recursive groups");
  Require(api::SecurityContextHasRight(context, "UPDATE", ids.table.canonical),
          "role grant inherited through group chain did not authorize");
  Require(api::SecurityContextHasRight(context,
                                       "DELETE",
                                       ids.other_table.canonical),
          "direct principal grant did not authorize");
  Require(!api::SecurityContextHasRight(context,
                                        "SELECT",
                                        ids.other_table.canonical),
          "target-specific recursive grant authorized the wrong object");

  const auto select =
      api::EngineAuthorize(AuthorizeRequest(context, ids.table, "SELECT"));
  Require(select.ok && select.authorized,
          "EngineAuthorize rejected recursive top-group SELECT grant");

  const auto second =
      api::MaterializeDurableAuthorizationContext(
          state, MaterializeRequest(ids, ids.second_principal));
  Require(second.ok, "second principal materialization failed");
  auto second_context = BaseContext(ids);
  second_context.principal_uuid = ids.second_principal;
  second_context.authorization_context = second.context;
  const auto denied =
      api::EngineAuthorize(AuthorizeRequest(second_context, ids.table, "SELECT"));
  Require(!denied.ok && !denied.authorized,
          "ungranted second principal inherited recursive group grant");
  Require(HasDiagnosticCode(denied, "SECURITY.AUTHORIZATION.DENIED"),
          "missing denied diagnostic for ungranted principal");

  auto replay_context = BaseContext(ids);
  replay_context.principal_uuid = ids.second_principal;
  replay_context.authorization_context = materialized.context;
  const auto replay =
      api::EngineAuthorize(AuthorizeRequest(replay_context, ids.table, "SELECT"));
  Require(!replay.ok && !replay.authorized,
          "cross-principal replay of materialized authorization context succeeded");
  Require(HasDiagnosticCode(replay, "SECURITY.AUTHORIZATION.PRINCIPAL_MISMATCH"),
          "cross-principal replay diagnostic drifted");
}

void RecursiveGroupAuthorizationFailsClosedOnDenyCycleAndEpochDrift() {
  constexpr std::size_t kDepth = 8;
  const FixtureIds ids;

  auto deny_state = RecursiveGroupState(ids, kDepth);
  deny_state.grants.push_back({ids.deny_grant,
                               ids.principal,
                               "principal",
                               ids.table,
                               "SELECT",
                               true,
                               true,
                               11});
  const auto deny_materialized =
      api::MaterializeDurableAuthorizationContext(deny_state,
                                                  MaterializeRequest(ids));
  Require(deny_materialized.ok, "deny materialization failed");
  auto deny_context = BaseContext(ids);
  deny_context.authorization_context = deny_materialized.context;
  const auto denied =
      api::EngineAuthorize(AuthorizeRequest(deny_context, ids.table, "SELECT"));
  Require(!denied.ok && !denied.authorized,
          "direct deny did not override recursive group allow");
  Require(HasDiagnosticCode(denied, "SECURITY.AUTHORIZATION.DENIED"),
          "direct deny diagnostic drifted");

  auto cycle_state = RecursiveGroupState(ids, 2);
  cycle_state.memberships.push_back(
      {GroupUuid(1), "group", GroupUuid(0), "group", true, 11});
  const auto cycle =
      api::MaterializeDurableAuthorizationContext(cycle_state,
                                                  MaterializeRequest(ids));
  Require(!cycle.ok, "membership cycle did not fail closed");
  Require(HasDiagnosticCode(cycle, "SECURITY.AUTHORIZATION.MEMBERSHIP_CYCLE"),
          "membership cycle diagnostic drifted");

  auto stale_state = RecursiveGroupState(ids, kDepth);
  stale_state.memberships[2].security_epoch = 10;
  const auto stale =
      api::MaterializeDurableAuthorizationContext(stale_state,
                                                  MaterializeRequest(ids));
  Require(!stale.ok, "stale membership epoch did not fail closed");
  Require(HasDiagnosticCode(stale, "SECURITY.CONTEXT.EXPIRED"),
          "stale membership diagnostic drifted");
}

}  // namespace

int main() {
  ProductionTraceTagsAreNotAuthorization();
  ExplicitBootstrapAndFixtureFallbackAreFenced();
  MaterializedContextIsAuthority();
  RecursiveGroupAuthorizationDepthAndVariationsAreEngineOwned();
  RecursiveGroupAuthorizationFailsClosedOnDenyCycleAndEpochDrift();
  std::cout << "engine_listener_materialized_authorization_conformance=passed\n";
  return EXIT_SUCCESS;
}
