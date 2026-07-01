# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

sb_connect <- function(dsn = "", ...) {
  cfg <- sb_config(dsn)
  if (length(list(...)) > 0) {
    overrides <- list(...)
    for (name in names(overrides)) {
      cfg <- apply_param(cfg, name, overrides[[name]])
    }
  }
  cfg$protocol <- normalize_native_protocol(cfg$protocol)
  cfg$front_door_mode <- normalize_front_door_mode(cfg$front_door_mode)
  if (cfg$user == "" || cfg$database == "") stop("user and database are required")
  con <- sb_open_socket(cfg)
  client <- new.env(parent = emptyenv())
  client$con <- con
  client$cfg <- cfg
  client$attachment_id <- raw(16)
  client$txn_id <- 0
  client$sequence <- 0
  client$last_query_sequence <- 0
  client$parameters <- list()
  client$notification_handlers <- list()
  client$last_plan <- NULL
  client$last_sblr <- NULL
  client$prepared <- new.env(parent = emptyenv())
  client$autocommit <- TRUE
  client$txn_active <- FALSE
  client$explicit_txn <- FALSE
  client$cancel_requested <- FALSE
  client$needs_reconnect <- FALSE
  client$portal_resume_pending <- FALSE
  client$resolved_auth_context <- sb_default_resolved_auth_context(cfg$front_door_mode)
  if (identical(cfg$front_door_mode, "manager_proxy")) {
    sb_perform_manager_connect(client)
  }
  sb_startup_and_auth(client)
  sb_ensure_implicit_transaction(client)
  sb_apply_schema(client)
  client
}

sb_disconnect <- function(client) {
  try(sb_socket_close(client$con), silent = TRUE)
  client$con <- NULL
  sb_clear_transaction_state(client)
  client$cancel_requested <- FALSE
  client$needs_reconnect <- FALSE
  client$portal_resume_pending <- FALSE
  if (!is.null(client$resolved_auth_context)) {
    client$resolved_auth_context$attached <- FALSE
  }
}

sb_probe_auth_surface <- function(dsn = "", ...) {
  cfg <- sb_config(dsn)
  if (length(list(...)) > 0) {
    overrides <- list(...)
    for (name in names(overrides)) {
      cfg <- apply_param(cfg, name, overrides[[name]])
    }
  }

  cfg$protocol <- normalize_native_protocol(cfg$protocol)
  cfg$front_door_mode <- normalize_front_door_mode(cfg$front_door_mode)
  con <- sb_open_socket(cfg)
  on.exit(try(sb_socket_close(con), silent = TRUE), add = TRUE)

  client <- new.env(parent = emptyenv())
  client$con <- con
  client$cfg <- cfg
  client$sequence <- 0L
  client$attachment_id <- raw(16)
  client$txn_id <- 0
  client$resolved_auth_context <- sb_default_resolved_auth_context(cfg$front_door_mode)

  if (identical(cfg$front_door_mode, "manager_proxy")) {
    return(sb_probe_manager_auth_surface(client))
  }

  sb_probe_direct_auth_surface(client)
}

sb_get_resolved_auth_context <- function(client) {
  client$resolved_auth_context
}

sb_transaction_active <- function(client) {
  isTRUE(client$txn_active) || (!is.null(client$txn_id) && is.finite(client$txn_id) && client$txn_id != 0)
}

sb_clear_transaction_state <- function(client) {
  client$txn_id <- 0
  client$txn_active <- FALSE
  client$explicit_txn <- FALSE
  invisible(NULL)
}

sb_apply_runtime_txn_id <- function(client, txn_id) {
  parsed <- suppressWarnings(as.numeric(txn_id))
  if (is.na(parsed) || parsed <= 0) {
    client$txn_id <- 0
    return(invisible(NULL))
  }
  client$txn_id <- parsed
  client$txn_active <- TRUE
  invisible(NULL)
}

sb_apply_runtime_ready_state <- function(client, status, txn_id) {
  parsed_status <- suppressWarnings(as.integer(status))
  parsed_txn <- suppressWarnings(as.numeric(txn_id))
  if (is.na(parsed_txn) || parsed_txn < 0) parsed_txn <- 0
  client$txn_id <- parsed_txn
  if (!is.na(parsed_status) && parsed_status != 0L) {
    # READY is authoritative for native MGA transaction activity. The
    # engine can publish a fresh active boundary while the public wire
    # header still reports txn_id == 0.
    client$txn_active <- TRUE
    return(invisible(NULL))
  }
  sb_clear_transaction_state(client)
}

sb_can_adopt_fresh_native_boundary <- function(args) {
  arg_names <- names(args)
  if (is.null(arg_names)) arg_names <- character()
  isolation <- if ("isolation_level" %in% arg_names) args$isolation_level else SB_ISOLATION_READ_COMMITTED
  normalized_isolation <- suppressWarnings(as.integer(isolation))
  if (is.na(normalized_isolation) ||
      !(normalized_isolation %in% c(SB_ISOLATION_READ_UNCOMMITTED, SB_ISOLATION_READ_COMMITTED))) {
    return(FALSE)
  }
  read_committed_mode <- if ("read_committed_mode" %in% arg_names) {
    args$read_committed_mode
  } else {
    SB_READ_COMMITTED_MODE_DEFAULT
  }
  normalized_rc_mode <- suppressWarnings(as.integer(read_committed_mode))
  if (is.na(normalized_rc_mode) || normalized_rc_mode != SB_READ_COMMITTED_MODE_DEFAULT) {
    return(FALSE)
  }
  if ("access_mode" %in% arg_names ||
      "deferrable" %in% arg_names ||
      "wait" %in% arg_names ||
      "timeout_ms" %in% arg_names ||
      "autocommit_mode" %in% arg_names) {
    return(FALSE)
  }
  conflict_action <- if ("conflict_action" %in% arg_names) args$conflict_action else 0L
  normalized_conflict <- suppressWarnings(as.integer(conflict_action))
  !is.na(normalized_conflict) && normalized_conflict == 0L
}

sb_set_autocommit <- function(client, value) {
  client$autocommit <- isTRUE(value)
}

sb_is_valid <- function(client) {
  !is.null(client$con)
}

sb_prepare_connection <- function(client) {
  # MGA recovery rule: reconnect only repairs the transport/session surface.
  # Local transaction/prepared state is reset so the lane never treats an
  # abandoned in-flight transaction as resumable after reconnect.
  if (!isTRUE(client$needs_reconnect) && !is.null(client$con)) {
    return(invisible(NULL))
  }

  if (!is.null(client$con)) {
    try(sb_socket_close(client$con), silent = TRUE)
  }

  client$con <- sb_open_socket(client$cfg)
  client$attachment_id <- raw(16)
  sb_clear_transaction_state(client)
  client$sequence <- 0
  client$last_query_sequence <- 0
  client$parameters <- list()
  client$last_plan <- NULL
  client$last_sblr <- NULL
  client$prepared <- new.env(parent = emptyenv())
  client$autocommit <- TRUE
  client$cancel_requested <- FALSE
  client$portal_resume_pending <- FALSE
  client$resolved_auth_context <- sb_default_resolved_auth_context(client$cfg$front_door_mode)

  if (identical(client$cfg$front_door_mode, "manager_proxy")) {
    sb_perform_manager_connect(client)
  }
  sb_startup_and_auth(client)
  sb_ensure_implicit_transaction(client)
  sb_apply_schema(client)
  client$needs_reconnect <- FALSE
  invisible(NULL)
}

sb_query <- function(client, sql, params = NULL) {
  normalized <- sb_normalize(sql, params)
  result <- sb_execute_query(client, normalized$sql, normalized$params)
  result$rows <- sb_fetch_rows(result, -1)
  result
}

sb_get_query <- function(client, sql, params = NULL) {
  result <- sb_query(client, sql, params)
  sb_result_to_df(result)
}

sb_send_query <- function(client, sql, params = NULL) {
  normalized <- sb_normalize(sql, params)
  sb_execute_query(client, normalized$sql, normalized$params)
}

sb_fetch <- function(result, n = -1) {
  rows <- sb_fetch_rows(result, n)
  sb_rows_to_df(rows, result$columns)
}

sb_clear_result <- function(result) {
  result$pending_rows <- list()
  result$done <- TRUE
  result
}

sb_cancel <- function(client) {
  # Cancel tears down the live wire and forces a clean reconnect path. The next
  # operation must re-enter through engine truth instead of resuming local TXN state.
  client$cancel_requested <- TRUE
  payload <- build_cancel_payload(0L, client$last_query_sequence)
  if (!is.null(client$con)) {
    try(sb_send_message(client, SB_MSG_CANCEL, payload, SB_MSG_FLAG_URGENT, FALSE), silent = TRUE)
    try(sb_socket_close(client$con), silent = TRUE)
  }
  client$con <- NULL
  sb_clear_transaction_state(client)
  client$portal_resume_pending <- FALSE
  client$needs_reconnect <- TRUE
  invisible(NULL)
}

sb_begin <- function(client, ...) {
  args <- list(...)
  flags <- 0L
  has_isolation <- "isolation_level" %in% names(args)
  isolation <- if (has_isolation) args$isolation_level else SB_ISOLATION_READ_COMMITTED
  read_committed_mode <- if ("read_committed_mode" %in% names(args)) args$read_committed_mode else NULL
  if (!is.null(read_committed_mode)) {
    normalized_isolation <- suppressWarnings(as.integer(isolation))
    if (is.na(normalized_isolation) ||
        !(normalized_isolation %in% c(SB_ISOLATION_READ_UNCOMMITTED, SB_ISOLATION_READ_COMMITTED))) {
      stop("read_committed_mode requires a READ COMMITTED isolation alias")
    }
    flags <- bitwOr(flags, SB_TXN_FLAG_HAS_READ_COMMITTED_MODE)
    if (!has_isolation) {
      isolation <- SB_ISOLATION_READ_COMMITTED
      has_isolation <- TRUE
    }
  }
  if (sb_transaction_active(client)) {
    if (isTRUE(client$explicit_txn)) {
      stop("Transaction already active")
    }
    if (!sb_can_adopt_fresh_native_boundary(args)) {
      sb_stop_sqlstate(
        "0A000",
        "non-default transaction options cannot adopt an existing fresh native boundary"
      )
    }
    client$explicit_txn <- TRUE
    return(invisible(NULL))
  }
  if (has_isolation) flags <- bitwOr(flags, SB_TXN_FLAG_HAS_ISOLATION)
  if ("access_mode" %in% names(args)) flags <- bitwOr(flags, SB_TXN_FLAG_HAS_ACCESS)
  if ("deferrable" %in% names(args)) flags <- bitwOr(flags, SB_TXN_FLAG_HAS_DEFERRABLE)
  if ("wait" %in% names(args)) flags <- bitwOr(flags, SB_TXN_FLAG_HAS_WAIT)
  if ("timeout_ms" %in% names(args)) flags <- bitwOr(flags, SB_TXN_FLAG_HAS_TIMEOUT)
  if ("autocommit_mode" %in% names(args)) flags <- bitwOr(flags, SB_TXN_FLAG_HAS_AUTOCOMMIT)
  payload <- build_txn_begin_payload(
    flags,
    if (!is.null(args$conflict_action)) args$conflict_action else 0L,
    if (!is.null(args$autocommit_mode)) args$autocommit_mode else 0L,
    isolation,
    if (!is.null(args$access_mode)) args$access_mode else 0L,
    if (isTRUE(args$deferrable)) 1L else 0L,
    if (isTRUE(args$wait)) 1L else 0L,
    if (!is.null(args$timeout_ms)) args$timeout_ms else 0L,
    if (!is.null(read_committed_mode)) read_committed_mode else SB_READ_COMMITTED_MODE_DEFAULT
  )
  sb_send_message(client, SB_MSG_TXN_BEGIN, payload, 0L, FALSE)
  sb_drain_until_ready(client)
  client$explicit_txn <- TRUE
}

sb_canonical_isolation_label <- function(isolation_level = SB_ISOLATION_READ_COMMITTED) {
  normalized <- suppressWarnings(as.integer(isolation_level))
  if (is.na(normalized)) {
    return(paste0("UNKNOWN(", deparse(isolation_level), ")"))
  }
  if (normalized == SB_ISOLATION_READ_UNCOMMITTED) return("READ COMMITTED")
  if (normalized == SB_ISOLATION_READ_COMMITTED) return("READ COMMITTED")
  if (normalized == SB_ISOLATION_REPEATABLE_READ) return("SNAPSHOT")
  if (normalized == SB_ISOLATION_SERIALIZABLE) return("SNAPSHOT TABLE STABILITY")
  paste0("UNKNOWN(", normalized, ")")
}

sb_canonical_read_committed_mode_label <- function(read_committed_mode = SB_READ_COMMITTED_MODE_DEFAULT) {
  normalized <- suppressWarnings(as.integer(read_committed_mode))
  if (is.na(normalized)) {
    return(paste0("UNKNOWN(", deparse(read_committed_mode), ")"))
  }
  if (normalized == SB_READ_COMMITTED_MODE_DEFAULT) return("READ COMMITTED")
  if (normalized == SB_READ_COMMITTED_MODE_READ_CONSISTENCY) return("READ COMMITTED READ CONSISTENCY")
  if (normalized == SB_READ_COMMITTED_MODE_RECORD_VERSION) return("READ COMMITTED RECORD VERSION")
  if (normalized == SB_READ_COMMITTED_MODE_NO_RECORD_VERSION) return("READ COMMITTED NO RECORD VERSION")
  paste0("UNKNOWN(", normalized, ")")
}

sb_supports_prepared_transactions <- function(client) {
  TRUE
}

sb_supports_dormant_reattach <- function(client) {
  FALSE
}

sb_prepare_transaction <- function(client, global_transaction_id) {
  sql <- sb_build_prepared_transaction_sql("PREPARE TRANSACTION", global_transaction_id)
  result <- sb_execute_query(client, sql)
  sb_fetch_rows(result, -1)
  invisible(NULL)
}

sb_commit_prepared <- function(client, global_transaction_id) {
  sql <- sb_build_prepared_transaction_sql("COMMIT PREPARED", global_transaction_id)
  result <- sb_execute_query(client, sql)
  sb_fetch_rows(result, -1)
  invisible(NULL)
}

sb_rollback_prepared <- function(client, global_transaction_id) {
  sql <- sb_build_prepared_transaction_sql("ROLLBACK PREPARED", global_transaction_id)
  result <- sb_execute_query(client, sql)
  sb_fetch_rows(result, -1)
  invisible(NULL)
}

sb_detach_to_dormant <- function(client) {
  sb_stop_sqlstate("0A000", "dormant detach is not supported by the R driver")
}

sb_reattach_dormant <- function(client, dormant_id, auth_token = NULL) {
  sb_stop_sqlstate("0A000", "dormant reattach is not supported by the R driver")
}

sb_commit <- function(client, flags = 0L) {
  if (!sb_transaction_active(client)) stop("commit requires an active transaction")
  result <- sb_execute_query(client, "COMMIT")
  sb_fetch_rows(result, -1)
  client$explicit_txn <- FALSE
  sb_ensure_implicit_transaction(client)
}

sb_rollback <- function(client, flags = 0L) {
  if (!sb_transaction_active(client)) stop("rollback requires an active transaction")
  result <- sb_execute_query(client, "ROLLBACK")
  sb_fetch_rows(result, -1)
  client$explicit_txn <- FALSE
  sb_ensure_implicit_transaction(client)
}

sb_savepoint <- function(client, name) {
  if (!sb_transaction_active(client)) stop("savepoint requires an active transaction")
  result <- sb_execute_query(client, paste("SAVEPOINT", quote_identifier(name)))
  sb_fetch_rows(result, -1)
}

sb_release_savepoint <- function(client, name) {
  if (!sb_transaction_active(client)) stop("release savepoint requires an active transaction")
  result <- sb_execute_query(client, paste("RELEASE SAVEPOINT", quote_identifier(name)))
  sb_fetch_rows(result, -1)
}

sb_rollback_to_savepoint <- function(client, name) {
  if (!sb_transaction_active(client)) stop("rollback to savepoint requires an active transaction")
  result <- sb_execute_query(client, paste("ROLLBACK TO SAVEPOINT", quote_identifier(name)))
  sb_fetch_rows(result, -1)
}

sb_set_option <- function(client, name, value) {
  payload <- build_set_option_payload(name, value)
  sb_send_message(client, SB_MSG_SET_OPTION, payload, 0L, FALSE)
  sb_drain_until_ready(client)
}

sb_ping <- function(client) {
  sb_prepare_connection(client)
  sb_send_message(client, SB_MSG_PING, raw(), 0L, FALSE)
  repeat {
    response <- sb_recv_message(client)
    if (sb_handle_async(client, response$type, response$payload)) next
    if (response$type == SB_MSG_PONG) return(invisible(NULL))
    if (response$type == SB_MSG_READY) {
      parsed <- parse_ready(response$payload)
      sb_apply_runtime_ready_state(client, parsed$status, parsed$txn_id)
      return(invisible(NULL))
    }
    if (response$type == SB_MSG_ERROR) sb_raise_query_error(response$payload)
  }
}

sb_terminate <- function(client) {
  if (is.null(client$con)) return(invisible(NULL))
  sb_send_message(client, SB_MSG_TERMINATE, raw(), 0L, FALSE)
  sb_disconnect(client)
}

sb_subscribe <- function(client, channel, subscribe_type = SB_SUB_TYPE_CHANNEL, filter_expr = "") {
  payload <- build_subscribe_payload(subscribe_type, channel, filter_expr)
  sb_send_message(client, SB_MSG_SUBSCRIBE, payload, 0L, FALSE)
  sb_drain_until_ready(client)
}

sb_unsubscribe <- function(client, channel) {
  payload <- build_unsubscribe_payload(channel)
  sb_send_message(client, SB_MSG_UNSUBSCRIBE, payload, 0L, FALSE)
  sb_drain_until_ready(client)
}

sb_execute_sblr <- function(client, sblr_hash, sblr_bytecode, params = list()) {
  sb_prepare_connection(client)
  encoded <- lapply(params, function(param) encode_param(param)$param)
  payload <- build_sblr_execute_payload(sblr_hash, sblr_bytecode, encoded)
  client$last_plan <- NULL
  client$last_sblr <- NULL
  client$last_query_sequence <- sb_send_message(client, SB_MSG_SBLR_EXECUTE, payload, 0L, FALSE)
  sb_send_message(client, SB_MSG_SYNC, raw(), 0L, FALSE)
  result <- new.env(parent = emptyenv())
  result$client <- client
  result$columns <- list()
  result$rowcount <- -1
  result$command_tag <- ""
  result$done <- FALSE
  result$page_size <- 0L
  result$pending_rows <- list()
  result$response_started <- FALSE
  result$ignored_stray_ready <- FALSE
  result
}

sb_stream_control <- function(client, control_type, window_size = 0L, timeout_ms = 0L) {
  payload <- build_stream_control_payload(control_type, window_size, timeout_ms)
  sb_send_message(client, SB_MSG_STREAM_CONTROL, payload, 0L, FALSE)
}

sb_attach_create <- function(client, emulation_mode, db_name) {
  payload <- build_attach_create_payload(emulation_mode, db_name)
  sb_send_message(client, SB_MSG_ATTACH_CREATE, payload, 0L, FALSE)
  sb_drain_until_ready(client)
}

sb_attach_detach <- function(client) {
  sb_send_message(client, SB_MSG_ATTACH_DETACH, raw(), 0L, FALSE)
  sb_drain_until_ready(client)
}

sb_attach_list <- function(client) {
  sb_send_message(client, SB_MSG_ATTACH_LIST, raw(), 0L, FALSE)
  sb_send_message(client, SB_MSG_SYNC, raw(), 0L, FALSE)
  result <- new.env(parent = emptyenv())
  result$client <- client
  result$columns <- list()
  result$rowcount <- -1
  result$command_tag <- ""
  result$done <- FALSE
  result$page_size <- 0L
  result$pending_rows <- list()
  result$response_started <- FALSE
  result$ignored_stray_ready <- FALSE
  result
}

sb_on_notification <- function(client, handler) {
  client$notification_handlers[[length(client$notification_handlers) + 1]] <- handler
  invisible(NULL)
}

sb_get_last_plan <- function(client) {
  client$last_plan
}

sb_get_last_sblr <- function(client) {
  client$last_sblr
}

sb_open_socket <- function(cfg) {
  sb_tls_connect_native(cfg)
}

sb_socket_write <- function(con, data) {
  if (is.null(con)) stop("connection closed")
  sb_tls_write_native(con, data)
  invisible(NULL)
}

sb_socket_read_exact <- function(con, n) {
  if (is.null(con)) stop("connection closed")
  sb_tls_read_exact_native(con, n)
}

sb_socket_close <- function(con) {
  if (is.null(con)) return(invisible(NULL))
  sb_tls_close_native(con)
  invisible(NULL)
}

SB_MANAGER_PROTOCOL_MAGIC <- 0x42444253L
SB_MANAGER_PROTOCOL_VERSION <- 0x0101L
SB_MANAGER_HEADER_SIZE <- 12L
SB_MANAGER_MAX_PAYLOAD_SIZE <- 16L * 1024L * 1024L
SB_MCP_PROTOCOL_VERSION <- 0x0100L

SB_MCP_MSG_CONNECT_RESPONSE <- 0x02L
SB_MCP_MSG_AUTH_CHALLENGE <- 0x12L
SB_MCP_MSG_AUTH_RESPONSE <- 0x11L
SB_MCP_MSG_STATUS_RESPONSE <- 0x64L
SB_MCP_MSG_HELLO <- 0x65L
SB_MCP_MSG_AUTH_START <- 0x66L
SB_MCP_MSG_AUTH_CONTINUE <- 0x67L
SB_MCP_MSG_DB_CONNECT <- 0x69L
SB_MCP_AUTH_METHOD_TOKEN <- 4L

sb_build_lp_string <- function(value) {
  bytes <- charToRaw(enc2utf8(value))
  c(pack_u32(length(bytes)), bytes)
}

sb_send_manager_frame <- function(client, type, payload) {
  frame <- c(
    pack_u32(SB_MANAGER_PROTOCOL_MAGIC),
    pack_u16(SB_MANAGER_PROTOCOL_VERSION),
    pack_u8(type),
    pack_u8(0L),
    pack_u32(length(payload)),
    payload
  )
  sb_socket_write(client$con, frame)
  invisible(NULL)
}

sb_recv_manager_frame <- function(client) {
  header <- sb_socket_read_exact(client$con, SB_MANAGER_HEADER_SIZE)
  if (length(header) != SB_MANAGER_HEADER_SIZE) stop("Manager frame read failed")
  magic <- read_u32(header, 1)
  if (magic != SB_MANAGER_PROTOCOL_MAGIC) stop("Manager frame magic mismatch")
  version <- read_u16(header, 5)
  if (version != SB_MANAGER_PROTOCOL_VERSION) stop("Manager frame version mismatch")
  type <- read_u8(header, 7)
  payload_len <- read_u32(header, 9)
  if (payload_len > SB_MANAGER_MAX_PAYLOAD_SIZE) stop("Manager payload too large")
  payload <- if (payload_len > 0) sb_socket_read_exact(client$con, payload_len) else raw()
  list(type = type, payload = payload)
}

sb_perform_manager_connect <- function(client) {
  token <- if (!is.null(client$cfg$manager_auth_token)) client$cfg$manager_auth_token else ""
  if (token == "") stop("manager_proxy mode requires manager_auth_token")

  manager_user <- if (!is.null(client$cfg$manager_username) && client$cfg$manager_username != "") {
    client$cfg$manager_username
  } else if (client$cfg$user != "") {
    client$cfg$user
  } else {
    "admin"
  }
  manager_database <- if (!is.null(client$cfg$manager_database) && client$cfg$manager_database != "") {
    client$cfg$manager_database
  } else {
    client$cfg$database
  }
  manager_profile <- if (!is.null(client$cfg$manager_connection_profile) && client$cfg$manager_connection_profile != "") {
    client$cfg$manager_connection_profile
  } else {
    "SBsql"
  }
  manager_intent <- if (!is.null(client$cfg$manager_client_intent) && client$cfg$manager_client_intent != "") {
    client$cfg$manager_client_intent
  } else {
    "SBsql"
  }
  manager_flags <- suppressWarnings(as.integer(client$cfg$manager_client_flags))
  if (is.na(manager_flags)) manager_flags <- 0L
  auth_fast_path <- isTRUE(client$cfg$manager_auth_fast_path)

  hello <- c(pack_u16(SB_MCP_PROTOCOL_VERSION), pack_u16(bitwAnd(manager_flags, 0xFFFFL)))
  sb_send_manager_frame(client, SB_MCP_MSG_HELLO, hello)
  frame <- sb_recv_manager_frame(client)
  if (frame$type != SB_MCP_MSG_STATUS_RESPONSE) stop("Expected MCP hello status response")

  auth_start <- c(sb_build_lp_string(manager_user), pack_u8(SB_MCP_AUTH_METHOD_TOKEN))
  if (auth_fast_path) {
    token_bytes <- charToRaw(enc2utf8(token))
    auth_start <- c(auth_start, pack_u32(length(token_bytes)), token_bytes)
  } else {
    auth_start <- c(auth_start, pack_u32(0L))
  }
  sb_send_manager_frame(client, SB_MCP_MSG_AUTH_START, auth_start)
  frame <- sb_recv_manager_frame(client)
  if (frame$type == SB_MCP_MSG_AUTH_CHALLENGE) {
    token_bytes <- charToRaw(enc2utf8(token))
    auth_continue <- c(pack_u32(length(token_bytes)), token_bytes)
    sb_send_manager_frame(client, SB_MCP_MSG_AUTH_CONTINUE, auth_continue)
    frame <- sb_recv_manager_frame(client)
  }
  if (frame$type != SB_MCP_MSG_AUTH_RESPONSE) stop("Expected MCP auth response")
  if (length(frame$payload) < 1 + 4 + 256) stop("Truncated MCP auth response")
  if (frame$payload[1] != as.raw(0x00)) {
    err_raw <- frame$payload[6:261]
    err_raw <- err_raw[err_raw != as.raw(0x00)]
    err <- if (length(err_raw) > 0) rawToChar(err_raw) else ""
    if (trimws(err) == "") err <- "MCP authentication failed"
    stop(err)
  }

  nonce <- as.raw(sample.int(256, 16, replace = TRUE) - 1L)
  db_connect <- c(
    charToRaw("MCP1"),
    sb_build_lp_string(manager_database),
    sb_build_lp_string(manager_profile),
    sb_build_lp_string(manager_intent),
    pack_u16(length(nonce)),
    nonce
  )
  sb_send_manager_frame(client, SB_MCP_MSG_DB_CONNECT, db_connect)
  frame <- sb_recv_manager_frame(client)
  if (frame$type != SB_MCP_MSG_CONNECT_RESPONSE) stop("Expected MCP connect response")
  if (length(frame$payload) < 1 + 2 + 2 + 16 + 64 + 32) stop("Truncated MCP connect response")
  if (frame$payload[1] != as.raw(0x00)) {
    message <- "MCP database connect failed"
    err_offset <- 1 + 2 + 2 + 16 + 64 + 32
    err_len_index <- err_offset + 1
    if (length(frame$payload) >= err_len_index + 3) {
      err_len <- read_u32(frame$payload, err_len_index)
      err_start <- err_len_index + 4
      err_end <- err_start + err_len - 1
      if (length(frame$payload) >= err_end && err_len > 0) {
        message <- rawToChar(frame$payload[err_start:err_end])
      }
    }
    stop(message)
  }

  client$resolved_auth_context$ingress_mode <- "manager_proxy"
  client$resolved_auth_context$manager_authenticated <- TRUE
}

sb_build_startup_params <- function(client) {
  params <- list(
    database = client$cfg$database,
    user = client$cfg$user,
    client_flags = as.character(if (is.null(client$cfg$connect_client_flags)) 0L else client$cfg$connect_client_flags)
  )
  if (!is.null(client$cfg$role) && trimws(client$cfg$role) != "") params$role <- client$cfg$role
  if (!is.null(client$cfg$application_name) && trimws(client$cfg$application_name) != "") {
    params$application_name <- client$cfg$application_name
  }

  dormant_id <- trimws(as.character(if (is.null(client$cfg$dormant_id)) "" else client$cfg$dormant_id))
  dormant_token <- trimws(as.character(if (is.null(client$cfg$dormant_reattach_token)) "" else client$cfg$dormant_reattach_token))
  if (xor(nzchar(dormant_id), nzchar(dormant_token))) {
    stop("dormant_id and dormant_reattach_token must be provided together")
  }
  if (nzchar(dormant_id) && nzchar(dormant_token)) {
    params$dormant_id <- dormant_id
    params$dormant_reattach_token <- dormant_token
  }

  sb_apply_auth_plugin_selection(params, client$cfg)
}

sb_requested_features <- function(cfg) {
  features <- 0L
  if (tolower(cfg$compression) == "zstd") features <- bitwOr(features, SB_FEATURE_COMPRESSION)
  if (isTRUE(cfg$binary_transfer)) features <- bitwOr(features, SB_FEATURE_STREAMING)
  features
}

sb_probe_direct_auth_surface <- function(client) {
  startup <- build_startup_payload(sb_requested_features(client$cfg), sb_build_startup_params(client))
  sb_send_message(client, SB_MSG_STARTUP, startup, 0L, TRUE)

  repeat {
    response <- sb_recv_message(client)
    type <- response$type
    payload <- response$payload

    if (type == SB_MSG_NEGOTIATE_VERSION) {
      next
    } else if (type == SB_MSG_AUTH_REQUEST) {
      parsed <- parse_auth_request(payload)
      method <- sb_describe_auth_method(parsed$method, client$cfg$auth_method_id)
      return(list(
        reachable = TRUE,
        ingress_mode = "direct",
        resolved_host = as.character(if (is.null(client$cfg$host)) "" else client$cfg$host),
        resolved_port = as.integer(if (is.null(client$cfg$port)) 0L else client$cfg$port),
        admitted_methods = if (is.null(method)) list() else list(method),
        required_method = if (is.null(method)) NULL else method$wire_method,
        required_plugin_method_id = if (is.null(method)) NULL else method$plugin_method_id,
        allowed_transport_mask = NULL,
        additional_continuation_possible = sb_additional_continuation_possible(parsed$method)
      ))
    } else if (type %in% c(SB_MSG_AUTH_OK, SB_MSG_READY)) {
      return(list(
        reachable = TRUE,
        ingress_mode = "direct",
        resolved_host = as.character(if (is.null(client$cfg$host)) "" else client$cfg$host),
        resolved_port = as.integer(if (is.null(client$cfg$port)) 0L else client$cfg$port),
        admitted_methods = list(),
        required_method = NULL,
        required_plugin_method_id = NULL,
        allowed_transport_mask = NULL,
        additional_continuation_possible = FALSE
      ))
    } else if (type == SB_MSG_ERROR) {
      sb_raise_query_error(payload)
    }
  }
}

sb_probe_manager_auth_surface <- function(client) {
  manager_flags <- suppressWarnings(as.integer(client$cfg$manager_client_flags))
  if (is.na(manager_flags)) manager_flags <- 0L
  hello <- c(pack_u16(SB_MCP_PROTOCOL_VERSION), pack_u16(bitwAnd(manager_flags, 0xFFFFL)))
  sb_send_manager_frame(client, SB_MCP_MSG_HELLO, hello)
  frame <- sb_recv_manager_frame(client)
  if (frame$type != SB_MCP_MSG_STATUS_RESPONSE) stop("Expected MCP hello status response")

  method <- sb_describe_auth_method(SB_AUTH_TOKEN, client$cfg$auth_method_id)
  list(
    reachable = TRUE,
    ingress_mode = "manager_proxy",
    resolved_host = as.character(if (is.null(client$cfg$host)) "" else client$cfg$host),
    resolved_port = as.integer(if (is.null(client$cfg$port)) 0L else client$cfg$port),
    admitted_methods = if (is.null(method)) list() else list(method),
    required_method = if (is.null(method)) NULL else method$wire_method,
    required_plugin_method_id = if (is.null(method)) NULL else method$plugin_method_id,
    allowed_transport_mask = NULL,
    additional_continuation_possible = TRUE
  )
}

sb_startup_and_auth <- function(client) {
  startup <- build_startup_payload(sb_requested_features(client$cfg), sb_build_startup_params(client))
  sb_send_message(client, SB_MSG_STARTUP, startup, 0L, TRUE)

  scram <- NULL

  repeat {
    response <- sb_recv_message(client)
    type <- response$type
    payload <- response$payload
    if (type == SB_MSG_NEGOTIATE_VERSION) {
      next
    } else if (type == SB_MSG_AUTH_REQUEST) {
      parsed <- parse_auth_request(payload)
      if (parsed$method == SB_AUTH_OK) {
        next
      }
      if (parsed$method == SB_AUTH_PASSWORD) {
        client$resolved_auth_context$resolved_auth_method <- "PASSWORD"
        client$resolved_auth_context$resolved_auth_plugin_id <- sb_auth_plugin_id_for_method(parsed$method, client$cfg$auth_method_id)
        sb_send_message(client, SB_MSG_AUTH_RESPONSE, charToRaw(client$cfg$password), 0L, TRUE)
        next
      }
      if (parsed$method == SB_AUTH_SCRAM_SHA256) {
        if (is.null(scram)) scram <- sb_scram_client(client$cfg$user, "sha256")
        client$resolved_auth_context$resolved_auth_method <- "SCRAM_SHA_256"
        client$resolved_auth_context$resolved_auth_plugin_id <- sb_auth_plugin_id_for_method(parsed$method, client$cfg$auth_method_id)
        first <- sb_scram_client_first(scram)
        scram <- first$state
        sb_send_message(client, SB_MSG_AUTH_RESPONSE, charToRaw(first$message), 0L, TRUE)
        next
      }
      if (parsed$method == SB_AUTH_SCRAM_SHA512) {
        if (is.null(scram)) scram <- sb_scram_client(client$cfg$user, "sha512")
        client$resolved_auth_context$resolved_auth_method <- "SCRAM_SHA_512"
        client$resolved_auth_context$resolved_auth_plugin_id <- sb_auth_plugin_id_for_method(parsed$method, client$cfg$auth_method_id)
        first <- sb_scram_client_first(scram)
        scram <- first$state
        sb_send_message(client, SB_MSG_AUTH_RESPONSE, charToRaw(first$message), 0L, TRUE)
        next
      }
      if (parsed$method == SB_AUTH_TOKEN) {
        token_payload <- sb_resolve_token_auth_payload(client$cfg)
        if (length(token_payload) == 0) stop("TOKEN auth requires auth_token or equivalent auth payload input")
        client$resolved_auth_context$resolved_auth_method <- "TOKEN"
        client$resolved_auth_context$resolved_auth_plugin_id <- sb_auth_plugin_id_for_method(parsed$method, client$cfg$auth_method_id)
        sb_send_message(client, SB_MSG_AUTH_RESPONSE, token_payload, 0L, TRUE)
        next
      }
      if (parsed$method == SB_AUTH_PEER) {
        stop("Admitted auth method PEER requires external broker support in this lane")
      }
      if (parsed$method == SB_AUTH_MD5) {
        stop("Admitted auth method MD5 is not executable locally in this lane")
      }
      if (parsed$method == SB_AUTH_REATTACH) {
        stop("Admitted auth method REATTACH is not executable locally in this lane")
      }
      stop("Unsupported auth method")
    } else if (type == SB_MSG_AUTH_CONTINUE) {
      parsed <- parse_auth_continue(payload)
      if (parsed$method %in% c(SB_AUTH_SCRAM_SHA256, SB_AUTH_SCRAM_SHA512) && !is.null(scram)) {
        server_first <- rawToChar(parsed$data)
        final <- sb_scram_handle_server_first(scram, client$cfg$password, server_first)
        scram <- final$state
        sb_send_message(client, SB_MSG_AUTH_RESPONSE, charToRaw(final$message), 0L, TRUE)
        next
      }
      if (parsed$method == SB_AUTH_TOKEN) {
        token_payload <- sb_resolve_token_auth_payload(client$cfg)
        if (length(token_payload) == 0) stop("TOKEN auth requires auth_token or equivalent auth payload input")
        client$resolved_auth_context$resolved_auth_method <- "TOKEN"
        client$resolved_auth_context$resolved_auth_plugin_id <- sb_auth_plugin_id_for_method(parsed$method, client$cfg$auth_method_id)
        sb_send_message(client, SB_MSG_AUTH_RESPONSE, token_payload, 0L, TRUE)
        next
      }
      if (parsed$method == SB_AUTH_PEER) {
        stop("Admitted auth method PEER requires external broker support in this lane")
      }
      stop("Unsupported auth continue")
    } else if (type == SB_MSG_AUTH_OK) {
      parsed <- parse_auth_ok(payload)
      client$attachment_id <- parsed$session_id
      sb_apply_runtime_txn_id(client, response$txn_id)
      if (!is.null(scram) && length(parsed$server_info) > 0) {
        server_info <- rawToChar(parsed$server_info)
        if (startsWith(server_info, "v=")) sb_scram_verify_server_final(scram, server_info)
      }
      next
    } else if (type == SB_MSG_PARAMETER_STATUS) {
      parsed <- parse_parameter_status(payload)
      for (entry in parsed$entries) sb_handle_parameter_status(client, entry$name, entry$value)
      next
    } else if (type == SB_MSG_READY) {
      parsed <- parse_ready(payload)
      sb_apply_runtime_ready_state(client, parsed$status, parsed$txn_id)
      client$resolved_auth_context$attached <- TRUE
      break
    } else if (type == SB_MSG_ERROR) {
      sb_raise_query_error(payload)
    }
  }
}

sb_apply_schema <- function(client) {
  schema <- trimws(client$cfg$schema)
  if (schema == "" || tolower(schema) == "public") return(invisible(NULL))
  statement <- build_schema_statement(schema)
  if (statement == "") return(invisible(NULL))
  sb_execute_query(client, statement)
  invisible(NULL)
}

build_schema_statement <- function(schema) {
  trimmed <- trimws(schema)
  if (trimmed == "") return("")
  if (grepl(",", trimmed, fixed = TRUE)) {
    parts <- trimws(strsplit(trimmed, ",", fixed = TRUE)[[1]])
    parts <- parts[parts != ""]
    if (length(parts) == 0) return("")
    quoted <- vapply(parts, quote_identifier, character(1))
    return(paste("SET SEARCH_PATH TO", paste(quoted, collapse = ", ")))
  }
  paste("SET SCHEMA", quote_identifier(trimmed))
}

quote_identifier <- function(name) {
  paste0('"', gsub('"', '""', name, fixed = TRUE), '"')
}

sb_quote_string_literal <- function(value) {
  paste0("'", gsub("'", "''", value, fixed = TRUE), "'")
}

sb_build_prepared_transaction_sql <- function(verb, global_transaction_id) {
  normalized <- trimws(global_transaction_id)
  if (!nzchar(normalized)) {
    sb_stop_sqlstate("42601", "global transaction id is required")
  }
  paste(verb, sb_quote_string_literal(normalized))
}

sb_execute_query <- function(client, sql, params = list()) {
  sb_prepare_connection(client)
  sb_ensure_implicit_transaction(client)
  page_size <- if (!is.null(client$cfg$fetch_size) && client$cfg$fetch_size > 0) client$cfg$fetch_size else 0L
  if (length(params) == 0) {
    sb_send_simple_query(client, sql, page_size)
  } else {
    sb_send_extended_query(client, sql, params, page_size)
  }
  result <- new.env(parent = emptyenv())
  result$client <- client
  result$columns <- list()
  result$rowcount <- -1
  result$command_tag <- ""
  result$done <- FALSE
  result$page_size <- page_size
  result$pending_rows <- list()
  result$response_started <- FALSE
  result$ignored_stray_ready <- FALSE
  result
}

sb_ensure_implicit_transaction <- function(client) {
  client$txn_active <- TRUE
  client$explicit_txn <- FALSE
  invisible(NULL)
}

sb_allow_portal_resume <- function(client) {
  client$portal_resume_pending <- TRUE
  invisible(NULL)
}

sb_resume_suspended_portal <- function(client, page_size) {
  if (!isTRUE(client$portal_resume_pending)) {
    sb_stop_sqlstate("55000", "portal resume requires explicit suspended state")
  }
  client$portal_resume_pending <- FALSE
  rows_to_fetch <- suppressWarnings(as.integer(page_size))
  if (is.na(rows_to_fetch) || rows_to_fetch < 1L) rows_to_fetch <- 1L
  exec_payload <- build_execute_payload("", rows_to_fetch)
  sb_send_message(client, SB_MSG_EXECUTE, exec_payload, 0L, FALSE)
}

sb_prime_result_metadata <- function(result) {
  if (isTRUE(result$done) || length(result$columns) > 0) return(invisible(result))
  if (is.null(result$pending_rows)) result$pending_rows <- list()
  client <- result$client
  repeat {
    response <- sb_recv_message(client)
    type <- response$type
    payload <- response$payload
    if (sb_handle_async(client, type, payload)) next
    if (type == SB_MSG_ERROR) {
      sb_drain_ready_after_error(client)
      sb_raise_query_error(payload)
    } else if (type == SB_MSG_ROW_DESCRIPTION) {
      result$response_started <- TRUE
      result$columns <- parse_row_description(payload)
      return(invisible(result))
    } else if (type == SB_MSG_DATA_ROW) {
      result$response_started <- TRUE
      values <- parse_data_row(payload)
      result$pending_rows[[length(result$pending_rows) + 1L]] <- sb_decode_row(result$columns, values)
      if (length(result$columns) > 0) {
        return(invisible(result))
      }
    } else if (type == SB_MSG_COMMAND_COMPLETE) {
      result$response_started <- TRUE
      parsed <- parse_command_complete(payload)
      result$command_tag <- parsed$tag
      result$rowcount <- parsed$rows
    } else if (type == SB_MSG_PARAMETER_STATUS) {
      parsed <- parse_parameter_status(payload)
      for (entry in parsed$entries) sb_handle_parameter_status(client, entry$name, entry$value)
    } else if (type == SB_MSG_PORTAL_SUSPENDED) {
      sb_allow_portal_resume(client)
      client$last_query_sequence <- sb_resume_suspended_portal(client, result$page_size)
    } else if (type == SB_MSG_READY) {
      parsed <- parse_ready(payload)
      sb_apply_runtime_ready_state(client, parsed$status, parsed$txn_id)
      client$portal_resume_pending <- FALSE
      if (!isTRUE(result$response_started) && !isTRUE(result$ignored_stray_ready)) {
        # Native rollback/commit can publish a fresh-session reopen boundary
        # before the next statement response begins. Ignore one READY frame
        # that arrives before any result material so the actual query is not
        # misclassified as empty.
        result$ignored_stray_ready <- TRUE
        next
      }
      result$done <- TRUE
      return(invisible(result))
    }
  }
}

sb_result_next_row <- function(result) {
  if (isTRUE(result$client$cancel_requested)) {
    result$client$cancel_requested <- FALSE
    result$done <- TRUE
    stop(structure(
      list(
        message = "[57014] query canceled",
        call = NULL,
        sqlstate = "57014",
        detail = "",
        hint = "",
        severity = "ERROR"
      ),
      class = sb_error_condition_classes("57014")
    ))
  }
  if (isTRUE(result$done)) return(NULL)
  if (is.null(result$pending_rows)) result$pending_rows <- list()
  if (length(result$pending_rows) > 0) {
    row <- result$pending_rows[[1]]
    if (length(result$pending_rows) == 1) {
      result$pending_rows <- list()
    } else {
      result$pending_rows <- result$pending_rows[-1]
    }
    return(row)
  }
  client <- result$client
  repeat {
    response <- sb_recv_message(client)
    type <- response$type
    payload <- response$payload
    if (sb_handle_async(client, type, payload)) next
    if (type == SB_MSG_ERROR) {
      sb_drain_ready_after_error(client)
      sb_raise_query_error(payload)
    } else if (type == SB_MSG_ROW_DESCRIPTION) {
      result$response_started <- TRUE
      result$columns <- parse_row_description(payload)
    } else if (type == SB_MSG_DATA_ROW) {
      result$response_started <- TRUE
      values <- parse_data_row(payload)
      return(sb_decode_row(result$columns, values))
    } else if (type == SB_MSG_COMMAND_COMPLETE) {
      result$response_started <- TRUE
      parsed <- parse_command_complete(payload)
      result$command_tag <- parsed$tag
      result$rowcount <- parsed$rows
    } else if (type == SB_MSG_PARAMETER_STATUS) {
      parsed <- parse_parameter_status(payload)
      for (entry in parsed$entries) sb_handle_parameter_status(client, entry$name, entry$value)
    } else if (type == SB_MSG_PORTAL_SUSPENDED) {
      sb_allow_portal_resume(client)
      client$last_query_sequence <- sb_resume_suspended_portal(client, result$page_size)
    } else if (type == SB_MSG_READY) {
      parsed <- parse_ready(payload)
      sb_apply_runtime_ready_state(client, parsed$status, parsed$txn_id)
      client$portal_resume_pending <- FALSE
      if (!isTRUE(result$response_started) && !isTRUE(result$ignored_stray_ready)) {
        result$ignored_stray_ready <- TRUE
        next
      }
      result$done <- TRUE
      return(NULL)
    }
  }
}

sb_fetch_rows <- function(result, n = -1) {
  rows <- list()
  if (n == 0) return(rows)
  repeat {
    if (n > 0 && length(rows) >= n) break
    row <- sb_result_next_row(result)
    if (is.null(row)) break
    rows[[length(rows) + 1]] <- row
  }
  rows
}

sb_decode_row <- function(columns, values) {
  row <- vector("list", length(values))
  for (idx in seq_along(values)) {
    col <- if (length(columns) >= idx) columns[[idx]] else list(type_oid = 0L, format = SB_FORMAT_BINARY)
    row[[idx]] <- decode_value(col$type_oid, values[[idx]]$data, col$format)
  }
  row
}

sb_sqlstate_error_class <- function(sqlstate) {
  if (!is.character(sqlstate) || length(sqlstate) != 1L || nchar(sqlstate) != 5L) {
    return(NULL)
  }

  if (sqlstate == "01000") return("scratchbird_warning")
  if (sqlstate == "02000") return("scratchbird_no_data")
  if (sqlstate %in% c("08001", "08003", "08004", "08006", "08P01")) return("scratchbird_connection_error")
  if (sqlstate == "0A000") return("scratchbird_not_supported")
  if (sqlstate %in% c("22001", "22003", "22007", "22012", "22023", "22P02", "22P03")) return("scratchbird_data_error")
  if (sqlstate %in% c("23000", "23502", "23503", "23505", "23514")) return("scratchbird_integrity_error")
  if (sqlstate %in% c("28000", "28P01")) return("scratchbird_auth_error")
  if (sqlstate %in% c("40001", "40P01")) return("scratchbird_transaction_error")
  if (sqlstate %in% c("42501", "42601", "42703", "42704", "42710", "42883", "42P01", "42P07")) return("scratchbird_syntax_error")
  if (sqlstate %in% c("53P00", "53100", "53200", "53300")) return("scratchbird_resource_error")
  if (sqlstate == "54000") return("scratchbird_limit_error")
  if (sqlstate %in% c("57014", "57P01", "57P03")) return("scratchbird_operator_intervention_error")
  if (sqlstate == "58000") return("scratchbird_system_error")
  if (sqlstate == "XX000") return("scratchbird_internal_error")

  prefix <- substr(sqlstate, 1, 2)
  if (prefix == "01") return("scratchbird_warning")
  if (prefix == "02") return("scratchbird_no_data")
  if (prefix == "08") return("scratchbird_connection_error")
  if (prefix == "0A") return("scratchbird_not_supported")
  if (prefix == "22") return("scratchbird_data_error")
  if (prefix == "23") return("scratchbird_integrity_error")
  if (prefix == "28") return("scratchbird_auth_error")
  if (prefix == "40") return("scratchbird_transaction_error")
  if (prefix == "42") return("scratchbird_syntax_error")
  if (prefix == "53") return("scratchbird_resource_error")
  if (prefix == "54") return("scratchbird_limit_error")
  if (prefix == "57") return("scratchbird_operator_intervention_error")
  if (prefix == "58") return("scratchbird_system_error")
  if (prefix == "XX") return("scratchbird_internal_error")
  NULL
}

sb_retry_scope_for_sqlstate <- function(sqlstate) {
  if (is.null(sqlstate) || !is.character(sqlstate) || length(sqlstate) != 1L) {
    return("none")
  }
  if (!nzchar(sqlstate) || nchar(sqlstate) != 5L) {
    return("none")
  }
  if (sqlstate %in% c("40001", "40P01")) {
    return("statement")
  }
  if (substr(sqlstate, 1, 2) == "08") {
    return("reconnect")
  }
  "none"
}

sb_is_retryable_sqlstate <- function(sqlstate) {
  sb_retry_scope_for_sqlstate(sqlstate) != "none"
}

sb_error_condition_classes <- function(sqlstate) {
  classes <- c()
  mapped <- sb_sqlstate_error_class(sqlstate)
  if (!is.null(mapped)) classes <- c(classes, mapped)
  if (nzchar(sqlstate)) classes <- c(classes, "scratchbird_sqlstate_error")
  unique(c(classes, "scratchbird_error", "error", "condition"))
}

sb_stop_sqlstate <- function(sqlstate, message, detail = "", hint = "", severity = "ERROR") {
  formatted <- if (nzchar(sqlstate)) paste0("[", sqlstate, "] ", message) else message
  stop(structure(
    list(
      message = formatted,
      call = NULL,
      sqlstate = sqlstate,
      detail = detail,
      hint = hint,
      severity = severity
    ),
    class = sb_error_condition_classes(sqlstate)
  ))
}

sb_raise_query_error <- function(payload) {
  parsed <- parse_error_message(payload)
  parts <- c()
  if (parsed$message != "") parts <- c(parts, parsed$message)
  if (parsed$detail != "") parts <- c(parts, paste0("DETAIL: ", parsed$detail))
  if (parsed$hint != "") parts <- c(parts, paste0("HINT: ", parsed$hint))
  message <- if (length(parts) == 0) "query failed" else paste(parts, collapse = "\n")
  if (parsed$sqlstate != "") message <- paste0("[", parsed$sqlstate, "] ", message)
  stop(structure(
    list(
      message = message,
      call = NULL,
      sqlstate = parsed$sqlstate,
      detail = parsed$detail,
      hint = parsed$hint,
      severity = parsed$severity
    ),
    class = sb_error_condition_classes(parsed$sqlstate)
  ))
}

parse_uuid_bytes <- function(value) {
  hex <- gsub("-", "", trimws(value), fixed = TRUE)
  if (nchar(hex) != 32 || !grepl("^[0-9A-Fa-f]+$", hex)) return(NULL)
  bytes <- raw(16)
  for (i in 0:15) {
    part <- substr(hex, i * 2 + 1, i * 2 + 2)
    bytes[i + 1] <- as.raw(strtoi(part, 16L))
  }
  bytes
}

sb_handle_parameter_status <- function(client, name, value) {
  if (name == "attachment_id") {
    parsed <- parse_uuid_bytes(value)
    if (!is.null(parsed)) client$attachment_id <- parsed
  }
  if (name == "current_txn_id") {
    parsed <- suppressWarnings(as.numeric(value))
    if (!is.na(parsed)) sb_apply_runtime_txn_id(client, parsed)
  }
  client$parameters[[name]] <- value
}

sb_handle_async <- function(client, type, payload) {
  if (type == SB_MSG_PARAMETER_STATUS) {
    parsed <- parse_parameter_status(payload)
    for (entry in parsed$entries) sb_handle_parameter_status(client, entry$name, entry$value)
    return(TRUE)
  }
  if (type == SB_MSG_NOTIFICATION) {
    notice <- parse_notification(payload)
    for (handler in client$notification_handlers) handler(notice)
    return(TRUE)
  }
  if (type == SB_MSG_QUERY_PLAN) {
    client$last_plan <- parse_query_plan(payload)
    return(TRUE)
  }
  if (type == SB_MSG_SBLR_COMPILED) {
    client$last_sblr <- parse_sblr_compiled(payload)
    return(TRUE)
  }
  if (type == SB_MSG_TXN_STATUS) {
    parsed <- parse_txn_status(payload)
    if (identical(parsed$status, "T")) {
      sb_apply_runtime_txn_id(client, parsed$txn_id)
    } else {
      sb_clear_transaction_state(client)
    }
    return(TRUE)
  }
  FALSE
}

sb_drain_until_ready <- function(client) {
  repeat {
    response <- sb_recv_message(client)
    if (sb_handle_async(client, response$type, response$payload)) next
    if (response$type == SB_MSG_READY) {
      parsed <- parse_ready(response$payload)
      sb_apply_runtime_ready_state(client, parsed$status, parsed$txn_id)
      client$portal_resume_pending <- FALSE
      break
    }
    if (response$type == SB_MSG_ERROR) sb_raise_query_error(response$payload)
  }
}

sb_drain_ready_after_error <- function(client) {
  max_followups <- 32L
  for (idx in seq_len(max_followups)) {
    response <- tryCatch(
      sb_recv_message(client),
      error = function(err) {
        client$portal_resume_pending <- FALSE
        client$needs_reconnect <- TRUE
        NULL
      }
    )
    if (is.null(response)) break
    if (sb_handle_async(client, response$type, response$payload)) next
    if (response$type == SB_MSG_READY) {
      parsed <- parse_ready(response$payload)
      sb_apply_runtime_ready_state(client, parsed$status, parsed$txn_id)
      client$portal_resume_pending <- FALSE
      return(invisible(NULL))
    }
    if (response$type == SB_MSG_ERROR) {
      client$portal_resume_pending <- FALSE
      client$needs_reconnect <- TRUE
      break
    }
  }
  invisible(NULL)
}

sb_send_simple_query <- function(client, sql, max_rows = 0L) {
  flags <- if (isTRUE(client$cfg$binary_transfer)) 0x04L else 0L
  payload <- build_query_payload(sql, flags, max_rows, 0L)
  client$last_plan <- NULL
  client$last_sblr <- NULL
  client$portal_resume_pending <- FALSE
  client$last_query_sequence <- sb_send_message(client, SB_MSG_QUERY, payload, 0L, FALSE)
}

sb_send_extended_query <- function(client, sql, params, max_rows = 0L) {
  param_values <- list()
  param_types <- c()
  for (param in params) {
    encoded <- encode_param(param)
    param_values[[length(param_values) + 1]] <- encoded$param
    param_types <- c(param_types, encoded$oid)
  }
  parse_payload <- build_parse_payload("", sql, param_types)
  sb_send_message(client, SB_MSG_PARSE, parse_payload, 0L, FALSE)
  described <- sb_describe_statement(client, "")
  if (described >= 0 && described != length(param_types)) {
    stop("parameter count mismatch (07001)")
  }

  result_formats <- if (isTRUE(client$cfg$binary_transfer)) c(SB_FORMAT_BINARY) else c()
  bind_payload <- build_bind_payload("", "", param_values, result_formats)
  sb_send_message(client, SB_MSG_BIND, bind_payload, 0L, FALSE)

  exec_payload <- build_execute_payload("", max_rows)
  client$last_plan <- NULL
  client$last_sblr <- NULL
  client$portal_resume_pending <- FALSE
  client$last_query_sequence <- sb_send_message(client, SB_MSG_EXECUTE, exec_payload, 0L, FALSE)
  if (max_rows == 0L) {
    sb_send_message(client, SB_MSG_SYNC, raw(), 0L, FALSE)
  }
}

sb_describe_statement <- function(client, statement_name) {
  payload <- build_describe_payload(as.integer(charToRaw("S")), statement_name)
  sb_send_message(client, SB_MSG_DESCRIBE, payload, 0L, FALSE)
  sb_send_message(client, SB_MSG_SYNC, raw(), 0L, FALSE)
  param_count <- -1L
  repeat {
    response <- sb_recv_message(client)
    if (sb_handle_async(client, response$type, response$payload)) next
    type <- response$type
    payload <- response$payload
    if (type == SB_MSG_ERROR) {
      sb_raise_query_error(payload)
    } else if (type == SB_MSG_PARAMETER_DESCRIPTION) {
      param_count <- length(parse_parameter_description(payload))
    } else if (type == SB_MSG_READY) {
      parsed <- parse_ready(payload)
      sb_apply_runtime_ready_state(client, parsed$status, parsed$txn_id)
      client$portal_resume_pending <- FALSE
      break
    }
  }
  param_count
}

sb_send_message <- function(client, type, payload, flags = 0L, force_zero = FALSE) {
  sequence <- client$sequence
  client$sequence <- client$sequence + 1
  attachment <- if (force_zero) raw(16) else client$attachment_id
  txn_id <- if (force_zero) 0 else client$txn_id
  data <- encode_message(type, payload, flags, sequence, attachment, txn_id)
  sb_socket_write(client$con, data)
  sequence
}

sb_recv_message <- function(client) {
  header <- sb_socket_read_exact(client$con, SB_HEADER_SIZE)
  if (length(header) != SB_HEADER_SIZE) stop("connection closed")
  parsed <- decode_header(header)
  payload <- if (parsed$length > 0) sb_socket_read_exact(client$con, parsed$length) else raw()
  list(
    type = parsed$type,
    flags = parsed$flags,
    payload = payload,
    sequence = parsed$sequence,
    attachment_id = parsed$attachment_id,
    txn_id = parsed$txn_id
  )
}

sb_rows_to_column <- function(rows, index) {
  values <- lapply(rows, function(row) row[[index]])
  non_null <- values[!vapply(values, is.null, logical(1))]
  if (length(non_null) == 0) {
    return(rep(NA, length(values)))
  }

  can_simplify <- all(vapply(
    non_null,
    function(value) is.atomic(value) && !is.list(value) && !is.raw(value) && length(value) == 1,
    logical(1)
  ))
  if (!can_simplify) {
    return(I(values))
  }

  simplified <- lapply(values, function(value) if (is.null(value)) NA else value)
  do.call(c, simplified)
}

sb_rows_to_df <- function(rows, columns) {
  names <- vapply(columns, function(col) col$name, character(1))
  if (length(rows) == 0) {
    empty_cols <- lapply(columns, function(...) logical(0))
    names(empty_cols) <- names
    return(as.data.frame(empty_cols, stringsAsFactors = FALSE))
  }
  cols <- vector("list", length(columns))
  for (i in seq_along(columns)) {
    cols[[i]] <- sb_rows_to_column(rows, i)
  }
  names(cols) <- names
  as.data.frame(cols, stringsAsFactors = FALSE)
}

sb_result_to_df <- function(result) {
  rows <- result$rows
  if (is.null(rows)) {
    rows <- sb_fetch_rows(result, -1)
  }
  sb_rows_to_df(rows, result$columns)
}
