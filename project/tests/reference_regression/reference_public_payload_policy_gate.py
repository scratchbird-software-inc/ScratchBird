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
import pathlib
import subprocess
import sys


REFERENCE_TEST_PREFIX = "project/tests/reference_regression/"
REFERENCE_ACQUISITION_PREFIX = (
    "project/tests/reference_regression/reference_release_acquisition/"
)
NATIVE_TOOL_COMPONENT = "/native_tool_harness/tools/"
FORBIDDEN_COMPONENTS = {
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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    args = parser.parse_args()

    repo_root = pathlib.Path(args.repo_root).resolve()
    failures: list[str] = []

    for rel in tracked_files(repo_root):
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
        failures.append(f"{rel}: reference native tool payloads must not be tracked")

    if failures:
        print("\n".join(failures[:300]))
        if len(failures) > 300:
            print(f"... {len(failures) - 300} more")
        return 1

    print("reference_public_payload_policy_gate: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
