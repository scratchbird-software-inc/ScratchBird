// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package com.scratchbird.jdbc;

import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.lang.reflect.Field;
import java.sql.ResultSet;
import java.sql.ResultSetMetaData;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import org.junit.jupiter.api.Test;
import sun.misc.Unsafe;

class SBResultSetMetaDataAutoIncrementTest {

    @Test
    void detectsAutoIncrementColumnsFromCatalogMetadata() throws Exception {
        AutoIncrementCatalogProtocol protocol = new AutoIncrementCatalogProtocol();
        SBConnection connection = newConnectionForTest(protocol);
        SBStatement statement = new SBStatement(connection, ResultSet.TYPE_SCROLL_INSENSITIVE,
            ResultSet.CONCUR_READ_ONLY, ResultSet.CLOSE_CURSORS_AT_COMMIT);
        statement.lastExecutedSql = "SELECT id, payload FROM public.demo";

        SBColumnInfo id = new SBColumnInfo();
        id.setName("id");
        id.setTableOid(878787);
        id.setColumnNumber((short) 1);

        SBColumnInfo payload = new SBColumnInfo();
        payload.setName("payload");
        payload.setTableOid(878787);
        payload.setColumnNumber((short) 2);

        SBResultSet rs = new SBResultSet(
            statement,
            Arrays.asList(id, payload),
            new ArrayList<>(Collections.singletonList(new Object[] {1, "alpha"}))
        );

        ResultSetMetaData meta = rs.getMetaData();
        assertTrue(meta.isAutoIncrement(1));
        assertFalse(meta.isAutoIncrement(2));
    }

    @Test
    void reportsPerColumnSchemaAndTableNamesForMultiTableResultSet() throws Exception {
        AutoIncrementCatalogProtocol protocol = new AutoIncrementCatalogProtocol();
        SBConnection connection = newConnectionForTest(protocol);
        SBStatement statement = new SBStatement(connection, ResultSet.TYPE_SCROLL_INSENSITIVE,
            ResultSet.CONCUR_READ_ONLY, ResultSet.CLOSE_CURSORS_AT_COMMIT);
        statement.lastExecutedSql =
            "SELECT l.id, r.payload FROM public.left_demo l JOIN public.right_demo r ON r.id = l.id";

        SBColumnInfo leftId = new SBColumnInfo();
        leftId.setName("id");
        leftId.setTableOid(100);
        leftId.setColumnNumber((short) 1);

        SBColumnInfo rightPayload = new SBColumnInfo();
        rightPayload.setName("payload");
        rightPayload.setTableOid(200);
        rightPayload.setColumnNumber((short) 2);

        SBResultSet rs = new SBResultSet(
            statement,
            Arrays.asList(leftId, rightPayload),
            new ArrayList<>(Collections.singletonList(new Object[] {1, "p"}))
        );

        ResultSetMetaData meta = rs.getMetaData();
        assertEquals("public", meta.getSchemaName(1));
        assertEquals("left_demo", meta.getTableName(1));
        assertEquals("main", meta.getCatalogName(1));
        assertEquals("public", meta.getSchemaName(2));
        assertEquals("right_demo", meta.getTableName(2));
        assertEquals("main", meta.getCatalogName(2));
    }

    @Test
    void tableMetadataCacheIsIsolatedPerConnectionNamespace() throws Exception {
        SBColumnInfo col = new SBColumnInfo();
        col.setName("id");
        col.setTableOid(500);
        col.setColumnNumber((short) 1);

        SBConnection connA = newConnectionForTest(new AutoIncrementCatalogProtocol("demo_a"), "main_a");
        SBStatement stmtA = new SBStatement(connA, ResultSet.TYPE_SCROLL_INSENSITIVE,
            ResultSet.CONCUR_READ_ONLY, ResultSet.CLOSE_CURSORS_AT_COMMIT);
        stmtA.lastExecutedSql = "SELECT id FROM public.demo_a";
        SBResultSet rsA = new SBResultSet(
            stmtA,
            Collections.singletonList(col),
            new ArrayList<>(Collections.singletonList(new Object[] {1}))
        );

        SBConnection connB = newConnectionForTest(new AutoIncrementCatalogProtocol("demo_b"), "main_b");
        SBStatement stmtB = new SBStatement(connB, ResultSet.TYPE_SCROLL_INSENSITIVE,
            ResultSet.CONCUR_READ_ONLY, ResultSet.CLOSE_CURSORS_AT_COMMIT);
        stmtB.lastExecutedSql = "SELECT id FROM public.demo_b";
        SBResultSet rsB = new SBResultSet(
            stmtB,
            Collections.singletonList(col),
            new ArrayList<>(Collections.singletonList(new Object[] {1}))
        );

        assertEquals("demo_a", rsA.getMetaData().getTableName(1));
        assertEquals("demo_b", rsB.getMetaData().getTableName(1));
    }

    private static SBConnection newConnectionForTest(SBProtocolHandler protocol) throws Exception {
        return newConnectionForTest(protocol, "main");
    }

    private static SBConnection newConnectionForTest(SBProtocolHandler protocol, String databaseName) throws Exception {
        SBConnection connection = (SBConnection) getUnsafe().allocateInstance(SBConnection.class);
        setField(connection, "protocol", protocol);
        SBConnectionProperties properties = new SBConnectionProperties();
        properties.setDatabase(databaseName);
        setField(connection, "properties", properties);
        setField(connection, "closed", new java.util.concurrent.atomic.AtomicBoolean(false));
        setField(connection, "circuitBreaker", new CircuitBreaker());
        setField(connection, "telemetry", new TelemetryCollector());
        setField(connection, "readOnly", false);
        setField(connection, "autoCommit", true);
        setField(connection, "schema", "public");
        setField(connection, "catalog", databaseName);
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

    private static final class AutoIncrementCatalogProtocol extends SBProtocolHandler {
        private final String defaultTableName;

        AutoIncrementCatalogProtocol() {
            this("demo");
        }

        AutoIncrementCatalogProtocol(String defaultTableName) {
            super(new SBConnectionProperties());
            this.defaultTableName = defaultTableName;
        }

        @Override
        public SBQueryResult execute(String sql, int maxRows, int timeoutMs) {
            SBQueryResult result = new SBQueryResult();
            if (sql.contains("FROM pg_catalog.pg_class c")) {
                result.setColumns(Arrays.asList(new SBColumnInfo(), new SBColumnInfo()));
                int oid = extractNumericSuffix(sql, "WHERE c.oid = ");
                String tableName = switch (oid) {
                    case 100 -> "left_demo";
                    case 200 -> "right_demo";
                    default -> defaultTableName;
                };
                result.setRows(Collections.singletonList(new Object[] {"public", tableName}));
                return result;
            }
            if (sql.contains("FROM pg_catalog.pg_attribute a")
                && sql.contains("pg_get_expr(ad.adbin")) {
                result.setColumns(Arrays.asList(new SBColumnInfo(), new SBColumnInfo()));
                int attnum = extractNumericSuffix(sql, "AND a.attnum = ");
                if (attnum == 1) {
                    result.setRows(Collections.singletonList(new Object[] {"d", "nextval('demo_id_seq')"}));
                } else {
                    result.setRows(Collections.singletonList(new Object[] {"", null}));
                }
                return result;
            }
            if (sql.contains("FROM pg_catalog.pg_attribute a")) {
                result.setColumns(Arrays.asList(new SBColumnInfo(), new SBColumnInfo()));
                result.setRows(List.of(
                    new Object[] {1, "id"},
                    new Object[] {2, "payload"}
                ));
                return result;
            }
            result.setColumns(Collections.emptyList());
            result.setRows(Collections.emptyList());
            return result;
        }

        private static int extractNumericSuffix(String sql, String marker) {
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
    }
}
