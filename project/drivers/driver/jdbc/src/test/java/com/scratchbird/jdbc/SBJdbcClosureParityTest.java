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

import java.sql.Connection;
import java.sql.DatabaseMetaData;
import java.sql.ResultSet;
import java.sql.Savepoint;
import java.sql.Statement;
import org.junit.jupiter.api.Test;

class SBJdbcClosureParityTest {

    private SBIntegrationRuntime.RuntimeConfig runtime() {
        return SBIntegrationRuntime.requireRuntime();
    }

    private Connection openConnection() throws Exception {
        String base = runtime().baseUrl();
        String dsn = base.contains("?") ? base + "&pooling=false" : base + "?pooling=false";
        return runtime().openConnection(dsn);
    }

    @Test
    void defaultSchemaComesFromServerAndAnchorsMetadataFamilies() throws Exception {
        String tableName = "jdbc054_meta_" + Long.toHexString(System.nanoTime());
        try (Connection conn = openConnection();
             Statement stmt = conn.createStatement()) {
            String currentSchema = conn.getSchema();
            assertNotNull(currentSchema);
            assertFalse(currentSchema.isBlank());
            assertFalse("public".equalsIgnoreCase(currentSchema));

            stmt.execute("CREATE TABLE " + tableName + " (id INTEGER, note TEXT)");

            DatabaseMetaData metadata = conn.getMetaData();
            assertTrue(schemaExists(metadata, currentSchema));
            assertTrue(tableExists(metadata, currentSchema, tableName));
            assertTrue(columnExists(metadata, currentSchema, tableName, "id"));
            assertTrue(columnExists(metadata, currentSchema, tableName, "note"));

            try (ResultSet columns = metadata.getColumns(null, currentSchema, tableName, "%")) {
                assertEquals(24, columns.getMetaData().getColumnCount());
            }
        } finally {
            try (Connection cleanup = openConnection();
                 Statement stmt = cleanup.createStatement()) {
                stmt.execute("DROP TABLE " + tableName);
            } catch (Exception ignored) {
                // Best-effort cleanup for unique test tables.
            }
        }
    }

    @Test
    void manualTransactionsRemainImmediatelyUsableAcrossCommitAndRollback() throws Exception {
        try (Connection conn = openConnection();
             Statement stmt = conn.createStatement()) {
            conn.setAutoCommit(false);

            Savepoint beforeCommit = conn.setSavepoint("before_commit");
            assertNotNull(beforeCommit);
            conn.releaseSavepoint(beforeCommit);

            conn.commit();

            Savepoint afterCommit = conn.setSavepoint("after_commit");
            assertNotNull(afterCommit);
            conn.releaseSavepoint(afterCommit);

            try (ResultSet rs = stmt.executeQuery("SELECT 1")) {
                assertTrue(rs.next());
                assertEquals(1, rs.getInt(1));
            }

            conn.rollback();

            Savepoint afterRollback = conn.setSavepoint("after_rollback");
            assertNotNull(afterRollback);
            conn.releaseSavepoint(afterRollback);

            try (ResultSet rs = stmt.executeQuery("SELECT 2")) {
                assertTrue(rs.next());
                assertEquals(2, rs.getInt(1));
            }

            conn.setAutoCommit(true);
        }
    }

    private static boolean schemaExists(DatabaseMetaData metadata, String schemaName) throws Exception {
        try (ResultSet schemas = metadata.getSchemas(null, schemaName)) {
            while (schemas.next()) {
                if (schemaName.equals(schemas.getString("TABLE_SCHEM"))) {
                    return true;
                }
            }
        }
        return false;
    }

    private static boolean tableExists(DatabaseMetaData metadata, String schemaName, String tableName)
        throws Exception {
        try (ResultSet tables = metadata.getTables(null, schemaName, tableName, new String[]{"TABLE"})) {
            while (tables.next()) {
                if (tableName.equalsIgnoreCase(tables.getString("TABLE_NAME"))) {
                    return true;
                }
            }
        }
        return false;
    }

    private static boolean columnExists(DatabaseMetaData metadata, String schemaName, String tableName,
                                        String columnName) throws Exception {
        if (columnExistsWithFilters(metadata, schemaName, schemaName, tableName, columnName)) {
            return true;
        }
        return columnExistsWithFilters(metadata, schemaName, null, tableName, columnName);
    }

    private static boolean columnExistsWithFilters(DatabaseMetaData metadata, String expectedSchema,
                                                   String schemaFilter, String tableName,
                                                   String columnName) throws Exception {
        try (ResultSet columns = metadata.getColumns(null, schemaFilter, tableName, "%")) {
            while (columns.next()) {
                if (expectedSchema.equalsIgnoreCase(columns.getString("TABLE_SCHEM"))
                    && tableName.equalsIgnoreCase(columns.getString("TABLE_NAME"))
                    && columnName.equalsIgnoreCase(columns.getString("COLUMN_NAME"))) {
                    return true;
                }
            }
        }
        return false;
    }
}
