#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Promote or verify reference parser worker packages under packaging/."""

from __future__ import annotations

import argparse
import csv
from datetime import datetime, timezone
import hashlib
import json
import shutil
import subprocess
import tarfile
from pathlib import Path
from typing import Any


REPORT_NAME = "reference_parser_packaging_promotion.json"
MANIFEST_REL = Path("project/src/parsers/compatibility/CompatibilityProfileManifest.csv")
LEGAL_SOURCE_FILES = ("LICENSE", "NOTICE", "THIRD_PARTY_NOTICES.md")
ROOT_SBOM_REL = Path("SBOM.json")
ROOT_METADATA = {"RELEASE_MANIFEST.json", "SHA256SUMS"}
REFERENCE_SOURCE_RELS = (
    "project/src/parsers/compatibility",
    "project/tests/compatibility_sql_parser_first_tranche",
    "project/tests/reference_regression",
)


def repo_root_from_script() -> Path:
    return Path(__file__).resolve().parents[2]


def git_text(repo_root: Path, *args: str) -> str | None:
    result = subprocess.run(
        ["git", *args],
        cwd=repo_root,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    if result.returncode != 0:
        return None
    return result.stdout.strip()


def latest_release_dir(repo_root: Path) -> Path:
    candidates = [
        path for path in (repo_root / "packaging").iterdir()
        if path.is_dir() and path.name[:4].isdigit()
    ]
    if not candidates:
        return repo_root / "packaging" / "2026.07.03"
    return sorted(candidates, key=lambda path: path.name)[-1]


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def collect_files(root: Path, *, include_root_metadata: bool = False) -> list[dict[str, Any]]:
    if not root.exists():
        return []
    rows: list[dict[str, Any]] = []
    for path in sorted(item for item in root.rglob("*") if item.is_file()):
        rel = path.relative_to(root).as_posix()
        if not include_root_metadata and rel in ROOT_METADATA:
            continue
        rows.append({
            "path": rel,
            "bytes": path.stat().st_size,
            "sha256": sha256(path),
        })
    return rows


def read_lanes(repo_root: Path) -> list[str]:
    manifest = repo_root / MANIFEST_REL
    with manifest.open(newline="", encoding="utf-8-sig") as handle:
        rows = list(csv.DictReader(handle))
    lanes = [
        row["family_id"].strip()
        for row in rows
        if row.get("profile_class", "").strip() == "compatibility_emulation"
    ]
    if len(lanes) != 25:
        raise RuntimeError(f"expected 25 reference parser lanes, found {len(lanes)}")
    return sorted(lanes)


def copy_file(src: Path, dst: Path, verify_only: bool) -> bool:
    if not src.is_file():
        return False
    if verify_only:
        return dst.is_file() and dst.stat().st_size == src.stat().st_size
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)
    return True


def write_text(path: Path, content: str, verify_only: bool) -> bool:
    if verify_only:
        return path.is_file()
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")
    return True


def make_source_archive(repo_root: Path,
                        package_root: Path,
                        verify_only: bool,
                        issues: list[str]) -> str:
    rel_archive = "support/scratchbird-reference-parser-source.tar.gz"
    target = package_root / rel_archive
    if verify_only:
        if not target.is_file():
            issues.append(f"missing_source_archive:{rel_archive}")
        return rel_archive
    target.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(target, "w:gz") as archive:
        for rel in REFERENCE_SOURCE_RELS:
            source = repo_root / rel
            if not source.exists():
                issues.append(f"missing_source_path:{rel}")
                continue
            archive.add(source, arcname=rel)
    return rel_archive


def add_common_release_materials(repo_root: Path,
                                 package_root: Path,
                                 verify_only: bool,
                                 issues: list[str]) -> None:
    for filename in LEGAL_SOURCE_FILES:
        src = repo_root / filename
        dst_name = "LICENSE.txt" if filename == "LICENSE" else filename
        if not copy_file(src, package_root / "legal" / dst_name, verify_only):
            issues.append(f"missing_legal_material:{filename}")
    if not copy_file(repo_root / ROOT_SBOM_REL, package_root / "support" / "root-SBOM.json", verify_only):
        issues.append("missing_root_sbom_support_material")


def write_sha256s(package_root: Path, verify_only: bool) -> None:
    target = package_root / "SHA256SUMS"
    if verify_only:
        return
    rows = collect_files(package_root)
    target.write_text(
        "".join(f"{row['sha256']}  {row['path']}\n" for row in rows),
        encoding="utf-8",
    )


def write_package_sbom(package_root: Path, verify_only: bool) -> None:
    if verify_only:
        return
    components = [
        row for row in collect_files(package_root)
        if row["path"] not in {"SBOM.json", "SHA256SUMS"}
    ]
    payload = {
        "schema_id": "scratchbird.release_package_sbom.v1",
        "component_id": "reference-parsers:all",
        "name": "reference-parsers",
        "generated_at_utc": datetime.now(timezone.utc).replace(microsecond=0).isoformat(),
        "components": components,
    }
    (package_root / "SBOM.json").write_text(
        json.dumps(payload, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def update_root_manifests(repo_root: Path,
                          release_root: Path,
                          verify_only: bool,
                          issues: list[str]) -> None:
    if verify_only:
        if not (release_root / "FILE_LOCATION_MANIFEST.json").is_file():
            issues.append("file_location_manifest_missing")
        if not (release_root / "RELEASE_MANIFEST.json").is_file():
            issues.append("release_manifest_missing")
        return

    files = collect_files(release_root, include_root_metadata=True)
    files = [row for row in files if row["path"] != "FILE_LOCATION_MANIFEST.json"]
    file_manifest = {
        "schema_id": "scratchbird.prerelease_file_location_manifest.v1",
        "release_date": release_root.name,
        "generated_at_utc": datetime.now(timezone.utc).replace(microsecond=0).isoformat(),
        "files": [
            {
                "path": row["path"],
                "bytes": row["bytes"],
                "sha256": row["sha256"],
                "role": "reference_parser" if row["path"].startswith("reference-parsers/") else "payload",
                "installer_destination_hint": (
                    "reference parser package inputs"
                    if row["path"].startswith("reference-parsers/")
                    else "release package input"
                ),
            }
            for row in files
            if row["path"] not in ROOT_METADATA
        ],
    }
    (release_root / "FILE_LOCATION_MANIFEST.json").write_text(
        json.dumps(file_manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )

    artifacts = collect_files(release_root, include_root_metadata=True)
    artifacts = [row for row in artifacts if row["path"] != "RELEASE_MANIFEST.json"]
    promoted_paths = sorted(
        str(path.relative_to(repo_root))
        for path in release_root.iterdir()
        if path.is_dir()
    )
    categories: dict[str, int] = {}
    for row in artifacts:
        category = row["path"].split("/", 1)[0] if "/" in row["path"] else "metadata"
        categories[category] = categories.get(category, 0) + 1
    release_manifest = {
        "schema_id": "scratchbird.prerelease_packaging_manifest.v1",
        "release_date": release_root.name,
        "channel": "temporary_prerelease_packaging",
        "pre_release_not_final": True,
        "generated_at_utc": datetime.now(timezone.utc).replace(microsecond=0).isoformat(),
        "source": {
            "source_commit": git_text(repo_root, "rev-parse", "HEAD"),
            "source_branch": git_text(repo_root, "rev-parse", "--abbrev-ref", "HEAD"),
        },
        "policy": {
            "packaging_tree_is_temporary": True,
            "installer_builders_consume_file_location_manifest": True,
        },
        "promoted_paths": promoted_paths,
        "categories": categories,
        "artifacts": [
            {
                "path": row["path"],
                "bytes": row["bytes"],
                "sha256": row["sha256"],
                "category": row["path"].split("/", 1)[0] if "/" in row["path"] else "metadata",
            }
            for row in artifacts
        ],
    }
    (release_root / "RELEASE_MANIFEST.json").write_text(
        json.dumps(release_manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def promote_reference_parsers(repo_root: Path,
                              release_root: Path,
                              build_bin_root: Path,
                              verify_only: bool) -> dict[str, Any]:
    lanes = read_lanes(repo_root)
    package_root = release_root / "reference-parsers"
    issues: list[str] = []
    if not verify_only and package_root.exists():
        shutil.rmtree(package_root)
    package_root.mkdir(parents=True, exist_ok=True)

    payloads: list[str] = []
    for lane in lanes:
        binary = f"sbp_{lane}"
        if copy_file(build_bin_root / binary, package_root / "bin" / binary, verify_only):
            payloads.append(f"bin/{binary}")
        else:
            issues.append(f"missing_reference_parser_binary:{binary}")

    source_archive = make_source_archive(repo_root, package_root, verify_only, issues)
    write_text(
        package_root / "README.md",
        "# ScratchBird Reference Parser Package\n\n"
        "This package stages ScratchBird-owned reference parser worker binaries. "
        "The workers lower reference-system syntax outside the engine into "
        "ScratchBird SBLR/UUID envelopes or exact fail-closed diagnostics.\n",
        verify_only,
    )
    write_text(
        package_root / "proofs" / "reference_parser_packaging_handoff.json",
        json.dumps({
            "schema_id": "scratchbird.reference_parser_packaging_handoff.v1",
            "lane_count": len(lanes),
            "lanes": lanes,
            "parser_workers": [f"bin/sbp_{lane}" for lane in lanes],
            "support_udr_package": "../udr/optional-parser-support",
            "engine_authority": "scratchbird_sblr_uuid_only",
            "reference_engine_authority": False,
            "mga_transaction_authority": True,
        }, indent=2, sort_keys=True) + "\n",
        verify_only,
    )
    add_common_release_materials(repo_root, package_root, verify_only, issues)
    write_text(
        package_root / "package_manifest.json",
        json.dumps({
            "schema_id": "scratchbird.reference_parser_release_package_manifest.v1",
            "package_id": "reference-parsers:all",
            "component_type": "reference_parser_workers",
            "source_commit": git_text(repo_root, "rev-parse", "HEAD"),
            "payloads": sorted(payloads),
            "source_archive": source_archive,
            "proofs": ["proofs/reference_parser_packaging_handoff.json"],
            "support_udr_package": "../udr/optional-parser-support",
            "license": "legal/LICENSE.txt",
            "notice": "legal/NOTICE",
            "third_party_notices": "legal/THIRD_PARTY_NOTICES.md",
            "sbom": "SBOM.json",
        }, indent=2, sort_keys=True) + "\n",
        verify_only,
    )
    write_package_sbom(package_root, verify_only)
    write_sha256s(package_root, verify_only)
    update_root_manifests(repo_root, release_root, verify_only, issues)
    return {
        "command": "promote_reference_parser_release_artifacts.py",
        "status": "fail" if issues else "pass",
        "release_root": str(release_root.relative_to(repo_root)),
        "package_root": str(package_root.relative_to(repo_root)),
        "lane_count": len(lanes),
        "payload_count": len(payloads),
        "issues": issues,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, default=repo_root_from_script())
    parser.add_argument("--release-root", type=Path)
    parser.add_argument("--build-bin-root", type=Path, required=True)
    parser.add_argument("--verify-only", action="store_true")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    release_root = args.release_root
    if release_root is None:
        release_root = latest_release_dir(repo_root)
    elif not release_root.is_absolute():
        release_root = repo_root / release_root
    build_bin_root = args.build_bin_root
    if not build_bin_root.is_absolute():
        build_bin_root = repo_root / build_bin_root
    output = args.output or repo_root / "build" / "reports" / REPORT_NAME
    try:
        report = promote_reference_parsers(
            repo_root,
            release_root.resolve(),
            build_bin_root.resolve(),
            args.verify_only,
        )
    except (OSError, RuntimeError) as exc:
        print(f"failed: {exc}")
        return 1
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"reference_parser_packaging_promotion={report['status']}")
    return 0 if report["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
