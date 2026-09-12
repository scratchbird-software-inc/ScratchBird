// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "server/diagnostics.hpp"
#include "server/server_observability.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <string>

namespace server = scratchbird::server;
namespace {
bool ReadU32(std::uint32_t* value) {
  std::array<unsigned char, 4> bytes{};
  if (!std::cin.read(reinterpret_cast<char*>(bytes.data()), 4)) return false;
  *value = std::uint32_t(bytes[0]) | (std::uint32_t(bytes[1]) << 8) |
           (std::uint32_t(bytes[2]) << 16) | (std::uint32_t(bytes[3]) << 24);
  return true;
}
void Emit(const std::string& value) {
  const auto size = static_cast<std::uint32_t>(value.size());
  const std::array<char, 4> bytes{static_cast<char>(size),
      static_cast<char>(size >> 8), static_cast<char>(size >> 16),
      static_cast<char>(size >> 24)};
  std::cout.write(bytes.data(), bytes.size());
  std::cout.write(value.data(), value.size());
}
}

int main() {
  // Length framing is test transport, not SBPS. Even malformed JSON must
  // remain associated with its exact expected case in the independent oracle.
  std::uint32_t count = 0;
  if (!ReadU32(&count) || count > 65536) return 2;
  for (std::uint32_t ordinal = 0; ordinal < count; ++ordinal) {
    std::uint32_t size = 0;
    if (!ReadU32(&size) || size > 65536) return 2;
    std::string value(size, '\0');
    if (!std::cin.read(value.data(), size)) return 2;
    Emit('"' + server::EscapeMessageVectorText(value) + '"');
    server::ServerDiagnostic diagnostic;
    diagnostic.code = "JSON_ESCAPE_COMPONENT_FIXTURE";
    diagnostic.message_key = "json.escape.fixture";
    diagnostic.safe_message = value;
    diagnostic.fields = {{"value", value}, {"key_" + value, value}};
    Emit(server::ToMessageVectorJsonLine(diagnostic));
    Emit(server::ToPrivateMessageVectorJsonLine(diagnostic));
    if (ordinal < 128) {
      // Actual lifecycle adapter; no durable audit acceptance is asserted.
      server::ServerObservabilityState state;
      server::ServerLifecycleObservabilityEvent event;
      event.operation_key = "open_database";
      event.outcome = "refused";
      event.private_detail = value;
      const auto record = server::RecordServerLifecycleObservability(&state, event);
      if (!record.recorded || state.lifecycle_events.size() != 1 ||
          state.lifecycle_events.front().private_detail != value) return 3;
      Emit(record.message_vector_public_json);
      Emit(record.message_vector_private_json);
    }
  }
  if (std::cin.peek() != std::char_traits<char>::eof()) return 2;
  std::cout.flush();
  return std::cout ? 0 : 4;
}
