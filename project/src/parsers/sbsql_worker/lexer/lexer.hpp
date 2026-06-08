// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "common/common.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace scratchbird::parser::sbsql {

enum class TokenKind {
  kIdentifier,
  kKeyword,
  kNumericLiteral,
  kStringLiteral,
  kBinaryLiteral,
  kTemporalLiteral,
  kUuidLiteral,
  kBooleanLiteral,
  kNullLiteral,
  kDefaultLiteral,
  kDocumentLiteral,
  kVectorLiteral,
  kRegexLiteral,
  kRangeLiteral,
  kParameter,
  kVariable,
  kOperator,
  kSymbol,
  kStatementTerminator,
  kComment,
  kWhitespace,
  kMetaCommand,
  kParserDirective,
  kEnd,
};

struct Token {
  TokenKind kind{TokenKind::kEnd};
  std::string text;
  std::size_t offset{0};
  std::size_t length{0};
  bool quoted{false};
  std::size_t line{1};
  std::size_t column{1};
  std::size_t end_line{1};
  std::size_t end_column{1};
  std::string raw_text;
  std::string literal_family;
  std::string keyword_class;
  std::string render_hint;
};

struct LexResult {
  std::vector<Token> tokens;
  MessageVectorSet messages;
};

LexResult Lex(std::string_view source);
std::string TokenKindName(TokenKind kind);
bool IsTriviaToken(const Token& token);

} // namespace scratchbird::parser::sbsql
