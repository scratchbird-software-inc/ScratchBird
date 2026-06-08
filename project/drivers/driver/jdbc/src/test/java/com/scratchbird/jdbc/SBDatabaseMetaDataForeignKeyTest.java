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

class SBDatabaseMetaDataForeignKeyTest {

    private static final class HarnessMetaData extends SBDatabaseMetaData {
        private final List<Object[]> fkRows;
        private final String catalog;

        private HarnessMetaData(String catalog, List<Object[]> fkRows) {
            super(null);
            this.catalog = catalog;
            this.fkRows = fkRows;
        }

        @Override
        protected String currentCatalogName() {
            return catalog;
        }

        @Override
        protected List<Object[]> queryRows(String sql) throws SQLException {
            if (sql != null && sql.contains("WHERE tc.constraint_type = 'FOREIGN KEY'")) {
                return fkRows;
            }
            return Collections.emptyList();
        }
    }

    @Test
    void importedKeysFilterByForeignTable() throws SQLException {
        SBDatabaseMetaData meta = new HarnessMetaData("demo", fixtureRows());
        ResultSet rs = meta.getImportedKeys(null, "public", "orders");

        List<String> fkColumns = new ArrayList<>();
        while (rs.next()) {
            assertEquals("orders", rs.getString("FKTABLE_NAME"));
            fkColumns.add(rs.getString("FKCOLUMN_NAME"));
        }

        assertEquals(2, fkColumns.size());
        assertTrue(fkColumns.contains("customer_id"));
        assertTrue(fkColumns.contains("product_id"));
    }

    @Test
    void exportedKeysFilterByPrimaryTable() throws SQLException {
        SBDatabaseMetaData meta = new HarnessMetaData("demo", fixtureRows());
        ResultSet rs = meta.getExportedKeys(null, "public", "customers");

        List<String> fkTables = new ArrayList<>();
        while (rs.next()) {
            assertEquals("customers", rs.getString("PKTABLE_NAME"));
            fkTables.add(rs.getString("FKTABLE_NAME"));
        }

        assertEquals(2, fkTables.size());
        assertTrue(fkTables.contains("orders"));
        assertTrue(fkTables.contains("invoices"));
    }

    @Test
    void crossReferenceFiltersParentAndForeign() throws SQLException {
        SBDatabaseMetaData meta = new HarnessMetaData("demo", fixtureRows());
        ResultSet rs = meta.getCrossReference(null, "public", "customers",
                                              null, "public", "orders");

        assertTrue(rs.next());
        assertEquals("customers", rs.getString("PKTABLE_NAME"));
        assertEquals("orders", rs.getString("FKTABLE_NAME"));
        assertEquals("customer_id", rs.getString("FKCOLUMN_NAME"));
        assertEquals(DatabaseMetaData.importedKeyCascade, rs.getShort("UPDATE_RULE"));
        assertEquals(DatabaseMetaData.importedKeyNoAction, rs.getShort("DELETE_RULE"));
        assertFalse(rs.next());
    }

    @Test
    void mismatchedCatalogReturnsEmptyResultWithStandardShape() throws SQLException {
        SBDatabaseMetaData meta = new HarnessMetaData("demo", fixtureRows());
        ResultSet rs = meta.getExportedKeys("other", "public", "customers");
        ResultSetMetaData md = rs.getMetaData();

        assertEquals(14, md.getColumnCount());
        assertEquals("PKTABLE_CAT", md.getColumnName(1));
        assertEquals("DEFERRABILITY", md.getColumnName(14));
        assertFalse(rs.next());
    }

    private static List<Object[]> fixtureRows() {
        return Arrays.asList(
            new Object[]{"public", "orders", "customer_id", "public", "customers", "id",
                1, "CASCADE", "NO ACTION", "fk_orders_customers", "pk_customers", "NOT DEFERRABLE"},
            new Object[]{"public", "orders", "product_id", "public", "products", "id",
                1, "NO ACTION", "RESTRICT", "fk_orders_products", "pk_products", "NOT DEFERRABLE"},
            new Object[]{"public", "invoices", "customer_id", "public", "customers", "id",
                1, "CASCADE", "NO ACTION", "fk_invoices_customers", "pk_customers", "NOT DEFERRABLE"}
        );
    }
}
