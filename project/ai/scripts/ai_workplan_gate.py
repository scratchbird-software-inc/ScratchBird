#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate ScratchBird AI current-repo integration execution plan rules."""

from __future__ import annotations

import argparse
import json
import re
import sys
from datetime import datetime, timezone
from pathlib import Path


EXCLUDED_DIR_NAMES = {
    "." + "git",
    ".mypy_cache",
    ".pytest_cache",
    ".ruff_cache",
    ".venv",
    "Testing",
    "__pycache__",
    "artifacts",
    "build",
    "dist",
}
EXCLUDED_SUFFIXES = {
    ".pyc",
    ".pyo",
}
REQUIRED_PATHS = [
    "src/scratchbird_ai/__init__.py",
    "tests/test_service.py",
    "tools/generate_ai_conformance_artifacts.py",
    "tools/validate_evidence_gates.py",
    "tools/validate_release_candidate.py",
    "tools/run_live_native_conformance.py",
    "examples/http-bridge.env.example",
    "docs/releases/EARLY_BETA_CONFORMANCE_GATES.md",
    "docs/status/EARLY_BETA_STATUS_2026-03-07.md",
    "pyproject.toml",
    "README.md",
    "CMakeLists.txt",
]
def legacy_cliwork_path(name: str) -> str:
    return str(Path("/").joinpath("home", "dcalford", "local workspace", name))


def legacy_project_name(suffix: str) -> str:
    return "ScratchBird-" + suffix


FORBIDDEN_TEXT = [
    legacy_cliwork_path(legacy_project_name("ai")),
    legacy_cliwork_path(legacy_project_name("driver")),
    "~/" + ".scratchbird/static-example",
]
TEST_ONLY_TEXT = [
    "secret-token",
    "replaceme",
]
TEXT_SUFFIXES = {
    ".cmake",
    ".csv",
    ".env",
    ".example",
    ".h",
    ".hpp",
    ".json",
    ".md",
    ".py",
    ".sh",
    ".toml",
    ".txt",
    ".xml",
    ".yaml",
    ".yml",
}


def utc_now() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def is_text_candidate(path: Path) -> bool:
    return path.suffix in TEXT_SUFFIXES or path.name.endswith(".env.example")


def relative(path: Path, root: Path) -> str:
    return path.relative_to(root).as_posix()


def scan_files(repo_root: Path) -> list[Path]:
    return sorted(path for path in repo_root.rglob("*") if path.is_file())


def validate_inventory(repo_root: Path) -> tuple[list[str], dict[str, object]]:
    errors: list[str] = []
    for rel_path in REQUIRED_PATHS:
        if not (repo_root / rel_path).exists():
            errors.append(f"missing required import path: {rel_path}")

    excluded_hits: list[str] = []
    for path in repo_root.rglob("*"):
        rel = relative(path, repo_root)
        if path.is_dir() and path.name in EXCLUDED_DIR_NAMES:
            excluded_hits.append(rel)
        if path.is_file() and (path.suffix in EXCLUDED_SUFFIXES or path.name.endswith(".egg-info")):
            excluded_hits.append(rel)
    if excluded_hits:
        errors.append(f"excluded import artifacts present: {excluded_hits}")

    source_files = [path for path in scan_files(repo_root) if path.is_relative_to(repo_root / "src")]
    test_files = [path for path in scan_files(repo_root) if path.is_relative_to(repo_root / "tests")]
    tool_files = [path for path in scan_files(repo_root) if path.is_relative_to(repo_root / "tools")]
    return errors, {
        "source_file_count": len(source_files),
        "test_file_count": len(test_files),
        "tool_file_count": len(tool_files),
        "excluded_hits": excluded_hits,
    }


def validate_old_paths(repo_root: Path) -> tuple[list[str], dict[str, object]]:
    errors: list[str] = []
    forbidden_hits: list[str] = []
    placeholder_hits: list[str] = []
    for path in scan_files(repo_root):
        if not is_text_candidate(path):
            continue
        rel = relative(path, repo_root)
        if rel == "scripts/ai_execution plan_gate.py":
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        for needle in FORBIDDEN_TEXT:
            if needle in text:
                forbidden_hits.append(f"{rel}: {needle}")
        for needle in TEST_ONLY_TEXT:
            pattern = rf"(?<![A-Za-z0-9_-]){re.escape(needle)}(?![A-Za-z0-9_-])"
            if not re.search(pattern, text):
                continue
            if not rel.startswith("tests/"):
                placeholder_hits.append(f"{rel}: {needle}")
    if forbidden_hits:
        errors.append(f"old local paths remain: {forbidden_hits}")
    if placeholder_hits:
        errors.append(f"test placeholder values leaked outside tests: {placeholder_hits}")
    return errors, {
        "forbidden_hits": forbidden_hits,
        "test_placeholder_hits": placeholder_hits,
    }


def validate_artifact_isolation(repo_root: Path, artifact_root: Path | None) -> tuple[list[str], dict[str, object]]:
    errors: list[str] = []
    source_artifact_dirs = [
        relative(path, repo_root)
        for path in repo_root.rglob("*")
        if path.is_dir() and path.name == "artifacts"
    ]
    if source_artifact_dirs:
        errors.append(f"source-tree artifact directories are not allowed: {source_artifact_dirs}")

    generated_ok = False
    if artifact_root is not None:
        generated_ok = (artifact_root / "ai_conformance").exists() or (artifact_root / "live_native_conformance").exists()
    return errors, {
        "source_artifact_dirs": source_artifact_dirs,
        "artifact_root": str(artifact_root) if artifact_root else None,
        "build_artifact_root_contains_ai_outputs": generated_ok,
    }


def write_report(output_dir: Path, mode: str, errors: list[str], details: dict[str, object]) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    payload = {
        "generated_at_utc": utc_now(),
        "mode": mode,
        "status": "PASS" if not errors else "FAIL",
        "check_count": details.get("check_count", 1),
        "passed_checks": 0 if errors else details.get("check_count", 1),
        "failed_checks": errors,
        "details": details,
    }
    (output_dir / f"{mode}.json").write_text(
        json.dumps(payload, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--artifact-root", default=None)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument(
        "--mode",
        choices=("inventory", "old-paths", "artifact-isolation", "final"),
        required=True,
    )
    args = parser.parse_args()
    repo_root = Path(args.repo_root).resolve()
    artifact_root = Path(args.artifact_root).resolve() if args.artifact_root else None
    output_dir = Path(args.output_dir).resolve()

    all_errors: list[str] = []
    details: dict[str, object] = {}
    if args.mode in {"inventory", "final"}:
        errors, payload = validate_inventory(repo_root)
        all_errors.extend(errors)
        details["inventory"] = payload
    if args.mode in {"old-paths", "final"}:
        errors, payload = validate_old_paths(repo_root)
        all_errors.extend(errors)
        details["old_paths"] = payload
    if args.mode in {"artifact-isolation", "final"}:
        errors, payload = validate_artifact_isolation(repo_root, artifact_root)
        all_errors.extend(errors)
        details["artifact_isolation"] = payload

    details["check_count"] = 3 if args.mode == "final" else 1
    write_report(output_dir, args.mode, all_errors, details)
    if all_errors:
        for error in all_errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1
    print(f"OK: ScratchBird AI execution plan gate passed ({args.mode})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
