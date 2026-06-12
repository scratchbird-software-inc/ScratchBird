#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""SBSFC-015 aggregate/window fixture gate.

Validates the deterministic fixture slice for ranking windows,
hypothetical-set ordered aggregates, and aggregate/window runtime functions.
This static gate checks canonical surface IDs, canonical builtin records,
runtime alias/finalizer coverage, and independently recomputes expected values
from fixture data. Executable runtime behavior is covered by
sbsql_sbsfc_015_aggregate_window_runtime_conformance. Neither gate executes SQL
text, uses a reference engine, mutates storage, or touches MGA/recovery behavior.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import re
import sys
from pathlib import Path
from typing import Any


SURFACE_REGISTRY = "public_input_snapshot"
BUILTIN_EXPRESSION_REGISTRY = "public_contract_snapshot"
BUILTIN_WINDOW_REGISTRY = "public_contract_snapshot"
BUILTIN_AGGREGATE_REGISTRY = "public_contract_snapshot"
WINDOW_ALIAS_SOURCE = "project/src/engine/sblr/sblr_aggregate_window_runtime_02_aggregate_ordered_approx.inc"
AGGREGATE_GENERAL_SOURCE = "project/src/engine/sblr/sblr_aggregate_window_runtime_01_aggregate_general.inc"
WINDOW_EVAL_SOURCE = "project/src/engine/sblr/sblr_aggregate_window_runtime_04_window_evaluation.inc"
QUERY_PLAN_SOURCE = "project/src/engine/internal_api/query/plan_api.cpp"
FIXTURES = "project/tests/sbsql_parser_worker/generated/full_surface/SBSFC_015_AGGREGATE_WINDOW_FIXTURES.csv"


REQUIRED_COLUMNS = [
    "fixture_id",
    "surface_id",
    "function_id",
    "canonical_builtin_id",
    "case_kind",
    "evaluation_mode",
    "ordered_values_json",
    "expected_result_value",
    "expected_result_descriptor",
    "oracle_authority_ref",
    "notes",
]


EXPECTED_SURFACES = {
    "SBSQL-9801E5B61066": "sb.aggregate.json_agg",
    "SBSQL-F0C6698894BE": "sb.aggregate.json_agg",
    "SBSQL-9A5FDCD9D262": "sb.aggregate.json_object_agg",
    "SBSQL-B09433FF3AB3": "sb.aggregate.json_object_agg",
    "SBSQL-9C5FBF5051E9": "sb.aggregate.listagg",
    "SBSQL-ED484B8BAA9E": "sb.aggregate.listagg",
    "SBSQL-A2126F80AF83": "sb.aggregate.string_agg",
    "SBSQL-D9693B850C39": "sb.aggregate.string_agg",
    "SBSQL-8B9608CDF04B": "sb.aggregate.string_agg",
    "SBSQL-6D779E51D7CD": "sb.aggregate.array_agg",
    "SBSQL-9182A9C9CE44": "sb.aggregate.array_agg",
    "SBSQL-D2ADEB38C58C": "sb.aggregate.array_agg",
    "SBSQL-C173CE237D05": "sb.aggregate.approx_count_distinct",
    "SBSQL-0CC023EA8142": "sb.aggregate.approx_count_distinct",
    "SBSQL-251130FD5154": "sb.aggregate.approx_median",
    "SBSQL-254DFA81AAEB": "sb.aggregate.approx_median",
    "SBSQL-053DEF4FCA88": "sb.aggregate.approx_percentile_cont",
    "SBSQL-3E3C0C230076": "sb.aggregate.approx_percentile_cont",
    "SBSQL-F36C85C9C652": "sb.aggregate.approx_percentile_cont",
    "SBSQL-18063AC3EF3B": "sb.aggregate.approx_percentile_disc",
    "SBSQL-880B467C4F5C": "sb.aggregate.approx_percentile_disc",
    "SBSQL-C22AD93E1A89": "sb.aggregate.approx_top_k",
    "SBSQL-1675235E95F5": "sb.aggregate.approx_top_k",
    "SBSQL-405DA76744CA": "sb.aggregate.rank",
    "SBSQL-D9E3FA320510": "sb.aggregate.dense_rank",
    "SBSQL-443C68E68D9F": "sb.aggregate.percent_rank",
    "SBSQL-63BB74EAD479": "sb.aggregate.cume_dist",
    "SBSQL-73159B932B38": "sb.window.rank",
    "SBSQL-E7B5D653D886": "sb.window.dense_rank",
    "SBSQL-8F46078CCAA2": "sb.window.percent_rank",
    "SBSQL-3A4D165FF59E": "sb.window.cume_dist",
    "SBSQL-6F988BD1E2E0": "sb.aggregate.rank",
    "SBSQL-7B0D1EA07215": "sb.aggregate.dense_rank",
    "SBSQL-374E6DE31900": "sb.aggregate.percent_rank",
    "SBSQL-70B39E494FED": "sb.aggregate.cume_dist",
    "SBSQL-E33776097240": "sb.window.rank",
    "SBSQL-E1BCEE3D98B7": "sb.window.dense_rank",
    "SBSQL-513700E3598C": "sb.window.percent_rank",
    "SBSQL-F959FD740DD3": "sb.window.cume_dist",
    "SBSQL-E52C3FB97F6C": "sb.window.ntile",
    "SBSQL-6412E60ED18E": "sb.window.ntile",
    "SBSQL-1EF274EAE8DC": "sb.window.ntile",
    "SBSQL-28B6483D8641": "sb.window.row_number",
    "SBSQL-7A6AFA548A76": "sb.window.row_number",
    "SBSQL-BAF3A91528AA": "sb.window.row_number",
    "SBSQL-C02257DB2BE3": "sb.window.lag",
    "SBSQL-35A1ECA35D13": "sb.window.lag",
    "SBSQL-0F7E089AB839": "sb.window.lag",
    "SBSQL-CD90EEAF7468": "sb.window.lead",
    "SBSQL-F14938CD9CF3": "sb.window.lead",
    "SBSQL-F7B4F498213C": "sb.window.lead",
    "SBSQL-842F61769B34": "sb.window.first_value",
    "SBSQL-BDDEB821D132": "sb.window.first_value",
    "SBSQL-AA6AE730A722": "sb.window.first_value",
    "SBSQL-2D40C15A4E0A": "sb.window.last_value",
    "SBSQL-23AF50D41FEC": "sb.window.last_value",
    "SBSQL-804D99407A3B": "sb.window.last_value",
    "SBSQL-ED86D05F9232": "sb.window.nth_value",
    "SBSQL-4BC628E8AD6C": "sb.window.nth_value",
    "SBSQL-C97299B0256C": "sb.window.nth_value",
    "SBSQL-911CE2CE8601": "sb.aggregate.mode",
    "SBSQL-405CA73B58F3": "sb.aggregate.mode",
    "SBSQL-C170616B6CF5": "sb.aggregate.percentile_cont",
    "SBSQL-57867F44B9AB": "sb.aggregate.percentile_cont",
    "SBSQL-AC75EEABD55F": "sb.aggregate.percentile_disc",
    "SBSQL-07F31F7E4962": "sb.aggregate.percentile_disc",
    "SBSQL-CCADA416A4FB": "sb.aggregate.every",
    "SBSQL-2B3B74FBBF85": "sb.aggregate.every",
    "SBSQL-8FAF4700288F": "sb.aggregate.stddev",
    "SBSQL-C13458F7C063": "sb.aggregate.stddev",
    "SBSQL-AADC120814C2": "sb.aggregate.variance",
    "SBSQL-E316D15D86B0": "sb.aggregate.variance",
    "SBSQL-D4A54D6879E1": "sb.aggregate.stddev_pop",
    "SBSQL-46D54006C21A": "sb.aggregate.stddev_pop",
    "SBSQL-1B1392E72628": "sb.aggregate.stddev_pop",
    "SBSQL-1926F7E782F3": "sb.aggregate.variance_pop",
    "SBSQL-7CBEA5B27835": "sb.aggregate.variance_pop",
    "SBSQL-F89AE449F324": "sb.aggregate.variance_pop",
    "SBSQL-4769D666283F": "sb.aggregate.corr",
    "SBSQL-B66B527161BA": "sb.aggregate.corr",
    "SBSQL-53E3A168AD26": "sb.aggregate.stddev_samp",
    "SBSQL-D155F7EC1FE1": "sb.aggregate.stddev_samp",
    "SBSQL-4AF99A06B193": "sb.aggregate.variance_samp",
    "SBSQL-482B2C54BAF1": "sb.aggregate.variance_samp",
    "SBSQL-7D77C331D16C": "sb.aggregate.covar_pop",
    "SBSQL-E662CB944FC2": "sb.aggregate.covar_pop",
    "SBSQL-5B5757128C3F": "sb.aggregate.covar_samp",
    "SBSQL-FC78A3D1CF86": "sb.aggregate.covar_samp",
    "SBSQL-7102C019D2CF": "sb.aggregate.regr_avgx",
    "SBSQL-54324247868A": "sb.aggregate.regr_avgx",
    "SBSQL-DF6313DE4B56": "sb.aggregate.regr_avgy",
    "SBSQL-189983EF2867": "sb.aggregate.regr_avgy",
    "SBSQL-1BBEB1E43F45": "sb.aggregate.regr_count",
    "SBSQL-3C0839E8B792": "sb.aggregate.regr_count",
    "SBSQL-431925B5EC67": "sb.aggregate.regr_intercept",
    "SBSQL-8F9FD6E0E1B0": "sb.aggregate.regr_intercept",
    "SBSQL-794AAFE26F38": "sb.aggregate.regr_r2",
    "SBSQL-BE43021856AE": "sb.aggregate.regr_r2",
    "SBSQL-559DFA580089": "sb.aggregate.regr_slope",
    "SBSQL-BB7BA14B2666": "sb.aggregate.regr_slope",
    "SBSQL-C77EA68C577B": "sb.aggregate.regr_sxx",
    "SBSQL-D291129F3FD3": "sb.aggregate.regr_sxx",
    "SBSQL-61641209CF6B": "sb.aggregate.regr_sxy",
    "SBSQL-1F514A240E49": "sb.aggregate.regr_sxy",
    "SBSQL-1D81FEFFF22A": "sb.aggregate.regr_syy",
    "SBSQL-9C9BD835BEAF": "sb.aggregate.regr_syy",
    "SBSQL-3A54D06A4432": "sb.aggregate.count",
    "SBSQL-8112C4E21190": "sb.aggregate.count",
    "SBSQL-C7BF89473F5F": "sb.aggregate.count",
    "SBSQL-406FE50E60FE": "sb.aggregate.avg",
    "SBSQL-4E1B63BC07BF": "sb.aggregate.avg",
    "SBSQL-D581367F03C0": "sb.aggregate.avg",
    "SBSQL-4A77FBB6CB29": "sb.aggregate.min",
    "SBSQL-DE8222B7D6BC": "sb.aggregate.min",
    "SBSQL-F78BCCADDD0C": "sb.aggregate.min",
    "SBSQL-3DA79419E917": "sb.aggregate.max",
    "SBSQL-805A8D850D21": "sb.aggregate.max",
    "SBSQL-B9354B0054DA": "sb.aggregate.max",
    "SBSQL-139B7CCA5747": "sb.aggregate.bool_or",
    "SBSQL-EA0B4031CF59": "sb.aggregate.bool_or",
    "SBSQL-F0106211F6D0": "sb.aggregate.bool_or",
    "SBSQL-194319F6A97D": "sb.aggregate.bool_and",
    "SBSQL-B211A368B2D6": "sb.aggregate.bool_and",
    "SBSQL-FAED8812FBB7": "sb.aggregate.bool_and",
}


ORDERED_SET_BUILTINS = {
    "sb.aggregate.rank",
    "sb.aggregate.dense_rank",
    "sb.aggregate.percent_rank",
    "sb.aggregate.cume_dist",
    "sb.aggregate.approx_percentile_cont",
    "sb.aggregate.approx_percentile_disc",
    "sb.aggregate.percentile_cont",
    "sb.aggregate.percentile_disc",
    "sb.aggregate.mode",
    "sb.aggregate.listagg",
    "sb.aggregate.string_agg",
    "sb.aggregate.array_agg",
}


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def read_csv(path: Path) -> list[dict[str, str]]:
    if not path.is_file():
        fail(f"required CSV missing: {path}")
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def read_builtin_ids(path: Path) -> set[str]:
    if not path.is_file():
        fail(f"builtin registry missing: {path}")
    text = path.read_text(encoding="utf-8")
    return set(re.findall(r"^\s*-\s*builtin_id:\s*([A-Za-z0-9_.-]+)\s*$", text, re.MULTILINE))


def value_to_number(value: dict[str, Any]) -> float:
    if value.get("is_null"):
        raise ValueError("NULL value is not admitted in this fixture slice")
    if "int64_value" in value:
        return float(value["int64_value"])
    if "uint64_value" in value:
        return float(value["uint64_value"])
    if "real64_value" in value:
        return float(value["real64_value"])
    if "encoded_value" in value:
        return float(value["encoded_value"])
    raise ValueError(f"value has no numeric payload: {value!r}")


def value_to_text(value: dict[str, Any]) -> str:
    if value.get("is_null"):
        raise ValueError("NULL value has no text payload")
    if "text_value" in value:
        return str(value["text_value"])
    if "encoded_value" in value:
        return str(value["encoded_value"])
    if "int64_value" in value:
        return str(value["int64_value"])
    if "uint64_value" in value:
        return str(value["uint64_value"])
    if "real64_value" in value:
        return f"{float(value['real64_value']):.17g}"
    raise ValueError(f"value has no text payload: {value!r}")


def value_to_bool(value: dict[str, Any]) -> bool:
    text = value_to_text(value).lower()
    if text in {"true", "t", "yes", "y", "1"}:
        return True
    if text in {"false", "f", "no", "n", "0"}:
        return False
    raise ValueError(f"value has no boolean payload: {value!r}")


def value_to_json(value: dict[str, Any]) -> Any:
    if value.get("is_null"):
        return None
    if "int64_value" in value:
        return int(value["int64_value"])
    if "uint64_value" in value:
        return int(value["uint64_value"])
    if "real64_value" in value:
        return float(value["real64_value"])
    if "boolean_value" in value:
        return bool(value["boolean_value"])
    if "text_value" in value:
        return str(value["text_value"])
    if "encoded_value" in value:
        return str(value["encoded_value"])
    raise ValueError(f"value has no JSON-compatible payload: {value!r}")


def window_value_result(value: dict[str, Any], descriptor: str) -> tuple[str, str]:
    if value.get("is_null"):
        return "NULL", descriptor
    return value_to_text(value), descriptor


def metadata_int(metadata: dict[str, Any], key: str, default: int) -> int:
    value = metadata.get(key, default)
    if isinstance(value, dict):
        return int(value_to_number(value))
    return int(value)


def metadata_text(value: Any) -> str:
    if isinstance(value, str):
        return value
    if isinstance(value, dict):
        return value_to_text(value)
    raise ValueError(f"text metadata must be a string or value object: {value!r}")


def listagg_metadata(row: dict[str, str]) -> tuple[str, dict[str, Any]]:
    separator = ","
    overflow: dict[str, Any] = {}
    raw = (row.get("hypothetical_value_json", "") or "").strip()
    if not raw:
        return separator, overflow
    payload = json.loads(raw)
    if isinstance(payload, dict) and ("separator" in payload or "overflow" in payload):
        if "separator" in payload:
            separator = metadata_text(payload["separator"])
        raw_overflow = payload.get("overflow", {})
        if raw_overflow:
            if not isinstance(raw_overflow, dict):
                raise ValueError("LISTAGG overflow metadata must be a JSON object")
            overflow = raw_overflow
        return separator, overflow
    return value_to_text(payload), overflow


def byte_len(value: str) -> int:
    return len(value.encode("utf-8"))


def listagg_truncation_suffix(overflow: dict[str, Any], truncated_count: int) -> str:
    indicator = metadata_text(overflow.get("indicator", "..."))
    if bool(overflow.get("with_count", True)):
        indicator += f"({truncated_count})"
    return indicator


def apply_listagg_overflow(text_items: list[str],
                           separator: str,
                           overflow: dict[str, Any]) -> str:
    full_text = separator.join(text_items)
    mode = str(overflow.get("mode", "none")).lower()
    max_output_bytes = int(overflow.get("max_output_bytes", 0) or 0)
    if mode == "none" or max_output_bytes == 0 or byte_len(full_text) <= max_output_bytes:
        return full_text
    if mode == "error":
        raise ValueError("SB_DIAG_AGGREGATE_LISTAGG_OVERFLOW")
    if mode != "truncate":
        raise ValueError(f"unsupported LISTAGG overflow mode: {mode}")

    for retained in range(len(text_items), 0, -1):
        truncated_count = len(text_items) - retained
        if truncated_count == 0:
            continue
        candidate = separator.join(text_items[:retained])
        candidate += separator
        candidate += listagg_truncation_suffix(overflow, truncated_count)
        if byte_len(candidate) <= max_output_bytes:
            return candidate

    suffix_only = listagg_truncation_suffix(overflow, len(text_items))
    if byte_len(suffix_only) <= max_output_bytes:
        return suffix_only
    raise ValueError("SB_DIAG_AGGREGATE_LISTAGG_TRUNCATION_INDICATOR_TOO_LARGE")


def list_element_text(value: dict[str, Any]) -> str:
    if value.get("is_null"):
        return "NULL"
    if "int64_value" in value:
        return f"int64:{int(value['int64_value'])}"
    if "real64_value" in value:
        return f"real64:{value_to_number(value):.17g}"
    if "text_value" in value:
        return f"text:{value_to_text(value)}"
    if "bool_value" in value:
        return f"boolean:{'true' if value['bool_value'] else 'false'}"
    return f"any:{value_to_text(value)}"


def expected_window(row: dict[str, str]) -> tuple[str, str]:
    values = json.loads(row["ordered_values_json"])
    peers = json.loads(row["peer_groups_json"])
    current = int(row["current_row_index"])
    metadata: dict[str, Any] = {}
    raw_metadata = (row.get("hypothetical_value_json", "") or "").strip()
    if raw_metadata:
        decoded = json.loads(raw_metadata)
        if isinstance(decoded, dict):
            metadata = decoded
    if not isinstance(values, list) or not isinstance(peers, list):
        raise ValueError("window values and peer groups must be JSON lists")
    if len(values) != len(peers):
        raise ValueError("window values and peer groups length mismatch")
    if current < 0 or current >= len(values):
        raise ValueError("current_row_index outside partition")

    current_peer = int(peers[current])
    if current_peer == 0:
        rank = current + 1
        dense_rank = current + 1
        last_peer_index = current
    else:
        rank = next(index + 1 for index, peer in enumerate(peers) if int(peer) == current_peer)
        dense_rank = len({int(peer) for peer in peers[: current + 1]})
        last_peer_index = max(index for index, peer in enumerate(peers) if int(peer) == current_peer)

    canonical = row["canonical_builtin_id"]
    if canonical.endswith(".row_number"):
        return str(current + 1), "int64"
    if canonical.endswith(".rank") and not canonical.endswith(".dense_rank"):
        return str(rank), "int64"
    if canonical.endswith(".dense_rank"):
        return str(dense_rank), "int64"
    if canonical.endswith(".percent_rank"):
        result = 0.0 if len(values) <= 1 else (rank - 1) / (len(values) - 1)
        return f"{result:.17g}", "real64"
    if canonical.endswith(".cume_dist"):
        return f"{((last_peer_index + 1) / len(values)):.17g}", "real64"
    if canonical.endswith(".ntile"):
        bucket_count = metadata_int(metadata, "bucket_count", 1)
        if bucket_count <= 0:
            raise ValueError("ntile bucket_count must be positive for positive fixtures")
        row_number = current + 1
        base_size = len(values) // bucket_count
        larger_bucket_count = len(values) % bucket_count
        larger_bucket_size = base_size + 1
        if row_number <= larger_bucket_count * larger_bucket_size:
            bucket = math.ceil(row_number / larger_bucket_size)
        else:
            smaller_offset = row_number - (larger_bucket_count * larger_bucket_size)
            bucket = larger_bucket_count + math.ceil(smaller_offset / base_size)
        return str(bucket), "int64"
    if canonical.endswith(".lag") or canonical.endswith(".lead"):
        offset = metadata_int(metadata, "offset", 1)
        if offset <= 0:
            offset = 1
        target = current - offset if canonical.endswith(".lag") else current + offset
        if target < 0 or target >= len(values):
            if "default_value" in metadata:
                return window_value_result(metadata["default_value"], row.get("expected_result_descriptor", "text") or "text")
            return "NULL", row.get("expected_result_descriptor", "text") or "text"
        return window_value_result(values[target], row.get("expected_result_descriptor", "text") or "text")
    if canonical.endswith(".first_value") or canonical.endswith(".last_value") or canonical.endswith(".nth_value"):
        frame_start = metadata_int(metadata, "frame_start_index", 0)
        frame_end = metadata_int(metadata, "frame_end_exclusive", len(values))
        descriptor = row.get("expected_result_descriptor", "text") or "text"
        if frame_start < 0 or frame_end > len(values) or frame_start > frame_end:
            raise ValueError("window value fixture frame metadata is invalid")
        if frame_start == frame_end:
            return "NULL", descriptor
        if canonical.endswith(".first_value"):
            return window_value_result(values[frame_start], descriptor)
        if canonical.endswith(".last_value"):
            return window_value_result(values[frame_end - 1], descriptor)
        nth = metadata_int(metadata, "nth", 1)
        if nth <= 0:
            raise ValueError("nth_value n must be positive for positive fixtures")
        target = frame_start + nth - 1
        if target >= frame_end:
            return "NULL", descriptor
        return window_value_result(values[target], descriptor)
    raise ValueError(f"unsupported window canonical: {canonical}")


def expected_ordered_set(row: dict[str, str]) -> tuple[str, str]:
    values = [value_to_number(v) for v in json.loads(row["ordered_values_json"])]
    hypothetical = value_to_number(json.loads(row["hypothetical_value_json"]))
    less = [value for value in values if value < hypothetical]
    less_or_equal = [value for value in values if value <= hypothetical]
    rank = len(less) + 1
    dense_rank = len(set(less)) + 1
    canonical = row["canonical_builtin_id"]
    if canonical == "sb.aggregate.rank":
        return str(rank), "int64"
    if canonical == "sb.aggregate.dense_rank":
        return str(dense_rank), "int64"
    if canonical == "sb.aggregate.percent_rank":
        result = 0.0 if not values else (rank - 1) / len(values)
        return f"{result:.17g}", "real64"
    if canonical == "sb.aggregate.cume_dist":
        return f"{((len(less_or_equal) + 1) / (len(values) + 1)):.17g}", "real64"
    raise ValueError(f"unsupported ordered-set canonical: {canonical}")


def percentile_cont(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    if not ordered:
        raise ValueError("percentile input must not be empty")
    scaled = fraction * (len(ordered) - 1)
    lower = math.floor(scaled)
    upper = math.ceil(scaled)
    if lower == upper:
        return ordered[lower]
    weight = scaled - lower
    return ordered[lower] + ((ordered[upper] - ordered[lower]) * weight)


def percentile_disc(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    if not ordered:
        raise ValueError("percentile input must not be empty")
    index = 0 if fraction <= 0 else math.ceil(fraction * len(ordered)) - 1
    return ordered[max(0, min(index, len(ordered) - 1))]


def expected_corr(values: list[Any]) -> tuple[str, str]:
    pairs: list[tuple[float, float]] = []
    for value in values:
        if not isinstance(value, list) or len(value) != 2:
            raise ValueError(f"corr fixture requires [y,x] pairs: {value!r}")
        y_value, x_value = value
        if not isinstance(y_value, dict) or not isinstance(x_value, dict):
            raise ValueError(f"corr pair entries must be value objects: {value!r}")
        if y_value.get("is_null") or x_value.get("is_null"):
            continue
        pairs.append((value_to_number(y_value), value_to_number(x_value)))
    if len(pairs) < 2:
        return "NULL", "real64"
    mean_y = sum(y for y, _ in pairs) / len(pairs)
    mean_x = sum(x for _, x in pairs) / len(pairs)
    m2_y = sum((y - mean_y) ** 2 for y, _ in pairs)
    m2_x = sum((x - mean_x) ** 2 for _, x in pairs)
    if m2_y <= 0.0 or m2_x <= 0.0:
        return "NULL", "real64"
    comoment = sum((y - mean_y) * (x - mean_x) for y, x in pairs)
    return f"{(comoment / math.sqrt(m2_y * m2_x)):.17g}", "real64"


def expected_pair_stat(values: list[Any], canonical: str) -> tuple[str, str]:
    pairs: list[tuple[float, float]] = []
    for value in values:
        if not isinstance(value, list) or len(value) != 2:
            raise ValueError(f"paired statistical fixture requires [y,x] pairs: {value!r}")
        y_value, x_value = value
        if not isinstance(y_value, dict) or not isinstance(x_value, dict):
            raise ValueError(f"paired entries must be value objects: {value!r}")
        if y_value.get("is_null") or x_value.get("is_null"):
            continue
        pairs.append((value_to_number(y_value), value_to_number(x_value)))
    if canonical == "sb.aggregate.regr_count":
        return str(len(pairs)), "int64"
    if not pairs:
        return "NULL", "real64"
    mean_y = sum(y for y, _ in pairs) / len(pairs)
    mean_x = sum(x for _, x in pairs) / len(pairs)
    m2_y = sum((y - mean_y) ** 2 for y, _ in pairs)
    m2_x = sum((x - mean_x) ** 2 for _, x in pairs)
    comoment = sum((y - mean_y) * (x - mean_x) for y, x in pairs)
    if canonical == "sb.aggregate.covar_pop":
        return f"{(comoment / len(pairs)):.17g}", "real64"
    if canonical == "sb.aggregate.covar_samp":
        if len(pairs) < 2:
            return "NULL", "real64"
        return f"{(comoment / (len(pairs) - 1)):.17g}", "real64"
    if canonical == "sb.aggregate.regr_avgx":
        return f"{mean_x:.17g}", "real64"
    if canonical == "sb.aggregate.regr_avgy":
        return f"{mean_y:.17g}", "real64"
    if canonical == "sb.aggregate.regr_sxx":
        return f"{m2_x:.17g}", "real64"
    if canonical == "sb.aggregate.regr_sxy":
        return f"{comoment:.17g}", "real64"
    if canonical == "sb.aggregate.regr_syy":
        return f"{m2_y:.17g}", "real64"
    if canonical == "sb.aggregate.regr_slope":
        if m2_x == 0.0:
            return "NULL", "real64"
        return f"{(comoment / m2_x):.17g}", "real64"
    if canonical == "sb.aggregate.regr_intercept":
        if m2_x == 0.0:
            return "NULL", "real64"
        slope = comoment / m2_x
        return f"{(mean_y - mean_x * slope):.17g}", "real64"
    if canonical == "sb.aggregate.regr_r2":
        if m2_x == 0.0:
            return "NULL", "real64"
        if m2_y == 0.0:
            return "1", "real64"
        return f"{((comoment * comoment) / (m2_x * m2_y)):.17g}", "real64"
    raise ValueError(f"unsupported paired statistical canonical: {canonical}")


def expected_aggregate(row: dict[str, str]) -> tuple[str, str]:
    values = json.loads(row["ordered_values_json"])
    if not isinstance(values, list):
        raise ValueError("aggregate values must be a JSON list")
    canonical = row["canonical_builtin_id"]
    if canonical == "sb.aggregate.corr":
        return expected_corr(values)
    if canonical in {
        "sb.aggregate.covar_pop",
        "sb.aggregate.covar_samp",
        "sb.aggregate.regr_avgx",
        "sb.aggregate.regr_avgy",
        "sb.aggregate.regr_count",
        "sb.aggregate.regr_intercept",
        "sb.aggregate.regr_r2",
        "sb.aggregate.regr_slope",
        "sb.aggregate.regr_sxx",
        "sb.aggregate.regr_sxy",
        "sb.aggregate.regr_syy",
    }:
        return expected_pair_stat(values, canonical)
    non_null = [
        value for value in values
        if isinstance(value, dict) and not value.get("is_null")
    ]
    if canonical == "sb.aggregate.count":
        if row.get("case_kind") == "positive_count_star" or "count(*)" in row.get("function_id", "").lower():
            return str(len(values)), "int64"
        return str(len(non_null)), "int64"
    if canonical == "sb.aggregate.avg":
        if not non_null:
            return "NULL", "real64"
        numbers = [value_to_number(value) for value in non_null]
        return f"{(sum(numbers) / len(numbers)):.17g}", "real64"
    if canonical in {"sb.aggregate.min", "sb.aggregate.max"}:
        descriptor = row.get("expected_result_descriptor", "") or "int64"
        if not non_null:
            return "NULL", descriptor
        ordered = sorted(non_null, key=value_to_number)
        selected = ordered[0] if canonical == "sb.aggregate.min" else ordered[-1]
        if descriptor == "real64":
            return f"{value_to_number(selected):.17g}", descriptor
        return value_to_text(selected), descriptor
    if canonical in {"sb.aggregate.bool_or", "sb.aggregate.bool_and"}:
        if not non_null:
            return "NULL", "boolean"
        bool_values = [value_to_bool(value) for value in non_null]
        if canonical == "sb.aggregate.bool_or":
            return "TRUE" if any(bool_values) else "FALSE", "boolean"
        return "TRUE" if all(bool_values) else "FALSE", "boolean"
    if canonical == "sb.aggregate.approx_count_distinct":
        return str(len({value_to_text(value) for value in non_null})), "int64"
    if canonical in {"sb.aggregate.approx_median", "sb.aggregate.approx_percentile_cont", "sb.aggregate.percentile_cont"}:
        if not non_null:
            return "NULL", "real64"
        fraction = 0.5
        if row.get("hypothetical_value_json"):
            fraction = value_to_number(json.loads(row["hypothetical_value_json"]))
        return f"{percentile_cont([value_to_number(value) for value in non_null], fraction):.17g}", "real64"
    if canonical in {"sb.aggregate.approx_percentile_disc", "sb.aggregate.percentile_disc"}:
        if not non_null:
            return "NULL", "real64"
        fraction = 0.5
        if row.get("hypothetical_value_json"):
            fraction = value_to_number(json.loads(row["hypothetical_value_json"]))
        return f"{percentile_disc([value_to_number(value) for value in non_null], fraction):.17g}", "real64"
    if canonical == "sb.aggregate.approx_top_k":
        limit = 10
        if row.get("hypothetical_value_json"):
            limit = int(value_to_number(json.loads(row["hypothetical_value_json"])))
        counts: dict[str, int] = {}
        for value in non_null:
            text = value_to_text(value)
            counts[text] = counts.get(text, 0) + 1
        ordered = sorted(counts.items(), key=lambda item: (-item[1], item[0]))[:limit]
        payload = [{"value": value, "count": count} for value, count in ordered]
        return json.dumps(payload, separators=(",", ":")), "json"
    if canonical == "sb.aggregate.json_agg":
        if not values:
            return "NULL", "json"
        return json.dumps([value_to_json(value) for value in values], separators=(",", ":")), "json"
    if canonical == "sb.aggregate.json_object_agg":
        if not values:
            return "NULL", "json"
        items: list[tuple[str, Any]] = []
        for value in values:
            if (not isinstance(value, list)) or len(value) != 2:
                raise ValueError(f"json_object_agg fixture requires [key,value] pairs: {value!r}")
            key_value, object_value = value
            if not isinstance(key_value, dict) or not isinstance(object_value, dict):
                raise ValueError(f"json_object_agg pair entries must be value objects: {value!r}")
            if key_value.get("is_null"):
                raise ValueError("json_object_agg positive fixture cannot use a NULL key")
            key_text = value_to_text(key_value)
            items = [item for item in items if item[0] != key_text]
            items.append((key_text, value_to_json(object_value)))
        return json.dumps(dict(items), separators=(",", ":")), "json"
    if canonical == "sb.aggregate.array_agg":
        descriptor = row.get("expected_result_descriptor", "") or "list<any nullable>"
        if not values:
            return "NULL", descriptor
        return "list[" + ";".join(list_element_text(value) for value in values) + "]", descriptor
    if canonical == "sb.aggregate.mode":
        if not non_null:
            return "NULL", "real64"
        counts: dict[str, int] = {}
        for value in non_null:
            text = value_to_text(value)
            counts[text] = counts.get(text, 0) + 1
        mode_value = sorted(counts.items(), key=lambda item: (-item[1], item[0]))[0][0]
        return mode_value, row.get("expected_result_descriptor", "real64") or "real64"
    if canonical in {"sb.aggregate.listagg", "sb.aggregate.string_agg"}:
        separator, overflow = listagg_metadata(row)
        return apply_listagg_overflow([value_to_text(value) for value in non_null], separator, overflow), "text"
    if canonical == "sb.aggregate.every":
        if not non_null:
            return "NULL", "boolean"
        for value in non_null:
            text = value_to_text(value).lower()
            if text in {"false", "f", "no", "n", "0"}:
                return "FALSE", "boolean"
            if text not in {"true", "t", "yes", "y", "1"}:
                raise ValueError(f"boolean aggregate input invalid: {value!r}")
        return "TRUE", "boolean"
    if canonical in {"sb.aggregate.stddev", "sb.aggregate.stddev_samp",
                     "sb.aggregate.variance", "sb.aggregate.variance_samp"}:
        numbers = [value_to_number(value) for value in non_null]
        if len(numbers) < 2:
            return "NULL", "real64"
        mean = sum(numbers) / len(numbers)
        sample_variance = sum((number - mean) ** 2 for number in numbers) / (len(numbers) - 1)
        if canonical in {"sb.aggregate.variance", "sb.aggregate.variance_samp"}:
            return f"{sample_variance:.17g}", "real64"
        return f"{math.sqrt(sample_variance):.17g}", "real64"
    if canonical in {"sb.aggregate.stddev_pop", "sb.aggregate.variance_pop"}:
        numbers = [value_to_number(value) for value in non_null]
        if not numbers:
            return "NULL", "real64"
        mean = sum(numbers) / len(numbers)
        population_variance = sum((number - mean) ** 2 for number in numbers) / len(numbers)
        if canonical == "sb.aggregate.variance_pop":
            return f"{population_variance:.17g}", "real64"
        return f"{math.sqrt(population_variance):.17g}", "real64"
    raise ValueError(f"unsupported aggregate canonical: {canonical}")


def expected_negative(row: dict[str, str]) -> None:
    expected_diagnostic = (row.get("expected_diagnostic_code", "") or "").strip()
    canonical = row.get("canonical_builtin_id")
    if canonical == "sb.window.ntile":
        if expected_diagnostic != "SB_DIAG_WINDOW_NTILE_BUCKET_INVALID":
            raise ValueError(f"unsupported NTILE diagnostic fixture: {expected_diagnostic}")
        metadata = json.loads(row.get("hypothetical_value_json", "") or "{}")
        if not isinstance(metadata, dict) or metadata_int(metadata, "bucket_count", 1) != 0:
            raise ValueError("NTILE diagnostic fixture must set bucket_count=0")
        return
    if canonical == "sb.window.nth_value":
        if expected_diagnostic != "SB_DIAG_WINDOW_NTH_VALUE_INVALID":
            raise ValueError(f"unsupported NTH_VALUE diagnostic fixture: {expected_diagnostic}")
        metadata = json.loads(row.get("hypothetical_value_json", "") or "{}")
        if not isinstance(metadata, dict) or metadata_int(metadata, "nth", 1) != 0:
            raise ValueError("NTH_VALUE diagnostic fixture must set nth=0")
        return
    if canonical == "sb.aggregate.avg":
        if expected_diagnostic != "SBSQL.QUERY.GROUP_ROUTE_UNSUPPORTED":
            raise ValueError(f"unsupported AVG diagnostic fixture: {expected_diagnostic}")
        if "distinct" not in row.get("function_id", "").lower():
            raise ValueError("AVG grouped-route diagnostic fixture must be a DISTINCT form")
        if row.get("expected_result_value", "") != "ERROR":
            raise ValueError("AVG grouped-route diagnostic fixture must set expected_result_value=ERROR")
        if row.get("expected_result_descriptor", "") != "real64":
            raise ValueError("AVG grouped-route diagnostic fixture must retain real64 descriptor evidence")
        if "avg_distinct_requires_distinct_aggregate_execution_route" not in row.get("oracle_authority_ref", ""):
            raise ValueError("AVG grouped-route diagnostic fixture missing precise DISTINCT route reason")
        return
    if canonical != "sb.aggregate.listagg":
        raise ValueError(f"unsupported negative aggregate/window fixture: {canonical}")
    if expected_diagnostic != "SB_DIAG_AGGREGATE_LISTAGG_OVERFLOW":
        raise ValueError(f"unsupported LISTAGG diagnostic fixture: {expected_diagnostic}")
    values = json.loads(row["ordered_values_json"])
    non_null = [value for value in values if not value.get("is_null")]
    separator, overflow = listagg_metadata(row)
    if str(overflow.get("mode", "")).lower() != "error":
        raise ValueError("LISTAGG overflow diagnostic fixture must use mode=error")
    max_output_bytes = int(overflow.get("max_output_bytes", 0) or 0)
    full_text = separator.join(value_to_text(value) for value in non_null)
    if max_output_bytes == 0 or byte_len(full_text) <= max_output_bytes:
        raise ValueError("LISTAGG overflow diagnostic fixture does not exceed max_output_bytes")


def values_match(actual: str, expected: str, descriptor: str) -> bool:
    if descriptor == "real64":
        return math.isclose(float(actual), float(expected), rel_tol=0.0, abs_tol=1e-15)
    return actual == expected


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    args = parser.parse_args()
    root = Path(args.repo_root).resolve()

    surfaces = {row["surface_id"]: row for row in read_csv(root / SURFACE_REGISTRY)}
    fixtures = read_csv(root / FIXTURES)
    builtin_ids = set()
    for registry in [BUILTIN_EXPRESSION_REGISTRY, BUILTIN_WINDOW_REGISTRY, BUILTIN_AGGREGATE_REGISTRY]:
      builtin_ids.update(read_builtin_ids(root / registry))
    expression_builtin_ids = read_builtin_ids(root / BUILTIN_EXPRESSION_REGISTRY)
    alias_source = (root / WINDOW_ALIAS_SOURCE).read_text(encoding="utf-8").lower()
    aggregate_general_source = (root / AGGREGATE_GENERAL_SOURCE).read_text(encoding="utf-8")
    eval_source = (root / WINDOW_EVAL_SOURCE).read_text(encoding="utf-8")
    query_plan_source = (root / QUERY_PLAN_SOURCE).read_text(encoding="utf-8")

    errors: list[str] = []
    seen_fixture_ids: set[str] = set()
    seen_surfaces: set[str] = set()

    if "SB_DIAG_ORDERED_SET_HYPOTHETICAL_VALUE_REQUIRED" not in eval_source:
        errors.append("runtime missing ordered-set hypothetical value diagnostic")
    if "less_or_equal_count + 1" not in eval_source:
        errors.append("runtime missing cume_dist hypothetical-row inclusion")
    if "PercentileResultValue" not in eval_source or "DiscretePercentileResultValue" not in eval_source:
        errors.append("runtime missing continuous/discrete percentile finalizers")
    if "ModeResultValue" not in eval_source:
        errors.append("runtime missing mode finalizer")
    if "sb_diag_aggregate_percentile_fraction_invalid" not in alias_source:
        errors.append("runtime missing invalid percentile-fraction diagnostic")
    if "sb_diag_aggregate_top_k_limit_invalid" not in alias_source:
        errors.append("runtime missing invalid top-k limit diagnostic")
    if "SB_DIAG_AGGREGATE_LISTAGG_OVERFLOW" not in aggregate_general_source:
        errors.append("runtime missing LISTAGG ON OVERFLOW ERROR diagnostic")
    if "SblrListAggOverflowMode::truncate" not in aggregate_general_source:
        errors.append("runtime missing LISTAGG ON OVERFLOW TRUNCATE finalizer")
    if "ApplyJsonArrayInput" not in aggregate_general_source or "JsonArrayResult" not in aggregate_general_source:
        errors.append("runtime missing json_agg JSON array state/finalizer")
    if "JsonAggAggregateResultShape" not in query_plan_source:
        errors.append("query plan runtime missing bounded JSON_AGG grouped route result shape")
    if "ArrayAggAggregateResultShape" not in query_plan_source or "ListDescriptor" not in query_plan_source:
        errors.append("query plan runtime missing bounded ARRAY_AGG list result shape")
    if "SB_DIAG_WINDOW_NTILE_BUCKET_INVALID" not in eval_source:
        errors.append("runtime missing NTILE positive bucket diagnostic")
    if "larger_bucket_count" not in eval_source:
        errors.append("runtime missing NTILE larger-buckets-first distribution")
    if "SB_DIAG_WINDOW_NTH_VALUE_INVALID" not in eval_source:
        errors.append("runtime missing NTH_VALUE positive index diagnostic")

    for row in fixtures:
        fid = row.get("fixture_id", "")
        if not fid:
            errors.append("fixture row missing fixture_id")
            continue
        if fid in seen_fixture_ids:
            errors.append(f"duplicate fixture_id: {fid}")
        seen_fixture_ids.add(fid)

        for column in REQUIRED_COLUMNS:
            if not (row.get(column, "") or "").strip():
                errors.append(f"{fid}: required column {column} is empty")

        surface_id = row.get("surface_id", "")
        if surface_id not in surfaces:
            errors.append(f"{fid}: surface_id {surface_id} not in canonical surface registry")
        if surface_id not in EXPECTED_SURFACES:
            errors.append(f"{fid}: surface_id {surface_id} is outside the SBSFC-015 target set")
        seen_surfaces.add(surface_id)

        canonical = row.get("canonical_builtin_id", "")
        expected_canonical = EXPECTED_SURFACES.get(surface_id)
        if expected_canonical and canonical != expected_canonical:
            errors.append(f"{fid}: canonical_builtin_id {canonical} != expected {expected_canonical}")
        if canonical not in builtin_ids:
            errors.append(f"{fid}: canonical_builtin_id {canonical} not present in canonical builtin registries")
        if canonical in ORDERED_SET_BUILTINS and canonical not in expression_builtin_ids:
            errors.append(f"{fid}: ordered-set canonical {canonical} missing from builtin-expression-registry.yaml")

        function_id = row.get("function_id", "")
        string_agg_query_route = (
            canonical == "sb.aggregate.string_agg"
            and "string_agg" in query_plan_source
            and "ListAggAggregateResultShape" in query_plan_source
        )
        array_agg_query_route = (
            canonical == "sb.aggregate.array_agg"
            and "array_agg" in query_plan_source
            and "ArrayAggAggregateResultShape" in query_plan_source
        )
        if function_id.lower() not in alias_source and not string_agg_query_route and not array_agg_query_route:
            errors.append(f"{fid}: function_id {function_id} not recognized by aggregate/window runtime resolver source")

        expected_diagnostic = (row.get("expected_diagnostic_code", "") or "").strip()
        if expected_diagnostic:
            try:
                expected_negative(row)
            except (json.JSONDecodeError, ValueError, KeyError) as exc:
                errors.append(f"{fid}: negative fixture oracle input invalid: {exc}")
            continue

        try:
            values = json.loads(row["ordered_values_json"])
            if not isinstance(values, list):
                errors.append(f"{fid}: ordered_values_json must be a JSON list")
            mode = row.get("evaluation_mode", "")
            if mode == "window":
                expected_value, expected_descriptor = expected_window(row)
            elif mode == "ordered_set":
                expected_value, expected_descriptor = expected_ordered_set(row)
            elif mode == "aggregate":
                expected_value, expected_descriptor = expected_aggregate(row)
            else:
                errors.append(f"{fid}: unsupported evaluation_mode {mode}")
                continue
        except (json.JSONDecodeError, ValueError, KeyError) as exc:
            errors.append(f"{fid}: fixture oracle input invalid: {exc}")
            continue

        descriptor = row.get("expected_result_descriptor", "")
        if descriptor != expected_descriptor:
            errors.append(f"{fid}: expected_result_descriptor {descriptor} != oracle {expected_descriptor}")
        if expected_value == "NULL":
            if row.get("expected_result_value", "") != "NULL":
                errors.append(f"{fid}: expected_result_value {row.get('expected_result_value', '')} != oracle NULL")
        elif not values_match(row.get("expected_result_value", ""), expected_value, expected_descriptor):
            errors.append(
                f"{fid}: expected_result_value {row.get('expected_result_value', '')} != oracle {expected_value}"
            )
    missing = sorted(set(EXPECTED_SURFACES) - seen_surfaces)
    for surface_id in missing:
        errors.append(f"missing required SBSFC-015 surface fixture: {surface_id}")

    print(
        "sbsql_sbsfc_015_aggregate_window_fixture_gate "
        f"fixtures={len(fixtures)} "
        f"surfaces={len(seen_surfaces)} "
        f"errors={len(errors)}"
    )
    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
