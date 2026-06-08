# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Metadata query helpers and schema-tree shaping utilities."""

from __future__ import annotations

from dataclasses import dataclass, field
import re
from typing import Any, Dict, Iterable, List, Mapping, Optional, Sequence, Set

SCHEMAS_QUERY = (
    "SELECT schema_id, schema_name, owner_id, default_tablespace_id "
    "FROM sys.schemas WHERE is_valid = 1 ORDER BY schema_name"
)
TABLES_QUERY = (
    "SELECT t.table_id, t.schema_id, s.schema_name, t.table_name, t.table_type, t.owner_id "
    "FROM sys.tables t "
    "LEFT JOIN sys.schemas s ON s.schema_id = t.schema_id "
    "WHERE t.is_valid = 1 ORDER BY s.schema_name, t.table_name"
)
COLUMNS_QUERY = (
    "SELECT c.column_id, c.table_id, t.table_name, t.schema_id, s.schema_name, "
    "c.column_name, c.data_type_id, c.data_type_name, c.ordinal_position, "
    "c.is_nullable, c.default_value, c.domain_id, c.collation_id, c.charset_id, "
    "c.is_identity, c.is_generated, c.generation_expression "
    "FROM sys.columns c "
    "LEFT JOIN sys.tables t ON t.table_id = c.table_id "
    "LEFT JOIN sys.schemas s ON s.schema_id = t.schema_id "
    "WHERE c.is_valid = 1 ORDER BY s.schema_name, t.table_name, c.ordinal_position"
)
INDEXES_QUERY = (
    "SELECT i.index_id, i.table_id, t.table_name, t.schema_id, s.schema_name, "
    "i.index_name, i.index_type, i.is_unique "
    "FROM sys.indexes i "
    "LEFT JOIN sys.tables t ON t.table_id = i.table_id "
    "LEFT JOIN sys.schemas s ON s.schema_id = t.schema_id "
    "WHERE i.is_valid = 1 ORDER BY s.schema_name, t.table_name, i.index_name"
)
INDEX_COLUMNS_QUERY = (
    "SELECT ic.index_id, i.index_name, ic.column_id, ic.column_name, ic.ordinal_position, "
    "ic.is_included, i.table_id, t.table_name, t.schema_id, s.schema_name "
    "FROM sys.index_columns ic "
    "LEFT JOIN sys.indexes i ON i.index_id = ic.index_id "
    "LEFT JOIN sys.tables t ON t.table_id = i.table_id "
    "LEFT JOIN sys.schemas s ON s.schema_id = t.schema_id "
    "ORDER BY s.schema_name, t.table_name, i.index_name, ic.ordinal_position"
)
CONSTRAINTS_QUERY = (
    "SELECT * FROM information_schema.table_constraints"
)
PROCEDURES_QUERY = (
    "SELECT * FROM information_schema.routines"
)
FUNCTIONS_QUERY = (
    PROCEDURES_QUERY
)
ROUTINES_QUERY = (
    PROCEDURES_QUERY
)
CATALOGS_QUERY = (
    "SELECT schema_id AS catalog_id, schema_name AS catalog_name "
    "FROM sys.schemas WHERE is_valid = 1 ORDER BY schema_name"
)
PRIMARY_KEYS_QUERY = (
    CONSTRAINTS_QUERY
)
FOREIGN_KEYS_QUERY = (
    CONSTRAINTS_QUERY
)
TABLE_PRIVILEGES_QUERY = (
    "SELECT t.table_id, t.table_name, t.schema_id, s.schema_name, "
    "t.owner_id AS grantor_id, t.owner_id AS grantee_id, "
    "'ALL' AS privilege_type "
    "FROM sys.tables t "
    "LEFT JOIN sys.schemas s ON s.schema_id = t.schema_id "
    "WHERE t.is_valid = 1 ORDER BY s.schema_name, t.table_name"
)
COLUMN_PRIVILEGES_QUERY = (
    "SELECT c.table_id, t.table_name, t.schema_id, s.schema_name, c.column_id, "
    "c.column_name, 'ALL' AS privilege_type "
    "FROM sys.columns c "
    "LEFT JOIN sys.tables t ON t.table_id = c.table_id "
    "LEFT JOIN sys.schemas s ON s.schema_id = t.schema_id "
    "WHERE c.is_valid = 1 ORDER BY s.schema_name, t.table_name, c.ordinal_position"
)
TYPE_INFO_QUERY = (
    "SELECT DISTINCT data_type_id, data_type_name, data_type_name AS type_name "
    "FROM sys.columns WHERE is_valid = 1 ORDER BY data_type_name"
)

DEFAULT_COLLECTION = "tables"

COLLECTION_QUERY_MAP: Dict[str, str] = {
    "schemas": SCHEMAS_QUERY,
    "tables": TABLES_QUERY,
    "columns": COLUMNS_QUERY,
    "indexes": INDEXES_QUERY,
    "index_columns": INDEX_COLUMNS_QUERY,
    "constraints": CONSTRAINTS_QUERY,
    "procedures": PROCEDURES_QUERY,
    "functions": FUNCTIONS_QUERY,
    "routines": ROUTINES_QUERY,
    "catalogs": CATALOGS_QUERY,
    "primary_keys": PRIMARY_KEYS_QUERY,
    "foreign_keys": FOREIGN_KEYS_QUERY,
    "table_privileges": TABLE_PRIVILEGES_QUERY,
    "column_privileges": COLUMN_PRIVILEGES_QUERY,
    "type_info": TYPE_INFO_QUERY,
}

COLLECTION_ALIASES: Dict[str, str] = {
    "schema": "schemas",
    "schemas": "schemas",
    "table": "tables",
    "tables": "tables",
    "column": "columns",
    "columns": "columns",
    "index": "indexes",
    "indexes": "indexes",
    "index_column": "index_columns",
    "index_columns": "index_columns",
    "indexcolumn": "index_columns",
    "indexcolumns": "index_columns",
    "constraint": "constraints",
    "constraints": "constraints",
    "procedure": "procedures",
    "procedures": "procedures",
    "function": "functions",
    "functions": "functions",
    "routine": "routines",
    "routines": "routines",
    "catalog": "catalogs",
    "catalogs": "catalogs",
    "primary_key": "primary_keys",
    "primary_keys": "primary_keys",
    "primarykey": "primary_keys",
    "primarykeys": "primary_keys",
    "foreign_key": "foreign_keys",
    "foreign_keys": "foreign_keys",
    "foreignkey": "foreign_keys",
    "foreignkeys": "foreign_keys",
    "table_privilege": "table_privileges",
    "table_privileges": "table_privileges",
    "tableprivilege": "table_privileges",
    "tableprivileges": "table_privileges",
    "column_privilege": "column_privileges",
    "column_privileges": "column_privileges",
    "columnprivilege": "column_privileges",
    "columnprivileges": "column_privileges",
    "type_info": "type_info",
    "typeinfo": "type_info",
}
RESTRICTION_KEY_ALIASES: Dict[str, List[str]] = {
    "catalog": ["catalog_name", "table_catalog", "table_cat", "catalog"],
    "schema": ["schema_name", "table_schema", "table_schem", "schema"],
    "table": ["table_name", "table", "relname"],
    "column": ["column_name", "column"],
    "index": ["index_name", "index"],
    "constraint": ["constraint_name", "constraint"],
    "procedure": ["procedure_name", "routine_name", "procedure"],
    "function": ["function_name", "routine_name", "function"],
    "routine": ["routine_name", "procedure_name", "function_name", "routine"],
    "type": ["type_name", "data_type_name", "data_type", "udt_name"],
}
COLLECTION_RESTRICTION_KEYS: Dict[str, List[str]] = {
    "schemas": ["catalog", "schema"],
    "tables": ["catalog", "schema", "table", "type"],
    "columns": ["catalog", "schema", "table", "column", "type"],
    "indexes": ["catalog", "schema", "table", "index"],
    "index_columns": ["catalog", "schema", "table", "index", "column"],
    "constraints": ["catalog", "schema", "table", "constraint"],
    "procedures": ["catalog", "schema", "procedure"],
    "functions": ["catalog", "schema", "function"],
    "routines": ["catalog", "schema", "routine"],
    "catalogs": ["catalog"],
    "primary_keys": ["catalog", "schema", "table", "constraint"],
    "foreign_keys": ["catalog", "schema", "table", "constraint"],
    "table_privileges": ["catalog", "schema", "table"],
    "column_privileges": ["catalog", "schema", "table", "column"],
    "type_info": ["type"],
}


@dataclass
class SchemaTreeNode:
    """Metadata-only schema tree node for recursive navigation surfaces."""

    name: str
    full_path: str
    is_terminal: bool = False
    children: List["SchemaTreeNode"] = field(default_factory=list)


@dataclass(frozen=True)
class _RestrictionBinding:
    aliases: List[str]
    expect_null: bool
    expected_text: Optional[str]
    pattern: Optional[re.Pattern[str]]


def schemas_query() -> str:
    return SCHEMAS_QUERY


def tables_query() -> str:
    return TABLES_QUERY


def columns_query() -> str:
    return COLUMNS_QUERY


def indexes_query() -> str:
    return INDEXES_QUERY


def index_columns_query() -> str:
    return INDEX_COLUMNS_QUERY


def constraints_query() -> str:
    return CONSTRAINTS_QUERY


def procedures_query() -> str:
    return PROCEDURES_QUERY


def functions_query() -> str:
    return FUNCTIONS_QUERY


def routines_query() -> str:
    return ROUTINES_QUERY


def catalogs_query() -> str:
    return CATALOGS_QUERY


def primary_keys_query() -> str:
    return PRIMARY_KEYS_QUERY


def foreign_keys_query() -> str:
    return FOREIGN_KEYS_QUERY


def table_privileges_query() -> str:
    return TABLE_PRIVILEGES_QUERY


def column_privileges_query() -> str:
    return COLUMN_PRIVILEGES_QUERY


def type_info_query() -> str:
    return TYPE_INFO_QUERY


def normalize_collection_name(collection_name: Optional[str] = None) -> str:
    raw = DEFAULT_COLLECTION if collection_name is None else str(collection_name)
    normalized = raw.strip().lower().replace("-", "_").replace(" ", "_")
    if not normalized:
        normalized = DEFAULT_COLLECTION
    collapsed = normalized.replace("_", "")
    resolved = COLLECTION_ALIASES.get(normalized) or COLLECTION_ALIASES.get(collapsed)
    if resolved is None:
        raise ValueError(f"metadata collection '{raw}' is not supported")
    return resolved


def resolve_collection_query(collection_name: Optional[str] = None) -> str:
    resolved = normalize_collection_name(collection_name)
    return COLLECTION_QUERY_MAP[resolved]


def normalize_restrictions(restrictions: Optional[Mapping[str, Any]]) -> Dict[str, Any]:
    if restrictions is None:
        return {}
    if hasattr(restrictions, "__len__") and len(restrictions) == 0:
        return {}
    if not isinstance(restrictions, Mapping):
        raise ValueError("metadata restrictions must be provided as a mapping")

    normalized: Dict[str, Any] = {}
    for key, value in restrictions.items():
        normalized_key = _normalize_identifier(key)
        if not normalized_key:
            continue
        normalized[normalized_key] = value
    return normalized


def filter_rows_by_restrictions(
    rows: Iterable[Any],
    restrictions: Optional[Mapping[str, Any]],
    *,
    collection_name: Optional[str] = None,
    column_names: Optional[Sequence[str]] = None,
) -> List[Any]:
    row_list = list(rows)
    normalized_restrictions = normalize_restrictions(restrictions)
    if not normalized_restrictions:
        return row_list

    bindings = _build_restriction_bindings(
        row_list,
        normalized_restrictions,
        collection_name=collection_name,
        column_names=column_names,
    )
    if not bindings:
        return row_list

    return [
        row
        for row in row_list
        if _row_matches_restrictions(row, bindings, column_names=column_names)
    ]


def filter_rows_for_collection_family(
    rows: Iterable[Any],
    collection_name: Optional[str],
    *,
    column_names: Optional[Sequence[str]] = None,
) -> List[Any]:
    row_list = list(rows)
    normalized_collection = normalize_collection_name(collection_name)
    if normalized_collection == "primary_keys":
        return [
            row
            for row in row_list
            if _row_has_expected_text(
                row,
                ("constraint_type", "CONSTRAINT_TYPE"),
                ("primary key", "primary"),
                column_names=column_names,
            )
        ]
    if normalized_collection == "foreign_keys":
        return [
            row
            for row in row_list
            if _row_has_expected_text(
                row,
                ("constraint_type", "CONSTRAINT_TYPE"),
                ("foreign key", "foreign"),
                column_names=column_names,
            )
        ]
    if normalized_collection == "procedures":
        return [
            row
            for row in row_list
            if _row_has_expected_text(
                row,
                ("routine_type", "ROUTINE_TYPE"),
                ("procedure",),
                column_names=column_names,
            )
        ]
    if normalized_collection == "functions":
        return [
            row
            for row in row_list
            if _row_has_expected_text(
                row,
                ("routine_type", "ROUTINE_TYPE"),
                ("function",),
                column_names=column_names,
            )
        ]
    return row_list


def schema_name_matches_pattern(schema_name: Optional[str], schema_pattern: Optional[str]) -> bool:
    """Return True when a schema name matches JDBC-style `%`/`_` wildcard pattern."""
    if not schema_pattern:
        return True
    if schema_name is None:
        return False
    return bool(_pattern_to_regex(schema_pattern).match(schema_name))


def schema_paths_for_navigation(
    schema_names: Iterable[str],
    *,
    expand_schema_parents: bool = False,
    schema_pattern: Optional[str] = None,
) -> List[str]:
    """
    Normalize, de-duplicate, and filter schema paths for metadata navigation.

    When `expand_schema_parents` is True, dotted parent segments are emitted in
    insertion order (for example `users`, `users.alice`, `users.alice.dev`).
    """
    if expand_schema_parents:
        return expand_schema_parent_paths(schema_names, schema_pattern=schema_pattern)

    out: List[str] = []
    seen: Set[str] = set()
    for schema_name in schema_names:
        normalized = _normalize_schema_name(schema_name)
        if normalized is None:
            continue
        if not schema_name_matches_pattern(normalized, schema_pattern):
            continue
        if normalized in seen:
            continue
        seen.add(normalized)
        out.append(normalized)
    return out


def expand_schema_parent_paths(
    schema_names: Iterable[str],
    *,
    schema_pattern: Optional[str] = None,
) -> List[str]:
    """
    Expand dotted schema names to include parent segments.

    Behavior mirrors the JDBC metadata expansion mode:
    `users.alice.dev` -> `users`, `users.alice`, `users.alice.dev`.
    """
    out: List[str] = []
    seen: Set[str] = set()
    for schema_name in schema_names:
        normalized = _normalize_schema_name(schema_name)
        if normalized is None:
            continue
        if not schema_name_matches_pattern(normalized, schema_pattern):
            continue
        parts = _split_schema_path(normalized)
        if not parts:
            continue
        current: List[str] = []
        for part in parts:
            current.append(part)
            candidate = ".".join(current)
            if candidate in seen:
                continue
            seen.add(candidate)
            out.append(candidate)
    return out


def build_schema_tree(schema_paths: Iterable[str]) -> List[SchemaTreeNode]:
    """
    Build a metadata-only recursive schema tree from dotted schema paths.

    - Child name uniqueness is enforced per parent.
    - Same-name nodes in different schema paths are preserved as distinct nodes.
    """
    nodes_by_path: Dict[str, SchemaTreeNode] = {}
    roots: List[SchemaTreeNode] = []

    for schema_path in schema_paths:
        parts = _split_schema_path(schema_path)
        if not parts:
            continue

        parent: Optional[SchemaTreeNode] = None
        current_path: List[str] = []
        for part in parts:
            current_path.append(part)
            full_path = ".".join(current_path)
            node = nodes_by_path.get(full_path)
            if node is None:
                node = SchemaTreeNode(name=part, full_path=full_path)
                nodes_by_path[full_path] = node
                if parent is None:
                    roots.append(node)
                else:
                    parent.children.append(node)
            parent = node

        if parent is not None:
            parent.is_terminal = True

    return roots


def build_ddl_editor_schema_payload(
    schema_rows: Iterable[Any],
    *,
    schema_pattern: Optional[str] = None,
    expand_schema_parents: bool = False,
    column_names: Optional[Sequence[str]] = None,
) -> Dict[str, Any]:
    """
    Build a deterministic metadata payload for DDL-editor schema navigation.

    Input rows can be mapping rows (for example dict-like) or tuple/list rows.
    Tuple/list rows require `column_names` to map schema values.
    """
    schema_names: List[str] = []
    for row in schema_rows:
        schema_name = _schema_name_from_row(row, column_names=column_names)
        if schema_name is None:
            continue
        schema_names.append(schema_name)

    schema_paths = schema_paths_for_navigation(
        schema_names,
        expand_schema_parents=expand_schema_parents,
        schema_pattern=schema_pattern,
    )
    schema_tree = build_schema_tree(schema_paths)
    return {
        "schemaPattern": schema_pattern,
        "expandSchemaParents": bool(expand_schema_parents),
        "schemaPaths": schema_paths,
        "schemaTree": _schema_tree_nodes_to_payload(schema_tree),
    }


def _normalize_schema_name(schema_name: Optional[str]) -> Optional[str]:
    parts = _split_schema_path(schema_name)
    if not parts:
        return None
    return ".".join(parts)


def _split_schema_path(schema_name: Optional[str]) -> List[str]:
    if schema_name is None:
        return []
    parts: List[str] = []
    for part in str(schema_name).split("."):
        normalized = part.strip()
        if normalized:
            parts.append(normalized)
    return parts


def _pattern_to_regex(pattern: str) -> re.Pattern[str]:
    out = ["^"]
    escaped = False
    for ch in pattern:
        if escaped:
            out.append(re.escape(ch))
            escaped = False
            continue
        if ch == "\\":
            escaped = True
            continue
        if ch == "%":
            out.append(".*")
        elif ch == "_":
            out.append(".")
        else:
            out.append(re.escape(ch))
    out.append("$")
    return re.compile("".join(out), re.IGNORECASE)


def _has_unescaped_wildcard(pattern: str) -> bool:
    escaped = False
    for ch in pattern:
        if escaped:
            if ch in {"%", "_"}:
                return True
            escaped = False
            continue
        if ch == "\\":
            escaped = True
            continue
        if ch in {"%", "_"}:
            return True
    return False


def _build_restriction_bindings(
    rows: Sequence[Any],
    normalized_restrictions: Mapping[str, Any],
    *,
    collection_name: Optional[str],
    column_names: Optional[Sequence[str]],
) -> List[_RestrictionBinding]:
    allowed_aliases: Set[str] = set()
    if collection_name:
        normalized_collection = normalize_collection_name(collection_name)
        for restriction_key in COLLECTION_RESTRICTION_KEYS.get(normalized_collection, []):
            for alias in _restriction_aliases_for_key(restriction_key):
                normalized_alias = _normalize_identifier(alias)
                if normalized_alias:
                    allowed_aliases.add(normalized_alias)

    bindings: List[_RestrictionBinding] = []
    for restriction_key, restriction_value in normalized_restrictions.items():
        aliases: Set[str] = set()
        for alias in _restriction_aliases_for_key(restriction_key):
            normalized_alias = _normalize_identifier(alias)
            if normalized_alias:
                aliases.add(normalized_alias)
        aliases.add(restriction_key)

        if allowed_aliases:
            aliases = {alias for alias in aliases if alias in allowed_aliases or alias == restriction_key}
        if not aliases:
            continue

        raw_text = str(restriction_value).strip()
        expected_text = _normalize_match_text(restriction_value)
        expect_null = expected_text == "null"
        pattern = None
        if not expect_null and _has_unescaped_wildcard(raw_text):
            pattern = _pattern_to_regex(raw_text)
        binding = _RestrictionBinding(
            aliases=sorted(aliases),
            expect_null=expect_null,
            expected_text=None if expect_null else expected_text,
            pattern=pattern,
        )
        if _rows_have_binding_aliases(rows, binding, column_names=column_names):
            bindings.append(binding)

    return bindings


def _rows_have_binding_aliases(
    rows: Sequence[Any],
    binding: _RestrictionBinding,
    *,
    column_names: Optional[Sequence[str]],
) -> bool:
    return any(
        _row_has_any_alias(row, binding.aliases, column_names=column_names)
        for row in rows
    )


def _row_matches_restrictions(
    row: Any,
    bindings: Sequence[_RestrictionBinding],
    *,
    column_names: Optional[Sequence[str]],
) -> bool:
    for binding in bindings:
        values = _row_values_for_aliases(row, binding.aliases, column_names=column_names)
        if not values:
            return False
        if binding.expect_null:
            if not any(value is None for value in values):
                return False
            continue
        if binding.pattern is not None:
            if not any(value is not None and binding.pattern.match(str(value).strip()) for value in values):
                return False
            continue
        if not any(value is not None and _normalize_match_text(value) == binding.expected_text for value in values):
            return False
    return True


def _row_has_any_alias(row: Any, aliases: Sequence[str], *, column_names: Optional[Sequence[str]]) -> bool:
    if isinstance(row, Mapping):
        return any(_mapping_row_value(row, alias)[0] for alias in aliases)

    if isinstance(row, (tuple, list)) and column_names:
        normalized_aliases = {_normalize_identifier(alias) for alias in aliases}
        return any(_normalize_identifier(name) in normalized_aliases for name in column_names if name is not None)

    return False


def _row_values_for_aliases(
    row: Any,
    aliases: Sequence[str],
    *,
    column_names: Optional[Sequence[str]],
) -> List[Any]:
    if isinstance(row, Mapping):
        values: List[Any] = []
        for alias in aliases:
            present, value = _mapping_row_value(row, alias)
            if present:
                values.append(value)
        return values

    if isinstance(row, (tuple, list)) and column_names:
        values = []
        alias_set = {_normalize_identifier(alias) for alias in aliases}
        for idx, column_name in enumerate(column_names):
            if idx >= len(row):
                break
            if _normalize_identifier(column_name) in alias_set:
                values.append(row[idx])
        return values

    return []


def _mapping_row_value(row: Mapping[Any, Any], alias: str) -> tuple[bool, Any]:
    if alias in row:
        return True, row[alias]

    target = _normalize_identifier(alias)
    for candidate_key, candidate_value in row.items():
        if _normalize_identifier(candidate_key) == target:
            return True, candidate_value
    return False, None


def _restriction_aliases_for_key(key: str) -> List[str]:
    canonical = _normalize_identifier(key)
    aliases = list(RESTRICTION_KEY_ALIASES.get(canonical, []))
    if canonical:
        aliases.append(canonical)
    return aliases


def _normalize_identifier(value: Any) -> str:
    return re.sub(r"[^a-z0-9]", "", str(value).strip().lower())


def _normalize_match_text(value: Any) -> str:
    return str(value).strip().lower()


def _schema_name_from_row(row: Any, *, column_names: Optional[Sequence[str]]) -> Optional[str]:
    aliases = [
        "schema_name",
        "table_schema",
        "table_schem",
        "schema",
        "constraint_schema",
        "routine_schema",
    ]
    values = _row_values_for_aliases(row, aliases, column_names=column_names)
    for value in values:
        normalized = _normalize_schema_name(value)
        if normalized is not None:
            return normalized
    return None


def _row_has_expected_text(
    row: Any,
    aliases: Sequence[str],
    expected_values: Sequence[str],
    *,
    column_names: Optional[Sequence[str]],
) -> bool:
    values = _row_values_for_aliases(row, aliases, column_names=column_names)
    normalized_expected = {_normalize_match_text(value) for value in expected_values}
    return any(value is not None and _normalize_match_text(value) in normalized_expected for value in values)


def _schema_tree_nodes_to_payload(nodes: Sequence[SchemaTreeNode]) -> List[Dict[str, Any]]:
    payload_nodes: List[Dict[str, Any]] = []
    for node in nodes:
        payload_nodes.append(
            {
                "name": node.name,
                "fullPath": node.full_path,
                "isTerminal": node.is_terminal,
                "children": _schema_tree_nodes_to_payload(node.children),
            }
        )
    return payload_nodes
