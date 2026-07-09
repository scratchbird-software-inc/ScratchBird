#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Stage reference parser release artifacts outside the source tree."""

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


MANIFEST_REL = Path("project/src/parsers/compatibility/CompatibilityProfileManifest.csv")
LEGAL_SOURCE_FILES = ("LICENSE", "NOTICE", "THIRD_PARTY_NOTICES.md")
ROOT_SBOM_REL = Path("SBOM.json")


def repo_root_from_script() -> Path:
    return Path(__file__).resolve().parents[3]


def as_display_path(path: Path, root: Path) -> str:
    try:
        return path.relative_to(root).as_posix()
    except ValueError:
        return path.as_posix()


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def git_text(repo_root: Path, *args: str) -> str:
    result = subprocess.run(
        ["git", *args],
        cwd=repo_root,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    return result.stdout.strip() if result.returncode == 0 else ""


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


def collect_files(root: Path) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    if not root.exists():
        return rows
    for path in sorted(item for item in root.rglob("*") if item.is_file()):
        rel = path.relative_to(root).as_posix()
        rows.append({"path": rel, "bytes": path.stat().st_size, "sha256": sha256(path)})
    return rows


def copy_required(src: Path, dst: Path, issues: list[str], label: str) -> bool:
    if not src.is_file():
        issues.append(f"missing_{label}:{src}")
        return False
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)
    return True


def find_support_library(build_root: Path, lane: str) -> Path | None:
    base = f"sbu_{lane}_parser_support"
    candidates = [
        f"lib{base}.a",
        f"{base}.lib",
        f"lib{base}.dylib",
        f"{base}.dll",
    ]
    for name in candidates:
        matches = sorted(path for path in build_root.rglob(name) if path.is_file())
        if matches:
            return matches[0]
    return None


def source_basis(repo_root: Path) -> dict[str, Any]:
    return {
        "source_commit_basis": git_text(repo_root, "rev-parse", "HEAD"),
        "source_branch_basis": git_text(repo_root, "rev-parse", "--abbrev-ref", "HEAD"),
        "source_basis_semantics": (
            "Source commit identifies the implementation checkout used to stage "
            "this generated release package; generated package payloads remain "
            "build artifacts and are not source-controlled."
        ),
    }


def stage_release(repo_root: Path,
                  release_root: Path,
                  build_root: Path,
                  build_bin_root: Path) -> dict[str, Any]:
    lanes = read_lanes(repo_root)
    package_root = release_root / "reference-parsers"
    support_root = release_root / "udr" / "optional-parser-support" / "lib"
    issues: list[str] = []

    if package_root.exists():
        shutil.rmtree(package_root)
    if support_root.exists():
        shutil.rmtree(support_root)
    package_root.mkdir(parents=True, exist_ok=True)
    support_root.mkdir(parents=True, exist_ok=True)

    parser_payloads: list[str] = []
    support_payloads: list[str] = []
    for lane in lanes:
        parser_name = f"sbp_{lane}"
        parser_src = build_bin_root / parser_name
        if not parser_src.is_file() and (build_bin_root / f"{parser_name}.exe").is_file():
            parser_name = f"{parser_name}.exe"
            parser_src = build_bin_root / parser_name
        if copy_required(parser_src, package_root / "bin" / parser_name, issues, "parser_worker"):
            parser_payloads.append(f"bin/{parser_name}")

        support = find_support_library(build_root, lane)
        support_name = f"libsbu_{lane}_parser_support.a"
        if support is None:
            issues.append(f"missing_parser_support_udr:{lane}")
        elif copy_required(support, support_root / support_name, issues, "parser_support_udr"):
            support_payloads.append(f"../udr/optional-parser-support/lib/{support_name}")

    for filename in LEGAL_SOURCE_FILES:
        dst_name = "LICENSE.txt" if filename == "LICENSE" else filename
        copy_required(repo_root / filename, package_root / "legal" / dst_name, issues, "legal_material")
    copy_required(repo_root / ROOT_SBOM_REL, package_root / "support" / "root-SBOM.json", issues, "root_sbom")

    generated_at = datetime.now(timezone.utc).replace(microsecond=0).isoformat()
    basis = source_basis(repo_root)
    (package_root / "README.md").write_text(
        "# ScratchBird Reference Parser Package\n\n"
        "Generated ScratchBird-owned compatibility parser worker package. "
        "Reference-system regression assets are acquired separately and are not "
        "packaged in this release payload.\n",
        encoding="utf-8",
    )
    (package_root / "proofs").mkdir(parents=True, exist_ok=True)
    (package_root / "proofs" / "reference_parser_packaging_handoff.json").write_text(
        json.dumps({
            "schema_id": "scratchbird.reference_parser_packaging_handoff.v1",
            "generated_at_utc": generated_at,
            "lane_count": len(lanes),
            "lanes": lanes,
            "parser_workers": parser_payloads,
            "support_udr_payloads": support_payloads,
            "engine_authority": "scratchbird_sblr_uuid_only",
            "reference_engine_authority": False,
            "mga_transaction_authority": True,
        }, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    (package_root / "package_manifest.json").write_text(
        json.dumps({
            "schema_id": "scratchbird.reference_parser_release_package_manifest.v1",
            "package_id": "reference-parsers:all",
            "component_type": "reference_parser_workers",
            "generated_at_utc": generated_at,
            **basis,
            "payloads": sorted(parser_payloads),
            "support_udr_payloads": sorted(support_payloads),
            "proofs": ["proofs/reference_parser_packaging_handoff.json"],
            "raw_reference_payloads_packaged": False,
        }, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )

    package_components = [
        row for row in collect_files(package_root)
        if row["path"] not in {"SBOM.json", "SHA256SUMS"}
    ]
    (package_root / "SBOM.json").write_text(
        json.dumps({
            "schema_id": "scratchbird.release_package_sbom.v1",
            "component_id": "reference-parsers:all",
            "generated_at_utc": generated_at,
            "components": package_components,
        }, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    package_rows = [
        row for row in collect_files(package_root)
        if row["path"] != "SHA256SUMS"
    ]
    (package_root / "SHA256SUMS").write_text(
        "".join(f"{row['sha256']}  {row['path']}\n" for row in package_rows),
        encoding="utf-8",
    )

    release_rows = [
        row for row in collect_files(release_root)
        if row["path"] not in {"FILE_LOCATION_MANIFEST.json", "RELEASE_MANIFEST.json"}
    ]
    (release_root / "FILE_LOCATION_MANIFEST.json").write_text(
        json.dumps({
            "schema_id": "scratchbird.prerelease_file_location_manifest.v1",
            "generated_at_utc": generated_at,
            "release_root": as_display_path(release_root, repo_root),
            "files": release_rows,
        }, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    (release_root / "RELEASE_MANIFEST.json").write_text(
        json.dumps({
            "schema_id": "scratchbird.prerelease_packaging_manifest.v1",
            "generated_at_utc": generated_at,
            "pre_release_not_final": True,
            **basis,
            "artifact_count": len(release_rows),
            "artifacts": release_rows,
        }, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return {
        "schema_id": "scratchbird.reference_parser_release_artifact_promotion.v1",
        "status": "fail" if issues else "pass",
        "release_root": as_display_path(release_root, repo_root),
        "lane_count": len(lanes),
        "parser_payload_count": len(parser_payloads),
        "support_udr_payload_count": len(support_payloads),
        "issues": issues,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, default=repo_root_from_script())
    parser.add_argument("--release-root", type=Path, required=True)
    parser.add_argument("--build-root", type=Path, required=True)
    parser.add_argument("--build-bin-root", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    release_root = args.release_root if args.release_root.is_absolute() else repo_root / args.release_root
    build_root = args.build_root if args.build_root.is_absolute() else repo_root / args.build_root
    build_bin_root = args.build_bin_root if args.build_bin_root.is_absolute() else repo_root / args.build_bin_root
    try:
        report = stage_release(
            repo_root,
            release_root.resolve(),
            build_root.resolve(),
            build_bin_root.resolve(),
        )
    except (OSError, RuntimeError) as exc:
        print(f"reference_parser_packaging_promotion=fail: {exc}")
        return 1
    output = args.output or build_root / "reports" / "reference_parser_packaging_promotion.json"
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"reference_parser_packaging_promotion={report['status']}")
    return 0 if report["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
