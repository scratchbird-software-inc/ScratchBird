// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

typedef struct sb_connection sb_connection;
typedef struct sb_prepared sb_prepared;
typedef struct sb_result sb_result;
typedef struct sb_row {
    struct sb_result* result;
    size_t row_index;
} sb_row;

typedef enum sb_error_code {
    SB_OK = 0,
    SB_ERR_CONNECTION_FAILED = 1,
    SB_ERR_AUTH_FAILED = 2,
    SB_ERR_SSL_FAILED = 3,
    SB_ERR_TIMEOUT = 4,
    SB_ERR_DISCONNECTED = 5,
    SB_ERR_PROTOCOL = 6,
    SB_ERR_SYNTAX = 100,
    SB_ERR_CONSTRAINT = 105,
    SB_ERR_TYPE_MISMATCH = 104,
    SB_ERR_TXN_CONFLICT = 200,
    SB_ERR_DEADLOCK = 201,
    SB_ERR_SERIALIZATION = 202,
    SB_ERR_TXN_ABORTED = 203,
    SB_ERR_NO_ACTIVE_TXN = 204,
    SB_ERR_OUT_OF_MEMORY = 300,
    SB_ERR_DISK_FULL = 301,
    SB_ERR_TOO_MANY_CONNECTIONS = 302,
    SB_ERR_RESOURCE_BUSY = 303,
    SB_ERR_INVALID_HANDLE = 400,
    SB_ERR_INVALID_PARAM = 401,
    SB_ERR_PARAM_COUNT = 402,
    SB_ERR_NULL_POINTER = 403,
    SB_ERR_INVALID_STATE = 500,
    SB_ERR_ALREADY_CONNECTED = 501,
    SB_ERR_NOT_CONNECTED = 502,
    SB_ERR_RESULT_EXHAUSTED = 503,
    SB_ERR_STATEMENT_CLOSED = 504,
    SB_ERR_NOT_IMPLEMENTED = 901,
    SB_ERR_UNKNOWN = 999
} sb_error_code;

typedef struct sb_error {
    sb_error_code code;
    char message[256];
} sb_error;

/* SB_PUBLIC_ABI_ALLOCATION_OWNERSHIP
 *
 * Public C ABI ownership rules:
 *   - sb_connection, sb_result, and sb_prepared handles are driver-owned handles
 *     and must be released with their matching sb_*_free function.
 *   - char* values returned by this API are driver-owned C buffers and must be
 *     released with sb_memory_release or sb_memory_free.
 *   - const char*, const uint8_t*, and sb_value pointers returned from rows,
 *     metadata, notifications, or callbacks are borrowed; they remain valid only
 *     for the documented parent handle/callback lifetime and must not be freed
 *     by the caller.
 *   - sb_error is caller-owned storage populated by the driver; the driver never
 *     allocates or frees sb_error.
 *   - Ownership evidence is diagnostic only. It is not transaction finality,
 *     visibility, security/authorization, recovery, parser, reference, or benchmark
 *     authority.
 */
typedef enum sb_memory_ownership_kind {
    SB_MEMORY_OWNERSHIP_UNKNOWN = 0,
    SB_MEMORY_OWNERSHIP_DRIVER_ALLOCATED = 1,
    SB_MEMORY_OWNERSHIP_DRIVER_HANDLE = 2,
    SB_MEMORY_OWNERSHIP_BORROWED = 3,
    SB_MEMORY_OWNERSHIP_CALLER = 4
} sb_memory_ownership_kind;

typedef struct sb_memory_ownership_info {
    uint32_t abi_version;
    sb_memory_ownership_kind ownership_kind;
    size_t bytes;
    char purpose[64];
    char release_function[64];
    char authority_scope[192];
} sb_memory_ownership_info;

typedef enum sb_retry_scope {
    SB_RETRY_SCOPE_NONE = 0,
    SB_RETRY_SCOPE_RECONNECT = 1,
    SB_RETRY_SCOPE_STATEMENT = 2,
    SB_RETRY_SCOPE_TRANSACTION = 3
} sb_retry_scope;

typedef enum sb_type {
    SB_TYPE_NULL = 0,
    SB_TYPE_BOOLEAN = 1,
    SB_TYPE_SMALLINT = 2,
    SB_TYPE_INTEGER = 3,
    SB_TYPE_BIGINT = 4,
    SB_TYPE_REAL = 5,
    SB_TYPE_DOUBLE = 6,
    SB_TYPE_DECIMAL = 7,
    SB_TYPE_JSONB = 8,
    SB_TYPE_CHAR = 10,
    SB_TYPE_VARCHAR = 11,
    SB_TYPE_TEXT = 12,
    SB_TYPE_XML = 13,
    SB_TYPE_TSVECTOR = 14,
    SB_TYPE_TSQUERY = 15,
    SB_TYPE_BLOB = 20,
    SB_TYPE_MONEY = 21,
    SB_TYPE_DATE = 30,
    SB_TYPE_TIME = 31,
    SB_TYPE_TIME_TZ = 35,
    SB_TYPE_TIMESTAMP = 32,
    SB_TYPE_TIMESTAMP_TZ = 33,
    SB_TYPE_INTERVAL = 34,
    SB_TYPE_UUID = 40,
    SB_TYPE_JSON = 41,
    SB_TYPE_GEOMETRY = 42,
    SB_TYPE_ARRAY = 50,
    SB_TYPE_COMPOSITE = 51,
    SB_TYPE_RANGE = 52,
    SB_TYPE_VECTOR = 53,
    SB_TYPE_INET = 60,
    SB_TYPE_CIDR = 61,
    SB_TYPE_MACADDR = 62,
    SB_TYPE_UNKNOWN = 99
} sb_type;

typedef struct sb_value {
    sb_type type;
    int is_null;
    uint32_t type_oid;
    union {
        int8_t boolean_val;
        int16_t smallint_val;
        int32_t integer_val;
        int64_t bigint_val;
        float real_val;
        double double_val;
        int64_t money_val;
        struct {
            const char* data;
            size_t length;
        } string_val;
        struct {
            const uint8_t* data;
            size_t length;
        } binary_val;
        struct {
            int64_t micros;
            int32_t days;
            int32_t months;
        } interval_val;
        struct {
            int32_t year;
            int32_t month;
            int32_t day;
        } date_val;
        struct {
            int32_t hour;
            int32_t minute;
            int32_t second;
            int32_t microsecond;
        } time_val;
        struct {
            int64_t epoch_microseconds;
            int32_t tz_offset_seconds;
        } timestamp_val;
        struct {
            uint8_t bytes[16];
        } uuid_val;
    } data;
} sb_value;

typedef struct sb_column_meta {
    const char* name;
    sb_type type;
    int32_t type_modifier;
    int nullable;
} sb_column_meta;

enum {
    SB_TXN_FLAG_HAS_ISOLATION = 0x0001,
    SB_TXN_FLAG_HAS_ACCESS_MODE = 0x0002,
    SB_TXN_FLAG_HAS_DEFERRABLE = 0x0004,
    SB_TXN_FLAG_HAS_WAIT_MODE = 0x0008,
    SB_TXN_FLAG_HAS_TIMEOUT = 0x0010,
    SB_TXN_FLAG_HAS_AUTOCOMMIT = 0x0020,
    SB_TXN_FLAG_HAS_READ_COMMITTED_MODE = 0x0100
};

typedef enum sb_read_committed_mode {
    SB_READ_COMMITTED_MODE_DEFAULT = 0,
    SB_READ_COMMITTED_MODE_READ_CONSISTENCY = 1,
    SB_READ_COMMITTED_MODE_RECORD_VERSION = 2,
    SB_READ_COMMITTED_MODE_NO_RECORD_VERSION = 3
} sb_read_committed_mode;

typedef struct sb_txn_options {
    uint16_t flags;
    uint8_t conflict_action;
    uint8_t autocommit_mode;
    /* SQL-style compatibility aliases:
     *   0 => READ UNCOMMITTED (legacy alias, not a distinct canonical MGA mode)
     *   1 => READ COMMITTED
     *   2 => REPEATABLE READ => canonical SNAPSHOT
     *   3 => SERIALIZABLE => canonical SNAPSHOT TABLE STABILITY
     * READ COMMITTED READ CONSISTENCY is requested separately via
     * read_committed_mode plus SB_TXN_FLAG_HAS_READ_COMMITTED_MODE.
     */
    uint8_t isolation_level;
    uint8_t read_committed_mode;
    uint8_t access_mode;
    uint8_t deferrable;
    uint8_t wait_mode;
    uint32_t timeout_ms;
} sb_txn_options;

static inline const char* sb_canonical_isolation_name(uint8_t isolation_level) {
    switch (isolation_level) {
        case 0:
        case 1:
            return "READ COMMITTED";
        case 2:
            return "SNAPSHOT";
        case 3:
            return "SNAPSHOT TABLE STABILITY";
        default:
            return "UNKNOWN";
    }
}

static inline const char* sb_canonical_read_committed_mode_name(uint8_t read_committed_mode) {
    switch (read_committed_mode) {
        case SB_READ_COMMITTED_MODE_DEFAULT:
            return "DEFAULT";
        case SB_READ_COMMITTED_MODE_READ_CONSISTENCY:
            return "READ CONSISTENCY";
        case SB_READ_COMMITTED_MODE_RECORD_VERSION:
            return "RECORD VERSION";
        case SB_READ_COMMITTED_MODE_NO_RECORD_VERSION:
            return "NO RECORD VERSION";
        default:
            return "UNKNOWN";
    }
}

static inline sb_retry_scope sb_retry_scope_for_sqlstate(const char* sqlstate) {
    if (!sqlstate ||
        !sqlstate[0] || !sqlstate[1] || !sqlstate[2] || !sqlstate[3] || !sqlstate[4] || sqlstate[5]) {
        return SB_RETRY_SCOPE_NONE;
    }
    if ((sqlstate[0] == '4' && sqlstate[1] == '0' &&
         ((sqlstate[2] == '0' && sqlstate[3] == '0' && sqlstate[4] == '1') ||
          (sqlstate[2] == 'P' && sqlstate[3] == '0' && sqlstate[4] == '1')))) {
        return SB_RETRY_SCOPE_STATEMENT;
    }
    if (sqlstate[0] == '0' && sqlstate[1] == '8') {
        return SB_RETRY_SCOPE_RECONNECT;
    }
    return SB_RETRY_SCOPE_NONE;
}

static inline int sb_is_retryable_sqlstate(const char* sqlstate) {
    return sb_retry_scope_for_sqlstate(sqlstate) != SB_RETRY_SCOPE_NONE;
}

static inline int sb_supports_prepared_transactions(void) {
    return 1;
}

static inline int sb_supports_dormant_reattach(void) {
    return 0;
}

static inline int sb_supports_portal_resume(void) {
    return 0;
}

typedef void (*sb_notification_listener_fn)(const char* channel,
                                            const uint8_t* payload,
                                            size_t payload_len,
                                            uint32_t process_id,
                                            uint8_t change_type,
                                            uint64_t row_id,
                                            int has_row_id,
                                            void* user_data);

static inline const char* sb_metadata_schemas_query(void) {
    return "SELECT schema_id, schema_name, owner_id, default_tablespace_id FROM sys.schemas WHERE is_valid = 1 ORDER BY schema_name";
}

static inline const char* sb_metadata_tables_query(void) {
    return "SELECT table_id, schema_id, table_name, table_type, owner_id FROM sys.tables WHERE is_valid = 1 ORDER BY table_name";
}

static inline const char* sb_metadata_columns_query(void) {
    return "SELECT column_id, table_id, column_name, data_type_id, data_type_name, ordinal_position, is_nullable, default_value, domain_id, collation_id, charset_id, is_identity, is_generated, generation_expression FROM sys.columns WHERE is_valid = 1 ORDER BY table_id, ordinal_position";
}

static inline const char* sb_metadata_indexes_query(void) {
    return "SELECT index_id, table_id, index_name, index_type, is_unique FROM sys.indexes WHERE is_valid = 1 ORDER BY table_id, index_name";
}

static inline const char* sb_metadata_index_columns_query(void) {
    return "SELECT index_id, column_id, column_name, ordinal_position, is_included FROM sys.index_columns ORDER BY index_id, ordinal_position";
}

static inline const char* sb_metadata_constraints_query(void) {
    return "SELECT constraint_id, table_id, constraint_name, constraint_type FROM sys.constraints WHERE is_valid = 1 ORDER BY table_id, constraint_name";
}

static inline const char* sb_metadata_procedures_query(void) {
    return "SELECT procedure_id, schema_id, procedure_name, routine_type FROM sys.procedures WHERE is_valid = 1 ORDER BY schema_id, procedure_name";
}

static inline const char* sb_metadata_functions_query(void) {
    return "SELECT function_id, schema_id, function_name FROM sys.functions WHERE is_valid = 1 ORDER BY schema_id, function_name";
}

static inline const char* sb_metadata_routines_query(void) {
    return "SELECT procedure_id AS routine_id, schema_id, procedure_name AS routine_name, routine_type FROM sys.procedures WHERE is_valid = 1 UNION ALL SELECT function_id AS routine_id, schema_id, function_name AS routine_name, 'FUNCTION' AS routine_type FROM sys.functions WHERE is_valid = 1 ORDER BY schema_id, routine_name";
}

static inline const char* sb_metadata_catalogs_query(void) {
    return "SELECT schema_id AS catalog_id, schema_name AS catalog_name FROM sys.schemas WHERE is_valid = 1 ORDER BY schema_name";
}

static inline const char* sb_metadata_primary_keys_query(void) {
    return "SELECT constraint_id, table_id, constraint_name, constraint_type FROM sys.constraints WHERE is_valid = 1 AND lower(constraint_type) IN ('primary key', 'primary') ORDER BY table_id, constraint_name";
}

static inline const char* sb_metadata_foreign_keys_query(void) {
    return "SELECT constraint_id, table_id, constraint_name, constraint_type FROM sys.constraints WHERE is_valid = 1 AND lower(constraint_type) IN ('foreign key', 'foreign') ORDER BY table_id, constraint_name";
}

static inline const char* sb_metadata_table_privileges_query(void) {
    return "SELECT table_id, table_name, owner_id AS grantor_id, owner_id AS grantee_id, 'ALL' AS privilege_type FROM sys.tables WHERE is_valid = 1 ORDER BY table_id, table_name";
}

static inline const char* sb_metadata_column_privileges_query(void) {
    return "SELECT table_id, column_id, column_name, 'ALL' AS privilege_type FROM sys.columns WHERE is_valid = 1 ORDER BY table_id, ordinal_position";
}

static inline const char* sb_metadata_type_info_query(void) {
    return "SELECT DISTINCT data_type_id, data_type_name FROM sys.columns WHERE is_valid = 1 ORDER BY data_type_name";
}

static inline const char* sb_metadata_ddl_fields_query(void) {
    return "SELECT table_id, column_id, column_name, data_type_name, default_value, generation_expression, is_nullable, is_identity, is_generated FROM sys.columns WHERE is_valid = 1 ORDER BY table_id, ordinal_position";
}

sb_connection* sb_connect(const char* conn_str, sb_error* err);
void sb_disconnect(sb_connection* conn);
char* sb_probe_auth_surface_json(const char* conn_str, sb_error* err);
char* sb_get_resolved_auth_context_json(sb_connection* conn, sb_error* err);

sb_result* sb_execute(sb_connection* conn, const char* sql, sb_error* err);
int sb_execute_copy_from_buffer(sb_connection* conn,
                                const char* sql,
                                const char* data,
                                size_t data_size,
                                int64_t* rows_affected_out,
                                sb_error* err);
sb_result* sb_query(sb_connection* conn, const char* sql, sb_error* err);
sb_result* sb_metadata_query(sb_connection* conn, const char* collection_name, sb_error* err);
char* sb_metadata_schema_payload(sb_connection* conn,
                                 const char* schema_pattern,
                                 int expand_schema_parents,
                                 sb_error* err);
int sb_cancel(sb_connection* conn, sb_error* err);
int sb_set_option(sb_connection* conn, const char* name, const char* value, sb_error* err);
int sb_ping(sb_connection* conn, sb_error* err);
int sb_is_healthy(sb_connection* conn, sb_error* err);

int sb_fetch(sb_result* result, sb_row* row, sb_error* err);
void sb_result_free(sb_result* result);
int64_t sb_rows_affected(sb_result* result);
int sb_memory_describe(const void* ptr, sb_memory_ownership_info* out, sb_error* err);
int sb_memory_release(void* ptr, sb_error* err);
void sb_memory_free(void* ptr);

int sb_column_count(sb_result* result);
int sb_get_column_meta(sb_result* result, int index, sb_column_meta* out);

int sb_value_get(sb_row* row, int column, sb_value* out);
int sb_get_int64(sb_row* row, int column, int64_t* out);
const char* sb_get_string(sb_row* row, int column, size_t* length);

sb_prepared* sb_prepare(sb_connection* conn, const char* sql, sb_error* err);
int sb_bind_index(sb_prepared* stmt, size_t index, const sb_value* value, sb_error* err);
int sb_bind_name(sb_prepared* stmt, const char* name, const sb_value* value, sb_error* err);
sb_result* sb_execute_prepared(sb_prepared* stmt, sb_error* err);
void sb_prepared_free(sb_prepared* stmt);

int sb_tx_begin(sb_connection* conn, sb_error* err);
int sb_tx_begin_ex(sb_connection* conn, const sb_txn_options* options, sb_error* err);
int sb_tx_commit(sb_connection* conn, sb_error* err);
int sb_tx_rollback(sb_connection* conn, sb_error* err);
int sb_tx_prepare_transaction(sb_connection* conn, const char* global_transaction_id, sb_error* err);
int sb_tx_commit_prepared(sb_connection* conn, const char* global_transaction_id, sb_error* err);
int sb_tx_rollback_prepared(sb_connection* conn, const char* global_transaction_id, sb_error* err);
int sb_tx_detach_to_dormant(sb_connection* conn, sb_error* err);
int sb_tx_reattach_dormant(sb_connection* conn,
                           const char* dormant_id,
                           const char* auth_token,
                           sb_error* err);
int sb_tx_savepoint(sb_connection* conn, const char* name, sb_error* err);
int sb_tx_release_savepoint(sb_connection* conn, const char* name, sb_error* err);
int sb_tx_rollback_to(sb_connection* conn, const char* name, sb_error* err);

int sb_subscribe(sb_connection* conn, uint8_t subscribe_type,
                 const char* channel, const char* filter, sb_error* err);
int sb_unsubscribe(sb_connection* conn, const char* channel, sb_error* err);
int sb_listen(sb_connection* conn, const char* channel, const char* filter, sb_error* err);
int sb_unlisten(sb_connection* conn, const char* channel, sb_error* err);
int sb_unlisten_all(sb_connection* conn, sb_error* err);
int sb_notify_channel(sb_connection* conn,
                      const char* channel,
                      const uint8_t* payload,
                      size_t payload_len,
                      sb_error* err);
int sb_poll_notifications(sb_connection* conn, sb_error* err);
size_t sb_notification_count(sb_connection* conn);
char* sb_get_notification(sb_connection* conn, sb_error* err);
char* sb_get_notifications(sb_connection* conn, sb_error* err);
void sb_clear_notifications(sb_connection* conn);
uint64_t sb_add_notification_listener(sb_connection* conn,
                                      sb_notification_listener_fn listener,
                                      void* user_data,
                                      sb_error* err);
int sb_remove_notification_listener(sb_connection* conn,
                                    uint64_t listener_id,
                                    sb_error* err);
int sb_stream_control(sb_connection* conn, uint8_t control_type,
                      uint32_t window_size, uint32_t timeout_ms, sb_error* err);

char* sb_get_diagnostics_json(sb_connection* conn, sb_error* err);
char* sb_get_telemetry_summary_json(sb_connection* conn, sb_error* err);
int sb_reset_telemetry(sb_connection* conn, sb_error* err);
char* sb_get_slow_operations_json(sb_connection* conn, sb_error* err);
char* sb_export_telemetry_prometheus(sb_connection* conn, sb_error* err);
char* sb_get_circuit_breaker_summary_json(sb_connection* conn, sb_error* err);
char* sb_get_keepalive_summary_json(sb_connection* conn, sb_error* err);
char* sb_get_leak_summary_json(sb_connection* conn, sb_error* err);

int sb_attach_create(sb_connection* conn, const char* mode, const char* db_name, sb_error* err);
int sb_attach_detach(sb_connection* conn, sb_error* err);
sb_result* sb_attach_list(sb_connection* conn, sb_error* err);

sb_result* sb_execute_sblr(sb_connection* conn,
                           uint64_t sblr_hash,
                           const uint8_t* bytecode,
                           size_t bytecode_len,
                           const sb_value* params,
                           size_t param_count,
                           sb_error* err);

#ifdef SCRATCHBIRD_TEST_HOOKS
sb_type sb_test_map_type_oid(uint32_t oid);
uint32_t sb_test_map_sb_type_to_oid(sb_type type);
char* sb_test_allocate_owned_memory(const char* payload, sb_error* err);
size_t sb_test_driver_owned_allocation_count(void);
#endif

#ifdef __cplusplus
}
#endif
