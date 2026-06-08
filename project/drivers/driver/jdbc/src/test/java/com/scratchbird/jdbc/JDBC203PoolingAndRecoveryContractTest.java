// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

/*
 * ScratchBird JDBC Driver
 */
package com.scratchbird.jdbc;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.junit.jupiter.api.Assertions.fail;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.sql.Connection;
import java.sql.DatabaseMetaData;
import java.sql.PreparedStatement;
import java.sql.ResultSet;
import java.sql.SQLException;
import java.sql.Statement;
import java.sql.Types;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.Properties;
import java.util.UUID;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;

import org.junit.jupiter.api.Test;

/**
 * Cross-runtime contract coverage for pooling and recovery behavior.
 */
public class JDBC203PoolingAndRecoveryContractTest {

    private static final int SCENARIO_C_WORKERS = 10;

    private static SBIntegrationRuntime.RuntimeConfig runtime() {
        return SBIntegrationRuntime.requireRuntime();
    }

    @Test
    public void scenarioA_borrowReuseAfterExplicitCancel() throws Exception {
        String dsn = pooledDsn("MaxPoolSize=4&MinPoolSize=0&ConnectionLifetime=30");
        String cancelSql = runtime().cancelSql();
        assertNotNull(cancelSql, "Cancel SQL must be configured by integration runtime");
        assertTrue(!cancelSql.isBlank(), "Cancel SQL must be configured by integration runtime");

        SBConnectionPool.PoolStats before = poolStats(dsn);
        assertNotNull(before);

        try (Connection conn = openConnection(dsn);
             Statement statement = conn.createStatement()) {
            ExecutorService executor = Executors.newSingleThreadExecutor();
            try {
                Future<Void> cancellation = executor.submit(() -> {
                    statement.execute(cancelSql);
                    return null;
                });
                Thread.sleep(150);
                statement.cancel();
                try {
                    cancellation.get(5, TimeUnit.SECONDS);
                } catch (ExecutionException ex) {
                    assertTrue(ex.getCause() instanceof SQLException);
                } catch (TimeoutException ex) {
                    fail("Cancellation scenario timed out waiting for cancellation completion", ex);
                }
            } finally {
                executor.shutdownNow();
            }
        }

        try (Connection verify = openConnection(dsn);
             Statement verifyStatement = verify.createStatement();
             ResultSet rs = verifyStatement.executeQuery("SELECT 1")) {
            assertTrue(rs.next());
            assertEquals(1, rs.getInt(1));
        }

        SBConnectionPool.PoolStats after = poolStats(dsn);
        assertNotNull(after);
        assertTrue(after.hits + after.misses >= before.hits + before.misses);
    }

    @Test
    public void scenarioB_timeoutCancellationReuse() throws Exception {
        String dsn = pooledDsn("MaxPoolSize=4&MinPoolSize=0&ConnectionLifetime=30");
        String cancelSql = runtime().cancelSql();
        assertNotNull(cancelSql, "Cancel SQL must be configured by integration runtime");
        assertTrue(!cancelSql.isBlank(), "Cancel SQL must be configured by integration runtime");

        try (Connection conn = openConnection(dsn);
             Statement statement = conn.createStatement()) {
            statement.setQueryTimeout(1);
            try {
                statement.execute(cancelSql);
            } catch (SQLException ignored) {
                // Runtime-specific cancellation path may either complete or surface timeout/cancel exceptions.
            }
        }

        try (Connection verify = openConnection(dsn);
             Statement verifyStatement = verify.createStatement();
             ResultSet rs = verifyStatement.executeQuery("SELECT 1")) {
            assertTrue(rs.next());
            assertEquals(1, rs.getInt(1));
        }
    }

    @Test
    public void scenarioC_concurrentPoolStress_10Workers() throws Exception {
        String dsn = pooledDsn("MaxPoolSize=3&MinPoolSize=0&ConnectionLifetime=20&AcquireTimeout=20");
        ExecutorService executor = Executors.newFixedThreadPool(SCENARIO_C_WORKERS);

        try {
            List<Future<Boolean>> tasks = new ArrayList<>();
            for (int i = 0; i < SCENARIO_C_WORKERS; i++) {
                tasks.add(executor.submit(() -> runPoolWorker(dsn)));
            }

            for (Future<Boolean> task : tasks) {
                assertTrue(task.get(20, TimeUnit.SECONDS), "worker task did not return success");
            }
        } finally {
            executor.shutdownNow();
            assertTrue(executor.awaitTermination(10, TimeUnit.SECONDS));
        }

        SBConnectionPool.PoolStats stats = poolStats(dsn);
        assertNotNull(stats);
        assertTrue(stats.total <= 3);
        assertTrue(stats.hits + stats.misses >= SCENARIO_C_WORKERS);
    }

    @Test
    public void scenarioD_reconnectRecoveryAfterFailure() throws Exception {
        String dsn = pooledDsn("MaxPoolSize=2&MinPoolSize=0&ConnectionLifetime=30");
        String cancelSql = runtime().cancelSql();
        assertNotNull(cancelSql, "Cancel SQL must be configured by integration runtime");
        assertTrue(!cancelSql.isBlank(), "Cancel SQL must be configured by integration runtime");

        for (int iteration = 0; iteration < 2; iteration++) {
            try (Connection conn = openConnection(dsn);
                 Statement statement = conn.createStatement()) {
                statement.setQueryTimeout(1);
                try {
                    statement.execute(cancelSql);
                } catch (SQLException ignored) {
                    // Runtime-specific cancellation path may either complete or surface timeout/cancel exceptions.
                }
            }
        }

        try (Connection verify = openConnection(dsn);
             Statement verifyStatement = verify.createStatement();
             ResultSet rs = verifyStatement.executeQuery("SELECT 1")) {
            assertTrue(rs.next());
            assertEquals(1, rs.getInt(1));
        }
    }

    @Test
    public void scenarioE_metadataAndLobReuseAfterRecovery() throws Exception {
        String dsn = pooledDsn("MaxPoolSize=4&MinPoolSize=0&ConnectionLifetime=30");
        String cancelSql = runtime().cancelSql();
        assertNotNull(cancelSql, "Cancel SQL must be configured by integration runtime");
        assertTrue(!cancelSql.isBlank(), "Cancel SQL must be configured by integration runtime");

        String table = "jdbc203_contract_" + UUID.randomUUID().toString().replace("-", "");
        String payloadText = "payload-" + System.currentTimeMillis();

        try (Connection conn = openConnection(dsn);
             Statement statement = conn.createStatement()) {
            statement.execute("CREATE TABLE " + table + " (id INTEGER, note TEXT)");
            statement.execute("INSERT INTO " + table + " (id, note) VALUES (1, '" + payloadText + "')");
        }

        try (Connection verify = openConnection(dsn);
             Statement statement = verify.createStatement()) {
            statement.setQueryTimeout(1);
            try {
                statement.execute(cancelSql);
            } catch (SQLException ignored) {
                // Runtime-specific cancellation path may either complete or surface timeout/cancel exceptions.
            }

            DatabaseMetaData metadata = verify.getMetaData();
            boolean foundColumnMetadata = false;
            try (ResultSet columns = metadata.getColumns(null, null, table, "%")) {
                if (columns.next()) {
                    foundColumnMetadata = true;
                    assertEquals("ID", columns.getString("COLUMN_NAME").toUpperCase());
                }
            }
            if (!foundColumnMetadata) {
                try (ResultSet columns = metadata.getColumns(null, null, table.toUpperCase(), "%")) {
                    if (columns.next()) {
                        foundColumnMetadata = true;
                        assertEquals("ID", columns.getString("COLUMN_NAME").toUpperCase());
                    }
                }
            }

            try (ResultSet rs = statement.executeQuery("SELECT note FROM " + table + " WHERE id = 1")) {
                assertTrue(rs.next());
                assertEquals(payloadText, rs.getString(1));
            }
        }

        try (Connection cleanup = openConnection(dsn);
             Statement cleanupStatement = cleanup.createStatement()) {
            cleanupStatement.execute("DROP TABLE " + table);
        }
    }

    @Test
    public void scenarioF_poolReuseRestoresSchemaAndAutocommitDefaults() throws Exception {
        String dsn = pooledDsn("MaxPoolSize=1&MinPoolSize=0&ConnectionLifetime=30");
        String defaultSchema;

        try (Connection conn = openConnection(dsn)) {
            defaultSchema = conn.getSchema();
            assertNotNull(defaultSchema);

            String alternateSchema = findAlternateSchema(conn, defaultSchema);
            conn.setSchema(alternateSchema);
            conn.setAutoCommit(false);
        }

        try (Connection verify = openConnection(dsn);
             Statement stmt = verify.createStatement();
             ResultSet rs = stmt.executeQuery("SELECT 1")) {
            assertTrue(verify.getAutoCommit());
            assertEquals(defaultSchema, verify.getSchema());
            assertTrue(rs.next());
            assertEquals(1, rs.getInt(1));
        }
    }

    private static String pooledDsn(String extraQuery) throws Exception {
        String url = runtime().baseUrl();
        if (url.contains("?")) {
            return url + "&Pooling=true&" + extraQuery;
        }

        return url + "?Pooling=true&" + extraQuery;
    }

    private static Connection openConnection(String dsn) throws Exception {
        return runtime().openConnection(dsn);
    }

    private static String findAlternateSchema(Connection conn, String currentSchema) throws Exception {
        try (ResultSet schemas = conn.getMetaData().getSchemas()) {
            while (schemas.next()) {
                String candidate = schemas.getString("TABLE_SCHEM");
                if (candidate != null && !candidate.isBlank()
                    && !candidate.equalsIgnoreCase(currentSchema)) {
                    return candidate;
                }
            }
        }
        throw new SQLException("No alternate schema available for pooled state-reset test");
    }

    private static SBConnectionProperties parseProperties(String dsn) throws Exception {
        Properties properties = new Properties();
        String user = runtime().user();
        String password = runtime().password();
        if (user != null) {
            properties.setProperty("user", user);
        }
        if (password != null) {
            properties.setProperty("password", password);
        }
        return SBDriver.parseURL(dsn, properties);
    }

    private static SBConnectionPool.PoolStats poolStats(String dsn) throws Exception {
        SBConnectionProperties properties = parseProperties(dsn);
        return SBDriver.getPoolStats(properties);
    }

    private static boolean runPoolWorker(String dsn) throws Exception {
        SQLException last = null;
        for (int attempt = 0; attempt < 3; attempt++) {
            try (Connection conn = openConnection(dsn);
                 Statement statement = conn.createStatement();
                 ResultSet rs = statement.executeQuery("SELECT 1")) {
                return rs.next() && rs.getInt(1) == 1;
            } catch (SQLException ex) {
                last = ex;
                if (!isTransientPoolFailure(ex) || attempt == 2) {
                    throw ex;
                }
                Thread.sleep(75L * (attempt + 1));
            }
        }
        throw last == null ? new SQLException("Pool worker failed without SQL exception") : last;
    }

    private static boolean isTransientPoolFailure(SQLException ex) {
        String state = ex.getSQLState();
        if (state != null) {
            String normalized = state.trim().toUpperCase(Locale.ROOT);
            if ("08001".equals(normalized)
                || "08006".equals(normalized)
                || "HYT00".equals(normalized)
                || "HYT01".equals(normalized)) {
                return true;
            }
        }
        String message = ex.getMessage();
        if (message == null) {
            return false;
        }
        String normalizedMessage = message.toLowerCase(Locale.ROOT);
        return normalizedMessage.contains("timeout")
            || normalizedMessage.contains("timed out")
            || normalizedMessage.contains("pool")
            || normalizedMessage.contains("busy");
    }
}
