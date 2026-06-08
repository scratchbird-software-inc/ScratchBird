// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "metadata/function_parser_projection.hpp"

#include <array>
#include <string>
#include <utility>

namespace scratchbird::engine::functions {
namespace {

struct AliasDef {
  const char* alias_name;
  const char* canonical_function_id;
  FunctionAliasSource source;
  const char* source_package;
};

constexpr auto kAliasDefs = std::to_array<AliasDef>({
    {"SBSQL.SUBSTRING", "data.scalar.substring", FunctionAliasSource::sb_native, "sbsql"},
    {"SBSQL.LOWER", "data.scalar.lower", FunctionAliasSource::sb_native, "sbsql"},
    {"SBSQL.UPPER", "data.scalar.upper", FunctionAliasSource::sb_native, "sbsql"},
    {"SBSQL.LENGTH", "data.scalar.length", FunctionAliasSource::sb_native, "sbsql"},
    {"POSTGRES.substring", "data.scalar.substring", FunctionAliasSource::donor, "postgresql"},
    {"POSTGRES.lower", "data.scalar.lower", FunctionAliasSource::donor, "postgresql"},
    {"POSTGRES.upper", "data.scalar.upper", FunctionAliasSource::donor, "postgresql"},
    {"POSTGRES.length", "data.scalar.length", FunctionAliasSource::donor, "postgresql"},
    {"POSTGRES.nextval", "data.sequence.next", FunctionAliasSource::donor, "postgresql"},
    {"POSTGRES.currval", "data.sequence.current", FunctionAliasSource::donor, "postgresql"},
    {"MYSQL.LCASE", "data.scalar.lower", FunctionAliasSource::donor, "mysql"},
    {"MYSQL.UCASE", "data.scalar.upper", FunctionAliasSource::donor, "mysql"},
    {"MYSQL.JSON_EXTRACT", "nosql.document.get", FunctionAliasSource::donor, "mysql"},
    {"MYSQL.JSON_SET", "nosql.document.put", FunctionAliasSource::donor, "mysql"},
    {"MYSQL.JSON_REMOVE", "nosql.document.delete", FunctionAliasSource::donor, "mysql"},
    {"MYSQL.GROUP_CONCAT", "data.aggregate.string_agg", FunctionAliasSource::donor, "mysql"},
    {"FIREBIRD.GEN_ID", "data.sequence.next", FunctionAliasSource::donor, "firebird"},
    {"FIREBIRD.NEXT_VALUE_FOR", "data.sequence.next", FunctionAliasSource::donor, "firebird"},
    {"SQLITE.json_extract", "nosql.document.get", FunctionAliasSource::donor, "sqlite"},
    {"SQLITE.last_insert_rowid", "data.identity.current", FunctionAliasSource::donor, "sqlite"},
    {"REDIS.GET", "nosql.kv.get", FunctionAliasSource::donor, "redis"},
    {"REDIS.SET", "nosql.kv.put", FunctionAliasSource::donor, "redis"},
    {"REDIS.DEL", "nosql.kv.delete", FunctionAliasSource::donor, "redis"},
    {"NEO4J.shortestPath", "nosql.graph.path", FunctionAliasSource::donor, "neo4j"},
    {"OPENSEARCH.match", "search.query", FunctionAliasSource::donor, "opensearch"},
    {"POSTGIS.ST_Distance", "spatial.distance", FunctionAliasSource::plugin_extension, "postgis"},
    {"POSTGIS.ST_Contains", "spatial.contains", FunctionAliasSource::plugin_extension, "postgis"},
    {"PGVECTOR.vector_l2_distance", "vector.distance", FunctionAliasSource::plugin_extension, "pgvector"},
    {"PG_TRGM.similarity", "sb.scalar.similarity", FunctionAliasSource::plugin_extension, "pg_trgm"},
    {"PG_TRGM.word_similarity", "sb.scalar.word_similarity", FunctionAliasSource::plugin_extension, "pg_trgm"},
    {"TIMESCALEDB.time_bucket", "timeseries.bucket", FunctionAliasSource::plugin_extension, "timescaledb"},
});

bool IsUnavailableState(FunctionImplementationState state) {
  return state == FunctionImplementationState::future_gated_package ||
         state == FunctionImplementationState::refuse_until_classified ||
         state == FunctionImplementationState::unsupported;
}

std::string ProjectionStateFor(const FunctionRegistryEntry* entry) {
  if (entry == nullptr) return "unresolved";
  if (IsUnavailableState(entry->implementation_state)) return "disabled";
  if (entry->implementation_state == FunctionImplementationState::implemented_policy_security_or_dependency_runtime_refusal ||
      entry->implementation_state == FunctionImplementationState::policy_blocked ||
      entry->implementation_state == FunctionImplementationState::optional_package_dependency_gated ||
      entry->implementation_state == FunctionImplementationState::udr_only) {
    return "runtime_refusal";
  }
  return "available";
}

FunctionParserProjectionRow BuildRow(const FunctionRegistry& registry,
                                     const FunctionParserProjectionRequest& request,
                                     const AliasDef& alias) {
  const auto* entry = registry.Lookup(alias.canonical_function_id);
  FunctionParserProjectionRow row;
  row.parser_profile = request.parser_profile.empty() ? "generic" : request.parser_profile;
  row.alias_name = alias.alias_name;
  row.canonical_function_id = alias.canonical_function_id;
  row.alias_source = alias.source;
  row.source_package = alias.source_package;
  row.projection_state = ProjectionStateFor(entry);
  row.parser_may_submit_sblr = entry != nullptr;
  row.parser_has_authority = false;
  row.metadata_redacted = !request.metadata_visible;
  row.function_uuid = request.metadata_visible && entry != nullptr ? entry->function_uuid : "";
  row.result_descriptor_rule = request.metadata_visible && entry != nullptr
                                   ? entry->optimizer_metadata.descriptor_rule
                                   : "redacted";
  row.diagnostic_rendering_hint = "parser renders donor/client diagnostic text from canonical engine diagnostic";
  row.refusal_policy = entry == nullptr ? "canonical_function_not_registered" :
                       row.projection_state == "disabled" ? "disabled_or_unimplemented" :
                       row.projection_state == "runtime_refusal" ? "policy_security_or_dependency_runtime_refusal" :
                       "execute_via_engine_gate";
  return row;
}

}  // namespace

std::vector<FunctionParserProjectionRow> BuildFunctionParserProjection(
    const FunctionRegistry& registry,
    const FunctionParserProjectionRequest& request) {
  std::vector<FunctionParserProjectionRow> rows;
  rows.reserve(kAliasDefs.size());
  for (const auto& alias : kAliasDefs) {
    auto row = BuildRow(registry, request, alias);
    if (!request.include_disabled && (row.projection_state == "disabled" || row.projection_state == "unresolved")) {
      continue;
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

FunctionParserAuthorityDecision ValidateFunctionParserProjectionAuthority(
    const FunctionParserProjectionRequest& request) {
  if (request.parser_claims_execution_authority) {
    return FunctionParserAuthorityDecision{false,
                                           "SB_DIAG_PARSER_EXECUTION_AUTHORITY_DENIED",
                                           "parser may submit SBLR but cannot claim function execution authority"};
  }
  if (request.parser_claims_security_authority) {
    return FunctionParserAuthorityDecision{false,
                                           "SB_DIAG_PARSER_SECURITY_AUTHORITY_DENIED",
                                           "parser cannot claim security or metadata redaction authority"};
  }
  return FunctionParserAuthorityDecision{true, "SB_DIAG_OK", "parser projection request is metadata-only"};
}

const char* ToString(FunctionAliasSource source) {
  switch (source) {
    case FunctionAliasSource::sb_native: return "sb_native";
    case FunctionAliasSource::donor: return "donor";
    case FunctionAliasSource::plugin_extension: return "plugin_extension";
  }
  return "unknown";
}

}  // namespace scratchbird::engine::functions
