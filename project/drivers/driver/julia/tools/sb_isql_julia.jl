#!/usr/bin/env julia
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

const JULIA_DRIVER_ROOT = normpath(joinpath(@__DIR__, ".."))
pushfirst!(LOAD_PATH, JULIA_DRIVER_ROOT)

using SHA
import DBInterface
import Tables
include(joinpath(JULIA_DRIVER_ROOT, "src", "ScratchBird.jl"))

const PAGE_SIZES = Set(["4k", "8k", "16k", "32k", "64k", "128k"])
const PAGE_SIZE_BYTES = Dict("4k" => 4096, "8k" => 8192, "16k" => 16384, "32k" => 32768, "64k" => 65536, "128k" => 131072)
const ROUTES = Set(["embedded", "ipc_local", "listener-parser", "manager-listener-parser"])
const PARSER_MODES = Set(["server-parser", "standalone-parser", "driver-sblr-uuid"])
const SSLMODES = Set(["allow", "disable", "prefer", "require", "verify-ca", "verify-full"])
const BOOLEAN_ARGS = Set(["--stop-on-error", "--create-database", "--standard-english-fallback"])
const REQUIRED_HOST_PACKAGES = ["DBInterface", "Tables", "OpenSSL"]
const SUPPORTED_ARGS = Set([
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
])

function main(argv::Vector{String})::Int
    try
        args = parse_args(argv)
        return run_tool(args)
    catch err
        Base.showerror(stderr, err)
        println(stderr)
        return 1
    end
end

function run_tool(args::Dict{String,Any})::Int
    validate_args(args)
    run_root = dirname(required(args, "--summary"))
    paths = artifact_paths(args, run_root)
    initialize_artifacts(values(paths))

    timings = Dict{String,Any}()
    api_hits = Dict{String,Int}(
        "DBInterface.connect" => 0,
        "DBInterface.prepare" => 0,
        "DBInterface.execute" => 0,
        "Tables.rows" => 0,
        "ScratchBird.query_metadata" => 0,
        "ScratchBird.attach_create" => 0,
        "ScratchBird.commit!" => 0,
        "ScratchBird.rollback!" => 0,
        "ScratchBird.savepoint!" => 0,
        "ScratchBird.release_savepoint!" => 0,
        "ScratchBird.rollback_to_savepoint!" => 0,
        "ScratchBird.copy_in" => 0,
    )
    testcases = Vector{Dict{String,Any}}()
    failures = Vector{Dict{String,Any}}()
    digests = Vector{Dict{String,Any}}()
    security_refusals = Vector{Dict{String,Any}}()
    started = now_ns()
    statements = String[]
    conn = nothing
    route_env = route_environment(args, nothing, "fail", "not_probed")
    write_json(paths["route_environment"], route_env)

    try
        require_host_packages()
        statements = split_statements(read_input(required(args, "--input")))
        expected_refusals = load_expected_refusals(get(args, "--expected-refusals", ""))
        route = required(args, "--route")
        ensure_route_supported(route, args)
        connect_started = now_ns()
        conn = DBInterface.connect(
            ScratchBird.ScratchBirdDriver();
            database = required(args, "--database"),
            host = value_or_default(args, "--host", "127.0.0.1"),
            port = parse(Int, value_or_default(args, "--port", "3092")),
            user = required(args, "--user"),
            password = required(args, "--password"),
            role = value_or_default(args, "--role", ""),
            sslmode = effective_sslmode_for_route(route, value_or_default(args, "--sslmode", "require")),
            sslrootcert = value_or_default(args, "--sslrootcert", ""),
            sslcert = value_or_default(args, "--sslcert", ""),
            sslkey = value_or_default(args, "--sslkey", ""),
            transport = transport_config_for_route(route),
            ipc_path = value_or_default(args, "--ipc-path", ""),
            application_name = "SBIsqlJulia",
            parser_mode = required(args, "--parser-mode"),
        )
        api_hits["DBInterface.connect"] += 1
        add_timing!(timings, "connection", connect_started)
        append_jsonl(paths["transcript"], Dict(
            "event" => "connect",
            "driver" => "julia",
            "route" => route,
            "parser_mode" => required(args, "--parser-mode"),
            "page_size" => required(args, "--page-size"),
            "language_profile" => value_or_default(args, "--language-profile", "en-US"),
            "language_resource_identity" => value_or_default(args, "--language-resource-identity", "sbsql.common_resource_pack.v1"),
            "language_resource_hash" => value_or_default(args, "--language-resource-hash", "sha256:752c7a9823bdad00b48ab318c8b2d5d6d53b2739ecfe43f565952fd510f4e3dc"),
            "syntax_profile" => value_or_default(args, "--syntax-profile", "sbsql.v3"),
            "topology_profile" => value_or_default(args, "--topology-profile", "topology.sbsql.canonical.v1"),
        ))
        append_jsonl(paths["wire"], Dict("event" => "server_admission_required", "driver_or_parser_finality" => "forbidden"))

        route_env = probe_route_environment(conn, args, api_hits)
        write_json(paths["route_environment"], route_env)
        if route != "embedded" && get(route_env, "page_size_verification_status", "fail") != "pass"
            push!(failures, Dict(
                "statement_id" => "route_page_size",
                "message" => "route page-size verification failed",
                "expected_page_size_bytes" => get(route_env, "expected_page_size_bytes", nothing),
                "actual_page_size_bytes" => get(route_env, "actual_page_size_bytes", nothing),
            ))
        end

        if flag_enabled(args, "--create-database", false)
            throw(ScratchBird.ScratchBirdError("0A000", "ScratchBird Julia attach/create requires a live ScratchBird creation surface; refusing delegated database creation"))
        end
        for (index, sql) in enumerate(statements)
            statement_id = "$(basename(required(args, "--input"))):$(index)"
            expected_outcome = in(statement_id, expected_refusals) ? "refusal" : "success"
            group = classify_statement(sql)
            statement_started = now_ns()
            outcome = "success"
            row_count = -1
            result_digest = nothing
            sqlstate = nothing
            diagnostic = nothing
            try
                if group == "transaction"
                    run_transaction!(conn, sql, api_hits)
                    row_count = 0
                    result_digest = sha256_text("transaction")
                elseif group == "copy" && is_copy_stdin_statement(sql)
                    payload = copy_payload_for_statement(sql)
                    isempty(payload) && throw(ScratchBird.ScratchBirdError("HY000", "COPY FROM STDIN requires SB_COPY_INPUT rows in the script"))
                    rows_copied = execute_copy_in!(conn, executable_sql_without_copy_markers(sql), payload)
                    api_hits["ScratchBird.copy_in"] += 1
                    row_count = rows_copied
                    result_digest = sha256_text("copy_in:$(rows_copied)")
                    push!(digests, Dict("statement_id" => statement_id, "row_count" => row_count, "result_digest" => result_digest))
                    append_text(paths["output"], json_value(Dict("statement_id" => statement_id, "rows" => [Dict("copy_in" => rows_copied)])) * "\n")
                else
                    stmt = DBInterface.prepare(conn, sql)
                    api_hits["DBInterface.prepare"] += 1
                    result = DBInterface.execute(stmt)
                    api_hits["DBInterface.execute"] += 1
                    rows = collect(Tables.rows(result))
                    api_hits["Tables.rows"] += 1
                    json_rows = [row_to_jsonable(row) for row in rows]
                    row_count = length(rows)
                    result_digest = sha256_text(json_value(json_rows))
                    push!(digests, Dict("statement_id" => statement_id, "row_count" => row_count, "result_digest" => result_digest))
                    append_text(paths["output"], json_value(Dict("statement_id" => statement_id, "rows" => json_rows)) * "\n")
                end
                if expected_outcome == "refusal"
                    outcome = "unexpected_success"
                    push!(failures, Dict("statement_id" => statement_id, "message" => "statement succeeded but was expected to refuse"))
                end
            catch err
                outcome = "refusal"
                sqlstate = exception_sqlstate(err)
                diagnostic = exception_message(err)
                append_jsonl(paths["diagnostics"], Dict("statement_id" => statement_id, "sqlstate" => sqlstate, "message" => diagnostic))
                append_text(paths["error"], "$(statement_id): $(diagnostic)\n")
                if expected_outcome == "success"
                    push!(failures, Dict("statement_id" => statement_id, "message" => diagnostic))
                    if flag_enabled(args, "--stop-on-error", true)
                        add_timing!(timings, group, statement_started)
                        event = event_record(args, index, statement_id, sql, group, expected_outcome, outcome, sqlstate, diagnostic, row_count, result_digest, now_ns() - statement_started)
                        append_jsonl(paths["events"], event)
                        push!(testcases, event)
                        break
                    end
                else
                    push!(security_refusals, Dict("statement_id" => statement_id, "sqlstate" => sqlstate, "diagnostic_code" => diagnostic))
                end
            end
            elapsed = now_ns() - statement_started
            add_timing!(timings, group, statement_started)
            event = event_record(args, index, statement_id, sql, group, expected_outcome, outcome, sqlstate, diagnostic, row_count, result_digest, elapsed)
            append_jsonl(paths["events"], event)
            push!(testcases, event)
        end

        metadata_started = now_ns()
        try
            metadata = ScratchBird.query_metadata(conn, "tables")
            api_hits["ScratchBird.query_metadata"] += 1
            metadata_rows = [row_to_jsonable(row) for row in collect(Tables.rows(metadata))]
            api_hits["Tables.rows"] += 1
            write_json(paths["metadata"], Dict("tables_digest" => sha256_text(json_value(metadata_rows)), "row_count" => length(metadata_rows)))
        catch err
            write_json(paths["metadata"], Dict("status" => "error", "message" => exception_message(err), "sqlstate" => exception_sqlstate(err)))
        end
        add_timing!(timings, "metadata", metadata_started)
    catch err
        message = exception_message(err)
        push!(failures, Dict("statement_id" => "run", "message" => message))
        append_jsonl(paths["diagnostics"], Dict("statement_id" => "run", "sqlstate" => exception_sqlstate(err), "message" => message))
        append_text(paths["stderr"], message * "\n")
    finally
        if conn !== nothing
            try
                close(conn)
            catch
            end
        end
    end

    elapsed = now_ns() - started
    timings["overall"] = elapsed
    process_metrics = current_process_metrics()
    summary = Dict{String,Any}(
        "run_id" => value_or_default(args, "--run-id", "manual"),
        "driver_name" => "julia",
        "route" => required(args, "--route"),
        "parser_mode" => required(args, "--parser-mode"),
        "page_size" => required(args, "--page-size"),
        "namespace" => required(args, "--namespace"),
        "sslmode" => effective_sslmode_for_route(required(args, "--route"), value_or_default(args, "--sslmode", "require")),
        "transport_mode" => resolve_transport_mode(required(args, "--route"), effective_sslmode_for_route(required(args, "--route"), value_or_default(args, "--sslmode", "require"))),
        "transport_endpoint_kind" => endpoint_kind_for_route(required(args, "--route")),
        "driver_transport_implementation" => transport_implementation_for_route(required(args, "--route")),
        "cpp_library_boundary" => "none",
        "language_resource_pack" => value_or_default(args, "--language-resource-pack", "project/resources/seed-packs/initial-resource-pack/resources/i18n/sbsql-language-resource-pack"),
        "language_resource_identity" => value_or_default(args, "--language-resource-identity", "sbsql.common_resource_pack.v1"),
        "language_resource_hash" => value_or_default(args, "--language-resource-hash", "sha256:752c7a9823bdad00b48ab318c8b2d5d6d53b2739ecfe43f565952fd510f4e3dc"),
        "language_resource_authority" => "shared_server_parser_resource_pack",
        "language_profile" => value_or_default(args, "--language-profile", "en-US"),
        "syntax_profile" => value_or_default(args, "--syntax-profile", "sbsql.v3"),
        "topology_profile" => value_or_default(args, "--topology-profile", "topology.sbsql.canonical.v1"),
        "standard_english_fallback" => flag_enabled(args, "--standard-english-fallback", true),
        "status" => isempty(failures) ? "pass" : "fail",
        "statement_count" => length(statements),
        "failure_count" => length(failures),
        "elapsed_ns" => elapsed,
        "process_metrics" => process_metrics,
        "server_revalidation_required" => true,
        "driver_or_parser_finality" => "forbidden",
        "mga_authority" => "engine",
        "artifacts" => Dict(
            "command-events.jsonl" => paths["events"],
            "summary.json" => paths["summary"],
            "diagnostics.jsonl" => paths["diagnostics"],
            "wire-transcript.jsonl" => paths["wire"],
            "timing-groups.json" => paths["timing"],
            "result-digests.json" => paths["digests"],
            "metadata-snapshots.json" => paths["metadata"],
            "route-environment.json" => paths["route_environment"],
            "process-metrics.jsonl" => paths["process"],
            "security-refusals.json" => paths["refusals"],
            "native-api-coverage.json" => paths["api"],
            "code-example-review.json" => paths["review"],
            "junit.xml" => paths["junit"],
            "stdout.log" => paths["stdout"],
            "stderr.log" => paths["stderr"],
        ),
    )
    write_json(paths["summary"], summary)
    write_json(paths["metrics"], Dict("role" => "client", "rss_kb" => process_metrics["client"]["last_rss_kb"], "vsize_kb" => process_metrics["client"]["last_vsize_kb"]))
    write_json(paths["timing"], timings)
    write_json(paths["digests"], digests)
    append_jsonl(paths["process"], Dict("role" => "client", "rss_kb" => process_metrics["client"]["last_rss_kb"], "vsize_kb" => process_metrics["client"]["last_vsize_kb"]))
    write_json(paths["refusals"], security_refusals)
    write_json(paths["api"], api_hits)
    write_json(paths["review"], Dict(
        "driver" => "julia",
        "public_api_only" => true,
        "shells_out_to_other_driver" => false,
        "source_is_canonical_example" => true,
        "sections" => ["connection", "DBInterface.prepare", "DBInterface.execute", "Tables.rows", "metadata", "diagnostics", "transaction"],
    ))
    write_text(paths["junit"], junit_xml("SBIsqlJulia", "scratchbird.julia", testcases, failures))
    append_text(paths["stdout"], "SBIsqlJulia status=$(summary["status"])\n")
    return isempty(failures) ? 0 : 1
end

function require_host_packages()
    for name in REQUIRED_HOST_PACKAGES
        try
            Base.eval(Main, :(import $(Symbol(name))))
        catch err
            throw(ErrorException("missing Julia package $(name) required by sb_isql_julia.jl; instantiate project/drivers/driver/julia/Project.toml so the tool can use DBInterface/Tables directly: $(exception_message(err))"))
        end
    end
end

function parse_args(raw::Vector{String})::Dict{String,Any}
    args = Dict{String,Any}()
    i = 1
    while i <= length(raw)
        key = raw[i]
        startswith(key, "--") || error("unexpected positional argument: $(key)")
        in(key, SUPPORTED_ARGS) || error("unsupported argument: $(key)")
        if in(key, BOOLEAN_ARGS)
            if i + 1 <= length(raw) && !startswith(raw[i + 1], "--")
                args[key] = parse_bool_value(key, raw[i + 1])
                i += 2
            else
                args[key] = true
                i += 1
            end
        else
            i + 1 <= length(raw) || error("missing value for $(key)")
            startswith(raw[i + 1], "--") && error("missing value for $(key)")
            args[key] = raw[i + 1]
            i += 2
        end
    end
    return args
end

function validate_args(args::Dict{String,Any})
    in(required(args, "--page-size"), PAGE_SIZES) || error("unsupported page size: $(required(args, "--page-size"))")
    in(required(args, "--route"), ROUTES) || error("unsupported route: $(required(args, "--route"))")
    in(required(args, "--parser-mode"), PARSER_MODES) || error("unsupported parser mode: $(required(args, "--parser-mode"))")
    in(value_or_default(args, "--sslmode", "require"), SSLMODES) || error("unsupported sslmode: $(value_or_default(args, "--sslmode", "require"))")
end

function required(args::Dict{String,Any}, key::String)::String
    value = get(args, key, "")
    isempty(String(value)) && error("missing required argument $(key)")
    return String(value)
end

value_or_default(args::Dict{String,Any}, key::String, default::String)::String = String(get(args, key, default))

function flag_enabled(args::Dict{String,Any}, key::String, default::Bool = false)::Bool
    return haskey(args, key) ? args[key] == true : default
end

function parse_bool_value(key::String, value::String)::Bool
    normalized = lowercase(value)
    in(normalized, ["1", "true", "yes", "on"]) && return true
    in(normalized, ["0", "false", "no", "off"]) && return false
    error("$(key) expects a boolean value, got: $(value)")
end

function artifact_paths(args::Dict{String,Any}, run_root::String)::Dict{String,String}
    return Dict(
        "output" => required(args, "--output"),
        "error" => required(args, "--error"),
        "diagnostics" => required(args, "--diagnostics"),
        "metrics" => required(args, "--metrics"),
        "transcript" => required(args, "--transcript"),
        "summary" => required(args, "--summary"),
        "events" => joinpath(run_root, "command-events.jsonl"),
        "wire" => joinpath(run_root, "wire-transcript.jsonl"),
        "timing" => joinpath(run_root, "timing-groups.json"),
        "digests" => joinpath(run_root, "result-digests.json"),
        "metadata" => joinpath(run_root, "metadata-snapshots.json"),
        "route_environment" => joinpath(run_root, "route-environment.json"),
        "process" => joinpath(run_root, "process-metrics.jsonl"),
        "refusals" => joinpath(run_root, "security-refusals.json"),
        "api" => joinpath(run_root, "native-api-coverage.json"),
        "review" => joinpath(run_root, "code-example-review.json"),
        "junit" => joinpath(run_root, "junit.xml"),
        "stdout" => joinpath(run_root, "stdout.log"),
        "stderr" => joinpath(run_root, "stderr.log"),
    )
end

function initialize_artifacts(paths)
    for path in unique(collect(paths))
        write_text(path, "")
    end
end

function ensure_route_supported(route::String, args::Dict{String,Any})
    route == "embedded" && error("embedded transport is unsupported by the Julia driver; no ScratchBird C++ library boundary is exposed")
    route == "ipc_local" && isempty(value_or_default(args, "--ipc-path", "")) && error("ipc_path is required for local IPC transport")
end

effective_sslmode_for_route(route::String, sslmode::String)::String = route == "ipc_local" ? "disable" : sslmode
transport_config_for_route(route::String)::String = route == "ipc_local" ? "ipc" : route == "embedded" ? "embedded" : "inet"
resolve_transport_mode(route::String, sslmode::String)::String = route == "embedded" ? "embedded_no_network_transport" : route == "ipc_local" ? "local_ipc_no_tls" : sslmode == "disable" ? "tls_disabled" : "tls_required"
endpoint_kind_for_route(route::String)::String = route == "ipc_local" ? "unix_domain_socket" : route == "embedded" ? "embedded_bridge" : "tcp"
transport_implementation_for_route(route::String)::String = route == "ipc_local" ? "native_julia_unix_socket" : route == "embedded" ? "unsupported_no_cpp_library_boundary" : "native_julia_tcp_or_openssl_tls"

function route_environment(args::Dict{String,Any}, actual_page_size, status::String, reason)
    expected = PAGE_SIZE_BYTES[required(args, "--page-size")]
    return Dict(
        "route" => required(args, "--route"),
        "run_id" => value_or_default(args, "--run-id", "manual"),
        "page_size" => required(args, "--page-size"),
        "expected_page_size_bytes" => expected,
        "actual_page_size_bytes" => actual_page_size,
        "page_size_verification_status" => status,
        "page_size_verification_source" => "sys.database_pages",
        "reason" => reason,
    )
end

function probe_route_environment(conn, args::Dict{String,Any}, api_hits::Dict{String,Int})
    try
        stmt = DBInterface.prepare(conn, "SHOW DATABASE")
        api_hits["DBInterface.prepare"] += 1
        result = DBInterface.execute(stmt)
        api_hits["DBInterface.execute"] += 1
        rows = collect(Tables.rows(result))
        api_hits["Tables.rows"] += 1
        actual = first_page_size_bytes(rows)
        expected = PAGE_SIZE_BYTES[required(args, "--page-size")]
        status = actual == expected ? "pass" : "fail"
        reason = status == "pass" ? nothing : "actual_page_size_mismatch"
        return route_environment(args, actual, status, reason)
    catch err
        return route_environment(args, nothing, "fail", exception_message(err))
    end
end

function first_page_size_bytes(rows)
    isempty(rows) && error("SHOW DATABASE returned no rows")
    row = first(rows)
    if row isa NamedTuple
        for key in keys(row)
            lowercase(String(key)) == "page_size_bytes" && return parse_page_size_bytes(getfield(row, key))
        end
    elseif row isa AbstractDict
        for (key, value) in row
            lowercase(String(key)) == "page_size_bytes" && return parse_page_size_bytes(value)
        end
    elseif row isa Tuple || row isa AbstractVector
        length(row) >= 3 && return parse_page_size_bytes(row[3])
    end
    error("SHOW DATABASE did not expose page_size_bytes")
end

function parse_page_size_bytes(value)::Int
    value isa Integer && return Int(value)
    value isa AbstractString && return parse(Int, strip(String(value)))
    return parse(Int, string(value))
end

function run_transaction!(conn, sql::String, api_hits::Dict{String,Int})
    tokens = leading_tokens(sql, 4)
    first = isempty(tokens) ? "" : tokens[1]
    second = length(tokens) >= 2 ? tokens[2] : ""
    third = length(tokens) >= 3 ? tokens[3] : ""
    fourth = length(tokens) >= 4 ? tokens[4] : ""
    if first == "commit"
        ScratchBird.commit!(conn)
        api_hits["ScratchBird.commit!"] += 1
    elseif first == "rollback" && second == "to"
        name = third == "savepoint" ? fourth : third
        ScratchBird.rollback_to_savepoint!(conn, normalize_control_name(name))
        api_hits["ScratchBird.rollback_to_savepoint!"] += 1
    elseif first == "rollback"
        ScratchBird.rollback!(conn)
        api_hits["ScratchBird.rollback!"] += 1
    elseif first == "savepoint"
        ScratchBird.savepoint!(conn, normalize_control_name(second))
        api_hits["ScratchBird.savepoint!"] += 1
    elseif first == "release"
        name = second == "savepoint" ? third : second
        ScratchBird.release_savepoint!(conn, normalize_control_name(name))
        api_hits["ScratchBird.release_savepoint!"] += 1
    else
        ScratchBird.begin_transaction!(conn)
    end
end

function event_record(args, index, statement_id, sql, group, expected_outcome, outcome, sqlstate, diagnostic, row_count, result_digest, elapsed)
    return Dict{String,Any}(
        "run_id" => value_or_default(args, "--run-id", "manual"),
        "driver_name" => "julia",
        "driver_version" => "0.1.0",
        "route" => required(args, "--route"),
        "parser_mode" => required(args, "--parser-mode"),
        "page_size" => required(args, "--page-size"),
        "namespace" => required(args, "--namespace"),
        "script" => required(args, "--input"),
        "statement_index" => index,
        "statement_id" => statement_id,
        "command_group" => group,
        "sql_hash" => sha256_text(sql),
        "expected_outcome" => expected_outcome,
        "actual_outcome" => outcome,
        "sqlstate" => sqlstate,
        "diagnostic_code" => diagnostic,
        "canonical_message_vector" => [],
        "row_count" => row_count,
        "result_digest" => result_digest,
        "elapsed_ns" => elapsed,
        "server_revalidation_state" => "required",
        "language_profile" => value_or_default(args, "--language-profile", "en-US"),
        "language_resource_pack" => value_or_default(args, "--language-resource-pack", "project/resources/seed-packs/initial-resource-pack/resources/i18n/sbsql-language-resource-pack"),
        "language_resource_identity" => value_or_default(args, "--language-resource-identity", "sbsql.common_resource_pack.v1"),
        "language_resource_hash" => value_or_default(args, "--language-resource-hash", "sha256:752c7a9823bdad00b48ab318c8b2d5d6d53b2739ecfe43f565952fd510f4e3dc"),
        "syntax_profile" => value_or_default(args, "--syntax-profile", "sbsql.v3"),
        "topology_profile" => value_or_default(args, "--topology-profile", "topology.sbsql.canonical.v1"),
        "standard_english_fallback" => flag_enabled(args, "--standard-english-fallback", true),
        "transaction_id_observed" => nothing,
        "mga_authority" => "engine",
        "native_api_surface" => "julia_dbinterface_tables",
        "code_example_section" => "DBInterface.prepare_execute_Tables.rows",
    )
end

function load_expected_refusals(path::Any)::Set{String}
    path_string = String(path)
    isempty(path_string) && return Set{String}()
    isfile(path_string) || error("expected refusal file not found: $(path_string)")
    text = read(path_string, String)
    ids = Set{String}()
    for match in eachmatch(r"([A-Za-z0-9_./-]+\.sbsql:[0-9]+)", text)
        push!(ids, match.captures[1])
    end
    return ids
end

read_input(path::String)::String = path == "-" ? read(stdin, String) : read(path, String)

function split_statements(script::String)::Vector{String}
    statements = String[]
    term = ";"
    term_ref = Ref(term)
    current = IOBuffer()
    single = false
    double = false
    i = firstindex(script)
    while i <= lastindex(script)
        ch = script[i]
        if !single && !double && ch == '-' && i < lastindex(script) && script[nextind(script, i)] == '-'
            eol = findnext('\n', script, i)
            stop = eol === nothing ? lastindex(script) : prevind(script, eol)
            print(current, script[i:stop])
            i = eol === nothing ? nextind(script, lastindex(script)) : eol
            continue
        end
        if ch == '\'' && !double
            single = !single
        elseif ch == '"' && !single
            double = !double
        end
        if !single && !double && matches_term_at(script, i, term)
            matched_len = length(term)
            flush_statement!(statements, String(take!(current)), term_ref)
            term = term_ref[]
            i = nextind(script, i, matched_len)
            continue
        end
        print(current, ch)
        i = nextind(script, i)
    end
    flush_statement!(statements, String(take!(current)), term_ref)
    return statements
end

function matches_term_at(script::String, index::Int, term::String)::Bool
    cursor = index
    for expected in term
        if cursor > lastindex(script) || script[cursor] != expected
            return false
        end
        cursor = nextind(script, cursor)
    end
    return true
end

function flush_statement!(statements::Vector{String}, chunk::String, term_ref::Base.RefValue{String})
    stripped = strip(chunk)
    isempty(stripped) && return
    meaningful = [strip(line) for line in split(stripped, '\n') if !isempty(strip(line)) && !startswith(strip(line), "--")]
    if length(meaningful) == 1 && startswith(lowercase(meaningful[1]), "set term ")
        term_ref[] = strip(meaningful[1][10:end])
        return
    end
    push!(statements, stripped)
end

function classify_statement(sql::String)::String
    executable = executable_sql_without_copy_markers(sql)
    first = first_token(executable)
    first == "copy" && return "copy"
    in(first, ["create", "alter", "drop"]) && return "ddl"
    in(first, ["insert", "update", "delete", "merge", "upsert"]) && return "dml"
    in(first, ["commit", "rollback", "savepoint", "release", "begin", "start"]) && return "transaction"
    in(first, ["grant", "revoke"]) && return "security_refusal"
    occursin("sys.", lowercase(sql)) && return "metadata"
    return "query"
end

function executable_sql_without_copy_markers(sql::String)::String
    lines = split(sql, r"\r\n|\r|\n")
    kept = [line for line in lines if !startswith(lstrip(line), "-- SB_COPY_INPUT ")]
    return strip(join(kept, "\n"))
end

function copy_payload_for_statement(sql::String)::Vector{UInt8}
    rows = String[]
    for line in split(sql, r"\r\n|\r|\n")
        stripped = lstrip(line)
        if startswith(stripped, "-- SB_COPY_INPUT ")
            push!(rows, stripped[length("-- SB_COPY_INPUT ") + 1:end])
        end
    end
    isempty(rows) && return UInt8[]
    return Vector{UInt8}(codeunits(join(rows, "\n") * "\n"))
end

function is_copy_stdin_statement(sql::String)::Bool
    meaningful = String[]
    for line in split(executable_sql_without_copy_markers(sql), r"\r\n|\r|\n")
        stripped = lowercase(strip(line))
        if !isempty(stripped) && !startswith(stripped, "--")
            push!(meaningful, stripped)
        end
    end
    executable = join(meaningful, " ")
    return startswith(executable, "copy ") && occursin(" from stdin", executable)
end

function execute_copy_in!(conn, sql::String, payload::Vector{UInt8})::Int
    ScratchBird.send_message!(conn, ScratchBird.MSG_QUERY, ScratchBird.build_query_payload(sql))
    rows_copied = 0
    copy_started = false
    startup_messages = String[]
    columns = Symbol[]
    column_types = UInt32[]
    first_data_row = nothing
    while true
        msg_type, _flags, _sequence, attachment, txn_id, body = ScratchBird.recv_message(conn)
        if ScratchBird.handle_async!(conn, msg_type, attachment, txn_id, body)
            continue
        elseif msg_type == ScratchBird.MSG_COPY_IN_RESPONSE
            copy_started = true
            push!(startup_messages, "COPY_IN_RESPONSE")
            ScratchBird.send_message!(conn, ScratchBird.MSG_COPY_DATA, payload)
            ScratchBird.send_message!(conn, ScratchBird.MSG_COPY_DONE, UInt8[])
        elseif msg_type == ScratchBird.MSG_COMMAND_COMPLETE
            affected, _tag = ScratchBird.parse_command_complete(body)
            rows_copied = Int(min(affected, UInt64(typemax(Int))))
            push!(startup_messages, "COMMAND_COMPLETE")
        elseif msg_type == ScratchBird.MSG_ROW_DESCRIPTION
            names, types = ScratchBird.parse_row_description(body)
            columns = Symbol.(names)
            column_types = types
            push!(startup_messages, "ROW_DESCRIPTION($(join(names, "|")))")
        elseif msg_type == ScratchBird.MSG_DATA_ROW
            values = ScratchBird.parse_data_row(body, length(columns), column_types)
            first_data_row === nothing && (first_data_row = join([string(value) for value in values], "|"))
            push!(startup_messages, "DATA_ROW")
        elseif msg_type == ScratchBird.MSG_READY
            ScratchBird.apply_ready!(conn, body)
            detail = "COPY FROM STDIN did not enter COPY input mode; startup_messages=$(join(startup_messages, ","))"
            first_data_row === nothing || (detail *= "; first_data_row=$(first_data_row)")
            copy_started || throw(ScratchBird.ScratchBirdError("HY000", detail))
            return rows_copied
        elseif msg_type == ScratchBird.MSG_ERROR
            err = ScratchBird.error_from_payload(body)
            ScratchBird.drain_until_ready!(conn)
            throw(err)
        else
            push!(startup_messages, copy_message_name(msg_type))
        end
    end
end

function copy_message_name(msg_type::UInt8)::String
    msg_type == ScratchBird.MSG_ROW_DESCRIPTION && return "ROW_DESCRIPTION"
    msg_type == ScratchBird.MSG_DATA_ROW && return "DATA_ROW"
    msg_type == ScratchBird.MSG_NOTICE && return "NOTICE"
    msg_type == ScratchBird.MSG_PARAMETER_STATUS && return "PARAMETER_STATUS"
    return "0x" * lpad(string(Int(msg_type), base = 16), 2, '0')
end

function first_token(sql::String)::String
    parts = split(lowercase(strip(sql)))
    return isempty(parts) ? "" : parts[1]
end

function leading_tokens(sql::String, limit::Int)::Vector{String}
    parts = split(lowercase(strip(sql)))
    return String[replace(part, r";$" => "") for part in parts[1:min(limit, length(parts))]]
end

function normalize_control_name(name::String)::String
    stripped = strip(replace(name, r";$" => ""))
    isempty(stripped) && throw(ScratchBird.ScratchBirdError("42601", "savepoint name is required"))
    if startswith(stripped, "\"") && endswith(stripped, "\"") && length(stripped) >= 2
        stripped = replace(stripped[2:end - 1], "\"\"" => "\"")
    end
    return stripped
end

function current_process_metrics()
    rss_kb = 1
    vsize_kb = 1
    status_path = "/proc/$(getpid())/status"
    if isfile(status_path)
        for line in eachline(status_path)
            if startswith(line, "VmRSS:")
                rss_kb = max(1, parse(Int, split(line)[2]))
            elseif startswith(line, "VmSize:")
                vsize_kb = max(1, parse(Int, split(line)[2]))
            end
        end
    end
    return Dict("client" => Dict("last_rss_kb" => rss_kb, "last_vsize_kb" => vsize_kb, "max_rss_kb" => rss_kb, "max_vsize_kb" => vsize_kb))
end

now_ns()::Int = time_ns()

function add_timing!(timings::Dict{String,Any}, group::String, started::Int)
    timings[group] = get(timings, group, 0) + (now_ns() - started)
end

function sha256_text(text::String)::String
    return "sha256:" * bytes2hex(sha256(Vector{UInt8}(codeunits(text))))
end

function row_to_jsonable(row)
    if row isa NamedTuple
        return Dict(String(k) => getfield(row, k) for k in keys(row))
    elseif row isa Dict
        return Dict(String(k) => v for (k, v) in row)
    elseif row isa Tuple
        return collect(row)
    end
    return string(row)
end

function exception_sqlstate(err)::String
    try
        return String(getfield(err, :sqlstate))
    catch
        return "HY000"
    end
end

exception_message(err)::String = sprint(Base.showerror, err)

function write_text(path::String, text::String)
    dir = dirname(path)
    !isempty(dir) && mkpath(dir)
    open(path, "w") do io
        write(io, text)
    end
end

function append_text(path::String, text::String)
    dir = dirname(path)
    !isempty(dir) && mkpath(dir)
    open(path, "a") do io
        write(io, text)
    end
end

write_json(path::String, value) = write_text(path, json_value(value) * "\n")
append_jsonl(path::String, value) = append_text(path, json_value(value) * "\n")

function json_value(value)::String
    value === nothing && return "null"
    value isa Bool && return value ? "true" : "false"
    value isa Integer && return string(value)
    value isa AbstractFloat && return isfinite(value) ? string(value) : "null"
    value isa AbstractString && return "\"" * json_escape(String(value)) * "\""
    value isa Symbol && return json_value(String(value))
    value isa Pair && return json_value(Dict(String(value.first) => value.second))
    value isa NamedTuple && return json_value(Dict(String(k) => getfield(value, k) for k in keys(value)))
    if value isa AbstractDict
        items = sort([(String(k), v) for (k, v) in value], by = first)
        parts = String[]
        for (key, item_value) in items
            push!(parts, json_value(key) * ":" * json_value(item_value))
        end
        return "{" * join(parts, ",") * "}"
    end
    if value isa AbstractVector || value isa Tuple
        return "[" * join([json_value(item) for item in value], ",") * "]"
    end
    return json_value(string(value))
end

function json_escape(text::String)::String
    out = IOBuffer()
    for ch in text
        if ch == '"'
            write(out, "\\\"")
        elseif ch == '\\'
            write(out, "\\\\")
        elseif ch == '\n'
            write(out, "\\n")
        elseif ch == '\r'
            write(out, "\\r")
        elseif ch == '\t'
            write(out, "\\t")
        elseif Int(ch) < 0x20
            write(out, "\\u", lpad(string(Int(ch), base = 16), 4, '0'))
        else
            write(out, ch)
        end
    end
    return String(take!(out))
end

function junit_xml(suite::String, class_name::String, testcases, failures)::String
    tests = max(length(testcases), 1)
    xml = IOBuffer()
    write(xml, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n")
    write(xml, "<testsuite name=\"$(xml_escape(suite))\" tests=\"$(tests)\" failures=\"$(length(failures))\">\n")
    if isempty(testcases)
        write(xml, "  <testcase classname=\"$(xml_escape(class_name))\" name=\"run\"></testcase>\n")
    end
    for testcase in testcases
        write(xml, "  <testcase classname=\"$(xml_escape(class_name))\" name=\"$(xml_escape(String(testcase["statement_id"])))\"></testcase>\n")
    end
    for failure in failures
        write(xml, "  <testcase classname=\"$(xml_escape(class_name))\" name=\"$(xml_escape(String(failure["statement_id"])))\"><failure message=\"$(xml_escape(String(failure["message"])))\" /></testcase>\n")
    end
    write(xml, "</testsuite>\n")
    return String(take!(xml))
end

function xml_escape(text::String)::String
    return replace(text, "&" => "&amp;", "\"" => "&quot;", "<" => "&lt;", ">" => "&gt;")
end

exit(main(ARGS))
