// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package com.scratchbird.jdbc.tools;

import java.io.BufferedWriter;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.security.MessageDigest;
import java.sql.Connection;
import java.sql.DatabaseMetaData;
import java.sql.DriverManager;
import java.sql.PreparedStatement;
import java.sql.ResultSet;
import java.sql.ResultSetMetaData;
import java.sql.SQLException;
import java.sql.SQLWarning;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedHashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Properties;
import java.util.Set;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * Native JDBC conformance shell and JDBC usage example.
 *
 * <p>The implementation deliberately uses public JDBC APIs: DriverManager,
 * Connection, PreparedStatement, ResultSet, DatabaseMetaData, SQLWarning, and
 * SQLException. It does not call the C++ command-line tool or any private test
 * transport.</p>
 */
public final class SBIsqlJdbc {
    private static final List<String> PAGE_SIZES =
        List.of("4k", "8k", "16k", "32k", "64k", "128k");
    private static final List<String> ROUTES =
        List.of("embedded", "ipc_local", "listener-parser", "manager-listener-parser");
    private static final List<String> PARSER_MODES =
        List.of("server-parser", "standalone-parser", "driver-sblr-uuid");
    private static final List<String> SSL_MODES =
        List.of("disable", "allow", "prefer", "require", "verify-ca", "verify-full");
    private static final Set<String> SUPPORTED_ARGS = Set.of(
        "--database",
        "--host",
        "--port",
        "--user",
        "--password",
        "--role",
        "--sslmode",
        "--sslrootcert",
        "--sslcert",
        "--sslkey",
        "--route",
        "--parser-mode",
        "--page-size",
        "--namespace",
        "--input",
        "--output",
        "--error",
        "--diagnostics",
        "--metrics",
        "--transcript",
        "--summary",
        "--stop-on-error",
        "--expected-refusals",
        "--statement-timeout-ms",
        "--fetch-size",
        "--concurrency-worker",
        "--create-database",
        "--create-emulation-mode",
        "--run-id",
        "--language-resource-pack",
        "--language-resource-identity",
        "--language-resource-hash",
        "--language-profile",
        "--syntax-profile",
        "--topology-profile",
        "--standard-english-fallback");

    private SBIsqlJdbc() {
    }

    public static void main(String[] rawArgs) {
        int rc;
        try {
            rc = run(parseArgs(rawArgs));
        } catch (Exception ex) {
            ex.printStackTrace(System.err);
            rc = 1;
        }
        System.exit(rc);
    }

    static int run(Args args) throws Exception {
        validateArgs(args);
        Path summaryPath = Path.of(args.required("--summary"));
        Path runRoot = summaryPath.getParent() == null ? Path.of(".") : summaryPath.getParent();
        Files.createDirectories(runRoot);

        Path outputPath = Path.of(args.required("--output"));
        Path errorPath = Path.of(args.required("--error"));
        Path diagnosticsPath = Path.of(args.required("--diagnostics"));
        Path metricsPath = Path.of(args.required("--metrics"));
        Path transcriptPath = Path.of(args.required("--transcript"));
        Path eventsPath = runRoot.resolve("command-events.jsonl");
        Path wirePath = runRoot.resolve("wire-transcript.jsonl");
        Path timingPath = runRoot.resolve("timing-groups.json");
        Path digestsPath = runRoot.resolve("result-digests.json");
        Path metadataPath = runRoot.resolve("metadata-snapshots.json");
        Path refusalsPath = runRoot.resolve("security-refusals.json");
        Path apiPath = runRoot.resolve("native-api-coverage.json");
        Path reviewPath = runRoot.resolve("code-example-review.json");
        Path junitPath = runRoot.resolve("junit.xml");
        Path stdoutLogPath = runRoot.resolve("stdout.log");
        Path stderrLogPath = runRoot.resolve("stderr.log");

        initialize(outputPath, errorPath, diagnosticsPath, metricsPath, transcriptPath,
            eventsPath, wirePath, stdoutLogPath, stderrLogPath);
        Set<String> expectedRefusals =
            loadExpectedRefusals(args.valueOrDefault("--expected-refusals", ""));

        Map<String, Long> timings = new LinkedHashMap<>();
        Map<String, Integer> apiHits = new LinkedHashMap<>();
        for (String key : List.of(
            "DriverManager.getConnection",
            "Connection",
            "PreparedStatement",
            "ResultSet",
            "DatabaseMetaData",
            "SQLException",
            "SQLWarning")) {
            apiHits.put(key, 0);
        }
        List<Map<String, Object>> testcases = new ArrayList<>();
        List<Map<String, Object>> failures = new ArrayList<>();
        List<Map<String, Object>> digests = new ArrayList<>();
        List<Map<String, Object>> securityRefusals = new ArrayList<>();

        long started = System.nanoTime();
        Connection conn = null;
        try (BufferedWriter events = writer(eventsPath);
             BufferedWriter diagnostics = writer(diagnosticsPath);
             BufferedWriter transcript = writer(transcriptPath);
             BufferedWriter wire = writer(wirePath)) {
            long connectStarted = System.nanoTime();
            conn = connect(args);
            apiHits.merge("DriverManager.getConnection", 1, Integer::sum);
            apiHits.merge("Connection", 1, Integer::sum);
            addTiming(timings, "connection", System.nanoTime() - connectStarted);
            writeJsonl(transcript, mapOf(
                "event", "connect",
                "driver", "jdbc",
                "route", args.required("--route"),
                "parser_mode", args.required("--parser-mode"),
                "page_size", args.required("--page-size")));
            writeJsonl(wire, mapOf(
                "event", "server_admission_required",
                "driver_or_parser_finality", "forbidden",
                "parser_output_to_engine_required", true,
                "engine_sql_text_execution", false));

            if (args.booleanFlag("--create-database")) {
                throw new UnsupportedOperationException(
                    "--create-database is not implemented in the JDBC native tool yet"
                    + " (create_emulation_mode="
                    + args.valueOrDefault("--create-emulation-mode", "sbsql") + ")");
            }
            if (!"server-parser".equals(args.required("--parser-mode"))) {
                throw new UnsupportedOperationException(
                    args.required("--parser-mode")
                    + " is not yet implemented by the JDBC native tool; it fails closed");
            }

            List<String> statements = splitStatements(readInput(args.required("--input")));
            int index = 0;
            for (String sql : statements) {
                index++;
                String statementId = Path.of(args.required("--input")).getFileName() + ":" + index;
                boolean expectedRefusal = expectedRefusals.contains(statementId);
                String expectedOutcome = expectedRefusal ? "refusal" : "success";
                String group = classifyStatement(sql);
                long stmtStarted = System.nanoTime();
                String outcome = "success";
                String sqlState = null;
                String diagnostic = null;
                int rowCount = -1;
                String resultDigest = null;
                boolean stopAfterStatement = false;
                try (PreparedStatement statement = conn.prepareStatement(sql)) {
                    apiHits.merge("PreparedStatement", 1, Integer::sum);
                    boolean hasResult = statement.execute();
                    if (hasResult) {
                        try (ResultSet rs = statement.getResultSet()) {
                            apiHits.merge("ResultSet", 1, Integer::sum);
                            List<List<Object>> rows = readRows(rs);
                            rowCount = rows.size();
                            resultDigest = sha256(toJson(rows));
                            append(outputPath, toJson(mapOf(
                                "statement_id", statementId,
                                "rows", rows)) + "\n");
                        }
                    } else {
                        rowCount = statement.getUpdateCount();
                        resultDigest = sha256(String.valueOf(rowCount));
                    }
                    SQLWarning warning = statement.getWarnings();
                    if (warning != null) {
                        apiHits.merge("SQLWarning", 1, Integer::sum);
                        writeJsonl(diagnostics, mapOf(
                            "statement_id", statementId,
                            "sqlstate", warning.getSQLState(),
                            "message", warning.getMessage()));
                    }
                    if (expectedRefusal) {
                        outcome = "unexpected_success";
                        diagnostic = "statement succeeded but was expected to refuse";
                        failures.add(mapOf("statement_id", statementId, "message", diagnostic));
                    }
                } catch (SQLException ex) {
                    apiHits.merge("SQLException", 1, Integer::sum);
                    outcome = "refusal";
                    sqlState = ex.getSQLState();
                    diagnostic = ex.getMessage();
                    writeJsonl(diagnostics, mapOf(
                        "statement_id", statementId,
                        "sqlstate", sqlState,
                        "message", diagnostic));
                    append(errorPath, statementId + ": " + diagnostic + "\n");
                    if (expectedRefusal) {
                        securityRefusals.add(mapOf(
                            "statement_id", statementId,
                            "sqlstate", sqlState,
                            "diagnostic_code", diagnostic));
                    } else {
                        failures.add(mapOf("statement_id", statementId, "message", diagnostic));
                        stopAfterStatement = args.stopOnError();
                    }
                }
                long elapsed = System.nanoTime() - stmtStarted;
                addTiming(timings, group, elapsed);
                Map<String, Object> event = mapOf(
                    "run_id", args.valueOrDefault("--run-id", "manual"),
                    "driver_name", "jdbc",
                    "driver_version", "unknown",
                    "route", args.required("--route"),
                    "parser_mode", args.required("--parser-mode"),
                    "page_size", args.required("--page-size"),
                    "namespace", args.required("--namespace"),
                    "script", args.required("--input"),
                    "statement_index", index,
                    "statement_id", statementId,
                    "command_group", group,
                    "sql_hash", sha256(sql),
                    "expected_outcome", expectedOutcome,
                    "actual_outcome", outcome,
                    "sqlstate", sqlState,
                    "diagnostic_code", diagnostic,
                    "canonical_message_vector", List.of(),
                    "row_count", rowCount,
                    "result_digest", resultDigest,
                    "elapsed_ns", elapsed,
                    "server_revalidation_state", "required",
                    "language_profile", args.valueOrDefault("--language-profile", "en-US"),
                    "language_resource_pack", args.valueOrDefault("--language-resource-pack", "project/resources/seed-packs/initial-resource-pack/resources/i18n/sbsql-language-resource-pack"),
                    "language_resource_identity", args.valueOrDefault("--language-resource-identity", "sbsql.common_resource_pack.v1"),
                    "language_resource_hash", args.valueOrDefault("--language-resource-hash", "sha256:752c7a9823bdad00b48ab318c8b2d5d6d53b2739ecfe43f565952fd510f4e3dc"),
                    "syntax_profile", args.valueOrDefault("--syntax-profile", "sbsql.v3"),
                    "topology_profile", args.valueOrDefault("--topology-profile", "topology.sbsql.canonical.v1"),
                    "standard_english_fallback", args.booleanFlag("--standard-english-fallback", true),
                    "transaction_id_observed", null,
                    "mga_authority", "engine",
                    "native_api_surface", "jdbc_4_x",
                    "code_example_section", "prepared_execute_fetch");
                writeJsonl(events, event);
                testcases.add(event);
                digests.add(mapOf(
                    "statement_id", statementId,
                    "row_count", rowCount,
                    "result_digest", resultDigest));
                if (stopAfterStatement) {
                    break;
                }
            }

            long metadataStarted = System.nanoTime();
            writeText(metadataPath, toJson(metadataSnapshot(conn, apiHits)) + "\n");
            addTiming(timings, "metadata", System.nanoTime() - metadataStarted);
            if (!conn.getAutoCommit()) {
                conn.commit();
            }
        } catch (Exception ex) {
            failures.add(mapOf("statement_id", "run", "message", ex.getMessage()));
            append(stderrLogPath, stackTrace(ex));
            if (conn != null) {
                try {
                    conn.rollback();
                } catch (SQLException ignored) {
                    // Rollback failure is already secondary to the primary run failure.
                }
            }
        } finally {
            if (conn != null) {
                try {
                    conn.close();
                } catch (SQLException ignored) {
                    // Nothing useful can be reported after close failure here.
                }
            }
        }

        long elapsed = System.nanoTime() - started;
        timings.put("overall", elapsed);
        String sslmode = args.required("--sslmode");
        String transportMode = transportModeForRoute(args.required("--route"), sslmode);
        Map<String, Object> summary = mapOf(
            "run_id", args.valueOrDefault("--run-id", "manual"),
            "driver_name", "jdbc",
            "route", args.required("--route"),
            "parser_mode", args.required("--parser-mode"),
            "page_size", args.required("--page-size"),
            "namespace", args.required("--namespace"),
            "sslmode", sslmode,
            "transport_mode", transportMode,
            "tls_policy", tlsPolicyForRoute(args.required("--route"), sslmode),
            "language_resource_pack", args.valueOrDefault("--language-resource-pack", "project/resources/seed-packs/initial-resource-pack/resources/i18n/sbsql-language-resource-pack"),
            "language_resource_identity", args.valueOrDefault("--language-resource-identity", "sbsql.common_resource_pack.v1"),
            "language_resource_hash", args.valueOrDefault("--language-resource-hash", "sha256:752c7a9823bdad00b48ab318c8b2d5d6d53b2739ecfe43f565952fd510f4e3dc"),
            "language_resource_authority", "shared_server_parser_resource_pack",
            "language_profile", args.valueOrDefault("--language-profile", "en-US"),
            "syntax_profile", args.valueOrDefault("--syntax-profile", "sbsql.v3"),
            "topology_profile", args.valueOrDefault("--topology-profile", "topology.sbsql.canonical.v1"),
            "standard_english_fallback", args.booleanFlag("--standard-english-fallback", true),
            "status", failures.isEmpty() ? "pass" : "fail",
            "failure_count", failures.size(),
            "elapsed_ns", elapsed,
            "server_revalidation_required", true,
            "driver_or_parser_finality", "forbidden",
            "mga_authority", "engine");
        writeText(summaryPath, toJson(summary) + "\n");
        writeText(metricsPath, toJson(timings) + "\n");
        writeText(timingPath, toJson(timings) + "\n");
        writeText(digestsPath, toJson(digests) + "\n");
        writeText(refusalsPath, toJson(securityRefusals) + "\n");
        writeText(apiPath, toJson(apiHits) + "\n");
        writeText(reviewPath, toJson(mapOf(
            "driver", "jdbc",
            "public_api_only", true,
            "shells_out_to_other_driver", false,
            "source_is_canonical_example", true,
            "sections", List.of("connection", "prepared_execution", "fetch", "metadata", "diagnostics", "transaction"))) + "\n");
        writeText(junitPath, junit(testcases, failures));
        append(stdoutLogPath, "SBIsqlJdbc status=" + summary.get("status") + "\n");
        return failures.isEmpty() ? 0 : 1;
    }

    private static Connection connect(Args args) throws SQLException, ClassNotFoundException {
        Class.forName("com.scratchbird.jdbc.SBDriver");
        String url = "jdbc:scratchbird://" + args.required("--host") + ":"
            + args.required("--port") + "/" + args.required("--database")
            + "?sslmode=" + args.required("--sslmode")
            + "&binary_transfer=true"
            + "&ApplicationName=SBIsqlJdbc";
        if (!args.valueOrDefault("--sslrootcert", "").isBlank()) {
            url += "&sslrootcert=" + args.valueOrDefault("--sslrootcert", "");
        }
        if (!args.valueOrDefault("--sslcert", "").isBlank()) {
            url += "&sslcert=" + args.valueOrDefault("--sslcert", "");
        }
        if (!args.valueOrDefault("--sslkey", "").isBlank()) {
            url += "&sslkey=" + args.valueOrDefault("--sslkey", "");
        }
        Properties props = new Properties();
        props.setProperty("user", args.required("--user"));
        props.setProperty("password", args.required("--password"));
        if (!args.valueOrDefault("--role", "").isBlank()) {
            props.setProperty("role", args.valueOrDefault("--role", ""));
        }
        props.setProperty("connectTimeout", String.valueOf(Math.max(1,
            Integer.parseInt(args.valueOrDefault("--statement-timeout-ms", "30000")) / 1000)));
        return DriverManager.getConnection(url, props);
    }

    private static Map<String, Object> metadataSnapshot(Connection conn, Map<String, Integer> apiHits)
        throws SQLException {
        DatabaseMetaData metadata = conn.getMetaData();
        apiHits.merge("DatabaseMetaData", 1, Integer::sum);
        Map<String, Object> snapshot = new LinkedHashMap<>();
        snapshot.put("database_product_name", metadata.getDatabaseProductName());
        snapshot.put("database_product_version", metadata.getDatabaseProductVersion());
        snapshot.put("driver_name", metadata.getDriverName());
        snapshot.put("driver_version", metadata.getDriverVersion());
        snapshot.put("schemas_digest", digestResultSet(metadata.getSchemas()));
        snapshot.put("tables_digest", digestResultSet(metadata.getTables(null, null, "%", null)));
        return snapshot;
    }

    private static String digestResultSet(ResultSet rs) throws SQLException {
        try (ResultSet resultSet = rs) {
            return sha256(toJson(readRows(resultSet)));
        }
    }

    private static List<List<Object>> readRows(ResultSet rs) throws SQLException {
        ResultSetMetaData metadata = rs.getMetaData();
        int columnCount = metadata.getColumnCount();
        List<List<Object>> rows = new ArrayList<>();
        while (rs.next()) {
            List<Object> row = new ArrayList<>();
            for (int column = 1; column <= columnCount; column++) {
                row.add(rs.getObject(column));
            }
            rows.add(row);
        }
        return rows;
    }

    private static String classifyStatement(String sql) {
        String trimmed = sql.trim();
        if (trimmed.isEmpty()) {
            return "query";
        }
        String first = trimmed.split("\\s+", 2)[0].toLowerCase(Locale.ROOT);
        if (List.of("create", "alter", "drop").contains(first)) {
            return "ddl";
        }
        if (List.of("insert", "update", "delete", "merge", "upsert").contains(first)) {
            return "dml";
        }
        if (List.of("commit", "rollback", "savepoint", "begin", "start").contains(first)) {
            return "transaction";
        }
        if (List.of("grant", "revoke").contains(first)) {
            return "security_refusal";
        }
        if (trimmed.toLowerCase(Locale.ROOT).contains("sys.")) {
            return "metadata";
        }
        return "query";
    }

    private static void validateArgs(Args args) {
        if (!PAGE_SIZES.contains(args.required("--page-size"))) {
            throw new IllegalArgumentException("unsupported page size: " + args.required("--page-size"));
        }
        if (!ROUTES.contains(args.required("--route"))) {
            throw new IllegalArgumentException("unsupported route: " + args.required("--route"));
        }
        if (!PARSER_MODES.contains(args.required("--parser-mode"))) {
            throw new IllegalArgumentException("unsupported parser mode: " + args.required("--parser-mode"));
        }
        if (!SSL_MODES.contains(args.valueOrDefault("--sslmode", "require"))) {
            throw new IllegalArgumentException("unsupported sslmode: " + args.valueOrDefault("--sslmode", "require"));
        }
    }

    private static void initialize(Path... paths) throws IOException {
        for (Path path : paths) {
            Path parent = path.getParent();
            if (parent != null) {
                Files.createDirectories(parent);
            }
            Files.writeString(path, "", StandardCharsets.UTF_8);
        }
    }

    private static BufferedWriter writer(Path path) throws IOException {
        Path parent = path.getParent();
        if (parent != null) {
            Files.createDirectories(parent);
        }
        return Files.newBufferedWriter(path, StandardCharsets.UTF_8);
    }

    private static void writeJsonl(BufferedWriter writer, Map<String, Object> record) throws IOException {
        writer.write(toJson(record));
        writer.newLine();
        writer.flush();
    }

    private static void writeText(Path path, String text) throws IOException {
        Path parent = path.getParent();
        if (parent != null) {
            Files.createDirectories(parent);
        }
        Files.writeString(path, text, StandardCharsets.UTF_8);
    }

    private static void append(Path path, String text) throws IOException {
        Path parent = path.getParent();
        if (parent != null) {
            Files.createDirectories(parent);
        }
        Files.writeString(path, text, StandardCharsets.UTF_8,
            StandardOpenOption.CREATE,
            StandardOpenOption.APPEND);
    }

    private static void addTiming(Map<String, Long> timings, String group, long elapsed) {
        timings.merge(group, elapsed, Long::sum);
    }

    private static List<String> splitStatements(String sql) {
        // Canonical, SET TERM- and comment-aware top-level chunker shared across the
        // driver (see SBSQLParser.splitTopLevelStatements and the cross-driver
        // conformance fixture tests/conformance/drivers/chunker_conformance).
        return com.scratchbird.jdbc.SBSQLParser.splitTopLevelStatements(sql);
    }

    private static String readInput(String path) throws IOException {
        if ("-".equals(path)) {
            return new String(System.in.readAllBytes(), StandardCharsets.UTF_8);
        }
        return Files.readString(Path.of(path), StandardCharsets.UTF_8);
    }

    private static Set<String> loadExpectedRefusals(String path) throws IOException {
        Set<String> ids = new LinkedHashSet<>();
        if (path == null || path.isBlank()) {
            return ids;
        }
        Path refusalPath = Path.of(path);
        if (!Files.isRegularFile(refusalPath)) {
            throw new IOException("expected refusal file not found: " + refusalPath);
        }
        String doc = Files.readString(refusalPath, StandardCharsets.UTF_8);
        String trimmed = doc.stripLeading();
        if (!(trimmed.startsWith("{") || trimmed.startsWith("["))) {
            throw new IOException("expected refusals must be a JSON object or array");
        }
        addStringArrayField(doc, "statement_ids", ids);
        addStringArrayField(doc, "expected_refusals", ids);
        addObjectKeysField(doc, "expected_diagnostics", ids);
        addStringField(doc, "statement_id", ids);
        if (trimmed.startsWith("[") && !trimmed.contains("\"statement_id\"")) {
            Matcher matcher = Pattern.compile("\"((?:\\\\.|[^\"\\\\])*)\"").matcher(trimmed);
            while (matcher.find()) {
                String value = unescapeJsonString(matcher.group(1));
                if (value.contains(":")) {
                    ids.add(value);
                }
            }
        }
        return ids;
    }

    private static void addStringArrayField(String doc, String field, Set<String> ids) {
        Matcher matcher = Pattern.compile(
            "\"" + Pattern.quote(field) + "\"\\s*:\\s*\\[(.*?)\\]",
            Pattern.DOTALL).matcher(doc);
        while (matcher.find()) {
            Matcher strings = Pattern.compile("\"((?:\\\\.|[^\"\\\\])*)\"").matcher(matcher.group(1));
            while (strings.find()) {
                ids.add(unescapeJsonString(strings.group(1)));
            }
        }
    }

    private static void addObjectKeysField(String doc, String field, Set<String> ids) {
        Matcher matcher = Pattern.compile(
            "\"" + Pattern.quote(field) + "\"\\s*:\\s*\\{(.*?)\\}\\s*(?:,|\\})",
            Pattern.DOTALL).matcher(doc);
        while (matcher.find()) {
            Matcher keys = Pattern.compile("\"((?:\\\\.|[^\"\\\\])*)\"\\s*:").matcher(matcher.group(1));
            while (keys.find()) {
                ids.add(unescapeJsonString(keys.group(1)));
            }
        }
    }

    private static void addStringField(String doc, String field, Set<String> ids) {
        Matcher matcher = Pattern.compile(
            "\"" + Pattern.quote(field) + "\"\\s*:\\s*\"((?:\\\\.|[^\"\\\\])*)\"",
            Pattern.DOTALL).matcher(doc);
        while (matcher.find()) {
            ids.add(unescapeJsonString(matcher.group(1)));
        }
    }

    private static String unescapeJsonString(String value) {
        StringBuilder out = new StringBuilder();
        for (int i = 0; i < value.length(); i++) {
            char ch = value.charAt(i);
            if (ch != '\\' || i + 1 >= value.length()) {
                out.append(ch);
                continue;
            }
            char escaped = value.charAt(++i);
            switch (escaped) {
                case '"':
                case '\\':
                case '/':
                    out.append(escaped);
                    break;
                case 'b':
                    out.append('\b');
                    break;
                case 'f':
                    out.append('\f');
                    break;
                case 'n':
                    out.append('\n');
                    break;
                case 'r':
                    out.append('\r');
                    break;
                case 't':
                    out.append('\t');
                    break;
                case 'u':
                    if (i + 4 < value.length()) {
                        out.append((char) Integer.parseInt(value.substring(i + 1, i + 5), 16));
                        i += 4;
                    }
                    break;
                default:
                    out.append(escaped);
                    break;
            }
        }
        return out.toString();
    }

    private static String transportModeForRoute(String route, String sslmode) {
        if ("embedded".equals(route)) {
            return "embedded_no_network_transport";
        }
        if ("ipc_local".equals(route)) {
            return "local_ipc_no_tls";
        }
        return "disable".equalsIgnoreCase(sslmode) ? "tls_disabled" : "tls_required";
    }

    private static String tlsPolicyForRoute(String route, String sslmode) {
        if (!("listener-parser".equals(route) || "manager-listener-parser".equals(route))) {
            return "not_applicable_non_network_route";
        }
        return "disable".equalsIgnoreCase(sslmode) ? "explicit_non_tls_test_route" : "scratchbird_tls_1_3_floor";
    }

    private static String sha256(String value) {
        try {
            MessageDigest digest = MessageDigest.getInstance("SHA-256");
            byte[] bytes = digest.digest(value.getBytes(StandardCharsets.UTF_8));
            StringBuilder out = new StringBuilder("sha256:");
            for (byte b : bytes) {
                out.append(String.format("%02x", b));
            }
            return out.toString();
        } catch (Exception ex) {
            throw new IllegalStateException(ex);
        }
    }

    private static String toJson(Object value) {
        if (value == null) {
            return "null";
        }
        if (value instanceof String text) {
            return "\"" + escape(text) + "\"";
        }
        if (value instanceof Number || value instanceof Boolean) {
            return String.valueOf(value);
        }
        if (value instanceof Map<?, ?> map) {
            List<String> parts = new ArrayList<>();
            for (Map.Entry<?, ?> entry : map.entrySet()) {
                parts.add(toJson(String.valueOf(entry.getKey())) + ":" + toJson(entry.getValue()));
            }
            return "{" + String.join(",", parts) + "}";
        }
        if (value instanceof Iterable<?> iterable) {
            List<String> parts = new ArrayList<>();
            for (Object item : iterable) {
                parts.add(toJson(item));
            }
            return "[" + String.join(",", parts) + "]";
        }
        return toJson(String.valueOf(value));
    }

    private static String escape(String value) {
        return value
            .replace("\\", "\\\\")
            .replace("\"", "\\\"")
            .replace("\n", "\\n")
            .replace("\r", "\\r")
            .replace("\t", "\\t");
    }

    private static String stackTrace(Exception ex) {
        StringBuilder builder = new StringBuilder();
        builder.append(ex.getClass().getName()).append(": ").append(ex.getMessage()).append('\n');
        for (StackTraceElement element : ex.getStackTrace()) {
            builder.append("  at ").append(element).append('\n');
        }
        return builder.toString();
    }

    private static String junit(List<Map<String, Object>> testcases, List<Map<String, Object>> failures) {
        StringBuilder xml = new StringBuilder();
        xml.append("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
        xml.append("<testsuite name=\"SBIsqlJdbc\" tests=\"").append(testcases.size())
            .append("\" failures=\"").append(failures.size()).append("\">\n");
        if (testcases.isEmpty()) {
            xml.append("  <testcase classname=\"scratchbird.jdbc\" name=\"run\"></testcase>\n");
        }
        for (Map<String, Object> testcase : testcases) {
            xml.append("  <testcase classname=\"scratchbird.jdbc\" name=\"")
                .append(escapeXml(String.valueOf(testcase.get("statement_id"))))
                .append("\"></testcase>\n");
        }
        for (Map<String, Object> failure : failures) {
            xml.append("  <testcase classname=\"scratchbird.jdbc\" name=\"")
                .append(escapeXml(String.valueOf(failure.get("statement_id"))))
                .append("\"><failure message=\"")
                .append(escapeXml(String.valueOf(failure.get("message"))))
                .append("\" /></testcase>\n");
        }
        xml.append("</testsuite>\n");
        return xml.toString();
    }

    private static String escapeXml(String value) {
        return value
            .replace("&", "&amp;")
            .replace("<", "&lt;")
            .replace(">", "&gt;")
            .replace("\"", "&quot;");
    }

    private static Map<String, Object> mapOf(Object... pairs) {
        Map<String, Object> map = new LinkedHashMap<>();
        for (int i = 0; i < pairs.length; i += 2) {
            map.put(String.valueOf(pairs[i]), pairs[i + 1]);
        }
        return map;
    }

    static Args parseArgs(String[] rawArgs) {
        Map<String, String> values = new LinkedHashMap<>();
        for (int i = 0; i < rawArgs.length; i++) {
            String arg = rawArgs[i];
            if (!arg.startsWith("--")) {
                throw new IllegalArgumentException("unexpected positional argument: " + arg);
            }
            if (!SUPPORTED_ARGS.contains(arg)) {
                throw new IllegalArgumentException("unsupported argument: " + arg);
            }
            if ("--create-database".equals(arg) || "--stop-on-error".equals(arg) || "--standard-english-fallback".equals(arg)) {
                if (i + 1 < rawArgs.length && !rawArgs[i + 1].startsWith("--")) {
                    values.put(arg, rawArgs[++i]);
                } else {
                    values.put(arg, "true");
                }
                continue;
            }
            if (i + 1 >= rawArgs.length) {
                throw new IllegalArgumentException("missing value for " + arg);
            }
            values.put(arg, rawArgs[++i]);
        }
        return new Args(values);
    }

    static final class Args {
        private final Map<String, String> values;

        Args(Map<String, String> values) {
            this.values = values;
        }

        String required(String key) {
            String value = values.get(key);
            if (value == null || value.isBlank()) {
                throw new IllegalArgumentException("missing required argument " + key);
            }
            return value;
        }

        String valueOrDefault(String key, String fallback) {
            String value = values.get(key);
            return value == null || value.isBlank() ? fallback : value;
        }

        boolean booleanFlag(String key) {
            return booleanFlag(key, false);
        }

        boolean booleanFlag(String key, boolean fallback) {
            String value = values.get(key);
            if (value == null || value.isBlank()) {
                return fallback;
            }
            String normalized = value.toLowerCase(Locale.ROOT);
            if (List.of("true", "1", "yes", "on").contains(normalized)) {
                return true;
            }
            if (List.of("false", "0", "no", "off").contains(normalized)) {
                return false;
            }
            throw new IllegalArgumentException("invalid boolean value for " + key + ": " + value);
        }

        boolean stopOnError() {
            return booleanFlag("--stop-on-error", true);
        }
    }
}
