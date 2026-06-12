// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "yugabytedb_dialect.hpp"

#include <array>

namespace scratchbird::parser::yugabytedb {
namespace {

using scratchbird::parser::compatibility::MappingDisposition;
using scratchbird::parser::compatibility::OperationPattern;
using scratchbird::parser::compatibility::PatternMatch;
using scratchbird::parser::compatibility::SurfaceDescriptor;

constexpr std::string_view kSblrFamily = "sblr.compatibility.yugabytedb.profile.v1";

constexpr OperationPattern kPatterns[] = {
    {"BACKUP", PatternMatch::kPrefix, "backup_restore", "yugabytedb.backup.backup",
     MappingDisposition::kPolicyRefusal, "yugabytedb.policy.backup",
     "", "", "YUGABYTEDB.AUTHORITY.BACKUP_DENIED",
     "Backup routes require trusted migration service admission.", true, false},
    {"RESTORE", PatternMatch::kPrefix, "backup_restore", "yugabytedb.restore.restore",
     MappingDisposition::kPolicyRefusal, "yugabytedb.policy.restore",
     "", "", "YUGABYTEDB.AUTHORITY.RESTORE_DENIED",
     "Restore routes require trusted migration service admission.", true, false},
    {"CREATE KEYSPACE", PatternMatch::kPrefix, "ycql", "yugabytedb.ycql.keyspace.create",
     MappingDisposition::kParserSupportUdr, "yugabytedb.udr.ycql.keyspace.create",
     "SBLR_COMPAT_YUGABYTEDB_YCQL_ROUTE", "ParserSupportYcqlRoute", "YUGABYTEDB.EMULATION.YCQL_ROUTE",
     "YCQL keyspace operations route through trusted catalog policy.", true, false},
    {"ALTER KEYSPACE", PatternMatch::kPrefix, "ycql", "yugabytedb.ycql.keyspace.alter",
     MappingDisposition::kParserSupportUdr, "yugabytedb.udr.ycql.keyspace.alter",
     "SBLR_COMPAT_YUGABYTEDB_YCQL_ROUTE", "ParserSupportYcqlRoute", "YUGABYTEDB.EMULATION.YCQL_ROUTE",
     "YCQL keyspace operations route through trusted catalog policy.", true, false},
    {"DROP KEYSPACE", PatternMatch::kPrefix, "ycql", "yugabytedb.ycql.keyspace.drop",
     MappingDisposition::kParserSupportUdr, "yugabytedb.udr.ycql.keyspace.drop",
     "SBLR_COMPAT_YUGABYTEDB_YCQL_ROUTE", "ParserSupportYcqlRoute", "YUGABYTEDB.EMULATION.YCQL_ROUTE",
     "YCQL keyspace operations route through trusted catalog policy.", true, false},
    {"SPLIT INTO", PatternMatch::kContains, "tablet", "yugabytedb.tablet.split_into",
     MappingDisposition::kParserSupportUdr, "yugabytedb.udr.tablet.split_into",
     "SBLR_COMPAT_YUGABYTEDB_TABLET_ROUTE", "ParserSupportTabletRoute", "YUGABYTEDB.EMULATION.TABLET_ROUTE",
     "Tablet split directives route through trusted placement policy.", true, true},
    {"CREATE TABLEGROUP", PatternMatch::kPrefix, "tablet", "yugabytedb.tablegroup.create",
     MappingDisposition::kParserSupportUdr, "yugabytedb.udr.tablegroup.create",
     "SBLR_COMPAT_YUGABYTEDB_TABLEGROUP_ROUTE", "ParserSupportTablegroupRoute", "YUGABYTEDB.EMULATION.TABLEGROUP_ROUTE",
     "Tablegroup operations route through trusted placement policy.", true, false},
    {"ALTER TABLEGROUP", PatternMatch::kPrefix, "tablet", "yugabytedb.tablegroup.alter",
     MappingDisposition::kParserSupportUdr, "yugabytedb.udr.tablegroup.alter",
     "SBLR_COMPAT_YUGABYTEDB_TABLEGROUP_ROUTE", "ParserSupportTablegroupRoute", "YUGABYTEDB.EMULATION.TABLEGROUP_ROUTE",
     "Tablegroup operations route through trusted placement policy.", true, false},
    {"DROP TABLEGROUP", PatternMatch::kPrefix, "tablet", "yugabytedb.tablegroup.drop",
     MappingDisposition::kParserSupportUdr, "yugabytedb.udr.tablegroup.drop",
     "SBLR_COMPAT_YUGABYTEDB_TABLEGROUP_ROUTE", "ParserSupportTablegroupRoute", "YUGABYTEDB.EMULATION.TABLEGROUP_ROUTE",
     "Tablegroup operations route through trusted placement policy.", true, false},
    {"CREATE CDC STREAM", PatternMatch::kPrefix, "cdc", "yugabytedb.cdc.create_stream",
     MappingDisposition::kParserSupportUdr, "yugabytedb.udr.cdc.create_stream",
     "SBLR_COMPAT_YUGABYTEDB_CDC_ROUTE", "ParserSupportCdcRoute",
     "YUGABYTEDB.EMULATION.CDC_ROUTE",
     "CDC stream requests route through the YugabyteDB compatibility UDR.", true, false},
    {"YB_SERVER_REGION", PatternMatch::kContains, "catalog_overlay", "yugabytedb.catalog_overlay.server_region",
     MappingDisposition::kCatalogProjection, "yugabytedb.catalog.server_region",
     "SBLR_COMPAT_YUGABYTEDB_CATALOG_PROJECT", "EngineCatalogProjection", "",
     "", false, false},
    {"COPY|| PROGRAM ", PatternMatch::kPrefixAndContains, "bulk_io", "yugabytedb.bulk_io.copy_program",
     MappingDisposition::kPolicyRefusal, "yugabytedb.policy.copy_program",
     "", "", "YUGABYTEDB.AUTHORITY.PROGRAM_DENIED",
     "COPY PROGRAM cannot spawn host programs from parser authority.", true, false},
    {"COPY|| TO '", PatternMatch::kPrefixAndContains, "bulk_io", "yugabytedb.bulk_io.copy_to_file",
     MappingDisposition::kPolicyRefusal, "yugabytedb.policy.copy_to_file",
     "", "", "YUGABYTEDB.AUTHORITY.FILE_IO_DENIED",
     "COPY TO file cannot perform compatibility filesystem writes.", true, false},
    {"COPY|| FROM '", PatternMatch::kPrefixAndContains, "bulk_io", "yugabytedb.bulk_io.copy_from_file",
     MappingDisposition::kPolicyRefusal, "yugabytedb.policy.copy_from_file",
     "", "", "YUGABYTEDB.AUTHORITY.FILE_IO_DENIED",
     "COPY FROM file requires a trusted ScratchBird import service.", true, false},
    {"COPY|| TO STDOUT", PatternMatch::kPrefixAndContains,
     "logical_stream_backup_restore", "yugabytedb.logical_stream.copy_to_stdout",
     MappingDisposition::kParserSupportUdr, "yugabytedb.udr.copy_to_stdout",
     "SBLR_COMPAT_YUGABYTEDB_COPY_ROUTE", "ParserSupportCopyRoute",
     "YUGABYTEDB.EMULATION.COPY_ROUTE",
     "COPY TO STDOUT is a remote logical export stream routed through trusted package policy.",
     true, false},
    {"COPY|| FROM STDIN", PatternMatch::kPrefixAndContains,
     "logical_stream_backup_restore",
     "yugabytedb.logical_stream.copy_from_stdin",
     MappingDisposition::kParserSupportUdr, "yugabytedb.udr.copy_from_stdin",
     "SBLR_COMPAT_YUGABYTEDB_COPY_ROUTE", "ParserSupportCopyRoute",
     "YUGABYTEDB.EMULATION.COPY_ROUTE",
     "COPY FROM STDIN is a remote logical import stream routed through trusted package policy.",
     true, false},
    {"COPY", PatternMatch::kPrefix, "bulk_io", "yugabytedb.bulk_io.copy",
     MappingDisposition::kParserSupportUdr, "yugabytedb.udr.copy",
     "SBLR_COMPAT_YUGABYTEDB_COPY_ROUTE", "ParserSupportCopyRoute", "YUGABYTEDB.EMULATION.COPY_ROUTE",
     "COPY routes through trusted package policy and engine admission.", true, false},
    {"CREATE FOREIGN DATA WRAPPER", PatternMatch::kPrefix, "connector", "yugabytedb.connector.fdw.create",
     MappingDisposition::kParserSupportUdr, "yugabytedb.udr.connector.fdw.create",
     "SBLR_COMPAT_YUGABYTEDB_CONNECTOR_ROUTE", "ParserSupportConnectorRoute",
     "YUGABYTEDB.EMULATION.CONNECTOR_ROUTE",
     "External connector operations route through the YugabyteDB compatibility UDR.", true, false},
    {"CREATE SERVER", PatternMatch::kPrefix, "connector", "yugabytedb.connector.server.create",
     MappingDisposition::kParserSupportUdr, "yugabytedb.udr.connector.server.create",
     "SBLR_COMPAT_YUGABYTEDB_CONNECTOR_ROUTE", "ParserSupportConnectorRoute",
     "YUGABYTEDB.EMULATION.CONNECTOR_ROUTE",
     "External server operations route through the YugabyteDB compatibility UDR.", true, false},
    {"CREATE USER MAPPING", PatternMatch::kPrefix, "connector", "yugabytedb.connector.user_mapping.create",
     MappingDisposition::kParserSupportUdr, "yugabytedb.udr.connector.user_mapping.create",
     "SBLR_COMPAT_YUGABYTEDB_CONNECTOR_ROUTE", "ParserSupportConnectorRoute",
     "YUGABYTEDB.EMULATION.CONNECTOR_ROUTE",
     "External user mapping operations route through the YugabyteDB compatibility UDR.", true, false},
    {"CREATE FOREIGN TABLE", PatternMatch::kPrefix, "connector", "yugabytedb.connector.foreign_table.create",
     MappingDisposition::kParserSupportUdr, "yugabytedb.udr.connector.foreign_table.create",
     "SBLR_COMPAT_YUGABYTEDB_CONNECTOR_ROUTE", "ParserSupportConnectorRoute",
     "YUGABYTEDB.EMULATION.CONNECTOR_ROUTE",
     "External table creation routes through the YugabyteDB compatibility UDR.", true, false},
    {"CREATE ROLE", PatternMatch::kPrefix, "security", "yugabytedb.security.create_role",
     MappingDisposition::kParserSupportUdr, "yugabytedb.udr.security.create_role",
     "SBLR_COMPAT_YUGABYTEDB_SECURITY_ROUTE", "ParserSupportSecurityRoute", "YUGABYTEDB.EMULATION.SECURITY_ROUTE",
     "Role management routes through trusted security policy.", true, false},
    {"ALTER ROLE", PatternMatch::kPrefix, "security", "yugabytedb.security.alter_role",
     MappingDisposition::kParserSupportUdr, "yugabytedb.udr.security.alter_role",
     "SBLR_COMPAT_YUGABYTEDB_SECURITY_ROUTE", "ParserSupportSecurityRoute", "YUGABYTEDB.EMULATION.SECURITY_ROUTE",
     "Role management routes through trusted security policy.", true, false},
    {"DROP ROLE", PatternMatch::kPrefix, "security", "yugabytedb.security.drop_role",
     MappingDisposition::kParserSupportUdr, "yugabytedb.udr.security.drop_role",
     "SBLR_COMPAT_YUGABYTEDB_SECURITY_ROUTE", "ParserSupportSecurityRoute", "YUGABYTEDB.EMULATION.SECURITY_ROUTE",
     "Role management routes through trusted security policy.", true, false},
    {"GRANT", PatternMatch::kPrefix, "security", "yugabytedb.security.grant",
     MappingDisposition::kParserSupportUdr, "yugabytedb.udr.security.grant",
     "SBLR_COMPAT_YUGABYTEDB_SECURITY_ROUTE", "ParserSupportSecurityRoute", "YUGABYTEDB.EMULATION.SECURITY_ROUTE",
     "Privilege changes route through trusted security policy.", true, false},
    {"REVOKE", PatternMatch::kPrefix, "security", "yugabytedb.security.revoke",
     MappingDisposition::kParserSupportUdr, "yugabytedb.udr.security.revoke",
     "SBLR_COMPAT_YUGABYTEDB_SECURITY_ROUTE", "ParserSupportSecurityRoute", "YUGABYTEDB.EMULATION.SECURITY_ROUTE",
     "Privilege changes route through trusted security policy.", true, false},
    {"CREATE DATABASE", PatternMatch::kPrefix, "database_lifecycle", "yugabytedb.lifecycle.create_database",
     MappingDisposition::kScratchBirdLifecycleApi, "yugabytedb.lifecycle.create_database",
     "SBLR_LIFECYCLE_CREATE_DATABASE", "EngineCreateLifecycle", "",
     "", false, false},
    {"DROP DATABASE", PatternMatch::kPrefix, "database_lifecycle", "yugabytedb.lifecycle.drop_database",
     MappingDisposition::kScratchBirdLifecycleApi, "yugabytedb.lifecycle.drop_database",
     "SBLR_LIFECYCLE_DROP_DATABASE", "EngineDropLifecycle", "",
     "", true, false},
    {"SHOW", PatternMatch::kPrefix, "catalog_overlay", "yugabytedb.catalog_overlay.show",
     MappingDisposition::kCatalogProjection, "yugabytedb.catalog.show",
     "SBLR_COMPAT_YUGABYTEDB_CATALOG_PROJECT", "EngineCatalogProjection", "",
     "", false, false},
    {"EXPLAIN", PatternMatch::kPrefix, "optimizer", "yugabytedb.optimizer.explain",
     MappingDisposition::kCatalogProjection, "yugabytedb.optimizer.explain",
     "SBLR_COMPAT_YUGABYTEDB_EXPLAIN", "EngineExplainPlan", "",
     "", false, false},
    {"SET", PatternMatch::kPrefix, "session", "yugabytedb.session.set",
     MappingDisposition::kAdmittedSblr, "yugabytedb.session.set",
     "SBLR_COMPAT_YUGABYTEDB_SET", "EngineSessionSet", "",
     "", false, false},
    {"RESET", PatternMatch::kPrefix, "session", "yugabytedb.session.reset",
     MappingDisposition::kAdmittedSblr, "yugabytedb.session.reset",
     "SBLR_COMPAT_YUGABYTEDB_RESET", "EngineSessionReset", "",
     "", false, false},
    {"PREPARE TRANSACTION", PatternMatch::kPrefix, "transaction", "yugabytedb.transaction.prepare_transaction",
     MappingDisposition::kUnsupportedRefusal, "yugabytedb.policy.transaction.prepare_transaction",
     "", "", "YUGABYTEDB.AUTHORITY.PREPARE_TRANSACTION_DENIED",
     "Two-phase transaction finality is not admitted by the parser.", true, true},
    {"PREPARE", PatternMatch::kPrefix, "prepared_statement", "yugabytedb.prepared.prepare",
     MappingDisposition::kAdmittedSblr, "yugabytedb.prepared.prepare",
     "SBLR_COMPAT_YUGABYTEDB_PREPARE", "EnginePrepareStatement", "",
     "", false, false},
    {"EXECUTE", PatternMatch::kPrefix, "prepared_statement", "yugabytedb.prepared.execute",
     MappingDisposition::kAdmittedSblr, "yugabytedb.prepared.execute",
     "SBLR_COMPAT_YUGABYTEDB_EXECUTE", "EngineExecuteStatement", "",
     "", false, true},
    {"DEALLOCATE", PatternMatch::kPrefix, "prepared_statement", "yugabytedb.prepared.deallocate",
     MappingDisposition::kAdmittedSblr, "yugabytedb.prepared.deallocate",
     "SBLR_COMPAT_YUGABYTEDB_DEALLOCATE", "EngineDeallocateStatement", "",
     "", false, false},
    {"CREATE", PatternMatch::kPrefix, "ddl", "yugabytedb.ddl.create",
     MappingDisposition::kAdmittedSblr, "yugabytedb.ddl.create",
     "SBLR_COMPAT_YUGABYTEDB_DDL_CREATE", "EngineDdlCreate", "",
     "", true, true},
    {"ALTER", PatternMatch::kPrefix, "ddl", "yugabytedb.ddl.alter",
     MappingDisposition::kAdmittedSblr, "yugabytedb.ddl.alter",
     "SBLR_COMPAT_YUGABYTEDB_DDL_ALTER", "EngineDdlAlter", "",
     "", true, true},
    {"DROP", PatternMatch::kPrefix, "ddl", "yugabytedb.ddl.drop",
     MappingDisposition::kAdmittedSblr, "yugabytedb.ddl.drop",
     "SBLR_COMPAT_YUGABYTEDB_DDL_DROP", "EngineDdlDrop", "",
     "", true, true},
    {"TRUNCATE", PatternMatch::kPrefix, "ddl", "yugabytedb.ddl.truncate",
     MappingDisposition::kAdmittedSblr, "yugabytedb.ddl.truncate",
     "SBLR_COMPAT_YUGABYTEDB_DDL_TRUNCATE", "EngineDdlTruncate", "",
     "", true, true},
    {"MERGE", PatternMatch::kPrefix, "dml", "yugabytedb.dml.merge",
     MappingDisposition::kAdmittedSblr, "yugabytedb.dml.merge",
     "SBLR_COMPAT_YUGABYTEDB_MERGE", "EngineDmlMerge", "",
     "", false, true},
    {"INSERT", PatternMatch::kPrefix, "dml", "yugabytedb.dml.insert",
     MappingDisposition::kAdmittedSblr, "yugabytedb.dml.insert",
     "SBLR_COMPAT_YUGABYTEDB_INSERT", "EngineDmlInsert", "",
     "", false, true},
    {"UPDATE", PatternMatch::kPrefix, "dml", "yugabytedb.dml.update",
     MappingDisposition::kAdmittedSblr, "yugabytedb.dml.update",
     "SBLR_COMPAT_YUGABYTEDB_UPDATE", "EngineDmlUpdate", "",
     "", false, true},
    {"DELETE", PatternMatch::kPrefix, "dml", "yugabytedb.dml.delete",
     MappingDisposition::kAdmittedSblr, "yugabytedb.dml.delete",
     "SBLR_COMPAT_YUGABYTEDB_DELETE", "EngineDmlDelete", "",
     "", false, true},
    {"SELECT", PatternMatch::kPrefix, "query", "yugabytedb.query.select",
     MappingDisposition::kAdmittedSblr, "yugabytedb.query.select",
     "SBLR_COMPAT_YUGABYTEDB_SELECT", "EngineQuerySelect", "",
     "", false, false},
    {"WITH", PatternMatch::kPrefix, "query", "yugabytedb.query.with",
     MappingDisposition::kAdmittedSblr, "yugabytedb.query.with",
     "SBLR_COMPAT_YUGABYTEDB_SELECT", "EngineQuerySelect", "",
     "", false, false},
    {"START TRANSACTION", PatternMatch::kPrefix, "transaction", "yugabytedb.transaction.start",
     MappingDisposition::kAdmittedSblr, "yugabytedb.transaction.start",
     "SBLR_TRANSACTION_BEGIN", "EngineBeginTransaction", "",
     "", false, false},
    {"BEGIN", PatternMatch::kPrefix, "transaction", "yugabytedb.transaction.begin",
     MappingDisposition::kAdmittedSblr, "yugabytedb.transaction.begin",
     "SBLR_TRANSACTION_BEGIN", "EngineBeginTransaction", "",
     "", false, false},
    {"COMMIT", PatternMatch::kPrefix, "transaction", "yugabytedb.transaction.commit",
     MappingDisposition::kAdmittedSblr, "yugabytedb.transaction.commit",
     "SBLR_TRANSACTION_COMMIT", "EngineCommitTransaction", "",
     "", false, true},
    {"ROLLBACK", PatternMatch::kPrefix, "transaction", "yugabytedb.transaction.rollback",
     MappingDisposition::kAdmittedSblr, "yugabytedb.transaction.rollback",
     "SBLR_TRANSACTION_ROLLBACK", "EngineRollbackTransaction", "",
     "", false, true},
    {"SAVEPOINT", PatternMatch::kPrefix, "transaction", "yugabytedb.transaction.savepoint",
     MappingDisposition::kAdmittedSblr, "yugabytedb.transaction.savepoint",
     "SBLR_TRANSACTION_SAVEPOINT", "EngineSavepoint", "",
     "", false, true},
    {"RELEASE SAVEPOINT", PatternMatch::kPrefix, "transaction", "yugabytedb.transaction.release_savepoint",
     MappingDisposition::kAdmittedSblr, "yugabytedb.transaction.release_savepoint",
     "SBLR_TRANSACTION_RELEASE_SAVEPOINT", "EngineReleaseSavepoint", "",
     "", false, true},
};

const std::array<SurfaceDescriptor, 12> kDatatypeSurfaces{{
    {"numeric", "SMALLINT;INTEGER;BIGINT;NUMERIC;DECIMAL;REAL;DOUBLE PRECISION", "descriptor"},
    {"serial_identity", "SERIAL;BIGSERIAL;IDENTITY", "descriptor_policy"},
    {"text", "CHAR;VARCHAR;TEXT;NAME", "descriptor"},
    {"binary", "BYTEA;BLOB", "descriptor"},
    {"temporal", "DATE;TIME;TIMESTAMP;TIMESTAMPTZ;INTERVAL", "descriptor"},
    {"boolean_uuid", "BOOLEAN;UUID", "descriptor"},
    {"json", "JSON;JSONB", "descriptor"},
    {"array", "ARRAY;[]", "descriptor"},
    {"ycql", "KEYSPACE;COUNTER;FROZEN", "parser_support_udr"},
    {"geo", "POINT;LINE;POLYGON", "parser_support_udr"},
    {"inet", "INET;CIDR", "descriptor"},
    {"domain_enum", "DOMAIN;ENUM", "catalog_policy"},
}};

const std::array<SurfaceDescriptor, 12> kBuiltinSurfaces{{
    {"aggregate", "COUNT;SUM;AVG;MIN;MAX;STRING_AGG", "sblr"},
    {"window", "ROW_NUMBER;RANK;DENSE_RANK;LAG;LEAD", "sblr"},
    {"string", "LOWER;UPPER;SUBSTRING;TRIM;OVERLAY", "sblr"},
    {"numeric", "ABS;ROUND;POWER;SQRT;MOD", "sblr"},
    {"temporal", "NOW;CURRENT_TIMESTAMP;DATE_PART;DATE_TRUNC", "sblr"},
    {"json", "JSONB_*;JSON_*", "parser_support_udr"},
    {"array", "ARRAY_APPEND;ARRAY_REMOVE;UNNEST", "parser_support_udr"},
    {"yb_catalog", "YB_SERVER_REGION;YB_IS_LOCAL_TABLE", "catalog_projection"},
    {"uuid", "GEN_RANDOM_UUID;UUID_*", "parser_support_udr"},
    {"security", "CURRENT_USER;SESSION_USER;CURRENT_ROLE", "catalog_projection"},
    {"ycql", "TOKEN;TTL;WRITETIME", "parser_support_udr"},
    {"cdc", "CDC_*;XCLUSTER_*", "parser_support_udr"},
}};

const std::array<SurfaceDescriptor, 10> kCatalogSurfaces{{
    {"yb_catalog", "YB_CATALOG.", "catalog_projection"},
    {"pg_catalog", "PG_CATALOG.", "catalog_projection"},
    {"information_schema", "INFORMATION_SCHEMA.", "catalog_projection"},
    {"ycql_keyspaces", "SYSTEM_SCHEMA.KEYSPACES;YCQL_KEYSPACES", "catalog_projection"},
    {"tablets", "YB_TABLETS;YB_SERVERS", "catalog_projection"},
    {"tablegroups", "YB_TABLEGROUPS", "catalog_projection"},
    {"xcluster", "YB_XCLUSTER;YB_CDC_STREAMS", "catalog_projection"},
    {"roles_security", "PG_AUTHID;PG_ROLES;PG_POLICY", "catalog_projection"},
    {"fdw", "PG_FOREIGN_DATA_WRAPPER;PG_FOREIGN_SERVER;PG_USER_MAPPING", "catalog_projection"},
    {"schema_table", "PG_NAMESPACE;PG_CLASS;PG_ATTRIBUTE;PG_TYPE", "catalog_projection"},
}};

const std::array<SurfaceDescriptor, 12> kDiagnosticSurfaces{{
    {"parse", "YUGABYTEDB.PARSE.INVALID_INPUT;YUGABYTEDB.PARSE.UNSUPPORTED_SURFACE", "parser"},
    {"file", "YUGABYTEDB.AUTHORITY.FILE_IO_DENIED", "fail_closed"},
    {"program", "YUGABYTEDB.AUTHORITY.PROGRAM_DENIED", "fail_closed"},
    {"backup", "YUGABYTEDB.AUTHORITY.BACKUP_DENIED", "fail_closed"},
    {"restore", "YUGABYTEDB.AUTHORITY.RESTORE_DENIED", "fail_closed"},
    {"ycql", "YUGABYTEDB.EMULATION.YCQL_ROUTE", "parser_support_udr"},
    {"tablet", "YUGABYTEDB.EMULATION.TABLET_ROUTE", "parser_support_udr"},
    {"tablegroup", "YUGABYTEDB.EMULATION.TABLEGROUP_ROUTE", "parser_support_udr"},
    {"cdc", "YUGABYTEDB.EMULATION.CDC_ROUTE", "parser_support_udr"},
    {"connector", "YUGABYTEDB.EMULATION.CONNECTOR_ROUTE", "parser_support_udr"},
    {"security", "YUGABYTEDB.EMULATION.SECURITY_ROUTE", "parser_support_udr"},
    {"transaction", "YUGABYTEDB.AUTHORITY.PREPARE_TRANSACTION_DENIED", "fail_closed"},
}};

const scratchbird::parser::compatibility::DialectProfile kProfile{
    "yugabytedb",
    "YugabyteDB",
    "sbp_yugabytedb",
    "sbup_yugabytedb",
    "2025.2.2.2",
    "YUGABYTEDB",
    kSblrFamily,
    kPatterns,
    kDatatypeSurfaces,
    kBuiltinSurfaces,
    kCatalogSurfaces,
    kDiagnosticSurfaces,
    19,
    477,
    355,
    102,
    10,
    10,
    0,
    0,
    0,
};

} // namespace

const scratchbird::parser::compatibility::DialectProfile& Profile() {
  return kProfile;
}

std::string TrimAscii(std::string_view text) {
  return scratchbird::parser::compatibility::TrimAscii(text);
}

std::string NormalizeWhitespace(std::string_view text) {
  return scratchbird::parser::compatibility::NormalizeWhitespace(text);
}

std::string ToUpperAscii(std::string_view text) {
  return scratchbird::parser::compatibility::ToUpperAscii(text);
}

std::string MessageVectorToJson(const std::vector<Diagnostic>& diagnostics) {
  return scratchbird::parser::compatibility::MessageVectorToJson(diagnostics);
}

std::vector<Token> LexTokens(std::string_view sql_text) {
  return scratchbird::parser::compatibility::LexTokens(sql_text);
}

ParseResult ParseStatement(std::string_view sql_text) {
  return scratchbird::parser::compatibility::ParseStatement(sql_text, kProfile);
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

std::string YugabytedbPackageIdentityJson() {
  return scratchbird::parser::compatibility::PackageIdentityJson(kProfile);
}

std::string YugabytedbSurfaceReportJson() {
  return scratchbird::parser::compatibility::SurfaceReportJson(kProfile);
}

} // namespace scratchbird::parser::yugabytedb
