// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "immudb_dialect.hpp"

#include <array>

namespace scratchbird::parser::immudb {
namespace {

using scratchbird::parser::donor::MappingDisposition;
using scratchbird::parser::donor::OperationPattern;
using scratchbird::parser::donor::PatternMatch;
using scratchbird::parser::donor::SurfaceDescriptor;

constexpr std::string_view kSblrFamily = "sblr.donor.immudb.profile.v1";

constexpr OperationPattern kPatterns[] = {
    {"CREATE DATABASE", PatternMatch::kPrefix, "database_lifecycle", "immudb.lifecycle.create_database",
     MappingDisposition::kScratchBirdLifecycleApi, "immudb.lifecycle.create_database",
     "SBLR_LIFECYCLE_CREATE_DATABASE", "EngineCreateLifecycle", "",
     "", true, false},
    {"DROP DATABASE", PatternMatch::kPrefix, "database_lifecycle", "immudb.lifecycle.drop_database",
     MappingDisposition::kScratchBirdLifecycleApi, "immudb.lifecycle.drop_database",
     "SBLR_LIFECYCLE_DROP_DATABASE", "EngineDropLifecycle", "",
     "", true, false},
    {"ALTER DATABASE", PatternMatch::kPrefix, "database_lifecycle", "immudb.lifecycle.alter_database",
     MappingDisposition::kParserSupportUdr, "immudb.udr.database.alter",
     "SBLR_DONOR_IMMUDB_DATABASE_ROUTE", "ParserSupportDatabaseRoute", "IMMUDB.EMULATION.DATABASE_ROUTE",
     "immudb database presentation updates route through trusted parser support and catalog policy.", true, true},
    {"USE DATABASE", PatternMatch::kPrefix, "session", "immudb.session.use_database",
     MappingDisposition::kAdmittedSblr, "immudb.session.use_database",
     "SBLR_DONOR_IMMUDB_USE_DATABASE", "EngineSessionRoute", "",
     "", false, false},
    {"USE", PatternMatch::kPrefix, "session", "immudb.session.use_database",
     MappingDisposition::kAdmittedSblr, "immudb.session.use_database",
     "SBLR_DONOR_IMMUDB_USE_DATABASE", "EngineSessionRoute", "",
     "", false, false},
    {"SHOW DATABASES", PatternMatch::kPrefix, "catalog_overlay", "immudb.catalog.show_databases",
     MappingDisposition::kCatalogProjection, "immudb.catalog.show_databases",
     "SBLR_DONOR_IMMUDB_CATALOG_PROJECT", "EngineCatalogProjection", "",
     "", false, false},
    {"SHOW TABLES", PatternMatch::kPrefix, "catalog_overlay", "immudb.catalog.show_tables",
     MappingDisposition::kCatalogProjection, "immudb.catalog.show_tables",
     "SBLR_DONOR_IMMUDB_CATALOG_PROJECT", "EngineCatalogProjection", "",
     "", false, false},
    {"SHOW TABLE", PatternMatch::kPrefix, "catalog_overlay", "immudb.catalog.show_table",
     MappingDisposition::kCatalogProjection, "immudb.catalog.show_table",
     "SBLR_DONOR_IMMUDB_CATALOG_PROJECT", "EngineCatalogProjection", "",
     "", false, false},
    {"DATABASES", PatternMatch::kContainsFunctionCall, "catalog_overlay", "immudb.catalog.databases",
     MappingDisposition::kCatalogProjection, "immudb.catalog.databases",
     "SBLR_DONOR_IMMUDB_CATALOG_PROJECT", "EngineCatalogProjection", "",
     "", false, false},
    {"TABLES", PatternMatch::kContainsFunctionCall, "catalog_overlay", "immudb.catalog.tables",
     MappingDisposition::kCatalogProjection, "immudb.catalog.tables",
     "SBLR_DONOR_IMMUDB_CATALOG_PROJECT", "EngineCatalogProjection", "",
     "", false, false},
    {"COLUMNS", PatternMatch::kContainsFunctionCall, "catalog_overlay", "immudb.catalog.columns",
     MappingDisposition::kCatalogProjection, "immudb.catalog.columns",
     "SBLR_DONOR_IMMUDB_CATALOG_PROJECT", "EngineCatalogProjection", "",
     "", false, false},
    {"INDEXES", PatternMatch::kContainsFunctionCall, "catalog_overlay", "immudb.catalog.indexes",
     MappingDisposition::kCatalogProjection, "immudb.catalog.indexes",
     "SBLR_DONOR_IMMUDB_CATALOG_PROJECT", "EngineCatalogProjection", "",
     "", false, false},
    {"VERIFIED_SET", PatternMatch::kPrefix, "kv_write", "immudb.kv.verified_set",
     MappingDisposition::kParserSupportUdr, "immudb.udr.verified_set",
     "SBLR_DONOR_IMMUDB_VERIFIED_KV_ROUTE", "ParserSupportVerifiedKvRoute", "IMMUDB.EMULATION.VERIFIED_KV_ROUTE",
     "immudb verified writes require trusted parser support for proof-result rendering; MGA remains engine-owned.", true, true},
    {"SET_REFERENCE", PatternMatch::kPrefix, "kv_write", "immudb.kv.set_reference",
     MappingDisposition::kParserSupportUdr, "immudb.udr.set_reference",
     "SBLR_DONOR_IMMUDB_REFERENCE_ROUTE", "ParserSupportReferenceRoute", "IMMUDB.EMULATION.REFERENCE_ROUTE",
     "immudb reference writes route through trusted parser support and engine descriptor policy.", true, true},
    {"SET", PatternMatch::kPrefix, "kv_write", "immudb.kv.set",
     MappingDisposition::kAdmittedSblr, "immudb.kv.set",
     "SBLR_DONOR_IMMUDB_SET", "EngineKvSet", "",
     "", false, true},
    {"VERIFIED_GET", PatternMatch::kPrefix, "kv_read", "immudb.kv.verified_get",
     MappingDisposition::kParserSupportUdr, "immudb.udr.verified_get",
     "SBLR_DONOR_IMMUDB_VERIFIED_KV_ROUTE", "ParserSupportVerifiedKvRoute", "IMMUDB.EMULATION.VERIFIED_KV_ROUTE",
     "immudb verified reads route through trusted parser support for donor-compatible proof rendering.", true, false},
    {"GET", PatternMatch::kPrefix, "kv_read", "immudb.kv.get",
     MappingDisposition::kAdmittedSblr, "immudb.kv.get",
     "SBLR_DONOR_IMMUDB_GET", "EngineKvGet", "",
     "", false, false},
    {"SCAN", PatternMatch::kPrefix, "kv_read", "immudb.kv.scan",
     MappingDisposition::kAdmittedSblr, "immudb.kv.scan",
     "SBLR_DONOR_IMMUDB_SCAN", "EngineKvScan", "",
     "", false, false},
    {"HISTORY", PatternMatch::kPrefix, "kv_read", "immudb.kv.history",
     MappingDisposition::kCatalogProjection, "immudb.catalog.history",
     "SBLR_DONOR_IMMUDB_CATALOG_PROJECT", "EngineCatalogProjection", "",
     "", false, false},
    {"CURRENT_STATE", PatternMatch::kPrefix, "state", "immudb.state.current",
     MappingDisposition::kCatalogProjection, "immudb.catalog.current_state",
     "SBLR_DONOR_IMMUDB_CATALOG_PROJECT", "EngineCatalogProjection", "",
     "", false, false},
    {"TX_BY_ID", PatternMatch::kPrefix, "state", "immudb.state.tx_by_id",
     MappingDisposition::kCatalogProjection, "immudb.catalog.tx_by_id",
     "SBLR_DONOR_IMMUDB_CATALOG_PROJECT", "EngineCatalogProjection", "",
     "", false, false},
    {"ZADD", PatternMatch::kPrefix, "kv_write", "immudb.sorted_set.zadd",
     MappingDisposition::kParserSupportUdr, "immudb.udr.sorted_set.zadd",
     "SBLR_DONOR_IMMUDB_SORTED_SET_ROUTE", "ParserSupportSortedSetRoute", "IMMUDB.EMULATION.SORTED_SET_ROUTE",
     "immudb sorted-set compatibility routes through trusted parser support.", true, true},
    {"ZSCAN", PatternMatch::kPrefix, "kv_read", "immudb.sorted_set.zscan",
     MappingDisposition::kParserSupportUdr, "immudb.udr.sorted_set.zscan",
     "SBLR_DONOR_IMMUDB_SORTED_SET_ROUTE", "ParserSupportSortedSetRoute", "IMMUDB.EMULATION.SORTED_SET_ROUTE",
     "immudb sorted-set scan compatibility routes through trusted parser support.", true, false},
    {"CREATE USER", PatternMatch::kPrefix, "security", "immudb.security.create_user",
     MappingDisposition::kParserSupportUdr, "immudb.udr.security.create_user",
     "SBLR_DONOR_IMMUDB_SECURITY_ROUTE", "ParserSupportSecurityRoute", "IMMUDB.EMULATION.SECURITY_ROUTE",
     "immudb user management routes through trusted security policy.", true, false},
    {"GRANT", PatternMatch::kPrefix, "security", "immudb.security.grant",
     MappingDisposition::kParserSupportUdr, "immudb.udr.security.grant",
     "SBLR_DONOR_IMMUDB_SECURITY_ROUTE", "ParserSupportSecurityRoute", "IMMUDB.EMULATION.SECURITY_ROUTE",
     "immudb privilege changes route through trusted security policy.", true, true},
    {"REVOKE", PatternMatch::kPrefix, "security", "immudb.security.revoke",
     MappingDisposition::kParserSupportUdr, "immudb.udr.security.revoke",
     "SBLR_DONOR_IMMUDB_SECURITY_ROUTE", "ParserSupportSecurityRoute", "IMMUDB.EMULATION.SECURITY_ROUTE",
     "immudb privilege changes route through trusted security policy.", true, true},
    {"DUMP", PatternMatch::kPrefix, "external_authority", "immudb.backup.dump",
     MappingDisposition::kPolicyRefusal, "immudb.policy.backup.dump",
     "", "", "IMMUDB.AUTHORITY.BACKUP_DENIED",
     "immudb backup/export requires external file authority and is blocked from parser authority.", true, false},
    {"RESTORE", PatternMatch::kPrefix, "external_authority", "immudb.backup.restore",
     MappingDisposition::kPolicyRefusal, "immudb.policy.backup.restore",
     "", "", "IMMUDB.AUTHORITY.BACKUP_DENIED",
     "immudb restore requires external file authority and is blocked from parser authority.", true, false},
    {"REPLICATION", PatternMatch::kPrefix, "external_authority", "immudb.replication.admin",
     MappingDisposition::kParserSupportUdr, "immudb.udr.replication.admin",
     "SBLR_DONOR_IMMUDB_REPLICATION_ROUTE", "ParserSupportReplicationRoute",
     "IMMUDB.EMULATION.REPLICATION_ROUTE",
     "immudb replication administration routes through the immudb donor UDR.", true, false},
    {"REPLICATE", PatternMatch::kPrefix, "external_authority", "immudb.replication.apply",
     MappingDisposition::kParserSupportUdr, "immudb.udr.replication.apply",
     "SBLR_DONOR_IMMUDB_REPLICATION_ROUTE", "ParserSupportReplicationRoute",
     "IMMUDB.EMULATION.REPLICATION_ROUTE",
     "immudb replication event application routes through the immudb donor UDR.", true, false},
    {"CREATE TABLE", PatternMatch::kPrefix, "ddl", "immudb.ddl.create_table",
     MappingDisposition::kAdmittedSblr, "immudb.ddl.create_table",
     "SBLR_DONOR_IMMUDB_DDL_CREATE", "EngineDdlCreate", "",
     "", true, true},
    {"CREATE INDEX", PatternMatch::kPrefix, "ddl", "immudb.ddl.create_index",
     MappingDisposition::kAdmittedSblr, "immudb.ddl.create_index",
     "SBLR_DONOR_IMMUDB_DDL_CREATE_INDEX", "EngineDdlCreateIndex", "",
     "", true, true},
    {"ALTER TABLE", PatternMatch::kPrefix, "ddl", "immudb.ddl.alter_table",
     MappingDisposition::kAdmittedSblr, "immudb.ddl.alter_table",
     "SBLR_DONOR_IMMUDB_DDL_ALTER", "EngineDdlAlter", "",
     "", true, true},
    {"DROP TABLE", PatternMatch::kPrefix, "ddl", "immudb.ddl.drop_table",
     MappingDisposition::kAdmittedSblr, "immudb.ddl.drop_table",
     "SBLR_DONOR_IMMUDB_DDL_DROP", "EngineDdlDrop", "",
     "", true, true},
    {"INSERT", PatternMatch::kPrefix, "dml", "immudb.dml.insert",
     MappingDisposition::kAdmittedSblr, "immudb.dml.insert",
     "SBLR_DONOR_IMMUDB_INSERT", "EngineDmlInsert", "",
     "", false, true},
    {"UPSERT", PatternMatch::kPrefix, "dml", "immudb.dml.upsert",
     MappingDisposition::kAdmittedSblr, "immudb.dml.upsert",
     "SBLR_DONOR_IMMUDB_UPSERT", "EngineDmlUpsert", "",
     "", false, true},
    {"UPDATE", PatternMatch::kPrefix, "dml", "immudb.dml.update",
     MappingDisposition::kAdmittedSblr, "immudb.dml.update",
     "SBLR_DONOR_IMMUDB_UPDATE", "EngineDmlUpdate", "",
     "", false, true},
    {"DELETE", PatternMatch::kPrefix, "dml", "immudb.dml.delete",
     MappingDisposition::kAdmittedSblr, "immudb.dml.delete",
     "SBLR_DONOR_IMMUDB_DELETE", "EngineDmlDelete", "",
     "", false, true},
    {"SELECT", PatternMatch::kPrefix, "query", "immudb.query.select",
     MappingDisposition::kAdmittedSblr, "immudb.query.select",
     "SBLR_DONOR_IMMUDB_SELECT", "EngineQuerySelect", "",
     "", false, false},
    {"WITH", PatternMatch::kPrefix, "query", "immudb.query.with",
     MappingDisposition::kAdmittedSblr, "immudb.query.with",
     "SBLR_DONOR_IMMUDB_SELECT", "EngineQuerySelect", "",
     "", false, false},
    {"BEGIN", PatternMatch::kPrefix, "transaction", "immudb.transaction.begin",
     MappingDisposition::kAdmittedSblr, "immudb.transaction.begin",
     "SBLR_TRANSACTION_BEGIN", "EngineBeginTransaction", "",
     "", false, false},
    {"COMMIT", PatternMatch::kPrefix, "transaction", "immudb.transaction.commit",
     MappingDisposition::kAdmittedSblr, "immudb.transaction.commit",
     "SBLR_TRANSACTION_COMMIT", "EngineCommitTransaction", "",
     "", false, true},
    {"ROLLBACK", PatternMatch::kPrefix, "transaction", "immudb.transaction.rollback",
     MappingDisposition::kAdmittedSblr, "immudb.transaction.rollback",
     "SBLR_TRANSACTION_ROLLBACK", "EngineRollbackTransaction", "",
     "", false, true},
};

const std::array<SurfaceDescriptor, 8> kDatatypeSurfaces{{
    {"scalar", "BOOLEAN;INTEGER;FLOAT;VARCHAR;BLOB;UUID", "descriptor"},
    {"temporal", "TIMESTAMP;DATE;TIME", "descriptor"},
    {"json", "JSON;DOCUMENT", "parser_support_udr"},
    {"kv", "KEY;VALUE;REVISION;REFERENCE", "descriptor_policy"},
    {"proof", "VERIFIED_PROOF;DUAL_PROOF;TX_HEADER", "parser_support_udr"},
    {"sorted_set", "SET_KEY;SET_SCORE;SET_MEMBER", "parser_support_udr"},
    {"catalog", "DATABASES;TABLES;COLUMNS;INDEXES", "catalog_projection"},
    {"security", "USER;PERMISSION;ROLE", "parser_support_udr"},
}};

const std::array<SurfaceDescriptor, 8> kBuiltinSurfaces{{
    {"sql", "SELECT;INSERT;UPSERT;UPDATE;DELETE;CREATE TABLE;CREATE INDEX", "sblr"},
    {"kv", "SET;GET;SCAN;HISTORY;TX_BY_ID;CURRENT_STATE", "sblr"},
    {"verified_kv", "VERIFIED_SET;VERIFIED_GET", "parser_support_udr"},
    {"sorted_set", "ZADD;ZSCAN", "parser_support_udr"},
    {"catalog", "DATABASES;TABLES;COLUMNS;INDEXES;SHOW DATABASES;SHOW TABLES;SHOW TABLE", "catalog_projection"},
    {"security", "CREATE USER;GRANT;REVOKE", "parser_support_udr"},
    {"backup", "DUMP;RESTORE", "fail_closed"},
    {"replication", "REPLICATION;REPLICATE", "parser_support_udr"},
}};

const std::array<SurfaceDescriptor, 7> kCatalogSurfaces{{
    {"pg_type", "PG_TYPE", "catalog_projection"},
    {"databases", "DATABASES", "catalog_projection"},
    {"tables", "TABLES", "catalog_projection"},
    {"columns", "COLUMNS", "catalog_projection"},
    {"indexes", "INDEXES", "catalog_projection"},
    {"state", "CURRENT_STATE;TX_BY_ID;HISTORY", "catalog_projection"},
    {"security", "IMMUDB_USERS;IMMUDB_PERMISSIONS", "catalog_projection"},
}};

const std::array<SurfaceDescriptor, 10> kDiagnosticSurfaces{{
    {"parse", "IMMUDB.PARSE.INVALID_INPUT;IMMUDB.PARSE.UNSUPPORTED_SURFACE", "parser"},
    {"policy", "IMMUDB.AUTHORITY.*", "fail_closed"},
    {"udr", "IMMUDB.EMULATION.*", "parser_support_udr"},
    {"catalog", "IMMUDB.CATALOG_OVERLAY.READ_ONLY", "catalog_projection"},
    {"session", "IMMUDB.SESSION.*", "sblr"},
    {"transaction", "IMMUDB.TRANSACTION.*", "sblr"},
    {"file_effects", "real_donor_file_effects=false", "authority_invariant"},
    {"donor_execution", "donor_engine_sql_executed=false", "authority_invariant"},
    {"mga", "parser_transaction_finality_authority=false", "authority_invariant"},
    {"support_bundle", "source_text_redacted=true", "diagnostic_redaction"},
}};

const scratchbird::parser::donor::DialectProfile kProfile{
    "immudb",
    "immudb",
    "sbp_immudb",
    "sbup_immudb",
    "1.11.0",
    "IMMUDB",
    kSblrFamily,
    kPatterns,
    kDatatypeSurfaces,
    kBuiltinSurfaces,
    kCatalogSurfaces,
    kDiagnosticSurfaces,
    19,
    7,
    0,
    0,
    7,
    0,
    0,
    0,
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
std::string ImmudbPackageIdentityJson() { return scratchbird::parser::donor::PackageIdentityJson(kProfile); }
std::string ImmudbSurfaceReportJson() { return scratchbird::parser::donor::SurfaceReportJson(kProfile); }

} // namespace scratchbird::parser::immudb
