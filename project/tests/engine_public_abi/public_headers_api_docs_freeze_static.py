#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Static public API/ABI freeze gate for core beta.

The gate intentionally reads only source-tree artifacts. It does not depend on
active execution_plan docs, and it is useful after the execution_plan package is archived or
deleted.
"""

from __future__ import annotations

import argparse
import csv
import json
import re
import sys
from pathlib import Path


def fail(message: str) -> None:
    print(f"public_headers_api_docs_freeze_static: {message}", file=sys.stderr)
    raise SystemExit(1)


def read(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except OSError as exc:
        fail(f"failed to read {path}: {exc}")


def rel(path: Path, repo_root: Path) -> str:
    return path.relative_to(repo_root).as_posix()


def parse_version_macros(version_header: str) -> tuple[int, int, int, int]:
    values: dict[str, int] = {}
    for name in ("MAJOR", "MINOR", "PATCH"):
        match = re.search(rf"#define\s+SB_ENGINE_ABI_VERSION_{name}\s+([0-9]+)u", version_header)
        if not match:
            fail(f"missing SB_ENGINE_ABI_VERSION_{name} macro")
        values[name] = int(match.group(1))
    packed = (values["MAJOR"] << 16) | (values["MINOR"] << 8) | values["PATCH"]
    return values["MAJOR"], values["MINOR"], values["PATCH"], packed


def public_header_paths(repo_root: Path, include_root: str) -> list[str]:
    root = repo_root / include_root
    if not root.is_dir():
        fail(f"public include root missing: {include_root}")
    return sorted(
        rel(path, repo_root)
        for path in root.rglob("*")
        if path.suffix in {".h", ".hpp"}
    )


def declared_c_symbols(repo_root: Path, headers: list[str]) -> list[str]:
    symbols: set[str] = set()
    for header in headers:
        text = read(repo_root / header)
        symbols.update(re.findall(r"\b(sb_engine_[A-Za-z0-9_]+)\s*\(", text))
    return sorted(symbols)


def validate_public_headers(repo_root: Path, manifest: dict) -> None:
    headers = public_header_paths(repo_root, manifest["packaged_include_root"])
    declared_headers = sorted(manifest["public_headers"])
    if headers != declared_headers:
        fail(f"public header inventory drifted: actual={headers} manifest={declared_headers}")

    major, minor, patch, packed = parse_version_macros(
        read(repo_root / manifest["abi_version"]["source_header"])
    )
    abi = manifest["abi_version"]
    if (major, minor, patch, packed) != (
        abi["major"],
        abi["minor"],
        abi["patch"],
        abi["packed"],
    ):
        fail("ABI version macros do not match manifest")
    if abi["packed_hex"].lower() != f"0x{packed:08x}":
        fail("ABI packed hex does not match version macros")

    symbols = declared_c_symbols(repo_root, headers)
    declared_symbols = sorted(manifest["c_api_symbols"])
    if symbols != declared_symbols:
        fail(f"C ABI symbol set drifted: actual={symbols} manifest={declared_symbols}")

    cmake_text = read(repo_root / "project/CMakeLists.txt")
    if "install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/scratchbird/engine" not in cmake_text:
        fail("CMake install rule no longer installs include/scratchbird/engine")
    if "add_library(sb_engine_shared SHARED" not in cmake_text:
        fail("shared public engine library target is missing")


def validate_driver_manifest(repo_root: Path, manifest: dict) -> None:
    driver = manifest["driver_manifest"]
    path = repo_root / driver["path"]
    rows = list(csv.DictReader(path.open(newline="", encoding="utf-8")))
    if len(rows) < driver["minimum_rows"]:
        fail(f"driver manifest row count below frozen minimum: {len(rows)}")

    allowed_surfaces = set(driver["allowed_api_surface_sets"])
    allowed_ingress = set(driver["allowed_ingress_modes"])
    for row in rows:
        if not (repo_root / row["source_path"]).exists():
            fail(f"driver source path missing for {row['component_id']}: {row['source_path']}")
        surfaces = set(row["api_surface_set"].split(";"))
        ingress_modes = set(row["ingress_mode_set"].split(";"))
        if not surfaces <= allowed_surfaces:
            fail(f"driver {row['component_id']} has undocumented API surface {surfaces - allowed_surfaces}")
        if not ingress_modes <= allowed_ingress:
            fail(f"driver {row['component_id']} has undocumented ingress mode {ingress_modes - allowed_ingress}")
        if row["wire_protocol_set"] != driver["wire_protocol_set"]:
            fail(f"driver {row['component_id']} wire protocol drifted: {row['wire_protocol_set']}")


def validate_parser_udr_contract(repo_root: Path, manifest: dict) -> None:
    contract = manifest["parser_udr_contract"]
    text = read(repo_root / contract["path"])
    for required in (
        contract["package_uuid_constant"],
        contract["package_name_constant"],
        "sbu_sbsql_package_descriptor",
    ):
        if required not in text:
            fail(f"parser UDR contract missing {required}")
    for entrypoint in contract["entrypoints"]:
        if entrypoint not in text:
            fail(f"parser UDR entrypoint missing from header: {entrypoint}")

    runtime_text = read(repo_root / "project/src/udr/runtime/sb_udr_runtime.hpp")
    for token in ("UdrPackageDescriptor", "abi_version", "UdrEntrypointDescriptor"):
        if token not in runtime_text:
            fail(f"UDR runtime contract missing {token}")

    parser_registry = read(repo_root / "project/src/server/parser_package_registry.hpp")
    for token in ("parser_api_major = 3", "parser_api_minor = 0", 'parser_support_udr_abi = "sb_udr_v1"'):
        if token not in parser_registry:
            fail(f"parser package registry contract missing {token}")

    native_parser = read(repo_root / "project/src/parsers/native/v3/package/native_v3_parser_package.hpp")
    for token in (
        "NativeV3ParserPackageRequest",
        "NativeV3ParserPackageResult",
        "sblr_parser_resolved_names_to_uuids",
        "dispatched_to_engine_api",
    ):
        if token not in native_parser:
            fail(f"native parser package contract missing {token}")


def validate_extension_boundaries(repo_root: Path, manifest: dict) -> None:
    boundary_text = read(repo_root / "project/src/engine/internal_api/extensibility/extension_boundary_manifest.cpp")
    count_match = re.search(r"std::array<EngineExtensionBoundaryManifestEntry,\s*([0-9]+)>", boundary_text)
    if not count_match:
        fail("extension boundary manifest array size missing")
    if int(count_match.group(1)) != len(manifest["extension_boundaries"]):
        fail("extension boundary manifest row count differs from source")

    for entry in manifest["extension_boundaries"]:
        for field in (
            "boundary_id",
            "boundary_type",
            "contract_version",
            "core_classification",
            "non_cluster_refusal_code",
        ):
            value = entry[field]
            if value != "none" and f'"{value}"' not in boundary_text:
                fail(f"extension boundary source missing {field}={value}")

    cluster_header = read(repo_root / "project/src/cluster_provider/cluster_provider.hpp")
    if manifest["surfaces"][-1]["non_cluster_refusal_code"] not in cluster_header:
        fail("cluster provider header no longer exposes the frozen non-cluster refusal code")
    for token in ("ClusterProviderSupportsExecution", "InspectClusterProvider", "ExecuteClusterOperation"):
        if token not in cluster_header:
            fail(f"cluster provider boundary missing {token}")


def validate_docs(repo_root: Path, manifest: dict, docs_path: Path) -> None:
    docs = read(docs_path)
    for token in (
        manifest["manifest_schema"],
        manifest["freeze_id"],
        manifest["abi_version"]["packed_hex"],
        "engine_sblr_internal_api_only",
        "SQL text is never runtime authority",
        "MGA transaction inventory remains finality authority",
    ):
        if token not in docs:
            fail(f"human-readable docs missing {token}")
    for header in manifest["public_headers"]:
        if header not in docs:
            fail(f"human-readable docs missing public header {header}")
    for symbol in manifest["c_api_symbols"]:
        if symbol not in docs:
            fail(f"human-readable docs missing C ABI symbol {symbol}")
    for surface in manifest["surfaces"]:
        if surface["surface_id"] not in docs:
            fail(f"human-readable docs missing surface {surface['surface_id']}")
    for boundary in manifest["extension_boundaries"]:
        if boundary["boundary_id"] not in docs:
            fail(f"human-readable docs missing extension boundary {boundary['boundary_id']}")


def validate_boundaries(manifest: dict) -> None:
    boundaries = manifest["architectural_boundaries"]
    expected = {
        "engine_execution_authority": "engine_sblr_internal_api_only",
        "sql_text_authoritative": False,
        "uuid_identity_authoritative": True,
        "mga_finality_authoritative": True,
        "cluster_positive_behavior_in_core": False,
    }
    if boundaries != expected:
        fail(f"architectural boundary claims drifted: {boundaries}")
    for surface in manifest["surfaces"]:
        authority = surface["execution_authority"]
        if surface["classification"] in {"core", "core_parser_package"}:
            if "engine" not in authority and "parser_translation_only" not in authority:
                fail(f"core surface has weak authority claim: {surface['surface_id']}")
        if surface["classification"].startswith("non_core_cluster"):
            if "provider" not in authority:
                fail(f"cluster surface lacks provider boundary: {surface['surface_id']}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    args = parser.parse_args()

    repo_root = Path(args.repo_root).resolve()
    manifest_path = repo_root / "project/docs/public_api/CORE_BETA_PUBLIC_API_ABI_MANIFEST.json"
    docs_path = repo_root / "project/docs/public_api/CORE_BETA_PUBLIC_API_ABI.md"
    manifest = json.loads(read(manifest_path))

    if str(manifest_path).find("docs" "/execution-plans/core-beta-qa-readiness-gap-closure") != -1:
        fail("manifest path depends on active execution_plan artifacts")
    validate_boundaries(manifest)
    validate_public_headers(repo_root, manifest)
    validate_driver_manifest(repo_root, manifest)
    validate_parser_udr_contract(repo_root, manifest)
    validate_extension_boundaries(repo_root, manifest)
    validate_docs(repo_root, manifest, docs_path)
    print("public_headers_api_docs_freeze_static=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
