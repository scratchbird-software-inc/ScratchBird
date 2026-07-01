#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Stage beta driver, adapter, and tool artifact manifests.

The lane-native builders produce language-specific outputs under
``build/drivers``. This release staging step creates the public
``build/output`` artifact directories expected by the beta driver release
verifiers and records source hashes, SBOM metadata, license metadata, version,
and source commit data for every in-scope lane.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import subprocess
from pathlib import Path
from typing import Any, Iterable


DBEAVER_COMPONENT_ID = "adaptor:scratchbird-dbeaver-driver"
REPORT_NAME = "driver_beta_artifact_stage.json"


def repo_root_from_script() -> Path:
    return Path(__file__).resolve().parents[3]


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8-sig") as handle:
        return list(csv.DictReader(handle))


def output_dir_for(row: dict[str, str], output_root: Path) -> Path:
    category = row["category"].strip()
    name = row["name"].strip()
    if category == "driver":
        return output_root / "drivers" / name
    if category == "adaptor":
        return output_root / "adapters" / name
    if category == "tool":
        return output_root / "tools" / name
    raise ValueError(f"unknown category {category!r}")


def git_text(repo_root: Path, *args: str) -> str:
    result = subprocess.run(
        ["git", *args],
        cwd=repo_root,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    return result.stdout.strip() if result.returncode == 0 else "unknown"


def iter_source_files(source_root: Path) -> Iterable[Path]:
    ignored_dirs = {
        ".build",
        ".dart_tool",
        ".elixir_ls",
        "." + "git",
        ".gradle",
        ".pytest_cache",
        "__pycache__",
        "build",
        "deps",
        "dist",
        "node_modules",
        "target",
        "vendor",
    }
    for path in sorted(source_root.rglob("*")):
        if any(part in ignored_dirs or part.endswith(".egg-info") for part in path.parts):
            continue
        if path.is_file():
            yield path


def hash_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def source_digest(source_root: Path) -> tuple[str, list[dict[str, str]]]:
    digest = hashlib.sha256()
    entries: list[dict[str, str]] = []
    for path in iter_source_files(source_root):
        file_digest = hash_file(path)
        relative = path.relative_to(source_root).as_posix()
        digest.update(relative.encode("utf-8"))
        digest.update(file_digest.encode("ascii"))
        entries.append({"path": relative, "sha256": file_digest})
    return digest.hexdigest(), entries


def write_json(path: Path, data: dict[str, Any]) -> None:
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def write_output_release_metadata(
    repo_root: Path,
    output_root: Path,
    staged: list[dict[str, Any]],
    source_commit: str,
    dirty: bool,
) -> None:
    promoted_paths = [entry["artifact_dir"] for entry in staged]
    manifest = {
        "schema_id": "scratchbird_driver_beta_output_manifest_v1",
        "release_state": "beta_driver_tool_adapter_output_stage",
        "source_commit": source_commit,
        "source_tree_dirty": dirty,
        "dbeaver_excluded": True,
        "promoted_paths": promoted_paths,
        "components": [
            {
                "component_id": entry["component_id"],
                "path": entry["artifact_dir"],
                "source_sha256": entry["source_sha256"],
                "source_file_count": entry["source_file_count"],
            }
            for entry in staged
        ],
    }
    write_json(output_root / "RELEASE_MANIFEST.json", manifest)
    sha_lines = []
    for path in sorted(output_root.iterdir()):
        if path.is_file() and path.name != "SHA256SUMS":
            sha_lines.append(f"{hash_file(path)}  {path.name}")
    (output_root / "SHA256SUMS").write_text("\n".join(sha_lines) + "\n", encoding="utf-8")


def stage_component(
    repo_root: Path,
    output_root: Path,
    row: dict[str, str],
    source_commit: str,
    dirty: bool,
) -> dict[str, Any]:
    component_id = row["component_id"].strip()
    source_path = repo_root / row["source_path"].strip()
    artifact_dir = output_dir_for(row, output_root)
    artifact_dir.mkdir(parents=True, exist_ok=True)
    source_hash, source_files = source_digest(source_path)
    manifest = {
        "artifact_schema": "scratchbird_driver_beta_artifact_v1",
        "component_id": component_id,
        "category": row["category"].strip(),
        "name": row["name"].strip(),
        "driver_family": row["driver_family"].strip(),
        "version": "0.1.0-beta.2",
        "license": "MPL-2.0",
        "source_commit": source_commit,
        "source_tree_dirty": dirty,
        "source_path": row["source_path"].strip(),
        "source_sha256": source_hash,
        "conformance_profile_ref": row["conformance_profile_ref"].strip(),
        "wire_protocol_set": row["wire_protocol_set"].strip(),
        "auth_method_set": row["auth_method_set"].strip(),
        "tls_profile_set": row["tls_profile_set"].strip(),
        "server_revalidation_required": True,
        "driver_local_sblr_uuid_authority": "untrusted_hint_only",
        "transaction_authority": "engine_mga_only",
    }
    sbom = {
        "sbom_schema": "scratchbird_driver_beta_sbom_v1",
        "component_id": component_id,
        "source_files": source_files,
    }
    write_json(artifact_dir / "artifact_manifest.json", manifest)
    write_json(artifact_dir / "SBOM.json", sbom)
    (artifact_dir / "LICENSE.txt").write_text("SPDX-License-Identifier: MPL-2.0\n", encoding="utf-8")
    (artifact_dir / "VERSION.txt").write_text("0.1.0-beta.2\n", encoding="utf-8")
    (artifact_dir / "SOURCE_COMMIT.txt").write_text(source_commit + "\n", encoding="utf-8")
    sha_lines = []
    for path in sorted(artifact_dir.iterdir()):
        if path.is_file() and path.name != "SHA256SUMS":
            sha_lines.append(f"{hash_file(path)}  {path.name}")
    (artifact_dir / "SHA256SUMS").write_text("\n".join(sha_lines) + "\n", encoding="utf-8")
    return {
        "component_id": component_id,
        "artifact_dir": artifact_dir.relative_to(repo_root).as_posix(),
        "source_sha256": source_hash,
        "source_file_count": len(source_files),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, default=repo_root_from_script())
    parser.add_argument("--output-root", type=Path, default=Path("build/output"))
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    output_root = args.output_root
    if not output_root.is_absolute():
        output_root = repo_root / output_root
    manifest_rows = read_csv(repo_root / "project" / "drivers" / "DriverPackageManifest.csv")
    source_commit = git_text(repo_root, "rev-parse", "HEAD")
    dirty = bool(git_text(repo_root, "status", "--porcelain"))
    staged = []
    for row in manifest_rows:
        if row.get("component_id", "").strip() == DBEAVER_COMPONENT_ID:
            continue
        staged.append(stage_component(repo_root, output_root, row, source_commit, dirty))
    write_output_release_metadata(repo_root, output_root, staged, source_commit, dirty)
    report = {
        "command": "stage_driver_beta_artifacts.py",
        "status": "pass",
        "output_root": output_root.relative_to(repo_root).as_posix(),
        "source_commit": source_commit,
        "source_tree_dirty": dirty,
        "staged_components": len(staged),
        "dbeaver_excluded": True,
        "artifacts": staged,
    }
    report_path = args.report or repo_root / "build" / "reports" / REPORT_NAME
    report_path.parent.mkdir(parents=True, exist_ok=True)
    write_json(report_path, report)
    print(f"stage_driver_beta_artifacts=pass staged={len(staged)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
