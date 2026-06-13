#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate the public investor-review surface.

This gate is intentionally narrow and evidence-oriented. It does not certify
production readiness; it blocks public repo patterns that make the source-review
release look self-certified, source-contaminated, or broader than its evidence.
"""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path
import subprocess
import sys
from typing import Any


ALLOWED_RELEASE_BUCKETS = {
    "release_supported",
    "release_candidate",
    "tracked_not_released",
}

CONTRACT_ONLY_DRIVER_STATUS = "planned_not_implemented"
CONTRACT_ONLY_RELEASE_BUCKET = "tracked_not_released"

CAPTURED_BENCHMARK_STATES = {
    "captured",
    "passed",
    "validated",
    "release_validated",
    "captured_and_validated",
}

SKIP_SUFFIXES = {
    ".a",
    ".dll",
    ".dylib",
    ".exe",
    ".jar",
    ".o",
    ".obj",
    ".pdf",
    ".png",
    ".pyc",
    ".so",
    ".zip",
}

REQUIRED_TEXT_SNIPPETS = {
    "REFERENCE_SYSTEMS_AND_IP_BOUNDARY.md": (
        "Raw upstream regression payloads",
        "ScratchBird-owned CTest harnesses",
        "must not track external implementation source",
    ),
    "THIRD_PARTY_NOTICES.md": (
        "Raw upstream payloads are not tracked",
        "ScratchBird-owned harnesses",
        "downloaded/native reference tools are not part of the tracked public GitHub source surface",
    ),
    "KNOWN_LIMITATIONS.md": (
        "early beta public source-review release",
        "Reference regression tests are external fixtures",
        "Benchmark harnesses are included for reproducibility",
        "Planned Driver Lanes Not Implemented",
        "ADBC",
        "Flight SQL",
        "Perl DBI",
        "planned_not_implemented",
        "tracked_not_released",
    ),
    "RELEASE_TERMS.md": (
        "release-complete profile",
        "not a claim of production fitness",
        "benchmark harness",
    ),
    "SECURITY.md": (
        "Do not report security-sensitive issues in public issues",
        "unsupported cluster-provider behavior not present in this public source tree",
    ),
    "README.md": (
        "not presented as a production-ready system",
        "REFERENCE_SYSTEMS_AND_IP_BOUNDARY.md",
        "Public source-review release",
    ),
}

REQUIRED_JSON_FILES = {
    "SBOM.json": "scratchbird.public_source_review_sbom.v1",
}

DECLARATION_FIXTURE_ROOTS = (
    "project/drivers/fixtures/driver_server_reconciliation/artifacts",
    "project/tools/sb_public_spec_zero_grey_gate/fixtures/driver_server_reconciliation/artifacts",
)

PUBLIC_WORKFILE_BLOCKLIST = (
    "docs/documentation/draft/audit_reports/",
    "docs/documentation/draft/.claude/",
)

PUBLIC_WORKFILE_NAMES = {
    ".generation_state.json",
    "combined.md",
    "combined_old.md",
    "file_listing.txt",
    "file_listing_2.txt",
    "MGA.md",
    "MGA.pdf",
    "Notes.md",
}


def fail(message: str) -> int:
    print(f"investor_readiness_surface_gate=fail:{message}", file=sys.stderr)
    return 1


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def git_paths(repo_root: Path) -> list[str]:
    result = subprocess.run(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard"],
        cwd=repo_root,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    return [line.strip() for line in result.stdout.splitlines() if line.strip()]


def excluded(path: str, excludes: tuple[str, ...]) -> bool:
    return any(path == prefix.rstrip("/") or path.startswith(prefix.rstrip("/") + "/") for prefix in excludes)


def path_display(path: Path, repo_root: Path) -> str:
    return path.relative_to(repo_root).as_posix()


def load_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def write_output(output: Path | None, payload: dict[str, Any]) -> None:
    if output is None:
        return
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def validate_required_docs(repo_root: Path) -> list[str]:
    errors: list[str] = []
    for rel_path, snippets in REQUIRED_TEXT_SNIPPETS.items():
        path = repo_root / rel_path
        if not path.is_file():
            errors.append(f"missing required public file: {rel_path}")
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for snippet in snippets:
            if snippet not in text:
                errors.append(f"{rel_path} missing required boundary text: {snippet}")
    for rel_path, schema in REQUIRED_JSON_FILES.items():
        path = repo_root / rel_path
        if not path.is_file():
            errors.append(f"missing required public file: {rel_path}")
            continue
        payload = load_json(path)
        if payload.get("schema") != schema:
            errors.append(f"{rel_path} schema mismatch")
    return errors


def validate_public_preset(repo_root: Path) -> list[str]:
    path = repo_root / "project" / "CMakePresets.json"
    if not path.is_file():
        return ["missing project/CMakePresets.json"]
    payload = load_json(path)
    configure = {preset.get("name"): preset for preset in payload.get("configurePresets", [])}
    build = {preset.get("name"): preset for preset in payload.get("buildPresets", [])}
    test = {preset.get("name"): preset for preset in payload.get("testPresets", [])}
    errors: list[str] = []
    if "public-release-linux" not in configure:
        errors.append("missing public-release-linux configure preset")
        return errors
    if "public-release-linux" not in build:
        errors.append("missing public-release-linux build preset")
    if "public-release-linux" not in test:
        errors.append("missing public-release-linux test preset")
    cache = configure["public-release-linux"].get("cacheVariables", {})
    for key in ("SB_BUILD_PUBLIC_RELEASE_CORRECTNESS", "SB_BUILD_PUBLIC_STANDALONE_OUTPUT", "SB_BUILD_TESTS"):
        if cache.get(key) != "ON":
            errors.append(f"public-release-linux preset must set {key}=ON")
    return errors


def validate_reference_payload_boundary(repo_root: Path, paths: list[str], excludes: tuple[str, ...]) -> list[str]:
    blocked: list[str] = []
    stale = ("d" "onor", "D" "onor", "D" "ONOR")
    for path in paths:
        if excluded(path, excludes):
            continue
        if any(token in path for token in stale):
            blocked.append(f"stale public path terminology: {path}")
        if "/native_tool_harness/tools/" in path:
            blocked.append(f"tracked native reference tool path: {path}")
        if path.startswith("project/tests/reference_regression/firebird/original_firebird_qa/"):
            blocked.append(f"tracked raw upstream regression path: {path}")
        if path.startswith("project/tests/reference_regression/reference_release_acquisition/"):
            name = Path(path).name
            allowed = (
                name == "PUBLIC_REGRESSION_SCOPE.md"
                or name.endswith("_CANDIDATE.md")
                or name.endswith("_MANIFEST.csv")
                or name.endswith("_INDEX.csv")
            )
            if not allowed:
                blocked.append(f"tracked reference acquisition payload: {path}")

    for rel_path in paths:
        if excluded(rel_path, excludes):
            continue
        if Path(rel_path).suffix in SKIP_SUFFIXES:
            continue
        path = repo_root / rel_path
        if not path.is_file():
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        for token in stale:
            if token in text:
                blocked.append(f"stale public text terminology in {rel_path}")
                break
    return blocked


def validate_public_workfile_boundary(paths: list[str], excludes: tuple[str, ...]) -> list[str]:
    errors: list[str] = []
    for path in paths:
        if excluded(path, excludes):
            continue
        if any(path.startswith(prefix) for prefix in PUBLIC_WORKFILE_BLOCKLIST):
            errors.append(f"tracked public draft workfile path: {path}")
            continue
        name = Path(path).name
        if path.startswith("docs/documentation/draft/") and (
            name in PUBLIC_WORKFILE_NAMES or name.startswith("audit_")
        ):
            errors.append(f"tracked public draft workfile path: {path}")
    return errors


def validate_driver_manifest(repo_root: Path) -> tuple[list[str], dict[str, int], dict[str, str]]:
    manifest = repo_root / "project" / "drivers" / "DriverPackageManifest.csv"
    rows = read_csv(manifest)
    errors: list[str] = []
    if not rows:
        return ["driver manifest is empty"], {}, {}
    if "release_bucket" not in rows[0]:
        errors.append("DriverPackageManifest.csv missing release_bucket column")
    counts: dict[str, int] = {}
    buckets: dict[str, str] = {}
    for row in rows:
        component_id = row.get("component_id", "")
        bucket = row.get("release_bucket", "")
        if bucket not in ALLOWED_RELEASE_BUCKETS:
            errors.append(f"{component_id} invalid release_bucket {bucket!r}")
            continue
        counts[bucket] = counts.get(bucket, 0) + 1
        buckets[component_id] = bucket
        if row.get("category") == "driver":
            source_path = row.get("source_path", "")
            source_dir = repo_root / source_path
            if source_dir.is_dir():
                source_files = sorted(
                    path.relative_to(source_dir).as_posix()
                    for path in source_dir.rglob("*")
                    if path.is_file()
                )
                if source_files == ["package_contract.json"]:
                    if row.get("driver_status") != CONTRACT_ONLY_DRIVER_STATUS:
                        errors.append(
                            f"{component_id} contract-only driver must use "
                            f"{CONTRACT_ONLY_DRIVER_STATUS} driver_status"
                        )
                    if bucket != CONTRACT_ONLY_RELEASE_BUCKET:
                        errors.append(
                            f"{component_id} contract-only driver must use "
                            f"{CONTRACT_ONLY_RELEASE_BUCKET} release_bucket"
                        )
                    contract = load_json(source_dir / "package_contract.json")
                    if contract.get("status") != CONTRACT_ONLY_DRIVER_STATUS:
                        errors.append(
                            f"{component_id} contract-only package_contract.json "
                            f"must use {CONTRACT_ONLY_DRIVER_STATUS} status"
                        )
    return errors, counts, buckets


def validate_benchmark_evidence(repo_root: Path, buckets: dict[str, str]) -> tuple[list[str], dict[str, int]]:
    path = (
        repo_root
        / "project"
        / "drivers"
        / "fixtures"
        / "driver_server_reconciliation"
        / "artifacts"
        / "FULL_ROUTE_BENCHMARK_EVIDENCE.json"
    )
    payload = load_json(path)
    rows = payload.get("benchmark_evidence", [])
    errors: list[str] = []
    status_counts: dict[str, int] = {}
    for row in rows:
        if not isinstance(row, dict):
            errors.append("benchmark evidence row is not an object")
            continue
        component_id = row.get("component_id", "")
        status = str(row.get("evidence_status", ""))
        status_counts[status] = status_counts.get(status, 0) + 1
        if buckets.get(component_id) == "release_supported" and status not in CAPTURED_BENCHMARK_STATES:
            errors.append(f"{component_id} is release_supported without captured benchmark evidence")
        for key in ("performance_claim", "faster_than", "speedup"):
            if key in row:
                errors.append(f"{component_id} benchmark evidence includes claim key {key}")
    return errors, status_counts


def validate_driver_declaration(repo_root: Path) -> list[str]:
    errors: list[str] = []
    script = repo_root / "project" / "drivers" / "scripts" / "driver_release_declaration_gate.py"
    text = script.read_text(encoding="utf-8", errors="replace")
    forbidden_script_fragments = (
        '"release_state": "supported"',
        '"release_state_counts": {"supported"',
        'row["spec_status"] =',
        'row["implementation_status"] =',
        'row["test_status"] =',
        'row["closure_status"] =',
        'evidence["status"] =',
    )
    for fragment in forbidden_script_fragments:
        if fragment in text:
            errors.append(f"driver release declaration script has self-promoting fragment: {fragment}")

    for root in DECLARATION_FIXTURE_ROOTS:
        json_path = repo_root / root / "DRIVER_SERVER_RELEASE_DECLARATION.json"
        csv_path = repo_root / root / "DRIVER_SERVER_RELEASE_DECLARATION.csv"
        if not json_path.is_file():
            errors.append(f"missing {path_display(json_path, repo_root)}")
            continue
        payload = load_json(json_path)
        rows = payload.get("rows", [])
        if not isinstance(rows, list):
            errors.append(f"{path_display(json_path, repo_root)} rows must be a list")
        else:
            for row in rows:
                if isinstance(row, dict) and row.get("release_state") == "supported":
                    errors.append(f"{path_display(json_path, repo_root)} contains legacy supported row state")
                    break
        summary_counts = payload.get("summary", {}).get("release_state_counts", {})
        if isinstance(summary_counts, dict) and summary_counts.get("supported"):
            errors.append(f"{path_display(json_path, repo_root)} contains legacy supported summary count")
        if csv_path.is_file() and ",supported," in csv_path.read_text(encoding="utf-8", errors="replace"):
            errors.append(f"{path_display(csv_path, repo_root)} contains legacy supported row state")
    return errors


def validate_ai_scope(repo_root: Path) -> list[str]:
    readme = repo_root / "project" / "ai" / "README.md"
    if not readme.is_file():
        return ["missing project/ai/README.md"]
    text = readme.read_text(encoding="utf-8", errors="replace")
    normalized = " ".join(text.split())
    errors: list[str] = []
    required = (
        "current early beta / technical Beta 1 review baseline",
        "Native-only AI support is in scope",
        "AI support for non-native emulated engine modes remains out of scope",
        "Direct-listener live-native certification must be regenerated",
    )
    for snippet in required:
        if snippet not in normalized:
            errors.append(f"project/ai/README.md missing AI scope text: {snippet}")
    forbidden = ("AI production complete", "production complete AI", "AI is production-ready")
    for snippet in forbidden:
        if snippet in text:
            errors.append(f"project/ai/README.md contains overclaim: {snippet}")
    return errors


def inventory(paths: list[str], excludes: tuple[str, ...]) -> dict[str, Any]:
    public_paths = [path for path in paths if not excluded(path, excludes)]
    todos = [path for path in public_paths if Path(path).suffix in {".md", ".py", ".cpp", ".hpp", ".h", ".c"}]
    return {
        "tracked_and_untracked_public_paths": len(public_paths),
        "candidate_todo_scan_paths": len(todos),
        "draft_excluded": list(excludes),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[3])
    parser.add_argument("--project-root", type=Path, default=None)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--inventory", action="store_true")
    parser.add_argument("--exclude", action="append")
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    excludes = tuple(dict.fromkeys(("docs/documentation/draft", *(args.exclude or []))))
    paths = git_paths(repo_root)

    errors: list[str] = []
    errors.extend(validate_required_docs(repo_root))
    errors.extend(validate_public_preset(repo_root))
    errors.extend(validate_public_workfile_boundary(paths, excludes))
    errors.extend(validate_reference_payload_boundary(repo_root, paths, excludes))
    driver_errors, bucket_counts, buckets = validate_driver_manifest(repo_root)
    errors.extend(driver_errors)
    benchmark_errors, benchmark_counts = validate_benchmark_evidence(repo_root, buckets)
    errors.extend(benchmark_errors)
    errors.extend(validate_driver_declaration(repo_root))
    errors.extend(validate_ai_scope(repo_root))

    evidence = {
        "schema": "scratchbird.investor_readiness_surface_gate.v1",
        "status": "failed" if errors else "passed",
        "inventory_mode": bool(args.inventory),
        "inventory": inventory(paths, excludes),
        "driver_release_bucket_counts": bucket_counts,
        "benchmark_evidence_status_counts": benchmark_counts,
        "errors": errors,
    }
    write_output(args.output, evidence)

    if errors:
        for error in errors[:200]:
            print(f"investor_readiness_surface_gate:error:{error}", file=sys.stderr)
        if len(errors) > 200:
            print(f"investor_readiness_surface_gate:error_count_omitted:{len(errors) - 200}", file=sys.stderr)
        return 1
    print("investor_readiness_surface_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
