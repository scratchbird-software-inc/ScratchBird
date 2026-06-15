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
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import org.junit.jupiter.api.Test;

class SBDatabaseMetaDataTablesTest {

    private static class HarnessMetaData extends SBDatabaseMetaData {
        private final List<Object[]> tableRows;
        private final List<Object[]> objectTreeRows;

        private HarnessMetaData(List<Object[]> tableRows) {
            this(tableRows, null);
        }

        private HarnessMetaData(List<Object[]> tableRows, List<Object[]> objectTreeRows) {
            super(null);
            this.tableRows = tableRows;
            this.objectTreeRows = objectTreeRows;
        }

        @Override
        protected List<Object[]> queryRows(String sql) throws SQLException {
            if (sql != null && sql.contains("FROM sys.catalog_readable.object_tree")) {
                if (objectTreeRows == null) {
                    throw new SQLException("readable object tree unavailable");
                }
                return objectTreeRows;
            }
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
    void getTablesUsesReadableObjectTreeBeforeLegacyMetadata() throws SQLException {
        SBDatabaseMetaData meta = new HarnessMetaData(
            List.<Object[]>of(new Object[]{"legacy_orders", "HEAP", "app"}),
            List.<Object[]>of(
                new Object[]{"app", "schema", "", "app"},
                new Object[]{"orders", "table", "app", "app.orders"},
                new Object[]{"object_tree", "view", "sys.catalog_readable", "sys.catalog_readable.object_tree"}
            )
        );

        try (ResultSet rs = meta.getTables(null, "%", "%", null)) {
            List<String> seen = new ArrayList<>();
            while (rs.next()) {
                seen.add(rs.getString("TABLE_SCHEM") + "." + rs.getString("TABLE_NAME") + ":" + rs.getString("TABLE_TYPE"));
            }
            assertEquals(List.of(
                "app.orders:TABLE",
                "sys.catalog_readable.object_tree:SYSTEM VIEW"
            ), seen);
        }
    }

    @Test
    void getTablesAcceptsReadableObjectTreeFullRowsFromLiveServer() throws SQLException {
        SBDatabaseMetaData meta = new HarnessMetaData(
            List.<Object[]>of(new Object[]{"legacy_orders", "HEAP", "app"}),
            List.<Object[]>of(
                new Object[]{"schema-sys", "", "sys", "sys", "schema", "", 0, "", "active", "visible"},
                new Object[]{"schema-cat", "schema-sys", "sys.catalog_readable", "catalog_readable", "schema", "sys", 1, "sys", "active", "visible"},
                new Object[]{"view-tree", "schema-cat", "sys.catalog_readable.object_tree", "object_tree", "view", "sys.catalog_readable", 2, "sys.catalog_readable", "active", "visible"},
                new Object[]{"schema-app", "", "app", "app", "schema", "", 0, "", "active", "visible"},
                new Object[]{"table-orders", "schema-app", "app.orders", "orders", "table", "app", 1, "app", "active", "visible"}
            )
        );

        try (ResultSet rs = meta.getTables(null, "%", "%", null)) {
            List<String> seen = new ArrayList<>();
            while (rs.next()) {
                seen.add(rs.getString("TABLE_SCHEM") + "." + rs.getString("TABLE_NAME") + ":" + rs.getString("TABLE_TYPE"));
            }
            assertEquals(List.of(
                "sys.catalog_readable.object_tree:SYSTEM VIEW",
                "app.orders:TABLE"
            ), seen);
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
            protected List<Object[]> queryRows(String sql) throws SQLException {
                throw new SQLException("live catalog metadata unavailable");
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

        try (ResultSet rs = meta.getTables(null, "sys.catalog", "%", new String[]{"SYSTEM VIEW"})) {
            List<String> seen = new ArrayList<>();
            while (rs.next()) {
                seen.add(rs.getString("TABLE_SCHEM") + "." + rs.getString("TABLE_NAME") + ":" + rs.getString("TABLE_TYPE"));
            }
            assertEquals(List.of(
                "sys.catalog.schemas:SYSTEM VIEW",
                "sys.catalog.tables:SYSTEM VIEW",
                "sys.catalog.columns:SYSTEM VIEW",
                "sys.catalog.views:SYSTEM VIEW",
                "sys.catalog.object_resolver:SYSTEM VIEW",
                "sys.catalog.object_dependencies:SYSTEM VIEW",
                "sys.catalog.object_descriptor:SYSTEM VIEW",
                "sys.catalog.object_comment:SYSTEM VIEW",
                "sys.catalog.generated_ddl:SYSTEM VIEW",
                "sys.catalog.generated_sbsql:SYSTEM VIEW",
                "sys.catalog.type_descriptor:SYSTEM VIEW",
                "sys.catalog.domain_descriptor:SYSTEM VIEW",
                "sys.catalog.domain_element:SYSTEM VIEW",
                "sys.catalog.reference_type_mapping:SYSTEM VIEW",
                "sys.catalog.type_capability:SYSTEM VIEW",
                "sys.catalog.operation_descriptor:SYSTEM VIEW",
                "sys.catalog.artifacts:SYSTEM VIEW"
            ), seen);
        }

        try (ResultSet rs = meta.getTables(null, "sys.%", "%", null)) {
            Map<String, Integer> countsBySchema = new LinkedHashMap<>();
            while (rs.next()) {
                String schema = rs.getString("TABLE_SCHEM");
                countsBySchema.put(schema, countsBySchema.getOrDefault(schema, 0) + 1);
            }
            for (String schema : List.of(
                "sys.catalog",
                "sys.metrics",
                "sys.security",
                "sys.configuration",
                "sys.management",
                "sys.fn",
                "sys.udr",
                "sys.parser",
                "sys.storage",
                "sys.mga",
                "sys.audit",
                "sys.compatibility",
                "sys.information",
                "sys.information_schema",
                "sys.catalog_readable",
                "sys.diagnostics"
            )) {
                assertEquals(true, countsBySchema.getOrDefault(schema, 0) > 0, schema + " fixture objects missing");
            }
        }
    }
}
