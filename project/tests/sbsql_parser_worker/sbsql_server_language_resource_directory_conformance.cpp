// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_dispatch_server.hpp"
#include "session_registry.hpp"
#include "canonical_sblr_admission_test_helper.hpp"

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
  session.connection_uuid = session.session_uuid;
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
  session.admitted_parser_package_version_major = 1;
  session.local_transaction_id = 1;
  session.snapshot_visible_through_local_transaction_id = 1;
  session.transaction_uuid = "019e0a8c-f015-7000-8000-000000000018";
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

  const std::array<std::string, 5> envelopes{{
      LanguageResourceDirectoryEnvelope("language.resource_directory.scan",
                                        "sha256:sml015-manifest-a", false),
      IncompleteDirectoryEnvelope(),
      LanguageResourceDirectoryEnvelope("language.resource_directory.scan",
                                        "sha256:sml015-manifest-a"),
      ShowDirectoryEnvelope(),
      LanguageResourceDirectoryEnvelope("language.resource_directory.reload",
                                        "sha256:sml015-manifest-b"),
  }};
  for (const auto& envelope : envelopes) {
    const auto result = server::HandleExecuteSblr(
        &registry, engine_state, ExecuteFrame(session_uuid, envelope));
    Require(!result.accepted &&
                HasDiagnostic(result, "SBLR.OPERATION.NONCANONICAL"),
            "retired language-resource-directory envelope bypassed canonical admission");
  }
  Require(registry.language_resource_directories_by_id.empty(),
          "refused language-resource-directory envelopes mutated server state");

  // Until a canonical directory-control opcode and descriptor are allocated,
  // directory activation remains non-executable.  Still prove that a
  // server-owned language-resource epoch transition invalidates prepared
  // statements without reinterpreting their canonical bytes.
  auto& session = registry.sessions_by_uuid.at(server::UuidBytesToText(session_uuid));
  ++session.language_resource_epoch;
  ++session.localized_name_epoch;
  ++session.message_resource_epoch;
  ++session.resource_epoch;
  ++session.name_resolution_epoch;
  const auto stale = server::HandleExecuteSblr(
      &registry, engine_state, ExecuteFrame(session_uuid, *prepared_uuid, ""));
  Require(!stale.accepted &&
              HasDiagnostic(stale, "PARSER_SERVER_IPC.PREPARED_STATEMENT_STALE"),
          "prepared statement was not invalidated by a resource epoch change");
}

}  // namespace

int main() {
  VerifyServerLanguageResourceDirectoryAdmission();
  std::cout << "sbsql_server_language_resource_directory_conformance=passed\n";
  return EXIT_SUCCESS;
}
