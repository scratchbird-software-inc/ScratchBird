// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "listener_tls_policy.hpp"
#include "session_registry.hpp"
#include "sbps.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

void PutU16(std::vector<std::uint8_t>* out, std::uint16_t value) {
  out->push_back(static_cast<std::uint8_t>(value & 0xffu));
  out->push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
}

void PutUuid(std::vector<std::uint8_t>* out,
             const std::array<std::uint8_t, 16>& uuid) {
  out->insert(out->end(), uuid.begin(), uuid.end());
}

void PutString(std::vector<std::uint8_t>* out, const std::string& value) {
  PutU16(out, static_cast<std::uint16_t>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
}

std::vector<std::uint8_t> AuthPayload(
    const std::array<std::uint8_t, 16>& connection_uuid,
    const std::string& evidence) {
  std::vector<std::uint8_t> out;
  PutUuid(&out, connection_uuid);
  out.push_back(1);  // credential evidence present
  out.push_back(0);  // credential not pre-marked invalid
  out.push_back(0);  // mfa not required
  out.push_back(0);  // mfa absent
  PutString(&out, "local_password");
  PutString(&out, "alice");
  PutString(&out, "default");
  PutString(&out, "en");
  PutString(&out, evidence);
  PutString(&out, "tls-channel-binding-conformance");
  return out;
}

scratchbird::server::HostedEngineState EngineState() {
  scratchbird::server::HostedDatabaseSnapshot database;
  database.state = scratchbird::server::HostedDatabaseState::kOpen;
  database.database_path = "default";
  database.database_uuid = "018f0000-0000-7000-8000-000000000066";
  database.database_open = true;
  database.write_admission_fenced = false;
  database.config_policy_security_lifecycle_present = true;
  database.config_policy_security_lifecycle_json =
      "{\"config_policy_security_lifecycle\":{\"present\":true}}";
  scratchbird::server::HostedEngineState state;
  state.engine_context_active = true;
  state.databases.push_back(std::move(database));
  return state;
}

scratchbird::server::sbps::Frame AuthFrame(const std::string& evidence) {
  scratchbird::server::sbps::Frame frame;
  frame.header.message_type =
      static_cast<std::uint16_t>(scratchbird::server::sbps::MessageType::kAuthHandoff);
  frame.header.connection_uuid = scratchbird::server::sbps::MakeUuidV7Bytes();
  frame.header.request_uuid = scratchbird::server::sbps::MakeUuidV7Bytes();
  frame.payload = AuthPayload(frame.header.connection_uuid, evidence);
  return frame;
}

bool HasDiagnostic(const scratchbird::server::SessionOperationResult& result,
                   const std::string& code,
                   const std::string& detail) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code != code) continue;
    if (detail.empty()) return true;
    for (const auto& field : diagnostic.fields) {
      if (field.key == "detail" && field.value == detail) return true;
    }
  }
  return false;
}

void RequireTlsDenied(const std::string& evidence_suffix,
                      const std::string& code,
                      const std::string& detail) {
  const auto evidence =
      "scheme=local_password_v1;principal=alice;"
      "verifier=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa;" +
      evidence_suffix;
  scratchbird::server::ServerSessionRegistry registry;
  auto result = scratchbird::server::HandleAuthHandoff(
      &registry, EngineState(), AuthFrame(evidence));
  Require(!result.accepted, "TLS-denied auth handoff was accepted");
  Require(registry.channel_state == scratchbird::server::ServerChannelState::kFailed,
          "TLS-denied auth handoff did not fail the channel");
  Require(HasDiagnostic(result, code, detail),
          "TLS denial diagnostic missing: " + code + " detail=" + detail);
}

void HandoffPolicyProof() {
  scratchbird::listener::ListenerConfig clear;
  scratchbird::listener::HandoffSocketPayload clear_payload;
  scratchbird::listener::ApplyListenerTlsHandoffPolicy(clear, &clear_payload);
  auto clear_policy =
      scratchbird::listener::DecodeListenerTlsHandoffPolicy(clear_payload);
  Require(!clear_payload.tls_active, "cleartext profile incorrectly marked TLS active");
  Require(!clear_policy.tls_required, "cleartext profile required TLS");
  Require(clear_policy.termination_owner ==
              scratchbird::listener::ListenerTlsTerminationOwner::kNone,
          "cleartext profile had a TLS termination owner");

  scratchbird::listener::ListenerConfig tls;
  tls.tls_required = true;
  tls.tls_cert_file = "/redacted/server.crt";
  tls.tls_key_file = "/redacted/server.key";
  tls.tls_ca_file = "/redacted/ca.crt";
  scratchbird::listener::HandoffSocketPayload payload;
  scratchbird::listener::ApplyListenerTlsHandoffPolicy(tls, &payload);
  auto policy = scratchbird::listener::DecodeListenerTlsHandoffPolicy(payload);
  Require(payload.tls_active, "TLS-required profile did not mark handoff TLS active");
  Require(policy.tls_required, "TLS handoff policy did not require TLS");
  Require(policy.ca_configured && policy.cert_configured && policy.key_configured,
          "TLS handoff policy lost certificate/key/CA evidence");
  Require(policy.termination_owner ==
              scratchbird::listener::ListenerTlsTerminationOwner::kParserWorker,
          "TLS termination owner must be parser_worker");
  Require(policy.channel_binding_required,
          "TLS channel binding evidence must be required");
  Require(policy.downgrade_refusal_enforced,
          "TLS downgrade refusal must be enforced before handoff");
  Require(policy.certificate_evidence_required,
          "TLS certificate evidence must be required");
  Require(!policy.listener_decrypts_client_stream,
          "listener must not claim to decrypt parser-terminated TLS");

  const auto encoded = scratchbird::listener::EncodeHandoffSocketPayload(payload);
  scratchbird::listener::proto::MessageVectorSet messages;
  auto decoded = scratchbird::listener::DecodeHandoffSocketPayload(encoded, &messages);
  Require(decoded.has_value(), "encoded TLS handoff payload did not decode");
  auto decoded_policy =
      scratchbird::listener::DecodeListenerTlsHandoffPolicy(*decoded);
  Require(decoded_policy.termination_owner ==
              scratchbird::listener::ListenerTlsTerminationOwner::kParserWorker,
          "TLS termination owner did not survive handoff encode/decode");
  Require(decoded_policy.channel_binding_required &&
              decoded_policy.downgrade_refusal_enforced,
          "TLS channel binding or downgrade evidence did not survive encode/decode");
}

}  // namespace

int main() {
  HandoffPolicyProof();
  RequireTlsDenied("tls_required=true;tls_negotiated=cleartext",
                   "SECURITY.AUTHENTICATION.TLS_DOWNGRADE_REFUSED",
                   "tls_downgrade_refused");
  RequireTlsDenied("tls_client_cert_status=wrong_ca",
                   "SECURITY.AUTHENTICATION.TLS_CLIENT_CA_INVALID",
                   "tls_client_ca_invalid");
  RequireTlsDenied("tls_client_cert_status=expired",
                   "SECURITY.AUTHENTICATION.TLS_CLIENT_CERT_EXPIRED",
                   "tls_client_cert_expired");
  RequireTlsDenied("tls_channel_binding_status=mismatch",
                   "SECURITY.AUTHENTICATION.TLS_CHANNEL_BINDING_MISMATCH",
                   "tls_channel_binding_mismatch");
  RequireTlsDenied("tls_required=true;tls_negotiated=tls1.3",
                   "SECURITY.AUTHENTICATION.TLS_CHANNEL_BINDING_MISSING",
                   "tls_channel_binding_missing");
  std::cout << "engine_listener_tls_channel_binding_conformance=passed\n";
  return EXIT_SUCCESS;
}
