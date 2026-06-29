#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Promote or verify beta driver release artifacts under packaging/.

The private tracker records when package proof has been accepted. This tool is
the proof producer: it validates concrete driver binaries and support files, and
in promotion mode writes the package payload. It must not require tracker rows
to be pre-marked complete.
"""

from __future__ import annotations

import argparse
import csv
from datetime import datetime, timezone
import hashlib
import json
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any, Iterable


REPORT_NAME = "driver_packaging_promotion.json"
MANIFEST_REL = Path("project/drivers/DriverPackageManifest.csv")
DEFAULT_BUILD_BIN_REL = Path("build/output/linux/bin")
DEFAULT_PROOF_REL = Path("build/reports")
DEFAULT_LANGUAGE_PACK_REL = Path(
    "project/resources/seed-packs/initial-resource-pack/resources/i18n/"
    "sbsql-language-resource-pack"
)
LEGAL_SOURCE_FILES = ("LICENSE", "NOTICE", "THIRD_PARTY_NOTICES.md")
ROOT_SBOM_REL = Path("SBOM.json")
STAGED_EXECUTABLES = {
    "cpp": "sb_isql_cpp",
    "dart": "sb_isql_dart",
    "dotnet": "sb_isql_dotnet",
    "elixir": "sb_isql_elixir",
    "go": "sb_isql_go",
    "jdbc": "sb_isql_jdbc",
    "mojo": "sb_isql_mojo",
    "node": "sb_isql_node",
    "odbc": "sb_isql_odbc",
    "pascal": "sb_isql_pascal",
    "php": "sb_isql_php",
    "python": "sb_isql_python",
    "r": "sb_isql_r",
    "ruby": "sb_isql_ruby",
    "rust": "sb_isql_rust",
    "swift": "sb_isql_swift",
    "adbc": "sb_isql_adbc",
    "flightsql": "sb_isql_flightsql",
    "julia": "sb_isql_julia",
    "perl": "sb_isql_perl",
    "r2dbc": "sb_isql_r2dbc",
}
OPTIONAL_SUPPORT_FILES = (
    "package_contract.json",
    "package.json",
    "pyproject.toml",
    "Project.toml",
    "Package.swift",
    "Package.resolved",
    "Makefile.PL",
    "pom.xml",
)
EXCLUDED_COPY_PARTS = {
    ".build",
    ".dart_tool",
    "__pycache__",
    "bin",
    "build",
    "dist",
    "node_modules",
    "obj",
    "target",
}
EXCLUDED_COPY_SUFFIXES = {".pyc", ".o", ".obj", ".class"}
ROOT_METADATA = {"RELEASE_MANIFEST.json", "SHA256SUMS"}


def repo_root_from_script() -> Path:
    return Path(__file__).resolve().parents[2]


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8-sig") as handle:
        return list(csv.DictReader(handle))


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


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
    packaging = repo_root / "packaging"
    candidates = [path for path in packaging.iterdir() if path.is_dir() and path.name[:4].isdigit()]
    if not candidates:
        return packaging / "2026.06.13"
    return sorted(candidates, key=lambda path: path.name)[-1]


def driver_rows(repo_root: Path) -> list[dict[str, str]]:
    return [row for row in read_csv(repo_root / MANIFEST_REL) if row.get("category") == "driver"]


def source_path(repo_root: Path, manifest: dict[str, str]) -> Path:
    rel = manifest.get("source_path", "").strip()
    return repo_root / rel


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


def should_copy(path: Path) -> bool:
    if any(part in EXCLUDED_COPY_PARTS for part in path.parts):
        return False
    if path.suffix in EXCLUDED_COPY_SUFFIXES:
        return False
    return True


def copy_tree(src: Path, dst: Path, verify_only: bool) -> bool:
    if not src.is_dir():
        return False
    files = [path for path in src.rglob("*") if path.is_file() and should_copy(path.relative_to(src))]
    if not files:
        return False
    if verify_only:
        return dst.is_dir() and any(path.is_file() for path in dst.rglob("*"))
    for item in files:
        rel = item.relative_to(src)
        target = dst / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(item, target)
    return True


def collect_files(root: Path, *, include_root_metadata: bool = False) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    if not root.exists():
        return rows
    for path in sorted(item for item in root.rglob("*") if item.is_file()):
        rel = path.relative_to(root).as_posix()
        if not include_root_metadata and rel in ROOT_METADATA:
            continue
        stat = path.stat()
        rows.append(
            {
                "path": rel,
                "bytes": stat.st_size,
                "sha256": sha256(path),
            }
        )
    return rows


def directory_digest(root: Path) -> str:
    digest = hashlib.sha256()
    for row in collect_files(root, include_root_metadata=True):
        digest.update(row["path"].encode("utf-8"))
        digest.update(b"\0")
        digest.update(row["sha256"].encode("ascii"))
        digest.update(b"\n")
    return digest.hexdigest()


def write_sha256s(root: Path, verify_only: bool) -> bool:
    target = root / "SHA256SUMS"
    if verify_only:
        return target.is_file()
    rows = collect_files(root)
    target.write_text(
        "".join(f"{row['sha256']}  {row['path']}\n" for row in rows),
        encoding="utf-8",
    )
    return True


def proof_sources(repo_root: Path) -> list[Path]:
    reports = repo_root / DEFAULT_PROOF_REL
    names = (
        "driver_complete_coverage_tests.json",
        "driver_complete_coverage_checklist.json",
        "driver_complete_delta_implementation.json",
        "driver_wiki_documentation.json",
        "driver_complete_coverage_tests_preflight_after_profile.json",
    )
    return [reports / name for name in names if (reports / name).is_file()]


def example_roots(driver_source: Path) -> list[tuple[str, Path]]:
    candidates = (
        ("tools", driver_source / "tools"),
        ("cmd", driver_source / "cmd"),
        ("src-tools", driver_source / "src" / "tools"),
        ("swift-sb-isql", driver_source / "Sources" / "SBIsqlSwift"),
        ("tests", driver_source / "tests"),
        ("test", driver_source / "test"),
        ("perl-tests", driver_source / "t"),
    )
    return [(name, path) for name, path in candidates if path.is_dir()]


def write_driver_sbom(driver_root: Path, driver: str, component_id: str, verify_only: bool) -> bool:
    target = driver_root / "SBOM.json"
    if verify_only:
        return target.is_file()
    components = [
        {
            "path": row["path"],
            "sha256": row["sha256"],
            "bytes": row["bytes"],
        }
        for row in collect_files(driver_root)
        if row["path"] not in {"SBOM.json", "SHA256SUMS"}
    ]
    payload = {
        "schema_id": "scratchbird.driver_package_sbom.v1",
        "component_id": component_id,
        "driver": driver,
        "generated_at_utc": datetime.now(timezone.utc).replace(microsecond=0).isoformat(),
        "components": components,
    }
    target.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return True


def write_release_manifest(
    repo_root: Path,
    release_root: Path,
    promoted_paths: Iterable[str],
    verify_only: bool,
) -> bool:
    manifest = release_root / "RELEASE_MANIFEST.json"
    sums = release_root / "SHA256SUMS"
    if verify_only:
        return manifest.is_file() and sums.is_file()
    files = collect_files(release_root)
    payload = {
        "schema_id": "scratchbird.prerelease_bundle_manifest.v1",
        "channel": "prerelease",
        "release_date": release_root.name,
        "generated_at_utc": datetime.now(timezone.utc).replace(microsecond=0).isoformat(),
        "pre_release_not_final": True,
        "source": {
            "commit": git_text(repo_root, "rev-parse", "HEAD"),
            "dirty_before_promotion": bool(git_text(repo_root, "status", "--porcelain")),
        },
        "policy": {
            "distribution": "private_pre_release",
            "official_release": False,
            "history_cleanup_required_before_public_release": True,
            "build_directory_is_disposable": True,
            "promotion_requires_explicit_command": True,
        },
        "categories": sorted({row["path"].split("/", 1)[0] for row in files if "/" in row["path"]}),
        "promoted_paths": sorted(set(promoted_paths)),
        "artifacts": [
            {
                "path": row["path"],
                "category": row["path"].split("/", 1)[0] if "/" in row["path"] else "metadata",
                "bytes": row["bytes"],
                "sha256": row["sha256"],
            }
            for row in files
        ],
    }
    manifest.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    sums.write_text(
        "".join(f"{row['sha256']}  {row['path']}\n" for row in files),
        encoding="utf-8",
    )
    return True


def build_driver_package(
    repo_root: Path,
    release_root: Path,
    manifest: dict[str, str],
    verify_only: bool,
) -> tuple[dict[str, Any], list[str]]:
    component = manifest.get("component_id", "").strip()
    name = manifest.get("name", "").strip()
    issues: list[str] = []
    driver_root = release_root / "drivers" / name
    src_root = source_path(repo_root, manifest)
    binary_name = STAGED_EXECUTABLES.get(name, f"sb_isql_{name}")
    binary_src = repo_root / DEFAULT_BUILD_BIN_REL / binary_name
    binary_dst = driver_root / "bin" / binary_name

    if not verify_only and driver_root.exists():
        shutil.rmtree(driver_root)

    if not copy_file(binary_src, binary_dst, verify_only):
        issues.append(f"missing_compiled_driver_binary:{binary_src.relative_to(repo_root)}")

    if not copy_file(src_root / "README.md", driver_root / "support" / "README.md", verify_only):
        issues.append("missing_driver_readme_support_material")
    if not copy_file(
        src_root / "BASELINE_REQUIREMENT_MAPPING.md",
        driver_root / "support" / "BASELINE_REQUIREMENT_MAPPING.md",
        verify_only,
    ):
        issues.append("missing_baseline_mapping_support_material")

    for filename in OPTIONAL_SUPPORT_FILES:
        copy_file(src_root / filename, driver_root / "support" / filename, verify_only)

    copied_example = False
    for label, path in example_roots(src_root):
        copied_example = copy_tree(path, driver_root / "examples" / label, verify_only) or copied_example
    example_readme = (
        f"# ScratchBird {name} driver example\n\n"
        f"The canonical executable example for this package is `bin/{binary_name}`. "
        "It accepts the shared sb_isql conformance command-line contract and shows "
        "connection setup, input/output/error routing, diagnostics, metrics, and "
        "server-parser execution for this driver.\n"
    )
    write_text(driver_root / "examples" / "README.md", example_readme, verify_only)
    if verify_only and not (driver_root / "examples" / "README.md").is_file():
        issues.append("missing_driver_example_readme")
    if not copied_example and not verify_only:
        # Some language packages expose their tool through package-native source
        # roots rather than a tools/ directory. The generated README remains the
        # reproducible example anchor for those packages.
        pass

    language_pack_src = repo_root / DEFAULT_LANGUAGE_PACK_REL
    if not copy_tree(language_pack_src, driver_root / "resources" / "sbsql-language-resource-pack", verify_only):
        issues.append(f"missing_language_resource_pack:{DEFAULT_LANGUAGE_PACK_REL.as_posix()}")

    for filename in LEGAL_SOURCE_FILES:
        src = repo_root / filename
        dst_name = "LICENSE.txt" if filename == "LICENSE" else filename
        if not copy_file(src, driver_root / "legal" / dst_name, verify_only):
            issues.append(f"missing_legal_material:{filename}")
        copy_file(src, driver_root / "support" / dst_name, verify_only)

    if not copy_file(repo_root / ROOT_SBOM_REL, driver_root / "support" / "root-SBOM.json", verify_only):
        issues.append("missing_root_sbom_support_material")

    proof_files = proof_sources(repo_root)
    for proof in proof_files:
        copy_file(proof, driver_root / "proofs" / proof.name, verify_only)
    if not proof_files and not verify_only:
        issues.append("missing_driver_proof_summary_source")
    if verify_only and not (driver_root / "proofs").is_dir():
        issues.append("missing_driver_proofs_directory")

    proof_summary_path = driver_root / "proofs" / "proof_summary.json"
    proof_summary = {
        "schema_id": "scratchbird.driver_package_proof_summary.v1",
        "component_id": component,
        "driver": name,
        "proof_files": sorted(path.name for path in proof_files),
        "coverage_proof_status": None,
    }
    coverage = repo_root / DEFAULT_PROOF_REL / "driver_complete_coverage_tests.json"
    if coverage.is_file():
        try:
            proof_summary["coverage_proof_status"] = json.loads(
                coverage.read_text(encoding="utf-8")
            ).get("status")
        except json.JSONDecodeError:
            proof_summary["coverage_proof_status"] = "invalid_json"
    write_text(
        proof_summary_path,
        json.dumps(proof_summary, indent=2, sort_keys=True) + "\n",
        verify_only,
    )
    if verify_only and not proof_summary_path.is_file():
        issues.append("missing_driver_proof_summary")

    package_manifest_path = driver_root / "package_manifest.json"
    binary_hash = sha256(binary_src) if binary_src.is_file() else ""
    language_digest = directory_digest(language_pack_src) if language_pack_src.is_dir() else ""
    package_manifest = {
        "schema_id": "scratchbird.driver_release_package_manifest.v1",
        "component_id": component,
        "driver": name,
        "source_path": manifest.get("source_path", ""),
        "version": manifest.get("driver_package_uuid", ""),
        "source_commit": git_text(repo_root, "rev-parse", "HEAD"),
        "binary": f"bin/{binary_name}",
        "binary_sha256": binary_hash,
        "language_resource_pack": "resources/sbsql-language-resource-pack",
        "language_resource_pack_sha256": language_digest,
        "support_materials": [
            "support/README.md",
            "support/BASELINE_REQUIREMENT_MAPPING.md",
            "support/root-SBOM.json",
        ],
        "examples": ["examples/README.md"],
        "proofs": ["proofs/proof_summary.json"],
        "license": "legal/LICENSE.txt",
        "notice": "legal/NOTICE",
        "third_party_notices": "legal/THIRD_PARTY_NOTICES.md",
        "sbom": "SBOM.json",
    }
    if not write_text(
        package_manifest_path,
        json.dumps(package_manifest, indent=2, sort_keys=True) + "\n",
        verify_only,
    ):
        issues.append("missing_package_manifest_json")
    support_bundle_manifest = {
        "schema_id": "scratchbird.driver_support_bundle_manifest.v1",
        "component_id": component,
        "driver": name,
        "hash": binary_hash,
        "SBOM": "root-SBOM.json",
        "license": "LICENSE.txt",
        "resource_pack_digest": language_digest,
        "support_bundle_schema": "scratchbird.driver_support_bundle_manifest.v1",
        "proof_summary": "../proofs/proof_summary.json",
    }
    write_text(
        driver_root / "support" / "support_bundle_manifest.json",
        json.dumps(support_bundle_manifest, indent=2, sort_keys=True) + "\n",
        verify_only,
    )

    if not write_driver_sbom(driver_root, name, component, verify_only):
        issues.append("missing_driver_package_sbom")
    if not write_sha256s(driver_root, verify_only):
        issues.append("missing_driver_sha256s")

    if verify_only and package_manifest_path.is_file():
        try:
            existing = json.loads(package_manifest_path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            issues.append("package_manifest_invalid_json")
            existing = {}
        for required in (
            "component_id",
            "driver",
            "version",
            "source_commit",
            "binary_sha256",
            "language_resource_pack_sha256",
            "license",
            "sbom",
        ):
            if not existing.get(required):
                issues.append(f"package_manifest_missing_{required}")
    for required_rel in (
        "SBOM.json",
        "SHA256SUMS",
        "package_manifest.json",
        f"bin/{binary_name}",
        "examples/README.md",
        "proofs/proof_summary.json",
        "resources/sbsql-language-resource-pack/manifest.sblrp.json",
        "legal/LICENSE.txt",
    ):
        if verify_only and not (driver_root / required_rel).is_file():
            issues.append(f"missing_packaged_file:{required_rel}")

    return (
        {
            "component_id": component,
            "driver": name,
            "release_path": str(driver_root.relative_to(repo_root)),
            "binary_source": str(binary_src.relative_to(repo_root)),
            "binary_sha256": binary_hash,
            "language_resource_pack_sha256": language_digest,
            "issues": issues,
        },
        issues,
    )


def build_report(
    repo_root: Path,
    matrix_path: Path,
    release_root: Path,
    verify_only: bool,
) -> dict[str, Any]:
    matrix_rows = {
        row.get("component_id", "").strip(): row
        for row in read_csv(matrix_path)
        if row.get("component_id", "").strip()
    }
    issues: list[str] = []
    promoted: list[dict[str, Any]] = []
    promoted_paths: list[str] = []
    for manifest in driver_rows(repo_root):
        component = manifest.get("component_id", "").strip()
        if component not in matrix_rows:
            issues.append(f"{component}:missing_complete_coverage_matrix_row")
        package, row_issues = build_driver_package(repo_root, release_root, manifest, verify_only)
        issues.extend(f"{component}:{issue}" for issue in row_issues)
        promoted.append(package)
        promoted_paths.append(package["release_path"])

    if not write_release_manifest(repo_root, release_root, promoted_paths, verify_only):
        issues.append("release_manifest_or_sha256s_missing")
    if verify_only:
        try:
            manifest = json.loads((release_root / "RELEASE_MANIFEST.json").read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            issues.append("release_manifest_invalid_or_missing")
            manifest = {}
        promoted_set = set(manifest.get("promoted_paths", [])) if isinstance(manifest, dict) else set()
        for package in promoted:
            if package["release_path"] not in promoted_set:
                issues.append(f"{package['component_id']}:release_manifest_missing_promoted_path")

    return {
        "command": "promote_driver_release_artifacts.py",
        "gate_id": "BETA-DTA-GATE-036",
        "status": "fail" if issues else "pass",
        "summary": {
            "verify_only": verify_only,
            "release_root": str(release_root.relative_to(repo_root)),
            "drivers": len(promoted),
            "issues": len(issues),
        },
        "drivers": promoted,
        "issues": issues,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, default=repo_root_from_script())
    parser.add_argument("--matrix", type=Path, required=True)
    parser.add_argument("--release-root", type=Path)
    parser.add_argument("--verify-only", action="store_true")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    release_root = args.release_root
    if release_root is None:
        release_root = latest_release_dir(repo_root)
    elif not release_root.is_absolute():
        release_root = repo_root / release_root
    output = args.output or repo_root / "build" / "reports" / REPORT_NAME
    try:
        report = build_report(
            repo_root,
            args.matrix.expanduser().resolve(),
            release_root.resolve(),
            args.verify_only,
        )
    except (OSError, ValueError) as exc:
        print(f"failed: {exc}", file=sys.stderr)
        return 1
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"driver_packaging_promotion={report['status']}")
    return 0 if report["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
