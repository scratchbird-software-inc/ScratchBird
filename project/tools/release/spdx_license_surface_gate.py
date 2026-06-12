#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate the release-tool SPDX and third-party notice surface."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
from typing import Any


SOURCE_SUFFIXES = {".py", ".cpp", ".hpp", ".h", ".c", ".sh", ".ps1"}
SOURCE_ROOTS = (
    "project/tools/release",
    "project/tests/release",
)
REQUIRED_NOTICE_FILES = (
    "LICENSE",
    "NOTICE",
    "THIRD_PARTY_NOTICES.md",
    "SBOM.json",
)


def write_output(output: Path | None, payload: dict[str, Any]) -> None:
    if output is None:
        return
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[3])
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    errors: list[str] = []
    checked_sources = 0

    for rel_path in REQUIRED_NOTICE_FILES:
        if not (repo_root / rel_path).is_file():
            errors.append(f"missing required notice file: {rel_path}")

    sbom_path = repo_root / "SBOM.json"
    if sbom_path.is_file():
        payload = json.loads(sbom_path.read_text(encoding="utf-8"))
        if payload.get("schema") != "scratchbird.public_source_review_sbom.v1":
            errors.append("SBOM.json schema mismatch")
        components = payload.get("components", [])
        if not isinstance(components, list) or not components:
            errors.append("SBOM.json components must be a non-empty list")
        if not any(
            isinstance(component, dict)
            and component.get("component_id") == "reference-system-test-material"
            and component.get("tracked_in_public_repository") is False
            for component in components
        ):
            errors.append("SBOM.json must mark reference test material as external/not tracked")

    third_party = repo_root / "THIRD_PARTY_NOTICES.md"
    if third_party.is_file():
        text = third_party.read_text(encoding="utf-8", errors="replace")
        for snippet in (
            "Raw upstream payloads are not tracked",
            "downloaded/native reference tools are not part of the tracked public GitHub source surface",
        ):
            if snippet not in text:
                errors.append(f"THIRD_PARTY_NOTICES.md missing: {snippet}")

    for rel_root in SOURCE_ROOTS:
        root = repo_root / rel_root
        if not root.is_dir():
            errors.append(f"missing source root: {rel_root}")
            continue
        for path in sorted(root.rglob("*")):
            if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
                continue
            checked_sources += 1
            header = path.read_text(encoding="utf-8", errors="replace")[:800]
            if "SPDX-License-Identifier:" not in header:
                errors.append(f"missing SPDX header: {path.relative_to(repo_root).as_posix()}")

    evidence = {
        "schema": "scratchbird.spdx_license_surface_gate.v1",
        "status": "failed" if errors else "passed",
        "checked_sources": checked_sources,
        "required_notice_files": list(REQUIRED_NOTICE_FILES),
        "errors": errors,
    }
    write_output(args.output, evidence)

    if errors:
        for error in errors[:200]:
            print(f"spdx_license_surface_gate:error:{error}", file=sys.stderr)
        return 1
    print("spdx_license_surface_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
