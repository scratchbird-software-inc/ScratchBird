// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "tidb_dialect.hpp"

#include <array>

namespace scratchbird::parser::tidb {
namespace {

using scratchbird::parser::compatibility::MappingDisposition;
using scratchbird::parser::compatibility::OperationPattern;
using scratchbird::parser::compatibility::PatternMatch;
using scratchbird::parser::compatibility::SurfaceDescriptor;

constexpr std::string_view kSblrFamily = "sblr.compatibility.tidb.profile.v1";

constexpr OperationPattern kPatterns[] = {
    {"ADMIN CHECKSUM", PatternMatch::kPrefix, "admin", "tidb.admin.checksum_table",
     MappingDisposition::kPolicyRefusal, "tidb.policy.admin.checksum",
     "", "", "TIDB.AUTHORITY.ADMIN_DENIED",
     "ADMIN CHECKSUM requires trusted engine administration and is blocked from parser authority.", true, false},
    {"BACKUP", PatternMatch::kPrefix, "backup_restore", "tidb.backup.backup",
     MappingDisposition::kPolicyRefusal, "tidb.policy.backup",
     "", "", "TIDB.AUTHORITY.BACKUP_DENIED",
     "Backup routes require trusted migration service admission.", true, false},
    {"RESTORE", PatternMatch::kPrefix, "backup_restore", "tidb.restore.restore",
     MappingDisposition::kPolicyRefusal, "tidb.policy.restore",
     "", "", "TIDB.AUTHORITY.RESTORE_DENIED",
     "Restore routes require trusted migration service admission.", true, false},
    {"SPLIT TABLE", PatternMatch::kPrefix, "placement", "tidb.placement.split_table",
     MappingDisposition::kAdmittedSblr, "cluster.security.validate_policy_version",
     "required_new:sblr.cluster.security.v1:cluster.security.validate_policy_version", "cluster.validate_policy_version", "",
     "", true, true},
    {"CREATE PLACEMENT POLICY", PatternMatch::kPrefix, "placement", "tidb.placement.policy.create",
     MappingDisposition::kAdmittedSblr, "cluster.security.validate_policy_version",
     "required_new:sblr.cluster.security.v1:cluster.security.validate_policy_version", "cluster.validate_policy_version", "",
     "", true, false},
    {"ALTER PLACEMENT POLICY", PatternMatch::kPrefix, "placement", "tidb.placement.policy.alter",
     MappingDisposition::kAdmittedSblr, "cluster.security.validate_policy_version",
     "required_new:sblr.cluster.security.v1:cluster.security.validate_policy_version", "cluster.validate_policy_version", "",
     "", true, false},
    {"DROP PLACEMENT POLICY", PatternMatch::kPrefix, "placement", "tidb.placement.policy.drop",
     MappingDisposition::kAdmittedSblr, "cluster.security.validate_policy_version",
     "required_new:sblr.cluster.security.v1:cluster.security.validate_policy_version", "cluster.validate_policy_version", "",
     "", true, false},
    {"CREATE RESOURCE GROUP", PatternMatch::kPrefix, "resource_control", "tidb.resource_group.create",
     MappingDisposition::kParserSupportUdr, "tidb.udr.resource_group.create",
     "SBLR_COMPAT_TIDB_RESOURCE_GROUP_ROUTE", "ParserSupportResourceGroupRoute", "TIDB.EMULATION.RESOURCE_GROUP_ROUTE",
     "Resource group policy routes through trusted operational policy.", true, false},
    {"ALTER RESOURCE GROUP", PatternMatch::kPrefix, "resource_control", "tidb.resource_group.alter",
     MappingDisposition::kParserSupportUdr, "tidb.udr.resource_group.alter",
     "SBLR_COMPAT_TIDB_RESOURCE_GROUP_ROUTE", "ParserSupportResourceGroupRoute", "TIDB.EMULATION.RESOURCE_GROUP_ROUTE",
     "Resource group policy routes through trusted operational policy.", true, false},
    {"DROP RESOURCE GROUP", PatternMatch::kPrefix, "resource_control", "tidb.resource_group.drop",
     MappingDisposition::kParserSupportUdr, "tidb.udr.resource_group.drop",
     "SBLR_COMPAT_TIDB_RESOURCE_GROUP_ROUTE", "ParserSupportResourceGroupRoute", "TIDB.EMULATION.RESOURCE_GROUP_ROUTE",
     "Resource group policy routes through trusted operational policy.", true, false},
    {"TIDB_VERSION", PatternMatch::kContains, "builtin", "tidb.builtin.version",
     MappingDisposition::kCatalogProjection, "tidb.catalog.version",
     "SBLR_COMPAT_TIDB_CATALOG_PROJECT", "EngineCatalogProjection", "",
     "", false, false},
    {"TICDC", PatternMatch::kPrefix, "cdc", "tidb.cdc.ticdc",
     MappingDisposition::kParserSupportUdr, "tidb.udr.cdc.ticdc",
     "SBLR_COMPAT_TIDB_CDC_ROUTE", "ParserSupportCdcRoute",
     "TIDB.EMULATION.CDC_ROUTE",
     "TiCDC command surfaces route through the TiDB compatibility UDR.", true, false},
    {"CHANGEFEED", PatternMatch::kPrefix, "cdc", "tidb.cdc.changefeed",
     MappingDisposition::kParserSupportUdr, "tidb.udr.cdc.changefeed",
     "SBLR_COMPAT_TIDB_CDC_ROUTE", "ParserSupportCdcRoute",
     "TIDB.EMULATION.CDC_ROUTE",
     "TiCDC changefeed command surfaces route through the TiDB compatibility UDR.", true, false},
    {"LOAD DATA LOCAL INFILE", PatternMatch::kLoadDataLocalInfile, "bulk_io", "tidb.bulk_io.load_data_local_infile",
     MappingDisposition::kParserSupportUdr, "tidb.udr.etl.load_data_local_infile",
     "SBLR_COMPAT_TIDB_ETL_ROUTE", "ParserSupportEtlRoute",
     "TIDB.EMULATION.ETL_ROUTE",
     "LOAD DATA LOCAL INFILE routes through the TiDB compatibility UDR as a client logical ETL stream.",
     true, true},
    {"LOAD DATA INFILE", PatternMatch::kLoadDataServerInfile, "bulk_io", "tidb.bulk_io.load_data_infile",
     MappingDisposition::kPolicyRefusal, "tidb.policy.file.load_data_infile",
     "", "", "TIDB.AUTHORITY.FILE_IO_DENIED",
     "File import requires a trusted ScratchBird import service.", true, false},
    {"LOAD_FILE", PatternMatch::kContainsFunctionCall, "bulk_io", "tidb.bulk_io.load_file",
     MappingDisposition::kPolicyRefusal, "tidb.policy.file.load_file",
     "", "", "TIDB.AUTHORITY.FILE_IO_DENIED",
     "Host file reads are blocked from parser authority.", true, false},
    {"SELECT|| INTO OUTFILE", PatternMatch::kPrefixAndContains, "bulk_io", "tidb.bulk_io.select_into_outfile",
     MappingDisposition::kPolicyRefusal, "tidb.policy.file.select_into_outfile",
     "", "", "TIDB.AUTHORITY.FILE_IO_DENIED",
     "Compatibility filesystem writes are blocked from parser authority.", true, false},
    {"INSTALL PLUGIN", PatternMatch::kPrefix, "plugin", "tidb.plugin.install",
     MappingDisposition::kPolicyRefusal, "tidb.policy.plugin.install",
     "", "", "TIDB.AUTHORITY.PLUGIN_DENIED",
     "Plugin installation is blocked from parser authority.", true, false},
    {"UNINSTALL PLUGIN", PatternMatch::kPrefix, "plugin", "tidb.plugin.uninstall",
     MappingDisposition::kPolicyRefusal, "tidb.policy.plugin.uninstall",
     "", "", "TIDB.AUTHORITY.PLUGIN_DENIED",
     "Plugin removal is blocked from parser authority.", true, false},
    {"CREATE USER", PatternMatch::kPrefix, "security", "tidb.security.create_user",
     MappingDisposition::kParserSupportUdr, "tidb.udr.security.create_user",
     "SBLR_COMPAT_TIDB_SECURITY_ROUTE", "ParserSupportSecurityRoute", "TIDB.EMULATION.SECURITY_ROUTE",
     "Account management routes through trusted security policy.", true, false},
    {"ALTER USER", PatternMatch::kPrefix, "security", "tidb.security.alter_user",
     MappingDisposition::kParserSupportUdr, "tidb.udr.security.alter_user",
     "SBLR_COMPAT_TIDB_SECURITY_ROUTE", "ParserSupportSecurityRoute", "TIDB.EMULATION.SECURITY_ROUTE",
     "Account management routes through trusted security policy.", true, false},
    {"DROP USER", PatternMatch::kPrefix, "security", "tidb.security.drop_user",
     MappingDisposition::kParserSupportUdr, "tidb.udr.security.drop_user",
     "SBLR_COMPAT_TIDB_SECURITY_ROUTE", "ParserSupportSecurityRoute", "TIDB.EMULATION.SECURITY_ROUTE",
     "Account management routes through trusted security policy.", true, false},
    {"GRANT", PatternMatch::kPrefix, "security", "tidb.security.grant",
     MappingDisposition::kParserSupportUdr, "tidb.udr.security.grant",
     "SBLR_COMPAT_TIDB_SECURITY_ROUTE", "ParserSupportSecurityRoute", "TIDB.EMULATION.SECURITY_ROUTE",
     "Privilege changes route through trusted security policy.", true, false},
    {"REVOKE", PatternMatch::kPrefix, "security", "tidb.security.revoke",
     MappingDisposition::kParserSupportUdr, "tidb.udr.security.revoke",
     "SBLR_COMPAT_TIDB_SECURITY_ROUTE", "ParserSupportSecurityRoute", "TIDB.EMULATION.SECURITY_ROUTE",
     "Privilege changes route through trusted security policy.", true, false},
    {"CREATE DATABASE", PatternMatch::kPrefix, "database_lifecycle", "tidb.lifecycle.create_database",
     MappingDisposition::kScratchBirdLifecycleApi, "tidb.lifecycle.create_database",
     "SBLR_LIFECYCLE_CREATE_DATABASE", "EngineCreateLifecycle", "",
     "", false, false},
    {"DROP DATABASE", PatternMatch::kPrefix, "database_lifecycle", "tidb.lifecycle.drop_database",
     MappingDisposition::kScratchBirdLifecycleApi, "tidb.lifecycle.drop_database",
     "SBLR_LIFECYCLE_DROP_DATABASE", "EngineDropLifecycle", "",
     "", true, false},
    {"SHOW", PatternMatch::kPrefix, "catalog_overlay", "tidb.catalog_overlay.show",
     MappingDisposition::kCatalogProjection, "tidb.catalog.show",
     "SBLR_COMPAT_TIDB_CATALOG_PROJECT", "EngineCatalogProjection", "",
     "", false, false},
    {"DESCRIBE", PatternMatch::kPrefix, "catalog_overlay", "tidb.catalog_overlay.describe",
     MappingDisposition::kCatalogProjection, "tidb.catalog.describe",
     "SBLR_COMPAT_TIDB_CATALOG_PROJECT", "EngineCatalogProjection", "",
     "", false, false},
    {"EXPLAIN", PatternMatch::kPrefix, "optimizer", "tidb.optimizer.explain",
     MappingDisposition::kCatalogProjection, "tidb.optimizer.explain",
     "SBLR_COMPAT_TIDB_EXPLAIN", "EngineExplainPlan", "",
     "", false, false},
    {"SET", PatternMatch::kPrefix, "session", "tidb.session.set",
     MappingDisposition::kAdmittedSblr, "tidb.session.set",
     "SBLR_COMPAT_TIDB_SET", "EngineSessionSet", "",
     "", false, false},
    {"USE", PatternMatch::kPrefix, "session", "tidb.session.use_database",
     MappingDisposition::kAdmittedSblr, "tidb.session.use_database",
     "SBLR_COMPAT_TIDB_USE_DATABASE", "EngineSessionRoute", "",
     "", false, false},
    {"PREPARE", PatternMatch::kPrefix, "prepared_statement", "tidb.prepared.prepare",
     MappingDisposition::kAdmittedSblr, "tidb.prepared.prepare",
     "SBLR_COMPAT_TIDB_PREPARE", "EnginePrepareStatement", "",
     "", false, false},
    {"EXECUTE", PatternMatch::kPrefix, "prepared_statement", "tidb.prepared.execute",
     MappingDisposition::kAdmittedSblr, "tidb.prepared.execute",
     "SBLR_COMPAT_TIDB_EXECUTE", "EngineExecuteStatement", "",
     "", false, true},
    {"DEALLOCATE", PatternMatch::kPrefix, "prepared_statement", "tidb.prepared.deallocate",
     MappingDisposition::kAdmittedSblr, "tidb.prepared.deallocate",
     "SBLR_COMPAT_TIDB_DEALLOCATE", "EngineDeallocateStatement", "",
     "", false, false},
    {"CREATE", PatternMatch::kPrefix, "ddl", "tidb.ddl.create",
     MappingDisposition::kAdmittedSblr, "tidb.ddl.create",
     "SBLR_COMPAT_TIDB_DDL_CREATE", "EngineDdlCreate", "",
     "", true, true},
    {"ALTER", PatternMatch::kPrefix, "ddl", "tidb.ddl.alter",
     MappingDisposition::kAdmittedSblr, "tidb.ddl.alter",
     "SBLR_COMPAT_TIDB_DDL_ALTER", "EngineDdlAlter", "",
     "", true, true},
    {"DROP", PatternMatch::kPrefix, "ddl", "tidb.ddl.drop",
     MappingDisposition::kAdmittedSblr, "tidb.ddl.drop",
     "SBLR_COMPAT_TIDB_DDL_DROP", "EngineDdlDrop", "",
     "", true, true},
    {"TRUNCATE", PatternMatch::kPrefix, "ddl", "tidb.ddl.truncate",
     MappingDisposition::kAdmittedSblr, "tidb.ddl.truncate",
     "SBLR_COMPAT_TIDB_DDL_TRUNCATE", "EngineDdlTruncate", "",
     "", true, true},
    {"INSERT", PatternMatch::kPrefix, "dml", "tidb.dml.insert",
     MappingDisposition::kAdmittedSblr, "tidb.dml.insert",
     "SBLR_COMPAT_TIDB_INSERT", "EngineDmlInsert", "",
     "", false, true},
    {"UPDATE", PatternMatch::kPrefix, "dml", "tidb.dml.update",
     MappingDisposition::kAdmittedSblr, "tidb.dml.update",
     "SBLR_COMPAT_TIDB_UPDATE", "EngineDmlUpdate", "",
     "", false, true},
    {"DELETE", PatternMatch::kPrefix, "dml", "tidb.dml.delete",
     MappingDisposition::kAdmittedSblr, "tidb.dml.delete",
     "SBLR_COMPAT_TIDB_DELETE", "EngineDmlDelete", "",
     "", false, true},
    {"SELECT", PatternMatch::kPrefix, "query", "tidb.query.select",
     MappingDisposition::kAdmittedSblr, "tidb.query.select",
     "SBLR_COMPAT_TIDB_SELECT", "EngineQuerySelect", "",
     "", false, false},
    {"WITH", PatternMatch::kPrefix, "query", "tidb.query.with",
     MappingDisposition::kAdmittedSblr, "tidb.query.with",
     "SBLR_COMPAT_TIDB_SELECT", "EngineQuerySelect", "",
     "", false, false},
    {"START TRANSACTION", PatternMatch::kPrefix, "transaction", "tidb.transaction.start",
     MappingDisposition::kAdmittedSblr, "tidb.transaction.start",
     "SBLR_TRANSACTION_BEGIN", "EngineBeginTransaction", "",
     "", false, false},
    {"BEGIN", PatternMatch::kPrefix, "transaction", "tidb.transaction.begin",
     MappingDisposition::kAdmittedSblr, "tidb.transaction.begin",
     "SBLR_TRANSACTION_BEGIN", "EngineBeginTransaction", "",
     "", false, false},
    {"COMMIT", PatternMatch::kPrefix, "transaction", "tidb.transaction.commit",
     MappingDisposition::kAdmittedSblr, "tidb.transaction.commit",
     "SBLR_TRANSACTION_COMMIT", "EngineCommitTransaction", "",
     "", false, true},
    {"ROLLBACK", PatternMatch::kPrefix, "transaction", "tidb.transaction.rollback",
     MappingDisposition::kAdmittedSblr, "tidb.transaction.rollback",
     "SBLR_TRANSACTION_ROLLBACK", "EngineRollbackTransaction", "",
     "", false, true},
    {"SAVEPOINT", PatternMatch::kPrefix, "transaction", "tidb.transaction.savepoint",
     MappingDisposition::kAdmittedSblr, "tidb.transaction.savepoint",
     "SBLR_TRANSACTION_SAVEPOINT", "EngineSavepoint", "",
     "", false, true},
    {"RELEASE SAVEPOINT", PatternMatch::kPrefix, "transaction", "tidb.transaction.release_savepoint",
     MappingDisposition::kAdmittedSblr, "tidb.transaction.release_savepoint",
     "SBLR_TRANSACTION_RELEASE_SAVEPOINT", "EngineReleaseSavepoint", "",
     "", false, true},
};

const std::array<SurfaceDescriptor, 12> kDatatypeSurfaces{{
    {"numeric", "TINYINT;SMALLINT;INT;BIGINT;DECIMAL;FLOAT;DOUBLE", "descriptor"},
    {"text", "CHAR;VARCHAR;TEXT;TINYTEXT;MEDIUMTEXT;LONGTEXT", "descriptor"},
    {"binary", "BINARY;VARBINARY;BLOB", "descriptor"},
    {"temporal", "DATE;TIME;DATETIME;TIMESTAMP;YEAR", "descriptor"},
    {"boolean", "BOOL;BOOLEAN", "descriptor_alias"},
    {"json", "JSON", "descriptor"},
    {"enum_set", "ENUM;SET", "parser_support_udr"},
    {"spatial", "GEOMETRY;POINT;LINESTRING;POLYGON", "parser_support_udr"},
    {"vector", "VECTOR", "parser_support_udr"},
    {"auto_random", "AUTO_RANDOM", "descriptor_policy"},
    {"placement", "PLACEMENT POLICY;RESOURCE GROUP", "catalog_policy"},
    {"charset_collation", "CHARACTER SET;COLLATE", "catalog_policy"},
}};

const std::array<SurfaceDescriptor, 12> kBuiltinSurfaces{{
    {"aggregate", "COUNT;SUM;AVG;MIN;MAX;GROUP_CONCAT", "sblr"},
    {"window", "ROW_NUMBER;RANK;DENSE_RANK;LAG;LEAD", "sblr"},
    {"string", "CONCAT;SUBSTRING;LOWER;UPPER;TRIM", "sblr"},
    {"numeric", "ABS;ROUND;POW;SQRT;MOD", "sblr"},
    {"temporal", "NOW;CURRENT_TIMESTAMP;DATE_ADD;DATE_SUB", "sblr"},
    {"json", "JSON_EXTRACT;JSON_VALUE;JSON_OBJECT", "parser_support_udr"},
    {"security", "CURRENT_USER;SESSION_USER;USER", "catalog_projection"},
    {"diagnostic", "TIDB_VERSION;TIDB_DECODE_KEY", "catalog_projection"},
    {"placement", "PLACEMENT_*", "parser_support_udr"},
    {"variables", "@user_variable;@@system_variable", "session_descriptor"},
    {"fulltext", "MATCH AGAINST", "sblr_optional"},
    {"vector", "VEC_*", "parser_support_udr"},
}};

const std::array<SurfaceDescriptor, 9> kCatalogSurfaces{{
    {"information_schema", "INFORMATION_SCHEMA.", "catalog_projection"},
    {"metrics_schema", "METRICS_SCHEMA.", "catalog_projection"},
    {"performance_schema", "PERFORMANCE_SCHEMA.", "catalog_projection"},
    {"tidb_schema", "TIDB.", "catalog_projection"},
    {"cluster_info", "CLUSTER_INFO;TIDB_SERVERS", "catalog_projection"},
    {"placement_metadata", "PLACEMENT_POLICIES;RESOURCE_GROUPS", "catalog_projection"},
    {"region_metadata", "SHOW TABLE REGIONS;SHOW TABLE TIFLASH REPLICA", "catalog_projection"},
    {"privilege_metadata", "SHOW GRANTS", "catalog_projection"},
    {"table_metadata", "SHOW COLUMNS;SHOW INDEX;DESCRIBE", "catalog_projection"},
}};

const std::array<SurfaceDescriptor, 14> kDiagnosticSurfaces{{
    {"parse", "TIDB.PARSE.INVALID_INPUT;TIDB.PARSE.UNSUPPORTED_SURFACE", "parser"},
    {"file", "TIDB.AUTHORITY.FILE_IO_DENIED", "fail_closed"},
    {"plugin", "TIDB.AUTHORITY.PLUGIN_DENIED", "fail_closed"},
    {"admin", "TIDB.AUTHORITY.ADMIN_DENIED", "fail_closed"},
    {"backup", "TIDB.AUTHORITY.BACKUP_DENIED", "fail_closed"},
    {"restore", "TIDB.AUTHORITY.RESTORE_DENIED", "fail_closed"},
    {"cdc", "TIDB.EMULATION.CDC_ROUTE", "parser_support_udr"},
    {"etl", "TIDB.EMULATION.ETL_ROUTE", "parser_support_udr"},
    {"placement", "TIDB.EMULATION.PLACEMENT_ROUTE", "parser_support_udr"},
    {"resource_group", "TIDB.EMULATION.RESOURCE_GROUP_ROUTE", "parser_support_udr"},
    {"security", "TIDB.EMULATION.SECURITY_ROUTE", "parser_support_udr"},
    {"catalog", "TIDB.CATALOG_OVERLAY.READ_ONLY", "catalog_projection"},
    {"session", "TIDB.SESSION.*", "sblr"},
    {"transaction", "TIDB.TRANSACTION.*", "sblr"},
}};

const scratchbird::parser::compatibility::DialectProfile kProfile{
    "tidb",
    "TiDB",
    "sbp_tidb",
    "sbup_tidb",
    "8.5.6",
    "TIDB",
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

std::string TidbPackageIdentityJson() {
  return scratchbird::parser::compatibility::PackageIdentityJson(kProfile);
}

std::string TidbSurfaceReportJson() {
  return scratchbird::parser::compatibility::SurfaceReportJson(kProfile);
}

} // namespace scratchbird::parser::tidb
