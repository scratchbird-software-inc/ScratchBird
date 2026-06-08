// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

/**
 * @file statement_cache.cpp
 * @brief Statement caching and batch operations for ScratchBird ODBC Driver
 */

#include "scratchbird/odbc/statement_cache.h"
#include "scratchbird/odbc/odbc_handles.h"

#include <algorithm>
#include <cstring>
#include <cstdint>
#include <unordered_map>
#include <list>
#include <chrono>
#include <thread>
#include <vector>

using namespace scratchbird::odbc;

namespace {

struct CacheEntry {
    OdbcStatement* stmt{nullptr};
    std::string sql;
    std::chrono::steady_clock::time_point last_used;
    bool in_use{false};
};

struct CacheState {
    sb_odbc_stmt_cache_config config{sb_odbc_stmt_cache_config_default()};
    std::unordered_map<std::string, CacheEntry> entries;
    std::list<std::string> lru;
    SQLULEN hits{0};
    SQLULEN misses{0};
};

std::mutex g_cache_mutex;
std::unordered_map<SQLHDBC, CacheState> g_caches;

static void touch_lru(CacheState& state, const std::string& key) {
    state.lru.remove(key);
    state.lru.push_front(key);
}

static void evict_if_needed(CacheState& state, OdbcConnection* conn) {
    while (state.entries.size() > state.config.max_size && !state.lru.empty()) {
        const std::string key = state.lru.back();
        state.lru.pop_back();
        auto it = state.entries.find(key);
        if (it != state.entries.end()) {
            if (it->second.stmt) {
                it->second.stmt->freeStmt(SQL_CLOSE);
                conn->removeStatement(it->second.stmt);
            }
            state.entries.erase(it);
        }
    }
}

static void purge_expired(CacheState& state, OdbcConnection* conn) {
    if (state.config.ttl_seconds == 0) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    for (auto it = state.entries.begin(); it != state.entries.end();) {
        auto age = std::chrono::duration_cast<std::chrono::seconds>(now - it->second.last_used).count();
        if (age > static_cast<long long>(state.config.ttl_seconds) && !it->second.in_use) {
            state.lru.remove(it->first);
            if (it->second.stmt) {
                it->second.stmt->freeStmt(SQL_CLOSE);
                conn->removeStatement(it->second.stmt);
            }
            it = state.entries.erase(it);
            continue;
        }
        ++it;
    }
}

static CacheState* get_state(SQLHDBC hdbc) {
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    auto it = g_caches.find(hdbc);
    if (it == g_caches.end()) {
        return nullptr;
    }
    return &it->second;
}

static SQLLEN fixed_size_for_c_type(SQLSMALLINT c_type) {
    switch (c_type) {
        case SQL_C_LONG:
        case SQL_C_SLONG:
            return static_cast<SQLLEN>(sizeof(SQLINTEGER));
        case SQL_C_SHORT:
        case SQL_C_SSHORT:
            return static_cast<SQLLEN>(sizeof(SQLSMALLINT));
        case SQL_C_SBIGINT:
            return static_cast<SQLLEN>(sizeof(int64_t));
        case SQL_C_DOUBLE:
            return static_cast<SQLLEN>(sizeof(SQLDOUBLE));
        case SQL_C_FLOAT:
            return static_cast<SQLLEN>(sizeof(SQLREAL));
        case SQL_C_BIT:
            return static_cast<SQLLEN>(sizeof(SQLCHAR));
        case SQL_C_DATE:
            return static_cast<SQLLEN>(sizeof(scratchbird::odbc::SQL_DATE_STRUCT));
        case SQL_C_TIME:
            return static_cast<SQLLEN>(sizeof(scratchbird::odbc::SQL_TIME_STRUCT));
        case SQL_C_TIMESTAMP:
            return static_cast<SQLLEN>(sizeof(scratchbird::odbc::SQL_TIMESTAMP_STRUCT));
        case SQL_C_GUID:
            return static_cast<SQLLEN>(sizeof(scratchbird::odbc::SQLGUID));
        default:
            return -1;
    }
}

static bool array_binding_supported_c_type(SQLSMALLINT c_type) {
    return c_type == SQL_C_CHAR ||
           c_type == SQL_C_BINARY ||
           fixed_size_for_c_type(c_type) > 0;
}

static SQLSMALLINT default_sql_type_for_c_type(SQLSMALLINT c_type) {
    switch (c_type) {
        case SQL_C_LONG:
        case SQL_C_SLONG:
            return SQL_INTEGER;
        case SQL_C_SHORT:
        case SQL_C_SSHORT:
            return SQL_SMALLINT;
        case SQL_C_SBIGINT:
            return SQL_BIGINT;
        case SQL_C_DOUBLE:
            return SQL_DOUBLE;
        case SQL_C_FLOAT:
            return SQL_REAL;
        case SQL_C_BIT:
            return SQL_BIT;
        case SQL_C_BINARY:
            return SQL_VARBINARY;
        case SQL_C_DATE:
            return SQL_TYPE_DATE;
        case SQL_C_TIME:
            return SQL_TYPE_TIME;
        case SQL_C_TIMESTAMP:
            return SQL_TYPE_TIMESTAMP;
        case SQL_C_GUID:
            return SQL_GUID;
        case SQL_C_CHAR:
        default:
            return SQL_VARCHAR;
    }
}

static bool sqlstate_allows_retry(const std::string& sqlstate,
                                  const sb_odbc_retry_config& cfg) {
    if (sqlstate == "40001" || sqlstate == "40P01") {
        return cfg.retry_on_deadlock == SQL_TRUE;
    }
    return cfg.retry_on_connection_lost == SQL_TRUE &&
           sqlstate.size() == 5 && sqlstate[0] == '0' && sqlstate[1] == '8';
}

} // namespace

SQLRETURN sb_odbc_stmt_cache_init(SQLHDBC hdbc, const sb_odbc_stmt_cache_config* config) {
    if (!hdbc) {
        return SQL_INVALID_HANDLE;
    }
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    CacheState& state = g_caches[hdbc];
    if (config) {
        state.config = *config;
    } else {
        state.config = sb_odbc_stmt_cache_config_default();
    }
    return SQL_SUCCESS;
}

SQLRETURN sb_odbc_stmt_cache_get(
    SQLHDBC hdbc,
    SQLCHAR* sql_text,
    SQLINTEGER sql_len,
    SQLHSTMT* hstmt,
    SQLSMALLINT* cache_hit
) {
    if (!hdbc || !sql_text || !hstmt) {
        return SQL_INVALID_HANDLE;
    }
    OdbcConnection* conn = static_cast<OdbcConnection*>(hdbc);
    const std::string sql(reinterpret_cast<const char*>(sql_text), sql_len > 0 ? sql_len : std::strlen(reinterpret_cast<const char*>(sql_text)));

    CacheState* state = get_state(hdbc);
    if (!state || !state->config.enable_caching) {
        OdbcStatement* stmt = conn->createStatement();
        if (!stmt) {
            return SQL_ERROR;
        }
        SQLRETURN rc = stmt->prepare(reinterpret_cast<const SQLCHAR*>(sql.c_str()), static_cast<SQLINTEGER>(sql.size()));
        if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
            conn->removeStatement(stmt);
            return rc;
        }
        *hstmt = static_cast<SQLHSTMT>(stmt);
        if (cache_hit) {
            *cache_hit = SQL_FALSE;
        }
        return SQL_SUCCESS;
    }

    purge_expired(*state, conn);

    auto it = state->entries.find(sql);
    if (it != state->entries.end() && !it->second.in_use && it->second.stmt) {
        it->second.in_use = true;
        it->second.last_used = std::chrono::steady_clock::now();
        touch_lru(*state, sql);
        state->hits++;
        *hstmt = static_cast<SQLHSTMT>(it->second.stmt);
        if (cache_hit) {
            *cache_hit = SQL_TRUE;
        }
        return SQL_SUCCESS;
    }

    // Cache miss
    state->misses++;
    OdbcStatement* stmt = conn->createStatement();
    if (!stmt) {
        return SQL_ERROR;
    }
    SQLRETURN rc = stmt->prepare(reinterpret_cast<const SQLCHAR*>(sql.c_str()), static_cast<SQLINTEGER>(sql.size()));
    if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
        conn->removeStatement(stmt);
        return rc;
    }
    *hstmt = static_cast<SQLHSTMT>(stmt);
    if (cache_hit) {
        *cache_hit = SQL_FALSE;
    }
    return SQL_SUCCESS;
}

SQLRETURN sb_odbc_stmt_cache_put(SQLHDBC hdbc, SQLHSTMT hstmt, SQLCHAR* sql_text, SQLINTEGER sql_len) {
    if (!hdbc || !hstmt || !sql_text) {
        return SQL_INVALID_HANDLE;
    }
    OdbcConnection* conn = static_cast<OdbcConnection*>(hdbc);
    CacheState* state = get_state(hdbc);
    const std::string sql(reinterpret_cast<const char*>(sql_text), sql_len > 0 ? sql_len : std::strlen(reinterpret_cast<const char*>(sql_text)));

    if (!state || !state->config.enable_caching) {
        return SQL_SUCCESS;
    }

    CacheEntry entry;
    entry.stmt = static_cast<OdbcStatement*>(hstmt);
    entry.sql = sql;
    entry.last_used = std::chrono::steady_clock::now();
    entry.in_use = false;

    state->entries[sql] = entry;
    touch_lru(*state, sql);
    evict_if_needed(*state, conn);
    return SQL_SUCCESS;
}

SQLRETURN sb_odbc_stmt_cache_invalidate(SQLHDBC hdbc, SQLCHAR* sql_text, SQLINTEGER sql_len) {
    if (!hdbc || !sql_text) {
        return SQL_INVALID_HANDLE;
    }
    OdbcConnection* conn = static_cast<OdbcConnection*>(hdbc);
    CacheState* state = get_state(hdbc);
    if (!state) {
        return SQL_SUCCESS;
    }
    const std::string sql(reinterpret_cast<const char*>(sql_text), sql_len > 0 ? sql_len : std::strlen(reinterpret_cast<const char*>(sql_text)));
    auto it = state->entries.find(sql);
    if (it != state->entries.end()) {
        if (it->second.stmt) {
            it->second.stmt->freeStmt(SQL_CLOSE);
            conn->removeStatement(it->second.stmt);
        }
        state->lru.remove(sql);
        state->entries.erase(it);
    }
    return SQL_SUCCESS;
}

SQLRETURN sb_odbc_stmt_cache_clear(SQLHDBC hdbc) {
    if (!hdbc) {
        return SQL_INVALID_HANDLE;
    }
    OdbcConnection* conn = static_cast<OdbcConnection*>(hdbc);
    CacheState* state = get_state(hdbc);
    if (!state) {
        return SQL_SUCCESS;
    }
    for (auto& kv : state->entries) {
        if (kv.second.stmt) {
            kv.second.stmt->freeStmt(SQL_CLOSE);
            conn->removeStatement(kv.second.stmt);
        }
    }
    state->entries.clear();
    state->lru.clear();
    return SQL_SUCCESS;
}

SQLRETURN sb_odbc_stmt_cache_stats(SQLHDBC hdbc, SQLULEN* cached_count, SQLULEN* hit_count, SQLULEN* miss_count) {
    if (!hdbc) {
        return SQL_INVALID_HANDLE;
    }
    CacheState* state = get_state(hdbc);
    if (!state) {
        if (cached_count) {
            *cached_count = 0;
        }
        if (hit_count) {
            *hit_count = 0;
        }
        if (miss_count) {
            *miss_count = 0;
        }
        return SQL_SUCCESS;
    }
    if (cached_count) {
        *cached_count = static_cast<SQLULEN>(state->entries.size());
    }
    if (hit_count) {
        *hit_count = state->hits;
    }
    if (miss_count) {
        *miss_count = state->misses;
    }
    return SQL_SUCCESS;
}

SQLRETURN sb_odbc_stmt_cache_destroy(SQLHDBC hdbc) {
    if (!hdbc) {
        return SQL_INVALID_HANDLE;
    }
    sb_odbc_stmt_cache_clear(hdbc);
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    g_caches.erase(hdbc);
    return SQL_SUCCESS;
}

SQLRETURN sb_odbc_batch_execute(
    SQLHSTMT hstmt,
    const sb_odbc_batch_op* operations,
    SQLULEN operation_count,
    SQLULEN* rows_affected,
    SQLULEN* error_index
) {
    if (!hstmt || !operations || operation_count == 0) {
        return SQL_INVALID_HANDLE;
    }
    auto* stmt = static_cast<OdbcStatement*>(hstmt);
    SQLULEN total_rows = 0;
    bool info_seen = false;
    for (SQLULEN i = 0; i < operation_count; ++i) {
        const auto& op = operations[i];
        if (!op.sql) {
            if (error_index) {
                *error_index = i;
            }
            return SQL_INVALID_HANDLE;
        }

        SQLRETURN rc = SQL_ERROR;
        if (op.param_count > 0) {
            rc = stmt->freeStmt(SQL_RESET_PARAMS);
            if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
                if (error_index) {
                    *error_index = i;
                }
                return rc;
            }

            rc = stmt->prepare(op.sql, op.sql_len);
            if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
                if (error_index) {
                    *error_index = i;
                }
                return rc;
            }

            std::vector<SQLLEN> indicators(static_cast<size_t>(op.param_count), SQL_NTS);
            for (SQLSMALLINT p = 0; p < op.param_count; ++p) {
                SQLPOINTER param_value = op.params ? op.params[p] : nullptr;
                SQLLEN indicator = SQL_NTS;
                if (op.param_lens) {
                    indicator = op.param_lens[p];
                } else if (!param_value) {
                    indicator = SQL_NULL_DATA;
                }
                indicators[static_cast<size_t>(p)] = indicator;

                SQLSMALLINT value_type = SQL_C_CHAR;
                SQLSMALLINT parameter_type = SQL_VARCHAR;
                SQLULEN column_size = 0;
                SQLSMALLINT decimal_digits = 0;
                if (op.param_c_types) {
                    value_type = op.param_c_types[p];
                }
                if (op.param_sql_types) {
                    parameter_type = op.param_sql_types[p];
                }
                if (op.param_column_sizes) {
                    column_size = op.param_column_sizes[p];
                } else if (value_type == SQL_C_CHAR && indicator > 0) {
                    column_size = static_cast<SQLULEN>(indicator);
                }
                if (op.param_decimal_digits) {
                    decimal_digits = op.param_decimal_digits[p];
                }

                SQLLEN buffer_length = 0;
                if (indicator > 0) {
                    buffer_length = indicator;
                }

                rc = stmt->bindParameter(
                    static_cast<SQLUSMALLINT>(p + 1),
                    SQL_PARAM_INPUT,
                    value_type,
                    parameter_type,
                    column_size,
                    decimal_digits,
                    param_value,
                    buffer_length,
                    &indicators[static_cast<size_t>(p)]);
                if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
                    if (error_index) {
                        *error_index = i;
                    }
                    return rc;
                }
            }

            rc = stmt->execute();
        } else {
            rc = stmt->execDirect(op.sql, op.sql_len);
        }

        if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO && rc != SQL_NO_DATA) {
            if (error_index) {
                *error_index = i;
            }
            if (rows_affected) {
                *rows_affected = total_rows;
            }
            return rc;
        }
        if (rc == SQL_SUCCESS_WITH_INFO) {
            info_seen = true;
        }

        SQLLEN row_count = stmt->getRowCount();
        if (row_count > 0) {
            total_rows += static_cast<SQLULEN>(row_count);
        } else {
            total_rows += 1;
        }
    }
    if (rows_affected) {
        *rows_affected = total_rows;
    }
    if (error_index) {
        *error_index = 0;
    }
    return info_seen ? SQL_SUCCESS_WITH_INFO : SQL_SUCCESS;
}

SQLRETURN sb_odbc_bulk_insert(
    SQLHSTMT hstmt,
    SQLCHAR* table_name,
    SQLCHAR** columns,
    SQLSMALLINT column_count,
    SQLPOINTER* data,
    SQLLEN* data_lens,
    SQLULEN row_count,
    SQLULEN* rows_inserted
) {
    return sb_odbc_bulk_insert_ex(
        hstmt, table_name, columns, column_count, data, data_lens,
        nullptr, nullptr, nullptr, nullptr, row_count, rows_inserted);
}

SQLRETURN sb_odbc_bulk_insert_ex(
    SQLHSTMT hstmt,
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
) {
    if (!hstmt || !table_name || !columns || column_count <= 0 || !data) {
        return SQL_INVALID_HANDLE;
    }
    if (rows_inserted) {
        *rows_inserted = 0;
    }
    if (row_count == 0) {
        return SQL_SUCCESS;
    }

    std::string sql = "INSERT INTO ";
    sql += reinterpret_cast<const char*>(table_name);
    sql += " (";
    for (SQLSMALLINT col = 0; col < column_count; ++col) {
        if (!columns[col]) {
            return SQL_INVALID_HANDLE;
        }
        if (col > 0) {
            sql += ", ";
        }
        sql += reinterpret_cast<const char*>(columns[col]);
    }
    sql += ") VALUES (";
    for (SQLSMALLINT col = 0; col < column_count; ++col) {
        if (col > 0) {
            sql += ", ";
        }
        sql += "?";
    }
    sql += ")";

    auto execute_via_batch = [&]() -> SQLRETURN {
        std::vector<sb_odbc_batch_op> operations(static_cast<size_t>(row_count));
        std::vector<std::vector<SQLPOINTER>> params(static_cast<size_t>(row_count));
        std::vector<std::vector<SQLLEN>> lens(static_cast<size_t>(row_count));

        for (SQLULEN row = 0; row < row_count; ++row) {
            auto& row_params = params[static_cast<size_t>(row)];
            auto& row_lens = lens[static_cast<size_t>(row)];
            row_params.resize(static_cast<size_t>(column_count), nullptr);
            row_lens.resize(static_cast<size_t>(column_count), SQL_NULL_DATA);

            for (SQLSMALLINT col = 0; col < column_count; ++col) {
                auto** col_values = reinterpret_cast<SQLPOINTER*>(data[col]);
                if (!col_values) {
                    row_params[static_cast<size_t>(col)] = nullptr;
                    row_lens[static_cast<size_t>(col)] = SQL_NULL_DATA;
                    continue;
                }
                SQLPOINTER value = col_values[row];
                row_params[static_cast<size_t>(col)] = value;

                SQLLEN len = SQL_NTS;
                if (data_lens) {
                    size_t idx = static_cast<size_t>(col) * static_cast<size_t>(row_count) +
                                 static_cast<size_t>(row);
                    len = data_lens[idx];
                } else if (!value) {
                    len = SQL_NULL_DATA;
                }
                row_lens[static_cast<size_t>(col)] = len;
            }

            operations[static_cast<size_t>(row)].sql = reinterpret_cast<SQLCHAR*>(
                const_cast<char*>(sql.c_str()));
            operations[static_cast<size_t>(row)].sql_len = SQL_NTS;
            operations[static_cast<size_t>(row)].params = row_params.data();
            operations[static_cast<size_t>(row)].param_lens = row_lens.data();
            operations[static_cast<size_t>(row)].param_count = column_count;
            operations[static_cast<size_t>(row)].param_c_types = param_c_types;
            operations[static_cast<size_t>(row)].param_sql_types = param_sql_types;
            operations[static_cast<size_t>(row)].param_column_sizes = param_column_sizes;
            operations[static_cast<size_t>(row)].param_decimal_digits = param_decimal_digits;
        }

        SQLULEN error_index = 0;
        SQLRETURN rc = sb_odbc_batch_execute(
            hstmt,
            operations.data(),
            row_count,
            rows_inserted,
            &error_index);
        if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO && rows_inserted) {
            *rows_inserted = error_index;
        }
        return rc;
    };

    struct ColumnArrayStorage {
        SQLSMALLINT c_type{SQL_C_CHAR};
        SQLSMALLINT sql_type{SQL_VARCHAR};
        SQLULEN column_size{0};
        SQLSMALLINT decimal_digits{0};
        SQLLEN element_size{1};
        std::vector<unsigned char> values;
        std::vector<SQLLEN> indicators;
    };

    auto* stmt = static_cast<OdbcStatement*>(hstmt);
    std::vector<ColumnArrayStorage> arrays(static_cast<size_t>(column_count));
    bool truncated = false;

    for (SQLSMALLINT col = 0; col < column_count; ++col) {
        auto& column = arrays[static_cast<size_t>(col)];
        column.c_type = param_c_types ? param_c_types[col] : SQL_C_CHAR;
        if (!array_binding_supported_c_type(column.c_type)) {
            return execute_via_batch();
        }
        column.sql_type = param_sql_types ? param_sql_types[col]
                                          : default_sql_type_for_c_type(column.c_type);
        column.column_size = param_column_sizes ? param_column_sizes[col] : 0;
        column.decimal_digits = param_decimal_digits ? param_decimal_digits[col] : 0;
        column.indicators.assign(static_cast<size_t>(row_count), SQL_NULL_DATA);

        auto** row_values = reinterpret_cast<SQLPOINTER*>(data[col]);
        if (!row_values) {
            return SQL_INVALID_HANDLE;
        }

        if (column.c_type == SQL_C_CHAR) {
            size_t max_len = 1;
            for (SQLULEN row = 0; row < row_count; ++row) {
                SQLPOINTER value = row_values[row];
                SQLLEN len = SQL_NTS;
                if (data_lens) {
                    size_t idx = static_cast<size_t>(col) * static_cast<size_t>(row_count) +
                                 static_cast<size_t>(row);
                    len = data_lens[idx];
                } else if (!value) {
                    len = SQL_NULL_DATA;
                }
                if (!value || len == SQL_NULL_DATA) {
                    continue;
                }
                if (len == SQL_NTS) {
                    len = static_cast<SQLLEN>(
                        std::strlen(static_cast<const char*>(value)));
                }
                if (len < 0) {
                    return execute_via_batch();
                }
                max_len = std::max(max_len, static_cast<size_t>(len) + 1);
            }
            column.element_size = static_cast<SQLLEN>(max_len);
            if (column.column_size == 0 && max_len > 0) {
                column.column_size = static_cast<SQLULEN>(max_len - 1);
            }
            column.values.assign(static_cast<size_t>(row_count) * max_len, 0);

            for (SQLULEN row = 0; row < row_count; ++row) {
                SQLPOINTER value = row_values[row];
                SQLLEN len = SQL_NTS;
                if (data_lens) {
                    size_t idx = static_cast<size_t>(col) * static_cast<size_t>(row_count) +
                                 static_cast<size_t>(row);
                    len = data_lens[idx];
                } else if (!value) {
                    len = SQL_NULL_DATA;
                }
                if (!value || len == SQL_NULL_DATA) {
                    column.indicators[static_cast<size_t>(row)] = SQL_NULL_DATA;
                    continue;
                }
                if (len == SQL_NTS) {
                    len = static_cast<SQLLEN>(
                        std::strlen(static_cast<const char*>(value)));
                }
                if (len < 0) {
                    return execute_via_batch();
                }

                size_t offset = static_cast<size_t>(row) * max_len;
                size_t copy_len = std::min(static_cast<size_t>(len), max_len - 1);
                if (static_cast<size_t>(len) > copy_len) {
                    truncated = true;
                }
                std::memcpy(column.values.data() + offset, value, copy_len);
                column.values[offset + copy_len] = '\0';
                column.indicators[static_cast<size_t>(row)] = static_cast<SQLLEN>(copy_len);
            }
        } else if (column.c_type == SQL_C_BINARY) {
            size_t max_len = 1;
            for (SQLULEN row = 0; row < row_count; ++row) {
                SQLPOINTER value = row_values[row];
                SQLLEN len = SQL_NULL_DATA;
                if (data_lens) {
                    size_t idx = static_cast<size_t>(col) * static_cast<size_t>(row_count) +
                                 static_cast<size_t>(row);
                    len = data_lens[idx];
                } else if (value && column.column_size > 0) {
                    len = static_cast<SQLLEN>(column.column_size);
                }
                if (!value || len == SQL_NULL_DATA) {
                    continue;
                }
                if (len < 0) {
                    return execute_via_batch();
                }
                max_len = std::max(max_len, static_cast<size_t>(len));
            }
            column.element_size = static_cast<SQLLEN>(max_len);
            if (column.column_size == 0) {
                column.column_size = static_cast<SQLULEN>(max_len);
            }
            column.values.assign(static_cast<size_t>(row_count) * max_len, 0);

            for (SQLULEN row = 0; row < row_count; ++row) {
                SQLPOINTER value = row_values[row];
                SQLLEN len = SQL_NULL_DATA;
                if (data_lens) {
                    size_t idx = static_cast<size_t>(col) * static_cast<size_t>(row_count) +
                                 static_cast<size_t>(row);
                    len = data_lens[idx];
                } else if (value && column.column_size > 0) {
                    len = static_cast<SQLLEN>(column.column_size);
                }
                if (!value || len == SQL_NULL_DATA) {
                    column.indicators[static_cast<size_t>(row)] = SQL_NULL_DATA;
                    continue;
                }
                if (len < 0) {
                    return execute_via_batch();
                }

                size_t offset = static_cast<size_t>(row) * max_len;
                size_t copy_len = std::min(static_cast<size_t>(len), max_len);
                if (static_cast<size_t>(len) > copy_len) {
                    truncated = true;
                }
                std::memcpy(column.values.data() + offset, value, copy_len);
                column.indicators[static_cast<size_t>(row)] = static_cast<SQLLEN>(copy_len);
            }
        } else {
            SQLLEN fixed_size = fixed_size_for_c_type(column.c_type);
            if (fixed_size <= 0) {
                return execute_via_batch();
            }
            column.element_size = fixed_size;
            if (column.column_size == 0) {
                column.column_size = static_cast<SQLULEN>(fixed_size);
            }
            size_t stride = static_cast<size_t>(fixed_size);
            column.values.assign(static_cast<size_t>(row_count) * stride, 0);

            for (SQLULEN row = 0; row < row_count; ++row) {
                SQLPOINTER value = row_values[row];
                SQLLEN len = 0;
                if (data_lens) {
                    size_t idx = static_cast<size_t>(col) * static_cast<size_t>(row_count) +
                                 static_cast<size_t>(row);
                    len = data_lens[idx];
                } else if (!value) {
                    len = SQL_NULL_DATA;
                }
                if (!value || len == SQL_NULL_DATA) {
                    column.indicators[static_cast<size_t>(row)] = SQL_NULL_DATA;
                    continue;
                }

                size_t offset = static_cast<size_t>(row) * stride;
                size_t copy_len = stride;
                if (len > 0) {
                    copy_len = std::min(copy_len, static_cast<size_t>(len));
                    if (static_cast<size_t>(len) > copy_len) {
                        truncated = true;
                    }
                }
                std::memcpy(column.values.data() + offset, value, copy_len);
                column.indicators[static_cast<size_t>(row)] = 0;
            }
        }
    }

    SQLRETURN rc = stmt->freeStmt(SQL_RESET_PARAMS);
    if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
        return rc;
    }

    rc = stmt->prepare(reinterpret_cast<const SQLCHAR*>(sql.c_str()), SQL_NTS);
    if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
        return rc;
    }

    SQLULEN processed = 0;
    std::vector<SQLUSMALLINT> param_status(static_cast<size_t>(row_count), 0);
    SQLLEN zero_bind_offset = 0;

    auto cleanup = [&]() {
        (void)stmt->setAttribute(SQL_ATTR_PARAMSET_SIZE,
                                 reinterpret_cast<SQLPOINTER>(static_cast<SQLULEN>(1)), 0);
        (void)stmt->setAttribute(SQL_ATTR_PARAM_BIND_TYPE,
                                 reinterpret_cast<SQLPOINTER>(static_cast<SQLULEN>(SQL_PARAM_BIND_BY_COLUMN)),
                                 0);
        (void)stmt->setAttribute(SQL_ATTR_PARAM_BIND_OFFSET_PTR, &zero_bind_offset, 0);
        (void)stmt->setAttribute(SQL_ATTR_PARAMS_PROCESSED_PTR, nullptr, 0);
        (void)stmt->setAttribute(SQL_ATTR_PARAM_STATUS_PTR, nullptr, 0);
        (void)stmt->freeStmt(SQL_RESET_PARAMS);
    };

    rc = stmt->setAttribute(SQL_ATTR_PARAM_BIND_TYPE,
                            reinterpret_cast<SQLPOINTER>(static_cast<SQLULEN>(SQL_PARAM_BIND_BY_COLUMN)),
                            0);
    if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
        cleanup();
        return rc;
    }
    rc = stmt->setAttribute(SQL_ATTR_PARAM_BIND_OFFSET_PTR, &zero_bind_offset, 0);
    if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
        cleanup();
        return rc;
    }
    rc = stmt->setAttribute(SQL_ATTR_PARAMSET_SIZE,
                            reinterpret_cast<SQLPOINTER>(row_count), 0);
    if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
        cleanup();
        return rc;
    }
    rc = stmt->setAttribute(SQL_ATTR_PARAMS_PROCESSED_PTR, &processed, 0);
    if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
        cleanup();
        return rc;
    }
    rc = stmt->setAttribute(SQL_ATTR_PARAM_STATUS_PTR, param_status.data(), 0);
    if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
        cleanup();
        return rc;
    }

    for (SQLSMALLINT col = 0; col < column_count; ++col) {
        auto& column = arrays[static_cast<size_t>(col)];
        rc = stmt->bindParameter(
            static_cast<SQLUSMALLINT>(col + 1),
            SQL_PARAM_INPUT,
            column.c_type,
            column.sql_type,
            column.column_size,
            column.decimal_digits,
            column.values.empty() ? nullptr : column.values.data(),
            column.element_size,
            column.indicators.data());
        if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
            cleanup();
            return rc;
        }
    }

    rc = stmt->bulkOperations(SQL_ADD);
    SQLULEN effective_processed = processed;
    if (effective_processed == 0 &&
        (rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)) {
        SQLLEN count = stmt->getRowCount();
        if (count > 0) {
            effective_processed = static_cast<SQLULEN>(count);
        } else {
            effective_processed = row_count;
        }
    }
    cleanup();

    if (rows_inserted) {
        *rows_inserted = effective_processed;
    }
    if (truncated && rc == SQL_SUCCESS) {
        return SQL_SUCCESS_WITH_INFO;
    }
    return rc;
}

SQLRETURN sb_odbc_with_retry(
    SQLHDBC hdbc,
    const sb_odbc_retry_config* config,
    SQLRETURN (*operation)(void* user_data),
    void* user_data,
    SQLULEN* attempt_count
) {
    if (!hdbc || !operation) {
        return SQL_INVALID_HANDLE;
    }
    sb_odbc_retry_config cfg = config ? *config : sb_odbc_retry_config_default();
    SQLULEN attempts = 0;
    SQLRETURN rc = SQL_ERROR;
    DWORD delay = static_cast<DWORD>(cfg.base_delay_ms);

    for (; attempts <= cfg.max_retries; ++attempts) {
        rc = operation(user_data);
        if (rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO) {
            break;
        }
        auto* conn = static_cast<OdbcConnection*>(hdbc);
        const auto* diag = conn ? conn->getDiagnostic(1) : nullptr;
        if (!diag || !sqlstate_allows_retry(diag->sqlstate, cfg)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        delay = std::min<DWORD>(delay * 2, static_cast<DWORD>(cfg.max_delay_ms));
    }

    if (attempt_count) {
        *attempt_count = attempts;
    }
    return rc;
}

SQLRETURN sb_odbc_connection_is_healthy(SQLHDBC hdbc, SQLSMALLINT* is_healthy) {
    if (!hdbc || !is_healthy) {
        return SQL_INVALID_HANDLE;
    }
    auto* conn = static_cast<OdbcConnection*>(hdbc);
    *is_healthy = conn->isConnected() ? SQL_TRUE : SQL_FALSE;
    return SQL_SUCCESS;
}

SQLRETURN sb_odbc_connection_validate(SQLHDBC hdbc, SQLULEN timeout_ms, SQLSMALLINT* is_valid) {
    (void)timeout_ms;
    if (!hdbc || !is_valid) {
        return SQL_INVALID_HANDLE;
    }
    auto* conn = static_cast<OdbcConnection*>(hdbc);
    if (!conn->isConnected()) {
        *is_valid = SQL_FALSE;
        return SQL_SUCCESS;
    }
    std::vector<std::vector<std::string>> rows;
    std::vector<ColumnMetadata> columns;
    SQLLEN rows_affected = 0;
    SQLRETURN rc = conn->executeSQL("SELECT 1", rows, columns, rows_affected);
    *is_valid = (rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO) ? SQL_TRUE : SQL_FALSE;
    return SQL_SUCCESS;
}
