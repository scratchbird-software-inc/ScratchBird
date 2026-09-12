// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "lexer/lexer.hpp"

#include <optional>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace scratchbird::parser::sbsql {

struct NativeRelationalAstDocument;
// Validate only a unary CTE wrapper and return its actual child root. Child
// query semantics and metadata must still pass the ordinary binder/lowerer.
std::optional<std::uint32_t> NativeCteProducerRoot(
    const NativeRelationalAstDocument& ast);

// Parser-local, half-open ranges into the caller's significant token stream.
// These are syntax handles, never catalog identities or executable SBLR.
struct CteTokenRange {
  std::size_t begin{0};
  std::size_t end{0};
  [[nodiscard]] bool empty() const { return begin == end; }
};

enum class CteMaterialization { kDefault, kMaterialized, kNotMaterialized };
enum class CteUnionDuplicates { kDistinct, kAll };
struct CteUnionSyntax {
  std::size_t token{0};
  std::size_t right_begin{0};
  CteUnionDuplicates duplicates{CteUnionDuplicates::kDistinct};
};
struct CteSearchSyntax {
  bool depth_first{false};
  std::vector<CteTokenRange> ordering_terms;
  std::size_t sequence_name{0};
};
struct CteCycleSyntax {
  std::vector<std::size_t> columns;
  std::size_t mark_name{0};
  std::optional<CteTokenRange> mark_value;
  std::optional<CteTokenRange> default_value;
  std::size_t path_name{0};
};
struct CteDefinitionSyntax {
  std::size_t name{0};
  std::vector<std::size_t> columns;
  std::vector<CteTokenRange> key_expressions;
  CteMaterialization materialization{CteMaterialization::kDefault};
  CteTokenRange body;
  // Top-level unquoted UNION token locations are candidates for the child
  // query parser, not semantic set operators or an anchor/recursive split.
  // Contextual identifiers and self-reference require actual query binding.
  std::vector<CteUnionSyntax> union_keywords;
  std::optional<CteSearchSyntax> search;
  std::optional<CteCycleSyntax> cycle;
};
enum class WithClauseSyntaxStatus { kNotRecognized, kValid, kMalformed, kLimit };
struct WithClauseSyntax {
  WithClauseSyntaxStatus status{WithClauseSyntaxStatus::kNotRecognized};
  bool recursive_keyword{false};
  std::vector<CteDefinitionSyntax> definitions;
  CteTokenRange query;
  std::size_t error_token{0};
  std::string detail;
};

// Parses WITH framing, not its expression/query children. All child ranges
// must subsequently be parsed and bound; kValid grants no execution admission.
// The caller supplies its existing token/depth budget, without a second policy.
WithClauseSyntax ParseWithClauseSyntax(std::span<const Token* const> tokens,
                                      std::size_t maximum_tokens,
                                      std::size_t maximum_depth);

}  // namespace scratchbird::parser::sbsql
