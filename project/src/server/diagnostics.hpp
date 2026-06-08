// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_PRODUCT_SKELETON_DIAGNOSTICS

#pragma once

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
};

const char* SeverityName(ServerDiagnosticSeverity severity);
std::string EscapeMessageVectorText(const std::string& value);
bool LooksLikeCanonicalUuid(std::string_view value);
bool IsPublicDiagnosticFieldAllowed(std::string_view key, std::string_view value);
bool IsRetryableDiagnosticCode(std::string_view code);
std::string DiagnosticShapeIdForCode(std::string_view code);
std::string ToMessageVectorJsonLine(const ServerDiagnostic& diagnostic);
std::string ToPrivateMessageVectorJsonLine(const ServerDiagnostic& diagnostic);

}  // namespace scratchbird::server
