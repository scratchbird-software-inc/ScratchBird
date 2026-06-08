// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "duckdb_dialect.hpp"

#include <array>

namespace scratchbird::parser::duckdb {
namespace {

using scratchbird::parser::donor::MappingDisposition;
using scratchbird::parser::donor::OperationPattern;
using scratchbird::parser::donor::PatternMatch;
using scratchbird::parser::donor::SurfaceDescriptor;

constexpr std::string_view kSblrFamily = "sblr.donor.duckdb.profile.v1";

constexpr OperationPattern kPatterns[] = {
    {"HTTP://||HTTPS://", PatternMatch::kFromStringLiteralUriScheme, "external_scan", "duckdb.external_scan.http",
     MappingDisposition::kParserSupportUdr, "duckdb.udr.etl.http",
     "SBLR_DONOR_DUCKDB_ETL_ROUTE", "ParserSupportEtlRoute",
     "DUCKDB.EMULATION.ETL_ROUTE",
     "DuckDB HTTP/HTTPS relation-literal scans route through the DuckDB donor UDR.", true, false},
    {"S3://", PatternMatch::kFromStringLiteralUriScheme, "external_scan", "duckdb.external_scan.s3",
     MappingDisposition::kParserSupportUdr, "duckdb.udr.etl.s3",
     "SBLR_DONOR_DUCKDB_ETL_ROUTE", "ParserSupportEtlRoute",
     "DUCKDB.EMULATION.ETL_ROUTE",
     "DuckDB S3 relation-literal scans route through the DuckDB donor UDR.", true, false},
    {" FROM '", PatternMatch::kContains, "bulk_io", "duckdb.bulk_io.file_from",
     MappingDisposition::kPolicyRefusal, "duckdb.policy.file.copy_from", "",
     "", "DUCKDB.AUTHORITY.FILE_IO_DENIED",
     "DuckDB file reads require a trusted ScratchBird import service.", true, false},
    {"COPY|| TO '", PatternMatch::kPrefixAndContains, "bulk_io", "duckdb.bulk_io.file_to",
     MappingDisposition::kPolicyRefusal, "duckdb.policy.file.copy_to", "",
     "", "DUCKDB.AUTHORITY.FILE_IO_DENIED",
     "DuckDB file writes require trusted engine admission.", true, false},
    {"READ_CSV", PatternMatch::kContainsFunctionCall, "external_scan", "duckdb.external_scan.read_csv",
     MappingDisposition::kParserSupportUdr, "duckdb.udr.etl.read_csv",
     "SBLR_DONOR_DUCKDB_ETL_ROUTE", "ParserSupportEtlRoute",
     "DUCKDB.EMULATION.ETL_ROUTE",
     "DuckDB READ_CSV routes through the DuckDB donor UDR for ETL admission.", true, false},
    {"READ_PARQUET", PatternMatch::kContainsFunctionCall, "external_scan", "duckdb.external_scan.read_parquet",
     MappingDisposition::kParserSupportUdr, "duckdb.udr.etl.read_parquet",
     "SBLR_DONOR_DUCKDB_ETL_ROUTE", "ParserSupportEtlRoute",
     "DUCKDB.EMULATION.ETL_ROUTE",
     "DuckDB READ_PARQUET routes through the DuckDB donor UDR for ETL admission.", true, false},
    {"READ_JSON", PatternMatch::kContainsFunctionCall, "external_scan", "duckdb.external_scan.read_json",
     MappingDisposition::kParserSupportUdr, "duckdb.udr.etl.read_json",
     "SBLR_DONOR_DUCKDB_ETL_ROUTE", "ParserSupportEtlRoute",
     "DUCKDB.EMULATION.ETL_ROUTE",
     "DuckDB READ_JSON routes through the DuckDB donor UDR for ETL admission.", true, false},
    {"COPY", PatternMatch::kPrefix, "bulk_io", "duckdb.bulk_io.copy",
     MappingDisposition::kParserSupportUdr, "duckdb.udr.copy",
     "SBLR_DONOR_DUCKDB_COPY_ROUTE", "ParserSupportCopyRoute",
     "DUCKDB.EMULATION.COPY_ROUTE",
     "DuckDB COPY routes through trusted import/export policy and cannot perform parser-owned file effects.", true, false},
    {"INSTALL", PatternMatch::kPrefix, "extension", "duckdb.extension.install",
     MappingDisposition::kPolicyRefusal, "duckdb.policy.extension.install", "",
     "", "DUCKDB.AUTHORITY.EXTENSION_DENIED",
     "DuckDB extension installation is blocked from parser authority.", true, false},
    {"LOAD", PatternMatch::kPrefix, "extension", "duckdb.extension.load",
     MappingDisposition::kPolicyRefusal, "duckdb.policy.extension.load", "",
     "", "DUCKDB.AUTHORITY.EXTENSION_DENIED",
     "DuckDB extension loading is blocked from parser authority.", true, false},
    {"EXPORT DATABASE", PatternMatch::kPrefix, "bulk_io", "duckdb.bulk_io.export_database",
     MappingDisposition::kPolicyRefusal, "duckdb.policy.file.export_database", "",
     "", "DUCKDB.AUTHORITY.FILE_IO_DENIED",
     "DuckDB EXPORT DATABASE cannot perform parser-owned filesystem writes.", true, false},
    {"IMPORT DATABASE", PatternMatch::kPrefix, "bulk_io", "duckdb.bulk_io.import_database",
     MappingDisposition::kPolicyRefusal, "duckdb.policy.file.import_database", "",
     "", "DUCKDB.AUTHORITY.FILE_IO_DENIED",
     "DuckDB IMPORT DATABASE requires trusted engine lifecycle admission.", true, false},
    {"ATTACH", PatternMatch::kPrefix, "database_lifecycle", "duckdb.lifecycle.attach",
     MappingDisposition::kPolicyRefusal, "duckdb.policy.file.attach", "",
     "", "DUCKDB.AUTHORITY.FILE_IO_DENIED",
     "DuckDB ATTACH would bind external storage outside parser authority.", true, false},
    {"DETACH", PatternMatch::kPrefix, "database_lifecycle", "duckdb.lifecycle.detach",
     MappingDisposition::kPolicyRefusal, "duckdb.policy.file.detach", "",
     "", "DUCKDB.AUTHORITY.FILE_IO_DENIED",
     "DuckDB DETACH storage lifecycle is not parser authority.", true, false},
    {"CHECKPOINT", PatternMatch::kPrefix, "storage_admin", "duckdb.storage.checkpoint",
     MappingDisposition::kUnsupportedRefusal, "duckdb.policy.unsupported.checkpoint",
     "", "", "DUCKDB.AUTHORITY.UNSUPPORTED_DENIED",
     "DuckDB CHECKPOINT is a donor low-level utility surface and is outside donor parser authority.",
     true, false},
    {"CREATE SECRET", PatternMatch::kPrefix, "security", "duckdb.security.create_secret",
     MappingDisposition::kParserSupportUdr, "duckdb.udr.security.create_secret",
     "SBLR_DONOR_DUCKDB_SECURITY_ROUTE", "ParserSupportSecurityRoute",
     "DUCKDB.EMULATION.SECURITY_ROUTE",
     "DuckDB secret metadata routes through trusted security policy.", true, false},
    {"ALTER SECRET", PatternMatch::kPrefix, "security", "duckdb.security.alter_secret",
     MappingDisposition::kParserSupportUdr, "duckdb.udr.security.alter_secret",
     "SBLR_DONOR_DUCKDB_SECURITY_ROUTE", "ParserSupportSecurityRoute",
     "DUCKDB.EMULATION.SECURITY_ROUTE",
     "DuckDB secret changes route through trusted security policy.", true, false},
    {"DROP SECRET", PatternMatch::kPrefix, "security", "duckdb.security.drop_secret",
     MappingDisposition::kParserSupportUdr, "duckdb.udr.security.drop_secret",
     "SBLR_DONOR_DUCKDB_SECURITY_ROUTE", "ParserSupportSecurityRoute",
     "DUCKDB.EMULATION.SECURITY_ROUTE",
     "DuckDB secret removal routes through trusted security policy.", true, false},
    {"CREATE MACRO", PatternMatch::kPrefix, "routine", "duckdb.routine.create_macro",
     MappingDisposition::kParserSupportUdr, "duckdb.udr.routine.create_macro",
     "SBLR_DONOR_DUCKDB_ROUTINE_ROUTE", "ParserSupportRoutineRoute",
     "DUCKDB.EMULATION.ROUTINE_ROUTE",
     "DuckDB macros route through trusted routine package policy.", true, true},
    {"CREATE FUNCTION", PatternMatch::kPrefix, "routine", "duckdb.routine.create_function",
     MappingDisposition::kParserSupportUdr, "duckdb.udr.routine.create_function",
     "SBLR_DONOR_DUCKDB_ROUTINE_ROUTE", "ParserSupportRoutineRoute",
     "DUCKDB.EMULATION.ROUTINE_ROUTE",
     "DuckDB functions route through trusted routine package policy.", true, true},
    {"CALL", PatternMatch::kPrefix, "routine", "duckdb.routine.call",
     MappingDisposition::kParserSupportUdr, "duckdb.udr.routine.call",
     "SBLR_DONOR_DUCKDB_ROUTINE_CALL", "ParserSupportRoutineRoute",
     "DUCKDB.EMULATION.ROUTINE_ROUTE",
     "DuckDB CALL routes through trusted package policy.", true, true},
    {"PRAGMA", PatternMatch::kPrefix, "pragma", "duckdb.pragma.generic",
     MappingDisposition::kParserSupportUdr, "duckdb.udr.pragma.generic",
     "SBLR_DONOR_DUCKDB_PRAGMA_ROUTE", "ParserSupportPragmaRoute",
     "DUCKDB.EMULATION.PRAGMA_ROUTE",
     "DuckDB PRAGMA routes through trusted parser-support policy.", true, false},
    {"SUMMARIZE", PatternMatch::kPrefix, "optimizer", "duckdb.optimizer.summarize",
     MappingDisposition::kCatalogProjection, "duckdb.optimizer.summarize",
     "SBLR_DONOR_DUCKDB_SUMMARIZE", "EngineSummaryProjection", "", "", false, false},
    {"PIVOT", PatternMatch::kPrefix, "query", "duckdb.query.pivot",
     MappingDisposition::kAdmittedSblr, "duckdb.query.pivot",
     "SBLR_DONOR_DUCKDB_PIVOT", "EngineQueryPivot", "", "", false, false},
    {"UNPIVOT", PatternMatch::kPrefix, "query", "duckdb.query.unpivot",
     MappingDisposition::kAdmittedSblr, "duckdb.query.unpivot",
     "SBLR_DONOR_DUCKDB_UNPIVOT", "EngineQueryUnpivot", "", "", false, false},
    {"DESCRIBE", PatternMatch::kPrefix, "catalog_overlay", "duckdb.catalog_overlay.describe",
     MappingDisposition::kCatalogProjection, "duckdb.catalog.describe",
     "SBLR_DONOR_DUCKDB_CATALOG_PROJECT", "EngineCatalogProjection", "", "", false, false},
    {"SHOW", PatternMatch::kPrefix, "catalog_overlay", "duckdb.catalog_overlay.show",
     MappingDisposition::kCatalogProjection, "duckdb.catalog.show",
     "SBLR_DONOR_DUCKDB_CATALOG_PROJECT", "EngineCatalogProjection", "", "", false, false},
    {"EXPLAIN", PatternMatch::kPrefix, "optimizer", "duckdb.optimizer.explain",
     MappingDisposition::kCatalogProjection, "duckdb.optimizer.explain",
     "SBLR_DONOR_DUCKDB_EXPLAIN", "EngineExplainPlan", "", "", false, false},
    {"BEGIN", PatternMatch::kPrefix, "transaction", "duckdb.transaction.begin",
     MappingDisposition::kAdmittedSblr, "duckdb.transaction.begin",
     "SBLR_TRANSACTION_BEGIN", "EngineBeginTransaction", "", "", false, false},
    {"START TRANSACTION", PatternMatch::kPrefix, "transaction", "duckdb.transaction.start",
     MappingDisposition::kAdmittedSblr, "duckdb.transaction.start",
     "SBLR_TRANSACTION_BEGIN", "EngineBeginTransaction", "", "", false, false},
    {"COMMIT", PatternMatch::kPrefix, "transaction", "duckdb.transaction.commit",
     MappingDisposition::kAdmittedSblr, "duckdb.transaction.commit",
     "SBLR_TRANSACTION_COMMIT", "EngineCommitTransaction", "", "", false, true},
    {"ROLLBACK", PatternMatch::kPrefix, "transaction", "duckdb.transaction.rollback",
     MappingDisposition::kAdmittedSblr, "duckdb.transaction.rollback",
     "SBLR_TRANSACTION_ROLLBACK", "EngineRollbackTransaction", "", "", false, true},
    {"SAVEPOINT", PatternMatch::kPrefix, "transaction", "duckdb.transaction.savepoint",
     MappingDisposition::kAdmittedSblr, "duckdb.transaction.savepoint",
     "SBLR_TRANSACTION_SAVEPOINT", "EngineSavepoint", "", "", false, true},
    {"RELEASE SAVEPOINT", PatternMatch::kPrefix, "transaction", "duckdb.transaction.release_savepoint",
     MappingDisposition::kAdmittedSblr, "duckdb.transaction.release_savepoint",
     "SBLR_TRANSACTION_RELEASE_SAVEPOINT", "EngineReleaseSavepoint", "", "", false, true},
    {"SET", PatternMatch::kPrefix, "session", "duckdb.session.set",
     MappingDisposition::kAdmittedSblr, "duckdb.session.set",
     "SBLR_DONOR_DUCKDB_SET", "EngineSessionSet", "", "", false, false},
    {"RESET", PatternMatch::kPrefix, "session", "duckdb.session.reset",
     MappingDisposition::kAdmittedSblr, "duckdb.session.reset",
     "SBLR_DONOR_DUCKDB_RESET", "EngineSessionReset", "", "", false, false},
    {"CREATE", PatternMatch::kPrefix, "ddl", "duckdb.ddl.create",
     MappingDisposition::kAdmittedSblr, "duckdb.ddl.create",
     "SBLR_DONOR_DUCKDB_DDL_CREATE", "EngineDdlCreate", "", "", true, true},
    {"ALTER", PatternMatch::kPrefix, "ddl", "duckdb.ddl.alter",
     MappingDisposition::kAdmittedSblr, "duckdb.ddl.alter",
     "SBLR_DONOR_DUCKDB_DDL_ALTER", "EngineDdlAlter", "", "", true, true},
    {"DROP", PatternMatch::kPrefix, "ddl", "duckdb.ddl.drop",
     MappingDisposition::kAdmittedSblr, "duckdb.ddl.drop",
     "SBLR_DONOR_DUCKDB_DDL_DROP", "EngineDdlDrop", "", "", true, true},
    {"INSERT", PatternMatch::kPrefix, "dml", "duckdb.dml.insert",
     MappingDisposition::kAdmittedSblr, "duckdb.dml.insert",
     "SBLR_DONOR_DUCKDB_INSERT", "EngineDmlInsert", "", "", false, true},
    {"UPDATE", PatternMatch::kPrefix, "dml", "duckdb.dml.update",
     MappingDisposition::kAdmittedSblr, "duckdb.dml.update",
     "SBLR_DONOR_DUCKDB_UPDATE", "EngineDmlUpdate", "", "", false, true},
    {"DELETE", PatternMatch::kPrefix, "dml", "duckdb.dml.delete",
     MappingDisposition::kAdmittedSblr, "duckdb.dml.delete",
     "SBLR_DONOR_DUCKDB_DELETE", "EngineDmlDelete", "", "", false, true},
    {"MERGE", PatternMatch::kPrefix, "dml", "duckdb.dml.merge",
     MappingDisposition::kAdmittedSblr, "duckdb.dml.merge",
     "SBLR_DONOR_DUCKDB_MERGE", "EngineDmlMerge", "", "", false, true},
    {"SELECT", PatternMatch::kPrefix, "query", "duckdb.query.select",
     MappingDisposition::kAdmittedSblr, "duckdb.query.select",
     "SBLR_DONOR_DUCKDB_SELECT", "EngineQuerySelect", "", "", false, false},
    {"WITH", PatternMatch::kPrefix, "query", "duckdb.query.with",
     MappingDisposition::kAdmittedSblr, "duckdb.query.with",
     "SBLR_DONOR_DUCKDB_SELECT", "EngineQuerySelect", "", "", false, false},
};

const std::array<SurfaceDescriptor, 11> kDatatypeSurfaces{{
    {"numeric", "TINYINT;SMALLINT;INTEGER;BIGINT;HUGEINT;UHUGEINT;DECIMAL;FLOAT;DOUBLE", "descriptor"},
    {"unsigned_numeric", "UTINYINT;USMALLINT;UINTEGER;UBIGINT", "descriptor"},
    {"text", "VARCHAR;TEXT;STRING", "descriptor"},
    {"binary", "BLOB;BIT;BITSTRING", "descriptor"},
    {"temporal", "DATE;TIME;TIMESTAMP;TIMESTAMPTZ;INTERVAL", "descriptor"},
    {"boolean", "BOOLEAN;BOOL", "descriptor"},
    {"nested", "STRUCT;LIST;ARRAY;MAP;UNION", "parser_support_udr"},
    {"json", "JSON", "parser_support_udr"},
    {"uuid", "UUID", "descriptor"},
    {"enum", "ENUM", "parser_support_udr"},
    {"spatial", "GEOMETRY;POINT;LINESTRING;POLYGON", "parser_support_udr"},
}};

const std::array<SurfaceDescriptor, 12> kBuiltinSurfaces{{
    {"aggregate", "COUNT;SUM;AVG;MIN;MAX;MEDIAN;QUANTILE;MODE;HISTOGRAM", "sblr"},
    {"window", "ROW_NUMBER;RANK;DENSE_RANK;LAG;LEAD;FIRST_VALUE;LAST_VALUE", "sblr"},
    {"string", "CONCAT;PRINTF;FORMAT;LOWER;UPPER;TRIM;REGEXP_REPLACE", "sblr"},
    {"numeric", "ABS;ROUND;POWER;SQRT;CBRT;GAMMA;FACTORIAL", "sblr"},
    {"temporal", "DATE_PART;DATE_TRUNC;EPOCH;STRFTIME;TIME_BUCKET", "sblr"},
    {"json", "JSON_EXTRACT;JSON_VALUE;JSON_TYPE;JSON_KEYS;TO_JSON", "parser_support_udr"},
    {"list", "LIST_VALUE;LIST_TRANSFORM;LIST_FILTER;UNNEST;GENERATE_SERIES", "parser_support_udr"},
    {"map_struct", "STRUCT_PACK;STRUCT_EXTRACT;MAP;MAP_EXTRACT", "parser_support_udr"},
    {"stats", "STATS;CAN_CAST_IMPLICITLY;TYPEOF", "catalog_projection"},
    {"external_scan", "READ_CSV;READ_PARQUET;READ_JSON;READ_TEXT", "parser_support_udr"},
    {"extension", "INSTALL;LOAD", "fail_closed"},
    {"secrets", "CREATE SECRET;DROP SECRET", "parser_support_udr"},
}};

const std::array<SurfaceDescriptor, 8> kCatalogSurfaces{{
    {"information_schema", "INFORMATION_SCHEMA.", "catalog_projection"},
    {"duckdb_catalog", "DUCKDB_TABLES;DUCKDB_COLUMNS;DUCKDB_INDEXES;DUCKDB_FUNCTIONS", "catalog_projection"},
    {"pragma_metadata", "PRAGMA_TABLE_INFO;PRAGMA_DATABASE_LIST;PRAGMA_SHOW", "catalog_projection"},
    {"settings", "DUCKDB_SETTINGS;CURRENT_SETTING", "catalog_projection"},
    {"extensions", "DUCKDB_EXTENSIONS;INSTALL;LOAD", "policy_overlay"},
    {"secrets", "DUCKDB_SECRETS;CREATE SECRET", "security_projection"},
    {"external_files", "READ_CSV;READ_PARQUET;COPY", "policy_overlay"},
    {"macros", "DUCKDB_MACROS;CREATE MACRO", "parser_support_udr"},
}};

const std::array<SurfaceDescriptor, 12> kDiagnosticSurfaces{{
    {"parse", "DUCKDB.PARSE.INVALID_INPUT;DUCKDB.PARSE.UNSUPPORTED_SURFACE", "parser"},
    {"file", "DUCKDB.AUTHORITY.FILE_IO_DENIED", "fail_closed"},
    {"external_io", "DUCKDB.AUTHORITY.EXTERNAL_IO_DENIED", "fail_closed"},
    {"etl", "DUCKDB.EMULATION.ETL_ROUTE", "parser_support_udr"},
    {"extension", "DUCKDB.AUTHORITY.EXTENSION_DENIED", "fail_closed"},
    {"checkpoint", "DUCKDB.AUTHORITY.UNSUPPORTED_DENIED", "fail_closed"},
    {"copy", "DUCKDB.EMULATION.COPY_ROUTE", "parser_support_udr"},
    {"security", "DUCKDB.EMULATION.SECURITY_ROUTE", "parser_support_udr"},
    {"routine", "DUCKDB.EMULATION.ROUTINE_ROUTE", "parser_support_udr"},
    {"pragma", "DUCKDB.EMULATION.PRAGMA_ROUTE", "parser_support_udr"},
    {"catalog", "DUCKDB.CATALOG_OVERLAY.READ_ONLY", "fail_closed"},
    {"mga", "DUCKDB.AUTHORITY.UNSUPPORTED_DENIED", "scratchbird_mga_authority"},
}};

const scratchbird::parser::donor::DialectProfile kProfile{
    "duckdb",
    "DuckDB",
    "sbp_duckdb",
    "sbup_duckdb",
    "1.5.2",
    "DUCKDB",
    kSblrFamily,
    kPatterns,
    kDatatypeSurfaces,
    kBuiltinSurfaces,
    kCatalogSurfaces,
    kDiagnosticSurfaces,
    46,
    132,
    101,
    19,
    3,
    4,
    9,
    4,
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

std::string DuckdbPackageIdentityJson() {
  return scratchbird::parser::donor::PackageIdentityJson(kProfile);
}

std::string DuckdbSurfaceReportJson() {
  return scratchbird::parser::donor::SurfaceReportJson(kProfile);
}

} // namespace scratchbird::parser::duckdb
