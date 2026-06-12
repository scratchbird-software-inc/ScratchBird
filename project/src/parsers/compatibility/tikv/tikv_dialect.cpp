// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "tikv_dialect.hpp"

#include <array>

namespace scratchbird::parser::tikv {
namespace {

using scratchbird::parser::compatibility::MappingDisposition;
using scratchbird::parser::compatibility::OperationPattern;
using scratchbird::parser::compatibility::PatternMatch;
using scratchbird::parser::compatibility::SurfaceDescriptor;

constexpr std::string_view kSblrFamily = "sblr.compatibility.tikv.profile.v1";

constexpr OperationPattern kPatterns[] = {
    {"SPLIT_REGION", PatternMatch::kPrefix, "admin", "tikv.admin.split_region",
     MappingDisposition::kAdmittedSblr, "cluster.placement.place_object",
     "sblr.cluster.control.v3:cluster.placement.place_object", "cluster.place_object", "",
     "", true, false},
    {"MERGE_REGION", PatternMatch::kPrefix, "admin", "tikv.admin.merge_region",
     MappingDisposition::kAdmittedSblr, "cluster.reconcile.apply_merge_policy",
     "required_new:sblr.cluster.reconciliation.v1:cluster.reconcile.apply_merge_policy", "cluster.apply_merge_policy", "",
     "", true, false},
    {"TRANSFER_LEADER", PatternMatch::kPrefix, "admin", "tikv.admin.transfer_leader",
     MappingDisposition::kAdmittedSblr, "cluster.node.set_role",
     "sblr.cluster.control.v3:cluster.node.set_role", "cluster.set_node_role", "",
     "", true, false},
    {"CHANGE_PEER", PatternMatch::kPrefix, "admin", "tikv.admin.change_peer",
     MappingDisposition::kAdmittedSblr, "cluster.job.start_controlled",
     "sblr.cluster.control.v3:cluster.job.start_controlled", "cluster.start_job", "",
     "", true, false},
    {"PD_", PatternMatch::kPrefix, "admin", "tikv.admin.pd_request",
     MappingDisposition::kAdmittedSblr, "cluster.job.start_controlled",
     "sblr.cluster.control.v3:cluster.job.start_controlled", "cluster.start_job", "",
     "", true, false},
    {"IMPORT_SST", PatternMatch::kPrefix, "bulk_io", "tikv.import_sst",
     MappingDisposition::kParserSupportUdr, "tikv.udr.import_sst",
     "SBLR_COMPAT_TIKV_IMPORT_SST_ROUTE", "ParserSupportImportSstRoute", "TIKV.EMULATION.IMPORT_SST_ROUTE",
     "TiKV SST import routing requires trusted parser support and engine authorization.", true, true},
    {"RAW_BATCH_GET", PatternMatch::kPrefix, "raw_kv", "tikv.raw.batch_get",
     MappingDisposition::kAdmittedSblr, "tikv.raw.batch_get",
     "SBLR_COMPAT_TIKV_RAW_ROUTE", "EngineRawKvBatchGet", "",
     "", false, false},
    {"RAW_GET", PatternMatch::kPrefix, "raw_kv", "tikv.raw.get",
     MappingDisposition::kAdmittedSblr, "tikv.raw.get",
     "SBLR_COMPAT_TIKV_RAW_GET", "EngineRawKvGet", "",
     "", false, false},
    {"RAW_PUT", PatternMatch::kPrefix, "raw_kv", "tikv.raw.put",
     MappingDisposition::kAdmittedSblr, "tikv.raw.put",
     "SBLR_COMPAT_TIKV_RAW_PUT", "EngineRawKvPut", "",
     "", false, true},
    {"RAW_DELETE", PatternMatch::kPrefix, "raw_kv", "tikv.raw.delete",
     MappingDisposition::kAdmittedSblr, "tikv.raw.delete",
     "SBLR_COMPAT_TIKV_RAW_DELETE", "EngineRawKvDelete", "",
     "", false, true},
    {"RAW_SCAN", PatternMatch::kPrefix, "raw_kv", "tikv.raw.scan",
     MappingDisposition::kAdmittedSblr, "tikv.raw.scan",
     "SBLR_COMPAT_TIKV_RAW_SCAN", "EngineRawKvScan", "",
     "", false, false},
    {"TXN_PREWRITE", PatternMatch::kPrefix, "txn_kv", "tikv.txn.prewrite",
     MappingDisposition::kParserSupportUdr, "tikv.udr.txn.prewrite",
     "SBLR_COMPAT_TIKV_TXN_PREWRITE_ROUTE", "ParserSupportTxnPrewriteRoute", "TIKV.EMULATION.TXN_ROUTE",
     "TiKV prewrite semantics are mapped through trusted parser support; ScratchBird MGA owns finality.", true, true},
    {"TXN_COMMIT", PatternMatch::kPrefix, "txn_kv", "tikv.txn.commit",
     MappingDisposition::kAdmittedSblr, "tikv.txn.commit",
     "SBLR_TRANSACTION_COMMIT", "EngineCommitTransaction", "",
     "", false, true},
    {"TXN_ROLLBACK", PatternMatch::kPrefix, "txn_kv", "tikv.txn.rollback",
     MappingDisposition::kAdmittedSblr, "tikv.txn.rollback",
     "SBLR_TRANSACTION_ROLLBACK", "EngineRollbackTransaction", "",
     "", false, true},
    {"TXN_GET", PatternMatch::kPrefix, "txn_kv", "tikv.txn.get",
     MappingDisposition::kAdmittedSblr, "tikv.txn.get",
     "SBLR_COMPAT_TIKV_TXN_GET", "EngineTxnKvGet", "",
     "", false, false},
    {"TXN_SCAN", PatternMatch::kPrefix, "txn_kv", "tikv.txn.scan",
     MappingDisposition::kAdmittedSblr, "tikv.txn.scan",
     "SBLR_COMPAT_TIKV_TXN_SCAN", "EngineTxnKvScan", "",
     "", false, false},
    {"COPROCESSOR", PatternMatch::kPrefix, "coprocessor", "tikv.coprocessor.request",
     MappingDisposition::kParserSupportUdr, "tikv.udr.coprocessor",
     "SBLR_COMPAT_TIKV_COPROCESSOR_ROUTE", "ParserSupportCoprocessorRoute", "TIKV.EMULATION.COPROCESSOR_ROUTE",
     "TiKV coprocessor requests route through trusted parser support and optimizer policy.", true, false},
    {"REGION_INFO", PatternMatch::kPrefix, "catalog_overlay", "tikv.catalog.region_info",
     MappingDisposition::kCatalogProjection, "tikv.catalog.region_info",
     "SBLR_COMPAT_TIKV_CATALOG_PROJECT", "EngineCatalogProjection", "",
     "", false, false},
    {"STORE_INFO", PatternMatch::kPrefix, "catalog_overlay", "tikv.catalog.store_info",
     MappingDisposition::kCatalogProjection, "tikv.catalog.store_info",
     "SBLR_COMPAT_TIKV_CATALOG_PROJECT", "EngineCatalogProjection", "",
     "", false, false},
    {"MVCC_INFO", PatternMatch::kPrefix, "catalog_overlay", "tikv.catalog.mvcc_info",
     MappingDisposition::kCatalogProjection, "tikv.catalog.mvcc_info",
     "SBLR_COMPAT_TIKV_CATALOG_PROJECT", "EngineCatalogProjection", "",
     "", false, false},
    {"LOCK_INFO", PatternMatch::kPrefix, "catalog_overlay", "tikv.catalog.lock_info",
     MappingDisposition::kCatalogProjection, "tikv.catalog.lock_info",
     "SBLR_COMPAT_TIKV_CATALOG_PROJECT", "EngineCatalogProjection", "",
     "", false, false},
};

const std::array<SurfaceDescriptor, 8> kDatatypeSurfaces{{
    {"key", "BYTES;KEY;RAW_KEY", "descriptor"},
    {"value", "BYTES;VALUE;RAW_VALUE", "descriptor"},
    {"timestamp", "START_TS;COMMIT_TS;TSO", "descriptor_policy"},
    {"ttl", "TTL;EXPIRE_TS", "catalog_policy"},
    {"lock", "LOCK;PRIMARY_LOCK;LOCK_TTL", "catalog_policy"},
    {"region", "REGION_ID;PEER_ID;STORE_ID", "catalog_projection"},
    {"coprocessor", "DAG_REQUEST;COP_REQUEST", "parser_support_udr"},
    {"sst", "SST;INGEST;IMPORT_SST", "parser_support_udr"},
}};

const std::array<SurfaceDescriptor, 8> kBuiltinSurfaces{{
    {"raw_kv", "RAW_GET;RAW_PUT;RAW_DELETE;RAW_SCAN;RAW_BATCH_GET", "sblr"},
    {"txn_kv", "TXN_GET;TXN_PREWRITE;TXN_COMMIT;TXN_ROLLBACK;TXN_SCAN", "sblr"},
    {"coprocessor", "COPROCESSOR;DAG;SELECTION;TABLE_SCAN;INDEX_SCAN", "parser_support_udr"},
    {"import", "IMPORT_SST;INGEST_SST", "parser_support_udr"},
    {"catalog", "REGION_INFO;STORE_INFO;MVCC_INFO;LOCK_INFO", "catalog_projection"},
    {"cluster", "SPLIT_REGION;MERGE_REGION;TRANSFER_LEADER;CHANGE_PEER;PD_*", "fail_closed"},
    {"transaction", "COMMIT;ROLLBACK;PREWRITE", "sblr"},
    {"security", "CERT;TLS;AUTH_TOKEN", "parser_support_udr"},
}};

const std::array<SurfaceDescriptor, 8> kCatalogSurfaces{{
    {"regions", "TIKV_REGIONS", "catalog_projection"},
    {"stores", "TIKV_STORES", "catalog_projection"},
    {"locks", "TIKV_LOCKS", "catalog_projection"},
    {"mvcc", "TIKV_MVCC", "catalog_projection"},
    {"raw_cf", "TIKV_RAW_CF", "catalog_projection"},
    {"scheduler", "TIKV_SCHEDULER", "catalog_projection"},
    {"coprocessor", "TIKV_COPROCESSOR", "catalog_projection"},
    {"import_jobs", "TIKV_IMPORT_JOBS", "catalog_projection"},
}};

const std::array<SurfaceDescriptor, 10> kDiagnosticSurfaces{{
    {"parse", "TIKV.PARSE.INVALID_INPUT;TIKV.PARSE.UNSUPPORTED_SURFACE", "parser"},
    {"policy", "TIKV.AUTHORITY.*", "fail_closed"},
    {"udr", "TIKV.EMULATION.*", "parser_support_udr"},
    {"catalog", "TIKV.CATALOG_OVERLAY.READ_ONLY", "catalog_projection"},
    {"session", "TIKV.SESSION.*", "sblr"},
    {"transaction", "TIKV.TRANSACTION.*", "sblr"},
    {"file_effects", "real_reference_file_effects=false", "authority_invariant"},
    {"reference_execution", "reference_engine_sql_executed=false", "authority_invariant"},
    {"mga", "parser_transaction_finality_authority=false", "authority_invariant"},
    {"support_bundle", "source_text_redacted=true", "diagnostic_redaction"},
}};

const scratchbird::parser::compatibility::DialectProfile kProfile{
    "tikv",
    "TiKV",
    "sbp_tikv",
    "sbup_tikv",
    "8.5.6",
    "TIKV",
    kSblrFamily,
    kPatterns,
    kDatatypeSurfaces,
    kBuiltinSurfaces,
    kCatalogSurfaces,
    kDiagnosticSurfaces,
    21,
    74,
    52,
    0,
    8,
    0,
    5,
    4,
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
std::string TikvPackageIdentityJson() { return scratchbird::parser::compatibility::PackageIdentityJson(kProfile); }
std::string TikvSurfaceReportJson() { return scratchbird::parser::compatibility::SurfaceReportJson(kProfile); }

} // namespace scratchbird::parser::tikv
