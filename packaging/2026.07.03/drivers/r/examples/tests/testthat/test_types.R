# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

test_that("decode UUID", {
  bytes <- as.raw(strtoi(substring("12345678123456781234567812345678", seq(1, 31, 2), seq(2, 32, 2)), 16L))
  out <- decode_value(SB_OID_UUID, bytes, SB_FORMAT_BINARY)
  expect_equal(out, "12345678-1234-5678-1234-567812345678")
})

test_that("decode primitive scalar matrix", {
  expect_true(decode_value(SB_OID_BOOL, as.raw(1), SB_FORMAT_BINARY))
  expect_equal(decode_value(SB_OID_INT2, writeBin(as.integer(12), raw(), size = 2, endian = "little"), SB_FORMAT_BINARY), 12)
  expect_equal(decode_value(SB_OID_INT4, writeBin(as.integer(34), raw(), size = 4, endian = "little"), SB_FORMAT_BINARY), 34)
  expect_equal(decode_value(SB_OID_INT8, pack_i64(56), SB_FORMAT_BINARY), 56)
  expect_equal(decode_value(SB_OID_FLOAT8, writeBin(as.numeric(1.25), raw(), size = 8, endian = "little"), SB_FORMAT_BINARY), 1.25)
  expect_equal(decode_value(SB_OID_NUMERIC, encode_length_prefixed(charToRaw("123.45")), SB_FORMAT_BINARY), 123.45)
  expect_equal(decode_value(SB_OID_MONEY, pack_i64(12345), SB_FORMAT_BINARY), 123.45)
  expect_equal(decode_value(SB_OID_TEXT, encode_length_prefixed(charToRaw("hello")), SB_FORMAT_BINARY), "hello")
  expect_equal(decode_value(SB_OID_INET, encode_length_prefixed(charToRaw("10.0.0.1")), SB_FORMAT_BINARY), "10.0.0.1")
  expect_equal(decode_value(SB_OID_CIDR, encode_length_prefixed(charToRaw("10.0.0.0/24")), SB_FORMAT_BINARY), "10.0.0.0/24")
  expect_equal(decode_value(SB_OID_MACADDR, encode_length_prefixed(charToRaw("08:00:2b:01:02:03")), SB_FORMAT_BINARY), "08:00:2b:01:02:03")
})

test_that("decode temporal and interval matrix", {
  out_date <- decode_value(SB_OID_DATE, writeBin(as.integer(1), raw(), size = 4, endian = "little"), SB_FORMAT_BINARY)
  expect_equal(out_date, as.Date("2000-01-02"))

  out_time <- decode_value(SB_OID_TIME, pack_i64(12 * 1e6), SB_FORMAT_BINARY)
  expect_equal(as.numeric(out_time), 12)

  out_ts <- decode_value(SB_OID_TIMESTAMPTZ, pack_i64(90 * 1e6), SB_FORMAT_BINARY)
  expect_equal(as.numeric(out_ts), as.numeric(as.POSIXct("2000-01-01 00:01:30", tz = "UTC")))

  interval_raw <- c(
    pack_i64(5000000),
    writeBin(as.integer(2), raw(), size = 4, endian = "little"),
    writeBin(as.integer(3), raw(), size = 4, endian = "little")
  )
  out_interval <- decode_value(SB_OID_INTERVAL, interval_raw, SB_FORMAT_BINARY)
  expect_equal(out_interval$months, 3)
  expect_equal(out_interval$days, 2)
  expect_equal(out_interval$micros, 5000000)
})

test_that("decode vector, range, and composite values", {
  vector_payload <- encode_length_prefixed(charToRaw("[1,2,3]"))
  out_vector <- decode_value(SB_OID_SB_VECTOR, vector_payload, SB_FORMAT_BINARY)
  expect_equal(out_vector, c(1, 2, 3))

  encoded_range <- encode_range(sb_range(lower = 2L, upper = 8L, lower_inclusive = TRUE, range_oid = SB_OID_INT4RANGE))
  out_range <- decode_value(SB_OID_INT4RANGE, encoded_range$data, SB_FORMAT_BINARY)
  expect_s3_class(out_range, "sb_range")
  expect_equal(out_range$lower, 2)
  expect_equal(out_range$upper, 8)
  expect_true(out_range$lower_inclusive)
  expect_false(out_range$upper_inclusive)

  encoded_composite <- encode_composite(sb_composite(fields = list(
    list(oid = SB_OID_INT4, value = 7L),
    list(oid = SB_OID_TEXT, value = "alpha")
  )))
  out_composite <- decode_value(SB_OID_RECORD, encoded_composite$data, SB_FORMAT_BINARY)
  expect_s3_class(out_composite, "sb_composite")
  expect_equal(out_composite$fields[[1]]$value, 7)
  expect_equal(out_composite$fields[[2]]$value, "alpha")
})

test_that("encode_param covers primitive and complex wrappers", {
  bool_encoded <- encode_param(TRUE)
  expect_equal(bool_encoded$oid, SB_OID_BOOL)

  int_encoded <- encode_param(42L)
  expect_equal(int_encoded$oid, SB_OID_INT4)

  float_encoded <- encode_param(1.5)
  expect_equal(float_encoded$oid, SB_OID_FLOAT8)

  uuid_encoded <- encode_param("12345678-1234-5678-1234-567812345678")
  expect_equal(uuid_encoded$oid, SB_OID_UUID)
  expect_equal(length(uuid_encoded$param$data), 16)

  jsonb_encoded <- encode_param(sb_jsonb(raw = charToRaw("{\"k\":1}")))
  expect_equal(jsonb_encoded$oid, SB_OID_JSONB)
  decoded_jsonb <- decode_value(SB_OID_JSONB, jsonb_encoded$param$data, SB_FORMAT_BINARY)
  expect_s3_class(decoded_jsonb, "sb_jsonb")
  expect_equal(rawToChar(decoded_jsonb$raw), "{\"k\":1}")

  geometry_encoded <- encode_param(sb_geometry(as.raw(c(1, 2, 3, 4))))
  expect_equal(geometry_encoded$oid, SB_OID_POINT)
  decoded_geometry <- decode_value(SB_OID_POINT, geometry_encoded$param$data, SB_FORMAT_BINARY)
  expect_s3_class(decoded_geometry, "sb_geometry")
  expect_equal(decoded_geometry$wkb, as.raw(c(1, 2, 3, 4)))
})

test_that("encode_param supports vector and array literal families", {
  vector_encoded <- encode_param(c(1, 2, 3))
  expect_equal(vector_encoded$oid, SB_OID_SB_VECTOR)
  decoded_vector <- decode_value(SB_OID_SB_VECTOR, vector_encoded$param$data, SB_FORMAT_BINARY)
  expect_equal(decoded_vector, c(1, 2, 3))

  array_encoded <- encode_param(list(1, "two", TRUE))
  expect_equal(array_encoded$oid, 0L)
  array_literal <- rawToChar(strip_length_prefixed(array_encoded$param$data))
  expect_equal(array_literal, "{1,\"two\",true}")

  nested_array_literal <- format_array_literal(list(1, list(2, 3)))
  expect_equal(nested_array_literal, "{1,{2,3}}")

  parsed <- parse_array_literal("{1,2,NULL,true,false}")
  expect_equal(parsed[[1]], 1)
  expect_equal(parsed[[2]], 2)
  expect_true(is.na(parsed[[3]]))
  expect_true(parsed[[4]])
  expect_false(parsed[[5]])
})
