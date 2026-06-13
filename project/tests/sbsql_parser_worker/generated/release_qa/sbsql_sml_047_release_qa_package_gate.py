#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate the SML-047 self-contained release QA package."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any


SCHEMA_VERSION = "sbsql.sml_047.release_qa_package.v1"
DEFAULT_MANIFEST = (
    "project/tests/sbsql_parser_worker/generated/release_qa/"
    "SML_047_RELEASE_QA_PACKAGE_MANIFEST.json"
)
GENERATOR = "project/tools/sb_parser_gen/generate_sbsql_sml_047_release_qa_package.py"
REQUIRED_GROUPS = {
    "SML-041-046",
    "SML-049-052",
    "SML-054-057-062",
    "SML-059-061",
    "SML-064-066",
}
REQUIRED_SLICES = {
    "SML-041",
    "SML-042",
    "SML-043",
    "SML-044",
    "SML-045",
    "SML-046",
    "SML-049",
    "SML-050",
    "SML-051",
    "SML-052",
    "SML-054",
    "SML-055",
    "SML-056",
    "SML-057",
    "SML-059",
    "SML-060",
    "SML-061",
    "SML-062",
    "SML-064",
    "SML-065",
    "SML-066",
}
REQUIRED_LABELS = {
    "sbsql_product_qa_manifest",
    "sbsql_hardening_oracle",
    "sbsql_filespace_relocation",
    "sbsql_multimodel_capability",
    "sbsql_encrypted_database_matrix",
    "sbsql_native_compile_jit_aot",
}
FORBIDDEN_PATH_PARTS = {
    "/docs/" + "workplans/",
    "/docs/completed-" + "workplans/",
    "/docs/" + "audit/",
    "/docs/" + "reports/",
    "/docs/documentation/draft/",
    "/" + "local" + "_work/",
    "/ScratchBird-" + "Private/",
    "/reference/project_clones/",
}
FORBIDDEN_TEXT = {
    "".join(("place", "holder")),
    "".join(("to", "do")),
    "tbd",
    "".join(("de", "ferred")),
    "".join(("skip", "ped")),
    "".join(("wai", "ved")),
    "".join(("manual", "_only")),
    "".join(("generated", "_only")),
    "".join(("file", "_presence")),
    "".join(("parser", "-owned")),
    "parser_owned",
    "parser finality",
}
NETWORK_IMPORT_RE = re.compile(
    r"^\s*(?:import|from)\s+("
    r"urllib|http|socket|ssl|ftplib|smtplib|telnetlib|imaplib|poplib|"
    r"nntplib|xmlrpc|webbrowser|requests|httpx|aiohttp|urllib3|pycurl|"
    r"paramiko|asyncssh|botocore|boto3"
    r")(?:[.\s]|$)",
    re.MULTILINE,
)


class GateError(AssertionError):
    pass


def canonical_json(value: Any) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True)


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def sha256_text(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def load_manifest(path: Path) -> dict[str, Any]:
    if not path.is_file():
        raise GateError(f"release QA manifest missing: {path}")
    payload = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(payload, dict):
        raise GateError("release QA manifest root must be an object")
    return payload


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


def require(condition: bool, message: str, errors: list[str]) -> None:
    if not condition:
        errors.append(message)


def scan_no_network(path: Path) -> list[str]:
    text = path.read_text(encoding="utf-8", errors="replace")
    return [
        f"{path}:{text[:match.start()].count(chr(10)) + 1}: forbidden network import {match.group(1)}"
        for match in NETWORK_IMPORT_RE.finditer(text)
    ]


def validate_no_forbidden_text(payload: dict[str, Any], errors: list[str]) -> None:
    for text in strings(payload):
        lowered = text.lower()
        for token in FORBIDDEN_TEXT:
            require(token not in lowered, f"forbidden release QA token {token!r} in {text!r}", errors)


def validate_paths(repo_root: Path, payload: dict[str, Any], errors: list[str]) -> None:
    for group in payload.get("evidence_groups", []):
        group_id = str(group.get("group_id", ""))
        for item in group.get("evidence_files", []):
            rel = str(item.get("path", ""))
            generic = "/" + rel.replace("\\", "/")
            for forbidden in FORBIDDEN_PATH_PARTS:
                require(forbidden not in generic, f"{group_id} uses forbidden path {rel}", errors)
            require(rel.startswith("project/"), f"{group_id} evidence must be project-scoped: {rel}", errors)
            path = repo_root / rel
            require(path.is_file(), f"{group_id} missing evidence file {rel}", errors)
            if not path.is_file():
                continue
            data = path.read_bytes()
            require(item.get("bytes") == len(data), f"{group_id} byte count drift for {rel}", errors)
            require(item.get("sha256") == sha256_bytes(data), f"{group_id} hash drift for {rel}", errors)


def validate_groups(payload: dict[str, Any], errors: list[str]) -> None:
    groups = payload.get("evidence_groups", [])
    require(isinstance(groups, list) and groups, "evidence groups must be present", errors)
    if not isinstance(groups, list):
        return
    by_id = {group.get("group_id"): group for group in groups if isinstance(group, dict)}
    require(set(by_id) == REQUIRED_GROUPS, f"group set drift: {sorted(by_id)}", errors)
    covered_slices: set[str] = set()
    covered_labels: set[str] = set()
    for group_id, group in by_id.items():
        covered_slices.update(group.get("sml_slices", []))
        covered_labels.update(group.get("ctest_labels", []))
        require(group.get("runtime_dependency") == "repo_tracked_project_evidence_only",
                f"{group_id} runtime dependency is not self-contained", errors)
        require(group.get("private_workplan_dependency") is False,
                f"{group_id} declares private workplan dependency", errors)
        require(group.get("reference_tree_dependency") is False,
                f"{group_id} declares reference tree dependency", errors)
        require(group.get("parser_executes_sql") is False,
                f"{group_id} lets parser execute SQL", errors)
        require(group.get("parser_owns_finality") is False,
                f"{group_id} gives parser finality", errors)
        require(group.get("storage_finality_authority") == "scratchbird_engine_mga_transaction_inventory",
                f"{group_id} storage finality authority drift", errors)
        expected_hash = sha256_text(canonical_json({
            key: value for key, value in group.items() if key != "group_sha256"
        }))
        require(group.get("group_sha256") == expected_hash,
                f"{group_id} group hash drift", errors)
    require(REQUIRED_SLICES <= covered_slices,
            f"missing SML slices {sorted(REQUIRED_SLICES - covered_slices)}", errors)
    require(REQUIRED_LABELS <= covered_labels,
            f"missing labels {sorted(REQUIRED_LABELS - covered_labels)}", errors)


def validate_determinism(repo_root: Path, manifest: Path, errors: list[str]) -> None:
    generator = repo_root / GENERATOR
    require(generator.is_file(), f"generator missing: {GENERATOR}", errors)
    if not generator.is_file():
        return
    errors.extend(scan_no_network(generator))
    errors.extend(scan_no_network(Path(__file__).resolve()))
    with tempfile.TemporaryDirectory(prefix="sml_047_release_qa_") as temp:
        output = Path(temp) / "manifest.json"
        result = subprocess.run(
            [sys.executable, str(generator), "--repo-root", str(repo_root), "--output", str(output)],
            cwd=repo_root,
            text=True,
            capture_output=True,
            check=False,
        )
        if result.returncode != 0:
            errors.append(
                "release QA regeneration failed "
                f"exit={result.returncode} stderr={result.stderr.strip()[:240]}"
            )
            return
        if output.read_bytes() != manifest.read_bytes():
            errors.append("tracked release QA manifest differs from deterministic regeneration")


def validate(repo_root: Path, manifest_path: Path) -> list[str]:
    errors: list[str] = []
    payload = load_manifest(manifest_path)
    require(payload.get("schema_version") == SCHEMA_VERSION, "schema version drift", errors)
    require(payload.get("gate_id") == "SML-GATE-047", "gate id drift", errors)
    require(payload.get("status") == "implemented_proven", "release QA package is not implemented_proven", errors)
    require(payload.get("network_dependency") is False, "release QA package declares network dependency", errors)
    require(payload.get("generated_from_tracked_project_files") is True,
            "release QA package must be generated from tracked project files", errors)
    require(payload.get("private_workplan_dependency") is False,
            "release QA package depends on private workplan material", errors)
    require(payload.get("reference_tree_dependency") is False,
            "release QA package depends on reference trees", errors)
    expected_manifest_hash = sha256_text(canonical_json({
        key: value for key, value in payload.items() if key != "manifest_sha256"
    }))
    require(payload.get("manifest_sha256") == expected_manifest_hash,
            "manifest hash drift", errors)
    validate_no_forbidden_text(payload, errors)
    validate_groups(payload, errors)
    validate_paths(repo_root, payload, errors)
    validate_determinism(repo_root, manifest_path, errors)
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, default=Path(DEFAULT_MANIFEST))
    args = parser.parse_args()
    repo_root = args.repo_root.resolve()
    manifest_path = args.manifest if args.manifest.is_absolute() else repo_root / args.manifest
    try:
        errors = validate(repo_root, manifest_path)
    except Exception as exc:  # noqa: BLE001 - gate reports closed failure.
        print(f"sbsql_sml_047_release_qa_package_gate=failed: {exc}", file=sys.stderr)
        return 2
    if errors:
        print("sbsql_sml_047_release_qa_package_gate=failed", file=sys.stderr)
        for error in errors[:100]:
            print(error, file=sys.stderr)
        if len(errors) > 100:
            print(f"... {len(errors) - 100} additional errors", file=sys.stderr)
        return 1
    payload = load_manifest(manifest_path)
    print(
        "sbsql_sml_047_release_qa_package_gate=passed "
        f"groups={len(payload.get('evidence_groups', []))} "
        f"manifest_sha256={payload.get('manifest_sha256')}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
