#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate public release provenance and attestation proof anchors."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path
import sys
from typing import Any


# PUBLIC_RELEASE_ATTESTATION

FORBIDDEN_REFERENCE_FRAGMENTS = (
    "docs" + "/" + "execution-plans",
    "docs" + "/" + "completed-execution-plans",
    "docs" + "/" + "findings",
    "." + "git",
    "/" + "home" + "/",
    "ScratchBird" + "-Private",
    "local" + "_work",
)

CHECKS: tuple[dict[str, Any], ...] = (
    {
        "surface": "dependency_sbom_source_provenance",
        "path": "project/tools/release/public_dependency_sbom.py",
        "tokens": (
            "build_metadata",
            "dependency_inventory",
            "signature_ready_artifact",
            "public_tree_inputs_only",
            "public_dependency_sbom=passed",
        ),
    },
    {
        "surface": "generated_source_provenance",
        "path": "project/tools/release/public_generated_source_provenance.py",
        "tokens": (
            "PUBLIC_GENERATED_SOURCE_PROVENANCE",
            "regeneration_required",
            "checked_in_authority_status",
            "drift_detection_required",
            "public_generated_provenance_gate=passed",
        ),
    },
    {
        "surface": "reproducible_source_archive",
        "path": "project/tools/release/public_reproducible_export.py",
        "tokens": (
            "PUBLIC_REPRODUCIBLE_EXPORT",
            "deterministic_two_pass_generation",
            "canonical_sha256",
            "raw_sha256",
            "public_reproducible_export=passed",
        ),
    },
    {
        "surface": "signature_ready_artifacts",
        "path": "project/tools/release/public_artifact_signature_gate.py",
        "tokens": (
            "PUBLIC_RELEASE_ARTIFACT_SIGNING",
            "signature-ready-ed25519",
            "public_tree_inputs_only",
            "RELEASE.ARTIFACT.CHECKSUM_MISMATCH",
            "public_artifact_signature_gate=passed",
        ),
    },
    {
        "surface": "crypto_signature_ready_policy",
        "path": "project/tests/release/public_crypto_entropy_policy_gate.py",
        "tokens": (
            "PUBLIC_CRYPTO_ENTROPY_GATE",
            "signature_ready_metadata",
            "approved_hash",
            "approved_mac",
            "weak_checksums_authority",
        ),
    },
    {
        "surface": "version_metadata_attestation",
        "path": "project/tools/release/public_release_version_metadata.cpp",
        "tokens": (
            "SB_PUBLIC_RELEASE_PROJECT_VERSION",
            "database_format",
            "catalog_record",
            "datatype_wire",
            "public_release_version_metadata=passed",
        ),
    },
    {
        "surface": "deterministic_generated_manifest",
        "path": "project/tests/sbsql_parser_worker/generated/repro/DETERMINISTIC_ARTIFACT_MANIFEST.csv",
        "tokens": (
            "artifact_path",
            "sha256",
            "size_bytes",
            "category",
            "source_inputs",
        ),
    },
    {
        "surface": "release_gate_wiring",
        "path": "project/tests/release/CMakeLists.txt",
        "tokens": (
            "PUBLIC_RELEASE_ATTESTATION_GATE",
            "public_release_attestation_gate",
            "public_dependency_sbom_gate",
            "public_generated_provenance_gate",
            "public_artifact_reproducibility_gate",
            "public_artifact_signature_gate",
            "public_crypto_entropy_policy_gate",
            "PCR-GATE-146",
        ),
    },
)


def fail(message: str) -> None:
    print(f"public_release_attestation_gate=fail:{message}", file=sys.stderr)
    raise SystemExit(1)


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def reject_private_reference(value: str, context: str) -> None:
    if Path(value).is_absolute():
        fail(f"absolute_path_recorded:{context}:{value}")
    for fragment in FORBIDDEN_REFERENCE_FRAGMENTS:
        if fragment in value:
            fail(f"private_reference_recorded:{context}:{value}")


def rel(path: Path, root: Path, context: str) -> str:
    try:
        value = path.resolve().relative_to(root.resolve()).as_posix()
    except ValueError:
        fail(f"path_outside_root:{context}:{path.name}")
    reject_private_reference(value, context)
    return value


def read_text(repo_root: Path, relative_path: str) -> str:
    reject_private_reference(relative_path, "source_path")
    path = repo_root / relative_path
    require(path.is_file(), f"source_missing:{relative_path}")
    try:
        return path.read_text(encoding="utf-8")
    except UnicodeDecodeError as exc:
        fail(f"utf8_decode_failed:{relative_path}:{exc}")
    except OSError as exc:
        fail(f"read_failed:{relative_path}:{exc}")


def parse_cache(cache_path: Path) -> dict[str, str]:
    require(cache_path.is_file(), "cmake_cache_missing")
    values: dict[str, str] = {}
    for raw in cache_path.read_text(encoding="utf-8", errors="replace").splitlines():
        if not raw or raw.startswith(("//", "#")) or ":" not in raw or "=" not in raw:
            continue
        key_type, value = raw.split("=", 1)
        key = key_type.split(":", 1)[0]
        values[key] = value
    return values


def parse_cmake_set_file(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    if not path.is_file():
        return values
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.strip()
        if not line.startswith("set(") or not line.endswith(")"):
            continue
        payload = line[4:-1].strip()
        pieces = payload.split(None, 1)
        if len(pieces) != 2:
            continue
        values[pieces[0]] = pieces[1].strip().strip('"')
    return values


def cmake_version(cache: dict[str, str]) -> str:
    direct = cache.get("CMAKE_VERSION", "")
    if direct:
        return direct
    major = cache.get("CMAKE_CACHE_MAJOR_VERSION", "")
    minor = cache.get("CMAKE_CACHE_MINOR_VERSION", "")
    patch = cache.get("CMAKE_CACHE_PATCH_VERSION", "")
    if major and minor and patch:
        return f"{major}.{minor}.{patch}"
    return ""


def compiler_metadata(build_root: Path, cache: dict[str, str]) -> dict[str, str]:
    metadata = {
        "id": cache.get("CMAKE_CXX_COMPILER_ID", ""),
        "version": cache.get("CMAKE_CXX_COMPILER_VERSION", ""),
        "compiler": cache.get("CMAKE_CXX_COMPILER", ""),
    }
    for compiler_file in sorted((build_root / "CMakeFiles").glob("*/CMakeCXXCompiler.cmake")):
        cmake_metadata = parse_cmake_set_file(compiler_file)
        metadata["id"] = metadata["id"] or cmake_metadata.get("CMAKE_CXX_COMPILER_ID", "")
        metadata["version"] = metadata["version"] or cmake_metadata.get("CMAKE_CXX_COMPILER_VERSION", "")
        metadata["compiler"] = metadata["compiler"] or cmake_metadata.get("CMAKE_CXX_COMPILER", "")
        if metadata["id"] and metadata["version"] and metadata["compiler"]:
            break
    require(bool(metadata["id"]), "compiler_id_missing")
    require(bool(metadata["version"]), "compiler_version_missing")
    require(bool(metadata["compiler"]), "compiler_path_missing")
    metadata["compiler"] = Path(metadata["compiler"]).name
    return metadata


def validate_build_metadata(repo_root: Path, build_root: Path) -> dict[str, Any]:
    require(build_root.is_dir(), "build_root_missing")
    reject_private_reference(build_root.name, "build_root")
    cache = parse_cache(build_root / "CMakeCache.txt")
    compiler = compiler_metadata(build_root, cache)
    required_values = {
        "SB_BUILD_PUBLIC_RELEASE_CORRECTNESS": "ON",
        "SCRATCHBIRD_ENABLE_DEBUG_LOGS": "OFF",
        "SCRATCHBIRD_ENABLE_HOTPATH_TRACE": "OFF",
        "SCRATCHBIRD_ENABLE_EXEC_PROFILE_TRACE": "OFF",
        "SCRATCHBIRD_ENABLE_PREPARED_TRACE": "OFF",
    }
    build_type = cache.get("CMAKE_BUILD_TYPE", "")
    if build_type and build_type != "Release":
        fail(f"build_cache_value_mismatch:CMAKE_BUILD_TYPE:{build_type}")
    for key, expected in required_values.items():
        require(cache.get(key) == expected, f"build_cache_value_mismatch:{key}:{cache.get(key, '')}")
    version = cmake_version(cache)
    require(bool(version), "cmake_version_missing")
    return {
        "cmake_version": version,
        "build_type": build_type or "single_config_or_default_generator",
        "cxx_compiler_id": compiler["id"],
        "cxx_compiler_version": compiler["version"],
        "cxx_compiler": compiler["compiler"],
        "system_name": cache.get("CMAKE_SYSTEM_NAME", ""),
        "system_processor": cache.get("CMAKE_SYSTEM_PROCESSOR", ""),
        "public_release_correctness": cache["SB_BUILD_PUBLIC_RELEASE_CORRECTNESS"],
        "instrumentation": {
            "debug_logs": cache["SCRATCHBIRD_ENABLE_DEBUG_LOGS"],
            "hotpath_trace": cache["SCRATCHBIRD_ENABLE_HOTPATH_TRACE"],
            "exec_profile_trace": cache["SCRATCHBIRD_ENABLE_EXEC_PROFILE_TRACE"],
            "prepared_trace": cache["SCRATCHBIRD_ENABLE_PREPARED_TRACE"],
        },
        "cluster_provider_mode": {
            "enabled": cache.get("SB_ENABLE_CLUSTER_PROVIDER", "OFF"),
            "stub": cache.get("SB_CLUSTER_PROVIDER_STUB", "OFF"),
            "external_library_present": bool(cache.get("SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY", "")),
        },
    }


def validate_check(repo_root: Path, check: dict[str, Any]) -> dict[str, Any]:
    surface = check["surface"]
    path_text = check["path"]
    tokens = check["tokens"]
    require(isinstance(surface, str) and surface, "surface_invalid")
    require(isinstance(path_text, str) and path_text, f"path_invalid:{surface}")
    require(isinstance(tokens, tuple) and tokens, f"tokens_invalid:{surface}")
    text = read_text(repo_root, path_text)
    token_digests: list[str] = []
    for token in tokens:
        require(isinstance(token, str) and token, f"token_invalid:{surface}")
        if token not in text:
            fail(f"token_missing:{surface}:{path_text}:{token}")
        token_digests.append(sha256_text(token))
    return {
        "surface": surface,
        "path": path_text,
        "token_count": len(tokens),
        "source_sha256": sha256_text(text),
        "token_digest_sha256": sha256_text("\n".join(token_digests) + "\n"),
        "status": "pass",
    }


def write_csv(path: Path, records: list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(
            handle,
            fieldnames=[
                "surface",
                "path",
                "token_count",
                "source_sha256",
                "token_digest_sha256",
                "status",
            ],
        )
        writer.writeheader()
        writer.writerows(records)


def write_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", required=True, type=Path)
    parser.add_argument("--build-root", required=True, type=Path)
    parser.add_argument("--csv-output", required=True, type=Path)
    parser.add_argument("--evidence-output", required=True, type=Path)
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    repo_root = args.repo_root.resolve()
    build_root = args.build_root.resolve()
    records = [validate_check(repo_root, check) for check in CHECKS]
    build_metadata = validate_build_metadata(repo_root, build_root)
    matrix_text = "\n".join(
        f"{record['surface']},{record['path']},{record['status']}" for record in records
    ) + "\n"
    payload = {
        "gate": "PUBLIC_RELEASE_ATTESTATION_GATE",
        "marker": "PUBLIC_RELEASE_ATTESTATION",
        "status": "pass",
        "checks": records,
        "check_count": len(records),
        "matrix_sha256": sha256_text(matrix_text),
        "build_environment": build_metadata,
        "attestation_subjects": [
            "dependency_sbom_source_provenance",
            "generated_source_provenance",
            "reproducible_source_archive",
            "signature_ready_artifacts",
            "crypto_signature_ready_policy",
            "version_metadata_attestation",
        ],
        "policy": {
            "public_tree_inputs_only": True,
            "private_docs_required": False,
            "git_history_required": False,
            "signature_ready_not_signed": True,
            "release_attestation_is_evidence_only": True,
        },
    }
    write_csv(args.csv_output, records)
    write_json(args.evidence_output, payload)
    print(
        "public_release_attestation_gate=passed "
        f"checks={len(records)} matrix_sha256={payload['matrix_sha256']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
