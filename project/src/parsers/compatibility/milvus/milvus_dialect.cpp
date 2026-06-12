// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "milvus_dialect.hpp"

#include <array>

namespace scratchbird::parser::milvus {
namespace {

using scratchbird::parser::compatibility::MappingDisposition;
using scratchbird::parser::compatibility::OperationPattern;
using scratchbird::parser::compatibility::PatternMatch;
using scratchbird::parser::compatibility::SurfaceDescriptor;

constexpr std::string_view kSblrFamily = "sblr.compatibility.milvus.profile.v1";

constexpr OperationPattern kPatterns[] = {
    {"TRANSFER_REPLICA", PatternMatch::kPrefix, "admin", "milvus.admin.transfer_replica",
     MappingDisposition::kAdmittedSblr, "cluster.tx.begin_distributed",
     "required_new:sblr.cluster.transaction.v1:cluster.tx.begin_distributed", "cluster.begin_distributed_transaction", "",
     "", true, false},
    {"LOAD_BALANCE", PatternMatch::kPrefix, "admin", "milvus.admin.load_balance",
     MappingDisposition::kAdmittedSblr, "cluster.job.start_controlled",
     "sblr.cluster.control.v3:cluster.job.start_controlled", "cluster.start_job", "",
     "", true, false},
    {"SHOW_REPLICAS", PatternMatch::kPrefix, "admin", "milvus.admin.show_replicas",
     MappingDisposition::kAdmittedSblr, "cluster.tx.begin_distributed",
     "required_new:sblr.cluster.transaction.v1:cluster.tx.begin_distributed", "cluster.begin_distributed_transaction", "",
     "", true, false},
    {"CREATE COLLECTION", PatternMatch::kPrefix, "collection", "milvus.collection.create",
     MappingDisposition::kAdmittedSblr, "milvus.collection.create",
     "SBLR_COMPAT_MILVUS_COLLECTION_ROUTE", "EngineVectorCollectionRoute", "",
     "", true, true},
    {"DROP COLLECTION", PatternMatch::kPrefix, "collection", "milvus.collection.drop",
     MappingDisposition::kAdmittedSblr, "milvus.collection.drop",
     "SBLR_COMPAT_MILVUS_COLLECTION_ROUTE", "EngineVectorCollectionRoute", "",
     "", true, true},
    {"HAS COLLECTION", PatternMatch::kPrefix, "collection", "milvus.collection.has",
     MappingDisposition::kCatalogProjection, "milvus.collection.has",
     "SBLR_COMPAT_MILVUS_CATALOG_PROJECT", "EngineCatalogProjection", "",
     "", false, false},
    {"DESCRIBE COLLECTION", PatternMatch::kPrefix, "collection", "milvus.collection.describe",
     MappingDisposition::kCatalogProjection, "milvus.collection.describe",
     "SBLR_COMPAT_MILVUS_CATALOG_PROJECT", "EngineCatalogProjection", "",
     "", false, false},
    {"CREATE PARTITION", PatternMatch::kPrefix, "partition", "milvus.partition.create",
     MappingDisposition::kAdmittedSblr, "milvus.partition.create",
     "SBLR_COMPAT_MILVUS_PARTITION_ROUTE", "EngineVectorPartitionRoute", "",
     "", true, true},
    {"DROP PARTITION", PatternMatch::kPrefix, "partition", "milvus.partition.drop",
     MappingDisposition::kAdmittedSblr, "milvus.partition.drop",
     "SBLR_COMPAT_MILVUS_PARTITION_ROUTE", "EngineVectorPartitionRoute", "",
     "", true, true},
    {"CREATE INDEX", PatternMatch::kPrefix, "index", "milvus.index.create",
     MappingDisposition::kParserSupportUdr, "milvus.udr.index.create",
     "SBLR_COMPAT_MILVUS_INDEX_ROUTE", "ParserSupportVectorIndexRoute", "MILVUS.EMULATION.INDEX_ROUTE",
     "Milvus vector indexes route through trusted parser support.", true, true},
    {"DROP INDEX", PatternMatch::kPrefix, "index", "milvus.index.drop",
     MappingDisposition::kParserSupportUdr, "milvus.udr.index.drop",
     "SBLR_COMPAT_MILVUS_INDEX_ROUTE", "ParserSupportVectorIndexRoute", "MILVUS.EMULATION.INDEX_ROUTE",
     "Milvus vector indexes route through trusted parser support.", true, true},
    {"LOAD COLLECTION", PatternMatch::kPrefix, "load_state", "milvus.collection.load",
     MappingDisposition::kParserSupportUdr, "milvus.udr.collection.load",
     "SBLR_COMPAT_MILVUS_LOAD_ROUTE", "ParserSupportLoadRoute", "MILVUS.EMULATION.LOAD_ROUTE",
     "Milvus load-state transitions route through trusted resource policy.", true, true},
    {"RELEASE COLLECTION", PatternMatch::kPrefix, "load_state", "milvus.collection.release",
     MappingDisposition::kParserSupportUdr, "milvus.udr.collection.release",
     "SBLR_COMPAT_MILVUS_LOAD_ROUTE", "ParserSupportLoadRoute", "MILVUS.EMULATION.LOAD_ROUTE",
     "Milvus release-state transitions route through trusted resource policy.", true, true},
    {"INSERT", PatternMatch::kPrefix, "dml", "milvus.dml.insert",
     MappingDisposition::kAdmittedSblr, "milvus.dml.insert",
     "SBLR_COMPAT_MILVUS_INSERT", "EngineVectorInsert", "",
     "", false, true},
    {"UPSERT", PatternMatch::kPrefix, "dml", "milvus.dml.upsert",
     MappingDisposition::kAdmittedSblr, "milvus.dml.upsert",
     "SBLR_COMPAT_MILVUS_UPSERT", "EngineVectorUpsert", "",
     "", false, true},
    {"DELETE", PatternMatch::kPrefix, "dml", "milvus.dml.delete",
     MappingDisposition::kAdmittedSblr, "milvus.dml.delete",
     "SBLR_COMPAT_MILVUS_DELETE", "EngineVectorDelete", "",
     "", false, true},
    {"HYBRID_SEARCH", PatternMatch::kPrefix, "query", "milvus.query.hybrid_search",
     MappingDisposition::kParserSupportUdr, "milvus.udr.hybrid_search",
     "SBLR_COMPAT_MILVUS_HYBRID_SEARCH_ROUTE", "ParserSupportHybridSearchRoute", "MILVUS.EMULATION.HYBRID_SEARCH_ROUTE",
     "Milvus hybrid search route binds vector and scalar evidence through trusted policy.", true, false},
    {"SEARCH", PatternMatch::kPrefix, "query", "milvus.query.search",
     MappingDisposition::kAdmittedSblr, "milvus.query.search",
     "SBLR_COMPAT_MILVUS_SEARCH", "EngineVectorSearch", "",
     "", false, false},
    {"QUERY", PatternMatch::kPrefix, "query", "milvus.query.scalar",
     MappingDisposition::kAdmittedSblr, "milvus.query.scalar",
     "SBLR_COMPAT_MILVUS_QUERY", "EngineVectorScalarQuery", "",
     "", false, false},
    {"RERANK", PatternMatch::kPrefix, "query", "milvus.query.rerank",
     MappingDisposition::kParserSupportUdr, "milvus.udr.rerank",
     "SBLR_COMPAT_MILVUS_RERANK_ROUTE", "ParserSupportRerankRoute", "MILVUS.EMULATION.RERANK_ROUTE",
     "Milvus rerank operations route through trusted parser support.", true, false},
    {"CREATE USER", PatternMatch::kPrefix, "security", "milvus.security.create_user",
     MappingDisposition::kParserSupportUdr, "milvus.udr.security.create_user",
     "SBLR_COMPAT_MILVUS_SECURITY_ROUTE", "ParserSupportSecurityRoute", "MILVUS.EMULATION.SECURITY_ROUTE",
     "Milvus security operations route through trusted security policy.", true, true},
    {"CREATE ROLE", PatternMatch::kPrefix, "security", "milvus.security.create_role",
     MappingDisposition::kParserSupportUdr, "milvus.udr.security.create_role",
     "SBLR_COMPAT_MILVUS_SECURITY_ROUTE", "ParserSupportSecurityRoute", "MILVUS.EMULATION.SECURITY_ROUTE",
     "Milvus security operations route through trusted security policy.", true, true},
    {"GRANT", PatternMatch::kPrefix, "security", "milvus.security.grant",
     MappingDisposition::kParserSupportUdr, "milvus.udr.security.grant",
     "SBLR_COMPAT_MILVUS_SECURITY_ROUTE", "ParserSupportSecurityRoute", "MILVUS.EMULATION.SECURITY_ROUTE",
     "Milvus privilege changes route through trusted security policy.", true, true},
    {"REVOKE", PatternMatch::kPrefix, "security", "milvus.security.revoke",
     MappingDisposition::kParserSupportUdr, "milvus.udr.security.revoke",
     "SBLR_COMPAT_MILVUS_SECURITY_ROUTE", "ParserSupportSecurityRoute", "MILVUS.EMULATION.SECURITY_ROUTE",
     "Milvus privilege changes route through trusted security policy.", true, true},
};

const std::array<SurfaceDescriptor, 6> kDatatypeSurfaces{{
    {"scalar", "BOOL;INT8;INT16;INT32;INT64;FLOAT;DOUBLE;VARCHAR", "descriptor"},
    {"json", "JSON", "parser_support_udr"},
    {"array", "ARRAY", "parser_support_udr"},
    {"vector", "FLOAT_VECTOR;BINARY_VECTOR;FLOAT16_VECTOR;BFLOAT16_VECTOR;SPARSE_FLOAT_VECTOR", "parser_support_udr"},
    {"geometry", "GEOMETRY", "parser_support_udr"},
    {"dynamic", "DYNAMIC_FIELD", "parser_support_udr"},
}};

const std::array<SurfaceDescriptor, 6> kBuiltinSurfaces{{
    {"metric", "L2;IP;COSINE;HAMMING;JACCARD", "parser_support_udr"},
    {"index", "IVF_FLAT;HNSW;DISKANN;SCANN;AUTOINDEX", "parser_support_udr"},
    {"filter", "LIKE;JSON_CONTAINS;ARRAY_CONTAINS", "sblr"},
    {"search", "SEARCH;HYBRID_SEARCH;RERANK", "sblr"},
    {"load", "LOAD_COLLECTION;RELEASE_COLLECTION", "parser_support_udr"},
    {"security", "USER;ROLE;PRIVILEGE", "parser_support_udr"},
}};

const std::array<SurfaceDescriptor, 9> kCatalogSurfaces{{
    {"collections", "MILVUS_COLLECTIONS", "catalog_projection"},
    {"partitions", "MILVUS_PARTITIONS", "catalog_projection"},
    {"indexes", "MILVUS_INDEXES", "catalog_projection"},
    {"segments", "MILVUS_SEGMENTS", "catalog_projection"},
    {"load_states", "MILVUS_LOAD_STATES", "catalog_projection"},
    {"users", "MILVUS_USERS", "catalog_projection"},
    {"roles", "MILVUS_ROLES", "catalog_projection"},
    {"privileges", "MILVUS_PRIVILEGES", "catalog_projection"},
    {"aliases", "MILVUS_ALIASES", "catalog_projection"},
}};

const std::array<SurfaceDescriptor, 10> kDiagnosticSurfaces{{
    {"parse", "MILVUS.PARSE.INVALID_INPUT;MILVUS.PARSE.UNSUPPORTED_SURFACE", "parser"},
    {"policy", "MILVUS.AUTHORITY.*", "fail_closed"},
    {"udr", "MILVUS.EMULATION.*", "parser_support_udr"},
    {"catalog", "MILVUS.CATALOG_OVERLAY.READ_ONLY", "catalog_projection"},
    {"session", "MILVUS.SESSION.*", "sblr"},
    {"transaction", "MILVUS.TRANSACTION.*", "sblr"},
    {"file_effects", "real_reference_file_effects=false", "authority_invariant"},
    {"reference_execution", "reference_engine_sql_executed=false", "authority_invariant"},
    {"mga", "parser_transaction_finality_authority=false", "authority_invariant"},
    {"support_bundle", "source_text_redacted=true", "diagnostic_redaction"},
}};

const scratchbird::parser::compatibility::DialectProfile kProfile{
    "milvus",
    "Milvus",
    "sbp_milvus",
    "sbup_milvus",
    "2.6.5",
    "MILVUS",
    kSblrFamily,
    kPatterns,
    kDatatypeSurfaces,
    kBuiltinSurfaces,
    kCatalogSurfaces,
    kDiagnosticSurfaces,
    24,
    72,
    54,
    0,
    9,
    0,
    3,
    10,
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
std::string MilvusPackageIdentityJson() { return scratchbird::parser::compatibility::PackageIdentityJson(kProfile); }
std::string MilvusSurfaceReportJson() { return scratchbird::parser::compatibility::SurfaceReportJson(kProfile); }

} // namespace scratchbird::parser::milvus
