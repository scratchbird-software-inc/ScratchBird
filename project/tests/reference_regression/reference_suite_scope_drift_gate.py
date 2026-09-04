#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate reference replay suite scope and version-drift metadata."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import pathlib
import sys


REFERENCE_ROOT = pathlib.Path("project/tests/reference_regression")
PROFILE_MANIFEST = pathlib.Path("project/src/parsers/compatibility/CompatibilityProfileManifest.csv")
ACQUISITION_SOURCES = REFERENCE_ROOT / "reference_regression_acquisition_sources.csv"
REPLAY_PLAN = REFERENCE_ROOT / "reference_original_replay_plan.csv"

FORBIDDEN_TEXT = ("future", "defer", "deferred", "stub", "todo", "tbd", "fixme")
REQUIRED_ENDPOINT_ENV = {
    "SCRATCHBIRD_REFERENCE_ENDPOINT",
    "SCRATCHBIRD_REFERENCE_AUTH_PACKET",
    "SCRATCHBIRD_REFERENCE_RESULT_DIR",
}
VALID_HARNESS_STATUS = {
    "client_replay_harness_passed",
    "native_tool_replay_passed",
}
VALID_UPSTREAM_STATUS = {
    "original_reference_replay_passed",
}

UPSTREAM_COLUMNS = {
    "reference_id",
    "display_name",
    "suite_id",
    "suite_relative_source",
    "suite_source_locator",
    "source_exists",
    "source_kind",
    "file_count",
    "total_bytes",
    "tree_shape_digest",
    "parser_facing_suite_families",
    "extraction_destination",
    "implementation_dependency",
    "status",
}
EXCLUSION_COLUMNS = {
    "reference_id",
    "exclusion_id",
    "scope",
    "reason_code",
    "owner",
    "acceptance_rule",
    "status",
}
HARNESS_COLUMNS = {
    "reference_id",
    "harness_id",
    "native_tool_family",
    "tool_locator",
    "tool_exists",
    "tool_kind",
    "tool_file_count",
    "tool_total_bytes",
    "tool_tree_shape_digest",
    "required_endpoint_env",
    "required_output",
    "parser_authority_rule",
    "status",
}


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def read_csv(path: pathlib.Path, required: set[str] | None = None) -> list[dict[str, str]]:
    require(path.is_file(), f"missing CSV: {path}")
    with path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        require(reader.fieldnames is not None, f"{path}: missing header")
        if required:
            missing = sorted(required - set(reader.fieldnames))
            require(not missing, f"{path}: missing columns {missing}")
        rows = [dict(row) for row in reader]
    require(rows, f"{path}: no rows")
    return rows


def split_semicolon(value: str) -> list[str]:
    return [item.strip() for item in value.split(";") if item.strip()]


def check_no_forbidden(path: pathlib.Path, rows: list[dict[str, str]]) -> None:
    for row_number, row in enumerate(rows, start=2):
        lowered = " ".join(row.values()).lower()
        for token in FORBIDDEN_TEXT:
            require(token not in lowered, f"{path}:{row_number}: forbidden planning token {token!r}")


def load_keyed(rows: list[dict[str, str]], key: str, path: pathlib.Path) -> dict[str, dict[str, str]]:
    keyed: dict[str, dict[str, str]] = {}
    for row in rows:
        value = row[key]
        require(value not in keyed, f"{path}: duplicate {key} {value}")
        keyed[value] = row
    return keyed


def validate_harness(repo_root: pathlib.Path, family: str, harness_root: pathlib.Path) -> int:
    path = harness_root / "native_tool_harness" / "native_tool_harness_manifest.csv"
    rows = read_csv(path, HARNESS_COLUMNS)
    check_no_forbidden(path, rows)
    native_tool_rows = 0
    for row_number, row in enumerate(rows, start=2):
        context = f"{path}:{row_number}"
        require(row["reference_id"], f"{context}: reference_id is empty")
        env = set(split_semicolon(row["required_endpoint_env"]))
        require(REQUIRED_ENDPOINT_ENV <= env, f"{context}: required endpoint env is incomplete")
        outputs = set(split_semicolon(row["required_output"]))
        require("normalized_native_replay_results.json" in outputs,
                f"{context}: normalized output is missing")
        require(row["status"] in VALID_HARNESS_STATUS, f"{context}: invalid status {row['status']!r}")
        if row["tool_exists"] == "yes":
            native_tool_rows += 1
            locator = repo_root / row["tool_locator"]
            require(row["tool_kind"] == "file", f"{context}: native tool kind must be file")
            require(int(row["tool_file_count"]) > 0, f"{context}: native tool file count is zero")
            require(int(row["tool_total_bytes"]) > 0, f"{context}: native tool bytes is zero")
            require(len(row["tool_tree_shape_digest"]) == 64,
                    f"{context}: native tool digest must be sha256")
            require("engine retains" in row["parser_authority_rule"],
                    f"{context}: native tool authority rule must preserve engine authority")
            # Native reference tools are external, ignored local installations.
            # The scope manifest records their acquired shape even when this
            # checkout has no runnable tool; execution gates classify that
            # condition as a skip rather than packaging a donor binary.
            if locator.exists():
                require(locator.is_file(), f"{context}: installed native tool is not a file")
                require(locator.stat().st_size == int(row["tool_total_bytes"]),
                        f"{context}: installed native tool size drift")
        else:
            require(row["tool_locator"] == "no_compiled_tool_packaged",
                    f"{context}: non-tool row must not point at a payload")
            require("client replay harness required" in row["parser_authority_rule"],
                    f"{context}: client replay fallback authority rule is incomplete")
    return native_tool_rows


def validate_upstream_manifest(harness_root: pathlib.Path) -> int:
    path = harness_root / "upstream_manifest.csv"
    rows = read_csv(path, UPSTREAM_COLUMNS)
    check_no_forbidden(path, rows)
    seen_suite_ids: set[str] = set()
    for row_number, row in enumerate(rows, start=2):
        context = f"{path}:{row_number}"
        suite_id = row["suite_id"]
        require(suite_id not in seen_suite_ids, f"{context}: duplicate suite_id {suite_id}")
        seen_suite_ids.add(suite_id)
        require(row["source_exists"] == "yes", f"{context}: source_exists must be yes")
        require(row["source_kind"] in {"directory", "file"}, f"{context}: invalid source_kind")
        require(int(row["file_count"]) > 0, f"{context}: file_count must be positive")
        require(int(row["total_bytes"]) > 0, f"{context}: total_bytes must be positive")
        require(len(row["tree_shape_digest"]) == 64, f"{context}: tree_shape_digest must be sha256")
        require(split_semicolon(row["parser_facing_suite_families"]),
                f"{context}: parser-facing suite family classification is empty")
        require(row["extraction_destination"], f"{context}: extraction_destination is empty")
        require(row["implementation_dependency"], f"{context}: implementation_dependency is empty")
        require(row["status"] in VALID_UPSTREAM_STATUS, f"{context}: invalid status {row['status']!r}")
    return len(rows)


def validate_exclusions(harness_root: pathlib.Path) -> int:
    path = harness_root / "exclusion_register.csv"
    rows = read_csv(path, EXCLUSION_COLUMNS)
    check_no_forbidden(path, rows)
    for row_number, row in enumerate(rows, start=2):
        context = f"{path}:{row_number}"
        require(row["reference_id"], f"{context}: reference_id is empty")
        require(row["scope"], f"{context}: scope is empty")
        require(row["reason_code"], f"{context}: reason_code is empty")
        require(row["owner"], f"{context}: owner is empty")
        require(row["acceptance_rule"], f"{context}: acceptance_rule is empty")
        require(row["status"], f"{context}: status is empty")
        if row["status"] in {"no_exclusions_accepted", "manifest_generated_no_exclusions_accepted_yet"}:
            require(row["reason_code"] == "mandatory_reason_code_required_for_any_non_imported_test",
                    f"{context}: no-exclusion policy row must keep the mandatory reason-code marker")
            continue
        if row["status"] == "locator_corrected":
            require(row["reason_code"] == "upstream_test_path_absent_grammar_sources_inventoried",
                    f"{context}: locator correction has unexpected reason {row['reason_code']!r}")
            continue
        if row["status"]:
            allowed_reasons = {
                "support_udr",
                "engine_listener",
                "physical_engine_only",
                "license_excluded",
                "environment_only",
                "not_parser_relevant",
                "explicit_refusal",
            }
            require(row["reason_code"] in allowed_reasons,
                    f"{context}: unapproved exclusion reason {row['reason_code']!r}")
    return len(rows)


def validate(repo_root: pathlib.Path) -> dict[str, object]:
    profiles = load_keyed(read_csv(repo_root / PROFILE_MANIFEST), "family_id", PROFILE_MANIFEST)
    sources = load_keyed(read_csv(repo_root / ACQUISITION_SOURCES), "source_id", ACQUISITION_SOURCES)
    plan_rows = read_csv(repo_root / REPLAY_PLAN)
    check_no_forbidden(REPLAY_PLAN, plan_rows)

    family_count = 0
    suite_rows = 0
    exclusion_rows = 0
    native_tool_rows = 0
    source_ids_seen: set[str] = set()

    for row_number, row in enumerate(plan_rows, start=2):
        context = f"{REPLAY_PLAN}:{row_number}"
        family = row["family_id"]
        require(family in profiles, f"{context}: family missing from compatibility manifest")
        profile = profiles[family]
        require(row["release_profile"] == profile["release_profile"],
                f"{context}: release profile drift")
        require(row["profile_class"] == profile["profile_class"],
                f"{context}: profile class drift")
        for source_id in split_semicolon(row["source_ids"]):
            require(source_id in sources, f"{context}: unknown source_id {source_id}")
            source_ids_seen.add(source_id)
            source = sources[source_id]
            require(source["repo_url"].startswith("https://"),
                    f"{context}: source {source_id} repo_url must be https")
            require(source["upstream_ref"], f"{context}: source {source_id} upstream_ref is empty")
            require(source["release_version"], f"{context}: source {source_id} release_version is empty")
            require(source["runner_tools"], f"{context}: source {source_id} runner_tools is empty")
        harness_root = repo_root / row["harness_root"]
        require(harness_root.is_dir(), f"{context}: harness root missing: {harness_root}")
        suite_rows += validate_upstream_manifest(harness_root)
        exclusion_rows += validate_exclusions(harness_root)
        native_tool_rows += validate_harness(repo_root, family, harness_root)
        family_count += 1

    digest_material = {
        "plan": plan_rows,
        "source_ids_seen": sorted(source_ids_seen),
    }
    digest = hashlib.sha256(
        json.dumps(digest_material, sort_keys=True, separators=(",", ":")).encode("utf-8")
    ).hexdigest()
    return {
        "gate": "reference_suite_scope_drift_gate",
        "status": "passed",
        "family_count": family_count,
        "suite_rows": suite_rows,
        "exclusion_rows": exclusion_rows,
        "native_tool_rows": native_tool_rows,
        "source_ids_seen": sorted(source_ids_seen),
        "digest": digest,
        "authority_policy": "reference_tools_feed_parser_routes_only_engine_mga_security_storage_authority",
    }


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True, type=pathlib.Path)
    parser.add_argument("--evidence-file", type=pathlib.Path)
    args = parser.parse_args(argv)

    payload = validate(args.repo_root.resolve())
    if args.evidence_file:
        args.evidence_file.parent.mkdir(parents=True, exist_ok=True)
        args.evidence_file.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n",
                                      encoding="utf-8")
    print(
        "reference_suite_scope_drift_gate=passed "
        f"families={payload['family_count']} "
        f"suite_rows={payload['suite_rows']} "
        f"native_tool_rows={payload['native_tool_rows']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
