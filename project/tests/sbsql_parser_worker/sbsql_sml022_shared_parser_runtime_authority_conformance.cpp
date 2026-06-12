// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "auth/auth_relay.hpp"
#include "cache/sblr_template_cache.hpp"
#include "sblr_admission.hpp"
#include "sblr_dispatch_server.hpp"
#include "session_registry.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace {

namespace parser = scratchbird::parser::sbsql;
namespace server = scratchbird::server;
namespace sbps = scratchbird::server::sbps;

struct AuthorityProofRow {
  std::string_view route;
  std::string_view surface;
  std::string_view mediation_point;
  std::string_view required_server_authority;
  bool parser_can_authenticate = false;
  bool parser_can_authorize = false;
  bool parser_can_mint_uuid = false;
  bool parser_can_own_finality = false;
  bool parser_can_bypass_sblr_validation = false;
};

constexpr std::array<AuthorityProofRow, 9> kAuthorityRows{{
    {"ipc", "SBPS prepare/execute", "server session registry",
     "request lifecycle and finality token"},
    {"session", "session binding", "server channel/session registry",
     "authenticated session context"},
    {"channel", "listener worker channel", "server channel state",
     "admitted SBPS channel"},
    {"auth", "auth handoff", "auth relay fail-closed path",
     "engine authentication provider"},
    {"authz", "security.authorize SBLR", "server SBLR admission",
     "engine security context"},
    {"diagnostic", "message vector", "server diagnostic shaping",
     "public diagnostic fields"},
    {"data_shape", "result_shape/diagnostic_shape", "server envelope validator",
     "SBLR descriptor and result contract"},
    {"resolver_cache", "resolved names to UUID claims", "parser cache epoch key",
     "server catalog/name-resolution epochs"},
    {"prepared_cache", "prepared SBLR", "server prepared registry",
     "security descriptor grant policy epochs"},
}};

void Require(bool condition, std::string_view message) {
  if (condition) return;
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

bool Contains(std::string_view value, std::string_view needle) {
  return value.find(needle) != std::string_view::npos;
}

bool HasParserDiagnostic(const parser::MessageVectorSet& messages,
                         std::string_view code) {
  for (const auto& diagnostic : messages.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

bool HasServerDiagnostic(const std::vector<server::ServerDiagnostic>& diagnostics,
                         std::string_view code) {
  for (const auto& diagnostic : diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

bool IsZeroUuid(const std::array<std::uint8_t, 16>& uuid) {
  return uuid == std::array<std::uint8_t, 16>{};
}

std::array<std::uint8_t, 16> FixedUuid(std::uint8_t seed) {
  std::array<std::uint8_t, 16> uuid{};
  for (std::size_t index = 0; index < uuid.size(); ++index) {
    uuid[index] = static_cast<std::uint8_t>(seed + index);
  }
  uuid[6] = static_cast<std::uint8_t>((uuid[6] & 0x0fu) | 0x70u);
  uuid[8] = static_cast<std::uint8_t>((uuid[8] & 0x3fu) | 0x80u);
  return uuid;
}

server::HostedEngineState MakeEngineState() {
  server::HostedEngineState state;
  state.engine_context_active = true;
  server::HostedDatabaseSnapshot database;
  database.state = server::HostedDatabaseState::kOpen;
  database.database_open = true;
  database.database_path = "/tmp/sbsql_sml022_shared_parser_runtime_authority.sbdb";
  database.database_uuid = "019f0220-0000-7000-8000-000000000022";
  state.databases.push_back(database);
  return state;
}

server::ServerSessionRegistry MakeRegistry(std::array<std::uint8_t, 16>* session_uuid) {
  server::ServerSessionRegistry registry;
  server::ServerSessionRecord session;
  session.connection_uuid = FixedUuid(0x10);
  session.session_uuid = FixedUuid(0x20);
  session.auth_context_uuid = FixedUuid(0x30);
  session.principal_uuid = FixedUuid(0x40);
  session.effective_user_uuid = session.principal_uuid;
  session.principal_claim = "sml022-user";
  session.provider_family = "local_password";
  session.database_path = "/tmp/sbsql_sml022_shared_parser_runtime_authority.sbdb";
  session.database_uuid = "019f0220-0000-7000-8000-000000000022";
  session.catalog_generation = 1;
  session.security_epoch = 1;
  session.descriptor_epoch = 1;
  session.grant_epoch = 1;
  session.policy_generation = 1;
  session.cache_invalidation_epoch = 16;
  session.name_resolution_epoch = 17;
  session.resource_epoch = 18;
  session.language_resource_epoch = 19;
  session.localized_name_epoch = 20;
  session.message_resource_epoch = 21;
  session.local_transaction_id = 1001;
  session.snapshot_visible_through_local_transaction_id = 1001;
  session.transaction_uuid = "019f0220-0000-7000-8000-000000000123";
  session.transaction_timestamp = "2026-06-12T00:00:00Z";
  *session_uuid = session.session_uuid;
  registry.channel_state = server::ServerChannelState::kReady;
  registry.sessions_by_uuid[server::UuidBytesToText(session.session_uuid)] = session;
  return registry;
}

sbps::Frame PrepareFrame(const std::array<std::uint8_t, 16>& session_uuid,
                         const std::string& encoded) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kPrepareSblr);
  frame.header.session_uuid = session_uuid;
  frame.header.connection_uuid = FixedUuid(0x10);
  frame.header.request_uuid = {};
  frame.payload = server::EncodePrepareSblrPayloadForTest(session_uuid, encoded);
  return frame;
}

sbps::Frame ExecutePreparedFrame(const std::array<std::uint8_t, 16>& session_uuid,
                                 const std::array<std::uint8_t, 16>& prepared_uuid) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kExecuteSblr);
  frame.header.session_uuid = session_uuid;
  frame.header.connection_uuid = FixedUuid(0x10);
  frame.header.request_uuid = {};
  frame.payload = server::EncodeExecuteSblrPayloadForTest(session_uuid, prepared_uuid, "");
  return frame;
}

std::string ParserJsonEnvelope() {
  return "{\"envelope\":\"SBLRExecutionEnvelope.v3\","
         "\"operation_family\":\"sblr.query.relational.v3\","
         "\"operation_id\":\"query.evaluate_projection\","
         "\"surface_key\":\"sml022.shared_runtime.query\","
         "\"sblr_operation_key\":\"op.sml022.shared_runtime.query\","
         "\"result_shape\":\"result.sml022.rowset.v1\","
         "\"diagnostic_shape\":\"diagnostic.sml022.message_vector.v1\","
         "\"resource_contract\":\"resource.sml022.shared_runtime.v1\","
         "\"trace_key\":\"SML-022\","
         "\"source_payload_embedded\":false,"
         "\"resolved_object_uuids\":[\"019f0220-0000-7000-8000-000000000201\"],"
         "\"descriptor_refs\":[\"sys.catalog.object_descriptor\","
         "\"sys.storage.row_descriptor\"],"
         "\"policy_refs\":[\"security.policy.sml022\"],"
         "\"required_authority_steps\":["
         "\"authority.server.resolve_name_registry_public\","
         "\"authority.server.security_policy_context_required\","
         "\"authority.server.transaction_context_required\","
         "\"authority.engine.mga_snapshot_visibility_required\","
         "\"authority.parser.no_security_authorization\","
         "\"authority.parser.no_storage_or_finality\","
         "\"authority.parser.no_sql_text_execution\"]}";
}

std::string TextOperationEnvelope(bool names_resolved_to_uuids,
                                  bool include_result_shape = true,
                                  bool include_diagnostic_shape = true) {
  std::string out;
  out += "envelope=SBLRExecutionEnvelope.v3\n";
  out += "operation_id=security.authorize\n";
  out += "sblr_operation_family=sblr.security.mutation_or_inspect.v3\n";
  if (include_result_shape) out += "result_shape=result.sml022.security_authority.v1\n";
  if (include_diagnostic_shape) {
    out += "diagnostic_shape=diagnostic.sml022.message_vector.v1\n";
  }
  out += "parser_resolved_names_to_uuids=";
  out += names_resolved_to_uuids ? "true\n" : "false\n";
  out += "authority.parser.no_security_authorization=true\n";
  out += "authority.server.security_policy_context_required=true\n";
  return out;
}

parser::CacheKey CacheKeyForEpochProof() {
  parser::CacheKey key;
  key.shape_hash = 2201;
  key.registry_version = 1;
  key.catalog_epoch = 11;
  key.security_policy_epoch = 12;
  key.grant_epoch = 14;
  key.descriptor_epoch = 13;
  key.udr_epoch = 1;
  key.name_resolution_epoch = 17;
  key.resource_epoch = 18;
  key.parser_package_generation = 1;
  key.protocol_version = 1;
  key.parser_package_version_hash = 2202;
  key.security_authority_epoch = 12;
  key.normalized_statement_hash = 2203;
  key.parameter_type_shape_hash = 2204;
  key.connection_uuid = "019f0220-0000-7000-8000-000000000301";
  key.transaction_context_hash = "mga:txn:1001";
  key.dialect = "sbsql";
  key.role_set_hash = "roles/sml022";
  key.group_set_hash = "groups/sml022";
  key.search_path_hash = "search/public";
  key.language_profile = "sbsql.builtin.recovery.en";
  key.language_tag = "en";
  key.input_syntax_profile = "sbsql.syntax.standard";
  key.common_resource_hash = "builtin.common.sbsql.v1";
  key.language_resource_epoch = 19;
  key.localized_name_epoch = 20;
  key.message_resource_epoch = 21;
  key.resource_compatibility_identity = "sbsql.resource.compat.v1";
  key.resource_version_identity = "sbsql.resource-pack.v1";
  key.result_contract_hash = "result.sml022.rowset.v1";
  return key;
}

parser::CacheEntry CacheEntryFor(parser::CacheKey key) {
  parser::CacheEntry entry;
  entry.key = std::move(key);
  entry.sblr_payload = ParserJsonEnvelope();
  entry.statement_family = "query";
  entry.operation_family = "query.evaluate_projection";
  entry.statement_hash = entry.key.normalized_statement_hash;
  return entry;
}

void VerifyAuthorityMatrixRows() {
  for (const auto& row : kAuthorityRows) {
    Require(!row.parser_can_authenticate,
            std::string(row.route) + " row grants parser authentication authority");
    Require(!row.parser_can_authorize,
            std::string(row.route) + " row grants parser authorization authority");
    Require(!row.parser_can_mint_uuid,
            std::string(row.route) + " row grants parser UUID minting authority");
    Require(!row.parser_can_own_finality,
            std::string(row.route) + " row grants parser finality authority");
    Require(!row.parser_can_bypass_sblr_validation,
            std::string(row.route) + " row grants parser SBLR validation bypass");
    Require(!row.mediation_point.empty() && !row.required_server_authority.empty(),
            std::string(row.route) + " row lost mediation evidence");
  }
}

void VerifyAuthRelayIsFailClosed() {
  parser::ParserConfig config;
  parser::AuthRelayRequest request;
  request.provider_id = "local_password";
  request.payload = "principal=sml022-user;credential=redacted";

  const auto no_endpoint = parser::FailClosedAuthRelay(request, config);
  Require(!no_endpoint.accepted && !no_endpoint.session.authenticated,
          "parser auth relay accepted authentication without server endpoint");
  Require(HasParserDiagnostic(no_endpoint.messages, "SBSQL.AUTH.SERVER_ENDPOINT_REQUIRED"),
          "parser auth relay did not emit server endpoint mediation diagnostic");

  config.server_endpoint = "ipc://sbsql-parser-test";
  const auto unavailable = parser::FailClosedAuthRelay(request, config);
  Require(!unavailable.accepted && !unavailable.session.authenticated,
          "parser auth relay accepted authentication when server relay was unavailable");
  Require(HasParserDiagnostic(unavailable.messages, "SBSQL.AUTH.SERVER_RELAY_UNAVAILABLE"),
          "parser auth relay did not emit server relay mediation diagnostic");

  const auto probe = parser::ProbeAuthRelay(request, config);
  Require(!probe.accepted && !probe.session.authenticated,
          "parser probe auth produced an authenticated session");
  Require(HasParserDiagnostic(probe.messages, "SBSQL.AUTH.ENGINE_AUTHORITY_REQUIRED"),
          "parser probe auth did not identify engine authentication authority");
}

void VerifyServerSblrAdmissionCannotBeBypassed() {
  const auto raw = server::AdmitServerSblrEnvelope(
      server::ServerSblrAdmissionRequest{"select * from parser_bypass_attempt", false});
  Require(!raw.admitted && HasServerDiagnostic(raw.diagnostics, "SBLR.SQL_TEXT_FORBIDDEN"),
          "server admitted raw SQL text as SBLR");

  const auto missing_shape = server::AdmitServerSblrEnvelope(
      server::ServerSblrAdmissionRequest{TextOperationEnvelope(true, false, true), false});
  Require(!missing_shape.admitted &&
              HasServerDiagnostic(missing_shape.diagnostics,
                                  "PARSER_SERVER_IPC.SBLR_REVALIDATION_FAILED"),
          "server admitted SBLR without result shape");
  Require(!missing_shape.diagnostics.empty() &&
              missing_shape.diagnostics.front().fields.front().value == "result_shape_required",
          "server result-shape diagnostic lost data-shaping detail");

  const auto unresolved = server::AdmitServerSblrEnvelope(
      server::ServerSblrAdmissionRequest{TextOperationEnvelope(false), false});
  Require(!unresolved.admitted &&
              HasServerDiagnostic(unresolved.diagnostics,
                                  "PARSER_SERVER_IPC.SBLR_REVALIDATION_FAILED"),
          "server admitted parser SBLR without UUID-resolved names");
  Require(!unresolved.diagnostics.empty() &&
              unresolved.diagnostics.front().fields.front().value ==
                  "names_not_resolved_to_uuids",
          "server UUID-resolution diagnostic lost mediation detail");

  const auto security = server::AdmitServerSblrEnvelope(
      server::ServerSblrAdmissionRequest{TextOperationEnvelope(true), false});
  Require(security.admitted &&
              security.operation_family == "sblr.security.mutation_or_inspect.v3" &&
              security.operation_id == "security.authorize",
          "server did not reclassify security authorization through SBLR authority");
}

void VerifyParserCacheRetainsNoAuthorityOrStaleResolverEntries() {
  parser::SblrTemplateCache cache(8);
  auto key = CacheKeyForEpochProof();

  auto refused = CacheEntryFor(key);
  refused.authorization_authority_cached = true;
  const auto refused_result = cache.StoreEntry(std::move(refused));
  Require(!refused_result.stored &&
              refused_result.diagnostic_code ==
                  "SB_ORH_FRONTDOOR_CACHE_AUTHORITY_REFUSAL.AUTHORIZATION_AUTHORITY_CACHED",
          "parser cache stored authorization authority");
  Require(Contains(refused_result.diagnostic_detail,
                   "parser cache cannot own storage visibility authorization finality rollback commit transaction inventory or recovery authority"),
          "parser cache refusal lost MGA authority diagnostic");
  Require(cache.Size() == 0, "parser cache retained refused authority entry");

  Require(cache.Store(key, ParserJsonEnvelope()).stored,
          "parser cache did not store mediation-only SBLR payload");
  auto name_changed = key;
  name_changed.name_resolution_epoch += 1;
  Require(!cache.Lookup(name_changed).has_value(),
          "parser cache reused stale name-resolution/UUID resolver entry");

  auto descriptor_changed = key;
  descriptor_changed.descriptor_epoch += 1;
  Require(!cache.Lookup(descriptor_changed).has_value(),
          "parser cache reused stale descriptor entry");

  auto security_changed = key;
  security_changed.security_policy_epoch += 1;
  Require(!cache.Lookup(security_changed).has_value(),
          "parser cache reused stale security-policy entry");

  auto grant_changed = key;
  grant_changed.grant_epoch += 1;
  Require(!cache.Lookup(grant_changed).has_value(),
          "parser cache reused stale grant entry");

  auto language_changed = key;
  language_changed.language_resource_epoch += 1;
  Require(!cache.Lookup(language_changed).has_value(),
          "parser cache reused stale language-resource entry");

  auto current = key;
  current.name_resolution_epoch += 2;
  current.normalized_statement_hash += 1;
  current.shape_hash += 1;
  Require(cache.Store(current, "payload/current").stored,
          "parser cache did not store current resolver epoch entry");
  cache.InvalidateNameResolutionEpoch(current.name_resolution_epoch);
  Require(!cache.Lookup(key).has_value(),
          "name-resolution invalidation retained older resolver cache entry");
  Require(cache.Lookup(current).value_or("") == "payload/current",
          "name-resolution invalidation removed current resolver cache entry");
}

void VerifyServerOwnsUuidMediationAndPreparedEpochs() {
  std::array<std::uint8_t, 16> session_uuid{};
  auto registry = MakeRegistry(&session_uuid);
  const auto engine_state = MakeEngineState();

  const auto prepare = server::HandlePrepareSblr(
      &registry, engine_state, PrepareFrame(session_uuid, ParserJsonEnvelope()));
  Require(prepare.accepted, "server prepare rejected mediation-only SBLR envelope");

  const auto prepared_uuid = server::DecodePreparedStatementUuidForTest(prepare.payload);
  Require(prepared_uuid.has_value() && !IsZeroUuid(*prepared_uuid),
          "server did not mint a prepared-statement UUID");
  Require(registry.prepared_by_uuid.count(server::UuidBytesToText(*prepared_uuid)) == 1,
          "server prepared registry did not own prepared UUID");
  Require(registry.requests_by_uuid.size() == 1,
          "server did not record prepare request lifecycle");

  const auto request = registry.requests_by_uuid.begin()->second;
  Require(!IsZeroUuid(request.request_uuid) && !IsZeroUuid(request.finality_token_uuid),
          "server did not mint request/finality UUIDs");
  Require(request.prepared_statement_uuid == *prepared_uuid,
          "server request lifecycle did not link prepared statement UUID");
  Require(request.transaction_finality_preserved,
          "server request lifecycle lost transaction finality preservation evidence");

  const auto finality_it =
      registry.finality_by_request_uuid.find(server::UuidBytesToText(request.request_uuid));
  Require(finality_it != registry.finality_by_request_uuid.end(),
          "server did not upsert finality record for prepare request");
  Require(finality_it->second.operation == "query.evaluate_projection" &&
              finality_it->second.state == "completed",
          "server finality record did not preserve completed engine/server authority");

  auto session_it = registry.sessions_by_uuid.find(server::UuidBytesToText(session_uuid));
  Require(session_it != registry.sessions_by_uuid.end(), "server session disappeared");
  session_it->second.security_epoch += 1;

  const auto stale_execute = server::HandleExecuteSblr(
      &registry, engine_state, ExecutePreparedFrame(session_uuid, *prepared_uuid));
  Require(!stale_execute.accepted &&
              HasServerDiagnostic(stale_execute.diagnostics,
                                  "PARSER_SERVER_IPC.PREPARED_STATEMENT_STALE"),
          "server executed prepared SBLR after security epoch changed");

  std::optional<server::ServerRequestRecord> stale_request;
  for (const auto& [_, record] : registry.requests_by_uuid) {
    if (record.detail == "prepared_statement_epoch_stale") {
      stale_request = record;
      break;
    }
  }
  Require(stale_request.has_value() &&
              stale_request->state == server::ServerRequestLifecycleState::kFailed &&
              stale_request->transaction_finality_preserved,
          "stale prepared execution did not fail closed with finality preserved");
}

}  // namespace

int main() {
  VerifyAuthorityMatrixRows();
  VerifyAuthRelayIsFailClosed();
  VerifyServerSblrAdmissionCannotBeBypassed();
  VerifyParserCacheRetainsNoAuthorityOrStaleResolverEntries();
  VerifyServerOwnsUuidMediationAndPreparedEpochs();
  std::cout << "sbsql_sml022_shared_parser_runtime_authority_conformance=passed\n";
  return EXIT_SUCCESS;
}
