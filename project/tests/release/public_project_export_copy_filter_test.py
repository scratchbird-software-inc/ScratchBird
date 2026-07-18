#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Exercise the source-archive-safe public export acquisition filter."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import sys


REPO_ROOT = Path(__file__).resolve().parents[3]
RELEASE_TOOL_ROOT = REPO_ROOT / "project" / "tools" / "release"
if str(RELEASE_TOOL_ROOT) not in sys.path:
    sys.path.insert(0, str(RELEASE_TOOL_ROOT))

from public_project_export_gate import (  # noqa: E402
    check_package_shape,
    copy_public_tree,
)
from public_reference_acquisition_policy import (  # noqa: E402
    REFERENCE_ACQUISITION_PREFIX,
    public_reference_acquisition_metadata_relative_paths,
)


ROOT_FILES = (
    "LICENSE",
    "NOTICE",
    "SECURITY.md",
    "KNOWN_LIMITATIONS.md",
    "RELEASE_TERMS.md",
    "THIRD_PARTY_NOTICES.md",
    "SBOM.json",
    "REFERENCE_SYSTEMS_AND_IP_BOUNDARY.md",
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def write(path: Path, text: str = "fixture\n") -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def build_source_archive(source_root: Path) -> tuple[tuple[Path, ...], tuple[Path, ...]]:
    for name in ROOT_FILES:
        write(source_root / name)

    for directory in ("LICENSES", "data", "public_contract_snapshot"):
        write(source_root / directory / "fixture.txt")
    write(source_root / "docs" / "build_requirements" / "README.md")
    write(source_root / "project" / "ordinary-public-fixture.txt")
    write(source_root / "release" / "README.md")
    for platform in ("linux", "windows", "freebsd", "macos"):
        write(source_root / "release" / platform / "ENGINE_BINARY_LAYOUT.json", "{}\n")

    allowed: list[Path] = []
    for relative in public_reference_acquisition_metadata_relative_paths():
        canonical = REPO_ROOT / relative
        require(canonical.is_file(), f"registered metadata is absent: {relative}")
        staged_source = source_root / relative
        staged_source.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(canonical, staged_source)
        allowed.append(staged_source)

    fixture_regression_root = (
        source_root
        / REFERENCE_ACQUISITION_PREFIX
        / "fixture_reference"
        / "1.0.0"
        / "regression"
    )
    forbidden = (
        fixture_regression_root / "FIXTURE_MANIFEST.csv",
        fixture_regression_root / "FIXTURE_ACQUISITION_MANIFEST.csv",
        fixture_regression_root / "acquired" / "upstream-test.sql",
        fixture_regression_root / "tools" / "reference-tool",
        fixture_regression_root / "regress" / "upstream-test.sql",
        fixture_regression_root / "nested" / "NESTED_MANIFEST.csv",
    )
    for path in forbidden:
        write(path)
    return tuple(allowed), forbidden


def require_invalid_registered_metadata_fails(
    source_root: Path, work_root: Path, relative: str, replacement: str
) -> None:
    invalid_source = work_root / "invalid-source-archive"
    invalid_stage = work_root / "invalid-public-export"
    shutil.copytree(source_root, invalid_source)
    write(invalid_source / relative, replacement)
    try:
        copy_public_tree(invalid_source, invalid_stage)
    except RuntimeError as exc:
        require(
            "metadata_content_hash_mismatch" in str(exc),
            f"altered registered metadata failed for the wrong reason: {exc}",
        )
    else:
        require(False, "altered registered metadata entered the public export")


def require_registered_symlink_fails(
    source_root: Path, work_root: Path, relative: str
) -> None:
    if os.name == "nt":
        return
    invalid_source = work_root / "symlink-source-archive"
    invalid_stage = work_root / "symlink-public-export"
    shutil.copytree(source_root, invalid_source)
    target = invalid_source / relative
    target.unlink()
    target.symlink_to(REPO_ROOT / relative)
    try:
        copy_public_tree(invalid_source, invalid_stage)
    except RuntimeError as exc:
        require(
            "metadata_must_not_be_symlink" in str(exc),
            f"registered metadata symlink failed for the wrong reason: {exc}",
        )
    else:
        require(False, "registered metadata symlink entered the public export")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work-root", type=Path, required=True)
    args = parser.parse_args()

    work_root = args.work_root.resolve()
    if work_root.exists():
        shutil.rmtree(work_root)
    source_root = work_root / "source-archive"
    stage_root = work_root / "public-export"
    allowed, forbidden = build_source_archive(source_root)

    require(
        not (source_root / ("." + "git")).exists(),
        "fixture must be a source archive without Git metadata",
    )
    copy_public_tree(source_root, stage_root)
    check_package_shape(stage_root)

    require(
        (stage_root / "project" / "ordinary-public-fixture.txt").is_file(),
        "ordinary project fixture was not staged",
    )
    for source in allowed:
        relative = source.relative_to(source_root)
        require((stage_root / relative).is_file(), f"allowed metadata was not staged: {relative}")
    for source in forbidden:
        relative = source.relative_to(source_root)
        require(not (stage_root / relative).exists(), f"local payload was staged: {relative}")

    require(
        not (stage_root / "public_execution_plan").exists(),
        "stale public execution-plan root was fabricated",
    )
    require(
        not (stage_root / "public_input_snapshot").exists(),
        "stale public input-snapshot root was fabricated",
    )
    registered_paths = public_reference_acquisition_metadata_relative_paths()
    require(
        len(allowed) == len(registered_paths),
        "source fixture did not contain every registered metadata file",
    )
    require_invalid_registered_metadata_fails(
        source_root,
        work_root,
        registered_paths[0],
        "raw upstream regression payload renamed as metadata\n",
    )
    require_registered_symlink_fails(source_root, work_root, registered_paths[0])

    print("public_project_export_copy_filter_test=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
