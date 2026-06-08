#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Public CTest wrapper for the cluster catalog manifest source gate."""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys


# PUBLIC_CLUSTER_MANIFEST_SOURCE_GATE


def fail(message: str) -> None:
    print(f"public_cluster_manifest_source_gate=fail:{message}", file=sys.stderr)
    raise SystemExit(1)


def require_file(path: Path, token: str) -> None:
    if not path.exists() or not path.is_file():
        fail(f"expected_output_missing:{path.name}")
    text = path.read_text(encoding="utf-8")
    if token not in text:
        fail(f"expected_output_token_missing:{path.name}:{token}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--project-root", type=Path, required=True)
    parser.add_argument("--build-root", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()

    project_root = args.project_root.resolve()
    build_root = args.build_root.resolve()
    output_dir = args.output_dir.resolve()
    tool = project_root / "tools" / "release" / "cluster_catalog_manifest_generator.py"
    manifest = (
        project_root / "resources" / "cluster-catalog" /
        "cluster_catalog_manifest_source.json"
    )
    command = [
        sys.executable,
        str(tool),
        "--project-root",
        str(project_root),
        "--manifest",
        str(manifest),
        "--build-root",
        str(build_root),
        "--output-dir",
        str(output_dir),
    ]
    completed = subprocess.run(command, text=True, check=False)
    if completed.returncode != 0:
        return completed.returncode

    require_file(output_dir / "cluster_catalog_manifest_matrix.csv",
                 "base_table")
    require_file(output_dir / "cluster_catalog_manifest_source_proof.json",
                 "PCR-GATE-099")
    require_file(output_dir / "cluster_catalog_manifest_readable.md",
                 "Cluster Catalog Manifest Source Proof")
    print("public_cluster_manifest_source_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
