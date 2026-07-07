# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

find_metadata_node_by_path <- function(nodes, path) {
  for (node in nodes) {
    if (identical(node$path, path)) return(node)
    nested <- find_metadata_node_by_path(node$children, path)
    if (!is.null(nested)) return(nested)
  }
  NULL
}

test_that("tree rows start at default database and expose top branches", {
  rows <- sb_metadata_build_schema_tree_rows(
    c("sys", "users.alice.dev", "users.bob.dev", "analytics.prod"),
    database = "",
    expand_schema_parents = FALSE
  )

  expect_true(nrow(rows) > 0)
  expect_equal(rows$kind[1], "database")
  expect_equal(rows$path[1], "default")

  top_branches <- rows$path[rows$kind == "schema" & rows$top_level_branch]
  expect_equal(top_branches, c("sys", "users", "analytics"))
})

test_that("parent expansion adds dotted schema ancestors", {
  expanded <- sb_metadata_schema_paths_for_navigation(
    c("users.alice.dev", "users.bob.dev", "users.bob.dev"),
    expand_schema_parents = TRUE
  )

  expect_equal(
    expanded,
    c("users", "users.alice", "users.alice.dev", "users.bob", "users.bob.dev")
  )
})

test_that("parent does not allow duplicate child names", {
  tree <- sb_metadata_build_schema_tree(
    c("users.bob.dev", "users.bob.dev"),
    database = "main",
    expand_schema_parents = FALSE
  )

  bob <- find_metadata_node_by_path(tree$schemas, "users.bob")
  expect_false(is.null(bob))
  expect_equal(length(bob$children), 1)
  expect_equal(bob$children[[1]]$name, "dev")
  expect_equal(bob$children[[1]]$path, "users.bob.dev")
})

test_that("same leaf name under different parents is preserved", {
  rows <- sb_metadata_build_schema_tree_rows(
    c("users.alice.orders", "users.bob.orders"),
    database = "main",
    expand_schema_parents = FALSE
  )

  alice_idx <- which(rows$path == "users.alice.orders")
  bob_idx <- which(rows$path == "users.bob.orders")
  expect_equal(length(alice_idx), 1)
  expect_equal(length(bob_idx), 1)
  expect_equal(rows$name[alice_idx], "orders")
  expect_equal(rows$name[bob_idx], "orders")
  expect_false(identical(rows$parent_path[alice_idx], rows$parent_path[bob_idx]))
})
