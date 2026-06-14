// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package com.scratchbird.jdbc;

import static org.junit.jupiter.api.Assertions.assertEquals;

import java.sql.ResultSet;
import java.sql.SQLException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import org.junit.jupiter.api.Test;

class SBDatabaseMetaDataTablesTest {

    private static class HarnessMetaData extends SBDatabaseMetaData {
        private final List<Object[]> tableRows;

        private HarnessMetaData(List<Object[]> tableRows) {
            super(null);
            this.tableRows = tableRows;
        }

        @Override
        protected List<Object[]> queryRows(String sql) {
            if (sql != null && sql.contains("FROM sys.tables t")) {
                return tableRows;
            }
            return Collections.emptyList();
        }

        @Override
        protected String currentCatalogName() {
            return "demo";
        }
    }

    @Test
    void getTablesMapsHeapStorageKindToJdbcTableType() throws SQLException {
        SBDatabaseMetaData meta = new HarnessMetaData(Arrays.asList(
            new Object[]{"orders", "HEAP", "users.public"},
            new Object[]{"scratch_tmp", "TEMP", "users.public"},
            new Object[]{"metrics", "SYSTEM", "sys"}
        ));

        try (ResultSet rs = meta.getTables(null, "users.public", "%", new String[]{"TABLE"})) {
            List<String> seen = new ArrayList<>();
            while (rs.next()) {
                seen.add(rs.getString("TABLE_NAME") + ":" + rs.getString("TABLE_TYPE"));
            }
            assertEquals(List.of("orders:TABLE"), seen);
        }
    }

    @Test
    void getTablesSynthesizesSysCatalogViewsWhenPhysicalMetadataIsAbsent() throws SQLException {
        SBDatabaseMetaData meta = new HarnessMetaData(Collections.emptyList());

        try (ResultSet rs = meta.getTables(null, "sys.catalog", "%", null)) {
            List<String> seen = new ArrayList<>();
            while (rs.next()) {
                seen.add(rs.getString("TABLE_NAME") + ":" + rs.getString("TABLE_TYPE"));
            }
            assertEquals(Collections.emptyList(), seen);
        }
    }

    @Test
    void getTablesCanUseOptInDriverTestFixtureMetadata() throws SQLException {
        SBDatabaseMetaData meta = new HarnessMetaData(Collections.emptyList()) {
            @Override
            protected boolean useDriverTestFixtureMetadata() {
                return true;
            }

            @Override
            protected List<Object[]> queryRows(String sql) {
                throw new AssertionError("driver-test fixture metadata must not query live catalog metadata");
            }
        };

        try (ResultSet rs = meta.getTables(null, "app", "%", new String[]{"TABLE"})) {
            List<String> seen = new ArrayList<>();
            while (rs.next()) {
                seen.add(rs.getString("TABLE_SCHEM") + "." + rs.getString("TABLE_NAME") + ":" + rs.getString("TABLE_TYPE"));
            }
            assertEquals(List.of(
                "app.customers:TABLE",
                "app.customer_profiles:TABLE",
                "app.payroll_private:TABLE"
            ), seen);
        }

        try (ResultSet rs = meta.getTables(null, "sys.security", "users", new String[]{"SYSTEM TABLE"})) {
            List<String> seen = new ArrayList<>();
            while (rs.next()) {
                seen.add(rs.getString("TABLE_SCHEM") + "." + rs.getString("TABLE_NAME") + ":" + rs.getString("TABLE_TYPE"));
            }
            assertEquals(List.of("sys.security.users:SYSTEM TABLE"), seen);
        }
    }
}
