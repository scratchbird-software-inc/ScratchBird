// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "neo4j_dialect.hpp"

#include <array>

namespace scratchbird::parser::neo4j {
namespace {

using scratchbird::parser::donor::MappingDisposition;
using scratchbird::parser::donor::OperationPattern;
using scratchbird::parser::donor::PatternMatch;
using scratchbird::parser::donor::SurfaceDescriptor;

constexpr std::string_view kSblrFamily = "sblr.donor.neo4j.profile.v1";

constexpr OperationPattern kPatterns[] = {
    {"SHOW SERVERS", PatternMatch::kPrefix, "admin", "neo4j.admin.show_servers",
     MappingDisposition::kAdmittedSblr, "cluster.admin.inspect_status",
     "sblr.cluster.report.v3:cluster.admin.inspect_status", "cluster.inspect_state", "",
     "", true, false},
    {"ENABLE SERVER", PatternMatch::kPrefix, "admin", "neo4j.admin.enable_server",
     MappingDisposition::kAdmittedSblr, "cluster.job.start_controlled",
     "sblr.cluster.control.v3:cluster.job.start_controlled", "cluster.start_job", "",
     "", true, false},
    {"REALLOCATE DATABASES", PatternMatch::kPrefix, "admin", "neo4j.admin.reallocate_databases",
     MappingDisposition::kAdmittedSblr, "cluster.job.start_controlled",
     "sblr.cluster.control.v3:cluster.job.start_controlled", "cluster.start_job", "",
     "", true, false},
    {"MATCH", PatternMatch::kPrefix, "graph_query", "neo4j.query.match",
     MappingDisposition::kAdmittedSblr, "neo4j.query.match",
     "SBLR_DONOR_NEO4J_MATCH", "EngineGraphMatch", "",
     "", false, false},
    {"RETURN", PatternMatch::kPrefix, "graph_query", "neo4j.query.return",
     MappingDisposition::kAdmittedSblr, "neo4j.query.return",
     "SBLR_DONOR_NEO4J_RETURN", "EngineGraphReturn", "",
     "", false, false},
    {"CREATE CONSTRAINT", PatternMatch::kPrefix, "schema", "neo4j.schema.constraint.create",
     MappingDisposition::kParserSupportUdr, "neo4j.udr.constraint.create",
     "SBLR_DONOR_NEO4J_SCHEMA_ROUTE", "ParserSupportSchemaRoute", "NEO4J.EMULATION.SCHEMA_ROUTE",
     "Neo4j schema constraints route through trusted catalog policy.", true, true},
    {"DROP CONSTRAINT", PatternMatch::kPrefix, "schema", "neo4j.schema.constraint.drop",
     MappingDisposition::kParserSupportUdr, "neo4j.udr.constraint.drop",
     "SBLR_DONOR_NEO4J_SCHEMA_ROUTE", "ParserSupportSchemaRoute", "NEO4J.EMULATION.SCHEMA_ROUTE",
     "Neo4j schema constraints route through trusted catalog policy.", true, true},
    {"CREATE INDEX", PatternMatch::kPrefix, "schema", "neo4j.schema.index.create",
     MappingDisposition::kParserSupportUdr, "neo4j.udr.index.create",
     "SBLR_DONOR_NEO4J_INDEX_ROUTE", "ParserSupportIndexRoute", "NEO4J.EMULATION.INDEX_ROUTE",
     "Neo4j graph indexes route through trusted catalog policy.", true, true},
    {"DROP INDEX", PatternMatch::kPrefix, "schema", "neo4j.schema.index.drop",
     MappingDisposition::kParserSupportUdr, "neo4j.udr.index.drop",
     "SBLR_DONOR_NEO4J_INDEX_ROUTE", "ParserSupportIndexRoute", "NEO4J.EMULATION.INDEX_ROUTE",
     "Neo4j graph indexes route through trusted catalog policy.", true, true},
    {"CREATE USER", PatternMatch::kPrefix, "security", "neo4j.security.create_user",
     MappingDisposition::kParserSupportUdr, "neo4j.udr.security.create_user",
     "SBLR_DONOR_NEO4J_SECURITY_ROUTE", "ParserSupportSecurityRoute", "NEO4J.EMULATION.SECURITY_ROUTE",
     "Neo4j security operations route through trusted security policy.", true, true},
    {"CREATE ROLE", PatternMatch::kPrefix, "security", "neo4j.security.create_role",
     MappingDisposition::kParserSupportUdr, "neo4j.udr.security.create_role",
     "SBLR_DONOR_NEO4J_SECURITY_ROUTE", "ParserSupportSecurityRoute", "NEO4J.EMULATION.SECURITY_ROUTE",
     "Neo4j security operations route through trusted security policy.", true, true},
    {"GRANT", PatternMatch::kPrefix, "security", "neo4j.security.grant",
     MappingDisposition::kParserSupportUdr, "neo4j.udr.security.grant",
     "SBLR_DONOR_NEO4J_SECURITY_ROUTE", "ParserSupportSecurityRoute", "NEO4J.EMULATION.SECURITY_ROUTE",
     "Neo4j privilege changes route through trusted security policy.", true, true},
    {"REVOKE", PatternMatch::kPrefix, "security", "neo4j.security.revoke",
     MappingDisposition::kParserSupportUdr, "neo4j.udr.security.revoke",
     "SBLR_DONOR_NEO4J_SECURITY_ROUTE", "ParserSupportSecurityRoute", "NEO4J.EMULATION.SECURITY_ROUTE",
     "Neo4j privilege changes route through trusted security policy.", true, true},
    {"CALL", PatternMatch::kPrefix, "procedure", "neo4j.procedure.call",
     MappingDisposition::kParserSupportUdr, "neo4j.udr.procedure.call",
     "SBLR_DONOR_NEO4J_PROCEDURE_ROUTE", "ParserSupportProcedureRoute", "NEO4J.EMULATION.PROCEDURE_ROUTE",
     "Neo4j procedures route through trusted package policy.", true, false},
    {"LOAD CSV", PatternMatch::kPrefix, "client_file", "neo4j.client_file.load_csv",
     MappingDisposition::kParserSupportUdr, "neo4j.udr.load_csv",
     "SBLR_DONOR_NEO4J_CLIENT_FILE_ROUTE", "ParserSupportClientFileRoute", "NEO4J.EMULATION.CLIENT_FILE_ROUTE",
     "Neo4j LOAD CSV routes through trusted import policy.", true, true},
    {"MERGE", PatternMatch::kPrefix, "graph_dml", "neo4j.graph.merge",
     MappingDisposition::kAdmittedSblr, "neo4j.graph.merge",
     "SBLR_DONOR_NEO4J_MERGE", "EngineGraphMerge", "",
     "", false, true},
    {"CREATE", PatternMatch::kPrefix, "graph_dml", "neo4j.graph.create",
     MappingDisposition::kAdmittedSblr, "neo4j.graph.create",
     "SBLR_DONOR_NEO4J_CREATE", "EngineGraphCreate", "",
     "", false, true},
    {"DETACH DELETE", PatternMatch::kPrefix, "graph_dml", "neo4j.graph.detach_delete",
     MappingDisposition::kAdmittedSblr, "neo4j.graph.delete",
     "SBLR_DONOR_NEO4J_DELETE", "EngineGraphDelete", "",
     "", false, true},
    {"DELETE", PatternMatch::kPrefix, "graph_dml", "neo4j.graph.delete",
     MappingDisposition::kAdmittedSblr, "neo4j.graph.delete",
     "SBLR_DONOR_NEO4J_DELETE", "EngineGraphDelete", "",
     "", false, true},
    {"SET", PatternMatch::kPrefix, "graph_dml", "neo4j.graph.set",
     MappingDisposition::kAdmittedSblr, "neo4j.graph.set",
     "SBLR_DONOR_NEO4J_SET", "EngineGraphSet", "",
     "", false, true},
    {"REMOVE", PatternMatch::kPrefix, "graph_dml", "neo4j.graph.remove",
     MappingDisposition::kAdmittedSblr, "neo4j.graph.remove",
     "SBLR_DONOR_NEO4J_REMOVE", "EngineGraphRemove", "",
     "", false, true},
    {"UNWIND", PatternMatch::kPrefix, "graph_query", "neo4j.query.unwind",
     MappingDisposition::kAdmittedSblr, "neo4j.query.unwind",
     "SBLR_DONOR_NEO4J_UNWIND", "EngineGraphUnwind", "",
     "", false, false},
    {"SHOW", PatternMatch::kPrefix, "catalog_overlay", "neo4j.catalog.show",
     MappingDisposition::kCatalogProjection, "neo4j.catalog.show",
     "SBLR_DONOR_NEO4J_CATALOG_PROJECT", "EngineCatalogProjection", "",
     "", false, false},
    {"USE", PatternMatch::kPrefix, "session", "neo4j.session.use",
     MappingDisposition::kAdmittedSblr, "neo4j.session.use",
     "SBLR_DONOR_NEO4J_SESSION_ROUTE", "EngineSessionRoute", "",
     "", false, false},
    {"BEGIN", PatternMatch::kPrefix, "transaction", "neo4j.transaction.begin",
     MappingDisposition::kAdmittedSblr, "neo4j.transaction.begin",
     "SBLR_TRANSACTION_BEGIN", "EngineTransactionBegin", "",
     "", false, true},
    {"COMMIT", PatternMatch::kPrefix, "transaction", "neo4j.transaction.commit",
     MappingDisposition::kAdmittedSblr, "neo4j.transaction.commit",
     "SBLR_TRANSACTION_COMMIT", "EngineTransactionCommit", "",
     "", false, true},
    {"ROLLBACK", PatternMatch::kPrefix, "transaction", "neo4j.transaction.rollback",
     MappingDisposition::kAdmittedSblr, "neo4j.transaction.rollback",
     "SBLR_TRANSACTION_ROLLBACK", "EngineTransactionRollback", "",
     "", false, true},
};

const std::array<SurfaceDescriptor, 6> kDatatypeSurfaces{{
    {"graph", "NODE;RELATIONSHIP;PATH", "descriptor"},
    {"scalar", "BOOLEAN;INTEGER;FLOAT;STRING", "descriptor"},
    {"temporal", "DATE;TIME;LOCALTIME;DATETIME;DURATION", "descriptor"},
    {"spatial", "POINT", "parser_support_udr"},
    {"collection", "LIST;MAP", "parser_support_udr"},
    {"null", "NULL", "descriptor"},
}};

const std::array<SurfaceDescriptor, 8> kBuiltinSurfaces{{
    {"predicate", "EXISTS;IS EMPTY;IS NULL", "sblr"},
    {"aggregate", "COUNT;COLLECT;AVG;SUM;MIN;MAX", "sblr"},
    {"graph", "SHORTESTPATH;ALLSHORTESTPATHS", "parser_support_udr"},
    {"string", "TOSTRING;SUBSTRING;REPLACE;SPLIT", "sblr"},
    {"temporal", "DATE;TIME;DATETIME;DURATION", "parser_support_udr"},
    {"spatial", "POINT;DISTANCE", "parser_support_udr"},
    {"list", "HEAD;LAST;SIZE;RANGE", "sblr"},
    {"procedure", "CALL;YIELD", "parser_support_udr"},
}};

const std::array<SurfaceDescriptor, 9> kCatalogSurfaces{{
    {"system_graph", "NEO4J_SYSTEM_GRAPH", "catalog_projection"},
    {"labels", "NEO4J_LABELS", "catalog_projection"},
    {"relationship_types", "NEO4J_RELATIONSHIP_TYPES", "catalog_projection"},
    {"property_keys", "NEO4J_PROPERTY_KEYS", "catalog_projection"},
    {"indexes", "NEO4J_INDEXES", "catalog_projection"},
    {"constraints", "NEO4J_CONSTRAINTS", "catalog_projection"},
    {"users", "NEO4J_USERS", "catalog_projection"},
    {"roles", "NEO4J_ROLES", "catalog_projection"},
    {"procedures", "NEO4J_PROCEDURES", "catalog_projection"},
}};

const std::array<SurfaceDescriptor, 10> kDiagnosticSurfaces{{
    {"parse", "NEO4J.PARSE.INVALID_INPUT;NEO4J.PARSE.UNSUPPORTED_SURFACE", "parser"},
    {"policy", "NEO4J.AUTHORITY.*", "fail_closed"},
    {"udr", "NEO4J.EMULATION.*", "parser_support_udr"},
    {"catalog", "NEO4J.CATALOG_OVERLAY.READ_ONLY", "catalog_projection"},
    {"session", "NEO4J.SESSION.*", "sblr"},
    {"transaction", "NEO4J.TRANSACTION.*", "sblr"},
    {"file_effects", "real_donor_file_effects=false", "authority_invariant"},
    {"donor_execution", "donor_engine_sql_executed=false", "authority_invariant"},
    {"mga", "parser_transaction_finality_authority=false", "authority_invariant"},
    {"support_bundle", "source_text_redacted=true", "diagnostic_redaction"},
}};

const scratchbird::parser::donor::DialectProfile kProfile{
    "neo4j",
    "Neo4j",
    "sbp_neo4j",
    "sbup_neo4j",
    "2026.04.0",
    "NEO4J",
    kSblrFamily,
    kPatterns,
    kDatatypeSurfaces,
    kBuiltinSurfaces,
    kCatalogSurfaces,
    kDiagnosticSurfaces,
    27,
    96,
    54,
    0,
    9,
    0,
    3,
    10,
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
std::string Neo4jPackageIdentityJson() { return scratchbird::parser::donor::PackageIdentityJson(kProfile); }
std::string Neo4jSurfaceReportJson() { return scratchbird::parser::donor::SurfaceReportJson(kProfile); }

} // namespace scratchbird::parser::neo4j
