// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "xtdb_dialect.hpp"

#include <array>

namespace scratchbird::parser::xtdb {
namespace {

using scratchbird::parser::compatibility::MappingDisposition;
using scratchbird::parser::compatibility::OperationPattern;
using scratchbird::parser::compatibility::PatternMatch;
using scratchbird::parser::compatibility::SurfaceDescriptor;

constexpr std::string_view kSblrFamily = "sblr.compatibility.xtdb.profile.v1";

constexpr OperationPattern kPatterns[] = {
    {"CREATE DATABASE", PatternMatch::kPrefix, "database_lifecycle", "xtdb.lifecycle.create_database",
     MappingDisposition::kScratchBirdLifecycleApi, "xtdb.lifecycle.create_database",
     "SBLR_LIFECYCLE_CREATE_DATABASE", "EngineCreateLifecycle", "",
     "", true, false},
    {"DROP DATABASE", PatternMatch::kPrefix, "database_lifecycle", "xtdb.lifecycle.drop_database",
     MappingDisposition::kScratchBirdLifecycleApi, "xtdb.lifecycle.drop_database",
     "SBLR_LIFECYCLE_DROP_DATABASE", "EngineDropLifecycle", "",
     "", true, false},
    {"ALTER DATABASE", PatternMatch::kPrefix, "database_lifecycle", "xtdb.lifecycle.alter_database",
     MappingDisposition::kParserSupportUdr, "xtdb.udr.database.alter",
     "SBLR_COMPAT_XTDB_DATABASE_ROUTE", "ParserSupportDatabaseRoute", "XTDB.EMULATION.DATABASE_ROUTE",
     "XTDB database presentation updates route through trusted parser support and catalog policy.", true, true},
    {"XTDB_Q", PatternMatch::kPrefix, "datalog", "xtdb.datalog.query",
     MappingDisposition::kParserSupportUdr, "xtdb.udr.datalog.query",
     "SBLR_COMPAT_XTDB_DATALOG_ROUTE", "ParserSupportDatalogRoute", "XTDB.EMULATION.DATALOG_ROUTE",
     "XTDB Datalog requests route through trusted parser support for entity/document result-shape rendering.", true, false},
    {"XTDB_SUBMIT_TX", PatternMatch::kPrefix, "entity_transaction", "xtdb.entity.submit_tx",
     MappingDisposition::kParserSupportUdr, "xtdb.udr.entity.submit_tx",
     "SBLR_COMPAT_XTDB_ENTITY_TX_ROUTE", "ParserSupportEntityTransactionRoute", "XTDB.EMULATION.ENTITY_TX_ROUTE",
     "XTDB entity transaction requests route through trusted parser support; MGA finality remains engine-owned.", true, true},
    {"XTDB_PUT", PatternMatch::kPrefix, "entity_transaction", "xtdb.entity.put",
     MappingDisposition::kParserSupportUdr, "xtdb.udr.entity.put",
     "SBLR_COMPAT_XTDB_ENTITY_TX_ROUTE", "ParserSupportEntityTransactionRoute", "XTDB.EMULATION.ENTITY_TX_ROUTE",
     "XTDB entity put requests route through trusted parser support and engine descriptor policy.", true, true},
    {"XTDB_DELETE", PatternMatch::kPrefix, "entity_transaction", "xtdb.entity.delete",
     MappingDisposition::kParserSupportUdr, "xtdb.udr.entity.delete",
     "SBLR_COMPAT_XTDB_ENTITY_TX_ROUTE", "ParserSupportEntityTransactionRoute", "XTDB.EMULATION.ENTITY_TX_ROUTE",
     "XTDB entity delete requests route through trusted parser support and engine MGA.", true, true},
    {"XTDB_EVICT", PatternMatch::kPrefix, "entity_transaction", "xtdb.entity.evict",
     MappingDisposition::kParserSupportUdr, "xtdb.udr.entity.evict",
     "SBLR_COMPAT_XTDB_ENTITY_TX_ROUTE", "ParserSupportEntityTransactionRoute", "XTDB.EMULATION.ENTITY_TX_ROUTE",
     "XTDB entity evict requests route through trusted parser support and catalog/security policy.", true, true},
    {"VALID_TIME", PatternMatch::kContains, "bitemporal", "xtdb.time.valid_time",
     MappingDisposition::kParserSupportUdr, "xtdb.udr.time.valid_time",
     "SBLR_COMPAT_XTDB_BITEMPORAL_ROUTE", "ParserSupportBitemporalRoute", "XTDB.EMULATION.BITEMPORAL_ROUTE",
     "XTDB valid-time predicates route through trusted parser support for compatibility-compatible temporal semantics.", true, false},
    {"SYSTEM_TIME", PatternMatch::kContains, "bitemporal", "xtdb.time.system_time",
     MappingDisposition::kParserSupportUdr, "xtdb.udr.time.system_time",
     "SBLR_COMPAT_XTDB_BITEMPORAL_ROUTE", "ParserSupportBitemporalRoute", "XTDB.EMULATION.BITEMPORAL_ROUTE",
     "XTDB system-time predicates route through trusted parser support for compatibility-compatible temporal semantics.", true, false},
    {"FOR VALID_TIME", PatternMatch::kContains, "bitemporal", "xtdb.time.valid_time",
     MappingDisposition::kParserSupportUdr, "xtdb.udr.time.valid_time",
     "SBLR_COMPAT_XTDB_BITEMPORAL_ROUTE", "ParserSupportBitemporalRoute", "XTDB.EMULATION.BITEMPORAL_ROUTE",
     "XTDB SQL valid-time query options route through trusted parser support.", true, false},
    {"FOR SYSTEM_TIME", PatternMatch::kContains, "bitemporal", "xtdb.time.system_time",
     MappingDisposition::kParserSupportUdr, "xtdb.udr.time.system_time",
     "SBLR_COMPAT_XTDB_BITEMPORAL_ROUTE", "ParserSupportBitemporalRoute", "XTDB.EMULATION.BITEMPORAL_ROUTE",
     "XTDB SQL system-time query options route through trusted parser support.", true, false},
    {"XTDB_STATUS", PatternMatch::kPrefix, "catalog_overlay", "xtdb.catalog.status",
     MappingDisposition::kCatalogProjection, "xtdb.catalog.status",
     "SBLR_COMPAT_XTDB_CATALOG_PROJECT", "EngineCatalogProjection", "",
     "", false, false},
    {"XTDB_TX_LOG", PatternMatch::kPrefix, "catalog_overlay", "xtdb.catalog.tx_log",
     MappingDisposition::kCatalogProjection, "xtdb.catalog.tx_log",
     "SBLR_COMPAT_XTDB_CATALOG_PROJECT", "EngineCatalogProjection", "",
     "", false, false},
    {"XTDB_MODULES", PatternMatch::kPrefix, "catalog_overlay", "xtdb.catalog.modules",
     MappingDisposition::kCatalogProjection, "xtdb.catalog.modules",
     "SBLR_COMPAT_XTDB_CATALOG_PROJECT", "EngineCatalogProjection", "",
     "", false, false},
    {"SELECT", PatternMatch::kPrefix, "query", "xtdb.sql.select",
     MappingDisposition::kAdmittedSblr, "xtdb.sql.select",
     "SBLR_COMPAT_XTDB_SELECT", "EngineQuerySelect", "",
     "", false, false},
    {"WITH", PatternMatch::kPrefix, "query", "xtdb.sql.with",
     MappingDisposition::kAdmittedSblr, "xtdb.sql.with",
     "SBLR_COMPAT_XTDB_SELECT", "EngineQuerySelect", "",
     "", false, false},
    {"INSERT", PatternMatch::kPrefix, "dml", "xtdb.sql.insert",
     MappingDisposition::kParserSupportUdr, "xtdb.udr.sql.insert",
     "SBLR_COMPAT_XTDB_ENTITY_TX_ROUTE", "ParserSupportEntityTransactionRoute", "XTDB.EMULATION.ENTITY_TX_ROUTE",
     "XTDB SQL insert maps to entity transaction routing under engine MGA.", true, true},
    {"UPDATE", PatternMatch::kPrefix, "dml", "xtdb.sql.update",
     MappingDisposition::kParserSupportUdr, "xtdb.udr.sql.update",
     "SBLR_COMPAT_XTDB_ENTITY_TX_ROUTE", "ParserSupportEntityTransactionRoute", "XTDB.EMULATION.ENTITY_TX_ROUTE",
     "XTDB SQL update maps to entity transaction routing under engine MGA.", true, true},
    {"DELETE", PatternMatch::kPrefix, "dml", "xtdb.sql.delete",
     MappingDisposition::kParserSupportUdr, "xtdb.udr.sql.delete",
     "SBLR_COMPAT_XTDB_ENTITY_TX_ROUTE", "ParserSupportEntityTransactionRoute", "XTDB.EMULATION.ENTITY_TX_ROUTE",
     "XTDB SQL delete maps to entity transaction routing under engine MGA.", true, true},
    {"CREATE TABLE", PatternMatch::kPrefix, "ddl", "xtdb.sql.create_table",
     MappingDisposition::kParserSupportUdr, "xtdb.udr.sql.create_table",
     "SBLR_COMPAT_XTDB_SCHEMA_ROUTE", "ParserSupportSchemaRoute", "XTDB.EMULATION.SCHEMA_ROUTE",
     "XTDB table/schema presentation routes through trusted parser support.", true, true},
    {"ALTER TABLE", PatternMatch::kPrefix, "ddl", "xtdb.sql.alter_table",
     MappingDisposition::kParserSupportUdr, "xtdb.udr.sql.alter_table",
     "SBLR_COMPAT_XTDB_SCHEMA_ROUTE", "ParserSupportSchemaRoute", "XTDB.EMULATION.SCHEMA_ROUTE",
     "XTDB table/schema presentation routes through trusted parser support.", true, true},
    {"DROP TABLE", PatternMatch::kPrefix, "ddl", "xtdb.sql.drop_table",
     MappingDisposition::kParserSupportUdr, "xtdb.udr.sql.drop_table",
     "SBLR_COMPAT_XTDB_SCHEMA_ROUTE", "ParserSupportSchemaRoute", "XTDB.EMULATION.SCHEMA_ROUTE",
     "XTDB table/schema presentation routes through trusted parser support.", true, true},
    {"MODULES CONFIGURATION", PatternMatch::kPrefix, "external_authority", "xtdb.modules.configuration",
     MappingDisposition::kPolicyRefusal, "xtdb.policy.modules.configuration",
     "", "", "XTDB.AUTHORITY.MODULE_CONFIGURATION_DENIED",
     "XTDB runtime module configuration controls external services and is blocked from parser authority.", true, false},
    {"INDEX_STORE", PatternMatch::kPrefix, "external_authority", "xtdb.modules.index_store",
     MappingDisposition::kPolicyRefusal, "xtdb.policy.modules.index_store",
     "", "", "XTDB.AUTHORITY.MODULE_CONFIGURATION_DENIED",
     "XTDB index-store module control is external authority and is blocked from parser authority.", true, false},
    {"DOCUMENT_STORE", PatternMatch::kPrefix, "external_authority", "xtdb.modules.document_store",
     MappingDisposition::kPolicyRefusal, "xtdb.policy.modules.document_store",
     "", "", "XTDB.AUTHORITY.MODULE_CONFIGURATION_DENIED",
     "XTDB document-store module control is external authority and is blocked from parser authority.", true, false},
    {"CLUSTER", PatternMatch::kPrefix, "cluster", "xtdb.cluster.control",
     MappingDisposition::kAdmittedSblr, "cluster.job.start_controlled",
     "sblr.cluster.control.v3:cluster.job.start_controlled", "cluster.start_job", "",
     "", true, false},
    {"NODE", PatternMatch::kPrefix, "cluster", "xtdb.node.control",
     MappingDisposition::kAdmittedSblr, "cluster.node.admit_member",
     "sblr.cluster.control.v3:cluster.node.admit_member", "cluster.admit_member", "",
     "", true, false},
    {"BEGIN", PatternMatch::kPrefix, "transaction", "xtdb.transaction.begin",
     MappingDisposition::kAdmittedSblr, "xtdb.transaction.begin",
     "SBLR_TRANSACTION_BEGIN", "EngineBeginTransaction", "",
     "", false, false},
    {"COMMIT", PatternMatch::kPrefix, "transaction", "xtdb.transaction.commit",
     MappingDisposition::kAdmittedSblr, "xtdb.transaction.commit",
     "SBLR_TRANSACTION_COMMIT", "EngineCommitTransaction", "",
     "", false, true},
    {"ROLLBACK", PatternMatch::kPrefix, "transaction", "xtdb.transaction.rollback",
     MappingDisposition::kAdmittedSblr, "xtdb.transaction.rollback",
     "SBLR_TRANSACTION_ROLLBACK", "EngineRollbackTransaction", "",
     "", false, true},
};

const std::array<SurfaceDescriptor, 9> kDatatypeSurfaces{{
    {"scalar", "BOOLEAN;BIGINT;DOUBLE;VARCHAR;VARBINARY;UUID", "descriptor"},
    {"temporal", "TIMESTAMP;DATE;TIME;INTERVAL;VALID_TIME;SYSTEM_TIME", "parser_support_udr"},
    {"document", "DOCUMENT;JSON;EDN;TRANSIT", "parser_support_udr"},
    {"entity", "ENTITY_ID;EID;ENTITY_DOCUMENT", "parser_support_udr"},
    {"datalog", "DATALOG_QUERY;LOGIC_VAR;PULL_PATTERN", "parser_support_udr"},
    {"bitemporal", "VALID_TIME;SYSTEM_TIME;TX_TIME", "parser_support_udr"},
    {"catalog", "XT;PG_CATALOG;INFORMATION_SCHEMA", "catalog_projection"},
    {"module", "INDEX_STORE;DOCUMENT_STORE;TX_LOG", "catalog_projection"},
    {"external", "MODULES_CONFIGURATION;CLUSTER_NODE", "fail_closed"},
}};

const std::array<SurfaceDescriptor, 9> kBuiltinSurfaces{{
    {"query", "XTDB_Q;SELECT;WITH", "sblr"},
    {"entity_tx", "XTDB_SUBMIT_TX;XTDB_PUT;XTDB_DELETE;XTDB_EVICT", "parser_support_udr"},
    {"bitemporal", "VALID_TIME;SYSTEM_TIME;FOR VALID_TIME;FOR SYSTEM_TIME", "parser_support_udr"},
    {"catalog", "XTDB_STATUS;XTDB_TX_LOG;XTDB_MODULES", "catalog_projection"},
    {"sql_dml", "INSERT;UPDATE;DELETE", "parser_support_udr"},
    {"sql_ddl", "CREATE TABLE;ALTER TABLE;DROP TABLE", "parser_support_udr"},
    {"modules", "MODULES CONFIGURATION;INDEX_STORE;DOCUMENT_STORE", "fail_closed"},
    {"cluster", "CLUSTER;NODE", "fail_closed"},
    {"transaction", "BEGIN;COMMIT;ROLLBACK", "sblr"},
}};

const std::array<SurfaceDescriptor, 10> kCatalogSurfaces{{
    {"xt_schema", "XT", "catalog_projection"},
    {"pg_catalog", "PG_CATALOG", "catalog_projection"},
    {"information_schema", "INFORMATION_SCHEMA", "catalog_projection"},
    {"public_schema", "PUBLIC", "catalog_projection"},
    {"tables", "INFORMATION_SCHEMA.TABLES;PG_CATALOG.PG_TABLES", "catalog_projection"},
    {"columns", "INFORMATION_SCHEMA.COLUMNS;PG_CATALOG.PG_ATTRIBUTE", "catalog_projection"},
    {"types", "PG_CATALOG.PG_TYPE;PG_CATALOG.PG_RANGE", "catalog_projection"},
    {"runtime", "XT.TRIE_STATS;XT.LIVE_TABLES", "catalog_projection"},
    {"modules", "INDEX_STORE;DOCUMENT_STORE;TX_LOG", "catalog_projection"},
    {"security", "PG_CATALOG.PG_USER", "catalog_projection"},
}};

const std::array<SurfaceDescriptor, 10> kDiagnosticSurfaces{{
    {"parse", "XTDB.PARSE.INVALID_INPUT;XTDB.PARSE.UNSUPPORTED_SURFACE", "parser"},
    {"policy", "XTDB.AUTHORITY.*", "fail_closed"},
    {"udr", "XTDB.EMULATION.*", "parser_support_udr"},
    {"catalog", "XTDB.CATALOG_OVERLAY.READ_ONLY", "catalog_projection"},
    {"session", "XTDB.SESSION.*", "sblr"},
    {"transaction", "XTDB.TRANSACTION.*", "sblr"},
    {"file_effects", "real_reference_file_effects=false", "authority_invariant"},
    {"reference_execution", "reference_engine_sql_executed=false", "authority_invariant"},
    {"mga", "parser_transaction_finality_authority=false", "authority_invariant"},
    {"support_bundle", "source_text_redacted=true", "diagnostic_redaction"},
}};

const scratchbird::parser::compatibility::DialectProfile kProfile{
    "xtdb",
    "XTDB",
    "sbp_xtdb",
    "sbup_xtdb",
    "2.1.0",
    "XTDB",
    kSblrFamily,
    kPatterns,
    kDatatypeSurfaces,
    kBuiltinSurfaces,
    kCatalogSurfaces,
    kDiagnosticSurfaces,
    19,
    9,
    5,
    0,
    4,
    0,
    0,
    0,
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
std::string XtdbPackageIdentityJson() { return scratchbird::parser::compatibility::PackageIdentityJson(kProfile); }
std::string XtdbSurfaceReportJson() { return scratchbird::parser::compatibility::SurfaceReportJson(kProfile); }

} // namespace scratchbird::parser::xtdb
