// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sqlite_dialect.hpp"

#include <array>

namespace scratchbird::parser::sqlite {
namespace {

using scratchbird::parser::donor::MappingDisposition;
using scratchbird::parser::donor::OperationPattern;
using scratchbird::parser::donor::PatternMatch;
using scratchbird::parser::donor::SurfaceDescriptor;

constexpr std::string_view kSblrFamily = "sblr.donor.sqlite.profile.v1";

constexpr OperationPattern kPatterns[] = {
    {"SELECT LOAD_EXTENSION", PatternMatch::kContainsFunctionCall, "extension", "sqlite.extension.load_extension",
     MappingDisposition::kPolicyRefusal, "sqlite.policy.extension.load_extension", "",
     "", "SQLITE.AUTHORITY.EXTENSION_DENIED",
     "load_extension cannot load host code from parser authority.", true, false},
    {"LOAD_EXTENSION", PatternMatch::kContainsFunctionCall, "extension", "sqlite.extension.load_extension",
     MappingDisposition::kPolicyRefusal, "sqlite.policy.extension.load_extension", "",
     "", "SQLITE.AUTHORITY.EXTENSION_DENIED",
     "load_extension cannot load host code from parser authority.", true, false},
    {"ATTACH DATABASE", PatternMatch::kPrefix, "database_lifecycle", "sqlite.lifecycle.attach_database",
     MappingDisposition::kPolicyRefusal, "sqlite.policy.file.attach_database", "",
     "", "SQLITE.AUTHORITY.FILE_IO_DENIED",
     "ATTACH DATABASE would bind a donor file outside ScratchBird engine authority.", true, false},
    {"DETACH DATABASE", PatternMatch::kPrefix, "database_lifecycle", "sqlite.lifecycle.detach_database",
     MappingDisposition::kPolicyRefusal, "sqlite.policy.file.detach_database", "",
     "", "SQLITE.AUTHORITY.FILE_IO_DENIED",
     "DETACH DATABASE file lifecycle is not parser authority.", true, false},
    {"VACUUM INTO", PatternMatch::kPrefix, "bulk_io", "sqlite.bulk_io.vacuum_into",
     MappingDisposition::kUnsupportedRefusal, "sqlite.policy.unsupported.vacuum_into",
     "", "", "SQLITE.AUTHORITY.UNSUPPORTED_DENIED",
     "SQLite VACUUM INTO is a donor low-level utility surface and is outside donor parser authority.",
     true, false},
    {"BACKUP", PatternMatch::kPrefix, "bulk_io", "sqlite.bulk_io.backup",
     MappingDisposition::kPolicyRefusal, "sqlite.policy.file.backup", "",
     "", "SQLITE.AUTHORITY.FILE_IO_DENIED",
     "SQLite backup file effects are blocked from parser authority.", true, false},
    {"RESTORE", PatternMatch::kPrefix, "bulk_io", "sqlite.bulk_io.restore",
     MappingDisposition::kPolicyRefusal, "sqlite.policy.file.restore", "",
     "", "SQLITE.AUTHORITY.FILE_IO_DENIED",
     "SQLite restore file effects require trusted engine lifecycle admission.", true, false},
    {"PRAGMA " "JOURNAL_MODE", PatternMatch::kPrefix, "pragma", "sqlite.pragma.journal_mode",
     MappingDisposition::kParserSupportUdr, "sqlite.udr.pragma.journal_mode",
     "SBLR_DONOR_SQLITE_PRAGMA_ROUTE", "ParserSupportPragmaRoute",
     "SQLITE.EMULATION.PRAGMA_ROUTE",
     "SQLite journal mode is interpreted as compatibility metadata; ScratchBird MGA remains authority.", true, false},
    {"PRAGMA WAL_", PatternMatch::kPrefix, "pragma", "sqlite.pragma.wal_control",
     MappingDisposition::kParserSupportUdr, "sqlite.udr.pragma.wal_control",
     "SBLR_DONOR_SQLITE_PRAGMA_ROUTE", "ParserSupportPragmaRoute",
     "SQLITE.EMULATION.PRAGMA_ROUTE",
     "SQLite WAL pragmas are compatibility metadata only; ScratchBird recovery authority is unchanged.", true, false},
    {"PRAGMA", PatternMatch::kPrefix, "pragma", "sqlite.pragma.generic",
     MappingDisposition::kParserSupportUdr, "sqlite.udr.pragma.generic",
     "SBLR_DONOR_SQLITE_PRAGMA_ROUTE", "ParserSupportPragmaRoute",
     "SQLITE.EMULATION.PRAGMA_ROUTE",
     "SQLite PRAGMA routes through trusted parser-support policy.", true, false},
    {"CREATE TEMP", PatternMatch::kPrefix, "ddl", "sqlite.ddl.create_temp",
     MappingDisposition::kAdmittedSblr, "sqlite.ddl.create_temp",
     "SBLR_DONOR_SQLITE_DDL_CREATE_TEMP", "EngineDdlCreateTemp", "", "", true, true},
    {"CREATE VIRTUAL TABLE", PatternMatch::kPrefix, "virtual_table", "sqlite.virtual_table.create",
     MappingDisposition::kParserSupportUdr, "sqlite.udr.virtual_table.create",
     "SBLR_DONOR_SQLITE_VIRTUAL_TABLE_ROUTE", "ParserSupportVirtualTableRoute",
     "SQLITE.EMULATION.VIRTUAL_TABLE_ROUTE",
     "Virtual table modules require trusted package admission.", true, true},
    {"CREATE TRIGGER", PatternMatch::kPrefix, "routine", "sqlite.routine.trigger.create",
     MappingDisposition::kParserSupportUdr, "sqlite.udr.routine.trigger.create",
     "SBLR_DONOR_SQLITE_ROUTINE_ROUTE", "ParserSupportRoutineRoute",
     "SQLITE.EMULATION.ROUTINE_ROUTE",
     "Triggers route through trusted routine package policy.", true, false},
    {"DROP TRIGGER", PatternMatch::kPrefix, "routine", "sqlite.routine.trigger.drop",
     MappingDisposition::kParserSupportUdr, "sqlite.udr.routine.trigger.drop",
     "SBLR_DONOR_SQLITE_ROUTINE_ROUTE", "ParserSupportRoutineRoute",
     "SQLITE.EMULATION.ROUTINE_ROUTE",
     "Trigger removal routes through trusted routine package policy.", true, true},
    {"CREATE VIEW", PatternMatch::kPrefix, "ddl", "sqlite.ddl.create_view",
     MappingDisposition::kAdmittedSblr, "sqlite.ddl.create_view",
     "SBLR_DONOR_SQLITE_DDL_CREATE_VIEW", "EngineDdlCreateView", "", "", true, true},
    {"DROP VIEW", PatternMatch::kPrefix, "ddl", "sqlite.ddl.drop_view",
     MappingDisposition::kAdmittedSblr, "sqlite.ddl.drop_view",
     "SBLR_DONOR_SQLITE_DDL_DROP_VIEW", "EngineDdlDropView", "", "", true, true},
    {"CREATE INDEX", PatternMatch::kPrefix, "ddl", "sqlite.ddl.create_index",
     MappingDisposition::kAdmittedSblr, "sqlite.ddl.create_index",
     "SBLR_DONOR_SQLITE_DDL_CREATE_INDEX", "EngineDdlCreateIndex", "", "", true, true},
    {"DROP INDEX", PatternMatch::kPrefix, "ddl", "sqlite.ddl.drop_index",
     MappingDisposition::kAdmittedSblr, "sqlite.ddl.drop_index",
     "SBLR_DONOR_SQLITE_DDL_DROP_INDEX", "EngineDdlDropIndex", "", "", true, true},
    {"EXPLAIN QUERY PLAN", PatternMatch::kPrefix, "optimizer", "sqlite.optimizer.explain_query_plan",
     MappingDisposition::kCatalogProjection, "sqlite.optimizer.explain_query_plan",
     "SBLR_DONOR_SQLITE_EXPLAIN", "EngineExplainPlan", "", "", false, false},
    {"EXPLAIN", PatternMatch::kPrefix, "optimizer", "sqlite.optimizer.explain",
     MappingDisposition::kCatalogProjection, "sqlite.optimizer.explain",
     "SBLR_DONOR_SQLITE_EXPLAIN", "EngineExplainPlan", "", "", false, false},
    {"ANALYZE", PatternMatch::kPrefix, "maintenance", "sqlite.maintenance.analyze",
     MappingDisposition::kUnsupportedRefusal, "sqlite.policy.unsupported.analyze",
     "", "", "SQLITE.AUTHORITY.UNSUPPORTED_DENIED",
     "SQLite ANALYZE is a donor low-level utility surface and is outside donor parser authority.",
     true, false},
    {"VACUUM", PatternMatch::kPrefix, "maintenance", "sqlite.maintenance.vacuum",
     MappingDisposition::kUnsupportedRefusal, "sqlite.policy.unsupported.vacuum",
     "", "", "SQLITE.AUTHORITY.UNSUPPORTED_DENIED",
     "SQLite VACUUM is a donor low-level utility surface and is outside donor parser authority.",
     true, false},
    {"REINDEX", PatternMatch::kPrefix, "maintenance", "sqlite.maintenance.reindex",
     MappingDisposition::kUnsupportedRefusal, "sqlite.policy.unsupported.reindex",
     "", "", "SQLITE.AUTHORITY.UNSUPPORTED_DENIED",
     "SQLite REINDEX is a donor low-level utility surface and is outside donor parser authority.",
     true, false},
    {"BEGIN", PatternMatch::kPrefix, "transaction", "sqlite.transaction.begin",
     MappingDisposition::kAdmittedSblr, "sqlite.transaction.begin",
     "SBLR_TRANSACTION_BEGIN", "EngineBeginTransaction", "", "", false, false},
    {"COMMIT", PatternMatch::kPrefix, "transaction", "sqlite.transaction.commit",
     MappingDisposition::kAdmittedSblr, "sqlite.transaction.commit",
     "SBLR_TRANSACTION_COMMIT", "EngineCommitTransaction", "", "", false, true},
    {"ROLLBACK", PatternMatch::kPrefix, "transaction", "sqlite.transaction.rollback",
     MappingDisposition::kAdmittedSblr, "sqlite.transaction.rollback",
     "SBLR_TRANSACTION_ROLLBACK", "EngineRollbackTransaction", "", "", false, true},
    {"SAVEPOINT", PatternMatch::kPrefix, "transaction", "sqlite.transaction.savepoint",
     MappingDisposition::kAdmittedSblr, "sqlite.transaction.savepoint",
     "SBLR_TRANSACTION_SAVEPOINT", "EngineSavepoint", "", "", false, true},
    {"RELEASE SAVEPOINT", PatternMatch::kPrefix, "transaction", "sqlite.transaction.release_savepoint",
     MappingDisposition::kAdmittedSblr, "sqlite.transaction.release_savepoint",
     "SBLR_TRANSACTION_RELEASE_SAVEPOINT", "EngineReleaseSavepoint", "", "", false, true},
    {"CREATE", PatternMatch::kPrefix, "ddl", "sqlite.ddl.create",
     MappingDisposition::kAdmittedSblr, "sqlite.ddl.create",
     "SBLR_DONOR_SQLITE_DDL_CREATE", "EngineDdlCreate", "", "", true, true},
    {"ALTER", PatternMatch::kPrefix, "ddl", "sqlite.ddl.alter",
     MappingDisposition::kAdmittedSblr, "sqlite.ddl.alter",
     "SBLR_DONOR_SQLITE_DDL_ALTER", "EngineDdlAlter", "", "", true, true},
    {"DROP", PatternMatch::kPrefix, "ddl", "sqlite.ddl.drop",
     MappingDisposition::kAdmittedSblr, "sqlite.ddl.drop",
     "SBLR_DONOR_SQLITE_DDL_DROP", "EngineDdlDrop", "", "", true, true},
    {"REPLACE", PatternMatch::kPrefix, "dml", "sqlite.dml.replace",
     MappingDisposition::kAdmittedSblr, "sqlite.dml.replace",
     "SBLR_DONOR_SQLITE_REPLACE", "EngineDmlReplace", "", "", false, true},
    {"INSERT", PatternMatch::kPrefix, "dml", "sqlite.dml.insert",
     MappingDisposition::kAdmittedSblr, "sqlite.dml.insert",
     "SBLR_DONOR_SQLITE_INSERT", "EngineDmlInsert", "", "", false, true},
    {"UPDATE", PatternMatch::kPrefix, "dml", "sqlite.dml.update",
     MappingDisposition::kAdmittedSblr, "sqlite.dml.update",
     "SBLR_DONOR_SQLITE_UPDATE", "EngineDmlUpdate", "", "", false, true},
    {"DELETE", PatternMatch::kPrefix, "dml", "sqlite.dml.delete",
     MappingDisposition::kAdmittedSblr, "sqlite.dml.delete",
     "SBLR_DONOR_SQLITE_DELETE", "EngineDmlDelete", "", "", false, true},
    {"SELECT", PatternMatch::kPrefix, "query", "sqlite.query.select",
     MappingDisposition::kAdmittedSblr, "sqlite.query.select",
     "SBLR_DONOR_SQLITE_SELECT", "EngineQuerySelect", "", "", false, false},
    {"WITH", PatternMatch::kPrefix, "query", "sqlite.query.with",
     MappingDisposition::kAdmittedSblr, "sqlite.query.with",
     "SBLR_DONOR_SQLITE_SELECT", "EngineQuerySelect", "", "", false, false},
};

const std::array<SurfaceDescriptor, 8> kDatatypeSurfaces{{
    {"affinity_integer", "INTEGER;INT;TINYINT;SMALLINT;MEDIUMINT;BIGINT;UNSIGNED BIG INT;INT2;INT8", "descriptor_affinity"},
    {"affinity_real", "REAL;DOUBLE;DOUBLE PRECISION;FLOAT", "descriptor_affinity"},
    {"affinity_numeric", "NUMERIC;DECIMAL;BOOLEAN;DATE;DATETIME", "descriptor_affinity"},
    {"affinity_text", "TEXT;CHARACTER;VARCHAR;VARYING CHARACTER;NCHAR;NATIVE CHARACTER;NVARCHAR;CLOB", "descriptor_affinity"},
    {"affinity_blob", "BLOB", "descriptor_affinity"},
    {"json", "JSON;JSON_EXTRACT;JSONB_*", "parser_support_udr"},
    {"rowid", "ROWID;OID;_ROWID_", "catalog_projection"},
    {"collation", "COLLATE;BINARY;NOCASE;RTRIM", "catalog_policy"},
}};

const std::array<SurfaceDescriptor, 10> kBuiltinSurfaces{{
    {"aggregate", "COUNT;SUM;AVG;MIN;MAX;TOTAL;GROUP_CONCAT;STRING_AGG", "sblr"},
    {"window", "ROW_NUMBER;RANK;DENSE_RANK;LAG;LEAD", "sblr"},
    {"string", "SUBSTR;SUBSTRING;LOWER;UPPER;TRIM;LTRIM;RTRIM;LENGTH;PRINTF;FORMAT", "sblr"},
    {"numeric", "ABS;ROUND;POWER;SQRT;MOD;RANDOM;SIGN", "sblr"},
    {"temporal", "DATE;TIME;DATETIME;JULIANDAY;STRFTIME;UNIXEPOCH", "sblr"},
    {"json", "JSON;JSON_ARRAY;JSON_OBJECT;JSON_EXTRACT;JSON_SET;JSON_PATCH", "parser_support_udr"},
    {"fts", "MATCH;HIGHLIGHT;SNIPPET;BM25", "parser_support_udr"},
    {"blob", "ZEROBLOB;HEX;UNHEX;QUOTE", "sblr"},
    {"session", "LAST_INSERT_ROWID;CHANGES;TOTAL_CHANGES", "engine_context"},
    {"extension", "LOAD_EXTENSION", "fail_closed"},
}};

const std::array<SurfaceDescriptor, 7> kCatalogSurfaces{{
    {"schema", "SQLITE_SCHEMA;SQLITE_MASTER;SQLITE_TEMP_SCHEMA;SQLITE_TEMP_MASTER", "catalog_projection"},
    {"pragma_table", "PRAGMA_TABLE_INFO;PRAGMA_TABLE_XINFO;PRAGMA_INDEX_LIST;PRAGMA_INDEX_INFO", "catalog_projection"},
    {"pragma_runtime", "PRAGMA_DATABASE_LIST;PRAGMA_COLLATION_LIST;PRAGMA_FUNCTION_LIST;PRAGMA_MODULE_LIST", "catalog_projection"},
    {"internal_tables", "SQLITE_SEQUENCE;SQLITE_STAT1;SQLITE_STAT4", "catalog_projection"},
    {"virtual_tables", "FTS3;FTS4;FTS5;RTREE", "parser_support_udr"},
    {"rowid_aliases", "ROWID;OID;_ROWID_", "catalog_projection"},
    {"extension_catalog", "LOAD_EXTENSION;CREATE VIRTUAL TABLE", "policy_overlay"},
}};

const std::array<SurfaceDescriptor, 9> kDiagnosticSurfaces{{
    {"parse", "SQLITE.PARSE.INVALID_INPUT;SQLITE.PARSE.UNSUPPORTED_SURFACE", "parser"},
    {"file", "SQLITE.AUTHORITY.FILE_IO_DENIED", "fail_closed"},
    {"extension", "SQLITE.AUTHORITY.EXTENSION_DENIED", "fail_closed"},
    {"pragma", "SQLITE.EMULATION.PRAGMA_ROUTE", "parser_support_udr"},
    {"virtual_table", "SQLITE.EMULATION.VIRTUAL_TABLE_ROUTE", "parser_support_udr"},
    {"routine", "SQLITE.EMULATION.ROUTINE_ROUTE", "parser_support_udr"},
    {"maintenance", "SQLITE.AUTHORITY.UNSUPPORTED_DENIED", "fail_closed"},
    {"catalog", "SQLITE.CATALOG_OVERLAY.READ_ONLY", "fail_closed"},
    {"mga", "SQLITE.EMULATION.PRAGMA_ROUTE", "scratchbird_mga_authority"},
}};

const scratchbird::parser::donor::DialectProfile kProfile{
    "sqlite",
    "SQLite",
    "sbp_sqlite",
    "sbup_sqlite",
    "3.53.0",
    "SQLITE",
    kSblrFamily,
    kPatterns,
    kDatatypeSurfaces,
    kBuiltinSurfaces,
    kCatalogSurfaces,
    kDiagnosticSurfaces,
    43,
    96,
    84,
    12,
    11,
    0,
    7,
    5,
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

std::string SqlitePackageIdentityJson() {
  return scratchbird::parser::donor::PackageIdentityJson(kProfile);
}

std::string SqliteSurfaceReportJson() {
  return scratchbird::parser::donor::SurfaceReportJson(kProfile);
}

} // namespace scratchbird::parser::sqlite
