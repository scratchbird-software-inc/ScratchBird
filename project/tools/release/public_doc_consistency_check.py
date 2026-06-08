#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate that public docs match public release readiness evidence."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path
import sys
from typing import Any


# PUBLIC_DOC_CONSISTENCY_CHECK

DOC_ROOT = Path("project") / "docs"
PUBLIC_API_DOC = DOC_ROOT / "public_api" / "CORE_BETA_PUBLIC_API_ABI.md"
PUBLIC_API_MANIFEST = DOC_ROOT / "public_api" / "CORE_BETA_PUBLIC_API_ABI_MANIFEST.json"
TEXT_DOC_SUFFIXES = {".csv", ".json", ".md", ".rst", ".txt"}

FORBIDDEN_REFERENCE_FRAGMENTS = (
    "docs" + "/" + "execution-plans",
    "docs" + "/" + "completed-execution-plans",
    "docs" + "/" + "findings",
    "." + "git",
    "/" + "home" + "/",
    "ScratchBird" + "-Private",
    "local" + "_work",
)

EXPECTED_ARCHITECTURAL_BOUNDARIES = {
    "engine_execution_authority": "engine_sblr_internal_api_only",
    "sql_text_authoritative": False,
    "uuid_identity_authoritative": True,
    "mga_finality_authoritative": True,
    "cluster_positive_behavior_in_core": False,
}

EXPECTED_SURFACE_IDS = {
    "embedded_engine_c_abi_v1",
    "embedded_engine_cpp_wrappers_v1",
    "driver_package_manifest_v1",
    "sbsql_parser_package_v3",
    "sbsql_parser_support_udr_v1",
    "trusted_cpp_udr_runtime_v1",
    "cluster_provider_boundary_v1",
}

RELEASE_READINESS_TOKENS = (
    "PUBLIC_DOC_CONSISTENCY_GATE",
    "public_enterprise_documentation_gate",
    "public_platform_matrix_gate",
    "public_index_readiness_matrix_gate",
    "public_agent_readiness_matrix_gate",
    "public_cluster_catalog_manifest_check_gate",
    "public_cluster_provider_handshake_gate",
    "public_cluster_build_matrix_gate",
    "public_cluster_catalog_backup_export_gate",
    "public_cluster_readable_view_gate",
    "public_cluster_projection_redaction_gate",
    "public_cluster_projection_authority_guard_gate",
    "public_cluster_catalog_crypto_evidence_gate",
    "public_sblr_uuid_mga_route_integration_gate",
    "public_api_boundary_gate",
)

ENGINE_PUBLIC_ABI_TOKENS = (
    "sb_engine_public_headers_api_docs_freeze_gate",
    "public_headers_api_docs_freeze_static.py",
)

SOURCE_CLAIM_TOKENS = {
    "project/src/engine/public_abi.cpp": (
        "sb_engine_open",
        "sb_engine_dispatch_sblr",
        "sb_engine_transaction_commit",
    ),
    "project/src/server/parser_package_registry.hpp": (
        "parser_api_major = 3",
        'parser_support_udr_abi = "sb_udr_v1"',
    ),
    "project/src/parsers/native/v3/package/native_v3_parser_package.hpp": (
        "NativeV3ParserPackageRequest",
        "sblr_parser_resolved_names_to_uuids",
        "dispatched_to_engine_api",
    ),
    "project/src/udr/sbu_sbsql_parser_support/sbu_sbsql_parser_support.hpp": (
        "kSbuSbsqlPackageUuid",
        "sbu_sbsql_package_descriptor",
    ),
    "project/src/udr/runtime/sb_udr_runtime.hpp": (
        "UdrPackageDescriptor",
        "UdrEntrypointDescriptor",
    ),
    "project/src/cluster_provider/cluster_provider.hpp": (
        "SBLR.CLUSTER.SUPPORT_NOT_ENABLED",
        "InspectClusterProvider",
        "ExecuteClusterOperation",
    ),
    "project/src/cluster_provider/no_cluster_provider.cpp": (
        "kClusterSupportNotEnabledCode",
        "ClusterProviderSupportsExecution",
    ),
    "project/src/cluster_provider_stub/stub_cluster_provider.cpp": (
        "scratchbird.cluster.compile_link_stub_provider",
        "ClusterProviderSupportsExecution",
    ),
    "project/src/engine/internal_api/extensibility/extension_boundary_manifest.cpp": (
        "cluster_provider.v1",
        "cluster_manager.v1",
        "SB_ENGINE_API_CLUSTER_AUTHORITY_UNAVAILABLE",
    ),
}


def fail(message: str) -> None:
    print(f"public_doc_consistency_check=fail:{message}", file=sys.stderr)
    raise SystemExit(1)


def read_text(path: Path, repo_root: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except OSError as exc:
        fail(f"read_failed:{rel(path, repo_root)}:{exc}")
    except UnicodeDecodeError as exc:
        fail(f"utf8_decode_failed:{rel(path, repo_root)}:{exc}")


def rel(path: Path, repo_root: Path) -> str:
    return path.resolve().relative_to(repo_root).as_posix()


def sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def reject_private_reference(value: str, context: str) -> None:
    if Path(value).is_absolute():
        fail(f"absolute_path_recorded:{context}:{value}")
    for fragment in FORBIDDEN_REFERENCE_FRAGMENTS:
        if fragment in value:
            fail(f"private_reference_recorded:{context}:{value}")


def require_contains(text: str, token: str, context: str) -> None:
    if token not in text:
        fail(f"{context}_missing:{token}")


def require_file(repo_root: Path, relative_path: str | Path) -> Path:
    path = repo_root / relative_path
    if not path.exists() or not path.is_file():
        fail(f"required_file_missing:{relative_path}")
    return path


def require_existing_path(repo_root: Path, relative_path: str | Path) -> Path:
    path = repo_root / relative_path
    if not path.exists():
        fail(f"required_path_missing:{relative_path}")
    return path


def load_manifest(repo_root: Path) -> dict[str, Any]:
    manifest_path = require_file(repo_root, PUBLIC_API_MANIFEST)
    try:
        manifest = json.loads(read_text(manifest_path, repo_root))
    except json.JSONDecodeError as exc:
        fail(f"manifest_json_invalid:{exc}")
    if not isinstance(manifest, dict):
        fail("manifest_root_not_object")
    return manifest


def scan_public_docs(repo_root: Path) -> dict[str, Any]:
    docs_root = repo_root / DOC_ROOT
    if not docs_root.is_dir():
        fail("project_docs_missing")

    files: list[Path] = []
    digest = hashlib.sha256()
    for path in sorted(docs_root.rglob("*")):
        if not path.is_file():
            continue
        if path.suffix not in TEXT_DOC_SUFFIXES:
            continue
        path_text = rel(path, repo_root)
        reject_private_reference(path_text, "public_doc_path")
        text = read_text(path, repo_root)
        reject_private_reference(text, path_text)
        files.append(path)
        digest.update(path_text.encode("utf-8"))
        digest.update(b"\0")
        digest.update(sha256_text(text).encode("ascii"))
        digest.update(b"\0")

    if not files:
        fail("public_docs_empty")
    return {
        "doc_count": len(files),
        "doc_paths": [rel(path, repo_root) for path in files],
        "aggregate_sha256": digest.hexdigest(),
    }


def validate_manifest_shape(repo_root: Path, manifest: dict[str, Any]) -> dict[str, Any]:
    if manifest.get("manifest_schema") != "scratchbird.core_beta.public_api_abi_freeze.v1":
        fail("manifest_schema_drift")
    if manifest.get("freeze_id") != "core_beta_public_api_abi_freeze_2026_05":
        fail("freeze_id_drift")
    if manifest.get("status") != "frozen_for_core_beta_qa":
        fail("manifest_status_drift")
    if manifest.get("architectural_boundaries") != EXPECTED_ARCHITECTURAL_BOUNDARIES:
        fail("architectural_boundaries_drift")

    include_root = manifest.get("packaged_include_root")
    if include_root != "project/include/scratchbird/engine":
        fail("public_include_root_drift")
    reject_private_reference(include_root, "packaged_include_root")

    public_headers = manifest.get("public_headers")
    c_api_symbols = manifest.get("c_api_symbols")
    surfaces = manifest.get("surfaces")
    extension_boundaries = manifest.get("extension_boundaries")
    if not isinstance(public_headers, list) or not public_headers:
        fail("manifest_public_headers_empty")
    if not isinstance(c_api_symbols, list) or not c_api_symbols:
        fail("manifest_c_api_symbols_empty")
    if not isinstance(surfaces, list) or not surfaces:
        fail("manifest_surfaces_empty")
    if not isinstance(extension_boundaries, list) or not extension_boundaries:
        fail("manifest_extension_boundaries_empty")

    for header in public_headers:
        if not isinstance(header, str):
            fail("public_header_path_not_string")
        reject_private_reference(header, "public_header")
        require_file(repo_root, header)

    surface_ids = {surface.get("surface_id") for surface in surfaces if isinstance(surface, dict)}
    if surface_ids != EXPECTED_SURFACE_IDS:
        fail(f"surface_id_set_drift:{sorted(surface_ids)}")

    source_paths: set[str] = set()
    for surface in surfaces:
        if not isinstance(surface, dict):
            fail("surface_row_not_object")
        surface_id = str(surface.get("surface_id"))
        if surface.get("classification") == "non_core_cluster_boundary":
            if surface.get("non_cluster_refusal_code") != "SBLR.CLUSTER.SUPPORT_NOT_ENABLED":
                fail(f"cluster_surface_refusal_code_drift:{surface_id}")
        for path_text in surface.get("source_paths", []):
            if not isinstance(path_text, str):
                fail(f"surface_source_path_not_string:{surface_id}")
            reject_private_reference(path_text, f"surface_source:{surface_id}")
            require_file(repo_root, path_text)
            source_paths.add(path_text)

    for boundary in extension_boundaries:
        if not isinstance(boundary, dict):
            fail("extension_boundary_not_object")
        for field in (
            "boundary_id",
            "boundary_type",
            "contract_version",
            "core_classification",
            "non_cluster_refusal_code",
        ):
            value = boundary.get(field)
            if not isinstance(value, str):
                fail(f"extension_boundary_field_missing:{field}")
            reject_private_reference(value, f"extension_boundary:{field}")

    return {
        "public_header_count": len(public_headers),
        "c_api_symbol_count": len(c_api_symbols),
        "surface_count": len(surfaces),
        "surface_ids": sorted(surface_ids),
        "source_path_count": len(source_paths),
        "extension_boundary_count": len(extension_boundaries),
    }


def validate_docs_pair(repo_root: Path, manifest: dict[str, Any]) -> dict[str, Any]:
    docs_path = require_file(repo_root, PUBLIC_API_DOC)
    docs = read_text(docs_path, repo_root)

    for token in (
        manifest["manifest_schema"],
        manifest["freeze_id"],
        manifest["abi_version"]["packed_hex"],
        "engine_sblr_internal_api_only",
        "SQL text is never runtime authority",
        "MGA transaction inventory remains finality authority",
        "Cluster-positive behavior is outside core",
    ):
        require_contains(docs, token, "public_api_doc")

    for header in manifest["public_headers"]:
        require_contains(docs, header, "public_api_doc_header")
    for symbol in manifest["c_api_symbols"]:
        require_contains(docs, symbol, "public_api_doc_symbol")
    for surface in manifest["surfaces"]:
        require_contains(docs, surface["surface_id"], "public_api_doc_surface")
        require_contains(docs, surface["contract_version"], "public_api_doc_surface")
    for boundary in manifest["extension_boundaries"]:
        require_contains(docs, boundary["boundary_id"], "public_api_doc_boundary")

    return {
        "path": PUBLIC_API_DOC.as_posix(),
        "sha256": sha256_text(docs),
        "status": "manifest_tokens_present",
    }


def validate_driver_manifest(repo_root: Path, manifest: dict[str, Any]) -> dict[str, Any]:
    driver = manifest.get("driver_manifest")
    if not isinstance(driver, dict):
        fail("driver_manifest_missing")
    path_text = driver.get("path")
    if not isinstance(path_text, str):
        fail("driver_manifest_path_missing")
    reject_private_reference(path_text, "driver_manifest_path")
    path = require_file(repo_root, path_text)
    rows = list(csv.DictReader(path.open(newline="", encoding="utf-8")))
    if len(rows) < int(driver.get("minimum_rows", 0)):
        fail(f"driver_manifest_row_count_below_claim:{len(rows)}")

    allowed_surfaces = set(driver.get("allowed_api_surface_sets", []))
    allowed_ingress = set(driver.get("allowed_ingress_modes", []))
    for row in rows:
        reject_private_reference(row.get("source_path", ""), f"driver_source:{row.get('component_id', '')}")
        require_existing_path(repo_root, row["source_path"])
        if row.get("wire_protocol_set") != driver.get("wire_protocol_set"):
            fail(f"driver_wire_protocol_drift:{row.get('component_id')}")
        surfaces = set(row.get("api_surface_set", "").split(";"))
        ingress = set(row.get("ingress_mode_set", "").split(";"))
        if not surfaces <= allowed_surfaces:
            fail(f"driver_surface_set_undocumented:{row.get('component_id')}")
        if not ingress <= allowed_ingress:
            fail(f"driver_ingress_set_undocumented:{row.get('component_id')}")

    return {"path": path_text, "row_count": len(rows), "status": "matched_manifest_claims"}


def validate_parser_udr_contract(repo_root: Path, manifest: dict[str, Any]) -> dict[str, Any]:
    contract = manifest.get("parser_udr_contract")
    if not isinstance(contract, dict):
        fail("parser_udr_contract_missing")
    path_text = contract.get("path")
    if not isinstance(path_text, str):
        fail("parser_udr_contract_path_missing")
    reject_private_reference(path_text, "parser_udr_contract_path")
    text = read_text(require_file(repo_root, path_text), repo_root)

    for token in (
        contract.get("package_uuid_constant"),
        contract.get("package_name_constant"),
        "sbu_sbsql_package_descriptor",
    ):
        if not isinstance(token, str):
            fail("parser_udr_contract_token_missing")
        require_contains(text, token, "parser_udr_contract")
    for entrypoint in contract.get("entrypoints", []):
        if not isinstance(entrypoint, str):
            fail("parser_udr_entrypoint_not_string")
        require_contains(text, entrypoint, "parser_udr_contract_entrypoint")

    return {
        "path": path_text,
        "entrypoint_count": len(contract.get("entrypoints", [])),
        "status": "matched_manifest_claims",
    }


def validate_source_claim_tokens(repo_root: Path) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for path_text, tokens in SOURCE_CLAIM_TOKENS.items():
        text = read_text(require_file(repo_root, path_text), repo_root)
        reject_private_reference(path_text, "source_claim_path")
        for token in tokens:
            require_contains(text, token, path_text)
        rows.append({"path": path_text, "token_count": len(tokens), "status": "present"})
    return rows


def validate_readiness_wiring(repo_root: Path) -> list[dict[str, Any]]:
    release_cmake = read_text(
        require_file(repo_root, Path("project") / "tests" / "release" / "CMakeLists.txt"),
        repo_root,
    )
    engine_public_abi_cmake = read_text(
        require_file(repo_root, Path("project") / "tests" / "engine_public_abi" / "CMakeLists.txt"),
        repo_root,
    )
    for token in RELEASE_READINESS_TOKENS:
        require_contains(release_cmake, token, "release_cmake_readiness")
    for token in ENGINE_PUBLIC_ABI_TOKENS:
        require_contains(engine_public_abi_cmake, token, "engine_public_abi_cmake")

    return [
        {"proof": token, "source": "project/tests/release/CMakeLists.txt", "status": "wired"}
        for token in RELEASE_READINESS_TOKENS
    ] + [
        {
            "proof": token,
            "source": "project/tests/engine_public_abi/CMakeLists.txt",
            "status": "wired",
        }
        for token in ENGINE_PUBLIC_ABI_TOKENS
    ]


def build_evidence(args: argparse.Namespace) -> dict[str, Any]:
    repo_root = args.repo_root.resolve()
    project_root = args.project_root.resolve()
    if not repo_root.is_dir() or not project_root.is_dir():
        fail("input_root_missing")
    if project_root != repo_root / "project":
        fail("project_root_must_be_repo_project")

    manifest = load_manifest(repo_root)
    docs_scan = scan_public_docs(repo_root)
    manifest_summary = validate_manifest_shape(repo_root, manifest)
    public_api_doc = validate_docs_pair(repo_root, manifest)
    driver_summary = validate_driver_manifest(repo_root, manifest)
    parser_udr_summary = validate_parser_udr_contract(repo_root, manifest)
    source_claims = validate_source_claim_tokens(repo_root)
    readiness_wiring = validate_readiness_wiring(repo_root)

    evidence: dict[str, Any] = {
        "schema_version": 1,
        "policy": {
            "public_docs_root": DOC_ROOT.as_posix(),
            "private_execution_plan_references_allowed": False,
            "private_findings_references_allowed": False,
            "absolute_local_paths_allowed": False,
            "public_claim_source": PUBLIC_API_MANIFEST.as_posix(),
            "readiness_matrix_wiring_required": True,
            "release_proof_is_evidence_only": True,
        },
        "docs_scan": docs_scan,
        "manifest": manifest_summary,
        "public_api_doc": public_api_doc,
        "driver_manifest": driver_summary,
        "parser_udr_contract": parser_udr_summary,
        "source_claim_tokens": source_claims,
        "readiness_wiring": readiness_wiring,
    }
    evidence["evidence_sha256"] = sha256_text(
        json.dumps(evidence, sort_keys=True, separators=(",", ":"))
    )
    return evidence


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, required=True)
    parser.add_argument("--project-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    evidence = build_evidence(args)
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    try:
        output_record = output.relative_to(args.repo_root.resolve()).as_posix()
    except ValueError:
        output_record = output.name
    print(f"public_doc_consistency_output={output_record}")
    print(f"public_doc_consistency_sha256={evidence['evidence_sha256']}")
    print("public_doc_consistency_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
