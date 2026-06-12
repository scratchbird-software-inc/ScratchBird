// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "mongodb_dialect.hpp"

#include <array>

namespace scratchbird::parser::mongodb {
namespace {

using scratchbird::parser::compatibility::MappingDisposition;
using scratchbird::parser::compatibility::OperationPattern;
using scratchbird::parser::compatibility::PatternMatch;
using scratchbird::parser::compatibility::SurfaceDescriptor;

constexpr std::string_view kSblrFamily = "sblr.compatibility.mongodb.profile.v1";

constexpr OperationPattern kPatterns[] = {
    {"EVAL", PatternMatch::kPrefix, "script", "mongodb.script.eval",
     MappingDisposition::kParserSupportUdr, "mongodb.udr.script.eval",
     "SBLR_COMPAT_MONGODB_SCRIPT_ROUTE", "ParserSupportScriptRoute",
     "MONGODB.EMULATION.SCRIPT_ROUTE",
     "Server-side JavaScript routes through trusted script policy.", true, true},
    {"MAPREDUCE", PatternMatch::kPrefix, "script", "mongodb.script.map_reduce",
     MappingDisposition::kParserSupportUdr, "mongodb.udr.map_reduce",
     "SBLR_COMPAT_MONGODB_MAP_REDUCE_ROUTE", "ParserSupportMapReduceRoute",
     "MONGODB.EMULATION.MAP_REDUCE_ROUTE",
     "MapReduce routes through trusted document pipeline policy.", true, true},
    {"LOAD", PatternMatch::kPrefix, "shell_file", "mongodb.shell.load",
     MappingDisposition::kParserSupportUdr, "mongodb.udr.shell.load",
     "SBLR_COMPAT_MONGODB_CLIENT_FILE_ROUTE", "ParserSupportClientFileRoute",
     "MONGODB.EMULATION.CLIENT_FILE_ROUTE",
     "Shell load routes through trusted client-script policy.", true, false},
    {"WATCH", PatternMatch::kPrefix, "cdc", "mongodb.cdc.watch",
     MappingDisposition::kParserSupportUdr, "mongodb.udr.cdc.watch",
     "SBLR_COMPAT_MONGODB_CDC_ROUTE", "ParserSupportCdcRoute",
     "MONGODB.EMULATION.CDC_ROUTE",
     "MongoDB change-stream watch routes through the MongoDB compatibility UDR.", true, false},
    {"$CHANGESTREAM", PatternMatch::kContains, "cdc", "mongodb.cdc.change_stream",
     MappingDisposition::kParserSupportUdr, "mongodb.udr.cdc.change_stream",
     "SBLR_COMPAT_MONGODB_CDC_ROUTE", "ParserSupportCdcRoute",
     "MONGODB.EMULATION.CDC_ROUTE",
     "MongoDB $changeStream pipelines route through the MongoDB compatibility UDR.", true, false},
    {"SH.SHARD", PatternMatch::kContains, "sharding_admin", "mongodb.sharding.shard_collection",
     MappingDisposition::kAdmittedSblr, "cluster.placement.assign_tablet_range",
     "required_new:sblr.cluster.placement.v1:cluster.placement.assign_tablet_range", "cluster.assign_tablet_range", "",
     "", true, false},
    {"RESHARD", PatternMatch::kContains, "sharding_admin", "mongodb.sharding.reshard_collection",
     MappingDisposition::kAdmittedSblr, "cluster.placement.assign_tablet_range",
     "required_new:sblr.cluster.placement.v1:cluster.placement.assign_tablet_range", "cluster.assign_tablet_range", "",
     "", true, false},
    {"REPLSET", PatternMatch::kPrefix, "replica_admin", "mongodb.replica_set.command",
     MappingDisposition::kPolicyRefusal, "mongodb.policy.replica_set",
     "", "", "MONGODB.AUTHORITY.REPLICA_ADMIN_DENIED",
     "Replica-set administration requires trusted operational admission.", true, false},
    {"SERVERSTATUS", PatternMatch::kContains, "catalog_overlay", "mongodb.catalog.server_status",
     MappingDisposition::kCatalogProjection, "mongodb.catalog.server_status",
     "SBLR_COMPAT_MONGODB_CATALOG_PROJECT", "EngineCatalogProjection", "",
     "", false, false},
    {"CURRENTOP", PatternMatch::kContains, "catalog_overlay", "mongodb.catalog.current_op",
     MappingDisposition::kCatalogProjection, "mongodb.catalog.current_op",
     "SBLR_COMPAT_MONGODB_CATALOG_PROJECT", "EngineCatalogProjection", "",
     "", false, false},
    {"CREATEINDEXES", PatternMatch::kContains, "index", "mongodb.index.create",
     MappingDisposition::kParserSupportUdr, "mongodb.udr.index.create",
     "SBLR_COMPAT_MONGODB_INDEX_ROUTE", "ParserSupportIndexRoute", "MONGODB.EMULATION.INDEX_ROUTE",
     "Index options route through trusted catalog/index policy.", true, true},
    {"DROPINDEXES", PatternMatch::kContains, "index", "mongodb.index.drop",
     MappingDisposition::kParserSupportUdr, "mongodb.udr.index.drop",
     "SBLR_COMPAT_MONGODB_INDEX_ROUTE", "ParserSupportIndexRoute", "MONGODB.EMULATION.INDEX_ROUTE",
     "Index options route through trusted catalog/index policy.", true, true},
    {"$OUT", PatternMatch::kContains, "pipeline_write", "mongodb.aggregate.out",
     MappingDisposition::kParserSupportUdr, "mongodb.udr.aggregate.out",
     "SBLR_COMPAT_MONGODB_PIPELINE_WRITE_ROUTE", "ParserSupportPipelineWriteRoute", "MONGODB.EMULATION.PIPELINE_WRITE_ROUTE",
     "Aggregation write stages route through trusted write policy.", true, true},
    {"$MERGE", PatternMatch::kContains, "pipeline_write", "mongodb.aggregate.merge",
     MappingDisposition::kParserSupportUdr, "mongodb.udr.aggregate.merge",
     "SBLR_COMPAT_MONGODB_PIPELINE_WRITE_ROUTE", "ParserSupportPipelineWriteRoute", "MONGODB.EMULATION.PIPELINE_WRITE_ROUTE",
     "Aggregation write stages route through trusted write policy.", true, true},
    {"CREATECOLLECTION", PatternMatch::kContains, "ddl", "mongodb.ddl.create_collection",
     MappingDisposition::kAdmittedSblr, "mongodb.ddl.create_collection",
     "SBLR_COMPAT_MONGODB_CREATE_COLLECTION", "EngineDdlCreate", "",
     "", true, true},
    {"DROPDATABASE", PatternMatch::kContains, "database_lifecycle", "mongodb.lifecycle.drop_database",
     MappingDisposition::kScratchBirdLifecycleApi, "mongodb.lifecycle.drop_database",
     "SBLR_LIFECYCLE_DROP_DATABASE", "EngineDropLifecycle", "",
     "", true, true},
    {"CREATEUSER", PatternMatch::kContains, "security", "mongodb.security.create_user",
     MappingDisposition::kParserSupportUdr, "mongodb.udr.security.create_user",
     "SBLR_COMPAT_MONGODB_SECURITY_ROUTE", "ParserSupportSecurityRoute", "MONGODB.EMULATION.SECURITY_ROUTE",
     "User management routes through trusted security policy.", true, false},
    {"GRANTROLES", PatternMatch::kContains, "security", "mongodb.security.grant_roles",
     MappingDisposition::kParserSupportUdr, "mongodb.udr.security.grant_roles",
     "SBLR_COMPAT_MONGODB_SECURITY_ROUTE", "ParserSupportSecurityRoute", "MONGODB.EMULATION.SECURITY_ROUTE",
     "Role management routes through trusted security policy.", true, false},
    {"FINDANDMODIFY", PatternMatch::kContains, "dml", "mongodb.dml.find_and_modify",
     MappingDisposition::kAdmittedSblr, "mongodb.dml.find_and_modify",
     "SBLR_COMPAT_MONGODB_FIND_AND_MODIFY", "EngineDmlUpdate", "",
     "", false, true},
    {"AGGREGATE", PatternMatch::kPrefix, "query", "mongodb.query.aggregate",
     MappingDisposition::kAdmittedSblr, "mongodb.query.aggregate",
     "SBLR_COMPAT_MONGODB_AGGREGATE", "EngineQueryAggregate", "",
     "", false, false},
    {"FIND", PatternMatch::kPrefix, "query", "mongodb.query.find",
     MappingDisposition::kAdmittedSblr, "mongodb.query.find",
     "SBLR_COMPAT_MONGODB_FIND", "EngineQuerySelect", "",
     "", false, false},
    {"COUNT", PatternMatch::kPrefix, "query", "mongodb.query.count",
     MappingDisposition::kAdmittedSblr, "mongodb.query.count",
     "SBLR_COMPAT_MONGODB_COUNT", "EngineQueryAggregate", "",
     "", false, false},
    {"DISTINCT", PatternMatch::kPrefix, "query", "mongodb.query.distinct",
     MappingDisposition::kAdmittedSblr, "mongodb.query.distinct",
     "SBLR_COMPAT_MONGODB_DISTINCT", "EngineQueryAggregate", "",
     "", false, false},
    {"INSERT", PatternMatch::kPrefix, "dml", "mongodb.dml.insert",
     MappingDisposition::kAdmittedSblr, "mongodb.dml.insert",
     "SBLR_COMPAT_MONGODB_INSERT", "EngineDmlInsert", "",
     "", false, true},
    {"UPDATE", PatternMatch::kPrefix, "dml", "mongodb.dml.update",
     MappingDisposition::kAdmittedSblr, "mongodb.dml.update",
     "SBLR_COMPAT_MONGODB_UPDATE", "EngineDmlUpdate", "",
     "", false, true},
    {"DELETE", PatternMatch::kPrefix, "dml", "mongodb.dml.delete",
     MappingDisposition::kAdmittedSblr, "mongodb.dml.delete",
     "SBLR_COMPAT_MONGODB_DELETE", "EngineDmlDelete", "",
     "", false, true},
    {"STARTSESSION", PatternMatch::kContains, "session", "mongodb.session.start",
     MappingDisposition::kAdmittedSblr, "mongodb.session.start",
     "SBLR_COMPAT_MONGODB_START_SESSION", "EngineSessionRoute", "",
     "", false, false},
    {"COMMITTRANSACTION", PatternMatch::kContains, "transaction", "mongodb.transaction.commit",
     MappingDisposition::kAdmittedSblr, "mongodb.transaction.commit",
     "SBLR_TRANSACTION_COMMIT", "EngineCommitTransaction", "",
     "", false, true},
    {"ABORTTRANSACTION", PatternMatch::kContains, "transaction", "mongodb.transaction.abort",
     MappingDisposition::kAdmittedSblr, "mongodb.transaction.abort",
     "SBLR_TRANSACTION_ROLLBACK", "EngineRollbackTransaction", "",
     "", false, true},
};

const std::array<SurfaceDescriptor, 10> kDatatypeSurfaces{{
    {"document", "BSON document", "descriptor"},
    {"array", "BSON array", "descriptor"},
    {"object_id", "ObjectId", "descriptor"},
    {"string", "String", "descriptor"},
    {"numeric", "Int32;Int64;Double;Decimal128", "descriptor"},
    {"date", "Date;Timestamp", "descriptor"},
    {"binary", "BinData", "descriptor"},
    {"boolean", "Boolean", "descriptor"},
    {"regex", "RegularExpression", "parser_support_udr"},
    {"geo", "Point;LineString;Polygon", "parser_support_udr"},
}};

const std::array<SurfaceDescriptor, 10> kBuiltinSurfaces{{
    {"query_operators", "$eq;$gt;$gte;$lt;$lte;$in;$nin", "$expr"},
    {"logical_operators", "$and;$or;$not;$nor", "sblr"},
    {"aggregate_accumulators", "$sum;$avg;$min;$max;$count;$push", "sblr"},
    {"pipeline_stages", "$match;$project;$group;$sort;$limit;$lookup;$changeStream", "parser_support_udr"},
    {"update_operators", "$set;$unset;$inc;$push;$pull", "sblr"},
    {"array_operators", "$elemMatch;$size;$all", "sblr"},
    {"text_search", "$text;$search", "parser_support_udr"},
    {"geospatial", "$near;$geoWithin;$geoIntersects", "parser_support_udr"},
    {"date_expression", "$dateAdd;$dateDiff;$dateTrunc", "sblr"},
    {"security", "createUser;grantRolesToUser", "parser_support_udr"},
}};

const std::array<SurfaceDescriptor, 9> kCatalogSurfaces{{
    {"collections", "listCollections;system.namespaces", "catalog_projection"},
    {"indexes", "listIndexes;createIndexes", "catalog_projection"},
    {"users_roles", "system.users;system.roles", "catalog_projection"},
    {"server_status", "serverStatus", "catalog_projection"},
    {"current_op", "currentOp", "catalog_projection"},
    {"profile", "system.profile", "catalog_projection"},
    {"config", "config.*", "catalog_projection"},
    {"change_streams", "changeStreamPreImages", "catalog_projection"},
    {"sharding", "config.collections;config.chunks", "catalog_projection"},
}};

const std::array<SurfaceDescriptor, 10> kDiagnosticSurfaces{{
    {"parse", "MONGODB.PARSE.INVALID_INPUT;MONGODB.PARSE.UNSUPPORTED_SURFACE", "parser"},
    {"policy", "MONGODB.AUTHORITY.*", "fail_closed"},
    {"udr", "MONGODB.EMULATION.*", "parser_support_udr"},
    {"catalog", "MONGODB.CATALOG_OVERLAY.READ_ONLY", "catalog_projection"},
    {"session", "MONGODB.SESSION.*", "sblr"},
    {"transaction", "MONGODB.TRANSACTION.*", "sblr"},
    {"file_effects", "real_reference_file_effects=false", "authority_invariant"},
    {"reference_execution", "reference_engine_sql_executed=false", "authority_invariant"},
    {"mga", "parser_transaction_finality_authority=false", "authority_invariant"},
    {"support_bundle", "source_text_redacted=true", "diagnostic_redaction"},
}};

const scratchbird::parser::compatibility::DialectProfile kProfile{
    "mongodb",
    "MongoDB",
    "sbp_mongodb",
    "sbup_mongodb",
    "8.2.6",
    "MONGODB",
    kSblrFamily,
    kPatterns,
    kDatatypeSurfaces,
    kBuiltinSurfaces,
    kCatalogSurfaces,
    kDiagnosticSurfaces,
    27,
    120,
    90,
    0,
    9,
    0,
    3,
    9,
    0,
};

} // namespace

const scratchbird::parser::compatibility::DialectProfile& Profile() {
  return kProfile;
}

std::string TrimAscii(std::string_view text) {
  return scratchbird::parser::compatibility::TrimAscii(text);
}

std::string NormalizeWhitespace(std::string_view text) {
  return scratchbird::parser::compatibility::NormalizeWhitespace(text);
}

std::string ToUpperAscii(std::string_view text) {
  return scratchbird::parser::compatibility::ToUpperAscii(text);
}

std::string MessageVectorToJson(const std::vector<Diagnostic>& diagnostics) {
  return scratchbird::parser::compatibility::MessageVectorToJson(diagnostics);
}

std::vector<Token> LexTokens(std::string_view sql_text) {
  return scratchbird::parser::compatibility::LexTokens(sql_text);
}

ParseResult ParseStatement(std::string_view sql_text) {
  return scratchbird::parser::compatibility::ParseStatement(sql_text, kProfile);
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

std::string MongodbPackageIdentityJson() {
  return scratchbird::parser::compatibility::PackageIdentityJson(kProfile);
}

std::string MongodbSurfaceReportJson() {
  return scratchbird::parser::compatibility::SurfaceReportJson(kProfile);
}

} // namespace scratchbird::parser::mongodb
