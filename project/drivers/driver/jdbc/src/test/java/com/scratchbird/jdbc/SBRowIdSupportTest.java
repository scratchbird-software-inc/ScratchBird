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
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.lang.reflect.Field;
import java.nio.charset.StandardCharsets;
import java.sql.ResultSet;
import java.sql.Types;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import sun.misc.Unsafe;

import org.junit.jupiter.api.Test;

public class SBRowIdSupportTest {

    @Test
    public void preparedStatementSetRowIdUsesRowIdType() throws Exception {
        CaptureProtocol protocol = new CaptureProtocol();
        SBConnection connection = newConnectionForTest(protocol);

        SBPreparedStatement statement = new SBPreparedStatement(connection,
            "INSERT INTO demo(id) VALUES (?)",
            ResultSet.TYPE_FORWARD_ONLY,
            ResultSet.CONCUR_READ_ONLY,
            ResultSet.CLOSE_CURSORS_AT_COMMIT);

        SBRowId rowId = new SBRowId("rid-42".getBytes(StandardCharsets.UTF_8));
        statement.setRowId(1, rowId);
        statement.executeUpdate();

        assertEquals("rid-42", protocol.lastParams.get(0));
        assertEquals(Types.ROWID, protocol.lastTypes.get(0));
    }

    @Test
    public void resultSetGetRowIdConvertsTextToRowId() throws Exception {
        CaptureProtocol protocol = new CaptureProtocol();
        SBConnection connection = newConnectionForTest(protocol);
        SBStatement statement = new SBStatement(connection, ResultSet.TYPE_SCROLL_INSENSITIVE,
            ResultSet.CONCUR_READ_ONLY, ResultSet.CLOSE_CURSORS_AT_COMMIT);

        SBColumnInfo col = new SBColumnInfo();
        col.setName("id");
        SBResultSet rs = new SBResultSet(statement, Collections.singletonList(col),
            Collections.singletonList(new Object[] {"rid-7"}));

        assertTrue(rs.next());
        assertArrayEquals("rid-7".getBytes(StandardCharsets.UTF_8), rs.getRowId(1).getBytes());
        assertArrayEquals("rid-7".getBytes(StandardCharsets.UTF_8), rs.getRowId("id").getBytes());
        assertFalse(rs.next());
    }

    @Test
    public void callableStatementGetRowIdHydratesOutParam() throws Exception {
        OutRowIdProtocol protocol = new OutRowIdProtocol();
        SBConnection connection = newConnectionForTest(protocol);
        SBCallableStatement statement = new SBCallableStatement(connection, "CALL demo(?)",
            ResultSet.TYPE_SCROLL_INSENSITIVE, ResultSet.CONCUR_READ_ONLY,
            ResultSet.CLOSE_CURSORS_AT_COMMIT);

        statement.registerOutParameter(1, Types.ROWID);
        statement.execute();

        assertArrayEquals("rid-out".getBytes(StandardCharsets.UTF_8), statement.getRowId(1).getBytes());
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

    private static final class CaptureProtocol extends SBProtocolHandler {
        List<Object> lastParams = Collections.emptyList();
        List<Integer> lastTypes = Collections.emptyList();

        CaptureProtocol() {
            super(new SBConnectionProperties());
        }

        @Override
        public SBQueryResult execute(String sql, List<Object> params, List<Integer> paramTypes,
                                     int maxRows, int timeoutMs) {
            this.lastParams = new ArrayList<>(params);
            this.lastTypes = new ArrayList<>(paramTypes);
            SBQueryResult result = new SBQueryResult();
            result.setUpdateCount(1);
            return result;
        }
    }

    private static final class OutRowIdProtocol extends SBProtocolHandler {
        OutRowIdProtocol() {
            super(new SBConnectionProperties());
        }

        @Override
        public SBQueryResult execute(String sql, List<Object> params, List<Integer> paramTypes,
                                     int maxRows, int timeoutMs) {
            SBQueryResult result = new SBQueryResult();
            List<SBColumnInfo> columns = new ArrayList<>();
            SBColumnInfo col = new SBColumnInfo();
            col.setName("out_id");
            columns.add(col);
            result.setColumns(columns);
            result.setRows(Collections.singletonList(new Object[] {"rid-out"}));
            result.setUpdateCount(0);
            return result;
        }
    }
}
