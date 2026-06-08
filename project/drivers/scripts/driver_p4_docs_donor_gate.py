#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate P4 documentation/sample and donor-driver route evidence."""

from __future__ import annotations

import argparse
import csv
import json
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any


DOC_EVIDENCE_NAME = "DSR_044_DOCUMENTATION_SAMPLE_APP_EVIDENCE.json"
DONOR_EVIDENCE_NAME = "DSR_045_DONOR_DRIVER_COMPATIBILITY_ROUTE_EVIDENCE.json"

REQUIRED_DOC_CAPABILITIES = {
    "dsn",
    "auth",
    "tls",
    "transactions",
    "prepared_statements",
    "metadata",
    "diagnostics",
    "cancel_or_lifecycle",
}

FORBIDDEN_ROUTE_TOKENS = (
    "parser_only",
    "parser-only",
    "direct_engine",
    "direct engine",
    "direct-to-engine",
    "direct_to_engine",
    "engine_api",
    "storage_api",
    "bypass_listener",
    "bypass_server",
    "embedded_engine",
)

DONOR_ROUTE_TOKENS = ("sbwp", "listener", "parser", "server", "engine")


@dataclass(frozen=True)
class Context:
    repo_root: Path
    project_root: Path
    execution_plan_root: Path

    @property
    def artifacts_root(self) -> Path:
        return self.execution_plan_root / "artifacts"

    @property
    def driver_manifest(self) -> Path:
        return self.project_root / "drivers" / "DriverPackageManifest.csv"


@dataclass(frozen=True)
class Component:
    component_id: str
    category: str
    name: str
    source_path: str
    conformance_profile_ref: str
    ingress_mode_set: str
    wire_protocol_set: str
    dsn_key_set: str
    auth_method_set: str
    tls_profile_set: str


def fail(message: str) -> int:
    print(f"failed: {message}", file=sys.stderr)
    return 1


def is_empty(value: Any) -> bool:
    if value is None:
        return True
    if isinstance(value, str):
        return value.strip() == ""
    if isinstance(value, (list, tuple, set, dict)):
        return len(value) == 0
    return False


def load_json(path: Path) -> dict[str, Any]:
    with path.open(encoding="utf-8") as handle:
        data = json.load(handle)
    if not isinstance(data, dict):
        raise ValueError(f"{path} must contain a JSON object")
    return data


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        if reader.fieldnames is None:
            raise ValueError(f"{path} has no CSV header")
        return [dict(row) for row in reader]


def render_path(path: Path, ctx: Context) -> str:
    try:
        return path.relative_to(ctx.repo_root).as_posix()
    except ValueError:
        return path.as_posix()


def resolve_repo_path(ctx: Context, value: str) -> Path:
    path = Path(value)
    if not path.is_absolute():
        path = ctx.repo_root / path
    return path.resolve()


def path_exists(ctx: Context, value: str, *, file_only: bool = True) -> bool:
    path = resolve_repo_path(ctx, value)
    if file_only:
        return path.is_file()
    return path.exists()


def contains_forbidden_shortcut(text: str) -> str | None:
    lowered = text.lower()
    for token in FORBIDDEN_ROUTE_TOKENS:
        if token in lowered:
            return token
    return None


def load_components(ctx: Context) -> dict[str, Component]:
    rows = read_csv(ctx.driver_manifest)
    components: dict[str, Component] = {}
    for row in rows:
        component_id = row.get("component_id", "").strip()
        if not component_id:
            raise ValueError(f"{render_path(ctx.driver_manifest, ctx)} has a row with empty component_id")
        if component_id in components:
            raise ValueError(f"{render_path(ctx.driver_manifest, ctx)} duplicate component_id {component_id}")
        components[component_id] = Component(
            component_id=component_id,
            category=row.get("category", "").strip(),
            name=row.get("name", "").strip(),
            source_path=row.get("source_path", "").strip(),
            conformance_profile_ref=row.get("conformance_profile_ref", "").strip(),
            ingress_mode_set=row.get("ingress_mode_set", "").strip(),
            wire_protocol_set=row.get("wire_protocol_set", "").strip(),
            dsn_key_set=row.get("dsn_key_set", "").strip(),
            auth_method_set=row.get("auth_method_set", "").strip(),
            tls_profile_set=row.get("tls_profile_set", "").strip(),
        )
    return components


def validate_shortcut_rejection(data: dict[str, Any], gate_id: str) -> list[str]:
    rejection = data.get("shortcut_rejection")
    if not isinstance(rejection, dict):
        return [f"{gate_id} missing shortcut_rejection mapping"]
    errors: list[str] = []
    for key in ("parser_only_shortcuts", "direct_engine_evidence", "allowed_route"):
        if is_empty(rejection.get(key)):
            errors.append(f"{gate_id} shortcut_rejection missing {key}")
    for key in ("parser_only_shortcuts", "direct_engine_evidence"):
        if rejection.get(key) != "rejected":
            errors.append(f"{gate_id} shortcut_rejection {key} must be rejected")
    route = str(rejection.get("allowed_route", ""))
    token = contains_forbidden_shortcut(route)
    if token:
        errors.append(f"{gate_id} allowed_route contains forbidden shortcut token {token}")
    return errors


def validate_route_manifest_fields(component: Component) -> list[str]:
    errors: list[str] = []
    if "sbwp_v1_1" not in component.wire_protocol_set:
        errors.append(f"{component.component_id} route missing sbwp_v1_1")
    if "scratchbird_tls_1_3_floor" not in component.tls_profile_set:
        errors.append(f"{component.component_id} route missing ScratchBird TLS floor")
    if "engine_local_password" not in component.auth_method_set:
        errors.append(f"{component.component_id} route missing engine auth method")
    dsn_keys = set(filter(None, component.dsn_key_set.split(";")))
    if not dsn_keys.intersection({"database", "dsn", "jdbc_url", "flight_endpoint"}):
        errors.append(f"{component.component_id} route missing DSN or connection target key")
    for field_name in (
        "source_path",
        "conformance_profile_ref",
        "ingress_mode_set",
        "wire_protocol_set",
        "dsn_key_set",
        "auth_method_set",
        "tls_profile_set",
    ):
        if is_empty(getattr(component, field_name)):
            errors.append(f"{component.component_id} DriverPackageManifest missing {field_name}")
    route_text = " ".join(
        [
            component.ingress_mode_set,
            component.wire_protocol_set,
            component.dsn_key_set,
            component.auth_method_set,
            component.tls_profile_set,
        ]
    )
    token = contains_forbidden_shortcut(route_text)
    if token:
        errors.append(f"{component.component_id} route declaration contains forbidden shortcut token {token}")
    return errors


def validate_doc_lane(
    ctx: Context,
    lane: dict[str, Any],
    component: Component,
    route_source: str,
) -> list[str]:
    errors: list[str] = []
    component_id = component.component_id
    source_dir = resolve_repo_path(ctx, component.source_path)
    if not source_dir.is_dir():
        errors.append(f"{component_id} source_path does not exist: {component.source_path}")

    doc_refs = lane.get("documentation_refs")
    if not isinstance(doc_refs, list) or not doc_refs:
        errors.append(f"{component_id} documentation_refs must be a non-empty list")
    else:
        for ref in doc_refs:
            if not isinstance(ref, str) or not ref.strip():
                errors.append(f"{component_id} documentation_refs contains an empty/non-string ref")
            elif not path_exists(ctx, ref):
                errors.append(f"{component_id} documentation ref missing: {ref}")

    expected_route_ref = f"{route_source}#{component_id}"
    if lane.get("declared_route_ref") != expected_route_ref:
        errors.append(
            f"{component_id} declared_route_ref must be {expected_route_ref}, got {lane.get('declared_route_ref')!r}"
        )

    harness = lane.get("sample_harness")
    if not isinstance(harness, dict):
        errors.append(f"{component_id} sample_harness must be a mapping")
    else:
        if harness.get("kind") not in {"ctest", "lane_smoke_harness"}:
            errors.append(f"{component_id} sample_harness kind must be ctest or lane_smoke_harness")
        ctest_name = str(harness.get("ctest_name", "")).strip()
        ctest_label = str(harness.get("ctest_label", "")).strip()
        command = str(harness.get("command", "")).strip()
        if ctest_name != component.conformance_profile_ref:
            errors.append(
                f"{component_id} ctest_name must cite {component.conformance_profile_ref}, got {ctest_name!r}"
            )
        if component.conformance_profile_ref not in ctest_label:
            errors.append(f"{component_id} ctest_label must include {component.conformance_profile_ref}")
        if "ctest" not in command or component.conformance_profile_ref not in command:
            errors.append(f"{component_id} command must invoke ctest for {component.conformance_profile_ref}")
        token = contains_forbidden_shortcut(command)
        if token:
            errors.append(f"{component_id} sample harness command contains forbidden shortcut token {token}")

    errors.extend(validate_route_manifest_fields(component))
    return errors


def validate_documentation_evidence(ctx: Context) -> int:
    path = ctx.artifacts_root / DOC_EVIDENCE_NAME
    try:
        data = load_json(path)
        components = load_components(ctx)
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        return fail(str(exc))

    errors: list[str] = []
    if data.get("gate_id") != "DSR-044":
        errors.append(f"{DOC_EVIDENCE_NAME} gate_id must be DSR-044")
    if data.get("route_declaration_source") != "project/drivers/DriverPackageManifest.csv":
        errors.append(f"{DOC_EVIDENCE_NAME} route_declaration_source must be project/drivers/DriverPackageManifest.csv")
    errors.extend(validate_shortcut_rejection(data, "DSR-044"))

    required = data.get("required_capabilities")
    if not isinstance(required, list) or set(required) != REQUIRED_DOC_CAPABILITIES:
        errors.append(f"{DOC_EVIDENCE_NAME} required_capabilities must match DSR-044 capability set")
    coverage = data.get("coverage_defaults")
    if not isinstance(coverage, dict):
        errors.append(f"{DOC_EVIDENCE_NAME} coverage_defaults must be a mapping")
    else:
        for capability in sorted(REQUIRED_DOC_CAPABILITIES):
            if is_empty(coverage.get(capability)):
                errors.append(f"{DOC_EVIDENCE_NAME} coverage_defaults missing {capability}")

    lanes = data.get("lanes")
    if not isinstance(lanes, list) or not lanes:
        errors.append(f"{DOC_EVIDENCE_NAME} lanes must be a non-empty list")
        lanes = []

    by_component: dict[str, dict[str, Any]] = {}
    duplicates: set[str] = set()
    for lane in lanes:
        if not isinstance(lane, dict):
            errors.append(f"{DOC_EVIDENCE_NAME} lanes contains a non-object item")
            continue
        component_id = str(lane.get("component_id", "")).strip()
        if not component_id:
            errors.append(f"{DOC_EVIDENCE_NAME} lanes contains an empty component_id")
            continue
        if component_id in by_component:
            duplicates.add(component_id)
        by_component[component_id] = lane
    if duplicates:
        errors.append(f"{DOC_EVIDENCE_NAME} duplicate lanes: {', '.join(sorted(duplicates))}")

    missing = sorted(set(components) - set(by_component))
    extra = sorted(set(by_component) - set(components))
    if missing:
        errors.append(f"{DOC_EVIDENCE_NAME} missing component lanes: {', '.join(missing)}")
    if extra:
        errors.append(f"{DOC_EVIDENCE_NAME} contains unknown component lanes: {', '.join(extra)}")

    route_source = str(data.get("route_declaration_source", ""))
    for component_id, lane in sorted(by_component.items()):
        component = components.get(component_id)
        if component is None:
            continue
        errors.extend(validate_doc_lane(ctx, lane, component, route_source))

    return print_result(errors, f"DSR-044 documentation sample evidence ok: {len(by_component)} lanes validated")


def validate_expected_sources(ctx: Context, route_id: str, value: Any) -> list[str]:
    errors: list[str] = []
    if not isinstance(value, list) or not value:
        return [f"{route_id} expected_behavior_source must be a non-empty list"]
    for ref in value:
        if not isinstance(ref, str) or not ref.strip():
            errors.append(f"{route_id} expected_behavior_source contains an empty/non-string ref")
        elif not path_exists(ctx, ref):
            errors.append(f"{route_id} expected behavior source missing: {ref}")
    return errors


def validate_donor_route(ctx: Context, route: dict[str, Any]) -> list[str]:
    route_id = str(route.get("route_id", "")).strip()
    prefix = route_id or "<empty route_id>"
    errors: list[str] = []
    required_fields = (
        "route_id",
        "donor_family",
        "donor_tool",
        "compatibility_profile",
        "scratchbird_route_command",
        "auth_identity",
        "expected_behavior_source",
        "comparison_rule",
        "refusal_rule",
        "ctest_label",
        "artifact_location",
    )
    for field in required_fields:
        if is_empty(route.get(field)):
            errors.append(f"{prefix} missing {field}")

    command = str(route.get("scratchbird_route_command", "")).strip()
    command_lower = command.lower()
    token = contains_forbidden_shortcut(command)
    if token:
        errors.append(f"{prefix} ScratchBird route command contains forbidden shortcut token {token}")
    for token in DONOR_ROUTE_TOKENS:
        if token not in command_lower:
            errors.append(f"{prefix} ScratchBird route command missing route token {token}")
    donor_tool = str(route.get("donor_tool", "")).strip().lower()
    profile = str(route.get("compatibility_profile", "")).strip().lower()
    if donor_tool and donor_tool not in command_lower:
        errors.append(f"{prefix} ScratchBird route command must cite donor tool {donor_tool}")
    if profile and profile not in command_lower:
        errors.append(f"{prefix} ScratchBird route command must cite compatibility profile {profile}")

    auth_identity = str(route.get("auth_identity", "")).strip()
    if auth_identity and not auth_identity.startswith("engine:"):
        errors.append(f"{prefix} auth_identity must be engine-owned and start with engine:")

    ctest_label = str(route.get("ctest_label", "")).strip()
    if "DSR-045" not in ctest_label:
        errors.append(f"{prefix} ctest_label must include DSR-045")
    if "donor_driver_compatibility_route" not in ctest_label:
        errors.append(f"{prefix} ctest_label must include donor_driver_compatibility_route")

    comparison = str(route.get("comparison_rule", "")).lower()
    refusal = str(route.get("refusal_rule", "")).lower()
    if "compare" not in comparison:
        errors.append(f"{prefix} comparison_rule must describe comparison")
    if "fail closed" not in refusal and "fail_closed" not in refusal:
        errors.append(f"{prefix} refusal_rule must require fail closed behavior")
    if "parser-only" not in refusal and "parser_only" not in refusal:
        errors.append(f"{prefix} refusal_rule must explicitly reject parser-only shortcuts")

    if route.get("parser_only_shortcut_rejected") is not True:
        errors.append(f"{prefix} parser_only_shortcut_rejected must be true")
    if route.get("direct_engine_evidence_rejected") is not True:
        errors.append(f"{prefix} direct_engine_evidence_rejected must be true")

    artifact_location = route.get("artifact_location")
    if isinstance(artifact_location, str) and artifact_location.strip():
        if not path_exists(ctx, artifact_location):
            errors.append(f"{prefix} artifact_location missing: {artifact_location}")

    errors.extend(validate_expected_sources(ctx, prefix, route.get("expected_behavior_source")))
    return errors


def validate_donor_evidence(ctx: Context) -> int:
    path = ctx.artifacts_root / DONOR_EVIDENCE_NAME
    try:
        data = load_json(path)
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        return fail(str(exc))

    errors: list[str] = []
    if data.get("gate_id") != "DSR-045":
        errors.append(f"{DONOR_EVIDENCE_NAME} gate_id must be DSR-045")
    if is_empty(data.get("route_requirement")):
        errors.append(f"{DONOR_EVIDENCE_NAME} missing route_requirement")
    errors.extend(validate_shortcut_rejection(data, "DSR-045"))

    routes = data.get("routes")
    if not isinstance(routes, list) or not routes:
        errors.append(f"{DONOR_EVIDENCE_NAME} routes must be a non-empty list")
        routes = []

    route_ids: dict[str, dict[str, Any]] = {}
    duplicate_ids: set[str] = set()
    families: set[str] = set()
    for route in routes:
        if not isinstance(route, dict):
            errors.append(f"{DONOR_EVIDENCE_NAME} routes contains a non-object item")
            continue
        route_id = str(route.get("route_id", "")).strip()
        if route_id in route_ids:
            duplicate_ids.add(route_id)
        if route_id:
            route_ids[route_id] = route
        family = str(route.get("donor_family", "")).strip()
        if family:
            families.add(family)
        errors.extend(validate_donor_route(ctx, route))

    if duplicate_ids:
        errors.append(f"{DONOR_EVIDENCE_NAME} duplicate routes: {', '.join(sorted(duplicate_ids))}")

    required_families = data.get("required_donor_families")
    if not isinstance(required_families, list) or not required_families:
        errors.append(f"{DONOR_EVIDENCE_NAME} required_donor_families must be a non-empty list")
    else:
        missing_families = sorted(set(str(item) for item in required_families) - families)
        if missing_families:
            errors.append(f"{DONOR_EVIDENCE_NAME} missing required donor families: {', '.join(missing_families)}")

    return print_result(errors, f"DSR-045 donor driver route evidence ok: {len(route_ids)} routes validated")


def print_result(errors: list[str], ok_message: str) -> int:
    if errors:
        print("failed: P4 docs/donor gate errors:", file=sys.stderr)
        for error in errors[:200]:
            print(f"- {error}", file=sys.stderr)
        if len(errors) > 200:
            print(f"- ... {len(errors) - 200} additional errors", file=sys.stderr)
        return 1
    print(ok_message)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, required=True)
    parser.add_argument("--project-root", type=Path, required=True)
    parser.add_argument("--fixture-root", "--execution_plan-root", dest="execution_plan_root", type=Path, required=True)
    parser.add_argument(
        "mode",
        choices=[
            "documentation-sample-evidence",
            "donor-driver-route-evidence",
            "all",
        ],
    )
    args = parser.parse_args()
    ctx = Context(
        repo_root=args.repo_root.resolve(),
        project_root=args.project_root.resolve(),
        execution_plan_root=args.execution_plan_root.resolve(),
    )

    if args.mode == "documentation-sample-evidence":
        return validate_documentation_evidence(ctx)
    if args.mode == "donor-driver-route-evidence":
        return validate_donor_evidence(ctx)
    doc_result = validate_documentation_evidence(ctx)
    donor_result = validate_donor_evidence(ctx)
    return 1 if doc_result != 0 or donor_result != 0 else 0


if __name__ == "__main__":
    raise SystemExit(main())
