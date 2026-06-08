# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

test_that("sb_probe_auth_surface reports direct SCRAM_SHA_512", {
  sent <- list()
  local_mocked_bindings(
    sb_open_socket = function(cfg) list(socket = "mock"),
    sb_socket_close = function(con) invisible(NULL),
    sb_send_message = function(client, type, payload, flags = 0L, force_zero = FALSE) {
      sent[[length(sent) + 1L]] <<- list(type = type, payload = payload)
      invisible(0L)
    },
    sb_recv_message = local({
      emitted <- FALSE
      function(client) {
        if (!emitted) {
          emitted <<- TRUE
          return(list(type = SB_MSG_AUTH_REQUEST, payload = c(pack_u8(SB_AUTH_SCRAM_SHA512), raw(3)), attachment_id = raw(16), txn_id = 0))
        }
        stop("unexpected extra recv")
      }
    }),
    .package = "scratchbird"
  )

  probe <- sb_probe_auth_surface("scratchbird://user:pass@localhost:3092/mydb?sslmode=disable")
  expect_true(probe$reachable)
  expect_equal(probe$ingress_mode, "direct")
  expect_equal(probe$required_method, "SCRAM_SHA_512")
  expect_equal(probe$required_plugin_method_id, "scratchbird.auth.scram_sha_512")
  expect_length(probe$admitted_methods, 1)
  expect_equal(probe$admitted_methods[[1]]$wire_method, "SCRAM_SHA_512")
  expect_true(probe$admitted_methods[[1]]$executable_locally)
  expect_true(probe$additional_continuation_possible)
  expect_equal(sent[[1]]$type, SB_MSG_STARTUP)
})

test_that("sb_probe_auth_surface reports manager TOKEN ingress", {
  manager_frames <- list()
  local_mocked_bindings(
    sb_open_socket = function(cfg) list(socket = "mock"),
    sb_socket_close = function(con) invisible(NULL),
    sb_send_manager_frame = function(client, type, payload) {
      manager_frames[[length(manager_frames) + 1L]] <<- list(type = type, payload = payload)
      invisible(NULL)
    },
    sb_recv_manager_frame = local({
      emitted <- FALSE
      function(client) {
        if (!emitted) {
          emitted <<- TRUE
          return(list(type = SB_MCP_MSG_STATUS_RESPONSE, payload = raw()))
        }
        stop("unexpected extra manager recv")
      }
    }),
    .package = "scratchbird"
  )

  probe <- sb_probe_auth_surface("scratchbird://admin:secret@localhost:3090/mydb?front_door_mode=manager_proxy&sslmode=disable")
  expect_true(probe$reachable)
  expect_equal(probe$ingress_mode, "manager_proxy")
  expect_equal(probe$required_method, "TOKEN")
  expect_equal(probe$required_plugin_method_id, "scratchbird.auth.authkey_token")
  expect_length(probe$admitted_methods, 1)
  expect_equal(probe$admitted_methods[[1]]$wire_method, "TOKEN")
  expect_true(probe$additional_continuation_possible)
  expect_equal(manager_frames[[1]]$type, SB_MCP_MSG_HELLO)
})

test_that("sb_connect tracks resolved SCRAM_SHA_512 auth context", {
  sent <- list()
  local_mocked_bindings(
    sb_open_socket = function(cfg) list(socket = "mock"),
    sb_socket_close = function(con) invisible(NULL),
    sb_apply_schema = function(client) invisible(NULL),
    sb_ensure_implicit_transaction = function(client) invisible(NULL),
    sb_send_message = function(client, type, payload, flags = 0L, force_zero = FALSE) {
      sent[[length(sent) + 1L]] <<- list(type = type, payload = payload)
      client$sequence <- client$sequence + 1L
      client$sequence
    },
    sb_recv_message = local({
      step <- 0L
      function(client) {
        step <<- step + 1L
        if (step == 1L) {
          return(list(type = SB_MSG_AUTH_REQUEST, payload = c(pack_u8(SB_AUTH_SCRAM_SHA512), raw(3)), attachment_id = raw(16), txn_id = 0))
        }
        if (step == 2L) {
          client_first <- rawToChar(sent[[2]]$payload)
          nonce <- sub("^.*r=([^,]+).*$", "\\1", client_first)
          server_first <- paste0("r=", nonce, "server,s=", openssl::base64_encode(charToRaw("r-salt")), ",i=4096")
          payload <- c(pack_u8(SB_AUTH_SCRAM_SHA512), pack_u8(0L), raw(2), pack_u32(nchar(server_first)), charToRaw(server_first))
          return(list(type = SB_MSG_AUTH_CONTINUE, payload = payload, attachment_id = raw(16), txn_id = 0))
        }
        if (step == 3L) {
          return(list(type = SB_MSG_AUTH_OK, payload = c(as.raw(rep(0x10, 16)), pack_u32(0L)), attachment_id = as.raw(rep(0x10, 16)), txn_id = 0))
        }
        if (step == 4L) {
          return(list(type = SB_MSG_READY, payload = c(pack_u8(0L), raw(3), pack_u64(0), pack_u64(0)), attachment_id = as.raw(rep(0x10, 16)), txn_id = 0))
        }
        stop("unexpected extra recv")
      }
    }),
    .package = "scratchbird"
  )

  client <- sb_connect("scratchbird://user:secret@localhost:3092/mydb?sslmode=disable")
  ctx <- sb_get_resolved_auth_context(client)
  expect_equal(ctx$ingress_mode, "direct")
  expect_equal(ctx$resolved_auth_method, "SCRAM_SHA_512")
  expect_equal(ctx$resolved_auth_plugin_id, "scratchbird.auth.scram_sha_512")
  expect_false(ctx$manager_authenticated)
  expect_true(ctx$attached)
})

test_that("sb_connect tracks resolved TOKEN auth context", {
  sent <- list()
  local_mocked_bindings(
    sb_open_socket = function(cfg) list(socket = "mock"),
    sb_socket_close = function(con) invisible(NULL),
    sb_apply_schema = function(client) invisible(NULL),
    sb_ensure_implicit_transaction = function(client) invisible(NULL),
    sb_send_message = function(client, type, payload, flags = 0L, force_zero = FALSE) {
      sent[[length(sent) + 1L]] <<- list(type = type, payload = payload)
      client$sequence <- client$sequence + 1L
      client$sequence
    },
    sb_recv_message = local({
      step <- 0L
      function(client) {
        step <<- step + 1L
        if (step == 1L) {
          return(list(type = SB_MSG_AUTH_REQUEST, payload = c(pack_u8(SB_AUTH_TOKEN), raw(3)), attachment_id = raw(16), txn_id = 0))
        }
        if (step == 2L) {
          return(list(type = SB_MSG_AUTH_OK, payload = c(as.raw(rep(0x22, 16)), pack_u32(0L)), attachment_id = as.raw(rep(0x22, 16)), txn_id = 0))
        }
        if (step == 3L) {
          return(list(type = SB_MSG_READY, payload = c(pack_u8(0L), raw(3), pack_u64(0), pack_u64(0)), attachment_id = as.raw(rep(0x22, 16)), txn_id = 0))
        }
        stop("unexpected extra recv")
      }
    }),
    .package = "scratchbird"
  )

  client <- sb_connect("scratchbird://user:pass@localhost:3092/mydb?sslmode=disable&auth_token=token-123")
  ctx <- sb_get_resolved_auth_context(client)
  expect_equal(ctx$resolved_auth_method, "TOKEN")
  expect_equal(ctx$resolved_auth_plugin_id, "scratchbird.auth.authkey_token")
  expect_true(ctx$attached)
  expect_equal(rawToChar(sent[[2]]$payload), "token-123")
})

test_that("sb_connect fails closed for PEER auth", {
  local_mocked_bindings(
    sb_open_socket = function(cfg) list(socket = "mock"),
    sb_socket_close = function(con) invisible(NULL),
    sb_apply_schema = function(client) invisible(NULL),
    sb_ensure_implicit_transaction = function(client) invisible(NULL),
    sb_send_message = function(client, type, payload, flags = 0L, force_zero = FALSE) {
      client$sequence <- client$sequence + 1L
      client$sequence
    },
    sb_recv_message = function(client) {
      list(type = SB_MSG_AUTH_REQUEST, payload = c(pack_u8(SB_AUTH_PEER), raw(3)), attachment_id = raw(16), txn_id = 0)
    },
    .package = "scratchbird"
  )

  expect_error(
    sb_connect("scratchbird://user:pass@localhost:3092/mydb?sslmode=disable"),
    "requires external broker support"
  )
})
