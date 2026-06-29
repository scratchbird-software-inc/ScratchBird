#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Cross-check the public driver-native conformance proof surface.

This gate intentionally does not read private workplans or specifications. It
validates that the public proof inputs expose the required coverage surfaces the
private tracker maps to: full-surface SBsql scripts, refusal diagnostics, native
tool examples, route/page/parser/concurrency axes, language resources, and
engine-owned MGA/SBLR authority artifacts.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import sys
from pathlib import Path
from typing import Any


FULL_SURFACE_MANIFEST_REL = Path(
    "project/tests/conformance/drivers/full_surface_scripts/manifest.json"
)
EXPECTED_ASSERTIONS_REL = Path(
    "project/tests/conformance/drivers/full_surface_scripts/expected/expected_assertions.json"
)
EXPECTED_REFUSALS_REL = Path(
    "project/tests/conformance/drivers/full_surface_scripts/expected/expected_refusals.json"
)
GATE_INPUT_REL = Path("project/tests/conformance/drivers/native_full_surface_gate_input.json")
TOOL_MATRIX_REL = Path("project/tests/conformance/drivers/native_tool_matrix.json")
DRIVER_PACKAGE_MANIFEST_REL = Path("project/drivers/DriverPackageManifest.csv")
LANGUAGE_SURFACE_REL = Path("project/drivers/language/sbsql_language_surface_manifest.json")
REPORT_REL = Path("build/reports/driver_native_workplan_coverage_gate.json")

RELEASE_BUCKETS = {"release_candidate", "release_supported", "supported"}
PLANNED_NOT_IMPLEMENTED = {"planned_not_implemented"}

REQUIRED_SCRIPT_IDS = {
    "SBDFS-000",
    "SBDFS-010",
    "SBDFS-011",
    "SBDFS-012",
    "SBDFS-013",
    "SBDFS-014",
    "SBDFS-015",
    "SBDFS-020",
    "SBDFS-030",
    "SBDFS-040",
    "SBDFS-045",
    "SBDFS-050",
    "SBDFS-052",
    "SBDFS-053",
    "SBDFS-054",
    "SBDFS-055",
    "SBDFS-056",
    "SBDFS-057",
    "SBDFS-058",
    "SBDFS-059",
    "SBDFS-060",
    "SBDFS-070",
    "SBDFS-080",
    "SBDFS-085",
    "SBDFS-086",
    "SBDFS-090",
    "SBDFS-092",
    "SBDFS-093",
    "SBDFS-099",
    "SBDFS-100",
    "SBDFS-101",
    "SBDFS-110",
    "SBDFS-120",
    "SBDFS-130",
    "SBDFS-140",
    "SBDFS-150",
    "SBDFS-160",
    "SBDFS-170",
    "SBDFS-180",
}

REQUIRED_COVERAGE_BY_TRACKER = {
    "DNFSC-006": {
        "schema_bootstrap",
        "generated_full_surface",
        "large_dataset_load",
        "copy",
    },
    "DNFSC-007": {
        "authorization_refusal",
        "cluster_inspection_refusal",
        "cluster_lifecycle_refusal",
        "domain_insert_refusal",
    },
    "DNFSC-008": {
        "all_table_column_type_families",
        "type_boundaries",
        "domain_as_column_type",
        "uuid_functions",
        "json_functions",
        "vector_functions",
        "large_dataset_load",
    },
    "DNFSC-009": {
        "arithmetic_operators",
        "comparison_operators",
        "logical_operators",
        "membership_operators",
        "pattern_operators",
        "function_operator_semantic_oracle_coverage",
        "operator_expected_output_fixture_pack",
    },
    "DNFSC-010": {
        "optimizer_cte",
        "optimizer_grouping",
        "optimizer_index_lookup",
        "optimizer_join",
        "inner_join",
        "left_join",
        "right_join",
        "full_outer_join",
        "cross_join",
        "lateral_join",
        "recursive_cte",
        "window_row_number",
        "group_by_having",
        "order_by_nulls_first",
        "order_by_nulls_last",
        "limit_offset",
        "fetch_first_rows_only",
        "select_distinct",
    },
    "DNFSC-011": {
        "authorization_filtered_resolution",
        "authorization_refusal",
        "cross_user_sblr_replay_refusal",
        "recursive_group_authorization",
        "security_grant_matrix",
        "grant_privilege",
        "revoke_privilege",
        "grant_role",
        "revoke_role",
        "has_role_expression",
        "policy_ddl",
        "rls_ddl",
        "mask_ddl",
    },
    "DNFSC-012": {
        "catalog_introspection_sys_schemas",
        "catalog_introspection_sys_tables",
        "catalog_introspection_sys_columns",
        "metadata_visibility",
        "security_catalog_introspection",
        "schema_qualified_name_resolution",
        "authorization_filtered_resolution",
    },
    "DNFSC-014": {
        "mga_begin_commit",
        "mga_rollback",
        "mga_savepoint",
    },
    "DNFSC-015": {
        "metadata_visibility",
        "session_configuration",
        "catalog_external_git",
    },
}

REQUIRED_GENERATED_SURFACE_IDS = {
    "SBDFS-100",
    "SBDFS-101",
    "SBDFS-110",
    "SBDFS-120",
    "SBDFS-130",
    "SBDFS-140",
    "SBDFS-150",
    "SBDFS-160",
    "SBDFS-170",
    "SBDFS-180",
}

REQUIRED_LANGUAGE_PROFILES = {
    "en-US",
    "en-CA",
    "fr-CA",
    "fr-FR",
    "de-DE",
    "it-IT",
    "es-ES",
}

REQUIRED_PAGE_SIZES = ["4k", "8k", "16k", "32k", "64k", "128k"]
REQUIRED_ROUTES = ["embedded", "ipc_local", "listener-parser", "manager-listener-parser"]
REQUIRED_PARSER_MODES = ["server-parser", "standalone-parser", "driver-sblr-uuid"]
REQUIRED_CONCURRENCY_MODES = {
    "single",
    "pooled",
    "parallel_isolated",
    "parallel_contention",
    "metadata_epoch_change",
    "cross_user_replay",
}
REQUIRED_TRANSPORT_MODES = {"tls_required", "tls_disabled"}
REQUIRED_COMMAND_GROUPS = {
    "connection",
    "database_create",
    "ddl",
    "dml",
    "query",
    "metadata",
    "transaction",
    "security_refusal",
    "sblr_uuid",
    "optimizer",
    "language",
    "cache",
}

REQUIRED_LANGUAGE_RESOURCE_FILES = {
    "resources/canonical/sbsql-dialect-baseline.json",
    "resources/canonical/system-object-name-registry.json",
    "resources/canonical/translation-source-corpus.jsonl",
    "resources/diagnostics/database-message-catalog.json",
    "resources/dialects/sbsql-v3-dialect-profile.json",
    "resources/phrases/phrase-table.json",
    "resources/predictive/predictive-grammar.json",
    "resources/rendering/rendering-templates.json",
    "resources/resolver/resolver-policy.json",
    "resources/topology/topology-profiles.json",
}


def repo_root_from_script() -> Path:
    return Path(__file__).resolve().parents[4]


def load_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8-sig") as handle:
        return list(csv.DictReader(handle))


def as_list(value: Any) -> list[Any]:
    return value if isinstance(value, list) else []


def script_assertions(script: dict[str, Any]) -> list[str]:
    return [str(item) for item in as_list(script.get("assertions"))]


def script_refusals(script: dict[str, Any]) -> list[str]:
    return [str(item) for item in as_list(script.get("expected_refusals"))]


def release_driver_rows(rows: list[dict[str, str]]) -> list[dict[str, str]]:
    return [
        row
        for row in rows
        if row.get("category") == "driver"
        and row.get("release_bucket") in RELEASE_BUCKETS
        and row.get("driver_status") not in PLANNED_NOT_IMPLEMENTED
    ]


def route_variant_count(routes: list[str]) -> int:
    count = 0
    for route in routes:
        if route in {"listener-parser", "manager-listener-parser"}:
            count += 2
        else:
            count += 1
    return count


def validate_scripts(
    manifest: dict[str, Any],
    expected_assertions: dict[str, Any],
    expected_refusals: dict[str, Any],
) -> tuple[list[str], dict[str, Any]]:
    errors: list[str] = []
    scripts = [item for item in as_list(manifest.get("scripts")) if isinstance(item, dict)]
    by_id = {str(item.get("script_id", "")): item for item in scripts}
    script_ids = set(by_id)
    if script_ids != REQUIRED_SCRIPT_IDS:
        errors.append(
            "scripts:script_id_set_drift:"
            f"missing={sorted(REQUIRED_SCRIPT_IDS - script_ids)} "
            f"extra={sorted(script_ids - REQUIRED_SCRIPT_IDS)}"
        )

    required_coverage = {str(item) for item in as_list(manifest.get("required_coverage"))}
    script_coverage: set[str] = set()
    assertion_ids: set[str] = set()
    refusal_refs: set[str] = set()
    for script in scripts:
        sid = str(script.get("script_id", ""))
        coverage = {str(item) for item in as_list(script.get("coverage"))}
        script_coverage.update(coverage)
        assertion_ids.update(script_assertions(script))
        refusal_refs.update(script_refusals(script))
        if not coverage:
            errors.append(f"scripts:{sid}:missing_coverage")
        if not script_assertions(script) and not script_refusals(script):
            errors.append(f"scripts:{sid}:missing_assertion_or_expected_refusal")
        path = str(script.get("path", ""))
        if not path.endswith(".sbsql"):
            errors.append(f"scripts:{sid}:path_must_be_sbsql")

    if script_coverage != required_coverage:
        errors.append(
            "scripts:required_coverage_must_equal_script_coverage:"
            f"missing={sorted(script_coverage - required_coverage)} "
            f"uncovered={sorted(required_coverage - script_coverage)}"
        )

    for tracker_id, coverage in REQUIRED_COVERAGE_BY_TRACKER.items():
        missing = coverage - required_coverage
        if missing:
            errors.append(f"coverage:{tracker_id}:missing:{','.join(sorted(missing))}")

    generated_missing = REQUIRED_GENERATED_SURFACE_IDS - script_ids
    if generated_missing:
        errors.append(f"scripts:missing_generated_surface_scripts:{sorted(generated_missing)}")

    expected_count = expected_assertions.get("assertion_count")
    actual_count = len(assertion_ids)
    if expected_count != actual_count:
        errors.append(f"assertions:count_drift:expected={expected_count}:actual={actual_count}")
    shape = set(str(item) for item in as_list(expected_assertions.get("assertion_result_shape")))
    if shape != {"assertion_id", "actual_*", "expected_*"}:
        errors.append("assertions:result_shape_drift")

    diagnostics = expected_refusals.get("expected_diagnostics")
    if not isinstance(diagnostics, dict) or not diagnostics:
        errors.append("refusals:expected_diagnostics_missing")
        diagnostics = {}
    for ref in refusal_refs:
        if ref not in diagnostics:
            errors.append(f"refusals:{ref}:missing_expected_diagnostics")
            continue
        values = diagnostics.get(ref)
        if not isinstance(values, list) or not values:
            errors.append(f"refusals:{ref}:empty_expected_diagnostics")

    if not any(ref.startswith("160_builtin_function_invocations.sbsql:") for ref in refusal_refs):
        errors.append("refusals:generated_function_refusals_missing")

    summary = {
        "script_count": len(scripts),
        "coverage_count": len(required_coverage),
        "assertion_count": actual_count,
        "expected_refusal_count": len(refusal_refs),
        "generated_surface_scripts": sorted(REQUIRED_GENERATED_SURFACE_IDS & script_ids),
        "tracker_coverage": {
            tracker_id: sorted(values) for tracker_id, values in REQUIRED_COVERAGE_BY_TRACKER.items()
        },
    }
    return errors, summary


def validate_gate_input(doc: dict[str, Any]) -> tuple[list[str], dict[str, Any]]:
    errors: list[str] = []
    page_sizes = [str(value) for value in as_list(doc.get("required_page_sizes"))]
    routes = [str(value) for value in as_list(doc.get("required_routes"))]
    parser_modes = [str(value) for value in as_list(doc.get("required_parser_modes"))]
    concurrency_modes = {str(value) for value in as_list(doc.get("required_concurrency_modes"))}
    transport_modes = {str(value) for value in as_list(doc.get("required_transport_modes"))}
    command_groups = {str(value) for value in as_list(doc.get("required_command_groups"))}
    artifacts = {str(value) for value in as_list(doc.get("required_artifacts"))}
    args = {str(value) for value in as_list(doc.get("required_tool_arguments"))}

    if page_sizes != REQUIRED_PAGE_SIZES:
        errors.append("gate_input:page_size_matrix_drift")
    if routes != REQUIRED_ROUTES:
        errors.append("gate_input:route_matrix_drift")
    if parser_modes != REQUIRED_PARSER_MODES:
        errors.append("gate_input:parser_mode_matrix_drift")
    if concurrency_modes != REQUIRED_CONCURRENCY_MODES:
        errors.append("gate_input:concurrency_mode_matrix_drift")
    if transport_modes != REQUIRED_TRANSPORT_MODES:
        errors.append("gate_input:transport_mode_matrix_drift")
    if command_groups != REQUIRED_COMMAND_GROUPS:
        errors.append("gate_input:command_group_drift")

    for artifact in (
        "wire-transcript.jsonl",
        "timing-groups.json",
        "metadata-snapshots.json",
        "route-environment.json",
        "security-refusals.json",
        "native-api-coverage.json",
        "code-example-review.json",
        "process-metrics.jsonl",
    ):
        if artifact not in artifacts:
            errors.append(f"gate_input:missing_artifact:{artifact}")

    for argument in (
        "--route",
        "--parser-mode",
        "--page-size",
        "--namespace",
        "--input",
        "--output",
        "--error",
        "--diagnostics",
        "--metrics",
        "--transcript",
        "--summary",
        "--expected-refusals",
        "--language-resource-pack",
        "--language-resource-identity",
        "--language-resource-hash",
        "--language-profile",
        "--syntax-profile",
        "--topology-profile",
        "--standard-english-fallback",
    ):
        if argument not in args:
            errors.append(f"gate_input:missing_tool_argument:{argument}")

    authority = doc.get("server_authority")
    if not isinstance(authority, dict):
        errors.append("gate_input:server_authority_missing")
    else:
        for key in (
            "driver_sblr_uuid_is_untrusted_hint",
            "server_revalidation_required",
            "mga_transaction_finality_engine_owned",
            "driver_or_parser_finality_forbidden",
        ):
            if authority.get(key) is not True:
                errors.append(f"gate_input:authority:{key}:not_true")

    if doc.get("fail_on_static_only_evidence") is not True:
        errors.append("gate_input:must_fail_on_static_only_evidence")

    summary = {
        "routes": routes,
        "route_transport_variant_count": route_variant_count(routes),
        "page_sizes": page_sizes,
        "parser_modes": parser_modes,
        "concurrency_modes": sorted(concurrency_modes),
        "command_groups": sorted(command_groups),
        "artifact_count": len(artifacts),
    }
    return errors, summary


def validate_tools(
    tool_matrix: dict[str, Any],
    package_rows: list[dict[str, str]],
) -> tuple[list[str], dict[str, Any]]:
    errors: list[str] = []
    tools = [item for item in as_list(tool_matrix.get("driver_tools")) if isinstance(item, dict)]
    by_driver = {str(item.get("driver", "")): item for item in tools}
    capabilities = tool_matrix.get("transport_capability_by_driver")
    if not isinstance(capabilities, dict):
        capabilities = {}
        errors.append("tools:transport_capability_map_missing")
    release_rows = release_driver_rows(package_rows)
    release_drivers = [str(row.get("name")) for row in release_rows]

    for driver in release_drivers:
        tool = by_driver.get(driver)
        if not isinstance(tool, dict):
            errors.append(f"tools:{driver}:missing_native_tool_entry")
            continue
        native_tokens = [str(token) for token in as_list(tool.get("native_tokens"))]
        if not native_tokens:
            errors.append(f"tools:{driver}:missing_native_api_tokens")
        path = str(tool.get("path", ""))
        if not path:
            errors.append(f"tools:{driver}:missing_path")
        cap = capabilities.get(driver)
        if not isinstance(cap, dict):
            errors.append(f"tools:{driver}:missing_transport_capability")
            continue
        if cap.get("inet_required") is not True:
            errors.append(f"tools:{driver}:inet_must_be_required")
        if cap.get("embedded_supported") is True:
            boundary = str(cap.get("embedded_boundary", "")).lower()
            if "cpp" not in boundary and "c++" not in boundary and "library" not in boundary:
                errors.append(f"tools:{driver}:embedded_requires_cpp_library_boundary")

    planned = [
        str(row.get("name"))
        for row in package_rows
        if row.get("category") == "driver"
        and row.get("driver_status") in PLANNED_NOT_IMPLEMENTED
    ]
    for driver in planned:
        row = next(row for row in package_rows if row.get("name") == driver)
        if row.get("release_bucket") != "tracked_not_released":
            errors.append(f"tools:{driver}:planned_driver_must_be_tracked_not_released")

    summary = {
        "release_driver_count": len(release_drivers),
        "release_drivers": release_drivers,
        "planned_not_implemented_drivers": planned,
        "tool_entry_count": len(tools),
    }
    return errors, summary


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return "sha256:" + digest.hexdigest()


def validate_language(repo_root: Path, language_doc: dict[str, Any]) -> tuple[list[str], dict[str, Any]]:
    errors: list[str] = []
    metadata = language_doc.get("common_resource_pack_metadata")
    if not isinstance(metadata, dict):
        return ["language:missing_common_resource_pack_metadata"], {}
    profiles = {str(value) for value in as_list(metadata.get("supported_exact_profiles"))}
    missing = REQUIRED_LANGUAGE_PROFILES - profiles
    if missing:
        errors.append(f"language:missing_profiles:{sorted(missing)}")
    if not str(metadata.get("resource_identity", "")):
        errors.append("language:missing_resource_identity")
    if not str(metadata.get("resource_pack_common_resource_hash", "")):
        errors.append("language:missing_resource_hash")
    resource_pack_path = str(metadata.get("resource_pack_path", ""))
    if not resource_pack_path:
        errors.append("language:missing_resource_pack_path")
        pack_root = None
    else:
        pack_root = (repo_root / resource_pack_path).resolve()
        try:
            pack_root.relative_to(repo_root)
        except ValueError:
            errors.append("language:resource_pack_path_must_be_under_repo")
        if not pack_root.is_dir():
            errors.append(f"language:resource_pack_path_missing:{resource_pack_path}")

    pack_manifest_summary: dict[str, Any] = {}
    if pack_root is not None and pack_root.is_dir():
        manifest_path = pack_root / "manifest.sblrp.json"
        if not manifest_path.is_file():
            errors.append("language:pack_manifest_missing")
        else:
            try:
                pack_manifest = load_json(manifest_path)
            except json.JSONDecodeError as exc:
                errors.append(f"language:pack_manifest_invalid_json:{exc}")
                pack_manifest = {}
            manifest_hash = file_sha256(manifest_path)
            if metadata.get("resource_pack_manifest_sha256") != manifest_hash:
                errors.append(
                    "language:resource_pack_manifest_sha256_drift:"
                    f"{metadata.get('resource_pack_manifest_sha256')}!={manifest_hash}"
                )
            common_hash = str(pack_manifest.get("common_resource_hash", ""))
            if common_hash != str(metadata.get("resource_pack_common_resource_hash", "")):
                errors.append("language:pack_common_resource_hash_drift")
            files = {
                str(item.get("path", "")): item
                for item in as_list(pack_manifest.get("files"))
                if isinstance(item, dict)
            }
            missing_files = REQUIRED_LANGUAGE_RESOURCE_FILES - set(files)
            if missing_files:
                errors.append(f"language:pack_missing_required_files:{sorted(missing_files)}")
            for profile in REQUIRED_LANGUAGE_PROFILES:
                profile_path = f"resources/languages/{profile}/language-profile.json"
                if profile_path not in files:
                    errors.append(f"language:pack_missing_profile_file:{profile_path}")
            for relative_path, item in files.items():
                actual_path = pack_root / relative_path
                if not actual_path.is_file():
                    errors.append(f"language:pack_file_missing:{relative_path}")
                    continue
                expected_hash = str(item.get("sha256", ""))
                if expected_hash and file_sha256(actual_path) != expected_hash:
                    errors.append(f"language:pack_file_sha256_drift:{relative_path}")
            pack_manifest_summary = {
                "manifest_path": str(manifest_path.relative_to(repo_root)),
                "manifest_sha256": manifest_hash if manifest_path.is_file() else "",
                "common_resource_hash": common_hash,
                "file_count": len(files),
                "required_resource_files": sorted(REQUIRED_LANGUAGE_RESOURCE_FILES),
            }
    return errors, {
        "resource_identity": metadata.get("resource_identity"),
        "resource_hash": metadata.get("resource_pack_common_resource_hash"),
        "resource_pack_path": resource_pack_path,
        "supported_profiles": sorted(profiles),
        "pack_manifest": pack_manifest_summary,
    }


def run(repo_root: Path) -> tuple[list[str], dict[str, Any]]:
    errors: list[str] = []
    manifest = load_json(repo_root / FULL_SURFACE_MANIFEST_REL)
    expected_assertions = load_json(repo_root / EXPECTED_ASSERTIONS_REL)
    expected_refusals = load_json(repo_root / EXPECTED_REFUSALS_REL)
    gate_input = load_json(repo_root / GATE_INPUT_REL)
    tool_matrix = load_json(repo_root / TOOL_MATRIX_REL)
    package_rows = read_csv(repo_root / DRIVER_PACKAGE_MANIFEST_REL)
    language_doc = load_json(repo_root / LANGUAGE_SURFACE_REL)

    script_errors, script_summary = validate_scripts(
        manifest,
        expected_assertions,
        expected_refusals,
    )
    gate_errors, gate_summary = validate_gate_input(gate_input)
    tool_errors, tool_summary = validate_tools(tool_matrix, package_rows)
    language_errors, language_summary = validate_language(repo_root, language_doc)

    errors.extend(script_errors)
    errors.extend(gate_errors)
    errors.extend(tool_errors)
    errors.extend(language_errors)

    release_driver_count = tool_summary.get("release_driver_count", 0)
    combination_count = (
        release_driver_count
        * gate_summary.get("route_transport_variant_count", 0)
        * len(gate_summary.get("page_sizes", []))
        * len(gate_summary.get("parser_modes", []))
        * len(gate_summary.get("concurrency_modes", []))
    )

    report = {
        "command": "driver_native_workplan_coverage_gate.py",
        "status": "pass" if not errors else "fail",
        "script_surface": script_summary,
        "gate_contract": gate_summary,
        "native_tools": tool_summary,
        "language_resources": language_summary,
        "planned_matrix_combination_upper_bound": combination_count,
        "issues": errors,
    }
    return errors, report


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, default=repo_root_from_script())
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    try:
        errors, report = run(repo_root)
    except (OSError, json.JSONDecodeError, KeyError) as exc:
        errors = [f"workplan_coverage:load_failed:{exc}"]
        report = {
            "command": "driver_native_workplan_coverage_gate.py",
            "status": "fail",
            "issues": errors,
        }

    output = args.output or repo_root / REPORT_REL
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1
    print(
        "driver_native_workplan_coverage_gate: OK "
        f"scripts={report['script_surface']['script_count']} "
        f"coverage={report['script_surface']['coverage_count']} "
        f"release_drivers={report['native_tools']['release_driver_count']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
