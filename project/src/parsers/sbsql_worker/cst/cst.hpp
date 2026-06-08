// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "lexer/lexer.hpp"

#include <string>
#include <vector>

namespace scratchbird::parser::sbsql {

struct SourceRange {
  std::size_t offset{0};
  std::size_t length{0};
  std::size_t line{1};
  std::size_t column{1};
  std::size_t end_line{1};
  std::size_t end_column{1};
};

struct CstNode {
  std::string kind;
  SourceRange range;
  std::size_t token_index{0};
  bool trivia{false};
  std::string raw_text;
  std::vector<std::size_t> children;
};

struct CstDocument {
  std::string source;
  std::string source_buffer_uuid;
  std::string source_hash;
  std::string dialect_profile_uuid{"sbsql.default"};
  std::string resource_epoch_vector_uuid{"resource_epoch_vector.pending_server_context"};
  std::string line_ending_mode{"lf"};
  bool cluster_schema_visible_flag{false};
  bool trivia_preserved{true};
  std::vector<Token> tokens;
  std::vector<CstNode> nodes;
  std::size_t root_node_index{0};
  MessageVectorSet messages;
};

CstDocument BuildCst(std::string_view source);
SourceRange TokenSourceRange(const Token& token);
std::string ReconstructSourceFromTokens(const CstDocument& cst);

} // namespace scratchbird::parser::sbsql
