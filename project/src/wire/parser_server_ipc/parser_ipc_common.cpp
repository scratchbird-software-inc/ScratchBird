// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "parser_ipc_common.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace scratchbird::parser::sbsql {

bool MessageVectorSet::ok() const { return !has_errors(); }

bool MessageVectorSet::has_errors() const {
  for (const auto& diagnostic : diagnostics) {
    if (ToUpperAscii(diagnostic.severity) == "ERROR" ||
        ToUpperAscii(diagnostic.severity) == "FATAL") {
      return true;
    }
  }
  return false;
}

Diagnostic MakeDiagnostic(std::string code,
                          std::string severity,
                          std::string message,
                          std::string component,
                          std::vector<Field> fields) {
  return {std::move(code), std::move(severity), std::move(message), std::move(component), std::move(fields)};
}

std::string ToUpperAscii(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
    return static_cast<char>(std::toupper(c));
  });
  return out;
}

std::string EscapeJson(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (const char ch : text) {
    switch (ch) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(ch) < 0x20) {
          out += "?";
        } else {
          out.push_back(ch);
        }
        break;
    }
  }
  return out;
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

bool IsPublicDiagnosticFieldAllowed(std::string_view name, std::string_view value) {
  if (LooksLikeCanonicalUuid(value)) return false;
  const auto lowered = ToUpperAscii(name);
  const auto lowered_value = ToUpperAscii(value);
  if (lowered == "PRESENTED_NAME" ||
      lowered == "OBJECT_NAME" ||
      lowered == "RELATION_NAME" ||
      lowered == "SCHEMA_NAME" ||
      lowered == "CANONICAL_NAME") {
    return false;
  }
  if (lowered == "EXPECTED_TOKEN_SET") {
    return lowered_value.find("HIDDEN_TABLE") == std::string::npos &&
           lowered_value.find("MISSING_TABLE") == std::string::npos &&
           lowered_value.find("POLICY.SECRET") == std::string::npos &&
           lowered_value.find("PROVIDER.LOCAL_PASSWORD") == std::string::npos &&
           lowered_value.find("/TMP/SECRET") == std::string::npos;
  }
  if (lowered.find("PASSWORD") != std::string::npos ||
      lowered.find("SECRET") != std::string::npos ||
      lowered.find("TOKEN") != std::string::npos ||
      lowered.find("CREDENTIAL") != std::string::npos ||
      lowered.find("POLICY") != std::string::npos ||
      lowered.find("PROVIDER") != std::string::npos ||
      lowered.find("PATH") != std::string::npos ||
      lowered.find("HIDDEN") != std::string::npos ||
      lowered.find("INTERNAL") != std::string::npos ||
      lowered.find("FILESPACE") != std::string::npos ||
      lowered.find("PRINCIPAL") != std::string::npos) {
    return false;
  }
  if (lowered_value.find("HIDDEN_TABLE") != std::string::npos ||
      lowered_value.find("MISSING_TABLE") != std::string::npos ||
      lowered_value.find("POLICY.SECRET") != std::string::npos ||
      lowered_value.find("PROVIDER.LOCAL_PASSWORD") != std::string::npos ||
      lowered_value.find("/TMP/SECRET") != std::string::npos) {
    return false;
  }
  if (lowered.find("UUID") != std::string::npos &&
      value != "not_assigned" && value != "not_available" && value != "redacted") {
    return false;
  }
  return true;
}

std::string MessageVectorToJson(const MessageVectorSet& messages) {
  std::ostringstream out;
  out << "[";
  for (std::size_t i = 0; i < messages.diagnostics.size(); ++i) {
    const auto& diagnostic = messages.diagnostics[i];
    if (i != 0) out << ',';
    out << "{\"code\":\"" << EscapeJson(diagnostic.code)
        << "\",\"severity\":\"" << EscapeJson(diagnostic.severity)
        << "\",\"message\":\"" << EscapeJson(diagnostic.message)
        << "\",\"component\":\"" << EscapeJson(diagnostic.component)
        << "\",\"fields\":{";
    bool first_field = true;
    for (const auto& field : diagnostic.fields) {
      if (!IsPublicDiagnosticFieldAllowed(field.name, field.value)) continue;
      if (!first_field) out << ',';
      first_field = false;
      out << '\"' << EscapeJson(field.name) << "\":\"" << EscapeJson(field.value) << '\"';
    }
    out << "}}";
  }
  out << "]";
  return out.str();
}

} // namespace scratchbird::parser::sbsql
