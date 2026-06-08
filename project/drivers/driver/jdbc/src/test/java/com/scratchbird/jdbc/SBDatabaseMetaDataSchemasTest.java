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
        private final boolean expandParents;

        private HarnessMetaData(boolean expandParents, List<String> schemaNames) {
            super(null);
            this.expandParents = expandParents;
            this.schemaRows = new ArrayList<>();
            for (String schemaName : schemaNames) {
                this.schemaRows.add(new Object[]{schemaName});
            }
        }

        @Override
        protected List<Object[]> queryRows(String sql) {
            if (sql != null && sql.contains("FROM sys.schemas")) {
                return schemaRows;
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

    private static List<String> collectSchemas(ResultSet rs) throws SQLException {
        List<String> schemas = new ArrayList<>();
        while (rs.next()) {
            schemas.add(rs.getString("TABLE_SCHEM"));
        }
        return schemas;
    }
}
