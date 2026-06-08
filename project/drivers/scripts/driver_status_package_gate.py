#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate driver/adaptor/tool row-status and package evidence manifests."""

from __future__ import annotations

import argparse
import csv
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable

import yaml


FORBIDDEN_STATUSES = {
    "not_started",
    "implemented_without_evidence",
    "server_unspecified",
    "undocumented_implementation",
}

CLOSING_STATUSES = {
    "implemented_and_proven",
    "not_applicable_with_citation",
}

PACKAGE_EVIDENCE_ALIASES = {
    "package_manifest_ref": ("package_manifest_ref", "package_manifest", "manifest_ref"),
    "package_name": ("package_name",),
    "package_type": ("package_type",),
    "version_source": ("version_source",),
    "compatibility_range": ("compatibility_range", "supported_version_range"),
    "build_command": ("build_command",),
    "package_output_dir": ("package_output_dir", "output_dir", "output_directory"),
    "install_smoke_ref": (
        "install_smoke_ref",
        "install_smoke",
        "install_smoke_command",
        "install_smoke_ctest",
    ),
    "runtime_dependency_ref": (
        "runtime_dependency_ref",
        "runtime_dependencies",
        "runtime_dependency_list",
    ),
    "sbom_ref": ("sbom_ref", "sbom_status", "sbom"),
    "signing_ref": ("signing_ref", "signing_status", "signing"),
    "license_ref": ("license_ref", "license_status", "license_notice_ref", "license_notice"),
    "clean_uninstall_ref": (
        "clean_uninstall_ref",
        "clean_uninstall",
        "clean_uninstall_command",
        "uninstall_smoke",
    ),
    "build_tree_isolation_ref": (
        "build_tree_isolation_ref",
        "build_tree_isolation",
        "artifact_isolation_ref",
    ),
    "ctest_label": ("ctest_label", "ctest_labels", "package_ctest_label"),
}


@dataclass(frozen=True)
class Context:
    repo_root: Path
    project_root: Path
    execution_plan_root: Path

    @property
    def trace_root(self) -> Path:
        return self.repo_root / "docs" / "contracts" / "trace"

    @property
    def drivers_root(self) -> Path:
        return self.project_root / "drivers"


@dataclass(frozen=True)
class Component:
    component_id: str
    category: str
    name: str
    source_path: str
    conformance_profile_ref: str

    @property
    def lane_kind(self) -> str:
        if self.category == "adaptor":
            return "adapter"
        return self.category

    @property
    def row_family(self) -> str:
        if self.category == "adaptor":
            return "adapter_lane"
        return "driver_lane"


def fail(message: str) -> int:
    print(f"failed: {message}", file=sys.stderr)
    return 1


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def read_yaml(path: Path) -> Any:
    with path.open(encoding="utf-8") as handle:
        return yaml.safe_load(handle)


def is_empty(value: Any) -> bool:
    if value is None:
        return True
    if isinstance(value, str):
        return value.strip() == ""
    if isinstance(value, (list, tuple, set, dict)):
        return len(value) == 0
    return False


def render_path(path: Path, ctx: Context) -> str:
    try:
        return str(path.relative_to(ctx.repo_root))
    except ValueError:
        return str(path)


def load_components(ctx: Context) -> list[Component]:
    manifest = ctx.drivers_root / "DriverPackageManifest.csv"
    rows = read_csv(manifest)
    components: list[Component] = []
    seen: set[str] = set()
    for row in rows:
        component_id = row.get("component_id", "").strip()
        category = row.get("category", "").strip()
        name = row.get("name", "").strip()
        if not component_id or not category or not name:
            raise ValueError(f"DriverPackageManifest row has empty component identity: {row}")
        if component_id in seen:
            raise ValueError(f"DriverPackageManifest duplicate component_id: {component_id}")
        seen.add(component_id)
        components.append(
            Component(
                component_id=component_id,
                category=category,
                name=name,
                source_path=row.get("source_path", "").strip(),
                conformance_profile_ref=row.get("conformance_profile_ref", "").strip(),
            )
        )
    return components


def expected_manifest_path(ctx: Context, component: Component) -> Path:
    if component.category == "driver":
        prefix = "driver"
    elif component.category == "adaptor":
        prefix = "adapter"
    elif component.category == "tool":
        prefix = "tool"
    else:
        raise ValueError(f"{component.component_id} has unknown category {component.category}")
    return ctx.trace_root / f"{prefix}-{component.name}-checklist-status.yaml"


def load_expected_rows(ctx: Context) -> dict[str, dict[str, dict[str, str]]]:
    target_path = ctx.execution_plan_root / "artifacts" / "TARGET_CHECKLIST_ROWS.csv"
    registry_path = ctx.repo_root / "docs" / "contracts" / "registries" / "driver-normalized-verification-checklist.yaml"
    target_rows = read_csv(target_path)
    registry = read_yaml(registry_path)
    registry_items = registry.get("items", []) if isinstance(registry, dict) else []
    registry_ids = {str(item.get("id", "")) for item in registry_items if isinstance(item, dict)}
    missing_from_registry = sorted(row["id"] for row in target_rows if row["id"] not in registry_ids)
    if missing_from_registry:
        raise ValueError(
            "TARGET_CHECKLIST_ROWS contains ids missing from normalized registry: "
            + ", ".join(missing_from_registry)
        )

    by_family: dict[str, dict[str, dict[str, str]]] = {"driver_lane": {}, "adapter_lane": {}}
    for row in target_rows:
        family = row.get("applies_to", "")
        if family in by_family:
            by_family[family][row["id"]] = row
    return by_family


def load_schema(ctx: Context) -> dict[str, Any]:
    schema_path = ctx.trace_root / "driver-checklist-status-schema.yaml"
    schema = read_yaml(schema_path)
    if not isinstance(schema, dict):
        raise ValueError(f"{render_path(schema_path, ctx)} is not a mapping")
    return schema


def component_filter(components: Iterable[Component], mode: str) -> list[Component]:
    if mode == "driver-row-status":
        return [component for component in components if component.category == "driver"]
    if mode == "adapter-row-status":
        return [component for component in components if component.category == "adaptor"]
    if mode == "tool-row-status":
        return [component for component in components if component.category == "tool"]
    return list(components)


def manifest_identity_errors(data: dict[str, Any], component: Component, path: Path) -> list[str]:
    errors: list[str] = []
    lane = data.get("lane")
    allowed_lanes = {
        component.component_id,
        component.name,
        f"{component.lane_kind}:{component.name}",
    }
    if component.category == "adaptor":
        allowed_lanes.add(f"adaptor:{component.name}")
    if lane not in allowed_lanes:
        errors.append(
            f"{path.name} lane must identify {component.component_id}; got {lane!r}"
        )
    if data.get("lane_kind") != component.lane_kind:
        errors.append(
            f"{path.name} lane_kind must be {component.lane_kind}; got {data.get('lane_kind')!r}"
        )
    return errors


def row_is_conditional(row: dict[str, str]) -> bool:
    requirement = row.get("requirement", "")
    return requirement != "required"


def row_touches_auth(row: dict[str, str]) -> bool:
    text = f"{row.get('id', '')} {row.get('section', '')} {row.get('capability', '')}".lower()
    keywords = (
        "auth",
        "authorization",
        "security",
        "password",
        "scram",
        "tls",
        "mtls",
        "certificate",
        "kerberos",
        "gssapi",
        "oidc",
        "jwt",
        "saml",
        "ldap",
        "radius",
        "webauthn",
        "token",
        "peer",
        "policy",
    )
    return any(keyword in text for keyword in keywords)


def row_touches_mga(row: dict[str, str]) -> bool:
    text = f"{row.get('id', '')} {row.get('section', '')} {row.get('capability', '')}".lower()
    keywords = (
        "mga",
        "transaction",
        "autocommit",
        "commit",
        "rollback",
        "savepoint",
        "2pc",
        "xa",
        "cancel",
        "retry",
        "reset",
        "reconnect",
        "pool",
        "finality",
        "prepared transaction",
    )
    return any(keyword in text for keyword in keywords)


def item_evidence_errors(
    component: Component,
    item: dict[str, Any],
    row: dict[str, str],
    required_item_fields: list[str],
) -> list[str]:
    errors: list[str] = []
    row_id = str(item.get("id", ""))
    status = item.get("status")
    if status in FORBIDDEN_STATUSES:
        errors.append(f"{component.component_id} {row_id} has forbidden grey status {status}")
        return errors
    if status not in CLOSING_STATUSES:
        errors.append(f"{component.component_id} {row_id} has unknown/non-closing status {status!r}")
        return errors
    if status == "not_applicable_with_citation" and not row_is_conditional(row):
        errors.append(f"{component.component_id} {row_id} is required and cannot be not_applicable_with_citation")

    for field in required_item_fields:
        if is_empty(item.get(field)):
            errors.append(f"{component.component_id} {row_id} missing required evidence field {field}")
    if status == "not_applicable_with_citation":
        for field in ("runtime_citation", "diagnostic_or_api_limitation"):
            if is_empty(item.get(field)):
                errors.append(f"{component.component_id} {row_id} missing N/A evidence field {field}")
    if row_touches_auth(row) and is_empty(item.get("auth_ref")):
        errors.append(f"{component.component_id} {row_id} missing auth_ref")
    if row_touches_mga(row) and is_empty(item.get("mga_ref")):
        errors.append(f"{component.component_id} {row_id} missing mga_ref")
    return errors


def validate_row_status_manifest(
    ctx: Context,
    component: Component,
    schema: dict[str, Any],
    expected_rows: dict[str, dict[str, str]],
) -> list[str]:
    path = expected_manifest_path(ctx, component)
    if not path.is_file():
        return [f"expected row-status manifest missing: {render_path(path, ctx)}"]

    data = read_yaml(path)
    if not isinstance(data, dict):
        return [f"{render_path(path, ctx)} must be a YAML mapping"]

    errors: list[str] = []
    for field in schema.get("required_document_fields", []):
        if is_empty(data.get(field)):
            errors.append(f"{path.name} missing required document field {field}")
    errors.extend(manifest_identity_errors(data, component, path))

    items = data.get("items")
    if not isinstance(items, list):
        errors.append(f"{path.name} items must be a list")
        return errors

    by_id: dict[str, dict[str, Any]] = {}
    duplicate_ids: set[str] = set()
    for item in items:
        if not isinstance(item, dict):
            errors.append(f"{path.name} contains a non-mapping item")
            continue
        row_id = str(item.get("id", "")).strip()
        if not row_id:
            errors.append(f"{path.name} contains an item with empty id")
            continue
        if row_id in by_id:
            duplicate_ids.add(row_id)
        by_id[row_id] = item

    if duplicate_ids:
        errors.append(f"{path.name} has duplicate row ids: {', '.join(sorted(duplicate_ids))}")

    missing = sorted(set(expected_rows) - set(by_id))
    if missing:
        errors.append(
            f"{component.component_id} manifest missing required checklist rows: "
            + ", ".join(missing[:50])
            + (f" ... ({len(missing)} total)" if len(missing) > 50 else "")
        )

    unknown = sorted(set(by_id) - set(expected_rows))
    if unknown:
        errors.append(f"{component.component_id} manifest contains unknown row ids: {', '.join(unknown[:50])}")

    required_item_fields = [str(field) for field in schema.get("required_item_fields", [])]
    for row_id, item in sorted(by_id.items()):
        row = expected_rows.get(row_id)
        if row is None:
            continue
        errors.extend(item_evidence_errors(component, item, row, required_item_fields))
    return errors


def package_evidence(data: dict[str, Any]) -> dict[str, Any] | None:
    for key in ("package_evidence", "packaging_evidence"):
        value = data.get(key)
        if isinstance(value, dict):
            return value
    release_evidence = data.get("release_evidence")
    if isinstance(release_evidence, dict):
        for key in ("package_evidence", "packaging_evidence", "package"):
            value = release_evidence.get(key)
            if isinstance(value, dict):
                return value
    return None


def get_alias_value(mapping: dict[str, Any], aliases: tuple[str, ...]) -> Any:
    for alias in aliases:
        if alias in mapping:
            return mapping[alias]
    return None


def output_dir_is_source_tree(ctx: Context, component: Component, value: Any) -> bool:
    if not isinstance(value, str) or "$" in value or "{" in value or "}" in value:
        return False
    output = Path(value)
    if not output.is_absolute():
        output = ctx.repo_root / output
    try:
        output_resolved = output.resolve()
        source_resolved = (ctx.repo_root / component.source_path).resolve()
        output_resolved.relative_to(source_resolved)
        return True
    except (OSError, ValueError):
        return False


def validate_package_evidence(ctx: Context, component: Component) -> list[str]:
    path = expected_manifest_path(ctx, component)
    if not path.is_file():
        return [f"expected row-status manifest missing for package evidence: {render_path(path, ctx)}"]
    data = read_yaml(path)
    if not isinstance(data, dict):
        return [f"{render_path(path, ctx)} must be a YAML mapping"]

    evidence = package_evidence(data)
    if evidence is None:
        return [f"{component.component_id} missing package_evidence mapping in {path.name}"]

    errors: list[str] = []
    for canonical, aliases in PACKAGE_EVIDENCE_ALIASES.items():
        value = get_alias_value(evidence, aliases)
        if is_empty(value):
            errors.append(f"{component.component_id} package_evidence missing {canonical}")

    output_value = get_alias_value(evidence, PACKAGE_EVIDENCE_ALIASES["package_output_dir"])
    if output_dir_is_source_tree(ctx, component, output_value):
        errors.append(f"{component.component_id} package_output_dir points inside source tree")

    ctest_label = get_alias_value(evidence, PACKAGE_EVIDENCE_ALIASES["ctest_label"])
    if not is_empty(ctest_label):
        label_text = ";".join(str(value) for value in ctest_label) if isinstance(ctest_label, list) else str(ctest_label)
        if "DSR-033" not in label_text and component.conformance_profile_ref not in label_text:
            errors.append(
                f"{component.component_id} package_evidence ctest_label must cite DSR-033 "
                f"or {component.conformance_profile_ref}"
            )
    return errors


def print_errors(errors: list[str]) -> int:
    if errors:
        print("failed: driver status/package gate errors:", file=sys.stderr)
        for error in errors[:200]:
            print(f"- {error}", file=sys.stderr)
        if len(errors) > 200:
            print(f"- ... {len(errors) - 200} additional errors", file=sys.stderr)
        return 1
    return 0


def run_row_status_mode(ctx: Context, mode: str) -> int:
    try:
        components = component_filter(load_components(ctx), mode)
        expected_by_family = load_expected_rows(ctx)
        schema = load_schema(ctx)
    except (OSError, ValueError, yaml.YAMLError) as exc:
        return fail(str(exc))

    errors: list[str] = []
    for component in components:
        expected_rows = expected_by_family.get(component.row_family, {})
        if not expected_rows:
            errors.append(f"no expected checklist rows for {component.component_id} family {component.row_family}")
            continue
        try:
            errors.extend(validate_row_status_manifest(ctx, component, schema, expected_rows))
        except (OSError, ValueError, yaml.YAMLError) as exc:
            errors.append(f"{component.component_id}: {exc}")
    result = print_errors(errors)
    if result == 0:
        print(f"{mode} ok: {len(components)} component manifests validated")
    return result


def run_packaging_mode(ctx: Context) -> int:
    try:
        components = load_components(ctx)
    except (OSError, ValueError) as exc:
        return fail(str(exc))

    errors: list[str] = []
    for component in components:
        if is_empty(component.source_path):
            errors.append(f"{component.component_id} DriverPackageManifest source_path is empty")
            continue
        if is_empty(component.conformance_profile_ref):
            errors.append(f"{component.component_id} DriverPackageManifest conformance_profile_ref is empty")
        source = ctx.repo_root / component.source_path
        if not source.is_dir():
            errors.append(f"{component.component_id} source_path does not exist: {component.source_path}")
        try:
            errors.extend(validate_package_evidence(ctx, component))
        except (OSError, ValueError, yaml.YAMLError) as exc:
            errors.append(f"{component.component_id}: {exc}")
    result = print_errors(errors)
    if result == 0:
        print(f"packaging-evidence ok: {len(components)} component package records validated")
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, required=True)
    parser.add_argument("--project-root", type=Path, required=True)
    parser.add_argument("--fixture-root", "--execution_plan-root", dest="execution_plan_root", type=Path, required=True)
    parser.add_argument(
        "mode",
        choices=[
            "driver-row-status",
            "adapter-row-status",
            "tool-row-status",
            "row-status",
            "packaging-evidence",
            "all",
        ],
    )
    args = parser.parse_args()
    ctx = Context(
        repo_root=args.repo_root.resolve(),
        project_root=args.project_root.resolve(),
        execution_plan_root=args.execution_plan_root.resolve(),
    )

    if args.mode == "packaging-evidence":
        return run_packaging_mode(ctx)
    if args.mode == "all":
        row_status = run_row_status_mode(ctx, "row-status")
        packaging = run_packaging_mode(ctx)
        return 1 if row_status != 0 or packaging != 0 else 0
    return run_row_status_mode(ctx, args.mode)


if __name__ == "__main__":
    raise SystemExit(main())
