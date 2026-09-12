// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace scratchbird::parser::ipc {

inline constexpr std::uint16_t kPsDisconnectMessageTypeV1 = 74;
inline constexpr std::uint32_t kPsDisconnectSchemaIdV1 = 1074;
using PsDisconnectUuidV1 = std::array<std::uint8_t, 16>;

enum class PsDisconnectReasonV1 : std::uint8_t {
  normal = 1, client_closed = 2, drain = 3, server_shutdown = 4,
  fence = 5, protocol_error = 6, resource_error = 7,
};
enum class PsDisconnectInitiatorV1 : std::uint8_t {
  parser = 1, server = 2, listener = 3, manager = 4,
};

struct PsDisconnectPayloadV1 {
  PsDisconnectReasonV1 reason = PsDisconnectReasonV1::normal;
  PsDisconnectInitiatorV1 initiator = PsDisconnectInitiatorV1::parser;
  std::vector<PsDisconnectUuidV1> active_requests;
  std::vector<std::vector<std::uint8_t>> finality_tokens;
  std::vector<std::uint8_t> message_vector_set;
  bool operator==(const PsDisconnectPayloadV1&) const = default;
};

// Policy can only lower the Core ceilings. Zero permits no bytes/elements.
struct PsDisconnectLimitsV1 {
  std::uint64_t maximum_payload_bytes = 64ull * 1024 * 1024;
  std::uint64_t maximum_message_vector_bytes = 16ull * 1024 * 1024;
  std::uint32_t maximum_active_requests = 1024;
  std::uint32_t maximum_finality_tokens = 1024;
  std::uint32_t maximum_finality_token_bytes = 4096;
};

enum class PsDisconnectCodecStatusV1 : std::uint8_t {
  ok, frame_payload_invalid, resource_limit_exceeded,
};
struct PsDisconnectCodecDiagnosticV1 {
  PsDisconnectCodecStatusV1 status =
      PsDisconnectCodecStatusV1::frame_payload_invalid;
  std::uint16_t field_id = 0;
  [[nodiscard]] bool ok() const { return status == PsDisconnectCodecStatusV1::ok; }
  [[nodiscard]] const char* code() const {
    switch (status) {
      case PsDisconnectCodecStatusV1::ok: return "";
      case PsDisconnectCodecStatusV1::resource_limit_exceeded:
        return "PARSER_SERVER_IPC.RESOURCE_LIMIT_EXCEEDED";
      default: return "PARSER_SERVER_IPC.FRAME_PAYLOAD_INVALID";
    }
  }
};
struct PsDisconnectDecodeResultV1 {
  PsDisconnectCodecDiagnosticV1 diagnostic;
  PsDisconnectPayloadV1 payload;
  [[nodiscard]] bool ok() const { return diagnostic.ok(); }
};
struct PsDisconnectEncodeResultV1 {
  PsDisconnectCodecDiagnosticV1 diagnostic;
  std::vector<std::uint8_t> bytes;
  [[nodiscard]] bool ok() const { return diagnostic.ok(); }
};

// Exact outer SBPS-PAYLOAD-TLV1 layout, not endpoint admission or finality
// authority. The caller must validate the frame/profile/identity tuple and
// canonical MessageVectorSet component, and resolve tokens only against engine
// authority, before any side effect or terminal success. Tokens and message
// bytes are preserved unchanged; this codec never interprets them as authority.
// Decode validates every field and all resource ceilings before allocating any
// returned payload. Failure publishes no decoded fields or encoded prefix.
PsDisconnectDecodeResultV1 DecodePsDisconnectPayloadV1(
    std::span<const std::uint8_t> bytes,
    const PsDisconnectLimitsV1& limits = {});
PsDisconnectEncodeResultV1 EncodePsDisconnectPayloadV1(
    const PsDisconnectPayloadV1& payload,
    const PsDisconnectLimitsV1& limits = {});

}  // namespace scratchbird::parser::ipc
