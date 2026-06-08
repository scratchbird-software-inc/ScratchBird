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
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.lang.reflect.Field;
import java.sql.PreparedStatement;
import java.sql.ResultSet;
import java.sql.ResultSetMetaData;
import java.sql.Statement;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;
import org.junit.jupiter.api.Test;
import sun.misc.Unsafe;

class SBResultSetCursorNameTest {

    @Test
    void autoAssignedCursorNameSupportsPositionedUpdateAndRecursiveSchemaMetadata() throws Exception {
        CursorProtocol protocol = new CursorProtocol();
        SBConnection connection = newConnectionForTest(protocol);
        SBStatement statement = new SBStatement(connection, ResultSet.TYPE_SCROLL_INSENSITIVE,
            ResultSet.CONCUR_UPDATABLE, ResultSet.CLOSE_CURSORS_AT_COMMIT);

        ResultSet rs = statement.executeQuery("SELECT id, note FROM emulated.mysql.mymain.demo");
        String cursorName = rs.getCursorName();
        assertNotNull(cursorName);
        assertFalse(cursorName.isBlank());
        assertTrue(cursorName.startsWith("sb_cursor_"));

        assertTrue(rs.next());
        ResultSetMetaData meta = rs.getMetaData();
        assertEquals("emulated.mysql.mymain", meta.getSchemaName(1));
        assertEquals("demo", meta.getTableName(1));

        SBStatement positionedMutation = new SBStatement(connection, ResultSet.TYPE_FORWARD_ONLY,
            ResultSet.CONCUR_READ_ONLY, ResultSet.CLOSE_CURSORS_AT_COMMIT);
        int updated = positionedMutation.executeUpdate(
            "UPDATE emulated.mysql.mymain.demo SET note = 'new' WHERE CURRENT OF " + cursorName);
        assertEquals(1, updated);
        assertTrue(protocol.executedSql.stream().anyMatch(sql ->
            sql.startsWith("UPDATE emulated.mysql.mymain.demo SET note = 'new' WHERE")));
    }

    @Test
    void preparedStatementResultSetRegistersCursorForPositionedMutation() throws Exception {
        CursorProtocol protocol = new CursorProtocol();
        SBConnection connection = newConnectionForTest(protocol);
        try (PreparedStatement prepared = new SBPreparedStatement(connection,
            "SELECT id, note FROM emulated.mysql.mymain.demo",
            ResultSet.TYPE_SCROLL_INSENSITIVE,
            ResultSet.CONCUR_UPDATABLE,
            ResultSet.CLOSE_CURSORS_AT_COMMIT)) {
            try (ResultSet rs = prepared.executeQuery()) {
                String cursorName = rs.getCursorName();
                assertNotNull(cursorName);
                assertFalse(cursorName.isBlank());
                assertTrue(cursorName.startsWith("sb_cursor_"));

                assertTrue(rs.next());
                ResultSetMetaData meta = rs.getMetaData();
                assertEquals("emulated.mysql.mymain", meta.getSchemaName(1));
                assertEquals("demo", meta.getTableName(1));

                SBStatement positionedMutation = new SBStatement(connection, ResultSet.TYPE_FORWARD_ONLY,
                    ResultSet.CONCUR_READ_ONLY, ResultSet.CLOSE_CURSORS_AT_COMMIT);
                int updated = positionedMutation.executeUpdate(
                    "UPDATE emulated.mysql.mymain.demo SET note = 'prepared-new' WHERE CURRENT OF " + cursorName);
                assertEquals(1, updated);
                assertTrue(protocol.executedSql.stream().anyMatch(sql ->
                    sql.startsWith("UPDATE emulated.mysql.mymain.demo SET note = 'prepared-new' WHERE")));
            }
        }
    }

    @Test
    void preparedStatementRespectsExplicitCursorNameForPositionedMutation() throws Exception {
        CursorProtocol protocol = new CursorProtocol();
        SBConnection connection = newConnectionForTest(protocol);
        try (SBPreparedStatement prepared = new SBPreparedStatement(connection,
            "SELECT id, note FROM emulated.mysql.mymain.demo",
            ResultSet.TYPE_SCROLL_INSENSITIVE,
            ResultSet.CONCUR_UPDATABLE,
            ResultSet.CLOSE_CURSORS_AT_COMMIT)) {
            prepared.setCursorName("prepared_cursor_named");
            try (ResultSet rs = prepared.executeQuery()) {
                String cursorName = rs.getCursorName();
                assertEquals("prepared_cursor_named", cursorName);
                assertTrue(rs.next());

                SBStatement positionedMutation = new SBStatement(connection, ResultSet.TYPE_FORWARD_ONLY,
                    ResultSet.CONCUR_READ_ONLY, ResultSet.CLOSE_CURSORS_AT_COMMIT);
                int updated = positionedMutation.executeUpdate(
                    "UPDATE emulated.mysql.mymain.demo SET note = 'prepared-explicit' WHERE CURRENT OF "
                        + cursorName);
                assertEquals(1, updated);
                assertTrue(protocol.executedSql.stream().anyMatch(sql ->
                    sql.startsWith("UPDATE emulated.mysql.mymain.demo SET note = 'prepared-explicit' WHERE")));
            }
        }
    }

    private static SBConnection newConnectionForTest(SBProtocolHandler protocol) throws Exception {
        SBConnection connection = (SBConnection) getUnsafe().allocateInstance(SBConnection.class);
        setField(connection, "protocol", protocol);
        setField(connection, "properties", new SBConnectionProperties());
        setField(connection, "closed", new AtomicBoolean(false));
        setField(connection, "circuitBreaker", new CircuitBreaker());
        setField(connection, "telemetry", new TelemetryCollector());
        setField(connection, "namedCursors", new HashMap<String, SBResultSet>());
        setField(connection, "readOnly", false);
        setField(connection, "autoCommit", true);
        setField(connection, "schema", "public");
        return connection;
    }

    private static Unsafe getUnsafe() throws Exception {
        Field field = Unsafe.class.getDeclaredField("theUnsafe");
        field.setAccessible(true);
        return (Unsafe) field.get(null);
    }

    private static void setField(Object object, String fieldName, Object value) throws Exception {
        Field field = SBConnection.class.getDeclaredField(fieldName);
        field.setAccessible(true);
        field.set(object, value);
    }

    private static final class CursorProtocol extends SBProtocolHandler {
        private final List<String> executedSql = new ArrayList<>();

        CursorProtocol() {
            super(new SBConnectionProperties());
        }

        @Override
        public synchronized SBQueryResult execute(String sql, int maxRows, int timeoutMs) {
            executedSql.add(sql);
            SBQueryResult result = new SBQueryResult();
            if (sql.toLowerCase().startsWith("select ")) {
                List<SBColumnInfo> columns = new ArrayList<>();
                SBColumnInfo id = new SBColumnInfo();
                id.setName("id");
                columns.add(id);
                SBColumnInfo note = new SBColumnInfo();
                note.setName("note");
                columns.add(note);
                result.setColumns(columns);
                List<Object[]> rows = new ArrayList<>();
                rows.add(new Object[] {1, "old"});
                result.setRows(rows);
                result.setUpdateCount(-1);
                return result;
            }
            result.setUpdateCount(1);
            return result;
        }

        @Override
        public synchronized SBQueryResult execute(String sql, List<Object> params, List<Integer> paramTypes,
                                                  int maxRows, int timeoutMs) {
            return execute(sql, maxRows, timeoutMs);
        }
    }
}
