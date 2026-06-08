// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#define private public
#include "scratchbird/odbc/odbc_handles.h"
#include "scratchbird/odbc/odbc_client_bridge.h"
#include "scratchbird/odbc/statement_cache.h"
#undef private
#include "scratchbird/odbc/odbc_driver.h"

using namespace scratchbird::odbc;

namespace {

class EnvGuard {
public:
    explicit EnvGuard(const char* name) : name_(name) {
        const char* existing = std::getenv(name);
        if (existing) {
            had_value_ = true;
            old_value_ = existing;
        }
#if defined(_WIN32)
        _putenv_s(name, "");
#else
        unsetenv(name);
#endif
    }

    ~EnvGuard() {
#if defined(_WIN32)
        if (had_value_) {
            _putenv_s(name_.c_str(), old_value_.c_str());
        } else {
            _putenv_s(name_.c_str(), "");
        }
#else
        if (had_value_) {
            setenv(name_.c_str(), old_value_.c_str(), 1);
        } else {
            unsetenv(name_.c_str());
        }
#endif
    }

private:
    std::string name_;
    bool had_value_{false};
    std::string old_value_;
};

class FakeOdbcClientBridge : public scratchbird::odbc::OdbcClientBridge {
public:
    SQLRETURN executeSQL(const std::string& sql,
                         std::vector<std::vector<std::string>>& results,
                         std::vector<scratchbird::odbc::ColumnMetadata>& columns,
                         SQLLEN& rows_affected) override {
        (void)columns;
        results.clear();
        rows_affected = 0;

        if (sql == "SHOW TABLES") {
            results = {{"users"}, {"audit_log"}, {"type_matrix"}};
            return SQL_SUCCESS;
        }
        if (sql == "SHOW COLUMNS FROM users") {
            results = {
                {"id", "UUID", "NO", "PRI", "", ""},
                {"created_at", "TIMESTAMP", "NO", "", "", ""},
                {"name", "VARCHAR(64)", "YES", "", "", ""}
            };
            return SQL_SUCCESS;
        }
        if (sql == "SHOW COLUMNS FROM audit_log") {
            results = {
                {"event_id", "BIGINT", "NO", "PRI", "", ""},
                {"event_time", "TIMESTAMP", "NO", "", "", ""}
            };
            return SQL_SUCCESS;
        }
        if (sql == "SHOW COLUMNS FROM type_matrix") {
            results = {
                {"price", "MONEY", "YES", "", "", ""},
                {"active_window", "INT4RANGE", "YES", "", "", ""},
                {"network_addr", "INET", "YES", "", "", ""},
                {"payload", "BYTEA", "YES", "", "", ""},
                {"embedding", "VECTOR", "YES", "", "", ""},
                {"window_size", "INTERVAL", "YES", "", "", ""}
            };
            return SQL_SUCCESS;
        }
        if (sql == "SHOW INDEXES FROM users") {
            results = {{"users", "0", "PRIMARY", "id", "BTREE"}};
            return SQL_SUCCESS;
        }
        if (sql == "SHOW INDEXES FROM audit_log") {
            results = {{"audit_log", "0", "PRIMARY", "event_id", "BTREE"}};
            return SQL_SUCCESS;
        }
        if (sql == "SHOW GRANTS") {
            results = {
                {"alice", "public.users", "SELECT", "dba", "YES"},
                {"alice", "public.users.id", "UPDATE", "dba", "NO"},
                {"dave", "public.orders", "INSERT", "SYSTEM", "NO"},
                {"carol", "public.users.email", "SELECT", "alice", "YES"},
                {"PUBLIC", "ROLE auditors", "ROLE", "dba", "YES"},
                {"eve", "sales.orders.status", "SELECT", "dba", "YES"}
            };
            return SQL_SUCCESS;
        }
        if (sql == "SELECT schema_id, schema_name FROM sb_catalog.sb_schemas") {
            results = {{"schema_public", "public"}};
            return SQL_SUCCESS;
        }
        if (sql == "SELECT table_id, schema_id, table_name FROM sb_catalog.sb_tables") {
            results = {
                {"tbl_users", "schema_public", "users"},
                {"tbl_audit", "schema_public", "audit_log"}
            };
            return SQL_SUCCESS;
        }
        if (sql == "SELECT fk_name, child_table_id, parent_table_id, child_columns, parent_columns, on_update, on_delete, match_type, is_enabled "
                   "FROM sb_catalog.sb_foreign_keys") {
            results = {
                {"fk_audit_user", "tbl_audit", "tbl_users", "user_id,role_id", "id,role_id", "2", "1", "0", "1"}
            };
            return SQL_SUCCESS;
        }
        if (sql == "SELECT routine_schema, routine_name, routine_type, data_type "
                   "FROM information_schema.routines "
                   "ORDER BY routine_schema, routine_name") {
            results = {
                {"public", "fn_total", "FUNCTION", "BIGINT"},
                {"analytics", "sp_cleanup", "PROCEDURE", "VOID"}
            };
            return SQL_SUCCESS;
        }
        if (sql == "SELECT routine_schema, routine_name, parameter_mode, parameter_name, data_type "
                   "FROM information_schema.parameters "
                   "ORDER BY routine_schema, routine_name, ordinal_position") {
            results = {
                {"public", "fn_total", "IN", "a", "INTEGER"},
                {"public", "fn_total", "IN", "b", "INTEGER"},
                {"analytics", "sp_cleanup", "OUT", "status", "VARCHAR"},
            };
            return SQL_SUCCESS;
        }
        if (sql == "SELECT routine_schema, routine_name, ordinal_position, parameter_mode, "
                   "parameter_name, data_type, character_maximum_length, numeric_precision, numeric_scale "
                   "FROM information_schema.parameters "
                   "ORDER BY routine_schema, routine_name, ordinal_position") {
            results = {
                {"public", "fn_total", "1", "IN", "a", "INTEGER", "0", "0", "0"},
                {"public", "fn_total", "2", "IN", "b", "INTEGER", "0", "0", "0"},
                {"analytics", "sp_cleanup", "1", "OUT", "status", "VARCHAR", "24", "0", "0"},
            };
            return SQL_SUCCESS;
        }

        return SQL_ERROR;
    }
};

class OdbcCatalogTest : public ::testing::Test {
protected:
    scratchbird::odbc::OdbcEnvironment env_{};
    scratchbird::odbc::OdbcConnection conn_{&env_};
    scratchbird::odbc::OdbcStatement stmt_{&conn_};

    void SetUp() override {
        conn_.connected_ = true;
        conn_.current_database_ = "testdb";
        conn_.current_schema_ = "public";
        conn_.client_bridge_ = std::make_unique<FakeOdbcClientBridge>();
    }

    static const SQLCHAR* toSqlChar(const std::string& value) {
        return reinterpret_cast<const SQLCHAR*>(value.c_str());
    }

    static const std::vector<std::string>* findRow(const std::vector<std::vector<std::string>>& rows,
                                                   size_t index,
                                                   const std::string& value) {
        for (const auto& row : rows) {
            if (row.size() > index && row[index] == value) {
                return &row;
            }
        }
        return nullptr;
    }
};

TEST_F(OdbcCatalogTest, TablesHonorsPatterns) {
    std::string table_pattern = "user%";
    SQLRETURN rc = stmt_.tables(nullptr, 0, nullptr, 0,
                                toSqlChar(table_pattern), SQL_NTS,
                                nullptr, 0);
    ASSERT_EQ(rc, SQL_SUCCESS);
    ASSERT_EQ(stmt_.columns_.size(), 10u);
    ASSERT_EQ(stmt_.rows_.size(), 1u);
    EXPECT_EQ(stmt_.columns_[0].name, "TABLE_CAT");
    EXPECT_EQ(stmt_.columns_[1].name, "TABLE_SCHEM");
    EXPECT_EQ(stmt_.columns_[2].name, "TABLE_NAME");
    EXPECT_EQ(stmt_.rows_[0][0], "testdb");
    EXPECT_EQ(stmt_.rows_[0][1], "public");
    EXPECT_EQ(stmt_.rows_[0][2], "users");
    EXPECT_EQ(stmt_.rows_[0][3], "TABLE");
    EXPECT_EQ(stmt_.rows_[0][4], "");
    EXPECT_EQ(stmt_.rows_[0][5], "");
    EXPECT_EQ(stmt_.rows_[0][6], "");
    EXPECT_EQ(stmt_.rows_[0][7], "");
    EXPECT_EQ(stmt_.rows_[0][8], "");
    EXPECT_EQ(stmt_.rows_[0][9], "");

    std::string view_type = "'VIEW'";
    rc = stmt_.tables(nullptr, 0, nullptr, 0, nullptr, 0,
                      toSqlChar(view_type), SQL_NTS);
    ASSERT_EQ(rc, SQL_SUCCESS);
    EXPECT_TRUE(stmt_.rows_.empty());
}

TEST(OdbcRetryContractTest, StopsOnOperatorInterventionSqlState) {
    OdbcEnvironment env;
    OdbcConnection conn(&env);
    conn.connected_ = true;

    int attempts = 0;
    auto op = [](void* user_data) -> SQLRETURN {
        auto* state = static_cast<std::pair<OdbcConnection*, int>*>(user_data);
        ++state->second;
        state->first->clearDiagnostics();
        DiagnosticRecord diag;
        diag.sqlstate = "57014";
        diag.message = "query canceled";
        state->first->addDiagnostic(diag);
        return SQL_ERROR;
    };

    std::pair<OdbcConnection*, int> state{&conn, 0};
    sb_odbc_retry_config cfg = sb_odbc_retry_config_default();
    cfg.max_retries = 3;
    cfg.base_delay_ms = 1;
    cfg.max_delay_ms = 2;
    SQLULEN attempt_count = 0;
    SQLRETURN rc = sb_odbc_with_retry(&conn, &cfg, op, &state, &attempt_count);

    EXPECT_EQ(rc, SQL_ERROR);
    EXPECT_EQ(state.second, 1);
    EXPECT_EQ(attempt_count, 0u);
}

TEST(OdbcRetryContractTest, RetriesReconnectOrStatementBoundarySqlStatesOnly) {
    OdbcEnvironment env;
    OdbcConnection conn(&env);
    conn.connected_ = true;

    auto op = [](void* user_data) -> SQLRETURN {
        auto* state = static_cast<std::pair<OdbcConnection*, int>*>(user_data);
        ++state->second;
        state->first->clearDiagnostics();
        if (state->second < 3) {
            DiagnosticRecord diag;
            diag.sqlstate = "08006";
            diag.message = "connection failure";
            state->first->addDiagnostic(diag);
            return SQL_ERROR;
        }
        return SQL_SUCCESS;
    };

    std::pair<OdbcConnection*, int> state{&conn, 0};
    sb_odbc_retry_config cfg = sb_odbc_retry_config_default();
    cfg.max_retries = 4;
    cfg.base_delay_ms = 1;
    cfg.max_delay_ms = 2;
    SQLULEN attempt_count = 0;
    SQLRETURN rc = sb_odbc_with_retry(&conn, &cfg, op, &state, &attempt_count);

    EXPECT_EQ(rc, SQL_SUCCESS);
    EXPECT_EQ(state.second, 3);
    EXPECT_EQ(attempt_count, 2u);
}

TEST(OdbcRetryContractTest, HonorsRetryFlagsForDeadlockFamily) {
    OdbcEnvironment env;
    OdbcConnection conn(&env);
    conn.connected_ = true;

    auto op = [](void* user_data) -> SQLRETURN {
        auto* state = static_cast<std::pair<OdbcConnection*, int>*>(user_data);
        ++state->second;
        state->first->clearDiagnostics();
        DiagnosticRecord diag;
        diag.sqlstate = "40001";
        diag.message = "serialization failure";
        state->first->addDiagnostic(diag);
        return SQL_ERROR;
    };

    std::pair<OdbcConnection*, int> state{&conn, 0};
    sb_odbc_retry_config cfg = sb_odbc_retry_config_default();
    cfg.max_retries = 3;
    cfg.retry_on_deadlock = SQL_FALSE;
    SQLULEN attempt_count = 0;
    SQLRETURN rc = sb_odbc_with_retry(&conn, &cfg, op, &state, &attempt_count);

    EXPECT_EQ(rc, SQL_ERROR);
    EXPECT_EQ(state.second, 1);
    EXPECT_EQ(attempt_count, 0u);
}

TEST_F(OdbcCatalogTest, ColumnsParseTypesAndPrimaryKeys) {
    std::string table_pattern = "users";
    SQLRETURN rc = stmt_.columns(nullptr, 0, nullptr, 0,
                                 toSqlChar(table_pattern), SQL_NTS,
                                 nullptr, 0);
    ASSERT_EQ(rc, SQL_SUCCESS);
    ASSERT_EQ(stmt_.rows_.size(), 3u);

    const auto* id_row = findRow(stmt_.rows_, 3, "id");
    ASSERT_NE(id_row, nullptr);
    EXPECT_EQ((*id_row)[4], std::to_string(SQL_GUID));
    EXPECT_EQ((*id_row)[5], "UUID");

    const auto* ts_row = findRow(stmt_.rows_, 3, "created_at");
    ASSERT_NE(ts_row, nullptr);
    EXPECT_EQ((*ts_row)[4], std::to_string(SQL_TYPE_TIMESTAMP));
    EXPECT_EQ((*ts_row)[5], "TIMESTAMP");

    const auto* name_row = findRow(stmt_.rows_, 3, "name");
    ASSERT_NE(name_row, nullptr);
    EXPECT_EQ((*name_row)[4], std::to_string(SQL_VARCHAR));
    EXPECT_EQ((*name_row)[6], "64");
    EXPECT_EQ((*name_row)[17], "YES");

    rc = stmt_.primaryKeys(nullptr, 0, nullptr, 0, toSqlChar(table_pattern), SQL_NTS);
    ASSERT_EQ(rc, SQL_SUCCESS);
    ASSERT_EQ(stmt_.rows_.size(), 1u);
    EXPECT_EQ(stmt_.rows_[0][2], "users");
    EXPECT_EQ(stmt_.rows_[0][3], "id");
    EXPECT_EQ(stmt_.rows_[0][5], "PRIMARY");
}

TEST(OdbcConnectionConfigTest, BuildConfigDefaultsSessionSchemaToUsersPublic) {
    EnvGuard schema("SCRATCHBIRD_SCHEMA");
    scratchbird::odbc::ConnectionParams params;
    params.server = "127.0.0.1";
    params.port = 13092;
    params.database = "main";
    params.user = "SysArch";
    params.password = "replaceme";

    scratchbird::odbc::OdbcClientBridge bridge;
    auto cfg = bridge.buildConfig(params);

    EXPECT_EQ(cfg.schema, "users.public");

    params.schema = "tenant.analytics";
    cfg = bridge.buildConfig(params);
    EXPECT_EQ(cfg.schema, "tenant.analytics");
}

TEST_F(OdbcCatalogTest, ColumnsParseExtendedTypeMatrixShapes) {
    std::string table_pattern = "type_matrix";
    SQLRETURN rc = stmt_.columns(nullptr, 0, nullptr, 0,
                                 toSqlChar(table_pattern), SQL_NTS,
                                 nullptr, 0);
    ASSERT_EQ(rc, SQL_SUCCESS);
    ASSERT_EQ(stmt_.rows_.size(), 6u);

    const auto* price_row = findRow(stmt_.rows_, 3, "price");
    ASSERT_NE(price_row, nullptr);
    EXPECT_EQ((*price_row)[4], std::to_string(SQL_DECIMAL));
    EXPECT_EQ((*price_row)[5], "MONEY");

    const auto* range_row = findRow(stmt_.rows_, 3, "active_window");
    ASSERT_NE(range_row, nullptr);
    EXPECT_EQ((*range_row)[4], std::to_string(SQL_LONGVARCHAR));
    EXPECT_EQ((*range_row)[5], "INT4RANGE");

    const auto* inet_row = findRow(stmt_.rows_, 3, "network_addr");
    ASSERT_NE(inet_row, nullptr);
    EXPECT_EQ((*inet_row)[4], std::to_string(SQL_VARCHAR));
    EXPECT_EQ((*inet_row)[5], "INET");

    const auto* payload_row = findRow(stmt_.rows_, 3, "payload");
    ASSERT_NE(payload_row, nullptr);
    EXPECT_EQ((*payload_row)[4], std::to_string(SQL_LONGVARBINARY));
    EXPECT_EQ((*payload_row)[5], "BYTEA");

    const auto* vector_row = findRow(stmt_.rows_, 3, "embedding");
    ASSERT_NE(vector_row, nullptr);
    EXPECT_EQ((*vector_row)[4], std::to_string(SQL_LONGVARBINARY));
    EXPECT_EQ((*vector_row)[5], "VECTOR");

    const auto* interval_row = findRow(stmt_.rows_, 3, "window_size");
    ASSERT_NE(interval_row, nullptr);
    EXPECT_EQ((*interval_row)[4], std::to_string(SQL_VARCHAR));
    EXPECT_EQ((*interval_row)[5], "INTERVAL");
}

TEST_F(OdbcCatalogTest, ProceduresExposeInputOutputAndResultCounts) {
    SQLRETURN rc = stmt_.procedures(nullptr, 0, toSqlChar("public"), SQL_NTS,
                                    toSqlChar("fn%"), SQL_NTS);
    ASSERT_EQ(rc, SQL_SUCCESS);
    ASSERT_EQ(stmt_.rows_.size(), 1u);

    const auto* row = findRow(stmt_.rows_, 2, "fn_total");
    ASSERT_NE(row, nullptr);
    EXPECT_EQ((*row)[1], "public");
    EXPECT_EQ((*row)[3], "2");  // two IN parameters
    EXPECT_EQ((*row)[4], "0");  // no OUT parameters
    EXPECT_EQ((*row)[5], "1");  // function returns result set count in this implementation
    EXPECT_EQ((*row)[7], "2");
}

TEST_F(OdbcCatalogTest, ProcedureColumnsExposeFunctionAndProcedurePaths) {
    SQLRETURN rc = stmt_.procedureColumns(nullptr, 0, toSqlChar("public"), SQL_NTS,
                                          toSqlChar("fn_total"), SQL_NTS,
                                          nullptr, 0);
    ASSERT_EQ(rc, SQL_SUCCESS);
    ASSERT_EQ(stmt_.rows_.size(), 3u);  // two inputs + synthetic return value row
    const auto* return_row = findRow(stmt_.rows_, 3, "RETURN_VALUE");
    ASSERT_NE(return_row, nullptr);
    EXPECT_EQ((*return_row)[4], std::to_string(5));
    EXPECT_EQ((*return_row)[17], "0");

    rc = stmt_.procedureColumns(nullptr, 0, toSqlChar("analytics"), SQL_NTS,
                                toSqlChar("sp_cleanup"), SQL_NTS,
                                toSqlChar("status"), SQL_NTS);
    ASSERT_EQ(rc, SQL_SUCCESS);
    ASSERT_EQ(stmt_.rows_.size(), 1u);
    EXPECT_EQ(stmt_.rows_[0][3], "status");
    EXPECT_EQ(stmt_.rows_[0][4], std::to_string(SQL_PARAM_OUTPUT));
}

TEST_F(OdbcCatalogTest, TablePrivilegesFiltersBySchemaAndPattern) {
    SQLRETURN rc = stmt_.tablePrivileges(nullptr, 0, toSqlChar("public"), SQL_NTS,
                                       toSqlChar("users"), SQL_NTS);
    ASSERT_EQ(rc, SQL_SUCCESS);
    ASSERT_EQ(stmt_.rows_.size(), 1u);

    const auto* users_row = findRow(stmt_.rows_, 2, "users");
    ASSERT_NE(users_row, nullptr);
    EXPECT_EQ((*users_row)[0], "testdb");
    EXPECT_EQ((*users_row)[1], "public");
    EXPECT_EQ((*users_row)[2], "users");
    EXPECT_EQ((*users_row)[3], "dba");
    EXPECT_EQ((*users_row)[4], "alice");
    EXPECT_EQ((*users_row)[5], "SELECT");
    EXPECT_EQ((*users_row)[6], "YES");

    rc = stmt_.tablePrivileges(nullptr, 0, nullptr, 0,
                              toSqlChar("sales.orders"), SQL_NTS);
    ASSERT_EQ(rc, SQL_SUCCESS);
    ASSERT_EQ(stmt_.rows_.size(), 1u);
    EXPECT_EQ(stmt_.rows_[0][1], "sales");
    EXPECT_EQ(stmt_.rows_[0][2], "orders");
    EXPECT_EQ(stmt_.rows_[0][5], "SELECT");
}

TEST_F(OdbcCatalogTest, ColumnPrivilegesFiltersByTableAndColumn) {
    SQLRETURN rc = stmt_.columnPrivileges(nullptr, 0, toSqlChar("public"), SQL_NTS,
                                        toSqlChar("users"), SQL_NTS,
                                        toSqlChar("id"), SQL_NTS);
    ASSERT_EQ(rc, SQL_SUCCESS);
    ASSERT_EQ(stmt_.rows_.size(), 1u);

    EXPECT_EQ(stmt_.rows_[0][0], "testdb");
    EXPECT_EQ(stmt_.rows_[0][1], "public");
    EXPECT_EQ(stmt_.rows_[0][2], "users");
    EXPECT_EQ(stmt_.rows_[0][3], "id");
    EXPECT_EQ(stmt_.rows_[0][4], "dba");
    EXPECT_EQ(stmt_.rows_[0][5], "alice");
    EXPECT_EQ(stmt_.rows_[0][6], "UPDATE");
    EXPECT_EQ(stmt_.rows_[0][7], "NO");

    rc = stmt_.columnPrivileges(nullptr, 0, nullptr, 0,
                               toSqlChar("public.users.email"), SQL_NTS,
                               nullptr, 0);
    ASSERT_EQ(rc, SQL_SUCCESS);
    ASSERT_EQ(stmt_.rows_.size(), 1u);
    EXPECT_EQ(stmt_.rows_[0][2], "users");
    EXPECT_EQ(stmt_.rows_[0][3], "email");
}

TEST_F(OdbcCatalogTest, StatisticsAndSpecialColumnsUsePrimaryKey) {
    std::string table_pattern = "users";
    SQLRETURN rc = stmt_.statistics(nullptr, 0, nullptr, 0, toSqlChar(table_pattern), SQL_NTS,
                                    0, 0);
    ASSERT_EQ(rc, SQL_SUCCESS);
    ASSERT_EQ(stmt_.rows_.size(), 1u);
    EXPECT_EQ(stmt_.rows_[0][2], "users");
    EXPECT_EQ(stmt_.rows_[0][3], "0");
    EXPECT_EQ(stmt_.rows_[0][5], "PRIMARY");
    EXPECT_EQ(stmt_.rows_[0][8], "id");

    rc = stmt_.specialColumns(0, nullptr, 0, nullptr, 0, toSqlChar(table_pattern), SQL_NTS,
                               0, 0);
    ASSERT_EQ(rc, SQL_SUCCESS);
    ASSERT_EQ(stmt_.rows_.size(), 1u);
    EXPECT_EQ(stmt_.rows_[0][1], "id");
    EXPECT_EQ(stmt_.rows_[0][0], "2");
    EXPECT_EQ(stmt_.rows_[0][7], "1");
}

TEST_F(OdbcCatalogTest, ForeignKeysExposeMappings) {
    std::string pk_table = "users";
    std::string fk_table = "audit_%";
    SQLRETURN rc = stmt_.foreignKeys(nullptr, 0, nullptr, 0,
                                     toSqlChar(pk_table), SQL_NTS,
                                     nullptr, 0, nullptr, 0,
                                     toSqlChar(fk_table), SQL_NTS);
    ASSERT_EQ(rc, SQL_SUCCESS);
    ASSERT_EQ(stmt_.rows_.size(), 2u);

    const auto* user_row = findRow(stmt_.rows_, 7, "user_id");
    ASSERT_NE(user_row, nullptr);
    EXPECT_EQ((*user_row)[2], "users");
    EXPECT_EQ((*user_row)[6], "audit_log");
    EXPECT_EQ((*user_row)[3], "id");
    EXPECT_EQ((*user_row)[8], "1");
    EXPECT_EQ((*user_row)[9], "0");
    EXPECT_EQ((*user_row)[10], "1");
    EXPECT_EQ((*user_row)[11], "fk_audit_user");
    EXPECT_EQ((*user_row)[12], "PRIMARY");
    EXPECT_EQ((*user_row)[13], "7");

    const auto* role_row = findRow(stmt_.rows_, 7, "role_id");
    ASSERT_NE(role_row, nullptr);
    EXPECT_EQ((*role_row)[3], "role_id");
    EXPECT_EQ((*role_row)[8], "2");
}

TEST(OdbcGetDataTest, TemporalAndGuidConversions) {
    scratchbird::odbc::OdbcEnvironment env;
    scratchbird::odbc::OdbcConnection conn(&env);
    scratchbird::odbc::OdbcStatement stmt(&conn);

    scratchbird::odbc::ColumnMetadata date_col;
    date_col.sql_type = SQL_TYPE_DATE;
    scratchbird::odbc::ColumnMetadata time_col;
    time_col.sql_type = SQL_TYPE_TIME;
    scratchbird::odbc::ColumnMetadata ts_col;
    ts_col.sql_type = SQL_TYPE_TIMESTAMP;
    scratchbird::odbc::ColumnMetadata guid_col;
    guid_col.sql_type = SQL_GUID;

    std::string date_value = "2025-01-02";
    std::string time_value = "13:14:15.123456";
    std::string ts_value = "2025-01-02 13:14:15.654321";
    std::string guid_text = "00112233-4455-6677-8899-aabbccddeeff";
    std::string guid_binary;
    guid_binary.assign(
        reinterpret_cast<const char*>("\x00\x11\x22\x33\x44\x55\x66\x77\x88\x99\xaa\xbb\xcc\xdd\xee\xff"),
        16);

    stmt.has_results_ = true;
    stmt.columns_ = {date_col, time_col, ts_col, guid_col};
    stmt.rows_ = {
        {date_value, time_value, ts_value, guid_text},
        {date_value, time_value, ts_value, guid_binary}
    };

    stmt.current_row_ = 1;
    scratchbird::odbc::SQL_DATE_STRUCT date_struct{};
    SQLLEN ind = 0;
    ASSERT_EQ(stmt.getData(1, SQL_C_DATE, &date_struct, sizeof(date_struct), &ind), SQL_SUCCESS);
    EXPECT_EQ(date_struct.year, 2025);
    EXPECT_EQ(date_struct.month, 1);
    EXPECT_EQ(date_struct.day, 2);
    EXPECT_EQ(ind, static_cast<SQLLEN>(sizeof(scratchbird::odbc::SQL_DATE_STRUCT)));

    scratchbird::odbc::SQL_TIME_STRUCT time_struct{};
    ASSERT_EQ(stmt.getData(2, SQL_C_TIME, &time_struct, sizeof(time_struct), &ind), SQL_SUCCESS);
    EXPECT_EQ(time_struct.hour, 13);
    EXPECT_EQ(time_struct.minute, 14);
    EXPECT_EQ(time_struct.second, 15);

    scratchbird::odbc::SQL_TIMESTAMP_STRUCT ts_struct{};
    ASSERT_EQ(stmt.getData(3, SQL_C_TIMESTAMP, &ts_struct, sizeof(ts_struct), &ind), SQL_SUCCESS);
    EXPECT_EQ(ts_struct.year, 2025);
    EXPECT_EQ(ts_struct.month, 1);
    EXPECT_EQ(ts_struct.day, 2);
    EXPECT_EQ(ts_struct.hour, 13);
    EXPECT_EQ(ts_struct.minute, 14);
    EXPECT_EQ(ts_struct.second, 15);
    EXPECT_EQ(ts_struct.fraction, 654321000u);

    scratchbird::odbc::SQLGUID guid{};
    ASSERT_EQ(stmt.getData(4, SQL_C_GUID, &guid, sizeof(guid), &ind), SQL_SUCCESS);
    EXPECT_EQ(guid.Data1, 0x00112233u);
    EXPECT_EQ(guid.Data2, 0x4455u);
    EXPECT_EQ(guid.Data3, 0x6677u);
    EXPECT_EQ(guid.Data4[0], 0x88u);
    EXPECT_EQ(guid.Data4[7], 0xffu);

    stmt.current_row_ = 2;
    scratchbird::odbc::SQLGUID guid_bin{};
    ASSERT_EQ(stmt.getData(4, SQL_C_GUID, &guid_bin, sizeof(guid_bin), &ind), SQL_SUCCESS);
    EXPECT_EQ(guid_bin.Data1, 0x00112233u);
    EXPECT_EQ(guid_bin.Data2, 0x4455u);
    EXPECT_EQ(guid_bin.Data3, 0x6677u);
    EXPECT_EQ(guid_bin.Data4[0], 0x88u);
    EXPECT_EQ(guid_bin.Data4[7], 0xffu);
}

class RecordingClientBridge : public scratchbird::odbc::OdbcClientBridge {
public:
    bool connected{true};
    std::vector<std::string> sql_log;
    scratchbird::odbc::ConnectionParams last_connect_params{};
    SQLRETURN connect(const scratchbird::odbc::ConnectionParams& params,
                      std::string& /*error*/) override {
        last_connect_params = params;
        connected = true;
        return SQL_SUCCESS;
    }
    SQLRETURN executeSQL(const std::string& sql,
                         std::vector<std::vector<std::string>>& results,
                         std::vector<scratchbird::odbc::ColumnMetadata>& columns,
                         SQLLEN& rows_affected) override {
        results.clear();
        columns.clear();
        rows_affected = 0;
        sql_log.push_back(sql);
        return SQL_SUCCESS;
    }
    bool isConnected() const override {
        return connected;
    }
};

class TransactionRecordingClientBridge : public RecordingClientBridge {
public:
    SQLRETURN commit_result{SQL_SUCCESS};
    SQLRETURN rollback_result{SQL_SUCCESS};
    int commit_calls{0};
    int rollback_calls{0};

    SQLRETURN commit() override {
        ++commit_calls;
        return commit_result;
    }

    SQLRETURN rollback() override {
        ++rollback_calls;
        return rollback_result;
    }
};

class WarningExecuteClientBridge : public RecordingClientBridge {
public:
    SQLRETURN execute_result{SQL_SUCCESS_WITH_INFO};

    SQLRETURN executeSQL(const std::string& sql,
                         std::vector<std::vector<std::string>>& results,
                         std::vector<scratchbird::odbc::ColumnMetadata>& columns,
                         SQLLEN& rows_affected) override {
        sql_log.push_back(sql);
        scratchbird::odbc::ColumnMetadata col;
        col.name = "value";
        col.sql_type = SQL_INTEGER;
        columns = {col};
        results = {{"1"}};
        rows_affected = 1;
        return execute_result;
    }
};

class StatusFailureClientBridge : public RecordingClientBridge {
public:
    explicit StatusFailureClientBridge(scratchbird::core::Status status,
                                       std::string message = "status failure")
        : status_(status), message_(std::move(message)) {}

    SQLRETURN executeSQL(const std::string& sql,
                         std::vector<std::vector<std::string>>& results,
                         std::vector<scratchbird::odbc::ColumnMetadata>& columns,
                         SQLLEN& rows_affected) override {
        (void)sql;
        results.clear();
        columns.clear();
        rows_affected = 0;
        last_status_ = status_;
        last_error_ = message_;
        return SQL_ERROR;
    }

private:
    scratchbird::core::Status status_;
    std::string message_;
};

class DisconnectRecordingClientBridge : public RecordingClientBridge {
public:
    int disconnect_calls{0};

    void disconnect() override {
        ++disconnect_calls;
        connected = false;
    }
};

class SmokeClientBridge : public RecordingClientBridge {
public:
    SQLRETURN executeSQL(const std::string& sql,
                         std::vector<std::vector<std::string>>& results,
                         std::vector<scratchbird::odbc::ColumnMetadata>& columns,
                         SQLLEN& rows_affected) override {
        results.clear();
        columns.clear();
        rows_affected = 0;
        sql_log.push_back(sql);

        if (sql == "SELECT 1") {
            scratchbird::odbc::ColumnMetadata col;
            col.name = "one";
            col.sql_type = SQL_INTEGER;
            columns.push_back(col);
            results.push_back({"1"});
            return SQL_SUCCESS;
        }
        return SQL_SUCCESS;
    }
};

TEST(OdbcAutocommitTest, SetAutocommitSendsConflictClause) {
    scratchbird::odbc::OdbcEnvironment env;
    scratchbird::odbc::OdbcConnection conn(&env);

    auto bridge = std::make_unique<RecordingClientBridge>();
    auto* bridge_ptr = bridge.get();
    conn.client_bridge_ = std::move(bridge);
    conn.connected_ = true;

    SQLRETURN rc = conn.setAttribute(SQL_ATTR_AUTOCOMMIT,
                                     reinterpret_cast<SQLPOINTER>(
                                         static_cast<uintptr_t>(SQL_AUTOCOMMIT_OFF)),
                                     0);
    ASSERT_EQ(rc, SQL_SUCCESS);
    EXPECT_EQ(conn.auto_commit_, SQL_AUTOCOMMIT_OFF);
    EXPECT_TRUE(conn.in_transaction_);
    ASSERT_EQ(bridge_ptr->sql_log.size(), 1u);
    EXPECT_EQ(bridge_ptr->sql_log[0], "SET AUTOCOMMIT OFF ON CONFLICT KEEP");

    rc = conn.setAttribute(SQL_ATTR_AUTOCOMMIT,
                           reinterpret_cast<SQLPOINTER>(
                               static_cast<uintptr_t>(SQL_AUTOCOMMIT_ON)),
                           0);
    ASSERT_EQ(rc, SQL_SUCCESS);
    EXPECT_EQ(conn.auto_commit_, SQL_AUTOCOMMIT_ON);
    EXPECT_FALSE(conn.in_transaction_);
    ASSERT_EQ(bridge_ptr->sql_log.size(), 2u);
    EXPECT_EQ(bridge_ptr->sql_log[1], "SET AUTOCOMMIT ON ON CONFLICT COMMIT");
}

TEST(OdbcLifecycleTest, DisconnectClearsAbandonedSessionState) {
    scratchbird::odbc::OdbcEnvironment env;
    scratchbird::odbc::OdbcConnection conn(&env);

    auto bridge = std::make_unique<DisconnectRecordingClientBridge>();
    auto* bridge_ptr = bridge.get();
    conn.client_bridge_ = std::move(bridge);
    conn.connected_ = true;
    conn.connection_dead_ = true;
    conn.in_transaction_ = true;
    conn.prepared_sql_[11] = "SELECT 1";
    ASSERT_NE(conn.createStatement(), nullptr);
    ASSERT_EQ(conn.getStatementCount(), 1u);

    SQLRETURN rc = conn.disconnect();

    ASSERT_EQ(rc, SQL_SUCCESS);
    EXPECT_FALSE(conn.connected_);
    EXPECT_FALSE(conn.connection_dead_);
    EXPECT_FALSE(conn.in_transaction_);
    EXPECT_TRUE(conn.prepared_sql_.empty());
    EXPECT_EQ(conn.getStatementCount(), 0u);
    EXPECT_EQ(bridge_ptr->disconnect_calls, 1);
    EXPECT_FALSE(bridge_ptr->connected);
}

TEST(OdbcAuthBootstrapTest, ParseConnectionStringCarriesAuthTokenAliasFamily) {
    scratchbird::odbc::OdbcEnvironment env;
    scratchbird::odbc::OdbcConnection conn(&env);

    SQLRETURN rc = conn.parseConnectionString(
        "Driver={ScratchBird};Server=127.0.0.1;Port=3092;Database=testdb;UID=user;PWD=pass;"
        "AuthToken=token_a;BearerToken=token_b;Token=token_c");
    ASSERT_EQ(rc, SQL_SUCCESS);
    EXPECT_EQ(conn.params_.auth_token, "token_c");
}

TEST(OdbcAuthBootstrapTest, EstablishConnectionPassesAuthTokenToBridge) {
    scratchbird::odbc::OdbcEnvironment env;
    scratchbird::odbc::OdbcConnection conn(&env);

    ASSERT_EQ(conn.parseConnectionString(
                  "Driver={ScratchBird};Server=127.0.0.1;Port=3092;Database=testdb;UID=user;PWD=pass;"
                  "AuthToken=bridge_token"),
              SQL_SUCCESS);

    auto bridge = std::make_unique<RecordingClientBridge>();
    auto* bridge_ptr = bridge.get();
    conn.client_bridge_ = std::move(bridge);

    ASSERT_EQ(conn.establishConnection(), SQL_SUCCESS);
    EXPECT_EQ(bridge_ptr->last_connect_params.auth_token, "bridge_token");
    EXPECT_EQ(bridge_ptr->last_connect_params.database, "testdb");
    EXPECT_EQ(bridge_ptr->last_connect_params.user, "user");
}

TEST(OdbcAuthBootstrapTest, ProbeAuthSurfaceRejectsInvalidAuthMethodNamespaceBeforeDial) {
    scratchbird::odbc::OdbcClientBridge bridge;
    scratchbird::odbc::ConnectionParams params;
    params.server = "127.0.0.1";
    params.port = 3092;
    params.database = "testdb";
    params.auth_method_id = "invalid.namespace";

    scratchbird::client::AuthProbeResult probe;
    std::string error;
    SQLRETURN rc = bridge.probeAuthSurface(params, probe, error);

    EXPECT_EQ(rc, SQL_ERROR);
    EXPECT_NE(error.find("auth_method_id must start with scratchbird.auth."), std::string::npos);
    auto auth = bridge.getResolvedAuthContext();
    EXPECT_FALSE(auth.attached);
}

TEST(OdbcAutocommitTest, IsolationMappingUsesSetTransaction) {
    scratchbird::odbc::OdbcEnvironment env;
    scratchbird::odbc::OdbcConnection conn(&env);

    auto bridge = std::make_unique<RecordingClientBridge>();
    auto* bridge_ptr = bridge.get();
    conn.client_bridge_ = std::move(bridge);
    conn.connected_ = true;

    SQLRETURN rc = conn.setAttribute(SQL_ATTR_TXN_ISOLATION,
                                     reinterpret_cast<SQLPOINTER>(
                                         static_cast<uintptr_t>(SQL_TXN_SERIALIZABLE)),
                                     0);
    ASSERT_EQ(rc, SQL_SUCCESS);
    EXPECT_EQ(conn.txn_isolation_, SQL_TXN_SERIALIZABLE);
    ASSERT_EQ(bridge_ptr->sql_log.size(), 2u);
    EXPECT_EQ(bridge_ptr->sql_log[0],
              "SET TRANSACTION ISOLATION LEVEL SERIALIZABLE ON CONFLICT COMMIT");
    EXPECT_EQ(bridge_ptr->sql_log[1], "SET AUTOCOMMIT ON ON CONFLICT COMMIT");
}

TEST(OdbcAutocommitTest, IsolationSqlDocumentsCanonicalAliasSurface) {
    scratchbird::odbc::OdbcEnvironment env;
    scratchbird::odbc::OdbcConnection conn(&env);

    auto bridge = std::make_unique<RecordingClientBridge>();
    auto* bridge_ptr = bridge.get();
    conn.client_bridge_ = std::move(bridge);
    conn.connected_ = true;

    auto setIsolation = [&](scratchbird::odbc::SQLUINTEGER isolation) -> std::string {
        bridge_ptr->sql_log.clear();
        SQLRETURN rc = conn.setAttribute(SQL_ATTR_TXN_ISOLATION,
                                         reinterpret_cast<SQLPOINTER>(static_cast<uintptr_t>(isolation)),
                                         0);
        EXPECT_EQ(rc, SQL_SUCCESS);
        EXPECT_GE(bridge_ptr->sql_log.size(), 1u);
        if (rc != SQL_SUCCESS || bridge_ptr->sql_log.empty()) {
            return "";
        }
        return bridge_ptr->sql_log.front();
    };

    EXPECT_EQ(setIsolation(SQL_TXN_READ_UNCOMMITTED),
              "SET TRANSACTION ISOLATION LEVEL READ UNCOMMITTED ON CONFLICT COMMIT");
    EXPECT_EQ(setIsolation(SQL_TXN_READ_COMMITTED),
              "SET TRANSACTION ISOLATION LEVEL READ COMMITTED ON CONFLICT COMMIT");
    EXPECT_EQ(setIsolation(SQL_TXN_REPEATABLE_READ),
              "SET TRANSACTION ISOLATION LEVEL REPEATABLE READ ON CONFLICT COMMIT");
    EXPECT_EQ(setIsolation(SQL_TXN_SERIALIZABLE),
              "SET TRANSACTION ISOLATION LEVEL SERIALIZABLE ON CONFLICT COMMIT");
}

TEST(OdbcLifecycleTest, PreparedDormantAndPortalCapabilitiesStayExplicit) {
    EXPECT_TRUE(scratchbird::odbc::supportsPreparedTransactions());
    EXPECT_FALSE(scratchbird::odbc::supportsDormantReattach());
    EXPECT_FALSE(scratchbird::odbc::supportsPortalResume());

    std::string sql;
    std::string sqlstate;
    std::string message;
    SQLRETURN rc = scratchbird::odbc::buildPreparedTransactionSql(
        " PREPARE TRANSACTION ",
        " gid'one ",
        sql,
        &sqlstate,
        &message);
    ASSERT_EQ(rc, SQL_SUCCESS);
    EXPECT_EQ(sql, "PREPARE TRANSACTION 'gid''one'");
    EXPECT_TRUE(sqlstate.empty());
    EXPECT_TRUE(message.empty());

    sql.clear();
    sqlstate.clear();
    message.clear();
    rc = scratchbird::odbc::buildPreparedTransactionSql(
        "COMMIT PREPARED",
        "   ",
        sql,
        &sqlstate,
        &message);
    ASSERT_EQ(rc, SQL_ERROR);
    EXPECT_TRUE(sql.empty());
    EXPECT_EQ(sqlstate, "42601");
    EXPECT_EQ(message, "Global transaction id is required");

    sqlstate.clear();
    message.clear();
    rc = scratchbird::odbc::rejectDormantReattach("reattach", &sqlstate, &message);
    ASSERT_EQ(rc, SQL_ERROR);
    EXPECT_EQ(sqlstate, "0A000");
    EXPECT_NE(message.find("dormant reattach"), std::string::npos);
}

TEST(OdbcTransactionTest, EnvHandleEndTranCommitsConnectedConnections) {
    scratchbird::odbc::OdbcEnvironment env;

    auto* conn1 = env.createConnection();
    auto* conn2 = env.createConnection();
    auto* conn3 = env.createConnection();

    auto bridge1 = std::make_unique<TransactionRecordingClientBridge>();
    auto bridge2 = std::make_unique<TransactionRecordingClientBridge>();
    auto bridge3 = std::make_unique<TransactionRecordingClientBridge>();

    auto* bridge1_ptr = bridge1.get();
    auto* bridge2_ptr = bridge2.get();
    auto* bridge3_ptr = bridge3.get();

    conn1->client_bridge_ = std::move(bridge1);
    conn1->connected_ = true;
    conn1->auto_commit_ = SQL_AUTOCOMMIT_OFF;
    conn1->in_transaction_ = true;

    conn2->client_bridge_ = std::move(bridge2);
    conn2->connected_ = true;
    conn2->auto_commit_ = SQL_AUTOCOMMIT_OFF;
    conn2->in_transaction_ = true;

    conn3->client_bridge_ = std::move(bridge3);
    conn3->connected_ = false;

    SQLRETURN rc = SQLEndTran(SQL_HANDLE_ENV, &env, SQL_COMMIT);
    ASSERT_EQ(rc, SQL_SUCCESS);
    EXPECT_EQ(bridge1_ptr->commit_calls, 1);
    EXPECT_EQ(bridge1_ptr->rollback_calls, 0);
    EXPECT_EQ(bridge2_ptr->commit_calls, 1);
    EXPECT_EQ(bridge2_ptr->rollback_calls, 0);
    EXPECT_EQ(bridge3_ptr->commit_calls, 0);
    EXPECT_EQ(bridge3_ptr->rollback_calls, 0);
}

TEST(OdbcExecutionParityTest, ExecuteAndExecDirectPropagateSuccessWithInfo) {
    scratchbird::odbc::OdbcEnvironment env;
    scratchbird::odbc::OdbcConnection conn(&env);
    scratchbird::odbc::OdbcStatement stmt(&conn);

    auto bridge = std::make_unique<WarningExecuteClientBridge>();
    auto* bridge_ptr = bridge.get();
    conn.client_bridge_ = std::move(bridge);
    conn.connected_ = true;

    ASSERT_EQ(stmt.prepare(reinterpret_cast<const SQLCHAR*>("SELECT 1"), SQL_NTS), SQL_SUCCESS);
    ASSERT_TRUE(stmt.prepared_);

    SQLRETURN rc = stmt.execute();
    ASSERT_EQ(rc, SQL_SUCCESS_WITH_INFO);
    EXPECT_TRUE(stmt.prepared_);
    EXPECT_TRUE(stmt.executed_);
    ASSERT_EQ(bridge_ptr->sql_log.size(), 1u);
    EXPECT_EQ(bridge_ptr->sql_log[0], "SELECT 1");

    stmt.prepared_ = true;
    rc = stmt.execDirect(reinterpret_cast<const SQLCHAR*>("SELECT 1"), SQL_NTS);
    ASSERT_EQ(rc, SQL_SUCCESS_WITH_INFO);
    EXPECT_FALSE(stmt.prepared_);
    EXPECT_TRUE(stmt.executed_);
    ASSERT_EQ(bridge_ptr->sql_log.size(), 2u);
    EXPECT_EQ(bridge_ptr->sql_log[1], "SELECT 1");
}

TEST(OdbcSqlStateMappingTest, MapsGranularStatusesToSpecificSqlStates) {
    struct MappingCase {
        scratchbird::core::Status status;
        const char* expected_sqlstate;
    };

    const std::vector<MappingCase> cases = {
        {scratchbird::core::Status::UNIQUE_VIOLATION, "23505"},
        {scratchbird::core::Status::FOREIGN_KEY_VIOLATION, "23503"},
        {scratchbird::core::Status::NOT_NULL_VIOLATION, "23502"},
        {scratchbird::core::Status::CHECK_VIOLATION, "23514"},
        {scratchbird::core::Status::EXCLUSION_VIOLATION, "23P01"},
        {scratchbird::core::Status::UNDEFINED_FUNCTION, "42883"},
        {scratchbird::core::Status::INSUFFICIENT_PRIVILEGE, "42501"},
        {scratchbird::core::Status::PROTOCOL_VIOLATION, "08S01"},
        {scratchbird::core::Status::TOO_MANY_CONNECTIONS, "08004"},
        {scratchbird::core::Status::NO_DATA_FOUND, "02000"},
    };

    for (const auto& item : cases) {
        scratchbird::odbc::OdbcEnvironment env;
        scratchbird::odbc::OdbcConnection conn(&env);
        conn.connected_ = true;
        conn.client_bridge_ = std::make_unique<StatusFailureClientBridge>(
            item.status,
            "forced status mapping");

        std::vector<std::vector<std::string>> rows;
        std::vector<scratchbird::odbc::ColumnMetadata> cols;
        SQLLEN rows_affected = 0;
        ASSERT_EQ(conn.executeSQL("SELECT 1", rows, cols, rows_affected), SQL_ERROR);
        const auto* diag = conn.getDiagnostic(1);
        ASSERT_NE(diag, nullptr);
        EXPECT_EQ(diag->sqlstate, item.expected_sqlstate);
    }
}

TEST(OdbcFetchTest, BindAndFetchPopulateBuffers) {
    scratchbird::odbc::OdbcEnvironment env;
    scratchbird::odbc::OdbcConnection conn(&env);
    scratchbird::odbc::OdbcStatement stmt(&conn);

    scratchbird::odbc::ColumnMetadata int_col;
    int_col.sql_type = SQL_INTEGER;
    scratchbird::odbc::ColumnMetadata text_col;
    text_col.sql_type = SQL_VARCHAR;

    stmt.has_results_ = true;
    stmt.columns_ = {int_col, text_col};
    stmt.rows_ = {{"42", "hello"}, {"100", ""}};
    stmt.current_row_ = 0;

    scratchbird::odbc::SQLINTEGER out_int = 0;
    char out_text[16] = {};
    SQLLEN int_ind = 0;
    SQLLEN text_ind = 0;
    SQLUSMALLINT row_status = 0;
    SQLULEN rows_fetched = 0;

    ASSERT_EQ(stmt.bindCol(1, SQL_C_LONG, &out_int, sizeof(out_int), &int_ind), SQL_SUCCESS);
    ASSERT_EQ(stmt.bindCol(2, SQL_C_CHAR, out_text, sizeof(out_text), &text_ind), SQL_SUCCESS);
    stmt.row_status_ptr_ = &row_status;
    stmt.rows_fetched_ptr_ = &rows_fetched;

    SQLRETURN rc = stmt.fetch();
    ASSERT_EQ(rc, SQL_SUCCESS);
    EXPECT_EQ(out_int, 42);
    EXPECT_STREQ(out_text, "hello");
    EXPECT_EQ(int_ind, static_cast<SQLLEN>(sizeof(scratchbird::odbc::SQLINTEGER)));
    EXPECT_EQ(text_ind, static_cast<SQLLEN>(std::strlen("hello")));
    EXPECT_EQ(row_status, SQL_ROW_SUCCESS);
    EXPECT_EQ(rows_fetched, 1u);

    rc = stmt.fetch();
    ASSERT_EQ(rc, SQL_SUCCESS);
    EXPECT_EQ(out_int, 100);
    EXPECT_EQ(text_ind, SQL_NULL_DATA);
}

TEST(OdbcSmokeTest, ConnectExecFetch) {
    scratchbird::odbc::OdbcEnvironment env;
    scratchbird::odbc::OdbcConnection conn(&env);

    auto bridge = std::make_unique<SmokeClientBridge>();
    auto* bridge_ptr = bridge.get();
    conn.client_bridge_ = std::move(bridge);

    SQLRETURN rc = conn.connect(nullptr, 0, nullptr, 0, nullptr, 0);
    ASSERT_EQ(rc, SQL_SUCCESS);
    ASSERT_TRUE(conn.connected_);
    ASSERT_GE(bridge_ptr->sql_log.size(), 2u);
    EXPECT_EQ(bridge_ptr->sql_log[0],
              "SET TRANSACTION ISOLATION LEVEL READ COMMITTED ON CONFLICT COMMIT");
    EXPECT_EQ(bridge_ptr->sql_log[1], "SET AUTOCOMMIT ON ON CONFLICT COMMIT");

    auto* stmt = conn.createStatement();
    ASSERT_NE(stmt, nullptr);
    rc = stmt->execDirect(reinterpret_cast<const SQLCHAR*>("SELECT 1"), SQL_NTS);
    ASSERT_EQ(rc, SQL_SUCCESS);

    scratchbird::odbc::SQLINTEGER out_value = 0;
    SQLLEN ind = 0;
    ASSERT_EQ(stmt->bindCol(1, SQL_C_LONG, &out_value, sizeof(out_value), &ind), SQL_SUCCESS);
    rc = stmt->fetch();
    ASSERT_EQ(rc, SQL_SUCCESS);
    EXPECT_EQ(out_value, 1);
}

} // namespace
