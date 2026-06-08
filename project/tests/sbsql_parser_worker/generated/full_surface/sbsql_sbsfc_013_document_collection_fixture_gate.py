#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""SBSFC-013 document/collection fixture gate.

This is a deterministic, local-only gate for the bounded JSON/document slice.
It cross-checks fixture rows against the canonical surface registry and the
builtin expression registry, and evaluates the declared simple JSON semantics
without invoking a parser, donor backend, network path, or storage engine.
"""

from __future__ import annotations

import argparse
import csv
import json
import re
import sys
from pathlib import Path


SURFACE_REGISTRY = "public_input_snapshot"
BUILTIN_REGISTRY = "public_contract_snapshot"
FIXTURES = "project/tests/sbsql_parser_worker/generated/full_surface/SBSFC_013_DOCUMENT_COLLECTION_FIXTURES.csv"

REQUIRED_COLUMNS = [
    "fixture_id",
    "surface_id",
    "function_id",
    "canonical_builtin_id",
    "case_kind",
    "arguments_json",
    "expected_result_descriptor",
    "oracle_authority_ref",
    "notes",
]

TARGET_SURFACES = {
    "SBSQL-FE519C1C20F6": "sb.json.typeof",
    "SBSQL-4324775DBCA0": "sb.json.typeof",
    "SBSQL-36FF2B0254C0": "sb.json.typeof",
    "SBSQL-83995B2BC266": "sb.json.extract",
    "SBSQL-35F0E9FF7755": "sb.json.extract",
    "SBSQL-7B0C5EB84734": "sb.json.extract",
    "SBSQL-7EE8FAD14C5A": "sb.json.exists",
    "SBSQL-3E3BA120C541": "sb.json.exists",
    "SBSQL-1342A8B02022": "sb.json.exists",
    "SBSQL-5E705E2E1462": "sb.json.value",
    "SBSQL-5B765753ADEC": "sb.json.value",
    "SBSQL-68134CF09B70": "sb.json.value",
    "SBSQL-09BA0A3A71DB": "sb.json.query",
    "SBSQL-DC8507F9B9C5": "sb.json.query",
    "SBSQL-03D2E8D0B9AE": "sb.json.set",
    "SBSQL-7AA44FB9077C": "sb.json.set",
    "SBSQL-E062D64D0C8F": "sb.json.remove",
    "SBSQL-B4CF3C70B1D3": "sb.json.remove",
    "SBSQL-14C23DAA9C77": "sb.json.replace",
    "SBSQL-23EEDCBB8140": "sb.json.replace",
    "SBSQL-AF045C026980": "sb.json.insert",
    "SBSQL-2E230404921F": "sb.json.insert",
    "SBSQL-C55F7ADBD13B": "sb.json.jsonb_set",
    "SBSQL-723E5CADA519": "sb.json.jsonb_set",
    "SBSQL-0A060E6427B3": "sb.json.array_length",
    "SBSQL-DEDF07FAB7F3": "sb.json.array_length",
    "SBSQL-5BCF9869AA4C": "sb.json.jsonb_array_length",
    "SBSQL-3CEB816A1165": "sb.json.jsonb_array_length",
    "SBSQL-7B99FF977C66": "sb.json.build_array",
    "SBSQL-4640811DBAC8": "sb.json.build_array",
    "SBSQL-E2DFF93CA59C": "sb.json.build_object",
    "SBSQL-3217FFB2F3BD": "sb.json.build_object",
    "SBSQL-36FBFED38C80": "sb.json.jsonb_build_array",
    "SBSQL-34E68EB56EDC": "sb.json.jsonb_build_object",
    "SBSQL-CB837AAEBEAD": "sb.json.to_json",
    "SBSQL-F0AB18F7417B": "sb.json.to_json",
    "SBSQL-4119D041403C": "sb.json.to_jsonb",
    "SBSQL-88E66066EBC7": "sb.json.to_jsonb",
    "SBSQL-048498BB9A7F": "sb.json.jsonb_typeof",
    "SBSQL-2F78C18D9292": "sb.json.jsonb_typeof",
    "SBSQL-0926D8F4ABD5": "sb.json.object",
    "SBSQL-551339ECEE75": "sb.json.jsonb_object",
    "SBSQL-90E1BC86D62F": "sb.json.object_keys",
    "SBSQL-A05313B740CC": "sb.json.object_keys",
    "SBSQL-03447BA4EB25": "sb.json.jsonb_object_keys",
    "SBSQL-EB982B4F95B3": "sb.json.jsonb_object_keys",
    "SBSQL-0390232C7296": "sb.json.array_elements",
    "SBSQL-7819B29C7AB5": "sb.json.array_elements",
    "SBSQL-4D97B9EA482B": "sb.json.array_elements_text",
    "SBSQL-76883ECD3648": "sb.json.each",
    "SBSQL-18521C5D03B8": "sb.json.each",
    "SBSQL-4B7CDEB23364": "sb.json.each_text",
    "SBSQL-6AF2FB9EDEB9": "sb.json.each_text",
    "SBSQL-CE5BD771D075": "sb.json.jsonb_insert",
    "SBSQL-429DB32D5CC2": "sb.json.jsonb_insert",
    "SBSQL-58F6D7F43DA6": "sb.json.jsonb_path_exists",
    "SBSQL-D4C29991D99B": "sb.json.jsonb_path_exists",
    "SBSQL-9A4AB48B76FD": "sb.json.jsonb_path_match",
    "SBSQL-7C4821112F94": "sb.json.jsonb_path_match",
    "SBSQL-436880E1F3F7": "sb.json.jsonb_path_query",
    "SBSQL-EA5E00825D4D": "sb.json.jsonb_path_query",
    "SBSQL-A1C65D80CE68": "sb.json.jsonb_path_query_array",
    "SBSQL-B64295F1B742": "sb.json.jsonb_path_query_first",
    "SBSQL-6910DED90537": "sb.json.jsonb_pretty",
    "SBSQL-4CFCAC326BFB": "sb.json.jsonb_pretty",
    "SBSQL-5157364BCB20": "sb.json.jsonb_strip_nulls",
    "SBSQL-98D9B54A7630": "sb.json.jsonb_strip_nulls",
}


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def read_csv(path: Path) -> list[dict[str, str]]:
    if not path.is_file():
        fail(f"required CSV missing: {path}")
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def builtin_ids(path: Path) -> set[str]:
    if not path.is_file():
        fail(f"builtin registry missing: {path}")
    text = path.read_text(encoding="utf-8")
    return set(re.findall(r"^- builtin_id: ([^\n]+)$", text, flags=re.MULTILINE))


def extract_path(document_text: str, path: str) -> object | None:
    if path == "$":
        return json.loads(document_text)
    if not path.startswith("$."):
        return None
    doc = json.loads(document_text)
    if not isinstance(doc, dict):
        return None
    return doc.get(path[2:])


def compact(value: object) -> str:
    return json.dumps(value, separators=(",", ":"))


def normalize_multiline(value: str) -> str:
    return value.replace("\r\n", "\n").replace("\r", "\n")


def evaluate(canonical: str, args: list[object]) -> tuple[str, str | None]:
    if canonical in {"sb.json.typeof", "sb.json.jsonb_typeof"}:
        value = json.loads(str(args[0]))
        if value is None:
            return ("null", None)
        if isinstance(value, bool):
            return ("boolean", None)
        if isinstance(value, (int, float)):
            return ("number", None)
        if isinstance(value, str):
            return ("string", None)
        if isinstance(value, list):
            return ("array", None)
        if isinstance(value, dict):
            return ("object", None)
        return ("", "SBSQL.FUNCTION.INVALID_INPUT")
    if canonical == "sb.json.exists":
        return ("1" if extract_path(str(args[0]), str(args[1])) is not None else "0", None)
    if canonical in {"sb.json.extract", "sb.json.value", "sb.json.query"}:
        value = extract_path(str(args[0]), str(args[1]))
        return ("" if value is None else compact(value), None)
    if canonical == "sb.json.set":
        doc = json.loads(str(args[0]))
        if not isinstance(doc, dict) or not str(args[1]).startswith("$."):
            return ("", "SBSQL.FUNCTION.INVALID_INPUT")
        doc[str(args[1])[2:]] = args[2]
        return (compact(doc), None)
    if canonical == "sb.json.remove":
        doc = json.loads(str(args[0]))
        if not isinstance(doc, dict) or not str(args[1]).startswith("$."):
            return ("", "SBSQL.FUNCTION.INVALID_INPUT")
        doc.pop(str(args[1])[2:], None)
        return (compact(doc), None)
    if canonical == "sb.json.replace":
        doc = json.loads(str(args[0]))
        if not isinstance(doc, dict) or not str(args[1]).startswith("$."):
            return ("", "SBSQL.FUNCTION.INVALID_INPUT")
        key = str(args[1])[2:]
        if key in doc:
            doc[key] = args[2]
        return (compact(doc), None)
    if canonical == "sb.json.insert":
        doc = json.loads(str(args[0]))
        if not isinstance(doc, dict) or not str(args[1]).startswith("$."):
            return ("", "SBSQL.FUNCTION.INVALID_INPUT")
        key = str(args[1])[2:]
        if key not in doc:
            doc[key] = args[2]
        return (compact(doc), None)
    if canonical == "sb.json.jsonb_set":
        doc = json.loads(str(args[0]))
        if not isinstance(doc, dict) or not str(args[1]).startswith("$."):
            return ("", "SBSQL.FUNCTION.INVALID_INPUT")
        key = str(args[1])[2:]
        create_missing = True if len(args) == 3 else bool(args[3])
        if key in doc or create_missing:
            doc[key] = args[2]
        return (compact(doc), None)
    if canonical in {"sb.json.array_length", "sb.json.jsonb_array_length"}:
        value = json.loads(str(args[0]))
        if not isinstance(value, list):
            return ("", "SBSQL.FUNCTION.INVALID_INPUT")
        return (str(len(value)), None)
    if canonical in {"sb.json.build_array", "sb.json.jsonb_build_array"}:
        return (compact(args), None)
    if canonical in {"sb.json.build_object", "sb.json.jsonb_build_object"}:
        if len(args) % 2:
            return ("", "SBSQL.FUNCTION.INVALID_INPUT")
        out = {}
        for index in range(0, len(args), 2):
            out[str(args[index])] = args[index + 1]
        return (compact(out), None)
    if canonical in {"sb.json.object", "sb.json.jsonb_object"}:
        if len(args) % 2:
            return ("", "SBSQL.FUNCTION.INVALID_INPUT")
        out = {}
        for index in range(0, len(args), 2):
            out[str(args[index])] = args[index + 1]
        return (compact(out), None)
    if canonical in {"sb.json.object_keys", "sb.json.jsonb_object_keys"}:
        doc = json.loads(str(args[0]))
        if not isinstance(doc, dict):
            return ("", "SBSQL.FUNCTION.INVALID_INPUT")
        return (compact(list(doc.keys())), None)
    if canonical in {"sb.json.array_elements", "sb.json.array_elements_text"}:
        value = json.loads(str(args[0]))
        if not isinstance(value, list):
            return ("", "SBSQL.FUNCTION.INVALID_INPUT")
        if canonical == "sb.json.array_elements_text":
            return (compact([compact(element) for element in value]), None)
        return (compact(value), None)
    if canonical in {"sb.json.each", "sb.json.each_text"}:
        doc = json.loads(str(args[0]))
        if not isinstance(doc, dict):
            return ("", "SBSQL.FUNCTION.INVALID_INPUT")
        out = []
        for key, value in doc.items():
            out.append({"key": key, "value": compact(value) if canonical == "sb.json.each_text" else value})
        return (compact(out), None)
    if canonical == "sb.json.jsonb_insert":
        doc = json.loads(str(args[0]))
        if not isinstance(doc, dict) or not str(args[1]).startswith("$."):
            return ("", "SBSQL.FUNCTION.INVALID_INPUT")
        key = str(args[1])[2:]
        if key not in doc:
            doc[key] = args[2]
        return (compact(doc), None)
    if canonical == "sb.json.jsonb_path_exists":
        return ("1" if extract_path(str(args[0]), str(args[1])) is not None else "0", None)
    if canonical == "sb.json.jsonb_path_match":
        return ("1" if extract_path(str(args[0]), str(args[1])) is True else "0", None)
    if canonical in {"sb.json.jsonb_path_query", "sb.json.jsonb_path_query_first"}:
        value = extract_path(str(args[0]), str(args[1]))
        return ("" if value is None else compact(value), None)
    if canonical == "sb.json.jsonb_path_query_array":
        value = extract_path(str(args[0]), str(args[1]))
        return ("[]" if value is None else compact([value]), None)
    if canonical == "sb.json.jsonb_pretty":
        return (json.dumps(json.loads(str(args[0])), indent=2, separators=(",", ": ")), None)
    if canonical == "sb.json.jsonb_strip_nulls":
        doc = json.loads(str(args[0]))
        if not isinstance(doc, dict):
            return ("", "SBSQL.FUNCTION.INVALID_INPUT")
        return (compact({key: value for key, value in doc.items() if value is not None}), None)
    if canonical in {"sb.json.to_json", "sb.json.to_jsonb"}:
        return (compact(args[0]), None)
    return ("", "SBSQL.FUNCTION.INVALID_INPUT")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    args = parser.parse_args()
    root = Path(args.repo_root).resolve()

    surfaces = {r["surface_id"]: r for r in read_csv(root / SURFACE_REGISTRY)}
    fixtures = read_csv(root / FIXTURES)
    builtins = builtin_ids(root / BUILTIN_REGISTRY)
    errors: list[str] = []

    seen_ids: set[str] = set()
    seen_surfaces: set[str] = set()
    for row in fixtures:
        fid = row.get("fixture_id", "")
        if not fid:
            errors.append("fixture row missing fixture_id")
            continue
        if fid in seen_ids:
            errors.append(f"duplicate fixture_id: {fid}")
        seen_ids.add(fid)

        for column in REQUIRED_COLUMNS:
            if not (row.get(column, "") or "").strip():
                errors.append(f"{fid}: required column {column} is empty")

        sid = row.get("surface_id", "")
        canonical = row.get("canonical_builtin_id", "")
        seen_surfaces.add(sid)
        if sid not in surfaces:
            errors.append(f"{fid}: unknown surface_id {sid}")
        if sid in TARGET_SURFACES and TARGET_SURFACES[sid] != canonical:
            errors.append(f"{fid}: canonical_builtin_id {canonical} does not match target {TARGET_SURFACES[sid]}")
        if canonical not in builtins:
            errors.append(f"{fid}: canonical_builtin_id {canonical} missing from builtin-expression-registry.yaml")

        try:
            args_value = json.loads(row.get("arguments_json", ""))
        except json.JSONDecodeError as exc:
            errors.append(f"{fid}: arguments_json is not valid JSON: {exc}")
            continue
        if not isinstance(args_value, list):
            errors.append(f"{fid}: arguments_json must be a JSON list")
            continue

        expected_diag = (row.get("expected_diagnostic_code", "") or "").strip()
        expected_value = (row.get("expected_result_value", "") or "").strip()
        actual_value, actual_diag = evaluate(canonical, args_value)
        if expected_diag:
            if actual_diag != expected_diag:
                errors.append(f"{fid}: expected diagnostic {expected_diag}, got {actual_diag or 'success'}")
            if expected_value:
                errors.append(f"{fid}: diagnostic fixture must not declare expected_result_value")
        else:
            if actual_diag:
                errors.append(f"{fid}: expected success, got diagnostic {actual_diag}")
            if normalize_multiline(actual_value) != normalize_multiline(expected_value):
                errors.append(f"{fid}: expected value {expected_value!r}, got {actual_value!r}")

    missing = sorted(set(TARGET_SURFACES) - seen_surfaces)
    for sid in missing:
        errors.append(f"target surface {sid} has no SBSFC-013 fixture")

    print(
        "sbsql_sbsfc_013_document_collection_fixture_gate "
        f"fixtures={len(fixtures)} "
        f"target_surfaces={len(TARGET_SURFACES)} "
        f"errors={len(errors)}"
    )
    if errors:
        print("sbsql_sbsfc_013_document_collection_fixture_gate=failed", file=sys.stderr)
        for err in errors[:30]:
            print(f"  {err}", file=sys.stderr)
        return 1
    print("sbsql_sbsfc_013_document_collection_fixture_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
