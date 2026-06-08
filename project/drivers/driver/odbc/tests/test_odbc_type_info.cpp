// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <cstring>
#include <set>
#include <string>

#include "scratchbird/odbc/odbc_handles.h"

using scratchbird::odbc::OdbcEnvironment;
using scratchbird::odbc::SQL_C_CHAR;
using scratchbird::odbc::SQL_UNKNOWN_TYPE;
using scratchbird::odbc::SQL_VARCHAR;
using scratchbird::odbc::SQL_SUCCESS;

TEST(OdbcTypeInfoTest, ReturnsVarcharInfo) {
    OdbcEnvironment env;
    auto* conn = env.createConnection();
    ASSERT_NE(conn, nullptr);
    auto* stmt = conn->createStatement();
    ASSERT_NE(stmt, nullptr);

    ASSERT_EQ(conn->getTypeInfo(SQL_VARCHAR, stmt), SQL_SUCCESS);

    scratchbird::odbc::SQLSMALLINT col_count = 0;
    EXPECT_EQ(stmt->numResultCols(&col_count), SQL_SUCCESS);
    EXPECT_EQ(col_count, 19);

    ASSERT_EQ(stmt->fetch(), SQL_SUCCESS);

    char type_name[64] = {};
    scratchbird::odbc::SQLLEN out_len = 0;
    EXPECT_EQ(stmt->getData(1, SQL_C_CHAR, type_name, sizeof(type_name), &out_len), SQL_SUCCESS);
    EXPECT_STREQ(type_name, "VARCHAR");
}

TEST(OdbcTypeInfoTest, ReturnsAllTypes) {
    OdbcEnvironment env;
    auto* conn = env.createConnection();
    ASSERT_NE(conn, nullptr);
    auto* stmt = conn->createStatement();
    ASSERT_NE(stmt, nullptr);

    ASSERT_EQ(conn->getTypeInfo(SQL_UNKNOWN_TYPE, stmt), SQL_SUCCESS);
    EXPECT_EQ(stmt->fetch(), SQL_SUCCESS);
}

TEST(OdbcTypeInfoTest, ReturnsExtendedTypeMatrixCoverage) {
    OdbcEnvironment env;
    auto* conn = env.createConnection();
    ASSERT_NE(conn, nullptr);
    auto* stmt = conn->createStatement();
    ASSERT_NE(stmt, nullptr);

    ASSERT_EQ(conn->getTypeInfo(SQL_UNKNOWN_TYPE, stmt), SQL_SUCCESS);

    std::set<std::string> type_names;
    while (true) {
        auto rc = stmt->fetch();
        if (rc == scratchbird::odbc::SQL_NO_DATA) {
            break;
        }
        ASSERT_EQ(rc, SQL_SUCCESS);
        char type_name[64] = {};
        scratchbird::odbc::SQLLEN out_len = 0;
        ASSERT_EQ(stmt->getData(1, SQL_C_CHAR, type_name, sizeof(type_name), &out_len), SQL_SUCCESS);
        type_names.insert(type_name);
    }

    EXPECT_TRUE(type_names.count("MONEY") == 1u);
    EXPECT_TRUE(type_names.count("INTERVAL") == 1u);
    EXPECT_TRUE(type_names.count("INET") == 1u);
    EXPECT_TRUE(type_names.count("INT4RANGE") == 1u);
    EXPECT_TRUE(type_names.count("BYTEA") == 1u);
    EXPECT_TRUE(type_names.count("VECTOR") == 1u);
    EXPECT_TRUE(type_names.count("TSVECTOR") == 1u);
    EXPECT_TRUE(type_names.count("TIMESTAMPTZ") == 1u);
}
