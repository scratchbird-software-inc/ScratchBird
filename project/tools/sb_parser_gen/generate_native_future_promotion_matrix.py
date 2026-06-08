#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Generate NATIVE_FUTURE_PROMOTION_MATRIX.csv for the SBsql Surface-to-SBLR execution_plan.

Inputs (repo-local, no network):
  public_input_snapshot

Output:
  project/tests/sbsql_parser_worker/fixtures/surface_to_sblr/artifacts/NATIVE_FUTURE_PROMOTION_MATRIX.csv

For every `native_future` row in the canonical SBsql surface registry, this
generator records the implementation decision now in force:

- Rows in `cluster_scope=cluster_private` are routed through the compile-gated
  cluster provider boundary. Default/open-core builds must return the exact
  no-cluster error vector, and cluster-enabled builds must return the external
  provider/stub result until the private provider is linked.
- All other `native_future` rows are implementation backlog. The canonical
  status must change to `native_now` and the row must reach `e2e_passed`
  through P2 implementation slices (SBSFC-010..SBSFC-016) plus full-route
  fixtures.

The project decision is "implement all"; this matrix no longer records
coordinator-default optional removal/deferral language for these rows.

Owner assignment for `promote` rows uses a name-pattern heuristic across the
function-implementation lanes declared in `AGENT_WRITE_SCOPE_REGISTER.csv`:
numeric/math, text/temporal/regex, multimodel/json/xml/array/spatial/vector,
aggregate/window, procedural/diagnostic. Names that do not match any lane
default to `function_oracle_worker` for SBSFC-004 oracle-driven sub-routing.

Architecture invariant compliance: read-only CSV generation; no transaction
model touched; no engine, parser worker, server, listener, storage, or MGA
file modified; no WAL surface introduced. The matrix is a coordinator
decision record over canonical metadata only.
"""

from __future__ import annotations

import argparse
import csv
import re
import sys
from collections import Counter
from pathlib import Path


REGISTRY = "public_input_snapshot"
DEFAULT_ARTIFACT_ROOT = "project/tests/sbsql_parser_worker/fixtures/surface_to_sblr/artifacts"
OUTPUT_NAME = "NATIVE_FUTURE_PROMOTION_MATRIX.csv"
DECISION_AUTHORITY = "user-directive-2026-05-18-implement-all"


COLUMNS = [
    "surface_id",
    "canonical_name",
    "current_status",
    "surface_kind",
    "family",
    "sblr_operation_family",
    "cluster_scope",
    "promotion_decision",
    "decision_authority",
    "canonical_spec_change_required",
    "implementation_owner",
    "implementation_target",
    "required_tests",
    "decision_status",
    "notes",
]


NUMERIC_TOKEN_RE = re.compile(
    r"^(abs|sign|round|ceil|floor|trunc|mod|power|pow|exp|log\d*|ln|sqrt|cbrt|sin|cos|tan|asin|acos|atan2?|sinh|cosh|tanh|degrees|radians|pi|gcd|lcm|factorial|bit_(and|or|xor|count|length|not|shift_left|shift_right)|bitnot|bitand|bitor|bitxor|bitcount|hash_int\d*|width_bucket|greatest|least|div|negate|plus|minus|times|divide|hex_to_int|int_to_hex|to_hex|from_hex|decimal_|numeric_|float_|real_|double_|int\d+_|uint\d+_|big_integer|safe_add|safe_sub|safe_mul|safe_div|isnan|isinf|isfinite|copysign|fma|nextafter|atan|asinh|acosh|atanh|coth|sech|csch)\b",
    re.IGNORECASE,
)
TEXT_TEMPORAL_RE = re.compile(
    r"^(length|char_length|character_length|octet_length|bit_length|position|locate|substring|substr|trim|btrim|ltrim|rtrim|upper|lower|initcap|concat|replace|repeat|reverse|left|right|lpad|rpad|format|to_char|to_date|to_timestamp|to_number|to_hex|chr|ascii|unicode|regex|re_|translate|overlay|split|string_to|array_to_string|to_ascii|md5|sha\d+|encode|decode|convert|charset|collate|like|ilike|similar|matches|now|current_date|current_time|current_timestamp|local_time|local_timestamp|extract|date_part|date_trunc|date_add|date_sub|date_diff|age|epoch_to|to_epoch|interval_|year|month|day|hour|minute|second|millisecond|microsecond|nanosecond|week|quarter|century|decade|millennium|isoyear|isodow|dow|doy|julian|timezone|at_time_zone|add_months|months_between|next_day|last_day|first_day|format_date|format_time|format_timestamp|parse_date|parse_time|parse_timestamp|unix_timestamp|from_unixtime|sec_to_time|time_to_sec|str_to_date|date_format|time_format|datetime_|temporal_|interval_|duration_)\b",
    re.IGNORECASE,
)
MULTIMODEL_RE = re.compile(
    r"^(json_|jsonb_|jsonpath_|xml_|xpath|xmlexists|xmlcast|xmlattributes|xmlelement|xmlforest|xmlpi|xmlroot|xmlconcat|xmlcomment|xmlagg|xmltable|array_|hstore_|range_|map_|set_|st_|geo_|spatial_|geom_|geometry_|geography_|vector_|cosine_|dot_|euclid_|l2_|distance_|search_|fts_|full_text_|tsvector|tsquery|graph_|cypher_|sparql_|document_|object_|tuple_|list_|approx_|topk_|hll_|sketch_|bloom_|cardinality_|quantile_|percentile_)\b",
    re.IGNORECASE,
)
AGGREGATE_WINDOW_RE = re.compile(
    r"^(sum|avg|count|min|max|listagg|string_agg|array_agg|json_agg|bool_and|bool_or|every|bit_and|bit_or|bit_xor|var|stddev|variance|covar|corr|regr_|grouping|rank|dense_rank|row_number|percent_rank|cume_dist|ntile|lag|lead|first_value|last_value|nth_value|window_|over_|frame_)\b",
    re.IGNORECASE,
)
PROCEDURAL_RE = re.compile(
    r"^(signal|resignal|raise|fetch|open|close|cursor|exception|when|continue|exit|leave|loop|while|repeat|until|return|return_set|return_next|perform|execute|prepare|deallocate|get_diagnostics|stacked|condition|sqlstate|sqlcode|sqlerrm|sqlerror|sqlwarning|not_found|found|row_count|notice|debug|info|warning|error|log_message|trace_|profile_|assert_)\b",
    re.IGNORECASE,
)


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def read_csv(path: Path) -> list[dict[str, str]]:
    if not path.is_file():
        fail(f"required CSV missing: {path}")
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def assign_owner(canonical_name: str) -> tuple[str, str]:
    """Return (owner_lane, implementation_target) by name-pattern heuristic."""
    name = canonical_name.strip()
    # Normalize for regex matching (strip leading non-alpha)
    norm = re.sub(r"^[^A-Za-z]+", "", name)

    if NUMERIC_TOKEN_RE.match(norm):
        return ("function_numeric_worker", "project/src/engine/functions/families")
    if TEXT_TEMPORAL_RE.match(norm):
        return ("function_text_temporal_worker", "project/src/engine/functions/families")
    if MULTIMODEL_RE.match(norm):
        return ("function_multimodel_worker", "project/src/engine/functions/families")
    if AGGREGATE_WINDOW_RE.match(norm):
        return ("function_aggregate_window_worker", "project/src/engine/functions/families")
    if PROCEDURAL_RE.match(norm):
        return ("function_procedural_worker", "project/src/engine/functions/families")
    return ("function_oracle_worker", "project/src/engine/functions/families")


def classify_row(row: dict[str, str]) -> dict[str, str]:
    surface_id = row["surface_id"]
    name = row["canonical_name"]
    cluster_scope = row["cluster_scope"]

    if cluster_scope == "cluster_private":
        return {
            "promotion_decision": "private_profile_gate",
            "decision_authority": DECISION_AUTHORITY,
            "canonical_spec_change_required": "yes_status_to_cluster_private_in_canonical_registry_status_matrix_operation_matrix_coverage_yaml_inventory",
            "implementation_owner": "cluster_profile_worker",
            "implementation_target": "project/src/cluster_provider;project/src/cluster_provider_stub;private_cluster_provider_when_linked",
            "required_tests": f"project/tests/sbsql_parser_worker/generated/full_surface/cluster_private_refusal/{surface_id}.fixture.yaml",
            "decision_status": "implementation_required_by_user_directive",
            "notes": "native_future row carrying cluster_scope=cluster_private; implement through the compile-gated cluster provider boundary. Default/open-core builds must return SBLR.CLUSTER.SUPPORT_NOT_ENABLED from the no-cluster provider; cluster-enabled builds must return cluster.provider.stub.v1/SBLR.CLUSTER.STUB_RESPONSE from the separate provider target until the private provider is linked.",
        }

    owner, target = assign_owner(name)
    return {
        "promotion_decision": "promote",
        "decision_authority": DECISION_AUTHORITY,
        "canonical_spec_change_required": "yes_status_to_native_now_in_canonical_registry_status_matrix_operation_matrix_coverage_yaml_inventory",
        "implementation_owner": owner,
        "implementation_target": target,
        "required_tests": f"project/tests/sbsql_parser_worker/generated/full_surface/promoted_native_future/{surface_id}.fixture.yaml",
        "decision_status": "implementation_required_by_user_directive",
        "notes": "native_future row selected for implementation: implement under SBLR expression runtime family and reach e2e_passed via P2 slices SBSFC-010..SBSFC-016 with row-level parser, lowering, server, engine, and fixture evidence.",
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--artifact-root", default=DEFAULT_ARTIFACT_ROOT)
    args = parser.parse_args()
    root = Path(args.repo_root)
    artifact_root = Path(args.artifact_root)
    if not artifact_root.is_absolute():
        artifact_root = root / artifact_root

    surfaces = read_csv(root / REGISTRY)
    native_future = [r for r in surfaces if r["status"] == "native_future"]

    if not native_future:
        output_path = artifact_root / OUTPUT_NAME
        output_path.parent.mkdir(parents=True, exist_ok=True)
        with output_path.open("w", newline="", encoding="utf-8") as handle:
            writer = csv.DictWriter(handle, fieldnames=COLUMNS, lineterminator="\n")
            writer.writeheader()
        print("native_future_promotion_matrix=generated rows=0")
        return 0

    decision_counts: Counter[str] = Counter()
    owner_counts: Counter[str] = Counter()
    output_rows: list[dict[str, str]] = []

    for surface in sorted(native_future, key=lambda r: r["surface_id"]):
        classification = classify_row(surface)
        output_rows.append({
            "surface_id": surface["surface_id"],
            "canonical_name": surface["canonical_name"],
            "current_status": surface["status"],
            "surface_kind": surface["surface_kind"],
            "family": surface["family"],
            "sblr_operation_family": surface["sblr_operation_family"],
            "cluster_scope": surface["cluster_scope"],
            **classification,
        })
        decision_counts[classification["promotion_decision"]] += 1
        owner_counts[classification["implementation_owner"]] += 1

    output_path = artifact_root / OUTPUT_NAME
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=COLUMNS, lineterminator="\n")
        writer.writeheader()
        writer.writerows(output_rows)

    print(
        "native_future_promotion_matrix=generated "
        f"rows={len(output_rows)} "
        + " ".join(f"{d}={c}" for d, c in sorted(decision_counts.items()))
        + " "
        + " ".join(f"{o}={c}" for o, c in sorted(owner_counts.items()))
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
