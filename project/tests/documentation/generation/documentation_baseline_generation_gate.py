#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Gate the generated documentation baseline artifacts."""

from __future__ import annotations

import argparse
import csv
import json
import shutil
import subprocess
import sys
from pathlib import Path


FORBIDDEN_RELEASE_TERMS = (
    "TBD",
    "TODO",
    "stub",
    "placeholder",
    "minimal",
    "future work",
    "implementation-defined",
    "as appropriate",
    "generally",
)

SECURITY_PUBLICATION_SOURCE_INPUTS = {
    "security_bootstrap_registry",
    "security_policy_registry",
    "security_policy",
    "policy",
    "authentication_registry",
    "authentication",
    "authorization_registry",
    "authorization",
    "audit_registry",
    "audit",
    "redaction_policy_registry",
    "redaction",
    "audit_checklists",
}

EXAMPLE_PUBLICATION_SOURCE_INPUTS = {
    "quick_start_example_manifest",
    "example_manifest",
    "example_replay_manifest",
    "example_sources",
    "examples",
    "sample_database_manifest",
}

CLI_PUBLICATION_SOURCE_INPUTS = {
    "cli_help_extraction",
    "cli_help",
    "cli_flag_registry",
    "tool_metadata",
}

DEPRECATED_CLI_DOC_TERMS = (
    "sb_fb_isql",
    "sb_my_isql",
    "sb_pg_isql",
)
DOCS_ROOT = Path("docs")
EXECUTION_PLAN_ROOT = DOCS_ROOT / "execution-plans"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", default=".", help="ScratchBird repository root")
    parser.add_argument(
        "--no-regenerate",
        action="store_true",
        help="validate existing artifacts without invoking the generator",
    )
    return parser.parse_args()


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as fh:
        return list(csv.DictReader(fh))


def load_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def rel(path: Path, root: Path) -> str:
    return path.resolve().relative_to(root.resolve()).as_posix()


def fail(problems: list[str]) -> None:
    for problem in problems:
        print(problem, file=sys.stderr)
    raise SystemExit(1)


def main() -> None:
    args = parse_args()
    root = Path(args.repo_root).resolve()
    problems: list[str] = []
    generator = root / "project/tools/documentation/generate_documentation_baseline.py"
    if not args.no_regenerate:
        subprocess.run(
            [sys.executable, str(generator), "--repo-root", str(root), "--check"],
            check=True,
            cwd=root,
        )

    baseline = root / "docs/documentation/_generated/baseline"
    summary_path = baseline / "documentation_generation_summary.json"
    missing_path = baseline / "missing_authority_inputs.csv"
    inventory_path = baseline / "documentation_authority_inventory.csv"
    execution_plan_missing_path = (
        root
        / EXECUTION_PLAN_ROOT
        / "end-user-documentation-baseline-generation/GENERATED_MISSING_AUTHORITY_LEDGER.csv"
    )
    if not summary_path.exists():
        problems.append("documentation generation summary is missing")
    if not missing_path.exists():
        problems.append("missing authority ledger is missing")
    if not inventory_path.exists():
        problems.append("authority inventory is missing")
    if not execution_plan_missing_path.exists():
        problems.append("execution_plan missing authority ledger is missing")
    if problems:
        fail(problems)

    summary = load_json(summary_path)
    missing_rows = read_csv(missing_path)
    execution_plan_missing_rows = read_csv(execution_plan_missing_path)
    inventory_rows = read_csv(inventory_path)
    expected_books = read_csv(
        root / EXECUTION_PLAN_ROOT / "end-user-documentation-baseline-generation/DOCUMENTATION_BOOK_REGISTRY.csv"
    )

    if summary.get("public_release_ready") is not False:
        problems.append("baseline summary must not mark documentation public-release ready")
    if int(summary.get("missing_authority_count", -1)) != len(missing_rows):
        problems.append("missing authority count does not match ledger rows")
    if missing_rows != execution_plan_missing_rows:
        problems.append("execution_plan missing authority ledger does not match generated ledger")
    if int(summary.get("book_count", 0)) < len(expected_books):
        problems.append("generated book count is lower than the execution_plan book registry")
    if not inventory_rows:
        problems.append("authority inventory has no rows")

    doc_models = sorted(
        path
        for path in (root / "docs/documentation").glob("**/_generated/doc_model.json")
        if "_generated/baseline" not in rel(path, root)
    )
    if len(doc_models) != int(summary.get("book_count", -1)):
        problems.append("doc_model count does not match generated book count")

    for doc_model_path in doc_models:
        model = load_json(doc_model_path)
        manual_id = model.get("manual_id", rel(doc_model_path, root))
        policy_blockers = model.get("publication_policy_blockers", [])
        source_inputs = {
            item.get("input")
            for item in model.get("source_inputs", [])
            if isinstance(item, dict)
        }
        if model.get("public_release_ready") is not False:
            problems.append(f"{manual_id}: doc_model marks public-release ready")
        if manual_id.startswith("reference_") and "reference_documentation_legal_hold_pending_ip_lawyer" not in policy_blockers:
            problems.append(f"{manual_id}: reference legal-hold publication blocker missing")
        if (
            manual_id == "reference_interbase_migration_reference"
            and "closed_source_interbase_private_only_pending_legal_approval" not in policy_blockers
        ):
            problems.append(f"{manual_id}: InterBase closed-source publication blocker missing")
        if (
            SECURITY_PUBLICATION_SOURCE_INPUTS.intersection(source_inputs)
            and "security_documentation_pre_gold_private_use_at_own_risk" not in policy_blockers
        ):
            problems.append(f"{manual_id}: security pre-gold publication blocker missing")
        if (
            EXAMPLE_PUBLICATION_SOURCE_INPUTS.intersection(source_inputs)
            and "example_replay_proof_pending" not in policy_blockers
        ):
            problems.append(f"{manual_id}: example replay-proof publication blocker missing")
        if (
            CLI_PUBLICATION_SOURCE_INPUTS.intersection(source_inputs)
            and "cli_branding_implementation_rename_pending" not in policy_blockers
        ):
            problems.append(f"{manual_id}: CLI branding implementation blocker missing")
        if not model.get("source_inputs"):
            problems.append(f"{manual_id}: doc_model has no source inputs")
        for artifact in model.get("generated_artifacts", []):
            artifact_path = root / artifact
            if not artifact_path.exists():
                problems.append(f"{manual_id}: generated artifact missing: {artifact}")
        generated_root = doc_model_path.parent
        source_map = generated_root / "documentation_source_map.csv"
        if not source_map.exists():
            problems.append(f"{manual_id}: source map missing")
        else:
            for row in read_csv(source_map):
                if not row.get("source_input") or not row.get("status") or not row.get("resolution"):
                    problems.append(f"{manual_id}: source map row is incomplete")
        egress_report = generated_root / "ai_egress_report.json"
        if not egress_report.exists():
            problems.append(f"{manual_id}: AI egress report missing")
        else:
            egress = load_json(egress_report)
            if egress.get("external_egress") is not False:
                problems.append(f"{manual_id}: AI egress report allows external egress")
        examples_manifest = generated_root / "examples_manifest.yaml"
        if not examples_manifest.exists():
            problems.append(f"{manual_id}: examples manifest missing")
        elif EXAMPLE_PUBLICATION_SOURCE_INPUTS.intersection(source_inputs):
            manifest_text = examples_manifest.read_text(encoding="utf-8")
            if "example_replay_proof_pending" not in manifest_text:
                problems.append(f"{manual_id}: examples manifest missing replay-proof blocker")
        pdfs = list(doc_model_path.parents[1].glob("pdf/*.pdf"))
        if not pdfs:
            problems.append(f"{manual_id}: PDF output missing")
        for pdf in pdfs:
            if not pdf.read_bytes().startswith(b"%PDF-"):
                problems.append(f"{manual_id}: PDF output is not a PDF: {rel(pdf, root)}")
            if shutil.which("pdftotext"):
                extracted = subprocess.run(
                    ["pdftotext", str(pdf), "-"],
                    check=False,
                    cwd=root,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                )
                if extracted.returncode != 0:
                    problems.append(f"{manual_id}: pdftotext failed for {rel(pdf, root)}")
                if "Syntax Error" in extracted.stderr:
                    problems.append(f"{manual_id}: PDF syntax errors reported for {rel(pdf, root)}")
                if len(extracted.stdout.strip()) < 40:
                    problems.append(f"{manual_id}: PDF text extraction is empty for {rel(pdf, root)}")

    for path in (root / "docs/documentation").glob("**/*"):
        if path.is_file() and path.suffix.lower() in {".md", ".html"}:
            text = path.read_text(encoding="utf-8")
            try:
                text.encode("ascii")
            except UnicodeEncodeError:
                problems.append(f"{rel(path, root)} is not ASCII clean")
            for term in FORBIDDEN_RELEASE_TERMS:
                if term in text:
                    problems.append(f"{rel(path, root)} contains release-blocked term {term!r}")
            for term in DEPRECATED_CLI_DOC_TERMS:
                if term in text:
                    problems.append(f"{rel(path, root)} contains deprecated CLI term {term!r}")

    if problems:
        fail(problems)
    print(
        json.dumps(
            {
                "status": "passed",
                "book_count": summary.get("book_count"),
                "missing_authority_count": summary.get("missing_authority_count"),
                "public_release_ready": summary.get("public_release_ready"),
            },
            indent=2,
            sort_keys=True,
        )
    )


if __name__ == "__main__":
    main()
