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
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.lang.reflect.Field;
import java.sql.ResultSet;
import java.sql.SQLException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import sun.misc.Unsafe;

import org.junit.jupiter.api.Test;

class SBStatementStreamingBehaviorTest {

    @Test
    void scrollInsensitiveStatementUsesBufferedExecution() throws Exception {
        TrackingProtocol protocol = new TrackingProtocol();
        SBConnection connection = newConnectionForTest(protocol);
        SBStatement statement = new SBStatement(connection, ResultSet.TYPE_SCROLL_INSENSITIVE,
            ResultSet.CONCUR_READ_ONLY, ResultSet.HOLD_CURSORS_OVER_COMMIT);
        statement.setFetchSize(32);

        try (ResultSet rs = statement.executeQuery("SELECT 1")) {
            assertTrue(rs.next());
            assertEquals(1, rs.getInt(1));
            assertTrue(rs.absolute(1));
            assertEquals(1, rs.getInt(1));
        }

        assertEquals(1, protocol.simpleExecCount);
        assertEquals(0, protocol.simpleStreamCount);
    }

    @Test
    void scrollSensitiveStatementUsesBufferedExecutionAndReportsSensitiveType() throws Exception {
        TrackingProtocol protocol = new TrackingProtocol();
        SBConnection connection = newConnectionForTest(protocol);
        SBStatement statement = new SBStatement(connection, ResultSet.TYPE_SCROLL_SENSITIVE,
            ResultSet.CONCUR_READ_ONLY, ResultSet.HOLD_CURSORS_OVER_COMMIT);
        statement.setFetchSize(32);

        try (ResultSet rs = statement.executeQuery("SELECT 1")) {
            assertEquals(ResultSet.TYPE_SCROLL_SENSITIVE, rs.getType());
            assertTrue(rs.next());
            assertEquals(1, rs.getInt(1));
            assertTrue(rs.absolute(1));
            assertEquals(1, rs.getInt(1));
        }

        assertEquals(1, protocol.simpleExecCount);
        assertEquals(0, protocol.simpleStreamCount);
    }

    @Test
    void scrollInsensitivePreparedStatementUsesBufferedExecution() throws Exception {
        TrackingProtocol protocol = new TrackingProtocol();
        SBConnection connection = newConnectionForTest(protocol);
        SBPreparedStatement statement = new SBPreparedStatement(connection, "SELECT ?",
            ResultSet.TYPE_SCROLL_INSENSITIVE, ResultSet.CONCUR_READ_ONLY,
            ResultSet.HOLD_CURSORS_OVER_COMMIT);
        statement.setFetchSize(16);
        statement.setInt(1, 42);

        try (ResultSet rs = statement.executeQuery()) {
            assertTrue(rs.next());
            assertEquals(1, rs.getInt(1));
            assertTrue(rs.absolute(1));
        }

        assertEquals(1, protocol.paramExecCount);
        assertEquals(0, protocol.paramStreamCount);
    }

    @Test
    void forwardOnlyStatementUsesStreamingWhenFetchSizeIsSet() throws Exception {
        TrackingProtocol protocol = new TrackingProtocol();
        SBConnection connection = newConnectionForTest(protocol);
        SBStatement statement = new SBStatement(connection, ResultSet.TYPE_FORWARD_ONLY,
            ResultSet.CONCUR_READ_ONLY, ResultSet.HOLD_CURSORS_OVER_COMMIT);
        statement.setFetchSize(8);

        try (ResultSet rs = statement.executeQuery("SELECT 1")) {
            assertTrue(rs.next());
            assertEquals(1, rs.getInt(1));
            assertFalse(rs.next());
            assertThrows(SQLException.class, () -> rs.absolute(1));
        }

        assertEquals(0, protocol.simpleExecCount);
        assertEquals(1, protocol.simpleStreamCount);
    }

    @Test
    void forwardOnlyUpdatableStatementUsesBufferedExecution() throws Exception {
        TrackingProtocol protocol = new TrackingProtocol();
        SBConnection connection = newConnectionForTest(protocol);
        SBStatement statement = new SBStatement(connection, ResultSet.TYPE_FORWARD_ONLY,
            ResultSet.CONCUR_UPDATABLE, ResultSet.HOLD_CURSORS_OVER_COMMIT);
        statement.setFetchSize(8);

        try (ResultSet rs = statement.executeQuery("SELECT 1")) {
            assertTrue(rs.next());
            assertEquals(1, rs.getInt(1));
            assertThrows(SQLException.class, () -> rs.absolute(1));
        }

        assertEquals(1, protocol.simpleExecCount);
        assertEquals(0, protocol.simpleStreamCount);
    }

    @Test
    void forwardOnlyBufferedResultSetStillRejectsScrollOperations() throws Exception {
        TrackingProtocol protocol = new TrackingProtocol();
        SBConnection connection = newConnectionForTest(protocol);
        SBStatement statement = new SBStatement(connection, ResultSet.TYPE_FORWARD_ONLY,
            ResultSet.CONCUR_READ_ONLY, ResultSet.HOLD_CURSORS_OVER_COMMIT);
        statement.setFetchSize(0);

        try (ResultSet rs = statement.executeQuery("SELECT 1")) {
            assertEquals(ResultSet.TYPE_FORWARD_ONLY, rs.getType());
            assertTrue(rs.next());
            assertEquals(1, rs.getInt(1));
            assertThrows(SQLException.class, () -> rs.absolute(1));
            assertThrows(SQLException.class, rs::beforeFirst);
            assertThrows(SQLException.class, rs::previous);
        }

        assertEquals(1, protocol.simpleExecCount);
        assertEquals(0, protocol.simpleStreamCount);
    }

    @Test
    void forwardOnlyUpdatablePreparedStatementUsesBufferedExecution() throws Exception {
        TrackingProtocol protocol = new TrackingProtocol();
        SBConnection connection = newConnectionForTest(protocol);
        SBPreparedStatement statement = new SBPreparedStatement(connection, "SELECT ?",
            ResultSet.TYPE_FORWARD_ONLY, ResultSet.CONCUR_UPDATABLE,
            ResultSet.HOLD_CURSORS_OVER_COMMIT);
        statement.setFetchSize(16);
        statement.setInt(1, 42);

        try (ResultSet rs = statement.executeQuery()) {
            assertTrue(rs.next());
            assertEquals(1, rs.getInt(1));
            assertThrows(SQLException.class, () -> rs.absolute(1));
        }

        assertEquals(1, protocol.paramExecCount);
        assertEquals(0, protocol.paramStreamCount);
    }

    @Test
    void parameterlessPreparedStatementAvoidsInlineStreamingPathWhenConnected() throws Exception {
        ConnectedTrackingProtocol protocol = new ConnectedTrackingProtocol();
        SBConnection connection = newConnectionForTest(protocol);
        SBPreparedStatement statement = new SBPreparedStatement(connection, "SELECT 1",
            ResultSet.TYPE_FORWARD_ONLY, ResultSet.CONCUR_READ_ONLY,
            ResultSet.HOLD_CURSORS_OVER_COMMIT);
        statement.setFetchSize(16);

        try (ResultSet rs = statement.executeQuery()) {
            assertTrue(rs.next());
            assertEquals(1, rs.getInt(1));
        }

        assertEquals(0, protocol.simpleStreamCount);
        assertEquals(1, protocol.paramStreamCount);
        assertEquals(0, protocol.noCacheExecCount);
    }

    @Test
    void closingStreamingResultSetClosesUnderlyingRowStream() throws Exception {
        TrackingProtocol protocol = new TrackingProtocol();
        CloseTrackingRowStream stream = new CloseTrackingRowStream();
        protocol.streamingRowStream = stream;
        SBConnection connection = newConnectionForTest(protocol);
        SBStatement statement = new SBStatement(connection, ResultSet.TYPE_FORWARD_ONLY,
            ResultSet.CONCUR_READ_ONLY, ResultSet.HOLD_CURSORS_OVER_COMMIT);
        statement.setFetchSize(8);

        ResultSet rs = statement.executeQuery("SELECT 1");
        assertTrue(rs.next());
        rs.close();

        assertEquals(1, stream.closeCalls);
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

    private static SBQueryResult bufferedResult() {
        SBQueryResult result = new SBQueryResult();
        List<SBColumnInfo> columns = new ArrayList<>();
        SBColumnInfo col = new SBColumnInfo();
        col.setName("value");
        columns.add(col);
        result.setColumns(columns);
        result.setRows(Collections.singletonList(new Object[] {1}));
        result.setUpdateCount(-1);
        return result;
    }

    private static class TrackingProtocol extends SBProtocolHandler {
        int simpleExecCount;
        int simpleStreamCount;
        int paramExecCount;
        int paramStreamCount;
        SBRowStream streamingRowStream = new StubRowStream();

        TrackingProtocol() {
            super(new SBConnectionProperties());
        }

        @Override
        public SBQueryResult execute(String sql, int maxRows, int timeoutMs) {
            simpleExecCount++;
            return bufferedResult();
        }

        @Override
        public SBQueryResult execute(String sql, List<Object> params, List<Integer> paramTypes,
                                     int maxRows, int timeoutMs) {
            paramExecCount++;
            return bufferedResult();
        }

        @Override
        public SBQueryResult executeStreaming(String sql, int pageSize, int timeoutMs) {
            simpleStreamCount++;
            SBQueryResult result = new SBQueryResult();
            result.setStream(streamingRowStream);
            return result;
        }

        @Override
        public SBQueryResult executeStreaming(String sql, List<Object> params, List<Integer> paramTypes,
                                              int pageSize, int timeoutMs) {
            paramStreamCount++;
            SBQueryResult result = new SBQueryResult();
            result.setStream(streamingRowStream);
            return result;
        }
    }

    private static final class ConnectedTrackingProtocol extends TrackingProtocol {
        int noCacheExecCount;

        @Override
        public synchronized boolean isConnected() {
            return true;
        }

        @Override
        public SBQueryResult executeNoCache(String sql, int maxRows, int timeoutMs) {
            noCacheExecCount++;
            return bufferedResult();
        }
    }

    private static class StubRowStream implements SBRowStream {
        private final List<SBColumnInfo> columns;
        private boolean emitted = false;

        StubRowStream() {
            SBColumnInfo col = new SBColumnInfo();
            col.setName("value");
            this.columns = Collections.singletonList(col);
        }

        @Override
        public Object[] nextRow() {
            if (emitted) {
                return null;
            }
            emitted = true;
            return new Object[] {1};
        }

        @Override
        public List<SBColumnInfo> getColumns() {
            return columns;
        }

        @Override
        public long getUpdateCount() {
            return -1;
        }

        @Override
        public String getCommandTag() {
            return "SELECT";
        }

        @Override
        public boolean isDone() {
            return emitted;
        }
    }

    private static final class CloseTrackingRowStream extends StubRowStream {
        int closeCalls;

        @Override
        public void close() {
            closeCalls++;
        }
    }
}
