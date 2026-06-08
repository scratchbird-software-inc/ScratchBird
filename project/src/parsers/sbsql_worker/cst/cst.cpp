// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cst/cst.hpp"

namespace scratchbird::parser::sbsql {
namespace {

std::string LineEndingMode(std::string_view source) {
  bool saw_lf = false;
  bool saw_crlf = false;
  bool saw_cr = false;
  for (std::size_t index = 0; index < source.size(); ++index) {
    if (source[index] != '\r' && source[index] != '\n') continue;
    if (source[index] == '\r' && index + 1 < source.size() && source[index + 1] == '\n') {
      saw_crlf = true;
      ++index;
    } else if (source[index] == '\r') {
      saw_cr = true;
    } else {
      saw_lf = true;
    }
  }
  const int modes = (saw_lf ? 1 : 0) + (saw_crlf ? 1 : 0) + (saw_cr ? 1 : 0);
  if (modes > 1) return "mixed";
  if (saw_crlf) return "crlf";
  if (saw_cr) return "cr";
  return "lf";
}

} // namespace

SourceRange TokenSourceRange(const Token& token) {
  return {token.offset, token.length, token.line, token.column, token.end_line,
          token.end_column};
}

std::string ReconstructSourceFromTokens(const CstDocument& cst) {
  std::string reconstructed;
  for (const auto& token : cst.tokens) {
    if (token.kind == TokenKind::kEnd) continue;
    reconstructed += token.raw_text;
  }
  return reconstructed;
}

CstDocument BuildCst(std::string_view source) {
  auto lexed = Lex(source);
  CstDocument document;
  document.source = std::string(source);
  document.source_hash = std::to_string(Fnv1a64(source));
  document.source_buffer_uuid = "cst.source_buffer." + document.source_hash;
  document.line_ending_mode = LineEndingMode(source);
  document.tokens = std::move(lexed.tokens);
  document.messages = std::move(lexed.messages);

  CstNode root;
  root.kind = "document";
  root.range.offset = 0;
  root.range.length = source.size();
  if (!document.tokens.empty()) {
    const auto& end = document.tokens.back();
    root.range.end_line = end.line;
    root.range.end_column = end.column;
  }
  document.nodes.push_back(std::move(root));
  document.root_node_index = 0;

  for (std::size_t index = 0; index < document.tokens.size(); ++index) {
    const auto& token = document.tokens[index];
    if (token.kind == TokenKind::kEnd) continue;
    CstNode node;
    node.kind = TokenKindName(token.kind);
    node.range = TokenSourceRange(token);
    node.token_index = index;
    node.trivia = IsTriviaToken(token);
    node.raw_text = token.raw_text;
    document.nodes[document.root_node_index].children.push_back(document.nodes.size());
    document.nodes.push_back(std::move(node));
  }

  return document;
}

} // namespace scratchbird::parser::sbsql
