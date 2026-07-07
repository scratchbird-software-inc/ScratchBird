// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#define private public
#include "scratchbird/odbc/odbc_handles.h"
#undef private
#include "scratchbird/odbc/odbc_client_bridge.h"
#include "scratchbird/odbc/statement_cache.h"

namespace {

class FakeStatementCacheBridge : public scratchbird::odbc::OdbcClientBridge {
public:
    SQLRETURN executeSQL(const std::string& sql,
                         std::vector<std::vector<std::string>>& results,
                         std::vector<scratchbird::odbc::ColumnMetadata>& columns,
                         SQLLEN& rows_affected) override {
        (void)columns;
        statements.push_back(sql);
        results.clear();
        rows_affected = 1;
        ++execute_count;
        if (fail_on_call > 0 && execute_count == fail_on_call) {
            return SQL_ERROR;
        }
        return SQL_SUCCESS;
    }

    int fail_on_call{0};
    int execute_count{0};
    std::vector<std::string> statements;
};

class OdbcStatementCacheApiTest : public ::testing::Test {
protected:
    scratchbird::odbc::OdbcEnvironment env_{};
    scratchbird::odbc::OdbcConnection conn_{&env_};
    scratchbird::odbc::OdbcStatement stmt_{&conn_};
    FakeStatementCacheBridge* bridge_{nullptr};

    void SetUp() override {
        conn_.connected_ = true;
        auto bridge = std::make_unique<FakeStatementCacheBridge>();
        bridge_ = bridge.get();
        conn_.client_bridge_ = std::move(bridge);
    }
};

TEST_F(OdbcStatementCacheApiTest, BatchExecuteSupportsParameterizedOperations) {
    SQLCHAR sql_one[] = "INSERT INTO cache_log (id, note) VALUES (?, ?)";
    SQLCHAR sql_two[] = "INSERT INTO cache_log (id, note) VALUES (?, ?)";

    SQLCHAR id_one[] = "1";
    SQLCHAR note_one[] = "alpha";
    SQLPOINTER params_one[] = {id_one, note_one};
    SQLLEN lens_one[] = {SQL_NTS, SQL_NTS};

    SQLCHAR id_two[] = "2";
    SQLCHAR note_two[] = "o'malley";
    SQLPOINTER params_two[] = {id_two, note_two};
    SQLLEN lens_two[] = {SQL_NTS, SQL_NTS};

    sb_odbc_batch_op ops[2] = {};
    ops[0].sql = sql_one;
    ops[0].sql_len = SQL_NTS;
    ops[0].params = params_one;
    ops[0].param_lens = lens_one;
    ops[0].param_count = 2;

    ops[1].sql = sql_two;
    ops[1].sql_len = SQL_NTS;
    ops[1].params = params_two;
    ops[1].param_lens = lens_two;
    ops[1].param_count = 2;

    SQLULEN affected = 0;
    SQLULEN error_index = 99;
    auto rc = sb_odbc_batch_execute(&stmt_, ops, 2, &affected, &error_index);
    EXPECT_EQ(rc, SQL_SUCCESS);
    EXPECT_EQ(affected, 2u);
    EXPECT_EQ(error_index, 0u);

    ASSERT_EQ(bridge_->statements.size(), 2u);
    EXPECT_NE(bridge_->statements[0].find("VALUES ('1','alpha')"), std::string::npos);
    EXPECT_NE(bridge_->statements[1].find("VALUES ('2','o''malley')"), std::string::npos);
}

TEST_F(OdbcStatementCacheApiTest, BatchExecuteReportsFirstFailureIndexAndPartialAffectCount) {
    bridge_->fail_on_call = 2;

    SQLCHAR sql_one[] = "INSERT INTO cache_log (id, note) VALUES (?, ?)";
    SQLCHAR sql_two[] = "INSERT INTO cache_log (id, note) VALUES (?, ?)";
    SQLCHAR id_one[] = "1";
    SQLCHAR note_one[] = "alpha";
    SQLPOINTER params_one[] = {id_one, note_one};
    SQLLEN lens_one[] = {SQL_NTS, SQL_NTS};
    SQLCHAR id_two[] = "2";
    SQLCHAR note_two[] = "beta";
    SQLPOINTER params_two[] = {id_two, note_two};
    SQLLEN lens_two[] = {SQL_NTS, SQL_NTS};

    sb_odbc_batch_op ops[2] = {};
    ops[0] = {sql_one, SQL_NTS, params_one, lens_one, 2};
    ops[1] = {sql_two, SQL_NTS, params_two, lens_two, 2};

    SQLULEN affected = 0;
    SQLULEN error_index = 99;
    auto rc = sb_odbc_batch_execute(&stmt_, ops, 2, &affected, &error_index);
    EXPECT_EQ(rc, SQL_ERROR);
    EXPECT_EQ(error_index, 1u);
    EXPECT_EQ(affected, 1u);
}

TEST_F(OdbcStatementCacheApiTest, BatchExecuteHonorsExplicitParameterTypes) {
    SQLCHAR sql[] = "INSERT INTO cache_typed (id, payload) VALUES (?, ?)";

    SQLINTEGER id = 42;
    unsigned char payload[] = {0xDE, 0xAD};
    SQLPOINTER params[] = {&id, payload};
    SQLLEN lens[] = {0, static_cast<SQLLEN>(sizeof(payload))};
    SQLSMALLINT c_types[] = {SQL_C_LONG, SQL_C_BINARY};
    SQLSMALLINT sql_types[] = {SQL_INTEGER, SQL_VARBINARY};

    sb_odbc_batch_op op = {};
    op.sql = sql;
    op.sql_len = SQL_NTS;
    op.params = params;
    op.param_lens = lens;
    op.param_count = 2;
    op.param_c_types = c_types;
    op.param_sql_types = sql_types;

    SQLULEN affected = 0;
    SQLULEN error_index = 99;
    auto rc = sb_odbc_batch_execute(&stmt_, &op, 1, &affected, &error_index);
    EXPECT_EQ(rc, SQL_SUCCESS);
    EXPECT_EQ(affected, 1u);
    EXPECT_EQ(error_index, 0u);

    ASSERT_EQ(bridge_->statements.size(), 1u);
    EXPECT_NE(bridge_->statements[0].find("INSERT INTO cache_typed"), std::string::npos);
    EXPECT_NE(bridge_->statements[0].find("VALUES (42,X'DEAD')"), std::string::npos);
}

TEST_F(OdbcStatementCacheApiTest, BulkInsertSupportsColumnMajorStringArrays) {
    SQLCHAR table[] = "cache_bulk";
    SQLCHAR col_id[] = "id";
    SQLCHAR col_note[] = "note";
    SQLCHAR* columns[] = {col_id, col_note};

    SQLCHAR id1[] = "1";
    SQLCHAR id2[] = "2";
    SQLCHAR id3[] = "3";
    SQLCHAR* ids[] = {id1, id2, id3};

    SQLCHAR note1[] = "alpha";
    SQLCHAR note3[] = "gamma-tail";
    SQLCHAR* notes[] = {note1, nullptr, note3};

    SQLPOINTER data[] = {ids, notes};
    SQLLEN lens[] = {
        SQL_NTS, SQL_NTS, SQL_NTS,
        SQL_NTS, SQL_NULL_DATA, 5
    };

    SQLULEN inserted = 0;
    auto rc = sb_odbc_bulk_insert(&stmt_, table, columns, 2, data, lens, 3, &inserted);
    EXPECT_EQ(rc, SQL_SUCCESS);
    EXPECT_EQ(inserted, 3u);

    ASSERT_EQ(bridge_->statements.size(), 3u);
    EXPECT_NE(bridge_->statements[0].find("INSERT INTO cache_bulk"), std::string::npos);
    EXPECT_NE(bridge_->statements[0].find("VALUES ('1','alpha')"), std::string::npos);
    EXPECT_NE(bridge_->statements[1].find("INSERT INTO cache_bulk"), std::string::npos);
    EXPECT_NE(bridge_->statements[1].find("VALUES ('2',NULL)"), std::string::npos);
    EXPECT_NE(bridge_->statements[2].find("INSERT INTO cache_bulk"), std::string::npos);
    EXPECT_NE(bridge_->statements[2].find("VALUES ('3','gamma')"), std::string::npos);
}

TEST_F(OdbcStatementCacheApiTest, BulkInsertExSupportsTypedColumns) {
    SQLCHAR table[] = "cache_bulk_typed";
    SQLCHAR col_id[] = "id";
    SQLCHAR col_payload[] = "payload";
    SQLCHAR* columns[] = {col_id, col_payload};

    SQLINTEGER id1 = 101;
    SQLINTEGER id2 = 202;
    SQLPOINTER id_values[] = {&id1, &id2};

    unsigned char payload1[] = {0xCA, 0xFE};
    unsigned char payload2[] = {0xBA, 0xBE};
    SQLPOINTER payload_values[] = {payload1, payload2};

    SQLPOINTER data[] = {id_values, payload_values};
    SQLLEN lens[] = {
        0, 0,
        2, 2
    };
    SQLSMALLINT c_types[] = {SQL_C_LONG, SQL_C_BINARY};
    SQLSMALLINT sql_types[] = {SQL_INTEGER, SQL_VARBINARY};

    SQLULEN inserted = 0;
    auto rc = sb_odbc_bulk_insert_ex(
        &stmt_, table, columns, 2, data, lens,
        c_types, sql_types, nullptr, nullptr,
        2, &inserted);
    EXPECT_EQ(rc, SQL_SUCCESS);
    EXPECT_EQ(inserted, 2u);

    ASSERT_EQ(bridge_->statements.size(), 2u);
    EXPECT_NE(bridge_->statements[0].find("INSERT INTO cache_bulk_typed"), std::string::npos);
    EXPECT_NE(bridge_->statements[0].find("VALUES (101,X'CAFE')"), std::string::npos);
    EXPECT_NE(bridge_->statements[1].find("INSERT INTO cache_bulk_typed"), std::string::npos);
    EXPECT_NE(bridge_->statements[1].find("VALUES (202,X'BABE')"), std::string::npos);
}

TEST_F(OdbcStatementCacheApiTest, BulkInsertWithZeroRowsIsNoOp) {
    SQLCHAR table[] = "cache_bulk";
    SQLCHAR col_id[] = "id";
    SQLCHAR* columns[] = {col_id};
    SQLCHAR* ids[] = {};
    SQLPOINTER data[] = {ids};
    SQLULEN inserted = 42;

    auto rc = sb_odbc_bulk_insert(&stmt_, table, columns, 1, data, nullptr, 0, &inserted);
    EXPECT_EQ(rc, SQL_SUCCESS);
    EXPECT_EQ(inserted, 0u);
    EXPECT_TRUE(bridge_->statements.empty());
}

}  // namespace
