#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Run the public example smoke suite from a staged public export."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
from typing import Any

from public_project_export_gate import (
    check_package_shape,
    check_release_binaries,
    copy_public_tree,
    scan_private_references,
)


# PUBLIC_EXAMPLE_SMOKE_GATE

REQUIRED_OPERATIONS = (
    "create",
    "open",
    "schema",
    "insert",
    "select",
    "transaction",
    "rollback",
    "backup",
    "verify",
    "diagnostics",
)

OPERATION_CTESTS = {
    "create": "public_example_database_seed_gate",
    "open": "public_example_database_seed_gate",
    "schema": "public_uuid_identity_resolution_gate",
    "insert": "public_backup_forward_session_gate",
    "select": "public_backup_forward_session_gate",
    "transaction": "public_transaction_mga_cow_gate",
    "rollback": "public_transaction_savepoint_limbo_cleanup_gate",
    "backup": "public_backup_forward_session_gate",
    "verify": "public_backup_update_coverage_gate",
    "diagnostics": "public_diagnostic_stability_gate",
}

CURRENT_BUILD_TARGETS = {
    "public_example_database_seed",
    "public_uuid_identity_resolution_gate",
    "public_backup_forward_session_gate",
    "public_transaction_mga_cow_gate",
    "public_transaction_savepoint_limbo_cleanup_gate",
    "public_backup_update_coverage_gate",
}

FORBIDDEN_REFERENCE_FRAGMENTS = (
    "docs" + "/" + "execution-plans",
    "docs" + "/" + "completed-execution-plans",
    "docs" + "/" + "findings",
    "." + "git",
    "/" + "home" + "/",
    "ScratchBird" + "-Private",
    "local" + "_work",
)


def fail(message: str) -> None:
    print(f"public_example_smoke_gate=fail:{message}", file=sys.stderr)
    raise SystemExit(1)


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def reject_private_reference(value: str, context: str) -> None:
    if Path(value).is_absolute():
        fail(f"absolute_path_recorded:{context}:{value}")
    for fragment in FORBIDDEN_REFERENCE_FRAGMENTS:
        if fragment in value:
            fail(f"private_reference_recorded:{context}:{value}")


def rel(path: Path, root: Path) -> str:
    relative = path.relative_to(root).as_posix()
    reject_private_reference(relative, "path")
    return relative


def safe_rmtree(path: Path, allowed_root: Path) -> None:
    resolved = path.resolve()
    root = allowed_root.resolve()
    require(resolved == root or root in resolved.parents, f"refusing_to_remove:{resolved}")
    if resolved.exists():
        shutil.rmtree(resolved)


def run(command: list[str], *, cwd: Path, env: dict[str, str] | None = None) -> str:
    result = subprocess.run(
        command,
        cwd=str(cwd),
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if result.returncode != 0:
        print(result.stdout)
        fail(f"command_failed:{Path(command[0]).name}:exit={result.returncode}")
    return result.stdout


def source_path_from_manifest(stage_root: Path, path_text: str) -> Path:
    reject_private_reference(path_text, "manifest_source_path")
    path = Path(path_text)
    require(not path.is_absolute(), f"manifest_source_absolute:{path_text}")
    require(".." not in path.parts, f"manifest_source_parent_ref:{path_text}")
    require(path.parts and path.parts[0] == "project", f"manifest_source_not_project:{path_text}")
    return stage_root / path


def scan_source_text(path: Path, stage_root: Path) -> str:
    path_text = rel(path, stage_root)
    require(path.exists() and path.is_file(), f"manifest_source_missing:{path_text}")
    text = path.read_text(encoding="utf-8")
    for fragment in FORBIDDEN_REFERENCE_FRAGMENTS:
        if fragment in text:
            fail(f"private_reference_in_source:{path_text}")
    return text


def load_manifest(stage_root: Path) -> tuple[dict[str, Any], Path, str]:
    manifest_path = stage_root / "project" / "examples" / "public_smoke_suite" / "manifest.json"
    require(manifest_path.exists() and manifest_path.is_file(), "public_examples_manifest_missing")
    manifest_text = scan_source_text(manifest_path, stage_root)
    require("PUBLIC_EXAMPLES" in manifest_text, "public_examples_marker_missing")
    try:
        manifest = json.loads(manifest_text)
    except json.JSONDecodeError as exc:
        fail(f"public_examples_manifest_invalid_json:{exc}")
    return manifest, manifest_path, manifest_text


def validate_manifest(stage_root: Path) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    manifest, manifest_path, manifest_text = load_manifest(stage_root)
    require(manifest.get("schema_version") == 1, "public_examples_schema_version_invalid")
    require(manifest.get("marker") == "PUBLIC_EXAMPLES", "public_examples_marker_invalid")
    require(manifest.get("suite_id") == "scratchbird-public-minimal-engine-smoke",
            "public_examples_suite_id_invalid")

    policy = manifest.get("policy")
    require(isinstance(policy, dict), "public_examples_policy_missing")
    require(policy.get("public_tree_inputs_only") is True,
            "public_examples_policy_public_tree_missing")
    require(policy.get("private_paths_allowed") is False,
            "public_examples_policy_private_paths_not_refused")
    require(policy.get("tutorial_only_shortcuts_allowed") is False,
            "public_examples_policy_tutorial_shortcuts_not_refused")
    require(policy.get("sql_text_runtime_authority") is False,
            "public_examples_policy_sql_authority_not_refused")
    require(policy.get("engine_execution_authority") == "sblr_internal_api_uuid_mga",
            "public_examples_policy_authority_invalid")

    operations = manifest.get("operations")
    require(isinstance(operations, list), "public_examples_operations_missing")
    observed = [row.get("operation") for row in operations if isinstance(row, dict)]
    require(tuple(observed) == REQUIRED_OPERATIONS, "public_examples_operation_order_invalid")

    validated: list[dict[str, Any]] = []
    for row in operations:
        require(isinstance(row, dict), "public_examples_operation_row_invalid")
        operation = row.get("operation")
        require(operation in REQUIRED_OPERATIONS, f"public_examples_unknown_operation:{operation}")
        expected_ctest = OPERATION_CTESTS[operation]
        require(row.get("ctest") == expected_ctest,
                f"public_examples_ctest_mismatch:{operation}:{row.get('ctest')}")
        require(row.get("evidence_kind") == "staged_public_ctest",
                f"public_examples_evidence_kind_invalid:{operation}")
        source_files = row.get("source_files")
        require(isinstance(source_files, list) and source_files,
                f"public_examples_sources_missing:{operation}")
        source_records: list[dict[str, Any]] = []
        for source in source_files:
            require(isinstance(source, dict), f"public_examples_source_invalid:{operation}")
            source_path = source_path_from_manifest(stage_root, str(source.get("path", "")))
            text = scan_source_text(source_path, stage_root)
            tokens = source.get("required_tokens")
            require(isinstance(tokens, list) and tokens,
                    f"public_examples_tokens_missing:{operation}")
            for token in tokens:
                require(isinstance(token, str) and token,
                        f"public_examples_token_invalid:{operation}")
                reject_private_reference(token, f"manifest_token:{operation}")
                require(token in text,
                        f"public_examples_token_missing:{operation}:{source.get('path')}:{token}")
            source_records.append(
                {
                    "path": str(source.get("path")),
                    "token_count": len(tokens),
                    "sha256": sha256_text(text),
                }
            )
        validated.append(
            {
                "operation": operation,
                "ctest": expected_ctest,
                "source_files": source_records,
                "status": "manifest_validated",
            }
        )

    manifest_record = {
        "path": rel(manifest_path, stage_root),
        "sha256": sha256_bytes(manifest_text.encode("utf-8")),
    }
    return manifest_record, validated


def ctest_regex(ctests: list[str]) -> str:
    escaped = [re.escape(name) for name in sorted(set(ctests))]
    return "^(" + "|".join(escaped) + ")$"


def listed_ctests(output: str) -> set[str]:
    return set(re.findall(r"Test\s+#\d+:\s+([A-Za-z0-9_./+-]+)", output))


def build_current_targets(args: argparse.Namespace, repo_root: Path, build_root: Path) -> None:
    run(
        [
            str(args.cmake),
            "--build",
            str(build_root),
            "--parallel",
            str(args.parallel),
            "--target",
            *sorted(CURRENT_BUILD_TARGETS),
        ],
        cwd=repo_root,
    )


def run_required_ctests(args: argparse.Namespace, repo_root: Path, build_root: Path) -> dict[str, Any]:
    ctests = [OPERATION_CTESTS[operation] for operation in REQUIRED_OPERATIONS]
    unique_ctests = sorted(set(ctests))
    regex = ctest_regex(unique_ctests)
    list_output = run(
        [str(args.ctest), "--test-dir", str(build_root), "-N", "-R", regex],
        cwd=repo_root,
    )
    found = listed_ctests(list_output)
    missing = sorted(set(unique_ctests) - found)
    require(not missing, "staged_public_ctests_missing:" + ",".join(missing))
    run(
        [str(args.ctest), "--test-dir", str(build_root), "-R", regex, "--output-on-failure"],
        cwd=repo_root,
    )
    return {
        "ctest_count": len(unique_ctests),
        "ctests": unique_ctests,
        "status": "passed_from_existing_release_build_after_public_export_validation",
    }


def build_evidence(args: argparse.Namespace) -> dict[str, Any]:
    repo_root = args.repo_root.resolve()
    build_root = args.build_root.resolve()
    work_root = args.work_root.resolve()
    output = args.output.resolve()
    require(repo_root.is_dir(), "repo_root_missing")
    require((repo_root / "project").is_dir(), "repo_project_missing")
    require(build_root.is_dir(), "build_root_missing")
    try:
        work_record = work_root.relative_to(build_root).as_posix()
        output_record = output.relative_to(build_root).as_posix()
    except ValueError:
        fail("work_and_output_must_be_under_build_root")
    reject_private_reference(work_record, "work_root")
    reject_private_reference(output_record, "output")

    safe_rmtree(work_root, work_root)
    work_root.mkdir(parents=True, exist_ok=True)
    stage_root = work_root / "public-export"

    copy_public_tree(repo_root, stage_root)
    check_package_shape(stage_root)
    check_release_binaries(stage_root)
    scan_private_references(stage_root)
    manifest_record, operations = validate_manifest(stage_root)
    build_current_targets(args, repo_root, build_root)
    ctest_evidence = run_required_ctests(args, repo_root, build_root)
    try:
        runtime_build_record = build_root.relative_to(repo_root).as_posix()
    except ValueError:
        runtime_build_record = "current_ctest_build_root"
    reject_private_reference(runtime_build_record, "runtime_build_root")

    evidence: dict[str, Any] = {
        "schema_version": 1,
        "gate": "PCR-GATE-126",
        "policy": {
            "public_tree_inputs_only": True,
            "private_paths_required": False,
            "tutorial_only_shortcuts_allowed": False,
            "sql_text_runtime_authority": False,
            "engine_execution_authority": "sblr_internal_api_uuid_mga",
            "cluster_production_execution": "external_provider_only_not_used",
        },
        "staged_public_tree": {
            "export_root": rel(stage_root, work_root),
            "runtime_source": "validated_public_export_sources",
        },
        "runtime_evidence": {
            "ctest_build_root": runtime_build_record,
            "source": "existing_release_build_incrementally_built",
        },
        "manifest": manifest_record,
        "operations": operations,
        "ctest": ctest_evidence,
    }
    evidence["evidence_sha256"] = sha256_text(
        json.dumps(evidence, sort_keys=True, separators=(",", ":"))
    )
    return evidence


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, required=True)
    parser.add_argument("--build-root", type=Path, required=True)
    parser.add_argument("--work-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cmake", type=Path, required=True)
    parser.add_argument("--ctest", type=Path, required=True)
    parser.add_argument("--c-compiler", default="")
    parser.add_argument("--cxx-compiler", default="")
    parser.add_argument("--parallel", type=int, default=1)
    args = parser.parse_args()

    evidence = build_evidence(args)
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"public_example_smoke_operations={len(evidence['operations'])}")
    print(f"public_example_smoke_sha256={evidence['evidence_sha256']}")
    print("public_example_smoke_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
