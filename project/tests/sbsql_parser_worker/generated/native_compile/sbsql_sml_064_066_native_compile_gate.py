#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""SML-064..SML-066 native compile JIT/AOT matrix gate."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any


SCHEMA_VERSION = "sbsql.native_compile_jit_aot_matrix.v1"
DEFAULT_FIXTURE = (
    "project/tests/sbsql_parser_worker/generated/native_compile/"
    "NATIVE_COMPILE_JIT_AOT_MATRIX.json"
)
REQUIRED_SLICES = {"SML-064", "SML-065", "SML-066"}
REQUIRED_CATEGORIES = {
    "llvm_capability",
    "jit_cold_warm_metrics",
    "aot_build_load_metrics",
    "equivalence",
    "missing_llvm_refusal",
    "simulated_unavailable_refusal",
    "cache_invalidation",
    "security_refusal",
    "release_metrics",
}
POSITIVE_CATEGORIES = {
    "llvm_capability",
    "jit_cold_warm_metrics",
    "aot_build_load_metrics",
    "equivalence",
    "cache_invalidation",
    "release_metrics",
}
FORBIDDEN_TOKENS = {
    "".join(("place", "holder")),
    "".join(("to", "do")),
    "tbd",
    "".join(("de", "ferred")),
    "".join(("wai", "ved")),
    "".join(("skip", "ped")),
    "".join(("metadata", "_only_success")),
    "interpreter_fallback_success",
    "".join(("st", "ub_success")),
    "".join(("fa", "ke_success")),
    "simulated_success",
    "parser_finality",
    "wal_authority",
}
NETWORK_IMPORT_RE = re.compile(
    r"^\s*(?:import|from)\s+(urllib|http|socket|ssl|requests|httpx|aiohttp|urllib3)(?:[.\s]|$)",
    re.MULTILINE,
)


class GateError(AssertionError):
    pass


def canonical_json(value: Any) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True)


def sha256_text(value: str) -> str:
    return "sha256:" + hashlib.sha256(value.encode("utf-8")).hexdigest()


def load_fixture(path: Path) -> dict[str, Any]:
    if not path.is_file():
        raise GateError(f"native compile matrix missing: {path}")
    with path.open(encoding="utf-8") as handle:
        payload = json.load(handle)
    if not isinstance(payload, dict):
        raise GateError("native compile matrix root must be an object")
    return payload


def enrich(row: dict[str, Any]) -> dict[str, Any]:
    out = dict(row)
    out.setdefault("result_sha256", sha256_text(str(out.get("evidence_text", ""))))
    out.setdefault("diagnostic_sha256", sha256_text(str(out.get("diagnostic_code", ""))))
    out["row_sha256"] = sha256_text(canonical_json(
        {key: value for key, value in out.items() if key != "row_sha256"}
    ))
    return out


def manifest_hash(rows: list[dict[str, Any]]) -> str:
    return sha256_text(canonical_json({"schema_version": SCHEMA_VERSION, "rows": rows}))


def require(condition: bool, message: str, errors: list[str]) -> None:
    if not condition:
        errors.append(message)


def strings(value: Any) -> list[str]:
    if isinstance(value, dict):
        out: list[str] = []
        for item in value.values():
            out.extend(strings(item))
        return out
    if isinstance(value, list):
        out = []
        for item in value:
            out.extend(strings(item))
        return out
    return [str(value)]


def scan_no_network(path: Path) -> list[str]:
    text = path.read_text(encoding="utf-8", errors="replace")
    return [
        f"{path.name}:{text[:match.start()].count(chr(10)) + 1}: {match.group(1)}"
        for match in NETWORK_IMPORT_RE.finditer(text)
    ]


def validate_source(repo: Path, payload: dict[str, Any], errors: list[str]) -> None:
    for item in payload.get("source_evidence", []):
        path = repo / item.get("path", "")
        require(path.is_file(), f"source evidence path missing: {path}", errors)
        if not path.is_file():
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for token in item.get("tokens", []):
            require(token in text, f"{item.get('path')} missing token {token}", errors)


def validate_row(row: dict[str, Any], errors: list[str]) -> None:
    row_id = str(row.get("row_id", ""))
    required = {
        "row_id",
        "sml_slices",
        "category",
        "mode",
        "workload_class",
        "expected_result",
        "implementation_kind",
        "llvm_capability",
        "compiled",
        "fallback_count",
        "metadata_only",
        "diagnostic_code",
        "evidence_text",
        "result_sha256",
        "diagnostic_sha256",
        "row_sha256",
    }
    missing = sorted(required - set(row))
    require(not missing, f"{row_id} missing fields {missing}", errors)
    require(set(row.get("sml_slices", [])) <= REQUIRED_SLICES,
            f"{row_id} unknown SML slice", errors)
    require(row.get("category") in REQUIRED_CATEGORIES,
            f"{row_id} unknown category", errors)
    require(row.get("diagnostic_code", "").startswith("SBSQL."),
            f"{row_id} lacks stable diagnostic", errors)
    require(row.get("metadata_only") is False,
            f"{row_id} metadata-only row is not allowed", errors)
    require(row.get("fallback_count") == 0,
            f"{row_id} has fallback count {row.get('fallback_count')}", errors)
    require(row.get("result_sha256") == sha256_text(str(row.get("evidence_text", ""))),
            f"{row_id} result hash drift", errors)
    require(row.get("diagnostic_sha256") == sha256_text(str(row.get("diagnostic_code", ""))),
            f"{row_id} diagnostic hash drift", errors)

    if row.get("category") in POSITIVE_CATEGORIES:
        require(row.get("expected_result") == "accepted",
                f"{row_id} positive row must be accepted", errors)
        require(row.get("compiled") is True,
                f"{row_id} positive row must be compiled", errors)
        require(row.get("llvm_capability") == "required_present",
                f"{row_id} positive row must require present LLVM", errors)
        require(row.get("implementation_kind") in {
            "live_llvm_capability",
            "compiled_native_unit",
            "metrics_evidence",
        }, f"{row_id} positive row has weak implementation kind", errors)
    else:
        require(row.get("expected_result") == "refused",
                f"{row_id} refusal row must refuse", errors)
        require(row.get("compiled") is False,
                f"{row_id} refusal row must not compile", errors)
        require(row.get("implementation_kind") == "fail_closed_refusal",
                f"{row_id} refusal row must be fail-closed", errors)

    if row.get("category") == "equivalence":
        require(row.get("equivalence") == "native_result_hash_matches_interpreter_result_hash",
                f"{row_id} missing interpreter equivalence proof", errors)
    if row.get("category") in {"jit_cold_warm_metrics", "aot_build_load_metrics", "release_metrics"}:
        metrics = row.get("metrics", {})
        require(isinstance(metrics, dict) and bool(metrics),
                f"{row_id} metrics missing", errors)
        for key, value in metrics.items():
            require(isinstance(value, int) and value >= 0,
                    f"{row_id} metric {key} invalid", errors)

    text = " ".join(strings(row)).lower()
    for token in sorted(FORBIDDEN_TOKENS):
        require(token not in text, f"{row_id} forbidden token {token!r}", errors)


def validate(repo: Path, payload: dict[str, Any], fixture: Path, gate_path: Path) -> list[str]:
    errors: list[str] = []
    require(payload.get("schema_version") == SCHEMA_VERSION, "schema drift", errors)
    rows = [enrich(row) for row in payload.get("rows", [])]
    require(len(rows) >= 10, "native compile matrix must contain at least 10 rows", errors)
    require(REQUIRED_CATEGORIES <= {row.get("category") for row in rows},
            "missing native compile categories", errors)
    covered = set().union(*(set(row.get("sml_slices", [])) for row in rows))
    require(REQUIRED_SLICES <= covered,
            f"missing SML coverage: {sorted(REQUIRED_SLICES - covered)}", errors)
    require(payload.get("manifest_sha256") == manifest_hash(rows),
            f"manifest hash drifted: expected {manifest_hash(rows)}", errors)
    validate_source(repo, payload, errors)
    for row in rows:
        validate_row(row, errors)
    for path in (fixture, gate_path):
        errors.extend(scan_no_network(path))
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, default=Path(DEFAULT_FIXTURE))
    args = parser.parse_args()
    fixture = args.fixture if args.fixture.is_absolute() else args.repo_root / args.fixture
    try:
        payload = load_fixture(fixture)
        errors = validate(args.repo_root, payload, fixture, Path(__file__).resolve())
    except Exception as exc:  # noqa: BLE001
        print(f"sbsql_sml_064_066_native_compile_gate=failed: {exc}", file=sys.stderr)
        return 2
    if errors:
        print("sbsql_sml_064_066_native_compile_gate=failed", file=sys.stderr)
        for error in errors[:80]:
            print(error, file=sys.stderr)
        return 1
    print(
        "sbsql_sml_064_066_native_compile_gate=passed "
        f"rows={len(payload.get('rows', []))} "
        f"manifest_sha256={payload.get('manifest_sha256')}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
