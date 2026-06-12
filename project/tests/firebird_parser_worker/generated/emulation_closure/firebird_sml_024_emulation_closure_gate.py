#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""SML-024 Firebird emulation/parser closure gate."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any


SCHEMA_VERSION = "firebird.emulation_closure.sml_024.v1"
MANIFEST_NAME = "SML_024_FIREBIRD_EMULATION_CLOSURE_MANIFEST.csv"
BOUNDARY_NAME = "SML_024_FIREBIRD_EMULATION_SOURCE_BOUNDARY.json"
GENERATOR = "project/tools/sb_parser_gen/generate_firebird_emulation_closure.py"
GATE_NAME = "firebird_sml_024_emulation_closure_gate"

MANIFEST_COLUMNS = [
    "sml_id",
    "gate_id",
    "row_id",
    "proof_surface",
    "coverage_class",
    "source_authority",
    "ctest_resource_class",
    "executable_gate",
    "parser_role",
    "storage_authority",
    "finality_authority",
    "reference_engine_source_used",
    "reference_engine_storage_used",
    "reference_engine_finality_used",
    "reference_engine_sql_executed",
    "raw_upstream_payload_tracked",
    "network_required",
    "closure_status",
    "source_rename_risk",
    "evidence_paths",
    "evidence_tokens",
    "result_contract",
    "result_hash",
]

REQUIRED_ROW_IDS = {
    "SML-024-FIREBIRD-PARSER-PIPELINE-SOURCE",
    "SML-024-FIREBIRD-LIFECYCLE-EMULATION",
    "SML-024-FIREBIRD-WORKER-SESSION-BOUNDARY",
    "SML-024-FIREBIRD-PARSER-SUPPORT-PACKAGE",
    "SML-024-FIREBIRD-REFERENCE-TEST-RESOURCE",
    "SML-024-FIREBIRD-REFERENCE-TOOL-SANDBOX",
    "SML-024-FIREBIRD-SOURCE-PROVENANCE-GATE",
}

LOCAL_GATES = {
    "firebird_parser_pipeline_probe",
    "firebird_runtime_absence_gate",
    "firebird_worker_session_probe",
    "sbu_firebird_parser_support_probe",
    "firebird_clean_room_provenance_gate",
}

EXTERNAL_RESOURCE_GATES = {
    "firebird_qa_replay_manifest_gate",
    "firebird_reference_tool_sandbox_gate",
}

FALSE_COLUMNS = {
    "reference_engine_source_used",
    "reference_engine_storage_used",
    "reference_engine_finality_used",
    "reference_engine_sql_executed",
    "raw_upstream_payload_tracked",
    "network_required",
}

IMPLEMENTATION_SOURCE_ROOTS = {
    "project/src/parsers/compatibility/firebird",
    "project/src/udr/sbu_firebird_parser_support",
}

SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hpp"}
TEXT_SUFFIXES = {".py", ".csv", ".json", ".md", ".txt", ".cmake", ".cpp", ".hpp", ".h"}
BLOCKED_PUBLIC_TERMS = ("".join(("do", "nor")),)

FORBIDDEN_SOURCE_TOKENS = (
    "The contents of this file are subject to the InterBase Public License",
    "Initial Developer",
    "Firebird Project",
    "#include <ibase.h>",
    "#include \"ibase.h\"",
    "#include <firebird/",
    "#include \"firebird/",
    "libfbclient",
    "libtommath",
    "libtomcrypt",
    "firebird-5.0.4-release-src",
    "reference/firebird",
)


def sha256_text(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def canonical_json(value: Any) -> str:
    return json.dumps(value, ensure_ascii=True, sort_keys=True, separators=(",", ":"))


def read_csv(path: Path) -> tuple[list[str], list[dict[str, str]]]:
    with path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        return list(reader.fieldnames or []), list(reader)


def load_json(path: Path) -> dict[str, Any]:
    with path.open(encoding="utf-8") as handle:
        loaded = json.load(handle)
    if not isinstance(loaded, dict):
        raise ValueError(f"{path} root must be a JSON object")
    return loaded


def require(condition: bool, message: str, errors: list[str]) -> None:
    if not condition:
        errors.append(message)


def rel_path(path_text: str, repo_root: Path) -> Path:
    path = Path(path_text)
    return path if path.is_absolute() else repo_root / path


def ctest_name_present(cmake_text: str, name: str) -> bool:
    return re.search(r"\bNAME\s+" + re.escape(name) + r"\b", cmake_text) is not None


def validate_manifest(
    repo_root: Path,
    rows: list[dict[str, str]],
    fieldnames: list[str],
    cmake_text: str,
    errors: list[str],
) -> None:
    require(fieldnames == MANIFEST_COLUMNS, "manifest columns drifted", errors)
    by_id = {row.get("row_id", ""): row for row in rows}
    require(set(by_id) == REQUIRED_ROW_IDS,
            f"manifest row set mismatch: {sorted(by_id)}", errors)

    for row in rows:
        row_id = row.get("row_id", "<missing>")
        require(row.get("sml_id") == "SML-024", f"{row_id} has wrong sml_id", errors)
        require(row.get("gate_id") == "SML-GATE-024", f"{row_id} has wrong gate_id", errors)
        require(row.get("closure_status") == "closed_proof",
                f"{row_id} must be closed_proof", errors)
        require(row.get("source_rename_risk") == "none_detected",
                f"{row_id} retained a source rename risk", errors)
        require(row.get("parser_role") not in {"", "unknown"},
                f"{row_id} parser_role must be explicit", errors)
        require(row.get("storage_authority") == "scratchbird_engine_runtime",
                f"{row_id} storage authority drifted", errors)
        require(row.get("finality_authority") == "scratchbird_engine_runtime",
                f"{row_id} finality authority drifted", errors)
        for column in FALSE_COLUMNS:
            require(row.get(column) == "false", f"{row_id} {column} must be false", errors)
        require(row.get("result_hash") == sha256_text(row.get("result_contract", "")),
                f"{row_id} result hash mismatch", errors)

        gate_name = row.get("executable_gate", "")
        require(ctest_name_present(cmake_text, gate_name),
                f"{row_id} executable gate is not registered in CMake: {gate_name}", errors)
        if gate_name in EXTERNAL_RESOURCE_GATES:
            require(row.get("ctest_resource_class") == "external_ctest_resource",
                    f"{row_id} must be an external CTest resource row", errors)
            require(row.get("source_authority") == "scratchbird_harness_plus_external_resource",
                    f"{row_id} external resource source authority drifted", errors)
        elif gate_name in LOCAL_GATES:
            require(row.get("ctest_resource_class") == "local_ctest",
                    f"{row_id} must be a local CTest row", errors)
            require(row.get("source_authority") == "scratchbird_owned_code",
                    f"{row_id} local source authority drifted", errors)
        else:
            errors.append(f"{row_id} has unrecognized executable gate {gate_name}")

        for evidence in filter(None, row.get("evidence_paths", "").split(";")):
            evidence_path = rel_path(evidence, repo_root)
            require(evidence_path.exists(), f"{row_id} evidence path missing: {evidence}", errors)


def validate_boundary(boundary: dict[str, Any], rows: list[dict[str, str]], errors: list[str]) -> None:
    require(boundary.get("schema_version") == SCHEMA_VERSION, "boundary schema drifted", errors)
    require(boundary.get("sml_id") == "SML-024", "boundary sml_id drifted", errors)
    require(boundary.get("gate_id") == "SML-GATE-024", "boundary gate_id drifted", errors)
    require(set(boundary.get("generated_artifacts", [])) == {MANIFEST_NAME, BOUNDARY_NAME},
            "boundary generated artifact list drifted", errors)
    require(boundary.get("manifest_sha256") == sha256_text(canonical_json(rows)),
            "boundary manifest sha256 mismatch", errors)

    source_roots = boundary.get("implementation_source_roots", [])
    require(isinstance(source_roots, list), "implementation_source_roots must be a list", errors)
    seen_roots = {row.get("path") for row in source_roots if isinstance(row, dict)}
    require(seen_roots == IMPLEMENTATION_SOURCE_ROOTS,
            f"implementation source root set drifted: {sorted(seen_roots)}", errors)
    for item in source_roots:
        if not isinstance(item, dict):
            errors.append("implementation source root entries must be objects")
            continue
        require(item.get("authority") == "scratchbird_owned_code",
                f"{item.get('path')} authority must be scratchbird_owned_code", errors)
        require(item.get("required_license") == "MPL-2.0",
                f"{item.get('path')} required license drifted", errors)
        require(item.get("reference_engine_source_used") is False,
                f"{item.get('path')} must not use reference engine source", errors)

    for item in boundary.get("external_ctest_resources", []):
        if not isinstance(item, dict):
            errors.append("external_ctest_resources entries must be objects")
            continue
        require(item.get("resource_class") == "external_ctest_resource",
                f"{item.get('path')} resource class drifted", errors)
        require(item.get("source_authority") == "not_scratchbird_implementation_source",
                f"{item.get('path')} source authority drifted", errors)
        require(item.get("raw_upstream_payload_tracked") is False,
                f"{item.get('path')} must not track raw upstream payloads for SML-024", errors)

    assertions = boundary.get("authority_assertions")
    require(isinstance(assertions, dict), "authority_assertions must be an object", errors)
    if isinstance(assertions, dict):
        for key, value in assertions.items():
            require(value is False, f"authority assertion {key} must be false", errors)
    required_gates = set(boundary.get("required_ctest_gates", []))
    expected_gates = {row["executable_gate"] for row in rows}
    require(required_gates == expected_gates,
            f"boundary required gate set drifted: {sorted(required_gates)}", errors)


def scan_implementation_sources(repo_root: Path, errors: list[str]) -> None:
    for root_text in IMPLEMENTATION_SOURCE_ROOTS:
        root = repo_root / root_text
        require(root.exists(), f"implementation source root missing: {root_text}", errors)
        if not root.exists():
            continue
        for path in root.rglob("*"):
            if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
                continue
            text = path.read_text(encoding="utf-8", errors="replace")
            rel = path.relative_to(repo_root).as_posix()
            require("Copyright (c) 2026 ScratchBird Software Inc." in text,
                    f"{rel} missing ScratchBird copyright header", errors)
            require("SPDX-License-Identifier: MPL-2.0" in text,
                    f"{rel} missing MPL SPDX header", errors)
            for token in FORBIDDEN_SOURCE_TOKENS:
                require(token not in text, f"{rel} contains forbidden source token {token!r}", errors)


def scan_new_files_for_blocked_terms(paths: list[Path], repo_root: Path, errors: list[str]) -> None:
    files: list[Path] = []
    for root in paths:
        if root.is_file():
            files.append(root)
            continue
        for path in root.rglob("*"):
            if path.is_file() and path.suffix in TEXT_SUFFIXES:
                files.append(path)
    for path in files:
        text = path.read_text(encoding="utf-8", errors="replace").lower()
        rel = path.relative_to(repo_root).as_posix()
        for term in BLOCKED_PUBLIC_TERMS:
            require(term not in text, f"{rel} contains blocked external-origin terminology", errors)


def validate_cmake_registration(cmake_text: str, errors: list[str]) -> None:
    require(ctest_name_present(cmake_text, GATE_NAME), f"{GATE_NAME} is not registered in CMake", errors)
    gate_block = re.search(
        r"set_tests_properties\(" + re.escape(GATE_NAME) + r"\s+PROPERTIES\s+LABELS\s+\"([^\"]+)\"",
        cmake_text,
        re.MULTILINE | re.DOTALL,
    )
    require(gate_block is not None, f"{GATE_NAME} labels are not declared", errors)
    if gate_block is not None:
        labels = set(gate_block.group(1).split(";"))
        for label in {"firebird_emulation_closure", "firebird_parser_worker", "SML-024", "SML-GATE-024"}:
            require(label in labels, f"{GATE_NAME} missing label {label}", errors)


def validate_deterministic_generation(repo_root: Path, fixture_root: Path, errors: list[str]) -> None:
    generator = repo_root / GENERATOR
    require(generator.is_file(), f"generator missing: {GENERATOR}", errors)
    if not generator.is_file():
        return
    with tempfile.TemporaryDirectory(prefix="firebird_sml_024_regen_") as temp:
        output_dir = Path(temp) / "emulation_closure"
        result = subprocess.run(
            [
                sys.executable,
                str(generator),
                "--repo-root",
                str(repo_root),
                "--output-dir",
                str(output_dir),
            ],
            cwd=repo_root,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        require(result.returncode == 0,
                f"SML-024 generator failed during deterministic check: {result.stdout}", errors)
        if result.returncode != 0:
            return
        for name in (MANIFEST_NAME, BOUNDARY_NAME):
            expected = fixture_root / name
            regenerated = output_dir / name
            require(regenerated.is_file(), f"regenerated artifact missing: {name}", errors)
            if regenerated.is_file() and expected.is_file():
                require(expected.read_bytes() == regenerated.read_bytes(),
                        f"deterministic regeneration mismatch: {name}", errors)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, required=True)
    parser.add_argument("--fixture-root", type=Path, required=True)
    parser.add_argument("--cmake-file", type=Path, required=True)
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    fixture_root = args.fixture_root
    if not fixture_root.is_absolute():
        fixture_root = repo_root / fixture_root
    cmake_file = args.cmake_file
    if not cmake_file.is_absolute():
        cmake_file = repo_root / cmake_file

    errors: list[str] = []
    manifest_path = fixture_root / MANIFEST_NAME
    boundary_path = fixture_root / BOUNDARY_NAME
    require(manifest_path.is_file(), f"manifest missing: {manifest_path}", errors)
    require(boundary_path.is_file(), f"boundary missing: {boundary_path}", errors)
    require(cmake_file.is_file(), f"CMake file missing: {cmake_file}", errors)
    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1

    fieldnames, rows = read_csv(manifest_path)
    boundary = load_json(boundary_path)
    cmake_text = cmake_file.read_text(encoding="utf-8", errors="replace")

    validate_manifest(repo_root, rows, fieldnames, cmake_text, errors)
    validate_boundary(boundary, rows, errors)
    scan_implementation_sources(repo_root, errors)
    scan_new_files_for_blocked_terms(
        [fixture_root, repo_root / GENERATOR, cmake_file],
        repo_root,
        errors,
    )
    validate_cmake_registration(cmake_text, errors)
    validate_deterministic_generation(repo_root, fixture_root, errors)

    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1
    print(
        "validated SML-024 Firebird emulation/parser closure: "
        f"rows={len(rows)} source_roots={len(IMPLEMENTATION_SOURCE_ROOTS)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
