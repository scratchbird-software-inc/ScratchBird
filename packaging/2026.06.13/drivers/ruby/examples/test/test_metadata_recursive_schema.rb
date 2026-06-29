# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

require "test_helper"

class TestMetadataRecursiveSchema < Minitest::Test
  def test_database_default_branch_style_metadata_rows
    rows = Scratchbird::Metadata.build_database_default_metadata_rows(
      [
        { "schema_name" => "users.alice.dev" },
        { "schema_name" => "users.bob.dev" }
      ],
      database: "main",
      expand_schema_parents: true
    )

    assert_equal(
      [
        "main",
        "main.default",
        "main.default.users",
        "main.default.users.alice",
        "main.default.users.alice.dev",
        "main.default.users.bob",
        "main.default.users.bob.dev"
      ],
      rows.map { |row| row["node_path"] }
    )
    assert_equal "database", rows[0]["node_type"]
    assert_equal "schema", rows[1]["node_type"]
    assert_equal "main", rows[1]["parent_path"]
    assert rows[4]["terminal"]
    assert rows[6]["terminal"]
  end

  def test_dotted_schema_parent_expansion
    paths = Scratchbird::Metadata.schema_paths_for_navigation(
      [
        { "schema_name" => "users.alice.dev" },
        { "schema_name" => "sys" },
        { "schema_name" => "users.bob.dev" },
        { "schema_name" => "users.bob.dev" }
      ],
      expand_schema_parents: true
    )

    assert_equal(
      [
        "users",
        "users.alice",
        "users.alice.dev",
        "sys",
        "users.bob",
        "users.bob.dev"
      ],
      paths
    )
  end

  def test_tree_uniqueness_within_parent
    roots = Scratchbird::Metadata.build_schema_tree(
      [
        "users.alice.dev",
        "users.bob.dev",
        "users.bob.dev"
      ]
    )

    users = find_node_by_name(roots, "users")
    bob = find_node_by_name(users.children, "bob")
    refute_nil bob
    assert_equal 1, bob.children.length
    assert_equal "dev", bob.children.first.name
  end

  def test_same_object_name_under_different_parents_is_preserved
    roots = Scratchbird::Metadata.build_schema_tree(
      [
        "users.alice.dev",
        "users.bob.dev"
      ]
    )

    users = find_node_by_name(roots, "users")
    alice = find_node_by_name(users.children, "alice")
    bob = find_node_by_name(users.children, "bob")
    refute_nil alice
    refute_nil bob

    alice_dev = find_node_by_name(alice.children, "dev")
    bob_dev = find_node_by_name(bob.children, "dev")
    refute_nil alice_dev
    refute_nil bob_dev
    refute_same alice_dev, bob_dev
    assert_equal "users.alice.dev", alice_dev.full_path
    assert_equal "users.bob.dev", bob_dev.full_path
  end

  private

  def find_node_by_name(nodes, name)
    nodes.find { |node| node.name == name }
  end
end
