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

import java.sql.DatabaseMetaData;
import java.sql.ResultSet;
import java.sql.ResultSetMetaData;
import java.sql.SQLException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import org.junit.jupiter.api.Test;

class SBDatabaseMetaDataIndexPrivilegesTest {

    private static final class HarnessMetaData extends SBDatabaseMetaData {
        private final List<Object[]> indexRows;
        private final List<Object[]> tablePrivilegeRows;
        private final List<Object[]> columnPrivilegeRows;

        private HarnessMetaData(List<Object[]> indexRows,
                                List<Object[]> tablePrivilegeRows,
                                List<Object[]> columnPrivilegeRows) {
            super(null);
            this.indexRows = indexRows;
            this.tablePrivilegeRows = tablePrivilegeRows;
            this.columnPrivilegeRows = columnPrivilegeRows;
        }

        @Override
        protected String currentCatalogName() {
            return "demo";
        }

        @Override
        protected List<Object[]> queryRows(String sql) throws SQLException {
            if (sql == null) {
                return Collections.emptyList();
            }
            if (sql.contains("FROM sys.indexes i")) {
                return indexRows;
            }
            if (sql.contains("FROM information_schema.table_privileges")) {
                return tablePrivilegeRows;
            }
            if (sql.contains("FROM information_schema.column_privileges")) {
                return columnPrivilegeRows;
            }
            return Collections.emptyList();
        }
    }

    @Test
    void getIndexInfoReturnsSortTypeAndCardinality() throws SQLException {
        SBDatabaseMetaData meta = new HarnessMetaData(
            Arrays.asList(
                new Object[]{"idx_orders_customer", "HASH", 1, "orders", "public", 1, "customer_id", "DESC", 1000L, 12L, "status='A'"},
                new Object[]{"idx_orders_status", "BTREE", 0, "orders", "public", 1, "status", "ASC", 500L, 8L, null}
            ),
            Collections.emptyList(),
            Collections.emptyList()
        );

        ResultSet rs = meta.getIndexInfo(null, "public", "orders", false, true);
        List<String> names = new ArrayList<>();
        while (rs.next()) {
            String indexName = rs.getString("INDEX_NAME");
            names.add(indexName);
            if ("idx_orders_customer".equals(indexName)) {
                assertFalse(rs.getBoolean("NON_UNIQUE"));
                assertEquals(DatabaseMetaData.tableIndexHashed, rs.getShort("TYPE"));
                assertEquals("D", rs.getString("ASC_OR_DESC"));
                assertEquals(1000L, rs.getLong("CARDINALITY"));
                assertEquals(12L, rs.getLong("PAGES"));
                assertEquals("status='A'", rs.getString("FILTER_CONDITION"));
            } else if ("idx_orders_status".equals(indexName)) {
                assertTrue(rs.getBoolean("NON_UNIQUE"));
                assertEquals(DatabaseMetaData.tableIndexOther, rs.getShort("TYPE"));
                assertEquals("A", rs.getString("ASC_OR_DESC"));
            }
        }

        assertEquals(2, names.size());
        assertTrue(names.contains("idx_orders_customer"));
        assertTrue(names.contains("idx_orders_status"));

        ResultSet uniqueOnly = meta.getIndexInfo(null, "public", "orders", true, true);
        assertTrue(uniqueOnly.next());
        assertEquals("idx_orders_customer", uniqueOnly.getString("INDEX_NAME"));
        assertFalse(uniqueOnly.next());
    }

    @Test
    void getIndexInfoCatalogMismatchReturnsEmptyShape() throws SQLException {
        SBDatabaseMetaData meta = new HarnessMetaData(Collections.emptyList(), Collections.emptyList(), Collections.emptyList());
        ResultSet rs = meta.getIndexInfo("other", "public", "orders", false, true);
        ResultSetMetaData md = rs.getMetaData();

        assertEquals(13, md.getColumnCount());
        assertEquals("TABLE_CAT", md.getColumnName(1));
        assertEquals("FILTER_CONDITION", md.getColumnName(13));
        assertFalse(rs.next());
    }

    @Test
    void getTablePrivilegesLoadsAndFiltersRows() throws SQLException {
        SBDatabaseMetaData meta = new HarnessMetaData(
            Collections.emptyList(),
            Arrays.asList(
                new Object[]{"public", "orders", "alice", "bob", "SELECT", "YES"},
                new Object[]{"admin", "orders", "root", "bob", "UPDATE", "NO"}
            ),
            Collections.emptyList()
        );

        ResultSet rs = meta.getTablePrivileges(null, "public", "orders");
        assertTrue(rs.next());
        assertEquals("public", rs.getString("TABLE_SCHEM"));
        assertEquals("orders", rs.getString("TABLE_NAME"));
        assertEquals("SELECT", rs.getString("PRIVILEGE"));
        assertEquals("YES", rs.getString("IS_GRANTABLE"));
        assertFalse(rs.next());
    }

    @Test
    void getColumnPrivilegesLoadsAndFiltersRows() throws SQLException {
        SBDatabaseMetaData meta = new HarnessMetaData(
            Collections.emptyList(),
            Collections.emptyList(),
            Arrays.asList(
                new Object[]{"public", "orders", "status", "alice", "bob", "UPDATE", "Y"},
                new Object[]{"public", "orders", "customer_id", "alice", "bob", "SELECT", "N"}
            )
        );

        ResultSet rs = meta.getColumnPrivileges(null, "public", "orders", "sta%");
        assertTrue(rs.next());
        assertEquals("status", rs.getString("COLUMN_NAME"));
        assertEquals("UPDATE", rs.getString("PRIVILEGE"));
        assertEquals("YES", rs.getString("IS_GRANTABLE"));
        assertFalse(rs.next());
    }

    @Test
    void getColumnPrivilegesCatalogMismatchReturnsEmptyShape() throws SQLException {
        SBDatabaseMetaData meta = new HarnessMetaData(Collections.emptyList(), Collections.emptyList(), Collections.emptyList());
        ResultSet rs = meta.getColumnPrivileges("other", "public", "orders", "%");
        ResultSetMetaData md = rs.getMetaData();

        assertEquals(8, md.getColumnCount());
        assertEquals("TABLE_CAT", md.getColumnName(1));
        assertEquals("IS_GRANTABLE", md.getColumnName(8));
        assertFalse(rs.next());
    }
}
