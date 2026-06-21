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
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <utility>
#include <vector>

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

constexpr std::size_t kMaxNameResolutionCacheEntries = 4096;
constexpr std::size_t kMaxSharedNameResolutionCacheEntries = 16384;
constexpr std::size_t kMaxStableRelationNameResolutionCacheEntries = 4096;

std::uint64_t CurrentUnixMillis() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

using ParserPipelineClock = std::chrono::steady_clock;

std::uint64_t ParserPipelineElapsedMicros(ParserPipelineClock::time_point start,
                                          ParserPipelineClock::time_point end) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
}

void WriteParserPipelinePhaseTrace(
    std::string_view sql,
    const PipelineResult& result,
    const std::vector<std::pair<std::string, std::uint64_t>>& phase_micros) {
  const char* path = std::getenv("SCRATCHBIRD_SBSQL_PIPELINE_PHASE_TRACE_FILE");
  if (path == nullptr || *path == '\0') return;
  std::ofstream out(path, std::ios::app);
  if (!out) return;
  out << "layer=sbsql_pipeline"
      << "\toperation=" << result.operation_family
      << "\tfamily=" << result.statement_family
      << "\taccepted=" << (result.accepted ? "true" : "false")
      << "\tsql_bytes=" << sql.size()
      << "\tstatement_hash=" << result.statement_hash;
  for (const auto& [phase, micros] : phase_micros) {
    out << '\t' << phase << "_us=" << micros;
  }
  out << '\n';
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

bool ApplyExecutedTransactionState(const ServerExecutionResult& executed,
                                   SessionContext* session) {
  if (session == nullptr || !executed.transaction_state_present) return false;
  const bool changed =
      session->local_transaction_id != executed.local_transaction_id ||
      session->transaction_uuid != executed.transaction_uuid;
  session->local_transaction_id = executed.local_transaction_id;
  session->snapshot_visible_through_local_transaction_id =
      executed.snapshot_visible_through_local_transaction_id;
  session->transaction_uuid = executed.transaction_uuid;
  session->transaction_timestamp = executed.transaction_timestamp;
  session->transaction_context = "always_active";
  return changed;
}

bool ExecutionInvalidatesNameResolution(std::string_view operation_id) {
  return operation_id.rfind("ddl.", 0) == 0 ||
         operation_id.rfind("catalog.", 0) == 0 ||
         operation_id.rfind("security.", 0) == 0 ||
         operation_id.rfind("language.", 0) == 0 ||
         operation_id.rfind("policy.", 0) == 0 ||
         operation_id.rfind("auth.", 0) == 0;
}

bool ExecutionPreservesReferencedRelationNames(std::string_view operation_id) {
  return operation_id == "ddl.create_index" ||
         operation_id == "ddl.create_index_template";
}

bool IsReferencedRelationNameClass(std::string_view object_class) {
  return object_class == "relation" ||
         object_class == "table" ||
         object_class == "view";
}

struct ObjectReference {
  std::string presented_name;
  std::string object_class{"relation"};
  bool quoted{false};
};

struct ResolvedObjectReferenceSeed {
  ObjectReference ref;
  PublicNameResolutionResult resolved;
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
  key.name_resolution_epoch = session.localized_name_epoch != 0
                                  ? session.localized_name_epoch
                                  : session.catalog_epoch;
  key.resource_epoch = session.language_resource_epoch != 0
                           ? session.language_resource_epoch
                           : (config.resource_budget.max_statement_bytes ^
                              config.resource_budget.max_sblr_envelope_bytes);
  key.localized_name_epoch = key.name_resolution_epoch;
  key.language_resource_epoch = key.resource_epoch;
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
  key.language_profile = session.language_profile.empty()
                             ? session.default_language
                             : session.language_profile;
  key.language_tag = session.language_tag.empty()
                         ? session.default_language
                         : session.language_tag;
  key.input_syntax_profile = session.input_syntax_profile;
  key.input_language_fallback_tag = session.input_language_fallback_tag;
  key.common_resource_hash = session.common_resource_hash;
  key.policy_profile = session.policy_profile_uuid;
  key.parser_profile = config.profile_id;
  key.message_resource_epoch = session.message_resource_epoch;
  key.resource_compatibility_identity = session.resource_compatibility_identity;
  key.resource_version_identity = session.resource_version_identity;
  key.result_contract_hash =
      std::to_string(Fnv1a64(session.result_rendering_policy + "|" +
                             config.dialect + "|" + session.common_resource_hash +
                             "|" + session.resource_version_identity));
  return key;
}

std::string BuildNameResolutionCacheKey(const SessionContext& session,
                                        std::string_view presented_name,
                                        bool quoted,
                                        std::string_view object_class) {
  std::ostringstream key;
  key << presented_name << "|quoted=" << (quoted ? "1" : "0")
      << "|class=" << object_class
      << "|catalog=" << session.catalog_epoch
      << "|security=" << session.security_policy_epoch
      << "|grant=" << session.grant_epoch
      << "|descriptor=" << session.descriptor_epoch
      << "|localized_name=" << session.localized_name_epoch
      << "|language_resource=" << session.language_resource_epoch
      << "|message_resource=" << session.message_resource_epoch
      << "|roles=" << JoinStable(session.effective_role_uuids)
      << "|groups=" << JoinStable(session.effective_group_uuids)
      << "|search_path=" << JoinStable(session.search_path)
      << "|language_profile=" << session.language_profile
      << "|language_tag=" << session.language_tag
      << "|input_syntax=" << session.input_syntax_profile
      << "|input_fallback=" << session.input_language_fallback_tag
      << "|common_resource=" << session.common_resource_hash
      << "|policy_profile=" << session.policy_profile_uuid
      << "|resource_compat=" << session.resource_compatibility_identity
      << "|resource_version=" << session.resource_version_identity;
  return key.str();
}

std::string BuildStableRelationNameResolutionCacheKey(
    const SessionContext& session,
    std::string_view presented_name,
    bool quoted,
    std::string_view object_class) {
  const bool qualified = presented_name.find('.') != std::string_view::npos;
  std::ostringstream key;
  key << presented_name << "|quoted=" << (quoted ? "1" : "0")
      << "|class=" << object_class
      << "|security=" << session.security_policy_epoch
      << "|grant=" << session.grant_epoch
      << "|localized_name=" << session.localized_name_epoch
      << "|language_resource=" << session.language_resource_epoch
      << "|message_resource=" << session.message_resource_epoch
      << "|roles=" << JoinStable(session.effective_role_uuids)
      << "|groups=" << JoinStable(session.effective_group_uuids)
      << "|search_path=" << (qualified ? std::string("<qualified>")
                                       : JoinStable(session.search_path))
      << "|language_profile=" << session.language_profile
      << "|language_tag=" << session.language_tag
      << "|input_syntax=" << session.input_syntax_profile
      << "|input_fallback=" << session.input_language_fallback_tag
      << "|common_resource=" << session.common_resource_hash
      << "|policy_profile=" << session.policy_profile_uuid
      << "|resource_compat=" << session.resource_compatibility_identity
      << "|resource_version=" << session.resource_version_identity;
  return key.str();
}

std::mutex& SharedNameResolutionCacheMutex() {
  static std::mutex mutex;
  return mutex;
}

std::map<std::string, CachedPublicNameResolution>& SharedNameResolutionCache() {
  static std::map<std::string, CachedPublicNameResolution> cache;
  return cache;
}

std::deque<std::string>& SharedNameResolutionLru() {
  static std::deque<std::string> lru;
  return lru;
}

std::optional<CachedPublicNameResolution> LookupSharedNameResolutionCache(
    const std::string& cache_key) {
  std::lock_guard<std::mutex> guard(SharedNameResolutionCacheMutex());
  const auto found = SharedNameResolutionCache().find(cache_key);
  if (found == SharedNameResolutionCache().end()) return std::nullopt;
  return found->second;
}

void StoreSharedNameResolutionCacheEntry(
    const std::string& cache_key,
    const CachedPublicNameResolution& cached) {
  if (cache_key.empty() || cached.object_uuid.empty()) return;
  std::lock_guard<std::mutex> guard(SharedNameResolutionCacheMutex());
  auto& cache = SharedNameResolutionCache();
  auto& lru = SharedNameResolutionLru();
  cache[cache_key] = cached;
  lru.erase(std::remove(lru.begin(), lru.end(), cache_key), lru.end());
  lru.push_back(cache_key);
  while (cache.size() > kMaxSharedNameResolutionCacheEntries && !lru.empty()) {
    cache.erase(lru.front());
    lru.pop_front();
  }
}

void ClearSharedNameResolutionCache() {
  std::lock_guard<std::mutex> guard(SharedNameResolutionCacheMutex());
  SharedNameResolutionCache().clear();
  SharedNameResolutionLru().clear();
}

std::optional<std::string> DdlResultRowField(std::string_view payload,
                                             std::string_view field_name) {
  std::istringstream in{std::string(payload)};
  std::string line;
  while (std::getline(in, line)) {
    if (!line.starts_with("row[")) continue;
    const auto eq = line.find("]=");
    if (eq == std::string::npos) continue;
    std::string_view body(line);
    body.remove_prefix(eq + 2);
    std::size_t start = 0;
    while (start <= body.size()) {
      const std::size_t end = body.find(';', start);
      const std::string_view item =
          body.substr(start, end == std::string_view::npos ? body.size() - start : end - start);
      const std::size_t item_eq = item.find('=');
      if (item_eq != std::string_view::npos && item.substr(0, item_eq) == field_name) {
        return std::string(item.substr(item_eq + 1));
      }
      if (end == std::string_view::npos) break;
      start = end + 1;
    }
  }
  return std::nullopt;
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

bool CanReuseFrontdoorCacheForSubmit(const PipelineResult& result) {
  return result.operation_family == "sblr.dml.operation.v3" ||
         result.operation_family == "sblr.query.relational.v3" ||
         result.operation_family == "sblr.transaction.control.v3";
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

void DropObjectReferenceLeaf(ObjectReference* ref) {
  if (ref == nullptr) return;
  const auto dot = ref->presented_name.rfind('.');
  if (dot == std::string::npos) return;
  ref->presented_name.erase(dot);
}

std::string LowerObjectReferenceName(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const unsigned char ch : text) {
    out.push_back(static_cast<char>(std::tolower(ch)));
  }
  return out;
}

std::vector<std::string> ExtractLeadingCteNames(const CstDocument& cst) {
  std::vector<std::string> names;
  std::size_t index = cst.tokens.size();
  for (std::size_t i = 0; i < cst.tokens.size(); ++i) {
    if (!IsTriviaToken(cst.tokens[i])) {
      index = i;
      break;
    }
  }
  if (index < cst.tokens.size() && IsWord(cst.tokens[index], "EXPLAIN")) {
    ++index;
    while (index < cst.tokens.size() && IsTriviaToken(cst.tokens[index])) ++index;
  }
  if (index >= cst.tokens.size() || !IsWord(cst.tokens[index], "WITH")) return names;
  ++index;
  while (index < cst.tokens.size() && IsTriviaToken(cst.tokens[index])) ++index;
  if (index < cst.tokens.size() && IsWord(cst.tokens[index], "RECURSIVE")) ++index;

  while (index < cst.tokens.size()) {
    while (index < cst.tokens.size() && IsTriviaToken(cst.tokens[index])) ++index;
    if (index >= cst.tokens.size() ||
        (cst.tokens[index].kind != TokenKind::kIdentifier &&
         cst.tokens[index].kind != TokenKind::kKeyword)) {
      break;
    }
    names.push_back(LowerObjectReferenceName(cst.tokens[index].text));
    ++index;
    while (index < cst.tokens.size() && IsTriviaToken(cst.tokens[index])) ++index;
    if (index < cst.tokens.size() && cst.tokens[index].kind == TokenKind::kSymbol &&
        cst.tokens[index].text == "(") {
      std::size_t depth = 1;
      ++index;
      while (index < cst.tokens.size() && depth != 0) {
        if (cst.tokens[index].kind == TokenKind::kSymbol && cst.tokens[index].text == "(") {
          ++depth;
        } else if (cst.tokens[index].kind == TokenKind::kSymbol && cst.tokens[index].text == ")") {
          --depth;
        }
        ++index;
      }
    }
    while (index < cst.tokens.size() && IsTriviaToken(cst.tokens[index])) ++index;
    if (index >= cst.tokens.size() || !IsWord(cst.tokens[index], "AS")) break;
    ++index;
    while (index < cst.tokens.size() && IsTriviaToken(cst.tokens[index])) ++index;
    if (index >= cst.tokens.size() || cst.tokens[index].kind != TokenKind::kSymbol ||
        cst.tokens[index].text != "(") {
      break;
    }
    std::size_t depth = 1;
    ++index;
    while (index < cst.tokens.size() && depth != 0) {
      if (cst.tokens[index].kind == TokenKind::kSymbol && cst.tokens[index].text == "(") {
        ++depth;
      } else if (cst.tokens[index].kind == TokenKind::kSymbol && cst.tokens[index].text == ")") {
        --depth;
      }
      ++index;
    }
    while (index < cst.tokens.size() && IsTriviaToken(cst.tokens[index])) ++index;
    if (index >= cst.tokens.size() || cst.tokens[index].kind != TokenKind::kSymbol ||
        cst.tokens[index].text != ",") {
      break;
    }
    ++index;
  }
  return names;
}

std::vector<std::string> ExtractDerivedCteNames(const CstDocument& cst) {
  std::vector<std::string> names;
  for (std::size_t index = 0; index < cst.tokens.size(); ++index) {
    if (!IsWord(cst.tokens[index], "FROM")) continue;
    std::size_t cursor = index + 1;
    while (cursor < cst.tokens.size() && IsTriviaToken(cst.tokens[cursor])) ++cursor;
    if (cursor >= cst.tokens.size() ||
        cst.tokens[cursor].kind != TokenKind::kSymbol ||
        cst.tokens[cursor].text != "(") {
      continue;
    }
    ++cursor;
    while (cursor < cst.tokens.size() && IsTriviaToken(cst.tokens[cursor])) ++cursor;
    if (cursor >= cst.tokens.size() || !IsWord(cst.tokens[cursor], "WITH")) continue;
    ++cursor;
    while (cursor < cst.tokens.size() && IsTriviaToken(cst.tokens[cursor])) ++cursor;
    if (cursor < cst.tokens.size() && IsWord(cst.tokens[cursor], "RECURSIVE")) ++cursor;
    while (cursor < cst.tokens.size() && IsTriviaToken(cst.tokens[cursor])) ++cursor;
    if (cursor >= cst.tokens.size() ||
        (cst.tokens[cursor].kind != TokenKind::kIdentifier &&
         cst.tokens[cursor].kind != TokenKind::kKeyword)) {
      continue;
    }
    const std::string name = LowerObjectReferenceName(cst.tokens[cursor].text);
    if (std::find(names.begin(), names.end(), name) == names.end()) {
      names.push_back(name);
    }
  }
  return names;
}

bool IsLocalCteReference(const ObjectReference& ref,
                         const std::vector<std::string>& local_cte_names) {
  if (ref.presented_name.empty() ||
      ref.presented_name.find('.') != std::string::npos) {
    return false;
  }
  const std::string lowered = LowerObjectReferenceName(ref.presented_name);
  return std::find(local_cte_names.begin(), local_cte_names.end(), lowered) !=
         local_cte_names.end();
}

std::vector<ObjectReference> ExtractMergeObjectReferences(const CstDocument& cst,
                                                          std::size_t first_token) {
  std::vector<ObjectReference> refs;
  bool target_seen = false;
  bool source_seen = false;
  for (std::size_t i = first_token + 1; i < cst.tokens.size(); ++i) {
    const auto& token = cst.tokens[i];
    if (IsTriviaToken(token)) continue;
    if (token.kind == TokenKind::kEnd) break;
    if (!target_seen && IsWord(token, "INTO")) {
      if (auto ref = ExtractObjectReferenceAt(cst, i + 1)) {
        refs.push_back(*ref);
        target_seen = true;
      }
      continue;
    }
    if (!source_seen && IsWord(token, "USING")) {
      if (auto ref = ExtractObjectReferenceAt(cst, i + 1)) {
        refs.push_back(*ref);
        source_seen = true;
      }
      continue;
    }
    if (target_seen && source_seen) break;
  }
  return refs;
}

std::vector<ObjectReference> ExtractInsertObjectReferences(const CstDocument& cst,
                                                           std::size_t first_token) {
  std::vector<ObjectReference> refs;
  bool target_seen = false;
  std::size_t after_target = first_token + 1;
  for (std::size_t i = first_token + 1; i < cst.tokens.size(); ++i) {
    const auto& token = cst.tokens[i];
    if (IsTriviaToken(token)) continue;
    if (token.kind == TokenKind::kEnd) break;
    if (IsWord(token, "INTO")) {
      if (auto ref = ExtractObjectReferenceAt(cst, i + 1)) {
        refs.push_back(*ref);
        target_seen = true;
      }
      after_target = i + 1;
      break;
    }
    if (token.kind != TokenKind::kKeyword && token.kind != TokenKind::kIdentifier) {
      break;
    }
  }
  if (!target_seen) return refs;

  bool saw_row_number = false;
  bool left_source_seen = false;
  for (std::size_t i = after_target; i < cst.tokens.size(); ++i) {
    const auto& token = cst.tokens[i];
    if (IsTriviaToken(token)) continue;
    if (token.kind == TokenKind::kEnd) break;
    if (!saw_row_number) {
      if (IsWord(token, "ROW_NUMBER")) saw_row_number = true;
      continue;
    }
    if (!left_source_seen && IsWord(token, "FROM")) {
      if (auto ref = ExtractObjectReferenceAt(cst, i + 1)) {
        refs.push_back(*ref);
        left_source_seen = true;
      }
      continue;
    }
    if (left_source_seen && IsWord(token, "JOIN")) {
      if (auto ref = ExtractObjectReferenceAt(cst, i + 1)) {
        refs.push_back(*ref);
      }
      break;
    }
  }
  return refs;
}

std::vector<ObjectReference> ExtractCreateIndexObjectReferences(const CstDocument& cst,
                                                                std::size_t first_token) {
  std::vector<ObjectReference> refs;
  bool saw_index = false;
  for (std::size_t i = first_token + 1; i < cst.tokens.size(); ++i) {
    const auto& token = cst.tokens[i];
    if (IsTriviaToken(token)) continue;
    if (token.kind == TokenKind::kEnd) break;
    if (!saw_index && IsWord(token, "INDEX")) {
      saw_index = true;
      continue;
    }
    if (saw_index && IsWord(token, "ON")) {
      if (auto ref = ExtractObjectReferenceAt(cst, i + 1)) {
        refs.push_back(*ref);
      }
      return refs;
    }
  }
  return refs;
}

std::size_t NextNonTriviaIndex(const CstDocument& cst, std::size_t index) {
  while (index < cst.tokens.size() && IsTriviaToken(cst.tokens[index])) ++index;
  return index;
}

std::vector<ObjectReference> ExtractMultimodelObjectReferences(const CstDocument& cst,
                                                               std::size_t first_token) {
  std::vector<ObjectReference> refs;
  if (first_token >= cst.tokens.size()) return refs;
  const auto push_ref_at = [&](std::size_t marker) {
    if (auto ref = ExtractObjectReferenceAt(cst, NextNonTriviaIndex(cst, marker))) {
      refs.push_back(*ref);
    }
  };

  if (IsWord(cst.tokens[first_token], "DOCUMENT") ||
      IsWord(cst.tokens[first_token], "FULLTEXT") ||
      IsWord(cst.tokens[first_token], "OPENSEARCH") ||
      IsWord(cst.tokens[first_token], "TIMESERIES") ||
      IsWord(cst.tokens[first_token], "GRAPH") ||
      IsWord(cst.tokens[first_token], "SEARCH")) {
    push_ref_at(first_token + 1);
    return refs;
  }

  if (IsWord(cst.tokens[first_token], "TIME")) {
    const std::size_t second = NextNonTriviaIndex(cst, first_token + 1);
    if (second < cst.tokens.size() && IsWord(cst.tokens[second], "SERIES")) {
      push_ref_at(second + 1);
    }
    return refs;
  }

  if (IsWord(cst.tokens[first_token], "CHANGE")) {
    const std::size_t second = NextNonTriviaIndex(cst, first_token + 1);
    if (second < cst.tokens.size() && IsWord(cst.tokens[second], "STREAM")) {
      push_ref_at(second + 1);
    }
    return refs;
  }

  if (IsWord(cst.tokens[first_token], "REINDEX")) {
    const std::size_t second = NextNonTriviaIndex(cst, first_token + 1);
    const std::size_t third = second < cst.tokens.size() ? NextNonTriviaIndex(cst, second + 1)
                                                        : cst.tokens.size();
    if (second < cst.tokens.size() && third < cst.tokens.size() &&
        IsWord(cst.tokens[second], "VECTOR") &&
        IsWord(cst.tokens[third], "COLLECTION")) {
      push_ref_at(third + 1);
    }
    return refs;
  }

  return refs;
}

std::vector<ObjectReference> ExtractFilespaceObjectReferences(const CstDocument& cst,
                                                              std::size_t first_token) {
  std::vector<ObjectReference> refs;
  if (first_token >= cst.tokens.size()) return refs;
  const auto push_ref_at = [&](std::size_t marker) {
    if (auto ref = ExtractObjectReferenceAt(cst, NextNonTriviaIndex(cst, marker))) {
      ref->object_class = "filespace";
      refs.push_back(*ref);
    }
  };
  const auto second = NextNonTriviaIndex(cst, first_token + 1);
  if (second >= cst.tokens.size()) return refs;

  if (IsWord(cst.tokens[first_token], "ALTER") &&
      IsWord(cst.tokens[second], "FILESPACE")) {
    push_ref_at(second + 1);
    return refs;
  }

  if ((IsWord(cst.tokens[first_token], "ATTACH") ||
       IsWord(cst.tokens[first_token], "DETACH") ||
       IsWord(cst.tokens[first_token], "DISCONNECT") ||
       IsWord(cst.tokens[first_token], "MOVE") ||
       IsWord(cst.tokens[first_token], "MERGE") ||
       IsWord(cst.tokens[first_token], "PROMOTE") ||
       IsWord(cst.tokens[first_token], "GROW") ||
       IsWord(cst.tokens[first_token], "RESIZE") ||
       IsWord(cst.tokens[first_token], "SHRINK") ||
       IsWord(cst.tokens[first_token], "VERIFY") ||
       IsWord(cst.tokens[first_token], "COMPACT") ||
       IsWord(cst.tokens[first_token], "FENCE") ||
       IsWord(cst.tokens[first_token], "RELEASE") ||
       IsWord(cst.tokens[first_token], "ARCHIVE") ||
       IsWord(cst.tokens[first_token], "QUARANTINE") ||
       IsWord(cst.tokens[first_token], "REPAIR") ||
       IsWord(cst.tokens[first_token], "REBUILD") ||
       IsWord(cst.tokens[first_token], "SALVAGE")) &&
      IsWord(cst.tokens[second], "FILESPACE")) {
    push_ref_at(second + 1);
    return refs;
  }

  if ((IsWord(cst.tokens[first_token], "DROP") ||
       IsWord(cst.tokens[first_token], "DELETE")) &&
      IsWord(cst.tokens[second], "STORAGE")) {
    const auto third = NextNonTriviaIndex(cst, second + 1);
    if (third < cst.tokens.size() && IsWord(cst.tokens[third], "FILESPACE")) {
      push_ref_at(third + 1);
    }
    return refs;
  }

  if (IsWord(cst.tokens[first_token], "STORAGE") &&
      IsWord(cst.tokens[second], "FILESPACE")) {
    push_ref_at(second + 1);
    return refs;
  }

  return refs;
}

std::vector<ObjectReference> ExtractDomainDdlObjectReferences(const CstDocument& cst,
                                                              std::size_t first_token) {
  std::vector<ObjectReference> refs;
  if (first_token >= cst.tokens.size()) return refs;
  const auto push_domain_ref_at = [&](std::size_t marker) {
    if (auto ref = ExtractObjectReferenceAt(cst, NextNonTriviaIndex(cst, marker))) {
      ref->object_class = "domain";
      refs.push_back(*ref);
    }
  };
  const auto second = NextNonTriviaIndex(cst, first_token + 1);
  if (second >= cst.tokens.size()) return refs;

  if ((IsWord(cst.tokens[first_token], "ALTER") ||
       IsWord(cst.tokens[first_token], "DROP")) &&
      IsWord(cst.tokens[second], "DOMAIN")) {
    push_domain_ref_at(second + 1);
    return refs;
  }

  if (IsWord(cst.tokens[first_token], "COMMENT") &&
      IsWord(cst.tokens[second], "ON")) {
    const auto third = NextNonTriviaIndex(cst, second + 1);
    if (third < cst.tokens.size() && IsWord(cst.tokens[third], "DOMAIN")) {
      push_domain_ref_at(third + 1);
    }
    return refs;
  }

  return refs;
}

std::vector<ObjectReference> ExtractSecurityDclObjectReferences(const CstDocument& cst,
                                                                std::size_t first_token) {
  std::vector<ObjectReference> refs;
  if (first_token >= cst.tokens.size() ||
      (!IsWord(cst.tokens[first_token], "GRANT") && !IsWord(cst.tokens[first_token], "REVOKE"))) {
    return refs;
  }
  const bool grant = IsWord(cst.tokens[first_token], "GRANT");
  bool has_on = false;
  std::size_t on_index = cst.tokens.size();
  std::size_t to_from_index = cst.tokens.size();
  for (std::size_t index = first_token + 1; index < cst.tokens.size(); ++index) {
    const auto& token = cst.tokens[index];
    if (IsTriviaToken(token)) continue;
    if (token.kind == TokenKind::kStatementTerminator || token.kind == TokenKind::kEnd) break;
    if (IsWord(token, "ON")) {
      has_on = true;
      on_index = index;
      continue;
    }
    if (IsWord(token, grant ? "TO" : "FROM")) {
      to_from_index = index;
      break;
    }
  }

  if (!has_on) {
    if (auto member = ExtractObjectReferenceAt(cst, first_token + 1)) {
      member->object_class = "role";
      refs.push_back(*member);
    }
    if (to_from_index < cst.tokens.size()) {
      std::size_t marker = NextNonTriviaIndex(cst, to_from_index + 1);
      std::string container_class = "group";
      if (marker < cst.tokens.size() && IsWord(cst.tokens[marker], "ROLE")) {
        container_class = "role";
        marker = NextNonTriviaIndex(cst, marker + 1);
      } else if (marker < cst.tokens.size() && IsWord(cst.tokens[marker], "GROUP")) {
        container_class = "group";
        marker = NextNonTriviaIndex(cst, marker + 1);
      }
      if (auto container = ExtractObjectReferenceAt(cst, marker)) {
        container->object_class = container_class;
        refs.push_back(*container);
      }
    }
    return refs;
  }

  if (on_index < cst.tokens.size()) {
    std::size_t marker = NextNonTriviaIndex(cst, on_index + 1);
    std::string target_class = "object";
    if (marker < cst.tokens.size()) {
      const std::string word = ToUpperAscii(cst.tokens[marker].text);
      if (word == "TABLE" || word == "VIEW" || word == "DOMAIN" ||
          word == "SEQUENCE" || word == "PROCEDURE" || word == "FUNCTION" ||
          word == "PACKAGE" || word == "INDEX") {
        target_class = LowerObjectReferenceName(cst.tokens[marker].text);
        marker = NextNonTriviaIndex(cst, marker + 1);
      }
    }
    if (auto target = ExtractObjectReferenceAt(cst, marker)) {
      target->object_class = target_class == "object" ? "relation" : target_class;
      refs.push_back(*target);
    }
  }
  if (to_from_index < cst.tokens.size()) {
    std::size_t marker = NextNonTriviaIndex(cst, to_from_index + 1);
    std::string grantee_class = "role";
    if (marker < cst.tokens.size() && IsWord(cst.tokens[marker], "ROLE")) {
      grantee_class = "role";
      marker = NextNonTriviaIndex(cst, marker + 1);
    } else if (marker < cst.tokens.size() && IsWord(cst.tokens[marker], "GROUP")) {
      grantee_class = "group";
      marker = NextNonTriviaIndex(cst, marker + 1);
    } else if (marker < cst.tokens.size() &&
               (IsWord(cst.tokens[marker], "USER") || IsWord(cst.tokens[marker], "PRINCIPAL"))) {
      grantee_class = "principal";
      marker = NextNonTriviaIndex(cst, marker + 1);
    }
    if (auto grantee = ExtractObjectReferenceAt(cst, marker)) {
      grantee->object_class = grantee_class;
      refs.push_back(*grantee);
    }
  }
  return refs;
}

std::vector<ObjectReference> ExtractSecurityPolicyObjectReferences(const CstDocument& cst,
                                                                   std::size_t first_token) {
  std::vector<ObjectReference> refs;
  if (first_token >= cst.tokens.size()) return refs;
  const auto push_ref_at = [&](std::size_t marker, std::string object_class) {
    if (auto ref = ExtractObjectReferenceAt(cst, NextNonTriviaIndex(cst, marker))) {
      ref->object_class = std::move(object_class);
      refs.push_back(*ref);
    }
  };

  if (IsWord(cst.tokens[first_token], "DROP")) {
    const std::size_t second = NextNonTriviaIndex(cst, first_token + 1);
    if (second >= cst.tokens.size()) return refs;
    if (IsWord(cst.tokens[second], "ROLE")) {
      push_ref_at(second + 1, "role");
    } else if (IsWord(cst.tokens[second], "GROUP")) {
      push_ref_at(second + 1, "group");
    } else if (IsWord(cst.tokens[second], "POLICY")) {
      push_ref_at(second + 1, "policy");
    } else if (IsWord(cst.tokens[second], "MASK")) {
      push_ref_at(second + 1, "mask");
    } else if (IsWord(cst.tokens[second], "RLS")) {
      push_ref_at(second + 1, "rls");
    }
    return refs;
  }

  if (!IsWord(cst.tokens[first_token], "CREATE")) return refs;
  const std::size_t second = NextNonTriviaIndex(cst, first_token + 1);
  if (second >= cst.tokens.size()) return refs;
  if (!IsWord(cst.tokens[second], "POLICY") &&
      !IsWord(cst.tokens[second], "MASK") &&
      !IsWord(cst.tokens[second], "RLS")) {
    return refs;
  }
  for (std::size_t index = second + 1; index < cst.tokens.size(); ++index) {
    const auto& token = cst.tokens[index];
    if (IsTriviaToken(token)) continue;
    if (token.kind == TokenKind::kStatementTerminator || token.kind == TokenKind::kEnd) break;
    if (IsWord(token, "ON")) {
      std::size_t marker = NextNonTriviaIndex(cst, index + 1);
      std::string target_class = "relation";
      bool drop_leaf = false;
      if (marker < cst.tokens.size()) {
        if (IsWord(cst.tokens[marker], "COLUMN")) {
          target_class = "relation";
          drop_leaf = true;
          marker = NextNonTriviaIndex(cst, marker + 1);
        } else if (IsWord(cst.tokens[marker], "TABLE")) {
          target_class = "table";
          marker = NextNonTriviaIndex(cst, marker + 1);
        } else if (IsWord(cst.tokens[marker], "VIEW")) {
          target_class = "view";
          marker = NextNonTriviaIndex(cst, marker + 1);
        }
      }
      if (auto ref = ExtractObjectReferenceAt(cst, marker)) {
        if (drop_leaf) DropObjectReferenceLeaf(&*ref);
        ref->object_class = std::move(target_class);
        refs.push_back(*ref);
      }
      continue;
    }
    if (IsWord(token, "TO")) {
      std::size_t marker = NextNonTriviaIndex(cst, index + 1);
      std::string subject_class = "role";
      if (marker < cst.tokens.size() && IsWord(cst.tokens[marker], "ROLE")) {
        subject_class = "role";
        marker = NextNonTriviaIndex(cst, marker + 1);
      } else if (marker < cst.tokens.size() && IsWord(cst.tokens[marker], "GROUP")) {
        subject_class = "group";
        marker = NextNonTriviaIndex(cst, marker + 1);
      } else if (marker < cst.tokens.size() &&
                 (IsWord(cst.tokens[marker], "USER") || IsWord(cst.tokens[marker], "PRINCIPAL"))) {
        subject_class = "principal";
        marker = NextNonTriviaIndex(cst, marker + 1);
      }
      push_ref_at(marker, subject_class);
    }
  }
  return refs;
}

std::vector<ObjectReference> ExtractObjectReferences(const CstDocument& cst) {
  std::vector<ObjectReference> refs;
  auto local_cte_names = ExtractLeadingCteNames(cst);
  for (const auto& name : ExtractDerivedCteNames(cst)) {
    if (std::find(local_cte_names.begin(), local_cte_names.end(), name) ==
        local_cte_names.end()) {
      local_cte_names.push_back(name);
    }
  }
  std::size_t first_token = cst.tokens.size();
  for (std::size_t i = 0; i < cst.tokens.size(); ++i) {
    if (!IsTriviaToken(cst.tokens[i])) {
      first_token = i;
      break;
    }
  }
  if (first_token == cst.tokens.size()) return refs;

  if (IsWord(cst.tokens[first_token], "EXECUTE")) {
    std::size_t second = first_token + 1;
    while (second < cst.tokens.size() && IsTriviaToken(cst.tokens[second])) ++second;
    if (second < cst.tokens.size() && IsWord(cst.tokens[second], "PROCEDURE")) {
      if (auto ref = ExtractObjectReferenceAt(cst, second + 1)) {
        ref->object_class = "procedure";
        refs.push_back(*ref);
      }
      return refs;
    }
  }

  if (IsWord(cst.tokens[first_token], "COPY")) {
    if (auto ref = ExtractObjectReferenceAt(cst, first_token + 1)) refs.push_back(*ref);
    return refs;
  }
  if (IsWord(cst.tokens[first_token], "MERGE")) {
    return ExtractMergeObjectReferences(cst, first_token);
  }
  if (IsWord(cst.tokens[first_token], "INSERT") ||
      IsWord(cst.tokens[first_token], "UPSERT")) {
    return ExtractInsertObjectReferences(cst, first_token);
  }
  if (IsWord(cst.tokens[first_token], "CREATE")) {
    auto create_index_refs = ExtractCreateIndexObjectReferences(cst, first_token);
    if (!create_index_refs.empty()) {
      return create_index_refs;
    }
  }

  auto multimodel_refs = ExtractMultimodelObjectReferences(cst, first_token);
  if (!multimodel_refs.empty()) {
    return multimodel_refs;
  }

  auto filespace_refs = ExtractFilespaceObjectReferences(cst, first_token);
  if (!filespace_refs.empty()) {
    return filespace_refs;
  }

  auto domain_ddl_refs = ExtractDomainDdlObjectReferences(cst, first_token);
  if (!domain_ddl_refs.empty()) {
    return domain_ddl_refs;
  }

  auto security_dcl_refs = ExtractSecurityDclObjectReferences(cst, first_token);
  if (!security_dcl_refs.empty()) {
    return security_dcl_refs;
  }

  auto security_policy_refs = ExtractSecurityPolicyObjectReferences(cst, first_token);
  if (!security_policy_refs.empty()) {
    return security_policy_refs;
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
    if (IsLocalCteReference(*ref, local_cte_names)) continue;
    refs.push_back(*ref);
    if (refs.size() >= 8) break;
  }

  if (refs.empty()) {
    if (auto ref = ExtractFirstObjectReference(cst)) {
      if (!IsLocalCteReference(*ref, local_cte_names)) refs.push_back(*ref);
    }
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

bool ConsumeRouteQualifiedNameParts(const CstDocument& cst,
                                    std::size_t* index,
                                    std::vector<std::string>* parts,
                                    bool* quoted = nullptr) {
  if (index == nullptr || parts == nullptr) return false;
  std::vector<std::string> parsed;
  bool saw_quoted = false;
  bool expect_part = true;
  for (;;) {
    SkipTriviaTokens(cst, index);
    if (*index >= cst.tokens.size()) break;
    const auto& token = cst.tokens[*index];
    if (token.kind == TokenKind::kEnd || token.kind == TokenKind::kStatementTerminator) break;
    if (expect_part) {
      if (!IsIdentifierLikeForRouteExecution(token)) break;
      parsed.push_back(token.text);
      saw_quoted = saw_quoted || token.quoted;
      expect_part = false;
      ++(*index);
      continue;
    }
    if (token.text != ".") break;
    expect_part = true;
    ++(*index);
  }
  if (parsed.empty() || expect_part) return false;
  *parts = std::move(parsed);
  if (quoted != nullptr) *quoted = saw_quoted;
  return true;
}

std::string JoinRouteNameParts(const std::vector<std::string>& parts,
                               std::size_t begin,
                               std::size_t end) {
  std::string out;
  for (std::size_t index = begin; index < end && index < parts.size(); ++index) {
    if (!out.empty()) out.push_back('.');
    out += parts[index];
  }
  return out;
}

bool ConsumeOptionalIfNotExists(const CstDocument& cst, std::size_t* index) {
  if (index == nullptr) return false;
  const std::size_t saved = *index;
  if (!ConsumeRouteKeyword(cst, index, "IF")) {
    *index = saved;
    return true;
  }
  if (!ConsumeRouteKeyword(cst, index, "NOT") ||
      !ConsumeRouteKeyword(cst, index, "EXISTS")) {
    *index = saved;
  }
  return true;
}

std::string RouteCanonicalTypeName(std::string_view type_text) {
  const std::string upper = ToUpperAscii(type_text);
  if (upper == "INT" || upper == "INTEGER") return "int";
  if (upper == "SMALLINT") return "smallint";
  if (upper == "BIGINT") return "bigint";
  if (upper == "DOUBLE PRECISION") return "double";
  if (upper == "FLOAT") return "float";
  if (upper == "DOUBLE") return "double";
  if (upper == "TEXT" || upper == "VARCHAR" || upper == "CHAR") return "text";
  if (upper == "CHARACTER") return "text";
  if (upper == "CHARACTER VARYING") return "text";
  if (upper == "BIT VARYING") return "bit_varying";
  if (upper == "BOOL") return "boolean";
  if (upper == "DEC" || upper == "NUMERIC") return "decimal";
  std::string lowered(type_text);
  for (auto& ch : lowered) {
    if (std::isspace(static_cast<unsigned char>(ch))) {
      ch = '_';
    } else {
      ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
  }
  return lowered;
}

std::string RouteTokenText(const Token& token) {
  return token.raw_text.empty() ? token.text : token.raw_text;
}

void AppendRouteTokenText(std::string* out, const Token& token) {
  if (out == nullptr) return;
  const std::string text = RouteTokenText(token);
  if (text.empty()) return;
  const bool punctuation = text == "(" || text == ")" || text == "," ||
                           text == "<" || text == ">" || text == "." ||
                           text == "[" || text == "]";
  const bool previous_punctuation =
      !out->empty() && (out->back() == '(' || out->back() == '<' ||
                        out->back() == '.' || out->back() == '[');
  if (!out->empty() && !punctuation && !previous_punctuation) {
    out->push_back(' ');
  }
  if ((text == ")" || text == ">" || text == "]" || text == ",") &&
      !out->empty() && out->back() == ' ') {
    out->pop_back();
  }
  *out += text;
  if (text == ",") out->push_back(' ');
}

std::string TrimRouteText(std::string text) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
    text.erase(text.begin());
  }
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
    text.pop_back();
  }
  return text;
}

bool IsColumnConstraintStarter(const Token& token) {
  return IsWord(token, "PRIMARY") || IsWord(token, "NOT") ||
         IsWord(token, "NULL") || IsWord(token, "DEFAULT") ||
         IsWord(token, "CHECK") || IsWord(token, "UNIQUE") ||
         IsWord(token, "REFERENCES") || IsWord(token, "COLLATE") ||
         IsWord(token, "GENERATED") || IsWord(token, "CONSTRAINT");
}

bool IsTableConstraintStarter(const Token& token) {
  return IsWord(token, "PRIMARY") || IsWord(token, "FOREIGN") ||
         IsWord(token, "UNIQUE") || IsWord(token, "CHECK") ||
         IsWord(token, "CONSTRAINT");
}

bool ConsumeBalancedRouteClause(const CstDocument& cst,
                                std::size_t* index,
                                std::string* text) {
  if (index == nullptr) return false;
  int paren_depth = 0;
  int angle_depth = 0;
  bool consumed = false;
  if (text != nullptr) text->clear();
  for (;;) {
    SkipTriviaTokens(cst, index);
    if (*index >= cst.tokens.size()) break;
    const auto& token = cst.tokens[*index];
    if (token.kind == TokenKind::kEnd || token.kind == TokenKind::kStatementTerminator) break;
    if (paren_depth == 0 && angle_depth == 0 && (token.text == "," || token.text == ")")) break;
    if (token.text == "(") {
      ++paren_depth;
    } else if (token.text == ")" && paren_depth > 0) {
      --paren_depth;
    } else if (token.text == "<") {
      ++angle_depth;
    } else if (token.text == ">" && angle_depth > 0) {
      --angle_depth;
    }
    if (text != nullptr) AppendRouteTokenText(text, token);
    consumed = true;
    ++(*index);
  }
  if (text != nullptr) *text = TrimRouteText(*text);
  return consumed;
}

std::string ConsumeRouteTypeText(const CstDocument& cst,
                                 std::size_t* index,
                                 std::string* raw_type_text) {
  if (raw_type_text != nullptr) raw_type_text->clear();
  SkipTriviaTokens(cst, index);
  if (index == nullptr || *index >= cst.tokens.size() ||
      !IsIdentifierLikeForRouteExecution(cst.tokens[*index])) {
    return {};
  }

  std::vector<std::string> type_words;
  std::string rendered;
  int paren_depth = 0;
  int angle_depth = 0;
  for (;;) {
    SkipTriviaTokens(cst, index);
    if (*index >= cst.tokens.size()) break;
    const auto& token = cst.tokens[*index];
    if (token.kind == TokenKind::kEnd || token.kind == TokenKind::kStatementTerminator) break;
    if (paren_depth == 0 && angle_depth == 0) {
      if (token.text == "," || token.text == ")" || IsColumnConstraintStarter(token)) break;
    }
    if (token.text == "(") {
      ++paren_depth;
    } else if (token.text == ")" && paren_depth > 0) {
      --paren_depth;
    } else if (token.text == "<") {
      ++angle_depth;
    } else if (token.text == ">" && angle_depth > 0) {
      --angle_depth;
    } else if (paren_depth == 0 && angle_depth == 0 &&
               IsIdentifierLikeForRouteExecution(token)) {
      type_words.push_back(token.text);
    }
    AppendRouteTokenText(&rendered, token);
    ++(*index);
  }
  rendered = TrimRouteText(rendered);
  if (raw_type_text != nullptr) *raw_type_text = rendered;
  if (type_words.empty()) return {};
  std::string canonical_input = type_words.front();
  if (type_words.size() > 1) {
    const std::string first_two = ToUpperAscii(type_words[0] + " " + type_words[1]);
    if (first_two == "DOUBLE PRECISION" ||
        first_two == "BIT VARYING" ||
        first_two == "CHARACTER VARYING") {
      canonical_input = type_words[0] + " " + type_words[1];
    }
  }
  return RouteCanonicalTypeName(canonical_input);
}

struct RouteColumnDefinition {
  std::string name;
  std::string canonical_type;
  std::string raw_type;
  bool nullable = true;
  bool primary_key = false;
  bool unique = false;
  std::string default_expression;
};

void AppendDescriptorFlag(std::string* descriptor,
                          std::string_view name,
                          std::string_view value) {
  if (descriptor == nullptr || name.empty()) return;
  if (!descriptor->empty() && descriptor->back() != ';') descriptor->push_back(';');
  descriptor->append(name);
  descriptor->push_back('=');
  descriptor->append(value);
}

bool ConsumeColumnConstraints(const CstDocument& cst,
                              std::size_t* index,
                              RouteColumnDefinition* column) {
  if (index == nullptr || column == nullptr) return false;
  for (;;) {
    SkipTriviaTokens(cst, index);
    if (*index >= cst.tokens.size()) return false;
    const auto& token = cst.tokens[*index];
    if (token.kind == TokenKind::kEnd || token.kind == TokenKind::kStatementTerminator ||
        token.text == "," || token.text == ")") {
      return true;
    }
    if (ConsumeRouteKeyword(cst, index, "CONSTRAINT")) {
      std::string ignored_name;
      if (!ConsumeRouteIdentifier(cst, index, &ignored_name)) return false;
      continue;
    }
    if (ConsumeRouteKeyword(cst, index, "PRIMARY")) {
      if (!ConsumeRouteKeyword(cst, index, "KEY")) return false;
      column->primary_key = true;
      column->unique = true;
      column->nullable = false;
      continue;
    }
    if (ConsumeRouteKeyword(cst, index, "UNIQUE")) {
      column->unique = true;
      continue;
    }
    if (ConsumeRouteKeyword(cst, index, "NOT")) {
      if (!ConsumeRouteKeyword(cst, index, "NULL")) return false;
      column->nullable = false;
      continue;
    }
    if (ConsumeRouteKeyword(cst, index, "NULL")) {
      column->nullable = true;
      continue;
    }
    if (ConsumeRouteKeyword(cst, index, "DEFAULT")) {
      std::string expression;
      if (!ConsumeBalancedRouteClause(cst, index, &expression)) return false;
      column->default_expression = std::move(expression);
      continue;
    }
    if (ConsumeRouteKeyword(cst, index, "CHECK")) {
      std::string ignored;
      if (!ConsumeBalancedRouteClause(cst, index, &ignored)) return false;
      continue;
    }
    if (ConsumeRouteKeyword(cst, index, "REFERENCES") ||
        ConsumeRouteKeyword(cst, index, "COLLATE") ||
        ConsumeRouteKeyword(cst, index, "GENERATED")) {
      std::string ignored;
      ConsumeBalancedRouteClause(cst, index, &ignored);
      continue;
    }
    return false;
  }
}

bool ConsumeTableConstraint(const CstDocument& cst,
                            std::size_t* index,
                            std::vector<RouteColumnDefinition>* columns) {
  if (index == nullptr || columns == nullptr) return false;
  SkipTriviaTokens(cst, index);
  if (*index >= cst.tokens.size()) return false;
  if (ConsumeRouteKeyword(cst, index, "CONSTRAINT")) {
    std::string ignored_name;
    if (!ConsumeRouteIdentifier(cst, index, &ignored_name)) return false;
  }
  if (ConsumeRouteKeyword(cst, index, "PRIMARY")) {
    if (!ConsumeRouteKeyword(cst, index, "KEY")) return false;
    if (!ConsumeRouteSymbol(cst, index, "(")) return false;
    for (;;) {
      std::string column_name;
      if (!ConsumeRouteIdentifier(cst, index, &column_name)) return false;
      for (auto& column : *columns) {
        if (ToUpperAscii(column.name) == ToUpperAscii(column_name)) {
          column.primary_key = true;
          column.unique = true;
          column.nullable = false;
        }
      }
      if (ConsumeRouteSymbol(cst, index, ")")) break;
      if (!ConsumeRouteSymbol(cst, index, ",")) return false;
    }
    return true;
  }
  if (ConsumeRouteKeyword(cst, index, "UNIQUE")) {
    if (ConsumeRouteSymbol(cst, index, "(")) {
      for (;;) {
        std::string column_name;
        if (!ConsumeRouteIdentifier(cst, index, &column_name)) return false;
        for (auto& column : *columns) {
          if (ToUpperAscii(column.name) == ToUpperAscii(column_name)) {
            column.unique = true;
          }
        }
        if (ConsumeRouteSymbol(cst, index, ")")) break;
        if (!ConsumeRouteSymbol(cst, index, ",")) return false;
      }
      return true;
    }
  }
  std::string ignored;
  return ConsumeBalancedRouteClause(cst, index, &ignored);
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

std::string HexEncodeRouteText(std::string_view value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(value.size() * 2u);
  for (const unsigned char ch : value) {
    out.push_back(kHex[(ch >> 4u) & 0x0fu]);
    out.push_back(kHex[ch & 0x0fu]);
  }
  return out;
}

struct FastInsertValueField {
  std::string name;
  std::string type_name;
  std::string value;
  bool is_null{false};
};

struct FastInsertValuesRoutePlan {
  ObjectReference target;
  std::vector<std::vector<FastInsertValueField>> rows;
  std::size_t column_count{0};
};

struct FastCopyFromStdinRoutePlan {
  ObjectReference target;
  std::string format_family{"csv"};
  bool copy_options_present{false};
  bool copy_header_option{false};
};

std::string FastInsertTypedLiteralType(std::string upper);

class FastCopyFromStdinScanner {
public:
  explicit FastCopyFromStdinScanner(std::string_view sql) : sql_(sql) {}

  std::optional<FastCopyFromStdinRoutePlan> Parse() {
    if (!ConsumeKeyword("COPY")) return std::nullopt;

    std::vector<std::string> target_parts;
    bool target_quoted = false;
    if (!ConsumeQualifiedNameParts(&target_parts, &target_quoted)) return std::nullopt;

    if (!ConsumeKeyword("FROM")) return std::nullopt;
    if (!ConsumeKeyword("STDIN")) return std::nullopt;

    FastCopyFromStdinRoutePlan plan;
    plan.target.presented_name = JoinRouteNameParts(target_parts, 0, target_parts.size());
    plan.target.quoted = target_quoted;
    plan.target.object_class = "relation";

    if (ConsumeKeyword("WITH")) {
      plan.copy_options_present = true;
      if (!ConsumeCopyOptions(&plan)) return std::nullopt;
    }

    SkipTrivia();
    while (pos_ < sql_.size() && sql_[pos_] == ';') {
      ++pos_;
      SkipTrivia();
    }
    if (pos_ != sql_.size()) return std::nullopt;
    return plan;
  }

private:
  void SkipTrivia() {
    for (;;) {
      while (pos_ < sql_.size() &&
             std::isspace(static_cast<unsigned char>(sql_[pos_]))) {
        ++pos_;
      }
      if (pos_ + 1 < sql_.size() && sql_[pos_] == '-' && sql_[pos_ + 1] == '-') {
        pos_ += 2;
        while (pos_ < sql_.size() && sql_[pos_] != '\n') ++pos_;
        continue;
      }
      if (pos_ + 1 < sql_.size() && sql_[pos_] == '/' && sql_[pos_ + 1] == '*') {
        pos_ += 2;
        while (pos_ + 1 < sql_.size() &&
               !(sql_[pos_] == '*' && sql_[pos_ + 1] == '/')) {
          ++pos_;
        }
        if (pos_ + 1 >= sql_.size()) {
          pos_ = sql_.size();
          return;
        }
        pos_ += 2;
        continue;
      }
      break;
    }
  }

  bool IsIdentifierStart(char ch) const {
    const auto uch = static_cast<unsigned char>(ch);
    return std::isalpha(uch) || ch == '_';
  }

  bool IsIdentifierBody(char ch) const {
    const auto uch = static_cast<unsigned char>(ch);
    return std::isalnum(uch) || ch == '_' || ch == '$';
  }

  bool KeywordBoundary(std::size_t end) const {
    return end >= sql_.size() || !IsIdentifierBody(sql_[end]);
  }

  bool ConsumeKeyword(std::string_view keyword) {
    SkipTrivia();
    if (pos_ + keyword.size() > sql_.size()) return false;
    for (std::size_t offset = 0; offset < keyword.size(); ++offset) {
      const auto ch = static_cast<unsigned char>(sql_[pos_ + offset]);
      if (std::toupper(ch) != keyword[offset]) return false;
    }
    const std::size_t end = pos_ + keyword.size();
    if (!KeywordBoundary(end)) return false;
    pos_ = end;
    return true;
  }

  bool ConsumeChar(char expected) {
    SkipTrivia();
    if (pos_ >= sql_.size() || sql_[pos_] != expected) return false;
    ++pos_;
    return true;
  }

  bool ConsumeIdentifier(std::string* out, bool* quoted) {
    if (out == nullptr || quoted == nullptr) return false;
    SkipTrivia();
    if (pos_ >= sql_.size()) return false;
    out->clear();
    *quoted = false;
    if (sql_[pos_] == '"') {
      *quoted = true;
      ++pos_;
      while (pos_ < sql_.size()) {
        const char ch = sql_[pos_++];
        if (ch == '"') {
          if (pos_ < sql_.size() && sql_[pos_] == '"') {
            out->push_back('"');
            ++pos_;
            continue;
          }
          return true;
        }
        out->push_back(ch);
      }
      return false;
    }
    if (!IsIdentifierStart(sql_[pos_])) return false;
    const std::size_t begin = pos_++;
    while (pos_ < sql_.size() && IsIdentifierBody(sql_[pos_])) ++pos_;
    out->assign(sql_.substr(begin, pos_ - begin));
    return true;
  }

  bool ConsumeQualifiedNameParts(std::vector<std::string>* parts, bool* quoted) {
    if (parts == nullptr || quoted == nullptr) return false;
    parts->clear();
    *quoted = false;
    for (;;) {
      std::string part;
      bool part_quoted = false;
      if (!ConsumeIdentifier(&part, &part_quoted)) return false;
      *quoted = *quoted || part_quoted;
      parts->push_back(std::move(part));
      SkipTrivia();
      if (pos_ >= sql_.size() || sql_[pos_] != '.') break;
      ++pos_;
    }
    return !parts->empty();
  }

  bool ConsumeSingleQuoted(std::string* out) {
    if (out == nullptr) return false;
    SkipTrivia();
    if (pos_ >= sql_.size() || sql_[pos_] != '\'') return false;
    out->clear();
    ++pos_;
    while (pos_ < sql_.size()) {
      const char ch = sql_[pos_++];
      if (ch == '\'') {
        if (pos_ < sql_.size() && sql_[pos_] == '\'') {
          out->push_back('\'');
          ++pos_;
          continue;
        }
        return true;
      }
      out->push_back(ch);
    }
    return false;
  }

  bool ConsumeOptionValue(std::string* value) {
    if (value == nullptr) return false;
    SkipTrivia();
    if (ConsumeSingleQuoted(value)) return true;
    bool quoted = false;
    if (ConsumeIdentifier(value, &quoted)) {
      (void)quoted;
      return true;
    }
    if (pos_ >= sql_.size()) return false;
    const std::size_t begin = pos_;
    if (sql_[pos_] == '+' || sql_[pos_] == '-') ++pos_;
    bool saw_digit = false;
    while (pos_ < sql_.size() &&
           std::isdigit(static_cast<unsigned char>(sql_[pos_]))) {
      saw_digit = true;
      ++pos_;
    }
    if (!saw_digit) {
      pos_ = begin;
      return false;
    }
    value->assign(sql_.substr(begin, pos_ - begin));
    return true;
  }

  bool ConsumeCopyOptions(FastCopyFromStdinRoutePlan* plan) {
    if (plan == nullptr) return false;
    if (!ConsumeChar('(')) {
      if (ConsumeKeyword("HEADER")) {
        plan->copy_header_option = true;
        return true;
      }
      return false;
    }
    for (;;) {
      std::string option;
      bool quoted = false;
      if (!ConsumeIdentifier(&option, &quoted) || quoted) return false;
      const std::string upper_option = ToUpperAscii(option);
      std::string value;
      if (ConsumeChar('=')) {
        if (!ConsumeOptionValue(&value)) return false;
      } else {
        const std::size_t saved = pos_;
        if (!ConsumeOptionValue(&value)) {
          pos_ = saved;
        }
      }
      if (upper_option == "HEADER") {
        plan->copy_header_option = true;
      } else if (upper_option == "FORMAT" && !value.empty()) {
        const std::string upper_value = ToUpperAscii(value);
        if (upper_value == "JSONL" || upper_value == "JSON") {
          plan->format_family = "jsonl";
        } else if (upper_value == "CSV") {
          plan->format_family = "csv";
        } else {
          return false;
        }
      } else if (upper_option != "NATIVE_BULK_INGEST" &&
                 upper_option != "NATIVE_BULK_INGEST_ENABLED") {
        return false;
      }
      if (ConsumeChar(',')) continue;
      if (!ConsumeChar(')')) return false;
      break;
    }
    return true;
  }

  std::string_view sql_;
  std::size_t pos_{0};
};

class FastInsertValuesScanner {
public:
  explicit FastInsertValuesScanner(std::string_view sql) : sql_(sql) {}

  std::optional<FastInsertValuesRoutePlan> Parse() {
    if (!ConsumeKeyword("INSERT")) return std::nullopt;
    if (!ConsumeKeyword("INTO")) return std::nullopt;

    std::vector<std::string> target_parts;
    bool target_quoted = false;
    if (!ConsumeQualifiedNameParts(&target_parts, &target_quoted)) return std::nullopt;

    std::vector<std::string> column_names;
    if (!ConsumeChar('(')) return std::nullopt;
    for (;;) {
      std::string column_name;
      bool column_quoted = false;
      if (!ConsumeIdentifier(&column_name, &column_quoted)) return std::nullopt;
      (void)column_quoted;
      column_names.push_back(std::move(column_name));
      if (ConsumeChar(',')) continue;
      if (!ConsumeChar(')')) return std::nullopt;
      break;
    }
    if (column_names.empty()) return std::nullopt;
    if (!ConsumeKeyword("VALUES")) return std::nullopt;

    FastInsertValuesRoutePlan plan;
    plan.target.presented_name =
        JoinRouteNameParts(target_parts, 0, target_parts.size());
    plan.target.quoted = target_quoted;
    plan.target.object_class = "relation";
    plan.column_count = column_names.size();

    for (;;) {
      if (!ConsumeChar('(')) return std::nullopt;
      std::vector<FastInsertValueField> row;
      row.reserve(column_names.size());
      for (std::size_t column_index = 0; column_index < column_names.size(); ++column_index) {
        FastInsertValueField field;
        field.name = column_names[column_index];
        if (!ConsumeLiteralValue(&field)) return std::nullopt;
        row.push_back(std::move(field));
        if (column_index + 1 < column_names.size() && !ConsumeChar(',')) {
          return std::nullopt;
        }
      }
      if (!ConsumeChar(')')) return std::nullopt;
      plan.rows.push_back(std::move(row));
      if (ConsumeChar(',')) continue;
      break;
    }

    if (plan.rows.empty()) return std::nullopt;
    SkipTrivia();
    while (pos_ < sql_.size() && sql_[pos_] == ';') {
      ++pos_;
      SkipTrivia();
    }
    if (pos_ != sql_.size()) return std::nullopt;
    return plan;
  }

private:
  void SkipTrivia() {
    for (;;) {
      while (pos_ < sql_.size() &&
             std::isspace(static_cast<unsigned char>(sql_[pos_]))) {
        ++pos_;
      }
      if (pos_ + 1 < sql_.size() && sql_[pos_] == '-' && sql_[pos_ + 1] == '-') {
        pos_ += 2;
        while (pos_ < sql_.size() && sql_[pos_] != '\n') ++pos_;
        continue;
      }
      if (pos_ + 1 < sql_.size() && sql_[pos_] == '/' && sql_[pos_ + 1] == '*') {
        pos_ += 2;
        while (pos_ + 1 < sql_.size() &&
               !(sql_[pos_] == '*' && sql_[pos_ + 1] == '/')) {
          ++pos_;
        }
        if (pos_ + 1 >= sql_.size()) {
          pos_ = sql_.size();
          return;
        }
        pos_ += 2;
        continue;
      }
      break;
    }
  }

  bool IsIdentifierStart(char ch) const {
    const auto uch = static_cast<unsigned char>(ch);
    return std::isalpha(uch) || ch == '_';
  }

  bool IsIdentifierBody(char ch) const {
    const auto uch = static_cast<unsigned char>(ch);
    return std::isalnum(uch) || ch == '_' || ch == '$';
  }

  bool KeywordBoundary(std::size_t end) const {
    return end >= sql_.size() || !IsIdentifierBody(sql_[end]);
  }

  bool ConsumeKeyword(std::string_view keyword) {
    SkipTrivia();
    if (pos_ + keyword.size() > sql_.size()) return false;
    for (std::size_t offset = 0; offset < keyword.size(); ++offset) {
      const auto ch = static_cast<unsigned char>(sql_[pos_ + offset]);
      if (std::toupper(ch) != keyword[offset]) return false;
    }
    const std::size_t end = pos_ + keyword.size();
    if (!KeywordBoundary(end)) return false;
    pos_ = end;
    return true;
  }

  bool ConsumeChar(char expected) {
    SkipTrivia();
    if (pos_ >= sql_.size() || sql_[pos_] != expected) return false;
    ++pos_;
    return true;
  }

  bool ConsumeIdentifier(std::string* out, bool* quoted) {
    if (out == nullptr || quoted == nullptr) return false;
    SkipTrivia();
    if (pos_ >= sql_.size()) return false;
    out->clear();
    *quoted = false;
    if (sql_[pos_] == '"') {
      *quoted = true;
      ++pos_;
      while (pos_ < sql_.size()) {
        const char ch = sql_[pos_++];
        if (ch == '"') {
          if (pos_ < sql_.size() && sql_[pos_] == '"') {
            out->push_back('"');
            ++pos_;
            continue;
          }
          return true;
        }
        out->push_back(ch);
      }
      return false;
    }
    if (!IsIdentifierStart(sql_[pos_])) return false;
    const std::size_t begin = pos_++;
    while (pos_ < sql_.size() && IsIdentifierBody(sql_[pos_])) ++pos_;
    out->assign(sql_.substr(begin, pos_ - begin));
    return true;
  }

  bool ConsumeQualifiedNameParts(std::vector<std::string>* parts, bool* quoted) {
    if (parts == nullptr || quoted == nullptr) return false;
    parts->clear();
    *quoted = false;
    for (;;) {
      std::string part;
      bool part_quoted = false;
      if (!ConsumeIdentifier(&part, &part_quoted)) return false;
      *quoted = *quoted || part_quoted;
      parts->push_back(std::move(part));
      SkipTrivia();
      if (pos_ >= sql_.size() || sql_[pos_] != '.') break;
      ++pos_;
    }
    return !parts->empty();
  }

  bool ConsumeSingleQuoted(std::string* out) {
    if (out == nullptr) return false;
    SkipTrivia();
    if (pos_ >= sql_.size() || sql_[pos_] != '\'') return false;
    out->clear();
    ++pos_;
    while (pos_ < sql_.size()) {
      const char ch = sql_[pos_++];
      if (ch == '\'') {
        if (pos_ < sql_.size() && sql_[pos_] == '\'') {
          out->push_back('\'');
          ++pos_;
          continue;
        }
        return true;
      }
      out->push_back(ch);
    }
    return false;
  }

  bool ConsumeUuidLiteral(std::string* out) {
    if (out == nullptr) return false;
    SkipTrivia();
    constexpr std::size_t kUuidTextSize = 36;
    if (pos_ + kUuidTextSize > sql_.size()) return false;
    const auto is_hex = [](char ch) {
      const auto uch = static_cast<unsigned char>(ch);
      return std::isxdigit(uch) != 0;
    };
    for (std::size_t offset = 0; offset < kUuidTextSize; ++offset) {
      const char ch = sql_[pos_ + offset];
      if (offset == 8 || offset == 13 || offset == 18 || offset == 23) {
        if (ch != '-') return false;
      } else if (!is_hex(ch)) {
        return false;
      }
    }
    const std::size_t end = pos_ + kUuidTextSize;
    if (end < sql_.size() && IsIdentifierBody(sql_[end])) return false;
    out->assign(sql_.substr(pos_, kUuidTextSize));
    pos_ = end;
    return true;
  }

  std::string NumericTypeForLiteral(std::string_view text) const {
    const std::string upper = ToUpperAscii(text);
    const auto has_suffix = [&](std::string_view suffix) {
      return upper.size() > suffix.size() &&
             upper.compare(upper.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    if (has_suffix("UINT128") || has_suffix("U128")) return "uint128";
    if (has_suffix("INT128") || has_suffix("I128")) return "int128";
    if (has_suffix("REAL128") || has_suffix("R128")) return "real128";
    if (has_suffix("UINT") || has_suffix("U")) return "uint64";
    if (has_suffix("DECIMAL") || has_suffix("DEC") || has_suffix("D") ||
        has_suffix("DOUBLE") || has_suffix("FLOAT") || has_suffix("F")) {
      return "numeric";
    }
    return upper.find('.') != std::string::npos ||
                   upper.find('E') != std::string::npos
               ? "numeric"
               : "bigint";
  }

  bool ConsumeNumericLiteral(std::string* value, std::string* type_name) {
    if (value == nullptr || type_name == nullptr) return false;
    SkipTrivia();
    std::size_t cursor = pos_;
    if (cursor >= sql_.size()) return false;
    if (sql_[cursor] == '+' || sql_[cursor] == '-') ++cursor;
    const std::size_t digits_begin = cursor;
    while (cursor < sql_.size() &&
           std::isdigit(static_cast<unsigned char>(sql_[cursor]))) {
      ++cursor;
    }
    bool saw_digit = cursor > digits_begin;
    if (cursor < sql_.size() && sql_[cursor] == '.') {
      ++cursor;
      const std::size_t fraction_begin = cursor;
      while (cursor < sql_.size() &&
             std::isdigit(static_cast<unsigned char>(sql_[cursor]))) {
        ++cursor;
      }
      saw_digit = saw_digit || cursor > fraction_begin;
    }
    if (!saw_digit) return false;
    if (cursor < sql_.size() && (sql_[cursor] == 'e' || sql_[cursor] == 'E')) {
      std::size_t exponent = cursor + 1;
      if (exponent < sql_.size() && (sql_[exponent] == '+' || sql_[exponent] == '-')) {
        ++exponent;
      }
      const std::size_t exponent_digits = exponent;
      while (exponent < sql_.size() &&
             std::isdigit(static_cast<unsigned char>(sql_[exponent]))) {
        ++exponent;
      }
      if (exponent == exponent_digits) return false;
      cursor = exponent;
    }
    const std::size_t suffix_begin = cursor;
    while (cursor < sql_.size() &&
           std::isalpha(static_cast<unsigned char>(sql_[cursor]))) {
      ++cursor;
    }
    if (cursor < sql_.size() && IsIdentifierBody(sql_[cursor])) return false;
    if (suffix_begin != cursor) {
      const std::string suffix =
          ToUpperAscii(sql_.substr(suffix_begin, cursor - suffix_begin));
      if (suffix != "UINT" && suffix != "U" && suffix != "INT128" &&
          suffix != "I128" && suffix != "UINT128" && suffix != "U128" &&
          suffix != "REAL128" && suffix != "R128" && suffix != "DECIMAL" &&
          suffix != "DEC" && suffix != "D" && suffix != "DOUBLE" &&
          suffix != "FLOAT" && suffix != "F") {
        return false;
      }
    }
    value->assign(sql_.substr(pos_, cursor - pos_));
    *type_name = NumericTypeForLiteral(*value);
    pos_ = cursor;
    return true;
  }

  bool ConsumeWord(std::string* out) {
    if (out == nullptr) return false;
    SkipTrivia();
    if (pos_ >= sql_.size() || !IsIdentifierStart(sql_[pos_])) return false;
    const std::size_t begin = pos_++;
    while (pos_ < sql_.size() && IsIdentifierBody(sql_[pos_])) ++pos_;
    out->assign(sql_.substr(begin, pos_ - begin));
    return true;
  }

  bool ConsumeLiteralValue(FastInsertValueField* field) {
    if (field == nullptr) return false;
    SkipTrivia();

    std::string uuid_text;
    if (ConsumeUuidLiteral(&uuid_text)) {
      field->type_name = "uuid";
      field->value = std::move(uuid_text);
      field->is_null = false;
      return true;
    }

    std::string string_value;
    if (ConsumeSingleQuoted(&string_value)) {
      field->type_name = "text";
      field->value = std::move(string_value);
      field->is_null = false;
      return true;
    }

    std::string number_value;
    std::string number_type;
    if (ConsumeNumericLiteral(&number_value, &number_type)) {
      field->type_name = std::move(number_type);
      field->value = std::move(number_value);
      field->is_null = false;
      return true;
    }

    std::size_t word_position = pos_;
    std::string word;
    if (!ConsumeWord(&word)) return false;
    const std::string upper = ToUpperAscii(word);
    if (upper == "TRUE" || upper == "FALSE") {
      field->type_name = "boolean";
      field->value = upper == "TRUE" ? "true" : "false";
      field->is_null = false;
      return true;
    }
    if (upper == "NULL") {
      field->type_name = "null";
      field->value.clear();
      field->is_null = true;
      return true;
    }
    const std::string typed_literal = FastInsertTypedLiteralType(upper);
    if (!typed_literal.empty()) {
      if (!ConsumeSingleQuoted(&string_value)) {
        pos_ = word_position;
        return false;
      }
      field->type_name = typed_literal;
      field->value = std::move(string_value);
      field->is_null = false;
      return true;
    }
    if ((upper == "X" || upper == "B") && ConsumeSingleQuoted(&string_value)) {
      field->type_name = upper == "B" ? "bit_string" : "binary";
      field->value = std::move(string_value);
      field->is_null = false;
      return true;
    }
    pos_ = word_position;
    return false;
  }

  std::string_view sql_;
  std::size_t pos_{0};
};

bool LooksLikeFastInsertValuesCandidate(std::string_view sql) {
  std::size_t index = 0;
  for (;;) {
    while (index < sql.size() &&
           std::isspace(static_cast<unsigned char>(sql[index]))) {
      ++index;
    }
    if (index + 1 < sql.size() && sql[index] == '-' && sql[index + 1] == '-') {
      index += 2;
      while (index < sql.size() && sql[index] != '\n') ++index;
      continue;
    }
    if (index + 1 < sql.size() && sql[index] == '/' && sql[index + 1] == '*') {
      index += 2;
      while (index + 1 < sql.size() && !(sql[index] == '*' && sql[index + 1] == '/')) {
        ++index;
      }
      if (index + 1 < sql.size()) index += 2;
      continue;
    }
    break;
  }
  constexpr std::string_view kInsert = "INSERT";
  if (index + kInsert.size() > sql.size()) return false;
  for (std::size_t offset = 0; offset < kInsert.size(); ++offset) {
    const auto ch = static_cast<unsigned char>(sql[index + offset]);
    if (std::toupper(ch) != kInsert[offset]) return false;
  }
  const std::size_t next = index + kInsert.size();
  return next >= sql.size() ||
         !std::isalnum(static_cast<unsigned char>(sql[next]));
}

bool LooksLikeFastCopyFromStdinCandidate(std::string_view sql) {
  std::size_t index = 0;
  for (;;) {
    while (index < sql.size() &&
           std::isspace(static_cast<unsigned char>(sql[index]))) {
      ++index;
    }
    if (index + 1 < sql.size() && sql[index] == '-' && sql[index + 1] == '-') {
      index += 2;
      while (index < sql.size() && sql[index] != '\n') ++index;
      continue;
    }
    if (index + 1 < sql.size() && sql[index] == '/' && sql[index + 1] == '*') {
      index += 2;
      while (index + 1 < sql.size() && !(sql[index] == '*' && sql[index + 1] == '/')) {
        ++index;
      }
      if (index + 1 < sql.size()) index += 2;
      continue;
    }
    break;
  }
  constexpr std::string_view kCopy = "COPY";
  if (index + kCopy.size() > sql.size()) return false;
  for (std::size_t offset = 0; offset < kCopy.size(); ++offset) {
    const auto ch = static_cast<unsigned char>(sql[index + offset]);
    if (std::toupper(ch) != kCopy[offset]) return false;
  }
  const std::size_t next = index + kCopy.size();
  return next >= sql.size() ||
         !std::isalnum(static_cast<unsigned char>(sql[next]));
}

std::optional<FastCopyFromStdinRoutePlan> TryParseFastCopyFromStdinRoutePlan(
    std::string_view sql) {
  return FastCopyFromStdinScanner(sql).Parse();
}

std::optional<FastInsertValuesRoutePlan> TryParseFastInsertValuesRoutePlan(
    std::string_view sql) {
  return FastInsertValuesScanner(sql).Parse();
}

bool ConsumeFastInsertKeyword(const CstDocument& cst,
                              std::size_t* index,
                              std::string_view keyword) {
  return ConsumeRouteKeyword(cst, index, keyword);
}

bool ConsumeFastInsertSymbol(const CstDocument& cst,
                             std::size_t* index,
                             std::string_view symbol) {
  return ConsumeRouteSymbol(cst, index, symbol);
}

std::string FastInsertLiteralPayload(const Token& token) {
  if (token.kind == TokenKind::kBooleanLiteral) {
    return ToUpperAscii(token.text) == "TRUE" ? "true" : "false";
  }
  return token.text;
}

std::string FastInsertScalarTypeForToken(const Token& token) {
  if (token.kind == TokenKind::kNumericLiteral) {
    if (token.literal_family == "uint") return "uint64";
    if (token.literal_family == "int128") return "int128";
    if (token.literal_family == "uint128") return "uint128";
    if (token.literal_family == "real128") return "real128";
    return token.literal_family == "decimal" || token.literal_family == "float" ? "numeric"
                                                                                 : "bigint";
  }
  if (token.kind == TokenKind::kBooleanLiteral) return "boolean";
  if (token.kind == TokenKind::kBinaryLiteral) {
    return token.literal_family == "bit_binary" ? "bit_string" : "binary";
  }
  if (token.kind == TokenKind::kUuidLiteral) return "uuid";
  if (token.kind == TokenKind::kTemporalLiteral) {
    const std::string family = ToUpperAscii(token.literal_family);
    if (family == "DATE") return "date";
    if (family == "TIME") return "time";
    if (family == "TIMESTAMP") return "timestamp";
    if (family == "INTERVAL") return "interval";
  }
  if (token.kind == TokenKind::kDocumentLiteral) {
    return ToUpperAscii(token.literal_family) == "JSON" ? "json_document" : "document";
  }
  if (token.kind == TokenKind::kVectorLiteral) return "dense_vector";
  if (token.kind == TokenKind::kNullLiteral) return "null";
  return "text";
}

bool IsFastInsertScalarLiteral(const Token& token) {
  return token.kind == TokenKind::kNumericLiteral ||
         token.kind == TokenKind::kStringLiteral ||
         token.kind == TokenKind::kBinaryLiteral ||
         token.kind == TokenKind::kBooleanLiteral ||
         token.kind == TokenKind::kNullLiteral ||
         token.kind == TokenKind::kUuidLiteral ||
         (token.kind == TokenKind::kDocumentLiteral &&
          (ToUpperAscii(token.literal_family) == "DOCUMENT" ||
           ToUpperAscii(token.literal_family) == "JSON")) ||
         (token.kind == TokenKind::kVectorLiteral &&
          ToUpperAscii(token.literal_family) == "VECTOR") ||
         (token.kind == TokenKind::kTemporalLiteral &&
          (ToUpperAscii(token.literal_family) == "DATE" ||
           ToUpperAscii(token.literal_family) == "TIME" ||
           ToUpperAscii(token.literal_family) == "TIMESTAMP" ||
           ToUpperAscii(token.literal_family) == "INTERVAL"));
}

std::string FastInsertTypedLiteralType(std::string upper) {
  if (upper == "DATE") return "date";
  if (upper == "TIME") return "time";
  if (upper == "TIMESTAMP") return "timestamp";
  if (upper == "TIMESTAMPTZ") return "timestamptz";
  if (upper == "INTERVAL") return "interval";
  if (upper == "UUID") return "uuid";
  if (upper == "JSON") return "json_document";
  if (upper == "XML") return "xml_document";
  if (upper == "VECTOR") return "dense_vector";
  return {};
}

bool ConsumeFastInsertLiteralValue(const CstDocument& cst,
                                   std::size_t* index,
                                   FastInsertValueField* field) {
  if (index == nullptr || field == nullptr) return false;
  SkipTriviaTokens(cst, index);
  if (*index >= cst.tokens.size()) return false;

  bool negative = false;
  if (cst.tokens[*index].text == "-" &&
      *index + 1 < cst.tokens.size() &&
      cst.tokens[*index + 1].kind == TokenKind::kNumericLiteral) {
    negative = true;
    ++(*index);
    SkipTriviaTokens(cst, index);
  }
  if (*index >= cst.tokens.size()) return false;
  const Token& token = cst.tokens[*index];
  if (negative && token.kind != TokenKind::kNumericLiteral) return false;

  if (!negative && IsIdentifierLikeForRouteExecution(token)) {
    const std::string typed_literal = FastInsertTypedLiteralType(ToUpperAscii(token.text));
    std::size_t literal_index = *index + 1;
    SkipTriviaTokens(cst, &literal_index);
    if (!typed_literal.empty() && literal_index < cst.tokens.size() &&
        cst.tokens[literal_index].kind == TokenKind::kStringLiteral) {
      field->type_name = typed_literal;
      field->value = cst.tokens[literal_index].text;
      field->is_null = false;
      *index = literal_index + 1;
      return true;
    }
  }

  if (!IsFastInsertScalarLiteral(token)) return false;
  field->type_name = FastInsertScalarTypeForToken(token);
  field->value = negative ? "-" + FastInsertLiteralPayload(token)
                          : FastInsertLiteralPayload(token);
  field->is_null = token.kind == TokenKind::kNullLiteral;
  ++(*index);
  return true;
}

std::string FastInsertCompactPayload(const FastInsertValuesRoutePlan& plan) {
  std::string payload;
  bool first = true;
  for (const auto& row : plan.rows) {
    for (const auto& field : row) {
      if (!first) payload.push_back(';');
      first = false;
      payload += HexEncodeRouteText(field.name);
      payload.push_back('|');
      payload += HexEncodeRouteText(field.type_name);
      payload.push_back('|');
      payload += HexEncodeRouteText(field.value);
      payload.push_back('|');
      payload.push_back(field.is_null ? '1' : '0');
    }
  }
  return payload;
}

std::optional<FastInsertValuesRoutePlan> TryParseFastInsertValuesRoutePlan(
    const CstDocument& cst) {
  std::size_t index = 0;
  if (!ConsumeFastInsertKeyword(cst, &index, "INSERT")) return std::nullopt;
  if (!ConsumeFastInsertKeyword(cst, &index, "INTO")) return std::nullopt;

  std::vector<std::string> target_parts;
  bool target_quoted = false;
  if (!ConsumeRouteQualifiedNameParts(cst, &index, &target_parts, &target_quoted)) {
    return std::nullopt;
  }

  std::vector<std::string> column_names;
  if (!ConsumeFastInsertSymbol(cst, &index, "(")) return std::nullopt;
  for (;;) {
    std::string column_name;
    if (!ConsumeRouteIdentifier(cst, &index, &column_name)) return std::nullopt;
    column_names.push_back(std::move(column_name));
    if (ConsumeFastInsertSymbol(cst, &index, ",")) continue;
    if (!ConsumeFastInsertSymbol(cst, &index, ")")) return std::nullopt;
    break;
  }
  if (column_names.empty()) return std::nullopt;
  if (!ConsumeFastInsertKeyword(cst, &index, "VALUES")) return std::nullopt;

  FastInsertValuesRoutePlan plan;
  plan.target.presented_name = JoinRouteNameParts(target_parts, 0, target_parts.size());
  plan.target.quoted = target_quoted;
  plan.target.object_class = "relation";
  plan.column_count = column_names.size();

  for (;;) {
    if (!ConsumeFastInsertSymbol(cst, &index, "(")) return std::nullopt;
    std::vector<FastInsertValueField> row;
    row.reserve(column_names.size());
    for (std::size_t column_index = 0; column_index < column_names.size(); ++column_index) {
      FastInsertValueField field;
      field.name = column_names[column_index];
      if (!ConsumeFastInsertLiteralValue(cst, &index, &field)) return std::nullopt;
      row.push_back(std::move(field));
      if (column_index + 1 < column_names.size()) {
        if (!ConsumeFastInsertSymbol(cst, &index, ",")) return std::nullopt;
      }
    }
    if (!ConsumeFastInsertSymbol(cst, &index, ")")) return std::nullopt;
    plan.rows.push_back(std::move(row));
    if (ConsumeFastInsertSymbol(cst, &index, ",")) continue;
    break;
  }

  if (plan.rows.empty()) return std::nullopt;
  SkipTriviaTokens(cst, &index);
  while (index < cst.tokens.size() &&
         cst.tokens[index].kind == TokenKind::kStatementTerminator) {
    ++index;
    SkipTriviaTokens(cst, &index);
  }
  if (index < cst.tokens.size() && cst.tokens[index].kind != TokenKind::kEnd) {
    return std::nullopt;
  }
  return plan;
}

std::string BuildFastInsertNativeBulkEnvelope(
    const FastInsertValuesRoutePlan& plan,
    std::string_view target_object_uuid) {
  std::string out;
  out += "operation_id=dml.execute_native_bulk_ingest\n";
  out += "opcode=SBLR_DML_EXECUTE_NATIVE_BULK_INGEST\n";
  out += "sblr_operation_family=sblr.dml.operation.v3\n";
  out += "result_shape=engine.api.result.v1\n";
  out += "diagnostic_shape=engine.diagnostic.v1\n";
  out += "trace_key=sbsql.parser.fast_insert_values.native_bulk\n";
  out += "contains_sql_text=false\n";
  out += "parser_resolved_names_to_uuids=true\n";
  out += "requires_security_context=true\n";
  out += "requires_transaction_context=true\n";
  out += "requires_cluster_authority=false\n";
  out += "target_object_uuid=";
  out += target_object_uuid;
  out += "\n";
  out += "target_object_kind=table\n";
  out += "dml_surface_variant=sbsql_insert_values_fast_native_bulk\n";
  out += "source_kind=sbsql_insert_values_compact_rowset\n";
  out += "format_family=sbsql.insert_values.cells.v1\n";
  out += "source_fingerprint=sbsql-fast-insert-values-explicit-columns\n";
  out += "source_position=row:0\n";
  out += "estimated_row_count=" + std::to_string(plan.rows.size()) + "\n";
  out += "native_bulk_ingest=true\n";
  out += "native_bulk_ingest_enabled=true\n";
  out += "reject_mode=fail_fast\n";
  out += "reject_limit_rows=0\n";
  out += "reject_payload_policy=diagnostic_only\n";
  out += "result_payload_policy=summary_only\n";
  out += "resume_policy=fail_closed\n";
  out += "checkpoint_mode=disabled\n";
  out += "duplicate_mode=error\n";
  out += "require_generated_row_uuid=true\n";
  AppendRouteTextOperand(&out, "insert_values_row_count", std::to_string(plan.rows.size()));
  AppendRouteTextOperand(&out, "insert_values_column_count", std::to_string(plan.column_count));
  AppendRouteTextOperand(&out, "insert_values_column_list_present", "true");
  AppendRouteTextOperand(&out, "insert_values_compact_format", "sbsql.insert_values.cells.v1");
  AppendRouteTextOperand(&out, "insert_values_compact_payload", FastInsertCompactPayload(plan));
  AppendRouteTextOperand(&out, "insert_values_parser_executes_sql", "false");
  AppendRouteTextOperand(&out, "sblr.canonical_rowset_shared_shape", "true");
  AppendRouteTextOperand(&out, "sblr.fast_insert_values_lowering", "true");
  return out;
}

std::string BuildFastCopyPlanExecutionEnvelope(
    const FastCopyFromStdinRoutePlan& plan,
    std::string_view target_object_uuid) {
  std::string out;
  out += "operation_id=dml.plan_import_rows\n";
  out += "opcode=SBLR_DML_PLAN_IMPORT_ROWS\n";
  out += "sblr_operation_family=sblr.dml.operation.v3\n";
  out += "result_shape=engine.api.result.v1\n";
  out += "diagnostic_shape=engine.diagnostic.v1\n";
  out += "trace_key=sbsql.parser.fast_copy.plan_import\n";
  out += "contains_sql_text=false\n";
  out += "parser_resolved_names_to_uuids=true\n";
  out += "requires_security_context=true\n";
  out += "requires_transaction_context=true\n";
  out += "requires_cluster_authority=false\n";
  out += "target_object_uuid=";
  out += target_object_uuid;
  out += "\n";
  out += "target_object_kind=table\n";
  out += "dml_surface_variant=copy_import_export\n";
  out += "source_kind=native_sbsql_import\n";
  out += "format_family=";
  out += plan.format_family;
  out += "\n";
  AppendRouteTextOperand(&out, "target_object_uuid", target_object_uuid);
  AppendRouteTextOperand(&out, "target_object_kind", "table");
  AppendRouteTextOperand(&out, "dml_surface_variant", "copy_import_export");
  AppendRouteTextOperand(&out, "source_kind", "native_sbsql_import");
  AppendRouteTextOperand(&out, "format_family", plan.format_family);
  AppendRouteTextOperand(&out, "copy_options_present",
                         plan.copy_options_present ? "true" : "false");
  AppendRouteTextOperand(&out, "copy_header_option",
                         plan.copy_header_option ? "true" : "false");
  AppendRouteTextOperand(&out, "source_handle_included", "false");
  AppendRouteTextOperand(&out, "parser_decodes_bytes", "false");
  AppendRouteTextOperand(&out, "row_persistence_claimed", "false");
  AppendRouteTextOperand(&out, "import_execution_deferred", "true");
  AppendRouteTextOperand(&out, "sblr.fast_copy_plan_lowering", "true");
  (void)plan.target;
  return out;
}

std::string BuildFastCopyPlanJsonPayload(
    const FastCopyFromStdinRoutePlan& plan,
    std::string_view target_object_uuid,
    std::uint64_t statement_hash) {
  std::string out = "{\"envelope\":\"SBLRExecutionEnvelope.v3\",";
  out += "\"operation_family\":\"sblr.dml.operation.v3\",";
  out += "\"surface_key\":\"copy_import_export\",";
  out += "\"command_family\":\"dml\",";
  out += "\"operation_id\":\"dml.plan_import_rows\",";
  out += "\"engine_api_operation_id\":\"dml.plan_import_rows\",";
  out += "\"sblr_operation\":\"SBLR_DML_PLAN_IMPORT_ROWS\",";
  out += "\"statement_surface_name\":\"copy_import_export\",";
  out += "\"sblr_operation_key\":\"sblr.dml.operation.v3\",";
  out += "\"result_shape\":\"engine.api.result.v1\",";
  out += "\"diagnostic_shape\":\"engine.diagnostic.v1\",";
  out += "\"resource_contract\":\"resource.contract.dml.import.v1\",";
  out += "\"trace_key\":\"sbsql.parser.fast_copy.plan_import\",";
  out += "\"statement_hash\":";
  out += std::to_string(statement_hash);
  out += ",\"catalog_epoch\":0,\"security_policy_epoch\":0,\"descriptor_epoch\":0,";
  out += "\"source_artifact_policy\":\"span_metadata_only\",";
  out += "\"source_payload_embedded\":false,";
  out += "\"real_file_effects\":false,";
  out += "\"parser_executes_sql\":false,";
  out += "\"import_execution_deferred\":true,";
  out += "\"target_object_kind\":\"table\",";
  out += "\"target_object_uuid\":\"";
  out += EscapeJson(target_object_uuid);
  out += "\",\"source_kind\":\"native_sbsql_import\",";
  out += "\"format_family\":\"";
  out += EscapeJson(plan.format_family);
  out += "\",\"copy_options_present\":";
  out += plan.copy_options_present ? "true" : "false";
  out += ",\"copy_header_option\":";
  out += plan.copy_header_option ? "true" : "false";
  out += ",\"source_handle_included\":false,";
  out += "\"parser_decodes_bytes\":false,";
  out += "\"row_persistence_claimed\":false,";
  out += "\"parser_authorizes\":false,";
  out += "\"name_text_included\":false,";
  out += "\"sql_text_included\":false,";
  out += "\"resolved_object_uuids\":[\"";
  out += EscapeJson(target_object_uuid);
  out += "\"],";
  out += "\"descriptor_refs\":[\"sys.storage.row_descriptor\",\"sys.import.plan_descriptor\"],";
  out += "\"policy_refs\":[\"import_planning_authorization_policy\"],";
  out += "\"required_rights\":[\"right.write\"],";
  out += "\"required_authority_steps\":[";
  out += "\"authority.parser.syntax_evidence_only\",";
  out += "\"authority.server.resolve_name_registry_public\",";
  out += "\"authority.server.security_policy_context_required\",";
  out += "\"authority.server.transaction_context_required\",";
  out += "\"authority.engine.import_planning_api_required\",";
  out += "\"authority.parser.no_storage_or_finality\",";
  out += "\"authority.parser.no_sql_text_execution\"]}";
  return out;
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
  std::vector<std::string> table_name_parts;
  if (!ConsumeRouteQualifiedNameParts(cst, &index, &table_name_parts)) return std::nullopt;
  const std::string table_name = table_name_parts.back();
  const std::string schema_parent_path =
      table_name_parts.size() > 1
          ? JoinRouteNameParts(table_name_parts, 0, table_name_parts.size() - 1)
          : std::string{};
  if (!ConsumeRouteSymbol(cst, &index, "(")) return std::nullopt;
  std::vector<RouteColumnDefinition> columns;
  for (;;) {
    SkipTriviaTokens(cst, &index);
    if (index >= cst.tokens.size()) return std::nullopt;
    if (ConsumeRouteSymbol(cst, &index, ")")) break;
    if (IsTableConstraintStarter(cst.tokens[index])) {
      if (!ConsumeTableConstraint(cst, &index, &columns)) return std::nullopt;
    } else {
      RouteColumnDefinition column;
      if (!ConsumeRouteIdentifier(cst, &index, &column.name)) return std::nullopt;
      column.canonical_type = ConsumeRouteTypeText(cst, &index, &column.raw_type);
      if (column.canonical_type.empty()) return std::nullopt;
      if (!ConsumeColumnConstraints(cst, &index, &column)) return std::nullopt;
      columns.push_back(std::move(column));
    }
    SkipTriviaTokens(cst, &index);
    if (ConsumeRouteSymbol(cst, &index, ",")) continue;
    if (ConsumeRouteSymbol(cst, &index, ")")) break;
    return std::nullopt;
  }
  if (columns.empty()) return std::nullopt;
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
  if (!schema_parent_path.empty()) {
    AppendRouteTextOperand(&out, "schema_parent_path", schema_parent_path);
  }
  AppendRouteTextOperand(&out, "column_count", std::to_string(columns.size()));
  for (std::size_t column_index = 0; column_index < columns.size(); ++column_index) {
    const auto& column = columns[column_index];
    const std::string prefix = "column_" + std::to_string(column_index) + "_";
    std::string descriptor = "type=" + column.canonical_type;
    if (!column.raw_type.empty() && column.raw_type != column.canonical_type) {
      AppendDescriptorFlag(&descriptor, "source_type", column.raw_type);
    }
    AppendDescriptorFlag(&descriptor, "nullable", column.nullable ? "true" : "false");
    if (column.primary_key) {
      AppendDescriptorFlag(&descriptor, "primary_key", "true");
      AppendDescriptorFlag(&descriptor, "unique", "true");
    } else if (column.unique) {
      AppendDescriptorFlag(&descriptor, "unique", "true");
    }
    if (!column.default_expression.empty()) {
      AppendDescriptorFlag(&descriptor, "default", column.default_expression);
    }
    AppendRouteTextOperand(&out, prefix + "name", column.name);
    AppendRouteTextOperand(&out, prefix + "type", column.canonical_type);
    AppendRouteTextOperand(&out, prefix + "descriptor", descriptor);
    AppendRouteTextOperand(&out, prefix + "nullable", column.nullable ? "true" : "false");
    if (!column.default_expression.empty()) {
      AppendRouteTextOperand(&out, prefix + "default", column.default_expression);
    }
  }
  if (temporary) {
    AppendRouteTextOperand(&out, "temporary", "true");
    AppendRouteTextOperand(&out, "temporary_scope", temporary_scope);
    AppendRouteTextOperand(&out, "on_commit", on_commit_action);
  }
  return out;
}

struct CreatedDdlName {
  std::string presented_name;
  std::vector<std::string> object_classes;
  bool quoted{false};
};

void PushCreatedDdlClassAliases(std::string_view object_kind,
                                std::vector<std::string>* classes) {
  if (classes == nullptr) return;
  const auto kind = ToUpperAscii(object_kind);
  if (kind == "TABLE") {
    classes->push_back("table");
    classes->push_back("relation");
  } else if (kind == "VIEW") {
    classes->push_back("view");
    classes->push_back("relation");
  } else if (kind == "SCHEMA") {
    classes->push_back("schema");
  } else if (kind == "DOMAIN") {
    classes->push_back("domain");
  } else if (kind == "INDEX") {
    classes->push_back("index");
  } else if (kind == "ROLE") {
    classes->push_back("role");
    classes->push_back("security_role");
    classes->push_back("principal");
  } else if (kind == "GROUP") {
    classes->push_back("group");
    classes->push_back("security_group");
    classes->push_back("principal");
  } else if (kind == "USER" || kind == "PRINCIPAL") {
    classes->push_back("principal");
    classes->push_back("user");
  } else if (kind == "POLICY") {
    classes->push_back("policy");
    classes->push_back("security_policy");
  } else if (kind == "MASK") {
    classes->push_back("mask");
    classes->push_back("security_policy");
  } else if (kind == "RLS") {
    classes->push_back("rls");
    classes->push_back("security_policy");
  } else if (kind == "PROCEDURE") {
    classes->push_back("procedure");
    classes->push_back("routine");
  } else if (kind == "FUNCTION") {
    classes->push_back("function");
    classes->push_back("routine");
  } else if (kind == "TRIGGER") {
    classes->push_back("trigger");
  } else if (kind == "FILESPACE") {
    classes->push_back("filespace");
  } else if (!object_kind.empty()) {
    classes->push_back(std::string(object_kind));
  }
  std::sort(classes->begin(), classes->end());
  classes->erase(std::unique(classes->begin(), classes->end()), classes->end());
}

std::optional<CreatedDdlName> ExtractCreatedDdlNameFromCst(
    const CstDocument& cst,
    std::string_view object_kind) {
  std::size_t index = 0;
  if (!ConsumeRouteKeyword(cst, &index, "CREATE")) return std::nullopt;
  bool temporary = false;
  std::string temporary_scope;
  (void)ConsumeOptionalTemporaryTablePrefix(cst, &index, &temporary, &temporary_scope);
  (void)temporary;
  (void)temporary_scope;
  const auto kind = ToUpperAscii(object_kind);
  if (kind == "TABLE" || kind == "RELATION") {
    if (!ConsumeRouteKeyword(cst, &index, "TABLE")) return std::nullopt;
  } else if (kind == "SCHEMA") {
    if (!ConsumeRouteKeyword(cst, &index, "SCHEMA")) return std::nullopt;
  } else if (kind == "VIEW") {
    if (!ConsumeRouteKeyword(cst, &index, "VIEW")) return std::nullopt;
  } else if (kind == "DOMAIN") {
    if (!ConsumeRouteKeyword(cst, &index, "DOMAIN")) return std::nullopt;
  } else if (kind == "INDEX") {
    if (!ConsumeRouteKeyword(cst, &index, "INDEX")) return std::nullopt;
  } else if (kind == "ROLE") {
    if (!ConsumeRouteKeyword(cst, &index, "ROLE")) return std::nullopt;
  } else if (kind == "GROUP") {
    if (!ConsumeRouteKeyword(cst, &index, "GROUP")) return std::nullopt;
  } else if (kind == "USER" || kind == "PRINCIPAL") {
    if (ConsumeRouteKeyword(cst, &index, "USER")) {
    } else if (!ConsumeRouteKeyword(cst, &index, "PRINCIPAL")) {
      return std::nullopt;
    }
  } else if (kind == "POLICY") {
    if (!ConsumeRouteKeyword(cst, &index, "POLICY")) return std::nullopt;
  } else if (kind == "MASK") {
    if (!ConsumeRouteKeyword(cst, &index, "MASK")) return std::nullopt;
  } else if (kind == "RLS") {
    if (!ConsumeRouteKeyword(cst, &index, "RLS")) return std::nullopt;
  } else if (kind == "PROCEDURE") {
    if (!ConsumeRouteKeyword(cst, &index, "PROCEDURE")) return std::nullopt;
  } else if (kind == "FUNCTION") {
    if (!ConsumeRouteKeyword(cst, &index, "FUNCTION")) return std::nullopt;
  } else if (kind == "TRIGGER") {
    if (!ConsumeRouteKeyword(cst, &index, "TRIGGER")) return std::nullopt;
  } else if (kind == "FILESPACE") {
    if (!ConsumeRouteKeyword(cst, &index, "FILESPACE")) return std::nullopt;
  } else {
    return std::nullopt;
  }
  (void)ConsumeOptionalIfNotExists(cst, &index);
  std::vector<std::string> name_parts;
  CreatedDdlName created;
  if (!ConsumeRouteQualifiedNameParts(cst, &index, &name_parts, &created.quoted)) {
    return std::nullopt;
  }
  created.presented_name = JoinRouteNameParts(name_parts, 0, name_parts.size());
  PushCreatedDdlClassAliases(object_kind, &created.object_classes);
  if (created.presented_name.empty() || created.object_classes.empty()) return std::nullopt;
  return created;
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

void InjectAutocommitEmulation(std::string* payload) {
  if (payload == nullptr || payload->empty() ||
      payload->find("autocommit_emulation=") != std::string::npos ||
      payload->find("\"autocommit_emulation\"") != std::string::npos) {
    return;
  }
  if (!payload->empty() && payload->back() != '\n') payload->push_back('\n');
  payload->append("autocommit_emulation=true\n");
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
  return ExecuteSblrOnRouteWithDataPacket(encoded_sblr_envelope, {}, cursor_requested);
}

ServerExecutionResult SbsqlTestWireSession::ExecuteSblrOnRouteWithDataPacket(
    std::string_view encoded_sblr_envelope,
    const std::vector<std::uint8_t>& data_packet,
    bool cursor_requested) {
  if (config_.embedded_engine_direct && embedded_client_ != nullptr) {
    return embedded_client_->ExecuteSblrWithDataPacket(
        session_, encoded_sblr_envelope, data_packet, cursor_requested);
  }
  SbpsClient client(config_.server_endpoint);
  return client.ExecuteSblrWithDataPacket(
      session_, encoded_sblr_envelope, data_packet, cursor_requested);
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

void SbsqlTestWireSession::ClearNameResolutionCache(
    bool preserve_stable_relation_names) {
  const bool local_had_entries =
      !name_resolution_cache_.empty() || !name_resolution_lru_.empty();
  name_resolution_cache_.clear();
  name_resolution_lru_.clear();
  ClearSharedNameResolutionCache();
  if (!preserve_stable_relation_names) {
    stable_relation_name_resolution_cache_.clear();
    stable_relation_name_resolution_lru_.clear();
  }
  if (metrics_ && local_had_entries) {
    metrics_->Increment("sys.metrics.parsers.name_resolution_cache.clears_total");
  }
}

void SbsqlTestWireSession::RehydrateStableRelationNameResolutionCache() {
  std::vector<StableCachedPublicNameResolution> stable_entries;
  stable_entries.reserve(stable_relation_name_resolution_cache_.size());
  for (const auto& [_, stable] : stable_relation_name_resolution_cache_) {
    if (stable.presented_name.empty() || stable.lookup_object_class.empty() ||
        stable.resolved.object_uuid.empty()) {
      continue;
    }
    stable_entries.push_back(stable);
  }
  for (const auto& stable : stable_entries) {
    StoreNameResolutionCacheEntry(stable.presented_name,
                                  stable.quoted,
                                  stable.lookup_object_class,
                                  stable.resolved.object_uuid,
                                  stable.resolved.canonical_name,
                                  session_.catalog_epoch,
                                  session_.security_policy_epoch,
                                  stable.resolved.object_class);
  }
  if (metrics_ && !stable_entries.empty()) {
    metrics_->Increment("sys.metrics.parsers.name_resolution_cache.stable_rehydrates_total");
  }
}

void SbsqlTestWireSession::StoreNameResolutionCacheEntry(
    std::string_view presented_name,
    bool quoted,
    std::string_view object_class,
    std::string_view object_uuid,
    std::string_view canonical_name,
    std::uint64_t catalog_epoch,
    std::uint64_t security_epoch,
    std::string_view resolved_object_class) {
  if (presented_name.empty() || object_class.empty() || object_uuid.empty()) return;
  const std::string cache_key =
      BuildNameResolutionCacheKey(session_, presented_name, quoted, object_class);
  CachedPublicNameResolution cached;
  cached.object_uuid = std::string(object_uuid);
  cached.canonical_name = canonical_name.empty() ? std::string(presented_name)
                                                 : std::string(canonical_name);
  cached.object_class = resolved_object_class.empty()
                            ? std::string(object_class)
                            : std::string(resolved_object_class);
  cached.catalog_epoch = catalog_epoch;
  cached.security_epoch = security_epoch;
  name_resolution_cache_[cache_key] = std::move(cached);
  StoreSharedNameResolutionCacheEntry(cache_key, name_resolution_cache_[cache_key]);
  name_resolution_lru_.erase(std::remove(name_resolution_lru_.begin(),
                                         name_resolution_lru_.end(),
                                         cache_key),
                             name_resolution_lru_.end());
  name_resolution_lru_.push_back(cache_key);
  while (name_resolution_cache_.size() > kMaxNameResolutionCacheEntries &&
         !name_resolution_lru_.empty()) {
    name_resolution_cache_.erase(name_resolution_lru_.front());
    name_resolution_lru_.pop_front();
  }
  if (metrics_) metrics_->Increment("sys.metrics.parsers.name_resolution_cache.stores_total");

  if (IsReferencedRelationNameClass(object_class) &&
      IsReferencedRelationNameClass(name_resolution_cache_[cache_key].object_class)) {
    const std::string stable_key =
        BuildStableRelationNameResolutionCacheKey(session_, presented_name, quoted, object_class);
    StableCachedPublicNameResolution stable;
    stable.presented_name = std::string(presented_name);
    stable.quoted = quoted;
    stable.lookup_object_class = std::string(object_class);
    stable.resolved = name_resolution_cache_[cache_key];
    stable_relation_name_resolution_cache_[stable_key] = std::move(stable);
    stable_relation_name_resolution_lru_.erase(
        std::remove(stable_relation_name_resolution_lru_.begin(),
                    stable_relation_name_resolution_lru_.end(),
                    stable_key),
        stable_relation_name_resolution_lru_.end());
    stable_relation_name_resolution_lru_.push_back(stable_key);
    while (stable_relation_name_resolution_cache_.size() >
               kMaxStableRelationNameResolutionCacheEntries &&
           !stable_relation_name_resolution_lru_.empty()) {
      stable_relation_name_resolution_cache_.erase(
          stable_relation_name_resolution_lru_.front());
      stable_relation_name_resolution_lru_.pop_front();
    }
  }
}

void SbsqlTestWireSession::SeedCreatedDdlNameResolutionCache(
    const CstDocument& cst,
    const PipelineResult& result) {
  if (!result.accepted || result.server_result_payload.empty()) return;
  const auto object_uuid = DdlResultRowField(result.server_result_payload, "object_uuid");
  const auto object_kind = DdlResultRowField(result.server_result_payload, "object_kind");
  if (!object_uuid || object_uuid->empty() || !object_kind || object_kind->empty()) return;
  const auto created = ExtractCreatedDdlNameFromCst(cst, *object_kind);
  if (!created) return;
  const auto payload_name = DdlResultRowField(result.server_result_payload, "name");
  const std::string canonical_name =
      payload_name && !payload_name->empty() ? *payload_name : created->presented_name;
  for (const auto& object_class : created->object_classes) {
    StoreNameResolutionCacheEntry(created->presented_name,
                                  created->quoted,
                                  object_class,
                                  *object_uuid,
                                  canonical_name,
                                  session_.catalog_epoch,
                                  session_.security_policy_epoch);
  }
}

PublicNameResolutionResult SbsqlTestWireSession::ResolveNameOnRoute(
    std::string_view presented_name,
    bool quoted,
    std::string_view object_class) {
  const std::string cache_key =
      BuildNameResolutionCacheKey(session_, presented_name, quoted, object_class);
  if (const auto found = name_resolution_cache_.find(cache_key);
      found != name_resolution_cache_.end()) {
    if (metrics_) {
      metrics_->Increment("sys.metrics.parsers.name_resolution_cache.hits_total");
      metrics_->Increment("sys.metrics.parsers.name_resolution_cache.route_skips_total");
    }
    PublicNameResolutionResult result;
    result.resolved = true;
    result.object_uuid = found->second.object_uuid;
    result.canonical_name = found->second.canonical_name;
    result.object_class = found->second.object_class;
    result.catalog_epoch = found->second.catalog_epoch;
    result.security_epoch = found->second.security_epoch;
    return result;
  }
  if (auto shared = LookupSharedNameResolutionCache(cache_key)) {
    if (metrics_) {
      metrics_->Increment("sys.metrics.parsers.name_resolution_cache.shared_hits_total");
      metrics_->Increment("sys.metrics.parsers.name_resolution_cache.route_skips_total");
    }
    name_resolution_cache_[cache_key] = *shared;
    name_resolution_lru_.erase(std::remove(name_resolution_lru_.begin(),
                                           name_resolution_lru_.end(),
                                           cache_key),
                               name_resolution_lru_.end());
    name_resolution_lru_.push_back(cache_key);
    PublicNameResolutionResult result;
    result.resolved = true;
    result.object_uuid = shared->object_uuid;
    result.canonical_name = shared->canonical_name;
    result.object_class = shared->object_class;
    result.catalog_epoch = shared->catalog_epoch;
    result.security_epoch = shared->security_epoch;
    return result;
  }
  if (metrics_) metrics_->Increment("sys.metrics.parsers.name_resolution_cache.misses_total");
  PublicNameResolutionResult resolved;
  if (config_.embedded_engine_direct && embedded_client_ != nullptr) {
    resolved =
        embedded_client_->ResolveNamePublic(session_, presented_name, quoted, object_class, config_);
  } else {
    SbpsClient client(config_.server_endpoint);
    resolved = client.ResolveNamePublic(session_, presented_name, quoted, object_class, config_);
  }
  if (resolved.resolved) {
    const std::string resolved_class =
        resolved.object_class.empty() ? std::string(object_class)
                                      : resolved.object_class;
    session_.catalog_epoch = std::max(session_.catalog_epoch, resolved.catalog_epoch);
    session_.security_policy_epoch =
        std::max(session_.security_policy_epoch, resolved.security_epoch);
    StoreNameResolutionCacheEntry(presented_name,
                                  quoted,
                                  object_class,
                                  resolved.object_uuid,
                                  resolved.canonical_name,
                                  resolved.catalog_epoch,
                                  resolved.security_epoch,
                                  resolved_class);
    if (resolved_class != std::string(object_class)) {
      StoreNameResolutionCacheEntry(presented_name,
                                    quoted,
                                    resolved_class,
                                    resolved.object_uuid,
                                    resolved.canonical_name,
                                    resolved.catalog_epoch,
                                    resolved.security_epoch,
                                    resolved_class);
    }
  }
  return resolved;
}

PublicNameResolutionResult SbsqlTestWireSession::ResolvePublicNameForWire(
    std::string_view presented_name,
    bool quoted,
    std::string_view object_class) {
  return ResolveNameOnRoute(presented_name, quoted, object_class);
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
                                                 std::uint64_t stream_row_count,
                                                 bool autocommit_emulation) {
  const bool phase_trace =
      std::getenv("SCRATCHBIRD_SBSQL_PIPELINE_PHASE_TRACE_FILE") != nullptr;
  std::vector<std::pair<std::string, std::uint64_t>> phase_micros;
  auto phase_start = ParserPipelineClock::now();
  auto mark_phase = [&](std::string phase) {
    if (!phase_trace) return;
    const auto now = ParserPipelineClock::now();
    phase_micros.push_back(
        {std::move(phase), ParserPipelineElapsedMicros(phase_start, now)});
    phase_start = now;
  };

  if (metrics_) metrics_->Increment("sys.metrics.parsers.parse_pipeline.attempts_total");
  ScopedParserState active(metrics_,
                           submit && session_.authenticated && HasExecutionRoute(),
                           ParserState::kActive,
                           ParserState::kAuthenticated);
  if (auto management = ParseServerManagementCommand(sql)) {
    auto result = RunServerManagementCommand(*management);
    mark_phase("server_management");
    WriteParserPipelinePhaseTrace(sql, result, phase_micros);
    return result;
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
    mark_phase("resource_budget");
    WriteParserPipelinePhaseTrace(sql, result, phase_micros);
    return result;
  }
  if (submit && !cursor_requested && session_.authenticated && HasExecutionRoute() &&
      LooksLikeFastCopyFromStdinCandidate(sql)) {
    const auto fast_copy = TryParseFastCopyFromStdinRoutePlan(sql);
    const std::uint64_t fast_statement_hash = Fnv1a64(sql);
    mark_phase(fast_copy ? "fast_copy_parse_raw"
                         : "fast_copy_parse_raw_fallback");
    if (fast_copy) {
      if (metrics_) {
        metrics_->Increment("sys.metrics.parsers.fast_copy_from_stdin.attempts_total");
      }
      PipelineResult result;
      result.statement_family = "dml.import";
      result.operation_family = "sblr.dml.operation.v3";
      result.statement_hash = fast_statement_hash;
      result.parser_executes_sql = false;
      result.cached_storage_authority = false;
      result.cached_authorization_authority = false;
      result.cached_finality_authority = false;

      auto resolved = ResolveNameOnRoute(fast_copy->target.presented_name,
                                         fast_copy->target.quoted,
                                         fast_copy->target.object_class);
      mark_phase("fast_copy_resolve_target");
      if (!resolved.resolved) {
        result.messages = std::move(resolved.messages);
        result.accepted = false;
        WriteParserPipelinePhaseTrace(sql, result, phase_micros);
        return result;
      }
      session_.catalog_epoch =
          std::max(session_.catalog_epoch, resolved.catalog_epoch);
      session_.security_policy_epoch =
          std::max(session_.security_policy_epoch, resolved.security_epoch);

      const std::string execution_payload =
          BuildFastCopyPlanExecutionEnvelope(*fast_copy, resolved.object_uuid);
      result.sblr_payload = BuildFastCopyPlanJsonPayload(*fast_copy,
                                                         resolved.object_uuid,
                                                         fast_statement_hash);
      mark_phase("fast_copy_build_sblr");
      const auto executed = ExecuteSblrOnRoute(execution_payload, false);
      mark_phase("fast_copy_execute_sblr_route");
      if (!executed.accepted) {
        result.accepted = false;
        result.messages = executed.messages;
      } else {
        result.accepted = true;
        result.server_operation_id = executed.operation_id;
        result.server_cursor_uuid = executed.cursor_uuid;
        result.server_row_count = executed.row_count;
        result.server_affected_rows = executed.affected_rows;
        result.server_affected_rows_present = executed.affected_rows_present;
        result.server_result_payload = executed.row_packet;
        ApplyExecutedTransactionState(executed, &session_);
        if (metrics_) {
          metrics_->Increment("sys.metrics.parsers.fast_copy_from_stdin.accepted_total");
        }
      }
      WriteParserPipelinePhaseTrace(sql, result, phase_micros);
      return result;
    }
    if (metrics_) {
      metrics_->Increment("sys.metrics.parsers.fast_copy_from_stdin.fallbacks_total");
    }
  }
  if (submit && !cursor_requested && session_.authenticated && HasExecutionRoute() &&
      LooksLikeFastInsertValuesCandidate(sql)) {
    auto fast_insert = TryParseFastInsertValuesRoutePlan(sql);
    std::uint64_t fast_statement_hash = Fnv1a64(sql);
    mark_phase(fast_insert ? "fast_insert_parse_raw"
                           : "fast_insert_parse_raw_fallback");
    if (!fast_insert) {
      auto fast_cst = BuildCst(sql);
      mark_phase("fast_insert_build_cst");
      if (!fast_cst.messages.has_errors()) {
        fast_insert = TryParseFastInsertValuesRoutePlan(fast_cst);
        fast_statement_hash = Fnv1a64(fast_cst.source);
      }
    }
    if (fast_insert) {
        if (metrics_) {
          metrics_->Increment("sys.metrics.parsers.fast_insert_values.attempts_total");
        }
        PipelineResult result;
        result.statement_family = "dml.insert";
        result.operation_family = "sblr.dml.operation.v3";
        result.statement_hash = fast_statement_hash;
        result.parser_executes_sql = false;
        result.cached_storage_authority = false;
        result.cached_authorization_authority = false;
        result.cached_finality_authority = false;

        auto resolved = ResolveNameOnRoute(fast_insert->target.presented_name,
                                           fast_insert->target.quoted,
                                           fast_insert->target.object_class);
        mark_phase("fast_insert_resolve_target");
        if (!resolved.resolved) {
          result.messages = std::move(resolved.messages);
          result.accepted = false;
          WriteParserPipelinePhaseTrace(sql, result, phase_micros);
          return result;
        }
        session_.catalog_epoch = std::max(session_.catalog_epoch, resolved.catalog_epoch);
        session_.security_policy_epoch =
            std::max(session_.security_policy_epoch, resolved.security_epoch);
        result.sblr_payload =
            BuildFastInsertNativeBulkEnvelope(*fast_insert, resolved.object_uuid);
        if (autocommit_emulation) {
          InjectAutocommitEmulation(&result.sblr_payload);
        }
        mark_phase("fast_insert_build_sblr");
        const auto executed = ExecuteSblrOnRoute(result.sblr_payload, false);
        mark_phase("fast_insert_execute_sblr_route");
        if (!executed.accepted) {
          result.accepted = false;
          result.messages = executed.messages;
        } else {
          result.accepted = true;
          result.server_operation_id = executed.operation_id;
          result.server_cursor_uuid = executed.cursor_uuid;
          result.server_row_count = executed.row_count;
          result.server_affected_rows = executed.affected_rows;
          result.server_affected_rows_present = executed.affected_rows_present;
          result.server_result_payload = executed.row_packet;
          ApplyExecutedTransactionState(executed, &session_);
          if (metrics_) {
            metrics_->Increment("sys.metrics.parsers.fast_insert_values.accepted_total");
          }
        }
        WriteParserPipelinePhaseTrace(sql, result, phase_micros);
        return result;
    }
    if (metrics_) {
      metrics_->Increment("sys.metrics.parsers.fast_insert_values.fallbacks_total");
    }
  }
  const auto frontdoor_cache_key = BuildFrontdoorLoweringCacheKey(config_, session_, sql);
  mark_phase("frontdoor_cache_key");
  if (cache_ != nullptr) {
    if (metrics_) metrics_->Increment("sys.metrics.parsers.frontdoor_cache.attempts_total");
    if (auto cached = cache_->LookupEntry(frontdoor_cache_key)) {
      auto result = PipelineResultFromCacheEntry(*cached);
      if (!submit || CanReuseFrontdoorCacheForSubmit(result)) {
        if (metrics_) {
          metrics_->Increment("sys.metrics.parsers.frontdoor_cache.hits_total");
          metrics_->Increment("sys.metrics.parsers.frontdoor_cache.parse_lower_skips_total");
        }
        mark_phase("frontdoor_cache_hit");
        if (!submit) {
          WriteParserPipelinePhaseTrace(sql, result, phase_micros);
          return result;
        }
        if (!HasExecutionRoute()) {
          result.accepted = false;
          result.messages.diagnostics.push_back(MakeDiagnostic(
              "SBSQL.SERVER.UNAVAILABLE", "ERROR",
              "SBLR submission requires an execution route",
              "sbp_sbsql.wire"));
        } else if (!session_.authenticated) {
          result.accepted = false;
          result.messages.diagnostics.push_back(MakeDiagnostic(
              "SBSQL.AUTH.REQUIRED", "ERROR",
              "SBLR submission requires an authenticated server session",
              "sbp_sbsql.wire"));
        } else {
          std::string execution_payload = result.sblr_payload;
          if (autocommit_emulation && !cursor_requested) {
            InjectAutocommitEmulation(&execution_payload);
          }
          mark_phase("prepare_execution_payload");
          const auto executed = ExecuteSblrOnRoute(execution_payload, cursor_requested);
          mark_phase("execute_sblr_route");
          if (!executed.accepted) {
            result.accepted = false;
            result.messages = executed.messages;
          } else {
            result.server_operation_id = executed.operation_id;
            result.server_cursor_uuid = executed.cursor_uuid;
            result.server_row_count = executed.row_count;
            result.server_affected_rows = executed.affected_rows;
            result.server_affected_rows_present = executed.affected_rows_present;
            result.server_result_payload = executed.row_packet;
            ApplyExecutedTransactionState(executed, &session_);
            if (ExecutionInvalidatesNameResolution(executed.operation_id)) {
              const bool preserve_stable_relations =
                  ExecutionPreservesReferencedRelationNames(executed.operation_id);
              ClearNameResolutionCache(preserve_stable_relations);
              if (preserve_stable_relations) {
                RehydrateStableRelationNameResolutionCache();
              }
            }
          }
        }
        WriteParserPipelinePhaseTrace(sql, result, phase_micros);
        return result;
      }
      if (metrics_) metrics_->Increment("sys.metrics.parsers.frontdoor_cache.misses_total");
    } else if (metrics_) {
      metrics_->Increment("sys.metrics.parsers.frontdoor_cache.misses_total");
    }
  }
  mark_phase("frontdoor_cache_lookup");
  auto cst = BuildCst(sql);
  mark_phase("build_cst");
  MessageVectorSet resource_messages = cst.messages;
  if (!EnforceCstResourceBudget(cst, config_.resource_budget, metrics_,
                                &resource_messages)) {
    PipelineResult result;
    result.accepted = false;
    result.statement_hash = Fnv1a64(cst.source);
    result.messages = std::move(resource_messages);
    mark_phase("cst_resource_budget");
    WriteParserPipelinePhaseTrace(sql, result, phase_micros);
    return result;
  }
  auto ast = BuildAst(cst);
  mark_phase("build_ast");
  std::vector<std::string> resolved_object_uuids;
  std::vector<ResolvedObjectReferenceSeed> resolved_object_reference_seeds;
  PipelineResult result;
  result.statement_family = StatementFamilyName(ast.family);
  result.operation_family = ast.operation_family;
  result.statement_hash = Fnv1a64(cst.source);
  result.messages = ast.messages;
  if (!result.messages.has_errors() && ast.requires_name_resolution &&
      HasExecutionRoute() && session_.authenticated) {
    const auto refs = ExtractObjectReferences(cst);
    mark_phase("extract_object_references");
    for (const auto& ref : refs) {
      auto resolved = ResolveNameOnRoute(ref.presented_name, ref.quoted, ref.object_class);
      if (!resolved.resolved) {
        result.messages = std::move(resolved.messages);
        break;
      }
      resolved_object_uuids.push_back(resolved.object_uuid);
      resolved_object_reference_seeds.push_back({ref, resolved});
      session_.catalog_epoch = std::max(session_.catalog_epoch, resolved.catalog_epoch);
      session_.security_policy_epoch =
          std::max(session_.security_policy_epoch, resolved.security_epoch);
    }
    mark_phase("resolve_object_references");
  }
  if (result.messages.has_errors()) {
    result.accepted = false;
    WriteParserPipelinePhaseTrace(sql, result, phase_micros);
    return result;
  }
  auto bound = BindAst(ast, cst, config_, session_, resolved_object_uuids);
  mark_phase("bind_ast");
  auto lowered = LowerToSblr(bound, cst, session_);
  mark_phase("lower_to_sblr");
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
  mark_phase("shape_pipeline_result");
  result.messages = std::move(lowered.messages);
  if (result.accepted && cache_ != nullptr &&
      (!submit || CanReuseFrontdoorCacheForSubmit(result))) {
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
  mark_phase("frontdoor_cache_store");
  auto reseed_preserved_reference_names = [&](std::string_view operation_id) {
    if (!ExecutionPreservesReferencedRelationNames(operation_id)) return;
    for (const auto& seed : resolved_object_reference_seeds) {
      if (!seed.resolved.resolved || seed.resolved.object_uuid.empty()) continue;
      const std::string object_class =
          seed.resolved.object_class.empty() ? seed.ref.object_class : seed.resolved.object_class;
      if (!IsReferencedRelationNameClass(object_class)) continue;
      StoreNameResolutionCacheEntry(seed.ref.presented_name,
                                    seed.ref.quoted,
                                    object_class,
                                    seed.resolved.object_uuid,
                                    seed.resolved.canonical_name,
                                    session_.catalog_epoch,
                                    session_.security_policy_epoch);
    }
  };
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
      if (autocommit_emulation && !cursor_requested) {
        InjectAutocommitEmulation(&execution_payload);
      }
      mark_phase("prepare_execution_payload");
      const auto executed = ExecuteSblrOnRoute(execution_payload, cursor_requested);
      mark_phase("execute_sblr_route");
      if (!executed.accepted) {
        result.accepted = false;
        result.messages = executed.messages;
      } else {
        result.server_operation_id = executed.operation_id;
        result.server_cursor_uuid = executed.cursor_uuid;
        result.server_row_count = executed.row_count;
        result.server_affected_rows = executed.affected_rows;
        result.server_affected_rows_present = executed.affected_rows_present;
        result.server_result_payload = executed.row_packet;
        ApplyExecutedTransactionState(executed, &session_);
        if (ExecutionInvalidatesNameResolution(executed.operation_id)) {
          const bool preserve_stable_relations =
              ExecutionPreservesReferencedRelationNames(executed.operation_id);
          ClearNameResolutionCache(preserve_stable_relations);
          if (preserve_stable_relations) {
            RehydrateStableRelationNameResolutionCache();
          }
        }
        reseed_preserved_reference_names(executed.operation_id);
        SeedCreatedDdlNameResolutionCache(cst, result);
      }
    }
  }
  WriteParserPipelinePhaseTrace(sql, result, phase_micros);
  return result;
}

PipelineResult SbsqlTestWireSession::RunSblrEnvelope(std::string_view encoded_sblr_envelope,
                                                     bool cursor_requested) {
  return RunSblrEnvelopeWithDataPacket(encoded_sblr_envelope, {}, cursor_requested);
}

PipelineResult SbsqlTestWireSession::RunSblrEnvelopeWithDataPacket(
    std::string_view encoded_sblr_envelope,
    const std::vector<std::uint8_t>& data_packet,
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
  const auto executed =
      ExecuteSblrOnRouteWithDataPacket(encoded_sblr_envelope, data_packet, cursor_requested);
  if (!executed.accepted) {
    result.messages = executed.messages;
    return result;
  }
  result.accepted = true;
  result.server_operation_id = executed.operation_id;
  result.server_cursor_uuid = executed.cursor_uuid;
  result.server_row_count = executed.row_count;
  result.server_affected_rows = executed.affected_rows;
  result.server_affected_rows_present = executed.affected_rows_present;
  result.server_result_payload = executed.row_packet;
  ApplyExecutedTransactionState(executed, &session_);
  if (ExecutionInvalidatesNameResolution(executed.operation_id)) {
    const bool preserve_stable_relations =
        ExecutionPreservesReferencedRelationNames(executed.operation_id);
    ClearNameResolutionCache(preserve_stable_relations);
    if (preserve_stable_relations) {
      RehydrateStableRelationNameResolutionCache();
    }
  }
  return result;
}

ServerPrepareSblrResult SbsqlTestWireSession::PrepareSblrForWire(
    std::string_view encoded_sblr_envelope) {
  ServerPrepareSblrResult result;
  if (!HasExecutionRoute()) {
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "SBSQL.SERVER.UNAVAILABLE", "ERROR", "SBLR prepare requires an execution route",
        "sbp_sbsql.wire"));
    return result;
  }
  if (!session_.authenticated) {
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "SBSQL.AUTH.REQUIRED", "ERROR", "SBLR prepare requires an authenticated server session",
        "sbp_sbsql.wire"));
    return result;
  }
  if (config_.embedded_engine_direct && embedded_client_ != nullptr) {
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "SBSQL.PREPARE.UNAVAILABLE", "ERROR",
        "server prepared SBLR handles require the SBPS server route",
        "sbp_sbsql.wire"));
    return result;
  }
  SbpsClient client(config_.server_endpoint);
  return client.PrepareSblr(session_, encoded_sblr_envelope);
}

PipelineResult SbsqlTestWireSession::RunPreparedSblrEnvelopeForWire(
    std::string_view prepared_statement_uuid,
    std::string_view encoded_sblr_envelope,
    const std::vector<std::uint8_t>& data_packet,
    bool cursor_requested) {
  PipelineResult result;
  result.accepted = false;
  if (!HasExecutionRoute()) {
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "SBSQL.SERVER.UNAVAILABLE", "ERROR", "prepared SBLR execution requires an execution route",
        "sbp_sbsql.wire"));
    return result;
  }
  if (!session_.authenticated) {
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "SBSQL.AUTH.REQUIRED", "ERROR", "prepared SBLR execution requires an authenticated server session",
        "sbp_sbsql.wire"));
    return result;
  }
  if (config_.embedded_engine_direct && embedded_client_ != nullptr) {
    return RunSblrEnvelopeWithDataPacket(encoded_sblr_envelope, data_packet, cursor_requested);
  }
  SbpsClient client(config_.server_endpoint);
  const auto executed = client.ExecutePreparedSblr(session_,
                                                  prepared_statement_uuid,
                                                  encoded_sblr_envelope,
                                                  data_packet,
                                                  cursor_requested);
  if (!executed.accepted) {
    result.messages = executed.messages;
    return result;
  }
  result.accepted = true;
  result.server_operation_id = executed.operation_id;
  result.server_cursor_uuid = executed.cursor_uuid;
  result.server_row_count = executed.row_count;
  result.server_affected_rows = executed.affected_rows;
  result.server_affected_rows_present = executed.affected_rows_present;
  result.server_result_payload = executed.row_packet;
  ApplyExecutedTransactionState(executed, &session_);
  if (ExecutionInvalidatesNameResolution(executed.operation_id)) {
    const bool preserve_stable_relations =
        ExecutionPreservesReferencedRelationNames(executed.operation_id);
    ClearNameResolutionCache(preserve_stable_relations);
    if (preserve_stable_relations) {
      RehydrateStableRelationNameResolutionCache();
    }
  }
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
