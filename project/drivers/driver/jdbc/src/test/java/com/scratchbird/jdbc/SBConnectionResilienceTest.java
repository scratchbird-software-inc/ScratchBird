// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

/*
 * ScratchBird JDBC Driver
 * Copyright (c) 2025 ScratchBird Project
 */
package com.scratchbird.jdbc;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.lang.reflect.Field;
import java.sql.SQLDataException;
import java.sql.SQLTransientConnectionException;
import java.util.Collections;
import java.util.concurrent.atomic.AtomicInteger;
import sun.misc.Unsafe;

import org.junit.jupiter.api.Test;

public class SBConnectionResilienceTest {

    @Test
    public void replayableQueryRetriesAfterTransientFailure() throws Exception {
        var protocol = new FailingReplayProtocol();
        SBConnection connection = newConnectionForTest(protocol);

        AtomicInteger attempts = new AtomicInteger();
        String result = connection.withResilience("query", "SELECT 1", () -> {
            attempts.incrementAndGet();
            if (attempts.get() == 1) {
                throw new SQLTransientConnectionException("transport reset", "08006");
            }
            return "ok";
        }, true);

        assertEquals(2, attempts.get());
        assertEquals(1, protocol.connectAttempts.get());
        assertEquals("ok", result);
    }

    @Test
    public void nonReplayableOperationDoesNotRetryAfterTransientFailure() throws Exception {
        var protocol = new FailingReplayProtocol();
        SBConnection connection = newConnectionForTest(protocol);

        AtomicInteger attempts = new AtomicInteger();
        assertThrows(SQLTransientConnectionException.class, () ->
            connection.withResilience("update", "UPDATE test", () -> {
                attempts.incrementAndGet();
                throw new SQLTransientConnectionException("transport reset", "08006");
            }, false)
        );

        assertEquals(1, attempts.get());
        assertEquals(0, protocol.connectAttempts.get());
    }

    @Test
    public void statementLevelSqlErrorsDoNotOpenCircuitBreaker() throws Exception {
        var protocol = new FailingReplayProtocol();
        SBConnection connection = newConnectionForTest(protocol);

        for (int i = 0; i < 10; i++) {
            assertThrows(SQLDataException.class, () ->
                connection.withResilience("execute", "SELECT bad_numeric()", () -> {
                    throw new SQLDataException("numeric domain", "22003");
                }, false)
            );
        }

        String result = connection.withResilience("query", "SELECT 1", () -> "ok", false);
        assertEquals("ok", result);
        assertEquals(0, protocol.connectAttempts.get());
    }

    @Test
    public void failoverReconnectInvalidatesNamedCursorRegistry() throws Exception {
        var protocol = new FailingReplayProtocol();
        SBConnection connection = newConnectionForTest(protocol);
        SBResultSet resultSet = new SBResultSet(null, Collections.emptyList(), Collections.emptyList());
        connection.registerNamedCursor("replay_cursor", resultSet);

        assertEquals("replay_cursor", resultSet.getCursorName());

        AtomicInteger attempts = new AtomicInteger();
        String result = connection.withResilience("query", "SELECT 1", () -> {
            attempts.incrementAndGet();
            if (attempts.get() == 1) {
                throw new SQLTransientConnectionException("transport reset", "08006");
            }
            return "ok";
        }, true);

        assertEquals("ok", result);
        assertEquals(2, attempts.get());
        assertEquals(1, protocol.connectAttempts.get());
        assertNull(connection.resolveNamedCursor("replay_cursor"));
        assertNull(resultSet.getCursorName());
    }

    private static SBConnection newConnectionForTest(SBProtocolHandler protocol) throws Exception {
        SBConnection connection = (SBConnection) getUnsafe().allocateInstance(SBConnection.class);
        setField(connection, "protocol", protocol);
        setField(connection, "properties", new SBConnectionProperties());
        setField(connection, "closed", new java.util.concurrent.atomic.AtomicBoolean(false));
        setField(connection, "circuitBreaker", new CircuitBreaker());
        setField(connection, "telemetry", new TelemetryCollector());
        setField(connection, "namedCursors", new java.util.concurrent.ConcurrentHashMap<String, SBResultSet>());
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

    private static final class FailingReplayProtocol extends SBProtocolHandler {
        final AtomicInteger connectAttempts = new AtomicInteger();
        final AtomicInteger executeAttempts = new AtomicInteger();

        FailingReplayProtocol() {
            super(new SBConnectionProperties());
        }

        @Override
        public void connect() {
            connectAttempts.incrementAndGet();
        }

        @Override
        public void close() {
            // No-op in this test
        }

        @Override
        public SBQueryResult execute(String sql) {
            executeAttempts.incrementAndGet();
            return new SBQueryResult();
        }
    }
}
