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
#include <utility>
#include <vector>

namespace {

namespace server = scratchbird::server;
namespace sbps = scratchbird::server::sbps;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) Fail(message);
}

std::array<std::uint8_t, 16> Uuid(std::uint8_t seed) {
  std::array<std::uint8_t, 16> uuid{};
  for (std::size_t i = 0; i < uuid.size(); ++i) {
    uuid[i] = static_cast<std::uint8_t>(seed + i);
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
  database.database_path = "/tmp/ipar_prepared_execution_cache_authority.sbdb";
  database.database_uuid = "database-ipar-cache-authority";
  database.read_only = false;
  database.write_admission_fenced = false;
  state.databases.push_back(std::move(database));
  return state;
}

server::ServerSessionRecord MakeSession(std::uint8_t seed) {
  server::ServerSessionRecord session;
  session.connection_uuid = Uuid(seed);
  session.session_uuid = Uuid(static_cast<std::uint8_t>(seed + 16));
  session.auth_context_uuid = Uuid(static_cast<std::uint8_t>(seed + 32));
  session.principal_uuid = Uuid(static_cast<std::uint8_t>(seed + 48));
  session.effective_user_uuid = session.principal_uuid;
  session.principal_claim = "ipar-cache-user";
  session.database_path = "/tmp/ipar_prepared_execution_cache_authority.sbdb";
  session.database_uuid = "database-ipar-cache-authority";
  session.catalog_generation = 7;
  session.security_epoch = 11;
  session.descriptor_epoch = 13;
  session.grant_epoch = 17;
  session.policy_generation = 19;
  session.capability_policy_generation = 23;
  session.cache_invalidation_epoch = 29;
  session.name_resolution_epoch = 31;
  session.resource_epoch = 37;
  session.role_set_hash = "roles/ipar-cache";
  session.group_set_hash = "groups/ipar-cache";
  session.search_path_hash = "search/ipar-cache";
  session.local_transaction_id = 41;
  session.snapshot_visible_through_local_transaction_id = 41;
  session.transaction_uuid = "transaction-ipar-cache-authority";
  return session;
}

void AddSession(server::ServerSessionRegistry* registry,
                const server::ServerSessionRecord& session) {
  registry->sessions_by_uuid[server::UuidBytesToText(session.session_uuid)] =
      session;
}

sbps::Frame Frame(sbps::MessageType type,
                  std::vector<std::uint8_t> payload,
                  const std::array<std::uint8_t, 16>& session_uuid) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(type);
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.session_uuid = session_uuid;
  frame.payload = std::move(payload);
  return frame;
}

sbps::Frame ExecuteFrame(const std::array<std::uint8_t, 16>& session_uuid,
                         const std::string& encoded) {
  return Frame(sbps::MessageType::kExecuteSblr,
               server::EncodeExecuteSblrPayloadForTest(session_uuid, {}, encoded),
               session_uuid);
}

sbps::Frame PrepareFrame(const std::array<std::uint8_t, 16>& session_uuid,
                         const std::string& encoded) {
  return Frame(sbps::MessageType::kPrepareSblr,
               server::EncodePrepareSblrPayloadForTest(session_uuid, encoded),
               session_uuid);
}

sbps::Frame ExecutePreparedFrame(
    const std::array<std::uint8_t, 16>& session_uuid,
    const std::array<std::uint8_t, 16>& prepared_statement_uuid) {
  return Frame(sbps::MessageType::kExecuteSblr,
               server::EncodeExecuteSblrPayloadForTest(
                   session_uuid, prepared_statement_uuid, ""),
               session_uuid);
}

std::string UnsupportedEnvelope() {
  return "envelope=SBLRExecutionEnvelope.v2\n"
         "operation_id=observability.show_version\n"
         "sblr_operation_family=sblr.observability.inspect.v3\n"
         "result_shape=engine.api.result.v1\n"
         "diagnostic_shape=engine.diagnostic.v1\n"
         "parser_resolved_names_to_uuids=true\n"
         "requires_security_context=true\n"
         "requires_transaction_context=true\n"
         "requires_cluster_authority=false\n";
}

std::string WithCacheKey(std::string envelope,
                         std::string_view field,
                         const std::string& key) {
  if (!envelope.empty() && envelope.back() != '\n') envelope.push_back('\n');
  envelope += std::string(field) + "=" + key + "\n";
  return envelope;
}

bool HasDiagnostic(const server::SessionOperationResult& result,
                   std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

void RequireAccepted(const server::SessionOperationResult& result,
                     std::string_view message) {
  if (result.accepted) return;
  std::cerr << message << '\n';
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << "  diagnostic=" << diagnostic.code << '\n';
    for (const auto& field : diagnostic.fields) {
      std::cerr << "    " << field.key << '=' << field.value << '\n';
    }
  }
  std::exit(EXIT_FAILURE);
}

server::ServerRequestRecord RequireRequest(
    const server::ServerSessionRegistry& registry,
    const std::array<std::uint8_t, 16>& request_uuid) {
  const auto record =
      server::FindServerRequestLifecycle(registry,
                                         server::UuidBytesToText(request_uuid));
  Require(record.has_value(), "IPAR cache authority request lifecycle missing");
  return *record;
}

void RequireFailedBeforeDispatch(const server::ServerSessionRegistry& registry,
                                 const sbps::Frame& frame,
                                 const server::SessionOperationResult& result,
                                 std::string_view diagnostic_code,
                                 std::string_view detail) {
  Require(!result.accepted, "IPAR cache refusal was accepted");
  Require(HasDiagnostic(result, diagnostic_code),
          "IPAR cache refusal diagnostic mismatch");
  const auto request = RequireRequest(registry, frame.header.request_uuid);
  Require(request.state == server::ServerRequestLifecycleState::kFailed,
          "IPAR cache refusal request did not fail");
  Require(request.detail == detail, "IPAR cache refusal detail mismatch");
  Require(request.operation_id == "sblr.dispatch.pending",
          "IPAR cache refusal reached dispatch operation update");
}

const server::ServerAuthorityCacheRecord* FindCache(
    const server::ServerSessionRegistry& registry,
    std::string_view kind) {
  const server::ServerAuthorityCacheRecord* best = nullptr;
  for (const auto& [_, record] : registry.authority_cache_by_key) {
    if (record.cache_kind != kind) continue;
    if (best == nullptr || record.generation > best->generation) {
      best = &record;
    }
  }
  return best;
}

void ValidatePreparedExecutionContextIsScoped() {
  server::ServerSessionRegistry registry;
  const auto engine_state = MakeEngineState();
  const auto primary = MakeSession(0x10);
  AddSession(&registry, primary);

  const auto prepare = server::HandlePrepareSblr(
      &registry,
      engine_state,
      PrepareFrame(primary.session_uuid, server::EncodeShowVersionSblrForTest()));
  RequireAccepted(prepare, "IPAR prepared context prepare failed");
  const auto prepared_uuid =
      server::DecodePreparedStatementUuidForTest(prepare.payload);
  Require(prepared_uuid.has_value(), "IPAR prepared context UUID decode failed");

  const std::string prepared_key = server::UuidBytesToText(*prepared_uuid);
  const auto prepared_it = registry.prepared_by_uuid.find(prepared_key);
  Require(prepared_it != registry.prepared_by_uuid.end(),
          "IPAR prepared statement missing");
  const auto context_it =
      registry.prepared_execution_contexts_by_uuid.find(prepared_key);
  Require(context_it != registry.prepared_execution_contexts_by_uuid.end(),
          "IPAR prepared execution context missing");
  const auto& context = context_it->second;
  Require(context.session_uuid == primary.session_uuid &&
              context.auth_context_uuid == primary.auth_context_uuid,
          "IPAR prepared execution context did not bind session/auth context");
  Require(context.principal_uuid == primary.principal_uuid &&
              context.effective_user_uuid == primary.effective_user_uuid &&
              context.database_uuid == primary.database_uuid,
          "IPAR prepared execution context did not bind user/database scope");
  Require(context.epoch_vector.catalog_generation == primary.catalog_generation &&
              context.epoch_vector.security_epoch == primary.security_epoch &&
              context.epoch_vector.capability_policy_generation ==
                  primary.capability_policy_generation &&
              !context.statement_shape_hash.empty() &&
              !context.authority_proof_hash.empty() &&
              !context.grants_authority,
          "IPAR prepared execution context did not seal authority vector");

  const auto accepted_frame =
      ExecutePreparedFrame(primary.session_uuid, *prepared_uuid);
  const auto accepted =
      server::HandleExecuteSblr(&registry, engine_state, accepted_frame);
  RequireAccepted(accepted, "IPAR prepared context baseline execute failed");

  auto& mutable_context =
      registry.prepared_execution_contexts_by_uuid[prepared_key];
  mutable_context.grants_authority = true;
  const auto stale_frame =
      ExecutePreparedFrame(primary.session_uuid, *prepared_uuid);
  const auto stale =
      server::HandleExecuteSblr(&registry, engine_state, stale_frame);
  RequireFailedBeforeDispatch(registry,
                              stale_frame,
                              stale,
                              "PARSER_SERVER_IPC.PREPARED_STATEMENT_STALE",
                              "prepared_statement_epoch_stale");
}

void ValidateNegativeAuthorizationCacheRefusesOnly() {
  server::ServerSessionRegistry registry;
  const auto engine_state = MakeEngineState();
  const auto primary = MakeSession(0x30);
  const auto other = MakeSession(0x90);
  AddSession(&registry, primary);
  AddSession(&registry, other);

  const auto first_frame = ExecuteFrame(primary.session_uuid, UnsupportedEnvelope());
  const auto first = server::HandleExecuteSblr(&registry, engine_state, first_frame);
  RequireFailedBeforeDispatch(registry,
                              first_frame,
                              first,
                              "PARSER_SERVER_IPC.SBLR_ENVELOPE_VERSION_UNSUPPORTED",
                              "PARSER_SERVER_IPC.SBLR_ENVELOPE_VERSION_UNSUPPORTED");
  const auto* negative = FindCache(registry, "negative_authorization");
  Require(negative != nullptr && negative->refusal && !negative->grants_authority,
          "IPAR negative authorization cache was not recorded as refusal-only");
  const std::string negative_key = negative->cache_key;

  const auto second_frame = ExecuteFrame(primary.session_uuid, UnsupportedEnvelope());
  const auto second =
      server::HandleExecuteSblr(&registry, engine_state, second_frame);
  RequireFailedBeforeDispatch(registry,
                              second_frame,
                              second,
                              "PARSER_SERVER_IPC.SBLR_ENVELOPE_VERSION_UNSUPPORTED",
                              "unsupported_sblr_execution_envelope_version");
  Require(registry.authority_cache_by_key[negative_key].hit_count == 1,
          "IPAR implicit negative cache did not record a hit");

  const auto cross_session_frame = ExecuteFrame(
      other.session_uuid,
      WithCacheKey(UnsupportedEnvelope(),
                   "negative_authorization_cache_key",
                   negative_key));
  const auto cross_session =
      server::HandleExecuteSblr(&registry, engine_state, cross_session_frame);
  RequireFailedBeforeDispatch(registry,
                              cross_session_frame,
                              cross_session,
                              "PARSER_SERVER_IPC.AUTHORITY_CACHE_STALE",
                              "authority_cache_cross_session");

  auto& session = registry.sessions_by_uuid[server::UuidBytesToText(primary.session_uuid)];
  const auto original = session;
  session.role_set_hash = "roles/ipar-cache-mutated";
  const auto cross_role_frame = ExecuteFrame(
      primary.session_uuid,
      WithCacheKey(UnsupportedEnvelope(),
                   "negative_authorization_cache_key",
                   negative_key));
  const auto cross_role =
      server::HandleExecuteSblr(&registry, engine_state, cross_role_frame);
  RequireFailedBeforeDispatch(registry,
                              cross_role_frame,
                              cross_role,
                              "PARSER_SERVER_IPC.AUTHORITY_CACHE_STALE",
                              "authority_cache_authorization_hash_stale");
  session = original;

  session.security_epoch += 1;
  const auto stale_epoch_frame = ExecuteFrame(
      primary.session_uuid,
      WithCacheKey(UnsupportedEnvelope(),
                   "negative_authorization_cache_key",
                   negative_key));
  const auto stale_epoch =
      server::HandleExecuteSblr(&registry, engine_state, stale_epoch_frame);
  RequireFailedBeforeDispatch(registry,
                              stale_epoch_frame,
                              stale_epoch,
                              "PARSER_SERVER_IPC.AUTHORITY_CACHE_STALE",
                              "authority_cache_epoch_stale");
}

void ValidateCapabilityAndPreflightCachesNeverGrant() {
  server::ServerSessionRegistry registry;
  const auto engine_state = MakeEngineState();
  const auto primary = MakeSession(0x50);
  AddSession(&registry, primary);

  const std::string show_envelope = server::EncodeShowVersionSblrForTest();
  const auto first_show_frame = ExecuteFrame(primary.session_uuid, show_envelope);
  const auto first_show =
      server::HandleExecuteSblr(&registry, engine_state, first_show_frame);
  RequireAccepted(first_show, "IPAR capability baseline execute failed");
  const auto* capability = FindCache(registry, "capability_route");
  Require(capability != nullptr && !capability->refusal &&
              !capability->grants_authority,
          "IPAR capability cache was not recorded as non-authoritative");
  const std::string capability_key = capability->cache_key;

  const auto capability_hit_frame = ExecuteFrame(
      primary.session_uuid,
      WithCacheKey(show_envelope, "capability_cache_key", capability_key));
  const auto capability_hit =
      server::HandleExecuteSblr(&registry, engine_state, capability_hit_frame);
  RequireAccepted(capability_hit, "IPAR capability cache hit blocked dispatch");
  Require(registry.authority_cache_by_key[capability_key].hit_count == 1,
          "IPAR capability cache hit was not recorded");

  registry.authority_cache_by_key[capability_key].grants_authority = true;
  const auto grant_frame = ExecuteFrame(
      primary.session_uuid,
      WithCacheKey(show_envelope, "capability_cache_key", capability_key));
  const auto grant =
      server::HandleExecuteSblr(&registry, engine_state, grant_frame);
  RequireFailedBeforeDispatch(registry,
                              grant_frame,
                              grant,
                              "PARSER_SERVER_IPC.AUTHORITY_CACHE_STALE",
                              "authority_cache_grant_forbidden");
  registry.authority_cache_by_key[capability_key].grants_authority = false;

  auto& session = registry.sessions_by_uuid[server::UuidBytesToText(primary.session_uuid)];
  const auto original = session;
  session.role_set_hash = "roles/ipar-cache-capability-mutated";
  const auto capability_cross_role_frame = ExecuteFrame(
      primary.session_uuid,
      WithCacheKey(show_envelope, "capability_cache_key", capability_key));
  const auto capability_cross_role =
      server::HandleExecuteSblr(&registry,
                                engine_state,
                                capability_cross_role_frame);
  RequireFailedBeforeDispatch(registry,
                              capability_cross_role_frame,
                              capability_cross_role,
                              "PARSER_SERVER_IPC.AUTHORITY_CACHE_STALE",
                              "authority_cache_authorization_hash_stale");
  session = original;

  const std::string begin_envelope = server::EncodeBeginTransactionSblrForTest();
  const auto first_begin_frame = ExecuteFrame(primary.session_uuid, begin_envelope);
  const auto first_begin =
      server::HandleExecuteSblr(&registry, engine_state, first_begin_frame);
  RequireAccepted(first_begin, "IPAR preflight baseline execute failed");
  const auto* preflight = FindCache(registry, "statement_preflight");
  Require(preflight != nullptr && !preflight->refusal &&
              !preflight->grants_authority,
          "IPAR statement preflight cache was not recorded as non-authoritative");
  const std::string preflight_key = preflight->cache_key;

  const auto preflight_hit_frame = ExecuteFrame(
      primary.session_uuid,
      WithCacheKey(begin_envelope,
                   "statement_preflight_cache_key",
                   preflight_key));
  const auto preflight_hit =
      server::HandleExecuteSblr(&registry, engine_state, preflight_hit_frame);
  RequireAccepted(preflight_hit, "IPAR preflight cache hit blocked dispatch");
  Require(registry.authority_cache_by_key[preflight_key].hit_count == 1,
          "IPAR preflight cache hit was not recorded");

  session.cache_invalidation_epoch += 1;
  const auto preflight_stale_frame = ExecuteFrame(
      primary.session_uuid,
      WithCacheKey(begin_envelope,
                   "statement_preflight_cache_key",
                   preflight_key));
  const auto preflight_stale =
      server::HandleExecuteSblr(&registry, engine_state, preflight_stale_frame);
  RequireFailedBeforeDispatch(registry,
                              preflight_stale_frame,
                              preflight_stale,
                              "PARSER_SERVER_IPC.AUTHORITY_CACHE_STALE",
                              "authority_cache_epoch_stale");
}

}  // namespace

int main() {
  ValidatePreparedExecutionContextIsScoped();
  ValidateNegativeAuthorizationCacheRefusesOnly();
  ValidateCapabilityAndPreflightCachesNeverGrant();
  return EXIT_SUCCESS;
}
