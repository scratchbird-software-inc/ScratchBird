// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "session_registry.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using scratchbird::server::HostedDatabaseSnapshot;
using scratchbird::server::HostedDatabaseState;
using scratchbird::server::HostedEngineState;
using scratchbird::server::ServerChannelState;
using scratchbird::server::ServerSessionRegistry;
using scratchbird::server::SessionOperationResult;
namespace sbps = scratchbird::server::sbps;

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

void PutString(std::vector<std::uint8_t>* out, const std::string& value) {
  PutU16(out, static_cast<std::uint16_t>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
}

void PutUuid(std::vector<std::uint8_t>* out, const std::array<std::uint8_t, 16>& uuid) {
  out->insert(out->end(), uuid.begin(), uuid.end());
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

HostedEngineState MakeEngineState() {
  HostedEngineState state;
  state.engine_context_active = true;
  HostedDatabaseSnapshot database;
  database.state = HostedDatabaseState::kOpen;
  database.database_open = true;
  database.database_path = "/tmp/sb_sbsql_tls_transport_policy_conformance.sbdb";
  database.database_uuid = "019e05bf-f010-7000-8000-0000000000ab";
  state.databases.push_back(database);
  return state;
}

std::vector<std::uint8_t> AuthPayload(const std::array<std::uint8_t, 16>& connection_uuid,
                                      const std::string& evidence) {
  std::vector<std::uint8_t> out;
  PutUuid(&out, connection_uuid);
  out.push_back(1);  // credential_evidence_present
  out.push_back(0);  // credential_invalid
  out.push_back(0);  // mfa_required
  out.push_back(0);  // mfa_evidence_present
  PutString(&out, "local_password");
  PutString(&out, "benchmark_user");
  PutString(&out, "default");
  PutString(&out, "en");
  PutString(&out, evidence);
  return out;
}

SessionOperationResult DenyWithEvidence(const HostedEngineState& engine_state,
                                        const std::string& evidence) {
  ServerSessionRegistry registry;
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kAuthHandoff);
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.connection_uuid = sbps::MakeUuidV7Bytes();
  frame.payload = AuthPayload(frame.header.connection_uuid, evidence);
  auto result = scratchbird::server::HandleAuthHandoff(&registry, engine_state, frame);
  Require(!result.accepted, "TLS transport evidence denial unexpectedly authenticated");
  Require(registry.auth_contexts_by_uuid.empty(), "denied TLS transport proof created an auth context");
  Require(registry.sessions_by_uuid.empty(), "denied TLS transport proof created a session");
  Require(registry.channel_state == ServerChannelState::kFailed,
          "denied TLS transport proof did not fail the auth channel");
  return result;
}

std::string ValidPasswordEvidence() {
  return "scheme=local_password_v1;principal=benchmark_user;"
         "verifier=cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
}

}  // namespace

int main() {
  const auto engine_state = MakeEngineState();

  const auto wrong_ca = DenyWithEvidence(
      engine_state,
      ValidPasswordEvidence() + ";tls_client_cert_status=wrong_ca");
  Require(HasDiagnostic(wrong_ca, "SECURITY.AUTHENTICATION.TLS_CLIENT_CA_INVALID"),
          "wrong CA TLS proof did not emit stable denial diagnostic");
  Require(PayloadContains(wrong_ca, "tls_client_ca_invalid"),
          "wrong CA TLS proof did not return stable denial detail");

  const auto expired = DenyWithEvidence(
      engine_state,
      ValidPasswordEvidence() + ";tls_client_cert_status=expired");
  Require(HasDiagnostic(expired, "SECURITY.AUTHENTICATION.TLS_CLIENT_CERT_EXPIRED"),
          "expired TLS client certificate did not emit stable denial diagnostic");
  Require(PayloadContains(expired, "tls_client_cert_expired"),
          "expired TLS client certificate did not return stable denial detail");

  const auto channel_binding = DenyWithEvidence(
      engine_state,
      ValidPasswordEvidence() + ";tls_channel_binding_status=mismatch");
  Require(HasDiagnostic(channel_binding,
                        "SECURITY.AUTHENTICATION.TLS_CHANNEL_BINDING_MISMATCH"),
          "channel-binding mismatch did not emit stable denial diagnostic");
  Require(PayloadContains(channel_binding, "tls_channel_binding_mismatch"),
          "channel-binding mismatch did not return stable denial detail");

  std::cout << "sbsql_tls_transport_policy_conformance=passed\n";
  return EXIT_SUCCESS;
}
