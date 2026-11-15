// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::parser::ipc {

// SEARCH_KEY: SB_MESSAGE_VECTOR_BINARY_FIELD (owner Q194 = 1, 2026-11-15).
// A field's value is text unless its kind says otherwise: binary_uuid holds
// the 16 raw bytes of an identity (as IdentityBytes carries one), never text.
// Renderers show it as canonical UUID text (RenderFieldValue), never as raw
// bytes, and the public filter refuses it as it refuses a text UUID. A field
// written as {name, value} stays text. (Was: name and value only; a binary
// identity was indistinguishable from text.)
enum class FieldKind : unsigned char { text = 0, binary_uuid = 1 };

struct Field {
  std::string name;
  std::string value;
  FieldKind kind = FieldKind::text;
};

// A binary_uuid field from the 16 raw bytes at `bytes` (exactly 16).
Field BinaryUuidField(std::string name, std::string_view bytes);
// The value as a renderer shows it: canonical lower-case UUID text for a
// binary_uuid of 16 bytes ("invalid_binary_uuid" otherwise), else the text.
std::string RenderFieldValue(const Field& field);

struct Diagnostic {
  std::string code;
  std::string severity;
  std::string message;
  std::string component;
  std::vector<Field> fields;
};

struct MessageVectorSet {
  std::vector<Diagnostic> diagnostics;

  [[nodiscard]] bool ok() const;
  [[nodiscard]] bool has_errors() const;
};

Diagnostic MakeDiagnostic(std::string code,
                          std::string severity,
                          std::string message,
                          std::string component,
                          std::vector<Field> fields = {});

std::string ToUpperAscii(std::string_view text);
std::string EscapeJson(std::string_view text);
// SB_MESSAGE_VECTOR_BINARY_FIELD (owner Q194 = 1): EscapeJson whose output
// is always well-formed UTF-8 ('?' for a byte beginning no well-formed
// sequence); for message-vector renderings, not for user data.
std::string EscapeDiagnosticJson(std::string_view text);
bool LooksLikeCanonicalUuid(std::string_view value);
bool IsPublicDiagnosticFieldAllowed(std::string_view name, std::string_view value);
// SB_MESSAGE_VECTOR_BINARY_FIELD: a binary_uuid field is refused as a text
// UUID value is; any other field as (name, value).
bool IsPublicDiagnosticFieldAllowed(const Field& field);
std::string MessageVectorToJson(const MessageVectorSet& messages);

} // namespace scratchbird::parser::ipc
