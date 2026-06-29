# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "src"))

import scratchbird


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def _find_node_by_name(nodes, name: str):
    for node in nodes:
        if node.name == name:
            return node
    return None


def test_database_default_branch_style_rows() -> None:
    rows = scratchbird.build_database_default_metadata_rows(
        [
            {"schema_name": "users.alice.dev"},
            {"schema_name": "users.bob.dev"},
        ],
        database="main",
        expand_schema_parents=True,
    )

    paths = [row["node_path"] for row in rows]
    _require(
        paths == [
            "main",
            "main.default",
            "main.default.users",
            "main.default.users.alice",
            "main.default.users.alice.dev",
            "main.default.users.bob",
            "main.default.users.bob.dev",
        ],
        "database/default branch rows should include default-rooted hierarchy",
    )
    _require(rows[0]["node_type"] == "database", "first row should be database node")
    _require(rows[1]["node_type"] == "schema", "second row should be schema node")
    _require(rows[1]["parent_path"] == "main", "default branch should be parented by database row")
    _require(rows[4]["terminal"] is True, "users.alice.dev should be terminal")
    _require(rows[6]["terminal"] is True, "users.bob.dev should be terminal")


def test_dotted_parent_expansion() -> None:
    paths = scratchbird.schema_paths_for_navigation(
        [
            {"schema_name": "users.alice.dev"},
            {"schema_name": "sys"},
            {"schema_name": "users.bob.dev"},
            {"schema_name": "users.bob.dev"},
        ],
        expand_schema_parents=True,
    )
    _require(
        paths == [
            "users",
            "users.alice",
            "users.alice.dev",
            "sys",
            "users.bob",
            "users.bob.dev",
        ],
        "parent expansion should include dotted ancestors in first-seen order",
    )


def test_tree_uniqueness_within_parent() -> None:
    roots = scratchbird.build_schema_tree(
        [
            "users.alice.dev",
            "users.bob.dev",
            "users.bob.dev",
        ]
    )

    users = _find_node_by_name(roots, "users")
    _require(users is not None, "users root missing")
    bob = _find_node_by_name(users.children, "bob")
    _require(bob is not None, "users.bob node missing")
    _require(len(bob.children) == 1, "duplicate child nodes should be deduplicated within same parent")
    _require(bob.children[0].name == "dev", "expected users.bob.dev leaf")


def test_same_leaf_name_under_different_parents() -> None:
    roots = scratchbird.build_schema_tree(
        [
            "users.alice.dev",
            "users.bob.dev",
        ]
    )

    users = _find_node_by_name(roots, "users")
    _require(users is not None, "users root missing")
    alice = _find_node_by_name(users.children, "alice")
    bob = _find_node_by_name(users.children, "bob")
    _require(alice is not None, "users.alice node missing")
    _require(bob is not None, "users.bob node missing")

    alice_dev = _find_node_by_name(alice.children, "dev")
    bob_dev = _find_node_by_name(bob.children, "dev")
    _require(alice_dev is not None, "users.alice.dev missing")
    _require(bob_dev is not None, "users.bob.dev missing")
    _require(alice_dev is not bob_dev, "leaf nodes under different parents should stay distinct")
    _require(alice_dev.full_path == "users.alice.dev", "alice leaf full_path mismatch")
    _require(bob_dev.full_path == "users.bob.dev", "bob leaf full_path mismatch")


def test_build_ddl_editor_schema_payload_shape() -> None:
    payload = scratchbird.build_ddl_editor_schema_payload(
        [
            {"schema_name": "users.alice.dev"},
            {"schema_name": "users.bob.dev"},
            {"schema_name": "sys"},
        ],
        schema_pattern="users.%",
        expand_schema_parents=True,
    )
    _require(
        set(payload.keys()) == {"schemaPattern", "expandSchemaParents", "schemaPaths", "schemaTree"},
        "ddl payload keys mismatch",
    )
    _require(payload["schemaPattern"] == "users.%", "ddl payload schemaPattern mismatch")
    _require(payload["expandSchemaParents"] is True, "ddl payload expandSchemaParents mismatch")
    _require(
        payload["schemaPaths"] == ["users", "users.alice", "users.alice.dev", "users.bob", "users.bob.dev", "sys"],
        "ddl payload schemaPaths mismatch",
    )
    _require(isinstance(payload["schemaTree"], list), "ddl payload schemaTree should be list")
    users_node = None
    for node in payload["schemaTree"]:
        if node.get("name") == "users":
            users_node = node
            break
    _require(users_node is not None, "ddl payload schemaTree should include users root")
    _require(isinstance(users_node.get("children"), list), "ddl payload users children should be list")


def test_shim_connection_ddl_editor_schema_payload_defaults() -> None:
    cfg = scratchbird.ScratchBirdConfig("scratchbird://user:pass@localhost:3092/testdb?sslmode=require")
    conn = scratchbird.connect(cfg)
    payload = conn.ddl_editor_schema_payload()
    _require(payload["schemaPattern"] == "%", "shim ddl payload should default schemaPattern to %")
    _require(payload["expandSchemaParents"] is False, "shim ddl payload should default to no parent expansion")
    _require(isinstance(payload["schemaPaths"], list), "shim ddl payload schemaPaths should be list")
    _require(len(payload["schemaPaths"]) > 0, "shim ddl payload schemaPaths should not be empty")
    _require("users.alice.dev" in payload["schemaPaths"], "shim ddl payload should include deterministic schema rows")
    conn.close()


def main() -> None:
    test_database_default_branch_style_rows()
    test_dotted_parent_expansion()
    test_tree_uniqueness_within_parent()
    test_same_leaf_name_under_different_parents()
    test_build_ddl_editor_schema_payload_shape()
    test_shim_connection_ddl_editor_schema_payload_defaults()
    print("Mojo metadata recursive schema tests OK")


if __name__ == "__main__":
    main()
