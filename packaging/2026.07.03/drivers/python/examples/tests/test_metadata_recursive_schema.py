# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

from scratchbird.metadata import (
    build_ddl_editor_schema_payload,
    build_schema_tree,
    expand_schema_parent_paths,
    schema_name_matches_pattern,
    schema_paths_for_navigation,
)


def _find_child(node, name: str):
    return next((child for child in node.children if child.name == name), None)


def test_schema_name_matches_pattern_supports_jdbc_wildcards_and_escape():
    assert schema_name_matches_pattern("users.alice.dev", "users.%")
    assert schema_name_matches_pattern("users_alice", r"users\_%")
    assert not schema_name_matches_pattern("users.alice", r"users\_%")


def test_schema_paths_for_navigation_normalizes_and_dedupes_without_parent_expansion():
    schema_names = [" users.alice.dev ", "users.alice.dev", "sys", "", "users..bob"]
    assert schema_paths_for_navigation(schema_names) == ["users.alice.dev", "sys", "users.bob"]


def test_expand_schema_parent_paths_preserves_ancestry_and_uniqueness():
    schema_names = [
        "users.alice.dev",
        "users.alice.prod",
        "users.bob.dev",
        "users.bob.dev",
        "analytics.prod",
    ]
    assert expand_schema_parent_paths(schema_names) == [
        "users",
        "users.alice",
        "users.alice.dev",
        "users.alice.prod",
        "users.bob",
        "users.bob.dev",
        "analytics",
        "analytics.prod",
    ]


def test_schema_paths_for_navigation_with_parent_expansion_preserves_pattern_filtering():
    schema_names = ["users.alice.dev", "users.alice.prod", "users.bob.dev"]
    assert schema_paths_for_navigation(
        schema_names,
        expand_schema_parents=True,
        schema_pattern="users.alice.%",
    ) == ["users", "users.alice", "users.alice.dev", "users.alice.prod"]


def test_build_schema_tree_enforces_per_parent_uniqueness_and_cross_path_identity():
    roots = build_schema_tree(
        [
            "users.alice.dev",
            "users.bob.dev",
            "users.bob.dev",
            "sys",
            "users",
        ]
    )
    assert [root.name for root in roots] == ["users", "sys"]

    users = roots[0]
    sys_root = roots[1]
    assert users.is_terminal is True
    assert sys_root.is_terminal is True

    alice = _find_child(users, "alice")
    bob = _find_child(users, "bob")
    assert alice is not None
    assert bob is not None
    assert len(bob.children) == 1
    assert bob.children[0].name == "dev"

    alice_dev = _find_child(alice, "dev")
    bob_dev = _find_child(bob, "dev")
    assert alice_dev is not None
    assert bob_dev is not None
    assert alice_dev.full_path == "users.alice.dev"
    assert bob_dev.full_path == "users.bob.dev"
    assert alice_dev is not bob_dev


def test_build_ddl_editor_schema_payload_snapshot_with_parent_expansion():
    rows = [
        {"schema_name": "users.alice.dev"},
        {"schema_name": "users.alice.prod"},
        {"schema_name": "sys"},
    ]

    payload = build_ddl_editor_schema_payload(rows, expand_schema_parents=True)

    assert payload == {
        "schemaPattern": None,
        "expandSchemaParents": True,
        "schemaPaths": [
            "users",
            "users.alice",
            "users.alice.dev",
            "users.alice.prod",
            "sys",
        ],
        "schemaTree": [
            {
                "name": "users",
                "fullPath": "users",
                "isTerminal": True,
                "children": [
                    {
                        "name": "alice",
                        "fullPath": "users.alice",
                        "isTerminal": True,
                        "children": [
                            {
                                "name": "dev",
                                "fullPath": "users.alice.dev",
                                "isTerminal": True,
                                "children": [],
                            },
                            {
                                "name": "prod",
                                "fullPath": "users.alice.prod",
                                "isTerminal": True,
                                "children": [],
                            },
                        ],
                    }
                ],
            },
            {
                "name": "sys",
                "fullPath": "sys",
                "isTerminal": True,
                "children": [],
            },
        ],
    }


def test_build_ddl_editor_schema_payload_supports_tuple_rows_and_pattern_filter():
    rows = [
        (1, "users.alice.dev"),
        (2, "sys"),
    ]

    payload = build_ddl_editor_schema_payload(
        rows,
        schema_pattern="users.%",
        expand_schema_parents=False,
        column_names=["schema_id", "schema_name"],
    )

    assert payload["schemaPaths"] == ["users.alice.dev"]
    assert payload["schemaTree"] == [
        {
            "name": "users",
            "fullPath": "users",
            "isTerminal": False,
            "children": [
                {
                    "name": "alice",
                    "fullPath": "users.alice",
                    "isTerminal": False,
                    "children": [
                        {
                            "name": "dev",
                            "fullPath": "users.alice.dev",
                            "isTerminal": True,
                            "children": [],
                        }
                    ],
                }
            ],
        }
    ]
