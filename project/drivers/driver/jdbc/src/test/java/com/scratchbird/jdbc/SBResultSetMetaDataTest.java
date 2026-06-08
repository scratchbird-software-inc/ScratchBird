// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package com.scratchbird.jdbc;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.math.BigDecimal;
import java.sql.Types;
import java.util.ArrayList;
import java.util.List;
import java.util.Set;
import org.junit.jupiter.api.Test;

class SBResultSetMetaDataTest {

    @Test
    void reportsWritableAndQualifiedTableMetadataWhenProvided() throws Exception {
        SBColumnInfo amount = new SBColumnInfo();
        amount.setName("amount");
        amount.setTypeOid(790); // money
        amount.setTableOid(42);
        amount.setColumnNumber((short) 1);

        SBColumnInfo note = new SBColumnInfo();
        note.setName("note");
        note.setTypeOid(25); // text

        List<SBColumnInfo> columns = new ArrayList<>();
        columns.add(amount);
        columns.add(note);

        SBResultSetMetaData meta = new SBResultSetMetaData(
            columns,
            true,
            Set.of(1),
            "public",
            "orders",
            "main");

        assertEquals(2, meta.getColumnCount());
        assertEquals("public", meta.getSchemaName(1));
        assertEquals("orders", meta.getTableName(1));
        assertEquals("main", meta.getCatalogName(1));
        assertTrue(meta.isCurrency(1));
        assertEquals(Types.NUMERIC, meta.getColumnType(1));
        assertEquals(BigDecimal.class.getName(), meta.getColumnClassName(1));

        assertFalse(meta.isReadOnly(1));
        assertTrue(meta.isWritable(1));
        assertTrue(meta.isDefinitelyWritable(1));

        assertTrue(meta.isReadOnly(2));
        assertFalse(meta.isWritable(2));
        assertFalse(meta.isDefinitelyWritable(2));
    }

    @Test
    void reportsUuidClassForUuidOid() throws Exception {
        SBColumnInfo id = new SBColumnInfo();
        id.setName("id");
        id.setTypeOid(2950); // uuid

        SBResultSetMetaData meta = new SBResultSetMetaData(List.of(id));
        assertEquals(java.util.UUID.class.getName(), meta.getColumnClassName(1));
    }

    @Test
    void displaySizeUsesTypemodForVarcharAndTextFallbackForTextType() throws Exception {
        SBColumnInfo shortVarchar = new SBColumnInfo();
        shortVarchar.setName("short_name");
        shortVarchar.setTypeOid(1043); // varchar
        shortVarchar.setTypeModifier(24); // typmod = declared length + 4 => 20

        SBColumnInfo textValue = new SBColumnInfo();
        textValue.setName("notes");
        textValue.setTypeOid(25); // text
        textValue.setTypeModifier(-1);

        SBResultSetMetaData meta = new SBResultSetMetaData(List.of(shortVarchar, textValue));
        assertEquals(20, meta.getColumnDisplaySize(1));
        assertEquals(65535, meta.getColumnDisplaySize(2));
    }

    @Test
    void precisionUsesTypeFallbacksWhenTypemodIsUnavailable() throws Exception {
        SBColumnInfo intValue = new SBColumnInfo();
        intValue.setName("id");
        intValue.setTypeOid(23); // int4

        SBColumnInfo numericValue = new SBColumnInfo();
        numericValue.setName("amount");
        numericValue.setTypeOid(1700); // numeric

        SBColumnInfo varcharWithTypemod = new SBColumnInfo();
        varcharWithTypemod.setName("short_name");
        varcharWithTypemod.setTypeOid(1043); // varchar
        varcharWithTypemod.setTypeModifier(44); // typmod = declared length + 4 => 40

        SBColumnInfo xidValue = new SBColumnInfo();
        xidValue.setName("xmin");
        xidValue.setTypeOid(28); // xid

        SBResultSetMetaData meta = new SBResultSetMetaData(
            List.of(intValue, numericValue, varcharWithTypemod, xidValue));

        assertEquals(10, meta.getPrecision(1));
        assertEquals(38, meta.getPrecision(2));
        assertEquals(40, meta.getPrecision(3));
        assertEquals(10, meta.getPrecision(4));
    }

    @Test
    void scaleUsesTypemodForNumericAndFallbackForFloatingPointAndMoney() throws Exception {
        SBColumnInfo numericWithTypemod = new SBColumnInfo();
        numericWithTypemod.setName("amount");
        numericWithTypemod.setTypeOid(1700); // numeric
        numericWithTypemod.setTypeModifier((12 << 16) + 4 + 4); // precision=12, scale=4

        SBColumnInfo realValue = new SBColumnInfo();
        realValue.setName("ratio");
        realValue.setTypeOid(700); // float4

        SBColumnInfo doubleValue = new SBColumnInfo();
        doubleValue.setName("score");
        doubleValue.setTypeOid(701); // float8

        SBColumnInfo moneyValue = new SBColumnInfo();
        moneyValue.setName("price");
        moneyValue.setTypeOid(790); // money

        SBResultSetMetaData meta = new SBResultSetMetaData(
            List.of(numericWithTypemod, realValue, doubleValue, moneyValue));

        assertEquals(4, meta.getScale(1));
        assertEquals(6, meta.getScale(2));
        assertEquals(15, meta.getScale(3));
        assertEquals(2, meta.getScale(4));
    }

    @Test
    void displaySizeUsesNumericPrecisionAndScaleWhenTypemodIsPresent() throws Exception {
        SBColumnInfo numericValue = new SBColumnInfo();
        numericValue.setName("total");
        numericValue.setTypeOid(1700); // numeric
        numericValue.setTypeModifier((12 << 16) + 4 + 2); // precision=12, scale=2

        SBResultSetMetaData meta = new SBResultSetMetaData(List.of(numericValue));
        assertEquals(14, meta.getColumnDisplaySize(1)); // sign + precision + decimal point
    }
}
