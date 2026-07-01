# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

SB_FORMAT_TEXT <- 0L
SB_FORMAT_BINARY <- 1L

SB_OID_BOOL <- 16L
SB_OID_BYTEA <- 17L
SB_OID_CHAR <- 18L
SB_OID_INT8 <- 20L
SB_OID_INT2 <- 21L
SB_OID_INT4 <- 23L
SB_OID_TEXT <- 25L
SB_OID_JSON <- 114L
SB_OID_XML <- 142L
SB_OID_POINT <- 600L
SB_OID_LSEG <- 601L
SB_OID_PATH <- 602L
SB_OID_BOX <- 603L
SB_OID_POLYGON <- 604L
SB_OID_LINE <- 628L
SB_OID_FLOAT4 <- 700L
SB_OID_FLOAT8 <- 701L
SB_OID_CIRCLE <- 718L
SB_OID_MONEY <- 790L
SB_OID_MACADDR <- 829L
SB_OID_CIDR <- 650L
SB_OID_INET <- 869L
SB_OID_MACADDR8 <- 774L
SB_OID_BPCHAR <- 1042L
SB_OID_VARCHAR <- 1043L
SB_OID_DATE <- 1082L
SB_OID_TIME <- 1083L
SB_OID_TIMESTAMP <- 1114L
SB_OID_TIMESTAMPTZ <- 1184L
SB_OID_INTERVAL <- 1186L
SB_OID_TIMETZ <- 1266L
SB_OID_NUMERIC <- 1700L
SB_OID_UUID <- 2950L
SB_OID_JSONB <- 3802L
SB_OID_RECORD <- 2249L
SB_OID_INT4RANGE <- 3904L
SB_OID_NUMRANGE <- 3906L
SB_OID_TSRANGE <- 3908L
SB_OID_TSTZRANGE <- 3910L
SB_OID_DATERANGE <- 3912L
SB_OID_INT8RANGE <- 3926L
SB_OID_TSVECTOR <- 3614L
SB_OID_TSQUERY <- 3615L
SB_OID_SB_VECTOR <- 16386L

SB_RANGE_EMPTY <- 0x01
SB_RANGE_LB_INC <- 0x02
SB_RANGE_UB_INC <- 0x04
SB_RANGE_LB_INF <- 0x08
SB_RANGE_UB_INF <- 0x10

sb_jsonb <- function(raw = NULL, value = NULL) {
  structure(list(raw = raw, value = value), class = "sb_jsonb")
}

sb_geometry <- function(wkb, srid = NULL, wkt = NULL) {
  structure(list(wkb = wkb, srid = srid, wkt = wkt), class = "sb_geometry")
}

sb_range <- function(lower = NULL, upper = NULL, lower_inclusive = FALSE, upper_inclusive = FALSE,
                     lower_infinite = FALSE, upper_infinite = FALSE, empty = FALSE, range_oid = NULL) {
  structure(list(
    lower = lower,
    upper = upper,
    lower_inclusive = isTRUE(lower_inclusive),
    upper_inclusive = isTRUE(upper_inclusive),
    lower_infinite = isTRUE(lower_infinite),
    upper_infinite = isTRUE(upper_infinite),
    empty = isTRUE(empty),
    range_oid = range_oid
  ), class = "sb_range")
}

sb_composite <- function(fields = list(), type_oid = SB_OID_RECORD) {
  structure(list(fields = fields, type_oid = type_oid), class = "sb_composite")
}

encode_param <- function(value) {
  if (is.null(value) || (is.atomic(value) && all(is.na(value)))) {
    return(list(param = list(format = SB_FORMAT_BINARY, is_null = TRUE), oid = 0L))
  }

  if (inherits(value, "sb_jsonb")) {
    raw <- value$raw
    if ((is.null(raw) || length(raw) == 0) && !is.null(value$value)) {
      if (!requireNamespace("jsonlite", quietly = TRUE)) stop("jsonlite is required for JSONB encoding")
      raw <- charToRaw(jsonlite::toJSON(value$value, auto_unbox = TRUE))
    }
    if (is.null(raw) || length(raw) == 0) stop("JSONB requires raw payload")
    return(list(param = list(format = SB_FORMAT_BINARY, data = encode_length_prefixed(raw)), oid = SB_OID_JSONB))
  }

  if (inherits(value, "sb_geometry")) {
    wkb <- value$wkb
    if (is.null(wkb) || length(wkb) == 0) stop("geometry requires WKB payload")
    return(list(param = list(format = SB_FORMAT_BINARY, data = encode_length_prefixed(wkb)), oid = SB_OID_POINT))
  }

  if (inherits(value, "sb_range")) {
    encoded <- encode_range(value)
    return(list(param = list(format = SB_FORMAT_BINARY, data = encoded$data), oid = encoded$oid))
  }

  if (inherits(value, "sb_composite")) {
    encoded <- encode_composite(value)
    return(list(param = list(format = SB_FORMAT_BINARY, data = encoded$data), oid = encoded$oid))
  }

  if (inherits(value, "Date")) {
    return(list(param = list(format = SB_FORMAT_BINARY, data = encode_date(value)), oid = SB_OID_DATE))
  }

  if (inherits(value, "POSIXct") || inherits(value, "POSIXt")) {
    return(list(param = list(format = SB_FORMAT_BINARY, data = encode_timestamp(value)), oid = SB_OID_TIMESTAMPTZ))
  }

  if (is.logical(value) && length(value) == 1) {
    return(list(param = list(format = SB_FORMAT_BINARY, data = as.raw(ifelse(value, 1, 0))), oid = SB_OID_BOOL))
  }

  if (is.integer(value) && length(value) == 1) {
    return(list(param = list(format = SB_FORMAT_BINARY, data = writeBin(as.integer(value), raw(), size = 4, endian = "little")), oid = SB_OID_INT4))
  }

  if (is.numeric(value) && length(value) == 1) {
    if (!is.finite(value)) stop("numeric value must be finite")
    if (value == floor(value) && value >= -2147483648 && value <= 2147483647) {
      return(list(param = list(format = SB_FORMAT_BINARY, data = writeBin(as.integer(value), raw(), size = 4, endian = "little")), oid = SB_OID_INT4))
    }
    return(list(param = list(format = SB_FORMAT_BINARY, data = writeBin(as.numeric(value), raw(), size = 8, endian = "little")), oid = SB_OID_FLOAT8))
  }

  if (is.raw(value)) {
    return(list(param = list(format = SB_FORMAT_BINARY, data = encode_length_prefixed(value)), oid = SB_OID_BYTEA))
  }

  if (is.numeric(value) && length(value) > 1) {
    literal <- format_vector_literal(value)
    return(list(param = list(format = SB_FORMAT_BINARY, data = encode_length_prefixed(charToRaw(literal))), oid = SB_OID_SB_VECTOR))
  }

  if (is.list(value) || (is.atomic(value) && length(value) > 1)) {
    literal <- format_array_literal(value)
    return(list(param = list(format = SB_FORMAT_BINARY, data = encode_length_prefixed(charToRaw(literal))), oid = 0L))
  }

  if (is.character(value)) {
    if (length(value) > 1) {
      literal <- format_array_literal(as.list(value))
      return(list(param = list(format = SB_FORMAT_BINARY, data = encode_length_prefixed(charToRaw(literal))), oid = 0L))
    }
    if (grepl("^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$", value)) {
      hex <- gsub("-", "", value)
      return(list(param = list(format = SB_FORMAT_BINARY, data = as.raw(strtoi(substring(hex, seq(1, 31, 2), seq(2, 32, 2)), 16L))), oid = SB_OID_UUID))
    }
    return(list(param = list(format = SB_FORMAT_BINARY, data = encode_length_prefixed(charToRaw(value))), oid = SB_OID_TEXT))
  }

  if (!requireNamespace("jsonlite", quietly = TRUE)) stop("jsonlite is required for JSON encoding")
  raw <- charToRaw(jsonlite::toJSON(value, auto_unbox = TRUE))
  list(param = list(format = SB_FORMAT_BINARY, data = encode_length_prefixed(raw)), oid = SB_OID_JSON)
}

decode_value <- function(type_oid, data, format) {
  if (is.null(data)) return(NA)
  if (type_oid == 0) {
    if (format == SB_FORMAT_TEXT) return(parse_unknown_text(decode_text_value(data)))
    return(decode_unknown_binary(data))
  }
  if (format == SB_FORMAT_TEXT) return(decode_text_value(data))
  decode_binary_value(type_oid, data)
}

read_i64_numeric <- function(data) {
  if (length(data) < 8) return(NA_real_)
  low <- read_u32(data, 1)
  high <- read_u32(data, 5)
  value <- low + high * 4294967296
  if (high >= 2147483648) {
    value <- value - 18446744073709551616
  }
  as.numeric(value)
}

decode_binary_value <- function(type_oid, data) {
  if (type_oid == SB_OID_BOOL) {
    return(as.logical(as.integer(data[1]) == 1))
  }
  if (type_oid == SB_OID_INT2) {
    return(readBin(data, integer(), size = 2, endian = "little"))
  }
  if (type_oid == SB_OID_INT4) {
    return(readBin(data, integer(), size = 4, endian = "little"))
  }
  if (type_oid == SB_OID_INT8) {
    return(read_i64_numeric(data))
  }
  if (type_oid == SB_OID_FLOAT4) {
    return(readBin(data, numeric(), size = 4, endian = "little"))
  }
  if (type_oid == SB_OID_FLOAT8) {
    return(readBin(data, numeric(), size = 8, endian = "little"))
  }
  if (type_oid == SB_OID_NUMERIC) {
    text <- raw_to_display_text(strip_length_prefixed(data))
    num <- suppressWarnings(as.numeric(text))
    return(ifelse(is.na(num), text, num))
  }
  if (type_oid == SB_OID_MONEY) {
    cents <- read_i64_numeric(data)
    return(cents / 100)
  }
  if (type_oid %in% c(SB_OID_TEXT, SB_OID_VARCHAR, SB_OID_CHAR, SB_OID_BPCHAR, SB_OID_JSON, SB_OID_XML, SB_OID_TSVECTOR, SB_OID_TSQUERY, SB_OID_INET, SB_OID_CIDR, SB_OID_MACADDR, SB_OID_MACADDR8)) {
    return(raw_to_display_text(strip_length_prefixed(data)))
  }
  if (type_oid == SB_OID_JSONB) {
    return(sb_jsonb(raw = strip_length_prefixed(data)))
  }
  if (type_oid == SB_OID_BYTEA) {
    return(strip_length_prefixed(data))
  }
  if (type_oid == SB_OID_DATE) {
    days <- readBin(data, integer(), size = 4, endian = "little")
    return(as.Date("2000-01-01") + days)
  }
  if (type_oid == SB_OID_TIME) {
    micros <- read_i64_numeric(data)
    seconds <- micros / 1e6
    return(as.POSIXct(seconds, origin = "1970-01-01", tz = "UTC"))
  }
  if (type_oid %in% c(SB_OID_TIMESTAMP, SB_OID_TIMESTAMPTZ)) {
    micros <- read_i64_numeric(data)
    base <- as.POSIXct("2000-01-01", tz = "UTC")
    return(base + micros / 1e6)
  }
  if (type_oid == SB_OID_INTERVAL) {
    micros <- read_i64_numeric(data[1:8])
    days <- readBin(data[9:12], integer(), size = 4, endian = "little")
    months <- readBin(data[13:16], integer(), size = 4, endian = "little")
    return(list(months = months, days = days, micros = micros))
  }
  if (type_oid == SB_OID_UUID) {
    hex <- paste(sprintf("%02x", as.integer(data)), collapse = "")
    return(paste0(substr(hex, 1, 8), "-", substr(hex, 9, 12), "-", substr(hex, 13, 16), "-", substr(hex, 17, 20), "-", substr(hex, 21, 32)))
  }
  if (type_oid %in% c(SB_OID_INT4RANGE, SB_OID_INT8RANGE, SB_OID_NUMRANGE, SB_OID_TSRANGE, SB_OID_TSTZRANGE, SB_OID_DATERANGE)) {
    return(decode_range(type_oid, data))
  }
  if (type_oid == SB_OID_SB_VECTOR) {
    payload <- strip_length_prefixed(data)
    if (contains_nul_byte(payload)) return(hex_encode_raw(payload))
    return(parse_vector_literal(rawToChar(payload)))
  }
  if (type_oid %in% c(SB_OID_POINT, SB_OID_LSEG, SB_OID_PATH, SB_OID_BOX, SB_OID_POLYGON, SB_OID_LINE, SB_OID_CIRCLE)) {
    return(sb_geometry(strip_length_prefixed(data)))
  }
  if (type_oid == SB_OID_RECORD) {
    return(decode_composite(data))
  }
  data
}

decode_text_value <- function(data) {
  if (length(data) >= 4) {
    len <- readBin(data[1:4], integer(), size = 4, endian = "little", signed = FALSE)
    if (len <= length(data) - 4) {
      return(raw_to_display_text(data[5:(4 + len)]))
    }
  }
  raw_to_display_text(data)
}

decode_unknown_binary <- function(data) {
  trimmed <- strip_trailing_nulls(data)
  if (length(trimmed) > 0 && !contains_nul_byte(trimmed) && looks_like_text(trimmed)) {
    return(parse_unknown_text(rawToChar(trimmed)))
  }
  len <- length(data)
  if (len == 1) return(as.integer(data[1]))
  if (len == 2) return(readBin(data, integer(), size = 2, endian = "little"))
  if (len == 4) return(readBin(data, integer(), size = 4, endian = "little"))
  if (len == 8) return(read_i64_numeric(data))
  paste0("0x", paste(sprintf("%02x", as.integer(data)), collapse = ""))
}

parse_unknown_text <- function(text) {
  trimmed <- trimws(text)
  if (trimmed == "") return(text)
  lowered <- tolower(trimmed)
  if (lowered == "true") return(TRUE)
  if (lowered == "false") return(FALSE)
  if (grepl("^[+-]?\\d+$", trimmed)) {
    value <- suppressWarnings(as.numeric(trimmed))
    return(ifelse(is.na(value), text, value))
  }
  if (grepl("^[+-]?(?:\\d+\\.?\\d*|\\d*\\.?\\d+)(?:[eE][+-]?\\d+)?$", trimmed)) {
    value <- suppressWarnings(as.numeric(trimmed))
    return(ifelse(is.na(value), text, value))
  }
  text
}

strip_trailing_nulls <- function(data) {
  end <- length(data)
  while (end > 0 && data[end] == as.raw(0)) {
    end <- end - 1
  }
  if (end == 0) return(raw())
  data[1:end]
}

contains_nul_byte <- function(data) {
  any(as.integer(data) == 0L)
}

hex_encode_raw <- function(data) {
  paste0("0x", paste(sprintf("%02x", as.integer(data)), collapse = ""))
}

raw_to_display_text <- function(data) {
  if (length(data) == 0) return("")
  if (contains_nul_byte(data)) return(hex_encode_raw(data))
  rawToChar(data)
}

looks_like_text <- function(data) {
  bytes <- as.integer(data)
  for (byte in bytes) {
    if (byte == 0x09 || byte == 0x0a || byte == 0x0d) next
    if (byte < 0x20 || byte > 0x7e) return(FALSE)
  }
  TRUE
}

encode_length_prefixed <- function(data) {
  c(pack_u32(length(data)), data)
}

strip_length_prefixed <- function(data) {
  if (length(data) < 4) return(data)
  len <- read_u32(data, 1)
  if (len <= length(data) - 4) return(data[5:(4 + len)])
  data
}

encode_date <- function(value) {
  base <- as.Date("2000-01-01")
  days <- as.integer(value - base)
  writeBin(days, raw(), size = 4, endian = "little")
}

encode_timestamp <- function(value) {
  t <- as.POSIXct(value, tz = "UTC")
  base <- as.POSIXct("2000-01-01", tz = "UTC")
  micros <- as.numeric(difftime(t, base, units = "secs")) * 1e6
  pack_i64(round(micros))
}

encode_range <- function(range) {
  oid <- resolve_range_oid(range)
  flags <- 0L
  if (isTRUE(range$empty)) flags <- bitwOr(flags, SB_RANGE_EMPTY)
  if (isTRUE(range$lower_inclusive)) flags <- bitwOr(flags, SB_RANGE_LB_INC)
  if (isTRUE(range$upper_inclusive)) flags <- bitwOr(flags, SB_RANGE_UB_INC)
  if (isTRUE(range$lower_infinite)) flags <- bitwOr(flags, SB_RANGE_LB_INF)
  if (isTRUE(range$upper_infinite)) flags <- bitwOr(flags, SB_RANGE_UB_INF)
  parts <- c(as.raw(flags), raw(3))
  if (!isTRUE(range$empty) && !isTRUE(range$lower_infinite)) {
    bound <- encode_range_bound(oid, range$lower)
    parts <- c(parts, pack_u32(length(bound)), bound)
  }
  if (!isTRUE(range$empty) && !isTRUE(range$upper_infinite)) {
    bound <- encode_range_bound(oid, range$upper)
    parts <- c(parts, pack_u32(length(bound)), bound)
  }
  list(data = parts, oid = oid)
}

resolve_range_oid <- function(range) {
  if (!is.null(range$range_oid)) return(range$range_oid)
  sample <- range$lower
  if (is.null(sample)) sample <- range$upper
  if (is.null(sample)) stop("range type cannot be inferred")
  if (inherits(sample, "POSIXct") || inherits(sample, "POSIXt")) return(SB_OID_TSTZRANGE)
  if (inherits(sample, "Date")) return(SB_OID_DATERANGE)
  if (is.integer(sample)) return(SB_OID_INT4RANGE)
  if (is.numeric(sample)) return(SB_OID_NUMRANGE)
  SB_OID_NUMRANGE
}

encode_range_bound <- function(range_oid, value) {
  if (range_oid == SB_OID_INT4RANGE) {
    return(writeBin(as.integer(value), raw(), size = 4, endian = "little"))
  }
  if (range_oid == SB_OID_INT8RANGE) {
    return(pack_i64(as.numeric(value)))
  }
  if (range_oid == SB_OID_NUMRANGE) {
    return(encode_length_prefixed(charToRaw(as.character(value))))
  }
  if (range_oid %in% c(SB_OID_TSRANGE, SB_OID_TSTZRANGE)) {
    return(encode_length_prefixed(encode_timestamp(value)))
  }
  if (range_oid == SB_OID_DATERANGE) {
    return(encode_length_prefixed(encode_date(value)))
  }
  encode_length_prefixed(charToRaw(as.character(value)))
}

decode_range <- function(range_oid, data) {
  flags <- as.integer(data[1])
  offset <- 5
  empty <- bitwAnd(flags, SB_RANGE_EMPTY) != 0
  lower_infinite <- bitwAnd(flags, SB_RANGE_LB_INF) != 0
  upper_infinite <- bitwAnd(flags, SB_RANGE_UB_INF) != 0
  lower <- NULL
  upper <- NULL
  if (!empty && !lower_infinite) {
    len <- read_u32(data, offset)
    offset <- offset + 4
    bound <- data[offset:(offset + len - 1)]
    offset <- offset + len
    lower <- decode_range_bound(range_oid, bound)
  }
  if (!empty && !upper_infinite) {
    len <- read_u32(data, offset)
    offset <- offset + 4
    bound <- data[offset:(offset + len - 1)]
    offset <- offset + len
    upper <- decode_range_bound(range_oid, bound)
  }
  sb_range(
    lower = lower,
    upper = upper,
    lower_inclusive = bitwAnd(flags, SB_RANGE_LB_INC) != 0,
    upper_inclusive = bitwAnd(flags, SB_RANGE_UB_INC) != 0,
    lower_infinite = lower_infinite,
    upper_infinite = upper_infinite,
    empty = empty,
    range_oid = range_oid
  )
}

decode_range_bound <- function(range_oid, data) {
  if (range_oid == SB_OID_INT4RANGE) {
    return(readBin(data, integer(), size = 4, endian = "little"))
  }
  if (range_oid == SB_OID_INT8RANGE) {
    return(read_i64_numeric(data))
  }
  if (range_oid == SB_OID_NUMRANGE) {
    text <- raw_to_display_text(strip_length_prefixed(data))
    num <- suppressWarnings(as.numeric(text))
    return(ifelse(is.na(num), text, num))
  }
  if (range_oid %in% c(SB_OID_TSRANGE, SB_OID_TSTZRANGE)) {
    return(decode_binary_value(SB_OID_TIMESTAMPTZ, strip_length_prefixed(data)))
  }
  if (range_oid == SB_OID_DATERANGE) {
    return(decode_binary_value(SB_OID_DATE, strip_length_prefixed(data)))
  }
  raw_to_display_text(strip_length_prefixed(data))
}

encode_composite <- function(value) {
  fields <- value$fields
  if (is.null(fields)) fields <- list()
  type_oid <- value$type_oid
  if (is.null(type_oid) || type_oid == 0) type_oid <- SB_OID_RECORD
  buffer <- pack_i32(length(fields))
  for (field in fields) {
    field_oid <- if (!is.null(field$oid)) field$oid else 0L
    data <- NULL
    if (!is.null(field$raw)) {
      data <- field$raw
    } else if (!is.null(field$value)) {
      encoded <- encode_param(field$value)
      if (field_oid == 0) field_oid <- encoded$oid
      if (!isTRUE(encoded$param$is_null)) data <- encoded$param$data
    }
    if (field_oid == 0) stop("composite field OID is required")
    buffer <- c(buffer, pack_u32(field_oid))
    if (is.null(data)) {
      buffer <- c(buffer, pack_i32(-1))
    } else {
      buffer <- c(buffer, pack_i32(length(data)), data)
    }
  }
  list(data = buffer, oid = type_oid)
}

decode_composite <- function(data) {
  if (length(data) < 4) return(sb_composite(list()))
  count <- read_i32(data, 1)
  offset <- 5
  fields <- list()
  for (i in seq_len(count)) {
    if (offset + 7 > length(data)) break
    oid <- read_u32(data, offset)
    offset <- offset + 4
    length_val <- read_i32(data, offset)
    offset <- offset + 4
    if (length_val < 0) {
      fields[[length(fields) + 1]] <- list(oid = oid, value = NULL, raw = NULL)
      next
    }
    if (offset + length_val - 1 > length(data)) break
    raw <- data[offset:(offset + length_val - 1)]
    offset <- offset + length_val
    value <- decode_binary_value(oid, raw)
    fields[[length(fields) + 1]] <- list(oid = oid, value = value, raw = raw)
  }
  sb_composite(fields)
}

format_array_literal <- function(values) {
  if (!is.list(values)) values <- as.list(values)
  items <- vapply(values, format_array_item, character(1))
  paste0("{", paste(items, collapse = ","), "}")
}

format_array_item <- function(value) {
  if (is.null(value) || (is.atomic(value) && all(is.na(value)))) return("NULL")
  if (is.list(value)) return(format_array_literal(value))
  if (is.logical(value)) return(ifelse(value, "true", "false"))
  if (is.numeric(value)) return(as.character(value))
  text <- gsub("\"", "\\\\\"", as.character(value))
  paste0("\"", text, "\"")
}

parse_array_literal <- function(text) {
  trimmed <- trimws(text)
  if (trimmed == "" || trimmed == "{}") return(list())
  if (startsWith(trimmed, "{") && endsWith(trimmed, "}")) {
    trimmed <- substr(trimmed, 2, nchar(trimmed) - 1)
  }
  split_array_items(trimmed)
}

split_array_items <- function(text) {
  items <- list()
  depth <- 0
  buffer <- ""
  for (ch in strsplit(text, "", fixed = TRUE)[[1]]) {
    if (ch == "{") {
      depth <- depth + 1
      buffer <- paste0(buffer, ch)
    } else if (ch == "}") {
      depth <- max(0, depth - 1)
      buffer <- paste0(buffer, ch)
    } else if (ch == "," && depth == 0) {
      items[[length(items) + 1]] <- parse_array_item(buffer)
      buffer <- ""
    } else {
      buffer <- paste0(buffer, ch)
    }
  }
  if (buffer != "" || text != "") {
    items[[length(items) + 1]] <- parse_array_item(buffer)
  }
  items
}

parse_array_item <- function(token) {
  token <- trimws(token)
  if (toupper(token) == "NULL") return(NA)
  if (startsWith(token, "{") && endsWith(token, "}")) return(parse_array_literal(token))
  if (startsWith(token, "[") && endsWith(token, "]")) return(parse_vector_literal(token))
  if (tolower(token) == "true") return(TRUE)
  if (tolower(token) == "false") return(FALSE)
  num <- suppressWarnings(as.numeric(token))
  if (!is.na(num)) return(num)
  token
}

format_vector_literal <- function(values) {
  paste0("[", paste(as.numeric(values), collapse = ","), "]")
}

parse_vector_literal <- function(text) {
  trimmed <- trimws(text)
  if (startsWith(trimmed, "[") && endsWith(trimmed, "]")) {
    trimmed <- substr(trimmed, 2, nchar(trimmed) - 1)
  }
  if (trimmed == "") return(numeric())
  as.numeric(strsplit(trimmed, ",", fixed = TRUE)[[1]])
}
