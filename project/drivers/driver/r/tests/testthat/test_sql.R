# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

test_that("normalize positional params", {
  sql <- "SELECT * FROM t WHERE id = ? AND name = ?"
  out <- sb_normalize(sql, list(42, "Ada"))
  expect_equal(out$sql, "SELECT * FROM t WHERE id = $1 AND name = $2")
  expect_equal(out$params, list(42, "Ada"))
})

test_that("normalize named params", {
  sql <- "SELECT * FROM users WHERE name = @name AND active = :active"
  out <- sb_normalize(sql, list(name = "Ada", active = TRUE))
  expect_equal(out$sql, "SELECT * FROM users WHERE name = $1 AND active = $2")
  expect_equal(out$params, list("Ada", TRUE))
})

test_that("normalize binary params", {
  sql <- "SELECT ?"
  out <- sb_normalize(sql, list(as.raw(c(0x01, 0x02))))
  expect_equal(out$sql, "SELECT $1")
  expect_equal(out$params, list(as.raw(c(0x01, 0x02))))
})
