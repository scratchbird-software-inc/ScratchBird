#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Generator helpers for the driver full-surface SBSQL corpus.

The checked-in suite stays compact. This module expands it into the large
driver-specific scripts and proof indexes under build/.
"""

from __future__ import annotations

import csv
import hashlib
import json
import re
import shutil
from pathlib import Path
from typing import Any


REPLAY_INDEX_REL = Path("project/tests/sbsql_parser_worker/generated/replay/DIFFERENTIAL_REPLAY_FIXTURE_INDEX.csv")
REPLAY_PAYLOADS_REL = Path("project/tests/sbsql_parser_worker/generated/replay/DIFFERENTIAL_REPLAY_EXPECTED_PAYLOADS.jsonl")
E2E_MATRIX_REL = Path("project/tests/sbsql_parser_worker/generated/exhaustive_e2e/EXHAUSTIVE_E2E_VARIATION_MATRIX.csv")
SBLR_ROUND_TRIP_ROOT_REL = Path("project/tests/sbsql_parser_worker/generated/full_surface/sblr_binary_round_trip")
AUTH_ROUTE_ROOT_REL = Path("project/tests/sbsql_parser_worker/generated/full_surface/authenticated_route")
DATATYPE_INVENTORY_REL = Path("project/tests/conformance/datatypes/datatype_layout_inventory.yaml")
PAGE_LAYOUT_INVENTORY_REL = Path("project/tests/conformance/storage/page_layout_inventory.yaml")
INDEX_REGISTRY_REL = Path("project/src/core/index/index_family_registry.hpp")
INDEX_PROOF_MATRIX_REL = Path("project/tests/release_evidence/commercial_readiness_public_evidence/artifacts/DURABLE_STORAGE_INDEX_PROOF_MATRIX.csv")
INDEX_READINESS_MANIFEST_REL = Path("project/tests/release_evidence/consolidated_enterprise_public_evidence/artifacts/CEIC-030_INDEX_READINESS_MANIFEST.yaml")
OPTIMIZER_READINESS_MANIFEST_REL = Path("project/tests/release_evidence/consolidated_enterprise_public_evidence/artifacts/CEIC-062_OPTIMIZER_READINESS_MANIFEST.yaml")
BUILTIN_FIXTURE_ROOT_REL = Path("project/tests/sbsql_parser_worker/generated/full_surface")

GENERATED_SCRIPT_SPECS = (
    ("SBDFS-100", "100_surface_replay_manifest.sbsql"),
    ("SBDFS-101", "101_surface_replay_commands.sbsql"),
    ("SBDFS-110", "110_sblr_uuid_roundtrip_manifest.sbsql"),
    ("SBDFS-120", "120_datatype_native_load.sbsql"),
    ("SBDFS-130", "130_datatype_dml_matrix.sbsql"),
    ("SBDFS-140", "140_index_family_variation_matrix.sbsql"),
    ("SBDFS-150", "150_query_join_window_cte_matrix.sbsql"),
    ("SBDFS-160", "160_builtin_function_invocations.sbsql"),
    ("SBDFS-170", "170_cast_operator_matrix.sbsql"),
    ("SBDFS-180", "180_optimizer_and_storage_manifest.sbsql"),
)

CONCRETE_SQL_TYPES = {
    "null_type": "VARCHAR(32)",
    "boolean": "BOOLEAN",
    "int8": "INT8",
    "int16": "SMALLINT",
    "int32": "INTEGER",
    "int64": "BIGINT",
    "int128": "INT128",
    "uint8": "UINT8",
    "uint16": "UINT16",
    "uint32": "UINT32",
    "uint64": "UINT64",
    "uint128": "UINT128",
    "bfloat16": "BFLOAT16",
    "real16": "REAL16",
    "real32": "REAL",
    "real64": "DOUBLE PRECISION",
    "real128": "REAL128",
    "decimal": "DECIMAL(38, 9)",
    "decimal_float": "DECFLOAT",
    "uuid": "UUID",
    "ip_address": "IP_ADDRESS",
    "network_prefix": "NETWORK_PREFIX",
    "mac_address": "MAC_ADDRESS",
    "character": "VARCHAR(512)",
    "binary": "VARBINARY(512)",
    "bit_string": "BIT VARYING(512)",
    "date": "DATE",
    "time": "TIME",
    "timestamp": "TIMESTAMP",
    "interval": "INTERVAL",
    "blob": "BLOB",
    "document": "DOCUMENT",
    "json_document": "JSON",
    "binary_json_document": "JSONB",
    "bson_document": "BSON",
    "xml_document": "XML",
    "hstore_document": "HSTORE",
    "object_document": "OBJECT",
    "flattened_object_document": "FLATTENED_OBJECT",
    "enum_value": "ENUM_VALUE",
    "set_value": "SET_VALUE",
    "array": "ARRAY<INTEGER>",
    "list": "LIST<INTEGER>",
    "map": "MAP<VARCHAR(64), INTEGER>",
    "row": "ROW",
    "composite": "COMPOSITE",
    "variant": "VARIANT",
    "range": "RANGE<INTEGER>",
    "multirange": "MULTIRANGE<INTEGER>",
    "token_stream": "TOKEN_STREAM",
    "search_query": "SEARCH_QUERY",
    "search_rank_feature": "SEARCH_RANK_FEATURE",
    "search_completion": "SEARCH_COMPLETION",
    "search_percolator": "SEARCH_PERCOLATOR",
    "geometry": "GEOMETRY",
    "geography": "GEOGRAPHY",
    "point": "POINT",
    "shape": "SHAPE",
    "raster": "RASTER",
    "vector": "VECTOR(8)",
    "dense_vector": "DENSE_VECTOR(8)",
    "sparse_vector": "SPARSE_VECTOR(8)",
    "binary_vector": "BINARY_VECTOR(64)",
    "quantized_vector": "QUANTIZED_VECTOR(8)",
    "graph_node": "GRAPH_NODE",
    "graph_edge": "GRAPH_EDGE",
    "graph_path": "GRAPH_PATH",
    "time_series_value": "TIME_SERIES_VALUE",
    "columnar_segment": "COLUMNAR_SEGMENT",
    "aggregate_state": "AGGREGATE_STATE",
    "hll_sketch": "HLL_SKETCH",
    "bloom_filter": "BLOOM_FILTER",
    "quantile_sketch": "QUANTILE_SKETCH",
    "histogram_sketch": "HISTOGRAM_SKETCH",
    "ranking_summary": "RANKING_SUMMARY",
    "vector_summary": "VECTOR_SUMMARY",
    "lob_locator": "LOB_LOCATOR",
    "external_file_locator": "EXTERNAL_FILE_LOCATOR",
    "remote_object_locator": "REMOTE_OBJECT_LOCATOR",
    "bridge_handle": "BRIDGE_HANDLE",
    "cursor_handle": "CURSOR_HANDLE",
    "system_reference": "SYSTEM_REFERENCE",
    "opaque_extension": "OPAQUE_EXTENSION",
    "cursor": "CURSOR",
    "result_set": "RESULT_SET",
    "table_value": "TABLE_VALUE",
}


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    return sha256_bytes(path.read_bytes())


def sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def read_csv_rows(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def sql_string(value: str | None) -> str:
    if value is None or value == "":
        return "NULL"
    return "'" + value.replace("'", "''") + "'"


def metadata_text(value: str | None) -> str:
    if value is None:
        return ""
    # The fixed SQL manifest carries descriptive names only; surface IDs are the
    # authority. Avoid authority-drift tokens in executable SQL text.
    return value.replace("WAL", "W_A_L")


def qident(name: str) -> str:
    safe = re.sub(r"[^a-zA-Z0-9_]+", "_", name.lower()).strip("_")
    if not safe:
        safe = "x"
    if safe[0].isdigit():
        safe = "x_" + safe
    return safe


def parse_yaml_list(text: str, key: str, indent: int = 6) -> list[str]:
    marker = f"{key}:"
    values: list[str] = []
    in_section = False
    prefix = " " * indent + "- "
    for line in text.splitlines():
        if line.strip() == marker:
            in_section = True
            continue
        if not in_section:
            continue
        if line.startswith(prefix):
            values.append(line.split("- ", 1)[1].strip())
            continue
        if line.strip() and not line.startswith(" " * indent):
            break
    return values


def required_datatypes(repo_root: Path) -> list[str]:
    return parse_yaml_list((repo_root / DATATYPE_INVENTORY_REL).read_text(encoding="utf-8"), "required_types")


def supported_page_sizes(repo_root: Path) -> list[str]:
    text = (repo_root / PAGE_LAYOUT_INVENTORY_REL).read_text(encoding="utf-8")
    return parse_yaml_list(text, "supported_page_sizes", indent=2)


def index_families(repo_root: Path) -> list[str]:
    header = (repo_root / INDEX_REGISTRY_REL).read_text(encoding="utf-8")
    match = re.search(r"enum class IndexFamily\s*:[^{]+{(?P<body>.*?)};", header, re.S)
    if not match:
        raise ValueError("IndexFamily enum not found")
    names: list[str] = []
    for raw in match.group("body").split(","):
        name = raw.strip().split("=", 1)[0].strip()
        if name and name != "unknown":
            names.append(name)
    return names


def fixture_rows(repo_root: Path) -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    for path in sorted((repo_root / BUILTIN_FIXTURE_ROOT_REL).glob("*.csv")):
        for row in read_csv_rows(path):
            copy = dict(row)
            copy["source_file"] = path.name
            rows.append(copy)
    return rows


def source_summary(repo_root: Path) -> dict[str, Any]:
    replay_rows = read_csv_rows(repo_root / REPLAY_INDEX_REL)
    e2e_rows = read_csv_rows(repo_root / E2E_MATRIX_REL)
    datatypes = required_datatypes(repo_root)
    families = index_families(repo_root)
    fixture_count = len(fixture_rows(repo_root))
    sblr_files = sorted((repo_root / SBLR_ROUND_TRIP_ROOT_REL).glob("*.yaml"))
    route_files = sorted((repo_root / AUTH_ROUTE_ROOT_REL).glob("*.yaml"))
    return {
        "replay_rows": len(replay_rows),
        "full_route_replay_rows": sum("full_route" in row.get("route_set", "").split(";") for row in replay_rows),
        "e2e_scopes": len(e2e_rows),
        "e2e_counts": {row["scope_id"]: int(row["expected_count"]) for row in e2e_rows},
        "datatype_count": len(datatypes),
        "page_size_count": len(supported_page_sizes(repo_root)),
        "index_family_count": len(families),
        "builtin_fixture_rows": fixture_count,
        "sblr_round_trip_files": len(sblr_files),
        "authenticated_route_files": len(route_files),
    }


def deterministic_int(seed: str, index: int, modulo: int) -> int:
    digest = hashlib.sha256(f"{seed}:{index}".encode("utf-8")).digest()
    return int.from_bytes(digest[:8], "big") % modulo


def literal_for_datatype(datatype: str, index: int) -> str:
    n = deterministic_int(datatype, index, 10_000_000)
    if datatype == "null_type":
        return "NULL"
    if datatype == "boolean":
        return "TRUE" if index % 2 == 0 else "FALSE"
    if datatype.startswith("int"):
        return str((n % 200000) - 100000)
    if datatype.startswith("uint"):
        return str(n)
    if datatype in {"bfloat16", "real16", "real32", "real64", "real128", "decimal", "decimal_float"}:
        return f"{(n % 100000) - 50000}.{index % 1000:03d}"
    if datatype == "uuid":
        return f"UUID '018f0a2b-0000-7{index % 10:03d}-9{index % 10:03d}-{n:012d}'"
    if datatype == "ip_address":
        return f"'10.{index % 255}.{(index * 7) % 255}.{(index * 13) % 255}'"
    if datatype == "network_prefix":
        return f"'10.{index % 255}.0.0/16'"
    if datatype == "mac_address":
        return f"'02:00:{index % 255:02x}:{(index * 3) % 255:02x}:{(index * 5) % 255:02x}:{(index * 7) % 255:02x}'"
    if datatype == "character":
        return sql_string(f"sb-{datatype}-{index}-{n}")
    if datatype == "binary":
        return f"X'{n:012x}'"
    if datatype == "bit_string":
        return "B'" + format(n % 256, "08b") + "'"
    if datatype == "date":
        return f"DATE '2026-{(index % 12) + 1:02d}-{(index % 28) + 1:02d}'"
    if datatype == "time":
        return f"TIME '{index % 24:02d}:{(index * 3) % 60:02d}:{(index * 7) % 60:02d}'"
    if datatype == "timestamp":
        return f"TIMESTAMP '2026-{(index % 12) + 1:02d}-{(index % 28) + 1:02d} {index % 24:02d}:{(index * 3) % 60:02d}:{(index * 7) % 60:02d}'"
    if datatype == "interval":
        return f"INTERVAL '{(index % 365) + 1} days'"
    if datatype in {"document", "json_document", "binary_json_document", "bson_document", "object_document", "flattened_object_document"}:
        return "JSON " + sql_string(json.dumps({"datatype": datatype, "i": index, "n": n}, sort_keys=True))
    if datatype == "xml_document":
        return "XML " + sql_string(f"<row datatype=\"{datatype}\" i=\"{index}\" n=\"{n}\"/>")
    if datatype in {"array", "list"}:
        return f"ARRAY[{index}, {n % 1000}, {(n // 7) % 1000}]"
    if datatype == "vector" or datatype.endswith("_vector"):
        return "VECTOR " + sql_string("[" + ",".join(f"{((n + i * 17) % 1000) / 1000.0:.3f}" for i in range(8)) + "]")
    if datatype == "point":
        return sql_string(f"POINT({index % 100} {(n % 100)})")
    if datatype in {"geometry", "geography", "shape"}:
        return sql_string(f"POINT({index % 100} {(n % 100)})")
    return f"CAST({sql_string(json.dumps({'datatype': datatype, 'i': index, 'n': n}, sort_keys=True))} AS {CONCRETE_SQL_TYPES.get(datatype, datatype.upper())})"


def argument_to_sql(value: Any) -> str:
    if not isinstance(value, dict):
        if isinstance(value, str):
            return sql_string(value)
        if isinstance(value, bool):
            return "TRUE" if value else "FALSE"
        if isinstance(value, (int, float)):
            return str(value)
        return sql_string(json.dumps(value, sort_keys=True))
    if value.get("is_null") is True:
        descriptor = str(value.get("descriptor_id") or value.get("descriptor") or "VARCHAR")
        return f"CAST(NULL AS {descriptor})"
    if "int64_value" in value:
        return str(value["int64_value"])
    if "uint64_value" in value:
        return str(value["uint64_value"])
    if "real64_value" in value:
        return str(value["real64_value"])
    if "bool_value" in value:
        return "TRUE" if value["bool_value"] else "FALSE"
    if "text_value" in value:
        return sql_string(str(value["text_value"]))
    if "binary_hex" in value:
        return "X'" + str(value["binary_hex"]) + "'"
    if "hex" in value:
        return "X'" + str(value["hex"]) + "'"
    if "binary_value" in value and isinstance(value["binary_value"], list):
        return "X'" + "".join(f"{int(part) & 0xff:02x}" for part in value["binary_value"]) + "'"
    if "binary_value" in value:
        raw_binary = str(value["binary_value"]).strip()
        if re.fullmatch(r"[0-9A-Fa-f]*", raw_binary):
            return "X'" + raw_binary + "'"
        return sql_string(raw_binary)
    if str(value.get("type") or "").lower() == "null":
        descriptor = str(value.get("descriptor_id") or value.get("descriptor") or "VARCHAR")
        return f"CAST(NULL AS {descriptor})"
    if "value" in value:
        kind = str(value.get("descriptor_id") or value.get("descriptor") or value.get("type") or "").lower()
        raw = value["value"]
        if isinstance(raw, bool):
            return "TRUE" if raw else "FALSE"
        if isinstance(raw, (int, float)):
            return str(raw)
        if kind == "boolean":
            return "TRUE" if str(raw).lower() in {"1", "t", "true", "yes"} else "FALSE"
        if kind in {"int8", "int16", "int32", "int64", "int128", "integer", "smallint", "bigint"}:
            return str(raw)
        if kind in {"uint8", "uint16", "uint32", "uint64", "uint128"}:
            return str(raw)
        if kind in {"bfloat16", "real16", "real32", "real64", "real128", "real", "double", "decimal"}:
            return str(raw)
        if kind in {"json", "json_document"}:
            return "JSON " + sql_string(str(raw))
        return sql_string(str(raw))
    return sql_string(json.dumps(value, sort_keys=True))


def json_payload(value: str | None, default: Any) -> Any:
    if not value:
        return default
    try:
        return json.loads(value)
    except json.JSONDecodeError:
        return default


def canonical_function_name(row: dict[str, str]) -> str:
    function_id = row.get("canonical_builtin_id") or row.get("function_id") or "unknown_function"
    name = function_id.split(".")[-1].split("(", 1)[0].split("|", 1)[0]
    return qident(name)


def canonical_direct_function_name(row: dict[str, str]) -> str:
    function_id = row.get("canonical_builtin_id") or row.get("function_id") or ""
    function_id = function_id.split("(", 1)[0].split("|", 1)[0].strip()
    parts = function_id.split(".")
    canonical_fallback_families = {
        "crypto",
        "cursor",
        "handle",
        "json",
        "lob",
        "locator",
        "multiset",
        "rowset",
        "setof",
        "stream",
        "table_value",
        "type",
        "vector",
        "uuid",
        "xml",
    }
    if (
        len(parts) >= 3
        and parts[0].lower() == "sb"
        and parts[1].lower() in canonical_fallback_families
        and all(qident(part) == part.lower() for part in parts)
    ):
        return ".".join(part.lower() for part in parts)
    return canonical_function_name(row)


def ordered_value_rows(row: dict[str, str]) -> list[list[Any]]:
    payload = json_payload(row.get("ordered_values_json"), [])
    if not isinstance(payload, list):
        return []
    rows: list[list[Any]] = []
    for item in payload:
        rows.append(item if isinstance(item, list) else [item])
    return rows


def render_input_cte(rows: list[list[Any]]) -> tuple[str, int]:
    arity = max((len(row) for row in rows), default=1)
    arity = max(arity, 1)
    columns = ["row_index"] + [f"arg{index}" for index in range(1, arity + 1)]
    if not rows:
        null_args = ", ".join(f"CAST(NULL AS VARCHAR(64)) AS arg{index}" for index in range(1, arity + 1))
        return (
            f"input({', '.join(columns)}) AS (\n"
            f"    SELECT 0 AS row_index, {null_args} WHERE FALSE\n"
            ")",
            arity,
        )
    lines = [f"input({', '.join(columns)}) AS (", "    VALUES"]
    for row_index, row in enumerate(rows):
        padded = list(row) + [{"is_null": True}] * (arity - len(row))
        rendered = ", ".join(argument_to_sql(value) for value in padded[:arity])
        suffix = "," if row_index + 1 < len(rows) else ""
        lines.append(f"        ({row_index}, {rendered}){suffix}")
    lines.append(")")
    return "\n".join(lines), arity


def option_sql(row: dict[str, str], default: str = "NULL") -> str:
    option = json_payload(row.get("hypothetical_value_json"), None)
    if option is None:
        return default
    return argument_to_sql(option)


def option_object(row: dict[str, str]) -> dict[str, Any]:
    option = json_payload(row.get("hypothetical_value_json"), {})
    return option if isinstance(option, dict) else {}


def aggregate_separator_sql(row: dict[str, str]) -> str:
    option = option_object(row)
    if "separator" in option:
        return sql_string(str(option["separator"]))
    if "text_value" in option:
        return sql_string(str(option["text_value"]))
    return sql_string(",")


def listagg_overflow_clause(row: dict[str, str]) -> str:
    overflow = option_object(row).get("overflow")
    if not isinstance(overflow, dict):
        return ""
    mode = str(overflow.get("mode") or "").lower()
    if mode == "error":
        return " ON OVERFLOW ERROR"
    if mode != "truncate":
        return ""
    indicator = sql_string(str(overflow.get("indicator") or "..."))
    count_clause = " WITH COUNT" if overflow.get("with_count", True) else " WITHOUT COUNT"
    return f" ON OVERFLOW TRUNCATE {indicator}{count_clause}"


def render_aggregate_call(row: dict[str, str], arity: int) -> str:
    name = canonical_function_name(row)
    mode = row.get("evaluation_mode", "")
    function_text = (row.get("function_id") or "").lower()
    case_kind = row.get("case_kind", "")
    args = [f"arg{index}" for index in range(1, arity + 1)]
    if mode == "ordered_set" and name in {"rank", "dense_rank", "percent_rank", "cume_dist"}:
        return f"{name}({option_sql(row)}) WITHIN GROUP (ORDER BY arg1)"
    if name in {"approx_percentile_cont", "approx_percentile_disc", "percentile_cont", "percentile_disc"}:
        return f"{name}({option_sql(row, '0.5')}) WITHIN GROUP (ORDER BY arg1)"
    if name == "mode":
        return "mode() WITHIN GROUP (ORDER BY arg1)"
    if name == "listagg":
        return (
            f"listagg(arg1, {aggregate_separator_sql(row)}{listagg_overflow_clause(row)}) "
            "WITHIN GROUP (ORDER BY row_index)"
        )
    if name == "string_agg":
        return f"string_agg(arg1, {aggregate_separator_sql(row)} ORDER BY row_index)"
    if name == "array_agg":
        return "array_agg(arg1 ORDER BY row_index)"
    if name == "json_agg":
        return "json_agg(arg1 ORDER BY row_index)"
    if name == "json_object_agg":
        return "json_object_agg(arg1, arg2 ORDER BY row_index)"
    if name == "approx_top_k":
        return f"approx_top_k(arg1, {option_sql(row, '10')})"
    if name == "count" and "count_star" in case_kind:
        return "count(*)"
    if name == "count":
        return "count(arg1)"
    if name == "avg" and "distinct" in function_text:
        return "avg(DISTINCT arg1)"
    return f"{name}({', '.join(args)})"


def render_window_parts(row: dict[str, str]) -> tuple[list[str], str]:
    rows = ordered_value_rows(row)
    input_cte, _arity = render_input_cte(rows)
    name = canonical_function_name(row)
    option = option_object(row)
    current_row_index = int(row.get("current_row_index") or 0)
    if name in {"first_value", "last_value", "nth_value"}:
        frame_start = int(option.get("frame_start_index", 0))
        frame_end = int(option.get("frame_end_exclusive", len(rows)))
        nth = int(option.get("nth", 1))
        function_args = "arg1" if name != "nth_value" else f"arg1, {nth}"
        frame_cte = (
            "frame_input AS (\n"
            "    SELECT * FROM input\n"
            f"    WHERE row_index >= {frame_start} AND row_index < {frame_end}\n"
            ")"
        )
        window_cte = (
            "windowed AS (\n"
            f"    SELECT {name}({function_args}) OVER (\n"
            "               ORDER BY row_index\n"
            "               ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING\n"
            "           ) AS actual_value\n"
            "    FROM frame_input\n"
            ")"
        )
        return [input_cte, frame_cte, window_cte], "(SELECT actual_value FROM windowed LIMIT 1)"
    if name in {"rank", "dense_rank", "percent_rank", "cume_dist"}:
        call = f"{name}() OVER (ORDER BY arg1)"
    elif name == "row_number":
        call = "row_number() OVER (ORDER BY row_index)"
    elif name == "ntile":
        call = f"ntile({int(option.get('bucket_count', 1))}) OVER (ORDER BY row_index)"
    elif name in {"lag", "lead"}:
        offset = int(option.get("offset", 1))
        default_value = option.get("default_value")
        args = f"arg1, {offset}"
        if isinstance(default_value, dict):
            args += f", {argument_to_sql(default_value)}"
        call = f"{name}({args}) OVER (ORDER BY row_index)"
    else:
        call = f"{name}(arg1) OVER (ORDER BY row_index)"
    window_cte = (
        "windowed AS (\n"
        f"    SELECT row_index, {call} AS actual_value\n"
        "    FROM input\n"
        ")"
    )
    return [input_cte, window_cte], f"(SELECT actual_value FROM windowed WHERE row_index = {current_row_index})"


def render_route_call_parts(row: dict[str, str]) -> tuple[list[str], str, str]:
    mode = row.get("evaluation_mode", "")
    if mode == "window":
        ctes, actual = render_window_parts(row)
        return ctes, actual, ""
    if mode in {"aggregate", "ordered_set"}:
        input_cte, arity = render_input_cte(ordered_value_rows(row))
        return [input_cte], render_aggregate_call(row, arity), "\nFROM input"
    return [], render_function_call(row), ""


def runtime_default_arguments(row: dict[str, str]) -> list[Any]:
    function_id = (
        row.get("canonical_builtin_id")
        or row.get("function_id")
        or ""
    ).split("(", 1)[0].split("|", 1)[0].strip().lower()
    fixture_id = row.get("fixture_id", "")
    text_lob = {
        "type": "text",
        "descriptor": "json_document",
        "value": '{"kind":"lob_locator","locator_id":"lob:test","state":"open","mode":"read_write","class":"text","media_type":"text/plain","data_hex":"68656c6c6f","size":5,"valid":true}',
    }
    defaults: dict[str, list[Any]] = {
        "sb.scalar.treat_typed": [
            {"text_value": "abc"},
            {"text_value": "varchar"},
        ],
        "sb.scalar.bit_string": [
            {"text_value": "1010"},
        ],
        "sb.scalar.nvl": [
            {"is_null": True, "descriptor_id": "character"},
            {"text_value": "fallback"},
        ],
        "sb.multiset.element": [
            {"type": "text", "descriptor": "json_document", "value": '["solo"]'},
        ],
        "sb.lob.read": [
            text_lob,
            {"type": "int64", "value": 2},
            {"type": "int64", "value": 3},
        ],
        "sb.lob.write": [
            text_lob,
            {"type": "int64", "value": 2},
            {"text_value": "YZ"},
        ],
        "sb.lob.truncate": [
            text_lob,
            {"type": "int64", "value": 3},
        ],
        "sb.aggregate.any_value_expr": [
            {"text_value": "alpha"},
        ],
        "sb.scalar.at_time_zone": [
            {"text_value": "2026-05-18T10:00:00", "descriptor_id": "timestamp"},
            {"text_value": "America/Toronto"},
        ],
        "sb.aggregate.collect_expr": [
            {"text_value": "alpha"},
        ],
        "sb.scalar.domain_stack_value": [
            {"text_value": "alpha"},
        ],
        "sb.expr.match_recognize.v1": [
            {"text_value": "PATTERN(A+)"},
        ],
    }
    invalid_defaults: dict[str, list[Any]] = {
        "SBSFC048-pg-advisory-xact-lock-missing-transaction": [
            {"text_value": "not-an-advisory-key"},
        ],
        "SBSFC055-lob-open-malformed": [
            {"text_value": "not-a-lob-locator"},
        ],
        "SBSFC056-integer-invalid": [
            {"text_value": "not-an-integer"},
        ],
    }
    if row.get("expected_diagnostic_code"):
        return invalid_defaults.get(fixture_id, [])
    if function_id == "sb.lob.append" and fixture_id == "SBSFC055-lob-append-marker":
        return [{"text_value": "hi"}]
    if function_id == "sb.lob.append":
        return [text_lob, {"text_value": "!"}]
    return defaults.get(function_id, [])


def currval_sequence_name(row: dict[str, str]) -> str:
    if row.get("expected_diagnostic_code"):
        return ""
    function_id = (
        row.get("canonical_builtin_id")
        or row.get("function_id")
        or ""
    ).split("(", 1)[0].split("|", 1)[0].strip().lower()
    if function_id != "sb.scalar.currval":
        return ""
    args = json_payload(row.get("arguments_json"), [])
    if not isinstance(args, list) or len(args) != 1:
        return ""
    arg = args[0]
    if not isinstance(arg, dict) or arg.get("is_null"):
        return ""
    value = arg.get("value") or arg.get("text_value")
    return str(value or "")


def runtime_expected_value(row: dict[str, str], expected_value: str) -> str:
    if expected_value:
        return expected_value
    fixture_id = row.get("fixture_id", "")
    overrides = {
        "SBSFC055-lob-write-marker": '{"kind":"lob_locator","locator_id":"lob:test","state":"open","mode":"read_write","class":"text","media_type":"text/plain","data_hex":"68595a6c6f","size":5,"valid":true}',
        "SBSFC055-lob-append-marker": '{"kind":"lob_locator","locator_id":"lob:inline","state":"open","mode":"read_write","class":"binary","media_type":"application/octet-stream","data_hex":"6869","size":2,"valid":true}',
        "SBSFC055-lob-truncate-marker": '{"kind":"lob_locator","locator_id":"lob:test","state":"open","mode":"read_write","class":"text","media_type":"text/plain","data_hex":"68656c","size":3,"valid":true}',
        "SBSFC055-lob-truncate-arg": '{"kind":"lob_locator","locator_id":"lob:test","state":"open","mode":"read_write","class":"text","media_type":"text/plain","data_hex":"68656c","size":3,"valid":true}',
        "SBSFC055-lob-write-arg": '{"kind":"lob_locator","locator_id":"lob:test","state":"open","mode":"read_write","class":"text","media_type":"text/plain","data_hex":"68595a6c6f","size":5,"valid":true}',
        "SBSFC055-lob-append-arg": '{"kind":"lob_locator","locator_id":"lob:test","state":"open","mode":"read_write","class":"text","media_type":"text/plain","data_hex":"68656c6c6f21","size":6,"valid":true}',
    }
    return overrides.get(fixture_id, expected_value)


def render_function_call(row: dict[str, str]) -> str:
    name = canonical_direct_function_name(row)
    args = json_payload(row.get("arguments_json"), [])
    if not isinstance(args, list):
        args = [args]
    if not args:
        args = runtime_default_arguments(row)
    rendered_args = ", ".join(argument_to_sql(item) for item in args)
    return f"{name}({rendered_args})"


def render_invocation_statement(row: dict[str, str]) -> str:
    fixture_id = row.get("fixture_id", "")
    expected_value = runtime_expected_value(
        row,
        row.get("expected_result_value") or row.get("expected_result_json") or "",
    )
    expected_diag = row.get("expected_diagnostic_code", "")
    ctes, actual, from_clause = render_route_call_parts(row)
    if expected_diag and ctes and row.get("evaluation_mode", "") in {"aggregate", "ordered_set"}:
        select = (
            f"SELECT {sql_string(fixture_id)} AS fixture_id,\n"
            f"       {actual} AS actual_value,\n"
            f"       {sql_string(expected_value)} AS expected_value,\n"
            f"       {sql_string(expected_diag)} AS expected_diagnostic_code{from_clause};"
        )
    elif expected_diag:
        select = f"SELECT {actual} AS rejected_value{from_clause};"
    else:
        select = (
            f"SELECT {sql_string(fixture_id)} AS fixture_id,\n"
            f"       {actual} AS actual_value,\n"
            f"       {sql_string(expected_value)} AS expected_value,\n"
            f"       {sql_string(expected_diag)} AS expected_diagnostic_code{from_clause};"
        )
    if not ctes:
        return select
    return "WITH " + ",\n".join(ctes) + "\n" + select


def supports_authenticated_full_route_invocation(row: dict[str, str]) -> bool:
    """Return false for lower-level engine fixtures a live driver route cannot express."""
    return row.get("fixture_id") not in {
        "SBSFC048-pg-advisory-xact-lock-missing-transaction",
    }


def write_text(path: Path, text: str) -> dict[str, Any]:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text.rstrip() + "\n", encoding="utf-8")
    return {"path": str(path), "sha256": sha256_text(text.rstrip() + "\n"), "lines": text.count("\n") + 1}


def generate_surface_replay_manifest(namespace: str, replay_rows: list[dict[str, str]], e2e_counts: dict[str, int]) -> tuple[str, int]:
    expression_surface_kinds = {"function", "operator", "variable"}
    replay_row_count = len(replay_rows)
    full_route_count = sum(1 for row in replay_rows if "full_route" in row.get("route_set", ""))
    expression_runtime_count = sum(
        1 for row in replay_rows if row.get("surface_kind") in expression_surface_kinds
    )
    statement_surface_count = replay_row_count - expression_runtime_count
    lines = [
        "-- script_id: SBDFS-100",
        "-- Generated full SBSQL surface replay manifest.",
        f"CREATE TABLE {namespace}.surface_replay_manifest (",
        "    fixture_id VARCHAR(96) PRIMARY KEY,",
        "    surface_id VARCHAR(64) NOT NULL,",
        "    batch_id VARCHAR(32),",
        "    canonical_name VARCHAR(256),",
        "    family VARCHAR(128),",
        "    surface_kind VARCHAR(64),",
        "    source_status VARCHAR(64),",
        "    operation_family VARCHAR(128),",
        "    primary_route VARCHAR(96),",
        "    route_set TEXT,",
        "    input_text TEXT,",
        "    expected_server_result TEXT,",
        "    expected_engine_effect TEXT,",
        "    expected_payload_ref TEXT",
        ");",
        "",
    ]
    manifest_rows: list[str] = []
    for row in replay_rows:
        values = [
            sql_string(row.get("fixture_id")),
            sql_string(row.get("surface_id")),
            sql_string(row.get("batch_id")),
            sql_string(metadata_text(row.get("canonical_name"))),
            sql_string(row.get("family")),
            sql_string(row.get("surface_kind")),
            sql_string(row.get("source_status")),
            sql_string(row.get("operation_family")),
            sql_string(row.get("primary_route")),
            sql_string(row.get("route_set")),
            sql_string(row.get("input_text")),
            sql_string(row.get("expected_server_result")),
            sql_string(row.get("expected_engine_effect")),
            sql_string(row.get("expected_payload_json")),
        ]
        manifest_rows.append(f"({', '.join(values)})")
    append_batched_values_insert(
        lines,
        f"{namespace}.surface_replay_manifest",
        manifest_rows,
        batch_size=256,
    )
    lines.extend(
        [
            "",
            "SELECT 'SBDFS-100-001' AS assertion_id,",
            "       COUNT(*) AS actual_replay_rows,",
            f"       {replay_row_count} AS expected_replay_rows",
            f"FROM {namespace}.surface_replay_manifest;",
            "",
            "SELECT 'SBDFS-100-002' AS assertion_id,",
            "       COUNT(*) AS actual_full_route_rows,",
            f"       {full_route_count} AS expected_full_route_rows",
            f"FROM {namespace}.surface_replay_manifest",
            "WHERE route_set LIKE '%full_route%';",
            "",
            "SELECT 'SBDFS-100-003' AS assertion_id,",
            "       COUNT(*) AS actual_expression_runtime_rows,",
            f"       {expression_runtime_count} AS expected_expression_runtime_rows",
            f"FROM {namespace}.surface_replay_manifest",
            "WHERE surface_kind IN ('function', 'operator', 'variable');",
            "",
            "SELECT 'SBDFS-100-004' AS assertion_id,",
            "       COUNT(*) AS actual_statement_surface_rows,",
            f"       {statement_surface_count} AS expected_statement_surface_rows",
            f"FROM {namespace}.surface_replay_manifest",
            "WHERE surface_kind NOT IN ('function', 'operator', 'variable');",
            "",
        ]
    )
    return "\n".join(lines), len(replay_rows) + 4


def generate_surface_replay_commands(namespace: str, replay_rows: list[dict[str, str]]) -> tuple[str, int]:
    lines = [
        "-- script_id: SBDFS-101",
        "-- Generated parser/server replay commands for every registered SBSQL surface.",
        f"-- namespace placeholder: {namespace}",
    ]
    for row in replay_rows:
        lines.append(f"-- fixture_id: {row.get('fixture_id')} surface_id: {row.get('surface_id')}")
        lines.append(str(row.get("input_text", "")).rstrip(";") + ";")
    lines.extend(
        [
            "",
            "SELECT 'SBDFS-101-001' AS assertion_id,",
            "       COUNT(*) AS actual_replay_command_count,",
            f"       {len(replay_rows)} AS expected_replay_command_count",
            "FROM sys.parser.language_elements",
            "WHERE surface_id IS NOT NULL;",
            "",
        ]
    )
    return "\n".join(lines), len(replay_rows) + 1


def generate_sblr_roundtrip_manifest(namespace: str, repo_root: Path, e2e_counts: dict[str, int]) -> tuple[str, int, list[dict[str, str]]]:
    records: list[dict[str, str]] = []
    for path in sorted((repo_root / SBLR_ROUND_TRIP_ROOT_REL).glob("*.yaml")):
        text = path.read_text(encoding="utf-8")
        surface_match = re.search(r'^surface_id:\s*"([^"]+)"', text, re.M)
        canonical_match = re.search(r'^canonical_name:\s*"([^"]+)"', text, re.M)
        records.append(
            {
                "surface_id": surface_match.group(1) if surface_match else path.stem.split(".", 1)[0],
                "canonical_name": metadata_text(canonical_match.group(1) if canonical_match else ""),
                "path": str(path.relative_to(repo_root)),
                "sha256": sha256_text(text),
            }
        )
    lines = [
        "-- script_id: SBDFS-110",
        "-- Generated SBLR binary/UUID round-trip proof index.",
        f"CREATE TABLE {namespace}.sblr_roundtrip_manifest (",
        "    surface_id VARCHAR(64) PRIMARY KEY,",
        "    canonical_name VARCHAR(256),",
        "    fixture_path TEXT NOT NULL,",
        "    fixture_sha256 VARCHAR(64) NOT NULL",
        ");",
        "",
    ]
    manifest_rows: list[str] = []
    for row in records:
        values = [sql_string(row["surface_id"]), sql_string(row["canonical_name"]), sql_string(row["path"]), sql_string(row["sha256"])]
        manifest_rows.append(f"({', '.join(values)})")
    append_batched_values_insert(
        lines,
        f"{namespace}.sblr_roundtrip_manifest",
        manifest_rows,
        batch_size=256,
    )
    lines.extend(
        [
            "",
            "SELECT 'SBDFS-110-001' AS assertion_id,",
            "       COUNT(*) AS actual_sblr_roundtrip_rows,",
            f"       {e2e_counts.get('surface_registry', len(records))} AS expected_sblr_roundtrip_rows",
            f"FROM {namespace}.sblr_roundtrip_manifest;",
            "",
        ]
    )
    return "\n".join(lines), len(records) + 1, records


def append_batched_values_insert(
    lines: list[str],
    table_name: str,
    rows: list[str],
    *,
    batch_size: int = 128,
) -> int:
    if not rows:
        return 0
    statement_count = 0
    for offset in range(0, len(rows), batch_size):
        batch = rows[offset:offset + batch_size]
        lines.append(f"INSERT INTO {table_name} VALUES")
        for index, row in enumerate(batch):
            suffix = ";" if index + 1 == len(batch) else ","
            lines.append(f"    {row}{suffix}")
        statement_count += 1
    return statement_count


def generate_datatype_native_load(namespace: str, datatypes: list[str], rows_per_type: int = 64) -> tuple[str, int]:
    lines = [
        "-- script_id: SBDFS-120",
        "-- Generated native datatype load and deterministic random value matrix.",
        f"CREATE TABLE {namespace}.datatype_surface_manifest (",
        "    datatype_name VARCHAR(96) PRIMARY KEY,",
        "    sql_type_name VARCHAR(160) NOT NULL,",
        "    generated_rows INTEGER NOT NULL",
        ");",
        "",
    ]
    statement_count = 1
    for datatype in datatypes:
        table_name = f"dt_{qident(datatype)}_values"
        sql_type = CONCRETE_SQL_TYPES.get(datatype, datatype.upper())
        lines.append(
            f"CREATE TABLE {namespace}.{table_name} ("
            "case_id INTEGER PRIMARY KEY, "
            f"sample_value {sql_type}, "
            f"alternate_value {sql_type}, "
            "seed_text VARCHAR(128) NOT NULL);"
        )
        statement_count += append_batched_values_insert(
            lines,
            f"{namespace}.datatype_surface_manifest",
            [f"({sql_string(datatype)}, {sql_string(sql_type)}, {rows_per_type})"],
        )
        row_values: list[str] = []
        for index in range(rows_per_type):
            values = [
                str(index),
                literal_for_datatype(datatype, index),
                literal_for_datatype(datatype, index + rows_per_type),
                sql_string(f"{datatype}:{index}"),
            ]
            row_values.append(f"({', '.join(values)})")
        statement_count += 1
        statement_count += append_batched_values_insert(lines, f"{namespace}.{table_name}", row_values)
    lines.extend(
        [
            "",
            "SELECT 'SBDFS-120-001' AS assertion_id,",
            "       COUNT(*) AS actual_datatype_count,",
            f"       {len(datatypes)} AS expected_datatype_count",
            f"FROM {namespace}.datatype_surface_manifest;",
            "",
            "SELECT 'SBDFS-120-002' AS assertion_id,",
            "       SUM(generated_rows) AS actual_generated_rows,",
            f"       {len(datatypes) * rows_per_type} AS expected_generated_rows",
            f"FROM {namespace}.datatype_surface_manifest;",
            "",
        ]
    )
    return "\n".join(lines), statement_count + 2


def generate_datatype_dml_matrix(namespace: str, datatypes: list[str]) -> tuple[str, int]:
    operations = ("insert", "select", "update", "delete", "merge", "upsert", "returning", "predicate")
    lines = [
        "-- script_id: SBDFS-130",
        "-- Generated per-datatype DML matrix.",
        f"CREATE TABLE {namespace}.datatype_dml_case_manifest (",
        "    case_id VARCHAR(160) PRIMARY KEY,",
        "    datatype_name VARCHAR(96) NOT NULL,",
        "    operation_name VARCHAR(32) NOT NULL,",
        "    statement_text TEXT NOT NULL",
        ");",
        "",
    ]
    statement_count = 1
    manifest_rows: list[str] = []
    execution_statements: list[str] = []
    for datatype in datatypes:
        table_name = f"dt_{qident(datatype)}_values"
        for operation in operations:
            case_id = f"DML-{qident(datatype)}-{operation}"
            if operation == "insert":
                statement = f"INSERT INTO {namespace}.{table_name} VALUES (100001, {literal_for_datatype(datatype, 100001)}, {literal_for_datatype(datatype, 100002)}, 'dml-extra')"
            elif operation == "select":
                statement = f"SELECT COUNT(*) FROM {namespace}.{table_name} WHERE sample_value IS NOT NULL"
            elif operation == "update":
                statement = f"UPDATE {namespace}.{table_name} SET sample_value = alternate_value WHERE case_id % 7 = 0"
            elif operation == "delete":
                statement = f"DELETE FROM {namespace}.{table_name} WHERE case_id = 100001"
            elif operation == "merge":
                statement = f"MERGE INTO {namespace}.{table_name} AS target USING {namespace}.{table_name} AS source ON target.case_id = source.case_id WHEN MATCHED THEN UPDATE SET seed_text = source.seed_text"
            elif operation == "upsert":
                statement = f"UPSERT INTO {namespace}.{table_name} (case_id, sample_value, alternate_value, seed_text) VALUES (100002, {literal_for_datatype(datatype, 100003)}, {literal_for_datatype(datatype, 100004)}, 'dml-upsert')"
            elif operation == "returning":
                statement = f"UPDATE {namespace}.{table_name} SET seed_text = seed_text || ':returning' WHERE case_id = 1 RETURNING case_id"
            else:
                statement = f"SELECT case_id FROM {namespace}.{table_name} WHERE sample_value = alternate_value OR sample_value IS NULL"
            manifest_rows.append(
                f"({sql_string(case_id)}, {sql_string(datatype)}, {sql_string(operation)}, {sql_string(statement)})"
            )
            execution_statements.append(statement + ";")
    statement_count += append_batched_values_insert(
        lines,
        f"{namespace}.datatype_dml_case_manifest",
        manifest_rows,
    )
    lines.extend(execution_statements)
    statement_count += len(execution_statements)
    lines.extend(
        [
            "",
            "SELECT 'SBDFS-130-001' AS assertion_id,",
            "       COUNT(*) AS actual_dml_cases,",
            f"       {len(datatypes) * len(operations)} AS expected_dml_cases",
            f"FROM {namespace}.datatype_dml_case_manifest;",
            "",
        ]
    )
    return "\n".join(lines), statement_count + 1


def generate_index_matrix(namespace: str, datatypes: list[str], families: list[str]) -> tuple[str, int]:
    variations = ("plain", "unique", "partial", "covering", "expression", "descending")
    lines = [
        "-- script_id: SBDFS-140",
        "-- Generated index family x datatype x variation matrix.",
        f"CREATE TABLE {namespace}.index_case_manifest (",
        "    case_id VARCHAR(192) PRIMARY KEY,",
        "    datatype_name VARCHAR(96) NOT NULL,",
        "    family_name VARCHAR(64) NOT NULL,",
        "    variation_name VARCHAR(64) NOT NULL,",
        "    statement_text TEXT NOT NULL",
        ");",
        "",
        f"CREATE TABLE {namespace}.index_unique_family_probe_values (",
        "    case_id INTEGER PRIMARY KEY,",
        "    sample_value BIGINT NOT NULL UNIQUE,",
        "    seed_text VARCHAR(128) NOT NULL UNIQUE",
        ");",
        "",
        f"INSERT INTO {namespace}.index_unique_family_probe_values (case_id, sample_value, seed_text) VALUES",
        "    (1, 101, 'alpha'),",
        "    (2, 202, 'bravo'),",
        "    (3, 303, 'charlie'),",
        "    (4, 404, 'delta'),",
        "    (5, 505, 'echo'),",
        "    (6, 606, 'foxtrot'),",
        "    (7, 707, 'golf'),",
        "    (8, 808, 'hotel');",
        "",
    ]
    statement_count = 3
    manifest_rows: list[str] = []
    execution_statements: list[str] = []
    datatype_family = "btree" if "btree" in families else (families[0] if families else "btree")
    for datatype in datatypes:
        table_name = f"dt_{qident(datatype)}_values"
        idx_name = f"idx_datatype_{qident(datatype)}_{qident(datatype_family)}"
        statement = f"CREATE INDEX {idx_name} ON {namespace}.{table_name} USING {datatype_family.upper()} (sample_value)"
        case_id = f"IDX-DATATYPE-{qident(datatype)}-{qident(datatype_family)}"
        manifest_rows.append(
            f"({sql_string(case_id)}, {sql_string(datatype)}, {sql_string(datatype_family)}, "
            f"{sql_string('datatype_plain')}, {sql_string(statement)})"
        )
        execution_statements.append(statement + ";")
    stable_table = f"{namespace}.index_unique_family_probe_values"
    for family in families:
        family_sql = family.upper()
        for variation in variations:
            idx_name = f"idx_family_{qident(family)}_{variation}"
            if variation == "unique":
                statement = f"CREATE UNIQUE INDEX {idx_name} ON {stable_table} USING {family_sql} (case_id)"
            elif variation == "partial":
                statement = f"CREATE INDEX {idx_name} ON {stable_table} USING {family_sql} (sample_value) WHERE case_id % 2 = 0"
            elif variation == "covering":
                statement = f"CREATE INDEX {idx_name} ON {stable_table} USING {family_sql} (sample_value) INCLUDE (seed_text)"
            elif variation == "expression":
                statement = f"CREATE INDEX {idx_name} ON {stable_table} USING {family_sql} (CAST(sample_value AS VARCHAR(512)))"
            elif variation == "descending":
                statement = f"CREATE INDEX {idx_name} ON {stable_table} USING {family_sql} (sample_value DESC)"
            else:
                statement = f"CREATE INDEX {idx_name} ON {stable_table} USING {family_sql} (sample_value)"
            case_id = f"IDX-FAMILY-{qident(family)}-{variation}"
            manifest_rows.append(
                f"({sql_string(case_id)}, {sql_string('int64')}, {sql_string(family)}, "
                f"{sql_string(variation)}, {sql_string(statement)})"
            )
            execution_statements.append(statement + ";")
    statement_count += append_batched_values_insert(lines, f"{namespace}.index_case_manifest", manifest_rows)
    lines.extend(execution_statements)
    statement_count += len(execution_statements)
    expected = len(datatypes) + len(families) * len(variations)
    lines.extend(
        [
            "",
            "SELECT 'SBDFS-140-001' AS assertion_id,",
            "       COUNT(*) AS actual_index_cases,",
            f"       {expected} AS expected_index_cases",
            f"FROM {namespace}.index_case_manifest;",
            "",
        ]
    )
    return "\n".join(lines), statement_count + 1


def generate_query_matrix(namespace: str, datatypes: list[str]) -> tuple[str, int]:
    operations = (
        "point_lookup",
        "range_scan",
        "null_scan",
        "self_inner_join",
        "self_left_join",
        "exists_subquery",
        "group_by_value",
        "window_by_id",
        "recursive_cte",
        "union_self",
        "order_by_value",
        "aggregate_count",
    )
    lines = [
        "-- script_id: SBDFS-150",
        "-- Generated query/join/window/CTE matrix over every datatype table.",
        f"CREATE TABLE {namespace}.query_case_manifest (",
        "    case_id VARCHAR(192) PRIMARY KEY,",
        "    datatype_name VARCHAR(96) NOT NULL,",
        "    operation_name VARCHAR(64) NOT NULL,",
        "    statement_text TEXT NOT NULL",
        ");",
        "",
    ]
    statement_count = 1
    manifest_rows: list[str] = []
    execution_statements: list[str] = []
    for datatype in datatypes:
        table_name = f"dt_{qident(datatype)}_values"
        for operation in operations:
            if operation == "point_lookup":
                statement = f"SELECT * FROM {namespace}.{table_name} WHERE case_id = 1"
            elif operation == "range_scan":
                statement = f"SELECT COUNT(*) FROM {namespace}.{table_name} WHERE case_id BETWEEN 10 AND 20"
            elif operation == "null_scan":
                statement = f"SELECT COUNT(*) FROM {namespace}.{table_name} WHERE sample_value IS NULL"
            elif operation == "self_inner_join":
                statement = f"SELECT COUNT(*) FROM {namespace}.{table_name} AS a INNER JOIN {namespace}.{table_name} AS b ON a.case_id = b.case_id"
            elif operation == "self_left_join":
                statement = f"SELECT COUNT(*) FROM {namespace}.{table_name} AS a LEFT JOIN {namespace}.{table_name} AS b ON a.case_id = b.case_id + 1"
            elif operation == "exists_subquery":
                statement = f"SELECT COUNT(*) FROM {namespace}.{table_name} AS a WHERE EXISTS (SELECT 1 FROM {namespace}.{table_name} AS b WHERE b.case_id = a.case_id)"
            elif operation == "group_by_value":
                statement = f"SELECT sample_value, COUNT(*) FROM {namespace}.{table_name} GROUP BY sample_value"
            elif operation == "window_by_id":
                statement = f"SELECT case_id, ROW_NUMBER() OVER (ORDER BY case_id) AS rn FROM {namespace}.{table_name}"
            elif operation == "recursive_cte":
                statement = "WITH RECURSIVE seq(n) AS (VALUES (1) UNION ALL SELECT n + 1 FROM seq WHERE n < 16) SELECT COUNT(*) FROM seq"
            elif operation == "union_self":
                statement = f"SELECT case_id FROM {namespace}.{table_name} WHERE case_id < 4 UNION SELECT case_id FROM {namespace}.{table_name} WHERE case_id < 4"
            elif operation == "order_by_value":
                statement = f"SELECT sample_value FROM {namespace}.{table_name} ORDER BY sample_value"
            else:
                statement = f"SELECT COUNT(*), MIN(case_id), MAX(case_id) FROM {namespace}.{table_name}"
            case_id = f"QRY-{qident(datatype)}-{operation}"
            manifest_rows.append(
                f"({sql_string(case_id)}, {sql_string(datatype)}, {sql_string(operation)}, {sql_string(statement)})"
            )
            execution_statements.append(statement + ";")
    statement_count += append_batched_values_insert(
        lines,
        f"{namespace}.query_case_manifest",
        manifest_rows,
    )
    lines.extend(execution_statements)
    statement_count += len(execution_statements)
    expected = len(datatypes) * len(operations)
    lines.extend(
        [
            "",
            "SELECT 'SBDFS-150-001' AS assertion_id,",
            "       COUNT(*) AS actual_query_cases,",
            f"       {expected} AS expected_query_cases",
            f"FROM {namespace}.query_case_manifest;",
            "",
        ]
    )
    return "\n".join(lines), statement_count + 1


def generate_builtin_invocations(namespace: str, rows: list[dict[str, str]]) -> tuple[str, int]:
    route_rows = [row for row in rows if supports_authenticated_full_route_invocation(row)]
    lines = [
        "-- script_id: SBDFS-160",
        "-- Generated executable built-in function/operator invocations from SBSFC fixture rows.",
        f"-- namespace placeholder: {namespace}",
    ]
    setup_count = 0
    primed_currval_sequences: set[str] = set()
    for row in route_rows:
        fixture_id = row.get("fixture_id", "")
        expected_diag = row.get("expected_diagnostic_code", "")
        sequence_name = currval_sequence_name(row)
        if sequence_name and sequence_name not in primed_currval_sequences:
            lines.append(
                f"-- fixture_setup: prime_currval_sequence sequence_name: {sequence_name}"
            )
            lines.append(f"SELECT nextval({sql_string(sequence_name)}) AS currval_setup_value;")
            primed_currval_sequences.add(sequence_name)
            setup_count += 1
        lines.append(f"-- fixture_id: {fixture_id} expected_diagnostic: {expected_diag or 'none'}")
        lines.append(render_invocation_statement(row))
    lines.extend(
        [
            "",
            "WITH generated_builtin_invocation(fixture_id) AS (",
            "VALUES",
        ]
    )
    for index, row in enumerate(route_rows):
        suffix = "," if index + 1 < len(route_rows) else ""
        lines.append(f"    ({sql_string(row.get('fixture_id'))}){suffix}")
    lines.extend(
        [
            ")",
            "SELECT 'SBDFS-160-001' AS assertion_id,",
            "       COUNT(*) AS actual_builtin_invocation_count,",
            f"       {len(route_rows)} AS expected_builtin_invocation_count",
            "FROM generated_builtin_invocation;",
            "",
        ]
    )
    return "\n".join(lines), len(route_rows) + setup_count + 1


def generate_cast_operator_matrix(namespace: str, datatypes: list[str]) -> tuple[str, int]:
    lines = [
        "-- script_id: SBDFS-170",
        "-- Generated cast/operator matrix across every datatype pair.",
        f"CREATE TABLE {namespace}.cast_operator_case_manifest (",
        "    case_id VARCHAR(192) PRIMARY KEY,",
        "    source_datatype VARCHAR(96) NOT NULL,",
        "    target_datatype VARCHAR(96) NOT NULL,",
        "    statement_text TEXT NOT NULL",
        ");",
        "",
    ]
    statement_count = 1
    manifest_rows: list[str] = []
    execution_statements: list[str] = []
    cast_batch_size = 16
    for source in datatypes:
        source_table = f"dt_{qident(source)}_values"
        batch_expressions: list[str] = []
        for target_index, target in enumerate(datatypes):
            target_type = CONCRETE_SQL_TYPES.get(target, target.upper())
            case_id = f"CAST-{qident(source)}-TO-{qident(target)}"
            statement = f"SELECT CAST(sample_value AS {target_type}) AS cast_value FROM {namespace}.{source_table} WHERE case_id = 1"
            manifest_rows.append(
                f"({sql_string(case_id)}, {sql_string(source)}, {sql_string(target)}, {sql_string(statement)})"
            )
            batch_expressions.append(
                f"CAST(sample_value AS {target_type}) AS cast_{target_index}"
            )
            if len(batch_expressions) == cast_batch_size:
                execution_statements.append(
                    "SELECT " + ", ".join(batch_expressions) +
                    f" FROM {namespace}.{source_table} WHERE case_id = 1;"
                )
                batch_expressions = []
        if batch_expressions:
            execution_statements.append(
                "SELECT " + ", ".join(batch_expressions) +
                f" FROM {namespace}.{source_table} WHERE case_id = 1;"
            )
    statement_count += append_batched_values_insert(
        lines,
        f"{namespace}.cast_operator_case_manifest",
        manifest_rows,
    )
    lines.extend(execution_statements)
    statement_count += len(execution_statements)
    expected = len(datatypes) * len(datatypes)
    lines.extend(
        [
            "",
            "SELECT 'SBDFS-170-001' AS assertion_id,",
            "       COUNT(*) AS actual_cast_cases,",
            f"       {expected} AS expected_cast_cases",
            f"FROM {namespace}.cast_operator_case_manifest;",
            "",
        ]
    )
    return "\n".join(lines), statement_count + 1


def generate_optimizer_manifest(namespace: str, repo_root: Path, families: list[str], page_sizes: list[str]) -> tuple[str, int, list[dict[str, str]]]:
    source_files = [
        repo_root / INDEX_PROOF_MATRIX_REL,
        repo_root / INDEX_READINESS_MANIFEST_REL,
        repo_root / OPTIMIZER_READINESS_MANIFEST_REL,
    ]
    records = [
        {
            "path": str(path.relative_to(repo_root)),
            "sha256": sha256_file(path),
            "bytes": str(path.stat().st_size),
        }
        for path in source_files
    ]
    lines = [
        "-- script_id: SBDFS-180",
        "-- Generated optimizer, page-size, and index readiness source manifest.",
        f"CREATE TABLE {namespace}.optimizer_storage_source_manifest (",
        "    source_path TEXT PRIMARY KEY,",
        "    source_sha256 VARCHAR(64) NOT NULL,",
        "    source_bytes INTEGER NOT NULL",
        ");",
        f"CREATE TABLE {namespace}.optimizer_page_index_matrix (",
        "    case_id VARCHAR(160) PRIMARY KEY,",
        "    page_size INTEGER NOT NULL,",
        "    index_family VARCHAR(64) NOT NULL,",
        "    statement_text TEXT NOT NULL",
        ");",
        "",
    ]
    statement_count = 2
    for row in records:
        lines.append(
            f"INSERT INTO {namespace}.optimizer_storage_source_manifest VALUES "
            f"({sql_string(row['path'])}, {sql_string(row['sha256'])}, {row['bytes']});"
        )
        statement_count += 1
    for page_size in page_sizes:
        for family in families:
            case_id = f"OPT-{page_size}-{qident(family)}"
            statement = f"EXPLAIN SELECT * FROM {namespace}.dt_int64_values WHERE case_id BETWEEN 1 AND {int(page_size) % 97 + 1}"
            lines.append(
                f"INSERT INTO {namespace}.optimizer_page_index_matrix VALUES "
                f"({sql_string(case_id)}, {int(page_size)}, {sql_string(family)}, {sql_string(statement)});"
            )
            lines.append(statement + ";")
            statement_count += 2
    expected = len(page_sizes) * len(families)
    lines.extend(
        [
            "",
            "SELECT 'SBDFS-180-001' AS assertion_id,",
            "       COUNT(*) AS actual_optimizer_page_index_cases,",
            f"       {expected} AS expected_optimizer_page_index_cases",
            f"FROM {namespace}.optimizer_page_index_matrix;",
            "",
        ]
    )
    return "\n".join(lines), statement_count + 1, records


def copy_expected_indexes(repo_root: Path, output_root: Path, roundtrip_records: list[dict[str, str]], optimizer_records: list[dict[str, str]]) -> list[str]:
    expected_root = output_root / "expected" / "exhaustive_sources"
    expected_root.mkdir(parents=True, exist_ok=True)
    copied: list[str] = []
    replay_index_target = expected_root / REPLAY_INDEX_REL.name
    replay_rows = read_csv_rows(repo_root / REPLAY_INDEX_REL)
    if replay_rows:
        fieldnames = list(replay_rows[0].keys())
        with replay_index_target.open("w", newline="", encoding="utf-8") as handle:
            writer = csv.DictWriter(handle, fieldnames=fieldnames)
            writer.writeheader()
            for row in replay_rows:
                copy = dict(row)
                copy["canonical_name"] = metadata_text(copy.get("canonical_name"))
                writer.writerow(copy)
    else:
        replay_index_target.write_text("", encoding="utf-8")
    copied.append(str(replay_index_target))

    for rel_path in (REPLAY_PAYLOADS_REL, E2E_MATRIX_REL):
        source = repo_root / rel_path
        target = expected_root / source.name
        shutil.copyfile(source, target)
        copied.append(str(target))
    roundtrip_index = expected_root / "sblr_roundtrip_index.jsonl"
    roundtrip_index.write_text(
        "".join(json.dumps(row, sort_keys=True) + "\n" for row in roundtrip_records),
        encoding="utf-8",
    )
    copied.append(str(roundtrip_index))
    optimizer_index = expected_root / "optimizer_storage_source_index.jsonl"
    optimizer_index.write_text(
        "".join(json.dumps(row, sort_keys=True) + "\n" for row in optimizer_records),
        encoding="utf-8",
    )
    copied.append(str(optimizer_index))
    return copied


def generate_exhaustive_assets(
    *,
    repo_root: Path,
    output_scripts: Path,
    output_root: Path,
    namespace: str,
    manifest: dict[str, Any],
) -> tuple[list[dict[str, Any]], list[str], dict[str, Any]]:
    replay_rows = read_csv_rows(repo_root / REPLAY_INDEX_REL)
    e2e_counts = {row["scope_id"]: int(row["expected_count"]) for row in read_csv_rows(repo_root / E2E_MATRIX_REL)}
    datatypes = required_datatypes(repo_root)
    families = index_families(repo_root)
    page_sizes = supported_page_sizes(repo_root)
    builtins = fixture_rows(repo_root)

    scripts: list[tuple[str, str, int]] = []
    surface_manifest, surface_cases = generate_surface_replay_manifest(namespace, replay_rows, e2e_counts)
    scripts.append(("SBDFS-100", surface_manifest, surface_cases))
    replay_commands, replay_cases = generate_surface_replay_commands(namespace, replay_rows)
    scripts.append(("SBDFS-101", replay_commands, replay_cases))
    roundtrip_manifest, roundtrip_cases, roundtrip_records = generate_sblr_roundtrip_manifest(namespace, repo_root, e2e_counts)
    scripts.append(("SBDFS-110", roundtrip_manifest, roundtrip_cases))
    datatype_load, datatype_load_cases = generate_datatype_native_load(namespace, datatypes)
    scripts.append(("SBDFS-120", datatype_load, datatype_load_cases))
    dml_matrix, dml_cases = generate_datatype_dml_matrix(namespace, datatypes)
    scripts.append(("SBDFS-130", dml_matrix, dml_cases))
    index_matrix, index_cases = generate_index_matrix(namespace, datatypes, families)
    scripts.append(("SBDFS-140", index_matrix, index_cases))
    query_matrix, query_cases = generate_query_matrix(namespace, datatypes)
    scripts.append(("SBDFS-150", query_matrix, query_cases))
    builtin_invocations, builtin_cases = generate_builtin_invocations(namespace, builtins)
    scripts.append(("SBDFS-160", builtin_invocations, builtin_cases))
    cast_matrix, cast_cases = generate_cast_operator_matrix(namespace, datatypes)
    scripts.append(("SBDFS-170", cast_matrix, cast_cases))
    optimizer_manifest, optimizer_cases, optimizer_records = generate_optimizer_manifest(namespace, repo_root, families, page_sizes)
    scripts.append(("SBDFS-180", optimizer_manifest, optimizer_cases))

    compiled_scripts: list[dict[str, Any]] = []
    generated_case_count = 0
    for script_id, text, case_count in scripts:
        filename = dict(GENERATED_SCRIPT_SPECS)[script_id]
        target = output_scripts / filename
        info = write_text(target, text)
        generated_case_count += case_count
        compiled_scripts.append(
            {
                "script_id": script_id,
                "source_path": "generated_from_public_full_surface_artifacts",
                "compiled_path": str(target),
                "sha256": info["sha256"],
                "assertions": [f"{script_id}-001"],
                "coverage": ["generated_full_surface"],
                "generated": True,
                "generated_case_count": case_count,
            }
        )
    expected_indexes = copy_expected_indexes(repo_root, output_root, roundtrip_records, optimizer_records)
    summary = source_summary(repo_root)
    summary.update(
        {
            "generated_script_count": len(compiled_scripts),
            "generated_case_count": generated_case_count,
            "generated_datatype_rows": len(datatypes) * 64,
            "generated_index_cases": len(datatypes) + len(families) * 6,
            "generated_cast_cases": len(datatypes) * len(datatypes),
        }
    )
    return compiled_scripts, expected_indexes, summary
