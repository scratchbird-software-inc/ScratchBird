"use strict";
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0
Object.defineProperty(exports, "__esModule", { value: true });
exports.NATIVE_ROWSET_TYPE_BINARY = exports.NATIVE_ROWSET_TYPE_REAL64 = exports.NATIVE_ROWSET_TYPE_UINT64 = exports.NATIVE_ROWSET_TYPE_INT32 = exports.NATIVE_ROWSET_TYPE_BOOLEAN = exports.NATIVE_ROWSET_TYPE_INT64 = exports.NATIVE_ROWSET_TYPE_TEXT = void 0;
exports.copyTextRowsToNativeFrame = copyTextRowsToNativeFrame;
// Native Node.js conformance shell and canonical driver usage example.
const node_crypto_1 = require("node:crypto");
const promises_1 = require("node:fs/promises");
const node_path_1 = require("node:path");
const index_1 = require("../index");
const protocol_1 = require("../protocol");
const sql_1 = require("../sql");
const PAGE_SIZES = new Set(["4k", "8k", "16k", "32k", "64k", "128k"]);
const PAGE_SIZE_BYTES = {
    "4k": 4096,
    "8k": 8192,
    "16k": 16384,
    "32k": 32768,
    "64k": 65536,
    "128k": 131072,
};
exports.NATIVE_ROWSET_TYPE_TEXT = 1;
exports.NATIVE_ROWSET_TYPE_INT64 = 2;
exports.NATIVE_ROWSET_TYPE_BOOLEAN = 3;
exports.NATIVE_ROWSET_TYPE_INT32 = 4;
exports.NATIVE_ROWSET_TYPE_UINT64 = 5;
exports.NATIVE_ROWSET_TYPE_REAL64 = 6;
exports.NATIVE_ROWSET_TYPE_BINARY = 7;
const ROUTES = new Set(["embedded", "ipc_local", "listener-parser", "manager-listener-parser"]);
const PARSER_MODES = new Set(["server-parser", "standalone-parser", "driver-sblr-uuid"]);
const SUPPORTED_ARGS = new Set([
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
    "--ipc-path",
    "--manager-auth-token",
    "--manager-database",
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
    "--standard-english-fallback",
]);
function currentProcessMetrics() {
    const usage = process.memoryUsage();
    const rssKb = Math.max(1, Math.ceil(usage.rss / 1024));
    const vsizeKb = Math.max(1, Math.ceil((usage.rss + usage.heapTotal + usage.external) / 1024));
    return {
        client: {
            max_rss_kb: rssKb,
            max_vsize_kb: vsizeKb,
            last_rss_kb: rssKb,
            last_vsize_kb: vsizeKb,
        },
    };
}
async function main() {
    const args = parseArgs(process.argv.slice(2));
    const status = await run(args);
    process.exitCode = status;
}
async function run(args) {
    validate(args);
    const runRoot = (0, node_path_1.dirname)(args.summary);
    await (0, promises_1.mkdir)(runRoot, { recursive: true });
    const paths = {
        events: `${runRoot}/command-events.jsonl`,
        wire: `${runRoot}/wire-transcript.jsonl`,
        timing: `${runRoot}/timing-groups.json`,
        digests: `${runRoot}/result-digests.json`,
        metadata: `${runRoot}/metadata-snapshots.json`,
        routeEnv: `${runRoot}/route-environment.json`,
        refusals: `${runRoot}/security-refusals.json`,
        api: `${runRoot}/native-api-coverage.json`,
        review: `${runRoot}/code-example-review.json`,
        junit: `${runRoot}/junit.xml`,
        stdoutLog: `${runRoot}/stdout.log`,
        stderrLog: `${runRoot}/stderr.log`,
    };
    for (const file of [
        args.output,
        args.error,
        args.diagnostics,
        args.metrics,
        args.transcript,
        args.summary,
        ...Object.values(paths),
    ]) {
        await writeText(file, "");
    }
    const timings = {};
    const apiHits = {
        ScratchBirdClient: 0,
        connect: 0,
        prepare: 0,
        execute: 0,
        query: 0,
        transaction: 0,
        queryMetadata: 0,
        copyIn: 0,
    };
    const testcases = [];
    const failures = [];
    const digests = [];
    const securityRefusals = [];
    const started = process.hrtime.bigint();
    let client;
    await writeText(paths.routeEnv, jsonText(routeEnvironment(args, null, "fail", "not_probed")) + "\n");
    try {
        const config = {
            host: args.host,
            port: args.port,
            database: args.database,
            user: args.user,
            password: args.password,
            role: args.role || undefined,
            sslrootcert: args.sslrootcert || undefined,
            sslcert: args.sslcert || undefined,
            sslkey: args.sslkey || undefined,
            binaryTransfer: true,
            applicationName: "SBIsqlNode",
            frontDoorMode: frontDoorModeForRoute(args.route),
            transportMode: args.route === "ipc_local" ? "local_ipc" : "inet_listener",
            ipcMethod: args.route === "ipc_local" ? "unix" : undefined,
            ipcPath: args.route === "ipc_local" ? args.ipcPath : undefined,
            sslmode: args.route === "ipc_local" ? "disable" : args.sslmode,
            managerAuthToken: args.route === "manager-listener-parser" ? args.managerAuthToken : undefined,
            managerDatabase: args.route === "manager-listener-parser" ? args.managerDatabase : undefined,
            managerUsername: args.route === "manager-listener-parser" ? args.user : undefined,
            metadataExpandSchemaParents: true,
        };
        client = new index_1.Client(config);
        apiHits.ScratchBirdClient++;
        const connectStarted = process.hrtime.bigint();
        await client.connect();
        apiHits.connect++;
        addTiming(timings, "connection", connectStarted);
        await appendJsonl(args.transcript, {
            event: "connect",
            driver: "node",
            route: args.route,
            parser_mode: args.parserMode,
            page_size: args.pageSize,
        });
        await appendJsonl(paths.wire, {
            event: "server_admission_required",
            driver_or_parser_finality: "forbidden",
        });
        if (args.createDatabase) {
            const createStarted = process.hrtime.bigint();
            await client.attachCreate(args.createEmulationMode, args.database);
            apiHits.attachCreate = (apiHits.attachCreate ?? 0) + 1;
            addTiming(timings, "database_create", createStarted);
        }
        const routeEnv = await probeRouteEnvironment(client, args);
        await writeText(paths.routeEnv, jsonText(routeEnv) + "\n");
        if (args.route !== "embedded" && routeEnv.page_size_verification_status !== "pass") {
            failures.push({
                statement_id: "route_page_size",
                message: "route page-size verification failed",
                expected_page_size_bytes: routeEnv.expected_page_size_bytes,
                actual_page_size_bytes: routeEnv.actual_page_size_bytes,
            });
            throw new Error("route page-size verification failed");
        }
        const expectedRefusals = await loadExpectedRefusals(args.expectedRefusals);
        const script = await readInput(args.input);
        const statements = (0, sql_1.splitTopLevelStatements)(script);
        for (let i = 0; i < statements.length; i++) {
            const sql = statements[i];
            const statementId = `${(0, node_path_1.basename)(args.input)}:${i + 1}`;
            const group = classifyStatement(sql);
            const statementStarted = process.hrtime.bigint();
            const expectedOutcome = expectedRefusals.has(statementId) ? "refusal" : "success";
            let outcome = "success";
            let rowCount = -1;
            let resultDigest = null;
            let diagnosticCode = null;
            let sqlState = null;
            let breakAfterEvent = false;
            try {
                const executableSql = executableSqlWithoutCopyMarkers(sql);
                const copyPayload = copyPayloadForStatement(sql);
                if (isCopyStdinStatement(sql)) {
                    if (copyPayload.length === 0) {
                        throw new Error("COPY FROM STDIN requires SB_COPY_INPUT rows in the script");
                    }
                    const nativeCopyPayload = copyTextRowsToNativeFrame(copyPayload);
                    const rowsCopied = await executeCopyIn(client, executableSql, nativeCopyPayload, args.statementTimeoutMs);
                    apiHits.copyIn++;
                    const rows = [["copy_in", rowsCopied]];
                    rowCount = rowsCopied;
                    resultDigest = sha256(jsonText(rows));
                    await appendText(args.output, jsonText({ statement_id: statementId, rows }) + "\n");
                    await appendJsonl(paths.wire, {
                        event: "copy_in",
                        statement_id: statementId,
                        parser_mode: args.parserMode,
                        payload_bytes: nativeCopyPayload.length,
                        copy_payload_format: "sbnr_native_rowset",
                        rows_copied: rowsCopied,
                        engine_sql_text_execution: false,
                        mga_authority: "engine",
                    });
                }
                else if (group === "transaction" && args.parserMode !== "server-parser") {
                    await runTransaction(client, sql);
                    apiHits.transaction++;
                    rowCount = 0;
                    resultDigest = sha256("transaction");
                }
                else if (args.parserMode === "server-parser") {
                    const result = await client.query(sql, undefined, {
                        maxRows: args.fetchSize,
                        timeoutMs: args.statementTimeoutMs,
                    });
                    apiHits.query++;
                    rowCount = result.rowCount;
                    resultDigest = sha256(jsonText(result.rows));
                    await appendText(args.output, jsonText({ statement_id: statementId, rows: result.rows }) + "\n");
                }
                else {
                    const compiled = await client.compileSblr(sql, { timeoutMs: args.statementTimeoutMs });
                    apiHits.returnSblr = (apiHits.returnSblr ?? 0) + 1;
                    await appendJsonl(paths.wire, {
                        event: "driver_sblr_compile",
                        driver: "node",
                        parser_mode: args.parserMode,
                        statement_id: statementId,
                        sblr_hash: compiled.hash.toString(),
                        sblr_version: compiled.version,
                        sblr_bytes: compiled.bytecode.length,
                    });
                    const result = await client.executeSblr(compiled.hash, compiled.bytecode, undefined, {
                        maxRows: args.fetchSize,
                        timeoutMs: args.statementTimeoutMs,
                    });
                    apiHits.executeSblr = (apiHits.executeSblr ?? 0) + 1;
                    rowCount = result.rowCount;
                    if (group === "query" || group === "metadata" || result.rows.length > 0) {
                        resultDigest = sha256(jsonText(result.rows));
                        await appendText(args.output, jsonText({ statement_id: statementId, rows: result.rows }) + "\n");
                    }
                    else {
                        resultDigest = sha256(String(rowCount));
                    }
                    await appendJsonl(paths.wire, {
                        event: "driver_sblr_execute",
                        driver: "node",
                        parser_mode: args.parserMode,
                        statement_id: statementId,
                        sblr_hash: compiled.hash.toString(),
                        sblr_version: compiled.version,
                        sblr_bytes: compiled.bytecode.length,
                        engine_sql_text_execution: false,
                        mga_authority: "engine",
                    });
                }
                if (expectedOutcome === "refusal") {
                    outcome = "unexpected_success";
                    failures.push({ statement_id: statementId, message: "statement succeeded but was expected to refuse" });
                    breakAfterEvent = args.stopOnError;
                }
            }
            catch (error) {
                outcome = "refusal";
                diagnosticCode = error instanceof Error ? error.message : String(error);
                sqlState = "HY000";
                await appendJsonl(args.diagnostics, {
                    statement_id: statementId,
                    sqlstate: sqlState,
                    message: diagnosticCode,
                });
                await appendText(args.error, `${statementId}: ${diagnosticCode}\n`);
                if (expectedOutcome === "refusal") {
                    securityRefusals.push({
                        statement_id: statementId,
                        sqlstate: sqlState,
                        diagnostic_code: diagnosticCode,
                    });
                }
                else {
                    failures.push({ statement_id: statementId, message: diagnosticCode });
                    breakAfterEvent = args.stopOnError;
                }
            }
            addTiming(timings, group, statementStarted);
            const event = {
                run_id: args.runId,
                driver_name: "node",
                driver_version: "unknown",
                route: args.route,
                parser_mode: args.parserMode,
                page_size: args.pageSize,
                namespace: args.namespace,
                script: args.input,
                statement_index: i + 1,
                statement_id: statementId,
                command_group: group,
                sql_hash: sha256(sql),
                expected_outcome: expectedOutcome,
                actual_outcome: outcome,
                sqlstate: sqlState,
                diagnostic_code: diagnosticCode,
                canonical_message_vector: [],
                row_count: rowCount,
                result_digest: resultDigest,
                elapsed_ns: Number(process.hrtime.bigint() - statementStarted),
                server_revalidation_state: "required",
                language_profile: args.languageProfile,
                language_resource_pack: args.languageResourcePack,
                language_resource_identity: args.languageResourceIdentity,
                language_resource_hash: args.languageResourceHash,
                syntax_profile: args.syntaxProfile,
                topology_profile: args.topologyProfile,
                standard_english_fallback: args.standardEnglishFallback,
                transaction_id_observed: null,
                mga_authority: "engine",
                native_api_surface: "node",
                code_example_section: isCopyStdinStatement(sql) ? "copy_in" : "query_execute_fetch",
            };
            await appendJsonl(paths.events, event);
            testcases.push(event);
            digests.push({ statement_id: statementId, row_count: rowCount, result_digest: resultDigest });
            if (breakAfterEvent) {
                break;
            }
        }
        const metadataStarted = process.hrtime.bigint();
        apiHits.queryMetadata += await emitMetadataSnapshot(client, paths.metadata, args);
        addTiming(timings, "metadata", metadataStarted);
    }
    catch (error) {
        const message = error instanceof Error ? error.message : String(error);
        failures.push({ statement_id: "run", message });
        await appendText(paths.stderrLog, `${message}\n`);
    }
    finally {
        if (client) {
            await client.end();
        }
    }
    const elapsed = Number(process.hrtime.bigint() - started);
    timings.overall = elapsed;
    const transportMode = transportModeForRoute(args.route, args.sslmode);
    const processMetrics = currentProcessMetrics();
    const summary = {
        run_id: args.runId,
        driver_name: "node",
        route: args.route,
        parser_mode: args.parserMode,
        page_size: args.pageSize,
        namespace: args.namespace,
        sslmode: args.sslmode,
        transport_mode: transportMode,
        transport_endpoint_kind: endpointKindForRoute(args.route),
        driver_transport_implementation: transportImplementationForRoute(args.route),
        cpp_library_boundary: "none",
        language_resource_pack: args.languageResourcePack,
        language_resource_identity: args.languageResourceIdentity,
        language_resource_hash: args.languageResourceHash,
        language_resource_authority: "shared_server_parser_resource_pack",
        language_profile: args.languageProfile,
        syntax_profile: args.syntaxProfile,
        topology_profile: args.topologyProfile,
        standard_english_fallback: args.standardEnglishFallback,
        status: failures.length === 0 ? "pass" : "fail",
        failure_count: failures.length,
        elapsed_ns: elapsed,
        process_metrics: processMetrics,
        server_revalidation_required: true,
        driver_or_parser_finality: "forbidden",
        mga_authority: "engine",
    };
    await writeText(args.summary, jsonText(summary) + "\n");
    const clientMetrics = processMetrics.client;
    await writeText(args.metrics, jsonText({
        role: "client",
        rss_kb: clientMetrics.last_rss_kb,
        vsize_kb: clientMetrics.last_vsize_kb,
    }) + "\n");
    await writeText(paths.timing, jsonText(timings) + "\n");
    await writeText(paths.digests, jsonText(digests) + "\n");
    await writeText(paths.refusals, jsonText(securityRefusals) + "\n");
    await writeText(paths.api, jsonText(apiHits) + "\n");
    await writeText(paths.review, jsonText({
        driver: "node",
        public_api_only: true,
        shells_out_to_other_driver: false,
        source_is_canonical_example: true,
        sections: ["connection", "prepared_execution", "fetch", "metadata", "diagnostics", "transaction"],
    }) + "\n");
    await writeText(paths.junit, junit(testcases, failures));
    await appendText(paths.stdoutLog, `SBIsqlNode status=${summary.status}\n`);
    return failures.length === 0 ? 0 : 1;
}
function executableSqlWithoutCopyMarkers(sql) {
    return sql
        .split(/\n/)
        .filter((line) => !line.trimStart().startsWith("-- SB_COPY_INPUT "))
        .join("\n")
        .trim();
}
function copyPayloadForStatement(sql) {
    const rows = sql
        .split(/\n/)
        .map((line) => line.trimStart())
        .filter((line) => line.startsWith("-- SB_COPY_INPUT "))
        .map((line) => line.slice("-- SB_COPY_INPUT ".length).replace(/\r$/, ""));
    return Buffer.from(rows.join("\n") + (rows.length ? "\n" : ""), "utf8");
}
function copyTextRowsToNativeFrame(data) {
    if (data.subarray(0, 4).toString("utf8") === "SBNR") {
        return data;
    }
    const lines = data
        .toString("utf8")
        .replace(/\r\n/g, "\n")
        .split("\n")
        .map((line) => line.replace(/\r$/, ""))
        .filter((line) => line.trim().length > 0);
    if (lines.length === 0) {
        throw new Error("COPY input contains no rows");
    }
    if (lines[0].includes(";") && lines[0].includes("=")) {
        let columns = null;
        const rows = [];
        for (const line of lines) {
            const names = [];
            const values = [];
            for (const part of line.split(";")) {
                if (part.length === 0) {
                    continue;
                }
                const separator = part.indexOf("=");
                if (separator <= 0) {
                    throw new Error("malformed canonical COPY field");
                }
                names.push(part.slice(0, separator));
                const value = part.slice(separator + 1);
                values.push(value.toLowerCase() === "null" ? null : value);
            }
            if (names.length === 0) {
                continue;
            }
            if (columns === null) {
                columns = names;
            }
            else if (!sameStrings(columns, names)) {
                throw new Error("COPY input changed row shape mid-stream");
            }
            rows.push(values);
        }
        if (columns === null) {
            throw new Error("COPY input contains no rows");
        }
        return buildNativeRowsetPayload(columns, rows);
    }
    const columns = splitCopyCsvLine(lines[0]).map((column) => column.trim());
    if (columns.length === 0 || columns.some((column) => column.length === 0)) {
        throw new Error("CSV COPY input requires a non-empty header row");
    }
    const rows = [];
    for (const line of lines.slice(1)) {
        const values = splitCopyCsvLine(line);
        if (values.length !== columns.length) {
            throw new Error("CSV COPY row shape mismatch");
        }
        rows.push(values.map((value) => value === "" || value.toLowerCase() === "null" ? null : value));
    }
    if (rows.length === 0) {
        throw new Error("CSV COPY input contains no data rows");
    }
    return buildNativeRowsetPayload(columns, rows);
}
function buildNativeRowsetPayload(columns, rows, columnTypes) {
    if (rows.length === 0) {
        throw new Error("native rowset requires at least one row");
    }
    if (columns.length === 0 || columns.some((column) => column.length === 0)) {
        throw new Error("native rowset requires non-empty column names");
    }
    for (const row of rows) {
        if (row.length !== columns.length) {
            throw new Error("native rowset row shape mismatch");
        }
    }
    const types = columnTypes ?? inferNativeRowsetColumnTypes(rows);
    if (types.length !== columns.length) {
        throw new Error("native rowset column/type shape mismatch");
    }
    const chunks = [];
    const header = Buffer.alloc(20 + types.length);
    header.write("SBNR", 0, "ascii");
    header.writeUInt16LE(2, 4);
    header.writeUInt16LE(0, 6);
    header.writeBigUInt64LE(BigInt(rows.length), 8);
    header.writeUInt32LE(columns.length, 16);
    for (let index = 0; index < types.length; index++) {
        header[20 + index] = types[index];
    }
    chunks.push(header);
    for (const column of columns) {
        const encoded = Buffer.from(column, "utf8");
        const length = Buffer.alloc(4);
        length.writeUInt32LE(encoded.length, 0);
        chunks.push(length, encoded);
    }
    const nullBitmapBytes = Math.ceil(columns.length / 8);
    for (const row of rows) {
        const nullBitmap = Buffer.alloc(nullBitmapBytes);
        const values = [];
        for (let index = 0; index < row.length; index++) {
            const value = row[index];
            if (value === null) {
                nullBitmap[Math.floor(index / 8)] |= 1 << (index % 8);
                continue;
            }
            values.push(encodeNativeRowsetValue(value, types[index]));
        }
        chunks.push(nullBitmap, ...values);
    }
    return Buffer.concat(chunks);
}
function encodeNativeRowsetValue(value, type) {
    switch (type) {
        case exports.NATIVE_ROWSET_TYPE_INT64: {
            const out = Buffer.alloc(8);
            out.writeBigInt64LE(BigInt(value.trim()), 0);
            return out;
        }
        case exports.NATIVE_ROWSET_TYPE_BOOLEAN:
            return Buffer.from([truthyNativeRowsetBoolean(value) ? 1 : 0]);
        case exports.NATIVE_ROWSET_TYPE_INT32: {
            const out = Buffer.alloc(4);
            out.writeInt32LE(Number.parseInt(value.trim(), 10), 0);
            return out;
        }
        case exports.NATIVE_ROWSET_TYPE_UINT64: {
            const out = Buffer.alloc(8);
            out.writeBigUInt64LE(BigInt(value.trim()), 0);
            return out;
        }
        case exports.NATIVE_ROWSET_TYPE_REAL64: {
            const out = Buffer.alloc(8);
            out.writeDoubleLE(Number(value.trim()), 0);
            return out;
        }
        case exports.NATIVE_ROWSET_TYPE_BINARY:
        case exports.NATIVE_ROWSET_TYPE_TEXT: {
            const encoded = Buffer.from(value, "utf8");
            const out = Buffer.alloc(4 + encoded.length);
            out.writeUInt32LE(encoded.length, 0);
            encoded.copy(out, 4);
            return out;
        }
        default:
            throw new Error(`unsupported native rowset type ${type}`);
    }
}
function inferNativeRowsetColumnTypes(rows) {
    if (rows.length === 0) {
        return [];
    }
    const columnCount = rows[0].length;
    const types = Array(columnCount).fill(exports.NATIVE_ROWSET_TYPE_TEXT);
    for (let column = 0; column < columnCount; column++) {
        const values = rows
            .map((row) => row[column])
            .filter((value) => value !== null);
        if (values.length === 0) {
            continue;
        }
        if (values.every((value) => {
            const normalized = value.trim().toLowerCase();
            return normalized === "true" || normalized === "false";
        })) {
            types[column] = exports.NATIVE_ROWSET_TYPE_BOOLEAN;
            continue;
        }
        if (values.every((value) => losslessInt(value, -2147483648n, 2147483647n))) {
            types[column] = exports.NATIVE_ROWSET_TYPE_INT32;
            continue;
        }
        if (values.every((value) => losslessInt(value, -(1n << 63n), (1n << 63n) - 1n))) {
            types[column] = exports.NATIVE_ROWSET_TYPE_INT64;
            continue;
        }
        if (values.every(losslessUint64)) {
            types[column] = exports.NATIVE_ROWSET_TYPE_UINT64;
            continue;
        }
        if (values.every(losslessReal64)) {
            types[column] = exports.NATIVE_ROWSET_TYPE_REAL64;
            continue;
        }
    }
    return types;
}
function splitCopyCsvLine(line) {
    const values = [];
    let current = "";
    let inQuote = false;
    for (let index = 0; index < line.length; index++) {
        const ch = line[index];
        if (ch === "\"") {
            if (inQuote && index + 1 < line.length && line[index + 1] === "\"") {
                current += "\"";
                index++;
            }
            else {
                inQuote = !inQuote;
            }
            continue;
        }
        if (ch === "," && !inQuote) {
            values.push(current);
            current = "";
            continue;
        }
        current += ch;
    }
    values.push(current);
    return values;
}
function sameStrings(left, right) {
    return left.length === right.length && left.every((value, index) => value === right[index]);
}
function losslessInt(value, min, max) {
    const text = value.trim();
    if (!/^-?\d+$/.test(text)) {
        return false;
    }
    try {
        const parsed = BigInt(text);
        return parsed >= min && parsed <= max && parsed.toString() === text;
    }
    catch {
        return false;
    }
}
function losslessUint64(value) {
    const text = value.trim();
    if (!/^\d+$/.test(text)) {
        return false;
    }
    try {
        const parsed = BigInt(text);
        return parsed <= (1n << 64n) - 1n && parsed.toString() === text;
    }
    catch {
        return false;
    }
}
function losslessReal64(value) {
    const parsed = Number(value.trim());
    return Number.isFinite(parsed);
}
function truthyNativeRowsetBoolean(value) {
    switch (value.trim().toLowerCase()) {
        case "1":
        case "true":
        case "t":
        case "yes":
        case "y":
        case "on":
            return true;
        default:
            return false;
    }
}
function isCopyStdinStatement(sql) {
    const executable = sql
        .split(/\n/)
        .map((line) => line.trim())
        .filter((line) => line.length > 0 && !line.startsWith("--"))
        .join(" ")
        .toLowerCase();
    return executable.startsWith("copy ") && executable.includes(" from stdin");
}
const METADATA_SNAPSHOT_COLLECTIONS = ["schemas", "tables", "columns", "indexes", "procedures", "functions"];
async function emitMetadataSnapshot(client, path, args) {
    const snapshots = {
        driver: "node",
        route: args.route,
        parser_mode: args.parserMode,
        page_size: args.pageSize,
        namespace: args.namespace,
        collections: {},
        tables_digest: null,
        row_count: null,
    };
    const collections = snapshots.collections;
    let tablesRows = null;
    let tablesRowCount = null;
    let attempts = 0;
    for (const collection of METADATA_SNAPSHOT_COLLECTIONS) {
        attempts++;
        const started = process.hrtime.bigint();
        try {
            const metadata = await client.queryMetadata(collection);
            const elapsedNs = Number(process.hrtime.bigint() - started);
            collections[collection] = {
                status: "ok",
                row_count: metadata.rowCount,
                elapsed_ns: elapsedNs,
                digest: sha256(jsonText(metadata.rows)),
            };
            if (collection === "tables") {
                tablesRows = metadata.rows;
                tablesRowCount = metadata.rowCount;
            }
        }
        catch (error) {
            const message = error instanceof Error ? error.message : String(error);
            collections[collection] = {
                status: "error",
                elapsed_ns: Number(process.hrtime.bigint() - started),
                error: message,
            };
        }
    }
    if (tablesRows !== null) {
        snapshots.tables_digest = sha256(jsonText(tablesRows));
        snapshots.row_count = tablesRowCount;
    }
    await writeText(path, jsonText(snapshots) + "\n");
    return attempts;
}
async function executeCopyIn(client, sql, payload, timeoutMs) {
    const protocol = client.protocol;
    const handleAsyncMessage = client.handleAsyncMessage.bind(client);
    const raiseProtocolError = client.raiseProtocolError?.bind(client);
    const drainReadyAfterError = client
        .drainReadyAfterError?.bind(client);
    const applyRuntimeReadyState = client
        .applyRuntimeReadyState?.bind(client);
    await protocol.sendMessage(protocol_1.MessageType.QUERY, (0, protocol_1.buildQueryPayload)(sql, protocol_1.QUERY_FLAG_BINARY_RESULT, 0, timeoutMs), 0, false);
    while (true) {
        const msg = await protocol.recv();
        if (handleAsyncMessage(msg)) {
            continue;
        }
        if (msg.header.type === protocol_1.MessageType.ERROR) {
            const error = raiseProtocolError ? raiseProtocolError(msg.payload) : null;
            if (drainReadyAfterError)
                await drainReadyAfterError();
            if (error)
                throw error;
            const parsed = (0, protocol_1.parseErrorMessage)(msg.payload);
            throw new Error(parsed.message || "query failed");
        }
        if (msg.header.type === protocol_1.MessageType.COPY_IN_RESPONSE) {
            break;
        }
        if (msg.header.type === protocol_1.MessageType.READY) {
            const ready = (0, protocol_1.parseReady)(msg.payload);
            if (applyRuntimeReadyState)
                applyRuntimeReadyState(ready.status, ready.txnId);
            else
                protocol.setTxnId(ready.txnId);
            throw new Error("expected COPY IN response");
        }
    }
    for (let offset = 0; offset < payload.length; offset += 65536) {
        await protocol.sendMessage(protocol_1.MessageType.COPY_DATA, payload.subarray(offset, offset + 65536), 0, false);
    }
    await protocol.sendMessage(protocol_1.MessageType.COPY_DONE, Buffer.alloc(0), 0, false);
    let rowsCopied = 0;
    while (true) {
        const msg = await protocol.recv();
        if (handleAsyncMessage(msg)) {
            continue;
        }
        if (msg.header.type === protocol_1.MessageType.COMMAND_COMPLETE) {
            rowsCopied = Number((0, protocol_1.parseCommandComplete)(msg.payload).rows);
            continue;
        }
        if (msg.header.type === protocol_1.MessageType.READY) {
            const ready = (0, protocol_1.parseReady)(msg.payload);
            if (applyRuntimeReadyState)
                applyRuntimeReadyState(ready.status, ready.txnId);
            else
                protocol.setTxnId(ready.txnId);
            return rowsCopied;
        }
        if (msg.header.type === protocol_1.MessageType.ERROR) {
            const error = raiseProtocolError ? raiseProtocolError(msg.payload) : null;
            if (drainReadyAfterError)
                await drainReadyAfterError();
            if (error)
                throw error;
            const parsed = (0, protocol_1.parseErrorMessage)(msg.payload);
            throw new Error(parsed.message || "query failed");
        }
        if (msg.header.type === protocol_1.MessageType.COPY_FAIL) {
            throw new Error("COPY failed on server side");
        }
    }
}
async function runTransaction(client, sql) {
    const tokens = controlTokens(sql);
    const first = tokens[0]?.toLowerCase() ?? "";
    if (first === "commit") {
        await client.commit();
    }
    else if (first === "rollback" &&
        tokens.length >= 4 &&
        tokens[1]?.toLowerCase() === "to" &&
        tokens[2]?.toLowerCase() === "savepoint") {
        await client.rollbackToSavepoint(normalizeControlName(tokens[3]));
    }
    else if (first === "rollback") {
        await client.rollback();
    }
    else if (first === "savepoint" && tokens.length >= 2) {
        await client.savepoint(normalizeControlName(tokens[1]));
    }
    else if (first === "release" &&
        tokens.length >= 3 &&
        tokens[1]?.toLowerCase() === "savepoint") {
        await client.releaseSavepoint(normalizeControlName(tokens[2]));
    }
    else if (first === "release" && tokens.length >= 2) {
        await client.releaseSavepoint(normalizeControlName(tokens[1]));
    }
    else {
        await client.begin();
    }
}
function controlTokens(sql) {
    return sql
        .split(/\r\n|\r|\n/)
        .map((line) => line.trim())
        .filter((line) => line.length > 0 && !line.startsWith("--"))
        .join(" ")
        .split(/\s+/)
        .filter((token) => token.length > 0);
}
function normalizeControlName(token) {
    return token.trim().replace(/;$/, "");
}
function routeEnvironment(args, actualPageSize, status, reason) {
    const record = {
        run_id: args.runId,
        driver: "node",
        route: args.route,
        sslmode: args.sslmode,
        parser_mode: args.parserMode,
        concurrency_mode: "single",
        namespace: args.namespace,
        page_size: args.pageSize,
        expected_page_size_bytes: PAGE_SIZE_BYTES[args.pageSize],
        actual_page_size_bytes: actualPageSize,
        page_size_verification_source: "SHOW DATABASE",
        page_size_verification_status: status,
        transport_mode: transportModeForRoute(args.route, args.sslmode),
        transport_endpoint_kind: endpointKindForRoute(args.route),
        driver_transport_implementation: transportImplementationForRoute(args.route),
    };
    if (reason) {
        record.failure_reason = reason;
    }
    return record;
}
async function probeRouteEnvironment(client, args) {
    try {
        const result = await client.query("SHOW DATABASE");
        const actual = pageSizeFromShowDatabase(result);
        const status = actual === PAGE_SIZE_BYTES[args.pageSize] ? "pass" : "fail";
        return routeEnvironment(args, actual, status, status === "pass" ? undefined : actual === null ? "show_database_missing_page_size_bytes" : "actual_page_size_mismatch");
    }
    catch (error) {
        return routeEnvironment(args, null, "fail", error instanceof Error ? error.message : String(error));
    }
}
function pageSizeFromShowDatabase(result) {
    const row = result.rows[0];
    if (!row)
        return null;
    const named = intValue(row.page_size_bytes);
    if (named !== null)
        return named;
    const fieldIndex = result.fields.findIndex((field) => field.name.toLowerCase() === "page_size_bytes");
    if (fieldIndex >= 0) {
        const value = intValue(row[result.fields[fieldIndex].name]);
        if (value !== null)
            return value;
    }
    if (result.fields.length >= 3) {
        const value = intValue(row[result.fields[2].name]);
        if (value !== null)
            return value;
    }
    const positional = Object.values(row);
    if (positional.length >= 3) {
        const value = intValue(positional[2]);
        if (value !== null)
            return value;
    }
    for (const value of positional) {
        const text = String(value ?? "").trim();
        const match = /page_size_bytes\s*[:=]\s*(\d+)/i.exec(text);
        if (match)
            return Number(match[1]);
    }
    return null;
}
function intValue(value) {
    if (typeof value === "number" && Number.isFinite(value))
        return value;
    if (typeof value === "bigint")
        return Number(value);
    if (typeof value === "string" && value.trim() !== "") {
        const parsed = Number(value);
        return Number.isFinite(parsed) ? parsed : null;
    }
    return null;
}
function parseArgs(raw) {
    const values = {};
    const flags = {};
    for (let i = 0; i < raw.length; i++) {
        const arg = raw[i];
        if (!arg.startsWith("--")) {
            throw new Error(`unexpected positional argument: ${arg}`);
        }
        if (!SUPPORTED_ARGS.has(arg)) {
            throw new Error(`unsupported argument: ${arg}`);
        }
        if (arg === "--stop-on-error" || arg === "--create-database" || arg === "--standard-english-fallback") {
            const next = raw[i + 1];
            if (next !== undefined && !next.startsWith("--")) {
                flags[arg] = parseBoolean(next);
                i++;
            }
            else {
                flags[arg] = true;
            }
            continue;
        }
        const value = raw[i + 1];
        if (value === undefined || value.startsWith("--")) {
            throw new Error(`missing value for ${arg}`);
        }
        values[arg] = value;
        i++;
    }
    return {
        database: required(values, "--database"),
        host: values["--host"] ?? "127.0.0.1",
        port: Number(values["--port"] ?? "3092"),
        user: required(values, "--user"),
        password: required(values, "--password"),
        role: values["--role"] ?? "",
        sslmode: values["--sslmode"] ?? "require",
        sslrootcert: values["--sslrootcert"] ?? "",
        sslcert: values["--sslcert"] ?? "",
        sslkey: values["--sslkey"] ?? "",
        ipcPath: values["--ipc-path"] ?? process.env.SCRATCHBIRD_IPC_PATH ?? "",
        managerAuthToken: values["--manager-auth-token"] ?? "",
        managerDatabase: values["--manager-database"] ?? "",
        route: values["--route"] ?? "listener-parser",
        parserMode: values["--parser-mode"] ?? "server-parser",
        pageSize: values["--page-size"] ?? "8k",
        namespace: required(values, "--namespace"),
        input: required(values, "--input"),
        output: required(values, "--output"),
        error: required(values, "--error"),
        diagnostics: required(values, "--diagnostics"),
        metrics: required(values, "--metrics"),
        transcript: required(values, "--transcript"),
        summary: required(values, "--summary"),
        stopOnError: flags["--stop-on-error"] ?? false,
        expectedRefusals: values["--expected-refusals"] ?? "",
        statementTimeoutMs: Number(values["--statement-timeout-ms"] ?? "30000"),
        fetchSize: Number(values["--fetch-size"] ?? "1000"),
        concurrencyWorker: Number(values["--concurrency-worker"] ?? "0"),
        runId: values["--run-id"] ?? "manual",
        createDatabase: flags["--create-database"] ?? false,
        languageResourcePack: values["--language-resource-pack"] ?? "project/resources/seed-packs/initial-resource-pack/resources/i18n/sbsql-language-resource-pack",
        languageResourceIdentity: values["--language-resource-identity"] ?? "sbsql.common_resource_pack.v1",
        languageResourceHash: values["--language-resource-hash"] ?? "sha256:752c7a9823bdad00b48ab318c8b2d5d6d53b2739ecfe43f565952fd510f4e3dc",
        languageProfile: values["--language-profile"] ?? "en-US",
        syntaxProfile: values["--syntax-profile"] ?? "sbsql.v3",
        topologyProfile: values["--topology-profile"] ?? "topology.sbsql.canonical.v1",
        standardEnglishFallback: flags["--standard-english-fallback"] ?? true,
        createEmulationMode: values["--create-emulation-mode"] ?? "sbsql",
    };
}
function parseBoolean(value) {
    switch (value.toLowerCase()) {
        case "1":
        case "true":
        case "yes":
        case "on":
            return true;
        case "0":
        case "false":
        case "no":
        case "off":
            return false;
        default:
            throw new Error(`invalid boolean value: ${value}`);
    }
}
async function loadExpectedRefusals(path) {
    const expected = new Set();
    if (!path) {
        return expected;
    }
    collectExpectedRefusalIds(expected, JSON.parse(await (0, promises_1.readFile)(path, "utf8")));
    return expected;
}
function collectExpectedRefusalIds(expected, value) {
    if (typeof value === "string") {
        expected.add(value);
        return;
    }
    if (Array.isArray(value)) {
        for (const item of value) {
            collectExpectedRefusalIds(expected, item);
        }
        return;
    }
    if (!value || typeof value !== "object") {
        return;
    }
    const record = value;
    for (const key of ["statement_id", "statementId", "id"]) {
        if (typeof record[key] === "string") {
            expected.add(record[key]);
        }
    }
    for (const key of [
        "compiled_chain_statement_aliases",
        "statement_ids",
        "statementIds",
        "expected_refusals",
        "expectedRefusals",
        "expected_diagnostics",
        "expectedDiagnostics",
    ]) {
        const nested = record[key];
        if (key === "compiled_chain_statement_aliases" && nested && typeof nested === "object") {
            for (const alias of Object.values(nested)) {
                if (typeof alias === "string") {
                    expected.add(alias);
                }
            }
            continue;
        }
        if ((key === "expected_diagnostics" || key === "expectedDiagnostics") &&
            nested && typeof nested === "object" && !Array.isArray(nested)) {
            for (const statementId of Object.keys(nested)) {
                expected.add(statementId);
            }
            continue;
        }
        collectExpectedRefusalIds(expected, nested);
    }
}
function frontDoorModeForRoute(route) {
    if (route === "manager-listener-parser")
        return "manager_proxy";
    return "direct";
}
function transportModeForRoute(route, sslmode) {
    if (route === "embedded")
        return "embedded_no_network_transport";
    if (route === "ipc_local")
        return "local_ipc_no_tls";
    return sslmode === "disable" ? "tls_disabled" : "tls_required";
}
function endpointKindForRoute(route) {
    if (route === "ipc_local")
        return "unix_domain_socket";
    if (route === "embedded")
        return "embedded_bridge";
    return "tcp";
}
function transportImplementationForRoute(route) {
    if (route === "embedded")
        return "unsupported_no_cpp_library_boundary";
    if (route === "ipc_local")
        return "native_node_unix_domain_socket";
    return "native_node_tcp";
}
function validate(args) {
    if (!PAGE_SIZES.has(args.pageSize))
        throw new Error(`unsupported page size: ${args.pageSize}`);
    if (!ROUTES.has(args.route))
        throw new Error(`unsupported route: ${args.route}`);
    if (args.route === "embedded") {
        throw new Error("embedded transport is unsupported by the Node driver; no ScratchBird C++ library boundary is exposed");
    }
    if (!PARSER_MODES.has(args.parserMode))
        throw new Error(`unsupported parser mode: ${args.parserMode}`);
    if (!Number.isInteger(args.port) || args.port <= 0)
        throw new Error(`invalid port: ${args.port}`);
}
function classifyStatement(sql) {
    const trimmed = sql
        .split(/\n/)
        .map((line) => line.trim())
        .filter((line) => line.length > 0 && !line.startsWith("--"))
        .join(" ")
        .toLowerCase();
    const first = trimmed.split(/\s+/, 1)[0] ?? "";
    if (first === "copy")
        return "copy";
    if (["create", "alter", "drop"].includes(first))
        return "ddl";
    if (["insert", "update", "delete", "merge", "upsert"].includes(first))
        return "dml";
    if (["commit", "rollback", "savepoint", "release", "begin", "start"].includes(first))
        return "transaction";
    if (["grant", "revoke"].includes(first))
        return "security_refusal";
    if (trimmed.includes("sys."))
        return "metadata";
    return "query";
}
async function readInput(path) {
    if (path === "-") {
        const chunks = [];
        for await (const chunk of process.stdin) {
            chunks.push(Buffer.from(chunk));
        }
        return Buffer.concat(chunks).toString("utf8");
    }
    return (0, promises_1.readFile)(path, "utf8");
}
function addTiming(timings, group, started) {
    timings[group] = (timings[group] ?? 0) + Number(process.hrtime.bigint() - started);
}
function sha256(text) {
    return `sha256:${(0, node_crypto_1.createHash)("sha256").update(text).digest("hex")}`;
}
async function writeText(path, text) {
    await (0, promises_1.mkdir)((0, node_path_1.dirname)(path), { recursive: true });
    await (0, promises_1.writeFile)(path, text, "utf8");
}
async function appendText(path, text) {
    await (0, promises_1.mkdir)((0, node_path_1.dirname)(path), { recursive: true });
    await (0, promises_1.appendFile)(path, text, "utf8");
}
async function appendJsonl(path, record) {
    await appendText(path, jsonText(record) + "\n");
}
function jsonText(value) {
    return JSON.stringify(value, (_key, item) => (typeof item === "bigint" ? item.toString() : item));
}
function required(values, key) {
    const value = values[key];
    if (!value) {
        throw new Error(`missing required argument ${key}`);
    }
    return value;
}
function junit(testcases, failures) {
    const rows = [
        '<?xml version="1.0" encoding="UTF-8"?>',
        `<testsuite name="SBIsqlNode" tests="${Math.max(testcases.length, 1)}" failures="${failures.length}">`,
    ];
    if (testcases.length === 0) {
        rows.push('  <testcase classname="scratchbird.node" name="run"></testcase>');
    }
    for (const testcase of testcases) {
        rows.push(`  <testcase classname="scratchbird.node" name="${escapeXml(String(testcase.statement_id))}"></testcase>`);
    }
    for (const failure of failures) {
        rows.push(`  <testcase classname="scratchbird.node" name="${escapeXml(String(failure.statement_id))}">`
            + `<failure message="${escapeXml(String(failure.message))}" /></testcase>`);
    }
    rows.push("</testsuite>");
    return rows.join("\n") + "\n";
}
function escapeXml(text) {
    return text
        .replace(/&/g, "&amp;")
        .replace(/"/g, "&quot;")
        .replace(/</g, "&lt;")
        .replace(/>/g, "&gt;");
}
if (require.main === module) {
    void main().catch((error) => {
        console.error(error instanceof Error ? error.message : String(error));
        process.exitCode = 1;
    });
}
