// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package com.scratchbird.jdbc;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertDoesNotThrow;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.lang.reflect.Field;
import java.time.Instant;
import java.time.LocalDate;
import java.time.LocalDateTime;
import java.time.LocalTime;
import java.time.OffsetDateTime;
import java.time.OffsetTime;
import java.time.ZonedDateTime;
import java.sql.Array;
import java.sql.JDBCType;
import java.sql.ResultSet;
import java.sql.SQLData;
import java.sql.SQLInput;
import java.sql.SQLOutput;
import java.sql.SQLType;
import java.sql.SQLException;
import java.sql.Struct;
import java.sql.Types;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.UUID;
import sun.misc.Unsafe;

import org.junit.jupiter.api.Test;

public class SBCallableStatementOutParamTest {

    public static class SampleOutRecord implements SQLData {
        private String label;
        private int code;

        @Override
        public String getSQLTypeName() {
            return "record";
        }

        @Override
        public void readSQL(SQLInput stream, String typeName) throws java.sql.SQLException {
            this.label = stream.readString();
            this.code = stream.readInt();
        }

        @Override
        public void writeSQL(SQLOutput stream) throws java.sql.SQLException {
            stream.writeString(label);
            stream.writeInt(code);
        }
    }

    @Test
    public void executeHydratesRegisteredOutParametersFromFirstRow() throws Exception {
        OutParamProtocol protocol = new OutParamProtocol();
        SBConnection connection = newConnectionForTest(protocol);
        SBCallableStatement statement = new SBCallableStatement(connection, "CALL demo(?, ?)",
            ResultSet.TYPE_FORWARD_ONLY, ResultSet.CONCUR_READ_ONLY, ResultSet.CLOSE_CURSORS_AT_COMMIT);

        statement.registerOutParameter(1, Types.INTEGER);
        statement.registerOutParameter(2, Types.VARCHAR);

        boolean hasResultSet = statement.execute();

        assertTrue(hasResultSet);
        assertEquals(7, statement.getInt(1));
        assertFalse(statement.wasNull());
        assertEquals("done", statement.getString(2));

        try (ResultSet rs = statement.getResultSet()) {
            assertTrue(rs.next());
            assertEquals(7, rs.getInt(1));
            assertEquals("done", rs.getString(2));
            assertFalse(rs.next());
        }
    }

    @Test
    public void nameBasedOutParamsResolveNumericFallbackPatterns() throws Exception {
        OutParamProtocol protocol = new OutParamProtocol();
        SBConnection connection = newConnectionForTest(protocol);
        SBCallableStatement statement = new SBCallableStatement(connection, "CALL demo(?, ?)",
            ResultSet.TYPE_FORWARD_ONLY, ResultSet.CONCUR_READ_ONLY, ResultSet.CLOSE_CURSORS_AT_COMMIT);

        statement.registerOutParameter("p1", Types.INTEGER);
        statement.registerOutParameter("param2", Types.VARCHAR);

        statement.execute();

        assertEquals(7, statement.getInt("1"));
        assertEquals("done", statement.getString("param2"));
    }

    @Test
    public void nameBasedOutParamsResolveNamedPlaceholders() throws Exception {
        OutParamProtocol protocol = new OutParamProtocol();
        SBConnection connection = newConnectionForTest(protocol);
        SBCallableStatement statement = new SBCallableStatement(connection, "CALL demo(:out_a, @out_b)",
            ResultSet.TYPE_FORWARD_ONLY, ResultSet.CONCUR_READ_ONLY, ResultSet.CLOSE_CURSORS_AT_COMMIT);

        statement.registerOutParameter("out_a", Types.INTEGER);
        statement.registerOutParameter("out_b", Types.VARCHAR);

        statement.execute();

        assertEquals(7, statement.getInt("out_a"));
        assertEquals("done", statement.getString("out_b"));
    }

    @Test
    public void refCursorOutParameterReturnsResultSetHandle() throws Exception {
        OutParamProtocol protocol = new OutParamProtocol();
        SBConnection connection = newConnectionForTest(protocol);
        SBCallableStatement statement = new SBCallableStatement(connection, "CALL demo(?)",
            ResultSet.TYPE_FORWARD_ONLY, ResultSet.CONCUR_READ_ONLY, ResultSet.CLOSE_CURSORS_AT_COMMIT);

        statement.registerOutParameter(1, Types.REF_CURSOR);
        assertTrue(statement.execute());

        ResultSet fromOutObject = (ResultSet) statement.getObject(1);
        ResultSet fromTyped = statement.getObject(1, ResultSet.class);
        ResultSet fromResultSet = statement.getResultSet();
        assertSame(fromResultSet, fromOutObject);
        assertSame(fromResultSet, fromTyped);
    }

    @Test
    public void arrayOutParameterSupportsLiteralAndTypedArrayAccess() throws Exception {
        ConfigurableOutParamProtocol protocol = new ConfigurableOutParamProtocol(
            new String[] {"arr"},
            new Object[] {"{1,2,3}"}
        );
        SBConnection connection = newConnectionForTest(protocol);
        SBCallableStatement statement = new SBCallableStatement(connection, "CALL demo(?)",
            ResultSet.TYPE_FORWARD_ONLY, ResultSet.CONCUR_READ_ONLY, ResultSet.CLOSE_CURSORS_AT_COMMIT);
        statement.registerOutParameter(1, Types.ARRAY);

        assertTrue(statement.execute());
        Array arr = statement.getArray(1);
        assertNotNull(arr);
        Object[] elements = (Object[]) arr.getArray();
        assertEquals(3, elements.length);
        assertEquals(1, elements[0]);
        assertEquals(2, elements[1]);
        assertEquals(3, elements[2]);

        Array typed = statement.getObject(1, Array.class);
        assertNotNull(typed);
        Object[] typedElements = (Object[]) typed.getArray();
        assertEquals(3, typedElements.length);
    }

    @Test
    public void structOutParameterSupportsTypedStructAccess() throws Exception {
        ConfigurableOutParamProtocol protocol = new ConfigurableOutParamProtocol(
            new String[] {"obj"},
            new Object[] {new Object[] {42, "alpha"}}
        );
        SBConnection connection = newConnectionForTest(protocol);
        SBCallableStatement statement = new SBCallableStatement(connection, "CALL demo(?)",
            ResultSet.TYPE_FORWARD_ONLY, ResultSet.CONCUR_READ_ONLY, ResultSet.CLOSE_CURSORS_AT_COMMIT);
        statement.registerOutParameter(1, Types.STRUCT);

        assertTrue(statement.execute());
        Struct struct = statement.getObject(1, Struct.class);
        assertNotNull(struct);
        Object[] attrs = struct.getAttributes();
        assertEquals(2, attrs.length);
        assertEquals(42, attrs[0]);
        assertEquals("alpha", attrs[1]);
    }

    @Test
    public void structOutParameterSupportsMapBasedSqlDataMapping() throws Exception {
        ConfigurableOutParamProtocol protocol = new ConfigurableOutParamProtocol(
            new String[] {"obj"},
            new Object[] {new SBStruct("public.record", new Object[] {"alpha", 77})}
        );
        SBConnection connection = newConnectionForTest(protocol);
        SBCallableStatement statement = new SBCallableStatement(connection, "CALL demo(?)",
            ResultSet.TYPE_FORWARD_ONLY, ResultSet.CONCUR_READ_ONLY, ResultSet.CLOSE_CURSORS_AT_COMMIT);
        statement.registerOutParameter(1, Types.STRUCT);

        assertTrue(statement.execute());
        Map<String, Class<?>> map = new HashMap<>();
        map.put("record", SampleOutRecord.class);
        Object mapped = statement.getObject(1, map);
        assertTrue(mapped instanceof SampleOutRecord);
        SampleOutRecord sample = (SampleOutRecord) mapped;
        assertEquals("alpha", sample.label);
        assertEquals(77, sample.code);
    }

    @Test
    public void structOutParameterSupportsTypedSqlDataAccess() throws Exception {
        ConfigurableOutParamProtocol protocol = new ConfigurableOutParamProtocol(
            new String[] {"obj"},
            new Object[] {new SBStruct("record", new Object[] {"beta", 91})}
        );
        SBConnection connection = newConnectionForTest(protocol);
        SBCallableStatement statement = new SBCallableStatement(connection, "CALL demo(?)",
            ResultSet.TYPE_FORWARD_ONLY, ResultSet.CONCUR_READ_ONLY, ResultSet.CLOSE_CURSORS_AT_COMMIT);
        statement.registerOutParameter(1, Types.STRUCT);

        assertTrue(statement.execute());
        SampleOutRecord sample = statement.getObject(1, SampleOutRecord.class);
        assertEquals("beta", sample.label);
        assertEquals(91, sample.code);
    }

    @Test
    public void sqlTypeRegisterOutParameterOverloadsHydrateValues() throws Exception {
        OutParamProtocol protocol = new OutParamProtocol();
        SBConnection connection = newConnectionForTest(protocol);
        SBCallableStatement statement = new SBCallableStatement(connection, "CALL demo(?, ?)",
            ResultSet.TYPE_FORWARD_ONLY, ResultSet.CONCUR_READ_ONLY, ResultSet.CLOSE_CURSORS_AT_COMMIT);

        statement.registerOutParameter(1, JDBCType.INTEGER);
        statement.registerOutParameter("param2", JDBCType.VARCHAR);

        assertTrue(statement.execute());
        assertEquals(7, statement.getInt(1));
        assertEquals("done", statement.getString("param2"));
    }

    @Test
    public void sqlTypeNamedSetObjectOverloadsBindUsingVendorTypeCode() throws Exception {
        OutParamProtocol protocol = new OutParamProtocol();
        SBConnection connection = newConnectionForTest(protocol);
        SBCallableStatement statement = new SBCallableStatement(connection, "CALL demo(?)",
            ResultSet.TYPE_FORWARD_ONLY, ResultSet.CONCUR_READ_ONLY, ResultSet.CLOSE_CURSORS_AT_COMMIT);

        statement.setObject("1", "42", JDBCType.INTEGER);
        statement.execute();

        assertEquals(1, protocol.lastParamTypes.size());
        assertEquals(Types.INTEGER, protocol.lastParamTypes.get(0).intValue());
        assertEquals(42, protocol.lastParams.get(0));
    }

    @Test
    public void sqlTypeOverloadsRejectNullSqlType() throws Exception {
        OutParamProtocol protocol = new OutParamProtocol();
        SBConnection connection = newConnectionForTest(protocol);
        SBCallableStatement statement = new SBCallableStatement(connection, "CALL demo(?)",
            ResultSet.TYPE_FORWARD_ONLY, ResultSet.CONCUR_READ_ONLY, ResultSet.CLOSE_CURSORS_AT_COMMIT);

        assertThrows(SQLException.class, () -> statement.registerOutParameter(1, (SQLType) null));
        assertThrows(SQLException.class, () -> statement.registerOutParameter("1", (SQLType) null));
        assertDoesNotThrow(() -> statement.setObject("1", 7, (SQLType) null));
    }

    @Test
    public void typedGetObjectSupportsJavaTimeUuidAndNclobMappings() throws Exception {
        ConfigurableOutParamProtocol protocol = new ConfigurableOutParamProtocol(
            new String[] {"d", "t", "ts", "uuid", "nclob", "timetz"},
            new Object[] {"2026-03-01", "12:34:56", "2026-03-01 12:34:56",
                "550e8400-e29b-41d4-a716-446655440000", "unicode-text", "01:02:03+02:00"}
        );
        SBConnection connection = newConnectionForTest(protocol);
        SBCallableStatement statement = new SBCallableStatement(connection, "CALL demo(?, ?, ?, ?, ?, ?)",
            ResultSet.TYPE_FORWARD_ONLY, ResultSet.CONCUR_READ_ONLY, ResultSet.CLOSE_CURSORS_AT_COMMIT);
        statement.registerOutParameter(1, Types.DATE);
        statement.registerOutParameter(2, Types.TIME);
        statement.registerOutParameter(3, Types.TIMESTAMP);
        statement.registerOutParameter(4, Types.VARCHAR);
        statement.registerOutParameter(5, Types.NCLOB);
        statement.registerOutParameter(6, Types.TIME_WITH_TIMEZONE);

        assertTrue(statement.execute());
        assertEquals(LocalDate.of(2026, 3, 1), statement.getObject(1, LocalDate.class));
        assertEquals(LocalTime.of(12, 34, 56), statement.getObject(2, LocalTime.class));
        assertEquals(LocalDateTime.of(2026, 3, 1, 12, 34, 56), statement.getObject(3, LocalDateTime.class));
        assertNotNull(statement.getObject(3, OffsetDateTime.class));
        assertNotNull(statement.getObject(3, Instant.class));
        assertEquals(UUID.fromString("550e8400-e29b-41d4-a716-446655440000"),
            statement.getObject(4, UUID.class));
        assertEquals("unicode-text", statement.getObject(5, java.sql.NClob.class).getSubString(1, 12));
        assertEquals(OffsetTime.parse("01:02:03+02:00"), statement.getObject(6, OffsetTime.class));
        ZonedDateTime zoned = statement.getObject(3, ZonedDateTime.class);
        assertNotNull(zoned);
    }

    private static SBConnection newConnectionForTest(SBProtocolHandler protocol) throws Exception {
        SBConnection connection = (SBConnection) getUnsafe().allocateInstance(SBConnection.class);
        setField(connection, "protocol", protocol);
        setField(connection, "properties", new SBConnectionProperties());
        setField(connection, "closed", new java.util.concurrent.atomic.AtomicBoolean(false));
        setField(connection, "circuitBreaker", new CircuitBreaker());
        setField(connection, "telemetry", new TelemetryCollector());
        setField(connection, "readOnly", false);
        setField(connection, "autoCommit", true);
        setField(connection, "schema", "public");
        return connection;
    }

    private static void setField(Object object, String fieldName, Object value) throws Exception {
        Field field = SBConnection.class.getDeclaredField(fieldName);
        field.setAccessible(true);
        field.set(object, value);
    }

    private static Unsafe getUnsafe() throws Exception {
        Field field = Unsafe.class.getDeclaredField("theUnsafe");
        field.setAccessible(true);
        return (Unsafe) field.get(null);
    }

    private static final class OutParamProtocol extends SBProtocolHandler {
        private List<Object> lastParams = Collections.emptyList();
        private List<Integer> lastParamTypes = Collections.emptyList();

        OutParamProtocol() {
            super(new SBConnectionProperties());
        }

        @Override
        public SBQueryResult execute(String sql, List<Object> params, List<Integer> paramTypes,
                                     int maxRows, int timeoutMs) {
            lastParams = params == null ? Collections.emptyList() : new ArrayList<>(params);
            lastParamTypes = paramTypes == null ? Collections.emptyList() : new ArrayList<>(paramTypes);
            SBQueryResult result = new SBQueryResult();
            List<SBColumnInfo> columns = new ArrayList<>();
            SBColumnInfo first = new SBColumnInfo();
            first.setName("out_a");
            columns.add(first);
            SBColumnInfo second = new SBColumnInfo();
            second.setName("out_b");
            columns.add(second);
            result.setColumns(columns);
            result.setRows(java.util.Collections.singletonList(new Object[] {7, "done"}));
            result.setUpdateCount(0);
            return result;
        }
    }

    private static final class ConfigurableOutParamProtocol extends SBProtocolHandler {
        private final String[] columnNames;
        private final Object[] rowValues;

        ConfigurableOutParamProtocol(String[] columnNames, Object[] rowValues) {
            super(new SBConnectionProperties());
            this.columnNames = columnNames;
            this.rowValues = rowValues;
        }

        @Override
        public SBQueryResult execute(String sql, List<Object> params, List<Integer> paramTypes,
                                     int maxRows, int timeoutMs) {
            SBQueryResult result = new SBQueryResult();
            List<SBColumnInfo> columns = new ArrayList<>();
            for (String name : columnNames) {
                SBColumnInfo col = new SBColumnInfo();
                col.setName(name);
                columns.add(col);
            }
            result.setColumns(columns);
            result.setRows(java.util.Collections.singletonList(rowValues));
            result.setUpdateCount(0);
            return result;
        }
    }
}
