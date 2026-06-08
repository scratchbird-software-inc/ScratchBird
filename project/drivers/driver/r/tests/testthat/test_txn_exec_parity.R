# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

new_mock_connection <- function(client = NULL) {
  if (is.null(client)) {
    client <- new.env(parent = emptyenv())
    client$autocommit <- TRUE
  }
  ptr <- new.env(parent = emptyenv())
  ptr$client <- client
  methods::new("ScratchbirdConnection", ptr = ptr)
}

test_that("DBI transaction methods delegate and align autocommit", {
  client <- new.env(parent = emptyenv())
  client$autocommit <- TRUE
  conn <- new_mock_connection(client)
  begin_args <- NULL
  call_log <- character()

  local_mocked_bindings(
    sb_begin = function(client_arg, ...) {
      call_log <<- c(call_log, "begin")
      begin_args <<- list(...)
      expect_identical(client_arg, client)
      invisible(NULL)
    },
    sb_commit = function(client_arg, ...) {
      call_log <<- c(call_log, "commit")
      expect_identical(client_arg, client)
      invisible(NULL)
    },
    sb_rollback = function(client_arg, ...) {
      call_log <<- c(call_log, "rollback")
      expect_identical(client_arg, client)
      invisible(NULL)
    },
    sb_set_autocommit = function(client_arg, value) {
      client_arg$autocommit <- isTRUE(value)
      call_log <<- c(call_log, paste0("autocommit=", client_arg$autocommit))
      invisible(NULL)
    },
    .package = "scratchbird"
  )

  expect_true(DBI::dbBegin(conn, isolation_level = SB_ISOLATION_SERIALIZABLE))
  expect_equal(begin_args$isolation_level, SB_ISOLATION_SERIALIZABLE)
  expect_false(client$autocommit)

  expect_true(DBI::dbCommit(conn))
  expect_true(client$autocommit)

  expect_true(DBI::dbBegin(conn))
  expect_true(DBI::dbRollback(conn))
  expect_true(client$autocommit)
  expect_equal(
    call_log,
    c(
      "begin",
      "autocommit=FALSE",
      "commit",
      "autocommit=TRUE",
      "begin",
      "autocommit=FALSE",
      "rollback",
      "autocommit=TRUE"
    )
  )
})

test_that("canonical isolation helper documents public alias mapping", {
  expect_equal(sb_canonical_isolation_label(SB_ISOLATION_READ_UNCOMMITTED), "READ COMMITTED")
  expect_equal(sb_canonical_isolation_label(SB_ISOLATION_READ_COMMITTED), "READ COMMITTED")
  expect_equal(sb_canonical_isolation_label(SB_ISOLATION_REPEATABLE_READ), "SNAPSHOT")
  expect_equal(sb_canonical_isolation_label(SB_ISOLATION_SERIALIZABLE), "SNAPSHOT TABLE STABILITY")
  expect_equal(sb_canonical_isolation_label(99L), "UNKNOWN(99)")
})

test_that("canonical read committed mode helper documents public selector", {
  expect_equal(sb_canonical_read_committed_mode_label(SB_READ_COMMITTED_MODE_DEFAULT), "READ COMMITTED")
  expect_equal(
    sb_canonical_read_committed_mode_label(SB_READ_COMMITTED_MODE_READ_CONSISTENCY),
    "READ COMMITTED READ CONSISTENCY"
  )
  expect_equal(
    sb_canonical_read_committed_mode_label(SB_READ_COMMITTED_MODE_RECORD_VERSION),
    "READ COMMITTED RECORD VERSION"
  )
  expect_equal(
    sb_canonical_read_committed_mode_label(SB_READ_COMMITTED_MODE_NO_RECORD_VERSION),
    "READ COMMITTED NO RECORD VERSION"
  )
  expect_equal(sb_canonical_read_committed_mode_label(99L), "UNKNOWN(99)")
})

test_that("transaction helpers emit expected message types", {
  client <- new.env(parent = emptyenv())
  client$sequence <- 0
  client$attachment_id <- raw(16)
  client$txn_id <- 0
  client$txn_active <- FALSE
  client$explicit_txn <- FALSE
  client$con <- list(socket = "mock")
  client$autocommit <- TRUE
  messages <- list()
  sql_calls <- character()
  drain_calls <- 0L

  local_mocked_bindings(
    sb_send_message = function(client_arg, type, payload, flags = 0L, force_zero = FALSE) {
      messages[[length(messages) + 1]] <<- list(
        type = as.integer(type),
        payload = payload,
        flags = as.integer(flags),
        force_zero = force_zero
      )
      0L
    },
    sb_drain_until_ready = function(client_arg) {
      drain_calls <<- drain_calls + 1L
      client_arg$txn_active <- TRUE
      client_arg$txn_id <- 9001
      invisible(NULL)
    },
    sb_execute_query = function(client_arg, sql, params = list()) {
      sql_calls <<- c(sql_calls, sql)
      result <- new.env(parent = emptyenv())
      result$pending_rows <- list()
      result$done <- TRUE
      result$columns <- list()
      result$rowcount <- 0L
      result
    },
    sb_fetch_rows = function(result, n = -1) {
      list()
    },
    sb_ensure_implicit_transaction = function(client_arg) {
      invisible(NULL)
    },
    .package = "scratchbird"
  )

  sb_begin(client, isolation_level = SB_ISOLATION_SERIALIZABLE, wait = TRUE, timeout_ms = 500L)
  sb_commit(client, flags = 3L)
  sb_rollback(client, flags = 2L)
  sb_savepoint(client, "sp1")
  sb_release_savepoint(client, "sp1")
  sb_rollback_to_savepoint(client, "sp1")

  expect_equal(length(messages), 1L)
  expect_equal(messages[[1]]$type, SB_MSG_TXN_BEGIN)
  expect_identical(
    messages[[1]]$payload,
    build_txn_begin_payload(
      bitwOr(bitwOr(SB_TXN_FLAG_HAS_ISOLATION, SB_TXN_FLAG_HAS_WAIT), SB_TXN_FLAG_HAS_TIMEOUT),
      0L,
      0L,
      SB_ISOLATION_SERIALIZABLE,
      0L,
      0L,
      1L,
      500L
    )
  )
  expect_equal(
    sql_calls,
    c(
      "COMMIT",
      "ROLLBACK",
      'SAVEPOINT "sp1"',
      'RELEASE SAVEPOINT "sp1"',
      'ROLLBACK TO SAVEPOINT "sp1"'
    )
  )
  expect_equal(drain_calls, 1L)
})

test_that("read committed mode expands begin payload and rejects snapshot aliases", {
  client <- new.env(parent = emptyenv())
  client$sequence <- 0
  client$attachment_id <- raw(16)
  client$txn_id <- 0
  client$con <- list(socket = "mock")
  client$autocommit <- TRUE
  messages <- list()
  drain_calls <- 0L

  local_mocked_bindings(
    sb_send_message = function(client_arg, type, payload, flags = 0L, force_zero = FALSE) {
      messages[[length(messages) + 1]] <<- list(
        type = as.integer(type),
        payload = payload,
        flags = as.integer(flags),
        force_zero = force_zero
      )
      0L
    },
    sb_drain_until_ready = function(client_arg) {
      drain_calls <<- drain_calls + 1L
      invisible(NULL)
    },
    .package = "scratchbird"
  )

  sb_begin(client, timeout_ms = 25L, read_committed_mode = SB_READ_COMMITTED_MODE_READ_CONSISTENCY)

  expect_equal(length(messages), 1L)
  expect_equal(length(messages[[1]]$payload), 16L)
  expect_identical(
    messages[[1]]$payload,
    build_txn_begin_payload(
      bitwOr(bitwOr(SB_TXN_FLAG_HAS_ISOLATION, SB_TXN_FLAG_HAS_TIMEOUT), SB_TXN_FLAG_HAS_READ_COMMITTED_MODE),
      0L,
      0L,
      SB_ISOLATION_READ_COMMITTED,
      0L,
      0L,
      0L,
      25L,
      SB_READ_COMMITTED_MODE_READ_CONSISTENCY
    )
  )
  expect_equal(drain_calls, 1L)

  client_invalid <- new.env(parent = emptyenv())
  client_invalid$txn_active <- FALSE
  client_invalid$explicit_txn <- FALSE
  expect_error(
    sb_begin(
      client_invalid,
      isolation_level = SB_ISOLATION_SERIALIZABLE,
      read_committed_mode = SB_READ_COMMITTED_MODE_READ_CONSISTENCY
    ),
    "READ COMMITTED isolation alias"
  )
})

test_that("fresh native boundary adopts default begin and rejects non-default options", {
  client <- new.env(parent = emptyenv())
  client$txn_id <- 0
  client$txn_active <- TRUE
  client$explicit_txn <- FALSE
  sent <- FALSE
  drained <- FALSE

  local_mocked_bindings(
    sb_send_message = function(client_arg, type, payload, flags = 0L, force_zero = FALSE) {
      sent <<- TRUE
      0L
    },
    sb_drain_until_ready = function(client_arg) {
      drained <<- TRUE
      invisible(NULL)
    },
    .package = "scratchbird"
  )

  expect_invisible(sb_begin(client))
  expect_true(client$explicit_txn)
  expect_false(sent)
  expect_false(drained)

  client_nondefault <- new.env(parent = emptyenv())
  client_nondefault$txn_id <- 0
  client_nondefault$txn_active <- TRUE
  client_nondefault$explicit_txn <- FALSE

  expect_error(
    sb_begin(client_nondefault, timeout_ms = 5L),
    "\\[0A000\\] non-default transaction options cannot adopt an existing fresh native boundary"
  )
})

test_that("runtime transaction state treats active ready and txn status as authoritative", {
  client <- new.env(parent = emptyenv())
  client$txn_id <- 0
  client$txn_active <- FALSE
  client$explicit_txn <- FALSE
  client$parameters <- list()
  client$notification_handlers <- list()

  sb_apply_runtime_ready_state(client, 1L, 0)
  expect_true(sb_transaction_active(client))
  expect_equal(client$txn_id, 0)

  txn_status_payload <- c(charToRaw("T"), raw(3), pack_u64(0))
  expect_true(sb_handle_async(client, SB_MSG_TXN_STATUS, txn_status_payload))
  expect_true(sb_transaction_active(client))
  expect_equal(client$txn_id, 0)

  sb_apply_runtime_ready_state(client, 0L, 0)
  expect_false(sb_transaction_active(client))

  sb_handle_parameter_status(client, "current_txn_id", "42")
  expect_true(sb_transaction_active(client))
  expect_equal(client$txn_id, 42)
})

test_that("prepared transaction helpers emit canonical control SQL", {
  client <- new.env(parent = emptyenv())
  sql_calls <- character()

  local_mocked_bindings(
    sb_execute_query = function(client_arg, sql, params = list()) {
      sql_calls <<- c(sql_calls, sql)
      result <- new.env(parent = emptyenv())
      result$pending_rows <- list()
      result$done <- TRUE
      result$columns <- list()
      result$rowcount <- 0L
      result
    },
    sb_fetch_rows = function(result, n = -1) {
      list()
    },
    .package = "scratchbird"
  )

  sb_prepare_transaction(client, "gid-1")
  sb_commit_prepared(client, "gid-1")
  sb_rollback_prepared(client, "gid'2")

  expect_equal(
    sql_calls,
    c(
      "PREPARE TRANSACTION 'gid-1'",
      "COMMIT PREPARED 'gid-1'",
      "ROLLBACK PREPARED 'gid''2'"
    )
  )
})

test_that("prepared transaction helpers reject empty gid", {
  client <- new.env(parent = emptyenv())

  expect_error(
    sb_prepare_transaction(client, "   "),
    "\\[42601\\] global transaction id is required"
  )
})

test_that("dormant helpers fail closed and capabilities stay explicit", {
  client <- new.env(parent = emptyenv())

  expect_true(sb_supports_prepared_transactions(client))
  expect_false(sb_supports_dormant_reattach(client))
  expect_error(sb_detach_to_dormant(client), "\\[0A000\\]")
  expect_error(sb_reattach_dormant(client, "dormant-1", "token-1"), "\\[0A000\\]")
})

test_that("dbSendQuery/dbFetch/dbClearResult follow DBI result lifecycle", {
  conn <- new_mock_connection()
  result_env <- new.env(parent = emptyenv())
  result_env$done <- FALSE
  captured_sql <- NULL
  captured_params <- NULL
  fetch_n <- NULL
  clear_called <- FALSE

  local_mocked_bindings(
    sb_send_query = function(client, sql, params = NULL) {
      captured_sql <<- sql
      captured_params <<- params
      result_env
    },
    sb_fetch = function(result, n = -1) {
      fetch_n <<- n
      data.frame(value = 1L)
    },
    sb_clear_result = function(result) {
      clear_called <<- TRUE
      result$done <- TRUE
      result
    },
    .package = "scratchbird"
  )

  res <- DBI::dbSendQuery(conn, "SELECT ?::INTEGER", params = list(42L))
  expect_s4_class(res, "ScratchbirdResult")

  fetched <- DBI::dbFetch(res, n = 1)
  expect_equal(fetch_n, 1)
  expect_equal(fetched$value, 1L)

  expect_true(DBI::dbClearResult(res))
  expect_true(clear_called)
  expect_true(result_env$done)
  expect_equal(captured_sql, "SELECT ?::INTEGER")
  expect_equal(captured_params, list(42L))
})

test_that("dbExecute drains rows and returns integer row count", {
  conn <- new_mock_connection()
  result_env <- new.env(parent = emptyenv())
  result_env$rowcount <- 7
  captured_sql <- NULL
  captured_params <- NULL
  drained_n <- NULL

  local_mocked_bindings(
    sb_send_query = function(client, sql, params = NULL) {
      captured_sql <<- sql
      captured_params <<- params
      result_env
    },
    sb_fetch_rows = function(result, n = -1) {
      drained_n <<- n
      list(list(id = 1L))
    },
    .package = "scratchbird"
  )

  count <- DBI::dbExecute(conn, "UPDATE t SET v = ? WHERE id = ?", params = list("x", 1L))
  expect_type(count, "integer")
  expect_equal(count, 7L)
  expect_equal(drained_n, -1)
  expect_equal(captured_sql, "UPDATE t SET v = ? WHERE id = ?")
  expect_equal(captured_params, list("x", 1L))
})
