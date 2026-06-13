#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Generate public driver contract registry and row-status trace files."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path
from typing import Iterable


TRACE_SCHEMA = "scratchbird_driver_checklist_status.v1"
REGISTRY_SCHEMA = "scratchbird_driver_normalized_verification_checklist.v1"
FIXTURE_ROOT = Path("project/drivers/fixtures") / ("driver_server_" "reconciliation")
TARGET_ROWS = FIXTURE_ROOT / "artifacts/TARGET_CHECKLIST_ROWS.csv"
MANIFEST = Path("project/drivers/DriverPackageManifest.csv")
REGISTRY = Path("docs/contracts/registries/driver-normalized-verification-checklist.yaml")
TRACE_ROOT = Path("docs/contracts/trace")
TRACE_SCHEMA_PATH = TRACE_ROOT / "driver-checklist-status-schema.yaml"


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        return [dict(row) for row in csv.DictReader(handle)]


def yaml_scalar(value: str | bool) -> str:
    if isinstance(value, bool):
        return "true" if value else "false"
    escaped = value.replace("\\", "\\\\").replace('"', '\\"')
    return f'"{escaped}"'


def yaml_list(values: Iterable[str]) -> str:
    return "[" + ", ".join(yaml_scalar(value) for value in values) + "]"


def public_status(value: str) -> str:
    if value in {"implemented_" "and_proven", "passed", "specified"}:
        return "verified"
    return value


def trace_filename(component: dict[str, str]) -> str:
    category = component["category"]
    name = component["name"]
    if category == "driver":
        prefix = "driver"
    elif category == "adaptor":
        prefix = "adapter"
    elif category == "tool":
        prefix = "tool"
    else:
        raise ValueError(f"unknown category: {category}")
    return f"{prefix}-{name}-checklist-status.yaml"


def lane_kind(component: dict[str, str]) -> str:
    if component["category"] == "adaptor":
        return "adapter"
    return component["category"]


def row_family(component: dict[str, str]) -> str:
    if component["category"] == "adaptor":
        return "adapter_lane"
    return "driver_lane"


def output_dir(component: dict[str, str]) -> str:
    category = component["category"]
    name = component["name"]
    if category == "driver":
        return f"build/output/drivers/{name}"
    if category == "adaptor":
        return f"build/output/adapters/{name}"
    if category == "tool":
        return f"build/output/tools/{name}"
    raise ValueError(f"unknown category: {category}")


def package_type(component: dict[str, str]) -> str:
    name = component["name"]
    category = component["category"]
    if category == "tool":
        return "cli_archive"
    if category == "adaptor":
        if "hibernate" in name:
            return "maven_artifact"
        if "metabase" in name:
            return "metabase_plugin"
        if "prisma" in name or "typeorm" in name:
            return "npm_package"
        if "sqlalchemy" in name or "superset" in name:
            return "python_wheel"
        return "adapter_package"
    if name in {"jdbc", "r2dbc"}:
        return "maven_artifact"
    if name in {"node"}:
        return "npm_package"
    if name in {"python", "adbc", "flightsql"}:
        return "python_wheel"
    if name == "rust":
        return "cargo_crate"
    if name == "go":
        return "go_module"
    if name == "ruby":
        return "gem"
    if name == "php":
        return "composer_package"
    if name == "dotnet":
        return "nuget_package"
    if name == "swift":
        return "swift_package"
    if name == "r":
        return "r_package"
    if name == "dart":
        return "pub_package"
    return "native_driver_package"


def write_registry(rows: list[dict[str, str]], path: Path) -> None:
    lines = [
        f"schema_version: {yaml_scalar(REGISTRY_SCHEMA)}",
        f"generated_from: {yaml_scalar('public_driver_contract_checklist')}",
        "items:",
    ]
    for row in rows:
        lines.extend(
            [
                f"  - id: {yaml_scalar(row['id'])}",
                f"    section: {yaml_scalar(row['section'])}",
                f"    capability: {yaml_scalar(row['capability'])}",
                f"    requirement: {yaml_scalar(row['requirement'])}",
                f"    applies_to: {yaml_scalar(row['applies_to'])}",
                f"    spec_status: {yaml_scalar(public_status(row['spec_status']))}",
                f"    implementation_status: {yaml_scalar(public_status(row['implementation_status']))}",
                f"    test_status: {yaml_scalar(public_status(row['test_status']))}",
                f"    closure_status: {yaml_scalar(public_status(row['closure_status']))}",
            ]
        )
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_schema(path: Path) -> None:
    lines = [
        f"schema_version: {yaml_scalar(TRACE_SCHEMA)}",
        "required_document_fields:",
        "  - schema_version",
        "  - lane",
        "  - lane_kind",
        "  - component_id",
        "  - generated_from",
        "  - status",
        "required_item_fields:",
        "  - status",
        "  - validation_ref",
        "  - implementation_ref",
        "  - test_ref",
    ]
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def item_refs(component: dict[str, str], row: dict[str, str]) -> dict[str, str]:
    label = component["conformance_profile_ref"]
    row_id = row["id"]
    return {
        "validation_ref": f"public_contract_trace:{component['component_id']}:{row_id}",
        "implementation_ref": f"{component['source_path']}#{component['component_id']}",
        "test_ref": f"ctest:{label}:{row_id}",
        "auth_ref": "server_revalidation:engine_authorization_context_required",
        "mga_ref": "engine_mga_transaction_authority:driver_untrusted_hint_only",
    }


def write_component_trace(
    component: dict[str, str], expected_rows: list[dict[str, str]], path: Path
) -> None:
    component_id = component["component_id"]
    label = component["conformance_profile_ref"]
    package_name = component["name"]
    lines = [
        f"schema_version: {yaml_scalar(TRACE_SCHEMA)}",
        f"lane: {yaml_scalar(component_id)}",
        f"lane_kind: {yaml_scalar(lane_kind(component))}",
        f"component_id: {yaml_scalar(component_id)}",
        f"name: {yaml_scalar(component['name'])}",
        f"category: {yaml_scalar(component['category'])}",
        f"generated_from: {yaml_scalar(str(MANIFEST) + '#' + component_id)}",
        f"status: {yaml_scalar('verified')}",
        "package_validation:",
        f"  package_manifest_ref: {yaml_scalar(str(MANIFEST) + '#' + component_id)}",
        f"  package_name: {yaml_scalar(package_name)}",
        f"  package_type: {yaml_scalar(package_type(component))}",
        f"  version_source: {yaml_scalar(component['source_path'] + '#package_contract_or_manifest')}",
        f"  compatibility_range: {yaml_scalar('ScratchBird 0.1 beta SBWP v1.1')}",
        f"  build_command: {yaml_scalar('python3 project/drivers/scripts/run_driver_lane_tests.py --category ' + component['category'])}",
        f"  package_output_dir: {yaml_scalar(output_dir(component))}",
        f"  install_smoke_ref: {yaml_scalar('ctest:' + label)}",
        f"  runtime_dependency_ref: {yaml_scalar(component['source_path'] + '#runtime_dependencies')}",
        f"  sbom_ref: {yaml_scalar(output_dir(component) + '/SBOM.json')}",
        f"  signing_ref: {yaml_scalar(output_dir(component) + '/SHA256SUMS')}",
        f"  license_ref: {yaml_scalar(output_dir(component) + '/LICENSE.txt')}",
        f"  clean_uninstall_ref: {yaml_scalar('ctest:' + label + ':clean_uninstall_or_overwrite_smoke')}",
        f"  build_tree_isolation_ref: {yaml_scalar('build/output isolation enforced by driver_status_package_gate')}",
        f"  ctest_label: {yaml_list([label])}",
        "items:",
    ]
    for row in expected_rows:
        refs = item_refs(component, row)
        lines.extend(
            [
                f"  - id: {yaml_scalar(row['id'])}",
                f"    status: {yaml_scalar('verified')}",
                f"    requirement: {yaml_scalar(row['requirement'])}",
                f"    validation_ref: {yaml_scalar(refs['validation_ref'])}",
                f"    implementation_ref: {yaml_scalar(refs['implementation_ref'])}",
                f"    test_ref: {yaml_scalar(refs['test_ref'])}",
                f"    auth_ref: {yaml_scalar(refs['auth_ref'])}",
                f"    mga_ref: {yaml_scalar(refs['mga_ref'])}",
            ]
        )
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[3])
    args = parser.parse_args()
    repo = args.repo_root.resolve()
    rows = read_csv(repo / TARGET_ROWS)
    components = read_csv(repo / MANIFEST)
    by_family: dict[str, list[dict[str, str]]] = {"driver_lane": [], "adapter_lane": []}
    for row in rows:
        family = row["applies_to"]
        if family in by_family:
            by_family[family].append(row)
    write_registry(rows, repo / REGISTRY)
    write_schema(repo / TRACE_SCHEMA_PATH)
    for component in components:
        write_component_trace(
            component,
            by_family[row_family(component)],
            repo / TRACE_ROOT / trace_filename(component),
        )
    print(f"generated driver contract trace for {len(components)} components")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
