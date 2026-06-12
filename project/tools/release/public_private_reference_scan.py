#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import argparse
import os
from pathlib import Path


SKIP_DIRS = {
    "__pycache__",
    ".pytest_cache",
    "build",
    "cmake-build-debug",
    "cmake-build-release",
    "node_modules",
    "vendor",
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

SKIP_PATH_PREFIXES = (
    Path("tests/reference_regression/reference_release_acquisition"),
    Path("tests/reference_regression/firebird/original_firebird_qa"),
)

GIT_REFERENCE_ALLOWLIST = {
    Path("drivers/driver/cpp/include/nlohmann/json.hpp"),
    Path("drivers/tool/cli/include/nlohmann/json.hpp"),
    Path("drivers/driver/php/composer.lock"),
    Path("drivers/driver/mojo/README.md"),
    Path("drivers/driver/mojo/BASELINE_REQUIREMENT_MAPPING.md"),
    Path("drivers/driver/swift/Package.swift"),
    Path("drivers/driver/swift/Package.resolved"),
    Path("drivers/adaptor/scratchbird-metabase-driver/deps.edn"),
    Path(
        "tests/firebird_parser_worker/fixtures/full_firebirdsql_parser_udr_emulation_closure/"
        "artifacts/FIREBIRD_QA_CANDIDATE_ASSET_HASH_MANIFEST.csv"
    ),
    Path("resources/seed-packs/initial-resource-pack/resources/timezones/CONTRIBUTING"),
    Path("resources/seed-packs/initial-resource-pack/resources/timezones/NEWS"),
    Path("resources/seed-packs/initial-resource-pack/resources/timezones/theory.html"),
    Path("resources/seed-packs/initial-resource-pack/resources/timezones/tz-link.html"),
}


def allow_git_reference(rel: Path) -> bool:
    if rel.name == "." + "gitignore":
        return True
    return rel in GIT_REFERENCE_ALLOWLIST


def banned_needles() -> list[tuple[str, str]]:
    private_docs = "docs" + "/"
    return [
        ("private_execution_plan_reference", private_docs + "execution-plans"),
        ("private_completed_execution_plan_reference", private_docs + "completed-execution-plans"),
        ("private_findings_reference", private_docs + "findings"),
        ("git_metadata_reference", "." + "git"),
        ("local_home_path_reference", "/" + "home" + "/" + "dcalford"),
        ("private_repo_reference", "ScratchBird" + "-Private"),
        ("legacy_repo_runtime_reference", "local workspace" + "/" + "ScratchBird"),
    ]


def iter_files(root: Path):
    for dirpath, dirnames, filenames in os.walk(root):
      dirnames[:] = [name for name in dirnames if name not in SKIP_DIRS]
      for filename in filenames:
          path = Path(dirpath) / filename
          rel = path.relative_to(root)
          if any(rel == prefix or prefix in rel.parents for prefix in SKIP_PATH_PREFIXES):
              continue
          if path.suffix in SKIP_SUFFIXES:
              continue
          yield path


def scan(root: Path) -> list[str]:
    findings: list[str] = []
    for path in iter_files(root):
        try:
            text = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        rel = path.relative_to(root)
        for label, needle in banned_needles():
            if label == "git_metadata_reference" and allow_git_reference(rel):
                continue
            if needle in text:
                findings.append(f"{rel}: {label}: {needle}")
    return findings


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--project-root", type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args()

    root = args.project_root.resolve()
    findings = scan(root)
    if findings:
        print("public private-reference scan failed")
        for finding in findings[:200]:
            print(finding)
        if len(findings) > 200:
            print(f"... {len(findings) - 200} additional findings omitted")
        return 1
    print("public private-reference scan passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
