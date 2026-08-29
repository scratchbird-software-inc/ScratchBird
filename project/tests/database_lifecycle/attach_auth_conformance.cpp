// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_dispatch_server.hpp"
#include "session_registry.hpp"
#include "database_lifecycle.hpp"
#include "database_lifecycle_test_memory.hpp"
#include "security/database_local_security_event_store.hpp"
#include "local_transaction_store.hpp"
#include "physical_mga_cow_store.hpp"
#include "security/security_principal_lifecycle.hpp"
#include "transaction_inventory.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace mga = scratchbird::transaction::mga;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::server::HostedDatabaseSnapshot;
using scratchbird::server::HostedDatabaseState;
using scratchbird::server::HostedEngineState;
using scratchbird::server::ServerChannelState;
using scratchbird::server::ServerSessionRegistry;
using scratchbird::server::SessionOperationResult;
namespace sbps = scratchbird::server::sbps;

constexpr std::string_view kDatabaseUuid = "019e0ef1-7b00-7000-8000-000000000001";
constexpr std::string_view kVerifier =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr std::string_view kWrongVerifier =
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
constexpr std::string_view kCredentialFingerprint =
    "local-password-pbkdf2-sha256:v1:iterations=600000:"
    "salt=0123456789abcdef0123456789abcdef:"
    "verifier=0358b60b6875c81e17d3e0ab67f8b785f"
    "49d4146547c79da401f21dc641c2c16";
constexpr std::string_view kAlicePrincipalUuid =
    "019e108d-1700-7000-8000-0000000007aa";
constexpr std::string_view kSysarchRoleUuid =
    "019e108d-1700-7000-8000-0000000007a1";
constexpr std::string_view kPublicGroupUuid =
    "019e108d-1700-7000-8000-0000000007a2";
constexpr std::string_view kAliceSysarchMembershipUuid =
    "019e108d-1700-7000-8000-0000000007b1";
constexpr std::string_view kAlicePublicMembershipUuid =
    "019e108d-1700-7000-8000-0000000007b2";
constexpr std::string_view kSysarchConnectGrantUuid =
    "019e108d-1700-7000-8000-0000000007c1";
constexpr std::string_view kSysarchSelectGrantUuid =
    "019e108d-1700-7000-8000-0000000007c2";
constexpr std::string_view kAliceConnectDenyGrantUuid =
    "019e108d-1700-7000-8000-0000000007d1";

struct AuthFixture {
  std::array<std::uint8_t, 16> connection_uuid{};
  std::array<std::uint8_t, 16> auth_context_uuid{};
  sbps::Frame frame;
};

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

void PutU16(std::vector<std::uint8_t>* out, std::uint16_t value) {
  out->push_back(static_cast<std::uint8_t>(value & 0xffu));
  out->push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
}

void PutUuid(std::vector<std::uint8_t>* out, const std::array<std::uint8_t, 16>& uuid) {
  out->insert(out->end(), uuid.begin(), uuid.end());
}

void PutString(std::vector<std::uint8_t>* out, std::string_view value) {
  PutU16(out, static_cast<std::uint16_t>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
}

bool HasDiagnostic(const SessionOperationResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

bool PayloadContains(const SessionOperationResult& result, std::string_view needle) {
  const std::string text(result.payload.begin(), result.payload.end());
  return text.find(needle) != std::string::npos;
}

bool Contains(std::string_view text, std::string_view needle) {
  return text.find(needle) != std::string_view::npos;
}

void PrintDiagnostics(const SessionOperationResult& result) {
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ": " << diagnostic.safe_message;
    for (const auto& field : diagnostic.fields) {
      std::cerr << " " << field.key << "=" << field.value;
    }
    std::cerr << '\n';
  }
}

std::string FinalityState(const ServerSessionRegistry& registry, const sbps::Frame& frame) {
  const auto found =
      registry.finality_by_request_uuid.find(scratchbird::server::UuidBytesToText(frame.header.request_uuid));
  return found == registry.finality_by_request_uuid.end() ? "" : found->second.state;
}

std::filesystem::path MakeTempDir() {
  std::string tmpl = "/tmp/sb_dblc007_attach_auth.XXXXXX";
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  Require(made != nullptr, "mkdtemp failed for DBLC-007 auth test");
  return std::filesystem::path(made);
}

void CreateOpenDatabase(const std::filesystem::path& path) {
  const auto database_uuid = uuid::ParseDurableEngineIdentityUuid(
      UuidKind::database, std::string(kDatabaseUuid));
  Require(database_uuid.ok(), "DBLC-007 fixed database identity was invalid");
  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid = database_uuid.value;
  create.filespace_uuid = uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1779101001001).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1779101001002;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.bootstrap_principal_name = "bootstrap_admin";
  create.bootstrap_credential_fingerprint = std::string(kCredentialFingerprint);
  create.require_bootstrap_principal = true;
  create.allow_uncredentialed_bootstrap = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ":"
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "DBLC-007 database create failed");
  const auto opened = db::OpenDatabaseFile({path.string(), false, false, false});
  Require(opened.ok(), "DBLC-007 first open activation failed");
  const auto clean = db::MarkDatabaseCleanShutdown(path.string());
  Require(clean.ok(), "DBLC-007 clean shutdown marker failed");
}

struct SecurityMutationTransaction {
  api::EngineRequestContext context;
  std::uint64_t security_context_generation = 0;
};

void PrintSecurityDiagnostics(const api::EngineApiResult& result) {
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
}

SecurityMutationTransaction BeginSecurityMutationTransaction(
    const std::filesystem::path& database_path,
    std::uint64_t identity_time) {
  const auto bootstrap =
      db::ReadDatabaseBootstrapSecurityCatalog(database_path.string());
  Require(bootstrap.ok() && bootstrap.state.present &&
              bootstrap.state.security_context_generation != 0,
          "DBLC-007 bootstrap security authority was unavailable");

  const auto loaded_inventory =
      db::LoadLocalTransactionInventoryFromDatabase(database_path.string());
  Require(loaded_inventory.ok(),
          "DBLC-007 durable transaction inventory load failed");
  const auto transaction_uuid = uuid::GenerateDurableEngineIdentityV7(
      UuidKind::transaction, identity_time);
  Require(transaction_uuid.ok(),
          "DBLC-007 durable security mutation identity was unavailable");
  const auto begun = mga::BeginLocalTransaction(
      loaded_inventory.inventory, transaction_uuid.value, identity_time + 2);
  Require(begun.ok(), "DBLC-007 security mutation transaction begin failed");
  Require(db::PersistLocalTransactionInventoryToDatabase(
              database_path.string(), begun.inventory)
              .ok(),
          "DBLC-007 active security mutation transaction was not durable");

  SecurityMutationTransaction transaction;
  auto& context = transaction.context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.database_path = database_path.string();
  context.database_uuid.canonical = std::string(kDatabaseUuid);
  context.database_page_size_bytes = 16384;
  context.principal_uuid.canonical =
      uuid::UuidToString(bootstrap.state.principal_uuid.value);
  context.session_uuid.canonical =
      scratchbird::server::UuidBytesToText(sbps::MakeUuidV7Bytes());
  context.transaction_uuid.canonical =
      uuid::UuidToString(begun.entry.identity.transaction_uuid.value);
  context.local_transaction_id = begun.entry.identity.local_id.value;
  context.snapshot_visible_through_local_transaction_id =
      begun.entry.begin_visible_through_local_transaction_id;
  context.security_epoch =
      std::max<std::uint64_t>(1, bootstrap.state.policy_generation);
  context.catalog_generation_id = 1;
  context.security_context_present = true;
  context.trace_tags.emplace_back(
      api::kDatabaseLocalSecurityLifecycleBootstrapAuthorityTagV1);
  scratchbird::tests::database_lifecycle::MaterializeAuthorizationRights(
      &context,
      "DBLC-007-page-backed-security-bootstrap",
      {"SEC_IDENTITY_ADMIN", "SEC_MEMBERSHIP_ADMIN", "SEC_GRANT_ADMIN"});
  const auto current = api::LoadSecurityPrincipalLifecycleState(context);
  Require(current.ok && current.state.security_context_generation != 0,
          "DBLC-007 current security-context authority was unavailable");
  context.authorization_context.security_context_generation =
      current.state.security_context_generation;
  transaction.security_context_generation =
      current.state.security_context_generation;
  return transaction;
}

void RefreshSecurityContextGeneration(SecurityMutationTransaction* transaction,
                                      std::uint64_t expected_generation) {
  const auto loaded =
      api::LoadSecurityPrincipalLifecycleState(transaction->context);
  Require(loaded.ok &&
              loaded.state.security_context_generation == expected_generation &&
              loaded.state.security_context_generation ==
                  transaction->security_context_generation + 1,
          "DBLC-007 security-context successor was not exact");
  transaction->security_context_generation =
      loaded.state.security_context_generation;
  transaction->context.authorization_context.security_context_generation =
      loaded.state.security_context_generation;
}

void CommitSecurityMutationTransaction(
    const SecurityMutationTransaction& transaction,
    std::uint64_t final_time) {
  db::PhysicalMgaCowFinalizeRequest finalize;
  finalize.database_path = transaction.context.database_path;
  finalize.local_transaction_id =
      mga::MakeLocalTransactionId(transaction.context.local_transaction_id);
  finalize.decision = db::PhysicalMgaCowFinalizeDecision::commit;
  finalize.final_unix_epoch_millis = final_time;
  Require(db::FinalizePhysicalMgaCowTransaction(finalize).ok(),
          "DBLC-007 page-backed security mutation commit failed");
}

void SeedPageBackedAuthorizationStore(
    const std::filesystem::path& database_path) {
  auto transaction =
      BeginSecurityMutationTransaction(database_path, 1779101002000ull);

  api::EngineSecurityCreatePrincipalRequest principal;
  principal.context = transaction.context;
  principal.target_object.uuid.canonical = std::string(kAlicePrincipalUuid);
  principal.target_object.object_kind = "security_principal";
  principal.principal_uuid = std::string(kAlicePrincipalUuid);
  principal.principal_name = "alice";
  principal.credential_fingerprint = std::string(kCredentialFingerprint);
  principal.option_envelopes.push_back("principal_authority:engine");
  const auto created_principal = api::EngineSecurityCreatePrincipal(principal);
  if (!created_principal.ok) PrintSecurityDiagnostics(created_principal);
  Require(created_principal.ok && created_principal.principal_created,
          "DBLC-007 page-backed principal creation failed");
  RefreshSecurityContextGeneration(&transaction,
                                   created_principal.security_generation);

  api::EngineSecurityCreateRoleRequest role;
  role.context = transaction.context;
  role.target_object.uuid.canonical = std::string(kSysarchRoleUuid);
  role.target_object.object_kind = "security_role";
  role.role_uuid = std::string(kSysarchRoleUuid);
  role.role_name = "sysarch";
  role.option_envelopes.push_back("role_authority:engine");
  const auto created_role = api::EngineSecurityCreateRole(role);
  if (!created_role.ok) PrintSecurityDiagnostics(created_role);
  Require(created_role.ok && created_role.role_created,
          "DBLC-007 page-backed role creation failed");
  RefreshSecurityContextGeneration(&transaction, created_role.security_generation);

  api::EngineSecurityCreateGroupRequest group;
  group.context = transaction.context;
  group.target_object.uuid.canonical = std::string(kPublicGroupUuid);
  group.target_object.object_kind = "security_group";
  group.group_uuid = std::string(kPublicGroupUuid);
  group.group_name = "PUBLIC";
  group.option_envelopes.push_back("group_authority:engine");
  const auto created_group = api::EngineSecurityCreateGroup(group);
  if (!created_group.ok) PrintSecurityDiagnostics(created_group);
  Require(created_group.ok && created_group.group_created,
          "DBLC-007 page-backed group creation failed");
  RefreshSecurityContextGeneration(&transaction,
                                   created_group.security_generation);

  auto grant_membership = [&](std::string_view membership_uuid,
                              std::string_view container_uuid,
                              std::string_view container_kind) {
    api::EngineSecurityGrantMembershipRequest request;
    request.context = transaction.context;
    request.membership_uuid = std::string(membership_uuid);
    request.member_principal_uuid = std::string(kAlicePrincipalUuid);
    request.container_uuid = std::string(container_uuid);
    request.container_kind = std::string(container_kind);
    request.option_envelopes.push_back("grant_authority:engine");
    const auto granted = api::EngineSecurityGrantMembership(request);
    if (!granted.ok) PrintSecurityDiagnostics(granted);
    Require(granted.ok && granted.membership_granted,
            "DBLC-007 page-backed membership grant failed");
    RefreshSecurityContextGeneration(&transaction,
                                     granted.security_generation);
  };
  grant_membership(kAliceSysarchMembershipUuid, kSysarchRoleUuid, "role");
  grant_membership(kAlicePublicMembershipUuid, kPublicGroupUuid, "group");

  auto grant_privilege = [&](std::string_view grant_uuid,
                             std::string_view privilege,
                             std::string_view effect) {
    api::EngineSecurityGrantPrivilegeRequest request;
    request.context = transaction.context;
    request.grant_uuid = std::string(grant_uuid);
    request.grantee_uuid = std::string(kSysarchRoleUuid);
    request.grantee_kind = "role";
    request.target_object_uuid.clear();
    request.target_object_kind.clear();
    request.privilege = std::string(privilege);
    request.grant_effect = std::string(effect);
    request.option_envelopes.push_back("grant_authority:engine");
    const auto granted = api::EngineSecurityGrantPrivilege(request);
    if (!granted.ok) PrintSecurityDiagnostics(granted);
    Require(granted.ok && granted.privilege_granted,
            "DBLC-007 page-backed privilege grant failed");
    RefreshSecurityContextGeneration(&transaction,
                                     granted.security_generation);
  };
  grant_privilege(kSysarchConnectGrantUuid, "CONNECT", "allow");
  grant_privilege(kSysarchSelectGrantUuid, "SELECT", "allow");

  CommitSecurityMutationTransaction(transaction, 1779101003000ull);
  auto committed_context = transaction.context;
  committed_context.local_transaction_id = 0;
  committed_context.transaction_uuid.canonical.clear();
  committed_context.snapshot_visible_through_local_transaction_id = 0;
  const auto committed =
      api::LoadSecurityPrincipalLifecycleState(committed_context);
  Require(committed.ok &&
              committed.state.security_context_generation ==
                  transaction.security_context_generation,
          "DBLC-007 committed page-backed security state did not reload");
  Require(!std::filesystem::exists(
              database_path.string() + ".sb.security_principal_events"),
          "DBLC-007 created the retired security-principal sidecar");
}

void AppendAliceConnectDeny(const std::filesystem::path& database_path) {
  auto transaction =
      BeginSecurityMutationTransaction(database_path, 1779101004000ull);
  api::EngineSecurityGrantPrivilegeRequest request;
  request.context = transaction.context;
  request.grant_uuid = std::string(kAliceConnectDenyGrantUuid);
  request.grantee_uuid = std::string(kAlicePrincipalUuid);
  request.grantee_kind = "principal";
  request.target_object_uuid.clear();
  request.target_object_kind.clear();
  request.privilege = "CONNECT";
  request.grant_effect = "deny";
  request.option_envelopes.push_back("grant_authority:engine");
  const auto denied = api::EngineSecurityGrantPrivilege(request);
  if (!denied.ok) PrintSecurityDiagnostics(denied);
  Require(denied.ok && denied.privilege_granted,
          "DBLC-007 page-backed CONNECT deny creation failed");
  RefreshSecurityContextGeneration(&transaction, denied.security_generation);
  CommitSecurityMutationTransaction(transaction, 1779101005000ull);
}

HostedEngineState MakeEngineState(const std::filesystem::path& database_path,
                                  HostedDatabaseState state = HostedDatabaseState::kOpen) {
  HostedEngineState engine_state;
  engine_state.engine_context_active = true;
  HostedDatabaseSnapshot database;
  database.state = state;
  database.database_open = true;
  database.database_path = database_path.string();
  database.database_uuid = std::string(kDatabaseUuid);
  database.read_only = state == HostedDatabaseState::kReadOnly;
  database.write_admission_fenced = false;
  engine_state.databases.push_back(std::move(database));
  return engine_state;
}

HostedEngineState MakeNoDatabaseState() {
  HostedEngineState engine_state;
  engine_state.engine_context_active = true;
  return engine_state;
}

std::vector<std::uint8_t> AuthPayload(const std::array<std::uint8_t, 16>& connection_uuid,
                                      std::string_view principal = "alice",
                                      std::string_view requested_database = "default",
                                      std::string_view verifier = kVerifier,
                                      bool credential_invalid = false,
                                      std::string_view requested_language = "en",
                                      std::string_view requested_role = "") {
  std::vector<std::uint8_t> out;
  PutUuid(&out, connection_uuid);
  out.push_back(1);
  out.push_back(credential_invalid ? 1 : 0);
  out.push_back(0);
  out.push_back(0);
  PutString(&out, "local_password");
  PutString(&out, principal);
  PutString(&out, requested_database);
  PutString(&out, requested_language);
  PutString(&out, verifier);
  PutString(&out, "database_lifecycle_attach_auth_conformance");
  PutString(&out, requested_role);
  return out;
}

std::vector<std::uint8_t> AttachPayload(const std::array<std::uint8_t, 16>& connection_uuid,
                                        const std::array<std::uint8_t, 16>& auth_context_uuid,
                                        std::string_view requested_database = "default",
                                        std::string_view mode = "read_write") {
  std::vector<std::uint8_t> out;
  PutUuid(&out, connection_uuid);
  PutUuid(&out, auth_context_uuid);
  PutString(&out, requested_database);
  PutString(&out, mode);
  return out;
}

sbps::Frame MakeFrame(sbps::MessageType type,
                      std::vector<std::uint8_t> payload,
                      const std::array<std::uint8_t, 16>& connection_uuid = {}) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(type);
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.connection_uuid = connection_uuid;
  frame.header.payload_schema_id = type == sbps::MessageType::kAuthHandoff ? 3001 : 3003;
  frame.payload = std::move(payload);
  return frame;
}

AuthFixture Authenticate(ServerSessionRegistry* registry,
                         const HostedEngineState& engine_state,
                         std::string_view principal = "alice",
                         std::string_view requested_database = "default",
                         std::string_view requested_language = "en",
                         std::string_view requested_role = "") {
  AuthFixture fixture;
  fixture.connection_uuid = sbps::MakeUuidV7Bytes();
  fixture.frame = MakeFrame(sbps::MessageType::kAuthHandoff,
                            AuthPayload(fixture.connection_uuid,
                                        principal,
                                        requested_database,
                                        kVerifier,
                                        false,
                                        requested_language,
                                        requested_role),
                            fixture.connection_uuid);
  const auto result = scratchbird::server::HandleAuthHandoff(registry, engine_state, fixture.frame);
  if (!result.accepted) {
    for (const auto& diagnostic : result.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.safe_message << '\n';
    }
  }
  Require(result.accepted, "authentication should have been accepted");
  const auto decoded = scratchbird::server::DecodeAuthContextUuidForTest(result.payload);
  Require(decoded.has_value(), "accepted authentication did not return auth context");
  fixture.auth_context_uuid = *decoded;
  Require(registry->sessions_by_uuid.empty(), "auth handoff created a session before attach");
  Require(registry->auth_contexts_by_uuid.size() == 1, "auth context was not retained");
  Require(FinalityState(*registry, fixture.frame) == "accepted", "auth finality was not recorded as accepted");
  return fixture;
}

SessionOperationResult Attach(ServerSessionRegistry* registry,
                              const HostedEngineState& engine_state,
                              const AuthFixture& auth,
                              std::string_view requested_database = "default",
                              std::string_view mode = "read_write",
                              std::array<std::uint8_t, 16> header_connection_uuid = {}) {
  if (sbps::IsZeroUuid(header_connection_uuid)) header_connection_uuid = auth.connection_uuid;
  auto frame = MakeFrame(sbps::MessageType::kAttachDatabase,
                         AttachPayload(auth.connection_uuid,
                                       auth.auth_context_uuid,
                                       requested_database,
                                       mode),
                         header_connection_uuid);
  auto result = scratchbird::server::HandleAttachDatabase(registry, engine_state, frame);
  Require(!FinalityState(*registry, frame).empty(), "attach did not record finality");
  return result;
}

void RequireDblcDenied(const SessionOperationResult& result, std::string_view message) {
  Require(!result.accepted, message);
  Require(HasDiagnostic(result, "ENGINE.DBLC_ATTACH_ADMISSION_DENIED"),
          "DBLC attach admission diagnostic family was not emitted");
}

void TestAcceptedAuthAttach(const std::filesystem::path& database_path) {
  ServerSessionRegistry registry;
  const auto engine_state = MakeEngineState(database_path);
  auto auth = Authenticate(&registry, engine_state, "alice", "default", "en", "sysarch");
  auto attach = Attach(&registry, engine_state, auth, std::string(kDatabaseUuid), "read_write");
  if (!attach.accepted) PrintDiagnostics(attach);
  Require(attach.accepted, "valid auth plus attach was rejected");
  Require(registry.channel_state == ServerChannelState::kReady, "attach did not move channel to ready");
  Require(registry.sessions_by_uuid.size() == 1, "attach did not create exactly one session");
  const auto& session = registry.sessions_by_uuid.begin()->second;
  Require(session.session_binding_present,
          "accepted attach did not publish the parser-server session binding");
  Require(session.connection_uuid == auth.connection_uuid, "session did not bind parser connection route");
  Require(session.auth_context_uuid == auth.auth_context_uuid, "session did not bind auth context");
  Require(session.database_path == database_path.string(), "session did not bind hosted database path");
  Require(session.database_uuid == kDatabaseUuid, "session did not bind hosted database UUID");
  Require(session.language_profile == "sbsql.builtin.recovery.en",
          "session did not carry built-in language profile id");
  Require(session.language_tag == "en",
          "session did not carry requested language tag");
  Require(session.default_language_tag == "en",
          "session did not carry default language tag");
  Require(session.input_syntax_profile == "sbsql.syntax.standard",
          "session did not carry input syntax profile");
  Require(session.common_resource_hash == "builtin.common.sbsql.v1",
          "session did not carry common resource hash");
  Require(session.language_resource_epoch != 0 &&
              session.localized_name_epoch != 0 &&
              session.message_resource_epoch != 0,
          "session did not carry language resource epochs");
  Require(session.resource_compatibility_identity == "sbsql.resource.compat.v1",
          "session did not carry resource compatibility identity");
  Require(session.resource_version_identity == "sbsql.resource-pack.v1",
          "session did not carry resource version identity");
  Require(session.local_transaction_id != 0, "attach did not admit the required active transaction");
  Require(!session.transaction_uuid.empty(), "attach did not bind the active transaction UUID");
  Require(scratchbird::server::UuidBytesToText(session.active_role_uuid) == kSysarchRoleUuid,
          "session did not activate requested sysarch role");
  Require(session.effective_role_uuids.size() == 1,
          "session did not materialize sysarch effective role");
  Require(session.effective_group_uuids.size() == 1,
          "session did not materialize PUBLIC effective group");
  Require(scratchbird::server::UuidBytesToText(session.effective_group_uuids.front()) ==
              kPublicGroupUuid,
          "session did not materialize expected PUBLIC group");
  Require(PayloadContains(attach, "accepted"), "attach result did not report accepted outcome");
  const auto status = scratchbird::server::SessionRegistryStatusJson(registry);
  Require(Contains(status, "\"language_profile_id\":\"sbsql.builtin.recovery.en\""),
          "session status omitted language profile id");
  Require(Contains(status, "\"language_tag\":\"en\""),
          "session status omitted language tag");
  Require(Contains(status, "\"common_resource_hash\":\"builtin.common.sbsql.v1\""),
          "session status omitted common resource hash");
  Require(Contains(status, "\"effective_role_count\":1"),
          "session status omitted effective role count");
  Require(Contains(status, "\"effective_group_count\":1"),
          "session status omitted effective group count");
}

void TestNonDefaultLanguageContextIdentity() {
  scratchbird::server::ServerSessionRecord session;
  session.session_uuid = sbps::MakeUuidV7Bytes();
  session.connection_uuid = sbps::MakeUuidV7Bytes();
  session.auth_context_uuid = sbps::MakeUuidV7Bytes();
  session.principal_claim = "alice";

  scratchbird::server::ApplyRequestedLanguageProfile(&session, "fr-CA");

  const auto language = scratchbird::server::ServerLanguageContextForSession(session);
  Require(language.language_profile_id == "sbsql.language-profile.fr-CA",
          "non-default language profile id was not derived");
  Require(language.language_tag == "fr-CA",
          "non-default language tag was not retained");
  Require(language.default_language_tag == "en",
          "non-default language context lost default fallback tag");
  Require(language.input_syntax_profile == "sbsql.syntax.standard",
          "non-default language context lost input syntax profile");
  Require(language.common_resource_hash == "builtin.common.sbsql.v1",
          "non-default language context lost common resource hash");
  Require(language.language_resource_epoch != 0 &&
              language.localized_name_epoch != 0 &&
              language.message_resource_epoch != 0,
          "non-default language context lost resource epochs");
  Require(language.resource_compatibility_identity == "sbsql.resource.compat.v1",
          "non-default language context lost compatibility identity");
  Require(language.resource_version_identity == "sbsql.resource-pack.v1",
          "non-default language context lost resource version identity");

  ServerSessionRegistry registry;
  registry.sessions_by_uuid[scratchbird::server::UuidBytesToText(session.session_uuid)] = session;
  const auto status = scratchbird::server::SessionRegistryStatusJson(registry);
  Require(Contains(status, "\"language_profile_id\":\"sbsql.language-profile.fr-CA\""),
          "session status omitted non-default language profile id");
  Require(Contains(status, "\"language_tag\":\"fr-CA\""),
          "session status omitted non-default language tag");
  Require(Contains(status, "\"resource_compatibility_identity\":\"sbsql.resource.compat.v1\""),
          "session status omitted resource compatibility identity");
  Require(Contains(status, "\"resource_version_identity\":\"sbsql.resource-pack.v1\""),
          "session status omitted resource version identity");
}

void TestAuthRefusals(const std::filesystem::path& database_path) {
  {
    ServerSessionRegistry registry;
    const auto conn = sbps::MakeUuidV7Bytes();
    auto frame = MakeFrame(sbps::MessageType::kAuthHandoff,
                           AuthPayload(conn, "alice", "default", kWrongVerifier, true),
                           conn);
    const auto result = scratchbird::server::HandleAuthHandoff(&registry, MakeEngineState(database_path), frame);
    RequireDblcDenied(result, "invalid credentials were accepted");
    Require(HasDiagnostic(result, "SECURITY.AUTHENTICATION.FAILED"),
            "invalid auth did not return a stable refusal diagnostic");
    Require(registry.auth_contexts_by_uuid.empty() && registry.sessions_by_uuid.empty(),
            "rejected auth created server state");
  }
  {
    ServerSessionRegistry registry;
    const auto conn = sbps::MakeUuidV7Bytes();
    auto frame = MakeFrame(sbps::MessageType::kAuthHandoff, AuthPayload(conn), conn);
    const auto result = scratchbird::server::HandleAuthHandoff(&registry, MakeNoDatabaseState(), frame);
    RequireDblcDenied(result, "auth without hosted database was accepted");
    Require(HasDiagnostic(result, "PARSER_SERVER_IPC.AUTH_DATABASE_UNAVAILABLE"),
            "auth without database did not report database unavailable");
  }
  {
    ServerSessionRegistry registry;
    const auto conn = sbps::MakeUuidV7Bytes();
    auto frame = MakeFrame(sbps::MessageType::kAuthHandoff, AuthPayload(conn, "alice", "wrong-database"), conn);
    const auto result = scratchbird::server::HandleAuthHandoff(&registry, MakeEngineState(database_path), frame);
    RequireDblcDenied(result, "auth database mismatch was accepted");
    Require(HasDiagnostic(result, "PARSER_SERVER_IPC.AUTH_DATABASE_MISMATCH"),
            "auth mismatch did not report database mismatch");
  }
  {
    ServerSessionRegistry registry;
    const auto payload_conn = sbps::MakeUuidV7Bytes();
    const auto header_conn = sbps::MakeUuidV7Bytes();
    auto frame = MakeFrame(sbps::MessageType::kAuthHandoff, AuthPayload(payload_conn), header_conn);
    const auto result = scratchbird::server::HandleAuthHandoff(&registry, MakeEngineState(database_path), frame);
    RequireDblcDenied(result, "auth route mismatch was accepted");
    Require(HasDiagnostic(result, "PARSER_SERVER_IPC.ROUTE_ASSOCIATION_MISMATCH"),
            "auth route mismatch did not report route mismatch");
  }
  {
    ServerSessionRegistry registry;
    auto clustered = MakeEngineState(database_path);
    clustered.databases.front().cluster_structures_present = true;
    const auto conn = sbps::MakeUuidV7Bytes();
    auto frame = MakeFrame(sbps::MessageType::kAuthHandoff, AuthPayload(conn), conn);
    const auto result = scratchbird::server::HandleAuthHandoff(&registry, clustered, frame);
    RequireDblcDenied(result, "cluster-scoped standalone auth was accepted");
    Require(HasDiagnostic(result, "ENGINE.DBLC_STANDALONE_CLUSTER_FAIL_CLOSED"),
            "cluster-scoped auth did not fail closed");
  }
}

void TestAttachRefusals(const std::filesystem::path& database_path) {
  {
    ServerSessionRegistry registry;
    AuthFixture auth;
    auth.connection_uuid = sbps::MakeUuidV7Bytes();
    auto frame = MakeFrame(sbps::MessageType::kAttachDatabase,
                           AttachPayload(auth.connection_uuid, {}),
                           auth.connection_uuid);
    const auto result = scratchbird::server::HandleAttachDatabase(&registry, MakeEngineState(database_path), frame);
    RequireDblcDenied(result, "attach without auth context was accepted");
    Require(HasDiagnostic(result, "PARSER_SERVER_IPC.AUTH_HANDOFF_REQUIRED"),
            "missing auth context did not report auth handoff required");
  }
  {
    ServerSessionRegistry registry;
    AuthFixture auth;
    auth.connection_uuid = sbps::MakeUuidV7Bytes();
    auth.auth_context_uuid = sbps::MakeUuidV7Bytes();
    auto result = Attach(&registry, MakeEngineState(database_path), auth);
    RequireDblcDenied(result, "unknown auth context was accepted");
    Require(HasDiagnostic(result, "PARSER_SERVER_IPC.SESSION_NOT_BOUND"),
            "unknown auth context did not report session not bound");
  }
  {
    ServerSessionRegistry registry;
    const auto auth = Authenticate(&registry, MakeEngineState(database_path));
    auto result = Attach(&registry, MakeNoDatabaseState(), auth);
    RequireDblcDenied(result, "attach without hosted database was accepted");
    Require(HasDiagnostic(result, "PARSER_SERVER_IPC.ATTACH_DATABASE_UNAVAILABLE"),
            "attach without database did not report database unavailable");
    Require(registry.sessions_by_uuid.empty(), "failed attach created a session");
  }
  {
    ServerSessionRegistry registry;
    const auto auth = Authenticate(&registry, MakeEngineState(database_path));
    auto result = Attach(&registry, MakeEngineState(database_path), auth, "wrong-database");
    RequireDblcDenied(result, "attach database mismatch was accepted");
    Require(HasDiagnostic(result, "PARSER_SERVER_IPC.ATTACH_DATABASE_MISMATCH"),
            "attach mismatch did not report database mismatch");
  }
  {
    ServerSessionRegistry registry;
    const auto auth = Authenticate(&registry, MakeEngineState(database_path));
    auto result = Attach(&registry, MakeEngineState(database_path), auth, "default", "exclusive_maintenance");
    RequireDblcDenied(result, "unsupported attach mode was accepted");
    Require(HasDiagnostic(result, "PARSER_SERVER_IPC.ATTACH_MODE_UNSUPPORTED"),
            "unsupported attach mode did not report mode diagnostic");
  }
  {
    ServerSessionRegistry registry;
    const auto auth = Authenticate(&registry, MakeEngineState(database_path));
    auto result = Attach(&registry,
                         MakeEngineState(database_path),
                         auth,
                         "default",
                         "read_write",
                         sbps::MakeUuidV7Bytes());
    RequireDblcDenied(result, "attach route mismatch was accepted");
    Require(HasDiagnostic(result, "PARSER_SERVER_IPC.ROUTE_ASSOCIATION_MISMATCH"),
            "attach route mismatch did not report route mismatch");
  }
}

void TestLifecycleAndAuthorizationFences(const std::filesystem::path& database_path) {
  {
    ServerSessionRegistry registry;
    const auto auth = Authenticate(&registry, MakeEngineState(database_path, HostedDatabaseState::kReadOnly));
    auto result = Attach(&registry,
                         MakeEngineState(database_path, HostedDatabaseState::kReadOnly),
                         auth,
                         "default",
                         "read_write");
    RequireDblcDenied(result, "read-write attach to read-only database was accepted");
    Require(HasDiagnostic(result, "PARSER_SERVER_IPC.ATTACH_MODE_DENIED"),
            "read-only database did not deny read-write attach");
  }
  {
    ServerSessionRegistry registry;
    const auto auth = Authenticate(&registry, MakeEngineState(database_path, HostedDatabaseState::kReadOnly));
    auto result = Attach(&registry,
                         MakeEngineState(database_path, HostedDatabaseState::kReadOnly),
                         auth,
                         "default",
                         "read_only");
    Require(result.accepted, "read-only attach to read-only database was rejected");
    Require(registry.sessions_by_uuid.begin()->second.local_transaction_id != 0,
            "read-only attach did not create the required active transaction");
  }
  {
    ServerSessionRegistry registry;
    const auto auth = Authenticate(&registry, MakeEngineState(database_path, HostedDatabaseState::kRestrictedOpen));
    auto result = Attach(&registry,
                         MakeEngineState(database_path, HostedDatabaseState::kRestrictedOpen),
                         auth,
                         "default",
                         "read_only");
    RequireDblcDenied(result, "restricted-open ordinary attach was accepted");
    Require(PayloadContains(result, "restricted_open_admission_required"),
            "restricted-open attach did not explain the lifecycle fence");
  }
  {
    AppendAliceConnectDeny(database_path);
    ServerSessionRegistry registry;
    auto auth = Authenticate(&registry, MakeEngineState(database_path));
    auto result = Attach(&registry, MakeEngineState(database_path), auth);
    RequireDblcDenied(result, "engine authorization denial was accepted");
    Require(HasDiagnostic(result, "SECURITY.AUTHORIZATION.DENIED"),
            "engine authorization denial was not surfaced");
    Require(registry.sessions_by_uuid.empty(), "authorization denial created a session");
  }
}

void TestAuthDoesNotPermitParserBypass(const std::filesystem::path& database_path) {
  ServerSessionRegistry registry;
  const auto engine_state = MakeEngineState(database_path);
  auto auth = Authenticate(&registry, engine_state);
  sbps::Frame execute;
  execute.header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kExecuteSblr);
  execute.header.request_uuid = sbps::MakeUuidV7Bytes();
  execute.header.session_uuid = auth.auth_context_uuid;
  execute.header.payload_schema_id = 4003;
  execute.payload = scratchbird::server::EncodeExecuteSblrPayloadForTest(
      auth.auth_context_uuid,
      {},
      scratchbird::server::EncodeShowVersionSblrForTest());
  const auto result = scratchbird::server::HandleExecuteSblr(&registry, engine_state, execute);
  Require(!result.accepted, "auth context alone allowed parser execute before attach");
  Require(HasDiagnostic(result, "PARSER_SERVER_IPC.SESSION_REQUIRED"),
          "execute before attach did not require a bound session");
  Require(registry.sessions_by_uuid.empty(), "execute before attach created a session");
}

}  // namespace

int main() {
  scratchbird::tests::database_lifecycle::ConfigureLifecycleMemoryFixture(
      "database_lifecycle_attach_auth_conformance");
  const auto temp_dir = MakeTempDir();
  const auto database_path = temp_dir / "dblc007_attach_auth.sbdb";
  CreateOpenDatabase(database_path);
  SeedPageBackedAuthorizationStore(database_path);

  TestAcceptedAuthAttach(database_path);
  TestNonDefaultLanguageContextIdentity();
  TestAuthRefusals(database_path);
  TestAttachRefusals(database_path);
  TestLifecycleAndAuthorizationFences(database_path);
  TestAuthDoesNotPermitParserBypass(database_path);

  std::filesystem::remove_all(temp_dir);
  return EXIT_SUCCESS;
}
