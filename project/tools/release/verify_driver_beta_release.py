#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Verify beta driver release output inclusion and DBeaver exclusion."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any

from driver_release_common import (
    DBEAVER_COMPONENT_ID,
    add_common_args,
    dbeaver_output_hits,
    default_report_path,
    fail,
    in_scope_manifest_rows,
    load_manifest,
    load_workplan_csv,
    map_expected_output,
    report_status,
    resolve_repo_root,
    resolve_workplan_root,
    unique_index,
    write_report,
)


REPORT_NAME = "driver_beta_release_verify.json"


def read_json(path: Path) -> tuple[dict[str, Any], str | None]:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except OSError as exc:
        return {}, str(exc)
    except json.JSONDecodeError as exc:
        return {}, f"invalid_json:{exc.msg}"
    if not isinstance(data, dict):
        return {}, "json_root_not_object"
    return data, None


def sha256(path: Path) -> str:
    import hashlib

    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def driver_package_root(path: Path, driver: str) -> Path:
    parts = path.parts
    for index in range(0, len(parts) - 1):
        if parts[index] == "drivers" and parts[index + 1] == driver:
            return Path(*parts[: index + 2])
    return path


def validate_sha256s(root: Path, issues: list[str], context: str) -> None:
    sums = root / "SHA256SUMS"
    if not sums.is_file():
        issues.append(f"release_output:{context}:missing_packaged_file:SHA256SUMS")
        return
    try:
        lines = sums.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        issues.append(f"release_output:{context}:sha256s_unreadable:{exc}")
        return
    for line_number, line in enumerate(lines, 1):
        if not line.strip():
            continue
        parts = line.split(None, 1)
        if len(parts) != 2:
            issues.append(f"release_output:{context}:sha256s_malformed_line:{line_number}")
            continue
        expected_hash, rel = parts
        rel = rel.strip()
        target = root / rel
        if not target.is_file():
            issues.append(f"release_output:{context}:sha256s_missing_file:{rel}")
            continue
        if sha256(target) != expected_hash:
            issues.append(f"release_output:{context}:sha256_mismatch:{rel}")


def validate_driver_package(
    issues: list[str],
    package_root: Path,
    component: str,
    driver: str,
    expected_component: str | None = None,
) -> None:
    if not package_root.is_dir():
        issues.append(f"release_output:missing_expected_output:{component}:{package_root}")
        return

    package_manifest_path = package_root / "package_manifest.json"
    package_manifest, manifest_error = read_json(package_manifest_path)
    if manifest_error:
        issues.append(f"release_output:{component}:package_manifest:{manifest_error}")
        package_manifest = {}

    binary_rel = str(package_manifest.get("binary") or f"bin/sb_isql_{driver}")
    binary_path = package_root / binary_rel
    required_files = (
        binary_rel,
        "support/README.md",
        "support/BASELINE_REQUIREMENT_MAPPING.md",
        "support/root-SBOM.json",
        "support/support_bundle_manifest.json",
        "proofs/proof_summary.json",
        "package_manifest.json",
        "SBOM.json",
        "SHA256SUMS",
        "resources/sbsql-language-resource-pack/manifest.sblrp.json",
        "resources/sbsql-language-resource-pack/hashes.sha256",
        "legal/LICENSE.txt",
    )
    for rel in required_files:
        if not (package_root / rel).is_file():
            issues.append(f"release_output:{component}:missing_packaged_file:{rel}")
    if binary_path.is_file():
        if binary_path.stat().st_size <= 0:
            issues.append(f"release_output:{component}:empty_binary:{binary_rel}")
        if binary_path.stat().st_mode & 0o111 == 0:
            issues.append(f"release_output:{component}:binary_not_executable:{binary_rel}")

    if package_manifest:
        for field in (
            "binary",
            "binary_sha256",
            "component_id",
            "driver",
            "language_resource_pack",
            "language_resource_pack_sha256",
            "license",
            "proofs",
            "sbom",
            "source_commit",
            "support_materials",
            "version",
        ):
            if not package_manifest.get(field):
                issues.append(f"release_output:{component}:package_manifest_missing:{field}")
        expected_component = expected_component or component
        if package_manifest.get("component_id") != expected_component:
            issues.append(f"release_output:{component}:package_manifest_component_mismatch")
        if package_manifest.get("driver") != driver:
            issues.append(f"release_output:{component}:package_manifest_driver_mismatch")
        if binary_path.is_file() and package_manifest.get("binary_sha256") != sha256(binary_path):
            issues.append(f"release_output:{component}:package_manifest_binary_sha256_mismatch")

    support_manifest, support_error = read_json(package_root / "support" / "support_bundle_manifest.json")
    if support_error:
        issues.append(f"release_output:{component}:support_bundle_manifest:{support_error}")
    else:
        for field in ("hash", "resource_pack_digest", "support_bundle_schema", "proof_summary"):
            if not support_manifest.get(field):
                issues.append(f"release_output:{component}:support_bundle_missing:{field}")

    package_contract = package_root / "support" / "package_contract.json"
    if package_contract.exists() and read_json(package_contract)[1]:
        issues.append(f"release_output:{component}:package_contract_invalid_json")

    validate_sha256s(package_root, issues, component)


def validate_release_metadata(release_root: Path, repo_root: Path, issues: list[str]) -> None:
    manifest_path = release_root / "RELEASE_MANIFEST.json"
    sums_path = release_root / "SHA256SUMS"
    if not manifest_path.is_file():
        issues.append("release_output:release_metadata:missing_RELEASE_MANIFEST")
        return
    if not sums_path.is_file():
        issues.append("release_output:release_metadata:missing_SHA256SUMS")
        return
    manifest, error = read_json(manifest_path)
    if error:
        issues.append(f"release_output:release_metadata:RELEASE_MANIFEST:{error}")
        return
    promoted_paths = set(manifest.get("promoted_paths", []))
    driver_root = release_root / "drivers"
    driver_paths = {
        str(path.relative_to(repo_root))
        for path in driver_root.iterdir()
        if path.is_dir()
    } if driver_root.is_dir() else set()
    for rel in sorted(driver_paths - promoted_paths):
        issues.append(f"release_output:release_metadata:missing_promoted_path:{rel}")
    validate_sha256s(release_root, issues, "release_metadata")


def validate_packaging_scripts(
    issues: list[str],
    repo_root: Path,
    release_root: Path,
    row: dict[str, str],
    output: Path | None,
) -> None:
    artifact_id = row.get("artifact_id", "release:driver-packaging-scripts")
    if output is None or not output.is_dir():
        issues.append(
            "release_output:missing_expected_output:"
            f"{artifact_id}:{row.get('expected_public_output', '')}"
        )
        return
    for rel in (
        "packaging/scripts/promote_driver_release_artifacts.py",
        "project/tools/release/driver_release_artifact_manifest_gate.py",
        "project/tools/release/verify_driver_beta_release.py",
    ):
        path = repo_root / rel
        if not path.is_file():
            issues.append(f"release_output:{artifact_id}:missing_script:{rel}")
        elif path.stat().st_size <= 0:
            issues.append(f"release_output:{artifact_id}:empty_script:{rel}")
    validate_release_metadata(release_root, repo_root, issues)


def rows_by_component(rows: list[dict[str, str]]) -> dict[str, list[dict[str, str]]]:
    grouped: dict[str, list[dict[str, str]]] = {}
    for row in rows:
        component = row.get("component_id", "").strip()
        if component:
            grouped.setdefault(component, []).append(row)
    return grouped


def artifact_id_issues(rows: list[dict[str, str]]) -> list[str]:
    seen: set[str] = set()
    issues: list[str] = []
    for row in rows:
        artifact_id = row.get("artifact_id", "").strip()
        if not artifact_id:
            issues.append("RELEASE_ARTIFACT_MANIFEST_MATRIX:empty_artifact_id")
            continue
        if artifact_id in seen:
            issues.append(f"RELEASE_ARTIFACT_MANIFEST_MATRIX:duplicate_artifact_id:{artifact_id}")
        seen.add(artifact_id)
    return issues


def expected_path(
    expected: str,
    output_root: Path,
    repo_root: Path,
    *,
    driver: str = "",
) -> Path | None:
    expected = expected.replace("<driver>", driver).replace("<release>", output_root.name)
    return map_expected_output(expected, output_root, repo_root)


def build_report(repo_root: Path, workplan_root: Path, output_root: Path) -> dict[str, Any]:
    manifest_rows = load_manifest(repo_root)
    artifact_rows = load_workplan_csv(workplan_root, "RELEASE_ARTIFACT_MANIFEST_MATRIX.csv")
    manifest_by_id, manifest_issues = unique_index(
        manifest_rows, "component_id", "DriverPackageManifest"
    )
    artifact_by_component = rows_by_component(artifact_rows)
    issues = list(manifest_issues) + artifact_id_issues(artifact_rows)

    expected_components = {
        row["component_id"]
        for row in in_scope_manifest_rows(manifest_rows)
        if row.get("category", "").strip() == "driver"
    }
    driver_names = {
        row["component_id"]: row.get("name", "").strip()
        for row in in_scope_manifest_rows(manifest_rows)
        if row.get("category", "").strip() == "driver"
    }
    artifact_components = {
        component
        for component, rows in artifact_by_component.items()
        if component in expected_components
        and any(row.get("release_scope", "").strip() == "in_scope_required" for row in rows)
    }
    non_driver_artifact_components = {
        component
        for component, rows in artifact_by_component.items()
        if component not in {"drivers:all", "release:driver-packaging-scripts"}
        and any(row.get("release_scope", "").strip() == "in_scope_required" for row in rows)
    }
    for component in sorted(expected_components - artifact_components):
        issues.append(f"release_output:missing_artifact_row:{component}")
    for component in sorted(non_driver_artifact_components - expected_components - set(manifest_by_id)):
        issues.append(f"release_output:unexpected_artifact_row:{component}")

    missing_outputs: list[str] = []
    for component in sorted(expected_components):
        rows = artifact_by_component.get(component, [])
        row = rows[0] if rows else None
        if row is None:
            continue
        expected = row.get("expected_public_output", "")
        output = expected_path(expected, output_root, repo_root, driver=driver_names.get(component, ""))
        if output is None:
            issues.append(f"release_output:{component}:expected_output_none")
            continue
        if not output.exists():
            missing_outputs.append(f"{component}:{expected}")
        else:
            validate_driver_package(issues, output, component, driver_names.get(component, ""))
    issues.extend(f"release_output:missing_expected_output:{item}" for item in missing_outputs)

    for row in artifact_by_component.get("drivers:all", []):
        for component, driver in sorted(driver_names.items()):
            output = expected_path(
                row.get("expected_public_output", ""),
                output_root,
                repo_root,
                driver=driver,
            )
            if output is None:
                issues.append(f"release_output:{row.get('artifact_id', 'drivers:all')}:{component}:expected_output_none")
            elif not output.exists():
                issues.append(
                    f"release_output:missing_expected_output:"
                    f"{row.get('artifact_id', 'drivers:all')}:{component}:"
                    f"{row.get('expected_public_output', '')}"
                )
            else:
                package_root = driver_package_root(output, driver)
                if package_root != output and package_root.is_dir():
                    validate_driver_package(
                        issues,
                        package_root,
                        f"{row.get('artifact_id', 'drivers:all')}:{component}",
                        driver,
                        component,
                    )

    for row in artifact_by_component.get("release:driver-packaging-scripts", []):
        output = expected_path(row.get("expected_public_output", ""), output_root, repo_root)
        validate_packaging_scripts(issues, repo_root, output_root, row, output)

    dbeaver_rows = artifact_by_component.get(DBEAVER_COMPONENT_ID, [])
    dbeaver_row = dbeaver_rows[0] if dbeaver_rows else None
    if dbeaver_row is None:
        issues.append("release_output:dbeaver_exclusion_row_missing")
    else:
        if dbeaver_row.get("release_scope", "").strip() != "separate_controller":
            issues.append("release_output:dbeaver_release_scope_not_separate_controller")
        if dbeaver_row.get("expected_public_output", "").strip().lower() != "none":
            issues.append("release_output:dbeaver_expected_output_not_none")

    for hit in dbeaver_output_hits(output_root):
        issues.append(f"release_output:dbeaver_output_present:{hit}")

    return {
        "command": "verify_driver_beta_release.py",
        "gate_id": "BETA-DTA-GATE-019",
        "status": report_status(issues),
        "summary": {
            "manifest_components": len(manifest_by_id),
            "in_scope_components": len(expected_components),
            "artifact_rows": len(artifact_rows),
            "missing_outputs": len(missing_outputs),
            "output_root_exists": output_root.exists(),
        },
        "issues": issues,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    add_common_args(parser, Path(__file__))
    parser.add_argument("output_root", nargs="?", type=Path, default=Path("build/output"))
    args = parser.parse_args()
    repo_root = resolve_repo_root(args.repo_root)
    workplan_root = resolve_workplan_root(repo_root, args.workplan_root)
    output_root = args.output_root
    if not output_root.is_absolute():
        output_root = repo_root / output_root
    output = args.output or default_report_path(repo_root, REPORT_NAME)
    try:
        report = build_report(repo_root, workplan_root, output_root.resolve())
    except (OSError, ValueError) as exc:
        return fail(str(exc))
    write_report(output, report)
    print(f"driver_beta_release_verify={report['status']}")
    return 0 if report["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
