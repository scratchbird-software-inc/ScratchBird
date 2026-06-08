// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cassandra_dialect.hpp"

#include <array>

namespace scratchbird::parser::cassandra {
namespace {

using scratchbird::parser::donor::MappingDisposition;
using scratchbird::parser::donor::OperationPattern;
using scratchbird::parser::donor::PatternMatch;
using scratchbird::parser::donor::SurfaceDescriptor;

constexpr std::string_view kSblrFamily = "sblr.donor.cassandra.profile.v1";

constexpr OperationPattern kPatterns[] = {
    {"NODETOOL", PatternMatch::kPrefix, "admin", "cassandra.admin.nodetool",
     MappingDisposition::kUnsupportedRefusal, "cassandra.policy.unsupported.nodetool",
     "", "", "CASSANDRA.AUTHORITY.UNSUPPORTED_DENIED",
     "Cassandra NODETOOL is a donor low-level utility surface and is outside donor parser authority.",
     true, false},
    {"REPAIR", PatternMatch::kPrefix, "admin", "cassandra.admin.repair",
     MappingDisposition::kUnsupportedRefusal, "cassandra.policy.unsupported.repair",
     "", "", "CASSANDRA.AUTHORITY.UNSUPPORTED_DENIED",
     "Cassandra REPAIR is a donor repair utility surface and is outside donor parser authority.",
     true, false},
    {"COPY", PatternMatch::kPrefix, "bulk_io", "cassandra.cqlsh.copy",
     MappingDisposition::kParserSupportUdr, "cassandra.udr.copy_file",
     "SBLR_DONOR_CASSANDRA_CLIENT_FILE_ROUTE", "ParserSupportClientFileRoute",
     "CASSANDRA.EMULATION.CLIENT_FILE_ROUTE",
     "cqlsh COPY file effects route through trusted import/export policy.", true, true},
    {"SOURCE", PatternMatch::kPrefix, "client_file", "cassandra.cqlsh.source",
     MappingDisposition::kParserSupportUdr, "cassandra.udr.source_file",
     "SBLR_DONOR_CASSANDRA_CLIENT_FILE_ROUTE", "ParserSupportClientFileRoute",
     "CASSANDRA.EMULATION.CLIENT_FILE_ROUTE",
     "cqlsh SOURCE routes through trusted client-script policy.", true, false},
    {"CONSISTENCY", PatternMatch::kPrefix, "session", "cassandra.session.consistency",
     MappingDisposition::kParserSupportUdr, "cassandra.udr.consistency",
     "SBLR_DONOR_CASSANDRA_CONSISTENCY_ROUTE", "ParserSupportConsistencyRoute", "CASSANDRA.EMULATION.CONSISTENCY_ROUTE",
     "Consistency policy routes through trusted engine session policy.", true, false},
    {"PAGING", PatternMatch::kPrefix, "session", "cassandra.session.paging",
     MappingDisposition::kParserSupportUdr, "cassandra.udr.paging",
     "SBLR_DONOR_CASSANDRA_SESSION_ROUTE", "ParserSupportSessionRoute", "CASSANDRA.EMULATION.SESSION_ROUTE",
     "cqlsh paging policy routes through trusted session policy.", true, false},
    {"CREATE KEYSPACE", PatternMatch::kPrefix, "keyspace", "cassandra.keyspace.create",
     MappingDisposition::kParserSupportUdr, "cassandra.udr.keyspace.create",
     "SBLR_DONOR_CASSANDRA_KEYSPACE_ROUTE", "ParserSupportKeyspaceRoute", "CASSANDRA.EMULATION.KEYSPACE_ROUTE",
     "Keyspace replication policy routes through trusted catalog policy.", true, false},
    {"ALTER KEYSPACE", PatternMatch::kPrefix, "keyspace", "cassandra.keyspace.alter",
     MappingDisposition::kParserSupportUdr, "cassandra.udr.keyspace.alter",
     "SBLR_DONOR_CASSANDRA_KEYSPACE_ROUTE", "ParserSupportKeyspaceRoute", "CASSANDRA.EMULATION.KEYSPACE_ROUTE",
     "Keyspace replication policy routes through trusted catalog policy.", true, false},
    {"DROP KEYSPACE", PatternMatch::kPrefix, "keyspace", "cassandra.keyspace.drop",
     MappingDisposition::kParserSupportUdr, "cassandra.udr.keyspace.drop",
     "SBLR_DONOR_CASSANDRA_KEYSPACE_ROUTE", "ParserSupportKeyspaceRoute", "CASSANDRA.EMULATION.KEYSPACE_ROUTE",
     "Keyspace lifecycle routes through trusted catalog policy.", true, false},
    {"CREATE MATERIALIZED VIEW", PatternMatch::kPrefix, "materialized_view", "cassandra.materialized_view.create",
     MappingDisposition::kParserSupportUdr, "cassandra.udr.materialized_view.create",
     "SBLR_DONOR_CASSANDRA_MATERIALIZED_VIEW_ROUTE", "ParserSupportMaterializedViewRoute", "CASSANDRA.EMULATION.MATERIALIZED_VIEW_ROUTE",
     "Cassandra materialized-view maintenance routes through trusted policy.", true, false},
    {"CREATE TYPE", PatternMatch::kPrefix, "type", "cassandra.type.create",
     MappingDisposition::kParserSupportUdr, "cassandra.udr.type.create",
     "SBLR_DONOR_CASSANDRA_TYPE_ROUTE", "ParserSupportTypeRoute", "CASSANDRA.EMULATION.TYPE_ROUTE",
     "User-defined type catalog policy routes through trusted package support.", true, false},
    {"ALTER TYPE", PatternMatch::kPrefix, "type", "cassandra.type.alter",
     MappingDisposition::kParserSupportUdr, "cassandra.udr.type.alter",
     "SBLR_DONOR_CASSANDRA_TYPE_ROUTE", "ParserSupportTypeRoute", "CASSANDRA.EMULATION.TYPE_ROUTE",
     "User-defined type catalog policy routes through trusted package support.", true, false},
    {"DROP TYPE", PatternMatch::kPrefix, "type", "cassandra.type.drop",
     MappingDisposition::kParserSupportUdr, "cassandra.udr.type.drop",
     "SBLR_DONOR_CASSANDRA_TYPE_ROUTE", "ParserSupportTypeRoute", "CASSANDRA.EMULATION.TYPE_ROUTE",
     "User-defined type catalog policy routes through trusted package support.", true, false},
    {"CREATE ROLE", PatternMatch::kPrefix, "security", "cassandra.security.create_role",
     MappingDisposition::kParserSupportUdr, "cassandra.udr.security.create_role",
     "SBLR_DONOR_CASSANDRA_SECURITY_ROUTE", "ParserSupportSecurityRoute", "CASSANDRA.EMULATION.SECURITY_ROUTE",
     "Role management routes through trusted security policy.", true, false},
    {"ALTER ROLE", PatternMatch::kPrefix, "security", "cassandra.security.alter_role",
     MappingDisposition::kParserSupportUdr, "cassandra.udr.security.alter_role",
     "SBLR_DONOR_CASSANDRA_SECURITY_ROUTE", "ParserSupportSecurityRoute", "CASSANDRA.EMULATION.SECURITY_ROUTE",
     "Role management routes through trusted security policy.", true, false},
    {"DROP ROLE", PatternMatch::kPrefix, "security", "cassandra.security.drop_role",
     MappingDisposition::kParserSupportUdr, "cassandra.udr.security.drop_role",
     "SBLR_DONOR_CASSANDRA_SECURITY_ROUTE", "ParserSupportSecurityRoute", "CASSANDRA.EMULATION.SECURITY_ROUTE",
     "Role management routes through trusted security policy.", true, false},
    {"GRANT", PatternMatch::kPrefix, "security", "cassandra.security.grant",
     MappingDisposition::kParserSupportUdr, "cassandra.udr.security.grant",
     "SBLR_DONOR_CASSANDRA_SECURITY_ROUTE", "ParserSupportSecurityRoute", "CASSANDRA.EMULATION.SECURITY_ROUTE",
     "Privilege changes route through trusted security policy.", true, false},
    {"REVOKE", PatternMatch::kPrefix, "security", "cassandra.security.revoke",
     MappingDisposition::kParserSupportUdr, "cassandra.udr.security.revoke",
     "SBLR_DONOR_CASSANDRA_SECURITY_ROUTE", "ParserSupportSecurityRoute", "CASSANDRA.EMULATION.SECURITY_ROUTE",
     "Privilege changes route through trusted security policy.", true, false},
    {"DESCRIBE", PatternMatch::kPrefix, "catalog_overlay", "cassandra.catalog.describe",
     MappingDisposition::kCatalogProjection, "cassandra.catalog.describe",
     "SBLR_DONOR_CASSANDRA_CATALOG_PROJECT", "EngineCatalogProjection", "",
     "", false, false},
    {"SELECT JSON", PatternMatch::kPrefix, "query", "cassandra.query.select_json",
     MappingDisposition::kAdmittedSblr, "cassandra.query.select_json",
     "SBLR_DONOR_CASSANDRA_SELECT", "EngineQuerySelect", "",
     "", false, false},
    {"SELECT", PatternMatch::kPrefix, "query", "cassandra.query.select",
     MappingDisposition::kAdmittedSblr, "cassandra.query.select",
     "SBLR_DONOR_CASSANDRA_SELECT", "EngineQuerySelect", "",
     "", false, false},
    {"INSERT JSON", PatternMatch::kPrefix, "dml", "cassandra.dml.insert_json",
     MappingDisposition::kAdmittedSblr, "cassandra.dml.insert_json",
     "SBLR_DONOR_CASSANDRA_INSERT", "EngineDmlInsert", "",
     "", false, true},
    {"INSERT", PatternMatch::kPrefix, "dml", "cassandra.dml.insert",
     MappingDisposition::kAdmittedSblr, "cassandra.dml.insert",
     "SBLR_DONOR_CASSANDRA_INSERT", "EngineDmlInsert", "",
     "", false, true},
    {"UPDATE", PatternMatch::kPrefix, "dml", "cassandra.dml.update",
     MappingDisposition::kAdmittedSblr, "cassandra.dml.update",
     "SBLR_DONOR_CASSANDRA_UPDATE", "EngineDmlUpdate", "",
     "", false, true},
    {"DELETE", PatternMatch::kPrefix, "dml", "cassandra.dml.delete",
     MappingDisposition::kAdmittedSblr, "cassandra.dml.delete",
     "SBLR_DONOR_CASSANDRA_DELETE", "EngineDmlDelete", "",
     "", false, true},
    {"BEGIN BATCH", PatternMatch::kPrefix, "batch", "cassandra.batch.begin",
     MappingDisposition::kParserSupportUdr, "cassandra.udr.batch.begin",
     "SBLR_DONOR_CASSANDRA_BATCH", "ParserSupportBatchRoute", "CASSANDRA.EMULATION.BATCH_ROUTE",
     "Cassandra batch semantics route through trusted policy.", true, true},
    {"APPLY BATCH", PatternMatch::kPrefix, "batch", "cassandra.batch.apply",
     MappingDisposition::kParserSupportUdr, "cassandra.udr.batch.apply",
     "SBLR_DONOR_CASSANDRA_BATCH", "ParserSupportBatchRoute", "CASSANDRA.EMULATION.BATCH_ROUTE",
     "Cassandra batch semantics route through trusted policy.", true, true},
    {"CREATE TABLE", PatternMatch::kPrefix, "ddl", "cassandra.ddl.create_table",
     MappingDisposition::kAdmittedSblr, "cassandra.ddl.create_table",
     "SBLR_DONOR_CASSANDRA_DDL_CREATE", "EngineDdlCreate", "",
     "", true, true},
    {"ALTER TABLE", PatternMatch::kPrefix, "ddl", "cassandra.ddl.alter_table",
     MappingDisposition::kAdmittedSblr, "cassandra.ddl.alter_table",
     "SBLR_DONOR_CASSANDRA_DDL_ALTER", "EngineDdlAlter", "",
     "", true, true},
    {"DROP TABLE", PatternMatch::kPrefix, "ddl", "cassandra.ddl.drop_table",
     MappingDisposition::kAdmittedSblr, "cassandra.ddl.drop_table",
     "SBLR_DONOR_CASSANDRA_DDL_DROP", "EngineDdlDrop", "",
     "", true, true},
    {"TRUNCATE", PatternMatch::kPrefix, "ddl", "cassandra.ddl.truncate",
     MappingDisposition::kAdmittedSblr, "cassandra.ddl.truncate",
     "SBLR_DONOR_CASSANDRA_DDL_TRUNCATE", "EngineDdlTruncate", "",
     "", true, true},
    {"USE", PatternMatch::kPrefix, "session", "cassandra.session.use_keyspace",
     MappingDisposition::kAdmittedSblr, "cassandra.session.use_keyspace",
     "SBLR_DONOR_CASSANDRA_USE_KEYSPACE", "EngineSessionRoute", "",
     "", false, false},
};

const std::array<SurfaceDescriptor, 10> kDatatypeSurfaces{{
    {"numeric", "TINYINT;SMALLINT;INT;BIGINT;VARINT;DECIMAL;FLOAT;DOUBLE", "descriptor"},
    {"text", "ASCII;TEXT;VARCHAR", "descriptor"},
    {"binary", "BLOB", "descriptor"},
    {"temporal", "DATE;TIME;TIMESTAMP;DURATION", "descriptor"},
    {"boolean", "BOOLEAN", "descriptor"},
    {"uuid", "UUID;TIMEUUID", "descriptor"},
    {"collection", "LIST;SET;MAP", "parser_support_udr"},
    {"tuple", "TUPLE", "parser_support_udr"},
    {"user_defined_type", "CREATE TYPE", "parser_support_udr"},
    {"vector", "VECTOR", "parser_support_udr"},
}};

const std::array<SurfaceDescriptor, 10> kBuiltinSurfaces{{
    {"aggregate", "COUNT;SUM;AVG;MIN;MAX", "sblr"},
    {"collection", "CONTAINS;CONTAINS KEY", "sblr"},
    {"token", "TOKEN", "catalog_projection"},
    {"uuid", "UUID;NOW;MIN_TIMEUUID;MAX_TIMEUUID", "sblr"},
    {"conversion", "CAST", "sblr"},
    {"json", "SELECT JSON;INSERT JSON", "sblr"},
    {"security", "CURRENTUSER;CURRENT_USER", "catalog_projection"},
    {"ttl", "TTL;WRITETIME", "catalog_projection"},
    {"blob", "BLOBASINT;INTASBLOB;TEXTASBLOB", "parser_support_udr"},
    {"vector", "SIMILARITY_COSINE;SIMILARITY_DOT_PRODUCT", "parser_support_udr"},
}};

const std::array<SurfaceDescriptor, 9> kCatalogSurfaces{{
    {"system_schema", "SYSTEM_SCHEMA.*", "catalog_projection"},
    {"system_auth", "SYSTEM_AUTH.*", "catalog_projection"},
    {"system_distributed", "SYSTEM_DISTRIBUTED.*", "catalog_projection"},
    {"keyspace_metadata", "DESCRIBE KEYSPACES;DESCRIBE KEYSPACE", "catalog_projection"},
    {"table_metadata", "DESCRIBE TABLES;DESCRIBE TABLE", "catalog_projection"},
    {"type_metadata", "DESCRIBE TYPES;DESCRIBE TYPE", "catalog_projection"},
    {"role_metadata", "LIST ROLES;LIST PERMISSIONS", "catalog_projection"},
    {"tracing", "TRACING;SYSTEM_TRACES", "catalog_projection"},
    {"token_ring", "SYSTEM.LOCAL;SYSTEM.PEERS", "catalog_projection"},
}};

const std::array<SurfaceDescriptor, 10> kDiagnosticSurfaces{{
    {"parse", "CASSANDRA.PARSE.INVALID_INPUT;CASSANDRA.PARSE.UNSUPPORTED_SURFACE", "parser"},
    {"policy", "CASSANDRA.AUTHORITY.*", "fail_closed"},
    {"udr", "CASSANDRA.EMULATION.*", "parser_support_udr"},
    {"catalog", "CASSANDRA.CATALOG_OVERLAY.READ_ONLY", "catalog_projection"},
    {"session", "CASSANDRA.SESSION.*", "sblr"},
    {"transaction", "CASSANDRA.TRANSACTION.*", "sblr"},
    {"file_effects", "real_donor_file_effects=false", "authority_invariant"},
    {"donor_execution", "donor_engine_sql_executed=false", "authority_invariant"},
    {"mga", "parser_transaction_finality_authority=false", "authority_invariant"},
    {"support_bundle", "source_text_redacted=true", "diagnostic_redaction"},
}};

const scratchbird::parser::donor::DialectProfile kProfile{
    "cassandra",
    "Cassandra",
    "sbp_cassandra",
    "sbup_cassandra",
    "5.0.8",
    "CASSANDRA",
    kSblrFamily,
    kPatterns,
    kDatatypeSurfaces,
    kBuiltinSurfaces,
    kCatalogSurfaces,
    kDiagnosticSurfaces,
    32,
    120,
    90,
    0,
    9,
    0,
    2,
    18,
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

std::string CassandraPackageIdentityJson() {
  return scratchbird::parser::donor::PackageIdentityJson(kProfile);
}

std::string CassandraSurfaceReportJson() {
  return scratchbird::parser::donor::SurfaceReportJson(kProfile);
}

} // namespace scratchbird::parser::cassandra
