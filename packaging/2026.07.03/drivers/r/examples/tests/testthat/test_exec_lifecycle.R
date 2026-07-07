# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

new_mock_client_for_exec <- function() {
  client <- new.env(parent = emptyenv())
  client$sequence <- 0L
  client$attachment_id <- raw(16)
  client$txn_id <- 0
  client$txn_active <- FALSE
  client$explicit_txn <- FALSE
  client$con <- list(socket = "mock")
  client$last_plan <- NULL
  client$last_sblr <- NULL
  client$last_query_sequence <- -1L
  client$notification_handlers <- list()
  client$parameters <- list()
  client$portal_resume_pending <- FALSE
  client
}

new_mock_result_for_exec <- function(client, page_size = 0L) {
  result <- new.env(parent = emptyenv())
  result$client <- client
  result$columns <- list()
  result$rowcount <- -1
  result$command_tag <- ""
  result$done <- FALSE
  result$page_size <- page_size
  result$response_started <- FALSE
  result$ignored_stray_ready <- FALSE
  result
}

test_that("sb_send_extended_query sends parse bind execute sync in order", {
  client <- new_mock_client_for_exec()
  send_types <- integer()
  send_sequences <- integer()

  local_mocked_bindings(
    encode_param = function(param) {
      if (is.character(param)) {
        return(list(param = list(format = SB_FORMAT_TEXT, data = charToRaw(param)), oid = 25L))
      }
      list(param = list(format = SB_FORMAT_BINARY, data = as.raw(0x2A)), oid = 23L)
    },
    sb_send_message = function(client_arg, type, payload, flags = 0L, force_zero = FALSE) {
      seq <- length(send_types) + 100L
      send_types <<- c(send_types, as.integer(type))
      send_sequences <<- c(send_sequences, seq)
      seq
    },
    sb_describe_statement = function(client_arg, statement_name) {
      2L
    },
    .package = "scratchbird"
  )

  scratchbird:::sb_send_extended_query(
    client,
    "SELECT ?::INTEGER, ?::VARCHAR",
    list(42L, "value"),
    max_rows = 0L
  )

  expect_equal(
    send_types,
    c(SB_MSG_PARSE, SB_MSG_BIND, SB_MSG_EXECUTE, SB_MSG_SYNC)
  )
  expect_equal(client$last_query_sequence, send_sequences[[3]])
})

test_that("sb_send_extended_query aborts before bind on parameter mismatch", {
  client <- new_mock_client_for_exec()
  send_types <- integer()

  local_mocked_bindings(
    encode_param = function(param) {
      list(param = list(format = SB_FORMAT_BINARY, data = as.raw(0x2A)), oid = 23L)
    },
    sb_send_message = function(client_arg, type, payload, flags = 0L, force_zero = FALSE) {
      send_types <<- c(send_types, as.integer(type))
      17L
    },
    sb_describe_statement = function(client_arg, statement_name) {
      2L
    },
    .package = "scratchbird"
  )

  expect_error(
    scratchbird:::sb_send_extended_query(
      client,
      "SELECT ?::INTEGER",
      list(42L),
      max_rows = 0L
    ),
    "parameter count mismatch \\(07001\\)"
  )
  expect_equal(send_types, c(SB_MSG_PARSE))
})

test_that("sb_result_next_row resumes suspended portal with execute", {
  client <- new_mock_client_for_exec()
  result <- new_mock_result_for_exec(client, page_size = 128L)
  recv_index <- 0L
  sent_types <- integer()
  sent_payloads <- list()

  local_mocked_bindings(
    sb_recv_message = function(client_arg) {
      recv_index <<- recv_index + 1L
      if (recv_index == 1L) {
        return(list(type = SB_MSG_ROW_DESCRIPTION, payload = raw()))
      }
      if (recv_index == 2L) {
        return(list(type = SB_MSG_PORTAL_SUSPENDED, payload = raw()))
      }
      list(type = SB_MSG_DATA_ROW, payload = raw())
    },
    parse_row_description = function(payload) {
      list(list(name = "value", type_oid = 23L, format = SB_FORMAT_BINARY))
    },
    parse_data_row = function(payload) {
      list(list(format = SB_FORMAT_BINARY, data = as.raw(0x2A)))
    },
    sb_decode_row = function(columns, values) {
      list(77L)
    },
    sb_send_message = function(client_arg, type, payload, flags = 0L, force_zero = FALSE) {
      sent_types <<- c(sent_types, as.integer(type))
      sent_payloads[[length(sent_payloads) + 1L]] <<- payload
      55L
    },
    .package = "scratchbird"
  )

  row <- scratchbird:::sb_result_next_row(result)
  expect_equal(row[[1]], 77L)
  expect_equal(sent_types, c(SB_MSG_EXECUTE))
  expect_identical(sent_payloads[[1]], build_execute_payload("", 128L))
  expect_equal(client$last_query_sequence, 55L)
  expect_false(client$portal_resume_pending)
})

test_that("sb_resume_suspended_portal requires explicit pending state", {
  client <- new_mock_client_for_exec()

  expect_error(
    scratchbird:::sb_resume_suspended_portal(client, 2L),
    "\\[55000\\] portal resume requires explicit suspended state"
  )
})

test_that("sb_result_next_row records completion and marks ready state", {
  client <- new_mock_client_for_exec()
  result <- new_mock_result_for_exec(client, page_size = 0L)
  recv_index <- 0L

  local_mocked_bindings(
    sb_recv_message = function(client_arg) {
      recv_index <<- recv_index + 1L
      if (recv_index == 1L) {
        return(list(type = SB_MSG_COMMAND_COMPLETE, payload = raw()))
      }
      list(type = SB_MSG_READY, payload = raw())
    },
    parse_command_complete = function(payload) {
      list(tag = "SELECT 3", rows = 3)
    },
    parse_ready = function(payload) {
      list(status = 1L, txn_id = 10)
    },
    .package = "scratchbird"
  )

  row <- scratchbird:::sb_result_next_row(result)
  expect_null(row)
  expect_true(result$done)
  expect_equal(result$command_tag, "SELECT 3")
  expect_equal(result$rowcount, 3)
  expect_equal(client$txn_id, 10)
  expect_true(client$txn_active)
})

test_that("sb_result_next_row ignores one stray reopen ready before actual rows", {
  client <- new_mock_client_for_exec()
  result <- new_mock_result_for_exec(client, page_size = 0L)
  recv_index <- 0L

  local_mocked_bindings(
    sb_recv_message = function(client_arg) {
      recv_index <<- recv_index + 1L
      if (recv_index == 1L) {
        return(list(type = SB_MSG_READY, payload = raw()))
      }
      if (recv_index == 2L) {
        return(list(type = SB_MSG_ROW_DESCRIPTION, payload = raw()))
      }
      list(type = SB_MSG_DATA_ROW, payload = raw())
    },
    parse_ready = function(payload) {
      list(status = 1L, txn_id = 0)
    },
    parse_row_description = function(payload) {
      list(list(name = "value", type_oid = 23L, format = SB_FORMAT_BINARY))
    },
    parse_data_row = function(payload) {
      list(list(format = SB_FORMAT_BINARY, data = as.raw(0x2A)))
    },
    sb_decode_row = function(columns, values) {
      list(42L)
    },
    .package = "scratchbird"
  )

  row <- scratchbird:::sb_result_next_row(result)
  expect_equal(row[[1]], 42L)
  expect_true(client$txn_active)
  expect_false(result$done)
  expect_true(result$ignored_stray_ready)
})
