// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dolt_dialect.hpp"

#include <array>

namespace scratchbird::parser::dolt {
namespace {

using scratchbird::parser::compatibility::MappingDisposition;
using scratchbird::parser::compatibility::OperationPattern;
using scratchbird::parser::compatibility::PatternMatch;
using scratchbird::parser::compatibility::SurfaceDescriptor;

constexpr std::string_view kSblrFamily = "sblr.compatibility.dolt.profile.v1";

constexpr OperationPattern kPatterns[] = {
    {"PUSH", PatternMatch::kPrefix, "remote", "dolt.remote.push",
     MappingDisposition::kParserSupportUdr, "dolt.udr.remote.push",
     "SBLR_COMPAT_DOLT_REMOTE_ROUTE", "ParserSupportRemoteRoute",
     "DOLT.EMULATION.REMOTE_ROUTE",
     "Dolt remote push routes through the Dolt compatibility UDR for remote synchronization admission.", true, false},
    {"PULL", PatternMatch::kPrefix, "remote", "dolt.remote.pull",
     MappingDisposition::kParserSupportUdr, "dolt.udr.remote.pull",
     "SBLR_COMPAT_DOLT_REMOTE_ROUTE", "ParserSupportRemoteRoute",
     "DOLT.EMULATION.REMOTE_ROUTE",
     "Dolt remote pull routes through the Dolt compatibility UDR for remote synchronization admission.", true, false},
    {"FETCH", PatternMatch::kPrefix, "remote", "dolt.remote.fetch",
     MappingDisposition::kParserSupportUdr, "dolt.udr.remote.fetch",
     "SBLR_COMPAT_DOLT_REMOTE_ROUTE", "ParserSupportRemoteRoute",
     "DOLT.EMULATION.REMOTE_ROUTE",
     "Dolt remote fetch routes through the Dolt compatibility UDR for remote synchronization admission.", true, false},
    {"CLONE", PatternMatch::kPrefix, "remote", "dolt.remote.clone",
     MappingDisposition::kParserSupportUdr, "dolt.udr.remote.clone",
     "SBLR_COMPAT_DOLT_REMOTE_ROUTE", "ParserSupportRemoteRoute",
     "DOLT.EMULATION.REMOTE_ROUTE",
     "Dolt clone routes through the Dolt compatibility UDR and must deny parser-owned server-local file effects.", true, false},
    {"DOLT_FETCH", PatternMatch::kContainsFunctionCall, "remote", "dolt.remote.fetch_sql",
     MappingDisposition::kParserSupportUdr, "dolt.udr.remote.fetch_sql",
     "SBLR_COMPAT_DOLT_REMOTE_ROUTE", "ParserSupportRemoteRoute",
     "DOLT.EMULATION.REMOTE_ROUTE",
     "Dolt SQL fetch procedure routes through the Dolt compatibility UDR.", true, false},
    {"DOLT_PUSH", PatternMatch::kContainsFunctionCall, "remote", "dolt.remote.push_sql",
     MappingDisposition::kParserSupportUdr, "dolt.udr.remote.push_sql",
     "SBLR_COMPAT_DOLT_REMOTE_ROUTE", "ParserSupportRemoteRoute",
     "DOLT.EMULATION.REMOTE_ROUTE",
     "Dolt SQL push procedure routes through the Dolt compatibility UDR.", true, false},
    {"DOLT_PULL", PatternMatch::kContainsFunctionCall, "remote", "dolt.remote.pull_sql",
     MappingDisposition::kParserSupportUdr, "dolt.udr.remote.pull_sql",
     "SBLR_COMPAT_DOLT_REMOTE_ROUTE", "ParserSupportRemoteRoute",
     "DOLT.EMULATION.REMOTE_ROUTE",
     "Dolt SQL pull procedure routes through the Dolt compatibility UDR.", true, false},
    {"DOLT_COMMIT", PatternMatch::kContainsFunctionCall, "version_control", "dolt.version.commit",
     MappingDisposition::kParserSupportUdr, "dolt.udr.version.commit",
     "SBLR_COMPAT_DOLT_VERSION_ROUTE", "ParserSupportDoltVersionRoute", "DOLT.EMULATION.VERSION_ROUTE",
     "Dolt version-control procedures route through trusted parser support and engine MGA.", true, true},
    {"DOLT_BRANCH", PatternMatch::kContainsFunctionCall, "version_control", "dolt.version.branch",
     MappingDisposition::kParserSupportUdr, "dolt.udr.version.branch",
     "SBLR_COMPAT_DOLT_BRANCH_ROUTE", "ParserSupportDoltBranchRoute", "DOLT.EMULATION.BRANCH_ROUTE",
     "Dolt branch procedures route through trusted parser support and engine catalog policy.", true, true},
    {"DOLT_CHECKOUT", PatternMatch::kContainsFunctionCall, "version_control", "dolt.version.checkout",
     MappingDisposition::kParserSupportUdr, "dolt.udr.version.checkout",
     "SBLR_COMPAT_DOLT_BRANCH_ROUTE", "ParserSupportDoltBranchRoute", "DOLT.EMULATION.BRANCH_ROUTE",
     "Dolt checkout procedures route through trusted parser support and engine catalog policy.", true, true},
    {"DOLT_MERGE", PatternMatch::kContainsFunctionCall, "version_control", "dolt.version.merge",
     MappingDisposition::kParserSupportUdr, "dolt.udr.version.merge",
     "SBLR_COMPAT_DOLT_MERGE_ROUTE", "ParserSupportDoltMergeRoute", "DOLT.EMULATION.MERGE_ROUTE",
     "Dolt merge procedures route through trusted parser support and engine MGA.", true, true},
    {"DOLT_STASH", PatternMatch::kContainsFunctionCall, "version_control", "dolt.version.stash",
     MappingDisposition::kParserSupportUdr, "dolt.udr.version.stash",
     "SBLR_COMPAT_DOLT_VERSION_ROUTE", "ParserSupportDoltVersionRoute", "DOLT.EMULATION.VERSION_ROUTE",
     "Dolt stash procedures route through trusted parser support and engine MGA.", true, true},
    {"DOLT_DIFF", PatternMatch::kRelationReference, "version_catalog", "dolt.version.diff",
     MappingDisposition::kCatalogProjection, "dolt.catalog.diff",
     "SBLR_COMPAT_DOLT_DIFF_ROUTE", "EngineCatalogProjection", "",
     "", false, false},
    {"DOLT_LOG", PatternMatch::kRelationReference, "version_catalog", "dolt.version.log",
     MappingDisposition::kCatalogProjection, "dolt.catalog.log",
     "SBLR_COMPAT_DOLT_CATALOG_PROJECT", "EngineCatalogProjection", "",
     "", false, false},
    {"DOLT_STATUS", PatternMatch::kRelationReference, "version_catalog", "dolt.version.status",
     MappingDisposition::kCatalogProjection, "dolt.catalog.status",
     "SBLR_COMPAT_DOLT_CATALOG_PROJECT", "EngineCatalogProjection", "",
     "", false, false},
    {"DOLT_BRANCHES", PatternMatch::kRelationReference, "version_catalog", "dolt.version.branches",
     MappingDisposition::kCatalogProjection, "dolt.catalog.branches",
     "SBLR_COMPAT_DOLT_CATALOG_PROJECT", "EngineCatalogProjection", "",
     "", false, false},
    {"SHOW", PatternMatch::kPrefix, "catalog_overlay", "dolt.catalog.show",
     MappingDisposition::kCatalogProjection, "dolt.catalog.show",
     "SBLR_COMPAT_DOLT_CATALOG_PROJECT", "EngineCatalogProjection", "",
     "", false, false},
    {"DESCRIBE", PatternMatch::kPrefix, "catalog_overlay", "dolt.catalog.describe",
     MappingDisposition::kCatalogProjection, "dolt.catalog.describe",
     "SBLR_COMPAT_DOLT_CATALOG_PROJECT", "EngineCatalogProjection", "",
     "", false, false},
    {"CREATE DATABASE", PatternMatch::kPrefix, "database_lifecycle", "dolt.lifecycle.create_database",
     MappingDisposition::kScratchBirdLifecycleApi, "dolt.lifecycle.create_database",
     "SBLR_LIFECYCLE_CREATE_DATABASE", "EngineCreateLifecycle", "",
     "", false, false},
    {"DROP DATABASE", PatternMatch::kPrefix, "database_lifecycle", "dolt.lifecycle.drop_database",
     MappingDisposition::kScratchBirdLifecycleApi, "dolt.lifecycle.drop_database",
     "SBLR_LIFECYCLE_DROP_DATABASE", "EngineDropLifecycle", "",
     "", true, false},
    {"CREATE USER", PatternMatch::kPrefix, "security", "dolt.security.create_user",
     MappingDisposition::kParserSupportUdr, "dolt.udr.security.create_user",
     "SBLR_COMPAT_DOLT_SECURITY_ROUTE", "ParserSupportSecurityRoute", "DOLT.EMULATION.SECURITY_ROUTE",
     "Dolt account management routes through trusted security policy.", true, false},
    {"GRANT", PatternMatch::kPrefix, "security", "dolt.security.grant",
     MappingDisposition::kParserSupportUdr, "dolt.udr.security.grant",
     "SBLR_COMPAT_DOLT_SECURITY_ROUTE", "ParserSupportSecurityRoute", "DOLT.EMULATION.SECURITY_ROUTE",
     "Dolt privilege changes route through trusted security policy.", true, true},
    {"REVOKE", PatternMatch::kPrefix, "security", "dolt.security.revoke",
     MappingDisposition::kParserSupportUdr, "dolt.udr.security.revoke",
     "SBLR_COMPAT_DOLT_SECURITY_ROUTE", "ParserSupportSecurityRoute", "DOLT.EMULATION.SECURITY_ROUTE",
     "Dolt privilege changes route through trusted security policy.", true, true},
    {"SET", PatternMatch::kPrefix, "session", "dolt.session.set",
     MappingDisposition::kAdmittedSblr, "dolt.session.set",
     "SBLR_COMPAT_DOLT_SET", "EngineSessionSet", "",
     "", false, false},
    {"USE", PatternMatch::kPrefix, "session", "dolt.session.use_database",
     MappingDisposition::kAdmittedSblr, "dolt.session.use_database",
     "SBLR_COMPAT_DOLT_USE_DATABASE", "EngineSessionRoute", "",
     "", false, false},
    {"CREATE", PatternMatch::kPrefix, "ddl", "dolt.ddl.create",
     MappingDisposition::kAdmittedSblr, "dolt.ddl.create",
     "SBLR_COMPAT_DOLT_DDL_CREATE", "EngineDdlCreate", "",
     "", true, true},
    {"ALTER", PatternMatch::kPrefix, "ddl", "dolt.ddl.alter",
     MappingDisposition::kAdmittedSblr, "dolt.ddl.alter",
     "SBLR_COMPAT_DOLT_DDL_ALTER", "EngineDdlAlter", "",
     "", true, true},
    {"DROP", PatternMatch::kPrefix, "ddl", "dolt.ddl.drop",
     MappingDisposition::kAdmittedSblr, "dolt.ddl.drop",
     "SBLR_COMPAT_DOLT_DDL_DROP", "EngineDdlDrop", "",
     "", true, true},
    {"INSERT", PatternMatch::kPrefix, "dml", "dolt.dml.insert",
     MappingDisposition::kAdmittedSblr, "dolt.dml.insert",
     "SBLR_COMPAT_DOLT_INSERT", "EngineDmlInsert", "",
     "", false, true},
    {"UPDATE", PatternMatch::kPrefix, "dml", "dolt.dml.update",
     MappingDisposition::kAdmittedSblr, "dolt.dml.update",
     "SBLR_COMPAT_DOLT_UPDATE", "EngineDmlUpdate", "",
     "", false, true},
    {"DELETE", PatternMatch::kPrefix, "dml", "dolt.dml.delete",
     MappingDisposition::kAdmittedSblr, "dolt.dml.delete",
     "SBLR_COMPAT_DOLT_DELETE", "EngineDmlDelete", "",
     "", false, true},
    {"SELECT", PatternMatch::kPrefix, "query", "dolt.query.select",
     MappingDisposition::kAdmittedSblr, "dolt.query.select",
     "SBLR_COMPAT_DOLT_SELECT", "EngineQuerySelect", "",
     "", false, false},
    {"WITH", PatternMatch::kPrefix, "query", "dolt.query.with",
     MappingDisposition::kAdmittedSblr, "dolt.query.with",
     "SBLR_COMPAT_DOLT_SELECT", "EngineQuerySelect", "",
     "", false, false},
    {"START TRANSACTION", PatternMatch::kPrefix, "transaction", "dolt.transaction.start",
     MappingDisposition::kAdmittedSblr, "dolt.transaction.start",
     "SBLR_TRANSACTION_BEGIN", "EngineBeginTransaction", "",
     "", false, false},
    {"COMMIT", PatternMatch::kPrefix, "transaction", "dolt.transaction.commit",
     MappingDisposition::kAdmittedSblr, "dolt.transaction.commit",
     "SBLR_TRANSACTION_COMMIT", "EngineCommitTransaction", "",
     "", false, true},
    {"ROLLBACK", PatternMatch::kPrefix, "transaction", "dolt.transaction.rollback",
     MappingDisposition::kAdmittedSblr, "dolt.transaction.rollback",
     "SBLR_TRANSACTION_ROLLBACK", "EngineRollbackTransaction", "",
     "", false, true},
};

const std::array<SurfaceDescriptor, 10> kDatatypeSurfaces{{
    {"numeric", "TINYINT;SMALLINT;INT;BIGINT;DECIMAL;FLOAT;DOUBLE", "descriptor"},
    {"text", "CHAR;VARCHAR;TEXT;LONGTEXT", "descriptor"},
    {"binary", "BINARY;VARBINARY;BLOB", "descriptor"},
    {"temporal", "DATE;TIME;DATETIME;TIMESTAMP;YEAR", "descriptor"},
    {"boolean", "BOOL;BOOLEAN", "descriptor_alias"},
    {"json", "JSON", "descriptor"},
    {"enum_set", "ENUM;SET", "parser_support_udr"},
    {"spatial", "GEOMETRY;POINT;LINESTRING;POLYGON", "parser_support_udr"},
    {"version_hash", "HASHOF;DOLT_HASHOF;COMMIT_HASH", "catalog_policy"},
    {"revision_selector", "AS OF;DOLT_REF;DOLT_REV", "catalog_policy"},
}};

const std::array<SurfaceDescriptor, 9> kBuiltinSurfaces{{
    {"aggregate", "COUNT;SUM;AVG;MIN;MAX;GROUP_CONCAT", "sblr"},
    {"string", "CONCAT;SUBSTRING;LOWER;UPPER;TRIM", "sblr"},
    {"json", "JSON_EXTRACT;JSON_OBJECT;JSON_ARRAY", "sblr"},
    {"version_control", "DOLT_COMMIT;DOLT_BRANCH;DOLT_CHECKOUT;DOLT_MERGE;DOLT_STASH", "parser_support_udr"},
    {"diff", "DOLT_DIFF;DOLT_COMMIT_DIFF;DOLT_PATCH", "catalog_projection"},
    {"history", "DOLT_LOG;DOLT_STATUS;DOLT_HASHOF", "catalog_projection"},
    {"remote", "DOLT_FETCH;DOLT_PUSH;DOLT_PULL", "parser_support_udr"},
    {"security", "USER;ROLE;GRANT;REVOKE", "parser_support_udr"},
    {"session", "USE;SET", "sblr"},
}};

const std::array<SurfaceDescriptor, 10> kCatalogSurfaces{{
    {"branches", "DOLT_BRANCHES", "catalog_projection"},
    {"commits", "DOLT_LOG;DOLT_COMMITS", "catalog_projection"},
    {"status", "DOLT_STATUS", "catalog_projection"},
    {"diff", "DOLT_DIFF;DOLT_COMMIT_DIFF", "catalog_projection"},
    {"conflicts", "DOLT_CONFLICTS;DOLT_CONSTRAINT_VIOLATIONS", "catalog_projection"},
    {"schemas", "DOLT_SCHEMAS", "catalog_projection"},
    {"docs", "DOLT_DOCS", "catalog_projection"},
    {"remotes", "DOLT_REMOTES", "catalog_projection"},
    {"tags", "DOLT_TAGS", "catalog_projection"},
    {"procedures", "DOLT_PROCEDURES", "catalog_projection"},
}};

const std::array<SurfaceDescriptor, 10> kDiagnosticSurfaces{{
    {"parse", "DOLT.PARSE.INVALID_INPUT;DOLT.PARSE.UNSUPPORTED_SURFACE", "parser"},
    {"policy", "DOLT.AUTHORITY.*", "fail_closed"},
    {"udr", "DOLT.EMULATION.*", "parser_support_udr"},
    {"catalog", "DOLT.CATALOG_OVERLAY.READ_ONLY", "catalog_projection"},
    {"session", "DOLT.SESSION.*", "sblr"},
    {"transaction", "DOLT.TRANSACTION.*", "sblr"},
    {"file_effects", "real_reference_file_effects=false", "authority_invariant"},
    {"reference_execution", "reference_engine_sql_executed=false", "authority_invariant"},
    {"mga", "parser_transaction_finality_authority=false", "authority_invariant"},
    {"support_bundle", "source_text_redacted=true", "diagnostic_redaction"},
}};

const scratchbird::parser::compatibility::DialectProfile kProfile{
    "dolt",
    "Dolt",
    "sbp_dolt",
    "sbup_dolt",
    "1.86.6",
    "DOLT",
    kSblrFamily,
    kPatterns,
    kDatatypeSurfaces,
    kBuiltinSurfaces,
    kCatalogSurfaces,
    kDiagnosticSurfaces,
    36,
    84,
    64,
    0,
    10,
    0,
    0,
    15,
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
std::string DoltPackageIdentityJson() { return scratchbird::parser::compatibility::PackageIdentityJson(kProfile); }
std::string DoltSurfaceReportJson() { return scratchbird::parser::compatibility::SurfaceReportJson(kProfile); }

} // namespace scratchbird::parser::dolt
