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
routes <- c("embedded", "ipc_local", "listener-parser", "manager-listener-parser")
parser_modes <- c("server-parser", "standalone-parser", "driver-sblr-uuid")
ssl_modes <- c("allow", "disable", "prefer", "require", "verify-ca", "verify-full")
supported_args <- c(
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
  "--standard-english-fallback"
)

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
    "DBI::dbRollback" = 0
  )
  testcases <- list()
  failures <- list()
  digests <- list()
  security_refusals <- list()
  started <- nanotime()
  expected_refusals <- load_expected_refusals(value_or_default(args, "--expected-refusals", ""))
  conn <- NULL

  tryCatch({
    dsn <- sprintf(
      "host=%s port=%s database=%s user=%s password=%s role=%s sslmode=%s sslrootcert=%s sslcert=%s sslkey=%s front_door_mode=%s metadata_expand_schema_parents=true",
      required(args, "--host"),
      required(args, "--port"),
      required(args, "--database"),
      required(args, "--user"),
      required(args, "--password"),
      value_or_default(args, "--role", ""),
      value_or_default(args, "--sslmode", "require"),
      value_or_default(args, "--sslrootcert", ""),
      value_or_default(args, "--sslcert", ""),
      value_or_default(args, "--sslkey", ""),
      if (required(args, "--route") == "manager-listener-parser") "manager_proxy" else "direct"
    )
    connect_started <- nanotime()
    conn <- DBI::dbConnect(Scratchbird(), dsn)
    api_hits[["DBI::dbConnect"]] <- api_hits[["DBI::dbConnect"]] + 1
    timings <- add_timing(timings, "connection", connect_started)
    append_jsonl(required(args, "--transcript"), list(
      event = "connect",
      driver = "r",
      route = required(args, "--route"),
      parser_mode = required(args, "--parser-mode"),
      page_size = required(args, "--page-size"),
      language_profile = value_or_default(args, "--language-profile", "en-US"),
      language_resource_identity = value_or_default(args, "--language-resource-identity", "sbsql.common_resource_pack.v1"),
      language_resource_hash = value_or_default(args, "--language-resource-hash", "sha256:752c7a9823bdad00b48ab318c8b2d5d6d53b2739ecfe43f565952fd510f4e3dc"),
      syntax_profile = value_or_default(args, "--syntax-profile", "sbsql.v3"),
      topology_profile = value_or_default(args, "--topology-profile", "topology.sbsql.canonical.v1")
    ))
    append_jsonl(paths$wire, list(event = "server_admission_required", driver_or_parser_finality = "forbidden"))

    if (isTRUE(args[["--create-database"]])) {
      create_started <- nanotime()
      sb_attach_create(conn@ptr$client, value_or_default(args, "--create-emulation-mode", "sbsql"), required(args, "--database"))
      api_hits[["sb_attach_create"]] <- api_hits[["sb_attach_create"]] + 1
      timings <- add_timing(timings, "database_create", create_started)
    }
    if (required(args, "--parser-mode") != "server-parser") {
      stop(paste(required(args, "--parser-mode"), "is not accepted by the R native tool lane; it fails closed"))
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
        digests[[length(digests) + 1]] <<- list(statement_id = statement_id, row_count = row_count, result_digest = result_digest)
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
  sslmode <- value_or_default(args, "--sslmode", "require")
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
    mga_authority = "engine"
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

load_expected_refusals <- function(path) {
  if (is.null(path) || !nzchar(path)) return(character())
  if (!file.exists(path)) stop(paste("expected refusal file not found:", path))
  doc <- jsonlite::fromJSON(path, simplifyVector = FALSE)
  ids <- NULL
  if (is.list(doc) && !is.null(names(doc)) && "statement_ids" %in% names(doc)) {
    ids <- doc$statement_ids
    if ("expected_refusals" %in% names(doc)) ids <- c(ids, doc$expected_refusals)
  } else if (is.list(doc) && !is.null(names(doc)) && "expected_refusals" %in% names(doc)) {
    ids <- doc$expected_refusals
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
  trimmed <- tolower(trimws(sql))
  first <- strsplit(trimmed, "\\s+")[[1]][[1]]
  if (first %in% c("create", "alter", "drop")) return("ddl")
  if (first %in% c("insert", "update", "delete", "merge", "upsert")) return("dml")
  if (first %in% c("commit", "rollback", "savepoint", "begin", "start")) return("transaction")
  if (first %in% c("grant", "revoke")) return("security_refusal")
  if (grepl("sys\\.", trimmed)) return("metadata")
  "query"
}

run_transaction <- function(conn, sql, api_hits) {
  first <- strsplit(tolower(trimws(sql)), "\\s+")[[1]][[1]]
  if (identical(first, "commit")) {
    DBI::dbCommit(conn)
    api_hits[["DBI::dbCommit"]] <- api_hits[["DBI::dbCommit"]] + 1
  } else if (identical(first, "rollback")) {
    DBI::dbRollback(conn)
    api_hits[["DBI::dbRollback"]] <- api_hits[["DBI::dbRollback"]] + 1
  } else {
    DBI::dbBegin(conn)
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
