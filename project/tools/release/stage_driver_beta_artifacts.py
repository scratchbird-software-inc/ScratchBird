#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Create a non-publishable source-provenance snapshot for driver source.

This utility does not build, package, install, or test a driver, adaptor, or
tool.  Its output is intentionally incompatible with every release verifier:
it is source metadata only and must never be uploaded as a driver release.

Real driver/adaptor/MCP publication is reserved for the completed-component
nightly route, which must build an installable payload and prove an installed
live ScratchBird route before it can publish anything.
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
DBEAVER_ALIASES = frozenset(
    {
        "adaptor:dbeaver",
        DBEAVER_COMPONENT_ID,
        "scratchbird-dbeaver-driver",
        "dbeaver",
    }
)
REPORT_NAME = "driver_beta_metadata_snapshot.json"
METADATA_SNAPSHOT_NAME = "METADATA_SNAPSHOT.json"
SOURCE_METADATA_NAME = "source_metadata.json"
SOURCE_INVENTORY_NAME = "source_inventory.json"
SOURCE_CHECKSUMS_NAME = "SOURCE_METADATA_SHA256SUMS"


def repo_root_from_script() -> Path:
    return Path(__file__).resolve().parents[3]


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8-sig") as handle:
        return list(csv.DictReader(handle))


def ascii_lower(value: str) -> str:
    return "".join(
        character.lower() if "A" <= character <= "Z" else character
        for character in value
    )


def is_dbeaver_identity(value: object) -> bool:
    if not isinstance(value, str):
        return False
    normalized = ascii_lower(value.strip())
    return normalized in DBEAVER_ALIASES or "dbeaver" in normalized


def row_is_dbeaver(row: dict[str, str]) -> bool:
    return any(
        is_dbeaver_identity(row.get(field, ""))
        for field in ("component_id", "category", "name", "driver_family", "source_path")
    )


def metadata_dir_for(row: dict[str, str], output_root: Path) -> Path:
    category = row["category"].strip()
    name = row["name"].strip()
    if category not in {"driver", "adaptor", "tool"}:
        raise ValueError(f"unknown category {category!r}")
    return output_root / "components" / category / name


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


def is_within(path: Path, root: Path) -> bool:
    try:
        path.relative_to(root)
    except ValueError:
        return False
    return True


def reject_release_shaped_output(repo_root: Path, output_root: Path) -> None:
    release_root = (repo_root / "build" / "output").resolve()
    if is_within(output_root, release_root):
        raise ValueError(
            "metadata snapshot output must not be build/output or a descendant; "
            "that location is reserved for verified release payloads"
        )


def write_snapshot_metadata(
    output_root: Path,
    snapshots: list[dict[str, Any]],
    source_commit: str,
    dirty: bool,
) -> None:
    snapshot = {
        "schema_id": "scratchbird.driver_source_metadata_snapshot.v1",
        "release_eligible": False,
        "must_not_publish": True,
        "source_commit": source_commit,
        "source_tree_dirty": dirty,
        "dbeaver_excluded": True,
        "components": [
            {
                "component_id": entry["component_id"],
                "metadata_dir": entry["metadata_dir"],
                "source_sha256": entry["source_sha256"],
                "source_file_count": entry["source_file_count"],
            }
            for entry in snapshots
        ],
    }
    write_json(output_root / METADATA_SNAPSHOT_NAME, snapshot)
    checksum_entries = []
    for path in sorted(output_root.iterdir()):
        if path.is_file() and path.name != SOURCE_CHECKSUMS_NAME:
            checksum_entries.append(f"{hash_file(path)}  {path.name}")
    (output_root / SOURCE_CHECKSUMS_NAME).write_text(
        "\n".join(checksum_entries) + "\n", encoding="utf-8"
    )


def snapshot_component(
    repo_root: Path,
    output_root: Path,
    row: dict[str, str],
    source_commit: str,
    dirty: bool,
) -> dict[str, Any]:
    component_id = row["component_id"].strip()
    source_path = repo_root / row["source_path"].strip()
    metadata_dir = metadata_dir_for(row, output_root)
    metadata_dir.mkdir(parents=True, exist_ok=True)
    source_hash, source_files = source_digest(source_path)
    metadata = {
        "schema_id": "scratchbird.driver_source_metadata.v1",
        "release_eligible": False,
        "must_not_publish": True,
        "component_id": component_id,
        "category": row["category"].strip(),
        "name": row["name"].strip(),
        "driver_family": row["driver_family"].strip(),
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
    inventory = {
        "schema_id": "scratchbird.driver_source_inventory.v1",
        "release_eligible": False,
        "must_not_publish": True,
        "component_id": component_id,
        "source_files": source_files,
    }
    write_json(metadata_dir / SOURCE_METADATA_NAME, metadata)
    write_json(metadata_dir / SOURCE_INVENTORY_NAME, inventory)
    checksum_entries = []
    for path in sorted(metadata_dir.iterdir()):
        if path.is_file() and path.name != SOURCE_CHECKSUMS_NAME:
            checksum_entries.append(f"{hash_file(path)}  {path.name}")
    (metadata_dir / SOURCE_CHECKSUMS_NAME).write_text(
        "\n".join(checksum_entries) + "\n", encoding="utf-8"
    )
    return {
        "component_id": component_id,
        "metadata_dir": metadata_dir.relative_to(output_root).as_posix(),
        "source_sha256": source_hash,
        "source_file_count": len(source_files),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, default=repo_root_from_script())
    parser.add_argument(
        "--metadata-only",
        action="store_true",
        help="acknowledge that this command writes non-publishable source metadata only",
    )
    parser.add_argument(
        "--output-root",
        type=Path,
        default=Path("build/reports/driver-beta-metadata"),
    )
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()

    if not args.metadata_only:
        parser.error(
            "--metadata-only is required: this command cannot stage a driver release"
        )

    repo_root = args.repo_root.resolve()
    output_root = args.output_root
    if not output_root.is_absolute():
        output_root = repo_root / output_root
    output_root = output_root.resolve()
    try:
        reject_release_shaped_output(repo_root, output_root)
    except ValueError as exc:
        parser.error(str(exc))

    manifest_rows = read_csv(repo_root / "project" / "drivers" / "DriverPackageManifest.csv")
    source_commit = git_text(repo_root, "rev-parse", "HEAD")
    dirty = bool(git_text(repo_root, "status", "--porcelain"))
    snapshots = []
    for row in manifest_rows:
        if row_is_dbeaver(row):
            continue
        snapshots.append(snapshot_component(repo_root, output_root, row, source_commit, dirty))
    write_snapshot_metadata(output_root, snapshots, source_commit, dirty)
    report = {
        "command": "stage_driver_beta_artifacts.py",
        "result": "metadata_snapshot",
        "release_eligible": False,
        "must_not_publish": True,
        "output_root": output_root.relative_to(repo_root).as_posix()
        if is_within(output_root, repo_root)
        else str(output_root),
        "source_commit": source_commit,
        "source_tree_dirty": dirty,
        "snapshotted_components": len(snapshots),
        "dbeaver_excluded": True,
        "components": snapshots,
    }
    report_path = args.report or repo_root / "build" / "reports" / REPORT_NAME
    if not report_path.is_absolute():
        report_path = repo_root / report_path
    report_path.parent.mkdir(parents=True, exist_ok=True)
    write_json(report_path, report)
    print(
        "stage_driver_beta_artifacts=metadata_snapshot "
        f"components={len(snapshots)} release_eligible=false"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
