// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "binder/binder.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <functional>
#include <limits>
#include <numeric>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace scratchbird::parser::sbsql {
namespace {

std::string ToLowerAscii(std::string value) {
  for (auto& ch : value) {
    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
  }
  return value;
}

bool ParseFixedTimeSeriesIntervalNs(const std::string_view encoded,
                                    std::int64_t* interval_ns) {
  if (interval_ns == nullptr || encoded.empty()) return false;
  if (std::ranges::all_of(encoded, [](const char ch) {
        return ch >= '0' && ch <= '9';
      })) {
    const auto parsed = std::from_chars(
        encoded.data(), encoded.data() + encoded.size(), *interval_ns);
    return parsed.ec == std::errc{} &&
           parsed.ptr == encoded.data() + encoded.size() && *interval_ns > 0;
  }
  if (!encoded.starts_with('P')) return false;
  std::size_t position = 1;
  bool in_time = false;
  bool component_seen = false;
  int last_order = -1;
  __int128 total = 0;
  while (position < encoded.size()) {
    if (encoded[position] == 'T') {
      if (in_time) return false;
      in_time = true;
      if (++position == encoded.size()) return false;
      continue;
    }
    const auto whole_start = position;
    while (position < encoded.size() && encoded[position] >= '0' &&
           encoded[position] <= '9') ++position;
    if (whole_start == position) return false;
    std::uint64_t whole = 0;
    const auto parsed = std::from_chars(
        encoded.data() + whole_start, encoded.data() + position, whole);
    if (parsed.ec != std::errc{} || parsed.ptr != encoded.data() + position) {
      return false;
    }
    std::uint64_t fraction = 0;
    bool fractional = false;
    if (position < encoded.size() && encoded[position] == '.') {
      fractional = true;
      const auto fraction_start = ++position;
      while (position < encoded.size() && encoded[position] >= '0' &&
             encoded[position] <= '9') ++position;
      const auto digits = position - fraction_start;
      if (digits == 0 || digits > 9) return false;
      const auto fraction_parse = std::from_chars(
          encoded.data() + fraction_start, encoded.data() + position,
          fraction);
      if (fraction_parse.ec != std::errc{} ||
          fraction_parse.ptr != encoded.data() + position) return false;
      for (std::size_t index = digits; index < 9; ++index) fraction *= 10;
    }
    if (position == encoded.size()) return false;
    const auto designator = encoded[position++];
    int order = -1;
    std::uint64_t multiplier = 0;
    if (!in_time && designator == 'D') { order = 0; multiplier = 86'400; }
    else if (in_time && designator == 'H') { order = 1; multiplier = 3'600; }
    else if (in_time && designator == 'M') { order = 2; multiplier = 60; }
    else if (in_time && designator == 'S') { order = 3; multiplier = 1; }
    else return false;
    if (order <= last_order || (fractional && designator != 'S')) return false;
    last_order = order;
    component_seen = true;
    total += static_cast<__int128>(whole) * multiplier * 1'000'000'000;
    if (designator == 'S') total += fraction;
    if (total > std::numeric_limits<std::int64_t>::max()) return false;
  }
  if (!component_seen || total <= 0) return false;
  *interval_ns = static_cast<std::int64_t>(total);
  return true;
}

bool RequiresDescriptorAuthority(const AstDocument& ast) {
  return ast.statement_binding_contract_key == "binder.statement.public_authority_required" ||
         ast.native_relational.recognized() ||
         ast.requires_name_resolution;
}

bool RequiresSecurityAuthority(const AstDocument& ast) {
  return ast.statement_parser_category == "security" ||
         ast.statement_parser_category == "migration" ||
         ast.statement_parser_category == "bridge" ||
         ast.family == StatementFamily::kBridge ||
         ast.statement_binding_contract_key == "binder.statement.cluster_profile_gate";
}

bool RequiresTransactionAuthority(const AstDocument& ast) {
  return ast.statement_parser_category == "transaction" ||
         ast.statement_parser_category == "migration" ||
         ast.statement_binding_contract_key == "binder.statement.transaction_context";
}

bool TokenTextIs(const Token& token, std::string_view expected) {
  return ToUpperAscii(token.text) == expected;
}

bool IsSourceFreeCteRoute(const CstDocument& cst) {
  std::vector<const Token*> tokens;
  for (const auto& token : cst.tokens) {
    if (IsTriviaToken(token)) continue;
    tokens.push_back(&token);
  }
  if (tokens.empty()) return false;
  std::size_t first = 0;
  if (TokenTextIs(*tokens[first], "EXPLAIN")) {
    ++first;
    if (first >= tokens.size()) return false;
  }
  if (TokenTextIs(*tokens[first], "WITH")) return true;
  if (!TokenTextIs(*tokens[first], "SELECT")) return false;
  for (std::size_t index = first + 1; index + 2 < tokens.size(); ++index) {
    if (TokenTextIs(*tokens[index], "FROM") &&
        tokens[index + 1]->text == "(" &&
        TokenTextIs(*tokens[index + 2], "WITH")) {
      return true;
    }
  }
  return false;
}

bool IsQualifiedNamePartToken(const Token& token) {
  return token.kind == TokenKind::kIdentifier || token.kind == TokenKind::kKeyword;
}

bool ConsumeEngineOwnedProjectionPath(const std::vector<const Token*>& tokens,
                                      std::size_t* index) {
  if (index == nullptr) return false;
  std::size_t cursor = *index;
  std::vector<std::string> parts;
  bool expect_part = true;
  while (cursor < tokens.size()) {
    const auto& token = *tokens[cursor];
    if (expect_part) {
      if (!IsQualifiedNamePartToken(token)) break;
      parts.push_back(ToUpperAscii(token.text));
      expect_part = false;
      ++cursor;
      continue;
    }
    if (token.text == ".") {
      expect_part = true;
      ++cursor;
      continue;
    }
    break;
  }
  if (parts.size() < 2 || expect_part) return false;
  bool engine_owned = parts.front() == "SYS" ||
                      parts.front() == "INFORMATION" ||
                      parts.front() == "EMULATED";
  for (std::size_t part = 1; part < parts.size(); ++part) {
    engine_owned = engine_owned || parts[part] == "SYS" ||
                   parts[part] == "INFORMATION" ||
                   parts[part] == "EMULATED";
  }
  if (!engine_owned) return false;
  *index = cursor;
  return true;
}

bool IsSourceFreeCatalogProjectionCountRoute(const CstDocument& cst) {
  std::vector<const Token*> tokens;
  for (const auto& token : cst.tokens) {
    if (IsTriviaToken(token)) continue;
    tokens.push_back(&token);
  }
  if (tokens.empty() || !TokenTextIs(*tokens.front(), "SELECT")) return false;

  std::size_t from_index = tokens.size();
  int paren_depth = 0;
  bool saw_count_projection = false;
  for (std::size_t index = 1; index < tokens.size(); ++index) {
    if (tokens[index]->text == "(") {
      ++paren_depth;
    } else if (tokens[index]->text == ")" && paren_depth > 0) {
      --paren_depth;
    }
    if (paren_depth == 0 && TokenTextIs(*tokens[index], "FROM")) {
      from_index = index;
      break;
    }
    if (TokenTextIs(*tokens[index], "COUNT")) {
      saw_count_projection = true;
    }
  }
  if (!saw_count_projection || from_index == tokens.size()) return false;

  std::size_t relation_index = from_index + 1;
  return ConsumeEngineOwnedProjectionPath(tokens, &relation_index);
}

std::string ResultShapeFor(const AstDocument& ast) {
  if (ast.family == StatementFamily::kQuery || ast.family == StatementFamily::kValues) {
    return "result.shape.rowset";
  }
  if (ast.family == StatementFamily::kShow || ast.family == StatementFamily::kObservability) {
    return "result.shape.management_report";
  }
  if (ast.family == StatementFamily::kCall) return "result.shape.routine_result";
  return "result.shape.command_status";
}

std::string ResourceContractFor(const AstDocument& ast) {
  if (ast.family == StatementFamily::kQuery || ast.family == StatementFamily::kValues) {
    return "resource.contract.query_read";
  }
  if (ast.family == StatementFamily::kInsert || ast.family == StatementFamily::kUpdate ||
      ast.family == StatementFamily::kDelete || ast.family == StatementFamily::kMerge ||
      ast.family == StatementFamily::kUpsert) {
    return "resource.contract.dml_write";
  }
  if (ast.family == StatementFamily::kCatalog || ast.family == StatementFamily::kSecurity ||
      ast.family == StatementFamily::kStorageManagement ||
      ast.family == StatementFamily::kMigration ||
      ast.family == StatementFamily::kBridge) {
    return "resource.contract.metadata_mutation";
  }
  return "resource.contract.control";
}

std::string RequiredRightFor(const AstDocument& ast) {
  if (ast.family == StatementFamily::kQuery || ast.family == StatementFamily::kValues) {
    return "right.read";
  }
  if (ast.family == StatementFamily::kInsert || ast.family == StatementFamily::kUpdate ||
      ast.family == StatementFamily::kDelete || ast.family == StatementFamily::kMerge ||
      ast.family == StatementFamily::kUpsert) {
    return "right.write";
  }
  if (ast.family == StatementFamily::kCatalog) return "right.catalog_mutate";
  if (ast.family == StatementFamily::kSecurity) return "right.security_admin";
  if (ast.family == StatementFamily::kTransaction) return "right.transaction_control";
  if (ast.family == StatementFamily::kMigration) return "right.migrate_database";
  if (ast.family == StatementFamily::kBridge) return "right.bridge.use";
  if (ast.family == StatementFamily::kShow || ast.family == StatementFamily::kObservability) {
    return "right.observe";
  }
  return "right.execute";
}

void PopulateAuthorityMetadata(BoundStatement* bound, const AstDocument& ast) {
  bound->statement_surface_id = ast.statement_surface_id;
  bound->statement_surface_name = ast.statement_surface_name;
  bound->statement_parser_category = ast.statement_parser_category;
  bound->parser_handler_key = ast.parser_handler_key;
  bound->binding_contract_key = ast.statement_binding_contract_key;
  bound->admission_contract_key = ast.statement_admission_contract_key;
  bound->behavior_descriptor_key = ast.statement_behavior_descriptor_key;
  bound->diagnostic_key = ast.diagnostic_key;
  bound->requires_name_resolution = ast.requires_name_resolution;
  bound->requires_descriptor_authority = RequiresDescriptorAuthority(ast);
  bound->requires_security_authority = RequiresSecurityAuthority(ast);
  bound->requires_transaction_authority = RequiresTransactionAuthority(ast);
  bound->requires_cluster_profile = ast.requires_cluster_profile;
  bound->exact_refusal_required = ast.exact_refusal_required;
  bound->command_family = ast.statement_kind;
  bound->surface_key = ast.statement_surface_id.empty() ? ast.registry_family
                                                        : ast.statement_surface_id;
  bound->sblr_operation_key = ast.operation_family;
  bound->result_shape_key = ResultShapeFor(ast);
  bound->diagnostic_shape_key = ast.diagnostic_key.empty() ? "diagnostic.canonical_message_vector"
                                                           : ast.diagnostic_key;
  bound->resource_contract_key = ResourceContractFor(ast);
  bound->conformance_case_key = ast.statement_surface_id.empty()
                                    ? "conformance.unclassified_statement"
                                    : "conformance." + ast.statement_surface_id;
  bound->trace_key = "trace.bound_ast." + std::to_string(ast.source_hash);
  bound->edition_gate_result = "edition_gate.not_evaluated_parser_binder";
  bound->profile_gate_result = bound->requires_cluster_profile
                                   ? "profile_gate.cluster_required"
                                   : "profile_gate.public_or_default";
  bound->granted_scope = "granted_scope.pending_server_authority";
  bound->required_rights.push_back(RequiredRightFor(ast));
  if (bound->requires_descriptor_authority) {
    bound->descriptor_refs.push_back("descriptor.pending_server_or_engine_authority");
  }
  if (bound->requires_security_authority) {
    bound->policy_refs.push_back("policy.pending_server_security_authority");
  }

  bound->name_resolution_authority_key =
      bound->requires_name_resolution ? "authority.server.resolve_name_registry_public"
                                      : "authority.not_required.parser_syntax_only";
  bound->descriptor_authority_key =
      bound->requires_descriptor_authority ? "authority.engine.descriptor_context_required"
                                           : "authority.not_required.parser_syntax_only";
  bound->security_authority_key =
      bound->requires_security_authority ? "authority.server.security_policy_context_required"
                                         : "authority.not_required.parser_syntax_only";
  bound->transaction_authority_key =
      bound->requires_transaction_authority ? "authority.server.transaction_context_required"
                                            : "authority.not_required.parser_syntax_only";

  bound->required_authority_steps.push_back("authority.parser.syntax_evidence_only");
  if (!bound->statement_surface_id.empty()) {
    bound->required_authority_steps.push_back("authority.parser.surface_descriptor_candidate");
  }
  if (bound->requires_name_resolution) {
    bound->required_authority_steps.push_back(bound->name_resolution_authority_key);
  }
  if (bound->requires_descriptor_authority) {
    bound->required_authority_steps.push_back(bound->descriptor_authority_key);
  }
  if (bound->requires_security_authority) {
    bound->required_authority_steps.push_back(bound->security_authority_key);
  }
  if (bound->requires_transaction_authority) {
    bound->required_authority_steps.push_back(bound->transaction_authority_key);
  }
  if (bound->requires_cluster_profile) {
    bound->required_authority_steps.push_back("authority.cluster.profile_gate_required");
  }
}

void AddBoundAstDiagnostic(BoundNativeRelationalDocument* document,
                           std::string code,
                           std::string message,
                           std::vector<Field> fields = {}) {
  if (document->messages.has_errors()) return;
  document->messages.diagnostics.push_back(MakeDiagnostic(
      std::move(code), "ERROR", std::move(message), "sbp_sbsql.native_binder",
      std::move(fields)));
}

bool LooksLikeUuidV7(const std::string_view value) {
  return LooksLikeCanonicalUuid(value) && value[14] == '7';
}

bool IsNonNullCanonicalUuid(const std::string_view value) {
  return LooksLikeCanonicalUuid(value) &&
         value != "00000000-0000-0000-0000-000000000000";
}

bool IsCanonicalStatementTimestamp(std::string_view value) {
  if (value.size() != 20 &&
      (value.size() < 22 || value.size() > 30)) {
    return false;
  }
  if (value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
      value[13] != ':' || value[16] != ':' || value.back() != 'Z') {
    return false;
  }
  constexpr std::size_t kDigitIndexes[] = {
      0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18};
  for (const auto index : kDigitIndexes) {
    if (value[index] < '0' || value[index] > '9') return false;
  }
  if (value.size() > 20) {
    if (value[19] != '.') return false;
    for (std::size_t index = 20; index + 1 < value.size(); ++index) {
      if (value[index] < '0' || value[index] > '9') return false;
    }
  }
  const auto decimal = [&](std::size_t offset, std::size_t digits) {
    unsigned result = 0;
    for (std::size_t index = 0; index < digits; ++index) {
      result = result * 10 +
               static_cast<unsigned>(value[offset + index] - '0');
    }
    return result;
  };
  const auto year = decimal(0, 4);
  const auto month = decimal(5, 2);
  const auto day = decimal(8, 2);
  const auto hour = decimal(11, 2);
  const auto minute = decimal(14, 2);
  const auto second = decimal(17, 2);
  if (year == 0 || month == 0 || month > 12 || hour > 23 || minute > 59 ||
      second > 59) {
    return false;
  }
  constexpr unsigned kDaysByMonth[] = {
      0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  auto maximum_day = kDaysByMonth[month];
  const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  if (month == 2 && leap) ++maximum_day;
  return day != 0 && day <= maximum_day;
}

bool IsCatalogRelationObjectType(const std::string_view value) {
  return value == "relation" || value == "table" || value == "view" ||
         value == "materialized_view" || value == "external_table" ||
         value == "foreign_table";
}

std::string_view ExpectedAggregateSemanticVariant(
    const NativeAggregateGroupingForm grouping_form,
    const NativeAggregateProjectionForm projection_form) {
  if (grouping_form == NativeAggregateGroupingForm::kSimple) {
    if (projection_form == NativeAggregateProjectionForm::kKeyCountSum) {
      return "aggregate.grouped-int64-key-count-sum.v1";
    }
    if (projection_form == NativeAggregateProjectionForm::kKeysCountSum) {
      return "aggregate.grouped-int64-keys-count-sum.v1";
    }
    return {};
  }
  const bool projects_grouping_metadata =
      projection_form ==
      NativeAggregateProjectionForm::kKeysCountSumGrouping;
  if (projection_form != NativeAggregateProjectionForm::kKeysCountSum &&
      !projects_grouping_metadata) {
    return {};
  }
  switch (grouping_form) {
    case NativeAggregateGroupingForm::kGroupingSets:
      return projects_grouping_metadata
                 ? "aggregate.grouping-sets-int64-keys-count-sum-grouping.v1"
                 : "aggregate.grouping-sets-int64-keys-count-sum.v1";
    case NativeAggregateGroupingForm::kRollup:
      return projects_grouping_metadata
                 ? "aggregate.rollup-int64-keys-count-sum-grouping.v1"
                 : "aggregate.rollup-int64-keys-count-sum.v1";
    case NativeAggregateGroupingForm::kCube:
      return projects_grouping_metadata
                 ? "aggregate.cube-int64-keys-count-sum-grouping.v1"
                 : "aggregate.cube-int64-keys-count-sum.v1";
    case NativeAggregateGroupingForm::kSimple:
    case NativeAggregateGroupingForm::kNone:
      return {};
  }
  return {};
}

BoundNativeRelationalDocument RefusedBoundAst(
    BoundNativeRelationalDocument document) {
  document.bound = false;
  document.bound_ast_uuid.clear();
  document.security_context_uuid.clear();
  document.statement_uuid.clear();
  document.statement_timestamp.clear();
  document.owning_transaction_uuid.clear();
  document.statement_snapshot_uuid.clear();
  document.statement_metadata_snapshot_uuid.clear();
  document.local_transaction_id = 0;
  document.snapshot_visible_through_local_transaction_id = 0;
  document.root_relation_id = 0;
  document.root_scope_id = 0;
  document.descriptors.clear();
  document.expressions.clear();
  document.values_rows.clear();
  document.grouping_sets.clear();
  document.window_definitions.clear();
  document.window_invocations.clear();
  document.outputs.clear();
  document.relations.clear();
  document.catalog_relation_sources.clear();
  document.scopes.clear();
  return document;
}

} // namespace

// QOW-SOURCE-QRY-001-BINDING-V1
BoundNativeRelationalDocument BindNativeRelationalAst(
    const NativeRelationalAstDocument& ast,
    const NativeRelationalBindingContext& context) {
  BoundNativeRelationalDocument bound;
  if (!ast.accepted()) {
    AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                          "only an accepted typed relational AST can be bound");
    return RefusedBoundAst(std::move(bound));
  }
  const auto key_value_source_ast = std::ranges::find_if(
      ast.catalog_relation_sources, [](const auto& source) {
        return source.source_kind == NativeRelationSourceAstKind::kKeyValue;
      });
  const auto time_series_source_ast = std::ranges::find_if(
      ast.catalog_relation_sources, [](const auto& source) {
        return source.source_kind == NativeRelationSourceAstKind::kTimeSeries;
      });
  const auto vector_source_ast = std::ranges::find_if(
      ast.catalog_relation_sources, [](const auto& source) {
        return source.source_kind == NativeRelationSourceAstKind::kVector;
      });
  const auto search_source_ast = std::ranges::find_if(
      ast.catalog_relation_sources, [](const auto& source) {
        return source.source_kind == NativeRelationSourceAstKind::kSearch;
      });
  const auto spatial_source_ast = std::ranges::find_if(
      ast.catalog_relation_sources, [](const auto& source) {
        return source.source_kind == NativeRelationSourceAstKind::kSpatial;
      });
  const auto columnar_source_ast = std::ranges::find_if(
      ast.catalog_relation_sources, [](const auto& source) {
        return source.source_kind == NativeRelationSourceAstKind::kColumnar;
      });
  if (!LooksLikeUuidV7(context.bound_ast_uuid)) {
    AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-SCOPE",
                          "binding requires a non-null UUIDv7 BoundAST identity");
    return RefusedBoundAst(std::move(bound));
  }
  if (!IsNonNullCanonicalUuid(context.catalog_epoch_uuid)) {
    AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-SCOPE",
                          "binding requires an engine-supplied catalog epoch UUID");
    return RefusedBoundAst(std::move(bound));
  }
  if (!LooksLikeCanonicalUuid(context.security_context_uuid)) {
    AddBoundAstDiagnostic(
        &bound, "QOW-DIAG-BOUNDAST-SCOPE",
        "binding requires an engine-supplied security context UUID");
    return RefusedBoundAst(std::move(bound));
  }
  if (!IsNonNullCanonicalUuid(context.statement_uuid) ||
      !IsNonNullCanonicalUuid(context.owning_transaction_uuid) ||
      !IsNonNullCanonicalUuid(context.statement_snapshot_uuid) ||
      !IsNonNullCanonicalUuid(context.statement_metadata_snapshot_uuid)) {
    AddBoundAstDiagnostic(
        &bound, "QOW-DIAG-BOUNDAST-SCOPE",
        "binding requires non-null canonical engine-supplied MGA statement UUIDs");
    return RefusedBoundAst(std::move(bound));
  }
  if (context.local_transaction_id == 0) {
    AddBoundAstDiagnostic(
        &bound, "QOW-DIAG-BOUNDAST-SCOPE",
        "binding requires a nonzero engine-supplied local transaction number");
    return RefusedBoundAst(std::move(bound));
  }
  const auto& authority = context.engine_statement_authority;
  if (!IsNonNullCanonicalUuid(authority.statement_uuid) ||
      !IsNonNullCanonicalUuid(authority.transaction_uuid) ||
      !IsNonNullCanonicalUuid(authority.statement_snapshot_uuid) ||
      !IsNonNullCanonicalUuid(
          authority.statement_metadata_snapshot_uuid) ||
      !IsNonNullCanonicalUuid(authority.catalog_epoch_uuid) ||
      authority.local_transaction_id == 0) {
    AddBoundAstDiagnostic(
        &bound, "QOW-DIAG-BOUNDAST-SCOPE",
        "binding requires complete non-null engine MGA statement authority");
    return RefusedBoundAst(std::move(bound));
  }
  const bool timestamp_model_source =
      key_value_source_ast != ast.catalog_relation_sources.end() ||
      time_series_source_ast != ast.catalog_relation_sources.end() ||
      vector_source_ast != ast.catalog_relation_sources.end() ||
      search_source_ast != ast.catalog_relation_sources.end() ||
      spatial_source_ast != ast.catalog_relation_sources.end() ||
      columnar_source_ast != ast.catalog_relation_sources.end();
  if (timestamp_model_source &&
      (!IsCanonicalStatementTimestamp(context.statement_timestamp) ||
       context.statement_timestamp != authority.statement_timestamp)) {
    AddBoundAstDiagnostic(
        &bound,
        time_series_source_ast != ast.catalog_relation_sources.end()
            ? "SB_MODEL_TIME_SERIES_TIMESTAMP_INVALID_V1"
            : (vector_source_ast != ast.catalog_relation_sources.end() ||
               search_source_ast != ast.catalog_relation_sources.end() ||
               spatial_source_ast != ast.catalog_relation_sources.end() ||
               columnar_source_ast != ast.catalog_relation_sources.end()
                   ? "SB_MODEL_MGA_CONTEXT_MISMATCH_V1"
                   : "SB_MODEL_KEY_VALUE_STATEMENT_TIMESTAMP_INVALID_V1"),
        "model binding timestamp does not exactly match engine authority");
    return RefusedBoundAst(std::move(bound));
  }
  if (context.statement_uuid != authority.statement_uuid ||
      context.statement_timestamp != authority.statement_timestamp ||
      context.owning_transaction_uuid != authority.transaction_uuid ||
      context.statement_snapshot_uuid != authority.statement_snapshot_uuid ||
      context.statement_metadata_snapshot_uuid !=
          authority.statement_metadata_snapshot_uuid ||
      context.catalog_epoch_uuid != authority.catalog_epoch_uuid ||
      context.local_transaction_id != authority.local_transaction_id ||
      context.snapshot_visible_through_local_transaction_id !=
          authority.snapshot_visible_through_local_transaction_id) {
    AddBoundAstDiagnostic(
        &bound, "QOW-DIAG-BOUNDAST-SCOPE",
        "binding MGA statement context does not exactly match engine authority");
    return RefusedBoundAst(std::move(bound));
  }
  bound.statement_uuid = context.statement_uuid;
  bound.statement_timestamp = context.statement_timestamp;
  bound.owning_transaction_uuid = context.owning_transaction_uuid;
  bound.statement_snapshot_uuid = context.statement_snapshot_uuid;
  bound.statement_metadata_snapshot_uuid =
      context.statement_metadata_snapshot_uuid;
  bound.local_transaction_id = context.local_transaction_id;
  bound.snapshot_visible_through_local_transaction_id =
      context.snapshot_visible_through_local_transaction_id;

  std::unordered_map<std::uint32_t, const NativeDescriptorBindingInput*>
      descriptor_by_id;
  std::unordered_set<std::string> descriptor_uuids;
  for (const auto& descriptor : context.descriptors) {
    if (descriptor.descriptor_id == 0 ||
        !LooksLikeCanonicalUuid(descriptor.descriptor_uuid) ||
        !LooksLikeCanonicalUuid(descriptor.type_uuid) ||
        (descriptor.collation_uuid.has_value() &&
         !LooksLikeCanonicalUuid(*descriptor.collation_uuid)) ||
        (descriptor.width_precision_scale.scale.has_value() &&
         (!descriptor.width_precision_scale.precision.has_value() ||
          *descriptor.width_precision_scale.scale >
              *descriptor.width_precision_scale.precision))) {
      AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-DESCRIPTOR",
                            "descriptor binding contains an invalid typed field");
      return RefusedBoundAst(std::move(bound));
    }
    if (!descriptor_by_id.emplace(descriptor.descriptor_id, &descriptor).second ||
        !descriptor_uuids.emplace(descriptor.descriptor_uuid).second) {
      AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-DESCRIPTOR",
                            "descriptor IDs and UUID handles must be unique");
      return RefusedBoundAst(std::move(bound));
    }
  }
  if (descriptor_by_id.empty()) {
    AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-DESCRIPTOR",
                          "typed relational binding requires descriptor handles");
    return RefusedBoundAst(std::move(bound));
  }

  const bool has_catalog_relation_ast = std::ranges::any_of(
      ast.relations, [](const auto& relation) {
        return relation.relation_kind == NativeRelationAstKind::kCatalogSource;
      });
  if (ast.catalog_relation_sources.size() >= 3 &&
      ast.catalog_relation_sources.size() <= 9) {
    // QOW-SOURCE-RCP-080-BOUNDED-MULTIMODEL-JOIN-BINDING-V1
    const auto source_count = ast.catalog_relation_sources.size();
    std::vector<const NativeRelationAstNode*> source_relations(source_count,
                                                               nullptr);
    std::vector<const NativeRelationAstNode*> join_relations;
    const NativeRelationAstNode* filter_relation = nullptr;
    const NativeRelationAstNode* project_relation = nullptr;
    for (const auto& relation : ast.relations) {
      if (relation.relation_kind == NativeRelationAstKind::kCatalogSource &&
          relation.relation_source_ids.size() == 1 &&
          relation.relation_source_ids.front() >= 1 &&
          relation.relation_source_ids.front() <= source_count) {
        auto*& slot = source_relations[relation.relation_source_ids.front() - 1];
        if (slot != nullptr) {
          AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                                "multimodel source relation is duplicated");
          return RefusedBoundAst(std::move(bound));
        }
        slot = &relation;
      } else if (relation.relation_kind == NativeRelationAstKind::kJoin) {
        join_relations.push_back(&relation);
      } else if (relation.relation_kind == NativeRelationAstKind::kFilter &&
                 filter_relation == nullptr) {
        filter_relation = &relation;
      } else if (relation.relation_kind == NativeRelationAstKind::kProject &&
                 project_relation == nullptr) {
        project_relation = &relation;
      }
    }
    std::ranges::sort(join_relations, {},
                      [](const auto* relation) { return relation->relation_id; });
    const auto model_source_count = std::ranges::count_if(
        ast.catalog_relation_sources, [](const auto& source) {
          return source.source_kind !=
                 NativeRelationSourceAstKind::kCatalogRelation;
        });
    const bool ordinary_multi_catalog_cross_join =
        model_source_count == 0 &&
        ast.model_object_resolution_requests.empty() &&
        std::ranges::all_of(ast.catalog_relation_sources,
                            [](const auto& source) {
                              return IsExactOrdinaryCatalogSourceProfile(source);
                            });
    const bool bounded_multimodel_join = model_source_count >= 2;
    const auto final_join_relation_id =
        static_cast<std::uint32_t>((2 * source_count) - 1);
    const bool filter_composition =
        ordinary_multi_catalog_cross_join && filter_relation != nullptr &&
        project_relation == nullptr && !join_relations.empty() &&
        filter_relation->relation_id == final_join_relation_id + 1 &&
        filter_relation->input_relation_ids ==
            std::vector<std::uint32_t>{final_join_relation_id} &&
        filter_relation->output_expression_ids ==
            join_relations.back()->output_expression_ids &&
        filter_relation->predicate_expression_ids.size() == 1 &&
        filter_relation->relation_source_ids.empty() &&
        filter_relation->values_row_ids.empty() &&
        filter_relation->grouping_key_expression_ids.empty() &&
        filter_relation->aggregate_expression_ids.empty() &&
        filter_relation->limit_expression_ids.empty() &&
        filter_relation->window_invocation_ids.empty() &&
        filter_relation->ordering_terms.empty() &&
        filter_relation->join_kind == NativeJoinAstKind::kNone &&
        filter_relation->aggregate_grouping_form ==
            NativeAggregateGroupingForm::kNone &&
        filter_relation->aggregate_projection_form ==
            NativeAggregateProjectionForm::kNone;
    const bool project_composition =
        ordinary_multi_catalog_cross_join && project_relation != nullptr &&
        filter_relation == nullptr &&
        project_relation->relation_id == final_join_relation_id + 1 &&
        project_relation->input_relation_ids ==
            std::vector<std::uint32_t>{final_join_relation_id} &&
        !project_relation->output_expression_ids.empty() &&
        project_relation->relation_source_ids.empty() &&
        project_relation->values_row_ids.empty() &&
        project_relation->grouping_key_expression_ids.empty() &&
        project_relation->aggregate_expression_ids.empty() &&
        project_relation->predicate_expression_ids.empty() &&
        project_relation->limit_expression_ids.empty() &&
        project_relation->window_invocation_ids.empty() &&
        project_relation->ordering_terms.empty() &&
        project_relation->join_kind == NativeJoinAstKind::kNone &&
        project_relation->aggregate_grouping_form ==
            NativeAggregateGroupingForm::kNone &&
        project_relation->aggregate_projection_form ==
            NativeAggregateProjectionForm::kNone;
    bool exact_graph =
        std::ranges::none_of(source_relations,
                            [](const auto* relation) { return relation == nullptr; }) &&
        join_relations.size() + 1 == source_count &&
        ast.relations.size() ==
            source_count + join_relations.size() +
                static_cast<std::size_t>(filter_composition) +
                static_cast<std::size_t>(project_composition) &&
        context.catalog_relations.size() == source_count &&
        context.relations.size() == join_relations.size() &&
        (filter_relation == nullptr || filter_composition) &&
        (project_relation == nullptr || project_composition) &&
        (!bounded_multimodel_join || filter_relation == nullptr) &&
        (!bounded_multimodel_join || project_relation == nullptr);
    if (ordinary_multi_catalog_cross_join) {
      for (std::size_t ordinal = 0;
           exact_graph && ordinal < source_relations.size(); ++ordinal) {
        const auto* source_relation = source_relations[ordinal];
        exact_graph =
            source_relation->relation_id == ordinal + 1 &&
            source_relation->input_relation_ids.empty() &&
            source_relation->relation_source_ids ==
                std::vector<std::uint32_t>{
                    static_cast<std::uint32_t>(ordinal + 1)} &&
            source_relation->values_row_ids.empty() &&
            source_relation->output_expression_ids.size() == 1 &&
            source_relation->grouping_key_expression_ids.empty() &&
            source_relation->aggregate_expression_ids.empty() &&
            source_relation->predicate_expression_ids.empty() &&
            source_relation->limit_expression_ids.empty() &&
            source_relation->window_invocation_ids.empty() &&
            source_relation->ordering_terms.empty() &&
            source_relation->join_kind == NativeJoinAstKind::kNone &&
            source_relation->aggregate_grouping_form ==
                NativeAggregateGroupingForm::kNone &&
            source_relation->aggregate_projection_form ==
                NativeAggregateProjectionForm::kNone;
      }
    }
    std::uint32_t left_relation_id = 0;
    if (exact_graph) {
      left_relation_id = source_relations.front()->relation_id;
    }
    std::vector<std::uint32_t> expected_join_output_expression_ids;
    if (!source_relations.empty()) {
      expected_join_output_expression_ids =
          source_relations.front()->output_expression_ids;
    }
    for (std::size_t ordinal = 1;
         exact_graph && ordinal < source_count; ++ordinal) {
      const auto* join = join_relations[ordinal - 1];
      const auto& relation_binding = context.relations[ordinal - 1];
      expected_join_output_expression_ids.insert(
          expected_join_output_expression_ids.end(),
          source_relations[ordinal]->output_expression_ids.begin(),
          source_relations[ordinal]->output_expression_ids.end());
      exact_graph =
          join->relation_id == source_count + ordinal &&
          join->input_relation_ids ==
              std::vector<std::uint32_t>{
                  left_relation_id, source_relations[ordinal]->relation_id} &&
          join->predicate_expression_ids.empty() &&
          join->relation_source_ids.empty() && join->values_row_ids.empty() &&
          join->output_expression_ids == expected_join_output_expression_ids &&
          join->grouping_key_expression_ids.empty() &&
          join->aggregate_expression_ids.empty() &&
          join->limit_expression_ids.empty() &&
          join->window_invocation_ids.empty() &&
          join->ordering_terms.empty() &&
          join->join_kind == NativeJoinAstKind::kCross &&
          join->aggregate_grouping_form == NativeAggregateGroupingForm::kNone &&
          join->aggregate_projection_form ==
              NativeAggregateProjectionForm::kNone &&
          relation_binding.relation_id == join->relation_id &&
          relation_binding.semantic_variant_id == "join.cross.v1";
      left_relation_id = join->relation_id;
    }
    exact_graph =
        exact_graph &&
        ast.root_relation_id ==
            (project_composition
                 ? project_relation->relation_id
                 : (filter_composition ? filter_relation->relation_id
                                       : left_relation_id));
    if (!exact_graph ||
        (!ordinary_multi_catalog_cross_join && !bounded_multimodel_join)) {
      AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                            "bounded multi-source JOIN graph is incomplete");
      return RefusedBoundAst(std::move(bound));
    }

    std::unordered_map<std::uint32_t, const NativeExpressionAstNode*>
        ast_expression_by_id;
    for (const auto& expression : ast.expressions) {
      if (expression.expression_id == 0 ||
          !ast_expression_by_id.emplace(expression.expression_id, &expression)
               .second) {
        AddBoundAstDiagnostic(&bound, "SB_MODEL_BINDING_INCOMPLETE_V1",
                              "multimodel AST expression identity is invalid");
        return RefusedBoundAst(std::move(bound));
      }
    }
    std::vector<const NativeExpressionAstNode*> project_identifiers;
    std::unordered_set<std::uint32_t> project_expression_ids;
    if (project_composition) {
      std::unordered_set<std::string> project_names;
      for (const auto expression_id : project_relation->output_expression_ids) {
        const auto expression = ast_expression_by_id.find(expression_id);
        if (expression == ast_expression_by_id.end() ||
            expression->second->expression_kind !=
                NativeExpressionAstKind::kIdentifier ||
            expression->second->qualified_identifier.size() != 1 ||
            expression->second->qualified_identifier.front().spelling !=
                expression->second->spelling ||
            expression->second->spelling.empty() ||
            !expression->second->child_expression_ids.empty() ||
            expression->second->literal_kind.has_value() ||
            !expression->second->operator_name.empty() ||
            expression->second->structural_literal_occurrence_id != 0 ||
            expression->second->structural_parameter_occurrence_id != 0 ||
            expression->second->structural_variable_occurrence_id != 0 ||
            !project_expression_ids.insert(expression_id).second ||
            !project_names
                 .insert(expression->second->qualified_identifier.front().quoted
                             ? expression->second->qualified_identifier.front()
                                   .spelling
                             : ToLowerAscii(
                                   expression->second->qualified_identifier.front()
                                       .spelling))
                 .second) {
          AddBoundAstDiagnostic(
              &bound, "QOW-DIAG-BOUNDAST-EXPRESSION",
              "ordinary multi-source PROJECT identifier is not exact");
          return RefusedBoundAst(std::move(bound));
        }
        project_identifiers.push_back(expression->second);
      }
    }
    const NativeExpressionAstNode* filter_predicate = nullptr;
    const NativeExpressionAstNode* filter_identifier = nullptr;
    const NativeExpressionAstNode* filter_value = nullptr;
    std::unordered_set<std::uint32_t> filter_expression_ids;
    if (filter_composition) {
      const auto predicate = ast_expression_by_id.find(
          filter_relation->predicate_expression_ids.front());
      if (predicate != ast_expression_by_id.end() &&
          predicate->second->child_expression_ids.size() == 2) {
        filter_predicate = predicate->second;
        const auto identifier = ast_expression_by_id.find(
            filter_predicate->child_expression_ids[0]);
        const auto value = ast_expression_by_id.find(
            filter_predicate->child_expression_ids[1]);
        if (identifier != ast_expression_by_id.end()) {
          filter_identifier = identifier->second;
        }
        if (value != ast_expression_by_id.end()) filter_value = value->second;
      }
      const bool accepted_operator =
          filter_predicate != nullptr &&
          (filter_predicate->operator_name == "=" ||
           filter_predicate->operator_name == "<>" ||
           filter_predicate->operator_name == "!=" ||
           filter_predicate->operator_name == "<" ||
           filter_predicate->operator_name == "<=" ||
           filter_predicate->operator_name == ">" ||
           filter_predicate->operator_name == ">=");
      const bool numeric_literal =
          filter_value != nullptr &&
          filter_value->expression_kind == NativeExpressionAstKind::kLiteral &&
          filter_value->literal_kind == NativeLiteralAstKind::kNumeric;
      const bool parameter_value =
          filter_value != nullptr &&
          filter_value->expression_kind == NativeExpressionAstKind::kParameter;
      const bool variable_value =
          filter_value != nullptr &&
          filter_value->expression_kind == NativeExpressionAstKind::kVariable;
      std::uint64_t parsed_numeric = 0;
      const auto parsed =
          numeric_literal
              ? std::from_chars(filter_value->spelling.data(),
                                filter_value->spelling.data() +
                                    filter_value->spelling.size(),
                                parsed_numeric)
              : std::from_chars_result{};
      const bool exact_value_occurrence =
          numeric_literal
              ? filter_value->structural_literal_occurrence_id != 0 &&
                    filter_value->structural_parameter_occurrence_id == 0 &&
                    filter_value->structural_variable_occurrence_id == 0
          : parameter_value
              ? filter_value->structural_literal_occurrence_id == 0 &&
                    filter_value->structural_parameter_occurrence_id != 0 &&
                    filter_value->structural_variable_occurrence_id == 0 &&
                    !filter_value->literal_kind.has_value()
              : variable_value &&
                    filter_value->structural_literal_occurrence_id == 0 &&
                    filter_value->structural_parameter_occurrence_id == 0 &&
                    filter_value->structural_variable_occurrence_id != 0 &&
                    !filter_value->literal_kind.has_value();
      if (!accepted_operator || filter_identifier == nullptr ||
          filter_value == nullptr ||
          filter_predicate->expression_kind !=
              NativeExpressionAstKind::kBinary ||
          filter_predicate->literal_kind.has_value() ||
          !filter_predicate->qualified_identifier.empty() ||
          filter_predicate->structural_literal_occurrence_id != 0 ||
          filter_predicate->structural_parameter_occurrence_id != 0 ||
          filter_predicate->structural_variable_occurrence_id != 0 ||
          filter_identifier->expression_kind !=
              NativeExpressionAstKind::kIdentifier ||
          filter_identifier->qualified_identifier.size() != 1 ||
          filter_identifier->qualified_identifier.front().spelling !=
              filter_identifier->spelling ||
          filter_identifier->spelling.empty() ||
          !filter_identifier->child_expression_ids.empty() ||
          filter_identifier->literal_kind.has_value() ||
          !filter_identifier->operator_name.empty() ||
          filter_identifier->structural_literal_occurrence_id != 0 ||
          filter_identifier->structural_parameter_occurrence_id != 0 ||
          filter_identifier->structural_variable_occurrence_id != 0 ||
          !filter_value->qualified_identifier.empty() ||
          !filter_value->child_expression_ids.empty() ||
          !filter_value->operator_name.empty() || !exact_value_occurrence ||
          (numeric_literal &&
           (filter_value->spelling.empty() ||
            (filter_value->spelling.size() > 1 &&
             filter_value->spelling.front() == '0') ||
            parsed.ec != std::errc{} ||
            parsed.ptr != filter_value->spelling.data() +
                              filter_value->spelling.size()))) {
        AddBoundAstDiagnostic(
            &bound, "QOW-DIAG-BOUNDAST-EXPRESSION",
            "ordinary multi-source FILTER predicate is not exact");
        return RefusedBoundAst(std::move(bound));
      }
      filter_expression_ids = {
          filter_identifier->expression_id, filter_value->expression_id,
          filter_predicate->expression_id};
      if (filter_expression_ids.size() != 3) {
        AddBoundAstDiagnostic(
            &bound, "QOW-DIAG-BOUNDAST-EXPRESSION",
            "ordinary multi-source FILTER expression identities overlap");
        return RefusedBoundAst(std::move(bound));
      }
    }
    if (ordinary_multi_catalog_cross_join) {
      std::unordered_set<std::uint32_t> source_wildcard_ids;
      bool exact_source_expression_inventory =
          ast.expressions.size() ==
          source_count + project_expression_ids.size() +
              filter_expression_ids.size();
      for (const auto* source_relation : source_relations) {
        if (!exact_source_expression_inventory ||
            source_relation->output_expression_ids.size() != 1) {
          exact_source_expression_inventory = false;
          break;
        }
        const auto expression_id =
            source_relation->output_expression_ids.front();
        const auto expression = ast_expression_by_id.find(expression_id);
        if (expression == ast_expression_by_id.end() ||
            !source_wildcard_ids.insert(expression_id).second ||
            expression->second->expression_kind !=
                NativeExpressionAstKind::kWildcard ||
            expression->second->spelling != "*" ||
            !expression->second->qualified_identifier.empty() ||
            !expression->second->child_expression_ids.empty() ||
            expression->second->literal_kind.has_value() ||
            !expression->second->operator_name.empty() ||
            expression->second->structural_literal_occurrence_id != 0 ||
            expression->second->structural_parameter_occurrence_id != 0 ||
            expression->second->structural_variable_occurrence_id != 0) {
          exact_source_expression_inventory = false;
          break;
        }
      }
      exact_source_expression_inventory =
          exact_source_expression_inventory &&
          std::ranges::none_of(project_expression_ids, [&](const auto id) {
            return source_wildcard_ids.contains(id);
          }) &&
          std::ranges::none_of(filter_expression_ids, [&](const auto id) {
            return source_wildcard_ids.contains(id) ||
                   project_expression_ids.contains(id);
          }) &&
          source_wildcard_ids.size() + project_expression_ids.size() +
                  filter_expression_ids.size() ==
              ast_expression_by_id.size();
      if (!exact_source_expression_inventory) {
        AddBoundAstDiagnostic(
            &bound, "QOW-DIAG-BOUNDAST-EXPRESSION",
            "ordinary multi-source CROSS JOIN expression inventory is incomplete");
        return RefusedBoundAst(std::move(bound));
      }
    }
    std::vector<std::vector<std::uint32_t>> source_closure_ids(source_count);
    std::unordered_map<std::uint32_t, std::size_t> expression_owner;
    for (std::size_t source_ordinal = 0; source_ordinal < source_count;
         ++source_ordinal) {
      const auto& source = ast.catalog_relation_sources[source_ordinal];
      if (source.source_kind == NativeRelationSourceAstKind::kCatalogRelation) {
        if (!source_relations[source_ordinal]->predicate_expression_ids.empty()) {
          AddBoundAstDiagnostic(&bound, "SB_MODEL_BINDING_INCOMPLETE_V1",
                                "bounded relational leg cannot own a predicate");
          return RefusedBoundAst(std::move(bound));
        }
        continue;
      }
      if (source.model_operation_expression_ids.size() != 1) {
        AddBoundAstDiagnostic(&bound, "SB_MODEL_BINDING_INCOMPLETE_V1",
                              "multimodel leg requires one operation root");
        return RefusedBoundAst(std::move(bound));
      }
      std::vector<std::uint32_t> pending{
          source.model_operation_expression_ids.front()};
      pending.insert(pending.end(),
                     source_relations[source_ordinal]
                         ->predicate_expression_ids.begin(),
                     source_relations[source_ordinal]
                         ->predicate_expression_ids.end());
      std::unordered_set<std::uint32_t> local;
      while (!pending.empty()) {
        const auto expression_id = pending.back();
        pending.pop_back();
        if (!local.insert(expression_id).second) continue;
        const auto expression = ast_expression_by_id.find(expression_id);
        if (expression == ast_expression_by_id.end() ||
            expression->second->expression_kind ==
                NativeExpressionAstKind::kWildcard) {
          AddBoundAstDiagnostic(&bound, "SB_MODEL_BINDING_INCOMPLETE_V1",
                                "multimodel operation closure is incomplete");
          return RefusedBoundAst(std::move(bound));
        }
        for (const auto child_id : expression->second->child_expression_ids) {
          if (child_id >= expression_id) {
            AddBoundAstDiagnostic(&bound, "SB_MODEL_BINDING_INCOMPLETE_V1",
                                  "multimodel operation closure is not acyclic");
            return RefusedBoundAst(std::move(bound));
          }
          pending.push_back(child_id);
        }
      }
      for (const auto expression_id : local) {
        const auto [owner, inserted] =
            expression_owner.emplace(expression_id, source_ordinal);
        if (!inserted && owner->second != source_ordinal) {
          AddBoundAstDiagnostic(&bound, "SB_MODEL_BINDING_INCOMPLETE_V1",
                                "multimodel operation closure crosses legs");
          return RefusedBoundAst(std::move(bound));
        }
        source_closure_ids[source_ordinal].push_back(expression_id);
      }
      std::ranges::sort(source_closure_ids[source_ordinal]);
    }
    if (std::ranges::any_of(ast.expressions, [&](const auto& expression) {
          return expression.expression_kind !=
                     NativeExpressionAstKind::kWildcard &&
                 !project_expression_ids.contains(expression.expression_id) &&
                 !filter_expression_ids.contains(expression.expression_id) &&
                 !expression_owner.contains(expression.expression_id);
        })) {
      AddBoundAstDiagnostic(&bound, "SB_MODEL_BINDING_INCOMPLETE_V1",
                            "multimodel AST contains an orphan expression");
      return RefusedBoundAst(std::move(bound));
    }
    const auto operation_closure_count = std::accumulate(
        source_closure_ids.begin(), source_closure_ids.end(), std::size_t{0},
        [](const auto count, const auto& ids) { return count + ids.size(); });
    std::vector<std::vector<const NativeOutputBindingInput*>>
        source_projection_bindings(source_count);
    for (const auto& output : context.outputs) {
      if (output.relation_id >= 1 && output.relation_id <= source_count) {
        source_projection_bindings[output.relation_id - 1].push_back(&output);
      }
    }
    for (auto& outputs : source_projection_bindings) {
      std::ranges::sort(outputs, {},
                        [](const auto* output) { return output->ordinal; });
    }
    const auto source_projection_count = std::accumulate(
        source_projection_bindings.begin(), source_projection_bindings.end(),
        std::size_t{0}, [](const auto count, const auto& outputs) {
          return count + outputs.size();
        });
    std::size_t expected_output_count = source_projection_count;
    std::size_t accumulated_width = source_projection_bindings.front().size();
    for (std::size_t source_ordinal = 1; source_ordinal < source_count;
         ++source_ordinal) {
      accumulated_width += source_projection_bindings[source_ordinal].size();
      expected_output_count += accumulated_width;
    }
    if (project_composition) {
      expected_output_count += project_identifiers.size();
    }
    if (filter_composition) {
      expected_output_count += source_projection_count;
    }
    if (context.expressions.size() !=
            source_projection_count + operation_closure_count +
                static_cast<std::size_t>(filter_composition) ||
        (filter_composition &&
         context.descriptors.size() != source_projection_count + 2) ||
        context.outputs.size() != expected_output_count) {
      AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                            "multimodel typed binding cardinality is incomplete");
      return RefusedBoundAst(std::move(bound));
    }

    const auto model_profile = [](const NativeRelationSourceAstKind kind)
        -> std::tuple<std::string_view, std::string_view,
                      std::string_view, std::string_view> {
      switch (kind) {
        case NativeRelationSourceAstKind::kDocument:
          return {"document", "DOCUMENT_FIND", "DOCUMENT_SOURCE",
                  "document_collection"};
        case NativeRelationSourceAstKind::kGraph:
          return {"graph", "GRAPH_MATCH", "GRAPH_MATCH", "graph"};
        case NativeRelationSourceAstKind::kKeyValue:
          return {"key_value", "KEY_VALUE_GET", "KV_KEY", "key_value"};
        case NativeRelationSourceAstKind::kTimeSeries:
          return {"time_series", "TIME_SERIES_RANGE_READ", "TIME_RANGE",
                  "time_series"};
        case NativeRelationSourceAstKind::kVector:
          return {"vector", "VECTOR_EXACT_SEARCH", "VECTOR_NEAREST",
                  "vector"};
        case NativeRelationSourceAstKind::kSearch:
          return {"search", "SEARCH_RANKED_QUERY", "SEARCH_MATCH", "search"};
        case NativeRelationSourceAstKind::kSpatial:
          return {"spatial", "SPATIAL_SOURCE", "SPATIAL_SOURCE",
                  "spatial_collection"};
        case NativeRelationSourceAstKind::kColumnar:
          return {"columnar", "COLUMNAR_SOURCE", "COLUMNAR_SOURCE",
                  "logical_relation"};
        default: return {"", "", "", ""};
      }
    };
    const auto semantic_for = [](const NativeRelationSourceAstKind kind) {
      switch (kind) {
        case NativeRelationSourceAstKind::kDocument:
          return std::string{"sblr.model-source.document.v1"};
        case NativeRelationSourceAstKind::kGraph:
          return std::string{"sblr.model-source.graph.v1"};
        case NativeRelationSourceAstKind::kKeyValue:
          return std::string{"sblr.model-source.key-value.v1"};
        case NativeRelationSourceAstKind::kTimeSeries:
          return std::string{"sblr.model-source.time-series.v1"};
        case NativeRelationSourceAstKind::kVector:
          return std::string{"sblr.model-source.vector.v1"};
        case NativeRelationSourceAstKind::kSearch:
          return std::string{"sblr.model-source.search.v1"};
        case NativeRelationSourceAstKind::kSpatial:
          return std::string{"sblr.model-source.spatial.v1"};
        case NativeRelationSourceAstKind::kColumnar:
          return std::string{"sblr.model-source.columnar.v1"};
        default: return std::string{"catalog.relation-source.v1"};
      }
    };

    std::size_t binding_offset = 0;
    std::vector<std::vector<std::uint32_t>> source_projection_ids;
    for (std::size_t source_ordinal = 0; source_ordinal < source_count;
         ++source_ordinal) {
      const auto& ast_source = ast.catalog_relation_sources[source_ordinal];
      const auto& relation_binding = context.catalog_relations[source_ordinal];
      const auto* ast_relation = source_relations[source_ordinal];
      const auto [family, logical_operation, attached_root, object_type] =
          model_profile(ast_source.source_kind);
      const bool model = ast_source.source_kind !=
                         NativeRelationSourceAstKind::kCatalogRelation;
      const bool accepted_relational_type =
          relation_binding.resolved_object_type == "relation" ||
          relation_binding.resolved_object_type == "table" ||
          relation_binding.resolved_object_type == "view" ||
          relation_binding.resolved_object_type == "materialized_view" ||
          relation_binding.resolved_object_type == "external_table" ||
          relation_binding.resolved_object_type == "foreign_table";
      if (ast_source.source_id != source_ordinal + 1 ||
          relation_binding.source_id != ast_source.source_id ||
          relation_binding.resolution_state !=
              NativeCatalogRelationResolutionState::kBound ||
          !IsNonNullCanonicalUuid(relation_binding.object_uuid) ||
          !IsNonNullCanonicalUuid(relation_binding.resolved_schema_uuid) ||
          relation_binding.catalog_generation_id == 0 ||
          relation_binding.security_epoch == 0 ||
          relation_binding.resource_epoch == 0 ||
          relation_binding.columns.empty() ||
          (model &&
           (ast_source.model_family_id != family ||
            ast_source.model_operation_id != logical_operation ||
            ast_source.model_operation_ids !=
                std::vector<std::string>{std::string(attached_root)} ||
            ast_source.model_operation_expression_ids.size() != 1 ||
            relation_binding.resolved_object_type != object_type ||
            (ast_source.source_kind == NativeRelationSourceAstKind::kSearch &&
             (!IsNonNullCanonicalUuid(context.search_analyzer_uuid) ||
              context.search_analyzer_generation == 0)))) ||
          (!model && !accepted_relational_type)) {
        AddBoundAstDiagnostic(&bound, "SB_MODEL_BINDING_INCOMPLETE_V1",
                              "multimodel source authority is incomplete");
        return RefusedBoundAst(std::move(bound));
      }

      BoundCatalogRelationSourceAstRecord bound_source;
      bound_source.source_id = ast_source.source_id;
      bound_source.source_kind = ast_source.source_kind;
      bound_source.resolution_state = relation_binding.resolution_state;
      bound_source.qualified_name = ast_source.qualified_name;
      bound_source.alias = ast_source.alias;
      bound_source.alias_is_explicit = ast_source.alias_is_explicit;
      bound_source.model_family_id = ast_source.model_family_id;
      bound_source.model_operation_id = ast_source.model_operation_id;
      bound_source.model_operation_ids = ast_source.model_operation_ids;
      bound_source.model_source_alias = ast_source.model_source_alias;
      bound_source.model_time_series_aggregate_id =
          ast_source.model_time_series_aggregate_id;
      bound_source.model_vector_result_alias =
          ast_source.model_vector_result_alias;
      bound_source.model_vector_metric_id = ast_source.model_vector_metric_id;
      bound_source.model_vector_top_k = ast_source.model_vector_top_k;
      bound_source.model_search_result_alias =
          ast_source.model_search_result_alias;
      bound_source.model_search_analyzer_name =
          ast_source.model_search_analyzer_name;
      bound_source.model_search_query_kind =
          ast_source.model_search_query_kind;
      bound_source.model_search_top_k = ast_source.model_search_top_k;
      if (ast_source.source_kind == NativeRelationSourceAstKind::kSearch) {
        bound_source.model_search_analyzer_uuid =
            context.search_analyzer_uuid;
        bound_source.model_search_analyzer_generation =
            context.search_analyzer_generation;
      }
      bound_source.model_spatial_predicate_id =
          ast_source.model_spatial_predicate_id;
      bound_source.model_spatial_top_k = ast_source.model_spatial_top_k;
      bound_source.model_graph_direction = ast_source.model_graph_direction;
      bound_source.model_graph_minimum_depth =
          ast_source.model_graph_minimum_depth;
      bound_source.model_graph_maximum_depth =
          ast_source.model_graph_maximum_depth;
      bound_source.model_graph_cycle_policy =
          ast_source.model_graph_cycle_policy;
      bound_source.model_comparison_operator =
          ast_source.model_comparison_operator;
      bound_source.model_wildcard_path = ast_source.model_wildcard_path;
      bound_source.qualified_name_range = ast_source.qualified_name_range;
      bound_source.range = ast_source.range;
      bound_source.object_uuid = relation_binding.object_uuid;
      bound_source.resolved_object_type = relation_binding.resolved_object_type;
      bound_source.resolved_schema_uuid = relation_binding.resolved_schema_uuid;
      bound_source.parent_object_uuid = relation_binding.parent_object_uuid;
      bound_source.catalog_generation_id = relation_binding.catalog_generation_id;
      bound_source.security_epoch = relation_binding.security_epoch;
      bound_source.resource_epoch = relation_binding.resource_epoch;

      BoundRelationAstRecord bound_source_relation;
      bound_source_relation.relation_id = ast_relation->relation_id;
      bound_source_relation.relation_kind = NativeRelationAstKind::kCatalogSource;
      bound_source_relation.semantic_variant_id = semantic_for(ast_source.source_kind);
      bound_source_relation.bound_object_uuid = relation_binding.object_uuid;
      std::vector<std::uint32_t> projection_ids;
      for (std::size_t column_ordinal = 0;
           column_ordinal < relation_binding.columns.size(); ++column_ordinal) {
        const auto& column = relation_binding.columns[column_ordinal];
        if (column.ordinal != column_ordinal || column.column_uuid.empty() ||
            !descriptor_by_id.contains(column.descriptor_id)) {
          AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-OUTPUT",
                                "multimodel persisted source descriptor is incomplete");
          return RefusedBoundAst(std::move(bound));
        }
        bound_source.columns.push_back(
            {column.ordinal, column.column_uuid, column.descriptor_id,
             column.canonical_name_key});
      }
      const auto& projection_bindings =
          source_projection_bindings[source_ordinal];
      const bool derived_projection =
          ast_source.source_kind == NativeRelationSourceAstKind::kVector ||
          ast_source.source_kind == NativeRelationSourceAstKind::kSearch;
      const std::vector<std::string_view> expected_derived_names =
          ast_source.source_kind == NativeRelationSourceAstKind::kVector
              ? std::vector<std::string_view>{"row_uuid", "distance", "score"}
          : ast_source.source_kind == NativeRelationSourceAstKind::kSearch
              ? std::vector<std::string_view>{
                    "document_uuid", "analyzer_uuid", "analyzer_generation",
                    "score", "rank"}
              : std::vector<std::string_view>{};
      if ((!derived_projection &&
           projection_bindings.size() != relation_binding.columns.size()) ||
          (derived_projection &&
           projection_bindings.size() != expected_derived_names.size())) {
        AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-OUTPUT",
                              "multimodel public source inventory is incomplete");
        return RefusedBoundAst(std::move(bound));
      }
      for (std::size_t column_ordinal = 0;
           column_ordinal < projection_bindings.size(); ++column_ordinal) {
        const auto& output = *projection_bindings[column_ordinal];
        const auto& expression = context.expressions[binding_offset];
        const auto expected_bound_name =
            derived_projection
                ? std::optional<std::string>{
                      ast_source.source_kind ==
                                  NativeRelationSourceAstKind::kSearch &&
                              (column_ordinal == 1 || column_ordinal == 2)
                          ? context.search_analyzer_uuid
                          : relation_binding.object_uuid}
                : std::optional<std::string>{
                      relation_binding.columns[column_ordinal].column_uuid};
        if (expression.expression_id != binding_offset + 1 ||
            !descriptor_by_id.contains(expression.descriptor_id) ||
            expression.function_uuid.has_value() ||
            expression.bound_name_uuid != expected_bound_name ||
            output.output_id != binding_offset + 1 ||
            output.expression_id != expression.expression_id ||
            output.descriptor_id != expression.descriptor_id || !output.visible ||
            output.ordinal != column_ordinal ||
            output.relation_id != ast_relation->relation_id ||
            (derived_projection &&
             output.output_name_utf8 != expected_derived_names[column_ordinal]) ||
            (!derived_projection &&
             (expression.descriptor_id !=
                  relation_binding.columns[column_ordinal].descriptor_id ||
              output.output_name_utf8 !=
                  relation_binding.columns[column_ordinal].canonical_name_key))) {
          AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-OUTPUT",
                                "multimodel source projection is incomplete");
          return RefusedBoundAst(std::move(bound));
        }
        bound.expressions.push_back(
            {expression.expression_id, NativeExpressionAstKind::kIdentifier,
             std::nullopt, {}, expression.descriptor_id, std::nullopt,
             expression.bound_name_uuid, std::nullopt, std::nullopt});
        bound.outputs.push_back(
            {output.output_id, output.relation_id, output.expression_id,
             output.output_name_utf8, output.descriptor_id, output.visible,
             output.ordinal});
        projection_ids.push_back(expression.expression_id);
        bound_source_relation.output_expression_ids.push_back(
            expression.expression_id);
        bound_source_relation.bound_expression_ids.push_back(
            expression.expression_id);
        ++binding_offset;
      }
      source_projection_ids.push_back(projection_ids);
      bound.catalog_relation_sources.push_back(std::move(bound_source));
      bound.relations.push_back(std::move(bound_source_relation));
    }

    std::unordered_map<std::uint32_t, std::uint32_t> ast_to_bound_expression;
    for (std::size_t source_ordinal = 0; source_ordinal < source_count;
         ++source_ordinal) {
      const auto& ast_source = ast.catalog_relation_sources[source_ordinal];
      if (ast_source.source_kind ==
          NativeRelationSourceAstKind::kCatalogRelation) {
        continue;
      }
      const auto [family, logical_operation, attached_root, object_type] =
          model_profile(ast_source.source_kind);
      const auto expected_arity =
          attached_root == "DOCUMENT_SOURCE" ? 1U
          : attached_root == "GRAPH_MATCH"   ? 2U
          : attached_root == "KV_KEY"        ? 1U
          : attached_root == "TIME_RANGE"    ? 3U
          : attached_root == "VECTOR_NEAREST" ||
                    attached_root == "SEARCH_MATCH"
              ? 4U
              : 0U;
      const auto primary_ast_id =
          ast_source.model_operation_expression_ids.front();
      const auto primary = ast_expression_by_id.find(primary_ast_id);
      if (primary == ast_expression_by_id.end() ||
          primary->second->expression_kind !=
              NativeExpressionAstKind::kFunctionCall ||
          primary->second->operator_name != attached_root ||
          primary->second->child_expression_ids.size() != expected_arity) {
        AddBoundAstDiagnostic(&bound, "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                              "multimodel operation identity or arity is invalid");
        return RefusedBoundAst(std::move(bound));
      }

      std::unordered_set<std::uint32_t> alias_expression_ids;
      if (expected_arity != 0) {
        alias_expression_ids.insert(
            primary->second->child_expression_ids.front());
      }
      for (const auto ast_expression_id : source_closure_ids[source_ordinal]) {
        const auto ast_expression = ast_expression_by_id.find(ast_expression_id);
        const auto& expression = context.expressions[binding_offset];
        if (ast_expression == ast_expression_by_id.end() ||
            expression.expression_id != binding_offset + 1 ||
            !descriptor_by_id.contains(expression.descriptor_id) ||
            expression.function_uuid.has_value()) {
          AddBoundAstDiagnostic(&bound, "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                                "multimodel operation binding is incomplete");
          return RefusedBoundAst(std::move(bound));
        }
        std::optional<std::string> expected_bound_name;
        if (alias_expression_ids.contains(ast_expression_id)) {
          expected_bound_name =
              context.catalog_relations[source_ordinal].object_uuid;
        } else if (ast_source.model_search_analyzer_expression_id ==
                   ast_expression_id) {
          expected_bound_name = context.search_analyzer_uuid;
        } else if (ast_expression_id == primary_ast_id &&
                   (ast_source.source_kind ==
                        NativeRelationSourceAstKind::kSpatial ||
                    ast_source.source_kind ==
                        NativeRelationSourceAstKind::kColumnar)) {
          expected_bound_name =
              context.catalog_relations[source_ordinal].object_uuid;
        }
        if (expression.bound_name_uuid != expected_bound_name) {
          AddBoundAstDiagnostic(&bound, "SB_MODEL_BINDING_INCOMPLETE_V1",
                                "multimodel operation bound-name is invalid");
          return RefusedBoundAst(std::move(bound));
        }
        BoundExpressionAstRecord operation;
        operation.expression_id = expression.expression_id;
        operation.expression_kind = ast_expression->second->expression_kind;
        operation.literal_kind = ast_expression->second->literal_kind;
        operation.result_descriptor_id = expression.descriptor_id;
        operation.bound_name_uuid = expression.bound_name_uuid;
        for (const auto child_ast_id :
             ast_expression->second->child_expression_ids) {
          const auto child = ast_to_bound_expression.find(child_ast_id);
          if (child == ast_to_bound_expression.end()) {
            AddBoundAstDiagnostic(&bound, "SB_MODEL_BINDING_INCOMPLETE_V1",
                                  "multimodel child binding is missing");
            return RefusedBoundAst(std::move(bound));
          }
          operation.child_expression_ids.push_back(child->second);
        }
        if (operation.expression_kind ==
                NativeExpressionAstKind::kFunctionCall ||
            operation.expression_kind == NativeExpressionAstKind::kBinary) {
          operation.canonical_operator_name =
              ast_expression->second->operator_name;
        } else if (operation.expression_kind ==
                       NativeExpressionAstKind::kLiteral ||
                   operation.expression_kind ==
                       NativeExpressionAstKind::kParameter) {
          operation.literal_or_parameter_ref = ast_expression->second->spelling;
        }
        if (operation.expression_kind ==
                NativeExpressionAstKind::kFunctionCall &&
            ast_expression_id != primary_ast_id &&
            !(ast_source.source_kind == NativeRelationSourceAstKind::kSearch &&
              operation.canonical_operator_name == "SEARCH_TERMS" &&
              operation.child_expression_ids.size() == 1)) {
          AddBoundAstDiagnostic(&bound, "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                                "multimodel auxiliary operation is invalid");
          return RefusedBoundAst(std::move(bound));
        }
        ast_to_bound_expression.emplace(ast_expression_id,
                                        expression.expression_id);
        bound.expressions.push_back(std::move(operation));
        bound.relations[source_ordinal].bound_expression_ids.push_back(
            expression.expression_id);
        ++binding_offset;
      }

      auto& bound_source = bound.catalog_relation_sources[source_ordinal];
      const auto map_required = [&](const std::uint32_t id)
          -> std::optional<std::uint32_t> {
        const auto mapped = ast_to_bound_expression.find(id);
        return mapped == ast_to_bound_expression.end()
                   ? std::nullopt
                   : std::optional<std::uint32_t>{mapped->second};
      };
      const auto map_optional = [&](const std::optional<std::uint32_t>& id) {
        return id.has_value() ? map_required(*id) : std::optional<std::uint32_t>{};
      };
      const auto map_vector = [&](const std::vector<std::uint32_t>& ids) {
        std::vector<std::uint32_t> mapped;
        for (const auto id : ids) {
          const auto value = map_required(id);
          if (!value.has_value()) return std::vector<std::uint32_t>{};
          mapped.push_back(*value);
        }
        return mapped;
      };
      const auto mapped_primary = map_required(primary_ast_id);
      if (!mapped_primary.has_value()) {
        AddBoundAstDiagnostic(&bound, "SB_MODEL_BINDING_INCOMPLETE_V1",
                              "multimodel primary operation is unbound");
        return RefusedBoundAst(std::move(bound));
      }
      bound_source.model_operation_expression_ids = {*mapped_primary};
      bound_source.model_document_expression_id =
          map_optional(ast_source.model_document_expression_id);
      bound_source.model_path_expression_id =
          map_optional(ast_source.model_path_expression_id);
      bound_source.model_value_expression_id =
          map_optional(ast_source.model_value_expression_id);
      bound_source.model_pattern_expression_id =
          map_optional(ast_source.model_pattern_expression_id);
      bound_source.model_graph_alias_expression_id =
          map_optional(ast_source.model_graph_alias_expression_id);
      bound_source.model_key_expression_ids =
          map_vector(ast_source.model_key_expression_ids);
      bound_source.model_time_series_alias_expression_id =
          map_optional(ast_source.model_time_series_alias_expression_id);
      bound_source.model_range_expression_id =
          map_optional(ast_source.model_range_expression_id);
      bound_source.model_range_start_expression_id =
          map_optional(ast_source.model_range_start_expression_id);
      bound_source.model_range_end_expression_id =
          map_optional(ast_source.model_range_end_expression_id);
      bound_source.model_vector_alias_expression_id =
          map_optional(ast_source.model_vector_alias_expression_id);
      bound_source.model_vector_nearest_expression_id =
          map_optional(ast_source.model_vector_nearest_expression_id);
      bound_source.model_vector_query_expression_id =
          map_optional(ast_source.model_vector_query_expression_id);
      bound_source.model_vector_metric_expression_id =
          map_optional(ast_source.model_vector_metric_expression_id);
      bound_source.model_vector_top_k_expression_id =
          map_optional(ast_source.model_vector_top_k_expression_id);
      bound_source.model_search_alias_expression_id =
          map_optional(ast_source.model_search_alias_expression_id);
      bound_source.model_search_match_expression_id =
          map_optional(ast_source.model_search_match_expression_id);
      bound_source.model_search_query_expression_id =
          map_optional(ast_source.model_search_query_expression_id);
      bound_source.model_search_text_expression_id =
          map_optional(ast_source.model_search_text_expression_id);
      bound_source.model_search_analyzer_expression_id =
          map_optional(ast_source.model_search_analyzer_expression_id);
      bound_source.model_search_top_k_expression_id =
          map_optional(ast_source.model_search_top_k_expression_id);
      bound_source.model_spatial_operation_expression_id =
          map_optional(ast_source.model_spatial_operation_expression_id);
      bound_source.model_columnar_operation_expression_id =
          map_optional(ast_source.model_columnar_operation_expression_id);
      if (ast_source.source_kind == NativeRelationSourceAstKind::kKeyValue) {
        const auto& predicates =
            source_relations[source_ordinal]->predicate_expression_ids;
        if (predicates.size() != 1) {
          AddBoundAstDiagnostic(&bound, "SB_MODEL_BINDING_INCOMPLETE_V1",
                                "multimodel KV predicate is missing");
          return RefusedBoundAst(std::move(bound));
        }
        const auto mapped = map_required(predicates.front());
        if (!mapped.has_value()) return RefusedBoundAst(std::move(bound));
        bound.relations[source_ordinal].predicate_expression_ids = {*mapped};
      } else if (source_relations[source_ordinal]
                     ->predicate_expression_ids.size() == 1) {
        const auto mapped = map_required(
            source_relations[source_ordinal]->predicate_expression_ids.front());
        if (!mapped.has_value() || *mapped_primary != *mapped) {
          AddBoundAstDiagnostic(&bound, "SB_MODEL_BINDING_INCOMPLETE_V1",
                                "multimodel source predicate is invalid");
          return RefusedBoundAst(std::move(bound));
        }
        bound.relations[source_ordinal].predicate_expression_ids = {*mapped};
      } else if (!source_relations[source_ordinal]
                      ->predicate_expression_ids.empty()) {
        AddBoundAstDiagnostic(&bound, "SB_MODEL_BINDING_INCOMPLETE_V1",
                              "multimodel source predicate is ambiguous");
        return RefusedBoundAst(std::move(bound));
      }
    }

    std::vector<std::uint32_t> accumulated_projection_ids;
    std::size_t output_offset = source_projection_count;
    std::vector<std::uint32_t> root_output_ids;
    for (std::size_t source_ordinal = 0; source_ordinal < source_count;
         ++source_ordinal) {
      accumulated_projection_ids.insert(
          accumulated_projection_ids.end(),
          source_projection_ids[source_ordinal].begin(),
          source_projection_ids[source_ordinal].end());
      if (source_ordinal == 0) continue;
      const auto* ast_join = join_relations[source_ordinal - 1];
      BoundRelationAstRecord bound_join;
      bound_join.relation_id = ast_join->relation_id;
      bound_join.relation_kind = NativeRelationAstKind::kJoin;
      bound_join.input_relation_ids = ast_join->input_relation_ids;
      bound_join.output_expression_ids = accumulated_projection_ids;
      bound_join.semantic_variant_id = "join.cross.v1";
      root_output_ids.clear();
      for (std::size_t ordinal = 0;
           ordinal < accumulated_projection_ids.size(); ++ordinal) {
        const auto& output = context.outputs[output_offset++];
        const auto expression_id = accumulated_projection_ids[ordinal];
        const auto expression = std::ranges::find_if(
            bound.expressions, [&](const auto& candidate) {
              return candidate.expression_id == expression_id;
            });
        if (expression == bound.expressions.end() ||
            output.output_id != output_offset ||
            output.relation_id != ast_join->relation_id ||
            output.expression_id != expression_id ||
            output.descriptor_id != expression->result_descriptor_id ||
            !output.visible || output.ordinal != ordinal) {
          AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-OUTPUT",
                                "multimodel JOIN projection is incomplete");
          return RefusedBoundAst(std::move(bound));
        }
        bound.outputs.push_back(
            {output.output_id, output.relation_id, output.expression_id,
             output.output_name_utf8, output.descriptor_id, output.visible,
             output.ordinal});
        root_output_ids.push_back(output.output_id);
      }
      bound.relations.push_back(std::move(bound_join));
    }
    if (filter_composition) {
      if (source_projection_count >
          std::numeric_limits<std::uint32_t>::max() - 2U) {
        AddBoundAstDiagnostic(
            &bound, "RESOURCE.BUDGET_EXCEEDED",
            "ordinary multi-source FILTER descriptor identity overflowed");
        return RefusedBoundAst(std::move(bound));
      }
      const auto filter_name =
          filter_identifier->qualified_identifier.front().quoted
              ? filter_identifier->qualified_identifier.front().spelling
              : ToLowerAscii(
                    filter_identifier->qualified_identifier.front().spelling);
      const BoundCatalogColumnAstRecord* selected_column = nullptr;
      std::uint32_t selected_expression_id = 0;
      std::size_t matching_column_count = 0;
      for (std::size_t source_ordinal = 0; source_ordinal < source_count;
           ++source_ordinal) {
        const auto& source = bound.catalog_relation_sources[source_ordinal];
        const auto& source_relation = bound.relations[source_ordinal];
        for (std::size_t ordinal = 0; ordinal < source.columns.size(); ++ordinal) {
          if (source.columns[ordinal].canonical_name_key != filter_name) continue;
          ++matching_column_count;
          selected_column = &source.columns[ordinal];
          selected_expression_id =
              source_relation.output_expression_ids[ordinal];
        }
      }

      const bool literal_value =
          filter_value->expression_kind == NativeExpressionAstKind::kLiteral;
      const bool parameter_value =
          filter_value->expression_kind == NativeExpressionAstKind::kParameter;
      const auto occurrence_matches = [&](const auto& expression) {
        return literal_value
                   ? expression.structural_literal_occurrence_id ==
                         filter_value->structural_literal_occurrence_id
               : parameter_value
                   ? expression.structural_parameter_occurrence_id ==
                         filter_value->structural_parameter_occurrence_id
                   : expression.structural_variable_occurrence_id ==
                         filter_value->structural_variable_occurrence_id;
      };
      const auto matching_operand_count =
          std::ranges::count_if(context.expressions, occurrence_matches);
      const auto operand_binding =
          std::ranges::find_if(context.expressions, occurrence_matches);
      const auto boolean_descriptor_id =
          static_cast<std::uint32_t>(source_projection_count + 1);
      const auto operand_descriptor_id =
          static_cast<std::uint32_t>(source_projection_count + 2);
      const auto boolean_descriptor =
          descriptor_by_id.find(boolean_descriptor_id);
      const auto operand_descriptor =
          descriptor_by_id.find(operand_descriptor_id);
      const auto selected_descriptor =
          selected_column == nullptr
              ? descriptor_by_id.end()
              : descriptor_by_id.find(selected_column->descriptor_id);
      const bool exact_operand_occurrence =
          operand_binding != context.expressions.end() &&
          (literal_value
               ? operand_binding->structural_literal_occurrence_id ==
                         filter_value->structural_literal_occurrence_id &&
                     operand_binding->structural_parameter_occurrence_id == 0 &&
                     operand_binding->structural_variable_occurrence_id == 0
           : parameter_value
               ? operand_binding->structural_literal_occurrence_id == 0 &&
                     operand_binding->structural_parameter_occurrence_id ==
                         filter_value->structural_parameter_occurrence_id &&
                     operand_binding->structural_variable_occurrence_id == 0
               : operand_binding->structural_literal_occurrence_id == 0 &&
                     operand_binding->structural_parameter_occurrence_id == 0 &&
                     operand_binding->structural_variable_occurrence_id ==
                         filter_value->structural_variable_occurrence_id);
      if (matching_column_count != 1 || selected_column == nullptr ||
          selected_expression_id == 0 || matching_operand_count != 1 ||
          operand_binding == context.expressions.end() ||
          !exact_operand_occurrence ||
          operand_binding->expression_id != boolean_descriptor_id ||
          operand_binding->descriptor_id != operand_descriptor_id ||
          operand_binding->function_uuid.has_value() ||
          operand_binding->bound_name_uuid.has_value() ||
          boolean_descriptor == descriptor_by_id.end() ||
          boolean_descriptor->second->nullability !=
              BoundNullability::kNullable ||
          boolean_descriptor->second->collation_uuid.has_value() ||
          boolean_descriptor->second->timezone_profile_id.has_value() ||
          operand_descriptor == descriptor_by_id.end() ||
          selected_descriptor == descriptor_by_id.end() ||
          operand_descriptor->second->type_uuid !=
              selected_descriptor->second->type_uuid ||
          operand_descriptor->second->collation_uuid.has_value() ||
          operand_descriptor->second->timezone_profile_id.has_value() ||
          selected_descriptor->second->collation_uuid.has_value() ||
          selected_descriptor->second->timezone_profile_id.has_value() ||
          (literal_value && operand_descriptor->second->nullability !=
                                BoundNullability::kNonNull) ||
          binding_offset != source_projection_count) {
        AddBoundAstDiagnostic(
            &bound, "QOW-DIAG-BOUNDAST-EXPRESSION",
            "ordinary multi-source FILTER value binding is incomplete");
        return RefusedBoundAst(std::move(bound));
      }

      BoundExpressionAstRecord operand;
      operand.expression_id = operand_binding->expression_id;
      operand.expression_kind = filter_value->expression_kind;
      operand.literal_kind = filter_value->literal_kind;
      operand.result_descriptor_id = operand_binding->descriptor_id;
      if (literal_value) {
        operand.literal_or_parameter_ref = filter_value->spelling;
      }
      operand.structural_literal_occurrence_id =
          filter_value->structural_literal_occurrence_id;
      operand.structural_parameter_occurrence_id =
          filter_value->structural_parameter_occurrence_id;
      operand.structural_variable_occurrence_id =
          filter_value->structural_variable_occurrence_id;
      const auto operand_expression_id = operand.expression_id;
      bound.expressions.push_back(std::move(operand));
      ++binding_offset;
      if (bound.expressions.size() >=
          std::numeric_limits<std::uint32_t>::max()) {
        AddBoundAstDiagnostic(
            &bound, "RESOURCE.BUDGET_EXCEEDED",
            "ordinary multi-source FILTER expression identity overflowed");
        return RefusedBoundAst(std::move(bound));
      }
      BoundExpressionAstRecord predicate;
      predicate.expression_id =
          static_cast<std::uint32_t>(bound.expressions.size() + 1);
      predicate.expression_kind = NativeExpressionAstKind::kBinary;
      predicate.result_descriptor_id = boolean_descriptor_id;
      predicate.child_expression_ids = {selected_expression_id,
                                        operand_expression_id};
      predicate.canonical_operator_name = filter_predicate->operator_name;
      const auto predicate_expression_id = predicate.expression_id;
      bound.expressions.push_back(std::move(predicate));

      std::vector<const NativeOutputBindingInput*> predecessor_outputs;
      for (const auto& output : context.outputs) {
        if (output.relation_id == final_join_relation_id) {
          predecessor_outputs.push_back(&output);
        }
      }
      std::ranges::sort(predecessor_outputs, {}, [](const auto* output) {
        return output->ordinal;
      });
      if (predecessor_outputs.size() != source_projection_count) {
        AddBoundAstDiagnostic(
            &bound, "QOW-DIAG-BOUNDAST-OUTPUT",
            "ordinary multi-source FILTER input width is incomplete");
        return RefusedBoundAst(std::move(bound));
      }
      root_output_ids.clear();
      for (std::size_t ordinal = 0; ordinal < predecessor_outputs.size();
           ++ordinal) {
        if (output_offset >= context.outputs.size()) {
          AddBoundAstDiagnostic(
              &bound, "QOW-DIAG-BOUNDAST-OUTPUT",
              "ordinary multi-source FILTER output is missing");
          return RefusedBoundAst(std::move(bound));
        }
        const auto& source = *predecessor_outputs[ordinal];
        const auto& output = context.outputs[output_offset++];
        if (output.output_id != output_offset ||
            output.relation_id != filter_relation->relation_id ||
            output.expression_id != source.expression_id ||
            output.output_name_utf8 != source.output_name_utf8 ||
            output.descriptor_id != source.descriptor_id || !output.visible ||
            output.ordinal != ordinal) {
          AddBoundAstDiagnostic(
              &bound, "QOW-DIAG-BOUNDAST-OUTPUT",
              "ordinary multi-source FILTER output does not preserve JOIN lineage");
          return RefusedBoundAst(std::move(bound));
        }
        bound.outputs.push_back(
            {output.output_id, output.relation_id, output.expression_id,
             output.output_name_utf8, output.descriptor_id, output.visible,
             output.ordinal});
        root_output_ids.push_back(output.output_id);
      }

      BoundRelationAstRecord bound_filter;
      bound_filter.relation_id = filter_relation->relation_id;
      bound_filter.relation_kind = NativeRelationAstKind::kFilter;
      bound_filter.input_relation_ids = {final_join_relation_id};
      bound_filter.output_expression_ids = accumulated_projection_ids;
      bound_filter.predicate_expression_ids = {predicate_expression_id};
      bound_filter.bound_expression_ids = {predicate_expression_id};
      bound_filter.semantic_variant_id =
          "filter.catalog-column-numeric-comparison.v1";
      bound.relations.push_back(std::move(bound_filter));
    }
    if (project_composition) {
      std::vector<const NativeOutputBindingInput*> predecessor_outputs;
      for (const auto& output : context.outputs) {
        if (output.relation_id == final_join_relation_id) {
          predecessor_outputs.push_back(&output);
        }
      }
      std::ranges::sort(predecessor_outputs, {}, [](const auto* output) {
        return output->ordinal;
      });
      if (project_identifiers.empty() || predecessor_outputs.empty() ||
          project_identifiers.size() >= predecessor_outputs.size() ||
          predecessor_outputs.size() != accumulated_projection_ids.size()) {
        AddBoundAstDiagnostic(
            &bound, "QOW-DIAG-BOUNDAST-OUTPUT",
            "ordinary multi-source PROJECT width is not a strict subset");
        return RefusedBoundAst(std::move(bound));
      }

      std::unordered_set<std::uint32_t> selected_predecessor_output_ids;
      std::vector<std::uint32_t> projected_expression_ids;
      root_output_ids.clear();
      for (std::size_t ordinal = 0; ordinal < project_identifiers.size();
           ++ordinal) {
        const auto* identifier = project_identifiers[ordinal];
        const auto identifier_key =
            identifier->qualified_identifier.front().quoted
                ? identifier->qualified_identifier.front().spelling
                : ToLowerAscii(
                      identifier->qualified_identifier.front().spelling);
        const auto matching_count = std::ranges::count_if(
            predecessor_outputs, [&](const auto* output) {
              return output->output_name_utf8 == identifier_key;
            });
        const auto selected = std::ranges::find_if(
            predecessor_outputs, [&](const auto* output) {
              return output->output_name_utf8 == identifier_key;
            });
        if (output_offset >= context.outputs.size()) {
          AddBoundAstDiagnostic(
              &bound, "QOW-DIAG-BOUNDAST-OUTPUT",
              "ordinary multi-source PROJECT output is missing");
          return RefusedBoundAst(std::move(bound));
        }
        const auto& output = context.outputs[output_offset++];
        const auto expected_output_id =
            static_cast<std::uint32_t>(output_offset);
        if (matching_count != 1 || selected == predecessor_outputs.end() ||
            !selected_predecessor_output_ids.insert((*selected)->output_id)
                 .second ||
            output.output_id != expected_output_id ||
            output.relation_id != project_relation->relation_id ||
            output.expression_id != (*selected)->expression_id ||
            output.output_name_utf8 != (*selected)->output_name_utf8 ||
            output.descriptor_id != (*selected)->descriptor_id ||
            !output.visible || output.ordinal != ordinal) {
          AddBoundAstDiagnostic(
              &bound, "QOW-DIAG-BOUNDAST-OUTPUT",
              "ordinary multi-source PROJECT output is unresolved or ambiguous");
          return RefusedBoundAst(std::move(bound));
        }
        bound.outputs.push_back(
            {output.output_id, output.relation_id, output.expression_id,
             output.output_name_utf8, output.descriptor_id, output.visible,
             output.ordinal});
        projected_expression_ids.push_back(output.expression_id);
        root_output_ids.push_back(output.output_id);
      }

      BoundRelationAstRecord bound_project;
      bound_project.relation_id = project_relation->relation_id;
      bound_project.relation_kind = NativeRelationAstKind::kProject;
      bound_project.input_relation_ids = {final_join_relation_id};
      bound_project.output_expression_ids = projected_expression_ids;
      bound_project.bound_expression_ids = projected_expression_ids;
      bound_project.semantic_variant_id =
          "project.catalog-visible-columns.v1";
      bound.relations.push_back(std::move(bound_project));
    }
    if (binding_offset != context.expressions.size() ||
        output_offset != context.outputs.size()) {
      AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                            "multimodel binding left unconsumed typed records");
      return RefusedBoundAst(std::move(bound));
    }
    for (const auto& descriptor : context.descriptors) {
      bound.descriptors.push_back(
          {descriptor.descriptor_id, descriptor.descriptor_uuid,
           descriptor.type_uuid, descriptor.nullability,
           descriptor.collation_uuid, descriptor.timezone_profile_id,
           descriptor.width_precision_scale, descriptor.canonical_type_name,
           descriptor.element_profile});
    }
    std::ranges::sort(bound.descriptors, {},
                      &BoundDescriptorAstRecord::descriptor_id);
    bound.scopes.push_back({1, std::nullopt, {ast.root_relation_id},
                            std::move(root_output_ids),
                            context.catalog_epoch_uuid});
    bound.bound_ast_uuid = context.bound_ast_uuid;
    bound.security_context_uuid = context.security_context_uuid;
    bound.root_relation_id = ast.root_relation_id;
    bound.root_scope_id = 1;
    bound.bound = true;
    return bound;
  }
  if ((spatial_source_ast != ast.catalog_relation_sources.end() ||
       columnar_source_ast != ast.catalog_relation_sources.end()) &&
      ast.catalog_relation_sources.size() == 1) {
    // QOW-SOURCE-RCP-079-SPATIAL-COLUMNAR-BINDING-V1
    const bool spatial =
        spatial_source_ast != ast.catalog_relation_sources.end();
    const auto& source = spatial ? *spatial_source_ast : *columnar_source_ast;
    const auto refuse_model = [&](const char* diagnostic,
                                  const char* detail) {
      AddBoundAstDiagnostic(&bound, diagnostic, detail);
      return RefusedBoundAst(std::move(bound));
    };
    const auto exact_operations = [&]() {
      if (spatial) {
        return source.model_operation_ids ==
                   std::vector<std::string>{"SPATIAL_SOURCE"} ||
               source.model_operation_ids ==
                   std::vector<std::string>{"SPATIAL_SOURCE",
                                            "SPATIAL_MATCH"} ||
               source.model_operation_ids ==
                   std::vector<std::string>{"SPATIAL_SOURCE",
                                            "SPATIAL_NEAREST"} ||
               source.model_operation_ids ==
                   std::vector<std::string>{"SPATIAL_SOURCE", "SPATIAL_MATCH",
                                            "SPATIAL_NEAREST"};
      }
      return source.model_operation_ids ==
                 std::vector<std::string>{"COLUMNAR_SOURCE"} ||
             source.model_operation_ids ==
                 std::vector<std::string>{"COLUMNAR_SOURCE",
                                          "COLUMNAR_FILTER"} ||
             source.model_operation_ids ==
                 std::vector<std::string>{"COLUMNAR_SOURCE",
                                          "COLUMNAR_PROJECT"} ||
             source.model_operation_ids ==
                 std::vector<std::string>{"COLUMNAR_SOURCE",
                                          "COLUMNAR_FILTER",
                                          "COLUMNAR_PROJECT"};
    }();
    const bool has_match = std::ranges::find(source.model_operation_ids,
                                             "SPATIAL_MATCH") !=
                           source.model_operation_ids.end();
    const bool has_nearest = std::ranges::find(source.model_operation_ids,
                                               "SPATIAL_NEAREST") !=
                             source.model_operation_ids.end();
    const bool has_filter = std::ranges::find(source.model_operation_ids,
                                              "COLUMNAR_FILTER") !=
                            source.model_operation_ids.end();
    const bool has_project = std::ranges::find(source.model_operation_ids,
                                               "COLUMNAR_PROJECT") !=
                             source.model_operation_ids.end();
    const std::string expected_semantic =
        spatial ? "sblr.model-source.spatial.v1"
                : "sblr.model-source.columnar.v1";
    if (ast.catalog_relation_sources.size() != 1 || ast.relations.size() != 1 ||
        ast.root_relation_id != ast.relations.front().relation_id ||
        ast.relations.front().relation_kind !=
            NativeRelationAstKind::kCatalogSource ||
        ast.relations.front().relation_source_ids !=
            std::vector<std::uint32_t>{source.source_id} ||
        source.model_family_id != (spatial ? "spatial" : "columnar") ||
        !exact_operations || source.model_operation_ids.size() !=
                                 source.model_operation_expression_ids.size() ||
        source.qualified_name.empty() || !source.alias.has_value() ||
        context.catalog_relations.size() != 1 || context.relations.size() != 1 ||
        context.relations.front().relation_id !=
            ast.relations.front().relation_id ||
        context.relations.front().semantic_variant_id != expected_semantic) {
      return refuse_model("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                          "spatial/columnar AST or binding cohort is incomplete");
    }
    const auto& resolution = context.catalog_relations.front();
    const auto expected_object_type =
        spatial ? std::string_view{"spatial_collection"}
                : std::string_view{"logical_relation"};
    if (resolution.source_id != source.source_id ||
        resolution.resolution_state !=
            NativeCatalogRelationResolutionState::kBound ||
        resolution.resolved_object_type != expected_object_type ||
        !IsNonNullCanonicalUuid(resolution.object_uuid) ||
        !IsNonNullCanonicalUuid(resolution.resolved_schema_uuid) ||
        resolution.catalog_generation_id == 0 || resolution.security_epoch == 0 ||
        resolution.resource_epoch == 0 || resolution.columns.empty() ||
        resolution.columns.size() > 256) {
      return refuse_model("SB_MODEL_BINDING_INCOMPLETE_V1",
                          "spatial/columnar catalog binding is incomplete");
    }
    const auto same_presented_name = [](const auto& left,
                                        const auto& right) {
      return left.size() == right.size() &&
             std::ranges::equal(left, right, [](const auto& lhs,
                                                const auto& rhs) {
               return lhs.spelling == rhs.spelling &&
                      lhs.quoted == rhs.quoted;
             });
    };
    const auto expected_request_count =
        std::size_t{1} + (spatial ? source.model_spatial_crs_names.size() : 0);
    if (ast.model_object_resolution_requests.size() != expected_request_count ||
        ast.model_object_resolution_requests.front().source_id !=
            source.source_id ||
        ast.model_object_resolution_requests.front().model_family_id !=
            source.model_family_id ||
        ast.model_object_resolution_requests.front().object_class !=
            expected_object_type ||
        !same_presented_name(
            ast.model_object_resolution_requests.front().qualified_name,
            source.qualified_name)) {
      return refuse_model("SB_MODEL_BINDING_INCOMPLETE_V1",
                          "spatial/columnar resolution description was substituted");
    }
    std::unordered_map<std::string, const NativeCatalogColumnBindingInput*>
        column_by_name;
    std::unordered_set<std::string> column_uuids;
    for (std::size_t ordinal = 0; ordinal < resolution.columns.size(); ++ordinal) {
      const auto& column = resolution.columns[ordinal];
      if (column.ordinal != ordinal ||
          !IsNonNullCanonicalUuid(column.column_uuid) ||
          !descriptor_by_id.contains(column.descriptor_id) ||
          column.canonical_name_key.empty() ||
          !column_by_name.emplace(column.canonical_name_key, &column).second ||
          !column_uuids.insert(column.column_uuid).second) {
        return refuse_model("SB_MODEL_BINDING_INCOMPLETE_V1",
                            "spatial/columnar catalog column vector is invalid");
      }
    }
    if (spatial) {
      if (resolution.columns.size() != 3 ||
          resolution.columns[0].canonical_name_key != "row_uuid" ||
          resolution.columns[1].canonical_name_key != "spatial_value" ||
          resolution.columns[2].canonical_name_key != "crs_uuid" ||
          descriptor_by_id.at(resolution.columns[0].descriptor_id)
                  ->canonical_type_name != "uuid" ||
          descriptor_by_id.at(resolution.columns[1].descriptor_id)
                  ->canonical_type_name != "geometry" ||
          descriptor_by_id.at(resolution.columns[2].descriptor_id)
                  ->canonical_type_name != "uuid" ||
          descriptor_by_id.at(resolution.columns[0].descriptor_id)->nullability !=
              BoundNullability::kNonNull ||
          descriptor_by_id.at(resolution.columns[1].descriptor_id)->nullability !=
              BoundNullability::kNonNull ||
          descriptor_by_id.at(resolution.columns[2].descriptor_id)->nullability !=
              BoundNullability::kNonNull ||
          ((has_match || has_nearest) &&
           (!resolution.spatial_crs_uuid.has_value() ||
            !IsNonNullCanonicalUuid(*resolution.spatial_crs_uuid) ||
            resolution.spatial_crs_generation == 0)) ||
          (!(has_match || has_nearest) &&
           (resolution.spatial_crs_uuid.has_value() ||
            resolution.spatial_crs_generation != 0)) ||
          context.spatial_crs_bindings.size() !=
              source.model_spatial_crs_names.size()) {
        return refuse_model("SB_MODEL_SPATIAL_PROFILE_UNSUPPORTED_V1",
                            "spatial source descriptor profile is incomplete");
      }
      for (std::size_t index = 0; index < context.spatial_crs_bindings.size();
           ++index) {
        const auto& request = ast.model_object_resolution_requests[index + 1];
        const auto& crs = context.spatial_crs_bindings[index];
        const auto operation_index = index + 1;
        if (request.source_id != source.source_id ||
            request.model_family_id != "spatial" ||
            request.object_class != "spatial_crs" ||
            !same_presented_name(request.qualified_name,
                                 source.model_spatial_crs_names[index]) ||
            operation_index >= source.model_operation_ids.size() ||
            crs.operation_id != source.model_operation_ids[operation_index] ||
            crs.crs_uuid != *resolution.spatial_crs_uuid ||
            crs.crs_generation != resolution.spatial_crs_generation) {
          return refuse_model("SB_MODEL_SPATIAL_CRS_MISMATCH_V1",
                              "spatial operation CRS does not match the source descriptor");
        }
      }
    } else if (resolution.spatial_crs_uuid.has_value() ||
               resolution.spatial_crs_generation != 0 ||
               !context.spatial_crs_bindings.empty()) {
      return refuse_model("SB_MODEL_BINDING_INCOMPLETE_V1",
                          "columnar binding contains spatial CRS authority");
    }

    const auto ast_expression_by_id = [&](const std::uint32_t expression_id)
        -> const NativeExpressionAstNode* {
      const auto found = std::ranges::find_if(
          ast.expressions, [&](const auto& expression) {
            return expression.expression_id == expression_id;
          });
      return found == ast.expressions.end() ? nullptr : &*found;
    };
    std::unordered_set<std::uint32_t> operation_roots;
    for (std::size_t index = 0; index < source.model_operation_ids.size();
         ++index) {
      const auto expression_id = source.model_operation_expression_ids[index];
      const auto* expression = ast_expression_by_id(expression_id);
      if (expression == nullptr ||
          expression->expression_kind != NativeExpressionAstKind::kFunctionCall ||
          expression->operator_name != source.model_operation_ids[index] ||
          !operation_roots.insert(expression_id).second ||
          (index == 0 && !expression->child_expression_ids.empty())) {
        return refuse_model("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                            "model operation root identity/order drifted");
      }
    }

    std::vector<const NativeCatalogColumnBindingInput*> selected_columns;
    if (!spatial) {
      if (has_project) {
        if (source.model_columnar_project_names.empty() ||
            source.model_columnar_project_names.size() !=
                source.model_columnar_project_expression_ids.size()) {
          return refuse_model("SB_MODEL_COLUMNAR_PROJECTION_INVALID_V1",
                              "columnar projection vector is incomplete");
        }
        std::unordered_set<std::string> selected_uuids;
        for (const auto& presented : source.model_columnar_project_names) {
          if (presented.size() < 2) {
            return refuse_model("SB_MODEL_COLUMNAR_PROJECTION_INVALID_V1",
                                "columnar projection name is not qualified");
          }
          const auto key = presented.back().quoted
                               ? presented.back().spelling
                               : ToLowerAscii(presented.back().spelling);
          const auto found = column_by_name.find(key);
          if (found == column_by_name.end() ||
              !selected_uuids.insert(found->second->column_uuid).second) {
            return refuse_model(
                found == column_by_name.end()
                    ? "SB_MODEL_COLUMNAR_PROJECTION_INVALID_V1"
                    : "SB_MODEL_COLUMNAR_PROJECT_DUPLICATE_REFUSED_V1",
                "columnar projection column resolution is absent or duplicate");
          }
          selected_columns.push_back(found->second);
        }
      } else {
        for (const auto& column : resolution.columns) {
          selected_columns.push_back(&column);
        }
      }
    }
    const auto output_count = spatial
                                  ? std::size_t{3} +
                                        static_cast<std::size_t>(has_match) +
                                        static_cast<std::size_t>(has_nearest)
                                  : selected_columns.size();
    const auto ast_expression_count = std::ranges::count_if(
        ast.expressions, [](const auto& expression) {
          return expression.expression_kind != NativeExpressionAstKind::kWildcard;
        });
    if (context.outputs.size() != output_count ||
        context.expressions.size() != output_count + ast_expression_count) {
      return refuse_model("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                          "spatial/columnar output or expression cohort is incomplete");
    }
    std::unordered_set<std::uint32_t> carried_descriptor_ids;
    for (const auto& output : context.outputs) {
      carried_descriptor_ids.insert(output.descriptor_id);
    }
    for (const auto& expression : context.expressions) {
      carried_descriptor_ids.insert(expression.descriptor_id);
    }
    for (const auto& descriptor : context.descriptors) {
      if (!carried_descriptor_ids.contains(descriptor.descriptor_id)) continue;
      bound.descriptors.push_back(
          {descriptor.descriptor_id, descriptor.descriptor_uuid,
           descriptor.type_uuid, descriptor.nullability,
           descriptor.collation_uuid, descriptor.timezone_profile_id,
           descriptor.width_precision_scale, descriptor.canonical_type_name,
           descriptor.element_profile});
    }
    std::ranges::sort(bound.descriptors, {},
                      &BoundDescriptorAstRecord::descriptor_id);
    BoundRelationAstRecord bound_relation;
    bound_relation.relation_id = ast.relations.front().relation_id;
    bound_relation.relation_kind = NativeRelationAstKind::kCatalogSource;
    bound_relation.semantic_variant_id = expected_semantic;
    bound_relation.bound_object_uuid = resolution.object_uuid;
    static constexpr std::array<std::string_view, 5> kSpatialNames{
        "row_uuid", "spatial_value", "crs_uuid", "predicate_truth",
        "distance"};
    static constexpr std::array<std::string_view, 5> kSpatialTypes{
        "uuid", "geometry", "uuid", "boolean", "real64"};
    std::unordered_set<std::uint32_t> output_expression_ids;
    for (std::size_t index = 0; index < output_count; ++index) {
      const auto& output = context.outputs[index];
      const auto& expression = context.expressions[index];
      const auto* selected = spatial && index < 3
                                 ? &resolution.columns[index]
                                 : (!spatial ? selected_columns[index] : nullptr);
      const auto spatial_shape_index =
          !spatial || index < 3
              ? index
              : (has_match && index == 3 ? std::size_t{3}
                                         : std::size_t{4});
      const auto expected_name =
          spatial ? kSpatialNames[spatial_shape_index]
                  : std::string_view{selected->canonical_name_key};
      const auto expected_type =
          spatial ? kSpatialTypes[spatial_shape_index]
                  : std::string_view{descriptor_by_id.at(selected->descriptor_id)
                                         ->canonical_type_name};
      const auto expected_binding =
          selected == nullptr ? resolution.object_uuid : selected->column_uuid;
      if (output.output_id != index + 1 || output.ordinal != index ||
          output.relation_id != bound_relation.relation_id || !output.visible ||
          output.expression_id != expression.expression_id ||
          output.output_name_utf8 != expected_name ||
          output.descriptor_id != expression.descriptor_id ||
          !descriptor_by_id.contains(output.descriptor_id) ||
          descriptor_by_id.at(output.descriptor_id)->canonical_type_name !=
              expected_type ||
          (spatial &&
           descriptor_by_id.at(output.descriptor_id)->nullability !=
               BoundNullability::kNonNull) ||
          expression.function_uuid.has_value() ||
          expression.bound_name_uuid != expected_binding ||
          !output_expression_ids.insert(expression.expression_id).second) {
        return refuse_model("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                            "spatial/columnar public output order/type drifted");
      }
      bound.expressions.push_back(
          {expression.expression_id, NativeExpressionAstKind::kIdentifier,
           std::nullopt, {}, expression.descriptor_id, std::nullopt,
           expression.bound_name_uuid, std::nullopt, std::nullopt});
      bound_relation.output_expression_ids.push_back(expression.expression_id);
      bound.outputs.push_back(
          {output.output_id, output.relation_id, output.expression_id,
           output.output_name_utf8, output.descriptor_id, output.visible,
           output.ordinal});
    }

    std::unordered_map<std::uint32_t, std::uint32_t> ast_to_bound;
    std::size_t binding_index = output_count;
    for (const auto& ast_expression : ast.expressions) {
      if (ast_expression.expression_kind == NativeExpressionAstKind::kWildcard) {
        continue;
      }
      const auto& expression = context.expressions[binding_index++];
      if (!descriptor_by_id.contains(expression.descriptor_id) ||
          (expression.function_uuid.has_value() &&
           !IsNonNullCanonicalUuid(*expression.function_uuid)) ||
          (expression.bound_name_uuid.has_value() &&
           !IsNonNullCanonicalUuid(*expression.bound_name_uuid)) ||
          (operation_roots.contains(ast_expression.expression_id) &&
           expression.function_uuid.has_value()) ||
          output_expression_ids.contains(expression.expression_id) ||
          !ast_to_bound.emplace(ast_expression.expression_id,
                                expression.expression_id).second) {
        return refuse_model("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                            "spatial/columnar expression binding is invalid");
      }
      BoundExpressionAstRecord record;
      record.expression_id = expression.expression_id;
      record.expression_kind = ast_expression.expression_kind;
      record.literal_kind = ast_expression.literal_kind;
      record.result_descriptor_id = expression.descriptor_id;
      record.bound_function_uuid = expression.function_uuid;
      record.bound_name_uuid = expression.bound_name_uuid;
      if (!ast_expression.operator_name.empty() &&
          !expression.function_uuid.has_value()) {
        record.canonical_operator_name = ast_expression.operator_name;
      }
      if (ast_expression.expression_kind == NativeExpressionAstKind::kLiteral ||
          ast_expression.expression_kind == NativeExpressionAstKind::kParameter) {
        record.literal_or_parameter_ref = ast_expression.spelling;
      }
      for (const auto child : ast_expression.child_expression_ids) {
        const auto mapped = ast_to_bound.find(child);
        if (mapped == ast_to_bound.end()) {
          return refuse_model("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                              "model expression child is unreachable/reordered");
        }
        record.child_expression_ids.push_back(mapped->second);
      }
      bound.expressions.push_back(std::move(record));
    }
    const auto mapped_id = [&](const std::optional<std::uint32_t> id)
        -> std::optional<std::uint32_t> {
      if (!id.has_value()) return std::nullopt;
      const auto found = ast_to_bound.find(*id);
      return found == ast_to_bound.end()
                 ? std::nullopt
                 : std::optional<std::uint32_t>{found->second};
    };
    const auto mapped_ids = [&](const auto& ids) {
      std::vector<std::uint32_t> result;
      result.reserve(ids.size());
      for (const auto id : ids) {
        const auto found = ast_to_bound.find(id);
        if (found == ast_to_bound.end()) return std::vector<std::uint32_t>{};
        result.push_back(found->second);
      }
      return result;
    };
    const auto mapped_operation_roots =
        mapped_ids(source.model_operation_expression_ids);
    if (mapped_operation_roots.size() != source.model_operation_ids.size()) {
      return refuse_model("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                          "model operation roots are not all bound");
    }
    BoundCatalogRelationSourceAstRecord bound_source;
    bound_source.source_id = source.source_id;
    bound_source.source_kind = source.source_kind;
    bound_source.resolution_state = NativeCatalogRelationResolutionState::kBound;
    bound_source.qualified_name = source.qualified_name;
    bound_source.alias = source.alias;
    bound_source.alias_is_explicit = source.alias_is_explicit;
    bound_source.model_family_id = source.model_family_id;
    bound_source.model_operation_id = source.model_operation_id;
    bound_source.model_operation_ids = source.model_operation_ids;
    bound_source.model_operation_expression_ids = mapped_operation_roots;
    bound_source.model_source_alias = source.model_source_alias;
    bound_source.model_spatial_alias_expression_id =
        mapped_id(source.model_spatial_alias_expression_id);
    bound_source.model_spatial_operation_expression_id =
        mapped_id(source.model_spatial_operation_expression_id);
    bound_source.model_spatial_match_expression_id =
        mapped_id(source.model_spatial_match_expression_id);
    bound_source.model_spatial_nearest_expression_id =
        mapped_id(source.model_spatial_nearest_expression_id);
    bound_source.model_spatial_query_expression_id =
        mapped_id(source.model_spatial_query_expression_id);
    bound_source.model_spatial_query_expression_ids =
        mapped_ids(source.model_spatial_query_expression_ids);
    bound_source.model_spatial_predicate_expression_id =
        mapped_id(source.model_spatial_predicate_expression_id);
    bound_source.model_spatial_crs_expression_id =
        mapped_id(source.model_spatial_crs_expression_id);
    bound_source.model_spatial_crs_expression_ids =
        mapped_ids(source.model_spatial_crs_expression_ids);
    bound_source.model_spatial_crs_name = source.model_spatial_crs_name;
    bound_source.model_spatial_crs_names = source.model_spatial_crs_names;
    bound_source.model_spatial_predicate_id = source.model_spatial_predicate_id;
    bound_source.model_spatial_top_k_expression_id =
        mapped_id(source.model_spatial_top_k_expression_id);
    bound_source.model_spatial_top_k = source.model_spatial_top_k;
    for (const auto& crs : context.spatial_crs_bindings) {
      bound_source.model_spatial_crs_uuids.push_back(crs.crs_uuid);
      bound_source.model_spatial_crs_generations.push_back(crs.crs_generation);
    }
    bound_source.model_columnar_alias_expression_id =
        mapped_id(source.model_columnar_alias_expression_id);
    bound_source.model_columnar_operation_expression_id =
        mapped_id(source.model_columnar_operation_expression_id);
    bound_source.model_columnar_project_expression_id =
        mapped_id(source.model_columnar_project_expression_id);
    bound_source.model_columnar_filter_expression_id =
        mapped_id(source.model_columnar_filter_expression_id);
    bound_source.model_columnar_predicate_expression_id =
        mapped_id(source.model_columnar_predicate_expression_id);
    bound_source.model_columnar_project_expression_ids =
        mapped_ids(source.model_columnar_project_expression_ids);
    for (const auto* column : selected_columns) {
      if (has_project) {
        bound_source.model_columnar_project_column_uuids.push_back(
            column->column_uuid);
      }
    }
    bound_source.qualified_name_range = source.qualified_name_range;
    bound_source.range = source.range;
    bound_source.object_uuid = resolution.object_uuid;
    bound_source.resolved_object_type = resolution.resolved_object_type;
    bound_source.resolved_schema_uuid = resolution.resolved_schema_uuid;
    bound_source.parent_object_uuid = resolution.parent_object_uuid;
    bound_source.catalog_generation_id = resolution.catalog_generation_id;
    bound_source.security_epoch = resolution.security_epoch;
    bound_source.resource_epoch = resolution.resource_epoch;
    for (const auto& column : resolution.columns) {
      bound_source.columns.push_back(
          {column.ordinal, column.column_uuid, column.descriptor_id,
           column.canonical_name_key});
    }
    for (const auto predicate : ast.relations.front().predicate_expression_ids) {
      const auto mapped = ast_to_bound.find(predicate);
      if (mapped == ast_to_bound.end()) {
        return refuse_model("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                            "model predicate root is unreachable");
      }
      bound_relation.predicate_expression_ids.push_back(mapped->second);
    }
    bound_relation.bound_expression_ids = bound_relation.output_expression_ids;
    bound_relation.bound_expression_ids.insert(
        bound_relation.bound_expression_ids.end(),
        mapped_operation_roots.begin(), mapped_operation_roots.end());
    for (const auto& ast_expression : ast.expressions) {
      if (ast_expression.expression_kind != NativeExpressionAstKind::kWildcard &&
          !operation_roots.contains(ast_expression.expression_id)) {
        bound_relation.bound_expression_ids.push_back(
            ast_to_bound.at(ast_expression.expression_id));
      }
    }
    bound.catalog_relation_sources.push_back(std::move(bound_source));
    bound.relations.push_back(std::move(bound_relation));
    BoundScopeAstRecord scope;
    scope.scope_id = 1;
    scope.visible_relation_ids = {ast.root_relation_id};
    for (const auto& output : bound.outputs) {
      if (output.visible) scope.visible_projection_ids.push_back(output.output_id);
    }
    scope.catalog_epoch_uuid = context.catalog_epoch_uuid;
    bound.scopes.push_back(std::move(scope));
    bound.bound_ast_uuid = context.bound_ast_uuid;
    bound.security_context_uuid = context.security_context_uuid;
    bound.root_relation_id = ast.root_relation_id;
    bound.root_scope_id = 1;
    bound.bound = true;
    return bound;
  }
  if (search_source_ast != ast.catalog_relation_sources.end()) {
    // QOW-SOURCE-RCP-078-SEARCH-BINDING-V1
    const auto refuse_search = [&](const char* diagnostic,
                                   const char* detail) {
      AddBoundAstDiagnostic(&bound, diagnostic, detail);
      return RefusedBoundAst(std::move(bound));
    };
    const bool ranked =
        search_source_ast->model_operation_id == "SEARCH_RANKED_QUERY";
    const bool phrase =
        search_source_ast->model_operation_id == "SEARCH_PHRASE_QUERY";
    const bool fuzzy =
        search_source_ast->model_operation_id == "SEARCH_FUZZY_QUERY";
    const bool any_filter =
        search_source_ast->model_search_filter_expression_id.has_value() ||
        search_source_ast->model_search_category_predicate_expression_id.has_value() ||
        search_source_ast->model_search_category_column_expression_id.has_value() ||
        search_source_ast->model_search_category_value_expression_id.has_value();
    const bool complete_filter =
        search_source_ast->model_search_filter_expression_id.has_value() &&
        search_source_ast->model_search_category_predicate_expression_id.has_value() &&
        search_source_ast->model_search_category_column_expression_id.has_value() &&
        search_source_ast->model_search_category_value_expression_id.has_value();
    const std::string semantic =
        std::string("sblr.model-source.search-") +
        (ranked ? "ranked-query" : (phrase ? "phrase-query" : "fuzzy-query")) +
        (complete_filter ? "-structured-filter.v1" : ".v1");
    if (!IsCanonicalStatementTimestamp(context.statement_timestamp) ||
        ast.catalog_relation_sources.size() != 1 || ast.relations.size() != 1 ||
        ast.root_relation_id != ast.relations.front().relation_id ||
        ast.relations.front().relation_kind !=
            NativeRelationAstKind::kCatalogSource ||
        ast.relations.front().relation_source_ids !=
            std::vector<std::uint32_t>{search_source_ast->source_id} ||
        ast.relations.front().predicate_expression_ids.size() != 1 ||
        search_source_ast->model_family_id != "search" ||
        (!ranked && !phrase && !fuzzy) || (any_filter && !complete_filter) ||
        fuzzy != search_source_ast->model_search_edit_expression_id.has_value() ||
        !search_source_ast->model_search_top_k.has_value() ||
        *search_source_ast->model_search_top_k == 0 ||
        *search_source_ast->model_search_top_k > 0xffffffffULL ||
        search_source_ast->qualified_name.empty() ||
        !search_source_ast->alias.has_value() ||
        !search_source_ast->model_search_alias_expression_id.has_value() ||
        !search_source_ast->model_search_match_expression_id.has_value() ||
        !search_source_ast->model_search_query_expression_id.has_value() ||
        !search_source_ast->model_search_text_expression_id.has_value() ||
        !search_source_ast->model_search_analyzer_expression_id.has_value() ||
        !search_source_ast->model_search_top_k_expression_id.has_value() ||
        search_source_ast->model_search_analyzer_name.empty() ||
        ast.model_object_resolution_requests.size() != 2 ||
        context.catalog_relations.size() != 1 || context.relations.size() != 1 ||
        context.relations.front().relation_id !=
            ast.relations.front().relation_id ||
        context.relations.front().semantic_variant_id != semantic ||
        !IsNonNullCanonicalUuid(context.search_analyzer_uuid) ||
        context.search_analyzer_generation == 0) {
      return refuse_search("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                           "search AST or binding cohort is incomplete");
    }
    const auto& collection_request = ast.model_object_resolution_requests[0];
    const auto& analyzer_request = ast.model_object_resolution_requests[1];
    const auto same_presented_name = [](const auto& left,
                                        const auto& right) {
      return left.size() == right.size() &&
             std::ranges::equal(left, right, [](const auto& lhs,
                                                const auto& rhs) {
               return lhs.spelling == rhs.spelling &&
                      lhs.quoted == rhs.quoted;
             });
    };
    if (collection_request.source_id != search_source_ast->source_id ||
        collection_request.model_family_id != "search" ||
        collection_request.object_class != "search" ||
        analyzer_request.source_id != search_source_ast->source_id ||
        analyzer_request.model_family_id != "search" ||
        analyzer_request.object_class != "search_analyzer" ||
        !same_presented_name(collection_request.qualified_name,
                             search_source_ast->qualified_name) ||
        !same_presented_name(analyzer_request.qualified_name,
                             search_source_ast->model_search_analyzer_name)) {
      return refuse_search("SB_MODEL_BINDING_INCOMPLETE_V1",
                           "search resolution descriptions were substituted");
    }
    const auto& resolution = context.catalog_relations.front();
    if (resolution.source_id != search_source_ast->source_id ||
        resolution.resolution_state !=
            NativeCatalogRelationResolutionState::kBound ||
        !IsNonNullCanonicalUuid(resolution.object_uuid) ||
        !IsNonNullCanonicalUuid(resolution.resolved_schema_uuid) ||
        resolution.resolved_object_type != "search" ||
        resolution.catalog_generation_id == 0 || resolution.security_epoch == 0 ||
        resolution.resource_epoch == 0 || resolution.columns.size() != 2 ||
        resolution.columns[0].ordinal != 0 ||
        resolution.columns[0].canonical_name_key != "body" ||
        resolution.columns[1].ordinal != 1 ||
        resolution.columns[1].canonical_name_key != "category" ||
        !IsNonNullCanonicalUuid(resolution.columns[0].column_uuid) ||
        !IsNonNullCanonicalUuid(resolution.columns[1].column_uuid) ||
        !descriptor_by_id.contains(resolution.columns[0].descriptor_id) ||
        !descriptor_by_id.contains(resolution.columns[1].descriptor_id)) {
      return refuse_search("SB_MODEL_BINDING_INCOMPLETE_V1",
                           "search storage descriptor is not BODY/CATEGORY");
    }
    const auto* body_descriptor =
        descriptor_by_id.at(resolution.columns[0].descriptor_id);
    const auto* category_descriptor =
        descriptor_by_id.at(resolution.columns[1].descriptor_id);
    if (body_descriptor->nullability != BoundNullability::kNonNull ||
        category_descriptor->nullability != BoundNullability::kNonNull ||
        body_descriptor->canonical_type_name != "text" ||
        category_descriptor->canonical_type_name != "text") {
      return refuse_search("SB_MODEL_SEARCH_DOCUMENT_INVALID_V1",
                           "search storage descriptors are not non-null TEXT");
    }
    const auto ast_expression_by_id = [&](const std::uint32_t expression_id)
        -> const NativeExpressionAstNode* {
      const auto found = std::ranges::find_if(
          ast.expressions, [&](const auto& expression) {
            return expression.expression_id == expression_id;
          });
      return found == ast.expressions.end() ? nullptr : &*found;
    };
    const auto* match = ast_expression_by_id(
        *search_source_ast->model_search_match_expression_id);
    const auto* alias = ast_expression_by_id(
        *search_source_ast->model_search_alias_expression_id);
    const auto* query = ast_expression_by_id(
        *search_source_ast->model_search_query_expression_id);
    const auto* text = ast_expression_by_id(
        *search_source_ast->model_search_text_expression_id);
    const auto* analyzer = ast_expression_by_id(
        *search_source_ast->model_search_analyzer_expression_id);
    const auto* top_k = ast_expression_by_id(
        *search_source_ast->model_search_top_k_expression_id);
    if (match == nullptr || alias == nullptr || query == nullptr ||
        text == nullptr || analyzer == nullptr || top_k == nullptr ||
        match->expression_kind != NativeExpressionAstKind::kFunctionCall ||
        match->operator_name != "SEARCH_MATCH" ||
        match->child_expression_ids !=
            std::vector<std::uint32_t>{alias->expression_id,
                                       query->expression_id,
                                       analyzer->expression_id,
                                       top_k->expression_id} ||
        query->expression_kind != NativeExpressionAstKind::kFunctionCall ||
        query->operator_name != search_source_ast->model_search_query_kind ||
        query->child_expression_ids.empty() ||
        query->child_expression_ids.front() != text->expression_id ||
        alias->expression_kind != NativeExpressionAstKind::kIdentifier ||
        analyzer->expression_kind != NativeExpressionAstKind::kIdentifier ||
        !same_presented_name(analyzer->qualified_identifier,
                             search_source_ast->model_search_analyzer_name) ||
        top_k->expression_kind != NativeExpressionAstKind::kLiteral ||
        top_k->literal_kind != NativeLiteralAstKind::kNumeric ||
        top_k->spelling !=
            std::to_string(*search_source_ast->model_search_top_k)) {
      return refuse_search("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                           "search typed-DAG identity/order drifted");
    }
    for (const auto& descriptor : context.descriptors) {
      bound.descriptors.push_back(
          {descriptor.descriptor_id, descriptor.descriptor_uuid,
           descriptor.type_uuid, descriptor.nullability,
           descriptor.collation_uuid, descriptor.timezone_profile_id,
           descriptor.width_precision_scale, descriptor.canonical_type_name,
           descriptor.element_profile});
    }
    std::ranges::sort(bound.descriptors, {},
                      &BoundDescriptorAstRecord::descriptor_id);
    if (context.outputs.size() != 5 || context.expressions.size() < 5) {
      return refuse_search("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                           "search public output cohort is incomplete");
    }
    static constexpr std::array<std::string_view, 5> kNames{
        "document_uuid", "analyzer_uuid", "analyzer_generation", "score",
        "rank"};
    static constexpr std::array<std::string_view, 5> kTypes{
        "uuid", "uuid", "uint64", "real64", "uint64"};
    BoundRelationAstRecord bound_relation;
    bound_relation.relation_id = ast.relations.front().relation_id;
    bound_relation.relation_kind = NativeRelationAstKind::kCatalogSource;
    bound_relation.semantic_variant_id = semantic;
    bound_relation.bound_object_uuid = resolution.object_uuid;
    std::unordered_set<std::uint32_t> public_descriptor_ids;
    for (std::size_t index = 0; index < kNames.size(); ++index) {
      const auto& output = context.outputs[index];
      const auto& expression = context.expressions[index];
      const auto& expected_output_binding =
          (index == 1 || index == 2) ? context.search_analyzer_uuid
                                     : resolution.object_uuid;
      if (output.output_id != index + 1 || output.ordinal != index ||
          output.relation_id != bound_relation.relation_id ||
          output.expression_id != expression.expression_id ||
          output.output_name_utf8 != kNames[index] || !output.visible ||
          output.descriptor_id != expression.descriptor_id ||
          !descriptor_by_id.contains(output.descriptor_id) ||
          descriptor_by_id.at(output.descriptor_id)->canonical_type_name !=
              kTypes[index] ||
          descriptor_by_id.at(output.descriptor_id)->nullability !=
              BoundNullability::kNonNull ||
          expression.bound_name_uuid != expected_output_binding ||
          !public_descriptor_ids.insert(output.descriptor_id).second) {
        return refuse_search("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                             "search public output order/type drifted");
      }
      bound.expressions.push_back(
          {expression.expression_id, NativeExpressionAstKind::kIdentifier,
           std::nullopt, {}, expression.descriptor_id, std::nullopt,
           expression.bound_name_uuid, std::nullopt, std::nullopt});
      bound_relation.output_expression_ids.push_back(expression.expression_id);
      bound.outputs.push_back(
          {output.output_id, output.relation_id, output.expression_id,
           output.output_name_utf8, output.descriptor_id, output.visible,
           output.ordinal});
    }
    std::unordered_map<std::uint32_t, std::uint32_t> ast_to_bound;
    std::size_t binding_index = 5;
    for (const auto& ast_expression : ast.expressions) {
      if (ast_expression.expression_kind == NativeExpressionAstKind::kWildcard) {
        continue;
      }
      if (binding_index >= context.expressions.size()) {
        return refuse_search("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                             "search expression binding is incomplete");
      }
      const auto& expression = context.expressions[binding_index++];
      if (expression.expression_id != ast_expression.expression_id ||
          !descriptor_by_id.contains(expression.descriptor_id) ||
          expression.function_uuid.has_value() ||
          (expression.bound_name_uuid.has_value() &&
           !IsNonNullCanonicalUuid(*expression.bound_name_uuid)) ||
          !ast_to_bound.emplace(ast_expression.expression_id,
                                expression.expression_id).second) {
        return refuse_search("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                             "search expression binding is invalid");
      }
      BoundExpressionAstRecord record;
      record.expression_id = expression.expression_id;
      record.expression_kind = ast_expression.expression_kind;
      record.literal_kind = ast_expression.literal_kind;
      record.result_descriptor_id = expression.descriptor_id;
      record.bound_name_uuid = expression.bound_name_uuid;
      if (!ast_expression.operator_name.empty()) {
        record.canonical_operator_name = ast_expression.operator_name;
      }
      if (ast_expression.expression_kind == NativeExpressionAstKind::kLiteral ||
          ast_expression.expression_kind == NativeExpressionAstKind::kParameter) {
        record.literal_or_parameter_ref = ast_expression.spelling;
      }
      for (const auto child : ast_expression.child_expression_ids) {
        const auto mapped = ast_to_bound.find(child);
        if (mapped == ast_to_bound.end()) {
          return refuse_search("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                               "search expression child is unreachable/reordered");
        }
        record.child_expression_ids.push_back(mapped->second);
      }
      bound.expressions.push_back(std::move(record));
    }
    if (binding_index != context.expressions.size()) {
      return refuse_search("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                           "search expression binding has extra nodes");
    }
    const auto mapped_id = [&](const std::optional<std::uint32_t> id)
        -> std::optional<std::uint32_t> {
      if (!id.has_value()) return std::nullopt;
      const auto found = ast_to_bound.find(*id);
      return found == ast_to_bound.end()
                 ? std::nullopt
                 : std::optional<std::uint32_t>(found->second);
    };
    const auto mapped_alias =
        mapped_id(search_source_ast->model_search_alias_expression_id);
    const auto mapped_text =
        mapped_id(search_source_ast->model_search_text_expression_id);
    const auto mapped_analyzer =
        mapped_id(search_source_ast->model_search_analyzer_expression_id);
    const auto mapped_top_k =
        mapped_id(search_source_ast->model_search_top_k_expression_id);
    const auto find_bound = [&](const std::optional<std::uint32_t> id)
        -> const BoundExpressionAstRecord* {
      if (!id.has_value()) return nullptr;
      const auto found = std::ranges::find_if(
          bound.expressions, [&](const auto& expression) {
            return expression.expression_id == *id;
          });
      return found == bound.expressions.end() ? nullptr : &*found;
    };
    const auto* bound_alias = find_bound(mapped_alias);
    const auto* bound_text = find_bound(mapped_text);
    const auto* bound_analyzer = find_bound(mapped_analyzer);
    const auto* bound_top_k = find_bound(mapped_top_k);
    if (bound_alias == nullptr || bound_text == nullptr ||
        bound_analyzer == nullptr || bound_top_k == nullptr ||
        bound_alias->bound_name_uuid != resolution.object_uuid ||
        bound_analyzer->bound_name_uuid != context.search_analyzer_uuid ||
        descriptor_by_id.at(bound_text->result_descriptor_id)
                ->canonical_type_name != "text" ||
        descriptor_by_id.at(bound_text->result_descriptor_id)->nullability !=
            BoundNullability::kNonNull ||
        descriptor_by_id.at(bound_top_k->result_descriptor_id)
                ->canonical_type_name != "uint64" ||
        descriptor_by_id.at(bound_top_k->result_descriptor_id)->nullability !=
            BoundNullability::kNonNull) {
      return refuse_search("SB_MODEL_SEARCH_QUERY_TYPE_REFUSED_V1",
                           "search query/analyzer/top-k binding drifted");
    }
    if (complete_filter) {
      const auto* filter = find_bound(mapped_id(
          search_source_ast->model_search_filter_expression_id));
      const auto* filter_alias =
          filter == nullptr || filter->child_expression_ids.empty()
              ? nullptr
              : find_bound(filter->child_expression_ids.front());
      const auto* column = find_bound(mapped_id(
          search_source_ast->model_search_category_column_expression_id));
      const auto* value = find_bound(mapped_id(
          search_source_ast->model_search_category_value_expression_id));
      if (filter == nullptr || filter_alias == nullptr || column == nullptr ||
          value == nullptr ||
          filter_alias->bound_name_uuid != resolution.object_uuid ||
          column->bound_name_uuid != resolution.columns[1].column_uuid ||
          column->result_descriptor_id != resolution.columns[1].descriptor_id ||
          descriptor_by_id.at(value->result_descriptor_id)
                  ->canonical_type_name != "text" ||
          descriptor_by_id.at(value->result_descriptor_id)->nullability !=
              BoundNullability::kNonNull) {
        return refuse_search("SB_MODEL_SEARCH_FILTER_REFUSED_V1",
                             "search category filter binding drifted");
      }
    }
    std::uint32_t maximum_expression_id = 0;
    for (const auto& expression : bound.expressions) {
      maximum_expression_id =
          std::max(maximum_expression_id, expression.expression_id);
    }
    const std::uint32_t synthetic_expression_count = complete_filter ? 3 : 4;
    if (maximum_expression_id >
            std::numeric_limits<std::uint32_t>::max() -
                synthetic_expression_count ||
        !mapped_analyzer.has_value() ||
        !mapped_id(search_source_ast->model_search_match_expression_id)
             .has_value()) {
      return refuse_search("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                           "search analyzer binding expression IDs overflow");
    }
    const auto analyzer_generation_expression_id = maximum_expression_id + 1;
    const auto analyzer_digest_expression_id = maximum_expression_id + 2;
    const auto analyzer_binding_expression_id = maximum_expression_id + 3;
    const auto category_binding_expression_id =
        complete_filter ? std::optional<std::uint32_t>{}
                        : std::optional<std::uint32_t>{maximum_expression_id + 4};
    const auto analyzer_descriptor_id = bound_analyzer->result_descriptor_id;
    const auto analyzer_generation_descriptor_id =
        bound_top_k->result_descriptor_id;
    const auto analyzer_digest_descriptor_id = bound_text->result_descriptor_id;
    BoundExpressionAstRecord analyzer_generation_expression;
    analyzer_generation_expression.expression_id =
        analyzer_generation_expression_id;
    analyzer_generation_expression.expression_kind =
        NativeExpressionAstKind::kLiteral;
    analyzer_generation_expression.literal_kind = NativeLiteralAstKind::kNumeric;
    analyzer_generation_expression.result_descriptor_id =
        analyzer_generation_descriptor_id;
    analyzer_generation_expression.literal_or_parameter_ref =
        std::to_string(context.search_analyzer_generation);
    bound.expressions.push_back(std::move(analyzer_generation_expression));

    BoundExpressionAstRecord analyzer_digest_expression;
    analyzer_digest_expression.expression_id = analyzer_digest_expression_id;
    analyzer_digest_expression.expression_kind =
        NativeExpressionAstKind::kLiteral;
    analyzer_digest_expression.literal_kind = NativeLiteralAstKind::kString;
    analyzer_digest_expression.result_descriptor_id =
        analyzer_digest_descriptor_id;
    analyzer_digest_expression.literal_or_parameter_ref =
        "9033908d159ddd442f2042467fd49e0a12b47679f7514e9aa6e55488e151d316";
    bound.expressions.push_back(std::move(analyzer_digest_expression));

    BoundExpressionAstRecord analyzer_binding_expression;
    analyzer_binding_expression.expression_id = analyzer_binding_expression_id;
    analyzer_binding_expression.expression_kind =
        NativeExpressionAstKind::kFunctionCall;
    analyzer_binding_expression.child_expression_ids = {
        *mapped_analyzer, analyzer_generation_expression_id,
        analyzer_digest_expression_id};
    analyzer_binding_expression.result_descriptor_id = analyzer_descriptor_id;
    analyzer_binding_expression.canonical_operator_name =
        "SEARCH_ANALYZER_BINDING";
    bound.expressions.push_back(std::move(analyzer_binding_expression));
    if (category_binding_expression_id.has_value()) {
      BoundExpressionAstRecord category_binding_expression;
      category_binding_expression.expression_id =
          *category_binding_expression_id;
      category_binding_expression.expression_kind =
          NativeExpressionAstKind::kIdentifier;
      category_binding_expression.result_descriptor_id =
          resolution.columns[1].descriptor_id;
      category_binding_expression.bound_name_uuid =
          resolution.columns[1].column_uuid;
      bound.expressions.push_back(std::move(category_binding_expression));
    }

    const auto mapped_match =
        *mapped_id(search_source_ast->model_search_match_expression_id);
    const auto bound_match = std::ranges::find_if(
        bound.expressions, [&](const auto& expression) {
          return expression.expression_id == mapped_match;
        });
    if (bound_match == bound.expressions.end() ||
        bound_match->child_expression_ids.size() != 4 ||
        bound_match->child_expression_ids[2] != *mapped_analyzer) {
      return refuse_search("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                           "search analyzer binding root cannot be attached");
    }
    bound_match->child_expression_ids[2] = analyzer_binding_expression_id;
    const auto match_offset =
        static_cast<std::size_t>(bound_match - bound.expressions.begin());
    std::rotate(bound.expressions.begin() + match_offset,
                bound.expressions.end() - synthetic_expression_count,
                bound.expressions.end());

    BoundCatalogRelationSourceAstRecord bound_source;
    bound_source.source_id = search_source_ast->source_id;
    bound_source.source_kind = search_source_ast->source_kind;
    bound_source.resolution_state = NativeCatalogRelationResolutionState::kBound;
    bound_source.qualified_name = search_source_ast->qualified_name;
    bound_source.alias = search_source_ast->alias;
    bound_source.alias_is_explicit = search_source_ast->alias_is_explicit;
    bound_source.model_family_id = search_source_ast->model_family_id;
    bound_source.model_operation_id = search_source_ast->model_operation_id;
    bound_source.model_search_alias_expression_id = mapped_alias;
    bound_source.model_search_match_expression_id = mapped_id(
        search_source_ast->model_search_match_expression_id);
    bound_source.model_search_query_expression_id = mapped_id(
        search_source_ast->model_search_query_expression_id);
    bound_source.model_search_text_expression_id = mapped_text;
    bound_source.model_search_edit_expression_id = mapped_id(
        search_source_ast->model_search_edit_expression_id);
    bound_source.model_search_analyzer_expression_id =
        analyzer_binding_expression_id;
    bound_source.model_search_top_k_expression_id = mapped_top_k;
    bound_source.model_search_filter_expression_id = mapped_id(
        search_source_ast->model_search_filter_expression_id);
    bound_source.model_search_category_predicate_expression_id = mapped_id(
        search_source_ast->model_search_category_predicate_expression_id);
    bound_source.model_search_category_column_expression_id = mapped_id(
        search_source_ast->model_search_category_column_expression_id);
    bound_source.model_search_category_value_expression_id = mapped_id(
        search_source_ast->model_search_category_value_expression_id);
    bound_source.model_search_result_alias =
        search_source_ast->model_search_result_alias;
    bound_source.model_search_analyzer_name =
        search_source_ast->model_search_analyzer_name;
    bound_source.model_search_query_kind =
        search_source_ast->model_search_query_kind;
    bound_source.model_search_top_k = search_source_ast->model_search_top_k;
    bound_source.model_search_analyzer_uuid = context.search_analyzer_uuid;
    bound_source.model_search_analyzer_generation =
        context.search_analyzer_generation;
    bound_source.qualified_name_range = search_source_ast->qualified_name_range;
    bound_source.range = search_source_ast->range;
    bound_source.object_uuid = resolution.object_uuid;
    bound_source.resolved_object_type = resolution.resolved_object_type;
    bound_source.resolved_schema_uuid = resolution.resolved_schema_uuid;
    bound_source.parent_object_uuid = resolution.parent_object_uuid;
    bound_source.catalog_generation_id = resolution.catalog_generation_id;
    bound_source.security_epoch = resolution.security_epoch;
    bound_source.resource_epoch = resolution.resource_epoch;
    for (const auto& column : resolution.columns) {
      bound_source.columns.push_back(
          {column.ordinal, column.column_uuid, column.descriptor_id,
           column.canonical_name_key});
    }
    const auto root = ast_to_bound.find(
        ast.relations.front().predicate_expression_ids.front());
    if (root == ast_to_bound.end()) {
      return refuse_search("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                           "search predicate root is unreachable");
    }
    bound_relation.predicate_expression_ids = {root->second};
    bound_relation.bound_expression_ids = bound_relation.output_expression_ids;
    for (const auto& ast_expression : ast.expressions) {
      if (ast_expression.expression_kind == NativeExpressionAstKind::kWildcard) {
        continue;
      }
      bound_relation.bound_expression_ids.push_back(
          ast_to_bound.at(ast_expression.expression_id));
    }
    if (category_binding_expression_id.has_value()) {
      bound_relation.bound_expression_ids.push_back(
          *category_binding_expression_id);
    }
    bound.catalog_relation_sources.push_back(std::move(bound_source));
    bound.relations.push_back(std::move(bound_relation));
    BoundScopeAstRecord scope;
    scope.scope_id = 1;
    scope.visible_relation_ids = {ast.root_relation_id};
    for (const auto& output : bound.outputs) {
      if (output.visible) scope.visible_projection_ids.push_back(output.output_id);
    }
    scope.catalog_epoch_uuid = context.catalog_epoch_uuid;
    bound.scopes.push_back(std::move(scope));
    bound.bound_ast_uuid = context.bound_ast_uuid;
    bound.security_context_uuid = context.security_context_uuid;
    bound.root_relation_id = ast.root_relation_id;
    bound.root_scope_id = 1;
    bound.bound = true;
    return bound;
  }
  if (vector_source_ast != ast.catalog_relation_sources.end()) {
    // QOW-SOURCE-RCP-077-VECTOR-BINDING-V1
    const auto refuse_vector = [&](const char* diagnostic,
                                   const char* detail) {
      AddBoundAstDiagnostic(&bound, diagnostic, detail);
      return RefusedBoundAst(std::move(bound));
    };
    const bool filtered =
        vector_source_ast->model_operation_id == "VECTOR_FILTERED_SEARCH";
    const bool exact =
        vector_source_ast->model_operation_id == "VECTOR_EXACT_SEARCH";
    const bool any_filter =
        vector_source_ast->model_vector_filter_expression_id.has_value() ||
        vector_source_ast->model_vector_metadata_predicate_expression_id.has_value() ||
        vector_source_ast->model_vector_metadata_column_expression_id.has_value() ||
        vector_source_ast->model_vector_metadata_value_expression_id.has_value();
    const bool complete_filter =
        vector_source_ast->model_vector_filter_expression_id.has_value() &&
        vector_source_ast->model_vector_metadata_predicate_expression_id.has_value() &&
        vector_source_ast->model_vector_metadata_column_expression_id.has_value() &&
        vector_source_ast->model_vector_metadata_value_expression_id.has_value();
    const bool exact_metric =
        vector_source_ast->model_vector_metric_id == "L2_SQUARED" ||
        vector_source_ast->model_vector_metric_id == "COSINE" ||
        vector_source_ast->model_vector_metric_id == "INNER_PRODUCT";
    const std::string_view expected_semantic =
        filtered ? "sblr.model-source.vector-filtered-search.v1"
                 : "sblr.model-source.vector-exact-search.v1";
    if (!IsCanonicalStatementTimestamp(context.statement_timestamp) ||
        ast.catalog_relation_sources.size() != 1 || ast.relations.size() != 1 ||
        ast.root_relation_id != ast.relations.front().relation_id ||
        ast.relations.front().relation_kind !=
            NativeRelationAstKind::kCatalogSource ||
        ast.relations.front().relation_source_ids !=
            std::vector<std::uint32_t>{vector_source_ast->source_id} ||
        ast.relations.front().predicate_expression_ids.size() != 1 ||
        vector_source_ast->model_family_id != "vector" ||
        (!exact && !filtered) || (exact && any_filter) ||
        (filtered && !complete_filter) || !exact_metric ||
        !vector_source_ast->model_vector_top_k.has_value() ||
        *vector_source_ast->model_vector_top_k == 0 ||
        *vector_source_ast->model_vector_top_k > 0xffffffffULL ||
        vector_source_ast->qualified_name.empty() ||
        !vector_source_ast->alias.has_value() ||
        !vector_source_ast->model_vector_alias_expression_id.has_value() ||
        !vector_source_ast->model_vector_nearest_expression_id.has_value() ||
        !vector_source_ast->model_vector_query_expression_id.has_value() ||
        !vector_source_ast->model_vector_metric_expression_id.has_value() ||
        !vector_source_ast->model_vector_top_k_expression_id.has_value() ||
        ast.model_object_resolution_requests.size() != 1 ||
        context.catalog_relations.size() != 1 || context.relations.size() != 1 ||
        context.relations.front().relation_id !=
            ast.relations.front().relation_id ||
        context.relations.front().semantic_variant_id != expected_semantic) {
      return refuse_vector("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                           "vector AST or binding cohort is incomplete");
    }
    const auto& request = ast.model_object_resolution_requests.front();
    if (request.source_id != vector_source_ast->source_id ||
        request.model_family_id != "vector" ||
        request.object_class != "vector" ||
        request.qualified_name.size() != vector_source_ast->qualified_name.size() ||
        !std::ranges::equal(
            request.qualified_name, vector_source_ast->qualified_name,
            [](const auto& left, const auto& right) {
              return left.spelling == right.spelling &&
                     left.quoted == right.quoted;
            })) {
      return refuse_vector("SB_MODEL_BINDING_INCOMPLETE_V1",
                           "vector resolution request does not match its source");
    }
    const auto& resolution = context.catalog_relations.front();
    if (resolution.source_id != vector_source_ast->source_id ||
        resolution.resolution_state !=
            NativeCatalogRelationResolutionState::kBound ||
        !IsNonNullCanonicalUuid(resolution.object_uuid) ||
        !IsNonNullCanonicalUuid(resolution.resolved_schema_uuid) ||
        resolution.resolved_object_type != "vector" ||
        resolution.catalog_generation_id == 0 || resolution.security_epoch == 0 ||
        resolution.resource_epoch == 0 || resolution.columns.size() != 2 ||
        resolution.columns[0].ordinal != 0 ||
        resolution.columns[0].canonical_name_key != "embedding" ||
        resolution.columns[1].ordinal != 1 ||
        resolution.columns[1].canonical_name_key != "metadata" ||
        !IsNonNullCanonicalUuid(resolution.columns[0].column_uuid) ||
        !IsNonNullCanonicalUuid(resolution.columns[1].column_uuid) ||
        !descriptor_by_id.contains(resolution.columns[0].descriptor_id) ||
        !descriptor_by_id.contains(resolution.columns[1].descriptor_id)) {
      return refuse_vector("SB_MODEL_BINDING_INCOMPLETE_V1",
                           "vector storage descriptor is not the exact two-column shape");
    }
    const auto* embedding_descriptor =
        descriptor_by_id.at(resolution.columns[0].descriptor_id);
    const auto* metadata_descriptor =
        descriptor_by_id.at(resolution.columns[1].descriptor_id);
    if (embedding_descriptor->nullability != BoundNullability::kNonNull ||
        metadata_descriptor->nullability != BoundNullability::kNonNull ||
        embedding_descriptor->canonical_type_name != "dense_vector" ||
        embedding_descriptor->element_profile != "real32" ||
        metadata_descriptor->canonical_type_name != "text" ||
        !metadata_descriptor->element_profile.empty() ||
        !embedding_descriptor->width_precision_scale.width.has_value() ||
        *embedding_descriptor->width_precision_scale.width != 3) {
      return refuse_vector("SB_MODEL_VECTOR_VALUE_REFUSED_V1",
                           "vector storage types are not DENSE_VECTOR(3,REAL32)/TEXT");
    }

    const auto ast_expression_by_id = [&](const std::uint32_t expression_id)
        -> const NativeExpressionAstNode* {
      const auto found = std::ranges::find_if(
          ast.expressions, [&](const auto& expression) {
            return expression.expression_id == expression_id;
          });
      return found == ast.expressions.end() ? nullptr : &*found;
    };
    const auto* nearest = ast_expression_by_id(
        *vector_source_ast->model_vector_nearest_expression_id);
    const auto* alias = ast_expression_by_id(
        *vector_source_ast->model_vector_alias_expression_id);
    const auto* query = ast_expression_by_id(
        *vector_source_ast->model_vector_query_expression_id);
    const auto* metric = ast_expression_by_id(
        *vector_source_ast->model_vector_metric_expression_id);
    const auto* top_k = ast_expression_by_id(
        *vector_source_ast->model_vector_top_k_expression_id);
    if (nearest == nullptr || alias == nullptr || query == nullptr ||
        metric == nullptr || top_k == nullptr ||
        nearest->expression_kind != NativeExpressionAstKind::kFunctionCall ||
        nearest->operator_name != "VECTOR_NEAREST" ||
        nearest->child_expression_ids !=
            std::vector<std::uint32_t>{alias->expression_id,
                                       query->expression_id,
                                       metric->expression_id,
                                       top_k->expression_id} ||
        alias->expression_kind != NativeExpressionAstKind::kIdentifier ||
        alias->qualified_identifier.size() != 1 ||
        !((query->expression_kind == NativeExpressionAstKind::kLiteral &&
           query->literal_kind == NativeLiteralAstKind::kVector) ||
          query->expression_kind == NativeExpressionAstKind::kParameter) ||
        metric->expression_kind != NativeExpressionAstKind::kLiteral ||
        metric->literal_kind != NativeLiteralAstKind::kString ||
        metric->spelling != vector_source_ast->model_vector_metric_id ||
        top_k->expression_kind != NativeExpressionAstKind::kLiteral ||
        top_k->literal_kind != NativeLiteralAstKind::kNumeric ||
        top_k->spelling != std::to_string(*vector_source_ast->model_vector_top_k)) {
      return refuse_vector("SB_MODEL_VECTOR_NEAREST_REFUSED_V1",
                           "vector nearest typed-DAG identity/order drifted");
    }
    const auto same_alias = [](const NativeIdentifierAstNode& left,
                               const NativeIdentifierAstNode& right) {
      return left.quoted == right.quoted &&
             (left.quoted ? left.spelling == right.spelling
                          : ToUpperAscii(left.spelling) ==
                                ToUpperAscii(right.spelling));
    };
    if (!same_alias(alias->qualified_identifier.front(),
                    *vector_source_ast->alias)) {
      return refuse_vector("SB_MODEL_BINDING_INCOMPLETE_V1",
                           "vector alias identity is substituted");
    }
    if (filtered) {
      const auto* filter = ast_expression_by_id(
          *vector_source_ast->model_vector_filter_expression_id);
      const auto* metadata_predicate = ast_expression_by_id(
          *vector_source_ast->model_vector_metadata_predicate_expression_id);
      const auto* metadata_column = ast_expression_by_id(
          *vector_source_ast->model_vector_metadata_column_expression_id);
      const auto* metadata_value = ast_expression_by_id(
          *vector_source_ast->model_vector_metadata_value_expression_id);
      const auto* filter_alias =
          filter != nullptr && filter->child_expression_ids.size() == 2
              ? ast_expression_by_id(filter->child_expression_ids[0])
              : nullptr;
      if (filter == nullptr || metadata_predicate == nullptr ||
          metadata_column == nullptr || metadata_value == nullptr ||
          filter_alias == nullptr ||
          filter->expression_kind != NativeExpressionAstKind::kFunctionCall ||
          filter->operator_name != "VECTOR_FILTER" ||
          filter->child_expression_ids !=
              std::vector<std::uint32_t>{filter_alias->expression_id,
                                         metadata_predicate->expression_id} ||
          filter_alias->expression_kind != NativeExpressionAstKind::kIdentifier ||
          filter_alias->qualified_identifier.size() != 1 ||
          !same_alias(filter_alias->qualified_identifier.front(),
                      *vector_source_ast->alias) ||
          metadata_predicate->expression_kind != NativeExpressionAstKind::kBinary ||
          metadata_predicate->operator_name != "=" ||
          metadata_predicate->child_expression_ids !=
              std::vector<std::uint32_t>{metadata_column->expression_id,
                                         metadata_value->expression_id} ||
          metadata_column->expression_kind != NativeExpressionAstKind::kIdentifier ||
          metadata_column->qualified_identifier.size() != 2 ||
          !same_alias(metadata_column->qualified_identifier.front(),
                      *vector_source_ast->alias) ||
          ToUpperAscii(metadata_column->qualified_identifier.back().spelling) !=
              "METADATA" ||
          !((metadata_value->expression_kind == NativeExpressionAstKind::kLiteral &&
             metadata_value->literal_kind == NativeLiteralAstKind::kString) ||
            metadata_value->expression_kind ==
                NativeExpressionAstKind::kParameter)) {
        return refuse_vector("SB_MODEL_VECTOR_FILTER_REFUSED_V1",
                             "vector metadata filter typed-DAG drifted");
      }
    }

    for (const auto& descriptor : context.descriptors) {
      bound.descriptors.push_back(
          {descriptor.descriptor_id, descriptor.descriptor_uuid,
           descriptor.type_uuid, descriptor.nullability,
           descriptor.collation_uuid, descriptor.timezone_profile_id,
           descriptor.width_precision_scale, descriptor.canonical_type_name,
           descriptor.element_profile});
    }
    std::ranges::sort(bound.descriptors, {},
                      &BoundDescriptorAstRecord::descriptor_id);

    const bool wildcard =
        ast.relations.front().output_expression_ids.size() == 1 &&
        ast_expression_by_id(ast.relations.front().output_expression_ids.front()) !=
            nullptr &&
        ast_expression_by_id(ast.relations.front().output_expression_ids.front())
                ->expression_kind == NativeExpressionAstKind::kWildcard;
    if (!wildcard || context.outputs.size() != 3 ||
        context.expressions.size() != ast.expressions.size() - 1 + 3) {
      return refuse_vector("SB_MODEL_BINDING_INCOMPLETE_V1",
                           "vector v1 requires its exact three-field projection");
    }
    const auto maximum_ast_expression = std::ranges::max_element(
        ast.expressions, {}, &NativeExpressionAstNode::expression_id);
    std::unordered_set<std::uint32_t> context_expression_ids;
    if (maximum_ast_expression == ast.expressions.end() ||
        maximum_ast_expression->expression_id >
            std::numeric_limits<std::uint32_t>::max() - 3 ||
        std::ranges::any_of(context.expressions, [&](const auto& expression) {
          return expression.expression_id == 0 ||
                 !context_expression_ids.insert(expression.expression_id).second;
        })) {
      return refuse_vector("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                           "vector expression identities are zero or duplicated");
    }
    for (std::size_t ordinal = 0; ordinal < 3; ++ordinal) {
      if (context.expressions[ordinal].expression_id !=
          maximum_ast_expression->expression_id + ordinal + 1) {
        return refuse_vector("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                             "vector public expression identities were substituted");
      }
    }
    static constexpr std::array<std::string_view, 3> kOutputNames{
        "row_uuid", "distance", "score"};
    BoundRelationAstRecord bound_relation;
    bound_relation.relation_id = ast.relations.front().relation_id;
    bound_relation.relation_kind = NativeRelationAstKind::kCatalogSource;
    bound_relation.semantic_variant_id = std::string(expected_semantic);
    bound_relation.bound_object_uuid = resolution.object_uuid;
    for (std::size_t ordinal = 0; ordinal < kOutputNames.size(); ++ordinal) {
      const auto& output = context.outputs[ordinal];
      const auto& expression = context.expressions[ordinal];
      const auto* descriptor = descriptor_by_id.contains(output.descriptor_id)
                                   ? descriptor_by_id.at(output.descriptor_id)
                                   : nullptr;
      if (output.output_id == 0 || output.ordinal != ordinal ||
          output.relation_id != bound_relation.relation_id ||
          output.output_name_utf8 != kOutputNames[ordinal] ||
          output.expression_id != expression.expression_id ||
          output.descriptor_id != expression.descriptor_id ||
          descriptor == nullptr ||
          descriptor->nullability != BoundNullability::kNonNull ||
          descriptor->canonical_type_name !=
              (ordinal == 0 ? "uuid" : "real64") ||
          !descriptor->element_profile.empty() ||
          !expression.bound_name_uuid.has_value() ||
          !IsNonNullCanonicalUuid(*expression.bound_name_uuid)) {
        return refuse_vector("SB_MODEL_BINDING_INCOMPLETE_V1",
                             "vector public projection binding is invalid");
      }
      bound.expressions.push_back(
          {expression.expression_id, NativeExpressionAstKind::kIdentifier,
           std::nullopt, {}, expression.descriptor_id, std::nullopt,
           expression.bound_name_uuid, std::nullopt, std::nullopt});
      bound_relation.output_expression_ids.push_back(expression.expression_id);
      bound.outputs.push_back(
          {output.output_id, output.relation_id, output.expression_id,
           output.output_name_utf8, output.descriptor_id, output.visible,
           output.ordinal});
    }
    if (context.outputs[0].descriptor_id == context.outputs[1].descriptor_id ||
        context.outputs[0].descriptor_id == context.outputs[2].descriptor_id ||
        context.outputs[1].descriptor_id == context.outputs[2].descriptor_id ||
        descriptor_by_id.at(context.outputs[1].descriptor_id)->type_uuid !=
            descriptor_by_id.at(context.outputs[2].descriptor_id)->type_uuid) {
      return refuse_vector("SB_MODEL_BINDING_INCOMPLETE_V1",
                           "vector public descriptor handles are duplicated or type-inconsistent");
    }

    std::unordered_map<std::uint32_t, std::uint32_t> ast_to_bound;
    std::size_t binding_index = 3;
    for (const auto& ast_expression : ast.expressions) {
      if (ast_expression.expression_kind == NativeExpressionAstKind::kWildcard) {
        continue;
      }
      const auto& expression = context.expressions[binding_index++];
      if (expression.expression_id != ast_expression.expression_id ||
          !descriptor_by_id.contains(expression.descriptor_id) ||
          (expression.function_uuid.has_value() &&
           (ast_expression.operator_name == "VECTOR_NEAREST" ||
            ast_expression.operator_name == "VECTOR_FILTER")) ||
          (expression.function_uuid.has_value() &&
           !IsNonNullCanonicalUuid(*expression.function_uuid)) ||
          (expression.bound_name_uuid.has_value() &&
           !IsNonNullCanonicalUuid(*expression.bound_name_uuid)) ||
          !ast_to_bound.emplace(ast_expression.expression_id,
                                expression.expression_id).second) {
        return refuse_vector("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                             "vector expression binding is invalid");
      }
      BoundExpressionAstRecord record;
      record.expression_id = expression.expression_id;
      record.expression_kind = ast_expression.expression_kind;
      record.literal_kind = ast_expression.literal_kind;
      record.result_descriptor_id = expression.descriptor_id;
      record.bound_function_uuid = expression.function_uuid;
      record.bound_name_uuid = expression.bound_name_uuid;
      if (!ast_expression.operator_name.empty()) {
        record.canonical_operator_name = ast_expression.operator_name;
      }
      if (ast_expression.expression_kind == NativeExpressionAstKind::kLiteral ||
          ast_expression.expression_kind == NativeExpressionAstKind::kParameter) {
        record.literal_or_parameter_ref = ast_expression.spelling;
      }
      for (const auto child : ast_expression.child_expression_ids) {
        const auto mapped = ast_to_bound.find(child);
        if (mapped == ast_to_bound.end()) {
          return refuse_vector("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                               "vector expression child is unreachable/reordered");
        }
        record.child_expression_ids.push_back(mapped->second);
      }
      bound.expressions.push_back(std::move(record));
    }
    const auto mapped_id = [&](const std::optional<std::uint32_t> id)
        -> std::optional<std::uint32_t> {
      if (!id.has_value()) return std::nullopt;
      const auto found = ast_to_bound.find(*id);
      return found == ast_to_bound.end()
                 ? std::nullopt
                 : std::optional<std::uint32_t>(found->second);
    };
    const auto mapped_alias = mapped_id(
        vector_source_ast->model_vector_alias_expression_id);
    const auto mapped_query = mapped_id(
        vector_source_ast->model_vector_query_expression_id);
    const auto mapped_metric = mapped_id(
        vector_source_ast->model_vector_metric_expression_id);
    const auto mapped_top_k = mapped_id(
        vector_source_ast->model_vector_top_k_expression_id);
    const auto find_bound = [&](const std::optional<std::uint32_t> id)
        -> const BoundExpressionAstRecord* {
      if (!id.has_value()) return nullptr;
      const auto found = std::ranges::find_if(
          bound.expressions, [&](const auto& expression) {
            return expression.expression_id == *id;
          });
      return found == bound.expressions.end() ? nullptr : &*found;
    };
    const auto* bound_alias = find_bound(mapped_alias);
    const auto* bound_query = find_bound(mapped_query);
    const auto* bound_metric = find_bound(mapped_metric);
    const auto* bound_top_k = find_bound(mapped_top_k);
    if (bound_alias == nullptr || bound_query == nullptr ||
        bound_metric == nullptr || bound_top_k == nullptr ||
        bound_alias->bound_name_uuid != resolution.object_uuid ||
        bound_query->result_descriptor_id != resolution.columns[0].descriptor_id ||
        descriptor_by_id.at(bound_query->result_descriptor_id)->nullability !=
            BoundNullability::kNonNull ||
        descriptor_by_id.at(bound_metric->result_descriptor_id)->nullability !=
            BoundNullability::kNonNull ||
        descriptor_by_id.at(bound_metric->result_descriptor_id)
                ->canonical_type_name != "text" ||
        descriptor_by_id.at(bound_top_k->result_descriptor_id)->nullability !=
            BoundNullability::kNonNull ||
        descriptor_by_id.at(bound_top_k->result_descriptor_id)
                ->canonical_type_name != "uint64") {
      return refuse_vector("SB_MODEL_VECTOR_VALUE_REFUSED_V1",
                           "vector query/metric/top-k descriptor binding drifted");
    }
    if (filtered) {
      const auto* bound_column = find_bound(mapped_id(
          vector_source_ast->model_vector_metadata_column_expression_id));
      const auto* bound_value = find_bound(mapped_id(
          vector_source_ast->model_vector_metadata_value_expression_id));
      if (bound_column == nullptr || bound_value == nullptr ||
          bound_column->bound_name_uuid != resolution.columns[1].column_uuid ||
          bound_column->result_descriptor_id != resolution.columns[1].descriptor_id ||
          descriptor_by_id.at(bound_value->result_descriptor_id)->type_uuid !=
              metadata_descriptor->type_uuid ||
          descriptor_by_id.at(bound_value->result_descriptor_id)
                  ->canonical_type_name != "text" ||
          descriptor_by_id.at(bound_value->result_descriptor_id)->nullability !=
              BoundNullability::kNonNull) {
        return refuse_vector("SB_MODEL_VECTOR_FILTER_REFUSED_V1",
                             "vector metadata filter descriptor binding drifted");
      }
    }

    BoundCatalogRelationSourceAstRecord bound_source;
    bound_source.source_id = vector_source_ast->source_id;
    bound_source.source_kind = vector_source_ast->source_kind;
    bound_source.resolution_state =
        NativeCatalogRelationResolutionState::kBound;
    bound_source.qualified_name = vector_source_ast->qualified_name;
    bound_source.alias = vector_source_ast->alias;
    bound_source.alias_is_explicit = vector_source_ast->alias_is_explicit;
    bound_source.model_family_id = vector_source_ast->model_family_id;
    bound_source.model_operation_id = vector_source_ast->model_operation_id;
    bound_source.model_vector_alias_expression_id = mapped_alias;
    bound_source.model_vector_nearest_expression_id = mapped_id(
        vector_source_ast->model_vector_nearest_expression_id);
    bound_source.model_vector_query_expression_id = mapped_query;
    bound_source.model_vector_metric_expression_id = mapped_metric;
    bound_source.model_vector_top_k_expression_id = mapped_top_k;
    bound_source.model_vector_filter_expression_id = mapped_id(
        vector_source_ast->model_vector_filter_expression_id);
    bound_source.model_vector_metadata_predicate_expression_id = mapped_id(
        vector_source_ast->model_vector_metadata_predicate_expression_id);
    bound_source.model_vector_metadata_column_expression_id = mapped_id(
        vector_source_ast->model_vector_metadata_column_expression_id);
    bound_source.model_vector_metadata_value_expression_id = mapped_id(
        vector_source_ast->model_vector_metadata_value_expression_id);
    bound_source.model_vector_result_alias =
        vector_source_ast->model_vector_result_alias;
    bound_source.model_vector_metric_id =
        vector_source_ast->model_vector_metric_id;
    bound_source.model_vector_top_k = vector_source_ast->model_vector_top_k;
    bound_source.qualified_name_range = vector_source_ast->qualified_name_range;
    bound_source.range = vector_source_ast->range;
    bound_source.object_uuid = resolution.object_uuid;
    bound_source.resolved_object_type = resolution.resolved_object_type;
    bound_source.resolved_schema_uuid = resolution.resolved_schema_uuid;
    bound_source.parent_object_uuid = resolution.parent_object_uuid;
    bound_source.catalog_generation_id = resolution.catalog_generation_id;
    bound_source.security_epoch = resolution.security_epoch;
    bound_source.resource_epoch = resolution.resource_epoch;
    for (const auto& column : resolution.columns) {
      bound_source.columns.push_back(
          {column.ordinal, column.column_uuid, column.descriptor_id,
           column.canonical_name_key});
    }
    const auto root = ast_to_bound.find(
        ast.relations.front().predicate_expression_ids.front());
    if (root == ast_to_bound.end()) {
      return refuse_vector("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                           "vector predicate root is unreachable");
    }
    bound_relation.predicate_expression_ids = {root->second};
    bound_relation.bound_expression_ids = bound_relation.output_expression_ids;
    for (const auto& ast_expression : ast.expressions) {
      if (ast_expression.expression_kind == NativeExpressionAstKind::kWildcard) {
        continue;
      }
      const auto mapped = ast_to_bound.find(ast_expression.expression_id);
      if (mapped == ast_to_bound.end()) {
        return refuse_vector("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                             "vector bound-expression order is incomplete");
      }
      bound_relation.bound_expression_ids.push_back(mapped->second);
    }
    bound.catalog_relation_sources.push_back(std::move(bound_source));
    bound.relations.push_back(std::move(bound_relation));
    BoundScopeAstRecord scope;
    scope.scope_id = 1;
    scope.visible_relation_ids = {ast.root_relation_id};
    for (const auto& output : bound.outputs) {
      if (output.visible) scope.visible_projection_ids.push_back(output.output_id);
    }
    scope.catalog_epoch_uuid = context.catalog_epoch_uuid;
    bound.scopes.push_back(std::move(scope));
    bound.bound_ast_uuid = context.bound_ast_uuid;
    bound.security_context_uuid = context.security_context_uuid;
    bound.root_relation_id = ast.root_relation_id;
    bound.root_scope_id = 1;
    bound.bound = true;
    return bound;
  }
  if (time_series_source_ast != ast.catalog_relation_sources.end()) {
    // QOW-SOURCE-RCP-076-TIME-SERIES-BINDING-V1
    const auto refuse_time_series = [&](const char* diagnostic,
                                        const char* detail) {
      AddBoundAstDiagnostic(&bound, diagnostic, detail);
      return RefusedBoundAst(std::move(bound));
    };
    const bool range_read =
        time_series_source_ast->model_operation_id ==
        "TIME_SERIES_RANGE_READ";
    const bool downsample =
        time_series_source_ast->model_operation_id ==
        "TIME_SERIES_DOWNSAMPLE";
    const bool bucket_projection =
        time_series_source_ast->model_bucket_expression_id.has_value();
    const std::string_view expected_semantic =
        downsample ? "sblr.model-aggregate.time-series-downsample.v1"
                   : "sblr.model-source.time-series-range-read.v1";
    if (!IsCanonicalStatementTimestamp(context.statement_timestamp)) {
      return refuse_time_series(
          "SB_MODEL_TIME_SERIES_TIMESTAMP_INVALID_V1",
          "time-series binding requires the canonical engine statement timestamp");
    }
    if (ast.catalog_relation_sources.size() != 1 || ast.relations.size() != 1 ||
        ast.root_relation_id != ast.relations.front().relation_id ||
        ast.relations.front().relation_kind !=
            NativeRelationAstKind::kCatalogSource ||
        ast.relations.front().relation_source_ids !=
            std::vector<std::uint32_t>{time_series_source_ast->source_id} ||
        ast.relations.front().predicate_expression_ids !=
            std::vector<std::uint32_t>{
                time_series_source_ast->model_range_expression_id.value_or(0)} ||
        time_series_source_ast->model_family_id != "time_series" ||
        (!range_read && !downsample) ||
        time_series_source_ast->qualified_name.empty() ||
        !time_series_source_ast->alias.has_value() ||
        !time_series_source_ast->model_time_series_alias_expression_id.has_value() ||
        !time_series_source_ast->model_range_expression_id.has_value() ||
        !time_series_source_ast->model_range_start_expression_id.has_value() ||
        !time_series_source_ast->model_range_end_expression_id.has_value() ||
        bucket_projection !=
            time_series_source_ast->model_bucket_interval_expression_id.has_value() ||
        bucket_projection !=
            time_series_source_ast->model_bucket_time_input_expression_id.has_value() ||
        (downsample &&
         (!time_series_source_ast->model_downsample_expression_id.has_value() ||
          !time_series_source_ast->model_interval_expression_id.has_value() ||
          !time_series_source_ast->model_time_input_expression_id.has_value() ||
          time_series_source_ast->model_time_series_aggregate_id.empty())) ||
        ast.model_object_resolution_requests.size() != 1 ||
        context.catalog_relations.size() != 1 || context.relations.size() != 1 ||
        context.relations.front().relation_id !=
            ast.relations.front().relation_id ||
        context.relations.front().semantic_variant_id != expected_semantic) {
      return refuse_time_series("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                                "time-series AST or binding authority is incomplete");
    }
    const auto& request = ast.model_object_resolution_requests.front();
    if (request.source_id != time_series_source_ast->source_id ||
        request.model_family_id != "time_series" ||
        request.object_class != "time_series" ||
        request.qualified_name.size() !=
            time_series_source_ast->qualified_name.size() ||
        !std::ranges::equal(
            request.qualified_name, time_series_source_ast->qualified_name,
            [](const auto& left, const auto& right) {
              return left.spelling == right.spelling &&
                     left.quoted == right.quoted;
            })) {
      return refuse_time_series("SB_MODEL_BINDING_INCOMPLETE_V1",
                                "time-series resolution request was substituted");
    }
    const auto& resolution = context.catalog_relations.front();
    static constexpr std::string_view kRawNames[] = {
        "row_uuid", "series_uuid", "metric_uuid", "point_timestamp",
        "tags", "value"};
    bool exact_projection = resolution.columns.size() == 6;
    for (std::size_t ordinal = 0;
         exact_projection && ordinal < resolution.columns.size(); ++ordinal) {
      exact_projection = resolution.columns[ordinal].ordinal == ordinal &&
                         resolution.columns[ordinal].canonical_name_key ==
                             kRawNames[ordinal];
    }
    if (resolution.source_id != time_series_source_ast->source_id ||
        resolution.resolution_state !=
            NativeCatalogRelationResolutionState::kBound ||
        !IsNonNullCanonicalUuid(resolution.object_uuid) ||
        !IsNonNullCanonicalUuid(resolution.resolved_schema_uuid) ||
        resolution.resolved_object_type != "time_series" ||
        resolution.catalog_generation_id == 0 || resolution.security_epoch == 0 ||
        resolution.resource_epoch == 0 || !exact_projection) {
      return refuse_time_series("SB_MODEL_BINDING_INCOMPLETE_V1",
                                "time-series raw storage projection is incomplete");
    }
    const auto ast_expression_by_id = [&](const std::uint32_t expression_id)
        -> const NativeExpressionAstNode* {
      const auto found = std::ranges::find_if(
          ast.expressions, [&](const auto& expression) {
            return expression.expression_id == expression_id;
          });
      return found == ast.expressions.end() ? nullptr : &*found;
    };
    const auto* range_expression = ast_expression_by_id(
        *time_series_source_ast->model_range_expression_id);
    if (range_expression == nullptr ||
        range_expression->expression_kind !=
            NativeExpressionAstKind::kFunctionCall ||
        range_expression->operator_name != "TIME_RANGE" ||
        range_expression->child_expression_ids !=
            std::vector<std::uint32_t>{
                *time_series_source_ast->model_time_series_alias_expression_id,
                *time_series_source_ast->model_range_start_expression_id,
                *time_series_source_ast->model_range_end_expression_id}) {
      return refuse_time_series("SB_MODEL_TIME_SERIES_RANGE_INVALID_V1",
                                "TIME_RANGE child identity was substituted");
    }
    for (const auto& descriptor : context.descriptors) {
      bound.descriptors.push_back(
          {descriptor.descriptor_id, descriptor.descriptor_uuid,
           descriptor.type_uuid, descriptor.nullability,
           descriptor.collation_uuid, descriptor.timezone_profile_id,
           descriptor.width_precision_scale});
    }
    std::ranges::sort(bound.descriptors, {},
                      &BoundDescriptorAstRecord::descriptor_id);

    const bool wildcard_projection = std::ranges::any_of(
        ast.relations.front().output_expression_ids,
        [&](const auto expression_id) {
          const auto* expression = ast_expression_by_id(expression_id);
          return expression != nullptr &&
                 expression->expression_kind == NativeExpressionAstKind::kWildcard;
        });
    if (downsample && wildcard_projection) {
      return refuse_time_series("SB_MODEL_TIME_SERIES_AGGREGATE_REFUSED_V1",
                                "downsample requires its exact derived projection");
    }
    const std::size_t wildcard_count = wildcard_projection ? 6 : 0;
    const std::size_t derived_downsample_count = downsample ? 7 : 0;
    const std::size_t bucket_source_descriptor_count =
        bucket_projection && !downsample ? 4 : 0;
    const auto non_wildcard_ast_count =
        ast.expressions.size() - static_cast<std::size_t>(wildcard_projection);
    if (context.expressions.size() != wildcard_count + non_wildcard_ast_count +
                                          derived_downsample_count +
                                          bucket_source_descriptor_count ||
        context.outputs.empty() ||
        context.outputs.size() !=
            (downsample
                 ? derived_downsample_count
                 : wildcard_projection
                       ? wildcard_count
                       : ast.relations.front().output_expression_ids.size())) {
      return refuse_time_series("SB_MODEL_BINDING_INCOMPLETE_V1",
                                "time-series expression/output binding is incomplete");
    }

    BoundCatalogRelationSourceAstRecord bound_source;
    bound_source.source_id = time_series_source_ast->source_id;
    bound_source.source_kind = time_series_source_ast->source_kind;
    bound_source.resolution_state =
        NativeCatalogRelationResolutionState::kBound;
    bound_source.qualified_name = time_series_source_ast->qualified_name;
    bound_source.alias = time_series_source_ast->alias;
    bound_source.alias_is_explicit = time_series_source_ast->alias_is_explicit;
    bound_source.model_family_id = "time_series";
    bound_source.model_operation_id =
        time_series_source_ast->model_operation_id;
    bound_source.model_time_series_aggregate_id =
        time_series_source_ast->model_time_series_aggregate_id;
    bound_source.qualified_name_range =
        time_series_source_ast->qualified_name_range;
    bound_source.range = time_series_source_ast->range;
    bound_source.object_uuid = resolution.object_uuid;
    bound_source.resolved_object_type = resolution.resolved_object_type;
    bound_source.resolved_schema_uuid = resolution.resolved_schema_uuid;
    bound_source.parent_object_uuid = resolution.parent_object_uuid;
    bound_source.catalog_generation_id = resolution.catalog_generation_id;
    bound_source.security_epoch = resolution.security_epoch;
    bound_source.resource_epoch = resolution.resource_epoch;
    for (const auto& column : resolution.columns) {
      if (!IsNonNullCanonicalUuid(column.column_uuid) ||
          !descriptor_by_id.contains(column.descriptor_id)) {
        return refuse_time_series("SB_MODEL_BINDING_INCOMPLETE_V1",
                                  "time-series column binding is incomplete");
      }
      bound_source.columns.push_back(
          {column.ordinal, column.column_uuid, column.descriptor_id,
           column.canonical_name_key});
    }

    BoundRelationAstRecord bound_relation;
    bound_relation.relation_id = ast.relations.front().relation_id;
    bound_relation.relation_kind = downsample
                                       ? NativeRelationAstKind::kAggregate
                                       : NativeRelationAstKind::kCatalogSource;
    bound_relation.semantic_variant_id = std::string(expected_semantic);
    bound_relation.bound_object_uuid = resolution.object_uuid;
    std::unordered_map<std::uint32_t, std::uint32_t> ast_to_bound;
    std::size_t binding_index = 0;
    if (wildcard_projection) {
      for (std::size_t ordinal = 0; ordinal < wildcard_count; ++ordinal) {
        const auto& expression = context.expressions[binding_index++];
        const auto& column = resolution.columns[ordinal];
        if (!descriptor_by_id.contains(expression.descriptor_id) ||
            expression.descriptor_id != column.descriptor_id ||
            expression.bound_name_uuid != column.column_uuid) {
          return refuse_time_series("SB_MODEL_BINDING_INCOMPLETE_V1",
                                    "time-series wildcard projection is not exact");
        }
        BoundExpressionAstRecord record;
        record.expression_id = expression.expression_id;
        record.expression_kind = NativeExpressionAstKind::kIdentifier;
        record.result_descriptor_id = expression.descriptor_id;
        record.bound_name_uuid = expression.bound_name_uuid;
        bound.expressions.push_back(std::move(record));
        bound_relation.output_expression_ids.push_back(expression.expression_id);
      }
    }
    for (const auto& ast_expression : ast.expressions) {
      if (ast_expression.expression_kind == NativeExpressionAstKind::kWildcard) {
        continue;
      }
      const auto& expression = context.expressions[binding_index++];
      const bool functionless_time_series =
          ast_expression.operator_name == "TIME_RANGE" ||
          ast_expression.operator_name == "TIME_BUCKET" ||
          ast_expression.operator_name == "TIME_DOWNSAMPLE";
      if (!descriptor_by_id.contains(expression.descriptor_id) ||
          (functionless_time_series && expression.function_uuid.has_value()) ||
          (expression.function_uuid.has_value() &&
           !IsNonNullCanonicalUuid(*expression.function_uuid)) ||
          (expression.bound_name_uuid.has_value() &&
           !IsNonNullCanonicalUuid(*expression.bound_name_uuid))) {
        return refuse_time_series("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                                  "time-series typed expression was substituted");
      }
      ast_to_bound.emplace(ast_expression.expression_id,
                           expression.expression_id);
      BoundExpressionAstRecord record;
      record.expression_id = expression.expression_id;
      record.expression_kind = ast_expression.expression_kind;
      record.literal_kind = ast_expression.literal_kind;
      record.result_descriptor_id = expression.descriptor_id;
      record.bound_function_uuid = expression.function_uuid;
      record.bound_name_uuid = expression.bound_name_uuid;
      if (!ast_expression.operator_name.empty()) {
        record.canonical_operator_name = ast_expression.operator_name;
      }
      if (ast_expression.expression_kind == NativeExpressionAstKind::kLiteral ||
          ast_expression.expression_kind == NativeExpressionAstKind::kParameter) {
        record.literal_or_parameter_ref = ast_expression.spelling;
      }
      for (const auto child : ast_expression.child_expression_ids) {
        const auto mapped = ast_to_bound.find(child);
        if (mapped == ast_to_bound.end()) {
          return refuse_time_series("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                                    "time-series expression child is unreachable");
        }
        record.child_expression_ids.push_back(mapped->second);
      }
      bound.expressions.push_back(std::move(record));
    }
    std::vector<std::uint32_t> bucket_source_expression_ids;
    if (bucket_projection && !downsample) {
      static constexpr std::array<std::size_t, 4> kColumnOrdinals{1, 2, 4, 5};
      for (const auto ordinal : kColumnOrdinals) {
        const auto& expression = context.expressions[binding_index++];
        const auto& column = resolution.columns[ordinal];
        if (expression.expression_id == 0 ||
            expression.descriptor_id != column.descriptor_id ||
            expression.function_uuid.has_value() ||
            expression.bound_name_uuid != column.column_uuid) {
          return refuse_time_series(
              "SB_MODEL_BINDING_INCOMPLETE_V1",
              "time-series bucket source descriptor was substituted");
        }
        BoundExpressionAstRecord record;
        record.expression_id = expression.expression_id;
        record.expression_kind = NativeExpressionAstKind::kIdentifier;
        record.result_descriptor_id = expression.descriptor_id;
        record.bound_name_uuid = expression.bound_name_uuid;
        bound.expressions.push_back(std::move(record));
        bucket_source_expression_ids.push_back(expression.expression_id);
      }
    }
    std::vector<std::uint32_t> downsample_output_expression_ids;
    if (downsample) {
      static constexpr std::array<std::string_view, 7> kNames{
          "series_uuid", "metric_uuid", "bucket_start", "bucket_end",
          "tags", "sample_count", "aggregate_value"};
      const std::array<std::uint32_t, 5> source_descriptor_ids{
          resolution.columns[1].descriptor_id,
          resolution.columns[2].descriptor_id,
          resolution.columns[3].descriptor_id,
          resolution.columns[4].descriptor_id,
          resolution.columns[5].descriptor_id};
      const auto sample_count_descriptor_id = context.outputs[5].descriptor_id;
      const std::array<std::uint32_t, 7> expected_descriptor_ids{
          source_descriptor_ids[0], source_descriptor_ids[1],
          source_descriptor_ids[2], context.outputs[3].descriptor_id,
          source_descriptor_ids[3], sample_count_descriptor_id,
          context.outputs[6].descriptor_id};
      const std::array<std::string, 7> expected_bound_names{
          resolution.columns[1].column_uuid,
          resolution.columns[2].column_uuid,
          resolution.columns[3].column_uuid,
          resolution.columns[3].column_uuid,
          resolution.columns[4].column_uuid, resolution.object_uuid,
          time_series_source_ast->model_time_series_aggregate_id == "COUNT"
              ? resolution.object_uuid
              : resolution.columns[5].column_uuid};
      const auto count_descriptor = descriptor_by_id.find(
          sample_count_descriptor_id);
      const auto bucket_end_descriptor =
          descriptor_by_id.find(context.outputs[3].descriptor_id);
      const auto aggregate_descriptor =
          descriptor_by_id.find(context.outputs[6].descriptor_id);
      if (count_descriptor == descriptor_by_id.end() ||
          bucket_end_descriptor == descriptor_by_id.end() ||
          aggregate_descriptor == descriptor_by_id.end() ||
          count_descriptor->second->nullability != BoundNullability::kNonNull ||
          bucket_end_descriptor->second->nullability !=
              BoundNullability::kNonNull ||
          aggregate_descriptor->second->nullability !=
              BoundNullability::kNonNull ||
          bucket_end_descriptor->second->type_uuid !=
              descriptor_by_id.at(source_descriptor_ids[2])->type_uuid ||
          aggregate_descriptor->second->type_uuid !=
              (time_series_source_ast->model_time_series_aggregate_id == "COUNT"
                   ? count_descriptor->second->type_uuid
                   : descriptor_by_id.at(source_descriptor_ids[4])->type_uuid) ||
          context.outputs[3].descriptor_id == source_descriptor_ids[2] ||
          (time_series_source_ast->model_time_series_aggregate_id == "COUNT" &&
           context.outputs[6].descriptor_id == sample_count_descriptor_id) ||
          count_descriptor->second->type_uuid ==
              descriptor_by_id.at(source_descriptor_ids[0])->type_uuid ||
          count_descriptor->second->type_uuid ==
              descriptor_by_id.at(source_descriptor_ids[2])->type_uuid ||
          count_descriptor->second->type_uuid ==
              descriptor_by_id.at(source_descriptor_ids[3])->type_uuid ||
          count_descriptor->second->type_uuid ==
              descriptor_by_id.at(source_descriptor_ids[4])->type_uuid) {
        return refuse_time_series("SB_MODEL_BINDING_INCOMPLETE_V1",
                                  "time-series sample-count descriptor is invalid");
      }
      for (std::size_t ordinal = 0; ordinal < kNames.size(); ++ordinal) {
        const auto& expression = context.expressions[binding_index++];
        const auto& output = context.outputs[ordinal];
        if (expression.expression_id == 0 ||
            expression.descriptor_id != expected_descriptor_ids[ordinal] ||
            expression.function_uuid.has_value() ||
            expression.bound_name_uuid != expected_bound_names[ordinal] ||
            output.output_name_utf8 != kNames[ordinal] ||
            output.expression_id != expression.expression_id ||
            output.descriptor_id != expression.descriptor_id) {
          return refuse_time_series(
              "SB_MODEL_BINDING_INCOMPLETE_V1",
              "time-series derived downsample descriptor was substituted");
        }
        BoundExpressionAstRecord record;
        record.expression_id = expression.expression_id;
        record.expression_kind = NativeExpressionAstKind::kIdentifier;
        record.result_descriptor_id = expression.descriptor_id;
        record.bound_name_uuid = expression.bound_name_uuid;
        bound.expressions.push_back(std::move(record));
        downsample_output_expression_ids.push_back(expression.expression_id);
      }
    }
    if (binding_index != context.expressions.size()) {
      return refuse_time_series("SB_MODEL_BINDING_INCOMPLETE_V1",
                                "time-series expression cohort has extra bindings");
    }
    const auto mapped_id = [&](const std::optional<std::uint32_t> id)
        -> std::optional<std::uint32_t> {
      if (!id.has_value()) return std::nullopt;
      const auto found = ast_to_bound.find(*id);
      return found == ast_to_bound.end() ? std::nullopt
                                        : std::optional(found->second);
    };
    bound_source.model_time_series_alias_expression_id =
        mapped_id(time_series_source_ast->model_time_series_alias_expression_id);
    bound_source.model_range_expression_id =
        mapped_id(time_series_source_ast->model_range_expression_id);
    bound_source.model_range_start_expression_id =
        mapped_id(time_series_source_ast->model_range_start_expression_id);
    bound_source.model_range_end_expression_id =
        mapped_id(time_series_source_ast->model_range_end_expression_id);
    bound_source.model_interval_expression_id =
        mapped_id(time_series_source_ast->model_interval_expression_id);
    bound_source.model_time_input_expression_id =
        mapped_id(time_series_source_ast->model_time_input_expression_id);
    bound_source.model_bucket_expression_id =
        mapped_id(time_series_source_ast->model_bucket_expression_id);
    bound_source.model_bucket_interval_expression_id =
        mapped_id(time_series_source_ast->model_bucket_interval_expression_id);
    bound_source.model_bucket_time_input_expression_id =
        mapped_id(time_series_source_ast->model_bucket_time_input_expression_id);
    bound_source.model_downsample_expression_id =
        mapped_id(time_series_source_ast->model_downsample_expression_id);
    if (!bound_source.model_time_series_alias_expression_id.has_value() ||
        !bound_source.model_range_expression_id.has_value() ||
        !bound_source.model_range_start_expression_id.has_value() ||
        !bound_source.model_range_end_expression_id.has_value() ||
        bucket_projection != bound_source.model_bucket_expression_id.has_value() ||
        bucket_projection !=
            bound_source.model_bucket_interval_expression_id.has_value() ||
        bucket_projection !=
            bound_source.model_bucket_time_input_expression_id.has_value() ||
        (downsample &&
         (!bound_source.model_interval_expression_id.has_value() ||
          !bound_source.model_time_input_expression_id.has_value() ||
          !bound_source.model_downsample_expression_id.has_value()))) {
      return refuse_time_series("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                                "time-series operation mapping is incomplete");
    }
    const auto bound_expression_by_id = [&](const std::uint32_t id)
        -> const BoundExpressionAstRecord* {
      const auto found = std::ranges::find_if(
          bound.expressions,
          [&](const auto& expression) { return expression.expression_id == id; });
      return found == bound.expressions.end() ? nullptr : &*found;
    };
    const auto* point_timestamp_descriptor =
        descriptor_by_id.at(resolution.columns[3].descriptor_id);
    const auto* value_descriptor =
        descriptor_by_id.at(resolution.columns[5].descriptor_id);
    for (const auto id : {*bound_source.model_range_start_expression_id,
                          *bound_source.model_range_end_expression_id}) {
      const auto* expression = bound_expression_by_id(id);
      const auto* descriptor = expression == nullptr
                                   ? nullptr
                                   : descriptor_by_id.at(
                                         expression->result_descriptor_id);
      if (descriptor == nullptr ||
          descriptor->type_uuid != point_timestamp_descriptor->type_uuid ||
          descriptor->nullability != BoundNullability::kNonNull) {
        return refuse_time_series("SB_MODEL_TIME_SERIES_TIMESTAMP_INVALID_V1",
                                  "TIME_RANGE endpoint is not non-null TIMESTAMP_TZ");
      }
    }
    if (bucket_projection) {
      const auto* operation =
          bound_expression_by_id(*bound_source.model_bucket_expression_id);
      const auto* interval = bound_expression_by_id(
          *bound_source.model_bucket_interval_expression_id);
      const auto* input = bound_expression_by_id(
          *bound_source.model_bucket_time_input_expression_id);
      const auto* interval_descriptor =
          interval == nullptr
              ? nullptr
              : descriptor_by_id.at(interval->result_descriptor_id);
      const auto* input_descriptor =
          input == nullptr ? nullptr
                           : descriptor_by_id.at(input->result_descriptor_id);
      if (operation == nullptr || interval == nullptr || input == nullptr ||
          operation->expression_kind !=
              NativeExpressionAstKind::kFunctionCall ||
          operation->bound_function_uuid.has_value() ||
          operation->canonical_operator_name != "TIME_BUCKET" ||
          operation->child_expression_ids !=
              std::vector<std::uint32_t>{
                  *bound_source.model_bucket_interval_expression_id,
                  *bound_source.model_bucket_time_input_expression_id} ||
          interval_descriptor == nullptr ||
          interval_descriptor->nullability != BoundNullability::kNonNull ||
          interval_descriptor->type_uuid == point_timestamp_descriptor->type_uuid ||
          interval_descriptor->type_uuid == value_descriptor->type_uuid ||
          input_descriptor == nullptr ||
          input_descriptor->type_uuid != point_timestamp_descriptor->type_uuid ||
          input_descriptor->nullability != BoundNullability::kNonNull ||
          input->expression_kind != NativeExpressionAstKind::kIdentifier ||
          input->bound_name_uuid != resolution.columns[3].column_uuid) {
        return refuse_time_series(
            "SB_MODEL_TIME_SERIES_INTERVAL_INVALID_V1",
            "TIME_BUCKET interval or persistent point_timestamp binding is invalid");
      }
      if (downsample) {
        const auto* downsample_interval = bound_expression_by_id(
            *bound_source.model_interval_expression_id);
        std::int64_t bucket_interval_ns = 0;
        std::int64_t downsample_interval_ns = 0;
        if (downsample_interval == nullptr ||
            !interval->literal_or_parameter_ref.has_value() ||
            !downsample_interval->literal_or_parameter_ref.has_value() ||
            !ParseFixedTimeSeriesIntervalNs(
                *interval->literal_or_parameter_ref, &bucket_interval_ns) ||
            !ParseFixedTimeSeriesIntervalNs(
                *downsample_interval->literal_or_parameter_ref,
                &downsample_interval_ns) ||
            bucket_interval_ns != downsample_interval_ns) {
          return refuse_time_series(
              "SB_MODEL_TIME_SERIES_INTERVAL_INVALID_V1",
              "TIME_BUCKET and TIME_DOWNSAMPLE evaluated intervals differ");
        }
      }
    }
    if (bound_source.model_time_input_expression_id.has_value()) {
      const auto* input =
          bound_expression_by_id(*bound_source.model_time_input_expression_id);
      const auto* descriptor = input == nullptr
                                   ? nullptr
                                   : descriptor_by_id.at(input->result_descriptor_id);
      const auto* expected = downsample ? value_descriptor
                                        : point_timestamp_descriptor;
      if (descriptor == nullptr || descriptor->type_uuid != expected->type_uuid ||
          descriptor->nullability != BoundNullability::kNonNull) {
        return refuse_time_series(
            downsample ? "SB_MODEL_TIME_SERIES_AGGREGATE_REFUSED_V1"
                       : "SB_MODEL_TIME_SERIES_TIMESTAMP_INVALID_V1",
            "time-series operation input descriptor is invalid");
      }
    }
    if (bound_source.model_interval_expression_id.has_value()) {
      const auto* interval =
          bound_expression_by_id(*bound_source.model_interval_expression_id);
      const auto* descriptor = interval == nullptr
                                   ? nullptr
                                   : descriptor_by_id.at(interval->result_descriptor_id);
      if (descriptor == nullptr ||
          descriptor->nullability != BoundNullability::kNonNull ||
          descriptor->type_uuid == point_timestamp_descriptor->type_uuid ||
          descriptor->type_uuid == value_descriptor->type_uuid) {
        return refuse_time_series("SB_MODEL_TIME_SERIES_INTERVAL_INVALID_V1",
                                  "time-series interval descriptor is invalid");
      }
    }
    const auto mapped_range =
        *bound_source.model_range_expression_id;
    bound_relation.predicate_expression_ids = {mapped_range};
    if (downsample) {
      bound_relation.output_expression_ids = downsample_output_expression_ids;
    } else if (!wildcard_projection) {
      for (const auto expression_id :
           ast.relations.front().output_expression_ids) {
        const auto mapped = ast_to_bound.find(expression_id);
        if (mapped == ast_to_bound.end()) {
          return refuse_time_series("SB_MODEL_BINDING_INCOMPLETE_V1",
                                    "time-series projection mapping is incomplete");
        }
        bound_relation.output_expression_ids.push_back(mapped->second);
      }
    }
    bound_relation.bound_expression_ids = bound_relation.output_expression_ids;
    bound_relation.bound_expression_ids.insert(
        bound_relation.bound_expression_ids.end(),
        bucket_source_expression_ids.begin(), bucket_source_expression_ids.end());
    if (downsample && bucket_projection) {
      bound_relation.bound_expression_ids.push_back(
          *bound_source.model_bucket_expression_id);
    }
    if (downsample) {
      bound_relation.bound_expression_ids.push_back(
          *bound_source.model_downsample_expression_id);
    }
    bound_relation.bound_expression_ids.push_back(mapped_range);
    for (std::size_t ordinal = 0; ordinal < context.outputs.size(); ++ordinal) {
      const auto& output = context.outputs[ordinal];
      if (output.output_id == 0 ||
          !descriptor_by_id.contains(output.descriptor_id) ||
          output.relation_id != bound_relation.relation_id ||
          output.ordinal != ordinal ||
          output.expression_id != bound_relation.output_expression_ids[ordinal]) {
        return refuse_time_series("SB_MODEL_BINDING_INCOMPLETE_V1",
                                  "time-series output binding is invalid");
      }
      bound.outputs.push_back(
          {output.output_id, output.relation_id, output.expression_id,
           output.output_name_utf8, output.descriptor_id, output.visible,
           output.ordinal});
    }
    bound.catalog_relation_sources.push_back(std::move(bound_source));
    bound.relations.push_back(std::move(bound_relation));
    BoundScopeAstRecord scope;
    scope.scope_id = 1;
    scope.visible_relation_ids = {ast.root_relation_id};
    for (const auto& output : bound.outputs) {
      if (output.visible) scope.visible_projection_ids.push_back(output.output_id);
    }
    scope.catalog_epoch_uuid = context.catalog_epoch_uuid;
    bound.scopes.push_back(std::move(scope));
    bound.bound_ast_uuid = context.bound_ast_uuid;
    bound.security_context_uuid = context.security_context_uuid;
    bound.root_relation_id = ast.root_relation_id;
    bound.root_scope_id = 1;
    bound.bound = true;
    return bound;
  }
  if (key_value_source_ast != ast.catalog_relation_sources.end()) {
    // QOW-SOURCE-RCP-075-KEY-VALUE-BINDING-V1
    const auto refuse_key_value = [&](const char* diagnostic,
                                      const char* detail) {
      AddBoundAstDiagnostic(&bound, diagnostic, detail);
      return RefusedBoundAst(std::move(bound));
    };
    if (!IsCanonicalStatementTimestamp(context.statement_timestamp)) {
      return refuse_key_value(
          "SB_MODEL_KEY_VALUE_STATEMENT_TIMESTAMP_INVALID_V1",
          "key/value binding requires the exact canonical engine-issued "
          "statement timestamp");
    }
    const bool exact_get =
        key_value_source_ast->model_operation_id == "KEY_VALUE_GET";
    const bool multi_get =
        key_value_source_ast->model_operation_id == "KEY_VALUE_MULTI_GET";
    const bool prefix =
        key_value_source_ast->model_operation_id ==
        "KEY_VALUE_PREFIX_RANGE";
    const std::string_view expected_semantic =
        exact_get
            ? "sblr.model-source.key-value-get.v1"
            : (multi_get
                   ? "sblr.model-source.key-value-multi-get.v1"
                   : "sblr.model-source.key-value-prefix-range.v1");
    if (ast.catalog_relation_sources.size() != 1 || ast.relations.size() != 1 ||
        ast.root_relation_id != ast.relations.front().relation_id ||
        ast.relations.front().relation_kind !=
            NativeRelationAstKind::kCatalogSource ||
        ast.relations.front().relation_source_ids !=
            std::vector<std::uint32_t>{key_value_source_ast->source_id} ||
        ast.relations.front().predicate_expression_ids.size() != 1 ||
        key_value_source_ast->model_family_id != "key_value" ||
        (!exact_get && !multi_get && !prefix) ||
        key_value_source_ast->qualified_name.empty() ||
        !key_value_source_ast->alias.has_value() ||
        key_value_source_ast->model_key_expression_ids.empty() ||
        (!multi_get &&
         key_value_source_ast->model_key_expression_ids.size() != 1) ||
        ((exact_get &&
          key_value_source_ast->model_comparison_operator != "=") ||
         (!exact_get &&
          !key_value_source_ast->model_comparison_operator.empty())) ||
        ast.model_object_resolution_requests.size() != 1 ||
        context.catalog_relations.size() != 1 || context.relations.size() != 1 ||
        context.relations.front().relation_id !=
            ast.relations.front().relation_id ||
        context.relations.front().semantic_variant_id != expected_semantic) {
      return refuse_key_value(
          exact_get ? "SB_MODEL_KEY_VALUE_OPERATOR_REFUSED_V1"
                    : "SB_MODEL_BINDING_INCOMPLETE_V1",
          "key/value source AST or binding authority is incomplete");
    }
    const auto& request = ast.model_object_resolution_requests.front();
    if (request.source_id != key_value_source_ast->source_id ||
        request.model_family_id != "key_value" ||
        request.object_class != "key_value" ||
        request.qualified_name.size() !=
            key_value_source_ast->qualified_name.size() ||
        !std::ranges::equal(
            request.qualified_name, key_value_source_ast->qualified_name,
            [](const auto& left, const auto& right) {
              return left.spelling == right.spelling &&
                     left.quoted == right.quoted;
            })) {
      return refuse_key_value(
          "SB_MODEL_BINDING_INCOMPLETE_V1",
          "key/value resolution request does not match the source AST");
    }
    const auto& resolution = context.catalog_relations.front();
    if (resolution.source_id != key_value_source_ast->source_id ||
        resolution.resolution_state !=
            NativeCatalogRelationResolutionState::kBound ||
        !IsNonNullCanonicalUuid(resolution.object_uuid) ||
        !IsNonNullCanonicalUuid(resolution.resolved_schema_uuid) ||
        resolution.resolved_object_type != "key_value" ||
        resolution.catalog_generation_id == 0 || resolution.security_epoch == 0 ||
        resolution.resource_epoch == 0 || resolution.columns.size() != 3 ||
        resolution.columns[0].ordinal != 0 ||
        resolution.columns[0].canonical_name_key != "row_uuid" ||
        resolution.columns[1].ordinal != 1 ||
        resolution.columns[1].canonical_name_key != "key" ||
        resolution.columns[2].ordinal != 2 ||
        resolution.columns[2].canonical_name_key != "value") {
      return refuse_key_value(
          "SB_MODEL_BINDING_INCOMPLETE_V1",
          "key/value did not resolve to the exact three-field public projection");
    }

    const auto ast_expression_by_id = [&](const std::uint32_t expression_id)
        -> const NativeExpressionAstNode* {
      const auto found = std::ranges::find_if(
          ast.expressions, [&](const auto& expression) {
            return expression.expression_id == expression_id;
          });
      return found == ast.expressions.end() ? nullptr : &*found;
    };
    const auto root_expression_id =
        ast.relations.front().predicate_expression_ids.front();
    const auto* root_expression = ast_expression_by_id(root_expression_id);
    const NativeExpressionAstNode* operation_expression = root_expression;
    if (exact_get) {
      if (root_expression == nullptr ||
          root_expression->expression_kind != NativeExpressionAstKind::kBinary ||
          root_expression->operator_name != "=" ||
          root_expression->child_expression_ids.size() != 2 ||
          root_expression->child_expression_ids[1] !=
              key_value_source_ast->model_key_expression_ids.front()) {
        return refuse_key_value("SB_MODEL_KEY_VALUE_OPERATOR_REFUSED_V1",
                                "KV_KEY is not exact equality");
      }
      operation_expression = ast_expression_by_id(
          root_expression->child_expression_ids.front());
    }
    if (operation_expression == nullptr ||
        operation_expression->expression_kind !=
            NativeExpressionAstKind::kFunctionCall ||
        operation_expression->operator_name !=
            (exact_get ? "KV_KEY" : (multi_get ? "KV_MULTI_GET" : "KV_PREFIX")) ||
        operation_expression->child_expression_ids.empty()) {
      return refuse_key_value("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                              "key/value functionless operation is incomplete");
    }
    const auto* alias_expression = ast_expression_by_id(
        operation_expression->child_expression_ids.front());
    const auto same_alias = [](const NativeIdentifierAstNode& left,
                               const NativeIdentifierAstNode& right) {
      return left.quoted == right.quoted &&
             (left.quoted ? left.spelling == right.spelling
                          : ToUpperAscii(left.spelling) ==
                                ToUpperAscii(right.spelling));
    };
    if (alias_expression == nullptr ||
        alias_expression->expression_kind !=
            NativeExpressionAstKind::kIdentifier ||
        alias_expression->qualified_identifier.size() != 1 ||
        !same_alias(alias_expression->qualified_identifier.front(),
                    *key_value_source_ast->alias)) {
      return refuse_key_value("SB_MODEL_BINDING_INCOMPLETE_V1",
                              "key/value alias expression is incomplete");
    }
    std::vector<std::uint32_t> expected_operation_children{
        alias_expression->expression_id};
    if (!exact_get) {
      expected_operation_children.insert(
          expected_operation_children.end(),
          key_value_source_ast->model_key_expression_ids.begin(),
          key_value_source_ast->model_key_expression_ids.end());
    }
    std::unordered_set<std::uint32_t> unique_operation_children(
        operation_expression->child_expression_ids.begin(),
        operation_expression->child_expression_ids.end());
    std::unordered_set<std::uint32_t> unique_key_nodes(
        key_value_source_ast->model_key_expression_ids.begin(),
        key_value_source_ast->model_key_expression_ids.end());
    if (operation_expression->child_expression_ids !=
            expected_operation_children ||
        unique_operation_children.size() !=
            operation_expression->child_expression_ids.size() ||
        unique_key_nodes.size() !=
            key_value_source_ast->model_key_expression_ids.size()) {
      return refuse_key_value("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                              "key/value typed-DAG child identity is duplicated or reordered");
    }
    for (const auto expression_id :
         key_value_source_ast->model_key_expression_ids) {
      if (ast_expression_by_id(expression_id) == nullptr) {
        return refuse_key_value("SB_MODEL_BINDING_INCOMPLETE_V1",
                                "key/value key expression is unreachable");
      }
    }

    for (const auto& descriptor : context.descriptors) {
      bound.descriptors.push_back(
          {descriptor.descriptor_id, descriptor.descriptor_uuid,
           descriptor.type_uuid, descriptor.nullability,
           descriptor.collation_uuid, descriptor.timezone_profile_id,
           descriptor.width_precision_scale});
    }
    std::ranges::sort(bound.descriptors, {},
                      &BoundDescriptorAstRecord::descriptor_id);

    const bool wildcard_projection = std::ranges::any_of(
        ast.relations.front().output_expression_ids,
        [&](const auto expression_id) {
          const auto* expression = ast_expression_by_id(expression_id);
          return expression != nullptr &&
                 expression->expression_kind ==
                     NativeExpressionAstKind::kWildcard;
        });
    const auto wildcard_count = wildcard_projection ? std::size_t{3}
                                                    : std::size_t{0};
    const auto non_wildcard_ast_count =
        ast.expressions.size() - static_cast<std::size_t>(wildcard_projection);
    if (context.expressions.size() != wildcard_count + non_wildcard_ast_count ||
        context.outputs.empty() ||
        context.outputs.size() !=
            (wildcard_projection
                 ? wildcard_count
                 : ast.relations.front().output_expression_ids.size())) {
      return refuse_key_value(
          "SB_MODEL_BINDING_INCOMPLETE_V1",
          "key/value expression or projection descriptors are incomplete");
    }

    BoundCatalogRelationSourceAstRecord bound_source;
    bound_source.source_id = key_value_source_ast->source_id;
    bound_source.source_kind = key_value_source_ast->source_kind;
    bound_source.resolution_state =
        NativeCatalogRelationResolutionState::kBound;
    bound_source.qualified_name = key_value_source_ast->qualified_name;
    bound_source.alias = key_value_source_ast->alias;
    bound_source.alias_is_explicit = key_value_source_ast->alias_is_explicit;
    bound_source.model_family_id = key_value_source_ast->model_family_id;
    bound_source.model_operation_id = key_value_source_ast->model_operation_id;
    bound_source.model_comparison_operator =
        key_value_source_ast->model_comparison_operator;
    bound_source.qualified_name_range =
        key_value_source_ast->qualified_name_range;
    bound_source.range = key_value_source_ast->range;
    bound_source.object_uuid = resolution.object_uuid;
    bound_source.resolved_object_type = resolution.resolved_object_type;
    bound_source.resolved_schema_uuid = resolution.resolved_schema_uuid;
    bound_source.parent_object_uuid = resolution.parent_object_uuid;
    bound_source.catalog_generation_id = resolution.catalog_generation_id;
    bound_source.security_epoch = resolution.security_epoch;
    bound_source.resource_epoch = resolution.resource_epoch;
    for (const auto& column : resolution.columns) {
      if (!IsNonNullCanonicalUuid(column.column_uuid) ||
          !descriptor_by_id.contains(column.descriptor_id)) {
        return refuse_key_value(
            "SB_MODEL_BINDING_INCOMPLETE_V1",
            "key/value public projection column binding is incomplete");
      }
      bound_source.columns.push_back(
          {column.ordinal, column.column_uuid, column.descriptor_id,
           column.canonical_name_key});
    }

    BoundRelationAstRecord bound_relation;
    bound_relation.relation_id = ast.relations.front().relation_id;
    bound_relation.relation_kind = NativeRelationAstKind::kCatalogSource;
    bound_relation.semantic_variant_id = std::string(expected_semantic);
    bound_relation.bound_object_uuid = resolution.object_uuid;

    std::unordered_map<std::uint32_t, std::uint32_t> ast_to_bound;
    std::size_t binding_index = 0;
    if (wildcard_projection) {
      for (std::size_t ordinal = 0; ordinal < wildcard_count; ++ordinal) {
        const auto& expression = context.expressions[binding_index++];
        const auto& column = resolution.columns[ordinal];
        if (!descriptor_by_id.contains(expression.descriptor_id) ||
            expression.descriptor_id != column.descriptor_id ||
            expression.bound_name_uuid != column.column_uuid) {
          return refuse_key_value("SB_MODEL_BINDING_INCOMPLETE_V1",
                                  "key/value wildcard projection is not exact");
        }
        BoundExpressionAstRecord record;
        record.expression_id = expression.expression_id;
        record.expression_kind = NativeExpressionAstKind::kIdentifier;
        record.result_descriptor_id = expression.descriptor_id;
        record.bound_name_uuid = expression.bound_name_uuid;
        bound.expressions.push_back(std::move(record));
        bound_relation.output_expression_ids.push_back(expression.expression_id);
      }
    }
    for (const auto& ast_expression : ast.expressions) {
      if (ast_expression.expression_kind == NativeExpressionAstKind::kWildcard) {
        continue;
      }
      const auto& expression = context.expressions[binding_index++];
      if (!descriptor_by_id.contains(expression.descriptor_id) ||
          (expression.function_uuid.has_value() &&
           (!IsNonNullCanonicalUuid(*expression.function_uuid) ||
            ast_expression.operator_name == "KV_KEY" ||
            ast_expression.operator_name == "KV_MULTI_GET" ||
            ast_expression.operator_name == "KV_PREFIX")) ||
          (expression.bound_name_uuid.has_value() &&
           !IsNonNullCanonicalUuid(*expression.bound_name_uuid))) {
        return refuse_key_value("SB_MODEL_BINDING_INCOMPLETE_V1",
                                "key/value typed expression binding is invalid");
      }
      ast_to_bound.emplace(ast_expression.expression_id,
                           expression.expression_id);
      BoundExpressionAstRecord record;
      record.expression_id = expression.expression_id;
      record.expression_kind = ast_expression.expression_kind;
      record.literal_kind = ast_expression.literal_kind;
      record.result_descriptor_id = expression.descriptor_id;
      record.bound_function_uuid = expression.function_uuid;
      record.bound_name_uuid = expression.bound_name_uuid;
      if (!ast_expression.operator_name.empty()) {
        record.canonical_operator_name = ast_expression.operator_name;
      }
      if (ast_expression.expression_kind == NativeExpressionAstKind::kLiteral ||
          ast_expression.expression_kind == NativeExpressionAstKind::kParameter) {
        record.literal_or_parameter_ref = ast_expression.spelling;
      }
      for (const auto child : ast_expression.child_expression_ids) {
        const auto mapped = ast_to_bound.find(child);
        if (mapped == ast_to_bound.end()) {
          return refuse_key_value("SB_MODEL_BINDING_INCOMPLETE_V1",
                                  "key/value expression dependency is unresolved");
        }
        record.child_expression_ids.push_back(mapped->second);
      }
      bound.expressions.push_back(std::move(record));
    }
    const auto& key_column = resolution.columns[1];
    const auto* key_descriptor = descriptor_by_id.at(key_column.descriptor_id);
    for (const auto expression_id :
         key_value_source_ast->model_key_expression_ids) {
      const auto mapped = ast_to_bound.find(expression_id);
      if (mapped == ast_to_bound.end()) {
        return refuse_key_value("SB_MODEL_BINDING_INCOMPLETE_V1",
                                "key/value key expression mapping is incomplete");
      }
      const auto binding = std::ranges::find_if(
          bound.expressions, [&](const auto& expression) {
            return expression.expression_id == mapped->second;
          });
      const auto* descriptor =
          binding == bound.expressions.end()
              ? nullptr
              : descriptor_by_id.at(binding->result_descriptor_id);
      if (descriptor == nullptr ||
          descriptor->type_uuid != key_descriptor->type_uuid ||
          descriptor->nullability != BoundNullability::kNonNull) {
        return refuse_key_value("SB_MODEL_KEY_VALUE_KEY_TYPE_REFUSED_V1",
                                "key/value key expression is not non-null TEXT");
      }
      bound_source.model_key_expression_ids.push_back(mapped->second);
    }
    const auto mapped_root = ast_to_bound.find(root_expression_id);
    if (mapped_root == ast_to_bound.end()) {
      return refuse_key_value("SB_MODEL_BINDING_INCOMPLETE_V1",
                              "key/value operation root binding is incomplete");
    }
    bound_relation.predicate_expression_ids = {mapped_root->second};
    if (!wildcard_projection) {
      for (const auto expression_id :
           ast.relations.front().output_expression_ids) {
        const auto mapped = ast_to_bound.find(expression_id);
        if (mapped == ast_to_bound.end()) {
          return refuse_key_value("SB_MODEL_BINDING_INCOMPLETE_V1",
                                  "key/value projection binding is incomplete");
        }
        bound_relation.output_expression_ids.push_back(mapped->second);
      }
    }
    bound_relation.bound_expression_ids = bound_relation.output_expression_ids;
    for (std::size_t ordinal = 0; ordinal < context.outputs.size(); ++ordinal) {
      const auto& output = context.outputs[ordinal];
      if (output.output_id == 0 ||
          !descriptor_by_id.contains(output.descriptor_id) ||
          output.relation_id != bound_relation.relation_id ||
          output.ordinal != ordinal ||
          output.expression_id != bound_relation.output_expression_ids[ordinal]) {
        return refuse_key_value("SB_MODEL_BINDING_INCOMPLETE_V1",
                                "key/value projection output binding is invalid");
      }
      bound.outputs.push_back(
          {output.output_id, output.relation_id, output.expression_id,
           output.output_name_utf8, output.descriptor_id, output.visible,
           output.ordinal});
    }
    bound.catalog_relation_sources.push_back(std::move(bound_source));
    bound.relations.push_back(std::move(bound_relation));
    BoundScopeAstRecord scope;
    scope.scope_id = 1;
    scope.visible_relation_ids = {ast.root_relation_id};
    for (const auto& output : bound.outputs) {
      if (output.visible) scope.visible_projection_ids.push_back(output.output_id);
    }
    scope.catalog_epoch_uuid = context.catalog_epoch_uuid;
    bound.scopes.push_back(std::move(scope));
    bound.bound_ast_uuid = context.bound_ast_uuid;
    bound.security_context_uuid = context.security_context_uuid;
    bound.root_relation_id = ast.root_relation_id;
    bound.root_scope_id = 1;
    bound.bound = true;
    return bound;
  }
  const auto graph_source_ast = std::ranges::find_if(
      ast.catalog_relation_sources, [](const auto& source) {
        return source.source_kind == NativeRelationSourceAstKind::kGraph;
      });
  if (graph_source_ast != ast.catalog_relation_sources.end()) {
    // QOW-SOURCE-RCP-074-GRAPH-BINDING-V1
    const auto refuse_graph = [&](const char* diagnostic,
                                  const char* detail) {
      AddBoundAstDiagnostic(&bound, diagnostic, detail);
      return RefusedBoundAst(std::move(bound));
    };
    const bool graph_match =
        graph_source_ast->model_operation_id == "GRAPH_MATCH";
    const bool graph_expand =
        graph_source_ast->model_operation_id == "GRAPH_EXPAND";
    const auto same_graph_alias = [](const NativeIdentifierAstNode& left,
                                     const NativeIdentifierAstNode& right) {
      return left.quoted == right.quoted &&
             (left.quoted ? left.spelling == right.spelling
                          : ToUpperAscii(left.spelling) ==
                                ToUpperAscii(right.spelling));
    };
    const std::string_view expected_semantic =
        graph_expand ? "sblr.model-expand.graph-expand.v1"
                     : "sblr.model-source.graph-match.v1";
    if (ast.catalog_relation_sources.size() != 1 || ast.relations.size() != 1 ||
        ast.root_relation_id != ast.relations.front().relation_id ||
        ast.relations.front().relation_kind !=
            NativeRelationAstKind::kCatalogSource ||
        ast.relations.front().relation_source_ids !=
            std::vector<std::uint32_t>{graph_source_ast->source_id} ||
        graph_source_ast->model_family_id != "graph" ||
        (!graph_match && !graph_expand) ||
        graph_source_ast->qualified_name.empty() ||
        !graph_source_ast->alias.has_value() ||
        (graph_match && graph_source_ast->model_source_alias.has_value()) ||
        (graph_expand && !graph_source_ast->model_source_alias.has_value()) ||
        (graph_expand && !graph_source_ast->alias_is_explicit &&
         !same_graph_alias(*graph_source_ast->alias,
                           *graph_source_ast->model_source_alias)) ||
        graph_source_ast->model_graph_cycle_policy != "visited_set" ||
        ast.model_object_resolution_requests.size() != 1 ||
        context.catalog_relations.size() != 1 || context.relations.size() != 1 ||
        context.relations.front().relation_id !=
            ast.relations.front().relation_id ||
        context.relations.front().semantic_variant_id != expected_semantic) {
      return refuse_graph("SB_MODEL_BINDING_INCOMPLETE_V1",
                          "graph source AST or binding authority is incomplete");
    }
    const auto& request = ast.model_object_resolution_requests.front();
    if (request.source_id != graph_source_ast->source_id ||
        request.model_family_id != "graph" || request.object_class != "graph" ||
        request.qualified_name.size() != graph_source_ast->qualified_name.size() ||
        !std::ranges::equal(
            request.qualified_name, graph_source_ast->qualified_name,
            [](const auto& left, const auto& right) {
              return left.spelling == right.spelling &&
                     left.quoted == right.quoted;
            })) {
      return refuse_graph("SB_MODEL_BINDING_INCOMPLETE_V1",
                          "graph resolution request does not match the source AST");
    }
    const auto& resolution = context.catalog_relations.front();
    if (resolution.source_id != graph_source_ast->source_id ||
        resolution.resolution_state !=
            NativeCatalogRelationResolutionState::kBound ||
        !IsNonNullCanonicalUuid(resolution.object_uuid) ||
        !IsNonNullCanonicalUuid(resolution.resolved_schema_uuid) ||
        resolution.resolved_object_type != "graph" ||
        resolution.catalog_generation_id == 0 || resolution.security_epoch == 0 ||
        resolution.resource_epoch == 0 || resolution.columns.empty()) {
      return refuse_graph("SB_MODEL_BINDING_INCOMPLETE_V1",
                          "graph did not resolve to current UUID descriptors");
    }
    const bool match_operands_exact =
        graph_match && graph_source_ast->model_pattern_expression_id.has_value() &&
        graph_source_ast->model_graph_alias_expression_id.has_value() &&
        graph_source_ast->model_graph_direction.empty() &&
        !graph_source_ast->model_graph_minimum_depth.has_value() &&
        !graph_source_ast->model_graph_maximum_depth.has_value();
    const bool expand_operands_exact =
        graph_expand && !graph_source_ast->model_pattern_expression_id.has_value() &&
        graph_source_ast->model_graph_alias_expression_id.has_value() &&
        (graph_source_ast->model_graph_direction == "outgoing" ||
         graph_source_ast->model_graph_direction == "incoming" ||
         graph_source_ast->model_graph_direction == "both") &&
        graph_source_ast->model_graph_minimum_depth.has_value() &&
        graph_source_ast->model_graph_maximum_depth.has_value() &&
        *graph_source_ast->model_graph_minimum_depth <=
            *graph_source_ast->model_graph_maximum_depth;
    if ((!match_operands_exact && !expand_operands_exact) ||
        ast.relations.front().predicate_expression_ids.size() != 1) {
      return refuse_graph(
          graph_expand ? "SB_MODEL_GRAPH_UNBOUNDED_EXPANSION_REFUSED_V1"
                       : "SB_MODEL_BINDING_INCOMPLETE_V1",
          "graph operation operands, bounds, or cycle policy are incomplete");
    }

    const auto ast_expression_by_id = [&](const std::uint32_t expression_id)
        -> const NativeExpressionAstNode* {
      const auto found = std::ranges::find_if(
          ast.expressions, [&](const auto& expression) {
            return expression.expression_id == expression_id;
          });
      return found == ast.expressions.end() ? nullptr : &*found;
    };
    const auto root_expression_id =
        ast.relations.front().predicate_expression_ids.front();
    const auto* root_expression = ast_expression_by_id(root_expression_id);
    const auto* alias_expression = ast_expression_by_id(
        *graph_source_ast->model_graph_alias_expression_id);
    const auto& operation_alias =
        graph_expand ? *graph_source_ast->model_source_alias
                     : *graph_source_ast->alias;
    const auto* pattern_expression =
        graph_match ? ast_expression_by_id(
                          *graph_source_ast->model_pattern_expression_id)
                    : nullptr;
    std::unordered_set<std::uint32_t> graph_child_expression_ids;
    if (root_expression != nullptr) {
      graph_child_expression_ids.insert(
          root_expression->child_expression_ids.begin(),
          root_expression->child_expression_ids.end());
    }
    if (root_expression == nullptr || alias_expression == nullptr ||
        root_expression->expression_kind !=
            NativeExpressionAstKind::kFunctionCall ||
        root_expression->operator_name != graph_source_ast->model_operation_id ||
        alias_expression->expression_kind !=
            NativeExpressionAstKind::kIdentifier ||
        graph_child_expression_ids.size() !=
            root_expression->child_expression_ids.size() ||
        alias_expression->qualified_identifier.size() != 1 ||
        !same_graph_alias(alias_expression->qualified_identifier.front(),
                          operation_alias) ||
        (graph_match &&
         (pattern_expression == nullptr ||
          pattern_expression->expression_kind !=
              NativeExpressionAstKind::kLiteral ||
          pattern_expression->literal_kind != NativeLiteralAstKind::kString ||
          !ExactBoundedGraphPatternV1(pattern_expression->spelling) ||
          root_expression->child_expression_ids !=
              std::vector<std::uint32_t>{
                  alias_expression->expression_id,
                  pattern_expression->expression_id})) ||
        (graph_expand && root_expression->child_expression_ids.size() != 5)) {
      return refuse_graph("SB_MODEL_BINDING_INCOMPLETE_V1",
                          "graph operation expression correspondence is invalid");
    }
    if (graph_expand) {
      const auto* direction =
          ast_expression_by_id(root_expression->child_expression_ids[1]);
      const auto* minimum =
          ast_expression_by_id(root_expression->child_expression_ids[2]);
      const auto* maximum =
          ast_expression_by_id(root_expression->child_expression_ids[3]);
      const auto* cycle =
          ast_expression_by_id(root_expression->child_expression_ids[4]);
      if (root_expression->child_expression_ids[0] !=
              alias_expression->expression_id ||
          direction == nullptr || minimum == nullptr || maximum == nullptr ||
          cycle == nullptr ||
          direction->expression_kind != NativeExpressionAstKind::kLiteral ||
          direction->literal_kind != NativeLiteralAstKind::kString ||
          ToUpperAscii(direction->spelling) !=
              ToUpperAscii(graph_source_ast->model_graph_direction) ||
          minimum->expression_kind != NativeExpressionAstKind::kLiteral ||
          minimum->literal_kind != NativeLiteralAstKind::kNumeric ||
          minimum->spelling != std::to_string(
                                   *graph_source_ast->model_graph_minimum_depth) ||
          maximum->expression_kind != NativeExpressionAstKind::kLiteral ||
          maximum->literal_kind != NativeLiteralAstKind::kNumeric ||
          maximum->spelling != std::to_string(
                                   *graph_source_ast->model_graph_maximum_depth) ||
          cycle->expression_kind != NativeExpressionAstKind::kLiteral ||
          cycle->literal_kind != NativeLiteralAstKind::kString ||
          cycle->spelling != "visited_set") {
        return refuse_graph(
            "SB_MODEL_GRAPH_UNBOUNDED_EXPANSION_REFUSED_V1",
            "GRAPH_EXPAND ordered operands or visited-set policy were substituted");
      }
    }

    for (const auto& descriptor : context.descriptors) {
      bound.descriptors.push_back(
          {descriptor.descriptor_id, descriptor.descriptor_uuid,
           descriptor.type_uuid, descriptor.nullability,
           descriptor.collation_uuid, descriptor.timezone_profile_id,
           descriptor.width_precision_scale});
    }
    std::ranges::sort(bound.descriptors, {},
                      &BoundDescriptorAstRecord::descriptor_id);

    const bool wildcard_projection = std::ranges::any_of(
        ast.relations.front().output_expression_ids,
        [&](const auto expression_id) {
          const auto* expression = ast_expression_by_id(expression_id);
          return expression != nullptr &&
                 expression->expression_kind ==
                     NativeExpressionAstKind::kWildcard;
        });
    const auto wildcard_count =
        wildcard_projection ? resolution.columns.size() : std::size_t{0};
    const auto non_wildcard_ast_count =
        ast.expressions.size() - static_cast<std::size_t>(wildcard_projection);
    if (context.expressions.size() != wildcard_count + non_wildcard_ast_count ||
        context.outputs.empty() ||
        context.outputs.size() !=
            (wildcard_projection
                 ? wildcard_count
                 : ast.relations.front().output_expression_ids.size())) {
      return refuse_graph("SB_MODEL_BINDING_INCOMPLETE_V1",
                          "graph expression or projection descriptors are incomplete");
    }

    BoundCatalogRelationSourceAstRecord bound_source;
    bound_source.source_id = graph_source_ast->source_id;
    bound_source.source_kind = graph_source_ast->source_kind;
    bound_source.resolution_state = NativeCatalogRelationResolutionState::kBound;
    bound_source.qualified_name = graph_source_ast->qualified_name;
    bound_source.alias = graph_source_ast->alias;
    bound_source.alias_is_explicit = graph_source_ast->alias_is_explicit;
    bound_source.model_family_id = graph_source_ast->model_family_id;
    bound_source.model_operation_id = graph_source_ast->model_operation_id;
    bound_source.model_source_alias = graph_source_ast->model_source_alias;
    bound_source.model_graph_direction =
        graph_source_ast->model_graph_direction;
    bound_source.model_graph_minimum_depth =
        graph_source_ast->model_graph_minimum_depth;
    bound_source.model_graph_maximum_depth =
        graph_source_ast->model_graph_maximum_depth;
    bound_source.model_graph_cycle_policy =
        graph_source_ast->model_graph_cycle_policy;
    bound_source.qualified_name_range = graph_source_ast->qualified_name_range;
    bound_source.range = graph_source_ast->range;
    bound_source.object_uuid = resolution.object_uuid;
    bound_source.resolved_object_type = resolution.resolved_object_type;
    bound_source.resolved_schema_uuid = resolution.resolved_schema_uuid;
    bound_source.parent_object_uuid = resolution.parent_object_uuid;
    bound_source.catalog_generation_id = resolution.catalog_generation_id;
    bound_source.security_epoch = resolution.security_epoch;
    bound_source.resource_epoch = resolution.resource_epoch;
    for (const auto& column : resolution.columns) {
      if (column.ordinal >= resolution.columns.size() ||
          !IsNonNullCanonicalUuid(column.column_uuid) ||
          !descriptor_by_id.contains(column.descriptor_id)) {
        return refuse_graph("SB_MODEL_BINDING_INCOMPLETE_V1",
                            "graph projection column binding is incomplete");
      }
      bound_source.columns.push_back(
          {column.ordinal, column.column_uuid, column.descriptor_id,
           column.canonical_name_key});
    }

    BoundRelationAstRecord bound_relation;
    bound_relation.relation_id = ast.relations.front().relation_id;
    bound_relation.relation_kind = NativeRelationAstKind::kCatalogSource;
    bound_relation.semantic_variant_id = std::string(expected_semantic);
    bound_relation.bound_object_uuid = resolution.object_uuid;

    std::unordered_map<std::uint32_t, std::uint32_t> ast_to_bound;
    std::size_t binding_index = 0;
    if (wildcard_projection) {
      for (std::size_t ordinal = 0; ordinal < wildcard_count; ++ordinal) {
        const auto& expression = context.expressions[binding_index++];
        const auto& column = resolution.columns[ordinal];
        if (!descriptor_by_id.contains(expression.descriptor_id) ||
            column.ordinal != ordinal ||
            expression.descriptor_id != column.descriptor_id ||
            expression.bound_name_uuid != column.column_uuid) {
          return refuse_graph("SB_MODEL_BINDING_INCOMPLETE_V1",
                              "graph wildcard projection is not exact");
        }
        BoundExpressionAstRecord record;
        record.expression_id = expression.expression_id;
        record.expression_kind = NativeExpressionAstKind::kIdentifier;
        record.result_descriptor_id = expression.descriptor_id;
        record.bound_name_uuid = expression.bound_name_uuid;
        bound.expressions.push_back(std::move(record));
        bound_relation.output_expression_ids.push_back(expression.expression_id);
      }
    }
    for (const auto& ast_expression : ast.expressions) {
      if (ast_expression.expression_kind == NativeExpressionAstKind::kWildcard) {
        continue;
      }
      const auto& expression = context.expressions[binding_index++];
      if (!descriptor_by_id.contains(expression.descriptor_id) ||
          (expression.function_uuid.has_value() &&
           (!IsNonNullCanonicalUuid(*expression.function_uuid) ||
            ast_expression.operator_name == "GRAPH_MATCH" ||
            ast_expression.operator_name == "GRAPH_EXPAND")) ||
          (expression.bound_name_uuid.has_value() &&
           !IsNonNullCanonicalUuid(*expression.bound_name_uuid))) {
        return refuse_graph("SB_MODEL_BINDING_INCOMPLETE_V1",
                            "graph typed expression binding is invalid");
      }
      ast_to_bound.emplace(ast_expression.expression_id,
                           expression.expression_id);
      BoundExpressionAstRecord record;
      record.expression_id = expression.expression_id;
      record.expression_kind = ast_expression.expression_kind;
      record.literal_kind = ast_expression.literal_kind;
      record.result_descriptor_id = expression.descriptor_id;
      record.bound_function_uuid = expression.function_uuid;
      record.bound_name_uuid = expression.bound_name_uuid;
      if (!ast_expression.operator_name.empty()) {
        record.canonical_operator_name = ast_expression.operator_name;
      }
      if (ast_expression.expression_kind == NativeExpressionAstKind::kLiteral ||
          ast_expression.expression_kind == NativeExpressionAstKind::kParameter) {
        record.literal_or_parameter_ref = ast_expression.spelling;
      }
      for (const auto child : ast_expression.child_expression_ids) {
        const auto mapped = ast_to_bound.find(child);
        if (mapped == ast_to_bound.end()) {
          return refuse_graph("SB_MODEL_BINDING_INCOMPLETE_V1",
                              "graph expression dependency is unresolved");
        }
        record.child_expression_ids.push_back(mapped->second);
      }
      bound.expressions.push_back(std::move(record));
    }
    const auto map_optional = [&](const std::optional<std::uint32_t> value)
        -> std::optional<std::uint32_t> {
      if (!value.has_value()) return std::nullopt;
      const auto mapped = ast_to_bound.find(*value);
      return mapped == ast_to_bound.end() ? std::nullopt
                                          : std::optional(mapped->second);
    };
    bound_source.model_pattern_expression_id =
        map_optional(graph_source_ast->model_pattern_expression_id);
    bound_source.model_graph_alias_expression_id =
        map_optional(graph_source_ast->model_graph_alias_expression_id);
    if ((graph_source_ast->model_pattern_expression_id.has_value() &&
         !bound_source.model_pattern_expression_id.has_value()) ||
        !bound_source.model_graph_alias_expression_id.has_value()) {
      return refuse_graph("SB_MODEL_BINDING_INCOMPLETE_V1",
                          "graph operand expression mapping is incomplete");
    }
    const auto mapped_root = ast_to_bound.find(root_expression_id);
    if (mapped_root == ast_to_bound.end()) {
      return refuse_graph("SB_MODEL_BINDING_INCOMPLETE_V1",
                          "graph operation root binding is incomplete");
    }
    bound_relation.predicate_expression_ids = {mapped_root->second};
    if (!wildcard_projection) {
      for (const auto expression_id :
           ast.relations.front().output_expression_ids) {
        const auto mapped = ast_to_bound.find(expression_id);
        if (mapped == ast_to_bound.end()) {
          return refuse_graph("SB_MODEL_BINDING_INCOMPLETE_V1",
                              "graph projection binding is incomplete");
        }
        bound_relation.output_expression_ids.push_back(mapped->second);
      }
    }
    bound_relation.bound_expression_ids = bound_relation.output_expression_ids;

    for (std::size_t ordinal = 0; ordinal < context.outputs.size(); ++ordinal) {
      const auto& output = context.outputs[ordinal];
      if (output.output_id == 0 ||
          !descriptor_by_id.contains(output.descriptor_id) ||
          output.relation_id != bound_relation.relation_id ||
          output.ordinal != ordinal ||
          output.expression_id != bound_relation.output_expression_ids[ordinal]) {
        return refuse_graph("SB_MODEL_BINDING_INCOMPLETE_V1",
                            "graph projection output binding is invalid");
      }
      bound.outputs.push_back(
          {output.output_id, output.relation_id, output.expression_id,
           output.output_name_utf8, output.descriptor_id, output.visible,
           output.ordinal});
    }
    bound.catalog_relation_sources.push_back(std::move(bound_source));
    bound.relations.push_back(std::move(bound_relation));
    BoundScopeAstRecord scope;
    scope.scope_id = 1;
    scope.visible_relation_ids = {ast.root_relation_id};
    for (const auto& output : bound.outputs) {
      if (output.visible) scope.visible_projection_ids.push_back(output.output_id);
    }
    scope.catalog_epoch_uuid = context.catalog_epoch_uuid;
    bound.scopes.push_back(std::move(scope));
    bound.bound_ast_uuid = context.bound_ast_uuid;
    bound.security_context_uuid = context.security_context_uuid;
    bound.root_relation_id = ast.root_relation_id;
    bound.root_scope_id = 1;
    bound.bound = true;
    return bound;
  }
  const auto document_source_ast = std::ranges::find_if(
      ast.catalog_relation_sources, [](const auto& source) {
        return source.source_kind == NativeRelationSourceAstKind::kDocument;
      });
  if (document_source_ast != ast.catalog_relation_sources.end()) {
    // QOW-SOURCE-CES05-DOCUMENT-BINDING-V1
    const auto refuse_document = [&](const char* diagnostic,
                                     const char* detail) {
      AddBoundAstDiagnostic(&bound, diagnostic, detail);
      return RefusedBoundAst(std::move(bound));
    };
    const bool expression_backed_unnest =
        document_source_ast->model_operation_id == "DOCUMENT_UNNEST";
    const std::string_view expected_semantic =
        expression_backed_unnest
            ? "sblr.model-expand.document-unnest.v1"
            : (document_source_ast->model_operation_id == "DOCUMENT_PATH"
                   ? "sblr.model-source.document-path.v1"
                   : "sblr.model-source.document-find.v1");
    if (ast.catalog_relation_sources.size() != 1 || ast.relations.size() != 1 ||
        ast.root_relation_id != ast.relations.front().relation_id ||
        ast.relations.front().relation_kind !=
            NativeRelationAstKind::kCatalogSource ||
        ast.relations.front().relation_source_ids !=
            std::vector<std::uint32_t>{document_source_ast->source_id} ||
        document_source_ast->model_family_id != "document" ||
        (document_source_ast->model_operation_id != "DOCUMENT_FIND" &&
         document_source_ast->model_operation_id != "DOCUMENT_PATH" &&
         document_source_ast->model_operation_id != "DOCUMENT_UNNEST") ||
        (expression_backed_unnest && !context.catalog_relations.empty()) ||
        (!expression_backed_unnest && context.catalog_relations.size() != 1) ||
        context.relations.size() != 1 ||
        context.relations.front().relation_id !=
            ast.relations.front().relation_id ||
        context.relations.front().semantic_variant_id != expected_semantic) {
      return refuse_document("SB_MODEL_BINDING_INCOMPLETE_V1",
                              "document source AST or binding authority is incomplete");
    }
    if (expression_backed_unnest) {
      if (!ast.model_object_resolution_requests.empty()) {
        return refuse_document(
            "SB_MODEL_BINDING_INCOMPLETE_V1",
            "expression-backed document expansion carried catalog resolution");
      }
    } else {
      if (ast.model_object_resolution_requests.size() != 1) {
        return refuse_document("SB_MODEL_BINDING_INCOMPLETE_V1",
                               "document collection resolution request is missing");
      }
      const auto& request = ast.model_object_resolution_requests.front();
      if (request.source_id != document_source_ast->source_id ||
          request.model_family_id != "document" ||
          request.object_class != "document_collection" ||
          request.qualified_name.size() !=
              document_source_ast->qualified_name.size() ||
          !std::ranges::equal(
              request.qualified_name, document_source_ast->qualified_name,
              [](const auto& left, const auto& right) {
                return left.spelling == right.spelling &&
                       left.quoted == right.quoted;
              })) {
        return refuse_document(
            "SB_MODEL_BINDING_INCOMPLETE_V1",
            "document collection resolution request does not match the source AST");
      }
    }
    const NativeCatalogRelationBindingInput* resolution =
        expression_backed_unnest ? nullptr : &context.catalog_relations.front();
    if (resolution != nullptr &&
        (resolution->source_id != document_source_ast->source_id ||
         resolution->resolution_state !=
             NativeCatalogRelationResolutionState::kBound ||
         !IsNonNullCanonicalUuid(resolution->object_uuid) ||
         !IsNonNullCanonicalUuid(resolution->resolved_schema_uuid) ||
         (resolution->resolved_object_type != "document" &&
          resolution->resolved_object_type != "document_collection") ||
         resolution->catalog_generation_id == 0 ||
         resolution->security_epoch == 0 || resolution->resource_epoch == 0 ||
         resolution->columns.empty())) {
      return refuse_document("SB_MODEL_BINDING_INCOMPLETE_V1",
                              "document collection did not resolve to current UUID descriptors");
    }
    if ((!expression_backed_unnest &&
         document_source_ast->qualified_name.empty()) ||
        (expression_backed_unnest &&
         !document_source_ast->model_document_expression_id.has_value()) ||
        ((document_source_ast->model_operation_id == "DOCUMENT_PATH" ||
          document_source_ast->model_operation_id == "DOCUMENT_UNNEST") &&
         !document_source_ast->model_path_expression_id.has_value())) {
      return refuse_document("SB_MODEL_BINDING_INCOMPLETE_V1",
                              "document operation operands are unresolved");
    }
    const auto ast_expression_by_id = [&](const std::uint32_t expression_id)
        -> const NativeExpressionAstNode* {
      const auto found = std::ranges::find_if(
          ast.expressions, [&](const auto& expression) {
            return expression.expression_id == expression_id;
          });
      return found == ast.expressions.end() ? nullptr : &*found;
    };
    if (expression_backed_unnest) {
      const auto* document_expression = ast_expression_by_id(
          *document_source_ast->model_document_expression_id);
      const auto* path_expression = ast_expression_by_id(
          *document_source_ast->model_path_expression_id);
      if (document_expression == nullptr || path_expression == nullptr ||
          document_expression->expression_id == path_expression->expression_id ||
          path_expression->expression_kind !=
              NativeExpressionAstKind::kLiteral ||
          path_expression->literal_kind != NativeLiteralAstKind::kString ||
          path_expression->spelling.empty() ||
          !ast.relations.front().predicate_expression_ids.empty()) {
        return refuse_document(
            "SB_MODEL_BINDING_INCOMPLETE_V1",
            "DOCUMENT_UNNEST requires one document root and one typed path literal");
      }
    }

    for (const auto& descriptor : context.descriptors) {
      bound.descriptors.push_back(
          {descriptor.descriptor_id, descriptor.descriptor_uuid,
           descriptor.type_uuid, descriptor.nullability,
           descriptor.collation_uuid, descriptor.timezone_profile_id,
           descriptor.width_precision_scale});
    }
    std::ranges::sort(bound.descriptors, {},
                      &BoundDescriptorAstRecord::descriptor_id);

    const bool wildcard_projection = std::ranges::any_of(
        ast.relations.front().output_expression_ids,
        [&](const auto expression_id) {
          const auto expression = std::ranges::find_if(
              ast.expressions, [&](const auto& candidate) {
                return candidate.expression_id == expression_id;
              });
          return expression != ast.expressions.end() &&
                 expression->expression_kind ==
                     NativeExpressionAstKind::kWildcard;
        });
    const auto wildcard_count =
        wildcard_projection
            ? (resolution != nullptr ? resolution->columns.size()
                                     : context.outputs.size())
            : std::size_t{0};
    const auto non_wildcard_ast_count =
        ast.expressions.size() - static_cast<std::size_t>(wildcard_projection);
    if (context.expressions.size() != wildcard_count + non_wildcard_ast_count ||
        context.outputs.empty() ||
        context.outputs.size() !=
            (wildcard_projection ? wildcard_count
                                 : ast.relations.front().output_expression_ids.size())) {
      return refuse_document("SB_MODEL_BINDING_INCOMPLETE_V1",
                              "document expression or projection descriptors are incomplete");
    }

    BoundCatalogRelationSourceAstRecord bound_source;
    bound_source.source_id = document_source_ast->source_id;
    bound_source.source_kind = document_source_ast->source_kind;
    bound_source.resolution_state = NativeCatalogRelationResolutionState::kBound;
    bound_source.qualified_name = document_source_ast->qualified_name;
    bound_source.alias = document_source_ast->alias;
    bound_source.alias_is_explicit = document_source_ast->alias_is_explicit;
    bound_source.model_family_id = document_source_ast->model_family_id;
    bound_source.model_operation_id = document_source_ast->model_operation_id;
    bound_source.model_comparison_operator =
        document_source_ast->model_comparison_operator;
    bound_source.model_wildcard_path = document_source_ast->model_wildcard_path;
    bound_source.qualified_name_range = document_source_ast->qualified_name_range;
    bound_source.range = document_source_ast->range;
    if (resolution != nullptr) {
      bound_source.object_uuid = resolution->object_uuid;
      bound_source.resolved_object_type = resolution->resolved_object_type;
      bound_source.resolved_schema_uuid = resolution->resolved_schema_uuid;
      bound_source.parent_object_uuid = resolution->parent_object_uuid;
      bound_source.catalog_generation_id = resolution->catalog_generation_id;
      bound_source.security_epoch = resolution->security_epoch;
      bound_source.resource_epoch = resolution->resource_epoch;
      for (const auto& column : resolution->columns) {
        if (column.ordinal >= resolution->columns.size() ||
            !IsNonNullCanonicalUuid(column.column_uuid) ||
            !descriptor_by_id.contains(column.descriptor_id)) {
          return refuse_document("SB_MODEL_BINDING_INCOMPLETE_V1",
                                  "document collection column binding is incomplete");
        }
        bound_source.columns.push_back(
            {column.ordinal, column.column_uuid, column.descriptor_id,
             column.canonical_name_key});
      }
    } else {
      bound_source.resolved_object_type = "document_expression";
    }

    BoundRelationAstRecord bound_relation;
    bound_relation.relation_id = ast.relations.front().relation_id;
    bound_relation.relation_kind = NativeRelationAstKind::kCatalogSource;
    bound_relation.semantic_variant_id = std::string(expected_semantic);
    if (resolution != nullptr) {
      bound_relation.bound_object_uuid = resolution->object_uuid;
    }

    std::unordered_map<std::uint32_t, std::uint32_t> ast_to_bound;
    std::size_t binding_index = 0;
    const NativeExpressionBindingInput* unnest_output_binding = nullptr;
    if (wildcard_projection) {
      for (std::size_t ordinal = 0; ordinal < wildcard_count; ++ordinal) {
        const auto& expression = context.expressions[binding_index++];
        const auto* column =
            resolution != nullptr ? &resolution->columns[ordinal] : nullptr;
        if (expression.descriptor_id == 0 ||
            !descriptor_by_id.contains(expression.descriptor_id) ||
            (column != nullptr &&
             (column->ordinal != ordinal ||
              !IsNonNullCanonicalUuid(column->column_uuid) ||
              expression.descriptor_id != column->descriptor_id ||
              expression.bound_name_uuid != column->column_uuid))) {
          return refuse_document("SB_MODEL_BINDING_INCOMPLETE_V1",
                                  "document wildcard column binding is not exact");
        }
        if (expression_backed_unnest) {
          if (ordinal != 0 || unnest_output_binding != nullptr ||
              expression.function_uuid.has_value() ||
              expression.bound_name_uuid.has_value()) {
            return refuse_document(
                "SB_MODEL_BINDING_INCOMPLETE_V1",
                "DOCUMENT_UNNEST output root binding is not exact");
          }
          unnest_output_binding = &expression;
          continue;
        }
        BoundExpressionAstRecord record;
        record.expression_id = expression.expression_id;
        record.expression_kind = NativeExpressionAstKind::kIdentifier;
        record.result_descriptor_id = expression.descriptor_id;
        record.bound_name_uuid = expression.bound_name_uuid;
        bound.expressions.push_back(std::move(record));
        bound_relation.output_expression_ids.push_back(expression.expression_id);
        bound_relation.bound_expression_ids.push_back(expression.expression_id);
      }
    }

    for (const auto& ast_expression : ast.expressions) {
      if (ast_expression.expression_kind == NativeExpressionAstKind::kWildcard) {
        continue;
      }
      const auto& expression = context.expressions[binding_index++];
      if (expression.descriptor_id == 0 ||
          !descriptor_by_id.contains(expression.descriptor_id) ||
          (expression.function_uuid.has_value() &&
           !IsNonNullCanonicalUuid(*expression.function_uuid)) ||
          (expression.bound_name_uuid.has_value() &&
           !IsNonNullCanonicalUuid(*expression.bound_name_uuid))) {
        return refuse_document("SB_MODEL_BINDING_INCOMPLETE_V1",
                                "document typed expression binding is invalid");
      }
      ast_to_bound.emplace(ast_expression.expression_id,
                           expression.expression_id);
      BoundExpressionAstRecord record;
      record.expression_id = expression.expression_id;
      record.expression_kind = ast_expression.expression_kind;
      record.literal_kind = ast_expression.literal_kind;
      record.result_descriptor_id = expression.descriptor_id;
      record.bound_function_uuid = expression.function_uuid;
      record.bound_name_uuid = expression.bound_name_uuid;
      if ((ast_expression.expression_kind ==
               NativeExpressionAstKind::kUnary ||
           ast_expression.expression_kind ==
               NativeExpressionAstKind::kBinary) &&
          !ast_expression.operator_name.empty()) {
        record.canonical_operator_name = ast_expression.operator_name;
      } else if (ast_expression.expression_kind ==
                     NativeExpressionAstKind::kFunctionCall &&
                 ast_expression.operator_name == "DOCUMENT_PATH" &&
                 !expression.function_uuid.has_value()) {
        // The acquired statement context has aggregate/window callable
        // profiles, but no scalar document-function registry. DOCUMENT_PATH
        // is therefore authenticated by the exact document model operation
        // and SBLR node binding, never by substituting an aggregate UUID.
        record.canonical_operator_name = ast_expression.operator_name;
      }
      if (ast_expression.expression_kind == NativeExpressionAstKind::kLiteral ||
          ast_expression.expression_kind == NativeExpressionAstKind::kParameter) {
        record.literal_or_parameter_ref = ast_expression.spelling;
      }
      for (const auto child : ast_expression.child_expression_ids) {
        const auto mapped = ast_to_bound.find(child);
        if (mapped == ast_to_bound.end()) {
          return refuse_document("SB_MODEL_BINDING_INCOMPLETE_V1",
                                  "document expression dependency is unresolved");
        }
        record.child_expression_ids.push_back(mapped->second);
      }
      bound.expressions.push_back(std::move(record));
      bound_relation.bound_expression_ids.push_back(expression.expression_id);
    }
    const auto map_optional = [&](const std::optional<std::uint32_t> value)
        -> std::optional<std::uint32_t> {
      if (!value.has_value()) return std::nullopt;
      const auto mapped = ast_to_bound.find(*value);
      return mapped == ast_to_bound.end() ? std::nullopt
                                          : std::optional(mapped->second);
    };
    bound_source.model_document_expression_id =
        map_optional(document_source_ast->model_document_expression_id);
    bound_source.model_path_expression_id =
        map_optional(document_source_ast->model_path_expression_id);
    bound_source.model_value_expression_id =
        map_optional(document_source_ast->model_value_expression_id);
    if ((document_source_ast->model_document_expression_id.has_value() &&
         !bound_source.model_document_expression_id.has_value()) ||
        (document_source_ast->model_path_expression_id.has_value() &&
         !bound_source.model_path_expression_id.has_value()) ||
        (document_source_ast->model_value_expression_id.has_value() &&
         !bound_source.model_value_expression_id.has_value())) {
      return refuse_document("SB_MODEL_BINDING_INCOMPLETE_V1",
                              "document operation expression mapping is incomplete");
    }
    if (expression_backed_unnest) {
      const auto document_bound = std::ranges::find_if(
          bound.expressions, [&](const auto& expression) {
            return expression.expression_id ==
                   *bound_source.model_document_expression_id;
          });
      const auto path_bound = std::ranges::find_if(
          bound.expressions, [&](const auto& expression) {
            return expression.expression_id ==
                   *bound_source.model_path_expression_id;
          });
      if (unnest_output_binding == nullptr ||
          document_bound == bound.expressions.end() ||
          path_bound == bound.expressions.end() ||
          path_bound->expression_kind != NativeExpressionAstKind::kLiteral ||
          path_bound->literal_kind != NativeLiteralAstKind::kString ||
          !path_bound->literal_or_parameter_ref.has_value() ||
          !descriptor_by_id.contains(unnest_output_binding->descriptor_id) ||
          !descriptor_by_id.contains(document_bound->result_descriptor_id) ||
          descriptor_by_id.at(unnest_output_binding->descriptor_id)->type_uuid !=
              descriptor_by_id.at(document_bound->result_descriptor_id)
                  ->type_uuid) {
        return refuse_document(
            "SB_MODEL_BINDING_INCOMPLETE_V1",
            "DOCUMENT_UNNEST document root descriptor kind was substituted");
      }
      BoundExpressionAstRecord root;
      root.expression_id = unnest_output_binding->expression_id;
      root.expression_kind = NativeExpressionAstKind::kFunctionCall;
      root.result_descriptor_id = unnest_output_binding->descriptor_id;
      root.canonical_operator_name = "DOCUMENT_UNNEST";
      root.child_expression_ids = {
          *bound_source.model_document_expression_id,
          *bound_source.model_path_expression_id};
      if (std::ranges::any_of(bound.expressions, [&](const auto& expression) {
            return expression.expression_id == root.expression_id;
          })) {
        return refuse_document(
            "SB_MODEL_BINDING_INCOMPLETE_V1",
            "DOCUMENT_UNNEST output root identity is duplicated");
      }
      bound.expressions.push_back(std::move(root));
      bound_relation.output_expression_ids = {
          unnest_output_binding->expression_id};
    }

    for (const auto expression_id : ast.relations.front().predicate_expression_ids) {
      const auto mapped = ast_to_bound.find(expression_id);
      if (mapped == ast_to_bound.end()) {
        return refuse_document("SB_MODEL_BINDING_INCOMPLETE_V1",
                                "document predicate binding is incomplete");
      }
      bound_relation.predicate_expression_ids.push_back(mapped->second);
    }

    if (!wildcard_projection) {
      for (const auto expression_id :
           ast.relations.front().output_expression_ids) {
        const auto mapped = ast_to_bound.find(expression_id);
        if (mapped == ast_to_bound.end()) {
          return refuse_document("SB_MODEL_BINDING_INCOMPLETE_V1",
                                  "document projection binding is incomplete");
        }
        bound_relation.output_expression_ids.push_back(mapped->second);
      }
    }
    // The model source node binds exactly its projected roots. Predicate and
    // model-operation operand graphs remain reachable typed expressions, but
    // they are not additional source outputs and therefore cannot inflate the
    // canonical node's bound-expression width.
    bound_relation.bound_expression_ids =
        bound_relation.output_expression_ids;

    for (std::size_t ordinal = 0; ordinal < context.outputs.size(); ++ordinal) {
      const auto& output = context.outputs[ordinal];
      if (output.output_id == 0 || output.expression_id == 0 ||
          !descriptor_by_id.contains(output.descriptor_id) ||
          output.relation_id != bound_relation.relation_id ||
          output.ordinal != ordinal ||
          output.expression_id != bound_relation.output_expression_ids[ordinal]) {
        return refuse_document("SB_MODEL_BINDING_INCOMPLETE_V1",
                                "document projection output binding is invalid");
      }
      bound.outputs.push_back(
          {output.output_id, output.relation_id, output.expression_id,
           output.output_name_utf8, output.descriptor_id, output.visible,
           output.ordinal});
    }
    bound.catalog_relation_sources.push_back(std::move(bound_source));
    bound.relations.push_back(std::move(bound_relation));

    BoundScopeAstRecord scope;
    scope.scope_id = 1;
    scope.visible_relation_ids = {ast.root_relation_id};
    for (const auto& output : bound.outputs) {
      if (output.visible) scope.visible_projection_ids.push_back(output.output_id);
    }
    scope.catalog_epoch_uuid = context.catalog_epoch_uuid;
    bound.scopes.push_back(std::move(scope));
    bound.bound_ast_uuid = context.bound_ast_uuid;
    bound.security_context_uuid = context.security_context_uuid;
    bound.root_relation_id = ast.root_relation_id;
    bound.root_scope_id = 1;
    bound.bound = true;
    return bound;
  }
  const auto window_relation = std::ranges::find_if(
      ast.relations, [](const auto& relation) {
        return relation.relation_kind == NativeRelationAstKind::kWindow;
      });
  if (window_relation != ast.relations.end()) {
    // QOW-SOURCE-RCP-050-TYPED-WINDOW-BOUND-AST-V1
    const auto qualify_relation = std::ranges::find_if(
        ast.relations, [](const auto& relation) {
          return relation.relation_kind == NativeRelationAstKind::kQualify;
        });
    const bool has_qualify = qualify_relation != ast.relations.end();
    const auto source_relation = std::ranges::find_if(
        ast.relations, [](const auto& relation) {
          return relation.relation_kind ==
                 NativeRelationAstKind::kCatalogSource;
        });
    const auto window_binding = std::ranges::find_if(
        context.relations, [&](const auto& relation) {
          return relation.relation_id == window_relation->relation_id;
        });
    const auto qualify_binding = std::ranges::find_if(
        context.relations, [&](const auto& relation) {
          return has_qualify &&
                 relation.relation_id == qualify_relation->relation_id;
        });
    const bool rank_window =
        window_binding != context.relations.end() &&
        window_binding->semantic_variant_id == "window.rank.v1";
    const bool dense_rank_window =
        window_binding != context.relations.end() &&
        window_binding->semantic_variant_id == "window.dense-rank.v1";
    const bool percent_rank_window =
        window_binding != context.relations.end() &&
        window_binding->semantic_variant_id == "window.percent-rank.v1";
    const bool cume_dist_window =
        window_binding != context.relations.end() &&
        window_binding->semantic_variant_id == "window.cume-dist.v1";
    const bool ntile_window =
        window_binding != context.relations.end() &&
        window_binding->semantic_variant_id == "window.ntile.v1";
    const bool lag_window =
        window_binding != context.relations.end() &&
        window_binding->semantic_variant_id == "window.lag.v1";
    const bool lead_window =
        window_binding != context.relations.end() &&
        window_binding->semantic_variant_id == "window.lead.v1";
    const bool first_value_window =
        window_binding != context.relations.end() &&
        window_binding->semantic_variant_id == "window.first-value.v1";
    const bool last_value_window =
        window_binding != context.relations.end() &&
        window_binding->semantic_variant_id == "window.last-value.v1";
    const bool nth_value_window =
        window_binding != context.relations.end() &&
        window_binding->semantic_variant_id == "window.nth-value.v1";
    const bool aggregate_window =
        window_binding != context.relations.end() &&
        window_binding->semantic_variant_id == "window.aggregate-bridge.v1";
    const bool navigation_window = lag_window || lead_window;
    const bool value_window =
        navigation_window || first_value_window || last_value_window ||
        nth_value_window || aggregate_window;
    const bool peer_ranking_window =
        rank_window || dense_rank_window || percent_rank_window ||
        cume_dist_window;
    const bool strict_ordered_window =
        peer_ranking_window || ntile_window || value_window;
    const bool recognized_ranking =
        window_binding != context.relations.end() &&
        (strict_ordered_window || window_binding->semantic_variant_id ==
                                      "window.row-number.v1");
    constexpr std::string_view kRowNumberFunctionUuid =
        "019de5fc-2400-7539-bcce-00eef3ae7220";
    constexpr std::string_view kRankFunctionUuid =
        "019de5fc-2400-7b94-870d-0dd789ca70ab";
    constexpr std::string_view kDenseRankFunctionUuid =
        "019de5fc-2400-741d-bef0-f079fd3ba494";
    constexpr std::string_view kPercentRankFunctionUuid =
        "019de5fc-2400-7d86-86fe-96f3f27b5dd6";
    constexpr std::string_view kCumeDistFunctionUuid =
        "019de5fc-2400-721c-be64-2568b64a02b9";
    constexpr std::string_view kNtileFunctionUuid =
        "019de5fc-2400-7047-9474-232ca488c094";
    constexpr std::string_view kLagFunctionUuid =
        "019de5fc-2400-782c-8436-9ac310301738";
    constexpr std::string_view kLeadFunctionUuid =
        "019de5fc-2400-7a06-bc3c-6747cf5be66f";
    constexpr std::string_view kFirstValueFunctionUuid =
        "019de5fc-2400-7264-90fb-d25bd0f806f2";
    constexpr std::string_view kLastValueFunctionUuid =
        "019de5fc-2400-7d23-a5be-7ed3f1a5c3ec";
    constexpr std::string_view kNthValueFunctionUuid =
        "019de5fc-2400-7dc9-80e6-9f2ccf08076f";
    const auto aggregate_function_ast =
        aggregate_window && ast.window_invocations.size() == 1
            ? std::ranges::find_if(ast.expressions, [&](const auto& candidate) {
                return candidate.expression_id ==
                       ast.window_invocations.front().function_expression_id;
              })
            : ast.expressions.end();
    const std::string_view aggregate_operator =
        aggregate_function_ast != ast.expressions.end() &&
                (aggregate_function_ast->operator_name == "SUM" ||
                 aggregate_function_ast->operator_name == "MIN" ||
                 aggregate_function_ast->operator_name == "MAX" ||
                 aggregate_function_ast->operator_name == "COUNT" ||
                 aggregate_function_ast->operator_name == "COUNT_STAR" ||
                 aggregate_function_ast->operator_name == "BOOL_AND" ||
                 aggregate_function_ast->operator_name == "BOOL_OR" ||
                 aggregate_function_ast->operator_name == "EVERY")
            ? aggregate_function_ast->operator_name
            : std::string_view{};
    const bool aggregate_count_star_window =
        aggregate_window && aggregate_operator == "COUNT_STAR";
    const bool aggregate_count_window =
        aggregate_window &&
        (aggregate_operator == "COUNT" || aggregate_count_star_window);
    const bool value_operand_window =
        value_window && !aggregate_count_star_window;
    const bool aggregate_boolean_window =
        aggregate_window &&
        (aggregate_operator == "BOOL_AND" || aggregate_operator == "BOOL_OR" ||
         aggregate_operator == "EVERY");
    const bool aggregate_bounded_signed_window =
        aggregate_window && !aggregate_count_window &&
        !aggregate_boolean_window;
    const auto is_bounded_signed_type = [](const std::string_view type_name) {
      return type_name == "int8" || type_name == "int16" ||
             type_name == "int32" || type_name == "int64";
    };
    const std::string_view expected_operator =
        aggregate_window
            ? aggregate_operator
            : (value_window
            ? (first_value_window ? "FIRST_VALUE"
                                  : (last_value_window
                                         ? "LAST_VALUE"
                                         : (nth_value_window
                                                ? "NTH_VALUE"
                                                : (lag_window ? "LAG"
                                                              : "LEAD"))))
            : (ntile_window
            ? "NTILE"
            : (cume_dist_window
            ? "CUME_DIST"
            : (percent_rank_window
                   ? "PERCENT_RANK"
                   : (dense_rank_window
                          ? "DENSE_RANK"
                          : (rank_window ? "RANK" : "ROW_NUMBER"))))));
    const std::string_view expected_builtin =
        aggregate_window
            ? (aggregate_operator == "SUM"
                   ? "sb.aggregate.sum"
                   : (aggregate_operator == "MIN" ? "sb.aggregate.min"
                      : (aggregate_operator == "MAX"
                             ? "sb.aggregate.max"
                             : (aggregate_count_window
                                    ? "sb.aggregate.count"
                                    : (aggregate_operator == "BOOL_AND"
                                           ? "sb.aggregate.bool_and"
                                           : (aggregate_operator == "BOOL_OR"
                                                  ? "sb.aggregate.bool_or"
                                                  : "sb.aggregate.every"))))))
            : (value_window
            ? (first_value_window
                   ? "sb.window.first_value"
                   : (last_value_window
                          ? "sb.window.last_value"
                          : (nth_value_window
                                 ? "sb.window.nth_value"
                                 : (lag_window ? "sb.window.lag"
                                               : "sb.window.lead"))))
            : (ntile_window
            ? "sb.window.ntile"
            : (cume_dist_window
            ? "sb.window.cume_dist"
            : (percent_rank_window
                   ? "sb.window.percent_rank"
                   : (dense_rank_window
                          ? "sb.window.dense_rank"
                          : (rank_window ? "sb.window.rank"
                                         : "sb.window.row_number"))))));
    const std::string_view expected_function_uuid =
        aggregate_window
            ? (context.window_functions.size() == 1
                   ? std::string_view{
                         context.window_functions.front().function_uuid}
                   : std::string_view{})
            : (value_window
            ? (first_value_window
                   ? kFirstValueFunctionUuid
                   : (last_value_window
                          ? kLastValueFunctionUuid
                          : (nth_value_window
                                 ? kNthValueFunctionUuid
                                 : (lag_window ? kLagFunctionUuid
                                               : kLeadFunctionUuid))))
            : (ntile_window
            ? kNtileFunctionUuid
            : (cume_dist_window
            ? kCumeDistFunctionUuid
            : (percent_rank_window
                   ? kPercentRankFunctionUuid
                   : (dense_rank_window
                          ? kDenseRankFunctionUuid
                          : (rank_window ? kRankFunctionUuid
                                         : kRowNumberFunctionUuid))))));
    if (source_relation == ast.relations.end() ||
        ast.relations.size() != 2 + static_cast<std::size_t>(has_qualify) ||
        ast.root_relation_id !=
            (has_qualify ? qualify_relation->relation_id
                         : window_relation->relation_id) ||
        window_relation->input_relation_ids !=
            std::vector<std::uint32_t>{source_relation->relation_id} ||
        ast.catalog_relation_sources.size() != 1 ||
        ast.window_definitions.empty() ||
        ast.window_definitions.size() > 1024 ||
        ast.window_invocations.size() != 1 ||
        ast.values_rows.size() != 0 || ast.grouping_sets.size() != 0 ||
        context.catalog_relations.size() != 1 ||
        context.relations.size() !=
            1 + static_cast<std::size_t>(has_qualify) ||
        context.window_functions.size() != 1 ||
        !recognized_ranking || (strict_ordered_window && has_qualify) ||
        (has_qualify &&
         (qualify_binding == context.relations.end() ||
          qualify_binding->semantic_variant_id !=
              "qualify.window-result-numeric-comparison.v1" ||
          qualify_relation->input_relation_ids !=
              std::vector<std::uint32_t>{window_relation->relation_id} ||
          qualify_relation->output_expression_ids !=
              window_relation->output_expression_ids ||
          qualify_relation->predicate_expression_ids.size() != 1)) ||
        window_relation->window_invocation_ids !=
            std::vector<std::uint32_t>{
                ast.window_invocations.front().invocation_id} ||
        window_relation->output_expression_ids !=
            std::vector<std::uint32_t>{
                ast.window_invocations.front().function_expression_id}) {
      AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                            "typed window binding shape is incomplete");
      return RefusedBoundAst(std::move(bound));
    }

    const auto& ast_source = ast.catalog_relation_sources.front();
    const auto& relation_binding = context.catalog_relations.front();
    const auto source_count = source_relation->output_expression_ids.size();
    const auto& invocation = ast.window_invocations.front();
    const auto frame_bound_exact = [&](const auto& frame_bound,
                                       const bool start) {
      if (!frame_bound.has_value()) return true;
      const bool offset_kind =
          frame_bound->bound_kind == NativeWindowFrameBoundKind::kPreceding ||
          frame_bound->bound_kind == NativeWindowFrameBoundKind::kFollowing;
      if (offset_kind != frame_bound->offset_expression_id.has_value()) {
        return false;
      }
      return start
                 ? frame_bound->bound_kind !=
                       NativeWindowFrameBoundKind::kUnboundedFollowing
                 : frame_bound->bound_kind !=
                       NativeWindowFrameBoundKind::kUnboundedPreceding;
    };
    const auto canonical_window_name = [](const std::string_view value) {
      return !value.empty() && value.size() <= 128 &&
             ((value.front() >= 'a' && value.front() <= 'z') ||
              value.front() == '_') &&
             std::ranges::all_of(value, [](const unsigned char ch) {
               return (ch >= 'a' && ch <= 'z') ||
                      (ch >= '0' && ch <= '9') || ch == '_';
             });
    };
    struct EffectiveWindowShape {
      bool partition{false};
      bool ordering{false};
      bool frame{false};
    };
    std::unordered_map<std::uint32_t, std::size_t> definition_by_id;
    std::unordered_map<std::string, std::size_t> definition_by_name;
    std::vector<EffectiveWindowShape> effective_window_shapes;
    std::vector<std::optional<std::uint32_t>> inherited_window_ids;
    effective_window_shapes.reserve(ast.window_definitions.size());
    inherited_window_ids.reserve(ast.window_definitions.size());
    const bool named_profile = ast.window_definitions.front().name.has_value();
    for (std::size_t index = 0; index < ast.window_definitions.size(); ++index) {
      const auto& candidate = ast.window_definitions[index];
      if (candidate.window_id == 0 ||
          !definition_by_id.emplace(candidate.window_id, index).second ||
          candidate.name.has_value() != named_profile ||
          (candidate.name.has_value() &&
           (!canonical_window_name(candidate.name->spelling) ||
            candidate.name->quoted ||
            !definition_by_name
                 .emplace(candidate.name->spelling, index)
                 .second)) ||
          candidate.frame_unit.has_value() !=
              candidate.frame_start.has_value() ||
          (candidate.frame_end.has_value() &&
           !candidate.frame_start.has_value()) ||
          (!candidate.frame_unit.has_value() &&
           candidate.exclusion != NativeWindowFrameExclusion::kNoOthers) ||
          !frame_bound_exact(candidate.frame_start, true) ||
          !frame_bound_exact(candidate.frame_end, false)) {
        AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                              "typed window declaration is not canonical");
        return RefusedBoundAst(std::move(bound));
      }
      EffectiveWindowShape shape{
          !candidate.partition_expression_ids.empty(),
          !candidate.ordering_terms.empty(), candidate.frame_unit.has_value()};
      std::optional<std::uint32_t> inherited_window_id;
      if (candidate.base_name.has_value()) {
        if (!named_profile || candidate.base_name->quoted ||
            !canonical_window_name(candidate.base_name->spelling)) {
          AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                                "typed window base name is not canonical");
          return RefusedBoundAst(std::move(bound));
        }
        const auto base =
            definition_by_name.find(candidate.base_name->spelling);
        if (base == definition_by_name.end() || base->second >= index) {
          AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                                "typed window base is forward or unknown");
          return RefusedBoundAst(std::move(bound));
        }
        const auto& inherited = effective_window_shapes[base->second];
        if ((shape.partition && inherited.partition) ||
            (shape.ordering && inherited.ordering) ||
            (shape.frame && inherited.frame)) {
          AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                                "typed window state overrides its base");
          return RefusedBoundAst(std::move(bound));
        }
        shape.partition = shape.partition || inherited.partition;
        shape.ordering = shape.ordering || inherited.ordering;
        shape.frame = shape.frame || inherited.frame;
        inherited_window_id =
            ast.window_definitions[base->second].window_id;
      }
      effective_window_shapes.push_back(shape);
      inherited_window_ids.push_back(inherited_window_id);
    }
    const auto selected_definition =
        definition_by_id.find(invocation.window_definition_id);
    if (selected_definition == definition_by_id.end() ||
        (!effective_window_shapes[selected_definition->second].partition &&
         !effective_window_shapes[selected_definition->second].ordering)) {
      AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                            "typed ranking requires a resolved window key");
      return RefusedBoundAst(std::move(bound));
    }
    const auto& selected_window_definition =
        ast.window_definitions[selected_definition->second];
    const auto strict_function_ast = std::ranges::find_if(
        ast.expressions, [&](const auto& candidate) {
          return candidate.expression_id == invocation.function_expression_id;
        });
    std::vector<std::uint32_t> expected_strict_source_expression_ids;
    if (value_operand_window && strict_function_ast != ast.expressions.end() &&
        strict_function_ast->child_expression_ids.size() ==
            (nth_value_window ? 2U : 1U)) {
      expected_strict_source_expression_ids.push_back(
          strict_function_ast->child_expression_ids.front());
    }
    if (selected_definition != definition_by_id.end() &&
        selected_window_definition.ordering_terms.size() == 1) {
      const auto order_expression_id =
          selected_window_definition.ordering_terms.front().expression_id;
      if (expected_strict_source_expression_ids.empty() ||
          expected_strict_source_expression_ids.back() !=
              order_expression_id) {
        expected_strict_source_expression_ids.push_back(order_expression_id);
      }
    }
    if (strict_ordered_window &&
        (ast.window_definitions.size() != 1 ||
         selected_definition->second != 0 ||
         selected_window_definition.name.has_value() ||
         selected_window_definition.base_name.has_value() ||
         !selected_window_definition.partition_expression_ids.empty() ||
         selected_window_definition.ordering_terms.size() != 1 ||
         selected_window_definition.frame_unit.has_value() ||
         selected_window_definition.frame_start.has_value() ||
         selected_window_definition.frame_end.has_value() ||
         selected_window_definition.exclusion !=
             NativeWindowFrameExclusion::kNoOthers ||
         source_relation->output_expression_ids !=
             expected_strict_source_expression_ids)) {
      AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                            "typed " + std::string(expected_operator) +
                                " window shape is not exact");
      return RefusedBoundAst(std::move(bound));
    }
    std::vector<std::uint32_t> offset_ast_ids;
    const auto collect_offset = [&](const auto& frame_bound) {
      if (frame_bound.has_value() &&
          frame_bound->offset_expression_id.has_value() &&
          std::ranges::find(offset_ast_ids,
                            *frame_bound->offset_expression_id) ==
              offset_ast_ids.end()) {
        offset_ast_ids.push_back(*frame_bound->offset_expression_id);
      }
    };
    for (const auto& candidate : ast.window_definitions) {
      collect_offset(candidate.frame_start);
      collect_offset(candidate.frame_end);
    }
    const auto expected_expression_count =
        source_count + offset_ast_ids.size() +
        static_cast<std::size_t>(ntile_window || nth_value_window) + 1 +
        (has_qualify ? 2 : 0);
    if (source_count == 0 || invocation.invocation_id == 0 ||
        context.descriptors.size() != expected_expression_count ||
        context.expressions.size() != expected_expression_count ||
        context.outputs.size() !=
            source_count + 1 + static_cast<std::size_t>(has_qualify) ||
        ast_source.source_id != relation_binding.source_id ||
        source_relation->relation_source_ids !=
            std::vector<std::uint32_t>{ast_source.source_id} ||
        relation_binding.resolution_state !=
            NativeCatalogRelationResolutionState::kBound ||
        !IsNonNullCanonicalUuid(relation_binding.object_uuid) ||
        !IsNonNullCanonicalUuid(relation_binding.resolved_schema_uuid) ||
        relation_binding.resolved_object_type.empty() ||
        relation_binding.catalog_generation_id == 0 ||
        relation_binding.security_epoch == 0 ||
        relation_binding.resource_epoch == 0 ||
        relation_binding.columns.size() != source_count) {
      AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                            "typed window source authority is incomplete");
      return RefusedBoundAst(std::move(bound));
    }

    BoundCatalogRelationSourceAstRecord bound_source;
    bound_source.source_id = ast_source.source_id;
    bound_source.source_kind = ast_source.source_kind;
    bound_source.resolution_state = relation_binding.resolution_state;
    bound_source.qualified_name = ast_source.qualified_name;
    bound_source.alias = ast_source.alias;
    bound_source.alias_is_explicit = ast_source.alias_is_explicit;
    bound_source.qualified_name_range = ast_source.qualified_name_range;
    bound_source.range = ast_source.range;
    bound_source.object_uuid = relation_binding.object_uuid;
    bound_source.resolved_object_type = relation_binding.resolved_object_type;
    bound_source.resolved_schema_uuid = relation_binding.resolved_schema_uuid;
    bound_source.parent_object_uuid = relation_binding.parent_object_uuid;
    bound_source.catalog_generation_id =
        relation_binding.catalog_generation_id;
    bound_source.security_epoch = relation_binding.security_epoch;
    bound_source.resource_epoch = relation_binding.resource_epoch;

    BoundRelationAstRecord bound_source_relation;
    bound_source_relation.relation_id = source_relation->relation_id;
    bound_source_relation.relation_kind =
        NativeRelationAstKind::kCatalogSource;
    bound_source_relation.semantic_variant_id = "catalog.relation-source.v1";
    bound_source_relation.bound_object_uuid = relation_binding.object_uuid;
    std::unordered_map<std::uint32_t, std::uint32_t> ast_to_bound;
    for (std::size_t ordinal = 0; ordinal < source_count; ++ordinal) {
      const auto ast_expression = std::ranges::find_if(
          ast.expressions, [&](const auto& candidate) {
            return candidate.expression_id ==
                   source_relation->output_expression_ids[ordinal];
          });
      const auto& column = relation_binding.columns[ordinal];
      const auto& expression = context.expressions[ordinal];
      const auto& output = context.outputs[ordinal];
      const auto descriptor = descriptor_by_id.find(column.descriptor_id);
      const auto expected_binding = static_cast<std::uint32_t>(ordinal + 1);
      if (ast_expression == ast.expressions.end() ||
          ast_expression->expression_kind !=
              NativeExpressionAstKind::kIdentifier ||
          ast_expression->spelling != column.canonical_name_key ||
          column.ordinal != ordinal || column.column_uuid.empty() ||
          descriptor == descriptor_by_id.end() ||
          expression.expression_id != expected_binding ||
          expression.descriptor_id != column.descriptor_id ||
          expression.function_uuid.has_value() ||
          expression.bound_name_uuid != column.column_uuid ||
          output.output_id != expected_binding ||
          output.expression_id != expression.expression_id ||
          output.descriptor_id != column.descriptor_id || output.visible ||
          output.ordinal != ordinal ||
          output.relation_id != source_relation->relation_id) {
        AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-OUTPUT",
                              "typed window source projection is not exact");
        return RefusedBoundAst(std::move(bound));
      }
      ast_to_bound.emplace(ast_expression->expression_id,
                           expression.expression_id);
      bound_source.columns.push_back(
          {column.ordinal, column.column_uuid, column.descriptor_id,
           column.canonical_name_key});
      BoundExpressionAstRecord bound_expression;
      bound_expression.expression_id = expression.expression_id;
      bound_expression.expression_kind = NativeExpressionAstKind::kIdentifier;
      bound_expression.result_descriptor_id = expression.descriptor_id;
      bound_expression.bound_name_uuid = expression.bound_name_uuid;
      bound.expressions.push_back(std::move(bound_expression));
      bound.outputs.push_back(
          {output.output_id, output.relation_id, output.expression_id,
           output.output_name_utf8, output.descriptor_id, output.visible,
           output.ordinal});
      bound_source_relation.output_expression_ids.push_back(
          expression.expression_id);
      bound_source_relation.bound_expression_ids.push_back(
          expression.expression_id);
    }
    bound.catalog_relation_sources.push_back(std::move(bound_source));
    bound.relations.push_back(std::move(bound_source_relation));

    for (std::size_t ordinal = 0; ordinal < offset_ast_ids.size(); ++ordinal) {
      const auto ast_expression = std::ranges::find_if(
          ast.expressions, [&](const auto& candidate) {
            return candidate.expression_id == offset_ast_ids[ordinal];
          });
      const auto binding_index = source_count + ordinal;
      const auto& expression = context.expressions[binding_index];
      if (ast_expression == ast.expressions.end() ||
          ast_expression->expression_kind !=
              NativeExpressionAstKind::kLiteral ||
          ast_expression->literal_kind != NativeLiteralAstKind::kNumeric ||
          expression.expression_id != binding_index + 1 ||
          expression.descriptor_id != binding_index + 1 ||
          expression.function_uuid.has_value() ||
          expression.bound_name_uuid.has_value()) {
        AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-EXPRESSION",
                              "typed window frame offset is not exact");
        return RefusedBoundAst(std::move(bound));
      }
      ast_to_bound.emplace(ast_expression->expression_id,
                           expression.expression_id);
      BoundExpressionAstRecord bound_expression;
      bound_expression.expression_id = expression.expression_id;
      bound_expression.expression_kind = NativeExpressionAstKind::kLiteral;
      bound_expression.literal_kind = NativeLiteralAstKind::kNumeric;
      bound_expression.result_descriptor_id = expression.descriptor_id;
      bound_expression.literal_or_parameter_ref = ast_expression->spelling;
      bound.expressions.push_back(std::move(bound_expression));
    }

    const auto function_ast = std::ranges::find_if(
        ast.expressions, [&](const auto& candidate) {
          return candidate.expression_id == invocation.function_expression_id;
        });
    std::optional<std::uint32_t> bound_numeric_window_operand_id;
    const NativeDescriptorBindingInput* numeric_window_operand_descriptor =
        nullptr;
    if (ntile_window || nth_value_window) {
      const NativeExpressionAstNode* operand_ast = nullptr;
      if (function_ast != ast.expressions.end() &&
          function_ast->child_expression_ids.size() ==
              (nth_value_window ? 2U : 1U)) {
        const auto operand = std::ranges::find_if(
            ast.expressions, [&](const auto& candidate) {
              return candidate.expression_id ==
                     function_ast->child_expression_ids[
                         nth_value_window ? 1U : 0U];
            });
        if (operand != ast.expressions.end()) operand_ast = &*operand;
      }
      const auto operand_binding_index = source_count + offset_ast_ids.size();
      const auto& operand_binding = context.expressions[operand_binding_index];
      const auto descriptor = descriptor_by_id.find(operand_binding.descriptor_id);
      std::uint64_t bucket_count = 0;
      const char* parsed_end = nullptr;
      std::errc parse_error = std::errc::invalid_argument;
      if (operand_ast != nullptr && !operand_ast->spelling.empty()) {
        const auto parsed = std::from_chars(
            operand_ast->spelling.data(),
            operand_ast->spelling.data() + operand_ast->spelling.size(),
            bucket_count);
        parsed_end = parsed.ptr;
        parse_error = parsed.ec;
      }
      if (operand_ast == nullptr ||
          operand_ast->expression_kind != NativeExpressionAstKind::kLiteral ||
          operand_ast->literal_kind != NativeLiteralAstKind::kNumeric ||
          !operand_ast->child_expression_ids.empty() ||
          !operand_ast->operator_name.empty() || operand_ast->spelling.empty() ||
          (operand_ast->spelling.size() > 1 &&
           operand_ast->spelling.front() == '0') ||
          parse_error != std::errc{} ||
          parsed_end != operand_ast->spelling.data() +
                            operand_ast->spelling.size() ||
          bucket_count == 0 ||
          bucket_count > static_cast<std::uint64_t>(
                             std::numeric_limits<std::int64_t>::max()) ||
          operand_ast->structural_literal_occurrence_id == 0 ||
          operand_binding.expression_id != operand_binding_index + 1 ||
          operand_binding.descriptor_id != operand_binding_index + 1 ||
          operand_binding.function_uuid.has_value() ||
          operand_binding.bound_name_uuid.has_value() ||
          operand_binding.structural_literal_occurrence_id !=
              operand_ast->structural_literal_occurrence_id ||
          operand_binding.structural_parameter_occurrence_id != 0 ||
          operand_binding.structural_variable_occurrence_id != 0 ||
          descriptor == descriptor_by_id.end() ||
          descriptor->second->nullability != BoundNullability::kNonNull ||
          descriptor->second->collation_uuid.has_value() ||
          descriptor->second->timezone_profile_id.has_value() ||
          descriptor->second->width_precision_scale.width.has_value() ||
          descriptor->second->width_precision_scale.precision.has_value() ||
          descriptor->second->width_precision_scale.scale.has_value() ||
          !ast_to_bound
               .emplace(operand_ast->expression_id,
                        operand_binding.expression_id)
               .second) {
        AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-EXPRESSION",
                              "typed " + std::string(expected_operator) +
                                  " numeric operand binding is not exact");
        return RefusedBoundAst(std::move(bound));
      }
      BoundExpressionAstRecord bound_operand;
      bound_operand.expression_id = operand_binding.expression_id;
      bound_operand.expression_kind = NativeExpressionAstKind::kLiteral;
      bound_operand.literal_kind = NativeLiteralAstKind::kNumeric;
      bound_operand.result_descriptor_id = operand_binding.descriptor_id;
      bound_operand.literal_or_parameter_ref = operand_ast->spelling;
      bound_operand.structural_literal_occurrence_id =
          operand_ast->structural_literal_occurrence_id;
      bound_numeric_window_operand_id = operand_binding.expression_id;
      numeric_window_operand_descriptor = descriptor->second;
      bound.expressions.push_back(std::move(bound_operand));
    }
    std::optional<std::uint32_t> bound_lag_operand_id;
    const NativeDescriptorBindingInput* lag_operand_descriptor = nullptr;
    if (value_operand_window) {
      const NativeExpressionAstNode* operand_ast = nullptr;
      if (function_ast != ast.expressions.end() &&
          function_ast->child_expression_ids.size() ==
              (nth_value_window ? 2U : 1U)) {
        const auto operand = std::ranges::find_if(
            ast.expressions, [&](const auto& candidate) {
              return candidate.expression_id ==
                     function_ast->child_expression_ids.front();
            });
        if (operand != ast.expressions.end()) operand_ast = &*operand;
      }
      const auto mapped_operand =
          operand_ast == nullptr
              ? ast_to_bound.end()
              : ast_to_bound.find(operand_ast->expression_id);
      const NativeExpressionBindingInput* operand_binding = nullptr;
      if (mapped_operand != ast_to_bound.end() &&
          mapped_operand->second != 0 &&
          mapped_operand->second <= context.expressions.size()) {
        operand_binding = &context.expressions[mapped_operand->second - 1];
      }
      const auto descriptor =
          operand_binding == nullptr
              ? descriptor_by_id.end()
              : descriptor_by_id.find(operand_binding->descriptor_id);
      if (operand_ast == nullptr ||
          operand_ast->expression_kind !=
              NativeExpressionAstKind::kIdentifier ||
          !operand_ast->child_expression_ids.empty() ||
          !operand_ast->operator_name.empty() || operand_ast->spelling.empty() ||
          mapped_operand == ast_to_bound.end() || operand_binding == nullptr ||
          operand_binding->expression_id != mapped_operand->second ||
          operand_binding->function_uuid.has_value() ||
          !operand_binding->bound_name_uuid.has_value() ||
          descriptor == descriptor_by_id.end()) {
        AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-EXPRESSION",
                              "typed " + std::string(expected_operator) +
                                  " operand binding is not exact");
        return RefusedBoundAst(std::move(bound));
      }
      bound_lag_operand_id = mapped_operand->second;
      lag_operand_descriptor = descriptor->second;
    }
    const auto function_binding_index =
        source_count + offset_ast_ids.size() +
        static_cast<std::size_t>(ntile_window || nth_value_window);
    const auto& function_binding = context.expressions[function_binding_index];
    const auto& window_function_binding = context.window_functions.front();
    const auto& result_output = context.outputs[source_count];
    const auto function_descriptor =
        descriptor_by_id.find(function_binding.descriptor_id);
    const auto mapped_order_expression =
        selected_window_definition.ordering_terms.size() == 1
            ? ast_to_bound.find(
                  selected_window_definition.ordering_terms.front().expression_id)
            : ast_to_bound.end();
    const NativeDescriptorBindingInput* order_descriptor = nullptr;
    if (mapped_order_expression != ast_to_bound.end() &&
        mapped_order_expression->second != 0 &&
        mapped_order_expression->second <= context.expressions.size()) {
      const auto& order_binding =
          context.expressions[mapped_order_expression->second - 1];
      const auto found = descriptor_by_id.find(order_binding.descriptor_id);
      if (found != descriptor_by_id.end()) order_descriptor = found->second;
    }
    std::vector<std::uint32_t> expected_function_ast_children;
    if (value_operand_window && bound_lag_operand_id.has_value()) {
      expected_function_ast_children.push_back(
          function_ast->child_expression_ids.front());
    } else if (ntile_window && bound_numeric_window_operand_id.has_value()) {
      expected_function_ast_children.push_back(
          function_ast->child_expression_ids.front());
    }
    if (nth_value_window && bound_numeric_window_operand_id.has_value()) {
      expected_function_ast_children.push_back(
          function_ast->child_expression_ids[1]);
    }
    if (function_ast == ast.expressions.end() ||
        function_ast->expression_kind !=
            NativeExpressionAstKind::kFunctionCall ||
        function_ast->operator_name != expected_operator ||
        function_ast->child_expression_ids != expected_function_ast_children ||
        function_binding.expression_id != function_binding_index + 1 ||
        function_binding.descriptor_id != function_binding_index + 1 ||
        !function_binding.function_uuid.has_value() ||
        *function_binding.function_uuid != expected_function_uuid ||
        !IsNonNullCanonicalUuid(*function_binding.function_uuid) ||
        function_binding.bound_name_uuid.has_value() ||
        window_function_binding.invocation_id != invocation.invocation_id ||
        window_function_binding.function_expression_id !=
            function_binding.expression_id ||
        window_function_binding.abi_version != 1 ||
        window_function_binding.builtin_id != expected_builtin ||
        window_function_binding.function_uuid != expected_function_uuid ||
        window_function_binding.function_uuid !=
            *function_binding.function_uuid ||
        !window_function_binding.executable ||
        window_function_binding.result_descriptor_id !=
            function_binding.descriptor_id ||
        function_descriptor == descriptor_by_id.end() ||
        function_descriptor->second->descriptor_uuid ==
            expected_function_uuid ||
        function_descriptor->second->type_uuid == expected_function_uuid ||
        (aggregate_window &&
         (order_descriptor == nullptr ||
          !is_bounded_signed_type(order_descriptor->canonical_type_name) ||
          order_descriptor->collation_uuid.has_value() ||
          order_descriptor->timezone_profile_id.has_value() ||
          order_descriptor->width_precision_scale.width.has_value() ||
          order_descriptor->width_precision_scale.precision.has_value() ||
          order_descriptor->width_precision_scale.scale.has_value() ||
          !order_descriptor->element_profile.empty())) ||
        (aggregate_window &&
         function_descriptor->second->canonical_type_name !=
             (aggregate_boolean_window ? "boolean" : "int64")) ||
        function_descriptor->second->nullability !=
            (value_window && !aggregate_count_window
                 ? BoundNullability::kNullable
                 : BoundNullability::kNonNull) ||
        ((!value_window || aggregate_window) &&
         (function_descriptor->second->collation_uuid.has_value() ||
          function_descriptor->second->timezone_profile_id.has_value() ||
          function_descriptor->second->width_precision_scale.width.has_value() ||
          function_descriptor->second->width_precision_scale.precision
              .has_value() ||
          function_descriptor->second->width_precision_scale.scale.has_value())) ||
        (ntile_window &&
         (numeric_window_operand_descriptor == nullptr ||
          numeric_window_operand_descriptor->descriptor_id ==
              function_descriptor->second->descriptor_id ||
          numeric_window_operand_descriptor->descriptor_uuid ==
              function_descriptor->second->descriptor_uuid ||
          numeric_window_operand_descriptor->type_uuid !=
              function_descriptor->second->type_uuid)) ||
        (value_operand_window &&
         (lag_operand_descriptor == nullptr ||
          lag_operand_descriptor->descriptor_id ==
              function_descriptor->second->descriptor_id ||
          lag_operand_descriptor->descriptor_uuid ==
              function_descriptor->second->descriptor_uuid ||
          lag_operand_descriptor->descriptor_uuid ==
              expected_function_uuid ||
          (!aggregate_count_window &&
           !aggregate_bounded_signed_window &&
           (lag_operand_descriptor->type_uuid !=
                function_descriptor->second->type_uuid ||
            lag_operand_descriptor->collation_uuid !=
                function_descriptor->second->collation_uuid ||
            lag_operand_descriptor->timezone_profile_id !=
                function_descriptor->second->timezone_profile_id ||
            lag_operand_descriptor->width_precision_scale.width !=
                function_descriptor->second->width_precision_scale.width ||
            lag_operand_descriptor->width_precision_scale.precision !=
                function_descriptor->second->width_precision_scale.precision ||
            lag_operand_descriptor->width_precision_scale.scale !=
                function_descriptor->second->width_precision_scale.scale ||
            lag_operand_descriptor->canonical_type_name !=
                function_descriptor->second->canonical_type_name ||
            lag_operand_descriptor->element_profile !=
                function_descriptor->second->element_profile)) ||
          (aggregate_bounded_signed_window &&
           (!is_bounded_signed_type(
                lag_operand_descriptor->canonical_type_name) ||
            lag_operand_descriptor->collation_uuid.has_value() ||
            lag_operand_descriptor->timezone_profile_id.has_value() ||
            lag_operand_descriptor->width_precision_scale.width.has_value() ||
            lag_operand_descriptor->width_precision_scale.precision
                .has_value() ||
            lag_operand_descriptor->width_precision_scale.scale.has_value() ||
            !lag_operand_descriptor->element_profile.empty())))) ||
        (nth_value_window &&
         (order_descriptor == nullptr ||
          order_descriptor->canonical_type_name != "int64" ||
          order_descriptor->collation_uuid.has_value() ||
          order_descriptor->timezone_profile_id.has_value() ||
          order_descriptor->width_precision_scale.width.has_value() ||
          order_descriptor->width_precision_scale.precision.has_value() ||
          order_descriptor->width_precision_scale.scale.has_value() ||
          !order_descriptor->element_profile.empty() ||
          numeric_window_operand_descriptor == nullptr ||
          lag_operand_descriptor == nullptr ||
          numeric_window_operand_descriptor->descriptor_id ==
              lag_operand_descriptor->descriptor_id ||
          numeric_window_operand_descriptor->descriptor_id ==
              function_descriptor->second->descriptor_id ||
          numeric_window_operand_descriptor->descriptor_id ==
              order_descriptor->descriptor_id ||
          numeric_window_operand_descriptor->descriptor_uuid ==
              lag_operand_descriptor->descriptor_uuid ||
          numeric_window_operand_descriptor->descriptor_uuid ==
              function_descriptor->second->descriptor_uuid ||
          numeric_window_operand_descriptor->descriptor_uuid ==
              order_descriptor->descriptor_uuid ||
          numeric_window_operand_descriptor->descriptor_uuid ==
              expected_function_uuid ||
          numeric_window_operand_descriptor->type_uuid !=
              order_descriptor->type_uuid ||
          numeric_window_operand_descriptor->canonical_type_name != "int64" ||
          numeric_window_operand_descriptor->nullability !=
              BoundNullability::kNonNull ||
          numeric_window_operand_descriptor->collation_uuid.has_value() ||
          numeric_window_operand_descriptor->timezone_profile_id.has_value() ||
          numeric_window_operand_descriptor->width_precision_scale.width
              .has_value() ||
          numeric_window_operand_descriptor->width_precision_scale.precision
              .has_value() ||
          numeric_window_operand_descriptor->width_precision_scale.scale
              .has_value() ||
          !numeric_window_operand_descriptor->element_profile.empty())) ||
        result_output.output_id != source_count + 1 ||
        result_output.expression_id != function_binding.expression_id ||
        result_output.descriptor_id != function_binding.descriptor_id ||
        result_output.visible == has_qualify || result_output.ordinal != 0 ||
        result_output.relation_id != window_relation->relation_id) {
      AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-EXPRESSION",
                            "typed ranking binding is not exact");
      return RefusedBoundAst(std::move(bound));
    }
    ast_to_bound.emplace(function_ast->expression_id,
                         function_binding.expression_id);
    BoundExpressionAstRecord bound_function;
    bound_function.expression_id = function_binding.expression_id;
    bound_function.expression_kind = NativeExpressionAstKind::kFunctionCall;
    bound_function.result_descriptor_id = function_binding.descriptor_id;
    bound_function.bound_function_uuid = function_binding.function_uuid;
    if (bound_lag_operand_id.has_value()) {
      bound_function.child_expression_ids.push_back(*bound_lag_operand_id);
    } else if (bound_numeric_window_operand_id.has_value()) {
      bound_function.child_expression_ids.push_back(
          *bound_numeric_window_operand_id);
    }
    if (nth_value_window && bound_numeric_window_operand_id.has_value()) {
      bound_function.child_expression_ids.push_back(
          *bound_numeric_window_operand_id);
    }
    bound.expressions.push_back(std::move(bound_function));
    bound.outputs.push_back(
        {result_output.output_id, result_output.relation_id,
         result_output.expression_id, result_output.output_name_utf8,
         result_output.descriptor_id, result_output.visible,
         result_output.ordinal});

    std::optional<std::uint32_t> bound_qualify_predicate_id;
    std::optional<std::uint32_t> qualify_output_id;
    if (has_qualify) {
      const auto predicate_ast = std::ranges::find_if(
          ast.expressions, [&](const auto& candidate) {
            return candidate.expression_id ==
                   qualify_relation->predicate_expression_ids.front();
          });
      const auto accepted_comparison = [&](const std::string_view name) {
        return name == "=" || name == "<>" || name == "!=" || name == "<" ||
               name == "<=" || name == ">" || name == ">=";
      };
      const NativeExpressionAstNode* literal_ast = nullptr;
      if (predicate_ast != ast.expressions.end() &&
          predicate_ast->expression_kind == NativeExpressionAstKind::kBinary &&
          predicate_ast->child_expression_ids.size() == 2 &&
          predicate_ast->child_expression_ids.front() ==
              invocation.function_expression_id &&
          accepted_comparison(predicate_ast->operator_name)) {
        const auto literal = std::ranges::find_if(
            ast.expressions, [&](const auto& candidate) {
              return candidate.expression_id ==
                     predicate_ast->child_expression_ids.back();
            });
        if (literal != ast.expressions.end()) literal_ast = &*literal;
      }
      const auto literal_binding_index = function_binding_index + 1;
      const auto predicate_binding_index = function_binding_index + 2;
      const auto& literal_binding =
          context.expressions[literal_binding_index];
      const auto& predicate_binding =
          context.expressions[predicate_binding_index];
      const auto& qualify_output = context.outputs[source_count + 1];
      const auto function_descriptor =
          descriptor_by_id.find(function_binding.descriptor_id);
      const auto literal_descriptor =
          descriptor_by_id.find(literal_binding.descriptor_id);
      const auto predicate_descriptor =
          descriptor_by_id.find(predicate_binding.descriptor_id);
      if (literal_ast == nullptr ||
          literal_ast->expression_kind != NativeExpressionAstKind::kLiteral ||
          literal_ast->literal_kind != NativeLiteralAstKind::kNumeric ||
          literal_ast->spelling.empty() ||
          literal_binding.expression_id != literal_binding_index + 1 ||
          literal_binding.descriptor_id != literal_binding_index + 1 ||
          literal_binding.function_uuid.has_value() ||
          literal_binding.bound_name_uuid.has_value() ||
          predicate_binding.expression_id != predicate_binding_index + 1 ||
          predicate_binding.descriptor_id != predicate_binding_index + 1 ||
          predicate_binding.function_uuid.has_value() ||
          predicate_binding.bound_name_uuid.has_value() ||
          function_descriptor == descriptor_by_id.end() ||
          literal_descriptor == descriptor_by_id.end() ||
          predicate_descriptor == descriptor_by_id.end() ||
          function_descriptor->second->type_uuid !=
              literal_descriptor->second->type_uuid ||
          literal_descriptor->second->nullability !=
              BoundNullability::kNonNull ||
          predicate_descriptor->second->nullability !=
              BoundNullability::kNullable ||
          qualify_output.output_id != source_count + 2 ||
          qualify_output.expression_id != function_binding.expression_id ||
          qualify_output.descriptor_id != function_binding.descriptor_id ||
          !qualify_output.visible || qualify_output.ordinal != 0 ||
          qualify_output.relation_id != qualify_relation->relation_id ||
          qualify_output.output_name_utf8 != result_output.output_name_utf8) {
        AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-EXPRESSION",
                              "typed QUALIFY binding is not exact");
        return RefusedBoundAst(std::move(bound));
      }
      ast_to_bound.emplace(literal_ast->expression_id,
                           literal_binding.expression_id);
      ast_to_bound.emplace(predicate_ast->expression_id,
                           predicate_binding.expression_id);
      BoundExpressionAstRecord bound_literal;
      bound_literal.expression_id = literal_binding.expression_id;
      bound_literal.expression_kind = NativeExpressionAstKind::kLiteral;
      bound_literal.literal_kind = NativeLiteralAstKind::kNumeric;
      bound_literal.result_descriptor_id = literal_binding.descriptor_id;
      bound_literal.literal_or_parameter_ref = literal_ast->spelling;
      bound.expressions.push_back(std::move(bound_literal));
      BoundExpressionAstRecord bound_predicate;
      bound_predicate.expression_id = predicate_binding.expression_id;
      bound_predicate.expression_kind = NativeExpressionAstKind::kBinary;
      bound_predicate.child_expression_ids = {
          function_binding.expression_id, literal_binding.expression_id};
      bound_predicate.result_descriptor_id = predicate_binding.descriptor_id;
      bound_predicate.canonical_operator_name = predicate_ast->operator_name;
      bound_qualify_predicate_id = bound_predicate.expression_id;
      bound.expressions.push_back(std::move(bound_predicate));
      bound.outputs.push_back(
          {qualify_output.output_id, qualify_output.relation_id,
           qualify_output.expression_id, qualify_output.output_name_utf8,
           qualify_output.descriptor_id, qualify_output.visible,
           qualify_output.ordinal});
      qualify_output_id = qualify_output.output_id;
    }

    const auto map_expression_ids = [&](const auto& ast_ids,
                                        std::vector<std::uint32_t>* result) {
      for (const auto ast_id : ast_ids) {
        const auto mapped = ast_to_bound.find(ast_id);
        if (mapped == ast_to_bound.end()) return false;
        result->push_back(mapped->second);
      }
      return true;
    };
    const auto bind_frame_bound = [&](const auto& input, auto* output) {
      if (!input.has_value()) return true;
      BoundWindowFrameBoundAstRecord record;
      record.bound_kind = input->bound_kind;
      if (input->offset_expression_id.has_value()) {
        const auto mapped = ast_to_bound.find(*input->offset_expression_id);
        if (mapped == ast_to_bound.end()) return false;
        record.offset_expression_id = mapped->second;
      }
      *output = std::move(record);
      return true;
    };
    for (std::size_t index = 0; index < ast.window_definitions.size(); ++index) {
      const auto& ast_definition = ast.window_definitions[index];
      BoundWindowDefinitionAstRecord bound_definition;
      bound_definition.window_id = ast_definition.window_id;
      if (ast_definition.name.has_value()) {
        bound_definition.canonical_name_key = ast_definition.name->spelling;
      }
      bound_definition.inherited_window_id = inherited_window_ids[index];
      if (!map_expression_ids(
              ast_definition.partition_expression_ids,
              &bound_definition.partition_expression_ids)) {
        AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-EXPRESSION",
                              "typed window partition key is unbound");
        return RefusedBoundAst(std::move(bound));
      }
      for (const auto& term : ast_definition.ordering_terms) {
        const auto mapped = ast_to_bound.find(term.expression_id);
        if (mapped == ast_to_bound.end()) {
          AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-EXPRESSION",
                                "typed window order key is unbound");
          return RefusedBoundAst(std::move(bound));
        }
        bound_definition.ordering_terms.push_back(
            {mapped->second, term.direction, term.null_placement});
      }
      bound_definition.frame_unit = ast_definition.frame_unit;
      if (!bind_frame_bound(ast_definition.frame_start,
                            &bound_definition.frame_start) ||
          !bind_frame_bound(ast_definition.frame_end,
                            &bound_definition.frame_end)) {
        AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-EXPRESSION",
                              "typed window frame bound is unbound");
        return RefusedBoundAst(std::move(bound));
      }
      bound_definition.exclusion = ast_definition.exclusion;
      bound.window_definitions.push_back(std::move(bound_definition));
    }
    bound.window_invocations.push_back(
        {invocation.invocation_id, function_binding.expression_id,
         invocation.window_definition_id, result_output.output_name_utf8,
         window_function_binding.abi_version,
         window_function_binding.builtin_id,
         window_function_binding.function_uuid,
         window_function_binding.result_descriptor_id,
         [&] {
           std::vector<std::uint32_t> arguments;
           if (bound_lag_operand_id.has_value()) {
             arguments.push_back(*bound_lag_operand_id);
           } else if (bound_numeric_window_operand_id.has_value()) {
             arguments.push_back(*bound_numeric_window_operand_id);
           }
           if (nth_value_window &&
               bound_numeric_window_operand_id.has_value()) {
             arguments.push_back(*bound_numeric_window_operand_id);
           }
           return arguments;
         }()});

    BoundRelationAstRecord bound_window;
    bound_window.relation_id = window_relation->relation_id;
    bound_window.relation_kind = NativeRelationAstKind::kWindow;
    bound_window.input_relation_ids = window_relation->input_relation_ids;
    bound_window.output_expression_ids = {function_binding.expression_id};
    bound_window.window_invocation_ids = {invocation.invocation_id};
    bound_window.semantic_variant_id = window_binding->semantic_variant_id;
    const auto append_bound_expression = [&](const std::uint32_t expression_id) {
      if (std::ranges::find(bound_window.bound_expression_ids,
                            expression_id) ==
          bound_window.bound_expression_ids.end()) {
        bound_window.bound_expression_ids.push_back(expression_id);
      }
    };
    for (const auto& bound_definition : bound.window_definitions) {
      for (const auto expression_id :
           bound_definition.partition_expression_ids) {
        append_bound_expression(expression_id);
      }
      for (const auto& term : bound_definition.ordering_terms) {
        append_bound_expression(term.expression_id);
      }
    }
    for (const auto expression_id : offset_ast_ids) {
      append_bound_expression(ast_to_bound.at(expression_id));
    }
    if (bound_lag_operand_id.has_value()) {
      // The optimizer contract carries separate order and value roles even
      // when both roles bind the same source expression.
      bound_window.bound_expression_ids.push_back(*bound_lag_operand_id);
    } else if (bound_numeric_window_operand_id.has_value()) {
      append_bound_expression(*bound_numeric_window_operand_id);
    }
    if (nth_value_window && bound_numeric_window_operand_id.has_value()) {
      append_bound_expression(*bound_numeric_window_operand_id);
    }
    append_bound_expression(function_binding.expression_id);
    bound.relations.push_back(std::move(bound_window));
    if (has_qualify) {
      BoundRelationAstRecord bound_qualify;
      bound_qualify.relation_id = qualify_relation->relation_id;
      bound_qualify.relation_kind = NativeRelationAstKind::kQualify;
      bound_qualify.input_relation_ids = {window_relation->relation_id};
      bound_qualify.output_expression_ids = {
          function_binding.expression_id};
      bound_qualify.predicate_expression_ids = {
          *bound_qualify_predicate_id};
      bound_qualify.bound_expression_ids = {
          *bound_qualify_predicate_id};
      bound_qualify.semantic_variant_id =
          qualify_binding->semantic_variant_id;
      bound.relations.push_back(std::move(bound_qualify));
    }

    bound.descriptors.reserve(context.descriptors.size());
    for (const auto& descriptor : context.descriptors) {
      bound.descriptors.push_back(
          {descriptor.descriptor_id, descriptor.descriptor_uuid,
           descriptor.type_uuid, descriptor.nullability,
           descriptor.collation_uuid, descriptor.timezone_profile_id,
           descriptor.width_precision_scale, descriptor.canonical_type_name,
           descriptor.element_profile});
    }
    std::ranges::sort(bound.descriptors, {},
                      &BoundDescriptorAstRecord::descriptor_id);
    BoundScopeAstRecord scope;
    scope.scope_id = 1;
    scope.visible_relation_ids = {ast.root_relation_id};
    scope.visible_projection_ids = {
        has_qualify ? *qualify_output_id : result_output.output_id};
    scope.catalog_epoch_uuid = context.catalog_epoch_uuid;
    bound.scopes.push_back(std::move(scope));
    bound.bound_ast_uuid = context.bound_ast_uuid;
    bound.security_context_uuid = context.security_context_uuid;
    bound.root_relation_id = ast.root_relation_id;
    bound.root_scope_id = 1;
    bound.bound = true;
    return bound;
  }
  const auto catalog_join = std::ranges::find_if(
      ast.relations, [](const auto& relation) {
        return relation.relation_kind == NativeRelationAstKind::kJoin;
      });
  if (catalog_join != ast.relations.end()) {
    std::vector<const NativeRelationAstNode*> source_relations;
    const NativeRelationAstNode* filter_relation = nullptr;
    const NativeRelationAstNode* project_relation = nullptr;
    for (const auto& relation : ast.relations) {
      if (relation.relation_kind == NativeRelationAstKind::kCatalogSource) {
        source_relations.push_back(&relation);
      } else if (relation.relation_kind == NativeRelationAstKind::kFilter &&
                 filter_relation == nullptr) {
        filter_relation = &relation;
      } else if (relation.relation_kind == NativeRelationAstKind::kProject &&
                 project_relation == nullptr) {
        project_relation = &relation;
      }
    }
    const bool spatial_columnar_join =
        ast.catalog_relation_sources.size() == 2 &&
        std::ranges::all_of(ast.catalog_relation_sources, [](const auto& source) {
          return source.source_kind == NativeRelationSourceAstKind::kSpatial ||
                 source.source_kind == NativeRelationSourceAstKind::kColumnar;
        }) &&
        std::ranges::count_if(ast.catalog_relation_sources,
                              [](const auto& source) {
                                return source.source_kind ==
                                       NativeRelationSourceAstKind::kSpatial;
                              }) == 1;
    const bool filter_composition =
        filter_relation != nullptr &&
        filter_relation->relation_id == catalog_join->relation_id + 1 &&
        filter_relation->input_relation_ids ==
            std::vector<std::uint32_t>{catalog_join->relation_id} &&
        filter_relation->output_expression_ids ==
            catalog_join->output_expression_ids &&
        filter_relation->predicate_expression_ids.size() == 1 &&
        filter_relation->relation_source_ids.empty() &&
        filter_relation->values_row_ids.empty() &&
        filter_relation->window_invocation_ids.empty() &&
        filter_relation->grouping_key_expression_ids.empty() &&
        filter_relation->aggregate_expression_ids.empty() &&
        filter_relation->limit_expression_ids.empty() &&
        filter_relation->ordering_terms.empty() &&
        filter_relation->join_kind == NativeJoinAstKind::kNone &&
        filter_relation->aggregate_grouping_form ==
            NativeAggregateGroupingForm::kNone &&
        filter_relation->aggregate_projection_form ==
            NativeAggregateProjectionForm::kNone;
    const auto project_predecessor_relation_id =
        filter_composition ? filter_relation->relation_id
                           : catalog_join->relation_id;
    const bool project_composition =
        project_relation != nullptr &&
        project_relation->relation_id == project_predecessor_relation_id + 1 &&
        project_relation->input_relation_ids ==
            std::vector<std::uint32_t>{project_predecessor_relation_id} &&
        !project_relation->output_expression_ids.empty() &&
        project_relation->relation_source_ids.empty() &&
        project_relation->values_row_ids.empty() &&
        project_relation->window_invocation_ids.empty() &&
        project_relation->grouping_key_expression_ids.empty() &&
        project_relation->aggregate_expression_ids.empty() &&
        project_relation->predicate_expression_ids.empty() &&
        project_relation->limit_expression_ids.empty() &&
        project_relation->ordering_terms.empty() &&
        project_relation->join_kind == NativeJoinAstKind::kNone &&
        project_relation->aggregate_grouping_form ==
            NativeAggregateGroupingForm::kNone &&
        project_relation->aggregate_projection_form ==
            NativeAggregateProjectionForm::kNone;
    if (ast.catalog_relation_sources.size() != 2 ||
        source_relations.size() != 2 ||
        ast.relations.size() !=
            3 + static_cast<std::size_t>(filter_composition) +
                static_cast<std::size_t>(project_composition) ||
        context.catalog_relations.size() != 2 ||
        context.relations.size() != 1 ||
        context.relations.front().relation_id != catalog_join->relation_id ||
        (context.relations.front().semantic_variant_id != "join.cross.v1" &&
         context.relations.front().semantic_variant_id != "join.inner.v1" &&
         context.relations.front().semantic_variant_id !=
             "join.left-outer.v1" &&
         context.relations.front().semantic_variant_id !=
             "join.right-outer.v1" &&
         context.relations.front().semantic_variant_id !=
             "join.full-outer.v1" &&
         context.relations.front().semantic_variant_id !=
             "join.left-semi.v1" &&
         context.relations.front().semantic_variant_id !=
             "join.left-anti.v1") ||
        ast.root_relation_id !=
            (project_composition
                 ? project_relation->relation_id
                 : (filter_composition ? filter_relation->relation_id
                                       : catalog_join->relation_id)) ||
        catalog_join->input_relation_ids !=
            std::vector<std::uint32_t>{source_relations[0]->relation_id,
                                       source_relations[1]->relation_id} ||
        catalog_join->predicate_expression_ids.size() > 1 ||
        ((filter_composition || project_composition) &&
         std::ranges::any_of(
             ast.catalog_relation_sources, [](const auto& source) {
               return source.source_kind !=
                      NativeRelationSourceAstKind::kCatalogRelation;
             })) ||
        (filter_relation != nullptr && !filter_composition) ||
        (project_relation != nullptr && !project_composition)) {
      AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                            "catalog JOIN binding shape is incomplete");
      return RefusedBoundAst(std::move(bound));
    }
    const bool predicate_join =
        context.relations.front().semantic_variant_id != "join.cross.v1";
    const bool left_only_join =
        context.relations.front().semantic_variant_id == "join.left-semi.v1" ||
        context.relations.front().semantic_variant_id == "join.left-anti.v1";
    std::vector<const NativeExpressionAstNode*> predicate_nodes;
    if (predicate_join) {
      const auto find_expression = [&](const std::uint32_t expression_id) {
        const auto found = std::ranges::find_if(
            ast.expressions, [&](const auto& expression) {
              return expression.expression_id == expression_id;
            });
        return found == ast.expressions.end() ? nullptr : &*found;
      };
      const auto is_comparison = [](const NativeExpressionAstNode* candidate) {
        return candidate != nullptr &&
               candidate->expression_kind ==
                   NativeExpressionAstKind::kBinary &&
               candidate->child_expression_ids.size() == 2 &&
               (candidate->operator_name == "=" ||
                candidate->operator_name == "<>" ||
                candidate->operator_name == "!=" ||
                candidate->operator_name == "<" ||
                candidate->operator_name == "<=" ||
                candidate->operator_name == ">" ||
                candidate->operator_name == ">=" ||
                candidate->operator_name == "IS DISTINCT FROM" ||
                candidate->operator_name == "IS NOT DISTINCT FROM");
      };
      const auto* predicate_root = find_expression(
          catalog_join->predicate_expression_ids.front());
      std::unordered_set<std::uint32_t> visited_predicate_ids;
      const auto collect_predicate = [&](auto&& self,
                                         const NativeExpressionAstNode* node,
                                         const std::size_t depth) -> bool {
        if (node == nullptr || depth > 32 ||
            !visited_predicate_ids.insert(node->expression_id).second) {
          return false;
        }
        if (!is_comparison(node)) {
          if (node->expression_kind != NativeExpressionAstKind::kBinary ||
              node->child_expression_ids.size() != 2 ||
              (node->operator_name != "AND" && node->operator_name != "OR")) {
            return false;
          }
          for (const auto child_id : node->child_expression_ids) {
            if (!self(self, find_expression(child_id), depth + 1)) return false;
          }
        }
        predicate_nodes.push_back(node);
        return predicate_nodes.size() <= 32;
      };
      if (!collect_predicate(collect_predicate, predicate_root, 1) ||
          predicate_nodes.empty() || predicate_nodes.size() > 32) {
        AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-EXPRESSION",
                              "catalog JOIN predicate is not exact");
        return RefusedBoundAst(std::move(bound));
      }
    }
    std::vector<const NativeExpressionAstNode*> project_identifiers;
    if (project_composition) {
      std::unordered_set<std::uint32_t> project_expression_ids;
      std::unordered_set<std::string> project_names;
      for (const auto expression_id : project_relation->output_expression_ids) {
        const auto expression = std::ranges::find_if(
            ast.expressions, [&](const auto& candidate) {
              return candidate.expression_id == expression_id;
            });
        if (expression == ast.expressions.end() ||
            expression->expression_kind !=
                NativeExpressionAstKind::kIdentifier ||
            expression->qualified_identifier.size() != 1 ||
            expression->qualified_identifier.front().spelling !=
                expression->spelling ||
            expression->spelling.empty() ||
            !expression->child_expression_ids.empty() ||
            expression->literal_kind.has_value() ||
            !expression->operator_name.empty() ||
            expression->structural_literal_occurrence_id != 0 ||
            expression->structural_parameter_occurrence_id != 0 ||
            expression->structural_variable_occurrence_id != 0 ||
            !project_expression_ids.insert(expression_id).second ||
            !project_names
                 .insert(expression->qualified_identifier.front().quoted
                             ? expression->qualified_identifier.front().spelling
                             : ToLowerAscii(
                                   expression->qualified_identifier.front()
                                       .spelling))
                 .second) {
          AddBoundAstDiagnostic(
              &bound, "QOW-DIAG-BOUNDAST-EXPRESSION",
              "catalog JOIN project identifier is not exact");
          return RefusedBoundAst(std::move(bound));
        }
        project_identifiers.push_back(&*expression);
      }
    }

    const NativeExpressionAstNode* filter_predicate = nullptr;
    const NativeExpressionAstNode* filter_identifier = nullptr;
    const NativeExpressionAstNode* filter_value = nullptr;
    if (filter_composition) {
      const auto find_expression = [&](const std::uint32_t expression_id) {
        const auto found = std::ranges::find_if(
            ast.expressions, [&](const auto& expression) {
              return expression.expression_id == expression_id;
            });
        return found == ast.expressions.end() ? nullptr : &*found;
      };
      filter_predicate =
          find_expression(filter_relation->predicate_expression_ids.front());
      if (filter_predicate != nullptr &&
          filter_predicate->child_expression_ids.size() == 2) {
        filter_identifier =
            find_expression(filter_predicate->child_expression_ids[0]);
        filter_value =
            find_expression(filter_predicate->child_expression_ids[1]);
      }
      const bool accepted_operator =
          filter_predicate != nullptr &&
          (filter_predicate->operator_name == "=" ||
           filter_predicate->operator_name == "<>" ||
           filter_predicate->operator_name == "!=" ||
           filter_predicate->operator_name == "<" ||
           filter_predicate->operator_name == "<=" ||
           filter_predicate->operator_name == ">" ||
           filter_predicate->operator_name == ">=");
      const bool numeric_literal =
          filter_value != nullptr &&
          filter_value->expression_kind == NativeExpressionAstKind::kLiteral &&
          filter_value->literal_kind == NativeLiteralAstKind::kNumeric;
      std::uint64_t parsed_numeric = 0;
      const auto parsed =
          numeric_literal
              ? std::from_chars(filter_value->spelling.data(),
                                filter_value->spelling.data() +
                                    filter_value->spelling.size(),
                                parsed_numeric)
              : std::from_chars_result{};
      if (!accepted_operator || filter_identifier == nullptr ||
          filter_value == nullptr ||
          filter_predicate->expression_kind !=
              NativeExpressionAstKind::kBinary ||
          filter_identifier->expression_kind !=
              NativeExpressionAstKind::kIdentifier ||
          filter_identifier->qualified_identifier.size() != 1 ||
          !filter_identifier->child_expression_ids.empty() ||
          !filter_identifier->operator_name.empty() ||
          !filter_value->child_expression_ids.empty() ||
          !filter_value->operator_name.empty() ||
          (!numeric_literal &&
           filter_value->expression_kind !=
               NativeExpressionAstKind::kParameter &&
           filter_value->expression_kind != NativeExpressionAstKind::kVariable) ||
          ((filter_value->expression_kind ==
                NativeExpressionAstKind::kParameter ||
            filter_value->expression_kind == NativeExpressionAstKind::kVariable) &&
           filter_value->literal_kind.has_value()) ||
          (numeric_literal &&
           (filter_value->spelling.empty() ||
            (filter_value->spelling.size() > 1 &&
             filter_value->spelling.front() == '0') ||
            parsed.ec != std::errc{} ||
            parsed.ptr != filter_value->spelling.data() +
                              filter_value->spelling.size()))) {
        AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-EXPRESSION",
                              "catalog JOIN filter predicate is not exact");
        return RefusedBoundAst(std::move(bound));
      }
      std::unordered_set<std::uint32_t> admitted_expression_ids;
      std::unordered_set<std::uint32_t> active_expression_ids;
      const auto admit_expression =
          [&](auto&& self, const std::uint32_t expression_id,
              const std::size_t depth) -> bool {
        if (expression_id == 0 || depth > 32) return false;
        if (admitted_expression_ids.contains(expression_id)) return true;
        if (!active_expression_ids.insert(expression_id).second) return false;
        const auto* expression = find_expression(expression_id);
        if (expression == nullptr) return false;
        for (const auto child_id : expression->child_expression_ids) {
          if (!self(self, child_id, depth + 1)) return false;
        }
        active_expression_ids.erase(expression_id);
        admitted_expression_ids.insert(expression_id);
        return true;
      };
      bool exact_expression_inventory = true;
      for (const auto* source_relation : source_relations) {
        for (const auto expression_id : source_relation->output_expression_ids) {
          exact_expression_inventory =
              exact_expression_inventory &&
              admit_expression(admit_expression, expression_id, 1);
        }
      }
      for (const auto expression_id : catalog_join->predicate_expression_ids) {
        exact_expression_inventory =
            exact_expression_inventory &&
            admit_expression(admit_expression, expression_id, 1);
      }
      exact_expression_inventory =
          exact_expression_inventory &&
          admit_expression(admit_expression,
                           filter_predicate->expression_id, 1);
      if (project_composition) {
        for (const auto expression_id :
             project_relation->output_expression_ids) {
          exact_expression_inventory =
              exact_expression_inventory &&
              admit_expression(admit_expression, expression_id, 1);
        }
      }
      exact_expression_inventory =
          exact_expression_inventory &&
          admitted_expression_ids.size() == ast.expressions.size();
      if (!exact_expression_inventory) {
        AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-EXPRESSION",
                              "catalog JOIN filter expression inventory is not closed");
        return RefusedBoundAst(std::move(bound));
      }
    }
    if (!filter_composition && project_composition) {
      const auto find_expression = [&](const std::uint32_t expression_id) {
        const auto found = std::ranges::find_if(
            ast.expressions, [&](const auto& expression) {
              return expression.expression_id == expression_id;
            });
        return found == ast.expressions.end() ? nullptr : &*found;
      };
      std::unordered_set<std::uint32_t> admitted_expression_ids;
      std::unordered_set<std::uint32_t> active_expression_ids;
      const auto admit_expression =
          [&](auto&& self, const std::uint32_t expression_id,
              const std::size_t depth) -> bool {
        if (expression_id == 0 || depth > 32) return false;
        if (admitted_expression_ids.contains(expression_id)) return true;
        if (!active_expression_ids.insert(expression_id).second) return false;
        const auto* expression = find_expression(expression_id);
        if (expression == nullptr) return false;
        for (const auto child_id : expression->child_expression_ids) {
          if (!self(self, child_id, depth + 1)) return false;
        }
        active_expression_ids.erase(expression_id);
        admitted_expression_ids.insert(expression_id);
        return true;
      };
      bool exact_expression_inventory = true;
      for (const auto* source_relation : source_relations) {
        for (const auto expression_id : source_relation->output_expression_ids) {
          exact_expression_inventory =
              exact_expression_inventory &&
              admit_expression(admit_expression, expression_id, 1);
        }
      }
      for (const auto expression_id : catalog_join->predicate_expression_ids) {
        exact_expression_inventory =
            exact_expression_inventory &&
            admit_expression(admit_expression, expression_id, 1);
      }
      for (const auto expression_id : project_relation->output_expression_ids) {
        exact_expression_inventory =
            exact_expression_inventory &&
            admit_expression(admit_expression, expression_id, 1);
      }
      if (!exact_expression_inventory ||
          admitted_expression_ids.size() != ast.expressions.size()) {
        AddBoundAstDiagnostic(
            &bound, "QOW-DIAG-BOUNDAST-EXPRESSION",
            "catalog JOIN project expression inventory is not closed");
        return RefusedBoundAst(std::move(bound));
      }
    }
    const auto predicate_node_count = predicate_nodes.size();
    const auto source_projection_count = std::accumulate(
        context.catalog_relations.begin(), context.catalog_relations.end(),
        std::size_t{0}, [](const auto count, const auto& source) {
          return count + source.columns.size();
        });
    const auto join_output_count =
        left_only_join ? context.catalog_relations.front().columns.size()
                       : source_projection_count;
    if (catalog_join->predicate_expression_ids.size() !=
            static_cast<std::size_t>(predicate_join) ||
        context.descriptors.size() !=
            source_projection_count + predicate_node_count +
                (filter_composition ? std::size_t{2} : std::size_t{0}) ||
        context.outputs.size() !=
            source_projection_count + join_output_count +
                (filter_composition ? join_output_count : std::size_t{0}) +
                (project_composition ? project_identifiers.size()
                                     : std::size_t{0}) ||
        context.expressions.size() !=
            source_projection_count +
                (spatial_columnar_join ? ast.catalog_relation_sources.size()
                                       : std::size_t{0}) +
                (filter_composition ? std::size_t{1} : std::size_t{0})) {
      AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                            "catalog JOIN predicate binding is incomplete");
      return RefusedBoundAst(std::move(bound));
    }

    std::size_t binding_offset = 0;
    std::vector<std::uint32_t> joined_expression_ids;
    for (std::size_t source_ordinal = 0; source_ordinal < 2;
         ++source_ordinal) {
      const auto& ast_source = ast.catalog_relation_sources[source_ordinal];
      const auto& ast_relation = *source_relations[source_ordinal];
      const auto& relation_binding = context.catalog_relations[source_ordinal];
      if (ast_source.source_id != relation_binding.source_id ||
          ast_relation.relation_source_ids !=
              std::vector<std::uint32_t>{ast_source.source_id} ||
          relation_binding.resolution_state !=
              NativeCatalogRelationResolutionState::kBound ||
          !IsNonNullCanonicalUuid(relation_binding.object_uuid) ||
          !IsNonNullCanonicalUuid(relation_binding.resolved_schema_uuid) ||
          relation_binding.resolved_object_type.empty() ||
          relation_binding.catalog_generation_id == 0 ||
          relation_binding.security_epoch == 0 ||
          relation_binding.resource_epoch == 0 ||
          relation_binding.columns.empty()) {
        AddBoundAstDiagnostic(
            &bound, "QOW-DIAG-BOUNDAST-RELATION",
            "catalog CROSS JOIN source authority is incomplete");
        return RefusedBoundAst(std::move(bound));
      }

      BoundCatalogRelationSourceAstRecord bound_source;
      bound_source.source_id = ast_source.source_id;
      bound_source.source_kind = ast_source.source_kind;
      bound_source.resolution_state = relation_binding.resolution_state;
      bound_source.qualified_name = ast_source.qualified_name;
      bound_source.alias = ast_source.alias;
      bound_source.alias_is_explicit = ast_source.alias_is_explicit;
      if (spatial_columnar_join) {
        const bool spatial =
            ast_source.source_kind == NativeRelationSourceAstKind::kSpatial;
        const auto expected_operation =
            spatial ? std::string_view{"SPATIAL_SOURCE"}
                    : std::string_view{"COLUMNAR_SOURCE"};
        const auto expected_type =
            spatial ? std::string_view{"spatial_collection"}
                    : std::string_view{"logical_relation"};
        if (ast_source.model_family_id !=
                (spatial ? std::string_view{"spatial"}
                         : std::string_view{"columnar"}) ||
            ast_source.model_operation_ids !=
                std::vector<std::string>{std::string(expected_operation)} ||
            ast_source.model_operation_expression_ids.size() != 1 ||
            relation_binding.resolved_object_type != expected_type) {
          AddBoundAstDiagnostic(
              &bound, "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
              "model JOIN leaf operation or object profile is incomplete");
          return RefusedBoundAst(std::move(bound));
        }
        bound_source.model_family_id = ast_source.model_family_id;
        bound_source.model_operation_id = ast_source.model_operation_id;
        bound_source.model_operation_ids = ast_source.model_operation_ids;
        bound_source.model_source_alias = ast_source.model_source_alias;
      }
      bound_source.qualified_name_range = ast_source.qualified_name_range;
      bound_source.range = ast_source.range;
      bound_source.object_uuid = relation_binding.object_uuid;
      bound_source.resolved_object_type =
          relation_binding.resolved_object_type;
      bound_source.resolved_schema_uuid =
          relation_binding.resolved_schema_uuid;
      bound_source.parent_object_uuid = relation_binding.parent_object_uuid;
      bound_source.catalog_generation_id =
          relation_binding.catalog_generation_id;
      bound_source.security_epoch = relation_binding.security_epoch;
      bound_source.resource_epoch = relation_binding.resource_epoch;

      BoundRelationAstRecord bound_source_relation;
      bound_source_relation.relation_id = ast_relation.relation_id;
      bound_source_relation.relation_kind =
          NativeRelationAstKind::kCatalogSource;
      bound_source_relation.semantic_variant_id =
          spatial_columnar_join
              ? (ast_source.source_kind == NativeRelationSourceAstKind::kSpatial
                     ? "sblr.model-source.spatial.v1"
                     : "sblr.model-source.columnar.v1")
              : "catalog.relation-source.v1";
      bound_source_relation.bound_object_uuid = relation_binding.object_uuid;
      for (std::size_t ordinal = 0;
           ordinal < relation_binding.columns.size(); ++ordinal) {
        const auto& column = relation_binding.columns[ordinal];
        const auto& expression = context.expressions[binding_offset];
        const auto& output = context.outputs[binding_offset];
        const auto descriptor = descriptor_by_id.find(column.descriptor_id);
        const auto expected_binding =
            static_cast<std::uint32_t>(binding_offset + 1);
        if (column.ordinal != ordinal || column.column_uuid.empty() ||
            column.canonical_name_key.empty() ||
            descriptor == descriptor_by_id.end() ||
            expression.expression_id != expected_binding ||
            expression.descriptor_id != column.descriptor_id ||
            expression.function_uuid.has_value() ||
            expression.bound_name_uuid != column.column_uuid ||
            output.output_id != expected_binding ||
            output.expression_id != expression.expression_id ||
            output.output_name_utf8 != column.canonical_name_key ||
            output.descriptor_id != column.descriptor_id || !output.visible ||
            output.ordinal != ordinal ||
            output.relation_id != ast_relation.relation_id) {
          AddBoundAstDiagnostic(
              &bound, "QOW-DIAG-BOUNDAST-OUTPUT",
              "catalog CROSS JOIN source projection is not exact");
          return RefusedBoundAst(std::move(bound));
        }
        bound_source.columns.push_back(
            {column.ordinal, column.column_uuid, column.descriptor_id,
             column.canonical_name_key});
        BoundExpressionAstRecord bound_expression;
        bound_expression.expression_id = expression.expression_id;
        bound_expression.expression_kind =
            NativeExpressionAstKind::kIdentifier;
        bound_expression.result_descriptor_id = expression.descriptor_id;
        bound_expression.bound_name_uuid = expression.bound_name_uuid;
        bound.expressions.push_back(std::move(bound_expression));
        BoundOutputAstRecord bound_output;
        bound_output.output_id = output.output_id;
        bound_output.relation_id = output.relation_id;
        bound_output.expression_id = output.expression_id;
        bound_output.output_name_utf8 = output.output_name_utf8;
        bound_output.descriptor_id = output.descriptor_id;
        bound_output.visible = output.visible;
        bound_output.ordinal = output.ordinal;
        bound.outputs.push_back(std::move(bound_output));
        bound_source_relation.output_expression_ids.push_back(
            expression.expression_id);
        bound_source_relation.bound_expression_ids.push_back(
            expression.expression_id);
        if (!left_only_join || source_ordinal == 0) {
          joined_expression_ids.push_back(expression.expression_id);
        }
        ++binding_offset;
      }
      bound.catalog_relation_sources.push_back(std::move(bound_source));
      bound.relations.push_back(std::move(bound_source_relation));
    }

    if (spatial_columnar_join) {
      // Preserve the two independently typed model operation roots after the
      // public source projections and before the JOIN predicate cohort.
      for (std::size_t source_ordinal = 0; source_ordinal < 2;
           ++source_ordinal) {
        const auto& ast_source = ast.catalog_relation_sources[source_ordinal];
        const auto ast_operation = std::ranges::find_if(
            ast.expressions, [&](const auto& expression) {
              return expression.expression_id ==
                     ast_source.model_operation_expression_ids.front();
            });
        const auto& expression = context.expressions[binding_offset];
        const auto descriptor = descriptor_by_id.find(expression.descriptor_id);
        const auto expected_expression_id =
            static_cast<std::uint32_t>(binding_offset + 1);
        if (ast_operation == ast.expressions.end() ||
            ast_operation->expression_kind !=
                NativeExpressionAstKind::kFunctionCall ||
            ast_operation->operator_name !=
                ast_source.model_operation_ids.front() ||
            !ast_operation->child_expression_ids.empty() ||
            expression.expression_id != expected_expression_id ||
            descriptor == descriptor_by_id.end() ||
            descriptor->second->nullability != BoundNullability::kNonNull ||
            expression.function_uuid.has_value() ||
            expression.bound_name_uuid !=
                context.catalog_relations[source_ordinal].object_uuid) {
          AddBoundAstDiagnostic(
              &bound, "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
              "model JOIN operation root binding is incomplete");
          return RefusedBoundAst(std::move(bound));
        }
        BoundExpressionAstRecord bound_operation;
        bound_operation.expression_id = expression.expression_id;
        bound_operation.expression_kind = NativeExpressionAstKind::kFunctionCall;
        bound_operation.result_descriptor_id = expression.descriptor_id;
        bound_operation.bound_name_uuid = expression.bound_name_uuid;
        bound_operation.canonical_operator_name = ast_operation->operator_name;
        bound.expressions.push_back(std::move(bound_operation));
        bound.catalog_relation_sources[source_ordinal]
            .model_operation_expression_ids = {expression.expression_id};
        bound.relations[source_ordinal].bound_expression_ids.push_back(
            expression.expression_id);
        ++binding_offset;
      }
    }

    std::vector<std::uint32_t> joined_output_ids;
    std::optional<std::uint32_t> bound_join_predicate_id;
    if (predicate_join) {
      std::unordered_map<std::uint32_t, std::uint32_t>
          bound_predicate_expression_ids;
      for (std::size_t predicate_ordinal = 0;
           predicate_ordinal < predicate_nodes.size(); ++predicate_ordinal) {
        const auto* predicate_ast = predicate_nodes[predicate_ordinal];
        std::vector<std::uint32_t> child_expression_ids;
        const bool boolean_node = predicate_ast->operator_name == "AND" ||
                                  predicate_ast->operator_name == "OR";
        if (boolean_node) {
          for (const auto child_id : predicate_ast->child_expression_ids) {
            const auto bound_child =
                bound_predicate_expression_ids.find(child_id);
            if (bound_child == bound_predicate_expression_ids.end()) {
              AddBoundAstDiagnostic(
                  &bound, "QOW-DIAG-BOUNDAST-EXPRESSION",
                  "catalog JOIN Boolean child binding is absent");
              return RefusedBoundAst(std::move(bound));
            }
            child_expression_ids.push_back(bound_child->second);
          }
        } else {
          for (std::size_t source_ordinal = 0; source_ordinal < 2;
               ++source_ordinal) {
            const auto key_ast = std::ranges::find_if(
                ast.expressions, [&](const auto& expression) {
                  return expression.expression_id ==
                         predicate_ast->child_expression_ids[source_ordinal];
                });
            if (key_ast == ast.expressions.end() ||
                key_ast->expression_kind !=
                    NativeExpressionAstKind::kIdentifier) {
              AddBoundAstDiagnostic(
                  &bound, "QOW-DIAG-BOUNDAST-EXPRESSION",
                  "catalog JOIN key is not an identifier");
              return RefusedBoundAst(std::move(bound));
            }
            const auto& source =
                bound.catalog_relation_sources[source_ordinal];
            const auto key_name =
                key_ast->qualified_identifier.size() == 1
                    ? (key_ast->qualified_identifier.front().quoted
                           ? key_ast->qualified_identifier.front().spelling
                           : ToLowerAscii(
                                 key_ast->qualified_identifier.front().spelling))
                    : std::string{};
            const auto column = std::ranges::find_if(
                source.columns, [&](const auto& candidate) {
                  return !key_name.empty() &&
                         candidate.canonical_name_key == key_name;
                });
            if (column == source.columns.end()) {
              AddBoundAstDiagnostic(&bound,
                                    "QOW-DIAG-BOUNDAST-EXPRESSION",
                                    "catalog JOIN key is unresolved");
              return RefusedBoundAst(std::move(bound));
            }
            const auto expression = std::ranges::find_if(
                bound.expressions, [&](const auto& candidate) {
                  return candidate.result_descriptor_id ==
                             column->descriptor_id &&
                         candidate.bound_name_uuid == column->column_uuid;
                });
            if (expression == bound.expressions.end()) {
              AddBoundAstDiagnostic(
                  &bound, "QOW-DIAG-BOUNDAST-EXPRESSION",
                  "catalog JOIN key binding is absent");
              return RefusedBoundAst(std::move(bound));
            }
            child_expression_ids.push_back(expression->expression_id);
          }
        }
        const auto& boolean_descriptor =
            context.descriptors[source_projection_count + predicate_ordinal];
        BoundExpressionAstRecord predicate;
        predicate.expression_id =
            static_cast<std::uint32_t>(bound.expressions.size() + 1);
        predicate.expression_kind = NativeExpressionAstKind::kBinary;
        predicate.result_descriptor_id = boolean_descriptor.descriptor_id;
        predicate.child_expression_ids = std::move(child_expression_ids);
        predicate.canonical_operator_name = predicate_ast->operator_name;
        bound_predicate_expression_ids.emplace(predicate_ast->expression_id,
                                               predicate.expression_id);
        bound.expressions.push_back(std::move(predicate));
      }
      const auto bound_root = bound_predicate_expression_ids.find(
          catalog_join->predicate_expression_ids.front());
      if (bound_root == bound_predicate_expression_ids.end()) {
        AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-EXPRESSION",
                              "catalog JOIN predicate root binding is absent");
        return RefusedBoundAst(std::move(bound));
      }
      bound_join_predicate_id = bound_root->second;
    }

    for (std::size_t ordinal = 0; ordinal < joined_expression_ids.size();
         ++ordinal) {
      const auto& output = context.outputs[source_projection_count + ordinal];
      const auto expression_id = joined_expression_ids[ordinal];
      if (output.output_id != source_projection_count + ordinal + 1 ||
          output.expression_id != expression_id ||
          output.descriptor_id != expression_id || !output.visible ||
          output.ordinal != ordinal ||
          output.relation_id != catalog_join->relation_id) {
        AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-OUTPUT",
                              "catalog JOIN result projection is not exact");
        return RefusedBoundAst(std::move(bound));
      }
      BoundOutputAstRecord bound_output;
      bound_output.output_id = output.output_id;
      bound_output.relation_id = output.relation_id;
      bound_output.expression_id = output.expression_id;
      bound_output.output_name_utf8 = output.output_name_utf8;
      bound_output.descriptor_id = output.descriptor_id;
      bound_output.visible = output.visible;
      bound_output.ordinal = output.ordinal;
      bound.outputs.push_back(std::move(bound_output));
      joined_output_ids.push_back(output.output_id);
    }

    std::optional<std::uint32_t> bound_filter_predicate_id;
    std::vector<std::uint32_t> bound_project_expression_ids;
    std::vector<std::uint32_t> terminal_output_ids = joined_output_ids;
    if (filter_composition) {
      const BoundCatalogColumnAstRecord* selected_column = nullptr;
      std::uint32_t selected_expression_id = 0;
      std::size_t matching_column_count = 0;
      const auto visible_source_count = left_only_join ? std::size_t{1}
                                                       : std::size_t{2};
      for (std::size_t source_ordinal = 0;
           source_ordinal < visible_source_count; ++source_ordinal) {
        const auto& source = bound.catalog_relation_sources[source_ordinal];
        const auto& source_relation = bound.relations[source_ordinal];
        for (std::size_t ordinal = 0; ordinal < source.columns.size();
             ++ordinal) {
          const auto filter_name =
              filter_identifier->qualified_identifier.front().quoted
                  ? filter_identifier->qualified_identifier.front().spelling
                  : ToLowerAscii(
                        filter_identifier->qualified_identifier.front().spelling);
          if (source.columns[ordinal].canonical_name_key !=
              filter_name) {
            continue;
          }
          ++matching_column_count;
          selected_column = &source.columns[ordinal];
          selected_expression_id =
              source_relation.output_expression_ids[ordinal];
        }
      }
      const auto boolean_descriptor_index =
          source_projection_count + predicate_node_count;
      const auto* boolean_descriptor =
          boolean_descriptor_index < context.descriptors.size()
              ? &context.descriptors[boolean_descriptor_index]
              : nullptr;
      const bool literal_value =
          filter_value->expression_kind == NativeExpressionAstKind::kLiteral;
      const bool parameter_value =
          filter_value->expression_kind == NativeExpressionAstKind::kParameter;
      const auto matching_operand_count = std::ranges::count_if(
          context.expressions, [&](const auto& expression) {
            return literal_value
                       ? expression.structural_literal_occurrence_id ==
                             filter_value->structural_literal_occurrence_id
                   : parameter_value
                       ? expression.structural_parameter_occurrence_id ==
                             filter_value->structural_parameter_occurrence_id
                       : expression.structural_variable_occurrence_id ==
                             filter_value->structural_variable_occurrence_id;
          });
      const auto operand_binding = std::ranges::find_if(
          context.expressions, [&](const auto& expression) {
            return literal_value
                       ? expression.structural_literal_occurrence_id ==
                             filter_value->structural_literal_occurrence_id
                   : parameter_value
                       ? expression.structural_parameter_occurrence_id ==
                             filter_value->structural_parameter_occurrence_id
                       : expression.structural_variable_occurrence_id ==
                             filter_value->structural_variable_occurrence_id;
          });
      const auto operand_descriptor =
          operand_binding == context.expressions.end()
              ? descriptor_by_id.end()
              : descriptor_by_id.find(operand_binding->descriptor_id);
      const auto selected_descriptor =
          selected_column == nullptr
              ? descriptor_by_id.end()
              : descriptor_by_id.find(selected_column->descriptor_id);
      const auto expected_operand_expression_id =
          static_cast<std::uint64_t>(bound.expressions.size()) + 1;
      const bool exact_operand_occurrence =
          literal_value
              ? filter_value->structural_literal_occurrence_id != 0 &&
                    filter_value->structural_parameter_occurrence_id == 0 &&
                    filter_value->structural_variable_occurrence_id == 0 &&
                    operand_binding != context.expressions.end() &&
                    operand_binding->structural_literal_occurrence_id ==
                        filter_value->structural_literal_occurrence_id &&
                    operand_binding->structural_parameter_occurrence_id == 0 &&
                    operand_binding->structural_variable_occurrence_id == 0
          : parameter_value
              ? filter_value->structural_literal_occurrence_id == 0 &&
                    filter_value->structural_parameter_occurrence_id != 0 &&
                    filter_value->structural_variable_occurrence_id == 0 &&
                    operand_binding != context.expressions.end() &&
                    operand_binding->structural_literal_occurrence_id == 0 &&
                    operand_binding->structural_parameter_occurrence_id ==
                        filter_value->structural_parameter_occurrence_id &&
                    operand_binding->structural_variable_occurrence_id == 0
              : filter_value->structural_literal_occurrence_id == 0 &&
                    filter_value->structural_parameter_occurrence_id == 0 &&
                    filter_value->structural_variable_occurrence_id != 0 &&
                    operand_binding != context.expressions.end() &&
                    operand_binding->structural_literal_occurrence_id == 0 &&
                    operand_binding->structural_parameter_occurrence_id == 0 &&
                    operand_binding->structural_variable_occurrence_id ==
                        filter_value->structural_variable_occurrence_id;
      if (matching_column_count != 1 || selected_column == nullptr ||
          selected_expression_id == 0 || boolean_descriptor == nullptr ||
          boolean_descriptor->descriptor_id !=
              boolean_descriptor_index + 1 ||
          boolean_descriptor->nullability != BoundNullability::kNullable ||
          boolean_descriptor->collation_uuid.has_value() ||
          boolean_descriptor->timezone_profile_id.has_value() ||
          matching_operand_count != 1 ||
          !exact_operand_occurrence ||
          operand_binding == context.expressions.end() ||
          operand_binding->function_uuid.has_value() ||
          operand_binding->bound_name_uuid.has_value() ||
          operand_descriptor == descriptor_by_id.end() ||
          operand_descriptor->first != boolean_descriptor->descriptor_id + 1 ||
          selected_descriptor == descriptor_by_id.end() ||
          operand_descriptor->second->type_uuid !=
              selected_descriptor->second->type_uuid ||
          operand_descriptor->second->collation_uuid.has_value() ||
          operand_descriptor->second->timezone_profile_id.has_value() ||
          selected_descriptor->second->collation_uuid.has_value() ||
          selected_descriptor->second->timezone_profile_id.has_value() ||
          expected_operand_expression_id >
              std::numeric_limits<std::uint32_t>::max() ||
          operand_binding->expression_id != expected_operand_expression_id) {
        AddBoundAstDiagnostic(
            &bound, "QOW-DIAG-BOUNDAST-EXPRESSION",
            "catalog JOIN filter value or persisted column binding is incomplete");
        return RefusedBoundAst(std::move(bound));
      }

      BoundExpressionAstRecord operand;
      operand.expression_id = operand_binding->expression_id;
      operand.expression_kind = filter_value->expression_kind;
      operand.literal_kind = filter_value->literal_kind;
      operand.result_descriptor_id = operand_binding->descriptor_id;
      if (literal_value) operand.literal_or_parameter_ref = filter_value->spelling;
      operand.structural_literal_occurrence_id =
          filter_value->structural_literal_occurrence_id;
      operand.structural_parameter_occurrence_id =
          filter_value->structural_parameter_occurrence_id;
      operand.structural_variable_occurrence_id =
          filter_value->structural_variable_occurrence_id;
      const auto operand_expression_id = operand.expression_id;
      bound.expressions.push_back(std::move(operand));
      if (bound.expressions.size() >=
          std::numeric_limits<std::uint32_t>::max()) {
        AddBoundAstDiagnostic(&bound, "RESOURCE.BUDGET_EXCEEDED",
                              "catalog JOIN filter expression identity overflowed");
        return RefusedBoundAst(std::move(bound));
      }
      BoundExpressionAstRecord predicate;
      predicate.expression_id =
          static_cast<std::uint32_t>(bound.expressions.size() + 1);
      predicate.expression_kind = NativeExpressionAstKind::kBinary;
      predicate.result_descriptor_id = boolean_descriptor->descriptor_id;
      predicate.child_expression_ids = {selected_expression_id,
                                        operand_expression_id};
      predicate.canonical_operator_name = filter_predicate->operator_name;
      bound_filter_predicate_id = predicate.expression_id;
      bound.expressions.push_back(std::move(predicate));

      terminal_output_ids.clear();
      const auto filter_output_offset =
          source_projection_count + join_output_count;
      for (std::size_t ordinal = 0; ordinal < join_output_count; ++ordinal) {
        const auto& join_output =
            context.outputs[source_projection_count + ordinal];
        const auto& output = context.outputs[filter_output_offset + ordinal];
        const auto expected_output_id =
            static_cast<std::uint32_t>(filter_output_offset + ordinal + 1);
        if (output.output_id != expected_output_id ||
            output.relation_id != filter_relation->relation_id ||
            output.expression_id != join_output.expression_id ||
            output.output_name_utf8 != join_output.output_name_utf8 ||
            output.descriptor_id != join_output.descriptor_id ||
            !output.visible || output.ordinal != ordinal) {
          AddBoundAstDiagnostic(
              &bound, "QOW-DIAG-BOUNDAST-OUTPUT",
              "catalog JOIN filter output does not preserve JOIN lineage");
          return RefusedBoundAst(std::move(bound));
        }
        BoundOutputAstRecord bound_output;
        bound_output.output_id = output.output_id;
        bound_output.relation_id = output.relation_id;
        bound_output.expression_id = output.expression_id;
        bound_output.output_name_utf8 = output.output_name_utf8;
        bound_output.descriptor_id = output.descriptor_id;
        bound_output.visible = output.visible;
        bound_output.ordinal = output.ordinal;
        bound.outputs.push_back(std::move(bound_output));
        terminal_output_ids.push_back(output.output_id);
      }
    }
    if (project_composition) {
      std::vector<NativeOutputBindingInput> predecessor_outputs;
      for (const auto& output : context.outputs) {
        if (output.relation_id == project_predecessor_relation_id) {
          predecessor_outputs.push_back(output);
        }
      }
      std::ranges::sort(predecessor_outputs, {},
                        &NativeOutputBindingInput::ordinal);
      if (project_identifiers.empty() || predecessor_outputs.empty() ||
          project_identifiers.size() >= predecessor_outputs.size()) {
        AddBoundAstDiagnostic(
            &bound, "QOW-DIAG-BOUNDAST-OUTPUT",
            "catalog JOIN project width is not a strict visible subset");
        return RefusedBoundAst(std::move(bound));
      }
      const auto project_output_offset =
          source_projection_count + join_output_count +
          (filter_composition ? join_output_count : std::size_t{0});
      std::unordered_set<std::uint32_t> selected_predecessor_output_ids;
      terminal_output_ids.clear();
      for (std::size_t ordinal = 0; ordinal < project_identifiers.size();
           ++ordinal) {
        const auto* identifier = project_identifiers[ordinal];
        const auto identifier_key =
            identifier->qualified_identifier.front().quoted
                ? identifier->qualified_identifier.front().spelling
                : ToLowerAscii(
                      identifier->qualified_identifier.front().spelling);
        const auto matching_count = std::ranges::count_if(
            predecessor_outputs, [&](const auto& output) {
              return output.output_name_utf8 == identifier_key;
            });
        const auto selected = std::ranges::find_if(
            predecessor_outputs, [&](const auto& output) {
              return output.output_name_utf8 == identifier_key;
            });
        const auto& output = context.outputs[project_output_offset + ordinal];
        const auto expected_output_id =
            static_cast<std::uint32_t>(project_output_offset + ordinal + 1);
        if (matching_count != 1 || selected == predecessor_outputs.end() ||
            !selected_predecessor_output_ids.insert(selected->output_id)
                 .second ||
            output.output_id != expected_output_id ||
            output.relation_id != project_relation->relation_id ||
            output.expression_id != selected->expression_id ||
            output.output_name_utf8 != selected->output_name_utf8 ||
            output.descriptor_id != selected->descriptor_id ||
            !output.visible || output.ordinal != ordinal) {
          AddBoundAstDiagnostic(
              &bound, "QOW-DIAG-BOUNDAST-OUTPUT",
              "catalog JOIN project output is unresolved or ambiguous");
          return RefusedBoundAst(std::move(bound));
        }
        BoundOutputAstRecord bound_output;
        bound_output.output_id = output.output_id;
        bound_output.relation_id = output.relation_id;
        bound_output.expression_id = output.expression_id;
        bound_output.output_name_utf8 = output.output_name_utf8;
        bound_output.descriptor_id = output.descriptor_id;
        bound_output.visible = output.visible;
        bound_output.ordinal = output.ordinal;
        bound.outputs.push_back(std::move(bound_output));
        bound_project_expression_ids.push_back(output.expression_id);
        terminal_output_ids.push_back(output.output_id);
      }
    }

    BoundRelationAstRecord bound_join;
    bound_join.relation_id = catalog_join->relation_id;
    bound_join.relation_kind = NativeRelationAstKind::kJoin;
    bound_join.input_relation_ids = catalog_join->input_relation_ids;
    bound_join.output_expression_ids = joined_expression_ids;
    if (predicate_join) {
      bound_join.predicate_expression_ids = {*bound_join_predicate_id};
      bound_join.bound_expression_ids =
          bound_join.predicate_expression_ids;
    }
    bound_join.semantic_variant_id =
        context.relations.front().semantic_variant_id;
    bound.relations.push_back(std::move(bound_join));
    if (filter_composition) {
      BoundRelationAstRecord bound_filter;
      bound_filter.relation_id = filter_relation->relation_id;
      bound_filter.relation_kind = NativeRelationAstKind::kFilter;
      bound_filter.input_relation_ids = {catalog_join->relation_id};
      bound_filter.output_expression_ids = joined_expression_ids;
      bound_filter.predicate_expression_ids = {*bound_filter_predicate_id};
      bound_filter.bound_expression_ids = bound_filter.predicate_expression_ids;
      bound_filter.semantic_variant_id =
          "filter.catalog-column-numeric-comparison.v1";
      bound.relations.push_back(std::move(bound_filter));
    }
    if (project_composition) {
      BoundRelationAstRecord bound_project;
      bound_project.relation_id = project_relation->relation_id;
      bound_project.relation_kind = NativeRelationAstKind::kProject;
      bound_project.input_relation_ids = {project_predecessor_relation_id};
      bound_project.output_expression_ids = bound_project_expression_ids;
      bound_project.bound_expression_ids = bound_project_expression_ids;
      bound_project.semantic_variant_id =
          "project.catalog-visible-columns.v1";
      bound.relations.push_back(std::move(bound_project));
    }

    bound.descriptors.reserve(context.descriptors.size());
    for (const auto& descriptor : context.descriptors) {
      bound.descriptors.push_back(
          {descriptor.descriptor_id, descriptor.descriptor_uuid,
           descriptor.type_uuid, descriptor.nullability,
           descriptor.collation_uuid, descriptor.timezone_profile_id,
           descriptor.width_precision_scale});
    }
    std::ranges::sort(bound.descriptors, {},
                      &BoundDescriptorAstRecord::descriptor_id);
    BoundScopeAstRecord scope;
    scope.scope_id = 1;
    scope.visible_relation_ids = {ast.root_relation_id};
    scope.visible_projection_ids = std::move(terminal_output_ids);
    scope.catalog_epoch_uuid = context.catalog_epoch_uuid;
    bound.scopes.push_back(std::move(scope));
    bound.bound_ast_uuid = context.bound_ast_uuid;
    bound.security_context_uuid = context.security_context_uuid;
    bound.root_relation_id = ast.root_relation_id;
    bound.root_scope_id = 1;
    bound.bound = true;
    return bound;
  }
  if (has_catalog_relation_ast || !ast.catalog_relation_sources.empty()) {
    const auto source_relation = std::ranges::find_if(
        ast.relations, [](const auto& candidate) {
          return candidate.relation_kind ==
                 NativeRelationAstKind::kCatalogSource;
        });
    const auto limit_relation = std::ranges::find_if(
        ast.relations, [](const auto& candidate) {
          return candidate.relation_kind == NativeRelationAstKind::kLimit;
        });
    const auto filter_relation = std::ranges::find_if(
        ast.relations, [](const auto& candidate) {
          return candidate.relation_kind == NativeRelationAstKind::kFilter;
        });
    const auto aggregate_relation = std::ranges::find_if(
        ast.relations, [](const auto& candidate) {
          return candidate.relation_kind == NativeRelationAstKind::kAggregate;
        });
    const auto project_relation = std::ranges::find_if(
        ast.relations, [](const auto& candidate) {
          return candidate.relation_kind == NativeRelationAstKind::kProject;
        });
    const auto sort_relation = std::ranges::find_if(
        ast.relations, [](const auto& candidate) {
          return candidate.relation_kind == NativeRelationAstKind::kSort;
        });
    const bool filter_composition =
        source_relation != ast.relations.end() &&
        filter_relation != ast.relations.end() &&
        filter_relation->input_relation_ids ==
            std::vector<std::uint32_t>{source_relation->relation_id};
    const bool sort_composition =
        source_relation != ast.relations.end() &&
        sort_relation != ast.relations.end() &&
        sort_relation->input_relation_ids ==
            std::vector<std::uint32_t>{
                filter_composition ? filter_relation->relation_id
                                   : source_relation->relation_id};
    const auto expected_project_predecessor =
        sort_composition
            ? sort_relation->relation_id
            : (filter_composition
                   ? filter_relation->relation_id
                   : (source_relation == ast.relations.end()
                          ? 0
                          : source_relation->relation_id));
    const bool project_composition =
        project_relation != ast.relations.end() &&
        project_relation->input_relation_ids ==
            std::vector<std::uint32_t>{expected_project_predecessor} &&
        !project_relation->output_expression_ids.empty();
    const auto expected_aggregate_predecessor =
        filter_composition
            ? filter_relation->relation_id
            : (source_relation == ast.relations.end()
                   ? 0
                   : source_relation->relation_id);
    const bool aggregate_composition =
        aggregate_relation != ast.relations.end() &&
        aggregate_relation->input_relation_ids ==
            std::vector<std::uint32_t>{expected_aggregate_predecessor};
    const bool limit_composition = limit_relation != ast.relations.end();
    const auto expected_root =
        limit_composition
            ? limit_relation->relation_id
            : (aggregate_composition
                   ? aggregate_relation->relation_id
                   : (project_composition
                   ? project_relation->relation_id
                   : (sort_composition
                          ? sort_relation->relation_id
                          : (filter_composition
                                 ? filter_relation->relation_id
                                 : (source_relation == ast.relations.end()
                                        ? 0
                                        : source_relation->relation_id)))));
    const auto expected_limit_input =
        aggregate_composition
            ? aggregate_relation->relation_id
            : (project_composition
            ? project_relation->relation_id
            : (sort_composition
                   ? sort_relation->relation_id
                   : (filter_composition
                          ? filter_relation->relation_id
                          : (source_relation == ast.relations.end()
                                 ? 0
                                 : source_relation->relation_id))));
    const bool catalog_chain =
        source_relation != ast.relations.end() &&
        ast.relations.size() ==
            1 + static_cast<std::size_t>(filter_composition) +
                static_cast<std::size_t>(sort_composition) +
                static_cast<std::size_t>(project_composition) +
                static_cast<std::size_t>(aggregate_composition) +
                static_cast<std::size_t>(limit_composition) &&
        ast.root_relation_id == expected_root &&
        (!limit_composition ||
         limit_relation->input_relation_ids ==
             std::vector<std::uint32_t>{expected_limit_input});
    const bool expanded_projection =
        !context.expressions.empty() || !context.outputs.empty();
    if (!has_catalog_relation_ast ||
        !catalog_chain ||
        ast.catalog_relation_sources.size() != 1 ||
        ast.root_relation_id == 0 || !ast.values_rows.empty() ||
        !ast.grouping_sets.empty() || ast.expressions.empty() ||
        (aggregate_composition
             ? (context.relations.size() != 1 ||
                context.relations.front().relation_id !=
                    aggregate_relation->relation_id ||
                (context.relations.front().semantic_variant_id !=
                     "aggregate.global-count-star.v1" &&
                 context.relations.front().semantic_variant_id !=
                     "aggregate.global-count-expression.v1" &&
                 context.relations.front().semantic_variant_id !=
                     "aggregate.global-sum-expression.v1" &&
                 context.relations.front().semantic_variant_id !=
                     "aggregate.global-avg-expression.v1" &&
                 context.relations.front().semantic_variant_id !=
                     "aggregate.global-min-expression.v1" &&
                 context.relations.front().semantic_variant_id !=
                     "aggregate.global-max-expression.v1" &&
                 context.relations.front().semantic_variant_id !=
                     "aggregate.global-bool-and-expression.v1" &&
                 context.relations.front().semantic_variant_id !=
                     "aggregate.global-bool-or-expression.v1" &&
                 context.relations.front().semantic_variant_id !=
                     "aggregate.global-every-expression.v1" &&
                 context.relations.front().semantic_variant_id !=
                     "aggregate.global-corr-expression.v1" &&
                 context.relations.front().semantic_variant_id !=
                     "aggregate.global-covar-pop-expression.v1" &&
                 context.relations.front().semantic_variant_id !=
                     "aggregate.global-covar-samp-expression.v1" &&
                 context.relations.front().semantic_variant_id !=
                     "aggregate.global-regr-count-expression.v1" &&
                 context.relations.front().semantic_variant_id !=
                     "aggregate.global-regr-avgx-expression.v1" &&
                 context.relations.front().semantic_variant_id !=
                     "aggregate.global-regr-avgy-expression.v1" &&
                 context.relations.front().semantic_variant_id !=
                     "aggregate.global-regr-intercept-expression.v1" &&
                 context.relations.front().semantic_variant_id !=
                     "aggregate.global-regr-r2-expression.v1" &&
                 context.relations.front().semantic_variant_id !=
                     "aggregate.global-regr-slope-expression.v1" &&
                 context.relations.front().semantic_variant_id !=
                     "aggregate.global-regr-sxx-expression.v1" &&
                 context.relations.front().semantic_variant_id !=
                     "aggregate.global-regr-sxy-expression.v1" &&
                 context.relations.front().semantic_variant_id !=
                     "aggregate.global-regr-syy-expression.v1" &&
                 context.relations.front().semantic_variant_id !=
                     "aggregate.global-stddev-pop-expression.v1" &&
                 context.relations.front().semantic_variant_id !=
                     "aggregate.global-variance-pop-expression.v1" &&
                 context.relations.front().semantic_variant_id !=
                     "aggregate.global-stddev-expression.v1" &&
                 context.relations.front().semantic_variant_id !=
                     "aggregate.global-variance-expression.v1" &&
                 context.relations.front().semantic_variant_id !=
                     "aggregate.global-stddev-samp-expression.v1" &&
                 context.relations.front().semantic_variant_id !=
                     "aggregate.global-variance-samp-expression.v1" &&
                 context.relations.front().semantic_variant_id !=
                     "aggregate.global-approx-count-distinct-expression.v1" &&
                 context.relations.front().semantic_variant_id !=
                     "aggregate.global-approx-median-expression.v1" &&
                 context.relations.front().semantic_variant_id !=
                     "aggregate.global-string-agg-expression.v1" &&
                 context.relations.front().semantic_variant_id !=
                     "aggregate.global-listagg-ordered-expression.v1" &&
                 context.relations.front().semantic_variant_id !=
                     "aggregate.global-mode-ordered-expression.v1" &&
                 context.relations.front().semantic_variant_id !=
                     "aggregate.global-percentile-cont-ordered-expression.v1" &&
                 context.relations.front().semantic_variant_id !=
                     "aggregate.global-percentile-disc-ordered-expression.v1" &&
                 context.relations.front().semantic_variant_id !=
                     "aggregate.global-rank-hypothetical-expression.v1" &&
                 context.relations.front().semantic_variant_id !=
                     "aggregate.global-dense-rank-hypothetical-expression.v1" &&
                 context.relations.front().semantic_variant_id !=
                     "aggregate.global-percent-rank-hypothetical-expression.v1" &&
                 context.relations.front().semantic_variant_id !=
                     "aggregate.global-cume-dist-hypothetical-expression.v1" &&
                 context.relations.front().semantic_variant_id !=
                     "aggregate.global-approx-percentile-cont-ordered-expression.v1" &&
                 context.relations.front().semantic_variant_id !=
                     "aggregate.global-approx-percentile-disc-ordered-expression.v1" &&
                 context.relations.front().semantic_variant_id !=
                     "aggregate.global-array-agg-ordered-expression.v1" &&
                 context.relations.front().semantic_variant_id !=
                     "aggregate.global-json-agg-ordered-expression.v1" &&
                 context.relations.front().semantic_variant_id !=
                     "aggregate.global-json-object-agg-ordered-expression.v1" &&
                 context.relations.front().semantic_variant_id !=
                     "aggregate.global-approx-top-k-expression.v1"))
             : !context.relations.empty()) ||
        (aggregate_composition && (sort_composition || project_composition)) ||
        context.catalog_relations.size() != 1 ||
        (expanded_projection &&
         (context.expressions.empty() || context.outputs.empty()))) {
      AddBoundAstDiagnostic(
          &bound, "QOW-DIAG-BOUNDAST-RELATION",
          "catalog relation binding requires one source and a complete projection");
      return RefusedBoundAst(std::move(bound));
    }

    const auto& relation = *source_relation;
    const auto& source = ast.catalog_relation_sources.front();
    const auto& visible_projection_expression_ids =
        aggregate_composition
            ? aggregate_relation->output_expression_ids
            : (project_composition ? project_relation->output_expression_ids
                                   : relation.output_expression_ids);
    const auto wildcard = std::ranges::find_if(
        ast.expressions, [&](const auto& candidate) {
          return relation.output_expression_ids.size() == 1 &&
                 candidate.expression_id ==
                     relation.output_expression_ids.front() &&
                 candidate.expression_kind ==
                     NativeExpressionAstKind::kWildcard;
        });
    const bool wildcard_projection = wildcard != ast.expressions.end();
    std::vector<const NativeExpressionAstNode*> projection_identifiers;
    std::vector<const NativeExpressionAstNode*> source_identifiers;
    std::unordered_set<std::string> projection_names;
    if (!wildcard_projection) {
      if (!aggregate_composition) {
        projection_identifiers.reserve(visible_projection_expression_ids.size());
        for (const auto expression_id : visible_projection_expression_ids) {
          const auto expression = std::ranges::find_if(
              ast.expressions, [&](const auto& candidate) {
                return candidate.expression_id == expression_id;
              });
          if (expression == ast.expressions.end() ||
              expression->expression_kind !=
                  NativeExpressionAstKind::kIdentifier ||
              expression->spelling.empty() ||
              !expression->child_expression_ids.empty() ||
              !expression->operator_name.empty() ||
              !projection_names.insert(expression->spelling).second) {
            AddBoundAstDiagnostic(
                &bound, "QOW-DIAG-BOUNDAST-EXPRESSION",
                "catalog projection requires unique source identifiers");
            return RefusedBoundAst(std::move(bound));
          }
          projection_identifiers.push_back(&*expression);
        }
      }
      source_identifiers.reserve(relation.output_expression_ids.size());
      std::unordered_set<std::string> source_names;
      for (const auto expression_id : relation.output_expression_ids) {
        const auto expression = std::ranges::find_if(
            ast.expressions, [&](const auto& candidate) {
              return candidate.expression_id == expression_id;
            });
        if (expression == ast.expressions.end() ||
            expression->expression_kind !=
                NativeExpressionAstKind::kIdentifier ||
            expression->spelling.empty() ||
            !expression->child_expression_ids.empty() ||
            !expression->operator_name.empty() ||
            !source_names.insert(expression->spelling).second) {
          AddBoundAstDiagnostic(
              &bound, "QOW-DIAG-BOUNDAST-EXPRESSION",
              "catalog source requires unique persisted identifiers");
          return RefusedBoundAst(std::move(bound));
        }
        source_identifiers.push_back(&*expression);
      }
    }
    const bool projection_syntax_exact =
        wildcard_projection
            ? (!wildcard->literal_kind.has_value() &&
               wildcard->child_expression_ids.empty() &&
               wildcard->spelling == "*" && wildcard->operator_name.empty())
            : (aggregate_composition ? !source_identifiers.empty()
                                     : !projection_identifiers.empty());
    if ((!filter_composition && !project_composition && !aggregate_composition &&
         !limit_composition &&
         relation.relation_id != ast.root_relation_id) ||
        relation.relation_kind != NativeRelationAstKind::kCatalogSource ||
        !relation.input_relation_ids.empty() ||
        relation.relation_source_ids !=
            std::vector<std::uint32_t>{source.source_id} ||
        !relation.values_row_ids.empty() ||
        relation.output_expression_ids.empty() ||
        relation.aggregate_grouping_form != NativeAggregateGroupingForm::kNone ||
        relation.aggregate_projection_form !=
            NativeAggregateProjectionForm::kNone ||
        !relation.grouping_key_expression_ids.empty() ||
        !relation.aggregate_expression_ids.empty() ||
        !relation.predicate_expression_ids.empty() ||
        !relation.limit_expression_ids.empty() ||
        !relation.ordering_terms.empty() || source.source_id == 0 ||
        source.source_kind != NativeRelationSourceAstKind::kCatalogRelation ||
        source.qualified_name.empty() ||
        std::ranges::any_of(source.qualified_name, [](const auto& component) {
          return component.spelling.empty();
        }) ||
        (source.alias.has_value() && source.alias->spelling.empty()) ||
        (!source.alias.has_value() && source.alias_is_explicit) ||
        !projection_syntax_exact) {
      AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                            "catalog relation AST is outside the bounded source profile");
      return RefusedBoundAst(std::move(bound));
    }
    const NativeExpressionAstNode* filter_predicate = nullptr;
    const NativeExpressionAstNode* filter_identifier = nullptr;
    const NativeExpressionAstNode* filter_literal = nullptr;
    const NativeExpressionAstNode* global_aggregate_expression = nullptr;
    std::vector<const NativeExpressionAstNode*> global_aggregate_arguments;
    const NativeExpressionAstNode* global_aggregate_direct_literal = nullptr;
    if (aggregate_composition) {
      if (aggregate_relation->relation_source_ids.size() != 0 ||
          !aggregate_relation->values_row_ids.empty() ||
          aggregate_relation->aggregate_grouping_form !=
              NativeAggregateGroupingForm::kNone ||
          aggregate_relation->aggregate_projection_form !=
              NativeAggregateProjectionForm::kGlobalUnary ||
          !aggregate_relation->grouping_key_expression_ids.empty() ||
          aggregate_relation->aggregate_expression_ids.size() != 1 ||
          aggregate_relation->output_expression_ids !=
              aggregate_relation->aggregate_expression_ids ||
          !aggregate_relation->predicate_expression_ids.empty() ||
          !aggregate_relation->limit_expression_ids.empty() ||
          !aggregate_relation->ordering_terms.empty()) {
        AddBoundAstDiagnostic(
            &bound, "QOW-DIAG-BOUNDAST-RELATION",
            "catalog global aggregate AST is outside the bounded profile");
        return RefusedBoundAst(std::move(bound));
      }
      const auto expression = std::ranges::find_if(
          ast.expressions, [&](const auto& candidate) {
            return candidate.expression_id ==
                   aggregate_relation->aggregate_expression_ids.front();
          });
      const auto function =
          expression == ast.expressions.end()
              ? std::string{}
              : ToUpperAscii(expression->operator_name);
      const bool count_function = function == "COUNT";
      const bool sum_function = function == "SUM";
      const bool avg_function = function == "AVG";
      const bool min_function = function == "MIN";
      const bool max_function = function == "MAX";
      const bool pair_function =
          function == "CORR" || function == "COVAR_POP" ||
          function == "COVAR_SAMP" || function == "REGR_COUNT" ||
          function == "REGR_AVGX" || function == "REGR_AVGY" ||
          function == "REGR_INTERCEPT" || function == "REGR_R2" ||
          function == "REGR_SLOPE" || function == "REGR_SXX" ||
          function == "REGR_SXY" || function == "REGR_SYY";
      const bool string_agg_function = function == "STRING_AGG";
      const bool listagg_function = function == "LISTAGG";
      const bool mode_function = function == "MODE";
      const bool percentile_function =
          function == "PERCENTILE_CONT" || function == "PERCENTILE_DISC" ||
          function == "APPROX_PERCENTILE_CONT" ||
          function == "APPROX_PERCENTILE_DISC";
      const bool hypothetical_function =
          function == "RANK" || function == "DENSE_RANK" ||
          function == "PERCENT_RANK" || function == "CUME_DIST";
      const bool ordered_single_collection_function =
          function == "ARRAY_AGG" || function == "JSON_AGG";
      const bool json_object_aggregate_function =
          function == "JSON_OBJECT_AGG";
      const bool ordered_collection_function =
          ordered_single_collection_function ||
          json_object_aggregate_function;
      const bool approx_top_k_function = function == "APPROX_TOP_K";
      const bool direct_numeric_ordered_function =
          percentile_function || hypothetical_function;
      const bool expression_function =
          sum_function || avg_function || min_function || max_function ||
          function == "BOOL_AND" || function == "BOOL_OR" ||
          function == "EVERY" ||
          function == "STDDEV_POP" || function == "VARIANCE_POP" ||
          function == "STDDEV" || function == "VARIANCE" ||
          function == "STDDEV_SAMP" || function == "VARIANCE_SAMP" ||
          function == "APPROX_COUNT_DISTINCT" ||
          function == "APPROX_MEDIAN" ||
          string_agg_function || listagg_function || mode_function ||
          direct_numeric_ordered_function || pair_function ||
          ordered_collection_function || approx_top_k_function;
      if (expression == ast.expressions.end() ||
          expression->expression_kind !=
              NativeExpressionAstKind::kFunctionCall ||
          (!count_function && !expression_function) ||
          (!pair_function && !string_agg_function && !listagg_function &&
           !ordered_collection_function && !approx_top_k_function &&
           !direct_numeric_ordered_function &&
           expression->child_expression_ids.size() > 1) ||
          (expression_function && expression->child_expression_ids.size() !=
                                      ((listagg_function ||
                                        json_object_aggregate_function)
                                           ? 3
                                           : ((pair_function ||
                                               string_agg_function ||
                                               ordered_single_collection_function ||
                                               approx_top_k_function ||
                                               direct_numeric_ordered_function)
                                                  ? 2
                                                  : 1))) ||
          expression->literal_kind.has_value()) {
        AddBoundAstDiagnostic(
            &bound, "QOW-DIAG-BOUNDAST-EXPRESSION",
            "catalog global aggregate requires an exact supported unary binding");
        return RefusedBoundAst(std::move(bound));
      }
      for (std::size_t ordinal = 0;
           ordinal < expression->child_expression_ids.size(); ++ordinal) {
        const auto argument_id = expression->child_expression_ids[ordinal];
        const auto argument = std::ranges::find_if(
            ast.expressions, [&](const auto& candidate) {
              return candidate.expression_id == argument_id;
            });
        const bool direct_text =
            (string_agg_function || listagg_function) && ordinal == 1;
        const bool direct_numeric =
            (direct_numeric_ordered_function || approx_top_k_function) &&
            ordinal == 0;
        if (direct_text || direct_numeric) {
          if (argument == ast.expressions.end() ||
              argument->expression_kind != NativeExpressionAstKind::kLiteral ||
              argument->literal_kind !=
                  (direct_text ? NativeLiteralAstKind::kString
                               : NativeLiteralAstKind::kNumeric) ||
              !argument->child_expression_ids.empty() ||
              !argument->operator_name.empty()) {
            AddBoundAstDiagnostic(
                &bound, "QOW-DIAG-BOUNDAST-EXPRESSION",
                "catalog aggregate direct argument is not an exact literal");
            return RefusedBoundAst(std::move(bound));
          }
          global_aggregate_direct_literal = &*argument;
          continue;
        }
        if (argument == ast.expressions.end() ||
            argument->expression_kind != NativeExpressionAstKind::kIdentifier ||
            argument->spelling.empty() ||
            std::ranges::find(relation.output_expression_ids,
                              argument->expression_id) ==
                relation.output_expression_ids.end()) {
          AddBoundAstDiagnostic(
              &bound, "QOW-DIAG-BOUNDAST-EXPRESSION",
              "catalog aggregate argument is not supplied by its source");
          return RefusedBoundAst(std::move(bound));
        }
        global_aggregate_arguments.push_back(&*argument);
      }
      global_aggregate_expression = &*expression;
    }
    if (filter_composition) {
      if (!filter_relation->relation_source_ids.empty() ||
          !filter_relation->values_row_ids.empty() ||
          filter_relation->output_expression_ids !=
              relation.output_expression_ids ||
          filter_relation->aggregate_grouping_form !=
              NativeAggregateGroupingForm::kNone ||
          filter_relation->aggregate_projection_form !=
              NativeAggregateProjectionForm::kNone ||
          !filter_relation->grouping_key_expression_ids.empty() ||
          !filter_relation->aggregate_expression_ids.empty() ||
          !filter_relation->limit_expression_ids.empty() ||
          !filter_relation->ordering_terms.empty() ||
          filter_relation->predicate_expression_ids.size() != 1) {
        AddBoundAstDiagnostic(
            &bound, "QOW-DIAG-BOUNDAST-RELATION",
            "catalog WHERE AST is outside the bounded composition profile");
        return RefusedBoundAst(std::move(bound));
      }
      const auto predicate = std::ranges::find_if(
          ast.expressions, [&](const auto& candidate) {
            return candidate.expression_id ==
                   filter_relation->predicate_expression_ids.front();
          });
      if (predicate != ast.expressions.end() &&
          predicate->expression_kind == NativeExpressionAstKind::kBinary &&
          predicate->child_expression_ids.size() == 2) {
        const auto left = std::ranges::find_if(
            ast.expressions, [&](const auto& candidate) {
              return candidate.expression_id ==
                     predicate->child_expression_ids[0];
            });
        const auto right = std::ranges::find_if(
            ast.expressions, [&](const auto& candidate) {
              return candidate.expression_id ==
                     predicate->child_expression_ids[1];
            });
        if (left != ast.expressions.end() && right != ast.expressions.end()) {
          filter_predicate = &*predicate;
          filter_identifier = &*left;
          filter_literal = &*right;
        }
      }
      const bool accepted_operator =
          filter_predicate != nullptr &&
          (filter_predicate->operator_name == "=" ||
           filter_predicate->operator_name == "<>" ||
           filter_predicate->operator_name == "!=" ||
           filter_predicate->operator_name == "<" ||
           filter_predicate->operator_name == "<=" ||
           filter_predicate->operator_name == ">" ||
           filter_predicate->operator_name == ">=");
      if (!accepted_operator || filter_identifier == nullptr ||
          filter_literal == nullptr ||
          filter_identifier->expression_kind !=
              NativeExpressionAstKind::kIdentifier ||
          !filter_identifier->child_expression_ids.empty() ||
          !filter_identifier->operator_name.empty() ||
          (filter_literal->expression_kind !=
               NativeExpressionAstKind::kLiteral &&
           filter_literal->expression_kind !=
               NativeExpressionAstKind::kParameter &&
           filter_literal->expression_kind !=
               NativeExpressionAstKind::kVariable) ||
          (filter_literal->expression_kind ==
               NativeExpressionAstKind::kLiteral &&
           filter_literal->literal_kind != NativeLiteralAstKind::kNumeric) ||
          (filter_literal->expression_kind ==
               NativeExpressionAstKind::kParameter &&
           filter_literal->literal_kind.has_value()) ||
          (filter_literal->expression_kind ==
               NativeExpressionAstKind::kVariable &&
           filter_literal->literal_kind.has_value()) ||
          !filter_literal->child_expression_ids.empty() ||
          !filter_literal->operator_name.empty()) {
        AddBoundAstDiagnostic(
            &bound, "QOW-DIAG-BOUNDAST-EXPRESSION",
            "catalog WHERE requires one bound numeric column comparison");
        return RefusedBoundAst(std::move(bound));
      }
      if (filter_literal->expression_kind == NativeExpressionAstKind::kLiteral) {
        std::uint64_t parsed = 0;
        const auto [end, error] = std::from_chars(
            filter_literal->spelling.data(),
            filter_literal->spelling.data() + filter_literal->spelling.size(),
            parsed);
        if (error != std::errc{} ||
            end != filter_literal->spelling.data() +
                       filter_literal->spelling.size()) {
          AddBoundAstDiagnostic(
              &bound, "QOW-DIAG-BOUNDAST-EXPRESSION",
              "catalog WHERE literal must be a canonical unsigned integer");
          return RefusedBoundAst(std::move(bound));
        }
      }
    }
    std::vector<const NativeExpressionAstNode*> sort_identifiers;
    if (sort_composition) {
      std::unordered_set<std::string> sort_names;
      for (const auto& term : sort_relation->ordering_terms) {
        const auto expression = std::ranges::find_if(
            ast.expressions, [&](const auto& candidate) {
              return candidate.expression_id == term.expression_id;
            });
        if (expression == ast.expressions.end() ||
            expression->expression_kind !=
                NativeExpressionAstKind::kIdentifier ||
            expression->spelling.empty() ||
            !expression->child_expression_ids.empty() ||
            !expression->operator_name.empty() ||
            !sort_names.insert(expression->spelling).second) {
          AddBoundAstDiagnostic(
              &bound, "QOW-DIAG-BOUNDAST-EXPRESSION",
              "catalog ORDER BY requires unique bound column identifiers");
          return RefusedBoundAst(std::move(bound));
        }
        sort_identifiers.push_back(&*expression);
      }
      if (!sort_relation->relation_source_ids.empty() ||
          !sort_relation->values_row_ids.empty() ||
          sort_relation->output_expression_ids != relation.output_expression_ids ||
          sort_relation->ordering_terms.empty() ||
          sort_relation->aggregate_grouping_form !=
              NativeAggregateGroupingForm::kNone ||
          sort_relation->aggregate_projection_form !=
              NativeAggregateProjectionForm::kNone ||
          !sort_relation->grouping_key_expression_ids.empty() ||
          !sort_relation->aggregate_expression_ids.empty() ||
          !sort_relation->predicate_expression_ids.empty() ||
          !sort_relation->limit_expression_ids.empty()) {
        AddBoundAstDiagnostic(
            &bound, "QOW-DIAG-BOUNDAST-RELATION",
            "catalog ORDER BY AST is outside the bounded composition profile");
        return RefusedBoundAst(std::move(bound));
      }
    }
    if (project_composition) {
      const bool exact_visible_subset =
          project_relation->output_expression_ids.size() <
              relation.output_expression_ids.size() &&
          std::ranges::all_of(
              project_relation->output_expression_ids,
              [&](const auto expression_id) {
                return std::ranges::find(relation.output_expression_ids,
                                         expression_id) !=
                       relation.output_expression_ids.end();
              });
      if (!project_relation->relation_source_ids.empty() ||
          !project_relation->values_row_ids.empty() ||
          project_relation->output_expression_ids !=
              visible_projection_expression_ids ||
          !exact_visible_subset ||
          project_relation->aggregate_grouping_form !=
              NativeAggregateGroupingForm::kNone ||
          project_relation->aggregate_projection_form !=
              NativeAggregateProjectionForm::kNone ||
          !project_relation->grouping_key_expression_ids.empty() ||
          !project_relation->aggregate_expression_ids.empty() ||
          !project_relation->predicate_expression_ids.empty() ||
          !project_relation->limit_expression_ids.empty() ||
          !project_relation->ordering_terms.empty() ||
          (filter_composition &&
           filter_relation->output_expression_ids !=
               relation.output_expression_ids) ||
          (sort_composition &&
           sort_relation->output_expression_ids !=
               relation.output_expression_ids)) {
        AddBoundAstDiagnostic(
            &bound, "QOW-DIAG-BOUNDAST-RELATION",
            "catalog hidden-column projection is outside the bounded composition profile");
        return RefusedBoundAst(std::move(bound));
      }
    }
    if (limit_composition) {
      if (!limit_relation->relation_source_ids.empty() ||
          !limit_relation->values_row_ids.empty() ||
          limit_relation->output_expression_ids !=
              visible_projection_expression_ids ||
          limit_relation->aggregate_grouping_form !=
              NativeAggregateGroupingForm::kNone ||
          limit_relation->aggregate_projection_form !=
              NativeAggregateProjectionForm::kNone ||
          !limit_relation->grouping_key_expression_ids.empty() ||
          !limit_relation->aggregate_expression_ids.empty() ||
          !limit_relation->predicate_expression_ids.empty() ||
          !limit_relation->ordering_terms.empty() ||
          (limit_relation->limit_expression_ids.size() != 1 &&
           limit_relation->limit_expression_ids.size() != 2)) {
        AddBoundAstDiagnostic(
            &bound, "QOW-DIAG-BOUNDAST-RELATION",
            "catalog LIMIT AST is outside the bounded composition profile");
        return RefusedBoundAst(std::move(bound));
      }
      for (const auto expression_id : limit_relation->limit_expression_ids) {
        const auto expression = std::ranges::find_if(
            ast.expressions, [&](const auto& candidate) {
              return candidate.expression_id == expression_id;
            });
        std::uint64_t parsed = 0;
        if (expression == ast.expressions.end() ||
            expression->expression_kind != NativeExpressionAstKind::kLiteral ||
            expression->literal_kind != NativeLiteralAstKind::kNumeric ||
            !expression->child_expression_ids.empty() ||
            !expression->operator_name.empty()) {
          AddBoundAstDiagnostic(
              &bound, "QOW-DIAG-BOUNDAST-EXPRESSION",
              "catalog LIMIT operands must be unsigned numeric literals");
          return RefusedBoundAst(std::move(bound));
        }
        const auto [end, error] = std::from_chars(
            expression->spelling.data(),
            expression->spelling.data() + expression->spelling.size(), parsed);
        if (error != std::errc{} ||
            end != expression->spelling.data() + expression->spelling.size()) {
          AddBoundAstDiagnostic(
              &bound, "QOW-DIAG-BOUNDAST-EXPRESSION",
              "catalog LIMIT operands must be canonical unsigned integers");
          return RefusedBoundAst(std::move(bound));
        }
      }
    }
    std::unordered_set<std::uint32_t> admitted_ast_expression_ids(
        relation.output_expression_ids.begin(),
        relation.output_expression_ids.end());
    if (filter_composition) {
      admitted_ast_expression_ids.insert(filter_predicate->expression_id);
      admitted_ast_expression_ids.insert(
          filter_predicate->child_expression_ids.begin(),
          filter_predicate->child_expression_ids.end());
    }
    if (sort_composition) {
      for (const auto& term : sort_relation->ordering_terms) {
        admitted_ast_expression_ids.insert(term.expression_id);
      }
    }
    if (limit_composition) {
      admitted_ast_expression_ids.insert(
          limit_relation->limit_expression_ids.begin(),
          limit_relation->limit_expression_ids.end());
    }
    if (aggregate_composition) {
      admitted_ast_expression_ids.insert(
          global_aggregate_expression->expression_id);
      if (global_aggregate_direct_literal != nullptr) {
        admitted_ast_expression_ids.insert(
            global_aggregate_direct_literal->expression_id);
      }
    }
    if (admitted_ast_expression_ids.size() != ast.expressions.size() ||
        std::ranges::any_of(ast.expressions, [&](const auto& expression) {
          return !admitted_ast_expression_ids.contains(
              expression.expression_id);
        })) {
      AddBoundAstDiagnostic(
          &bound, "QOW-DIAG-BOUNDAST-EXPRESSION",
          "catalog source expression cardinality is outside its bounded chain");
      return RefusedBoundAst(std::move(bound));
    }

    const auto& relation_binding = context.catalog_relations.front();
    if (relation_binding.source_id != source.source_id ||
        relation_binding.resolution_state !=
            NativeCatalogRelationResolutionState::kBound ||
        !IsNonNullCanonicalUuid(relation_binding.object_uuid) ||
        !IsCatalogRelationObjectType(relation_binding.resolved_object_type) ||
        !IsNonNullCanonicalUuid(relation_binding.resolved_schema_uuid) ||
        (relation_binding.parent_object_uuid.has_value() &&
         !IsNonNullCanonicalUuid(*relation_binding.parent_object_uuid)) ||
        relation_binding.catalog_generation_id == 0 ||
        relation_binding.security_epoch == 0 ||
        relation_binding.resource_epoch == 0 ||
        relation_binding.columns.empty()) {
      AddBoundAstDiagnostic(
          &bound, "QOW-DIAG-BOUNDAST-RELATION",
          "catalog relation requires caller-supplied bound UUID and epoch evidence");
      return RefusedBoundAst(std::move(bound));
    }
    if (!wildcard_projection &&
        (source_identifiers.size() != relation_binding.columns.size() ||
         !std::ranges::equal(
             source_identifiers, relation_binding.columns,
             [](const auto* expression, const auto& column) {
               return expression->spelling == column.canonical_name_key;
             }))) {
      AddBoundAstDiagnostic(
          &bound, "QOW-DIAG-BOUNDAST-OUTPUT",
          "catalog source dependency projection differs from its resolved column order");
      return RefusedBoundAst(std::move(bound));
    }

    std::unordered_set<std::string> column_uuids;
    std::unordered_set<std::uint32_t> used_descriptor_ids;
    BoundCatalogRelationSourceAstRecord bound_source;
    bound_source.source_id = source.source_id;
    bound_source.source_kind = source.source_kind;
    bound_source.resolution_state =
        NativeCatalogRelationResolutionState::kBound;
    bound_source.qualified_name = source.qualified_name;
    bound_source.alias = source.alias;
    bound_source.alias_is_explicit = source.alias_is_explicit;
    bound_source.qualified_name_range = source.qualified_name_range;
    bound_source.range = source.range;
    bound_source.object_uuid = relation_binding.object_uuid;
    bound_source.resolved_object_type = relation_binding.resolved_object_type;
    bound_source.resolved_schema_uuid = relation_binding.resolved_schema_uuid;
    bound_source.parent_object_uuid = relation_binding.parent_object_uuid;
    bound_source.catalog_generation_id =
        relation_binding.catalog_generation_id;
    bound_source.security_epoch = relation_binding.security_epoch;
    bound_source.resource_epoch = relation_binding.resource_epoch;
    bound_source.columns.reserve(relation_binding.columns.size());
    for (std::size_t ordinal = 0; ordinal < relation_binding.columns.size();
         ++ordinal) {
      const auto& column = relation_binding.columns[ordinal];
      const auto descriptor = descriptor_by_id.find(column.descriptor_id);
      if (column.ordinal != ordinal ||
          !IsNonNullCanonicalUuid(column.column_uuid) ||
          column.canonical_name_key.empty() ||
          !column_uuids.insert(column.column_uuid).second ||
          descriptor == descriptor_by_id.end() ||
          descriptor->second->nullability == BoundNullability::kUnknown ||
          (descriptor->second->timezone_profile_id.has_value() &&
           descriptor->second->timezone_profile_id->empty())) {
        AddBoundAstDiagnostic(
            &bound, "QOW-DIAG-BOUNDAST-DESCRIPTOR",
            "catalog columns require ordered UUID and complete descriptor evidence");
        return RefusedBoundAst(std::move(bound));
      }
      used_descriptor_ids.insert(column.descriptor_id);
      bound_source.columns.push_back(
          {column.ordinal, column.column_uuid, column.descriptor_id,
           column.canonical_name_key});
    }
    const NativeDescriptorBindingInput* aggregate_descriptor = nullptr;
    const NativeExpressionBindingInput* aggregate_expression_binding = nullptr;
    std::vector<std::uint32_t> aggregate_argument_expression_ids;
    const std::string_view aggregate_semantic =
        aggregate_composition
            ? std::string_view(context.relations.front().semantic_variant_id)
            : std::string_view{};
    const bool aggregate_count =
        aggregate_composition &&
        aggregate_semantic.starts_with("aggregate.global-count-");
    const bool aggregate_regr_count =
        aggregate_composition &&
        aggregate_semantic.starts_with("aggregate.global-regr-count-");
    const bool aggregate_approx_count_distinct =
        aggregate_composition &&
        aggregate_semantic.starts_with(
            "aggregate.global-approx-count-distinct-");
    const bool aggregate_string_agg =
        aggregate_composition &&
        aggregate_semantic.starts_with("aggregate.global-string-agg-");
    const bool aggregate_listagg =
        aggregate_composition &&
        aggregate_semantic.starts_with("aggregate.global-listagg-");
    const bool aggregate_array_agg =
        aggregate_composition &&
        aggregate_semantic.starts_with("aggregate.global-array-agg-");
    const bool aggregate_json_agg =
        aggregate_composition &&
        aggregate_semantic.starts_with("aggregate.global-json-agg-");
    const bool aggregate_json_object_agg =
        aggregate_composition &&
        aggregate_semantic.starts_with("aggregate.global-json-object-agg-");
    const bool aggregate_approx_top_k =
        aggregate_composition &&
        aggregate_semantic.starts_with("aggregate.global-approx-top-k-");
    const bool aggregate_mode =
        aggregate_composition &&
        aggregate_semantic.starts_with("aggregate.global-mode-");
    const bool aggregate_percentile_cont =
        aggregate_composition &&
        aggregate_semantic.starts_with("aggregate.global-percentile-cont-");
    const bool aggregate_percentile_disc =
        aggregate_composition &&
        aggregate_semantic.starts_with("aggregate.global-percentile-disc-");
    const bool aggregate_approx_percentile_cont =
        aggregate_composition &&
        aggregate_semantic.starts_with(
            "aggregate.global-approx-percentile-cont-");
    const bool aggregate_approx_percentile_disc =
        aggregate_composition &&
        aggregate_semantic.starts_with(
            "aggregate.global-approx-percentile-disc-");
    const bool aggregate_rank =
        aggregate_composition &&
        aggregate_semantic.starts_with("aggregate.global-rank-hypothetical-");
    const bool aggregate_dense_rank =
        aggregate_composition &&
        aggregate_semantic.starts_with(
            "aggregate.global-dense-rank-hypothetical-");
    const bool aggregate_percent_rank =
        aggregate_composition &&
        aggregate_semantic.starts_with(
            "aggregate.global-percent-rank-hypothetical-");
    const bool aggregate_cume_dist =
        aggregate_composition &&
        aggregate_semantic.starts_with(
            "aggregate.global-cume-dist-hypothetical-");
    const bool aggregate_hypothetical =
        aggregate_rank || aggregate_dense_rank || aggregate_percent_rank ||
        aggregate_cume_dist;
    const std::string aggregate_output_name = [&]() {
      if (aggregate_count) return std::string("row_count");
      if (aggregate_semantic.starts_with("aggregate.global-avg-")) {
        return std::string("average_value");
      }
      if (aggregate_semantic.starts_with("aggregate.global-min-")) {
        return std::string("minimum_value");
      }
      if (aggregate_semantic.starts_with("aggregate.global-max-")) {
        return std::string("maximum_value");
      }
      if (aggregate_semantic.starts_with("aggregate.global-bool-and-")) {
        return std::string("bool_and_value");
      }
      if (aggregate_semantic.starts_with("aggregate.global-bool-or-")) {
        return std::string("bool_or_value");
      }
      if (aggregate_semantic.starts_with("aggregate.global-every-")) {
        return std::string("every_value");
      }
      if (aggregate_semantic.starts_with("aggregate.global-corr-")) {
        return std::string("corr_value");
      }
      if (aggregate_semantic.starts_with("aggregate.global-covar-pop-")) {
        return std::string("covar_pop_value");
      }
      if (aggregate_semantic.starts_with("aggregate.global-covar-samp-")) {
        return std::string("covar_samp_value");
      }
      if (aggregate_regr_count) return std::string("regr_count_value");
      if (aggregate_semantic.starts_with("aggregate.global-regr-avgx-")) {
        return std::string("regr_avgx_value");
      }
      if (aggregate_semantic.starts_with("aggregate.global-regr-avgy-")) {
        return std::string("regr_avgy_value");
      }
      if (aggregate_semantic.starts_with(
              "aggregate.global-regr-intercept-")) {
        return std::string("regr_intercept_value");
      }
      if (aggregate_semantic.starts_with("aggregate.global-regr-r2-")) {
        return std::string("regr_r2_value");
      }
      if (aggregate_semantic.starts_with("aggregate.global-regr-slope-")) {
        return std::string("regr_slope_value");
      }
      if (aggregate_semantic.starts_with("aggregate.global-regr-sxx-")) {
        return std::string("regr_sxx_value");
      }
      if (aggregate_semantic.starts_with("aggregate.global-regr-sxy-")) {
        return std::string("regr_sxy_value");
      }
      if (aggregate_semantic.starts_with("aggregate.global-regr-syy-")) {
        return std::string("regr_syy_value");
      }
      if (aggregate_semantic.starts_with("aggregate.global-stddev-pop-")) {
        return std::string("stddev_pop_value");
      }
      if (aggregate_semantic.starts_with("aggregate.global-variance-pop-")) {
        return std::string("variance_pop_value");
      }
      if (aggregate_semantic.starts_with("aggregate.global-stddev-samp-")) {
        return std::string("stddev_samp_value");
      }
      if (aggregate_semantic.starts_with("aggregate.global-variance-samp-")) {
        return std::string("variance_samp_value");
      }
      if (aggregate_approx_count_distinct) {
        return std::string("approx_count_distinct_value");
      }
      if (aggregate_semantic.starts_with("aggregate.global-approx-median-")) {
        return std::string("approx_median_value");
      }
      if (aggregate_string_agg) return std::string("string_agg_value");
      if (aggregate_listagg) return std::string("listagg_value");
      if (aggregate_array_agg) return std::string("array_agg_value");
      if (aggregate_json_agg) return std::string("json_agg_value");
      if (aggregate_json_object_agg) {
        return std::string("json_object_agg_value");
      }
      if (aggregate_approx_top_k) {
        return std::string("approx_top_k_value");
      }
      if (aggregate_mode) return std::string("mode_value");
      if (aggregate_percentile_cont) {
        return std::string("percentile_cont_value");
      }
      if (aggregate_percentile_disc) {
        return std::string("percentile_disc_value");
      }
      if (aggregate_approx_percentile_cont) {
        return std::string("approx_percentile_cont_value");
      }
      if (aggregate_approx_percentile_disc) {
        return std::string("approx_percentile_disc_value");
      }
      if (aggregate_rank) return std::string("rank_value");
      if (aggregate_dense_rank) return std::string("dense_rank_value");
      if (aggregate_percent_rank) return std::string("percent_rank_value");
      if (aggregate_cume_dist) return std::string("cume_dist_value");
      if (aggregate_semantic.starts_with("aggregate.global-stddev-")) {
        return std::string("stddev_value");
      }
      if (aggregate_semantic.starts_with("aggregate.global-variance-")) {
        return std::string("variance_value");
      }
      return std::string("total_amount");
    }();
    if (aggregate_composition) {
      const auto expected_aggregate_descriptor_id =
          static_cast<std::uint32_t>(relation_binding.columns.size() + 1);
      const auto descriptor =
          descriptor_by_id.find(expected_aggregate_descriptor_id);
      const auto expression = std::ranges::find_if(
          context.expressions, [&](const auto& candidate) {
            return candidate.expression_id ==
                   expected_aggregate_descriptor_id;
          });
      if (descriptor == descriptor_by_id.end() ||
          descriptor->second->nullability !=
              ((aggregate_count || aggregate_regr_count ||
                aggregate_approx_count_distinct || aggregate_hypothetical)
                   ? BoundNullability::kNonNull
                   : BoundNullability::kNullable) ||
          descriptor->second->collation_uuid.has_value() ||
          descriptor->second->timezone_profile_id.has_value() ||
          expression == context.expressions.end() ||
          expression->descriptor_id != expected_aggregate_descriptor_id ||
          !expression->function_uuid.has_value() ||
          !IsNonNullCanonicalUuid(*expression->function_uuid) ||
          expression->bound_name_uuid.has_value()) {
        AddBoundAstDiagnostic(
            &bound, "QOW-DIAG-BOUNDAST-EXPRESSION",
            "catalog global aggregate binding is incomplete");
        return RefusedBoundAst(std::move(bound));
      }
      aggregate_descriptor = descriptor->second;
      aggregate_expression_binding = &*expression;
      for (const auto* global_aggregate_argument :
           global_aggregate_arguments) {
        const auto column = std::ranges::find_if(
            relation_binding.columns, [&](const auto& candidate) {
              return candidate.canonical_name_key ==
                     global_aggregate_argument->spelling;
            });
        if (column == relation_binding.columns.end()) {
          AddBoundAstDiagnostic(
              &bound, "QOW-DIAG-BOUNDAST-EXPRESSION",
              "catalog aggregate argument column is unresolved");
          return RefusedBoundAst(std::move(bound));
        }
        aggregate_argument_expression_ids.push_back(
            static_cast<std::uint32_t>(column->ordinal + 1));
      }
      if (global_aggregate_direct_literal != nullptr) {
        const auto expected_direct_descriptor_id =
            expected_aggregate_descriptor_id + 1;
        const auto direct_descriptor =
            descriptor_by_id.find(expected_direct_descriptor_id);
        const auto direct_expression = std::ranges::find_if(
            context.expressions, [&](const auto& candidate) {
              return candidate.expression_id ==
                     global_aggregate_direct_literal->expression_id;
            });
        if ((!aggregate_string_agg && !aggregate_listagg &&
             !aggregate_percentile_cont && !aggregate_percentile_disc &&
             !aggregate_approx_percentile_cont &&
             !aggregate_approx_percentile_disc &&
             !aggregate_hypothetical && !aggregate_approx_top_k) ||
            direct_descriptor == descriptor_by_id.end() ||
            direct_descriptor->second->nullability !=
                BoundNullability::kNonNull ||
            direct_descriptor->second->collation_uuid.has_value() ||
            direct_descriptor->second->timezone_profile_id.has_value() ||
            direct_descriptor->second->width_precision_scale.width.has_value() ||
            direct_descriptor->second->width_precision_scale.precision.has_value() ||
            direct_descriptor->second->width_precision_scale.scale.has_value() ||
            direct_expression == context.expressions.end() ||
            direct_expression->descriptor_id !=
                expected_direct_descriptor_id ||
            direct_expression->function_uuid.has_value() ||
            direct_expression->bound_name_uuid.has_value()) {
          AddBoundAstDiagnostic(
              &bound, "QOW-DIAG-BOUNDAST-EXPRESSION",
              "catalog STRING_AGG separator binding is incomplete");
          return RefusedBoundAst(std::move(bound));
        }
        if (aggregate_listagg) {
          aggregate_argument_expression_ids.insert(
              aggregate_argument_expression_ids.begin() + 1,
              global_aggregate_direct_literal->expression_id);
        } else if (aggregate_percentile_cont || aggregate_percentile_disc ||
                   aggregate_approx_percentile_cont ||
                   aggregate_approx_percentile_disc ||
                   aggregate_hypothetical || aggregate_approx_top_k) {
          aggregate_argument_expression_ids.insert(
              aggregate_argument_expression_ids.begin(),
              global_aggregate_direct_literal->expression_id);
        } else {
          aggregate_argument_expression_ids.push_back(
              global_aggregate_direct_literal->expression_id);
        }
        used_descriptor_ids.insert(expected_direct_descriptor_id);
      }
      used_descriptor_ids.insert(expected_aggregate_descriptor_id);
    }
    const auto aggregate_direct_descriptor_count =
        global_aggregate_direct_literal != nullptr ? 1U : 0U;
    const NativeDescriptorBindingInput* numeric_descriptor = nullptr;
    if (limit_composition) {
      const auto expected_numeric_descriptor_id =
          static_cast<std::uint32_t>(
              relation_binding.columns.size() +
              (aggregate_composition
                   ? 2 + aggregate_direct_descriptor_count
                   : 1));
      const auto descriptor =
          descriptor_by_id.find(expected_numeric_descriptor_id);
      if (descriptor == descriptor_by_id.end() ||
          descriptor->second->nullability != BoundNullability::kNonNull ||
          descriptor->second->collation_uuid.has_value() ||
          descriptor->second->timezone_profile_id.has_value() ||
          descriptor->second->width_precision_scale.width.has_value() ||
          descriptor->second->width_precision_scale.precision.has_value() ||
          descriptor->second->width_precision_scale.scale.has_value()) {
        AddBoundAstDiagnostic(
            &bound, "QOW-DIAG-BOUNDAST-DESCRIPTOR",
            "catalog composition requires one non-null numeric descriptor profile");
        return RefusedBoundAst(std::move(bound));
      }
      numeric_descriptor = descriptor->second;
      used_descriptor_ids.insert(expected_numeric_descriptor_id);
    }
    const NativeDescriptorBindingInput* boolean_descriptor = nullptr;
    const NativeExpressionBindingInput* negotiated_literal_binding = nullptr;
    const NativeCatalogColumnBindingInput* filter_column = nullptr;
    if (filter_composition) {
      const auto expected_boolean_descriptor_id =
          static_cast<std::uint32_t>(
              relation_binding.columns.size() +
              (aggregate_composition ? 1 : 0) +
              aggregate_direct_descriptor_count +
              (limit_composition ? 2 : 1));
      const auto descriptor =
          descriptor_by_id.find(expected_boolean_descriptor_id);
      const auto column = std::ranges::find_if(
          relation_binding.columns, [&](const auto& candidate) {
            return candidate.canonical_name_key ==
                   filter_identifier->spelling;
          });
      if (descriptor == descriptor_by_id.end() ||
          descriptor->second->nullability != BoundNullability::kNullable ||
          descriptor->second->collation_uuid.has_value() ||
          descriptor->second->timezone_profile_id.has_value() ||
          descriptor->second->width_precision_scale.width.has_value() ||
          descriptor->second->width_precision_scale.precision.has_value() ||
          descriptor->second->width_precision_scale.scale.has_value() ||
          column == relation_binding.columns.end()) {
        AddBoundAstDiagnostic(
            &bound, "QOW-DIAG-BOUNDAST-DESCRIPTOR",
            "catalog WHERE requires one persisted numeric column and nullable "
            "boolean descriptor profile");
        return RefusedBoundAst(std::move(bound));
      }
      boolean_descriptor = descriptor->second;
      filter_column = &*column;
      used_descriptor_ids.insert(expected_boolean_descriptor_id);
      const bool parameter_leaf =
          filter_literal->expression_kind == NativeExpressionAstKind::kParameter;
      const bool variable_leaf =
          filter_literal->expression_kind == NativeExpressionAstKind::kVariable;
      const auto literal_binding = std::ranges::find_if(
          context.expressions, [&](const auto& candidate) {
            return candidate.expression_id == filter_literal->expression_id &&
                   (parameter_leaf
                        ? candidate.structural_parameter_occurrence_id ==
                              filter_literal->structural_parameter_occurrence_id
                        : variable_leaf
                        ? candidate.structural_variable_occurrence_id ==
                              filter_literal->structural_variable_occurrence_id
                        : candidate.structural_literal_occurrence_id ==
                              filter_literal->structural_literal_occurrence_id);
          });
      if (literal_binding == context.expressions.end() ||
          (parameter_leaf
               ? filter_literal->structural_parameter_occurrence_id == 0
               : variable_leaf
               ? filter_literal->structural_variable_occurrence_id == 0
               : filter_literal->structural_literal_occurrence_id == 0) ||
          !descriptor_by_id.contains(literal_binding->descriptor_id)) {
        AddBoundAstDiagnostic(
            &bound, "QOW-DIAG-BOUNDAST-DESCRIPTOR",
            "catalog WHERE value lacks its exact negotiated occurrence descriptor");
        return RefusedBoundAst(std::move(bound));
      }
      negotiated_literal_binding = &*literal_binding;
      used_descriptor_ids.insert(literal_binding->descriptor_id);
    }
    if (used_descriptor_ids.size() != descriptor_by_id.size()) {
      AddBoundAstDiagnostic(
          &bound, "QOW-DIAG-BOUNDAST-DESCRIPTOR",
          "catalog relation binding contains an unused descriptor handle");
      return RefusedBoundAst(std::move(bound));
    }

    std::vector<std::uint32_t> expanded_expression_ids;
    std::vector<std::uint32_t> visible_expression_ids;
    std::vector<std::uint32_t> expanded_output_ids;
    if (expanded_projection) {
      const auto column_count = relation_binding.columns.size();
      const auto visible_column_count =
          aggregate_composition
              ? 1
              : (project_composition ? projection_identifiers.size()
                                     : column_count);
      const auto expected_output_count =
          column_count +
          (filter_composition ? column_count : 0) +
          (sort_composition ? column_count : 0) +
          (project_composition ? visible_column_count : 0) +
          (aggregate_composition ? 1 : 0) +
          (limit_composition ? visible_column_count : 0);
      if (context.expressions.size() !=
              column_count + (aggregate_composition ? 1 : 0) +
                  aggregate_direct_descriptor_count +
                  (filter_composition ? 1 : 0) ||
          visible_column_count == 0 ||
          context.outputs.size() != expected_output_count) {
        AddBoundAstDiagnostic(
            &bound, "QOW-DIAG-BOUNDAST-OUTPUT",
            "catalog projection must cover its source and visible widths exactly");
        return RefusedBoundAst(std::move(bound));
      }
      bound.expressions.reserve(
          column_count + (aggregate_composition ? 1 : 0) +
          aggregate_direct_descriptor_count +
          (filter_composition ? 3 : 0) +
          (limit_composition ? limit_relation->limit_expression_ids.size()
                             : 0));
      bound.outputs.reserve(expected_output_count);
      expanded_expression_ids.reserve(column_count);
      visible_expression_ids.reserve(visible_column_count);
      expanded_output_ids.reserve(column_count);
      for (std::size_t ordinal = 0; ordinal < column_count; ++ordinal) {
        const auto expected_id = static_cast<std::uint32_t>(ordinal + 1);
        const auto& column = relation_binding.columns[ordinal];
        const auto& expression = context.expressions[ordinal];
        const auto& output = context.outputs[ordinal];
        if (expression.expression_id != expected_id ||
            expression.descriptor_id != column.descriptor_id ||
            expression.function_uuid.has_value() ||
            !expression.bound_name_uuid.has_value() ||
            *expression.bound_name_uuid != column.column_uuid) {
          AddBoundAstDiagnostic(
              &bound, "QOW-DIAG-BOUNDAST-EXPRESSION",
              "catalog projection expression does not match its persisted column");
          return RefusedBoundAst(std::move(bound));
        }
        if (output.output_id != expected_id ||
            output.relation_id != relation.relation_id ||
            output.expression_id != expression.expression_id ||
            output.output_name_utf8 != column.canonical_name_key ||
            output.descriptor_id != column.descriptor_id || !output.visible ||
            output.ordinal != ordinal) {
          AddBoundAstDiagnostic(
              &bound, "QOW-DIAG-BOUNDAST-OUTPUT",
              "catalog projection output does not match its persisted column");
          return RefusedBoundAst(std::move(bound));
        }

        BoundExpressionAstRecord bound_expression;
        bound_expression.expression_id = expression.expression_id;
        bound_expression.expression_kind = NativeExpressionAstKind::kIdentifier;
        bound_expression.result_descriptor_id = expression.descriptor_id;
        bound_expression.bound_name_uuid = expression.bound_name_uuid;
        bound.expressions.push_back(std::move(bound_expression));

        BoundOutputAstRecord bound_output;
        bound_output.output_id = output.output_id;
        bound_output.relation_id = output.relation_id;
        bound_output.expression_id = output.expression_id;
        bound_output.output_name_utf8 = output.output_name_utf8;
        bound_output.descriptor_id = output.descriptor_id;
        bound_output.visible = output.visible;
        bound_output.ordinal = output.ordinal;
        bound.outputs.push_back(std::move(bound_output));
        expanded_expression_ids.push_back(expression.expression_id);
        if (!filter_composition && !project_composition &&
            !aggregate_composition && !limit_composition) {
          expanded_output_ids.push_back(output.output_id);
        }
      }
      if (aggregate_composition) {
        if (global_aggregate_direct_literal != nullptr) {
          const auto direct_binding = std::ranges::find_if(
              context.expressions, [&](const auto& candidate) {
                return candidate.expression_id ==
                       global_aggregate_direct_literal->expression_id;
              });
          BoundExpressionAstRecord literal;
          literal.expression_id = direct_binding->expression_id;
          literal.expression_kind = NativeExpressionAstKind::kLiteral;
          literal.literal_kind = global_aggregate_direct_literal->literal_kind;
          literal.result_descriptor_id = direct_binding->descriptor_id;
          literal.literal_or_parameter_ref =
              global_aggregate_direct_literal->spelling;
          bound.expressions.push_back(std::move(literal));
        }
        BoundExpressionAstRecord bound_expression;
        bound_expression.expression_id =
            aggregate_expression_binding->expression_id;
        bound_expression.expression_kind =
            NativeExpressionAstKind::kFunctionCall;
        bound_expression.result_descriptor_id =
            aggregate_expression_binding->descriptor_id;
        bound_expression.bound_function_uuid =
            aggregate_expression_binding->function_uuid;
        bound_expression.child_expression_ids =
            aggregate_argument_expression_ids;
        bound.expressions.push_back(std::move(bound_expression));
        visible_expression_ids = {
            aggregate_expression_binding->expression_id};
      } else {
        visible_expression_ids = project_composition
                                     ? project_relation->output_expression_ids
                                     : expanded_expression_ids;
      }
      std::size_t output_index = column_count;
      for (std::size_t relation_ordinal = 1;
           relation_ordinal < ast.relations.size(); ++relation_ordinal) {
        const auto& downstream = ast.relations[relation_ordinal];
        if (aggregate_composition &&
            (downstream.relation_id == aggregate_relation->relation_id ||
             (limit_composition &&
              downstream.relation_id == limit_relation->relation_id))) {
          if (output_index >= context.outputs.size()) {
            AddBoundAstDiagnostic(
                &bound, "QOW-DIAG-BOUNDAST-OUTPUT",
                "catalog aggregate output evidence is missing");
            return RefusedBoundAst(std::move(bound));
          }
          const auto& output = context.outputs[output_index];
          const auto expected_output_id =
              static_cast<std::uint32_t>(output_index + 1);
          if (output.output_id != expected_output_id ||
              output.relation_id != downstream.relation_id ||
              output.expression_id !=
                  aggregate_expression_binding->expression_id ||
              output.output_name_utf8 != aggregate_output_name ||
              output.descriptor_id != aggregate_descriptor->descriptor_id ||
              !output.visible || output.ordinal != 0) {
            AddBoundAstDiagnostic(
                &bound, "QOW-DIAG-BOUNDAST-OUTPUT",
                "catalog aggregate output does not match its function binding");
            return RefusedBoundAst(std::move(bound));
          }
          BoundOutputAstRecord bound_output;
          bound_output.output_id = output.output_id;
          bound_output.relation_id = output.relation_id;
          bound_output.expression_id = output.expression_id;
          bound_output.output_name_utf8 = output.output_name_utf8;
          bound_output.descriptor_id = output.descriptor_id;
          bound_output.visible = output.visible;
          bound_output.ordinal = output.ordinal;
          bound.outputs.push_back(std::move(bound_output));
          if (downstream.relation_id == ast.root_relation_id) {
            expanded_output_ids.push_back(output.output_id);
          }
          ++output_index;
          continue;
        }
        std::vector<std::size_t> source_ordinals;
        if (wildcard_projection) {
          source_ordinals.reserve(column_count);
          for (std::size_t ordinal = 0; ordinal < column_count; ++ordinal) {
            source_ordinals.push_back(ordinal);
          }
        } else {
          source_ordinals.reserve(downstream.output_expression_ids.size());
          for (const auto expression_id : downstream.output_expression_ids) {
            const auto source_expression = std::ranges::find(
                relation.output_expression_ids, expression_id);
            if (source_expression == relation.output_expression_ids.end()) {
              AddBoundAstDiagnostic(
                  &bound, "QOW-DIAG-BOUNDAST-OUTPUT",
                  "catalog operator output has no source dependency");
              return RefusedBoundAst(std::move(bound));
            }
            source_ordinals.push_back(static_cast<std::size_t>(std::distance(
                relation.output_expression_ids.begin(), source_expression)));
          }
        }
        const auto expected_downstream_width =
            downstream.relation_kind == NativeRelationAstKind::kFilter ||
                    downstream.relation_kind == NativeRelationAstKind::kSort
                ? column_count
                : visible_column_count;
        if (source_ordinals.size() != expected_downstream_width) {
          AddBoundAstDiagnostic(
              &bound, "QOW-DIAG-BOUNDAST-OUTPUT",
              "catalog operator output width differs from its bounded profile");
          return RefusedBoundAst(std::move(bound));
        }
        for (std::size_t ordinal = 0; ordinal < source_ordinals.size();
             ++ordinal, ++output_index) {
          const auto source_ordinal = source_ordinals[ordinal];
          const auto& column = relation_binding.columns[source_ordinal];
          const auto& expression = context.expressions[source_ordinal];
          const auto& output = context.outputs[output_index];
          const auto expected_output_id = static_cast<std::uint32_t>(
              output_index + 1);
          if (output.output_id != expected_output_id ||
              output.relation_id != downstream.relation_id ||
              output.expression_id != expression.expression_id ||
              output.output_name_utf8 != column.canonical_name_key ||
              output.descriptor_id != column.descriptor_id || !output.visible ||
              output.ordinal != ordinal) {
            AddBoundAstDiagnostic(
                &bound, "QOW-DIAG-BOUNDAST-OUTPUT",
                "catalog operator output must preserve every source projection");
            return RefusedBoundAst(std::move(bound));
          }
          BoundOutputAstRecord bound_output;
          bound_output.output_id = output.output_id;
          bound_output.relation_id = output.relation_id;
          bound_output.expression_id = output.expression_id;
          bound_output.output_name_utf8 = output.output_name_utf8;
          bound_output.descriptor_id = output.descriptor_id;
          bound_output.visible = output.visible;
          bound_output.ordinal = output.ordinal;
          bound.outputs.push_back(std::move(bound_output));
          if (downstream.relation_id == ast.root_relation_id) {
            expanded_output_ids.push_back(output.output_id);
          }
        }
      }
      if (output_index != context.outputs.size()) {
        AddBoundAstDiagnostic(
            &bound, "QOW-DIAG-BOUNDAST-OUTPUT",
            "catalog operator outputs contain trailing projection evidence");
        return RefusedBoundAst(std::move(bound));
      }
    }

    BoundRelationAstRecord bound_relation;
    bound_relation.relation_id = relation.relation_id;
    bound_relation.relation_kind = relation.relation_kind;
    bound_relation.semantic_variant_id = "catalog.relation-source.v1";
    bound_relation.bound_object_uuid = relation_binding.object_uuid;
    bound_relation.output_expression_ids = expanded_expression_ids;
    bound_relation.bound_expression_ids = expanded_expression_ids;
    bound.relations.push_back(std::move(bound_relation));
    if (filter_composition) {
      BoundExpressionAstRecord literal;
      literal.expression_id =
          filter_literal->expression_id;
      literal.expression_kind = filter_literal->expression_kind;
      literal.literal_kind = filter_literal->literal_kind;
      literal.result_descriptor_id = negotiated_literal_binding->descriptor_id;
      if (filter_literal->expression_kind == NativeExpressionAstKind::kLiteral) {
        literal.literal_or_parameter_ref = filter_literal->spelling;
      }
      literal.structural_literal_occurrence_id =
          filter_literal->structural_literal_occurrence_id;
      literal.structural_parameter_occurrence_id =
          filter_literal->structural_parameter_occurrence_id;
      literal.structural_variable_occurrence_id =
          filter_literal->structural_variable_occurrence_id;
      const auto literal_id = literal.expression_id;
      bound.expressions.push_back(std::move(literal));

      BoundExpressionAstRecord predicate;
      predicate.expression_id =
          filter_predicate->expression_id;
      predicate.expression_kind = NativeExpressionAstKind::kBinary;
      predicate.result_descriptor_id = boolean_descriptor->descriptor_id;
      predicate.child_expression_ids = {
          static_cast<std::uint32_t>(filter_column->ordinal + 1), literal_id};
      predicate.canonical_operator_name = filter_predicate->operator_name;
      const auto predicate_id = predicate.expression_id;
      bound.expressions.push_back(std::move(predicate));

      BoundRelationAstRecord bound_filter;
      bound_filter.relation_id = filter_relation->relation_id;
      bound_filter.relation_kind = NativeRelationAstKind::kFilter;
      bound_filter.input_relation_ids = {relation.relation_id};
      bound_filter.output_expression_ids = expanded_expression_ids;
      bound_filter.predicate_expression_ids = {predicate_id};
      bound_filter.bound_expression_ids = {predicate_id};
      bound_filter.semantic_variant_id =
          "filter.catalog-column-numeric-comparison.v1";
      bound.relations.push_back(std::move(bound_filter));
    }
    if (aggregate_composition) {
      BoundRelationAstRecord bound_aggregate;
      bound_aggregate.relation_id = aggregate_relation->relation_id;
      bound_aggregate.relation_kind = NativeRelationAstKind::kAggregate;
      bound_aggregate.aggregate_grouping_form =
          NativeAggregateGroupingForm::kNone;
      bound_aggregate.aggregate_projection_form =
          NativeAggregateProjectionForm::kGlobalUnary;
      bound_aggregate.input_relation_ids = {
          filter_composition ? filter_relation->relation_id
                             : relation.relation_id};
      bound_aggregate.output_expression_ids = visible_expression_ids;
      bound_aggregate.aggregate_expression_ids = visible_expression_ids;
      bound_aggregate.bound_expression_ids = visible_expression_ids;
      bound_aggregate.semantic_variant_id =
          context.relations.front().semantic_variant_id;
      bound.relations.push_back(std::move(bound_aggregate));
    }
    if (sort_composition) {
      BoundRelationAstRecord bound_sort;
      bound_sort.relation_id = sort_relation->relation_id;
      bound_sort.relation_kind = NativeRelationAstKind::kSort;
      bound_sort.input_relation_ids = {
          filter_composition ? filter_relation->relation_id
                             : relation.relation_id};
      bound_sort.output_expression_ids = expanded_expression_ids;
      for (std::size_t ordinal = 0; ordinal < sort_identifiers.size();
           ++ordinal) {
        const auto column = std::ranges::find_if(
            relation_binding.columns, [&](const auto& candidate) {
              return candidate.canonical_name_key ==
                     sort_identifiers[ordinal]->spelling;
            });
        if (column == relation_binding.columns.end()) {
          AddBoundAstDiagnostic(
              &bound, "QOW-DIAG-BOUNDAST-EXPRESSION",
              "catalog ORDER BY column is not present in the bound source width");
          return RefusedBoundAst(std::move(bound));
        }
        const auto expression_id =
            static_cast<std::uint32_t>(column->ordinal + 1);
        bound_sort.bound_expression_ids.push_back(expression_id);
        bound_sort.ordering_terms.push_back(
            {expression_id, sort_relation->ordering_terms[ordinal].direction,
             sort_relation->ordering_terms[ordinal].null_placement});
      }
      bound_sort.semantic_variant_id = "sort.required-order.v1";
      bound.relations.push_back(std::move(bound_sort));
    }
    if (project_composition) {
      BoundRelationAstRecord bound_project;
      bound_project.relation_id = project_relation->relation_id;
      bound_project.relation_kind = NativeRelationAstKind::kProject;
      bound_project.input_relation_ids = {
          sort_composition
              ? sort_relation->relation_id
              : (filter_composition ? filter_relation->relation_id
                                    : relation.relation_id)};
      bound_project.output_expression_ids = visible_expression_ids;
      bound_project.bound_expression_ids = visible_expression_ids;
      bound_project.semantic_variant_id =
          "project.catalog-visible-columns.v1";
      bound.relations.push_back(std::move(bound_project));
    }
    if (limit_composition) {
      std::vector<std::uint32_t> bound_limit_expression_ids;
      bound_limit_expression_ids.reserve(
          limit_relation->limit_expression_ids.size());
      for (const auto ast_expression_id :
           limit_relation->limit_expression_ids) {
        const auto ast_expression = std::ranges::find_if(
            ast.expressions, [&](const auto& candidate) {
              return candidate.expression_id == ast_expression_id;
            });
        BoundExpressionAstRecord bound_expression;
        bound_expression.expression_id =
            static_cast<std::uint32_t>(bound.expressions.size() + 1);
        bound_expression.expression_kind = NativeExpressionAstKind::kLiteral;
        bound_expression.literal_kind = NativeLiteralAstKind::kNumeric;
        bound_expression.result_descriptor_id =
            numeric_descriptor->descriptor_id;
        bound_expression.literal_or_parameter_ref = ast_expression->spelling;
        bound_limit_expression_ids.push_back(bound_expression.expression_id);
        bound.expressions.push_back(std::move(bound_expression));
      }

      BoundRelationAstRecord bound_limit;
      bound_limit.relation_id = limit_relation->relation_id;
      bound_limit.relation_kind = NativeRelationAstKind::kLimit;
      bound_limit.input_relation_ids = {
          aggregate_composition
              ? aggregate_relation->relation_id
              : (project_composition
              ? project_relation->relation_id
              : (sort_composition
                     ? sort_relation->relation_id
                     : (filter_composition ? filter_relation->relation_id
                                           : relation.relation_id)))};
      bound_limit.output_expression_ids = visible_expression_ids;
      bound_limit.limit_expression_ids = bound_limit_expression_ids;
      bound_limit.bound_expression_ids =
          std::move(bound_limit_expression_ids);
      bound_limit.semantic_variant_id =
          limit_relation->limit_expression_ids.size() == 1
              ? "limit.bound-count.v1"
              : "limit.bound-count-offset.v1";
      bound.relations.push_back(std::move(bound_limit));
    }
    bound.catalog_relation_sources.push_back(std::move(bound_source));

    bound.descriptors.reserve(context.descriptors.size());
    for (const auto& descriptor : context.descriptors) {
      BoundDescriptorAstRecord record;
      record.descriptor_id = descriptor.descriptor_id;
      record.descriptor_uuid = descriptor.descriptor_uuid;
      record.type_uuid = descriptor.type_uuid;
      record.nullability = descriptor.nullability;
      record.collation_uuid = descriptor.collation_uuid;
      record.timezone_profile_id = descriptor.timezone_profile_id;
      record.width_precision_scale = descriptor.width_precision_scale;
      bound.descriptors.push_back(std::move(record));
    }
    std::ranges::sort(bound.descriptors,
                      [](const auto& left, const auto& right) {
                        return left.descriptor_id < right.descriptor_id;
                      });

    BoundScopeAstRecord scope;
    scope.scope_id = 1;
    scope.parent_scope_id = std::nullopt;
    scope.visible_relation_ids = {ast.root_relation_id};
    scope.visible_projection_ids = std::move(expanded_output_ids);
    scope.catalog_epoch_uuid = context.catalog_epoch_uuid;
    bound.scopes.push_back(std::move(scope));

    bound.bound_ast_uuid = context.bound_ast_uuid;
    bound.security_context_uuid = context.security_context_uuid;
    bound.root_relation_id = ast.root_relation_id;
    bound.root_scope_id = 1;
    bound.bound = true;
    return bound;
  }
  if (!context.catalog_relations.empty()) {
    AddBoundAstDiagnostic(
        &bound, "QOW-DIAG-BOUNDAST-RELATION",
        "catalog relation evidence cannot bind a source-free relation graph");
    return RefusedBoundAst(std::move(bound));
  }

  std::unordered_map<std::uint32_t, const NativeExpressionBindingInput*>
      expression_binding_by_id;
  for (const auto& expression_binding : context.expressions) {
    if (expression_binding.expression_id == 0 ||
        expression_binding.descriptor_id == 0 ||
        descriptor_by_id.find(expression_binding.descriptor_id) ==
            descriptor_by_id.end() ||
        !expression_binding_by_id
             .emplace(expression_binding.expression_id, &expression_binding)
             .second) {
      AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-EXPRESSION",
                            "expression binding contains a missing or duplicate handle");
      return RefusedBoundAst(std::move(bound));
    }
  }

  std::unordered_map<std::uint32_t, const NativeExpressionAstNode*> ast_expression_by_id;
  for (const auto& expression : ast.expressions) {
    if (expression.expression_id == 0 ||
        !ast_expression_by_id.emplace(expression.expression_id, &expression).second) {
      AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-EXPRESSION",
                            "typed AST expression handles are invalid");
      return RefusedBoundAst(std::move(bound));
    }
  }
  if (expression_binding_by_id.size() != ast_expression_by_id.size()) {
    AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-EXPRESSION",
                          "every typed AST expression requires exactly one binding");
    return RefusedBoundAst(std::move(bound));
  }

  std::unordered_map<std::uint32_t, std::uint8_t> expression_visit_state;
  std::function<bool(std::uint32_t)> visit_expression =
      [&](const std::uint32_t expression_id) {
        const auto state = expression_visit_state[expression_id];
        if (state == 1) return false;
        if (state == 2) return true;
        const auto expression_iterator = ast_expression_by_id.find(expression_id);
        if (expression_iterator == ast_expression_by_id.end()) return false;
        expression_visit_state[expression_id] = 1;
        for (const auto child_id : expression_iterator->second->child_expression_ids) {
          if (child_id == 0 || !visit_expression(child_id)) return false;
        }
        expression_visit_state[expression_id] = 2;
        return true;
      };
  for (const auto& [expression_id, expression] : ast_expression_by_id) {
    (void)expression;
    if (!visit_expression(expression_id)) {
      AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-EXPRESSION",
                            "expression graph contains a cycle or dangling child");
      return RefusedBoundAst(std::move(bound));
    }
  }

  std::unordered_set<std::uint32_t> used_descriptor_ids;
  bound.expressions.reserve(ast.expressions.size());
  for (const auto& expression : ast.expressions) {
    const auto binding_iterator =
        expression_binding_by_id.find(expression.expression_id);
    if (binding_iterator == expression_binding_by_id.end()) {
      AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-EXPRESSION",
                            "typed AST expression is missing its descriptor binding");
      return RefusedBoundAst(std::move(bound));
    }
    const auto& expression_binding = *binding_iterator->second;
    const bool function_call =
        expression.expression_kind == NativeExpressionAstKind::kFunctionCall;
    const bool identifier =
        expression.expression_kind == NativeExpressionAstKind::kIdentifier;
    if (function_call != expression_binding.function_uuid.has_value() ||
        (expression_binding.function_uuid.has_value() &&
         !LooksLikeCanonicalUuid(*expression_binding.function_uuid))) {
      AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-EXPRESSION",
                            "function UUID state does not match the expression kind");
      return RefusedBoundAst(std::move(bound));
    }
    if (identifier != expression_binding.bound_name_uuid.has_value() ||
        (expression_binding.bound_name_uuid.has_value() &&
         !LooksLikeCanonicalUuid(*expression_binding.bound_name_uuid))) {
      AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-EXPRESSION",
                            "identifier UUID state does not match the expression kind");
      return RefusedBoundAst(std::move(bound));
    }
    const bool literal =
        expression.expression_kind == NativeExpressionAstKind::kLiteral;
    if (literal != expression.literal_kind.has_value()) {
      AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-EXPRESSION",
                            "literal kind state does not match the expression kind");
      return RefusedBoundAst(std::move(bound));
    }
    const bool operator_expression =
        expression.expression_kind == NativeExpressionAstKind::kUnary ||
        expression.expression_kind == NativeExpressionAstKind::kBinary;
    if ((operator_expression && expression.operator_name.empty()) ||
        (!operator_expression && !function_call &&
         !expression.operator_name.empty())) {
      AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-EXPRESSION",
                            "operator identity state does not match the expression kind");
      return RefusedBoundAst(std::move(bound));
    }
    for (const auto child_id : expression.child_expression_ids) {
      if (child_id == 0 || ast_expression_by_id.find(child_id) == ast_expression_by_id.end()) {
        AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-EXPRESSION",
                              "expression contains a dangling child handle");
        return RefusedBoundAst(std::move(bound));
      }
    }

    BoundExpressionAstRecord record;
    record.expression_id = expression.expression_id;
    record.expression_kind = expression.expression_kind;
    record.literal_kind = expression.literal_kind;
    record.child_expression_ids = expression.child_expression_ids;
    record.result_descriptor_id = expression_binding.descriptor_id;
    record.bound_function_uuid = expression_binding.function_uuid;
    record.bound_name_uuid = expression_binding.bound_name_uuid;
    if (expression.structural_literal_occurrence_id !=
        expression_binding.structural_literal_occurrence_id) {
      AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-EXPRESSION",
                            "literal structural occurrence identity changed during binding");
      return RefusedBoundAst(std::move(bound));
    }
    record.structural_literal_occurrence_id =
        expression.structural_literal_occurrence_id;
    if (expression.structural_parameter_occurrence_id !=
        expression_binding.structural_parameter_occurrence_id) {
      AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-EXPRESSION",
                            "parameter structural occurrence identity changed during binding");
      return RefusedBoundAst(std::move(bound));
    }
    record.structural_parameter_occurrence_id =
        expression.structural_parameter_occurrence_id;
    if (expression.structural_variable_occurrence_id !=
        expression_binding.structural_variable_occurrence_id) {
      AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-EXPRESSION",
                            "variable structural occurrence identity changed during binding");
      return RefusedBoundAst(std::move(bound));
    }
    record.structural_variable_occurrence_id =
        expression.structural_variable_occurrence_id;
    if (operator_expression) {
      record.canonical_operator_name = expression.operator_name;
    }
    if (expression.expression_kind == NativeExpressionAstKind::kLiteral) {
      record.literal_or_parameter_ref = expression.spelling;
    }
    used_descriptor_ids.insert(record.result_descriptor_id);
    bound.expressions.push_back(std::move(record));
  }

  if (ast.values_rows.empty() || ast.relations.empty() ||
      ast.root_relation_id == 0) {
    AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                          "typed relational AST requires a reachable VALUES-backed root");
    return RefusedBoundAst(std::move(bound));
  }
  std::unordered_set<std::uint32_t> values_row_ids;
  const auto& first_row = ast.values_rows.front();
  const auto values_arity = first_row.expression_ids.size();
  if (values_arity == 0) {
    AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                          "VALUES rows require at least one typed expression handle");
    return RefusedBoundAst(std::move(bound));
  }
  bound.values_rows.reserve(ast.values_rows.size());
  for (const auto& row : ast.values_rows) {
    if (row.row_id == 0 || !values_row_ids.insert(row.row_id).second ||
        row.expression_ids.size() != values_arity) {
      AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                            "VALUES row handles and arity must be exact");
      return RefusedBoundAst(std::move(bound));
    }
    for (const auto expression_id : row.expression_ids) {
      if (expression_id == 0 ||
          ast_expression_by_id.find(expression_id) == ast_expression_by_id.end()) {
        AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                              "VALUES row contains a dangling expression handle");
        return RefusedBoundAst(std::move(bound));
      }
    }
    bound.values_rows.push_back({row.row_id, row.expression_ids});
  }

  std::unordered_map<std::uint32_t, const NativeRelationAstNode*>
      ast_relation_by_id;
  for (const auto& relation : ast.relations) {
    if (relation.relation_id == 0 ||
        !ast_relation_by_id.emplace(relation.relation_id, &relation).second) {
      AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                            "typed relation handles must be nonzero and unique");
      return RefusedBoundAst(std::move(bound));
    }
  }
  const auto root_relation = ast_relation_by_id.find(ast.root_relation_id);
  if (root_relation == ast_relation_by_id.end() ||
      ast.relations.size() < 1 || ast.relations.size() > 3) {
    AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                          "native relation graph is outside the bounded profile");
    return RefusedBoundAst(std::move(bound));
  }

  std::unordered_map<std::uint32_t, const NativeRelationBindingInput*>
      relation_binding_by_id;
  for (const auto& relation_binding : context.relations) {
    if (relation_binding.relation_id == 0 ||
        relation_binding.semantic_variant_id.empty() ||
        !ast_relation_by_id.contains(relation_binding.relation_id) ||
        !relation_binding_by_id
             .emplace(relation_binding.relation_id, &relation_binding)
             .second) {
      AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                            "relation semantic binding is missing or contradictory");
      return RefusedBoundAst(std::move(bound));
    }
  }

  std::size_t expected_output_count = 0;
  const NativeRelationAstNode* values_relation_ast = nullptr;
  const NativeRelationAstNode* aggregate_relation_ast = nullptr;
  const NativeRelationAstNode* filter_relation_ast = nullptr;
  bound.relations.reserve(ast.relations.size());
  for (const auto& relation : ast.relations) {
    for (const auto input_id : relation.input_relation_ids) {
      if (input_id == 0 || input_id == relation.relation_id ||
          !ast_relation_by_id.contains(input_id)) {
        AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                              "relation graph contains a dangling or cyclic input");
        return RefusedBoundAst(std::move(bound));
      }
    }
    for (const auto expression_id : relation.output_expression_ids) {
      if (!ast_expression_by_id.contains(expression_id)) {
        AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                              "relation output contains a dangling expression");
        return RefusedBoundAst(std::move(bound));
      }
    }
    for (const auto expression_id : relation.predicate_expression_ids) {
      if (!ast_expression_by_id.contains(expression_id)) {
        AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                              "relation predicate contains a dangling expression");
        return RefusedBoundAst(std::move(bound));
      }
    }
    for (const auto expression_id : relation.limit_expression_ids) {
      if (!ast_expression_by_id.contains(expression_id)) {
        AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                              "relation row bound contains a dangling expression");
        return RefusedBoundAst(std::move(bound));
      }
    }

    BoundRelationAstRecord record;
    record.relation_id = relation.relation_id;
    record.relation_kind = relation.relation_kind;
    record.aggregate_grouping_form = relation.aggregate_grouping_form;
    record.aggregate_projection_form = relation.aggregate_projection_form;
    record.input_relation_ids = relation.input_relation_ids;
    record.values_row_ids = relation.values_row_ids;
    record.output_expression_ids = relation.output_expression_ids;
    record.grouping_key_expression_ids = relation.grouping_key_expression_ids;
    record.aggregate_expression_ids = relation.aggregate_expression_ids;
    record.predicate_expression_ids = relation.predicate_expression_ids;
    record.limit_expression_ids = relation.limit_expression_ids;
    record.bound_object_uuid = std::nullopt;
    record.lateral = false;

    if (relation.relation_kind == NativeRelationAstKind::kValues) {
      if (values_relation_ast != nullptr || !relation.input_relation_ids.empty() ||
          relation.values_row_ids.size() != bound.values_rows.size() ||
          relation.output_expression_ids != first_row.expression_ids ||
          relation.aggregate_grouping_form !=
              NativeAggregateGroupingForm::kNone ||
          relation.aggregate_projection_form !=
              NativeAggregateProjectionForm::kNone ||
          !relation.grouping_key_expression_ids.empty() ||
          !relation.aggregate_expression_ids.empty() ||
          !relation.predicate_expression_ids.empty() ||
          !relation.limit_expression_ids.empty()) {
        AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                              "typed VALUES relation graph is not canonical");
        return RefusedBoundAst(std::move(bound));
      }
      for (std::size_t index = 0; index < relation.values_row_ids.size(); ++index) {
        if (relation.values_row_ids[index] != bound.values_rows[index].row_id) {
          AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                                "VALUES relation row handles are not canonical");
          return RefusedBoundAst(std::move(bound));
        }
      }
      values_relation_ast = &relation;
      record.semantic_variant_id = "values.literal-table.v1";
      for (const auto& row : bound.values_rows) {
        record.bound_expression_ids.insert(record.bound_expression_ids.end(),
                                           row.expression_ids.begin(),
                                           row.expression_ids.end());
      }
    } else if (relation.relation_kind == NativeRelationAstKind::kAggregate) {
      const bool projects_key_count_sum =
          relation.aggregate_projection_form ==
          NativeAggregateProjectionForm::kKeyCountSum;
      const bool projects_keys_count_sum =
          relation.aggregate_projection_form ==
          NativeAggregateProjectionForm::kKeysCountSum;
      const bool projects_grouping_metadata =
          relation.aggregate_projection_form ==
          NativeAggregateProjectionForm::kKeysCountSumGrouping;
      const bool one_key_profile =
          relation.aggregate_grouping_form ==
              NativeAggregateGroupingForm::kSimple &&
          projects_key_count_sum;
      const bool two_key_profile =
          (relation.aggregate_grouping_form ==
               NativeAggregateGroupingForm::kSimple &&
           projects_keys_count_sum) ||
          ((relation.aggregate_grouping_form ==
               NativeAggregateGroupingForm::kGroupingSets ||
           relation.aggregate_grouping_form ==
               NativeAggregateGroupingForm::kRollup ||
           relation.aggregate_grouping_form ==
               NativeAggregateGroupingForm::kCube) &&
           (projects_keys_count_sum || projects_grouping_metadata));
      const std::size_t expected_output_count =
          one_key_profile ? 3 : (projects_grouping_metadata ? 7 : 4);
      bool output_shape_matches = false;
      if (one_key_profile &&
          relation.grouping_key_expression_ids.size() == 1 &&
          relation.aggregate_expression_ids.size() == 2 &&
          relation.output_expression_ids.size() == 3) {
        output_shape_matches =
            relation.output_expression_ids[0] ==
                relation.grouping_key_expression_ids[0] &&
            relation.output_expression_ids[1] ==
                relation.aggregate_expression_ids[0] &&
            relation.output_expression_ids[2] ==
                relation.aggregate_expression_ids[1];
      } else if (two_key_profile &&
                 relation.grouping_key_expression_ids.size() == 2 &&
                 relation.aggregate_expression_ids.size() == 2 &&
                 relation.output_expression_ids.size() ==
                     expected_output_count) {
        output_shape_matches =
            relation.output_expression_ids[0] ==
                relation.grouping_key_expression_ids[0] &&
            relation.output_expression_ids[1] ==
                relation.grouping_key_expression_ids[1] &&
            relation.output_expression_ids[2] ==
                relation.aggregate_expression_ids[0] &&
            relation.output_expression_ids[3] ==
                relation.aggregate_expression_ids[1];
      }
      if (aggregate_relation_ast != nullptr ||
          relation.input_relation_ids.size() != 1 ||
          !relation.values_row_ids.empty() ||
          !relation.predicate_expression_ids.empty() ||
          !relation.limit_expression_ids.empty() ||
          (!one_key_profile && !two_key_profile) ||
          relation.output_expression_ids.size() != expected_output_count ||
          !output_shape_matches) {
        AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                              "typed aggregate relation is outside the bounded profile");
        return RefusedBoundAst(std::move(bound));
      }
      if (projects_grouping_metadata) {
        const auto grouping_a =
            ast_expression_by_id.find(relation.output_expression_ids[4]);
        const auto grouping_b =
            ast_expression_by_id.find(relation.output_expression_ids[5]);
        const auto grouping_id =
            ast_expression_by_id.find(relation.output_expression_ids[6]);
        const auto metadata_descriptor_is_exact =
            [&](const auto expression) {
              if (expression == ast_expression_by_id.end()) return false;
              const auto binding =
                  expression_binding_by_id.find(expression->first);
              if (binding == expression_binding_by_id.end()) return false;
              const auto descriptor =
                  descriptor_by_id.find(binding->second->descriptor_id);
              return descriptor != descriptor_by_id.end() &&
                     !binding->second->function_uuid.has_value() &&
                     !binding->second->bound_name_uuid.has_value() &&
                     descriptor->second->nullability ==
                         BoundNullability::kNonNull &&
                     !descriptor->second->collation_uuid.has_value() &&
                     !descriptor->second->timezone_profile_id.has_value() &&
                     !descriptor->second->width_precision_scale.width
                          .has_value() &&
                     !descriptor->second->width_precision_scale.precision
                          .has_value() &&
                     !descriptor->second->width_precision_scale.scale
                          .has_value();
            };
        if (grouping_a == ast_expression_by_id.end() ||
            grouping_b == ast_expression_by_id.end() ||
            grouping_id == ast_expression_by_id.end() ||
            grouping_a->second->expression_kind !=
                NativeExpressionAstKind::kUnary ||
            grouping_a->second->operator_name != "grouping" ||
            grouping_a->second->child_expression_ids !=
                std::vector<std::uint32_t>{
                    relation.grouping_key_expression_ids[0]} ||
            grouping_b->second->expression_kind !=
                NativeExpressionAstKind::kUnary ||
            grouping_b->second->operator_name != "grouping" ||
            grouping_b->second->child_expression_ids !=
                std::vector<std::uint32_t>{
                    relation.grouping_key_expression_ids[1]} ||
            grouping_id->second->expression_kind !=
                NativeExpressionAstKind::kBinary ||
            grouping_id->second->operator_name != "grouping_id" ||
            grouping_id->second->child_expression_ids !=
                relation.grouping_key_expression_ids ||
            !metadata_descriptor_is_exact(grouping_a) ||
            !metadata_descriptor_is_exact(grouping_b) ||
            !metadata_descriptor_is_exact(grouping_id)) {
          AddBoundAstDiagnostic(
              &bound, "QOW-DIAG-BOUNDAST-RELATION",
              "grouping metadata projections must bind the two ordered keys and non-null descriptors exactly");
          return RefusedBoundAst(std::move(bound));
        }
      }
      const auto semantic_binding =
          relation_binding_by_id.find(relation.relation_id);
      if (semantic_binding == relation_binding_by_id.end()) {
        AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                              "typed aggregate requires an authoritative semantic binding");
        return RefusedBoundAst(std::move(bound));
      }
      const auto expected_semantic =
          ExpectedAggregateSemanticVariant(
              relation.aggregate_grouping_form,
              relation.aggregate_projection_form);
      if (expected_semantic.empty() ||
          semantic_binding->second->semantic_variant_id != expected_semantic) {
        AddBoundAstDiagnostic(
            &bound, "QOW-DIAG-BOUNDAST-RELATION",
            "aggregate semantic binding contradicts the parsed grouping form");
        return RefusedBoundAst(std::move(bound));
      }
      aggregate_relation_ast = &relation;
      record.semantic_variant_id =
          semantic_binding->second->semantic_variant_id;
      record.bound_expression_ids = relation.output_expression_ids;
    } else if (relation.relation_kind == NativeRelationAstKind::kFilter) {
      const auto semantic_binding =
          relation_binding_by_id.find(relation.relation_id);
      const bool expected_not_not_sum_count_or_profile =
          semantic_binding != relation_binding_by_id.end() &&
          semantic_binding->second->semantic_variant_id ==
              "filter.having-not-not-sum-count-or-gt-int64-literals.v1";
      const NativeExpressionAstNode* predicate = nullptr;
      const NativeExpressionAstNode* inner_not = nullptr;
      const NativeExpressionAstNode* boolean_root = nullptr;
      const NativeExpressionAstNode* count_comparison = nullptr;
      const NativeExpressionAstNode* sum_comparison = nullptr;
      const NativeExpressionAstNode* having_count = nullptr;
      const NativeExpressionAstNode* count_threshold = nullptr;
      const NativeExpressionAstNode* having_sum = nullptr;
      const NativeExpressionAstNode* sum_threshold = nullptr;
      const NativeExpressionAstNode* having_argument = nullptr;
      const NativeExpressionAstNode* projected_count = nullptr;
      const NativeExpressionAstNode* projected_sum = nullptr;
      const NativeExpressionAstNode* projected_argument = nullptr;
      if (relation.predicate_expression_ids.size() == 1) {
        predicate = ast_expression_by_id.at(
            relation.predicate_expression_ids.front());
        if (predicate->expression_kind == NativeExpressionAstKind::kBinary &&
            predicate->operator_name == ">" &&
            predicate->child_expression_ids.size() == 2) {
          sum_comparison = predicate;
        } else if (predicate->expression_kind ==
                       NativeExpressionAstKind::kUnary &&
                   predicate->operator_name == "NOT" &&
                   predicate->child_expression_ids.size() == 1) {
          const auto* operand = ast_expression_by_id.at(
              predicate->child_expression_ids.front());
          if (operand->expression_kind == NativeExpressionAstKind::kUnary &&
              operand->operator_name == "NOT" &&
              operand->child_expression_ids.size() == 1) {
            inner_not = operand;
            const auto* inner_operand = ast_expression_by_id.at(
                operand->child_expression_ids.front());
            if (inner_operand->expression_kind ==
                    NativeExpressionAstKind::kBinary &&
                (inner_operand->operator_name == "AND" ||
                 inner_operand->operator_name == "OR") &&
                inner_operand->child_expression_ids.size() == 2) {
              boolean_root = inner_operand;
              if (expected_not_not_sum_count_or_profile &&
                  inner_operand->operator_name == "OR") {
                sum_comparison = ast_expression_by_id.at(
                    inner_operand->child_expression_ids[0]);
                count_comparison = ast_expression_by_id.at(
                    inner_operand->child_expression_ids[1]);
              } else {
                count_comparison = ast_expression_by_id.at(
                    inner_operand->child_expression_ids[0]);
                sum_comparison = ast_expression_by_id.at(
                    inner_operand->child_expression_ids[1]);
              }
            } else {
              const auto* comparison = inner_operand;
              const auto* function =
                  comparison->expression_kind ==
                              NativeExpressionAstKind::kBinary &&
                          comparison->operator_name == ">" &&
                          comparison->child_expression_ids.size() == 2
                      ? ast_expression_by_id.at(
                            comparison->child_expression_ids.front())
                      : nullptr;
              if (function != nullptr &&
                  function->expression_kind ==
                      NativeExpressionAstKind::kFunctionCall &&
                  ToUpperAscii(function->operator_name) == "COUNT") {
                count_comparison = comparison;
              } else {
                sum_comparison = comparison;
              }
            }
          } else if (operand->expression_kind ==
                         NativeExpressionAstKind::kBinary &&
              (operand->operator_name == "AND" ||
               operand->operator_name == "OR") &&
              operand->child_expression_ids.size() == 2) {
            boolean_root = operand;
            count_comparison = ast_expression_by_id.at(
                operand->child_expression_ids[0]);
            sum_comparison = ast_expression_by_id.at(
                operand->child_expression_ids[1]);
          } else {
            const auto* function =
                operand->expression_kind == NativeExpressionAstKind::kBinary &&
                        operand->operator_name == ">" &&
                        operand->child_expression_ids.size() == 2
                    ? ast_expression_by_id.at(
                          operand->child_expression_ids.front())
                    : nullptr;
            if (function != nullptr &&
                function->expression_kind ==
                    NativeExpressionAstKind::kFunctionCall &&
                ToUpperAscii(function->operator_name) == "COUNT") {
              count_comparison = operand;
            } else {
              sum_comparison = operand;
            }
          }
        } else if (predicate->expression_kind ==
                       NativeExpressionAstKind::kBinary &&
                   (predicate->operator_name == "AND" ||
                    predicate->operator_name == "OR") &&
                   predicate->child_expression_ids.size() == 2) {
          boolean_root = predicate;
          count_comparison =
              ast_expression_by_id.at(predicate->child_expression_ids[0]);
          sum_comparison =
              ast_expression_by_id.at(predicate->child_expression_ids[1]);
        }
        if (count_comparison != nullptr &&
            count_comparison->expression_kind ==
                NativeExpressionAstKind::kBinary &&
            count_comparison->operator_name == ">" &&
            count_comparison->child_expression_ids.size() == 2) {
          having_count = ast_expression_by_id.at(
              count_comparison->child_expression_ids[0]);
          count_threshold = ast_expression_by_id.at(
              count_comparison->child_expression_ids[1]);
        }
        if (sum_comparison != nullptr &&
            sum_comparison->expression_kind ==
                NativeExpressionAstKind::kBinary &&
            sum_comparison->operator_name == ">" &&
            sum_comparison->child_expression_ids.size() == 2) {
          having_sum = ast_expression_by_id.at(
              sum_comparison->child_expression_ids[0]);
          sum_threshold = ast_expression_by_id.at(
              sum_comparison->child_expression_ids[1]);
          if (having_sum->child_expression_ids.size() == 1) {
            having_argument = ast_expression_by_id.at(
                having_sum->child_expression_ids.front());
          }
        }
      }
      if (aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_expression_ids.size() == 2) {
        projected_count = ast_expression_by_id.at(
            aggregate_relation_ast->aggregate_expression_ids[0]);
        projected_sum = ast_expression_by_id.at(
            aggregate_relation_ast->aggregate_expression_ids[1]);
        if (projected_sum->child_expression_ids.size() == 1) {
          projected_argument = ast_expression_by_id.at(
              projected_sum->child_expression_ids.front());
        }
      }
      const auto binding_for = [&](const NativeExpressionAstNode* expression) {
        return expression == nullptr
                   ? expression_binding_by_id.end()
                   : expression_binding_by_id.find(expression->expression_id);
      };
      const auto descriptor_is = [&](const NativeExpressionAstNode* expression,
                                     const BoundNullability nullability) {
        const auto binding = binding_for(expression);
        return binding != expression_binding_by_id.end() &&
               descriptor_by_id.at(binding->second->descriptor_id)
                       ->nullability == nullability;
      };
      const auto descriptor_is_unqualified =
          [&](const NativeExpressionAstNode* expression,
              const BoundNullability nullability) {
            const auto binding = binding_for(expression);
            if (binding == expression_binding_by_id.end()) return false;
            const auto descriptor =
                descriptor_by_id.at(binding->second->descriptor_id);
            return descriptor->nullability == nullability &&
                   !descriptor->collation_uuid.has_value() &&
                   !descriptor->timezone_profile_id.has_value() &&
                   !descriptor->width_precision_scale.width.has_value() &&
                   !descriptor->width_precision_scale.precision.has_value() &&
                   !descriptor->width_precision_scale.scale.has_value();
          };
      const auto predicate_binding = binding_for(predicate);
      const auto inner_not_binding = binding_for(inner_not);
      const auto boolean_root_binding = binding_for(boolean_root);
      const auto count_comparison_binding = binding_for(count_comparison);
      const auto sum_comparison_binding = binding_for(sum_comparison);
      const auto having_count_binding = binding_for(having_count);
      const auto projected_count_binding = binding_for(projected_count);
      const auto having_sum_binding = binding_for(having_sum);
      const auto projected_sum_binding = binding_for(projected_sum);
      const auto having_argument_binding = binding_for(having_argument);
      const auto projected_argument_binding = binding_for(projected_argument);
      const bool simple_sum_profile =
          predicate != nullptr && sum_comparison == predicate;
      // QOW-SOURCE-QRY-001-BINDING-TWO-KEY-HAVING-NOT-NOT-COUNT-SUM-AND-GT-V1
      const bool not_not_count_sum_and_profile =
          count_comparison != nullptr && sum_comparison != nullptr &&
          predicate != nullptr && inner_not != nullptr &&
          boolean_root != nullptr &&
          predicate->expression_kind == NativeExpressionAstKind::kUnary &&
          predicate->operator_name == "NOT" &&
          predicate->child_expression_ids ==
              std::vector<std::uint32_t>{inner_not->expression_id} &&
          inner_not->expression_kind == NativeExpressionAstKind::kUnary &&
          inner_not->operator_name == "NOT" &&
          inner_not->child_expression_ids ==
              std::vector<std::uint32_t>{boolean_root->expression_id} &&
          boolean_root->operator_name == "AND";
      // QOW-SOURCE-QRY-001-BINDING-TWO-KEY-HAVING-NOT-NOT-COUNT-SUM-OR-GT-V1
      const bool not_not_count_sum_or_profile =
          count_comparison != nullptr && sum_comparison != nullptr &&
          predicate != nullptr && inner_not != nullptr &&
          boolean_root != nullptr &&
          predicate->expression_kind == NativeExpressionAstKind::kUnary &&
          predicate->operator_name == "NOT" &&
          predicate->child_expression_ids ==
              std::vector<std::uint32_t>{inner_not->expression_id} &&
          inner_not->expression_kind == NativeExpressionAstKind::kUnary &&
          inner_not->operator_name == "NOT" &&
          inner_not->child_expression_ids ==
              std::vector<std::uint32_t>{boolean_root->expression_id} &&
          boolean_root->operator_name == "OR" &&
          !expected_not_not_sum_count_or_profile;
      // QOW-SOURCE-QRY-001-BINDING-TWO-KEY-HAVING-NOT-NOT-SUM-COUNT-OR-GT-V1
      const bool not_not_sum_count_or_profile =
          count_comparison != nullptr && sum_comparison != nullptr &&
          predicate != nullptr && inner_not != nullptr &&
          boolean_root != nullptr &&
          predicate->expression_kind == NativeExpressionAstKind::kUnary &&
          predicate->operator_name == "NOT" &&
          predicate->child_expression_ids ==
              std::vector<std::uint32_t>{inner_not->expression_id} &&
          inner_not->expression_kind == NativeExpressionAstKind::kUnary &&
          inner_not->operator_name == "NOT" &&
          inner_not->child_expression_ids ==
              std::vector<std::uint32_t>{boolean_root->expression_id} &&
          boolean_root->operator_name == "OR" &&
          expected_not_not_sum_count_or_profile;
      const bool not_not_count_sum_boolean_profile =
          not_not_count_sum_and_profile || not_not_count_sum_or_profile ||
          not_not_sum_count_or_profile;
      const bool not_not_sum_profile =
          predicate != nullptr && inner_not != nullptr &&
          sum_comparison != nullptr &&
          predicate->expression_kind == NativeExpressionAstKind::kUnary &&
          predicate->operator_name == "NOT" &&
          predicate->child_expression_ids ==
              std::vector<std::uint32_t>{inner_not->expression_id} &&
          inner_not->expression_kind == NativeExpressionAstKind::kUnary &&
          inner_not->operator_name == "NOT" &&
          inner_not->child_expression_ids ==
              std::vector<std::uint32_t>{sum_comparison->expression_id};
      const bool not_not_count_profile =
          predicate != nullptr && inner_not != nullptr &&
          count_comparison != nullptr &&
          predicate->expression_kind == NativeExpressionAstKind::kUnary &&
          predicate->operator_name == "NOT" &&
          predicate->child_expression_ids ==
              std::vector<std::uint32_t>{inner_not->expression_id} &&
          inner_not->expression_kind == NativeExpressionAstKind::kUnary &&
          inner_not->operator_name == "NOT" &&
          inner_not->child_expression_ids ==
              std::vector<std::uint32_t>{count_comparison->expression_id};
      const bool count_sum_and_profile =
          count_comparison != nullptr && predicate != nullptr &&
          boolean_root == predicate && predicate->operator_name == "AND";
      const bool count_sum_or_profile =
          count_comparison != nullptr && predicate != nullptr &&
          boolean_root == predicate && predicate->operator_name == "OR";
      const bool not_count_sum_and_profile =
          count_comparison != nullptr && predicate != nullptr &&
          boolean_root != nullptr && boolean_root != predicate &&
          predicate->expression_kind == NativeExpressionAstKind::kUnary &&
          predicate->operator_name == "NOT" &&
          predicate->child_expression_ids ==
              std::vector<std::uint32_t>{boolean_root->expression_id} &&
          boolean_root->operator_name == "AND";
      const bool not_count_sum_or_profile =
          count_comparison != nullptr && predicate != nullptr &&
          boolean_root != nullptr && boolean_root != predicate &&
          predicate->expression_kind == NativeExpressionAstKind::kUnary &&
          predicate->operator_name == "NOT" &&
          predicate->child_expression_ids ==
              std::vector<std::uint32_t>{boolean_root->expression_id} &&
          boolean_root->operator_name == "OR";
      const bool not_count_sum_boolean_profile =
          not_count_sum_and_profile || not_count_sum_or_profile;
      const bool count_sum_boolean_profile =
          count_sum_and_profile || count_sum_or_profile ||
          not_count_sum_boolean_profile ||
          not_not_count_sum_boolean_profile;
      const bool not_sum_profile =
          predicate != nullptr && sum_comparison != nullptr &&
          predicate->expression_kind == NativeExpressionAstKind::kUnary &&
          predicate->operator_name == "NOT" &&
          predicate->child_expression_ids ==
              std::vector<std::uint32_t>{sum_comparison->expression_id};
      const bool not_count_profile =
          predicate != nullptr && count_comparison != nullptr &&
          boolean_root == nullptr &&
          predicate->expression_kind == NativeExpressionAstKind::kUnary &&
          predicate->operator_name == "NOT" &&
          predicate->child_expression_ids ==
              std::vector<std::uint32_t>{count_comparison->expression_id};
      const bool not_sum_descriptors_are_exact =
          !not_sum_profile ||
          (descriptor_is_unqualified(predicate,
                                     BoundNullability::kNullable) &&
           descriptor_is_unqualified(sum_comparison,
                                     BoundNullability::kNullable) &&
           descriptor_is_unqualified(sum_threshold,
                                     BoundNullability::kNonNull));
      const bool not_not_sum_descriptors_are_exact =
          !not_not_sum_profile ||
          (descriptor_is_unqualified(predicate,
                                      BoundNullability::kNullable) &&
           descriptor_is_unqualified(inner_not,
                                      BoundNullability::kNullable) &&
           descriptor_is_unqualified(sum_comparison,
                                      BoundNullability::kNullable) &&
           descriptor_is_unqualified(having_sum,
                                      BoundNullability::kNullable) &&
           descriptor_is_unqualified(projected_sum,
                                      BoundNullability::kNullable) &&
           descriptor_is_unqualified(sum_threshold,
                                      BoundNullability::kNonNull));
      const bool not_not_count_descriptors_are_exact =
          !not_not_count_profile ||
          (descriptor_is_unqualified(predicate,
                                      BoundNullability::kNullable) &&
           descriptor_is_unqualified(inner_not,
                                      BoundNullability::kNullable) &&
           descriptor_is_unqualified(count_comparison,
                                      BoundNullability::kNullable) &&
           descriptor_is_unqualified(count_threshold,
                                      BoundNullability::kNonNull) &&
           descriptor_is_unqualified(having_count,
                                      BoundNullability::kNonNull) &&
           descriptor_is_unqualified(projected_count,
                                      BoundNullability::kNonNull));
      const bool not_count_sum_boolean_descriptors_are_exact =
          (!not_count_sum_boolean_profile &&
           !not_not_count_sum_boolean_profile) ||
          (descriptor_is_unqualified(predicate,
                                      BoundNullability::kNullable) &&
           (!not_not_count_sum_boolean_profile ||
            descriptor_is_unqualified(inner_not,
                                      BoundNullability::kNullable)) &&
           descriptor_is_unqualified(boolean_root,
                                      BoundNullability::kNullable) &&
           descriptor_is_unqualified(count_comparison,
                                      BoundNullability::kNullable) &&
           descriptor_is_unqualified(sum_comparison,
                                      BoundNullability::kNullable) &&
           descriptor_is_unqualified(count_threshold,
                                      BoundNullability::kNonNull) &&
           descriptor_is_unqualified(sum_threshold,
                                      BoundNullability::kNonNull));
      const bool not_count_descriptors_are_exact =
          !not_count_profile ||
          (descriptor_is_unqualified(predicate,
                                     BoundNullability::kNullable) &&
           descriptor_is_unqualified(count_comparison,
                                     BoundNullability::kNullable) &&
           descriptor_is_unqualified(count_threshold,
                                     BoundNullability::kNonNull) &&
           descriptor_is_unqualified(having_count,
                                     BoundNullability::kNonNull) &&
           descriptor_is_unqualified(projected_count,
                                     BoundNullability::kNonNull));
      const std::string_view expected_filter_semantic =
          not_not_sum_count_or_profile
              ? "filter.having-not-not-sum-count-or-gt-int64-literals.v1"
          : not_not_count_sum_or_profile
              ? "filter.having-not-not-count-sum-or-gt-int64-literals.v1"
          : not_not_count_sum_and_profile
              ? "filter.having-not-not-count-sum-and-gt-int64-literals.v1"
          : not_not_count_profile
              ? "filter.having-not-not-count-gt-int64-literal.v1"
          : not_not_sum_profile
              ? "filter.having-not-not-sum-gt-int64-literal.v1"
          : not_count_sum_or_profile
              ? "filter.having-not-count-sum-or-gt-int64-literals.v1"
              : not_count_sum_and_profile
              ? "filter.having-not-count-sum-and-gt-int64-literals.v1"
              : not_count_profile
              ? "filter.having-not-count-gt-int64-literal.v1"
              : not_sum_profile
              ? "filter.having-not-sum-gt-int64-literal.v1"
              : count_sum_or_profile
              ? "filter.having-count-sum-or-gt-int64-literals.v1"
              : (count_sum_and_profile
                     ? "filter.having-count-sum-and-gt-int64-literals.v1"
                     : "filter.having-sum-gt-int64-literal.v1");
      // QOW-SOURCE-QRY-001-BINDING-HAVING-COUNT-SUM-OR-GT-V1
      const bool admitted_one_key_or_having =
          count_sum_or_profile && aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kSimple &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeyCountSum;
      // QOW-SOURCE-QRY-001-BINDING-TWO-KEY-HAVING-COUNT-SUM-OR-GT-V1
      const bool admitted_two_key_or_having =
          count_sum_or_profile && aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kSimple &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSum;
      // QOW-SOURCE-QRY-001-BINDING-GROUPING-SETS-HAVING-COUNT-SUM-OR-GT-V1
      const bool admitted_grouping_sets_or_having =
          count_sum_or_profile && aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kGroupingSets &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSum &&
          aggregate_relation_ast->grouping_key_expression_ids.size() == 2 &&
          ast.grouping_sets.size() == 4 &&
          ast.grouping_sets[0].relation_id ==
              aggregate_relation_ast->relation_id &&
          ast.grouping_sets[0].ordinal == 0 &&
          ast.grouping_sets[0].expression_ids ==
              std::vector<std::uint32_t>{
                  aggregate_relation_ast->grouping_key_expression_ids[1]} &&
          ast.grouping_sets[1].relation_id ==
              aggregate_relation_ast->relation_id &&
          ast.grouping_sets[1].ordinal == 1 &&
          ast.grouping_sets[1].expression_ids.empty() &&
          ast.grouping_sets[2].relation_id ==
              aggregate_relation_ast->relation_id &&
          ast.grouping_sets[2].ordinal == 2 &&
          ast.grouping_sets[2].expression_ids ==
              std::vector<std::uint32_t>{
                  aggregate_relation_ast->grouping_key_expression_ids[1],
                  aggregate_relation_ast->grouping_key_expression_ids[0]} &&
          ast.grouping_sets[3].relation_id ==
              aggregate_relation_ast->relation_id &&
          ast.grouping_sets[3].ordinal == 3 &&
          ast.grouping_sets[3].expression_ids ==
              ast.grouping_sets[0].expression_ids;
      // QOW-SOURCE-QRY-001-BINDING-GROUPING-SETS-GROUPING-METADATA-HAVING-COUNT-SUM-OR-GT-V1
      const bool admitted_grouping_sets_metadata_or_having =
          count_sum_or_profile && aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kGroupingSets &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSumGrouping &&
          aggregate_relation_ast->grouping_key_expression_ids.size() == 2 &&
          ast.grouping_sets.size() == 4 &&
          ast.grouping_sets[0].relation_id ==
              aggregate_relation_ast->relation_id &&
          ast.grouping_sets[0].ordinal == 0 &&
          ast.grouping_sets[0].expression_ids ==
              std::vector<std::uint32_t>{
                  aggregate_relation_ast->grouping_key_expression_ids[1]} &&
          ast.grouping_sets[1].relation_id ==
              aggregate_relation_ast->relation_id &&
          ast.grouping_sets[1].ordinal == 1 &&
          ast.grouping_sets[1].expression_ids.empty() &&
          ast.grouping_sets[2].relation_id ==
              aggregate_relation_ast->relation_id &&
          ast.grouping_sets[2].ordinal == 2 &&
          ast.grouping_sets[2].expression_ids ==
              std::vector<std::uint32_t>{
                  aggregate_relation_ast->grouping_key_expression_ids[1],
                  aggregate_relation_ast->grouping_key_expression_ids[0]} &&
          ast.grouping_sets[3].relation_id ==
              aggregate_relation_ast->relation_id &&
          ast.grouping_sets[3].ordinal == 3 &&
          ast.grouping_sets[3].expression_ids ==
              ast.grouping_sets[0].expression_ids;
      // QOW-SOURCE-QRY-001-BINDING-ROLLUP-HAVING-COUNT-SUM-OR-GT-V1
      const bool admitted_rollup_or_having =
          count_sum_or_profile && aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kRollup &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSum &&
          aggregate_relation_ast->grouping_key_expression_ids.size() == 2 &&
          ast.grouping_sets.empty();
      // QOW-SOURCE-QRY-001-BINDING-ROLLUP-GROUPING-METADATA-HAVING-COUNT-SUM-OR-GT-V1
      const bool admitted_rollup_metadata_or_having =
          count_sum_or_profile && aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kRollup &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSumGrouping &&
          aggregate_relation_ast->grouping_key_expression_ids.size() == 2 &&
          ast.grouping_sets.empty();
      // QOW-SOURCE-QRY-001-BINDING-CUBE-HAVING-COUNT-SUM-OR-GT-V1
      const bool admitted_cube_or_having =
          count_sum_or_profile && aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kCube &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSum &&
          aggregate_relation_ast->grouping_key_expression_ids.size() == 2 &&
          ast.grouping_sets.empty();
      // QOW-SOURCE-QRY-001-BINDING-CUBE-GROUPING-METADATA-HAVING-COUNT-SUM-OR-GT-V1
      const bool admitted_cube_metadata_or_having =
          count_sum_or_profile && aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kCube &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSumGrouping &&
          aggregate_relation_ast->grouping_key_expression_ids.size() == 2 &&
          ast.grouping_sets.empty();
      // QOW-SOURCE-QRY-001-BINDING-TWO-KEY-HAVING-NOT-SUM-GT-V1
      const bool admitted_two_key_not_sum_having =
          not_sum_profile && aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kSimple &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSum &&
          aggregate_relation_ast->grouping_key_expression_ids.size() == 2 &&
          ast.grouping_sets.empty();
      // QOW-SOURCE-QRY-001-BINDING-TWO-KEY-HAVING-NOT-NOT-SUM-GT-V1
      const bool admitted_two_key_not_not_sum_having =
          not_not_sum_profile && aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kSimple &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSum &&
          aggregate_relation_ast->grouping_key_expression_ids.size() == 2 &&
          ast.grouping_sets.empty();
      // QOW-SOURCE-QRY-001-BINDING-TWO-KEY-HAVING-NOT-NOT-COUNT-GT-V1
      const bool admitted_two_key_not_not_count_having =
          not_not_count_profile && aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kSimple &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSum &&
          aggregate_relation_ast->grouping_key_expression_ids.size() == 2 &&
          ast.grouping_sets.empty();
      // QOW-SOURCE-QRY-001-BINDING-TWO-KEY-HAVING-NOT-NOT-COUNT-SUM-AND-GT-ADMISSION-V1
      const bool admitted_two_key_not_not_count_sum_and_having =
          not_not_count_sum_and_profile &&
          aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kSimple &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSum &&
          aggregate_relation_ast->grouping_key_expression_ids.size() == 2 &&
          ast.grouping_sets.empty();
      // QOW-SOURCE-QRY-001-BINDING-TWO-KEY-HAVING-NOT-NOT-COUNT-SUM-OR-GT-ADMISSION-V1
      const bool admitted_two_key_not_not_count_sum_or_having =
          not_not_count_sum_or_profile &&
          aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kSimple &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSum &&
          aggregate_relation_ast->grouping_key_expression_ids.size() == 2 &&
          ast.grouping_sets.empty();
      // QOW-SOURCE-QRY-001-BINDING-TWO-KEY-HAVING-NOT-NOT-SUM-COUNT-OR-GT-ADMISSION-V1
      const bool admitted_two_key_not_not_sum_count_or_having =
          not_not_sum_count_or_profile &&
          aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kSimple &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSum &&
          aggregate_relation_ast->grouping_key_expression_ids.size() == 2 &&
          ast.grouping_sets.empty();
      // QOW-SOURCE-QRY-001-BINDING-TWO-KEY-HAVING-NOT-COUNT-GT-V1
      const bool admitted_two_key_not_count_having =
          not_count_profile && aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kSimple &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSum &&
          aggregate_relation_ast->grouping_key_expression_ids.size() == 2 &&
          ast.grouping_sets.empty();
      // QOW-SOURCE-QRY-001-BINDING-TWO-KEY-HAVING-NOT-COUNT-SUM-AND-GT-V1
      const bool admitted_two_key_not_count_sum_and_having =
          not_count_sum_and_profile && aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kSimple &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSum &&
          aggregate_relation_ast->grouping_key_expression_ids.size() == 2 &&
          ast.grouping_sets.empty();
      // QOW-SOURCE-QRY-001-BINDING-TWO-KEY-HAVING-NOT-COUNT-SUM-OR-GT-V1
      const bool admitted_two_key_not_count_sum_or_having =
          not_count_sum_or_profile && aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kSimple &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSum &&
          aggregate_relation_ast->grouping_key_expression_ids.size() == 2 &&
          ast.grouping_sets.empty();
      // QOW-SOURCE-QRY-001-BINDING-GROUPING-SETS-HAVING-NOT-COUNT-SUM-AND-GT-V1
      const bool admitted_grouping_sets_not_count_sum_and_having =
          not_count_sum_and_profile && aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kGroupingSets &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSum &&
          aggregate_relation_ast->grouping_key_expression_ids.size() == 2 &&
          ast.grouping_sets.size() == 4 &&
          ast.grouping_sets[0].relation_id ==
              aggregate_relation_ast->relation_id &&
          ast.grouping_sets[0].ordinal == 0 &&
          ast.grouping_sets[0].expression_ids ==
              std::vector<std::uint32_t>{
                  aggregate_relation_ast->grouping_key_expression_ids[1]} &&
          ast.grouping_sets[1].relation_id ==
              aggregate_relation_ast->relation_id &&
          ast.grouping_sets[1].ordinal == 1 &&
          ast.grouping_sets[1].expression_ids.empty() &&
          ast.grouping_sets[2].relation_id ==
              aggregate_relation_ast->relation_id &&
          ast.grouping_sets[2].ordinal == 2 &&
          ast.grouping_sets[2].expression_ids ==
              std::vector<std::uint32_t>{
                  aggregate_relation_ast->grouping_key_expression_ids[1],
                  aggregate_relation_ast->grouping_key_expression_ids[0]} &&
          ast.grouping_sets[3].relation_id ==
              aggregate_relation_ast->relation_id &&
          ast.grouping_sets[3].ordinal == 3 &&
          ast.grouping_sets[3].expression_ids ==
              ast.grouping_sets[0].expression_ids;
      // QOW-SOURCE-QRY-001-BINDING-GROUPING-SETS-GROUPING-METADATA-HAVING-NOT-COUNT-SUM-AND-GT-V1
      const bool admitted_grouping_sets_metadata_not_count_sum_and_having =
          not_count_sum_and_profile && aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kGroupingSets &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSumGrouping &&
          aggregate_relation_ast->grouping_key_expression_ids.size() == 2 &&
          ast.grouping_sets.size() == 4 &&
          ast.grouping_sets[0].relation_id ==
              aggregate_relation_ast->relation_id &&
          ast.grouping_sets[0].ordinal == 0 &&
          ast.grouping_sets[0].expression_ids ==
              std::vector<std::uint32_t>{
                  aggregate_relation_ast->grouping_key_expression_ids[1]} &&
          ast.grouping_sets[1].relation_id ==
              aggregate_relation_ast->relation_id &&
          ast.grouping_sets[1].ordinal == 1 &&
          ast.grouping_sets[1].expression_ids.empty() &&
          ast.grouping_sets[2].relation_id ==
              aggregate_relation_ast->relation_id &&
          ast.grouping_sets[2].ordinal == 2 &&
          ast.grouping_sets[2].expression_ids ==
              std::vector<std::uint32_t>{
                  aggregate_relation_ast->grouping_key_expression_ids[1],
                  aggregate_relation_ast->grouping_key_expression_ids[0]} &&
          ast.grouping_sets[3].relation_id ==
              aggregate_relation_ast->relation_id &&
          ast.grouping_sets[3].ordinal == 3 &&
          ast.grouping_sets[3].expression_ids ==
              ast.grouping_sets[0].expression_ids;
      // QOW-SOURCE-QRY-001-BINDING-ROLLUP-HAVING-NOT-COUNT-SUM-AND-GT-V1
      const bool admitted_rollup_not_count_sum_and_having =
          not_count_sum_and_profile && aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kRollup &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSum &&
          aggregate_relation_ast->grouping_key_expression_ids.size() == 2 &&
          ast.grouping_sets.empty();
      // QOW-SOURCE-QRY-001-BINDING-ROLLUP-GROUPING-METADATA-HAVING-NOT-COUNT-SUM-AND-GT-V1
      const bool admitted_rollup_metadata_not_count_sum_and_having =
          not_count_sum_and_profile && aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kRollup &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSumGrouping &&
          aggregate_relation_ast->grouping_key_expression_ids.size() == 2 &&
          ast.grouping_sets.empty();
      // QOW-SOURCE-QRY-001-BINDING-CUBE-HAVING-NOT-COUNT-SUM-AND-GT-V1
      const bool admitted_cube_not_count_sum_and_having =
          not_count_sum_and_profile && aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kCube &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSum &&
          aggregate_relation_ast->grouping_key_expression_ids.size() == 2 &&
          ast.grouping_sets.empty();
      // QOW-SOURCE-QRY-001-BINDING-CUBE-GROUPING-METADATA-HAVING-NOT-COUNT-SUM-AND-GT-V1
      const bool admitted_cube_metadata_not_count_sum_and_having =
          not_count_sum_and_profile && aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kCube &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSumGrouping &&
          aggregate_relation_ast->grouping_key_expression_ids.size() == 2 &&
          ast.grouping_sets.empty();
      // QOW-SOURCE-QRY-001-BINDING-GROUPING-SETS-HAVING-NOT-SUM-GT-V1
      // QOW-SOURCE-QRY-001-BINDING-GROUPING-SETS-GROUPING-METADATA-HAVING-NOT-SUM-GT-V1
      const bool admitted_grouping_sets_not_sum_having =
          not_sum_profile && aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kGroupingSets &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSum &&
          aggregate_relation_ast->grouping_key_expression_ids.size() == 2 &&
          ast.grouping_sets.size() == 4 &&
          ast.grouping_sets[0].relation_id ==
              aggregate_relation_ast->relation_id &&
          ast.grouping_sets[0].ordinal == 0 &&
          ast.grouping_sets[0].expression_ids ==
              std::vector<std::uint32_t>{
                  aggregate_relation_ast->grouping_key_expression_ids[1]} &&
          ast.grouping_sets[1].relation_id ==
              aggregate_relation_ast->relation_id &&
          ast.grouping_sets[1].ordinal == 1 &&
          ast.grouping_sets[1].expression_ids.empty() &&
          ast.grouping_sets[2].relation_id ==
              aggregate_relation_ast->relation_id &&
          ast.grouping_sets[2].ordinal == 2 &&
          ast.grouping_sets[2].expression_ids ==
              std::vector<std::uint32_t>{
                  aggregate_relation_ast->grouping_key_expression_ids[1],
                  aggregate_relation_ast->grouping_key_expression_ids[0]} &&
          ast.grouping_sets[3].relation_id ==
              aggregate_relation_ast->relation_id &&
          ast.grouping_sets[3].ordinal == 3 &&
          ast.grouping_sets[3].expression_ids ==
              ast.grouping_sets[0].expression_ids;
      const bool admitted_grouping_sets_metadata_not_sum_having =
          not_sum_profile && aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kGroupingSets &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSumGrouping &&
          aggregate_relation_ast->grouping_key_expression_ids.size() == 2 &&
          ast.grouping_sets.size() == 4 &&
          ast.grouping_sets[0].relation_id ==
              aggregate_relation_ast->relation_id &&
          ast.grouping_sets[0].ordinal == 0 &&
          ast.grouping_sets[0].expression_ids ==
              std::vector<std::uint32_t>{
                  aggregate_relation_ast->grouping_key_expression_ids[1]} &&
          ast.grouping_sets[1].relation_id ==
              aggregate_relation_ast->relation_id &&
          ast.grouping_sets[1].ordinal == 1 &&
          ast.grouping_sets[1].expression_ids.empty() &&
          ast.grouping_sets[2].relation_id ==
              aggregate_relation_ast->relation_id &&
          ast.grouping_sets[2].ordinal == 2 &&
          ast.grouping_sets[2].expression_ids ==
              std::vector<std::uint32_t>{
                  aggregate_relation_ast->grouping_key_expression_ids[1],
                  aggregate_relation_ast->grouping_key_expression_ids[0]} &&
          ast.grouping_sets[3].relation_id ==
              aggregate_relation_ast->relation_id &&
          ast.grouping_sets[3].ordinal == 3 &&
          ast.grouping_sets[3].expression_ids ==
              ast.grouping_sets[0].expression_ids;
      // QOW-SOURCE-QRY-001-BINDING-ROLLUP-HAVING-NOT-SUM-GT-V1
      const bool admitted_rollup_not_sum_having =
          not_sum_profile && aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kRollup &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSum &&
          aggregate_relation_ast->grouping_key_expression_ids.size() == 2 &&
          ast.grouping_sets.empty();
      // QOW-SOURCE-QRY-001-BINDING-ROLLUP-GROUPING-METADATA-HAVING-NOT-SUM-GT-V1
      const bool admitted_rollup_metadata_not_sum_having =
          not_sum_profile && aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kRollup &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSumGrouping &&
          aggregate_relation_ast->grouping_key_expression_ids.size() == 2 &&
          ast.grouping_sets.empty();
      // QOW-SOURCE-QRY-001-BINDING-CUBE-HAVING-NOT-SUM-GT-V1
      const bool admitted_cube_not_sum_having =
          not_sum_profile && aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kCube &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSum &&
          aggregate_relation_ast->grouping_key_expression_ids.size() == 2 &&
          ast.grouping_sets.empty();
      // QOW-SOURCE-QRY-001-BINDING-CUBE-GROUPING-METADATA-HAVING-NOT-SUM-GT-V1
      const bool admitted_cube_metadata_not_sum_having =
          not_sum_profile && aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kCube &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSumGrouping &&
          aggregate_relation_ast->grouping_key_expression_ids.size() == 2 &&
          ast.grouping_sets.empty();
      // QOW-SOURCE-QRY-001-BINDING-TWO-KEY-HAVING-SUM-GT-V1
      const bool admitted_simple_having =
          (simple_sum_profile ||
           (count_sum_and_profile && aggregate_relation_ast != nullptr &&
            aggregate_relation_ast->aggregate_projection_form ==
                NativeAggregateProjectionForm::kKeyCountSum)) &&
          aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kSimple &&
          (aggregate_relation_ast->aggregate_projection_form ==
               NativeAggregateProjectionForm::kKeyCountSum ||
           aggregate_relation_ast->aggregate_projection_form ==
               NativeAggregateProjectionForm::kKeysCountSum);
      // QOW-SOURCE-QRY-001-BINDING-GROUPING-SETS-HAVING-SUM-GT-V1
      const bool admitted_grouping_sets_sum_having =
          simple_sum_profile && aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kGroupingSets &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSum;
      // QOW-SOURCE-QRY-001-BINDING-GROUPING-SETS-GROUPING-METADATA-HAVING-SUM-GT-V1
      const bool admitted_grouping_sets_metadata_sum_having =
          simple_sum_profile && aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kGroupingSets &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSumGrouping;
      // QOW-SOURCE-QRY-001-BINDING-ROLLUP-HAVING-SUM-GT-V1
      const bool admitted_rollup_sum_having =
          simple_sum_profile && aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kRollup &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSum;
      // QOW-SOURCE-QRY-001-BINDING-ROLLUP-GROUPING-METADATA-HAVING-SUM-GT-V1
      const bool admitted_rollup_metadata_sum_having =
          simple_sum_profile && aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kRollup &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSumGrouping;
      // QOW-SOURCE-QRY-001-BINDING-CUBE-HAVING-SUM-GT-V1
      const bool admitted_cube_sum_having =
          simple_sum_profile && aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kCube &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSum;
      // QOW-SOURCE-QRY-001-BINDING-CUBE-GROUPING-METADATA-HAVING-SUM-GT-V1
      const bool admitted_cube_metadata_sum_having =
          simple_sum_profile && aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kCube &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSumGrouping;
      // QOW-SOURCE-QRY-001-BINDING-GROUPING-SETS-GROUPING-METADATA-HAVING-V1
      // QOW-SOURCE-QRY-001-BINDING-ROLLUP-GROUPING-METADATA-HAVING-V1
      // QOW-SOURCE-QRY-001-BINDING-CUBE-GROUPING-METADATA-HAVING-V1
      const bool admitted_multi_key_boolean_having =
          count_sum_and_profile && aggregate_relation_ast != nullptr &&
          (aggregate_relation_ast->aggregate_grouping_form ==
               NativeAggregateGroupingForm::kSimple ||
           aggregate_relation_ast->aggregate_grouping_form ==
               NativeAggregateGroupingForm::kGroupingSets ||
           aggregate_relation_ast->aggregate_grouping_form ==
               NativeAggregateGroupingForm::kRollup ||
           aggregate_relation_ast->aggregate_grouping_form ==
               NativeAggregateGroupingForm::kCube) &&
          (aggregate_relation_ast->aggregate_projection_form ==
               NativeAggregateProjectionForm::kKeysCountSum ||
           ((aggregate_relation_ast->aggregate_grouping_form ==
                 NativeAggregateGroupingForm::kGroupingSets ||
             aggregate_relation_ast->aggregate_grouping_form ==
                 NativeAggregateGroupingForm::kRollup ||
             aggregate_relation_ast->aggregate_grouping_form ==
                 NativeAggregateGroupingForm::kCube) &&
            aggregate_relation_ast->aggregate_projection_form ==
                NativeAggregateProjectionForm::kKeysCountSumGrouping));
      const auto metadata_not_sum_outputs_are_exact = [&] {
        if (!admitted_grouping_sets_metadata_not_count_sum_and_having &&
            !admitted_rollup_metadata_not_count_sum_and_having &&
            !admitted_cube_metadata_not_count_sum_and_having &&
            !admitted_grouping_sets_metadata_not_sum_having &&
            !admitted_rollup_metadata_not_sum_having &&
            !admitted_cube_metadata_not_sum_having) {
          return true;
        }
        constexpr std::array<std::string_view, 7> kOutputNames = {
            "key_a",      "key_b",     "row_count", "total_amount",
            "grouping_a", "grouping_b", "grouping_id"};
        for (const auto [relation_id, first_output_id] :
             {std::pair{aggregate_relation_ast->relation_id, 4U},
              std::pair{relation.relation_id, 11U}}) {
          for (std::size_t ordinal = 0; ordinal < kOutputNames.size();
               ++ordinal) {
            const auto output = std::ranges::find_if(
                context.outputs, [&](const auto& candidate) {
                  return candidate.relation_id == relation_id &&
                         candidate.ordinal == ordinal;
                });
            if (output == context.outputs.end() ||
                output->output_id != first_output_id + ordinal ||
                output->output_name_utf8 != kOutputNames[ordinal]) {
              return false;
            }
          }
        }
        return true;
      };
      const auto not_count_sum_and_outputs_are_exact = [&] {
        if (!admitted_two_key_not_not_sum_having &&
            !admitted_two_key_not_not_count_having &&
            !admitted_two_key_not_not_count_sum_and_having &&
            !admitted_two_key_not_not_count_sum_or_having &&
            !admitted_two_key_not_not_sum_count_or_having &&
            !admitted_two_key_not_count_having &&
            !admitted_two_key_not_count_sum_and_having &&
            !admitted_two_key_not_count_sum_or_having &&
            !admitted_grouping_sets_not_count_sum_and_having &&
            !admitted_rollup_not_count_sum_and_having &&
            !admitted_cube_not_count_sum_and_having) {
          return true;
        }
        constexpr std::array<std::string_view, 4> kOutputNames = {
            "key_a", "key_b", "row_count", "total_amount"};
        for (const auto [relation_id, first_output_id] :
             {std::pair{aggregate_relation_ast->relation_id, 4U},
              std::pair{relation.relation_id, 8U}}) {
          for (std::size_t ordinal = 0; ordinal < kOutputNames.size();
               ++ordinal) {
            const auto output = std::ranges::find_if(
                context.outputs, [&](const auto& candidate) {
                  return candidate.relation_id == relation_id &&
                         candidate.ordinal == ordinal;
                });
            if (output == context.outputs.end() ||
                output->output_id != first_output_id + ordinal ||
                output->output_name_utf8 != kOutputNames[ordinal] ||
                !output->visible) {
              return false;
            }
          }
        }
        return true;
      };
      if (filter_relation_ast != nullptr || aggregate_relation_ast == nullptr ||
          (!admitted_one_key_or_having && !admitted_two_key_or_having &&
           !admitted_grouping_sets_or_having &&
           !admitted_grouping_sets_metadata_or_having &&
           !admitted_rollup_or_having &&
           !admitted_rollup_metadata_or_having &&
           !admitted_cube_or_having &&
           !admitted_cube_metadata_or_having &&
           !admitted_two_key_not_not_sum_having &&
           !admitted_two_key_not_not_count_having &&
           !admitted_two_key_not_not_count_sum_and_having &&
           !admitted_two_key_not_not_count_sum_or_having &&
           !admitted_two_key_not_not_sum_count_or_having &&
           !admitted_two_key_not_sum_having &&
           !admitted_two_key_not_count_having &&
           !admitted_two_key_not_count_sum_and_having &&
           !admitted_two_key_not_count_sum_or_having &&
           !admitted_grouping_sets_not_count_sum_and_having &&
           !admitted_grouping_sets_metadata_not_count_sum_and_having &&
           !admitted_rollup_not_count_sum_and_having &&
           !admitted_rollup_metadata_not_count_sum_and_having &&
           !admitted_cube_not_count_sum_and_having &&
           !admitted_cube_metadata_not_count_sum_and_having &&
           !admitted_grouping_sets_not_sum_having &&
           !admitted_grouping_sets_metadata_not_sum_having &&
           !admitted_rollup_not_sum_having &&
           !admitted_rollup_metadata_not_sum_having &&
           !admitted_cube_not_sum_having &&
           !admitted_cube_metadata_not_sum_having &&
           !admitted_simple_having &&
           !admitted_grouping_sets_sum_having &&
           !admitted_grouping_sets_metadata_sum_having &&
           !admitted_rollup_sum_having &&
           !admitted_rollup_metadata_sum_having &&
           !admitted_cube_sum_having &&
           !admitted_cube_metadata_sum_having &&
           !admitted_multi_key_boolean_having) ||
          ((admitted_grouping_sets_metadata_not_count_sum_and_having ||
            admitted_rollup_metadata_not_count_sum_and_having ||
            admitted_cube_metadata_not_count_sum_and_having ||
            admitted_grouping_sets_metadata_not_sum_having ||
            admitted_rollup_metadata_not_sum_having ||
            admitted_cube_not_sum_having ||
            admitted_cube_metadata_not_sum_having) &&
           std::ranges::any_of(context.outputs,
                               [](const auto& output) {
                                 return !output.visible;
                               })) ||
          !metadata_not_sum_outputs_are_exact() ||
          !not_count_sum_and_outputs_are_exact() ||
          relation.relation_id != ast.root_relation_id ||
          relation.input_relation_ids !=
              std::vector<std::uint32_t>{aggregate_relation_ast->relation_id} ||
          !relation.values_row_ids.empty() ||
          relation.output_expression_ids !=
              aggregate_relation_ast->output_expression_ids ||
          relation.aggregate_grouping_form !=
              NativeAggregateGroupingForm::kNone ||
          relation.aggregate_projection_form !=
              NativeAggregateProjectionForm::kNone ||
          !relation.grouping_key_expression_ids.empty() ||
          !relation.aggregate_expression_ids.empty() || predicate == nullptr ||
          predicate_binding == expression_binding_by_id.end() ||
          (!not_count_profile && !not_not_count_profile &&
           (sum_comparison == nullptr || having_sum == nullptr ||
            sum_threshold == nullptr ||
            sum_comparison->expression_kind !=
                NativeExpressionAstKind::kBinary ||
            sum_comparison->operator_name != ">" ||
            sum_comparison->child_expression_ids.size() != 2 ||
            having_sum->expression_kind !=
                NativeExpressionAstKind::kFunctionCall ||
            ToUpperAscii(having_sum->operator_name) != "SUM" ||
            having_sum->child_expression_ids.size() != 1 ||
            having_argument == nullptr || projected_sum == nullptr ||
            projected_argument == nullptr ||
            having_argument->expression_kind !=
                NativeExpressionAstKind::kIdentifier ||
            projected_argument->expression_kind !=
                NativeExpressionAstKind::kIdentifier ||
            sum_threshold->expression_kind !=
                NativeExpressionAstKind::kLiteral ||
            sum_threshold->literal_kind != NativeLiteralAstKind::kNumeric ||
            !sum_threshold->child_expression_ids.empty() ||
            having_sum_binding == expression_binding_by_id.end() ||
            projected_sum_binding == expression_binding_by_id.end() ||
            having_argument_binding == expression_binding_by_id.end() ||
            projected_argument_binding == expression_binding_by_id.end() ||
            having_sum_binding->second->function_uuid !=
                "019de5fc-2400-72e4-8549-82b2eef5a777" ||
            projected_sum_binding->second->function_uuid !=
                having_sum_binding->second->function_uuid ||
            having_sum_binding->second->descriptor_id !=
                projected_sum_binding->second->descriptor_id ||
            having_argument_binding->second->bound_name_uuid !=
                projected_argument_binding->second->bound_name_uuid ||
            having_argument_binding->second->descriptor_id !=
                projected_argument_binding->second->descriptor_id ||
            !descriptor_is(sum_comparison, BoundNullability::kNullable) ||
            !descriptor_is(sum_threshold, BoundNullability::kNonNull))) ||
          !descriptor_is(predicate, BoundNullability::kNullable) ||
          !not_not_sum_descriptors_are_exact ||
          !not_not_count_descriptors_are_exact ||
          !not_sum_descriptors_are_exact ||
          !not_count_descriptors_are_exact ||
          !not_count_sum_boolean_descriptors_are_exact ||
          (not_sum_profile &&
           (predicate->expression_kind != NativeExpressionAstKind::kUnary ||
            predicate->operator_name != "NOT" ||
            predicate->child_expression_ids !=
                std::vector<std::uint32_t>{sum_comparison->expression_id} ||
            predicate_binding->second->descriptor_id !=
                sum_comparison_binding->second->descriptor_id)) ||
          (not_not_sum_profile &&
           (inner_not_binding == expression_binding_by_id.end() ||
            sum_comparison_binding == expression_binding_by_id.end() ||
            predicate->expression_kind != NativeExpressionAstKind::kUnary ||
            predicate->operator_name != "NOT" ||
            predicate->child_expression_ids !=
                std::vector<std::uint32_t>{inner_not->expression_id} ||
            inner_not->expression_kind != NativeExpressionAstKind::kUnary ||
            inner_not->operator_name != "NOT" ||
            inner_not->child_expression_ids !=
                std::vector<std::uint32_t>{sum_comparison->expression_id} ||
            predicate_binding->second->descriptor_id !=
                inner_not_binding->second->descriptor_id ||
            inner_not_binding->second->descriptor_id !=
                sum_comparison_binding->second->descriptor_id)) ||
          (not_not_count_profile &&
           (inner_not_binding == expression_binding_by_id.end() ||
            count_comparison_binding == expression_binding_by_id.end() ||
            having_count == nullptr || count_threshold == nullptr ||
            projected_count == nullptr ||
            predicate->expression_kind != NativeExpressionAstKind::kUnary ||
            predicate->operator_name != "NOT" ||
            predicate->child_expression_ids !=
                std::vector<std::uint32_t>{inner_not->expression_id} ||
            inner_not->expression_kind != NativeExpressionAstKind::kUnary ||
            inner_not->operator_name != "NOT" ||
            inner_not->child_expression_ids !=
                std::vector<std::uint32_t>{count_comparison->expression_id} ||
            count_comparison->expression_kind !=
                NativeExpressionAstKind::kBinary ||
            count_comparison->operator_name != ">" ||
            count_comparison->child_expression_ids !=
                std::vector<std::uint32_t>{having_count->expression_id,
                                           count_threshold->expression_id} ||
            having_count->expression_kind !=
                NativeExpressionAstKind::kFunctionCall ||
            ToUpperAscii(having_count->operator_name) != "COUNT" ||
            !having_count->child_expression_ids.empty() ||
            projected_count->expression_kind !=
                NativeExpressionAstKind::kFunctionCall ||
            ToUpperAscii(projected_count->operator_name) != "COUNT" ||
            !projected_count->child_expression_ids.empty() ||
            count_threshold->expression_kind !=
                NativeExpressionAstKind::kLiteral ||
            count_threshold->literal_kind != NativeLiteralAstKind::kNumeric ||
            !count_threshold->child_expression_ids.empty() ||
            having_count_binding == expression_binding_by_id.end() ||
            projected_count_binding == expression_binding_by_id.end() ||
            having_count_binding->second->function_uuid !=
                "019de5fc-2400-784a-9aec-371f8b95b7ea" ||
            projected_count_binding->second->function_uuid !=
                having_count_binding->second->function_uuid ||
            having_count_binding->second->descriptor_id !=
                projected_count_binding->second->descriptor_id ||
            predicate_binding->second->descriptor_id !=
                inner_not_binding->second->descriptor_id ||
            inner_not_binding->second->descriptor_id !=
                count_comparison_binding->second->descriptor_id)) ||
          (not_not_count_sum_boolean_profile &&
           (inner_not_binding == expression_binding_by_id.end() ||
            boolean_root_binding == expression_binding_by_id.end() ||
            predicate->child_expression_ids !=
                std::vector<std::uint32_t>{inner_not->expression_id} ||
            inner_not->child_expression_ids !=
                std::vector<std::uint32_t>{boolean_root->expression_id} ||
            predicate_binding->second->descriptor_id !=
                inner_not_binding->second->descriptor_id ||
            inner_not_binding->second->descriptor_id !=
                boolean_root_binding->second->descriptor_id)) ||
          (not_count_profile &&
           (count_comparison == nullptr || having_count == nullptr ||
            count_threshold == nullptr || projected_count == nullptr ||
            count_comparison->expression_kind !=
                NativeExpressionAstKind::kBinary ||
            count_comparison->operator_name != ">" ||
            count_comparison->child_expression_ids !=
                std::vector<std::uint32_t>{having_count->expression_id,
                                           count_threshold->expression_id} ||
            having_count->expression_kind !=
                NativeExpressionAstKind::kFunctionCall ||
            ToUpperAscii(having_count->operator_name) != "COUNT" ||
            !having_count->child_expression_ids.empty() ||
            projected_count->expression_kind !=
                NativeExpressionAstKind::kFunctionCall ||
            ToUpperAscii(projected_count->operator_name) != "COUNT" ||
            !projected_count->child_expression_ids.empty() ||
            count_threshold->expression_kind !=
                NativeExpressionAstKind::kLiteral ||
            count_threshold->literal_kind != NativeLiteralAstKind::kNumeric ||
            !count_threshold->child_expression_ids.empty() ||
            count_comparison_binding == expression_binding_by_id.end() ||
            having_count_binding == expression_binding_by_id.end() ||
            projected_count_binding == expression_binding_by_id.end() ||
            having_count_binding->second->function_uuid !=
                "019de5fc-2400-784a-9aec-371f8b95b7ea" ||
            projected_count_binding->second->function_uuid !=
                having_count_binding->second->function_uuid ||
            having_count_binding->second->descriptor_id !=
                projected_count_binding->second->descriptor_id ||
            predicate->child_expression_ids !=
                std::vector<std::uint32_t>{count_comparison->expression_id} ||
            predicate_binding->second->descriptor_id !=
                count_comparison_binding->second->descriptor_id)) ||
          (count_sum_boolean_profile &&
           (boolean_root == nullptr ||
            boolean_root_binding == expression_binding_by_id.end() ||
            boolean_root->expression_kind !=
                NativeExpressionAstKind::kBinary ||
            boolean_root->operator_name !=
                ((count_sum_or_profile || not_count_sum_or_profile ||
                  not_not_count_sum_or_profile ||
                  not_not_sum_count_or_profile)
                     ? "OR"
                     : "AND") ||
            boolean_root->child_expression_ids !=
                (not_not_sum_count_or_profile
                     ? std::vector<std::uint32_t>{
                           sum_comparison->expression_id,
                           count_comparison->expression_id}
                     : std::vector<std::uint32_t>{
                           count_comparison->expression_id,
                           sum_comparison->expression_id}) ||
            !descriptor_is(boolean_root, BoundNullability::kNullable) ||
            count_comparison->expression_kind !=
                NativeExpressionAstKind::kBinary ||
            count_comparison->operator_name != ">" ||
            count_comparison->child_expression_ids.size() != 2 ||
            having_count == nullptr || count_threshold == nullptr ||
            projected_count == nullptr ||
            having_count->expression_kind !=
                NativeExpressionAstKind::kFunctionCall ||
            ToUpperAscii(having_count->operator_name) != "COUNT" ||
            !having_count->child_expression_ids.empty() ||
            projected_count->expression_kind !=
                NativeExpressionAstKind::kFunctionCall ||
            ToUpperAscii(projected_count->operator_name) != "COUNT" ||
            !projected_count->child_expression_ids.empty() ||
            count_threshold->expression_kind !=
                NativeExpressionAstKind::kLiteral ||
            count_threshold->literal_kind != NativeLiteralAstKind::kNumeric ||
            !count_threshold->child_expression_ids.empty() ||
            having_count_binding == expression_binding_by_id.end() ||
            projected_count_binding == expression_binding_by_id.end() ||
            having_count_binding->second->function_uuid !=
                "019de5fc-2400-784a-9aec-371f8b95b7ea" ||
            projected_count_binding->second->function_uuid !=
                having_count_binding->second->function_uuid ||
            having_count_binding->second->descriptor_id !=
                projected_count_binding->second->descriptor_id ||
            !descriptor_is(count_comparison,
                           BoundNullability::kNullable) ||
            !descriptor_is(count_threshold, BoundNullability::kNonNull) ||
            boolean_root_binding->second->descriptor_id !=
                count_comparison_binding->second->descriptor_id ||
            boolean_root_binding->second->descriptor_id !=
                sum_comparison_binding->second->descriptor_id ||
            (!not_count_sum_boolean_profile &&
             !not_not_count_sum_boolean_profile && boolean_root != predicate) ||
            (not_count_sum_boolean_profile &&
             (predicate->expression_kind !=
                  NativeExpressionAstKind::kUnary ||
              predicate->operator_name != "NOT" ||
              predicate->child_expression_ids !=
                  std::vector<std::uint32_t>{boolean_root->expression_id} ||
              predicate_binding->second->descriptor_id !=
                  boolean_root_binding->second->descriptor_id)) ||
            (not_not_count_sum_boolean_profile &&
             inner_not_binding->second->descriptor_id !=
                 boolean_root_binding->second->descriptor_id))) ||
          semantic_binding == relation_binding_by_id.end() ||
          semantic_binding->second->semantic_variant_id !=
              expected_filter_semantic) {
        AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                              "typed HAVING filter is outside the bounded profile");
        return RefusedBoundAst(std::move(bound));
      }
      filter_relation_ast = &relation;
      record.semantic_variant_id =
          semantic_binding->second->semantic_variant_id;
      record.bound_expression_ids = relation.predicate_expression_ids;
    }
    expected_output_count += relation.output_expression_ids.size();
    bound.relations.push_back(std::move(record));
  }
  if (values_relation_ast == nullptr ||
      (aggregate_relation_ast == nullptr && ast.relations.size() != 1) ||
      (aggregate_relation_ast != nullptr &&
       ((filter_relation_ast == nullptr &&
         (ast.relations.size() != 2 ||
          ast.root_relation_id != aggregate_relation_ast->relation_id)) ||
        (filter_relation_ast != nullptr &&
         (ast.relations.size() != 3 ||
          ast.root_relation_id != filter_relation_ast->relation_id ||
          filter_relation_ast->input_relation_ids.front() !=
              aggregate_relation_ast->relation_id)) ||
        aggregate_relation_ast->input_relation_ids.front() !=
            values_relation_ast->relation_id)) ||
      relation_binding_by_id.size() !=
          (aggregate_relation_ast == nullptr
               ? 0
               : (filter_relation_ast == nullptr ? 1 : 2))) {
    AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                          "native VALUES/aggregate graph is not canonical");
    return RefusedBoundAst(std::move(bound));
  }

  if (context.outputs.size() != expected_output_count) {
    AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-OUTPUT",
                          "output bindings must cover every relation output");
    return RefusedBoundAst(std::move(bound));
  }
  std::unordered_set<std::uint32_t> output_ids;
  std::unordered_map<std::uint32_t, std::unordered_set<std::uint32_t>>
      output_ordinals_by_relation;
  bound.outputs.reserve(context.outputs.size());
  for (const auto& output : context.outputs) {
    const auto relation_id =
        output.relation_id == 0 && ast.relations.size() == 1
            ? ast.root_relation_id
            : output.relation_id;
    const auto relation = ast_relation_by_id.find(relation_id);
    if (relation == ast_relation_by_id.end() || output.output_id == 0 ||
        output.ordinal >= relation->second->output_expression_ids.size() ||
        relation->second->output_expression_ids[output.ordinal] !=
            output.expression_id ||
        !output_ids.insert(output.output_id).second ||
        !output_ordinals_by_relation[relation_id]
             .insert(output.ordinal)
             .second) {
      AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-OUTPUT",
                            "output relation, ID, ordinal, and expression must be exact");
      return RefusedBoundAst(std::move(bound));
    }
    const auto expression_binding =
        expression_binding_by_id.find(output.expression_id);
    if (expression_binding == expression_binding_by_id.end() ||
        expression_binding->second->descriptor_id != output.descriptor_id) {
      AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-OUTPUT",
                            "output descriptor does not match its bound expression");
      return RefusedBoundAst(std::move(bound));
    }
    BoundOutputAstRecord record;
    record.output_id = output.output_id;
    record.relation_id = relation_id;
    record.expression_id = output.expression_id;
    record.output_name_utf8 = output.output_name_utf8;
    record.descriptor_id = output.descriptor_id;
    record.visible = output.visible;
    record.ordinal = output.ordinal;
    used_descriptor_ids.insert(record.descriptor_id);
    bound.outputs.push_back(std::move(record));
  }
  std::ranges::sort(bound.outputs, [](const auto& left, const auto& right) {
    return left.relation_id != right.relation_id
               ? left.relation_id < right.relation_id
               : left.ordinal < right.ordinal;
  });

  if (aggregate_relation_ast == nullptr) {
    if (!ast.grouping_sets.empty()) {
      AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                            "VALUES leaf cannot own grouping sets");
      return RefusedBoundAst(std::move(bound));
    }
  } else if (aggregate_relation_ast->aggregate_grouping_form ==
             NativeAggregateGroupingForm::kGroupingSets) {
    // QOW-SOURCE-QRY-001-BINDING-GROUPING-SETS-V1
    if (ast.grouping_sets.empty()) {
      AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                            "GROUPING SETS aggregate requires typed set records");
      return RefusedBoundAst(std::move(bound));
    }
    bound.grouping_sets.reserve(ast.grouping_sets.size());
    for (std::size_t ordinal = 0; ordinal < ast.grouping_sets.size(); ++ordinal) {
      const auto& grouping_set = ast.grouping_sets[ordinal];
      if (grouping_set.relation_id != aggregate_relation_ast->relation_id ||
          grouping_set.ordinal != ordinal) {
        AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                              "grouping-set relation and ordinals must be exact");
        return RefusedBoundAst(std::move(bound));
      }
      std::vector<std::pair<std::size_t, std::uint32_t>> canonical_members;
      std::unordered_set<std::uint32_t> members;
      for (const auto expression_id : grouping_set.expression_ids) {
        const auto key = std::ranges::find(
            aggregate_relation_ast->grouping_key_expression_ids,
            expression_id);
        if (key == aggregate_relation_ast->grouping_key_expression_ids.end() ||
            !members.insert(expression_id).second) {
          AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                                "grouping-set member must be a unique bound key");
          return RefusedBoundAst(std::move(bound));
        }
        canonical_members.emplace_back(
            static_cast<std::size_t>(std::distance(
                aggregate_relation_ast->grouping_key_expression_ids.begin(),
                key)),
            expression_id);
      }
      std::ranges::sort(canonical_members);
      BoundGroupingSetAstRecord record;
      record.relation_id = grouping_set.relation_id;
      record.ordinal = grouping_set.ordinal;
      for (const auto& [key_ordinal, expression_id] : canonical_members) {
        (void)key_ordinal;
        record.expression_ids.push_back(expression_id);
      }
      bound.grouping_sets.push_back(std::move(record));
    }
  } else if (aggregate_relation_ast->aggregate_grouping_form ==
                 NativeAggregateGroupingForm::kSimple ||
             aggregate_relation_ast->aggregate_grouping_form ==
                 NativeAggregateGroupingForm::kRollup ||
             aggregate_relation_ast->aggregate_grouping_form ==
                 NativeAggregateGroupingForm::kCube) {
    // QOW-SOURCE-QRY-001-BINDING-FIXED-GROUPING-FORM-V1
    if (!ast.grouping_sets.empty()) {
      AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                            "fixed ordinary/ROLLUP/CUBE aggregate cannot own arbitrary grouping-set records");
      return RefusedBoundAst(std::move(bound));
    }
  } else {
    AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                          "typed aggregate grouping form is not supported");
    return RefusedBoundAst(std::move(bound));
  }

  if (used_descriptor_ids.size() != descriptor_by_id.size()) {
    AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-DESCRIPTOR",
                          "binding context contains an unused descriptor handle");
    return RefusedBoundAst(std::move(bound));
  }
  bound.descriptors.reserve(context.descriptors.size());
  for (const auto& descriptor : context.descriptors) {
    BoundDescriptorAstRecord record;
    record.descriptor_id = descriptor.descriptor_id;
    record.descriptor_uuid = descriptor.descriptor_uuid;
    record.type_uuid = descriptor.type_uuid;
    record.nullability = descriptor.nullability;
    record.collation_uuid = descriptor.collation_uuid;
    record.timezone_profile_id = descriptor.timezone_profile_id;
    record.width_precision_scale = descriptor.width_precision_scale;
    bound.descriptors.push_back(std::move(record));
  }
  std::ranges::sort(bound.descriptors,
                    [](const auto& left, const auto& right) {
                      return left.descriptor_id < right.descriptor_id;
                    });

  BoundScopeAstRecord scope;
  scope.scope_id = 1;
  scope.parent_scope_id = std::nullopt;
  scope.visible_relation_ids = {ast.root_relation_id};
  for (const auto& output : bound.outputs) {
    if (output.relation_id == ast.root_relation_id && output.visible) {
      scope.visible_projection_ids.push_back(output.output_id);
    }
  }
  std::ranges::sort(scope.visible_projection_ids);
  scope.catalog_epoch_uuid = context.catalog_epoch_uuid;
  bound.scopes.push_back(std::move(scope));

  bound.bound_ast_uuid = context.bound_ast_uuid;
  bound.security_context_uuid = context.security_context_uuid;
  bound.root_relation_id = ast.root_relation_id;
  bound.root_scope_id = 1;
  bound.bound = true;
  return bound;
}

BoundStatement BindAst(const AstDocument& ast,
                       const CstDocument& cst,
                       const ParserConfig& config,
                       const SessionContext& session,
                       const std::vector<std::string>& resolved_object_uuids,
                       const NativeRelationalBindingContext* native_binding_context) {
  BoundStatement bound;
  bound.parser_api_major = config.parser_api_major;
  bound.protocol_version = config.protocol_version;
  bound.parser_package_uuid = config.parser_uuid;
  bound.parser_package_version = config.bundle_contract_id;
  bound.parser_build_id = config.build_id;
  bound.command_registry_snapshot_uuid = "sbsql-generated-registry.v1";
  bound.session_uuid = session.session_uuid;
  bound.connection_uuid = session.connection_uuid;
  bound.database_uuid = session.database_uuid;
  bound.dialect_profile_uuid = session.dialect_profile_uuid;
  bound.catalog_epoch = session.catalog_epoch;
  bound.security_policy_epoch = session.security_policy_epoch;
  bound.descriptor_epoch = session.descriptor_epoch;
  bound.transaction_context = session.transaction_context;
  bound.registry_family = ast.registry_family;
  bound.operation_family = ast.operation_family;
  bound.statement_hash = Fnv1a64(cst.source);
  bound.native_relational_recognized = ast.native_relational.recognized();
  bound.messages = ast.messages;
  PopulateAuthorityMetadata(&bound, ast);
  if (bound.messages.has_errors()) return bound;
  if (!session.authenticated && ast.family != StatementFamily::kUnknown) {
    bound.messages.diagnostics.push_back(MakeDiagnostic(
        "SBSQL.AUTH.REQUIRED", "ERROR", "statement binding requires an authenticated server session",
        "sbp_sbsql.binder"));
    return bound;
  }
  if (bound.requires_cluster_profile) {
    bound.messages.diagnostics.push_back(MakeDiagnostic(
        "SBSQL.CLUSTER.AUTHORITY_REQUIRED", "ERROR",
        "cluster-private statement binding requires a cluster profile authority context",
        "sbp_sbsql.binder",
        {{"statement_surface_id", bound.statement_surface_id},
         {"authority", "authority.cluster.profile_gate_required"}}));
    return bound;
  }
  if (bound.exact_refusal_required && bound.behavior_descriptor_key.find("fail_closed") != std::string::npos) {
    bound.messages.diagnostics.push_back(MakeDiagnostic(
        "SBSQL.STATEMENT.EXACT_REFUSAL_REQUIRED", "ERROR",
        "statement binding requires exact refusal before SBLR lowering",
        "sbp_sbsql.binder",
        {{"statement_surface_id", bound.statement_surface_id},
         {"diagnostic_key", bound.diagnostic_key}}));
    return bound;
  }
  if (ast.native_relational.recognized()) {
    if (native_binding_context == nullptr || session.catalog_epoch == 0 ||
        session.descriptor_epoch == 0) {
      bound.messages.diagnostics.push_back(MakeDiagnostic(
          "QOW-DIAG-BOUNDAST-SCOPE", "ERROR",
          "typed relational binding requires engine-supplied descriptor and epoch context",
          "sbp_sbsql.native_binder"));
      return bound;
    }
    bound.native_relational =
        BindNativeRelationalAst(ast.native_relational, *native_binding_context);
    bound.messages.diagnostics.insert(
        bound.messages.diagnostics.end(),
        bound.native_relational.messages.diagnostics.begin(),
        bound.native_relational.messages.diagnostics.end());
    if (bound.messages.has_errors()) return bound;
    bound.descriptor_refs.clear();
    for (const auto& descriptor : bound.native_relational.descriptors) {
      bound.descriptor_refs.push_back(descriptor.descriptor_uuid);
    }
    bound.resolved_object_uuids.clear();
    for (const auto& source :
         bound.native_relational.catalog_relation_sources) {
      if (!source.object_uuid.empty()) {
        bound.resolved_object_uuids.push_back(source.object_uuid);
      }
    }
    bound.bound = bound.native_relational.bound;
    return bound;
  }
  if (ast.requires_name_resolution) {
    if (!resolved_object_uuids.empty()) {
      bound.resolved_object_uuids = resolved_object_uuids;
      bound.bound = true;
      return bound;
    }
    if (IsSourceFreeCteRoute(cst) || IsSourceFreeCatalogProjectionCountRoute(cst)) {
      bound.bound = true;
      return bound;
    }
    if (config.server_endpoint.empty()) {
      bound.messages.diagnostics.push_back(MakeDiagnostic(
          "SBSQL.NAME_RESOLUTION.SERVER_ENDPOINT_REQUIRED", "ERROR",
          "object-name binding requires ResolveNameRegistryPublic through sb_server IPC",
          "sbp_sbsql.binder"));
      return bound;
    }
    bound.messages.diagnostics.push_back(MakeDiagnostic(
        "SBSQL.NAME_RESOLUTION.PUBLIC_RESOLVER_REQUIRED", "ERROR",
        "public name resolution must be performed by sb_server before this statement can lower to final SBLR",
        "sbp_sbsql.binder"));
    return bound;
  }
  bound.bound = true;
  return bound;
}

} // namespace scratchbird::parser::sbsql
