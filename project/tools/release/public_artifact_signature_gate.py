#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Generate and verify public release artifact checksum/signature metadata."""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
from pathlib import Path
import shutil
import sys
from typing import Any

from public_project_export_gate import (
    check_package_shape,
    check_release_binaries,
    copy_public_tree,
    rmtree_public,
    scan_private_references,
    write_cleanup_outputs,
)


# PUBLIC_RELEASE_ARTIFACT_SIGNING

REQUIRED_RELEASE_PLATFORMS = ("linux", "windows", "freebsd")
FORBIDDEN_REFERENCE_FRAGMENTS = (
    "docs" + "/" + "execution-plans",
    "docs" + "/" + "completed-execution-plans",
    "docs" + "/" + "findings",
    "." + "git",
    "/" + "home" + "/",
    "ScratchBird" + "-Private",
    "local" + "_work",
)

REQUIRED_SOURCE_TOKENS: dict[str, tuple[str, ...]] = {
    "tools/release/public_artifact_signature_gate.py": (
        "PUBLIC_RELEASE_ARTIFACT_SIGNING",
        "signature-ready-ed25519",
        "RELEASE.ARTIFACT.CHECKSUM_MISMATCH",
    ),
    "tools/release/public_reproducible_export.py": (
        "PUBLIC_REPRODUCIBLE_EXPORT",
        "canonical_sha256",
        "raw_sha256",
    ),
    "tools/release/public_dependency_sbom.py": (
        "signature_ready_artifact",
        "sha256_file",
        "public_dependency_sbom=passed",
    ),
    "tests/release/public_crypto_entropy_policy_gate.py": (
        "approved_hash",
        "approved_mac",
        "signature_ready_metadata",
        "weak_checksums_authority",
    ),
    "tests/release/CMakeLists.txt": (
        "PUBLIC_ARTIFACT_SIGNATURE_GATE",
        "public_artifact_signature_gate",
        "PCR-GATE-127",
    ),
}


def fail(message: str) -> None:
    print(f"public_artifact_signature_gate=fail:{message}", file=sys.stderr)
    raise SystemExit(1)


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def reject_private_reference(value: str, context: str) -> None:
    if Path(value).is_absolute():
        fail(f"absolute_path_recorded:{context}:{value}")
    for fragment in FORBIDDEN_REFERENCE_FRAGMENTS:
        if fragment in value:
            fail(f"private_reference_recorded:{context}:{value}")


def private_reference_present(value: str) -> bool:
    if Path(value).is_absolute():
        return True
    for fragment in FORBIDDEN_REFERENCE_FRAGMENTS:
        if fragment in value:
            return True
    return False


def rel(path: Path, root: Path, context: str) -> str:
    value = path.resolve().relative_to(root.resolve()).as_posix()
    reject_private_reference(value, context)
    return value


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def safe_rmtree(path: Path, allowed_root: Path) -> None:
    resolved = path.resolve()
    root = allowed_root.resolve()
    require(resolved == root or root in resolved.parents, f"refusing_to_remove:{resolved}")
    rmtree_public(resolved)


def write_deterministic_example(stage_root: Path) -> None:
    example_root = stage_root / "data" / "example"
    example_root.mkdir(parents=True, exist_ok=True)
    (example_root / "scratchbird-example.sbdb").write_bytes(
        b"SCRATCHBIRD_PUBLIC_ARTIFACT_SIGNATURE_FIXTURE\n"
    )
    (example_root / "scratchbird-example.manifest.json").write_text(
        json.dumps(
            {
                "schema_version": 1,
                "profile": "public_artifact_signature_fixture",
                "authority": "release_evidence_only",
            },
            indent=2,
            sort_keys=True,
        )
        + "\n",
        encoding="utf-8",
    )


def scan_required_source_tokens(project_root: Path) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for relative, tokens in REQUIRED_SOURCE_TOKENS.items():
        reject_private_reference(relative, "source_token_path")
        path = project_root / relative
        require(path.is_file(), f"source_token_file_missing:{relative}")
        text = path.read_text(encoding="utf-8")
        for token in tokens:
            reject_private_reference(token, f"source_token:{relative}")
            require(token in text, f"source_token_missing:{relative}:{token}")
        rows.append(
            {
                "path": relative,
                "token_count": len(tokens),
                "source_sha256": sha256_text(text),
                "status": "present",
            }
        )
    return rows


def stage_public_export(repo_root: Path, work_root: Path) -> dict[str, Path]:
    stage_root = work_root / "public-export"
    external_file_list = work_root / "public-export-file-list.txt"
    external_cleanup_manifest = work_root / "public-export-cleanup-manifest.json"

    copy_public_tree(repo_root, stage_root)
    check_package_shape(stage_root)
    scan_private_references(stage_root)
    write_deterministic_example(stage_root)
    check_release_binaries(stage_root)
    write_cleanup_outputs(stage_root, external_file_list, external_cleanup_manifest)
    check_package_shape(stage_root)
    check_release_binaries(stage_root)
    scan_private_references(stage_root)
    return {
        "stage_root": stage_root,
        "external_file_list": external_file_list,
        "external_cleanup_manifest": external_cleanup_manifest,
    }


def artifact_path_record(path: Path, stage_root: Path, work_root: Path) -> str:
    try:
        value = path.resolve().relative_to(stage_root.resolve()).as_posix()
    except ValueError:
        value = "work/" + rel(path, work_root, "artifact_work_path")
    reject_private_reference(value, "artifact_path")
    return value


def collect_artifacts(stage_root: Path, work_root: Path, staged: dict[str, Path]) -> list[dict[str, Any]]:
    candidates = [
        stage_root / "LICENSE",
        stage_root / "NOTICE",
        stage_root / "release" / "metadata" / "public-export-cleanup-manifest.json",
        stage_root / "release" / "metadata" / "public-export-file-list.txt",
        staged["external_cleanup_manifest"],
        staged["external_file_list"],
    ]
    for platform_id in REQUIRED_RELEASE_PLATFORMS:
        candidates.append(stage_root / "release" / platform_id / "ENGINE_BINARY_LAYOUT.json")

    artifacts: list[dict[str, Any]] = []
    seen: set[str] = set()
    for path in candidates:
        require(path.exists() and path.is_file(), f"artifact_missing:{path.name}")
        artifact_path = artifact_path_record(path, stage_root, work_root)
        if artifact_path in seen:
            continue
        seen.add(artifact_path)
        artifacts.append(
            {
                "artifact_path": artifact_path,
                "sha256": sha256_file(path),
                "bytes": path.stat().st_size,
                "checksum_algorithm": "sha256",
            }
        )
    return sorted(artifacts, key=lambda item: item["artifact_path"])


def checksum_text(artifacts: list[dict[str, Any]]) -> str:
    lines = [
        f"sha256 {artifact['sha256']} {artifact['artifact_path']}"
        for artifact in artifacts
    ]
    return "\n".join(lines) + "\n"


def signing_policy(checksums_digest: str, artifact_count: int) -> dict[str, Any]:
    return {
        "schema_version": 1,
        "marker": "PUBLIC_RELEASE_ARTIFACT_SIGNING",
        "checksum_algorithm": "sha256",
        "signature_algorithm": "signature-ready-ed25519",
        "approved_mac": "hmac-sha256",
        "signing_mode": "offline_release_owner",
        "signature_key_material_embedded": False,
        "signature_envelope_materialized": False,
        "public_tree_inputs_only": True,
        "artifact_count": artifact_count,
        "checksums_sha256": checksums_digest,
        "diagnostic_contract": {
            "checksum_mismatch": "RELEASE.ARTIFACT.CHECKSUM_MISMATCH",
            "weak_checksum": "RELEASE.ARTIFACT.WEAK_CHECKSUM_REFUSED",
            "missing_signing_policy": "RELEASE.ARTIFACT.SIGNING_POLICY_MISSING",
            "private_input": "RELEASE.ARTIFACT.PRIVATE_INPUT_REFUSED",
        },
    }


def write_release_artifacts(
    signing_root: Path,
    artifacts: list[dict[str, Any]],
) -> dict[str, Path]:
    signing_root.mkdir(parents=True, exist_ok=True)
    checksums = checksum_text(artifacts)
    checksums_path = signing_root / "public-release-artifact-checksums.txt"
    checksums_path.write_text(checksums, encoding="utf-8")
    policy = signing_policy(sha256_text(checksums), len(artifacts))
    policy_path = signing_root / "public-release-artifact-signing-policy.json"
    policy_path.write_text(json.dumps(policy, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    manifest = {
        "schema_version": 1,
        "marker": "PUBLIC_RELEASE_ARTIFACT_SIGNING",
        "artifacts": artifacts,
        "checksums": artifact_path_record(checksums_path, signing_root, signing_root),
        "signing_policy": artifact_path_record(policy_path, signing_root, signing_root),
    }
    manifest_path = signing_root / "public-release-artifact-manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return {
        "checksums": checksums_path,
        "signing_policy": policy_path,
        "manifest": manifest_path,
    }


def parse_checksums(text: str) -> dict[str, str]:
    checksums: dict[str, str] = {}
    for raw in text.splitlines():
        if not raw:
            continue
        parts = raw.split(" ", 2)
        if len(parts) != 3:
            return {}
        algorithm, digest, artifact_path = parts
        if algorithm != "sha256":
            return {}
        if private_reference_present(artifact_path):
            return {"__private_reference__": artifact_path}
        checksums[artifact_path] = digest
    return checksums


def verify_bundle(
    artifacts: list[dict[str, Any]],
    checksums: str,
    policy: dict[str, Any],
) -> tuple[bool, str]:
    if policy.get("checksum_algorithm") != "sha256":
        return False, "RELEASE.ARTIFACT.WEAK_CHECKSUM_REFUSED"
    if policy.get("signature_algorithm") != "signature-ready-ed25519":
        return False, "RELEASE.ARTIFACT.SIGNING_POLICY_MISSING"
    if policy.get("signature_key_material_embedded") is not False:
        return False, "RELEASE.ARTIFACT.SIGNING_POLICY_MISSING"
    parsed = parse_checksums(checksums)
    if not parsed:
        return False, "RELEASE.ARTIFACT.CHECKSUM_MANIFEST_INVALID"
    if "__private_reference__" in parsed:
        return False, "RELEASE.ARTIFACT.PRIVATE_INPUT_REFUSED"
    expected_paths = {artifact["artifact_path"] for artifact in artifacts}
    if set(parsed) != expected_paths:
        return False, "RELEASE.ARTIFACT.CHECKSUM_MANIFEST_INVALID"
    for artifact in artifacts:
        if parsed[artifact["artifact_path"]] != artifact["sha256"]:
            return False, "RELEASE.ARTIFACT.CHECKSUM_MISMATCH"
    return True, "RELEASE.ARTIFACT.OK"


def verify_failure_cases(
    artifacts: list[dict[str, Any]],
    checksums: str,
    policy: dict[str, Any],
) -> list[dict[str, str]]:
    cases: list[tuple[str, str, str, dict[str, Any]]] = []
    first = artifacts[0]
    corrupt_checksums = checksums.replace(first["sha256"], "0" * 64, 1)
    cases.append(("checksum_mismatch", "RELEASE.ARTIFACT.CHECKSUM_MISMATCH", corrupt_checksums, policy))
    weak_policy = copy.deepcopy(policy)
    weak_policy["checksum_algorithm"] = "crc32"
    cases.append(("weak_checksum", "RELEASE.ARTIFACT.WEAK_CHECKSUM_REFUSED", checksums, weak_policy))
    missing_signature = copy.deepcopy(policy)
    missing_signature.pop("signature_algorithm", None)
    cases.append(("missing_signing_policy", "RELEASE.ARTIFACT.SIGNING_POLICY_MISSING", checksums, missing_signature))
    private_checksums = (
        checksums + "sha256 " + ("1" * 64) + " "
        + "docs" + "/" + "execution-plans" + "/private.csv\n"
    )
    cases.append(("private_input", "RELEASE.ARTIFACT.PRIVATE_INPUT_REFUSED", private_checksums, policy))

    rows: list[dict[str, str]] = []
    for case_id, expected, case_checksums, case_policy in cases:
        ok, diagnostic = verify_bundle(artifacts, case_checksums, case_policy)
        require(not ok and diagnostic == expected, f"failure_case_diagnostic_mismatch:{case_id}:{diagnostic}")
        rows.append({"case_id": case_id, "diagnostic": diagnostic, "status": "fail_closed"})
    return rows


def build_evidence(args: argparse.Namespace) -> dict[str, Any]:
    repo_root = args.repo_root.resolve()
    project_root = args.project_root.resolve()
    build_root = args.build_root.resolve()
    work_root = args.work_root.resolve()
    require(repo_root.is_dir() and project_root == repo_root / "project", "repo_project_root_invalid")
    require(build_root.is_dir(), "build_root_missing")
    try:
        work_record = work_root.relative_to(build_root).as_posix()
    except ValueError:
        fail("work_root_must_be_under_build_root")
    reject_private_reference(work_record, "work_root")
    safe_rmtree(work_root, work_root)
    work_root.mkdir(parents=True, exist_ok=True)

    source_rows = scan_required_source_tokens(project_root)
    staged = stage_public_export(repo_root, work_root)
    stage_root = staged["stage_root"]
    artifacts = collect_artifacts(stage_root, work_root, staged)
    output_paths = write_release_artifacts(work_root / "signing", artifacts)
    checksums = output_paths["checksums"].read_text(encoding="utf-8")
    policy = json.loads(output_paths["signing_policy"].read_text(encoding="utf-8"))
    ok, diagnostic = verify_bundle(artifacts, checksums, policy)
    require(ok and diagnostic == "RELEASE.ARTIFACT.OK", f"positive_verification_failed:{diagnostic}")
    failure_rows = verify_failure_cases(artifacts, checksums, policy)

    evidence: dict[str, Any] = {
        "schema_version": 1,
        "gate": "PCR-GATE-127",
        "marker": "PUBLIC_RELEASE_ARTIFACT_SIGNING",
        "policy": {
            "public_tree_inputs_only": True,
            "private_docs_required": False,
            "approved_hash": "sha256",
            "approved_mac": "hmac-sha256",
            "signature_ready_metadata": "signature-ready-ed25519",
            "signature_key_material_embedded": False,
            "release_metadata_authority": "evidence_only",
        },
        "source_rows": source_rows,
        "artifact_count": len(artifacts),
        "artifacts": artifacts,
        "generated_outputs": {
            key: rel(path, build_root, "generated_output")
            for key, path in output_paths.items()
        },
        "verification": {
            "positive": diagnostic,
            "failure_cases": failure_rows,
        },
    }
    evidence["evidence_sha256"] = sha256_text(
        json.dumps(evidence, sort_keys=True, separators=(",", ":"))
    )
    return evidence


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, required=True)
    parser.add_argument("--project-root", type=Path, required=True)
    parser.add_argument("--build-root", type=Path, required=True)
    parser.add_argument("--work-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    evidence = build_evidence(args)
    output = args.output.resolve()
    try:
        output_record = output.relative_to(args.build_root.resolve()).as_posix()
    except ValueError:
        fail("output_must_be_under_build_root")
    reject_private_reference(output_record, "output")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"public_artifact_signature_output={output_record}")
    print(f"public_artifact_signature_sha256={evidence['evidence_sha256']}")
    print("public_artifact_signature_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
