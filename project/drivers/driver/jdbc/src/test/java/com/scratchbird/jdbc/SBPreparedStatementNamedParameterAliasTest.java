// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package com.scratchbird.jdbc;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.junit.jupiter.api.Assertions.assertFalse;

import java.lang.reflect.Field;
import java.sql.ResultSet;
import java.sql.ResultSetMetaData;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.atomic.AtomicBoolean;
import org.junit.jupiter.api.Test;
import sun.misc.Unsafe;

class SBPreparedStatementNamedParameterAliasTest {

    @Test
    void duplicateNamedPlaceholdersShareBoundValueForPreparedStatement() throws Exception {
        CaptureProtocol protocol = new CaptureProtocol();
        SBConnection connection = newConnectionForTest(protocol);

        SBPreparedStatement stmt = new SBPreparedStatement(connection,
            "SELECT id, note FROM demo WHERE id = :id OR parent_id = :id",
            ResultSet.TYPE_FORWARD_ONLY,
            ResultSet.CONCUR_READ_ONLY,
            ResultSet.CLOSE_CURSORS_AT_COMMIT);

        stmt.setInt(1, 41);
        try (ResultSet rs = stmt.executeQuery()) {
            assertTrue(rs.next());
            ResultSetMetaData meta = rs.getMetaData();
            assertEquals("id", meta.getColumnName(1).toLowerCase(Locale.ROOT));
        }

        assertEquals(2, protocol.lastParams.size());
        assertEquals(41, protocol.lastParams.get(0));
        assertEquals(41, protocol.lastParams.get(1));

        stmt.setInt(2, 77);
        stmt.executeQuery().close();
        assertEquals(2, protocol.lastParams.size());
        assertEquals(77, protocol.lastParams.get(0));
        assertEquals(77, protocol.lastParams.get(1));
    }

    @Test
    void callableNamedSettersPropagateAcrossDuplicateNamedPlaceholders() throws Exception {
        CaptureProtocol protocol = new CaptureProtocol();
        SBConnection connection = newConnectionForTest(protocol);

        SBCallableStatement stmt = new SBCallableStatement(connection,
            "{call demo_proc(:id, :id)}",
            ResultSet.TYPE_FORWARD_ONLY,
            ResultSet.CONCUR_READ_ONLY,
            ResultSet.CLOSE_CURSORS_AT_COMMIT);

        stmt.setInt("id", 9);
        stmt.execute();

        assertEquals(2, protocol.lastParams.size());
        assertEquals(9, protocol.lastParams.get(0));
        assertEquals(9, protocol.lastParams.get(1));
    }

    @Test
    void parseSqlIgnoresPlaceholderTokensInsideCommentsAndQuotedLiterals() throws Exception {
        CaptureProtocol protocol = new CaptureProtocol();
        SBConnection connection = newConnectionForTest(protocol);

        SBPreparedStatement stmt = new SBPreparedStatement(connection,
            "SELECT ? AS a, :id AS b, @id AS c, ? AS d, " +
                "'? literal', \"@id ident\", $$ ? :id @id $$, " +
                "$tag$ ? :id @id $tag$ -- ? :ignored @ignored\n" +
                "/* ? :ignored2 @ignored2 */",
            ResultSet.TYPE_FORWARD_ONLY,
            ResultSet.CONCUR_READ_ONLY,
            ResultSet.CLOSE_CURSORS_AT_COMMIT);

        stmt.setInt(1, 11);
        stmt.setInt(2, 22);
        stmt.setInt(4, 44);
        try (ResultSet rs = stmt.executeQuery()) {
            assertTrue(rs.next());
        }

        assertEquals(4, protocol.lastParams.size());
        assertEquals(11, protocol.lastParams.get(0));
        assertEquals(22, protocol.lastParams.get(1));
        assertEquals(22, protocol.lastParams.get(2));
        assertEquals(44, protocol.lastParams.get(3));
        assertTrue(protocol.lastSql.contains("$1"));
        assertTrue(protocol.lastSql.contains("$2"));
        assertTrue(protocol.lastSql.contains("$3"));
        assertTrue(protocol.lastSql.contains("$4"));
        assertFalse(protocol.lastSql.contains("$5"));
    }

    @Test
    void parseSqlIgnoresPlaceholderTokensInsideTaggedDollarQuotes() throws Exception {
        CaptureProtocol protocol = new CaptureProtocol();
        SBConnection connection = newConnectionForTest(protocol);

        SBPreparedStatement stmt = new SBPreparedStatement(connection,
            "SELECT :id AS id, $abc$ @id ? :id $abc$ AS payload, ? AS tail",
            ResultSet.TYPE_FORWARD_ONLY,
            ResultSet.CONCUR_READ_ONLY,
            ResultSet.CLOSE_CURSORS_AT_COMMIT);

        stmt.setInt(1, 7);
        stmt.setInt(2, 8);
        try (ResultSet rs = stmt.executeQuery()) {
            assertTrue(rs.next());
        }

        assertEquals(2, protocol.lastParams.size());
        assertEquals(7, protocol.lastParams.get(0));
        assertEquals(8, protocol.lastParams.get(1));
        assertTrue(protocol.lastSql.contains("$1"));
        assertTrue(protocol.lastSql.contains("$2"));
        assertFalse(protocol.lastSql.contains("$3"));
    }

    @Test
    void parseSqlPreservesPostgresCastDoubleColonWithoutCreatingPhantomNamedParameter() throws Exception {
        CaptureProtocol protocol = new CaptureProtocol();
        SBConnection connection = newConnectionForTest(protocol);

        SBPreparedStatement stmt = new SBPreparedStatement(connection,
            "SELECT :id::INTEGER AS casted, ? AS q",
            ResultSet.TYPE_FORWARD_ONLY,
            ResultSet.CONCUR_READ_ONLY,
            ResultSet.CLOSE_CURSORS_AT_COMMIT);

        stmt.setInt(1, 31);
        stmt.setInt(2, 99);
        try (ResultSet rs = stmt.executeQuery()) {
            assertTrue(rs.next());
        }

        assertEquals(2, protocol.lastParams.size());
        assertEquals(31, protocol.lastParams.get(0));
        assertEquals(99, protocol.lastParams.get(1));
        assertTrue(protocol.lastSql.contains("$1::INTEGER"));
        assertTrue(protocol.lastSql.contains("$2"));
        assertFalse(protocol.lastSql.contains("$3"));
    }

    private static SBConnection newConnectionForTest(SBProtocolHandler protocol) throws Exception {
        SBConnection connection = (SBConnection) getUnsafe().allocateInstance(SBConnection.class);
        setField(connection, "protocol", protocol);
        setField(connection, "properties", new SBConnectionProperties());
        setField(connection, "closed", new AtomicBoolean(false));
        setField(connection, "circuitBreaker", new CircuitBreaker());
        setField(connection, "telemetry", new TelemetryCollector());
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
        private List<Object> lastParams = List.of();
        private String lastSql = "";

        CaptureProtocol() {
            super(new SBConnectionProperties());
        }

        @Override
        public synchronized SBQueryResult execute(String sql, List<Object> params, List<Integer> paramTypes,
                                                  int maxRows, int timeoutMs) {
            this.lastSql = sql == null ? "" : sql;
            this.lastParams = params == null ? List.of() : new ArrayList<>(params);
            SBQueryResult result = new SBQueryResult();
            if (sql != null && sql.toLowerCase(Locale.ROOT).startsWith("select")) {
                List<SBColumnInfo> columns = new ArrayList<>();
                SBColumnInfo id = new SBColumnInfo();
                id.setName("id");
                columns.add(id);
                SBColumnInfo note = new SBColumnInfo();
                note.setName("note");
                columns.add(note);
                result.setColumns(columns);
                result.setRows(Collections.singletonList(new Object[] {1, "ok"}));
                result.setUpdateCount(-1);
                return result;
            }
            result.setUpdateCount(1);
            return result;
        }
    }
}
