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
import static org.junit.jupiter.api.Assertions.assertNull;

import java.nio.charset.StandardCharsets;
import java.lang.reflect.Field;
import java.sql.JDBCType;
import java.sql.Ref;
import java.sql.ResultSet;
import java.sql.Types;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import sun.misc.Unsafe;

import org.junit.jupiter.api.Test;

public class SBRefSupportTest {

    @Test
    public void preparedStatementSetRefUsesRefTypeAndValue() throws Exception {
        CaptureProtocol protocol = new CaptureProtocol();
        SBConnection connection = newConnectionForTest(protocol);
        SBPreparedStatement statement = new SBPreparedStatement(connection,
            "INSERT INTO demo(ref_col) VALUES (?)",
            ResultSet.TYPE_FORWARD_ONLY,
            ResultSet.CONCUR_READ_ONLY,
            ResultSet.CLOSE_CURSORS_AT_COMMIT);

        Ref ref = new SBRef("demo_ref", "rid-100");
        statement.setRef(1, ref);
        statement.executeUpdate();

        assertEquals("rid-100", protocol.lastParams.get(0));
        assertEquals(Types.REF, protocol.lastTypes.get(0));
    }

    @Test
    public void resultSetGetRefWrapsUnderlyingValue() throws Exception {
        SBConnection connection = newConnectionForTest(new CaptureProtocol());
        SBStatement statement = new SBStatement(connection, ResultSet.TYPE_SCROLL_INSENSITIVE,
            ResultSet.CONCUR_READ_ONLY, ResultSet.CLOSE_CURSORS_AT_COMMIT);

        SBColumnInfo col = new SBColumnInfo();
        col.setName("ref_col");
        SBResultSet rs = new SBResultSet(statement, Collections.singletonList(col),
            Collections.singletonList(new Object[] {"ref-value"}));

        assertEquals(true, rs.next());
        Ref byIndex = rs.getRef(1);
        Ref byLabel = rs.getRef("ref_col");
        assertNotNull(byIndex);
        assertNotNull(byLabel);
        assertEquals("ref-value", byIndex.getObject());
        assertEquals("ref-value", byLabel.getObject());
    }

    @Test
    public void callableStatementGetRefHydratesOutParameter() throws Exception {
        OutRefProtocol protocol = new OutRefProtocol();
        SBConnection connection = newConnectionForTest(protocol);
        SBCallableStatement statement = new SBCallableStatement(connection, "CALL demo(?)",
            ResultSet.TYPE_SCROLL_INSENSITIVE, ResultSet.CONCUR_READ_ONLY,
            ResultSet.CLOSE_CURSORS_AT_COMMIT);

        statement.registerOutParameter(1, Types.REF);
        statement.execute();

        Ref out = statement.getRef(1);
        assertNotNull(out);
        assertEquals("rid-out", out.getObject());
        assertNull(statement.getRef(2));
    }

    @Test
    public void setObjectRoutesSqlXmlAndRowIdThroughTypedBindings() throws Exception {
        CaptureProtocol protocol = new CaptureProtocol();
        SBConnection connection = newConnectionForTest(protocol);
        SBPreparedStatement statement = new SBPreparedStatement(connection,
            "INSERT INTO demo(xml_col, rowid_col) VALUES (?, ?)",
            ResultSet.TYPE_FORWARD_ONLY,
            ResultSet.CONCUR_READ_ONLY,
            ResultSet.CLOSE_CURSORS_AT_COMMIT);

        statement.setObject(1, new SBSQLXML("<xml/>"), Types.SQLXML);
        statement.setObject(2, new SBRowId("rid-200".getBytes(StandardCharsets.UTF_8)));
        statement.executeUpdate();

        assertEquals("<xml/>", protocol.lastParams.get(0));
        assertEquals(Types.SQLXML, protocol.lastTypes.get(0));
        assertEquals("rid-200", protocol.lastParams.get(1));
        assertEquals(Types.ROWID, protocol.lastTypes.get(1));
    }

    @Test
    public void setObjectSqlTypeOverloadsUseJdbcTypeVendorIds() throws Exception {
        CaptureProtocol protocol = new CaptureProtocol();
        SBConnection connection = newConnectionForTest(protocol);
        SBPreparedStatement statement = new SBPreparedStatement(connection,
            "INSERT INTO demo(xml_col, rowid_col) VALUES (?, ?)",
            ResultSet.TYPE_FORWARD_ONLY,
            ResultSet.CONCUR_READ_ONLY,
            ResultSet.CLOSE_CURSORS_AT_COMMIT);

        statement.setObject(1, new SBSQLXML("<typed/>"), JDBCType.SQLXML);
        statement.setObject(2, new SBRowId("rid-typed".getBytes(StandardCharsets.UTF_8)), JDBCType.ROWID, 0);
        statement.executeUpdate();

        assertEquals("<typed/>", protocol.lastParams.get(0));
        assertEquals(Types.SQLXML, protocol.lastTypes.get(0));
        assertEquals("rid-typed", protocol.lastParams.get(1));
        assertEquals(Types.ROWID, protocol.lastTypes.get(1));
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

    private static final class OutRefProtocol extends SBProtocolHandler {
        OutRefProtocol() {
            super(new SBConnectionProperties());
        }

        @Override
        public SBQueryResult execute(String sql, List<Object> params, List<Integer> paramTypes,
                                     int maxRows, int timeoutMs) {
            SBQueryResult result = new SBQueryResult();
            List<SBColumnInfo> columns = new ArrayList<>();
            SBColumnInfo col = new SBColumnInfo();
            col.setName("out_ref");
            columns.add(col);
            result.setColumns(columns);
            result.setRows(Collections.singletonList(new Object[] {"rid-out"}));
            result.setUpdateCount(0);
            return result;
        }
    }
}
