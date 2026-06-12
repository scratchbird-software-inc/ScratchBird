// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ast/ast.hpp"
#include "binder/binder.hpp"
#include "cst/cst.hpp"
#include "lowering/lowering.hpp"
#include "sblr_dispatch_server.hpp"
#include "session_registry.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace sbsql = scratchbird::parser::sbsql;
namespace server = scratchbird::server;
namespace sbps = scratchbird::server::sbps;

void Require(bool condition, std::string_view message) {
  if (condition) return;
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

bool Contains(std::string_view value, std::string_view needle) {
  return value.find(needle) != std::string_view::npos;
}

sbsql::ParserConfig TestConfig() {
  sbsql::ParserConfig config;
  config.parser_uuid = "sml008-server-language-session-control-parser";
  config.dialect = "sbsql";
  config.profile_id = "default";
  config.registry_version = sbsql::kSbsqlWorkerRegistryCurrentVersion;
  config.protocol_version = sbsql::kSbsqlWorkerProtocolCurrentVersion;
  config.bundle_contract_id = "sbp_sbsql@1";
  config.build_id = "sml008-server-language-session-control-build";
  return config;
}

sbsql::SessionContext ParserSession() {
  sbsql::SessionContext session;
  session.authenticated = true;
  session.session_uuid = "00000000-0000-7000-8000-000000000881";
  session.connection_uuid = "00000000-0000-7000-8000-000000000882";
  session.database_uuid = "00000000-0000-7000-8000-000000000883";
  session.authenticated_user_uuid = "00000000-0000-7000-8000-000000000884";
  session.principal_claim = "sml008-user";
  session.auth_provider_family = "test";
  session.search_path = {"public"};
  session.default_language = "en";
  session.language_tag = "en";
  session.language_profile = "sbsql.builtin.recovery.en";
  session.input_syntax_profile = "sbsql.syntax.standard";
  session.common_resource_hash = "builtin.common.sbsql.v1";
  session.resource_compatibility_identity = "sbsql.resource.compat.v1";
  session.resource_version_identity = "sbsql.resource-pack.v1";
  session.language_resource_epoch = 11;
  session.localized_name_epoch = 12;
  session.message_resource_epoch = 13;
  session.catalog_epoch = 21;
  session.security_policy_epoch = 22;
  session.grant_epoch = 23;
  session.descriptor_epoch = 24;
  return session;
}

std::string LowerLanguageControl(std::string_view sql) {
  const auto cst = sbsql::BuildCst(std::string(sql));
  auto ast = sbsql::BuildAst(cst);
  const auto config = TestConfig();
  const auto session = ParserSession();
  auto bound = sbsql::BindAst(ast, cst, config, session);
  auto envelope = sbsql::LowerToSblr(bound, cst, session);
  return envelope.payload;
}

std::string QueryEnvelope() {
  return "{\"envelope\":\"SBLRExecutionEnvelope.v3\","
         "\"operation_family\":\"sblr.query.relational.v3\","
         "\"surface_key\":\"sml008.fixture.query\","
         "\"operation_id\":\"query.evaluate_projection\","
         "\"sblr_operation_key\":\"sblr.query.relational.v3\","
         "\"result_shape\":\"result.shape.rowset\","
         "\"diagnostic_shape\":\"diagnostic.canonical_message_vector\","
         "\"resource_contract\":\"resource.contract.test\","
         "\"trace_key\":\"SML-008\","
         "\"source_payload_embedded\":false,"
         "\"resolved_object_uuids\":[],"
         "\"descriptor_refs\":[],"
         "\"policy_refs\":[]}";
}

std::string AdmittedLanguageBundleEnvelope(std::string_view operation_id,
                                           std::string_view bundle_uuid,
                                           std::string_view language_profile_id,
                                           std::string_view language_tag,
                                           bool required_profile = false) {
  return "{\"envelope\":\"SBLRExecutionEnvelope.v3\","
         "\"operation_family\":\"sblr.language.resource_control.v3\","
         "\"surface_key\":\"sml009.fixture.language_bundle\","
         "\"operation_id\":\"" +
         std::string(operation_id) +
         "\","
         "\"sblr_operation_key\":\"sblr.language.resource_control.v3\","
         "\"result_shape\":\"result.shape.language_resource_control\","
         "\"diagnostic_shape\":\"diagnostic.canonical_message_vector\","
         "\"resource_contract\":\"sbsql.language.resource_control.v1\","
         "\"trace_key\":\"SML-009\","
         "\"source_payload_embedded\":false,"
         "\"resource_bundle_operation\":true,"
         "\"signed_bundle_required\":true,"
         "\"compatible_bundle_required\":true,"
         "\"security_admission_required\":true,"
         "\"admitted_bundle_manifest_attached\":true,"
         "\"bundle_signature_verified\":true,"
         "\"bundle_security_admitted\":true,"
         "\"bundle_compatible_with_server\":true,"
         "\"bundle_provenance_verified\":true,"
         "\"parser_language_library_admission\":false,"
         "\"load_or_unload_effects_executed_by_parser\":false,"
         "\"row_storage_touched\":false,"
         "\"mga_finality_claimed\":false,"
         "\"bundle_uuid\":\"" +
         std::string(bundle_uuid) +
         "\","
         "\"language_profile_id\":\"" +
         std::string(language_profile_id) +
         "\","
         "\"language_tag\":\"" +
         std::string(language_tag) +
         "\","
         "\"dialect_profile_uuid\":\"sbsql.v3\","
         "\"topology_profile_uuid\":\"topology.sbsql.canonical_svo.v1\","
         "\"common_resource_hash\":\"common.resource." +
         std::string(language_tag) +
         "\","
         "\"resource_hash\":\"resource.hash." +
         std::string(language_tag) +
         "\","
         "\"required_profile\":" +
         (required_profile ? std::string("true") : std::string("false")) +
         ","
         "\"resolved_object_uuids\":[],"
         "\"descriptor_refs\":[\"sys.language.resource_bundle\"],"
         "\"policy_refs\":[\"language_resource_admission_policy\"],"
         "\"required_rights\":[\"right.language_bundle_admin\"],"
         "\"required_authority_steps\":["
         "\"authority.server.language_resource_registry_required\","
         "\"authority.security.language_bundle_admission_required\","
         "\"authority.parser.no_bundle_loading\"]}";
}

server::ServerSessionRegistry MakeRegistry(std::array<std::uint8_t, 16>* session_uuid) {
  server::ServerSessionRegistry registry;
  server::ServerSessionRecord session;
  session.session_uuid = sbps::MakeUuidV7Bytes();
  session.auth_context_uuid = sbps::MakeUuidV7Bytes();
  session.principal_uuid = sbps::MakeUuidV7Bytes();
  session.effective_user_uuid = session.principal_uuid;
  session.principal_claim = "sml008-server-user";
  session.database_path = "/tmp/sbsql_server_language_session_control_conformance.sbdb";
  session.database_uuid = "019e0a8c-f010-7000-8000-000000000008";
  session.catalog_generation = 21;
  session.security_epoch = 22;
  session.descriptor_epoch = 24;
  session.grant_epoch = 23;
  session.policy_generation = 25;
  server::ApplyRequestedLanguageProfile(&session, "en");
  *session_uuid = session.session_uuid;
  registry.sessions_by_uuid[server::UuidBytesToText(session.session_uuid)] = session;
  return registry;
}

server::HostedEngineState MakeEngineState() {
  server::HostedEngineState state;
  state.engine_context_active = true;
  server::HostedDatabaseSnapshot database;
  database.state = server::HostedDatabaseState::kOpen;
  database.database_open = true;
  database.database_path = "/tmp/sbsql_server_language_session_control_conformance.sbdb";
  database.database_uuid = "019e0a8c-f010-7000-8000-000000000008";
  state.databases.push_back(database);
  return state;
}

sbps::Frame PrepareFrame(const std::array<std::uint8_t, 16>& session_uuid,
                         const std::string& encoded) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kPrepareSblr);
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.session_uuid = session_uuid;
  frame.payload = server::EncodePrepareSblrPayloadForTest(session_uuid, encoded);
  return frame;
}

sbps::Frame ExecuteFrame(const std::array<std::uint8_t, 16>& session_uuid,
                         const std::array<std::uint8_t, 16>& prepared_uuid,
                         const std::string& encoded) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kExecuteSblr);
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.session_uuid = session_uuid;
  frame.payload = server::EncodeExecuteSblrPayloadForTest(
      session_uuid, prepared_uuid, encoded);
  return frame;
}

sbps::Frame ExecuteFrame(const std::array<std::uint8_t, 16>& session_uuid,
                         const std::string& encoded) {
  return ExecuteFrame(session_uuid, {}, encoded);
}

std::string PayloadText(const server::SessionOperationResult& result) {
  return std::string(result.payload.begin(), result.payload.end());
}

bool PayloadContains(const server::SessionOperationResult& result,
                     std::string_view needle) {
  return Contains(PayloadText(result), needle);
}

bool HasDiagnostic(const server::SessionOperationResult& result,
                   std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

void VerifyServerOwnsLanguageSessionControl() {
  std::array<std::uint8_t, 16> session_uuid{};
  auto registry = MakeRegistry(&session_uuid);
  const auto engine_state = MakeEngineState();

  const auto prepare_frame = PrepareFrame(session_uuid, QueryEnvelope());
  const auto prepare = server::HandlePrepareSblr(&registry, engine_state, prepare_frame);
  Require(prepare.accepted, "baseline prepare was not accepted");
  const auto prepared_uuid = server::DecodePreparedStatementUuidForTest(prepare.payload);
  Require(prepared_uuid.has_value(), "baseline prepare did not return prepared UUID");

  const auto set_frame =
      ExecuteFrame(session_uuid, LowerLanguageControl("SET LANGUAGE PROFILE fr_CA;"));
  const auto set = server::HandleExecuteSblr(&registry, engine_state, set_frame);
  Require(set.accepted, "server did not accept SET LANGUAGE execution");
  Require(PayloadContains(set, "server_session_language_context_authority=true"),
          "SET LANGUAGE was not executed by server session-language authority");
  Require(PayloadContains(set, "parser_updates_session_language=false"),
          "SET LANGUAGE result claimed parser-side session mutation");
  Require(PayloadContains(set, "prepared_statement_reinterpretation=false"),
          "SET LANGUAGE result allowed prepared statement reinterpretation");
  Require(PayloadContains(set, "language_profile_id=sbsql.language-profile.fr_CA"),
          "SET LANGUAGE did not update profile id");
  Require(PayloadContains(set, "language_tag=fr_CA"),
          "SET LANGUAGE did not update language tag");
  Require(PayloadContains(set, "input_language_fallback_tag=en"),
          "SET LANGUAGE did not retain standard SBsql fallback");

  const auto session_it = registry.sessions_by_uuid.find(server::UuidBytesToText(session_uuid));
  Require(session_it != registry.sessions_by_uuid.end(), "mutated session disappeared");
  const auto language = server::ServerLanguageContextForSession(session_it->second);
  Require(language.language_profile_id == "sbsql.language-profile.fr_CA",
          "registry session language profile was not mutated");
  Require(language.language_tag == "fr_CA",
          "registry session language tag was not mutated");
  Require(language.input_language_fallback_tag == "en",
          "registry session did not preserve English fallback");
  Require(language.language_resource_epoch > 1 &&
              language.localized_name_epoch > 1 &&
              language.message_resource_epoch > 1,
          "language control did not advance resource epochs");

  const auto stale_frame = ExecuteFrame(session_uuid, *prepared_uuid, "");
  const auto stale = server::HandleExecuteSblr(&registry, engine_state, stale_frame);
  Require(!stale.accepted &&
              HasDiagnostic(stale, "PARSER_SERVER_IPC.PREPARED_STATEMENT_STALE"),
          "prepared statement was not invalidated after language context mutation");

  const auto show = server::HandleExecuteSblr(
      &registry, engine_state,
      ExecuteFrame(session_uuid, LowerLanguageControl("SHOW LANGUAGE;")));
  Require(show.accepted, "SHOW LANGUAGE did not execute");
  Require(PayloadContains(show, "result_kind=language.session_context.v1"),
          "SHOW LANGUAGE did not return language context rows");
  Require(PayloadContains(show, "mutated_session_language=false"),
          "SHOW LANGUAGE mutated session state");
  Require(PayloadContains(show, "language_profile_id=sbsql.language-profile.fr_CA"),
          "SHOW LANGUAGE did not report active profile");

  const auto reset = server::HandleExecuteSblr(
      &registry, engine_state,
      ExecuteFrame(session_uuid, LowerLanguageControl("RESET LANGUAGE;")));
  Require(reset.accepted, "RESET LANGUAGE did not execute");
  Require(PayloadContains(reset, "language_profile_id=sbsql.builtin.recovery.en"),
          "RESET LANGUAGE did not restore default profile");
  Require(PayloadContains(reset, "language_tag=en"),
          "RESET LANGUAGE did not restore default language tag");

  const auto reset_language =
      server::ServerLanguageContextForSession(
          registry.sessions_by_uuid[server::UuidBytesToText(session_uuid)]);
  Require(reset_language.language_profile_id == "sbsql.builtin.recovery.en",
          "registry did not retain reset language profile");
  Require(reset_language.language_tag == "en",
          "registry did not retain reset language tag");
  Require(reset_language.input_language_fallback_tag.empty(),
          "default English language context should not carry fallback tag");
}

void VerifyLanguageBundleOperationsAtServer() {
  std::array<std::uint8_t, 16> session_uuid{};
  auto registry = MakeRegistry(&session_uuid);
  const auto engine_state = MakeEngineState();
  const auto load = server::HandleExecuteSblr(
      &registry, engine_state,
      ExecuteFrame(session_uuid, LowerLanguageControl("LOAD LANGUAGE BUNDLE fr_ca_bundle;")));
  Require(!load.accepted &&
              HasDiagnostic(load, "PARSER_SERVER_IPC.LANGUAGE_BUNDLE_ADMISSION_REQUIRED"),
          "LOAD LANGUAGE BUNDLE did not fail closed at server admission");

  const auto validate = server::HandleExecuteSblr(
      &registry, engine_state,
      ExecuteFrame(session_uuid,
                   AdmittedLanguageBundleEnvelope(
                       "language.bundle.validate",
                       "bundle.es_ES",
                       "sbsql.language-profile.es_ES",
                       "es_ES")));
  Require(validate.accepted, "admitted VALIDATE LANGUAGE BUNDLE was not accepted");
  Require(PayloadContains(validate, "server_language_resource_registry_authority=true"),
          "VALIDATE LANGUAGE BUNDLE did not use server registry authority");
  Require(PayloadContains(validate, "mutated_language_bundle_registry=false"),
          "VALIDATE LANGUAGE BUNDLE mutated the registry");
  Require(registry.language_bundles_by_uuid.empty(),
          "VALIDATE LANGUAGE BUNDLE wrote a loaded bundle record");

  const auto load_admitted = server::HandleExecuteSblr(
      &registry, engine_state,
      ExecuteFrame(session_uuid,
                   AdmittedLanguageBundleEnvelope(
                       "language.bundle.load",
                       "bundle.es_ES",
                       "sbsql.language-profile.es_ES",
                       "es_ES")));
  Require(load_admitted.accepted, "admitted LOAD LANGUAGE BUNDLE was not accepted");
  Require(PayloadContains(load_admitted, "language_bundle_loaded"),
          "LOAD LANGUAGE BUNDLE did not report loaded outcome");
  Require(PayloadContains(load_admitted, "parser_language_library_admission=false"),
          "LOAD LANGUAGE BUNDLE claimed parser-side library admission");
  Require(PayloadContains(load_admitted, "mga_finality_claimed=false"),
          "LOAD LANGUAGE BUNDLE claimed MGA transaction finality");
  Require(registry.language_bundles_by_uuid.count("bundle.es_ES") == 1,
          "LOAD LANGUAGE BUNDLE did not register the bundle");

  const auto unload = server::HandleExecuteSblr(
      &registry, engine_state,
      ExecuteFrame(session_uuid,
                   AdmittedLanguageBundleEnvelope(
                       "language.bundle.unload",
                       "bundle.es_ES",
                       "sbsql.language-profile.es_ES",
                       "es_ES")));
  Require(unload.accepted, "inactive UNLOAD LANGUAGE BUNDLE was not accepted");
  Require(PayloadContains(unload, "language_bundle_unloaded"),
          "UNLOAD LANGUAGE BUNDLE did not report unloaded outcome");
  Require(registry.language_bundles_by_uuid.count("bundle.es_ES") == 0,
          "UNLOAD LANGUAGE BUNDLE did not remove the registry record");

  const auto load_active_profile = server::HandleExecuteSblr(
      &registry, engine_state,
      ExecuteFrame(session_uuid,
                   AdmittedLanguageBundleEnvelope(
                       "language.bundle.load",
                       "bundle.fr_CA",
                       "sbsql.language-profile.fr_CA",
                       "fr_CA")));
  Require(load_active_profile.accepted, "fr_CA bundle load was not accepted");

  const auto set = server::HandleExecuteSblr(
      &registry, engine_state,
      ExecuteFrame(session_uuid, LowerLanguageControl("SET LANGUAGE PROFILE fr_CA;")));
  Require(set.accepted, "SET LANGUAGE fr_CA did not execute before active unload test");

  const auto unload_active = server::HandleExecuteSblr(
      &registry, engine_state,
      ExecuteFrame(session_uuid,
                   AdmittedLanguageBundleEnvelope(
                       "language.bundle.unload",
                       "bundle.fr_CA",
                       "sbsql.language-profile.fr_CA",
                       "fr_CA")));
  Require(!unload_active.accepted &&
              HasDiagnostic(unload_active,
                            "PARSER_SERVER_IPC.LANGUAGE_BUNDLE_ACTIVE_PROFILE_IN_USE"),
          "active language bundle unload was not refused");

  const auto load_required = server::HandleExecuteSblr(
      &registry, engine_state,
      ExecuteFrame(session_uuid,
                   AdmittedLanguageBundleEnvelope(
                       "language.bundle.load",
                       "bundle.de_DE.required",
                       "sbsql.language-profile.de_DE",
                       "de_DE",
                       true)));
  Require(load_required.accepted, "required profile bundle load was not accepted");
  const auto unload_required = server::HandleExecuteSblr(
      &registry, engine_state,
      ExecuteFrame(session_uuid,
                   AdmittedLanguageBundleEnvelope(
                       "language.bundle.unload",
                       "bundle.de_DE.required",
                       "sbsql.language-profile.de_DE",
                       "de_DE",
                       true)));
  Require(!unload_required.accepted &&
              HasDiagnostic(unload_required,
                            "PARSER_SERVER_IPC.LANGUAGE_BUNDLE_REQUIRED_PROFILE"),
          "required language bundle unload was not refused");
}

}  // namespace

int main() {
  VerifyServerOwnsLanguageSessionControl();
  VerifyLanguageBundleOperationsAtServer();
  std::cout << "sbsql_server_language_session_control_conformance=passed\n";
  return EXIT_SUCCESS;
}
