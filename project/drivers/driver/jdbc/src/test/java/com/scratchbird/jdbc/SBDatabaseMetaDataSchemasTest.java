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

class SBDatabaseMetaDataSchemasTest {

    private static class HarnessMetaData extends SBDatabaseMetaData {
        private final List<Object[]> schemaRows;
        private final List<Object[]> informationSchemaRows;
        private final List<Object[]> objectTreeRows;
        private final boolean expandParents;
        private final boolean failSysSchemas;
        private final boolean failInformationSchema;

        private HarnessMetaData(boolean expandParents, List<String> schemaNames) {
            this(expandParents, schemaNames, Collections.emptyList(), false, false);
        }

        private HarnessMetaData(
                boolean expandParents,
                List<String> schemaNames,
                List<String> informationSchemaNames,
                boolean failSysSchemas,
                boolean failInformationSchema) {
            this(expandParents, schemaNames, informationSchemaNames, failSysSchemas, failInformationSchema, null);
        }

        private HarnessMetaData(
                boolean expandParents,
                List<String> schemaNames,
                List<String> informationSchemaNames,
                boolean failSysSchemas,
                boolean failInformationSchema,
                List<Object[]> objectTreeRows) {
            super(null);
            this.expandParents = expandParents;
            this.informationSchemaRows = new ArrayList<>();
            this.schemaRows = new ArrayList<>();
            this.objectTreeRows = objectTreeRows;
            this.failSysSchemas = failSysSchemas;
            this.failInformationSchema = failInformationSchema;
            for (String schemaName : schemaNames) {
                this.schemaRows.add(new Object[]{schemaName});
            }
            for (String schemaName : informationSchemaNames) {
                this.informationSchemaRows.add(new Object[]{schemaName});
            }
        }

        @Override
        protected List<Object[]> queryRows(String sql) throws SQLException {
            if (sql != null && sql.contains("FROM sys.catalog_readable.object_tree")) {
                if (objectTreeRows == null) {
                    throw new SQLException("readable object tree unavailable");
                }
                return objectTreeRows;
            }
            if (sql != null && sql.contains("FROM sys.schemas")) {
                if (failSysSchemas) {
                    throw new SQLException("sys.schemas unavailable");
                }
                return schemaRows;
            }
            if (sql != null && sql.contains("FROM information_schema.schemata")) {
                if (failInformationSchema) {
                    throw new SQLException("information_schema.schemata unavailable");
                }
                return informationSchemaRows;
            }
            return Collections.emptyList();
        }

        @Override
        protected String currentCatalogName() {
            return "demo";
        }

        @Override
        protected boolean expandSchemaParentNodesInMetadata() {
            return expandParents;
        }
    }

    @Test
    void getSchemasUsesReadableObjectTreeBeforeLegacyMetadata() throws SQLException {
        SBDatabaseMetaData meta = new HarnessMetaData(
            false,
            Arrays.asList("legacy.schema"),
            Collections.emptyList(),
            false,
            true,
            List.<Object[]>of(
                new Object[]{"sys", "schema"},
                new Object[]{"sys.catalog_readable", "schema"},
                new Object[]{"sys.catalog_readable.object_tree", "view"},
                new Object[]{"app", "schema"},
                new Object[]{"app.orders", "table"}
            )
        );

        try (ResultSet rs = meta.getSchemas(null, null)) {
            assertEquals(Arrays.asList(
                "sys",
                "sys.catalog_readable",
                "app"
            ), collectSchemas(rs));
        }
    }

    @Test
    void getSchemasAcceptsReadableObjectTreeFullRowsFromLiveServer() throws SQLException {
        SBDatabaseMetaData meta = new HarnessMetaData(
            false,
            Arrays.asList("legacy.schema"),
            Collections.emptyList(),
            false,
            true,
            List.<Object[]>of(
                new Object[]{"schema-sys", "", "sys", "sys", "schema", "", 0, "", "active", "visible"},
                new Object[]{"schema-cat", "schema-sys", "sys.catalog_readable", "catalog_readable", "schema", "sys", 1, "sys", "active", "visible"},
                new Object[]{"view-tree", "schema-cat", "sys.catalog_readable.object_tree", "object_tree", "view", "sys.catalog_readable", 2, "sys.catalog_readable", "active", "visible"},
                new Object[]{"schema-app", "", "app", "app", "schema", "", 0, "", "active", "visible"},
                new Object[]{"table-orders", "schema-app", "app.orders", "orders", "table", "app", 1, "app", "active", "visible"}
            )
        );

        try (ResultSet rs = meta.getSchemas(null, null)) {
            assertEquals(Arrays.asList(
                "sys",
                "sys.catalog_readable",
                "app"
            ), collectSchemas(rs));
        }
    }

    @Test
    void getSchemasDefaultKeepsOnlyPhysicalSchemaRows() throws SQLException {
        SBDatabaseMetaData meta = new HarnessMetaData(false, Arrays.asList(
            "sys",
            "users.alice.dev",
            "users.bob.dev",
            "analytics.prod"
        ));

        try (ResultSet rs = meta.getSchemas(null, null)) {
            assertEquals(Arrays.asList(
                "sys",
                "users.alice.dev",
                "users.bob.dev",
                "analytics.prod"
            ), collectSchemas(rs));
        }
    }

    @Test
    void getSchemasCanExpandParentNodesForRecursiveSchemaNavigation() throws SQLException {
        SBDatabaseMetaData meta = new HarnessMetaData(true, Arrays.asList(
            "analytics.prod",
            "sys",
            "users.alice.dev",
            "users.bob.dev"
        ));

        try (ResultSet rs = meta.getSchemas(null, null)) {
            assertEquals(Arrays.asList(
                "analytics",
                "analytics.prod",
                "sys",
                "users",
                "users.alice",
                "users.alice.dev",
                "users.bob",
                "users.bob.dev"
            ), collectSchemas(rs));
        }
    }

    @Test
    void getSchemasExpansionStillRespectsSchemaPattern() throws SQLException {
        SBDatabaseMetaData meta = new HarnessMetaData(true, Arrays.asList(
            "users.alice.dev",
            "users.bob.dev",
            "analytics.prod"
        ));

        try (ResultSet rs = meta.getSchemas(null, "users.%")) {
            assertEquals(Arrays.asList(
                "users.alice",
                "users.alice.dev",
                "users.bob",
                "users.bob.dev"
            ), collectSchemas(rs));
        }
    }

    @Test
    void getSchemasFallsBackToInformationSchemaWhenSysSchemasUnavailable() throws SQLException {
        SBDatabaseMetaData meta = new HarnessMetaData(
            false,
            Collections.emptyList(),
            Arrays.asList("users.alice.dev", "analytics.prod"),
            true,
            false
        );

        try (ResultSet rs = meta.getSchemas(null, "users.%")) {
            assertEquals(Arrays.asList("users.alice.dev"), collectSchemas(rs));
        }
    }

    @Test
    void getSchemasSynthesizesSystemMetadataWhenCatalogViewsUnavailable() throws SQLException {
        SBDatabaseMetaData meta = new HarnessMetaData(
            false,
            Collections.emptyList(),
            Collections.emptyList(),
            true,
            true
        );

        try (ResultSet rs = meta.getSchemas(null, "sys.%")) {
            assertEquals(Collections.emptyList(), collectSchemas(rs));
        }
    }

    @Test
    void getSchemasCanUseOptInDriverTestFixtureMetadata() throws SQLException {
        SBDatabaseMetaData meta = new HarnessMetaData(
            false,
            Collections.emptyList(),
            Collections.emptyList(),
            true,
            true
        ) {
            @Override
            protected boolean useDriverTestFixtureMetadata() {
                return true;
            }
        };

        try (ResultSet rs = meta.getSchemas(null, "app")) {
            assertEquals(Arrays.asList("app"), collectSchemas(rs));
        }
        try (ResultSet rs = meta.getSchemas(null, "sys.security")) {
            assertEquals(Arrays.asList("sys.security"), collectSchemas(rs));
        }
        try (ResultSet rs = meta.getSchemas(null, "sys.%")) {
            assertEquals(Arrays.asList(
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
            ), collectSchemas(rs));
        }
    }

    private static List<String> collectSchemas(ResultSet rs) throws SQLException {
        List<String> schemas = new ArrayList<>();
        while (rs.next()) {
            schemas.add(rs.getString("TABLE_SCHEM"));
        }
        return schemas;
    }
}
