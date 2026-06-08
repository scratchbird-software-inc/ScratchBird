// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package com.scratchbird.jdbc;

import java.io.IOException;
import java.net.InetSocketAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.attribute.PosixFilePermission;
import java.sql.Connection;
import java.sql.DriverManager;
import java.sql.ResultSet;
import java.sql.SQLException;
import java.sql.Statement;
import java.time.Duration;
import java.time.Instant;
import java.util.ArrayList;
import java.util.EnumSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;
import java.util.concurrent.TimeUnit;

/**
 * Shared runtime harness for JDBC integration tests.
 */
final class SBIntegrationRuntime {

    private static RuntimeConfig runtime;
    private static String lastAutobuildDiagnostic = "";

    private SBIntegrationRuntime() {
    }

    static synchronized RuntimeConfig requireRuntime() {
        if (runtime == null || !runtime.isUsable()) {
            if (runtime != null) {
                runtime.shutdown();
            }
            runtime = discoverOrStartRuntime();
        }
        if (!runtime.available) {
            throw new IllegalStateException(runtime.skipReason);
        }
        return runtime;
    }

    private static RuntimeConfig discoverOrStartRuntime() {
        String envUrl = env("SCRATCHBIRD_JDBC_URL");
        if (!envUrl.isBlank()) {
            RuntimeConfig envRuntime = new RuntimeConfig(
                true,
                envUrl,
                nullableEnv("SCRATCHBIRD_JDBC_USER"),
                nullableEnv("SCRATCHBIRD_JDBC_PASSWORD"),
                nullableEnv("SCRATCHBIRD_JDBC_CANCEL_SQL"),
                null,
                null,
                null,
                null,
                null,
                null,
                null
            );
            try {
                envRuntime.initializeFixture();
                Runtime.getRuntime().addShutdownHook(new Thread(envRuntime::shutdown, "sb-jdbc-it-shutdown"));
                return envRuntime;
            } catch (SQLException ex) {
                return new RuntimeConfig(false, null, null, null, null, null, null, null, null, null, null,
                    "Integration fixture bootstrap failed for SCRATCHBIRD_JDBC_URL: " + ex.getMessage());
            }
        }

        Path serverBinary = findServerBinary();
        if (serverBinary == null) {
            serverBinary = tryBuildServerBinary();
        }
        if (serverBinary == null) {
            String detail = lastAutobuildDiagnostic == null || lastAutobuildDiagnostic.isBlank()
                ? "No prebuilt sb_server found and auto-build did not produce a binary."
                : lastAutobuildDiagnostic;
            return new RuntimeConfig(false, null, null, null, null, null, null, null, null, null, null,
                "SCRATCHBIRD_JDBC_URL is not set and sb_server binary was not found. " + detail);
        }

        try {
            Path runtimeDir = Files.createTempDirectory("sb-jdbc-it-");
            int port = allocatePort();
            Path config = runtimeDir.resolve("sb_server.conf");
            Path dbFile = runtimeDir.resolve("integration.sbdb");
            Path logFile = runtimeDir.resolve("sb_server.log");
            Path bootstrapTokenFile = runtimeDir.resolve("bootstrap.token");
            String bootstrapToken = "sb-jdbc-it-" + Long.toHexString(System.nanoTime());
            Files.createDirectories(runtimeDir);
            Files.writeString(config, buildConfig(runtimeDir, dbFile, port), StandardCharsets.UTF_8);
            writeBootstrapToken(bootstrapTokenFile, bootstrapToken);

            ProcessBuilder processBuilder = new ProcessBuilder(
                serverBinary.toAbsolutePath().toString(),
                "-F",
                "--config", config.toAbsolutePath().toString(),
                "--verbose"
            )
                .redirectErrorStream(true)
                .redirectOutput(logFile.toFile());
            processBuilder.environment().put("SCRATCHBIRD_BOOTSTRAP_TOKEN_FILE",
                bootstrapTokenFile.toAbsolutePath().toString());
            processBuilder.environment().put("SCRATCHBIRD_BOOTSTRAP_FORCE", "1");
            processBuilder.environment().put("SCRATCHBIRD_NATIVE_FORCE_PASSWORD_AUTH", "1");
            Process process = processBuilder.start();

            if (!waitForPort("127.0.0.1", port, Duration.ofSeconds(20), process)) {
                String logTail = readLogTail(logFile);
                process.destroyForcibly();
                return new RuntimeConfig(false, null, null, null, null, null, null, null, null, null, null,
                    "Failed to start local sb_server on port " + port + ": " + logTail);
            }

            String url = "jdbc:scratchbird://127.0.0.1:" + port + "/main?sslmode=disable";
            RuntimeConfig configResult = new RuntimeConfig(
                true, url, "bootstrap", bootstrapToken, nullableEnv("SCRATCHBIRD_JDBC_CANCEL_SQL"),
                process, runtimeDir, config, logFile, bootstrapTokenFile, bootstrapToken, null
            );
            configResult.initializeFixture();
            Runtime.getRuntime().addShutdownHook(new Thread(configResult::shutdown, "sb-jdbc-it-shutdown"));
            return configResult;
        } catch (Exception ex) {
            return new RuntimeConfig(false, null, null, null, null, null, null, null, null, null, null,
                "Local integration bootstrap failed: " + ex.getMessage());
        }
    }

    private static String buildConfig(Path runtimeDir, Path dbFile, int port) {
        String user = currentUser();
        String group = currentGroup();
        return """
            [server]
            mode = single-database
            database = %s
            auto_create = true
            pid_file = %s
            run_as_user = %s
            run_as_group = %s

            [network]
            bind_address = 127.0.0.1
            control_socket_dir = %s
            native_port = %d
            native_pool_min = 2
            native_pool_max = 16
            native_health_check_interval_ms = 0
            pg_port = 0
            mysql_port = 0
            fb_port = 0

            [authentication]
            methods = trust
            """.formatted(
            dbFile.toAbsolutePath(),
            runtimeDir.resolve("sb_server.pid").toAbsolutePath(),
            user,
            group,
            runtimeDir.toAbsolutePath(),
            port
        );
    }

    private static void writeBootstrapToken(Path tokenFile, String token) throws IOException {
        Files.writeString(tokenFile, token + System.lineSeparator(), StandardCharsets.UTF_8);
        try {
            Set<PosixFilePermission> permissions = EnumSet.of(
                PosixFilePermission.OWNER_READ,
                PosixFilePermission.OWNER_WRITE
            );
            Files.setPosixFilePermissions(tokenFile, permissions);
        } catch (UnsupportedOperationException ignored) {
            // Non-POSIX filesystem; keep default permissions.
        }
    }

    private static String currentUser() {
        String user = System.getProperty("user.name");
        return (user == null || user.isBlank()) ? "scratchbird" : user;
    }

    private static String currentGroup() {
        try {
            Process process = new ProcessBuilder("id", "-gn").redirectErrorStream(true).start();
            byte[] output = process.getInputStream().readAllBytes();
            process.waitFor();
            String group = new String(output, StandardCharsets.UTF_8).trim();
            if (!group.isBlank()) {
                return group;
            }
        } catch (Exception ignored) {
            // Fall through to user-name fallback.
        }
        return currentUser();
    }

    private static Path findServerBinary() {
        List<Path> candidates = new ArrayList<>();

        String envServer = env("SCRATCHBIRD_SB_SERVER");
        if (!envServer.isBlank()) {
            candidates.add(Path.of(envServer));
        }

        String envRoot = env("SCRATCHBIRD_SOURCE_ROOT");
        if (!envRoot.isBlank()) {
            candidates.addAll(sbServerBinaryCandidates(Path.of(envRoot).toAbsolutePath().normalize()));
        }

        Path cwd = Path.of(System.getProperty("user.dir")).toAbsolutePath();
        Path cursor = cwd;
        for (int i = 0; i < 8 && cursor != null; i++) {
            if (Files.isDirectory(cursor.resolve("project")) && Files.isDirectory(cursor.resolve("docs"))) {
                candidates.addAll(sbServerBinaryCandidates(cursor.normalize()));
            }
            cursor = cursor.getParent();
        }

        for (Path candidate : candidates) {
            if (candidate != null && Files.isRegularFile(candidate) && Files.isExecutable(candidate)) {
                return candidate;
            }
        }
        Path onPath = findExecutableOnPath("sb_server");
        if (onPath != null) {
            return onPath;
        }
        return null;
    }

    private static Path tryBuildServerBinary() {
        StringBuilder diagnostic = new StringBuilder();
        String cmakeCommand = resolveFirstExecutable("cmake", "cmake3");
        if (cmakeCommand == null || cmakeCommand.isBlank()) {
            lastAutobuildDiagnostic = "cmake/cmake3 not available on PATH; cannot auto-build sb_server";
            return null;
        }
        for (Path sourceRoot : findServerSourceRoots()) {
            Path binary = firstExecutable(sbServerBinaryCandidates(sourceRoot));
            if (binary != null) {
                lastAutobuildDiagnostic = "";
                return binary;
            }

            Path srcDir = sourceRoot.resolve("src");
            if (!Files.isDirectory(srcDir)) {
                diagnostic.append("skip ").append(sourceRoot).append(" (missing src/). ");
                continue;
            }

            Path buildDir = sourceRoot.resolve("build");
            Path buildLog = buildDir.resolve("jdbc_it_autobuild.log");
            try {
                Files.createDirectories(buildDir);
            } catch (IOException ignored) {
                diagnostic.append("skip ").append(sourceRoot)
                    .append(" (cannot create build dir ").append(buildDir).append("). ");
                continue;
            }

            if (!runProcess(buildLog,
                cmakeCommand,
                "-S", sourceRoot.toAbsolutePath().toString(),
                "-B", buildDir.toAbsolutePath().toString(),
                "-DCMAKE_BUILD_TYPE=RelWithDebInfo")) {
                diagnostic.append(cmakeCommand).append(" configure failed for ").append(sourceRoot)
                    .append(" (see ").append(buildLog).append("). ");
                continue;
            }
            if (!runProcess(buildLog,
                cmakeCommand,
                "--build", buildDir.toAbsolutePath().toString(),
                "--target", "sb_server",
                "-j", "4")) {
                diagnostic.append(cmakeCommand).append(" build failed for ").append(sourceRoot)
                    .append(" (see ").append(buildLog).append("). ");
                continue;
            }
            binary = firstExecutable(sbServerBinaryCandidates(sourceRoot));
            if (binary != null) {
                lastAutobuildDiagnostic = "";
                return binary;
            }
            diagnostic.append("build completed without executable SBsrv under ")
                .append(sourceRoot.resolve("build")).append(". ");
        }
        lastAutobuildDiagnostic = diagnostic.toString().trim();
        return null;
    }

    private static List<Path> sbServerBinaryCandidates(Path sourceRoot) {
        if (sourceRoot == null) {
            return List.of();
        }
        Path normalized = sourceRoot.toAbsolutePath().normalize();
        List<Path> candidates = new ArrayList<>();
        candidates.add(normalized.resolve("build/output/linux/bin/SBsrv"));
        candidates.add(normalized.resolve("build/output/bsd/bin/SBsrv"));
        candidates.add(normalized.resolve("build/output/windows/bin/SBsrv.exe"));
        candidates.add(normalized.resolve("build/output/windows/bin/SBsrv"));
        candidates.add(normalized.resolve("build/bin/SBsrv"));
        candidates.add(normalized.resolve("build/src/sb_server"));
        candidates.add(normalized.resolve("build/src/server/sb_server"));
        candidates.add(normalized.resolve("build/bin/sb_server"));
        return candidates;
    }

    private static Path firstExecutable(List<Path> candidates) {
        if (candidates == null || candidates.isEmpty()) {
            return null;
        }
        for (Path candidate : candidates) {
            if (candidate != null && Files.isRegularFile(candidate) && Files.isExecutable(candidate)) {
                return candidate;
            }
        }
        return null;
    }

    private static String resolveFirstExecutable(String... candidates) {
        if (candidates == null || candidates.length == 0) {
            return null;
        }
        for (String candidate : candidates) {
            Path onPath = findExecutableOnPath(candidate);
            if (onPath != null) {
                return candidate;
            }
        }
        return null;
    }

    private static List<Path> findServerSourceRoots() {
        List<Path> roots = new ArrayList<>();

        String envRoot = env("SCRATCHBIRD_SOURCE_ROOT");
        if (!envRoot.isBlank()) {
            roots.add(Path.of(envRoot).toAbsolutePath().normalize());
        }

        Path cwd = Path.of(System.getProperty("user.dir")).toAbsolutePath();
        Path cursor = cwd;
        for (int i = 0; i < 8 && cursor != null; i++) {
            if (Files.isDirectory(cursor.resolve("project")) && Files.isDirectory(cursor.resolve("docs"))) {
                roots.add(cursor.normalize());
            }
            cursor = cursor.getParent();
        }

        List<Path> deduped = new ArrayList<>();
        for (Path root : roots) {
            if (root == null || deduped.contains(root)) {
                continue;
            }
            deduped.add(root);
        }
        return deduped;
    }

    private static boolean runProcess(Path logFile, String... command) {
        try {
            Process process = new ProcessBuilder(command)
                .redirectErrorStream(true)
                .redirectOutput(logFile.toFile())
                .start();
            if (!process.waitFor(10, TimeUnit.MINUTES)) {
                process.destroyForcibly();
                return false;
            }
            return process.exitValue() == 0;
        } catch (IOException | InterruptedException ex) {
            if (ex instanceof InterruptedException) {
                Thread.currentThread().interrupt();
            }
            return false;
        }
    }

    private static Path findExecutableOnPath(String executable) {
        if (executable == null || executable.isBlank()) {
            return null;
        }
        String path = System.getenv("PATH");
        if (path == null || path.isBlank()) {
            return null;
        }
        String[] entries = path.split(":");
        for (String entry : entries) {
            if (entry == null || entry.isBlank()) {
                continue;
            }
            Path candidate = Path.of(entry, executable);
            if (Files.isRegularFile(candidate) && Files.isExecutable(candidate)) {
                return candidate.toAbsolutePath().normalize();
            }
        }
        return null;
    }

    private static int allocatePort() throws IOException {
        try (ServerSocket socket = new ServerSocket(0)) {
            socket.setReuseAddress(true);
            return socket.getLocalPort();
        }
    }

    private static boolean waitForPort(String host, int port, Duration timeout, Process process) {
        Instant deadline = Instant.now().plus(timeout);
        while (Instant.now().isBefore(deadline)) {
            if (process != null && !process.isAlive()) {
                return false;
            }
            try (Socket socket = new Socket()) {
                socket.connect(new InetSocketAddress(host, port), 300);
                return true;
            } catch (IOException ignored) {
                try {
                    Thread.sleep(150);
                } catch (InterruptedException interrupted) {
                    Thread.currentThread().interrupt();
                    return false;
                }
            }
        }
        return false;
    }

    private static String readLogTail(Path logFile) {
        try {
            if (!Files.exists(logFile)) {
                return "no server log available";
            }
            List<String> lines = Files.readAllLines(logFile);
            int start = Math.max(0, lines.size() - 8);
            return String.join(" | ", lines.subList(start, lines.size()));
        } catch (IOException ex) {
            return "unable to read log: " + ex.getMessage();
        }
    }

    private static String env(String key) {
        String value = System.getenv(key);
        return value == null ? "" : value.trim();
    }

    private static String nullableEnv(String key) {
        String value = env(key);
        return value.isBlank() ? null : value;
    }

    static final class RuntimeConfig {
        private final boolean available;
        private final String url;
        private final String user;
        private final String password;
        private final Process process;
        private final Path runtimeDir;
        private final Path configFile;
        private final Path logFile;
        private final Path bootstrapTokenFile;
        private final String bootstrapToken;
        private final String skipReason;
        private volatile String runtimeUser;
        private volatile String runtimePassword;
        private volatile String runtimeCancelSql;

        private RuntimeConfig(boolean available, String url, String user, String password, String cancelSql,
                              Process process, Path runtimeDir, Path configFile, Path logFile,
                              Path bootstrapTokenFile, String bootstrapToken, String skipReason) {
            this.available = available;
            this.url = url;
            this.user = user;
            this.password = password;
            this.process = process;
            this.runtimeDir = runtimeDir;
            this.configFile = configFile;
            this.logFile = logFile;
            this.bootstrapTokenFile = bootstrapTokenFile;
            this.bootstrapToken = bootstrapToken;
            this.skipReason = skipReason == null ? "" : skipReason;
            this.runtimeUser = user;
            this.runtimePassword = password;
            this.runtimeCancelSql = cancelSql;
        }

        Connection openConnection() throws SQLException {
            refreshBootstrapTokenIfConfigured();
            ensureDriverLoaded();
            return openRuntimeConnection(url);
        }

        Connection openConnection(String dsn) throws SQLException {
            refreshBootstrapTokenIfConfigured();
            ensureDriverLoaded();
            return openRuntimeConnection(dsn);
        }

        private Connection openRuntimeConnection(String dsn) throws SQLException {
            String effectiveUser = runtimeUser;
            String effectivePassword = runtimePassword;
            if (effectiveUser != null) {
                return DriverManager.getConnection(dsn, effectiveUser,
                    effectivePassword == null ? "" : effectivePassword);
            }
            return DriverManager.getConnection(dsn);
        }

        String baseUrl() {
            return url;
        }

        String user() {
            return runtimeUser;
        }

        String password() {
            return runtimePassword;
        }

        String cancelSql() {
            return runtimeCancelSql;
        }

        private void refreshBootstrapTokenIfConfigured() throws SQLException {
            if (runtimeUser == null || !"bootstrap".equalsIgnoreCase(runtimeUser)) {
                return;
            }
            if (bootstrapTokenFile == null || bootstrapToken == null || bootstrapToken.isBlank()) {
                return;
            }
            try {
                writeBootstrapToken(bootstrapTokenFile, bootstrapToken);
            } catch (IOException ex) {
                throw new SQLException("Failed to refresh bootstrap token file: " + ex.getMessage(),
                    "08001", ex);
            }
        }

        private void ensureDriverLoaded() throws SQLException {
            try {
                Class.forName("com.scratchbird.jdbc.SBDriver");
            } catch (ClassNotFoundException ex) {
                throw new SQLException("ScratchBird JDBC driver class not found", "08001", ex);
            }
        }

        private void initializeFixture() throws SQLException {
            try (Connection conn = openConnection();
                 Statement stmt = conn.createStatement()) {
                try {
                    stmt.execute("CREATE TABLE type_coverage (id INTEGER, txt TEXT, b BYTEA, created_at TIMESTAMP)");
                } catch (SQLException ignored) {
                    // Table already exists from previous run.
                }

                int count = 0;
                try (ResultSet rs = stmt.executeQuery("SELECT COUNT(*) FROM type_coverage")) {
                    if (rs.next()) {
                        count = rs.getInt(1);
                    }
                }
                if (count == 0) {
                    seedTypeCoverageFixture(stmt);
                }

                // Move post-bootstrap integration runs onto a stable test principal so
                // concurrent pooling tests do not depend on one-time bootstrap token flow.
                final String candidateUser = "jdbc_it_runner";
                final String candidatePassword = "JdbcItRunner_Compat1!";
                try {
                    stmt.execute("CREATE USER " + candidateUser +
                        " WITH PASSWORD '" + candidatePassword + "' SUPERUSER");
                } catch (SQLException ignored) {
                    // User may already exist from prior bootstrap pass.
                }
                try {
                    stmt.execute("ALTER USER " + candidateUser +
                        " WITH PASSWORD '" + candidatePassword + "' SUPERUSER");
                } catch (SQLException ignored) {
                    // Keep bootstrap fallback if ALTER USER is unavailable.
                }
                try (Connection probe = DriverManager.getConnection(url, candidateUser, candidatePassword)) {
                    this.runtimeUser = candidateUser;
                    this.runtimePassword = candidatePassword;
                } catch (SQLException ignored) {
                    // Leave bootstrap credentials in place when user provisioning isn't available.
                }

                configureCancellationScenario();
            }
        }

        private void configureCancellationScenario() throws SQLException {
            if (runtimeCancelSql != null && !runtimeCancelSql.isBlank()) {
                return;
            }
            runtimeCancelSql = chooseCancellationProbeSql();
        }

        private void seedTypeCoverageFixture(Statement stmt) throws SQLException {
            String[] seedStatements = new String[] {
                "INSERT INTO type_coverage (id, txt, b, created_at) " +
                    "VALUES (1, 'fixture', E'\\\\x00', CURRENT_TIMESTAMP)",
                "INSERT INTO type_coverage (txt, b, created_at) " +
                    "VALUES ('fixture', E'\\\\x00', CURRENT_TIMESTAMP)",
                "INSERT INTO type_coverage DEFAULT VALUES"
            };
            for (String seedSql : seedStatements) {
                try {
                    stmt.execute(seedSql);
                    return;
                } catch (SQLException ignored) {
                    // Try the next shape variant.
                }
            }
        }

        private String chooseCancellationProbeSql() throws SQLException {
            List<String> candidates = List.of(
                "SELECT COUNT(*) FROM information_schema.columns c1, information_schema.columns c2, " +
                    "information_schema.columns c3",
                "SELECT COUNT(*) FROM information_schema.tables t1, information_schema.tables t2, " +
                    "information_schema.tables t3",
                "SELECT COUNT(*) FROM information_schema.columns c1, information_schema.columns c2",
                "SELECT 1"
            );

            String firstExecutable = null;
            for (String candidate : candidates) {
                ProbeOutcome outcome = probeCancellationSql(candidate);
                if (outcome.timeoutLike) {
                    return candidate;
                }
                if (!outcome.error && firstExecutable == null) {
                    firstExecutable = candidate;
                }
            }

            if (firstExecutable != null) {
                return firstExecutable;
            }
            return "SELECT 1";
        }

        private ProbeOutcome probeCancellationSql(String sql) throws SQLException {
            try (Connection conn = openConnection();
                 Statement stmt = conn.createStatement()) {
                stmt.setQueryTimeout(1);
                stmt.execute(sql);
                return ProbeOutcome.success();
            } catch (SQLException ex) {
                return ProbeOutcome.failure(isTimeoutOrCancel(ex));
            }
        }

        private boolean isTimeoutOrCancel(SQLException ex) {
            if (ex instanceof java.sql.SQLTimeoutException) {
                return true;
            }
            String state = ex.getSQLState();
            if (state != null) {
                String normalized = state.trim().toUpperCase(Locale.ROOT);
                if ("57014".equals(normalized) || "HYT00".equals(normalized) || "HYT01".equals(normalized)) {
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
                || normalizedMessage.contains("cancel");
        }

        private static final class ProbeOutcome {
            private final boolean error;
            private final boolean timeoutLike;

            private ProbeOutcome(boolean error, boolean timeoutLike) {
                this.error = error;
                this.timeoutLike = timeoutLike;
            }

            static ProbeOutcome success() {
                return new ProbeOutcome(false, false);
            }

            static ProbeOutcome failure(boolean timeoutLike) {
                return new ProbeOutcome(true, timeoutLike);
            }
        }

        private boolean isUsable() {
            if (!available) {
                return false;
            }
            if (process != null && !process.isAlive()) {
                return false;
            }
            return true;
        }

        private void shutdown() {
            if (process != null && process.isAlive()) {
                process.destroy();
                try {
                    if (!process.waitFor(5, TimeUnit.SECONDS)) {
                        process.destroyForcibly();
                        process.waitFor(5, TimeUnit.SECONDS);
                    }
                } catch (InterruptedException ex) {
                    Thread.currentThread().interrupt();
                    process.destroyForcibly();
                }
            }
            if (runtimeDir != null) {
                try {
                    if (logFile != null && Files.exists(logFile) && !Files.isWritable(logFile)) {
                        return;
                    }
                    if (configFile != null) {
                        Files.deleteIfExists(configFile);
                    }
                    if (logFile != null) {
                        Files.deleteIfExists(logFile);
                    }
                    if (bootstrapTokenFile != null) {
                        Files.deleteIfExists(bootstrapTokenFile);
                    }
                    Files.deleteIfExists(runtimeDir.resolve("integration.sbdb"));
                    Files.deleteIfExists(runtimeDir.resolve("integration.sbdb.lck"));
                    Files.deleteIfExists(runtimeDir.resolve("sb_server.pid"));
                    Files.deleteIfExists(runtimeDir);
                } catch (IOException ignored) {
                    // Best-effort cleanup.
                }
            }
        }
    }
}
