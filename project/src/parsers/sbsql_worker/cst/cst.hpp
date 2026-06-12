// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "lexer/lexer.hpp"
#include "resources/language_resource_contract.hpp"

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
  std::string language_profile_uuid{"sbsql.builtin.recovery.en"};
  std::string exact_language_tag{"en"};
  std::string topology_profile_uuid{"topology.sbsql.canonical_svo.v1"};
  std::string input_syntax_profile_uuid{"sbsql.syntax.en.canonical"};
  std::string common_resource_hash{"builtin.common.sbsql.v1"};
  std::string resource_epoch_vector_uuid{"resource_epoch_vector.pending_server_context"};
  std::string line_ending_mode{"lf"};
  bool cluster_schema_visible_flag{false};
  bool trivia_preserved{true};
  bool canonical_english_fallback_used{false};
  std::vector<Token> tokens;
  std::vector<CstNode> nodes;
  std::size_t root_node_index{0};
  CanonicalElementStream canonical_element_stream;
  MessageVectorSet messages;
};

struct LanguageTokenAlias {
  std::string localized_text;
  std::string canonical_text;
  std::string canonical_token_id;
  std::string alias_id;
};

struct LanguageNormalizationOptions {
  std::string resource_identity{"sbsql.common_resource_pack.v1"};
  std::string language_profile_uuid{"sbsql.builtin.recovery.en"};
  std::string exact_language_tag{"en"};
  std::string dialect_profile_uuid{"sbsql.v3"};
  std::string topology_profile_uuid{"topology.sbsql.canonical_svo.v1"};
  std::string input_syntax_profile_uuid{"sbsql.syntax.en.canonical"};
  std::string common_resource_hash{"builtin.common.sbsql.v1"};
  bool canonical_english_fallback_used{false};
  std::vector<LanguageTokenAlias> aliases;
};

CstDocument BuildCst(std::string_view source);
CstDocument BuildCst(std::string_view source, const LanguageNormalizationOptions& options);
SourceRange TokenSourceRange(const Token& token);
std::string ReconstructSourceFromTokens(const CstDocument& cst);

} // namespace scratchbird::parser::sbsql
