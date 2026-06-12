// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "vitess_dialect.hpp"

#include <array>

namespace scratchbird::parser::vitess {
namespace {

using scratchbird::parser::compatibility::MappingDisposition;
using scratchbird::parser::compatibility::OperationPattern;
using scratchbird::parser::compatibility::PatternMatch;
using scratchbird::parser::compatibility::SurfaceDescriptor;

constexpr std::string_view kSblrFamily = "sblr.compatibility.vitess.profile.v1";

constexpr OperationPattern kPatterns[] = {
    {"VTCTL REPARENT", PatternMatch::kPrefix, "topology_admin", "vitess.topology.reparent",
     MappingDisposition::kAdmittedSblr, "cluster.job.start_controlled",
     "sblr.cluster.control.v3:cluster.job.start_controlled", "cluster.start_job", "",
     "", true, false},
    {"VTCTL", PatternMatch::kPrefix, "topology_admin", "vitess.topology.command",
     MappingDisposition::kAdmittedSblr, "cluster.job.start_controlled",
     "sblr.cluster.control.v3:cluster.job.start_controlled", "cluster.start_job", "",
     "", true, false},
    {"MOVE TABLES", PatternMatch::kPrefix, "vreplication", "vitess.vreplication.move_tables",
     MappingDisposition::kParserSupportUdr, "vitess.udr.vreplication.move_tables",
     "SBLR_COMPAT_VITESS_VREPLICATION_ROUTE", "ParserSupportVReplicationRoute",
     "VITESS.EMULATION.VREPLICATION_ROUTE",
     "VReplication move-table requests route through the Vitess compatibility UDR.", true, false},
    {"RESHARD", PatternMatch::kPrefix, "vreplication", "vitess.vreplication.reshard",
     MappingDisposition::kParserSupportUdr, "vitess.udr.vreplication.reshard",
     "SBLR_COMPAT_VITESS_VREPLICATION_ROUTE", "ParserSupportVReplicationRoute",
     "VITESS.EMULATION.VREPLICATION_ROUTE",
     "VReplication reshard requests route through the Vitess compatibility UDR.", true, false},
    {"VDIFF", PatternMatch::kPrefix, "vreplication", "vitess.vreplication.vdiff",
     MappingDisposition::kParserSupportUdr, "vitess.udr.vreplication.vdiff",
     "SBLR_COMPAT_VITESS_VREPLICATION_ROUTE", "ParserSupportVReplicationRoute",
     "VITESS.EMULATION.VREPLICATION_ROUTE",
     "VReplication diff requests route through the Vitess compatibility UDR.", true, false},
    {"ALTER VSCHEMA", PatternMatch::kPrefix, "vschema", "vitess.vschema.alter",
     MappingDisposition::kParserSupportUdr, "vitess.udr.vschema.alter",
     "SBLR_COMPAT_VITESS_VSCHEMA_ROUTE", "ParserSupportVSchemaRoute", "VITESS.EMULATION.VSCHEMA_ROUTE",
     "VSchema changes route through trusted topology policy.", true, false},
    {"SHOW VSCHEMA", PatternMatch::kPrefix, "catalog_overlay", "vitess.catalog_overlay.show_vschema",
     MappingDisposition::kCatalogProjection, "vitess.catalog.show_vschema",
     "SBLR_COMPAT_VITESS_CATALOG_PROJECT", "EngineCatalogProjection", "",
     "", false, false},
    {"SHOW VITESS", PatternMatch::kPrefix, "catalog_overlay", "vitess.catalog_overlay.show_status",
     MappingDisposition::kCatalogProjection, "vitess.catalog.show_status",
     "SBLR_COMPAT_VITESS_CATALOG_PROJECT", "EngineCatalogProjection", "",
     "", false, false},
    {"LOAD DATA LOCAL INFILE", PatternMatch::kLoadDataLocalInfile, "bulk_io", "vitess.bulk_io.load_data_local_infile",
     MappingDisposition::kParserSupportUdr, "vitess.udr.etl.load_data_local_infile",
     "SBLR_COMPAT_VITESS_ETL_ROUTE", "ParserSupportEtlRoute",
     "VITESS.EMULATION.ETL_ROUTE",
     "LOAD DATA LOCAL INFILE routes through the Vitess compatibility UDR as a client logical ETL stream.",
     true, true},
    {"LOAD DATA INFILE", PatternMatch::kLoadDataServerInfile, "bulk_io", "vitess.bulk_io.load_data_infile",
     MappingDisposition::kPolicyRefusal, "vitess.policy.file.load_data_infile",
     "", "", "VITESS.AUTHORITY.FILE_IO_DENIED",
     "File import requires a trusted ScratchBird import service.", true, false},
    {"LOAD_FILE", PatternMatch::kContainsFunctionCall, "bulk_io", "vitess.bulk_io.load_file",
     MappingDisposition::kPolicyRefusal, "vitess.policy.file.load_file",
     "", "", "VITESS.AUTHORITY.FILE_IO_DENIED",
     "Host file reads are blocked from parser authority.", true, false},
    {"SELECT|| INTO OUTFILE", PatternMatch::kPrefixAndContains, "bulk_io", "vitess.bulk_io.select_into_outfile",
     MappingDisposition::kPolicyRefusal, "vitess.policy.file.select_into_outfile",
     "", "", "VITESS.AUTHORITY.FILE_IO_DENIED",
     "Compatibility filesystem writes are blocked from parser authority.", true, false},
    {"INSTALL PLUGIN", PatternMatch::kPrefix, "plugin", "vitess.plugin.install",
     MappingDisposition::kPolicyRefusal, "vitess.policy.plugin.install",
     "", "", "VITESS.AUTHORITY.PLUGIN_DENIED",
     "Plugin installation is blocked from parser authority.", true, false},
    {"UNINSTALL PLUGIN", PatternMatch::kPrefix, "plugin", "vitess.plugin.uninstall",
     MappingDisposition::kPolicyRefusal, "vitess.policy.plugin.uninstall",
     "", "", "VITESS.AUTHORITY.PLUGIN_DENIED",
     "Plugin removal is blocked from parser authority.", true, false},
    {"CREATE USER", PatternMatch::kPrefix, "security", "vitess.security.create_user",
     MappingDisposition::kParserSupportUdr, "vitess.udr.security.create_user",
     "SBLR_COMPAT_VITESS_SECURITY_ROUTE", "ParserSupportSecurityRoute", "VITESS.EMULATION.SECURITY_ROUTE",
     "Account management routes through trusted security policy.", true, false},
    {"ALTER USER", PatternMatch::kPrefix, "security", "vitess.security.alter_user",
     MappingDisposition::kParserSupportUdr, "vitess.udr.security.alter_user",
     "SBLR_COMPAT_VITESS_SECURITY_ROUTE", "ParserSupportSecurityRoute", "VITESS.EMULATION.SECURITY_ROUTE",
     "Account management routes through trusted security policy.", true, false},
    {"DROP USER", PatternMatch::kPrefix, "security", "vitess.security.drop_user",
     MappingDisposition::kParserSupportUdr, "vitess.udr.security.drop_user",
     "SBLR_COMPAT_VITESS_SECURITY_ROUTE", "ParserSupportSecurityRoute", "VITESS.EMULATION.SECURITY_ROUTE",
     "Account management routes through trusted security policy.", true, false},
    {"GRANT", PatternMatch::kPrefix, "security", "vitess.security.grant",
     MappingDisposition::kParserSupportUdr, "vitess.udr.security.grant",
     "SBLR_COMPAT_VITESS_SECURITY_ROUTE", "ParserSupportSecurityRoute", "VITESS.EMULATION.SECURITY_ROUTE",
     "Privilege changes route through trusted security policy.", true, false},
    {"REVOKE", PatternMatch::kPrefix, "security", "vitess.security.revoke",
     MappingDisposition::kParserSupportUdr, "vitess.udr.security.revoke",
     "SBLR_COMPAT_VITESS_SECURITY_ROUTE", "ParserSupportSecurityRoute", "VITESS.EMULATION.SECURITY_ROUTE",
     "Privilege changes route through trusted security policy.", true, false},
    {"CREATE DATABASE", PatternMatch::kPrefix, "database_lifecycle", "vitess.lifecycle.create_database",
     MappingDisposition::kScratchBirdLifecycleApi, "vitess.lifecycle.create_database",
     "SBLR_LIFECYCLE_CREATE_DATABASE", "EngineCreateLifecycle", "",
     "", false, false},
    {"DROP DATABASE", PatternMatch::kPrefix, "database_lifecycle", "vitess.lifecycle.drop_database",
     MappingDisposition::kScratchBirdLifecycleApi, "vitess.lifecycle.drop_database",
     "SBLR_LIFECYCLE_DROP_DATABASE", "EngineDropLifecycle", "",
     "", true, false},
    {"SHOW", PatternMatch::kPrefix, "catalog_overlay", "vitess.catalog_overlay.show",
     MappingDisposition::kCatalogProjection, "vitess.catalog.show",
     "SBLR_COMPAT_VITESS_CATALOG_PROJECT", "EngineCatalogProjection", "",
     "", false, false},
    {"DESCRIBE", PatternMatch::kPrefix, "catalog_overlay", "vitess.catalog_overlay.describe",
     MappingDisposition::kCatalogProjection, "vitess.catalog.describe",
     "SBLR_COMPAT_VITESS_CATALOG_PROJECT", "EngineCatalogProjection", "",
     "", false, false},
    {"EXPLAIN", PatternMatch::kPrefix, "optimizer", "vitess.optimizer.explain",
     MappingDisposition::kCatalogProjection, "vitess.optimizer.explain",
     "SBLR_COMPAT_VITESS_EXPLAIN", "EngineExplainPlan", "",
     "", false, false},
    {"SET", PatternMatch::kPrefix, "session", "vitess.session.set",
     MappingDisposition::kAdmittedSblr, "vitess.session.set",
     "SBLR_COMPAT_VITESS_SET", "EngineSessionSet", "",
     "", false, false},
    {"USE", PatternMatch::kPrefix, "session", "vitess.session.use_database",
     MappingDisposition::kAdmittedSblr, "vitess.session.use_database",
     "SBLR_COMPAT_VITESS_USE_DATABASE", "EngineSessionRoute", "",
     "", false, false},
    {"PREPARE", PatternMatch::kPrefix, "prepared_statement", "vitess.prepared.prepare",
     MappingDisposition::kAdmittedSblr, "vitess.prepared.prepare",
     "SBLR_COMPAT_VITESS_PREPARE", "EnginePrepareStatement", "",
     "", false, false},
    {"EXECUTE", PatternMatch::kPrefix, "prepared_statement", "vitess.prepared.execute",
     MappingDisposition::kAdmittedSblr, "vitess.prepared.execute",
     "SBLR_COMPAT_VITESS_EXECUTE", "EngineExecuteStatement", "",
     "", false, true},
    {"DEALLOCATE", PatternMatch::kPrefix, "prepared_statement", "vitess.prepared.deallocate",
     MappingDisposition::kAdmittedSblr, "vitess.prepared.deallocate",
     "SBLR_COMPAT_VITESS_DEALLOCATE", "EngineDeallocateStatement", "",
     "", false, false},
    {"CREATE", PatternMatch::kPrefix, "ddl", "vitess.ddl.create",
     MappingDisposition::kAdmittedSblr, "vitess.ddl.create",
     "SBLR_COMPAT_VITESS_DDL_CREATE", "EngineDdlCreate", "",
     "", true, true},
    {"ALTER", PatternMatch::kPrefix, "ddl", "vitess.ddl.alter",
     MappingDisposition::kAdmittedSblr, "vitess.ddl.alter",
     "SBLR_COMPAT_VITESS_DDL_ALTER", "EngineDdlAlter", "",
     "", true, true},
    {"DROP", PatternMatch::kPrefix, "ddl", "vitess.ddl.drop",
     MappingDisposition::kAdmittedSblr, "vitess.ddl.drop",
     "SBLR_COMPAT_VITESS_DDL_DROP", "EngineDdlDrop", "",
     "", true, true},
    {"TRUNCATE", PatternMatch::kPrefix, "ddl", "vitess.ddl.truncate",
     MappingDisposition::kAdmittedSblr, "vitess.ddl.truncate",
     "SBLR_COMPAT_VITESS_DDL_TRUNCATE", "EngineDdlTruncate", "",
     "", true, true},
    {"INSERT", PatternMatch::kPrefix, "dml", "vitess.dml.insert",
     MappingDisposition::kAdmittedSblr, "vitess.dml.insert",
     "SBLR_COMPAT_VITESS_INSERT", "EngineDmlInsert", "",
     "", false, true},
    {"UPDATE", PatternMatch::kPrefix, "dml", "vitess.dml.update",
     MappingDisposition::kAdmittedSblr, "vitess.dml.update",
     "SBLR_COMPAT_VITESS_UPDATE", "EngineDmlUpdate", "",
     "", false, true},
    {"DELETE", PatternMatch::kPrefix, "dml", "vitess.dml.delete",
     MappingDisposition::kAdmittedSblr, "vitess.dml.delete",
     "SBLR_COMPAT_VITESS_DELETE", "EngineDmlDelete", "",
     "", false, true},
    {"SELECT", PatternMatch::kPrefix, "query", "vitess.query.select",
     MappingDisposition::kAdmittedSblr, "vitess.query.select",
     "SBLR_COMPAT_VITESS_SELECT", "EngineQuerySelect", "",
     "", false, false},
    {"WITH", PatternMatch::kPrefix, "query", "vitess.query.with",
     MappingDisposition::kAdmittedSblr, "vitess.query.with",
     "SBLR_COMPAT_VITESS_SELECT", "EngineQuerySelect", "",
     "", false, false},
    {"START TRANSACTION", PatternMatch::kPrefix, "transaction", "vitess.transaction.start",
     MappingDisposition::kAdmittedSblr, "vitess.transaction.start",
     "SBLR_TRANSACTION_BEGIN", "EngineBeginTransaction", "",
     "", false, false},
    {"BEGIN", PatternMatch::kPrefix, "transaction", "vitess.transaction.begin",
     MappingDisposition::kAdmittedSblr, "vitess.transaction.begin",
     "SBLR_TRANSACTION_BEGIN", "EngineBeginTransaction", "",
     "", false, false},
    {"COMMIT", PatternMatch::kPrefix, "transaction", "vitess.transaction.commit",
     MappingDisposition::kAdmittedSblr, "vitess.transaction.commit",
     "SBLR_TRANSACTION_COMMIT", "EngineCommitTransaction", "",
     "", false, true},
    {"ROLLBACK", PatternMatch::kPrefix, "transaction", "vitess.transaction.rollback",
     MappingDisposition::kAdmittedSblr, "vitess.transaction.rollback",
     "SBLR_TRANSACTION_ROLLBACK", "EngineRollbackTransaction", "",
     "", false, true},
    {"SAVEPOINT", PatternMatch::kPrefix, "transaction", "vitess.transaction.savepoint",
     MappingDisposition::kAdmittedSblr, "vitess.transaction.savepoint",
     "SBLR_TRANSACTION_SAVEPOINT", "EngineSavepoint", "",
     "", false, true},
    {"RELEASE SAVEPOINT", PatternMatch::kPrefix, "transaction", "vitess.transaction.release_savepoint",
     MappingDisposition::kAdmittedSblr, "vitess.transaction.release_savepoint",
     "SBLR_TRANSACTION_RELEASE_SAVEPOINT", "EngineReleaseSavepoint", "",
     "", false, true},
};

const std::array<SurfaceDescriptor, 10> kDatatypeSurfaces{{
    {"numeric", "TINYINT;SMALLINT;INT;BIGINT;DECIMAL;FLOAT;DOUBLE", "descriptor"},
    {"text", "CHAR;VARCHAR;TEXT;TINYTEXT;MEDIUMTEXT;LONGTEXT", "descriptor"},
    {"binary", "BINARY;VARBINARY;BLOB", "descriptor"},
    {"temporal", "DATE;TIME;DATETIME;TIMESTAMP;YEAR", "descriptor"},
    {"boolean", "BOOL;BOOLEAN", "descriptor_alias"},
    {"json", "JSON", "descriptor"},
    {"enum_set", "ENUM;SET", "parser_support_udr"},
    {"sharding", "KEYRANGE;VINDEX", "catalog_policy"},
    {"sequence", "SEQUENCE", "descriptor"},
    {"charset_collation", "CHARACTER SET;COLLATE", "catalog_policy"},
}};

const std::array<SurfaceDescriptor, 10> kBuiltinSurfaces{{
    {"aggregate", "COUNT;SUM;AVG;MIN;MAX;GROUP_CONCAT", "sblr"},
    {"window", "ROW_NUMBER;RANK;DENSE_RANK;LAG;LEAD", "sblr"},
    {"string", "CONCAT;SUBSTRING;LOWER;UPPER;TRIM", "sblr"},
    {"numeric", "ABS;ROUND;POW;SQRT;MOD", "sblr"},
    {"temporal", "NOW;CURRENT_TIMESTAMP;DATE_ADD;DATE_SUB", "sblr"},
    {"json", "JSON_EXTRACT;JSON_VALUE;JSON_OBJECT", "parser_support_udr"},
    {"security", "CURRENT_USER;SESSION_USER;USER", "catalog_projection"},
    {"routing", "VSCHEMA;VINDEX;KEYSPACE_ID", "catalog_projection"},
    {"variables", "@user_variable;@@system_variable", "session_descriptor"},
    {"fulltext", "MATCH AGAINST", "sblr_optional"},
}};

const std::array<SurfaceDescriptor, 8> kCatalogSurfaces{{
    {"information_schema", "INFORMATION_SCHEMA.", "catalog_projection"},
    {"vt_schema", "VT_SCHEMA.", "catalog_projection"},
    {"vschema", "VSCHEMA;SHOW VSCHEMA", "catalog_projection"},
    {"keyspace", "VT_KEYSPACES;KEYSPACES", "catalog_projection"},
    {"shard", "VT_SHARDS;SHARDS", "catalog_projection"},
    {"tablet", "VT_TABLETS;TABLETS", "catalog_projection"},
    {"vreplication", "VREPLICATION;VDIFF", "catalog_projection"},
    {"routing_rules", "ROUTING_RULES;VINDEXES", "catalog_projection"},
}};

const std::array<SurfaceDescriptor, 11> kDiagnosticSurfaces{{
    {"parse", "VITESS.PARSE.INVALID_INPUT;VITESS.PARSE.UNSUPPORTED_SURFACE", "parser"},
    {"file", "VITESS.AUTHORITY.FILE_IO_DENIED", "fail_closed"},
    {"plugin", "VITESS.AUTHORITY.PLUGIN_DENIED", "fail_closed"},
    {"topology", "VITESS.AUTHORITY.TOPOLOGY_DENIED", "fail_closed"},
    {"etl", "VITESS.EMULATION.ETL_ROUTE", "parser_support_udr"},
    {"vreplication", "VITESS.EMULATION.VREPLICATION_ROUTE", "parser_support_udr"},
    {"vschema", "VITESS.EMULATION.VSCHEMA_ROUTE", "parser_support_udr"},
    {"security", "VITESS.EMULATION.SECURITY_ROUTE", "parser_support_udr"},
    {"catalog", "VITESS.CATALOG_OVERLAY.READ_ONLY", "catalog_projection"},
    {"session", "VITESS.SESSION.*", "sblr"},
    {"transaction", "VITESS.TRANSACTION.*", "sblr"},
}};

const scratchbird::parser::compatibility::DialectProfile kProfile{
    "vitess",
    "Vitess",
    "sbp_vitess",
    "sbup_vitess",
    "23.0.3",
    "VITESS",
    kSblrFamily,
    kPatterns,
    kDatatypeSurfaces,
    kBuiltinSurfaces,
    kCatalogSurfaces,
    kDiagnosticSurfaces,
    19,
    123,
    118,
    0,
    1,
    0,
    4,
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

std::string VitessPackageIdentityJson() {
  return scratchbird::parser::compatibility::PackageIdentityJson(kProfile);
}

std::string VitessSurfaceReportJson() {
  return scratchbird::parser::compatibility::SurfaceReportJson(kProfile);
}

} // namespace scratchbird::parser::vitess
