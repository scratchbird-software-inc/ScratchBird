// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cst/cst.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

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

bool EqualsFoldAscii(std::string_view left, std::string_view right) {
  return ToUpperAscii(left) == ToUpperAscii(right);
}

std::string LiteralTokenId(TokenKind kind) {
  switch (kind) {
    case TokenKind::kNumericLiteral: return "SBSQL.TOKEN.NUMERIC_LITERAL";
    case TokenKind::kStringLiteral: return "SBSQL.TOKEN.STRING_LITERAL";
    case TokenKind::kBinaryLiteral: return "SBSQL.TOKEN.BINARY_LITERAL";
    case TokenKind::kTemporalLiteral: return "SBSQL.TOKEN.TEMPORAL_LITERAL";
    case TokenKind::kUuidLiteral: return "SBSQL.TOKEN.UUID_LITERAL";
    case TokenKind::kBooleanLiteral: return "SBSQL.TOKEN.BOOLEAN_LITERAL";
    case TokenKind::kNullLiteral: return "SBSQL.TOKEN.NULL_LITERAL";
    case TokenKind::kDefaultLiteral: return "SBSQL.TOKEN.DEFAULT_LITERAL";
    case TokenKind::kDocumentLiteral: return "SBSQL.TOKEN.DOCUMENT_LITERAL";
    case TokenKind::kVectorLiteral: return "SBSQL.TOKEN.VECTOR_LITERAL";
    case TokenKind::kRegexLiteral: return "SBSQL.TOKEN.REGEX_LITERAL";
    case TokenKind::kRangeLiteral: return "SBSQL.TOKEN.RANGE_LITERAL";
    default: return {};
  }
}

const LanguageTokenAlias* FindAlias(const std::vector<LanguageTokenAlias>& aliases,
                                    const Token& token) {
  for (const auto& alias : aliases) {
    if (alias.localized_text.empty()) continue;
    if (EqualsFoldAscii(alias.localized_text, token.raw_text.empty() ? token.text : token.raw_text)) {
      return &alias;
    }
  }
  return nullptr;
}

std::string CanonicalTextForToken(const Token& token,
                                  const LanguageTokenAlias* alias) {
  if (alias != nullptr && !alias->canonical_text.empty()) {
    return ToUpperAscii(alias->canonical_text);
  }
  if (token.kind == TokenKind::kStringLiteral && token.text.empty() &&
      !token.raw_text.empty()) {
    return token.raw_text;
  }
  if (!token.canonical_text.empty()) {
    return token.kind == TokenKind::kIdentifier ? token.canonical_text
                                                : ToUpperAscii(token.canonical_text);
  }
  return token.kind == TokenKind::kIdentifier ? token.text : ToUpperAscii(token.text);
}

std::string CanonicalTokenIdFor(const Token& token,
                                const LanguageTokenAlias* alias,
                                std::string_view canonical_text) {
  if (alias != nullptr && !alias->canonical_token_id.empty()) return alias->canonical_token_id;
  if (token.kind == TokenKind::kKeyword) return std::string("SBSQL.TOKEN.") + std::string(canonical_text);
  const auto literal = LiteralTokenId(token.kind);
  if (!literal.empty()) return literal;
  if (token.kind == TokenKind::kIdentifier) return "SBSQL.TOKEN.IDENTIFIER";
  if (token.kind == TokenKind::kOperator) return "SBSQL.TOKEN.OPERATOR";
  if (token.kind == TokenKind::kSymbol) return "SBSQL.TOKEN.SYMBOL";
  if (token.kind == TokenKind::kStatementTerminator) return "SBSQL.TOKEN.STATEMENT_TERMINATOR";
  if (token.kind == TokenKind::kParameter) return "SBSQL.TOKEN.PARAMETER";
  if (token.kind == TokenKind::kVariable) return "SBSQL.TOKEN.VARIABLE";
  return token.canonical_token_id.empty() ? "SBSQL.TOKEN.SOURCE" : token.canonical_token_id;
}

CanonicalElementKind ElementKindFor(const Token& token,
                                    std::string_view canonical_text,
                                    bool select_topology_active) {
  if (!select_topology_active &&
      token.kind != TokenKind::kOperator &&
      token.kind != TokenKind::kSymbol &&
      token.kind != TokenKind::kStatementTerminator &&
      LiteralTokenId(token.kind).empty()) {
    return CanonicalElementKind::kIdentifier;
  }
  if (canonical_text == "SELECT") return CanonicalElementKind::kCommand;
  if (canonical_text == "FROM" || canonical_text == "WHERE" || canonical_text == "GROUP" ||
      canonical_text == "HAVING" || canonical_text == "ORDER" || canonical_text == "LIMIT") {
    return CanonicalElementKind::kClause;
  }
  if (!LiteralTokenId(token.kind).empty()) return CanonicalElementKind::kLiteral;
  if (token.kind == TokenKind::kOperator) return CanonicalElementKind::kOperator;
  if (token.kind == TokenKind::kSymbol || token.kind == TokenKind::kStatementTerminator) {
    return CanonicalElementKind::kPunctuation;
  }
  return CanonicalElementKind::kIdentifier;
}

std::string SlotForCanonicalText(std::string_view canonical_text, std::string_view current_slot) {
  if (canonical_text == "SELECT") return "slot.projection";
  if (canonical_text == "FROM") return "slot.source";
  if (canonical_text == "WHERE") return "slot.condition";
  if (canonical_text == "GROUP") return "slot.grouping";
  if (canonical_text == "HAVING") return "slot.having";
  if (canonical_text == "ORDER") return "slot.ordering";
  if (canonical_text == "LIMIT" || canonical_text == "OFFSET") return "slot.paging";
  if (!current_slot.empty()) return std::string(current_slot);
  return "slot.statement";
}

std::string RoleForSlot(std::string_view slot) {
  if (slot == "slot.projection") return "projection";
  if (slot == "slot.source") return "source";
  if (slot == "slot.condition") return "condition";
  if (slot == "slot.grouping") return "grouping";
  if (slot == "slot.having") return "having";
  if (slot == "slot.ordering") return "ordering";
  if (slot == "slot.paging") return "paging";
  return "statement";
}

int SlotOrder(std::string_view slot) {
  if (slot == "slot.projection") return 10;
  if (slot == "slot.source") return 20;
  if (slot == "slot.condition") return 30;
  if (slot == "slot.grouping") return 40;
  if (slot == "slot.having") return 50;
  if (slot == "slot.ordering") return 60;
  if (slot == "slot.paging") return 70;
  return 100;
}

bool IsClauseOpener(std::string_view canonical_text) {
  return canonical_text == "SELECT" || canonical_text == "FROM" ||
         canonical_text == "WHERE" || canonical_text == "GROUP" ||
         canonical_text == "HAVING" || canonical_text == "ORDER" ||
         canonical_text == "LIMIT" || canonical_text == "OFFSET";
}

struct PendingElement {
  CanonicalElement element;
  int slot_order{100};
  int intra_slot_order{1};
  std::size_t source_index{0};
};

CanonicalElementStream BuildCanonicalElementStream(const std::vector<Token>& tokens,
                                                   std::string_view source,
                                                   const LanguageNormalizationOptions& options) {
  std::vector<PendingElement> pending;
  bool select_topology_active = false;
  for (const auto& token : tokens) {
    if (token.kind == TokenKind::kEnd) break;
    if (IsTriviaToken(token)) continue;
    const auto* alias = FindAlias(options.aliases, token);
    if (CanonicalTextForToken(token, alias) == "SELECT") {
      select_topology_active = true;
      break;
    }
  }
  std::string current_slot;
  for (std::size_t index = 0; index < tokens.size(); ++index) {
    const auto& token = tokens[index];
    if (token.kind == TokenKind::kEnd) break;
    if (IsTriviaToken(token)) continue;

    const auto* alias = FindAlias(options.aliases, token);
    const auto canonical_text = CanonicalTextForToken(token, alias);
    const auto slot = select_topology_active
                          ? SlotForCanonicalText(canonical_text, current_slot)
                          : std::string("slot.statement");
    if (select_topology_active && IsClauseOpener(canonical_text)) current_slot = slot;

    CanonicalElement element;
    element.kind = ElementKindFor(token, canonical_text, select_topology_active);
    element.canonical_text = canonical_text;
    element.canonical_id = CanonicalTokenIdFor(token, alias, canonical_text);
    element.surface_id = element.kind == CanonicalElementKind::kCommand ||
                                 element.kind == CanonicalElementKind::kClause
                             ? "SBSQL.SURFACE." + canonical_text
                             : "";
    element.slot_id = slot;
    element.alias_id = alias != nullptr ? alias->alias_id : token.canonical_alias_id;
    element.topology_role = RoleForSlot(slot);
    element.localized_text_hash =
        std::to_string(Fnv1a64(token.raw_text.empty() ? token.text : token.raw_text));
    element.source_span = CanonicalElementSourceSpan{token.offset, token.length};

    pending.push_back(PendingElement{std::move(element), SlotOrder(slot),
                                     select_topology_active && IsClauseOpener(canonical_text) ? 0 : 1,
                                     index});
  }

  std::stable_sort(pending.begin(), pending.end(),
                   [](const PendingElement& left, const PendingElement& right) {
                     if (left.slot_order != right.slot_order) return left.slot_order < right.slot_order;
                     if (left.intra_slot_order != right.intra_slot_order) {
                       return left.intra_slot_order < right.intra_slot_order;
                     }
                     return left.source_index < right.source_index;
                   });

  CanonicalElementStream stream;
  stream.resource_identity = options.resource_identity;
  stream.language_profile_uuid = options.language_profile_uuid;
  stream.exact_tag = options.exact_language_tag;
  stream.dialect_profile_uuid = options.dialect_profile_uuid;
  stream.topology_profile_uuid = options.topology_profile_uuid;
  stream.common_resource_hash = options.common_resource_hash;
  stream.source_hash = std::to_string(Fnv1a64(source));
  stream.normalized_before_uuid_resolution = true;
  stream.server_revalidation_required = true;
  for (auto& item : pending) {
    stream.elements.push_back(std::move(item.element));
  }
  return stream;
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
  return BuildCst(source, LanguageNormalizationOptions{});
}

CstDocument BuildCst(std::string_view source, const LanguageNormalizationOptions& options) {
  auto lexed = Lex(source);
  CstDocument document;
  document.source = std::string(source);
  document.source_hash = std::to_string(Fnv1a64(source));
  document.source_buffer_uuid = "cst.source_buffer." + document.source_hash;
  document.dialect_profile_uuid = options.dialect_profile_uuid;
  document.language_profile_uuid = options.language_profile_uuid;
  document.exact_language_tag = options.exact_language_tag;
  document.topology_profile_uuid = options.topology_profile_uuid;
  document.input_syntax_profile_uuid = options.input_syntax_profile_uuid;
  document.common_resource_hash = options.common_resource_hash;
  document.canonical_english_fallback_used = options.canonical_english_fallback_used;
  document.line_ending_mode = LineEndingMode(source);
  document.tokens = std::move(lexed.tokens);
  document.canonical_element_stream =
      BuildCanonicalElementStream(document.tokens, source, options);
  document.messages.diagnostics.insert(document.messages.diagnostics.end(),
                                       lexed.messages.diagnostics.begin(),
                                       lexed.messages.diagnostics.end());
  if (!document.canonical_element_stream.elements.empty()) {
    auto stream_validation = ValidateCanonicalElementStream(document.canonical_element_stream);
    if (!stream_validation.accepted) {
      for (const auto& issue : stream_validation.issues) {
        if (issue.severity != ResourceValidationSeverity::kError) continue;
        document.messages.diagnostics.push_back(MakeDiagnostic(
            issue.code, "ERROR", issue.detail, "sbp_sbsql.cst"));
      }
    }
  }

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
