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

namespace scratchbird::parser::ipc {

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

namespace {
// SB_MESSAGE_VECTOR_BINARY_FIELD (owner Q194 = 1): the length of the
// well-formed UTF-8 sequence at text[at] (RFC 3629: no overlong form, no
// surrogate, nothing above U+10FFFF), 0 when there is none.
std::size_t Utf8SequenceLength(std::string_view text, std::size_t at) {
  const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(text[i]); };
  const unsigned char lead = byte(at);
  std::size_t length = 0;
  unsigned char low = 0x80, high = 0xBF;
  if (lead >= 0xC2 && lead <= 0xDF) {
    length = 2;
  } else if (lead >= 0xE0 && lead <= 0xEF) {
    length = 3;
    if (lead == 0xE0) low = 0xA0;
    if (lead == 0xED) high = 0x9F;
  } else if (lead >= 0xF0 && lead <= 0xF4) {
    length = 4;
    if (lead == 0xF0) low = 0x90;
    if (lead == 0xF4) high = 0x8F;
  } else {
    return 0;
  }
  if (at + length > text.size()) return 0;
  if (byte(at + 1) < low || byte(at + 1) > high) return 0;
  for (std::size_t i = 2; i < length; ++i)
    if (byte(at + i) < 0x80 || byte(at + i) > 0xBF) return 0;
  return length;
}
}  // namespace

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

// SB_MESSAGE_VECTOR_BINARY_FIELD (owner Q194 = 1, 2026-11-15): EscapeJson for
// message-vector renderings only; the output is always well-formed UTF-8: a
// byte that does not begin a well-formed sequence becomes '?', as a control
// character does. EscapeJson itself is unchanged: it also serialises user
// data (lowering payloads), which must not be rewritten.
std::string EscapeDiagnosticJson(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (std::size_t at = 0; at < text.size();) {
    if (static_cast<unsigned char>(text[at]) < 0x80) {
      out += EscapeJson(text.substr(at, 1));
      ++at;
      continue;
    }
    const auto length = Utf8SequenceLength(text, at);
    if (length == 0) {
      out += "?";
      ++at;
    } else {
      out.append(text.substr(at, length));
      at += length;
    }
  }
  return out;
}

Field BinaryUuidField(std::string name, std::string_view bytes) {
  return {std::move(name), std::string(bytes), FieldKind::binary_uuid};
}

std::string RenderFieldValue(const Field& field) {
  if (field.kind != FieldKind::binary_uuid) return field.value;
  if (field.value.size() != 16) return "invalid_binary_uuid";
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(36);
  for (std::size_t i = 0; i < 16; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) out.push_back('-');
    const auto b = static_cast<unsigned char>(field.value[i]);
    out.push_back(kHex[b >> 4]);
    out.push_back(kHex[b & 0x0F]);
  }
  return out;
}

bool IsPublicDiagnosticFieldAllowed(const Field& field) {
  if (field.kind == FieldKind::binary_uuid) return false;
  return IsPublicDiagnosticFieldAllowed(field.name, field.value);
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
    out << "{\"code\":\"" << EscapeDiagnosticJson(diagnostic.code)
        << "\",\"severity\":\"" << EscapeDiagnosticJson(diagnostic.severity)
        << "\",\"message\":\"" << EscapeDiagnosticJson(diagnostic.message)
        << "\",\"component\":\"" << EscapeDiagnosticJson(diagnostic.component)
        << "\",\"fields\":{";
    bool first_field = true;
    for (const auto& field : diagnostic.fields) {
      // SB_MESSAGE_VECTOR_BINARY_FIELD: the field's kind decides (a
      // binary_uuid is refused as a text UUID is); a value is rendered as
      // its kind shows it. (Was: (name, value) and the raw value.)
      if (!IsPublicDiagnosticFieldAllowed(field)) continue;
      if (!first_field) out << ',';
      first_field = false;
      out << '\"' << EscapeDiagnosticJson(field.name) << "\":\"" << EscapeDiagnosticJson(RenderFieldValue(field)) << '\"';
    }
    out << "}}";
  }
  out << "]";
  return out.str();
}

} // namespace scratchbird::parser::ipc
