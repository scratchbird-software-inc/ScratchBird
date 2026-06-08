// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "ast_model.hpp"

#include <string>
#include <string_view>
#include <variant>

namespace scratchbird::parser::native_v3 {

struct ParseDiagnostic {
  std::string code;
  std::string message;
  scratchbird::parser::ast::SourceRange source_range;
};

struct ParseResult {
  std::variant<scratchbird::parser::ast::ShowIdentityAst, ParseDiagnostic> value;

  bool ok() const;
};

ParseResult ParseMinimalIdentityShow(std::string_view command_text);

std::string SerializeDiagnosticToJson(const ParseDiagnostic& diagnostic);
std::string SerializeParseResultToJson(const ParseResult& result);

}  // namespace scratchbird::parser::native_v3
