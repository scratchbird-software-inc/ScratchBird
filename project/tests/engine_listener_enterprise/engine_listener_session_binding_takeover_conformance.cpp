// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "control_plane.hpp"
#include "session_registry.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

std::array<std::uint8_t, 16> Uuid(std::uint8_t seed) {
  std::array<std::uint8_t, 16> uuid{};
  for (std::size_t i = 0; i < uuid.size(); ++i) {
    uuid[i] = static_cast<std::uint8_t>(seed + i);
  }
  uuid[6] = static_cast<std::uint8_t>((uuid[6] & 0x0fu) | 0x70u);
  uuid[8] = static_cast<std::uint8_t>((uuid[8] & 0x3fu) | 0x80u);
  return uuid;
}

void Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

scratchbird::server::ServerSessionBindingReport ToServerReport(
    const scratchbird::listener::SessionBindingReportPayload& payload) {
  scratchbird::server::ServerSessionBindingReport report;
  report.attachment_id = payload.attachment_id;
  report.catalog_session_id = payload.catalog_session_id;
  report.transaction_uuid = payload.transaction_uuid;
  report.protocol_session_id = payload.protocol_session_id;
  report.authenticated_principal_id = payload.authenticated_principal_id;
  report.session_user_id = payload.session_user_id;
  report.active_role_id = payload.active_role_id;
  report.authkey_id = payload.authkey_id;
  report.current_txn_id = payload.current_txn_id;
  report.effective_group_ids = payload.effective_group_ids;
  return report;
}

scratchbird::server::ServerSessionTakeoverRequest ToServerRequest(
    const scratchbird::listener::TakeoverRequestPayload& payload) {
  scratchbird::server::ServerSessionTakeoverRequest request;
  request.mask = payload.mask;
  request.attachment_id = payload.attachment_id;
  request.catalog_session_id = payload.catalog_session_id;
  request.protocol_session_id = payload.protocol_session_id;
  request.authkey_id = payload.authkey_id;
  request.authenticated_principal_id = payload.authenticated_principal_id;
  request.session_user_id = payload.session_user_id;
  request.active_role_id = payload.active_role_id;
  request.current_txn_id = payload.current_txn_id;
  request.group_ids = payload.group_ids;
  return request;
}

scratchbird::server::ServerSessionControlAuthority Authority(std::uint64_t sequence) {
  scratchbird::server::ServerSessionControlAuthority authority;
  authority.authenticated = true;
  authority.may_report_binding = true;
  authority.may_clear_binding = true;
  authority.may_takeover = true;
  authority.sequence = sequence;
  authority.authority_class = "listener_manager";
  authority.actor_token = "manager-session-eler063";
  return authority;
}

scratchbird::server::ServerSessionRecord ActiveSession() {
  scratchbird::server::ServerSessionRecord session;
  session.connection_uuid = Uuid(0x10);
  session.session_uuid = Uuid(0x20);
  session.auth_context_uuid = Uuid(0x30);
  session.principal_uuid = Uuid(0x40);
  session.effective_user_uuid = Uuid(0x50);
  session.principal_claim = "alice";
  session.provider_family = "local_password";
  session.database_path = "/tmp/eler063.sbdb";
  session.database_uuid = "db-eler063";
  session.local_transaction_id = 77;
  session.transaction_uuid = scratchbird::server::UuidBytesToText(Uuid(0x60));
  return session;
}

void ProveControlPlaneCodecCompatibility() {
  static_assert(scratchbird::listener::kTakeoverClaimAttachmentId ==
                scratchbird::server::kServerTakeoverClaimAttachmentId);
  static_assert(scratchbird::listener::kTakeoverClaimCatalogSessionId ==
                scratchbird::server::kServerTakeoverClaimCatalogSessionId);
  static_assert(scratchbird::listener::kTakeoverClaimProtocolSessionId ==
                scratchbird::server::kServerTakeoverClaimProtocolSessionId);
  static_assert(scratchbird::listener::kTakeoverClaimAuthkeyId ==
                scratchbird::server::kServerTakeoverClaimAuthkeyId);
  static_assert(scratchbird::listener::kTakeoverClaimAuthenticatedPrincipalId ==
                scratchbird::server::kServerTakeoverClaimAuthenticatedPrincipalId);
  static_assert(scratchbird::listener::kTakeoverClaimSessionUserId ==
                scratchbird::server::kServerTakeoverClaimSessionUserId);
  static_assert(scratchbird::listener::kTakeoverClaimActiveRoleId ==
                scratchbird::server::kServerTakeoverClaimActiveRoleId);
  static_assert(scratchbird::listener::kTakeoverClaimCurrentTxnId ==
                scratchbird::server::kServerTakeoverClaimCurrentTxnId);

  scratchbird::listener::TakeoverRequestPayload empty;
  auto decoded_empty = scratchbird::listener::DecodeTakeoverRequestPayload(
      scratchbird::listener::EncodeTakeoverRequestPayload(empty), nullptr);
  Require(!decoded_empty.has_value(), "empty TAKEOVER_REQUEST must be refused by codec");
}

void ProveBindingTakeoverAndClear() {
  scratchbird::server::ServerSessionRegistry registry;
  auto session = ActiveSession();
  const auto session_key = scratchbird::server::UuidBytesToText(session.session_uuid);
  registry.sessions_by_uuid[session_key] = session;
  registry.auth_contexts_by_uuid[scratchbird::server::UuidBytesToText(session.auth_context_uuid)] = session;
  registry.channel_state = scratchbird::server::ServerChannelState::kReady;

  scratchbird::listener::SessionBindingReportPayload binding;
  binding.attachment_id = session.connection_uuid;
  binding.catalog_session_id = session.session_uuid;
  binding.transaction_uuid = Uuid(0x60);
  binding.protocol_session_id = Uuid(0x70);
  binding.authenticated_principal_id = session.principal_uuid;
  binding.session_user_id = session.effective_user_uuid;
  binding.active_role_id = Uuid(0x80);
  binding.authkey_id = Uuid(0x90);
  binding.current_txn_id = session.local_transaction_id;
  binding.effective_group_ids = {Uuid(0xa0), Uuid(0xb0)};
  const auto decoded_binding = scratchbird::listener::DecodeSessionBindingReportPayload(
      scratchbird::listener::EncodeSessionBindingReportPayload(binding), nullptr);
  Require(decoded_binding.has_value(), "SESSION_BINDING_REPORT must decode before server application");

  auto applied = scratchbird::server::ApplyServerSessionBindingReport(
      &registry, ToServerReport(*decoded_binding), Authority(10));
  Require(applied.accepted && applied.mutated, "SESSION_BINDING_REPORT must mutate registry state");
  auto& bound = registry.sessions_by_uuid.at(session_key);
  Require(bound.session_binding_present, "session binding flag was not set");
  Require(bound.protocol_session_id == binding.protocol_session_id,
          "protocol session id was not stored");
  Require(bound.effective_group_uuids.size() == 2,
          "effective groups were not stored");
  Require(registry.channel_state == scratchbird::server::ServerChannelState::kSessionBound,
          "binding report must move channel to session_bound");
  Require(scratchbird::server::SessionRegistryStatusJson(registry)
              .find("\"session_binding_present\":true") != std::string::npos,
          "status JSON must expose live session binding state");

  auto replay = scratchbird::server::ApplyServerSessionBindingReport(
      &registry, ToServerReport(*decoded_binding), Authority(10));
  Require(!replay.accepted && replay.replay_rejected,
          "replayed binding sequence must be refused");

  auto wrong_binding = *decoded_binding;
  wrong_binding.authenticated_principal_id = Uuid(0xc0);
  auto wrong_principal = scratchbird::server::ApplyServerSessionBindingReport(
      &registry, ToServerReport(wrong_binding), Authority(11));
  Require(!wrong_principal.accepted &&
              wrong_principal.diagnostic_code == "SERVER.SESSION_BINDING.PRINCIPAL_MISMATCH",
          "binding report with wrong principal must be refused");

  scratchbird::listener::TakeoverRequestPayload takeover;
  takeover.mask = scratchbird::listener::kTakeoverClaimAttachmentId |
                  scratchbird::listener::kTakeoverClaimCatalogSessionId |
                  scratchbird::listener::kTakeoverClaimProtocolSessionId |
                  scratchbird::listener::kTakeoverClaimAuthkeyId |
                  scratchbird::listener::kTakeoverClaimAuthenticatedPrincipalId |
                  scratchbird::listener::kTakeoverClaimSessionUserId |
                  scratchbird::listener::kTakeoverClaimActiveRoleId |
                  scratchbird::listener::kTakeoverClaimCurrentTxnId;
  takeover.attachment_id = Uuid(0xd0);
  takeover.catalog_session_id = session.session_uuid;
  takeover.protocol_session_id = Uuid(0xe0);
  takeover.authkey_id = binding.authkey_id;
  takeover.authenticated_principal_id = session.principal_uuid;
  takeover.session_user_id = session.effective_user_uuid;
  takeover.active_role_id = binding.active_role_id;
  takeover.current_txn_id = session.local_transaction_id;
  takeover.group_ids = {binding.effective_group_ids.front()};
  const auto decoded_takeover = scratchbird::listener::DecodeTakeoverRequestPayload(
      scratchbird::listener::EncodeTakeoverRequestPayload(takeover), nullptr);
  Require(decoded_takeover.has_value(), "TAKEOVER_REQUEST must decode before server application");

  auto probe = scratchbird::server::EvaluateServerSessionTakeoverProbe(
      registry, ToServerRequest(*decoded_takeover), Authority(20));
  Require(probe.accepted && probe.takeover_allowed,
          "TAKEOVER_PROBE must report the takeover would pass");
  Require((probe.probe_flags & scratchbird::server::kServerTakeoverProbeSessionBound) != 0,
          "TAKEOVER_PROBE must expose session-bound state");
  Require((probe.probe_flags & scratchbird::server::kServerTakeoverProbeActiveTransaction) != 0,
          "TAKEOVER_PROBE must expose active transaction state");

  auto takeover_result = scratchbird::server::ApplyServerSessionTakeoverRequest(
      &registry, ToServerRequest(*decoded_takeover), Authority(20));
  Require(takeover_result.accepted && takeover_result.mutated &&
              takeover_result.takeover_allowed,
          "TAKEOVER_REQUEST must mutate session ownership");
  const auto& taken = registry.sessions_by_uuid.at(session_key);
  Require(taken.connection_uuid == takeover.attachment_id,
          "TAKEOVER_REQUEST must move attachment ownership");
  Require(taken.protocol_session_id == takeover.protocol_session_id,
          "TAKEOVER_REQUEST must move protocol session ownership");
  Require(taken.takeover_generation == 1 && taken.takeover_control_sequence == 20,
          "TAKEOVER_REQUEST must publish generation and replay sequence");

  auto takeover_replay = scratchbird::server::ApplyServerSessionTakeoverRequest(
      &registry, ToServerRequest(*decoded_takeover), Authority(20));
  Require(!takeover_replay.accepted && takeover_replay.replay_rejected,
          "replayed TAKEOVER_REQUEST must be refused");

  auto unauthorized = Authority(21);
  unauthorized.may_takeover = false;
  auto denied = scratchbird::server::ApplyServerSessionTakeoverRequest(
      &registry, ToServerRequest(*decoded_takeover), unauthorized);
  Require(!denied.accepted && denied.authorization_denied,
          "TAKEOVER_REQUEST without rights must be refused");

  auto bad_txn = *decoded_takeover;
  bad_txn.current_txn_id += 1;
  auto mismatch = scratchbird::server::ApplyServerSessionTakeoverRequest(
      &registry, ToServerRequest(bad_txn), Authority(22));
  Require(!mismatch.accepted &&
              mismatch.diagnostic_code == "SERVER.SESSION_TAKEOVER.CLAIM_MISMATCH",
          "TAKEOVER_REQUEST with wrong transaction id must be refused");

  scratchbird::listener::TakeoverRequestPayload clear_payload;
  clear_payload.mask = scratchbird::listener::kTakeoverClaimCatalogSessionId;
  clear_payload.catalog_session_id = session.session_uuid;
  auto clear = scratchbird::server::ClearServerSessionBinding(
      &registry, ToServerRequest(clear_payload), Authority(30));
  Require(clear.accepted && clear.mutated, "SESSION_BINDING_CLEAR must mutate registry state");
  Require(!registry.sessions_by_uuid.at(session_key).session_binding_present,
          "SESSION_BINDING_CLEAR must remove binding state");
  Require(registry.channel_state == scratchbird::server::ServerChannelState::kReady,
          "SESSION_BINDING_CLEAR must return channel to ready");

  auto clear_replay = scratchbird::server::ClearServerSessionBinding(
      &registry, ToServerRequest(clear_payload), Authority(30));
  Require(!clear_replay.accepted && clear_replay.replay_rejected,
          "replayed SESSION_BINDING_CLEAR must be refused");
}

}  // namespace

int main() {
  ProveControlPlaneCodecCompatibility();
  ProveBindingTakeoverAndClear();
  std::cout << "engine_listener_session_binding_takeover_conformance=passed\n";
  return EXIT_SUCCESS;
}
