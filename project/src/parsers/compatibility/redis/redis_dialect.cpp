// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "redis_dialect.hpp"

#include <array>

namespace scratchbird::parser::redis {
namespace {

using scratchbird::parser::compatibility::MappingDisposition;
using scratchbird::parser::compatibility::OperationPattern;
using scratchbird::parser::compatibility::PatternMatch;
using scratchbird::parser::compatibility::SurfaceDescriptor;

constexpr std::string_view kSblrFamily = "sblr.compatibility.redis.profile.v1";

constexpr OperationPattern kPatterns[] = {
    {"SAVE", PatternMatch::kPrefix, "persistence", "redis.persistence.save",
     MappingDisposition::kParserSupportUdr, "redis.udr.persistence.save",
     "SBLR_COMPAT_REDIS_PERSISTENCE_ROUTE", "ParserSupportPersistenceRoute",
     "REDIS.EMULATION.PERSISTENCE_ROUTE",
     "Persistence commands route through trusted engine checkpoint policy.", true, true},
    {"BGSAVE", PatternMatch::kPrefix, "persistence", "redis.persistence.bgsave",
     MappingDisposition::kParserSupportUdr, "redis.udr.persistence.bgsave",
     "SBLR_COMPAT_REDIS_PERSISTENCE_ROUTE", "ParserSupportPersistenceRoute",
     "REDIS.EMULATION.PERSISTENCE_ROUTE",
     "Persistence commands route through trusted engine checkpoint policy.", true, true},
    {"BGREWRITEAOF", PatternMatch::kPrefix, "persistence", "redis.persistence.bgrewriteaof",
     MappingDisposition::kParserSupportUdr, "redis.udr.persistence.aof",
     "SBLR_COMPAT_REDIS_PERSISTENCE_ROUTE", "ParserSupportPersistenceRoute",
     "REDIS.EMULATION.PERSISTENCE_ROUTE",
     "AOF rewrite routes through trusted persistence policy.", true, true},
    {"CONFIG", PatternMatch::kPrefix, "admin", "redis.admin.config",
     MappingDisposition::kParserSupportUdr, "redis.udr.config",
     "SBLR_COMPAT_REDIS_CONFIG_ROUTE", "ParserSupportConfigRoute",
     "REDIS.EMULATION.CONFIG_ROUTE",
     "Configuration commands route through trusted management policy.", true, false},
    {"MODULE", PatternMatch::kPrefix, "module", "redis.module.command",
     MappingDisposition::kParserSupportUdr, "redis.udr.module",
     "SBLR_COMPAT_REDIS_MODULE_ROUTE", "ParserSupportModuleRoute",
     "REDIS.EMULATION.MODULE_ROUTE",
     "Module commands route through trusted package policy.", true, true},
    {"CLUSTER", PatternMatch::kPrefix, "cluster", "redis.cluster.command",
     MappingDisposition::kAdmittedSblr, "cluster.job.start_controlled",
     "sblr.cluster.control.v3:cluster.job.start_controlled", "cluster.start_job", "",
     "", true, false},
    {"REPLICAOF", PatternMatch::kPrefix, "replication", "redis.replication.replicaof",
     MappingDisposition::kParserSupportUdr, "redis.udr.replication.replicaof",
     "SBLR_COMPAT_REDIS_REPLICATION_ROUTE", "ParserSupportReplicationRoute",
     "REDIS.EMULATION.REPLICATION_ROUTE",
     "Replica configuration requests route through the Redis compatibility UDR.", true, false},
    {"SLAVEOF", PatternMatch::kPrefix, "replication", "redis.replication.slaveof",
     MappingDisposition::kParserSupportUdr, "redis.udr.replication.slaveof",
     "SBLR_COMPAT_REDIS_REPLICATION_ROUTE", "ParserSupportReplicationRoute",
     "REDIS.EMULATION.REPLICATION_ROUTE",
     "Legacy replica configuration requests route through the Redis compatibility UDR.", true, false},
    {"FLUSHALL", PatternMatch::kPrefix, "admin", "redis.admin.flushall",
     MappingDisposition::kParserSupportUdr, "redis.udr.flush",
     "SBLR_COMPAT_REDIS_FLUSH_ROUTE", "ParserSupportFlushRoute",
     "REDIS.EMULATION.FLUSH_ROUTE",
     "Data destruction routes through trusted engine management policy.", true, true},
    {"FLUSHDB", PatternMatch::kPrefix, "admin", "redis.admin.flushdb",
     MappingDisposition::kParserSupportUdr, "redis.udr.flush",
     "SBLR_COMPAT_REDIS_FLUSH_ROUTE", "ParserSupportFlushRoute",
     "REDIS.EMULATION.FLUSH_ROUTE",
     "Database destruction routes through trusted engine management policy.", true, true},
    {"MIGRATE", PatternMatch::kPrefix, "network", "redis.network.migrate",
     MappingDisposition::kParserSupportUdr, "redis.udr.migrate",
     "SBLR_COMPAT_REDIS_MIGRATE_ROUTE", "ParserSupportNetworkRoute",
     "REDIS.EMULATION.MIGRATE_ROUTE",
     "Compatibility key migration routes through trusted network policy.", true, true},
    {"EVALSHA", PatternMatch::kPrefix, "script", "redis.script.evalsha",
     MappingDisposition::kParserSupportUdr, "redis.udr.script.evalsha",
     "SBLR_COMPAT_REDIS_SCRIPT_ROUTE", "ParserSupportScriptRoute", "REDIS.EMULATION.SCRIPT_ROUTE",
     "Lua script routing requires trusted script policy.", true, true},
    {"EVAL", PatternMatch::kPrefix, "script", "redis.script.eval",
     MappingDisposition::kParserSupportUdr, "redis.udr.script.eval",
     "SBLR_COMPAT_REDIS_SCRIPT_ROUTE", "ParserSupportScriptRoute", "REDIS.EMULATION.SCRIPT_ROUTE",
     "Lua script routing requires trusted script policy.", true, true},
    {"FUNCTION", PatternMatch::kPrefix, "script", "redis.function.command",
     MappingDisposition::kParserSupportUdr, "redis.udr.function",
     "SBLR_COMPAT_REDIS_FUNCTION_ROUTE", "ParserSupportFunctionRoute", "REDIS.EMULATION.FUNCTION_ROUTE",
     "Function library policy routes through trusted package support.", true, true},
    {"ACL", PatternMatch::kPrefix, "security", "redis.security.acl",
     MappingDisposition::kParserSupportUdr, "redis.udr.security.acl",
     "SBLR_COMPAT_REDIS_SECURITY_ROUTE", "ParserSupportSecurityRoute", "REDIS.EMULATION.SECURITY_ROUTE",
     "ACL mutation routes through trusted security policy.", true, false},
    {"XADD", PatternMatch::kPrefix, "stream", "redis.stream.xadd",
     MappingDisposition::kParserSupportUdr, "redis.udr.stream.xadd",
     "SBLR_COMPAT_REDIS_STREAM_ROUTE", "ParserSupportStreamRoute",
     "REDIS.EMULATION.STREAM_ROUTE",
     "Redis stream append routes through the Redis compatibility UDR.", false, true},
    {"XREAD", PatternMatch::kPrefix, "stream", "redis.stream.xread",
     MappingDisposition::kParserSupportUdr, "redis.udr.stream.xread",
     "SBLR_COMPAT_REDIS_STREAM_ROUTE", "ParserSupportStreamRoute",
     "REDIS.EMULATION.STREAM_ROUTE",
     "Redis stream reads route through the Redis compatibility UDR.", false, false},
    {"XGROUP", PatternMatch::kPrefix, "stream", "redis.stream.xgroup",
     MappingDisposition::kParserSupportUdr, "redis.udr.stream.xgroup",
     "SBLR_COMPAT_REDIS_STREAM_GROUP_ROUTE", "ParserSupportStreamGroupRoute", "REDIS.EMULATION.STREAM_GROUP_ROUTE",
     "Stream group policy routes through trusted package support.", true, true},
    {"SET", PatternMatch::kPrefix, "kv", "redis.kv.set",
     MappingDisposition::kAdmittedSblr, "redis.kv.set",
     "SBLR_COMPAT_REDIS_SET", "EngineKvSet", "",
     "", false, true},
    {"GET", PatternMatch::kPrefix, "kv", "redis.kv.get",
     MappingDisposition::kAdmittedSblr, "redis.kv.get",
     "SBLR_COMPAT_REDIS_GET", "EngineKvGet", "",
     "", false, false},
    {"DEL", PatternMatch::kPrefix, "kv", "redis.kv.del",
     MappingDisposition::kAdmittedSblr, "redis.kv.del",
     "SBLR_COMPAT_REDIS_DEL", "EngineKvDelete", "",
     "", false, true},
    {"HSET", PatternMatch::kPrefix, "hash", "redis.hash.hset",
     MappingDisposition::kAdmittedSblr, "redis.hash.hset",
     "SBLR_COMPAT_REDIS_HASH_SET", "EngineHashSet", "",
     "", false, true},
    {"HGET", PatternMatch::kPrefix, "hash", "redis.hash.hget",
     MappingDisposition::kAdmittedSblr, "redis.hash.hget",
     "SBLR_COMPAT_REDIS_HASH_GET", "EngineHashGet", "",
     "", false, false},
    {"LPUSH", PatternMatch::kPrefix, "list", "redis.list.lpush",
     MappingDisposition::kAdmittedSblr, "redis.list.lpush",
     "SBLR_COMPAT_REDIS_LIST_PUSH", "EngineListPush", "",
     "", false, true},
    {"SADD", PatternMatch::kPrefix, "set", "redis.set.sadd",
     MappingDisposition::kAdmittedSblr, "redis.set.sadd",
     "SBLR_COMPAT_REDIS_SET_ADD", "EngineSetAdd", "",
     "", false, true},
    {"ZADD", PatternMatch::kPrefix, "zset", "redis.zset.zadd",
     MappingDisposition::kAdmittedSblr, "redis.zset.zadd",
     "SBLR_COMPAT_REDIS_ZSET_ADD", "EngineSortedSetAdd", "",
     "", false, true},
    {"INCR", PatternMatch::kPrefix, "counter", "redis.counter.incr",
     MappingDisposition::kAdmittedSblr, "redis.counter.incr",
     "SBLR_COMPAT_REDIS_COUNTER_INCR", "EngineCounterIncrement", "",
     "", false, true},
    {"EXPIRE", PatternMatch::kPrefix, "ttl", "redis.ttl.expire",
     MappingDisposition::kAdmittedSblr, "redis.ttl.expire",
     "SBLR_COMPAT_REDIS_EXPIRE", "EngineTtlSet", "",
     "", false, true},
    {"SELECT", PatternMatch::kPrefix, "session", "redis.session.select_db",
     MappingDisposition::kAdmittedSblr, "redis.session.select_db",
     "SBLR_COMPAT_REDIS_SELECT_DB", "EngineSessionRoute", "",
     "", false, false},
    {"MULTI", PatternMatch::kPrefix, "transaction", "redis.transaction.multi",
     MappingDisposition::kAdmittedSblr, "redis.transaction.multi",
     "SBLR_TRANSACTION_BEGIN", "EngineBeginTransaction", "",
     "", false, false},
    {"EXEC", PatternMatch::kPrefix, "transaction", "redis.transaction.exec",
     MappingDisposition::kAdmittedSblr, "redis.transaction.exec",
     "SBLR_TRANSACTION_COMMIT", "EngineCommitTransaction", "",
     "", false, true},
    {"DISCARD", PatternMatch::kPrefix, "transaction", "redis.transaction.discard",
     MappingDisposition::kAdmittedSblr, "redis.transaction.discard",
     "SBLR_TRANSACTION_ROLLBACK", "EngineRollbackTransaction", "",
     "", false, true},
};

const std::array<SurfaceDescriptor, 10> kDatatypeSurfaces{{
    {"string", "String value", "descriptor"},
    {"list", "List", "descriptor"},
    {"set", "Set", "descriptor"},
    {"sorted_set", "Sorted set", "descriptor"},
    {"hash", "Hash", "descriptor"},
    {"stream", "Stream", "descriptor"},
    {"bitmap", "Bitmap", "descriptor"},
    {"hyperloglog", "HyperLogLog", "parser_support_udr"},
    {"geospatial", "Geo set", "parser_support_udr"},
    {"vector_set", "Vector set", "parser_support_udr"},
}};

const std::array<SurfaceDescriptor, 10> kBuiltinSurfaces{{
    {"kv", "GET;SET;DEL;EXISTS;TYPE", "sblr"},
    {"hash", "HGET;HSET;HDEL;HGETALL", "sblr"},
    {"list", "LPUSH;RPUSH;LPOP;RPOP;LRANGE", "sblr"},
    {"set", "SADD;SREM;SMEMBERS;SINTER", "sblr"},
    {"zset", "ZADD;ZRANGE;ZREM", "sblr"},
    {"stream", "XADD;XREAD;XRANGE", "parser_support_udr"},
    {"transaction", "MULTI;EXEC;DISCARD;WATCH", "sblr"},
    {"scripting", "EVAL;EVALSHA;FUNCTION", "parser_support_udr"},
    {"security", "ACL;AUTH", "parser_support_udr"},
    {"admin", "CONFIG;SAVE;MODULE;FLUSHALL;FLUSHDB;MIGRATE", "parser_support_udr"},
}};

const std::array<SurfaceDescriptor, 9> kCatalogSurfaces{{
    {"command_catalog", "COMMAND;COMMAND INFO", "catalog_projection"},
    {"keyspace", "DBSIZE;SCAN;INFO keyspace", "catalog_projection"},
    {"server_info", "INFO;CLIENT LIST", "catalog_projection"},
    {"acl", "ACL LIST;ACL USERS", "catalog_projection"},
    {"streams", "XINFO STREAM;XINFO GROUPS", "catalog_projection"},
    {"cluster", "CLUSTER INFO;CLUSTER NODES", "cluster_control_reserved"},
    {"modules", "MODULE LIST", "parser_support_udr"},
    {"memory", "MEMORY STATS", "catalog_projection"},
    {"latency", "LATENCY LATEST", "catalog_projection"},
}};

const std::array<SurfaceDescriptor, 10> kDiagnosticSurfaces{{
    {"parse", "REDIS.PARSE.INVALID_INPUT;REDIS.PARSE.UNSUPPORTED_SURFACE", "parser"},
    {"policy", "REDIS.AUTHORITY.*", "fail_closed"},
    {"udr", "REDIS.EMULATION.*", "parser_support_udr"},
    {"catalog", "REDIS.CATALOG_OVERLAY.READ_ONLY", "catalog_projection"},
    {"session", "REDIS.SESSION.*", "sblr"},
    {"transaction", "REDIS.TRANSACTION.*", "sblr"},
    {"file_effects", "real_reference_file_effects=false", "authority_invariant"},
    {"reference_execution", "reference_engine_sql_executed=false", "authority_invariant"},
    {"mga", "parser_transaction_finality_authority=false", "authority_invariant"},
    {"support_bundle", "source_text_redacted=true", "diagnostic_redaction"},
}};

const scratchbird::parser::compatibility::DialectProfile kProfile{
    "redis",
    "Redis",
    "sbp_redis",
    "sbup_redis",
    "8.6.2",
    "REDIS",
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
    1,
    3,
    13,
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

std::string RedisPackageIdentityJson() {
  return scratchbird::parser::compatibility::PackageIdentityJson(kProfile);
}

std::string RedisSurfaceReportJson() {
  return scratchbird::parser::compatibility::SurfaceReportJson(kProfile);
}

} // namespace scratchbird::parser::redis
