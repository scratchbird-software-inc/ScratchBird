# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

SB_MAGIC_BYTES <- charToRaw("SBWP")
SB_VERSION_MAJOR <- 1L
SB_VERSION_MINOR <- 1L
SB_HEADER_SIZE <- 40L
SB_MAX_MESSAGE_SIZE <- 1024L * 1024L * 1024L

SB_MSG_STARTUP <- 0x01
SB_MSG_AUTH_RESPONSE <- 0x02
SB_MSG_QUERY <- 0x03
SB_MSG_PARSE <- 0x04
SB_MSG_BIND <- 0x05
SB_MSG_DESCRIBE <- 0x06
SB_MSG_EXECUTE <- 0x07
SB_MSG_CLOSE <- 0x08
SB_MSG_SYNC <- 0x09
SB_MSG_FLUSH <- 0x0A
SB_MSG_CANCEL <- 0x0B
SB_MSG_TERMINATE <- 0x0C
SB_MSG_COPY_DATA <- 0x0D
SB_MSG_COPY_DONE <- 0x0E
SB_MSG_COPY_FAIL <- 0x0F
SB_MSG_SBLR_EXECUTE <- 0x10
SB_MSG_SUBSCRIBE <- 0x11
SB_MSG_UNSUBSCRIBE <- 0x12
SB_MSG_FEDERATED_QUERY <- 0x13
SB_MSG_STREAM_CONTROL <- 0x14
SB_MSG_TXN_BEGIN <- 0x15
SB_MSG_TXN_COMMIT <- 0x16
SB_MSG_TXN_ROLLBACK <- 0x17
SB_MSG_TXN_SAVEPOINT <- 0x18
SB_MSG_TXN_RELEASE <- 0x19
SB_MSG_TXN_ROLLBACK_TO <- 0x1A
SB_MSG_PING <- 0x1B
SB_MSG_SET_OPTION <- 0x1C
SB_MSG_CLUSTER_AUTH <- 0x1D
SB_MSG_ATTACH_CREATE <- 0x1E
SB_MSG_ATTACH_DETACH <- 0x1F
SB_MSG_ATTACH_LIST <- 0x20

SB_MSG_AUTH_REQUEST <- 0x40
SB_MSG_AUTH_OK <- 0x41
SB_MSG_AUTH_CONTINUE <- 0x42
SB_MSG_READY <- 0x43
SB_MSG_ROW_DESCRIPTION <- 0x44
SB_MSG_DATA_ROW <- 0x45
SB_MSG_COMMAND_COMPLETE <- 0x46
SB_MSG_EMPTY_QUERY <- 0x47
SB_MSG_ERROR <- 0x48
SB_MSG_NOTICE <- 0x49
SB_MSG_PARSE_COMPLETE <- 0x4A
SB_MSG_BIND_COMPLETE <- 0x4B
SB_MSG_CLOSE_COMPLETE <- 0x4C
SB_MSG_PORTAL_SUSPENDED <- 0x4D
SB_MSG_NO_DATA <- 0x4E
SB_MSG_PARAMETER_STATUS <- 0x4F
SB_MSG_PARAMETER_DESCRIPTION <- 0x50
SB_MSG_COPY_IN_RESPONSE <- 0x51
SB_MSG_COPY_OUT_RESPONSE <- 0x52
SB_MSG_COPY_BOTH_RESPONSE <- 0x53
SB_MSG_NOTIFICATION <- 0x54
SB_MSG_FUNCTION_RESULT <- 0x55
SB_MSG_NEGOTIATE_VERSION <- 0x56
SB_MSG_SBLR_COMPILED <- 0x57
SB_MSG_QUERY_PLAN <- 0x58
SB_MSG_STREAM_READY <- 0x59
SB_MSG_STREAM_DATA <- 0x5A
SB_MSG_STREAM_END <- 0x5B
SB_MSG_TXN_STATUS <- 0x5C
SB_MSG_PONG <- 0x5D
SB_MSG_CLUSTER_AUTH_OK <- 0x5E
SB_MSG_FEDERATED_RESULT <- 0x5F
SB_MSG_HEARTBEAT <- 0x80
SB_MSG_EXTENSION <- 0x81

SB_AUTH_OK <- 0L
SB_AUTH_PASSWORD <- 1L
SB_AUTH_MD5 <- 2L
SB_AUTH_SCRAM_SHA256 <- 3L
SB_AUTH_SCRAM_SHA512 <- 4L
SB_AUTH_TOKEN <- 5L
SB_AUTH_PEER <- 6L
SB_AUTH_REATTACH <- 7L

SB_MSG_FLAG_COMPRESSED <- 0x01
SB_MSG_FLAG_CONTINUED <- 0x02
SB_MSG_FLAG_FINAL <- 0x04
SB_MSG_FLAG_URGENT <- 0x08
SB_MSG_FLAG_ENCRYPTED <- 0x10
SB_MSG_FLAG_CHECKSUM <- 0x20

SB_FEATURE_COMPRESSION <- 1L
SB_FEATURE_STREAMING <- 2L
SB_FEATURE_SBLR <- 4L
SB_FEATURE_FEDERATION <- 8L
SB_FEATURE_NOTIFICATIONS <- 16L
SB_FEATURE_QUERY_PLAN <- 32L
SB_FEATURE_BATCH <- 64L
SB_FEATURE_PIPELINE <- 128L
SB_FEATURE_BINARY_COPY <- 256L
SB_FEATURE_SAVEPOINTS <- 512L
SB_FEATURE_2PC <- 1024L
SB_FEATURE_CHECKSUMS <- 2048L

SB_QUERY_FLAG_DESCRIBE_ONLY <- 0x01
SB_QUERY_FLAG_NO_PORTAL <- 0x02
SB_QUERY_FLAG_BINARY_RESULT <- 0x04
SB_QUERY_FLAG_INCLUDE_PLAN <- 0x08
SB_QUERY_FLAG_RETURN_SBLR <- 0x10
SB_QUERY_FLAG_NO_CACHE <- 0x20

SB_ISOLATION_READ_UNCOMMITTED <- 0L
SB_ISOLATION_READ_COMMITTED <- 1L
SB_ISOLATION_REPEATABLE_READ <- 2L
SB_ISOLATION_SERIALIZABLE <- 3L

SB_READ_COMMITTED_MODE_DEFAULT <- 0L
SB_READ_COMMITTED_MODE_READ_CONSISTENCY <- 1L
SB_READ_COMMITTED_MODE_RECORD_VERSION <- 2L
SB_READ_COMMITTED_MODE_NO_RECORD_VERSION <- 3L

SB_TXN_FLAG_HAS_ISOLATION <- 0x0001
SB_TXN_FLAG_HAS_ACCESS <- 0x0002
SB_TXN_FLAG_HAS_DEFERRABLE <- 0x0004
SB_TXN_FLAG_HAS_WAIT <- 0x0008
SB_TXN_FLAG_HAS_TIMEOUT <- 0x0010
SB_TXN_FLAG_HAS_AUTOCOMMIT <- 0x0020
SB_TXN_FLAG_HAS_READ_COMMITTED_MODE <- 0x0100

SB_STREAM_START <- 0L
SB_STREAM_PAUSE <- 1L
SB_STREAM_RESUME <- 2L
SB_STREAM_CANCEL <- 3L
SB_STREAM_ACK <- 4L

SB_SUB_TYPE_CHANNEL <- 0L
SB_SUB_TYPE_TABLE <- 1L
SB_SUB_TYPE_QUERY <- 2L
SB_SUB_TYPE_EVENT <- 3L

SB_P1_ROW_DESCRIPTION_HEADER_BYTES <- 72L
SB_P1_CANONICAL_TYPE_REF_BYTES <- 144L

pack_u64 <- function(x) {
  value <- as.numeric(x)
  if (!is.finite(value) || value < 0) stop("u64 value must be a finite non-negative number")
  low <- value %% 4294967296
  high <- floor(value / 4294967296)
  c(pack_u32(low), pack_u32(high))
}

pack_i64 <- function(x) {
  value <- as.numeric(x)
  if (!is.finite(value)) stop("i64 value must be finite")
  if (value < 0) {
    value <- value + 18446744073709551616
  }
  pack_u64(value)
}
pack_u32 <- function(x) writeBin(as.integer(x), raw(), size = 4, endian = "little")
pack_u16 <- function(x) writeBin(as.integer(x), raw(), size = 2, endian = "little")
pack_u8 <- function(x) writeBin(as.integer(x), raw(), size = 1, endian = "little")
pack_i32 <- function(x) writeBin(as.integer(x), raw(), size = 4, endian = "little")

read_u64 <- function(data, offset) {
  low <- read_u32(data, offset)
  high <- read_u32(data, offset + 4)
  low + high * 4294967296
}

read_u32 <- function(data, offset) {
  value <- as.numeric(readBin(data[offset:(offset + 3)], integer(), size = 4, endian = "little", signed = TRUE))
  if (value < 0) value <- value + 4294967296
  value
}

read_u16 <- function(data, offset) {
  as.integer(readBin(data[offset:(offset + 1)], integer(), size = 2, endian = "little", signed = FALSE))
}

read_u8 <- function(data, offset) {
  as.integer(readBin(data[offset], integer(), size = 1, signed = FALSE))
}

protocol_hex_encode_raw <- function(data) {
  paste0("0x", paste(sprintf("%02x", as.integer(data)), collapse = ""))
}

protocol_raw_to_text <- function(data) {
  if (length(data) == 0) return("")
  if (any(as.integer(data) == 0L)) return(protocol_hex_encode_raw(data))
  rawToChar(data)
}

read_i32 <- function(data, offset) {
  as.integer(readBin(data[offset:(offset + 3)], integer(), size = 4, endian = "little", signed = TRUE))
}

encode_message <- function(type, payload, flags = 0L, sequence = 0L, attachment_id = raw(16), txn_id = 0) {
  payload <- if (is.null(payload)) raw() else payload
  length <- length(payload)
  header <- c(
    SB_MAGIC_BYTES,
    pack_u8(SB_VERSION_MAJOR),
    pack_u8(SB_VERSION_MINOR),
    pack_u8(type),
    pack_u8(flags),
    pack_u32(length),
    pack_u32(sequence)
  )
  attachment <- attachment_id
  if (length(attachment) < 16) attachment <- c(attachment, raw(16 - length(attachment)))
  if (length(attachment) > 16) attachment <- attachment[1:16]
  c(header, attachment, pack_u64(txn_id), payload)
}

decode_header <- function(data) {
  if (length(data) != SB_HEADER_SIZE) stop("Invalid header length")
  if (!identical(as.raw(data[1:4]), SB_MAGIC_BYTES)) stop("Invalid protocol magic")
  major <- read_u8(data, 5)
  minor <- read_u8(data, 6)
  if (major != SB_VERSION_MAJOR || minor != SB_VERSION_MINOR) stop("Unsupported protocol version")
  msg_type <- read_u8(data, 7)
  flags <- read_u8(data, 8)
  length <- read_u32(data, 9)
  if (length > SB_MAX_MESSAGE_SIZE) stop("Payload too large")
  sequence <- read_u32(data, 13)
  attachment_id <- data[17:32]
  txn_id <- read_u64(data, 33)
  list(type = msg_type, flags = flags, length = length, sequence = sequence, attachment_id = attachment_id, txn_id = txn_id)
}

build_startup_payload <- function(features, params) {
  protocol_version <- bitwShiftL(SB_VERSION_MAJOR, 8L) + SB_VERSION_MINOR
  buf <- c(pack_u16(protocol_version), pack_u16(protocol_version), pack_u32(0))
  buf <- c(buf, pack_u64(features), pack_u64(0), pack_u64(0))
  buf <- c(buf, rep(as.raw(0x11), 16), raw(32))
  entries <- sort(names(params))
  buf <- c(buf, pack_u32(length(entries)))
  for (key in entries) {
    key_bytes <- charToRaw(enc2utf8(key))
    value_bytes <- charToRaw(enc2utf8(as.character(params[[key]])))
    buf <- c(
      buf,
      pack_u32(length(key_bytes)),
      key_bytes,
      pack_u16(1),
      pack_u32(length(value_bytes)),
      value_bytes
    )
  }
  c(buf, pack_u32(0))
}

build_param_list <- function(params) {
  buf <- raw()
  for (name in names(params)) {
    buf <- c(buf, charToRaw(name), as.raw(0x00), charToRaw(as.character(params[[name]])), as.raw(0x00))
  }
  c(buf, as.raw(0x00))
}

parse_auth_request <- function(payload) {
  if (length(payload) < 4) stop("Auth request truncated")
  method <- payload[1]
  data <- if (length(payload) > 4) payload[5:length(payload)] else raw()
  list(method = as.integer(method), data = data)
}

parse_auth_continue <- function(payload) {
  if (length(payload) < 8) stop("Auth continue truncated")
  method <- payload[1]
  stage <- payload[2]
  data_len <- read_u32(payload, 5)
  if (8 + data_len > length(payload)) stop("Auth continue truncated")
  data <- if (data_len > 0) payload[9:(8 + data_len)] else raw()
  list(method = as.integer(method), stage = as.integer(stage), data = data)
}

parse_auth_ok <- function(payload) {
  if (length(payload) < 20) stop("Auth ok truncated")
  session_id <- payload[1:16]
  info_len <- read_u32(payload, 17)
  if (20 + info_len > length(payload)) stop("Auth ok truncated")
  info <- if (info_len > 0) payload[21:(20 + info_len)] else raw()
  list(session_id = session_id, server_info = info)
}

build_query_payload <- function(sql, flags = 0L, max_rows = 0L, timeout_ms = 0L) {
  c(pack_u32(flags), pack_u32(max_rows), pack_u32(timeout_ms), charToRaw(sql), as.raw(0x00))
}

build_parse_payload <- function(statement_name, sql, param_types) {
  name_bytes <- charToRaw(statement_name)
  sql_bytes <- charToRaw(sql)
  payload <- c(pack_u32(length(name_bytes)), name_bytes, pack_u32(length(sql_bytes)), sql_bytes)
  payload <- c(payload, pack_u16(length(param_types)), pack_u16(0))
  if (length(param_types) > 0) {
    for (oid in param_types) {
      payload <- c(payload, pack_u32(oid))
    }
  }
  payload
}

build_bind_payload <- function(portal_name, statement_name, params, result_formats) {
  portal_bytes <- charToRaw(portal_name)
  stmt_bytes <- charToRaw(statement_name)
  payload <- c(pack_u32(length(portal_bytes)), portal_bytes, pack_u32(length(stmt_bytes)), stmt_bytes)
  payload <- c(payload, pack_u16(length(params)))
  if (length(params) > 0) {
    for (param in params) {
      payload <- c(payload, pack_u16(param$format))
    }
  }
  payload <- c(payload, pack_u16(length(params)), pack_u16(0))
  if (length(params) > 0) {
    for (param in params) {
      if (isTRUE(param$is_null)) {
        payload <- c(payload, pack_i32(-1L))
      } else {
        data <- param$data
        payload <- c(payload, pack_i32(length(data)), data)
      }
    }
  }
  payload <- c(payload, pack_u16(length(result_formats)))
  if (length(result_formats) > 0) {
    for (fmt in result_formats) {
      payload <- c(payload, pack_u16(fmt))
    }
  }
  payload
}

build_execute_payload <- function(portal_name, max_rows = 0L) {
  portal_bytes <- charToRaw(portal_name)
  c(pack_u32(length(portal_bytes)), portal_bytes, pack_u32(max_rows))
}

build_describe_payload <- function(describe_type, name) {
  name_bytes <- charToRaw(name)
  c(pack_u8(describe_type), as.raw(c(0x00, 0x00, 0x00)), pack_u32(length(name_bytes)), name_bytes)
}

build_cancel_payload <- function(cancel_type, target_sequence) {
  c(pack_u32(cancel_type), pack_u32(target_sequence))
}

build_sblr_execute_payload <- function(sblr_hash, sblr_bytecode, params) {
  bytecode <- if (is.null(sblr_bytecode)) raw() else sblr_bytecode
  payload <- c(
    pack_u64(sblr_hash),
    pack_u32(length(bytecode)),
    pack_u16(length(params)),
    pack_u16(0)
  )
  if (length(bytecode) > 0) payload <- c(payload, bytecode)
  for (param in params) {
    if (is.null(param$data)) {
      payload <- c(payload, pack_i32(-1))
    } else {
      payload <- c(payload, pack_i32(length(param$data)), param$data)
    }
  }
  payload
}

build_subscribe_payload <- function(subscribe_type, channel, filter_expr = "") {
  channel_bytes <- charToRaw(channel)
  filter_bytes <- charToRaw(filter_expr)
  c(
    pack_u8(subscribe_type),
    raw(3),
    pack_u32(length(channel_bytes)),
    channel_bytes,
    pack_u32(length(filter_bytes)),
    filter_bytes
  )
}

build_unsubscribe_payload <- function(channel) {
  channel_bytes <- charToRaw(channel)
  c(pack_u32(length(channel_bytes)), channel_bytes)
}

build_txn_begin_payload <- function(flags, conflict_action, autocommit_mode, isolation_level, access_mode, deferrable, wait_mode, timeout_ms, read_committed_mode = SB_READ_COMMITTED_MODE_DEFAULT) {
  payload <- c(
    pack_u16(flags),
    pack_u8(conflict_action),
    pack_u8(autocommit_mode),
    pack_u8(isolation_level),
    pack_u8(access_mode),
    pack_u8(deferrable),
    pack_u8(wait_mode),
    pack_u32(timeout_ms)
  )
  if (bitwAnd(flags, SB_TXN_FLAG_HAS_READ_COMMITTED_MODE) != 0L) {
    payload <- c(payload, pack_u8(read_committed_mode), raw(3))
  }
  payload
}

build_txn_commit_payload <- function(flags) {
  c(pack_u8(flags), raw(3))
}

build_txn_rollback_payload <- function(flags) {
  c(pack_u8(flags), raw(3))
}

build_txn_savepoint_payload <- function(name) {
  name_bytes <- charToRaw(name)
  c(pack_u32(length(name_bytes)), name_bytes)
}

build_txn_release_payload <- function(name) {
  build_txn_savepoint_payload(name)
}

build_txn_rollback_to_payload <- function(name) {
  build_txn_savepoint_payload(name)
}

build_set_option_payload <- function(name, value) {
  name_bytes <- charToRaw(name)
  value_bytes <- charToRaw(value)
  c(
    pack_u32(length(name_bytes)),
    name_bytes,
    pack_u32(length(value_bytes)),
    value_bytes
  )
}

build_stream_control_payload <- function(control_type, window_size, timeout_ms) {
  c(pack_u8(control_type), raw(3), pack_u32(window_size), pack_u32(timeout_ms))
}

build_attach_create_payload <- function(emulation_mode, db_name) {
  mode_bytes <- charToRaw(emulation_mode)
  db_bytes <- charToRaw(db_name)
  c(
    pack_u32(length(mode_bytes)),
    mode_bytes,
    pack_u32(length(db_bytes)),
    db_bytes
  )
}

parse_ready <- function(payload) {
  if (length(payload) >= 76) {
    status_byte <- as.integer(payload[57])
    if (status_byte %in% c(0x49, 0x54, 0x45, 0x52, 0x41)) {
      txn_id <- read_u64(payload, 49)
      status <- if (status_byte %in% c(0x54, 0x45)) 1L else 0L
      return(list(status = status, txn_id = txn_id, visibility = txn_id))
    }
  }
  if (length(payload) < 20) stop("Ready truncated")
  status <- payload[1]
  txn_id <- read_u64(payload, 5)
  visibility <- read_u64(payload, 13)
  list(status = as.integer(status), txn_id = txn_id, visibility = visibility)
}

parse_txn_status <- function(payload) {
  if (length(payload) < 12) stop("Txn status truncated")
  status <- protocol_raw_to_text(payload[1])
  txn_id <- read_u64(payload, 5)
  list(status = status, txn_id = txn_id)
}

parse_parameter_status <- function(payload) {
  if (length(payload) < 4) stop("Parameter status truncated")
  offset <- 1
  count <- read_u32(payload, offset)
  offset <- offset + 4
  entries <- list()
  if (count > 0 && count <= 256) {
    for (idx in seq_len(count)) {
      if (offset + 3 > length(payload)) stop("Parameter status truncated")
      name_len <- read_u32(payload, offset)
      offset <- offset + 4
      if (offset + name_len - 1 > length(payload)) stop("Parameter status truncated")
      name <- if (name_len == 0) "" else protocol_raw_to_text(payload[offset:(offset + name_len - 1)])
      offset <- offset + name_len
      if (offset + 6 > length(payload)) stop("Parameter status truncated")
      value_type <- read_u16(payload, offset)
      offset <- offset + 2
      defaulted <- as.integer(payload[offset]) != 0L
      offset <- offset + 1
      value_len <- read_u32(payload, offset)
      offset <- offset + 4
      if (offset + value_len - 1 > length(payload)) stop("Parameter status truncated")
      value <- if (value_len == 0) "" else protocol_raw_to_text(payload[offset:(offset + value_len - 1)])
      offset <- offset + value_len
      entries[[length(entries) + 1]] <- list(
        name = name,
        value = value,
        value_type = value_type,
        defaulted = defaulted
      )
    }
    first <- if (length(entries) > 0) entries[[1]] else list(name = "", value = "")
    return(list(name = first$name, value = first$value, entries = entries))
  }
  stop("Parameter status entry count is invalid")
}

parse_parameter_description <- function(payload) {
  if (is_p1_row_description(payload)) {
    count <- read_i32(payload, 69)
    if (count < 0) stop("P1 parameter description column count invalid")
    offset <- SB_P1_ROW_DESCRIPTION_HEADER_BYTES + 1L
    types <- integer()
    for (idx in seq_len(count)) {
      if (offset + 4 + 4 + 8 + 8 + SB_P1_CANONICAL_TYPE_REF_BYTES + 4 + 5 - 1 > length(payload)) {
        stop("P1 parameter description truncated")
      }
      type_offset <- offset + 4 + 4 + 8 + 8
      types <- c(types, oid_from_canonical_type_ref(payload, type_offset))
      offset <- type_offset + SB_P1_CANONICAL_TYPE_REF_BYTES + 4
      read <- read_nullable_text(payload, offset)
      offset <- read$offset
    }
    return(types)
  }
  if (length(payload) < 4) stop("Parameter description truncated")
  offset <- 1
  count <- read_u16(payload, offset)
  offset <- offset + 4
  types <- integer()
  for (idx in seq_len(count)) {
    if (offset + 3 > length(payload)) stop("Parameter description truncated")
    types <- c(types, read_u32(payload, offset))
    offset <- offset + 4
  }
  types
}

parse_row_description <- function(payload) {
  if (is_p1_row_description(payload)) {
    return(parse_p1_row_description(payload))
  }
  if (length(payload) < 4) stop("Row description truncated")
  offset <- 1
  count <- read_u16(payload, offset)
  offset <- offset + 4
  columns <- vector("list", count)
  for (idx in seq_len(count)) {
    name_len <- read_u32(payload, offset)
    offset <- offset + 4
    name <- if (name_len == 0) "" else protocol_raw_to_text(payload[offset:(offset + name_len - 1)])
    offset <- offset + name_len
    table_oid <- read_u32(payload, offset)
    offset <- offset + 4
    column_index <- read_u16(payload, offset)
    offset <- offset + 2
    type_oid <- read_u32(payload, offset)
    offset <- offset + 4
    type_size <- readBin(payload[offset:(offset + 1)], integer(), size = 2, endian = "little", signed = TRUE)
    offset <- offset + 2
    type_modifier <- readBin(payload[offset:(offset + 3)], integer(), size = 4, endian = "little", signed = TRUE)
    offset <- offset + 4
    format <- read_u8(payload, offset)
    offset <- offset + 1
    nullable <- read_u8(payload, offset) == 1
    offset <- offset + 1
    offset <- offset + 2
    columns[[idx]] <- list(
      name = name,
      table_oid = table_oid,
      column_index = column_index,
      type_oid = type_oid,
      type_size = type_size,
      type_modifier = type_modifier,
      format = format,
      nullable = nullable
    )
  }
  columns
}

is_p1_row_description <- function(payload) {
  length(payload) >= SB_P1_ROW_DESCRIPTION_HEADER_BYTES &&
    read_u16(payload, 1) == 1L &&
    as.integer(payload[4]) == 1L
}

parse_p1_row_description <- function(payload) {
  count <- read_i32(payload, 5)
  if (count < 0) stop("P1 row description column count invalid")
  offset <- SB_P1_ROW_DESCRIPTION_HEADER_BYTES + 1L
  columns <- vector("list", count)
  fixed_column_bytes <- 4 + 4 + 8 + SB_P1_CANONICAL_TYPE_REF_BYTES + 56
  for (idx in seq_len(count)) {
    if (offset + fixed_column_bytes - 1 > length(payload)) stop("P1 row description truncated")
    ordinal <- read_i32(payload, offset)
    offset <- offset + 4
    offset <- offset + 1
    format <- if (as.integer(payload[offset]) == 1L) SB_FORMAT_TEXT else SB_FORMAT_BINARY
    offset <- offset + 1
    nullable <- as.integer(payload[offset]) == 1L
    offset <- offset + 1
    offset <- offset + 1
    offset <- offset + 8
    type_oid <- oid_from_canonical_type_ref(payload, offset)
    offset <- offset + SB_P1_CANONICAL_TYPE_REF_BYTES
    offset <- offset + 16 * 3
    offset <- offset + 4
    offset <- offset + 2
    offset <- offset + 2
    read <- read_nullable_text(payload, offset)
    offset <- read$offset
    name <- read$text
    if (!nzchar(name)) name <- paste0("column", idx)
    column_index <- if (ordinal == 0L) idx - 1L else ordinal - 1L
    columns[[idx]] <- list(
      name = name,
      table_oid = 0L,
      column_index = as.integer(column_index),
      type_oid = as.integer(type_oid),
      type_size = type_size_for_oid(type_oid),
      type_modifier = -1L,
      format = format,
      nullable = nullable
    )
  }
  columns
}

oid_from_canonical_type_ref <- function(payload, offset) {
  if (offset + 3 > length(payload)) return(SB_OID_TEXT)
  family <- read_u16(payload, offset)
  code <- read_u16(payload, offset + 2)
  if (family == 1L && code == 1L) return(SB_OID_BOOL)
  if (family == 2L && code == 3L) return(SB_OID_INT4)
  if (family == 2L && code == 4L) return(SB_OID_INT8)
  if (family == 4L && code == 1L) return(SB_OID_NUMERIC)
  if (family == 6L && code == 2L) return(SB_OID_FLOAT8)
  if (family == 8L && code == 1L) return(SB_OID_TEXT)
  if (family == 9L) return(SB_OID_BYTEA)
  if (family == 11L && code == 1L) return(SB_OID_DATE)
  if (family == 11L && code == 2L) return(SB_OID_TIME)
  if (family == 11L) return(SB_OID_TIMESTAMP)
  if (family == 12L) return(SB_OID_INTERVAL)
  if (family == 13L) return(SB_OID_UUID)
  if (family == 19L && code == 3L) return(SB_OID_MACADDR)
  if (family == 19L) return(SB_OID_INET)
  if (family == 20L) return(SB_OID_JSON)
  SB_OID_TEXT
}

type_size_for_oid <- function(type_oid) {
  if (type_oid == SB_OID_BOOL) return(1L)
  if (type_oid == SB_OID_INT4) return(4L)
  if (type_oid %in% c(SB_OID_INT8, SB_OID_FLOAT8)) return(8L)
  if (type_oid == SB_OID_UUID) return(16L)
  -1L
}

read_nullable_text <- function(payload, offset) {
  if (offset + 4 > length(payload)) stop("nullable text truncated")
  tag <- as.integer(payload[offset])
  offset <- offset + 1
  len <- read_i32(payload, offset)
  offset <- offset + 4
  if (len < 0) stop("nullable text length invalid")
  if (tag == 0L) return(list(text = "", offset = offset))
  if (offset + len - 1 > length(payload)) stop("nullable text truncated")
  text <- if (len == 0) "" else protocol_raw_to_text(payload[offset:(offset + len - 1)])
  list(text = text, offset = offset + len)
}

parse_data_row <- function(payload) {
  if (length(payload) < 4) stop("Row data truncated")
  offset <- 1
  count <- read_u16(payload, offset)
  offset <- offset + 2
  null_bytes <- read_u16(payload, offset)
  offset <- offset + 2
  null_bitmap <- if (null_bytes > 0) payload[offset:(offset + null_bytes - 1)] else raw()
  offset <- offset + null_bytes
  values <- vector("list", count)
  for (idx in seq_len(count)) {
    byte_index <- as.integer((idx - 1) / 8)
    bit_index <- (idx - 1) %% 8
    is_null <- byte_index < null_bytes && bitwAnd(as.integer(null_bitmap[byte_index + 1]), bitwShiftL(1, bit_index)) != 0
    if (is_null) {
      values[[idx]] <- list(data = NULL)
      next
    }
    len <- read_i32(payload, offset)
    offset <- offset + 4
    if (len < 0) {
      values[[idx]] <- list(data = NULL)
      next
    }
    data <- payload[offset:(offset + len - 1)]
    offset <- offset + len
    values[[idx]] <- list(data = data)
  }
  values
}

parse_command_complete <- function(payload) {
  if (length(payload) < 20) stop("Command complete truncated")
  command_type <- payload[1]
  rows <- read_u64(payload, 5)
  last_id <- read_u64(payload, 13)
  tag_bytes <- if (length(payload) > 20) payload[21:length(payload)] else raw()
  tag <- ""
  if (length(tag_bytes) > 0) {
    zero_pos <- which(tag_bytes == as.raw(0x00))
    if (length(zero_pos) > 0 && zero_pos[1] > 1) {
      tag_bytes <- tag_bytes[1:(zero_pos[1] - 1)]
    } else if (length(zero_pos) > 0) {
      tag_bytes <- raw()
    }
    if (length(tag_bytes) > 0) {
      tag <- protocol_raw_to_text(tag_bytes)
    }
  }
  list(command_type = as.integer(command_type), rows = rows, last_id = last_id, tag = tag)
}

parse_notification <- function(payload) {
  if (length(payload) < 12) stop("Notification truncated")
  offset <- 1
  process_id <- read_u32(payload, offset)
  offset <- offset + 4
  channel_len <- read_u32(payload, offset)
  offset <- offset + 4
  if (offset + channel_len + 4 - 1 > length(payload)) stop("Notification truncated")
  channel <- if (channel_len == 0) "" else protocol_raw_to_text(payload[offset:(offset + channel_len - 1)])
  offset <- offset + channel_len
  payload_len <- read_u32(payload, offset)
  offset <- offset + 4
  if (offset + payload_len - 1 > length(payload)) stop("Notification truncated")
  data <- if (payload_len > 0) payload[offset:(offset + payload_len - 1)] else raw()
  offset <- offset + payload_len
  change_type <- NULL
  row_id <- NULL
  if (offset <= length(payload)) {
    change_type <- protocol_raw_to_text(payload[offset])
    offset <- offset + 1
    if (offset + 7 <= length(payload)) {
      row_id <- read_u64(payload, offset)
    }
  }
  list(process_id = process_id, channel = channel, payload = data, change_type = change_type, row_id = row_id)
}

parse_query_plan <- function(payload) {
  if (length(payload) < 32) stop("Query plan truncated")
  format <- read_u32(payload, 1)
  plan_len <- read_u32(payload, 5)
  planning_time_us <- read_u64(payload, 9)
  estimated_rows <- read_u64(payload, 17)
  estimated_cost <- read_u64(payload, 25)
  if (32 + plan_len > length(payload)) stop("Query plan truncated")
  start <- 33
  plan <- if (plan_len > 0) payload[start:(start + plan_len - 1)] else raw()
  list(
    format = format,
    planning_time_us = planning_time_us,
    estimated_rows = estimated_rows,
    estimated_cost = estimated_cost,
    plan = plan
  )
}

parse_sblr_compiled <- function(payload) {
  if (length(payload) < 16) stop("SBLR compiled truncated")
  hash <- read_u64(payload, 1)
  version <- read_u32(payload, 9)
  len <- read_u32(payload, 13)
  if (16 + len > length(payload)) stop("SBLR compiled truncated")
  start <- 17
  bytecode <- if (len > 0) payload[start:(start + len - 1)] else raw()
  list(hash = hash, version = version, bytecode = bytecode)
}

parse_error_message <- function(payload) {
  offset <- 1
  severity <- ""
  sqlstate <- ""
  message <- ""
  detail <- ""
  hint <- ""
  while (offset <= length(payload)) {
    field <- as.integer(payload[offset])
    offset <- offset + 1
    if (field == 0) break
    start <- offset
    while (offset <= length(payload) && payload[offset] != as.raw(0x00)) {
      offset <- offset + 1
    }
    if (offset > length(payload)) break
    value <- if (offset == start) "" else protocol_raw_to_text(payload[start:(offset - 1)])
    offset <- offset + 1
    if (field == as.integer(charToRaw("S"))) severity <- value
    if (field == as.integer(charToRaw("C"))) sqlstate <- value
    if (field == as.integer(charToRaw("M"))) message <- value
    if (field == as.integer(charToRaw("D"))) detail <- value
    if (field == as.integer(charToRaw("H"))) hint <- value
  }
  list(severity = severity, sqlstate = sqlstate, message = message, detail = detail, hint = hint)
}
