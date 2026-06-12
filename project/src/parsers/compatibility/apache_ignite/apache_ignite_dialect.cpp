// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "apache_ignite_dialect.hpp"

#include <array>

namespace scratchbird::parser::apache_ignite {
namespace {

using scratchbird::parser::compatibility::MappingDisposition;
using scratchbird::parser::compatibility::OperationPattern;
using scratchbird::parser::compatibility::PatternMatch;
using scratchbird::parser::compatibility::SurfaceDescriptor;

constexpr std::string_view kSblrFamily = "sblr.compatibility.apache_ignite.profile.v1";

constexpr OperationPattern kPatterns[] = {
    {"CONTROL.SH", PatternMatch::kContains, "admin", "apache_ignite.admin.control_script",
     MappingDisposition::kPolicyRefusal, "apache_ignite.policy.cluster.control_script",
     "", "", "APACHE_IGNITE.AUTHORITY.CLUSTER_CONTROL_RESERVED",
     "Apache Ignite control.sh cluster actions are reserved for normalized cluster SBLR.", true, false},
    {"BASELINE", PatternMatch::kContains, "admin", "apache_ignite.admin.baseline",
     MappingDisposition::kAdmittedSblr, "cluster.job.start_controlled",
     "sblr.cluster.control.v3:cluster.job.start_controlled", "cluster.start_job", "",
     "", true, false},
    {"SET STATE", PatternMatch::kPrefix, "admin", "apache_ignite.admin.set_state",
     MappingDisposition::kAdmittedSblr, "cluster.job.start_controlled",
     "sblr.cluster.control.v3:cluster.job.start_controlled", "cluster.start_job", "",
     "", true, false},
    {"ACTIVATE", PatternMatch::kPrefix, "admin", "apache_ignite.admin.activate",
     MappingDisposition::kAdmittedSblr, "cluster.job.start_controlled",
     "sblr.cluster.control.v3:cluster.job.start_controlled", "cluster.start_job", "",
     "", true, false},
    {"DEACTIVATE", PatternMatch::kPrefix, "admin", "apache_ignite.admin.deactivate",
     MappingDisposition::kAdmittedSblr, "cluster.job.start_controlled",
     "sblr.cluster.control.v3:cluster.job.start_controlled", "cluster.start_job", "",
     "", true, false},
    {"SNAPSHOT", PatternMatch::kPrefix, "admin", "apache_ignite.admin.snapshot",
     MappingDisposition::kAdmittedSblr, "cluster.job.start_controlled",
     "sblr.cluster.control.v3:cluster.job.start_controlled", "cluster.start_job", "",
     "", true, false},
    {"CREATE CACHE", PatternMatch::kPrefix, "cache", "apache_ignite.cache.create",
     MappingDisposition::kParserSupportUdr, "apache_ignite.udr.cache.create",
     "SBLR_COMPAT_APACHE_IGNITE_CACHE_ROUTE", "ParserSupportCacheRoute", "APACHE_IGNITE.EMULATION.CACHE_ROUTE",
     "Apache Ignite cache lifecycle routes through trusted parser support.", true, true},
    {"DROP CACHE", PatternMatch::kPrefix, "cache", "apache_ignite.cache.drop",
     MappingDisposition::kParserSupportUdr, "apache_ignite.udr.cache.drop",
     "SBLR_COMPAT_APACHE_IGNITE_CACHE_ROUTE", "ParserSupportCacheRoute", "APACHE_IGNITE.EMULATION.CACHE_ROUTE",
     "Apache Ignite cache lifecycle routes through trusted parser support.", true, true},
    {"CACHE GET", PatternMatch::kPrefix, "cache", "apache_ignite.cache.get",
     MappingDisposition::kAdmittedSblr, "apache_ignite.cache.get",
     "SBLR_COMPAT_APACHE_IGNITE_CACHE_GET", "EngineCacheGet", "",
     "", false, false},
    {"CACHE PUT", PatternMatch::kPrefix, "cache", "apache_ignite.cache.put",
     MappingDisposition::kAdmittedSblr, "apache_ignite.cache.put",
     "SBLR_COMPAT_APACHE_IGNITE_CACHE_PUT", "EngineCachePut", "",
     "", false, true},
    {"CACHE REMOVE", PatternMatch::kPrefix, "cache", "apache_ignite.cache.remove",
     MappingDisposition::kAdmittedSblr, "apache_ignite.cache.remove",
     "SBLR_COMPAT_APACHE_IGNITE_CACHE_REMOVE", "EngineCacheRemove", "",
     "", false, true},
    {"SCAN", PatternMatch::kPrefix, "cache", "apache_ignite.cache.scan",
     MappingDisposition::kParserSupportUdr, "apache_ignite.udr.cache.scan",
     "SBLR_COMPAT_APACHE_IGNITE_SCAN_ROUTE", "ParserSupportScanRoute", "APACHE_IGNITE.EMULATION.SCAN_ROUTE",
     "Apache Ignite scan query options route through trusted parser support.", true, false},
    {"CONTINUOUS QUERY", PatternMatch::kPrefix, "cache", "apache_ignite.cache.continuous_query",
     MappingDisposition::kParserSupportUdr, "apache_ignite.udr.continuous_query",
     "SBLR_COMPAT_APACHE_IGNITE_CONTINUOUS_QUERY_ROUTE", "ParserSupportContinuousQueryRoute", "APACHE_IGNITE.EMULATION.CONTINUOUS_QUERY_ROUTE",
     "Apache Ignite continuous queries route through trusted parser support.", true, false},
    {"CREATE TABLE", PatternMatch::kPrefix, "ddl", "apache_ignite.ddl.create_table",
     MappingDisposition::kAdmittedSblr, "apache_ignite.ddl.create_table",
     "SBLR_COMPAT_APACHE_IGNITE_DDL_ROUTE", "EngineDdlCreate", "",
     "", true, true},
    {"CREATE INDEX", PatternMatch::kPrefix, "ddl", "apache_ignite.ddl.create_index",
     MappingDisposition::kAdmittedSblr, "apache_ignite.ddl.create_index",
     "SBLR_COMPAT_APACHE_IGNITE_DDL_ROUTE", "EngineDdlCreate", "",
     "", true, true},
    {"DROP TABLE", PatternMatch::kPrefix, "ddl", "apache_ignite.ddl.drop_table",
     MappingDisposition::kAdmittedSblr, "apache_ignite.ddl.drop_table",
     "SBLR_COMPAT_APACHE_IGNITE_DDL_ROUTE", "EngineDdlDrop", "",
     "", true, true},
    {"DROP INDEX", PatternMatch::kPrefix, "ddl", "apache_ignite.ddl.drop_index",
     MappingDisposition::kAdmittedSblr, "apache_ignite.ddl.drop_index",
     "SBLR_COMPAT_APACHE_IGNITE_DDL_ROUTE", "EngineDdlDrop", "",
     "", true, true},
    {"ALTER TABLE", PatternMatch::kPrefix, "ddl", "apache_ignite.ddl.alter_table",
     MappingDisposition::kAdmittedSblr, "apache_ignite.ddl.alter_table",
     "SBLR_COMPAT_APACHE_IGNITE_DDL_ROUTE", "EngineDdlAlter", "",
     "", true, true},
    {"MERGE", PatternMatch::kPrefix, "dml", "apache_ignite.dml.merge",
     MappingDisposition::kAdmittedSblr, "apache_ignite.dml.merge",
     "SBLR_COMPAT_APACHE_IGNITE_MERGE", "EngineDmlMerge", "",
     "", false, true},
    {"INSERT", PatternMatch::kPrefix, "dml", "apache_ignite.dml.insert",
     MappingDisposition::kAdmittedSblr, "apache_ignite.dml.insert",
     "SBLR_COMPAT_APACHE_IGNITE_INSERT", "EngineDmlInsert", "",
     "", false, true},
    {"UPDATE", PatternMatch::kPrefix, "dml", "apache_ignite.dml.update",
     MappingDisposition::kAdmittedSblr, "apache_ignite.dml.update",
     "SBLR_COMPAT_APACHE_IGNITE_UPDATE", "EngineDmlUpdate", "",
     "", false, true},
    {"DELETE", PatternMatch::kPrefix, "dml", "apache_ignite.dml.delete",
     MappingDisposition::kAdmittedSblr, "apache_ignite.dml.delete",
     "SBLR_COMPAT_APACHE_IGNITE_DELETE", "EngineDmlDelete", "",
     "", false, true},
    {"SELECT", PatternMatch::kPrefix, "query", "apache_ignite.query.select",
     MappingDisposition::kAdmittedSblr, "apache_ignite.query.select",
     "SBLR_COMPAT_APACHE_IGNITE_SELECT", "EngineQuerySelect", "",
     "", false, false},
    {"WITH", PatternMatch::kPrefix, "query", "apache_ignite.query.with",
     MappingDisposition::kAdmittedSblr, "apache_ignite.query.with",
     "SBLR_COMPAT_APACHE_IGNITE_SELECT", "EngineQuerySelect", "",
     "", false, false},
    {"EXPLAIN", PatternMatch::kPrefix, "optimizer", "apache_ignite.optimizer.explain",
     MappingDisposition::kCatalogProjection, "apache_ignite.optimizer.explain",
     "SBLR_COMPAT_APACHE_IGNITE_EXPLAIN", "EngineExplainPlan", "",
     "", false, false},
    {"SHOW", PatternMatch::kPrefix, "catalog_overlay", "apache_ignite.catalog.show",
     MappingDisposition::kCatalogProjection, "apache_ignite.catalog.show",
     "SBLR_COMPAT_APACHE_IGNITE_CATALOG_PROJECT", "EngineCatalogProjection", "",
     "", false, false},
    {"CREATE USER", PatternMatch::kPrefix, "security", "apache_ignite.security.create_user",
     MappingDisposition::kParserSupportUdr, "apache_ignite.udr.security.create_user",
     "SBLR_COMPAT_APACHE_IGNITE_SECURITY_ROUTE", "ParserSupportSecurityRoute", "APACHE_IGNITE.EMULATION.SECURITY_ROUTE",
     "Apache Ignite account management routes through trusted security policy.", true, false},
    {"GRANT", PatternMatch::kPrefix, "security", "apache_ignite.security.grant",
     MappingDisposition::kParserSupportUdr, "apache_ignite.udr.security.grant",
     "SBLR_COMPAT_APACHE_IGNITE_SECURITY_ROUTE", "ParserSupportSecurityRoute", "APACHE_IGNITE.EMULATION.SECURITY_ROUTE",
     "Apache Ignite privilege changes route through trusted security policy.", true, true},
    {"REVOKE", PatternMatch::kPrefix, "security", "apache_ignite.security.revoke",
     MappingDisposition::kParserSupportUdr, "apache_ignite.udr.security.revoke",
     "SBLR_COMPAT_APACHE_IGNITE_SECURITY_ROUTE", "ParserSupportSecurityRoute", "APACHE_IGNITE.EMULATION.SECURITY_ROUTE",
     "Apache Ignite privilege changes route through trusted security policy.", true, true},
    {"SET STREAMING", PatternMatch::kPrefix, "session", "apache_ignite.session.streaming",
     MappingDisposition::kParserSupportUdr, "apache_ignite.udr.session.streaming",
     "SBLR_COMPAT_APACHE_IGNITE_STREAMING_ROUTE", "ParserSupportStreamingRoute",
     "APACHE_IGNITE.EMULATION.STREAMING_ROUTE",
     "Apache Ignite streaming mode routes through the Apache Ignite compatibility UDR.", true, false},
    {"SET", PatternMatch::kPrefix, "session", "apache_ignite.session.set",
     MappingDisposition::kAdmittedSblr, "apache_ignite.session.set",
     "SBLR_COMPAT_APACHE_IGNITE_SET", "EngineSessionSet", "",
     "", false, false},
    {"COMMIT", PatternMatch::kPrefix, "transaction", "apache_ignite.transaction.commit",
     MappingDisposition::kAdmittedSblr, "apache_ignite.transaction.commit",
     "SBLR_TRANSACTION_COMMIT", "EngineCommitTransaction", "",
     "", false, true},
    {"ROLLBACK", PatternMatch::kPrefix, "transaction", "apache_ignite.transaction.rollback",
     MappingDisposition::kAdmittedSblr, "apache_ignite.transaction.rollback",
     "SBLR_TRANSACTION_ROLLBACK", "EngineRollbackTransaction", "",
     "", false, true},
};

const std::array<SurfaceDescriptor, 10> kDatatypeSurfaces{{
    {"numeric", "TINYINT;SMALLINT;INT;BIGINT;DECIMAL;FLOAT;DOUBLE", "descriptor"},
    {"text", "CHAR;VARCHAR;UUID", "descriptor"},
    {"binary", "BINARY;VARBINARY", "descriptor"},
    {"temporal", "DATE;TIME;TIMESTAMP", "descriptor"},
    {"boolean", "BOOLEAN", "descriptor"},
    {"object", "OBJECT;BINARY_OBJECT", "parser_support_udr"},
    {"cache_key", "KEY_TYPE;VALUE_TYPE;AFFINITY_KEY", "catalog_policy"},
    {"cache_policy", "CACHE_NAME;BACKUPS;TEMPLATE;DATA_REGION", "catalog_policy"},
    {"query_hint", "LOCAL;LAZY;COLLOCATED;DISTRIBUTED_JOINS", "catalog_policy"},
    {"service", "CONTINUOUS_QUERY;SCAN_QUERY", "parser_support_udr"},
}};

const std::array<SurfaceDescriptor, 10> kBuiltinSurfaces{{
    {"aggregate", "COUNT;SUM;AVG;MIN;MAX", "sblr"},
    {"string", "LOWER;UPPER;TRIM;LIKE", "sblr"},
    {"datetime", "CURRENT_DATE;CURRENT_TIME;CURRENT_TIMESTAMP", "sblr"},
    {"cache", "CACHE_GET;CACHE_PUT;CACHE_REMOVE;SCAN", "sblr"},
    {"continuous_query", "CONTINUOUS_QUERY;INITIAL_QUERY", "parser_support_udr"},
    {"optimizer", "EXPLAIN;INDEX_SCAN", "catalog_projection"},
    {"security", "USER;ROLE;GRANT;REVOKE", "parser_support_udr"},
    {"streaming", "SET STREAMING", "parser_support_udr"},
    {"session", "SET", "sblr"},
    {"cluster", "BASELINE;ACTIVATE;DEACTIVATE;SNAPSHOT", "fail_closed"},
}};

const std::array<SurfaceDescriptor, 10> kCatalogSurfaces{{
    {"caches", "IGNITE_CACHES", "catalog_projection"},
    {"tables", "IGNITE_TABLES", "catalog_projection"},
    {"indexes", "IGNITE_INDEXES", "catalog_projection"},
    {"query_views", "IGNITE_SQL_FIELDS", "catalog_projection"},
    {"metrics", "IGNITE_METRICS", "catalog_projection"},
    {"system_views", "IGNITE_SYSTEM_VIEWS", "catalog_projection"},
    {"users", "IGNITE_USERS", "catalog_projection"},
    {"roles", "IGNITE_ROLES", "catalog_projection"},
    {"baseline", "IGNITE_BASELINE", "fail_closed"},
    {"snapshots", "IGNITE_SNAPSHOTS", "fail_closed"},
}};

const std::array<SurfaceDescriptor, 10> kDiagnosticSurfaces{{
    {"parse", "APACHE_IGNITE.PARSE.INVALID_INPUT;APACHE_IGNITE.PARSE.UNSUPPORTED_SURFACE", "parser"},
    {"policy", "APACHE_IGNITE.AUTHORITY.*", "fail_closed"},
    {"udr", "APACHE_IGNITE.EMULATION.*", "parser_support_udr"},
    {"catalog", "APACHE_IGNITE.CATALOG_OVERLAY.READ_ONLY", "catalog_projection"},
    {"session", "APACHE_IGNITE.SESSION.*", "sblr"},
    {"transaction", "APACHE_IGNITE.TRANSACTION.*", "sblr"},
    {"file_effects", "real_reference_file_effects=false", "authority_invariant"},
    {"reference_execution", "reference_engine_sql_executed=false", "authority_invariant"},
    {"mga", "parser_transaction_finality_authority=false", "authority_invariant"},
    {"support_bundle", "source_text_redacted=true", "diagnostic_redaction"},
}};

const scratchbird::parser::compatibility::DialectProfile kProfile{
    "apache_ignite",
    "Apache Ignite",
    "sbp_apache_ignite",
    "sbup_apache_ignite",
    "2.17.0",
    "APACHE_IGNITE",
    kSblrFamily,
    kPatterns,
    kDatatypeSurfaces,
    kBuiltinSurfaces,
    kCatalogSurfaces,
    kDiagnosticSurfaces,
    34,
    92,
    68,
    0,
    10,
    0,
    6,
    8,
    0,
};

} // namespace

const scratchbird::parser::compatibility::DialectProfile& Profile() { return kProfile; }
std::string TrimAscii(std::string_view text) { return scratchbird::parser::compatibility::TrimAscii(text); }
std::string NormalizeWhitespace(std::string_view text) { return scratchbird::parser::compatibility::NormalizeWhitespace(text); }
std::string ToUpperAscii(std::string_view text) { return scratchbird::parser::compatibility::ToUpperAscii(text); }
std::string MessageVectorToJson(const std::vector<Diagnostic>& diagnostics) { return scratchbird::parser::compatibility::MessageVectorToJson(diagnostics); }
std::vector<Token> LexTokens(std::string_view sql_text) { return scratchbird::parser::compatibility::LexTokens(sql_text); }
ParseResult ParseStatement(std::string_view sql_text) { return scratchbird::parser::compatibility::ParseStatement(sql_text, kProfile); }
std::span<const SurfaceDescriptor> DatatypeSurfaces() { return kDatatypeSurfaces; }
std::span<const SurfaceDescriptor> BuiltinFunctionSurfaces() { return kBuiltinSurfaces; }
std::span<const SurfaceDescriptor> CatalogOverlaySurfaces() { return kCatalogSurfaces; }
std::span<const SurfaceDescriptor> DiagnosticSurfaces() { return kDiagnosticSurfaces; }
std::string ApacheIgnitePackageIdentityJson() { return scratchbird::parser::compatibility::PackageIdentityJson(kProfile); }
std::string ApacheIgniteSurfaceReportJson() { return scratchbird::parser::compatibility::SurfaceReportJson(kProfile); }

} // namespace scratchbird::parser::apache_ignite
