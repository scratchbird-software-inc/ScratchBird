// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>
#include <memory>

#define private public
#include "scratchbird/odbc/odbc_handles.h"
#include "scratchbird/odbc/odbc_client_bridge.h"
#undef private

namespace {

class FakePreparedClientBridge : public scratchbird::odbc::OdbcClientBridge {
public:
    SQLRETURN executeSQL(const std::string& sql,
                         std::vector<std::vector<std::string>>& results,
                         std::vector<scratchbird::odbc::ColumnMetadata>& columns,
                         SQLLEN& rows_affected) override {
        (void)columns;
        (void)results;

        statements.push_back(sql);
        rows_affected = 1;
        return SQL_SUCCESS;
    }

    std::vector<std::string> statements;
};

class OdbcLobStreamingTest : public ::testing::Test {
protected:
    scratchbird::odbc::OdbcEnvironment env_{};
    scratchbird::odbc::OdbcConnection conn_{&env_};
    scratchbird::odbc::OdbcStatement stmt_{&conn_};
    FakePreparedClientBridge* bridge_{nullptr};

    void SetUp() override {
        conn_.connected_ = true;
        auto bridge = std::make_unique<FakePreparedClientBridge>();
        bridge_ = bridge.get();
        conn_.client_bridge_ = std::move(bridge);
        stmt_.has_results_ = true;
    }

    void seedTextRows(std::vector<std::string> rows, SQLSMALLINT sql_type = SQL_VARCHAR) {
        stmt_.rows_.clear();
        for (auto& row : rows) {
            stmt_.rows_.push_back({row});
        }
        stmt_.columns_ = {
            {"body", "VARCHAR", "", "", "", "", sql_type, static_cast<SQLULEN>(rows.empty() ? 0 : rows[0].size()),
             0, SQL_NO_NULLS, false, false, true, SQL_PRED_BASIC, 0, 0}
        };
        stmt_.row_count_ = static_cast<SQLLEN>(rows.size());
        stmt_.current_row_ = rows.empty() ? 0 : 1;
    }
};

TEST_F(OdbcLobStreamingTest, TextGetDataStreamsInChunksAndFinishes) {
    seedTextRows({"ABCDEFGHIJKLMNOPQRSTUVWXYZ"});

    char chunk[6] = {};
    SQLLEN indicator = -1;

    EXPECT_EQ(stmt_.getData(1, SQL_C_CHAR, chunk, sizeof(chunk), &indicator), SQL_SUCCESS_WITH_INFO);
    EXPECT_EQ(indicator, 26);
    EXPECT_STREQ(chunk, "ABCDE");

    EXPECT_EQ(stmt_.getData(1, SQL_C_CHAR, chunk, sizeof(chunk), &indicator), SQL_SUCCESS_WITH_INFO);
    EXPECT_EQ(indicator, 26);
    EXPECT_STREQ(chunk, "FGHIJ");

    EXPECT_EQ(stmt_.getData(1, SQL_C_CHAR, chunk, sizeof(chunk), &indicator), SQL_SUCCESS_WITH_INFO);
    EXPECT_EQ(indicator, 26);
    EXPECT_STREQ(chunk, "KLMNO");

    EXPECT_EQ(stmt_.getData(1, SQL_C_CHAR, chunk, sizeof(chunk), &indicator), SQL_SUCCESS_WITH_INFO);
    EXPECT_EQ(indicator, 26);
    EXPECT_STREQ(chunk, "PQRST");

    EXPECT_EQ(stmt_.getData(1, SQL_C_CHAR, chunk, sizeof(chunk), &indicator), SQL_SUCCESS_WITH_INFO);
    EXPECT_EQ(indicator, 26);
    EXPECT_STREQ(chunk, "UVWXY");

    EXPECT_EQ(stmt_.getData(1, SQL_C_CHAR, chunk, sizeof(chunk), &indicator), SQL_SUCCESS);
    EXPECT_EQ(indicator, 0);
    EXPECT_STREQ(chunk, "Z");

    EXPECT_EQ(stmt_.getData(1, SQL_C_CHAR, chunk, sizeof(chunk), &indicator), SQL_SUCCESS);
    EXPECT_EQ(indicator, 0);
}

TEST_F(OdbcLobStreamingTest, BinaryGetDataStreamsRawBytes) {
    std::string blob;
    blob.push_back('\x01');
    blob.push_back('\x00');
    blob.push_back('\x02');
    blob.push_back('\xFE');
    blob.push_back('\x00');
    blob.push_back('\x7F');
    seedTextRows({blob}, SQL_VARBINARY);

    uint8_t chunk[4] = {};
    SQLLEN indicator = -1;
    uint8_t expected1[] = {0x01, 0x00, 0x02, static_cast<uint8_t>(0xFE)};
    uint8_t expected2[] = {0x00, 0x7F};

    std::fill(std::begin(chunk), std::end(chunk), 0);
    EXPECT_EQ(stmt_.getData(1, SQL_C_BINARY, chunk, sizeof(chunk), &indicator), SQL_SUCCESS_WITH_INFO);
    EXPECT_EQ(indicator, 6);
    EXPECT_EQ(std::vector<uint8_t>(chunk, chunk + 4), std::vector<uint8_t>(std::begin(expected1), std::end(expected1)));

    std::fill(std::begin(chunk), std::end(chunk), 0);
    EXPECT_EQ(stmt_.getData(1, SQL_C_BINARY, chunk, sizeof(chunk), &indicator), SQL_SUCCESS);
    EXPECT_EQ(indicator, 6);
    EXPECT_EQ(std::vector<uint8_t>(chunk, chunk + 2), std::vector<uint8_t>(std::begin(expected2), std::end(expected2)));
}

TEST_F(OdbcLobStreamingTest, StreamStateResetsOnPositionChange) {
    seedTextRows({"first-row-data", "second-row-data"});
    stmt_.cursor_type_ = SQL_CURSOR_STATIC;
    stmt_.current_row_ = 1;

    char row1_chunk[8] = {};
    SQLLEN indicator = -1;
    EXPECT_EQ(stmt_.getData(1, SQL_C_CHAR, row1_chunk, sizeof(row1_chunk), &indicator), SQL_SUCCESS_WITH_INFO);
    EXPECT_EQ(indicator, 14);
    EXPECT_STREQ(row1_chunk, "first-r");

    EXPECT_EQ(stmt_.setPos(2, SQL_POSITION, SQL_LOCK_NO_CHANGE), SQL_SUCCESS);
    EXPECT_EQ(stmt_.current_row_, 2u);

    char row2_chunk[8] = {};
    EXPECT_EQ(stmt_.getData(1, SQL_C_CHAR, row2_chunk, sizeof(row2_chunk), &indicator), SQL_SUCCESS_WITH_INFO);
    EXPECT_EQ(indicator, 15);
    EXPECT_STREQ(row2_chunk, "second-");
}

TEST_F(OdbcLobStreamingTest, SQLPutDataUnknownLengthStreamsTextAndExecutes) {
    const char* sql = "INSERT INTO audit_log (message) VALUES (?)";
    ASSERT_EQ(stmt_.prepare(reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)), SQL_NTS), SQL_SUCCESS);

    char payload_part_one[] = "alpha-";
    char payload_part_two[] = "payload";
    SQLLEN ind = SQL_DATA_AT_EXEC;
    char ignored_buffer[] = "";

    ASSERT_EQ(stmt_.bindParameter(1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
                                 0, 0, ignored_buffer, 0, &ind), SQL_SUCCESS);

    EXPECT_EQ(stmt_.execute(), SQL_NEED_DATA);

    SQLPOINTER token = nullptr;
    EXPECT_EQ(stmt_.paramData(&token), SQL_NEED_DATA);
    ASSERT_NE(token, nullptr);

    EXPECT_EQ(stmt_.putData(reinterpret_cast<SQLPOINTER>(const_cast<char*>(payload_part_one)),
                           static_cast<SQLLEN>(sizeof(payload_part_one) - 1)),
              SQL_SUCCESS);
    EXPECT_EQ(stmt_.putData(reinterpret_cast<SQLPOINTER>(const_cast<char*>(payload_part_two)),
                           static_cast<SQLLEN>(sizeof(payload_part_two) - 1)),
              SQL_SUCCESS);
    EXPECT_EQ(stmt_.putData(nullptr, 0), SQL_SUCCESS);
    EXPECT_EQ(stmt_.paramData(&token), SQL_SUCCESS);
    EXPECT_EQ(token, nullptr);

    EXPECT_EQ(stmt_.execute(), SQL_SUCCESS);
    ASSERT_EQ(bridge_->statements.size(), 1u);
    EXPECT_NE(bridge_->statements[0].find("INSERT INTO audit_log (message) VALUES ('alpha-payload')"), std::string::npos);
}

TEST_F(OdbcLobStreamingTest, SQLPutDataKnownLengthBinaryAutoCompletes) {
    const char* sql = "INSERT INTO audit_blob (payload) VALUES (?)";
    ASSERT_EQ(stmt_.prepare(reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)), SQL_NTS), SQL_SUCCESS);

    unsigned char chunk_one[] = {0x01, 0xFE};
    unsigned char chunk_two[] = {0x10, 0x7F};
    std::string binary_payload(
        reinterpret_cast<const char*>(chunk_one),
        reinterpret_cast<const char*>(chunk_one) + sizeof(chunk_one));
    binary_payload.append(reinterpret_cast<const char*>(chunk_two),
                         reinterpret_cast<const char*>(chunk_two) + sizeof(chunk_two));
    SQLLEN ind = -100 - static_cast<SQLLEN>(binary_payload.size());
    unsigned char ignored_buffer[] = {0x01, 0xFE};

    ASSERT_EQ(stmt_.bindParameter(1, SQL_PARAM_INPUT, SQL_C_BINARY, SQL_VARBINARY,
                                 0, 0, ignored_buffer, 0, &ind), SQL_SUCCESS);

    EXPECT_EQ(stmt_.execute(), SQL_NEED_DATA);
    SQLPOINTER token = nullptr;
    EXPECT_EQ(stmt_.paramData(&token), SQL_NEED_DATA);
    ASSERT_NE(token, nullptr);
    EXPECT_EQ(stmt_.putData(reinterpret_cast<SQLPOINTER>(ignored_buffer), sizeof(ignored_buffer)),
              SQL_SUCCESS);
    EXPECT_EQ(stmt_.paramData(&token), SQL_NEED_DATA);
    ASSERT_NE(token, nullptr);
    EXPECT_EQ(stmt_.putData(reinterpret_cast<SQLPOINTER>(chunk_two), sizeof(chunk_two)),
              SQL_SUCCESS);
    EXPECT_EQ(stmt_.paramData(&token), SQL_SUCCESS);
    EXPECT_EQ(token, nullptr);
    EXPECT_EQ(stmt_.execute(), SQL_SUCCESS);

    ASSERT_EQ(bridge_->statements.size(), 1u);
    EXPECT_NE(bridge_->statements[0].find("INSERT INTO audit_blob (payload) VALUES (X'01FE107F')"), std::string::npos);
}
}  // namespace
