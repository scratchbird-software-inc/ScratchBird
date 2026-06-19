// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ast/ast.hpp"

#include "meta/meta_command_surface.hpp"
#include "statement/statement_catalog.hpp"

#include <initializer_list>
#include <utility>
#include <vector>

namespace scratchbird::parser::sbsql {
namespace {

const Token* FirstMeaningfulToken(const CstDocument& cst) {
  for (const auto& token : cst.tokens) {
    if (token.kind != TokenKind::kEnd && !IsTriviaToken(token)) return &token;
  }
  return nullptr;
}

std::string CanonicalWordForToken(const Token& token) {
  if (!token.canonical_text.empty()) {
    return token.kind == TokenKind::kIdentifier ? token.canonical_text
                                                : ToUpperAscii(token.canonical_text);
  }
  return token.kind == TokenKind::kIdentifier ? token.text : ToUpperAscii(token.text);
}

std::string CanonicalWordForElement(const CanonicalElement& element) {
  if (!element.canonical_text.empty()) return ToUpperAscii(element.canonical_text);
  constexpr std::string_view prefix = "SBSQL.TOKEN.";
  if (element.canonical_id.rfind(prefix, 0) == 0) {
    return element.canonical_id.substr(prefix.size());
  }
  return ToUpperAscii(element.canonical_id);
}

std::size_t FirstMeaningfulTokenIndex(const CstDocument& cst) {
  for (std::size_t index = 0; index < cst.tokens.size(); ++index) {
    const auto& token = cst.tokens[index];
    if (token.kind != TokenKind::kEnd && !IsTriviaToken(token)) return index;
  }
  return cst.tokens.size();
}

std::size_t LastStatementTokenIndex(const CstDocument& cst, std::size_t begin) {
  std::size_t last = begin;
  for (std::size_t index = begin; index < cst.tokens.size(); ++index) {
    const auto& token = cst.tokens[index];
    if (token.kind == TokenKind::kEnd) break;
    if (IsTriviaToken(token)) continue;
    last = index;
    if (token.kind == TokenKind::kStatementTerminator) break;
  }
  return last;
}

bool ContainsTopLevelKeyword(const CstDocument& cst, std::string_view wanted) {
  const auto upper = ToUpperAscii(wanted);
  int depth = 0;
  std::string previous_word;
  std::string previous_previous_word;
  for (const auto& token : cst.tokens) {
    if (token.kind == TokenKind::kEnd || token.kind == TokenKind::kStatementTerminator) break;
    if (IsTriviaToken(token)) continue;
    if (token.text == "(") {
      ++depth;
      continue;
    }
    if (token.text == ")") {
      if (depth > 0) --depth;
      continue;
    }
    if (depth == 0 && (token.kind == TokenKind::kKeyword || token.kind == TokenKind::kIdentifier) &&
        CanonicalWordForToken(token) == upper) {
      if (upper == "FROM" && previous_word == "DISTINCT" &&
          previous_previous_word == "IS") {
        previous_previous_word = previous_word;
        previous_word = CanonicalWordForToken(token);
        continue;
      }
      return true;
    }
    if (token.kind == TokenKind::kKeyword || token.kind == TokenKind::kIdentifier) {
      previous_previous_word = previous_word;
      previous_word = CanonicalWordForToken(token);
    }
  }
  return false;
}

bool ContainsNestedKeyword(const CstDocument& cst, std::string_view wanted) {
  const auto upper = ToUpperAscii(wanted);
  int depth = 0;
  for (const auto& token : cst.tokens) {
    if (token.kind == TokenKind::kEnd || token.kind == TokenKind::kStatementTerminator) break;
    if (IsTriviaToken(token)) continue;
    if (token.text == "(") {
      ++depth;
      continue;
    }
    if (token.text == ")") {
      if (depth > 0) --depth;
      continue;
    }
    if (depth > 0 && (token.kind == TokenKind::kKeyword || token.kind == TokenKind::kIdentifier) &&
        CanonicalWordForToken(token) == upper) {
      return true;
    }
  }
  return false;
}

bool IsArraySelectPolicyRefusalProjection(const CstDocument& cst) {
  std::vector<std::string> words;
  for (const auto& token : cst.tokens) {
    if (token.kind == TokenKind::kEnd || token.kind == TokenKind::kStatementTerminator) break;
    if (IsTriviaToken(token)) continue;
    words.push_back(CanonicalWordForToken(token));
    if (words.size() >= 4) break;
  }
  return words.size() >= 4 &&
         words[0] == "SELECT" &&
         words[1] == "ARRAY" &&
         words[2] == "(" &&
         words[3] == "SELECT";
}

std::vector<std::string> AllMeaningfulTokenWords(const CstDocument& cst) {
  if (!cst.canonical_element_stream.elements.empty()) {
    std::vector<std::string> words;
    for (const auto& element : cst.canonical_element_stream.elements) {
      words.push_back(CanonicalWordForElement(element));
      if (words.back() == "STATEMENT_TERMINATOR") break;
    }
    return words;
  }
  std::vector<std::string> words;
  for (const auto& token : cst.tokens) {
    if (token.kind == TokenKind::kEnd) break;
    if (IsTriviaToken(token)) continue;
    words.push_back(CanonicalWordForToken(token));
    if (token.kind == TokenKind::kStatementTerminator) break;
  }
  return words;
}

std::vector<std::string> MeaningfulTokenWords(const CstDocument& cst) {
  if (!cst.canonical_element_stream.elements.empty()) {
    std::vector<std::string> words;
    for (const auto& element : cst.canonical_element_stream.elements) {
      words.push_back(CanonicalWordForElement(element));
      if (words.size() >= 4) break;
    }
    return words;
  }
  std::vector<std::string> words;
  for (const auto& token : cst.tokens) {
    if (token.kind == TokenKind::kEnd) break;
    if (IsTriviaToken(token)) continue;
    words.push_back(CanonicalWordForToken(token));
    if (words.size() >= 4) break;
  }
  return words;
}

std::vector<std::string> RawMeaningfulTokenWords(const CstDocument& cst) {
  std::vector<std::string> words;
  for (const auto& token : cst.tokens) {
    if (token.kind == TokenKind::kEnd) break;
    if (IsTriviaToken(token)) continue;
    words.push_back(CanonicalWordForToken(token));
    if (words.size() >= 4) break;
  }
  return words;
}

bool IsParenthesizedCallInvocation(const CstDocument& cst) {
  const auto words = AllMeaningfulTokenWords(cst);
  if (words.empty() || words.front() != "CALL") return false;
  for (std::size_t index = 1; index < words.size(); ++index) {
    if (words[index] == ";") break;
    if (words[index] == "(") return true;
  }
  return false;
}

bool IsBridgeStatementWords(const std::vector<std::string>& words) {
  if (words.empty()) return false;
  const auto word = [&](std::size_t index) -> std::string_view {
    return index < words.size() ? std::string_view(words[index]) : std::string_view();
  };
  if (word(0) == "BRIDGE") return true;
  if (word(0) == "SHOW" && word(1) == "BRIDGE") return true;
  if ((word(0) == "CREATE" || word(0) == "ALTER" || word(0) == "DROP" ||
       word(0) == "VALIDATE" || word(0) == "OPEN" || word(0) == "CLOSE" ||
       word(0) == "CONNECT" || word(0) == "DISCONNECT" ||
       word(0) == "ATTACH" || word(0) == "DETACH") &&
      word(1) == "BRIDGE") {
    return true;
  }
  return false;
}

bool IsShowCreateStatement(const CstDocument& cst) {
  const auto words = MeaningfulTokenWords(cst);
  return words.size() >= 2 && words[0] == "SHOW" && words[1] == "CREATE";
}

bool IsCreateSynonymStatement(const CstDocument& cst) {
  const auto words = AllMeaningfulTokenWords(cst);
  if (words.empty() || words.front() != "CREATE") return false;
  std::size_t index = 1;
  if (index + 1 < words.size() && words[index] == "OR" && words[index + 1] == "REPLACE") {
    index += 2;
  }
  if (index < words.size() && words[index] == "PUBLIC") ++index;
  return index < words.size() && words[index] == "SYNONYM";
}

bool IsCreateIndexStatement(const CstDocument& cst) {
  const auto words = AllMeaningfulTokenWords(cst);
  if (words.empty() || words.front() != "CREATE") return false;
  std::size_t index = 1;
  if (index < words.size() && words[index] == "UNIQUE") ++index;
  return index < words.size() && words[index] == "INDEX";
}

bool IsCreateStatisticsStatement(const CstDocument& cst) {
  const auto words = AllMeaningfulTokenWords(cst);
  if (words.empty() || words.front() != "CREATE") return false;
  std::size_t index = 1;
  if (index + 2 < words.size() &&
      words[index] == "IF" &&
      words[index + 1] == "NOT" &&
      words[index + 2] == "EXISTS") {
    index += 3;
  }
  return index < words.size() && words[index] == "STATISTICS";
}

bool IsRuntimeManagementStatement(const CstDocument& cst) {
  const auto words = AllMeaningfulTokenWords(cst);
  if (words.size() >= 2 && words[0] == "SHOW" &&
      (words[1] == "AGENT" || words[1] == "AGENTS")) {
    return words.size() < 3 || words[2] != "EXTENDED";
  }
  if (words.size() >= 2 && words[0] == "ALTER" && words[1] == "AGENT") {
    return true;
  }
  if (words.size() >= 2 && words[0] == "SHOW" &&
      (words[1] == "LISTENER" || words[1] == "LISTENERS" ||
       words[1] == "PARSER" || words[1] == "PARSERS")) {
    return true;
  }
  if (words.size() >= 2 && words[0] == "SHOW" && words[1] == "UDR") {
    return true;
  }
  if (words.size() >= 2 && words[0] == "REGISTER" && words[1] == "UDR") {
    return true;
  }
  return words.size() >= 3 && words[0] == "SHOW" &&
         words[1] == "PARSER" && words[2] == "PACKAGES";
}

bool IsPublicExactAccelerationStatement(const CstDocument& cst) {
  const auto words = AllMeaningfulTokenWords(cst);
  if (words.size() >= 2 && words[0] == "ALTER" && words[1] == "GPU") return true;
  if (words.size() >= 3 && words[0] == "ALTER" && words[1] == "NATIVE" &&
      words[2] == "COMPILE") {
    return true;
  }
  if (words.size() >= 3 && words[0] == "SHOW" && words[1] == "AOT" &&
      words[2] == "ARTIFACTS") {
    return true;
  }
  if (words.size() >= 2 && words[0] == "SHOW" &&
      (words[1] == "GPU" || words[1] == "LLVM")) {
    return true;
  }
  return false;
}

bool IsPublicExactManagementStatement(const CstDocument& cst) {
  const auto words = AllMeaningfulTokenWords(cst);
  if (!words.empty() && words[0] == "MEMORY") return true;
  if (words.size() >= 2 && words[0] == "ALTER" && words[1] == "MANAGEMENT") return true;
  if (words.size() >= 2 && words[0] == "CONFIG" && words[1] == "RELOAD") return true;
  if (words.size() >= 2 && words[0] == "SUPPORT" && words[1] == "BUNDLE") return true;
  return words.size() >= 3 && words[0] == "SHOW" && words[1] == "MANAGEMENT" &&
         (words[2] == "CONFIG" || words[2] == "DRIFT");
}

bool IsPublicExactMigrationStatement(const CstDocument& cst) {
  const auto words = AllMeaningfulTokenWords(cst);
  if (words.size() >= 3 && words[0] == "MIGRATE" && words[1] == "FROM" &&
      words[2] == "REFERENCE") {
    return true;
  }
  if (words.size() >= 2 && words[0] == "ALTER" && words[1] == "MIGRATION") {
    return true;
  }
  return words.size() >= 2 && words[0] == "SHOW" &&
         (words[1] == "MIGRATION" || words[1] == "MIGRATIONS");
}

bool IsPublicExactSecurityInspectStatement(const CstDocument& cst) {
  const auto words = AllMeaningfulTokenWords(cst);
  if (words.size() < 2 || words[0] != "SHOW") return false;
  if (words[1] == "AUDIT" || words[1] == "GRANTS" || words[1] == "GROUPS") return true;
  if (words.size() >= 3 && words[1] == "DISCOVERY" && words[2] == "RIGHTS") return true;
  return words.size() >= 3 && words[1] == "IDENTITY" && words[2] == "PROVIDERS";
}

bool IsPublicExactObservabilityInspectStatement(const CstDocument& cst) {
  const auto words = AllMeaningfulTokenWords(cst);
  if (words.size() < 2 || words[0] != "SHOW") return false;
  if (words[1] == "BUFFER" || words[1] == "CACHE" ||
      words[1] == "CAPABILITIES" || words[1] == "CONTEXT" ||
      words[1] == "DIALECT" || words[1] == "INDEX" ||
      words[1] == "IO" || words[1] == "JOB") {
    return true;
  }
  return false;
}

bool IsClusterProviderRouteStatement(const CstDocument& cst) {
  const auto words = AllMeaningfulTokenWords(cst);
  if (words.size() >= 2 && words[0] == "ENGINE" && words[1] == "CLUSTER") {
    return true;
  }
  if (words.size() >= 2 && words[0] == "ALTER" && words[1] == "CLUSTER") {
    return true;
  }
  return words.size() >= 3 && words[0] == "SHOW" && words[1] == "CLUSTER" &&
         words[2] != "PROVIDER";
}

bool WordsEqual(std::vector<std::string> words,
                std::initializer_list<std::string_view> expected) {
  if (!words.empty() && words.back() == ";") words.pop_back();
  if (words.size() != expected.size()) return false;
  std::size_t index = 0;
  for (const auto word : expected) {
    if (words[index] != word) return false;
    ++index;
  }
  return true;
}

enum class EngineApiCommandAstDomain {
  kNone,
  kAgentManagement,
  kArtifact,
  kDmlImport,
  kCatalog,
  kQueryRuntime,
  kTransactionControl,
  kSecurity,
  kManagement,
  kLifecycle,
  kParserPackage,
  kEventNotification,
  kClusterProvider,
};

EngineApiCommandAstDomain EngineApiCommandAstDomainFor(const CstDocument& cst) {
  const auto words = AllMeaningfulTokenWords(cst);
  if (WordsEqual(words, {"ENGINE", "AGENT", "REQUEST", "PAGE", "PREALLOCATION"}) ||
      WordsEqual(words, {"ENGINE", "AGENT", "REQUEST", "PAGE", "RELOCATION"}) ||
      WordsEqual(words, {"ENGINE", "AGENT", "REQUEST", "FILESPACE", "GROWTH"}) ||
      WordsEqual(words, {"ENGINE", "AGENT", "NOTIFY", "FILESPACE", "SHRINK", "READINESS"}) ||
      WordsEqual(words, {"ENGINE", "AGENT", "REQUEST", "INDEX", "DELTA", "MERGE"}) ||
      WordsEqual(words, {"ENGINE", "AGENT", "REQUEST", "INDEX", "REBUILD"})) {
    return EngineApiCommandAstDomain::kAgentManagement;
  }
  if (WordsEqual(words, {"EXPORT", "CATALOG", "ARTIFACT"}) ||
      WordsEqual(words, {"IMPORT", "CATALOG", "ARTIFACT"}) ||
      WordsEqual(words, {"EXPORT", "EXTERNAL", "GIT", "CATALOG", "SNAPSHOT"}) ||
      WordsEqual(words, {"DIFF", "EXTERNAL", "GIT", "CATALOG", "SNAPSHOT"}) ||
      WordsEqual(words, {"PLAN", "EXTERNAL", "GIT", "CATALOG", "ROLLBACK"})) {
    return EngineApiCommandAstDomain::kArtifact;
  }
  if (WordsEqual(words, {"ENGINE", "IMPORT", "ROWS", "EXECUTE"}) ||
      WordsEqual(words, {"ENGINE", "IMPORT", "CHECKPOINT", "MODEL", "NORMALIZE"}) ||
      WordsEqual(words, {"ENGINE", "IMPORT", "REJECT", "MODEL", "NORMALIZE"})) {
    return EngineApiCommandAstDomain::kDmlImport;
  }
  if (WordsEqual(words, {"ENGINE", "DDL", "CREATE", "DATABASE"}) ||
      WordsEqual(words, {"ENGINE", "CATALOG", "RESOLVE", "NAME"}) ||
      WordsEqual(words, {"ENGINE", "CATALOG", "MAP", "UUID", "TO", "NAME"}) ||
      WordsEqual(words, {"ENGINE", "CATALOG", "LOOKUP", "OBJECT"}) ||
      WordsEqual(words, {"ENGINE", "CATALOG", "LIST", "CHILDREN"}) ||
      WordsEqual(words, {"ENGINE", "CATALOG", "GET", "DEPENDENCIES"})) {
    return EngineApiCommandAstDomain::kCatalog;
  }
  if (WordsEqual(words, {"ENGINE", "QUERY", "BIND", "EXPRESSION"}) ||
      WordsEqual(words, {"ENGINE", "QUERY", "BIND", "PREDICATE"}) ||
      WordsEqual(words, {"ENGINE", "QUERY", "BIND", "PROJECTION"}) ||
      WordsEqual(words, {"ENGINE", "QUERY", "EXTRACT", "VALUE"}) ||
      WordsEqual(words, {"ENGINE", "QUERY", "SET", "OPERATION"}) ||
      WordsEqual(words, {"ENGINE", "QUERY", "APPLY", "NUMERIC", "OPERATION"}) ||
      WordsEqual(words, {"ENGINE", "QUERY", "CANONICALIZE", "DOCUMENT", "VALUE"}) ||
      WordsEqual(words, {"ENGINE", "QUERY", "EVALUATE", "ADVANCED", "DATATYPE", "FAMILY"}) ||
      WordsEqual(words, {"ENGINE", "QUERY", "VALIDATE", "DOMAIN", "VALUE"}) ||
      WordsEqual(words, {"ENGINE", "QUERY", "INVOKE", "DOMAIN", "METHOD"})) {
    return EngineApiCommandAstDomain::kQueryRuntime;
  }
  if (WordsEqual(words, {"PREPARE", "TRANSACTION"})) {
    return EngineApiCommandAstDomain::kTransactionControl;
  }
  if (WordsEqual(words, {"ENGINE", "SECURITY", "CREATE", "IDENTITY"}) ||
      WordsEqual(words, {"ENGINE", "SECURITY", "ALTER", "IDENTITY"}) ||
      WordsEqual(words, {"ENGINE", "SECURITY", "GRANT", "RIGHT"}) ||
      WordsEqual(words, {"ENGINE", "SECURITY", "REVOKE", "RIGHT"}) ||
      WordsEqual(words, {"ENGINE", "SECURITY", "EVALUATE", "VISIBILITY"}) ||
      WordsEqual(words, {"ENGINE", "SECURITY", "EVALUATE", "POLICY"}) ||
      WordsEqual(words, {"ENGINE", "SECURITY", "EVALUATE", "DEEP", "ENFORCEMENT"})) {
    return EngineApiCommandAstDomain::kSecurity;
  }
  if (WordsEqual(words, {"ENGINE", "MANAGEMENT", "INSPECT", "CONFIG"}) ||
      WordsEqual(words, {"ENGINE", "MANAGEMENT", "SET", "CONFIG"}) ||
      WordsEqual(words, {"ENGINE", "MANAGEMENT", "RESET", "CONFIG"}) ||
      WordsEqual(words, {"ENGINE", "MANAGEMENT", "PREPARE", "SUPPORT", "BUNDLE"})) {
    return EngineApiCommandAstDomain::kManagement;
  }
  if (WordsEqual(words, {"ALTER", "DATABASE", "ENTER", "RESTRICTED", "OPEN"}) ||
      WordsEqual(words, {"ALTER", "DATABASE", "EXIT", "RESTRICTED", "OPEN"})) {
    return EngineApiCommandAstDomain::kLifecycle;
  }
  if (WordsEqual(words, {"REGISTER", "PARSER", "PACKAGE"})) {
    return EngineApiCommandAstDomain::kParserPackage;
  }
  if (WordsEqual(words, {"UNLISTEN", "ALL", "NOTIFICATIONS"})) {
    return EngineApiCommandAstDomain::kEventNotification;
  }
  if (words.size() >= 2 && words[0] == "ENGINE" && words[1] == "CLUSTER") {
    return EngineApiCommandAstDomain::kClusterProvider;
  }
  return EngineApiCommandAstDomain::kNone;
}

bool IsDatabaseLifecycleStatement(const CstDocument& cst) {
  const auto words = AllMeaningfulTokenWords(cst);
  if (words.empty()) return false;
  if (words[0] == "ATTACH") {
    return words.size() < 2 || (words[1] != "POLICY" && words[1] != "FILESPACE");
  }
  if (words[0] == "USE") return true;
  if (words.size() >= 2 &&
      ((words[0] == "CREATE" && words[1] == "DATABASE") ||
       (words[0] == "OPEN" && words[1] == "DATABASE") ||
       (words[0] == "ATTACH" && words[1] == "DATABASE") ||
       (words[0] == "USE" && words[1] == "DATABASE") ||
       (words[0] == "DETACH" && words[1] == "DATABASE") ||
       (words[0] == "INSPECT" && words[1] == "DATABASE") ||
       (words[0] == "DIAGNOSE" && words[1] == "DATABASE") ||
       (words[0] == "VERIFY" && words[1] == "DATABASE") ||
       (words[0] == "REPAIR" && words[1] == "DATABASE") ||
       (words[0] == "SHUTDOWN" && words[1] == "DATABASE") ||
       (words[0] == "DROP" && words[1] == "DATABASE") ||
       (words[0] == "MAINTENANCE" && words[1] == "DATABASE"))) {
    return true;
  }
  if (words.size() >= 3 &&
      ((words[0] == "ENTER" && words[1] == "DATABASE" &&
        (words[2] == "MAINTENANCE" || words[2] == "RESTRICTED")) ||
       (words[0] == "EXIT" && words[1] == "DATABASE" &&
        (words[2] == "MAINTENANCE" || words[2] == "RESTRICTED")) ||
       (words[0] == "CLEAR" && words[1] == "DATABASE" &&
        (words[2] == "MAINTENANCE" || words[2] == "RESTRICTED")) ||
       (words[0] == "FORCE" && words[1] == "SHUTDOWN" &&
        words[2] == "DATABASE") ||
       (words[0] == "ACKNOWLEDGE" && words[1] == "SHUTDOWN" &&
        words[2] == "DATABASE") ||
       (words[0] == "SHUTDOWN" && words[1] == "ACKNOWLEDGE" &&
        words[2] == "DATABASE"))) {
    return true;
  }
  return words.size() >= 2 && words[0] == "ALTER" && words[1] == "DATABASE";
}

const StatementSurfaceDescriptor* DescriptorForStatementTokens(
    const CstDocument& cst, std::string_view first_keyword) {
  const auto words = MeaningfulTokenWords(cst);
  const auto keyword = ToUpperAscii(first_keyword);
  const auto second = words.size() > 1 ? std::string_view(words[1]) : std::string_view();
  const auto third = words.size() > 2 ? std::string_view(words[2]) : std::string_view();
  const auto fourth = words.size() > 3 ? std::string_view(words[3]) : std::string_view();
  const bool transaction_lock_surface =
      keyword == "LOCK" ||
      (keyword == "UNLOCK" && (second == "TABLE" || second == "NAMED"));
  std::string_view canonical_name;
  if (keyword == "JT" && second == "COLUMN") canonical_name = "jt_column";
  else if (keyword == "DICT" && second == "SOURCE") canonical_name = "dict_source";
  else if (keyword == "CONFORMANCE" && second == "TARGET") canonical_name = "conformance_target";
  else if (keyword == "PACKAGE" && second == "ITEM") canonical_name = "package_item";
  else if (keyword == "PERIOD" && second == "CLAUSE") canonical_name = "period_clause";
  else if (keyword == "HISTORICAL" && second == "READ" && third == "TARGET") canonical_name = "historical_read_target";
  else if (keyword == "MANAGEMENT" && second == "STMT") canonical_name = "management_stmt";
  else if (keyword == "HASH" && second == "FIELD" && third == "VALUE") canonical_name = "hash_field_value";
  else if (keyword == "CH" && second == "CODEC" && third == "SPEC") canonical_name = "ch_codec_spec";
  else if (keyword == "PUBLICATION" && second == "STMT") canonical_name = "publication_stmt";
  else if (keyword == "FOR" && second == "PORTION" && third == "OF" && fourth == "CLAUSE") canonical_name = "for_portion_of_clause";
  else if (keyword == "FK" && second == "ACTION" && third == "CLAUSE") canonical_name = "fk_action_clause";
  else if (keyword == "TAG" && second != "OPTIONS") canonical_name = "tag";
  else if (keyword == "COLLATE" && second == "POSTFIX") canonical_name = "collate_postfix";
  else if (keyword == "NEXT" && second == "VALUE" && third == "FORM") canonical_name = "next_value_form";
  else if (keyword == "TRIGGER" && second == "REFERENCING") canonical_name = "trigger_referencing";
  else if (keyword == "DICT" && second == "LAYOUT") canonical_name = "dict_layout";
  else if (keyword == "PROFILE" && second == "NAME") canonical_name = "profile_name";
  else if (keyword == "GENERIC" && second == "OPTIONS") canonical_name = "generic_options";
  else if (keyword == "MATCH" && second == "ARG") canonical_name = "match_arg";
  else if (keyword == "SERVICE" && second == "STMT") canonical_name = "service_stmt";
  else if (keyword == "TRANSFORM" && second == "OP") canonical_name = "transform_op";
  else if (keyword == "DDL" && second == "ROUTINE" && third == "STMT") canonical_name = "ddl_routine_stmt";
  else if (keyword == "TIMELINE" && second == "CLAUSE") canonical_name = "timeline_clause";
  else if (keyword == "CAPPED" && second == "CLAUSE") canonical_name = "capped_clause";
  else if (keyword == "HISTORICAL" && second == "TIME" && third == "SPEC") canonical_name = "historical_time_spec";
  else if (keyword == "LABEL" && second == "SET") canonical_name = "label_set";
  else if (keyword == "COLLATE" && second == "CLAUSE") canonical_name = "collate_clause";
  else if (keyword == "FIELD" && second == "DEF") canonical_name = "field_def";
  else if (keyword == "POSITION" && second == "REGEX" && third == "FORM") canonical_name = "position_regex_form";
  else if (keyword == "ANN" && second == "OPTIONS") canonical_name = "ann_options";
  else if (keyword == "ROW" && second == "SHAPE") canonical_name = "row_shape";
  else if (keyword == "CONTINUOUS" && second == "AGGREGATE" && third == "STMT") canonical_name = "continuous_aggregate_stmt";
  else if (keyword == "ENFORCED" && second == "MODIFIER") canonical_name = "enforced_modifier";
  else if (keyword == "MULTISET" && second == "SET" && third == "OP") canonical_name = "multiset_set_op";
  else if (keyword == "TRIGGER" && second == "EVENT" && third != "LIST") canonical_name = "trigger_event";
  else if (keyword == "BUCKET" && second == "ATTR") canonical_name = "bucket_attr";
  else if (keyword == "IDENTITY" && second == "OPTIONS") canonical_name = "identity_options";
  else if (keyword == "CORRELATION" && second == "NAME") canonical_name = "correlation_name";
  else if (keyword == "REFRESH" && second == "STRATEGY") canonical_name = "refresh_strategy";
  else if (keyword == "TRIGGER" && second == "TARGET") canonical_name = "trigger_target";
  else if (keyword == "PIPELINE" && second == "STAGE") canonical_name = "pipeline_stage";
  else if (keyword == "CONSISTENCY" && second == "LEVEL") canonical_name = "consistency_level";
  else if (keyword == "THROTTLE" && second == "ASSIGN") canonical_name = "throttle_assign";
  else if (keyword == "VAR" && second == "DEFAULT") canonical_name = "var_default";
  else if (keyword == "MONITOR" && second == "ACTION") canonical_name = "monitor_action";
  else if (keyword == "POSTFIX" && second == "UNARY" && third == "EXPR") canonical_name = "postfix_unary_expr";
  else if (keyword == "CH" && second == "SYSTEM" && third == "VERB") canonical_name = "ch_system_verb";
  else if (keyword == "DATA" && second == "CHANGE" && third == "STMT") canonical_name = "data_change_stmt";
  else if (keyword == "PSQL" && second == "IF" && third == "STMT") canonical_name = "psql_if_stmt";
  else if (keyword == "RERANKER" && second == "SPEC") canonical_name = "reranker_spec";
  else if (keyword == "LIMBO" && second == "STMT") canonical_name = "limbo_stmt";
  else if (keyword == "PRE" && second == "SPLIT" && third == "CLAUSE") canonical_name = "pre_split_clause";
  else if (keyword == "SALVAGE" && second == "OPTIONS") canonical_name = "salvage_options";
  else if (keyword == "REFERENCE" && second == "PROFILE" && third == "STMT") canonical_name = "reference_profile_stmt";
  else if (keyword == "RETURNING" && second == "CLAUSE") canonical_name = "returning_clause";
  else if (keyword == "HISTORICAL" && second == "READ" && third == "CLAUSE") canonical_name = "historical_read_clause";
  else if (keyword == "ARTIFACT" && second == "REF") canonical_name = "artifact_ref";
  else if (keyword == "FILL" && second == "STRATEGY") canonical_name = "fill_strategy";
  else if (keyword == "BRANCH" && second == "OPTIONS") canonical_name = "branch_options";
  else if (keyword == "HEX" && second == "DIGIT") canonical_name = "hex_digit";
  else if (keyword == "RESIGNAL") canonical_name = "resignal";
  else if (keyword == "FOR" && second == "CURSOR" && third == "FORM") canonical_name = "for_cursor_form";
  else if (keyword == "INTO" && second == "TARGET" && third != "LIST") canonical_name = "into_target";
  else if (keyword == "PK" && second == "COLUMN" && third == "SPEC") canonical_name = "pk_column_spec";
  else if (keyword == "CONSISTENCY" && second == "OPTIONS") canonical_name = "consistency_options";
  else if (keyword == "ROUTINE" && second == "NAME") canonical_name = "routine_name";
  else if (keyword == "CTE" && second == "CYCLE" && third == "CLAUSE") canonical_name = "cte_cycle_clause";
  else if (keyword == "PIT" && second == "STMT") canonical_name = "pit_stmt";
  else if (keyword == "FOR" && second == "PERIOD" && third == "CLAUSE") canonical_name = "for_period_clause";
  else if (keyword == "ARG" && second == "VALUE") canonical_name = "arg";
  else if (keyword == "EVICTION" && second == "CLAUSE") canonical_name = "eviction_clause";
  else if (keyword == "FK" && second == "ACTION" && third != "CLAUSE") canonical_name = "fk_action";
  else if (keyword == "PSQL" && second == "DML" && third == "STMT") canonical_name = "psql_dml_stmt";
  else if (keyword == "IDEMPOTENCY" && second == "LEVEL") canonical_name = "idempotency_level";
  else if (keyword == "TENANT" && second == "ACTION") canonical_name = "tenant_action";
  else if (keyword == "ENCRYPTION" && second == "CLAUSE") canonical_name = "encryption_clause";
  else if (keyword == "SIZE" && second == "SPEC") canonical_name = "size_spec";
  else if (keyword == "CAPPED" && second == "OPTION") canonical_name = "capped_option";
  else if (keyword == "OPERATOR" && second == "ATTR") canonical_name = "operator_attr";
  else if (keyword == "TCL" && second == "STMT") canonical_name = "tcl_stmt";
  else if (keyword == "TREAT" && second == "FORM") canonical_name = "treat_form";
  else if (keyword == "PLAN" && second == "OPTIONS") canonical_name = "plan_options";
  else if (keyword == "MULTISET" && second == "CONSTRUCTOR") canonical_name = "multiset_constructor";
  else if (keyword == "TRY" && second == "CAST" && third == "FORM") canonical_name = "try_cast_form";
  else if (keyword == "PARAM" && second == "LIST") canonical_name = "param_list";
  else if (keyword == "SET" && second == "ASSIGNMENT") canonical_name = "set_assignment";
  else if (keyword == "CALL" && second == "TARGET" && third != "LIST") canonical_name = "call_target";
  else if (keyword == "CALL" && second == "RESULT" && third == "CLAUSE") canonical_name = "call_result_clause";
  else if (keyword == "CONTAINER" && second == "PREDICATE") canonical_name = "container_predicate";
  else if (keyword == "SWEEP" && second == "ACTION") canonical_name = "sweep_action";
  else if (keyword == "PSQL" && second == "SUSPEND" && third == "STMT") canonical_name = "psql_suspend_stmt";
  else if (keyword == "SECRET" && second == "ATTR") canonical_name = "secret_attr";
  else if (keyword == "CTE" && second == "FUNCTION" && third == "DEF") canonical_name = "cte_function_def";
  else if (keyword == "PERIOD" && second == "NAME") canonical_name = "period_name";
  else if (keyword == "DDL" && second == "RELATIONAL" && third == "STMT") canonical_name = "ddl_relational_stmt";
  else if (keyword == "ID" && second == "START") canonical_name = "id_start";
  else if (keyword == "PIPELINE" && second == "PROCESSOR") canonical_name = "pipeline_processor";
  else if (keyword == "PREVIOUS" && second == "VALUE" && third == "FORM") canonical_name = "previous_value_form";
  else if (keyword == "VARIABLE" && second == "DECLARATION") canonical_name = "variable_declaration";
  else if (keyword == "REFERENCE" && second == "PROFILE" && third == "OPTIONS") canonical_name = "reference_profile_options";
  else if (keyword == "NEW" && second == "SURFACE" && third == "STMT") canonical_name = "new_surface_stmt";
  else if (keyword == "TOPOLOGY" && second == "ASSIGN") canonical_name = "topology_assign";
  else if (keyword == "FRAME" && second == "BOUND") canonical_name = "frame_bound";
  else if (keyword == "FDB" && second == "TX" && third == "OPTIONS") canonical_name = "fdb_tx_options";
  else if (keyword == "SERVER" && second == "ACTION") canonical_name = "server_action";
  else if (keyword == "PARTITION" && second == "SPEC" && third != "LIST") canonical_name = "partition_spec";
  else if (keyword == "SELECT" && second == "WITH" && third == "TIMELINE") canonical_name = "select_with_timeline";
  else if (keyword == "SELECT") canonical_name = "select";
  else if (keyword == "VALUES") canonical_name = "values_stmt";
  else if (keyword == "SEARCH") canonical_name = "vector_search_query";
  else if (keyword == "INSERT") canonical_name = "insert";
  else if (keyword == "UPDATE") canonical_name = "update";
  else if (keyword == "DELETE" && second == "STORAGE" && third == "FILESPACE") {
    canonical_name = "delete_storage_filespace_stmt";
  }
  else if (keyword == "DELETE") canonical_name = "delete";
  else if (keyword == "MERGE" && second == "FILESPACE") canonical_name = "merge_filespace_stmt";
  else if (keyword == "MERGE") canonical_name = "merge";
  else if (keyword == "REPAIR" && second == "FILESPACE") canonical_name = "repair_filespace_stmt";
  else if (keyword == "REBUILD" && second == "FILESPACE") canonical_name = "rebuild_filespace_stmt";
  else if (keyword == "SALVAGE" && second == "FILESPACE") canonical_name = "salvage_filespace_stmt";
  else if (keyword == "UPSERT") canonical_name = "upsert";
  else if ((keyword == "CREATE" && second == "PROTECTED" && third == "MATERIAL") ||
           (keyword == "ADD" && second == "PROTECTED" && third == "MATERIAL") ||
           (keyword == "ROTATE" && second == "PROTECTED" && third == "MATERIAL") ||
           (keyword == "RESOLVE" && second == "PROTECTED" && third == "MATERIAL") ||
           (keyword == "RELEASE" && second == "PROTECTED" && third == "MATERIAL") ||
           (keyword == "PURGE" && second == "PROTECTED" && third == "MATERIAL") ||
           (keyword == "EXPORT" && second == "PROTECTED" && third == "MATERIAL") ||
           (keyword == "IMPORT" && second == "PROTECTED" && third == "MATERIAL") ||
           (keyword == "SHOW" && second == "PROTECTED" && third == "MATERIAL" &&
            (fourth == "CATALOG" || fourth == "AUDIT"))) {
    canonical_name = "public_exact_protected_material";
  }
  else if (keyword == "CREATE" && second == "DATABASE") canonical_name = "create_database_stmt";
  else if (keyword == "CREATE" && second == "RESOURCE" && third == "GROUP") canonical_name = "create_resource_group_stmt";
  else if (keyword == "CREATE" && second == "JOB") canonical_name = "create_job_stmt";
  else if (keyword == "CREATE" && second == "SCHEDULE") canonical_name = "create_schedule_stmt";
  else if (keyword == "CREATE" && second == "EVENT") canonical_name = "create_event_stmt";
  else if (keyword == "CREATE" && second == "PRINCIPAL") canonical_name = "create_principal_stmt";
  else if (keyword == "CREATE" && second == "POLICY") canonical_name = "create_policy_stmt";
  else if (keyword == "CREATE" && second == "VECTOR" && third == "COLLECTION") {
    canonical_name = "create_vector_collection";
  } else if (keyword == "CREATE") canonical_name = "create_object";
  else if (keyword == "REINDEX" && second == "VECTOR" && third == "COLLECTION") {
    canonical_name = "vector_op_stmt";
  }
  else if (keyword == "ALTER" && second == "DATABASE") canonical_name = "alter_database_action";
  else if (keyword == "ALTER" && second == "JOB") canonical_name = "alter_job_stmt";
  else if (keyword == "ALTER" && second == "SCHEDULE") canonical_name = "alter_schedule_stmt";
  else if (keyword == "ALTER" && second == "PRINCIPAL") canonical_name = "alter_principal_stmt";
  else if (keyword == "ALTER" && second == "POLICY") canonical_name = "alter_policy_stmt";
  else if (keyword == "ATTACH" && second == "POLICY") canonical_name = "attach_policy_stmt";
  else if (keyword == "ATTACH") canonical_name = "attach_database_stmt";
  else if (keyword == "USE") canonical_name = "use_database_alias";
  else if (keyword == "MAINTENANCE") canonical_name = "maintenance_stmt";
  else if (keyword == "VERIFY") canonical_name = "verify_options";
  else if (keyword == "REPAIR") canonical_name = "repair_options";
  else if (keyword == "ALTER" && second == "AGENT") canonical_name = "agent_control_stmt";
  else if (keyword == "ALTER" && second == "GPU") canonical_name = "public_exact_alter_gpu";
  else if (keyword == "ALTER" && second == "NATIVE" && third == "COMPILE") canonical_name = "public_exact_alter_native_compile";
  else if (keyword == "ALTER" && second == "MANAGEMENT") canonical_name = "public_exact_alter_management";
  else if (keyword == "ALTER" && second == "FILESPACE" && third == "ENCRYPTION") {
    canonical_name = "public_exact_encryption_maintenance";
  }
  else if (keyword == "ALTER") canonical_name = "alter_object";
  else if (keyword == "CONFIG" && second == "RELOAD") canonical_name = "public_exact_config_reload";
  else if (keyword == "SUPPORT" && second == "BUNDLE") canonical_name = "public_exact_support_bundle_create";
  else if (keyword == "MEMORY") canonical_name = "public_exact_memory_management";
  else if (keyword == "DISCOVER" &&
           (second == "FILESPACE" || second == "ORPHAN" || second == "STALE")) {
    canonical_name = "public_exact_filespace_discovery";
  }
  else if ((keyword == "EXPORT" || keyword == "INSPECT" || keyword == "IMPORT" ||
            keyword == "ADMIT" || keyword == "REJECT") &&
           second == "FILESPACE" && third == "PACKAGE") {
    canonical_name = "public_exact_filespace_package";
  }
  else if (keyword == "SHARD" && second == "PLACEMENT") {
    canonical_name = "public_exact_shard_placement";
  }
  else if ((keyword == "ADMIT" && second == "ENCRYPTION" && third == "KEY") ||
           (keyword == "REKEY" && second == "FILESPACE") ||
           (keyword == "SHOW" && second == "PROTECTED" && third == "MATERIAL") ||
           (keyword == "PURGE" && second == "PROTECTED" && third == "MATERIAL") ||
           (keyword == "SHUTDOWN" && second == "PROTECTED" && third == "MATERIAL") ||
           (keyword == "OPEN" && second == "ENCRYPTED" && third == "FILESPACE") ||
           (keyword == "REQUEST" && second == "KEY" && third == "RELEASE") ||
           (keyword == "CRYPTOGRAPHIC" && second == "ERASE" && third == "FILESPACE")) {
    canonical_name = "public_exact_encryption_maintenance";
  }
  else if (keyword == "DROP" && second == "JOB") canonical_name = "drop_job_stmt";
  else if (keyword == "DROP") canonical_name = "drop_object";
  else if (keyword == "RENAME") canonical_name = "rename_object_stmt";
  else if (keyword == "COMMENT") canonical_name = "comment_on_stmt";
  else if (keyword == "SHOW" && second == "CREATE") canonical_name = "show_create";
  else if (keyword == "SHOW" && second == "TRANSACTION" && third == "RUNTIME") canonical_name = "show_transaction_runtime";
  else if (keyword == "SHOW" && second == "SECURITY" && third == "POLICY") {
    canonical_name = "show_security_policy";
  }
  else if (keyword == "SHOW" && second == "AGENTS" && third == "WHERE") canonical_name = "agent_filter";
  else if (keyword == "SHOW" && (second == "AGENT" || (second == "AGENTS" && third != "EXTENDED"))) {
    canonical_name = "agent_stmt";
  } else if (keyword == "SHOW" &&
             (second == "LISTENER" || second == "LISTENERS")) {
    canonical_name = "listener_stmt";
  } else if (keyword == "SHOW" && second == "PARSER" && third == "PACKAGES") {
    canonical_name = "parser_package_stmt";
  } else if (keyword == "SHOW" && (second == "PARSER" || second == "PARSERS")) {
    canonical_name = "parser_name";
  } else if (keyword == "SHOW" && second == "UDR" &&
             (third == "PACKAGE" || third == "PACKAGES")) {
    canonical_name = "udr_package_stmt";
  } else if (keyword == "SHOW" &&
             (second == "AOT" || second == "AUDIT" || second == "BUFFER" ||
              second == "CACHE" || second == "CAPABILITIES" || second == "CONTEXT" ||
              second == "DIALECT" || second == "DISCOVERY" || second == "GPU" ||
              second == "GRANTS" || second == "GROUPS" || second == "IDENTITY" ||
              second == "INDEX" || second == "IO" || second == "JOB" ||
              second == "LLVM" || second == "MANAGEMENT")) {
    canonical_name = "public_exact_show";
  } else if (keyword == "SHOW") canonical_name = "show";
  else if (keyword == "REGISTER" && second == "UDR") canonical_name = "udr_package_stmt";
  else if (keyword == "DESCRIBE") canonical_name = "describe";
  else if (keyword == "EXPLAIN") canonical_name = "explain";
  else if (keyword == "ARCHIVE") canonical_name = "archive_stmt";
  else if (keyword == "BACKUP") canonical_name = "backup_stmt";
  else if (keyword == "RESTORE") canonical_name = "restore_stmt";
  else if (keyword == "REPLICATE" || keyword == "REPLICATION") canonical_name = "replication_stmt";
  else if (keyword == "CHANGEFEED") canonical_name = "changefeed_stmt";
  else if (keyword == "EVENT" && second == "TRIGGER" && third == "FILTER") canonical_name = "event_trigger_filter";
  else if (keyword == "AGGREGATE" && second == "ATTR") canonical_name = "aggregate_attr";
  else if (keyword == "OP" && second == "CLASS" && third == "REF") canonical_name = "op_class_ref";
  else if (keyword == "CASSANDRA" && second == "USING") canonical_name = "cassandra_using_clause";
  else if (keyword == "LETTER") canonical_name = "letter";
  else if (keyword == "EXP" && second == "PART") canonical_name = "exp_part";
  else if (keyword == "PARTITION" && second == "SPEC" && third == "LIST") canonical_name = "partition_spec_list";
  else if (keyword == "PATTERN" && second == "NAME") canonical_name = "pattern_name";
  else if (keyword == "LABEL" && second == "REF") canonical_name = "label_ref";
  else if (keyword == "TRIGGER" && second == "TIMING") canonical_name = "trigger_timing";
  else if (keyword == "POSTFIX" && second == "OP") canonical_name = "postfix_op";
  else if (keyword == "FORK" && second == "STMT") canonical_name = "fork_stmt";
  else if (keyword == "ACTION" && second == "DESTINATION") canonical_name = "action_destination";
  else if (keyword == "LOG" && second == "MESSAGE") canonical_name = "log_message";
  else if (keyword == "OBJECT" && second == "PATH") canonical_name = "object_path";
  else if (keyword == "OBJECT" && second == "FILTER") canonical_name = "object_filter";
  else if (keyword == "SUBSTRING" && second == "REGEX" && third == "FORM") canonical_name = "substring_regex_form";
  else if (keyword == "TRIGGER" && second == "EVENT" && third == "LIST") canonical_name = "trigger_event_list";
  else if (keyword == "RANGE" && second == "SPEC") canonical_name = "range_spec";
  else if (keyword == "STRICT" && second == "TYPING" && third == "CLAUSE") canonical_name = "strict_typing_clause";
  else if (keyword == "PLAN" && second == "FORMAT") canonical_name = "plan_format";
  else if (keyword == "DICT" && second == "ATTRIBUTE") canonical_name = "dict_attribute";
  else if (keyword == "XT" && second == "COLUMN") canonical_name = "xt_column";
  else if (keyword == "REGEX" && second == "FLAGS") canonical_name = "regex_flags";
  else if (keyword == "EVENT" && second == "TRIGGER" && third == "EVENT") canonical_name = "event_trigger_event";
  else if (keyword == "CALL" && second == "ARG" && third != "LIST") canonical_name = "call_arg";
  else if (keyword == "CH" && second == "SYSTEM" && third == "TARGET") canonical_name = "ch_system_target";
  else if (keyword == "CH" && second == "COLUMN" && third == "SOURCE") canonical_name = "ch_column_source_mode";
  else if (keyword == "TZ" && second == "CLAUSE") canonical_name = "tz_clause";
  else if (keyword == "OPAQUE" && second == "OPTIONS") canonical_name = "opaque_options";
  else if (keyword == "UUID" && second == "TO" && third == "NAME") canonical_name = "uuid_to_name";
  else if (keyword == "MONITOR" && second == "TRIGGER") canonical_name = "monitor_trigger";
  else if (keyword == "LOCATOR" && second == "CLASS") canonical_name = "locator_class";
  else if (keyword == "COLON" && second == "LAMBDA") canonical_name = "colon_lambda";
  else if (keyword == "FOR" && second == "CLAUSE") canonical_name = "for_clause";
  else if (keyword == "PARTITION" && second == "VALUES") canonical_name = "partition_values";
  else if (keyword == "TRANSLATE" && second == "REGEX" && third == "FORM") canonical_name = "translate_regex_form";
  else if (keyword == "SET" && second == "CLAUSE" && third == "LIST") canonical_name = "set_clause_list";
  else if (keyword == "ID" && second == "CONTINUE") canonical_name = "id_continue";
  else if (keyword == "CONSISTENCY" && second == "CLAUSE") canonical_name = "consistency_clause";
  else if (keyword == "TAG" && second == "OPTIONS") canonical_name = "tag_options";
  else if (keyword == "INTERVAL" && second == "UNIT") canonical_name = "interval_unit";
  else if (keyword == "DIAG" && second == "FIELD") canonical_name = "diag_field";
  else if (keyword == "LANG" && second == "NAME") canonical_name = "lang_name";
  else if (keyword == "PATH" && second == "SEGMENT") canonical_name = "path_segment";
  else if (keyword == "EXCLUSION" && second == "ELEMENT") canonical_name = "exclusion_element";
  else if (keyword == "HINT" && second == "LIST") canonical_name = "hint_list";
  else if (keyword == "SUBPARTITION" && second == "SPEC" && third == "LIST") canonical_name = "subpartition_spec_list";
  else if (keyword == "SUBPARTITION" && second == "SPEC") canonical_name = "subpartition_spec";
  else if (keyword == "OS" && second == "FIELD" && third == "MAPPING") canonical_name = "os_field_mapping";
  else if (keyword == "REVISION" && second == "SPEC") canonical_name = "revision_spec";
  else if (keyword == "OCCURRENCES" && second == "REGEX" && third == "FORM") canonical_name = "occurrences_regex_form";
  else if (keyword == "OPERATOR" && second == "NAME") canonical_name = "operator_name";
  else if (keyword == "VALIDATOR" && second == "CLAUSE") canonical_name = "validator_clause";
  else if (keyword == "REFUSAL" && second == "STMT") canonical_name = "refusal_stmt";
  else if (keyword == "REFUSAL" && second == "DIAGNOSTIC") canonical_name = "refusal_diagnostic";
  else if (keyword == "REFUSAL" && second == "TARGET") canonical_name = "refusal_target";
  else if (keyword == "SUBJECT" && second == "CAPABILITY") canonical_name = "subject_capability";
  else if (keyword == "SUBJECT" && second == "AREA" && third == "OP") canonical_name = "subject_area_op_stmt";
  else if (keyword == "SUBJECT" && second == "AREA" && third == "NAME") canonical_name = "subject_area_name";
  else if (keyword == "EDITION" && second == "REFUSAL") canonical_name = "edition_refusal_stmt";
  else if (keyword == "CONFIG" && second == "STMT") canonical_name = "config_stmt";
  else if (keyword == "EXTENSION" && second == "ACTION") canonical_name = "extension_action";
  else if (keyword == "EXTENSION" && second == "ATTR") canonical_name = "extension_attr";
  else if (keyword == "EXTENSION" && second == "STMT") canonical_name = "extension_stmt";
  else if (keyword == "QUOTA" && second == "SUBJECT") canonical_name = "quota_subject";
  else if (keyword == "QUOTA" && second == "KEY") canonical_name = "quota_key";
  else if (keyword == "SETTINGS" && second == "CLAUSE") canonical_name = "settings_clause";
  else if (keyword == "VERIFICATION" && second == "CLAUSE") canonical_name = "verification_clause";
  else if (keyword == "PRAGMA" && second == "TARGET") canonical_name = "pragma_target";
  else if (keyword == "AUDIT" && second == "CLASS") canonical_name = "audit_class";
  else if (keyword == "AUDIT" && second == "TARGET") canonical_name = "audit_target";
  else if (keyword == "SESSION" && second == "SETTING" && third == "TARGET") canonical_name = "session_setting_target";
  else if (keyword == "SESSION" && second == "SETTING" && third == "STMT") canonical_name = "session_setting_stmt";
  else if (keyword == "SESSION" && second == "ID") canonical_name = "session_id";
  else if (keyword == "EVENT" && second == "TRIGGER" && third == "AUDIT") canonical_name = "event_trigger_audit_clause";
  else if (keyword == "EVENT" && second == "AUDIT") canonical_name = "event_audit_clause";
  else if (keyword == "MASKING" && second == "OPTIONS") canonical_name = "masking_options";
  else if (keyword == "DISCONNECT" && second == "SESSION") canonical_name = "disconnect_session";
  else if (keyword == "CONNECT" && second == "SESSION") canonical_name = "connect_session";
  else if (keyword == "SET" && second == "SESSION") canonical_name = "set_session";
  else if (keyword == "SET" && second == "CONSTRAINTS") canonical_name = "set_constraints_stmt";
  else if (keyword == "SNAPSHOT" && second == "STMT") canonical_name = "snapshot_stmt";
  else if (keyword == "SNAPSHOT" && second == "NAME") canonical_name = "snapshot_name";
  else if (keyword == "ATOMICITY" && second == "MODIFIER") canonical_name = "atomicity_modifier";
  else if (keyword == "ISO" && second == "LEVEL") canonical_name = "iso_level";
  else if (keyword == "NAMED" && second == "SNAPSHOT") canonical_name = "named_snapshot_stmt";
  else if (keyword == "COLOCATION" && second == "CLAUSE") canonical_name = "colocation_clause";
  else if (keyword == "LOCALITY" && second == "CLAUSE") canonical_name = "locality_clause";
  else if (keyword == "LOCALITY" && second == "SPEC") canonical_name = "locality_spec";
  else if (keyword == "ZONE" && second == "TARGET") canonical_name = "zone_target";
  else if (keyword == "ZONE" && second == "SETTING" && third == "ASSIGN") canonical_name = "zone_setting_assign";
  else if (keyword == "IGNITE" && second == "ZONE") canonical_name = "ignite_zone_clause";
  else if (keyword == "PIPELINE" && second == "CLAUSE") canonical_name = "pipeline_clause";
  else if (keyword == "ACCELERATION" && second == "STMT") canonical_name = "acceleration_stmt";
  else if (keyword == "BUFFER" && second == "ACTION") canonical_name = "buffer_action";
  else if (keyword == "KERNEL" && second == "NAME") canonical_name = "kernel_name";
  else if (keyword == "SWEEP" && second == "CONTROL") canonical_name = "sweep_control_stmt";
  else if (keyword == "ENGINE" && second == "NAME") canonical_name = "engine_name";
  else if (keyword == "ENGINE" && second == "CLAUSE") canonical_name = "engine_clause";
  else if (keyword == "COMPILE" && second == "OPTIONS") canonical_name = "compile_options";
  else if (keyword == "OPTIMIZATION" && second == "LEVEL") canonical_name = "optimization_level";
  else if (keyword == "COMPRESSION" && second == "SPEC") canonical_name = "compression_spec";
  else if (keyword == "MONITOR" && second == "INPUT") canonical_name = "monitor_input";
  else if (keyword == "EXPIRY" && second == "CLAUSE") canonical_name = "expiry_clause";
  else if (keyword == "BOOL" && second == "CLAUSE") canonical_name = "bool_clause";
  else if (keyword == "SQL" && second == "TEXT") canonical_name = "sql_text";
  else if (keyword == "REBUILD" && second == "OPTIONS") canonical_name = "rebuild_options";
  else if (keyword == "MATERIALIZATION" && second == "HINT") canonical_name = "materialization_hint";
  else if (keyword == "RESOLVE" && second == "NAME" && third == "PUBLIC") canonical_name = "resolve_name_public";
  else if (keyword == "RENDER" && second == "CONTRACT" && third == "NAME") canonical_name = "render_contract_name";
  else if (keyword == "CHARSET" && second == "CLAUSE") canonical_name = "charset_clause";
  else if (keyword == "DIAG" && second == "ASSIGNMENT") canonical_name = "diag_assignment";
  else if (keyword == "LABEL" && second == "DECL") canonical_name = "label_decl";
  else if (keyword == "IN" && second == "TARGET") canonical_name = "in_target";
  else if (keyword == "RANGE" && second == "OPTIONS") canonical_name = "range_options";
  else if (keyword == "DICT" && second == "LIFETIME") canonical_name = "dict_lifetime";
  else if (keyword == "RANGE" && second == "OP") canonical_name = "range_op";
  else if (keyword == "HETEROGENEOUS" && second == "FORM") canonical_name = "heterogeneous_form";
  else if (keyword == "TTL" && second == "SET" && third == "ASSIGN") canonical_name = "ttl_set_assign";
  else if (keyword == "PARTITION" && second == "METHOD") canonical_name = "partition_method";
  else if (keyword == "HINT" && second == "DIRECTIVE") canonical_name = "hint_directive";
  else if (keyword == "FLASHBACK" && second == "STMT") canonical_name = "flashback_stmt";
  else if (keyword == "DDL" && second == "MULTIMODEL" && third == "STMT") canonical_name = "ddl_multimodel_stmt";
  else if (keyword == "QUALIFY" && second == "CLAUSE") canonical_name = "qualify_clause";
  else if (keyword == "SUPPORT" && second == "STMT") canonical_name = "support_stmt";
  else if (keyword == "DIGIT") canonical_name = "digit";
  else if (keyword == "VACUUM" && second == "OPTION") canonical_name = "vacuum_option";
  else if (keyword == "ENCRYPTION" && second == "OPTIONS") canonical_name = "encryption_options";
  else if (keyword == "PERIOD" && second == "LOOKUP") canonical_name = "period_lookup";
  else if (keyword == "DIALECT" && second == "NAME") canonical_name = "dialect_name";
  else if (keyword == "COPROCESSOR" && second == "HINT") canonical_name = "coprocessor_hint";
  else if (keyword == "CA" && second == "NAME") canonical_name = "ca_name";
  else if (keyword == "UUID" && second == "REFERENCE") canonical_name = "uuid_reference";
  else if (keyword == "COMPRESSION" && second == "CLAUSE") canonical_name = "compression_clause";
  else if (keyword == "SET" && second == "QUANTIFIER") canonical_name = "set_quantifier";
  else if (keyword == "EDITION" && second == "SCOPE") canonical_name = "edition_scope";
  else if (keyword == "SWEEP" && second == "PRIORITY") canonical_name = "sweep_priority";
  else if (keyword == "OS" && second == "ANALYZER" && third == "DEF") canonical_name = "os_analyzer_def";
  else if (keyword == "TTL" && second == "ACTION") canonical_name = "ttl_action";
  else if (keyword == "COMPACT" && second == "OPTIONS") canonical_name = "compact_options";
  else if (keyword == "MOVE" && second == "OPTIONS") canonical_name = "move_options";
  else if (keyword == "IMPORT" && second == "OPTIONS") canonical_name = "import_options";
  else if (keyword == "FILTER" && second == "CLAUSE") canonical_name = "filter_clause";
  else if (keyword == "GENERATION" && second == "CLAUSE") canonical_name = "generation_clause";
  else if (keyword == "ANALYZE" && second == "OPTION") canonical_name = "analyze_option";
  else if (keyword == "BUFFER" && second == "POOL" && third == "STMT") canonical_name = "buffer_pool_stmt";
  else if (keyword == "CLEANUP" && second == "CONTROL" && third == "STMT") canonical_name = "cleanup_control_stmt";
  else if (keyword == "CLEANUP" && second == "ACTION") canonical_name = "cleanup_action";
  else if (keyword == "EVENT" && second == "BODY") canonical_name = "event_body";
  else if (keyword == "BRANCH" && second == "OP" && third == "STMT") canonical_name = "branch_op_stmt";
  else if (keyword == "ADMISSION" && second == "STMT") canonical_name = "admission_stmt";
  else if (keyword == "OPCLASS" && second == "ITEM") canonical_name = "opclass_item";
  else if (keyword == "ENUM" && second == "VALUE" && third == "ASSIGN") canonical_name = "enum_value_assign";
  else if (keyword == "CYPHER" && second == "NODE" && third == "PATTERN") canonical_name = "cypher_node_pattern";
  else if (keyword == "CYPHER" && second == "RETURN" && third == "LIST") canonical_name = "cypher_return_list";
  else if (keyword == "CYPHER" && second == "RETURN" && third == "ITEM") canonical_name = "cypher_return_item";
  else if (keyword == "CYPHER" && second == "WITH" && third == "PIPELINE") canonical_name = "cypher_with_pipeline_clause";
  else if (keyword == "CYPHER" && second == "CONSTRAINT" && third == "PREDICATE") canonical_name = "cypher_constraint_predicate";
  else if (keyword == "CYPHER" && second == "CONSTRAINT" && third == "TARGET") canonical_name = "cypher_constraint_target";
  else if (keyword == "CYPHER" && second == "RELATIONSHIP" && third == "PATTERN") canonical_name = "cypher_relationship_pattern";
  else if (keyword == "CYPHER" && second == "REMOVE" && third == "CLAUSE") canonical_name = "cypher_remove_clause";
  else if (keyword == "CYPHER" && second == "REMOVE" && third == "ITEM") canonical_name = "cypher_remove_item";
  else if (keyword == "CYPHER" && second == "STMT") canonical_name = "cypher_stmt";
  else if (keyword == "CYPHER" && second == "CALL" && third == "CLAUSE") canonical_name = "cypher_call_clause";
  else if (keyword == "CYPHER" && second == "SET" && third == "ITEM") canonical_name = "cypher_set_item";
  else if (keyword == "CYPHER" && second == "SET" && third == "CLAUSE") canonical_name = "cypher_set_clause";
  else if (keyword == "CYPHER" && second == "RANGE" && third == "SPEC") canonical_name = "cypher_range_spec";
  else if (keyword == "CYPHER" && second == "FOREACH" && third == "CLAUSE") canonical_name = "cypher_foreach_clause";
  else if (keyword == "CYPHER" && second == "OPTIONAL" && third == "MATCH") canonical_name = "cypher_optional_match";
  else if (keyword == "CYPHER" && second == "CLAUSE") canonical_name = "cypher_clause";
  else if (keyword == "CYPHER" && second == "LABEL" && third == "LIST") canonical_name = "cypher_label_list";
  else if (keyword == "CYPHER" && second == "PATTERN") canonical_name = "cypher_pattern";
  else if (keyword == "CYPHER" && second == "PROPERTY" && third == "MATCH") canonical_name = "cypher_property_match";
  else if (keyword == "CYPHER" && second == "USE" && third == "CLAUSE") canonical_name = "cypher_use_clause";
  else if (keyword == "CYPHER" && second == "REL" && third == "DETAIL") canonical_name = "cypher_rel_detail";
  else if (keyword == "CYPHER" && second == "YIELD" && third == "ITEM") canonical_name = "cypher_yield_item";
  else if (keyword == "CYPHER" && second == "UNWIND" && third == "CLAUSE") canonical_name = "cypher_unwind_clause";
  else if (keyword == "CYPHER" && second == "MATCH" && third == "CLAUSE") canonical_name = "cypher_match_clause";
  else if (keyword == "CYPHER" && second == "CREATE") canonical_name = "cypher_create_clause";
  else if (keyword == "CYPHER" && second == "DELETE") canonical_name = "cypher_delete_clause";
  else if (keyword == "CYPHER" && second == "MERGE") canonical_name = "cypher_merge_clause";
  else if (keyword == "CYPHER" && second == "LOAD") canonical_name = "cypher_load_csv";
  else if (keyword == "CYPHER" && second == "WITH") canonical_name = "cypher_with_clause";
  else if (keyword == "CYPHER" && second == "WHERE") canonical_name = "cypher_where_clause";
  else if (keyword == "CYPHER" && second == "CALL") canonical_name = "cypher_call_subquery";
  else if (keyword == "CYPHER" && second == "SUBQUERY") canonical_name = "cypher_subquery_expr";
  else if (keyword == "GRAPH" && second == "SUBQUERY") canonical_name = "graph_subquery_stmt";
  else if (keyword == "GRAPH" && second == "DELETE" && third == "NODE") canonical_name = "graph_delete_node_stmt";
  else if (keyword == "GRAPH" && second == "DELETE" && third == "EDGE") canonical_name = "graph_delete_edge_stmt";
  else if (keyword == "GPU" && second == "WORKLOAD") canonical_name = "gpu_workload_action";
  else if (keyword == "GPU" && second == "CAPABILITY" && third == "NAME") canonical_name = "gpu_capability_name";
  else if (keyword == "GPU" && second == "CAPABILITY" && third == "OPTIONS") canonical_name = "gpu_capability_options";
  else if (keyword == "GPU") canonical_name = "gpu_stmt";
  else if (keyword == "LLVM") canonical_name = "llvm_stmt";
  else if (keyword == "LOAD" && second == "DATA") canonical_name = "load_data_clause";
  else if (keyword == "REFRESH" && second == "MATERIALIZED" && third == "VIEW") {
    canonical_name = "refresh_materialized_view_stmt";
  }
  else if (keyword == "GRAPH") canonical_name = "graph_op_stmt";
  else if (keyword == "KEYVALUE") canonical_name = "keyvalue_op_stmt";
  else if (keyword == "DOCUMENT") canonical_name = "document_op_stmt";
  else if (keyword == "DOC" && second == "WRITE" && third == "OPTIONS") canonical_name = "doc_write_options";
  else if (keyword == "DOC" && second == "PIPELINE" && third == "OPTIONS") canonical_name = "doc_pipeline_options";
  else if (keyword == "DOC" && second == "PIPELINE" && third == "STAGE") canonical_name = "doc_pipeline_stage";
  else if (keyword == "DOC" && second == "PIPELINE" && third == "STMT") canonical_name = "doc_pipeline_stmt";
  else if (keyword == "DOC" && second == "PIPELINE" && third == "VERB") canonical_name = "doc_pipeline_verb";
  else if (keyword == "DOC" && second == "CRUD") canonical_name = "doc_crud_stmt";
  else if (keyword == "DOC" && second == "PATH" && third == "MUTATION") canonical_name = "doc_path_mutation";
  else if (keyword == "DOC" && second == "READ" && third == "OPTIONS") canonical_name = "doc_read_options";
  else if (keyword == "DOC" && second == "VERIFIABLE" && third == "STMT") canonical_name = "doc_verifiable_stmt";
  else if (keyword == "KV") canonical_name = "kv_string_op";
  else if (keyword == "TIMESERIES" || (keyword == "TIME" && second == "SERIES")) canonical_name = "time_series_op_stmt";
  else if (keyword == "FULLTEXT" && second == "FILTER") canonical_name = "fulltext_filter";
  else if (keyword == "FULLTEXT" && second == "AGGS") canonical_name = "fulltext_aggs";
  else if (keyword == "FULLTEXT" && second == "AGG" && third == "DEF") canonical_name = "fulltext_agg_def";
  else if (keyword == "FULLTEXT" && second == "PAGING") canonical_name = "fulltext_paging";
  else if (keyword == "FULLTEXT") canonical_name = "fulltext_search_body";
  else if (keyword == "QUERY" && second == "CH_JOIN_STRICTNESS") canonical_name = "ch_join_strictness";
  else if (keyword == "QUERY" && second == "FULLTEXT") canonical_name = "fulltext_search_query";
  else if (keyword == "QUERY" && second == "PREWHERE") canonical_name = "prewhere_clause";
  else if (keyword == "QUERY" && second == "GROUPING" && third == "SET") canonical_name = "grouping_set";
  else if (keyword == "QUERY" && second == "GROUPING" && third == "FORM") canonical_name = "grouping_form";
  else if (keyword == "QUERY" && second == "QUANTIFIED") canonical_name = "quantified_subquery";
  else if (keyword == "STREAM" && second == "CONSUMER") canonical_name = "stream_consumer_group_stmt";
  else if (keyword == "RESOURCE" && second == "QUOTA") canonical_name = "quota_limit";
  else if (keyword == "TABLEGROUP") canonical_name = "tablegroup_name";
  else if (keyword == "CREATE" && second == "RESOURCE" && third == "GROUP") canonical_name = "create_resource_group_stmt";
  else if (keyword == "SELECT" && second == "WITH" && third == "TIMELINE") canonical_name = "select_with_timeline";
  else if (keyword == "FOR" && second == "SELECT") canonical_name = "for_select_form";
  else if (keyword == "FOR" && second == "RANGE") canonical_name = "for_range_form";
  else if (keyword == "FORALL" && second == "DML") canonical_name = "forall_dml_or_execute";
  else if (keyword == "FORALL" && second == "RANGE") canonical_name = "forall_range";
  else if (keyword == "PSQL" && second == "SELECT") canonical_name = "psql_select_into";
  else if (keyword == "PSQL" && second == "STATEMENT") canonical_name = "psql_statement";
  else if (keyword == "PSQL" && second == "EXECUTE" && third == "STATEMENT") canonical_name = "psql_execute_statement";
  else if (keyword == "PSQL" && second == "EXECUTE" && third == "BLOCK") canonical_name = "psql_execute_block";
  else if (keyword == "PSQL" && second == "REPEAT") canonical_name = "psql_repeat_stmt";
  else if (keyword == "PSQL" && second == "FORALL") canonical_name = "psql_forall_stmt";
  else if (keyword == "PSQL" && second == "FOR") canonical_name = "psql_for_stmt";
  else if (keyword == "PSQL" && second == "CALL") canonical_name = "psql_call_stmt";
  else if (keyword == "PSQL" && second == "COMPOUND" && third == "STMT") canonical_name = "psql_compound_stmt";
  else if (keyword == "PSQL" && second == "PIPE" && third == "ROW" && fourth == "STMT") canonical_name = "psql_pipe_row_stmt";
  else if (keyword == "PSQL" && second == "EXIT" && third == "STMT") canonical_name = "psql_exit_stmt";
  else if (keyword == "PSQL" && second == "CONTINUE" && third == "STMT") canonical_name = "psql_continue_stmt";
  else if (keyword == "PSQL" && second == "LEAVE") canonical_name = "psql_leave_stmt";
  else if (keyword == "PSQL" && second == "OPEN" && third == "CHANNEL") canonical_name = "psql_open_channel_stmt";
  else if (keyword == "PSQL" && second == "NULL") canonical_name = "psql_null_stmt";
  else if (keyword == "PSQL" && second == "RESIGNAL") canonical_name = "psql_resignal_stmt";
  else if (keyword == "PSQL" && second == "DECLARE") canonical_name = "psql_declare_section";
  else if (keyword == "PSQL" && second == "WHILE") canonical_name = "psql_while_stmt";
  else if (keyword == "PSQL" && second == "EMIT") canonical_name = "psql_emit_channel_stmt";
  else if (keyword == "PSQL" && second == "ASSIGN") canonical_name = "psql_assignment";
  else if (keyword == "PSQL" && second == "GET") canonical_name = "psql_get_diagnostics";
  else if (keyword == "PSQL" && second == "RETURN") canonical_name = "psql_return_stmt";
  else if (keyword == "PSQL" && second == "SIGNAL") canonical_name = "psql_signal_stmt";
  else if (keyword == "PSQL" && second == "LOOP") canonical_name = "psql_loop_stmt";
  else if (keyword == "PSQL" && second == "RAISE") canonical_name = "psql_raise_stmt";
  else if (keyword == "SHOW" && second == "TRANSACTION" && third == "RUNTIME") canonical_name = "show_transaction_runtime";
  else if (keyword == "AUTONOMOUS") canonical_name = "psql_autonomous_block";
  else if (keyword == "LOCK" && second == "MODE") canonical_name = "lock_mode";
  else if (keyword == "TRANSACTION" && second == "REF") canonical_name = "transaction_ref";
  else if (keyword == "PACKAGE" && second == "INIT") canonical_name = "package_init_block";
  else if (keyword == "PACKAGE" && second == "BODY") canonical_name = "package_body_item";
  else if (keyword == "PACKAGE" && second == "NAME") canonical_name = "package_name";
  else if (keyword == "EXECUTE" && second == "BLOCK") canonical_name = "execute_block";
  else if (keyword == "STATISTICS" && second == "KIND") canonical_name = "statistics_kind";
  else if (keyword == "STATEMENT" && second == "EXTENSION") canonical_name = "statement_extension";
  else if (keyword == "STATEMENT") canonical_name = "statement";
  else if (keyword == "TRUNCATE" && second == "STATEMENT") canonical_name = "truncate_statement";
  else if (keyword == "GROW" && second == "FILESPACE") canonical_name = "grow_filespace_stmt";
  else if (keyword == "RESIZE" && second == "FILESPACE") canonical_name = "resize_filespace_stmt";
  else if (keyword == "SHRINK" && second == "FILESPACE") canonical_name = "shrink_filespace_stmt";
  else if (keyword == "MOVE" && second == "FILESPACE") canonical_name = "move_filespace_stmt";
  else if (keyword == "MERGE" && second == "FILESPACE") canonical_name = "merge_filespace_stmt";
  else if (keyword == "VERIFY" && second == "FILESPACE") canonical_name = "verify_filespace_stmt";
  else if (keyword == "COMPACT" && second == "FILESPACE") canonical_name = "compact_filespace_stmt";
  else if (keyword == "FENCE" && second == "FILESPACE") canonical_name = "fence_filespace_stmt";
  else if (keyword == "RELEASE" && second == "FILESPACE") canonical_name = "release_filespace_stmt";
  else if (keyword == "ARCHIVE" && second == "FILESPACE") canonical_name = "archive_filespace_stmt";
  else if (keyword == "QUARANTINE" && second == "FILESPACE") canonical_name = "quarantine_filespace_stmt";
  else if (keyword == "REPAIR" && second == "FILESPACE") canonical_name = "repair_filespace_stmt";
  else if (keyword == "REBUILD" && second == "FILESPACE") canonical_name = "rebuild_filespace_stmt";
  else if (keyword == "SALVAGE" && second == "FILESPACE") canonical_name = "salvage_filespace_stmt";
  else if (keyword == "CREATE" && second == "SNAPSHOT" && third == "FILESPACE") canonical_name = "create_snapshot_filespace_stmt";
  else if (keyword == "REFRESH" && second == "SNAPSHOT" && third == "FILESPACE") canonical_name = "refresh_snapshot_filespace_stmt";
  else if (keyword == "VALIDATE" && second == "SNAPSHOT" && third == "FILESPACE") canonical_name = "validate_snapshot_filespace_stmt";
  else if (keyword == "RETIRE" && second == "SNAPSHOT" && third == "FILESPACE") canonical_name = "retire_snapshot_filespace_stmt";
  else if (keyword == "CREATE" && second == "SHADOW" && third == "FILESPACE") canonical_name = "create_shadow_filespace_stmt";
  else if (keyword == "REFRESH" && second == "SHADOW" && third == "FILESPACE") canonical_name = "refresh_shadow_filespace_stmt";
  else if (keyword == "VALIDATE" && second == "SHADOW" && third == "FILESPACE") canonical_name = "validate_shadow_filespace_stmt";
  else if (keyword == "ALTER" && second == "SHADOW") canonical_name = "promote_shadow_filespace_stmt";
  else if (keyword == "DISCONNECT" && second == "FILESPACE") canonical_name = "disconnect_filespace_stmt";
  else if (keyword == "ATTACH" && second == "FILESPACE") canonical_name = "attach_filespace_stmt";
  else if (keyword == "DETACH" && second == "FILESPACE") canonical_name = "detach_filespace_stmt";
  else if (keyword == "PROMOTE" && second == "FILESPACE") canonical_name = "promote_filespace_stmt";
  else if (keyword == "OPENSEARCH") canonical_name = "opensearch_mapping_clause";
  else if (keyword == "CHANGE" && second == "STREAM") canonical_name = "change_stream_stmt";
  else if (keyword == "ACTIVATE" && second == "POLICY") canonical_name = "activate_policy_stmt";
  else if (keyword == "DEACTIVATE" && second == "POLICY") canonical_name = "deactivate_policy_stmt";
  else if (keyword == "VALIDATE" && second == "POLICY") canonical_name = "validate_policy_stmt";
  else if (keyword == "SET" && second == "ROLE") canonical_name = "set_role_stmt";
  else if (keyword == "SET" && second == "TRANSACTION") canonical_name = "set_transaction_stmt";
  else if (keyword == "SET") canonical_name = "set_session";
  else if (keyword == "BEGIN") {
    return second == "TRANSACTION"
               ? FindStatementSurfaceById("SBSQL-41AABA342C25")
               : FindStatementSurfaceById("SBSQL-1B59D6E97591");
  } else if (keyword == "COMMIT") {
    if (second == "RETAIN" || second == "OPTIONS") return FindStatementSurfaceByName("commit_options");
    return (second == "WORK" || second == "TRANSACTION")
               ? FindStatementSurfaceById("SBSQL-7A09CE443D7A")
               : FindStatementSurfaceById("SBSQL-37B92A5842F6");
  } else if (keyword == "ROLLBACK") {
    if (second == "TO" || third == "TO") {
      return FindStatementSurfaceById("SBSQL-3BF8303CFB36");
    }
    return (second == "WORK" || second == "TRANSACTION")
               ? FindStatementSurfaceById("SBSQL-129ADA0B6225")
               : FindStatementSurfaceById("SBSQL-EACF8DB1CB02");
  } else if (keyword == "SAVEPOINT") {
    return second.empty() || second == ";"
               ? FindStatementSurfaceById("SBSQL-35C5F6EA0613")
               : FindStatementSurfaceById("SBSQL-9EC31122A564");
  } else if (keyword == "RELEASE") {
    return second == "SAVEPOINT" ? FindStatementSurfaceById("SBSQL-9E33ED8C3B3D")
                                 : nullptr;
  }
  else if (keyword == "PREPARE") canonical_name = "prepare_stmt";
  else if (keyword == "EXECUTE") canonical_name = "execute_stmt";
  else if (keyword == "DEALLOCATE") canonical_name = "deallocate_stmt";
  else if (keyword == "OPEN" && second != "DATABASE") canonical_name = "psql_open_cursor_stmt";
  else if (keyword == "FETCH") canonical_name = "psql_fetch_stmt";
  else if (keyword == "CLOSE") canonical_name = "psql_close_cursor_stmt";
  else if (keyword == "DECLARE" && third == "CURSOR") canonical_name = "declare_cursor";
  else if (keyword == "CALL" && second == "TARGET" && third == "LIST") canonical_name = "call_target_list";
  else if (keyword == "CALL" && second == "ARG") canonical_name = "call_arg_list";
  else if (keyword == "CALL" && second == "STMT") canonical_name = "call_stmt";
  else if (keyword == "CALL") canonical_name = "call";
  else if (keyword == "GRANT") canonical_name = "grant";
  else if (keyword == "REVOKE") canonical_name = "revoke";
  else if (keyword == "LISTEN" || keyword == "UNLISTEN" || keyword == "NOTIFY") canonical_name = "listen_notify_stmt";
  else if (keyword == "POST") canonical_name = "post_event_stmt";
  else if (keyword == "SUBSCRIBE" || keyword == "UNSUBSCRIBE") canonical_name = "subscription_stmt";
  else if (keyword == "CHECKPOINT" && second == "ACTION") canonical_name = "checkpoint_action";
  else if (keyword == "CHECKPOINT" && second == "OPTIONS") canonical_name = "checkpoint_options";
  else if (keyword == "CHECKPOINT") canonical_name = "checkpoint_stmt";
  else if (keyword == "DECLARE" && second == "SUBROUTINE") canonical_name = "declare_subroutine";
  else if (keyword == "DECLARE" && second == "VARIABLE") canonical_name = "declare_variable";
  else if (keyword == "DECLARE" && second == "EXCEPTION") canonical_name = "declare_exception";
  else if (keyword == "ROUTINE" && second == "ATTRIBUTE") canonical_name = "routine_attribute";
  else if (keyword == "ROUTINE" && second == "BODY") canonical_name = "routine_body";
  else if (keyword == "SIGNAL" && second == "INFO" && third == "FIELD") canonical_name = "signal_info_field";
  else if (keyword == "SIGNAL" && second == "INFO" && third == "ASSIGNMENT") canonical_name = "signal_info_assignment";
  else if (keyword == "SIGNAL" && second == "TARGET") canonical_name = "signal_target";
  else if (keyword == "SIGNAL") canonical_name = "signal";
  else if (keyword == "COLON" && second == "VARIABLE") canonical_name = "colon_variable";
  else if (keyword == "EXCEPTION" && second == "HANDLER") canonical_name = "exception_handler";
  else if (keyword == "EXCEPTION" && second == "CONDITION" && third == "LIST") canonical_name = "exception_condition_list";
  else if (keyword == "EXCEPTION" && second == "CONDITION") canonical_name = "exception_condition";
  else if (keyword == "EXCEPTION" && second == "DECLARATION") canonical_name = "exception_declaration";
  else if (keyword == "EXCEPTION" && second == "SECTION") canonical_name = "exception_section";
  else if (keyword == "CALL" && second == "TARGET" && third == "LIST") canonical_name = "call_target_list";
  else if (keyword == "CALL" && second == "ARG") canonical_name = "call_arg_list";
  else if (keyword == "RAISE" && second == "SEVERITY") canonical_name = "raise_severity";
  else if (keyword == "RAISE" && second == "OPTION") canonical_name = "raise_option";
  else if (keyword == "RAISE") canonical_name = "raise";
  else if (keyword == "LVALUE") canonical_name = "lvalue";
  else if (keyword == "VARIABLE" && second == "DECL") canonical_name = "variable_decl_form";
  else if (keyword == "SINGLE" && second == "VAR") canonical_name = "single_var_form";
  else if (keyword == "ARG" && second == "LIST") canonical_name = "arg_list";
  else if (keyword == "PARAM" && second == "MODE") canonical_name = "param_mode";
  else if (keyword == "INTO" && second == "TARGET" && third == "LIST") canonical_name = "into_target_list";
  else if (keyword == "DECLARE" && second == "ITEM") canonical_name = "declare_item";
  else if (keyword == "DIAGNOSTIC" && second == "FILTER") canonical_name = "diagnostic_filter";
  else if (keyword == "DIAGNOSTIC" && second == "FAMILY") canonical_name = "diagnostic_family";
  else if (keyword == "RETURN" && second == "SHAPE") canonical_name = "return_shape";
  else if (keyword == "STORAGE" && second == "FILESPACE") canonical_name = "filespace_name";
  else if (keyword == "STORAGE" && second == "AOF") canonical_name = "aof_mode";
  else if (keyword == "STORAGE" && second == "REFERENCE") canonical_name = "reference_log_mode";  // reference compatibility, not authority
  else if (keyword == "STORAGE" && second == "TIER") canonical_name = "storage_tier";
  else if (keyword == "STORAGE" && second == "SECRET") canonical_name = "secret_storage";
  else if (keyword == "STORAGE" && second == "SHADOW") canonical_name = "shadow_name";
  else if (keyword == "RUN" && second == "JOB") canonical_name = "run_job_stmt";
  else if (keyword == "PAUSE" && (second == "JOB" || second == "JOBS")) canonical_name = "pause_job_stmt";
  else if (keyword == "RESUME" && (second == "JOB" || second == "JOBS")) canonical_name = "resume_job_stmt";
  else if (keyword == "CANCEL" && second == "JOB") canonical_name = "cancel_job_stmt";
  else if (keyword == "CLUSTER") canonical_name = "cluster_stmt";
  else if (keyword == "MULTIMODEL" && second == "OP") canonical_name = "multi_model_op_stmt";
  else if (keyword == "MULTIMODEL" && second == "METHOD") canonical_name = "multi_model_method";
  else if (keyword == "RECURSIVE" && second == "PATH" && third == "EXPR") canonical_name = "recursive_path_expr";
  else if (keyword == "TRAVERSAL" && second == "STRATEGY") canonical_name = "traversal_strategy";
  else if (keyword == "NEO4J" && second == "NODE" && third == "CONSTRAINT") canonical_name = "neo4j_node_constraint";
  else if (keyword == "MAP" && second == "CONSTRUCTOR") canonical_name = "map_constructor";
  else if (keyword == "MAP" && second == "ENTRY") canonical_name = "map_entry";
  else if (keyword == "NODE" && second == "MATCH") canonical_name = "node_match";
  else if (keyword == "NODE" && second == "PATTERN") canonical_name = "node_pattern";
  else if (keyword == "EDGE" && second == "PATTERN") canonical_name = "edge_pattern";
  else if (keyword == "PROPERTY" && second == "MATCH") canonical_name = "property_match";
  else if (keyword == "NODE") canonical_name = "cluster_node_op_stmt";
  else if (keyword == "MEMBER") canonical_name = "cluster_member_op_stmt";
  else if (keyword == "FAILOVER") canonical_name = "cluster_failover_stmt";
  else if (keyword == "RECONCILE") canonical_name = "cluster_reconcile_stmt";
  else if (keyword == "TOPOLOGY") canonical_name = "cluster_topology_stmt";
  else if (keyword == "THROTTLE") canonical_name = "cluster_throttle_stmt";
  else if (keyword == "PLACEMENT") canonical_name = "placement_clause";
  else if (keyword == "REGION" && second == "SPLIT") canonical_name = "region_split_stmt";
  else if (keyword == "REGION") canonical_name = "region_name";
  else if (keyword == "SHARD" && second == "METHOD") canonical_name = "shard_method";
  else if (keyword == "SHARD") canonical_name = "shard_clause";
  else if (keyword == "COPY") canonical_name = "copy_import_export";
  else if (keyword == "LOCK") canonical_name = "lock_table";
  if (canonical_name.empty()) return nullptr;
  return FindStatementSurfaceByName(canonical_name);
}

void ApplyStatementDescriptorMetadata(AstDocument* ast,
                                      const StatementSurfaceDescriptor* descriptor) {
  if (descriptor == nullptr) return;
  ast->statement_surface_id = descriptor->surface_id;
  ast->statement_surface_name = descriptor->canonical_name;
  ast->statement_parser_category = StatementParserCategoryName(descriptor->category);
  ast->parser_handler_key = descriptor->parser_handler_key;
  ast->statement_binding_contract_key = descriptor->binding_contract_key;
  ast->statement_admission_contract_key = descriptor->admission_contract_key;
  ast->statement_behavior_descriptor_key = descriptor->behavior_descriptor_key;
  ast->diagnostic_key = descriptor->diagnostic_key;
  ast->exact_refusal_required = descriptor->exact_refusal_required;
  ast->requires_cluster_profile = descriptor->cluster_scope == "cluster_private" ||
                                  descriptor->source_status == "cluster_private";
  if (ast->operation_family.empty()) {
    ast->operation_family = descriptor->sblr_operation_family;
  }
}

std::string AstNodeKindForToken(const Token& token) {
  switch (token.kind) {
    case TokenKind::kKeyword: return "keyword";
    case TokenKind::kIdentifier: return token.quoted ? "delimited_identifier" : "identifier";
    case TokenKind::kNumericLiteral:
    case TokenKind::kStringLiteral:
    case TokenKind::kBinaryLiteral:
    case TokenKind::kTemporalLiteral:
    case TokenKind::kUuidLiteral:
    case TokenKind::kBooleanLiteral:
    case TokenKind::kNullLiteral:
    case TokenKind::kDefaultLiteral:
    case TokenKind::kDocumentLiteral:
    case TokenKind::kVectorLiteral:
    case TokenKind::kRegexLiteral:
    case TokenKind::kRangeLiteral:
      return "literal";
    case TokenKind::kParameter: return "parameter";
    case TokenKind::kVariable: return "variable";
    case TokenKind::kOperator: return "operator";
    case TokenKind::kStatementTerminator: return "statement_terminator";
    case TokenKind::kMetaCommand: return "meta_command";
    case TokenKind::kSymbol: return "symbol";
    case TokenKind::kComment:
    case TokenKind::kWhitespace:
    case TokenKind::kParserDirective:
    case TokenKind::kEnd:
      return "source_artifact";
  }
  return "source_artifact";
}

void PopulateSourceArtifactNodes(AstDocument* ast, const CstDocument& cst) {
  ast->source_text = ReconstructSourceFromTokens(cst);
  ast->canonical_render = ast->source_text;
  ast->source_hash = Fnv1a64(ast->source_text);
  ast->statement_token_begin = FirstMeaningfulTokenIndex(cst);
  ast->statement_token_end = ast->statement_token_begin < cst.tokens.size()
                                 ? LastStatementTokenIndex(cst, ast->statement_token_begin)
                                 : ast->statement_token_begin;

  AstDocument::Node root;
  root.kind = "statement";
  root.text = "unknown";
  root.token_begin = ast->statement_token_begin;
  root.token_end = ast->statement_token_end;
  if (ast->statement_token_begin < cst.tokens.size()) {
    root.range = TokenSourceRange(cst.tokens[ast->statement_token_begin]);
    const auto& end = cst.tokens[ast->statement_token_end];
    root.range.length = (end.offset + end.length) - root.range.offset;
    root.range.end_line = end.end_line;
    root.range.end_column = end.end_column;
  }
  ast->nodes.push_back(std::move(root));
  ast->root_node_index = 0;

  if (ast->statement_token_begin >= cst.tokens.size()) return;
  for (std::size_t index = ast->statement_token_begin; index < cst.tokens.size(); ++index) {
    const auto& token = cst.tokens[index];
    if (token.kind == TokenKind::kEnd) break;
    AstDocument::Node node;
    node.kind = AstNodeKindForToken(token);
    node.text = token.text;
    node.range = TokenSourceRange(token);
    node.token_begin = index;
    node.token_end = index;
    node.source_artifact = true;
    ast->nodes[ast->root_node_index].children.push_back(ast->nodes.size());
    ast->nodes.push_back(std::move(node));
    if (token.kind == TokenKind::kStatementTerminator) break;
  }
}

StatementFamily FamilyForMetaCommandClass(MetaCommandSurfaceClass surface_class) {
  switch (surface_class) {
    case MetaCommandSurfaceClass::kMetadataReport: return StatementFamily::kObservability;
    case MetaCommandSurfaceClass::kConnectionSession: return StatementFamily::kSession;
    case MetaCommandSurfaceClass::kQueryExecution: return StatementFamily::kExecute;
    case MetaCommandSurfaceClass::kBulkIo: return StatementFamily::kInsert;
    case MetaCommandSurfaceClass::kAdminControl: return StatementFamily::kRuntimeManagement;
    case MetaCommandSurfaceClass::kToolLocal:
    case MetaCommandSurfaceClass::kUnsafeLocalShell:
    case MetaCommandSurfaceClass::kUnknown:
      return StatementFamily::kRuntimeManagement;
  }
  return StatementFamily::kRuntimeManagement;
}

void ApplyMetaCommandSurface(AstDocument* ast, const Token& token) {
  const auto* record = ResolveMetaCommandSurface(token.raw_text.empty() ? token.text : token.raw_text);
  if (record == nullptr) record = &UnknownMetaCommandSurface();
  ast->family = FamilyForMetaCommandClass(record->surface_class);
  ast->registry_family = std::string(record->registry_ref);
  ast->operation_family = record->sblr_operation_family.empty()
                              ? "sblr.diagnostic.refusal.v3"
                              : std::string(record->sblr_operation_family);
  ast->statement_kind = "meta_command";
  ast->statement_surface_id = std::string(record->surface_id);
  ast->statement_surface_name = std::string(record->command);
  ast->statement_parser_category =
      std::string(MetaCommandSurfaceClassName(record->surface_class));
  ast->parser_handler_key = "parser.meta_command_surface";
  ast->statement_binding_contract_key = "binder.meta_command.exact_refusal_or_tool_local";
  ast->statement_admission_contract_key = "admission.meta_command.fail_closed";
  ast->statement_behavior_descriptor_key =
      std::string("behavior.meta_command.") +
      std::string(MetaCommandDispositionName(record->disposition));
  ast->diagnostic_key = std::string(record->refusal_diagnostic);
  ast->produces_sblr = record->disposition == MetaCommandDisposition::kLowerThroughEngine;
  ast->exact_refusal_required = record->disposition != MetaCommandDisposition::kLowerThroughEngine;
  ast->messages.diagnostics.push_back(
      MetaCommandRefusalDiagnostic(*record, token.raw_text.empty() ? token.text : token.raw_text));
}

} // namespace

std::string StatementFamilyName(StatementFamily family) {
  switch (family) {
    case StatementFamily::kUnknown: return "unknown";
    case StatementFamily::kQuery: return "query";
    case StatementFamily::kInsert: return "insert";
    case StatementFamily::kUpdate: return "update";
    case StatementFamily::kDelete: return "delete";
    case StatementFamily::kMerge: return "merge";
    case StatementFamily::kUpsert: return "upsert";
    case StatementFamily::kCatalog: return "catalog";
    case StatementFamily::kShow: return "show";
    case StatementFamily::kSession: return "session";
    case StatementFamily::kTransaction: return "transaction";
    case StatementFamily::kExecute: return "execute";
    case StatementFamily::kCall: return "call";
    case StatementFamily::kValues: return "values";
    case StatementFamily::kSecurity: return "security";
    case StatementFamily::kObservability: return "observability";
    case StatementFamily::kRuntimeManagement: return "runtime_management";
    case StatementFamily::kStorageManagement: return "storage_management";
    case StatementFamily::kJobsScheduler: return "jobs_scheduler";
    case StatementFamily::kArchiveReplication: return "archive_replication";
    case StatementFamily::kAcceleration: return "acceleration";
    case StatementFamily::kMultiModel: return "multi_model";
    case StatementFamily::kMigration: return "migration";
    case StatementFamily::kBridge: return "bridge";
    case StatementFamily::kClusterPrivate: return "cluster_private";
  }
  return "unknown";
}

AstDocument BuildAst(const CstDocument& cst) {
  AstDocument ast;
  ast.messages = cst.messages;
  PopulateSourceArtifactNodes(&ast, cst);
  if (ast.messages.has_errors()) return ast;
  const auto* first = FirstMeaningfulToken(cst);
  if (first == nullptr) {
    ast.messages.diagnostics.push_back(MakeDiagnostic(
        "SBSQL.PARSER.EMPTY_STATEMENT", "ERROR", "statement is empty", "sbp_sbsql.ast"));
    return ast;
  }
  if (first->kind == TokenKind::kMetaCommand) {
    ApplyMetaCommandSurface(&ast, *first);
    if (!ast.nodes.empty()) {
      ast.nodes[ast.root_node_index].text = ast.statement_kind;
    }
    return ast;
  }
  const auto words = MeaningfulTokenWords(cst);
  const auto raw_words = RawMeaningfulTokenWords(cst);
  const auto keyword = words.empty() ? CanonicalWordForToken(*first) : words[0];
  const auto second = words.size() > 1 ? std::string_view(words[1]) : std::string_view();
  const auto third = words.size() > 2 ? std::string_view(words[2]) : std::string_view();
  const auto fourth = words.size() > 3 ? std::string_view(words[3]) : std::string_view();
  const auto raw_keyword = raw_words.empty() ? CanonicalWordForToken(*first) : raw_words[0];
  const auto raw_second =
      raw_words.size() > 1 ? std::string_view(raw_words[1]) : std::string_view();
  const bool filespace_lifecycle_target_surface =
      (keyword == "DELETE" && second == "STORAGE" && third == "FILESPACE") ||
      ((keyword == "ATTACH" || keyword == "DETACH" || keyword == "PROMOTE" ||
        keyword == "MOVE" || keyword == "MERGE" ||
        keyword == "GROW" || keyword == "RESIZE" || keyword == "SHRINK" ||
        keyword == "VERIFY" || keyword == "COMPACT" || keyword == "FENCE" ||
        keyword == "RELEASE" || keyword == "ARCHIVE" ||
        keyword == "QUARANTINE" || keyword == "REPAIR" ||
        keyword == "REBUILD" || keyword == "SALVAGE" ||
        keyword == "DISCONNECT") &&
       second == "FILESPACE") ||
      ((keyword == "CREATE" || keyword == "REFRESH" || keyword == "VALIDATE" ||
        keyword == "RETIRE") &&
       second == "SNAPSHOT" && third == "FILESPACE") ||
      ((keyword == "CREATE" || keyword == "REFRESH" || keyword == "VALIDATE") &&
       second == "SHADOW" && third == "FILESPACE") ||
      (keyword == "ALTER" && second == "SHADOW");
  const bool language_control_surface =
      (keyword == "SET" && second == "LANGUAGE") ||
      (keyword == "RESET" && second == "LANGUAGE") ||
      (keyword == "SHOW" && second == "LANGUAGE") ||
      ((keyword == "LOAD" || keyword == "UNLOAD" || keyword == "VALIDATE") &&
       second == "LANGUAGE" && third == "BUNDLE");
  const bool transaction_lock_surface =
      keyword == "LOCK" ||
      (keyword == "UNLOCK" && (second == "TABLE" || second == "NAMED"));
  const auto engine_api_command_domain = EngineApiCommandAstDomainFor(cst);
  const bool sbsfc079_general_residual =
      keyword == "KEYVALUE" ||
      keyword == "PROPERTY" ||
      keyword == "EDGE" ||
      keyword == "MULTIMODEL" ||
      keyword == "RECURSIVE" ||
      keyword == "TRAVERSAL" ||
      keyword == "NEO4J" ||
      keyword == "MAP" ||
      (keyword == "NODE" && (second == "MATCH" || second == "PATTERN")) ||
      (keyword == "DOC" &&
       (second == "WRITE" || second == "PIPELINE" || second == "CRUD" ||
        second == "PATH" || second == "READ" || second == "VERIFIABLE")) ||
      (keyword == "FULLTEXT" &&
       (second == "FILTER" || second == "AGGS" || second == "AGG" ||
        second == "PAGING")) ||
      (keyword == "CYPHER" &&
       ((second == "NODE" && third == "PATTERN") ||
        (second == "RETURN" && (third == "LIST" || third == "ITEM")) ||
        (second == "WITH" && third == "PIPELINE") ||
        (second == "CONSTRAINT" &&
         (third == "PREDICATE" || third == "TARGET")) ||
        (second == "RELATIONSHIP" && third == "PATTERN") ||
        (second == "REMOVE" && (third == "CLAUSE" || third == "ITEM")) ||
        second == "STMT" ||
        (second == "CALL" && third == "CLAUSE") ||
        (second == "SET" && (third == "ITEM" || third == "CLAUSE")) ||
        (second == "RANGE" && third == "SPEC") ||
        (second == "FOREACH" && third == "CLAUSE") ||
        (second == "OPTIONAL" && third == "MATCH") ||
        second == "CLAUSE" ||
        (second == "LABEL" && third == "LIST") ||
        second == "PATTERN" ||
        (second == "PROPERTY" && third == "MATCH") ||
        (second == "USE" && third == "CLAUSE") ||
        (second == "REL" && third == "DETAIL") ||
        (second == "YIELD" && third == "ITEM") ||
        (second == "UNWIND" && third == "CLAUSE") ||
        (second == "MATCH" && third == "CLAUSE")));
  const bool sbsfc081_descriptor_expression_residual =
      keyword == "LETTER" ||
      keyword == "POSTFIX" ||
      keyword == "FORK" ||
      keyword == "ACTION" ||
      keyword == "OBJECT" ||
      keyword == "STRICT" ||
      keyword == "PLAN" ||
      keyword == "DICT" ||
      keyword == "XT" ||
      keyword == "REGEX" ||
      keyword == "CH" ||
      keyword == "TZ" ||
      keyword == "OPAQUE" ||
      keyword == "UUID" ||
      keyword == "MONITOR" ||
      keyword == "LOCATOR" ||
      keyword == "ID" ||
      keyword == "CONSISTENCY" ||
      keyword == "TAG" ||
      keyword == "INTERVAL" ||
      keyword == "DIAG" ||
      keyword == "LANG" ||
      keyword == "PATH" ||
      keyword == "EXCLUSION" ||
      keyword == "HINT" ||
      keyword == "SUBPARTITION" ||
      keyword == "OS" ||
      keyword == "REVISION" ||
      keyword == "OCCURRENCES" ||
      keyword == "OPERATOR" ||
      (keyword == "EVENT" && second == "TRIGGER" &&
       (third == "FILTER" || third == "EVENT")) ||
      (keyword == "AGGREGATE" && second == "ATTR") ||
      (keyword == "OP" && second == "CLASS") ||
      (keyword == "CASSANDRA" && second == "USING") ||
      (keyword == "EXP" && second == "PART") ||
      (keyword == "PARTITION" &&
       ((second == "SPEC" && third == "LIST") || second == "VALUES")) ||
      (keyword == "PATTERN" && second == "NAME") ||
      (keyword == "LABEL" && second == "REF") ||
      (keyword == "TRIGGER" &&
       (second == "TIMING" || (second == "EVENT" && third == "LIST"))) ||
      (keyword == "LOG" && second == "MESSAGE") ||
      (keyword == "SUBSTRING" && second == "REGEX") ||
      (keyword == "RANGE" && second == "SPEC") ||
      (keyword == "CALL" && second == "ARG" && third != "LIST") ||
      (keyword == "COLON" && second == "LAMBDA") ||
      (keyword == "FOR" && second == "CLAUSE") ||
      (keyword == "TRANSLATE" && second == "REGEX") ||
      (keyword == "SET" && second == "CLAUSE" && third == "LIST");
  const bool sbsfc082_surface_descriptor =
      keyword == "DIGIT" ||
      (keyword == "MONITOR" && second == "INPUT") ||
      (keyword == "EXPIRY" && second == "CLAUSE") ||
      (keyword == "BOOL" && second == "CLAUSE") ||
      (keyword == "SQL" && second == "TEXT") ||
      (keyword == "REBUILD" && second == "OPTIONS") ||
      (keyword == "MATERIALIZATION" && second == "HINT") ||
      (keyword == "RESOLVE" && second == "NAME" && third == "PUBLIC") ||
      (keyword == "RENDER" && second == "CONTRACT" && third == "NAME") ||
      (keyword == "CHARSET" && second == "CLAUSE") ||
      (keyword == "DIAG" && second == "ASSIGNMENT") ||
      (keyword == "LABEL" && second == "DECL") ||
      (keyword == "IN" && second == "TARGET") ||
      (keyword == "RANGE" && (second == "OPTIONS" || second == "OP")) ||
      (keyword == "DICT" && second == "LIFETIME") ||
      (keyword == "HETEROGENEOUS" && second == "FORM") ||
      (keyword == "TTL" &&
       ((second == "SET" && third == "ASSIGN") || second == "ACTION")) ||
      (keyword == "PARTITION" && second == "METHOD") ||
      (keyword == "HINT" && second == "DIRECTIVE") ||
      (keyword == "FLASHBACK" && second == "STMT") ||
      (keyword == "DDL" && second == "MULTIMODEL" && third == "STMT") ||
      (keyword == "QUALIFY" && second == "CLAUSE") ||
      (keyword == "SUPPORT" && second == "STMT") ||
      (keyword == "VACUUM" && second == "OPTION") ||
      (keyword == "ENCRYPTION" && second == "OPTIONS") ||
      (keyword == "PERIOD" && second == "LOOKUP") ||
      (keyword == "DIALECT" && second == "NAME") ||
      (keyword == "COPROCESSOR" && second == "HINT") ||
      (keyword == "CA" && second == "NAME") ||
      (keyword == "UUID" && second == "REFERENCE") ||
      (keyword == "COMPRESSION" && second == "CLAUSE") ||
      (keyword == "SET" && second == "QUANTIFIER") ||
      (keyword == "EDITION" && second == "SCOPE") ||
      (keyword == "SWEEP" && second == "PRIORITY") ||
      (keyword == "OS" && second == "ANALYZER" && third == "DEF") ||
      (keyword == "COMPACT" && second == "OPTIONS") ||
      (keyword == "MOVE" && second == "OPTIONS") ||
      (keyword == "IMPORT" && second == "OPTIONS") ||
      (keyword == "FILTER" && second == "CLAUSE") ||
      (keyword == "GENERATION" && second == "CLAUSE") ||
      (keyword == "ANALYZE" && second == "OPTION") ||
      (keyword == "BUFFER" && second == "POOL" && third == "STMT") ||
      (keyword == "CLEANUP" && second == "ACTION") ||
      (keyword == "EVENT" && second == "BODY") ||
      (keyword == "BRANCH" && second == "OP" && third == "STMT") ||
      (keyword == "ADMISSION" && second == "STMT") ||
      (keyword == "OPCLASS" && second == "ITEM") ||
      (keyword == "ENUM" && second == "VALUE" && third == "ASSIGN");
  const bool sbsfc084_grammar_surface =
      (keyword == "RERANKER" && second == "SPEC") ||
      (keyword == "LIMBO" && second == "STMT") ||
      (keyword == "PRE" && second == "SPLIT" && third == "CLAUSE") ||
      (keyword == "SALVAGE" && second == "OPTIONS") ||
      (keyword == "REFERENCE" && second == "PROFILE" && third == "STMT") ||
      (keyword == "RETURNING" && second == "CLAUSE") ||
      (keyword == "HISTORICAL" && second == "READ" && third == "CLAUSE") ||
      (keyword == "ARTIFACT" && second == "REF") ||
      (keyword == "FILL" && second == "STRATEGY") ||
      (keyword == "BRANCH" && second == "OPTIONS") ||
      (keyword == "HEX" && second == "DIGIT") ||
      (keyword == "RESIGNAL") ||
      (keyword == "FOR" &&
       ((second == "CURSOR" && third == "FORM") ||
        (second == "PERIOD" && third == "CLAUSE"))) ||
      (keyword == "INTO" && second == "TARGET" && third != "LIST") ||
      (keyword == "PK" && second == "COLUMN" && third == "SPEC") ||
      (keyword == "CONSISTENCY" && second == "OPTIONS") ||
      (keyword == "ROUTINE" && second == "NAME") ||
      (keyword == "CTE" &&
       ((second == "CYCLE" && third == "CLAUSE") ||
        (second == "FUNCTION" && third == "DEF"))) ||
      (keyword == "PIT" && second == "STMT") ||
      (keyword == "ARG" && second == "VALUE") ||
      (keyword == "EVICTION" && second == "CLAUSE") ||
      (keyword == "FK" && second == "ACTION" && third != "CLAUSE") ||
      (keyword == "PSQL" &&
       ((second == "DML" && third == "STMT") ||
        (second == "SUSPEND" && third == "STMT"))) ||
      (keyword == "IDEMPOTENCY" && second == "LEVEL") ||
      (keyword == "TENANT" && second == "ACTION") ||
      (keyword == "ENCRYPTION" && second == "CLAUSE") ||
      (keyword == "SIZE" && second == "SPEC") ||
      (keyword == "CAPPED" && second == "OPTION") ||
      (keyword == "OPERATOR" && second == "ATTR") ||
      (keyword == "TCL" && second == "STMT") ||
      (keyword == "TREAT" && second == "FORM") ||
      (keyword == "PLAN" && second == "OPTIONS") ||
      (keyword == "MULTISET" && second == "CONSTRUCTOR") ||
      (keyword == "TRY" && second == "CAST" && third == "FORM") ||
      (keyword == "PARAM" && second == "LIST") ||
      (keyword == "SET" && second == "ASSIGNMENT") ||
      (keyword == "CALL" && second == "TARGET" && third != "LIST") ||
      (keyword == "CONTAINER" && second == "PREDICATE") ||
      (keyword == "SWEEP" && second == "ACTION") ||
      (keyword == "SECRET" && second == "ATTR") ||
      (keyword == "PERIOD" && second == "NAME") ||
      (keyword == "DDL" && second == "RELATIONAL" && third == "STMT") ||
      (keyword == "ID" && second == "START") ||
      (keyword == "PIPELINE" && second == "PROCESSOR") ||
      (keyword == "PREVIOUS" && second == "VALUE" && third == "FORM") ||
      (keyword == "VARIABLE" && second == "DECLARATION") ||
      (keyword == "PARTITION" && second == "SPEC" && third != "LIST");
  const bool sbsfc085_grammar_surface =
      (keyword == "PSQL" &&
       ((second == "PIPE" && third == "ROW" && fourth == "STMT") ||
        (second == "COMPOUND" && third == "STMT") ||
        (second == "EXIT" && third == "STMT") ||
        (second == "CONTINUE" && third == "STMT"))) ||
      (keyword == "CALL" && second != "TARGET" && second != "ARG" &&
       !IsParenthesizedCallInvocation(cst)) ||
      (keyword == "REFERENCE" && second == "PROFILE" && third == "OPTIONS") ||
      (keyword == "NEW" && second == "SURFACE" && third == "STMT") ||
      (keyword == "CH" && second == "SYSTEM" && third == "TARGET") ||
      (keyword == "CLEANUP" && second == "CONTROL" && third == "STMT") ||
      (keyword == "TOPOLOGY" && second == "ASSIGN") ||
      (keyword == "FRAME" && second == "BOUND") ||
      (keyword == "OBJECT" && second == "PATH") ||
      (keyword == "FDB" && second == "TX" && third == "OPTIONS") ||
      (keyword == "SERVER" && second == "ACTION") ||
      (keyword == "SUBPARTITION" && second == "SPEC" && third == "LIST") ||
      (keyword == "EXCEPTION" && second == "CONDITION" && third != "LIST") ||
      (keyword == "DECLARE" && second == "ITEM");
  const bool sbsfc083_grammar_surface =
      (keyword == "JT" && second == "COLUMN") ||
      (keyword == "DICT" && (second == "SOURCE" || second == "LAYOUT")) ||
      (keyword == "CONFORMANCE" && second == "TARGET") ||
      (keyword == "PACKAGE" && second == "ITEM") ||
      (keyword == "PERIOD" && second == "CLAUSE") ||
      (keyword == "HISTORICAL" &&
       ((second == "READ" && third == "TARGET") ||
        (second == "TIME" && third == "SPEC"))) ||
      (keyword == "MANAGEMENT" && second == "STMT") ||
      (keyword == "HASH" && second == "FIELD" && third == "VALUE") ||
      (keyword == "CH" &&
       ((second == "CODEC" && third == "SPEC") ||
        (second == "SYSTEM" && third == "VERB"))) ||
      (keyword == "PUBLICATION" && second == "STMT") ||
      (keyword == "FOR" && second == "PORTION" && third == "OF" &&
       fourth == "CLAUSE") ||
      (keyword == "FK" && second == "ACTION" && third == "CLAUSE") ||
      (keyword == "TAG" && second != "OPTIONS") ||
      (keyword == "COLLATE" && (second == "POSTFIX" || second == "CLAUSE")) ||
      (keyword == "NEXT" && second == "VALUE" && third == "FORM") ||
      (keyword == "TRIGGER" &&
       (second == "REFERENCING" || (second == "EVENT" && third != "LIST") ||
        second == "TARGET")) ||
      (keyword == "PROFILE" && second == "NAME") ||
      (keyword == "GENERIC" && second == "OPTIONS") ||
      (keyword == "MATCH" && second == "ARG") ||
      (keyword == "SERVICE" && second == "STMT") ||
      (keyword == "TRANSFORM" && second == "OP") ||
      (keyword == "DDL" && second == "ROUTINE" && third == "STMT") ||
      (keyword == "TIMELINE" && second == "CLAUSE") ||
      (keyword == "CAPPED" && second == "CLAUSE") ||
      (keyword == "LABEL" && second == "SET") ||
      (keyword == "FIELD" && second == "DEF") ||
      (keyword == "POSITION" && second == "REGEX" && third == "FORM") ||
      (keyword == "ANN" && second == "OPTIONS") ||
      (keyword == "ROW" && second == "SHAPE") ||
      (keyword == "CONTINUOUS" && second == "AGGREGATE" && third == "STMT") ||
      (keyword == "ENFORCED" && second == "MODIFIER") ||
      (keyword == "MULTISET" && second == "SET" && third == "OP") ||
      (keyword == "BUCKET" && second == "ATTR") ||
      (keyword == "IDENTITY" && second == "OPTIONS") ||
      (keyword == "CORRELATION" && second == "NAME") ||
      (keyword == "REFRESH" && second == "STRATEGY") ||
      (keyword == "PIPELINE" && second == "STAGE") ||
      (keyword == "CONSISTENCY" && second == "LEVEL") ||
      (keyword == "THROTTLE" && second == "ASSIGN") ||
      (keyword == "VAR" && second == "DEFAULT") ||
      (keyword == "MONITOR" && second == "ACTION") ||
      (keyword == "POSTFIX" && second == "UNARY" && third == "EXPR") ||
      (keyword == "DATA" && second == "CHANGE" && third == "STMT") ||
      (keyword == "PSQL" && second == "IF" && third == "STMT");
  if (raw_keyword == "INSERT") {
    ast.family = StatementFamily::kInsert;
    ast.registry_family = "sbsql.dml.insert.v3";
    ast.operation_family = "sblr.dml.insert.v3";
    ast.requires_name_resolution = true;
    ast.produces_sblr = true;
  } else if (raw_keyword == "UPDATE") {
    ast.family = StatementFamily::kUpdate;
    ast.registry_family = "sbsql.dml.update.v3";
    ast.operation_family = "sblr.dml.update.v3";
    ast.requires_name_resolution = true;
    ast.produces_sblr = true;
  } else if (raw_keyword == "DELETE" && raw_second != "STORAGE") {
    ast.family = StatementFamily::kDelete;
    ast.registry_family = "sbsql.dml.delete.v3";
    ast.operation_family = "sblr.dml.delete.v3";
    ast.requires_name_resolution = true;
    ast.produces_sblr = true;
  } else if (raw_keyword == "MERGE" && raw_second != "FILESPACE") {
    ast.family = StatementFamily::kMerge;
    ast.registry_family = "sbsql.dml.merge.v3";
    ast.operation_family = "sblr.dml.merge.v3";
    ast.requires_name_resolution = true;
    ast.produces_sblr = true;
  } else if (raw_keyword == "UPSERT") {
    ast.family = StatementFamily::kUpsert;
    ast.registry_family = "sbsql.dml.upsert.v3";
    ast.operation_family = "sblr.dml.operation.v3";
    ast.requires_name_resolution = true;
    ast.produces_sblr = true;
  } else if (raw_keyword == "COPY") {
    ast.family = StatementFamily::kInsert;
    ast.requires_name_resolution = true;
    ast.produces_sblr = true;
  } else if ((raw_keyword == "CYPHER" &&
              (raw_second == "DELETE" || raw_second == "MERGE" || raw_second == "LOAD")) ||
             (raw_keyword == "GRAPH" && raw_second == "DELETE") ||
             (raw_keyword == "DOCUMENT" && (raw_second == "UPDATE" || raw_second == "BULK")) ||
             (raw_keyword == "GPU" && raw_second == "WORKLOAD") ||
             (raw_keyword == "LOAD" && raw_second == "DATA")) {
    ast.family = StatementFamily::kUpdate;
    ast.registry_family = "sbsql.dml.operation.v3";
    ast.operation_family = "sblr.dml.operation.v3";
    ast.requires_name_resolution = true;
    ast.produces_sblr = true;
  } else if (IsPublicExactMigrationStatement(cst)) {
    ast.family = StatementFamily::kMigration;
    ast.registry_family = "sbsql.migration.operation.v3";
    ast.operation_family = "sblr.migration.operation.v3";
    ast.statement_parser_category = "migration";
    ast.requires_name_resolution = false;
    ast.produces_sblr = true;
  } else if (engine_api_command_domain != EngineApiCommandAstDomain::kNone) {
    switch (engine_api_command_domain) {
      case EngineApiCommandAstDomain::kAgentManagement:
      case EngineApiCommandAstDomain::kManagement:
      case EngineApiCommandAstDomain::kLifecycle:
      case EngineApiCommandAstDomain::kEventNotification:
        ast.family = StatementFamily::kRuntimeManagement;
        ast.registry_family = "sbsql.runtime_management.v3";
        ast.operation_family = "sblr.management.runtime_operation.v3";
        break;
      case EngineApiCommandAstDomain::kArtifact:
      case EngineApiCommandAstDomain::kCatalog:
        ast.family = StatementFamily::kCatalog;
        ast.registry_family = "sbsql.catalog.mutation.v3";
        ast.operation_family = "sblr.catalog.mutation.v3";
        break;
      case EngineApiCommandAstDomain::kDmlImport:
        ast.family = StatementFamily::kInsert;
        ast.registry_family = "sbsql.dml.operation.v3";
        ast.operation_family = "sblr.dml.operation.v3";
        break;
      case EngineApiCommandAstDomain::kQueryRuntime:
        ast.family = StatementFamily::kQuery;
        ast.registry_family = "sbsql.query.runtime.v3";
        ast.operation_family = "sblr.expression.runtime.v3";
        break;
      case EngineApiCommandAstDomain::kTransactionControl:
        ast.family = StatementFamily::kTransaction;
        ast.registry_family = "sbsql.transaction.control.v3";
        ast.operation_family = "sblr.transaction.control.v3";
        break;
      case EngineApiCommandAstDomain::kSecurity:
        ast.family = StatementFamily::kSecurity;
        ast.registry_family = "sbsql.security.v3";
        ast.operation_family = "sblr.security.mutation.v3";
        break;
      case EngineApiCommandAstDomain::kParserPackage:
        ast.family = StatementFamily::kRuntimeManagement;
        ast.registry_family = "sbsql.extensibility.operation.v3";
        ast.operation_family = "sblr.udr.operation.v3";
        break;
      case EngineApiCommandAstDomain::kClusterProvider:
        ast.family = StatementFamily::kClusterPrivate;
        ast.registry_family = "sbsql.cluster.private_operation.v3";
        ast.operation_family = "sblr.cluster.private_operation.v3";
        break;
      case EngineApiCommandAstDomain::kNone:
        break;
    }
    ast.requires_name_resolution = false;
    ast.produces_sblr = true;
  } else if (keyword == "SBSQL_SURFACE_REPLAY") {
    ast.family = StatementFamily::kObservability;
    ast.registry_family = "sbsql.observability.inspect.v3";
    ast.operation_family = "sblr.observability.inspect.v3";
    ast.statement_parser_category = "observability";
    ast.statement_binding_contract_key = "binder.statement.observability_surface_replay";
    ast.statement_admission_contract_key = "admission.sblr.observability_inspect";
    ast.statement_behavior_descriptor_key = "behavior.sbsql.surface_replay";
    ast.diagnostic_key = "diagnostic.canonical_message_vector";
    ast.requires_name_resolution = false;
    ast.produces_sblr = true;
  } else if (IsBridgeStatementWords(words)) {
    ast.family = StatementFamily::kBridge;
    ast.registry_family = "sbsql.bridge.operation.v3";
    ast.operation_family = "sblr.bridge.operation.v3";
    ast.statement_parser_category = "bridge";
    ast.statement_binding_contract_key = "binder.statement.bridge_context";
    ast.statement_admission_contract_key = "admission.sblr.bridge_operation";
    ast.statement_behavior_descriptor_key = "behavior.bridge.udr_dispatch";
    ast.diagnostic_key = "diagnostic.canonical_message_vector";
    ast.requires_name_resolution = false;
    ast.produces_sblr = true;
  } else if (language_control_surface) {
    if (keyword == "SHOW") {
      ast.family = StatementFamily::kObservability;
    } else if (keyword == "SET" || keyword == "RESET") {
      ast.family = StatementFamily::kSession;
    } else {
      ast.family = StatementFamily::kRuntimeManagement;
    }
    ast.registry_family = "sbsql.language.resource_control.v3";
    ast.operation_family = "sblr.language.resource_control.v3";
    ast.statement_parser_category = "language_resource";
    ast.statement_binding_contract_key = "binder.statement.language_resource_control";
    ast.statement_admission_contract_key = "admission.sblr.language_resource_control";
    ast.statement_behavior_descriptor_key =
        "behavior.language_resource.parser_surface_server_revalidation";
    ast.diagnostic_key = "diagnostic.canonical_message_vector";
    ast.requires_name_resolution = false;
    ast.produces_sblr = true;
  } else if (sbsfc079_general_residual) {
    ast.family = StatementFamily::kMultiModel;
    ast.registry_family = "sbsql.general.operation.v3";
    ast.operation_family = "sblr.general.operation.v3";
    ast.requires_name_resolution = true;
    ast.produces_sblr = true;
  } else if (sbsfc085_grammar_surface) {
    ast.family = StatementFamily::kQuery;
    ast.registry_family = "sbsql.general.operation.v3";
    ast.operation_family = "sblr.general.operation.v3";
    ast.requires_name_resolution = false;
    ast.produces_sblr = true;
  } else if (sbsfc084_grammar_surface) {
    ast.family = StatementFamily::kQuery;
    ast.registry_family = "sbsql.general.operation.v3";
    ast.operation_family = "sblr.general.operation.v3";
    ast.requires_name_resolution = false;
    ast.produces_sblr = true;
  } else if (sbsfc083_grammar_surface) {
    ast.family = StatementFamily::kQuery;
    ast.registry_family = "sbsql.general.operation.v3";
    ast.operation_family = "sblr.general.operation.v3";
    ast.requires_name_resolution = false;
    ast.produces_sblr = true;
  } else if (sbsfc082_surface_descriptor) {
    ast.family = StatementFamily::kQuery;
    ast.registry_family = "sbsql.general.operation.v3";
    ast.operation_family = "sblr.general.operation.v3";
    ast.requires_name_resolution = false;
    ast.produces_sblr = true;
  } else if (sbsfc081_descriptor_expression_residual) {
    ast.family = StatementFamily::kQuery;
    ast.registry_family = "sbsql.general.operation.v3";
    ast.operation_family = "sblr.general.operation.v3";
    ast.requires_name_resolution = false;
    ast.produces_sblr = true;
  } else if (IsPublicExactAccelerationStatement(cst)) {
    ast.family = StatementFamily::kAcceleration;
    ast.registry_family = "sbsql.acceleration.operation.v3";
    ast.operation_family = "sblr.acceleration.operation.v3";
    ast.requires_name_resolution = false;
    ast.produces_sblr = true;
  } else if (IsPublicExactManagementStatement(cst)) {
    ast.family = StatementFamily::kRuntimeManagement;
    ast.registry_family = "sbsql.runtime_management.v3";
    ast.operation_family = "sblr.management.runtime_operation.v3";
    ast.requires_name_resolution = false;
    ast.produces_sblr = true;
  } else if (IsPublicExactSecurityInspectStatement(cst)) {
    ast.family = StatementFamily::kSecurity;
    ast.registry_family = "sbsql.security.v3";
    ast.operation_family = "sblr.catalog.introspect.v3";
    ast.requires_name_resolution = false;
    ast.produces_sblr = true;
  } else if (IsPublicExactObservabilityInspectStatement(cst)) {
    ast.family = StatementFamily::kObservability;
    ast.registry_family = "sbsql.observability.inspect.v3";
    ast.operation_family = "sblr.observability.inspect.v3";
    ast.requires_name_resolution = false;
    ast.produces_sblr = true;
  } else if (IsClusterProviderRouteStatement(cst)) {
    ast.family = StatementFamily::kClusterPrivate;
    ast.registry_family = "sbsql.cluster.private_operation.v3";
    ast.operation_family = "sblr.cluster.private_operation.v3";
    ast.requires_name_resolution = false;
    ast.produces_sblr = true;
  } else if (keyword == "VALIDATOR" ||
             keyword == "REFUSAL" ||
             keyword == "SUBJECT" ||
             keyword == "EDITION" ||
             keyword == "CONFIG" ||
             keyword == "EXTENSION" ||
             keyword == "QUOTA" ||
             keyword == "SETTINGS" ||
             keyword == "VERIFICATION" ||
             keyword == "PRAGMA" ||
             keyword == "AUDIT" ||
             keyword == "SESSION" ||
             keyword == "EVENT" ||
             keyword == "MASKING" ||
             keyword == "DISCONNECT" ||
             keyword == "CONNECT" ||
             keyword == "SNAPSHOT" ||
             keyword == "ATOMICITY" ||
             keyword == "ISO" ||
             keyword == "NAMED" ||
             keyword == "COLOCATION" ||
             keyword == "LOCALITY" ||
             keyword == "ZONE" ||
             keyword == "IGNITE" ||
             keyword == "PIPELINE" ||
             keyword == "ACCELERATION" ||
             keyword == "BUFFER" ||
             keyword == "KERNEL" ||
             keyword == "SWEEP" ||
             keyword == "ENGINE" ||
             keyword == "COMPILE" ||
             keyword == "OPTIMIZATION" ||
             keyword == "COMPRESSION" ||
             (keyword == "SET" && (second == "SESSION" || second == "CONSTRAINTS"))) {
    ast.family = StatementFamily::kRuntimeManagement;
    ast.registry_family = "sbsql.general.operation.v3";
    ast.operation_family = "sblr.general.operation.v3";
    ast.requires_name_resolution = false;
    ast.produces_sblr = true;
  } else if (raw_keyword == "GRANT" || raw_keyword == "REVOKE") {
    ast.family = StatementFamily::kSecurity;
    ast.registry_family = "sbsql.security.v3";
    ast.operation_family = "sblr.security.mutation.v3";
    ast.requires_name_resolution = true;
    ast.produces_sblr = true;
  } else if (keyword == "SELECT" || keyword == "WITH") {
    ast.family = StatementFamily::kQuery;
    ast.registry_family = "sbsql.query.relational.v3";
    ast.operation_family = "sblr.query.relational.v3";
    const bool array_select_policy_refusal = IsArraySelectPolicyRefusalProjection(cst);
    const bool recursive_cte_exact_route = keyword == "WITH" && second == "RECURSIVE";
    ast.requires_name_resolution =
        recursive_cte_exact_route
            ? false
            : (keyword == "WITH" ||
               ContainsTopLevelKeyword(cst, "FROM") ||
               (ContainsNestedKeyword(cst, "SELECT") &&
                !array_select_policy_refusal));
    ast.produces_sblr = true;
  } else if (keyword == "VALUES") {
    ast.family = StatementFamily::kValues;
    ast.registry_family = "sbsql.query.values.v3";
    ast.operation_family = "sblr.query.relational.v3";
    ast.produces_sblr = true;
  } else if (keyword == "SEARCH") {
    ast.family = StatementFamily::kQuery;
    ast.registry_family = "sbsql.query.multimodel_or_ddl.v3";
    ast.operation_family = "sblr.query.multimodel_or_ddl.v3";
    ast.requires_name_resolution = true;
    ast.produces_sblr = true;
  } else if (keyword == "LLVM" || (keyword == "GPU" && second != "WORKLOAD")) {
    ast.family = StatementFamily::kAcceleration;
    ast.registry_family = "sbsql.acceleration.operation.v3";
    ast.operation_family = "sblr.acceleration.operation.v3";
    ast.requires_name_resolution = false;
    ast.produces_sblr = true;
  } else if ((keyword == "QUERY") ||
             (keyword == "STREAM" && second == "CONSUMER") ||
             (keyword == "GRAPH" && second == "SUBQUERY") ||
             (keyword == "CYPHER" &&
              (second == "WITH" || second == "WHERE" || second == "CALL" ||
               second == "SUBQUERY")) ||
             (keyword == "PSQL" && second == "SELECT") ||
             keyword == "TABLEGROUP" ||
             keyword == "RESOURCE" ||
             (keyword == "CREATE" && second == "RESOURCE" && third == "GROUP") ||
             (keyword == "FOR" && second == "SELECT")) {
    ast.family = StatementFamily::kQuery;
    ast.registry_family = "sbsql.query.relational.v3";
    ast.operation_family = "sblr.query.relational.v3";
    ast.requires_name_resolution = true;
    ast.produces_sblr = true;
  } else if ((keyword == "SHOW" && second == "TRANSACTION" && third == "RUNTIME") ||
             (keyword == "PSQL" && second == "EXECUTE" && third == "BLOCK") ||
             keyword == "AUTONOMOUS" ||
             (keyword == "LOCK" && second == "MODE") ||
             transaction_lock_surface ||
             (keyword == "TRANSACTION" && second == "REF") ||
             (keyword == "PACKAGE" && second == "INIT") ||
             (keyword == "EXECUTE" && second == "BLOCK")) {
    ast.family = StatementFamily::kTransaction;
    ast.registry_family = "sbsql.transaction.control.v3";
    ast.operation_family = "sblr.transaction.control.v3";
    ast.requires_name_resolution = false;
    ast.produces_sblr = true;
  } else if ((keyword == "PSQL" && second == "STATEMENT") ||
             (keyword == "PSQL" && second == "EXECUTE" && third == "STATEMENT") ||
             keyword == "STATEMENT" ||
             (keyword == "STATISTICS" && second == "KIND") ||
             (keyword == "TRUNCATE" && second == "STATEMENT")) {
    ast.family = StatementFamily::kObservability;
    ast.registry_family = "sbsql.observability.inspect.v3";
    ast.operation_family = "sblr.observability.inspect.v3";
    ast.requires_name_resolution = false;
    ast.produces_sblr = true;
  } else if (keyword == "STORAGE" ||
             (keyword == "DISCOVER" &&
              (second == "FILESPACE" || second == "ORPHAN" || second == "STALE")) ||
             ((keyword == "EXPORT" || keyword == "INSPECT" || keyword == "IMPORT" ||
               keyword == "ADMIT" || keyword == "REJECT") &&
              second == "FILESPACE" && third == "PACKAGE") ||
             (keyword == "SHARD" && second == "PLACEMENT") ||
             filespace_lifecycle_target_surface) {
    ast.family = StatementFamily::kStorageManagement;
    ast.registry_family = "sbsql.storage.management_operation.v3";
    ast.operation_family = "sblr.filespace.management.v3";
    ast.requires_name_resolution = filespace_lifecycle_target_surface;
    ast.produces_sblr = true;
  } else if ((keyword == "CREATE" && second == "PROTECTED" && third == "MATERIAL") ||
             (keyword == "ADD" && second == "PROTECTED" && third == "MATERIAL") ||
             (keyword == "ROTATE" && second == "PROTECTED" && third == "MATERIAL") ||
             (keyword == "RESOLVE" && second == "PROTECTED" && third == "MATERIAL") ||
             (keyword == "RELEASE" && second == "PROTECTED" && third == "MATERIAL") ||
             (keyword == "PURGE" && second == "PROTECTED" && third == "MATERIAL") ||
             (keyword == "EXPORT" && second == "PROTECTED" && third == "MATERIAL") ||
             (keyword == "IMPORT" && second == "PROTECTED" && third == "MATERIAL") ||
             (keyword == "SHOW" && second == "PROTECTED" && third == "MATERIAL" &&
              (fourth == "CATALOG" || fourth == "AUDIT"))) {
    ast.family = StatementFamily::kSecurity;
    ast.registry_family = "sbsql.security.v3";
    ast.operation_family = "sblr.security.mutation.v3";
    ast.requires_name_resolution = false;
    ast.produces_sblr = true;
  } else if ((keyword == "ADMIT" && second == "ENCRYPTION" && third == "KEY") ||
             (keyword == "REKEY" && second == "FILESPACE") ||
             (keyword == "ALTER" && second == "FILESPACE" && third == "ENCRYPTION") ||
             (keyword == "SHOW" && second == "PROTECTED" && third == "MATERIAL") ||
             (keyword == "PURGE" && second == "PROTECTED" && third == "MATERIAL") ||
             (keyword == "SHUTDOWN" && second == "PROTECTED" && third == "MATERIAL") ||
             (keyword == "OPEN" && second == "ENCRYPTED" && third == "FILESPACE") ||
             (keyword == "REQUEST" && second == "KEY" && third == "RELEASE") ||
             (keyword == "CRYPTOGRAPHIC" && second == "ERASE" && third == "FILESPACE")) {
    ast.family = StatementFamily::kSecurity;
    ast.registry_family = "sbsql.security.v3";
    ast.operation_family = "sblr.security.mutation.v3";
    ast.produces_sblr = true;
  } else if ((raw_keyword == "CYPHER" &&
              (second == "DELETE" || second == "MERGE" || second == "LOAD")) ||
             (raw_keyword == "GRAPH" && second == "DELETE") ||
             (raw_keyword == "DOCUMENT" && (second == "UPDATE" || second == "BULK")) ||
             (raw_keyword == "GPU" && second == "WORKLOAD") ||
             (raw_keyword == "LOAD" && second == "DATA")) {
    ast.family = StatementFamily::kUpdate;
    ast.registry_family = "sbsql.dml.operation.v3";
    ast.operation_family = "sblr.dml.operation.v3";
    ast.requires_name_resolution = true;
    ast.produces_sblr = true;
  } else if (keyword == "KV") {
    ast.family = StatementFamily::kMultiModel;
    ast.registry_family = "sbsql.query.multimodel_or_ddl.v3";
    ast.operation_family = "sblr.query.multimodel_or_ddl.v3";
    ast.requires_name_resolution = false;
    ast.produces_sblr = true;
  } else if (keyword == "GRAPH" || keyword == "DOCUMENT" ||
             keyword == "TIMESERIES" || (keyword == "TIME" && second == "SERIES") ||
             keyword == "FULLTEXT" || keyword == "OPENSEARCH" ||
             (keyword == "CHANGE" && second == "STREAM")) {
    ast.family = StatementFamily::kMultiModel;
    ast.registry_family = "sbsql.query.multimodel_or_ddl.v3";
    ast.operation_family = "sblr.query.multimodel_or_ddl.v3";
    ast.requires_name_resolution = true;
    ast.produces_sblr = true;
  } else if (keyword == "REINDEX" && second == "VECTOR" && third == "COLLECTION") {
    ast.family = StatementFamily::kMultiModel;
    ast.registry_family = "sbsql.query.multimodel_or_ddl.v3";
    ast.operation_family = "sblr.query.multimodel_or_ddl.v3";
    ast.requires_name_resolution = true;
    ast.produces_sblr = true;
  } else if (raw_keyword == "INSERT") {
    ast.family = StatementFamily::kInsert;
    ast.registry_family = "sbsql.dml.insert.v3";
    ast.operation_family = "sblr.dml.insert.v3";
    ast.requires_name_resolution = true;
    ast.produces_sblr = true;
  } else if (raw_keyword == "UPDATE") {
    ast.family = StatementFamily::kUpdate;
    ast.registry_family = "sbsql.dml.update.v3";
    ast.operation_family = "sblr.dml.update.v3";
    ast.requires_name_resolution = true;
    ast.produces_sblr = true;
  } else if (raw_keyword == "DELETE") {
    ast.family = StatementFamily::kDelete;
    ast.registry_family = "sbsql.dml.delete.v3";
    ast.operation_family = "sblr.dml.delete.v3";
    ast.requires_name_resolution = true;
    ast.produces_sblr = true;
  } else if (raw_keyword == "MERGE") {
    ast.family = StatementFamily::kMerge;
    ast.registry_family = "sbsql.dml.merge.v3";
    ast.operation_family = "sblr.dml.merge.v3";
    ast.requires_name_resolution = true;
    ast.produces_sblr = true;
  } else if (raw_keyword == "UPSERT") {
    ast.family = StatementFamily::kUpsert;
    ast.registry_family = "sbsql.dml.upsert.v3";
    ast.operation_family = "sblr.dml.operation.v3";
    ast.requires_name_resolution = true;
    ast.produces_sblr = true;
  } else if (IsRuntimeManagementStatement(cst)) {
    ast.family = StatementFamily::kRuntimeManagement;
    ast.registry_family = "sbsql.runtime_management.v3";
    ast.operation_family = "sblr.management.runtime_operation.v3";
    ast.requires_name_resolution = false;
    ast.produces_sblr = true;
  } else if ((keyword == "CREATE" && (second == "PRINCIPAL" || second == "POLICY")) ||
             (keyword == "ALTER" && (second == "PRINCIPAL" || second == "POLICY")) ||
             (keyword == "ATTACH" && second == "POLICY") ||
             (keyword == "ACTIVATE" && second == "POLICY") ||
             (keyword == "DEACTIVATE" && second == "POLICY") ||
             (keyword == "VALIDATE" && second == "POLICY") ||
             (keyword == "SHOW" && second == "SECURITY" && third == "POLICY")) {
    ast.family = StatementFamily::kSecurity;
    ast.registry_family = "sbsql.security.v3";
    ast.operation_family =
        second == "PRINCIPAL" ? "sblr.security.mutation.v3"
                              : "sblr.policy.operation.v3";
    ast.requires_name_resolution = true;
    ast.produces_sblr = true;
  } else if (IsDatabaseLifecycleStatement(cst)) {
    ast.family = StatementFamily::kSession;
    ast.registry_family = "sbsql.session.setting.v3";
    ast.operation_family = "sblr.management.runtime_operation.v3";
    ast.requires_name_resolution = false;
    ast.produces_sblr = true;
  } else if ((keyword == "ALTER" && (second == "JOB" || second == "SCHEDULE")) ||
             (keyword == "CREATE" && second == "SCHEDULE")) {
    ast.family = StatementFamily::kCatalog;
    ast.registry_family = "sbsql.catalog.mutation.v3";
    ast.operation_family = "sblr.catalog.mutation.v3";
    ast.requires_name_resolution = false;
    ast.produces_sblr = true;
  } else if (keyword == "CREATE" || keyword == "ALTER" || keyword == "DROP" ||
             keyword == "RENAME" || keyword == "COMMENT" ||
             (keyword == "CYPHER" && second == "CREATE") ||
             (keyword == "REFRESH" && second == "MATERIALIZED" && third == "VIEW")) {
    ast.family = StatementFamily::kCatalog;
    ast.registry_family = "sbsql.catalog.mutation.v3";
    ast.operation_family = "sblr.catalog.mutation.v3";
    ast.requires_name_resolution =
        keyword == "COMMENT" || keyword == "RENAME" ||
        (keyword != "CREATE" && keyword != "CYPHER") ||
        IsCreateIndexStatement(cst) ||
        IsCreateStatisticsStatement(cst) ||
        IsCreateSynonymStatement(cst);
    ast.produces_sblr = true;
  } else if (keyword == "SHOW") {
    ast.family = StatementFamily::kShow;
    if (IsShowCreateStatement(cst)) {
      ast.registry_family = "sbsql.catalog.mutation.v3";
      ast.operation_family = "sblr.catalog.mutation.v3";
      ast.requires_name_resolution = true;
    } else {
      ast.registry_family = "sbsql.observability.inspect.v3";
      ast.operation_family = "sblr.observability.inspect.v3";
    }
    ast.produces_sblr = true;
  } else if (keyword == "ARCHIVE" || keyword == "BACKUP" || keyword == "RESTORE" ||
             keyword == "REPLICATE" || keyword == "REPLICATION" ||
             keyword == "CHANGEFEED") {
    ast.family = StatementFamily::kArchiveReplication;
    ast.registry_family = "sbsql.archive_replication.operation.v3";
    ast.operation_family = "sblr.archive_replication.operation.v3";
    ast.requires_name_resolution = false;
    ast.produces_sblr = true;
  } else if (keyword == "DESCRIBE" || keyword == "EXPLAIN") {
    ast.family = StatementFamily::kObservability;
    ast.registry_family = "sbsql.observability.inspect.v3";
    ast.operation_family = "sblr.observability.inspect.v3";
    ast.requires_name_resolution = keyword == "DESCRIBE";
    ast.produces_sblr = true;
  } else if (keyword == "SET") {
    if (second == "ROLE") {
      ast.family = StatementFamily::kSecurity;
      ast.requires_name_resolution = true;
    } else if (second == "TRANSACTION") {
      ast.family = StatementFamily::kTransaction;
    } else {
      ast.family = StatementFamily::kSession;
      ast.registry_family = "sbsql.session.setting.v3";
      ast.operation_family = "sblr.session.setting.v3";
    }
    ast.produces_sblr = true;
  } else if (keyword == "BEGIN" || keyword == "COMMIT" || keyword == "ROLLBACK" ||
             keyword == "SAVEPOINT" || keyword == "RELEASE") {
    ast.family = StatementFamily::kTransaction;
    ast.registry_family = "sbsql.transaction.control.v3";
    ast.operation_family = "sblr.transaction.control.v3";
    ast.produces_sblr = true;
  } else if (keyword == "PREPARE" || keyword == "DEALLOCATE" ||
             (keyword == "OPEN" && second != "DATABASE") || keyword == "FETCH" || keyword == "CLOSE" ||
             (keyword == "DECLARE" && third == "CURSOR")) {
    ast.family = StatementFamily::kExecute;
    ast.registry_family = "sbsql.execute.v3";
    ast.operation_family = "sblr.general.operation.v3";
    ast.produces_sblr = true;
  } else if (keyword == "EXECUTE") {
    ast.family = StatementFamily::kExecute;
    if (second == "PROCEDURE") {
      ast.registry_family = "sbsql.routine.execute.v3";
      ast.operation_family = "sblr.routine.execute.v3";
      ast.requires_name_resolution = true;
    } else {
      ast.registry_family = "sbsql.execute.v3";
      ast.operation_family = "sblr.general.operation.v3";
    }
    ast.produces_sblr = true;
  } else if (keyword == "CALL" && second != "TARGET" && second != "ARG") {
    ast.family = StatementFamily::kCall;
    ast.registry_family = "sbsql.routine.call.v3";
    ast.operation_family = "sblr.routine.call.v3";
    ast.requires_name_resolution = true;
    ast.produces_sblr = true;
  } else if (keyword == "GRANT" || keyword == "REVOKE") {
    ast.family = StatementFamily::kSecurity;
    ast.requires_name_resolution = true;
    ast.produces_sblr = true;
  } else if (keyword == "LISTEN" || keyword == "UNLISTEN" ||
             keyword == "NOTIFY" || keyword == "POST" ||
             keyword == "SUBSCRIBE" || keyword == "UNSUBSCRIBE") {
    ast.family = StatementFamily::kRuntimeManagement;
    ast.registry_family = "sbsql.general.operation.v3";
    ast.operation_family = "sblr.general.operation.v3";
    ast.requires_name_resolution = true;
    ast.produces_sblr = true;
  } else if (keyword == "CHECKPOINT") {
    ast.family = StatementFamily::kStorageManagement;
    ast.registry_family = "sbsql.storage.management_operation.v3";
    ast.operation_family = "sblr.filespace.management.v3";
    ast.produces_sblr = true;
  } else if ((keyword == "RUN" || keyword == "PAUSE" ||
              keyword == "RESUME" || keyword == "CANCEL") &&
             (second == "JOB" || second == "JOBS")) {
    ast.family = StatementFamily::kJobsScheduler;
    ast.registry_family = "sbsql.jobs_scheduler.v3";
    ast.operation_family = "sblr.jobs.operation.v3";
    ast.requires_name_resolution = false;
    ast.produces_sblr = true;
  } else if ((keyword == "PSQL" &&
              (second == "REPEAT" || second == "FOR" || second == "FORALL" ||
               second == "CALL" || second == "LEAVE" || second == "OPEN" ||
               second == "NULL" || second == "RESIGNAL" || second == "DECLARE" ||
               second == "WHILE" || second == "EMIT" || second == "ASSIGN" ||
               second == "GET" || second == "RETURN" || second == "SIGNAL" ||
               second == "LOOP" || second == "RAISE")) ||
             (keyword == "FORALL") ||
             (keyword == "FOR" && second == "RANGE") ||
             (keyword == "DECLARE" &&
              (second == "SUBROUTINE" || second == "VARIABLE" ||
               second == "EXCEPTION")) ||
             (keyword == "ROUTINE" &&
              (second == "ATTRIBUTE" || second == "BODY")) ||
             (keyword == "SIGNAL") ||
             (keyword == "RAISE") ||
             (keyword == "COLON" && second == "VARIABLE") ||
             (keyword == "EXCEPTION") ||
             (keyword == "CALL" &&
              (second == "TARGET" || second == "ARG")) ||
             keyword == "LVALUE" ||
             (keyword == "VARIABLE" && second == "DECL") ||
             (keyword == "SINGLE" && second == "VAR") ||
             (keyword == "ARG" && second == "LIST") ||
             (keyword == "PARAM" && second == "MODE") ||
             (keyword == "PACKAGE" &&
              (second == "BODY" || second == "NAME")) ||
             (keyword == "INTO" && second == "TARGET") ||
             (keyword == "DIAGNOSTIC" &&
              (second == "FILTER" || second == "FAMILY")) ||
             (keyword == "RETURN" && second == "SHAPE")) {
    ast.family = StatementFamily::kExecute;
    ast.registry_family = "sbsql.general.operation.v3";
    ast.operation_family = "sblr.general.operation.v3";
    ast.requires_name_resolution = false;
    ast.produces_sblr = true;
  } else if (keyword == "PLACEMENT" ||
             keyword == "REGION" ||
             keyword == "SHARD") {
    ast.family = StatementFamily::kClusterPrivate;
    ast.registry_family = "sbsql.cluster.private_operation.v3";
    ast.operation_family = "sblr.cluster.private_operation.v3";
    ast.requires_name_resolution = false;
    ast.produces_sblr = true;
  } else if (keyword == "CLUSTER" || keyword == "NODE" || keyword == "MEMBER" ||
             keyword == "FAILOVER" || keyword == "RECONCILE" ||
             keyword == "TOPOLOGY" || keyword == "THROTTLE") {
    ast.family = StatementFamily::kClusterPrivate;
    ast.registry_family = "sbsql.cluster.private_operation.v3";
    ast.operation_family = "sblr.cluster.private_operation.v3";
    ast.requires_cluster_profile = true;
    ast.exact_refusal_required = true;
    ast.produces_sblr = true;
  } else if (raw_keyword == "COPY") {
    ast.family = StatementFamily::kInsert;
    ast.requires_name_resolution = true;
    ast.produces_sblr = true;
  } else {
    ast.messages.diagnostics.push_back(MakeDiagnostic(
        "SBSQL.PARSER.STATEMENT_FAMILY_UNKNOWN", "ERROR", "statement family is not recognized by the vertical-slice parser",
        "sbp_sbsql.ast", {{"first_token", first->text}}));
  }
  ApplyStatementDescriptorMetadata(
      &ast,
      DescriptorForStatementTokens(
          cst, (raw_keyword == "GRANT" || raw_keyword == "REVOKE") ? raw_keyword : keyword));
  if (ast.family == StatementFamily::kMigration) {
    ast.statement_parser_category = "migration";
    ast.registry_family = "sbsql.migration.operation.v3";
    ast.operation_family = "sblr.migration.operation.v3";
  }
  if (transaction_lock_surface) {
    ast.family = StatementFamily::kTransaction;
    ast.registry_family = "sbsql.transaction.control.v3";
    ast.operation_family = "sblr.transaction.control.v3";
    ast.statement_parser_category = "transaction";
    ast.statement_binding_contract_key = "binder.statement.transaction_context";
    ast.statement_admission_contract_key = "admission.statement.server_revalidation_required";
    ast.statement_behavior_descriptor_key = "behavior.statement.parse_ast_bind_lower_engine_rule";
    ast.diagnostic_key = "diagnostic.canonical_message_vector";
    ast.requires_name_resolution = false;
    ast.produces_sblr = true;
    if (keyword == "LOCK" && second != "NAMED") {
      ast.statement_surface_name = "lock_table";
      ast.statement_surface_id = "SBSQL-35D014F65562";
    } else if (keyword == "LOCK" && second == "NAMED") {
      ast.statement_surface_name = "lock_named";
      ast.statement_surface_id.clear();
    } else if (keyword == "UNLOCK" && second == "TABLE") {
      ast.statement_surface_name = "unlock_table";
      ast.statement_surface_id.clear();
    } else if (keyword == "UNLOCK" && second == "NAMED") {
      ast.statement_surface_name = "unlock_named";
      ast.statement_surface_id.clear();
    }
  }
  if (language_control_surface) {
    ast.registry_family = "sbsql.language.resource_control.v3";
    ast.operation_family = "sblr.language.resource_control.v3";
    ast.statement_parser_category = "language_resource";
    ast.statement_binding_contract_key = "binder.statement.language_resource_control";
    ast.statement_admission_contract_key = "admission.sblr.language_resource_control";
    ast.statement_behavior_descriptor_key =
        "behavior.language_resource.parser_surface_server_revalidation";
    ast.diagnostic_key = "diagnostic.canonical_message_vector";
    ast.requires_name_resolution = false;
    ast.produces_sblr = true;
    if (keyword == "SET") {
      ast.family = StatementFamily::kSession;
      ast.statement_surface_name = "set_language";
      ast.statement_surface_id = "SBSQL-SML008-SET-LANGUAGE";
    } else if (keyword == "RESET") {
      ast.family = StatementFamily::kSession;
      ast.statement_surface_name = "reset_language";
      ast.statement_surface_id = "SBSQL-SML008-RESET-LANGUAGE";
    } else if (keyword == "SHOW") {
      ast.family = StatementFamily::kObservability;
      ast.statement_surface_name = "show_language";
      ast.statement_surface_id = "SBSQL-SML008-SHOW-LANGUAGE";
    } else if (keyword == "LOAD") {
      ast.family = StatementFamily::kRuntimeManagement;
      ast.statement_surface_name = "load_language_bundle";
      ast.statement_surface_id = "SBSQL-SML009-LOAD-LANGUAGE-BUNDLE";
    } else if (keyword == "UNLOAD") {
      ast.family = StatementFamily::kRuntimeManagement;
      ast.statement_surface_name = "unload_language_bundle";
      ast.statement_surface_id = "SBSQL-SML009-UNLOAD-LANGUAGE-BUNDLE";
    } else if (keyword == "VALIDATE") {
      ast.family = StatementFamily::kRuntimeManagement;
      ast.statement_surface_name = "validate_language_bundle";
      ast.statement_surface_id = "SBSQL-SML009-VALIDATE-LANGUAGE-BUNDLE";
    }
  }
  ast.statement_kind = StatementFamilyName(ast.family);
  if (!ast.nodes.empty()) {
    ast.nodes[ast.root_node_index].text = ast.statement_kind;
  }
  return ast;
}

} // namespace scratchbird::parser::sbsql
