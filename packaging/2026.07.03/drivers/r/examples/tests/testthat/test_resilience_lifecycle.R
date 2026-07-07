# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

new_mock_connection_for_res <- function(client = NULL) {
  if (is.null(client)) {
    client <- new.env(parent = emptyenv())
    client$autocommit <- TRUE
  }
  ptr <- new.env(parent = emptyenv())
  ptr$client <- client
  methods::new("ScratchbirdConnection", ptr = ptr)
}

build_server_error_payload_for_res <- function(
  sqlstate = "",
  message = "query failed",
  detail = "",
  hint = "",
  severity = "ERROR"
) {
  fields <- list(S = severity, C = sqlstate, M = message, D = detail, H = hint)
  payload <- raw()
  for (field in names(fields)) {
    value <- fields[[field]]
    if (!nzchar(value)) next
    payload <- c(payload, charToRaw(field), charToRaw(value), as.raw(0x00))
  }
  c(payload, as.raw(0x00))
}

test_that("sb_disconnect is idempotent and closes socket once", {
  client <- new.env(parent = emptyenv())
  client$con <- list(socket = "mock")
  non_null_close_calls <- 0L
  null_close_calls <- 0L

  local_mocked_bindings(
    sb_socket_close = function(con) {
      if (is.null(con)) {
        null_close_calls <<- null_close_calls + 1L
      } else {
        non_null_close_calls <<- non_null_close_calls + 1L
      }
      invisible(NULL)
    },
    .package = "scratchbird"
  )

  sb_disconnect(client)
  sb_disconnect(client)

  expect_equal(non_null_close_calls, 1L)
  expect_equal(null_close_calls, 1L)
  expect_null(client$con)
})

test_that("dbDisconnect returns TRUE on repeated calls and only closes once", {
  client <- new.env(parent = emptyenv())
  client$con <- list(socket = "mock")
  conn <- new_mock_connection_for_res(client)
  non_null_close_calls <- 0L
  null_close_calls <- 0L

  local_mocked_bindings(
    sb_socket_close = function(con) {
      if (is.null(con)) {
        null_close_calls <<- null_close_calls + 1L
      } else {
        non_null_close_calls <<- non_null_close_calls + 1L
      }
      invisible(NULL)
    },
    .package = "scratchbird"
  )

  expect_true(DBI::dbDisconnect(conn))
  expect_true(DBI::dbDisconnect(conn))
  expect_equal(non_null_close_calls, 1L)
  expect_equal(null_close_calls, 1L)
  expect_null(client$con)
})

test_that("sb_prepare_connection clears abandoned transaction state before reconnect startup", {
  client <- new.env(parent = emptyenv())
  client$con <- list(socket = "stale")
  client$cfg <- list(front_door_mode = "direct")
  client$attachment_id <- as.raw(rep(0x7f, 16))
  client$txn_id <- 99
  client$sequence <- 41L
  client$last_query_sequence <- 17L
  client$parameters <- list(current_txn_id = "99")
  client$last_plan <- list(cost = 10)
  client$last_sblr <- list(hash = 123)
  client$prepared <- new.env(parent = emptyenv())
  client$prepared$stale_stmt <- TRUE
  client$autocommit <- FALSE
  client$txn_active <- TRUE
  client$explicit_txn <- TRUE
  client$cancel_requested <- TRUE
  client$needs_reconnect <- TRUE

  closed_socket <- NULL
  startup_checked <- FALSE
  implicit_checked <- FALSE
  schema_applied <- FALSE

  local_mocked_bindings(
    sb_socket_close = function(con) {
      closed_socket <<- con$socket
      invisible(NULL)
    },
    sb_open_socket = function(cfg) {
      list(socket = "fresh")
    },
    sb_startup_and_auth = function(client_arg) {
      startup_checked <<- TRUE
      expect_equal(client_arg$txn_id, 0)
      expect_false(client_arg$txn_active)
      expect_false(client_arg$explicit_txn)
      expect_false(client_arg$cancel_requested)
      expect_true(identical(ls(client_arg$prepared), character()))
      expect_equal(client_arg$parameters, list())
      invisible(NULL)
    },
    sb_ensure_implicit_transaction = function(client_arg) {
      implicit_checked <<- TRUE
      client_arg$txn_active <- TRUE
      client_arg$explicit_txn <- FALSE
      invisible(NULL)
    },
    sb_apply_schema = function(client_arg) {
      schema_applied <<- TRUE
      invisible(NULL)
    },
    .package = "scratchbird"
  )

  expect_invisible(sb_prepare_connection(client))
  expect_identical(closed_socket, "stale")
  expect_true(startup_checked)
  expect_true(implicit_checked)
  expect_true(schema_applied)
  expect_identical(client$con$socket, "fresh")
  expect_equal(client$txn_id, 0)
  expect_true(client$txn_active)
  expect_false(client$explicit_txn)
  expect_false(client$cancel_requested)
  expect_false(client$needs_reconnect)
})

test_that("dbClearResult is idempotent after completion", {
  result_env <- new.env(parent = emptyenv())
  result_env$done <- FALSE
  res <- methods::new("ScratchbirdResult", result = result_env)

  expect_true(DBI::dbClearResult(res))
  expect_true(DBI::dbClearResult(res))
  expect_true(result_env$done)
})

test_that("result cleanup can run after server error during row fetch", {
  client <- new.env(parent = emptyenv())
  client$notification_handlers <- list()
  client$parameters <- list()
  client$con <- list(socket = "mock")

  result <- new.env(parent = emptyenv())
  result$client <- client
  result$columns <- list()
  result$rowcount <- -1
  result$command_tag <- ""
  result$done <- FALSE
  result$page_size <- 0L

  local_mocked_bindings(
    sb_recv_message = function(client_arg) {
      list(
        type = SB_MSG_ERROR,
        payload = build_server_error_payload_for_res(
          sqlstate = "23505",
          message = "duplicate key value violates unique constraint"
        )
      )
    },
    .package = "scratchbird"
  )

  expect_error(
    sb_result_next_row(result),
    "\\[23505\\] duplicate key value violates unique constraint"
  )
  expect_false(result$done)

  sb_clear_result(result)
  expect_true(result$done)
})
