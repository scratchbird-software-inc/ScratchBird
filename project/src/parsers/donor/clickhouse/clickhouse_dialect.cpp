// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "clickhouse_dialect.hpp"

#include <array>

namespace scratchbird::parser::clickhouse {
namespace {

using scratchbird::parser::donor::MappingDisposition;
using scratchbird::parser::donor::OperationPattern;
using scratchbird::parser::donor::PatternMatch;
using scratchbird::parser::donor::SurfaceDescriptor;

constexpr std::string_view kSblrFamily = "sblr.donor.clickhouse.profile.v1";

constexpr OperationPattern kPatterns[] = {
    {"SYSTEM", PatternMatch::kPrefix, "system_admin", "clickhouse.system.command",
     MappingDisposition::kUnsupportedRefusal, "clickhouse.policy.unsupported.system",
     "", "", "CLICKHOUSE.AUTHORITY.UNSUPPORTED_DENIED",
     "ClickHouse SYSTEM commands are donor low-level utility surfaces and are outside donor parser authority.",
     true, false},
    {"KILL", PatternMatch::kPrefix, "system_admin", "clickhouse.system.kill",
     MappingDisposition::kUnsupportedRefusal, "clickhouse.policy.unsupported.kill",
     "", "", "CLICKHOUSE.AUTHORITY.UNSUPPORTED_DENIED",
     "ClickHouse KILL commands are donor low-level utility surfaces and are outside donor parser authority.",
     true, false},
    {"BACKUP", PatternMatch::kPrefix, "bulk_io", "clickhouse.bulk_io.backup",
     MappingDisposition::kPolicyRefusal, "clickhouse.policy.backup", "",
     "", "CLICKHOUSE.AUTHORITY.BACKUP_DENIED",
     "ClickHouse BACKUP file/object-store effects require trusted engine lifecycle admission.", true, false},
    {"RESTORE", PatternMatch::kPrefix, "bulk_io", "clickhouse.bulk_io.restore",
     MappingDisposition::kPolicyRefusal, "clickhouse.policy.restore", "",
     "", "CLICKHOUSE.AUTHORITY.BACKUP_DENIED",
     "ClickHouse RESTORE file/object-store effects require trusted engine lifecycle admission.", true, false},
    {"ATTACH", PatternMatch::kPrefix, "storage_admin", "clickhouse.storage.attach",
     MappingDisposition::kPolicyRefusal, "clickhouse.policy.attach", "",
     "", "CLICKHOUSE.AUTHORITY.STORAGE_ADMIN_DENIED",
     "ClickHouse ATTACH would bind external storage outside parser authority.", true, false},
    {"DETACH", PatternMatch::kPrefix, "storage_admin", "clickhouse.storage.detach",
     MappingDisposition::kPolicyRefusal, "clickhouse.policy.detach", "",
     "", "CLICKHOUSE.AUTHORITY.STORAGE_ADMIN_DENIED",
     "ClickHouse DETACH storage lifecycle is not parser authority.", true, false},
    {"INSERT INTO FUNCTION", PatternMatch::kPrefix, "external_io", "clickhouse.external_io.insert_function",
     MappingDisposition::kParserSupportUdr, "clickhouse.udr.etl.insert_function",
     "SBLR_DONOR_CLICKHOUSE_ETL_ROUTE", "ParserSupportEtlRoute",
     "CLICKHOUSE.EMULATION.ETL_ROUTE",
     "ClickHouse INSERT INTO FUNCTION routes through the ClickHouse donor UDR.", true, true},
    {"FILE", PatternMatch::kContainsFunctionCall, "external_io", "clickhouse.external_io.file_function",
     MappingDisposition::kPolicyRefusal, "clickhouse.policy.external.file", "",
     "", "CLICKHOUSE.AUTHORITY.EXTERNAL_IO_DENIED",
     "ClickHouse file table functions require trusted connector admission.", true, false},
    {"URL", PatternMatch::kContainsFunctionCall, "external_io", "clickhouse.external_io.url_function",
     MappingDisposition::kParserSupportUdr, "clickhouse.udr.etl.url",
     "SBLR_DONOR_CLICKHOUSE_ETL_ROUTE", "ParserSupportEtlRoute",
     "CLICKHOUSE.EMULATION.ETL_ROUTE",
     "ClickHouse URL table functions route through the ClickHouse donor UDR.", true, false},
    {"S3", PatternMatch::kContainsFunctionCall, "external_io", "clickhouse.external_io.s3_function",
     MappingDisposition::kParserSupportUdr, "clickhouse.udr.etl.s3",
     "SBLR_DONOR_CLICKHOUSE_ETL_ROUTE", "ParserSupportEtlRoute",
     "CLICKHOUSE.EMULATION.ETL_ROUTE",
     "ClickHouse S3 table functions route through the ClickHouse donor UDR.", true, false},
    {"HDFS", PatternMatch::kContainsFunctionCall, "external_io", "clickhouse.external_io.hdfs_function",
     MappingDisposition::kParserSupportUdr, "clickhouse.udr.etl.hdfs",
     "SBLR_DONOR_CLICKHOUSE_ETL_ROUTE", "ParserSupportEtlRoute",
     "CLICKHOUSE.EMULATION.ETL_ROUTE",
     "ClickHouse HDFS table functions route through the ClickHouse donor UDR.", true, false},
    {"KAFKA", PatternMatch::kCreateTableEngineClause, "etl", "clickhouse.etl.kafka",
     MappingDisposition::kParserSupportUdr, "clickhouse.udr.etl.kafka",
     "SBLR_DONOR_CLICKHOUSE_ETL_ROUTE", "ParserSupportEtlRoute",
     "CLICKHOUSE.EMULATION.ETL_ROUTE",
     "ClickHouse Kafka ETL routes through the ClickHouse donor UDR.", true, true},
    {"REMOTE", PatternMatch::kContainsFunctionCall, "distributed", "clickhouse.distributed.remote_function",
     MappingDisposition::kPolicyRefusal, "clickhouse.policy.distributed.remote", "",
     "", "CLICKHOUSE.AUTHORITY.DISTRIBUTED_DENIED",
     "ClickHouse remote execution requires cluster/distributed policy admission.", true, false},
    {" ON CLUSTER ", PatternMatch::kContains, "distributed", "clickhouse.distributed.cluster_clause",
     MappingDisposition::kAdmittedSblr, "cluster.query.plan_distributed",
     "required_new:sblr.cluster.query.v1:cluster.query.plan_distributed",
     "cluster.query.plan_distributed", "", "", true, false},
    {"CREATE USER", PatternMatch::kPrefix, "security", "clickhouse.security.create_user",
     MappingDisposition::kParserSupportUdr, "clickhouse.udr.security.create_user",
     "SBLR_DONOR_CLICKHOUSE_SECURITY_ROUTE", "ParserSupportSecurityRoute",
     "CLICKHOUSE.EMULATION.SECURITY_ROUTE",
     "ClickHouse account management routes through trusted security policy.", true, false},
    {"ALTER USER", PatternMatch::kPrefix, "security", "clickhouse.security.alter_user",
     MappingDisposition::kParserSupportUdr, "clickhouse.udr.security.alter_user",
     "SBLR_DONOR_CLICKHOUSE_SECURITY_ROUTE", "ParserSupportSecurityRoute",
     "CLICKHOUSE.EMULATION.SECURITY_ROUTE",
     "ClickHouse account changes route through trusted security policy.", true, false},
    {"DROP USER", PatternMatch::kPrefix, "security", "clickhouse.security.drop_user",
     MappingDisposition::kParserSupportUdr, "clickhouse.udr.security.drop_user",
     "SBLR_DONOR_CLICKHOUSE_SECURITY_ROUTE", "ParserSupportSecurityRoute",
     "CLICKHOUSE.EMULATION.SECURITY_ROUTE",
     "ClickHouse account removal routes through trusted security policy.", true, false},
    {"CREATE ROLE", PatternMatch::kPrefix, "security", "clickhouse.security.create_role",
     MappingDisposition::kParserSupportUdr, "clickhouse.udr.security.create_role",
     "SBLR_DONOR_CLICKHOUSE_SECURITY_ROUTE", "ParserSupportSecurityRoute",
     "CLICKHOUSE.EMULATION.SECURITY_ROUTE",
     "ClickHouse role management routes through trusted security policy.", true, false},
    {"CREATE ROW POLICY", PatternMatch::kPrefix, "security", "clickhouse.security.create_row_policy",
     MappingDisposition::kParserSupportUdr, "clickhouse.udr.security.create_row_policy",
     "SBLR_DONOR_CLICKHOUSE_SECURITY_ROUTE", "ParserSupportSecurityRoute",
     "CLICKHOUSE.EMULATION.SECURITY_ROUTE",
     "ClickHouse row policies route through trusted security policy.", true, false},
    {"CREATE QUOTA", PatternMatch::kPrefix, "security", "clickhouse.security.create_quota",
     MappingDisposition::kParserSupportUdr, "clickhouse.udr.security.create_quota",
     "SBLR_DONOR_CLICKHOUSE_SECURITY_ROUTE", "ParserSupportSecurityRoute",
     "CLICKHOUSE.EMULATION.SECURITY_ROUTE",
     "ClickHouse quota management routes through trusted security policy.", true, false},
    {"CREATE SETTINGS PROFILE", PatternMatch::kPrefix, "security", "clickhouse.security.create_settings_profile",
     MappingDisposition::kParserSupportUdr, "clickhouse.udr.security.create_settings_profile",
     "SBLR_DONOR_CLICKHOUSE_SECURITY_ROUTE", "ParserSupportSecurityRoute",
     "CLICKHOUSE.EMULATION.SECURITY_ROUTE",
     "ClickHouse settings profile management routes through trusted security policy.", true, false},
    {"GRANT", PatternMatch::kPrefix, "security", "clickhouse.security.grant",
     MappingDisposition::kParserSupportUdr, "clickhouse.udr.security.grant",
     "SBLR_DONOR_CLICKHOUSE_SECURITY_ROUTE", "ParserSupportSecurityRoute",
     "CLICKHOUSE.EMULATION.SECURITY_ROUTE",
     "ClickHouse GRANT routes through trusted security policy.", true, false},
    {"REVOKE", PatternMatch::kPrefix, "security", "clickhouse.security.revoke",
     MappingDisposition::kParserSupportUdr, "clickhouse.udr.security.revoke",
     "SBLR_DONOR_CLICKHOUSE_SECURITY_ROUTE", "ParserSupportSecurityRoute",
     "CLICKHOUSE.EMULATION.SECURITY_ROUTE",
     "ClickHouse REVOKE routes through trusted security policy.", true, false},
    {"CREATE DICTIONARY", PatternMatch::kPrefix, "connector", "clickhouse.connector.dictionary.create",
     MappingDisposition::kPolicyRefusal, "clickhouse.policy.connector.dictionary.create",
     "", "",
     "CLICKHOUSE.EMULATION.CONNECTOR_ROUTE",
     "Dictionaries route through trusted connector/catalog policy.", true, false},
    {"DROP DICTIONARY", PatternMatch::kPrefix, "connector", "clickhouse.connector.dictionary.drop",
     MappingDisposition::kPolicyRefusal, "clickhouse.policy.connector.dictionary.drop",
     "", "",
     "CLICKHOUSE.EMULATION.CONNECTOR_ROUTE",
     "Dictionary removal routes through trusted connector/catalog policy.", true, true},
    {"CREATE FUNCTION", PatternMatch::kPrefix, "routine", "clickhouse.routine.create_function",
     MappingDisposition::kParserSupportUdr, "clickhouse.udr.routine.create_function",
     "SBLR_DONOR_CLICKHOUSE_ROUTINE_ROUTE", "ParserSupportRoutineRoute",
     "CLICKHOUSE.EMULATION.ROUTINE_ROUTE",
     "ClickHouse user-defined functions route through trusted routine policy.", true, true},
    {"OPTIMIZE", PatternMatch::kPrefix, "maintenance", "clickhouse.maintenance.optimize",
     MappingDisposition::kUnsupportedRefusal, "clickhouse.policy.unsupported.optimize",
     "", "", "CLICKHOUSE.AUTHORITY.UNSUPPORTED_DENIED",
     "ClickHouse OPTIMIZE is a donor low-level utility surface and is outside donor parser authority.",
     true, false},
    {"CHECK TABLE", PatternMatch::kPrefix, "maintenance", "clickhouse.maintenance.check_table",
     MappingDisposition::kUnsupportedRefusal, "clickhouse.policy.unsupported.check_table",
     "", "", "CLICKHOUSE.AUTHORITY.UNSUPPORTED_DENIED",
     "ClickHouse CHECK TABLE is a donor verification utility surface and is outside donor parser authority.",
     true, false},
    {"EXCHANGE", PatternMatch::kPrefix, "ddl", "clickhouse.ddl.exchange",
     MappingDisposition::kAdmittedSblr, "clickhouse.ddl.exchange",
     "SBLR_DONOR_CLICKHOUSE_DDL_EXCHANGE", "EngineDdlExchange", "", "", true, true},
    {"RENAME", PatternMatch::kPrefix, "ddl", "clickhouse.ddl.rename",
     MappingDisposition::kAdmittedSblr, "clickhouse.ddl.rename",
     "SBLR_DONOR_CLICKHOUSE_DDL_RENAME", "EngineDdlRename", "", "", true, true},
    {"TRUNCATE", PatternMatch::kPrefix, "ddl", "clickhouse.ddl.truncate",
     MappingDisposition::kAdmittedSblr, "clickhouse.ddl.truncate",
     "SBLR_DONOR_CLICKHOUSE_DDL_TRUNCATE", "EngineDdlTruncate", "", "", true, true},
    {"EXPLAIN", PatternMatch::kPrefix, "optimizer", "clickhouse.optimizer.explain",
     MappingDisposition::kCatalogProjection, "clickhouse.optimizer.explain",
     "SBLR_DONOR_CLICKHOUSE_EXPLAIN", "EngineExplainPlan", "", "", false, false},
    {"DESCRIBE", PatternMatch::kPrefix, "catalog_overlay", "clickhouse.catalog_overlay.describe",
     MappingDisposition::kCatalogProjection, "clickhouse.catalog.describe",
     "SBLR_DONOR_CLICKHOUSE_CATALOG_PROJECT", "EngineCatalogProjection", "", "", false, false},
    {"DESC", PatternMatch::kPrefix, "catalog_overlay", "clickhouse.catalog_overlay.desc",
     MappingDisposition::kCatalogProjection, "clickhouse.catalog.describe",
     "SBLR_DONOR_CLICKHOUSE_CATALOG_PROJECT", "EngineCatalogProjection", "", "", false, false},
    {"SHOW", PatternMatch::kPrefix, "catalog_overlay", "clickhouse.catalog_overlay.show",
     MappingDisposition::kCatalogProjection, "clickhouse.catalog.show",
     "SBLR_DONOR_CLICKHOUSE_CATALOG_PROJECT", "EngineCatalogProjection", "", "", false, false},
    {"USE", PatternMatch::kPrefix, "session", "clickhouse.session.use_database",
     MappingDisposition::kAdmittedSblr, "clickhouse.session.use_database",
     "SBLR_DONOR_CLICKHOUSE_USE_DATABASE", "EngineSessionRoute", "", "", false, false},
    {"SET", PatternMatch::kPrefix, "session", "clickhouse.session.set",
     MappingDisposition::kAdmittedSblr, "clickhouse.session.set",
     "SBLR_DONOR_CLICKHOUSE_SET", "EngineSessionSet", "", "", false, false},
    {"BEGIN TRANSACTION", PatternMatch::kPrefix, "transaction", "clickhouse.transaction.begin",
     MappingDisposition::kAdmittedSblr, "clickhouse.transaction.begin",
     "SBLR_TRANSACTION_BEGIN", "EngineBeginTransaction", "", "", false, false},
    {"COMMIT", PatternMatch::kPrefix, "transaction", "clickhouse.transaction.commit",
     MappingDisposition::kAdmittedSblr, "clickhouse.transaction.commit",
     "SBLR_TRANSACTION_COMMIT", "EngineCommitTransaction", "", "", false, true},
    {"ROLLBACK", PatternMatch::kPrefix, "transaction", "clickhouse.transaction.rollback",
     MappingDisposition::kAdmittedSblr, "clickhouse.transaction.rollback",
     "SBLR_TRANSACTION_ROLLBACK", "EngineRollbackTransaction", "", "", false, true},
    {"CREATE", PatternMatch::kPrefix, "ddl", "clickhouse.ddl.create",
     MappingDisposition::kAdmittedSblr, "clickhouse.ddl.create",
     "SBLR_DONOR_CLICKHOUSE_DDL_CREATE", "EngineDdlCreate", "", "", true, true},
    {"ALTER", PatternMatch::kPrefix, "ddl", "clickhouse.ddl.alter",
     MappingDisposition::kAdmittedSblr, "clickhouse.ddl.alter",
     "SBLR_DONOR_CLICKHOUSE_DDL_ALTER", "EngineDdlAlter", "", "", true, true},
    {"DROP", PatternMatch::kPrefix, "ddl", "clickhouse.ddl.drop",
     MappingDisposition::kAdmittedSblr, "clickhouse.ddl.drop",
     "SBLR_DONOR_CLICKHOUSE_DDL_DROP", "EngineDdlDrop", "", "", true, true},
    {"INSERT", PatternMatch::kPrefix, "dml", "clickhouse.dml.insert",
     MappingDisposition::kAdmittedSblr, "clickhouse.dml.insert",
     "SBLR_DONOR_CLICKHOUSE_INSERT", "EngineDmlInsert", "", "", false, true},
    {"UPDATE", PatternMatch::kPrefix, "dml", "clickhouse.dml.update",
     MappingDisposition::kAdmittedSblr, "clickhouse.dml.update",
     "SBLR_DONOR_CLICKHOUSE_UPDATE", "EngineDmlUpdate", "", "", false, true},
    {"DELETE", PatternMatch::kPrefix, "dml", "clickhouse.dml.delete",
     MappingDisposition::kAdmittedSblr, "clickhouse.dml.delete",
     "SBLR_DONOR_CLICKHOUSE_DELETE", "EngineDmlDelete", "", "", false, true},
    {"SELECT", PatternMatch::kPrefix, "query", "clickhouse.query.select",
     MappingDisposition::kAdmittedSblr, "clickhouse.query.select",
     "SBLR_DONOR_CLICKHOUSE_SELECT", "EngineQuerySelect", "", "", false, false},
    {"WITH", PatternMatch::kPrefix, "query", "clickhouse.query.with",
     MappingDisposition::kAdmittedSblr, "clickhouse.query.with",
     "SBLR_DONOR_CLICKHOUSE_SELECT", "EngineQuerySelect", "", "", false, false},
};

const std::array<SurfaceDescriptor, 12> kDatatypeSurfaces{{
    {"integer", "Int8;Int16;Int32;Int64;Int128;Int256;UInt8;UInt16;UInt32;UInt64;UInt128;UInt256", "descriptor"},
    {"float_decimal", "Float32;Float64;Decimal;Decimal32;Decimal64;Decimal128;Decimal256", "descriptor"},
    {"string", "String;FixedString", "descriptor"},
    {"temporal", "Date;Date32;DateTime;DateTime64;Time;Time64", "descriptor"},
    {"boolean", "Bool", "descriptor"},
    {"nullable", "Nullable", "descriptor"},
    {"low_cardinality", "LowCardinality", "descriptor_policy"},
    {"arrays", "Array;Map;Tuple;Nested", "parser_support_udr"},
    {"variant", "Variant;Dynamic;JSON", "parser_support_udr"},
    {"uuid_ip", "UUID;IPv4;IPv6", "descriptor"},
    {"aggregate_state", "AggregateFunction;SimpleAggregateFunction", "parser_support_udr"},
    {"geo", "Point;Ring;Polygon;MultiPolygon", "parser_support_udr"},
}};

const std::array<SurfaceDescriptor, 13> kBuiltinSurfaces{{
    {"aggregate", "COUNT;SUM;AVG;MIN;MAX;ANY;ARGMIN;ARGMAX;QUANTILE", "sblr"},
    {"array", "ARRAYJOIN;ARRAYMAP;ARRAYFILTER;ARRAYREDUCE;ARRAYZIP", "parser_support_udr"},
    {"window", "ROW_NUMBER;RANK;DENSE_RANK;LAG;LEAD", "sblr"},
    {"string", "CONCAT;SUBSTRING;LOWER;UPPER;TRIM;POSITION;MATCH", "sblr"},
    {"numeric", "ABS;ROUND;POW;SQRT;INTDIV;GCD;LCM", "sblr"},
    {"temporal", "NOW;TODATE;TODATETIME;DATE_TRUNC;TOSTARTOFINTERVAL", "sblr"},
    {"json", "JSONEXTRACT;JSON_VALUE;JSON_QUERY;VISITPARAMEXTRACT", "parser_support_udr"},
    {"hashing", "CITYHASH64;SIPHASH64;MURMURHASH;SHA256", "sblr"},
    {"url_ip", "DOMAIN;CUTTOFIRSTSIGNIFICANTSUBDOMAIN;TOIPV4;TOIPV6", "sblr"},
    {"external", "FILE;URL;S3;HDFS;REMOTE", "fail_closed"},
    {"dictionary", "DICTGET;DICTHAS;DICTGETORDEFAULT", "parser_support_udr"},
    {"cluster", "CLUSTER;ON CLUSTER", "fail_closed"},
    {"system", "SYSTEM;KILL;BACKUP;RESTORE", "fail_closed"},
}};

const std::array<SurfaceDescriptor, 10> kCatalogSurfaces{{
    {"system_tables", "SYSTEM.TABLES;SYSTEM.COLUMNS;SYSTEM.DATABASES;SYSTEM.PARTS", "catalog_projection"},
    {"system_query", "SYSTEM.QUERY_LOG;SYSTEM.PROCESSES;SYSTEM.MUTATIONS", "catalog_projection"},
    {"information_schema", "INFORMATION_SCHEMA.", "catalog_projection"},
    {"settings", "SYSTEM.SETTINGS;SYSTEM.MERGES;SYSTEM.REPLICAS", "catalog_projection"},
    {"dictionaries", "SYSTEM.DICTIONARIES;CREATE DICTIONARY", "parser_support_udr"},
    {"security", "SYSTEM.USERS;SYSTEM.ROLES;SYSTEM.QUOTAS;SYSTEM.ROW_POLICIES", "security_projection"},
    {"storage_policy", "SYSTEM.STORAGE_POLICIES;SYSTEM.DISKS", "policy_overlay"},
    {"clusters", "SYSTEM.CLUSTERS;ON CLUSTER;REMOTE", "policy_overlay"},
    {"functions", "SYSTEM.FUNCTIONS;CREATE FUNCTION", "catalog_projection"},
    {"formats", "SYSTEM.FORMATS;FILE;URL;S3", "policy_overlay"},
}};

const std::array<SurfaceDescriptor, 13> kDiagnosticSurfaces{{
    {"parse", "CLICKHOUSE.PARSE.INVALID_INPUT;CLICKHOUSE.PARSE.UNSUPPORTED_SURFACE", "parser"},
    {"system", "CLICKHOUSE.AUTHORITY.UNSUPPORTED_DENIED", "fail_closed"},
    {"backup", "CLICKHOUSE.AUTHORITY.BACKUP_DENIED", "fail_closed"},
    {"storage_admin", "CLICKHOUSE.AUTHORITY.STORAGE_ADMIN_DENIED", "fail_closed"},
    {"external_io", "CLICKHOUSE.AUTHORITY.EXTERNAL_IO_DENIED", "fail_closed"},
    {"etl", "CLICKHOUSE.EMULATION.ETL_ROUTE", "parser_support_udr"},
    {"distributed", "CLICKHOUSE.AUTHORITY.DISTRIBUTED_DENIED", "fail_closed"},
    {"security", "CLICKHOUSE.EMULATION.SECURITY_ROUTE", "parser_support_udr"},
    {"connector", "CLICKHOUSE.EMULATION.CONNECTOR_ROUTE", "parser_support_udr"},
    {"routine", "CLICKHOUSE.EMULATION.ROUTINE_ROUTE", "parser_support_udr"},
    {"maintenance", "CLICKHOUSE.AUTHORITY.UNSUPPORTED_DENIED", "fail_closed"},
    {"catalog", "CLICKHOUSE.CATALOG_OVERLAY.READ_ONLY", "fail_closed"},
    {"cluster_stub", "CLICKHOUSE.AUTHORITY.DISTRIBUTED_DENIED", "compile_time_cluster_stub"},
}};

const scratchbird::parser::donor::DialectProfile kProfile{
    "clickhouse",
    "ClickHouse",
    "sbp_clickhouse",
    "sbup_clickhouse",
    "25.12.10.7-stable",
    "CLICKHOUSE",
    kSblrFamily,
    kPatterns,
    kDatatypeSurfaces,
    kBuiltinSurfaces,
    kCatalogSurfaces,
    kDiagnosticSurfaces,
    58,
    178,
    129,
    27,
    6,
    8,
    12,
    6,
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

std::string ClickhousePackageIdentityJson() {
  return scratchbird::parser::donor::PackageIdentityJson(kProfile);
}

std::string ClickhouseSurfaceReportJson() {
  return scratchbird::parser::donor::SurfaceReportJson(kProfile);
}

} // namespace scratchbird::parser::clickhouse
