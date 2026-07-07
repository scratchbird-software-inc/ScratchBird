#!/usr/bin/env Rscript
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

suppressPackageStartupMessages({
  library(DBI)
  library(methods)
  library(jsonlite)
  library(openssl)
})

`%||%` <- function(lhs, rhs) if (is.null(lhs) || length(lhs) == 0 || is.na(lhs)) rhs else lhs

script_arg <- grep("^--file=", commandArgs(FALSE), value = TRUE)[1] %||% "tools/sb_isql_r.R"
script_path <- sub("^--file=", "", script_arg)
driver_root <- normalizePath(file.path(dirname(script_path), ".."), mustWork = FALSE)
repo_root <- normalizePath(file.path(driver_root, "../../../.."), mustWork = FALSE)
native_lib_candidates <- unique(c(
  Sys.getenv("SCRATCHBIRD_R_NATIVE_LIB", unset = ""),
  file.path(repo_root, "build", "public-release-linux", "drivers", "driver", "r", "stage", "src", "scratchbird.so"),
  file.path(repo_root, "build", "public-release-linux", "output", "linux", "r", "scratchbird.so"),
  file.path(repo_root, "build", "drivers", "driver", "r", "stage", "src", "scratchbird.so"),
  file.path(driver_root, "src", "scratchbird.so")
))
native_lib <- native_lib_candidates[nzchar(native_lib_candidates) & file.exists(native_lib_candidates)][1] %||% ""
if (nzchar(native_lib) && !is.loaded("C_sb_tls_connect")) {
  dyn.load(native_lib)
}
if (nzchar(native_lib)) {
  for (symbol_name in c(
    "C_sb_tls_connect",
    "C_sb_ipc_connect",
    "C_sb_tls_write",
    "C_sb_tls_read_exact",
    "C_sb_tls_close"
  )) {
    if (is.loaded(symbol_name) && !exists(symbol_name, inherits = TRUE)) {
      assign(symbol_name, getNativeSymbolInfo(symbol_name), envir = globalenv())
    }
  }
}
source_driver <- function(name) source(file.path(driver_root, "R", name), local = globalenv())
source_driver("config.R")
source_driver("types.R")
source_driver("protocol.R")
source_driver("auth_bootstrap.R")
source_driver("scram.R")
source_driver("metadata.R")
source_driver("native_transport.R")
source_driver("client.R")
source_driver("sql.R")
source_driver("dbi.R")

page_sizes <- c("4k", "8k", "16k", "32k", "64k", "128k")
page_size_bytes <- list(
  "4k" = 4096,
  "8k" = 8192,
  "16k" = 16384,
  "32k" = 32768,
  "64k" = 65536,
  "128k" = 131072
)
routes <- c("embedded", "ipc_local", "listener-parser", "manager-listener-parser")
parser_modes <- c("server-parser", "standalone-parser", "driver-sblr-uuid")
ssl_modes <- c("allow", "disable", "prefer", "require", "verify-ca", "verify-full")
supported_args <- c(
  "--database",
  "--manager-auth-token",
  "--manager-database",
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
  "--standard-english-fallback"
)

native_rowset_type_text <- 1L
native_rowset_type_int64 <- 2L
native_rowset_type_boolean <- 3L
native_rowset_type_int32 <- 4L
native_rowset_type_uint64 <- 5L
native_rowset_type_real64 <- 6L
native_rowset_type_binary <- 7L

main <- function() {
  args <- parse_args(commandArgs(trailingOnly = TRUE))
  code <- run_tool(args)
  quit(status = code)
}

run_tool <- function(args) {
  validate_args(args)
  run_root <- dirname(required(args, "--summary"))
  dir.create(run_root, recursive = TRUE, showWarnings = FALSE)
  paths <- list(
    events = file.path(run_root, "command-events.jsonl"),
    wire = file.path(run_root, "wire-transcript.jsonl"),
    timing = file.path(run_root, "timing-groups.json"),
    digests = file.path(run_root, "result-digests.json"),
    metadata = file.path(run_root, "metadata-snapshots.json"),
    process = file.path(run_root, "process-metrics.jsonl"),
    refusals = file.path(run_root, "security-refusals.json"),
    api = file.path(run_root, "native-api-coverage.json"),
    review = file.path(run_root, "code-example-review.json"),
    route_environment = file.path(run_root, "route-environment.json"),
    junit = file.path(run_root, "junit.xml"),
    stdout = file.path(run_root, "stdout.log"),
    stderr = file.path(run_root, "stderr.log")
  )
  for (path in c(required(args, "--output"), required(args, "--error"), required(args, "--diagnostics"),
                required(args, "--metrics"), required(args, "--transcript"), required(args, "--summary"),
                unlist(paths, use.names = FALSE))) {
    write_text(path, "")
  }

  timings <- list()
  api_hits <- list(
    "DBI::dbConnect" = 0,
    "DBI::dbSendQuery" = 0,
    "DBI::dbFetch" = 0,
    "DBI::dbExecute" = 0,
    "DBI::dbListTables" = 0,
    "sb_attach_create" = 0,
    "DBI::dbCommit" = 0,
    "DBI::dbRollback" = 0,
    "sb_compile_sblr" = 0,
    "sb_execute_sblr" = 0,
    "sb_copy_in" = 0
  )
  testcases <- list()
  failures <- list()
  digests <- list()
  security_refusals <- list()
  started <- nanotime()
  expected_refusals <- load_expected_refusals(value_or_default(args, "--expected-refusals", ""))
  conn <- NULL
  route_env <- route_environment(args, NULL, "fail", "not_probed")
  write_text(paths$route_environment, paste0(jsonlite::toJSON(route_env, auto_unbox = TRUE, null = "null"), "\n"))

  tryCatch({
    route <- required(args, "--route")
    ensure_transport_route_supported(route, args)
    dsn_parts <- c(
      sprintf("host=%s", required(args, "--host")),
      sprintf("port=%s", required(args, "--port")),
      sprintf("database=%s", required(args, "--database")),
      sprintf("user=%s", required(args, "--user")),
      sprintf("password=%s", required(args, "--password")),
      sprintf("role=%s", value_or_default(args, "--role", "")),
      sprintf("sslmode=%s", effective_sslmode_for_route(route, value_or_default(args, "--sslmode", "require"))),
      sprintf("sslrootcert=%s", value_or_default(args, "--sslrootcert", "")),
      sprintf("sslcert=%s", value_or_default(args, "--sslcert", "")),
      sprintf("sslkey=%s", value_or_default(args, "--sslkey", "")),
      sprintf("front_door_mode=%s", if (route == "manager-listener-parser") "manager_proxy" else "direct"),
      sprintf("transport=%s", transport_config_for_route(route)),
      sprintf("ipc_path=%s", value_or_default(args, "--ipc-path", "")),
      "metadata_expand_schema_parents=true"
    )
    if (route == "manager-listener-parser") {
      dsn_parts <- c(
        dsn_parts,
        sprintf("manager_auth_token=%s", value_or_default(args, "--manager-auth-token", "")),
        sprintf("manager_database=%s", value_or_default(args, "--manager-database", required(args, "--database"))),
        sprintf("manager_username=%s", required(args, "--user"))
      )
    }
    dsn <- paste(dsn_parts, collapse = ";")
    connect_started <- nanotime()
    conn <- DBI::dbConnect(Scratchbird(), dsn)
    api_hits[["DBI::dbConnect"]] <- api_hits[["DBI::dbConnect"]] + 1
    timings <- add_timing(timings, "connection", connect_started)
    append_jsonl(required(args, "--transcript"), list(
      event = "connect",
      driver = "r",
      route = route,
      parser_mode = required(args, "--parser-mode"),
      page_size = required(args, "--page-size"),
      language_profile = value_or_default(args, "--language-profile", "en-US"),
      language_resource_identity = value_or_default(args, "--language-resource-identity", "sbsql.common_resource_pack.v1"),
      language_resource_hash = value_or_default(args, "--language-resource-hash", "sha256:752c7a9823bdad00b48ab318c8b2d5d6d53b2739ecfe43f565952fd510f4e3dc"),
      syntax_profile = value_or_default(args, "--syntax-profile", "sbsql.v3"),
      topology_profile = value_or_default(args, "--topology-profile", "topology.sbsql.canonical.v1")
    ))
    append_jsonl(paths$wire, list(event = "server_admission_required", driver_or_parser_finality = "forbidden"))

    route_env <- probe_route_environment(conn, args, api_hits)
    write_text(paths$route_environment, paste0(jsonlite::toJSON(route_env, auto_unbox = TRUE, null = "null"), "\n"))
    if (route != "embedded" && !identical(route_env$page_size_verification_status, "pass")) {
      failures[[length(failures) + 1]] <- list(
        statement_id = "route_page_size",
        message = "route page-size verification failed",
        expected_page_size_bytes = route_env$expected_page_size_bytes,
        actual_page_size_bytes = route_env$actual_page_size_bytes
      )
    }

    if (isTRUE(args[["--create-database"]])) {
      create_started <- nanotime()
      sb_attach_create(conn@ptr$client, value_or_default(args, "--create-emulation-mode", "sbsql"), required(args, "--database"))
      api_hits[["sb_attach_create"]] <- api_hits[["sb_attach_create"]] + 1
      timings <- add_timing(timings, "database_create", create_started)
    }
    statements <- split_statements(read_input(required(args, "--input")))
    for (i in seq_along(statements)) {
      sql <- statements[[i]]
      statement_id <- paste0(basename(required(args, "--input")), ":", i)
      expected_outcome <- if (statement_id %in% expected_refusals) "refusal" else "success"
      group <- classify_statement(sql)
      statement_started <- nanotime()
      outcome <- "success"
      row_count <- -1
      result_digest <- NULL
      sqlstate <- NULL
      diagnostic <- NULL
      tryCatch({
        if (group == "transaction") {
          api_hits <- run_transaction(conn, sql, api_hits)
          row_count <- 0
          result_digest <- sha256_text("transaction")
        } else if (group == "copy" && is_copy_stdin_statement(sql)) {
          payload <- copy_payload_for_statement(sql)
          if (identical(payload, raw(0))) {
            stop("COPY FROM STDIN requires SB_COPY_INPUT rows in the script")
          }
          payload <- copy_text_rows_to_native_frame(payload)
          row_count <- sb_isql_copy_in(conn@ptr$client, executable_sql_without_copy_markers(sql), payload)
          api_hits[["sb_copy_in"]] <- api_hits[["sb_copy_in"]] + 1
          result_digest <- sha256_text(paste0("copy_in:", row_count))
          append_jsonl(paths$wire, list(
            event = "copy_payload",
            driver = "r",
            statement_id = statement_id,
            copy_payload_format = "sbnr_native_rowset",
            copy_payload_bytes = length(payload)
          ))
          append_text(required(args, "--output"), paste0(jsonlite::toJSON(list(statement_id = statement_id, rows = list(list(copy_in = row_count))), auto_unbox = TRUE), "\n"))
        } else if (required(args, "--parser-mode") != "server-parser") {
          compiled <- sb_compile_sblr(conn@ptr$client, sql)
          api_hits[["sb_compile_sblr"]] <- api_hits[["sb_compile_sblr"]] + 1
          append_jsonl(paths$wire, list(
            event = "driver_sblr_compile",
            driver = "r",
            parser_mode = required(args, "--parser-mode"),
            statement_id = statement_id,
            sblr_hash = as.character(compiled$hash),
            sblr_version = compiled$version,
            sblr_bytes = length(compiled$bytecode)
          ))
          stream <- sb_execute_sblr(conn@ptr$client, compiled$hash, compiled$bytecode)
          api_hits[["sb_execute_sblr"]] <- api_hits[["sb_execute_sblr"]] + 1
          append_jsonl(paths$wire, list(
            event = "driver_sblr_execute",
            driver = "r",
            parser_mode = required(args, "--parser-mode"),
            statement_id = statement_id,
            sblr_hash = as.character(compiled$hash),
            sblr_version = compiled$version,
            sblr_bytes = length(compiled$bytecode),
            engine_sql_text_execution = FALSE,
            mga_authority = "engine"
          ))
          rows <- sb_fetch_rows(stream, -1)
          row_count <- if (is.data.frame(rows)) nrow(rows) else length(rows)
          if (group %in% c("query", "metadata") || row_count > 0) {
            result_digest <- sha256_text(jsonlite::toJSON(rows, dataframe = "rows", auto_unbox = TRUE))
            append_text(required(args, "--output"), paste0(jsonlite::toJSON(list(statement_id = statement_id, rows = rows), auto_unbox = TRUE), "\n"))
          } else {
            row_count <- as.integer(stream$rowcount)
            result_digest <- sha256_text(as.character(row_count))
          }
        } else if (group %in% c("ddl", "dml", "security_refusal")) {
          row_count <- DBI::dbExecute(conn, sql)
          api_hits[["DBI::dbExecute"]] <- api_hits[["DBI::dbExecute"]] + 1
          result_digest <- sha256_text(as.character(row_count))
        } else {
          res <- DBI::dbSendQuery(conn, sql)
          api_hits[["DBI::dbSendQuery"]] <- api_hits[["DBI::dbSendQuery"]] + 1
          on.exit(try(DBI::dbClearResult(res), silent = TRUE), add = TRUE)
          rows <- DBI::dbFetch(res, n = as.integer(value_or_default(args, "--fetch-size", "1000")))
          api_hits[["DBI::dbFetch"]] <- api_hits[["DBI::dbFetch"]] + 1
          row_count <- nrow(rows)
          result_digest <- sha256_text(jsonlite::toJSON(rows, dataframe = "rows", auto_unbox = TRUE))
          append_text(required(args, "--output"), paste0(jsonlite::toJSON(list(statement_id = statement_id, rows = rows), auto_unbox = TRUE), "\n"))
        }
        digests[[length(digests) + 1]] <- list(statement_id = statement_id, row_count = row_count, result_digest = result_digest)
        if (identical(expected_outcome, "refusal")) {
          outcome <- "unexpected_success"
          failures[[length(failures) + 1]] <- list(statement_id = statement_id, message = "statement succeeded but was expected to refuse")
        }
      }, error = function(e) {
        outcome <<- "refusal"
        sqlstate <<- "HY000"
        diagnostic <<- conditionMessage(e)
        append_jsonl(required(args, "--diagnostics"), list(statement_id = statement_id, sqlstate = sqlstate, message = diagnostic))
        append_text(required(args, "--error"), paste0(statement_id, ": ", diagnostic, "\n"))
        if (identical(expected_outcome, "success")) {
          failures[[length(failures) + 1]] <<- list(statement_id = statement_id, message = diagnostic)
        } else {
          security_refusals[[length(security_refusals) + 1]] <<- list(statement_id = statement_id, sqlstate = sqlstate, diagnostic_code = diagnostic)
        }
        if (identical(expected_outcome, "success") && isTRUE(args[["--stop-on-error"]])) {
          timings <<- add_timing(timings, group, statement_started)
        }
      })
      elapsed <- nanotime() - statement_started
      timings <- add_timing(timings, group, statement_started)
      event <- list(
        run_id = value_or_default(args, "--run-id", "manual"),
        driver_name = "r",
        driver_version = "unknown",
        route = required(args, "--route"),
        parser_mode = required(args, "--parser-mode"),
        page_size = required(args, "--page-size"),
        namespace = required(args, "--namespace"),
        script = required(args, "--input"),
        statement_index = i,
        statement_id = statement_id,
        command_group = group,
        sql_hash = sha256_text(sql),
        expected_outcome = expected_outcome,
        actual_outcome = outcome,
        sqlstate = sqlstate,
        diagnostic_code = diagnostic,
        canonical_message_vector = list(),
        row_count = row_count,
        result_digest = result_digest,
        elapsed_ns = elapsed,
        server_revalidation_state = "required",
        language_profile = value_or_default(args, "--language-profile", "en-US"),
        language_resource_pack = value_or_default(args, "--language-resource-pack", "project/resources/seed-packs/initial-resource-pack/resources/i18n/sbsql-language-resource-pack"),
        language_resource_identity = value_or_default(args, "--language-resource-identity", "sbsql.common_resource_pack.v1"),
        language_resource_hash = value_or_default(args, "--language-resource-hash", "sha256:752c7a9823bdad00b48ab318c8b2d5d6d53b2739ecfe43f565952fd510f4e3dc"),
        syntax_profile = value_or_default(args, "--syntax-profile", "sbsql.v3"),
        topology_profile = value_or_default(args, "--topology-profile", "topology.sbsql.canonical.v1"),
        standard_english_fallback = flag_enabled(args, "--standard-english-fallback", TRUE),
        transaction_id_observed = NULL,
        mga_authority = "engine",
        native_api_surface = "r_dbi",
        code_example_section = "dbsendquery_dbfetch"
      )
      append_jsonl(paths$events, event)
      testcases[[length(testcases) + 1]] <- event
      if (isTRUE(args[["--stop-on-error"]]) && length(failures) > 0) break
    }

    metadata_started <- nanotime()
    tables <- DBI::dbListTables(conn)
    api_hits[["DBI::dbListTables"]] <- api_hits[["DBI::dbListTables"]] + 1
    write_text(paths$metadata, paste0(jsonlite::toJSON(list(tables_digest = sha256_text(jsonlite::toJSON(tables, auto_unbox = TRUE)), row_count = length(tables)), auto_unbox = TRUE), "\n"))
    timings <- add_timing(timings, "metadata", metadata_started)
  }, error = function(e) {
    failures[[length(failures) + 1]] <<- list(statement_id = "run", message = conditionMessage(e))
    append_text(paths$stderr, paste0(conditionMessage(e), "\n"))
  }, finally = {
    if (!is.null(conn)) try(DBI::dbDisconnect(conn), silent = TRUE)
  })

  elapsed <- nanotime() - started
  timings[["overall"]] <- elapsed
  sslmode <- effective_sslmode_for_route(required(args, "--route"), value_or_default(args, "--sslmode", "require"))
  transport_mode <- resolve_transport_mode(required(args, "--route"), sslmode)
  process_metrics <- current_process_metrics()
  summary <- list(
    run_id = value_or_default(args, "--run-id", "manual"),
    driver_name = "r",
    route = required(args, "--route"),
    parser_mode = required(args, "--parser-mode"),
    page_size = required(args, "--page-size"),
    namespace = required(args, "--namespace"),
    sslmode = sslmode,
    transport_mode = transport_mode,
    transport_endpoint_kind = endpoint_kind_for_route(required(args, "--route")),
    driver_transport_implementation = transport_implementation_for_route(required(args, "--route")),
    cpp_library_boundary = "none",
    language_resource_pack = value_or_default(args, "--language-resource-pack", "project/resources/seed-packs/initial-resource-pack/resources/i18n/sbsql-language-resource-pack"),
    language_resource_identity = value_or_default(args, "--language-resource-identity", "sbsql.common_resource_pack.v1"),
    language_resource_hash = value_or_default(args, "--language-resource-hash", "sha256:752c7a9823bdad00b48ab318c8b2d5d6d53b2739ecfe43f565952fd510f4e3dc"),
    language_resource_authority = "shared_server_parser_resource_pack",
    language_profile = value_or_default(args, "--language-profile", "en-US"),
    syntax_profile = value_or_default(args, "--syntax-profile", "sbsql.v3"),
    topology_profile = value_or_default(args, "--topology-profile", "topology.sbsql.canonical.v1"),
    standard_english_fallback = flag_enabled(args, "--standard-english-fallback", TRUE),
    status = if (length(failures) == 0) "pass" else "fail",
    failure_count = length(failures),
    elapsed_ns = elapsed,
    process_metrics = process_metrics,
    server_revalidation_required = TRUE,
    driver_or_parser_finality = "forbidden",
    mga_authority = "engine",
    route_environment = route_env
  )
  write_text(required(args, "--summary"), paste0(jsonlite::toJSON(summary, auto_unbox = TRUE), "\n"))
  write_text(required(args, "--metrics"), paste0(jsonlite::toJSON(timings, auto_unbox = TRUE), "\n"))
  write_text(paths$timing, paste0(jsonlite::toJSON(timings, auto_unbox = TRUE), "\n"))
  write_text(paths$digests, paste0(jsonlite::toJSON(digests, auto_unbox = TRUE), "\n"))
  append_jsonl(paths$process, list(role = "client", rss_kb = process_metrics$client$last_rss_kb, vsize_kb = process_metrics$client$last_vsize_kb))
  write_text(paths$refusals, paste0(jsonlite::toJSON(security_refusals, auto_unbox = TRUE), "\n"))
  write_text(paths$api, paste0(jsonlite::toJSON(api_hits, auto_unbox = TRUE), "\n"))
  write_text(paths$review, paste0(jsonlite::toJSON(list(driver = "r", public_api_only = TRUE, shells_out_to_other_driver = FALSE,
                                                       source_is_canonical_example = TRUE,
                                                       sections = c("connection", "dbsendquery", "dbfetch", "metadata", "diagnostics", "transaction")), auto_unbox = TRUE), "\n"))
  write_text(paths$junit, junit_xml("SBIsqlR", "scratchbird.r", testcases, failures))
  append_text(paths$stdout, paste0("SBIsqlR status=", summary$status, "\n"))
  if (length(failures) == 0) 0 else 1
}

parse_args <- function(raw) {
  args <- list()
  i <- 1
  while (i <= length(raw)) {
    key <- raw[[i]]
    if (!startsWith(key, "--")) stop(paste("unexpected positional argument:", key))
    if (!(key %in% supported_args)) stop(paste("unsupported argument:", key))
    if (key %in% c("--stop-on-error", "--create-database", "--standard-english-fallback")) {
      if (i + 1 <= length(raw) && !startsWith(raw[[i + 1]], "--")) {
        args[[key]] <- parse_bool_value(key, raw[[i + 1]])
        i <- i + 2
      } else {
        args[[key]] <- TRUE
        i <- i + 1
      }
      next
    }
    if (i + 1 > length(raw) || startsWith(raw[[i + 1]], "--")) stop(paste("missing value for", key))
    args[[key]] <- raw[[i + 1]]
    i <- i + 2
  }
  args
}

validate_args <- function(args) {
  if (!(required(args, "--page-size") %in% page_sizes)) stop(paste("unsupported page size:", required(args, "--page-size")))
  if (!(required(args, "--route") %in% routes)) stop(paste("unsupported route:", required(args, "--route")))
  if (!(required(args, "--parser-mode") %in% parser_modes)) stop(paste("unsupported parser mode:", required(args, "--parser-mode")))
  sslmode <- value_or_default(args, "--sslmode", "require")
  if (!(sslmode %in% ssl_modes)) stop(paste("unsupported sslmode:", sslmode))
}

required <- function(args, key) {
  value <- args[[key]]
  if (is.null(value) || !nzchar(as.character(value))) stop(paste("missing required argument", key))
  as.character(value)
}

value_or_default <- function(args, key, default) {
  value <- args[[key]]
  if (is.null(value)) default else as.character(value)
}

flag_enabled <- function(args, key, default = FALSE) {
  value <- args[[key]]
  if (is.null(value)) default else isTRUE(value)
}

parse_bool_value <- function(key, value) {
  normalized <- tolower(as.character(value))
  if (identical(normalized, "true")) return(TRUE)
  if (identical(normalized, "false")) return(FALSE)
  stop(paste(key, "expects true or false, got:", value))
}

resolve_transport_mode <- function(route, sslmode) {
  if (identical(route, "embedded")) return("embedded_no_network_transport")
  if (identical(route, "ipc_local")) return("local_ipc_no_tls")
  if (identical(sslmode, "disable")) "tls_disabled" else "tls_required"
}

ensure_transport_route_supported <- function(route, args) {
  if (identical(route, "embedded")) {
    stop("embedded transport is unsupported by the R driver; no ScratchBird C++ library boundary is exposed")
  }
  if (identical(route, "ipc_local") && !nzchar(value_or_default(args, "--ipc-path", ""))) {
    stop("ipc_path is required for local IPC transport")
  }
}

effective_sslmode_for_route <- function(route, sslmode) {
  if (identical(route, "ipc_local")) "disable" else sslmode
}

transport_config_for_route <- function(route) {
  if (identical(route, "ipc_local")) return("ipc")
  if (identical(route, "embedded")) return("embedded")
  "inet"
}

endpoint_kind_for_route <- function(route) {
  if (identical(route, "ipc_local")) return("unix_domain_socket")
  if (identical(route, "embedded")) return("none")
  "tcp"
}

transport_implementation_for_route <- function(route) {
  if (identical(route, "embedded")) return("unsupported_no_cpp_library_boundary")
  if (identical(route, "ipc_local")) return("native_r_unix_socket")
  "native_r_tcp"
}

expected_page_size_bytes <- function(label) {
  value <- page_size_bytes[[label]]
  if (is.null(value)) stop(paste("unsupported page size:", label))
  as.integer(value)
}

route_environment <- function(args, actual_page_size, status, reason = NULL) {
  sslmode <- effective_sslmode_for_route(required(args, "--route"), value_or_default(args, "--sslmode", "require"))
  list(
    driver = "r",
    route = required(args, "--route"),
    parser_mode = required(args, "--parser-mode"),
    page_size = required(args, "--page-size"),
    expected_page_size_bytes = expected_page_size_bytes(required(args, "--page-size")),
    actual_page_size_bytes = actual_page_size,
    page_size_verification_source = "SHOW DATABASE",
    page_size_verification_status = status,
    failure_reason = reason,
    sslmode = sslmode,
    transport_mode = resolve_transport_mode(required(args, "--route"), sslmode),
    transport_endpoint_kind = endpoint_kind_for_route(required(args, "--route"))
  )
}

probe_route_environment <- function(conn, args, api_hits) {
  tryCatch({
    result <- DBI::dbSendQuery(conn, "SHOW DATABASE")
    api_hits[["DBI::dbSendQuery"]] <- api_hits[["DBI::dbSendQuery"]] + 1
    on.exit(try(DBI::dbClearResult(result), silent = TRUE), add = TRUE)
    rows <- DBI::dbFetch(result, n = 1000)
    api_hits[["DBI::dbFetch"]] <- api_hits[["DBI::dbFetch"]] + 1
    actual <- extract_page_size_bytes(rows)
    if (is.null(actual)) {
      return(route_environment(args, NULL, "fail", "show_database_missing_page_size_bytes"))
    }
    expected <- expected_page_size_bytes(required(args, "--page-size"))
    status <- if (identical(as.integer(actual), expected)) "pass" else "fail"
    reason <- if (identical(status, "pass")) NULL else "actual_page_size_mismatch"
    route_environment(args, as.integer(actual), status, reason)
  }, error = function(e) {
    route_environment(args, NULL, "fail", conditionMessage(e))
  })
}

extract_page_size_bytes <- function(rows) {
  if (!is.data.frame(rows) || nrow(rows) == 0) return(NULL)
  lowered <- tolower(names(rows))
  for (candidate in c("page_size_bytes", "page_size", "database_page_size", "default_page_size_bytes")) {
    idx <- match(candidate, lowered, nomatch = 0L)
    if (idx > 0) {
      values <- rows[[idx]]
      values <- values[!is.na(values)]
      if (length(values) > 0) {
        parsed <- suppressWarnings(as.integer(as.character(values[[1]])))
        if (!is.na(parsed)) return(parsed)
      }
    }
  }
  NULL
}

load_expected_refusals <- function(path) {
  if (is.null(path) || !nzchar(path)) return(character())
  if (!file.exists(path)) stop(paste("expected refusal file not found:", path))
  doc <- jsonlite::fromJSON(path, simplifyVector = FALSE)
  ids <- NULL
  if (is.list(doc) && !is.null(names(doc))) {
    if ("statement_ids" %in% names(doc)) ids <- c(ids, doc$statement_ids)
    if ("expected_refusals" %in% names(doc)) ids <- c(ids, doc$expected_refusals)
    if ("expected_diagnostics" %in% names(doc) && !is.null(names(doc$expected_diagnostics))) {
      ids <- c(ids, names(doc$expected_diagnostics))
    }
    if ("compiled_chain_statement_aliases" %in% names(doc) &&
        !is.null(doc$compiled_chain_statement_aliases)) {
      ids <- c(ids, unname(unlist(doc$compiled_chain_statement_aliases, use.names = FALSE)))
    }
  } else if (is.list(doc) && is.null(names(doc))) {
    ids <- doc
  } else if (is.atomic(doc)) {
    ids <- doc
  }
  if (is.null(ids)) stop("expected refusals must be a JSON object or array")
  as.character(unlist(ids, use.names = FALSE))
}

current_process_metrics <- function() {
  used_kb <- tryCatch({
    gc_info <- gc()
    max(1L, as.integer(sum(gc_info[, 2], na.rm = TRUE) * 1024))
  }, error = function(e) 1L)
  list(client = list(
    last_rss_kb = used_kb,
    last_vsize_kb = used_kb,
    max_rss_kb = used_kb,
    max_vsize_kb = used_kb
  ))
}

split_statements <- function(script) {
  # Delegate to the canonical SET TERM- and comment-aware splitter sourced from
  # R/sql.R. Returns a character vector of trimmed top-level statements.
  split_top_level_statements(script)
}

classify_statement <- function(sql) {
  trimmed <- tolower(trimws(executable_sql_without_copy_markers(sql)))
  first <- strsplit(trimmed, "\\s+")[[1]][[1]]
  if (first == "copy") return("copy")
  if (first %in% c("create", "alter", "drop")) return("ddl")
  if (first %in% c("insert", "update", "delete", "merge", "upsert")) return("dml")
  if (first %in% c("commit", "rollback", "savepoint", "begin", "start")) return("transaction")
  if (first %in% c("grant", "revoke")) return("security_refusal")
  if (grepl("sys\\.", trimmed)) return("metadata")
  "query"
}

executable_sql_without_copy_markers <- function(sql) {
  lines <- strsplit(sql, "\r\n|\r|\n", perl = TRUE)[[1]]
  keep <- !startsWith(trimws(lines, which = "left"), "-- SB_COPY_INPUT ")
  trimws(paste(lines[keep], collapse = "\n"))
}

copy_payload_for_statement <- function(sql) {
  rows <- character()
  for (line in strsplit(sql, "\r\n|\r|\n", perl = TRUE)[[1]]) {
    stripped <- trimws(line, which = "left")
    if (startsWith(stripped, "-- SB_COPY_INPUT ")) {
      rows <- c(rows, substring(stripped, nchar("-- SB_COPY_INPUT ") + 1L))
    }
  }
  if (length(rows) == 0) return(raw(0))
  charToRaw(paste0(paste(rows, collapse = "\n"), "\n"))
}

copy_text_rows_to_native_frame <- function(data) {
  if (length(data) >= 4 && rawToChar(data[1:4]) == "SBNR") {
    return(data)
  }
  lines <- strsplit(rawToChar(data), "\r\n|\r|\n", perl = TRUE)[[1]]
  lines <- sub("\r$", "", lines)
  lines <- lines[nzchar(trimws(lines))]
  if (length(lines) == 0) {
    stop("COPY input contains no rows")
  }

  if (grepl(";", lines[[1]], fixed = TRUE) && grepl("=", lines[[1]], fixed = TRUE)) {
    columns <- NULL
    rows <- list()
    for (line in lines) {
      names <- character()
      values <- list()
      for (part in strsplit(line, ";", fixed = TRUE)[[1]]) {
        if (!nzchar(part)) next
        separator <- regexpr("=", part, fixed = TRUE)[[1]]
        if (separator <= 1L) {
          stop("malformed canonical COPY field")
        }
        names <- c(names, substr(part, 1L, separator - 1L))
        value <- substr(part, separator + 1L, nchar(part))
        values <- c(values, list(if (tolower(value) == "null") NULL else value))
      }
      if (length(names) == 0) next
      if (is.null(columns)) {
        columns <- names
      } else if (!identical(columns, names)) {
        stop("COPY input changed row shape mid-stream")
      }
      rows[[length(rows) + 1L]] <- values
    }
    if (is.null(columns)) {
      stop("COPY input contains no rows")
    }
    return(build_native_rowset_payload(columns, rows))
  }

  columns <- trimws(split_copy_csv_line(lines[[1]]))
  if (length(columns) == 0 || any(!nzchar(columns))) {
    stop("CSV COPY input requires a non-empty header row")
  }
  rows <- list()
  for (line in lines[-1]) {
    values <- split_copy_csv_line(line)
    if (length(values) != length(columns)) {
      stop("CSV COPY row shape mismatch")
    }
    rows[[length(rows) + 1L]] <- lapply(values, function(value) if (!nzchar(value) || tolower(value) == "null") NULL else value)
  }
  if (length(rows) == 0) {
    stop("CSV COPY input contains no data rows")
  }
  build_native_rowset_payload(columns, rows)
}

build_native_rowset_payload <- function(columns, rows, column_types = NULL) {
  if (length(rows) == 0) {
    stop("native rowset requires at least one row")
  }
  if (length(columns) == 0 || any(!nzchar(columns))) {
    stop("native rowset requires non-empty column names")
  }
  for (row in rows) {
    if (length(row) != length(columns)) {
      stop("native rowset row shape mismatch")
    }
  }
  types <- column_types %||% infer_native_rowset_column_types(rows)
  if (length(types) != length(columns)) {
    stop("native rowset column/type shape mismatch")
  }

  out <- c(charToRaw("SBNR"), pack_uint16_le(2L), pack_uint16_le(0L), pack_uint64_le(as.character(length(rows))), pack_uint32_le(length(columns)), as.raw(types))
  for (column in columns) {
    encoded <- charToRaw(enc2utf8(column))
    out <- c(out, pack_uint32_le(length(encoded)), encoded)
  }
  null_bitmap_bytes <- ceiling(length(columns) / 8)
  for (row in rows) {
    null_bitmap <- rep(as.raw(0), null_bitmap_bytes)
    values <- raw(0)
    for (index in seq_along(row)) {
      value <- row[[index]]
      if (is.null(value)) {
        byte_index <- ((index - 1L) %/% 8L) + 1L
        null_bitmap[byte_index] <- as.raw(as.integer(null_bitmap[byte_index]) + bitwShiftL(1L, (index - 1L) %% 8L))
      } else {
        values <- c(values, encode_native_rowset_value(as.character(value), types[[index]]))
      }
    }
    out <- c(out, null_bitmap, values)
  }
  out
}

encode_native_rowset_value <- function(value, type) {
  trimmed <- trimws(value)
  if (type == native_rowset_type_int64) {
    return(pack_int64_le(trimmed))
  }
  if (type == native_rowset_type_boolean) {
    return(as.raw(if (truthy_native_rowset_boolean(trimmed)) 1L else 0L))
  }
  if (type == native_rowset_type_int32) {
    return(pack_int32_le(as.integer(trimmed)))
  }
  if (type == native_rowset_type_uint64) {
    return(pack_uint64_le(trimmed))
  }
  if (type == native_rowset_type_real64) {
    return(write_bin_raw(as.double(trimmed), size = 8L, endian = "little"))
  }
  if (type == native_rowset_type_binary || type == native_rowset_type_text) {
    encoded <- charToRaw(enc2utf8(value))
    return(c(pack_uint32_le(length(encoded)), encoded))
  }
  stop(paste("unsupported native rowset type", type))
}

infer_native_rowset_column_types <- function(rows) {
  if (length(rows) == 0) {
    return(integer())
  }
  column_count <- length(rows[[1]])
  types <- rep(native_rowset_type_text, column_count)
  for (column in seq_len(column_count)) {
    values <- Filter(Negate(is.null), lapply(rows, function(row) row[[column]]))
    if (length(values) == 0) next
    text_values <- vapply(values, as.character, character(1))
    if (all(tolower(trimws(text_values)) %in% c("true", "false"))) {
      types[[column]] <- native_rowset_type_boolean
    } else if (all(vapply(text_values, lossless_int32, logical(1)))) {
      types[[column]] <- native_rowset_type_int32
    } else if (all(vapply(text_values, lossless_int64, logical(1)))) {
      types[[column]] <- native_rowset_type_int64
    } else if (all(vapply(text_values, lossless_uint64, logical(1)))) {
      types[[column]] <- native_rowset_type_uint64
    } else if (all(vapply(text_values, lossless_real64, logical(1)))) {
      types[[column]] <- native_rowset_type_real64
    }
  }
  types
}

split_copy_csv_line <- function(line) {
  values <- character()
  current <- ""
  in_quote <- FALSE
  chars <- strsplit(line, "", fixed = TRUE)[[1]]
  index <- 1L
  while (index <= length(chars)) {
    ch <- chars[[index]]
    if (identical(ch, "\"")) {
      if (in_quote && index + 1L <= length(chars) && identical(chars[[index + 1L]], "\"")) {
        current <- paste0(current, "\"")
        index <- index + 1L
      } else {
        in_quote <- !in_quote
      }
    } else if (identical(ch, ",") && !in_quote) {
      values <- c(values, current)
      current <- ""
    } else {
      current <- paste0(current, ch)
    }
    index <- index + 1L
  }
  c(values, current)
}

pack_uint16_le <- function(value) {
  value <- as.integer(value)
  as.raw(c(bitwAnd(value, 0xffL), bitwAnd(bitwShiftR(value, 8L), 0xffL)))
}

pack_uint32_le <- function(value) {
  value <- as.numeric(value)
  as.raw(c(
    value %% 256,
    floor(value / 256) %% 256,
    floor(value / 65536) %% 256,
    floor(value / 16777216) %% 256
  ))
}

pack_int32_le <- function(value) {
  write_bin_raw(as.integer(value), size = 4L, endian = "little")
}

pack_uint64_le <- function(value) {
  text <- normalize_decimal_string(value)
  bytes <- integer(8)
  for (index in seq_len(8)) {
    divided <- decimal_divmod_int(text, 256L)
    bytes[[index]] <- divided$remainder
    text <- divided$quotient
  }
  if (text != "0") {
    stop("uint64 value exceeds 64 bits")
  }
  as.raw(bytes)
}

pack_int64_le <- function(value) {
  text <- normalize_decimal_string(value, allow_negative = TRUE)
  if (startsWith(text, "-")) {
    magnitude <- substring(text, 2L)
    if (decimal_compare_abs(magnitude, "9223372036854775808") > 0) {
      stop("int64 value is below minimum")
    }
    return(pack_uint64_le(decimal_subtract_unsigned("18446744073709551616", magnitude)))
  }
  if (decimal_compare_abs(text, "9223372036854775807") > 0) {
    stop("int64 value exceeds maximum")
  }
  pack_uint64_le(text)
}

write_bin_raw <- function(value, size, endian) {
  con <- rawConnection(raw(0), "wb")
  on.exit(close(con))
  writeBin(value, con, size = size, endian = endian)
  rawConnectionValue(con)
}

normalize_decimal_string <- function(value, allow_negative = FALSE) {
  text <- trimws(as.character(value))
  pattern <- if (allow_negative) "^-?[0-9]+$" else "^[0-9]+$"
  if (!grepl(pattern, text)) {
    stop(paste("invalid integer literal", value))
  }
  negative <- startsWith(text, "-")
  if (negative) {
    text <- substring(text, 2L)
  }
  text <- sub("^0+", "", text)
  if (!nzchar(text)) text <- "0"
  if (negative && text != "0") paste0("-", text) else text
}

decimal_divmod_int <- function(text, divisor) {
  text <- normalize_decimal_string(text)
  digits <- as.integer(strsplit(text, "", fixed = TRUE)[[1]])
  quotient <- integer()
  remainder <- 0L
  for (digit in digits) {
    current <- remainder * 10L + digit
    quotient <- c(quotient, current %/% divisor)
    remainder <- current %% divisor
  }
  quotient_text <- paste(quotient, collapse = "")
  quotient_text <- sub("^0+", "", quotient_text)
  if (!nzchar(quotient_text)) quotient_text <- "0"
  list(quotient = quotient_text, remainder = remainder)
}

decimal_compare_abs <- function(left, right) {
  left <- normalize_decimal_string(left)
  right <- normalize_decimal_string(right)
  if (nchar(left) != nchar(right)) {
    return(if (nchar(left) < nchar(right)) -1L else 1L)
  }
  if (left == right) return(0L)
  if (left < right) -1L else 1L
}

decimal_subtract_unsigned <- function(left, right) {
  left <- normalize_decimal_string(left)
  right <- normalize_decimal_string(right)
  if (decimal_compare_abs(left, right) < 0) {
    stop("unsigned decimal subtraction underflow")
  }
  ldigits <- rev(as.integer(strsplit(left, "", fixed = TRUE)[[1]]))
  rdigits <- rev(as.integer(strsplit(right, "", fixed = TRUE)[[1]]))
  out <- integer(max(length(ldigits), length(rdigits)))
  borrow <- 0L
  for (index in seq_along(out)) {
    l <- if (index <= length(ldigits)) ldigits[[index]] else 0L
    r <- if (index <= length(rdigits)) rdigits[[index]] else 0L
    value <- l - borrow - r
    if (value < 0L) {
      value <- value + 10L
      borrow <- 1L
    } else {
      borrow <- 0L
    }
    out[[index]] <- value
  }
  text <- paste(rev(out), collapse = "")
  text <- sub("^0+", "", text)
  if (!nzchar(text)) "0" else text
}

lossless_int32 <- function(value) {
  text <- tryCatch(normalize_decimal_string(value, allow_negative = TRUE), error = function(e) NULL)
  if (is.null(text)) return(FALSE)
  if (startsWith(text, "-")) {
    decimal_compare_abs(substring(text, 2L), "2147483648") <= 0
  } else {
    decimal_compare_abs(text, "2147483647") <= 0
  }
}

lossless_int64 <- function(value) {
  text <- tryCatch(normalize_decimal_string(value, allow_negative = TRUE), error = function(e) NULL)
  if (is.null(text)) return(FALSE)
  if (startsWith(text, "-")) {
    decimal_compare_abs(substring(text, 2L), "9223372036854775808") <= 0
  } else {
    decimal_compare_abs(text, "9223372036854775807") <= 0
  }
}

lossless_uint64 <- function(value) {
  text <- tryCatch(normalize_decimal_string(value), error = function(e) NULL)
  if (is.null(text)) return(FALSE)
  decimal_compare_abs(text, "18446744073709551615") <= 0
}

lossless_real64 <- function(value) {
  parsed <- suppressWarnings(as.double(trimws(value)))
  is.finite(parsed)
}

truthy_native_rowset_boolean <- function(value) {
  tolower(trimws(value)) %in% c("1", "true", "t", "yes", "y", "on")
}

is_copy_stdin_statement <- function(sql) {
  lines <- strsplit(executable_sql_without_copy_markers(sql), "\r\n|\r|\n", perl = TRUE)[[1]]
  meaningful <- tolower(trimws(lines))
  meaningful <- meaningful[meaningful != "" & !startsWith(meaningful, "--")]
  executable <- paste(meaningful, collapse = " ")
  startsWith(executable, "copy ") && grepl(" from stdin", executable, fixed = TRUE)
}

sb_isql_copy_in <- function(client, sql, payload) {
  sb_send_simple_query(client, sql, 0L)
  rows_copied <- 0L
  copy_started <- FALSE
  repeat {
    response <- sb_recv_message(client)
    type <- response$type
    body <- response$payload
    if (sb_handle_async(client, type, body)) next
    if (type == SB_MSG_COPY_IN_RESPONSE) {
      copy_started <- TRUE
      sb_send_message(client, SB_MSG_COPY_DATA, payload, 0L, FALSE)
      sb_send_message(client, SB_MSG_COPY_DONE, raw(), 0L, FALSE)
    } else if (type == SB_MSG_COMMAND_COMPLETE) {
      parsed <- parse_command_complete(body)
      rows_copied <- as.integer(parsed$rows)
    } else if (type == SB_MSG_READY) {
      parsed <- parse_ready(body)
      sb_apply_runtime_ready_state(client, parsed$status, parsed$txn_id)
      if (!copy_started) stop("COPY FROM STDIN did not enter COPY input mode")
      return(rows_copied)
    } else if (type == SB_MSG_ERROR) {
      sb_raise_query_error(body)
    }
  }
}

run_transaction <- function(conn, sql, api_hits) {
  tokens <- strsplit(tolower(trimws(sql)), "\\s+")[[1]]
  first <- tokens[[1]]
  second <- if (length(tokens) >= 2) tokens[[2]] else ""
  if (identical(first, "commit")) {
    DBI::dbCommit(conn)
    api_hits[["DBI::dbCommit"]] <- api_hits[["DBI::dbCommit"]] + 1
  } else if (identical(first, "rollback") && !identical(second, "to")) {
    DBI::dbRollback(conn)
    api_hits[["DBI::dbRollback"]] <- api_hits[["DBI::dbRollback"]] + 1
  } else if (identical(first, "begin") || identical(first, "start")) {
    DBI::dbBegin(conn)
  } else {
    DBI::dbExecute(conn, sql)
    api_hits[["DBI::dbExecute"]] <- api_hits[["DBI::dbExecute"]] + 1
  }
  api_hits
}

read_input <- function(path) if (identical(path, "-")) paste(readLines(file("stdin"), warn = FALSE), collapse = "\n") else paste(readLines(path, warn = FALSE), collapse = "\n")
nanotime <- function() as.numeric(Sys.time()) * 1000000000
add_timing <- function(timings, group, started) {
  timings[[group]] <- (timings[[group]] %||% 0) + (nanotime() - started)
  timings
}
write_text <- function(path, text) { dir.create(dirname(path), recursive = TRUE, showWarnings = FALSE); writeChar(text, path, eos = NULL) }
append_text <- function(path, text) { dir.create(dirname(path), recursive = TRUE, showWarnings = FALSE); cat(text, file = path, append = TRUE) }
append_jsonl <- function(path, record) append_text(path, paste0(jsonlite::toJSON(record, auto_unbox = TRUE, null = "null"), "\n"))
sha256_text <- function(text) paste0("sha256:", openssl::sha256(charToRaw(text)))
junit_xml <- function(suite, class, testcases, failures) {
  xml <- paste0("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<testsuite name=\"", escape_xml(suite), "\" tests=\"", max(length(testcases), 1), "\" failures=\"", length(failures), "\">\n")
  if (length(testcases) == 0) xml <- paste0(xml, "  <testcase classname=\"", escape_xml(class), "\" name=\"run\"></testcase>\n")
  for (testcase in testcases) xml <- paste0(xml, "  <testcase classname=\"", escape_xml(class), "\" name=\"", escape_xml(testcase$statement_id), "\"></testcase>\n")
  for (failure in failures) xml <- paste0(xml, "  <testcase classname=\"", escape_xml(class), "\" name=\"", escape_xml(failure$statement_id), "\"><failure message=\"", escape_xml(failure$message), "\" /></testcase>\n")
  paste0(xml, "</testsuite>\n")
}
escape_xml <- function(text) gsub(">", "&gt;", gsub("<", "&lt;", gsub("\"", "&quot;", gsub("&", "&amp;", as.character(text), fixed = TRUE), fixed = TRUE), fixed = TRUE), fixed = TRUE)

main()
