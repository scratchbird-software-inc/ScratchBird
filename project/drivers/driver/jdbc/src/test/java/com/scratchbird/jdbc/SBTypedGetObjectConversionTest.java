// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package com.scratchbird.jdbc;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotNull;

import java.lang.reflect.Field;
import java.net.URL;
import java.sql.ResultSet;
import java.sql.RowId;
import java.sql.SQLXML;
import java.sql.Types;
import java.time.OffsetDateTime;
import java.time.OffsetTime;
import java.time.ZoneOffset;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import org.junit.jupiter.api.Test;
import sun.misc.Unsafe;

class SBTypedGetObjectConversionTest {

    @Test
    void resultSetTypedGetObjectCoversStandardJdbcTargets() throws Exception {
        SBConnection connection = newConnectionForTest(new OutProtocol(new Object[]{}));
        SBStatement statement = new SBStatement(connection, ResultSet.TYPE_SCROLL_INSENSITIVE,
            ResultSet.CONCUR_READ_ONLY, ResultSet.CLOSE_CURSORS_AT_COMMIT);

        List<SBColumnInfo> columns = new ArrayList<>();
        columns.add(column("int_col"));
        columns.add(column("url_col"));
        columns.add(column("xml_col"));
        columns.add(column("ref_col"));
        columns.add(column("rowid_col"));
        columns.add(column("bool_col"));
        columns.add(column("timetz_col"));
        columns.add(column("timestamptz_col"));

        List<Object[]> rows = new ArrayList<>();
        rows.add(new Object[] {
            "42",
            "https://example.com",
            "<doc/>",
            "rid-1",
            "rid-2",
            "true",
            OffsetTime.of(6, 7, 8, 0, ZoneOffset.ofHours(-4)),
            OffsetDateTime.parse("2026-03-01T12:34:56+02:00")
        });
        SBResultSet rs = new SBResultSet(statement, columns, rows);

        rs.next();
        assertEquals(42, rs.getObject(1, Integer.class));
        assertEquals(42, rs.getObject(1, int.class));
        assertEquals("https://example.com", rs.getObject(2, URL.class).toString());
        assertNotNull(rs.getObject(3, SQLXML.class));
        assertEquals("<doc/>", rs.getObject(3, SQLXML.class).getString());
        assertEquals("rid-1", rs.getObject(4, java.sql.Ref.class).getObject());
        assertEquals("rid-2", new String(rs.getObject(5, RowId.class).getBytes()));
        assertEquals(Boolean.TRUE, rs.getObject(6, Boolean.class));
        assertEquals(Boolean.TRUE, rs.getObject(6, boolean.class));
        assertEquals(OffsetTime.of(6, 7, 8, 0, ZoneOffset.ofHours(-4)),
            rs.getObject(7, OffsetTime.class));
        assertEquals(OffsetDateTime.parse("2026-03-01T12:34:56+02:00").toInstant(),
            rs.getObject(8, java.time.ZonedDateTime.class).toInstant());
    }

    @Test
    void callableTypedGetObjectCoversCoreConversions() throws Exception {
        OutProtocol protocol = new OutProtocol(new Object[] {"7", "https://scratchbird.dev", "<xml/>", "rid-out"});
        SBConnection connection = newConnectionForTest(protocol);
        SBCallableStatement statement = new SBCallableStatement(connection, "CALL demo(?, ?, ?, ?)",
            ResultSet.TYPE_SCROLL_INSENSITIVE, ResultSet.CONCUR_READ_ONLY,
            ResultSet.CLOSE_CURSORS_AT_COMMIT);

        statement.registerOutParameter(1, Types.INTEGER);
        statement.registerOutParameter(2, Types.VARCHAR);
        statement.registerOutParameter(3, Types.SQLXML);
        statement.registerOutParameter(4, Types.ROWID);
        statement.execute();

        assertEquals(7, statement.getObject(1, Integer.class));
        assertEquals("https://scratchbird.dev", statement.getObject(2, URL.class).toString());
        assertEquals("<xml/>", statement.getObject(3, SQLXML.class).getString());
        assertEquals("rid-out", new String(statement.getObject(4, RowId.class).getBytes()));
    }

    private static SBColumnInfo column(String name) {
        SBColumnInfo col = new SBColumnInfo();
        col.setName(name);
        return col;
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

    private static final class OutProtocol extends SBProtocolHandler {
        private final Object[] outRow;

        OutProtocol(Object[] outRow) {
            super(new SBConnectionProperties());
            this.outRow = outRow == null ? new Object[0] : outRow;
        }

        @Override
        public SBQueryResult execute(String sql, List<Object> params, List<Integer> paramTypes,
                                     int maxRows, int timeoutMs) {
            SBQueryResult result = new SBQueryResult();
            List<SBColumnInfo> columns = new ArrayList<>();
            for (int i = 0; i < outRow.length; i++) {
                columns.add(column("c" + i));
            }
            result.setColumns(columns);
            result.setRows(Collections.singletonList(outRow));
            result.setUpdateCount(0);
            return result;
        }
    }
}
