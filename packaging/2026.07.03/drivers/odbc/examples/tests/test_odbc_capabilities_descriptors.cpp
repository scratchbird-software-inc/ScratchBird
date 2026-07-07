// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <string>
#include <vector>

#define private public
#include "scratchbird/odbc/odbc_handles.h"
#undef private

namespace {

TEST(OdbcDescriptorConformanceTest, FieldSetGetRoundTrip) {
    scratchbird::odbc::OdbcEnvironment env{};
    scratchbird::odbc::OdbcConnection conn{&env};

    scratchbird::odbc::OdbcDescriptor descriptor(&conn, scratchbird::odbc::OdbcDescriptor::DescriptorType::APD,
                                                 false);

    SQLSMALLINT count = 2;
    EXPECT_EQ(descriptor.setField(0, SQL_DESC_COUNT, &count, 0), SQL_SUCCESS);

    SQLSMALLINT sql_type = SQL_INTEGER;
    SQLLEN column_length = 42;
    SQLLEN octet_length = 44;
    SQLSMALLINT precision = 10;
    SQLSMALLINT scale = 4;
    SQLSMALLINT nullable = SQL_NULLABLE;
    SQLSMALLINT searchable = 2;  // SQL_SEARCHABLE
    SQLSMALLINT updatable = 1;
    SQLSMALLINT precision_out = 0;
    SQLLEN length_out = 0;

    EXPECT_EQ(descriptor.setField(1, SQL_DESC_TYPE, &sql_type, 0), SQL_SUCCESS);
    EXPECT_EQ(descriptor.setField(1, SQL_DESC_CONCISE_TYPE, &sql_type, 0), SQL_SUCCESS);
    EXPECT_EQ(descriptor.setField(1, SQL_DESC_LENGTH, &column_length, 0), SQL_SUCCESS);
    EXPECT_EQ(descriptor.setField(1, SQL_DESC_OCTET_LENGTH, &octet_length, 0), SQL_SUCCESS);
    EXPECT_EQ(descriptor.setField(1, SQL_DESC_PRECISION, &precision, 0), SQL_SUCCESS);
    EXPECT_EQ(descriptor.setField(1, SQL_DESC_SCALE, &scale, 0), SQL_SUCCESS);
    EXPECT_EQ(descriptor.setField(1, SQL_DESC_NULLABLE, &nullable, 0), SQL_SUCCESS);
    EXPECT_EQ(descriptor.setField(1, SQL_DESC_SEARCHABLE, &searchable, 0), SQL_SUCCESS);
    EXPECT_EQ(descriptor.setField(1, SQL_DESC_UPDATABLE, &updatable, 0), SQL_SUCCESS);
    EXPECT_EQ(descriptor.setField(1, SQL_DESC_NAME, (SQLPOINTER)"account_id", SQL_NTS),
              SQL_SUCCESS);

    SQLINTEGER string_length = 0;
    EXPECT_EQ(descriptor.getField(1, SQL_DESC_TYPE, &sql_type, 0, &string_length), SQL_SUCCESS);
    EXPECT_EQ(sql_type, SQL_INTEGER);

    EXPECT_EQ(descriptor.getField(1, SQL_DESC_PRECISION, &precision_out, 0, &string_length),
              SQL_SUCCESS);
    EXPECT_EQ(precision_out, precision);

    EXPECT_EQ(descriptor.getField(1, SQL_DESC_SCALE, &scale, 0, &string_length), SQL_SUCCESS);
    EXPECT_EQ(scale, 4);

    EXPECT_EQ(descriptor.getField(1, SQL_DESC_NULLABLE, &nullable, 0, &string_length), SQL_SUCCESS);
    EXPECT_EQ(nullable, SQL_NULLABLE);

    EXPECT_EQ(descriptor.getField(1, SQL_DESC_SEARCHABLE, &searchable, 0, &string_length),
              SQL_SUCCESS);
    EXPECT_EQ(searchable, 2);

    EXPECT_EQ(descriptor.getField(1, SQL_DESC_UPDATABLE, &updatable, 0, &string_length), SQL_SUCCESS);
    EXPECT_EQ(updatable, 1);

    char name_buf[32] = {};
    EXPECT_EQ(descriptor.getField(1, SQL_DESC_NAME, name_buf, static_cast<SQLINTEGER>(sizeof(name_buf)),
                               &string_length),
              SQL_SUCCESS);
    EXPECT_STREQ(name_buf, "account_id");

    EXPECT_EQ(descriptor.getField(0, SQL_DESC_COUNT, &count, 0, &string_length), SQL_SUCCESS);
    EXPECT_EQ(count, 2);

    EXPECT_EQ(descriptor.getField(1, SQL_DESC_LENGTH, &length_out, 0, &string_length),
              SQL_SUCCESS);
    EXPECT_EQ(length_out, column_length);

    EXPECT_EQ(descriptor.getField(1, SQL_DESC_OCTET_LENGTH, &length_out, 0, &string_length),
              SQL_SUCCESS);
    EXPECT_EQ(length_out, octet_length);

    EXPECT_EQ(descriptor.setField(1, SQL_DESC_PRECISION, nullptr, 0), SQL_ERROR);
}

TEST(OdbcDescriptorConformanceTest, SetRecGetRecRoundTrip) {
    scratchbird::odbc::OdbcEnvironment env{};
    scratchbird::odbc::OdbcConnection conn{&env};
    scratchbird::odbc::OdbcDescriptor descriptor(&conn, scratchbird::odbc::OdbcDescriptor::DescriptorType::ARD,
                                                 false);

    SQLLEN slen = 7;
    SQLLEN indicator = -1;
    int payload = 99;

    EXPECT_EQ(descriptor.setRec(1, SQL_VARCHAR, 0, 128, 32, 0,
                               reinterpret_cast<SQLPOINTER>(&payload), &slen, &indicator),
              SQL_SUCCESS);

    SQLCHAR name[16] = {};
    SQLSMALLINT len = 0;
    SQLSMALLINT type = SQL_UNKNOWN_TYPE;
    SQLSMALLINT subtype = SQL_UNKNOWN_TYPE;
    SQLLEN rec_length = 0;
    SQLSMALLINT precision = 0;
    SQLSMALLINT scale = 0;
    SQLSMALLINT nullable = SQL_NULLABLE_UNKNOWN;

    EXPECT_EQ(descriptor.getRec(1, name, sizeof(name), &len, &type, &subtype,
                               &rec_length, &precision, &scale, &nullable),
              SQL_SUCCESS);
    EXPECT_EQ(type, SQL_VARCHAR);
    EXPECT_EQ(subtype, 0);
    EXPECT_EQ(rec_length, 128);
    EXPECT_EQ(precision, 32);
    EXPECT_EQ(scale, 0);
    EXPECT_EQ(nullable, SQL_NULLABLE_UNKNOWN);

    scratchbird::odbc::OdbcDescriptor target(&conn, scratchbird::odbc::OdbcDescriptor::DescriptorType::IRD,
                                            false);
    SQLSMALLINT copy_count = 1;
    EXPECT_EQ(descriptor.setField(0, SQL_DESC_COUNT, &copy_count, 0), SQL_SUCCESS);
    EXPECT_EQ(descriptor.copyDesc(&target), SQL_SUCCESS);
    EXPECT_EQ(target.getCount(), 1);
}

TEST(OdbcDescriptorConformanceTest, StatementParameterAndRowDescriptorUpdates) {
    scratchbird::odbc::OdbcEnvironment env{};
    scratchbird::odbc::OdbcConnection conn{&env};
    scratchbird::odbc::OdbcStatement stmt{&conn};
    conn.connected_ = true;

    int bound_value = 123;
    SQLSMALLINT precision = 0;
    SQLSMALLINT scale = 0;
    SQLLEN desc_length = 0;
    SQLSMALLINT desc_nullable = SQL_NULLABLE;
    SQLSMALLINT desc_type = 0;

    EXPECT_EQ(stmt.bindParameter(1, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 4, 0,
                                &bound_value, sizeof(bound_value), nullptr),
              SQL_SUCCESS);

    auto* apd = stmt.getAppParamDescriptor();
    auto* ipd = stmt.getImpParamDescriptor();
    ASSERT_NE(apd, nullptr);
    ASSERT_NE(ipd, nullptr);

    EXPECT_EQ(apd->getCount(), 1);
    EXPECT_EQ(ipd->getCount(), 1);

    EXPECT_EQ(apd->getField(1, SQL_DESC_TYPE, &desc_type, 0, nullptr), SQL_SUCCESS);
    EXPECT_EQ(desc_type, SQL_INTEGER);
    EXPECT_EQ(apd->getField(1, SQL_DESC_PRECISION, &precision, 0, nullptr), SQL_SUCCESS);
    EXPECT_EQ(apd->getField(1, SQL_DESC_SCALE, &scale, 0, nullptr), SQL_SUCCESS);
    EXPECT_EQ(apd->getField(1, SQL_DESC_NULLABLE, &desc_nullable, 0, nullptr), SQL_SUCCESS);
    EXPECT_EQ(apd->getField(1, SQL_DESC_OCTET_LENGTH, &desc_length, 0, nullptr), SQL_SUCCESS);
    EXPECT_EQ(desc_nullable, SQL_NULLABLE_UNKNOWN);
    EXPECT_EQ(desc_length, static_cast<SQLLEN>(sizeof(bound_value)));

    std::vector<scratchbird::odbc::ColumnMetadata> columns;
    scratchbird::odbc::ColumnMetadata col;
    col.name = "id";
    col.sql_type = SQL_INTEGER;
    col.column_size = 4;
    col.decimal_digits = 0;
    col.nullable = SQL_NO_NULLS;
    col.searchable = 2;
    columns.push_back(col);
    stmt.setCatalogResult(columns, {});

    int row_value = 0;
    SQLLEN row_indicator = 0;
    EXPECT_EQ(stmt.bindCol(1, SQL_C_LONG, &row_value, sizeof(row_value), &row_indicator),
              SQL_SUCCESS);

    auto* ard = stmt.getAppRowDescriptor();
    auto* ird = stmt.getImpRowDescriptor();
    ASSERT_NE(ard, nullptr);
    ASSERT_NE(ird, nullptr);
    EXPECT_EQ(ard->getCount(), 1);
    EXPECT_EQ(ird->getCount(), 1);

    EXPECT_EQ(ard->getField(1, SQL_DESC_LENGTH, &desc_length, 0, nullptr), SQL_SUCCESS);
    EXPECT_EQ(desc_length, static_cast<SQLLEN>(col.column_size));

    EXPECT_EQ(ard->getField(1, SQL_DESC_NULLABLE, &desc_nullable, 0, nullptr), SQL_SUCCESS);
    EXPECT_EQ(desc_nullable, SQL_NO_NULLS);

    EXPECT_EQ(stmt.freeStmt(SQL_UNBIND), SQL_SUCCESS);
    EXPECT_EQ(ard->getCount(), 0);
    EXPECT_EQ(ird->getCount(), 0);

    EXPECT_EQ(stmt.freeStmt(SQL_RESET_PARAMS), SQL_SUCCESS);
    EXPECT_EQ(apd->getCount(), 0);
    EXPECT_EQ(ipd->getCount(), 0);
}

}  // namespace
