// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "config.hpp"
#include "config_policy_security_lifecycle.hpp"
#include "database_lifecycle.hpp"
#include "database_lifecycle_test_memory.hpp"
#include "engine_host.hpp"
#include "security/authentication_api.hpp"
#include "session_registry.hpp"
#include "sbps.hpp"
#include "uuid.hpp"

#include <algorithm>
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

namespace engine_api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace server = scratchbird::server;
namespace sbps = scratchbird::server::sbps;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

constexpr std::string_view kDatabaseUuid = "019e0ef1-7b00-7000-8000-000000000011";
constexpr std::string_view kVerifier =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr std::string_view kWrongVerifier =
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
constexpr std::string_view kAlicePrincipalUuid =
    "019e108d-1700-7000-8000-0000000000ae";

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

void PutU16(std::vector<std::uint8_t>* out, std::uint16_t value) {
  out->push_back(static_cast<std::uint8_t>(value & 0xffu));
  out->push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
}

void PutUuid(std::vector<std::uint8_t>* out,
             const std::array<std::uint8_t, 16>& uuid_bytes) {
  out->insert(out->end(), uuid_bytes.begin(), uuid_bytes.end());
}

void PutString(std::vector<std::uint8_t>* out, std::string_view value) {
  PutU16(out, static_cast<std::uint16_t>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
}

void ReplaceOption(std::vector<std::string>* options,
                   std::string_view prefix,
                   std::string value) {
  for (auto& option : *options) {
    if (option.rfind(std::string(prefix), 0) == 0) {
      option = std::move(value);
      return;
    }
  }
  options->push_back(std::move(value));
}

std::filesystem::path MakeTempDir(std::string_view prefix) {
  std::string tmpl = "/tmp/";
  tmpl += prefix;
  tmpl += ".XXXXXX";
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  Require(made != nullptr, "mkdtemp failed");
  return std::filesystem::path(made);
}

void WriteAuthStore(const std::filesystem::path& database_path,
                    std::string_view verifier = kVerifier) {
  scratchbird::tests::database_lifecycle::CreateDurableLocalPasswordPrincipal(
      database_path,
      kDatabaseUuid,
      kAlicePrincipalUuid,
      "alice",
      verifier,
      17,
      "DBLC-013J");
}

void CreateOpenDatabase(const std::filesystem::path& path) {
  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid = uuid::GenerateEngineIdentityV7(UuidKind::database, 1779541201000).value;
  create.filespace_uuid = uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1779541201001).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1779541201002;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "DBLC-013J database create failed");
  const auto opened = db::OpenDatabaseFile({path.string(), false, false, false});
  Require(opened.ok(), "DBLC-013J database open failed");
  const auto clean = db::MarkDatabaseCleanShutdown(path.string());
  Require(clean.ok(), "DBLC-013J clean shutdown marker failed");
}

bool HasDiagnostic(const server::SessionOperationResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

std::string DiagnosticSummary(const server::SessionOperationResult& result) {
  std::string summary;
  for (const auto& diagnostic : result.diagnostics) {
    if (!summary.empty()) summary += ";";
    summary += diagnostic.code;
    for (const auto& field : diagnostic.fields) {
      if (field.key == "detail" && !field.value.empty()) {
        summary += ":";
        summary += field.value;
        break;
      }
    }
  }
  return summary.empty() ? "no diagnostics" : summary;
}

bool HasEngineDiagnostic(const engine_api::EngineApiResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

sbps::Frame MakeFrame(sbps::MessageType type, std::vector<std::uint8_t> payload) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(type);
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.payload_schema_id = type == sbps::MessageType::kAuthHandoff ? 3001 : 3003;
  frame.payload = std::move(payload);
  return frame;
}

server::HostedEngineState MakeEngineState(const std::filesystem::path& database_path,
                                          std::uint64_t policy_generation = 1,
                                          std::uint64_t security_epoch = 1,
                                          std::uint64_t provider_generation = 1,
                                          std::string provider_state = "healthy") {
  server::HostedEngineState engine_state;
  engine_state.engine_context_active = true;
  server::HostedDatabaseSnapshot database;
  database.state = server::HostedDatabaseState::kOpen;
  database.database_open = true;
  database.database_path = database_path.string();
  database.database_uuid = std::string(kDatabaseUuid);
  database.policy_generation = policy_generation;
  database.security_epoch = security_epoch;
  database.security_provider_generation = provider_generation;
  database.security_provider_state = std::move(provider_state);
  database.security_provider_family = "local_password";
  database.default_policy_installed = true;
  database.config_source_epoch = policy_generation;
  database.config_reload_generation = policy_generation;
  database.capability_policy_generation = policy_generation;
  database.cache_invalidation_epoch =
      std::max({policy_generation, security_epoch, provider_generation});
  server::ConfigPolicySecurityLifecycle lifecycle;
  lifecycle.database_path = database.database_path;
  lifecycle.database_uuid = database.database_uuid;
  lifecycle.config_source_epoch = database.config_source_epoch;
  lifecycle.config_reload_generation = database.config_reload_generation;
  lifecycle.capability_policy_generation = database.capability_policy_generation;
  lifecycle.policy_generation = database.policy_generation;
  lifecycle.security_epoch = database.security_epoch;
  lifecycle.provider_generation = database.security_provider_generation;
  lifecycle.provider_family = database.security_provider_family;
  lifecycle.provider_state =
      server::ParseSecurityProviderLifecycleState(database.security_provider_state);
  lifecycle.provider_plugin_loaded = provider_state != "disabled" && provider_state != "quarantined";
  lifecycle.provider_started = provider_state != "loaded" && lifecycle.provider_plugin_loaded;
  lifecycle.provider_healthy = provider_state == "healthy" || provider_state == "started";
  lifecycle.default_policy_installed = database.default_policy_installed;
  lifecycle.cache_invalidation_epoch = database.cache_invalidation_epoch;
  database.config_policy_security_lifecycle_present = true;
  database.config_policy_security_lifecycle_json =
      server::SerializeConfigPolicySecurityLifecycleJson(lifecycle);
  engine_state.databases.push_back(std::move(database));
  return engine_state;
}

std::string Evidence(std::string_view verifier = kVerifier) {
  return scratchbird::tests::database_lifecycle::DurableLocalPasswordEvidence(
      "alice",
      kAlicePrincipalUuid,
      verifier,
      "right:CONNECT,right:OBS_RUNTIME_ALL");
}

std::vector<std::uint8_t> AuthHandoffPayload(bool credential_valid) {
  std::vector<std::uint8_t> out;
  PutUuid(&out, sbps::MakeUuidV7Bytes());
  out.push_back(credential_valid ? 1 : 0);
  out.push_back(credential_valid ? 0 : 1);
  out.push_back(0);
  out.push_back(0);
  PutString(&out, "local_password");
  PutString(&out, "alice");
  PutString(&out, "default");
  PutString(&out, "en");
  PutString(&out, Evidence(credential_valid ? kVerifier : kWrongVerifier));
  return out;
}

engine_api::EngineAuthenticateRequest AuthRequest(const std::filesystem::path& database_path,
                                                  std::string_view verifier = kVerifier) {
  engine_api::EngineAuthenticateRequest request;
  request.context.trust_mode = engine_api::EngineTrustMode::server_isolated;
  request.context.database_path = database_path.string();
  request.context.database_uuid.canonical = std::string(kDatabaseUuid);
  request.context.catalog_generation_id = 2;
  request.context.security_epoch = 2;
  request.provider_family = "local_password";
  request.principal_claim = "alice";
  request.credential_evidence = Evidence(verifier);
  request.credential_evidence_present = true;
  request.target_database.uuid.canonical = std::string(kDatabaseUuid);
  request.target_object.uuid.canonical = std::string(kDatabaseUuid);
  request.option_envelopes.push_back("auth_authority:engine");
  request.option_envelopes.push_back("policy_generation_current:2");
  request.option_envelopes.push_back("policy_generation_observed:2");
  request.option_envelopes.push_back("security_epoch_current:2");
  request.option_envelopes.push_back("security_epoch_observed:2");
  request.option_envelopes.push_back("provider_generation_current:2");
  request.option_envelopes.push_back("provider_generation_observed:2");
  request.option_envelopes.push_back("provider_lifecycle_state:healthy");
  request.option_envelopes.push_back("default_policy_installed:true");
  return request;
}

void TestConfigSourceEpochParsing(const std::filesystem::path& temp_dir) {
  const auto config_path = temp_dir / "sb_server.conf";
  std::ofstream out(config_path, std::ios::trunc);
  out << "[config]\n";
  out << "format = SBCD1\n";
  out << "[server.config]\n";
  out << "source_epoch = 7\n";
  out << "reload_generation = 8\n";
  out << "[server.runtime]\n";
  out << "cache_invalidation_epoch = 3\n";
  out << "[server.capability]\n";
  out << "policy_generation = 12\n";
  out << "[server.security]\n";
  out << "policy_generation = 9\n";
  out << "epoch = 10\n";
  out << "provider_family = local_password\n";
  out << "provider_generation = 11\n";
  out << "provider_state = healthy\n";
  out << "default_policy_installed = true\n";
  Require(static_cast<bool>(out), "config file write failed");
  out.close();

  server::ServerCliOptions cli;
  cli.config_path = config_path.string();
  const auto resolved = server::ResolveServerBootstrapConfig(cli);
  if (!resolved.ok()) {
    for (const auto& diagnostic : resolved.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.safe_message << '\n';
    }
  }
  Require(resolved.ok(), "config source epoch file did not parse");
  Require(resolved.config.config_source_epoch == 7, "config source epoch mismatch");
  Require(resolved.config.config_reload_generation == 8, "config reload generation mismatch");
  Require(resolved.config.security_policy_generation == 9, "security policy generation mismatch");
  Require(resolved.config.capability_policy_generation == 12,
          "capability policy generation mismatch");
  Require(resolved.config.security_epoch == 10, "security epoch mismatch");
  Require(resolved.config.security_provider_generation == 11,
          "security provider generation mismatch");
  Require(resolved.config.cache_invalidation_epoch == 12,
          "cache invalidation epoch was not raised to newest authority epoch");
}

void TestLifecycleStartReloadAndAdmission(const std::filesystem::path& database_path) {
  server::ServerBootstrapConfig config;
  config.selected_config_source = "explicit_file";
  config.config_source_epoch = 1;
  config.config_reload_generation = 1;
  config.capability_policy_generation = 1;
  config.security_policy_generation = 1;
  config.security_epoch = 1;
  config.security_provider_generation = 1;
  config.security_provider_family = "local_password";
  config.security_provider_state = "healthy";
  config.security_default_policy_installed = true;
  config.cache_invalidation_epoch = 1;

  auto start = server::StartConfigPolicySecurityLifecycle(
      server::BuildConfigPolicySecurityLifecycleInput(config,
                                                      database_path.string(),
                                                      std::string(kDatabaseUuid),
                                                      true,
                                                      false));
  Require(start.ok(), "config/policy/security lifecycle failed to start");
  const auto json = server::SerializeConfigPolicySecurityLifecycleJson(start.lifecycle);
  Require(Contains(json, "prepared_statements"), "cache invalidation target missing");
  Require(Contains(json, "password_hash_verification_engine_owned"),
          "password hash authority was not published");

  auto reload = server::ReloadConfigPolicySecurityLifecycle(start.lifecycle, 2, 2, 2, 2);
  Require(reload.ok(), "config/policy/security reload failed");
  auto stale = server::ValidateConfigPolicySecurityAdmission(
      reload.lifecycle, 1, 1, 1, 1, "local_password", "engine");
  Require(!stale.ok() && stale.diagnostic.code == "ENGINE.DBLC_STALE_POLICY_REFUSED",
          "stale policy generation was not refused");
  auto current = server::ValidateConfigPolicySecurityAdmission(
      reload.lifecycle, 2, 2, 2, 2, "local_password", "engine");
  Require(current.ok(), "current policy generation was refused");
  auto parser = server::ValidateConfigPolicySecurityAdmission(
      reload.lifecycle, 2, 2, 2, 2, "local_password", "parser");
  Require(!parser.ok() && parser.diagnostic.code == "ENGINE.DBLC_AUTHORITY_BYPASS_REFUSED",
          "parser-side auth authority bypass was accepted");

  config.security_provider_state = "disabled";
  auto disabled = server::StartConfigPolicySecurityLifecycle(
      server::BuildConfigPolicySecurityLifecycleInput(config,
                                                      database_path.string(),
                                                      std::string(kDatabaseUuid),
                                                      true,
                                                      false));
  Require(!disabled.ok() &&
              disabled.diagnostic.code == "ENGINE.DBLC_SECURITY_PROVIDER_UNAVAILABLE",
          "disabled security provider admitted ordinary authentication");
}

void TestEnginePasswordHashAndPolicyGates(const std::filesystem::path& database_path) {
  WriteAuthStore(database_path);
  auto good = engine_api::EngineAuthenticate(AuthRequest(database_path));
  Require(good.ok && good.authenticated, "engine password-hash verifier path rejected valid hash");
  Require(good.connection_security_context.policy_epoch == 2,
          "engine auth result did not carry policy generation");
  Require(good.connection_security_context.security_epoch == 2,
          "engine auth result did not carry security epoch");

  auto wrong = engine_api::EngineAuthenticate(AuthRequest(database_path, kWrongVerifier));
  Require(!wrong.ok && HasEngineDiagnostic(wrong, "SECURITY.AUTHENTICATION.FAILED"),
          "engine password-hash verifier path accepted wrong hash");

  auto stale = AuthRequest(database_path);
  ReplaceOption(&stale.option_envelopes,
                "policy_generation_observed:",
                "policy_generation_observed:1");
  auto stale_result = engine_api::EngineAuthenticate(stale);
  Require(!stale_result.ok && HasEngineDiagnostic(stale_result, "SECURITY.POLICY.STALE"),
          "engine auth accepted a stale policy generation");

  auto parser = AuthRequest(database_path);
  ReplaceOption(&parser.option_envelopes, "auth_authority:", "auth_authority:parser");
  auto parser_result = engine_api::EngineAuthenticate(parser);
  Require(!parser_result.ok && HasEngineDiagnostic(parser_result, "SECURITY.AUTHORITY.INVALID"),
          "engine auth accepted parser-side authority");

  auto plaintext = AuthRequest(database_path);
  plaintext.option_envelopes.push_back("credential_plaintext:persist");
  auto plaintext_result = engine_api::EngineAuthenticate(plaintext);
  Require(!plaintext_result.ok &&
              HasEngineDiagnostic(plaintext_result, "SECURITY.PROTECTED_MATERIAL.DENIED"),
          "engine auth accepted reusable plaintext credential persistence");
}

void TestServerAuthAttachLifecycle(const std::filesystem::path& database_path) {
  CreateOpenDatabase(database_path);
  WriteAuthStore(database_path);
  server::ServerSessionRegistry registry;
  auto auth_frame = MakeFrame(sbps::MessageType::kAuthHandoff,
                              AuthHandoffPayload(true));
  const auto auth = server::HandleAuthHandoff(&registry, MakeEngineState(database_path), auth_frame);
  Require(auth.accepted, "server auth handoff rejected valid lifecycle state");
  const auto auth_context = server::DecodeAuthContextUuidForTest(auth.payload);
  Require(auth_context.has_value(), "auth handoff did not return context uuid");

  auto stale_attach_frame = MakeFrame(
      sbps::MessageType::kAttachDatabase,
      server::EncodeAttachPayloadForTest(*auth_context, "read_write"));
  const auto stale_attach = server::HandleAttachDatabase(
      &registry, MakeEngineState(database_path, 2, 2, 2), stale_attach_frame);
  Require(!stale_attach.accepted &&
              HasDiagnostic(stale_attach, "ENGINE.DBLC_STALE_POLICY_REFUSED"),
          "attach accepted an auth context created under stale policy generation");

  server::ServerSessionRegistry current_registry;
  auto current_auth_frame = MakeFrame(
      sbps::MessageType::kAuthHandoff,
      AuthHandoffPayload(true));
  const auto current_auth = server::HandleAuthHandoff(
      &current_registry, MakeEngineState(database_path, 2, 2, 2), current_auth_frame);
  Require(current_auth.accepted, "server auth handoff rejected current policy generation");
  const auto current_auth_context = server::DecodeAuthContextUuidForTest(current_auth.payload);
  Require(current_auth_context.has_value(), "current auth did not return context uuid");
  auto current_attach_frame = MakeFrame(
      sbps::MessageType::kAttachDatabase,
      server::EncodeAttachPayloadForTest(*current_auth_context, "read_write"));
  const auto current_attach = server::HandleAttachDatabase(
      &current_registry, MakeEngineState(database_path, 2, 2, 2), current_attach_frame);
  Require(current_attach.accepted,
          "attach rejected current policy generation: " + DiagnosticSummary(current_attach));

  server::ServerSessionRegistry provider_registry;
  auto provider_frame = MakeFrame(sbps::MessageType::kAuthHandoff,
                                  AuthHandoffPayload(true));
  const auto provider = server::HandleAuthHandoff(
      &provider_registry, MakeEngineState(database_path, 1, 1, 1, "quarantined"), provider_frame);
  Require(!provider.accepted &&
              HasDiagnostic(provider, "ENGINE.DBLC_SECURITY_PROVIDER_UNAVAILABLE"),
          "quarantined provider admitted server authentication");
}

}  // namespace

int main() {
  scratchbird::tests::database_lifecycle::ConfigureLifecycleMemoryFixture(
      "database_lifecycle_config_policy_security_provider_conformance");
  const auto temp_dir = MakeTempDir("sb_dblc013j_config_policy_security");
  const auto lifecycle_database_path = temp_dir / "dblc013j_lifecycle.sbdb";
  const auto engine_auth_database_path = temp_dir / "dblc013j_engine_auth.sbdb";
  const auto server_auth_database_path = temp_dir / "dblc013j_server_auth.sbdb";

  TestConfigSourceEpochParsing(temp_dir);
  TestLifecycleStartReloadAndAdmission(lifecycle_database_path);
  TestEnginePasswordHashAndPolicyGates(engine_auth_database_path);
  TestServerAuthAttachLifecycle(server_auth_database_path);

  std::filesystem::remove_all(temp_dir);
  return EXIT_SUCCESS;
}
