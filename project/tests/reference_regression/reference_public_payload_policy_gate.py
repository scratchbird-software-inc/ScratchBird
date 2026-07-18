#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Enforce public reference-test payload boundaries."""

from __future__ import annotations

import argparse
import csv
import json
import pathlib
import subprocess
import sys


RELEASE_TOOL_ROOT = pathlib.Path(__file__).resolve().parents[2] / "tools" / "release"
if str(RELEASE_TOOL_ROOT) not in sys.path:
    sys.path.insert(0, str(RELEASE_TOOL_ROOT))

from public_reference_acquisition_policy import (  # noqa: E402
    is_public_reference_acquisition_metadata,
    validate_public_reference_acquisition_metadata_inventory,
)


REFERENCE_TEST_PREFIX = "project/tests/reference_regression/"
REFERENCE_ACQUISITION_PREFIX = (
    "project/tests/reference_regression/reference_release_acquisition/"
)
NATIVE_TOOL_COMPONENT = "/native_tool_harness/tools/"
FORBIDDEN_COMPONENTS = {
    "acquired",
    "clean-room",
    "evidence",
    "license",
    "release-notes",
    "source",
    "source-archive",
    "version-proof",
    "visibility-redaction",
}
FORBIDDEN_FILENAMES = {
    "RELEASE_EVIDENCE_MANIFEST.yaml",
    "TREE_MANIFEST.sha256",
}


def tracked_files(repo_root: pathlib.Path) -> list[str]:
    result = subprocess.run(
        ["git", "ls-files", REFERENCE_TEST_PREFIX],
        cwd=repo_root,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
    )
    return [line for line in result.stdout.splitlines() if line]


def git_lines(repo_root: pathlib.Path, args: list[str]) -> list[str]:
    result = subprocess.run(
        ["git", *args],
        cwd=repo_root,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
    )
    return [line for line in result.stdout.splitlines() if line]


def declared_native_tools(repo_root: pathlib.Path) -> set[str]:
    allowed: set[str] = set()
    manifests = sorted(
        (repo_root / REFERENCE_TEST_PREFIX).glob(
            "*/native_tool_harness/native_tool_harness_manifest.csv"
        )
    )
    for manifest in manifests:
        with manifest.open(newline="", encoding="utf-8") as handle:
            reader = csv.DictReader(handle)
            for row in reader:
                locator = (row.get("tool_locator") or "").strip()
                if not locator.startswith(REFERENCE_TEST_PREFIX):
                    continue
                if NATIVE_TOOL_COMPONENT not in locator:
                    continue
                allowed.add(locator)
    return allowed


def local_payload_roots(repo_root: pathlib.Path) -> list[pathlib.Path]:
    root = repo_root / "project/tests/reference_regression"
    roots: list[pathlib.Path] = []
    acquisition = root / "reference_release_acquisition"
    if acquisition.exists():
        roots.append(acquisition)
    for family_dir in sorted(path for path in root.iterdir() if path.is_dir()):
        for candidate in sorted(family_dir.glob("original*")):
            if candidate.is_dir():
                roots.append(candidate)
    return roots


def tracked_under(repo_root: pathlib.Path, root: pathlib.Path) -> list[str]:
    rel = root.relative_to(repo_root).as_posix()
    return git_lines(repo_root, ["ls-files", rel])


def is_ignored(repo_root: pathlib.Path, path: pathlib.Path) -> bool:
    rel = path.relative_to(repo_root).as_posix()
    candidates = [rel]
    if path.is_dir():
        candidates.append(rel.rstrip("/") + "/")
        first_file = next((child for child in path.rglob("*") if child.is_file()), None)
        if first_file is not None:
            candidates.append(first_file.relative_to(repo_root).as_posix())
    for candidate in candidates:
        result = subprocess.run(
            ["git", "check-ignore", "--quiet", candidate],
            cwd=repo_root,
            text=True,
        )
        if result.returncode == 0:
            return True
    return False


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--evidence-file", type=pathlib.Path)
    args = parser.parse_args()

    repo_root = pathlib.Path(args.repo_root).resolve()
    failures: list[str] = []
    failures.extend(
        "invalid registered reference acquisition metadata: " + error
        for error in validate_public_reference_acquisition_metadata_inventory(repo_root)
    )
    allowed_tools = declared_native_tools(repo_root)
    tracked_count = 0
    local_payload_count = 0
    ignored_local_payload_count = 0

    for rel in tracked_files(repo_root):
        tracked_count += 1
        if not rel.startswith(REFERENCE_ACQUISITION_PREFIX):
            continue
        suffix = rel[len(REFERENCE_ACQUISITION_PREFIX) :]
        components = suffix.split("/")
        filename = components[-1]
        if filename in FORBIDDEN_FILENAMES:
            failures.append(f"{rel}: release-source evidence manifests are private")
        blocked = sorted(set(components) & FORBIDDEN_COMPONENTS)
        if blocked:
            failures.append(f"{rel}: forbidden reference public payload component {blocked[0]!r}")
        if suffix and components[2:] and components[2] != "regression":
            failures.append(f"{rel}: reference acquisition payload must be under regression/")

    for rel in tracked_files(repo_root):
        if NATIVE_TOOL_COMPONENT not in rel:
            continue
        if rel not in allowed_tools:
            failures.append(f"{rel}: tracked reference native tool is not declared in a harness manifest")

    for root in local_payload_roots(repo_root):
        local_payload_count += 1
        rel = root.relative_to(repo_root).as_posix()
        tracked_payload = [
            path for path in tracked_under(repo_root, root)
            if is_public_reference_acquisition_metadata(path)
        ]
        all_tracked = tracked_under(repo_root, root)
        unexpected = sorted(set(all_tracked) - set(tracked_payload))
        if unexpected:
            failures.append(f"{rel}: tracked local payload files are not public-safe scope manifests")
        if root.name.startswith("original") and not is_ignored(repo_root, root):
            failures.append(f"{rel}: local original-suite payload root is not gitignored")
        if root.name.startswith("original") and is_ignored(repo_root, root):
            ignored_local_payload_count += 1
        if root.name == "reference_release_acquisition":
            ignored = is_ignored(repo_root, root)
            if ignored:
                ignored_local_payload_count += 1

    if failures:
        print("\n".join(failures[:300]))
        if len(failures) > 300:
            print(f"... {len(failures) - 300} more")
        return 1

    if args.evidence_file:
        args.evidence_file.parent.mkdir(parents=True, exist_ok=True)
        args.evidence_file.write_text(
            json.dumps(
                {
                    "gate": "reference_public_payload_policy_gate",
                    "status": "passed",
                    "tracked_reference_files": tracked_count,
                    "declared_native_tool_paths": len(allowed_tools),
                    "local_payload_roots": local_payload_count,
                    "ignored_local_payload_roots": ignored_local_payload_count,
                    "forbidden_tracked_components": sorted(FORBIDDEN_COMPONENTS),
                },
                indent=2,
                sort_keys=True,
            )
            + "\n",
            encoding="utf-8",
        )
    print("reference_public_payload_policy_gate: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
