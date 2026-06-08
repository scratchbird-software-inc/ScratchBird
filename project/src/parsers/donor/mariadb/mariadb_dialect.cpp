// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "mariadb_dialect.hpp"

#include <array>

namespace scratchbird::parser::mariadb {
namespace {

using scratchbird::parser::donor::MappingDisposition;
using scratchbird::parser::donor::OperationPattern;
using scratchbird::parser::donor::PatternMatch;
using scratchbird::parser::donor::SurfaceDescriptor;

constexpr std::string_view kSblrFamily = "sblr.donor.mariadb.profile.v1";

constexpr OperationPattern kPatterns[] = {
    {"INSTALL SONAME", PatternMatch::kPrefix, "plugin", "mariadb.plugin.install_soname",
     MappingDisposition::kPolicyRefusal, "mariadb.policy.plugin.install_soname", "",
     "", "MARIADB.AUTHORITY.PLUGIN_DENIED",
     "MariaDB SONAME plugin installation is blocked from parser authority.", true, false},
    {"UNINSTALL SONAME", PatternMatch::kPrefix, "plugin", "mariadb.plugin.uninstall_soname",
     MappingDisposition::kPolicyRefusal, "mariadb.policy.plugin.uninstall_soname", "",
     "", "MARIADB.AUTHORITY.PLUGIN_DENIED",
     "MariaDB SONAME plugin removal is blocked from parser authority.", true, false},
    {"BACKUP STAGE", PatternMatch::kPrefix, "bulk_io", "mariadb.bulk_io.backup_stage",
     MappingDisposition::kPolicyRefusal, "mariadb.policy.backup_stage", "",
     "", "MARIADB.AUTHORITY.FILE_IO_DENIED",
     "MariaDB backup stage file effects require trusted engine lifecycle admission.", true, false},
    {"CREATE SEQUENCE", PatternMatch::kPrefix, "sequence", "mariadb.sequence.create",
     MappingDisposition::kAdmittedSblr, "mariadb.sequence.create",
     "SBLR_DONOR_MARIADB_SEQUENCE_CREATE", "EngineCreateSequence", "", "", true, true},
    {"ALTER SEQUENCE", PatternMatch::kPrefix, "sequence", "mariadb.sequence.alter",
     MappingDisposition::kAdmittedSblr, "mariadb.sequence.alter",
     "SBLR_DONOR_MARIADB_SEQUENCE_ALTER", "EngineAlterSequence", "", "", true, true},
    {"DROP SEQUENCE", PatternMatch::kPrefix, "sequence", "mariadb.sequence.drop",
     MappingDisposition::kAdmittedSblr, "mariadb.sequence.drop",
     "SBLR_DONOR_MARIADB_SEQUENCE_DROP", "EngineDropSequence", "", "", true, true},
    {"NEXT VALUE FOR", PatternMatch::kContains, "sequence", "mariadb.sequence.next_value_for",
     MappingDisposition::kAdmittedSblr, "mariadb.sequence.next_value_for",
     "SBLR_DONOR_MARIADB_SEQUENCE_NEXT_VALUE", "EngineNextSequenceValue", "", "", false, true},
    {"RETURNING", PatternMatch::kWord, "dml", "mariadb.dml.returning",
     MappingDisposition::kAdmittedSblr, "mariadb.dml.returning",
     "SBLR_DONOR_MARIADB_DML_RETURNING", "EngineDmlReturning", "", "", false, true},
    {"HANDLER", PatternMatch::kPrefix, "handler", "mariadb.handler.cursor",
     MappingDisposition::kParserSupportUdr, "mariadb.udr.handler.cursor",
     "SBLR_DONOR_MARIADB_HANDLER_ROUTE", "ParserSupportHandlerRoute",
     "MARIADB.EMULATION.HANDLER_ROUTE",
     "MariaDB HANDLER cursor operations route through trusted parser-support policy.", true, true},
    {"KILL", PatternMatch::kPrefix, "session_admin", "mariadb.session_admin.kill",
     MappingDisposition::kSecurityRefusal, "mariadb.policy.kill", "",
     "", "MARIADB.AUTHORITY.SESSION_ADMIN_DENIED",
     "KILL cannot terminate sessions from parser authority.", true, false},
    {"RESET MASTER", PatternMatch::kPrefix, "replication", "mariadb.replication.reset_master",
     MappingDisposition::kParserSupportUdr, "mariadb.udr.replication.reset_master",
     "SBLR_DONOR_MARIADB_REPLICATION_ROUTE", "ParserSupportReplicationRoute",
     "MARIADB.EMULATION.REPLICATION_ROUTE",
     "Replication reset requests route through the MariaDB donor UDR.", true, false},
    {"CREATE OR REPLACE", PatternMatch::kPrefix, "ddl", "mariadb.ddl.create_or_replace",
     MappingDisposition::kAdmittedSblr, "mariadb.ddl.create_or_replace",
     "SBLR_DONOR_MARIADB_DDL_CREATE_OR_REPLACE", "EngineDdlCreateOrReplace", "", "", true, true},
    {"LOAD DATA LOCAL INFILE", PatternMatch::kLoadDataLocalInfile, "bulk_io", "mariadb.bulk_io.load_data_local_infile",
     MappingDisposition::kParserSupportUdr, "mariadb.udr.etl.load_data_local_infile",
     "SBLR_DONOR_MARIADB_ETL_ROUTE", "ParserSupportEtlRoute",
     "MARIADB.EMULATION.ETL_ROUTE",
     "LOAD DATA LOCAL INFILE routes through the MariaDB donor UDR as a client logical ETL stream.",
     true, true},
    {"LOAD DATA INFILE", PatternMatch::kLoadDataServerInfile, "bulk_io", "mariadb.bulk_io.load_data_infile",
     MappingDisposition::kPolicyRefusal, "mariadb.policy.file.load_data_infile", "",
     "", "MARIADB.AUTHORITY.FILE_IO_DENIED",
     "LOAD DATA INFILE is parsed but refused unless a trusted ScratchBird import service admits it.",
     true, false},
    {"LOAD_FILE", PatternMatch::kContainsFunctionCall, "bulk_io", "mariadb.bulk_io.load_file",
     MappingDisposition::kPolicyRefusal, "mariadb.policy.file.load_file", "",
     "", "MARIADB.AUTHORITY.FILE_IO_DENIED",
     "LOAD_FILE cannot read host files from parser authority.", true, false},
    {"SELECT|| INTO OUTFILE", PatternMatch::kPrefixAndContains, "bulk_io", "mariadb.bulk_io.select_into_outfile",
     MappingDisposition::kPolicyRefusal, "mariadb.policy.file.select_into_outfile", "",
     "", "MARIADB.AUTHORITY.FILE_IO_DENIED",
     "SELECT INTO OUTFILE cannot perform donor filesystem writes.", true, false},
    {"SELECT|| INTO DUMPFILE", PatternMatch::kPrefixAndContains, "bulk_io", "mariadb.bulk_io.select_into_dumpfile",
     MappingDisposition::kPolicyRefusal, "mariadb.policy.file.select_into_dumpfile", "",
     "", "MARIADB.AUTHORITY.FILE_IO_DENIED",
     "SELECT INTO DUMPFILE cannot perform donor filesystem writes.", true, false},
    {"INSTALL PLUGIN", PatternMatch::kPrefix, "plugin", "mariadb.plugin.install",
     MappingDisposition::kPolicyRefusal, "mariadb.policy.plugin.install", "",
     "", "MARIADB.AUTHORITY.PLUGIN_DENIED",
     "MariaDB plugin installation is blocked from parser authority.", true, false},
    {"UNINSTALL PLUGIN", PatternMatch::kPrefix, "plugin", "mariadb.plugin.uninstall",
     MappingDisposition::kPolicyRefusal, "mariadb.policy.plugin.uninstall", "",
     "", "MARIADB.AUTHORITY.PLUGIN_DENIED",
     "MariaDB plugin uninstallation is blocked from parser authority.", true, false},
    {"CREATE TABLESPACE", PatternMatch::kPrefix, "storage_admin", "mariadb.storage.tablespace.create",
     MappingDisposition::kPolicyRefusal, "mariadb.policy.tablespace.create", "",
     "", "MARIADB.AUTHORITY.TABLESPACE_DENIED",
     "Tablespace physical storage administration is not parser authority.", true, false},
    {"ALTER TABLESPACE", PatternMatch::kPrefix, "storage_admin", "mariadb.storage.tablespace.alter",
     MappingDisposition::kPolicyRefusal, "mariadb.policy.tablespace.alter", "",
     "", "MARIADB.AUTHORITY.TABLESPACE_DENIED",
     "Tablespace physical storage administration is not parser authority.", true, false},
    {"DROP TABLESPACE", PatternMatch::kPrefix, "storage_admin", "mariadb.storage.tablespace.drop",
     MappingDisposition::kPolicyRefusal, "mariadb.policy.tablespace.drop", "",
     "", "MARIADB.AUTHORITY.TABLESPACE_DENIED",
     "Tablespace physical storage administration is not parser authority.", true, false},
    {"CHANGE REPLICATION SOURCE", PatternMatch::kPrefix, "replication", "mariadb.replication.change_source",
     MappingDisposition::kParserSupportUdr, "mariadb.udr.replication.change_source",
     "SBLR_DONOR_MARIADB_REPLICATION_ROUTE", "ParserSupportReplicationRoute",
     "MARIADB.EMULATION.REPLICATION_ROUTE",
     "Replication source changes route through the MariaDB donor UDR.", true, false},
    {"CHANGE MASTER", PatternMatch::kPrefix, "replication", "mariadb.replication.change_master_legacy",
     MappingDisposition::kParserSupportUdr, "mariadb.udr.replication.change_master_legacy",
     "SBLR_DONOR_MARIADB_REPLICATION_ROUTE", "ParserSupportReplicationRoute",
     "MARIADB.EMULATION.REPLICATION_ROUTE",
     "Legacy replication source changes route through the MariaDB donor UDR.", true, false},
    {"START REPLICA", PatternMatch::kPrefix, "replication", "mariadb.replication.start_replica",
     MappingDisposition::kParserSupportUdr, "mariadb.udr.replication.start_replica",
     "SBLR_DONOR_MARIADB_REPLICATION_ROUTE", "ParserSupportReplicationRoute",
     "MARIADB.EMULATION.REPLICATION_ROUTE",
     "Replica start requests route through the MariaDB donor UDR.", true, false},
    {"STOP REPLICA", PatternMatch::kPrefix, "replication", "mariadb.replication.stop_replica",
     MappingDisposition::kParserSupportUdr, "mariadb.udr.replication.stop_replica",
     "SBLR_DONOR_MARIADB_REPLICATION_ROUTE", "ParserSupportReplicationRoute",
     "MARIADB.EMULATION.REPLICATION_ROUTE",
     "Replica stop requests route through the MariaDB donor UDR.", true, false},
    {"RESET REPLICA", PatternMatch::kPrefix, "replication", "mariadb.replication.reset_replica",
     MappingDisposition::kParserSupportUdr, "mariadb.udr.replication.reset_replica",
     "SBLR_DONOR_MARIADB_REPLICATION_ROUTE", "ParserSupportReplicationRoute",
     "MARIADB.EMULATION.REPLICATION_ROUTE",
     "Replica reset requests route through the MariaDB donor UDR.", true, false},
    {"SHOW REPLICA STATUS", PatternMatch::kPrefix, "replication", "mariadb.replication.show_replica_status",
     MappingDisposition::kParserSupportUdr, "mariadb.udr.replication.show_replica_status",
     "SBLR_DONOR_MARIADB_REPLICATION_ROUTE", "ParserSupportReplicationRoute",
     "MARIADB.EMULATION.REPLICATION_ROUTE",
     "Replica status reports route through the MariaDB donor UDR.", false, false},
    {"SHOW SLAVE STATUS", PatternMatch::kPrefix, "replication", "mariadb.replication.show_slave_status_legacy",
     MappingDisposition::kParserSupportUdr, "mariadb.udr.replication.show_slave_status_legacy",
     "SBLR_DONOR_MARIADB_REPLICATION_ROUTE", "ParserSupportReplicationRoute",
     "MARIADB.EMULATION.REPLICATION_ROUTE",
     "Legacy replica status reports route through the MariaDB donor UDR.", false, false},
    {"PURGE BINARY LOGS", PatternMatch::kPrefix, "replication", "mariadb.replication.purge_binary_logs",
     MappingDisposition::kParserSupportUdr, "mariadb.udr.replication.purge_binary_logs",
     "SBLR_DONOR_MARIADB_REPLICATION_ROUTE", "ParserSupportReplicationRoute",
     "MARIADB.EMULATION.REPLICATION_ROUTE",
     "Binary-log CDC retention requests route through the MariaDB donor UDR.", true, false},
    {"RESET BINARY LOGS", PatternMatch::kPrefix, "replication", "mariadb.replication.reset_binary_logs",
     MappingDisposition::kParserSupportUdr, "mariadb.udr.replication.reset_binary_logs",
     "SBLR_DONOR_MARIADB_REPLICATION_ROUTE", "ParserSupportReplicationRoute",
     "MARIADB.EMULATION.REPLICATION_ROUTE",
     "Binary-log reset requests route through the MariaDB donor UDR.", true, false},
    {"CREATE USER", PatternMatch::kPrefix, "security", "mariadb.security.create_user",
     MappingDisposition::kParserSupportUdr, "mariadb.udr.security.create_user",
     "SBLR_DONOR_MARIADB_SECURITY_ROUTE", "ParserSupportSecurityRoute",
     "MARIADB.EMULATION.SECURITY_ROUTE",
     "Account management routes through trusted security policy.", true, false},
    {"ALTER USER", PatternMatch::kPrefix, "security", "mariadb.security.alter_user",
     MappingDisposition::kParserSupportUdr, "mariadb.udr.security.alter_user",
     "SBLR_DONOR_MARIADB_SECURITY_ROUTE", "ParserSupportSecurityRoute",
     "MARIADB.EMULATION.SECURITY_ROUTE",
     "Account management routes through trusted security policy.", true, false},
    {"DROP USER", PatternMatch::kPrefix, "security", "mariadb.security.drop_user",
     MappingDisposition::kParserSupportUdr, "mariadb.udr.security.drop_user",
     "SBLR_DONOR_MARIADB_SECURITY_ROUTE", "ParserSupportSecurityRoute",
     "MARIADB.EMULATION.SECURITY_ROUTE",
     "Account management routes through trusted security policy.", true, false},
    {"CREATE ROLE", PatternMatch::kPrefix, "security", "mariadb.security.create_role",
     MappingDisposition::kParserSupportUdr, "mariadb.udr.security.create_role",
     "SBLR_DONOR_MARIADB_SECURITY_ROUTE", "ParserSupportSecurityRoute",
     "MARIADB.EMULATION.SECURITY_ROUTE",
     "Role management routes through trusted security policy.", true, false},
    {"DROP ROLE", PatternMatch::kPrefix, "security", "mariadb.security.drop_role",
     MappingDisposition::kParserSupportUdr, "mariadb.udr.security.drop_role",
     "SBLR_DONOR_MARIADB_SECURITY_ROUTE", "ParserSupportSecurityRoute",
     "MARIADB.EMULATION.SECURITY_ROUTE",
     "Role management routes through trusted security policy.", true, false},
    {"GRANT", PatternMatch::kPrefix, "security", "mariadb.security.grant",
     MappingDisposition::kParserSupportUdr, "mariadb.udr.security.grant",
     "SBLR_DONOR_MARIADB_SECURITY_ROUTE", "ParserSupportSecurityRoute",
     "MARIADB.EMULATION.SECURITY_ROUTE",
     "Privilege changes route through trusted security policy.", true, false},
    {"REVOKE", PatternMatch::kPrefix, "security", "mariadb.security.revoke",
     MappingDisposition::kParserSupportUdr, "mariadb.udr.security.revoke",
     "SBLR_DONOR_MARIADB_SECURITY_ROUTE", "ParserSupportSecurityRoute",
     "MARIADB.EMULATION.SECURITY_ROUTE",
     "Privilege changes route through trusted security policy.", true, false},
    {"CREATE EVENT", PatternMatch::kPrefix, "routine", "mariadb.routine.event.create",
     MappingDisposition::kParserSupportUdr, "mariadb.udr.routine.event.create",
     "SBLR_DONOR_MARIADB_ROUTINE_ROUTE", "ParserSupportRoutineRoute",
     "MARIADB.EMULATION.ROUTINE_ROUTE",
     "Events route through trusted routine package policy.", true, false},
    {"CREATE TRIGGER", PatternMatch::kPrefix, "routine", "mariadb.routine.trigger.create",
     MappingDisposition::kParserSupportUdr, "mariadb.udr.routine.trigger.create",
     "SBLR_DONOR_MARIADB_ROUTINE_ROUTE", "ParserSupportRoutineRoute",
     "MARIADB.EMULATION.ROUTINE_ROUTE",
     "Triggers route through trusted routine package policy.", true, false},
    {"CREATE PROCEDURE", PatternMatch::kPrefix, "routine", "mariadb.routine.procedure.create",
     MappingDisposition::kParserSupportUdr, "mariadb.udr.routine.procedure.create",
     "SBLR_DONOR_MARIADB_ROUTINE_ROUTE", "ParserSupportRoutineRoute",
     "MARIADB.EMULATION.ROUTINE_ROUTE",
     "Stored procedures route through trusted routine package policy.", true, false},
    {"CREATE FUNCTION", PatternMatch::kPrefix, "routine", "mariadb.routine.function.create",
     MappingDisposition::kParserSupportUdr, "mariadb.udr.routine.function.create",
     "SBLR_DONOR_MARIADB_ROUTINE_ROUTE", "ParserSupportRoutineRoute",
     "MARIADB.EMULATION.ROUTINE_ROUTE",
     "Stored functions route through trusted routine package policy.", true, false},
    {"CREATE DATABASE", PatternMatch::kPrefix, "database_lifecycle", "mariadb.lifecycle.create_database",
     MappingDisposition::kScratchBirdLifecycleApi, "mariadb.lifecycle.create_database",
     "SBLR_LIFECYCLE_CREATE_DATABASE", "EngineCreateLifecycle", "", "", false, false},
    {"DROP DATABASE", PatternMatch::kPrefix, "database_lifecycle", "mariadb.lifecycle.drop_database",
     MappingDisposition::kScratchBirdLifecycleApi, "mariadb.lifecycle.drop_database",
     "SBLR_LIFECYCLE_DROP_DATABASE", "EngineDropLifecycle", "", "", true, false},
    {"USE", PatternMatch::kPrefix, "session", "mariadb.session.use_database",
     MappingDisposition::kAdmittedSblr, "mariadb.session.use_database",
     "SBLR_DONOR_MARIADB_USE_DATABASE", "EngineSessionRoute", "", "", false, false},
    {"SHOW", PatternMatch::kPrefix, "catalog_overlay", "mariadb.catalog_overlay.show",
     MappingDisposition::kCatalogProjection, "mariadb.catalog.show",
     "SBLR_DONOR_MARIADB_CATALOG_PROJECT", "EngineCatalogProjection", "", "", false, false},
    {"DESCRIBE", PatternMatch::kPrefix, "catalog_overlay", "mariadb.catalog_overlay.describe",
     MappingDisposition::kCatalogProjection, "mariadb.catalog.describe",
     "SBLR_DONOR_MARIADB_CATALOG_PROJECT", "EngineCatalogProjection", "", "", false, false},
    {"EXPLAIN", PatternMatch::kPrefix, "optimizer", "mariadb.optimizer.explain",
     MappingDisposition::kCatalogProjection, "mariadb.optimizer.explain",
     "SBLR_DONOR_MARIADB_EXPLAIN", "EngineExplainPlan", "", "", false, false},
    {"PREPARE", PatternMatch::kPrefix, "prepared_statement", "mariadb.prepared.prepare",
     MappingDisposition::kAdmittedSblr, "mariadb.prepared.prepare",
     "SBLR_DONOR_MARIADB_PREPARE", "EnginePrepareStatement", "", "", false, false},
    {"EXECUTE", PatternMatch::kPrefix, "prepared_statement", "mariadb.prepared.execute",
     MappingDisposition::kAdmittedSblr, "mariadb.prepared.execute",
     "SBLR_DONOR_MARIADB_EXECUTE", "EngineExecuteStatement", "", "", false, true},
    {"DEALLOCATE", PatternMatch::kPrefix, "prepared_statement", "mariadb.prepared.deallocate",
     MappingDisposition::kAdmittedSblr, "mariadb.prepared.deallocate",
     "SBLR_DONOR_MARIADB_DEALLOCATE", "EngineDeallocateStatement", "", "", false, false},
    {"LOCK TABLES", PatternMatch::kPrefix, "locking", "mariadb.locking.lock_tables",
     MappingDisposition::kAdmittedSblr, "mariadb.locking.lock_tables",
     "SBLR_DONOR_MARIADB_LOCK_TABLES", "EngineLockTables", "", "", true, true},
    {"UNLOCK TABLES", PatternMatch::kPrefix, "locking", "mariadb.locking.unlock_tables",
     MappingDisposition::kAdmittedSblr, "mariadb.locking.unlock_tables",
     "SBLR_DONOR_MARIADB_UNLOCK_TABLES", "EngineUnlockTables", "", "", true, true},
    {"START TRANSACTION", PatternMatch::kPrefix, "transaction", "mariadb.transaction.start",
     MappingDisposition::kAdmittedSblr, "mariadb.transaction.start",
     "SBLR_TRANSACTION_BEGIN", "EngineBeginTransaction", "", "", false, false},
    {"BEGIN", PatternMatch::kPrefix, "transaction", "mariadb.transaction.begin",
     MappingDisposition::kAdmittedSblr, "mariadb.transaction.begin",
     "SBLR_TRANSACTION_BEGIN", "EngineBeginTransaction", "", "", false, false},
    {"COMMIT", PatternMatch::kPrefix, "transaction", "mariadb.transaction.commit",
     MappingDisposition::kAdmittedSblr, "mariadb.transaction.commit",
     "SBLR_TRANSACTION_COMMIT", "EngineCommitTransaction", "", "", false, true},
    {"ROLLBACK", PatternMatch::kPrefix, "transaction", "mariadb.transaction.rollback",
     MappingDisposition::kAdmittedSblr, "mariadb.transaction.rollback",
     "SBLR_TRANSACTION_ROLLBACK", "EngineRollbackTransaction", "", "", false, true},
    {"SAVEPOINT", PatternMatch::kPrefix, "transaction", "mariadb.transaction.savepoint",
     MappingDisposition::kAdmittedSblr, "mariadb.transaction.savepoint",
     "SBLR_TRANSACTION_SAVEPOINT", "EngineSavepoint", "", "", false, true},
    {"RELEASE SAVEPOINT", PatternMatch::kPrefix, "transaction", "mariadb.transaction.release_savepoint",
     MappingDisposition::kAdmittedSblr, "mariadb.transaction.release_savepoint",
     "SBLR_TRANSACTION_RELEASE_SAVEPOINT", "EngineReleaseSavepoint", "", "", false, true},
    {"SET", PatternMatch::kPrefix, "session", "mariadb.session.set",
     MappingDisposition::kAdmittedSblr, "mariadb.session.set",
     "SBLR_DONOR_MARIADB_SET", "EngineSessionSet", "", "", false, false},
    {"CREATE", PatternMatch::kPrefix, "ddl", "mariadb.ddl.create",
     MappingDisposition::kAdmittedSblr, "mariadb.ddl.create",
     "SBLR_DONOR_MARIADB_DDL_CREATE", "EngineDdlCreate", "", "", true, true},
    {"ALTER", PatternMatch::kPrefix, "ddl", "mariadb.ddl.alter",
     MappingDisposition::kAdmittedSblr, "mariadb.ddl.alter",
     "SBLR_DONOR_MARIADB_DDL_ALTER", "EngineDdlAlter", "", "", true, true},
    {"DROP", PatternMatch::kPrefix, "ddl", "mariadb.ddl.drop",
     MappingDisposition::kAdmittedSblr, "mariadb.ddl.drop",
     "SBLR_DONOR_MARIADB_DDL_DROP", "EngineDdlDrop", "", "", true, true},
    {"TRUNCATE", PatternMatch::kPrefix, "ddl", "mariadb.ddl.truncate",
     MappingDisposition::kAdmittedSblr, "mariadb.ddl.truncate",
     "SBLR_DONOR_MARIADB_DDL_TRUNCATE", "EngineDdlTruncate", "", "", true, true},
    {"REPLACE", PatternMatch::kPrefix, "dml", "mariadb.dml.replace",
     MappingDisposition::kAdmittedSblr, "mariadb.dml.replace",
     "SBLR_DONOR_MARIADB_REPLACE", "EngineDmlReplace", "", "", false, true},
    {"INSERT", PatternMatch::kPrefix, "dml", "mariadb.dml.insert",
     MappingDisposition::kAdmittedSblr, "mariadb.dml.insert",
     "SBLR_DONOR_MARIADB_INSERT", "EngineDmlInsert", "", "", false, true},
    {"UPDATE", PatternMatch::kPrefix, "dml", "mariadb.dml.update",
     MappingDisposition::kAdmittedSblr, "mariadb.dml.update",
     "SBLR_DONOR_MARIADB_UPDATE", "EngineDmlUpdate", "", "", false, true},
    {"DELETE", PatternMatch::kPrefix, "dml", "mariadb.dml.delete",
     MappingDisposition::kAdmittedSblr, "mariadb.dml.delete",
     "SBLR_DONOR_MARIADB_DELETE", "EngineDmlDelete", "", "", false, true},
    {"SELECT", PatternMatch::kPrefix, "query", "mariadb.query.select",
     MappingDisposition::kAdmittedSblr, "mariadb.query.select",
     "SBLR_DONOR_MARIADB_SELECT", "EngineQuerySelect", "", "", false, false},
    {"WITH", PatternMatch::kPrefix, "query", "mariadb.query.with",
     MappingDisposition::kAdmittedSblr, "mariadb.query.with",
     "SBLR_DONOR_MARIADB_SELECT", "EngineQuerySelect", "", "", false, false},
    {"CALL", PatternMatch::kPrefix, "routine", "mariadb.routine.call",
     MappingDisposition::kParserSupportUdr, "mariadb.udr.routine.call",
     "SBLR_DONOR_MARIADB_ROUTINE_CALL", "ParserSupportRoutineRoute",
     "MARIADB.EMULATION.ROUTINE_ROUTE",
     "Routine calls route through trusted package policy.", true, true},
    {"ANALYZE TABLE", PatternMatch::kPrefix, "maintenance", "mariadb.maintenance.analyze_table",
     MappingDisposition::kUnsupportedRefusal, "mariadb.policy.unsupported.analyze_table",
     "", "", "MARIADB.AUTHORITY.UNSUPPORTED_DENIED",
     "MariaDB ANALYZE TABLE is a donor low-level utility surface and is outside donor parser authority.",
     true, false},
    {"OPTIMIZE TABLE", PatternMatch::kPrefix, "maintenance", "mariadb.maintenance.optimize_table",
     MappingDisposition::kUnsupportedRefusal, "mariadb.policy.unsupported.optimize_table",
     "", "", "MARIADB.AUTHORITY.UNSUPPORTED_DENIED",
     "MariaDB OPTIMIZE TABLE is a donor low-level utility surface and is outside donor parser authority.",
     true, false},
    {"CHECK TABLE", PatternMatch::kPrefix, "maintenance", "mariadb.maintenance.check_table",
     MappingDisposition::kUnsupportedRefusal, "mariadb.policy.unsupported.check_table",
     "", "", "MARIADB.AUTHORITY.UNSUPPORTED_DENIED",
     "MariaDB CHECK TABLE is a donor verification utility surface and is outside donor parser authority.",
     true, false},
    {"REPAIR TABLE", PatternMatch::kPrefix, "maintenance", "mariadb.maintenance.repair_table",
     MappingDisposition::kUnsupportedRefusal, "mariadb.policy.unsupported.repair_table",
     "", "", "MARIADB.AUTHORITY.UNSUPPORTED_DENIED",
     "MariaDB REPAIR TABLE is a donor repair utility surface and is outside donor parser authority.",
     true, false},
    {"FLUSH", PatternMatch::kPrefix, "maintenance", "mariadb.maintenance.flush",
     MappingDisposition::kUnsupportedRefusal, "mariadb.policy.unsupported.flush",
     "", "", "MARIADB.AUTHORITY.UNSUPPORTED_DENIED",
     "MariaDB FLUSH is a donor low-level utility surface and is outside donor parser authority.",
     true, false},
    {"XA", PatternMatch::kPrefix, "transaction", "mariadb.transaction.xa",
     MappingDisposition::kUnsupportedRefusal, "mariadb.policy.transaction.xa", "",
     "", "MARIADB.AUTHORITY.XA_DENIED",
     "XA distributed transaction authority is not admitted by the parser.", true, true},
};

const std::array<SurfaceDescriptor, 12> kDatatypeSurfaces{{
    {"numeric", "TINYINT;SMALLINT;MEDIUMINT;INT;BIGINT;DECIMAL;FLOAT;DOUBLE", "descriptor"},
    {"unsigned_numeric", "UNSIGNED;ZEROFILL", "descriptor_policy"},
    {"text", "CHAR;VARCHAR;TEXT;TINYTEXT;MEDIUMTEXT;LONGTEXT", "descriptor"},
    {"binary", "BINARY;VARBINARY;BLOB;TINYBLOB;MEDIUMBLOB;LONGBLOB", "descriptor"},
    {"temporal", "DATE;TIME;DATETIME;TIMESTAMP;YEAR", "descriptor"},
    {"boolean", "BOOL;BOOLEAN", "descriptor_alias"},
    {"json", "JSON", "descriptor"},
    {"enum_set", "ENUM;SET", "parser_support_udr"},
    {"spatial", "GEOMETRY;POINT;LINESTRING;POLYGON", "parser_support_udr"},
    {"inet", "INET4;INET6", "descriptor"},
    {"uuid", "UUID", "descriptor"},
    {"charset_collation", "CHARACTER SET;COLLATE", "catalog_policy"},
}};

const std::array<SurfaceDescriptor, 12> kBuiltinSurfaces{{
    {"aggregate", "COUNT;SUM;AVG;MIN;MAX;GROUP_CONCAT", "sblr"},
    {"window", "ROW_NUMBER;RANK;DENSE_RANK;LAG;LEAD", "sblr"},
    {"string", "CONCAT;SUBSTRING;LOWER;UPPER;TRIM;CHAR_LENGTH", "sblr"},
    {"numeric", "ABS;ROUND;POW;SQRT;MOD", "sblr"},
    {"temporal", "NOW;CURRENT_TIMESTAMP;DATE_ADD;DATE_SUB;TIMESTAMPDIFF", "sblr"},
    {"json", "JSON_EXTRACT;JSON_VALUE;JSON_TABLE;JSON_OBJECT;JSON_DETAILED;JSON_LOOSE", "parser_support_udr"},
    {"sequence", "NEXTVAL;PREVIOUS VALUE FOR;NEXT VALUE FOR", "sblr"},
    {"regular_expression", "REGEXP_REPLACE;REGEXP_SUBSTR;REGEXP_INSTR", "sblr"},
    {"security", "CURRENT_USER;SESSION_USER;USER", "catalog_projection"},
    {"variables", "@user_variable;@@system_variable", "session_descriptor"},
    {"fulltext", "MATCH AGAINST", "sblr_optional"},
    {"spatial", "ST_*", "parser_support_udr"},
}};

const std::array<SurfaceDescriptor, 9> kCatalogSurfaces{{
    {"information_schema", "INFORMATION_SCHEMA.", "catalog_projection"},
    {"mysql_schema", "MYSQL.USER;MYSQL.DB;MYSQL.TABLES_PRIV;MYSQL.PROCS_PRIV;MYSQL.GLOBAL_PRIV", "catalog_projection"},
    {"performance_schema", "PERFORMANCE_SCHEMA.", "catalog_projection"},
    {"sys_schema", "SYS.", "catalog_projection"},
    {"replication_status", "SHOW REPLICA STATUS;SHOW BINARY LOGS", "catalog_projection"},
    {"sequence_metadata", "INFORMATION_SCHEMA.SEQUENCES;MYSQL.SEQUENCES", "catalog_projection"},
    {"routine_metadata", "INFORMATION_SCHEMA.ROUTINES;TRIGGERS;EVENTS", "catalog_projection"},
    {"table_metadata", "SHOW COLUMNS;SHOW INDEX;DESCRIBE", "catalog_projection"},
    {"privilege_metadata", "SHOW GRANTS", "catalog_projection"},
}};

const std::array<SurfaceDescriptor, 14> kDiagnosticSurfaces{{
    {"parse", "MARIADB.PARSE.INVALID_INPUT;MARIADB.PARSE.UNSUPPORTED_SURFACE", "parser"},
    {"file", "MARIADB.AUTHORITY.FILE_IO_DENIED", "fail_closed"},
    {"plugin", "MARIADB.AUTHORITY.PLUGIN_DENIED", "fail_closed"},
    {"tablespace", "MARIADB.AUTHORITY.TABLESPACE_DENIED", "fail_closed"},
    {"etl", "MARIADB.EMULATION.ETL_ROUTE", "parser_support_udr"},
    {"replication", "MARIADB.EMULATION.REPLICATION_ROUTE", "parser_support_udr"},
    {"security", "MARIADB.EMULATION.SECURITY_ROUTE", "parser_support_udr"},
    {"routine", "MARIADB.EMULATION.ROUTINE_ROUTE", "parser_support_udr"},
    {"maintenance", "MARIADB.AUTHORITY.UNSUPPORTED_DENIED", "fail_closed"},
    {"sequence", "MARIADB.SEQUENCE.*", "sblr"},
    {"handler", "MARIADB.EMULATION.HANDLER_ROUTE", "parser_support_udr"},
    {"session_admin", "MARIADB.AUTHORITY.SESSION_ADMIN_DENIED", "fail_closed"},
    {"binlog", "MARIADB.AUTHORITY.BINLOG_DENIED", "fail_closed"},
    {"xa", "MARIADB.AUTHORITY.XA_DENIED", "fail_closed"},
}};

const scratchbird::parser::donor::DialectProfile kProfile{
    "mariadb",
    "MariaDB",
    "sbp_mariadb",
    "sbup_mariadb",
    "12.2.2",
    "MARIADB",
    kSblrFamily,
    kPatterns,
    kDatatypeSurfaces,
    kBuiltinSurfaces,
    kCatalogSurfaces,
    kDiagnosticSurfaces,
    31,
    141,
    127,
    14,
    2,
    0,
    8,
    2,
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

std::string MariadbPackageIdentityJson() {
  return scratchbird::parser::donor::PackageIdentityJson(kProfile);
}

std::string MariadbSurfaceReportJson() {
  return scratchbird::parser::donor::SurfaceReportJson(kProfile);
}

} // namespace scratchbird::parser::mariadb
