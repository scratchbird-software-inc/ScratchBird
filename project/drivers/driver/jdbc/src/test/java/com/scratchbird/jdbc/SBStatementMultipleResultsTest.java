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

import java.lang.reflect.Field;
import java.sql.ResultSet;
import java.sql.Statement;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;
import org.junit.jupiter.api.Test;
import sun.misc.Unsafe;

class SBStatementMultipleResultsTest {

    @Test
    void executeSupportsMultipleResultsAndKeepCurrentResultSemantics() throws Exception {
        MultiResultProtocol protocol = new MultiResultProtocol();
        SBConnection connection = newConnectionForTest(protocol);
        SBStatement statement = new SBStatement(connection, ResultSet.TYPE_SCROLL_INSENSITIVE,
            ResultSet.CONCUR_READ_ONLY, ResultSet.CLOSE_CURSORS_AT_COMMIT);

        boolean hasResultSet = statement.execute("SELECT 1; SELECT 2; UPDATE demo SET value = 3");
        assertTrue(hasResultSet);

        ResultSet first = statement.getResultSet();
        assertTrue(first.next());
        assertEquals(1, first.getInt(1));

        assertTrue(statement.getMoreResults(Statement.KEEP_CURRENT_RESULT));
        ResultSet second = statement.getResultSet();
        assertTrue(second.next());
        assertEquals(2, second.getInt(1));
        assertFalse(first.isClosed());

        assertFalse(statement.getMoreResults(Statement.CLOSE_CURRENT_RESULT));
        assertEquals(1, statement.getUpdateCount());
        assertTrue(second.isClosed());

        assertFalse(statement.getMoreResults(Statement.CLOSE_ALL_RESULTS));
        assertTrue(first.isClosed());
        assertFalse(statement.getMoreResults());
        assertEquals(-1, statement.getUpdateCount());
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

    private static final class MultiResultProtocol extends SBProtocolHandler {
        private final List<SBColumnInfo> singleColumn;

        MultiResultProtocol() {
            super(new SBConnectionProperties());
            SBColumnInfo col = new SBColumnInfo();
            col.setName("?column?");
            this.singleColumn = Collections.singletonList(col);
        }

        @Override
        public SBQueryResult execute(String sql, int maxRows, int timeoutMs) {
            String normalized = sql.trim();
            if (normalized.equalsIgnoreCase("SELECT 1")) {
                return queryResult(1);
            }
            if (normalized.equalsIgnoreCase("SELECT 2")) {
                return queryResult(2);
            }

            SBQueryResult update = new SBQueryResult();
            update.setColumns(new ArrayList<>());
            update.setRows(new ArrayList<>());
            update.setUpdateCount(1);
            return update;
        }

        private SBQueryResult queryResult(int value) {
            SBQueryResult result = new SBQueryResult();
            result.setColumns(singleColumn);
            result.setRows(Collections.singletonList(new Object[]{value}));
            result.setUpdateCount(-1);
            return result;
        }
    }
}
