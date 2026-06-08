#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Materialize canonical SBSQL per-element contract sources.

The semantic-oracle authority map points every surface fixture at a stable
``public_contract_snapshot`` document. This generator mirrors the
canonical registry, SBLR operation matrix, backlog, and oracle authority into
those durable documentation paths so proof gates validate independent source
files rather than generated implementation output.
"""

from __future__ import annotations

import argparse
import csv
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
CANONICALIZATION_ROOT = (
    "public_input_snapshot"
)
FULL_SURFACE_ARTIFACT_ROOT = (
    "project/tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/artifacts"
)


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def index_unique(rows: list[dict[str, str]], key: str, label: str) -> dict[str, dict[str, str]]:
    out: dict[str, dict[str, str]] = {}
    for row in rows:
        value = row.get(key, "")
        if not value:
            raise ValueError(f"{label} row missing {key}")
        if value in out:
            raise ValueError(f"{label} duplicate {key}: {value}")
        out[value] = row
    return out


def markdown_cell(value: str) -> str:
    return value.replace("\\", "\\\\").replace("|", "\\|").replace("\n", " ")


def spec_body(
    surface: dict[str, str],
    operation: dict[str, str],
    backlog: dict[str, str],
    oracle: dict[str, str],
) -> str:
    title = surface["canonical_name"].replace("_", " ").title()
    rows = [
        ("Surface ID", surface["surface_id"]),
        ("Fixed UUID v7", surface["fixed_uuid_v7"]),
        ("Canonical name", surface["canonical_name"]),
        ("Family", surface["family"]),
        ("Surface kind", surface["surface_kind"]),
        ("Source status", surface["status"]),
        ("Cluster scope", surface["cluster_scope"]),
        ("SBLR operation family", operation["sblr_operation_family"]),
        ("Required context", operation["required_context"]),
        ("Binding steps", operation["binding_steps"]),
        ("Result shape", operation["result_shape"]),
        ("Diagnostics", operation["diagnostics"]),
        ("Diagnostic target", backlog["diagnostic_target"]),
        ("Final acceptance rule", backlog["final_acceptance_rule"]),
        ("Oracle type", oracle["oracle_type"]),
        ("Oracle source", oracle["oracle_source"]),
        ("Oracle search key", oracle["source_search_key"]),
        ("Expected result summary", oracle["expected_result_summary"]),
        ("Fixture ID", oracle["fixture_id"]),
    ]

    lines = [
        f"# {title}",
        "",
        "Status: canonical",
        f"Search key: `{surface['surface_id']}`",
        "",
        "## Authority",
        "",
        "| Field | Value |",
        "| --- | --- |",
    ]
    lines.extend(f"| {name} | {markdown_cell(value)} |" for name, value in rows)
    lines.extend(
        [
            "",
            "## Route Contract",
            "",
            "This per-element authority is generated from the canonical SBSQL "
            "surface registry, SBLR operation matrix, surface backlog, and "
            "semantic-oracle authority map. The engine contract remains SBLR "
            "and internal API based; SQL text is parser-side input only.",
            "",
            "## Closure",
            "",
            "- Parser, binder, lowering, server admission, engine behavior, and "
            "diagnostic expectations are reconciled by the generated proof "
            "suite.",
            "- Cluster-private or future-native rows must fail closed unless a "
            "matching profile or promotion rule is present.",
            "- This document is the stable contract source referenced by "
            "`SEMANTIC_ORACLE_AUTHORITY_MAP.csv`.",
            "",
        ]
    )
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=REPO_ROOT)
    parser.add_argument("--canonicalization-root", default=CANONICALIZATION_ROOT)
    parser.add_argument("--artifact-root", default=FULL_SURFACE_ARTIFACT_ROOT)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    root = args.repo_root
    canonicalization_root = root / args.canonicalization_root
    artifact_root = root / args.artifact_root

    surfaces = read_csv(canonicalization_root / "SBSQL_SURFACE_REGISTRY.csv")
    operations = index_unique(
        read_csv(canonicalization_root / "SBSQL_TO_SBLR_OPERATION_MATRIX.csv"),
        "surface_id",
        "SBSQL_TO_SBLR_OPERATION_MATRIX",
    )
    backlog = index_unique(
        read_csv(artifact_root / "SURFACE_IMPLEMENTATION_BACKLOG.csv"),
        "surface_id",
        "SURFACE_IMPLEMENTATION_BACKLOG",
    )
    oracles = index_unique(
        read_csv(artifact_root / "SEMANTIC_ORACLE_AUTHORITY_MAP.csv"),
        "surface_id",
        "SEMANTIC_ORACLE_AUTHORITY_MAP",
    )

    changed: list[Path] = []
    for surface in surfaces:
        sid = surface["surface_id"]
        spec_path = surface["canonical_spec"].split("#", 1)[0]
        if not spec_path.startswith("public_contract_snapshot"):
            raise ValueError(f"{sid}: canonical spec is outside per-element docs: {spec_path}")
        if sid not in operations or sid not in backlog or sid not in oracles:
            raise ValueError(f"{sid}: missing per-element authority source row")

        output_path = root / spec_path
        body = spec_body(surface, operations[sid], backlog[sid], oracles[sid])
        existing = output_path.read_text(encoding="utf-8") if output_path.exists() else None
        if existing != body:
            changed.append(output_path)
            if not args.check:
                output_path.parent.mkdir(parents=True, exist_ok=True)
                output_path.write_text(body, encoding="utf-8", newline="\n")

    if args.check and changed:
        for path in changed[:20]:
            print(f"stale_per_element_spec={path.relative_to(root).as_posix()}")
        print(f"per_element_specs=stale count={len(changed)}")
        return 1

    status = "verified" if args.check else "generated"
    print(f"per_element_specs={status} rows={len(surfaces)} changed={len(changed)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
