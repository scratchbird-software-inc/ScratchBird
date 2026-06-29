// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

use std::collections::{BTreeMap, BTreeSet};
use std::env;
use std::fs;
use std::io::{self, Read};
use std::path::{Path, PathBuf};
use std::time::Instant;

use scratchbird::sql::Params;
use scratchbird::{Client, Config, Value};
use serde_json::{json, Value as JsonValue};
use sha2::{Digest, Sha256};

const SUPPORTED_ARGS: &[&str] = &[
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
];

#[derive(Debug)]
struct Args {
    database: String,
    host: String,
    port: u16,
    user: String,
    password: String,
    role: String,
    sslmode: String,
    sslrootcert: String,
    sslcert: String,
    sslkey: String,
    ipc_path: String,
    route: String,
    parser_mode: String,
    page_size: String,
    namespace: String,
    input: String,
    output: PathBuf,
    error: PathBuf,
    diagnostics: PathBuf,
    metrics: PathBuf,
    transcript: PathBuf,
    summary: PathBuf,
    stop_on_error: bool,
    expected_refusals: String,
    statement_timeout_ms: u64,
    fetch_size: u32,
    concurrency_worker: u32,
    run_id: String,
    create_database: bool,
    create_emulation_mode: String,
    language_resource_pack: String,
    language_resource_identity: String,
    language_resource_hash: String,
    language_profile: String,
    syntax_profile: String,
    topology_profile: String,
    standard_english_fallback: bool,
}

#[tokio::main]
async fn main() {
    let args = match parse_args(env::args().skip(1).collect()) {
        Ok(args) => args,
        Err(err) => {
            eprintln!("{err}");
            std::process::exit(2);
        }
    };
    match run(args).await {
        Ok(code) => std::process::exit(code),
        Err(err) => {
            eprintln!("{err}");
            std::process::exit(1);
        }
    }
}

async fn run(args: Args) -> Result<i32, Box<dyn std::error::Error>> {
    validate(&args)?;
    let run_root = args.summary.parent().unwrap_or_else(|| Path::new("."));
    fs::create_dir_all(run_root)?;
    let paths = ArtifactPaths::new(run_root);
    for path in [
        &args.output,
        &args.error,
        &args.diagnostics,
        &args.metrics,
        &args.transcript,
        &args.summary,
        &paths.events,
        &paths.wire,
        &paths.timing,
        &paths.digests,
        &paths.metadata,
        &paths.route_env,
        &paths.refusals,
        &paths.api,
        &paths.review,
        &paths.junit,
        &paths.stdout_log,
        &paths.stderr_log,
    ] {
        write_text(path, "")?;
    }

    let started = Instant::now();
    let mut route_env = route_environment(&args, None, "fail", Some("not_probed"));
    write_text(&paths.route_env, &format!("{}\n", route_env))?;
    let mut timings = BTreeMap::<String, u128>::new();
    let mut api_hits = BTreeMap::<String, u64>::from([
        ("Client".to_string(), 0),
        ("connect".to_string(), 0),
        ("query".to_string(), 0),
        ("query_params".to_string(), 0),
        ("begin".to_string(), 0),
        ("commit".to_string(), 0),
        ("rollback".to_string(), 0),
        ("query_metadata".to_string(), 0),
    ]);
    let mut testcases = Vec::<JsonValue>::new();
    let mut failures = Vec::<JsonValue>::new();
    let mut digests = Vec::<JsonValue>::new();
    let mut security_refusals = Vec::<JsonValue>::new();

    let mut config = Config::default();
    config.host = args.host.clone();
    config.port = args.port;
    config.database = args.database.clone();
    config.user = args.user.clone();
    config.password = args.password.clone();
    config.role = args.role.clone();
    config.sslmode = args.sslmode.clone();
    if args.route == "ipc_local" {
        config.transport_mode = "local_ipc".to_string();
        config.ipc_method = "unix".to_string();
        config.ipc_path = args.ipc_path.clone();
        config.sslmode = "disable".to_string();
    }
    config.sslrootcert = if args.sslrootcert.is_empty() {
        None
    } else {
        Some(args.sslrootcert.clone())
    };
    config.sslcert = if args.sslcert.is_empty() {
        None
    } else {
        Some(args.sslcert.clone())
    };
    config.sslkey = if args.sslkey.is_empty() {
        None
    } else {
        Some(args.sslkey.clone())
    };
    config.front_door_mode = if args.route == "manager-listener-parser" {
        "manager_proxy".to_string()
    } else {
        "direct".to_string()
    };
    config.application_name = "SBIsqlRust".to_string();
    config.metadata_expand_schema_parents = true;
    config.fetch_size = args.fetch_size;

    let mut client = Client::new(config);
    *api_hits.entry("Client".to_string()).or_default() += 1;
    let connect_started = Instant::now();
    match client.connect().await {
        Ok(()) => {
            *api_hits.entry("connect".to_string()).or_default() += 1;
            add_timing(&mut timings, "connection", connect_started);
            append_jsonl(
                &args.transcript,
                json!({
                    "event": "connect",
                    "driver": "rust",
                    "route": args.route,
                    "parser_mode": args.parser_mode,
                    "page_size": args.page_size
                }),
            )?;
            append_jsonl(
                &paths.wire,
                json!({"event": "server_admission_required", "driver_or_parser_finality": "forbidden"}),
            )?;
        }
        Err(err) => {
            failures.push(json!({"statement_id": "connect", "message": err.to_string()}));
        }
    }

    if failures.is_empty() && args.create_database {
        let create_started = Instant::now();
        match client
            .attach_create(&args.create_emulation_mode, &args.database)
            .await
        {
            Ok(()) => {
                *api_hits.entry("attach_create".to_string()).or_default() += 1;
                add_timing(&mut timings, "database_create", create_started);
            }
            Err(err) => failures.push(json!({
                "statement_id": "database_create",
                "message": err.to_string()
            })),
        }
    }
    if failures.is_empty() {
        route_env = probe_route_environment(&mut client, &args).await;
        write_text(&paths.route_env, &format!("{}\n", route_env))?;
        if args.route != "embedded"
            && route_env["page_size_verification_status"].as_str() != Some("pass")
        {
            failures.push(json!({
                "statement_id": "route_page_size",
                "message": "route page-size verification failed",
                "expected_page_size_bytes": route_env["expected_page_size_bytes"],
                "actual_page_size_bytes": route_env["actual_page_size_bytes"]
            }));
        }
    }
    if failures.is_empty() && args.parser_mode != "server-parser" {
        failures.push(json!({
            "statement_id": "parser_mode",
            "message": format!("{} is not yet implemented by the Rust native tool; it fails closed", args.parser_mode)
        }));
    }

    if failures.is_empty() {
        let expected_refusals = load_expected_refusals(&args.expected_refusals)?;
        let script = read_input(&args.input)?;
        for (index, statement) in split_statements(&script).iter().enumerate() {
            let statement_id = format!(
                "{}:{}",
                Path::new(&args.input)
                    .file_name()
                    .and_then(|value| value.to_str())
                    .unwrap_or("stdin"),
                index + 1
            );
            let group = classify(statement);
            let statement_started = Instant::now();
            let expected_outcome = if expected_refusals.contains(&statement_id) {
                "refusal"
            } else {
                "success"
            };
            let mut outcome = "success".to_string();
            let mut row_count = -1_i64;
            let mut result_digest = JsonValue::Null;
            let mut sqlstate = JsonValue::Null;
            let mut diagnostic = JsonValue::Null;
            let mut break_after_event = false;
            let result = if group == "transaction" {
                run_transaction(&mut client, statement, &mut api_hits)
                    .await
                    .map(|_| scratchbird::QueryResult {
                        columns: Vec::new(),
                        rows: Vec::new(),
                        row_count: 0,
                        command_tag: "TRANSACTION".to_string(),
                    })
            } else {
                *api_hits.entry("query_params".to_string()).or_default() += 1;
                client
                    .query_params(statement, Params::Positional(Vec::new()))
                    .await
            };
            match result {
                Ok(result) => {
                    *api_hits.entry("query".to_string()).or_default() += 1;
                    row_count = result.row_count;
                    let rows_debug = format!("{:?}", result.rows);
                    result_digest = json!(sha256(&rows_debug));
                    append_text(
                        &args.output,
                        &format!(
                            "{}\n",
                            json!({"statement_id": statement_id, "rows_debug": rows_debug})
                        ),
                    )?;
                    digests.push(json!({
                        "statement_id": statement_id,
                        "row_count": row_count,
                        "result_digest": result_digest
                    }));
                    if expected_outcome == "refusal" {
                        outcome = "unexpected_success".to_string();
                        failures.push(json!({
                            "statement_id": statement_id,
                            "message": "statement succeeded but was expected to refuse"
                        }));
                        break_after_event = args.stop_on_error;
                    }
                }
                Err(err) => {
                    outcome = "refusal".to_string();
                    sqlstate = json!(err.sqlstate.clone().unwrap_or_else(|| "HY000".to_string()));
                    diagnostic = json!(err.to_string());
                    append_jsonl(
                        &args.diagnostics,
                        json!({"statement_id": statement_id, "sqlstate": sqlstate, "message": diagnostic}),
                    )?;
                    append_text(&args.error, &format!("{}: {}\n", statement_id, err))?;
                    if expected_outcome == "refusal" {
                        security_refusals.push(json!({
                            "statement_id": statement_id,
                            "sqlstate": sqlstate,
                            "diagnostic_code": diagnostic
                        }));
                    } else {
                        failures.push(
                            json!({"statement_id": statement_id, "message": err.to_string()}),
                        );
                        break_after_event = args.stop_on_error;
                    }
                }
            }
            let elapsed_ns = statement_started.elapsed().as_nanos();
            add_timing(&mut timings, &group, statement_started);
            let event = json!({
                "run_id": args.run_id,
                "driver_name": "rust",
                "driver_version": "unknown",
                "route": args.route,
                "parser_mode": args.parser_mode,
                "page_size": args.page_size,
                "namespace": args.namespace,
                "script": args.input,
                "statement_index": index + 1,
                "statement_id": statement_id,
                "command_group": group,
                "sql_hash": sha256(statement),
                "expected_outcome": expected_outcome,
                "actual_outcome": outcome,
                "sqlstate": sqlstate,
                "diagnostic_code": diagnostic,
                "canonical_message_vector": [],
                "row_count": row_count,
                "result_digest": result_digest,
                "elapsed_ns": elapsed_ns,
                "server_revalidation_state": "required",
                "language_profile": args.language_profile,
                "language_resource_pack": args.language_resource_pack,
                "language_resource_identity": args.language_resource_identity,
                "language_resource_hash": args.language_resource_hash,
                "syntax_profile": args.syntax_profile,
                "topology_profile": args.topology_profile,
                "standard_english_fallback": args.standard_english_fallback,
                "transaction_id_observed": null,
                "mga_authority": "engine",
                "native_api_surface": "rust",
                "code_example_section": "query_params_fetch"
            });
            append_jsonl(&paths.events, event.clone())?;
            testcases.push(event);
            if break_after_event {
                break;
            }
        }
        let metadata_started = Instant::now();
        match client.query_metadata("tables").await {
            Ok(metadata) => {
                *api_hits.entry("query_metadata".to_string()).or_default() += 1;
                add_timing(&mut timings, "metadata", metadata_started);
                write_text(
                    &paths.metadata,
                    &format!(
                        "{}\n",
                        json!({
                            "tables_digest": sha256(&format!("{:?}", metadata.rows)),
                            "row_count": metadata.row_count
                        })
                    ),
                )?;
            }
            Err(err) => {
                failures.push(json!({"statement_id": "metadata", "message": err.to_string()}))
            }
        }
    }
    client.close().await;

    timings.insert("overall".to_string(), started.elapsed().as_nanos());
    let transport_mode = transport_mode_for_route(&args.route, &args.sslmode);
    let process_metrics = current_process_metrics();
    let summary = json!({
        "run_id": args.run_id,
        "driver_name": "rust",
        "route": args.route,
        "parser_mode": args.parser_mode,
        "page_size": args.page_size,
        "namespace": args.namespace,
        "sslmode": args.sslmode,
        "transport_mode": transport_mode,
        "transport_endpoint_kind": endpoint_kind_for_route(&args.route),
        "driver_transport_implementation": transport_implementation_for_route(&args.route),
        "cpp_library_boundary": "none",
        "language_resource_pack": args.language_resource_pack,
        "language_resource_identity": args.language_resource_identity,
        "language_resource_hash": args.language_resource_hash,
        "language_resource_authority": "shared_server_parser_resource_pack",
        "language_profile": args.language_profile,
        "syntax_profile": args.syntax_profile,
        "topology_profile": args.topology_profile,
        "standard_english_fallback": args.standard_english_fallback,
        "status": if failures.is_empty() { "pass" } else { "fail" },
        "failure_count": failures.len(),
        "elapsed_ns": started.elapsed().as_nanos(),
        "process_metrics": process_metrics.clone(),
        "server_revalidation_required": true,
        "driver_or_parser_finality": "forbidden",
        "mga_authority": "engine"
    });
    write_text(&args.summary, &format!("{summary}\n"))?;
    write_text(
        &args.metrics,
        &format!(
            "{}\n",
            serde_json::to_string(&json!({
                "role": "client",
                "rss_kb": process_metrics["client"]["last_rss_kb"],
                "vsize_kb": process_metrics["client"]["last_vsize_kb"]
            }))?
        ),
    )?;
    write_text(
        &paths.timing,
        &format!("{}\n", serde_json::to_string(&timings)?),
    )?;
    write_text(
        &paths.digests,
        &format!("{}\n", serde_json::to_string(&digests)?),
    )?;
    write_text(
        &paths.refusals,
        &format!("{}\n", serde_json::to_string(&security_refusals)?),
    )?;
    write_text(
        &paths.api,
        &format!("{}\n", serde_json::to_string(&api_hits)?),
    )?;
    write_text(
        &paths.review,
        &format!(
            "{}\n",
            json!({
                "driver": "rust",
                "public_api_only": true,
                "shells_out_to_other_driver": false,
                "source_is_canonical_example": true,
                "sections": ["connection", "query_params", "fetch", "metadata", "diagnostics", "transaction"]
            })
        ),
    )?;
    write_text(&paths.junit, &junit(&testcases, &failures))?;
    append_text(
        &paths.stdout_log,
        &format!(
            "SBIsqlRust status={}\n",
            summary["status"].as_str().unwrap_or("fail")
        ),
    )?;
    if !failures.is_empty() {
        append_text(
            &paths.stderr_log,
            &format!("{}\n", serde_json::to_string(&failures)?),
        )?;
    }
    Ok(if failures.is_empty() { 0 } else { 1 })
}

async fn run_transaction(
    client: &mut Client,
    sql: &str,
    api_hits: &mut BTreeMap<String, u64>,
) -> scratchbird::Result<()> {
    let first = sql
        .trim()
        .split_whitespace()
        .next()
        .unwrap_or("")
        .to_ascii_lowercase();
    match first.as_str() {
        "commit" => {
            *api_hits.entry("commit".to_string()).or_default() += 1;
            client.commit(None).await
        }
        "rollback" => {
            *api_hits.entry("rollback".to_string()).or_default() += 1;
            client.rollback(None).await
        }
        _ => {
            *api_hits.entry("begin".to_string()).or_default() += 1;
            client.begin(None).await
        }
    }
}

struct ArtifactPaths {
    events: PathBuf,
    wire: PathBuf,
    timing: PathBuf,
    digests: PathBuf,
    metadata: PathBuf,
    route_env: PathBuf,
    refusals: PathBuf,
    api: PathBuf,
    review: PathBuf,
    junit: PathBuf,
    stdout_log: PathBuf,
    stderr_log: PathBuf,
}

impl ArtifactPaths {
    fn new(root: &Path) -> Self {
        Self {
            events: root.join("command-events.jsonl"),
            wire: root.join("wire-transcript.jsonl"),
            timing: root.join("timing-groups.json"),
            digests: root.join("result-digests.json"),
            metadata: root.join("metadata-snapshots.json"),
            route_env: root.join("route-environment.json"),
            refusals: root.join("security-refusals.json"),
            api: root.join("native-api-coverage.json"),
            review: root.join("code-example-review.json"),
            junit: root.join("junit.xml"),
            stdout_log: root.join("stdout.log"),
            stderr_log: root.join("stderr.log"),
        }
    }
}

fn parse_args(raw: Vec<String>) -> Result<Args, String> {
    let mut values = BTreeMap::<String, String>::new();
    let mut flags = BTreeMap::<String, bool>::new();
    let mut index = 0;
    while index < raw.len() {
        let key = &raw[index];
        if !key.starts_with("--") {
            return Err(format!("unexpected positional argument: {key}"));
        }
        if !SUPPORTED_ARGS.contains(&key.as_str()) {
            return Err(format!("unsupported argument: {key}"));
        }
        if key == "--stop-on-error"
            || key == "--create-database"
            || key == "--standard-english-fallback"
        {
            if let Some(value) = raw.get(index + 1) {
                if !value.starts_with("--") {
                    flags.insert(key.clone(), parse_bool(value)?);
                    index += 2;
                    continue;
                }
            }
            flags.insert(key.clone(), true);
            index += 1;
            continue;
        }
        let value = raw
            .get(index + 1)
            .ok_or_else(|| format!("missing value for {key}"))?;
        if value.starts_with("--") {
            return Err(format!("missing value for {key}"));
        }
        values.insert(key.clone(), value.clone());
        index += 2;
    }
    Ok(Args {
        database: required(&values, "--database")?,
        host: values
            .get("--host")
            .cloned()
            .unwrap_or_else(|| "127.0.0.1".to_string()),
        port: values
            .get("--port")
            .and_then(|value| value.parse().ok())
            .unwrap_or(3092),
        user: required(&values, "--user")?,
        password: required(&values, "--password")?,
        role: values.get("--role").cloned().unwrap_or_default(),
        sslmode: values
            .get("--sslmode")
            .cloned()
            .unwrap_or_else(|| "require".to_string()),
        sslrootcert: values.get("--sslrootcert").cloned().unwrap_or_default(),
        sslcert: values.get("--sslcert").cloned().unwrap_or_default(),
        sslkey: values.get("--sslkey").cloned().unwrap_or_default(),
        ipc_path: values
            .get("--ipc-path")
            .cloned()
            .or_else(|| std::env::var("SCRATCHBIRD_IPC_PATH").ok())
            .unwrap_or_default(),
        route: values
            .get("--route")
            .cloned()
            .unwrap_or_else(|| "listener-parser".to_string()),
        parser_mode: values
            .get("--parser-mode")
            .cloned()
            .unwrap_or_else(|| "server-parser".to_string()),
        page_size: values
            .get("--page-size")
            .cloned()
            .unwrap_or_else(|| "8k".to_string()),
        namespace: required(&values, "--namespace")?,
        input: required(&values, "--input")?,
        output: PathBuf::from(required(&values, "--output")?),
        error: PathBuf::from(required(&values, "--error")?),
        diagnostics: PathBuf::from(required(&values, "--diagnostics")?),
        metrics: PathBuf::from(required(&values, "--metrics")?),
        transcript: PathBuf::from(required(&values, "--transcript")?),
        summary: PathBuf::from(required(&values, "--summary")?),
        stop_on_error: flags.get("--stop-on-error").copied().unwrap_or(false),
        expected_refusals: values
            .get("--expected-refusals")
            .cloned()
            .unwrap_or_default(),
        statement_timeout_ms: values
            .get("--statement-timeout-ms")
            .and_then(|value| value.parse().ok())
            .unwrap_or(30_000),
        fetch_size: values
            .get("--fetch-size")
            .and_then(|value| value.parse().ok())
            .unwrap_or(1000),
        concurrency_worker: values
            .get("--concurrency-worker")
            .and_then(|value| value.parse().ok())
            .unwrap_or(0),
        run_id: values
            .get("--run-id")
            .cloned()
            .unwrap_or_else(|| "manual".to_string()),
        create_database: flags.get("--create-database").copied().unwrap_or(false),
        create_emulation_mode: values
            .get("--create-emulation-mode")
            .cloned()
            .unwrap_or_else(|| "sbsql".to_string()),
        language_resource_pack: values
            .get("--language-resource-pack")
            .cloned()
            .unwrap_or_else(|| "project/resources/seed-packs/initial-resource-pack/resources/i18n/sbsql-language-resource-pack".to_string()),
        language_resource_identity: values
            .get("--language-resource-identity")
            .cloned()
            .unwrap_or_else(|| "sbsql.common_resource_pack.v1".to_string()),
        language_resource_hash: values
            .get("--language-resource-hash")
            .cloned()
            .unwrap_or_else(|| "sha256:752c7a9823bdad00b48ab318c8b2d5d6d53b2739ecfe43f565952fd510f4e3dc".to_string()),
        language_profile: values
            .get("--language-profile")
            .cloned()
            .unwrap_or_else(|| "en-US".to_string()),
        syntax_profile: values
            .get("--syntax-profile")
            .cloned()
            .unwrap_or_else(|| "sbsql.v3".to_string()),
        topology_profile: values
            .get("--topology-profile")
            .cloned()
            .unwrap_or_else(|| "topology.sbsql.canonical.v1".to_string()),
        standard_english_fallback: flags
            .get("--standard-english-fallback")
            .copied()
            .unwrap_or(true),
    })
}

fn parse_bool(value: &str) -> Result<bool, String> {
    match value.to_ascii_lowercase().as_str() {
        "1" | "true" | "yes" | "on" => Ok(true),
        "0" | "false" | "no" | "off" => Ok(false),
        _ => Err(format!("invalid boolean value: {value}")),
    }
}

fn required(values: &BTreeMap<String, String>, key: &str) -> Result<String, String> {
    values
        .get(key)
        .cloned()
        .filter(|value| !value.is_empty())
        .ok_or_else(|| format!("missing required argument {key}"))
}

fn validate(args: &Args) -> Result<(), String> {
    if !["4k", "8k", "16k", "32k", "64k", "128k"].contains(&args.page_size.as_str()) {
        return Err(format!("unsupported page size: {}", args.page_size));
    }
    if ![
        "embedded",
        "ipc_local",
        "listener-parser",
        "manager-listener-parser",
    ]
    .contains(&args.route.as_str())
    {
        return Err(format!("unsupported route: {}", args.route));
    }
    if args.route == "embedded" {
        return Err(
            "embedded transport is unsupported by the Rust driver; no ScratchBird C++ library boundary is exposed"
                .to_string(),
        );
    }
    if !["server-parser", "standalone-parser", "driver-sblr-uuid"]
        .contains(&args.parser_mode.as_str())
    {
        return Err(format!("unsupported parser mode: {}", args.parser_mode));
    }
    Ok(())
}

fn transport_mode_for_route(route: &str, sslmode: &str) -> &'static str {
    match route {
        "embedded" => "embedded_no_network_transport",
        "ipc_local" => "local_ipc_no_tls",
        _ if sslmode == "disable" => "tls_disabled",
        _ => "tls_required",
    }
}

fn endpoint_kind_for_route(route: &str) -> &'static str {
    if route == "ipc_local" {
        return "unix_domain_socket";
    }
    if route == "embedded" {
        return "embedded_bridge";
    }
    "tcp"
}

fn transport_implementation_for_route(route: &str) -> &'static str {
    match route {
        "embedded" => "unsupported_no_cpp_library_boundary",
        "ipc_local" => "native_rust_unix_domain_socket",
        _ => "native_rust_tcp",
    }
}

fn page_size_bytes(label: &str) -> u32 {
    match label {
        "4k" => 4096,
        "8k" => 8192,
        "16k" => 16384,
        "32k" => 32768,
        "64k" => 65536,
        "128k" => 131072,
        _ => 0,
    }
}

fn route_environment(
    args: &Args,
    actual_page_size: Option<u32>,
    status: &str,
    reason: Option<&str>,
) -> JsonValue {
    let mut record = json!({
        "run_id": args.run_id,
        "driver": "rust",
        "route": args.route,
        "sslmode": args.sslmode,
        "parser_mode": args.parser_mode,
        "concurrency_mode": "single",
        "namespace": args.namespace,
        "page_size": args.page_size,
        "expected_page_size_bytes": page_size_bytes(&args.page_size),
        "actual_page_size_bytes": actual_page_size,
        "page_size_verification_source": "SHOW DATABASE",
        "page_size_verification_status": status,
        "transport_mode": transport_mode_for_route(&args.route, &args.sslmode),
        "transport_endpoint_kind": endpoint_kind_for_route(&args.route),
        "driver_transport_implementation": transport_implementation_for_route(&args.route),
    });
    if let Some(reason) = reason {
        record["failure_reason"] = json!(reason);
    }
    record
}

async fn probe_route_environment(client: &mut Client, args: &Args) -> JsonValue {
    let result = match client.query("SHOW DATABASE").await {
        Ok(result) => result,
        Err(err) => return route_environment(args, None, "fail", Some(&err.to_string())),
    };
    let mut page_index = result
        .columns
        .iter()
        .position(|column| column.name.eq_ignore_ascii_case("page_size_bytes"));
    if page_index.is_none() && result.columns.len() >= 3 {
        page_index = Some(2);
    }
    let Some(page_index) = page_index else {
        return route_environment(
            args,
            None,
            "fail",
            Some("show_database_missing_page_size_bytes"),
        );
    };
    let Some(row) = result.rows.first() else {
        return route_environment(args, None, "fail", Some("show_database_returned_no_rows"));
    };
    let actual = match row.get(page_index) {
        Some(Value::Int16(value)) => Some(*value as u32),
        Some(Value::Int32(value)) => Some(*value as u32),
        Some(Value::Int64(value)) => Some(*value as u32),
        Some(Value::String(value)) => value.parse::<u32>().ok(),
        _ => None,
    };
    let status = if actual == Some(page_size_bytes(&args.page_size)) {
        "pass"
    } else {
        "fail"
    };
    let reason = if status == "pass" {
        None
    } else {
        Some("actual_page_size_mismatch")
    };
    route_environment(args, actual, status, reason)
}

fn current_process_metrics() -> JsonValue {
    let mut vsize_kb: u64 = 1;
    let mut rss_kb: u64 = 1;
    if let Ok(statm) = fs::read_to_string("/proc/self/statm") {
        let parts: Vec<&str> = statm.split_whitespace().collect();
        if parts.len() >= 2 {
            let page_kb = 4;
            if let Ok(size_pages) = parts[0].parse::<u64>() {
                vsize_kb = std::cmp::max(1, size_pages.saturating_mul(page_kb));
            }
            if let Ok(resident_pages) = parts[1].parse::<u64>() {
                rss_kb = std::cmp::max(1, resident_pages.saturating_mul(page_kb));
            }
        }
    }
    json!({
        "client": {
            "max_rss_kb": rss_kb,
            "max_vsize_kb": vsize_kb,
            "last_rss_kb": rss_kb,
            "last_vsize_kb": vsize_kb
        }
    })
}

fn load_expected_refusals(path: &str) -> Result<BTreeSet<String>, Box<dyn std::error::Error>> {
    let mut expected = BTreeSet::<String>::new();
    if path.is_empty() {
        return Ok(expected);
    }
    let doc: JsonValue = serde_json::from_str(&fs::read_to_string(path)?)?;
    collect_expected_refusal_ids(&mut expected, &doc);
    Ok(expected)
}

fn collect_expected_refusal_ids(expected: &mut BTreeSet<String>, value: &JsonValue) {
    match value {
        JsonValue::String(text) => {
            expected.insert(text.clone());
        }
        JsonValue::Array(items) => {
            for item in items {
                collect_expected_refusal_ids(expected, item);
            }
        }
        JsonValue::Object(object) => {
            for key in ["statement_id", "statementId", "id"] {
                if let Some(JsonValue::String(text)) = object.get(key) {
                    expected.insert(text.clone());
                }
            }
            for key in [
                "statement_ids",
                "statementIds",
                "expected_refusals",
                "expectedRefusals",
                "expected_diagnostics",
                "expectedDiagnostics",
            ] {
                if let Some(nested) = object.get(key) {
                    collect_expected_refusal_ids(expected, nested);
                }
            }
        }
        _ => {}
    }
}

fn split_statements(script: &str) -> Vec<String> {
    // Delegate to the canonical SET TERM- and comment-aware splitter so this
    // tool stays consistent with the cross-driver chunker conformance fixture.
    scratchbird::sql::split_top_level_statements(script)
}

fn classify(sql: &str) -> String {
    let trimmed = sql.trim().to_ascii_lowercase();
    let first = trimmed.split_whitespace().next().unwrap_or("");
    if ["create", "alter", "drop"].contains(&first) {
        "ddl".to_string()
    } else if ["insert", "update", "delete", "merge", "upsert"].contains(&first) {
        "dml".to_string()
    } else if ["commit", "rollback", "savepoint", "begin", "start"].contains(&first) {
        "transaction".to_string()
    } else if ["grant", "revoke"].contains(&first) {
        "security_refusal".to_string()
    } else if trimmed.contains("sys.") {
        "metadata".to_string()
    } else {
        "query".to_string()
    }
}

fn read_input(path: &str) -> io::Result<String> {
    if path == "-" {
        let mut buffer = String::new();
        io::stdin().read_to_string(&mut buffer)?;
        Ok(buffer)
    } else {
        fs::read_to_string(path)
    }
}

fn add_timing(timings: &mut BTreeMap<String, u128>, group: &str, started: Instant) {
    *timings.entry(group.to_string()).or_default() += started.elapsed().as_nanos();
}

fn write_text(path: &Path, text: &str) -> io::Result<()> {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)?;
    }
    fs::write(path, text)
}

fn append_text(path: &Path, text: &str) -> io::Result<()> {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)?;
    }
    fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(path)?
        .write_all(text.as_bytes())
}

fn append_jsonl(path: &Path, record: JsonValue) -> io::Result<()> {
    append_text(path, &format!("{record}\n"))
}

fn sha256(text: &str) -> String {
    let mut hasher = Sha256::new();
    hasher.update(text.as_bytes());
    format!("sha256:{:x}", hasher.finalize())
}

fn junit(testcases: &[JsonValue], failures: &[JsonValue]) -> String {
    let mut xml = String::new();
    xml.push_str("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    xml.push_str(&format!(
        "<testsuite name=\"SBIsqlRust\" tests=\"{}\" failures=\"{}\">\n",
        testcases.len().max(1),
        failures.len()
    ));
    if testcases.is_empty() {
        xml.push_str("  <testcase classname=\"scratchbird.rust\" name=\"run\"></testcase>\n");
    }
    for testcase in testcases {
        let name = testcase["statement_id"].as_str().unwrap_or("statement");
        xml.push_str(&format!(
            "  <testcase classname=\"scratchbird.rust\" name=\"{}\"></testcase>\n",
            escape_xml(name)
        ));
    }
    for failure in failures {
        let name = failure["statement_id"].as_str().unwrap_or("run");
        let message = failure["message"].as_str().unwrap_or("failure");
        xml.push_str(&format!(
            "  <testcase classname=\"scratchbird.rust\" name=\"{}\"><failure message=\"{}\" /></testcase>\n",
            escape_xml(name),
            escape_xml(message)
        ));
    }
    xml.push_str("</testsuite>\n");
    xml
}

fn escape_xml(text: &str) -> String {
    text.replace('&', "&amp;")
        .replace('"', "&quot;")
        .replace('<', "&lt;")
        .replace('>', "&gt;")
}

use std::io::Write;
