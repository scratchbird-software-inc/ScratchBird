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

    private static final class HarnessMetaData extends SBDatabaseMetaData {
        private final List<Object[]> schemaRows;
        private final List<Object[]> informationSchemaRows;
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
            super(null);
            this.expandParents = expandParents;
            this.informationSchemaRows = new ArrayList<>();
            this.schemaRows = new ArrayList<>();
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
                "analytics.prod",
                "sys.catalog"
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
                "users.bob.dev",
                "sys.catalog"
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
            assertEquals(Arrays.asList("sys.catalog"), collectSchemas(rs));
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
