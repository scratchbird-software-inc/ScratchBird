#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Check reproducibility of public release proof artifacts."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
from typing import Any


# PUBLIC_REPRODUCIBLE_EXPORT

FORBIDDEN_REFERENCE_FRAGMENTS = (
    "docs" + "/" + "execution-plans",
    "docs" + "/" + "completed-execution-plans",
    "docs" + "/" + "findings",
    "." + "git",
    "/" + "home" + "/",
    "ScratchBird" + "-Private",
    "local" + "_work",
)

RUN_IDS = ("run_a", "run_b")


def fail(message: str) -> None:
    print(f"public_reproducible_export=fail:{message}", file=sys.stderr)
    raise SystemExit(1)


def reject_private_reference(value: str, context: str) -> None:
    if Path(value).is_absolute():
        fail(f"absolute_path_recorded:{context}:{value}")
    for fragment in FORBIDDEN_REFERENCE_FRAGMENTS:
        if fragment in value:
            fail(f"private_reference_recorded:{context}:{value}")


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def relative_to_build(path: Path, build_root: Path) -> str:
    try:
        value = path.resolve().relative_to(build_root.resolve()).as_posix()
    except ValueError:
        fail(f"path_outside_build_root:{path.name}")
    reject_private_reference(value, "build_output")
    return value


def load_export_module(repo_root: Path) -> Any:
    module_path = repo_root / "project" / "tools" / "release" / "public_project_export_gate.py"
    spec = importlib.util.spec_from_file_location("public_project_export_gate_repro", module_path)
    if spec is None or spec.loader is None:
        fail("public_project_export_gate_module_unavailable")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def run_command(command: list[str], cwd: Path, label: str) -> None:
    completed = subprocess.run(
        command,
        cwd=cwd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if completed.returncode != 0:
        if completed.stdout:
            print(completed.stdout)
        fail(f"generator_failed:{label}:{completed.returncode}")


def write_deterministic_example(stage_root: Path) -> None:
    example_root = stage_root / "data" / "example"
    example_root.mkdir(parents=True, exist_ok=True)
    (example_root / "scratchbird-example.sbdb").write_bytes(
        b"SCRATCHBIRD_PUBLIC_REPRODUCIBLE_EXPORT_EXAMPLE\n"
    )
    (example_root / "scratchbird-example.manifest.json").write_text(
        json.dumps(
            {
                "schema_version": 1,
                "profile": "public_reproducibility_fixture",
                "authority": "release_evidence_only",
            },
            indent=2,
            sort_keys=True,
        )
        + "\n",
        encoding="utf-8",
    )


def run_cleanup_export_pair(repo_root: Path, work_root: Path, export_module: Any) -> dict[str, Path]:
    stage_root = work_root / "public-export"
    external_file_list = work_root / "public-export-file-list.txt"
    external_cleanup_manifest = work_root / "public-export-cleanup-manifest.json"

    export_module.copy_public_tree(repo_root, stage_root)
    export_module.check_package_shape(stage_root)
    export_module.scan_private_references(stage_root)
    write_deterministic_example(stage_root)
    export_module.check_release_binaries(stage_root)
    export_module.write_cleanup_outputs(stage_root, external_file_list, external_cleanup_manifest)
    export_module.check_package_shape(stage_root)
    export_module.check_release_binaries(stage_root)
    export_module.scan_private_references(stage_root)
    return {
        "cleanup_manifest": external_cleanup_manifest,
        "package_file_list": external_file_list,
    }


def run_generated_artifacts(args: argparse.Namespace, run_root: Path) -> dict[str, Path]:
    outputs = {
        "platform_matrix": run_root / "public_platform_matrix.json",
        "diagnostic_matrix": run_root / "public_diagnostic_matrix.csv",
        "proof_list": run_root / "public_proof_migration.json",
        "version_metadata": run_root / "public_release_version_metadata.json",
        "sbom": run_root / "public_dependency_sbom.json",
        "doc_consistency": run_root / "public_doc_consistency.json",
    }

    common_roots = [
        "--repo-root",
        str(args.repo_root.resolve()),
        "--project-root",
        str(args.project_root.resolve()),
    ]
    run_command(
        [
            args.python,
            str(args.project_root / "tools" / "release" / "public_platform_matrix_gate.py"),
            *common_roots,
            "--build-root",
            str(args.build_root.resolve()),
            "--output",
            str(outputs["platform_matrix"]),
            "--configured-system-name",
            args.configured_system_name,
            "--configured-system-processor",
            args.configured_system_processor,
            "--configured-cxx-compiler-id",
            args.configured_cxx_compiler_id,
            "--configured-cxx-compiler-version",
            args.configured_cxx_compiler_version,
            "--configured-cmake-version",
            args.configured_cmake_version,
            "--configured-cxx-standard",
            args.configured_cxx_standard,
            "--cluster-provider-enabled",
            args.cluster_provider_enabled,
            "--cluster-provider-stub",
            args.cluster_provider_stub,
            "--cluster-provider-external-library",
            args.cluster_provider_external_library,
        ],
        args.repo_root,
        "platform_matrix",
    )
    run_command(
        [
            args.python,
            str(args.project_root / "tools" / "release" / "public_proof_migration_gate.py"),
            *common_roots,
            "--build-root",
            str(args.build_root.resolve()),
            "--output",
            str(outputs["proof_list"]),
        ],
        args.repo_root,
        "proof_list",
    )
    run_command(
        [
            args.python,
            str(args.project_root / "tools" / "release" / "public_diagnostic_matrix_generator.py"),
            "--project-root",
            str(args.project_root.resolve()),
            "--matrix-output",
            str(outputs["diagnostic_matrix"]),
            "--evidence-output",
            str(run_root / "public_diagnostic_matrix.json"),
        ],
        args.repo_root,
        "diagnostic_matrix",
    )
    run_command(
        [
            args.version_metadata_tool,
            "--self-test",
            "--out",
            str(outputs["version_metadata"]),
        ],
        args.repo_root,
        "version_metadata",
    )
    run_command(
        [
            args.python,
            str(args.project_root / "tools" / "release" / "public_dependency_sbom.py"),
            *common_roots,
            "--build-root",
            str(args.build_root.resolve()),
            "--output",
            str(outputs["sbom"]),
        ],
        args.repo_root,
        "sbom",
    )
    run_command(
        [
            args.python,
            str(args.project_root / "tools" / "release" / "public_doc_consistency_check.py"),
            *common_roots,
            "--output",
            str(outputs["doc_consistency"]),
        ],
        args.repo_root,
        "doc_consistency",
    )
    return outputs


def artifact_bytes(path: Path) -> bytes:
    if path.suffix == ".json":
        payload = json.loads(path.read_text(encoding="utf-8"))
        return (json.dumps(payload, sort_keys=True, separators=(",", ":")) + "\n").encode("utf-8")
    return path.read_bytes()


def compare_artifacts(
    first: dict[str, Path],
    second: dict[str, Path],
    build_root: Path,
) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    names = sorted(first)
    if names != sorted(second):
        fail("artifact_name_set_mismatch")
    for name in names:
        first_path = first[name]
        second_path = second[name]
        first_bytes = artifact_bytes(first_path)
        second_bytes = artifact_bytes(second_path)
        if first_bytes != second_bytes:
            fail(f"artifact_digest_mismatch:{name}")
        rows.append(
            {
                "artifact": name,
                "first_output": relative_to_build(first_path, build_root),
                "second_output": relative_to_build(second_path, build_root),
                "canonical_sha256": sha256_bytes(first_bytes),
                "raw_sha256": sha256_file(first_path),
                "bytes": len(first_bytes),
                "status": "reproducible",
            }
        )
    return rows


def build_evidence(args: argparse.Namespace) -> dict[str, Any]:
    repo_root = args.repo_root.resolve()
    project_root = args.project_root.resolve()
    build_root = args.build_root.resolve()
    work_root = args.work_root.resolve()
    if not repo_root.is_dir() or not project_root.is_dir() or not build_root.is_dir():
        fail("input_root_missing")
    if project_root != repo_root / "project":
        fail("project_root_must_be_repo_project")
    if not Path(args.version_metadata_tool).exists():
        fail("version_metadata_tool_missing")
    relative_to_build(work_root, build_root)

    export_module = load_export_module(repo_root)
    export_module.rmtree_public(work_root)
    work_root.mkdir(parents=True)
    run_outputs: dict[str, dict[str, Path]] = {}
    for run_id in RUN_IDS:
        run_root = work_root / run_id
        run_root.mkdir(parents=True)
        cleanup_outputs = run_cleanup_export_pair(repo_root, run_root / "cleanup", export_module)
        generated_outputs = run_generated_artifacts(args, run_root / "generated")
        run_outputs[run_id] = {**cleanup_outputs, **generated_outputs}

    comparisons = compare_artifacts(run_outputs["run_a"], run_outputs["run_b"], build_root)
    evidence: dict[str, Any] = {
        "schema_version": 1,
        "gate": "PCR-GATE-118",
        "policy": {
            "public_tree_inputs_only": True,
            "private_docs_required": False,
            "git_history_required": False,
            "release_proof_is_evidence_only": True,
            "deterministic_two_pass_generation": True,
        },
        "artifact_count": len(comparisons),
        "compared_artifacts": comparisons,
    }
    evidence["evidence_sha256"] = sha256_bytes(
        json.dumps(evidence, sort_keys=True, separators=(",", ":")).encode("utf-8")
    )
    return evidence


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, required=True)
    parser.add_argument("--project-root", type=Path, required=True)
    parser.add_argument("--build-root", type=Path, required=True)
    parser.add_argument("--work-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--python", required=True)
    parser.add_argument("--version-metadata-tool", required=True)
    parser.add_argument("--configured-system-name", required=True)
    parser.add_argument("--configured-system-processor", required=True)
    parser.add_argument("--configured-cxx-compiler-id", required=True)
    parser.add_argument("--configured-cxx-compiler-version", required=True)
    parser.add_argument("--configured-cmake-version", required=True)
    parser.add_argument("--configured-cxx-standard", required=True)
    parser.add_argument("--cluster-provider-enabled", required=True)
    parser.add_argument("--cluster-provider-stub", required=True)
    parser.add_argument("--cluster-provider-external-library", default="")
    args = parser.parse_args()

    evidence = build_evidence(args)
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"public_reproducible_export_output={relative_to_build(output, args.build_root)}")
    print(f"public_reproducible_export_sha256={evidence['evidence_sha256']}")
    print("public_reproducible_export=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
