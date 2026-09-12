// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "parser/cte_clause.hpp"
#include "ast/ast.hpp"

#include <unordered_set>
#include <utility>

namespace scratchbird::parser::sbsql {
std::optional<std::uint32_t> NativeCteProducerRoot(
    const NativeRelationalAstDocument& ast) {
  if (!ast.accepted() || ast.relations.size() < 2) return std::nullopt;
  const auto& cte = ast.relations.back();
  if (cte.relation_id != ast.root_relation_id ||
      cte.relation_kind != NativeRelationAstKind::kCte ||
      cte.input_relation_ids.size() != 1 ||
      cte.aggregate_grouping_form != NativeAggregateGroupingForm::kNone ||
      cte.aggregate_projection_form != NativeAggregateProjectionForm::kNone ||
      cte.join_kind != NativeJoinAstKind::kNone ||
      !cte.relation_source_ids.empty() || !cte.values_row_ids.empty() ||
      !cte.output_expression_ids.empty() || !cte.grouping_key_expression_ids.empty() ||
      !cte.aggregate_expression_ids.empty() || !cte.predicate_expression_ids.empty() ||
      !cte.limit_expression_ids.empty() || !cte.table_function_name.empty() ||
      !cte.table_function_argument_expression_ids.empty() ||
      !cte.window_invocation_ids.empty() || !cte.ordering_terms.empty()) return std::nullopt;
  std::unordered_set<std::uint32_t> ids;
  for (const auto& relation : ast.relations) {
    if (!relation.relation_id || !ids.insert(relation.relation_id).second ||
        relation.relation_id > cte.relation_id) return std::nullopt;
  }
  const auto child = cte.input_relation_ids.front();
  if (child == cte.relation_id || !ids.contains(child)) return std::nullopt;
  return child;
}

namespace {
class WithParser {
 public:
  WithParser(std::span<const Token* const> tokens, std::size_t maximum_depth)
      : tokens_(tokens), maximum_depth_(maximum_depth) {}

  WithClauseSyntax Parse() {
    if (!Word("WITH")) return result_;
    ++position_;
    result_.recursive_keyword = ConsumeWord("RECURSIVE");
    for (;;) {
      CteDefinitionSyntax definition;
      if (!Identifier(&definition.name)) return Fail("CTE name required");
      if (Symbol("(") && !IdentifierList(&definition.columns)) return result_;
      if (ConsumeWord("USING")) {
        if (!ConsumeWord("KEY") || !ConsumeSymbol("("))
          return Fail("USING KEY requires a parenthesized expression list");
        if (!ExpressionList(")", false, &definition.key_expressions)) return result_;
        ++position_;
      }
      if (!ConsumeWord("AS")) return Fail("CTE definition requires AS");
      if (ConsumeWord("NOT")) {
        if (!ConsumeWord("MATERIALIZED")) return Fail("NOT requires MATERIALIZED");
        definition.materialization = CteMaterialization::kNotMaterialized;
      } else if (ConsumeWord("MATERIALIZED")) {
        definition.materialization = CteMaterialization::kMaterialized;
      }
      if (!ConsumeSymbol("(")) return Fail("CTE query requires opening parenthesis");
      definition.body.begin = position_;
      if (!ScanTo(")", false)) return result_;
      definition.body.end = position_;
      if (definition.body.empty()) return Fail("CTE query cannot be empty");
      CollectUnions(&definition);
      ++position_;
      if (ConsumeWord("SEARCH")) {
        CteSearchSyntax search;
        if (ConsumeWord("DEPTH")) search.depth_first = true;
        else if (!ConsumeWord("BREADTH")) return Fail("SEARCH requires DEPTH or BREADTH");
        if (!ConsumeWord("FIRST") || !ConsumeWord("BY"))
          return Fail("SEARCH requires FIRST BY");
        if (!ExpressionList("SET", true, &search.ordering_terms)) return result_;
        ++position_;
        if (!Identifier(&search.sequence_name)) return Fail("SEARCH sequence name required");
        definition.search = std::move(search);
      }
      if (ConsumeWord("CYCLE")) {
        CteCycleSyntax cycle;
        for (;;) {
          std::size_t column = 0;
          if (!Identifier(&column)) return Fail("CYCLE column name required");
          cycle.columns.push_back(column);
          if (!ConsumeSymbol(",")) break;
        }
        if (!ConsumeWord("SET") || !Identifier(&cycle.mark_name))
          return Fail("CYCLE requires SET and a mark name");
        if (ConsumeWord("TO")) {
          CteTokenRange value{position_, 0};
          if (!ScanTo("DEFAULT", true)) return result_;
          value.end = position_;
          if (value.empty()) return Fail("CYCLE mark value required");
          cycle.mark_value = value;
          ++position_;
          value.begin = position_;
          if (!ScanTo("USING", true)) return result_;
          value.end = position_;
          if (value.empty()) return Fail("CYCLE default value required");
          cycle.default_value = value;
        }
        if (!ConsumeWord("USING") || !Identifier(&cycle.path_name))
          return Fail("CYCLE requires USING and a path name");
        definition.cycle = std::move(cycle);
      }
      result_.definitions.push_back(std::move(definition));
      if (!ConsumeSymbol(",")) break;
    }
    result_.query = {position_, tokens_.size()};
    if (result_.query.end > position_ &&
        tokens_.back()->kind == TokenKind::kStatementTerminator) --result_.query.end;
    if (result_.query.empty()) return Fail("WITH requires an outer statement");
    // Do not absorb a second statement or lose unmatched outer delimiters.
    std::vector<std::string_view> closers;
    for (; position_ < result_.query.end; ++position_) {
      if (!Balance(&closers)) return result_;
    }
    if (!closers.empty()) return Fail("outer statement delimiter is not closed");
    result_.status = WithClauseSyntaxStatus::kValid;
    return std::move(result_);
  }

 private:
  bool Word(std::string_view word) const {
    if (position_ == tokens_.size()) return false;
    const auto& token = *tokens_[position_];
    if (token.quoted || (token.kind != TokenKind::kKeyword &&
                         token.kind != TokenKind::kIdentifier &&
                         token.kind != TokenKind::kDefaultLiteral)) return false;
    return ToUpperAscii(token.canonical_text.empty() ? token.text : token.canonical_text) == word;
  }
  bool Symbol(std::string_view symbol) const {
    return position_ < tokens_.size() &&
           tokens_[position_]->kind == TokenKind::kSymbol &&
           tokens_[position_]->text == symbol;
  }
  bool ConsumeWord(std::string_view word) {
    if (!Word(word)) return false;
    ++position_;
    return true;
  }
  bool ConsumeSymbol(std::string_view symbol) {
    if (!Symbol(symbol)) return false;
    ++position_;
    return true;
  }
  bool Identifier(std::size_t* index) {
    // SBsql keywords are contextual. In a name slot, PATH, KEY and other
    // keyword-classified tokens retain identifier semantics.
    if (position_ == tokens_.size() ||
        (tokens_[position_]->kind != TokenKind::kIdentifier &&
         tokens_[position_]->kind != TokenKind::kKeyword)) return false;
    *index = position_++;
    return true;
  }
  WithClauseSyntax Fail(std::string detail, bool limit = false) {
    result_.status = limit ? WithClauseSyntaxStatus::kLimit : WithClauseSyntaxStatus::kMalformed;
    result_.error_token = position_;
    result_.detail = std::move(detail);
    result_.definitions.clear();
    result_.query = {};
    return result_;
  }
  bool IdentifierList(std::vector<std::size_t>* names) {
    ++position_;
    for (;;) {
      std::size_t name = 0;
      if (!Identifier(&name)) { Fail("CTE column name required"); return false; }
      names->push_back(name);
      if (!ConsumeSymbol(",")) break;
    }
    if (!ConsumeSymbol(")")) { Fail("CTE column list is not closed"); return false; }
    return true;
  }
  bool Balance(std::vector<std::string_view>* closers) {
    if (tokens_[position_]->kind == TokenKind::kStatementTerminator) {
      Fail("statement terminator inside WITH statement"); return false;
    }
    if (Symbol("(") || Symbol("[") || Symbol("{")) {
      if (closers->size() >= maximum_depth_) {
        Fail("WITH nesting budget exceeded", true); return false;
      }
      closers->push_back(Symbol("(") ? ")" : Symbol("[") ? "]" : "}");
    } else if (Symbol(")") || Symbol("]") || Symbol("}")) {
      if (closers->empty() || !Symbol(closers->back())) {
        Fail("mismatched WITH delimiter"); return false;
      }
      closers->pop_back();
    }
    return true;
  }
  bool ScanTo(std::string_view end, bool word, bool comma = false) {
    std::vector<std::string_view> closers;
    for (; position_ < tokens_.size(); ++position_) {
      if (closers.empty() && ((word ? Word(end) : Symbol(end)) ||
                             (comma && Symbol(",")))) return true;
      if (!Balance(&closers)) return false;
    }
    Fail("WITH clause delimiter or keyword is missing");
    return false;
  }
  bool ExpressionList(std::string_view end, bool word,
                      std::vector<CteTokenRange>* expressions) {
    for (;;) {
      CteTokenRange range{position_, 0};
      if (!ScanTo(end, word, true)) return false;
      range.end = position_;
      if (range.empty()) { Fail("WITH clause expression required"); return false; }
      expressions->push_back(range);
      if (!ConsumeSymbol(",")) return true;
    }
  }
  void CollectUnions(CteDefinitionSyntax* definition) {
    const auto saved = position_;
    std::size_t depth = 0;
    for (position_ = definition->body.begin; position_ < definition->body.end; ++position_) {
      if (Symbol("(") || Symbol("[") || Symbol("{")) ++depth;
      else if (Symbol(")") || Symbol("]") || Symbol("}")) --depth;
      else if (depth == 0 && Word("UNION")) {
        CteUnionSyntax entry{position_, position_ + 1, CteUnionDuplicates::kDistinct};
        ++position_;
        if (Word("ALL")) { entry.duplicates = CteUnionDuplicates::kAll; ++entry.right_begin; }
        else if (Word("DISTINCT")) ++entry.right_begin;
        --position_;
        definition->union_keywords.push_back(entry);
      }
    }
    position_ = saved;
  }
  std::span<const Token* const> tokens_;
  std::size_t maximum_depth_;
  std::size_t position_{0};
  WithClauseSyntax result_;
};
}  // namespace

WithClauseSyntax ParseWithClauseSyntax(std::span<const Token* const> tokens,
                                      std::size_t maximum_tokens,
                                      std::size_t maximum_depth) {
  if (tokens.size() > maximum_tokens || maximum_depth == 0) {
    WithClauseSyntax result;
    result.status = WithClauseSyntaxStatus::kLimit;
    result.detail = "WITH parser token or nesting budget exceeded";
    return result;
  }
  for (const auto* token : tokens) {
    if (!token || IsTriviaToken(*token) || token->kind == TokenKind::kEnd) {
      WithClauseSyntax result;
      result.status = WithClauseSyntaxStatus::kMalformed;
      result.detail = "WITH parser requires significant non-null tokens";
      return result;
    }
  }
  return WithParser(tokens, maximum_depth).Parse();
}
}  // namespace scratchbird::parser::sbsql
