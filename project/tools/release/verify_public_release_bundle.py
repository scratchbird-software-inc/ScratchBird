#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate a staged public release bundle and emit an artifact manifest."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import subprocess
import sys
from typing import Any


MANIFEST_NAME = "PUBLIC_RELEASE_ARTIFACT_MANIFEST.json"


def run(command: list[str], *, cwd: Path) -> str:
    result = subprocess.run(
        command,
        cwd=cwd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if result.returncode != 0:
        print(result.stdout, end="")
        raise SystemExit(f"command_failed:{command[0]}:exit={result.returncode}")
    return result.stdout.strip()


def optional_run(command: list[str], *, cwd: Path) -> str | None:
    try:
        return run(command, cwd=cwd)
    except (OSError, SystemExit):
        return None


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def collect_files(root: Path) -> list[dict[str, Any]]:
    files: list[dict[str, Any]] = []
    for path in sorted(candidate for candidate in root.rglob("*") if candidate.is_file()):
        if path.name == MANIFEST_NAME:
            continue
        stat = path.stat()
        files.append(
            {
                "path": path.relative_to(root).as_posix(),
                "bytes": stat.st_size,
                "sha256": sha256(path),
            }
        )
    return files


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("artifact_root", type=Path)
    parser.add_argument("--repo-root", type=Path)
    args = parser.parse_args()

    repo_root = (args.repo_root or Path(__file__).resolve().parents[3]).resolve()
    artifact_root = args.artifact_root.resolve()
    if not artifact_root.is_dir():
        raise SystemExit(f"artifact_root_not_found:{artifact_root}")
    if artifact_root.name not in {"linux", "windows", "bsd"}:
        raise SystemExit(f"unsupported_artifact_platform:{artifact_root.name}")

    stage_gate = repo_root / "project" / "tools" / "release" / "public_output_stage_gate.py"
    run(
        [
            sys.executable,
            str(stage_gate),
            f"--artifact-root={artifact_root}",
            f"--platform={artifact_root.name}",
        ],
        cwd=repo_root,
    )

    commit = optional_run(["git", "rev-parse", "HEAD"], cwd=repo_root)
    dirty = optional_run(["git", "status", "--short"], cwd=repo_root)
    cmake_version = optional_run(["cmake", "--version"], cwd=repo_root)
    cxx_version = optional_run(["c++", "--version"], cwd=repo_root)

    manifest = {
        "schema_id": "scratchbird.public_release_artifact_manifest.v1",
        "generated_at_utc": datetime.now(timezone.utc).replace(microsecond=0).isoformat(),
        "artifact_root": artifact_root.as_posix(),
        "platform": artifact_root.name,
        "source": {
            "commit": commit,
            "dirty": bool(dirty),
        },
        "tools": {
            "cmake": cmake_version.splitlines()[0] if cmake_version else None,
            "cxx": cxx_version.splitlines()[0] if cxx_version else None,
        },
        "files": collect_files(artifact_root),
    }
    output_path = artifact_root / MANIFEST_NAME
    output_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"verify_public_release_bundle=passed:{output_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
