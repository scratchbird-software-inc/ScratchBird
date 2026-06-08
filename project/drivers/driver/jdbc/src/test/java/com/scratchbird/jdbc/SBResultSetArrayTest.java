// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package com.scratchbird.jdbc;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.sql.Array;
import java.sql.SQLData;
import java.sql.SQLInput;
import java.sql.SQLException;
import java.sql.Struct;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import org.junit.jupiter.api.Test;

class SBResultSetArrayTest {

    public static class SampleRecordData implements SQLData {
        private String first;
        private int second;

        @Override
        public String getSQLTypeName() {
            return "record";
        }

        @Override
        public void readSQL(SQLInput stream, String typeName) throws SQLException {
            this.first = stream.readString();
            this.second = stream.readInt();
        }

        @Override
        public void writeSQL(java.sql.SQLOutput stream) throws SQLException {
            stream.writeString(first);
            stream.writeInt(second);
        }
    }

    @Test
    void parsesBraceIntegerArrayLiterals() throws SQLException {
        SBResultSet rs = singleColumnResultSet("{1,2,3}");
        assertTrue(rs.next());

        Array array = rs.getArray(1);
        assertNotNull(array);
        assertEquals("integer", array.getBaseTypeName());
        assertArrayEquals(new Object[] {1, 2, 3}, (Object[]) array.getArray());
    }

    @Test
    void parsesArrayKeywordWithQuotedValues() throws SQLException {
        SBResultSet rs = singleColumnResultSet("ARRAY['alpha','be\\'ta',NULL,'3']");
        assertTrue(rs.next());

        Array array = rs.getArray(1);
        assertNotNull(array);
        assertEquals("text", array.getBaseTypeName());
        assertArrayEquals(new Object[] {"alpha", "be'ta", null, "3"}, (Object[]) array.getArray());
    }

    @Test
    void wrapsObjectArrayWithoutStringParsing() throws SQLException {
        SBResultSet rs = singleColumnResultSet(new Object[] {true, false, true});
        assertTrue(rs.next());

        Array array = rs.getArray(1);
        assertNotNull(array);
        assertEquals("boolean", array.getBaseTypeName());
        assertArrayEquals(new Object[] {true, false, true}, (Object[]) array.getArray());
    }

    @Test
    void parsesNestedArrayLiterals() throws SQLException {
        SBResultSet rs = singleColumnResultSet("{{1,2},{3,4}}");
        assertTrue(rs.next());

        Array array = rs.getArray(1);
        Object[] outer = (Object[]) array.getArray();
        assertEquals(2, outer.length);
        assertArrayEquals(new Object[] {1, 2}, (Object[]) outer[0]);
        assertArrayEquals(new Object[] {3, 4}, (Object[]) outer[1]);
    }

    @Test
    void convertsObjectArraysAndCollectionsToStructForTypedGetObject() throws SQLException {
        SBResultSet fromArray = singleColumnResultSet(new Object[] {"a", 5});
        assertTrue(fromArray.next());
        Struct arrayStruct = fromArray.getObject(1, Struct.class);
        assertNotNull(arrayStruct);
        assertEquals("record", arrayStruct.getSQLTypeName());
        assertArrayEquals(new Object[] {"a", 5}, arrayStruct.getAttributes());

        SBResultSet fromCollection = singleColumnResultSet(Arrays.asList("b", 7));
        assertTrue(fromCollection.next());
        Struct collectionStruct = fromCollection.getObject(1, Struct.class);
        assertNotNull(collectionStruct);
        assertEquals("record", collectionStruct.getSQLTypeName());
        assertArrayEquals(new Object[] {"b", 7}, collectionStruct.getAttributes());
    }

    @Test
    void typedStructGetObjectRejectsScalarValues() throws SQLException {
        SBResultSet rs = singleColumnResultSet("not-a-struct");
        assertTrue(rs.next());
        assertThrows(SQLException.class, () -> rs.getObject(1, Struct.class));
    }

    @Test
    void mapsStructToSqlDataViaGetObjectWithMap() throws SQLException {
        SBResultSet rs = singleColumnResultSet(new SBStruct("public.record", new Object[] {"alpha", 12}));
        assertTrue(rs.next());

        Map<String, Class<?>> map = new HashMap<>();
        map.put("record", SampleRecordData.class);
        Object mapped = rs.getObject(1, map);

        assertTrue(mapped instanceof SampleRecordData);
        SampleRecordData sample = (SampleRecordData) mapped;
        assertEquals("alpha", sample.first);
        assertEquals(12, sample.second);
    }

    @Test
    void mapsStructToSqlDataViaTypedGetObject() throws SQLException {
        SBResultSet rs = singleColumnResultSet(new SBStruct("record", new Object[] {"beta", 9}));
        assertTrue(rs.next());

        SampleRecordData sample = rs.getObject(1, SampleRecordData.class);
        assertEquals("beta", sample.first);
        assertEquals(9, sample.second);
    }

    private static SBResultSet singleColumnResultSet(Object value) {
        SBColumnInfo column = new SBColumnInfo();
        column.setName("arr");
        List<SBColumnInfo> columns = Collections.singletonList(column);
        List<Object[]> rows = Collections.singletonList(new Object[] {value});
        return new SBResultSet(null, columns, rows);
    }
}
