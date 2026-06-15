// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package com.scratchbird.jdbc;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.junit.jupiter.api.Assumptions.assumeTrue;

import java.sql.Connection;
import java.sql.DriverManager;
import java.sql.ResultSet;
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
}
