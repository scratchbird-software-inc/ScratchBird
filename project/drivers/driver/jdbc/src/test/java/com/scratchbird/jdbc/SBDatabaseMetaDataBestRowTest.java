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
import java.sql.Types;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import org.junit.jupiter.api.Test;

class SBDatabaseMetaDataBestRowTest {

    private static final class HarnessMetaData extends SBDatabaseMetaData {
        private final List<Object[]> primaryKeyRows;
        private final List<Object[]> versionRows;

        private HarnessMetaData(List<Object[]> primaryKeyRows, List<Object[]> versionRows) {
            super(null);
            this.primaryKeyRows = primaryKeyRows;
            this.versionRows = versionRows;
        }

        @Override
        protected String currentCatalogName() {
            return "demo";
        }

        @Override
        protected List<Object[]> queryRows(String sql) throws SQLException {
            if (sql != null && sql.contains("WHERE tc.constraint_type = 'PRIMARY KEY'")) {
                return primaryKeyRows;
            }
            if (sql != null && sql.contains("a.attname = 'xmin'")) {
                return versionRows;
            }
            return Collections.emptyList();
        }
    }

    @Test
    void bestRowIdentifierUsesPrimaryKeyMetadata() throws SQLException {
        SBDatabaseMetaData meta = new HarnessMetaData(Arrays.asList(
            new Object[]{"public", "orders", "id", "integer", null, 10, 0, "NO"},
            new Object[]{"public", "orders", "tenant_id", "varchar", 64, null, null, "NO"},
            new Object[]{"public", "orders", "nullable_pk", "integer", null, 10, 0, "YES"},
            new Object[]{"public", "users", "id", "integer", null, 10, 0, "NO"}
        ), Collections.emptyList());

        ResultSet rs = meta.getBestRowIdentifier(null, "public", "orders",
            DatabaseMetaData.bestRowSession, false);

        assertTrue(rs.next());
        assertEquals(DatabaseMetaData.bestRowSession, rs.getShort("SCOPE"));
        assertEquals("id", rs.getString("COLUMN_NAME"));
        assertEquals(Types.INTEGER, rs.getInt("DATA_TYPE"));
        assertEquals("integer", rs.getString("TYPE_NAME"));
        assertEquals(10, rs.getInt("COLUMN_SIZE"));
        assertEquals(DatabaseMetaData.bestRowNotPseudo, rs.getShort("PSEUDO_COLUMN"));

        assertTrue(rs.next());
        assertEquals("tenant_id", rs.getString("COLUMN_NAME"));
        assertEquals(Types.VARCHAR, rs.getInt("DATA_TYPE"));
        assertEquals(64, rs.getInt("COLUMN_SIZE"));

        // nullable=false filter excludes nullable PK entries.
        assertFalse(rs.next());
    }

    @Test
    void bestRowIdentifierCatalogMismatchReturnsEmptyShape() throws SQLException {
        SBDatabaseMetaData meta = new HarnessMetaData(Collections.emptyList(), Collections.emptyList());
        ResultSet rs = meta.getBestRowIdentifier("other", "public", "orders",
            DatabaseMetaData.bestRowSession, true);
        ResultSetMetaData md = rs.getMetaData();

        assertEquals(8, md.getColumnCount());
        assertEquals("SCOPE", md.getColumnName(1));
        assertEquals("PSEUDO_COLUMN", md.getColumnName(8));
        assertFalse(rs.next());
    }

    @Test
    void versionColumnsCatalogMismatchReturnsEmptyShape() throws SQLException {
        SBDatabaseMetaData meta = new HarnessMetaData(Collections.emptyList(), Collections.emptyList());
        ResultSet rs = meta.getVersionColumns("other", "public", "orders");
        ResultSetMetaData md = rs.getMetaData();

        assertEquals(8, md.getColumnCount());
        assertEquals("SCOPE", md.getColumnName(1));
        assertEquals("PSEUDO_COLUMN", md.getColumnName(8));
        assertFalse(rs.next());
    }

    @Test
    void versionColumnsUsesXminPseudoColumnWhenAvailable() throws SQLException {
        SBDatabaseMetaData meta = new HarnessMetaData(
            Collections.emptyList(),
            Arrays.asList(
                new Object[]{"public", "orders", 28},
                new Object[]{"public", "users", 28}
            )
        );
        ResultSet rs = meta.getVersionColumns(null, "public", "orders");

        assertTrue(rs.next());
        assertEquals(DatabaseMetaData.versionColumnUnknown, rs.getShort("SCOPE"));
        assertEquals("xmin", rs.getString("COLUMN_NAME"));
        assertEquals(Types.BIGINT, rs.getInt("DATA_TYPE"));
        assertEquals("xid", rs.getString("TYPE_NAME"));
        assertEquals(10, rs.getInt("COLUMN_SIZE"));
        assertEquals(DatabaseMetaData.versionColumnPseudo, rs.getShort("PSEUDO_COLUMN"));
        assertFalse(rs.next());
    }
}
