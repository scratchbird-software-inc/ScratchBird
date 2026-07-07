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
from pathlib import Path
from typing import Any


REPORT_NAME = "reference_parser_packaging_promotion.json"
MANIFEST_REL = Path("project/src/parsers/compatibility/CompatibilityProfileManifest.csv")
LEGAL_SOURCE_FILES = ("LICENSE", "NOTICE", "THIRD_PARTY_NOTICES.md")
ROOT_SBOM_REL = Path("SBOM.json")
ROOT_METADATA = {"RELEASE_MANIFEST.json", "SHA256SUMS"}
REFERENCE_SOURCE_RELS = (
    "packaging/scripts/promote_reference_parser_release_artifacts.py",
    "project/src/parsers/compatibility",
    "project/src/udr",
    "project/tests/compatibility_sql_parser_first_tranche",
    "project/tests/reference_regression",
    "public_execution_plan",
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


def collect_source_basis(repo_root: Path) -> dict[str, Any]:
    digest = hashlib.sha256()
    rows: list[dict[str, Any]] = []
    result = subprocess.run(
        ["git", "ls-files", *REFERENCE_SOURCE_RELS],
        cwd=repo_root,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    tracked_files = sorted(line for line in result.stdout.splitlines() if line)
    for item_rel in tracked_files:
        path = repo_root / item_rel
        if not path.is_file():
            continue
        item_sha = sha256(path)
        size = path.stat().st_size
        rows.append({
            "path": item_rel,
            "bytes": size,
            "sha256": item_sha,
        })
        digest.update(item_rel.encode("utf-8"))
        digest.update(b"\0")
        digest.update(str(size).encode("ascii"))
        digest.update(b"\0")
        digest.update(item_sha.encode("ascii"))
        digest.update(b"\n")
    return {
        "source_commit_basis": git_text(repo_root, "rev-parse", "HEAD"),
        "source_branch_basis": git_text(repo_root, "rev-parse", "--abbrev-ref", "HEAD"),
        "source_tree_digest_algorithm": "sha256(path\\0bytes\\0sha256\\n)",
        "source_tree_digest": digest.hexdigest(),
        "source_file_count": len(rows),
        "source_roots": list(REFERENCE_SOURCE_RELS),
    }


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


def write_source_reference_manifest(repo_root: Path,
                                    package_root: Path,
                                    verify_only: bool,
                                    issues: list[str]) -> str:
    rel_manifest = "support/reference_parser_source_reference_manifest.json"
    target = package_root / rel_manifest
    if verify_only:
        if not target.is_file():
            issues.append(f"missing_source_reference_manifest:{rel_manifest}")
        return rel_manifest

    source_rows: list[dict[str, Any]] = []
    for rel in REFERENCE_SOURCE_RELS:
        source = repo_root / rel
        if not source.exists():
            issues.append(f"missing_source_path:{rel}")
            continue
        source_rows.append({
            "path": rel,
            "kind": "public_scratchbird_source_or_test_contract",
        })
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(
        json.dumps({
            "schema_id": "scratchbird.reference_parser_source_reference_manifest.v1",
            "purpose": "source locations and acquisition contracts for reference parser packaging",
            "packaged_source_archive": False,
            "raw_reference_payloads_packaged": False,
            "public_source_paths": source_rows,
            "reference_test_acquisition": {
                "source_map": "project/tests/reference_regression/reference_regression_acquisition_sources.csv",
                "acquisition_tool": "project/tests/reference_regression/acquire_reference_regression_assets.py",
                "policy": "downloaded upstream payloads and locally built original tools are local test inputs, not packaged public source artifacts",
            },
        }, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return rel_manifest


def verify_source_basis(path: Path, source_basis: dict[str, Any], issues: list[str]) -> None:
    if not path.is_file():
        issues.append(f"missing_source_basis_manifest:{path.name}")
        return
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        issues.append(f"invalid_source_basis_manifest:{path.name}:{exc}")
        return
    if "source_commit" in payload or (
        isinstance(payload.get("source"), dict) and "source_commit" in payload["source"]
    ):
        issues.append(f"legacy_source_commit_field:{path.name}")
    container = payload.get("source") if isinstance(payload.get("source"), dict) else payload
    source_commit_basis = container.get("source_commit_basis")
    if not (
        isinstance(source_commit_basis, str)
        and len(source_commit_basis) == 40
        and all(char in "0123456789abcdef" for char in source_commit_basis)
    ):
        issues.append(f"invalid_source_basis_commit:{path.name}")
    for key in ("source_tree_digest", "source_tree_digest_algorithm", "source_file_count"):
        if container.get(key) != source_basis.get(key):
            issues.append(f"source_basis_mismatch:{path.name}:{key}")


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
        else:
            verify_source_basis(
                release_root / "RELEASE_MANIFEST.json",
                collect_source_basis(repo_root),
                issues,
            )
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
    source_basis = collect_source_basis(repo_root)
    release_manifest = {
        "schema_id": "scratchbird.prerelease_packaging_manifest.v1",
        "release_date": release_root.name,
        "channel": "temporary_prerelease_packaging",
        "pre_release_not_final": True,
        "generated_at_utc": datetime.now(timezone.utc).replace(microsecond=0).isoformat(),
        "source": {
            **source_basis,
            "source_basis_semantics": (
                "source_commit_basis is the checked-out implementation revision used "
                "when staging this temporary packaging tree; source_tree_digest is the "
                "release-package implementation/test contract identity and is stable "
                "across later packaging-only commits."
            ),
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
    source_basis = collect_source_basis(repo_root)
    if verify_only:
        verify_source_basis(package_root / "package_manifest.json", source_basis, issues)

    payloads: list[str] = []
    for lane in lanes:
        binary = f"sbp_{lane}"
        if copy_file(build_bin_root / binary, package_root / "bin" / binary, verify_only):
            payloads.append(f"bin/{binary}")
        else:
            issues.append(f"missing_reference_parser_binary:{binary}")

    source_reference_manifest = write_source_reference_manifest(
        repo_root, package_root, verify_only, issues
    )
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
            **source_basis,
            "source_basis_semantics": (
                "source_commit_basis is the checked-out implementation revision used "
                "when staging this temporary package; source_tree_digest is the "
                "verifiable parser implementation/test surface identity."
            ),
            "payloads": sorted(payloads),
            "source_reference_manifest": source_reference_manifest,
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
