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
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.sql.ResultSet;
import java.sql.ResultSetMetaData;
import java.sql.SQLException;
import java.sql.Types;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import org.junit.jupiter.api.Test;

class SBDatabaseMetaDataColumnsTest {

    private static class HarnessMetaData extends SBDatabaseMetaData {
        private final List<Object[]> columnRows;
        private final List<ProbeColumnMetadata> probeColumns;

        private HarnessMetaData(List<Object[]> columnRows) {
            this(columnRows, Collections.emptyList());
        }

        private HarnessMetaData(List<Object[]> columnRows, List<ProbeColumnMetadata> probeColumns) {
            super(null);
            this.columnRows = columnRows;
            this.probeColumns = probeColumns;
        }

        @Override
        protected String currentCatalogName() {
            return "demo";
        }

        @Override
        protected List<Object[]> queryRows(String sql) throws SQLException {
            if (sql != null && sql.contains("FROM sys.columns c")) {
                return columnRows;
            }
            return Collections.emptyList();
        }

        @Override
        protected List<ProbeColumnMetadata> loadProbeColumnMetadata(String schemaName, String tableName) {
            if ("sys.catalog".equals(schemaName) && "object_resolver".equals(tableName)) {
                return probeColumns;
            }
            return Collections.emptyList();
        }
    }

    @Test
    void getColumnsReportsUsefulTypeSizesAndScale() throws SQLException {
        SBDatabaseMetaData meta = new HarnessMetaData(Arrays.asList(
            new Object[]{"id", "int4", 1, 0, "nextval('orders_id_seq'::regclass)", "orders", "public"},
            new Object[]{"price", "numeric", 2, 1, "0", "orders", "public"},
            new Object[]{"name", "varchar", 3, 1, "GENERATED ALWAYS AS (upper(payload)) STORED", "orders", "public"},
            new Object[]{"created_at", "timestamp", 4, 1, null, "orders", "public"}
        ));

        ResultSet rs = meta.getColumns(null, "public", "orders", "%");
        int seen = 0;
        while (rs.next()) {
            seen++;
            String column = rs.getString("COLUMN_NAME");
            assertNotNull(column);
            if ("id".equals(column)) {
                assertEquals(Types.INTEGER, rs.getInt("DATA_TYPE"));
                assertEquals(10, rs.getInt("COLUMN_SIZE"));
                assertEquals(0, rs.getInt("DECIMAL_DIGITS"));
                assertEquals(10, rs.getInt("NUM_PREC_RADIX"));
                assertEquals("YES", rs.getString("IS_AUTOINCREMENT"));
            } else if ("price".equals(column)) {
                assertEquals(Types.NUMERIC, rs.getInt("DATA_TYPE"));
                assertEquals(38, rs.getInt("COLUMN_SIZE"));
                assertEquals(0, rs.getInt("DECIMAL_DIGITS"));
                assertEquals(10, rs.getInt("NUM_PREC_RADIX"));
                assertEquals("NO", rs.getString("IS_AUTOINCREMENT"));
            } else if ("name".equals(column)) {
                assertEquals(Types.VARCHAR, rs.getInt("DATA_TYPE"));
                assertEquals(65535, rs.getInt("COLUMN_SIZE"));
                assertEquals(65535, rs.getInt("CHAR_OCTET_LENGTH"));
                assertEquals("NO", rs.getString("IS_AUTOINCREMENT"));
                assertEquals("YES", rs.getString("IS_GENERATEDCOLUMN"));
            } else if ("created_at".equals(column)) {
                assertEquals(Types.TIMESTAMP, rs.getInt("DATA_TYPE"));
                assertEquals(29, rs.getInt("COLUMN_SIZE"));
                assertEquals("NO", rs.getString("IS_AUTOINCREMENT"));
                assertEquals("NO", rs.getString("IS_GENERATEDCOLUMN"));
            }
        }

        assertEquals(4, seen);
    }

    @Test
    void supportsResultSetConcurrencyRequiresSupportedType() throws SQLException {
        SBDatabaseMetaData meta = new HarnessMetaData(Collections.emptyList());
        assertTrue(meta.supportsResultSetConcurrency(ResultSet.TYPE_FORWARD_ONLY, ResultSet.CONCUR_READ_ONLY));
        assertTrue(meta.supportsResultSetConcurrency(ResultSet.TYPE_SCROLL_INSENSITIVE, ResultSet.CONCUR_READ_ONLY));
        assertTrue(meta.supportsResultSetConcurrency(ResultSet.TYPE_FORWARD_ONLY, ResultSet.CONCUR_UPDATABLE));
        assertTrue(meta.supportsResultSetConcurrency(ResultSet.TYPE_SCROLL_INSENSITIVE, ResultSet.CONCUR_UPDATABLE));
        assertTrue(meta.supportsResultSetConcurrency(ResultSet.TYPE_SCROLL_SENSITIVE, ResultSet.CONCUR_READ_ONLY));
        assertTrue(meta.supportsResultSetConcurrency(ResultSet.TYPE_SCROLL_SENSITIVE, ResultSet.CONCUR_UPDATABLE));
    }

    @Test
    void catalogMismatchStillReturnsStandardColumnsShape() throws SQLException {
        SBDatabaseMetaData meta = new HarnessMetaData(Collections.emptyList());
        ResultSet rs = meta.getColumns("other", "public", "orders", "%");
        ResultSetMetaData md = rs.getMetaData();

        assertEquals(24, md.getColumnCount());
        assertEquals("TABLE_CAT", md.getColumnName(1));
        assertEquals("IS_GENERATEDCOLUMN", md.getColumnName(24));
        assertFalse(rs.next());
    }

    @Test
    void getColumnsCanFallBackToLiveProbeMetadataForSyntheticSystemViews() throws SQLException {
        SBDatabaseMetaData meta = new HarnessMetaData(
            Collections.emptyList(),
            Arrays.asList(
                new SBDatabaseMetaData.ProbeColumnMetadata(
                    "object_id", Types.VARCHAR, "UUID", 36, 0, ResultSetMetaData.columnNoNulls, false, 1),
                new SBDatabaseMetaData.ProbeColumnMetadata(
                    "object_type", Types.VARCHAR, "VARCHAR", 32, 0, ResultSetMetaData.columnNullable, false, 2),
                new SBDatabaseMetaData.ProbeColumnMetadata(
                    "full_path", Types.VARCHAR, "VARCHAR", 512, 0, ResultSetMetaData.columnNullable, false, 3)
            ));

        try (ResultSet rs = meta.getColumns(null, "sys.catalog", "object_resolver", "%")) {
            assertTrue(rs.next());
            assertEquals("object_id", rs.getString("COLUMN_NAME"));
            assertEquals(Types.VARCHAR, rs.getInt("DATA_TYPE"));
            assertEquals(36, rs.getInt("COLUMN_SIZE"));
            assertEquals("NO", rs.getString("IS_NULLABLE"));

            assertTrue(rs.next());
            assertEquals("object_type", rs.getString("COLUMN_NAME"));
            assertEquals("YES", rs.getString("IS_NULLABLE"));

            assertTrue(rs.next());
            assertEquals("full_path", rs.getString("COLUMN_NAME"));
            assertEquals(512, rs.getInt("CHAR_OCTET_LENGTH"));
            assertFalse(rs.next());
        }
    }

    @Test
    void getColumnsCanUseOptInDriverTestFixtureMetadata() throws SQLException {
        SBDatabaseMetaData meta = new HarnessMetaData(Collections.emptyList()) {
            @Override
            protected boolean useDriverTestFixtureMetadata() {
                return true;
            }

            @Override
            protected List<Object[]> queryRows(String sql) throws SQLException {
                throw new SQLException("live catalog metadata unavailable");
            }

            @Override
            protected List<ProbeColumnMetadata> loadProbeColumnMetadata(String schemaName, String tableName) {
                throw new AssertionError("driver-test fixture metadata must not probe live table metadata");
            }
        };

        try (ResultSet rs = meta.getColumns(null, "app", "customers", "%")) {
            assertTrue(rs.next());
            assertEquals("id", rs.getString("COLUMN_NAME"));
            assertEquals(Types.BIGINT, rs.getInt("DATA_TYPE"));
            assertEquals("NO", rs.getString("IS_NULLABLE"));

            assertTrue(rs.next());
            assertEquals("customer_name", rs.getString("COLUMN_NAME"));
            assertEquals(Types.VARCHAR, rs.getInt("DATA_TYPE"));
            assertEquals("YES", rs.getString("IS_NULLABLE"));

            assertTrue(rs.next());
            assertEquals("status", rs.getString("COLUMN_NAME"));
            assertFalse(rs.next());
        }

        try (ResultSet rs = meta.getColumns(null, "sys.information", "tables", "%")) {
            assertTrue(rs.next());
            assertEquals("table_catalog", rs.getString("COLUMN_NAME"));
            assertEquals(Types.VARCHAR, rs.getInt("DATA_TYPE"));

            assertTrue(rs.next());
            assertEquals("table_schema", rs.getString("COLUMN_NAME"));

            assertTrue(rs.next());
            assertEquals("table_name", rs.getString("COLUMN_NAME"));

            assertTrue(rs.next());
            assertEquals("table_type", rs.getString("COLUMN_NAME"));
            assertFalse(rs.next());
        }

        try (ResultSet rs = meta.getColumns(null, "sys.parser", "dialects", "%")) {
            assertTrue(rs.next());
            assertEquals("dialect_name", rs.getString("COLUMN_NAME"));

            assertTrue(rs.next());
            assertEquals("base_dialect", rs.getString("COLUMN_NAME"));

            assertTrue(rs.next());
            assertEquals("compatibility_state", rs.getString("COLUMN_NAME"));

            assertTrue(rs.next());
            assertEquals("parser_family", rs.getString("COLUMN_NAME"));
            assertFalse(rs.next());
        }

        try (ResultSet rs = meta.getColumns(null, "sys.catalog", "object_resolver", "object_%")) {
            assertTrue(rs.next());
            assertEquals("object_id", rs.getString("COLUMN_NAME"));

            assertTrue(rs.next());
            assertEquals("object_type", rs.getString("COLUMN_NAME"));

            assertTrue(rs.next());
            assertEquals("object_name", rs.getString("COLUMN_NAME"));
            assertFalse(rs.next());
        }
    }
}
