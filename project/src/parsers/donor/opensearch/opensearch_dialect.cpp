// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "opensearch_dialect.hpp"

#include <array>

namespace scratchbird::parser::opensearch {
namespace {

using scratchbird::parser::donor::MappingDisposition;
using scratchbird::parser::donor::OperationPattern;
using scratchbird::parser::donor::PatternMatch;
using scratchbird::parser::donor::SurfaceDescriptor;

constexpr std::string_view kSblrFamily = "sblr.donor.opensearch.profile.v1";

constexpr OperationPattern kPatterns[] = {
    {"GET /_CLUSTER", PatternMatch::kPrefix, "admin", "opensearch.admin.cluster_health",
     MappingDisposition::kAdmittedSblr, "cluster.admin.inspect_status",
     "sblr.cluster.report.v3:cluster.admin.inspect_status", "cluster.inspect_state", "",
     "", true, false},
    {"POST /_CLUSTER", PatternMatch::kPrefix, "admin", "opensearch.admin.cluster_update",
     MappingDisposition::kAdmittedSblr, "cluster.job.start_controlled",
     "sblr.cluster.control.v3:cluster.job.start_controlled", "cluster.start_job", "",
     "", true, false},
    {"GET /_NODES", PatternMatch::kPrefix, "admin", "opensearch.admin.nodes",
     MappingDisposition::kAdmittedSblr, "cluster.node.admit_member",
     "sblr.cluster.control.v3:cluster.node.admit_member", "cluster.admit_member", "",
     "", true, false},
    {"_SEARCH", PatternMatch::kRestPathSegment, "query", "opensearch.search.query",
     MappingDisposition::kAdmittedSblr, "opensearch.search.query",
     "SBLR_DONOR_OPENSEARCH_SEARCH_ROUTE", "EngineSearchRoute", "",
     "", false, false},
    {"_MSEARCH", PatternMatch::kRestPathSegment, "query", "opensearch.search.multi",
     MappingDisposition::kParserSupportUdr, "opensearch.udr.msearch",
     "SBLR_DONOR_OPENSEARCH_MULTI_SEARCH_ROUTE", "ParserSupportMultiSearchRoute", "OPENSEARCH.EMULATION.MULTI_SEARCH_ROUTE",
     "OpenSearch msearch framing routes through trusted parser support.", true, false},
    {"_BULK", PatternMatch::kRestPathSegment, "bulk", "opensearch.bulk.write",
     MappingDisposition::kParserSupportUdr, "opensearch.udr.bulk",
     "SBLR_DONOR_OPENSEARCH_BULK_ROUTE", "ParserSupportBulkRoute", "OPENSEARCH.EMULATION.BULK_ROUTE",
     "OpenSearch bulk write framing routes through trusted parser support.", true, true},
    {"_MGET", PatternMatch::kRestPathSegment, "document", "opensearch.document.multi_get",
     MappingDisposition::kAdmittedSblr, "opensearch.document.multi_get",
     "SBLR_DONOR_OPENSEARCH_MULTI_GET_ROUTE", "EngineDocumentMultiGet", "",
     "", false, false},
    {"_MAPPING", PatternMatch::kRestPathSegment, "catalog_overlay", "opensearch.catalog.mapping",
     MappingDisposition::kCatalogProjection, "opensearch.catalog.mapping",
     "SBLR_DONOR_OPENSEARCH_CATALOG_PROJECT", "EngineCatalogProjection", "",
     "", false, false},
    {"_ALIASES", PatternMatch::kRestPathSegment, "catalog_overlay", "opensearch.catalog.aliases",
     MappingDisposition::kParserSupportUdr, "opensearch.udr.aliases",
     "SBLR_DONOR_OPENSEARCH_ALIAS_ROUTE", "ParserSupportAliasRoute", "OPENSEARCH.EMULATION.ALIAS_ROUTE",
     "OpenSearch alias effects route through trusted catalog policy.", true, true},
    {"_INGEST/PIPELINE", PatternMatch::kRestPathSegment, "pipeline", "opensearch.ingest.pipeline",
     MappingDisposition::kParserSupportUdr, "opensearch.udr.pipeline",
     "SBLR_DONOR_OPENSEARCH_PIPELINE_ROUTE", "ParserSupportPipelineRoute", "OPENSEARCH.EMULATION.PIPELINE_ROUTE",
     "OpenSearch ingest pipelines route through trusted package support.", true, true},
    {"_SECURITY", PatternMatch::kRestPathSegment, "security", "opensearch.security.route",
     MappingDisposition::kParserSupportUdr, "opensearch.udr.security",
     "SBLR_DONOR_OPENSEARCH_SECURITY_ROUTE", "ParserSupportSecurityRoute", "OPENSEARCH.EMULATION.SECURITY_ROUTE",
     "OpenSearch security operations route through trusted security policy.", true, true},
    {"_CAT", PatternMatch::kRestPathSegment, "catalog_overlay", "opensearch.catalog.cat",
     MappingDisposition::kCatalogProjection, "opensearch.catalog.cat",
     "SBLR_DONOR_OPENSEARCH_CATALOG_PROJECT", "EngineCatalogProjection", "",
     "", false, false},
    {"PUT", PatternMatch::kPrefix, "index", "opensearch.index.create",
     MappingDisposition::kAdmittedSblr, "opensearch.index.create",
     "SBLR_DONOR_OPENSEARCH_INDEX_ROUTE", "EngineIndexLifecycleRoute", "",
     "", true, true},
    {"POST", PatternMatch::kPrefix, "document", "opensearch.document.write",
     MappingDisposition::kAdmittedSblr, "opensearch.document.write",
     "SBLR_DONOR_OPENSEARCH_DOCUMENT_ROUTE", "EngineDocumentWrite", "",
     "", false, true},
    {"DELETE", PatternMatch::kPrefix, "document", "opensearch.document.delete",
     MappingDisposition::kAdmittedSblr, "opensearch.document.delete",
     "SBLR_DONOR_OPENSEARCH_DELETE_ROUTE", "EngineDocumentDelete", "",
     "", false, true},
};

const std::array<SurfaceDescriptor, 8> kDatatypeSurfaces{{
    {"scalar", "BOOLEAN;BYTE;SHORT;INTEGER;LONG;FLOAT;DOUBLE;KEYWORD;TEXT", "descriptor"},
    {"binary", "BINARY", "descriptor"},
    {"temporal", "DATE;DATE_NANOS", "descriptor"},
    {"geo", "GEO_POINT;GEO_SHAPE", "parser_support_udr"},
    {"range", "INTEGER_RANGE;LONG_RANGE;DATE_RANGE", "parser_support_udr"},
    {"object", "OBJECT;NESTED;FLATTENED", "parser_support_udr"},
    {"rank", "RANK_FEATURE;RANK_FEATURES", "parser_support_udr"},
    {"vector", "KNN_VECTOR;VECTOR", "parser_support_udr"},
}};

const std::array<SurfaceDescriptor, 8> kBuiltinSurfaces{{
    {"query", "MATCH;TERM;RANGE;BOOL;FILTER", "sblr"},
    {"aggregation", "TERMS;AVG;SUM;MIN;MAX;CARDINALITY", "sblr"},
    {"script", "PAINLESS;SCRIPT_SCORE", "parser_support_udr"},
    {"geo", "GEO_DISTANCE;GEO_BOUNDING_BOX", "parser_support_udr"},
    {"highlight", "HIGHLIGHT;INNER_HITS", "parser_support_udr"},
    {"sort", "SORT;SEARCH_AFTER;POINT_IN_TIME", "sblr"},
    {"analyzer", "ANALYZER;TOKENIZER;NORMALIZER", "parser_support_udr"},
    {"security", "USER;ROLE;PERMISSION", "parser_support_udr"},
}};

const std::array<SurfaceDescriptor, 10> kCatalogSurfaces{{
    {"indices", "OPENSEARCH_INDICES", "catalog_projection"},
    {"mappings", "OPENSEARCH_MAPPINGS", "catalog_projection"},
    {"aliases", "OPENSEARCH_ALIASES", "catalog_projection"},
    {"templates", "OPENSEARCH_TEMPLATES", "catalog_projection"},
    {"ingest", "OPENSEARCH_PIPELINES", "catalog_projection"},
    {"security", "OPENSEARCH_SECURITY", "catalog_projection"},
    {"cat", "OPENSEARCH_CAT", "catalog_projection"},
    {"tasks", "OPENSEARCH_TASKS", "catalog_projection"},
    {"snapshots", "OPENSEARCH_SNAPSHOTS", "catalog_projection"},
    {"plugins", "OPENSEARCH_PLUGINS", "catalog_projection"},
}};

const std::array<SurfaceDescriptor, 10> kDiagnosticSurfaces{{
    {"parse", "OPENSEARCH.PARSE.INVALID_INPUT;OPENSEARCH.PARSE.UNSUPPORTED_SURFACE", "parser"},
    {"policy", "OPENSEARCH.AUTHORITY.*", "fail_closed"},
    {"udr", "OPENSEARCH.EMULATION.*", "parser_support_udr"},
    {"catalog", "OPENSEARCH.CATALOG_OVERLAY.READ_ONLY", "catalog_projection"},
    {"session", "OPENSEARCH.SESSION.*", "sblr"},
    {"transaction", "OPENSEARCH.TRANSACTION.*", "sblr"},
    {"file_effects", "real_donor_file_effects=false", "authority_invariant"},
    {"donor_execution", "donor_engine_sql_executed=false", "authority_invariant"},
    {"mga", "parser_transaction_finality_authority=false", "authority_invariant"},
    {"support_bundle", "source_text_redacted=true", "diagnostic_redaction"},
}};

const scratchbird::parser::donor::DialectProfile kProfile{
    "opensearch",
    "OpenSearch",
    "sbp_opensearch",
    "sbup_opensearch",
    "3.6.0",
    "OPENSEARCH",
    kSblrFamily,
    kPatterns,
    kDatatypeSurfaces,
    kBuiltinSurfaces,
    kCatalogSurfaces,
    kDiagnosticSurfaces,
    15,
    96,
    72,
    0,
    10,
    0,
    3,
    5,
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
std::string OpensearchPackageIdentityJson() { return scratchbird::parser::donor::PackageIdentityJson(kProfile); }
std::string OpensearchSurfaceReportJson() { return scratchbird::parser::donor::SurfaceReportJson(kProfile); }

} // namespace scratchbird::parser::opensearch
