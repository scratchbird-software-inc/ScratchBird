# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

build_server_error_payload <- function(
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

test_that("SQLSTATE mapping supports exact and class-prefix fallbacks", {
  expect_equal(sb_sqlstate_error_class("23505"), "scratchbird_integrity_error")
  expect_equal(sb_sqlstate_error_class("08ZZZ"), "scratchbird_connection_error")
  expect_equal(sb_sqlstate_error_class("22ZZZ"), "scratchbird_data_error")
  expect_equal(sb_sqlstate_error_class("XX999"), "scratchbird_internal_error")
  expect_null(sb_sqlstate_error_class("ZZZZZ"))
  expect_null(sb_sqlstate_error_class("2200"))
})

test_that("query errors expose SQLSTATE metadata with typed classes", {
  payload <- build_server_error_payload(
    sqlstate = "23505",
    message = "duplicate key value violates unique constraint",
    detail = "Key (id)=(1) already exists",
    hint = "Use a different key"
  )

  err <- tryCatch(
    {
      sb_raise_query_error(payload)
      NULL
    },
    error = function(e) e
  )

  expect_s3_class(err, "scratchbird_integrity_error")
  expect_s3_class(err, "scratchbird_sqlstate_error")
  expect_s3_class(err, "scratchbird_error")
  expect_equal(err$sqlstate, "23505")
  expect_equal(err$detail, "Key (id)=(1) already exists")
  expect_equal(err$hint, "Use a different key")
  expect_match(conditionMessage(err), "\\[23505\\]")
})

test_that("unknown SQLSTATE classes fall back to generic scratchbird errors", {
  payload <- build_server_error_payload(sqlstate = "ZZZZZ", message = "unmapped failure")
  err <- tryCatch(
    {
      sb_raise_query_error(payload)
      NULL
    },
    error = function(e) e
  )

  expect_s3_class(err, "scratchbird_sqlstate_error")
  expect_s3_class(err, "scratchbird_error")
  expect_false(inherits(err, "scratchbird_data_error"))
  expect_match(conditionMessage(err), "\\[ZZZZZ\\]")
})

test_that("errors without SQLSTATE stay generic and omit SQLSTATE prefix", {
  payload <- build_server_error_payload(sqlstate = "", message = "query failed")
  err <- tryCatch(
    {
      sb_raise_query_error(payload)
      NULL
    },
    error = function(e) e
  )

  expect_s3_class(err, "scratchbird_error")
  expect_false(inherits(err, "scratchbird_sqlstate_error"))
  expect_equal(err$sqlstate, "")
  expect_equal(conditionMessage(err), "query failed")
})

test_that("retry scope helper classifies statement and reconnect boundaries", {
  expect_equal(sb_retry_scope_for_sqlstate("40001"), "statement")
  expect_equal(sb_retry_scope_for_sqlstate("40P01"), "statement")
  expect_equal(sb_retry_scope_for_sqlstate("08006"), "reconnect")
  expect_equal(sb_retry_scope_for_sqlstate("57014"), "none")
  expect_equal(sb_retry_scope_for_sqlstate(NULL), "none")
  expect_true(sb_is_retryable_sqlstate("40001"))
  expect_false(sb_is_retryable_sqlstate("57014"))
})
