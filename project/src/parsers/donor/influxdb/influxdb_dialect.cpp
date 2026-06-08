// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "influxdb_dialect.hpp"

#include <array>

namespace scratchbird::parser::influxdb {
namespace {

using scratchbird::parser::donor::MappingDisposition;
using scratchbird::parser::donor::OperationPattern;
using scratchbird::parser::donor::PatternMatch;
using scratchbird::parser::donor::SurfaceDescriptor;

constexpr std::string_view kSblrFamily = "sblr.donor.influxdb.profile.v1";

constexpr OperationPattern kPatterns[] = {
    {"SHOW SERVERS", PatternMatch::kPrefix, "admin", "influxdb.admin.show_servers",
     MappingDisposition::kAdmittedSblr, "cluster.admin.inspect_status",
     "sblr.cluster.report.v3:cluster.admin.inspect_status", "cluster.inspect_state", "",
     "", true, false},
    {"SHOW CLUSTER", PatternMatch::kPrefix, "admin", "influxdb.admin.show_cluster",
     MappingDisposition::kAdmittedSblr, "cluster.admin.inspect_status",
     "sblr.cluster.report.v3:cluster.admin.inspect_status", "cluster.inspect_state", "",
     "", true, false},
    {"FROM", PatternMatch::kPrefix, "flux", "influxdb.flux.from",
     MappingDisposition::kParserSupportUdr, "influxdb.udr.flux.from",
     "SBLR_DONOR_INFLUXDB_FLUX_ROUTE", "ParserSupportFluxRoute", "INFLUXDB.EMULATION.FLUX_ROUTE",
     "InfluxDB Flux pipelines route through trusted parser support.", true, false},
    {"RANGE", PatternMatch::kPrefix, "flux", "influxdb.flux.range",
     MappingDisposition::kParserSupportUdr, "influxdb.udr.flux.range",
     "SBLR_DONOR_INFLUXDB_FLUX_ROUTE", "ParserSupportFluxRoute", "INFLUXDB.EMULATION.FLUX_ROUTE",
     "InfluxDB Flux pipelines route through trusted parser support.", true, false},
    {"FILTER", PatternMatch::kPrefix, "flux", "influxdb.flux.filter",
     MappingDisposition::kParserSupportUdr, "influxdb.udr.flux.filter",
     "SBLR_DONOR_INFLUXDB_FLUX_ROUTE", "ParserSupportFluxRoute", "INFLUXDB.EMULATION.FLUX_ROUTE",
     "InfluxDB Flux pipelines route through trusted parser support.", true, false},
    {"INSERT", PatternMatch::kPrefix, "write", "influxdb.write.insert",
     MappingDisposition::kAdmittedSblr, "influxdb.write.insert",
     "SBLR_DONOR_INFLUXDB_INSERT", "EngineTimeSeriesInsert", "",
     "", false, true},
    {",", PatternMatch::kContains, "write", "influxdb.write.line_protocol",
     MappingDisposition::kParserSupportUdr, "influxdb.udr.line_protocol",
     "SBLR_DONOR_INFLUXDB_LINE_PROTOCOL_ROUTE", "ParserSupportLineProtocolRoute", "INFLUXDB.EMULATION.LINE_PROTOCOL_ROUTE",
     "InfluxDB line protocol routes through trusted parser support.", true, true},
    {"SELECT", PatternMatch::kPrefix, "query", "influxdb.query.select",
     MappingDisposition::kAdmittedSblr, "influxdb.query.select",
     "SBLR_DONOR_INFLUXDB_SELECT", "EngineTimeSeriesSelect", "",
     "", false, false},
    {"DELETE", PatternMatch::kPrefix, "dml", "influxdb.dml.delete",
     MappingDisposition::kAdmittedSblr, "influxdb.dml.delete",
     "SBLR_DONOR_INFLUXDB_DELETE", "EngineTimeSeriesDelete", "",
     "", false, true},
    {"CREATE DATABASE", PatternMatch::kPrefix, "database", "influxdb.database.create",
     MappingDisposition::kAdmittedSblr, "influxdb.database.create",
     "SBLR_DONOR_INFLUXDB_DATABASE_ROUTE", "EngineDatabaseRoute", "",
     "", true, true},
    {"DROP DATABASE", PatternMatch::kPrefix, "database", "influxdb.database.drop",
     MappingDisposition::kAdmittedSblr, "influxdb.database.drop",
     "SBLR_DONOR_INFLUXDB_DATABASE_ROUTE", "EngineDatabaseRoute", "",
     "", true, true},
    {"CREATE RETENTION POLICY", PatternMatch::kPrefix, "retention_policy", "influxdb.retention_policy.create",
     MappingDisposition::kParserSupportUdr, "influxdb.udr.retention_policy.create",
     "SBLR_DONOR_INFLUXDB_RETENTION_ROUTE", "ParserSupportRetentionRoute", "INFLUXDB.EMULATION.RETENTION_ROUTE",
     "InfluxDB retention policy semantics route through trusted catalog policy.", true, true},
    {"ALTER RETENTION POLICY", PatternMatch::kPrefix, "retention_policy", "influxdb.retention_policy.alter",
     MappingDisposition::kParserSupportUdr, "influxdb.udr.retention_policy.alter",
     "SBLR_DONOR_INFLUXDB_RETENTION_ROUTE", "ParserSupportRetentionRoute", "INFLUXDB.EMULATION.RETENTION_ROUTE",
     "InfluxDB retention policy semantics route through trusted catalog policy.", true, true},
    {"DROP RETENTION POLICY", PatternMatch::kPrefix, "retention_policy", "influxdb.retention_policy.drop",
     MappingDisposition::kParserSupportUdr, "influxdb.udr.retention_policy.drop",
     "SBLR_DONOR_INFLUXDB_RETENTION_ROUTE", "ParserSupportRetentionRoute", "INFLUXDB.EMULATION.RETENTION_ROUTE",
     "InfluxDB retention policy semantics route through trusted catalog policy.", true, true},
    {"CREATE CONTINUOUS QUERY", PatternMatch::kPrefix, "continuous_query", "influxdb.continuous_query.create",
     MappingDisposition::kParserSupportUdr, "influxdb.udr.continuous_query.create",
     "SBLR_DONOR_INFLUXDB_CONTINUOUS_QUERY_ROUTE", "ParserSupportContinuousQueryRoute", "INFLUXDB.EMULATION.CONTINUOUS_QUERY_ROUTE",
     "InfluxDB continuous queries route through trusted scheduler policy.", true, true},
    {"DROP CONTINUOUS QUERY", PatternMatch::kPrefix, "continuous_query", "influxdb.continuous_query.drop",
     MappingDisposition::kParserSupportUdr, "influxdb.udr.continuous_query.drop",
     "SBLR_DONOR_INFLUXDB_CONTINUOUS_QUERY_ROUTE", "ParserSupportContinuousQueryRoute", "INFLUXDB.EMULATION.CONTINUOUS_QUERY_ROUTE",
     "InfluxDB continuous queries route through trusted scheduler policy.", true, true},
    {"CREATE SUBSCRIPTION", PatternMatch::kPrefix, "subscription", "influxdb.subscription.create",
     MappingDisposition::kParserSupportUdr, "influxdb.udr.subscription.create",
     "SBLR_DONOR_INFLUXDB_SUBSCRIPTION_ROUTE", "ParserSupportSubscriptionRoute", "INFLUXDB.EMULATION.SUBSCRIPTION_ROUTE",
     "InfluxDB subscriptions route through trusted external-effect policy.", true, true},
    {"SHOW", PatternMatch::kPrefix, "catalog_overlay", "influxdb.catalog.show",
     MappingDisposition::kCatalogProjection, "influxdb.catalog.show",
     "SBLR_DONOR_INFLUXDB_CATALOG_PROJECT", "EngineCatalogProjection", "",
     "", false, false},
};

const std::array<SurfaceDescriptor, 7> kDatatypeSurfaces{{
    {"numeric", "INTEGER;UNSIGNED;FLOAT", "descriptor"},
    {"string", "STRING", "descriptor"},
    {"boolean", "BOOLEAN", "descriptor"},
    {"timestamp", "TIMESTAMP;RFC3339;UNIX_NANO", "descriptor"},
    {"tag", "TAG", "descriptor"},
    {"field", "FIELD", "descriptor"},
    {"line_protocol", "MEASUREMENT;TAGSET;FIELDSET", "parser_support_udr"},
}};

const std::array<SurfaceDescriptor, 7> kBuiltinSurfaces{{
    {"aggregate", "COUNT;MEAN;MEDIAN;SUM;MIN;MAX;SPREAD", "sblr"},
    {"selector", "FIRST;LAST;TOP;BOTTOM;PERCENTILE", "sblr"},
    {"transform", "DERIVATIVE;NON_NEGATIVE_DERIVATIVE;ELAPSED", "parser_support_udr"},
    {"time", "NOW;TIME;TZ", "sblr"},
    {"flux", "FROM;RANGE;FILTER;MAP;YIELD", "parser_support_udr"},
    {"math", "ABS;SIN;COS;LOG;POW", "sblr"},
    {"fill", "FILL;LINEAR;PREVIOUS", "parser_support_udr"},
}};

const std::array<SurfaceDescriptor, 9> kCatalogSurfaces{{
    {"databases", "INFLUXDB_DATABASES", "catalog_projection"},
    {"buckets", "INFLUXDB_BUCKETS", "catalog_projection"},
    {"measurements", "INFLUXDB_MEASUREMENTS", "catalog_projection"},
    {"tag_keys", "INFLUXDB_TAG_KEYS", "catalog_projection"},
    {"field_keys", "INFLUXDB_FIELD_KEYS", "catalog_projection"},
    {"retention_policies", "INFLUXDB_RETENTION_POLICIES", "catalog_projection"},
    {"continuous_queries", "INFLUXDB_CONTINUOUS_QUERIES", "catalog_projection"},
    {"subscriptions", "INFLUXDB_SUBSCRIPTIONS", "catalog_projection"},
    {"tasks", "INFLUXDB_TASKS", "catalog_projection"},
}};

const std::array<SurfaceDescriptor, 10> kDiagnosticSurfaces{{
    {"parse", "INFLUXDB.PARSE.INVALID_INPUT;INFLUXDB.PARSE.UNSUPPORTED_SURFACE", "parser"},
    {"policy", "INFLUXDB.AUTHORITY.*", "fail_closed"},
    {"udr", "INFLUXDB.EMULATION.*", "parser_support_udr"},
    {"catalog", "INFLUXDB.CATALOG_OVERLAY.READ_ONLY", "catalog_projection"},
    {"session", "INFLUXDB.SESSION.*", "sblr"},
    {"transaction", "INFLUXDB.TRANSACTION.*", "sblr"},
    {"file_effects", "real_donor_file_effects=false", "authority_invariant"},
    {"donor_execution", "donor_engine_sql_executed=false", "authority_invariant"},
    {"mga", "parser_transaction_finality_authority=false", "authority_invariant"},
    {"support_bundle", "source_text_redacted=true", "diagnostic_redaction"},
}};

const scratchbird::parser::donor::DialectProfile kProfile{
    "influxdb",
    "InfluxDB",
    "sbp_influxdb",
    "sbup_influxdb",
    "3.9.0",
    "INFLUXDB",
    kSblrFamily,
    kPatterns,
    kDatatypeSurfaces,
    kBuiltinSurfaces,
    kCatalogSurfaces,
    kDiagnosticSurfaces,
    18,
    84,
    63,
    0,
    9,
    0,
    2,
    10,
    0,
};

} // namespace

const scratchbird::parser::donor::DialectProfile& Profile() { return kProfile; }
std::string TrimAscii(std::string_view text) { return scratchbird::parser::donor::TrimAscii(text); }
std::string NormalizeWhitespace(std::string_view text) { return scratchbird::parser::donor::NormalizeWhitespace(text); }
std::string ToUpperAscii(std::string_view text) { return scratchbird::parser::donor::ToUpperAscii(text); }
std::string MessageVectorToJson(const std::vector<Diagnostic>& diagnostics) { return scratchbird::parser::donor::MessageVectorToJson(diagnostics); }
std::vector<Token> LexTokens(std::string_view sql_text) { return scratchbird::parser::donor::LexTokens(sql_text); }
ParseResult ParseStatement(std::string_view sql_text) { return scratchbird::parser::donor::ParseStatement(sql_text, kProfile); }
std::span<const SurfaceDescriptor> DatatypeSurfaces() { return kDatatypeSurfaces; }
std::span<const SurfaceDescriptor> BuiltinFunctionSurfaces() { return kBuiltinSurfaces; }
std::span<const SurfaceDescriptor> CatalogOverlaySurfaces() { return kCatalogSurfaces; }
std::span<const SurfaceDescriptor> DiagnosticSurfaces() { return kDiagnosticSurfaces; }
std::string InfluxdbPackageIdentityJson() { return scratchbird::parser::donor::PackageIdentityJson(kProfile); }
std::string InfluxdbSurfaceReportJson() { return scratchbird::parser::donor::SurfaceReportJson(kProfile); }

} // namespace scratchbird::parser::influxdb
