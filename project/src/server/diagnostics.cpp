// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_PRODUCT_DIAGNOSTICS

#include "diagnostics.hpp"

#include <cctype>
#include <sstream>
#include <string>

namespace scratchbird::server {

const char* SeverityName(ServerDiagnosticSeverity severity) {
  switch (severity) {
    case ServerDiagnosticSeverity::kInfo:
      return "info";
    case ServerDiagnosticSeverity::kWarning:
      return "warning";
    case ServerDiagnosticSeverity::kError:
      return "error";
  }
  return "error";
}

std::string EscapeMessageVectorText(const std::string& value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (const char ch : value) {
    switch (ch) {
      case '\\':
        escaped += "\\\\\\\\";
        break;
      case '"':
        escaped += "\\\\\"";
        break;
      case '\n':
        escaped += "\\\\n";
        break;
      case '\r':
        escaped += "\\\\r";
        break;
      case '\t':
        escaped += "\\\\t";
        break;
      default:
        escaped += ch;
        break;
    }
  }
  return escaped;
}

bool LooksLikeCanonicalUuid(std::string_view value) {
  if (value.size() != 36) return false;
  for (std::size_t i = 0; i < value.size(); ++i) {
    const char ch = value[i];
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (ch != '-') return false;
      continue;
    }
    if (!std::isxdigit(static_cast<unsigned char>(ch))) return false;
  }
  return true;
}

bool IsPublicDiagnosticFieldAllowed(std::string_view key, std::string_view value) {
  if (LooksLikeCanonicalUuid(value)) return false;
  std::string lowered(key);
  for (char& ch : lowered) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  std::string lowered_value(value);
  for (char& ch : lowered_value) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  if (lowered == "presented_name" ||
      lowered == "object_name" ||
      lowered == "relation_name" ||
      lowered == "schema_name" ||
      lowered == "canonical_name") {
    return false;
  }
  if (lowered == "expected_token_set") {
    return lowered_value.find("hidden_table") == std::string::npos &&
           lowered_value.find("missing_table") == std::string::npos &&
           lowered_value.find("policy.secret") == std::string::npos &&
           lowered_value.find("provider.local_password") == std::string::npos &&
           lowered_value.find("/tmp/secret") == std::string::npos;
  }
  if (lowered.find("password") != std::string::npos ||
      lowered.find("secret") != std::string::npos ||
      lowered.find("token") != std::string::npos ||
      lowered.find("credential") != std::string::npos ||
      lowered.find("policy") != std::string::npos ||
      lowered.find("provider") != std::string::npos ||
      lowered.find("path") != std::string::npos ||
      lowered.find("hidden") != std::string::npos ||
      lowered.find("internal") != std::string::npos ||
      lowered.find("filespace") != std::string::npos ||
      lowered.find("principal") != std::string::npos) {
    return false;
  }
  if (lowered_value.find("hidden_table") != std::string::npos ||
      lowered_value.find("missing_table") != std::string::npos ||
      lowered_value.find("policy.secret") != std::string::npos ||
      lowered_value.find("provider.local_password") != std::string::npos ||
      lowered_value.find("/tmp/secret") != std::string::npos) {
    return false;
  }
  if (lowered.find("uuid") != std::string::npos &&
      value != "not_assigned" && value != "not_available" && value != "redacted") {
    return false;
  }
  return true;
}

std::string LowerDiagnosticText(std::string_view value) {
  std::string lowered(value);
  for (char& ch : lowered) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return lowered;
}

bool IsRetryableDiagnosticCode(std::string_view code) {
  const auto lowered = LowerDiagnosticText(code);
  return lowered.find("timeout") != std::string::npos ||
         lowered.find("busy") != std::string::npos ||
         lowered.find("retry") != std::string::npos ||
         lowered.find("stale") != std::string::npos ||
         lowered.find("unavailable") != std::string::npos ||
         lowered.find("drain") != std::string::npos ||
         lowered.find("ack") != std::string::npos ||
         lowered.find("lock_timeout") != std::string::npos ||
         lowered.find("serialization") != std::string::npos;
}

std::string DiagnosticShapeIdForCode(std::string_view code) {
  const auto lowered = LowerDiagnosticText(code);
  if (lowered.find("parser_server_ipc") != std::string::npos ||
      lowered.find("ipc") != std::string::npos) {
    return "diag.parser_server_ipc.v1";
  }
  if (lowered.find("security") != std::string::npos ||
      lowered.find("auth") != std::string::npos ||
      lowered.find("access_denied") != std::string::npos) {
    return "diag.rights.failure.v1";
  }
  if (lowered.find("listener") != std::string::npos) {
    return "diag.listener.control.v1";
  }
  if (lowered.find("management") != std::string::npos ||
      lowered.find("maintenance") != std::string::npos ||
      lowered.find("dblc") != std::string::npos ||
      lowered.find("shutdown") != std::string::npos ||
      lowered.find("lifecycle") != std::string::npos) {
    return "diag.server.lifecycle.v1";
  }
  return "diag.message_vector.v1";
}

bool PrivateFieldValueRequiresRedaction(std::string_view key, std::string_view value) {
  const auto lowered_key = LowerDiagnosticText(key);
  const auto lowered_value = LowerDiagnosticText(value);
  return lowered_key.find("password") != std::string::npos ||
         lowered_key.find("secret") != std::string::npos ||
         lowered_key.find("token") != std::string::npos ||
         lowered_key.find("credential") != std::string::npos ||
         lowered_key.find("private_key") != std::string::npos ||
         lowered_key.find("encryption_key") != std::string::npos ||
         lowered_value.find("password=") != std::string::npos ||
         lowered_value.find("secret=") != std::string::npos ||
         lowered_value.find("token=") != std::string::npos ||
         lowered_value.find("credential=") != std::string::npos ||
         lowered_value.find("private_key=") != std::string::npos ||
         lowered_value.find("encryption_key=") != std::string::npos;
}

std::string RedactPrivateFieldValue(std::string_view key, const std::string& value) {
  return PrivateFieldValueRequiresRedaction(key, value) ? "[redacted]" : value;
}

void WriteMessageVectorHeader(std::ostringstream* out,
                              const ServerDiagnostic& diagnostic,
                              std::string_view visibility) {
  const auto shape = diagnostic.diagnostic_shape_id.empty()
      ? DiagnosticShapeIdForCode(diagnostic.code)
      : diagnostic.diagnostic_shape_id;
  const bool retryable = diagnostic.retryable || IsRetryableDiagnosticCode(diagnostic.code);
  *out << "{\"message_vector\":{\"code\":\"" << EscapeMessageVectorText(diagnostic.code)
       << "\",\"message_key\":\"" << EscapeMessageVectorText(diagnostic.message_key)
       << "\",\"severity\":\"" << SeverityName(diagnostic.severity)
       << "\",\"safe_message\":\"" << EscapeMessageVectorText(diagnostic.safe_message)
       << "\",\"diagnostic_shape_id\":\"" << EscapeMessageVectorText(shape)
       << "\",\"visibility\":\"" << visibility
       << "\",\"redaction_profile\":\"canonical_lifecycle_least_disclosure_v1"
       << "\",\"retryable\":" << (retryable ? "true" : "false")
       << ",\"parser_finality_authority\":false"
       << ",\"reference_finality_authority\":false";
}

std::string ToMessageVectorJsonLine(const ServerDiagnostic& diagnostic) {
  std::ostringstream out;
  WriteMessageVectorHeader(&out, diagnostic, "public");
  out << ",\"correlation_evidence\":\""
      << (diagnostic.correlation_uuid.empty() && diagnostic.request_uuid.empty() &&
                  diagnostic.session_uuid.empty() && diagnostic.database_uuid.empty()
              ? "absent"
              : "redacted")
      << "\",\"fields\":{";
  bool first = true;
  for (const auto& field : diagnostic.fields) {
    if (!IsPublicDiagnosticFieldAllowed(field.key, field.value)) continue;
    if (!first) {
      out << ',';
    }
    first = false;
    out << '"' << EscapeMessageVectorText(field.key) << "\":\""
        << EscapeMessageVectorText(field.value) << '"';
  }
  out << "}}}";
  return out.str();
}

std::string ToPrivateMessageVectorJsonLine(const ServerDiagnostic& diagnostic) {
  std::ostringstream out;
  WriteMessageVectorHeader(&out, diagnostic, "private");
  out << ",\"correlation\":{\"correlation_uuid\":\""
      << EscapeMessageVectorText(diagnostic.correlation_uuid)
      << "\",\"request_uuid\":\"" << EscapeMessageVectorText(diagnostic.request_uuid)
      << "\",\"session_uuid\":\"" << EscapeMessageVectorText(diagnostic.session_uuid)
      << "\",\"database_uuid\":\"" << EscapeMessageVectorText(diagnostic.database_uuid)
      << "\"},\"fields\":{";
  bool first = true;
  for (const auto& field : diagnostic.fields) {
    if (!first) {
      out << ',';
    }
    first = false;
    out << '"' << EscapeMessageVectorText(field.key) << "\":\""
        << EscapeMessageVectorText(RedactPrivateFieldValue(field.key, field.value)) << '"';
  }
  out << "}}}";
  return out.str();
}

}  // namespace scratchbird::server
