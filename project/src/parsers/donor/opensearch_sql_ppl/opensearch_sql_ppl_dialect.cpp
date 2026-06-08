// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "opensearch_sql_ppl_dialect.hpp"

#include <array>

namespace scratchbird::parser::opensearch_sql_ppl {
namespace {

using scratchbird::parser::donor::MappingDisposition;
using scratchbird::parser::donor::OperationPattern;
using scratchbird::parser::donor::PatternMatch;
using scratchbird::parser::donor::SurfaceDescriptor;

constexpr std::string_view kSblrFamily = "sblr.donor.opensearch_sql_ppl.profile.v1";

constexpr OperationPattern kPatterns[] = {
    {"GET /_CLUSTER", PatternMatch::kPrefix, "cluster_control", "opensearch_sql_ppl.cluster.get",
     MappingDisposition::kAdmittedSblr, "cluster.admin.inspect_status",
     "sblr.cluster.report.v3:cluster.admin.inspect_status", "cluster.inspect_state", "",
     "", true, false},
    {"POST /_CLUSTER", PatternMatch::kPrefix, "cluster_control", "opensearch_sql_ppl.cluster.post",
     MappingDisposition::kAdmittedSblr, "cluster.job.start_controlled",
     "sblr.cluster.control.v3:cluster.job.start_controlled", "cluster.start_job", "",
     "", true, false},
    {"GET /_PLUGINS/_SQL||GET /_PLUGINS/_PPL", PatternMatch::kRestMethodRoute, "rest_plugin_api", "opensearch_sql_ppl.rest.get",
     MappingDisposition::kParserSupportUdr, "opensearch_sql_ppl.udr.rest.get",
     "SBLR_DONOR_OPENSEARCH_SQL_PPL_REST_DSL_ROUTE", "ParserSupportRestDslRoute",
     "OPENSEARCH_SQL_PPL.EMULATION.REST_DSL_ROUTE",
     "OpenSearch SQL/PPL plugin GET requests route through trusted parser support.", true, true},
    {"POST /_PLUGINS/_SQL||POST /_PLUGINS/_PPL||POST /_OPENDISTRO/_SQL", PatternMatch::kRestMethodRoute, "rest_plugin_api", "opensearch_sql_ppl.rest.post",
     MappingDisposition::kParserSupportUdr, "opensearch_sql_ppl.udr.rest.post",
     "SBLR_DONOR_OPENSEARCH_SQL_PPL_REST_DSL_ROUTE", "ParserSupportRestDslRoute",
     "OPENSEARCH_SQL_PPL.EMULATION.REST_DSL_ROUTE",
     "OpenSearch SQL/PPL plugin POST requests route through trusted parser support.", true, true},
    {"PUT /_PLUGINS/_QUERY", PatternMatch::kRestMethodRoute, "rest_plugin_api", "opensearch_sql_ppl.rest.put",
     MappingDisposition::kParserSupportUdr, "opensearch_sql_ppl.udr.rest.put",
     "SBLR_DONOR_OPENSEARCH_SQL_PPL_REST_DSL_ROUTE", "ParserSupportRestDslRoute",
     "OPENSEARCH_SQL_PPL.EMULATION.REST_DSL_ROUTE",
     "OpenSearch SQL/PPL query-settings or datasource PUT requests route through trusted parser support.", true, true},
    {"DELETE /_PLUGINS/_QUERY", PatternMatch::kRestMethodRoute, "rest_plugin_api", "opensearch_sql_ppl.rest.delete",
     MappingDisposition::kParserSupportUdr, "opensearch_sql_ppl.udr.rest.delete",
     "SBLR_DONOR_OPENSEARCH_SQL_PPL_REST_DSL_ROUTE", "ParserSupportRestDslRoute",
     "OPENSEARCH_SQL_PPL.EMULATION.REST_DSL_ROUTE",
     "OpenSearch SQL/PPL datasource DELETE requests route through trusted parser support.", true, true},
    {"DROP INDEX", PatternMatch::kPrefix, "index_admin", "opensearch_sql_ppl.index.drop",
     MappingDisposition::kParserSupportUdr, "opensearch_sql_ppl.udr.index.drop",
     "SBLR_DONOR_OPENSEARCH_SQL_PPL_INDEX_ROUTE", "ParserSupportIndexRoute",
     "OPENSEARCH_SQL_PPL.EMULATION.INDEX_ROUTE",
     "Index administration routes through trusted catalog/index policy.", true, true},
    {"CREATE INDEX", PatternMatch::kPrefix, "index_admin", "opensearch_sql_ppl.index.create",
     MappingDisposition::kParserSupportUdr, "opensearch_sql_ppl.udr.index.create",
     "SBLR_DONOR_OPENSEARCH_SQL_PPL_INDEX_ROUTE", "ParserSupportIndexRoute", "OPENSEARCH_SQL_PPL.EMULATION.INDEX_ROUTE",
     "Index options route through trusted catalog/index policy.", true, true},
    {"CALL", PatternMatch::kPrefix, "plugin", "opensearch_sql_ppl.plugin.call",
     MappingDisposition::kParserSupportUdr, "opensearch_sql_ppl.udr.plugin.call",
     "SBLR_DONOR_OPENSEARCH_SQL_PPL_PLUGIN_ROUTE", "ParserSupportPluginRoute", "OPENSEARCH_SQL_PPL.EMULATION.PLUGIN_ROUTE",
     "Plugin procedures route through trusted package policy.", true, false},
    {"ML ", PatternMatch::kPplPipelineStage, "ml", "opensearch_sql_ppl.ml.command",
     MappingDisposition::kParserSupportUdr, "opensearch_sql_ppl.udr.ml",
     "SBLR_DONOR_OPENSEARCH_SQL_PPL_ML_ROUTE", "ParserSupportMlRoute", "OPENSEARCH_SQL_PPL.EMULATION.ML_ROUTE",
     "ML commands route through trusted plugin policy.", true, false},
    {"AD ", PatternMatch::kPplPipelineStage, "ad", "opensearch_sql_ppl.ad.command",
     MappingDisposition::kParserSupportUdr, "opensearch_sql_ppl.udr.ad",
     "SBLR_DONOR_OPENSEARCH_SQL_PPL_ML_ROUTE", "ParserSupportMlRoute", "OPENSEARCH_SQL_PPL.EMULATION.ML_ROUTE",
     "Anomaly-detection commands route through trusted plugin policy.", true, false},
    {"LOOKUP", PatternMatch::kPplPipelineStage, "ppl_pipeline", "opensearch_sql_ppl.ppl.lookup",
     MappingDisposition::kParserSupportUdr, "opensearch_sql_ppl.udr.lookup",
     "SBLR_DONOR_OPENSEARCH_SQL_PPL_PPL_PIPELINE", "ParserSupportPplPipelineRoute", "OPENSEARCH_SQL_PPL.EMULATION.PPL_PIPELINE_ROUTE",
     "Lookup pipelines route through trusted connector policy.", true, false},
    {"JOIN", PatternMatch::kPplPipelineStage, "ppl_pipeline", "opensearch_sql_ppl.ppl.join",
     MappingDisposition::kParserSupportUdr, "opensearch_sql_ppl.udr.join",
     "SBLR_DONOR_OPENSEARCH_SQL_PPL_PPL_PIPELINE", "ParserSupportPplPipelineRoute", "OPENSEARCH_SQL_PPL.EMULATION.PPL_PIPELINE_ROUTE",
     "Join pipelines route through trusted planner policy.", true, false},
    {"STATS", PatternMatch::kPplPipelineStage, "ppl_pipeline", "opensearch_sql_ppl.ppl.stats",
     MappingDisposition::kAdmittedSblr, "opensearch_sql_ppl.ppl.stats",
     "SBLR_DONOR_OPENSEARCH_SQL_PPL_PPL_STATS", "EngineAggregatePlan", "",
     "", false, false},
    {"SOURCE", PatternMatch::kPplPipelineStage, "ppl_query", "opensearch_sql_ppl.ppl.source",
     MappingDisposition::kAdmittedSblr, "opensearch_sql_ppl.ppl.source",
     "SBLR_DONOR_OPENSEARCH_SQL_PPL_PPL_SOURCE", "EngineQuerySelect", "",
     "", false, false},
    {"SEARCH", PatternMatch::kPrefix, "ppl_query", "opensearch_sql_ppl.ppl.search",
     MappingDisposition::kAdmittedSblr, "opensearch_sql_ppl.ppl.search",
     "SBLR_DONOR_OPENSEARCH_SQL_PPL_PPL_SEARCH", "EngineQuerySelect", "",
     "", false, false},
    {"WHERE", PatternMatch::kPrefix, "ppl_pipeline", "opensearch_sql_ppl.ppl.where",
     MappingDisposition::kAdmittedSblr, "opensearch_sql_ppl.ppl.where",
     "SBLR_DONOR_OPENSEARCH_SQL_PPL_PPL_FILTER", "EngineQueryFilter", "",
     "", false, false},
    {"FIELDS", PatternMatch::kPrefix, "ppl_pipeline", "opensearch_sql_ppl.ppl.fields",
     MappingDisposition::kAdmittedSblr, "opensearch_sql_ppl.ppl.fields",
     "SBLR_DONOR_OPENSEARCH_SQL_PPL_PPL_PROJECT", "EngineQueryProject", "",
     "", false, false},
    {"SORT", PatternMatch::kPrefix, "ppl_pipeline", "opensearch_sql_ppl.ppl.sort",
     MappingDisposition::kAdmittedSblr, "opensearch_sql_ppl.ppl.sort",
     "SBLR_DONOR_OPENSEARCH_SQL_PPL_PPL_SORT", "EngineQuerySort", "",
     "", false, false},
    {"HEAD", PatternMatch::kPrefix, "ppl_pipeline", "opensearch_sql_ppl.ppl.head",
     MappingDisposition::kAdmittedSblr, "opensearch_sql_ppl.ppl.head",
     "SBLR_DONOR_OPENSEARCH_SQL_PPL_PPL_LIMIT", "EngineQueryLimit", "",
     "", false, false},
    {"EVAL", PatternMatch::kPrefix, "ppl_pipeline", "opensearch_sql_ppl.ppl.eval",
     MappingDisposition::kAdmittedSblr, "opensearch_sql_ppl.ppl.eval",
     "SBLR_DONOR_OPENSEARCH_SQL_PPL_PPL_EVAL", "EngineExpressionProject", "",
     "", false, false},
    {"SHOW DATASOURCES", PatternMatch::kPrefix, "catalog_overlay", "opensearch_sql_ppl.catalog.show_datasources",
     MappingDisposition::kCatalogProjection, "opensearch_sql_ppl.catalog.show_datasources",
     "SBLR_DONOR_OPENSEARCH_SQL_PPL_CATALOG_PROJECT", "EngineCatalogProjection", "",
     "", false, false},
    {"SHOW", PatternMatch::kPrefix, "catalog_overlay", "opensearch_sql_ppl.catalog.show",
     MappingDisposition::kCatalogProjection, "opensearch_sql_ppl.catalog.show",
     "SBLR_DONOR_OPENSEARCH_SQL_PPL_CATALOG_PROJECT", "EngineCatalogProjection", "",
     "", false, false},
    {"DESCRIBE", PatternMatch::kPrefix, "catalog_overlay", "opensearch_sql_ppl.catalog.describe",
     MappingDisposition::kCatalogProjection, "opensearch_sql_ppl.catalog.describe",
     "SBLR_DONOR_OPENSEARCH_SQL_PPL_CATALOG_PROJECT", "EngineCatalogProjection", "",
     "", false, false},
    {"EXPLAIN", PatternMatch::kPrefix, "optimizer", "opensearch_sql_ppl.optimizer.explain",
     MappingDisposition::kCatalogProjection, "opensearch_sql_ppl.optimizer.explain",
     "SBLR_DONOR_OPENSEARCH_SQL_PPL_EXPLAIN", "EngineExplainPlan", "",
     "", false, false},
    {"SELECT", PatternMatch::kPrefix, "sql_query", "opensearch_sql_ppl.sql.select",
     MappingDisposition::kAdmittedSblr, "opensearch_sql_ppl.sql.select",
     "SBLR_DONOR_OPENSEARCH_SQL_PPL_SQL_SELECT", "EngineQuerySelect", "",
     "", false, false},
    {"DELETE", PatternMatch::kPrefix, "sql_dml", "opensearch_sql_ppl.sql.delete",
     MappingDisposition::kAdmittedSblr, "opensearch_sql_ppl.sql.delete",
     "SBLR_DONOR_OPENSEARCH_SQL_PPL_SQL_DELETE", "EngineDmlDelete", "",
     "", false, true},
};

const std::array<SurfaceDescriptor, 10> kDatatypeSurfaces{{
    {"boolean", "BOOLEAN", "descriptor"},
    {"numeric", "BYTE;SHORT;INTEGER;LONG;FLOAT;DOUBLE;HALF_FLOAT;SCALED_FLOAT", "descriptor"},
    {"keyword_text", "KEYWORD;TEXT", "descriptor"},
    {"temporal", "DATE;TIME;TIMESTAMP", "descriptor"},
    {"ip", "IP", "descriptor"},
    {"geo", "GEO_POINT;GEO_SHAPE", "parser_support_udr"},
    {"object", "OBJECT;NESTED", "descriptor"},
    {"array", "MULTI_VALUE_FIELD", "descriptor"},
    {"rank", "RANK_FEATURE;RANK_FEATURES", "parser_support_udr"},
    {"knn_vector", "KNN_VECTOR", "parser_support_udr"},
}};

const std::array<SurfaceDescriptor, 11> kBuiltinSurfaces{{
    {"aggregate", "COUNT;AVG;SUM;MIN;MAX;PERCENTILE", "sblr"},
    {"ppl", "SOURCE;WHERE;FIELDS;STATS;DEDUP;SORT;HEAD;EVAL", "sblr"},
    {"ppl_join", "JOIN;LOOKUP;APPENDCOL", "parser_support_udr"},
    {"sql", "SELECT;DELETE;SHOW;DESCRIBE", "sblr"},
    {"string", "SUBSTRING;LOWER;UPPER;TRIM", "sblr"},
    {"math", "ABS;ROUND;CEIL;FLOOR", "sblr"},
    {"date", "DATE_FORMAT;DATE_ADD;DATE_DIFF", "sblr"},
    {"json", "JSON_EXTRACT;JSON_VALUE", "parser_support_udr"},
    {"rest_dsl", "GET;POST;PUT;DELETE non-cluster REST DSL", "parser_support_udr"},
    {"ml", "ML;AD;KMEANS", "parser_support_udr"},
    {"security", "DATASOURCES;CONNECTORS", "catalog_projection"},
}};

const std::array<SurfaceDescriptor, 9> kCatalogSurfaces{{
    {"datasources", "SHOW DATASOURCES", "catalog_projection"},
    {"indexes", "SHOW TABLES;DESCRIBE TABLE", "catalog_projection"},
    {"mappings", "INDEX MAPPINGS", "catalog_projection"},
    {"plugins", "SQL PLUGIN;PPL PLUGIN", "parser_support_udr"},
    {"fields", "DESCRIBE;FIELD CAPS", "catalog_projection"},
    {"stats", "CAT INDICES;CLUSTER STATS", "catalog_projection"},
    {"roles", "SECURITY ROLES", "catalog_projection"},
    {"pipelines", "SEARCH PIPELINES", "catalog_projection"},
    {"models", "ML COMMONS MODELS", "parser_support_udr"},
}};

const std::array<SurfaceDescriptor, 10> kDiagnosticSurfaces{{
    {"parse", "OPENSEARCH_SQL_PPL.PARSE.INVALID_INPUT;OPENSEARCH_SQL_PPL.PARSE.UNSUPPORTED_SURFACE", "parser"},
    {"policy", "OPENSEARCH_SQL_PPL.AUTHORITY.*", "fail_closed"},
    {"udr", "OPENSEARCH_SQL_PPL.EMULATION.*", "parser_support_udr"},
    {"catalog", "OPENSEARCH_SQL_PPL.CATALOG_OVERLAY.READ_ONLY", "catalog_projection"},
    {"session", "OPENSEARCH_SQL_PPL.SESSION.*", "sblr"},
    {"transaction", "OPENSEARCH_SQL_PPL.TRANSACTION.*", "sblr"},
    {"file_effects", "real_donor_file_effects=false", "authority_invariant"},
    {"donor_execution", "donor_engine_sql_executed=false", "authority_invariant"},
    {"mga", "parser_transaction_finality_authority=false", "authority_invariant"},
    {"support_bundle", "source_text_redacted=true", "diagnostic_redaction"},
}};

const scratchbird::parser::donor::DialectProfile kProfile{
    "opensearch_sql_ppl",
    "OpenSearch SQL/PPL",
    "sbp_opensearch_sql_ppl",
    "sbup_opensearch_sql_ppl",
    "3.6.0-sql-ppl",
    "OPENSEARCH_SQL_PPL",
    kSblrFamily,
    kPatterns,
    kDatatypeSurfaces,
    kBuiltinSurfaces,
    kCatalogSurfaces,
    kDiagnosticSurfaces,
    27,
    120,
    90,
    0,
    11,
    0,
    2,
    11,
    0,
};

} // namespace

const scratchbird::parser::donor::DialectProfile& Profile() {
  return kProfile;
}

std::string TrimAscii(std::string_view text) {
  return scratchbird::parser::donor::TrimAscii(text);
}

std::string NormalizeWhitespace(std::string_view text) {
  return scratchbird::parser::donor::NormalizeWhitespace(text);
}

std::string ToUpperAscii(std::string_view text) {
  return scratchbird::parser::donor::ToUpperAscii(text);
}

std::string MessageVectorToJson(const std::vector<Diagnostic>& diagnostics) {
  return scratchbird::parser::donor::MessageVectorToJson(diagnostics);
}

std::vector<Token> LexTokens(std::string_view sql_text) {
  return scratchbird::parser::donor::LexTokens(sql_text);
}

ParseResult ParseStatement(std::string_view sql_text) {
  return scratchbird::parser::donor::ParseStatement(sql_text, kProfile);
}

std::span<const SurfaceDescriptor> DatatypeSurfaces() {
  return kDatatypeSurfaces;
}

std::span<const SurfaceDescriptor> BuiltinFunctionSurfaces() {
  return kBuiltinSurfaces;
}

std::span<const SurfaceDescriptor> CatalogOverlaySurfaces() {
  return kCatalogSurfaces;
}

std::span<const SurfaceDescriptor> DiagnosticSurfaces() {
  return kDiagnosticSurfaces;
}

std::string OpensearchSqlPplPackageIdentityJson() {
  return scratchbird::parser::donor::PackageIdentityJson(kProfile);
}

std::string OpensearchSqlPplSurfaceReportJson() {
  return scratchbird::parser::donor::SurfaceReportJson(kProfile);
}

} // namespace scratchbird::parser::opensearch_sql_ppl
