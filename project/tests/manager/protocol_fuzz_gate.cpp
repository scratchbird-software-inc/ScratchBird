// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SBMI_MANAGER_PROTOCOL_FUZZ_GATE

#include "manager_protocol.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace proto = scratchbird::manager::protocol;

namespace {

proto::Bytes Pattern(std::uint32_t seed, std::size_t size) {
  proto::Bytes out(size);
  std::uint32_t state = seed;
  for (auto& byte : out) {
    state = state * 1664525u + 1013904223u;
    byte = static_cast<std::uint8_t>(state >> 24u);
  }
  return out;
}

bool Expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    return false;
  }
  return true;
}

bool HasDiagnostic(const std::vector<proto::Diagnostic>& diagnostics,
                   const std::string& code) {
  for (const auto& diagnostic : diagnostics) {
    if (diagnostic.code == code) {
      return true;
    }
  }
  return false;
}

void ExerciseDecoders(const proto::Bytes& data) {
  std::vector<proto::Diagnostic> diagnostics;
  (void)proto::DecodeSbdbFrame(data, &diagnostics);
  diagnostics.clear();
  (void)proto::DecodeControlPlaneMessage(data, &diagnostics);
  diagnostics.clear();
  (void)proto::DecodeMessageVectorSetV1(data, &diagnostics);
  diagnostics.clear();
  (void)proto::DecodeDbbt(data, &diagnostics);
  diagnostics.clear();
  (void)proto::DecodeLpreface(data, &diagnostics);
  diagnostics.clear();
  (void)proto::DecodeLprefaceAck(data, &diagnostics);
}

bool ProveValidRoundTrips() {
  bool ok = true;
  std::vector<proto::Diagnostic> diagnostics;

  proto::SbdbFrame frame;
  frame.type = 0x65;
  frame.flags = 0x01;
  frame.payload = {0x10, 0x20, 0x30};
  const auto encoded_frame = proto::EncodeSbdbFrame(frame);
  const auto decoded_frame = proto::DecodeSbdbFrame(encoded_frame, &diagnostics);
  ok = Expect(decoded_frame.has_value(),
              "valid SBDB frame should decode during fuzz proof") && ok;
  ok = Expect(diagnostics.empty(),
              "valid SBDB frame should not produce diagnostics") && ok;
  if (decoded_frame) {
    ok = Expect(decoded_frame->payload == frame.payload,
                "valid SBDB frame payload should round-trip") && ok;
  }

  diagnostics.clear();
  proto::ControlPlaneMessage control;
  control.message_type = 0x0060;
  control.request_id = 77;
  const std::string command = "LPREFACE_VALIDATE fuzz-proof";
  control.payload.assign(command.begin(), command.end());
  const auto encoded_control = proto::EncodeControlPlaneMessage(control);
  const auto decoded_control =
      proto::DecodeControlPlaneMessage(encoded_control, &diagnostics);
  ok = Expect(decoded_control.has_value(),
              "valid control-plane frame should decode during fuzz proof") && ok;
  ok = Expect(diagnostics.empty(),
              "valid control-plane frame should not produce diagnostics") && ok;
  if (decoded_control) {
    ok = Expect(decoded_control->payload == control.payload,
                "valid control-plane payload should round-trip") && ok;
  }

  diagnostics.clear();
  proto::LprefaceAck ack;
  ack.accepted = false;
  ack.nack_code = 44;
  ack.message = "fuzz-proof-nack";
  proto::Bytes encoded_ack;
  const auto encoded_ack_result = proto::EncodeLprefaceAck(ack, &encoded_ack);
  ok = Expect(encoded_ack_result.ok,
              "valid LPREFACE_ACK should encode during fuzz proof") && ok;
  const auto decoded_ack = proto::DecodeLprefaceAck(encoded_ack, &diagnostics);
  ok = Expect(decoded_ack.has_value(),
              "valid LPREFACE_ACK should decode during fuzz proof") && ok;
  ok = Expect(diagnostics.empty(),
              "valid LPREFACE_ACK should not produce diagnostics") && ok;
  if (decoded_ack) {
    ok = Expect(decoded_ack->nack_code == ack.nack_code,
                "valid LPREFACE_ACK nack code should round-trip") && ok;
  }
  return ok;
}

bool ProveMalformedInputsFailClosed() {
  bool ok = true;
  std::vector<proto::Diagnostic> diagnostics;

  proto::Bytes oversized_sbdb = {'S', 'B', 'D', 'B', 0x01, 0x01, 0x65, 0x00,
                                 0xff, 0xff, 0xff, 0x7f};
  const auto decoded_sbdb = proto::DecodeSbdbFrame(oversized_sbdb, &diagnostics);
  ok = Expect(!decoded_sbdb.has_value(),
              "oversized SBDB frame should fail closed") && ok;
  ok = Expect(HasDiagnostic(diagnostics, "SBDB.PAYLOAD_TOO_LARGE"),
              "oversized SBDB frame should report stable diagnostic") && ok;
  ExerciseDecoders(oversized_sbdb);

  diagnostics.clear();
  proto::Bytes oversized_control = {
      'S', 'B', 'C', 'T', 0x01, 0x00, 0x60, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0x7f,
      0x00, 0x00, 0x00, 0x00};
  const auto decoded_control =
      proto::DecodeControlPlaneMessage(oversized_control, &diagnostics);
  ok = Expect(!decoded_control.has_value(),
              "oversized control-plane frame should fail closed") && ok;
  ok = Expect(HasDiagnostic(diagnostics, "CONTROL.PAYLOAD_TOO_LARGE"),
              "oversized control-plane frame should report stable diagnostic") && ok;
  ExerciseDecoders(oversized_control);

  diagnostics.clear();
  const auto malformed_claim = proto::DecodeLprefaceHandoffClaim(
      "SB-LPREFACE-CLAIM/1 client=too-short server=also-short",
      &diagnostics);
  ok = Expect(!malformed_claim.has_value(),
              "malformed LPREFACE handoff claim should fail closed") && ok;
  ok = Expect(!diagnostics.empty(),
              "malformed LPREFACE handoff claim should report diagnostics") && ok;
  return ok;
}

}  // namespace

int main() {
  bool ok = true;
  for (std::size_t size = 0; size < 4096; ++size) {
    ExerciseDecoders(Pattern(static_cast<std::uint32_t>(0x5bd1e995u + size), size));
  }

  ok = ProveValidRoundTrips() && ok;
  ok = ProveMalformedInputsFailClosed() && ok;

  if (ok) {
    std::cout << "manager protocol deterministic fuzz gate passed\n";
    return EXIT_SUCCESS;
  }
  return EXIT_FAILURE;
}
