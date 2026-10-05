#include "../support/binary_uuid_fixture.hpp"
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "security/security_principal_lifecycle.hpp"
#include "../support/durable_authorization_fixture.hpp"
#include "transaction/transaction_api.hpp"
#include "local_transaction_store.hpp"
#include "uuid.hpp"
#include "security/database_local_security_event_store.hpp"
#include "database_local_private_relation_locator.hpp"
#include "security_lifecycle_event_codec.hpp"
#include "hash_digest.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <stdexcept>
#include <unistd.h>
#include <vector>
#include <type_traits>

namespace {

namespace engine_api = scratchbird::engine::internal_api;

constexpr auto kDatabaseUuid = scratchbird::tests::FixtureUuidLiteral("019e1a14-013a-7000-8000-0000000000ae");
constexpr auto kAdminPrincipal = scratchbird::tests::FixtureUuid(1500, 1);
constexpr auto kUserAlice = scratchbird::tests::FixtureUuid(1500, 2);
constexpr auto kUserBob = scratchbird::tests::FixtureUuid(1500, 3);
constexpr auto kDefiner = scratchbird::tests::FixtureUuid(1500, 4);
constexpr auto kRoleReader = scratchbird::tests::FixtureUuid(1500, 5);
constexpr auto kGroupAnalysts = scratchbird::tests::FixtureUuid(1500, 6);
constexpr auto kTableOrders = scratchbird::tests::FixtureUuid(1500, 7);
constexpr std::string_view kProtectedSecret = "credential-secret-dblc013ae";
constexpr std::string_view kPlaintextSecret = "CorrectHorseBatteryStaple-DBLC013AE";

[[noreturn]] void Fail(std::string_view message) {
  throw std::runtime_error(std::string(message));
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

struct NativeFixture {
  struct BeginBoundary {
    std::uint64_t allocation = 0, commit_sequence = 0, committed_local_id = 0;
  };
  engine_api::EngineRequestContext owner;
  std::optional<engine_api::EngineRequestContext> writer;
  std::uint64_t writer_phase = 0;
  // Phase labels are test bookkeeping, never transaction IDs or watermarks.
  std::map<std::uint64_t, engine_api::EngineRequestContext> readers;
  std::map<std::uint64_t, engine_api::EngineRequestContext> committed_writers;
  std::map<engine_api::EngineUuid, BeginBoundary> admitted_boundaries;
} fixture;

void RequireNativeOk(const engine_api::EngineApiResult& result, std::string_view message) {
  if (!result.ok)
    for (const auto& diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  Require(result.ok, message);
}

void RequireInventoryState(const engine_api::EngineRequestContext& context,
                           scratchbird::transaction::mga::TransactionState expected) {
  namespace mga = scratchbird::transaction::mga;
  const auto loaded = scratchbird::storage::database::LoadLocalTransactionInventoryFromDatabase(
      context.database_path);
  Require(loaded.ok(), "DBLC-013AE native transaction inventory read failed");
  const auto found = mga::LookupLocalTransaction(
      loaded.inventory, mga::MakeLocalTransactionId(context.local_transaction_id));
  const auto admitted = fixture.admitted_boundaries.find(context.transaction_uuid);
  Require(admitted != fixture.admitted_boundaries.end(), "DBLC-013AE transaction lacks BEGIN observation");
  const auto& boundary = admitted->second;
  if (!found.ok() || found.entry.state != expected ||
      found.entry.identity.transaction_uuid.value != context.transaction_uuid ||
      found.entry.begin_visible_through_local_transaction_id != boundary.allocation ||
      found.entry.begin_visible_through_commit_sequence != boundary.commit_sequence ||
      context.snapshot_visible_through_local_transaction_id != boundary.committed_local_id ||
      !found.entry.stable_snapshot)
    std::cerr << "native inventory tx=" << context.local_transaction_id
              << " found=" << found.ok() << " expected_state=" << static_cast<int>(expected)
              << " state=" << static_cast<int>(found.entry.state)
              << " identity_match=" << (found.entry.identity.transaction_uuid.value == context.transaction_uuid)
              << " begin_allocation=" << found.entry.begin_visible_through_local_transaction_id
              << " expected_allocation=" << boundary.allocation
              << " begin_commit_sequence=" << found.entry.begin_visible_through_commit_sequence
              << " expected_commit_sequence=" << boundary.commit_sequence
              << " context_visibility=" << context.snapshot_visible_through_local_transaction_id
              << " expected_context_visibility=" << boundary.committed_local_id
              << " stable_snapshot=" << found.entry.stable_snapshot << '\n';
  Require(found.ok() && found.entry.state == expected &&
              found.entry.identity.transaction_uuid.value == context.transaction_uuid &&
              scratchbird::core::uuid::IsEngineIdentityUuid(context.transaction_uuid) &&
              found.entry.begin_visible_through_local_transaction_id == boundary.allocation &&
              found.entry.begin_visible_through_commit_sequence == boundary.commit_sequence &&
              context.snapshot_visible_through_local_transaction_id == boundary.committed_local_id &&
              found.entry.stable_snapshot,
          "DBLC-013AE transaction identity/snapshot not backed by actual inventory");
  if (expected == mga::TransactionState::committed)
    Require(found.entry.commit_sequence != 0 && found.entry.evidence_record_written,
            "DBLC-013AE native commitment lacks sequence/evidence");
}

void InitializeNativeFixture(const std::filesystem::path& database_path) {
  namespace db = scratchbird::storage::database;
  namespace platform = scratchbird::core::platform;
  db::DatabaseCreateConfig create;
  create.path = database_path.string();
  create.database_uuid = {platform::UuidKind::database, kDatabaseUuid};
  create.filespace_uuid = {platform::UuidKind::filespace, scratchbird::tests::FixtureUuid(1500, 10)};
  create.page_size = 16384;
  create.creation_unix_epoch_millis = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.require_bootstrap_principal = true;
  create.bootstrap_principal_name = "security_fixture_owner";
  create.bootstrap_credential_fingerprint =
      "local-password-pbkdf2-sha256:v1:iterations=600000:salt=0123456789abcdef0123456789abcdef:verifier=4ce03aa5a5657aaf221192635ed9c63acdb76d78a0994ec6e6ab55286e29e6a5";
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) std::cerr << created.diagnostic.diagnostic_code << ':'
                              << created.diagnostic.message_key << '\n';
  Require(created.ok(), "DBLC-013AE actual database creation failed");
  const auto bootstrap = db::ReadDatabaseBootstrapSecurityCatalog(create.path);
  Require(bootstrap.ok() && bootstrap.state.present && bootstrap.state.committed_by_inventory,
          "DBLC-013AE durable bootstrap principal unavailable");
  fixture.owner.trust_mode = engine_api::EngineTrustMode::embedded_in_process;
  fixture.owner.database_path = create.path;
  fixture.owner.database_uuid = kDatabaseUuid;
  fixture.owner.principal_uuid = bootstrap.state.principal_uuid.value;
  fixture.owner.session_uuid = scratchbird::tests::FixtureUuid(1500, 11);
  fixture.owner.catalog_generation_id = 1;
  // This embedded fixture has one fixed resource configuration, with no reload.
  // Its epoch is request metadata, not a replacement for a resource grant.
  fixture.owner.resource_epoch = 1;
  fixture.owner.name_resolution_epoch = 1;
  fixture.owner.security_context_present = true;
  scratchbird::tests::MaterializeBootstrapFixtureAuthorization(fixture.owner);
}

void CommitNative(const engine_api::EngineRequestContext& context) {
  engine_api::EngineCommitTransactionRequest request;
  request.context = context;
  RequireNativeOk(engine_api::EngineCommitTransaction(request),
                  "DBLC-013AE actual transaction commit failed");
  RequireInventoryState(context, scratchbird::transaction::mga::TransactionState::committed);
}

void CommitWriter() {
  if (!fixture.writer) return;
  CommitNative(*fixture.writer);
  fixture.committed_writers.emplace(fixture.writer_phase, *fixture.writer);
  fixture.writer.reset();
}

engine_api::EngineRequestContext Context(const std::filesystem::path& database_path,
                                         std::uint64_t phase,
                                         engine_api::EngineUuid principal,
                                         std::uint64_t snapshot_phase = 0,
                                         bool admin = false,
                                         bool commit_pending_writer = true) {
  Require(database_path == fixture.owner.database_path,
          "DBLC-013AE fixture owning database mismatch");
  if (fixture.writer && fixture.writer_phase != phase && commit_pending_writer) CommitWriter();
  if (!admin && snapshot_phase != phase) {
    const auto old = fixture.readers.find(snapshot_phase);
    if (old != fixture.readers.end()) {
      Require(old->second.principal_uuid == principal,
              "DBLC-013AE retained reader belongs to another principal");
      RequireInventoryState(old->second, scratchbird::transaction::mga::TransactionState::active);
      return old->second;
    }
    Require(snapshot_phase == 0 || fixture.committed_writers.contains(snapshot_phase),
            "DBLC-013AE requested snapshot phase was never executed");
  }
  if (admin && fixture.writer) return *fixture.writer;
  auto context = fixture.owner;
  if (admin) {
    Require(principal == kAdminPrincipal, "DBLC-013AE invalid bootstrap-owner fixture label");
    scratchbird::tests::MaterializeBootstrapFixtureAuthorization(context);
  } else {
    context.trust_mode = engine_api::EngineTrustMode::server_isolated;
    context.principal_uuid = principal;
    context.authorization_context = {};
  }
  engine_api::EngineBeginTransactionRequest begin;
  begin.context = context;
  begin.isolation_level = "snapshot";
  const auto prior = scratchbird::storage::database::LoadLocalTransactionInventoryFromDatabase(
      context.database_path);
  Require(prior.ok() && prior.inventory.next_local_transaction_id != 0 &&
              prior.inventory.next_commit_sequence != 0,
          "DBLC-013AE pre-BEGIN native inventory unavailable");
  NativeFixture::BeginBoundary boundary;
  boundary.allocation = prior.inventory.next_local_transaction_id - 1;
  boundary.commit_sequence = prior.inventory.next_commit_sequence - 1;
  for (const auto& row : prior.inventory.entries)
    if (scratchbird::transaction::mga::InventoryVisibilityState(row) ==
        scratchbird::transaction::mga::TransactionState::committed)
      boundary.committed_local_id = std::max(boundary.committed_local_id, row.identity.local_id.value);
  const auto begun = engine_api::EngineBeginTransaction(begin);
  RequireNativeOk(begun, "DBLC-013AE actual transaction begin failed");
  Require(begun.local_transaction_id == prior.inventory.next_local_transaction_id &&
              fixture.admitted_boundaries.emplace(begun.transaction_uuid, boundary).second,
          "DBLC-013AE BEGIN did not allocate the exact fresh native identity");
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = begun.isolation_level;
  RequireInventoryState(context, scratchbird::transaction::mga::TransactionState::active);
  if (admin) {
    fixture.writer = context;
    fixture.writer_phase = phase;
  } else {
    Require(fixture.readers.emplace(phase, context).second,
            "DBLC-013AE duplicate reader phase");
  }
  return context;
}

engine_api::EngineRequestContext NoAuthorityContext(const std::filesystem::path& database_path,
                                                    std::uint64_t phase) {
  auto context = Context(database_path, phase, kUserBob, phase);
  context.security_context_present = false;
  context.authorization_context = {};
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
    out << evidence.evidence_kind << '\n';
    std::visit([&](const auto& value) {
      if constexpr (std::is_same_v<std::decay_t<decltype(value)>, engine_api::EngineUuid>)
        out.write(reinterpret_cast<const char*>(value.bytes.data()), value.bytes.size());
      else out.write(value.data(), value.size());
    }, evidence.evidence_id);
    out << '\n';
  }
  for (const auto& row : result.result_shape.rows) {
    for (const auto& field : row.fields) {
      out << field.first << '=' << field.second.encoded_value << '\n';
      out.write(reinterpret_cast<const char*>(field.second.binary_value.data()),
                field.second.binary_value.size());
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
    engine_api::EngineUuid principal_uuid,
    std::string_view name,
    std::string_view protected_ref = {}) {
  engine_api::EngineSecurityCreatePrincipalRequest request;
  request.context = Context(database_path, tx, kAdminPrincipal, tx, true);
  request.principal_uuid = principal_uuid;
  request.principal_name = std::string(name);
  request.credential_protected_material_ref = std::string(protected_ref);
  request.option_envelopes.push_back("principal_authority:engine");
  return engine_api::EngineSecurityCreatePrincipal(request);
}

engine_api::EngineSecurityCreateRoleResult CreateRole(const std::filesystem::path& database_path,
                                                      std::uint64_t tx) {
  engine_api::EngineSecurityCreateRoleRequest request;
  request.context = Context(database_path, tx, kAdminPrincipal, tx, true);
  request.role_uuid = kRoleReader;
  request.role_name = "reader";
  request.option_envelopes.push_back("role_authority:engine");
  return engine_api::EngineSecurityCreateRole(request);
}

engine_api::EngineSecurityCreateGroupResult CreateGroup(const std::filesystem::path& database_path,
                                                        std::uint64_t tx) {
  engine_api::EngineSecurityCreateGroupRequest request;
  request.context = Context(database_path, tx, kAdminPrincipal, tx, true);
  request.group_uuid = kGroupAnalysts;
  request.group_name = "analysts";
  request.external_authority_ref = std::string("ldap-ref:") + std::string(kProtectedSecret);
  request.option_envelopes.push_back("group_authority:engine");
  return engine_api::EngineSecurityCreateGroup(request);
}

engine_api::EngineSecurityGrantMembershipResult GrantMembership(
    const std::filesystem::path& database_path,
    std::uint64_t tx,
    engine_api::EngineUuid container_uuid,
    std::string_view container_kind) {
  engine_api::EngineSecurityGrantMembershipRequest request;
  request.context = Context(database_path, tx, kAdminPrincipal, tx, true);
  request.member_principal_uuid = kUserAlice;
  request.container_uuid = container_uuid;
  request.container_kind = std::string(container_kind);
  request.option_envelopes.push_back("security_authority:engine");
  return engine_api::EngineSecurityGrantMembership(request);
}

engine_api::EngineSecurityGrantPrivilegeResult GrantPrivilege(
    const std::filesystem::path& database_path,
    std::uint64_t tx,
    engine_api::EngineUuid grantee_uuid,
    std::string_view grantee_kind,
    std::string_view privilege,
    engine_api::EngineUuid target_uuid = kTableOrders) {
  engine_api::EngineSecurityGrantPrivilegeRequest request;
  request.context = Context(database_path, tx, kAdminPrincipal, tx, true);
  request.grantee_uuid = grantee_uuid;
  request.grantee_kind = std::string(grantee_kind);
  request.target_object_uuid = target_uuid;
  request.target_object_kind = "table";
  request.privilege = std::string(privilege);
  request.option_envelopes.push_back("grant_authority:engine");
  return engine_api::EngineSecurityGrantPrivilege(request);
}

engine_api::EngineSecurityRevokePrivilegeResult RevokePrivilege(
    const std::filesystem::path& database_path,
    std::uint64_t tx,
    engine_api::EngineUuid grantee_uuid,
    std::string_view privilege,
    engine_api::EngineUuid target_uuid = kTableOrders) {
  engine_api::EngineSecurityRevokePrivilegeRequest request;
  request.context = Context(database_path, tx, kAdminPrincipal, tx, true);
  request.grantee_uuid = grantee_uuid;
  request.target_object_uuid = target_uuid;
  request.privilege = std::string(privilege);
  request.option_envelopes.push_back("grant_authority:engine");
  return engine_api::EngineSecurityRevokePrivilege(request);
}

engine_api::EngineSecurityEvaluatePrivilegeResult EvaluatePrivilege(
    const std::filesystem::path& database_path,
    std::uint64_t tx,
    std::uint64_t visible_through,
    engine_api::EngineUuid principal_uuid,
    std::string_view privilege,
    engine_api::EngineUuid target_uuid = kTableOrders) {
  engine_api::EngineSecurityEvaluatePrivilegeRequest request;
  request.context = Context(database_path, tx, principal_uuid, visible_through);
  request.principal_uuid = principal_uuid;
  request.target_object_uuid = target_uuid;
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
  plaintext.context = Context(database_path, 8, kAdminPrincipal, 8, true);
  plaintext.principal_uuid = scratchbird::tests::FixtureUuid(1501, 1);
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
  if (before_grant.ok || !HasDiagnostic(before_grant,
          engine_api::kSecurityPrincipalDiagnosticGrantNotVisible))
    for (const auto& diagnostic : before_grant.diagnostics)
      std::cerr << "before-grant reader: " << diagnostic.code << ':' << diagnostic.detail << '\n';
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
  stale_cache.context = Context(database_path, 29, kUserAlice, 26);
  stale_cache.observed_policy_generation = role_grant.security_generation;
  stale_cache.observed_cache_invalidation_epoch = role_grant.cache_invalidation_epoch;
  stale_cache.option_envelopes.push_back("policy_authority:engine");
  const auto stale_cache_result = engine_api::EngineSecurityValidatePolicyCache(stale_cache);
  Require(!stale_cache_result.ok && stale_cache_result.stale_policy_refused &&
              HasDiagnostic(stale_cache_result,
                            engine_api::kSecurityPrincipalDiagnosticCacheStale),
          "DBLC-013AE stale authorization policy cache was accepted");

  engine_api::EngineSecurityValidatePolicyCacheRequest current_cache;
  current_cache.context = Context(database_path, 30, kUserAlice, 26);
  current_cache.observed_policy_generation = revoked.security_generation;
  current_cache.observed_cache_invalidation_epoch = revoked.cache_invalidation_epoch;
  current_cache.option_envelopes.push_back("policy_authority:engine");
  const auto current_cache_result = engine_api::EngineSecurityValidatePolicyCache(current_cache);
  Require(current_cache_result.ok && current_cache_result.cache_valid,
          "DBLC-013AE current authorization policy cache was rejected");
}

void TestUncommittedAndRolledBackGrants(const std::filesystem::path& database_path) {
  constexpr auto target = scratchbird::tests::FixtureUuid(1501, 5);
  const auto old_reader = Context(database_path, 100, kUserBob, 100);
  const auto granted = GrantPrivilege(database_path, 101, kUserBob, "principal", "SELECT", target);
  RequireOk(granted, "DBLC-013AE pending grant setup failed");
  Require(fixture.writer.has_value(), "DBLC-013AE grant writer lost its lifetime");
  const auto writer = *fixture.writer;
  const auto concurrent_reader = Context(database_path, 102, kUserBob, 102, false, false);
  RequireInventoryState(writer, scratchbird::transaction::mga::TransactionState::active);

  engine_api::EngineSecurityEvaluatePrivilegeRequest inspect;
  inspect.principal_uuid = kUserBob;
  inspect.target_object_uuid = target;
  inspect.target_object_kind = "table";
  inspect.privilege = "SELECT";
  inspect.option_envelopes.push_back("authorization_authority:engine");
  inspect.context = writer;
  const auto own = engine_api::EngineSecurityEvaluatePrivilege(inspect);
  Require(own.ok && own.authorized, "DBLC-013AE writer could not see its own pending grant");
  for (const auto& reader : {old_reader, concurrent_reader}) {
    inspect.context = reader;
    const auto denied = engine_api::EngineSecurityEvaluatePrivilege(inspect);
    Require(!denied.ok && !denied.authorized &&
                HasDiagnostic(denied, engine_api::kSecurityPrincipalDiagnosticDefaultDeny) &&
                !HasDiagnostic(denied, engine_api::kSecurityPrincipalDiagnosticGrantNotVisible),
            "DBLC-013AE diagnostic lookup exposed another transaction's uncommitted grant");
  }
  engine_api::EngineRollbackTransactionRequest rollback;
  rollback.context = writer;
  RequireNativeOk(engine_api::EngineRollbackTransaction(rollback),
                  "DBLC-013AE actual pending-grant rollback failed");
  RequireInventoryState(writer, scratchbird::transaction::mga::TransactionState::rolled_back);
  fixture.writer.reset();
  const auto fresh_reader = Context(database_path, 103, kUserBob, 103);
  for (const auto& reader : {old_reader, concurrent_reader, fresh_reader}) {
    inspect.context = reader;
    const auto denied = engine_api::EngineSecurityEvaluatePrivilege(inspect);
    Require(!denied.ok && !denied.authorized &&
                HasDiagnostic(denied, engine_api::kSecurityPrincipalDiagnosticDefaultDeny) &&
                !HasDiagnostic(denied, engine_api::kSecurityPrincipalDiagnosticGrantNotVisible),
            "DBLC-013AE diagnostic lookup exposed a rolled-back grant");
  }
}

void TestRowSecurityAndDefinerRightsCache(const std::filesystem::path& database_path) {
  const auto definer_grant = GrantPrivilege(database_path, 40, kDefiner, "principal", "SELECT");
  RequireOk(definer_grant, "DBLC-013AE definer grant failed");

  engine_api::EngineSecurityPrimeDefinerRightsCacheRequest prime;
  prime.context = Context(database_path, 41, kAdminPrincipal, 41, true);
  prime.definer_principal_uuid = kDefiner;
  prime.target_object_uuid = kTableOrders;
  prime.privilege = "SELECT";
  prime.option_envelopes.push_back("definer_rights_authority:engine");
  const auto primed = engine_api::EngineSecurityPrimeDefinerRightsCache(prime);
  RequireOk(primed, "DBLC-013AE definer-rights cache prime failed");
  Require(primed.cached && !primed.cache_key.empty(),
          "DBLC-013AE definer-rights cache did not return a cache key");

  engine_api::EngineSecurityValidateDefinerRightsCacheRequest validate;
  validate.context = Context(database_path, 42, kUserAlice, 41);
  validate.cache_key = primed.cache_key;
  validate.observed_policy_generation = primed.policy_generation;
  validate.observed_cache_invalidation_epoch = definer_grant.cache_invalidation_epoch;
  validate.option_envelopes.push_back("definer_rights_authority:engine");
  const auto cache_valid = engine_api::EngineSecurityValidateDefinerRightsCache(validate);
  Require(cache_valid.ok && cache_valid.cache_valid,
          "DBLC-013AE fresh definer-rights cache was rejected");

  const auto definer_revoked = RevokePrivilege(database_path, 43, kDefiner, "SELECT");
  RequireOk(definer_revoked, "DBLC-013AE definer revoke failed");
  validate.context = Context(database_path, 44, kUserAlice, 43);
  const auto cache_stale = engine_api::EngineSecurityValidateDefinerRightsCache(validate);
  Require(!cache_stale.ok && cache_stale.stale_policy_refused &&
              HasDiagnostic(cache_stale,
                            engine_api::kSecurityPrincipalDiagnosticCacheStale),
          "DBLC-013AE stale definer-rights cache was accepted after revoke");

  engine_api::EngineSecurityPutRowPolicyRequest put_policy;
  put_policy.context = Context(database_path, 50, kAdminPrincipal, 50, true);
  put_policy.policy_uuid = scratchbird::tests::FixtureUuid(1501, 2);
  put_policy.target_object_uuid = kTableOrders;
  put_policy.target_object_kind = "table";
  put_policy.policy_effect = "allow_owner";
  put_policy.predicate_envelope = "owner_uuid == effective_user_uuid";
  put_policy.option_envelopes.push_back("row_security_authority:engine");
  const auto policy = engine_api::EngineSecurityPutRowPolicy(put_policy);
  RequireOk(policy, "DBLC-013AE row policy put failed");

  engine_api::EngineSecurityEvaluateRowPolicyRequest row_eval;
  row_eval.context = Context(database_path, 51, kUserAlice, 50);
  row_eval.principal_uuid = kUserAlice;
  row_eval.target_object_uuid = kTableOrders;
  row_eval.row_owner_principal_uuid = kUserAlice;
  row_eval.observed_policy_generation = policy.policy_generation;
  row_eval.option_envelopes.push_back("row_security_authority:engine");
  const auto row_allow = engine_api::EngineSecurityEvaluateRowPolicy(row_eval);
  Require(row_allow.ok && row_allow.row_visible,
          "DBLC-013AE owner row-security hook denied visible row");

  row_eval.context = Context(database_path, 52, kUserBob, 50);
  row_eval.principal_uuid = kUserBob;
  const auto row_deny = engine_api::EngineSecurityEvaluateRowPolicy(row_eval);
  Require(!row_deny.ok && !row_deny.row_visible &&
              HasDiagnostic(row_deny, engine_api::kSecurityPrincipalDiagnosticAccessDenied),
          "DBLC-013AE row-security hook allowed non-owner row");

  row_eval.context = Context(database_path, 53, kUserAlice, 50);
  row_eval.principal_uuid = kUserAlice;
  row_eval.row_owner_principal_uuid = kUserAlice;
  row_eval.observed_policy_generation = policy.policy_generation - 1;
  const auto stale_row_policy = engine_api::EngineSecurityEvaluateRowPolicy(row_eval);
  Require(!stale_row_policy.ok && stale_row_policy.stale_policy_refused &&
              HasDiagnostic(stale_row_policy,
                            engine_api::kSecurityPrincipalDiagnosticPolicyStale),
          "DBLC-013AE stale row policy generation was accepted");
}

void TestDerivedCacheRollbackAndVisibility(const std::filesystem::path& database_path) {
  constexpr auto target = scratchbird::tests::FixtureUuid(1501, 6);
  const auto grant = GrantPrivilege(database_path, 200, kDefiner, "principal", "SELECT", target);
  RequireOk(grant, "DBLC-013AE rollback-cache grant setup failed");
  engine_api::EngineSecurityPrimeDefinerRightsCacheRequest prime;
  prime.context = Context(database_path, 201, kAdminPrincipal, 201, true);
  prime.definer_principal_uuid = kDefiner;
  prime.target_object_uuid = target;
  prime.privilege = "SELECT";
  const auto before = engine_api::LoadSecurityPrincipalLifecycleState(prime.context);
  Require(before.ok, "DBLC-013AE pre-cache native load failed");
  const auto primed = engine_api::EngineSecurityPrimeDefinerRightsCache(prime);
  RequireOk(primed, "DBLC-013AE rollback-cache prime failed");
  const auto after = engine_api::LoadSecurityPrincipalLifecycleState(prime.context);
  Require(after.ok && after.state.security_context_generation == before.state.security_context_generation + 1 &&
              after.state.policy_generation == before.state.policy_generation &&
              after.state.cache_invalidation_epoch == before.state.cache_invalidation_epoch,
          "DBLC-013AE derived cache changed policy/invalidation or lost publication lineage");
  engine_api::EngineSecurityValidateDefinerRightsCacheRequest validate;
  validate.context = prime.context;
  validate.cache_key = primed.cache_key;
  validate.observed_policy_generation = primed.policy_generation;
  validate.observed_cache_invalidation_epoch = grant.cache_invalidation_epoch;
  const auto own = engine_api::EngineSecurityValidateDefinerRightsCache(validate);
  Require(own.ok && own.cache_valid, "DBLC-013AE own uncommitted cache not visible");
  const auto concurrent = Context(database_path, 202, kUserAlice, 202, false, false);
  validate.context = concurrent;
  const auto pending = engine_api::EngineSecurityValidateDefinerRightsCache(validate);
  Require(!pending.ok && HasDiagnostic(pending, engine_api::kSecurityPrincipalDiagnosticCacheMissing),
          "DBLC-013AE pending derived cache leaked to another reader");
  engine_api::EngineRollbackTransactionRequest rollback;
  rollback.context = prime.context;
  RequireNativeOk(engine_api::EngineRollbackTransaction(rollback), "DBLC-013AE cache rollback failed");
  RequireInventoryState(prime.context, scratchbird::transaction::mga::TransactionState::rolled_back);
  fixture.writer.reset();
  validate.context = Context(database_path, 203, kUserAlice, 203);
  const auto rolled_back = engine_api::EngineSecurityValidateDefinerRightsCache(validate);
  Require(!rolled_back.ok && HasDiagnostic(rolled_back, engine_api::kSecurityPrincipalDiagnosticCacheMissing),
          "DBLC-013AE rolled-back cache survived fresh native load");
}

void TestDerivedCacheBatchCodec() {
  namespace db = scratchbird::storage::database;
  namespace codec = db::security_event_codec;
  namespace hash = scratchbird::core::hash;
  const auto loaded = engine_api::LoadDatabaseLocalSecurityEventStoreV1(fixture.owner);
  Require(loaded.ok, "DBLC-013AE cache codec native source read failed");
  const auto& events = loaded.state.events;
  std::size_t index = 0;
  while (index < events.size() && codec::Decode(events[index])[1] != "DEFINER_CACHE") ++index;
  Require(index + 2 < events.size(), "DBLC-013AE committed native cache batch missing");
  db::DatabaseLocalSecurityBatchEnvelopeV1 batch;
  batch.database_uuid = fixture.owner.database_uuid;
  batch.relation_uuid = engine_api::kDatabaseLocalSecurityRelationUuidV1;
  batch.events.assign(events.begin() + index, events.begin() + index + 3);
  const auto first = codec::Decode(batch.events.front());
  batch.creator_local_transaction_id = std::stoull(first[2]);
  batch.actor_principal_uuid = codec::Decode(batch.events[1]).identity(5);
  batch.successor_security_context_generation = std::stoull(codec::Decode(batch.events.back())[3]);
  batch.prior_security_context_generation = batch.successor_security_context_generation - 1;
  const auto inventory = db::LoadLocalTransactionInventoryFromDatabase(fixture.owner.database_path);
  Require(inventory.ok(), "DBLC-013AE cache codec inventory unavailable");
  const auto creator = scratchbird::transaction::mga::LookupLocalTransaction(inventory.inventory,
      scratchbird::transaction::mga::MakeLocalTransactionId(batch.creator_local_transaction_id));
  Require(creator.ok(), "DBLC-013AE cache codec creator missing");
  batch.transaction_uuid = creator.entry.identity.transaction_uuid.value;
  const auto bytes = db::EncodeDatabaseLocalSecurityBatchEnvelopeV1(batch);
  Require(bytes.size() > 12 && bytes[8] == 1 && bytes[9] == 0 && bytes[10] == 2 && bytes[11] == 0,
          "DBLC-013AE derived cache native version not 1.2");
  db::DatabaseLocalSecurityBatchEnvelopeV1 restored;
  Require(db::DecodeDatabaseLocalSecurityBatchEnvelopeV1(bytes, &restored) && restored.events == batch.events,
          "DBLC-013AE cache codec round trip changed native fields");
  for (std::size_t size = 0; size < bytes.size(); ++size) {
    auto truncated = bytes;
    truncated.resize(size);
    Require(!db::DecodeDatabaseLocalSecurityBatchEnvelopeV1(truncated, &restored),
            "DBLC-013AE cache codec accepted a truncated record");
  }
  auto wrong_version = bytes;
  wrong_version[10] = 1;
  Require(!db::DecodeDatabaseLocalSecurityBatchEnvelopeV1(wrong_version, &restored),
          "DBLC-013AE authority variant accepted the cache shape");
  const auto reseal = [](auto& value) {
    const auto framed = codec::Frame(value.events, value.events.size() - 1);
    const auto digest = hash::ComputeSha256Digest(
        reinterpret_cast<const std::uint8_t*>(framed.data()), framed.size());
    Require(digest.ok(), "DBLC-013AE negative-vector seal failed");
    value.events.back() = codec::Encode("AUTH_CONTEXT_SUCCESSOR", value.creator_local_transaction_id,
        {std::to_string(value.successor_security_context_generation),
         "security-context-successor:v1:sha256:" + hash::HexLower(digest.digest)});
  };
  const auto reject = [&](auto value) {
    reseal(value); // Reject semantic corruption even with a fresh valid seal.
    Require(!db::ValidateDatabaseLocalSecurityLifecycleBatchV1(value) &&
                db::EncodeDatabaseLocalSecurityBatchEnvelopeV1(value).empty(),
            "DBLC-013AE cache codec accepted an invalid sealed batch");
  };
  auto wrong = batch;
  wrong.events.erase(wrong.events.begin() + 1);
  reject(wrong);
  wrong = batch;
  wrong.events.insert(wrong.events.begin() + 2, batch.events[1]);
  reject(wrong);
  wrong = batch;
  std::swap(wrong.events[0], wrong.events[1]);
  reject(wrong);
  wrong = batch;
  wrong.actor_principal_uuid = kUserAlice;
  reject(wrong);
  const auto corrupt = [&](std::size_t event, std::size_t field, codec::Field replacement) {
    auto value = batch;
    auto parts = codec::Decode(value.events[event]);
    parts.fields[field] = std::move(replacement);
    std::vector<codec::Field> fields(parts.fields.begin() + 3, parts.fields.end());
    value.events[event] = codec::Encode(parts[1], std::stoull(parts[2]), std::move(fields));
    reject(std::move(value));
  };
  corrupt(0, 3, std::string("forged-cache-key"));
  corrupt(0, 4, engine_api::EngineUuid{});
  corrupt(0, 5, scratchbird::tests::FixtureUuid(1501, 99));
  corrupt(0, 7, std::string("deny"));
  corrupt(0, 8, std::string("0"));
  corrupt(0, 8, std::to_string(batch.successor_security_context_generation));
  corrupt(1, 4, std::string("invalid-operation"));
  corrupt(1, 5, kUserAlice);
  corrupt(1, 6, scratchbird::tests::FixtureUuid(1501, 99));
  corrupt(1, 10, kUserAlice);
  corrupt(1, 11, std::string("grantee_uuid"));
}

void TestAuditEvidenceAndAuthorityDiagnostics(const std::filesystem::path& database_path) {
  engine_api::EngineSecurityInspectAuditRequest inspect;
  inspect.context = Context(database_path, 60, kAdminPrincipal, 60, true);
  inspect.option_envelopes.push_back("security_authority:engine");
  const auto audit = engine_api::EngineSecurityInspectAudit(inspect);
  RequireOk(audit, "DBLC-013AE audit inspect failed");
  Require(audit.protected_material_redacted && audit.audit_records.size() >= 8,
          "DBLC-013AE audit evidence was not persisted for security mutations");
  RequireNoLeak(audit, kProtectedSecret);

  engine_api::EngineSecurityCreatePrincipalRequest no_authority;
  no_authority.context = NoAuthorityContext(database_path, 61);
  no_authority.principal_uuid = scratchbird::tests::FixtureUuid(1501, 3);
  no_authority.principal_name = "denied";
  const auto denied = engine_api::EngineSecurityCreatePrincipal(no_authority);
  Require(!denied.ok &&
              HasDiagnostic(denied,
                            engine_api::kSecurityPrincipalDiagnosticAuthorityRequired),
          "DBLC-013AE missing security authority did not produce stable diagnostic");

  const auto before_spoof = engine_api::LoadSecurityPrincipalLifecycleState(fixture.owner);
  Require(before_spoof.ok, "DBLC-013AE pre-spoof native security read failed");
  auto trace_spoof = no_authority;
  trace_spoof.context.security_context_present = true;
  trace_spoof.context.trace_tags = {"security.fixture_trace_authority", "right:SEC_IDENTITY_ADMIN"};
  const auto spoofed = engine_api::EngineSecurityCreatePrincipal(trace_spoof);
  Require(!spoofed.ok && HasDiagnostic(spoofed,
              engine_api::kSecurityPrincipalDiagnosticAuthorityRequired),
          "DBLC-013AE trace tags conferred security authority");
  const auto after_spoof = engine_api::LoadSecurityPrincipalLifecycleState(fixture.owner);
  Require(after_spoof.ok &&
              after_spoof.state.principals.size() == before_spoof.state.principals.size() &&
              std::none_of(after_spoof.state.principals.begin(), after_spoof.state.principals.end(),
                  [&](const auto& row) { return row.principal_uuid == trace_spoof.principal_uuid; }),
          "DBLC-013AE refused trace-only create changed native principal state");

  engine_api::EngineSecurityCreatePrincipalRequest parser_authority;
  parser_authority.context = Context(database_path, 62, kAdminPrincipal, 62, true);
  parser_authority.principal_uuid = scratchbird::tests::FixtureUuid(1501, 4);
  parser_authority.principal_name = "parser_bypass";
  parser_authority.option_envelopes.push_back("principal_authority:parser");
  const auto bypass = engine_api::EngineSecurityCreatePrincipal(parser_authority);
  Require(!bypass.ok &&
              HasDiagnostic(bypass,
                            engine_api::kSecurityPrincipalDiagnosticAuthorityBypassRefused),
          "DBLC-013AE parser-side security authority was accepted");
}

}  // namespace

int main(int argc, char** argv) {
  const auto temp_dir = MakeTempDir();
  const auto database_path = temp_dir / "dblc013ae.sbdb";
  try {
    const bool grant_visibility_only = argc == 2 &&
        std::string_view(argv[1]) == "--native-grant-visibility";
    Require(argc == 1 || grant_visibility_only, "DBLC-013AE unsupported test selection");
    InitializeNativeFixture(database_path);
    TestPrincipalRoleGroupAndProtectedMaterial(database_path);
    TestDefaultDenyGrantRevokeAndMgaVisibility(database_path);
    TestUncommittedAndRolledBackGrants(database_path);
    if (!grant_visibility_only) {
      TestRowSecurityAndDefinerRightsCache(database_path);
      TestDerivedCacheRollbackAndVisibility(database_path);
      TestDerivedCacheBatchCodec();
    }
    TestAuditEvidenceAndAuthorityDiagnostics(database_path);
    CommitWriter();
    for (const auto& [phase, reader] : fixture.readers) CommitNative(reader);
    const auto reopened = engine_api::LoadSecurityPrincipalLifecycleState(fixture.owner);
    Require(reopened.ok, "DBLC-013AE final native security reopen failed");
    for (const auto identity : {kUserAlice, kUserBob, kDefiner})
      Require(std::any_of(reopened.state.principals.begin(), reopened.state.principals.end(),
                          [&](const auto& row) { return row.principal_uuid == identity && !row.deleted; }),
              "DBLC-013AE committed principal missing after fresh native read");
    fixture.readers.clear();
    fixture.committed_writers.clear();
    std::filesystem::remove_all(temp_dir);
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << error.what() << "\nRetained DBLC-013AE fixture: " << temp_dir << '\n';
    return EXIT_FAILURE;
  }
}
