// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <string>
#include <memory>

#define private public
#include "scratchbird/odbc/odbc_handles.h"
#include "scratchbird/odbc/odbc_client_bridge.h"
#undef private
#include "scratchbird/odbc/odbc_driver.h"

namespace {

class FakeUnicodeMetadataBridge : public scratchbird::odbc::OdbcClientBridge {
public:
    SQLRETURN executeSQL(const std::string& /*sql*/,
                         std::vector<std::vector<std::string>>& results,
                         std::vector<scratchbird::odbc::ColumnMetadata>& columns,
                         SQLLEN& rows_affected) override {
        results.clear();
        columns.clear();
        rows_affected = 0;
        return SQL_SUCCESS;
    }
};

class OdbcUnicodeCompatTest : public ::testing::Test {
protected:
    scratchbird::odbc::OdbcEnvironment env_{};
    scratchbird::odbc::OdbcConnection conn_{&env_};
    scratchbird::odbc::OdbcStatement stmt_{&conn_};

    void SetUp() override {
        conn_.connected_ = true;
        conn_.current_database_ = "db";
        conn_.current_schema_ = "public";
        conn_.client_bridge_ = std::make_unique<FakeUnicodeMetadataBridge>();
    }
};

TEST_F(OdbcUnicodeCompatTest, NativeSqlRoundTripAndTruncation) {
    SQLCHAR in_sql[] = "SELECT 1";
    SQLCHAR out_small[5] = {};
    SQLINTEGER out_len = 0;
    EXPECT_EQ(SQLNativeSql(&conn_, in_sql, SQL_NTS, out_small,
                           static_cast<SQLINTEGER>(sizeof(out_small)), &out_len),
              SQL_SUCCESS_WITH_INFO);
    EXPECT_EQ(out_len, 8);
    EXPECT_STREQ(reinterpret_cast<const char*>(out_small), "SELE");

    SQLCHAR out_big[32] = {};
    EXPECT_EQ(SQLNativeSql(&conn_, in_sql, SQL_NTS, out_big,
                           static_cast<SQLINTEGER>(sizeof(out_big)), &out_len),
              SQL_SUCCESS);
    EXPECT_EQ(out_len, 8);
    EXPECT_STREQ(reinterpret_cast<const char*>(out_big), "SELECT 1");
}

TEST_F(OdbcUnicodeCompatTest, CursorNameApisRoundTrip) {
    SQLCHAR cursor_name[] = "CURSOR_A";
    EXPECT_EQ(SQLSetCursorName(&stmt_, cursor_name, SQL_NTS), SQL_SUCCESS);

    SQLCHAR out_name[32] = {};
    SQLSMALLINT out_len = 0;
    EXPECT_EQ(SQLGetCursorName(&stmt_, out_name, static_cast<SQLSMALLINT>(sizeof(out_name)), &out_len),
              SQL_SUCCESS);
    EXPECT_STREQ(reinterpret_cast<const char*>(out_name), "CURSOR_A");
    EXPECT_EQ(out_len, 8);
}

TEST_F(OdbcUnicodeCompatTest, WideNativeSqlAndCursorNameRoundTrip) {
    SQLWCHAR in_sql[] = {'S','E','L','E','C','T',' ','1',0};
    SQLWCHAR out_sql[32] = {};
    SQLINTEGER out_len = 0;
    EXPECT_EQ(SQLNativeSqlW(&conn_, in_sql, SQL_NTS, out_sql, 32, &out_len), SQL_SUCCESS);
    EXPECT_EQ(out_len, 8);
    EXPECT_EQ(out_sql[0], static_cast<SQLWCHAR>('S'));
    EXPECT_EQ(out_sql[1], static_cast<SQLWCHAR>('E'));

    SQLWCHAR cursor_name[] = {'C','U','R','S','O','R','_','W',0};
    EXPECT_EQ(SQLSetCursorNameW(&stmt_, cursor_name, SQL_NTS), SQL_SUCCESS);

    SQLWCHAR out_name[32] = {};
    SQLSMALLINT name_len = 0;
    EXPECT_EQ(SQLGetCursorNameW(&stmt_, out_name, 32, &name_len), SQL_SUCCESS);
    EXPECT_EQ(name_len, 8);
    EXPECT_EQ(out_name[0], static_cast<SQLWCHAR>('C'));
    EXPECT_EQ(out_name[7], static_cast<SQLWCHAR>('W'));
}

TEST_F(OdbcUnicodeCompatTest, OptionWrappersRouteToAttributes) {
    EXPECT_EQ(SQLSetStmtOption(&stmt_, SQL_ATTR_QUERY_TIMEOUT, 9), SQL_SUCCESS);
    SQLULEN query_timeout = 0;
    EXPECT_EQ(SQLGetStmtOption(&stmt_, SQL_ATTR_QUERY_TIMEOUT, &query_timeout), SQL_SUCCESS);
    EXPECT_EQ(query_timeout, 9u);

    EXPECT_EQ(SQLSetConnectOption(&conn_, SQL_ATTR_LOGIN_TIMEOUT, 12), SQL_SUCCESS);
    SQLUINTEGER login_timeout = 0;
    EXPECT_EQ(SQLGetConnectOption(&conn_, SQL_ATTR_LOGIN_TIMEOUT, &login_timeout), SQL_SUCCESS);
    EXPECT_EQ(login_timeout, 12u);
}

TEST_F(OdbcUnicodeCompatTest, WidePrepareAndDiagRecReturnWideDiagnostics) {
    EXPECT_EQ(SQLPrepareW(&stmt_, nullptr, 0), SQL_ERROR);

    SQLWCHAR sql_state[6] = {};
    SQLINTEGER native_error = 0;
    SQLWCHAR message[256] = {};
    SQLSMALLINT message_len = 0;
    EXPECT_EQ(SQLGetDiagRecW(SQL_HANDLE_STMT, &stmt_, 1, sql_state,
                             &native_error, message,
                             static_cast<SQLSMALLINT>(sizeof(message) / sizeof(SQLWCHAR)),
                             &message_len),
              SQL_SUCCESS);
    std::string state_ascii;
    for (int i = 0; i < 5 && sql_state[i] != 0; ++i) {
        state_ascii.push_back(static_cast<char>(sql_state[i] & 0xFF));
    }
    EXPECT_EQ(state_ascii, "HY009");
    EXPECT_GT(message_len, 0);
}

TEST_F(OdbcUnicodeCompatTest, WideCatalogFunctionsRouteToAnsiSurfaces) {
    SQLWCHAR catalog[] = {'d', 'b', 0};
    SQLWCHAR schema[] = {'p', 'u', 'b', 'l', 'i', 'c', 0};
    SQLWCHAR table[] = {'t', '1', 0};
    SQLWCHAR column[] = {'c', '1', 0};
    SQLWCHAR proc[] = {'p', '1', 0};

    EXPECT_EQ(SQLPrimaryKeysW(&stmt_, catalog, SQL_NTS, schema, SQL_NTS, table, SQL_NTS), SQL_SUCCESS);
    EXPECT_EQ(SQLForeignKeysW(&stmt_, catalog, SQL_NTS, schema, SQL_NTS, table, SQL_NTS,
                              catalog, SQL_NTS, schema, SQL_NTS, table, SQL_NTS), SQL_SUCCESS);
    EXPECT_EQ(SQLStatisticsW(&stmt_, catalog, SQL_NTS, schema, SQL_NTS, table, SQL_NTS, 0, 0),
              SQL_SUCCESS);
    EXPECT_EQ(SQLSpecialColumnsW(&stmt_, 0, catalog, SQL_NTS, schema, SQL_NTS, table, SQL_NTS, 0, 0),
              SQL_SUCCESS);
    EXPECT_EQ(SQLProceduresW(&stmt_, catalog, SQL_NTS, schema, SQL_NTS, proc, SQL_NTS), SQL_SUCCESS);
    EXPECT_EQ(SQLProcedureColumnsW(&stmt_, catalog, SQL_NTS, schema, SQL_NTS, proc, SQL_NTS,
                                   column, SQL_NTS),
              SQL_SUCCESS);
    EXPECT_EQ(SQLTablePrivilegesW(&stmt_, catalog, SQL_NTS, schema, SQL_NTS, table, SQL_NTS), SQL_SUCCESS);
    EXPECT_EQ(SQLColumnPrivilegesW(&stmt_, catalog, SQL_NTS, schema, SQL_NTS, table, SQL_NTS,
                                   column, SQL_NTS),
              SQL_SUCCESS);
}

TEST(OdbcUnicodeCompatStandaloneTest, CancelHandleRejectsNonStatement) {
    scratchbird::odbc::OdbcEnvironment env{};
    EXPECT_EQ(SQLCancelHandle(SQL_HANDLE_ENV, &env), SQL_INVALID_HANDLE);
}

TEST(OdbcUnicodeCompatStandaloneTest, CancelHandleOnConnectionRoutesToConnectionCancel) {
    scratchbird::odbc::OdbcEnvironment env{};
    scratchbird::odbc::OdbcConnection conn{&env};

    EXPECT_EQ(SQLCancelHandle(SQL_HANDLE_DBC, &conn), SQL_ERROR);

    SQLCHAR sql_state[6] = {};
    SQLINTEGER native_error = 0;
    SQLCHAR message[256] = {};
    SQLSMALLINT message_len = 0;
    EXPECT_EQ(SQLGetDiagRec(SQL_HANDLE_DBC, &conn, 1, sql_state,
                            &native_error, message,
                            static_cast<SQLSMALLINT>(sizeof(message)),
                            &message_len),
              SQL_SUCCESS);
    EXPECT_STREQ(reinterpret_cast<const char*>(sql_state), "08003");
    EXPECT_GT(message_len, 0);
}

}  // namespace
