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

namespace scratchbird::parser::sbsql {

struct Field {
  std::string name;
  std::string value;
};

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
bool LooksLikeCanonicalUuid(std::string_view value);
bool IsPublicDiagnosticFieldAllowed(std::string_view name, std::string_view value);
std::string MessageVectorToJson(const MessageVectorSet& messages);

} // namespace scratchbird::parser::sbsql
