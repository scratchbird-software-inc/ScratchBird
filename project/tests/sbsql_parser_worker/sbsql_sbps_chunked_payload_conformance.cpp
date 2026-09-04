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
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace sbps = scratchbird::server::sbps;

constexpr std::uint32_t kSchemaExecuteSblrTestV1 = 4003;
constexpr std::uint64_t kChunkLimit = 64 * 1024;

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

sbps::Frame AssembleFrames(const std::vector<std::vector<std::uint8_t>>& encoded_frames) {
  Require(!encoded_frames.empty(), "chunk sequence must not be empty");
  std::vector<sbps::Frame> chunks;
  chunks.reserve(encoded_frames.size());
  std::uint64_t expected_sequence = 1;
  for (const auto& encoded : encoded_frames) {
    const auto decoded = sbps::DecodeFrameBytes(encoded, static_cast<std::uint32_t>(kChunkLimit));
    Require(decoded.ok(), "physical chunk frame must decode");
    const auto& frame = *decoded.frame;
    Require(frame.header.payload_len <= kChunkLimit, "physical chunk exceeded frame limit");
    Require((frame.header.flags & sbps::kFlagPayloadChunk) != 0, "chunk flag missing");
    Require(frame.header.sequence_number == expected_sequence++, "chunk sequence not deterministic");
    chunks.push_back(frame);
  }
  Require((chunks.back().header.flags & sbps::kFlagFinal) != 0,
          "final chunk flag missing");
  const auto assembled = sbps::AssembleDecodedChunkSequence(chunks, 4 * 1024 * 1024);
  Require(assembled.ok(), "production chunk assembler rejected a valid sequence");
  return *assembled.frame;
}

bool HasCode(const std::vector<std::string>& codes, std::string_view code) {
  for (const auto& item : codes) {
    if (item == code) return true;
  }
  return false;
}

void ValidateLargeRequestAndResultPayloads() {
  const auto session_uuid = sbps::MakeUuidV7Bytes();
  std::vector<std::uint8_t> request_payload(1100 * 1024);
  for (std::size_t index = 0; index < request_payload.size(); ++index) {
    request_payload[index] = static_cast<std::uint8_t>(index % 251);
  }

  sbps::FrameHeader request_header;
  request_header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kExecuteSblr);
  request_header.payload_schema_id = kSchemaExecuteSblrTestV1;
  request_header.request_uuid = sbps::MakeUuidV7Bytes();
  request_header.session_uuid = session_uuid;
  Require(request_payload.size() > 1024 * 1024,
          "large request payload did not exceed one frame");

  const auto request_frames =
      sbps::EncodeFrameSequence(request_header, request_payload, kChunkLimit);
  Require(request_frames.size() > 1, "large SBLR parameter payload was not chunked");
  const auto assembled_request = AssembleFrames(request_frames);
  Require(assembled_request.payload == request_payload,
          "chunked request payload did not reassemble byte-exactly");

  std::vector<std::uint8_t> result_payload(1200 * 1024);
  for (std::size_t index = 0; index < result_payload.size(); ++index) {
    result_payload[index] = static_cast<std::uint8_t>((index * 17) % 253);
  }
  Require(result_payload.size() > 1024 * 1024,
          "large result payload did not exceed one frame");

  sbps::FrameHeader response_header;
  response_header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kExecuteResult);
  response_header.flags = sbps::kFlagResponse | sbps::kFlagFinal;
  response_header.request_uuid = request_header.request_uuid;
  response_header.session_uuid = session_uuid;
  const auto response_frames =
      sbps::EncodeFrameSequence(response_header, result_payload, kChunkLimit);
  Require(response_frames.size() > 1, "large result payload was not chunked");
  const auto assembled_response = AssembleFrames(response_frames);
  Require(assembled_response.payload == result_payload,
          "chunked result payload did not reassemble byte-exactly");
}

void ValidateLargeMessageVectorPayload() {
  std::vector<scratchbird::server::ServerDiagnostic> diagnostics;
  for (int i = 0; i < 32; ++i) {
    scratchbird::server::ServerDiagnostic diagnostic;
    diagnostic.code = "PARSER_SERVER_IPC.CHUNK_TEST_" + std::to_string(i);
    diagnostic.message_key = diagnostic.code;
    diagnostic.severity = scratchbird::server::ServerDiagnosticSeverity::kError;
    diagnostic.safe_message = "chunked message-vector diagnostic";
    diagnostic.fields.push_back({"detail", std::string(40 * 1024, 'd')});
    diagnostics.push_back(std::move(diagnostic));
  }
  const auto message_vector = sbps::EncodeMessageVectorSet(diagnostics, sbps::MakeUuidV7Bytes());
  Require(message_vector.size() > 1024 * 1024, "large message vector did not exceed one frame");

  sbps::FrameHeader header;
  header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kDiagnostic);
  header.flags = sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
  header.payload_schema_id = sbps::kSchemaMessageVectorSetV1;
  header.request_uuid = sbps::MakeUuidV7Bytes();
  const auto frames = sbps::EncodeFrameSequence(header, message_vector, kChunkLimit);
  Require(frames.size() > 1, "large message vector was not chunked");
  const auto assembled = AssembleFrames(frames);
  const auto codes = sbps::DecodeMessageVectorDiagnosticCodes(assembled.payload);
  Require(HasCode(codes, "PARSER_SERVER_IPC.CHUNK_TEST_0"),
          "chunked message vector lost first diagnostic");
  Require(HasCode(codes, "PARSER_SERVER_IPC.CHUNK_TEST_31"),
          "chunked message vector lost final diagnostic");
}

}  // namespace

int main() {
  ValidateLargeRequestAndResultPayloads();
  ValidateLargeMessageVectorPayload();
  std::cout << "sbps_chunked_payload_conformance=passed\n";
  return EXIT_SUCCESS;
}
