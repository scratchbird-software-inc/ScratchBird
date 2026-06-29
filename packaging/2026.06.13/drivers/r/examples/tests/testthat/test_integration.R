# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

integration_dsn <- function() {
  dsn <- trimws(Sys.getenv("SCRATCHBIRD_R_URL"))
  if (dsn == "") {
    skip("SCRATCHBIRD_R_URL not set")
  }
  dsn
}

integration_manager_dsn <- function() {
  dsn <- trimws(Sys.getenv("SCRATCHBIRD_R_MANAGER_URL"))
  if (dsn == "") {
    skip("SCRATCHBIRD_R_MANAGER_URL not set")
  }
  cfg <- sb_config(dsn)
  if (!identical(cfg$front_door_mode, "manager_proxy")) {
    skip("SCRATCHBIRD_R_MANAGER_URL must include front_door_mode=manager_proxy")
  }
  dsn
}

with_integration_client <- function(callback) {
  client <- sb_connect(integration_dsn())
  on.exit(sb_disconnect(client), add = TRUE)
  callback(client)
}

with_manager_integration_client <- function(callback) {
  client <- sb_connect(integration_manager_dsn())
  on.exit(sb_disconnect(client), add = TRUE)
  callback(client)
}

with_integration_connection <- function(callback) {
  conn <- DBI::dbConnect(Scratchbird(), integration_dsn())
  on.exit(DBI::dbDisconnect(conn), add = TRUE)
  callback(conn)
}

expected_integration_database_name <- function(client) {
  database <- trimws(client$cfg$database)
  if (database == "") {
    return("default")
  }
  database
}

ensure_type_coverage_fixture <- function(client) {
  scratchbird:::sb_query(client, "DROP TABLE IF EXISTS type_coverage")
  scratchbird:::sb_query(client, "CREATE TABLE type_coverage (id INTEGER, note VARCHAR(32))")
  scratchbird:::sb_query(client, "INSERT INTO type_coverage VALUES (1, 'baseline')")
}

ensure_fetch_fixture <- function(client) {
  scratchbird:::sb_query(client, "DROP TABLE IF EXISTS r_fetch_fixture")
  scratchbird:::sb_query(client, "CREATE TABLE r_fetch_fixture (value INTEGER)")
  scratchbird:::sb_query(client, "INSERT INTO r_fetch_fixture VALUES (11), (22), (33)")
}

integration_cancel_query <- function() {
  cancel_sql <- trimws(Sys.getenv("SCRATCHBIRD_R_CANCEL_SQL"))
  if (cancel_sql == "" || identical(cancel_sql, "SELECT pg_sleep(5)")) {
    return(list(
      sql = paste(
        "SELECT a.value",
        "FROM r_fetch_fixture a",
        "CROSS JOIN r_fetch_fixture b",
        "CROSS JOIN r_fetch_fixture c",
        "CROSS JOIN r_fetch_fixture d",
        "CROSS JOIN r_fetch_fixture e",
        "WHERE a.value >= ?::INTEGER"
      ),
      params = list(0L)
    ))
  }
  list(sql = cancel_sql, params = NULL)
}

test_that("integration query", {
  with_integration_client(function(client) {
    result <- sb_query(client, "SELECT 1")
    expect_true(length(result$rows) > 0)
    expect_true(length(result$rows[[1]]) > 0)
    expect_equal(as.integer(result$rows[[1]][[1]]), 1L)
  })
})

test_that("integration manager-proxy connect and basic query", {
  with_manager_integration_client(function(client) {
    expect_identical(client$cfg$front_door_mode, "manager_proxy")
    result <- sb_query(client, "SELECT 1")
    expect_true(length(result$rows) > 0)
    expect_true(length(result$rows[[1]]) > 0)
    expect_equal(as.integer(result$rows[[1]][[1]]), 1L)
  })
})

test_that("integration prepare bind", {
  with_integration_client(function(client) {
    result <- sb_query(client, "SELECT ?::INTEGER", list(42))
    expect_true(length(result$rows) > 0)
    expect_equal(as.integer(result$rows[[1]][[1]]), 42L)
  })
})

test_that("integration DBI transaction lifecycle tracks autocommit across failure path", {
  with_integration_connection(function(conn) {
    client <- conn@ptr$client
    expect_true(client$autocommit)

    expect_true(DBI::dbBegin(conn))
    expect_false(client$autocommit)

    expect_error(DBI::dbGetQuery(conn, "SELECT FROM"), "\\[42")
    expect_true(DBI::dbRollback(conn))
    expect_true(client$autocommit)

    result <- DBI::dbGetQuery(conn, "SELECT 9")
    expect_equal(as.integer(result[[1]][[1]]), 9L)
  })
})

test_that("integration savepoint lifecycle", {
  with_integration_client(function(client) {
    scratchbird:::sb_savepoint(client, "sp_r_bootstrap")
    scratchbird:::sb_release_savepoint(client, "sp_r_bootstrap")
    scratchbird:::sb_begin(client)
    scratchbird:::sb_savepoint(client, "sp_r_live")
    scratchbird:::sb_rollback_to_savepoint(client, "sp_r_live")
    scratchbird:::sb_release_savepoint(client, "sp_r_live")
    scratchbird:::sb_commit(client)
    scratchbird:::sb_savepoint(client, "sp_r_after_commit")
    scratchbird:::sb_release_savepoint(client, "sp_r_after_commit")

    scratchbird:::sb_begin(client)
    scratchbird:::sb_rollback(client)
    post_rollback <- sb_query(client, "SELECT 2")
    expect_true(length(post_rollback$rows) > 0)
    expect_equal(as.integer(post_rollback$rows[[1]][[1]]), 2L)
    scratchbird:::sb_savepoint(client, "sp_r_after_rollback")
    scratchbird:::sb_release_savepoint(client, "sp_r_after_rollback")
  })
})

test_that("integration metadata wrappers and schema tree rows", {
  with_integration_client(function(client) {
    schemas <- sb_get_query(client, sb_metadata_schemas_query())
    expect_true(is.data.frame(schemas))
    expect_true(ncol(schemas) > 0)

    tables <- sb_get_query(client, sb_metadata_tables_query())
    expect_true(is.data.frame(tables))
    expect_true(ncol(tables) > 0)

    tree_rows <- sb_metadata_build_schema_tree_rows(
      schemas,
      expected_integration_database_name(client),
      expand_schema_parents = TRUE
    )
    expect_true(is.data.frame(tree_rows))
    expect_true(nrow(tree_rows) > 0)
    expect_equal(as.character(tree_rows$kind[[1]]), "database")
    expect_equal(as.character(tree_rows$path[[1]]), expected_integration_database_name(client))
  })
})

test_that("integration metadata wrapper family smoke", {
  with_integration_client(function(client) {
    metadata_queries <- list(
      sb_metadata_indexes_query(),
      sb_metadata_index_columns_query(),
      sb_metadata_constraints_query(),
      sb_metadata_procedures_query(),
      sb_metadata_functions_query()
    )

    for (query in metadata_queries) {
      result <- sb_get_query(client, query)
      expect_true(is.data.frame(result))
      expect_true(ncol(result) > 0)
    }
  })
})

test_that("integration incremental fetch lifecycle with fetch_size", {
  dsn <- integration_dsn()
  client <- sb_connect(dsn, fetch_size = 1L)
  on.exit(sb_disconnect(client), add = TRUE)
  ensure_fetch_fixture(client)

  result <- sb_send_query(
    client,
    "SELECT value FROM r_fetch_fixture ORDER BY value"
  )

  chunk1 <- sb_fetch(result, n = 1)
  chunk2 <- sb_fetch(result, n = 1)
  chunk3 <- sb_fetch(result, n = 1)
  chunk4 <- sb_fetch(result, n = 1)

  expect_equal(nrow(chunk1), 1L)
  expect_equal(as.integer(chunk1[[1]][[1]]), 11L)
  expect_equal(nrow(chunk2), 1L)
  expect_equal(as.integer(chunk2[[1]][[1]]), 22L)
  expect_equal(nrow(chunk3), 1L)
  expect_equal(as.integer(chunk3[[1]][[1]]), 33L)
  expect_equal(nrow(chunk4), 0L)
})

test_that("integration ping roundtrip", {
  with_integration_client(function(client) {
    scratchbird:::sb_ping(client)
    result <- sb_query(client, "SELECT 2")
    expect_true(length(result$rows) > 0)
    expect_equal(as.integer(result$rows[[1]][[1]]), 2L)
  })
})

test_that("integration connection remains usable after server error", {
  with_integration_client(function(client) {
    expect_error(sb_query(client, "SELECT FROM"), "\\[42")
    result <- sb_query(client, "SELECT 7")
    expect_true(length(result$rows) > 0)
    expect_equal(as.integer(result$rows[[1]][[1]]), 7L)
  })
})

test_that("integration types fixture", {
  with_integration_client(function(client) {
    ensure_type_coverage_fixture(client)
    result <- sb_query(client, "SELECT * FROM type_coverage")
    expect_true(length(result$rows) > 0)
  })
})

test_that("cancel query", {
  dsn <- integration_dsn()
  client <- sb_connect(dsn, fetch_size = 1L)
  on.exit(sb_disconnect(client), add = TRUE)
  ensure_fetch_fixture(client)
  cancel_query <- integration_cancel_query()
  result <- sb_send_query(client, cancel_query$sql, cancel_query$params)

  for (idx in seq_len(4)) {
    chunk <- sb_fetch(result, n = 1)
    if (nrow(chunk) == 0L) {
      skip("cancel SQL completed before cancel window")
    }
  }

  sb_cancel(client)

  cancelled <- FALSE
  cancel_error <- NULL
  for (idx in seq_len(8)) {
    outcome <- tryCatch(sb_fetch(result, n = 1), error = function(err) err)
    if (inherits(outcome, "error")) {
      cancelled <- TRUE
      cancel_error <- outcome
      break
    }
    if (nrow(outcome) == 0L) {
      cancelled <- TRUE
      break
    }
  }

  expect_true(cancelled)
  if (!is.null(cancel_error) && !is.null(cancel_error$sqlstate) && nzchar(cancel_error$sqlstate)) {
    expect_equal(cancel_error$sqlstate, "57014")
  }

  recovery <- sb_query(client, "SELECT 1")
  expect_equal(as.integer(recovery$rows[[1]][[1]]), 1L)
})
