// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

use serde_json::Value;

use scratchbird::metadata::{
    build_metadata_schema_tree, expand_schema_metadata_rows, list_metadata_schema_paths,
    normalize_metadata_collection_name, resolve_metadata_collection_query, MetadataRow,
    MetadataSchemaTreeNode, MetadataSchemaTreeOptions,
};

#[test]
fn expand_schema_metadata_rows_supports_database_default_branch_style_rows() {
    let rows = vec![
        metadata_row(&[
            ("schema_id", Value::from(11)),
            ("TABLE_SCHEM", Value::from("database.default.users")),
            ("TABLE_CATALOG", Value::from("database")),
        ]),
        metadata_row(&[
            ("schema_id", Value::from(12)),
            ("TABLE_SCHEM", Value::from("database.default.audit")),
            ("TABLE_CATALOG", Value::from("database")),
        ]),
    ];

    let expanded = expand_schema_metadata_rows(&rows);
    assert_eq!(
        collect_schema_values(&expanded, "TABLE_SCHEM"),
        vec![
            "database".to_string(),
            "database.default".to_string(),
            "database.default.users".to_string(),
            "database.default.audit".to_string(),
        ]
    );
    assert_eq!(
        expanded
            .iter()
            .map(|row| row.get("schema_id").and_then(Value::as_i64))
            .collect::<Vec<_>>(),
        vec![None, None, Some(11), Some(12)]
    );
}

#[test]
fn list_metadata_schema_paths_expands_dotted_schema_parents() {
    let rows = vec![
        schema_row("users.alice.dev"),
        schema_row("sys"),
        schema_row("users.bob.dev"),
        schema_row("users.bob.dev"),
        schema_row("users..bob.dev"),
        schema_row(""),
    ];

    let expanded = list_metadata_schema_paths(&rows, true);
    assert_eq!(
        expanded,
        vec![
            "users".to_string(),
            "users.alice".to_string(),
            "users.alice.dev".to_string(),
            "sys".to_string(),
            "users.bob".to_string(),
            "users.bob.dev".to_string(),
        ]
    );
}

#[test]
fn build_metadata_schema_tree_enforces_uniqueness_within_same_parent() {
    let rows = vec![
        schema_row("users.bob.dev"),
        schema_row("users.bob.dev"),
        schema_row("users.bob.prod"),
    ];

    let tree = build_metadata_schema_tree(&rows, MetadataSchemaTreeOptions::default());
    let bob = find_node_by_path(&tree.schemas, "users.bob").expect("missing users.bob");
    assert_eq!(bob.children.len(), 2);
    assert_eq!(
        bob.children
            .iter()
            .map(|node| node.path.clone())
            .collect::<Vec<_>>(),
        vec!["users.bob.dev".to_string(), "users.bob.prod".to_string()]
    );
    assert_eq!(
        bob.children
            .iter()
            .filter(|node| node.name == "dev")
            .count(),
        1
    );
}

#[test]
fn build_metadata_schema_tree_allows_same_leaf_name_under_different_parents() {
    let rows = vec![schema_row("users.alice.dev"), schema_row("users.bob.dev")];
    let tree = build_metadata_schema_tree(
        &rows,
        MetadataSchemaTreeOptions {
            expand_parents: true,
            database: Some("demo".to_string()),
        },
    );

    assert_eq!(tree.database.as_deref(), Some("demo"));
    let alice_dev =
        find_node_by_path(&tree.schemas, "users.alice.dev").expect("missing users.alice.dev");
    let bob_dev = find_node_by_path(&tree.schemas, "users.bob.dev").expect("missing users.bob.dev");
    assert_eq!(alice_dev.name, "dev");
    assert_eq!(bob_dev.name, "dev");
    assert_ne!(alice_dev.path, bob_dev.path);
    assert!(!std::ptr::eq(alice_dev, bob_dev));
}

#[test]
fn metadata_collection_aliases_resolve_consistently() {
    assert_eq!(
        normalize_metadata_collection_name("catalog").unwrap(),
        "catalogs"
    );
    assert_eq!(
        normalize_metadata_collection_name("primary_keys").unwrap(),
        "primary_keys"
    );
    assert_eq!(
        normalize_metadata_collection_name("fk").unwrap(),
        "foreign_keys"
    );
    assert_eq!(
        normalize_metadata_collection_name("typeinfo").unwrap(),
        "type_info"
    );
    assert_eq!(
        normalize_metadata_collection_name("routine").unwrap(),
        "routines"
    );
}

#[test]
fn metadata_collection_query_resolution_covers_extended_families() {
    assert!(resolve_metadata_collection_query("catalogs")
        .unwrap()
        .contains("catalog"));
    assert!(resolve_metadata_collection_query("primary_keys")
        .unwrap()
        .contains("primary"));
    assert!(resolve_metadata_collection_query("foreign_keys")
        .unwrap()
        .contains("foreign"));
    assert!(resolve_metadata_collection_query("routines")
        .unwrap()
        .contains("routine_name"));
    assert!(resolve_metadata_collection_query("type_info")
        .unwrap()
        .contains("data_type_name"));
    assert!(resolve_metadata_collection_query("bad_collection").is_none());
}

fn schema_row(schema: &str) -> MetadataRow {
    metadata_row(&[("schema_name", Value::from(schema))])
}

fn metadata_row(values: &[(&str, Value)]) -> MetadataRow {
    let mut row = MetadataRow::with_capacity(values.len());
    for (key, value) in values {
        row.insert((*key).to_string(), value.clone());
    }
    row
}

fn collect_schema_values(rows: &[MetadataRow], key: &str) -> Vec<String> {
    rows.iter()
        .filter_map(|row| row.get(key).and_then(Value::as_str))
        .map(str::to_string)
        .collect()
}

fn find_node_by_path<'a>(
    nodes: &'a [MetadataSchemaTreeNode],
    path: &str,
) -> Option<&'a MetadataSchemaTreeNode> {
    for node in nodes {
        if node.path == path {
            return Some(node);
        }
        if let Some(child) = find_node_by_path(&node.children, path) {
            return Some(child);
        }
    }
    None
}
