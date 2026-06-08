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
constexpr std::uint16_t kLongStringSentinel = 0xffff;
constexpr std::uint64_t kChunkLimit = 64 * 1024;

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

std::uint16_t GetU16(const std::vector<std::uint8_t>& data, std::size_t offset) {
  return static_cast<std::uint16_t>(data[offset]) |
         static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[offset + 1]) << 8u);
}

std::uint64_t GetU64(const std::vector<std::uint8_t>& data, std::size_t offset) {
  std::uint64_t value = 0;
  for (int byte = 7; byte >= 0; --byte) {
    value <<= 8u;
    value |= data[offset + static_cast<std::size_t>(byte)];
  }
  return value;
}

bool ReadString(const std::vector<std::uint8_t>& data, std::size_t* offset, std::string* out) {
  if (*offset + 2 > data.size()) return false;
  auto length = static_cast<std::uint64_t>(GetU16(data, *offset));
  *offset += 2;
  if (length == kLongStringSentinel) {
    if (*offset + 8 > data.size()) return false;
    length = GetU64(data, *offset);
    *offset += 8;
  }
  if (*offset + length > data.size()) return false;
  out->assign(reinterpret_cast<const char*>(data.data() + *offset),
              static_cast<std::size_t>(length));
  *offset += static_cast<std::size_t>(length);
  return true;
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

std::string LargeParserJsonEnvelope(std::size_t parameter_bytes, std::uint64_t result_rows) {
  std::string out = "{\"envelope\":\"SBLRExecutionEnvelope.v3\",";
  out += "\"operation_family\":\"sblr.query.relational.v3\",";
  out += "\"surface_key\":\"fspe010b4.chunked_payload\",";
  out += "\"sblr_operation_key\":\"op.fspe010b4.chunked_payload\",";
  out += "\"result_shape\":\"rs.fspe010b4.large_result.v1\",";
  out += "\"diagnostic_shape\":\"diag.fspe010b4.v1\",";
  out += "\"resource_contract\":\"resource.fspe010b4.v1\",";
  out += "\"trace_key\":\"FSPE-010B4\",";
  out += "\"stream_row_count\":";
  out += std::to_string(result_rows);
  out += ",\"source_payload_embedded\":false,";
  out += "\"resolved_object_uuids\":[],\"descriptor_refs\":[],\"policy_refs\":[],";
  out += "\"parameter_packet\":\"";
  out.append(parameter_bytes, 'x');
  out += "\"}";
  return out;
}

scratchbird::server::HostedEngineState MakeEngineState() {
  scratchbird::server::HostedEngineState state;
  state.engine_context_active = true;
  scratchbird::server::HostedDatabaseSnapshot database;
  database.state = scratchbird::server::HostedDatabaseState::kOpen;
  database.database_open = true;
  database.database_path = "/tmp/sb_sbps_chunked_payload_conformance.sbdb";
  database.database_uuid = "019e05df-f010-7000-8000-000000000014";
  state.databases.push_back(database);
  return state;
}

scratchbird::server::ServerSessionRegistry MakeRegistry(
    std::array<std::uint8_t, 16>* session_uuid) {
  scratchbird::server::ServerSessionRecord session;
  session.session_uuid = sbps::MakeUuidV7Bytes();
  session.auth_context_uuid = sbps::MakeUuidV7Bytes();
  session.principal_uuid = sbps::MakeUuidV7Bytes();
  session.effective_user_uuid = session.principal_uuid;
  session.database_path = "/tmp/sb_sbps_chunked_payload_conformance.sbdb";
  session.database_uuid = "019e05df-f010-7000-8000-000000000014";
  *session_uuid = session.session_uuid;
  scratchbird::server::ServerSessionRegistry registry;
  registry.sessions_by_uuid[scratchbird::server::UuidBytesToText(session.session_uuid)] = session;
  return registry;
}

sbps::Frame AssembleFrames(const std::vector<std::vector<std::uint8_t>>& encoded_frames) {
  Require(!encoded_frames.empty(), "chunk sequence must not be empty");
  std::vector<std::uint8_t> assembled_payload;
  sbps::Frame first;
  sbps::Frame last;
  std::uint64_t expected_sequence = 1;
  for (const auto& encoded : encoded_frames) {
    const auto decoded = sbps::DecodeFrameBytes(encoded, static_cast<std::uint32_t>(kChunkLimit));
    Require(decoded.ok(), "physical chunk frame must decode");
    const auto& frame = *decoded.frame;
    Require(frame.header.payload_len <= kChunkLimit, "physical chunk exceeded frame limit");
    Require((frame.header.flags & sbps::kFlagPayloadChunk) != 0, "chunk flag missing");
    Require(frame.header.sequence_number == expected_sequence++, "chunk sequence not deterministic");
    assembled_payload.insert(assembled_payload.end(), frame.payload.begin(), frame.payload.end());
    if (expected_sequence == 2) first = frame;
    last = frame;
  }
  Require((last.header.flags & sbps::kFlagFinal) != 0, "final chunk flag missing");
  first.payload = std::move(assembled_payload);
  first.header.flags = (last.header.flags & ~sbps::kFlagPayloadChunk) | sbps::kFlagFinal;
  first.header.payload_len = static_cast<std::uint32_t>(first.payload.size());
  return first;
}

bool HasCode(const std::vector<std::string>& codes, std::string_view code) {
  for (const auto& item : codes) {
    if (item == code) return true;
  }
  return false;
}

void ValidateLargeSblrAndResultPayloads() {
  std::array<std::uint8_t, 16> session_uuid{};
  auto registry = MakeRegistry(&session_uuid);
  const auto engine_state = MakeEngineState();
  const auto envelope = LargeParserJsonEnvelope(1100 * 1024, 24000);

  sbps::FrameHeader request_header;
  request_header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kExecuteSblr);
  request_header.payload_schema_id = kSchemaExecuteSblrTestV1;
  request_header.request_uuid = sbps::MakeUuidV7Bytes();
  request_header.session_uuid = session_uuid;
  const auto request_payload =
      scratchbird::server::EncodeExecuteSblrPayloadForTest(session_uuid, {}, envelope, false);
  Require(request_payload.size() > 1024 * 1024, "large SBLR parameter payload did not exceed one frame");

  const auto request_frames =
      sbps::EncodeFrameSequence(request_header, request_payload, kChunkLimit);
  Require(request_frames.size() > 1, "large SBLR parameter payload was not chunked");
  const auto assembled_request = AssembleFrames(request_frames);

  const auto execute =
      scratchbird::server::HandleExecuteSblr(&registry, engine_state, assembled_request);
  Require(execute.accepted, "chunked execute payload was rejected");
  Require(execute.payload.size() > 1024 * 1024, "large result payload did not exceed one frame");

  sbps::FrameHeader response_header;
  response_header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kExecuteResult);
  response_header.flags = sbps::kFlagResponse | sbps::kFlagFinal;
  response_header.request_uuid = request_header.request_uuid;
  response_header.session_uuid = session_uuid;
  const auto response_frames =
      sbps::EncodeFrameSequence(response_header, execute.payload, kChunkLimit);
  Require(response_frames.size() > 1, "large result payload was not chunked");
  const auto assembled_response = AssembleFrames(response_frames);

  std::size_t offset = 0;
  std::string outcome;
  Require(ReadString(assembled_response.payload, &offset, &outcome) && outcome == "accepted",
          "execute result outcome was malformed");
  Require(offset + 16 + 16 + 8 <= assembled_response.payload.size(),
          "execute result fixed fields were malformed");
  offset += 32;
  const auto row_count = GetU64(assembled_response.payload, offset);
  offset += 8;
  std::string operation_id;
  std::string row_packet;
  Require(ReadString(assembled_response.payload, &offset, &operation_id),
          "execute operation id was malformed");
  Require(ReadString(assembled_response.payload, &offset, &row_packet),
          "large execute row packet was malformed");
  Require(row_count == 24000, "large execute row count was not preserved");
  Require(Contains(row_packet, "\"row_index\":23999"), "large execute result lost final row");
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
  ValidateLargeSblrAndResultPayloads();
  ValidateLargeMessageVectorPayload();
  std::cout << "sbps_chunked_payload_conformance=passed\n";
  return EXIT_SUCCESS;
}
