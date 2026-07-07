# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

test_that("native TLS transport path is active", {
  err <- tryCatch(
    {
      sb_connect("scratchbird://user:pass@127.0.0.1:1/testdb")
      ""
    },
    error = function(e) conditionMessage(e)
  )
  expect_false(grepl("TLS transport is not implemented", err, fixed = TRUE))
  expect_true(grepl("TCP connection|failed|timeout|refused|TLS", err, ignore.case = TRUE))
})
