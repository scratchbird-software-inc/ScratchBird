#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Generate and validate public generated-source provenance evidence."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
from typing import Any


# PUBLIC_GENERATED_SOURCE_PROVENANCE

FORBIDDEN_REFERENCE_FRAGMENTS = (
    "docs" + "/" + "execution-plans",
    "docs" + "/" + "completed-execution-plans",
    "docs" + "/" + "findings",
    "." + "git",
    "/" + "home" + "/",
    "ScratchBird" + "-Private",
    "local" + "_work",
)

REGISTRY_OUTPUTS = (
    "src/parsers/sbsql_worker/registry/generated/sbsql_generated_registry.cpp",
    "src/parsers/sbsql_worker/registry/generated/sbsql_generated_registry.hpp",
    "src/parsers/sbsql_worker/registry/generated/sbsql_generated_registry.manifest",
)

REGISTRY_INPUTS = (
    "tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/artifacts/SURFACE_IMPLEMENTATION_BACKLOG.csv",
    "tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/artifacts/BATCH_ROW_MEMBERSHIP.csv",
    "tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/artifacts/SEMANTIC_ORACLE_AUTHORITY_MAP.csv",
    "../public_input_snapshot",
    "../public_input_snapshot",
)

DETERMINISTIC_MANIFEST = (
    "tests/sbsql_parser_worker/generated/repro/DETERMINISTIC_ARTIFACT_MANIFEST.csv"
)

CSV_FIELDS = (
    "provenance_id",
    "output_scope",
    "output_count",
    "output_aggregate_sha256",
    "generator_path",
    "generator_sha256",
    "tool_version",
    "input_count",
    "input_aggregate_sha256",
    "regeneration_required",
    "regeneration_rule",
    "checked_in_authority_status",
    "drift_test",
)


def fail(message: str) -> None:
    print(f"public_generated_source_provenance=fail:{message}", file=sys.stderr)
    raise SystemExit(1)


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def reject_private_reference(value: str, context: str) -> None:
    if Path(value).is_absolute():
        fail(f"absolute_path_recorded:{context}:{value}")
    for fragment in FORBIDDEN_REFERENCE_FRAGMENTS:
        if fragment in value:
            fail(f"private_reference_recorded:{context}:{value}")


def sha256_text(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def rel_repo(path: Path, repo_root: Path) -> str:
    try:
        value = path.resolve().relative_to(repo_root.resolve()).as_posix()
    except ValueError:
        fail(f"path_outside_repo_root:{path}")
    reject_private_reference(value, "repo_path")
    return value


def project_file(project_root: Path, relative: str) -> Path:
    reject_private_reference(relative, "project_relative_path")
    path = (project_root / relative).resolve()
    if not path.is_file():
        fail(f"required_file_missing:{relative}")
    return path


def repo_file(repo_root: Path, relative: str) -> Path:
    reject_private_reference(relative, "repo_relative_path")
    path = (repo_root / relative).resolve()
    if not path.is_file():
        fail(f"required_file_missing:{relative}")
    return path


def aggregate_files(paths: list[Path], repo_root: Path) -> str:
    digest = hashlib.sha256()
    for path in sorted(paths, key=lambda item: rel_repo(item, repo_root)):
        rel = rel_repo(path, repo_root)
        digest.update(rel.encode("utf-8"))
        digest.update(b"\0")
        digest.update(sha256_file(path).encode("ascii"))
        digest.update(b"\0")
    return digest.hexdigest()


def read_manifest(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        rows = list(csv.DictReader(handle))
    required = {"artifact_path", "sha256", "size_bytes", "category", "source_inputs"}
    require(set(rows[0].keys() if rows else ()) == required,
            "deterministic_manifest_schema_mismatch")
    return rows


def generated_artifact_rows(repo_root: Path) -> list[dict[str, str]]:
    roots = (
        repo_root / "project/src/parsers/sbsql_worker/registry/generated",
        repo_root / "project/tests/sbsql_parser_worker/generated",
    )
    files: list[Path] = []
    for root in roots:
        for path in root.rglob("*"):
            if not path.is_file():
                continue
            if "__pycache__" in path.parts or path.suffix == ".pyc":
                continue
            if "/generated/repro/" in path.as_posix():
                continue
            files.append(path)

    def category(path: Path) -> str:
        parts = path.parts
        if "registry" in parts and "generated" in parts:
            return "registry_generated"
        if "generated" in parts:
            index = parts.index("generated")
            return "test_generated_" + (parts[index + 1] if index + 1 < len(parts) else "root")
        return "generated"

    def source_inputs(path: Path) -> str:
        parts = path.parts
        if "registry" in parts and "generated" in parts:
            return "SURFACE_IMPLEMENTATION_BACKLOG.csv;BATCH_ROW_MEMBERSHIP.csv;SEMANTIC_ORACLE_AUTHORITY_MAP.csv"
        return "repo_tracked_generated_fixture"

    rows: list[dict[str, str]] = []
    for path in sorted(files, key=lambda item: rel_repo(item, repo_root)):
        rows.append(
            {
                "artifact_path": rel_repo(path, repo_root),
                "sha256": sha256_file(path),
                "size_bytes": str(path.stat().st_size),
                "category": category(path),
                "source_inputs": source_inputs(path),
            }
        )
    return rows


def check_manifest_drift(repo_root: Path, manifest_path: Path) -> dict[str, Any]:
    expected = read_manifest(manifest_path)
    actual = generated_artifact_rows(repo_root)
    require(actual == expected,
            "deterministic_artifact_manifest_drift")
    paths = [repo_file(repo_root, row["artifact_path"]) for row in actual]
    categories: dict[str, int] = {}
    for row in actual:
        categories[row["category"]] = categories.get(row["category"], 0) + 1
    return {
        "artifact_count": len(actual),
        "aggregate_sha256": aggregate_files(paths, repo_root),
        "category_counts": categories,
        "manifest_sha256": sha256_file(manifest_path),
    }


def run_registry_generator(
    repo_root: Path,
    project_root: Path,
    output_dir: Path,
) -> None:
    generator = project_root / "tools/sb_parser_gen/generate_sbsql_registry.py"
    artifact_root = (
        project_root /
        "tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/artifacts"
    )
    env = os.environ.copy()
    env.update(
        {
            "LC_ALL": "C",
            "TZ": "UTC",
            "PYTHONHASHSEED": "0",
            "PYTHONNOUSERSITE": "1",
            "http_proxy": "",
            "https_proxy": "",
            "HTTP_PROXY": "",
            "HTTPS_PROXY": "",
            "ALL_PROXY": "",
            "NO_PROXY": "*",
        }
    )
    completed = subprocess.run(
        [
            sys.executable,
            str(generator),
            "--artifact-root",
            str(artifact_root),
            "--output-dir",
            str(output_dir),
        ],
        cwd=repo_root,
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if completed.returncode != 0:
        if completed.stdout:
            print(completed.stdout)
        fail(f"registry_generator_failed:{completed.returncode}")


def check_registry_drift(
    repo_root: Path,
    project_root: Path,
    work_root: Path,
) -> dict[str, Any]:
    output_dir = work_root / "registry_regenerated"
    output_dir.mkdir(parents=True, exist_ok=True)
    run_registry_generator(repo_root, project_root, output_dir)
    compared: list[str] = []
    for relative in REGISTRY_OUTPUTS:
        checked_in = project_file(project_root, relative)
        generated = output_dir / Path(relative).name
        require(generated.is_file(), f"regenerated_output_missing:{generated.name}")
        require(checked_in.read_bytes() == generated.read_bytes(),
                f"generated_registry_drift:{relative}")
        compared.append(rel_repo(checked_in, repo_root))
    return {
        "compared_outputs": compared,
        "output_count": len(compared),
        "aggregate_sha256": aggregate_files(
            [project_file(project_root, relative) for relative in REGISTRY_OUTPUTS],
            repo_root,
        ),
    }


def build_rows(
    repo_root: Path,
    project_root: Path,
    registry_drift: dict[str, Any],
    manifest_drift: dict[str, Any],
) -> list[dict[str, str]]:
    registry_generator = project_file(
        project_root, "tools/sb_parser_gen/generate_sbsql_registry.py")
    deterministic_gate = project_file(
        project_root,
        "tests/sbsql_parser_worker/generated/repro/sbsql_deterministic_no_network_gate.py",
    )
    registry_inputs = []
    for relative in REGISTRY_INPUTS:
        if relative.startswith("../"):
            registry_inputs.append(repo_file(repo_root, relative.removeprefix("../")))
        else:
            registry_inputs.append(project_file(project_root, relative))
    manifest = project_file(project_root, DETERMINISTIC_MANIFEST)

    rows = [
        {
            "provenance_id": "sbsql_generated_registry",
            "output_scope": "project/src/parsers/sbsql_worker/registry/generated",
            "output_count": str(registry_drift["output_count"]),
            "output_aggregate_sha256": str(registry_drift["aggregate_sha256"]),
            "generator_path": rel_repo(registry_generator, repo_root),
            "generator_sha256": sha256_file(registry_generator),
            "tool_version": "sha256:" + sha256_file(registry_generator),
            "input_count": str(len(registry_inputs)),
            "input_aggregate_sha256": aggregate_files(registry_inputs, repo_root),
            "regeneration_required": "true",
            "regeneration_rule": "python3 project/tools/sb_parser_gen/generate_sbsql_registry.py",
            "checked_in_authority_status": "checked_in_generated_source_evidence_only",
            "drift_test": "regenerate_and_byte_compare",
        },
        {
            "provenance_id": "sbsql_parser_worker_generated_fixture_corpus",
            "output_scope": "project/tests/sbsql_parser_worker/generated",
            "output_count": str(manifest_drift["artifact_count"]),
            "output_aggregate_sha256": str(manifest_drift["aggregate_sha256"]),
            "generator_path": rel_repo(deterministic_gate, repo_root),
            "generator_sha256": sha256_file(deterministic_gate),
            "tool_version": "sha256:" + sha256_file(deterministic_gate),
            "input_count": "1",
            "input_aggregate_sha256": sha256_file(manifest),
            "regeneration_required": "true",
            "regeneration_rule": "update checked-in fixture corpus and DETERMINISTIC_ARTIFACT_MANIFEST.csv together",
            "checked_in_authority_status": "checked_in_generated_fixture_evidence_only",
            "drift_test": "runtime_manifest_hash_compare",
        },
    ]
    for row in rows:
        for key, value in row.items():
            reject_private_reference(str(value), f"provenance_row:{row['provenance_id']}:{key}")
            require(str(value) != "", f"empty_provenance_field:{row['provenance_id']}:{key}")
        require(row["regeneration_required"] == "true",
                f"regeneration_not_required:{row['provenance_id']}")
        require(row["checked_in_authority_status"].endswith("_evidence_only"),
                f"checked_in_authority_overclaim:{row['provenance_id']}")
    return rows


def write_csv(path: Path, rows: list[dict[str, str]]) -> str:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=CSV_FIELDS, lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)
    return path.read_text(encoding="utf-8")


def build_evidence(args: argparse.Namespace) -> dict[str, Any]:
    repo_root = args.repo_root.resolve()
    project_root = args.project_root.resolve()
    build_root = args.build_root.resolve()
    work_root = args.work_root.resolve()
    if project_root.name != "project" or not project_root.is_dir():
        fail("project_root_must_be_project_directory")
    if not repo_root.is_dir() or not build_root.is_dir():
        fail("input_root_missing")
    try:
        work_root.relative_to(build_root)
    except ValueError:
        fail("work_root_must_be_under_build_root")
    work_root.mkdir(parents=True, exist_ok=True)

    manifest_path = project_file(project_root, DETERMINISTIC_MANIFEST)
    registry_drift = check_registry_drift(repo_root, project_root, work_root)
    manifest_drift = check_manifest_drift(repo_root, manifest_path)
    rows = build_rows(repo_root, project_root, registry_drift, manifest_drift)
    matrix_text = write_csv(args.provenance_output.resolve(), rows)
    evidence: dict[str, Any] = {
        "schema_version": 1,
        "gate": "PCR-GATE-125",
        "marker": "PUBLIC_GENERATED_SOURCE_PROVENANCE",
        "policy": {
            "public_tree_inputs_only": True,
            "private_docs_required": False,
            "git_history_required": False,
            "checked_in_authority_is_evidence_only": True,
            "regeneration_rule_required": True,
            "drift_detection_required": True,
        },
        "provenance_path": args.provenance_output.resolve().name,
        "provenance_sha256": sha256_text(matrix_text),
        "row_count": len(rows),
        "registry_drift": registry_drift,
        "manifest_drift": manifest_drift,
        "rows": rows,
    }
    evidence["evidence_sha256"] = sha256_text(
        json.dumps(evidence, sort_keys=True, separators=(",", ":"))
    )
    return evidence


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, required=True)
    parser.add_argument("--project-root", type=Path, required=True)
    parser.add_argument("--build-root", type=Path, required=True)
    parser.add_argument("--work-root", type=Path, required=True)
    parser.add_argument("--provenance-output", type=Path, required=True)
    parser.add_argument("--evidence-output", type=Path, required=True)
    args = parser.parse_args(argv)

    evidence = build_evidence(args)
    output = args.evidence_output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n",
                      encoding="utf-8")
    print(f"public_generated_source_provenance_rows={evidence['row_count']}")
    print(f"public_generated_source_provenance_sha256={evidence['provenance_sha256']}")
    print("public_generated_provenance_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
