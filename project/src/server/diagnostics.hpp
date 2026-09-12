// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_PRODUCT_DIAGNOSTICS

#pragma once

#include "../core/uuid/diagnostic_identity.hpp"
#include "../server_engine_bridge/diagnostic_fields.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::server {

enum class ServerDiagnosticSeverity {
  kInfo,
  kWarning,
  kError,
};

struct ServerDiagnosticField {
  std::string key;
  std::string value;
};

struct ServerDiagnostic {
  std::string code;
  std::string message_key;
  ServerDiagnosticSeverity severity = ServerDiagnosticSeverity::kError;
  std::string safe_message;
  std::vector<ServerDiagnosticField> fields;
  std::string diagnostic_shape_id;
  bool retryable = false;
  std::string correlation_uuid;
  std::string request_uuid;
  std::string session_uuid;
  std::string database_uuid;
  // Server-only deterministic branch identity. Never serialized to clients.
  std::string internal_audit_key;
  // Newly emitted server records own this identity. An engine-to-server
  // adapter must preserve the engine occurrence instead of using this new ID.
  std::array<std::uint8_t, 16> occurrence_uuid =
      scratchbird::core::uuid::NewDiagnosticOccurrenceUuid();
  // Trusted engine source, not parser-safe payload. Retain exact key, native
  // cause, canonical severity/retry/outcome and fields until the owning bridge
  // applies actual template/redaction authority. Legacy serializers ignore it.
  std::optional<scratchbird::server_engine_bridge::EngineDiagnosticSnapshot>
      engine_source_snapshot;
};

// Adopt only the matching source with a valid binary occurrence identity.
// Failure leaves the complete target unchanged, including allocation failure.
bool AdoptEngineDiagnosticSource(
    const scratchbird::server_engine_bridge::EngineDiagnosticSnapshot& source,
    ServerDiagnostic* target);

const char* SeverityName(ServerDiagnosticSeverity severity);
std::string EscapeMessageVectorText(const std::string& value);
bool LooksLikeCanonicalUuid(std::string_view value);
bool IsPublicDiagnosticFieldAllowed(std::string_view key, std::string_view value);
std::string DiagnosticShapeIdForCode(std::string_view code);
std::string ToMessageVectorJsonLine(const ServerDiagnostic& diagnostic);
std::string ToPrivateMessageVectorJsonLine(const ServerDiagnostic& diagnostic);

}  // namespace scratchbird::server
