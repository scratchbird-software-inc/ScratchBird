// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cockroachdb_dialect.hpp"

#include <array>

namespace scratchbird::parser::cockroachdb {
namespace {

using scratchbird::parser::donor::MappingDisposition;
using scratchbird::parser::donor::OperationPattern;
using scratchbird::parser::donor::PatternMatch;
using scratchbird::parser::donor::SurfaceDescriptor;

constexpr std::string_view kSblrFamily = "sblr.donor.cockroachdb.profile.v1";

constexpr OperationPattern kPatterns[] = {
    {"BACKUP", PatternMatch::kPrefix, "backup_restore", "cockroachdb.backup.backup",
     MappingDisposition::kPolicyRefusal, "cockroachdb.policy.backup",
     "", "", "COCKROACHDB.AUTHORITY.BACKUP_DENIED",
     "Backup routes require trusted migration service admission.", true, false},
    {"RESTORE", PatternMatch::kPrefix, "backup_restore", "cockroachdb.restore.restore",
     MappingDisposition::kPolicyRefusal, "cockroachdb.policy.restore",
     "", "", "COCKROACHDB.AUTHORITY.RESTORE_DENIED",
     "Restore routes require trusted migration service admission.", true, false},
    {"IMPORT", PatternMatch::kPrefix, "bulk_io", "cockroachdb.bulk_io.import",
     MappingDisposition::kPolicyRefusal, "cockroachdb.policy.import",
     "", "", "COCKROACHDB.AUTHORITY.FILE_IO_DENIED",
     "IMPORT requires trusted import service admission.", true, false},
    {"CREATE CHANGEFEED", PatternMatch::kPrefix, "changefeed", "cockroachdb.changefeed.create",
     MappingDisposition::kParserSupportUdr, "cockroachdb.udr.changefeed.create",
     "SBLR_DONOR_COCKROACHDB_CHANGEFEED_ROUTE", "ParserSupportChangefeedRoute",
     "COCKROACHDB.EMULATION.CHANGEFEED_ROUTE",
     "Changefeed requests route through the CockroachDB donor UDR.", true, false},
    {"SHOW RANGES", PatternMatch::kPrefix, "catalog_overlay", "cockroachdb.catalog_overlay.show_ranges",
     MappingDisposition::kCatalogProjection, "cockroachdb.catalog.show_ranges",
     "SBLR_DONOR_COCKROACHDB_CATALOG_PROJECT", "EngineCatalogProjection", "",
     "", false, false},
    {"SHOW JOBS", PatternMatch::kPrefix, "catalog_overlay", "cockroachdb.catalog_overlay.show_jobs",
     MappingDisposition::kCatalogProjection, "cockroachdb.catalog.show_jobs",
     "SBLR_DONOR_COCKROACHDB_CATALOG_PROJECT", "EngineCatalogProjection", "",
     "", false, false},
    {"ALTER RANGE", PatternMatch::kPrefix, "zone_config", "cockroachdb.zone_config.alter_range",
     MappingDisposition::kParserSupportUdr, "cockroachdb.udr.zone_config.alter_range",
     "SBLR_DONOR_COCKROACHDB_ZONE_ROUTE", "ParserSupportZoneRoute", "COCKROACHDB.EMULATION.ZONE_ROUTE",
     "Range configuration routes through trusted placement policy.", true, false},
    {"ALTER TABLE", PatternMatch::kPrefix, "ddl", "cockroachdb.ddl.alter_table",
     MappingDisposition::kAdmittedSblr, "cockroachdb.ddl.alter_table",
     "SBLR_DONOR_COCKROACHDB_DDL_ALTER", "EngineDdlAlter", "",
     "", true, true},
    {"SET CLUSTER SETTING", PatternMatch::kPrefix, "cluster_admin", "cockroachdb.cluster.setting.set",
     MappingDisposition::kAdmittedSblr, "cluster.job.start_controlled",
     "sblr.cluster.control.v3:cluster.job.start_controlled", "cluster.start_job", "",
     "", true, false},
    {"EXPERIMENTAL_RELOCATE", PatternMatch::kContains, "cluster_admin", "cockroachdb.cluster.experimental_relocate",
     MappingDisposition::kAdmittedSblr, "cluster.job.start_controlled",
     "sblr.cluster.control.v3:cluster.job.start_controlled", "cluster.start_job", "",
     "", true, false},
    {"CRDB_INTERNAL", PatternMatch::kContains, "catalog_overlay", "cockroachdb.catalog_overlay.crdb_internal",
     MappingDisposition::kCatalogProjection, "cockroachdb.catalog.crdb_internal",
     "SBLR_DONOR_COCKROACHDB_CATALOG_PROJECT", "EngineCatalogProjection", "",
     "", false, false},
    {"COPY|| PROGRAM ", PatternMatch::kPrefixAndContains, "bulk_io", "cockroachdb.bulk_io.copy_program",
     MappingDisposition::kPolicyRefusal, "cockroachdb.policy.copy_program",
     "", "", "COCKROACHDB.AUTHORITY.PROGRAM_DENIED",
     "COPY PROGRAM cannot spawn host programs from parser authority.", true, false},
    {"COPY|| TO '", PatternMatch::kPrefixAndContains, "bulk_io", "cockroachdb.bulk_io.copy_to_file",
     MappingDisposition::kPolicyRefusal, "cockroachdb.policy.copy_to_file",
     "", "", "COCKROACHDB.AUTHORITY.FILE_IO_DENIED",
     "COPY TO file cannot perform donor filesystem writes.", true, false},
    {"COPY|| FROM '", PatternMatch::kPrefixAndContains, "bulk_io", "cockroachdb.bulk_io.copy_from_file",
     MappingDisposition::kPolicyRefusal, "cockroachdb.policy.copy_from_file",
     "", "", "COCKROACHDB.AUTHORITY.FILE_IO_DENIED",
     "COPY FROM file requires a trusted ScratchBird import service.", true, false},
    {"COPY|| TO STDOUT", PatternMatch::kPrefixAndContains,
     "logical_stream_backup_restore", "cockroachdb.logical_stream.copy_to_stdout",
     MappingDisposition::kParserSupportUdr, "cockroachdb.udr.copy_to_stdout",
     "SBLR_DONOR_COCKROACHDB_COPY_ROUTE", "ParserSupportCopyRoute",
     "COCKROACHDB.EMULATION.COPY_ROUTE",
     "COPY TO STDOUT is a remote logical export stream routed through trusted package policy.",
     true, false},
    {"COPY|| FROM STDIN", PatternMatch::kPrefixAndContains,
     "logical_stream_backup_restore",
     "cockroachdb.logical_stream.copy_from_stdin",
     MappingDisposition::kParserSupportUdr, "cockroachdb.udr.copy_from_stdin",
     "SBLR_DONOR_COCKROACHDB_COPY_ROUTE", "ParserSupportCopyRoute",
     "COCKROACHDB.EMULATION.COPY_ROUTE",
     "COPY FROM STDIN is a remote logical import stream routed through trusted package policy.",
     true, false},
    {"COPY", PatternMatch::kPrefix, "bulk_io", "cockroachdb.bulk_io.copy",
     MappingDisposition::kParserSupportUdr, "cockroachdb.udr.copy",
     "SBLR_DONOR_COCKROACHDB_COPY_ROUTE", "ParserSupportCopyRoute", "COCKROACHDB.EMULATION.COPY_ROUTE",
     "COPY routes through trusted package policy and engine admission.", true, false},
    {"CREATE FOREIGN DATA WRAPPER", PatternMatch::kPrefix, "connector", "cockroachdb.connector.fdw.create",
     MappingDisposition::kParserSupportUdr, "cockroachdb.udr.connector.fdw.create",
     "SBLR_DONOR_COCKROACHDB_CONNECTOR_ROUTE", "ParserSupportConnectorRoute",
     "COCKROACHDB.EMULATION.CONNECTOR_ROUTE",
     "External connector operations route through the CockroachDB donor UDR.", true, false},
    {"CREATE SERVER", PatternMatch::kPrefix, "connector", "cockroachdb.connector.server.create",
     MappingDisposition::kParserSupportUdr, "cockroachdb.udr.connector.server.create",
     "SBLR_DONOR_COCKROACHDB_CONNECTOR_ROUTE", "ParserSupportConnectorRoute",
     "COCKROACHDB.EMULATION.CONNECTOR_ROUTE",
     "External server operations route through the CockroachDB donor UDR.", true, false},
    {"CREATE USER MAPPING", PatternMatch::kPrefix, "connector", "cockroachdb.connector.user_mapping.create",
     MappingDisposition::kParserSupportUdr, "cockroachdb.udr.connector.user_mapping.create",
     "SBLR_DONOR_COCKROACHDB_CONNECTOR_ROUTE", "ParserSupportConnectorRoute",
     "COCKROACHDB.EMULATION.CONNECTOR_ROUTE",
     "External user mapping operations route through the CockroachDB donor UDR.", true, false},
    {"CREATE FOREIGN TABLE", PatternMatch::kPrefix, "connector", "cockroachdb.connector.foreign_table.create",
     MappingDisposition::kParserSupportUdr, "cockroachdb.udr.connector.foreign_table.create",
     "SBLR_DONOR_COCKROACHDB_CONNECTOR_ROUTE", "ParserSupportConnectorRoute",
     "COCKROACHDB.EMULATION.CONNECTOR_ROUTE",
     "External table creation routes through the CockroachDB donor UDR.", true, false},
    {"CREATE ROLE", PatternMatch::kPrefix, "security", "cockroachdb.security.create_role",
     MappingDisposition::kParserSupportUdr, "cockroachdb.udr.security.create_role",
     "SBLR_DONOR_COCKROACHDB_SECURITY_ROUTE", "ParserSupportSecurityRoute", "COCKROACHDB.EMULATION.SECURITY_ROUTE",
     "Role management routes through trusted security policy.", true, false},
    {"ALTER ROLE", PatternMatch::kPrefix, "security", "cockroachdb.security.alter_role",
     MappingDisposition::kParserSupportUdr, "cockroachdb.udr.security.alter_role",
     "SBLR_DONOR_COCKROACHDB_SECURITY_ROUTE", "ParserSupportSecurityRoute", "COCKROACHDB.EMULATION.SECURITY_ROUTE",
     "Role management routes through trusted security policy.", true, false},
    {"DROP ROLE", PatternMatch::kPrefix, "security", "cockroachdb.security.drop_role",
     MappingDisposition::kParserSupportUdr, "cockroachdb.udr.security.drop_role",
     "SBLR_DONOR_COCKROACHDB_SECURITY_ROUTE", "ParserSupportSecurityRoute", "COCKROACHDB.EMULATION.SECURITY_ROUTE",
     "Role management routes through trusted security policy.", true, false},
    {"GRANT", PatternMatch::kPrefix, "security", "cockroachdb.security.grant",
     MappingDisposition::kParserSupportUdr, "cockroachdb.udr.security.grant",
     "SBLR_DONOR_COCKROACHDB_SECURITY_ROUTE", "ParserSupportSecurityRoute", "COCKROACHDB.EMULATION.SECURITY_ROUTE",
     "Privilege changes route through trusted security policy.", true, false},
    {"REVOKE", PatternMatch::kPrefix, "security", "cockroachdb.security.revoke",
     MappingDisposition::kParserSupportUdr, "cockroachdb.udr.security.revoke",
     "SBLR_DONOR_COCKROACHDB_SECURITY_ROUTE", "ParserSupportSecurityRoute", "COCKROACHDB.EMULATION.SECURITY_ROUTE",
     "Privilege changes route through trusted security policy.", true, false},
    {"CREATE DATABASE", PatternMatch::kPrefix, "database_lifecycle", "cockroachdb.lifecycle.create_database",
     MappingDisposition::kScratchBirdLifecycleApi, "cockroachdb.lifecycle.create_database",
     "SBLR_LIFECYCLE_CREATE_DATABASE", "EngineCreateLifecycle", "",
     "", false, false},
    {"DROP DATABASE", PatternMatch::kPrefix, "database_lifecycle", "cockroachdb.lifecycle.drop_database",
     MappingDisposition::kScratchBirdLifecycleApi, "cockroachdb.lifecycle.drop_database",
     "SBLR_LIFECYCLE_DROP_DATABASE", "EngineDropLifecycle", "",
     "", true, false},
    {"SHOW", PatternMatch::kPrefix, "catalog_overlay", "cockroachdb.catalog_overlay.show",
     MappingDisposition::kCatalogProjection, "cockroachdb.catalog.show",
     "SBLR_DONOR_COCKROACHDB_CATALOG_PROJECT", "EngineCatalogProjection", "",
     "", false, false},
    {"EXPLAIN", PatternMatch::kPrefix, "optimizer", "cockroachdb.optimizer.explain",
     MappingDisposition::kCatalogProjection, "cockroachdb.optimizer.explain",
     "SBLR_DONOR_COCKROACHDB_EXPLAIN", "EngineExplainPlan", "",
     "", false, false},
    {"SET", PatternMatch::kPrefix, "session", "cockroachdb.session.set",
     MappingDisposition::kAdmittedSblr, "cockroachdb.session.set",
     "SBLR_DONOR_COCKROACHDB_SET", "EngineSessionSet", "",
     "", false, false},
    {"RESET", PatternMatch::kPrefix, "session", "cockroachdb.session.reset",
     MappingDisposition::kAdmittedSblr, "cockroachdb.session.reset",
     "SBLR_DONOR_COCKROACHDB_RESET", "EngineSessionReset", "",
     "", false, false},
    {"PREPARE TRANSACTION", PatternMatch::kPrefix, "transaction", "cockroachdb.transaction.prepare_transaction",
     MappingDisposition::kUnsupportedRefusal, "cockroachdb.policy.transaction.prepare_transaction",
     "", "", "COCKROACHDB.AUTHORITY.PREPARE_TRANSACTION_DENIED",
     "Two-phase transaction finality is not admitted by the parser.", true, true},
    {"PREPARE", PatternMatch::kPrefix, "prepared_statement", "cockroachdb.prepared.prepare",
     MappingDisposition::kAdmittedSblr, "cockroachdb.prepared.prepare",
     "SBLR_DONOR_COCKROACHDB_PREPARE", "EnginePrepareStatement", "",
     "", false, false},
    {"EXECUTE", PatternMatch::kPrefix, "prepared_statement", "cockroachdb.prepared.execute",
     MappingDisposition::kAdmittedSblr, "cockroachdb.prepared.execute",
     "SBLR_DONOR_COCKROACHDB_EXECUTE", "EngineExecuteStatement", "",
     "", false, true},
    {"DEALLOCATE", PatternMatch::kPrefix, "prepared_statement", "cockroachdb.prepared.deallocate",
     MappingDisposition::kAdmittedSblr, "cockroachdb.prepared.deallocate",
     "SBLR_DONOR_COCKROACHDB_DEALLOCATE", "EngineDeallocateStatement", "",
     "", false, false},
    {"CREATE", PatternMatch::kPrefix, "ddl", "cockroachdb.ddl.create",
     MappingDisposition::kAdmittedSblr, "cockroachdb.ddl.create",
     "SBLR_DONOR_COCKROACHDB_DDL_CREATE", "EngineDdlCreate", "",
     "", true, true},
    {"ALTER", PatternMatch::kPrefix, "ddl", "cockroachdb.ddl.alter",
     MappingDisposition::kAdmittedSblr, "cockroachdb.ddl.alter",
     "SBLR_DONOR_COCKROACHDB_DDL_ALTER", "EngineDdlAlter", "",
     "", true, true},
    {"DROP", PatternMatch::kPrefix, "ddl", "cockroachdb.ddl.drop",
     MappingDisposition::kAdmittedSblr, "cockroachdb.ddl.drop",
     "SBLR_DONOR_COCKROACHDB_DDL_DROP", "EngineDdlDrop", "",
     "", true, true},
    {"TRUNCATE", PatternMatch::kPrefix, "ddl", "cockroachdb.ddl.truncate",
     MappingDisposition::kAdmittedSblr, "cockroachdb.ddl.truncate",
     "SBLR_DONOR_COCKROACHDB_DDL_TRUNCATE", "EngineDdlTruncate", "",
     "", true, true},
    {"MERGE", PatternMatch::kPrefix, "dml", "cockroachdb.dml.merge",
     MappingDisposition::kAdmittedSblr, "cockroachdb.dml.merge",
     "SBLR_DONOR_COCKROACHDB_MERGE", "EngineDmlMerge", "",
     "", false, true},
    {"INSERT", PatternMatch::kPrefix, "dml", "cockroachdb.dml.insert",
     MappingDisposition::kAdmittedSblr, "cockroachdb.dml.insert",
     "SBLR_DONOR_COCKROACHDB_INSERT", "EngineDmlInsert", "",
     "", false, true},
    {"UPDATE", PatternMatch::kPrefix, "dml", "cockroachdb.dml.update",
     MappingDisposition::kAdmittedSblr, "cockroachdb.dml.update",
     "SBLR_DONOR_COCKROACHDB_UPDATE", "EngineDmlUpdate", "",
     "", false, true},
    {"DELETE", PatternMatch::kPrefix, "dml", "cockroachdb.dml.delete",
     MappingDisposition::kAdmittedSblr, "cockroachdb.dml.delete",
     "SBLR_DONOR_COCKROACHDB_DELETE", "EngineDmlDelete", "",
     "", false, true},
    {"SELECT", PatternMatch::kPrefix, "query", "cockroachdb.query.select",
     MappingDisposition::kAdmittedSblr, "cockroachdb.query.select",
     "SBLR_DONOR_COCKROACHDB_SELECT", "EngineQuerySelect", "",
     "", false, false},
    {"WITH", PatternMatch::kPrefix, "query", "cockroachdb.query.with",
     MappingDisposition::kAdmittedSblr, "cockroachdb.query.with",
     "SBLR_DONOR_COCKROACHDB_SELECT", "EngineQuerySelect", "",
     "", false, false},
    {"START TRANSACTION", PatternMatch::kPrefix, "transaction", "cockroachdb.transaction.start",
     MappingDisposition::kAdmittedSblr, "cockroachdb.transaction.start",
     "SBLR_TRANSACTION_BEGIN", "EngineBeginTransaction", "",
     "", false, false},
    {"BEGIN", PatternMatch::kPrefix, "transaction", "cockroachdb.transaction.begin",
     MappingDisposition::kAdmittedSblr, "cockroachdb.transaction.begin",
     "SBLR_TRANSACTION_BEGIN", "EngineBeginTransaction", "",
     "", false, false},
    {"COMMIT", PatternMatch::kPrefix, "transaction", "cockroachdb.transaction.commit",
     MappingDisposition::kAdmittedSblr, "cockroachdb.transaction.commit",
     "SBLR_TRANSACTION_COMMIT", "EngineCommitTransaction", "",
     "", false, true},
    {"ROLLBACK", PatternMatch::kPrefix, "transaction", "cockroachdb.transaction.rollback",
     MappingDisposition::kAdmittedSblr, "cockroachdb.transaction.rollback",
     "SBLR_TRANSACTION_ROLLBACK", "EngineRollbackTransaction", "",
     "", false, true},
    {"SAVEPOINT", PatternMatch::kPrefix, "transaction", "cockroachdb.transaction.savepoint",
     MappingDisposition::kAdmittedSblr, "cockroachdb.transaction.savepoint",
     "SBLR_TRANSACTION_SAVEPOINT", "EngineSavepoint", "",
     "", false, true},
    {"RELEASE SAVEPOINT", PatternMatch::kPrefix, "transaction", "cockroachdb.transaction.release_savepoint",
     MappingDisposition::kAdmittedSblr, "cockroachdb.transaction.release_savepoint",
     "SBLR_TRANSACTION_RELEASE_SAVEPOINT", "EngineReleaseSavepoint", "",
     "", false, true},
};

const std::array<SurfaceDescriptor, 12> kDatatypeSurfaces{{
    {"numeric", "SMALLINT;INTEGER;BIGINT;DECIMAL;FLOAT;REAL", "descriptor"},
    {"serial_identity", "SERIAL;BIGSERIAL;IDENTITY", "descriptor_policy"},
    {"text", "CHAR;VARCHAR;TEXT;NAME", "descriptor"},
    {"binary", "BYTES;BYTEA", "descriptor"},
    {"temporal", "DATE;TIME;TIMESTAMP;TIMESTAMPTZ;INTERVAL", "descriptor"},
    {"boolean_uuid", "BOOL;BOOLEAN;UUID", "descriptor"},
    {"json", "JSON;JSONB", "descriptor"},
    {"array", "ARRAY;[]", "descriptor"},
    {"spatial", "GEOMETRY;GEOGRAPHY", "parser_support_udr"},
    {"enum", "ENUM", "catalog_policy"},
    {"inet", "INET", "descriptor"},
    {"collated_string", "COLLATE", "catalog_policy"},
}};

const std::array<SurfaceDescriptor, 10> kBuiltinSurfaces{{
    {"aggregate", "COUNT;SUM;AVG;MIN;MAX;STRING_AGG", "sblr"},
    {"window", "ROW_NUMBER;RANK;DENSE_RANK;LAG;LEAD", "sblr"},
    {"string", "LOWER;UPPER;SUBSTRING;TRIM;OVERLAY", "sblr"},
    {"numeric", "ABS;ROUND;POWER;SQRT;MOD", "sblr"},
    {"temporal", "NOW;CURRENT_TIMESTAMP;DATE_PART;DATE_TRUNC", "sblr"},
    {"json", "JSONB_*;JSON_*", "parser_support_udr"},
    {"spatial", "ST_*", "parser_support_udr"},
    {"security", "CURRENT_USER;SESSION_USER;CURRENT_ROLE", "catalog_projection"},
    {"catalog", "CRDB_INTERNAL;VERSION", "catalog_projection"},
    {"uuid", "GEN_RANDOM_UUID;UUID_*", "parser_support_udr"},
}};

const std::array<SurfaceDescriptor, 10> kCatalogSurfaces{{
    {"crdb_internal", "CRDB_INTERNAL.", "catalog_projection"},
    {"information_schema", "INFORMATION_SCHEMA.", "catalog_projection"},
    {"system", "SYSTEM.", "catalog_projection"},
    {"ranges", "SHOW RANGES;RANGES", "catalog_projection"},
    {"jobs", "SHOW JOBS;SYSTEM.JOBS", "catalog_projection"},
    {"cluster_settings", "CLUSTER_SETTINGS;SHOW CLUSTER SETTING", "catalog_projection"},
    {"zone_configs", "ZONE_CONFIGS;ALTER RANGE", "catalog_projection"},
    {"statistics", "TABLE_STATISTICS;CREATE STATISTICS", "catalog_projection"},
    {"users_roles", "USERS;ROLE_MEMBERS", "catalog_projection"},
    {"descriptors", "DESCRIPTOR;NAMESPACE", "catalog_projection"},
}};

const std::array<SurfaceDescriptor, 12> kDiagnosticSurfaces{{
    {"parse", "COCKROACHDB.PARSE.INVALID_INPUT;COCKROACHDB.PARSE.UNSUPPORTED_SURFACE", "parser"},
    {"file", "COCKROACHDB.AUTHORITY.FILE_IO_DENIED", "fail_closed"},
    {"program", "COCKROACHDB.AUTHORITY.PROGRAM_DENIED", "fail_closed"},
    {"backup", "COCKROACHDB.AUTHORITY.BACKUP_DENIED", "fail_closed"},
    {"restore", "COCKROACHDB.AUTHORITY.RESTORE_DENIED", "fail_closed"},
    {"changefeed", "COCKROACHDB.EMULATION.CHANGEFEED_ROUTE", "parser_support_udr"},
    {"zone", "COCKROACHDB.EMULATION.ZONE_ROUTE", "parser_support_udr"},
    {"cluster", "COCKROACHDB.EMULATION.CLUSTER_ROUTE", "parser_support_udr"},
    {"connector", "COCKROACHDB.EMULATION.CONNECTOR_ROUTE", "parser_support_udr"},
    {"security", "COCKROACHDB.EMULATION.SECURITY_ROUTE", "parser_support_udr"},
    {"transaction", "COCKROACHDB.AUTHORITY.PREPARE_TRANSACTION_DENIED", "fail_closed"},
    {"unsupported", "COCKROACHDB.AUTHORITY.UNSUPPORTED_CLUSTER_SURFACE", "fail_closed"},
}};

const scratchbird::parser::donor::DialectProfile kProfile{
    "cockroachdb",
    "CockroachDB",
    "sbp_cockroachdb",
    "sbup_cockroachdb",
    "26.1.3",
    "COCKROACHDB",
    kSblrFamily,
    kPatterns,
    kDatatypeSurfaces,
    kBuiltinSurfaces,
    kCatalogSurfaces,
    kDiagnosticSurfaces,
    19,
    4,
    0,
    0,
    2,
    0,
    0,
    0,
    2,
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

std::string CockroachdbPackageIdentityJson() {
  return scratchbird::parser::donor::PackageIdentityJson(kProfile);
}

std::string CockroachdbSurfaceReportJson() {
  return scratchbird::parser::donor::SurfaceReportJson(kProfile);
}

} // namespace scratchbird::parser::cockroachdb
