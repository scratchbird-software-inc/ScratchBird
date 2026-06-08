// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "parser_ipc_common.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace scratchbird::parser::sbsql {

inline constexpr std::uint32_t kParserServerIpcProtocolCurrent = 1;
inline constexpr std::uint32_t kParserServerIpcProtocolMinSupported = 1;
inline constexpr std::uint32_t kParserServerIpcProtocolMaxSupported = 1;
inline constexpr std::uint32_t kParserServerIpcMetricsSchemaCurrent = 1;
inline constexpr std::uint32_t kParserServerIpcMetricsSchemaMinSupported = 1;
inline constexpr std::uint32_t kParserServerIpcMetricsSchemaMaxSupported = 1;

enum class ParserServerOpcode : std::uint16_t {
  kParserHello = 1,
  kParserHelloResult = 2,
  kAuthRelay = 3,
  kAuthRelayResult = 4,
  kSessionContext = 5,
  kNameResolutionRequest = 6,
  kNameResolutionResult = 7,
  kSblrSubmitRequest = 8,
  kSblrSubmitResult = 9,
  kParserControlCommand = 10,
  kParserControlAck = 11,
  kHeartbeatRequest = 12,
  kHeartbeatResponse = 13,
  kMetricsSnapshotRequest = 14,
  kMetricsSnapshot = 15,
  kParserExitReport = 16,
};

struct ParserServerPacket {
  ParserServerOpcode opcode{ParserServerOpcode::kParserHello};
  std::uint32_t protocol_version{kParserServerIpcProtocolCurrent};
  std::uint64_t request_id{0};
  std::vector<std::uint8_t> payload;
};

struct ParserHello {
  std::string parser_uuid;
  std::string dialect;
  std::string build_id;
  std::string registry_versions;
  std::uint32_t protocol_version{kParserServerIpcProtocolCurrent};
  std::string auth_relay_modes;
  std::uint32_t metrics_schema_version{kParserServerIpcMetricsSchemaCurrent};
};

struct ParserHelloResult {
  bool accepted{false};
  std::uint32_t server_protocol_version{kParserServerIpcProtocolCurrent};
  std::string server_capabilities;
  MessageVectorSet messages;
};

struct NameResolutionRequest {
  std::string session_uuid;
  std::string language;
  std::vector<std::string> search_path;
  std::string presented_name;
  bool quoted{false};
};

struct NameResolutionResult {
  bool resolved{false};
  std::string object_uuid;
  MessageVectorSet messages;
};

std::vector<std::uint8_t> EncodePacket(const ParserServerPacket& packet);
std::optional<ParserServerPacket> DecodePacket(const std::vector<std::uint8_t>& bytes, MessageVectorSet* messages);
std::vector<std::uint8_t> EncodeParserHello(const ParserHello& hello);
ParserHelloResult RefusedHelloResult(std::string code, std::string reason);
std::string OpcodeName(ParserServerOpcode opcode);

} // namespace scratchbird::parser::sbsql
