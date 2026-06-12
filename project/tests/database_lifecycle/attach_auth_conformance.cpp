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
#include "uuid.hpp"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

namespace db = scratchbird::storage::database;
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
constexpr std::string_view kAlicePrincipalUuid =
    "019e108d-1700-7000-8000-0000000007aa";

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
  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid = uuid::GenerateEngineIdentityV7(UuidKind::database, 1779101001000).value;
  create.filespace_uuid = uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1779101001001).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1779101001002;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
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

void WriteAuthStore(const std::filesystem::path& database_path,
                    std::string_view principal = "alice",
                    std::string_view verifier = kVerifier) {
  scratchbird::tests::database_lifecycle::CreateDurableLocalPasswordPrincipal(
      database_path,
      kDatabaseUuid,
      kAlicePrincipalUuid,
      principal,
      verifier,
      7,
      "DBLC-007");
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

std::string Evidence(std::string_view principal, std::string_view verifier) {
  return scratchbird::tests::database_lifecycle::DurableLocalPasswordEvidence(
      principal,
      kAlicePrincipalUuid,
      verifier,
      "right:CONNECT");
}

std::vector<std::uint8_t> AuthPayload(const std::array<std::uint8_t, 16>& connection_uuid,
                                      std::string_view principal = "alice",
                                      std::string_view requested_database = "default",
                                      std::string_view verifier = kVerifier,
                                      bool credential_invalid = false,
                                      std::string_view requested_language = "en") {
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
  PutString(&out, Evidence(principal, verifier));
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
                         std::string_view requested_language = "en") {
  AuthFixture fixture;
  fixture.connection_uuid = sbps::MakeUuidV7Bytes();
  fixture.frame = MakeFrame(sbps::MessageType::kAuthHandoff,
                            AuthPayload(fixture.connection_uuid,
                                        principal,
                                        requested_database,
                                        kVerifier,
                                        false,
                                        requested_language),
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
  auto auth = Authenticate(&registry, engine_state);
  auto attach = Attach(&registry, engine_state, auth, std::string(kDatabaseUuid), "read_write");
  Require(attach.accepted, "valid auth plus attach was rejected");
  Require(registry.channel_state == ServerChannelState::kReady, "attach did not move channel to ready");
  Require(registry.sessions_by_uuid.size() == 1, "attach did not create exactly one session");
  const auto& session = registry.sessions_by_uuid.begin()->second;
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
  Require(PayloadContains(attach, "accepted"), "attach result did not report accepted outcome");
  const auto status = scratchbird::server::SessionRegistryStatusJson(registry);
  Require(Contains(status, "\"language_profile_id\":\"sbsql.builtin.recovery.en\""),
          "session status omitted language profile id");
  Require(Contains(status, "\"language_tag\":\"en\""),
          "session status omitted language tag");
  Require(Contains(status, "\"common_resource_hash\":\"builtin.common.sbsql.v1\""),
          "session status omitted common resource hash");
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
    ServerSessionRegistry registry;
    auto auth = Authenticate(&registry, MakeEngineState(database_path));
    const auto key = scratchbird::server::UuidBytesToText(auth.auth_context_uuid);
    registry.auth_contexts_by_uuid[key].engine_authorization_trace_tags.push_back("deny:CONNECT");
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
  WriteAuthStore(database_path);

  TestAcceptedAuthAttach(database_path);
  TestNonDefaultLanguageContextIdentity();
  TestAuthRefusals(database_path);
  TestAttachRefusals(database_path);
  TestLifecycleAndAuthorizationFences(database_path);
  TestAuthDoesNotPermitParserBypass(database_path);

  std::filesystem::remove_all(temp_dir);
  return EXIT_SUCCESS;
}
