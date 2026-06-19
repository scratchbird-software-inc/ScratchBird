#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate public API/ABI compatibility policy evidence."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path
import re
import sys
from typing import Any


# PUBLIC_COMPATIBILITY_POLICY_CHECK
# PUBLIC_API_ABI_COMPAT_GATE

MANIFEST_PATH = Path("project") / "docs" / "public_api" / "CORE_BETA_PUBLIC_API_ABI_MANIFEST.json"
API_DOC_PATH = Path("project") / "docs" / "public_api" / "CORE_BETA_PUBLIC_API_ABI.md"
POLICY_DOC_PATH = Path("project") / "docs" / "public_api" / "CORE_BETA_PUBLIC_COMPATIBILITY_POLICY.md"
ENGINE_ABI_CMAKE_PATH = Path("project") / "tests" / "engine_public_abi" / "CMakeLists.txt"
RELEASE_CMAKE_PATH = Path("project") / "tests" / "release" / "CMakeLists.txt"

FORBIDDEN_REFERENCE_FRAGMENTS = (
    "docs" + "/" + "execution-plans",
    "docs" + "/" + "completed-execution-plans",
    "docs" + "/" + "findings",
    "." + "git",
    "/" + "home" + "/",
    "ScratchBird" + "-Private",
    "local" + "_work",
)

REQUIRED_SURFACE_IDS = {
    "embedded_engine_c_abi_v1",
    "embedded_engine_cpp_wrappers_v1",
    "driver_package_manifest_v1",
    "sbsql_parser_package_v3",
    "sbsql_parser_support_udr_v1",
    "trusted_cpp_udr_runtime_v1",
    "cluster_provider_boundary_v1",
}

POLICY_DOC_TOKENS = (
    "PUBLIC_API_COMPATIBILITY_POLICY",
    "PUBLIC_API_ABI_SURFACE",
    "Major versions may remove or break public ABI symbols",
    "Minor versions may add compatible public headers",
    "Patch versions must not add, remove, or reorder public ABI symbols",
    "Deprecation requires a stable diagnostic or manifest row",
    "Removal of a public header, C ABI symbol, provider contract, diagnostic code",
)

EVIDENCE_CHECKS: tuple[dict[str, Any], ...] = (
    {
        "surface": "public_header_and_c_abi_freeze",
        "path": "project/tests/engine_public_abi/CMakeLists.txt",
        "tokens": (
            "sb_engine_public_headers_api_docs_freeze_gate",
            "sb_engine_public_abi_c_fixture",
            "sb_engine_public_abi_cpp_fixture",
            "sb_engine_public_abi_symbol_gate",
        ),
    },
    {
        "surface": "diagnostic_abi_shape",
        "path": "project/tests/engine_public_abi/CMakeLists.txt",
        "tokens": (
            "sb_engine_public_diagnostic_shape_fixture",
            "sb_engine_public_wire_stability_fixture",
            "sb_engine_public_documentation_example_fixture",
        ),
    },
    {
        "surface": "file_format_compatibility",
        "path": "project/tests/release/CMakeLists.txt",
        "tokens": (
            "public_release_version_metadata_gate",
            "public_upgrade_migration_gate",
            "downgrade_refusal",
            "stable_diagnostics",
        ),
    },
    {
        "surface": "config_compatibility",
        "path": "project/tools/release/public_default_config_check.py",
        "tokens": (
            "PUBLIC_DEFAULT_CONFIG_CHECK",
            "security_default_policy_installed",
            "default_local_server_memory_cache_v1",
        ),
    },
    {
        "surface": "policy_pack_schema_compatibility",
        "path": "project/tools/release/public_policy_pack_manifest_gate.py",
        "tokens": (
            "min_supported_schema_version",
            "max_supported_schema_version",
            "policy_pack_version",
            "validate_policy_profiles",
        ),
    },
    {
        "surface": "diagnostic_matrix_compatibility",
        "path": "project/tools/release/public_diagnostic_matrix_generator.py",
        "tokens": (
            "PUBLIC_DIAGNOSTIC_MATRIX_GENERATOR",
            "stable_public",
            "fail_closed_stable",
            "unsupported_stable",
        ),
    },
    {
        "surface": "extension_boundary_compatibility",
        "path": "project/src/engine/internal_api/extensibility/extension_boundary_manifest.cpp",
        "tokens": (
            "cluster_provider.v1",
            "cluster_manager.v1",
            "SB_ENGINE_API_CLUSTER_AUTHORITY_UNAVAILABLE",
            "sb_parser_package_v3",
            "sb_udr_v1",
        ),
    },
)


def fail(message: str) -> None:
    print(f"public_compatibility_policy_check=fail:{message}", file=sys.stderr)
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


def read_text(repo_root: Path, relative_path: str | Path) -> str:
    path_text = Path(relative_path).as_posix()
    reject_private_reference(path_text, "source_path")
    path = repo_root / relative_path
    require(path.is_file(), f"source_missing:{path_text}")
    try:
        text = path.read_text(encoding="utf-8")
    except UnicodeDecodeError as exc:
        fail(f"utf8_decode_failed:{path_text}:{exc}")
    except OSError as exc:
        fail(f"read_failed:{path_text}:{exc}")
    for fragment in FORBIDDEN_REFERENCE_FRAGMENTS:
        if fragment in text:
            fail(f"private_reference_recorded:{path_text}:{fragment}")
    return text


def require_token(text: str, token: str, context: str) -> None:
    require(isinstance(token, str) and token, f"token_invalid:{context}")
    if token not in text:
        fail(f"token_missing:{context}:{token}")


def parse_version_macros(header: str) -> tuple[int, int, int, int]:
    values: dict[str, int] = {}
    for name in ("MAJOR", "MINOR", "PATCH"):
        match = re.search(rf"#define\s+SB_ENGINE_ABI_VERSION_{name}\s+([0-9]+)u", header)
        require(match is not None, f"version_macro_missing:{name}")
        values[name] = int(match.group(1))
    packed = (values["MAJOR"] << 16) | (values["MINOR"] << 8) | values["PATCH"]
    return values["MAJOR"], values["MINOR"], values["PATCH"], packed


def load_manifest(repo_root: Path) -> tuple[dict[str, Any], str]:
    text = read_text(repo_root, MANIFEST_PATH)
    try:
        manifest = json.loads(text)
    except json.JSONDecodeError as exc:
        fail(f"manifest_json_invalid:{exc}")
    require(isinstance(manifest, dict), "manifest_root_not_object")
    return manifest, text


def actual_public_headers(repo_root: Path, include_root: str) -> list[str]:
    reject_private_reference(include_root, "include_root")
    root = repo_root / include_root
    require(root.is_dir(), f"include_root_missing:{include_root}")
    return sorted(
        path.resolve().relative_to(repo_root).as_posix()
        for path in root.rglob("*")
        if path.suffix in {".h", ".hpp"}
    )


def validate_manifest(repo_root: Path, manifest: dict[str, Any]) -> dict[str, Any]:
    require(manifest.get("manifest_schema") == "scratchbird.core_beta.public_api_abi_freeze.v1",
            "manifest_schema_drift")
    require(manifest.get("status") == "frozen_for_core_beta_qa", "manifest_status_drift")
    abi = manifest.get("abi_version")
    require(isinstance(abi, dict), "abi_version_missing")
    header_path = abi.get("source_header")
    require(isinstance(header_path, str), "abi_source_header_missing")
    header = read_text(repo_root, header_path)
    require_token(header, "PUBLIC_API_ABI_SURFACE", "abi_header")
    major, minor, patch, packed = parse_version_macros(header)
    require((major, minor, patch) == (abi.get("major"), abi.get("minor"), abi.get("patch")),
            "abi_semver_macro_drift")
    require(packed == abi.get("packed"), "abi_packed_drift")
    require(abi.get("packed_hex") == f"0x{packed:08x}", "abi_packed_hex_drift")
    require(major >= 1 and minor >= 0 and patch >= 0, "semver_values_invalid")

    public_headers = manifest.get("public_headers")
    c_api_symbols = manifest.get("c_api_symbols")
    surfaces = manifest.get("surfaces")
    extension_boundaries = manifest.get("extension_boundaries")
    require(isinstance(public_headers, list) and public_headers, "public_headers_missing")
    require(isinstance(c_api_symbols, list) and c_api_symbols, "c_api_symbols_missing")
    require(isinstance(surfaces, list) and surfaces, "surfaces_missing")
    require(isinstance(extension_boundaries, list) and extension_boundaries, "extension_boundaries_missing")
    require(len(public_headers) == len(set(public_headers)), "public_header_duplicate")
    require(len(c_api_symbols) == len(set(c_api_symbols)), "c_api_symbol_duplicate")

    include_root = manifest.get("packaged_include_root")
    require(isinstance(include_root, str), "packaged_include_root_missing")
    require(actual_public_headers(repo_root, include_root) == sorted(public_headers),
            "public_header_inventory_drift")

    surface_ids = {surface.get("surface_id") for surface in surfaces if isinstance(surface, dict)}
    require(surface_ids == REQUIRED_SURFACE_IDS, f"surface_ids_drift:{sorted(surface_ids)}")
    for surface in surfaces:
        require(isinstance(surface, dict), "surface_not_object")
        require(isinstance(surface.get("contract_version"), str) and surface["contract_version"],
                f"surface_contract_missing:{surface.get('surface_id')}")
        for path_text in surface.get("source_paths", []):
            require(isinstance(path_text, str), f"surface_path_not_string:{surface.get('surface_id')}")
            read_text(repo_root, path_text)

    return {
        "abi_version": f"{major}.{minor}.{patch}",
        "packed_hex": abi["packed_hex"],
        "public_header_count": len(public_headers),
        "c_api_symbol_count": len(c_api_symbols),
        "surface_count": len(surfaces),
        "extension_boundary_count": len(extension_boundaries),
    }


def validate_policy_doc(repo_root: Path) -> dict[str, Any]:
    text = read_text(repo_root, POLICY_DOC_PATH)
    for token in POLICY_DOC_TOKENS:
        require_token(text, token, "compatibility_policy_doc")
    return {"path": POLICY_DOC_PATH.as_posix(), "sha256": sha256_text(text), "token_count": len(POLICY_DOC_TOKENS)}


def validate_evidence_checks(repo_root: Path) -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
    for check in EVIDENCE_CHECKS:
        surface = check["surface"]
        path_text = check["path"]
        tokens = check["tokens"]
        text = read_text(repo_root, path_text)
        token_digests: list[str] = []
        for token in tokens:
            require_token(text, token, f"evidence:{surface}:{path_text}")
            token_digests.append(sha256_text(token))
        records.append(
            {
                "surface": surface,
                "path": path_text,
                "token_count": len(tokens),
                "source_sha256": sha256_text(text),
                "token_digest_sha256": sha256_text("\n".join(token_digests) + "\n"),
                "status": "pass",
            }
        )
    return records


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


def write_evidence(path: Path, evidence: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True, type=Path)
    parser.add_argument("--csv-output", required=True, type=Path)
    parser.add_argument("--evidence-output", required=True, type=Path)
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    manifest, manifest_text = load_manifest(repo_root)
    manifest_summary = validate_manifest(repo_root, manifest)
    api_doc_text = read_text(repo_root, API_DOC_PATH)
    require_token(api_doc_text, manifest["freeze_id"], "api_doc")
    policy_doc = validate_policy_doc(repo_root)
    records = validate_evidence_checks(repo_root)
    write_csv(args.csv_output, records)
    csv_text = args.csv_output.read_text(encoding="utf-8")

    evidence = {
        "schema": "scratchbird.public.api_abi_compatibility_gate.v1",
        "marker": "PUBLIC_COMPATIBILITY_POLICY_CHECK",
        "gate_marker": "PUBLIC_API_ABI_COMPAT_GATE",
        "gate": "PCR-GATE-141",
        "status": "pass",
        "manifest_path": MANIFEST_PATH.as_posix(),
        "manifest_sha256": sha256_text(manifest_text),
        "api_doc_path": API_DOC_PATH.as_posix(),
        "api_doc_sha256": sha256_text(api_doc_text),
        "policy_doc": policy_doc,
        "manifest": manifest_summary,
        "csv_sha256": sha256_text(csv_text),
        "surface_evidence_count": len(records),
        "authority": "public_release_evidence_only",
        "policy": {
            "semantic_versioning_required": True,
            "deprecation_requires_stable_diagnostic_or_manifest_row": True,
            "removal_forbidden_current_major": True,
            "patch_symbol_add_remove_forbidden": True,
            "mga_finality_authority_preserved": True,
            "parser_sql_text_authority": False,
            "cluster_public_production_claims": False,
        },
        "records": records,
    }
    write_evidence(args.evidence_output, evidence)
    print(
        "public_api_abi_compat_gate=passed "
        f"surfaces={len(records)} "
        f"csv_sha256={evidence['csv_sha256']}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
