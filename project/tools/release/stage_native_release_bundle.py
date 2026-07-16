#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Create an allowlisted native-SB distribution tree from proof build output."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys


PROFILE_SCHEMA = "scratchbird.native_release_profile.v1"
PROFILE_NAME = "native-sbsql-only"

EMBEDDED_TLS_MARKERS = (
    "-----BEGIN " + "PRIVATE KEY-----",
    "-----BEGIN " + "CERTIFICATE-----",
)

NATIVE_TOPOLOGY = {
    "manager": "optional_front_door_disabled_by_default",
    "listener_executable": "SBgate",
    "listener_model": "one_shared_listener_executable_for_all_parser_families",
    "parser_process_model": "one_standalone_parser_for_the_selected_dialect",
    "native_route": "client_to_SBgate_to_SBParser_to_SBPS_IPC_to_SBsrv_engine",
    "parser_engine_transport": "sbps_ipc_only",
    "direct_engine_link": "forbidden",
    "cross_parser_dependency": "forbidden",
}

NATIVE_EXECUTABLES = (
    "SBsrv",
    "SBgate",
    "SBmgr",
    "SBParser",
    "SBsql",
    "SBadm",
    "SBbak",
    "SBsec",
    "SBdoc",
    "SBcop",
)

NATIVE_CONFIGS = (
    "SBsrv.conf",
    "SBgate.conf",
    "SBmgr.conf",
    "SBParser.conf",
    "SBbootstrap.profile",
)

REQUIRED_RESOURCE_DIRS = (
    "resources/seed-packs/initial-resource-pack/resources/charsets",
    "resources/seed-packs/initial-resource-pack/resources/collations",
    "resources/seed-packs/initial-resource-pack/resources/timezones",
    "resources/seed-packs/initial-resource-pack/resources/i18n",
    "resources/seed-packs/initial-resource-pack/resources/i18n/sbsql-language-resource-pack",
    "resources/seed-packs/initial-resource-pack/resources/i18n/sbsql-language-resource-pack/resources",
    "resources/policy-packs/default-local-password/policies",
)

REQUIRED_RESOURCE_FILES = (
    "resources/seed-packs/initial-resource-pack/RESOURCE_SEED_MANIFEST.csv",
    "resources/seed-packs/initial-resource-pack/RESOURCE_SEED_ARTIFACTS.csv",
    "resources/seed-packs/initial-resource-pack/resources/charsets/charsets.json",
    "resources/seed-packs/initial-resource-pack/resources/collations/collations.json",
    "resources/seed-packs/initial-resource-pack/resources/timezones/version",
    "resources/seed-packs/initial-resource-pack/resources/i18n/version",
    "resources/seed-packs/initial-resource-pack/resources/i18n/sbsql-language-resource-pack/manifest.sblrp.json",
    "resources/seed-packs/initial-resource-pack/resources/i18n/sbsql-language-resource-pack/manifest.sblrp.sig",
    "resources/seed-packs/initial-resource-pack/resources/i18n/sbsql-language-resource-pack/hashes.sha256",
    "resources/policy-packs/default-local-password/POLICY_PACK_MANIFEST.json",
    "resources/policy-packs/default-local-password/catalog_materialization.json",
    "resources/policy-packs/default-local-password/policies/security_providers.json",
    "resources/policy-packs/default-local-password/policies/roles.json",
    "resources/policy-packs/default-local-password/policies/groups.json",
    "resources/policy-packs/default-local-password/policies/grants.json",
    "resources/policy-packs/default-local-password/policies/policy_profiles.json",
    "resources/policy-packs/default-local-password/policies/server_memory_cache_policy.json",
    "resources/policy-packs/default-local-password/policies/default_policy_catalog.json",
    "resources/policy-packs/default-local-password/policies/policy_defaults.json",
)

REQUIRED_OPERABILITY_FILES = (
    "docs/public_api/CORE_BETA_PUBLIC_API_ABI_MANIFEST.json",
    "docs/release/PUBLIC_SUPPORT_MAINTENANCE_POLICY.md",
    "examples/core_beta_qa/manifest.json",
    "examples/native_release_qa/README.md",
    "examples/native_release_qa/prepare_native_qa_instance.py",
)

NATIVE_SHARE_SUBTREES = (
    "resources",
    "docs/public_api",
    "docs/release",
    "examples/core_beta_qa",
    "examples/native_release_qa",
)

NATIVE_DOC_SUBTREES = {"public_api", "release"}
NATIVE_EXAMPLE_SUBTREES = {"core_beta_qa", "native_release_qa"}

REQUIRED_CONFIG_TOKENS = {
    "SBsrv.conf": (
        "provider_family = local_password",
        "default_policy_installed = true",
        "parser_executable_path = bin/SBParser",
        "port = 3092",
        "tls_required = true",
        "failure_mode = return_error",
    ),
    "SBgate.conf": (
        "protocol_family = sbsql",
        "parser_package = SBParser",
        "parser_executable = bin/SBParser",
        "port = 3092",
        "tls_required = true",
        "dbbt_key_source = keyring",
    ),
    "SBmgr.conf": (
        "manager.release.profile = enterprise",
        "manager.proxy.enabled = false",
        "manager.proxy.bind = 127.0.0.1",
        "manager.proxy.port = 3092",
        "manager.proxy.tls_required = true",
        "manager.backend.native_port = 0",
    ),
    "SBParser.conf": (
        "parser.family = sbsql",
        "parser.worker_binary = bin/SBParser",
        "parser.security.auth_relay_required = true",
        "parser.execution.engine_authority_required = true",
        "parser.execution.engine_transport = sbps_ipc_only",
        "parser.execution.direct_engine_link = forbidden",
        "parser.execution.cross_parser_dependency = forbidden",
    ),
    "SBbootstrap.profile": (
        "schema_id = scratchbird.bootstrap_platform_profile.v1",
        "platform = operator_required",
        "service_identity = operator_required",
        "service_group = operator_required",
    ),
}

REQUIRED_LIBRARY_CANDIDATES = {
    "linux": {
        "engine_shared": ("libSBcore.so",),
        "engine_static": ("libSBcore_static.a",),
        "native_parser_udr": ("libSBParser_udr.a",),
    },
    "macos": {
        "engine_shared": ("libSBcore.dylib", "libSBcore.so"),
        "engine_static": ("libSBcore_static.a",),
        "native_parser_udr": ("libSBParser_udr.a",),
    },
    "windows": {
        "engine_shared": ("SBcore.dll", "libSBcore.dll"),
        "engine_static": ("SBcore_static.lib", "libSBcore_static.a"),
        "native_parser_udr": ("SBParser_udr.lib", "libSBParser_udr.a"),
    },
}

OPTIONAL_NATIVE_LIBRARY_NAMES = {
    "libSBcore.dll.a",
    "SBcore.lib",
    "libscratchbird_client.a",
    "scratchbird_client.lib",
    "libscratchbird_odbc.so",
    "libscratchbird_odbc.dylib",
    "scratchbird_odbc.dll",
    "libscratchbird_odbc.dll.a",
    "scratchbird_odbc.lib",
    "libscratchbird_mojo_client_bridge.so",
    "libscratchbird_mojo_client_bridge.dylib",
    "scratchbird_mojo_client_bridge.dll",
}

WINDOWS_SYSTEM_DLLS = {
    "advapi32.dll",
    "bcrypt.dll",
    "cabinet.dll",
    "comdlg32.dll",
    "crypt32.dll",
    "dbghelp.dll",
    "dnsapi.dll",
    "gdi32.dll",
    "imagehlp.dll",
    "imm32.dll",
    "iphlpapi.dll",
    "kernel32.dll",
    "msvcrt.dll",
    "netapi32.dll",
    "normaliz.dll",
    "ntdll.dll",
    "ole32.dll",
    "oleaut32.dll",
    "powrprof.dll",
    "psapi.dll",
    "rpcrt4.dll",
    "secur32.dll",
    "setupapi.dll",
    "shell32.dll",
    "shlwapi.dll",
    "ucrtbase.dll",
    "user32.dll",
    "userenv.dll",
    "version.dll",
    "winhttp.dll",
    "winmm.dll",
    "ws2_32.dll",
    "wtsapi32.dll",
}

FORBIDDEN_RUNTIME_NAME_FRAGMENTS = (
    "firebird",
    "mysql",
    "postgres",
    "sqlite",
    "mariadb",
    "duckdb",
    "clickhouse",
    "cockroach",
    "yugabyte",
    "cassandra",
    "mongodb",
    "redis",
    "opensearch",
    "neo4j",
    "influx",
    "milvus",
    "vitess",
)

LLVM_MINIMUM_MAJOR = {
    "linux": 23,
    "windows": 22,
    "macos": 22,
}

LLVM_RUNTIME_DELIVERY = {
    "linux": "system-package",
    "windows": "bundled",
    "macos": "external-homebrew",
}

def fail(message: str) -> None:
    print(f"stage_native_release_bundle=fail:{message}", file=sys.stderr)
    raise SystemExit(1)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def canonical_resource_bytes(path: Path) -> bytes:
    data = path.read_bytes()
    if path.name.endswith(".tar.gz"):
        return data
    return data.replace(b"\r\n", b"\n").replace(b"\r", b"\n")


def fnv1a64(data: bytes) -> str:
    value = 0xCBF29CE484222325
    for byte in data:
        value ^= byte
        value = (value * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return f"fnv1a64:{value:016x}"


def safe_relative_path(root: Path, raw: str, label: str) -> Path:
    relative = Path(raw)
    if not raw or relative.is_absolute() or ".." in relative.parts:
        fail(f"unsafe_manifest_path:{label}:{raw}")
    path = root / relative
    if path.is_symlink():
        fail(f"symlink_forbidden:{label}:{raw}")
    return path


def validate_resource_seed_pack(seed_root: Path) -> dict[str, int]:
    artifact_index = require_regular_file(
        seed_root / "RESOURCE_SEED_ARTIFACTS.csv", "resource_artifact_index"
    )
    with artifact_index.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        if reader.fieldnames != [
            "canonical_path",
            "content_hash",
            "content_size_bytes",
        ]:
            fail("resource_artifact_index_header_invalid")
        rows = list(reader)
    if not rows:
        fail("resource_artifact_index_empty")

    declared: set[str] = set()
    family_counts: dict[str, int] = {}
    for row in rows:
        relative = row["canonical_path"]
        if relative in declared:
            fail(f"resource_artifact_duplicate:{relative}")
        declared.add(relative)
        path = require_regular_file(
            safe_relative_path(seed_root, relative, "resource_artifact"),
            f"resource_artifact:{relative}",
        )
        content = canonical_resource_bytes(path)
        if row["content_hash"] != fnv1a64(content):
            fail(f"resource_artifact_hash_mismatch:{relative}")
        try:
            expected_size = int(row["content_size_bytes"])
        except ValueError:
            fail(f"resource_artifact_size_invalid:{relative}")
        if expected_size != len(content):
            fail(f"resource_artifact_size_mismatch:{relative}")
        parts = Path(relative).parts
        family = parts[1] if len(parts) > 1 and parts[0] == "resources" else "other"
        family_counts[family] = family_counts.get(family, 0) + 1

    actual = {
        path.relative_to(seed_root).as_posix()
        for path in (seed_root / "resources").rglob("*")
        if path.is_file()
    }
    if actual != declared:
        fail(
            "resource_artifact_set_mismatch:"
            f"missing={sorted(declared - actual)}:unexpected={sorted(actual - declared)}"
        )

    seed_manifest = require_regular_file(
        seed_root / "RESOURCE_SEED_MANIFEST.csv", "resource_seed_manifest"
    )
    with seed_manifest.open(newline="", encoding="utf-8") as handle:
        manifest_rows = list(csv.DictReader(handle))
    if not manifest_rows:
        fail("resource_seed_manifest_empty")
    for row in manifest_rows:
        family = row.get("seed_family", "")
        patterns = row.get("source_pattern", "").split(";")
        if not family or not patterns or any(not pattern for pattern in patterns):
            fail(f"resource_seed_manifest_row_invalid:{family}")
        for pattern in patterns:
            matches = [path for path in seed_root.glob(pattern) if path.is_file()]
            if not matches:
                fail(f"resource_seed_pattern_unmatched:{family}:{pattern}")
    return dict(sorted(family_counts.items()))


def canonical_policy_bytes(path: Path) -> bytes:
    return path.read_bytes().replace(b"\r\n", b"\n").replace(b"\r", b"\n")


def validate_policy_pack(policy_root: Path) -> int:
    manifest_path = require_regular_file(
        policy_root / "POLICY_PACK_MANIFEST.json", "policy_pack_manifest"
    )
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        fail(f"policy_pack_manifest_invalid:{exc}")
    if manifest.get("policy_pack_id") != "default-local-password":
        fail("policy_pack_identity_mismatch")
    rows = manifest.get("content_manifest")
    if not isinstance(rows, list) or not rows:
        fail("policy_pack_content_manifest_invalid")
    declared: set[str] = set()
    aggregate = bytearray()
    for row in rows:
        if not isinstance(row, dict):
            fail("policy_pack_content_row_invalid")
        relative = str(row.get("path", ""))
        if relative in declared:
            fail(f"policy_pack_content_duplicate:{relative}")
        declared.add(relative)
        path = require_regular_file(
            safe_relative_path(policy_root, relative, "policy_pack_content"),
            f"policy_pack_content:{relative}",
        )
        digest = hashlib.sha256(canonical_policy_bytes(path)).hexdigest()
        if row.get("sha256") != digest:
            fail(f"policy_pack_content_hash_mismatch:{relative}")
        aggregate.extend(relative.encode("utf-8"))
        aggregate.extend(b"\0")
        aggregate.extend(digest.encode("ascii"))
        aggregate.extend(b"\n")
    if manifest.get("content_sha256") != hashlib.sha256(aggregate).hexdigest():
        fail("policy_pack_aggregate_hash_mismatch")
    declared_policies = {path for path in declared if path.startswith("policies/")}
    actual_policies = {
        path.relative_to(policy_root).as_posix()
        for path in (policy_root / "policies").rglob("*")
        if path.is_file()
    }
    if actual_policies != declared_policies:
        fail("policy_pack_policy_file_set_mismatch")
    return len(rows)


def require_native_configs(
    config_root: Path,
    *,
    required_tokens: dict[str, tuple[str, ...]] | None = None,
    forbidden_tokens: dict[str, tuple[str, ...]] | None = None,
) -> None:
    required = REQUIRED_CONFIG_TOKENS if required_tokens is None else required_tokens
    forbidden = {} if forbidden_tokens is None else forbidden_tokens
    if set(required) != set(REQUIRED_CONFIG_TOKENS):
        fail("native_config_required_token_file_set_mismatch")
    if set(forbidden) - set(REQUIRED_CONFIG_TOKENS):
        fail("native_config_forbidden_token_file_set_mismatch")
    for file_name, tokens in required.items():
        path = require_regular_file(config_root / file_name, f"native_config:{file_name}")
        text = path.read_text(encoding="utf-8")
        if re.search(
            r"(?m)^\s*(?:port|manager[.]proxy[.]port|manager[.]backend[.]native_port)\s*=\s*(?:3050|3090|3392)\s*$",
            text,
        ):
            fail(f"native_config_forbidden_native_port:{file_name}")
        if file_name == "SBmgr.conf":
            backend_ports = re.findall(
                r"(?m)^\s*manager[.]backend[.]native_port\s*=\s*([0-9]+)\s*$",
                text,
            )
            if backend_ports != ["0"]:
                fail("native_manager_backend_must_be_unset")
        for token in tokens:
            if token not in text:
                fail(f"native_config_token_missing:{file_name}:{token}")
        for token in forbidden.get(file_name, ()):
            if token in text:
                fail(f"native_config_forbidden_token:{file_name}:{token}")
        if any(marker in text for marker in EMBEDDED_TLS_MARKERS):
            fail(f"native_config_embedded_tls_material_forbidden:{file_name}")


def require_regular_file(path: Path, label: str) -> Path:
    if path.is_symlink():
        fail(f"symlink_forbidden:{label}:{path}")
    if not path.is_file() or path.stat().st_size <= 0:
        fail(f"required_file_missing:{label}:{path}")
    return path


def copy_file(source: Path, destination: Path) -> None:
    require_regular_file(source, "copy_source")
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)
    if sha256(source) != sha256(destination):
        fail(f"copy_hash_mismatch:{source}:{destination}")


def copy_tree(source: Path, destination: Path) -> None:
    if not source.is_dir():
        fail(f"required_directory_missing:{source}")
    for path in source.rglob("*"):
        if path.is_symlink():
            fail(f"symlink_forbidden:copy_tree:{path}")
    shutil.copytree(source, destination)


def platform_executable(name: str, platform: str) -> str:
    return f"{name}.exe" if platform == "windows" else name


def locate_library(source_root: Path, platform: str, candidates: tuple[str, ...]) -> Path:
    search_roots = (source_root / "lib", source_root / "bin")
    matches = [
        root / candidate
        for root in search_roots
        for candidate in candidates
        if (root / candidate).is_file()
    ]
    if len(matches) != 1:
        fail(
            "required_library_cardinality:"
            f"{platform}:{','.join(candidates)}:{len(matches)}"
        )
    return matches[0]


def parse_pe_dependencies(output: str) -> set[str]:
    return {
        match.group(1).strip()
        for match in re.finditer(r"(?im)^\s*DLL Name:\s*([^\s]+)\s*$", output)
    }


def windows_system_dll(name: str) -> bool:
    lowered = name.lower()
    return (
        lowered in WINDOWS_SYSTEM_DLLS
        or lowered.startswith("api-ms-win-")
        or lowered.startswith("ext-ms-win-")
    )


def safe_runtime_dependency_name(name: str) -> bool:
    lowered = name.lower()
    return (
        Path(name).name == name
        and lowered.endswith(".dll")
        and not any(fragment in lowered for fragment in FORBIDDEN_RUNTIME_NAME_FRAGMENTS)
        and not lowered.startswith("sbp_")
        and not lowered.startswith("sbu_")
    )


def require_llvm_runtime_contract(
    value: object,
    platform: str,
) -> dict[str, object]:
    if not isinstance(value, dict):
        fail("llvm_runtime_contract_missing")
    link_mode = value.get("link_mode")
    runtime_library = value.get("runtime_library")
    runtime_paths = value.get("runtime_libraries_by_architecture")
    delivery = value.get("delivery")
    minimum_major = value.get("minimum_major")
    if link_mode != "dynamic":
        fail(f"llvm_runtime_link_mode_invalid:{link_mode}")
    if platform == "macos" and runtime_paths is not None:
        if runtime_library is not None:
            fail("macos_universal_llvm_runtime_scalar_must_be_null")
    elif not isinstance(runtime_library, str) or not runtime_library:
        fail("llvm_runtime_library_missing")
    if delivery != LLVM_RUNTIME_DELIVERY[platform]:
        fail(f"llvm_runtime_delivery_invalid:{platform}:{delivery}")
    if (
        not isinstance(minimum_major, int)
        or isinstance(minimum_major, bool)
        or minimum_major < LLVM_MINIMUM_MAJOR[platform]
    ):
        fail(f"llvm_runtime_minimum_major_invalid:{platform}:{minimum_major}")

    if platform == "linux":
        if Path(runtime_library).name != runtime_library or not re.fullmatch(
            r"libLLVM(?:-[0-9]+)?[.]so(?:[.][0-9]+)*", runtime_library
        ):
            fail(f"linux_llvm_runtime_soname_invalid:{runtime_library}")
    elif platform == "windows":
        if (
            not safe_runtime_dependency_name(runtime_library)
            or "llvm" not in runtime_library.lower()
            or not re.fullmatch(r"(?i)libLLVM-[0-9]+(?:[^/]*)[.]dll", runtime_library)
        ):
            fail(f"windows_llvm_runtime_dll_invalid:{runtime_library}")
    elif platform == "macos":
        if runtime_paths is None:
            if not re.fullmatch(
                r"/(?:opt/homebrew|usr/local)/opt/llvm/lib/libLLVM(?:-[0-9]+)?[.]dylib",
                runtime_library,
            ):
                fail(f"macos_llvm_runtime_homebrew_path_invalid:{runtime_library}")
        else:
            if not isinstance(runtime_paths, dict) or set(runtime_paths) != {
                "x86_64",
                "arm64",
            }:
                fail("macos_universal_llvm_runtime_architecture_map_invalid")
            expected_prefixes = {
                "x86_64": "/usr/local/opt/llvm/lib/",
                "arm64": "/opt/homebrew/opt/llvm/lib/",
            }
            for architecture, expected_prefix in expected_prefixes.items():
                path = runtime_paths.get(architecture)
                if (
                    not isinstance(path, str)
                    or not path.startswith(expected_prefix)
                    or not re.fullmatch(
                        r"/(?:opt/homebrew|usr/local)/opt/llvm/lib/libLLVM(?:-[0-9]+)?[.]dylib",
                        path,
                    )
                ):
                    fail(
                        "macos_universal_llvm_runtime_path_invalid:"
                        f"{architecture}:{path}"
                    )
    else:
        fail(f"unsupported_platform:{platform}")
    contract = {
        "link_mode": link_mode,
        "runtime_library": runtime_library,
        "delivery": delivery,
        "minimum_major": minimum_major,
    }
    if platform == "macos" and runtime_paths is not None:
        contract["runtime_libraries_by_architecture"] = dict(
            runtime_paths
        )
    return contract


def locate_case_insensitive(name: str, roots: tuple[Path, ...]) -> Path | None:
    lowered = name.lower()
    matches = [
        path
        for root in roots
        if root.is_dir()
        for path in root.iterdir()
        if path.is_file() and path.name.lower() == lowered
    ]
    if len(matches) > 1:
        hashes = {sha256(path) for path in matches}
        if len(hashes) != 1:
            fail(f"runtime_dependency_ambiguous:{name}:{matches}")
    return matches[0] if matches else None


def stage_windows_runtime_dependencies(
    output_root: Path,
    search_roots: tuple[Path, ...],
    required_dependencies: tuple[str, ...] = (),
) -> list[str]:
    objdump = shutil.which("objdump")
    if not objdump:
        fail("windows_objdump_not_found")
    roots = (output_root / "bin", *search_roots)
    copied: set[str] = set()
    for dependency in required_dependencies:
        if not safe_runtime_dependency_name(dependency):
            fail(f"windows_runtime_dependency_forbidden:{dependency}")
        source = locate_case_insensitive(dependency, search_roots)
        if source is None:
            fail(f"windows_required_runtime_dependency_unresolved:{dependency}")
        destination = output_root / "bin" / source.name
        copy_file(source, destination)
        copied.add(destination.name)

    queue = sorted((output_root / "bin").glob("*.exe")) + sorted(
        (output_root / "bin").glob("*.dll")
    )
    inspected: set[str] = set()
    while queue:
        binary = queue.pop(0)
        key = binary.name.lower()
        if key in inspected:
            continue
        inspected.add(key)
        result = subprocess.run(
            [objdump, "-p", str(binary)],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        if result.returncode != 0:
            fail(f"windows_objdump_failed:{binary}:{result.returncode}")
        for dependency in sorted(parse_pe_dependencies(result.stdout)):
            if windows_system_dll(dependency):
                continue
            if not safe_runtime_dependency_name(dependency):
                fail(f"windows_runtime_dependency_forbidden:{dependency}")
            existing = locate_case_insensitive(dependency, (output_root / "bin",))
            if existing is not None:
                queue.append(existing)
                continue
            source = locate_case_insensitive(dependency, roots)
            if source is None:
                fail(f"windows_runtime_dependency_unresolved:{binary.name}:{dependency}")
            destination = output_root / "bin" / source.name
            copy_file(source, destination)
            copied.add(destination.name)
            queue.append(destination)
    return sorted(copied)


def source_manifest(source_root: Path, platform: str) -> dict[str, object]:
    manifest_path = require_regular_file(
        source_root / "STANDALONE_OUTPUT_MANIFEST.json", "source_manifest"
    )
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        fail(f"source_manifest_invalid:{exc}")
    if manifest.get("platform") != platform:
        fail(f"source_manifest_platform_mismatch:{manifest.get('platform')}:{platform}")
    return manifest


def require_operational_resources(share_root: Path) -> dict[str, object]:
    scratchbird_root = share_root / "scratchbird"
    for rel in REQUIRED_RESOURCE_DIRS:
        if not (scratchbird_root / rel).is_dir():
            fail(f"operational_resource_directory_missing:{rel}")
    for rel in REQUIRED_RESOURCE_FILES:
        require_regular_file(
            scratchbird_root / rel,
            f"operational_resource:{rel}",
        )
    for rel in REQUIRED_OPERABILITY_FILES:
        require_regular_file(
            scratchbird_root / rel,
            f"operability_file:{rel}",
        )
    seed_root = (
        scratchbird_root
        / "resources"
        / "seed-packs"
        / "initial-resource-pack"
    )
    policy_root = (
        scratchbird_root
        / "resources"
        / "policy-packs"
        / "default-local-password"
    )
    return {
        "resource_artifact_counts": validate_resource_seed_pack(seed_root),
        "policy_content_file_count": validate_policy_pack(policy_root),
    }


def require_native_share_layout(share_root: Path) -> None:
    scratchbird_root = share_root / "scratchbird"
    for parent_name, expected in (
        ("docs", NATIVE_DOC_SUBTREES),
        ("examples", NATIVE_EXAMPLE_SUBTREES),
    ):
        parent = scratchbird_root / parent_name
        if not parent.is_dir():
            fail(f"native_share_parent_missing:{parent_name}")
        actual = {entry.name for entry in parent.iterdir()}
        if actual != expected:
            fail(
                f"native_share_subtree_set_mismatch:{parent_name}:"
                f"missing={sorted(expected - actual)}:"
                f"unexpected={sorted(actual - expected)}"
            )


def stage(
    source_root: Path,
    output_root: Path,
    platform: str,
    runtime_search_roots: tuple[Path, ...] = (),
) -> None:
    if platform not in REQUIRED_LIBRARY_CANDIDATES:
        fail(f"unsupported_platform:{platform}")
    source_root = source_root.resolve()
    output_root = output_root.resolve()
    if source_root == output_root or source_root in output_root.parents:
        fail("output_must_not_be_inside_source")
    build_manifest = source_manifest(source_root, platform)
    runtime_requirements = build_manifest.get("runtime_requirements")
    if not isinstance(runtime_requirements, dict):
        fail("runtime_requirements_missing")
    llvm_runtime = require_llvm_runtime_contract(
        runtime_requirements.get("llvm"), platform
    )
    source_resource_summary = require_operational_resources(source_root / "share")
    require_native_configs(source_root / "etc" / "scratchbird")

    if output_root.exists():
        shutil.rmtree(output_root)
    (output_root / "bin").mkdir(parents=True)
    (output_root / "lib").mkdir(parents=True)

    copied_bins: list[str] = []
    for name in NATIVE_EXECUTABLES:
        file_name = platform_executable(name, platform)
        copy_file(source_root / "bin" / file_name, output_root / "bin" / file_name)
        copied_bins.append(file_name)

    copied_libraries: list[str] = []
    for label, candidates in REQUIRED_LIBRARY_CANDIDATES[platform].items():
        source = locate_library(source_root, platform, candidates)
        destination_dir = "bin" if source.parent.name == "bin" else "lib"
        copy_file(source, output_root / destination_dir / source.name)
        copied_libraries.append(f"{destination_dir}/{source.name}")

    for source_dir in (source_root / "lib", source_root / "bin"):
        if not source_dir.is_dir():
            continue
        for source in sorted(source_dir.iterdir()):
            if source.name not in OPTIONAL_NATIVE_LIBRARY_NAMES or not source.is_file():
                continue
            destination_dir = "bin" if source_dir.name == "bin" else "lib"
            destination = output_root / destination_dir / source.name
            if not destination.exists():
                copy_file(source, destination)
                copied_libraries.append(f"{destination_dir}/{source.name}")

    config_root = source_root / "etc" / "scratchbird"
    for file_name in NATIVE_CONFIGS:
        copy_file(
            config_root / file_name,
            output_root / "etc" / "scratchbird" / file_name,
        )
    source_scratchbird_share = source_root / "share" / "scratchbird"
    output_scratchbird_share = output_root / "share" / "scratchbird"
    for relative in NATIVE_SHARE_SUBTREES:
        copy_tree(
            source_scratchbird_share / relative,
            output_scratchbird_share / relative,
        )
    output_resource_summary = require_operational_resources(output_root / "share")
    if output_resource_summary != source_resource_summary:
        fail("operational_resource_summary_copy_mismatch")
    require_native_configs(output_root / "etc" / "scratchbird")
    require_native_share_layout(output_root / "share")

    runtime_dependencies: list[str] = []
    if platform == "windows":
        runtime_dependencies = stage_windows_runtime_dependencies(
            output_root,
            tuple(path.resolve() for path in runtime_search_roots),
            (str(llvm_runtime["runtime_library"]),),
        )
    elif runtime_search_roots:
        fail(f"runtime_search_root_unsupported_for_platform:{platform}")

    standalone_manifest = {
        "product": "ScratchBird Convergent Data Engine",
        "short_brand": "SBcde",
        "platform": platform,
        "artifact_root": f"<scratchbird-native-{platform}-artifact-root>",
        "distribution_profile": PROFILE_NAME,
        "emulation_components": "excluded",
        "topology": NATIVE_TOPOLOGY,
        "runtime_requirements": {"llvm": llvm_runtime},
        "layout": {
            "bin": "bin",
            "lib": "lib",
            "configuration": "etc/scratchbird",
            "resources": "share/scratchbird/resources",
            "docs": "share/scratchbird/docs",
            "examples": "share/scratchbird/examples",
        },
    }
    (output_root / "STANDALONE_OUTPUT_MANIFEST.json").write_text(
        json.dumps(standalone_manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    profile = {
        "schema_id": PROFILE_SCHEMA,
        "profile": PROFILE_NAME,
        "platform": platform,
        "native_parser": "SBSQL",
        "emulation_components": "excluded",
        "topology": NATIVE_TOPOLOGY,
        "resource_policy": (
            "Core catalog charset/collation seed data is retained; no compatibility "
            "parser, compatibility parser-support UDR, listener profile, or emulation "
            "executable is included."
        ),
        "executables": sorted(copied_bins),
        "libraries": sorted(copied_libraries),
        "runtime_dependencies": runtime_dependencies,
        "llvm_runtime": llvm_runtime,
        "configuration": list(NATIVE_CONFIGS),
        "required_resource_directories": list(REQUIRED_RESOURCE_DIRS),
        "required_resource_files": list(REQUIRED_RESOURCE_FILES),
        "required_operability_files": list(REQUIRED_OPERABILITY_FILES),
        "native_share_subtrees": list(NATIVE_SHARE_SUBTREES),
        **source_resource_summary,
    }
    (output_root / "NATIVE_RELEASE_PROFILE.json").write_text(
        json.dumps(profile, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(f"stage_native_release_bundle=passed:{output_root}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--platform", choices=tuple(REQUIRED_LIBRARY_CANDIDATES), required=True)
    parser.add_argument("--runtime-search-root", type=Path, action="append", default=[])
    args = parser.parse_args()
    stage(
        args.source_root,
        args.output_root,
        args.platform,
        tuple(args.runtime_search_root),
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
