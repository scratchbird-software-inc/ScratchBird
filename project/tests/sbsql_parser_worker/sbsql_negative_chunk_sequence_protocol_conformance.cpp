// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sbps.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace sbps = scratchbird::server::sbps;

namespace {

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

bool HasCode(const std::vector<scratchbird::server::ServerDiagnostic>& diagnostics,
             std::string_view code) {
  return std::any_of(diagnostics.begin(), diagnostics.end(),
                     [code](const scratchbird::server::ServerDiagnostic& diagnostic) {
                       return diagnostic.code == code;
                     });
}

std::vector<std::uint8_t> Bytes(std::string_view text) {
  return std::vector<std::uint8_t>(text.begin(), text.end());
}

sbps::Frame DecodeEncodedChunk(const sbps::FrameHeader& header,
                               const std::vector<std::uint8_t>& payload) {
  const auto encoded = sbps::EncodeFrame(header, payload);
  const auto decoded = sbps::DecodeFrameBytes(encoded, 4096);
  Require(decoded.ok(), "encoded physical chunk frame did not decode");
  return *decoded.frame;
}

sbps::FrameHeader BaseHeader(std::uint64_t sequence, bool final) {
  sbps::FrameHeader header;
  header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kPing);
  header.flags = sbps::kFlagPayloadChunk | (final ? sbps::kFlagFinal : 0u);
  header.stream_id = 17;
  header.sequence_number = sequence;
  header.request_uuid = sbps::MakeUuidV7Bytes();
  header.connection_uuid = sbps::MakeUuidV7Bytes();
  header.session_uuid = {};
  return header;
}

std::vector<sbps::Frame> ValidTwoChunkSequence() {
  auto first = BaseHeader(1, false);
  auto second = first;
  second.sequence_number = 2;
  second.flags |= sbps::kFlagFinal;
  return {DecodeEncodedChunk(first, Bytes("abc")),
          DecodeEncodedChunk(second, Bytes("def"))};
}

void ValidateAcceptedSequence() {
  const auto assembled = sbps::AssembleDecodedChunkSequence(ValidTwoChunkSequence(), 1024);
  Require(assembled.ok(), "valid chunk sequence was refused");
  Require(assembled.frame->payload == Bytes("abcdef"), "assembled payload was not preserved");
  Require((assembled.frame->header.flags & sbps::kFlagPayloadChunk) == 0,
          "assembled frame retained physical chunk flag");
  Require((assembled.frame->header.flags & sbps::kFlagFinal) != 0,
          "assembled frame did not retain final flag");
}

void ValidateInvalidSequenceHeader() {
  auto bad = BaseHeader(0, true);
  const auto assembled =
      sbps::AssembleDecodedChunkSequence({DecodeEncodedChunk(bad, Bytes("x"))}, 1024);
  Require(!assembled.ok() &&
              HasCode(assembled.diagnostics, "PARSER_SERVER_IPC.CHUNK_SEQUENCE_INVALID"),
          "zero sequence chunk did not return exact invalid-sequence diagnostic");
}

void ValidateSequenceGap() {
  auto frames = ValidTwoChunkSequence();
  frames[1].header.sequence_number = 3;
  const auto assembled = sbps::AssembleDecodedChunkSequence(frames, 1024);
  Require(!assembled.ok() &&
              HasCode(assembled.diagnostics, "PARSER_SERVER_IPC.CHUNK_SEQUENCE_INVALID"),
          "non-contiguous chunk sequence did not return exact invalid-sequence diagnostic");
}

void ValidateMismatchedAuthority() {
  auto frames = ValidTwoChunkSequence();
  frames[1].header.request_uuid = sbps::MakeUuidV7Bytes();
  const auto assembled = sbps::AssembleDecodedChunkSequence(frames, 1024);
  Require(!assembled.ok() &&
              HasCode(assembled.diagnostics, "PARSER_SERVER_IPC.CHUNK_SEQUENCE_INVALID"),
          "mismatched chunk request authority did not return exact invalid-sequence diagnostic");
}

void ValidateMissingFinalChunk() {
  auto frames = ValidTwoChunkSequence();
  frames[1].header.flags &= ~sbps::kFlagFinal;
  const auto assembled = sbps::AssembleDecodedChunkSequence(frames, 1024);
  Require(!assembled.ok() &&
              HasCode(assembled.diagnostics, "PARSER_SERVER_IPC.CHUNK_SEQUENCE_INVALID"),
          "unterminated chunk sequence did not return exact invalid-sequence diagnostic");
}

void ValidateFinalBeforeTail() {
  auto frames = ValidTwoChunkSequence();
  auto tail = frames.back();
  tail.header.sequence_number = 3;
  tail.payload = Bytes("tail");
  frames.push_back(tail);
  const auto assembled = sbps::AssembleDecodedChunkSequence(frames, 1024);
  Require(!assembled.ok() &&
              HasCode(assembled.diagnostics, "PARSER_SERVER_IPC.CHUNK_SEQUENCE_INVALID"),
          "chunk after final chunk did not return exact invalid-sequence diagnostic");
}

void ValidateAssembledPayloadLimit() {
  const auto assembled = sbps::AssembleDecodedChunkSequence(ValidTwoChunkSequence(), 4);
  Require(!assembled.ok() &&
              HasCode(assembled.diagnostics, "PARSER_SERVER_IPC.PAYLOAD_TOO_LARGE"),
          "assembled chunk payload overflow did not return exact payload-too-large diagnostic");
}

}  // namespace

int main() {
  ValidateAcceptedSequence();
  ValidateInvalidSequenceHeader();
  ValidateSequenceGap();
  ValidateMismatchedAuthority();
  ValidateMissingFinalChunk();
  ValidateFinalBeforeTail();
  ValidateAssembledPayloadLimit();
  std::cout << "sbsql_negative_chunk_sequence_protocol_conformance=passed\n";
  return EXIT_SUCCESS;
}
