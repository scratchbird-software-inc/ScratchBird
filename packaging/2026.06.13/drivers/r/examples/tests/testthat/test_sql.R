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

test_that("statement chunker matches cross-driver conformance fixture", {
  skip_if_not_installed("jsonlite")
  # Locate the shared oracle fixture relative to the R driver root. The test
  # may run from tests/testthat (testthat) or the package root (R CMD check).
  candidates <- c(
    "../../../../../tests/conformance/drivers/chunker_conformance/cases.json",
    "../../../../tests/conformance/drivers/chunker_conformance/cases.json",
    file.path(
      Sys.getenv("SCRATCHBIRD_REPO_ROOT", unset = "."),
      "project/tests/conformance/drivers/chunker_conformance/cases.json"
    )
  )
  cases_path <- Filter(file.exists, candidates)
  skip_if(length(cases_path) == 0, "chunker conformance cases.json not found")
  fixture <- jsonlite::fromJSON(cases_path[[1]], simplifyDataFrame = FALSE)
  for (case in fixture$cases) {
    expected <- as.character(unlist(case$expected))
    actual <- as.character(split_top_level_statements(case$input))
    expect_identical(actual, expected, info = case$name)
  }
})
