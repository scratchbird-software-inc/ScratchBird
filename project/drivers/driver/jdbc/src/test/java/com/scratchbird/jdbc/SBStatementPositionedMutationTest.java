// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package com.scratchbird.jdbc;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.lang.reflect.Field;
import java.sql.ResultSet;
import java.sql.SQLException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.atomic.AtomicBoolean;
import org.junit.jupiter.api.Test;
import sun.misc.Unsafe;

class SBStatementPositionedMutationTest {

    @Test
    void rewritesPositionedUpdateUsingNamedCursor() throws Exception {
        CaptureProtocol protocol = new CaptureProtocol();
        SBConnection connection = newConnectionForTest(protocol);
        SBResultSet cursor = createNamedCursor(connection, "SELECT id, note FROM demo", "c_demo",
            new Object[] {1, "old"});

        SBStatement statement = new SBStatement(connection, ResultSet.TYPE_FORWARD_ONLY,
            ResultSet.CONCUR_READ_ONLY, ResultSet.CLOSE_CURSORS_AT_COMMIT);
        long count = statement.executeLargeUpdate("UPDATE demo SET note = 'new' WHERE CURRENT OF c_demo");

        assertEquals(1L, count);
        assertEquals(1, protocol.executedSql.size());
        String rewritten = protocol.executedSql.get(0);
        assertTrue(rewritten.startsWith("UPDATE demo SET note = 'new' WHERE"));
        assertTrue(rewritten.contains("\"id\" = 1"));
        assertTrue(rewritten.contains("\"note\" = 'old'"));

        cursor.close();
    }

    @Test
    void rewritesPositionedDeleteUsingNamedCursor() throws Exception {
        CaptureProtocol protocol = new CaptureProtocol();
        SBConnection connection = newConnectionForTest(protocol);
        SBResultSet cursor = createNamedCursor(connection, "SELECT id, note FROM demo", "c_delete",
            new Object[] {2, "remove"});

        SBStatement statement = new SBStatement(connection, ResultSet.TYPE_FORWARD_ONLY,
            ResultSet.CONCUR_READ_ONLY, ResultSet.CLOSE_CURSORS_AT_COMMIT);
        int count = statement.executeUpdate("DELETE FROM demo WHERE CURRENT OF c_delete");

        assertEquals(1, count);
        assertEquals(1, protocol.executedSql.size());
        String rewritten = protocol.executedSql.get(0);
        assertTrue(rewritten.startsWith("DELETE FROM demo WHERE"));
        assertTrue(rewritten.contains("\"id\" = 2"));
        assertTrue(rewritten.contains("\"note\" = 'remove'"));

        cursor.close();
    }

    @Test
    void positionedMutationFailsWhenCursorMissing() throws Exception {
        CaptureProtocol protocol = new CaptureProtocol();
        SBConnection connection = newConnectionForTest(protocol);
        SBStatement statement = new SBStatement(connection, ResultSet.TYPE_FORWARD_ONLY,
            ResultSet.CONCUR_READ_ONLY, ResultSet.CLOSE_CURSORS_AT_COMMIT);

        SQLException ex = assertThrows(SQLException.class,
            () -> statement.executeUpdate("DELETE FROM demo WHERE CURRENT OF missing_cursor"));
        assertEquals("34000", ex.getSQLState());
    }

    @Test
    void positionedMutationFailsWhenTargetTableDiffers() throws Exception {
        CaptureProtocol protocol = new CaptureProtocol();
        SBConnection connection = newConnectionForTest(protocol);
        SBResultSet cursor = createNamedCursor(connection, "SELECT id, note FROM demo", "c_demo",
            new Object[] {1, "old"});
        SBStatement statement = new SBStatement(connection, ResultSet.TYPE_FORWARD_ONLY,
            ResultSet.CONCUR_READ_ONLY, ResultSet.CLOSE_CURSORS_AT_COMMIT);

        SQLException ex = assertThrows(SQLException.class,
            () -> statement.executeUpdate("UPDATE other_table SET note = 'x' WHERE CURRENT OF c_demo"));
        assertEquals("34000", ex.getSQLState());
        cursor.close();
    }

    @Test
    void rewritesPositionedUpdateAgainstSecondaryTableWhenCursorSpansMultipleBaseTables() throws Exception {
        CaptureProtocol protocol = new CaptureProtocol();
        protocol.registerTableMetadata(100, "public", "left_demo", Map.of(1, "id", 2, "left_note"));
        protocol.registerTableMetadata(200, "public", "right_demo", Map.of(1, "id", 2, "right_note"));
        SBConnection connection = newConnectionForTest(protocol);

        SBResultSet cursor = createMultiTableMetadataCursor(connection, "c_multi",
            new Object[] {1, "l-old", 1, "r-old"});

        SBStatement statement = new SBStatement(connection, ResultSet.TYPE_FORWARD_ONLY,
            ResultSet.CONCUR_READ_ONLY, ResultSet.CLOSE_CURSORS_AT_COMMIT);
        int count = statement.executeUpdate(
            "UPDATE right_demo SET right_note = 'r-new' WHERE CURRENT OF c_multi");

        assertEquals(1, count);
        List<String> mutations = protocol.executedSql.stream()
            .filter(sql -> sql != null && sql.trim().toUpperCase().startsWith("UPDATE "))
            .toList();
        assertEquals(1, mutations.size(), "executed SQL: " + protocol.executedSql);
        String rewritten = mutations.get(0);
        assertTrue(rewritten.startsWith("UPDATE right_demo SET right_note = 'r-new' WHERE"));
        assertTrue(rewritten.contains("\"id\" = 1"), "rewritten SQL: " + rewritten);
        assertTrue(rewritten.contains("'r-old'"), "rewritten SQL: " + rewritten);
        assertTrue(!rewritten.contains("\"left_note\""), "rewritten SQL: " + rewritten);

        cursor.close();
    }

    @Test
    void rewritesPositionedDeleteAgainstSecondaryTableWhenCursorSpansMultipleBaseTables() throws Exception {
        CaptureProtocol protocol = new CaptureProtocol();
        protocol.registerTableMetadata(100, "public", "left_demo", Map.of(1, "id", 2, "left_note"));
        protocol.registerTableMetadata(200, "public", "right_demo", Map.of(1, "id", 2, "right_note"));
        SBConnection connection = newConnectionForTest(protocol);

        SBResultSet cursor = createMultiTableMetadataCursor(connection, "c_multi_delete",
            new Object[] {7, "l-keep", 7, "r-drop"});

        SBStatement statement = new SBStatement(connection, ResultSet.TYPE_FORWARD_ONLY,
            ResultSet.CONCUR_READ_ONLY, ResultSet.CLOSE_CURSORS_AT_COMMIT);
        int count = statement.executeUpdate("DELETE FROM right_demo WHERE CURRENT OF c_multi_delete");

        assertEquals(1, count);
        List<String> mutations = protocol.executedSql.stream()
            .filter(sql -> sql != null && sql.trim().toUpperCase().startsWith("DELETE "))
            .toList();
        assertEquals(1, mutations.size(), "executed SQL: " + protocol.executedSql);
        String rewritten = mutations.get(0);
        assertTrue(rewritten.startsWith("DELETE FROM right_demo WHERE"), "rewritten SQL: " + rewritten);
        assertTrue(rewritten.contains("\"id\" = 7"), "rewritten SQL: " + rewritten);
        assertTrue(rewritten.contains("'r-drop'"), "rewritten SQL: " + rewritten);
        assertTrue(!rewritten.contains("\"left_note\""), "rewritten SQL: " + rewritten);

        cursor.close();
    }

    @Test
    void positionedMutationFailsWhenSecondaryTableIdentityNotProjected() throws Exception {
        CaptureProtocol protocol = new CaptureProtocol();
        protocol.registerTableMetadata(100, "public", "left_demo", Map.of(1, "id", 2, "left_note"));
        protocol.registerTableMetadata(200, "public", "right_demo", Map.of(1, "id", 2, "right_note"));
        SBConnection connection = newConnectionForTest(protocol);
        SBResultSet cursor = createMultiTableCursorWithoutSecondaryId(connection, "c_missing_secondary_id",
            new Object[] {9, "l-available", "r-available"});

        SBStatement statement = new SBStatement(connection, ResultSet.TYPE_FORWARD_ONLY,
            ResultSet.CONCUR_READ_ONLY, ResultSet.CLOSE_CURSORS_AT_COMMIT);

        int updateCount = statement.executeUpdate(
            "UPDATE right_demo SET right_note = 'r-new' WHERE CURRENT OF c_missing_secondary_id");
        assertEquals(1, updateCount);
        List<String> updates = protocol.executedSql.stream()
            .filter(sql -> sql != null && sql.trim().toUpperCase().startsWith("UPDATE "))
            .toList();
        assertEquals(1, updates.size(), "executed SQL: " + protocol.executedSql);
        String updateSql = updates.get(0);
        assertTrue(updateSql.contains("'r-available'"), "rewritten SQL: " + updateSql);
        assertTrue(!updateSql.contains("\"left_note\""), "rewritten SQL: " + updateSql);

        int deleteCount = statement.executeUpdate("DELETE FROM right_demo WHERE CURRENT OF c_missing_secondary_id");
        assertEquals(1, deleteCount);
        List<String> deletes = protocol.executedSql.stream()
            .filter(sql -> sql != null && sql.trim().toUpperCase().startsWith("DELETE "))
            .toList();
        assertEquals(1, deletes.size(), "executed SQL: " + protocol.executedSql);
        String deleteSql = deletes.get(0);
        assertTrue(deleteSql.contains("'r-available'"), "rewritten SQL: " + deleteSql);
        assertTrue(!deleteSql.contains("\"left_note\""), "rewritten SQL: " + deleteSql);

        cursor.close();
    }

    private static SBResultSet createNamedCursor(SBConnection connection, String sql,
                                                 String cursorName, Object[] row) throws Exception {
        SBStatement cursorStatement = new SBStatement(connection, ResultSet.TYPE_SCROLL_INSENSITIVE,
            ResultSet.CONCUR_UPDATABLE, ResultSet.CLOSE_CURSORS_AT_COMMIT);
        cursorStatement.lastExecutedSql = sql;
        cursorStatement.setCursorName(cursorName);

        List<SBColumnInfo> columns = new ArrayList<>();
        SBColumnInfo id = new SBColumnInfo();
        id.setName("id");
        columns.add(id);
        SBColumnInfo note = new SBColumnInfo();
        note.setName("note");
        columns.add(note);

        SBResultSet resultSet = new SBResultSet(cursorStatement, columns,
            new ArrayList<>(Collections.singletonList(row.clone())));
        assertTrue(resultSet.next());
        connection.registerNamedCursor(cursorName, resultSet);
        return resultSet;
    }

    private static SBResultSet createMultiTableMetadataCursor(SBConnection connection,
                                                               String cursorName, Object[] row) throws Exception {
        SBStatement cursorStatement = new SBStatement(connection, ResultSet.TYPE_SCROLL_INSENSITIVE,
            ResultSet.CONCUR_UPDATABLE, ResultSet.CLOSE_CURSORS_AT_COMMIT);
        cursorStatement.lastExecutedSql =
            "SELECT l.id AS left_id, l.left_note, r.id AS right_id, r.right_note FROM public.left_demo l "
                + "JOIN public.right_demo r ON r.id = l.id";
        cursorStatement.setCursorName(cursorName);

        List<SBColumnInfo> columns = new ArrayList<>();
        SBColumnInfo leftId = new SBColumnInfo();
        leftId.setName("left_id");
        leftId.setTableOid(100);
        leftId.setColumnNumber((short) 1);
        columns.add(leftId);

        SBColumnInfo leftNote = new SBColumnInfo();
        leftNote.setName("left_note");
        leftNote.setTableOid(100);
        leftNote.setColumnNumber((short) 2);
        columns.add(leftNote);

        SBColumnInfo rightId = new SBColumnInfo();
        rightId.setName("right_id");
        rightId.setTableOid(200);
        rightId.setColumnNumber((short) 1);
        columns.add(rightId);

        SBColumnInfo rightNote = new SBColumnInfo();
        rightNote.setName("right_note");
        rightNote.setTableOid(200);
        rightNote.setColumnNumber((short) 2);
        columns.add(rightNote);

        SBResultSet resultSet = new SBResultSet(cursorStatement, columns,
            new ArrayList<>(Collections.singletonList(row.clone())));
        assertTrue(resultSet.next());
        connection.registerNamedCursor(cursorName, resultSet);
        return resultSet;
    }

    private static SBResultSet createMultiTableCursorWithoutSecondaryId(SBConnection connection,
                                                                         String cursorName, Object[] row)
            throws Exception {
        SBStatement cursorStatement = new SBStatement(connection, ResultSet.TYPE_SCROLL_INSENSITIVE,
            ResultSet.CONCUR_UPDATABLE, ResultSet.CLOSE_CURSORS_AT_COMMIT);
        cursorStatement.lastExecutedSql =
            "SELECT l.id AS left_id, l.left_note, r.right_note FROM public.left_demo l "
                + "JOIN public.right_demo r ON r.id = l.id";
        cursorStatement.setCursorName(cursorName);

        List<SBColumnInfo> columns = new ArrayList<>();
        SBColumnInfo leftId = new SBColumnInfo();
        leftId.setName("left_id");
        leftId.setTableOid(100);
        leftId.setColumnNumber((short) 1);
        columns.add(leftId);

        SBColumnInfo leftNote = new SBColumnInfo();
        leftNote.setName("left_note");
        leftNote.setTableOid(100);
        leftNote.setColumnNumber((short) 2);
        columns.add(leftNote);

        SBColumnInfo rightNote = new SBColumnInfo();
        rightNote.setName("right_note");
        rightNote.setTableOid(200);
        rightNote.setColumnNumber((short) 2);
        columns.add(rightNote);

        SBResultSet resultSet = new SBResultSet(cursorStatement, columns,
            new ArrayList<>(Collections.singletonList(row.clone())));
        assertTrue(resultSet.next());
        connection.registerNamedCursor(cursorName, resultSet);
        return resultSet;
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

    private static final class CaptureProtocol extends SBProtocolHandler {
        private final List<String> executedSql = new ArrayList<>();
        private final Map<Integer, TableMeta> tableMetaByOid = new HashMap<>();

        CaptureProtocol() {
            super(new SBConnectionProperties());
        }

        void registerTableMetadata(int oid, String schema, String table, Map<Integer, String> attNamesByNumber) {
            tableMetaByOid.put(oid, new TableMeta(schema, table, new LinkedHashMap<>(attNamesByNumber)));
        }

        @Override
        public synchronized SBQueryResult execute(String sql) throws SQLException {
            return execute(sql, 0, 0);
        }

        @Override
        public synchronized SBQueryResult execute(String sql, int maxRows, int timeoutMs) throws SQLException {
            executedSql.add(sql);
            SBQueryResult result = new SBQueryResult();
            if (sql.contains("FROM pg_catalog.pg_class")) {
                int oid = parseTrailingInteger(sql, "WHERE c.oid = ");
                TableMeta meta = tableMetaByOid.get(oid);
                if (meta != null) {
                    result.setColumns(Arrays.asList(new SBColumnInfo(), new SBColumnInfo()));
                    result.setRows(Collections.singletonList(new Object[] {meta.schema(), meta.table()}));
                    return result;
                }
            }
            if (sql.contains("FROM pg_catalog.pg_attribute")) {
                int oid = parseTrailingInteger(sql, "WHERE attrelid = ");
                TableMeta meta = tableMetaByOid.get(oid);
                if (meta != null) {
                    result.setColumns(Arrays.asList(new SBColumnInfo(), new SBColumnInfo()));
                    List<Object[]> rows = new ArrayList<>();
                    for (Map.Entry<Integer, String> entry : meta.attNamesByNumber().entrySet()) {
                        rows.add(new Object[] {entry.getKey(), entry.getValue()});
                    }
                    result.setRows(rows);
                    return result;
                }
            }
            result.setUpdateCount(1);
            return result;
        }

        private static int parseTrailingInteger(String sql, String marker) {
            int markerIndex = sql.indexOf(marker);
            if (markerIndex < 0) {
                return -1;
            }
            int start = markerIndex + marker.length();
            int end = start;
            while (end < sql.length() && Character.isDigit(sql.charAt(end))) {
                end++;
            }
            if (end <= start) {
                return -1;
            }
            try {
                return Integer.parseInt(sql.substring(start, end));
            } catch (NumberFormatException ex) {
                return -1;
            }
        }

        private record TableMeta(String schema, String table, Map<Integer, String> attNamesByNumber) {}
    }
}
