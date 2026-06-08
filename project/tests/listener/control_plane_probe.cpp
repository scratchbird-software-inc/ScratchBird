// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "control_plane.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>

int main() {
  auto check = [](bool condition, const char* message) {
    if (!condition) {
      std::cerr << message << '\n';
      std::exit(EXIT_FAILURE);
    }
  };

  check(static_cast<std::uint16_t>(scratchbird::listener::ListenerControlOpcode::kSessionBindingReport) == 0x0032,
        "SESSION_BINDING_REPORT opcode must match P1 listener control-plane spec");
  check(static_cast<std::uint16_t>(scratchbird::listener::ListenerControlOpcode::kSessionBindingClear) == 0x0033,
        "SESSION_BINDING_CLEAR opcode must match P1 listener control-plane spec");
  check(static_cast<std::uint16_t>(scratchbird::listener::ListenerControlOpcode::kErrorMessage) == 0x00FF,
        "ERROR_MESSAGE opcode must match P1 listener control-plane spec");

  scratchbird::listener::ListenerControlFrame frame;
  frame.opcode = scratchbird::listener::ListenerControlOpcode::kManagementCommand;
  frame.sequence = 42;
  frame.payload_json = "STATUS";
  auto encoded = scratchbird::listener::EncodeControlFrame(frame);
  check(encoded.size() == 28 + frame.payload_json.size(), "SBCT frame size mismatch");
  check(encoded[0] == 'S' && encoded[1] == 'B' && encoded[2] == 'C' && encoded[3] == 'T',
        "SBCT magic mismatch");
  auto decoded = scratchbird::listener::DecodeControlFrame(encoded);
  if (!decoded.ok) {
    std::cerr << scratchbird::listener::MessageVectorSetJson(decoded.messages) << '\n';
    return EXIT_FAILURE;
  }
  check(decoded.frame.sequence == 42 && decoded.frame.payload_json == frame.payload_json &&
            decoded.frame.opcode == scratchbird::listener::ListenerControlOpcode::kManagementCommand,
        "decoded listener control frame mismatch");
  scratchbird::listener::ParserHelloPayload hello;
  hello.protocol = "sbsql";
  hello.pid = 123;
  hello.worker_id = 7;
  hello.parser_api_major = 1;
  hello.profile_id = "default";
  hello.bundle_contract_id = "bundle.default@1";
  auto hello_payload = scratchbird::listener::EncodeHelloPayload(hello);
  auto messages = scratchbird::listener::MakeMessageVectorSet({});
  auto decoded_hello = scratchbird::listener::DecodeHelloPayload(hello_payload, &messages);
  check(decoded_hello && decoded_hello->protocol == hello.protocol &&
            decoded_hello->worker_id == hello.worker_id &&
            decoded_hello->bundle_contract_id == hello.bundle_contract_id,
        "HELLO payload round trip mismatch");

  scratchbird::listener::HealthReportPayload health{77, 1, 4201};
  auto health_payload = scratchbird::listener::EncodeHealthReportPayload(health);
  check(health_payload.size() == 15, "HEALTH_REPORT payload size mismatch");
  auto decoded_health = scratchbird::listener::DecodeHealthReportPayload(health_payload, &messages);
  check(decoded_health && decoded_health->request_id_echo == 77 &&
            decoded_health->state == 1 && decoded_health->last_error == 4201,
        "HEALTH_REPORT payload round trip mismatch");

  scratchbird::listener::SessionBindingReportPayload binding;
  for (std::size_t i = 0; i < binding.attachment_id.size(); ++i) {
    binding.attachment_id[i] = static_cast<std::uint8_t>(i);
    binding.catalog_session_id[i] = static_cast<std::uint8_t>(0x10 + i);
    binding.transaction_uuid[i] = static_cast<std::uint8_t>(0x20 + i);
    binding.protocol_session_id[i] = static_cast<std::uint8_t>(0x30 + i);
    binding.authenticated_principal_id[i] = static_cast<std::uint8_t>(0x40 + i);
    binding.session_user_id[i] = static_cast<std::uint8_t>(0x50 + i);
    binding.active_role_id[i] = static_cast<std::uint8_t>(0x60 + i);
    binding.authkey_id[i] = static_cast<std::uint8_t>(0x70 + i);
  }
  binding.current_txn_id = 0xDEADBEEFCAFEBABEULL;
  auto binding_payload = scratchbird::listener::EncodeSessionBindingReportPayload(binding);
  check(binding_payload.size() == 138, "SESSION_BINDING_REPORT no-group size mismatch");
  auto decoded_binding = scratchbird::listener::DecodeSessionBindingReportPayload(binding_payload, &messages);
  check(decoded_binding && decoded_binding->current_txn_id == binding.current_txn_id &&
            decoded_binding->effective_group_ids.empty() &&
            decoded_binding->attachment_id == binding.attachment_id,
        "SESSION_BINDING_REPORT payload round trip mismatch");

  scratchbird::listener::TakeoverRequestPayload takeover;
  takeover.mask = scratchbird::listener::kTakeoverClaimAttachmentId |
                  scratchbird::listener::kTakeoverClaimCurrentTxnId;
  takeover.attachment_id = binding.attachment_id;
  takeover.current_txn_id = 0x42;
  auto takeover_payload = scratchbird::listener::EncodeTakeoverRequestPayload(takeover);
  check(takeover_payload.size() == 28 && takeover_payload[0] == 0x81 && takeover_payload[1] == 0x00,
        "TAKEOVER_REQUEST attachment+txn vector mismatch");
  auto decoded_takeover = scratchbird::listener::DecodeTakeoverRequestPayload(takeover_payload, &messages);
  check(decoded_takeover && decoded_takeover->mask == takeover.mask &&
            decoded_takeover->attachment_id == takeover.attachment_id &&
            decoded_takeover->current_txn_id == takeover.current_txn_id,
        "TAKEOVER_REQUEST payload round trip mismatch");

  scratchbird::listener::TakeoverDecisionPayload decision;
  decision.allowed = false;
  decision.reason = "Existing parser still owns a live client transport for the requested session";
  auto decision_payload = scratchbird::listener::EncodeTakeoverDecisionPayload(decision);
  auto decoded_decision = scratchbird::listener::DecodeTakeoverDecisionPayload(decision_payload, &messages);
  check(decoded_decision && !decoded_decision->allowed && decoded_decision->reason == decision.reason,
        "TAKEOVER_DECISION payload round trip mismatch");

  auto probe_payload = scratchbird::listener::EncodeTakeoverProbeResultPayload({0x0B});
  auto decoded_probe = scratchbird::listener::DecodeTakeoverProbeResultPayload(probe_payload, &messages);
  check(decoded_probe && decoded_probe->flags == 0x0B, "TAKEOVER_PROBE_RESULT payload round trip mismatch");

  auto recycle_reason = scratchbird::listener::DecodeRecyclePayload({0x01, 0x00}, &messages);
  check(recycle_reason && *recycle_reason == 1, "RECYCLE payload round trip mismatch");
  check(!scratchbird::listener::DecodeRecyclePayload({0x05, 0x00}, &messages),
        "RECYCLE reserved reason must be rejected");

  scratchbird::listener::ErrorMessagePayload error;
  error.reason = "dummy parser fatal";
  auto error_payload = scratchbird::listener::EncodeErrorMessagePayload(error);
  auto decoded_error = scratchbird::listener::DecodeErrorMessagePayload(error_payload, &messages);
  check(decoded_error && decoded_error->reason == error.reason, "ERROR_MESSAGE payload round trip mismatch");
  return EXIT_SUCCESS;
}
