#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Regenerate only the nine ``dml.plan_import_rows`` evidence rows/files.

All four full-table generators run against a temporary artifact copy.  The
candidate is rejected unless every header and every non-selector physical CSV
row remains byte-identical.  Fixture authoring likewise runs in a temporary
root and is copied back only for the exact eighteen selector fixture paths.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import io
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from plan_import_rows_generated_evidence import (
    PLAN_IMPORT_ROWS_CORE_SELECTOR_KEY,
    PLAN_IMPORT_ROWS_SURFACE_IDS,
    authoritative_provenance_inputs,
)


DEFAULT_ARTIFACT_ROOT = (
    "project/tests/sbsql_parser_worker/fixtures/surface_to_sblr/artifacts"
)
GENERATORS = (
    (
        "project/tools/sb_parser_gen/generate_strict_row_coverage_ledger.py",
        "STRICT_ROW_COVERAGE_LEDGER.csv",
    ),
    (
        "project/tools/sb_parser_gen/generate_authenticated_full_route_matrix.py",
        "AUTHENTICATED_FULL_ROUTE_MATRIX.csv",
    ),
    (
        "project/tools/sb_parser_gen/generate_sblr_binary_round_trip_matrix.py",
        "SBLR_BINARY_ROUND_TRIP_MATRIX.csv",
    ),
    (
        "project/tools/sb_parser_gen/generate_per_row_evidence_manifest.py",
        "PER_ROW_EVIDENCE_MANIFEST.csv",
    ),
)
AUTH_MATRIX_NAME = "AUTHENTICATED_FULL_ROUTE_MATRIX.csv"
ROUND_MATRIX_NAME = "SBLR_BINARY_ROUND_TRIP_MATRIX.csv"
AUTHOR_HELPER = "project/tools/sb_parser_gen/author_route_and_round_trip_fixtures.py"
RELEASE_GENERATOR = "project/tools/sb_parser_gen/generate_sbsql_surface_release_declaration.py"
RELEASE_CSV_NAME = "SBSQL_SURFACE_RELEASE_DECLARATION.csv"
RELEASE_JSON_NAME = "SBSQL_SURFACE_RELEASE_DECLARATION.json"
PER_ELEMENT_GENERATOR = "project/tools/sb_parser_gen/generate_per_element_spec_sources.py"
PER_ELEMENT_DIRECTORY = "PER_ELEMENT_CONTRACTS"
HASH_MANIFEST_NAME = "PLAN_IMPORT_ROWS_GENERATED_EVIDENCE_SHA256.csv"
HASH_MANIFEST_COLUMNS = (
    "artifact_path",
    "artifact_kind",
    "surface_id",
    "sha256",
)
WORKPLAN_RELATIVE = "Workplans/sbsql-sblr-implementation-alignment"
PROVENANCE_NAME = "GENERATED_PROVENANCE.csv"
PROVENANCE_ARTIFACT_ID = "IA-GEN-0008"
PROVENANCE_GENERATOR_VERSION = "v1"
PROVENANCE_GENERATED_AT = "2026-08-29T00:57:02Z"
PROVENANCE_COLUMNS = (
    "artifact_id",
    "area_id",
    "artifact_paths",
    "authoritative_inputs",
    "input_sha256_set",
    "generator_path",
    "generator_version",
    "deterministic_invocation",
    "output_sha256_set",
    "generated_at",
    "validation_result",
    "status",
)
SELECTOR_GENERATOR_PATH = (
    "project/tools/sb_parser_gen/refresh_plan_import_rows_generated_evidence.py"
)
SELECTOR_CHECK_INVOCATION = (
    "cwd=ScratchBird;check=python3 "
    f"{SELECTOR_GENERATOR_PATH} --repo-root .;apply=python3 "
    f"{SELECTOR_GENERATOR_PATH} --repo-root . --apply"
)


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def csv_bytes(columns: tuple[str, ...], rows: list[dict[str, str]]) -> bytes:
    buffer = io.StringIO(newline="")
    writer = csv.DictWriter(
        buffer,
        fieldnames=columns,
        lineterminator="\n",
    )
    writer.writeheader()
    writer.writerows(rows)
    return buffer.getvalue().encode("utf-8")


def artifact_manifest_bytes(
    root: Path,
    artifact_root: Path,
    fixture_root: Path,
    auth_paths: dict[str, str],
    round_paths: dict[str, str],
) -> bytes:
    rows: list[dict[str, str]] = []

    def add(
        artifact_path: str,
        artifact_kind: str,
        surface_id: str,
        physical_path: Path,
    ) -> None:
        if not physical_path.is_file():
            fail(f"plan-import provenance output missing: {physical_path}")
        rows.append(
            {
                "artifact_path": artifact_path,
                "artifact_kind": artifact_kind,
                "surface_id": surface_id,
                "sha256": digest(physical_path),
            }
        )

    summary_names = tuple(output for _, output in GENERATORS) + (
        RELEASE_CSV_NAME,
        RELEASE_JSON_NAME,
    )
    for name in summary_names:
        add(
            f"{DEFAULT_ARTIFACT_ROOT}/{name}",
            "summary",
            "",
            artifact_root / name,
        )
    for surface_id in sorted(PLAN_IMPORT_ROWS_SURFACE_IDS):
        add(
            f"{DEFAULT_ARTIFACT_ROOT}/{PER_ELEMENT_DIRECTORY}/{surface_id}.md",
            "per_element_contract",
            surface_id,
            artifact_root / PER_ELEMENT_DIRECTORY / f"{surface_id}.md",
        )
        add(
            auth_paths[surface_id],
            "authenticated_route_fixture",
            surface_id,
            fixture_root / auth_paths[surface_id],
        )
        add(
            round_paths[surface_id],
            "sblr_binary_round_trip_fixture",
            surface_id,
            fixture_root / round_paths[surface_id],
        )
    rows.sort(key=lambda row: row["artifact_path"])
    if len(rows) != 33 or len({row["artifact_path"] for row in rows}) != 33:
        fail("plan-import provenance manifest does not contain exactly 33 artifacts")
    return csv_bytes(HASH_MANIFEST_COLUMNS, rows)


def provenance_row(
    root: Path,
    core_root: Path,
    output_manifest_sha256: str,
) -> dict[str, str]:
    inputs = authoritative_provenance_inputs(root, core_root)
    return {
        "artifact_id": PROVENANCE_ARTIFACT_ID,
        "area_id": "IA-07",
        "artifact_paths": f"{DEFAULT_ARTIFACT_ROOT}/{HASH_MANIFEST_NAME}",
        "authoritative_inputs": ";".join(name for name, _ in inputs),
        "input_sha256_set": ";".join(value for _, value in inputs),
        "generator_path": SELECTOR_GENERATOR_PATH,
        "generator_version": PROVENANCE_GENERATOR_VERSION,
        "deterministic_invocation": SELECTOR_CHECK_INVOCATION,
        "output_sha256_set": output_manifest_sha256,
        "generated_at": PROVENANCE_GENERATED_AT,
        "validation_result": "PASS",
        "status": (
            "accepted_nonfinal_plan_import_rows_generated_evidence_"
            "pending_independent_SBWP_TLS_post_state_proof"
        ),
    }


def replace_physical_csv_row(
    current: Path,
    key_name: str,
    key_value: str,
    columns: tuple[str, ...],
    replacement: dict[str, str],
) -> bytes:
    lines = current.read_bytes().splitlines(keepends=True)
    if not lines or any(not line.endswith(b"\n") for line in lines):
        fail(f"provenance CSV has missing header or noncanonical line ending: {current}")
    header = next(csv.reader([lines[0].decode("utf-8").rstrip("\n")]))
    if tuple(header) != columns:
        fail(f"provenance CSV header drift: {current}")
    replacement_line = csv_bytes(columns, [replacement]).splitlines(keepends=True)[1]
    candidate = [lines[0]]
    replaced = False
    seen: set[str] = set()
    for line in lines[1:]:
        parsed = list(csv.DictReader([lines[0].decode("utf-8"), line.decode("utf-8")]))
        if len(parsed) != 1:
            fail(f"provenance CSV physical row is malformed: {current}")
        row_key = parsed[0].get(key_name, "")
        if not row_key or row_key in seen:
            fail(f"provenance CSV has missing or duplicate {key_name}: {row_key}")
        seen.add(row_key)
        if row_key == key_value:
            candidate.append(replacement_line)
            replaced = True
        else:
            candidate.append(line)
    if not replaced:
        candidate.append(replacement_line)
    return b"".join(candidate)


def run_checked(command: list[str]) -> None:
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        fail(
            "selector regeneration command failed: "
            f"command={command} exit={result.returncode} "
            f"stdout={result.stdout.strip()[:500]} stderr={result.stderr.strip()[:500]}"
        )


def physical_rows(path: Path) -> tuple[bytes, dict[str, bytes]]:
    lines = path.read_bytes().splitlines(keepends=True)
    if not lines:
        fail(f"generated CSV is empty: {path}")
    if any(not line.endswith(b"\n") for line in lines):
        fail(f"generated CSV has a noncanonical line ending: {path}")
    rows: dict[str, bytes] = {}
    for line in lines[1:]:
        surface_id = line.split(b",", 1)[0].decode("ascii")
        if not surface_id.startswith("SBSQL-"):
            fail(f"generated CSV physical row has invalid surface id in {path}: {surface_id}")
        if surface_id in rows:
            fail(f"generated CSV has duplicate physical row {surface_id}: {path}")
        rows[surface_id] = line
    return lines[0], rows


def assert_selector_only_csv(current: Path, candidate: Path) -> int:
    current_header, current_rows = physical_rows(current)
    candidate_header, candidate_rows = physical_rows(candidate)
    if current_header != candidate_header:
        fail(f"selector regeneration changed CSV header: {current}")
    if set(current_rows) != set(candidate_rows):
        fail(f"selector regeneration changed CSV row identity set: {current}")
    unexpected = sorted(
        surface_id
        for surface_id in current_rows
        if surface_id not in PLAN_IMPORT_ROWS_SURFACE_IDS
        and current_rows[surface_id] != candidate_rows[surface_id]
    )
    if unexpected:
        fail(
            f"selector regeneration changed non-target rows in {current}: "
            f"count={len(unexpected)} first={unexpected[:5]}"
        )
    changed_targets = sum(
        current_rows[surface_id] != candidate_rows[surface_id]
        for surface_id in PLAN_IMPORT_ROWS_SURFACE_IDS
    )
    return changed_targets


def assert_selector_only_per_element_tree(current: Path, candidate: Path) -> int:
    current_files = {
        path.relative_to(current).as_posix(): path
        for path in current.rglob("*")
        if path.is_file()
    }
    candidate_files = {
        path.relative_to(candidate).as_posix(): path
        for path in candidate.rglob("*")
        if path.is_file()
    }
    if set(current_files) != set(candidate_files):
        fail("selector regeneration changed per-element output file identity set")
    target_names = {f"{surface_id}.md" for surface_id in PLAN_IMPORT_ROWS_SURFACE_IDS}
    unexpected = sorted(
        name
        for name in current_files
        if name not in target_names
        and current_files[name].read_bytes() != candidate_files[name].read_bytes()
    )
    if unexpected:
        fail(
            "selector regeneration changed non-target per-element files: "
            f"count={len(unexpected)} first={unexpected[:5]}"
        )
    return sum(
        current_files[name].read_bytes() != candidate_files[name].read_bytes()
        for name in target_names
    )


def matrix_fixture_paths(path: Path) -> dict[str, str]:
    import csv

    with path.open(newline="", encoding="utf-8") as handle:
        rows = {
            row["surface_id"]: row["fixture_path"]
            for row in csv.DictReader(handle)
            if row.get("surface_id") in PLAN_IMPORT_ROWS_SURFACE_IDS
        }
    if set(rows) != set(PLAN_IMPORT_ROWS_SURFACE_IDS):
        fail(f"selector matrix does not contain the exact nine surface ids: {path}")
    return rows


def parse_fixture(path: Path) -> dict[str, str]:
    fields: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#") or line.startswith((" ", "-")):
            continue
        if ":" not in line:
            continue
        key, raw = line.split(":", 1)
        value = raw.strip()
        fields[key] = json.loads(value) if value.startswith('"') else value
    return fields


def fixture_snapshot(root: Path, fixture_paths: set[str]) -> dict[str, str]:
    return {
        fixture_path: digest(root / fixture_path)
        for fixture_path in fixture_paths
        if (root / fixture_path).is_file()
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--artifact-root", default=DEFAULT_ARTIFACT_ROOT)
    parser.add_argument(
        "--apply",
        action="store_true",
        help="Copy the validated selector-only candidate into the working tree.",
    )
    args = parser.parse_args()
    root = Path(args.repo_root).resolve()
    core_root = root.parent / "Specifications/Core"
    workplan_root = root.parent / WORKPLAN_RELATIVE
    provenance_path = workplan_root / PROVENANCE_NAME
    if not core_root.is_dir():
        fail(f"manifest-listed Core root missing: {core_root}")
    if not provenance_path.is_file():
        fail(f"workplan provenance registry missing: {provenance_path}")
    artifact_root = Path(args.artifact_root)
    if not artifact_root.is_absolute():
        artifact_root = root / artifact_root
    if not artifact_root.is_dir():
        fail(f"artifact root missing: {artifact_root}")
    if args.apply and artifact_root != root / DEFAULT_ARTIFACT_ROOT:
        fail("--apply requires the canonical plan-import artifact root")

    with tempfile.TemporaryDirectory(prefix="sbsql_plan_import_rows_") as temp:
        temp_root = Path(temp)
        temp_artifact_root = temp_root / "artifacts"
        temp_fixture_root = temp_root / "repo"
        shutil.copytree(artifact_root, temp_artifact_root)

        changed_by_output: dict[str, int] = {}
        for generator_rel, output_name in GENERATORS:
            run_checked(
                [
                    sys.executable,
                    str(root / generator_rel),
                    "--repo-root",
                    str(root),
                    "--artifact-root",
                    str(temp_artifact_root),
                ]
            )
            changed_by_output[output_name] = assert_selector_only_csv(
                artifact_root / output_name, temp_artifact_root / output_name
            )

        run_checked(
            [
                sys.executable,
                str(root / RELEASE_GENERATOR),
                "--repo-root",
                str(root),
                "--artifact-root",
                str(temp_artifact_root),
            ]
        )
        changed_by_output[RELEASE_CSV_NAME] = assert_selector_only_csv(
            artifact_root / RELEASE_CSV_NAME,
            temp_artifact_root / RELEASE_CSV_NAME,
        )
        release_summary = json.loads(
            (temp_artifact_root / RELEASE_JSON_NAME).read_text(encoding="utf-8")
        )
        if (
            release_summary.get("status") != "blocked"
            or release_summary.get("blocked_rows") != len(PLAN_IMPORT_ROWS_SURFACE_IDS)
            or release_summary.get("final_status_counts", {}).get("pending")
            != len(PLAN_IMPORT_ROWS_SURFACE_IDS)
            or release_summary.get("remaining_risk_rows")
            != len(PLAN_IMPORT_ROWS_SURFACE_IDS)
            or release_summary.get("authenticated_route_pending_rows")
            != len(PLAN_IMPORT_ROWS_SURFACE_IDS)
            or release_summary.get("sblr_round_trip_pending_rows")
            != len(PLAN_IMPORT_ROWS_SURFACE_IDS)
        ):
            fail(f"selector release summary does not expose exactly nine blocked rows: {release_summary}")

        run_checked(
            [
                sys.executable,
                str(root / PER_ELEMENT_GENERATOR),
                "--repo-root",
                str(root),
                "--artifact-root",
                str(temp_artifact_root),
            ]
        )
        per_element_changed = assert_selector_only_per_element_tree(
            artifact_root / PER_ELEMENT_DIRECTORY,
            temp_artifact_root / PER_ELEMENT_DIRECTORY,
        )

        auth_paths = matrix_fixture_paths(temp_artifact_root / AUTH_MATRIX_NAME)
        round_paths = matrix_fixture_paths(temp_artifact_root / ROUND_MATRIX_NAME)
        target_fixture_paths = set(auth_paths.values()) | set(round_paths.values())
        if len(target_fixture_paths) != 18:
            fail(
                "selector fixture path set is not exact: "
                f"expected=18 observed={len(target_fixture_paths)}"
            )
        for fixture_path in sorted(target_fixture_paths):
            source = root / fixture_path
            if source.is_file():
                target = temp_fixture_root / fixture_path
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source, target)

        author_command = [
            sys.executable,
            str(root / AUTHOR_HELPER),
            "--repo-root",
            str(temp_fixture_root),
            "--artifact-root",
            str(temp_artifact_root),
            "--refresh",
            "--limit",
            str(len(PLAN_IMPORT_ROWS_SURFACE_IDS)),
        ]
        for surface_id in sorted(PLAN_IMPORT_ROWS_SURFACE_IDS):
            author_command.extend(("--surface-id", surface_id))
        run_checked(author_command)

        for surface_id in sorted(PLAN_IMPORT_ROWS_SURFACE_IDS):
            for fixture_path in (auth_paths[surface_id], round_paths[surface_id]):
                candidate = temp_fixture_root / fixture_path
                if not candidate.is_file():
                    fail(f"selector fixture authoring did not create {fixture_path}")
                fields = parse_fixture(candidate)
                if fields.get("surface_id") != surface_id:
                    fail(f"selector fixture surface-id drift: {fixture_path}")
                if fields.get("fixture_status") != "fixture_authored":
                    fail(f"selector fixture is not fixture_authored: {fixture_path}")
                if fields.get("per_row_final_state") != "pending":
                    fail(f"selector fixture overstates final state: {fixture_path}")

        output_manifest = artifact_manifest_bytes(
            root,
            temp_artifact_root,
            temp_fixture_root,
            auth_paths,
            round_paths,
        )
        (temp_artifact_root / HASH_MANIFEST_NAME).write_bytes(output_manifest)
        manifest_path = artifact_root / HASH_MANIFEST_NAME
        hash_manifest_changed = (
            not manifest_path.is_file()
            or manifest_path.read_bytes() != output_manifest
        )
        candidate_provenance = replace_physical_csv_row(
            provenance_path,
            "artifact_id",
            PROVENANCE_ARTIFACT_ID,
            PROVENANCE_COLUMNS,
            provenance_row(
                root,
                core_root,
                hashlib.sha256(output_manifest).hexdigest(),
            ),
        )
        provenance_changed = provenance_path.read_bytes() != candidate_provenance
        release_json_changed = (
            (artifact_root / RELEASE_JSON_NAME).read_bytes()
            != (temp_artifact_root / RELEASE_JSON_NAME).read_bytes()
        )

        if args.apply:
            before_fixtures = fixture_snapshot(root, target_fixture_paths)
            for _, output_name in GENERATORS:
                shutil.copyfile(
                    temp_artifact_root / output_name, artifact_root / output_name
                )
            shutil.copyfile(
                temp_artifact_root / RELEASE_CSV_NAME,
                artifact_root / RELEASE_CSV_NAME,
            )
            shutil.copyfile(
                temp_artifact_root / RELEASE_JSON_NAME,
                artifact_root / RELEASE_JSON_NAME,
            )
            shutil.copyfile(
                temp_artifact_root / HASH_MANIFEST_NAME,
                artifact_root / HASH_MANIFEST_NAME,
            )
            for surface_id in sorted(PLAN_IMPORT_ROWS_SURFACE_IDS):
                name = f"{surface_id}.md"
                shutil.copyfile(
                    temp_artifact_root / PER_ELEMENT_DIRECTORY / name,
                    artifact_root / PER_ELEMENT_DIRECTORY / name,
                )
            for fixture_path in sorted(target_fixture_paths):
                target = root / fixture_path
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(temp_fixture_root / fixture_path, target)
            after_fixtures = fixture_snapshot(root, target_fixture_paths)
            changed_fixtures = sorted(
                path
                for path in target_fixture_paths
                if before_fixtures.get(path) != after_fixtures.get(path)
            )
            provenance_path.write_bytes(candidate_provenance)
        else:
            changed_fixtures = sorted(
                path
                for path in target_fixture_paths
                if not (root / path).is_file()
                or (root / path).read_bytes() != (temp_fixture_root / path).read_bytes()
            )

        drifted = (
            any(changed_by_output.values())
            or release_json_changed
            or per_element_changed != 0
            or bool(changed_fixtures)
            or hash_manifest_changed
            or provenance_changed
        )
        print(
            "plan_import_rows_generated_evidence=validated "
            f"apply={args.apply} surfaces={len(PLAN_IMPORT_ROWS_SURFACE_IDS)} "
            f"target_fixture_files_changed={len(changed_fixtures)} "
            f"per_element_target_files_changed={per_element_changed} "
            f"release_json_changed={int(release_json_changed)} "
            f"hash_manifest_changed={int(hash_manifest_changed)} "
            f"provenance_changed={int(provenance_changed)} "
            + " ".join(
                f"{name}_target_rows_changed={count}"
                for name, count in changed_by_output.items()
            )
        )
        if drifted and not args.apply:
            fail("plan-import selector outputs or provenance are stale; run exact --apply invocation")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
