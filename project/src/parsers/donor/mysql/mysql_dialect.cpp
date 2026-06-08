// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "mysql_dialect.hpp"

#include <array>

namespace scratchbird::parser::mysql {
namespace {

using scratchbird::parser::donor::MappingDisposition;
using scratchbird::parser::donor::OperationPattern;
using scratchbird::parser::donor::PatternMatch;
using scratchbird::parser::donor::SurfaceDescriptor;

constexpr std::string_view kSblrFamily = "sblr.donor.mysql.profile.v1";

constexpr OperationPattern kPatterns[] = {
    {"LOAD DATA LOCAL INFILE", PatternMatch::kLoadDataLocalInfile, "bulk_io", "mysql.bulk_io.load_data_local_infile",
     MappingDisposition::kParserSupportUdr, "mysql.udr.etl.load_data_local_infile",
     "SBLR_DONOR_MYSQL_ETL_ROUTE", "ParserSupportEtlRoute",
     "MYSQL.EMULATION.ETL_ROUTE",
     "LOAD DATA LOCAL INFILE routes through the MySQL donor UDR as a client logical ETL stream.",
     true, true},
    {"LOAD DATA INFILE", PatternMatch::kLoadDataServerInfile, "bulk_io", "mysql.bulk_io.load_data_infile",
     MappingDisposition::kPolicyRefusal, "mysql.policy.file.load_data_infile", "",
     "", "MYSQL.AUTHORITY.FILE_IO_DENIED",
     "LOAD DATA INFILE is parsed but refused unless a trusted ScratchBird import service admits it.",
     true, false},
    {"LOAD_FILE", PatternMatch::kContainsFunctionCall, "bulk_io", "mysql.bulk_io.load_file",
     MappingDisposition::kPolicyRefusal, "mysql.policy.file.load_file", "",
     "", "MYSQL.AUTHORITY.FILE_IO_DENIED",
     "LOAD_FILE cannot read host files from parser authority.", true, false},
    {"SELECT|| INTO OUTFILE", PatternMatch::kPrefixAndContains, "bulk_io", "mysql.bulk_io.select_into_outfile",
     MappingDisposition::kPolicyRefusal, "mysql.policy.file.select_into_outfile", "",
     "", "MYSQL.AUTHORITY.FILE_IO_DENIED",
     "SELECT INTO OUTFILE cannot perform donor filesystem writes.", true, false},
    {"SELECT|| INTO DUMPFILE", PatternMatch::kPrefixAndContains, "bulk_io", "mysql.bulk_io.select_into_dumpfile",
     MappingDisposition::kPolicyRefusal, "mysql.policy.file.select_into_dumpfile", "",
     "", "MYSQL.AUTHORITY.FILE_IO_DENIED",
     "SELECT INTO DUMPFILE cannot perform donor filesystem writes.", true, false},
    {"INSTALL PLUGIN", PatternMatch::kPrefix, "plugin", "mysql.plugin.install",
     MappingDisposition::kPolicyRefusal, "mysql.policy.plugin.install", "",
     "", "MYSQL.AUTHORITY.PLUGIN_DENIED",
     "MySQL plugin installation is blocked from parser authority.", true, false},
    {"UNINSTALL PLUGIN", PatternMatch::kPrefix, "plugin", "mysql.plugin.uninstall",
     MappingDisposition::kPolicyRefusal, "mysql.policy.plugin.uninstall", "",
     "", "MYSQL.AUTHORITY.PLUGIN_DENIED",
     "MySQL plugin uninstallation is blocked from parser authority.", true, false},
    {"CREATE TABLESPACE", PatternMatch::kPrefix, "storage_admin", "mysql.storage.tablespace.create",
     MappingDisposition::kPolicyRefusal, "mysql.policy.tablespace.create", "",
     "", "MYSQL.AUTHORITY.TABLESPACE_DENIED",
     "Tablespace physical storage administration is not parser authority.", true, false},
    {"ALTER TABLESPACE", PatternMatch::kPrefix, "storage_admin", "mysql.storage.tablespace.alter",
     MappingDisposition::kPolicyRefusal, "mysql.policy.tablespace.alter", "",
     "", "MYSQL.AUTHORITY.TABLESPACE_DENIED",
     "Tablespace physical storage administration is not parser authority.", true, false},
    {"DROP TABLESPACE", PatternMatch::kPrefix, "storage_admin", "mysql.storage.tablespace.drop",
     MappingDisposition::kPolicyRefusal, "mysql.policy.tablespace.drop", "",
     "", "MYSQL.AUTHORITY.TABLESPACE_DENIED",
     "Tablespace physical storage administration is not parser authority.", true, false},
    {"CHANGE REPLICATION SOURCE", PatternMatch::kPrefix, "replication", "mysql.replication.change_source",
     MappingDisposition::kParserSupportUdr, "mysql.udr.replication.change_source",
     "SBLR_DONOR_MYSQL_REPLICATION_ROUTE", "ParserSupportReplicationRoute",
     "MYSQL.EMULATION.REPLICATION_ROUTE",
     "Replication source changes route through the MySQL donor UDR.", true, false},
    {"CHANGE MASTER", PatternMatch::kPrefix, "replication", "mysql.replication.change_master_legacy",
     MappingDisposition::kParserSupportUdr, "mysql.udr.replication.change_master_legacy",
     "SBLR_DONOR_MYSQL_REPLICATION_ROUTE", "ParserSupportReplicationRoute",
     "MYSQL.EMULATION.REPLICATION_ROUTE",
     "Legacy replication source changes route through the MySQL donor UDR.", true, false},
    {"START REPLICA", PatternMatch::kPrefix, "replication", "mysql.replication.start_replica",
     MappingDisposition::kParserSupportUdr, "mysql.udr.replication.start_replica",
     "SBLR_DONOR_MYSQL_REPLICATION_ROUTE", "ParserSupportReplicationRoute",
     "MYSQL.EMULATION.REPLICATION_ROUTE",
     "Replica start requests route through the MySQL donor UDR.", true, false},
    {"STOP REPLICA", PatternMatch::kPrefix, "replication", "mysql.replication.stop_replica",
     MappingDisposition::kParserSupportUdr, "mysql.udr.replication.stop_replica",
     "SBLR_DONOR_MYSQL_REPLICATION_ROUTE", "ParserSupportReplicationRoute",
     "MYSQL.EMULATION.REPLICATION_ROUTE",
     "Replica stop requests route through the MySQL donor UDR.", true, false},
    {"RESET REPLICA", PatternMatch::kPrefix, "replication", "mysql.replication.reset_replica",
     MappingDisposition::kParserSupportUdr, "mysql.udr.replication.reset_replica",
     "SBLR_DONOR_MYSQL_REPLICATION_ROUTE", "ParserSupportReplicationRoute",
     "MYSQL.EMULATION.REPLICATION_ROUTE",
     "Replica reset requests route through the MySQL donor UDR.", true, false},
    {"SHOW REPLICA STATUS", PatternMatch::kPrefix, "replication", "mysql.replication.show_replica_status",
     MappingDisposition::kParserSupportUdr, "mysql.udr.replication.show_replica_status",
     "SBLR_DONOR_MYSQL_REPLICATION_ROUTE", "ParserSupportReplicationRoute",
     "MYSQL.EMULATION.REPLICATION_ROUTE",
     "Replica status reports route through the MySQL donor UDR.", false, false},
    {"SHOW SLAVE STATUS", PatternMatch::kPrefix, "replication", "mysql.replication.show_slave_status_legacy",
     MappingDisposition::kParserSupportUdr, "mysql.udr.replication.show_slave_status_legacy",
     "SBLR_DONOR_MYSQL_REPLICATION_ROUTE", "ParserSupportReplicationRoute",
     "MYSQL.EMULATION.REPLICATION_ROUTE",
     "Legacy replica status reports route through the MySQL donor UDR.", false, false},
    {"PURGE BINARY LOGS", PatternMatch::kPrefix, "replication", "mysql.replication.purge_binary_logs",
     MappingDisposition::kParserSupportUdr, "mysql.udr.replication.purge_binary_logs",
     "SBLR_DONOR_MYSQL_REPLICATION_ROUTE", "ParserSupportReplicationRoute",
     "MYSQL.EMULATION.REPLICATION_ROUTE",
     "Binary-log CDC retention requests route through the MySQL donor UDR.", true, false},
    {"RESET BINARY LOGS", PatternMatch::kPrefix, "replication", "mysql.replication.reset_binary_logs",
     MappingDisposition::kParserSupportUdr, "mysql.udr.replication.reset_binary_logs",
     "SBLR_DONOR_MYSQL_REPLICATION_ROUTE", "ParserSupportReplicationRoute",
     "MYSQL.EMULATION.REPLICATION_ROUTE",
     "Binary-log reset requests route through the MySQL donor UDR.", true, false},
    {"CREATE USER", PatternMatch::kPrefix, "security", "mysql.security.create_user",
     MappingDisposition::kParserSupportUdr, "mysql.udr.security.create_user",
     "SBLR_DONOR_MYSQL_SECURITY_ROUTE", "ParserSupportSecurityRoute",
     "MYSQL.EMULATION.SECURITY_ROUTE",
     "Account management routes through trusted security policy.", true, false},
    {"ALTER USER", PatternMatch::kPrefix, "security", "mysql.security.alter_user",
     MappingDisposition::kParserSupportUdr, "mysql.udr.security.alter_user",
     "SBLR_DONOR_MYSQL_SECURITY_ROUTE", "ParserSupportSecurityRoute",
     "MYSQL.EMULATION.SECURITY_ROUTE",
     "Account management routes through trusted security policy.", true, false},
    {"DROP USER", PatternMatch::kPrefix, "security", "mysql.security.drop_user",
     MappingDisposition::kParserSupportUdr, "mysql.udr.security.drop_user",
     "SBLR_DONOR_MYSQL_SECURITY_ROUTE", "ParserSupportSecurityRoute",
     "MYSQL.EMULATION.SECURITY_ROUTE",
     "Account management routes through trusted security policy.", true, false},
    {"CREATE ROLE", PatternMatch::kPrefix, "security", "mysql.security.create_role",
     MappingDisposition::kParserSupportUdr, "mysql.udr.security.create_role",
     "SBLR_DONOR_MYSQL_SECURITY_ROUTE", "ParserSupportSecurityRoute",
     "MYSQL.EMULATION.SECURITY_ROUTE",
     "Role management routes through trusted security policy.", true, false},
    {"DROP ROLE", PatternMatch::kPrefix, "security", "mysql.security.drop_role",
     MappingDisposition::kParserSupportUdr, "mysql.udr.security.drop_role",
     "SBLR_DONOR_MYSQL_SECURITY_ROUTE", "ParserSupportSecurityRoute",
     "MYSQL.EMULATION.SECURITY_ROUTE",
     "Role management routes through trusted security policy.", true, false},
    {"GRANT", PatternMatch::kPrefix, "security", "mysql.security.grant",
     MappingDisposition::kParserSupportUdr, "mysql.udr.security.grant",
     "SBLR_DONOR_MYSQL_SECURITY_ROUTE", "ParserSupportSecurityRoute",
     "MYSQL.EMULATION.SECURITY_ROUTE",
     "Privilege changes route through trusted security policy.", true, false},
    {"REVOKE", PatternMatch::kPrefix, "security", "mysql.security.revoke",
     MappingDisposition::kParserSupportUdr, "mysql.udr.security.revoke",
     "SBLR_DONOR_MYSQL_SECURITY_ROUTE", "ParserSupportSecurityRoute",
     "MYSQL.EMULATION.SECURITY_ROUTE",
     "Privilege changes route through trusted security policy.", true, false},
    {"CREATE EVENT", PatternMatch::kPrefix, "routine", "mysql.routine.event.create",
     MappingDisposition::kParserSupportUdr, "mysql.udr.routine.event.create",
     "SBLR_DONOR_MYSQL_ROUTINE_ROUTE", "ParserSupportRoutineRoute",
     "MYSQL.EMULATION.ROUTINE_ROUTE",
     "Events route through trusted routine package policy.", true, false},
    {"CREATE TRIGGER", PatternMatch::kPrefix, "routine", "mysql.routine.trigger.create",
     MappingDisposition::kParserSupportUdr, "mysql.udr.routine.trigger.create",
     "SBLR_DONOR_MYSQL_ROUTINE_ROUTE", "ParserSupportRoutineRoute",
     "MYSQL.EMULATION.ROUTINE_ROUTE",
     "Triggers route through trusted routine package policy.", true, false},
    {"CREATE PROCEDURE", PatternMatch::kPrefix, "routine", "mysql.routine.procedure.create",
     MappingDisposition::kParserSupportUdr, "mysql.udr.routine.procedure.create",
     "SBLR_DONOR_MYSQL_ROUTINE_ROUTE", "ParserSupportRoutineRoute",
     "MYSQL.EMULATION.ROUTINE_ROUTE",
     "Stored procedures route through trusted routine package policy.", true, false},
    {"CREATE FUNCTION", PatternMatch::kPrefix, "routine", "mysql.routine.function.create",
     MappingDisposition::kParserSupportUdr, "mysql.udr.routine.function.create",
     "SBLR_DONOR_MYSQL_ROUTINE_ROUTE", "ParserSupportRoutineRoute",
     "MYSQL.EMULATION.ROUTINE_ROUTE",
     "Stored functions route through trusted routine package policy.", true, false},
    {"CREATE DATABASE", PatternMatch::kPrefix, "database_lifecycle", "mysql.lifecycle.create_database",
     MappingDisposition::kScratchBirdLifecycleApi, "mysql.lifecycle.create_database",
     "SBLR_LIFECYCLE_CREATE_DATABASE", "EngineCreateLifecycle", "", "", false, false},
    {"DROP DATABASE", PatternMatch::kPrefix, "database_lifecycle", "mysql.lifecycle.drop_database",
     MappingDisposition::kScratchBirdLifecycleApi, "mysql.lifecycle.drop_database",
     "SBLR_LIFECYCLE_DROP_DATABASE", "EngineDropLifecycle", "", "", true, false},
    {"USE", PatternMatch::kPrefix, "session", "mysql.session.use_database",
     MappingDisposition::kAdmittedSblr, "mysql.session.use_database",
     "SBLR_DONOR_MYSQL_USE_DATABASE", "EngineSessionRoute", "", "", false, false},
    {"SHOW", PatternMatch::kPrefix, "catalog_overlay", "mysql.catalog_overlay.show",
     MappingDisposition::kCatalogProjection, "mysql.catalog.show",
     "SBLR_DONOR_MYSQL_CATALOG_PROJECT", "EngineCatalogProjection", "", "", false, false},
    {"DESCRIBE", PatternMatch::kPrefix, "catalog_overlay", "mysql.catalog_overlay.describe",
     MappingDisposition::kCatalogProjection, "mysql.catalog.describe",
     "SBLR_DONOR_MYSQL_CATALOG_PROJECT", "EngineCatalogProjection", "", "", false, false},
    {"EXPLAIN", PatternMatch::kPrefix, "optimizer", "mysql.optimizer.explain",
     MappingDisposition::kCatalogProjection, "mysql.optimizer.explain",
     "SBLR_DONOR_MYSQL_EXPLAIN", "EngineExplainPlan", "", "", false, false},
    {"PREPARE", PatternMatch::kPrefix, "prepared_statement", "mysql.prepared.prepare",
     MappingDisposition::kAdmittedSblr, "mysql.prepared.prepare",
     "SBLR_DONOR_MYSQL_PREPARE", "EnginePrepareStatement", "", "", false, false},
    {"EXECUTE", PatternMatch::kPrefix, "prepared_statement", "mysql.prepared.execute",
     MappingDisposition::kAdmittedSblr, "mysql.prepared.execute",
     "SBLR_DONOR_MYSQL_EXECUTE", "EngineExecuteStatement", "", "", false, true},
    {"DEALLOCATE", PatternMatch::kPrefix, "prepared_statement", "mysql.prepared.deallocate",
     MappingDisposition::kAdmittedSblr, "mysql.prepared.deallocate",
     "SBLR_DONOR_MYSQL_DEALLOCATE", "EngineDeallocateStatement", "", "", false, false},
    {"LOCK TABLES", PatternMatch::kPrefix, "locking", "mysql.locking.lock_tables",
     MappingDisposition::kAdmittedSblr, "mysql.locking.lock_tables",
     "SBLR_DONOR_MYSQL_LOCK_TABLES", "EngineLockTables", "", "", true, true},
    {"UNLOCK TABLES", PatternMatch::kPrefix, "locking", "mysql.locking.unlock_tables",
     MappingDisposition::kAdmittedSblr, "mysql.locking.unlock_tables",
     "SBLR_DONOR_MYSQL_UNLOCK_TABLES", "EngineUnlockTables", "", "", true, true},
    {"START TRANSACTION", PatternMatch::kPrefix, "transaction", "mysql.transaction.start",
     MappingDisposition::kAdmittedSblr, "mysql.transaction.start",
     "SBLR_TRANSACTION_BEGIN", "EngineBeginTransaction", "", "", false, false},
    {"BEGIN", PatternMatch::kPrefix, "transaction", "mysql.transaction.begin",
     MappingDisposition::kAdmittedSblr, "mysql.transaction.begin",
     "SBLR_TRANSACTION_BEGIN", "EngineBeginTransaction", "", "", false, false},
    {"COMMIT", PatternMatch::kPrefix, "transaction", "mysql.transaction.commit",
     MappingDisposition::kAdmittedSblr, "mysql.transaction.commit",
     "SBLR_TRANSACTION_COMMIT", "EngineCommitTransaction", "", "", false, true},
    {"ROLLBACK TO SAVEPOINT", PatternMatch::kPrefix, "transaction", "mysql.transaction.rollback_to_savepoint",
     MappingDisposition::kAdmittedSblr, "mysql.transaction.rollback_to_savepoint",
     "SBLR_TRANSACTION_ROLLBACK_TO_SAVEPOINT", "EngineRollbackToSavepoint", "", "", false, true},
    {"ROLLBACK WORK TO SAVEPOINT", PatternMatch::kPrefix, "transaction", "mysql.transaction.rollback_to_savepoint",
     MappingDisposition::kAdmittedSblr, "mysql.transaction.rollback_to_savepoint",
     "SBLR_TRANSACTION_ROLLBACK_TO_SAVEPOINT", "EngineRollbackToSavepoint", "", "", false, true},
    {"ROLLBACK WORK TO", PatternMatch::kPrefix, "transaction", "mysql.transaction.rollback_to_savepoint",
     MappingDisposition::kAdmittedSblr, "mysql.transaction.rollback_to_savepoint",
     "SBLR_TRANSACTION_ROLLBACK_TO_SAVEPOINT", "EngineRollbackToSavepoint", "", "", false, true},
    {"ROLLBACK TO", PatternMatch::kPrefix, "transaction", "mysql.transaction.rollback_to_savepoint",
     MappingDisposition::kAdmittedSblr, "mysql.transaction.rollback_to_savepoint",
     "SBLR_TRANSACTION_ROLLBACK_TO_SAVEPOINT", "EngineRollbackToSavepoint", "", "", false, true},
    {"ROLLBACK", PatternMatch::kPrefix, "transaction", "mysql.transaction.rollback",
     MappingDisposition::kAdmittedSblr, "mysql.transaction.rollback",
     "SBLR_TRANSACTION_ROLLBACK", "EngineRollbackTransaction", "", "", false, true},
    {"SAVEPOINT", PatternMatch::kPrefix, "transaction", "mysql.transaction.savepoint",
     MappingDisposition::kAdmittedSblr, "mysql.transaction.savepoint",
     "SBLR_TRANSACTION_SAVEPOINT", "EngineSavepoint", "", "", false, true},
    {"RELEASE SAVEPOINT", PatternMatch::kPrefix, "transaction", "mysql.transaction.release_savepoint",
     MappingDisposition::kAdmittedSblr, "mysql.transaction.release_savepoint",
     "SBLR_TRANSACTION_RELEASE_SAVEPOINT", "EngineReleaseSavepoint", "", "", false, true},
    {"SET", PatternMatch::kPrefix, "session", "mysql.session.set",
     MappingDisposition::kAdmittedSblr, "mysql.session.set",
     "SBLR_DONOR_MYSQL_SET", "EngineSessionSet", "", "", false, false},
    {"CREATE UNIQUE INDEX", PatternMatch::kPrefix, "ddl", "mysql.ddl.create.unique_index",
     MappingDisposition::kAdmittedSblr, "mysql.ddl.create.unique_index",
     "SBLR_DONOR_MYSQL_INDEX_CREATE", "EngineDdlCreateIndex", "", "", true, true},
    {"CREATE INDEX", PatternMatch::kPrefix, "ddl", "mysql.ddl.create.index",
     MappingDisposition::kAdmittedSblr, "mysql.ddl.create.index",
     "SBLR_DONOR_MYSQL_INDEX_CREATE", "EngineDdlCreateIndex", "", "", true, true},
    {"CREATE TEMPORARY TABLE", PatternMatch::kPrefix, "ddl", "mysql.ddl.create.temporary_table",
     MappingDisposition::kAdmittedSblr, "mysql.ddl.create.temporary_table",
     "SBLR_DONOR_MYSQL_TEMPORARY_TABLE_CREATE", "EngineDdlCreateTemporaryTable",
     "", "", true, true},
    {"CREATE OR REPLACE VIEW", PatternMatch::kPrefix, "ddl", "mysql.ddl.create_or_replace.view",
     MappingDisposition::kAdmittedSblr, "mysql.ddl.create_or_replace.view",
     "SBLR_DONOR_MYSQL_VIEW_CREATE_OR_REPLACE", "EngineDdlCreateOrReplaceView",
     "", "", true, true},
    {"CREATE VIEW", PatternMatch::kPrefix, "ddl", "mysql.ddl.create.view",
     MappingDisposition::kAdmittedSblr, "mysql.ddl.create.view",
     "SBLR_DONOR_MYSQL_VIEW_CREATE", "EngineDdlCreateView", "", "", true, true},
    {"CREATE", PatternMatch::kPrefix, "ddl", "mysql.ddl.create",
     MappingDisposition::kAdmittedSblr, "mysql.ddl.create",
     "SBLR_DONOR_MYSQL_DDL_CREATE", "EngineDdlCreate", "", "", true, true},
    {"ALTER VIEW", PatternMatch::kPrefix, "ddl", "mysql.ddl.alter.view",
     MappingDisposition::kAdmittedSblr, "mysql.ddl.alter.view",
     "SBLR_DONOR_MYSQL_VIEW_ALTER", "EngineDdlAlterView", "", "", true, true},
    {"ALTER", PatternMatch::kPrefix, "ddl", "mysql.ddl.alter",
     MappingDisposition::kAdmittedSblr, "mysql.ddl.alter",
     "SBLR_DONOR_MYSQL_DDL_ALTER", "EngineDdlAlter", "", "", true, true},
    {"DROP TEMPORARY TABLE", PatternMatch::kPrefix, "ddl", "mysql.ddl.drop.temporary_table",
     MappingDisposition::kAdmittedSblr, "mysql.ddl.drop.temporary_table",
     "SBLR_DONOR_MYSQL_TEMPORARY_TABLE_DROP", "EngineDdlDropTemporaryTable",
     "", "", true, true},
    {"DROP VIEW", PatternMatch::kPrefix, "ddl", "mysql.ddl.drop.view",
     MappingDisposition::kAdmittedSblr, "mysql.ddl.drop.view",
     "SBLR_DONOR_MYSQL_VIEW_DROP", "EngineDdlDropView", "", "", true, true},
    {"DROP", PatternMatch::kPrefix, "ddl", "mysql.ddl.drop",
     MappingDisposition::kAdmittedSblr, "mysql.ddl.drop",
     "SBLR_DONOR_MYSQL_DDL_DROP", "EngineDdlDrop", "", "", true, true},
    {"TRUNCATE", PatternMatch::kPrefix, "ddl", "mysql.ddl.truncate",
     MappingDisposition::kAdmittedSblr, "mysql.ddl.truncate",
     "SBLR_DONOR_MYSQL_DDL_TRUNCATE", "EngineDdlTruncate", "", "", true, true},
    {"REPLACE", PatternMatch::kPrefix, "dml", "mysql.dml.replace",
     MappingDisposition::kAdmittedSblr, "mysql.dml.replace",
     "SBLR_DONOR_MYSQL_REPLACE", "EngineDmlReplace", "", "", false, true},
    {"INSERT", PatternMatch::kPrefix, "dml", "mysql.dml.insert",
     MappingDisposition::kAdmittedSblr, "mysql.dml.insert",
     "SBLR_DONOR_MYSQL_INSERT", "EngineDmlInsert", "", "", false, true},
    {"UPDATE", PatternMatch::kPrefix, "dml", "mysql.dml.update",
     MappingDisposition::kAdmittedSblr, "mysql.dml.update",
     "SBLR_DONOR_MYSQL_UPDATE", "EngineDmlUpdate", "", "", false, true},
    {"DELETE", PatternMatch::kPrefix, "dml", "mysql.dml.delete",
     MappingDisposition::kAdmittedSblr, "mysql.dml.delete",
     "SBLR_DONOR_MYSQL_DELETE", "EngineDmlDelete", "", "", false, true},
    {"SELECT", PatternMatch::kPrefix, "query", "mysql.query.select",
     MappingDisposition::kAdmittedSblr, "mysql.query.select",
     "SBLR_DONOR_MYSQL_SELECT", "EngineQuerySelect", "", "", false, false},
    {"WITH", PatternMatch::kPrefix, "query", "mysql.query.with",
     MappingDisposition::kAdmittedSblr, "mysql.query.with",
     "SBLR_DONOR_MYSQL_SELECT", "EngineQuerySelect", "", "", false, false},
    {"CALL", PatternMatch::kPrefix, "routine", "mysql.routine.call",
     MappingDisposition::kParserSupportUdr, "mysql.udr.routine.call",
     "SBLR_DONOR_MYSQL_ROUTINE_CALL", "ParserSupportRoutineRoute",
     "MYSQL.EMULATION.ROUTINE_ROUTE",
     "Routine calls route through trusted package policy.", true, true},
    {"ANALYZE TABLE", PatternMatch::kPrefix, "maintenance", "mysql.maintenance.analyze_table",
     MappingDisposition::kUnsupportedRefusal, "mysql.policy.unsupported.analyze_table",
     "", "", "MYSQL.AUTHORITY.UNSUPPORTED_DENIED",
     "MySQL ANALYZE TABLE is a donor low-level utility surface and is outside donor parser authority.",
     true, false},
    {"OPTIMIZE TABLE", PatternMatch::kPrefix, "maintenance", "mysql.maintenance.optimize_table",
     MappingDisposition::kUnsupportedRefusal, "mysql.policy.unsupported.optimize_table",
     "", "", "MYSQL.AUTHORITY.UNSUPPORTED_DENIED",
     "MySQL OPTIMIZE TABLE is a donor low-level utility surface and is outside donor parser authority.",
     true, false},
    {"CHECK TABLE", PatternMatch::kPrefix, "maintenance", "mysql.maintenance.check_table",
     MappingDisposition::kUnsupportedRefusal, "mysql.policy.unsupported.check_table",
     "", "", "MYSQL.AUTHORITY.UNSUPPORTED_DENIED",
     "MySQL CHECK TABLE is a donor verification utility surface and is outside donor parser authority.",
     true, false},
    {"REPAIR TABLE", PatternMatch::kPrefix, "maintenance", "mysql.maintenance.repair_table",
     MappingDisposition::kUnsupportedRefusal, "mysql.policy.unsupported.repair_table",
     "", "", "MYSQL.AUTHORITY.UNSUPPORTED_DENIED",
     "MySQL REPAIR TABLE is a donor repair utility surface and is outside donor parser authority.",
     true, false},
    {"FLUSH", PatternMatch::kPrefix, "maintenance", "mysql.maintenance.flush",
     MappingDisposition::kUnsupportedRefusal, "mysql.policy.unsupported.flush",
     "", "", "MYSQL.AUTHORITY.UNSUPPORTED_DENIED",
     "MySQL FLUSH is a donor low-level utility surface and is outside donor parser authority.",
     true, false},
    {"XA", PatternMatch::kPrefix, "transaction", "mysql.transaction.xa",
     MappingDisposition::kUnsupportedRefusal, "mysql.policy.transaction.xa", "",
     "", "MYSQL.AUTHORITY.XA_DENIED",
     "XA distributed transaction authority is not admitted by the parser.", true, true},
};

const std::array<SurfaceDescriptor, 10> kDatatypeSurfaces{{
    {"numeric", "TINYINT;SMALLINT;MEDIUMINT;INT;BIGINT;DECIMAL;FLOAT;DOUBLE", "descriptor"},
    {"unsigned_numeric", "UNSIGNED;ZEROFILL", "descriptor_policy"},
    {"text", "CHAR;VARCHAR;TEXT;TINYTEXT;MEDIUMTEXT;LONGTEXT", "descriptor"},
    {"binary", "BINARY;VARBINARY;BLOB;TINYBLOB;MEDIUMBLOB;LONGBLOB", "descriptor"},
    {"temporal", "DATE;TIME;DATETIME;TIMESTAMP;YEAR", "descriptor"},
    {"boolean", "BOOL;BOOLEAN", "descriptor_alias"},
    {"json", "JSON", "descriptor"},
    {"enum_set", "ENUM;SET", "parser_support_udr"},
    {"spatial", "GEOMETRY;POINT;LINESTRING;POLYGON", "parser_support_udr"},
    {"charset_collation", "CHARACTER SET;COLLATE", "catalog_policy"},
}};

const std::array<SurfaceDescriptor, 10> kBuiltinSurfaces{{
    {"aggregate", "COUNT;SUM;AVG;MIN;MAX;GROUP_CONCAT", "sblr"},
    {"window", "ROW_NUMBER;RANK;DENSE_RANK;LAG;LEAD", "sblr"},
    {"string", "CONCAT;SUBSTRING;LOWER;UPPER;TRIM;CHAR_LENGTH", "sblr"},
    {"numeric", "ABS;ROUND;POW;SQRT;MOD", "sblr"},
    {"temporal", "NOW;CURRENT_TIMESTAMP;DATE_ADD;DATE_SUB;TIMESTAMPDIFF", "sblr"},
    {"json", "JSON_EXTRACT;JSON_VALUE;JSON_TABLE;JSON_OBJECT", "parser_support_udr"},
    {"security", "CURRENT_USER;SESSION_USER;USER", "catalog_projection"},
    {"variables", "@user_variable;@@system_variable", "session_descriptor"},
    {"fulltext", "MATCH AGAINST", "sblr_optional"},
    {"spatial", "ST_*", "parser_support_udr"},
}};

const std::array<SurfaceDescriptor, 8> kCatalogSurfaces{{
    {"information_schema", "INFORMATION_SCHEMA.", "catalog_projection"},
    {"mysql_schema", "MYSQL.USER;MYSQL.DB;MYSQL.TABLES_PRIV;MYSQL.PROCS_PRIV", "catalog_projection"},
    {"performance_schema", "PERFORMANCE_SCHEMA.", "catalog_projection"},
    {"sys_schema", "SYS.", "catalog_projection"},
    {"replication_status", "SHOW REPLICA STATUS;SHOW BINARY LOGS", "catalog_projection"},
    {"routine_metadata", "INFORMATION_SCHEMA.ROUTINES;TRIGGERS;EVENTS", "catalog_projection"},
    {"table_metadata", "SHOW COLUMNS;SHOW INDEX;DESCRIBE", "catalog_projection"},
    {"privilege_metadata", "SHOW GRANTS", "catalog_projection"},
}};

const std::array<SurfaceDescriptor, 11> kDiagnosticSurfaces{{
    {"parse", "MYSQL.PARSE.INVALID_INPUT;MYSQL.PARSE.UNSUPPORTED_SURFACE", "parser"},
    {"file", "MYSQL.AUTHORITY.FILE_IO_DENIED", "fail_closed"},
    {"plugin", "MYSQL.AUTHORITY.PLUGIN_DENIED", "fail_closed"},
    {"tablespace", "MYSQL.AUTHORITY.TABLESPACE_DENIED", "fail_closed"},
    {"etl", "MYSQL.EMULATION.ETL_ROUTE", "parser_support_udr"},
    {"replication", "MYSQL.EMULATION.REPLICATION_ROUTE", "parser_support_udr"},
    {"security", "MYSQL.EMULATION.SECURITY_ROUTE", "parser_support_udr"},
    {"routine", "MYSQL.EMULATION.ROUTINE_ROUTE", "parser_support_udr"},
    {"maintenance", "MYSQL.AUTHORITY.UNSUPPORTED_DENIED", "fail_closed"},
    {"binlog", "MYSQL.AUTHORITY.BINLOG_DENIED", "fail_closed"},
    {"xa", "MYSQL.AUTHORITY.XA_DENIED", "fail_closed"},
}};

const scratchbird::parser::donor::DialectProfile kProfile{
    "mysql",
    "MySQL",
    "sbp_mysql",
    "sbup_mysql",
    "9.7.0",
    "MYSQL",
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

std::string MysqlPackageIdentityJson() {
  return scratchbird::parser::donor::PackageIdentityJson(kProfile);
}

std::string MysqlSurfaceReportJson() {
  return scratchbird::parser::donor::SurfaceReportJson(kProfile);
}

} // namespace scratchbird::parser::mysql
