#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Generate public SBSQL per-element contract snapshots.

Every generated file represents exactly one surface row.  The generator reads
only tracked public CSV artifacts and writes a deterministic, generator-owned
``PER_ELEMENT_CONTRACTS/SBSQL-<id>.md`` tree below the public release artifact
root.  It deliberately does not read the old canonicalization snapshots or
emit implementation/source-tree or private paths.
"""

from __future__ import annotations

import argparse
import csv
import re
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
FULL_SURFACE_ARTIFACT_ROOT = (
    "project/tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/artifacts"
)
PUBLIC_RELEASE_ARTIFACT_ROOT = (
    "project/tests/sbsql_parser_worker/fixtures/surface_to_sblr/artifacts"
)
OUTPUT_DIRECTORY_NAME = "PER_ELEMENT_CONTRACTS"
SURFACE_FILE_RE = re.compile(r"SBSQL-[0-9A-F]{12}\.md\Z")
SURFACE_ID_RE = re.compile(r"SBSQL-[0-9A-F]{12}\Z")
FORBIDDEN_OUTPUT_PATH_TOKENS = (
    "public_input_snapshot",
    "public_contract_snapshot",
    "ScratchBird-Private",
    "/home/",
    "project/src/",
    "project/tools/",
)

BACKLOG_COLUMNS = (
    "surface_id",
    "fixed_uuid_v7",
    "canonical_name",
    "surface_kind",
    "family",
    "source_status",
    "cluster_scope",
    "sblr_operation_family",
    "source_search_key",
    "diagnostic_target",
    "validation_fixture_id",
    "final_acceptance_rule",
    "closure_action",
    "status",
)
ORACLE_COLUMNS = (
    "fixture_id",
    "surface_id",
    "oracle_type",
    "source_search_key",
    "expected_result_summary",
    "status",
)
RELEASE_COLUMNS = (
    "surface_id",
    "fixed_uuid_v7",
    "canonical_name",
    "surface_kind",
    "family",
    "final_status",
    "release_claim",
    "remaining_risk",
    "release_status",
)
IDENTITY_COLUMNS = ("fixed_uuid_v7", "canonical_name", "surface_kind", "family")
ALLOWED_CLOSURE_STATUS_PAIRS = {
    ("e2e_passed", "e2e_passed"),
    ("exact_refusal_passed", "cluster_provider_route_passed"),
    ("e2e_passed", "cluster_provider_route_passed"),
}


def fail(message: str) -> None:
    print(f"per_element_specs=failed: {message}", file=sys.stderr)
    raise SystemExit(1)


def relative_to(path: Path, root: Path) -> bool:
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def repo_path(root: Path, value: Path | str, label: str) -> Path:
    candidate = Path(value)
    if not candidate.is_absolute():
        candidate = root / candidate
    resolved = candidate.resolve()
    if not relative_to(resolved, root):
        fail(f"{label} must stay inside --repo-root: {candidate}")
    return resolved


def artifact_root_path(root: Path, value: Path | str, label: str) -> Path:
    """Resolve a public artifact directory.

    A deterministic gate supplies a copied artifact tree outside the repository,
    so this intentionally permits an absolute path.  The generator never writes
    outside that directory and never serializes the path into its output.
    """

    candidate = Path(value)
    if not candidate.is_absolute():
        candidate = root / candidate
    if candidate.is_symlink():
        fail(f"{label} must not be a symlink: {candidate}")
    resolved = candidate.resolve()
    if not resolved.is_dir():
        fail(f"{label} must be a non-symlink directory: {candidate}")
    return resolved


def output_root_path(artifact_root: Path, value: str) -> Path:
    relative = Path(value)
    if relative.is_absolute() or relative == Path("."):
        fail("--output-root must name a child directory below --artifact-root")
    unresolved_output_root = artifact_root / relative
    cursor = artifact_root
    for part in relative.parts:
        cursor = cursor / part
        if cursor.exists() and cursor.is_symlink():
            fail("--output-root must not traverse a symlink")
    output_root = unresolved_output_root.resolve()
    if not relative_to(output_root, artifact_root):
        fail("--output-root must not escape --artifact-root")
    if output_root.exists() and output_root.is_symlink():
        fail("--output-root must not be a symlink")
    return output_root


def require_regular_file(path: Path, label: str) -> None:
    if path.is_symlink() or not path.is_file():
        fail(f"required tracked public artifact missing or not regular: {label}")


def read_csv(path: Path, label: str, required_columns: tuple[str, ...]) -> list[dict[str, str]]:
    require_regular_file(path, label)
    with path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        fieldnames = tuple(reader.fieldnames or ())
        missing = [column for column in required_columns if column not in fieldnames]
        if missing:
            fail(f"{label} missing columns: {', '.join(missing)}")
        rows = list(reader)
    if not rows:
        fail(f"{label} has no rows")
    return rows


def index_unique(
    rows: list[dict[str, str]],
    key: str,
    label: str,
    required_columns: tuple[str, ...],
) -> dict[str, dict[str, str]]:
    indexed: dict[str, dict[str, str]] = {}
    for row_number, row in enumerate(rows, start=2):
        for column in required_columns:
            if not row.get(column, ""):
                fail(f"{label} row {row_number} missing {column}")
        value = row[key]
        if value in indexed:
            fail(f"{label} duplicate {key}: {value}")
        indexed[value] = row
    return indexed


def markdown_cell(value: str) -> str:
    return value.replace("\\", "\\\\").replace("|", "\\|").replace("\r", " ").replace("\n", " ")


def markdown_heading(value: str) -> str:
    return value.replace("\r", " ").replace("\n", " ")


def validate_inputs(
    backlog: dict[str, dict[str, str]],
    oracles: dict[str, dict[str, str]],
    release: dict[str, dict[str, str]],
) -> None:
    expected_ids = set(backlog)
    for label, indexed in (("SEMANTIC_ORACLE_AUTHORITY_MAP.csv", oracles),
                           ("SBSQL_SURFACE_RELEASE_DECLARATION.csv", release)):
        missing = sorted(expected_ids - set(indexed))
        unexpected = sorted(set(indexed) - expected_ids)
        if missing or unexpected:
            detail = []
            if missing:
                detail.append(f"missing={','.join(missing[:5])}")
            if unexpected:
                detail.append(f"unexpected={','.join(unexpected[:5])}")
            fail(f"{label} surface membership mismatch: {' '.join(detail)}")

    for surface_id in sorted(expected_ids):
        if not SURFACE_ID_RE.fullmatch(surface_id):
            fail(f"invalid surface_id for generated filename: {surface_id}")
        backlog_row = backlog[surface_id]
        oracle_row = oracles[surface_id]
        release_row = release[surface_id]
        for column in IDENTITY_COLUMNS:
            if backlog_row[column] != release_row[column]:
                fail(
                    f"{surface_id} identity mismatch for {column}: "
                    "SURFACE_IMPLEMENTATION_BACKLOG.csv and "
                    "SBSQL_SURFACE_RELEASE_DECLARATION.csv disagree"
                )
        if backlog_row["source_search_key"] != oracle_row["source_search_key"]:
            fail(f"{surface_id} source_search_key disagreement between public artifacts")
        if backlog_row["validation_fixture_id"] != oracle_row["fixture_id"]:
            fail(f"{surface_id} fixture identifier disagreement between public artifacts")
        status_pair = (backlog_row["status"], release_row["final_status"])
        if status_pair not in ALLOWED_CLOSURE_STATUS_PAIRS:
            fail(
                f"{surface_id} unsupported public closure-status pair: "
                f"backlog={status_pair[0]} release={status_pair[1]}"
            )
        if (
            status_pair == ("e2e_passed", "cluster_provider_route_passed")
            and backlog_row["cluster_scope"] != "cluster_private"
        ):
            fail(
                f"{surface_id} non-cluster row cannot promote an e2e backlog "
                "status to a cluster-provider release route"
            )


def readme_body(count: int) -> str:
    return "\n".join(
        (
            "# SBSQL Public Per-Element Contract Snapshots",
            "",
            "This generator-owned directory contains one deterministic Markdown "
            "snapshot for each published SBSQL surface row.",
            "",
            "## Inputs",
            "",
            "- `SURFACE_IMPLEMENTATION_BACKLOG.csv`",
            "- `SEMANTIC_ORACLE_AUTHORITY_MAP.csv`",
            "- `SBSQL_SURFACE_RELEASE_DECLARATION.csv`",
            "",
            "All inputs are tracked public release artifacts. The generator reads "
            "them without mutation and does not require network access.",
            "",
            "## Layout",
            "",
            "Each `SBSQL-<12 uppercase hexadecimal digits>.md` file contains one "
            "surface identity, route, closure, and oracle snapshot. There is no "
            "shared per-surface output file.",
            "",
            f"Published surface snapshots: {count}",
            "",
            "## Boundaries",
            "",
            "These files are generated public evidence. They do not execute SQL, "
            "do not grant parser or engine authority, and contain no private "
            "canonicalization or source-tree path references.",
            "",
        )
    )


def spec_body(
    backlog: dict[str, str],
    oracle: dict[str, str],
    release: dict[str, str],
) -> str:
    title = markdown_heading(backlog["canonical_name"])
    identity_rows = (
        ("Surface ID", backlog["surface_id"]),
        ("Fixed UUID v7", backlog["fixed_uuid_v7"]),
        ("Canonical name", backlog["canonical_name"]),
        ("Surface kind", backlog["surface_kind"]),
        ("Family", backlog["family"]),
    )
    route_rows = (
        ("Source status", backlog["source_status"]),
        ("Cluster scope", backlog["cluster_scope"]),
        ("SBLR operation family", backlog["sblr_operation_family"]),
        ("Diagnostic target", backlog["diagnostic_target"]),
        ("Final acceptance rule", backlog["final_acceptance_rule"]),
        ("Closure action", backlog["closure_action"]),
    )
    closure_rows = (
        ("Backlog closure status", backlog["status"]),
        ("Release final status", release["final_status"]),
        ("Release claim", release["release_claim"]),
        ("Release status", release["release_status"]),
        ("Remaining risk", release["remaining_risk"]),
    )
    oracle_rows = (
        ("Fixture ID", oracle["fixture_id"]),
        ("Oracle type", oracle["oracle_type"]),
        ("Oracle search key", oracle["source_search_key"]),
        ("Expected result summary", oracle["expected_result_summary"]),
        ("Oracle closure status", oracle["status"]),
    )

    def table(rows: tuple[tuple[str, str], ...]) -> list[str]:
        return ["| Field | Value |", "| --- | --- |"] + [
            f"| {name} | {markdown_cell(value)} |" for name, value in rows
        ]

    lines = [
        f"# {backlog['surface_id']} — {title}",
        "",
        "Generated public per-element contract snapshot.",
        "",
        "## Identity",
        "",
        *table(identity_rows),
        "",
        "## Route Contract",
        "",
        *table(route_rows),
        "",
        "## Release Closure",
        "",
        *table(closure_rows),
        "",
        "## Semantic Oracle",
        "",
        *table(oracle_rows),
        "",
        "## Boundary",
        "",
        "- This snapshot is derived only from tracked public release artifacts.",
        "- SQL text remains parser-side input; engine behavior is reached through "
        "the published SBLR/internal-API contract.",
        "- This snapshot carries no implementation, source-tree, absolute, or "
        "private canonicalization path.",
        "",
    ]
    return "\n".join(lines)


def expected_output(
    backlog: dict[str, dict[str, str]],
    oracles: dict[str, dict[str, str]],
    release: dict[str, dict[str, str]],
) -> dict[str, str]:
    outputs = {"README.md": readme_body(len(backlog))}
    for surface_id in sorted(backlog):
        name = f"{surface_id}.md"
        if not SURFACE_FILE_RE.fullmatch(name):
            fail(f"invalid generated per-element filename: {name}")
        outputs[name] = spec_body(
            backlog[surface_id], oracles[surface_id], release[surface_id]
        )
    for name, body in outputs.items():
        for token in FORBIDDEN_OUTPUT_PATH_TOKENS:
            if token in body:
                fail(f"generated output {name} contains forbidden path token: {token}")
    return outputs


def existing_output_names(output_root: Path) -> set[str]:
    if not output_root.exists():
        return set()
    if not output_root.is_dir() or output_root.is_symlink():
        fail("per-element output root must be a non-symlink directory")
    names: set[str] = set()
    for path in output_root.iterdir():
        if path.is_symlink() or not path.is_file():
            fail(f"unexpected non-regular output entry: {path.name}")
        names.add(path.name)
    return names


def output_drift(output_root: Path, expected: dict[str, str]) -> list[str]:
    existing_names = existing_output_names(output_root)
    unexpected = sorted(existing_names - set(expected))
    drift = [f"unexpected_per_element_spec={name}" for name in unexpected]
    for name, body in expected.items():
        path = output_root / name
        existing = path.read_text(encoding="utf-8") if path.is_file() else None
        if existing != body:
            drift.append(f"stale_per_element_spec={name}")
    return drift


def write_output(output_root: Path, expected: dict[str, str]) -> int:
    existing_names = existing_output_names(output_root)
    unexpected = sorted(existing_names - set(expected))
    if unexpected:
        fail(
            "output tree contains entries not owned by this generator: "
            + ", ".join(unexpected[:10])
        )
    output_root.mkdir(parents=True, exist_ok=True)
    changed = 0
    for name, body in expected.items():
        path = output_root / name
        existing = path.read_text(encoding="utf-8") if path.is_file() else None
        if existing != body:
            path.write_text(body, encoding="utf-8", newline="\n")
            changed += 1
    return changed


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=REPO_ROOT)
    parser.add_argument(
        "--full-surface-artifact-root",
        default=FULL_SURFACE_ARTIFACT_ROOT,
        help="Tracked public full-surface artifact directory containing backlog and oracle CSVs.",
    )
    parser.add_argument(
        "--artifact-root",
        default=PUBLIC_RELEASE_ARTIFACT_ROOT,
        help="Tracked public release artifact directory containing the release declaration and output tree.",
    )
    parser.add_argument(
        "--output-root",
        default=OUTPUT_DIRECTORY_NAME,
        help="Relative generator-owned output directory below --artifact-root.",
    )
    parser.add_argument("--check", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    root = Path(args.repo_root).resolve()
    if not root.is_dir():
        fail(f"--repo-root is not a directory: {args.repo_root}")
    full_surface_root = repo_path(root, args.full_surface_artifact_root, "--full-surface-artifact-root")
    artifact_root = artifact_root_path(root, args.artifact_root, "--artifact-root")
    output_root = output_root_path(artifact_root, args.output_root)

    backlog = index_unique(
        read_csv(
            full_surface_root / "SURFACE_IMPLEMENTATION_BACKLOG.csv",
            "SURFACE_IMPLEMENTATION_BACKLOG.csv",
            BACKLOG_COLUMNS,
        ),
        "surface_id",
        "SURFACE_IMPLEMENTATION_BACKLOG.csv",
        BACKLOG_COLUMNS,
    )
    oracles = index_unique(
        read_csv(
            full_surface_root / "SEMANTIC_ORACLE_AUTHORITY_MAP.csv",
            "SEMANTIC_ORACLE_AUTHORITY_MAP.csv",
            ORACLE_COLUMNS,
        ),
        "surface_id",
        "SEMANTIC_ORACLE_AUTHORITY_MAP.csv",
        ORACLE_COLUMNS,
    )
    release = index_unique(
        read_csv(
            artifact_root / "SBSQL_SURFACE_RELEASE_DECLARATION.csv",
            "SBSQL_SURFACE_RELEASE_DECLARATION.csv",
            RELEASE_COLUMNS,
        ),
        "surface_id",
        "SBSQL_SURFACE_RELEASE_DECLARATION.csv",
        RELEASE_COLUMNS,
    )
    validate_inputs(backlog, oracles, release)
    expected = expected_output(backlog, oracles, release)

    if args.check:
        drift = output_drift(output_root, expected)
        if drift:
            for finding in drift[:20]:
                print(finding)
            print(f"per_element_specs=stale count={len(drift)}")
            return 1
        print(f"per_element_specs=verified rows={len(backlog)} changed=0")
        return 0

    changed = write_output(output_root, expected)
    print(f"per_element_specs=generated rows={len(backlog)} changed={changed}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
