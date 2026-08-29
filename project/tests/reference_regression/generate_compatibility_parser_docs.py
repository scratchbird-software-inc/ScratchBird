#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Generate and verify public compatibility parser status documentation."""

from __future__ import annotations

import argparse
import csv
import difflib
import hashlib
import json
from pathlib import Path
from typing import Any


STATUS_REL = Path("docs/compatibility-parsers/compatibility-parser-status.csv")
DOC_ROOT_REL = Path("docs/compatibility-parsers/parsers")
REMAP_MATRIX_REL = Path("docs/compatibility-parsers/remap/COMPATIBILITY_PARSER_REMAP_MATRIX.csv")


def md_escape(value: str) -> str:
    return (
        (value or "")
        .replace("\\", "\\\\")
        .replace("|", "\\|")
        .replace("\n", " ")
        .replace("\r", " ")
    )


def status_label(runtime_disposition: str) -> str:
    mapping = {
        "admitted_sblr_or_parser_support_route": "Supported through ScratchBird SBLR or parser-support route",
        "admitted_normalized_cluster_sblr_provider_boundary": "Routed to cluster provider boundary",
        "documentation_evidence_only": "Documented compatibility behavior",
        "exact_fail_closed_refusal": "Explicit fail-closed refusal",
        "exact_pre_provider_refusal": "Explicit pre-provider refusal",
        "fail_closed_external_authority": "Explicit external-authority refusal",
    }
    return mapping.get(runtime_disposition, runtime_disposition)


def family_from_package(package: str) -> str | None:
    parser_prefix = "project/src/parsers/compatibility/"
    if package.startswith(parser_prefix):
        return package[len(parser_prefix):].split("/", 1)[0]
    udr_prefix = "project/src/udr/sbu_"
    udr_suffix = "_parser_support"
    if package.startswith(udr_prefix):
        rest = package[len(udr_prefix):].split("/", 1)[0]
        if rest.endswith(udr_suffix):
            return rest[: -len(udr_suffix)]
    return None


def diagnostic_from_route(row: dict[str, str]) -> str:
    for field in ("current_route", "refusal_policy"):
        for part in row.get(field, "").split(";"):
            if part.startswith("diagnostic="):
                return part.split("=", 1)[1]
    refusal = row.get("refusal_policy", "")
    if refusal and refusal != "not_refusal":
        return refusal
    return "none"


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8-sig") as handle:
        return list(csv.DictReader(handle))


def public_lanes(repo_root: Path) -> list[dict[str, str]]:
    rows = read_csv(repo_root / STATUS_REL)
    if not rows:
        raise RuntimeError(f"empty compatibility parser status file: {STATUS_REL}")
    return rows


def matrix_by_family(repo_root: Path) -> dict[str, list[dict[str, str]]]:
    grouped: dict[str, list[dict[str, str]]] = {}
    for row in read_csv(repo_root / REMAP_MATRIX_REL):
        family = family_from_package(row["parser_package"])
        if not family:
            raise RuntimeError(f"cannot derive parser family from {row['parser_package']}")
        grouped.setdefault(family, []).append(row)
    for rows in grouped.values():
        rows.sort(key=lambda item: int(item["declared_row_ordinal"]))
    return grouped


def digest_rows(rows: list[dict[str, str]]) -> str:
    digest = hashlib.sha256()
    for row in rows:
        digest.update(row["declared_row_id"].encode("utf-8"))
        digest.update(b"\0")
        digest.update(row["declared_surface"].encode("utf-8"))
        digest.update(b"\0")
        digest.update(row["runtime_disposition"].encode("utf-8"))
        digest.update(b"\0")
        digest.update(row["status"].encode("utf-8"))
        digest.update(b"\n")
    return digest.hexdigest()


def render_parser_page(lane: dict[str, str], rows: list[dict[str, str]]) -> str:
    family_id = lane["family_id"]
    display_name = lane["display_name"]
    dispositions: dict[str, int] = {}
    classifications: dict[str, int] = {}
    for row in rows:
        dispositions[row["runtime_disposition"]] = dispositions.get(row["runtime_disposition"], 0) + 1
        classifications[row["classification"]] = classifications.get(row["classification"], 0) + 1

    lines = [
        f"# {display_name} Compatibility Parser Status",
        "",
        "<!-- AUTO-GENERATED: compatibility parser status. Regenerate with",
        "python3 project/tests/reference_regression/generate_compatibility_parser_docs.py --repo-root . --write",
        "-->",
        "",
        f"Parser family: `{family_id}`",
        "",
        f"Reference profile: `{lane['reference_profile']}`",
        "",
        f"Release batch: `{lane['batch']}`",
        "",
        f"Retained pre-hold beta evidence status: `{lane['public_beta_status']}`",
        "",
        f"Declared public surfaces covered: `{len(rows)}`",
        "",
        f"Surface digest: `{digest_rows(rows)}`",
        "",
        "This page is generated from the public compatibility parser remap matrix. "
        "Its status and support wording records the last verified pre-hold SBLR "
        "baseline; it is historical evidence and is not a claim of executable "
        "conformance to the in-progress SBLR contract. Every row below is a declared "
        "beta parser surface and states whether it was supported through ScratchBird "
        "SBLR/parser-support routing, routed to a cluster/provider boundary, "
        "documented as presentation-only behavior, or explicitly refused with a "
        "deterministic diagnostic.",
        "",
        "The ScratchBird engine remains SBLR/UUID-only. Compatibility SQL is parsed "
        "outside the engine, and accepted work is still revalidated by ScratchBird "
        "authority before execution.",
        "",
        "## Summary",
        "",
        "| Runtime disposition | Count | Meaning |",
        "| --- | ---: | --- |",
    ]
    for disposition in sorted(dispositions):
        lines.append(
            f"| `{md_escape(disposition)}` | {dispositions[disposition]} | "
            f"{md_escape(status_label(disposition))} |"
        )
    lines.extend([
        "",
        "| Classification | Count |",
        "| --- | ---: |",
    ])
    for classification in sorted(classifications):
        lines.append(f"| `{md_escape(classification)}` | {classifications[classification]} |")
    lines.extend([
        "",
        "## Surface Status",
        "",
        "| Row | Functionality | Implementation status | Runtime disposition | Route or SBLR | Diagnostic/refusal policy | Proof status |",
        "| --- | --- | --- | --- | --- | --- | --- |",
    ])
    for row in rows:
        route = row.get("final_sblr") or row.get("final_route") or row.get("current_route")
        lines.append(
            "| "
            f"`{md_escape(row['declared_row_id'])}` | "
            f"`{md_escape(row['declared_surface'])}` | "
            f"{md_escape(status_label(row['runtime_disposition']))} | "
            f"`{md_escape(row['runtime_disposition'])}` | "
            f"`{md_escape(route)}` | "
            f"`{md_escape(diagnostic_from_route(row))}` | "
            f"`{md_escape(row['status'])}` |"
        )
    lines.extend([
        "",
        "## Source Anchors",
        "",
        "These anchors identify the source-backed declaration used to generate each "
        "row. They are included so a developer or auditor can trace the public "
        "status back to the implementation declaration without using private notes.",
        "",
        "| Row | Source anchor | Parser package |",
        "| --- | --- | --- |",
    ])
    for row in rows:
        lines.append(
            "| "
            f"`{md_escape(row['declared_row_id'])}` | "
            f"`{md_escape(row['source_search_key'])}` | "
            f"`{md_escape(row['parser_package'])}` |"
        )
    lines.append("")
    return "\n".join(lines)


def render_index(lanes: list[dict[str, str]], grouped: dict[str, list[dict[str, str]]]) -> str:
    lines = [
        "# Compatibility Parser Surface Status",
        "",
        "<!-- AUTO-GENERATED: compatibility parser status index. Regenerate with",
        "python3 project/tests/reference_regression/generate_compatibility_parser_docs.py --repo-root . --write",
        "-->",
        "",
        "This directory contains one generated status page per public compatibility "
        "parser lane. Each page is generated from the public compatibility parser "
        "remap matrix and is checked by CTest so every declared surface has a "
        "searchable support, route, or refusal entry. All statuses are retained "
        "pre-hold evidence and do not claim conformance to the in-progress SBLR "
        "contract.",
        "",
        "| Parser | Reference profile | Batch | Declared surfaces | Retained pre-hold status |",
        "| --- | --- | --- | ---: | --- |",
    ]
    for lane in lanes:
        family_id = lane["family_id"]
        display_name = lane["display_name"]
        rows = grouped.get(family_id, [])
        lines.append(
            f"| [{md_escape(display_name)}]({family_id}.md) | "
            f"`{md_escape(lane['reference_profile'])}` | "
            f"`{md_escape(lane['batch'])}` | "
            f"{len(rows)} | "
            f"`{md_escape(lane['public_beta_status'])}` |"
        )
    lines.append("")
    return "\n".join(lines)


def expected_docs(repo_root: Path) -> dict[Path, str]:
    lanes = public_lanes(repo_root)
    grouped = matrix_by_family(repo_root)
    lane_ids = {lane["family_id"] for lane in lanes}
    extra = sorted(set(grouped) - lane_ids)
    missing = sorted(lane_ids - set(grouped))
    if extra:
        raise RuntimeError(f"matrix contains non-public parser lane rows: {', '.join(extra)}")
    if missing:
        raise RuntimeError(f"public parser lanes missing matrix rows: {', '.join(missing)}")

    docs: dict[Path, str] = {
        DOC_ROOT_REL / "README.md": render_index(lanes, grouped),
    }
    for lane in lanes:
        family_id = lane["family_id"]
        docs[DOC_ROOT_REL / f"{family_id}.md"] = render_parser_page(lane, grouped[family_id])
    return docs


def write_docs(repo_root: Path, docs: dict[Path, str]) -> None:
    doc_root = repo_root / DOC_ROOT_REL
    doc_root.mkdir(parents=True, exist_ok=True)
    expected_paths = {repo_root / rel for rel in docs}
    for existing in doc_root.glob("*.md"):
        if existing not in expected_paths:
            existing.unlink()
    for rel, content in docs.items():
        target = repo_root / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(content, encoding="utf-8")


def check_docs(repo_root: Path, docs: dict[Path, str]) -> dict[str, Any]:
    issues: list[str] = []
    doc_root = repo_root / DOC_ROOT_REL
    expected_rel_paths = set(docs)
    existing_rel_paths = {
        path.relative_to(repo_root)
        for path in doc_root.glob("*.md")
    } if doc_root.exists() else set()
    for rel in sorted(expected_rel_paths - existing_rel_paths):
        issues.append(f"missing_doc:{rel.as_posix()}")
    for rel in sorted(existing_rel_paths - expected_rel_paths):
        issues.append(f"unexpected_doc:{rel.as_posix()}")
    for rel, expected in sorted(docs.items(), key=lambda item: item[0].as_posix()):
        target = repo_root / rel
        if not target.is_file():
            continue
        actual = target.read_text(encoding="utf-8")
        if actual != expected:
            diff = "\n".join(
                difflib.unified_diff(
                    actual.splitlines(),
                    expected.splitlines(),
                    fromfile=str(rel),
                    tofile=f"{rel} (expected)",
                    lineterm="",
                    n=3,
                )
            )
            issues.append(f"stale_doc:{rel.as_posix()}\n{diff[:4000]}")

    matrix_rows = read_csv(repo_root / REMAP_MATRIX_REL)
    for row in matrix_rows:
        family = family_from_package(row["parser_package"])
        if not family:
            issues.append(f"unmapped_package:{row['parser_package']}")
            continue
        rel = DOC_ROOT_REL / f"{family}.md"
        text = (repo_root / rel).read_text(encoding="utf-8") if (repo_root / rel).is_file() else ""
        for required in (row["declared_row_id"], row["declared_surface"], row["runtime_disposition"]):
            if required not in text:
                issues.append(f"missing_surface_doc:{rel.as_posix()}:{row['declared_row_id']}:{required}")
    return {
        "schema_id": "scratchbird.compatibility_parser_docs_gate.v1",
        "status": "fail" if issues else "pass",
        "doc_count": len(docs),
        "matrix_row_count": len(matrix_rows),
        "issues": issues,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[3])
    parser.add_argument("--write", action="store_true")
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--evidence-file", type=Path)
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    docs = expected_docs(repo_root)
    if args.write:
        write_docs(repo_root, docs)
    result = check_docs(repo_root, docs) if args.check or not args.write else {
        "schema_id": "scratchbird.compatibility_parser_docs_gate.v1",
        "status": "pass",
        "doc_count": len(docs),
        "matrix_row_count": len(read_csv(repo_root / REMAP_MATRIX_REL)),
        "issues": [],
    }
    if args.evidence_file:
        args.evidence_file.parent.mkdir(parents=True, exist_ok=True)
        args.evidence_file.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(
        "compatibility_parser_docs_gate="
        f"{result['status']} docs={result['doc_count']} matrix_rows={result['matrix_row_count']}"
    )
    if result["issues"]:
        for issue in result["issues"][:20]:
            print(issue)
    return 0 if result["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
