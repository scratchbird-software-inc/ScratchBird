// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "foundationdb_dialect.hpp"

#include <array>

namespace scratchbird::parser::foundationdb {
namespace {

using scratchbird::parser::compatibility::MappingDisposition;
using scratchbird::parser::compatibility::OperationPattern;
using scratchbird::parser::compatibility::PatternMatch;
using scratchbird::parser::compatibility::SurfaceDescriptor;

constexpr std::string_view kSblrFamily = "sblr.compatibility.foundationdb.profile.v1";

constexpr OperationPattern kPatterns[] = {
    {"CONFIGURE", PatternMatch::kPrefix, "admin", "foundationdb.admin.configure",
     MappingDisposition::kAdmittedSblr, "cluster.job.start_controlled",
     "sblr.cluster.control.v3:cluster.job.start_controlled", "cluster.start_job", "",
     "", true, false},
    {"COORDINATORS", PatternMatch::kPrefix, "admin", "foundationdb.admin.coordinators",
     MappingDisposition::kAdmittedSblr, "cluster.job.start_controlled",
     "sblr.cluster.control.v3:cluster.job.start_controlled", "cluster.start_job", "",
     "", true, false},
    {"EXCLUDE", PatternMatch::kPrefix, "admin", "foundationdb.admin.exclude",
     MappingDisposition::kAdmittedSblr, "cluster.job.start_controlled",
     "sblr.cluster.control.v3:cluster.job.start_controlled", "cluster.start_job", "",
     "", true, false},
    {"INCLUDE", PatternMatch::kPrefix, "admin", "foundationdb.admin.include",
     MappingDisposition::kAdmittedSblr, "cluster.job.start_controlled",
     "sblr.cluster.control.v3:cluster.job.start_controlled", "cluster.start_job", "",
     "", true, false},
    {"KILL", PatternMatch::kPrefix, "admin", "foundationdb.admin.kill",
     MappingDisposition::kAdmittedSblr, "cluster.job.start_controlled",
     "sblr.cluster.control.v3:cluster.job.start_controlled", "cluster.start_job", "",
     "", true, false},
    {"FORCE_RECOVERY", PatternMatch::kPrefix, "admin", "foundationdb.admin.force_recovery",
     MappingDisposition::kAdmittedSblr, "cluster.tx.recover_limbo_participant",
     "required_new:sblr.cluster.transaction.v1:cluster.tx.recover_limbo_participant", "cluster.recover_limbo_participant", "",
     "", true, false},
    {"GET_RANGE", PatternMatch::kPrefix, "kv_read", "foundationdb.kv.get_range",
     MappingDisposition::kAdmittedSblr, "foundationdb.kv.get_range",
     "SBLR_COMPAT_FOUNDATIONDB_GET_RANGE", "EngineKvGetRange", "",
     "", false, false},
    {"GET_KEY", PatternMatch::kPrefix, "kv_read", "foundationdb.kv.get_key",
     MappingDisposition::kAdmittedSblr, "foundationdb.kv.get_key",
     "SBLR_COMPAT_FOUNDATIONDB_GET", "EngineKvGet", "",
     "", false, false},
    {"GET", PatternMatch::kPrefix, "kv_read", "foundationdb.kv.get",
     MappingDisposition::kAdmittedSblr, "foundationdb.kv.get",
     "SBLR_COMPAT_FOUNDATIONDB_GET", "EngineKvGet", "",
     "", false, false},
    {"CLEAR_RANGE", PatternMatch::kPrefix, "kv_write", "foundationdb.kv.clear_range",
     MappingDisposition::kAdmittedSblr, "foundationdb.kv.clear_range",
     "SBLR_COMPAT_FOUNDATIONDB_CLEAR_RANGE", "EngineKvClearRange", "",
     "", false, true},
    {"CLEAR", PatternMatch::kPrefix, "kv_write", "foundationdb.kv.clear",
     MappingDisposition::kAdmittedSblr, "foundationdb.kv.clear",
     "SBLR_COMPAT_FOUNDATIONDB_CLEAR", "EngineKvClear", "",
     "", false, true},
    {"SET_VERSIONSTAMPED", PatternMatch::kPrefix, "kv_write", "foundationdb.kv.set_versionstamped",
     MappingDisposition::kParserSupportUdr, "foundationdb.udr.versionstamp",
     "SBLR_COMPAT_FOUNDATIONDB_VERSIONSTAMP_ROUTE", "ParserSupportVersionstampRoute", "FOUNDATIONDB.EMULATION.VERSIONSTAMP_ROUTE",
     "FoundationDB versionstamped mutations route through trusted parser support and engine commit context.", true, true},
    {"SET", PatternMatch::kPrefix, "kv_write", "foundationdb.kv.set",
     MappingDisposition::kAdmittedSblr, "foundationdb.kv.set",
     "SBLR_COMPAT_FOUNDATIONDB_SET", "EngineKvSet", "",
     "", false, true},
    {"ATOMIC_OP", PatternMatch::kPrefix, "kv_write", "foundationdb.kv.atomic_op",
     MappingDisposition::kParserSupportUdr, "foundationdb.udr.atomic_op",
     "SBLR_COMPAT_FOUNDATIONDB_ATOMIC_ROUTE", "ParserSupportAtomicRoute", "FOUNDATIONDB.EMULATION.ATOMIC_ROUTE",
     "FoundationDB atomic operation encodings route through trusted parser support.", true, true},
    {"WATCH", PatternMatch::kPrefix, "kv_read", "foundationdb.kv.watch",
     MappingDisposition::kParserSupportUdr, "foundationdb.udr.watch",
     "SBLR_COMPAT_FOUNDATIONDB_WATCH_ROUTE", "ParserSupportWatchRoute", "FOUNDATIONDB.EMULATION.WATCH_ROUTE",
     "FoundationDB watches route through trusted parser support and listener cancellation policy.", true, false},
    {"DIRECTORY_CREATE", PatternMatch::kPrefix, "directory", "foundationdb.directory.create",
     MappingDisposition::kParserSupportUdr, "foundationdb.udr.directory.create",
     "SBLR_COMPAT_FOUNDATIONDB_DIRECTORY_ROUTE", "ParserSupportDirectoryRoute", "FOUNDATIONDB.EMULATION.DIRECTORY_ROUTE",
     "FoundationDB directory layer operations route through trusted parser support.", true, true},
    {"DIRECTORY_OPEN", PatternMatch::kPrefix, "directory", "foundationdb.directory.open",
     MappingDisposition::kCatalogProjection, "foundationdb.directory.open",
     "SBLR_COMPAT_FOUNDATIONDB_DIRECTORY_ROUTE", "EngineCatalogProjection", "",
     "", false, false},
    {"DIRECTORY_REMOVE", PatternMatch::kPrefix, "directory", "foundationdb.directory.remove",
     MappingDisposition::kParserSupportUdr, "foundationdb.udr.directory.remove",
     "SBLR_COMPAT_FOUNDATIONDB_DIRECTORY_ROUTE", "ParserSupportDirectoryRoute", "FOUNDATIONDB.EMULATION.DIRECTORY_ROUTE",
     "FoundationDB directory removal routes through trusted parser support.", true, true},
    {"TUPLE_PACK", PatternMatch::kPrefix, "tuple", "foundationdb.tuple.pack",
     MappingDisposition::kParserSupportUdr, "foundationdb.udr.tuple.pack",
     "SBLR_COMPAT_FOUNDATIONDB_TUPLE_ROUTE", "ParserSupportTupleRoute", "FOUNDATIONDB.EMULATION.TUPLE_ROUTE",
     "FoundationDB tuple encoding routes through trusted parser support.", true, false},
    {"TUPLE_UNPACK", PatternMatch::kPrefix, "tuple", "foundationdb.tuple.unpack",
     MappingDisposition::kParserSupportUdr, "foundationdb.udr.tuple.unpack",
     "SBLR_COMPAT_FOUNDATIONDB_TUPLE_ROUTE", "ParserSupportTupleRoute", "FOUNDATIONDB.EMULATION.TUPLE_ROUTE",
     "FoundationDB tuple decoding routes through trusted parser support.", true, false},
    {"TENANT_CREATE", PatternMatch::kPrefix, "tenant", "foundationdb.tenant.create",
     MappingDisposition::kParserSupportUdr, "foundationdb.udr.tenant.create",
     "SBLR_COMPAT_FOUNDATIONDB_TENANT_ROUTE", "ParserSupportTenantRoute", "FOUNDATIONDB.EMULATION.TENANT_ROUTE",
     "FoundationDB tenant management routes through trusted parser support and catalog policy.", true, true},
    {"TENANT_DELETE", PatternMatch::kPrefix, "tenant", "foundationdb.tenant.delete",
     MappingDisposition::kParserSupportUdr, "foundationdb.udr.tenant.delete",
     "SBLR_COMPAT_FOUNDATIONDB_TENANT_ROUTE", "ParserSupportTenantRoute", "FOUNDATIONDB.EMULATION.TENANT_ROUTE",
     "FoundationDB tenant management routes through trusted parser support and catalog policy.", true, true},
    {"STATUS", PatternMatch::kPrefix, "catalog_overlay", "foundationdb.catalog.status",
     MappingDisposition::kCatalogProjection, "foundationdb.catalog.status",
     "SBLR_COMPAT_FOUNDATIONDB_CATALOG_PROJECT", "EngineCatalogProjection", "",
     "", false, false},
    {"GET_READ_VERSION", PatternMatch::kPrefix, "transaction", "foundationdb.transaction.get_read_version",
     MappingDisposition::kCatalogProjection, "foundationdb.transaction.get_read_version",
     "SBLR_COMPAT_FOUNDATIONDB_CATALOG_PROJECT", "EngineCatalogProjection", "",
     "", false, false},
    {"COMMIT", PatternMatch::kPrefix, "transaction", "foundationdb.transaction.commit",
     MappingDisposition::kAdmittedSblr, "foundationdb.transaction.commit",
     "SBLR_TRANSACTION_COMMIT", "EngineCommitTransaction", "",
     "", false, true},
    {"RESET", PatternMatch::kPrefix, "transaction", "foundationdb.transaction.reset",
     MappingDisposition::kAdmittedSblr, "foundationdb.transaction.rollback",
     "SBLR_TRANSACTION_ROLLBACK", "EngineRollbackTransaction", "",
     "", false, true},
    {"CANCEL", PatternMatch::kPrefix, "transaction", "foundationdb.transaction.cancel",
     MappingDisposition::kAdmittedSblr, "foundationdb.transaction.cancel",
     "SBLR_TRANSACTION_ROLLBACK", "EngineRollbackTransaction", "",
     "", false, true},
};

const std::array<SurfaceDescriptor, 8> kDatatypeSurfaces{{
    {"key", "KEY;BYTE_STRING;SUBSPACE_KEY", "descriptor"},
    {"value", "VALUE;BYTE_STRING", "descriptor"},
    {"tuple", "TUPLE;VERSIONSTAMP;NESTED_TUPLE", "parser_support_udr"},
    {"range", "KEY_RANGE;KEY_SELECTOR;PREFIX_RANGE", "descriptor_policy"},
    {"version", "READ_VERSION;COMMIT_VERSION", "catalog_policy"},
    {"tenant", "TENANT;TENANT_GROUP", "catalog_policy"},
    {"directory", "DIRECTORY;DIRECTORY_LAYER;SUBSPACE", "parser_support_udr"},
    {"watch", "WATCH;FUTURE", "parser_support_udr"},
}};

const std::array<SurfaceDescriptor, 9> kBuiltinSurfaces{{
    {"kv_read", "GET;GET_KEY;GET_RANGE;GET_RANGE_SELECTOR", "sblr"},
    {"kv_write", "SET;CLEAR;CLEAR_RANGE;ATOMIC_OP", "sblr"},
    {"versionstamp", "SET_VERSIONSTAMPED_KEY;SET_VERSIONSTAMPED_VALUE", "parser_support_udr"},
    {"tuple", "TUPLE_PACK;TUPLE_UNPACK;TUPLE_RANGE;TUPLE_SORT", "parser_support_udr"},
    {"directory", "DIRECTORY_CREATE;DIRECTORY_OPEN;DIRECTORY_REMOVE", "parser_support_udr"},
    {"tenant", "TENANT_CREATE;TENANT_DELETE;TENANT_SET_ACTIVE", "parser_support_udr"},
    {"watch", "WATCH;ON_ERROR", "parser_support_udr"},
    {"transaction", "COMMIT;RESET;CANCEL;GET_READ_VERSION", "sblr"},
    {"cluster", "CONFIGURE;COORDINATORS;EXCLUDE;INCLUDE;KILL;FORCE_RECOVERY", "fail_closed"},
}};

const std::array<SurfaceDescriptor, 9> kCatalogSurfaces{{
    {"status", "FOUNDATIONDB_STATUS", "catalog_projection"},
    {"directories", "FOUNDATIONDB_DIRECTORIES", "catalog_projection"},
    {"subspaces", "FOUNDATIONDB_SUBSPACES", "catalog_projection"},
    {"tenants", "FOUNDATIONDB_TENANTS", "catalog_projection"},
    {"watches", "FOUNDATIONDB_WATCHES", "catalog_projection"},
    {"versions", "FOUNDATIONDB_VERSIONS", "catalog_projection"},
    {"conflicts", "FOUNDATIONDB_CONFLICT_RANGES", "catalog_projection"},
    {"metrics", "FOUNDATIONDB_METRICS", "catalog_projection"},
    {"cluster", "FOUNDATIONDB_CLUSTER_STATUS", "fail_closed"},
}};

const std::array<SurfaceDescriptor, 10> kDiagnosticSurfaces{{
    {"parse", "FOUNDATIONDB.PARSE.INVALID_INPUT;FOUNDATIONDB.PARSE.UNSUPPORTED_SURFACE", "parser"},
    {"policy", "FOUNDATIONDB.AUTHORITY.*", "fail_closed"},
    {"udr", "FOUNDATIONDB.EMULATION.*", "parser_support_udr"},
    {"catalog", "FOUNDATIONDB.CATALOG_OVERLAY.READ_ONLY", "catalog_projection"},
    {"session", "FOUNDATIONDB.SESSION.*", "sblr"},
    {"transaction", "FOUNDATIONDB.TRANSACTION.*", "sblr"},
    {"file_effects", "real_reference_file_effects=false", "authority_invariant"},
    {"reference_execution", "reference_engine_sql_executed=false", "authority_invariant"},
    {"mga", "parser_transaction_finality_authority=false", "authority_invariant"},
    {"support_bundle", "source_text_redacted=true", "diagnostic_redaction"},
}};

const scratchbird::parser::compatibility::DialectProfile kProfile{
    "foundationdb",
    "FoundationDB",
    "sbp_foundationdb",
    "sbup_foundationdb",
    "7.3.77",
    "FOUNDATIONDB",
    kSblrFamily,
    kPatterns,
    kDatatypeSurfaces,
    kBuiltinSurfaces,
    kCatalogSurfaces,
    kDiagnosticSurfaces,
    29,
    88,
    58,
    0,
    9,
    0,
    6,
    9,
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
std::string FoundationdbPackageIdentityJson() { return scratchbird::parser::compatibility::PackageIdentityJson(kProfile); }
std::string FoundationdbSurfaceReportJson() { return scratchbird::parser::compatibility::SurfaceReportJson(kProfile); }

} // namespace scratchbird::parser::foundationdb
