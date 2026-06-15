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
import static org.junit.jupiter.api.Assumptions.assumeTrue;

import java.sql.Connection;
import java.sql.DriverManager;
import java.sql.ResultSet;
import java.sql.ResultSetMetaData;
import java.sql.Statement;
import org.junit.jupiter.api.Test;

class SBDirectConnectionSmokeTest {

    @Test
    void connectsToConfiguredUrlAndRunsSelectOne() throws Exception {
        String url = System.getenv("SCRATCHBIRD_JDBC_URL");
        assumeTrue(url != null && !url.isBlank(), "SCRATCHBIRD_JDBC_URL is not set");

        String user = System.getenv("SCRATCHBIRD_JDBC_USER");
        String password = System.getenv("SCRATCHBIRD_JDBC_PASSWORD");
        try (Connection conn = user == null || user.isBlank()
                ? DriverManager.getConnection(url)
                : DriverManager.getConnection(url, user, password == null ? "" : password);
             Statement stmt = conn.createStatement();
             ResultSet rs = stmt.executeQuery("SELECT 1")) {
            assertTrue(rs.next());
            assertEquals(1, rs.getInt(1));
        }
    }

    @Test
    void connectsToConfiguredUrlAndReadsNavigatorTree() throws Exception {
        String url = System.getenv("SCRATCHBIRD_JDBC_URL");
        assumeTrue(url != null && !url.isBlank(), "SCRATCHBIRD_JDBC_URL is not set");

        String user = System.getenv("SCRATCHBIRD_JDBC_USER");
        String password = System.getenv("SCRATCHBIRD_JDBC_PASSWORD");
        try (Connection conn = user == null || user.isBlank()
                ? DriverManager.getConnection(url)
                : DriverManager.getConnection(url, user, password == null ? "" : password);
             Statement stmt = conn.createStatement();
             ResultSet rs = stmt.executeQuery(
                 "SELECT node_id, parent_node_id, object_id, parent_object_id, node_path, node_name, " +
                     "node_role, object_kind, object_path, schema_path " +
                     "FROM sys.catalog_readable.navigator_tree")) {
            ResultSetMetaData metadata = rs.getMetaData();
            assertTrue(metadata.getColumnCount() >= 10);
            assertTrue(rs.findColumn("node_path") > 0);
            assertTrue(rs.findColumn("node_role") > 0);
            assertTrue(rs.findColumn("object_kind") > 0);

            boolean sawDatabase = false;
            boolean sawManagement = false;
            boolean sawPhysicalSchema = false;
            while (rs.next()) {
                String nodePath = rs.getString("node_path");
                String nodeRole = rs.getString("node_role");
                if ("database".equalsIgnoreCase(nodeRole)) {
                    sawDatabase = true;
                }
                if ("default/Management".equalsIgnoreCase(nodePath)) {
                    sawManagement = true;
                }
                if ("default/sys".equalsIgnoreCase(nodePath) || "default/users".equalsIgnoreCase(nodePath)) {
                    sawPhysicalSchema = true;
                }
            }
            assertTrue(sawDatabase, "navigator tree did not expose the database root");
            assertTrue(sawManagement, "navigator tree did not expose Management");
            assertTrue(sawPhysicalSchema, "navigator tree did not expose a physical schema");
            assertFalse(conn.isClosed());
        }
    }
}
