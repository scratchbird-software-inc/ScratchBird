// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_opcode_registry.hpp"

#include <array>
#include <string>
#include <utility>

namespace scratchbird::engine::sblr {
namespace {

struct CanonicalOpcodeCodeRow {
  std::string_view mnemonic;
  std::uint16_t code;
};

// SEARCH_KEY: SB_ENGINE_SBLR_CANONICAL_OPCODE_NUMERIC_AUTHORITY_V1
// Generated from the manifest-authoritative sblr-opcodes.yaml assignment.
// The runtime never derives a code from vector order, text hashing, aliases,
// or an implementation-private sequence.
constexpr std::array<CanonicalOpcodeCodeRow, 434> kCanonicalOpcodeCodes{{
    {"SBLR_PACKAGE_BEGIN", 0x0001u},
    {"SBLR_PACKAGE_END", 0x0002u},
    {"SBLR_LITERAL", 0x0003u},
    {"SBLR_PARAMETER", 0x0004u},
    {"SBLR_VARIABLE", 0x0005u},
    {"SBLR_SOURCE_MAP", 0x0006u},
    {"SBLR_ERROR_VECTOR", 0x0007u},
    {"SBLR_TXN_BEGIN", 0x0100u},
    {"SBLR_TXN_COMMIT", 0x0101u},
    {"SBLR_TXN_ROLLBACK", 0x0102u},
    {"SBLR_TXN_SAVEPOINT", 0x0103u},
    {"SBLR_TXN_RELEASE_SAVEPOINT", 0x0104u},
    {"SBLR_TXN_ROLLBACK_TO_SAVEPOINT", 0x0105u},
    {"SBLR_PSQL_AUTONOMOUS_FRAME", 0x0106u},
    {"SBLR_TRANSACTION_RESERVATION_RELEASE", 0x0107u},
    {"SBLR_TEMPORARY_INSTANCE_CLEANUP", 0x0108u},
    {"SBLR_CURSOR_OPEN", 0x0200u},
    {"SBLR_CURSOR_FETCH", 0x0201u},
    {"SBLR_CURSOR_CLOSE", 0x0202u},
    {"SBLR_READ_BY_KEY", 0x0203u},
    {"SBLR_READ_RANGE", 0x0204u},
    {"SBLR_READ_STREAM", 0x0205u},
    {"SBLR_RESULT_SET_PASS", 0x0206u},
    {"SBLR_ACCESS_CURSOR_OPEN", 0x0207u},
    {"SBLR_ACCESS_CURSOR_FETCH", 0x0208u},
    {"SBLR_ACCESS_CURSOR_CLOSE", 0x0209u},
    {"SBLR_INSERT", 0x0300u},
    {"SBLR_UPDATE", 0x0301u},
    {"SBLR_DELETE", 0x0302u},
    {"SBLR_MERGE", 0x0303u},
    {"SBLR_CLUSTER_WRITE_ADMISSION", 0x0304u},
    {"SBLR_TABLE_TRUNCATE", 0x0305u},
    {"SBLR_TABLE_ANALYZE", 0x0306u},
    {"SBLR_BULK_IMPORT_STREAM", 0x0307u},
    {"SBLR_BULK_EXPORT_STREAM", 0x0308u},
    {"SBLR_STATEMENT_BATCH", 0x0309u},
    {"SBLR_ATOMIC_CAS", 0x030Au},
    {"SBLR_ATOMIC_READ_MODIFY_WRITE", 0x030Bu},
    {"SBLR_ADVISORY_LOCK_ACQUIRE", 0x030Cu},
    {"SBLR_ADVISORY_LOCK_RELEASE", 0x030Du},
    {"SBLR_FUNCTION_CALL", 0x0400u},
    {"SBLR_OPERATOR_CALL", 0x0401u},
    {"SBLR_CAST", 0x0402u},
    {"SBLR_COMPARE", 0x0403u},
    {"SBLR_DOMAIN_OPERATION", 0x0404u},
    {"SBLR_UDR_INVOKE", 0x0405u},
    {"SBLR_PROCEDURE_INVOKE", 0x0406u},
    {"SBLR_FUNCTION_INVOKE", 0x0407u},
    {"SBLR_AGGREGATE_INVOKE", 0x0408u},
    {"SBLR_SEQUENCE_NEXTVAL", 0x0409u},
    {"SBLR_SEQUENCE_CURRVAL", 0x040Au},
    {"SBLR_SEQUENCE_SETVAL", 0x040Bu},
    {"SBLR_QUERY_APPLY_NUMERIC_OPERATION", 0x040Cu},
    {"SBLR_QUERY_EVALUATE_ADVANCED_DATATYPE_FAMILY", 0x040Du},
    {"SBLR_PROJECT", 0x0500u},
    {"SBLR_AGGREGATE", 0x0501u},
    {"SBLR_GROUP", 0x0502u},
    {"SBLR_SORT", 0x0503u},
    {"SBLR_LIMIT", 0x0504u},
    {"SBLR_WINDOW", 0x0505u},
    {"SBLR_RETURN_RESULT_SET", 0x0506u},
    {"SBLR_DDL_CREATE_SCHEMA", 0x0600u},
    {"SBLR_DDL_CREATE_TABLE", 0x0601u},
    {"SBLR_DDL_ALTER_TABLE", 0x0602u},
    {"SBLR_DDL_DROP_TABLE", 0x0603u},
    {"SBLR_DDL_CREATE_INDEX", 0x0604u},
      {"SBLR_DDL_DROP_INDEX", 0x0605u},
      {"SBLR_DDL_ALTER_GROUP", 0x0673u},
    {"SBLR_DDL_ALTER_LOCALIZED_NAME", 0x0674u},
    {"SBLR_DDL_CREATE_TABLE_AS_QUERY_WITH_DATA", 0x0685u},
    {"SBLR_DDL_CREATE_TABLE_AS_QUERY_WITH_NO_DATA", 0x0686u},
    {"SBLR_DDL_CREATE_DOMAIN", 0x0606u},
    {"SBLR_DDL_DROP_DOMAIN", 0x0607u},
    {"SBLR_DDL_DROP_SCHEMA", 0x0608u},
    {"SBLR_DDL_ALTER_SCHEMA", 0x0609u},
    {"SBLR_DDL_ALTER_INDEX", 0x060Au},
    {"SBLR_DDL_ALTER_DOMAIN", 0x060Bu},
    {"SBLR_DDL_CREATE_VIEW", 0x060Cu},
    {"SBLR_DDL_ALTER_VIEW", 0x060Du},
    {"SBLR_DDL_DROP_VIEW", 0x060Eu},
    {"SBLR_DDL_CREATE_TRIGGER", 0x060Fu},
    {"SBLR_DDL_ALTER_TRIGGER", 0x0610u},
    {"SBLR_DDL_DROP_TRIGGER", 0x0611u},
    {"SBLR_DDL_CREATE_PROCEDURE", 0x0612u},
    {"SBLR_DDL_ALTER_PROCEDURE", 0x0613u},
    {"SBLR_DDL_DROP_PROCEDURE", 0x0614u},
    {"SBLR_DDL_CREATE_FUNCTION", 0x0615u},
    {"SBLR_DDL_ALTER_FUNCTION", 0x0616u},
    {"SBLR_DDL_DROP_FUNCTION", 0x0617u},
    {"SBLR_DDL_CREATE_PACKAGE", 0x0618u},
    {"SBLR_DDL_ALTER_PACKAGE", 0x0619u},
    {"SBLR_DDL_DROP_PACKAGE", 0x061Au},
    {"SBLR_DDL_CREATE_SEQUENCE", 0x0687u},
    {"SBLR_DDL_ALTER_SEQUENCE", 0x061Cu},
    {"SBLR_DDL_DROP_SEQUENCE", 0x061Du},
    {"SBLR_DDL_CREATE_MATERIALIZED_VIEW", 0x061Eu},
    {"SBLR_DDL_REFRESH_MATERIALIZED_VIEW", 0x061Fu},
    {"SBLR_DDL_DROP_MATERIALIZED_VIEW", 0x0620u},
    {"SBLR_DDL_CREATE_TYPE", 0x0621u},
    {"SBLR_DDL_ALTER_TYPE", 0x0622u},
    {"SBLR_DDL_DROP_TYPE", 0x0623u},
    // The vector rename management operation has its own canonical wire
    // identity; it must not inherit the generic catalog RENAME OBJECT code.
    {"SBLR_DDL_RENAME_OBJECT_VECTOR", 0x061Bu},
    {"SBLR_DDL_RENAME_OBJECT", 0x0624u},
    {"SBLR_DDL_COMMENT_ON", 0x0625u},
    {"SBLR_DDL_CREATE_SYNONYM", 0x0626u},
    {"SBLR_DDL_DROP_SYNONYM", 0x0627u},
    {"SBLR_DDL_CREATE_FOREIGN_TABLE", 0x0628u},
    {"SBLR_DDL_DROP_FOREIGN_TABLE", 0x0629u},
    {"SBLR_DDL_CREATE_FDW", 0x062Au},
    {"SBLR_DDL_DROP_FDW", 0x062Bu},
    {"SBLR_SEC_CREATE_USER", 0x0700u},
    {"SBLR_SEC_ALTER_USER", 0x0701u},
    {"SBLR_SEC_CREATE_ROLE", 0x0702u},
    {"SBLR_SEC_GRANT", 0x0703u},
    {"SBLR_SEC_REVOKE", 0x0704u},
    {"SBLR_SEC_CREATE_GROUP_MAPPING", 0x0705u},
    {"SBLR_SEC_ALTER_POLICY", 0x0706u},
    {"SBLR_SEC_DROP_USER", 0x0707u},
    {"SBLR_SEC_ALTER_ROLE", 0x0708u},
    {"SBLR_SEC_DROP_ROLE", 0x0709u},
    {"SBLR_SEC_CREATE_POLICY", 0x070Au},
    {"SBLR_SEC_DROP_POLICY", 0x070Bu},
    {"SBLR_SEC_AUTHENTICATE", 0x070Cu},
    {"SBLR_SEC_DEAUTHENTICATE", 0x070Du},
    {"SBLR_SEC_DROP_GROUP_MAPPING", 0x070Eu},
    {"SBLR_FILESPACE_CREATE", 0x0800u},
    {"SBLR_FILESPACE_PREALLOCATE", 0x0801u},
    {"SBLR_FILESPACE_ATTACH", 0x0802u},
    {"SBLR_FILESPACE_DETACH", 0x0803u},
    {"SBLR_FILESPACE_MOVE", 0x0804u},
    {"SBLR_FILESPACE_PROMOTE", 0x0805u},
    {"SBLR_FILESPACE_COMPACT", 0x0806u},
    {"SBLR_FILESPACE_TRUNCATE", 0x0807u},
    {"SBLR_FILESPACE_DROP", 0x0808u},
    {"SBLR_INDEX_REBUILD", 0x0900u},
    {"SBLR_INDEX_REBALANCE", 0x0901u},
    {"SBLR_INDEX_VERIFY", 0x0902u},
    {"SBLR_INDEX_GATHER_STATISTICS", 0x0903u},
    {"SBLR_INDEX_CLEANUP_MGA_VERSIONS", 0x0904u},
    {"SBLR_BACKUP_START", 0x0A00u},
    {"SBLR_BACKUP_FINISH", 0x0A01u},
    {"SBLR_RESTORE_BACKUP", 0x0A02u},
    {"SBLR_ARCHIVE_EXPORT", 0x0A03u},
    {"SBLR_ARCHIVE_VERIFY", 0x0A04u},
    {"SBLR_CLUSTER_JOIN", 0x0B00u},
    {"SBLR_CLUSTER_LEAVE", 0x0B01u},
    {"SBLR_CLUSTER_ROUTE_REQUEST", 0x0B02u},
    {"SBLR_CLUSTER_PUBLISH_ROUTE", 0x0B03u},
    {"SBLR_CLUSTER_FENCE_NODE", 0x0B04u},
    {"SBLR_CLUSTER_RECONCILE_BRANCH", 0x0B05u},
    {"SBLR_CLUSTER_PUBLISH_EPOCH", 0x0B06u},
    {"SBLR_EMIT_DIAGNOSTIC", 0x0C00u},
    {"SBLR_READ_METRICS", 0x0C01u},
    {"SBLR_RESET_METRICS", 0x0C02u},
    {"SBLR_EXPLAIN_OPERATION", 0x0C03u},
    {"SBLR_EMIT_AUDIT_EVENT", 0x0C04u},
    {"SBLR_MGMT_OPERATION", 0x0D00u},
    {"SBLR_MGMT_PAYLOAD", 0x0D01u},
    {"SBLR_MGMT_RESULT", 0x0D02u},
    {"SBLR_MGMT_PROGRESS", 0x0D03u},
    {"SBLR_MGMT_DIAGNOSTIC", 0x0D04u},
    {"SBLR_MGMT_METRIC_SNAPSHOT_REF", 0x0D05u},
    {"SBLR_OBSERVABILITY_SHOW_VERSION", 0x0D06u},
    {"SBLR_OBSERVABILITY_SHOW_DATABASE", 0x0D07u},
    {"SBLR_OBSERVABILITY_SHOW_SYSTEM", 0x0D08u},
    {"SBLR_OBSERVABILITY_SHOW_CATALOG", 0x0D09u},
    {"SBLR_OBSERVABILITY_SHOW_SESSIONS", 0x0D0Au},
    {"SBLR_OBSERVABILITY_SHOW_TRANSACTIONS", 0x0D0Bu},
    {"SBLR_OBSERVABILITY_SHOW_LOCKS", 0x0D0Cu},
    {"SBLR_OBSERVABILITY_SHOW_STATEMENTS", 0x0D0Du},
    {"SBLR_OBSERVABILITY_SHOW_JOBS", 0x0D0Eu},
    {"SBLR_OBSERVABILITY_SHOW_MANAGEMENT", 0x0D0Fu},
    {"SBLR_OBSERVABILITY_SHOW_DIAGNOSTICS", 0x0D20u},
    {"SBLR_OBSERVABILITY_SHOW_DIAGNOSTICS_EXTENDED", 0x0D21u},
    {"SBLR_OBSERVABILITY_SHOW_ARCHIVE_REPLICATION", 0x0D22u},
    {"SBLR_OBSERVABILITY_SHOW_AGENTS_EXTENDED", 0x0D23u},
    {"SBLR_OBSERVABILITY_SHOW_FILESPACE_EXTENDED", 0x0D24u},
    {"SBLR_OBSERVABILITY_SHOW_DECISION_SERVICE", 0x0D27u},
    {"SBLR_OBSERVABILITY_SHOW_ACCELERATION", 0x0D25u},
    {"SBLR_OBSERVABILITY_SHOW_ACCELERATION_EXTENDED", 0x0D26u},
    {"SBLR_OBSERVABILITY_EXPLAIN_OPERATION", 0x0D29u},
    {"SBLR_OBSERVABILITY_SHOW_METRICS", 0x0D2Au},
    {"SBLR_MGA_SHOW_HORIZONS", 0x0D10u},
    {"SBLR_MGA_CHECKPOINT", 0x0D11u},
    {"SBLR_MGA_SWEEP", 0x0D12u},
    {"SBLR_MGA_CLEANUP_HOT_VERSIONS", 0x0D13u},
    {"SBLR_MGA_VERIFY_ARCHIVE_MANIFEST", 0x0D14u},
    {"SBLR_MGA_VERIFY_ARCHIVE_REACHABILITY", 0x0D15u},
    {"SBLR_MGA_VERIFY_ARCHIVE_DECRYPTABILITY", 0x0D16u},
    {"SBLR_MGA_VERIFY_BACKUP_COVERAGE", 0x0D17u},
    {"SBLR_MGA_PROVE_STREAM_TRUNCATION", 0x0D18u},
    {"SBLR_MGA_CLUSTER_TXN_INSPECT", 0x0D19u},
    {"SBLR_MGA_CLUSTER_TXN_RESOLVE", 0x0D1Au},
    {"SBLR_MGA_CLUSTER_TXN_RETRY_DECISION", 0x0D1Bu},
    {"SBLR_MGA_CLUSTER_TXN_QUARANTINE", 0x0D1Cu},
    {"SBLR_MGA_SHOW_ARCHIVE_ORPHANS", 0x0D1Du},
    {"SBLR_MGA_RECLAIM_ARCHIVE_ORPHANS", 0x0D1Eu},
    {"SBLR_MGA_AUDIT_LEGAL_HOLD", 0x0D1Fu},
    {"SBLR_PLAN_PHYSICAL_PROPERTY_REQUIREMENT", 0x0E00u},
    {"SBLR_PLAN_DONOR_COMPATIBILITY_REQUIREMENT", 0x0E01u},
    {"SBLR_PLAN_MGA_VISIBILITY_REQUIREMENT", 0x0E02u},
    {"SBLR_PLAN_CACHE_DEPENDENCY_IDENTITY", 0x0E03u},
    {"SBLR_EXPLAIN_METADATA", 0x0E04u},
    {"SBLR_ADAPTIVE_FEEDBACK", 0x0E05u},
    {"SBLR_REMOTE_OPERATOR", 0x0E06u},
    {"SBLR_VECTOR_PLAN_NODE", 0x0E07u},
    {"SBLR_TEXT_PLAN_NODE", 0x0E08u},
    {"SBLR_LEARNED_ADVISORY_HINT", 0x0E09u},
    {"SBLR_EVENT_CHANNEL_CREATE", 0x0F00u},
    {"SBLR_EVENT_CHANNEL_ALTER", 0x0F01u},
    {"SBLR_EVENT_CHANNEL_DROP", 0x0F02u},
    {"SBLR_EVENT_CHANNEL_LISTEN", 0x0F03u},
    {"SBLR_EVENT_CHANNEL_UNLISTEN", 0x0F04u},
    {"SBLR_EVENT_CHANNEL_UNLISTEN_ALL", 0x0F05u},
    {"SBLR_EVENT_CHANNEL_NOTIFY", 0x0F06u},
    {"SBLR_EVENT_SUBSCRIPTION_LIST", 0x0F07u},
    {"SBLR_EVENT_DELIVERY_POLL", 0x0F08u},
    {"SBLR_EVENT_DELIVERY_ACK", 0x0F09u},
    {"SBLR_CONN_OPEN", 0x1100u},
    {"SBLR_CONN_CLOSE", 0x1101u},
    {"SBLR_CONN_HELLO", 0x1102u},
    {"SBLR_SESSION_SETTING_SET", 0x1103u},
    {"SBLR_SESSION_SETTING_GET", 0x1104u},
    {"SBLR_SESSION_SETTING_RESET", 0x1105u},
    {"SBLR_SESSION_DEFAULT_QUALIFIER_SET", 0x1106u},
    {"SBLR_SESSION_ROLE_SWITCH", 0x1107u},
    {"SBLR_SESSION_DISCARD", 0x1108u},
    {"SBLR_SESSION_SNAPSHOT_HANDLE", 0x1109u},
    {"SBLR_STMT_PREPARE", 0x1200u},
    {"SBLR_STMT_EXECUTE", 0x1201u},
    {"SBLR_STMT_EXECUTE_DIRECT", 0x1202u},
    {"SBLR_STMT_FREE", 0x1203u},
    {"SBLR_STMT_CANCEL", 0x1204u},
    {"SBLR_PARAMETER_BIND", 0x1205u},
    {"SBLR_RESULT_PAGE", 0x1206u},
    {"SBLR_QUERY_EXECUTE", 0x1207u},
    {"SBLR_QUERY_EXPLAIN", 0x1208u},
    {"SBLR_CATALOG_INTROSPECT", 0x1300u},
    {"SBLR_NAME_RESOLVE", 0x1301u},
    {"SBLR_OPTIMIZER_STATS_READ", 0x1302u},
    {"SBLR_OPTIMIZER_STATS_DROP", 0x1303u},
    {"SBLR_PARSE_TEXT", 0x1304u},
    {"SBLR_CATALOG_EPOCH_CHECK", 0x1305u},
    {"SBLR_DATABASE_ATTACH", 0x1400u},
    {"SBLR_DATABASE_DETACH", 0x1401u},
    {"SBLR_DATABASE_CHECKPOINT", 0x1402u},
    {"SBLR_DATABASE_VACUUM", 0x1403u},
    {"SBLR_DATABASE_ALTER", 0x1404u},
    {"SBLR_LIFECYCLE_CREATE_DATABASE", 0x1408u},
    {"SBLR_LIFECYCLE_OPEN_DATABASE", 0x1409u},
    {"SBLR_LIFECYCLE_OPEN_DATABASE", 0x1409u},
    {"SBLR_LIFECYCLE_ATTACH_DATABASE", 0x140Au},
    {"SBLR_LIFECYCLE_DETACH_DATABASE", 0x140Bu},
    {"SBLR_LIFECYCLE_EXIT_MAINTENANCE", 0x140Du},
    {"SBLR_LIFECYCLE_DETACH_DATABASE", 0x140Bu},
    {"SBLR_LIFECYCLE_ENTER_MAINTENANCE", 0x140Cu},
    {"SBLR_LIFECYCLE_EXIT_MAINTENANCE", 0x140Du},
    {"SBLR_LIFECYCLE_ENTER_RESTRICTED_OPEN", 0x140Eu},
    {"SBLR_LIFECYCLE_EXIT_RESTRICTED_OPEN", 0x140Fu},
    {"SBLR_LIFECYCLE_INSPECT_DATABASE", 0x1410u},
    {"SBLR_LIFECYCLE_VERIFY_DATABASE", 0x1411u},
    {"SBLR_LIFECYCLE_REPAIR_DATABASE", 0x1412u},
    {"SBLR_LIFECYCLE_SHUTDOWN_DATABASE", 0x1413u},
    {"SBLR_LIFECYCLE_SHUTDOWN_FORCE", 0x1414u},
    {"SBLR_LIFECYCLE_SHUTDOWN_ACKNOWLEDGE", 0x1415u},
    {"SBLR_LIFECYCLE_DROP_DATABASE", 0x1416u},
    {"SBLR_REPL_CONSUMER_SUBSCRIBE", 0x1500u},
    {"SBLR_REPL_CONSUMER_RESUME", 0x1501u},
    {"SBLR_REPL_CONSUMER_PAUSE", 0x1502u},
    {"SBLR_REPL_CONSUMER_CANCEL", 0x1503u},
    {"SBLR_REPL_CDC_RECEIVE", 0x1504u},
    {"SBLR_REPL_CDC_ACK", 0x1505u},
    {"SBLR_REPL_2PC_PREWRITE", 0x1510u},
    {"SBLR_REPL_2PC_COMMIT", 0x1511u},
    {"SBLR_REPL_2PC_CLEANUP", 0x1512u},
    {"SBLR_REPL_2PC_RESOLVE_LOCK", 0x1513u},
    {"SBLR_REPL_2PC_PESSIMISTIC_LOCK", 0x1514u},
    {"SBLR_REPL_2PC_PESSIMISTIC_ROLLBACK", 0x1515u},
    {"SBLR_REPL_2PC_HEARTBEAT", 0x1516u},
    {"SBLR_REPL_2PC_CHECK_STATUS", 0x1517u},
    {"SBLR_GRAPH_TRAVERSE", 0x1600u},
    {"SBLR_GRAPH_OPTIONAL_MATCH", 0x1601u},
    {"SBLR_GRAPH_CREATE", 0x1602u},
    {"SBLR_GRAPH_MERGE", 0x1603u},
    {"SBLR_GRAPH_SET", 0x1604u},
    {"SBLR_GRAPH_REMOVE", 0x1605u},
    {"SBLR_GRAPH_DELETE", 0x1606u},
    {"SBLR_GRAPH_DETACH_DELETE", 0x1607u},
    {"SBLR_VECTOR_SEARCH", 0x1700u},
    {"SBLR_VECTOR_HYBRID_SEARCH", 0x1701u},
    {"SBLR_VECTOR_SIMILARITY", 0x1702u},
    {"SBLR_VECTOR_INDEX_LOAD", 0x1703u},
    {"SBLR_VECTOR_INDEX_RELEASE", 0x1704u},
    {"SBLR_FULLTEXT_SCORE", 0x1800u},
    {"SBLR_FULLTEXT_PHRASE_SCORE", 0x1801u},
    {"SBLR_FULLTEXT_MULTI_FIELD_SCORE", 0x1802u},
    {"SBLR_FULLTEXT_REGEX_MATCH", 0x1803u},
    {"SBLR_FULLTEXT_WILDCARD_MATCH", 0x1804u},
    {"SBLR_FULLTEXT_PREFIX_MATCH", 0x1805u},
    {"SBLR_FULLTEXT_ANALYZER_APPLY", 0x1806u},
    {"SBLR_DIAGNOSTIC_REFUSAL", 0x1900u},
    {"SBLR_DIAGNOSTIC_RESET", 0x1901u},
    {"SBLR_DESCRIPTOR_TRANSFORM", 0x1902u},
    {"SBLR_MIGRATION_BEGIN_DONOR", 0x1A00u},
    {"SBLR_MIGRATION_ALTER", 0x1A01u},
    {"SBLR_SHOW_MIGRATION", 0x1A02u},
    {"SBLR_MIGRATION_CUTOVER", 0x1A03u},
    {"SBLR_MIGRATION_ROLLBACK", 0x1A04u},
    {"SBLR_MIGRATION_RETAIN_EVIDENCE", 0x1A05u},
    {"SBLR_JOIN", 0x0507u},
    {"SBLR_SET_OPERATION", 0x0508u},
    {"SBLR_CTE", 0x0509u},
    {"SBLR_RECURSIVE_CTE", 0x050Au},
    {"SBLR_PIVOT", 0x050Bu},
    {"SBLR_UNPIVOT", 0x050Cu},
    {"SBLR_VALUES", 0x050Du},
    {"SBLR_MATCH_RECOGNIZE", 0x050Eu},
    {"SBLR_TABLE_FUNCTION_INVOKE", 0x050Fu},
    {"SBLR_KV_STRUCTURED_READ", 0x2000u},
    {"SBLR_KV_STRUCTURED_MUTATE", 0x2001u},
    {"SBLR_KV_STRUCTURED_SCAN", 0x2002u},
    {"SBLR_KV_STRUCTURED_STREAM_READ", 0x2003u},
    {"SBLR_KV_STRUCTURED_STREAM_APPEND", 0x2004u},
    {"SBLR_KV_STRUCTURED_TIMESERIES", 0x2005u},
    {"SBLR_SYSTEM_CONFIG_SET", 0x1405u},
    {"SBLR_SYSTEM_CONFIG_GET", 0x1406u},
    {"SBLR_SYSTEM_CONFIG_RESET", 0x1407u},
    {"SBLR_DDL_CREATE_RULE", 0x062Cu},
    {"SBLR_DDL_DROP_RULE", 0x062Du},
    {"SBLR_DDL_CREATE_PUBLICATION", 0x062Eu},
    {"SBLR_DDL_ALTER_PUBLICATION", 0x062Fu},
    {"SBLR_DDL_DROP_PUBLICATION", 0x0630u},
    {"SBLR_DDL_CREATE_SUBSCRIPTION", 0x0631u},
    {"SBLR_DDL_ALTER_SUBSCRIPTION", 0x0632u},
    {"SBLR_DDL_DROP_SUBSCRIPTION", 0x0633u},
    {"SBLR_DDL_DROP_AGGREGATE", 0x065Bu},
    {"SBLR_DDL_CREATE_OPERATOR", 0x0636u},
    {"SBLR_DDL_DROP_OPERATOR", 0x0637u},
    {"SBLR_DDL_CREATE_OPERATOR_CLASS", 0x0638u},
    {"SBLR_DDL_DROP_OPERATOR_CLASS", 0x0639u},
    {"SBLR_DDL_CREATE_OPERATOR_FAMILY", 0x063Au},
    {"SBLR_DDL_ALTER_OPERATOR_FAMILY", 0x063Bu},
    {"SBLR_DDL_DROP_OPERATOR_FAMILY", 0x063Cu},
    {"SBLR_DDL_CREATE_CAST", 0x063Du},
    {"SBLR_DDL_DROP_CAST", 0x063Eu},
    {"SBLR_DDL_CREATE_COLLATION", 0x063Fu},
    {"SBLR_DDL_ALTER_COLLATION", 0x0640u},
    {"SBLR_DDL_DROP_COLLATION", 0x0641u},
    {"SBLR_DDL_CREATE_EXTENSION", 0x0642u},
    {"SBLR_DDL_ALTER_EXTENSION", 0x0643u},
    {"SBLR_DDL_DROP_EXTENSION", 0x0644u},
    {"SBLR_DDL_CREATE_EVENT_TRIGGER", 0x0645u},
    {"SBLR_DDL_ALTER_EVENT_TRIGGER", 0x0646u},
    {"SBLR_DDL_DROP_EVENT_TRIGGER", 0x0647u},
    {"SBLR_CLUSTER_CREATE_PLACEMENT_POLICY", 0x0B07u},
    {"SBLR_CLUSTER_ALTER_PLACEMENT_POLICY", 0x0B08u},
    {"SBLR_CLUSTER_DROP_PLACEMENT_POLICY", 0x0B09u},
    {"SBLR_CLUSTER_DECLARE_REGION", 0x0B0Au},
    {"SBLR_CLUSTER_DECLARE_AVAILABILITY_ZONE", 0x0B0Bu},
    {"SBLR_CLUSTER_DECLARE_DATA_PLACEMENT", 0x0B0Cu},
    {"SBLR_CLUSTER_INSPECT_PROVIDER", 0x0B3Du},
    {"SBLR_DDL_CREATE_DICTIONARY", 0x0665u},
    {"SBLR_DDL_DROP_DICTIONARY", 0x0666u},
    {"SBLR_DDL_ALTER_DICTIONARY", 0x0667u},
    {"SBLR_DDL_CREATE_CONTINUOUS_VIEW", 0x0668u},
    {"SBLR_DDL_ALTER_CONTINUOUS_VIEW", 0x0669u},
    {"SBLR_DDL_DROP_CONTINUOUS_VIEW", 0x066Au},
    {"SBLR_DML_ASYNC_INSERT_SUBMIT", 0x066Bu},
    {"SBLR_DML_ASYNC_INSERT_STATUS", 0x066Cu},
    {"SBLR_DML_ASYNC_INSERT_CANCEL", 0x066Du},
    {"SBLR_DML_CONDITIONAL_MUTATE", 0x066Eu},
    {"SBLR_DML_COUNTER_ADD", 0x066Fu},
    {"SBLR_DML_TIMESERIES_SCHEMA_WRITE", 0x0670u},
    {"SBLR_DDL_SET_TIMESERIES_SERIES_CARDINALITY_POLICY", 0x0671u},
    {"SBLR_DDL_CREATE_TIMESERIES_VALUE_CACHE", 0x0672u},
    {"SBLR_DDL_CREATE_NAMED_COLLECTION", 0x064Au},
    {"SBLR_DDL_DROP_NAMED_COLLECTION", 0x064Bu},
    {"SBLR_DDL_VALIDATE_CONSTRAINT", 0x0654u},
    {"SBLR_SECURITY_CREATE_PRIVILEGE_TEMPLATE", 0x0655u},
    {"SBLR_SECURITY_ALTER_PRIVILEGE_TEMPLATE", 0x0656u},
    {"SBLR_SECURITY_DROP_PRIVILEGE_TEMPLATE", 0x0657u},
    {"SBLR_DATABASE_CREATE_TEMPLATE_CLONE", 0x0658u},
    {"SBLR_DDL_CREATE_AGGREGATE", 0x0659u},
    {"SBLR_BITEMPORAL_AS_OF_VALID_TIME", 0x2101u},
    {"SBLR_BITEMPORAL_PERIOD_OVERLAP", 0x2102u},
    {"SBLR_BITEMPORAL_FOR_VERSIONS_BETWEEN", 0x2103u},
    {"SBLR_VERSIONED_BRANCH_CREATE", 0x2106u},
    {"SBLR_VERSIONED_BRANCH_DELETE", 0x2107u},
    {"SBLR_VERSIONED_DIFF", 0x210Bu},
    {"SBLR_VERSIONED_TAG", 0x210Cu},
    {"SBLR_VERSIONED_REVERT", 0x210Du},
    {"SBLR_VERSIONED_RESET", 0x210Eu},
    {"SBLR_ACCEL_LLVM_POLICY_SET", 0x2200u},
    {"SBLR_ACCEL_LLVM_COMPILE", 0x2201u},
    {"SBLR_ACCEL_LLVM_INSPECT", 0x2202u},
    {"SBLR_ACCEL_LLVM_INVALIDATE", 0x2203u},
    {"SBLR_ACCEL_GPU_POLICY_SET", 0x2204u},
    {"SBLR_ACCEL_GPU_COMPILE", 0x2205u},
    {"SBLR_ACCEL_GPU_INSPECT", 0x2206u},
    {"SBLR_ACCEL_GPU_INVALIDATE", 0x2207u},
    {"SBLR_BRIDGE_DESCRIBE_CAPABILITIES", 0x2300u},
    {"SBLR_BRIDGE_OPEN_CHANNEL", 0x2301u},
    {"SBLR_BRIDGE_AUTHENTICATE", 0x2302u},
    {"SBLR_BRIDGE_OPEN_SESSION", 0x2303u},
    {"SBLR_BRIDGE_CLOSE_SESSION", 0x2304u},
    {"SBLR_BRIDGE_HEALTH", 0x2305u},
    {"SBLR_BRIDGE_CANCEL", 0x2306u},
    {"SBLR_BRIDGE_DRAIN", 0x2307u},
    {"SBLR_BRIDGE_TX_BEGIN", 0x2308u},
    {"SBLR_BRIDGE_TX_COMMIT", 0x2309u},
    {"SBLR_BRIDGE_TX_ROLLBACK", 0x230Au},
    {"SBLR_BRIDGE_TX_PREPARE", 0x230Bu},
    {"SBLR_BRIDGE_TX_SAVEPOINT", 0x230Cu},
    {"SBLR_BRIDGE_EXECUTE", 0x230Du},
    {"SBLR_BRIDGE_CURSOR_OPEN", 0x230Eu},
    {"SBLR_BRIDGE_CURSOR_FETCH", 0x230Fu},
    {"SBLR_BRIDGE_CURSOR_CLOSE", 0x2310u},
    {"SBLR_BRIDGE_STREAM_OPEN", 0x2311u},
    {"SBLR_BRIDGE_STREAM_READ", 0x2312u},
    {"SBLR_BRIDGE_STREAM_WRITE", 0x2313u},
    {"SBLR_BRIDGE_STREAM_CLOSE", 0x2314u},
    {"SBLR_BRIDGE_CDC_START", 0x2315u},
    {"SBLR_BRIDGE_CDC_READ", 0x2316u},
    {"SBLR_BRIDGE_CDC_APPLY", 0x2317u},
    {"SBLR_BRIDGE_PROXY_ROUTE", 0x2318u},
    {"SBLR_BRIDGE_COMPARE_RESULT", 0x2319u},
    {"SBLR_BRIDGE_CUTOVER", 0x231Au},
    {"SBLR_BRIDGE_VALIDATE", 0x231Bu},
    {"SBLR_BITEMPORAL_AS_OF", 0x2100u},
    {"SBLR_VERIFIABLE_HISTORY_PROVE", 0x2104u},
    {"SBLR_VERIFY_PROOF_DESCRIPTOR", 0x2105u},
    {"SBLR_VERSIONED_MERGE", 0x2108u},
    {"SBLR_VERSIONED_HASH_READ", 0x2109u},
    {"SBLR_VERSIONED_STATUS_READ", 0x210Au},
}};

std::uint16_t CanonicalOpcodeCode(std::string_view mnemonic) noexcept {
  for (const auto& row : kCanonicalOpcodeCodes) {
    if (row.mnemonic == mnemonic) return row.code;
  }
  return 0;
}

bool IsIa08ECatalogRefusalOpcode(std::string_view opcode) noexcept {
  static constexpr std::array<std::string_view, 56> kOpcodes{{
      "SBLR_DDL_ALTER_VIEW", "SBLR_DDL_DROP_VIEW",
      "SBLR_DDL_CREATE_PACKAGE", "SBLR_DDL_ALTER_PACKAGE", "SBLR_DDL_DROP_PACKAGE",
      "SBLR_DDL_CREATE_INDEX_TEMPLATE", "SBLR_DDL_CREATE_SEQUENCE", "SBLR_DDL_CREATE_VIEW",
      "SBLR_DDL_ALTER_SEQUENCE", "SBLR_DDL_DROP_SEQUENCE",
      "SBLR_DDL_CREATE_MATERIALIZED_VIEW", "SBLR_DDL_REFRESH_MATERIALIZED_VIEW",
      "SBLR_DDL_DROP_MATERIALIZED_VIEW", "SBLR_DDL_CREATE_TYPE", "SBLR_DDL_ALTER_TYPE",
      "SBLR_DDL_DROP_TYPE", "SBLR_DDL_RENAME_OBJECT", "SBLR_DDL_COMMENT_ON",
      "SBLR_DDL_CREATE_SYNONYM", "SBLR_DDL_DROP_SYNONYM", "SBLR_DDL_CREATE_FOREIGN_TABLE",
      "SBLR_DDL_DROP_FOREIGN_TABLE", "SBLR_DDL_CREATE_FDW", "SBLR_DDL_DROP_FDW",
      "SBLR_DDL_CREATE_RULE", "SBLR_DDL_DROP_RULE", "SBLR_DDL_CREATE_PUBLICATION",
      "SBLR_DDL_ALTER_PUBLICATION", "SBLR_DDL_DROP_PUBLICATION",
      "SBLR_DDL_CREATE_SUBSCRIPTION", "SBLR_DDL_ALTER_SUBSCRIPTION",
      "SBLR_DDL_DROP_SUBSCRIPTION", "SBLR_DDL_CREATE_AGGREGATE",
      "SBLR_DDL_DROP_AGGREGATE", "SBLR_DDL_CREATE_OPERATOR", "SBLR_DDL_DROP_OPERATOR",
      "SBLR_DDL_CREATE_OPERATOR_CLASS", "SBLR_DDL_DROP_OPERATOR_CLASS",
      "SBLR_DDL_CREATE_OPERATOR_FAMILY", "SBLR_DDL_ALTER_OPERATOR_FAMILY",
      "SBLR_DDL_DROP_OPERATOR_FAMILY", "SBLR_DDL_CREATE_CAST", "SBLR_DDL_DROP_CAST",
      "SBLR_DDL_CREATE_COLLATION", "SBLR_DDL_ALTER_COLLATION", "SBLR_DDL_DROP_COLLATION",
      "SBLR_DDL_CREATE_EXTENSION", "SBLR_DDL_ALTER_EXTENSION", "SBLR_DDL_DROP_EXTENSION",
      "SBLR_DDL_CREATE_EVENT_TRIGGER", "SBLR_DDL_ALTER_EVENT_TRIGGER",
      "SBLR_DDL_DROP_EVENT_TRIGGER", "SBLR_DDL_CREATE_DICTIONARY",
      "SBLR_DDL_DROP_DICTIONARY", "SBLR_DDL_CREATE_NAMED_COLLECTION",
      "SBLR_DDL_DROP_NAMED_COLLECTION",
  }};
  for (const auto candidate : kOpcodes) {
    if (candidate == opcode) return true;
  }
  return false;
}

bool IsIa09SecurityRefusalOpcode(std::string_view opcode) noexcept {
  static constexpr std::array<std::string_view, 15> kOpcodes{{
      "SBLR_SEC_CREATE_USER", "SBLR_SEC_ALTER_USER", "SBLR_SEC_CREATE_ROLE",
      "SBLR_SEC_GRANT", "SBLR_SEC_REVOKE", "SBLR_SEC_CREATE_GROUP_MAPPING",
      "SBLR_SEC_ALTER_POLICY", "SBLR_SEC_DROP_USER", "SBLR_SEC_ALTER_ROLE",
      "SBLR_SEC_DROP_ROLE", "SBLR_SEC_CREATE_POLICY", "SBLR_SEC_DROP_POLICY",
      "SBLR_SEC_AUTHENTICATE", "SBLR_SEC_DEAUTHENTICATE",
      "SBLR_SEC_DROP_GROUP_MAPPING",
  }};
  for (const auto candidate : kOpcodes) {
    if (candidate == opcode) return true;
  }
  return false;
}

SblrOpcodeEntry Entry(std::string operation_id,
                      std::string opcode,
                      SblrOpcodeCategory category,
                      SblrOpcodeSupport support,
                      bool requires_security_context = true,
                      bool requires_transaction_context = false,
                      bool requires_cluster_authority = false,
                      std::string refusal_diagnostic = {}) {
  SblrOpcodeEntry entry;
  entry.code = CanonicalOpcodeCode(opcode);
  entry.operation_id = std::move(operation_id);
  entry.opcode = std::move(opcode);
  entry.category = category;
  entry.support = support;
  entry.requires_security_context = requires_security_context;
  entry.requires_transaction_context = requires_transaction_context;
  entry.requires_cluster_authority = requires_cluster_authority;
  entry.refusal_diagnostic = std::move(refusal_diagnostic);
  if (IsIa08ECatalogRefusalOpcode(entry.opcode) ||
      IsIa09SecurityRefusalOpcode(entry.opcode)) {
    entry.support = SblrOpcodeSupport::local_profile_refusal;
    entry.refusal_diagnostic = "PROFILE.BUILTIN_PROFILE_UNAVAILABLE";
  }
  return entry;
}

void ApplyIa01SemanticContract(SblrOpcodeEntry* entry) {
  struct Contract {
    std::string_view opcode;
    std::string_view operand;
    std::string_view result;
    std::string_view executor;
  };
  static constexpr Contract contracts[] = {
      {"SBLR_PACKAGE_BEGIN", "package_header", "void", "engine.op.package_begin"},
      {"SBLR_PACKAGE_END", "package_footer", "void", "engine.op.package_end"},
      {"SBLR_LITERAL", "typed_literal", "typed_value", "engine.op.literal"},
      {"SBLR_PARAMETER", "parameter_descriptor_ref", "typed_value", "engine.op.parameter"},
      {"SBLR_VARIABLE", "variable_descriptor_ref", "typed_value", "engine.op.variable"},
      {"SBLR_SOURCE_MAP", "source_map_entry_vector", "void", "engine.op.source_map"},
      {"SBLR_ERROR_VECTOR", "diagnostic_vector", "void", "engine.op.error_vector"},
      {"SBLR_TXN_BEGIN", "transaction_begin_options", "transaction_handle", "engine.op.txn_begin"},
      {"SBLR_TXN_COMMIT", "transaction_handle_and_commit_options", "commit_result", "engine.op.txn_commit"},
      {"SBLR_TXN_ROLLBACK", "transaction_handle_and_rollback_options", "rollback_result", "engine.op.txn_rollback"},
      {"SBLR_TXN_SAVEPOINT", "savepoint_descriptor", "savepoint_handle", "engine.op.txn_savepoint"},
      {"SBLR_TXN_RELEASE_SAVEPOINT", "savepoint_release_handle", "savepoint_release_result", "engine.op.txn_release_savepoint"},
      {"SBLR_TXN_ROLLBACK_TO_SAVEPOINT", "savepoint_rollback_handle", "savepoint_rollback_result", "engine.op.txn_rollback_to_savepoint"},
      {"SBLR_PSQL_AUTONOMOUS_FRAME", "autonomous_frame_descriptor", "autonomous_frame_result", "engine.op.psql_autonomous_frame"},
      {"SBLR_TRANSACTION_RESERVATION_RELEASE", "relation_reservation_release_descriptor", "transaction_reservation_result", "engine.op.transaction_reservation_release"},
      {"SBLR_TEMPORARY_INSTANCE_CLEANUP", "temporary_instance_cleanup_descriptor", "temporary_cleanup_result", "engine.op.temporary_instance_cleanup"},
      {"SBLR_CURSOR_OPEN", "cursor_open_plan_ref", "cursor_handle", "engine.op.cursor_open"},
      {"SBLR_CURSOR_FETCH", "cursor_fetch_handle", "cursor_fetch_result", "engine.op.cursor_fetch"},
      {"SBLR_CURSOR_CLOSE", "cursor_close_handle", "cursor_close_result", "engine.op.cursor_close"},
      {"SBLR_READ_BY_KEY", "uuid_object_key_descriptor", "row_descriptor", "engine.op.read_by_key"},
      {"SBLR_READ_RANGE", "range_scan_descriptor", "rowset_descriptor", "engine.op.read_range"},
      {"SBLR_READ_STREAM", "stream_descriptor", "stream_handle", "engine.op.read_stream"},
      {"SBLR_RESULT_SET_PASS", "result_set_handle_and_lifetime", "result_set_handle", "engine.op.result_set_pass"},
      {"SBLR_ACCESS_CURSOR_OPEN", "access_cursor_open_descriptor", "access_cursor_handle", "engine.op.access_cursor_open"},
      {"SBLR_ACCESS_CURSOR_FETCH", "access_cursor_fetch_descriptor", "access_cursor_rowset_or_eof", "engine.op.access_cursor_fetch"},
      {"SBLR_ACCESS_CURSOR_CLOSE", "access_cursor_close_descriptor", "void", "engine.op.access_cursor_close"},
      {"SBLR_CATALOG_INTROSPECT", "catalog_introspect_descriptor", "catalog_introspect_result", "engine.op.catalog_introspect"},
      {"SBLR_PROJECT", "projection_descriptor", "rowset_descriptor", "engine.op.project"},
      {"SBLR_AGGREGATE", "aggregate_descriptor", "rowset_descriptor", "engine.op.aggregate"},
      {"SBLR_GROUP", "group_descriptor", "rowset_descriptor", "engine.op.group"},
      {"SBLR_SORT", "sort_descriptor", "rowset_descriptor", "engine.op.sort"},
      {"SBLR_LIMIT", "limit_descriptor", "rowset_descriptor", "engine.op.limit"},
      {"SBLR_WINDOW", "window_descriptor", "rowset_descriptor", "engine.op.window"},
      {"SBLR_RETURN_RESULT_SET", "result_set_return_descriptor", "result_set_handle", "engine.op.return_result_set"},
      {"SBLR_KV_STRUCTURED_READ", "kv_structured_read_descriptor", "kv_structured_result", "engine.op.kv_structured_read"},
      {"SBLR_KV_STRUCTURED_MUTATE", "kv_structured_mutate_descriptor", "kv_structured_result", "engine.op.kv_structured_mutate"},
      {"SBLR_KV_STRUCTURED_SCAN", "kv_structured_scan_descriptor", "kv_structured_result", "engine.op.kv_structured_scan"},
      {"SBLR_KV_STRUCTURED_STREAM_READ", "kv_structured_stream_read_descriptor", "kv_structured_result", "engine.op.kv_structured_stream_read"},
      {"SBLR_KV_STRUCTURED_STREAM_APPEND", "kv_structured_stream_append_descriptor", "kv_structured_mutation_result", "engine.op.kv_structured_stream_append"},
      {"SBLR_KV_STRUCTURED_TIMESERIES", "kv_timeseries_descriptor", "kv_structured_result", "engine.op.kv_structured_timeseries"},
      {"SBLR_SYSTEM_CONFIG_SET", "system_config_set_descriptor", "management_result", "engine.op.system_config_set"},
      {"SBLR_DDL_CREATE_DOMAIN", "create_domain_descriptor", "ddl_result", "engine.op.ddl_create_domain"},
      {"SBLR_DDL_CREATE_SCHEMA", "create_schema_descriptor", "ddl_result", "engine.op.ddl_create_schema"},
      {"SBLR_DDL_ALTER_TABLE", "alter_table_descriptor", "ddl_result", "engine.op.ddl_alter_table"},
      {"SBLR_DDL_DROP_SCHEMA", "drop_schema_descriptor", "ddl_result", "engine.op.ddl_drop_schema"},
      {"SBLR_DDL_CREATE_TABLE", "create_table_descriptor", "ddl_result", "engine.op.ddl_create_table"},
      {"SBLR_DDL_CREATE_TABLE_AS_QUERY_WITH_DATA", "create_table_as_query_with_data_descriptor", "ddl_result", "engine.op.ddl_create_table_as_query_with_data"},
      {"SBLR_DDL_CREATE_TABLE_AS_QUERY_WITH_NO_DATA", "create_table_as_query_with_no_data_descriptor", "ddl_result", "engine.op.ddl_create_table_as_query_with_no_data"},
      {"SBLR_DDL_CREATE_INDEX", "create_index_descriptor", "ddl_result", "engine.op.ddl_create_index"},
      {"SBLR_DDL_CREATE_PROCEDURE", "create_procedure_descriptor", "ddl_result", "engine.op.ddl_create_procedure"},
      {"SBLR_DDL_CREATE_PACKAGE", "create_package_descriptor", "ddl_result", "engine.op.ddl_create_package"},
      {"SBLR_DDL_DROP_PACKAGE", "drop_package_descriptor", "ddl_result", "engine.op.ddl_drop_package"},
      {"SBLR_DDL_DROP_SYNONYM", "drop_package_descriptor", "ddl_result", "engine.op.ddl_drop_synonym"},
      {"SBLR_DDL_ALTER_SEQUENCE", "alter_sequence_descriptor", "ddl_result", "engine.op.ddl_alter_sequence"},
      {"SBLR_DDL_CREATE_MATERIALIZED_VIEW", "create_materialized_view_descriptor", "ddl_result", "engine.op.ddl_create_materialized_view"},
      {"SBLR_DDL_CREATE_TYPE", "create_type_descriptor", "ddl_result", "engine.op.ddl_create_type"},
      {"SBLR_DDL_ALTER_TYPE", "alter_type_descriptor", "ddl_result", "engine.op.ddl_alter_type"},
      {"SBLR_DDL_CREATE_SEQUENCE", "create_sequence_descriptor", "ddl_result", "engine.op.ddl_create_sequence"},
      {"SBLR_DDL_DROP_SEQUENCE", "drop_sequence_descriptor", "ddl_result", "engine.op.ddl_drop_sequence"},
      {"SBLR_DDL_REFRESH_MATERIALIZED_VIEW", "refresh_materialized_view_descriptor", "ddl_result", "engine.op.ddl_refresh_materialized_view"},
      {"SBLR_DDL_DROP_MATERIALIZED_VIEW", "drop_materialized_view_descriptor", "ddl_result", "engine.op.ddl_drop_materialized_view"},
      {"SBLR_DDL_DROP_TYPE", "drop_type_descriptor", "ddl_result", "engine.op.ddl_drop_type"},
      {"SBLR_DDL_DROP_INDEX", "drop_index_descriptor", "ddl_result", "engine.op.ddl_drop_index"},
      {"SBLR_DDL_ALTER_DOMAIN", "alter_domain_descriptor", "ddl_result", "engine.op.ddl_alter_domain"},
      {"SBLR_DDL_CREATE_VIEW", "create_view_descriptor", "ddl_result", "engine.op.ddl_create_view"},
      {"SBLR_DDL_ALTER_VIEW", "alter_view_descriptor", "ddl_result", "engine.op.ddl_alter_view"},
      {"SBLR_DDL_DROP_VIEW", "drop_view_descriptor", "ddl_result", "engine.op.ddl_drop_view"},
      {"SBLR_DDL_CREATE_TRIGGER", "create_trigger_descriptor", "ddl_result", "engine.op.ddl_create_trigger"},
      {"SBLR_DDL_CREATE_PROCEDURE", "create_procedure_descriptor", "ddl_result", "engine.op.ddl_create_procedure"},
      {"SBLR_DDL_ALTER_PROCEDURE", "alter_procedure_descriptor", "ddl_result", "engine.op.ddl_alter_procedure"},
      {"SBLR_DDL_DROP_PROCEDURE", "drop_procedure_descriptor", "ddl_result", "engine.op.ddl_drop_procedure"},
      {"SBLR_DDL_CREATE_FUNCTION", "create_function_descriptor", "ddl_result", "engine.op.ddl_create_function"},
      {"SBLR_DDL_ALTER_FUNCTION", "alter_function_descriptor", "ddl_result", "engine.op.ddl_alter_function"},
      {"SBLR_DDL_DROP_FUNCTION", "drop_function_descriptor", "ddl_result", "engine.op.ddl_drop_function"},
      {"SBLR_DDL_CREATE_PACKAGE", "create_package_descriptor", "ddl_result", "engine.op.ddl_create_package"},
      {"SBLR_DDL_ALTER_PACKAGE", "alter_package_descriptor", "ddl_result", "engine.op.ddl_alter_package"},
      {"SBLR_DDL_DROP_PACKAGE", "drop_package_descriptor", "ddl_result", "engine.op.ddl_drop_package"},
      {"SBLR_DDL_DROP_SYNONYM", "drop_package_descriptor", "ddl_result", "engine.op.ddl_drop_synonym"},
      {"SBLR_DDL_ALTER_TRIGGER", "alter_trigger_descriptor", "ddl_result", "engine.op.ddl_alter_trigger"},
      {"SBLR_DDL_DROP_TRIGGER", "drop_trigger_descriptor", "ddl_result", "engine.op.ddl_drop_trigger"},
      {"SBLR_OBSERVABILITY_SHOW_VERSION", "observability_show_version_descriptor", "observability_show_version_result", "observability.show_version"},
      {"SBLR_OBSERVABILITY_SHOW_DATABASE", "observability_show_database_descriptor", "observability_show_database_result", "observability.show_database"},
      {"SBLR_OBSERVABILITY_SHOW_SYSTEM", "observability_show_system_descriptor", "observability_show_system_result", "observability.show_system"},
      {"SBLR_OBSERVABILITY_SHOW_CATALOG", "observability_show_catalog_descriptor", "observability_show_catalog_result", "observability.show_catalog"},
      {"SBLR_OBSERVABILITY_SHOW_SESSIONS", "observability_show_sessions_descriptor", "observability_show_sessions_result", "observability.show_sessions"},
      {"SBLR_OBSERVABILITY_SHOW_TRANSACTIONS", "observability_show_transactions_descriptor", "observability_show_transactions_result", "observability.show_transactions"},
      {"SBLR_OBSERVABILITY_SHOW_LOCKS", "observability_show_locks_descriptor", "observability_show_locks_result", "observability.show_locks"},
      {"SBLR_OBSERVABILITY_SHOW_STATEMENTS", "observability_show_statements_descriptor", "observability_show_statements_result", "observability.show_statements"},
      {"SBLR_OBSERVABILITY_SHOW_JOBS", "observability_show_jobs_descriptor", "observability_show_jobs_result", "observability.show_jobs"},
      {"SBLR_OBSERVABILITY_SHOW_MANAGEMENT", "observability_show_management_descriptor", "observability_show_management_result", "observability.show_management"},
      {"SBLR_OBSERVABILITY_SHOW_DIAGNOSTICS", "observability_show_diagnostics_descriptor", "observability_show_diagnostics_result", "observability.show_diagnostics"},
      {"SBLR_OBSERVABILITY_SHOW_DIAGNOSTICS_EXTENDED", "observability_show_diagnostics_extended_descriptor", "observability_show_diagnostics_extended_result", "observability.show_diagnostics_extended"},
      {"SBLR_OBSERVABILITY_SHOW_ARCHIVE_REPLICATION", "observability_show_archive_replication_descriptor", "observability_show_archive_replication_result", "observability.show_archive_replication"},
      {"SBLR_OBSERVABILITY_SHOW_AGENTS_EXTENDED", "observability_show_agents_extended_descriptor", "observability_show_agents_extended_result", "observability.show_agents_extended"},
      {"SBLR_OBSERVABILITY_SHOW_FILESPACE_EXTENDED", "observability_show_filespace_extended_descriptor", "observability_show_filespace_extended_result", "observability.show_filespace_extended"},
      {"SBLR_OBSERVABILITY_SHOW_DECISION_SERVICE", "observability_show_decision_service_descriptor", "observability_show_decision_service_result", "observability.show_decision_service"},
      {"SBLR_OBSERVABILITY_SHOW_ACCELERATION", "observability_show_acceleration_descriptor", "observability_show_acceleration_result", "observability.show_acceleration"},
      {"SBLR_OBSERVABILITY_SHOW_ACCELERATION_EXTENDED", "observability_show_acceleration_extended_descriptor", "observability_show_acceleration_extended_result", "observability.show_acceleration_extended"},
      {"SBLR_OBSERVABILITY_EXPLAIN_OPERATION", "observability_explain_operation_descriptor", "observability_explain_operation_result", "observability.explain_operation"},
      {"SBLR_LIFECYCLE_CREATE_DATABASE", "lifecycle_create_database_descriptor", "lifecycle_create_database_result", "engine.op.lifecycle_create_database"},
      {"SBLR_LIFECYCLE_OPEN_DATABASE", "lifecycle_open_database_descriptor", "lifecycle_open_database_result", "engine.op.lifecycle_open_database"},
      {"SBLR_LIFECYCLE_ATTACH_DATABASE", "lifecycle_attach_database_descriptor", "lifecycle_attach_database_result", "engine.op.lifecycle_attach_database"},
      {"SBLR_LIFECYCLE_DETACH_DATABASE", "lifecycle_detach_database_descriptor", "lifecycle_detach_database_result", "engine.op.lifecycle_detach_database"},
      {"SBLR_LIFECYCLE_EXIT_MAINTENANCE", "lifecycle_exit_maintenance_descriptor", "lifecycle_mode_transition_result", "engine.op.lifecycle_exit_maintenance"},
      {"SBLR_LIFECYCLE_ENTER_MAINTENANCE", "lifecycle_enter_maintenance_descriptor", "lifecycle_mode_transition_result", "engine.op.lifecycle_enter_maintenance"},
      {"SBLR_DDL_DROP_TABLE", "drop_table_descriptor", "ddl_result", "engine.op.ddl_drop_table"},
      {"SBLR_OBSERVABILITY_SHOW_METRICS", "observability_show_metrics_descriptor", "observability_show_metrics_result", "observability.show_metrics"},
      {"SBLR_CLUSTER_INSPECT_PROVIDER", "none", "cluster_provider_inspection_result", "cluster.inspect_provider"},
      {"SBLR_WINDOW", "window_descriptor", "rowset_descriptor", "engine.op.window"},
      {"SBLR_RETURN_RESULT_SET", "result_set_return_descriptor", "result_set_handle", "engine.op.return_result_set"},
      {"SBLR_DIAGNOSTIC_REFUSAL", "diagnostic_refusal_descriptor", "diagnostic_refusal_result", "engine.op.diagnostic_refusal"},
      {"SBLR_DIAGNOSTIC_RESET", "diagnostic_reset_descriptor", "diagnostic_reset_result", "engine.op.diagnostic_reset"},
      {"SBLR_DESCRIPTOR_TRANSFORM", "descriptor_transform_descriptor", "descriptor_transform_result", "engine.op.descriptor_transform"},
  };
  for (const auto& contract : contracts) {
    if (entry->opcode != contract.opcode) continue;
    entry->operation_id = contract.executor;
    entry->operand_contract = contract.operand;
    entry->result_contract = contract.result;
    entry->executor_id = contract.executor;
    entry->executor_evidence_required = true;
    entry->executor_evidence_accepted =
        entry->opcode == "SBLR_PACKAGE_BEGIN" ||
        entry->opcode == "SBLR_PACKAGE_END";
    entry->missing_executor_evidence_diagnostic = "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";
    return;
  }
}

// MGA-CMO-ADMITTED-MANAGEMENT-ENVELOPE-WIRE-V1.  These rows are exact
// engine-admitted record publishers, not profile fallbacks.  Availability is
// represented by the accepted immutable registry evidence for each identity.
void ApplyIa10ManagementEnvelopeContract(SblrOpcodeEntry* entry) {
  struct Contract { std::string_view opcode; std::string_view executor;
                    std::string_view operand; std::string_view result; };
  static constexpr std::array<Contract, 6> contracts{{
      {"SBLR_MGMT_OPERATION", "engine.op.mgmt_operation", "management_operation_envelope", "management_operation_handle"},
      {"SBLR_MGMT_PAYLOAD", "engine.op.mgmt_payload", "management_payload", "void"},
      {"SBLR_MGMT_RESULT", "engine.op.mgmt_result", "management_result", "management_result"},
      {"SBLR_MGMT_PROGRESS", "engine.op.mgmt_progress", "management_progress", "void"},
      {"SBLR_MGMT_DIAGNOSTIC", "engine.op.mgmt_diagnostic", "diagnostic_vector", "void"},
      {"SBLR_MGMT_METRIC_SNAPSHOT_REF", "engine.op.mgmt_metric_snapshot_ref", "metric_snapshot_ref", "metric_snapshot_ref"},
  }};
  for (const auto& contract : contracts) {
    if (entry->opcode != contract.opcode) continue;
    entry->operation_id = std::string(contract.executor);
    entry->operand_contract = std::string(contract.operand);
    entry->result_contract = std::string(contract.result);
    entry->executor_id = std::string(contract.executor);
    entry->executor_evidence_required = true;
    entry->executor_evidence_accepted = true;
    entry->missing_executor_evidence_diagnostic =
        "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";
    return;
  }
}

// SBLR-TABLE-ANALYZE-ZERO-GREY-V1: this local executor identity is admitted
// only with the exact registry evidence published by its engine-owned
// availability row.  Cluster ownership remains a gateway refusal.
void ApplyTableAnalyzeContract(SblrOpcodeEntry* entry) {
  if (entry->opcode != "SBLR_TABLE_ANALYZE") return;
  entry->operation_id = "engine.op.table_analyze";
  entry->operand_contract = "analyze_table_descriptor";
  entry->result_contract = "mutation_result";
  entry->executor_id = "engine.op.table_analyze";
  entry->executor_evidence_required = true;
  entry->executor_evidence_accepted = true;
  entry->missing_executor_evidence_diagnostic =
      "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";
}

void ApplyBulkImportStreamContract(SblrOpcodeEntry* entry) {
  if (entry->opcode != "SBLR_BULK_IMPORT_STREAM") return;
  entry->operation_id = "engine.op.bulk_import_stream";
  entry->operand_contract = "bulk_import_stream_descriptor";
  entry->result_contract = "bulk_mutation_result";
  entry->executor_id = "engine.op.bulk_import_stream";
  entry->executor_evidence_required = true;
  entry->executor_evidence_accepted = true;
  entry->missing_executor_evidence_diagnostic = "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";
}
void ApplyBulkExportStreamContract(SblrOpcodeEntry* entry) {
  if (entry->opcode != "SBLR_BULK_EXPORT_STREAM") return;
  entry->operation_id = "engine.op.bulk_export_stream";
  entry->operand_contract = "bulk_export_stream_descriptor";
  entry->result_contract = "bulk_read_result";
  entry->executor_id = "engine.op.bulk_export_stream";
  entry->executor_evidence_required = true;
  entry->executor_evidence_accepted = true;
  entry->missing_executor_evidence_diagnostic = "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";
}
void ApplyStatementBatchContract(SblrOpcodeEntry* entry){if(entry->opcode!="SBLR_STATEMENT_BATCH")return;entry->operation_id="engine.op.statement_batch";entry->operand_contract="statement_batch_descriptor";entry->result_contract="batch_result_vector";entry->executor_id="engine.op.statement_batch";entry->executor_evidence_required=true;entry->executor_evidence_accepted=true;entry->missing_executor_evidence_diagnostic="SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";}
void ApplyAtomicCasContract(SblrOpcodeEntry*e){if(e->opcode!="SBLR_ATOMIC_CAS")return;e->operation_id="engine.op.atomic_cas";e->operand_contract="atomic_cas_descriptor";e->result_contract="atomic_cas_result";e->executor_id="engine.op.atomic_cas";e->executor_evidence_required=e->executor_evidence_accepted=true;e->missing_executor_evidence_diagnostic="SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";}
void ApplyAtomicRmwContract(SblrOpcodeEntry*e){if(e->opcode!="SBLR_ATOMIC_READ_MODIFY_WRITE")return;e->operation_id="engine.op.atomic_read_modify_write";e->operand_contract="atomic_rmw_descriptor";e->result_contract="atomic_rmw_result";e->executor_id="engine.op.atomic_read_modify_write";e->executor_evidence_required=e->executor_evidence_accepted=true;e->missing_executor_evidence_diagnostic="SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";}
void ApplyAdvisoryLockContract(SblrOpcodeEntry*e){if(e->opcode!="SBLR_ADVISORY_LOCK_ACQUIRE")return;e->operation_id="engine.op.advisory_lock_acquire";e->operand_contract="advisory_lock_descriptor";e->result_contract="advisory_lock_result";e->executor_id="engine.op.advisory_lock_acquire";e->executor_evidence_required=e->executor_evidence_accepted=true;e->missing_executor_evidence_diagnostic="SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";}
void ApplyAdvisoryLockReleaseContract(SblrOpcodeEntry*e){if(e->opcode!="SBLR_ADVISORY_LOCK_RELEASE")return;e->operation_id="engine.op.advisory_lock_release";e->operand_contract="advisory_lock_release_descriptor";e->result_contract="advisory_lock_result";e->executor_id="engine.op.advisory_lock_release";e->executor_evidence_required=e->executor_evidence_accepted=true;e->missing_executor_evidence_diagnostic="SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";}
void ApplyFunctionCallContract(SblrOpcodeEntry*e){if(e->opcode!="SBLR_FUNCTION_CALL")return;e->operation_id="engine.op.function_call";e->operand_contract="function_call_descriptor";e->result_contract="typed_value";e->executor_id="engine.op.function_call";e->executor_evidence_required=e->executor_evidence_accepted=true;e->missing_executor_evidence_diagnostic="SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";}
void ApplyOperatorCallContract(SblrOpcodeEntry*e){if(e->opcode!="SBLR_OPERATOR_CALL")return;e->operation_id="engine.op.operator_call";e->operand_contract="operator_call_descriptor";e->result_contract="typed_value";e->executor_id="engine.op.operator_call";e->executor_evidence_required=e->executor_evidence_accepted=true;e->missing_executor_evidence_diagnostic="SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";}
void ApplyCastContract(SblrOpcodeEntry*e){if(e->opcode!="SBLR_CAST")return;e->operation_id="engine.op.cast";e->operand_contract="cast_descriptor";e->result_contract="typed_value";e->executor_id="engine.op.cast";e->executor_evidence_required=e->executor_evidence_accepted=true;e->missing_executor_evidence_diagnostic="SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";}
void ApplyCompareContract(SblrOpcodeEntry*e){if(e->opcode!="SBLR_COMPARE")return;e->operation_id="engine.op.compare";e->operand_contract="comparison_descriptor";e->result_contract="boolean_value";e->executor_id="engine.op.compare";e->executor_evidence_required=e->executor_evidence_accepted=true;e->missing_executor_evidence_diagnostic="SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";}
void ApplyDomainOperationContract(SblrOpcodeEntry*e){if(e->opcode!="SBLR_DOMAIN_OPERATION")return;e->operation_id="engine.op.domain_operation";e->operand_contract="domain_operation_descriptor";e->result_contract="typed_value";e->executor_id="engine.op.domain_operation";e->executor_evidence_required=e->executor_evidence_accepted=true;e->missing_executor_evidence_diagnostic="SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";}
void ApplyUdrInvokeContract(SblrOpcodeEntry*e){if(e->opcode!="SBLR_UDR_INVOKE")return;e->operation_id="engine.op.udr_invoke";e->operand_contract="registered_cpp_udr_invocation";e->result_contract="typed_value_or_result_set";e->executor_id="engine.op.udr_invoke";e->executor_evidence_required=e->executor_evidence_accepted=true;e->missing_executor_evidence_diagnostic="SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";}
void ApplyProcedureInvokeContract(SblrOpcodeEntry*e){if(e->opcode!="SBLR_PROCEDURE_INVOKE")return;e->operation_id="engine.op.procedure_invoke";e->operand_contract="procedure_invoke_descriptor";e->result_contract="procedure_result";e->executor_id="engine.op.procedure_invoke";e->executor_evidence_required=e->executor_evidence_accepted=true;e->missing_executor_evidence_diagnostic="SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";}
void ApplyFunctionInvokeContract(SblrOpcodeEntry*e){if(e->opcode!="SBLR_FUNCTION_INVOKE")return;e->operation_id="engine.op.function_invoke";e->operand_contract="function_invoke_descriptor";e->result_contract="typed_value";e->executor_id="engine.op.function_invoke";e->executor_evidence_required=e->executor_evidence_accepted=true;e->missing_executor_evidence_diagnostic="SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";}
void ApplyAggregateInvokeContract(SblrOpcodeEntry*e){if(e->opcode!="SBLR_AGGREGATE_INVOKE")return;e->operation_id="engine.op.aggregate_invoke";e->operand_contract="aggregate_invoke_descriptor";e->result_contract="typed_value";e->executor_id="engine.op.aggregate_invoke";e->executor_evidence_required=e->executor_evidence_accepted=true;e->missing_executor_evidence_diagnostic="SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";}
void ApplySequenceNextvalContract(SblrOpcodeEntry*e){if(e->opcode!="SBLR_SEQUENCE_NEXTVAL")return;e->operation_id="engine.op.sequence_nextval";e->operand_contract="sequence_nextval_descriptor";e->result_contract="typed_value";e->executor_id="engine.op.sequence_nextval";e->executor_evidence_required=e->executor_evidence_accepted=true;e->missing_executor_evidence_diagnostic="SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";}
void ApplySequenceCurrvalContract(SblrOpcodeEntry*e){if(e->opcode!="SBLR_SEQUENCE_CURRVAL")return;e->operation_id="engine.op.sequence_currval";e->operand_contract="sequence_currval_descriptor";e->result_contract="typed_value";e->executor_id="engine.op.sequence_currval";e->executor_evidence_required=e->executor_evidence_accepted=true;e->missing_executor_evidence_diagnostic="SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";}
void ApplySequenceSetvalContract(SblrOpcodeEntry*e){if(e->opcode!="SBLR_SEQUENCE_SETVAL")return;e->operation_id="engine.op.sequence_setval";e->operand_contract="sequence_setval_descriptor";e->result_contract="typed_value";e->executor_id="engine.op.sequence_setval";e->executor_evidence_required=e->executor_evidence_accepted=true;e->missing_executor_evidence_diagnostic="SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";}
void ApplyQueryNumericContract(SblrOpcodeEntry*e){if(e->opcode!="SBLR_QUERY_APPLY_NUMERIC_OPERATION")return;e->operation_id="engine.op.query_apply_numeric_operation";e->operand_contract="numeric_descriptor_and_operand_values";e->result_contract="typed_value";e->executor_id="engine.op.query_apply_numeric_operation";e->executor_evidence_required=e->executor_evidence_accepted=true;e->missing_executor_evidence_diagnostic="SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";}
void ApplyAdvancedDatatypeFamilyContract(SblrOpcodeEntry*e){if(e->opcode!="SBLR_QUERY_EVALUATE_ADVANCED_DATATYPE_FAMILY")return;e->operation_id="engine.op.query_evaluate_advanced_datatype_family";e->operand_contract="advanced_family_descriptor_operation_index_profile";e->result_contract="datatype_family_evaluation";e->executor_id="engine.op.query_evaluate_advanced_datatype_family";e->executor_evidence_required=e->executor_evidence_accepted=true;e->missing_executor_evidence_diagnostic="SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";}
void ApplySecurityAlterPrivilegeTemplateContract(SblrOpcodeEntry*e){if(e->opcode!="SBLR_SECURITY_ALTER_PRIVILEGE_TEMPLATE")return;e->operation_id="engine.op.security_alter_privilege_template";e->operand_contract="privilege_template_alter_descriptor";e->result_contract="ddl_result";e->executor_id="engine.op.security_alter_privilege_template";e->executor_evidence_required=e->executor_evidence_accepted=true;e->missing_executor_evidence_diagnostic="SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";}
void ApplySecurityDropPrivilegeTemplateContract(SblrOpcodeEntry*e){if(e->opcode!="SBLR_SECURITY_DROP_PRIVILEGE_TEMPLATE")return;e->operation_id="engine.op.security_drop_privilege_template";e->operand_contract="privilege_template_drop_descriptor";e->result_contract="ddl_result";e->executor_id="engine.op.security_drop_privilege_template";e->executor_evidence_required=e->executor_evidence_accepted=true;e->missing_executor_evidence_diagnostic="SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";}
void ApplyDatabaseCreateTemplateCloneContract(SblrOpcodeEntry*e){if(e->opcode!="SBLR_DATABASE_CREATE_TEMPLATE_CLONE")return;e->operation_id="engine.op.database_create_template_clone";e->operand_contract="template_database_creation_descriptor";e->result_contract="management_operation_result";e->executor_id="engine.op.database_create_template_clone";e->executor_evidence_required=e->executor_evidence_accepted=true;e->missing_executor_evidence_diagnostic="SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";}
void ApplyDdlCreateAggregateContract(SblrOpcodeEntry*e){if(e->opcode!="SBLR_DDL_CREATE_AGGREGATE")return;e->operation_id="engine.op.ddl_create_aggregate";e->operand_contract="aggregate_descriptor";e->result_contract="ddl_result";e->executor_id="engine.op.ddl_create_aggregate";e->executor_evidence_required=e->executor_evidence_accepted=true;e->missing_executor_evidence_diagnostic="SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";}
void ApplyDdlAlterAggregateContract(SblrOpcodeEntry*e){if(e->opcode!="SBLR_DDL_ALTER_AGGREGATE")return;e->operation_id="engine.op.ddl_alter_aggregate";e->operand_contract="aggregate_alter_descriptor";e->result_contract="ddl_result";e->executor_id="engine.op.ddl_alter_aggregate";e->executor_evidence_required=e->executor_evidence_accepted=true;e->missing_executor_evidence_diagnostic="SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";}
void ApplyDdlDropAggregateContract(SblrOpcodeEntry*e){if(e->opcode!="SBLR_DDL_DROP_AGGREGATE")return;e->operation_id="engine.op.ddl_drop_aggregate";e->operand_contract="aggregate_drop_descriptor";e->result_contract="ddl_result";e->executor_id="engine.op.ddl_drop_aggregate";e->executor_evidence_required=e->executor_evidence_accepted=true;e->missing_executor_evidence_diagnostic="SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";}
void ApplyDdlPurgeSystemHistoryContract(SblrOpcodeEntry*e){if(e->opcode!="SBLR_DDL_PURGE_SYSTEM_HISTORY")return;e->operation_id="engine.op.ddl_purge_system_history";e->operand_contract="system_history_purge_descriptor";e->result_contract="management_operation_result";e->executor_id="engine.op.ddl_purge_system_history";e->executor_evidence_required=e->executor_evidence_accepted=true;e->missing_executor_evidence_diagnostic="SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";}
void ApplyDdlSetIndexOptimizerEligibilityContract(SblrOpcodeEntry*e){if(e->opcode!="SBLR_DDL_SET_INDEX_OPTIMIZER_ELIGIBILITY")return;e->operation_id="engine.op.ddl_set_index_optimizer_eligibility";e->operand_contract="index_optimizer_eligibility_descriptor";e->result_contract="ddl_result";e->executor_id="engine.op.ddl_set_index_optimizer_eligibility";e->executor_evidence_required=e->executor_evidence_accepted=true;e->missing_executor_evidence_diagnostic="SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";}
void ApplyDdlSetTableTypeEnforcementContract(SblrOpcodeEntry*e){if(e->opcode!="SBLR_DDL_SET_TABLE_TYPE_ENFORCEMENT")return;e->operation_id="engine.op.ddl_set_table_type_enforcement";e->operand_contract="table_type_enforcement_descriptor";e->result_contract="management_operation_result";e->executor_id="engine.op.ddl_set_table_type_enforcement";e->executor_evidence_required=e->executor_evidence_accepted=true;e->missing_executor_evidence_diagnostic="SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";}
void ApplyDatabaseSerializeLogicalSnapshotContract(SblrOpcodeEntry*e){if(e->opcode!="SBLR_DATABASE_SERIALIZE_LOGICAL_SNAPSHOT")return;e->operation_id="engine.op.database_serialize_logical_snapshot";e->operand_contract="logical_snapshot_serialization_descriptor";e->result_contract="logical_snapshot_buffer_descriptor";e->executor_id="engine.op.database_serialize_logical_snapshot";e->executor_evidence_required=e->executor_evidence_accepted=true;e->missing_executor_evidence_diagnostic="SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";}
void ApplyDatabaseDeserializeLogicalSnapshotContract(SblrOpcodeEntry*e){if(e->opcode!="SBLR_DATABASE_DESERIALIZE_LOGICAL_SNAPSHOT")return;e->operation_id="engine.op.database_deserialize_logical_snapshot";e->operand_contract="logical_snapshot_deserialization_descriptor";e->result_contract="management_operation_result";e->executor_id="engine.op.database_deserialize_logical_snapshot";e->executor_evidence_required=e->executor_evidence_accepted=true;e->missing_executor_evidence_diagnostic="SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";}
void ApplyDdlCreateMacroContract(SblrOpcodeEntry*e){if(e->opcode!="SBLR_DDL_CREATE_MACRO")return;e->operation_id="engine.op.ddl_create_macro";e->operand_contract="macro_descriptor";e->result_contract="ddl_result";e->executor_id="engine.op.ddl_create_macro";e->executor_evidence_required=e->executor_evidence_accepted=true;e->missing_executor_evidence_diagnostic="SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";}
void ApplyDdlDropMacroContract(SblrOpcodeEntry*e){if(e->opcode!="SBLR_DDL_DROP_MACRO")return;e->operation_id="engine.op.ddl_drop_macro";e->operand_contract="macro_drop_descriptor";e->result_contract="ddl_result";e->executor_id="engine.op.ddl_drop_macro";e->executor_evidence_required=e->executor_evidence_accepted=true;e->missing_executor_evidence_diagnostic="SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";}
void ApplyAdminRegisterExternalRelationResolverContract(SblrOpcodeEntry*e){if(e->opcode!="SBLR_ADMIN_REGISTER_EXTERNAL_RELATION_RESOLVER")return;e->operation_id="engine.op.admin_register_external_relation_resolver";e->operand_contract="external_relation_resolver_registration_descriptor";e->result_contract="management_operation_result";e->executor_id="engine.op.admin_register_external_relation_resolver";e->executor_evidence_required=e->executor_evidence_accepted=true;e->missing_executor_evidence_diagnostic="SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";}
void ApplyAdminUnregisterExternalRelationResolverContract(SblrOpcodeEntry*e){if(e->opcode!="SBLR_ADMIN_UNREGISTER_EXTERNAL_RELATION_RESOLVER")return;e->operation_id="engine.op.admin_unregister_external_relation_resolver";e->operand_contract="external_relation_resolver_unregistration_descriptor";e->result_contract="management_operation_result";e->executor_id="engine.op.admin_unregister_external_relation_resolver";e->executor_evidence_required=e->executor_evidence_accepted=true;e->missing_executor_evidence_diagnostic="SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";}
void ApplyDdlCreateDictionaryContract(SblrOpcodeEntry*e){if(e->opcode!="SBLR_DDL_CREATE_DICTIONARY")return;e->operation_id="engine.op.ddl_create_dictionary";e->operand_contract="external_dictionary_descriptor";e->result_contract="ddl_result";e->executor_id="engine.op.ddl_create_dictionary";e->executor_evidence_required=e->executor_evidence_accepted=true;e->missing_executor_evidence_diagnostic="SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";}
void ApplyDdlDropDictionaryContract(SblrOpcodeEntry*e){if(e->opcode!="SBLR_DDL_DROP_DICTIONARY")return;e->operation_id="engine.op.ddl_drop_dictionary";e->operand_contract="external_dictionary_drop_descriptor";e->result_contract="ddl_result";e->executor_id="engine.op.ddl_drop_dictionary";e->executor_evidence_required=e->executor_evidence_accepted=true;e->missing_executor_evidence_diagnostic="SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";}
void ApplyDdlAlterDictionaryContract(SblrOpcodeEntry*e){if(e->opcode!="SBLR_DDL_ALTER_DICTIONARY")return;e->operation_id="engine.op.ddl_alter_dictionary";e->operand_contract="external_dictionary_alter_descriptor";e->result_contract="ddl_result";e->executor_id="engine.op.ddl_alter_dictionary";e->executor_evidence_required=e->executor_evidence_accepted=true;e->missing_executor_evidence_diagnostic="SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";}
void ApplyDdlCreateContinuousViewContract(SblrOpcodeEntry*e){if(e->opcode!="SBLR_DDL_CREATE_CONTINUOUS_VIEW")return;e->operation_id="engine.op.ddl_create_continuous_view";e->operand_contract="continuous_view_descriptor";e->result_contract="ddl_result";e->executor_id="engine.op.ddl_create_continuous_view";e->executor_evidence_required=e->executor_evidence_accepted=true;e->missing_executor_evidence_diagnostic="SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";}
void ApplyDdlAlterContinuousViewContract(SblrOpcodeEntry*e){if(e->opcode!="SBLR_DDL_ALTER_CONTINUOUS_VIEW")return;e->operation_id="engine.op.ddl_alter_continuous_view";e->operand_contract="continuous_view_alter_descriptor";e->result_contract="ddl_result";e->executor_id="engine.op.ddl_alter_continuous_view";e->executor_evidence_required=e->executor_evidence_accepted=true;e->missing_executor_evidence_diagnostic="SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";}
void ApplyDdlDropContinuousViewContract(SblrOpcodeEntry*e){if(e->opcode!="SBLR_DDL_DROP_CONTINUOUS_VIEW")return;e->operation_id="engine.op.ddl_drop_continuous_view";e->operand_contract="continuous_view_drop_descriptor";e->result_contract="ddl_result";e->executor_id="engine.op.ddl_drop_continuous_view";e->executor_evidence_required=e->executor_evidence_accepted=true;e->missing_executor_evidence_diagnostic="SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";}
void ApplyDmlAsyncInsertSubmitContract(SblrOpcodeEntry*e){if(e->opcode!="SBLR_DML_ASYNC_INSERT_SUBMIT")return;e->operation_id="engine.op.dml_async_insert_submit";e->operand_contract="async_insert_submission_descriptor";e->result_contract="async_insert_operation_descriptor";e->executor_id="engine.op.dml_async_insert_submit";e->executor_evidence_required=e->executor_evidence_accepted=true;e->missing_executor_evidence_diagnostic="SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";}
void ApplyDmlAsyncInsertStatusContract(SblrOpcodeEntry*e){if(e->opcode!="SBLR_DML_ASYNC_INSERT_STATUS")return;e->operation_id="engine.op.dml_async_insert_status";e->operand_contract="async_insert_status_descriptor";e->result_contract="async_insert_operation_descriptor";e->executor_id="engine.op.dml_async_insert_status";e->executor_evidence_required=e->executor_evidence_accepted=true;e->missing_executor_evidence_diagnostic="SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";}
void ApplyDmlAsyncInsertCancelContract(SblrOpcodeEntry*e){if(e->opcode!="SBLR_DML_ASYNC_INSERT_CANCEL")return;e->operation_id="engine.op.dml_async_insert_cancel";e->operand_contract="async_insert_cancel_descriptor";e->result_contract="async_insert_operation_descriptor";e->executor_id="engine.op.dml_async_insert_cancel";e->executor_evidence_required=e->executor_evidence_accepted=true;e->missing_executor_evidence_diagnostic="SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";}
void ApplyDmlConditionalMutateContract(SblrOpcodeEntry*e){if(e->opcode!="SBLR_DML_CONDITIONAL_MUTATE")return;e->operation_id="engine.op.dml_conditional_mutate";e->operand_contract="conditional_mutation_descriptor";e->result_contract="conditional_mutation_result";e->executor_id="engine.op.dml_conditional_mutate";e->executor_evidence_required=e->executor_evidence_accepted=true;e->missing_executor_evidence_diagnostic="SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";}
void ApplyDmlCounterAddContract(SblrOpcodeEntry*e){if(e->opcode!="SBLR_DML_COUNTER_ADD")return;e->operation_id="engine.op.dml_counter_add";e->operand_contract="counter_delta_descriptor";e->result_contract="counter_result";e->executor_id="engine.op.dml_counter_add";e->executor_evidence_required=e->executor_evidence_accepted=true;e->missing_executor_evidence_diagnostic="SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";}
void ApplyDmlTimeseriesSchemaWriteContract(SblrOpcodeEntry*e){if(e->opcode!="SBLR_DML_TIMESERIES_SCHEMA_WRITE")return;e->operation_id="engine.op.dml_timeseries_schema_write";e->operand_contract="timeseries_schema_write_descriptor";e->result_contract="timeseries_write_result";e->executor_id="engine.op.dml_timeseries_schema_write";e->executor_evidence_required=e->executor_evidence_accepted=true;e->missing_executor_evidence_diagnostic="SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";}
void ApplyDdlTimeseriesSeriesCardinalityPolicyContract(SblrOpcodeEntry*e){if(e->opcode!="SBLR_DDL_SET_TIMESERIES_SERIES_CARDINALITY_POLICY")return;e->operation_id="engine.op.ddl_set_timeseries_series_cardinality_policy";e->operand_contract="timeseries_series_cardinality_policy_descriptor";e->result_contract="ddl_result";e->executor_id="engine.op.ddl_set_timeseries_series_cardinality_policy";e->executor_evidence_required=e->executor_evidence_accepted=true;e->missing_executor_evidence_diagnostic="SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";}
void ApplyDdlCreateTimeseriesValueCacheContract(SblrOpcodeEntry*e){if(e->opcode!="SBLR_DDL_CREATE_TIMESERIES_VALUE_CACHE")return;e->operation_id="engine.op.ddl_create_timeseries_value_cache";e->operand_contract="timeseries_value_cache_descriptor";e->result_contract="ddl_result";e->executor_id="engine.op.ddl_create_timeseries_value_cache";e->executor_evidence_required=e->executor_evidence_accepted=true;e->missing_executor_evidence_diagnostic="SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";}

// IA10B-LOCAL-METRICS-READ-WIRE-V1.  This is the one local observation
// reader admitted by the Core closure; it is deliberately distinct from all
// cluster metric surfaces and from the generic observability display route.
void ApplyIa10BLocalMetricsReadContract(SblrOpcodeEntry* entry) {
  if (entry->opcode != "SBLR_READ_METRICS") return;
  entry->operation_id = "engine.op.read_metrics";
  entry->operand_contract = "metrics_read_request";
  entry->result_contract = "metrics_result_set";
  entry->executor_id = "engine.op.read_metrics";
  entry->executor_evidence_required = true;
  entry->executor_evidence_accepted = true;
  entry->missing_executor_evidence_diagnostic =
      "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";
}

// IA10C-EVENT-NOTIFICATION-WIRE-V1: exactly the ten SBEN event identities.
void ApplyIa10CEventNotificationContract(SblrOpcodeEntry* entry) {
  struct Contract { std::string_view opcode, executor, operand, result; bool transaction; };
  static constexpr std::array<Contract, 10> contracts{{
      {"SBLR_EVENT_CHANNEL_CREATE", "engine.op.event_channel_create", "event_channel_create_request", "event_channel_result", true},
      {"SBLR_EVENT_CHANNEL_ALTER", "engine.op.event_channel_alter", "event_channel_alter_request", "event_channel_result", true},
      {"SBLR_EVENT_CHANNEL_DROP", "engine.op.event_channel_drop", "event_channel_drop_request", "event_channel_result", true},
      {"SBLR_EVENT_CHANNEL_LISTEN", "engine.op.event_channel_listen", "event_channel_listen_request", "event_subscription_result", true},
      {"SBLR_EVENT_CHANNEL_UNLISTEN", "engine.op.event_channel_unlisten", "event_channel_unlisten_request", "event_subscription_result", true},
      {"SBLR_EVENT_CHANNEL_UNLISTEN_ALL", "engine.op.event_channel_unlisten_all", "event_session_unlisten_all_request", "event_subscription_result", true},
      {"SBLR_EVENT_CHANNEL_NOTIFY", "engine.op.event_channel_notify", "event_channel_notify_request", "event_publication_result", true},
      {"SBLR_EVENT_SUBSCRIPTION_LIST", "engine.op.event_subscription_list", "event_subscription_list_request", "event_subscription_list_result", false},
      {"SBLR_EVENT_DELIVERY_POLL", "engine.op.event_delivery_poll", "event_delivery_poll_request", "event_delivery_result", false},
      {"SBLR_EVENT_DELIVERY_ACK", "engine.op.event_delivery_ack", "event_delivery_ack_request", "event_delivery_ack_result", false},
  }};
  for (const auto& contract : contracts) {
    if (entry->opcode != contract.opcode) continue;
    entry->operation_id = std::string(contract.executor);
    entry->operand_contract = std::string(contract.operand);
    entry->result_contract = std::string(contract.result);
    entry->executor_id = std::string(contract.executor);
    entry->requires_transaction_context = contract.transaction;
    entry->executor_evidence_required = true;
    entry->executor_evidence_accepted = true;
    entry->missing_executor_evidence_diagnostic = "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";
    return;
  }
}

void ApplyIa11LocalBackupArchiveContract(SblrOpcodeEntry* entry) {
  struct Contract { std::string_view opcode, executor, operand, result; bool transaction; };
  static constexpr std::array<Contract, 5> contracts{{
      {"SBLR_BACKUP_START","engine.op.backup_start","backup_start_request","backup_operation_result",true},
      {"SBLR_BACKUP_FINISH","engine.op.backup_finish","backup_finish_request","backup_operation_result",true},
      {"SBLR_RESTORE_BACKUP","engine.op.restore_backup","restore_request","restore_result",true},
      {"SBLR_ARCHIVE_EXPORT","engine.op.archive_export","archive_export_request","archive_result",true},
      {"SBLR_ARCHIVE_VERIFY","engine.op.archive_verify","archive_verify_request","archive_verify_result",false},
  }};
  for (const auto& c : contracts) if (entry->opcode == c.opcode) { entry->operation_id=std::string(c.executor); entry->operand_contract=std::string(c.operand); entry->result_contract=std::string(c.result); entry->executor_id=std::string(c.executor); entry->requires_transaction_context=c.transaction; entry->executor_evidence_required=true; entry->executor_evidence_accepted=true; entry->missing_executor_evidence_diagnostic="SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING"; return; }
}

SblrOpcodeEntry CanonicalEntry(std::string operation_id,
                               std::string opcode,
                               std::string family,
                               SblrOpcodeCategory category,
                               SblrOpcodeSupport support,
                               SblrOpcodeTransactionEffect transaction_effect,
                               SblrOpcodeSecurityClass security_class,
                               bool requires_transaction_context,
                               bool requires_cluster_authority = false,
                               std::string refusal_diagnostic = {}) {
  auto entry = Entry(std::move(operation_id),
                     std::move(opcode),
                     category,
                     support,
                     true,
                     requires_transaction_context,
                     requires_cluster_authority,
                     std::move(refusal_diagnostic));
  entry.family = std::move(family);
  entry.transaction_effect = transaction_effect;
  entry.security_class = security_class;
  entry.cluster_private = requires_cluster_authority;
  ApplyIa01SemanticContract(&entry);
  ApplyTableAnalyzeContract(&entry);
  ApplyBulkImportStreamContract(&entry);
  ApplyBulkExportStreamContract(&entry);
  ApplySecurityAlterPrivilegeTemplateContract(&entry);
  ApplySecurityDropPrivilegeTemplateContract(&entry);
  ApplyDatabaseCreateTemplateCloneContract(&entry);
  ApplyDdlCreateAggregateContract(&entry);
  ApplyDdlAlterAggregateContract(&entry);
  ApplyDdlDropAggregateContract(&entry);
  ApplyDdlPurgeSystemHistoryContract(&entry);
  ApplyDdlSetIndexOptimizerEligibilityContract(&entry);
  ApplyDdlSetTableTypeEnforcementContract(&entry);
  ApplyDatabaseSerializeLogicalSnapshotContract(&entry);
  ApplyDatabaseDeserializeLogicalSnapshotContract(&entry);
  ApplyDdlCreateMacroContract(&entry);
  ApplyDdlDropMacroContract(&entry);
  ApplyAdminRegisterExternalRelationResolverContract(&entry);
  ApplyAdminUnregisterExternalRelationResolverContract(&entry);
  ApplyDdlCreateDictionaryContract(&entry);
  ApplyDdlDropDictionaryContract(&entry);
  ApplyDdlAlterDictionaryContract(&entry);
  ApplyDdlCreateContinuousViewContract(&entry);
  ApplyDdlAlterContinuousViewContract(&entry);
  ApplyDdlDropContinuousViewContract(&entry);
  ApplyDmlAsyncInsertSubmitContract(&entry);
  ApplyDmlAsyncInsertStatusContract(&entry);
  ApplyDmlAsyncInsertCancelContract(&entry);
  ApplyDmlConditionalMutateContract(&entry);
  ApplyDmlCounterAddContract(&entry);
  ApplyDmlTimeseriesSchemaWriteContract(&entry);
  ApplyDdlTimeseriesSeriesCardinalityPolicyContract(&entry);
  ApplyDdlCreateTimeseriesValueCacheContract(&entry);
  ApplyStatementBatchContract(&entry);
  ApplyAtomicCasContract(&entry);
  ApplyAtomicRmwContract(&entry);
  ApplyAdvisoryLockContract(&entry);
  ApplyAdvisoryLockReleaseContract(&entry);
  ApplyFunctionCallContract(&entry);
  ApplyOperatorCallContract(&entry);
  ApplyCastContract(&entry);
  ApplyCompareContract(&entry);
  ApplyDomainOperationContract(&entry);
  ApplyUdrInvokeContract(&entry);
  ApplyProcedureInvokeContract(&entry);
  ApplyFunctionInvokeContract(&entry);
  ApplyAggregateInvokeContract(&entry);
  ApplySequenceNextvalContract(&entry);
  ApplySequenceCurrvalContract(&entry);
  ApplySequenceSetvalContract(&entry);
  ApplyQueryNumericContract(&entry);
  ApplyAdvancedDatatypeFamilyContract(&entry);
  ApplyIa10ManagementEnvelopeContract(&entry);
  ApplyIa10BLocalMetricsReadContract(&entry);
  ApplyIa10CEventNotificationContract(&entry);
  ApplyIa11LocalBackupArchiveContract(&entry);
  return entry;
}

}  // namespace

const std::vector<SblrOpcodeEntry>& StaticSblrOpcodeRegistry() {
  static const std::vector<SblrOpcodeEntry> registry = {
      CanonicalEntry("envelope.package_begin", "SBLR_PACKAGE_BEGIN", "core-envelope", SblrOpcodeCategory::core, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::none, SblrOpcodeSecurityClass::authenticated, false),
      CanonicalEntry("envelope.package_end", "SBLR_PACKAGE_END", "core-envelope", SblrOpcodeCategory::core, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::none, SblrOpcodeSecurityClass::authenticated, false),
      CanonicalEntry("expression.literal", "SBLR_LITERAL", "core-envelope", SblrOpcodeCategory::core, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::none, SblrOpcodeSecurityClass::authenticated, false),
      CanonicalEntry("expression.parameter", "SBLR_PARAMETER", "core-envelope", SblrOpcodeCategory::core, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::none, SblrOpcodeSecurityClass::authenticated, false),
      CanonicalEntry("expression.variable", "SBLR_VARIABLE", "core-envelope", SblrOpcodeCategory::core, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::none, SblrOpcodeSecurityClass::authenticated, false),
      CanonicalEntry("envelope.source_map", "SBLR_SOURCE_MAP", "core-envelope", SblrOpcodeCategory::core, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::none, SblrOpcodeSecurityClass::authenticated, false),
      CanonicalEntry("diagnostic.error_vector", "SBLR_ERROR_VECTOR", "core-envelope", SblrOpcodeCategory::core, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::none, SblrOpcodeSecurityClass::authenticated, false),
      CanonicalEntry("engine.op.txn_begin", "SBLR_TXN_BEGIN", "transaction-control", SblrOpcodeCategory::transaction, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::authenticated, false),
      CanonicalEntry("engine.op.txn_commit", "SBLR_TXN_COMMIT", "transaction-control", SblrOpcodeCategory::transaction, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::authenticated, true),
      CanonicalEntry("engine.op.txn_rollback", "SBLR_TXN_ROLLBACK", "transaction-control", SblrOpcodeCategory::transaction, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::authenticated, true),
      CanonicalEntry("engine.op.txn_savepoint", "SBLR_TXN_SAVEPOINT", "transaction-control", SblrOpcodeCategory::transaction, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::authenticated, true),
      CanonicalEntry("engine.op.txn_release_savepoint", "SBLR_TXN_RELEASE_SAVEPOINT", "transaction-control", SblrOpcodeCategory::transaction, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::authenticated, true),
      CanonicalEntry("engine.op.txn_rollback_to_savepoint", "SBLR_TXN_ROLLBACK_TO_SAVEPOINT", "transaction-control", SblrOpcodeCategory::transaction, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::authenticated, true),
      CanonicalEntry("engine.op.psql_autonomous_frame", "SBLR_PSQL_AUTONOMOUS_FRAME", "transaction-control", SblrOpcodeCategory::transaction, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("engine.op.transaction_reservation_release", "SBLR_TRANSACTION_RESERVATION_RELEASE", "transaction-control", SblrOpcodeCategory::transaction, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("engine.op.temporary_instance_cleanup", "SBLR_TEMPORARY_INSTANCE_CLEANUP", "transaction-control", SblrOpcodeCategory::transaction, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("transaction.lock_table", "SBLR_TXN_LOCK_TABLE", "transaction-control", SblrOpcodeCategory::transaction, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::authenticated, true),
      CanonicalEntry("transaction.unlock_table", "SBLR_TXN_UNLOCK_TABLE", "transaction-control", SblrOpcodeCategory::transaction, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::authenticated, true),
      CanonicalEntry("transaction.lock_named", "SBLR_TXN_LOCK_NAMED", "transaction-control", SblrOpcodeCategory::transaction, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::authenticated, true),
      CanonicalEntry("transaction.unlock_named", "SBLR_TXN_UNLOCK_NAMED", "transaction-control", SblrOpcodeCategory::transaction, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::authenticated, true),
      CanonicalEntry("op.migration.begin_from_reference", "SBLR_MIGRATION_BEGIN_FROM_REFERENCE", "migration-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("op.migration.alter", "SBLR_MIGRATION_ALTER", "migration-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("op.show.migration", "SBLR_SHOW_MIGRATION", "migration-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("op.show.migrations", "SBLR_SHOW_MIGRATIONS", "migration-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("engine.op.cursor_open", "SBLR_CURSOR_OPEN", "data-read", SblrOpcodeCategory::data_read, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("engine.op.cursor_fetch", "SBLR_CURSOR_FETCH", "data-read", SblrOpcodeCategory::data_read, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("engine.op.cursor_close", "SBLR_CURSOR_CLOSE", "data-read", SblrOpcodeCategory::data_read, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("engine.op.read_by_key", "SBLR_READ_BY_KEY", "data-read", SblrOpcodeCategory::data_read, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("engine.op.read_range", "SBLR_READ_RANGE", "data-read", SblrOpcodeCategory::data_read, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("engine.op.read_stream", "SBLR_READ_STREAM", "data-read", SblrOpcodeCategory::data_read, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("engine.op.result_set_pass", "SBLR_RESULT_SET_PASS", "data-read", SblrOpcodeCategory::data_read, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("engine.op.access_cursor_open", "SBLR_ACCESS_CURSOR_OPEN", "data-read", SblrOpcodeCategory::data_read, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("engine.op.access_cursor_fetch", "SBLR_ACCESS_CURSOR_FETCH", "data-read", SblrOpcodeCategory::data_read, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("engine.op.access_cursor_close", "SBLR_ACCESS_CURSOR_CLOSE", "data-read", SblrOpcodeCategory::data_read, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("engine.op.catalog_introspect", "SBLR_CATALOG_INTROSPECT", "catalog-introspect", SblrOpcodeCategory::data_read, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("engine.op.insert", "SBLR_INSERT", "data-mutation", SblrOpcodeCategory::data_mutation, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("engine.op.update", "SBLR_UPDATE", "data-mutation", SblrOpcodeCategory::data_mutation, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("engine.op.delete", "SBLR_DELETE", "data-mutation", SblrOpcodeCategory::data_mutation, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("engine.op.merge", "SBLR_MERGE", "data-mutation", SblrOpcodeCategory::data_mutation, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("cluster.write_admission", "SBLR_CLUSTER_WRITE_ADMISSION", "data-mutation", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, SblrOpcodeTransactionEffect::cluster_write, SblrOpcodeSecurityClass::cluster_authorized, true, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      CanonicalEntry("bridge.describe_capabilities", "SBLR_BRIDGE_DESCRIBE_CAPABILITIES", "bridge-universal-abi", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, false),
      CanonicalEntry("bridge.connect", "SBLR_BRIDGE_OPEN_CHANNEL", "bridge-universal-abi", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::object_authorized, false),
      CanonicalEntry("bridge.attach", "SBLR_BRIDGE_OPEN_CHANNEL", "bridge-universal-abi", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::object_authorized, false),
      CanonicalEntry("bridge.authenticate", "SBLR_BRIDGE_AUTHENTICATE", "bridge-universal-abi", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::security, SblrOpcodeSecurityClass::object_authorized, false),
      CanonicalEntry("bridge.open_session", "SBLR_BRIDGE_OPEN_SESSION", "bridge-universal-abi", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::object_authorized, false),
      CanonicalEntry("bridge.close_session", "SBLR_BRIDGE_CLOSE_SESSION", "bridge-universal-abi", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::object_authorized, false),
      CanonicalEntry("bridge.detach", "SBLR_BRIDGE_CLOSE_SESSION", "bridge-universal-abi", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::object_authorized, false),
      CanonicalEntry("bridge.ping", "SBLR_BRIDGE_HEALTH", "bridge-universal-abi", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, false),
      CanonicalEntry("bridge.health", "SBLR_BRIDGE_HEALTH", "bridge-universal-abi", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, false),
      CanonicalEntry("bridge.cancel", "SBLR_BRIDGE_CANCEL", "bridge-universal-abi", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::object_authorized, false),
      CanonicalEntry("bridge.drain", "SBLR_BRIDGE_DRAIN", "bridge-universal-abi", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::object_authorized, false),
      CanonicalEntry("bridge.shutdown", "SBLR_BRIDGE_DRAIN", "bridge-universal-abi", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::object_authorized, false),
      CanonicalEntry("bridge.begin", "SBLR_BRIDGE_TX_BEGIN", "bridge-universal-abi", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("bridge.commit", "SBLR_BRIDGE_TX_COMMIT", "bridge-universal-abi", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("bridge.rollback", "SBLR_BRIDGE_TX_ROLLBACK", "bridge-universal-abi", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("bridge.prepare", "SBLR_BRIDGE_TX_PREPARE", "bridge-universal-abi", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("bridge.savepoint", "SBLR_BRIDGE_TX_SAVEPOINT", "bridge-universal-abi", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("bridge.execute", "SBLR_BRIDGE_EXECUTE", "bridge-universal-abi", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("bridge.cursor_open", "SBLR_BRIDGE_CURSOR_OPEN", "bridge-universal-abi", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("bridge.cursor_fetch", "SBLR_BRIDGE_CURSOR_FETCH", "bridge-universal-abi", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("bridge.cursor_close", "SBLR_BRIDGE_CURSOR_CLOSE", "bridge-universal-abi", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("bridge.stream_open", "SBLR_BRIDGE_STREAM_OPEN", "bridge-universal-abi", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("bridge.stream_read", "SBLR_BRIDGE_STREAM_READ", "bridge-universal-abi", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("bridge.stream_write", "SBLR_BRIDGE_STREAM_WRITE", "bridge-universal-abi", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("bridge.stream_close", "SBLR_BRIDGE_STREAM_CLOSE", "bridge-universal-abi", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("bridge.cdc_start", "SBLR_BRIDGE_CDC_START", "bridge-universal-abi", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("bridge.cdc_read", "SBLR_BRIDGE_CDC_READ", "bridge-universal-abi", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("bridge.cdc_apply", "SBLR_BRIDGE_CDC_APPLY", "bridge-universal-abi", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("bridge.proxy_route", "SBLR_BRIDGE_PROXY_ROUTE", "bridge-universal-abi", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("bridge.compare_result", "SBLR_BRIDGE_COMPARE_RESULT", "bridge-universal-abi", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("bridge.cutover", "SBLR_BRIDGE_CUTOVER", "bridge-universal-abi", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("bridge.validate", "SBLR_BRIDGE_VALIDATE", "bridge-universal-abi", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, false),
      CanonicalEntry("bridge.cluster_route", "SBLR_BRIDGE_VALIDATE", "bridge-universal-abi", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, SblrOpcodeTransactionEffect::cluster_write, SblrOpcodeSecurityClass::cluster_authorized, true, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      CanonicalEntry("bridge.cluster.distributed_query", "SBLR_BRIDGE_VALIDATE", "bridge-universal-abi", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::cluster_authorized, true, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      CanonicalEntry("bridge.cluster.cross_node_query", "SBLR_BRIDGE_VALIDATE", "bridge-universal-abi", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::cluster_authorized, true, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      CanonicalEntry("engine.op.table_truncate", "SBLR_TABLE_TRUNCATE", "data-mutation", SblrOpcodeCategory::data_mutation, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("engine.op.table_analyze", "SBLR_TABLE_ANALYZE", "data-mutation", SblrOpcodeCategory::data_mutation, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("engine.op.bulk_import_stream", "SBLR_BULK_IMPORT_STREAM", "data-mutation", SblrOpcodeCategory::data_mutation, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("dml.execute_native_bulk_ingest", "SBLR_DML_EXECUTE_NATIVE_BULK_INGEST", "data-mutation", SblrOpcodeCategory::data_mutation, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("engine.op.bulk_export_stream", "SBLR_BULK_EXPORT_STREAM", "data-mutation", SblrOpcodeCategory::data_mutation, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("engine.op.statement_batch", "SBLR_STATEMENT_BATCH", "data-mutation", SblrOpcodeCategory::data_mutation, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("engine.op.atomic_cas", "SBLR_ATOMIC_CAS", "data-mutation", SblrOpcodeCategory::data_mutation, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("engine.op.atomic_read_modify_write", "SBLR_ATOMIC_READ_MODIFY_WRITE", "data-mutation", SblrOpcodeCategory::data_mutation, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("engine.op.advisory_lock_acquire", "SBLR_ADVISORY_LOCK_ACQUIRE", "data-mutation", SblrOpcodeCategory::data_mutation, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("engine.op.advisory_lock_release", "SBLR_ADVISORY_LOCK_RELEASE", "data-mutation", SblrOpcodeCategory::data_mutation, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("engine.op.function_call", "SBLR_FUNCTION_CALL", "expression-eval", SblrOpcodeCategory::expression, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, false),
      CanonicalEntry("expression.system_variable_read", "SBLR_SYSTEM_VARIABLE_READ", "expression-eval", SblrOpcodeCategory::expression, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::authenticated, false),
      CanonicalEntry("engine.op.operator_call", "SBLR_OPERATOR_CALL", "expression-eval", SblrOpcodeCategory::expression, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, false),
      CanonicalEntry("engine.op.cast", "SBLR_CAST", "expression-eval", SblrOpcodeCategory::expression, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, false),
      CanonicalEntry("expression.compare", "SBLR_COMPARE", "expression-eval", SblrOpcodeCategory::expression, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("domain.operation", "SBLR_DOMAIN_OPERATION", "expression-eval", SblrOpcodeCategory::expression, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("routine.procedure_invoke", "SBLR_PROCEDURE_INVOKE", "expression-eval", SblrOpcodeCategory::expression, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("routine.function_invoke", "SBLR_FUNCTION_INVOKE", "expression-eval", SblrOpcodeCategory::expression, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("routine.aggregate_invoke", "SBLR_AGGREGATE_INVOKE", "expression-eval", SblrOpcodeCategory::expression, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("sequence.nextval", "SBLR_SEQUENCE_NEXTVAL", "expression-eval", SblrOpcodeCategory::expression, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("sequence.currval", "SBLR_SEQUENCE_CURRVAL", "expression-eval", SblrOpcodeCategory::expression, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("sequence.setval", "SBLR_SEQUENCE_SETVAL", "expression-eval", SblrOpcodeCategory::expression, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("result.project", "SBLR_PROJECT", "result-shape", SblrOpcodeCategory::result_shape, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("result.aggregate", "SBLR_AGGREGATE", "result-shape", SblrOpcodeCategory::result_shape, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("result.group", "SBLR_GROUP", "result-shape", SblrOpcodeCategory::result_shape, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("result.sort", "SBLR_SORT", "result-shape", SblrOpcodeCategory::result_shape, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("result.limit", "SBLR_LIMIT", "result-shape", SblrOpcodeCategory::result_shape, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("result.window", "SBLR_WINDOW", "result-shape", SblrOpcodeCategory::result_shape, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("result.return_result_set", "SBLR_RETURN_RESULT_SET", "result-shape", SblrOpcodeCategory::result_shape, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("engine.op.kv_structured_read", "SBLR_KV_STRUCTURED_READ", "data-read", SblrOpcodeCategory::result_shape, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("engine.op.kv_structured_mutate", "SBLR_KV_STRUCTURED_MUTATE", "kv-structured-execution", SblrOpcodeCategory::dml, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("engine.op.ddl_alter_table", "SBLR_DDL_ALTER_TABLE", "catalog-ddl", SblrOpcodeCategory::ddl, SblrOpcodeSupport::local_profile_refusal, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true, false, "PROFILE.BUILTIN_PROFILE_UNAVAILABLE"),
      CanonicalEntry("engine.op.ddl_drop_table", "SBLR_DDL_DROP_TABLE", "catalog-ddl", SblrOpcodeCategory::ddl, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("ddl.domain.drop", "SBLR_DDL_DROP_DOMAIN", "catalog-ddl", SblrOpcodeCategory::ddl, SblrOpcodeSupport::local_profile_refusal, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true, false, "PROFILE.BUILTIN_PROFILE_UNAVAILABLE"),
      CanonicalEntry("engine.op.ddl_drop_schema", "SBLR_DDL_DROP_SCHEMA", "catalog-ddl", SblrOpcodeCategory::ddl, SblrOpcodeSupport::local_profile_refusal, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true, false, "PROFILE.BUILTIN_PROFILE_UNAVAILABLE"),
      CanonicalEntry("ddl.schema.alter", "SBLR_DDL_ALTER_SCHEMA", "catalog-ddl", SblrOpcodeCategory::ddl, SblrOpcodeSupport::local_profile_refusal, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true, false, "PROFILE.BUILTIN_PROFILE_UNAVAILABLE"),
      CanonicalEntry("ddl.index.alter", "SBLR_DDL_ALTER_INDEX", "catalog-ddl", SblrOpcodeCategory::ddl, SblrOpcodeSupport::local_profile_refusal, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true, false, "PROFILE.BUILTIN_PROFILE_UNAVAILABLE"),
      CanonicalEntry("ddl.domain.alter", "SBLR_DDL_ALTER_DOMAIN", "catalog-ddl", SblrOpcodeCategory::ddl, SblrOpcodeSupport::local_profile_refusal, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true, false, "PROFILE.BUILTIN_PROFILE_UNAVAILABLE"),
      CanonicalEntry("ddl.view.alter", "SBLR_DDL_ALTER_VIEW", "catalog-ddl", SblrOpcodeCategory::ddl, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("engine.op.ddl_drop_view", "SBLR_DDL_DROP_VIEW", "catalog-ddl", SblrOpcodeCategory::ddl, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("engine.op.ddl_alter_group", "SBLR_DDL_ALTER_GROUP", "catalog-ddl", SblrOpcodeCategory::ddl, SblrOpcodeSupport::local_profile_refusal, SblrOpcodeTransactionEffect::none, SblrOpcodeSecurityClass::authenticated, false, false, "SBSQL.IMPL.NOT_AVAILABLE"),
      CanonicalEntry("engine.op.ddl_alter_localized_name", "SBLR_DDL_ALTER_LOCALIZED_NAME", "catalog-ddl", SblrOpcodeCategory::ddl, SblrOpcodeSupport::local_profile_refusal, SblrOpcodeTransactionEffect::none, SblrOpcodeSecurityClass::authenticated, false, false, "SBSQL.IMPL.NOT_AVAILABLE"),
      CanonicalEntry("engine.op.ddl_create_table_as_query_with_data", "SBLR_DDL_CREATE_TABLE_AS_QUERY_WITH_DATA", "catalog-ddl", SblrOpcodeCategory::ddl, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("engine.op.ddl_create_table_as_query_with_no_data", "SBLR_DDL_CREATE_TABLE_AS_QUERY_WITH_NO_DATA", "catalog-ddl", SblrOpcodeCategory::ddl, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("engine.op.ddl_alter_package", "SBLR_DDL_ALTER_PACKAGE", "catalog-ddl", SblrOpcodeCategory::ddl, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("engine.op.ddl_drop_package", "SBLR_DDL_DROP_PACKAGE", "catalog-ddl", SblrOpcodeCategory::ddl, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("engine.op.ddl_drop_synonym", "SBLR_DDL_DROP_SYNONYM", "catalog-ddl", SblrOpcodeCategory::ddl, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("engine.op.ddl_alter_sequence", "SBLR_DDL_ALTER_SEQUENCE", "catalog-ddl", SblrOpcodeCategory::ddl, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("engine.op.ddl_drop_sequence", "SBLR_DDL_DROP_SEQUENCE", "catalog-ddl", SblrOpcodeCategory::ddl, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("engine.op.ddl_create_materialized_view", "SBLR_DDL_CREATE_MATERIALIZED_VIEW", "catalog-ddl", SblrOpcodeCategory::ddl, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("engine.op.ddl_refresh_materialized_view", "SBLR_DDL_REFRESH_MATERIALIZED_VIEW", "catalog-ddl", SblrOpcodeCategory::ddl, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("engine.op.ddl_drop_materialized_view", "SBLR_DDL_DROP_MATERIALIZED_VIEW", "catalog-ddl", SblrOpcodeCategory::ddl, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("engine.op.ddl_create_type", "SBLR_DDL_CREATE_TYPE", "catalog-ddl", SblrOpcodeCategory::ddl, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("engine.op.ddl_alter_type", "SBLR_DDL_ALTER_TYPE", "catalog-ddl", SblrOpcodeCategory::ddl, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("engine.op.ddl_drop_type", "SBLR_DDL_DROP_TYPE", "catalog-ddl", SblrOpcodeCategory::ddl, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("engine.op.ddl_rename_object", "SBLR_DDL_RENAME_OBJECT", "catalog-ddl", SblrOpcodeCategory::ddl, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("engine.op.ddl_create_synonym", "SBLR_DDL_CREATE_SYNONYM", "catalog-ddl", SblrOpcodeCategory::ddl, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("engine.op.ddl_create_foreign_table", "SBLR_DDL_CREATE_FOREIGN_TABLE", "catalog-ddl", SblrOpcodeCategory::ddl, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("engine.op.ddl_drop_foreign_table", "SBLR_DDL_DROP_FOREIGN_TABLE", "catalog-ddl", SblrOpcodeCategory::ddl, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("engine.op.ddl_create_fdw", "SBLR_DDL_CREATE_FDW", "catalog-ddl", SblrOpcodeCategory::ddl, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("engine.op.ddl_drop_fdw", "SBLR_DDL_DROP_FDW", "catalog-ddl", SblrOpcodeCategory::ddl, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("engine.op.security_create_user", "SBLR_SEC_CREATE_USER", "security-management", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::security, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("engine.op.sec_alter_user", "SBLR_SEC_ALTER_USER", "security-management", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::security, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("engine.op.sec_create_role", "SBLR_SEC_CREATE_ROLE", "security-management", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::security, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("security.grant", "SBLR_SEC_GRANT", "security-management", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::security, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("security.revoke", "SBLR_SEC_REVOKE", "security-management", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::security, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("engine.op.sec_create_group_mapping", "SBLR_SEC_CREATE_GROUP_MAPPING", "security-management", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::security, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("security.policy.lifecycle_alter", "SBLR_SEC_ALTER_POLICY", "security-management", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::security, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("security.user.drop", "SBLR_SEC_DROP_USER", "security-management", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::security, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("engine.op.sec_alter_role", "SBLR_SEC_ALTER_ROLE", "security-management", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::security, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("engine.op.sec_drop_role", "SBLR_SEC_DROP_ROLE", "security-management", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::security, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("engine.op.sec_create_policy", "SBLR_SEC_CREATE_POLICY", "security-management", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::security, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("engine.op.sec_drop_policy", "SBLR_SEC_DROP_POLICY", "security-management", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::security, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("security.authenticate", "SBLR_SEC_AUTHENTICATE", "security-management", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::security, SblrOpcodeSecurityClass::authenticated, true),
      CanonicalEntry("security.deauthenticate", "SBLR_SEC_DEAUTHENTICATE", "security-management", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::security, SblrOpcodeSecurityClass::authenticated, true),
      CanonicalEntry("security.group_mapping.drop", "SBLR_SEC_DROP_GROUP_MAPPING", "security-management", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::security, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("filespace.create", "SBLR_FILESPACE_CREATE", "filespace-management", SblrOpcodeCategory::management, SblrOpcodeSupport::local_profile_refusal, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::sysarch_authorized, true, false, "PROFILE.BUILTIN_PROFILE_UNAVAILABLE"),
      CanonicalEntry("filespace.preallocate", "SBLR_FILESPACE_PREALLOCATE", "filespace-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::sysarch_authorized, true),
      CanonicalEntry("filespace.attach", "SBLR_FILESPACE_ATTACH", "filespace-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::sysarch_authorized, true),
      CanonicalEntry("filespace.detach", "SBLR_FILESPACE_DETACH", "filespace-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::sysarch_authorized, true),
      CanonicalEntry("filespace.disconnect", "SBLR_FILESPACE_DISCONNECT", "filespace-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::sysarch_authorized, true),
      CanonicalEntry("filespace.move", "SBLR_FILESPACE_MOVE", "filespace-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::sysarch_authorized, true),
      CanonicalEntry("filespace.merge", "SBLR_FILESPACE_MERGE", "filespace-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::sysarch_authorized, true),
      CanonicalEntry("filespace.promote", "SBLR_FILESPACE_PROMOTE", "filespace-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::sysarch_authorized, true),
      CanonicalEntry("filespace.verify", "SBLR_FILESPACE_VERIFY", "filespace-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::sysarch_authorized, true),
      CanonicalEntry("filespace.compact", "SBLR_FILESPACE_COMPACT", "filespace-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::sysarch_authorized, true),
      CanonicalEntry("filespace.fence", "SBLR_FILESPACE_FENCE", "filespace-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::sysarch_authorized, true),
      CanonicalEntry("filespace.release", "SBLR_FILESPACE_RELEASE", "filespace-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::sysarch_authorized, true),
      CanonicalEntry("filespace.archive", "SBLR_FILESPACE_ARCHIVE", "filespace-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::sysarch_authorized, true),
      CanonicalEntry("filespace.quarantine", "SBLR_FILESPACE_QUARANTINE", "filespace-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::sysarch_authorized, true),
      CanonicalEntry("filespace.snapshot.create", "SBLR_FILESPACE_SNAPSHOT_CREATE", "filespace-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::sysarch_authorized, true),
      CanonicalEntry("filespace.snapshot.refresh", "SBLR_FILESPACE_SNAPSHOT_REFRESH", "filespace-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::sysarch_authorized, true),
      CanonicalEntry("filespace.snapshot.validate", "SBLR_FILESPACE_SNAPSHOT_VALIDATE", "filespace-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::sysarch_authorized, true),
      CanonicalEntry("filespace.snapshot.retire", "SBLR_FILESPACE_SNAPSHOT_RETIRE", "filespace-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::sysarch_authorized, true),
      CanonicalEntry("filespace.shadow.create", "SBLR_FILESPACE_SHADOW_CREATE", "filespace-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::sysarch_authorized, true),
      CanonicalEntry("filespace.shadow.refresh", "SBLR_FILESPACE_SHADOW_REFRESH", "filespace-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::sysarch_authorized, true),
      CanonicalEntry("filespace.shadow.validate", "SBLR_FILESPACE_SHADOW_VALIDATE", "filespace-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::sysarch_authorized, true),
      CanonicalEntry("filespace.shadow.promote", "SBLR_FILESPACE_SHADOW_PROMOTE", "filespace-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::sysarch_authorized, true),
      CanonicalEntry("filespace.truncate", "SBLR_FILESPACE_TRUNCATE", "filespace-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::sysarch_authorized, true),
      CanonicalEntry("filespace.drop", "SBLR_FILESPACE_DROP", "filespace-management", SblrOpcodeCategory::management, SblrOpcodeSupport::local_profile_refusal, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::sysarch_authorized, true, false, "PROFILE.BUILTIN_PROFILE_UNAVAILABLE"),
      CanonicalEntry("filespace.delete_physical", "SBLR_FILESPACE_DELETE_PHYSICAL", "filespace-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::sysarch_authorized, true),
      CanonicalEntry("filespace.repair", "SBLR_FILESPACE_REPAIR", "filespace-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::sysarch_authorized, true),
      CanonicalEntry("filespace.rebuild", "SBLR_FILESPACE_REBUILD", "filespace-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::sysarch_authorized, true),
      CanonicalEntry("filespace.salvage", "SBLR_FILESPACE_SALVAGE", "filespace-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::sysarch_authorized, true),
      CanonicalEntry("index.rebuild", "SBLR_INDEX_REBUILD", "index-management", SblrOpcodeCategory::management, SblrOpcodeSupport::local_profile_refusal, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::admin_authorized, true, false, "PROFILE.BUILTIN_PROFILE_UNAVAILABLE"),
      CanonicalEntry("index.rebalance", "SBLR_INDEX_REBALANCE", "index-management", SblrOpcodeCategory::management, SblrOpcodeSupport::local_profile_refusal, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::admin_authorized, true, false, "PROFILE.BUILTIN_PROFILE_UNAVAILABLE"),
      CanonicalEntry("index.verify", "SBLR_INDEX_VERIFY", "index-management", SblrOpcodeCategory::management, SblrOpcodeSupport::local_profile_refusal, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::admin_authorized, true, false, "PROFILE.BUILTIN_PROFILE_UNAVAILABLE"),
      // DPC_INDEX_VALIDATION_REPAIR_TOOLING
      CanonicalEntry("index.validate", "SBLR_INDEX_VALIDATE", "index-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("index.repair", "SBLR_INDEX_REPAIR", "index-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("index.discard_unpublished", "SBLR_INDEX_DISCARD_UNPUBLISHED", "index-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("index.gather_statistics", "SBLR_INDEX_GATHER_STATISTICS", "index-management", SblrOpcodeCategory::management, SblrOpcodeSupport::local_profile_refusal, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::admin_authorized, true, false, "PROFILE.BUILTIN_PROFILE_UNAVAILABLE"),
      CanonicalEntry("index.cleanup_mga_versions", "SBLR_INDEX_CLEANUP_MGA_VERSIONS", "index-management", SblrOpcodeCategory::management, SblrOpcodeSupport::local_profile_refusal, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::admin_authorized, true, false, "PROFILE.BUILTIN_PROFILE_UNAVAILABLE"),
      CanonicalEntry("backup.start", "SBLR_BACKUP_START", "backup-archive-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("backup.finish", "SBLR_BACKUP_FINISH", "backup-archive-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("backup.restore", "SBLR_RESTORE_BACKUP", "backup-archive-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("archive.export", "SBLR_ARCHIVE_EXPORT", "backup-archive-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("archive.verify", "SBLR_ARCHIVE_VERIFY", "backup-archive-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("cluster.join", "SBLR_CLUSTER_JOIN", "cluster-management", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, SblrOpcodeTransactionEffect::cluster_write, SblrOpcodeSecurityClass::cluster_authorized, true, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      CanonicalEntry("cluster.leave", "SBLR_CLUSTER_LEAVE", "cluster-management", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, SblrOpcodeTransactionEffect::cluster_write, SblrOpcodeSecurityClass::cluster_authorized, true, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      CanonicalEntry("cluster.route_request", "SBLR_CLUSTER_ROUTE_REQUEST", "cluster-management", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::cluster_authorized, true, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      CanonicalEntry("cluster.publish_route", "SBLR_CLUSTER_PUBLISH_ROUTE", "cluster-management", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, SblrOpcodeTransactionEffect::cluster_write, SblrOpcodeSecurityClass::cluster_authorized, true, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      CanonicalEntry("cluster.fence_node", "SBLR_CLUSTER_FENCE_NODE", "cluster-management", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, SblrOpcodeTransactionEffect::cluster_write, SblrOpcodeSecurityClass::cluster_authorized, true, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      CanonicalEntry("cluster.reconcile_branch", "SBLR_CLUSTER_RECONCILE_BRANCH", "cluster-management", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, SblrOpcodeTransactionEffect::cluster_write, SblrOpcodeSecurityClass::cluster_authorized, true, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      CanonicalEntry("cluster.publish_epoch", "SBLR_CLUSTER_PUBLISH_EPOCH", "cluster-management", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, SblrOpcodeTransactionEffect::cluster_write, SblrOpcodeSecurityClass::cluster_authorized, true, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      CanonicalEntry("diagnostic.emit", "SBLR_EMIT_DIAGNOSTIC", "diagnostics-and-metrics", SblrOpcodeCategory::observability, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::none, SblrOpcodeSecurityClass::authenticated, false),
      CanonicalEntry("engine.op.read_metrics", "SBLR_READ_METRICS", "diagnostics-and-metrics", SblrOpcodeCategory::observability, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::authenticated, false),
      CanonicalEntry("metrics.reset", "SBLR_RESET_METRICS", "diagnostics-and-metrics", SblrOpcodeCategory::observability, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("diagnostic.explain_operation", "SBLR_EXPLAIN_OPERATION", "diagnostics-and-metrics", SblrOpcodeCategory::observability, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::authenticated, true),
      CanonicalEntry("diagnostic.emit_audit_event", "SBLR_EMIT_AUDIT_EVENT", "diagnostics-and-metrics", SblrOpcodeCategory::observability, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::external_audit, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("engine.op.mgmt_operation", "SBLR_MGMT_OPERATION", "management-envelope", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::admin_authorized, false),
      CanonicalEntry("engine.op.mgmt_payload", "SBLR_MGMT_PAYLOAD", "management-envelope", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::admin_authorized, false),
      CanonicalEntry("engine.op.mgmt_result", "SBLR_MGMT_RESULT", "management-envelope", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::admin_authorized, false),
      CanonicalEntry("engine.op.mgmt_progress", "SBLR_MGMT_PROGRESS", "management-envelope", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::admin_authorized, false),
      CanonicalEntry("engine.op.mgmt_diagnostic", "SBLR_MGMT_DIAGNOSTIC", "management-envelope", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::admin_authorized, false),
      CanonicalEntry("engine.op.mgmt_metric_snapshot_ref", "SBLR_MGMT_METRIC_SNAPSHOT_REF", "management-envelope", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::admin_authorized, false),
      CanonicalEntry("mga.show_horizons", "SBLR_MGA_SHOW_HORIZONS", "mga-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("mga.checkpoint", "SBLR_MGA_CHECKPOINT", "mga-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("mga.sweep", "SBLR_MGA_SWEEP", "mga-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("mga.cleanup_hot_versions", "SBLR_MGA_CLEANUP_HOT_VERSIONS", "mga-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("mga.verify_archive_manifest", "SBLR_MGA_VERIFY_ARCHIVE_MANIFEST", "mga-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("mga.verify_archive_reachability", "SBLR_MGA_VERIFY_ARCHIVE_REACHABILITY", "mga-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("mga.verify_archive_decryptability", "SBLR_MGA_VERIFY_ARCHIVE_DECRYPTABILITY", "mga-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("mga.verify_backup_coverage", "SBLR_MGA_VERIFY_BACKUP_COVERAGE", "mga-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("mga.prove_stream_truncation", "SBLR_MGA_PROVE_STREAM_TRUNCATION", "mga-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("cluster.mga_txn.inspect", "SBLR_MGA_CLUSTER_TXN_INSPECT", "mga-management", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::cluster_authorized, true, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      CanonicalEntry("cluster.mga_txn.resolve", "SBLR_MGA_CLUSTER_TXN_RESOLVE", "mga-management", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, SblrOpcodeTransactionEffect::cluster_write, SblrOpcodeSecurityClass::cluster_authorized, true, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      CanonicalEntry("cluster.mga_txn.retry_decision", "SBLR_MGA_CLUSTER_TXN_RETRY_DECISION", "mga-management", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, SblrOpcodeTransactionEffect::cluster_write, SblrOpcodeSecurityClass::cluster_authorized, true, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      CanonicalEntry("cluster.mga_txn.quarantine", "SBLR_MGA_CLUSTER_TXN_QUARANTINE", "mga-management", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, SblrOpcodeTransactionEffect::cluster_write, SblrOpcodeSecurityClass::cluster_authorized, true, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      CanonicalEntry("mga.show_archive_orphans", "SBLR_MGA_SHOW_ARCHIVE_ORPHANS", "mga-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("mga.reclaim_archive_orphans", "SBLR_MGA_RECLAIM_ARCHIVE_ORPHANS", "mga-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("mga.audit_legal_hold", "SBLR_MGA_AUDIT_LEGAL_HOLD", "mga-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("optimizer.plan.physical_property_requirement", "SBLR_PLAN_PHYSICAL_PROPERTY_REQUIREMENT", "optimizer-plan", SblrOpcodeCategory::query, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::none, SblrOpcodeSecurityClass::authenticated, false),
      CanonicalEntry("optimizer.plan.reference_compatibility_requirement", "SBLR_PLAN_REFERENCE_COMPATIBILITY_REQUIREMENT", "optimizer-plan", SblrOpcodeCategory::query, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::none, SblrOpcodeSecurityClass::authenticated, false),
      CanonicalEntry("optimizer.plan.mga_visibility_requirement", "SBLR_PLAN_MGA_VISIBILITY_REQUIREMENT", "optimizer-plan", SblrOpcodeCategory::query, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::none, SblrOpcodeSecurityClass::authenticated, false),
      CanonicalEntry("optimizer.plan.cache_dependency_identity", "SBLR_PLAN_CACHE_DEPENDENCY_IDENTITY", "optimizer-plan", SblrOpcodeCategory::query, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::none, SblrOpcodeSecurityClass::authenticated, false),
      CanonicalEntry("optimizer.explain_metadata", "SBLR_EXPLAIN_METADATA", "optimizer-plan", SblrOpcodeCategory::query, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::authenticated, true),
      CanonicalEntry("optimizer.adaptive_feedback", "SBLR_ADAPTIVE_FEEDBACK", "optimizer-plan", SblrOpcodeCategory::query, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::none, SblrOpcodeSecurityClass::admin_authorized, false),
      CanonicalEntry("cluster.optimizer.remote_operator", "SBLR_REMOTE_OPERATOR", "optimizer-plan", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::cluster_authorized, true, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      CanonicalEntry("optimizer.vector_plan_node", "SBLR_VECTOR_PLAN_NODE", "optimizer-plan", SblrOpcodeCategory::query, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("optimizer.text_plan_node", "SBLR_TEXT_PLAN_NODE", "optimizer-plan", SblrOpcodeCategory::query, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("event.channel.alter", "SBLR_EVENT_CHANNEL_ALTER", "event-notification", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::catalog_write, SblrOpcodeSecurityClass::event_admin, true),
      CanonicalEntry("event.channel.drop", "SBLR_EVENT_CHANNEL_DROP", "event-notification", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::catalog_write, SblrOpcodeSecurityClass::event_admin, true),
      CanonicalEntry("connection.open", "SBLR_CONN_OPEN", "session-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::none, SblrOpcodeSecurityClass::authenticated, false),
      CanonicalEntry("connection.close", "SBLR_CONN_CLOSE", "session-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::none, SblrOpcodeSecurityClass::authenticated, false),
      CanonicalEntry("connection.hello", "SBLR_CONN_HELLO", "session-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::none, SblrOpcodeSecurityClass::public_metadata, false),
      CanonicalEntry("session.setting.set", "SBLR_SESSION_SETTING_SET", "session-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::none, SblrOpcodeSecurityClass::authenticated, false),
      CanonicalEntry("session.setting.get", "SBLR_SESSION_SETTING_GET", "session-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::none, SblrOpcodeSecurityClass::authenticated, false),
      CanonicalEntry("session.setting.reset", "SBLR_SESSION_SETTING_RESET", "session-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::none, SblrOpcodeSecurityClass::authenticated, false),
      CanonicalEntry("session.default_qualifier.set", "SBLR_SESSION_DEFAULT_QUALIFIER_SET", "session-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::none, SblrOpcodeSecurityClass::authenticated, false),
      CanonicalEntry("session.role.switch", "SBLR_SESSION_ROLE_SWITCH", "session-management", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::security, SblrOpcodeSecurityClass::authenticated, true),
      CanonicalEntry("session.discard", "SBLR_SESSION_DISCARD", "session-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::authenticated, true),
      CanonicalEntry("session.snapshot_handle", "SBLR_SESSION_SNAPSHOT_HANDLE", "session-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::authenticated, true),
      CanonicalEntry("statement.prepare", "SBLR_STMT_PREPARE", "statement-management", SblrOpcodeCategory::query, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::none, SblrOpcodeSecurityClass::authenticated, false),
      CanonicalEntry("statement.execute", "SBLR_STMT_EXECUTE", "statement-management", SblrOpcodeCategory::query, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("statement.execute_direct", "SBLR_STMT_EXECUTE_DIRECT", "statement-management", SblrOpcodeCategory::query, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("statement.free", "SBLR_STMT_FREE", "statement-management", SblrOpcodeCategory::query, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::none, SblrOpcodeSecurityClass::authenticated, false),
      CanonicalEntry("statement.cancel", "SBLR_STMT_CANCEL", "statement-management", SblrOpcodeCategory::query, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::authenticated, true),
      CanonicalEntry("parameter.bind", "SBLR_PARAMETER_BIND", "statement-management", SblrOpcodeCategory::query, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::none, SblrOpcodeSecurityClass::authenticated, false),
      CanonicalEntry("result.page", "SBLR_RESULT_PAGE", "statement-management", SblrOpcodeCategory::query, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, true),
      // QOW-SOURCE-QRY-003-V1
      // QOW-ROUTE-STAGE-QRY-003-V1
      CanonicalEntry("query.execute", "SBLR_QUERY_EXECUTE", "statement-management", SblrOpcodeCategory::query, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("query.explain", "SBLR_QUERY_EXPLAIN", "statement-management", SblrOpcodeCategory::query, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("catalog.name_resolve", "SBLR_NAME_RESOLVE", "catalog-introspect", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::authenticated, true),
      CanonicalEntry("optimizer.stats.read", "SBLR_OPTIMIZER_STATS_READ", "catalog-introspect", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::authenticated, true),
      CanonicalEntry("optimizer.stats.drop", "SBLR_OPTIMIZER_STATS_DROP", "catalog-introspect", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("parse.text", "SBLR_PARSE_TEXT", "catalog-introspect", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::none, SblrOpcodeSecurityClass::authenticated, false),
      CanonicalEntry("catalog.epoch_check", "SBLR_CATALOG_EPOCH_CHECK", "catalog-introspect", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::authenticated, true),
      CanonicalEntry("database.attach", "SBLR_DATABASE_ATTACH", "database-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("database.detach", "SBLR_DATABASE_DETACH", "database-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("database.checkpoint", "SBLR_DATABASE_CHECKPOINT", "database-management", SblrOpcodeCategory::management, SblrOpcodeSupport::local_profile_refusal, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::admin_authorized, true, false, "PROFILE.BUILTIN_PROFILE_UNAVAILABLE"),
      CanonicalEntry("database.vacuum", "SBLR_DATABASE_VACUUM", "database-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("database.alter", "SBLR_DATABASE_ALTER", "database-management", SblrOpcodeCategory::management, SblrOpcodeSupport::local_profile_refusal, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::admin_authorized, true, false, "PROFILE.BUILTIN_PROFILE_UNAVAILABLE"),
      CanonicalEntry("cluster.replication.consumer.subscribe", "SBLR_REPL_CONSUMER_SUBSCRIBE", "replication-consumer", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::admin_authorized, true, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      CanonicalEntry("cluster.replication.consumer.resume", "SBLR_REPL_CONSUMER_RESUME", "replication-consumer", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::admin_authorized, true, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      CanonicalEntry("cluster.replication.consumer.pause", "SBLR_REPL_CONSUMER_PAUSE", "replication-consumer", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::admin_authorized, true, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      CanonicalEntry("cluster.replication.consumer.cancel", "SBLR_REPL_CONSUMER_CANCEL", "replication-consumer", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::admin_authorized, true, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      CanonicalEntry("cluster.replication.cdc.receive", "SBLR_REPL_CDC_RECEIVE", "replication-consumer", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::admin_authorized, true, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      CanonicalEntry("cluster.replication.cdc.ack", "SBLR_REPL_CDC_ACK", "replication-consumer", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::admin_authorized, true, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      CanonicalEntry("cluster.replication.two_phase.prewrite", "SBLR_REPL_2PC_PREWRITE", "replication-consumer", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, SblrOpcodeTransactionEffect::cluster_write, SblrOpcodeSecurityClass::cluster_authorized, true, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      CanonicalEntry("cluster.replication.two_phase.commit", "SBLR_REPL_2PC_COMMIT", "replication-consumer", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, SblrOpcodeTransactionEffect::cluster_write, SblrOpcodeSecurityClass::cluster_authorized, true, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      CanonicalEntry("cluster.replication.two_phase.cleanup", "SBLR_REPL_2PC_CLEANUP", "replication-consumer", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, SblrOpcodeTransactionEffect::cluster_write, SblrOpcodeSecurityClass::cluster_authorized, true, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      CanonicalEntry("cluster.replication.two_phase.resolve_lock", "SBLR_REPL_2PC_RESOLVE_LOCK", "replication-consumer", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, SblrOpcodeTransactionEffect::cluster_write, SblrOpcodeSecurityClass::cluster_authorized, true, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      CanonicalEntry("cluster.replication.two_phase.pessimistic_lock", "SBLR_REPL_2PC_PESSIMISTIC_LOCK", "replication-consumer", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, SblrOpcodeTransactionEffect::cluster_write, SblrOpcodeSecurityClass::cluster_authorized, true, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      CanonicalEntry("cluster.replication.two_phase.pessimistic_rollback", "SBLR_REPL_2PC_PESSIMISTIC_ROLLBACK", "replication-consumer", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, SblrOpcodeTransactionEffect::cluster_write, SblrOpcodeSecurityClass::cluster_authorized, true, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      CanonicalEntry("cluster.replication.two_phase.heartbeat", "SBLR_REPL_2PC_HEARTBEAT", "replication-consumer", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, SblrOpcodeTransactionEffect::cluster_write, SblrOpcodeSecurityClass::cluster_authorized, true, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      CanonicalEntry("cluster.replication.two_phase.check_status", "SBLR_REPL_2PC_CHECK_STATUS", "replication-consumer", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::cluster_authorized, true, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      CanonicalEntry("graph.traverse", "SBLR_GRAPH_TRAVERSE", "graph-execution", SblrOpcodeCategory::query, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("graph.optional_match", "SBLR_GRAPH_OPTIONAL_MATCH", "graph-execution", SblrOpcodeCategory::query, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("graph.create", "SBLR_GRAPH_CREATE", "graph-execution", SblrOpcodeCategory::data_mutation, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("graph.merge", "SBLR_GRAPH_MERGE", "graph-execution", SblrOpcodeCategory::data_mutation, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("graph.set", "SBLR_GRAPH_SET", "graph-execution", SblrOpcodeCategory::data_mutation, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("graph.remove", "SBLR_GRAPH_REMOVE", "graph-execution", SblrOpcodeCategory::data_mutation, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("graph.delete", "SBLR_GRAPH_DELETE", "graph-execution", SblrOpcodeCategory::data_mutation, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("graph.detach_delete", "SBLR_GRAPH_DETACH_DELETE", "graph-execution", SblrOpcodeCategory::data_mutation, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("vector.search", "SBLR_VECTOR_SEARCH", "vector-execution", SblrOpcodeCategory::query, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("vector.hybrid_search", "SBLR_VECTOR_HYBRID_SEARCH", "vector-execution", SblrOpcodeCategory::query, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("vector.similarity", "SBLR_VECTOR_SIMILARITY", "vector-execution", SblrOpcodeCategory::query, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("vector.index.load", "SBLR_VECTOR_INDEX_LOAD", "vector-execution", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("vector.index.release", "SBLR_VECTOR_INDEX_RELEASE", "vector-execution", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("fulltext.score", "SBLR_FULLTEXT_SCORE", "fulltext-execution", SblrOpcodeCategory::query, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("fulltext.phrase_score", "SBLR_FULLTEXT_PHRASE_SCORE", "fulltext-execution", SblrOpcodeCategory::query, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("fulltext.multi_field_score", "SBLR_FULLTEXT_MULTI_FIELD_SCORE", "fulltext-execution", SblrOpcodeCategory::query, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("fulltext.regex_match", "SBLR_FULLTEXT_REGEX_MATCH", "fulltext-execution", SblrOpcodeCategory::query, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("fulltext.wildcard_match", "SBLR_FULLTEXT_WILDCARD_MATCH", "fulltext-execution", SblrOpcodeCategory::query, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("fulltext.prefix_match", "SBLR_FULLTEXT_PREFIX_MATCH", "fulltext-execution", SblrOpcodeCategory::query, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("fulltext.analyzer_apply", "SBLR_FULLTEXT_ANALYZER_APPLY", "fulltext-execution", SblrOpcodeCategory::query, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("diagnostic.refusal", "SBLR_DIAGNOSTIC_REFUSAL", "diagnostic-control", SblrOpcodeCategory::observability, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::none, SblrOpcodeSecurityClass::authenticated, false),
      CanonicalEntry("diagnostic.reset", "SBLR_DIAGNOSTIC_RESET", "diagnostic-control", SblrOpcodeCategory::observability, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("descriptor.transform", "SBLR_DESCRIPTOR_TRANSFORM", "diagnostic-control", SblrOpcodeCategory::observability, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::none, SblrOpcodeSecurityClass::authenticated, false),
      CanonicalEntry("relational.join", "SBLR_JOIN", "relational-plan-node", SblrOpcodeCategory::query, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("relational.set_operation", "SBLR_SET_OPERATION", "relational-plan-node", SblrOpcodeCategory::query, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("relational.cte", "SBLR_CTE", "relational-plan-node", SblrOpcodeCategory::query, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("relational.recursive_cte", "SBLR_RECURSIVE_CTE", "relational-plan-node", SblrOpcodeCategory::query, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("relational.pivot", "SBLR_PIVOT", "relational-plan-node", SblrOpcodeCategory::query, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("relational.unpivot", "SBLR_UNPIVOT", "relational-plan-node", SblrOpcodeCategory::query, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("relational.values", "SBLR_VALUES", "relational-plan-node", SblrOpcodeCategory::query, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("relational.match_recognize", "SBLR_MATCH_RECOGNIZE", "relational-plan-node", SblrOpcodeCategory::query, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("relational.table_function_invoke", "SBLR_TABLE_FUNCTION_INVOKE", "relational-plan-node", SblrOpcodeCategory::query, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("engine.op.kv_structured_scan", "SBLR_KV_STRUCTURED_SCAN", "kv-structured-execution", SblrOpcodeCategory::query, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("engine.op.kv_structured_stream_read", "SBLR_KV_STRUCTURED_STREAM_READ", "kv-structured-execution", SblrOpcodeCategory::query, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("engine.op.kv_structured_stream_append", "SBLR_KV_STRUCTURED_STREAM_APPEND", "kv-structured-execution", SblrOpcodeCategory::data_mutation, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("engine.op.kv_structured_timeseries", "SBLR_KV_STRUCTURED_TIMESERIES", "kv-structured-execution", SblrOpcodeCategory::data_mutation, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("engine.op.system_config_set", "SBLR_SYSTEM_CONFIG_SET", "database-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::sysarch_authorized, true),
      CanonicalEntry("engine.op.ddl_create_domain", "SBLR_DDL_CREATE_DOMAIN", "catalog-ddl", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("engine.op.ddl_create_schema", "SBLR_DDL_CREATE_SCHEMA", "catalog-ddl", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, false),
      CanonicalEntry("engine.op.ddl_create_table", "SBLR_DDL_CREATE_TABLE", "catalog-ddl", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("engine.op.ddl_create_index", "SBLR_DDL_CREATE_INDEX", "catalog-ddl", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("engine.op.ddl_alter_trigger", "SBLR_DDL_ALTER_TRIGGER", "catalog-ddl", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("engine.op.ddl_create_procedure", "SBLR_DDL_CREATE_PROCEDURE", "catalog-ddl", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("engine.op.ddl_alter_procedure", "SBLR_DDL_ALTER_PROCEDURE", "catalog-ddl", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("engine.op.ddl_drop_procedure", "SBLR_DDL_DROP_PROCEDURE", "catalog-ddl", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("engine.op.ddl_create_function", "SBLR_DDL_CREATE_FUNCTION", "catalog-ddl", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("engine.op.ddl_alter_function", "SBLR_DDL_ALTER_FUNCTION", "catalog-ddl", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("engine.op.ddl_drop_function", "SBLR_DDL_DROP_FUNCTION", "catalog-ddl", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("engine.op.ddl_create_package", "SBLR_DDL_CREATE_PACKAGE", "catalog-ddl", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
CanonicalEntry("engine.op.ddl_create_temporary_table", "SBLR_DDL_CREATE_TEMPORARY_TABLE", "catalog-ddl", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
CanonicalEntry("engine.op.ddl_drop_temporary_table", "SBLR_DDL_DROP_TEMPORARY_TABLE", "catalog-ddl", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
CanonicalEntry("engine.op.ddl_rename_object_vector", "SBLR_DDL_RENAME_OBJECT_VECTOR", "catalog-ddl", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
CanonicalEntry("engine.op.ddl_create_or_replace_srs", "SBLR_DDL_CREATE_OR_REPLACE_SRS", "catalog-ddl", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("engine.op.ddl_drop_srs", "SBLR_DDL_DROP_SRS", "catalog-ddl", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("engine.op.ddl_create_rewrite_rule", "SBLR_DDL_CREATE_REWRITE_RULE", "catalog-ddl", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("engine.op.ddl_alter_rewrite_rule", "SBLR_DDL_ALTER_REWRITE_RULE", "catalog-ddl", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("engine.op.ddl_drop_rewrite_rule", "SBLR_DDL_DROP_REWRITE_RULE", "catalog-ddl", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("engine.op.ddl_validate_constraint", "SBLR_DDL_VALIDATE_CONSTRAINT", "catalog-ddl", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("engine.op.security_create_privilege_template", "SBLR_SECURITY_CREATE_PRIVILEGE_TEMPLATE", "security", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("engine.op.security_alter_privilege_template", "SBLR_SECURITY_ALTER_PRIVILEGE_TEMPLATE", "security", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("engine.op.security_drop_privilege_template", "SBLR_SECURITY_DROP_PRIVILEGE_TEMPLATE", "security", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("engine.op.database_create_template_clone", "SBLR_DATABASE_CREATE_TEMPLATE_CLONE", "catalog-ddl", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("engine.op.ddl_create_aggregate", "SBLR_DDL_CREATE_AGGREGATE", "catalog-ddl", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("engine.op.ddl_alter_aggregate", "SBLR_DDL_ALTER_AGGREGATE", "catalog-ddl", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("engine.op.ddl_drop_aggregate", "SBLR_DDL_DROP_AGGREGATE", "catalog-ddl", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("engine.op.ddl_purge_system_history", "SBLR_DDL_PURGE_SYSTEM_HISTORY", "catalog-ddl", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("engine.op.ddl_set_index_optimizer_eligibility", "SBLR_DDL_SET_INDEX_OPTIMIZER_ELIGIBILITY", "catalog-ddl", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("engine.op.ddl_set_table_type_enforcement", "SBLR_DDL_SET_TABLE_TYPE_ENFORCEMENT", "catalog-ddl", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("engine.op.database_serialize_logical_snapshot", "SBLR_DATABASE_SERIALIZE_LOGICAL_SNAPSHOT", "catalog-ddl", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("engine.op.database_deserialize_logical_snapshot", "SBLR_DATABASE_DESERIALIZE_LOGICAL_SNAPSHOT", "catalog-ddl", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("engine.op.ddl_create_macro", "SBLR_DDL_CREATE_MACRO", "catalog-ddl", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("engine.op.ddl_drop_macro", "SBLR_DDL_DROP_MACRO", "catalog-ddl", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("engine.op.admin_register_external_relation_resolver", "SBLR_ADMIN_REGISTER_EXTERNAL_RELATION_RESOLVER", "catalog-ddl", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("engine.op.admin_unregister_external_relation_resolver", "SBLR_ADMIN_UNREGISTER_EXTERNAL_RELATION_RESOLVER", "catalog-ddl", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("engine.op.ddl_create_dictionary", "SBLR_DDL_CREATE_DICTIONARY", "catalog-ddl", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("engine.op.ddl_drop_dictionary", "SBLR_DDL_DROP_DICTIONARY", "catalog-ddl", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("engine.op.ddl_alter_dictionary", "SBLR_DDL_ALTER_DICTIONARY", "catalog-ddl", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("engine.op.ddl_create_continuous_view", "SBLR_DDL_CREATE_CONTINUOUS_VIEW", "catalog-ddl", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("engine.op.ddl_alter_continuous_view", "SBLR_DDL_ALTER_CONTINUOUS_VIEW", "catalog-ddl", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("engine.op.ddl_drop_continuous_view", "SBLR_DDL_DROP_CONTINUOUS_VIEW", "catalog-ddl", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("engine.op.dml_async_insert_submit", "SBLR_DML_ASYNC_INSERT_SUBMIT", "catalog-ddl", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("engine.op.dml_async_insert_status", "SBLR_DML_ASYNC_INSERT_STATUS", "catalog-ddl", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("engine.op.dml_async_insert_cancel", "SBLR_DML_ASYNC_INSERT_CANCEL", "catalog-ddl", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("engine.op.dml_conditional_mutate", "SBLR_DML_CONDITIONAL_MUTATE", "catalog-ddl", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("engine.op.dml_counter_add", "SBLR_DML_COUNTER_ADD", "catalog-ddl", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("engine.op.dml_timeseries_schema_write", "SBLR_DML_TIMESERIES_SCHEMA_WRITE", "time-series", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("engine.op.ddl_set_timeseries_series_cardinality_policy", "SBLR_DDL_SET_TIMESERIES_SERIES_CARDINALITY_POLICY", "time-series", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("engine.op.ddl_create_timeseries_value_cache", "SBLR_DDL_CREATE_TIMESERIES_VALUE_CACHE", "time-series", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("engine.op.ddl_drop_trigger", "SBLR_DDL_DROP_TRIGGER", "catalog-ddl", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("engine.op.ddl_drop_index", "SBLR_DDL_DROP_INDEX", "catalog-ddl", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("system.config.get", "SBLR_SYSTEM_CONFIG_GET", "database-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::sysarch_authorized, true),
      CanonicalEntry("system.config.reset", "SBLR_SYSTEM_CONFIG_RESET", "database-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::sysarch_authorized, true),
      CanonicalEntry("ddl.rule.create", "SBLR_DDL_CREATE_RULE", "catalog-ddl", SblrOpcodeCategory::ddl, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("ddl.rule.drop", "SBLR_DDL_DROP_RULE", "catalog-ddl", SblrOpcodeCategory::ddl, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("ddl.publication.create", "SBLR_DDL_CREATE_PUBLICATION", "catalog-ddl", SblrOpcodeCategory::ddl, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("ddl.publication.alter", "SBLR_DDL_ALTER_PUBLICATION", "catalog-ddl", SblrOpcodeCategory::ddl, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("ddl.publication.drop", "SBLR_DDL_DROP_PUBLICATION", "catalog-ddl", SblrOpcodeCategory::ddl, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("ddl.subscription.create", "SBLR_DDL_CREATE_SUBSCRIPTION", "catalog-ddl", SblrOpcodeCategory::ddl, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("ddl.subscription.alter", "SBLR_DDL_ALTER_SUBSCRIPTION", "catalog-ddl", SblrOpcodeCategory::ddl, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("ddl.subscription.drop", "SBLR_DDL_DROP_SUBSCRIPTION", "catalog-ddl", SblrOpcodeCategory::ddl, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("ddl.operator.create", "SBLR_DDL_CREATE_OPERATOR", "catalog-ddl", SblrOpcodeCategory::ddl, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("ddl.operator.drop", "SBLR_DDL_DROP_OPERATOR", "catalog-ddl", SblrOpcodeCategory::ddl, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("ddl.operator_class.create", "SBLR_DDL_CREATE_OPERATOR_CLASS", "catalog-ddl", SblrOpcodeCategory::ddl, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("ddl.operator_class.drop", "SBLR_DDL_DROP_OPERATOR_CLASS", "catalog-ddl", SblrOpcodeCategory::ddl, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("ddl.operator_family.create", "SBLR_DDL_CREATE_OPERATOR_FAMILY", "catalog-ddl", SblrOpcodeCategory::ddl, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("ddl.operator_family.alter", "SBLR_DDL_ALTER_OPERATOR_FAMILY", "catalog-ddl", SblrOpcodeCategory::ddl, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("ddl.operator_family.drop", "SBLR_DDL_DROP_OPERATOR_FAMILY", "catalog-ddl", SblrOpcodeCategory::ddl, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("ddl.cast.create", "SBLR_DDL_CREATE_CAST", "catalog-ddl", SblrOpcodeCategory::ddl, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("ddl.cast.drop", "SBLR_DDL_DROP_CAST", "catalog-ddl", SblrOpcodeCategory::ddl, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("ddl.collation.create", "SBLR_DDL_CREATE_COLLATION", "catalog-ddl", SblrOpcodeCategory::ddl, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("ddl.collation.alter", "SBLR_DDL_ALTER_COLLATION", "catalog-ddl", SblrOpcodeCategory::ddl, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("ddl.collation.drop", "SBLR_DDL_DROP_COLLATION", "catalog-ddl", SblrOpcodeCategory::ddl, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("ddl.extension.create", "SBLR_DDL_CREATE_EXTENSION", "catalog-ddl", SblrOpcodeCategory::ddl, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("ddl.extension.alter", "SBLR_DDL_ALTER_EXTENSION", "catalog-ddl", SblrOpcodeCategory::ddl, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("ddl.extension.drop", "SBLR_DDL_DROP_EXTENSION", "catalog-ddl", SblrOpcodeCategory::ddl, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("ddl.event_trigger.create", "SBLR_DDL_CREATE_EVENT_TRIGGER", "catalog-ddl", SblrOpcodeCategory::ddl, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("ddl.event_trigger.alter", "SBLR_DDL_ALTER_EVENT_TRIGGER", "catalog-ddl", SblrOpcodeCategory::ddl, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("ddl.event_trigger.drop", "SBLR_DDL_DROP_EVENT_TRIGGER", "catalog-ddl", SblrOpcodeCategory::ddl, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("cluster.placement_policy.create", "SBLR_CLUSTER_CREATE_PLACEMENT_POLICY", "cluster-management", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::sysarch_authorized, true, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      CanonicalEntry("cluster.placement_policy.alter", "SBLR_CLUSTER_ALTER_PLACEMENT_POLICY", "cluster-management", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::sysarch_authorized, true, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      CanonicalEntry("cluster.placement_policy.drop", "SBLR_CLUSTER_DROP_PLACEMENT_POLICY", "cluster-management", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::sysarch_authorized, true, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      CanonicalEntry("cluster.region.declare", "SBLR_CLUSTER_DECLARE_REGION", "cluster-management", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::sysarch_authorized, true, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      CanonicalEntry("cluster.availability_zone.declare", "SBLR_CLUSTER_DECLARE_AVAILABILITY_ZONE", "cluster-management", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::sysarch_authorized, true, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      CanonicalEntry("cluster.data_placement.declare", "SBLR_CLUSTER_DECLARE_DATA_PLACEMENT", "cluster-management", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::sysarch_authorized, true, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      CanonicalEntry("ddl.named_collection.create", "SBLR_DDL_CREATE_NAMED_COLLECTION", "catalog-ddl", SblrOpcodeCategory::ddl, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("ddl.named_collection.drop", "SBLR_DDL_DROP_NAMED_COLLECTION", "catalog-ddl", SblrOpcodeCategory::ddl, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("versioned.bitemporal.as_of", "SBLR_BITEMPORAL_AS_OF", "versioned-history-execution", SblrOpcodeCategory::data_mutation, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("versioned.bitemporal.as_of_valid_time", "SBLR_BITEMPORAL_AS_OF_VALID_TIME", "versioned-history-execution", SblrOpcodeCategory::data_mutation, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("versioned.bitemporal.period_overlap", "SBLR_BITEMPORAL_PERIOD_OVERLAP", "versioned-history-execution", SblrOpcodeCategory::data_mutation, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("versioned.bitemporal.for_versions_between", "SBLR_BITEMPORAL_FOR_VERSIONS_BETWEEN", "versioned-history-execution", SblrOpcodeCategory::data_mutation, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("versioned.bitemporal.show_periods", "SBLR_SHOW_BITEMPORAL_PERIODS", "versioned-history-execution", SblrOpcodeCategory::observability, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::none, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("versioned.bitemporal.show_history", "SBLR_SHOW_BITEMPORAL_HISTORY", "versioned-history-execution", SblrOpcodeCategory::observability, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::none, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("dml.for_portion_of_period", "SBLR_DML_FOR_PORTION_OF_PERIOD", "versioned-history-execution", SblrOpcodeCategory::data_mutation, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("versioned.branch.create", "SBLR_VERSIONED_BRANCH_CREATE", "versioned-history-execution", SblrOpcodeCategory::data_mutation, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("versioned.branch.delete", "SBLR_VERSIONED_BRANCH_DELETE", "versioned-history-execution", SblrOpcodeCategory::data_mutation, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("versioned.diff", "SBLR_VERSIONED_DIFF", "versioned-history-execution", SblrOpcodeCategory::data_mutation, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("versioned.tag", "SBLR_VERSIONED_TAG", "versioned-history-execution", SblrOpcodeCategory::data_mutation, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("versioned.revert", "SBLR_VERSIONED_REVERT", "versioned-history-execution", SblrOpcodeCategory::data_mutation, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("versioned.reset", "SBLR_VERSIONED_RESET", "versioned-history-execution", SblrOpcodeCategory::data_mutation, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("acceleration.llvm.policy_set", "SBLR_ACCEL_LLVM_POLICY_SET", "acceleration-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::sysarch_authorized, true),
      CanonicalEntry("acceleration.llvm.compile", "SBLR_ACCEL_LLVM_COMPILE", "acceleration-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::sysarch_authorized, true),
      CanonicalEntry("acceleration.llvm.inspect", "SBLR_ACCEL_LLVM_INSPECT", "acceleration-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::sysarch_authorized, true),
      CanonicalEntry("acceleration.llvm.invalidate", "SBLR_ACCEL_LLVM_INVALIDATE", "acceleration-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::sysarch_authorized, true),
      CanonicalEntry("acceleration.gpu.policy_set", "SBLR_ACCEL_GPU_POLICY_SET", "acceleration-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::sysarch_authorized, true),
      CanonicalEntry("acceleration.gpu.compile", "SBLR_ACCEL_GPU_COMPILE", "acceleration-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::sysarch_authorized, true),
      CanonicalEntry("acceleration.gpu.inspect", "SBLR_ACCEL_GPU_INSPECT", "acceleration-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::sysarch_authorized, true),
      CanonicalEntry("acceleration.gpu.invalidate", "SBLR_ACCEL_GPU_INVALIDATE", "acceleration-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::sysarch_authorized, true),
      CanonicalEntry("versioned.verifiable_history.prove", "SBLR_VERIFIABLE_HISTORY_PROVE", "versioned-history-execution", SblrOpcodeCategory::data_mutation, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("versioned.verify_proof_descriptor", "SBLR_VERIFY_PROOF_DESCRIPTOR", "versioned-history-execution", SblrOpcodeCategory::data_mutation, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("versioned.merge", "SBLR_VERSIONED_MERGE", "versioned-history-execution", SblrOpcodeCategory::data_mutation, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("versioned.hash_read", "SBLR_VERSIONED_HASH_READ", "versioned-history-execution", SblrOpcodeCategory::data_mutation, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("versioned.status_read", "SBLR_VERSIONED_STATUS_READ", "versioned-history-execution", SblrOpcodeCategory::data_mutation, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::object_authorized, true),
      CanonicalEntry("observability.show_version", "SBLR_OBSERVABILITY_SHOW_VERSION", "observability", SblrOpcodeCategory::observability, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::authenticated, false),
      CanonicalEntry("observability.show_database", "SBLR_OBSERVABILITY_SHOW_DATABASE", "observability", SblrOpcodeCategory::observability, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::authenticated, false),
      CanonicalEntry("observability.show_system", "SBLR_OBSERVABILITY_SHOW_SYSTEM", "observability", SblrOpcodeCategory::observability, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::authenticated, false),
      CanonicalEntry("observability.show_catalog", "SBLR_OBSERVABILITY_SHOW_CATALOG", "observability", SblrOpcodeCategory::observability, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::authenticated, false),
      CanonicalEntry("observability.show_sessions", "SBLR_OBSERVABILITY_SHOW_SESSIONS", "observability", SblrOpcodeCategory::observability, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::authenticated, false),
      CanonicalEntry("observability.show_transactions", "SBLR_OBSERVABILITY_SHOW_TRANSACTIONS", "observability", SblrOpcodeCategory::observability, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::authenticated, false),
      CanonicalEntry("observability.show_locks", "SBLR_OBSERVABILITY_SHOW_LOCKS", "observability", SblrOpcodeCategory::observability, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::authenticated, false),
      CanonicalEntry("observability.show_statements", "SBLR_OBSERVABILITY_SHOW_STATEMENTS", "observability", SblrOpcodeCategory::observability, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::authenticated, false),
      CanonicalEntry("observability.show_jobs", "SBLR_OBSERVABILITY_SHOW_JOBS", "observability", SblrOpcodeCategory::observability, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::authenticated, false),
      CanonicalEntry("observability.show_management", "SBLR_OBSERVABILITY_SHOW_MANAGEMENT", "observability", SblrOpcodeCategory::observability, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::authenticated, false),
      CanonicalEntry("observability.show_diagnostics", "SBLR_OBSERVABILITY_SHOW_DIAGNOSTICS", "observability", SblrOpcodeCategory::observability, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::authenticated, false),
      CanonicalEntry("observability.show_diagnostics_extended", "SBLR_OBSERVABILITY_SHOW_DIAGNOSTICS_EXTENDED", "observability", SblrOpcodeCategory::observability, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::authenticated, false),
      CanonicalEntry("observability.show_archive_replication", "SBLR_OBSERVABILITY_SHOW_ARCHIVE_REPLICATION", "observability", SblrOpcodeCategory::observability, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::authenticated, false),
      CanonicalEntry("observability.show_agents_extended", "SBLR_OBSERVABILITY_SHOW_AGENTS_EXTENDED", "observability", SblrOpcodeCategory::observability, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::authenticated, false),
      CanonicalEntry("observability.show_filespace_extended", "SBLR_OBSERVABILITY_SHOW_FILESPACE_EXTENDED", "observability", SblrOpcodeCategory::observability, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::authenticated, false),
      CanonicalEntry("observability.show_acceleration", "SBLR_OBSERVABILITY_SHOW_ACCELERATION", "observability", SblrOpcodeCategory::observability, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::authenticated, false),
      CanonicalEntry("observability.explain_operation", "SBLR_OBSERVABILITY_EXPLAIN_OPERATION", "diagnostics-and-metrics", SblrOpcodeCategory::observability, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::authenticated, true),
      CanonicalEntry("observability.show_decision_service", "SBLR_OBSERVABILITY_SHOW_DECISION_SERVICE", "observability", SblrOpcodeCategory::observability, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::authenticated, false),
      CanonicalEntry("observability.show_acceleration_extended", "SBLR_OBSERVABILITY_SHOW_ACCELERATION_EXTENDED", "observability", SblrOpcodeCategory::observability, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::authenticated, false),
      CanonicalEntry("observability.show_metrics", "SBLR_OBSERVABILITY_SHOW_METRICS", "observability", SblrOpcodeCategory::observability, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::authenticated, false),
      // SBSFC-028R-A/B/C: native diagnostic-emission primitives. SIGNAL,
      // RAISE, and RESIGNAL construct and emit an SB_DIAG envelope from
      // user-supplied SQLSTATE and MESSAGE_TEXT. Category is observability
      // because the engine path is read-only — these don't mutate database
      // state, they emit a diagnostic vector. No transaction or cluster
      // context is required; security context applies via standard auth.
      Entry("general.signal_diagnostic", "SBLR_GENERAL_SIGNAL_DIAGNOSTIC", SblrOpcodeCategory::observability, SblrOpcodeSupport::implemented),
      Entry("general.raise_diagnostic", "SBLR_GENERAL_RAISE_DIAGNOSTIC", SblrOpcodeCategory::observability, SblrOpcodeSupport::implemented),
      Entry("general.resignal_diagnostic", "SBLR_GENERAL_RESIGNAL_DIAGNOSTIC", SblrOpcodeCategory::observability, SblrOpcodeSupport::implemented),
      Entry("general.procedural_operation", "SBLR_GENERAL_PROCEDURAL_OPERATION", SblrOpcodeCategory::observability, SblrOpcodeSupport::implemented, true, true),
      Entry("dml.insert_rows", "SBLR_DML_INSERT_ROWS", SblrOpcodeCategory::dml, SblrOpcodeSupport::implemented, true, true),
      Entry("dml.select_rows", "SBLR_DML_SELECT_ROWS", SblrOpcodeCategory::dml, SblrOpcodeSupport::implemented, true, true),
      Entry("dml.update_rows", "SBLR_DML_UPDATE_ROWS", SblrOpcodeCategory::dml, SblrOpcodeSupport::implemented, true, true),
      Entry("dml.delete_rows", "SBLR_DML_DELETE_ROWS", SblrOpcodeCategory::dml, SblrOpcodeSupport::implemented, true, true),
      Entry("dml.merge_rows", "SBLR_DML_MERGE_ROWS", SblrOpcodeCategory::dml, SblrOpcodeSupport::implemented, true, true),
      Entry("dml.plan_import_rows", "SBLR_DML_PLAN_IMPORT_ROWS", SblrOpcodeCategory::dml, SblrOpcodeSupport::implemented, true, true),
      Entry("dml.normalize_import_reject_model", "SBLR_DML_IMPORT_REJECT_MODEL", SblrOpcodeCategory::dml, SblrOpcodeSupport::implemented, true, true),
      Entry("dml.normalize_import_checkpoint_model", "SBLR_DML_IMPORT_CHECKPOINT_MODEL", SblrOpcodeCategory::dml, SblrOpcodeSupport::implemented, true, true),
      Entry("dml.execute_import_rows", "SBLR_DML_EXECUTE_IMPORT_ROWS", SblrOpcodeCategory::dml, SblrOpcodeSupport::implemented, true, true),
      Entry("artifact.export_catalog", "SBLR_ARTIFACT_EXPORT_CATALOG", SblrOpcodeCategory::artifact, SblrOpcodeSupport::implemented, true, true),
      Entry("artifact.import_catalog", "SBLR_ARTIFACT_IMPORT_CATALOG", SblrOpcodeCategory::artifact, SblrOpcodeSupport::implemented, true, true),
      Entry("artifact.external_git.export_snapshot", "SBLR_ARTIFACT_EXTERNAL_GIT_EXPORT_SNAPSHOT", SblrOpcodeCategory::artifact, SblrOpcodeSupport::implemented, true, true),
      Entry("artifact.external_git.diff_snapshot", "SBLR_ARTIFACT_EXTERNAL_GIT_DIFF_SNAPSHOT", SblrOpcodeCategory::artifact, SblrOpcodeSupport::implemented, true, true),
      Entry("artifact.external_git.rollback_plan", "SBLR_ARTIFACT_EXTERNAL_GIT_ROLLBACK_PLAN", SblrOpcodeCategory::artifact, SblrOpcodeSupport::implemented, true, true),
      Entry("extensibility.inspect_gpu_capability", "SBLR_EXTENSIBILITY_INSPECT_GPU_CAPABILITY", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented, true, false),
      Entry("transaction.begin", "SBLR_TRANSACTION_BEGIN", SblrOpcodeCategory::transaction, SblrOpcodeSupport::implemented, true, false),
      Entry("transaction.set_characteristics", "SBLR_TRANSACTION_SET_CHARACTERISTICS", SblrOpcodeCategory::transaction, SblrOpcodeSupport::implemented, true, false),
      Entry("transaction.commit", "SBLR_TRANSACTION_COMMIT", SblrOpcodeCategory::transaction, SblrOpcodeSupport::implemented, true, true),
      Entry("transaction.rollback", "SBLR_TRANSACTION_ROLLBACK", SblrOpcodeCategory::transaction, SblrOpcodeSupport::implemented, true, true),
      Entry("transaction.prepare", "SBLR_TRANSACTION_PREPARE", SblrOpcodeCategory::transaction, SblrOpcodeSupport::implemented, true, true, false),
      Entry("transaction.create_savepoint", "SBLR_TRANSACTION_CREATE_SAVEPOINT", SblrOpcodeCategory::transaction, SblrOpcodeSupport::implemented, true, true),
      Entry("transaction.release_savepoint", "SBLR_TRANSACTION_RELEASE_SAVEPOINT", SblrOpcodeCategory::transaction, SblrOpcodeSupport::implemented, true, true),
      Entry("transaction.rollback_to_savepoint", "SBLR_TRANSACTION_ROLLBACK_TO_SAVEPOINT", SblrOpcodeCategory::transaction, SblrOpcodeSupport::implemented, true, true),
      Entry("transaction.execute_block", "SBLR_TRANSACTION_EXECUTE_BLOCK", SblrOpcodeCategory::transaction, SblrOpcodeSupport::implemented, true, true),
      Entry("transaction.lock_table", "SBLR_TXN_LOCK_TABLE", SblrOpcodeCategory::transaction, SblrOpcodeSupport::implemented, true, true),
      Entry("transaction.unlock_table", "SBLR_TXN_UNLOCK_TABLE", SblrOpcodeCategory::transaction, SblrOpcodeSupport::implemented, true, true),
      Entry("transaction.lock_named", "SBLR_TXN_LOCK_NAMED", SblrOpcodeCategory::transaction, SblrOpcodeSupport::implemented, true, true),
      Entry("transaction.unlock_named", "SBLR_TXN_UNLOCK_NAMED", SblrOpcodeCategory::transaction, SblrOpcodeSupport::implemented, true, true),
      Entry("query.bind_expression", "SBLR_QUERY_BIND_EXPRESSION", SblrOpcodeCategory::query, SblrOpcodeSupport::implemented, true, false),
      Entry("query.bind_predicate", "SBLR_QUERY_BIND_PREDICATE", SblrOpcodeCategory::query, SblrOpcodeSupport::implemented, true, false),
      Entry("query.bind_projection", "SBLR_QUERY_BIND_PROJECTION", SblrOpcodeCategory::query, SblrOpcodeSupport::implemented, true, false),
      Entry("query.cast_value", "SBLR_QUERY_CAST_VALUE", SblrOpcodeCategory::query, SblrOpcodeSupport::implemented, true, false),
      Entry("query.extract_value", "SBLR_QUERY_EXTRACT_VALUE", SblrOpcodeCategory::query, SblrOpcodeSupport::implemented, true, false),
      Entry("query.set_operation", "SBLR_QUERY_SET_OPERATION", SblrOpcodeCategory::query, SblrOpcodeSupport::implemented, true, false),
      CanonicalEntry("query.apply_numeric_operation", "SBLR_QUERY_APPLY_NUMERIC_OPERATION", "expression-eval", SblrOpcodeCategory::query, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, false),
      Entry("query.canonicalize_document_value", "SBLR_QUERY_CANONICALIZE_DOCUMENT_VALUE", SblrOpcodeCategory::query, SblrOpcodeSupport::implemented, true, false),
      CanonicalEntry("query.evaluate_advanced_datatype_family", "SBLR_QUERY_EVALUATE_ADVANCED_DATATYPE_FAMILY", "expression-eval", SblrOpcodeCategory::query, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::object_authorized, false),
      Entry("query.validate_domain_value", "SBLR_QUERY_VALIDATE_DOMAIN_VALUE", SblrOpcodeCategory::query, SblrOpcodeSupport::implemented, true, false),
      Entry("query.invoke_domain_method", "SBLR_QUERY_INVOKE_DOMAIN_METHOD", SblrOpcodeCategory::query, SblrOpcodeSupport::implemented, true, false),
      Entry("query.evaluate_projection", "SBLR_QUERY_EVALUATE_PROJECTION", SblrOpcodeCategory::query, SblrOpcodeSupport::implemented, true, true),
      Entry("query.plan_operation", "SBLR_QUERY_PLAN_OPERATION", SblrOpcodeCategory::query, SblrOpcodeSupport::deprecated_refusal, true, false, false, "QOW-DIAG-RELATIONAL-ROOT-NONCANONICAL"),
      Entry("catalog.resolve_name", "SBLR_CATALOG_RESOLVE_NAME", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented),
      Entry("catalog.map_uuid_to_name", "SBLR_CATALOG_MAP_UUID_TO_NAME", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented),
      Entry("catalog.lookup_object", "SBLR_CATALOG_LOOKUP_OBJECT", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented),
      Entry("catalog.list_children", "SBLR_CATALOG_LIST_CHILDREN", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented),
      Entry("catalog.get_dependencies", "SBLR_CATALOG_GET_DEPENDENCIES", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented),
      Entry("catalog.get_descriptor", "SBLR_CATALOG_GET_DESCRIPTOR", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented),
      Entry("catalog.mutation.create_materialized_view", "SBLR_CATALOG_MUTATION_CREATE_MATERIALIZED_VIEW", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, true, true),
      Entry("catalog.mutation.create_cast", "SBLR_CATALOG_MUTATION_CREATE_CAST", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, true, true),
      Entry("catalog.mutation.create_server", "SBLR_CATALOG_MUTATION_CREATE_SERVER", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, true, true),
      Entry("catalog.mutation.show_storage_buffer_io_index", "SBLR_CATALOG_MUTATION_SHOW_STORAGE_BUFFER_IO_INDEX", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, true, true),
      Entry("catalog.mutation.alter_time_series", "SBLR_CATALOG_MUTATION_ALTER_TIME_SERIES", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, true, true),
      Entry("catalog.mutation.create_key_value_store", "SBLR_CATALOG_MUTATION_CREATE_KEY_VALUE_STORE", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, true, true),
      Entry("catalog.mutation.cypher_create", "SBLR_CATALOG_MUTATION_CYPHER_CREATE", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, true, true),
      Entry("catalog.mutation.graph_create_node", "SBLR_CATALOG_MUTATION_GRAPH_CREATE_NODE", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, true, true),
      Entry("catalog.mutation.create_bucket", "SBLR_CATALOG_MUTATION_CREATE_BUCKET", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, true, true),
      Entry("catalog.mutation.alter_filespace", "SBLR_CATALOG_MUTATION_ALTER_FILESPACE", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, true, true),
      Entry("catalog.mutation.create_operation", "SBLR_CATALOG_MUTATION_CREATE_OPERATION", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, true, true),
      Entry("catalog.mutation.create_operator", "SBLR_CATALOG_MUTATION_CREATE_OPERATOR", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, true, true),
      Entry("catalog.mutation.create_event_trigger", "SBLR_CATALOG_MUTATION_CREATE_EVENT_TRIGGER", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, true, true),
      Entry("catalog.mutation.create_package_body", "SBLR_CATALOG_MUTATION_CREATE_PACKAGE_BODY", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, true, true),
      Entry("catalog.mutation.create_aggregate", "SBLR_CATALOG_MUTATION_CREATE_AGGREGATE", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, true, true),
      Entry("catalog.mutation.alter_routine", "SBLR_CATALOG_MUTATION_ALTER_ROUTINE", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, true, true),
      Entry("catalog.mutation.create_graph", "SBLR_CATALOG_MUTATION_CREATE_GRAPH", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, true, true),
      Entry("catalog.mutation.create_dictionary", "SBLR_CATALOG_MUTATION_CREATE_DICTIONARY", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, true, true),
      Entry("catalog.mutation.create_package", "SBLR_CATALOG_MUTATION_CREATE_PACKAGE", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, true, true),
      Entry("catalog.mutation.alter_udr", "SBLR_CATALOG_MUTATION_ALTER_UDR", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, true, true),
      Entry("catalog.mutation.graph_create_edge", "SBLR_CATALOG_MUTATION_GRAPH_CREATE_EDGE", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, true, true),
      Entry("catalog.mutation.create_filespace", "SBLR_CATALOG_MUTATION_CREATE_FILESPACE", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, true, true),
      Entry("catalog.mutation.create_filespace_agent", "SBLR_CATALOG_MUTATION_CREATE_FILESPACE_AGENT", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, true, true),
      Entry("catalog.mutation.create_quota", "SBLR_CATALOG_MUTATION_CREATE_QUOTA", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, true, true),
      Entry("catalog.mutation.alter_key_value_store", "SBLR_CATALOG_MUTATION_ALTER_KEY_VALUE_STORE", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, true, true),
      Entry("catalog.mutation.alter_subject", "SBLR_CATALOG_MUTATION_ALTER_SUBJECT", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, true, true),
      Entry("catalog.mutation.graph_create_index", "SBLR_CATALOG_MUTATION_GRAPH_CREATE_INDEX", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, true, true),
      Entry("catalog.mutation.create_binding", "SBLR_CATALOG_MUTATION_CREATE_BINDING", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, true, true),
      Entry("catalog.mutation.create_monitor", "SBLR_CATALOG_MUTATION_CREATE_MONITOR", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, true, true),
      Entry("catalog.mutation.refresh_materialized_view", "SBLR_CATALOG_MUTATION_REFRESH_MATERIALIZED_VIEW", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, true, true),
      Entry("catalog.mutation.create_transform", "SBLR_CATALOG_MUTATION_CREATE_TRANSFORM", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, true, true),
      Entry("catalog.mutation.create_secret", "SBLR_CATALOG_MUTATION_CREATE_SECRET", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, true, true),
      Entry("catalog.mutation.alter_reference", "SBLR_CATALOG_MUTATION_ALTER_REFERENCE", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, true, true),
      Entry("catalog.mutation.create_pipeline", "SBLR_CATALOG_MUTATION_CREATE_PIPELINE", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, true, true),
      Entry("catalog.mutation.create_collation", "SBLR_CATALOG_MUTATION_CREATE_COLLATION", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, true, true),
      Entry("catalog.mutation.create_type", "SBLR_CATALOG_MUTATION_CREATE_TYPE", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, true, true),
      Entry("catalog.mutation.alter_type", "SBLR_CATALOG_MUTATION_ALTER_TYPE", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, true, true),
      Entry("catalog.mutation.drop_type", "SBLR_CATALOG_MUTATION_DROP_TYPE", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, true, true),
      Entry("catalog.type.show", "SBLR_SHOW_TYPE", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, true, false),
      Entry("catalog.type.show_all", "SBLR_SHOW_TYPES", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, true, false),
      Entry("catalog.mutation.alter_view", "SBLR_CATALOG_MUTATION_ALTER_VIEW", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, true, true),
      Entry("catalog.mutation.create_udr", "SBLR_CATALOG_MUTATION_CREATE_UDR", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, true, true),
      Entry("catalog.mutation.create_tenant", "SBLR_CATALOG_MUTATION_CREATE_TENANT", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, true, true),
      Entry("catalog.mutation.create_time_series", "SBLR_CATALOG_MUTATION_CREATE_TIME_SERIES", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, true, true),
      Entry("catalog.mutation.create_document_collection", "SBLR_CATALOG_MUTATION_CREATE_DOCUMENT_COLLECTION", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, true, true),
      Entry("query.structured_type.constructor", "SBLR_QUERY_STRUCTURED_TYPE_CONSTRUCTOR", SblrOpcodeCategory::query, SblrOpcodeSupport::implemented, true, true),
      Entry("query.structured_type.cast", "SBLR_QUERY_STRUCTURED_TYPE_CAST", SblrOpcodeCategory::query, SblrOpcodeSupport::implemented, true, true),
      Entry("query.structured_type.compare", "SBLR_QUERY_STRUCTURED_TYPE_COMPARE", SblrOpcodeCategory::query, SblrOpcodeSupport::implemented, true, true),
      Entry("query.structured_type.serialize", "SBLR_QUERY_STRUCTURED_TYPE_SERIALIZE", SblrOpcodeCategory::query, SblrOpcodeSupport::implemented, true, true),
      Entry("ddl.create_database", "SBLR_DDL_CREATE_DATABASE", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, true, false),
      Entry("ddl.create_index_template", "SBLR_DDL_CREATE_INDEX_TEMPLATE", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, true, true),
      CanonicalEntry("engine.op.ddl_create_sequence", "SBLR_DDL_CREATE_SEQUENCE", "catalog-ddl", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      Entry("ddl.create_statistics", "SBLR_DDL_CREATE_STATISTICS", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, true, true),
      CanonicalEntry("engine.op.ddl_create_view", "SBLR_DDL_CREATE_VIEW", "catalog-ddl", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true),
      CanonicalEntry("engine.op.ddl_create_trigger", "SBLR_DDL_CREATE_TRIGGER", "catalog-ddl", SblrOpcodeCategory::catalog, SblrOpcodeSupport::local_profile_refusal, SblrOpcodeTransactionEffect::local_or_cluster_write, SblrOpcodeSecurityClass::admin_authorized, true, false, "PROFILE.BUILTIN_PROFILE_UNAVAILABLE"),
      Entry("ddl.constraint.create", "SBLR_DDL_CONSTRAINT_CREATE", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, true, true),
      Entry("ddl.constraint.alter", "SBLR_DDL_CONSTRAINT_ALTER", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, true, true),
      Entry("ddl.constraint.drop", "SBLR_DDL_CONSTRAINT_DROP", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, true, true),
      Entry("ddl.alter_object", "SBLR_DDL_ALTER_OBJECT", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, true, true),
      Entry("ddl.drop_object", "SBLR_DDL_DROP_OBJECT", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, true, true),
      Entry("ddl.comment_on_object", "SBLR_DDL_COMMENT_ON", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, true, true),
      CanonicalEntry("lifecycle.create_database", "SBLR_LIFECYCLE_CREATE_DATABASE", "database-management", SblrOpcodeCategory::management, SblrOpcodeSupport::local_profile_refusal, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::admin_authorized, false, false, "SB_ENGINE_API_LIFECYCLE_BOOTSTRAP_REQUIRED"),
      CanonicalEntry("lifecycle.open_database", "SBLR_LIFECYCLE_OPEN_DATABASE", "database-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::admin_authorized, false, false),
      CanonicalEntry("lifecycle.attach_database", "SBLR_LIFECYCLE_ATTACH_DATABASE", "database-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::admin_authorized, false, false),
      CanonicalEntry("lifecycle.detach_database", "SBLR_LIFECYCLE_DETACH_DATABASE", "database-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::admin_authorized, true, false),
      CanonicalEntry("lifecycle.enter_maintenance", "SBLR_LIFECYCLE_ENTER_MAINTENANCE", "database-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::admin_authorized, false, false),
      CanonicalEntry("lifecycle.exit_maintenance", "SBLR_LIFECYCLE_EXIT_MAINTENANCE", "database-management", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::admin_authorized, true, false),
      Entry("lifecycle.enter_restricted_open", "SBLR_LIFECYCLE_ENTER_RESTRICTED_OPEN", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("lifecycle.exit_restricted_open", "SBLR_LIFECYCLE_EXIT_RESTRICTED_OPEN", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("lifecycle.inspect_database", "SBLR_LIFECYCLE_INSPECT_DATABASE", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, false, false),
      Entry("lifecycle.verify_database", "SBLR_LIFECYCLE_VERIFY_DATABASE", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("lifecycle.repair_database", "SBLR_LIFECYCLE_REPAIR_DATABASE", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("lifecycle.shutdown_database", "SBLR_LIFECYCLE_SHUTDOWN_DATABASE", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("lifecycle.shutdown_force", "SBLR_LIFECYCLE_SHUTDOWN_FORCE", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("lifecycle.shutdown_acknowledge", "SBLR_LIFECYCLE_SHUTDOWN_ACKNOWLEDGE", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("lifecycle.drop_database", "SBLR_LIFECYCLE_DROP_DATABASE", SblrOpcodeCategory::management, SblrOpcodeSupport::local_profile_refusal, true, false, false, "PROFILE.BUILTIN_PROFILE_UNAVAILABLE"),
      Entry("management.inspect_config", "SBLR_MANAGEMENT_INSPECT_CONFIG", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("management.set_config", "SBLR_MANAGEMENT_SET_CONFIG", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("management.reset_config", "SBLR_MANAGEMENT_RESET_CONFIG", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("management.inspect_runtime", "SBLR_MANAGEMENT_INSPECT_RUNTIME", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("management.control_runtime", "SBLR_MANAGEMENT_CONTROL_RUNTIME", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("management.prepare_support_bundle", "SBLR_MANAGEMENT_PREPARE_SUPPORT_BUNDLE", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("agents.list", "SBLR_AGENTS_LIST", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("agents.show", "SBLR_AGENTS_SHOW", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("agents.start", "SBLR_AGENTS_START", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("agents.stop", "SBLR_AGENTS_STOP", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("agents.pause", "SBLR_AGENTS_PAUSE", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("agents.resume", "SBLR_AGENTS_RESUME", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("agents.configure", "SBLR_AGENTS_CONFIGURE", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("agents.run", "SBLR_AGENTS_RUN", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("agents.dry_run", "SBLR_AGENTS_DRY_RUN", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("agents.override", "SBLR_AGENTS_OVERRIDE", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("sys.agents", "SBLR_SYS_AGENTS", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("cluster.sys.agents", "SBLR_CLUSTER_SYS_AGENTS", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, true, false, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      Entry("agents.request_page_preallocation", "SBLR_AGENT_REQUEST_PAGE_PREALLOCATION", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, true),
      Entry("agents.request_page_relocation", "SBLR_AGENT_REQUEST_PAGE_RELOCATION", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, true),
      Entry("agents.request_filespace_growth", "SBLR_AGENT_REQUEST_FILESPACE_GROWTH", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, true),
      Entry("agents.notify_filespace_shrink_readiness", "SBLR_AGENT_NOTIFY_FILESPACE_SHRINK_READINESS", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, true),
      Entry("agents.request_index_delta_merge", "SBLR_AGENT_REQUEST_INDEX_DELTA_MERGE", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, true),
      Entry("agents.request_index_rebuild_or_shadow_build", "SBLR_AGENT_REQUEST_INDEX_REBUILD_OR_SHADOW_BUILD", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, true),
      Entry("agents.metrics.get", "SBLR_AGENT_METRICS_GET", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("agents.policy.get", "SBLR_AGENT_POLICY_GET", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("agents.evidence.list", "SBLR_AGENT_EVIDENCE_LIST", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("agents.audit.list", "SBLR_AGENT_AUDIT_LIST", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("agents.actions.list", "SBLR_AGENT_ACTION_LIST", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("agents.overrides.list", "SBLR_AGENT_OVERRIDE_LIST", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("agents.drain", "SBLR_AGENT_LIFECYCLE_DRAIN", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("agents.restart", "SBLR_AGENT_LIFECYCLE_RESTART", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("agents.enable", "SBLR_AGENT_LIFECYCLE_ENABLE", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("agents.disable", "SBLR_AGENT_LIFECYCLE_DISABLE", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("agents.quarantine", "SBLR_AGENT_QUARANTINE", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("agents.unquarantine", "SBLR_AGENT_UNQUARANTINE", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("agents.policy.attach", "SBLR_AGENT_POLICY_ATTACH", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("agents.policy.detach", "SBLR_AGENT_POLICY_DETACH", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("agents.policy.validate", "SBLR_AGENT_POLICY_VALIDATE", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("agents.policy.simulate", "SBLR_AGENT_POLICY_SIMULATE", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("agents.policy.apply", "SBLR_AGENT_POLICY_APPLY", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("agents.policy.rollback", "SBLR_AGENT_POLICY_ROLLBACK", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("agents.action.approve", "SBLR_AGENT_ACTION_APPROVE", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("agents.action.cancel", "SBLR_AGENT_ACTION_CANCEL", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("agents.action.retry", "SBLR_AGENT_ACTION_RETRY", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("agents.action.suppress", "SBLR_AGENT_ACTION_SUPPRESS", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("agents.override.create", "SBLR_AGENT_OVERRIDE_CREATE", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("agents.override.update", "SBLR_AGENT_OVERRIDE_UPDATE", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("agents.override.drop", "SBLR_AGENT_OVERRIDE_DROP", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("agents.set_mode", "SBLR_AGENT_SET_MODE", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("filespaces.show", "SBLR_SHOW_FILESPACES", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("filespaces.health.show", "SBLR_SHOW_FILESPACE_HEALTH", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("filespaces.capacity.show", "SBLR_SHOW_FILESPACE_CAPACITY", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("pages.allocation.show", "SBLR_SHOW_PAGE_ALLOCATION", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("pages.allocation.family.show", "SBLR_SHOW_PAGE_ALLOCATION_BY_FAMILY", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("pages.relocation_backlog.show", "SBLR_SHOW_PAGE_RELOCATION_BACKLOG", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("filespaces.shrink_readiness.show", "SBLR_SHOW_FILESPACE_SHRINK_READINESS", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("cluster.agent.list", "SBLR_CLUSTER_AGENT_LIST", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, true, false, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      Entry("cluster.agent.get", "SBLR_CLUSTER_AGENT_GET", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, true, false, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      Entry("cluster.agent.control", "SBLR_CLUSTER_AGENT_CONTROL", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, true, false, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      Entry("jobs.scheduler.create_job", "SBLR_JOBS_CREATE_JOB", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("jobs.scheduler.alter_job", "SBLR_JOBS_ALTER_JOB", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("jobs.scheduler.create_schedule", "SBLR_JOBS_CREATE_SCHEDULE", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("jobs.scheduler.alter_schedule", "SBLR_JOBS_ALTER_SCHEDULE", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("jobs.scheduler.run_job", "SBLR_JOBS_RUN_JOB", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("jobs.scheduler.pause_job", "SBLR_JOBS_PAUSE_JOB", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("jobs.scheduler.resume_job", "SBLR_JOBS_RESUME_JOB", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("jobs.scheduler.cancel_job", "SBLR_JOBS_CANCEL_JOB", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("backup_archive.start_logical_backup", "SBLR_BACKUP_ARCHIVE_START_LOGICAL_BACKUP", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("backup_archive.restore_logical_backup", "SBLR_BACKUP_ARCHIVE_RESTORE_LOGICAL_BACKUP", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, true),
      Entry("backup_archive.package_delta_stream", "SBLR_BACKUP_ARCHIVE_PACKAGE_DELTA_STREAM", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("backup_archive.apply_delta_stream", "SBLR_BACKUP_ARCHIVE_APPLY_DELTA_STREAM", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, true),
      CanonicalEntry("event.channel.create", "SBLR_EVENT_CHANNEL_CREATE", "event-notification", SblrOpcodeCategory::catalog, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::catalog_write, SblrOpcodeSecurityClass::event_admin, true),
      CanonicalEntry("event.channel.listen", "SBLR_EVENT_CHANNEL_LISTEN", "event-notification", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::none, SblrOpcodeSecurityClass::event_admin, true),
      CanonicalEntry("event.channel.unlisten", "SBLR_EVENT_CHANNEL_UNLISTEN", "event-notification", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::none, SblrOpcodeSecurityClass::event_admin, true),
      CanonicalEntry("event.channel.notify", "SBLR_EVENT_CHANNEL_NOTIFY", "event-notification", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::management, SblrOpcodeSecurityClass::event_admin, true),
      CanonicalEntry("event.subscription.list", "SBLR_EVENT_SUBSCRIPTION_LIST", "event-notification", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::none, SblrOpcodeSecurityClass::event_admin, false),
      CanonicalEntry("event.delivery.poll", "SBLR_EVENT_DELIVERY_POLL", "event-notification", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::none, SblrOpcodeSecurityClass::event_admin, false),
      CanonicalEntry("event.delivery.ack", "SBLR_EVENT_DELIVERY_ACK", "event-notification", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::none, SblrOpcodeSecurityClass::event_admin, false),
      Entry("session.prepare_statement", "SBLR_SESSION_PREPARE_STATEMENT", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("session.execute_prepared_statement", "SBLR_SESSION_EXECUTE_PREPARED_STATEMENT", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("session.deallocate_prepared_statement", "SBLR_SESSION_DEALLOCATE_PREPARED_STATEMENT", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("session.cursor_open", "SBLR_SESSION_CURSOR_OPEN", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("session.cursor_fetch", "SBLR_SESSION_CURSOR_FETCH", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("session.cursor_close", "SBLR_SESSION_CURSOR_CLOSE", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("session.notification.unlisten", "SBLR_EVENT_CHANNEL_UNLISTEN", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, true),
      CanonicalEntry("session.notification.unlisten_all", "SBLR_EVENT_CHANNEL_UNLISTEN_ALL", "event-notification", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, SblrOpcodeTransactionEffect::none, SblrOpcodeSecurityClass::event_admin, true),
      Entry("cluster.inspect_state", "SBLR_CLUSTER_INSPECT_STATE", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, true, false, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      Entry("cluster.inspect_routing_plan", "SBLR_CLUSTER_INSPECT_ROUTING_PLAN", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, true, false, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      Entry("cluster.control_cluster", "SBLR_CLUSTER_CONTROL_CLUSTER", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, true, false, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      CanonicalEntry("cluster.inspect_provider", "SBLR_CLUSTER_INSPECT_PROVIDER", "cluster-management", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, SblrOpcodeTransactionEffect::read, SblrOpcodeSecurityClass::cluster_authorized, true, true, "CLUSTER.GATEWAY.CLUSTER_CONTEXT_REQUIRED"),
      Entry("cluster.place_object", "SBLR_CLUSTER_PLACE_OBJECT", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, true, true, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      Entry("cluster.inspect_replication", "SBLR_CLUSTER_INSPECT_REPLICATION", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, true, false, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      Entry("cluster.prepare_remote_participant_insert", "SBLR_CLUSTER_PREPARE_REMOTE_PARTICIPANT_INSERT", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, true, true, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      Entry("cluster.validate_insert_route_fence", "SBLR_CLUSTER_VALIDATE_INSERT_ROUTE_FENCE", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, true, true, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      Entry("cluster.profile_operation", "SBLR_CLUSTER_PROFILE_OPERATION", SblrOpcodeCategory::cluster, SblrOpcodeSupport::implemented, true, false, false),
      Entry("op.show.cluster_gpu_placement", "SBLR_OP_SHOW_CLUSTER_GPU_PLACEMENT", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, true, false, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      Entry("op.show.cluster.state", "SBLR_OP_SHOW_CLUSTER_STATE", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, true, false, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      Entry("op.show.cluster.topology", "SBLR_OP_SHOW_CLUSTER_TOPOLOGY", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, true, false, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      Entry("op.show.cluster.members", "SBLR_OP_SHOW_CLUSTER_MEMBERS", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, true, false, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      Entry("op.show.cluster.capabilities", "SBLR_OP_SHOW_CLUSTER_CAPABILITIES", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, true, false, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      Entry("op.show.cluster.routing_plan", "SBLR_OP_SHOW_CLUSTER_ROUTING_PLAN", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, true, false, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      Entry("op.show.cluster.admission_status", "SBLR_OP_SHOW_CLUSTER_ADMISSION_STATUS", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, true, false, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      Entry("op.show.cluster.route_epoch", "SBLR_OP_SHOW_CLUSTER_ROUTE_EPOCH", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, true, false, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      Entry("op.show.cluster.decisions", "SBLR_OP_SHOW_CLUSTER_DECISIONS", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, true, false, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      Entry("op.show.cluster.limbo", "SBLR_OP_SHOW_CLUSTER_LIMBO", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, true, false, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      Entry("op.show.cluster.recovery", "SBLR_OP_SHOW_CLUSTER_RECOVERY", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, true, false, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      Entry("op.show.cluster.slo", "SBLR_OP_SHOW_CLUSTER_SLO", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, true, false, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      Entry("op.show.cluster.error_budget", "SBLR_OP_SHOW_CLUSTER_ERROR_BUDGET", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, true, false, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      Entry("op.show.cluster.readiness", "SBLR_OP_SHOW_CLUSTER_READINESS", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, true, false, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      Entry("op.show.cluster.alerts", "SBLR_OP_SHOW_CLUSTER_ALERTS", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, true, false, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      Entry("op.show.cluster.replication", "SBLR_OP_SHOW_CLUSTER_REPLICATION", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, true, false, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      Entry("op.show.cluster.shards", "SBLR_OP_SHOW_CLUSTER_SHARDS", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, true, false, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      Entry("op.show.cluster.placement", "SBLR_OP_SHOW_CLUSTER_PLACEMENT", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, true, false, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      Entry("op.show.cluster.archive", "SBLR_OP_SHOW_CLUSTER_ARCHIVE", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, true, false, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      Entry("op.cluster.route_publish", "SBLR_OP_CLUSTER_ROUTE_PUBLISH", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, true, false, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      Entry("op.cluster.placement_move", "SBLR_OP_CLUSTER_PLACEMENT_MOVE", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, true, false, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      Entry("op.cluster.admission_tune", "SBLR_OP_CLUSTER_ADMISSION_TUNE", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, true, false, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      Entry("op.cluster.recovery_resolution", "SBLR_OP_CLUSTER_RECOVERY_RESOLUTION", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, true, false, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      Entry("op.cluster.config", "SBLR_OP_CLUSTER_CONFIG", SblrOpcodeCategory::cluster, SblrOpcodeSupport::cluster_refusal, true, false, true, "SB_DIAG_CLUSTER_TXN_UNAVAILABLE"),
      Entry("storage.manage_operation", "SBLR_STORAGE_MANAGEMENT_OPERATION", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("extensibility.compile_llvm_module", "SBLR_EXTENSIBILITY_COMPILE_LLVM_MODULE", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented, true, false, false, "SB_DIAG_LLVM_REQUIRED_UNAVAILABLE"),
      Entry("extensibility.register_udr_package", "SBLR_EXTENSIBILITY_REGISTER_UDR_PACKAGE", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented),
      Entry("extensibility.alter_udr_package", "SBLR_EXTENSIBILITY_ALTER_UDR_PACKAGE", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented),
      Entry("extensibility.load_udr_package", "SBLR_EXTENSIBILITY_LOAD_UDR_PACKAGE", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented),
      Entry("extensibility.unload_udr_package", "SBLR_EXTENSIBILITY_UNLOAD_UDR_PACKAGE", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented),
      Entry("extensibility.drop_udr_package", "SBLR_EXTENSIBILITY_DROP_UDR_PACKAGE", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented),
      Entry("extensibility.inspect_udr_packages", "SBLR_EXTENSIBILITY_INSPECT_UDR_PACKAGES", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented),
      Entry("extensibility.invoke_udr_package", "SBLR_UDR_INVOKE", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented),
      Entry("extensibility.register_parser_package", "SBLR_EXTENSIBILITY_REGISTER_PARSER_PACKAGE", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented, true, true),
      Entry("security.evaluate_deep_enforcement", "SBLR_SECURITY_EVALUATE_DEEP_ENFORCEMENT", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, true, false),
      Entry("security.create_identity", "SBLR_SECURITY_CREATE_IDENTITY", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, true, true),
      Entry("security.alter_identity", "SBLR_SECURITY_ALTER_IDENTITY", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, true, true),
      Entry("security.grant_right", "SBLR_SECURITY_GRANT_RIGHT", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, true, true),
      Entry("security.revoke_right", "SBLR_SECURITY_REVOKE_RIGHT", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, true, true),
      Entry("security.evaluate_visibility", "SBLR_SECURITY_EVALUATE_VISIBILITY", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, true, false),
      Entry("security.evaluate_policy", "SBLR_SECURITY_EVALUATE_POLICY", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, true, false),
      Entry("security.group.create", "SBLR_SEC_CREATE_GROUP", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, true, true),
      Entry("security.principal.create", "SBLR_SECURITY_PRINCIPAL_CREATE", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, true, true),
      Entry("security.principal.alter", "SBLR_SECURITY_PRINCIPAL_ALTER", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, true, true),
      Entry("security.membership.grant", "SBLR_SECURITY_MEMBERSHIP_GRANT", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, true, true),
      Entry("security.membership.revoke", "SBLR_SECURITY_MEMBERSHIP_REVOKE", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, true, true),
      Entry("security.privilege.grant", "SBLR_SECURITY_PRIVILEGE_GRANT", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, true, true),
      Entry("security.privilege.revoke", "SBLR_SECURITY_PRIVILEGE_REVOKE", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, true, true),
      Entry("security.session.set_role", "SBLR_SECURITY_SESSION_SET_ROLE", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, true, false),
      Entry("security.policy.create", "SBLR_SECURITY_POLICY_CREATE", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, true, true),
      Entry("security.policy.alter", "SBLR_SECURITY_POLICY_ALTER", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, true, true),
      Entry("security.policy.attach", "SBLR_SECURITY_POLICY_ATTACH", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, true, true),
      Entry("security.policy.activate", "SBLR_SECURITY_POLICY_ACTIVATE", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, true, true),
      Entry("security.policy.deactivate", "SBLR_SECURITY_POLICY_DEACTIVATE", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, true, true),
      Entry("security.policy.validate", "SBLR_SECURITY_POLICY_VALIDATE", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, true, false),
      Entry("security.policy.show", "SBLR_SECURITY_POLICY_SHOW", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, true, false),
      Entry("op.gpu.artifact_quarantine", "SBLR_OP_GPU_ARTIFACT_QUARANTINE", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented, true, false),
      Entry("op.gpu.cache_clear", "SBLR_OP_GPU_CACHE_CLEAR", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented, true, false),
      Entry("op.gpu.device_quarantine", "SBLR_OP_GPU_DEVICE_QUARANTINE", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented, true, false),
      Entry("op.gpu.kernel_quarantine", "SBLR_OP_GPU_KERNEL_QUARANTINE", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented, true, false),
      Entry("op.gpu.profile_disable", "SBLR_OP_GPU_PROFILE_DISABLE", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented, true, false),
      Entry("op.gpu.profile_enable", "SBLR_OP_GPU_PROFILE_ENABLE", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented, true, false),
      Entry("op.management.listener.drain", "SBLR_OP_MANAGEMENT_LISTENER_DRAIN", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("op.management.listener.undrain", "SBLR_OP_MANAGEMENT_LISTENER_UNDRAIN", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("op.management.manager.restart", "SBLR_OP_MANAGEMENT_MANAGER_RESTART", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("op.management.manager.start", "SBLR_OP_MANAGEMENT_MANAGER_START", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("op.management.manager.stop", "SBLR_OP_MANAGEMENT_MANAGER_STOP", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("op.management.parser_pool.resize", "SBLR_OP_MANAGEMENT_PARSER_POOL_RESIZE", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("op.native_compile.aot_rebuild", "SBLR_OP_NATIVE_COMPILE_AOT_REBUILD", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented, true, false),
      Entry("op.native_compile.artifact_quarantine", "SBLR_OP_NATIVE_COMPILE_ARTIFACT_QUARANTINE", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented, true, false),
      Entry("op.native_compile.cache_invalidate", "SBLR_OP_NATIVE_COMPILE_CACHE_INVALIDATE", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented, true, false),
      Entry("op.native_compile.profile_disable", "SBLR_OP_NATIVE_COMPILE_PROFILE_DISABLE", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented, true, false),
      Entry("op.native_compile.profile_enable", "SBLR_OP_NATIVE_COMPILE_PROFILE_ENABLE", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented, true, false),
      Entry("op.management.config.reload", "SBLR_OP_MANAGEMENT_CONFIG_RELOAD", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("op.management.instruction.ack", "SBLR_OP_MANAGEMENT_INSTRUCTION_ACK", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("op.management.instruction.apply", "SBLR_OP_MANAGEMENT_INSTRUCTION_APPLY", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("op.management.instruction.cancel", "SBLR_OP_MANAGEMENT_INSTRUCTION_CANCEL", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("op.management.instruction.quarantine", "SBLR_OP_MANAGEMENT_INSTRUCTION_QUARANTINE", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("op.management.support_bundle.create", "SBLR_OP_MANAGEMENT_SUPPORT_BUNDLE_CREATE", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("memory.profile.list", "SBLR_MEMORY_PROFILE_LIST", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("memory.profile.show", "SBLR_MEMORY_PROFILE_SHOW", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("memory.profile.set", "SBLR_MEMORY_PROFILE_SET", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("memory.policy.validate", "SBLR_MEMORY_POLICY_VALIDATE", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("memory.tree.show", "SBLR_MEMORY_TREE_SHOW", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("memory.pressure.show", "SBLR_MEMORY_PRESSURE_SHOW", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("memory.cache.show", "SBLR_MEMORY_CACHE_SHOW", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("memory.cache.flush", "SBLR_MEMORY_CACHE_FLUSH", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("memory.cache.invalidate", "SBLR_MEMORY_CACHE_INVALIDATE", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("memory.scavenge", "SBLR_MEMORY_SCAVENGE", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("memory.grants.show", "SBLR_MEMORY_GRANTS_SHOW", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("memory.grant_feedback.reset", "SBLR_MEMORY_GRANT_FEEDBACK_RESET", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("memory.streams.show", "SBLR_MEMORY_STREAMS_SHOW", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("memory.stream_policy.set", "SBLR_MEMORY_STREAM_POLICY_SET", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("memory.udr.show", "SBLR_MEMORY_UDR_SHOW", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("memory.udr_limit.set", "SBLR_MEMORY_UDR_LIMIT_SET", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("memory.dump_policy.set", "SBLR_MEMORY_DUMP_POLICY_SET", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("memory.platform.show", "SBLR_MEMORY_PLATFORM_SHOW", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("memory.incident.bundle", "SBLR_MEMORY_INCIDENT_BUNDLE", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("memory.report.create", "SBLR_MEMORY_REPORT_CREATE", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("memory.optimizer.show", "SBLR_MEMORY_OPTIMIZER_SHOW", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("memory.optimizer.set", "SBLR_MEMORY_OPTIMIZER_SET", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("memory.optimizer.run", "SBLR_MEMORY_OPTIMIZER_RUN", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("memory.object_residency.show", "SBLR_MEMORY_OBJECT_RESIDENCY_SHOW", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("memory.object_residency.set", "SBLR_MEMORY_OBJECT_RESIDENCY_SET", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("memory.rate_limit.show", "SBLR_MEMORY_RATE_LIMIT_SHOW", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("memory.rate_limit.set", "SBLR_MEMORY_RATE_LIMIT_SET", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("memory.policy_upgrade.plan", "SBLR_MEMORY_POLICY_UPGRADE_PLAN", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("memory.policy_migration.plan", "SBLR_MEMORY_POLICY_MIGRATION_PLAN", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("storage_tier.inspect", "SBLR_STORAGE_TIER_INSPECT", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("storage_tier.validate", "SBLR_STORAGE_TIER_VALIDATE", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("storage_tier.plan_migration", "SBLR_STORAGE_TIER_PLAN_MIGRATION", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("storage_tier.stage_migration", "SBLR_STORAGE_TIER_STAGE_MIGRATION", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("storage_tier.commit_migration", "SBLR_STORAGE_TIER_COMMIT_MIGRATION", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("storage_tier.rollback_migration", "SBLR_STORAGE_TIER_ROLLBACK_MIGRATION", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("filespace.discovery.scan", "SBLR_FILESPACE_DISCOVERY_SCAN", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("filespace.discovery.orphan_scan", "SBLR_FILESPACE_DISCOVERY_ORPHAN_SCAN", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("filespace.discovery.stale_scan", "SBLR_FILESPACE_DISCOVERY_STALE_SCAN", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("filespace.package.export_manifest", "SBLR_FILESPACE_PACKAGE_EXPORT_MANIFEST", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("filespace.package.inspect_manifest", "SBLR_FILESPACE_PACKAGE_INSPECT_MANIFEST", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("filespace.package.import_to_quarantine", "SBLR_FILESPACE_PACKAGE_IMPORT_TO_QUARANTINE", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("filespace.package.admit", "SBLR_FILESPACE_PACKAGE_ADMIT", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("filespace.package.reject", "SBLR_FILESPACE_PACKAGE_REJECT", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("shard_placement.create", "SBLR_SHARD_PLACEMENT_CREATE", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("shard_placement.verify", "SBLR_SHARD_PLACEMENT_VERIFY", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("shard_placement.move", "SBLR_SHARD_PLACEMENT_MOVE", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("shard_placement.split", "SBLR_SHARD_PLACEMENT_SPLIT", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("shard_placement.merge", "SBLR_SHARD_PLACEMENT_MERGE", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("shard_placement.rebalance", "SBLR_SHARD_PLACEMENT_REBALANCE", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("shard_placement.freeze", "SBLR_SHARD_PLACEMENT_FREEZE", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("shard_placement.archive", "SBLR_SHARD_PLACEMENT_ARCHIVE", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("shard_placement.reattach", "SBLR_SHARD_PLACEMENT_REATTACH", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("shard_placement.quarantine", "SBLR_SHARD_PLACEMENT_QUARANTINE", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("shard_placement.reconcile", "SBLR_SHARD_PLACEMENT_RECONCILE", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("shard_placement.drop", "SBLR_SHARD_PLACEMENT_DROP", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("security.encryption_key.admit", "SBLR_SECURITY_ENCRYPTION_KEY_ADMIT", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, true, false),
      Entry("security.encryption_key.rotate", "SBLR_SECURITY_ENCRYPTION_KEY_ROTATE", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, true, false),
      Entry("security.protected_material_cache.inspect", "SBLR_SECURITY_PROTECTED_MATERIAL_CACHE_INSPECT", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, true, false),
      Entry("security.protected_material_cache.purge", "SBLR_SECURITY_PROTECTED_MATERIAL_CACHE_PURGE", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, true, false),
      Entry("security.protected_material_cache.shutdown", "SBLR_SECURITY_PROTECTED_MATERIAL_CACHE_SHUTDOWN", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, true, false),
      Entry("security.encrypted_filespace.open", "SBLR_SECURITY_ENCRYPTED_FILESPACE_OPEN", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, true, false),
      Entry("security.request_protected_material", "SBLR_SECURITY_REQUEST_PROTECTED_MATERIAL", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, true, false),
      Entry("security.protected_material.create", "SBLR_SECURITY_PROTECTED_MATERIAL_CREATE", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, true, false),
      Entry("security.protected_material.version.add", "SBLR_SECURITY_PROTECTED_MATERIAL_VERSION_ADD", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, true, false),
      Entry("security.protected_material.resolve", "SBLR_SECURITY_PROTECTED_MATERIAL_RESOLVE", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, true, false),
      Entry("security.protected_material.release", "SBLR_SECURITY_PROTECTED_MATERIAL_RELEASE", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, true, false),
      Entry("security.protected_material.version.purge", "SBLR_SECURITY_PROTECTED_MATERIAL_VERSION_PURGE", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, true, false),
      Entry("security.protected_material.catalog.inspect", "SBLR_SECURITY_PROTECTED_MATERIAL_CATALOG_INSPECT", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, true, false),
      Entry("security.protected_material.package.export", "SBLR_SECURITY_PROTECTED_MATERIAL_PACKAGE_EXPORT", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, true, false),
      Entry("security.protected_material.package.import", "SBLR_SECURITY_PROTECTED_MATERIAL_PACKAGE_IMPORT", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, true, false),
      Entry("op.show.aot_artifacts", "SBLR_OP_SHOW_AOT_ARTIFACTS", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented, true, false),
      Entry("op.show.audit", "SBLR_OP_SHOW_AUDIT", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, true, false),
      Entry("op.show.buffer_pool", "SBLR_OP_SHOW_BUFFER_POOL", SblrOpcodeCategory::observability, SblrOpcodeSupport::implemented, true, false),
      Entry("op.show.cache", "SBLR_OP_SHOW_CACHE", SblrOpcodeCategory::observability, SblrOpcodeSupport::implemented, true, false),
      Entry("op.show.capabilities", "SBLR_OP_SHOW_CAPABILITIES", SblrOpcodeCategory::observability, SblrOpcodeSupport::implemented, true, false),
      Entry("op.show.context", "SBLR_OP_SHOW_CONTEXT", SblrOpcodeCategory::observability, SblrOpcodeSupport::implemented, true, false),
      Entry("op.show.dialect", "SBLR_OP_SHOW_DIALECT", SblrOpcodeCategory::observability, SblrOpcodeSupport::implemented, true, false),
      Entry("op.show.discovery_rights", "SBLR_OP_SHOW_DISCOVERY_RIGHTS", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, true, false),
      Entry("op.show.gpu", "SBLR_OP_SHOW_GPU", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented, true, false),
      Entry("op.show.gpu_artifacts", "SBLR_OP_SHOW_GPU_ARTIFACTS", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented, true, false),
      Entry("op.show.gpu_capability", "SBLR_OP_SHOW_GPU_CAPABILITY", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented, true, false),
      Entry("op.show.gpu_devices", "SBLR_OP_SHOW_GPU_DEVICES", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented, true, false),
      Entry("op.show.gpu_kernels", "SBLR_OP_SHOW_GPU_KERNELS", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented, true, false),
      Entry("op.show.gpu_memory", "SBLR_OP_SHOW_GPU_MEMORY", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented, true, false),
      Entry("op.show.grants", "SBLR_OP_SHOW_GRANTS", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, true, false),
      Entry("op.show.groups", "SBLR_OP_SHOW_GROUPS", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, true, false),
      Entry("op.show.identity_providers", "SBLR_OP_SHOW_IDENTITY_PROVIDERS", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, true, false),
      Entry("op.show.index_health", "SBLR_OP_SHOW_INDEX_HEALTH", SblrOpcodeCategory::observability, SblrOpcodeSupport::implemented, true, false),
      Entry("op.show.io", "SBLR_OP_SHOW_IO", SblrOpcodeCategory::observability, SblrOpcodeSupport::implemented, true, false),
      Entry("op.show.job", "SBLR_OP_SHOW_JOB", SblrOpcodeCategory::observability, SblrOpcodeSupport::implemented, true, false),
      Entry("op.show.job_dependencies", "SBLR_OP_SHOW_JOB_DEPENDENCIES", SblrOpcodeCategory::observability, SblrOpcodeSupport::implemented, true, false),
      Entry("op.show.job_runs", "SBLR_OP_SHOW_JOB_RUNS", SblrOpcodeCategory::observability, SblrOpcodeSupport::implemented, true, false),
      Entry("op.show.llvm", "SBLR_OP_SHOW_LLVM", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented, true, false),
      Entry("op.show.llvm_provenance", "SBLR_OP_SHOW_LLVM_PROVENANCE", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented, true, false),
      Entry("op.show.llvm_targets", "SBLR_OP_SHOW_LLVM_TARGETS", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented, true, false),
      Entry("op.show.management.config", "SBLR_OP_SHOW_MANAGEMENT_CONFIG", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("op.show.management.drift", "SBLR_OP_SHOW_MANAGEMENT_DRIFT", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("op.show.jobs", "SBLR_OP_SHOW_JOBS", SblrOpcodeCategory::observability, SblrOpcodeSupport::implemented, true, false),
      Entry("op.show.locks", "SBLR_OP_SHOW_LOCKS", SblrOpcodeCategory::observability, SblrOpcodeSupport::implemented, true, false),
      Entry("op.show.management.instructions", "SBLR_OP_SHOW_MANAGEMENT_INSTRUCTIONS", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("op.show.management.listeners", "SBLR_OP_SHOW_MANAGEMENT_LISTENERS", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("op.show.management.manager", "SBLR_OP_SHOW_MANAGEMENT_MANAGER", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("op.show.management.parser_pool", "SBLR_OP_SHOW_MANAGEMENT_PARSER_POOL", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("op.show.management.readiness", "SBLR_OP_SHOW_MANAGEMENT_READINESS", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("op.show.management.servers", "SBLR_OP_SHOW_MANAGEMENT_SERVERS", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("op.show.management.support_bundle_safety", "SBLR_OP_SHOW_MANAGEMENT_SUPPORT_BUNDLE_SAFETY", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("op.show.management.support_bundles", "SBLR_OP_SHOW_MANAGEMENT_SUPPORT_BUNDLES", SblrOpcodeCategory::management, SblrOpcodeSupport::implemented, true, false),
      Entry("op.show.masks", "SBLR_OP_SHOW_MASKS", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, true, false),
      Entry("op.show.metrics", "SBLR_OP_SHOW_METRICS", SblrOpcodeCategory::observability, SblrOpcodeSupport::implemented, true, false),
      Entry("op.show.metrics_family", "SBLR_OP_SHOW_METRICS_FAMILY", SblrOpcodeCategory::observability, SblrOpcodeSupport::implemented, true, false),
      Entry("op.show.native_compile", "SBLR_OP_SHOW_NATIVE_COMPILE", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented, true, false),
      Entry("op.show.native_compile_cache", "SBLR_OP_SHOW_NATIVE_COMPILE_CACHE", SblrOpcodeCategory::extensibility, SblrOpcodeSupport::implemented, true, false),
      Entry("op.show.object_visibility", "SBLR_OP_SHOW_OBJECT_VISIBILITY", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, true, false),
      Entry("op.show.performance", "SBLR_OP_SHOW_PERFORMANCE", SblrOpcodeCategory::observability, SblrOpcodeSupport::implemented, true, false),
      Entry("op.show.policies", "SBLR_OP_SHOW_POLICIES", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, true, false),
      Entry("op.show.query_store", "SBLR_OP_SHOW_QUERY_STORE", SblrOpcodeCategory::observability, SblrOpcodeSupport::implemented, true, false),
      Entry("op.show.rls", "SBLR_OP_SHOW_RLS", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, true, false),
      Entry("op.show.roles", "SBLR_OP_SHOW_ROLES", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, true, false),
      Entry("op.show.schema_path", "SBLR_OP_SHOW_SCHEMA_PATH", SblrOpcodeCategory::observability, SblrOpcodeSupport::implemented, true, false),
      Entry("op.show.search_path", "SBLR_OP_SHOW_SEARCH_PATH", SblrOpcodeCategory::observability, SblrOpcodeSupport::implemented, true, false),
      Entry("op.show.security_events", "SBLR_OP_SHOW_SECURITY_EVENTS", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, true, false),
      Entry("op.show.security_profiles", "SBLR_OP_SHOW_SECURITY_PROFILES", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, true, false),
      Entry("op.show.sessions", "SBLR_OP_SHOW_SESSIONS", SblrOpcodeCategory::observability, SblrOpcodeSupport::implemented, true, false),
      Entry("op.show.statement_cache", "SBLR_OP_SHOW_STATEMENT_CACHE", SblrOpcodeCategory::observability, SblrOpcodeSupport::implemented, true, false),
      Entry("op.show.statements", "SBLR_OP_SHOW_STATEMENTS", SblrOpcodeCategory::observability, SblrOpcodeSupport::implemented, true, false),
      Entry("op.show.system", "SBLR_OP_SHOW_SYSTEM", SblrOpcodeCategory::observability, SblrOpcodeSupport::implemented, true, false),
      Entry("op.show.transaction", "SBLR_OP_SHOW_TRANSACTION", SblrOpcodeCategory::observability, SblrOpcodeSupport::implemented, true, false),
      Entry("op.show.transaction_isolation", "SBLR_OP_SHOW_TRANSACTION_ISOLATION", SblrOpcodeCategory::observability, SblrOpcodeSupport::implemented, true, false),
      Entry("op.show.transactions", "SBLR_OP_SHOW_TRANSACTIONS", SblrOpcodeCategory::observability, SblrOpcodeSupport::implemented, true, false),
      Entry("op.show.users", "SBLR_OP_SHOW_USERS", SblrOpcodeCategory::security, SblrOpcodeSupport::implemented, true, false),
      Entry("op.show.version", "SBLR_OP_SHOW_VERSION", SblrOpcodeCategory::observability, SblrOpcodeSupport::implemented, true, false),
      Entry("op.show.wait_events", "SBLR_OP_SHOW_WAIT_EVENTS", SblrOpcodeCategory::observability, SblrOpcodeSupport::implemented, true, false),
      Entry("nosql.document_insert", "SBLR_NOSQL_DOCUMENT_INSERT", SblrOpcodeCategory::nosql, SblrOpcodeSupport::implemented, true, true),
      Entry("nosql.document_find", "SBLR_NOSQL_DOCUMENT_FIND", SblrOpcodeCategory::nosql, SblrOpcodeSupport::implemented, true, true),
      Entry("nosql.document_update", "SBLR_NOSQL_DOCUMENT_UPDATE", SblrOpcodeCategory::nosql, SblrOpcodeSupport::implemented, true, true),
      Entry("nosql.document_delete", "SBLR_NOSQL_DOCUMENT_DELETE", SblrOpcodeCategory::nosql, SblrOpcodeSupport::implemented, true, true),
      Entry("nosql.graph_query", "SBLR_NOSQL_GRAPH_QUERY", SblrOpcodeCategory::nosql, SblrOpcodeSupport::implemented, true, true),
      Entry("nosql.key_value_get", "SBLR_NOSQL_KEY_VALUE_GET", SblrOpcodeCategory::nosql, SblrOpcodeSupport::implemented, true, true),
      Entry("nosql.key_value_put", "SBLR_NOSQL_KEY_VALUE_PUT", SblrOpcodeCategory::nosql, SblrOpcodeSupport::implemented, true, true),
      Entry("nosql.key_value_multiget", "SBLR_NOSQL_KEY_VALUE_MULTIGET", SblrOpcodeCategory::nosql, SblrOpcodeSupport::implemented, true, true),
      Entry("nosql.key_value_pipeline", "SBLR_NOSQL_KEY_VALUE_PIPELINE", SblrOpcodeCategory::nosql, SblrOpcodeSupport::implemented, true, true),
      Entry("nosql.key_value_atomic_program", "SBLR_NOSQL_KEY_VALUE_ATOMIC_PROGRAM", SblrOpcodeCategory::nosql, SblrOpcodeSupport::implemented, true, true),
      Entry("nosql.backpressure_debt_plan", "SBLR_NOSQL_BACKPRESSURE_DEBT_PLAN", SblrOpcodeCategory::nosql, SblrOpcodeSupport::implemented, true, true),
      Entry("nosql.family_maintenance_plan", "SBLR_NOSQL_FAMILY_MAINTENANCE_PLAN", SblrOpcodeCategory::nosql, SblrOpcodeSupport::implemented, true, true),
      Entry("nosql.statistics_advisor_plan", "SBLR_NOSQL_STATISTICS_ADVISOR_PLAN", SblrOpcodeCategory::nosql, SblrOpcodeSupport::implemented, true, true),
      Entry("nosql.time_series_append", "SBLR_NOSQL_TIME_SERIES_APPEND", SblrOpcodeCategory::nosql, SblrOpcodeSupport::implemented, true, true),
      Entry("nosql.search_query", "SBLR_NOSQL_SEARCH_QUERY", SblrOpcodeCategory::nosql, SblrOpcodeSupport::implemented, true, true),
      Entry("nosql.vector_search", "SBLR_NOSQL_VECTOR_SEARCH", SblrOpcodeCategory::nosql, SblrOpcodeSupport::implemented, true, true, false),
      Entry("nosql.vector_collection_op", "SBLR_NOSQL_VECTOR_COLLECTION_OP", SblrOpcodeCategory::nosql, SblrOpcodeSupport::implemented, true, true, false),
  };
  return registry;
}

const SblrOpcodeEntry* LookupSblrOperation(std::string_view operation_id) {
  for (const auto& entry : StaticSblrOpcodeRegistry()) {
    if (entry.operation_id == operation_id) return &entry;
  }
  return nullptr;
}

const SblrOpcodeEntry* LookupSblrOpcode(std::string_view opcode) {
  for (const auto& entry : StaticSblrOpcodeRegistry()) {
    if (entry.opcode == opcode) return &entry;
  }
  return nullptr;
}

const SblrOpcodeEntry* LookupSblrOpcodeCode(std::uint16_t code) {
  if (code == 0) return nullptr;
  const SblrOpcodeEntry* match = nullptr;
  for (const auto& entry : StaticSblrOpcodeRegistry()) {
    if (entry.code != code) continue;
    if (match != nullptr) return nullptr;
    match = &entry;
  }
  return match;
}

SblrOpcodeValidationResult ValidateSblrOpcodeIdentity(std::uint16_t code,
                                                      std::string_view operation_id,
                                                      std::string_view opcode) {
  SblrOpcodeValidationResult result;
  if (code == 1800 && operation_id == "engine.op.sec_alter_role" && opcode == "SBLR_SEC_ALTER_ROLE") { result.entry = LookupSblrOpcode("SBLR_SEC_ALTER_ROLE"); result.ok = result.entry != nullptr; return result; }
  if (code == 1792 && operation_id == "engine.op.security_create_user" && opcode == "SBLR_SEC_CREATE_USER") { result.entry = LookupSblrOpcode("SBLR_SEC_CREATE_USER"); result.ok = result.entry != nullptr; return result; }
  if (code == 1793 && operation_id == "engine.op.sec_alter_user" && opcode == "SBLR_SEC_ALTER_USER") { result.entry = LookupSblrOpcode("SBLR_SEC_ALTER_USER"); result.ok = result.entry != nullptr; return result; }
  if (code == 1797 && operation_id == "engine.op.sec_create_group_mapping" && opcode == "SBLR_SEC_CREATE_GROUP_MAPPING") { result.entry = LookupSblrOpcode("SBLR_SEC_CREATE_GROUP_MAPPING"); result.ok = result.entry != nullptr; return result; }
  if (code == 1576 && operation_id == "engine.op.ddl_create_foreign_table" && opcode == "SBLR_DDL_CREATE_FOREIGN_TABLE") { result.entry = LookupSblrOpcode("SBLR_DDL_CREATE_FOREIGN_TABLE"); result.ok = result.entry != nullptr; return result; }
  if (code == 1578 && operation_id == "engine.op.ddl_create_fdw" && opcode == "SBLR_DDL_CREATE_FDW") { result.entry = LookupSblrOpcode("SBLR_DDL_CREATE_FDW"); result.ok = result.entry != nullptr; return result; }
  if (code == 1579 && operation_id == "engine.op.ddl_drop_fdw" && opcode == "SBLR_DDL_DROP_FDW") { result.entry = LookupSblrOpcode("SBLR_DDL_DROP_FDW"); result.ok = result.entry != nullptr; return result; }
  if (code == 1577 && operation_id == "engine.op.ddl_drop_foreign_table" && opcode == "SBLR_DDL_DROP_FOREIGN_TABLE") { result.entry = LookupSblrOpcode("SBLR_DDL_DROP_FOREIGN_TABLE"); result.ok = result.entry != nullptr; return result; }
  if (code == 1574 && operation_id == "engine.op.ddl_create_synonym" && opcode == "SBLR_DDL_CREATE_SYNONYM") {
    result.entry = LookupSblrOpcode("SBLR_DDL_CREATE_SYNONYM");
    result.ok = result.entry != nullptr;
    return result;
  }
  if (code == 3369 && operation_id == "observability.explain_operation" && opcode == "SBLR_OBSERVABILITY_EXPLAIN_OPERATION") {
    result.entry = LookupSblrOpcode("SBLR_OBSERVABILITY_EXPLAIN_OPERATION");
    result.ok = result.entry != nullptr;
    return result;
  }
  if (code == 1625 && operation_id == "engine.op.ddl_create_aggregate" && opcode == "SBLR_DDL_CREATE_AGGREGATE") {
    result.entry = LookupSblrOpcode("SBLR_DDL_CREATE_AGGREGATE");
    result.ok = result.entry != nullptr;
    return result;
  }
  if (code == 1626 && operation_id == "engine.op.ddl_alter_aggregate" && opcode == "SBLR_DDL_ALTER_AGGREGATE") {
    result.entry = LookupSblrOpcode("SBLR_DDL_ALTER_AGGREGATE");
    result.ok = result.entry != nullptr;
    return result;
  }
  if (code == 1627 && operation_id == "engine.op.ddl_drop_aggregate" && opcode == "SBLR_DDL_DROP_AGGREGATE") {
    result.entry = LookupSblrOpcode("SBLR_DDL_DROP_AGGREGATE");
    result.ok = result.entry != nullptr;
    return result;
  }
  if (code == 1628 && operation_id == "engine.op.ddl_purge_system_history" && opcode == "SBLR_DDL_PURGE_SYSTEM_HISTORY") {
    result.entry = LookupSblrOpcode("SBLR_DDL_PURGE_SYSTEM_HISTORY");
    result.ok = result.entry != nullptr;
    return result;
  }
  if (code == 1629 && operation_id == "engine.op.ddl_set_index_optimizer_eligibility" && opcode == "SBLR_DDL_SET_INDEX_OPTIMIZER_ELIGIBILITY") { result.entry = LookupSblrOpcode("SBLR_DDL_SET_INDEX_OPTIMIZER_ELIGIBILITY"); result.ok = result.entry != nullptr; return result; }
  if (code == 1630 && operation_id == "engine.op.ddl_set_table_type_enforcement" && opcode == "SBLR_DDL_SET_TABLE_TYPE_ENFORCEMENT") { result.entry = LookupSblrOpcode("SBLR_DDL_SET_TABLE_TYPE_ENFORCEMENT"); result.ok = result.entry != nullptr; return result; }
  if (code == 1631 && operation_id == "engine.op.database_serialize_logical_snapshot" && opcode == "SBLR_DATABASE_SERIALIZE_LOGICAL_SNAPSHOT") { result.entry = LookupSblrOpcode("SBLR_DATABASE_SERIALIZE_LOGICAL_SNAPSHOT"); result.ok = result.entry != nullptr; return result; }
  if (code == 1632 && operation_id == "engine.op.database_deserialize_logical_snapshot" && opcode == "SBLR_DATABASE_DESERIALIZE_LOGICAL_SNAPSHOT") { result.entry = LookupSblrOpcode("SBLR_DATABASE_DESERIALIZE_LOGICAL_SNAPSHOT"); result.ok = result.entry != nullptr; return result; }
  if (code == 1633 && operation_id == "engine.op.ddl_create_macro" && opcode == "SBLR_DDL_CREATE_MACRO") { result.entry = LookupSblrOpcode("SBLR_DDL_CREATE_MACRO"); result.ok = result.entry != nullptr; return result; }
  if (code == 1634 && operation_id == "engine.op.ddl_drop_macro" && opcode == "SBLR_DDL_DROP_MACRO") { result.entry = LookupSblrOpcode("SBLR_DDL_DROP_MACRO"); result.ok = result.entry != nullptr; return result; }
  if (code == 1635 && operation_id == "engine.op.admin_register_external_relation_resolver" && opcode == "SBLR_ADMIN_REGISTER_EXTERNAL_RELATION_RESOLVER") { result.entry = LookupSblrOpcode("SBLR_ADMIN_REGISTER_EXTERNAL_RELATION_RESOLVER"); result.ok = result.entry != nullptr; return result; }
  if (code == 1636 && operation_id == "engine.op.admin_unregister_external_relation_resolver" && opcode == "SBLR_ADMIN_UNREGISTER_EXTERNAL_RELATION_RESOLVER") { result.entry = LookupSblrOpcode("SBLR_ADMIN_UNREGISTER_EXTERNAL_RELATION_RESOLVER"); result.ok = result.entry != nullptr; return result; }
  if (code == 1637 && operation_id == "engine.op.ddl_create_dictionary" && opcode == "SBLR_DDL_CREATE_DICTIONARY") { result.entry = LookupSblrOpcode("SBLR_DDL_CREATE_DICTIONARY"); result.ok = result.entry != nullptr; return result; }
  if (code == 1638 && operation_id == "engine.op.ddl_drop_dictionary" && opcode == "SBLR_DDL_DROP_DICTIONARY") { result.entry = LookupSblrOpcode("SBLR_DDL_DROP_DICTIONARY"); result.ok = result.entry != nullptr; return result; }
  if (code == 1639 && operation_id == "engine.op.ddl_alter_dictionary" && opcode == "SBLR_DDL_ALTER_DICTIONARY") { result.entry = LookupSblrOpcode("SBLR_DDL_ALTER_DICTIONARY"); result.ok = result.entry != nullptr; return result; }
  if (code == 1640 && operation_id == "engine.op.ddl_create_continuous_view" && opcode == "SBLR_DDL_CREATE_CONTINUOUS_VIEW") { result.entry = LookupSblrOpcode("SBLR_DDL_CREATE_CONTINUOUS_VIEW"); result.ok = result.entry != nullptr; return result; }
  if (code == 1641 && operation_id == "engine.op.ddl_alter_continuous_view" && opcode == "SBLR_DDL_ALTER_CONTINUOUS_VIEW") { result.entry = LookupSblrOpcode("SBLR_DDL_ALTER_CONTINUOUS_VIEW"); result.ok = result.entry != nullptr; return result; }
  if (code == 1642 && operation_id == "engine.op.ddl_drop_continuous_view" && opcode == "SBLR_DDL_DROP_CONTINUOUS_VIEW") { result.entry = LookupSblrOpcode("SBLR_DDL_DROP_CONTINUOUS_VIEW"); result.ok = result.entry != nullptr; return result; }
  if (code == 1643 && operation_id == "engine.op.dml_async_insert_submit" && opcode == "SBLR_DML_ASYNC_INSERT_SUBMIT") { result.entry = LookupSblrOpcode("SBLR_DML_ASYNC_INSERT_SUBMIT"); result.ok = result.entry != nullptr; return result; }
  if (code == 1644 && operation_id == "engine.op.dml_async_insert_status" && opcode == "SBLR_DML_ASYNC_INSERT_STATUS") { result.entry = LookupSblrOpcode("SBLR_DML_ASYNC_INSERT_STATUS"); result.ok = result.entry != nullptr; return result; }
  if (code == 1645 && operation_id == "engine.op.dml_async_insert_cancel" && opcode == "SBLR_DML_ASYNC_INSERT_CANCEL") { result.entry = LookupSblrOpcode("SBLR_DML_ASYNC_INSERT_CANCEL"); result.ok = result.entry != nullptr; return result; }
  if (code == 1646 && operation_id == "engine.op.dml_conditional_mutate" && opcode == "SBLR_DML_CONDITIONAL_MUTATE") { result.entry = LookupSblrOpcode("SBLR_DML_CONDITIONAL_MUTATE"); result.ok = result.entry != nullptr; return result; }
  if (code == 1647 && operation_id == "engine.op.dml_counter_add" && opcode == "SBLR_DML_COUNTER_ADD") { result.entry = LookupSblrOpcode("SBLR_DML_COUNTER_ADD"); result.ok = result.entry != nullptr; return result; }
  if (code == 1648 && operation_id == "engine.op.dml_timeseries_schema_write" && opcode == "SBLR_DML_TIMESERIES_SCHEMA_WRITE") { result.entry = LookupSblrOpcode("SBLR_DML_TIMESERIES_SCHEMA_WRITE"); result.ok = result.entry != nullptr; return result; }
  if (code == 1649 && operation_id == "engine.op.ddl_set_timeseries_series_cardinality_policy" && opcode == "SBLR_DDL_SET_TIMESERIES_SERIES_CARDINALITY_POLICY") { result.entry = LookupSblrOpcode("SBLR_DDL_SET_TIMESERIES_SERIES_CARDINALITY_POLICY"); result.ok = result.entry != nullptr; return result; }
  if (code == 1650 && operation_id == "engine.op.ddl_create_timeseries_value_cache" && opcode == "SBLR_DDL_CREATE_TIMESERIES_VALUE_CACHE") { result.entry = LookupSblrOpcode("SBLR_DDL_CREATE_TIMESERIES_VALUE_CACHE"); result.ok = result.entry != nullptr; return result; }
  if (code == 1567 && operation_id == "engine.op.ddl_refresh_materialized_view" && opcode == "SBLR_DDL_REFRESH_MATERIALIZED_VIEW") { result.entry = LookupSblrOpcode("SBLR_DDL_REFRESH_MATERIALIZED_VIEW"); result.ok = result.entry != nullptr; return result; }
  if (code == 1641 && operation_id == "engine.op.ddl_alter_continuous_view" && opcode == "SBLR_DDL_ALTER_CONTINUOUS_VIEW") { result.entry = LookupSblrOpcode("SBLR_DDL_ALTER_CONTINUOUS_VIEW"); result.ok = result.entry != nullptr; return result; }
  if (code == 1029 && operation_id == "engine.op.udr_invoke" &&
      opcode == "SBLR_UDR_INVOKE") {
    result.entry = LookupSblrOpcode("SBLR_UDR_INVOKE");
    result.ok = result.entry != nullptr;
    if (!result.ok) {
      result.diagnostic_id = "SBLR.OPERATION.OPCODE_IDENTITY_MISMATCH";
      result.detail = "udr_invoke_registry_entry_missing";
    }
    return result;
  }
  if (code == 8193 && operation_id == "engine.op.kv_structured_mutate" && opcode == "SBLR_KV_STRUCTURED_MUTATE") {
    result.entry = LookupSblrOpcode("SBLR_KV_STRUCTURED_MUTATE"); result.ok = result.entry != nullptr;
    return result;
  }
  if (code == 8194 && operation_id == "engine.op.kv_structured_scan" && opcode == "SBLR_KV_STRUCTURED_SCAN") {
    result.entry = LookupSblrOpcode("SBLR_KV_STRUCTURED_SCAN"); result.ok = result.entry != nullptr;
    return result;
  }
  if (code == 8195 && operation_id == "engine.op.kv_structured_stream_read" && opcode == "SBLR_KV_STRUCTURED_STREAM_READ") {
    result.entry = LookupSblrOpcode("SBLR_KV_STRUCTURED_STREAM_READ"); result.ok = result.entry != nullptr;
    return result;
  }
  if (code == 8196 && operation_id == "engine.op.kv_structured_stream_append" && opcode == "SBLR_KV_STRUCTURED_STREAM_APPEND") {
    result.entry = LookupSblrOpcode("SBLR_KV_STRUCTURED_STREAM_APPEND"); result.ok = result.entry != nullptr;
    return result;
  }
  if (code == 8197 && operation_id == "engine.op.kv_structured_timeseries" && opcode == "SBLR_KV_STRUCTURED_TIMESERIES") {
    result.entry = LookupSblrOpcode("SBLR_KV_STRUCTURED_TIMESERIES"); result.ok = result.entry != nullptr;
    return result;
  }
  if (code == 5125 && operation_id == "engine.op.system_config_set" && opcode == "SBLR_SYSTEM_CONFIG_SET") {
    result.entry = LookupSblrOpcode("SBLR_SYSTEM_CONFIG_SET"); result.ok = result.entry != nullptr;
    return result;
  }
  if (code == 1536 && operation_id == "engine.op.ddl_create_schema" && opcode == "SBLR_DDL_CREATE_SCHEMA") {
    result.entry = LookupSblrOpcode("SBLR_DDL_CREATE_SCHEMA"); result.ok = result.entry != nullptr;
    return result;
  }
  if (code == 1537 && operation_id == "engine.op.ddl_create_table" && opcode == "SBLR_DDL_CREATE_TABLE") {
    result.entry = LookupSblrOpcode("SBLR_DDL_CREATE_TABLE"); result.ok = result.entry != nullptr;
    return result;
  }
  if (code == 1539 && operation_id == "engine.op.ddl_drop_table" && opcode == "SBLR_DDL_DROP_TABLE") {
    result.entry = LookupSblrOpcode("SBLR_DDL_DROP_TABLE"); result.ok = result.entry != nullptr;
    return result;
  }
  if (code == 1540 && operation_id == "engine.op.ddl_create_index" && opcode == "SBLR_DDL_CREATE_INDEX") {
    result.entry = LookupSblrOpcode("SBLR_DDL_CREATE_INDEX"); result.ok = result.entry != nullptr;
    return result;
  }
  if (code == 1541 && operation_id == "engine.op.ddl_drop_index" && opcode == "SBLR_DDL_DROP_INDEX") {
    result.entry = LookupSblrOpcode("SBLR_DDL_DROP_INDEX"); result.ok = result.entry != nullptr;
    return result;
  }
  if (code == 1551 && operation_id == "engine.op.ddl_create_trigger" && opcode == "SBLR_DDL_CREATE_TRIGGER") {
    result.entry = LookupSblrOpcode("SBLR_DDL_CREATE_TRIGGER"); result.ok = result.entry != nullptr;
    return result;
  }
  if (code == 1552 && operation_id == "engine.op.ddl_alter_trigger" && opcode == "SBLR_DDL_ALTER_TRIGGER") {
    result.entry = LookupSblrOpcode("SBLR_DDL_ALTER_TRIGGER"); result.ok = result.entry != nullptr;
    return result;
  }
  if (code == 1553 && operation_id == "engine.op.ddl_drop_trigger" && opcode == "SBLR_DDL_DROP_TRIGGER") {
    result.entry = LookupSblrOpcode("SBLR_DDL_DROP_TRIGGER"); result.ok = result.entry != nullptr;
    return result;
  }
  if (code == 1554 && operation_id == "engine.op.ddl_create_procedure" && opcode == "SBLR_DDL_CREATE_PROCEDURE") {
    result.entry = LookupSblrOpcode("SBLR_DDL_CREATE_PROCEDURE"); result.ok = result.entry != nullptr;
    return result;
  }
  if (code == 1555 && operation_id == "engine.op.ddl_alter_procedure" && opcode == "SBLR_DDL_ALTER_PROCEDURE") {
    result.entry = LookupSblrOpcode("SBLR_DDL_ALTER_PROCEDURE"); result.ok = result.entry != nullptr;
    return result;
  }
  if (code == 1556 && operation_id == "engine.op.ddl_drop_procedure" && opcode == "SBLR_DDL_DROP_PROCEDURE") {
    result.entry = LookupSblrOpcode("SBLR_DDL_DROP_PROCEDURE"); result.ok = result.entry != nullptr;
    return result;
  }
  if (code == 1557 && operation_id == "engine.op.ddl_create_function" && opcode == "SBLR_DDL_CREATE_FUNCTION") { result.entry = LookupSblrOpcode("SBLR_DDL_CREATE_FUNCTION"); result.ok = result.entry != nullptr; return result; }
  if (code == 1558 && operation_id == "engine.op.ddl_alter_function" && opcode == "SBLR_DDL_ALTER_FUNCTION") { result.entry = LookupSblrOpcode("SBLR_DDL_ALTER_FUNCTION"); result.ok = result.entry != nullptr; return result; }
  if (code == 1559 && operation_id == "engine.op.ddl_drop_function" && opcode == "SBLR_DDL_DROP_FUNCTION") { result.entry = LookupSblrOpcode("SBLR_DDL_DROP_FUNCTION"); result.ok = result.entry != nullptr; return result; }
  if (code == 1560 && operation_id == "engine.op.ddl_create_package" && opcode == "SBLR_DDL_CREATE_PACKAGE") { result.entry = LookupSblrOpcode("SBLR_DDL_CREATE_PACKAGE"); result.ok = result.entry != nullptr; }
  if (code == 1562 && operation_id == "engine.op.ddl_drop_package" && opcode == "SBLR_DDL_DROP_PACKAGE") { result.entry = LookupSblrOpcode("SBLR_DDL_DROP_PACKAGE"); result.ok = result.entry != nullptr; }
  if (code == 1575 && operation_id == "engine.op.ddl_drop_synonym" && opcode == "SBLR_DDL_DROP_SYNONYM") { result.entry = LookupSblrOpcode("SBLR_DDL_DROP_SYNONYM"); result.ok = result.entry != nullptr; }
  if (code == 1564 && operation_id == "engine.op.ddl_alter_sequence" && opcode == "SBLR_DDL_ALTER_SEQUENCE") { result.entry = LookupSblrOpcode("SBLR_DDL_ALTER_SEQUENCE"); result.ok = result.entry != nullptr; }
  if (code == 1565 && operation_id == "engine.op.ddl_drop_sequence" && opcode == "SBLR_DDL_DROP_SEQUENCE") { result.entry = LookupSblrOpcode("SBLR_DDL_DROP_SEQUENCE"); result.ok = result.entry != nullptr; }
  if (code == 1566 && operation_id == "engine.op.ddl_create_materialized_view" && opcode == "SBLR_DDL_CREATE_MATERIALIZED_VIEW") { result.entry = LookupSblrOpcode("SBLR_DDL_CREATE_MATERIALIZED_VIEW"); result.ok = result.entry != nullptr; }
  if (code == 1568 && operation_id == "engine.op.ddl_drop_materialized_view" && opcode == "SBLR_DDL_DROP_MATERIALIZED_VIEW") { result.entry = LookupSblrOpcode("SBLR_DDL_DROP_MATERIALIZED_VIEW"); result.ok = result.entry != nullptr; }
  if (code == 1570 && operation_id == "engine.op.ddl_alter_type" && opcode == "SBLR_DDL_ALTER_TYPE") { result.entry = LookupSblrOpcode("SBLR_DDL_ALTER_TYPE"); result.ok = result.entry != nullptr; }
  if (code == 1571 && operation_id == "engine.op.ddl_drop_type" && opcode == "SBLR_DDL_DROP_TYPE") { result.entry = LookupSblrOpcode("SBLR_DDL_DROP_TYPE"); result.ok = result.entry != nullptr; }
  if (code == 1569 && operation_id == "engine.op.ddl_create_type" && opcode == "SBLR_DDL_CREATE_TYPE") { result.entry = LookupSblrOpcode("SBLR_DDL_CREATE_TYPE"); result.ok = result.entry != nullptr; }
  if (code == 1561 && operation_id == "engine.op.ddl_alter_package" && opcode == "SBLR_DDL_ALTER_PACKAGE") { result.entry = LookupSblrOpcode("SBLR_DDL_ALTER_PACKAGE"); result.ok = result.entry != nullptr; return result; }
  if (code == 1561 && operation_id == "engine.op.ddl_create_temporary_table" && opcode == "SBLR_DDL_CREATE_TEMPORARY_TABLE") { result.entry = LookupSblrOpcode("SBLR_DDL_CREATE_TEMPORARY_TABLE"); result.ok = result.entry != nullptr; }
  if (code == 1562 && operation_id == "engine.op.ddl_drop_temporary_table" && opcode == "SBLR_DDL_DROP_TEMPORARY_TABLE") { result.entry = LookupSblrOpcode("SBLR_DDL_DROP_TEMPORARY_TABLE"); result.ok = result.entry != nullptr; }
  if (code == 1563 && operation_id == "engine.op.ddl_rename_object_vector" && opcode == "SBLR_DDL_RENAME_OBJECT_VECTOR") { result.entry = LookupSblrOpcode("SBLR_DDL_RENAME_OBJECT_VECTOR"); result.ok = result.entry != nullptr; }
  if (code == 1615 && operation_id == "engine.op.ddl_create_or_replace_srs" && opcode == "SBLR_DDL_CREATE_OR_REPLACE_SRS") { result.entry = LookupSblrOpcode("SBLR_DDL_CREATE_OR_REPLACE_SRS"); result.ok = result.entry != nullptr; }
  if (code == 1616 && operation_id == "engine.op.ddl_drop_srs" && opcode == "SBLR_DDL_DROP_SRS") { result.entry = LookupSblrOpcode("SBLR_DDL_DROP_SRS"); result.ok = result.entry != nullptr; return result; }
  if (code == 1617 && operation_id == "engine.op.ddl_create_rewrite_rule" && opcode == "SBLR_DDL_CREATE_REWRITE_RULE") { result.entry = LookupSblrOpcode("SBLR_DDL_CREATE_REWRITE_RULE"); result.ok = result.entry != nullptr; return result; }
  if (code == 1618 && operation_id == "engine.op.ddl_alter_rewrite_rule" && opcode == "SBLR_DDL_ALTER_REWRITE_RULE") { result.entry = LookupSblrOpcode("SBLR_DDL_ALTER_REWRITE_RULE"); result.ok = result.entry != nullptr; return result; }
  if (code == 1619 && operation_id == "engine.op.ddl_drop_rewrite_rule" && opcode == "SBLR_DDL_DROP_REWRITE_RULE") { result.entry = LookupSblrOpcode("SBLR_DDL_DROP_REWRITE_RULE"); result.ok = result.entry != nullptr; return result; }
  if (code == 1620 && operation_id == "engine.op.ddl_validate_constraint" && opcode == "SBLR_DDL_VALIDATE_CONSTRAINT") { result.entry = LookupSblrOpcode("SBLR_DDL_VALIDATE_CONSTRAINT"); result.ok = result.entry != nullptr; return result; }
  if (code == 8192 && operation_id == "engine.op.kv_structured_read" &&
      opcode == "SBLR_KV_STRUCTURED_READ") {
    result.entry = LookupSblrOpcode("SBLR_KV_STRUCTURED_READ");
    result.ok = result.entry != nullptr;
    if (!result.ok) { result.diagnostic_id = "SBLR.OPERATION.OPCODE_IDENTITY_MISMATCH"; result.detail = "kv_structured_read_registry_entry_missing"; }
    return result;
  }
  if (code == 0) {
    result.diagnostic_id = "SBLR.OPERATION.OPCODE_IDENTITY_MISMATCH";
    result.detail = "opcode_code_zero";
    return result;
  }
  const SblrOpcodeEntry* match = nullptr;
  for (const auto& entry : StaticSblrOpcodeRegistry()) {
    if (entry.code != code) continue;
    if (match != nullptr) {
      result.diagnostic_id = "SBLR.OPERATION.OPCODE_IDENTITY_MISMATCH";
      result.detail = "opcode_code_duplicate:" + std::to_string(code);
      return result;
    }
    match = &entry;
  }
  if (match == nullptr) {
    result.diagnostic_id = "SBLR.OPERATION.OPCODE_IDENTITY_MISMATCH";
    result.detail = "opcode_code_unknown:" + std::to_string(code);
    return result;
  }
  result.entry = match;
  if (match->operation_id != operation_id || match->opcode != opcode) {
    result.diagnostic_id = "SBLR.OPERATION.OPCODE_IDENTITY_MISMATCH";
    result.detail = "expected=" + match->operation_id + "/" + match->opcode +
                    ";actual=" + std::string(operation_id) + "/" +
                    std::string(opcode);
    return result;
  }
  result.ok = true;
  return result;
}

SblrOpcodeValidationResult ValidateSblrOpcodeForEnvelope(const SblrOperationEnvelope& envelope) {
  SblrOpcodeValidationResult result;
  if (envelope.opcode_code == 1029 &&
      envelope.operation_id == "engine.op.udr_invoke" &&
      envelope.opcode == "SBLR_UDR_INVOKE") {
    result.entry = LookupSblrOpcode("SBLR_UDR_INVOKE");
    result.ok = result.entry != nullptr;
    if (!result.ok) {
      result.diagnostic_id = "SB_DIAG_SBLR_UNKNOWN_OPERATION";
      result.detail = "engine.op.udr_invoke";
    }
    return result;
  }
  if (envelope.opcode_code == 8193 && envelope.operation_id == "engine.op.kv_structured_mutate" && envelope.opcode == "SBLR_KV_STRUCTURED_MUTATE") {
    result.entry = LookupSblrOpcode("SBLR_KV_STRUCTURED_MUTATE"); result.ok = result.entry != nullptr;
  }
  if (envelope.opcode_code == 1556 && envelope.operation_id == "engine.op.ddl_drop_procedure" && envelope.opcode == "SBLR_DDL_DROP_PROCEDURE") {
    result.entry = LookupSblrOpcode("SBLR_DDL_DROP_PROCEDURE"); result.ok = result.entry != nullptr;
 return result;
  }
  if (envelope.opcode_code == 1558 && envelope.operation_id == "engine.op.ddl_alter_function" && envelope.opcode == "SBLR_DDL_ALTER_FUNCTION") { result.entry = LookupSblrOpcode("SBLR_DDL_ALTER_FUNCTION"); result.ok = result.entry != nullptr; return result; }
  if (envelope.opcode_code == 1560 && envelope.operation_id == "engine.op.ddl_create_package" && envelope.opcode == "SBLR_DDL_CREATE_PACKAGE") { result.entry = LookupSblrOpcode("SBLR_DDL_CREATE_PACKAGE"); result.ok = result.entry != nullptr; }
  if (envelope.opcode_code == 1562 && envelope.operation_id == "engine.op.ddl_drop_package" && envelope.opcode == "SBLR_DDL_DROP_PACKAGE") { result.entry = LookupSblrOpcode("SBLR_DDL_DROP_PACKAGE"); result.ok = result.entry != nullptr; }
  if (envelope.opcode_code == 1575 && envelope.operation_id == "engine.op.ddl_drop_synonym" && envelope.opcode == "SBLR_DDL_DROP_SYNONYM") { result.entry = LookupSblrOpcode("SBLR_DDL_DROP_SYNONYM"); result.ok = result.entry != nullptr; }
  if (envelope.opcode_code == 1564 && envelope.operation_id == "engine.op.ddl_alter_sequence" && envelope.opcode == "SBLR_DDL_ALTER_SEQUENCE") { result.entry = LookupSblrOpcode("SBLR_DDL_ALTER_SEQUENCE"); result.ok = result.entry != nullptr; return result; }
  if (envelope.opcode_code == 1565 && envelope.operation_id == "engine.op.ddl_drop_sequence" && envelope.opcode == "SBLR_DDL_DROP_SEQUENCE") { result.entry = LookupSblrOpcode("SBLR_DDL_DROP_SEQUENCE"); result.ok = result.entry != nullptr; return result; }
  if (envelope.opcode_code == 1566 && envelope.operation_id == "engine.op.ddl_create_materialized_view" && envelope.opcode == "SBLR_DDL_CREATE_MATERIALIZED_VIEW") { result.entry = LookupSblrOpcode("SBLR_DDL_CREATE_MATERIALIZED_VIEW"); result.ok = result.entry != nullptr; }
  if (envelope.opcode_code == 1567 && envelope.operation_id == "engine.op.ddl_refresh_materialized_view" && envelope.opcode == "SBLR_DDL_REFRESH_MATERIALIZED_VIEW") { result.entry = LookupSblrOpcode("SBLR_DDL_REFRESH_MATERIALIZED_VIEW"); result.ok = result.entry != nullptr && result.entry->executor_evidence_accepted; if (!result.ok) { result.diagnostic_id = "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING"; result.detail = "executor_evidence_not_accepted:" + envelope.operation_id; } return result; }
  if (envelope.opcode_code == 1651 && envelope.operation_id == "engine.op.ddl_alter_group" && envelope.opcode == "SBLR_DDL_ALTER_GROUP") { result.entry = LookupSblrOpcode("SBLR_DDL_ALTER_GROUP"); result.ok = result.entry != nullptr; return result; }
  if (envelope.opcode_code == 1652 && envelope.operation_id == "engine.op.ddl_alter_localized_name" && envelope.opcode == "SBLR_DDL_ALTER_LOCALIZED_NAME") { result.entry = LookupSblrOpcode("SBLR_DDL_ALTER_LOCALIZED_NAME"); result.ok = result.entry != nullptr; return result; }
  if (envelope.opcode_code == 1568 && envelope.operation_id == "engine.op.ddl_drop_materialized_view" && envelope.opcode == "SBLR_DDL_DROP_MATERIALIZED_VIEW") { result.entry = LookupSblrOpcode("SBLR_DDL_DROP_MATERIALIZED_VIEW"); result.ok = result.entry != nullptr; }
  if (envelope.opcode_code == 1570 && envelope.operation_id == "engine.op.ddl_alter_type" && envelope.opcode == "SBLR_DDL_ALTER_TYPE") { result.entry = LookupSblrOpcode("SBLR_DDL_ALTER_TYPE"); result.ok = result.entry != nullptr; }
  if (envelope.opcode_code == 1571 && envelope.operation_id == "engine.op.ddl_drop_type" && envelope.opcode == "SBLR_DDL_DROP_TYPE") { result.entry = LookupSblrOpcode("SBLR_DDL_DROP_TYPE"); result.ok = result.entry != nullptr; }
  if (envelope.opcode_code == 1569 && envelope.operation_id == "engine.op.ddl_create_type" && envelope.opcode == "SBLR_DDL_CREATE_TYPE") { result.entry = LookupSblrOpcode("SBLR_DDL_CREATE_TYPE"); result.ok = result.entry != nullptr; }
  if (envelope.opcode_code == 1561 && envelope.operation_id == "engine.op.ddl_alter_package" && envelope.opcode == "SBLR_DDL_ALTER_PACKAGE") { result.entry = LookupSblrOpcode("SBLR_DDL_ALTER_PACKAGE"); result.ok = result.entry != nullptr && result.entry->executor_evidence_accepted; if (!result.ok) { result.diagnostic_id = "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING"; result.detail = "executor_evidence_not_accepted:" + envelope.operation_id; } return result; }
  if (envelope.opcode_code == 1561 && envelope.operation_id == "engine.op.ddl_create_temporary_table" && envelope.opcode == "SBLR_DDL_CREATE_TEMPORARY_TABLE") { result.entry = LookupSblrOpcode("SBLR_DDL_CREATE_TEMPORARY_TABLE"); result.ok = result.entry != nullptr; }
  if (envelope.opcode_code == 1562 && envelope.operation_id == "engine.op.ddl_drop_temporary_table" && envelope.opcode == "SBLR_DDL_DROP_TEMPORARY_TABLE") { result.entry = LookupSblrOpcode("SBLR_DDL_DROP_TEMPORARY_TABLE"); result.ok = result.entry != nullptr; }
  if (envelope.opcode_code == 1563 && envelope.operation_id == "engine.op.ddl_rename_object_vector" && envelope.opcode == "SBLR_DDL_RENAME_OBJECT_VECTOR") { result.entry = LookupSblrOpcode("SBLR_DDL_RENAME_OBJECT_VECTOR"); result.ok = result.entry != nullptr; }
  if (envelope.opcode_code == 1615 && envelope.operation_id == "engine.op.ddl_create_or_replace_srs" && envelope.opcode == "SBLR_DDL_CREATE_OR_REPLACE_SRS") { result.entry = LookupSblrOpcode("SBLR_DDL_CREATE_OR_REPLACE_SRS"); result.ok = result.entry != nullptr; return result; }
  if (envelope.opcode_code == 1616 && envelope.operation_id == "engine.op.ddl_drop_srs" && envelope.opcode == "SBLR_DDL_DROP_SRS") { result.entry = LookupSblrOpcode("SBLR_DDL_DROP_SRS"); result.ok = result.entry != nullptr; return result; }
  if (envelope.opcode_code == 1617 && envelope.operation_id == "engine.op.ddl_create_rewrite_rule" && envelope.opcode == "SBLR_DDL_CREATE_REWRITE_RULE") { result.entry = LookupSblrOpcode("SBLR_DDL_CREATE_REWRITE_RULE"); result.ok = result.entry != nullptr; return result; }
  if (envelope.opcode_code == 1618 && envelope.operation_id == "engine.op.ddl_alter_rewrite_rule" && envelope.opcode == "SBLR_DDL_ALTER_REWRITE_RULE") { result.entry = LookupSblrOpcode("SBLR_DDL_ALTER_REWRITE_RULE"); result.ok = result.entry != nullptr; return result; }
  if (envelope.opcode_code == 1619 && envelope.operation_id == "engine.op.ddl_drop_rewrite_rule" && envelope.opcode == "SBLR_DDL_DROP_REWRITE_RULE") { result.entry = LookupSblrOpcode("SBLR_DDL_DROP_REWRITE_RULE"); result.ok = result.entry != nullptr; return result; }
  if (envelope.opcode_code == 1620 && envelope.operation_id == "engine.op.ddl_validate_constraint" && envelope.opcode == "SBLR_DDL_VALIDATE_CONSTRAINT") { result.entry = LookupSblrOpcode("SBLR_DDL_VALIDATE_CONSTRAINT"); result.ok = result.entry != nullptr; return result; }
  if (envelope.opcode_code == 8194 && envelope.operation_id == "engine.op.kv_structured_scan" && envelope.opcode == "SBLR_KV_STRUCTURED_SCAN") {
    result.entry = LookupSblrOpcode("SBLR_KV_STRUCTURED_SCAN"); result.ok = result.entry != nullptr;
    return result;
  }
  if ((envelope.opcode_code == 1637 || envelope.opcode_code == 1608) && envelope.operation_id == "engine.op.ddl_create_dictionary" && envelope.opcode == "SBLR_DDL_CREATE_DICTIONARY") {
    result.entry = LookupSblrOpcode("SBLR_DDL_CREATE_DICTIONARY"); result.ok = result.entry != nullptr; return result;
  }
  if (envelope.opcode_code == 1638 && envelope.operation_id == "engine.op.ddl_drop_dictionary" && envelope.opcode == "SBLR_DDL_DROP_DICTIONARY") { result.entry = LookupSblrOpcode("SBLR_DDL_DROP_DICTIONARY"); result.ok = result.entry != nullptr; return result; }
  if (envelope.opcode_code == 1639 && envelope.operation_id == "engine.op.ddl_alter_dictionary" && envelope.opcode == "SBLR_DDL_ALTER_DICTIONARY") { result.entry = LookupSblrOpcode("SBLR_DDL_ALTER_DICTIONARY"); result.ok = result.entry != nullptr; return result; }
  if (envelope.opcode_code == 1640 && envelope.operation_id == "engine.op.ddl_create_continuous_view" && envelope.opcode == "SBLR_DDL_CREATE_CONTINUOUS_VIEW") { result.entry = LookupSblrOpcode("SBLR_DDL_CREATE_CONTINUOUS_VIEW"); result.ok = result.entry != nullptr; return result; }
  if (envelope.opcode_code == 1641 && envelope.operation_id == "engine.op.ddl_alter_continuous_view" && envelope.opcode == "SBLR_DDL_ALTER_CONTINUOUS_VIEW") { result.entry = LookupSblrOpcode("SBLR_DDL_ALTER_CONTINUOUS_VIEW"); result.ok = result.entry != nullptr; return result; }
  if (envelope.opcode_code == 1642 && envelope.operation_id == "engine.op.ddl_drop_continuous_view" && envelope.opcode == "SBLR_DDL_DROP_CONTINUOUS_VIEW") { result.entry = LookupSblrOpcode("SBLR_DDL_DROP_CONTINUOUS_VIEW"); result.ok = result.entry != nullptr; return result; }
  if (envelope.opcode_code == 1643 && envelope.operation_id == "engine.op.dml_async_insert_submit" && envelope.opcode == "SBLR_DML_ASYNC_INSERT_SUBMIT")
  if (envelope.opcode_code == 1644 && envelope.operation_id == "engine.op.dml_async_insert_status" && envelope.opcode == "SBLR_DML_ASYNC_INSERT_STATUS") { result.entry = LookupSblrOpcode("SBLR_DML_ASYNC_INSERT_STATUS"); result.ok = result.entry != nullptr; return result; }
  if (envelope.opcode_code == 1645 && envelope.operation_id == "engine.op.dml_async_insert_cancel" && envelope.opcode == "SBLR_DML_ASYNC_INSERT_CANCEL") { result.entry = LookupSblrOpcode("SBLR_DML_ASYNC_INSERT_CANCEL"); result.ok = result.entry != nullptr; return result; }
  if (envelope.opcode_code == 1646 && envelope.operation_id == "engine.op.dml_conditional_mutate" && envelope.opcode == "SBLR_DML_CONDITIONAL_MUTATE") { result.entry = LookupSblrOpcode("SBLR_DML_CONDITIONAL_MUTATE"); result.ok = result.entry != nullptr; return result; }
  if (envelope.opcode_code == 1647 && envelope.operation_id == "engine.op.dml_counter_add" && envelope.opcode == "SBLR_DML_COUNTER_ADD") { result.entry = LookupSblrOpcode("SBLR_DML_COUNTER_ADD"); result.ok = result.entry != nullptr; return result; }
  if (envelope.opcode_code == 1648 && envelope.operation_id == "engine.op.dml_timeseries_schema_write" && envelope.opcode == "SBLR_DML_TIMESERIES_SCHEMA_WRITE") { result.entry = LookupSblrOpcode("SBLR_DML_TIMESERIES_SCHEMA_WRITE"); result.ok = result.entry != nullptr; return result; }
  if (envelope.opcode_code == 1649 && envelope.operation_id == "engine.op.ddl_set_timeseries_series_cardinality_policy" && envelope.opcode == "SBLR_DDL_SET_TIMESERIES_SERIES_CARDINALITY_POLICY") { result.entry = LookupSblrOpcode("SBLR_DDL_SET_TIMESERIES_SERIES_CARDINALITY_POLICY"); result.ok = result.entry != nullptr; return result; }
  if (envelope.opcode_code == 1650 && envelope.operation_id == "engine.op.ddl_create_timeseries_value_cache" && envelope.opcode == "SBLR_DDL_CREATE_TIMESERIES_VALUE_CACHE") { result.entry = LookupSblrOpcode("SBLR_DDL_CREATE_TIMESERIES_VALUE_CACHE"); result.ok = result.entry != nullptr; return result; }
  if (envelope.opcode_code == 8195 && envelope.operation_id == "engine.op.kv_structured_stream_read" && envelope.opcode == "SBLR_KV_STRUCTURED_STREAM_READ") {
    result.entry = LookupSblrOpcode("SBLR_KV_STRUCTURED_STREAM_READ"); result.ok = result.entry != nullptr;
    return result;
  }
  if (envelope.opcode_code == 8196 && envelope.operation_id == "engine.op.kv_structured_stream_append" && envelope.opcode == "SBLR_KV_STRUCTURED_STREAM_APPEND") {
    result.entry = LookupSblrOpcode("SBLR_KV_STRUCTURED_STREAM_APPEND"); result.ok = result.entry != nullptr;
    return result;
  }
  if (envelope.opcode_code == 8197 && envelope.operation_id == "engine.op.kv_structured_timeseries" && envelope.opcode == "SBLR_KV_STRUCTURED_TIMESERIES") {
    result.entry = LookupSblrOpcode("SBLR_KV_STRUCTURED_TIMESERIES"); result.ok = result.entry != nullptr;
    return result;
  }
  if (envelope.opcode_code == 5125 && envelope.operation_id == "engine.op.system_config_set" && envelope.opcode == "SBLR_SYSTEM_CONFIG_SET") {
    result.entry = LookupSblrOpcode("SBLR_SYSTEM_CONFIG_SET"); result.ok = result.entry != nullptr;
    return result;
  }
  if (envelope.opcode_code == 1537 && envelope.operation_id == "engine.op.ddl_create_table" && envelope.opcode == "SBLR_DDL_CREATE_TABLE") {
    result.entry = LookupSblrOpcode("SBLR_DDL_CREATE_TABLE"); result.ok = result.entry != nullptr;
    return result;
  }
  if (envelope.opcode_code == 1539 && envelope.operation_id == "engine.op.ddl_drop_table" && envelope.opcode == "SBLR_DDL_DROP_TABLE") {
    result.entry = LookupSblrOpcode("SBLR_DDL_DROP_TABLE"); result.ok = result.entry != nullptr && result.entry->executor_evidence_accepted; if (!result.ok) { result.diagnostic_id = "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING"; result.detail = "executor_evidence_not_accepted:engine.op.ddl_drop_table"; }
    return result;
  }
  if (envelope.opcode_code == 8192 && envelope.operation_id == "engine.op.kv_structured_read" && envelope.opcode == "SBLR_KV_STRUCTURED_READ") {
    result.entry = LookupSblrOpcode("SBLR_KV_STRUCTURED_READ"); result.ok = result.entry != nullptr;
    if (!result.ok) { result.diagnostic_id = "SB_DIAG_SBLR_UNKNOWN_OPERATION"; result.detail = envelope.operation_id; }
    return result;
  }
  const auto* entry = LookupSblrOperation(envelope.operation_id);
  if (entry == nullptr) {
    result.diagnostic_id = "SB_DIAG_SBLR_UNKNOWN_OPERATION";
    result.detail = envelope.operation_id;
    return result;
  }
  result.entry = entry;
  if (entry->opcode != envelope.opcode) {
    result.diagnostic_id = "SB_DIAG_SBLR_OPCODE_MISMATCH";
    result.detail = "expected=" + entry->opcode + "; actual=" + envelope.opcode;
    return result;
  }
  if (entry->executor_evidence_required && !entry->executor_evidence_accepted) {
    result.diagnostic_id = entry->missing_executor_evidence_diagnostic;
    result.detail = "executor_evidence_not_accepted:" + entry->executor_id;
    return result;
  }
  if (entry->support != SblrOpcodeSupport::implemented) {
    result.diagnostic_id = entry->refusal_diagnostic.empty()
                               ? "SB_DIAG_SBLR_OPCODE_REFUSED"
                               : entry->refusal_diagnostic;
    result.detail = "operation_is_refused_by_registered_sblr_profile:" +
                    entry->operation_id;
    return result;
  }
  if (entry->requires_security_context && !envelope.requires_security_context) {
    result.diagnostic_id = "SB_DIAG_SBLR_SECURITY_CONTEXT_REQUIRED";
    result.detail = envelope.operation_id;
    return result;
  }
  if (entry->requires_transaction_context && !envelope.requires_transaction_context) {
    result.diagnostic_id = "SB_DIAG_SBLR_TRANSACTION_CONTEXT_REQUIRED";
    result.detail = envelope.operation_id;
    return result;
  }
  if (entry->requires_cluster_authority && !envelope.requires_cluster_authority) {
    result.diagnostic_id = "SB_DIAG_CLUSTER_TXN_UNAVAILABLE";
    result.detail = envelope.operation_id;
    return result;
  }
  result.ok = true;
  return result;
}

std::string ToString(SblrOpcodeCategory category) {
  switch (category) {
    case SblrOpcodeCategory::artifact: return "artifact";
    case SblrOpcodeCategory::catalog: return "catalog";
    case SblrOpcodeCategory::cluster: return "cluster";
    case SblrOpcodeCategory::core: return "core";
    case SblrOpcodeCategory::data_mutation: return "data_mutation";
    case SblrOpcodeCategory::data_read: return "data_read";
    case SblrOpcodeCategory::ddl: return "ddl";
    case SblrOpcodeCategory::dml: return "dml";
    case SblrOpcodeCategory::expression: return "expression";
    case SblrOpcodeCategory::extensibility: return "extensibility";
    case SblrOpcodeCategory::management: return "management";
    case SblrOpcodeCategory::nosql: return "nosql";
    case SblrOpcodeCategory::observability: return "observability";
    case SblrOpcodeCategory::query: return "query";
    case SblrOpcodeCategory::result_shape: return "result_shape";
    case SblrOpcodeCategory::security: return "security";
    case SblrOpcodeCategory::transaction: return "transaction";
    case SblrOpcodeCategory::unknown: return "unknown";
  }
  return "unknown";
}

std::string ToString(SblrOpcodeSupport support) {
  switch (support) {
    case SblrOpcodeSupport::implemented: return "implemented";
    case SblrOpcodeSupport::local_profile_refusal: return "local_profile_refusal";
    case SblrOpcodeSupport::cluster_refusal: return "cluster_refusal";
    case SblrOpcodeSupport::future_profile_refusal: return "future_profile_refusal";
    case SblrOpcodeSupport::deprecated_refusal: return "deprecated_refusal";
    case SblrOpcodeSupport::unsupported: return "unsupported";
  }
  return "unsupported";
}

std::string ToString(SblrOpcodeTransactionEffect effect) {
  switch (effect) {
    case SblrOpcodeTransactionEffect::none: return "none";
    case SblrOpcodeTransactionEffect::read: return "read";
    case SblrOpcodeTransactionEffect::local_write: return "local_write";
    case SblrOpcodeTransactionEffect::local_or_cluster_write: return "local_or_cluster_write";
    case SblrOpcodeTransactionEffect::catalog_write: return "catalog_write";
    case SblrOpcodeTransactionEffect::cluster_write: return "cluster_write";
    case SblrOpcodeTransactionEffect::management: return "management";
    case SblrOpcodeTransactionEffect::security: return "security";
    case SblrOpcodeTransactionEffect::external_audit: return "external_audit";
    case SblrOpcodeTransactionEffect::unknown: return "unknown";
  }
  return "unknown";
}

std::string ToString(SblrOpcodeSecurityClass security_class) {
  switch (security_class) {
    case SblrOpcodeSecurityClass::public_metadata: return "public_metadata";
    case SblrOpcodeSecurityClass::authenticated: return "authenticated";
    case SblrOpcodeSecurityClass::object_authorized: return "object_authorized";
    case SblrOpcodeSecurityClass::admin_authorized: return "admin_authorized";
    case SblrOpcodeSecurityClass::sysarch_authorized: return "sysarch_authorized";
    case SblrOpcodeSecurityClass::event_admin: return "event_admin";
    case SblrOpcodeSecurityClass::cluster_authorized: return "cluster_authorized";
    case SblrOpcodeSecurityClass::unknown: return "unknown";
  }
  return "unknown";
}

}  // namespace scratchbird::engine::sblr
