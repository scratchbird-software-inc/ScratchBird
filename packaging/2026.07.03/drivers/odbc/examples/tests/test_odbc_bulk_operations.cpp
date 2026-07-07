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
#include "scratchbird/odbc/odbc_client_bridge.h"
#undef private

namespace {

class FakeBulkClientBridge : public scratchbird::odbc::OdbcClientBridge {
public:
    SQLRETURN executeSQL(const std::string& sql,
                         std::vector<std::vector<std::string>>& results,
                         std::vector<scratchbird::odbc::ColumnMetadata>& columns,
                         SQLLEN& rows_affected) override {
        (void)columns;
        (void)rows_affected;

        statements.push_back(sql);
        results.clear();
        rows_affected = 0;
        return SQL_SUCCESS;
    }

    std::vector<std::string> statements;
};

class OdbcBulkOperationsTest : public ::testing::Test {
protected:
    scratchbird::odbc::OdbcEnvironment env_{};
    scratchbird::odbc::OdbcConnection conn_{&env_};
    scratchbird::odbc::OdbcStatement stmt_{&conn_};
    FakeBulkClientBridge* bridge_{nullptr};

    void SetUp() override {
        conn_.connected_ = true;
        auto bridge = std::make_unique<FakeBulkClientBridge>();
        bridge_ = bridge.get();
        conn_.client_bridge_ = std::move(bridge);
    }
};

class OdbcFlakyBulkClientBridge : public scratchbird::odbc::OdbcClientBridge {
public:
    explicit OdbcFlakyBulkClientBridge(SQLULEN fail_row)
        : fail_row_(fail_row) {}

    SQLRETURN executeSQL(const std::string& sql,
                        std::vector<std::vector<std::string>>& results,
                        std::vector<scratchbird::odbc::ColumnMetadata>& columns,
                        SQLLEN& rows_affected) override {
        (void)columns;
        (void)rows_affected;

        statements.push_back(sql);
        results.clear();
        rows_affected = 0;
        ++executed_rows_;

        if (executed_rows_ == fail_row_) {
            last_status_ = scratchbird::core::Status::UNIQUE_VIOLATION;
            last_error_ = "duplicate key value violates unique constraint";
            return SQL_ERROR;
        }
        last_status_ = scratchbird::core::Status::OK;
        last_error_.clear();
        return SQL_SUCCESS;
    }

    SQLRETURN rollback() override {
        ++rollback_calls_;
        return rollback_result_;
    }

    SQLULEN fail_row_;
    SQLULEN executed_rows_{0};
    SQLRETURN rollback_result_{SQL_SUCCESS};
    SQLULEN rollback_calls_{0};
    std::vector<std::string> statements;
};

TEST_F(OdbcBulkOperationsTest, BulkOperationsExecutesEachRowInOrder) {
    const char* sql = "INSERT INTO bulk_load (id, note, is_active) VALUES (?, ?, ?)";
    ASSERT_EQ(stmt_.prepare(reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)), SQL_NTS), SQL_SUCCESS);

    SQLINTEGER ids[] = {10, 20, 30};
    SQLLEN id_ind[] = {0, 0, 0};

    char notes[3][16] = {};
    std::strcpy(notes[0], "alpha");
    std::strcpy(notes[1], "skip");
    std::strcpy(notes[2], "gamma");
    SQLLEN note_ind[] = {SQL_NTS, SQL_NULL_DATA, SQL_NTS};

    SQLCHAR flags[] = {1, 0, 1};
    SQLLEN flag_ind[] = {0, 0, 0};

    ASSERT_EQ(stmt_.bindParameter(1, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER,
                                 0, 0, ids, 0, id_ind), SQL_SUCCESS);
    ASSERT_EQ(stmt_.bindParameter(2, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
                                 sizeof(notes[0]), 0, notes, sizeof(notes[0]), note_ind),
              SQL_SUCCESS);
    ASSERT_EQ(stmt_.bindParameter(3, SQL_PARAM_INPUT, SQL_C_BIT, SQL_BIT,
                                 0, 0, flags, 0, flag_ind), SQL_SUCCESS);

    SQLULEN paramset_size = 3;
    SQLUSMALLINT param_status[3] = {0};
    SQLULEN processed = 0;
    SQLULEN fetched = 0;

    ASSERT_EQ(stmt_.setAttribute(SQL_ATTR_PARAMSET_SIZE, reinterpret_cast<SQLPOINTER>(paramset_size), 0),
              SQL_SUCCESS);
    ASSERT_EQ(stmt_.setAttribute(SQL_ATTR_PARAM_STATUS_PTR, param_status, 0), SQL_SUCCESS);
    ASSERT_EQ(stmt_.setAttribute(SQL_ATTR_PARAMS_PROCESSED_PTR, &processed, 0), SQL_SUCCESS);
    ASSERT_EQ(stmt_.setAttribute(SQL_ATTR_ROWS_FETCHED_PTR, &fetched, 0), SQL_SUCCESS);

    EXPECT_EQ(stmt_.bulkOperations(SQL_ADD), SQL_SUCCESS);

    ASSERT_EQ(bridge_->statements.size(), 3u);
    EXPECT_NE(bridge_->statements[0].find("VALUES (10,'alpha',1)"), std::string::npos);
    EXPECT_NE(bridge_->statements[1].find("VALUES (20,NULL,0)"), std::string::npos);
    EXPECT_NE(bridge_->statements[2].find("VALUES (30,'gamma',1)"), std::string::npos);

    EXPECT_EQ(param_status[0], SQL_PARAM_SUCCESS);
    EXPECT_EQ(param_status[1], SQL_PARAM_SUCCESS);
    EXPECT_EQ(param_status[2], SQL_PARAM_SUCCESS);
    EXPECT_EQ(processed, 3u);
    EXPECT_EQ(fetched, 3u);
    EXPECT_EQ(stmt_.row_count_, 3u);
}

TEST_F(OdbcBulkOperationsTest, BulkOperationsUsesBindOffsetInArrayAddressing) {
    const char* sql = "UPDATE bulk_flags SET value = ? WHERE id = ?";
    ASSERT_EQ(stmt_.prepare(reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)), SQL_NTS), SQL_SUCCESS);

    SQLINTEGER flags[] = {2, 4, 6, 8};
    SQLINTEGER ids[] = {10, 20, 30, 40};
    SQLLEN ind_flags[] = {0, 0, 0, 0};
    SQLLEN ind_ids[] = {0, 0, 0, 0};

    ASSERT_EQ(stmt_.bindParameter(1, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER,
                                 0, 0, flags, 0, ind_flags), SQL_SUCCESS);
    ASSERT_EQ(stmt_.bindParameter(2, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER,
                                 0, 0, ids, 0, ind_ids), SQL_SUCCESS);

    SQLLEN bind_offset = sizeof(SQLINTEGER);
    SQLULEN paramset_size = 3;
    SQLUSMALLINT param_status[3] = {0};
    SQLULEN processed = 0;
    SQLULEN fetched = 0;

    ASSERT_EQ(stmt_.setAttribute(SQL_ATTR_PARAM_BIND_OFFSET_PTR, &bind_offset, 0), SQL_SUCCESS);
    ASSERT_EQ(stmt_.setAttribute(SQL_ATTR_PARAMSET_SIZE, reinterpret_cast<SQLPOINTER>(paramset_size), 0),
              SQL_SUCCESS);
    ASSERT_EQ(stmt_.setAttribute(SQL_ATTR_PARAM_STATUS_PTR, param_status, 0), SQL_SUCCESS);
    ASSERT_EQ(stmt_.setAttribute(SQL_ATTR_PARAMS_PROCESSED_PTR, &processed, 0), SQL_SUCCESS);
    ASSERT_EQ(stmt_.setAttribute(SQL_ATTR_ROWS_FETCHED_PTR, &fetched, 0), SQL_SUCCESS);

    EXPECT_EQ(stmt_.bulkOperations(SQL_ADD), SQL_SUCCESS);

    ASSERT_EQ(bridge_->statements.size(), 3u);
    EXPECT_NE(bridge_->statements[0].find("UPDATE bulk_flags SET value = 2 WHERE id = 10"), std::string::npos);
    EXPECT_NE(bridge_->statements[1].find("UPDATE bulk_flags SET value = 4 WHERE id = 20"), std::string::npos);
    EXPECT_NE(bridge_->statements[2].find("UPDATE bulk_flags SET value = 6 WHERE id = 30"), std::string::npos);
    EXPECT_NE(bridge_->statements[2].find("UPDATE bulk_flags SET value = 6 WHERE id = 30"), std::string::npos);

    EXPECT_EQ(processed, 3u);
    EXPECT_EQ(fetched, 3u);
}

TEST_F(OdbcBulkOperationsTest, BulkOperationsRejectsUnsupportedOperationCode) {
    ASSERT_EQ(stmt_.setAttribute(SQL_ATTR_PARAMSET_SIZE,
                                 reinterpret_cast<SQLPOINTER>(static_cast<SQLULEN>(1)), 0),
              SQL_SUCCESS);
    EXPECT_EQ(stmt_.bulkOperations(99), SQL_ERROR);
}

TEST_F(OdbcBulkOperationsTest, BulkOperationsSupportsUpdateAndDeleteByBookmarkCodes) {
    const char* sql = "DELETE FROM bulk_flags WHERE id = ?";
    ASSERT_EQ(stmt_.prepare(reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)), SQL_NTS), SQL_SUCCESS);

    SQLINTEGER ids[] = {10, 20};
    SQLLEN ind[] = {0, 0};

    ASSERT_EQ(stmt_.bindParameter(1, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER,
                                 0, 0, ids, 0, ind), SQL_SUCCESS);

    SQLULEN paramset_size = 2;
    ASSERT_EQ(stmt_.setAttribute(SQL_ATTR_PARAMSET_SIZE,
                                 reinterpret_cast<SQLPOINTER>(paramset_size), 0),
              SQL_SUCCESS);

    EXPECT_EQ(stmt_.bulkOperations(SQL_DELETE_BY_BOOKMARK), SQL_SUCCESS);
    EXPECT_EQ(stmt_.bulkOperations(SQL_UPDATE_BY_BOOKMARK), SQL_SUCCESS);
    EXPECT_EQ(bridge_->statements.size(), 4u);
}

TEST_F(OdbcBulkOperationsTest, BulkOperationsSupportsFetchByBookmarkWhenAvailable) {
#ifdef SQL_FETCH_BY_BOOKMARK
    stmt_.rows_ = {
        {"one"},
        {"two"},
        {"three"}
    };
    stmt_.columns_ = {
        {"id", "INTEGER", "", "", "", "", SQL_INTEGER, 10, 0, SQL_NO_NULLS,
         false, false, true, SQL_PRED_BASIC, 10, 10}
    };
    stmt_.has_results_ = true;
    stmt_.current_row_ = 0;

    BOOKMARK bookmark = 2;
    SQLULEN rows_fetched = 0;
    SQLUSMALLINT row_status[1] = {0};

    ASSERT_EQ(stmt_.setAttribute(SQL_ATTR_FETCH_BOOKMARK_PTR, &bookmark, 0), SQL_SUCCESS);
    ASSERT_EQ(stmt_.setAttribute(SQL_ATTR_ROWS_FETCHED_PTR, &rows_fetched, 0), SQL_SUCCESS);
    ASSERT_EQ(stmt_.setAttribute(SQL_ATTR_ROW_STATUS_PTR, row_status, 0), SQL_SUCCESS);

    EXPECT_EQ(stmt_.bulkOperations(SQL_FETCH_BY_BOOKMARK), SQL_SUCCESS);
    EXPECT_EQ(stmt_.current_row_, 2u);
    EXPECT_EQ(rows_fetched, 1u);
    EXPECT_EQ(row_status[0], SQL_ROW_SUCCESS);

    bookmark = 100;
    EXPECT_EQ(stmt_.bulkOperations(SQL_FETCH_BY_BOOKMARK), SQL_NO_DATA);
    EXPECT_EQ(rows_fetched, 0u);
    EXPECT_EQ(row_status[0], SQL_ROW_NOROW);
#else
    GTEST_SKIP() << "SQL_FETCH_BY_BOOKMARK is not available in this ODBC header set";
#endif
}

TEST_F(OdbcBulkOperationsTest, BulkOperationsNoRowsIsNoOp) {
    const char* sql = "DELETE FROM bulk_load WHERE id = ?";
    ASSERT_EQ(stmt_.prepare(reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)), SQL_NTS), SQL_SUCCESS);

    SQLUSMALLINT param_status[4] = {0};
    SQLULEN processed = 0;
    SQLULEN fetched = 0;

    ASSERT_EQ(stmt_.setAttribute(SQL_ATTR_PARAMSET_SIZE,
                                 reinterpret_cast<SQLPOINTER>(static_cast<SQLULEN>(0)), 0),
              SQL_SUCCESS);
    ASSERT_EQ(stmt_.setAttribute(SQL_ATTR_PARAM_STATUS_PTR, param_status, 0), SQL_SUCCESS);
    ASSERT_EQ(stmt_.setAttribute(SQL_ATTR_PARAMS_PROCESSED_PTR, &processed, 0), SQL_SUCCESS);
    ASSERT_EQ(stmt_.setAttribute(SQL_ATTR_ROWS_FETCHED_PTR, &fetched, 0), SQL_SUCCESS);

    EXPECT_EQ(stmt_.bulkOperations(SQL_ADD), SQL_SUCCESS);
    EXPECT_EQ(processed, 0u);
    EXPECT_EQ(fetched, 0u);
    EXPECT_EQ(param_status[0], 0u);
    EXPECT_TRUE(bridge_->statements.empty());
}

TEST_F(OdbcBulkOperationsTest, BulkOperationsSupportsRowWiseBindingMode) {
    const char* sql = "UPDATE bulk_flags SET value = ? WHERE id = ?";
    ASSERT_EQ(stmt_.prepare(reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)), SQL_NTS), SQL_SUCCESS);

    struct RowBinding {
        SQLINTEGER value;
        SQLINTEGER id;
        SQLLEN value_ind;
        SQLLEN id_ind;
    };
    RowBinding rows[] = {
        {1, 10, 0, 0},
        {2, 20, 0, 0},
    };

    ASSERT_EQ(stmt_.bindParameter(1, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER,
                                 0, 0, &rows[0].value, 0, &rows[0].value_ind), SQL_SUCCESS);
    ASSERT_EQ(stmt_.bindParameter(2, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER,
                                 0, 0, &rows[0].id, 0, &rows[0].id_ind), SQL_SUCCESS);

    SQLULEN paramset_size = 2;
    ASSERT_EQ(stmt_.setAttribute(SQL_ATTR_PARAMSET_SIZE,
                                 reinterpret_cast<SQLPOINTER>(paramset_size), 0),
              SQL_SUCCESS);
    // Row-wise parameter array binding uses a non-zero structure size stride.
    ASSERT_EQ(stmt_.setAttribute(SQL_ATTR_PARAM_BIND_TYPE,
                                 reinterpret_cast<SQLPOINTER>(static_cast<SQLULEN>(sizeof(RowBinding))),
                                 0),
              SQL_SUCCESS);

    EXPECT_EQ(stmt_.bulkOperations(SQL_ADD), SQL_SUCCESS);
    ASSERT_EQ(bridge_->statements.size(), 2u);
    EXPECT_NE(bridge_->statements[0].find("UPDATE bulk_flags SET value = 1 WHERE id = 10"), std::string::npos);
    EXPECT_NE(bridge_->statements[1].find("UPDATE bulk_flags SET value = 2 WHERE id = 20"), std::string::npos);
}

TEST_F(OdbcBulkOperationsTest, BulkOperationsSupportsDataAtExecAcrossArrayRows) {
    const char* sql = "INSERT INTO bulk_stream (id, payload) VALUES (?, ?)";
    ASSERT_EQ(stmt_.prepare(reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)), SQL_NTS), SQL_SUCCESS);

    SQLINTEGER ids[] = {1, 2};
    SQLLEN id_ind[] = {0, 0};
    char payload_buffer[2][8] = {};
    SQLLEN payload_ind[] = {SQL_DATA_AT_EXEC, SQL_DATA_AT_EXEC};

    ASSERT_EQ(stmt_.bindParameter(1, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER,
                                 0, 0, ids, 0, id_ind), SQL_SUCCESS);
    ASSERT_EQ(stmt_.bindParameter(2, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
                                 sizeof(payload_buffer[0]), 0,
                                 payload_buffer, sizeof(payload_buffer[0]), payload_ind),
              SQL_SUCCESS);

    SQLULEN paramset_size = 2;
    SQLUSMALLINT param_status[2] = {0, 0};
    SQLULEN processed = 0;
    SQLULEN fetched = 0;
    ASSERT_EQ(stmt_.setAttribute(SQL_ATTR_PARAMSET_SIZE, reinterpret_cast<SQLPOINTER>(paramset_size), 0),
              SQL_SUCCESS);
    ASSERT_EQ(stmt_.setAttribute(SQL_ATTR_PARAM_STATUS_PTR, param_status, 0), SQL_SUCCESS);
    ASSERT_EQ(stmt_.setAttribute(SQL_ATTR_PARAMS_PROCESSED_PTR, &processed, 0), SQL_SUCCESS);
    ASSERT_EQ(stmt_.setAttribute(SQL_ATTR_ROWS_FETCHED_PTR, &fetched, 0), SQL_SUCCESS);

    EXPECT_EQ(stmt_.bulkOperations(SQL_ADD), SQL_NEED_DATA);

    SQLPOINTER token = nullptr;
    EXPECT_EQ(stmt_.paramData(&token), SQL_NEED_DATA);
    ASSERT_NE(token, nullptr);
    const char row_one[] = "row-one";
    EXPECT_EQ(stmt_.putData(reinterpret_cast<SQLPOINTER>(const_cast<char*>(row_one)),
                           static_cast<SQLLEN>(sizeof(row_one) - 1)),
              SQL_SUCCESS);
    EXPECT_EQ(stmt_.putData(nullptr, 0), SQL_SUCCESS);
    EXPECT_EQ(stmt_.paramData(&token), SQL_SUCCESS);
    EXPECT_EQ(token, nullptr);

    EXPECT_EQ(stmt_.bulkOperations(SQL_ADD), SQL_NEED_DATA);
    EXPECT_EQ(stmt_.paramData(&token), SQL_NEED_DATA);
    ASSERT_NE(token, nullptr);
    const char row_two[] = "row-two";
    EXPECT_EQ(stmt_.putData(reinterpret_cast<SQLPOINTER>(const_cast<char*>(row_two)),
                           static_cast<SQLLEN>(sizeof(row_two) - 1)),
              SQL_SUCCESS);
    EXPECT_EQ(stmt_.putData(nullptr, 0), SQL_SUCCESS);
    EXPECT_EQ(stmt_.paramData(&token), SQL_SUCCESS);
    EXPECT_EQ(token, nullptr);

    EXPECT_EQ(stmt_.bulkOperations(SQL_ADD), SQL_SUCCESS);

    ASSERT_EQ(bridge_->statements.size(), 2u);
    EXPECT_NE(bridge_->statements[0].find("VALUES (1,'row-one')"), std::string::npos);
    EXPECT_NE(bridge_->statements[1].find("VALUES (2,'row-two')"), std::string::npos);
    EXPECT_EQ(param_status[0], SQL_PARAM_SUCCESS);
    EXPECT_EQ(param_status[1], SQL_PARAM_SUCCESS);
    EXPECT_EQ(processed, 2u);
    EXPECT_EQ(fetched, 2u);
}

TEST_F(OdbcBulkOperationsTest, BulkOperationsPartialFailureStopsExecution) {
    const char* sql = "INSERT INTO bulk_audit (id, note) VALUES (?, ?)";
    ASSERT_EQ(stmt_.prepare(reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)), SQL_NTS), SQL_SUCCESS);

    SQLINTEGER ids[] = {1, 2, 3};
    SQLLEN id_ind[] = {0, 0, 0};
    char notes[3][12] = {};
    std::strcpy(notes[0], "a");
    std::strcpy(notes[1], "b");
    std::strcpy(notes[2], "c");
    SQLLEN note_ind[] = {0, 0, 0};

    ASSERT_EQ(stmt_.bindParameter(1, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER,
                                 0, 0, ids, 0, id_ind),
              SQL_SUCCESS);
    ASSERT_EQ(stmt_.bindParameter(2, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
                                 sizeof(notes[0]), 0, notes, sizeof(notes[0]), note_ind),
              SQL_SUCCESS);

    auto flaky_bridge = std::make_unique<OdbcFlakyBulkClientBridge>(2);
    auto* flaky_ptr = flaky_bridge.get();
    conn_.client_bridge_ = std::move(flaky_bridge);

    SQLULEN paramset_size = 3;
    SQLUSMALLINT param_status[3] = {0};
    SQLULEN processed = 0;
    SQLULEN fetched = 0;
    ASSERT_EQ(stmt_.setAttribute(SQL_ATTR_PARAMSET_SIZE, reinterpret_cast<SQLPOINTER>(paramset_size), 0),
              SQL_SUCCESS);
    ASSERT_EQ(stmt_.setAttribute(SQL_ATTR_PARAM_STATUS_PTR, param_status, 0), SQL_SUCCESS);
    ASSERT_EQ(stmt_.setAttribute(SQL_ATTR_PARAMS_PROCESSED_PTR, &processed, 0), SQL_SUCCESS);
    ASSERT_EQ(stmt_.setAttribute(SQL_ATTR_ROWS_FETCHED_PTR, &fetched, 0), SQL_SUCCESS);

    EXPECT_EQ(stmt_.bulkOperations(SQL_ADD), SQL_ERROR);
    EXPECT_EQ(processed, 1u);
    EXPECT_EQ(fetched, 1u);
    EXPECT_EQ(param_status[0], SQL_PARAM_SUCCESS);
    EXPECT_EQ(param_status[1], SQL_PARAM_ERROR);
    EXPECT_EQ(param_status[2], 0u);
    EXPECT_EQ(flaky_ptr->statements.size(), 2u);
    EXPECT_EQ(flaky_ptr->rollback_calls_, 0u);
    EXPECT_EQ(conn_.client_bridge_.get(), flaky_ptr);
}

TEST_F(OdbcBulkOperationsTest, BulkOperationsReportsSuccessWithInfoForTruncatedParameterData) {
    const char* sql = "INSERT INTO bulk_errors (note, id) VALUES (?, ?)";
    ASSERT_EQ(stmt_.prepare(reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)), SQL_NTS), SQL_SUCCESS);

    SQLINTEGER ids[] = {7};
    SQLLEN id_ind[] = {0};
    char notes[1][16] = {};
    std::strcpy(notes[0], "overflow");
    SQLLEN note_ind[] = {7};  // > buffer declared for this column binding

    ASSERT_EQ(stmt_.bindParameter(1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
                                 4, 0, notes, 0, note_ind),
              SQL_SUCCESS);
    ASSERT_EQ(stmt_.bindParameter(2, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER,
                                 0, 0, ids, 0, id_ind),
              SQL_SUCCESS);

    SQLULEN paramset_size = 1;
    SQLUSMALLINT param_status[1] = {0};
    SQLULEN processed = 0;
    SQLULEN fetched = 0;
    ASSERT_EQ(stmt_.setAttribute(SQL_ATTR_PARAMSET_SIZE,
                                 reinterpret_cast<SQLPOINTER>(paramset_size), 0),
              SQL_SUCCESS);
    ASSERT_EQ(stmt_.setAttribute(SQL_ATTR_PARAM_STATUS_PTR, param_status, 0), SQL_SUCCESS);
    ASSERT_EQ(stmt_.setAttribute(SQL_ATTR_PARAMS_PROCESSED_PTR, &processed, 0), SQL_SUCCESS);
    ASSERT_EQ(stmt_.setAttribute(SQL_ATTR_ROWS_FETCHED_PTR, &fetched, 0), SQL_SUCCESS);
    EXPECT_EQ(stmt_.bulkOperations(SQL_ADD), SQL_SUCCESS_WITH_INFO);
    ASSERT_EQ(bridge_->statements.size(), 1u);
    EXPECT_NE(bridge_->statements[0].find("INSERT INTO bulk_errors (note,id) VALUES ('overflo',7)"),
              std::string::npos);
    EXPECT_EQ(param_status[0], SQL_PARAM_SUCCESS_WITH_INFO);
    EXPECT_EQ(processed, 1u);
    EXPECT_EQ(fetched, 1u);
}

TEST_F(OdbcBulkOperationsTest, BulkOperationsPartialFailureOnFirstRowLeavesLaterRowsUnprocessed) {
    const char* sql = "INSERT INTO bulk_audit (id, note) VALUES (?, ?)";
    ASSERT_EQ(stmt_.prepare(reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)), SQL_NTS), SQL_SUCCESS);

    SQLINTEGER ids[] = {1, 2, 3};
    SQLLEN id_ind[] = {0, 0, 0};
    char notes[3][12] = {};
    std::strcpy(notes[0], "a");
    std::strcpy(notes[1], "b");
    std::strcpy(notes[2], "c");
    SQLLEN note_ind[] = {0, 0, 0};

    ASSERT_EQ(stmt_.bindParameter(1, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER,
                                 0, 0, ids, 0, id_ind),
              SQL_SUCCESS);
    ASSERT_EQ(stmt_.bindParameter(2, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
                                 sizeof(notes[0]), 0, notes, sizeof(notes[0]), note_ind),
              SQL_SUCCESS);

    auto flaky_bridge = std::make_unique<OdbcFlakyBulkClientBridge>(1);
    auto* flaky_ptr = flaky_bridge.get();
    conn_.client_bridge_ = std::move(flaky_bridge);

    SQLULEN paramset_size = 3;
    SQLUSMALLINT param_status[3] = {0};
    SQLULEN processed = 0;
    SQLULEN fetched = 0;
    ASSERT_EQ(stmt_.setAttribute(SQL_ATTR_PARAMSET_SIZE, reinterpret_cast<SQLPOINTER>(paramset_size), 0),
              SQL_SUCCESS);
    ASSERT_EQ(stmt_.setAttribute(SQL_ATTR_PARAM_STATUS_PTR, param_status, 0), SQL_SUCCESS);
    ASSERT_EQ(stmt_.setAttribute(SQL_ATTR_PARAMS_PROCESSED_PTR, &processed, 0), SQL_SUCCESS);
    ASSERT_EQ(stmt_.setAttribute(SQL_ATTR_ROWS_FETCHED_PTR, &fetched, 0), SQL_SUCCESS);

    EXPECT_EQ(stmt_.bulkOperations(SQL_ADD), SQL_ERROR);
    EXPECT_EQ(processed, 0u);
    EXPECT_EQ(fetched, 0u);
    EXPECT_EQ(param_status[0], SQL_PARAM_ERROR);
    EXPECT_EQ(param_status[1], 0u);
    EXPECT_EQ(param_status[2], 0u);
    EXPECT_EQ(flaky_ptr->statements.size(), 1u);
    EXPECT_EQ(flaky_ptr->rollback_calls_, 0u);
    EXPECT_EQ(conn_.client_bridge_.get(), flaky_ptr);
}

TEST_F(OdbcBulkOperationsTest, BulkOperationsPartialFailureRollsBackWhenTransactionIsActive) {
    const char* sql = "INSERT INTO bulk_audit (id, note) VALUES (?, ?)";
    ASSERT_EQ(stmt_.prepare(reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)), SQL_NTS), SQL_SUCCESS);

    SQLINTEGER ids[] = {1, 2, 3};
    SQLLEN id_ind[] = {0, 0, 0};
    char notes[3][12] = {};
    std::strcpy(notes[0], "a");
    std::strcpy(notes[1], "b");
    std::strcpy(notes[2], "c");
    SQLLEN note_ind[] = {0, 0, 0};

    ASSERT_EQ(stmt_.bindParameter(1, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER,
                                 0, 0, ids, 0, id_ind),
              SQL_SUCCESS);
    ASSERT_EQ(stmt_.bindParameter(2, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
                                 sizeof(notes[0]), 0, notes, sizeof(notes[0]), note_ind),
              SQL_SUCCESS);

    auto flaky_bridge = std::make_unique<OdbcFlakyBulkClientBridge>(2);
    auto* flaky_ptr = flaky_bridge.get();
    conn_.client_bridge_ = std::move(flaky_bridge);
    conn_.auto_commit_ = SQL_AUTOCOMMIT_OFF;
    conn_.in_transaction_ = true;

    SQLULEN paramset_size = 3;
    SQLUSMALLINT param_status[3] = {0};
    SQLULEN processed = 0;
    SQLULEN fetched = 0;
    ASSERT_EQ(stmt_.setAttribute(SQL_ATTR_PARAMSET_SIZE, reinterpret_cast<SQLPOINTER>(paramset_size), 0),
              SQL_SUCCESS);
    ASSERT_EQ(stmt_.setAttribute(SQL_ATTR_PARAM_STATUS_PTR, param_status, 0), SQL_SUCCESS);
    ASSERT_EQ(stmt_.setAttribute(SQL_ATTR_PARAMS_PROCESSED_PTR, &processed, 0), SQL_SUCCESS);
    ASSERT_EQ(stmt_.setAttribute(SQL_ATTR_ROWS_FETCHED_PTR, &fetched, 0), SQL_SUCCESS);

    EXPECT_EQ(stmt_.bulkOperations(SQL_ADD), SQL_ERROR);
    EXPECT_EQ(processed, 1u);
    EXPECT_EQ(fetched, 1u);
    EXPECT_EQ(param_status[0], SQL_PARAM_SUCCESS);
    EXPECT_EQ(param_status[1], SQL_PARAM_ERROR);
    EXPECT_EQ(param_status[2], 0u);
    EXPECT_EQ(flaky_ptr->statements.size(), 2u);
    EXPECT_EQ(flaky_ptr->rollback_calls_, 1u);

    const auto* diag1 = stmt_.getDiagnostic(1);
    ASSERT_NE(diag1, nullptr);
    EXPECT_EQ(diag1->sqlstate, "23505");
    const auto* diag2 = stmt_.getDiagnostic(2);
    ASSERT_NE(diag2, nullptr);
    EXPECT_EQ(diag2->sqlstate, "01000");
}

}  // namespace
