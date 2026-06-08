// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <sql.h>
#include <sqlext.h>

#include <cstdlib>
#include <sstream>
#include <string>

namespace {

std::string diagMessage(SQLSMALLINT handle_type, SQLHANDLE handle) {
    std::ostringstream out;
    SQLSMALLINT record = 1;
    while (true) {
        SQLCHAR sqlstate[6] = {};
        SQLINTEGER native = 0;
        SQLCHAR message[512] = {};
        SQLSMALLINT message_len = 0;
        SQLRETURN rc = SQLGetDiagRec(handle_type,
                                     handle,
                                     record,
                                     sqlstate,
                                     &native,
                                     message,
                                     sizeof(message),
                                     &message_len);
        if (rc == SQL_NO_DATA) {
            break;
        }
        if (!SQL_SUCCEEDED(rc)) {
            break;
        }
        if (record > 1) {
            out << " | ";
        }
        out << "[" << reinterpret_cast<char*>(sqlstate) << "] "
            << reinterpret_cast<char*>(message)
            << " (native=" << native << ")";
        ++record;
    }
    return out.str();
}

void runBiMetadataSmoke(const std::string& conn_str, const char* label) {
    SQLHENV env = SQL_NULL_HENV;
    SQLHDBC dbc = SQL_NULL_HDBC;
    SQLHSTMT stmt = SQL_NULL_HSTMT;

    ASSERT_EQ(SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env), SQL_SUCCESS)
        << label << ": SQLAllocHandle(SQL_HANDLE_ENV) failed";
    ASSERT_EQ(SQLSetEnvAttr(env, SQL_ATTR_ODBC_VERSION,
                            reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0), SQL_SUCCESS)
        << label << ": SQLSetEnvAttr(SQL_ATTR_ODBC_VERSION) failed";
    ASSERT_EQ(SQLAllocHandle(SQL_HANDLE_DBC, env, &dbc), SQL_SUCCESS)
        << label << ": SQLAllocHandle(SQL_HANDLE_DBC) failed";

    SQLCHAR out_conn[1024] = {};
    SQLSMALLINT out_len = 0;
    SQLRETURN conn_rc = SQLDriverConnect(dbc,
                                         nullptr,
                                         reinterpret_cast<SQLCHAR*>(const_cast<char*>(conn_str.c_str())),
                                         SQL_NTS,
                                         out_conn,
                                         sizeof(out_conn),
                                         &out_len,
                                         SQL_DRIVER_NOPROMPT);
    ASSERT_TRUE(SQL_SUCCEEDED(conn_rc))
        << label << ": SQLDriverConnect failed: " << diagMessage(SQL_HANDLE_DBC, dbc);

    SQLCHAR dbms_name[128] = {};
    SQLSMALLINT dbms_name_len = 0;
    ASSERT_TRUE(SQL_SUCCEEDED(SQLGetInfo(dbc,
                                         SQL_DBMS_NAME,
                                         dbms_name,
                                         sizeof(dbms_name),
                                         &dbms_name_len)))
        << label << ": SQLGetInfo(SQL_DBMS_NAME) failed: " << diagMessage(SQL_HANDLE_DBC, dbc);

    ASSERT_EQ(SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt), SQL_SUCCESS)
        << label << ": SQLAllocHandle(SQL_HANDLE_STMT) failed";

    SQLRETURN tables_rc = SQLTables(stmt, nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0);
    ASSERT_TRUE(SQL_SUCCEEDED(tables_rc))
        << label << ": SQLTables failed: " << diagMessage(SQL_HANDLE_STMT, stmt);
    (void)SQLCloseCursor(stmt);

    SQLRETURN columns_rc = SQLColumns(stmt, nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0);
    ASSERT_TRUE(SQL_SUCCEEDED(columns_rc))
        << label << ": SQLColumns failed: " << diagMessage(SQL_HANDLE_STMT, stmt);

    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    SQLDisconnect(dbc);
    SQLFreeHandle(SQL_HANDLE_DBC, dbc);
    SQLFreeHandle(SQL_HANDLE_ENV, env);
}

std::string fetchSingleText(SQLHSTMT stmt, const char* sql) {
    SQLRETURN exec_rc = SQLExecDirect(
        stmt,
        reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)),
        SQL_NTS);
    EXPECT_TRUE(SQL_SUCCEEDED(exec_rc))
        << "SQLExecDirect failed: " << diagMessage(SQL_HANDLE_STMT, stmt);
    if (!SQL_SUCCEEDED(exec_rc)) {
        return {};
    }

    SQLRETURN fetch_rc = SQLFetch(stmt);
    EXPECT_EQ(fetch_rc, SQL_SUCCESS)
        << "SQLFetch failed: " << diagMessage(SQL_HANDLE_STMT, stmt);
    if (fetch_rc != SQL_SUCCESS) {
        return {};
    }

    char value[256] = {};
    SQLLEN value_len = 0;
    SQLRETURN get_rc = SQLGetData(stmt, 1, SQL_C_CHAR, value, sizeof(value), &value_len);
    EXPECT_TRUE(SQL_SUCCEEDED(get_rc))
        << "SQLGetData failed: " << diagMessage(SQL_HANDLE_STMT, stmt);
    SQLCloseCursor(stmt);
    return SQL_SUCCEEDED(get_rc) ? std::string(value) : std::string();
}

}  // namespace

TEST(OdbcExternalRuntimeTest, ConnectsThroughListenerAndQueriesFixtureData) {
    const char* conn_env = std::getenv("SCRATCHBIRD_ODBC_TEST_CONNSTR");
    if (conn_env == nullptr || std::string(conn_env).empty()) {
        GTEST_SKIP() << "SCRATCHBIRD_ODBC_TEST_CONNSTR is not set";
    }

    std::string conn_str(conn_env);

    SQLHENV env = SQL_NULL_HENV;
    SQLHDBC dbc = SQL_NULL_HDBC;
    SQLHSTMT stmt = SQL_NULL_HSTMT;

    ASSERT_EQ(SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env), SQL_SUCCESS);
    ASSERT_EQ(SQLSetEnvAttr(env, SQL_ATTR_ODBC_VERSION,
                            reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0), SQL_SUCCESS);

    ASSERT_EQ(SQLAllocHandle(SQL_HANDLE_DBC, env, &dbc), SQL_SUCCESS);

    SQLCHAR out_conn[1024] = {};
    SQLSMALLINT out_len = 0;
    SQLRETURN conn_rc = SQLDriverConnect(dbc,
                                         nullptr,
                                         reinterpret_cast<SQLCHAR*>(&conn_str[0]),
                                         SQL_NTS,
                                         out_conn,
                                         sizeof(out_conn),
                                         &out_len,
                                         SQL_DRIVER_NOPROMPT);
    ASSERT_TRUE(SQL_SUCCEEDED(conn_rc)) << "SQLDriverConnect failed: " << diagMessage(SQL_HANDLE_DBC, dbc);

    ASSERT_EQ(SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt), SQL_SUCCESS);

    SQLRETURN exec_rc = SQLExecDirect(stmt,
                                      reinterpret_cast<SQLCHAR*>(const_cast<char*>(
                                          "UPDATE users.public.basic_table SET name = name "
                                          "WHERE name = 'baseline'")),
                                      SQL_NTS);
    ASSERT_TRUE(SQL_SUCCEEDED(exec_rc)) << "SQLExecDirect failed: " << diagMessage(SQL_HANDLE_STMT, stmt);
    SQLLEN row_count = 0;
    ASSERT_EQ(SQLRowCount(stmt, &row_count), SQL_SUCCESS);
    EXPECT_GE(row_count, 0);

    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    SQLDisconnect(dbc);
    SQLFreeHandle(SQL_HANDLE_DBC, dbc);
    SQLFreeHandle(SQL_HANDLE_ENV, env);
}

TEST(OdbcExternalRuntimeTest, RollbackLeavesImmediateQueryUsableOnFreshBoundary) {
    const char* conn_env = std::getenv("SCRATCHBIRD_ODBC_TEST_CONNSTR");
    if (conn_env == nullptr || std::string(conn_env).empty()) {
        GTEST_SKIP() << "SCRATCHBIRD_ODBC_TEST_CONNSTR is not set";
    }

    std::string conn_str(conn_env);

    SQLHENV env = SQL_NULL_HENV;
    SQLHDBC dbc = SQL_NULL_HDBC;
    SQLHSTMT stmt = SQL_NULL_HSTMT;

    ASSERT_EQ(SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env), SQL_SUCCESS);
    ASSERT_EQ(SQLSetEnvAttr(env, SQL_ATTR_ODBC_VERSION,
                            reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0), SQL_SUCCESS);
    ASSERT_EQ(SQLAllocHandle(SQL_HANDLE_DBC, env, &dbc), SQL_SUCCESS);

    SQLCHAR out_conn[1024] = {};
    SQLSMALLINT out_len = 0;
    SQLRETURN conn_rc = SQLDriverConnect(dbc,
                                         nullptr,
                                         reinterpret_cast<SQLCHAR*>(&conn_str[0]),
                                         SQL_NTS,
                                         out_conn,
                                         sizeof(out_conn),
                                         &out_len,
                                         SQL_DRIVER_NOPROMPT);
    ASSERT_TRUE(SQL_SUCCEEDED(conn_rc))
        << "SQLDriverConnect failed: " << diagMessage(SQL_HANDLE_DBC, dbc);

    ASSERT_EQ(SQLSetConnectAttr(dbc,
                                SQL_ATTR_AUTOCOMMIT,
                                reinterpret_cast<SQLPOINTER>(static_cast<uintptr_t>(SQL_AUTOCOMMIT_OFF)),
                                0),
              SQL_SUCCESS);

    ASSERT_EQ(SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt), SQL_SUCCESS);

    SQLRETURN exec_rc = SQLExecDirect(
        stmt,
        reinterpret_cast<SQLCHAR*>(const_cast<char*>(
            "UPDATE users.public.basic_table SET name = 'rollback-probe' WHERE name = 'baseline'")),
        SQL_NTS);
    ASSERT_TRUE(SQL_SUCCEEDED(exec_rc))
        << "UPDATE failed: " << diagMessage(SQL_HANDLE_STMT, stmt);
    SQLCloseCursor(stmt);

    ASSERT_EQ(SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_ROLLBACK), SQL_SUCCESS)
        << "Rollback failed: " << diagMessage(SQL_HANDLE_DBC, dbc);

    std::string baseline_name = fetchSingleText(
        stmt,
        "SELECT name FROM users.public.basic_table WHERE name = 'baseline'");
    ASSERT_EQ(baseline_name, "baseline")
        << "Fresh-boundary query did not observe rolled-back state";

    SQLRETURN probe_exec_rc = SQLExecDirect(
        stmt,
        reinterpret_cast<SQLCHAR*>(const_cast<char*>(
            "SELECT name FROM users.public.basic_table WHERE name = 'rollback-probe'")),
        SQL_NTS);
    ASSERT_TRUE(SQL_SUCCEEDED(probe_exec_rc))
        << "Probe select failed: " << diagMessage(SQL_HANDLE_STMT, stmt);
    ASSERT_EQ(SQLFetch(stmt), SQL_NO_DATA)
        << "Rolled-back row should not remain visible after rollback";
    SQLCloseCursor(stmt);

    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    SQLDisconnect(dbc);
    SQLFreeHandle(SQL_HANDLE_DBC, dbc);
    SQLFreeHandle(SQL_HANDLE_ENV, env);
}

TEST(OdbcExternalRuntimeTest, HostedTableauMetadataSmoke) {
    const char* conn_env = std::getenv("SCRATCHBIRD_ODBC_TABLEAU_CONNSTR");
    if (conn_env == nullptr || std::string(conn_env).empty()) {
        GTEST_SKIP() << "SCRATCHBIRD_ODBC_TABLEAU_CONNSTR is not set";
    }
    runBiMetadataSmoke(conn_env, "Tableau");
}

TEST(OdbcExternalRuntimeTest, HostedPowerBiMetadataSmoke) {
    const char* conn_env = std::getenv("SCRATCHBIRD_ODBC_POWERBI_CONNSTR");
    if (conn_env == nullptr || std::string(conn_env).empty()) {
        GTEST_SKIP() << "SCRATCHBIRD_ODBC_POWERBI_CONNSTR is not set";
    }
    runBiMetadataSmoke(conn_env, "Power BI");
}

TEST(OdbcExternalRuntimeTest, HostedExcelMetadataSmoke) {
    const char* conn_env = std::getenv("SCRATCHBIRD_ODBC_EXCEL_CONNSTR");
    if (conn_env == nullptr || std::string(conn_env).empty()) {
        GTEST_SKIP() << "SCRATCHBIRD_ODBC_EXCEL_CONNSTR is not set";
    }
    runBiMetadataSmoke(conn_env, "Excel");
}
