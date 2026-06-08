// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "parser_server_ipc.hpp"

#include <cstring>
#include <sstream>

namespace scratchbird::parser::sbsql {
namespace {

constexpr std::uint32_t kMagic = 0x53504943u; // SPIC
constexpr std::size_t kHeaderSize = 24;
constexpr std::uint64_t kMaxPayload = 16u * 1024u * 1024u;

void PutU16(std::vector<std::uint8_t>* out, std::uint16_t value) {
  out->push_back(static_cast<std::uint8_t>(value & 0xffu));
  out->push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
}

void PutU32(std::vector<std::uint8_t>* out, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) out->push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xffu));
}

void PutU64(std::vector<std::uint8_t>* out, std::uint64_t value) {
  for (int i = 0; i < 8; ++i) out->push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xffu));
}

std::uint16_t ReadU16(const std::vector<std::uint8_t>& bytes, std::size_t off) {
  return static_cast<std::uint16_t>(bytes[off]) | static_cast<std::uint16_t>(bytes[off + 1]) << 8u;
}

std::uint32_t ReadU32(const std::vector<std::uint8_t>& bytes, std::size_t off) {
  std::uint32_t out = 0;
  for (int i = 0; i < 4; ++i) out |= static_cast<std::uint32_t>(bytes[off + i]) << (i * 8);
  return out;
}

std::uint64_t ReadU64(const std::vector<std::uint8_t>& bytes, std::size_t off) {
  std::uint64_t out = 0;
  for (int i = 0; i < 8; ++i) out |= static_cast<std::uint64_t>(bytes[off + i]) << (i * 8);
  return out;
}

void AddText(std::vector<std::uint8_t>* out, std::string_view value) {
  PutU16(out, static_cast<std::uint16_t>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
}

std::optional<std::string> ProtocolVersionRefusal(std::uint32_t version) {
  if (version < kParserServerIpcProtocolMinSupported) {
    return "PARSER_IPC.PROTOCOL.TOO_OLD";
  }
  if (version > kParserServerIpcProtocolMaxSupported) {
    return "PARSER_IPC.PROTOCOL.FUTURE_UNSUPPORTED";
  }
  return std::nullopt;
}

} // namespace

std::string OpcodeName(ParserServerOpcode opcode) {
  switch (opcode) {
    case ParserServerOpcode::kParserHello: return "ParserHello";
    case ParserServerOpcode::kParserHelloResult: return "ParserHelloResult";
    case ParserServerOpcode::kAuthRelay: return "AuthRelay";
    case ParserServerOpcode::kAuthRelayResult: return "AuthRelayResult";
    case ParserServerOpcode::kSessionContext: return "SessionContext";
    case ParserServerOpcode::kNameResolutionRequest: return "NameResolutionRequest";
    case ParserServerOpcode::kNameResolutionResult: return "NameResolutionResult";
    case ParserServerOpcode::kSblrSubmitRequest: return "SblrSubmitRequest";
    case ParserServerOpcode::kSblrSubmitResult: return "SblrSubmitResult";
    case ParserServerOpcode::kParserControlCommand: return "ParserControlCommand";
    case ParserServerOpcode::kParserControlAck: return "ParserControlAck";
    case ParserServerOpcode::kHeartbeatRequest: return "HeartbeatRequest";
    case ParserServerOpcode::kHeartbeatResponse: return "HeartbeatResponse";
    case ParserServerOpcode::kMetricsSnapshotRequest: return "MetricsSnapshotRequest";
    case ParserServerOpcode::kMetricsSnapshot: return "MetricsSnapshot";
    case ParserServerOpcode::kParserExitReport: return "ParserExitReport";
  }
  return "Unknown";
}

std::vector<std::uint8_t> EncodePacket(const ParserServerPacket& packet) {
  std::vector<std::uint8_t> out;
  out.reserve(kHeaderSize + packet.payload.size());
  PutU32(&out, kMagic);
  PutU16(&out, static_cast<std::uint16_t>(packet.opcode));
  PutU16(&out, 0);
  PutU32(&out, packet.protocol_version);
  PutU64(&out, packet.request_id);
  PutU32(&out, static_cast<std::uint32_t>(packet.payload.size()));
  out.insert(out.end(), packet.payload.begin(), packet.payload.end());
  return out;
}

std::optional<ParserServerPacket> DecodePacket(const std::vector<std::uint8_t>& bytes, MessageVectorSet* messages) {
  if (bytes.size() < kHeaderSize) {
    if (messages) messages->diagnostics.push_back(MakeDiagnostic("PARSER_IPC.FRAME.TRUNCATED", "ERROR", "parser IPC frame header is truncated", "sbp_sbsql.ipc"));
    return std::nullopt;
  }
  if (ReadU32(bytes, 0) != kMagic) {
    if (messages) messages->diagnostics.push_back(MakeDiagnostic("PARSER_IPC.FRAME.BAD_MAGIC", "ERROR", "parser IPC frame magic is invalid", "sbp_sbsql.ipc"));
    return std::nullopt;
  }
  const auto payload_len = ReadU32(bytes, 20);
  if (payload_len > kMaxPayload || bytes.size() != kHeaderSize + payload_len) {
    if (messages) messages->diagnostics.push_back(MakeDiagnostic("PARSER_IPC.FRAME.LENGTH_INVALID", "ERROR", "parser IPC frame length is invalid", "sbp_sbsql.ipc"));
    return std::nullopt;
  }
  ParserServerPacket packet;
  packet.opcode = static_cast<ParserServerOpcode>(ReadU16(bytes, 4));
  packet.protocol_version = ReadU32(bytes, 8);
  if (const auto refusal = ProtocolVersionRefusal(packet.protocol_version)) {
    if (messages) {
      std::ostringstream reason;
      reason << "parser IPC protocol version " << packet.protocol_version
             << " is outside supported range "
             << kParserServerIpcProtocolMinSupported << ".."
             << kParserServerIpcProtocolMaxSupported;
      messages->diagnostics.push_back(MakeDiagnostic(*refusal, "ERROR", reason.str(), "sbp_sbsql.ipc"));
    }
    return std::nullopt;
  }
  packet.request_id = ReadU64(bytes, 12);
  packet.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(kHeaderSize), bytes.end());
  return packet;
}

std::vector<std::uint8_t> EncodeParserHello(const ParserHello& hello) {
  std::vector<std::uint8_t> out;
  AddText(&out, hello.parser_uuid);
  AddText(&out, hello.dialect);
  AddText(&out, hello.build_id);
  AddText(&out, hello.registry_versions);
  PutU32(&out, hello.protocol_version);
  AddText(&out, hello.auth_relay_modes);
  PutU32(&out, hello.metrics_schema_version);
  return out;
}

ParserHelloResult RefusedHelloResult(std::string code, std::string reason) {
  ParserHelloResult result;
  result.accepted = false;
  result.server_protocol_version = kParserServerIpcProtocolCurrent;
  result.messages.diagnostics.push_back(MakeDiagnostic(std::move(code), "ERROR", std::move(reason), "sb_server.parser_ipc"));
  return result;
}

} // namespace scratchbird::parser::sbsql
