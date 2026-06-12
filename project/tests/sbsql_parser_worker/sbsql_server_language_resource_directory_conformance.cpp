// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_dispatch_server.hpp"
#include "session_registry.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

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

server::ServerSessionRegistry MakeRegistry(std::array<std::uint8_t, 16>* session_uuid) {
  server::ServerSessionRegistry registry;
  server::ServerSessionRecord session;
  session.session_uuid = sbps::MakeUuidV7Bytes();
  session.auth_context_uuid = sbps::MakeUuidV7Bytes();
  session.principal_uuid = sbps::MakeUuidV7Bytes();
  session.effective_user_uuid = session.principal_uuid;
  session.principal_claim = "sml015-server-user";
  session.database_path =
      "/tmp/sbsql_server_language_resource_directory_conformance.sbdb";
  session.database_uuid = "019e0a8c-f015-7000-8000-000000000015";
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
  database.database_path =
      "/tmp/sbsql_server_language_resource_directory_conformance.sbdb";
  database.database_uuid = "019e0a8c-f015-7000-8000-000000000015";
  state.databases.push_back(database);
  return state;
}

std::string QueryEnvelope() {
  return "{\"envelope\":\"SBLRExecutionEnvelope.v3\","
         "\"operation_family\":\"sblr.query.relational.v3\","
         "\"surface_key\":\"sml015.fixture.query\","
         "\"operation_id\":\"query.evaluate_projection\","
         "\"sblr_operation_key\":\"sblr.query.relational.v3\","
         "\"result_shape\":\"result.shape.rowset\","
         "\"diagnostic_shape\":\"diagnostic.canonical_message_vector\","
         "\"resource_contract\":\"resource.contract.test\","
         "\"trace_key\":\"SML-015\","
         "\"source_payload_embedded\":false,"
         "\"resolved_object_uuids\":[],"
         "\"descriptor_refs\":[],"
         "\"policy_refs\":[]}";
}

std::string LanguageResourceDirectoryEnvelope(std::string_view operation_id,
                                              std::string_view manifest_hash,
                                              bool admitted = true,
                                              bool complete = true) {
  return "{\"envelope\":\"SBLRExecutionEnvelope.v3\","
         "\"operation_family\":\"sblr.language.resource_control.v3\","
         "\"surface_key\":\"sml015.fixture.language_resource_directory\","
         "\"operation_id\":\"" +
         std::string(operation_id) +
         "\","
         "\"sblr_operation_key\":\"sblr.language.resource_control.v3\","
         "\"result_shape\":\"result.shape.language_resource_directory_control\","
         "\"diagnostic_shape\":\"diagnostic.canonical_message_vector\","
         "\"resource_contract\":\"sbsql.language.resource_directory.v1\","
         "\"trace_key\":\"SML-015\","
         "\"source_payload_embedded\":false,"
         "\"language_resource_directory_operation\":true,"
         "\"language_resource_directory_manifest_attached\":" +
         (admitted ? std::string("true") : std::string("false")) +
         ","
         "\"language_resource_directory_signature_verified\":" +
         (admitted ? std::string("true") : std::string("false")) +
         ","
         "\"language_resource_directory_security_admitted\":" +
         (admitted ? std::string("true") : std::string("false")) +
         ","
         "\"language_resource_directory_compatible\":" +
         (admitted ? std::string("true") : std::string("false")) +
         ","
         "\"row_storage_touched\":false,"
         "\"mga_finality_claimed\":false,"
         "\"directory_id\":\"sbsql.language.resources.primary\","
         "\"directory_path\":\"/srv/scratchbird/language-resources\","
         "\"manifest_hash\":\"" +
         std::string(manifest_hash) +
         "\","
         "\"signing_key_id\":\"sbsql.langpack.signing.2026q2\","
         "\"scan_evidence_id\":\"sml015.scan.20260612\","
         "\"audit_reason\":\"investor_readiness_language_resource_reload\","
         "\"resolved_object_uuids\":[],"
         "\"descriptor_refs\":[\"sys.server_language_resource_directory\"],"
         "\"policy_refs\":[\"language_resource_directory_admission_policy\"],"
         "\"required_rights\":[\"right.language_resource_directory_admin\"],"
         "\"required_authority_steps\":["
         "\"authority.server.language_resource_directory_required\","
         "\"authority.security.language_resource_directory_admission_required\","
         "\"authority.parser.no_resource_directory_loading\"]}" +
         (complete ? std::string() : std::string(" "));
}

std::string IncompleteDirectoryEnvelope() {
  return "{\"envelope\":\"SBLRExecutionEnvelope.v3\","
         "\"operation_family\":\"sblr.language.resource_control.v3\","
         "\"surface_key\":\"sml015.fixture.language_resource_directory\","
         "\"operation_id\":\"language.resource_directory.scan\","
         "\"sblr_operation_key\":\"sblr.language.resource_control.v3\","
         "\"result_shape\":\"result.shape.language_resource_directory_control\","
         "\"diagnostic_shape\":\"diagnostic.canonical_message_vector\","
         "\"resource_contract\":\"sbsql.language.resource_directory.v1\","
         "\"trace_key\":\"SML-015\","
         "\"source_payload_embedded\":false,"
         "\"language_resource_directory_manifest_attached\":true,"
         "\"language_resource_directory_signature_verified\":true,"
         "\"language_resource_directory_security_admitted\":true,"
         "\"language_resource_directory_compatible\":true,"
         "\"row_storage_touched\":false,"
         "\"mga_finality_claimed\":false,"
         "\"directory_id\":\"sbsql.language.resources.primary\","
         "\"directory_path\":\"/srv/scratchbird/language-resources\","
         "\"manifest_hash\":\"sha256:sml015-incomplete\","
         "\"resolved_object_uuids\":[],"
         "\"descriptor_refs\":[\"sys.server_language_resource_directory\"],"
         "\"policy_refs\":[\"language_resource_directory_admission_policy\"]}";
}

std::string ShowDirectoryEnvelope() {
  return "{\"envelope\":\"SBLRExecutionEnvelope.v3\","
         "\"operation_family\":\"sblr.language.resource_control.v3\","
         "\"surface_key\":\"sml015.fixture.language_resource_directory\","
         "\"operation_id\":\"language.resource_directory.show\","
         "\"sblr_operation_key\":\"sblr.language.resource_control.v3\","
         "\"result_shape\":\"result.shape.language_resource_directory_control\","
         "\"diagnostic_shape\":\"diagnostic.canonical_message_vector\","
         "\"resource_contract\":\"sbsql.language.resource_directory.v1\","
         "\"trace_key\":\"SML-015\","
         "\"source_payload_embedded\":false,"
         "\"directory_id\":\"sbsql.language.resources.primary\","
         "\"resolved_object_uuids\":[],"
         "\"descriptor_refs\":[\"sys.server_language_resource_directory\"],"
         "\"policy_refs\":[\"language_resource_directory_show_policy\"]}";
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
  frame.payload =
      server::EncodeExecuteSblrPayloadForTest(session_uuid, prepared_uuid, encoded);
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

void VerifyServerLanguageResourceDirectoryAdmission() {
  std::array<std::uint8_t, 16> session_uuid{};
  auto registry = MakeRegistry(&session_uuid);
  const auto engine_state = MakeEngineState();

  const auto prepare = server::HandlePrepareSblr(
      &registry, engine_state, PrepareFrame(session_uuid, QueryEnvelope()));
  Require(prepare.accepted, "baseline prepare was not accepted");
  const auto prepared_uuid = server::DecodePreparedStatementUuidForTest(prepare.payload);
  Require(prepared_uuid.has_value(), "baseline prepare did not return prepared UUID");

  const auto missing_admission = server::HandleExecuteSblr(
      &registry, engine_state,
      ExecuteFrame(session_uuid,
                   LanguageResourceDirectoryEnvelope(
                       "language.resource_directory.scan",
                       "sha256:sml015-manifest-a",
                       false)));
  Require(!missing_admission.accepted &&
              HasDiagnostic(missing_admission,
                            "PARSER_SERVER_IPC.LANGUAGE_RESOURCE_DIRECTORY_ADMISSION_REQUIRED"),
          "directory scan without admitted manifest did not fail closed");

  const auto incomplete = server::HandleExecuteSblr(
      &registry, engine_state, ExecuteFrame(session_uuid, IncompleteDirectoryEnvelope()));
  Require(!incomplete.accepted &&
              HasDiagnostic(incomplete,
                            "PARSER_SERVER_IPC.LANGUAGE_RESOURCE_DIRECTORY_MANIFEST_INCOMPLETE"),
          "incomplete directory manifest did not fail closed");

  const auto scan = server::HandleExecuteSblr(
      &registry, engine_state,
      ExecuteFrame(session_uuid,
                   LanguageResourceDirectoryEnvelope(
                       "language.resource_directory.scan",
                       "sha256:sml015-manifest-a")));
  Require(scan.accepted, "admitted directory scan was not accepted");
  Require(PayloadContains(scan, "server_language_resource_directory_authority=true"),
          "directory scan did not use server directory authority");
  Require(PayloadContains(scan, "parser_language_library_admission=false"),
          "directory scan claimed parser-side library admission");
  Require(PayloadContains(scan, "load_or_reload_effects_executed_by_parser=false"),
          "directory scan claimed parser-side reload effects");
  Require(PayloadContains(scan, "cache_invalidated=true"),
          "directory scan did not invalidate language caches");
  Require(PayloadContains(scan, "directory_path_state=redacted"),
          "directory scan did not redact local path state");
  Require(!PayloadContains(scan, "/srv/scratchbird/language-resources"),
          "directory scan leaked the local resource path");
  Require(PayloadContains(scan, "mga_finality_claimed=false"),
          "directory scan claimed MGA transaction finality");
  Require(registry.language_resource_directories_by_id.count(
              "sbsql.language.resources.primary") == 1,
          "directory scan did not register server resource directory");

  const auto stale = server::HandleExecuteSblr(
      &registry, engine_state, ExecuteFrame(session_uuid, *prepared_uuid, ""));
  Require(!stale.accepted &&
              HasDiagnostic(stale, "PARSER_SERVER_IPC.PREPARED_STATEMENT_STALE"),
          "prepared statement was not invalidated after directory scan");

  const auto show = server::HandleExecuteSblr(
      &registry, engine_state, ExecuteFrame(session_uuid, ShowDirectoryEnvelope()));
  Require(show.accepted, "directory show was not accepted");
  Require(PayloadContains(show, "result_kind=language.resource_directory_registry.v1"),
          "directory show did not return registry packet");
  Require(PayloadContains(show, "mutated_language_resource_directory=false"),
          "directory show mutated registry state");
  Require(PayloadContains(show, "manifest_hash=sha256:sml015-manifest-a"),
          "directory show did not report the admitted manifest hash");
  Require(!PayloadContains(show, "/srv/scratchbird/language-resources"),
          "directory show leaked the local resource path");

  const auto before_reload =
      registry.sessions_by_uuid[server::UuidBytesToText(session_uuid)]
          .language_resource_epoch;
  const auto reload = server::HandleExecuteSblr(
      &registry, engine_state,
      ExecuteFrame(session_uuid,
                   LanguageResourceDirectoryEnvelope(
                       "language.resource_directory.reload",
                       "sha256:sml015-manifest-b")));
  Require(reload.accepted, "admitted directory reload was not accepted");
  Require(PayloadContains(reload, "language_resource_directory_reloaded"),
          "directory reload did not report reloaded outcome");
  Require(PayloadContains(reload, "manifest_hash=sha256:sml015-manifest-b"),
          "directory reload did not update manifest hash");
  const auto after_reload =
      registry.sessions_by_uuid[server::UuidBytesToText(session_uuid)]
          .language_resource_epoch;
  Require(after_reload > before_reload,
          "directory reload did not advance the language resource epoch");
}

}  // namespace

int main() {
  VerifyServerLanguageResourceDirectoryAdmission();
  std::cout << "sbsql_server_language_resource_directory_conformance=passed\n";
  return EXIT_SUCCESS;
}
