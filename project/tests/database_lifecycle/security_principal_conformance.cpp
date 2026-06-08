// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "security/security_principal_lifecycle.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

namespace engine_api = scratchbird::engine::internal_api;

constexpr std::string_view kDatabaseUuid = "019e1a14-013a-7000-8000-0000000000ae";
constexpr std::string_view kAdminPrincipal = "principal-sysarch";
constexpr std::string_view kUserAlice = "principal-alice";
constexpr std::string_view kUserBob = "principal-bob";
constexpr std::string_view kDefiner = "principal-definer";
constexpr std::string_view kRoleReader = "role-reader";
constexpr std::string_view kGroupAnalysts = "group-analysts";
constexpr std::string_view kTableOrders = "table-orders";
constexpr std::string_view kProtectedSecret = "credential-secret-dblc013ae";
constexpr std::string_view kPlaintextSecret = "CorrectHorseBatteryStaple-DBLC013AE";

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) { Fail(message); }
}

std::filesystem::path MakeTempDir() {
  std::string tmpl = "/tmp/sb_dblc013ae_security_principal.XXXXXX";
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  Require(made != nullptr, "mkdtemp failed for DBLC-013AE security-principal test");
  return std::filesystem::path(made);
}

engine_api::EngineRequestContext Context(const std::filesystem::path& database_path,
                                         std::uint64_t tx,
                                         std::string principal,
                                         std::uint64_t visible_through = 0,
                                         bool admin = false) {
  engine_api::EngineRequestContext context;
  context.trust_mode = engine_api::EngineTrustMode::server_isolated;
  context.database_path = database_path.string();
  context.database_uuid.canonical = std::string(kDatabaseUuid);
  context.local_transaction_id = tx;
  context.transaction_uuid.canonical = "txn-" + std::to_string(tx);
  context.principal_uuid.canonical = std::move(principal);
  context.security_context_present = true;
  context.snapshot_visible_through_local_transaction_id = visible_through;
  if (admin) {
    context.trace_tags.push_back("security.bootstrap");
    context.trace_tags.push_back("group:ROOT");
    context.trace_tags.push_back("group:SEC");
    context.trace_tags.push_back("group:AUD");
  }
  return context;
}

engine_api::EngineRequestContext NoAuthorityContext(const std::filesystem::path& database_path,
                                                    std::uint64_t tx) {
  engine_api::EngineRequestContext context;
  context.trust_mode = engine_api::EngineTrustMode::server_isolated;
  context.database_path = database_path.string();
  context.database_uuid.canonical = std::string(kDatabaseUuid);
  context.local_transaction_id = tx;
  context.transaction_uuid.canonical = "txn-" + std::to_string(tx);
  context.principal_uuid.canonical = "principal-no-authority";
  return context;
}

bool HasDiagnostic(const engine_api::EngineApiResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) { return true; }
    if (diagnostic.detail == code) { return true; }
  }
  return false;
}

std::string FlattenResult(const engine_api::EngineApiResult& result) {
  std::ostringstream out;
  out << result.operation_id << '\n';
  for (const auto& diagnostic : result.diagnostics) {
    out << diagnostic.code << '\n' << diagnostic.message_key << '\n'
        << diagnostic.detail << '\n';
  }
  for (const auto& evidence : result.evidence) {
    out << evidence.evidence_kind << '\n' << evidence.evidence_id << '\n';
  }
  for (const auto& row : result.result_shape.rows) {
    for (const auto& field : row.fields) {
      out << field.first << '=' << field.second.encoded_value << '\n';
    }
  }
  return out.str();
}

void RequireNoLeak(const engine_api::EngineApiResult& result, std::string_view forbidden) {
  const auto flattened = FlattenResult(result);
  Require(flattened.find(forbidden) == std::string::npos,
          "DBLC-013AE protected material leaked through result or diagnostic output");
}

template <typename TResult>
void RequireOk(const TResult& result, std::string_view message) {
  if (!result.ok) {
    for (const auto& diagnostic : result.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
    }
  }
  Require(result.ok, message);
}

engine_api::EngineSecurityCreatePrincipalResult CreatePrincipal(
    const std::filesystem::path& database_path,
    std::uint64_t tx,
    std::string_view principal_uuid,
    std::string_view name,
    std::string_view protected_ref = {}) {
  engine_api::EngineSecurityCreatePrincipalRequest request;
  request.context = Context(database_path, tx, std::string(kAdminPrincipal), tx, true);
  request.principal_uuid = std::string(principal_uuid);
  request.principal_name = std::string(name);
  request.credential_protected_material_ref = std::string(protected_ref);
  request.option_envelopes.push_back("principal_authority:engine");
  return engine_api::EngineSecurityCreatePrincipal(request);
}

engine_api::EngineSecurityCreateRoleResult CreateRole(const std::filesystem::path& database_path,
                                                      std::uint64_t tx) {
  engine_api::EngineSecurityCreateRoleRequest request;
  request.context = Context(database_path, tx, std::string(kAdminPrincipal), tx, true);
  request.role_uuid = std::string(kRoleReader);
  request.role_name = "reader";
  request.option_envelopes.push_back("role_authority:engine");
  return engine_api::EngineSecurityCreateRole(request);
}

engine_api::EngineSecurityCreateGroupResult CreateGroup(const std::filesystem::path& database_path,
                                                        std::uint64_t tx) {
  engine_api::EngineSecurityCreateGroupRequest request;
  request.context = Context(database_path, tx, std::string(kAdminPrincipal), tx, true);
  request.group_uuid = std::string(kGroupAnalysts);
  request.group_name = "analysts";
  request.external_authority_ref = std::string("ldap-ref:") + std::string(kProtectedSecret);
  request.option_envelopes.push_back("group_authority:engine");
  return engine_api::EngineSecurityCreateGroup(request);
}

engine_api::EngineSecurityGrantMembershipResult GrantMembership(
    const std::filesystem::path& database_path,
    std::uint64_t tx,
    std::string_view container_uuid,
    std::string_view container_kind) {
  engine_api::EngineSecurityGrantMembershipRequest request;
  request.context = Context(database_path, tx, std::string(kAdminPrincipal), tx, true);
  request.member_principal_uuid = std::string(kUserAlice);
  request.container_uuid = std::string(container_uuid);
  request.container_kind = std::string(container_kind);
  request.option_envelopes.push_back("security_authority:engine");
  return engine_api::EngineSecurityGrantMembership(request);
}

engine_api::EngineSecurityGrantPrivilegeResult GrantPrivilege(
    const std::filesystem::path& database_path,
    std::uint64_t tx,
    std::string_view grantee_uuid,
    std::string_view grantee_kind,
    std::string_view privilege,
    std::string_view target_uuid = kTableOrders) {
  engine_api::EngineSecurityGrantPrivilegeRequest request;
  request.context = Context(database_path, tx, std::string(kAdminPrincipal), tx, true);
  request.grantee_uuid = std::string(grantee_uuid);
  request.grantee_kind = std::string(grantee_kind);
  request.target_object_uuid = std::string(target_uuid);
  request.target_object_kind = "table";
  request.privilege = std::string(privilege);
  request.option_envelopes.push_back("grant_authority:engine");
  return engine_api::EngineSecurityGrantPrivilege(request);
}

engine_api::EngineSecurityRevokePrivilegeResult RevokePrivilege(
    const std::filesystem::path& database_path,
    std::uint64_t tx,
    std::string_view grantee_uuid,
    std::string_view privilege,
    std::string_view target_uuid = kTableOrders) {
  engine_api::EngineSecurityRevokePrivilegeRequest request;
  request.context = Context(database_path, tx, std::string(kAdminPrincipal), tx, true);
  request.grantee_uuid = std::string(grantee_uuid);
  request.target_object_uuid = std::string(target_uuid);
  request.privilege = std::string(privilege);
  request.option_envelopes.push_back("grant_authority:engine");
  return engine_api::EngineSecurityRevokePrivilege(request);
}

engine_api::EngineSecurityEvaluatePrivilegeResult EvaluatePrivilege(
    const std::filesystem::path& database_path,
    std::uint64_t tx,
    std::uint64_t visible_through,
    std::string_view principal_uuid,
    std::string_view privilege,
    std::string_view target_uuid = kTableOrders) {
  engine_api::EngineSecurityEvaluatePrivilegeRequest request;
  request.context = Context(database_path, tx, std::string(principal_uuid), visible_through);
  request.principal_uuid = std::string(principal_uuid);
  request.target_object_uuid = std::string(target_uuid);
  request.target_object_kind = "table";
  request.privilege = std::string(privilege);
  request.option_envelopes.push_back("authorization_authority:engine");
  return engine_api::EngineSecurityEvaluatePrivilege(request);
}

void TestPrincipalRoleGroupAndProtectedMaterial(const std::filesystem::path& database_path) {
  const auto alice = CreatePrincipal(database_path,
                                     1,
                                     kUserAlice,
                                     "alice",
                                     std::string("kms-ref:") + std::string(kProtectedSecret));
  RequireOk(alice, "DBLC-013AE user principal create failed");
  Require(alice.principal_created && !alice.plaintext_material_stored,
          "DBLC-013AE principal create did not preserve protected-material boundary");
  RequireNoLeak(alice, kProtectedSecret);

  const auto bob = CreatePrincipal(database_path, 2, kUserBob, "bob");
  RequireOk(bob, "DBLC-013AE second principal create failed");
  const auto definer = CreatePrincipal(database_path, 3, kDefiner, "definer");
  RequireOk(definer, "DBLC-013AE definer principal create failed");
  RequireOk(CreateRole(database_path, 4), "DBLC-013AE role create failed");
  const auto group = CreateGroup(database_path, 5);
  RequireOk(group, "DBLC-013AE group create failed");
  RequireNoLeak(group, kProtectedSecret);
  RequireOk(GrantMembership(database_path, 6, kRoleReader, "role"),
            "DBLC-013AE role membership grant failed");
  RequireOk(GrantMembership(database_path, 7, kGroupAnalysts, "group"),
            "DBLC-013AE group membership grant failed");

  engine_api::EngineSecurityCreatePrincipalRequest plaintext;
  plaintext.context = Context(database_path, 8, std::string(kAdminPrincipal), 8, true);
  plaintext.principal_uuid = "principal-plaintext-refused";
  plaintext.principal_name = "plaintext_refused";
  plaintext.credential_protected_material_ref =
      std::string("password=") + std::string(kPlaintextSecret);
  plaintext.option_envelopes.push_back("principal_authority:engine");
  const auto refused = engine_api::EngineSecurityCreatePrincipal(plaintext);
  Require(!refused.ok &&
              HasDiagnostic(refused,
                            engine_api::kSecurityPrincipalDiagnosticProtectedMaterialPlaintextRefused),
          "DBLC-013AE plaintext credential material was accepted");
  RequireNoLeak(refused, kPlaintextSecret);
}

void TestDefaultDenyGrantRevokeAndMgaVisibility(const std::filesystem::path& database_path) {
  const auto default_deny = EvaluatePrivilege(database_path, 20, 20, kUserAlice, "SELECT");
  Require(!default_deny.ok &&
              HasDiagnostic(default_deny,
                            engine_api::kSecurityPrincipalDiagnosticDefaultDeny),
          "DBLC-013AE default-deny privilege check did not fail closed");

  const auto role_grant = GrantPrivilege(database_path, 21, kRoleReader, "role", "SELECT");
  RequireOk(role_grant, "DBLC-013AE role privilege grant failed");
  Require(role_grant.cache_invalidation_epoch > 0,
          "DBLC-013AE grant did not publish cache invalidation");

  const auto before_grant = EvaluatePrivilege(database_path, 22, 20, kUserAlice, "SELECT");
  Require(!before_grant.ok &&
              HasDiagnostic(before_grant,
                            engine_api::kSecurityPrincipalDiagnosticGrantNotVisible),
          "DBLC-013AE grant visible before MGA snapshot boundary");

  const auto after_grant = EvaluatePrivilege(database_path, 23, 21, kUserAlice, "SELECT");
  Require(after_grant.ok && after_grant.authorized && !after_grant.matched_grant_uuids.empty(),
          "DBLC-013AE role grant did not authorize after MGA visibility boundary");

  const auto group_grant = GrantPrivilege(database_path, 24, kGroupAnalysts, "group", "UPDATE");
  RequireOk(group_grant, "DBLC-013AE group privilege grant failed");
  const auto group_allow = EvaluatePrivilege(database_path, 25, 24, kUserAlice, "UPDATE");
  Require(group_allow.ok && group_allow.authorized,
          "DBLC-013AE group grant did not contribute additive rights");

  const auto revoked = RevokePrivilege(database_path, 26, kRoleReader, "SELECT");
  RequireOk(revoked, "DBLC-013AE privilege revoke failed");
  Require(revoked.cache_invalidation_epoch > role_grant.cache_invalidation_epoch,
          "DBLC-013AE revoke did not advance cache invalidation");

  const auto pre_revoke_snapshot = EvaluatePrivilege(database_path, 27, 25, kUserAlice, "SELECT");
  Require(pre_revoke_snapshot.ok && pre_revoke_snapshot.authorized,
          "DBLC-013AE revoke affected an older MGA snapshot");
  const auto post_revoke_snapshot = EvaluatePrivilege(database_path, 28, 26, kUserAlice, "SELECT");
  Require(!post_revoke_snapshot.ok &&
              HasDiagnostic(post_revoke_snapshot,
                            engine_api::kSecurityPrincipalDiagnosticDefaultDeny),
          "DBLC-013AE revoked privilege remained visible");

  engine_api::EngineSecurityValidatePolicyCacheRequest stale_cache;
  stale_cache.context = Context(database_path, 29, std::string(kUserAlice), 26);
  stale_cache.observed_policy_generation = role_grant.security_generation;
  stale_cache.observed_cache_invalidation_epoch = role_grant.cache_invalidation_epoch;
  stale_cache.option_envelopes.push_back("policy_authority:engine");
  const auto stale_cache_result = engine_api::EngineSecurityValidatePolicyCache(stale_cache);
  Require(!stale_cache_result.ok && stale_cache_result.stale_policy_refused &&
              HasDiagnostic(stale_cache_result,
                            engine_api::kSecurityPrincipalDiagnosticCacheStale),
          "DBLC-013AE stale authorization policy cache was accepted");

  engine_api::EngineSecurityValidatePolicyCacheRequest current_cache;
  current_cache.context = Context(database_path, 30, std::string(kUserAlice), 26);
  current_cache.observed_policy_generation = revoked.security_generation;
  current_cache.observed_cache_invalidation_epoch = revoked.cache_invalidation_epoch;
  current_cache.option_envelopes.push_back("policy_authority:engine");
  const auto current_cache_result = engine_api::EngineSecurityValidatePolicyCache(current_cache);
  Require(current_cache_result.ok && current_cache_result.cache_valid,
          "DBLC-013AE current authorization policy cache was rejected");
}

void TestRowSecurityAndDefinerRightsCache(const std::filesystem::path& database_path) {
  const auto definer_grant = GrantPrivilege(database_path, 40, kDefiner, "principal", "SELECT");
  RequireOk(definer_grant, "DBLC-013AE definer grant failed");

  engine_api::EngineSecurityPrimeDefinerRightsCacheRequest prime;
  prime.context = Context(database_path, 41, std::string(kAdminPrincipal), 41, true);
  prime.definer_principal_uuid = std::string(kDefiner);
  prime.target_object_uuid = std::string(kTableOrders);
  prime.privilege = "SELECT";
  prime.option_envelopes.push_back("definer_rights_authority:engine");
  const auto primed = engine_api::EngineSecurityPrimeDefinerRightsCache(prime);
  RequireOk(primed, "DBLC-013AE definer-rights cache prime failed");
  Require(primed.cached && !primed.cache_key.empty(),
          "DBLC-013AE definer-rights cache did not return a cache key");

  engine_api::EngineSecurityValidateDefinerRightsCacheRequest validate;
  validate.context = Context(database_path, 42, std::string(kUserAlice), 41);
  validate.cache_key = primed.cache_key;
  validate.observed_policy_generation = primed.policy_generation;
  validate.observed_cache_invalidation_epoch = definer_grant.cache_invalidation_epoch;
  validate.option_envelopes.push_back("definer_rights_authority:engine");
  const auto cache_valid = engine_api::EngineSecurityValidateDefinerRightsCache(validate);
  Require(cache_valid.ok && cache_valid.cache_valid,
          "DBLC-013AE fresh definer-rights cache was rejected");

  const auto definer_revoked = RevokePrivilege(database_path, 43, kDefiner, "SELECT");
  RequireOk(definer_revoked, "DBLC-013AE definer revoke failed");
  validate.context = Context(database_path, 44, std::string(kUserAlice), 43);
  const auto cache_stale = engine_api::EngineSecurityValidateDefinerRightsCache(validate);
  Require(!cache_stale.ok && cache_stale.stale_policy_refused &&
              HasDiagnostic(cache_stale,
                            engine_api::kSecurityPrincipalDiagnosticCacheStale),
          "DBLC-013AE stale definer-rights cache was accepted after revoke");

  engine_api::EngineSecurityPutRowPolicyRequest put_policy;
  put_policy.context = Context(database_path, 50, std::string(kAdminPrincipal), 50, true);
  put_policy.policy_uuid = "row-policy-orders-owner";
  put_policy.target_object_uuid = std::string(kTableOrders);
  put_policy.target_object_kind = "table";
  put_policy.policy_effect = "allow_owner";
  put_policy.predicate_envelope = "owner_uuid == effective_user_uuid";
  put_policy.option_envelopes.push_back("row_security_authority:engine");
  const auto policy = engine_api::EngineSecurityPutRowPolicy(put_policy);
  RequireOk(policy, "DBLC-013AE row policy put failed");

  engine_api::EngineSecurityEvaluateRowPolicyRequest row_eval;
  row_eval.context = Context(database_path, 51, std::string(kUserAlice), 50);
  row_eval.principal_uuid = std::string(kUserAlice);
  row_eval.target_object_uuid = std::string(kTableOrders);
  row_eval.row_owner_principal_uuid = std::string(kUserAlice);
  row_eval.observed_policy_generation = policy.policy_generation;
  row_eval.option_envelopes.push_back("row_security_authority:engine");
  const auto row_allow = engine_api::EngineSecurityEvaluateRowPolicy(row_eval);
  Require(row_allow.ok && row_allow.row_visible,
          "DBLC-013AE owner row-security hook denied visible row");

  row_eval.context = Context(database_path, 52, std::string(kUserBob), 50);
  row_eval.principal_uuid = std::string(kUserBob);
  const auto row_deny = engine_api::EngineSecurityEvaluateRowPolicy(row_eval);
  Require(!row_deny.ok && !row_deny.row_visible &&
              HasDiagnostic(row_deny, engine_api::kSecurityPrincipalDiagnosticAccessDenied),
          "DBLC-013AE row-security hook allowed non-owner row");

  row_eval.context = Context(database_path, 53, std::string(kUserAlice), 50);
  row_eval.principal_uuid = std::string(kUserAlice);
  row_eval.row_owner_principal_uuid = std::string(kUserAlice);
  row_eval.observed_policy_generation = policy.policy_generation - 1;
  const auto stale_row_policy = engine_api::EngineSecurityEvaluateRowPolicy(row_eval);
  Require(!stale_row_policy.ok && stale_row_policy.stale_policy_refused &&
              HasDiagnostic(stale_row_policy,
                            engine_api::kSecurityPrincipalDiagnosticPolicyStale),
          "DBLC-013AE stale row policy generation was accepted");
}

void TestAuditEvidenceAndAuthorityDiagnostics(const std::filesystem::path& database_path) {
  engine_api::EngineSecurityInspectAuditRequest inspect;
  inspect.context = Context(database_path, 60, std::string(kAdminPrincipal), 60, true);
  inspect.option_envelopes.push_back("security_authority:engine");
  const auto audit = engine_api::EngineSecurityInspectAudit(inspect);
  RequireOk(audit, "DBLC-013AE audit inspect failed");
  Require(audit.protected_material_redacted && audit.audit_records.size() >= 8,
          "DBLC-013AE audit evidence was not persisted for security mutations");
  RequireNoLeak(audit, kProtectedSecret);

  engine_api::EngineSecurityCreatePrincipalRequest no_authority;
  no_authority.context = NoAuthorityContext(database_path, 61);
  no_authority.principal_uuid = "principal-denied";
  no_authority.principal_name = "denied";
  const auto denied = engine_api::EngineSecurityCreatePrincipal(no_authority);
  Require(!denied.ok &&
              HasDiagnostic(denied,
                            engine_api::kSecurityPrincipalDiagnosticAuthorityRequired),
          "DBLC-013AE missing security authority did not produce stable diagnostic");

  engine_api::EngineSecurityCreatePrincipalRequest parser_authority;
  parser_authority.context = Context(database_path, 62, std::string(kAdminPrincipal), 62, true);
  parser_authority.principal_uuid = "principal-parser-bypass";
  parser_authority.principal_name = "parser_bypass";
  parser_authority.option_envelopes.push_back("principal_authority:parser");
  const auto bypass = engine_api::EngineSecurityCreatePrincipal(parser_authority);
  Require(!bypass.ok &&
              HasDiagnostic(bypass,
                            engine_api::kSecurityPrincipalDiagnosticAuthorityBypassRefused),
          "DBLC-013AE parser-side security authority was accepted");
}

}  // namespace

int main() {
  const auto temp_dir = MakeTempDir();
  const auto database_path = temp_dir / "dblc013ae.sbdb";
  std::ofstream touch(database_path, std::ios::app);
  Require(static_cast<bool>(touch), "DBLC-013AE database fixture create failed");

  TestPrincipalRoleGroupAndProtectedMaterial(database_path);
  TestDefaultDenyGrantRevokeAndMgaVisibility(database_path);
  TestRowSecurityAndDefinerRightsCache(database_path);
  TestAuditEvidenceAndAuthorityDiagnostics(database_path);

  std::filesystem::remove_all(temp_dir);
  return EXIT_SUCCESS;
}
