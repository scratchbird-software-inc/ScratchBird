// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "wire/sbsql_test_wire.hpp"

#include "ast/ast.hpp"
#include "binder/binder.hpp"
#include "cst/cst.hpp"
#include "embedded/embedded_engine_client.hpp"
#include "ipc/sbps_client.hpp"
#include "lowering/lowering.hpp"
#include "rendering/rendering.hpp"

#include "scratchbird/engine/sblr/lowering.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <sstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <cerrno>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace scratchbird::parser::sbsql {
namespace {

namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

std::uint64_t CurrentUnixMillis() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

std::string NewRowUuid() {
  static std::uint64_t sequence = 0;
  const auto generated =
      uuid::GenerateEngineIdentityV7(UuidKind::row, CurrentUnixMillis() + (++sequence));
  return generated.ok() ? uuid::UuidToString(generated.value.value) : std::string{};
}

std::string AfterCommand(std::string_view line, std::string_view command) {
  auto trimmed = TrimAscii(line);
  if (trimmed.size() <= command.size()) return {};
  return TrimAscii(std::string_view(trimmed).substr(command.size()));
}

bool WriteAll(std::intptr_t fd, std::string_view text) {
  std::size_t written = 0;
  while (written < text.size()) {
#ifdef _WIN32
    const int want = static_cast<int>(std::min<std::size_t>(
        text.size() - written, static_cast<std::size_t>(std::numeric_limits<int>::max())));
    const int rc = ::send(static_cast<SOCKET>(fd), text.data() + written, want, 0);
#else
    const auto rc = ::write(static_cast<int>(fd), text.data() + written, text.size() - written);
#endif
    if (rc > 0) {
      written += static_cast<std::size_t>(rc);
      continue;
    }
#ifndef _WIN32
    if (rc < 0 && errno == EINTR) continue;
#endif
    return false;
  }
  return true;
}

bool ReadLine(std::intptr_t fd, std::string* line) {
  line->clear();
  char ch = 0;
  for (;;) {
#ifdef _WIN32
    const int rc = ::recv(static_cast<SOCKET>(fd), &ch, 1, 0);
#else
    const auto rc = ::read(static_cast<int>(fd), &ch, 1);
#endif
    if (rc == 1) {
      if (ch == '\n') return true;
      if (ch != '\r') line->push_back(ch);
      continue;
    }
#ifndef _WIN32
    if (rc < 0 && errno == EINTR) continue;
#endif
    return !line->empty();
  }
}

class ScopedParserState {
 public:
  ScopedParserState(ParserMetrics* metrics,
                    bool enabled,
                    ParserState active,
                    ParserState fallback)
      : metrics_(metrics), enabled_(enabled), fallback_(fallback) {
    if (metrics_ != nullptr && enabled_) metrics_->SetState(active);
  }
  ~ScopedParserState() {
    if (metrics_ != nullptr && enabled_) metrics_->SetState(fallback_);
  }

 private:
  ParserMetrics* metrics_;
  bool enabled_;
  ParserState fallback_;
};

void ApplyExecutedTransactionState(const ServerExecutionResult& executed,
                                   SessionContext* session) {
  if (session == nullptr || !executed.transaction_state_present) return;
  session->local_transaction_id = executed.local_transaction_id;
  session->snapshot_visible_through_local_transaction_id =
      executed.snapshot_visible_through_local_transaction_id;
  session->transaction_uuid = executed.transaction_uuid;
  session->transaction_timestamp = executed.transaction_timestamp;
  session->transaction_context = "always_active";
}

struct ObjectReference {
  std::string presented_name;
  bool quoted{false};
};

bool IsWord(const Token& token, std::string_view word) {
  return ToUpperAscii(token.text) == ToUpperAscii(word);
}

bool IsLiteralKind(TokenKind kind) {
  return kind == TokenKind::kNumericLiteral ||
         kind == TokenKind::kStringLiteral ||
         kind == TokenKind::kBinaryLiteral ||
         kind == TokenKind::kTemporalLiteral ||
         kind == TokenKind::kUuidLiteral ||
         kind == TokenKind::kBooleanLiteral ||
         kind == TokenKind::kNullLiteral ||
         kind == TokenKind::kDefaultLiteral ||
         kind == TokenKind::kDocumentLiteral ||
         kind == TokenKind::kVectorLiteral ||
         kind == TokenKind::kRegexLiteral ||
         kind == TokenKind::kRangeLiteral;
}

std::string JoinStable(const std::vector<std::string>& values) {
  std::string out;
  for (const auto& value : values) {
    if (!out.empty()) out.push_back(';');
    out += value;
  }
  return out;
}

std::string NormalizeFrontdoorSql(std::string_view sql) {
  std::string out;
  bool in_space = false;
  bool in_string_literal = false;
  bool in_quoted_identifier = false;
  for (std::size_t i = 0; i < sql.size(); ++i) {
    const char ch = sql[i];
    const auto uch = static_cast<unsigned char>(ch);
    if (in_quoted_identifier) {
      out.push_back(ch);
      if (ch == '"') {
        if (i + 1 < sql.size() && sql[i + 1] == '"') {
          out.push_back(sql[i + 1]);
          ++i;
        } else {
          in_quoted_identifier = false;
        }
      }
      continue;
    }
    if (in_string_literal) {
      out.push_back(ch);
      if (ch == '\'') {
        if (i + 1 < sql.size() && sql[i + 1] == '\'') {
          out.push_back(sql[i + 1]);
          ++i;
        } else {
          in_string_literal = false;
        }
      }
      continue;
    }
    if (std::isspace(uch)) {
      in_space = true;
      continue;
    }
    if (in_space && !out.empty()) out.push_back(' ');
    in_space = false;
    if (ch == '\'') {
      in_string_literal = true;
      out.push_back(ch);
      continue;
    }
    if (ch == '"') {
      in_quoted_identifier = true;
      out.push_back(ch);
      continue;
    }
    out.push_back(static_cast<char>(std::toupper(uch)));
  }
  return out;
}

std::string ParameterTypeShape(std::string_view sql) {
  std::string shape;
  for (std::size_t i = 0; i < sql.size(); ++i) {
    const unsigned char ch = static_cast<unsigned char>(sql[i]);
    if (sql[i] == '?') {
      if (!shape.empty()) shape.push_back(';');
      shape += "param";
      continue;
    }
    if (std::isdigit(ch)) {
      if (!shape.empty()) shape.push_back(';');
      shape += "numeric";
      while (i + 1 < sql.size() && std::isdigit(static_cast<unsigned char>(sql[i + 1]))) ++i;
      continue;
    }
    if (sql[i] == '\'') {
      if (!shape.empty()) shape.push_back(';');
      shape += "text";
      while (i + 1 < sql.size()) {
        ++i;
        if (sql[i] == '\'') break;
      }
    }
  }
  return shape.empty() ? "no_parameters" : shape;
}

// SEARCH_KEY: SBSQL_FRONTDOOR_LOWERING_CACHE_ODFR_011
// Parser-owned front-door lowering cache key. It reuses lowering artifacts only;
// execution, authorization, storage access, and transaction finality remain with
// the existing engine route and MGA/security authority.
CacheKey BuildFrontdoorLoweringCacheKey(const ParserConfig& config,
                                        const SessionContext& session,
                                        std::string_view sql) {
  const std::string normalized = NormalizeFrontdoorSql(sql);
  CacheKey key;
  key.shape_hash = Fnv1a64(normalized);
  key.normalized_statement_hash = Fnv1a64(normalized);
  key.registry_version = config.registry_version;
  key.catalog_epoch = session.catalog_epoch;
  key.security_policy_epoch = session.security_policy_epoch;
  key.grant_epoch = session.grant_epoch;
  key.descriptor_epoch = session.descriptor_epoch;
  key.udr_epoch = session.udr_epoch;
  key.name_resolution_epoch = session.catalog_epoch;
  key.resource_epoch = config.resource_budget.max_statement_bytes ^
                       config.resource_budget.max_sblr_envelope_bytes;
  key.parser_package_generation = Fnv1a64(config.bundle_contract_id);
  key.protocol_version = config.protocol_version;
  key.parser_package_version_hash = Fnv1a64(config.build_id);
  key.disclosure_policy_generation = Fnv1a64(session.result_rendering_policy);
  key.redaction_policy_generation = Fnv1a64(session.metric_redaction_policy);
  key.security_authority_epoch = session.security_policy_epoch ^ session.grant_epoch;
  key.cluster_policy_generation = 0;
  key.ttl_generation = 0;
  key.memory_pressure_generation = 0;
  key.parameter_type_shape_hash = Fnv1a64(ParameterTypeShape(sql));
  key.connection_uuid = session.connection_uuid;
  key.transaction_context_hash = std::to_string(Fnv1a64(session.transaction_context));
  key.dialect = config.dialect;
  key.role_set_hash = std::to_string(Fnv1a64(JoinStable(session.effective_role_uuids)));
  key.group_set_hash = std::to_string(Fnv1a64(JoinStable(session.effective_group_uuids)));
  key.search_path_hash = std::to_string(Fnv1a64(JoinStable(session.search_path)));
  key.language_profile = session.default_language;
  key.policy_profile = session.policy_profile_uuid;
  key.parser_profile = config.profile_id;
  key.result_contract_hash =
      std::to_string(Fnv1a64(session.result_rendering_policy + "|" + config.dialect));
  return key;
}

PipelineResult PipelineResultFromCacheEntry(const CacheEntry& entry) {
  PipelineResult result;
  result.accepted = !entry.sblr_payload.empty();
  result.frontdoor_cache_hit = true;
  result.parser_executes_sql = entry.parser_executes_sql;
  result.cached_storage_authority = entry.storage_authority_cached;
  result.cached_authorization_authority = entry.authorization_authority_cached;
  result.cached_finality_authority = entry.finality_authority_cached;
  result.statement_family = entry.statement_family;
  result.operation_family = entry.operation_family;
  result.statement_hash = entry.statement_hash;
  result.sblr_payload = entry.sblr_payload;
  return result;
}

void AddResourceDiagnostic(MessageVectorSet* messages,
                           std::string code,
                           std::string message,
                           std::vector<Field> fields) {
  messages->diagnostics.push_back(MakeDiagnostic(
      std::move(code), "ERROR", std::move(message), "sbp_sbsql.wire",
      std::move(fields)));
}

std::optional<ObjectReference> ExtractFirstObjectReference(const CstDocument& cst) {
  std::size_t marker = cst.tokens.size();
  for (std::size_t i = 0; i < cst.tokens.size(); ++i) {
    const auto& token = cst.tokens[i];
    if (IsTriviaToken(token)) continue;
    if (IsWord(token, "COPY")) {
      marker = i + 1;
    }
    break;
  }
  if (marker == cst.tokens.size()) {
    for (std::size_t i = 0; i < cst.tokens.size(); ++i) {
      const auto& token = cst.tokens[i];
      if (IsTriviaToken(token)) continue;
      if (token.kind != TokenKind::kKeyword && token.kind != TokenKind::kIdentifier) continue;
      if (IsWord(token, "FROM") || IsWord(token, "INTO") || IsWord(token, "UPDATE") ||
          IsWord(token, "TABLE") || IsWord(token, "CALL")) {
        marker = i + 1;
        break;
      }
    }
  }
  if (marker >= cst.tokens.size()) return std::nullopt;
  ObjectReference ref;
  for (std::size_t i = marker; i < cst.tokens.size(); ++i) {
    const auto& token = cst.tokens[i];
    if (IsTriviaToken(token)) continue;
    if (token.kind == TokenKind::kEnd) break;
    if (token.kind == TokenKind::kSymbol && token.text == ".") {
      if (!ref.presented_name.empty() && ref.presented_name.back() != '.') ref.presented_name.push_back('.');
      continue;
    }
    if (token.kind != TokenKind::kIdentifier && token.kind != TokenKind::kKeyword) break;
    if (!ref.presented_name.empty() && ref.presented_name.back() != '.') break;
    ref.presented_name += token.text;
    ref.quoted = ref.quoted || token.quoted;
  }
  if (ref.presented_name.empty()) return std::nullopt;
  return ref;
}

std::optional<ObjectReference> ExtractObjectReferenceAt(const CstDocument& cst,
                                                        std::size_t marker) {
  if (marker >= cst.tokens.size()) return std::nullopt;
  ObjectReference ref;
  for (std::size_t i = marker; i < cst.tokens.size(); ++i) {
    const auto& token = cst.tokens[i];
    if (IsTriviaToken(token)) continue;
    if (token.kind == TokenKind::kEnd) break;
    if (token.kind == TokenKind::kSymbol && token.text == ".") {
      if (!ref.presented_name.empty() && ref.presented_name.back() != '.') ref.presented_name.push_back('.');
      continue;
    }
    if (token.kind != TokenKind::kIdentifier && token.kind != TokenKind::kKeyword) break;
    if (!ref.presented_name.empty() && ref.presented_name.back() != '.') break;
    ref.presented_name += token.text;
    ref.quoted = ref.quoted || token.quoted;
  }
  if (ref.presented_name.empty()) return std::nullopt;
  return ref;
}

std::vector<ObjectReference> ExtractObjectReferences(const CstDocument& cst) {
  std::vector<ObjectReference> refs;
  std::size_t first_token = cst.tokens.size();
  for (std::size_t i = 0; i < cst.tokens.size(); ++i) {
    if (!IsTriviaToken(cst.tokens[i])) {
      first_token = i;
      break;
    }
  }
  if (first_token == cst.tokens.size()) return refs;

  if (IsWord(cst.tokens[first_token], "COPY")) {
    if (auto ref = ExtractObjectReferenceAt(cst, first_token + 1)) refs.push_back(*ref);
    return refs;
  }

  for (std::size_t i = first_token; i < cst.tokens.size(); ++i) {
    const auto& token = cst.tokens[i];
    if (IsTriviaToken(token)) continue;
    if (token.kind == TokenKind::kEnd) break;
    if (!IsWord(token, "FROM") && !IsWord(token, "INTO") &&
        !IsWord(token, "UPDATE") && !IsWord(token, "TABLE") &&
        !IsWord(token, "CALL") && !IsWord(token, "JOIN")) {
      continue;
    }
    auto ref = ExtractObjectReferenceAt(cst, i + 1);
    if (!ref) continue;
    refs.push_back(*ref);
    if (refs.size() >= 8) break;
  }

  if (refs.empty()) {
    if (auto ref = ExtractFirstObjectReference(cst)) refs.push_back(*ref);
  }
  return refs;
}

bool IsIdentifierLikeForRouteExecution(const Token& token) {
  return token.kind == TokenKind::kIdentifier || token.kind == TokenKind::kKeyword;
}

void SkipTriviaTokens(const CstDocument& cst, std::size_t* index) {
  if (index == nullptr) return;
  while (*index < cst.tokens.size() && IsTriviaToken(cst.tokens[*index])) ++(*index);
}

bool ConsumeRouteKeyword(const CstDocument& cst, std::size_t* index, std::string_view keyword) {
  if (index == nullptr) return false;
  SkipTriviaTokens(cst, index);
  if (*index >= cst.tokens.size() || !IsWord(cst.tokens[*index], keyword)) return false;
  ++(*index);
  return true;
}

bool ConsumeRouteSymbol(const CstDocument& cst, std::size_t* index, std::string_view symbol) {
  if (index == nullptr) return false;
  SkipTriviaTokens(cst, index);
  if (*index >= cst.tokens.size() || cst.tokens[*index].text != symbol) return false;
  ++(*index);
  return true;
}

bool ConsumeRouteIdentifier(const CstDocument& cst, std::size_t* index, std::string* text) {
  if (index == nullptr || text == nullptr) return false;
  SkipTriviaTokens(cst, index);
  if (*index >= cst.tokens.size() || !IsIdentifierLikeForRouteExecution(cst.tokens[*index])) {
    return false;
  }
  *text = cst.tokens[*index].text;
  ++(*index);
  return true;
}

bool ConsumeRouteQualifiedNameLeaf(const CstDocument& cst, std::size_t* index, std::string* leaf) {
  if (index == nullptr || leaf == nullptr) return false;
  bool consumed = false;
  bool expect_part = true;
  std::string last;
  for (;;) {
    SkipTriviaTokens(cst, index);
    if (*index >= cst.tokens.size()) break;
    const auto& token = cst.tokens[*index];
    if (token.kind == TokenKind::kEnd || token.kind == TokenKind::kStatementTerminator) break;
    if (expect_part) {
      if (!IsIdentifierLikeForRouteExecution(token)) break;
      last = token.text;
      consumed = true;
      expect_part = false;
      ++(*index);
      continue;
    }
    if (token.text != ".") break;
    expect_part = true;
    ++(*index);
  }
  if (!consumed || expect_part) return false;
  *leaf = std::move(last);
  return true;
}

std::string RouteCanonicalTypeName(std::string_view type_text) {
  const std::string upper = ToUpperAscii(type_text);
  if (upper == "INT" || upper == "INTEGER") return "int";
  if (upper == "BIGINT") return "bigint";
  if (upper == "FLOAT") return "float";
  if (upper == "DOUBLE") return "double";
  if (upper == "TEXT" || upper == "VARCHAR" || upper == "CHAR") return "text";
  std::string lowered(type_text);
  for (auto& ch : lowered) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return lowered;
}

bool ConsumeOptionalTemporaryTablePrefix(const CstDocument& cst,
                                         std::size_t* index,
                                         bool* temporary,
                                         std::string* temporary_scope) {
  if (index == nullptr || temporary == nullptr || temporary_scope == nullptr) {
    return false;
  }
  SkipTriviaTokens(cst, index);
  *temporary = false;
  *temporary_scope = "private";
  if (*index < cst.tokens.size() && IsWord(cst.tokens[*index], "GLOBAL")) {
    ++(*index);
    if (!ConsumeRouteKeyword(cst, index, "TEMPORARY")) return false;
    *temporary = true;
    *temporary_scope = "global";
    return true;
  }
  if (*index < cst.tokens.size() && IsWord(cst.tokens[*index], "LOCAL")) {
    ++(*index);
    SkipTriviaTokens(cst, index);
    if (*index >= cst.tokens.size() ||
        (!IsWord(cst.tokens[*index], "TEMPORARY") &&
         !IsWord(cst.tokens[*index], "TEMP"))) {
      return false;
    }
    ++(*index);
    *temporary = true;
    *temporary_scope = "private";
    return true;
  }
  if (*index < cst.tokens.size() &&
      (IsWord(cst.tokens[*index], "TEMPORARY") ||
       IsWord(cst.tokens[*index], "TEMP"))) {
    ++(*index);
    *temporary = true;
    *temporary_scope = "private";
  }
  return true;
}

bool ConsumeOptionalOnCommitAction(const CstDocument& cst,
                                   std::size_t* index,
                                   std::string* on_commit_action) {
  if (index == nullptr || on_commit_action == nullptr) return false;
  *on_commit_action = "delete_rows";
  SkipTriviaTokens(cst, index);
  if (*index >= cst.tokens.size() || cst.tokens[*index].kind == TokenKind::kEnd ||
      cst.tokens[*index].kind == TokenKind::kStatementTerminator) {
    return true;
  }
  if (!ConsumeRouteKeyword(cst, index, "ON") ||
      !ConsumeRouteKeyword(cst, index, "COMMIT")) {
    return false;
  }
  if (ConsumeRouteKeyword(cst, index, "DELETE")) {
    if (!ConsumeRouteKeyword(cst, index, "ROWS")) return false;
    *on_commit_action = "delete_rows";
    return true;
  }
  if (ConsumeRouteKeyword(cst, index, "PRESERVE")) {
    if (!ConsumeRouteKeyword(cst, index, "ROWS")) return false;
    *on_commit_action = "preserve_rows";
    return true;
  }
  return false;
}

std::string EscapeRouteOperandField(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (const char ch : value) {
    if (ch == '\\' || ch == '\t' || ch == '\n' || ch == '\r') out.push_back('\\');
    if (ch == '\n') {
      out.push_back('n');
    } else if (ch == '\r') {
      out.push_back('r');
    } else if (ch == '\t') {
      out.push_back('t');
    } else {
      out.push_back(ch);
    }
  }
  return out;
}

void AppendRouteTextOperand(std::string* out, std::string_view name, std::string_view value) {
  if (out == nullptr) return;
  *out += "operand=text\t";
  *out += name;
  *out += "\t";
  *out += EscapeRouteOperandField(value);
  *out += "\n";
}

std::optional<std::string> CreateTableRouteExecutionEnvelope(
    const CstDocument& cst,
    std::string_view operation_family) {
  std::size_t index = 0;
  if (!ConsumeRouteKeyword(cst, &index, "CREATE")) return std::nullopt;
  bool temporary = false;
  std::string temporary_scope = "private";
  if (!ConsumeOptionalTemporaryTablePrefix(
          cst, &index, &temporary, &temporary_scope)) {
    return std::nullopt;
  }
  if (!ConsumeRouteKeyword(cst, &index, "TABLE")) return std::nullopt;
  std::string table_name;
  if (!ConsumeRouteQualifiedNameLeaf(cst, &index, &table_name)) return std::nullopt;
  if (!ConsumeRouteSymbol(cst, &index, "(")) return std::nullopt;
  std::string column_name;
  if (!ConsumeRouteIdentifier(cst, &index, &column_name)) return std::nullopt;
  std::string type_name;
  if (!ConsumeRouteIdentifier(cst, &index, &type_name)) return std::nullopt;
  const std::string canonical_type = RouteCanonicalTypeName(type_name);
  if (!ConsumeRouteSymbol(cst, &index, ")")) return std::nullopt;
  std::string on_commit_action = "delete_rows";
  if (temporary &&
      !ConsumeOptionalOnCommitAction(cst, &index, &on_commit_action)) {
    return std::nullopt;
  }
  SkipTriviaTokens(cst, &index);
  while (index < cst.tokens.size() &&
         cst.tokens[index].kind == TokenKind::kStatementTerminator) {
    ++index;
    SkipTriviaTokens(cst, &index);
  }
  if (index < cst.tokens.size() && cst.tokens[index].kind != TokenKind::kEnd) {
    return std::nullopt;
  }

  std::string out;
  out += "operation_id=ddl.create_table\n";
  out += "opcode=SBLR_DDL_CREATE_TABLE\n";
  out += "sblr_operation_family=";
  out += operation_family.empty() ? "sblr.query.multimodel_or_ddl.v3" : operation_family;
  out += "\n";
  out += "result_shape=engine.api.result.v1\n";
  out += "diagnostic_shape=engine.diagnostic.v1\n";
  out += "trace_key=sbsql.parser.live_route.ddl.create_table\n";
  out += "contains_sql_text=false\n";
  out += "parser_resolved_names_to_uuids=true\n";
  out += "requires_security_context=true\n";
  out += "requires_transaction_context=true\n";
  out += "requires_cluster_authority=false\n";
  AppendRouteTextOperand(&out, "target_object_kind", "table");
  AppendRouteTextOperand(&out, "table_name", table_name);
  AppendRouteTextOperand(&out, "column_count", "1");
  AppendRouteTextOperand(&out, "column_0_name", column_name);
  AppendRouteTextOperand(&out, "column_0_type", canonical_type);
  AppendRouteTextOperand(&out, "column_0_descriptor", "type=" + canonical_type);
  AppendRouteTextOperand(&out, "column_0_nullable", "true");
  if (temporary) {
    AppendRouteTextOperand(&out, "temporary", "true");
    AppendRouteTextOperand(&out, "temporary_scope", temporary_scope);
    AppendRouteTextOperand(&out, "on_commit", on_commit_action);
  }
  return out;
}

bool EnforceCstResourceBudget(const CstDocument& cst,
                              const ParserResourceBudget& budget,
                              ParserMetrics* metrics,
                              MessageVectorSet* messages) {
  const auto before = messages->diagnostics.size();
  std::uint64_t token_count = 0;
  std::uint64_t parameter_count = 0;
  std::uint64_t current_depth = 0;
  std::uint64_t max_depth = 0;
  bool emitted_token_count = false;
  bool emitted_identifier = false;
  bool emitted_literal = false;

  for (const auto& token : cst.tokens) {
    if (token.kind == TokenKind::kEnd) continue;
    ++token_count;
    if (!emitted_token_count && token_count > budget.max_token_count) {
      AddResourceDiagnostic(
          messages,
          "SBSQL.RESOURCE.TOKEN_COUNT_EXCEEDED",
          "statement token count exceeds parser resource budget",
          {{"token_count", std::to_string(token_count)},
           {"max_token_count", std::to_string(budget.max_token_count)}});
      emitted_token_count = true;
    }
    if (!emitted_identifier && token.kind == TokenKind::kIdentifier &&
        token.raw_text.size() > budget.max_identifier_bytes) {
      AddResourceDiagnostic(
          messages,
          "SBSQL.RESOURCE.IDENTIFIER_TOO_LARGE",
          "identifier exceeds parser resource budget",
          {{"identifier_bytes", std::to_string(token.raw_text.size())},
           {"max_identifier_bytes", std::to_string(budget.max_identifier_bytes)},
           {"line", std::to_string(token.line)},
           {"column", std::to_string(token.column)}});
      emitted_identifier = true;
    }
    if (!emitted_literal && IsLiteralKind(token.kind) &&
        token.raw_text.size() > budget.max_literal_bytes) {
      AddResourceDiagnostic(
          messages,
          "SBSQL.RESOURCE.LITERAL_TOO_LARGE",
          "literal exceeds parser resource budget",
          {{"literal_bytes", std::to_string(token.raw_text.size())},
           {"max_literal_bytes", std::to_string(budget.max_literal_bytes)},
           {"line", std::to_string(token.line)},
           {"column", std::to_string(token.column)}});
      emitted_literal = true;
    }
    if (token.kind == TokenKind::kParameter) ++parameter_count;
    if (token.kind == TokenKind::kSymbol && token.text == "(") {
      ++current_depth;
      max_depth = std::max(max_depth, current_depth);
    } else if (token.kind == TokenKind::kSymbol && token.text == ")" &&
               current_depth > 0) {
      --current_depth;
    }
  }

  if (parameter_count > budget.max_parameter_count) {
    AddResourceDiagnostic(
        messages,
        "SBSQL.RESOURCE.PARAMETER_COUNT_EXCEEDED",
        "statement parameter count exceeds parser resource budget",
        {{"parameter_count", std::to_string(parameter_count)},
         {"max_parameter_count", std::to_string(budget.max_parameter_count)}});
  }
  if (max_depth > budget.max_ast_depth) {
    AddResourceDiagnostic(
        messages,
        "SBSQL.RESOURCE.AST_DEPTH_EXCEEDED",
        "expression or statement nesting exceeds parser resource budget",
        {{"ast_depth", std::to_string(max_depth)},
         {"max_ast_depth", std::to_string(budget.max_ast_depth)}});
  }

  if (messages->diagnostics.size() != before && metrics != nullptr) {
    metrics->Increment("sys.metrics.parsers.resource.limit_exceeded_total",
                       messages->diagnostics.size() - before);
    metrics->SetGauge("sys.metrics.parsers.resource.last_token_count",
                      static_cast<double>(token_count));
  }
  return messages->diagnostics.size() == before;
}

void InjectStreamRowCount(std::string* payload, std::uint64_t stream_row_count) {
  if (payload == nullptr || stream_row_count == 0) return;
  const auto close = payload->rfind('}');
  if (close == std::string::npos) return;
  payload->insert(close, ",\"stream_row_count\":" + std::to_string(stream_row_count));
}

void InjectCursorFetchWindow(std::string* payload,
                             std::uint64_t max_chunk_rows,
                             std::uint64_t max_chunk_bytes) {
  if (payload == nullptr || max_chunk_rows == 0) return;
  const std::string json_fields =
      ",\"cursor_max_chunk_rows\":" + std::to_string(max_chunk_rows) +
      ",\"cursor_max_chunk_bytes\":" + std::to_string(max_chunk_bytes);
  const auto close = payload->rfind('}');
  if (close != std::string::npos) {
    payload->insert(close, json_fields);
    return;
  }
  if (!payload->empty() && payload->back() != '\n') payload->push_back('\n');
  *payload += "cursor_max_chunk_rows=" + std::to_string(max_chunk_rows) + "\n";
  *payload += "cursor_max_chunk_bytes=" + std::to_string(max_chunk_bytes) + "\n";
}

std::string StripStatementTerminator(std::string sql) {
  sql = TrimAscii(sql);
  while (!sql.empty() && sql.back() == ';') {
    sql.pop_back();
    sql = TrimAscii(sql);
  }
  return sql;
}

std::optional<ServerManagementCommand> ParseServerManagementCommand(std::string_view sql) {
  const auto normalized = ToUpperAscii(StripStatementTerminator(std::string(sql)));
  ServerManagementCommand command;
  command.audit_reason = "sbsql_sbwp_tls_database_lifecycle_route";
  if (normalized == "VERIFY DATABASE") {
    command.operation_key = "verify_database";
    command.operation_id = "lifecycle.verify_database";
  } else if (normalized == "INSPECT DATABASE" || normalized == "DIAGNOSE DATABASE") {
    command.operation_key = normalized.starts_with("INSPECT") ? "inspect_database" : "diagnose_database";
    command.operation_id = "lifecycle.inspect_database";
  } else if (normalized == "SHOW SERVER LIFECYCLE") {
    command.operation_key = "show_server_lifecycle";
    command.operation_id = "lifecycle.show_server_lifecycle";
  } else if (normalized == "SHOW DATABASE SHUTDOWN STATE") {
    command.operation_key = "show_database_shutdown_state";
    command.operation_id = "lifecycle.show_database_shutdown_state";
  } else if (normalized == "SHUTDOWN DATABASE") {
    command.operation_key = "shutdown_database";
    command.operation_id = "lifecycle.shutdown_database";
    command.mode =
        "acknowledgements_satisfied:true;"
        "drain_complete:true";
  } else if (normalized == "SHUTDOWN DATABASE FORCE" || normalized == "FORCE SHUTDOWN DATABASE") {
    command.operation_key = "shutdown_database_force";
    command.operation_id = "lifecycle.shutdown_force";
    command.mode =
        "shutdown_mode:force;"
        "acknowledgements_satisfied:true;"
        "force_termination_policy_uuid:019e0ec6-d13c-7000-8000-000000000013;"
        "recovery_evidence_preserved:true";
  } else if (normalized == "DROP DATABASE" || normalized == "DROP DATABASE LOGICAL" ||
             normalized == "DROP DATABASE LOGICAL PRESERVE") {
    command.operation_key = "drop_database";
    command.operation_id = "lifecycle.drop_database";
    command.mode = "drop_mode:logical";
  } else {
    return std::nullopt;
  }
  return command;
}

std::string ChunkedParserJsonEnvelope(std::size_t parameter_bytes,
                                      std::uint64_t result_rows) {
  std::string out = "{\"envelope\":\"SBLRExecutionEnvelope.v3\",";
  out += "\"operation_family\":\"sblr.query.relational.v3\",";
  out += "\"surface_key\":\"fspe010b4.chunked_payload\",";
  out += "\"sblr_operation_key\":\"op.fspe010b4.chunked_payload\",";
  out += "\"result_shape\":\"rs.fspe010b4.large_result.v1\",";
  out += "\"diagnostic_shape\":\"diag.fspe010b4.v1\",";
  out += "\"resource_contract\":\"resource.fspe010b4.v1\",";
  out += "\"trace_key\":\"FSPE-010B4\",";
  out += "\"stream_row_count\":";
  out += std::to_string(result_rows);
  out += ",\"source_payload_embedded\":false,";
  out += "\"resolved_object_uuids\":[],\"descriptor_refs\":[],\"policy_refs\":[],";
  out += "\"parameter_packet\":\"";
  out.append(parameter_bytes, 'x');
  out += "\"}";
  return out;
}

std::string CopyStreamParserJsonEnvelope(std::string_view kind,
                                         std::uint64_t total_rows,
                                         std::uint64_t reject_rows) {
  std::string out = "{\"envelope\":\"SBLRExecutionEnvelope.v3\",";
  out += "\"operation_family\":\"sblr.dml.operation.v3\",";
  out += "\"surface_key\":\"fspe010b5.copy_streaming\",";
  out += "\"sblr_operation_key\":\"op.fspe010b5.copy_streaming\",";
  out += "\"result_shape\":\"rs.fspe010b5.copy_stream.v1\",";
  out += "\"diagnostic_shape\":\"diag.fspe010b5.v1\",";
  out += "\"resource_contract\":\"resource.fspe010b5.v1\",";
  out += "\"trace_key\":\"FSPE-010B5\",";
  out += "\"source_payload_embedded\":false,";
  out += "\"resolved_object_uuids\":[\"019e05df-f010-7000-8000-000000000055\"],";
  out += "\"descriptor_refs\":[\"descriptor.copy.target.uuid\"],";
  out += "\"policy_refs\":[\"policy.copy.reject_row\"],";
  out += "\"copy_stream_kind\":\"";
  out += EscapeJson(kind);
  out += "\",\"copy_total_rows\":";
  out += std::to_string(total_rows);
  out += ",\"copy_reject_rows\":";
  out += std::to_string(reject_rows);
  out += "}";
  return out;
}

std::string MultiResultParserJsonEnvelope(std::uint64_t result_sets) {
  std::string out = "{\"envelope\":\"SBLRExecutionEnvelope.v3\",";
  out += "\"operation_family\":\"sblr.query.relational.v3\",";
  out += "\"surface_key\":\"fspe010b6.multi_result\",";
  out += "\"sblr_operation_key\":\"op.fspe010b6.multi_result\",";
  out += "\"result_shape\":\"rs.fspe010b6.multi_result.v1\",";
  out += "\"diagnostic_shape\":\"diag.fspe010b6.v1\",";
  out += "\"resource_contract\":\"resource.fspe010b6.v1\",";
  out += "\"trace_key\":\"FSPE-010B6\",";
  out += "\"source_payload_embedded\":false,";
  out += "\"resolved_object_uuids\":[\"019e05df-f010-7000-8000-000000000066\"],";
  out += "\"descriptor_refs\":[\"descriptor.multi_result.sequence\"],";
  out += "\"policy_refs\":[\"policy.multi_result.forward_only\"],";
  out += "\"multi_result_count\":";
  out += std::to_string(result_sets);
  out += "}";
  return out;
}

std::string WarningStreamParserJsonEnvelope(std::uint64_t partial_rows,
                                            std::uint64_t warnings) {
  std::string out = "{\"envelope\":\"SBLRExecutionEnvelope.v3\",";
  out += "\"operation_family\":\"sblr.query.relational.v3\",";
  out += "\"surface_key\":\"fspe010b7.warning_partial\",";
  out += "\"sblr_operation_key\":\"op.fspe010b7.warning_partial\",";
  out += "\"result_shape\":\"rs.fspe010b7.warning_partial.v1\",";
  out += "\"diagnostic_shape\":\"diag.fspe010b7.v1\",";
  out += "\"resource_contract\":\"resource.fspe010b7.v1\",";
  out += "\"trace_key\":\"FSPE-010B7\",";
  out += "\"source_payload_embedded\":false,";
  out += "\"resolved_object_uuids\":[\"019e05df-f010-7000-8000-000000000077\"],";
  out += "\"descriptor_refs\":[\"descriptor.partial_result.warning_chain\"],";
  out += "\"policy_refs\":[\"policy.partial_result.forward_only\"],";
  out += "\"partial_result_rows\":";
  out += std::to_string(partial_rows);
  out += ",\"warning_chain_count\":";
  out += std::to_string(warnings);
  out += "}";
  return out;
}

std::string FinalityStreamParserJsonEnvelope(std::string_view mode,
                                             std::uint64_t rows,
                                             std::uint64_t after_fetches) {
  std::string out = "{\"envelope\":\"SBLRExecutionEnvelope.v3\",";
  out += "\"operation_family\":\"sblr.query.relational.v3\",";
  out += "\"surface_key\":\"fspe010b8.stream_finality\",";
  out += "\"sblr_operation_key\":\"op.fspe010b8.stream_finality\",";
  out += "\"result_shape\":\"rs.fspe010b8.stream_finality.v1\",";
  out += "\"diagnostic_shape\":\"diag.fspe010b8.v1\",";
  out += "\"resource_contract\":\"resource.fspe010b8.v1\",";
  out += "\"trace_key\":\"FSPE-010B8\",";
  out += "\"source_payload_embedded\":false,";
  out += "\"resolved_object_uuids\":[\"019e05df-f010-7000-8000-000000000088\"],";
  out += "\"descriptor_refs\":[\"descriptor.stream.finality\"],";
  out += "\"policy_refs\":[\"policy.stream.finality.forward_only\"],";
  out += "\"stream_row_count\":";
  out += std::to_string(rows);
  out += ",\"stream_finality_mode\":\"";
  out += EscapeJson(mode);
  out += "\",\"stream_finality_after_fetches\":";
  out += std::to_string(after_fetches);
  out += "}";
  return out;
}

std::string RoutineCursorArgumentJsonEnvelope(std::string_view cursor_uuid) {
  std::string out = "{\"envelope\":\"SBLRExecutionEnvelope.v3\",";
  out += "\"operation_family\":\"sblr.routine.execute.v3\",";
  out += "\"operation_id\":\"routine.execute_cursor_argument\",";
  out += "\"surface_key\":\"routine_cursor_argument.live_route\",";
  out += "\"sblr_operation_key\":\"routine_cursor_argument.live_route\",";
  out += "\"result_shape\":\"routine_cursor_argument.rows.v1\",";
  out += "\"diagnostic_shape\":\"routine_cursor_argument.diag.v1\",";
  out += "\"resource_contract\":\"routine_cursor_argument.resource.v1\",";
  out += "\"trace_key\":\"ROUTINE-CURSOR-FULL-ROUTE\",";
  out += "\"source_payload_embedded\":false,";
  out += "\"routine_cursor_uuid\":\"";
  out += EscapeJson(cursor_uuid);
  out += "\",\"routine_context_kind\":\"procedure\",";
  out += "\"routine_cursor_action\":\"fetch\",";
  out += "\"routine_cursor_borrow_policy\":\"borrowed_read\",";
  out += "\"routine_cursor_argument_binding\":\"descriptor.cursor_handle.session_registry\",";
  out += "\"routine_cursor_descriptor\":\"rowshape:int64:value\",";
  out += "\"routine_expected_cursor_descriptor\":\"rowshape:int64:value\",";
  out += "\"routine_security_recheck\":\"passed\",";
  out += "\"routine_protected_material_policy\":\"rechecked\",";
  out += "\"routine_deterministic_context\":false,";
  out += "\"routine_cursor_fetch_max_rows\":1,";
  out += "\"resolved_object_uuids\":[],";
  out += "\"descriptor_refs\":[\"sys.server.cursor_descriptor\",";
  out += "\"sys.routine.cursor_parameter_descriptor\"],";
  out += "\"policy_refs\":[\"routine_cursor_session_registry_policy\",";
  out += "\"routine_cursor_security_recheck_policy\"]}";
  return out;
}

std::string EngineShowVersionOperationEnvelope() {
  std::string out;
  out += "operation_id=observability.show_version\n";
  out += "opcode=SBLR_OBSERVABILITY_SHOW_VERSION\n";
  out += "sblr_operation_family=sblr.observability.inspect.v3\n";
  out += "result_shape=engine.api.result.v1\n";
  out += "diagnostic_shape=engine.diagnostic.v1\n";
  out += "trace_key=FSPE-010B3\n";
  out += "contains_sql_text=false\n";
  out += "parser_resolved_names_to_uuids=true\n";
  out += "requires_security_context=true\n";
  out += "requires_transaction_context=false\n";
  out += "requires_cluster_authority=false\n";
  const auto binary = scratchbird::engine::sblr::EnvelopeBuilder()
                          .operation(scratchbird::engine::SblrOperationFamily::management_inspect, 1)
                          .append_bytes(reinterpret_cast<const std::uint8_t*>(out.data()), out.size())
                          .encode();
  return std::string(reinterpret_cast<const char*>(binary.data()), binary.size());
}

std::string ExactOperationEnvelope(std::string_view operation_id,
                                   std::string_view opcode,
                                   std::string_view family,
                                   bool requires_transaction_context,
                                   std::string_view trace_key) {
  std::string out;
  out += "operation_id=";
  out += operation_id;
  out += "\n";
  out += "opcode=";
  out += opcode;
  out += "\n";
  out += "sblr_operation_family=";
  out += family;
  out += "\n";
  out += "result_shape=engine.api.result.v1\n";
  out += "diagnostic_shape=engine.diagnostic.v1\n";
  out += "trace_key=";
  out += trace_key;
  out += "\n";
  out += "contains_sql_text=false\n";
  out += "parser_resolved_names_to_uuids=true\n";
  out += "requires_security_context=true\n";
  out += "requires_transaction_context=";
  out += requires_transaction_context ? "true\n" : "false\n";
  out += "requires_cluster_authority=false\n";
  return out;
}

std::string TransactionBeginOperationEnvelope() {
  return ExactOperationEnvelope("transaction.begin",
                                "SBLR_TRANSACTION_BEGIN",
                                "sblr.transaction.control.v3",
                                false,
                                "SBSFC-021-copy-stream-full-route-begin");
}

std::string TransactionCommitOperationEnvelope() {
  return ExactOperationEnvelope("transaction.commit",
                                "SBLR_TRANSACTION_COMMIT",
                                "sblr.transaction.control.v3",
                                true,
                                "SBSFC-021-copy-stream-full-route-commit");
}

std::string TransactionRollbackOperationEnvelope() {
  return ExactOperationEnvelope("transaction.rollback",
                                "SBLR_TRANSACTION_ROLLBACK",
                                "sblr.transaction.control.v3",
                                true,
                                "SBSFC-021-copy-stream-full-route-rollback");
}

std::string EngineBackedCopyStreamImportEnvelope(std::string_view target_object_uuid) {
  const auto good_row_uuid = NewRowUuid();
  const auto reject_row_uuid = NewRowUuid();
  std::string out = ExactOperationEnvelope("dml.execute_import_rows",
                                           "SBLR_DML_EXECUTE_IMPORT_ROWS",
                                           "sblr.dml.operation.v3",
                                           true,
                                           "SBSFC-021-copy-stream-full-route-import");
  out += "copy_stream_kind=copy_import\n";
  out += "target_object_uuid=";
  out += target_object_uuid;
  out += "\n";
  out += "target_object_kind=table\n";
  out += "source_kind=csv_stream\n";
  out += "source_fingerprint=sbsfc021-copy-stream-full-route\n";
  out += "source_position=row:0\n";
  out += "format_family=csv\n";
  out += "encoding=utf8\n";
  out += "line_ending=lf\n";
  out += "delimiter=,\n";
  out += "quote=\"\n";
  out += "escape=\"\n";
  out += "header_policy=absent\n";
  out += "estimated_row_count=2\n";
  out += "duplicate_mode=error\n";
  out += "require_generated_row_uuid=true\n";
  out += "reject_mode=reject_row\n";
  out += "reject_limit_rows=10\n";
  out += "reject_payload_policy=diagnostic_only\n";
  out += "resume_policy=fail_closed\n";
  out += "checkpoint_mode=disabled\n";
  out += "operand=row_field\t";
  out += good_row_uuid;
  out += "|id\t8\n";
  out += "operand=row_field\t";
  out += good_row_uuid;
  out += "|payload\tstream-valid\n";
  out += "operand=row_field\t";
  out += reject_row_uuid;
  out += "|id\t6\n";
  out += "operand=row_field\t";
  out += reject_row_uuid;
  out += "|payload\tstream-duplicate\n";
  return out;
}

} // namespace

SbsqlTestWireSession::SbsqlTestWireSession(ParserConfig config, ParserMetrics* metrics, SblrTemplateCache* cache)
    : config_(std::move(config)), metrics_(metrics), cache_(cache) {
  if (config_.embedded_engine_direct) {
    embedded_client_ = std::make_unique<EmbeddedEngineClient>(config_);
  }
}

SbsqlTestWireSession::~SbsqlTestWireSession() = default;

bool SbsqlTestWireSession::HasExecutionRoute() const {
  return config_.embedded_engine_direct || !config_.server_endpoint.empty();
}

ServerExecutionResult SbsqlTestWireSession::ExecuteSblrOnRoute(
    std::string_view encoded_sblr_envelope,
    bool cursor_requested) {
  if (config_.embedded_engine_direct && embedded_client_ != nullptr) {
    return embedded_client_->ExecuteSblr(session_, encoded_sblr_envelope, cursor_requested);
  }
  SbpsClient client(config_.server_endpoint);
  return client.ExecuteSblr(session_, encoded_sblr_envelope, cursor_requested);
}

ServerFetchResult SbsqlTestWireSession::FetchCursorOnRoute(std::string_view cursor_uuid,
                                                           std::uint64_t max_rows,
                                                           std::uint64_t max_bytes,
                                                           std::uint32_t fetch_flags) {
  if (config_.embedded_engine_direct && embedded_client_ != nullptr) {
    return embedded_client_->FetchCursor(session_, cursor_uuid, max_rows, max_bytes, fetch_flags);
  }
  SbpsClient client(config_.server_endpoint);
  return client.FetchCursor(session_, cursor_uuid, max_rows, max_bytes, fetch_flags);
}

ServerCloseCursorResult SbsqlTestWireSession::CloseCursorOnRoute(std::string_view cursor_uuid) {
  if (config_.embedded_engine_direct && embedded_client_ != nullptr) {
    return embedded_client_->CloseCursor(session_, cursor_uuid);
  }
  SbpsClient client(config_.server_endpoint);
  return client.CloseCursor(session_, cursor_uuid);
}

ServerCloseCursorResult SbsqlTestWireSession::CancelCursorOnRoute(std::string_view cursor_uuid) {
  if (config_.embedded_engine_direct && embedded_client_ != nullptr) {
    return embedded_client_->CancelCursor(session_, cursor_uuid);
  }
  SbpsClient client(config_.server_endpoint);
  return client.CancelCursor(session_, cursor_uuid);
}

PublicNameResolutionResult SbsqlTestWireSession::ResolveNameOnRoute(
    std::string_view presented_name,
    bool quoted,
    std::string_view object_class) {
  if (config_.embedded_engine_direct && embedded_client_ != nullptr) {
    return embedded_client_->ResolveNamePublic(session_, presented_name, quoted, object_class, config_);
  }
  SbpsClient client(config_.server_endpoint);
  return client.ResolveNamePublic(session_, presented_name, quoted, object_class, config_);
}

bool SbsqlTestWireSession::DisconnectExecutionRoute(MessageVectorSet* messages) {
  if (!session_.authenticated) return true;
  if (config_.embedded_engine_direct && embedded_client_ != nullptr) {
    return embedded_client_->DisconnectSession(session_, messages);
  }
  if (!config_.server_endpoint.empty()) {
    SbpsClient client(config_.server_endpoint);
    return client.DisconnectSession(session_, messages);
  }
  return true;
}

PipelineResult SbsqlTestWireSession::RunServerManagementCommand(
    const ServerManagementCommand& command) {
  ScopedParserState active(metrics_,
                           session_.authenticated && HasExecutionRoute(),
                           ParserState::kActive,
                           ParserState::kAuthenticated);
  PipelineResult result;
  result.statement_family = "runtime_management";
  result.operation_family = "sblr.management.runtime_operation.v3";
  result.server_operation_id = command.operation_id;
  if (!HasExecutionRoute()) {
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "SBSQL.SERVER.UNAVAILABLE",
        "ERROR",
        "server lifecycle management requires an execution route",
        "sbp_sbsql.wire"));
    return result;
  }
  if (!session_.authenticated) {
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "SBSQL.AUTH.REQUIRED",
        "ERROR",
        "server lifecycle management requires an authenticated server session",
        "sbp_sbsql.wire"));
    return result;
  }
  ServerManagementResult managed;
  if (config_.embedded_engine_direct && embedded_client_ != nullptr) {
    managed = embedded_client_->Manage(session_,
                                       command.operation_key,
                                       "",
                                       command.mode,
                                       command.audit_reason,
                                       30000,
                                       false);
  } else {
    SbpsClient client(config_.server_endpoint);
    managed = client.Manage(session_,
                            command.operation_key,
                            "",
                            command.mode,
                            command.audit_reason,
                            30000,
                            false);
  }
  if (!managed.accepted) {
    result.messages = managed.messages;
    return result;
  }
  result.accepted = true;
  result.server_row_count = 1;
  result.server_result_payload =
      "row[0]=operation_key=" + command.operation_key +
      ";operation_id=" + command.operation_id +
      ";route=sbwp_tls_listener_parser_sbps_server_engine" +
      ";accepted=true;payload_bytes=" + std::to_string(managed.payload.size()) + "\n";
  return result;
}

PipelineResult SbsqlTestWireSession::RunPipeline(std::string_view sql,
                                                 bool submit,
                                                 bool cursor_requested,
                                                 std::uint64_t stream_row_count) {
  if (metrics_) metrics_->Increment("sys.metrics.parsers.parse_pipeline.attempts_total");
  ScopedParserState active(metrics_,
                           submit && session_.authenticated && HasExecutionRoute(),
                           ParserState::kActive,
                           ParserState::kAuthenticated);
  if (auto management = ParseServerManagementCommand(sql)) {
    return RunServerManagementCommand(*management);
  }
  if (sql.size() > config_.resource_budget.max_statement_bytes) {
    if (metrics_) {
      metrics_->Increment("sys.metrics.parsers.resource.limit_exceeded_total");
      metrics_->SetGauge("sys.metrics.parsers.resource.last_statement_bytes",
                         static_cast<double>(sql.size()));
    }
    PipelineResult result;
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "SBSQL.RESOURCE.STATEMENT_TOO_LARGE",
        "ERROR",
        "statement exceeds parser resource budget",
        "sbp_sbsql.wire"));
    return result;
  }
  const auto frontdoor_cache_key = BuildFrontdoorLoweringCacheKey(config_, session_, sql);
  if (!submit && cache_ != nullptr) {
    if (metrics_) metrics_->Increment("sys.metrics.parsers.frontdoor_cache.attempts_total");
    if (auto cached = cache_->LookupEntry(frontdoor_cache_key)) {
      if (metrics_) {
        metrics_->Increment("sys.metrics.parsers.frontdoor_cache.hits_total");
        metrics_->Increment("sys.metrics.parsers.frontdoor_cache.parse_lower_skips_total");
      }
      return PipelineResultFromCacheEntry(*cached);
    }
    if (metrics_) metrics_->Increment("sys.metrics.parsers.frontdoor_cache.misses_total");
  }
  auto cst = BuildCst(sql);
  MessageVectorSet resource_messages = cst.messages;
  if (!EnforceCstResourceBudget(cst, config_.resource_budget, metrics_,
                                &resource_messages)) {
    PipelineResult result;
    result.accepted = false;
    result.statement_hash = Fnv1a64(cst.source);
    result.messages = std::move(resource_messages);
    return result;
  }
  auto ast = BuildAst(cst);
  std::vector<std::string> resolved_object_uuids;
  PipelineResult result;
  result.statement_family = StatementFamilyName(ast.family);
  result.operation_family = ast.operation_family;
  result.statement_hash = Fnv1a64(cst.source);
  result.messages = ast.messages;
  if (!result.messages.has_errors() && ast.requires_name_resolution &&
      HasExecutionRoute() && session_.authenticated) {
    const auto refs = ExtractObjectReferences(cst);
    if (refs.empty()) {
      result.messages.diagnostics.push_back(MakeDiagnostic(
          "SBSQL.NAME_RESOLUTION.REFERENCE_MISSING",
          "ERROR",
          "statement requires an object reference but no resolvable name was found",
          "sbp_sbsql.wire"));
    } else {
      for (const auto& ref : refs) {
        auto resolved = ResolveNameOnRoute(ref.presented_name, ref.quoted, "relation");
        if (!resolved.resolved) {
          result.messages = std::move(resolved.messages);
          break;
        }
        resolved_object_uuids.push_back(resolved.object_uuid);
        session_.catalog_epoch = std::max(session_.catalog_epoch, resolved.catalog_epoch);
        session_.security_policy_epoch = std::max(session_.security_policy_epoch, resolved.security_epoch);
      }
    }
  }
  if (result.messages.has_errors()) {
    result.accepted = false;
    return result;
  }
  auto bound = BindAst(ast, cst, config_, session_, resolved_object_uuids);
  auto lowered = LowerToSblr(bound, cst, session_);
  if (!lowered.payload.empty() &&
      lowered.payload.size() > config_.resource_budget.max_sblr_envelope_bytes) {
    lowered.messages.diagnostics.push_back(MakeDiagnostic(
        "SBSQL.RESOURCE.SBLR_ENVELOPE_TOO_LARGE",
        "ERROR",
        "lowered SBLR envelope exceeds parser resource budget",
        "sbp_sbsql.wire",
        {{"sblr_envelope_bytes", std::to_string(lowered.payload.size())},
         {"max_sblr_envelope_bytes",
          std::to_string(config_.resource_budget.max_sblr_envelope_bytes)}}));
  }
  result.accepted = !lowered.messages.has_errors() && !lowered.payload.empty();
  result.parser_executes_sql = false;
  result.cached_storage_authority = false;
  result.cached_authorization_authority = false;
  result.cached_finality_authority = false;
  result.statement_family = StatementFamilyName(ast.family);
  result.operation_family = lowered.operation_family;
  result.statement_hash = lowered.statement_hash;
  result.sblr_payload = lowered.payload;
  if (cursor_requested) {
    if (stream_row_count != 0) {
      InjectStreamRowCount(&result.sblr_payload, stream_row_count);
    } else {
      InjectCursorFetchWindow(&result.sblr_payload, 1024, 4u * 1024u * 1024u);
    }
  }
  result.messages = std::move(lowered.messages);
  if (!submit && result.accepted && cache_ != nullptr) {
    CacheEntry entry;
    entry.key = BuildFrontdoorLoweringCacheKey(config_, session_, sql);
    entry.sblr_payload = result.sblr_payload;
    entry.statement_family = result.statement_family;
    entry.operation_family = result.operation_family;
    entry.statement_hash = result.statement_hash;
    entry.parser_executes_sql = false;
    entry.storage_authority_cached = false;
    entry.authorization_authority_cached = false;
    entry.finality_authority_cached = false;
    cache_->StoreEntry(std::move(entry));
    if (metrics_) metrics_->Increment("sys.metrics.parsers.frontdoor_cache.stores_total");
  }
  if (submit && result.accepted) {
    if (!HasExecutionRoute()) {
      result.accepted = false;
      result.messages.diagnostics.push_back(MakeDiagnostic(
          "SBSQL.SERVER.UNAVAILABLE", "ERROR", "SBLR submission requires an execution route",
          "sbp_sbsql.wire"));
    } else if (!session_.authenticated) {
      result.accepted = false;
      result.messages.diagnostics.push_back(MakeDiagnostic(
          "SBSQL.AUTH.REQUIRED", "ERROR", "SBLR submission requires an authenticated server session",
          "sbp_sbsql.wire"));
    } else {
      std::string execution_payload = result.sblr_payload;
      if (auto create_table_execution =
              CreateTableRouteExecutionEnvelope(cst, result.operation_family)) {
        execution_payload = std::move(*create_table_execution);
      }
      const auto executed = ExecuteSblrOnRoute(execution_payload, cursor_requested);
      if (!executed.accepted) {
        result.accepted = false;
        result.messages = executed.messages;
      } else {
        result.server_operation_id = executed.operation_id;
        result.server_cursor_uuid = executed.cursor_uuid;
        result.server_row_count = executed.row_count;
        result.server_result_payload = executed.row_packet;
        ApplyExecutedTransactionState(executed, &session_);
      }
    }
  }
  return result;
}

PipelineResult SbsqlTestWireSession::RunSblrEnvelope(std::string_view encoded_sblr_envelope,
                                                     bool cursor_requested) {
  PipelineResult result;
  result.accepted = false;
  if (!HasExecutionRoute()) {
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "SBSQL.SERVER.UNAVAILABLE", "ERROR", "SBLR submission requires an execution route",
        "sbp_sbsql.wire"));
    return result;
  }
  if (!session_.authenticated) {
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "SBSQL.AUTH.REQUIRED", "ERROR", "SBLR submission requires an authenticated server session",
        "sbp_sbsql.wire"));
    return result;
  }
  const auto executed = ExecuteSblrOnRoute(encoded_sblr_envelope, cursor_requested);
  if (!executed.accepted) {
    result.messages = executed.messages;
    return result;
  }
  result.accepted = true;
  result.server_operation_id = executed.operation_id;
  result.server_cursor_uuid = executed.cursor_uuid;
  result.server_row_count = executed.row_count;
  result.server_result_payload = executed.row_packet;
  ApplyExecutedTransactionState(executed, &session_);
  return result;
}

WireResponse SbsqlTestWireSession::HandleLine(std::string_view line) {
  const auto trimmed = TrimAscii(line);
  const auto upper = ToUpperAscii(trimmed);
  if (upper.empty()) return {false, "OK EMPTY\n"};
  if (upper == "QUIT" || upper == "EXIT") return {true, "OK BYE\n"};
  if (upper == "PING") return {false, "OK PONG\n"};
  if (upper == "HEARTBEAT") {
    return {false, "HEARTBEAT " + metrics_->HeartbeatJson(config_, session_, *cache_, "idle") + "\n"};
  }
  if (upper == "METRICS") {
    return {false, "METRICS " + metrics_->SnapshotJson(config_, session_, *cache_) + "\n"};
  }
  if (upper == "FLUSH CACHE") {
    cache_->Flush();
    return {false, "OK CACHE_FLUSHED\n"};
  }
  if (upper.starts_with("AUTH")) {
    if (metrics_) {
      metrics_->SetState(ParserState::kAuthenticating);
      metrics_->Increment("sys.metrics.parsers.auth.attempts_total");
    }
    AuthRelayRequest request;
    request.provider_id = "sbsql_test";
    request.payload = AfterCommand(trimmed, "AUTH");
    AuthRelayResult result;
    if (config_.embedded_engine_direct && embedded_client_ != nullptr) {
      AuthCredentialEnvelope credentials;
      credentials.principal = "sysarch";
      credentials.requested_database = config_.embedded_database_path.empty()
                                           ? config_.database_token
                                           : config_.embedded_database_path;
      credentials.requested_language = "en";
      credentials.application_name = "sbp_sbsql_line";
      result.accepted =
          embedded_client_->AuthenticateAndAttachSysarch(credentials, &session_, &result.messages);
    } else if (!config_.server_endpoint.empty()) {
      SbpsClient client(config_.server_endpoint);
      result.accepted = client.AuthenticateAndAttach(request.payload, config_, &session_, &result.messages);
    } else {
      result = config_.allow_probe_auth || config_.probe_mode ? ProbeAuthRelay(request, config_) : FailClosedAuthRelay(request, config_);
      if (result.accepted) session_ = std::move(result.session);
    }
    if (result.accepted || session_.authenticated) {
      if (metrics_) metrics_->SetState(ParserState::kAuthenticated);
      return {false, RenderMessageVectorSet(result.messages) + "OK AUTHENTICATED\n"};
    }
    if (metrics_) {
      metrics_->Increment("sys.metrics.parsers.auth.failures_total");
      metrics_->SetState(ParserState::kIdlePreAuth);
    }
    return {false, RenderMessageVectorSet(result.messages)};
  }
  if (upper.starts_with("PARSE ")) {
    const auto source = trimmed.substr(6);
    if (source.size() > config_.resource_budget.max_statement_bytes) {
      if (metrics_) {
        metrics_->Increment("sys.metrics.parsers.resource.limit_exceeded_total");
        metrics_->SetGauge("sys.metrics.parsers.resource.last_statement_bytes",
                           static_cast<double>(source.size()));
      }
      MessageVectorSet messages;
      messages.diagnostics.push_back(MakeDiagnostic(
          "SBSQL.RESOURCE.STATEMENT_TOO_LARGE",
          "ERROR",
          "statement exceeds parser resource budget",
          "sbp_sbsql.wire"));
      return {false, RenderMessageVectorSet(messages)};
    }
    auto cst = BuildCst(source);
    MessageVectorSet resource_messages = cst.messages;
    if (!EnforceCstResourceBudget(cst, config_.resource_budget, metrics_,
                                  &resource_messages)) {
      return {false, RenderMessageVectorSet(resource_messages)};
    }
    auto ast = BuildAst(cst);
    if (ast.messages.has_errors()) return {false, RenderMessageVectorSet(ast.messages)};
    return {false, "OK PARSED " + StatementFamilyName(ast.family) + "\n"};
  }
  if (upper.starts_with("EXECUTE ")) {
    auto result = RunPipeline(trimmed.substr(8), true);
    return {false, RenderPipelineResult(result)};
  }
  if (upper.starts_with("STREAM ")) {
    std::string stream_body = AfterCommand(trimmed, "STREAM");
    std::uint64_t stream_rows = 5;
    char* end = nullptr;
    const auto parsed = std::strtoull(stream_body.c_str(), &end, 10);
    if (end != nullptr && *end == ' ' && parsed > 0) {
      stream_rows = static_cast<std::uint64_t>(parsed);
      stream_body = TrimAscii(end);
    }
    auto result = RunPipeline(stream_body, true, true, stream_rows);
    if (result.accepted && !result.server_cursor_uuid.empty()) {
      last_cursor_uuid_ = result.server_cursor_uuid;
    }
    return {false, RenderPipelineResult(result)};
  }
  if (upper == "ENGINE STREAM") {
    MessageVectorSet messages;
    if (!HasExecutionRoute()) {
      messages.diagnostics.push_back(MakeDiagnostic(
          "SBSQL.SERVER.UNAVAILABLE",
          "ERROR",
          "engine-backed streaming requires an execution route",
          "sbp_sbsql.wire"));
      return {false, RenderMessageVectorSet(messages)};
    }
    if (!session_.authenticated) {
      messages.diagnostics.push_back(MakeDiagnostic(
          "SBSQL.AUTH.REQUIRED",
          "ERROR",
          "engine-backed streaming requires an authenticated server session",
          "sbp_sbsql.wire"));
      return {false, RenderMessageVectorSet(messages)};
    }
    const auto executed = ExecuteSblrOnRoute(EngineShowVersionOperationEnvelope(), true);
    if (!executed.accepted) return {false, RenderMessageVectorSet(executed.messages)};
    last_cursor_uuid_ = executed.cursor_uuid;
    std::ostringstream out;
    out << "CURSOR " << executed.cursor_uuid << ' ' << executed.row_count << " source=engine\n";
    return {false, out.str()};
  }
  if (upper == "SBPS CHUNKED EXECUTE") {
    MessageVectorSet messages;
    if (!HasExecutionRoute()) {
      messages.diagnostics.push_back(MakeDiagnostic(
          "SBSQL.SERVER.UNAVAILABLE",
          "ERROR",
          "chunked SBPS execution requires an execution route",
          "sbp_sbsql.wire"));
      return {false, RenderMessageVectorSet(messages)};
    }
    if (!session_.authenticated) {
      messages.diagnostics.push_back(MakeDiagnostic(
          "SBSQL.AUTH.REQUIRED",
          "ERROR",
          "chunked SBPS execution requires an authenticated server session",
          "sbp_sbsql.wire"));
      return {false, RenderMessageVectorSet(messages)};
    }
    constexpr std::size_t kParameterBytes = 1100 * 1024;
    constexpr std::uint64_t kResultRows = 24000;
    const auto envelope = ChunkedParserJsonEnvelope(kParameterBytes, kResultRows);
    const auto executed = ExecuteSblrOnRoute(envelope, false);
    if (!executed.accepted) return {false, RenderMessageVectorSet(executed.messages)};
    const bool saw_last_row =
        executed.row_packet.find("\"row_index\":23999") != std::string::npos;
    std::ostringstream out;
    out << "CHUNKED_EXECUTE accepted request_bytes=" << envelope.size()
        << " result_bytes=" << executed.row_packet.size()
        << " row_count=" << executed.row_count
        << " last_row=" << (saw_last_row ? "true" : "false") << '\n';
    return {false, out.str()};
  }
  if (upper == "COPY STREAM") {
    MessageVectorSet messages;
    if (!HasExecutionRoute()) {
      messages.diagnostics.push_back(MakeDiagnostic(
          "SBSQL.SERVER.UNAVAILABLE",
          "ERROR",
          "COPY streaming requires an execution route",
          "sbp_sbsql.wire"));
      return {false, RenderMessageVectorSet(messages)};
    }
    if (!session_.authenticated) {
      messages.diagnostics.push_back(MakeDiagnostic(
          "SBSQL.AUTH.REQUIRED",
          "ERROR",
          "COPY streaming requires an authenticated server session",
          "sbp_sbsql.wire"));
      return {false, RenderMessageVectorSet(messages)};
    }
    auto resolved = ResolveNameOnRoute("users.public.sbsfc021_stream_table", false, "relation");
    if (!resolved.resolved) {
      return {false, RenderMessageVectorSet(resolved.messages)};
    }
    session_.catalog_epoch = std::max(session_.catalog_epoch, resolved.catalog_epoch);
    session_.security_policy_epoch = std::max(session_.security_policy_epoch, resolved.security_epoch);

    const auto begun = ExecuteSblrOnRoute(TransactionBeginOperationEnvelope(), false);
    if (!begun.accepted) return {false, RenderMessageVectorSet(begun.messages)};
    const auto executed =
        ExecuteSblrOnRoute(EngineBackedCopyStreamImportEnvelope(resolved.object_uuid), true);
    if (!executed.accepted) {
      const auto rolled_back =
          ExecuteSblrOnRoute(TransactionRollbackOperationEnvelope(), false);
      (void)rolled_back;
      return {false, RenderMessageVectorSet(executed.messages)};
    }
    const auto committed = ExecuteSblrOnRoute(TransactionCommitOperationEnvelope(), false);
    if (!committed.accepted) {
      const auto rolled_back =
          ExecuteSblrOnRoute(TransactionRollbackOperationEnvelope(), false);
      (void)rolled_back;
      return {false, RenderMessageVectorSet(committed.messages)};
    }
    last_cursor_uuid_ = executed.cursor_uuid;
    std::ostringstream out;
    out << "COPY_CURSOR " << executed.cursor_uuid << " events=" << executed.row_count
        << " source=engine operation_id=" << executed.operation_id
        << " committed=true commit_operation_id=" << committed.operation_id << '\n';
    return {false, out.str()};
  }
  if (upper == "MULTI RESULT") {
    MessageVectorSet messages;
    if (!HasExecutionRoute()) {
      messages.diagnostics.push_back(MakeDiagnostic(
          "SBSQL.SERVER.UNAVAILABLE",
          "ERROR",
          "multi-result streaming requires an execution route",
          "sbp_sbsql.wire"));
      return {false, RenderMessageVectorSet(messages)};
    }
    if (!session_.authenticated) {
      messages.diagnostics.push_back(MakeDiagnostic(
          "SBSQL.AUTH.REQUIRED",
          "ERROR",
          "multi-result streaming requires an authenticated server session",
          "sbp_sbsql.wire"));
      return {false, RenderMessageVectorSet(messages)};
    }
    const auto executed = ExecuteSblrOnRoute(MultiResultParserJsonEnvelope(3), true);
    if (!executed.accepted) return {false, RenderMessageVectorSet(executed.messages)};
    last_cursor_uuid_ = executed.cursor_uuid;
    std::ostringstream out;
    out << "MULTI_CURSOR " << executed.cursor_uuid << " events=" << executed.row_count << '\n';
    return {false, out.str()};
  }
  if (upper == "WARNING STREAM") {
    MessageVectorSet messages;
    if (!HasExecutionRoute()) {
      messages.diagnostics.push_back(MakeDiagnostic(
          "SBSQL.SERVER.UNAVAILABLE",
          "ERROR",
          "warning/partial-result streaming requires an execution route",
          "sbp_sbsql.wire"));
      return {false, RenderMessageVectorSet(messages)};
    }
    if (!session_.authenticated) {
      messages.diagnostics.push_back(MakeDiagnostic(
          "SBSQL.AUTH.REQUIRED",
          "ERROR",
          "warning/partial-result streaming requires an authenticated server session",
          "sbp_sbsql.wire"));
      return {false, RenderMessageVectorSet(messages)};
    }
    const auto executed = ExecuteSblrOnRoute(WarningStreamParserJsonEnvelope(3, 2), true);
    if (!executed.accepted) return {false, RenderMessageVectorSet(executed.messages)};
    last_cursor_uuid_ = executed.cursor_uuid;
    std::ostringstream out;
    out << "WARNING_CURSOR " << executed.cursor_uuid << " events=" << executed.row_count << '\n';
    return {false, out.str()};
  }
  if (upper == "TIMEOUT STREAM" || upper == "DRAIN STREAM" || upper == "CANCEL STREAM") {
    MessageVectorSet messages;
    if (!HasExecutionRoute()) {
      messages.diagnostics.push_back(MakeDiagnostic(
          "SBSQL.SERVER.UNAVAILABLE",
          "ERROR",
          "stream finality testing requires an execution route",
          "sbp_sbsql.wire"));
      return {false, RenderMessageVectorSet(messages)};
    }
    if (!session_.authenticated) {
      messages.diagnostics.push_back(MakeDiagnostic(
          "SBSQL.AUTH.REQUIRED",
          "ERROR",
          "stream finality testing requires an authenticated server session",
          "sbp_sbsql.wire"));
      return {false, RenderMessageVectorSet(messages)};
    }
    const std::string mode = upper == "TIMEOUT STREAM" ? "timeout" :
                             (upper == "DRAIN STREAM" ? "drain" : "cancel");
    const std::uint64_t after_fetches = upper == "TIMEOUT STREAM" ? 1 : 0;
    const auto executed = ExecuteSblrOnRoute(FinalityStreamParserJsonEnvelope(mode, 2, after_fetches), true);
    if (!executed.accepted) return {false, RenderMessageVectorSet(executed.messages)};
    last_cursor_uuid_ = executed.cursor_uuid;
    std::ostringstream out;
    out << ToUpperAscii(mode) << "_CURSOR " << executed.cursor_uuid
        << " events=" << executed.row_count << '\n';
    return {false, out.str()};
  }
  if (upper == "ROUTINE CURSOR") {
    MessageVectorSet messages;
    if (!HasExecutionRoute()) {
      messages.diagnostics.push_back(MakeDiagnostic(
          "SBSQL.SERVER.UNAVAILABLE",
          "ERROR",
          "routine cursor testing requires an execution route",
          "sbp_sbsql.wire"));
      return {false, RenderMessageVectorSet(messages)};
    }
    if (!session_.authenticated) {
      messages.diagnostics.push_back(MakeDiagnostic(
          "SBSQL.AUTH.REQUIRED",
          "ERROR",
          "routine cursor testing requires an authenticated server session",
          "sbp_sbsql.wire"));
      return {false, RenderMessageVectorSet(messages)};
    }
    const auto opened = ExecuteSblrOnRoute(MultiResultParserJsonEnvelope(2), true);
    if (!opened.accepted) return {false, RenderMessageVectorSet(opened.messages)};
    const auto routine =
        ExecuteSblrOnRoute(RoutineCursorArgumentJsonEnvelope(opened.cursor_uuid), false);
    if (!routine.accepted) {
      (void)CloseCursorOnRoute(opened.cursor_uuid);
      return {false, RenderMessageVectorSet(routine.messages)};
    }
    last_cursor_uuid_ = opened.cursor_uuid;
    const bool same_cursor = routine.cursor_uuid == opened.cursor_uuid;
    const bool routine_row_zero =
        routine.row_packet.find("\"row_index\":0") != std::string::npos;
    const bool routine_operation =
        routine.operation_id == "routine.execute_cursor_argument" &&
        routine.row_packet.find("\"operation_id\":\"routine.execute_cursor_argument\"") !=
            std::string::npos;
    std::ostringstream out;
    out << "ROUTINE_CURSOR " << opened.cursor_uuid
        << " operation_id=" << routine.operation_id
        << " routine_rows=" << routine.row_count
        << " same_cursor=" << (same_cursor ? "true" : "false")
        << " routine_row_zero=" << (routine_row_zero ? "true" : "false")
        << " routine_operation=" << (routine_operation ? "true" : "false") << '\n';
    return {false, out.str()};
  }
  if (upper.starts_with("FETCH")) {
    MessageVectorSet messages;
    if (last_cursor_uuid_.empty()) {
      messages.diagnostics.push_back(MakeDiagnostic(
          "PARSER_SERVER_IPC.CURSOR_REQUIRED",
          "ERROR",
          "FETCH requires a live server cursor.",
          "sbp_sbsql.wire"));
      return {false, RenderMessageVectorSet(messages)};
    }
    std::uint64_t max_rows = 1;
    const auto argument = AfterCommand(trimmed, "FETCH");
    if (!argument.empty()) {
      char* end = nullptr;
      const auto parsed = std::strtoull(argument.c_str(), &end, 10);
      if (end != nullptr && *end == '\0' && parsed > 0) {
        max_rows = static_cast<std::uint64_t>(parsed);
      }
    }
    const auto fetched = FetchCursorOnRoute(last_cursor_uuid_, max_rows);
    if (!fetched.accepted) return {false, RenderMessageVectorSet(fetched.messages)};
    std::ostringstream out;
    out << "FETCH " << fetched.cursor_uuid << ' ' << fetched.row_count
        << " end=" << (fetched.end_of_cursor ? "true" : "false") << ' ';
    if (!fetched.detail.empty()) {
      out << "detail=" << fetched.detail << ' ';
    }
    out
        << fetched.row_packet;
    if (fetched.row_packet.empty() || fetched.row_packet.back() != '\n') out << '\n';
    return {false, out.str()};
  }
  if (upper == "CLOSE CURSOR") {
    MessageVectorSet messages;
    if (last_cursor_uuid_.empty()) {
      messages.diagnostics.push_back(MakeDiagnostic(
          "PARSER_SERVER_IPC.CURSOR_REQUIRED",
          "ERROR",
          "CLOSE CURSOR requires a live server cursor.",
          "sbp_sbsql.wire"));
      return {false, RenderMessageVectorSet(messages)};
    }
    const auto closed = CloseCursorOnRoute(last_cursor_uuid_);
    if (!closed.accepted) return {false, RenderMessageVectorSet(closed.messages)};
    last_cursor_uuid_.clear();
    return {false, "OK CURSOR_CLOSED\n"};
  }
  if (upper == "CANCEL CURSOR") {
    MessageVectorSet messages;
    if (last_cursor_uuid_.empty()) {
      messages.diagnostics.push_back(MakeDiagnostic(
          "PARSER_SERVER_IPC.CURSOR_REQUIRED",
          "ERROR",
          "CANCEL CURSOR requires a live server cursor.",
          "sbp_sbsql.wire"));
      return {false, RenderMessageVectorSet(messages)};
    }
    const auto closed = CancelCursorOnRoute(last_cursor_uuid_);
    if (!closed.accepted) return {false, RenderMessageVectorSet(closed.messages)};
    last_cursor_uuid_.clear();
    return {false, "OK CURSOR_CANCELLED detail=" + closed.detail + "\n"};
  }
  MessageVectorSet messages;
  messages.diagnostics.push_back(MakeDiagnostic(
      "SBSQL.TEST_WIRE.COMMAND_UNKNOWN", "ERROR", "test wire command is not recognized",
      "sbp_sbsql.wire", {{"command", trimmed}}));
  return {false, RenderMessageVectorSet(messages)};
}

int SbsqlTestWireSession::ServeFd(std::intptr_t fd) {
  if (metrics_) metrics_->SetState(ParserState::kIdlePreAuth);
  if (config_.tls_required) {
    const int rc = ServeSbwp(fd);
    if (metrics_) metrics_->SetState(rc == 0 ? ParserState::kDisconnected : ParserState::kFailed);
    return rc;
  }

  fd_set read_fds;
  FD_ZERO(&read_fds);
#ifdef _WIN32
  FD_SET(static_cast<SOCKET>(fd), &read_fds);
#else
  const int posix_fd = static_cast<int>(fd);
  FD_SET(posix_fd, &read_fds);
#endif
  timeval timeout{};
  timeout.tv_sec = 0;
  timeout.tv_usec = 100000;
#ifdef _WIN32
  const int ready = ::select(0, &read_fds, nullptr, nullptr, &timeout);
#else
  const int ready = ::select(posix_fd + 1, &read_fds, nullptr, nullptr, &timeout);
#endif
#ifdef _WIN32
  if (ready > 0 && FD_ISSET(static_cast<SOCKET>(fd), &read_fds)) {
#else
  if (ready > 0 && FD_ISSET(posix_fd, &read_fds)) {
#endif
    char magic[4]{};
    int rc = 0;
#ifdef _WIN32
    rc = ::recv(static_cast<SOCKET>(fd), magic, sizeof(magic), MSG_PEEK);
#else
    do {
      rc = static_cast<int>(::recv(posix_fd, magic, sizeof(magic), MSG_PEEK));
    } while (rc < 0 && errno == EINTR);
#endif
    if (rc == static_cast<int>(sizeof(magic)) &&
        std::memcmp(magic, "SBWP", sizeof(magic)) == 0) {
      const int sbwp_rc = ServeSbwp(fd);
      if (metrics_) metrics_->SetState(sbwp_rc == 0 ? ParserState::kDisconnected : ParserState::kFailed);
      return sbwp_rc;
    }
  }

  if (!WriteAll(fd, "ScratchBird SBSQL parser ready\n")) return 1;
  std::string line;
  int rc = 0;
  while (ReadLine(fd, &line)) {
    const auto response = HandleLine(line);
    if (!WriteAll(fd, response.text)) {
      rc = 1;
      break;
    }
    if (response.close) break;
  }
  if (session_.authenticated && HasExecutionRoute()) {
    MessageVectorSet disconnect_messages;
    (void)DisconnectExecutionRoute(&disconnect_messages);
  }
  if (metrics_) metrics_->SetState(rc == 0 ? ParserState::kDisconnected : ParserState::kFailed);
  return rc;
}

} // namespace scratchbird::parser::sbsql
