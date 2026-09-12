// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace scratchbird::parser::ipc {

// Validation of the currently deployed private response, NOT Core schema
// 1074. Producer and consumer must migrate together to SBPS-PAYLOAD-TLV1.
// No outcome here establishes engine transaction finality.
enum class DisconnectResultDisposition {
  invalid,
  detached,
  recovery_quarantined,
  session_not_found,
  binding_mismatch,
};

using DisconnectIdentity = std::array<std::uint8_t, 16>;

inline DisconnectResultDisposition ValidatePrivateDisconnectResult(
    std::uint16_t message_type,
    std::uint32_t schema_id,
    std::uint32_t flags,
    const DisconnectIdentity& response_connection,
    const DisconnectIdentity& response_session,
    const DisconnectIdentity& expected_connection,
    const DisconnectIdentity& expected_session,
    std::span<const std::uint8_t> payload) {
  const auto present = [](const auto& id) {
    return std::any_of(id.begin(), id.end(), [](auto b) { return b != 0; });
  };
  if (message_type != 74 || schema_id != 3005 || flags != 5 ||
      !present(expected_connection) || !present(expected_session) ||
      response_connection != expected_connection ||
      response_session != expected_session) {
    return DisconnectResultDisposition::invalid;
  }
  std::size_t offset = 0;
  const auto read_string = [&](std::string_view* value) {
    if (payload.size() - offset < 2) return false;
    const auto size = static_cast<std::size_t>(payload[offset]) |
                      (static_cast<std::size_t>(payload[offset + 1]) << 8);
    offset += 2;
    if (size > payload.size() - offset) return false;
    *value = std::string_view(
        reinterpret_cast<const char*>(payload.data() + offset), size);
    offset += size;
    return value->find('\0') == std::string_view::npos;
  };
  std::string_view outcome, detail;
  if (!read_string(&outcome) || payload.size() - offset < 16 ||
      !std::equal(expected_session.begin(), expected_session.end(),
                  payload.begin() + offset)) {
    return DisconnectResultDisposition::invalid;
  }
  offset += 16;
  if (!read_string(&detail) || offset != payload.size()) {
    return DisconnectResultDisposition::invalid;
  }
  if (outcome == "detached") return DisconnectResultDisposition::detached;
  if (outcome == "recovery_quarantined") {
    return DisconnectResultDisposition::recovery_quarantined;
  }
  if (outcome == "session_not_found") {
    return DisconnectResultDisposition::session_not_found;
  }
  if (outcome == "binding_mismatch") {
    return DisconnectResultDisposition::binding_mismatch;
  }
  return DisconnectResultDisposition::invalid;
}

}  // namespace scratchbird::parser::ipc
