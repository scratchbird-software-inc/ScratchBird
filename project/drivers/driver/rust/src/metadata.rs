// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

use std::collections::{HashMap, HashSet};

use serde_json::Value;

pub const SCHEMAS_QUERY: &str =
    "SELECT schema_id, schema_name, owner_id, default_tablespace_id FROM sys.schemas WHERE is_valid = 1 ORDER BY schema_name";

pub const CATALOGS_QUERY: &str =
    "SELECT schema_id AS catalog_id, schema_name AS catalog_name FROM sys.schemas WHERE is_valid = 1 ORDER BY schema_name";

pub const TABLES_QUERY: &str =
    "SELECT table_id, schema_id, table_name, table_type, owner_id FROM sys.tables WHERE is_valid = 1 ORDER BY table_name";

pub const COLUMNS_QUERY: &str =
    "SELECT column_id, table_id, column_name, data_type_id, data_type_name, ordinal_position, is_nullable, default_value, domain_id, collation_id, charset_id, is_identity, is_generated, generation_expression FROM sys.columns WHERE is_valid = 1 ORDER BY table_id, ordinal_position";

pub const INDEXES_QUERY: &str =
    "SELECT index_id, table_id, index_name, index_type, is_unique FROM sys.indexes WHERE is_valid = 1 ORDER BY table_id, index_name";

pub const INDEX_COLUMNS_QUERY: &str =
    "SELECT index_id, column_id, column_name, ordinal_position, is_included FROM sys.index_columns ORDER BY index_id, ordinal_position";

pub const CONSTRAINTS_QUERY: &str =
    "SELECT constraint_id, table_id, constraint_name, constraint_type FROM sys.constraints WHERE is_valid = 1 ORDER BY table_id, constraint_name";

pub const PRIMARY_KEYS_QUERY: &str =
    "SELECT constraint_id, table_id, constraint_name, constraint_type FROM sys.constraints WHERE is_valid = 1 AND lower(constraint_type) IN ('primary key', 'primary') ORDER BY table_id, constraint_name";

pub const FOREIGN_KEYS_QUERY: &str =
    "SELECT constraint_id, table_id, constraint_name, constraint_type FROM sys.constraints WHERE is_valid = 1 AND lower(constraint_type) IN ('foreign key', 'foreign') ORDER BY table_id, constraint_name";

pub const TABLE_PRIVILEGES_QUERY: &str =
    "SELECT table_id, table_name, owner_id AS grantor_id, owner_id AS grantee_id, 'ALL' AS privilege_type FROM sys.tables WHERE is_valid = 1 ORDER BY table_id, table_name";

pub const COLUMN_PRIVILEGES_QUERY: &str =
    "SELECT table_id, column_id, column_name, 'ALL' AS privilege_type FROM sys.columns WHERE is_valid = 1 ORDER BY table_id, ordinal_position";

pub const PROCEDURES_QUERY: &str =
    "SELECT procedure_id, schema_id, procedure_name, routine_type FROM sys.procedures WHERE is_valid = 1 ORDER BY schema_id, procedure_name";

pub const FUNCTIONS_QUERY: &str =
    "SELECT function_id, schema_id, function_name FROM sys.functions WHERE is_valid = 1 ORDER BY schema_id, function_name";

pub const ROUTINES_QUERY: &str =
    "SELECT procedure_id AS routine_id, schema_id, procedure_name AS routine_name, routine_type FROM sys.procedures WHERE is_valid = 1 UNION ALL SELECT function_id AS routine_id, schema_id, function_name AS routine_name, 'FUNCTION' AS routine_type FROM sys.functions WHERE is_valid = 1 ORDER BY schema_id, routine_name";

pub const TYPE_INFO_QUERY: &str =
    "SELECT DISTINCT data_type_id, data_type_name FROM sys.columns WHERE is_valid = 1 ORDER BY data_type_name";

const SCHEMA_FIELD_CANDIDATES: [&str; 6] = [
    "schema_name",
    "TABLE_SCHEM",
    "table_schem",
    "table_schema",
    "TABLE_SCHEMA",
    "schema",
];

pub type MetadataRow = HashMap<String, Value>;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct MetadataSchemaTreeNode {
    pub name: String,
    pub path: String,
    pub terminal: bool,
    pub children: Vec<MetadataSchemaTreeNode>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct MetadataSchemaTree {
    pub database: Option<String>,
    pub schemas: Vec<MetadataSchemaTreeNode>,
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct MetadataSchemaTreeOptions {
    pub expand_parents: bool,
    pub database: Option<String>,
}

pub fn normalize_metadata_collection_name(collection: &str) -> Option<&'static str> {
    let normalized = collection.trim().to_ascii_lowercase();
    let key = if normalized.is_empty() {
        "tables"
    } else {
        normalized.as_str()
    };
    match key {
        "catalog" | "catalogs" => Some("catalogs"),
        "schema" | "schemas" => Some("schemas"),
        "table" | "tables" => Some("tables"),
        "column" | "columns" => Some("columns"),
        "index" | "indexes" => Some("indexes"),
        "indexcolumns" | "index_columns" => Some("index_columns"),
        "constraint" | "constraints" => Some("constraints"),
        "primarykey" | "primarykeys" | "primary_keys" | "pk" => Some("primary_keys"),
        "foreignkey" | "foreignkeys" | "foreign_keys" | "fk" => Some("foreign_keys"),
        "tableprivileges" | "table_privileges" => Some("table_privileges"),
        "columnprivileges" | "column_privileges" => Some("column_privileges"),
        "procedure" | "procedures" => Some("procedures"),
        "function" | "functions" => Some("functions"),
        "routine" | "routines" => Some("routines"),
        "typeinfo" | "type_info" | "types" => Some("type_info"),
        _ => None,
    }
}

pub fn resolve_metadata_collection_query(collection: &str) -> Option<&'static str> {
    let normalized = normalize_metadata_collection_name(collection)?;
    match normalized {
        "catalogs" => Some(CATALOGS_QUERY),
        "schemas" => Some(SCHEMAS_QUERY),
        "tables" => Some(TABLES_QUERY),
        "columns" => Some(COLUMNS_QUERY),
        "indexes" => Some(INDEXES_QUERY),
        "index_columns" => Some(INDEX_COLUMNS_QUERY),
        "constraints" => Some(CONSTRAINTS_QUERY),
        "primary_keys" => Some(PRIMARY_KEYS_QUERY),
        "foreign_keys" => Some(FOREIGN_KEYS_QUERY),
        "table_privileges" => Some(TABLE_PRIVILEGES_QUERY),
        "column_privileges" => Some(COLUMN_PRIVILEGES_QUERY),
        "procedures" => Some(PROCEDURES_QUERY),
        "functions" => Some(FUNCTIONS_QUERY),
        "routines" => Some(ROUTINES_QUERY),
        "type_info" => Some(TYPE_INFO_QUERY),
        _ => None,
    }
}

pub fn expand_schema_paths<I, S>(schema_paths: I) -> Vec<String>
where
    I: IntoIterator<Item = S>,
    S: AsRef<str>,
{
    let mut out = Vec::new();
    let mut seen = HashSet::new();

    for schema_path in schema_paths {
        let segments = split_schema_path(schema_path.as_ref());
        if segments.is_empty() {
            continue;
        }

        let mut current = String::new();
        for segment in segments {
            if !current.is_empty() {
                current.push('.');
            }
            current.push_str(segment);
            if seen.insert(current.clone()) {
                out.push(current.clone());
            }
        }
    }

    out
}

pub fn list_metadata_schema_paths(rows: &[MetadataRow], expand_parents: bool) -> Vec<String> {
    let mut deduped = Vec::new();
    let mut seen = HashSet::new();

    for row in rows {
        let Some(schema_path) = read_schema_path(row) else {
            continue;
        };
        if seen.insert(schema_path.clone()) {
            deduped.push(schema_path);
        }
    }

    if expand_parents {
        return expand_schema_paths(deduped.iter().map(String::as_str));
    }
    deduped
}

pub fn build_metadata_schema_tree(
    rows: &[MetadataRow],
    options: MetadataSchemaTreeOptions,
) -> MetadataSchemaTree {
    let base_paths = list_metadata_schema_paths(rows, false);
    let expanded_paths = if options.expand_parents {
        expand_schema_paths(base_paths.iter().map(String::as_str))
    } else {
        base_paths.clone()
    };
    let terminal_paths: HashSet<String> = if options.expand_parents {
        expanded_paths.iter().cloned().collect()
    } else {
        base_paths.iter().cloned().collect()
    };

    let mut roots: Vec<MetadataSchemaTreeNode> = Vec::new();

    for schema_path in expanded_paths {
        let segments = split_schema_path(&schema_path);
        if segments.is_empty() {
            continue;
        }

        let mut current_path = String::new();
        let mut children = &mut roots;

        for segment in segments {
            if !current_path.is_empty() {
                current_path.push('.');
            }
            current_path.push_str(segment);

            let node = upsert_schema_node(
                children,
                segment,
                &current_path,
                terminal_paths.contains(current_path.as_str()),
            );
            children = &mut node.children;
        }
    }

    let database = options.database.and_then(|value| {
        let trimmed = value.trim();
        if trimmed.is_empty() {
            return None;
        }
        Some(trimmed.to_string())
    });

    MetadataSchemaTree {
        database,
        schemas: roots,
    }
}

pub fn expand_schema_metadata_rows(rows: &[MetadataRow]) -> Vec<MetadataRow> {
    let mut out = Vec::new();
    let mut seen = HashSet::new();

    for row in rows {
        let Some(schema_path) = read_schema_path(row) else {
            out.push(row.clone());
            continue;
        };

        let segments = split_schema_path(&schema_path);
        if segments.is_empty() {
            out.push(row.clone());
            continue;
        }

        let mut current = String::new();
        for (idx, segment) in segments.iter().enumerate() {
            if !current.is_empty() {
                current.push('.');
            }
            current.push_str(segment);
            if !seen.insert(current.clone()) {
                continue;
            }

            if idx + 1 == segments.len() {
                out.push(row.clone());
            } else {
                out.push(create_synthetic_schema_row(row, &current));
            }
        }
    }

    out
}

fn upsert_schema_node<'a>(
    children: &'a mut Vec<MetadataSchemaTreeNode>,
    name: &str,
    path: &str,
    terminal: bool,
) -> &'a mut MetadataSchemaTreeNode {
    if let Some(index) = children.iter().position(|node| node.path == path) {
        if terminal {
            children[index].terminal = true;
        }
        return &mut children[index];
    }

    children.push(MetadataSchemaTreeNode {
        name: name.to_string(),
        path: path.to_string(),
        terminal,
        children: Vec::new(),
    });
    let last = children.len() - 1;
    &mut children[last]
}

fn split_schema_path(value: &str) -> Vec<&str> {
    value
        .split('.')
        .map(str::trim)
        .filter(|segment| !segment.is_empty())
        .collect()
}

fn read_schema_path(row: &MetadataRow) -> Option<String> {
    for candidate in SCHEMA_FIELD_CANDIDATES {
        let Some(value) = metadata_row_value(row, candidate) else {
            continue;
        };
        if let Some(schema_path) = value.as_str().and_then(normalize_schema_path) {
            return Some(schema_path);
        }
    }
    None
}

fn normalize_schema_path(value: &str) -> Option<String> {
    let segments = split_schema_path(value);
    if segments.is_empty() {
        return None;
    }
    Some(segments.join("."))
}

fn create_synthetic_schema_row(sample: &MetadataRow, schema_path: &str) -> MetadataRow {
    let mut synthetic = HashMap::with_capacity(sample.len() + 1);
    for key in sample.keys() {
        synthetic.insert(key.clone(), Value::Null);
    }

    let mut assigned = false;
    for candidate in SCHEMA_FIELD_CANDIDATES {
        let Some(key) = metadata_row_key(&synthetic, candidate) else {
            continue;
        };
        synthetic.insert(key, Value::String(schema_path.to_string()));
        assigned = true;
    }

    if !assigned {
        synthetic.insert(
            "schema_name".to_string(),
            Value::String(schema_path.to_string()),
        );
    }
    synthetic
}

fn metadata_row_value<'a>(row: &'a MetadataRow, key: &str) -> Option<&'a Value> {
    row.get(key).or_else(|| {
        row.iter()
            .find_map(|(candidate, value)| candidate.eq_ignore_ascii_case(key).then_some(value))
    })
}

fn metadata_row_key(row: &MetadataRow, key: &str) -> Option<String> {
    if row.contains_key(key) {
        return Some(key.to_string());
    }
    row.keys()
        .find(|candidate| candidate.eq_ignore_ascii_case(key))
        .cloned()
}
