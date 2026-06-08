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
#include <time.h>

#include "scratchbird/client/scratchbird_client.h"

// =============================================================================
// Connection Pool Configuration
// =============================================================================

typedef struct sb_pool_config {
    size_t min_connections;
    size_t max_connections;
    uint32_t max_lifetime_seconds;
    uint32_t idle_timeout_seconds;
    uint32_t acquire_timeout_seconds;
    int test_on_checkout;
} sb_pool_config;

// Default pool configuration
static inline sb_pool_config sb_pool_config_default(void) {
    sb_pool_config cfg = {
        .min_connections = 1,
        .max_connections = 10,
        .max_lifetime_seconds = 3600,
        .idle_timeout_seconds = 600,
        .acquire_timeout_seconds = 30,
        .test_on_checkout = 1
    };
    return cfg;
}

// =============================================================================
// Connection Pool
// =============================================================================

typedef struct sb_connection_pool sb_connection_pool;

// Create a new connection pool
sb_connection_pool* sb_pool_create(const char* conn_str,
                                   const sb_pool_config* config,
                                   sb_error* err);

// Destroy the pool and close all connections
void sb_pool_destroy(sb_connection_pool* pool);

// Acquire a connection from the pool
sb_connection* sb_pool_acquire(sb_connection_pool* pool, sb_error* err);

// Release a connection back to the pool
void sb_pool_release(sb_connection_pool* pool, sb_connection* conn);

// Get pool statistics
typedef struct sb_pool_stats {
    size_t available_connections;
    size_t total_connections;
    size_t max_connections;
} sb_pool_stats;

sb_pool_stats sb_pool_get_stats(sb_connection_pool* pool);

// =============================================================================
// Retry Configuration
// =============================================================================

typedef struct sb_retry_config {
    uint32_t max_retries;
    uint32_t base_delay_ms;
    uint32_t max_delay_ms;
    int retry_connection_errors;
    int retry_transaction_errors;
} sb_retry_config;

static inline sb_retry_config sb_retry_config_default(void) {
    sb_retry_config cfg = {
        .max_retries = 3,
        .base_delay_ms = 100,
        .max_delay_ms = 5000,
        .retry_connection_errors = 1,
        .retry_transaction_errors = 1
    };
    return cfg;
}

// =============================================================================
// Retry Utility Functions
// =============================================================================

// Execute a function with retry logic
typedef sb_error (*sb_retryable_func)(void* user_data);

sb_error sb_with_retry(const sb_retry_config* config,
                       sb_retryable_func func,
                       void* user_data);

// Execute query with retry
sb_result* sb_query_with_retry(sb_connection* conn,
                               const char* sql,
                               const sb_retry_config* config,
                               sb_error* err);

// Execute with retry
sb_result* sb_execute_with_retry(sb_connection* conn,
                                 const char* sql,
                                 const sb_retry_config* config,
                                 sb_error* err);

// =============================================================================
// Batch Operations
// =============================================================================

typedef struct sb_batch_operation {
    const char* sql;
    sb_value** params;
    size_t param_count;
} sb_batch_operation;

// Execute multiple prepared statements in a batch
// Returns number of rows affected for each operation
int64_t* sb_batch_execute(sb_connection* conn,
                          const sb_batch_operation* operations,
                          size_t operation_count,
                          int64_t* rows_affected_out,
                          sb_error* err);

// Bulk insert using COPY protocol
int sb_bulk_insert(sb_connection* conn,
                   const char* table_name,
                   const char** columns,
                   size_t column_count,
                   const sb_value** rows,
                   size_t row_count,
                   int64_t* rows_inserted,
                   sb_error* err);

// =============================================================================
// Connection Health Check
// =============================================================================

// Check if connection is healthy (non-blocking)
int sb_connection_is_healthy(sb_connection* conn);

// Validate connection with ping
int sb_connection_validate(sb_connection* conn, uint32_t timeout_ms, sb_error* err);

// Get connection age in seconds
uint32_t sb_connection_get_age_seconds(sb_connection* conn);

// Get connection idle time in seconds
uint32_t sb_connection_get_idle_seconds(sb_connection* conn);

// =============================================================================
// Statement Cache
// =============================================================================

typedef struct sb_statement_cache sb_statement_cache;

// Create statement cache
sb_statement_cache* sb_stmt_cache_create(size_t max_size);

// Destroy statement cache
void sb_stmt_cache_destroy(sb_statement_cache* cache);

// Get or prepare statement from cache.
// Cached statements are owned by the cache; do not call sb_prepared_free on them.
sb_prepared* sb_stmt_cache_get(sb_statement_cache* cache,
                               sb_connection* conn,
                               const char* sql,
                               sb_error* err);

// Invalidate cached statement
void sb_stmt_cache_invalidate(sb_statement_cache* cache, const char* sql);

// Clear all cached statements
void sb_stmt_cache_clear(sb_statement_cache* cache);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus

#include <string>

namespace scratchbird {
namespace client {

struct PoolStats {
    size_t available_connections{0};
    size_t total_connections{0};
    size_t max_connections{0};
};

class ConnectionLease {
public:
    ConnectionLease();
    ~ConnectionLease();

    ConnectionLease(ConnectionLease&& other) noexcept;
    ConnectionLease& operator=(ConnectionLease&& other) noexcept;
    ConnectionLease(const ConnectionLease&) = delete;
    ConnectionLease& operator=(const ConnectionLease&) = delete;

    bool valid() const;
    sb_connection* raw() const;
    sb_result* query(const std::string& sql, sb_error* err = nullptr) const;
    sb_result* execute(const std::string& sql, sb_error* err = nullptr) const;
    void reset();

private:
    friend class ConnectionPool;
    ConnectionLease(sb_connection_pool* pool, sb_connection* conn);

    sb_connection_pool* pool_{nullptr};
    sb_connection* conn_{nullptr};
};

class ConnectionPool {
public:
    ConnectionPool();
    ~ConnectionPool();

    ConnectionPool(ConnectionPool&& other) noexcept;
    ConnectionPool& operator=(ConnectionPool&& other) noexcept;
    ConnectionPool(const ConnectionPool&) = delete;
    ConnectionPool& operator=(const ConnectionPool&) = delete;

    bool open(const std::string& conn_str,
              const sb_pool_config& config = sb_pool_config_default(),
              sb_error* err = nullptr);
    void close();
    bool isOpen() const;
    PoolStats stats() const;
    ConnectionLease acquire(sb_error* err = nullptr);

private:
    sb_connection_pool* pool_{nullptr};
};

} // namespace client
} // namespace scratchbird

#endif
