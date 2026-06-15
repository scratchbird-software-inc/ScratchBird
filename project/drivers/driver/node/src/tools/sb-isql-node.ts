// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// Native Node.js conformance shell and canonical driver usage example.

import { createHash } from "node:crypto";
import { mkdir, readFile, writeFile, appendFile } from "node:fs/promises";
import { dirname, basename } from "node:path";
import { Client as ScratchBirdClient, type ClientConfig, type QueryResult } from "../index";
import { splitTopLevelStatements } from "../sql";

type JsonRecord = Record<string, unknown>;

interface Args {
  database: string;
  host: string;
  port: number;
  user: string;
  password: string;
  role: string;
  sslmode: string;
  route: string;
  parserMode: string;
  pageSize: string;
  namespace: string;
  input: string;
  output: string;
  error: string;
  diagnostics: string;
  metrics: string;
  transcript: string;
  summary: string;
  stopOnError: boolean;
  expectedRefusals: string;
  statementTimeoutMs: number;
  fetchSize: number;
  concurrencyWorker: number;
  runId: string;
  createDatabase: boolean;
  createEmulationMode: string;
}

const PAGE_SIZES = new Set(["4k", "8k", "16k", "32k", "64k", "128k"]);
const ROUTES = new Set(["embedded", "ipc_local", "listener-parser", "manager-listener-parser"]);
const PARSER_MODES = new Set(["server-parser", "standalone-parser", "driver-sblr-uuid"]);

async function main(): Promise<void> {
  const args = parseArgs(process.argv.slice(2));
  const status = await run(args);
  process.exitCode = status;
}

async function run(args: Args): Promise<number> {
  validate(args);
  const runRoot = dirname(args.summary);
  await mkdir(runRoot, { recursive: true });
  const paths = {
    events: `${runRoot}/command-events.jsonl`,
    wire: `${runRoot}/wire-transcript.jsonl`,
    timing: `${runRoot}/timing-groups.json`,
    digests: `${runRoot}/result-digests.json`,
    metadata: `${runRoot}/metadata-snapshots.json`,
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

  const timings: Record<string, number> = {};
  const apiHits: Record<string, number> = {
    ScratchBirdClient: 0,
    connect: 0,
    prepare: 0,
    execute: 0,
    query: 0,
    transaction: 0,
    queryMetadata: 0,
  };
  const testcases: JsonRecord[] = [];
  const failures: JsonRecord[] = [];
  const digests: JsonRecord[] = [];
  const securityRefusals: JsonRecord[] = [];
  const started = process.hrtime.bigint();
  let client: ScratchBirdClient | undefined;

  try {
    const config: ClientConfig = {
      host: args.host,
      port: args.port,
      database: args.database,
      user: args.user,
      password: args.password,
      role: args.role || undefined,
      sslmode: args.sslmode,
      binaryTransfer: true,
      applicationName: "SBIsqlNode",
      frontDoorMode: args.route === "manager-listener-parser" ? "manager_proxy" : "direct",
      metadataExpandSchemaParents: true,
    };
    client = new ScratchBirdClient(config);
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
      throw new Error("--create-database is not implemented in the Node native tool yet");
    }
    if (args.parserMode !== "server-parser") {
      throw new Error(`${args.parserMode} is not yet implemented by the Node native tool; it fails closed`);
    }

    const script = await readInput(args.input);
    const statements = splitTopLevelStatements(script);
    for (let i = 0; i < statements.length; i++) {
      const sql = statements[i];
      const statementId = `${basename(args.input)}:${i + 1}`;
      const group = classifyStatement(sql);
      const statementStarted = process.hrtime.bigint();
      let outcome = "success";
      let rowCount = -1;
      let resultDigest: string | null = null;
      let diagnosticCode: string | null = null;
      let sqlState: string | null = null;
      try {
        if (group === "transaction") {
          await runTransaction(client, sql);
          apiHits.transaction++;
          rowCount = 0;
          resultDigest = sha256("transaction");
        } else {
          const name = `sb_isql_node_${i + 1}`;
          await client.prepare(name, sql);
          apiHits.prepare++;
          const result: QueryResult = await client.execute(name);
          apiHits.execute++;
          apiHits.query++;
          rowCount = result.rowCount;
          resultDigest = sha256(JSON.stringify(result.rows));
          await appendText(args.output, JSON.stringify({ statement_id: statementId, rows: result.rows }) + "\n");
        }
      } catch (error) {
        outcome = "refusal";
        diagnosticCode = error instanceof Error ? error.message : String(error);
        sqlState = "HY000";
        await appendJsonl(args.diagnostics, {
          statement_id: statementId,
          sqlstate: sqlState,
          message: diagnosticCode,
        });
        await appendText(args.error, `${statementId}: ${diagnosticCode}\n`);
        failures.push({ statement_id: statementId, message: diagnosticCode });
        if (args.stopOnError) {
          addTiming(timings, group, statementStarted);
          break;
        }
      }
      addTiming(timings, group, statementStarted);
      const event: JsonRecord = {
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
        expected_outcome: "success",
        actual_outcome: outcome,
        sqlstate: sqlState,
        diagnostic_code: diagnosticCode,
        canonical_message_vector: [],
        row_count: rowCount,
        result_digest: resultDigest,
        elapsed_ns: Number(process.hrtime.bigint() - statementStarted),
        server_revalidation_state: "required",
        transaction_id_observed: null,
        mga_authority: "engine",
        native_api_surface: "node",
        code_example_section: "prepared_execute_fetch",
      };
      await appendJsonl(paths.events, event);
      testcases.push(event);
      digests.push({ statement_id: statementId, row_count: rowCount, result_digest: resultDigest });
    }

    const metadataStarted = process.hrtime.bigint();
    const metadata = await client.queryMetadata("tables");
    apiHits.queryMetadata++;
    addTiming(timings, "metadata", metadataStarted);
    await writeText(paths.metadata, JSON.stringify({
      tables_digest: sha256(JSON.stringify(metadata.rows)),
      row_count: metadata.rowCount,
    }) + "\n");
  } catch (error) {
    const message = error instanceof Error ? error.message : String(error);
    failures.push({ statement_id: "run", message });
    await appendText(paths.stderrLog, `${message}\n`);
  } finally {
    if (client) {
      await client.end();
    }
  }

  const elapsed = Number(process.hrtime.bigint() - started);
  timings.overall = elapsed;
  const summary = {
    run_id: args.runId,
    driver_name: "node",
    route: args.route,
    parser_mode: args.parserMode,
    page_size: args.pageSize,
    namespace: args.namespace,
    status: failures.length === 0 ? "pass" : "fail",
    failure_count: failures.length,
    elapsed_ns: elapsed,
    server_revalidation_required: true,
    driver_or_parser_finality: "forbidden",
    mga_authority: "engine",
  };
  await writeText(args.summary, JSON.stringify(summary) + "\n");
  await writeText(args.metrics, JSON.stringify(timings) + "\n");
  await writeText(paths.timing, JSON.stringify(timings) + "\n");
  await writeText(paths.digests, JSON.stringify(digests) + "\n");
  await writeText(paths.refusals, JSON.stringify(securityRefusals) + "\n");
  await writeText(paths.api, JSON.stringify(apiHits) + "\n");
  await writeText(paths.review, JSON.stringify({
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

async function runTransaction(client: ScratchBirdClient, sql: string): Promise<void> {
  const first = sql.trim().split(/\s+/, 1)[0]?.toLowerCase();
  if (first === "commit") {
    await client.commit();
  } else if (first === "rollback") {
    await client.rollback();
  } else {
    await client.begin();
  }
}

function parseArgs(raw: string[]): Args {
  const values: Record<string, string> = {};
  const flags = new Set<string>();
  for (let i = 0; i < raw.length; i++) {
    const arg = raw[i];
    if (!arg.startsWith("--")) {
      throw new Error(`unexpected positional argument: ${arg}`);
    }
    if (arg === "--stop-on-error" || arg === "--create-database") {
      flags.add(arg);
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
    stopOnError: flags.has("--stop-on-error"),
    expectedRefusals: values["--expected-refusals"] ?? "",
    statementTimeoutMs: Number(values["--statement-timeout-ms"] ?? "30000"),
    fetchSize: Number(values["--fetch-size"] ?? "1000"),
    concurrencyWorker: Number(values["--concurrency-worker"] ?? "0"),
    runId: values["--run-id"] ?? "manual",
    createDatabase: flags.has("--create-database"),
    createEmulationMode: values["--create-emulation-mode"] ?? "sbsql",
  };
}

function validate(args: Args): void {
  if (!PAGE_SIZES.has(args.pageSize)) throw new Error(`unsupported page size: ${args.pageSize}`);
  if (!ROUTES.has(args.route)) throw new Error(`unsupported route: ${args.route}`);
  if (!PARSER_MODES.has(args.parserMode)) throw new Error(`unsupported parser mode: ${args.parserMode}`);
  if (!Number.isInteger(args.port) || args.port <= 0) throw new Error(`invalid port: ${args.port}`);
}

function classifyStatement(sql: string): string {
  const trimmed = sql.trim().toLowerCase();
  const first = trimmed.split(/\s+/, 1)[0] ?? "";
  if (["create", "alter", "drop"].includes(first)) return "ddl";
  if (["insert", "update", "delete", "merge", "upsert"].includes(first)) return "dml";
  if (["commit", "rollback", "savepoint", "begin", "start"].includes(first)) return "transaction";
  if (["grant", "revoke"].includes(first)) return "security_refusal";
  if (trimmed.includes("sys.")) return "metadata";
  return "query";
}

async function readInput(path: string): Promise<string> {
  if (path === "-") {
    const chunks: Buffer[] = [];
    for await (const chunk of process.stdin) {
      chunks.push(Buffer.from(chunk));
    }
    return Buffer.concat(chunks).toString("utf8");
  }
  return readFile(path, "utf8");
}

function addTiming(timings: Record<string, number>, group: string, started: bigint): void {
  timings[group] = (timings[group] ?? 0) + Number(process.hrtime.bigint() - started);
}

function sha256(text: string): string {
  return `sha256:${createHash("sha256").update(text).digest("hex")}`;
}

async function writeText(path: string, text: string): Promise<void> {
  await mkdir(dirname(path), { recursive: true });
  await writeFile(path, text, "utf8");
}

async function appendText(path: string, text: string): Promise<void> {
  await mkdir(dirname(path), { recursive: true });
  await appendFile(path, text, "utf8");
}

async function appendJsonl(path: string, record: JsonRecord): Promise<void> {
  await appendText(path, JSON.stringify(record) + "\n");
}

function required(values: Record<string, string>, key: string): string {
  const value = values[key];
  if (!value) {
    throw new Error(`missing required argument ${key}`);
  }
  return value;
}

function junit(testcases: JsonRecord[], failures: JsonRecord[]): string {
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
    rows.push(
      `  <testcase classname="scratchbird.node" name="${escapeXml(String(failure.statement_id))}">`
      + `<failure message="${escapeXml(String(failure.message))}" /></testcase>`,
    );
  }
  rows.push("</testsuite>");
  return rows.join("\n") + "\n";
}

function escapeXml(text: string): string {
  return text
    .replace(/&/g, "&amp;")
    .replace(/"/g, "&quot;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;");
}

void main().catch((error: unknown) => {
  console.error(error instanceof Error ? error.message : String(error));
  process.exitCode = 1;
});
