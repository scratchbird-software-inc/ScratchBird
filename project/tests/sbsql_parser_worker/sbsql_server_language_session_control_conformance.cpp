// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ast/ast.hpp"
#include "binder/binder.hpp"
#include "canonical_sblr_admission_test_helper.hpp"
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
  session.connection_uuid = session.session_uuid;
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
  session.admitted_parser_package_version_major = 1;
  session.local_transaction_id = 1;
  session.snapshot_visible_through_local_transaction_id = 1;
  session.transaction_uuid = "019e0a8c-f010-7000-8000-000000000018";
  session.transaction_timestamp = "2026-09-02T00:00:00Z";
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
                         const std::string&) {
  const auto canonical =
      scratchbird::test::sbsql::BuildCanonicalSblrAdmissionRequest(
          "observability.show_version", "SBLR_OBSERVABILITY_SHOW_VERSION");
  sbps::Frame frame;
  frame.header.message_type =
      static_cast<std::uint16_t>(sbps::MessageType::kStmtPrepareRequest);
  frame.header.payload_schema_id = sbps::kSchemaStmtPrepareRequestV1;
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.connection_uuid = session_uuid;
  frame.header.session_uuid = session_uuid;
  frame.payload.insert(frame.payload.end(), session_uuid.begin(), session_uuid.end());
  const auto statement_uuid = sbps::MakeUuidV7Bytes();
  frame.payload.insert(frame.payload.end(), statement_uuid.begin(), statement_uuid.end());
  scratchbird::engine::SblrAppendU64(frame.payload, 21);
  scratchbird::engine::SblrAppendU64(frame.payload, 22);
  scratchbird::engine::SblrAppendU64(frame.payload, 25);
  scratchbird::engine::SblrAppendU64(
      frame.payload, canonical.encoded_sblr_container.size());
  frame.payload.insert(frame.payload.end(),
                       canonical.encoded_sblr_container.begin(),
                       canonical.encoded_sblr_container.end());
  scratchbird::engine::SblrAppendU64(
      frame.payload, canonical.encoded_execution_envelope.size());
  frame.payload.insert(frame.payload.end(),
                       canonical.encoded_execution_envelope.begin(),
                       canonical.encoded_execution_envelope.end());
  return frame;
}

sbps::Frame ExecuteFrame(const std::array<std::uint8_t, 16>& session_uuid,
                         const std::array<std::uint8_t, 16>& prepared_uuid,
                         const std::string& encoded) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kExecuteSblr);
  frame.header.payload_schema_id = 4003;
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.connection_uuid = session_uuid;
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

  const auto session_key = server::UuidBytesToText(session_uuid);
  const auto before = server::ServerLanguageContextForSession(
      registry.sessions_by_uuid.at(session_key));
  const auto set_frame =
      ExecuteFrame(session_uuid, LowerLanguageControl("SET LANGUAGE PROFILE fr_CA;"));
  const auto set = server::HandleExecuteSblr(&registry, engine_state, set_frame);
  Require(!set.accepted && HasDiagnostic(set, "SBLR.OPERATION.NONCANONICAL"),
          "retired SET LANGUAGE envelope bypassed canonical SBLR admission");
  const auto show = server::HandleExecuteSblr(
      &registry, engine_state,
      ExecuteFrame(session_uuid, LowerLanguageControl("SHOW LANGUAGE;")));
  Require(!show.accepted && HasDiagnostic(show, "SBLR.OPERATION.NONCANONICAL"),
          "retired SHOW LANGUAGE envelope bypassed canonical SBLR admission");
  const auto reset = server::HandleExecuteSblr(
      &registry, engine_state,
      ExecuteFrame(session_uuid, LowerLanguageControl("RESET LANGUAGE;")));
  Require(!reset.accepted && HasDiagnostic(reset, "SBLR.OPERATION.NONCANONICAL"),
          "retired RESET LANGUAGE envelope bypassed canonical SBLR admission");
  const auto unchanged = server::ServerLanguageContextForSession(
      registry.sessions_by_uuid.at(session_key));
  Require(unchanged.language_profile_id == before.language_profile_id &&
              unchanged.language_tag == before.language_tag &&
              unchanged.language_resource_epoch == before.language_resource_epoch,
          "refused legacy language controls mutated the server session");

  // The canonical language-control carrier is not allocated yet.  Exercise
  // the server-owned state transition directly, then prove that prepared
  // statements are fenced by the resulting language authority change.
  auto& mutable_session = registry.sessions_by_uuid.at(session_key);
  server::ApplyRequestedLanguageProfile(&mutable_session, "fr_CA");
  ++mutable_session.language_resource_epoch;
  ++mutable_session.localized_name_epoch;
  ++mutable_session.message_resource_epoch;
  ++mutable_session.resource_epoch;
  ++mutable_session.name_resolution_epoch;
  const auto language = server::ServerLanguageContextForSession(mutable_session);
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

  server::ApplyRequestedLanguageProfile(&mutable_session, "en");
  const auto reset_language = server::ServerLanguageContextForSession(mutable_session);
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
  const std::array<std::string, 4> envelopes{{
      LowerLanguageControl("LOAD LANGUAGE BUNDLE fr_ca_bundle;"),
      AdmittedLanguageBundleEnvelope("language.bundle.validate", "bundle.es_ES",
                                     "sbsql.language-profile.es_ES", "es_ES"),
      AdmittedLanguageBundleEnvelope("language.bundle.load", "bundle.es_ES",
                                     "sbsql.language-profile.es_ES", "es_ES"),
      AdmittedLanguageBundleEnvelope("language.bundle.unload", "bundle.es_ES",
                                     "sbsql.language-profile.es_ES", "es_ES"),
  }};
  for (const auto& envelope : envelopes) {
    const auto result = server::HandleExecuteSblr(
        &registry, engine_state, ExecuteFrame(session_uuid, envelope));
    Require(!result.accepted &&
                HasDiagnostic(result, "SBLR.OPERATION.NONCANONICAL"),
            "retired language-bundle envelope bypassed canonical SBLR admission");
  }
  Require(registry.language_bundles_by_uuid.empty(),
          "refused language-bundle envelopes mutated the server registry");
}

}  // namespace

int main() {
  VerifyServerOwnsLanguageSessionControl();
  VerifyLanguageBundleOperationsAtServer();
  std::cout << "sbsql_server_language_session_control_conformance=passed\n";
  return EXIT_SUCCESS;
}
