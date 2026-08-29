// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "security/authorization_api.hpp"
#include "security/grant_api.hpp"
#include "security/security_model.hpp"
#include "uuid.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace uuid = scratchbird::core::uuid;

using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::u64;

constexpr u64 kBaseMillis = 1770600000000ull;

bool Expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
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
  api::EngineUuid analyst_group = MakeUuid(UuidKind::object, 4);
  api::EngineUuid reader_group = MakeUuid(UuidKind::object, 5);
  api::EngineUuid reader_role = MakeUuid(UuidKind::object, 6);
  api::EngineUuid admin_role = MakeUuid(UuidKind::object, 7);
  api::EngineUuid table = MakeUuid(UuidKind::object, 8);
  api::EngineUuid other_table = MakeUuid(UuidKind::object, 9);
  api::EngineUuid domain = MakeUuid(UuidKind::object, 10);
};

api::DurableAuthorizationMembershipRecord Membership(
    api::EngineUuid member,
    std::string member_kind,
    api::EngineUuid parent,
    std::string parent_kind) {
  api::DurableAuthorizationMembershipRecord edge;
  edge.member_uuid = std::move(member);
  edge.member_kind = std::move(member_kind);
  edge.parent_uuid = std::move(parent);
  edge.parent_kind = std::move(parent_kind);
  edge.security_epoch = 7;
  return edge;
}

api::DurableAuthorizationGrantRecord Grant(api::EngineUuid subject,
                                           std::string subject_kind,
                                           api::EngineUuid target,
                                           std::string right,
                                           bool deny,
                                           u64 id_offset) {
  api::DurableAuthorizationGrantRecord grant;
  grant.grant_uuid = MakeUuid(UuidKind::object, id_offset);
  grant.subject_uuid = std::move(subject);
  grant.subject_kind = std::move(subject_kind);
  grant.target_uuid = std::move(target);
  grant.right = std::move(right);
  grant.deny = deny;
  grant.security_epoch = 7;
  return grant;
}

api::DurableAuthorizationPolicyRecord Policy(api::EngineUuid subject,
                                             std::string subject_kind,
                                             api::EngineUuid target,
                                             std::string right,
                                             std::string kind,
                                             bool deny,
                                             bool recheck,
                                             u64 id_offset) {
  api::DurableAuthorizationPolicyRecord policy;
  policy.policy_uuid = MakeUuid(UuidKind::object, id_offset);
  policy.subject_uuid = std::move(subject);
  policy.subject_kind = std::move(subject_kind);
  policy.target_uuid = std::move(target);
  policy.right = std::move(right);
  policy.policy_kind = std::move(kind);
  policy.deny = deny;
  policy.requires_runtime_recheck = recheck;
  policy.policy_epoch = 9;
  policy.canonical_policy_envelope = "sblr.policy.v1";
  if (policy.policy_kind == "row_policy") {
    policy.source_policy_generation = id_offset;
    policy.update_policy_phase = 1;
    policy.effective_policy_uuid =
        MakeUuid(UuidKind::object, id_offset + 100);
    policy.effective_policy_generation = 2;
    policy.effective_expression_uuid =
        MakeUuid(UuidKind::object, id_offset + 200);
    policy.effective_expression_generation = 3;
    policy.effective_expression_evidence_sha256.fill(0xa5);
  }
  return policy;
}

api::DurableAuthorizationState DurableState(const FixtureIds& ids) {
  api::DurableAuthorizationState state;
  state.authority_uuid = ids.authority;
  state.security_context_generation = 5;
  state.security_epoch = 7;
  state.policy_epoch = 9;
  state.catalog_generation_id = 11;
  state.principals.push_back({ids.principal, "principal", true, 7});
  state.groups.push_back({ids.analyst_group, true, 7});
  state.groups.push_back({ids.reader_group, true, 7});
  state.roles.push_back({ids.reader_role, true, 7});
  state.roles.push_back({ids.admin_role, true, 7});
  state.memberships.push_back(
      Membership(ids.principal, "principal", ids.analyst_group, "group"));
  state.memberships.push_back(
      Membership(ids.analyst_group, "group", ids.reader_group, "group"));
  state.memberships.push_back(
      Membership(ids.reader_group, "group", ids.reader_role, "role"));
  state.memberships.push_back(
      Membership(ids.principal, "principal", ids.admin_role, "role"));
  state.grants.push_back(
      Grant(ids.reader_role, "role", ids.table, "SELECT", false, 20));
  state.grants.push_back(
      Grant(ids.reader_role, "role", ids.table, "UPDATE", false, 21));
  state.grants.push_back(
      Grant(ids.reader_group, "group", ids.table, "UPDATE", true, 22));
  state.grants.push_back(
      Grant(ids.admin_role, "role", {}, "SEC_GRANT_ADMIN", false, 23));
  state.grants.push_back(
      Grant(ids.reader_role, "role", ids.domain, "DOMAIN_USE", false, 24));
  state.policies.push_back(
      Policy(ids.reader_group,
             "group",
             ids.table,
             "SELECT",
             "row_policy",
             false,
             true,
             30));
  state.policies.push_back(
      Policy(ids.reader_role,
             "role",
             ids.domain,
             "DOMAIN_USE",
             "domain_policy",
             false,
             true,
             31));
  return state;
}

api::DurableAuthorizationMaterializeRequest MaterializeRequest(
    const FixtureIds& ids) {
  api::DurableAuthorizationMaterializeRequest request;
  request.principal_uuid = ids.principal;
  request.observed_security_epoch = 7;
  request.observed_policy_epoch = 9;
  request.observed_catalog_generation_id = 11;
  return request;
}

api::EngineRequestContext RequestContext(
    const FixtureIds& ids,
    const api::EngineMaterializedAuthorizationContext& authorization_context,
    const std::filesystem::path& work_dir = {}) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = "pcr090-durable-authorization";
  context.database_uuid = ids.database;
  context.principal_uuid = ids.principal;
  context.session_uuid = MakeUuid(UuidKind::session, 40);
  context.transaction_uuid = MakeUuid(UuidKind::transaction, 41);
  context.local_transaction_id = 90;
  context.security_context_present = true;
  context.catalog_generation_id = 11;
  context.security_epoch = 7;
  context.resource_epoch = 1;
  context.authorization_context = authorization_context;
  if (!work_dir.empty()) {
    context.database_path = (work_dir / "pcr090.sbdb").string();
  }
  return context;
}

api::EngineAuthorizeRequest AuthorizeRequest(
    api::EngineRequestContext context,
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

bool MaterializesNestedGroupsAndPolicies(const std::filesystem::path& work_dir) {
  const FixtureIds ids;
  const auto state = DurableState(ids);
  const auto materialized =
      api::MaterializeDurableAuthorizationContext(state, MaterializeRequest(ids));
  bool ok = true;
  ok = Expect(materialized.ok, "durable authorization context should materialize") && ok;
  ok = Expect(materialized.context.present, "materialized context should be present") && ok;
  ok = Expect(materialized.context.security_context_generation == 5,
              "materialized context generation should remain provider-issued") && ok;
  ok = Expect(materialized.context.effective_subjects.size() == 5,
              "principal plus nested groups and roles should resolve") && ok;
  ok = Expect(materialized.context.grants.size() == 5,
              "effective durable grants should materialize") && ok;
  ok = Expect(materialized.context.policies.size() == 2,
              "row and domain policies should materialize") && ok;
  const auto& row_policy = materialized.context.policies.front();
  const auto& source_row_policy = state.policies.front();
  ok = Expect(row_policy.source_policy_generation ==
                  source_row_policy.source_policy_generation &&
                  row_policy.update_policy_phase == 1 &&
                  row_policy.effective_policy_uuid.canonical ==
                      source_row_policy.effective_policy_uuid.canonical &&
                  row_policy.effective_policy_generation ==
                      source_row_policy.effective_policy_generation &&
                  row_policy.effective_expression_uuid.canonical ==
                      source_row_policy.effective_expression_uuid.canonical &&
                  row_policy.effective_expression_generation ==
                      source_row_policy.effective_expression_generation &&
                  row_policy.effective_expression_evidence_sha256 ==
                      source_row_policy.effective_expression_evidence_sha256,
              "typed row-policy authority should survive materialization") && ok;

  auto select = api::EngineAuthorize(AuthorizeRequest(
      RequestContext(ids, materialized.context),
      ids.table,
      "SELECT"));
  ok = Expect(select.ok && select.authorized,
              "SELECT should authorize from durable role grant") && ok;
  ok = Expect(select.policy_recheck_required,
              "row policy should require runtime recheck") && ok;
  ok = Expect(select.decision == "allow_recheck_required",
              "row policy decision should be allow with recheck") && ok;
  ok = Expect(HasEvidence(select,
                          "authorization_authority",
                          "materialized_authorization_context"),
              "authorization authority evidence should name materialized context") && ok;

  auto tagged_context = RequestContext(ids, materialized.context);
  tagged_context.trace_tags = {
      "security.bootstrap",
      "group:ROOT",
      "deny:SELECT",
      "security_context:expired"};
  auto tag_ignored = api::EngineAuthorize(AuthorizeRequest(
      tagged_context,
      ids.table,
      "SELECT"));
  ok = Expect(tag_ignored.ok && tag_ignored.authorized,
              "EngineAuthorize should ignore trace tags and consume only materialized context") && ok;

  auto other_target = api::EngineAuthorize(AuthorizeRequest(
      RequestContext(ids, materialized.context),
      ids.other_table,
      "SELECT"));
  ok = Expect(!other_target.ok && !other_target.authorized,
              "target-specific grant must not authorize another object") && ok;

  auto update = api::EngineAuthorize(AuthorizeRequest(
      RequestContext(ids, materialized.context),
      ids.table,
      "UPDATE"));
  ok = Expect(!update.ok && !update.authorized,
              "explicit durable deny should override allow grant") && ok;
  ok = Expect(HasDiagnosticCode(update, "SECURITY.AUTHORIZATION.DENIED"),
              "deny override should report authorization denial") && ok;

  auto domain = api::EngineAuthorize(AuthorizeRequest(
      RequestContext(ids, materialized.context),
      ids.domain,
      "DOMAIN_USE"));
  ok = Expect(domain.ok && domain.authorized,
              "domain grant should authorize") && ok;
  ok = Expect(domain.policy_recheck_required,
              "domain policy should require runtime recheck") && ok;

  auto legacy_shortcut = AuthorizeRequest(api::EngineRequestContext{},
                                          ids.table,
                                          "SEC_GRANT_ADMIN");
  legacy_shortcut.context.security_context_present = true;
  legacy_shortcut.context.principal_uuid = ids.principal;
  legacy_shortcut.context.security_epoch = 7;
  legacy_shortcut.context.catalog_generation_id = 11;
  legacy_shortcut.context.trace_tags.push_back("security.bootstrap");
  legacy_shortcut.context.trace_tags.push_back("group:ROOT");
  const auto shortcut = api::EngineAuthorize(legacy_shortcut);
  ok = Expect(!shortcut.ok && !shortcut.authorized,
              "EngineAuthorize must not accept bootstrap or group tags without materialized context") && ok;
  ok = Expect(HasDiagnosticCode(shortcut,
                                "SECURITY.AUTHORIZATION.CONTEXT_REQUIRED"),
              "missing materialized context should fail with stable diagnostic") && ok;

  api::EngineGrantRightRequest grant;
  grant.context = RequestContext(ids, materialized.context, work_dir);
  grant.operation_id = "security.grant_right";
  grant.target_object.uuid = MakeUuid(UuidKind::object, 60);
  grant.target_object.object_kind = "grant";
  grant.localized_names.push_back(
      {"en", "default", "grant", "pcr090_grant", true});
  grant.option_envelopes.push_back("right:SELECT");
  const auto grant_result = api::EngineGrantRight(grant);
  ok = Expect(grant_result.ok,
              "EngineGrantRight should authorize through materialized SEC_GRANT_ADMIN") && ok;
  ok = Expect(HasEvidence(grant_result,
                          "security_grant_admin",
                          ids.principal.canonical),
              "grant API should preserve grant admin evidence") && ok;
  return ok;
}

bool RejectsStaleEpoch() {
  const FixtureIds ids;
  auto request = MaterializeRequest(ids);
  request.observed_security_epoch = 8;
  const auto stale =
      api::MaterializeDurableAuthorizationContext(DurableState(ids), request);
  return Expect(!stale.ok, "stale observed security epoch should fail closed") &&
         Expect(!stale.diagnostics.empty() &&
                    stale.diagnostics.front().code == "SECURITY.CONTEXT.EXPIRED",
                "stale epoch should report SECURITY.CONTEXT.EXPIRED");
}

bool RejectsMembershipCycle() {
  const FixtureIds ids;
  auto state = DurableState(ids);
  state.memberships.push_back(
      Membership(ids.reader_group, "group", ids.analyst_group, "group"));
  const auto cyclic =
      api::MaterializeDurableAuthorizationContext(state, MaterializeRequest(ids));
  return Expect(!cyclic.ok, "nested group cycle should fail closed") &&
         Expect(!cyclic.diagnostics.empty() &&
                    cyclic.diagnostics.front().code ==
                        "SECURITY.AUTHORIZATION.MEMBERSHIP_CYCLE",
                "cycle should report stable diagnostic");
}

bool RejectsMissingNativePolicyAuthority() {
  const FixtureIds ids;
  auto state = DurableState(ids);
  state.policies.front().effective_expression_generation = 0;
  const auto refused = api::MaterializeDurableAuthorizationContext(
      state, MaterializeRequest(ids));
  return Expect(!refused.ok,
                "row policy without native expression generation should fail closed") &&
         Expect(!refused.diagnostics.empty() &&
                    refused.diagnostics.front().code == "SECURITY.CONTEXT.EXPIRED" &&
                    refused.diagnostics.front().detail.find(
                        "row_policy_native_authority_missing") != std::string::npos,
                "missing native row-policy authority diagnostic drifted");
}

bool RejectsMissingSecurityContextGeneration() {
  const FixtureIds ids;
  auto state = DurableState(ids);
  state.security_context_generation = 0;
  const auto refused = api::MaterializeDurableAuthorizationContext(
      state, MaterializeRequest(ids));
  return Expect(!refused.ok,
                "missing durable security-context generation should fail closed") &&
         Expect(!refused.diagnostics.empty() &&
                    refused.diagnostics.front().code ==
                        "SECURITY.AUTHORIZATION.STATE_EPOCH_REQUIRED",
                "missing security-context generation diagnostic drifted");
}

}  // namespace

int main(int argc, char** argv) {
  const std::filesystem::path work_dir =
      argc > 1 ? std::filesystem::path(argv[1])
               : std::filesystem::temp_directory_path() /
                     "scratchbird_pcr090_authorization";
  std::error_code ignored;
  std::filesystem::remove_all(work_dir, ignored);
  std::filesystem::create_directories(work_dir);

  bool ok = true;
  ok = MaterializesNestedGroupsAndPolicies(work_dir) && ok;
  ok = RejectsStaleEpoch() && ok;
  ok = RejectsMembershipCycle() && ok;
  ok = RejectsMissingNativePolicyAuthority() && ok;
  ok = RejectsMissingSecurityContextGeneration() && ok;

  std::filesystem::remove_all(work_dir, ignored);
  return ok ? 0 : 1;
}
