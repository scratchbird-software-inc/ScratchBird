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

import java.sql.Connection;
import java.sql.ResultSet;
import java.sql.Statement;

import org.junit.jupiter.api.Test;

class SBNativeSQLParityTest {

    private SBIntegrationRuntime.RuntimeConfig runtime() {
        return SBIntegrationRuntime.requireRuntime();
    }

    private Connection openConnection() throws Exception {
        String base = runtime().baseUrl();
        String dsn = base.contains("?") ? base + "&pooling=false" : base + "?pooling=false";
        return runtime().openConnection(dsn);
    }

    @Test
    void nativeSqlFunctionEscapeMatchesCanonicalExecution() throws Exception {
        try (Connection conn = openConnection()) {
            String converted = conn.nativeSQL("SELECT {fn UCASE('abc')}");
            assertEquals("SELECT UPPER('abc')", converted);

            Object viaNative = querySingleValue(conn, converted);
            Object viaCanonical = querySingleValue(conn, "SELECT UPPER('abc')");
            assertEquals(viaCanonical, viaNative);
        }
    }

    @Test
    void nativeSqlDateLiteralMatchesCanonicalExecution() throws Exception {
        try (Connection conn = openConnection()) {
            String converted = conn.nativeSQL("SELECT {d '2026-03-03'}");
            assertEquals("SELECT CAST('2026-03-03' AS DATE)", converted);

            Object viaNative = querySingleValue(conn, converted);
            Object viaCanonical = querySingleValue(conn, "SELECT CAST('2026-03-03' AS DATE)");
            assertEquals(viaCanonical, viaNative);
        }
    }

    @Test
    void nativeSqlTimeLiteralMatchesCanonicalExecution() throws Exception {
        try (Connection conn = openConnection()) {
            String converted = conn.nativeSQL("SELECT {t '12:34:56'}");
            assertEquals("SELECT CAST('12:34:56' AS TIME)", converted);

            Object viaNative = querySingleValue(conn, converted);
            Object viaCanonical = querySingleValue(conn, "SELECT CAST('12:34:56' AS TIME)");
            assertEquals(viaCanonical, viaNative);
        }
    }

    @Test
    void nativeSqlTimestampLiteralMatchesCanonicalExecution() throws Exception {
        try (Connection conn = openConnection()) {
            String converted = conn.nativeSQL("SELECT {ts '2026-03-03 12:34:56'}");
            assertEquals("SELECT CAST('2026-03-03 12:34:56' AS TIMESTAMP)", converted);

            Object viaNative = querySingleValue(conn, converted);
            Object viaCanonical = querySingleValue(conn, "SELECT CAST('2026-03-03 12:34:56' AS TIMESTAMP)");
            assertEquals(viaCanonical, viaNative);
        }
    }

    private static Object querySingleValue(Connection conn, String sql) throws Exception {
        try (Statement stmt = conn.createStatement();
             ResultSet rs = stmt.executeQuery(sql)) {
            assertTrue(rs.next(), "expected one row for SQL: " + sql);
            return rs.getObject(1);
        }
    }
}
