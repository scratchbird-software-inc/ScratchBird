# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

test_that("sb_connect validates DSN/auth options before opening socket", {
  socket_opened <- FALSE
  local_mocked_bindings(
    sb_open_socket = function(cfg) {
      socket_opened <<- TRUE
      list()
    },
    sb_startup_and_auth = function(client) invisible(NULL),
    sb_ensure_implicit_transaction = function(client) invisible(NULL),
    sb_apply_schema = function(client) invisible(NULL),
    .package = "scratchbird"
  )

  expect_error(sb_connect("scratchbird://localhost:3092/mydb"), "user and database are required")
  client <- sb_connect("scratchbird://user:pass@localhost:3092/mydb?binary_transfer=false&compression=zstd")
  expect_true(socket_opened)
  expect_false(client$cfg$binary_transfer)
  expect_equal(client$cfg$compression, "zstd")
})

test_that("sb_connect initializes client through startup/auth path", {
  socket_opened <- FALSE
  startup_done <- FALSE
  schema_applied <- FALSE
  socket_closed <- FALSE
  local_mocked_bindings(
    sb_open_socket = function(cfg) {
      socket_opened <<- TRUE
      list(socket = "mock")
    },
    sb_startup_and_auth = function(client) {
      startup_done <<- TRUE
      client$attachment_id <- as.raw(rep(0L, 16))
      client$txn_id <- 0
      invisible(NULL)
    },
    sb_apply_schema = function(client) {
      schema_applied <<- TRUE
      invisible(NULL)
    },
    sb_socket_close = function(con) {
      socket_closed <<- TRUE
      invisible(NULL)
    },
    .package = "scratchbird"
  )

  client <- sb_connect("scratchbird://user:pass@localhost:3092/mydb")
  expect_true(socket_opened)
  expect_true(startup_done)
  expect_true(schema_applied)
  expect_true(sb_is_valid(client))

  sb_disconnect(client)
  expect_true(socket_closed)
  expect_false(sb_is_valid(client))
})

test_that("dbCanConnect returns TRUE and cleans up on successful connect", {
  disconnect_called <- FALSE
  local_mocked_bindings(
    sb_connect = function(dsn = "", ...) {
      env <- new.env(parent = emptyenv())
      env$con <- list(socket = "mock")
      env
    },
    sb_disconnect = function(client) {
      disconnect_called <<- TRUE
      invisible(NULL)
    },
    .package = "scratchbird"
  )

  ok <- DBI::dbCanConnect(Scratchbird(), "scratchbird://user:pass@localhost:3092/mydb")
  expect_true(ok)
  expect_true(disconnect_called)
})

test_that("dbCanConnect returns FALSE when connect fails", {
  local_mocked_bindings(
    sb_connect = function(dsn = "", ...) stop("connect failed"),
    .package = "scratchbird"
  )
  expect_false(DBI::dbCanConnect(Scratchbird(), "scratchbird://user:pass@localhost:3092/mydb"))
})

test_that("auth/protocol parsers decode auth request and continue frames", {
  request <- c(pack_u8(SB_AUTH_PASSWORD), raw(3), charToRaw("pw"))
  parsed_request <- parse_auth_request(request)
  expect_equal(parsed_request$method, SB_AUTH_PASSWORD)
  expect_equal(rawToChar(parsed_request$data), "pw")

  continue_data <- charToRaw("r=nonce,s=c2FsdA==,i=4096")
  continue_payload <- c(
    pack_u8(SB_AUTH_SCRAM_SHA256),
    pack_u8(1L),
    raw(2),
    pack_u32(length(continue_data)),
    continue_data
  )
  parsed_continue <- parse_auth_continue(continue_payload)
  expect_equal(parsed_continue$method, SB_AUTH_SCRAM_SHA256)
  expect_equal(parsed_continue$stage, 1L)
  expect_equal(rawToChar(parsed_continue$data), rawToChar(continue_data))
})

test_that("auth/protocol parsers reject truncated payloads", {
  expect_error(parse_auth_request(raw(3)), "Auth request truncated")
  expect_error(parse_auth_continue(raw(7)), "Auth continue truncated")
  expect_error(parse_auth_ok(raw(19)), "Auth ok truncated")
})

test_that("ready parser accepts long fallback payloads with nul status marker byte", {
  payload <- raw(128)
  payload[1] <- as.raw(1L)
  payload[5:12] <- pack_u64(42)
  payload[13:20] <- pack_u64(7)

  parsed <- parse_ready(payload)

  expect_equal(parsed$status, 1L)
  expect_equal(parsed$txn_id, 42)
  expect_equal(parsed$visibility, 7)
})

test_that("protocol text fields render embedded nul bytes as hex", {
  name <- charToRaw("binary_status")
  value <- as.raw(c(0x41, 0x00, 0x42))
  payload <- c(
    pack_u32(1L),
    pack_u32(length(name)),
    name,
    pack_u16(1L),
    pack_u8(0L),
    pack_u32(length(value)),
    value
  )

  parsed <- parse_parameter_status(payload)

  expect_equal(parsed$name, "binary_status")
  expect_equal(parsed$value, "0x410042")
})

test_that("P1 row descriptions decode column names and canonical type refs", {
  name <- charToRaw("page_size_bytes")
  type_ref <- c(pack_u16(2L), pack_u16(3L), raw(SB_P1_CANONICAL_TYPE_REF_BYTES - 4L))
  column <- c(
    pack_i32(1L),
    pack_u8(0L),
    pack_u8(1L),
    pack_u8(0L),
    pack_u8(0L),
    raw(8),
    type_ref,
    raw(16L * 3L),
    pack_u32(0L),
    pack_u16(0L),
    pack_u16(0L),
    pack_u8(1L),
    pack_i32(length(name)),
    name
  )
  payload <- c(
    pack_u16(1L),
    pack_u8(0L),
    pack_u8(1L),
    pack_i32(1L),
    raw(SB_P1_ROW_DESCRIPTION_HEADER_BYTES - 8L),
    column
  )

  cols <- parse_row_description(payload)

  expect_equal(length(cols), 1L)
  expect_equal(cols[[1]]$name, "page_size_bytes")
  expect_equal(cols[[1]]$column_index, 0L)
  expect_equal(cols[[1]]$type_oid, SB_OID_INT4)
  expect_equal(cols[[1]]$type_size, 4L)
  expect_equal(cols[[1]]$format, SB_FORMAT_TEXT)
  expect_false(cols[[1]]$nullable)
})

test_that("sb_open_socket allows sslmode=disable and delegates to native transport", {
  cfg <- sb_config("scratchbird://user:pass@localhost:3092/mydb?sslmode=disable")
  seen_sslmode <- NULL
  local_mocked_bindings(
    sb_tls_connect_native = function(inner_cfg) {
      seen_sslmode <<- tolower(inner_cfg$sslmode)
      structure(list(socket = "mock"), class = "scratchbird_socket")
    },
    .package = "scratchbird"
  )

  expect_silent(sb_open_socket(cfg))
  expect_identical(seen_sslmode, "disable")
})
