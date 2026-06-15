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
import java.sql.SQLFeatureNotSupportedException;
import java.sql.SQLException;
import java.sql.SQLSyntaxErrorException;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;
import org.junit.jupiter.api.Test;
import sun.misc.Unsafe;

class SBConnectionTransactionModeTest {

    @Test
    void setAutoCommitTrueSkipsCommitWhenNoActiveTransaction() throws Exception {
        TrackingProtocol protocol = new TrackingProtocol();
        protocol.activeTransaction = false;
        SBConnection connection = newConnectionForTest(protocol, false);

        connection.setAutoCommit(true);

        assertEquals(0, protocol.commitCalls);
        assertFalse(protocol.executedSql.stream().anyMatch(sql -> sql.toUpperCase().contains("AUTOCOMMIT")));
        assertTrue(connection.getAutoCommit());
    }

    @Test
    void setAutoCommitTrueCommitsWhenActiveTransactionExists() throws Exception {
        TrackingProtocol protocol = new TrackingProtocol();
        protocol.activeTransaction = true;
        SBConnection connection = newConnectionForTest(protocol, false);

        connection.setAutoCommit(true);

        assertEquals(1, protocol.commitCalls);
        assertTrue(connection.getAutoCommit());
        assertFalse(protocol.activeTransaction);
    }

    @Test
    void setAutoCommitFalseDoesNotBeginWhenServerAlreadyHasTransaction() throws Exception {
        TrackingProtocol protocol = new TrackingProtocol();
        protocol.activeTransaction = true;
        SBConnection connection = newConnectionForTest(protocol, true);

        connection.setAutoCommit(false);

        assertEquals(0, protocol.beginCalls);
        assertFalse(protocol.executedSql.stream().anyMatch(sql -> sql.toUpperCase().contains("AUTOCOMMIT")));
        assertFalse(connection.getAutoCommit());
    }

    @Test
    void commitAndRollbackAreNoOpsWhenNoActiveTransaction() throws Exception {
        TrackingProtocol protocol = new TrackingProtocol();
        protocol.activeTransaction = false;
        SBConnection connection = newConnectionForTest(protocol, false);

        connection.commit();
        connection.rollback();

        assertEquals(0, protocol.commitCalls);
        assertEquals(0, protocol.rollbackCalls);
    }

    @Test
    void setTransactionIsolationWithReadCommittedModeUsesCanonicalSelector() throws Exception {
        TrackingProtocol protocol = new TrackingProtocol();
        SBConnection connection = newConnectionForTest(protocol, true);

        connection.setTransactionIsolation(
            SBConnection.TRANSACTION_READ_COMMITTED,
            SBConnection.READ_COMMITTED_MODE_READ_CONSISTENCY
        );

        assertEquals(
            "SET TRANSACTION ISOLATION LEVEL READ COMMITTED READ CONSISTENCY",
            protocol.executedSql.get(protocol.executedSql.size() - 1)
        );
        assertEquals(SBConnection.READ_COMMITTED_MODE_READ_CONSISTENCY, connection.getReadCommittedMode());
        assertEquals("READ COMMITTED READ CONSISTENCY",
            SBConnection.canonicalReadCommittedModeLabel(connection.getReadCommittedMode()));
    }

    @Test
    void setReadCommittedModeRejectsSnapshotAliases() throws Exception {
        TrackingProtocol protocol = new TrackingProtocol();
        SBConnection connection = newConnectionForTest(protocol, true);
        connection.setTransactionIsolation(SBConnection.TRANSACTION_SERIALIZABLE);

        SQLFeatureNotSupportedException ex = assertThrows(
            SQLFeatureNotSupportedException.class,
            () -> connection.setReadCommittedMode(SBConnection.READ_COMMITTED_MODE_READ_CONSISTENCY)
        );

        assertEquals("0A000", ex.getSQLState());
    }

    @Test
    void setAutoCommitFalseBeginsManagedTransactionWithConfiguredReadCommittedMode() throws Exception {
        TrackingProtocol protocol = new TrackingProtocol();
        protocol.activeTransaction = false;
        SBConnection connection = newConnectionForTest(protocol, true);
        connection.setReadCommittedMode(SBConnection.READ_COMMITTED_MODE_READ_CONSISTENCY);

        connection.setAutoCommit(false);

        assertEquals(1, protocol.beginCalls);
        assertFalse(protocol.executedSql.stream().anyMatch(sql -> sql.toUpperCase().contains("AUTOCOMMIT")));
        assertEquals(SBProtocolHandler.ISOLATION_READ_COMMITTED, protocol.lastIsolationLevel);
        assertEquals(SBConnection.READ_COMMITTED_MODE_READ_CONSISTENCY, protocol.lastReadCommittedMode);
    }

    @Test
    void preparedTransactionHelpersEmitCanonicalControlSql() throws Exception {
        TrackingProtocol protocol = new TrackingProtocol();
        SBConnection connection = newConnectionForTest(protocol, true);

        connection.prepareTransaction("gid-1");
        connection.commitPrepared("gid-1");
        connection.rollbackPrepared("gid'2");

        assertTrue(protocol.executedSql.contains("PREPARE TRANSACTION 'gid-1'"));
        assertTrue(protocol.executedSql.contains("COMMIT PREPARED 'gid-1'"));
        assertTrue(protocol.executedSql.contains("ROLLBACK PREPARED 'gid''2'"));
    }

    @Test
    void preparedTransactionHelpersRejectEmptyGid() {
        SQLSyntaxErrorException ex = assertThrows(
            SQLSyntaxErrorException.class,
            () -> SBConnection.buildPreparedTransactionSql("PREPARE TRANSACTION", "   ")
        );

        assertEquals("42601", ex.getSQLState());
    }

    @Test
    void dormantHelpersFailClosedAndCapabilitiesStayExplicit() throws Exception {
        TrackingProtocol protocol = new TrackingProtocol();
        SBConnection connection = newConnectionForTest(protocol, true);

        assertTrue(connection.supportsPreparedTransactions());
        assertFalse(connection.supportsDormantReattach());

        SQLFeatureNotSupportedException detach = assertThrows(
            SQLFeatureNotSupportedException.class,
            connection::detachToDormant
        );
        assertEquals("0A000", detach.getSQLState());

        SQLFeatureNotSupportedException reattach = assertThrows(
            SQLFeatureNotSupportedException.class,
            () -> connection.reattachDormant("dormant-1", "token-1")
        );
        assertEquals("0A000", reattach.getSQLState());
    }

    @Test
    void resetForPoolReuseClearsCircuitBreakerAfterSuccessfulReset() throws Exception {
        TrackingProtocol protocol = new TrackingProtocol();
        SBConnection connection = newConnectionForTest(protocol, true);
        CircuitBreaker breaker = (CircuitBreaker) getField(connection, "circuitBreaker");
        SBConnectionProperties baseline = new SBConnectionProperties();
        baseline.setAutoCommit(true);
        baseline.setCurrentSchema("public");

        for (int i = 0; i < CircuitBreakerConfig.DEFAULT_FAILURE_THRESHOLD; i++) {
            breaker.recordFailure();
        }

        assertEquals(CircuitState.OPEN, breaker.getState());

        connection.resetForPoolReuse(baseline);

        assertEquals(CircuitState.CLOSED, breaker.getState());
    }

    private static SBConnection newConnectionForTest(SBProtocolHandler protocol, boolean autoCommit) throws Exception {
        SBConnection connection = (SBConnection) getUnsafe().allocateInstance(SBConnection.class);
        setField(connection, "protocol", protocol);
        setField(connection, "properties", new SBConnectionProperties());
        setField(connection, "closed", new AtomicBoolean(false));
        setField(connection, "circuitBreaker", new CircuitBreaker());
        setField(connection, "telemetry", new TelemetryCollector());
        setField(connection, "readOnly", false);
        setField(connection, "autoCommit", autoCommit);
        setField(connection, "transactionIsolation", SBConnection.TRANSACTION_READ_COMMITTED);
        setField(connection, "readCommittedMode", (byte) SBConnection.READ_COMMITTED_MODE_DEFAULT);
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

    private static Object getField(Object object, String fieldName) throws Exception {
        Field field = SBConnection.class.getDeclaredField(fieldName);
        field.setAccessible(true);
        return field.get(object);
    }

    private static final class TrackingProtocol extends SBProtocolHandler {
        private final List<String> executedSql = new ArrayList<>();
        private int beginCalls;
        private int commitCalls;
        private int rollbackCalls;
        private int lastIsolationLevel = -1;
        private int lastReadCommittedMode = -1;
        private boolean activeTransaction;

        TrackingProtocol() {
            super(new SBConnectionProperties());
        }

        @Override
        public synchronized SBQueryResult execute(String sql) throws SQLException {
            return execute(sql, 0, 0);
        }

        @Override
        public synchronized SBQueryResult execute(String sql, int maxRows, int timeoutMs) throws SQLException {
            executedSql.add(sql);
            return new SBQueryResult();
        }

        @Override
        public synchronized boolean hasActiveTransaction() {
            return activeTransaction;
        }

        @Override
        public synchronized void beginTransaction() throws SQLException {
            beginCalls++;
            activeTransaction = true;
        }

        @Override
        public synchronized void beginTransaction(byte isolationLevel, byte accessMode, boolean deferrable,
                                                  boolean wait, int timeoutMs, byte autocommitMode,
                                                  byte conflictAction, byte readCommittedMode) throws SQLException {
            beginCalls++;
            activeTransaction = true;
            lastIsolationLevel = Byte.toUnsignedInt(isolationLevel);
            lastReadCommittedMode = Byte.toUnsignedInt(readCommittedMode);
        }

        @Override
        public synchronized void commitTransaction(byte flags) throws SQLException {
            commitCalls++;
            activeTransaction = false;
        }

        @Override
        public synchronized void rollbackTransaction(byte flags) throws SQLException {
            rollbackCalls++;
            activeTransaction = false;
        }
    }
}
