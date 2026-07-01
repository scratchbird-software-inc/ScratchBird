#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate driver release artifact manifest rows and produced metadata."""

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
    is_closing_status,
    load_manifest,
    load_workplan_csv,
    map_expected_output,
    report_status,
    resolve_repo_root,
    resolve_workplan_root,
    status_value,
    unique_index,
    write_report,
)


REPORT_NAME = "driver_release_artifact_manifest.json"

METADATA_SYNONYMS = {
    "hash": ("hash", "sha256", "digest"),
    "sbom": ("sbom",),
    "license": ("license", "notice"),
    "version": ("version",),
    "source_commit": ("source_commit", "commit", "revision"),
    "resource_pack_digest": ("resource_pack_digest", "language_resource_pack_sha256"),
    "support_bundle_schema": ("support_bundle_schema",),
    "proof_summary": ("proof_summary",),
}


def artifact_metadata_tokens(path: Path) -> set[str]:
    tokens: set[str] = set()
    if not path.exists():
        return tokens
    if path.is_file():
        candidates = [path]
    else:
        candidates = [item for item in path.rglob("*") if item.is_file()]
    for item in candidates:
        name = item.name.lower()
        for token, aliases in METADATA_SYNONYMS.items():
            if any(alias in name for alias in aliases):
                tokens.add(token)
    json_manifests: list[Path] = []
    if path.is_file() and path.suffix.lower() == ".json":
        json_manifests = [path]
    elif path.is_dir():
        json_manifests = [item for item in path.rglob("*.json") if item.is_file()]
    for manifest in json_manifests:
        try:
            data = json.loads(manifest.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            continue
        keys = {str(key).lower() for key in data.keys()} if isinstance(data, dict) else set()
        for token, aliases in METADATA_SYNONYMS.items():
            if token in keys or any(alias in keys for alias in aliases):
                tokens.add(token)
    return tokens


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


def validate_sha256s(root: Path, issues: list[str], context: str) -> None:
    sums = root / "SHA256SUMS"
    if not sums.is_file():
        issues.append(f"artifact_manifest:{context}:missing_packaged_file:SHA256SUMS")
        return
    try:
        lines = sums.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        issues.append(f"artifact_manifest:{context}:sha256s_unreadable:{exc}")
        return
    for line_number, line in enumerate(lines, 1):
        if not line.strip():
            continue
        parts = line.split(None, 1)
        if len(parts) != 2:
            issues.append(f"artifact_manifest:{context}:sha256s_malformed_line:{line_number}")
            continue
        expected_hash, rel = parts
        rel = rel.strip()
        target = root / rel
        if not target.is_file():
            issues.append(f"artifact_manifest:{context}:sha256s_missing_file:{rel}")
            continue
        actual_hash = sha256(target)
        if actual_hash != expected_hash:
            issues.append(f"artifact_manifest:{context}:sha256_mismatch:{rel}")


def validate_driver_package(
    issues: list[str],
    package_root: Path,
    component: str,
    driver: str,
    expected_component: str | None = None,
) -> None:
    if not package_root.is_dir():
        issues.append(f"artifact_manifest:{component}:expected_output_missing:{package_root}")
        return

    package_manifest_path = package_root / "package_manifest.json"
    package_manifest, manifest_error = read_json(package_manifest_path)
    if manifest_error:
        issues.append(f"artifact_manifest:{component}:package_manifest:{manifest_error}")
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
            issues.append(f"artifact_manifest:{component}:missing_packaged_file:{rel}")
    if binary_path.is_file():
        if binary_path.stat().st_size <= 0:
            issues.append(f"artifact_manifest:{component}:empty_binary:{binary_rel}")
        if binary_path.stat().st_mode & 0o111 == 0:
            issues.append(f"artifact_manifest:{component}:binary_not_executable:{binary_rel}")

    if package_manifest:
        expected_manifest_fields = (
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
        )
        for field in expected_manifest_fields:
            if not package_manifest.get(field):
                issues.append(f"artifact_manifest:{component}:package_manifest_missing:{field}")
        expected_component = expected_component or component
        if package_manifest.get("component_id") != expected_component:
            issues.append(f"artifact_manifest:{component}:package_manifest_component_mismatch")
        if package_manifest.get("driver") != driver:
            issues.append(f"artifact_manifest:{component}:package_manifest_driver_mismatch")
        if binary_path.is_file() and package_manifest.get("binary_sha256") != sha256(binary_path):
            issues.append(f"artifact_manifest:{component}:package_manifest_binary_sha256_mismatch")

    support_manifest_path = package_root / "support" / "support_bundle_manifest.json"
    support_manifest, support_error = read_json(support_manifest_path)
    if support_error:
        issues.append(f"artifact_manifest:{component}:support_bundle_manifest:{support_error}")
    else:
        for field in ("hash", "resource_pack_digest", "support_bundle_schema", "proof_summary"):
            if not support_manifest.get(field):
                issues.append(f"artifact_manifest:{component}:support_bundle_missing:{field}")

    package_contract = package_root / "support" / "package_contract.json"
    if package_contract.exists() and read_json(package_contract)[1]:
        issues.append(f"artifact_manifest:{component}:package_contract_invalid_json")

    validate_sha256s(package_root, issues, component)


def driver_package_root(path: Path, driver: str) -> Path:
    parts = path.parts
    for index in range(0, len(parts) - 1):
        if parts[index] == "drivers" and parts[index + 1] == driver:
            return Path(*parts[: index + 2])
    return path


def validate_driver_subartifact(
    issues: list[str],
    row: dict[str, str],
    component: str,
    artifact_path: Path | None,
    driver: str,
    expected_component: str,
) -> None:
    validate_artifact_path(issues, row, component, artifact_path, driver=driver)
    if artifact_path is None or not artifact_path.exists():
        return
    package_root = driver_package_root(artifact_path, driver)
    if package_root != artifact_path and package_root.is_dir():
        validate_driver_package(issues, package_root, component, driver, expected_component)


def validate_release_metadata(release_root: Path, repo_root: Path, issues: list[str]) -> None:
    manifest_path = release_root / "RELEASE_MANIFEST.json"
    sums_path = release_root / "SHA256SUMS"
    if not manifest_path.is_file():
        issues.append("artifact_manifest:release_metadata:missing_RELEASE_MANIFEST")
        return
    if not sums_path.is_file():
        issues.append("artifact_manifest:release_metadata:missing_SHA256SUMS")
        return
    manifest, error = read_json(manifest_path)
    if error:
        issues.append(f"artifact_manifest:release_metadata:RELEASE_MANIFEST:{error}")
        return
    promoted_paths = set(manifest.get("promoted_paths", []))
    package_roots: set[str] = set()
    for category in ("drivers", "adapters", "tools"):
        root = release_root / category
        if root.is_dir():
            package_roots.update(
                str(path.relative_to(repo_root))
                for path in root.iterdir()
                if path.is_dir()
            )
    for rel in sorted(package_roots - promoted_paths):
        issues.append(f"artifact_manifest:release_metadata:missing_promoted_path:{rel}")
    validate_sha256s(release_root, issues, "release_metadata")


def validate_packaging_scripts(
    issues: list[str],
    repo_root: Path,
    release_root: Path,
    row: dict[str, str],
    artifact_path: Path | None,
) -> None:
    context = row.get("artifact_id", "release:driver-packaging-scripts")
    if artifact_path is None or not artifact_path.is_dir():
        issues.append(f"artifact_manifest:{context}:expected_output_missing:{row.get('expected_public_output', '')}")
        return
    required_scripts = (
        "packaging/scripts/promote_driver_release_artifacts.py",
        "project/tools/release/driver_release_artifact_manifest_gate.py",
        "project/tools/release/verify_driver_beta_release.py",
    )
    for rel in required_scripts:
        path = repo_root / rel
        if not path.is_file():
            issues.append(f"artifact_manifest:{context}:missing_script:{rel}")
        elif path.stat().st_size <= 0:
            issues.append(f"artifact_manifest:{context}:empty_script:{rel}")
    validate_release_metadata(release_root, repo_root, issues)
    promotion_report = repo_root / "build" / "reports" / "driver_packaging_promotion.json"
    if not promotion_report.is_file():
        issues.append(f"artifact_manifest:{context}:missing_promotion_manifest")


def required_metadata_tokens(value: str) -> set[str]:
    required: set[str] = set()
    text = value.lower()
    for token, aliases in METADATA_SYNONYMS.items():
        if token in text or any(alias in text for alias in aliases):
            required.add(token)
    return required


def rows_by_component(rows: list[dict[str, str]]) -> dict[str, list[dict[str, str]]]:
    grouped: dict[str, list[dict[str, str]]] = {}
    for row in rows:
        component = row.get("component_id", "").strip()
        if not component:
            continue
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


def substitute_expected(value: str, *, driver: str, release: str) -> str:
    return value.replace("<driver>", driver).replace("<release>", release)


def resolve_expected_output(
    expected: str,
    output_root: Path,
    repo_root: Path,
    *,
    driver: str = "",
) -> Path | None:
    expected = substitute_expected(
        expected.strip(),
        driver=driver,
        release=output_root.name,
    )
    return map_expected_output(expected, output_root, repo_root)


def validate_artifact_path(
    issues: list[str],
    row: dict[str, str],
    component: str,
    artifact_path: Path | None,
    *,
    driver: str = "",
) -> None:
    if artifact_path is None:
        issues.append(f"artifact_manifest:{component}:expected_output_none")
        return
    if not artifact_path.exists():
        issues.append(
            f"artifact_manifest:{component}:expected_output_missing:"
            f"{row.get('expected_public_output', '')}"
        )
        return
    present = artifact_metadata_tokens(artifact_path)
    if driver:
        package_root = driver_package_root(artifact_path, driver)
        if package_root != artifact_path and package_root.exists():
            present |= artifact_metadata_tokens(package_root)
    required = required_metadata_tokens(row.get("required_metadata", ""))
    for token in sorted(required - present):
        issues.append(f"artifact_manifest:{component}:metadata_missing:{token}")


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
    }
    manifest_category = {
        row["component_id"]: row.get("category", "").strip()
        for row in in_scope_manifest_rows(manifest_rows)
    }
    driver_names = {
        row["component_id"]: row.get("name", "").strip()
        for row in in_scope_manifest_rows(manifest_rows)
        if row.get("category", "").strip() == "driver"
    }
    for component in sorted(expected_components):
        candidates = artifact_by_component.get(component, [])
        if manifest_category.get(component) == "driver":
            candidates = [
                row for row in candidates
                if row.get("artifact_type", "").strip() == "driver_package"
            ]
        row = candidates[0] if candidates else None
        if row is None:
            issues.append(f"artifact_manifest:{component}:missing_row")
            continue
        if row.get("release_scope", "").strip() != "in_scope_required":
            issues.append(f"artifact_manifest:{component}:release_scope_not_in_scope_required")
        artifact_path = resolve_expected_output(
            row.get("expected_public_output", ""),
            output_root,
            repo_root,
            driver=driver_names.get(component, ""),
        )
        validate_artifact_path(
            issues,
            row,
            component,
            artifact_path,
            driver=driver_names.get(component, ""),
        )
        if manifest_category.get(component) == "driver" and artifact_path is not None:
            validate_driver_package(
                issues,
                artifact_path,
                component,
                driver_names.get(component, ""),
            )

    aggregate_components = {"drivers:all", "release:driver-packaging-scripts", DBEAVER_COMPONENT_ID}
    for row in artifact_by_component.get("drivers:all", []):
        if row.get("release_scope", "").strip() != "in_scope_required":
            issues.append(
                f"artifact_manifest:{row.get('artifact_id', 'drivers:all')}:"
                "release_scope_not_in_scope_required"
            )
        for component, driver in sorted(driver_names.items()):
            artifact_path = resolve_expected_output(
                row.get("expected_public_output", ""),
                output_root,
                repo_root,
                driver=driver,
            )
            validate_driver_subartifact(
                issues,
                row,
                f"{row.get('artifact_id', 'drivers:all')}:{component}",
                artifact_path,
                driver,
                component,
            )

    for row in artifact_by_component.get("release:driver-packaging-scripts", []):
        if row.get("release_scope", "").strip() != "in_scope_required":
            issues.append(
                "artifact_manifest:"
                f"{row.get('artifact_id', 'release:driver-packaging-scripts')}:"
                "release_scope_not_in_scope_required"
            )
        validate_packaging_scripts(
            issues,
            repo_root,
            output_root,
            row,
            resolve_expected_output(row.get("expected_public_output", ""), output_root, repo_root),
        )

    for component in sorted(set(artifact_by_component) - set(manifest_by_id) - aggregate_components):
        issues.append(f"artifact_manifest:{component}:not_in_manifest")

    dbeaver_rows = artifact_by_component.get(DBEAVER_COMPONENT_ID, [])
    dbeaver_row = dbeaver_rows[0] if dbeaver_rows else None
    if dbeaver_row is None:
        issues.append("artifact_manifest:dbeaver_exclusion_row_missing")
    else:
        if dbeaver_row.get("status", "").strip() != "excluded":
            issues.append("artifact_manifest:dbeaver_status_not_excluded")
        if dbeaver_row.get("expected_public_output", "").strip().lower() != "none":
            issues.append("artifact_manifest:dbeaver_expected_output_not_none")
    for hit in dbeaver_output_hits(output_root):
        issues.append(f"artifact_manifest:dbeaver_output_present:{hit}")

    return {
        "command": "driver_release_artifact_manifest_gate.py",
        "gate_id": "BETA-DTA-GATE-028",
        "status": report_status(issues),
        "summary": {
            "manifest_components": len(manifest_by_id),
            "in_scope_components": len(expected_components),
            "artifact_rows": len(artifact_rows),
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
    output_root = args.output_root if args.output_root.is_absolute() else repo_root / args.output_root
    output = args.output or default_report_path(repo_root, REPORT_NAME)
    try:
        report = build_report(repo_root, workplan_root, output_root.resolve())
    except (OSError, ValueError) as exc:
        return fail(str(exc))
    write_report(output, report)
    print(f"driver_release_artifact_manifest={report['status']}")
    return 0 if report["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
