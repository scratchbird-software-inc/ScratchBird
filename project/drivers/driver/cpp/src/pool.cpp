// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

/**
 * @file pool.cpp
 * @brief Connection pooling, retry helpers, and connection health APIs.
 */

#include "scratchbird/client/pool.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

struct PooledConnection {
    sb_connection* conn{nullptr};
    std::chrono::steady_clock::time_point created_at{};
    std::chrono::steady_clock::time_point last_used_at{};
    bool in_use{false};
};

struct sb_connection_pool {
    std::string conn_str;
    sb_pool_config config{};
    std::vector<PooledConnection> connections;
    std::mutex mutex;
};

namespace {

struct ConnTracking {
    std::chrono::steady_clock::time_point created_at{};
    std::chrono::steady_clock::time_point last_used_at{};
};

std::mutex g_tracking_mutex;
std::unordered_map<sb_connection*, ConnTracking> g_tracking;

void set_error(sb_error* err, sb_error_code code, const std::string& message) {
    if (!err) {
        return;
    }
    err->code = code;
    std::snprintf(err->message, sizeof(err->message), "%s", message.c_str());
}

void ensure_tracking(sb_connection* conn) {
    if (!conn) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(g_tracking_mutex);
    auto it = g_tracking.find(conn);
    if (it == g_tracking.end()) {
        g_tracking.emplace(conn, ConnTracking{now, now});
        return;
    }
    it->second.last_used_at = now;
}

void register_connection(sb_connection* conn) {
    if (!conn) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(g_tracking_mutex);
    g_tracking[conn] = ConnTracking{now, now};
}

void unregister_connection(sb_connection* conn) {
    if (!conn) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_tracking_mutex);
    g_tracking.erase(conn);
}

bool should_retry(sb_error_code code, const sb_retry_config& config) {
    const bool is_connection_error =
        code == SB_ERR_CONNECTION_FAILED ||
        code == SB_ERR_DISCONNECTED ||
        code == SB_ERR_TIMEOUT ||
        code == SB_ERR_PROTOCOL;
    const bool is_txn_error =
        code == SB_ERR_TXN_CONFLICT ||
        code == SB_ERR_DEADLOCK ||
        code == SB_ERR_SERIALIZATION;

    if (is_connection_error && config.retry_connection_errors) {
        return true;
    }
    if (is_txn_error && config.retry_transaction_errors) {
        return true;
    }
    return false;
}

uint32_t retry_delay_ms(const sb_retry_config& config, uint32_t attempt) {
    const uint32_t base = config.base_delay_ms == 0 ? 1u : config.base_delay_ms;
    const uint32_t max_delay = config.max_delay_ms == 0 ? base : config.max_delay_ms;
    uint64_t delay = static_cast<uint64_t>(base) << std::min<uint32_t>(attempt, 16u);
    if (delay > max_delay) {
        delay = max_delay;
    }
    return static_cast<uint32_t>(delay);
}

void copy_error(sb_error* dst, const sb_error& src) {
    if (!dst) {
        return;
    }
    *dst = src;
}

int64_t count_rows(sb_result* result) {
    if (!result) {
        return 0;
    }
    int64_t rows = 0;
    sb_row row{};
    sb_error err{};
    while (sb_fetch(result, &row, &err) == SB_OK) {
        ++rows;
    }
    return rows;
}

std::string trim_ascii(const std::string& value) {
    size_t start = 0;
    while (start < value.size() &&
           std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        ++start;
    }
    size_t end = value.size();
    while (end > start &&
           std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return value.substr(start, end - start);
}

std::string quote_identifier_component(const std::string& component) {
    std::string out;
    out.reserve(component.size() + 2);
    out.push_back('"');
    for (char ch : component) {
        if (ch == '"') {
            out.push_back('"');
        }
        out.push_back(ch);
    }
    out.push_back('"');
    return out;
}

bool append_quoted_identifier_path(const std::string& identifier,
                                   std::string* out,
                                   std::string* error) {
    if (!out) {
        return false;
    }
    std::stringstream ss(identifier);
    std::string token;
    bool first = true;
    while (std::getline(ss, token, '.')) {
        token = trim_ascii(token);
        if (token.empty()) {
            if (error) {
                *error = "Identifier contains empty path segment";
            }
            return false;
        }
        if (!first) {
            out->push_back('.');
        }
        out->append(quote_identifier_component(token));
        first = false;
    }
    if (first) {
        if (error) {
            *error = "Identifier is empty";
        }
        return false;
    }
    return true;
}

bool build_bulk_insert_sql(const char* table_name,
                           const char** columns,
                           size_t column_count,
                           std::string* sql_out,
                           std::string* error) {
    if (!table_name || !sql_out) {
        if (error) {
            *error = "table_name is required";
        }
        return false;
    }
    const std::string table = trim_ascii(table_name);
    if (table.empty()) {
        if (error) {
            *error = "table_name is required";
        }
        return false;
    }
    if (!columns || column_count == 0) {
        if (error) {
            *error = "At least one column is required";
        }
        return false;
    }

    std::string quoted_table;
    if (!append_quoted_identifier_path(table, &quoted_table, error)) {
        return false;
    }

    std::vector<std::string> quoted_columns;
    quoted_columns.reserve(column_count);
    for (size_t i = 0; i < column_count; ++i) {
        if (!columns[i]) {
            if (error) {
                *error = "Column name is required";
            }
            return false;
        }
        const std::string column = trim_ascii(columns[i]);
        if (column.empty()) {
            if (error) {
                *error = "Column name is required";
            }
            return false;
        }
        std::string quoted_column;
        if (!append_quoted_identifier_path(column, &quoted_column, error)) {
            return false;
        }
        quoted_columns.push_back(std::move(quoted_column));
    }

    std::stringstream sql;
    sql << "INSERT INTO " << quoted_table << " (";
    for (size_t i = 0; i < quoted_columns.size(); ++i) {
        if (i > 0) {
            sql << ", ";
        }
        sql << quoted_columns[i];
    }
    sql << ") VALUES (";
    for (size_t i = 0; i < column_count; ++i) {
        if (i > 0) {
            sql << ", ";
        }
        sql << "$" << (i + 1);
    }
    sql << ")";

    *sql_out = sql.str();
    return true;
}

bool build_bulk_copy_sql(const char* table_name,
                         const char** columns,
                         size_t column_count,
                         std::string* sql_out,
                         std::string* error) {
    std::string insert_sql;
    if (!build_bulk_insert_sql(table_name, columns, column_count, &insert_sql, error)) {
        return false;
    }
    const std::string prefix = "INSERT INTO ";
    const auto values_pos = insert_sql.find(" VALUES ");
    if (insert_sql.rfind(prefix, 0) != 0 || values_pos == std::string::npos) {
        if (error) {
            *error = "Bulk insert SQL could not be converted to COPY";
        }
        return false;
    }
    *sql_out = "COPY " + insert_sql.substr(prefix.size(), values_pos - prefix.size()) +
               " FROM STDIN";
    return true;
}

bool append_copy_value(const sb_value& value, std::string* out, std::string* error) {
    if (!out) {
        return false;
    }
    if (value.is_null || value.type == SB_TYPE_NULL) {
        out->append("NULL");
        return true;
    }
    std::ostringstream text;
    switch (value.type) {
        case SB_TYPE_BOOLEAN:
            text << (value.data.boolean_val ? "true" : "false");
            break;
        case SB_TYPE_SMALLINT:
            text << value.data.smallint_val;
            break;
        case SB_TYPE_INTEGER:
            text << value.data.integer_val;
            break;
        case SB_TYPE_BIGINT:
            text << value.data.bigint_val;
            break;
        case SB_TYPE_REAL:
            text << std::setprecision(9) << value.data.real_val;
            break;
        case SB_TYPE_DOUBLE:
            text << std::setprecision(17) << value.data.double_val;
            break;
        case SB_TYPE_MONEY:
            text << value.data.money_val;
            break;
        case SB_TYPE_UUID:
            text << std::hex << std::setfill('0');
            for (size_t i = 0; i < sizeof(value.data.uuid_val.bytes); ++i) {
                text << std::setw(2) << static_cast<unsigned>(value.data.uuid_val.bytes[i]);
            }
            break;
        case SB_TYPE_DATE:
            text << value.data.date_val.year << '-'
                 << std::setw(2) << std::setfill('0') << value.data.date_val.month << '-'
                 << std::setw(2) << std::setfill('0') << value.data.date_val.day;
            break;
        case SB_TYPE_TIME:
        case SB_TYPE_TIME_TZ:
            text << std::setw(2) << std::setfill('0') << value.data.time_val.hour << ':'
                 << std::setw(2) << std::setfill('0') << value.data.time_val.minute << ':'
                 << std::setw(2) << std::setfill('0') << value.data.time_val.second;
            if (value.data.time_val.microsecond != 0) {
                text << '.' << std::setw(6) << std::setfill('0')
                     << value.data.time_val.microsecond;
            }
            break;
        case SB_TYPE_TIMESTAMP:
        case SB_TYPE_TIMESTAMP_TZ:
            text << value.data.timestamp_val.epoch_microseconds;
            break;
        case SB_TYPE_INTERVAL:
            text << value.data.interval_val.months << " months "
                 << value.data.interval_val.days << " days "
                 << value.data.interval_val.micros << " micros";
            break;
        case SB_TYPE_BLOB:
            text << "hex:";
            text << std::hex << std::setfill('0');
            for (size_t i = 0; i < value.data.binary_val.length; ++i) {
                text << std::setw(2) << static_cast<unsigned>(value.data.binary_val.data[i]);
            }
            break;
        default:
            if (!value.data.string_val.data && value.data.string_val.length > 0) {
                if (error) {
                    *error = "String bulk value has null data";
                }
                return false;
            }
            text.write(value.data.string_val.data ? value.data.string_val.data : "",
                       static_cast<std::streamsize>(value.data.string_val.length));
            break;
    }
    const std::string rendered = text.str();
    if (rendered.find_first_of(";\r\n") != std::string::npos) {
        if (error) {
            *error = "Bulk COPY canonical value contains a row or field delimiter";
        }
        return false;
    }
    out->append(rendered);
    return true;
}

bool build_bulk_copy_data(const char** columns,
                          size_t column_count,
                          const sb_value** rows,
                          size_t row_count,
                          std::string* data_out,
                          std::string* error) {
    if (!data_out) {
        return false;
    }
    data_out->clear();
    for (size_t r = 0; r < row_count; ++r) {
        const sb_value* row_values = rows[r];
        if (!row_values) {
            if (error) {
                *error = "Bulk insert row values cannot be null";
            }
            return false;
        }
        for (size_t c = 0; c < column_count; ++c) {
            if (c != 0) {
                data_out->push_back(';');
            }
            if (!columns[c] || columns[c][0] == '\0') {
                if (error) {
                    *error = "Column name is required";
                }
                return false;
            }
            data_out->append(columns[c]);
            data_out->push_back('=');
            if (!append_copy_value(row_values[c], data_out, error)) {
                return false;
            }
        }
        data_out->push_back('\n');
    }
    return true;
}

} // namespace

sb_connection_pool* sb_pool_create(const char* conn_str,
                                   const sb_pool_config* config,
                                   sb_error* err) {
    if (!conn_str || conn_str[0] == '\0') {
        set_error(err, SB_ERR_NULL_POINTER, "Connection string is required");
        return nullptr;
    }

    const sb_pool_config pool_cfg = config ? *config : sb_pool_config_default();
    if (pool_cfg.max_connections == 0 || pool_cfg.min_connections > pool_cfg.max_connections) {
        set_error(err, SB_ERR_INVALID_PARAM, "Invalid pool bounds");
        return nullptr;
    }

    auto* pool = new sb_connection_pool();
    pool->conn_str = conn_str;
    pool->config = pool_cfg;
    pool->connections.reserve(pool_cfg.max_connections);

    for (size_t i = 0; i < pool_cfg.min_connections; ++i) {
        sb_error conn_err{};
        sb_connection* conn = sb_connect(pool->conn_str.c_str(), &conn_err);
        if (!conn) {
            for (auto& entry : pool->connections) {
                if (entry.conn) {
                    unregister_connection(entry.conn);
                    sb_disconnect(entry.conn);
                    entry.conn = nullptr;
                }
            }
            set_error(err, conn_err.code, conn_err.message);
            delete pool;
            return nullptr;
        }
        const auto now = std::chrono::steady_clock::now();
        pool->connections.push_back(PooledConnection{conn, now, now, false});
        register_connection(conn);
    }

    set_error(err, SB_OK, "");
    return pool;
}

void sb_pool_destroy(sb_connection_pool* pool) {
    if (!pool) {
        return;
    }

    std::vector<sb_connection*> to_close;
    {
        std::lock_guard<std::mutex> lock(pool->mutex);
        to_close.reserve(pool->connections.size());
        for (auto& entry : pool->connections) {
            if (entry.conn) {
                to_close.push_back(entry.conn);
                entry.conn = nullptr;
            }
        }
        pool->connections.clear();
    }

    for (auto* conn : to_close) {
        unregister_connection(conn);
        sb_disconnect(conn);
    }
    delete pool;
}

sb_connection* sb_pool_acquire(sb_connection_pool* pool, sb_error* err) {
    if (!pool) {
        set_error(err, SB_ERR_INVALID_HANDLE, "Pool is null");
        return nullptr;
    }

    const auto start = std::chrono::steady_clock::now();
    const auto timeout = std::chrono::seconds(pool->config.acquire_timeout_seconds);

    while (true) {
        PooledConnection* selected = nullptr;
        {
            std::lock_guard<std::mutex> lock(pool->mutex);
            for (auto& entry : pool->connections) {
                if (entry.conn && !entry.in_use) {
                    entry.in_use = true;
                    entry.last_used_at = std::chrono::steady_clock::now();
                    selected = &entry;
                    break;
                }
            }
        }

        if (selected) {
            if (pool->config.test_on_checkout && sb_connection_is_healthy(selected->conn) == 0) {
                unregister_connection(selected->conn);
                sb_disconnect(selected->conn);
                sb_error reconnect_err{};
                selected->conn = sb_connect(pool->conn_str.c_str(), &reconnect_err);
                if (!selected->conn) {
                    selected->in_use = false;
                    set_error(err, reconnect_err.code, reconnect_err.message);
                    return nullptr;
                }
                const auto now = std::chrono::steady_clock::now();
                selected->created_at = now;
                selected->last_used_at = now;
                register_connection(selected->conn);
            }
            ensure_tracking(selected->conn);
            set_error(err, SB_OK, "");
            return selected->conn;
        }

        bool can_create = false;
        {
            std::lock_guard<std::mutex> lock(pool->mutex);
            can_create = pool->connections.size() < pool->config.max_connections;
        }

        if (can_create) {
            sb_error conn_err{};
            sb_connection* conn = sb_connect(pool->conn_str.c_str(), &conn_err);
            if (!conn) {
                set_error(err, conn_err.code, conn_err.message);
                return nullptr;
            }
            const auto now = std::chrono::steady_clock::now();
            {
                std::lock_guard<std::mutex> lock(pool->mutex);
                pool->connections.push_back(PooledConnection{conn, now, now, true});
            }
            register_connection(conn);
            set_error(err, SB_OK, "");
            return conn;
        }

        if (std::chrono::steady_clock::now() - start >= timeout) {
            set_error(err, SB_ERR_TIMEOUT, "Timed out acquiring pooled connection");
            return nullptr;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void sb_pool_release(sb_connection_pool* pool, sb_connection* conn) {
    if (!pool || !conn) {
        return;
    }
    std::lock_guard<std::mutex> lock(pool->mutex);
    for (auto& entry : pool->connections) {
        if (entry.conn == conn) {
            entry.in_use = false;
            entry.last_used_at = std::chrono::steady_clock::now();
            ensure_tracking(conn);
            return;
        }
    }
}

sb_pool_stats sb_pool_get_stats(sb_connection_pool* pool) {
    sb_pool_stats stats{};
    if (!pool) {
        return stats;
    }
    std::lock_guard<std::mutex> lock(pool->mutex);
    stats.max_connections = pool->config.max_connections;
    for (const auto& entry : pool->connections) {
        if (entry.conn) {
            ++stats.total_connections;
            if (!entry.in_use) {
                ++stats.available_connections;
            }
        }
    }
    return stats;
}

sb_error sb_with_retry(const sb_retry_config* config,
                       sb_retryable_func func,
                       void* user_data) {
    const sb_retry_config retry_cfg = config ? *config : sb_retry_config_default();
    if (!func) {
        sb_error err{};
        set_error(&err, SB_ERR_NULL_POINTER, "Retry callback is required");
        return err;
    }

    for (uint32_t attempt = 0;; ++attempt) {
        sb_error current = func(user_data);
        if (current.code == SB_OK) {
            return current;
        }
        if (attempt >= retry_cfg.max_retries || !should_retry(current.code, retry_cfg)) {
            return current;
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(retry_delay_ms(retry_cfg, attempt)));
    }
}

sb_result* sb_query_with_retry(sb_connection* conn,
                               const char* sql,
                               const sb_retry_config* config,
                               sb_error* err) {
    if (!conn || !sql) {
        set_error(err, SB_ERR_NULL_POINTER, "Connection and SQL required");
        return nullptr;
    }

    const sb_retry_config retry_cfg = config ? *config : sb_retry_config_default();
    for (uint32_t attempt = 0;; ++attempt) {
        sb_error local_err{};
        sb_result* result = sb_query(conn, sql, &local_err);
        if (result) {
            copy_error(err, local_err);
            ensure_tracking(conn);
            return result;
        }
        if (attempt >= retry_cfg.max_retries || !should_retry(local_err.code, retry_cfg)) {
            copy_error(err, local_err);
            return nullptr;
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(retry_delay_ms(retry_cfg, attempt)));
    }
}

sb_result* sb_execute_with_retry(sb_connection* conn,
                                 const char* sql,
                                 const sb_retry_config* config,
                                 sb_error* err) {
    if (!conn || !sql) {
        set_error(err, SB_ERR_NULL_POINTER, "Connection and SQL required");
        return nullptr;
    }

    const sb_retry_config retry_cfg = config ? *config : sb_retry_config_default();
    for (uint32_t attempt = 0;; ++attempt) {
        sb_error local_err{};
        sb_result* result = sb_execute(conn, sql, &local_err);
        if (result) {
            copy_error(err, local_err);
            ensure_tracking(conn);
            return result;
        }
        if (attempt >= retry_cfg.max_retries || !should_retry(local_err.code, retry_cfg)) {
            copy_error(err, local_err);
            return nullptr;
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(retry_delay_ms(retry_cfg, attempt)));
    }
}

int64_t* sb_batch_execute(sb_connection* conn,
                          const sb_batch_operation* operations,
                          size_t operation_count,
                          int64_t* rows_affected_out,
                          sb_error* err) {
    if (!conn || (!operations && operation_count > 0)) {
        set_error(err, SB_ERR_NULL_POINTER, "Connection and operations are required");
        return nullptr;
    }
    if (operation_count == 0) {
        if (rows_affected_out) {
            *rows_affected_out = 0;
        }
        set_error(err, SB_OK, "");
        return nullptr;
    }

    auto* rows = static_cast<int64_t*>(std::calloc(operation_count, sizeof(int64_t)));
    if (!rows) {
        set_error(err, SB_ERR_OUT_OF_MEMORY, "Out of memory allocating batch result");
        return nullptr;
    }

    int64_t total_rows = 0;
    for (size_t i = 0; i < operation_count; ++i) {
        const auto& op = operations[i];
        if (!op.sql || trim_ascii(op.sql).empty()) {
            set_error(err, SB_ERR_INVALID_PARAM, "Batch SQL is required");
            std::free(rows);
            return nullptr;
        }

        sb_error op_err{};
        sb_result* result = nullptr;
        if (op.param_count == 0) {
            result = sb_execute(conn, op.sql, &op_err);
        } else {
            if (!op.params) {
                set_error(err, SB_ERR_INVALID_PARAM, "Batch parameters are required when param_count > 0");
                std::free(rows);
                return nullptr;
            }

            sb_prepared* stmt = sb_prepare(conn, op.sql, &op_err);
            if (!stmt) {
                copy_error(err, op_err);
                std::free(rows);
                return nullptr;
            }

            bool bind_ok = true;
            for (size_t p = 0; p < op.param_count; ++p) {
                if (!op.params[p]) {
                    set_error(&op_err, SB_ERR_INVALID_PARAM, "Batch parameter value is null");
                    bind_ok = false;
                    break;
                }
                const int bind_rc = sb_bind_index(stmt, p + 1, op.params[p], &op_err);
                if (bind_rc != SB_OK) {
                    bind_ok = false;
                    break;
                }
            }
            if (!bind_ok) {
                sb_prepared_free(stmt);
                copy_error(err, op_err);
                std::free(rows);
                return nullptr;
            }

            result = sb_execute_prepared(stmt, &op_err);
            sb_prepared_free(stmt);
        }

        if (!result) {
            copy_error(err, op_err);
            std::free(rows);
            return nullptr;
        }

        int64_t affected = sb_rows_affected(result);
        if (affected <= 0) {
            affected = count_rows(result);
        }
        rows[i] = affected;
        total_rows += rows[i];
        sb_result_free(result);
        ensure_tracking(conn);
    }

    if (rows_affected_out) {
        *rows_affected_out = total_rows;
    }
    set_error(err, SB_OK, "");
    return rows;
}

int sb_bulk_insert(sb_connection* conn,
                   const char* table_name,
                   const char** columns,
                   size_t column_count,
                   const sb_value** rows,
                   size_t row_count,
                   int64_t* rows_inserted,
                   sb_error* err) {
    if (!conn) {
        set_error(err, SB_ERR_NULL_POINTER, "Connection is required");
        return SB_ERR_NULL_POINTER;
    }
    if ((row_count > 0 && !rows) || !table_name || !columns || column_count == 0) {
        set_error(err, SB_ERR_INVALID_PARAM, "Bulk insert requires table, columns, and rows");
        return SB_ERR_INVALID_PARAM;
    }

    if (rows_inserted) {
        *rows_inserted = 0;
    }
    if (row_count == 0) {
        set_error(err, SB_OK, "");
        return SB_OK;
    }

    std::string sql;
    std::string build_error;
    if (!build_bulk_copy_sql(table_name, columns, column_count, &sql, &build_error)) {
        set_error(err, SB_ERR_INVALID_PARAM, build_error);
        return SB_ERR_INVALID_PARAM;
    }

    std::string copy_data;
    if (!build_bulk_copy_data(columns,
                              column_count,
                              rows,
                              row_count,
                              &copy_data,
                              &build_error)) {
        set_error(err, SB_ERR_INVALID_PARAM, build_error);
        return SB_ERR_INVALID_PARAM;
    }

    sb_error op_err{};
    int64_t inserted = 0;
    const int rc = sb_execute_copy_from_buffer(conn,
                                               sql.c_str(),
                                               copy_data.data(),
                                               copy_data.size(),
                                               &inserted,
                                               &op_err);
    if (rc != SB_OK) {
        copy_error(err, op_err);
        return rc;
    }
    if (inserted <= 0) {
        inserted = static_cast<int64_t>(row_count);
    }

    if (rows_inserted) {
        *rows_inserted = inserted;
    }
    ensure_tracking(conn);
    set_error(err, SB_OK, "");
    return SB_OK;
}

int sb_connection_is_healthy(sb_connection* conn) {
    sb_error err{};
    return sb_is_healthy(conn, &err);
}

int sb_connection_validate(sb_connection* conn, uint32_t timeout_ms, sb_error* err) {
    (void)timeout_ms;
    if (!conn) {
        set_error(err, SB_ERR_INVALID_HANDLE, "Connection is null");
        return SB_ERR_INVALID_HANDLE;
    }
    const int rc = sb_ping(conn, err);
    if (rc == SB_OK) {
        ensure_tracking(conn);
    }
    return rc;
}

uint32_t sb_connection_get_age_seconds(sb_connection* conn) {
    if (!conn) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(g_tracking_mutex);
    auto it = g_tracking.find(conn);
    if (it == g_tracking.end()) {
        return 0;
    }
    auto age = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - it->second.created_at);
    return static_cast<uint32_t>(std::max<int64_t>(0, age.count()));
}

uint32_t sb_connection_get_idle_seconds(sb_connection* conn) {
    if (!conn) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(g_tracking_mutex);
    auto it = g_tracking.find(conn);
    if (it == g_tracking.end()) {
        return 0;
    }
    auto idle = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - it->second.last_used_at);
    return static_cast<uint32_t>(std::max<int64_t>(0, idle.count()));
}

namespace scratchbird {
namespace client {

ConnectionLease::ConnectionLease() = default;

ConnectionLease::ConnectionLease(sb_connection_pool* pool, sb_connection* conn)
    : pool_(pool), conn_(conn) {}

ConnectionLease::~ConnectionLease() {
    reset();
}

ConnectionLease::ConnectionLease(ConnectionLease&& other) noexcept
    : pool_(other.pool_), conn_(other.conn_) {
    other.pool_ = nullptr;
    other.conn_ = nullptr;
}

ConnectionLease& ConnectionLease::operator=(ConnectionLease&& other) noexcept {
    if (this != &other) {
        reset();
        pool_ = other.pool_;
        conn_ = other.conn_;
        other.pool_ = nullptr;
        other.conn_ = nullptr;
    }
    return *this;
}

bool ConnectionLease::valid() const {
    return conn_ != nullptr;
}

sb_connection* ConnectionLease::raw() const {
    return conn_;
}

sb_result* ConnectionLease::query(const std::string& sql, sb_error* err) const {
    if (!conn_) {
        set_error(err, SB_ERR_INVALID_HANDLE, "Connection lease is empty");
        return nullptr;
    }
    return sb_query(conn_, sql.c_str(), err);
}

sb_result* ConnectionLease::execute(const std::string& sql, sb_error* err) const {
    if (!conn_) {
        set_error(err, SB_ERR_INVALID_HANDLE, "Connection lease is empty");
        return nullptr;
    }
    return sb_execute(conn_, sql.c_str(), err);
}

void ConnectionLease::reset() {
    if (pool_ && conn_) {
        sb_pool_release(pool_, conn_);
    }
    pool_ = nullptr;
    conn_ = nullptr;
}

ConnectionPool::ConnectionPool() = default;

ConnectionPool::~ConnectionPool() {
    close();
}

ConnectionPool::ConnectionPool(ConnectionPool&& other) noexcept
    : pool_(other.pool_) {
    other.pool_ = nullptr;
}

ConnectionPool& ConnectionPool::operator=(ConnectionPool&& other) noexcept {
    if (this != &other) {
        close();
        pool_ = other.pool_;
        other.pool_ = nullptr;
    }
    return *this;
}

bool ConnectionPool::open(const std::string& conn_str,
                          const sb_pool_config& config,
                          sb_error* err) {
    close();
    pool_ = sb_pool_create(conn_str.c_str(), &config, err);
    return pool_ != nullptr;
}

void ConnectionPool::close() {
    if (pool_) {
        sb_pool_destroy(pool_);
        pool_ = nullptr;
    }
}

bool ConnectionPool::isOpen() const {
    return pool_ != nullptr;
}

PoolStats ConnectionPool::stats() const {
    PoolStats out{};
    if (!pool_) {
        return out;
    }
    const sb_pool_stats stats = sb_pool_get_stats(pool_);
    out.available_connections = stats.available_connections;
    out.total_connections = stats.total_connections;
    out.max_connections = stats.max_connections;
    return out;
}

ConnectionLease ConnectionPool::acquire(sb_error* err) {
    if (!pool_) {
        set_error(err, SB_ERR_INVALID_HANDLE, "Pool is not open");
        return ConnectionLease();
    }
    sb_connection* conn = sb_pool_acquire(pool_, err);
    if (!conn) {
        return ConnectionLease();
    }
    return ConnectionLease(pool_, conn);
}

} // namespace client
} // namespace scratchbird
