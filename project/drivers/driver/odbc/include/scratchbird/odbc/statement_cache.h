// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

/**
 * @file statement_cache.h
 * @brief Statement caching and batch operations for ScratchBird ODBC Driver
 * 
 * Part of Phase 3.8: ODBC Driver - Optional Enhancements
 */

#ifndef SCRATCHBIRD_ODBC_STATEMENT_CACHE_H
#define SCRATCHBIRD_ODBC_STATEMENT_CACHE_H

#include "scratchbird/odbc/odbc_types.h"
#include "scratchbird/odbc/odbc_handles.h"

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// Statement Cache Configuration
// =============================================================================

typedef struct sb_odbc_stmt_cache_config {
    SQLULEN max_size;
    SQLULEN ttl_seconds;
    SQLSMALLINT enable_caching;
} sb_odbc_stmt_cache_config;

// Default cache configuration
static inline sb_odbc_stmt_cache_config sb_odbc_stmt_cache_config_default(void) {
    sb_odbc_stmt_cache_config cfg = {
        .max_size = 100,
        .ttl_seconds = 3600,
        .enable_caching = SQL_TRUE
    };
    return cfg;
}

// =============================================================================
// Statement Cache Operations
// =============================================================================

/**
 * @brief Initialize statement cache for a connection
 */
SQLRETURN sb_odbc_stmt_cache_init(
    scratchbird::odbc::SQLHDBC hdbc,
    const sb_odbc_stmt_cache_config* config
);

/**
 * @brief Get cached statement handle for SQL text
 * @param hstmt Output statement handle (may be new or cached)
 * @param cache_hit Output flag indicating if statement was found in cache
 */
SQLRETURN sb_odbc_stmt_cache_get(
    scratchbird::odbc::SQLHDBC hdbc,
    SQLCHAR* sql_text,
    SQLINTEGER sql_len,
    scratchbird::odbc::SQLHSTMT* hstmt,
    SQLSMALLINT* cache_hit
);

/**
 * @brief Return statement to cache for reuse
 */
SQLRETURN sb_odbc_stmt_cache_put(
    scratchbird::odbc::SQLHDBC hdbc,
    scratchbird::odbc::SQLHSTMT hstmt,
    SQLCHAR* sql_text,
    SQLINTEGER sql_len
);

/**
 * @brief Invalidate cached statement
 */
SQLRETURN sb_odbc_stmt_cache_invalidate(
    scratchbird::odbc::SQLHDBC hdbc,
    SQLCHAR* sql_text,
    SQLINTEGER sql_len
);

/**
 * @brief Clear all cached statements for connection
 */
SQLRETURN sb_odbc_stmt_cache_clear(scratchbird::odbc::SQLHDBC hdbc);

/**
 * @brief Get cache statistics
 */
SQLRETURN sb_odbc_stmt_cache_stats(
    scratchbird::odbc::SQLHDBC hdbc,
    SQLULEN* cached_count,
    SQLULEN* hit_count,
    SQLULEN* miss_count
);

/**
 * @brief Destroy statement cache
 */
SQLRETURN sb_odbc_stmt_cache_destroy(scratchbird::odbc::SQLHDBC hdbc);

// =============================================================================
// Batch Operations
// =============================================================================

typedef struct sb_odbc_batch_op {
    SQLCHAR* sql;
    SQLINTEGER sql_len;
    SQLPOINTER* params;
    SQLLEN* param_lens;
    SQLSMALLINT param_count;
    const SQLSMALLINT* param_c_types;
    const SQLSMALLINT* param_sql_types;
    const SQLULEN* param_column_sizes;
    const SQLSMALLINT* param_decimal_digits;
} sb_odbc_batch_op;

/**
 * @brief Execute multiple prepared statements as a batch
 * 
 * This function optimizes batch execution by:
 * - Reusing prepared statements
 * - Minimizing network round trips
 * - Using array binding where supported
 */
SQLRETURN sb_odbc_batch_execute(
    scratchbird::odbc::SQLHSTMT hstmt,
    const sb_odbc_batch_op* operations,
    SQLULEN operation_count,
    SQLULEN* rows_affected,
    SQLULEN* error_index
);

/**
 * @brief Execute bulk insert using array binding
 * 
 * Efficiently inserts multiple rows using a single INSERT statement
 * with array-bound parameters.
 */
SQLRETURN sb_odbc_bulk_insert(
    scratchbird::odbc::SQLHSTMT hstmt,
    SQLCHAR* table_name,
    SQLCHAR** columns,
    SQLSMALLINT column_count,
    SQLPOINTER* data,
    SQLLEN* data_lens,
    SQLULEN row_count,
    SQLULEN* rows_inserted
);

/**
 * @brief Execute bulk insert using explicit parameter type metadata.
 *
 * The type arrays are indexed by column (0..column_count-1). Pass nullptr to
 * use default string binding behavior for that type attribute.
 */
SQLRETURN sb_odbc_bulk_insert_ex(
    scratchbird::odbc::SQLHSTMT hstmt,
    SQLCHAR* table_name,
    SQLCHAR** columns,
    SQLSMALLINT column_count,
    SQLPOINTER* data,
    SQLLEN* data_lens,
    const SQLSMALLINT* param_c_types,
    const SQLSMALLINT* param_sql_types,
    const SQLULEN* param_column_sizes,
    const SQLSMALLINT* param_decimal_digits,
    SQLULEN row_count,
    SQLULEN* rows_inserted
);

// =============================================================================
// Connection Retry Configuration
// =============================================================================

typedef struct sb_odbc_retry_config {
    SQLULEN max_retries;
    SQLULEN base_delay_ms;
    SQLULEN max_delay_ms;
    SQLSMALLINT retry_on_connection_lost;
    SQLSMALLINT retry_on_deadlock;
} sb_odbc_retry_config;

static inline sb_odbc_retry_config sb_odbc_retry_config_default(void) {
    sb_odbc_retry_config cfg = {
        .max_retries = 3,
        .base_delay_ms = 100,
        .max_delay_ms = 5000,
        .retry_on_connection_lost = SQL_TRUE,
        .retry_on_deadlock = SQL_TRUE
    };
    return cfg;
}

/**
 * @brief Execute operation with automatic retry
 */
SQLRETURN sb_odbc_with_retry(
    scratchbird::odbc::SQLHDBC hdbc,
    const sb_odbc_retry_config* config,
    SQLRETURN (*operation)(void* user_data),
    void* user_data,
    SQLULEN* attempt_count
);

// =============================================================================
// Connection Health Check
// =============================================================================

/**
 * @brief Check if connection is healthy (non-blocking)
 */
SQLRETURN sb_odbc_connection_is_healthy(
    scratchbird::odbc::SQLHDBC hdbc,
    SQLSMALLINT* is_healthy
);

/**
 * @brief Validate connection with optional timeout
 */
SQLRETURN sb_odbc_connection_validate(
    scratchbird::odbc::SQLHDBC hdbc,
    SQLULEN timeout_ms,
    SQLSMALLINT* is_valid
);

/**
 * @brief Get connection metrics
 */
typedef struct sb_odbc_connection_metrics {
    SQLULEN connection_time_ms;
    SQLULEN total_queries;
    SQLULEN total_errors;
    SQLULEN last_activity_ms_ago;
} sb_odbc_connection_metrics;

SQLRETURN sb_odbc_connection_get_metrics(
    scratchbird::odbc::SQLHDBC hdbc,
    sb_odbc_connection_metrics* metrics
);

#ifdef __cplusplus
}
#endif

#endif // SCRATCHBIRD_ODBC_STATEMENT_CACHE_H
