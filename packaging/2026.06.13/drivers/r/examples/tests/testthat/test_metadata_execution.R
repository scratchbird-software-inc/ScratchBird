# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

new_metadata_mock_connection <- function() {
  ptr <- new.env(parent = emptyenv())
  ptr$client <- new.env(parent = emptyenv())
  methods::new("ScratchbirdConnection", ptr = ptr)
}

new_metadata_mock_result <- function(client = NULL) {
  if (is.null(client)) {
    client <- new.env(parent = emptyenv())
    client$notification_handlers <- list()
    client$parameters <- list()
    client$txn_id <- 0
  }
  result_env <- new.env(parent = emptyenv())
  result_env$client <- client
  result_env$columns <- list()
  result_env$rowcount <- -1
  result_env$command_tag <- ""
  result_env$done <- FALSE
  result_env$page_size <- 0L
  result_env$pending_rows <- list()
  methods::new("ScratchbirdResult", result = result_env)
}

test_that("dbListTables lists schema-qualified names from metadata only", {
  conn <- new_metadata_mock_connection()
  tables <- data.frame(
    table_id = c(1L, 2L, 3L),
    schema_id = c(10L, 11L, 11L),
    table_name = c("events", "events", "users"),
    stringsAsFactors = FALSE
  )
  schemas <- data.frame(
    schema_id = c(10L, 11L),
    schema_name = c("sys", "users"),
    stringsAsFactors = FALSE
  )

  local_mocked_bindings(
    sb_get_query = function(client, sql, ...) {
      if (identical(sql, sb_metadata_tables_query())) return(tables)
      if (identical(sql, sb_metadata_schemas_query())) return(schemas)
      stop("unexpected SQL")
    },
    .package = "scratchbird"
  )

  listed <- DBI::dbListTables(conn)
  expect_equal(listed, c("sys.events", "users.events", "users.users"))
})

test_that("dbExistsTable resolves qualified table names against metadata", {
  conn <- new_metadata_mock_connection()
  tables <- data.frame(
    table_id = c(1L, 2L),
    schema_id = c(10L, 11L),
    table_name = c("events", "events"),
    stringsAsFactors = FALSE
  )
  schemas <- data.frame(
    schema_id = c(10L, 11L),
    schema_name = c("sys", "users"),
    stringsAsFactors = FALSE
  )

  local_mocked_bindings(
    sb_get_query = function(client, sql, ...) {
      if (identical(sql, sb_metadata_tables_query())) return(tables)
      if (identical(sql, sb_metadata_schemas_query())) return(schemas)
      stop("unexpected SQL")
    },
    .package = "scratchbird"
  )

  expect_true(DBI::dbExistsTable(conn, "users.events"))
  expect_true(DBI::dbExistsTable(conn, DBI::Id(schema = "sys", table = "events")))
  expect_false(DBI::dbExistsTable(conn, "audit.events"))
})

test_that("dbListFields filters metadata columns by schema and table", {
  conn <- new_metadata_mock_connection()
  tables <- data.frame(
    table_id = c(1L, 2L),
    schema_id = c(10L, 11L),
    table_name = c("events", "events"),
    stringsAsFactors = FALSE
  )
  schemas <- data.frame(
    schema_id = c(10L, 11L),
    schema_name = c("sys", "users"),
    stringsAsFactors = FALSE
  )
  columns <- data.frame(
    table_id = c(1L, 1L, 2L, 2L),
    column_name = c("id", "payload", "id", "email"),
    ordinal_position = c(1L, 2L, 1L, 2L),
    stringsAsFactors = FALSE
  )

  local_mocked_bindings(
    sb_get_query = function(client, sql, ...) {
      if (identical(sql, sb_metadata_tables_query())) return(tables)
      if (identical(sql, sb_metadata_schemas_query())) return(schemas)
      if (identical(sql, sb_metadata_columns_query())) return(columns)
      stop("unexpected SQL")
    },
    .package = "scratchbird"
  )

  fields <- DBI::dbListFields(conn, DBI::Id(schema = "users", table = "events"))
  expect_equal(fields, c("id", "email"))
})

test_that("dbColumnInfo primes row metadata and preserves subsequent fetch rows", {
  res <- new_metadata_mock_result()
  recv_index <- 0L

  local_mocked_bindings(
    sb_recv_message = function(client_arg) {
      recv_index <<- recv_index + 1L
      if (recv_index == 1L) {
        return(list(type = SB_MSG_ROW_DESCRIPTION, payload = raw()))
      }
      list(type = SB_MSG_DATA_ROW, payload = raw())
    },
    parse_row_description = function(payload) {
      list(list(
        name = "id",
        table_oid = 10L,
        column_index = 1L,
        type_oid = 23L,
        type_size = 4L,
        type_modifier = -1L,
        format = SB_FORMAT_BINARY,
        nullable = FALSE
      ))
    },
    parse_data_row = function(payload) {
      list(list(data = as.raw(0x2A)))
    },
    sb_decode_row = function(columns, values) {
      list(42L)
    },
    .package = "scratchbird"
  )

  info <- DBI::dbColumnInfo(res)
  expect_equal(info$name, "id")
  expect_equal(info$type_oid, 23L)
  expect_equal(info$table_oid, 10L)
  expect_equal(info$column_index, 1L)
  expect_equal(info$format, SB_FORMAT_BINARY)
  expect_false(info$nullable)

  fetched <- DBI::dbFetch(res, n = 1)
  expect_equal(nrow(fetched), 1L)
  expect_equal(as.integer(fetched$id[[1]]), 42L)
})

test_that("dbColumnInfo returns empty data frame when result has no columns", {
  res <- new_metadata_mock_result()
  res@result$done <- TRUE

  info <- DBI::dbColumnInfo(res)
  expect_true(is.data.frame(info))
  expect_equal(nrow(info), 0L)
  expect_equal(
    names(info),
    c("name", "type_oid", "type_size", "type_modifier", "table_oid", "column_index", "format", "nullable")
  )
})
