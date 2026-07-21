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
import stat
import subprocess
import sys
import tarfile


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

# SBlaunch is a macOS-only privilege-drop boundary for launchd.  It is not a
# server, listener, manager, parser, or emulation component and must never
# expand the Linux or Windows native release contract.
PLATFORM_NATIVE_EXECUTABLES = {
    "macos": ("SBlaunch",),
}


def native_executables(platform: str) -> tuple[str, ...]:
    return NATIVE_EXECUTABLES + PLATFORM_NATIVE_EXECUTABLES.get(platform, ())


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
NATIVE_SHARE_TOP_LEVELS = {"resources", "docs", "examples"}

# Native-server releases deliberately do not carry an unadmitted client
# surface.  The output is assembled from a few broad resource/document/example
# directories, so a bin/lib allowlist alone is insufficient: a client archive
# or renamed native library could otherwise be hidden under share/.  Keep this
# policy local to the native bundle and apply it recursively to both staged and
# installed payloads.
NATIVE_SHARE_CLIENT_COMPONENTS = frozenset(
    {
        "driver",
        "drivers",
        "adapter",
        "adapters",
        "adaptor",
        "adaptors",
        "mcp",
        "mcps",
    }
)
NATIVE_SHARE_CLIENT_NAME_FRAGMENTS = (
    "dbeaver",
    "scratchbird_client",
    "scratchbird_odbc",
    "scratchbird_mojo_client_bridge",
)
NATIVE_SHARE_FORBIDDEN_SUFFIXES = (
    ".a",
    ".apk",
    ".appx",
    ".appxbundle",
    ".cab",
    ".class",
    ".dll",
    ".dmg",
    ".dylib",
    ".egg",
    ".exe",
    ".gem",
    ".ipa",
    ".jar",
    ".lib",
    ".msi",
    ".msix",
    ".msixbundle",
    ".node",
    ".nupkg",
    ".o",
    ".obj",
    ".pdb",
    ".pkg",
    ".pyc",
    ".snap",
    ".so",
    ".wasm",
    ".whl",
)
NATIVE_SHARE_ARCHIVE_SUFFIXES = (
    ".7z",
    ".bz2",
    ".deb",
    ".gz",
    ".rar",
    ".rpm",
    ".tar",
    ".tar.bz2",
    ".tar.gz",
    ".tar.xz",
    ".tbz",
    ".tbz2",
    ".tgz",
    ".txz",
    ".xz",
    ".zip",
    ".zst",
)
NATIVE_SHARE_TIMEZONE_ARCHIVE_PREFIX = (
    "resources/seed-packs/initial-resource-pack/resources/timezones/"
)
NATIVE_SHARE_TIMEZONE_ARCHIVE_SEED_PREFIX = "resources/timezones/"
NATIVE_SHARE_TIMEZONE_ARCHIVE_NAME = re.compile(
    r"(?:tzcode|tzdata)[0-9]{4}[a-z][.]tar[.]gz$"
)
NATIVE_SHARE_EXECUTABLE_PATHS = frozenset(
    {
        "examples/core_beta_qa/admin_lifecycle_smoke.sh",
        "examples/core_beta_qa/driver_route_smoke.sh",
        "examples/core_beta_qa/embedded_public_abi_smoke.sh",
    }
)
# The docs/examples portion of a native release is deliberately closed.  The
# resource trees have their own seed/policy manifests, but these human-facing
# trees used to be copied wholesale.  That allowed an unfinished client source
# module to hide under a neutral filename in an otherwise approved subtree.
# Each output file is tied to its canonical public-repository source; adding a
# new release document or example therefore requires an explicit inventory
# edit rather than silently widening the shipped surface.
PUBLIC_REPO_ROOT = Path(__file__).resolve().parents[3]
NATIVE_SHARE_NONRESOURCE_SOURCE_FILES = {
    "docs/public_api/CORE_BETA_PUBLIC_API_ABI_MANIFEST.json": (
        "project/docs/public_api/CORE_BETA_PUBLIC_API_ABI_MANIFEST.json"
    ),
    "docs/public_api/CORE_BETA_PUBLIC_API_ABI.md": (
        "project/docs/public_api/CORE_BETA_PUBLIC_API_ABI.md"
    ),
    "docs/release/PUBLIC_SUPPORT_MAINTENANCE_POLICY.md": (
        "project/docs/release/PUBLIC_SUPPORT_MAINTENANCE_POLICY.md"
    ),
    "examples/core_beta_qa/README.md": "project/examples/core_beta_qa/README.md",
    "examples/core_beta_qa/admin_lifecycle_smoke.sh": (
        "project/examples/core_beta_qa/admin_lifecycle_smoke.sh"
    ),
    "examples/core_beta_qa/driver_route_smoke.sh": (
        "project/examples/core_beta_qa/driver_route_smoke.sh"
    ),
    "examples/core_beta_qa/embedded_public_abi_smoke.sh": (
        "project/examples/core_beta_qa/embedded_public_abi_smoke.sh"
    ),
    "examples/core_beta_qa/manifest.json": "project/examples/core_beta_qa/manifest.json",
    "examples/native_release_qa/README.md": (
        "project/examples/native_release_qa/README.md"
    ),
    "examples/native_release_qa/prepare_native_qa_instance.py": (
        "project/examples/native_release_qa/prepare_native_qa_instance.py"
    ),
}
# The QA script is an operational test helper, not a distributable driver.
# It is the only native-share path whose filename uses the otherwise forbidden
# generic `driver` token.
NATIVE_SHARE_NONPAYLOAD_CLIENT_TOKEN_EXCEPTIONS = frozenset(
    {"examples/core_beta_qa/driver_route_smoke.sh"}
)

REQUIRED_CONFIG_TOKENS = {
    "SBsrv.conf": (
        "provider_family = local_password",
        "default_policy_installed = true",
        "auto_create = false",
        "executable_path = bin/SBgate",
        "control_dir = runtime/listener/control",
        "runtime_dir = runtime/listener/runtime",
        "sbps_enabled = true",
        "sbps_endpoint = runtime/control/sb_server.sbps.sock",
        "failure_mode = return_error",
    ),
    "SBgate.conf": (
        "protocol_family = sbsql",
        "parser_package = SBParser",
        "parser_executable = bin/SBParser",
        "server_endpoint = runtime/control/sb_server.sbps.sock",
        "port = 3092",
        "tls_required = true",
        "managed_by_server = true",
        "managed_by_manager = false",
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

# A system package rewrites portable paths and the bootstrap platform profile,
# but it must retain the native-only security and topology invariants below.
# Keep this separate from REQUIRED_CONFIG_TOKENS: that map is intentionally the
# exact portable/staged configuration contract.
INSTALLED_CONFIG_TOKENS = {
    "SBsrv.conf": (
        "provider_family = local_password",
        "default_policy_installed = true",
        "auto_create = false",
        "sbps_enabled = true",
        "failure_mode = return_error",
    ),
    "SBgate.conf": (
        "protocol_family = sbsql",
        "parser_package = SBParser",
        "port = 3092",
        "tls_required = true",
        "managed_by_server = true",
        "managed_by_manager = false",
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
        "parser.security.auth_relay_required = true",
        "parser.execution.engine_authority_required = true",
        "parser.execution.engine_transport = sbps_ipc_only",
        "parser.execution.direct_engine_link = forbidden",
        "parser.execution.cross_parser_dependency = forbidden",
    ),
    "SBbootstrap.profile": (
        "schema_id = scratchbird.bootstrap_platform_profile.v1",
    ),
}

# The first binding is the generic server-to-listener launcher.  The second is
# the actual listener-to-parser launch setting.  SBParser.conf is declarative
# today (the worker does not load it), but its declared worker path must still
# agree with the listener's launch target so a shipped package cannot describe
# a different parser than the binary it contains.
NATIVE_EXECUTABLE_CONFIG_BINDINGS = (
    ("SBsrv.conf", "server.listener", "executable_path", "SBgate"),
    ("SBgate.conf", "", "parser_executable", "SBParser"),
    ("SBParser.conf", "", "parser.worker_binary", "SBParser"),
)

DEFAULT_SERVER_PROFILE_ONLY_KEYS = frozenset(
    {
        "bind_address",
        "bundle_contract_id",
        "database_selector",
        "dialect_profile_uuid",
        "parser_api_major",
        "parser_executable",
        "parser_executable_path",
        "parser_package",
        "parser_package_uuid",
        "port",
        "profile_id",
        "protocol_family",
        "ready_timeout_ms",
        "tls_ca_file",
        "tls_cert_file",
        "tls_key_file",
        "tls_required",
        "warm_pool_max",
        "warm_pool_min",
    }
)

INSTALLED_BOOTSTRAP_IDENTITIES = {
    "linux": ("scratchbird", "scratchbird"),
    "macos": ("scratchbird", "scratchbird"),
    "windows": (r"NT SERVICE\scratchbird", "ScratchBird"),
}

DEFAULT_SERVER_INVARIANT_ASSIGNMENTS = {
    ("server.security", "provider_family"): "local_password",
    ("server.security", "default_policy_installed"): "true",
    ("server.database", "auto_create"): "false",
    ("server.parser", "sbps_enabled"): "true",
    ("server.memory", "failure_mode"): "return_error",
}

PORTABLE_DEFAULT_SERVER_ASSIGNMENTS = {
    ("server.listener", "control_dir"): "runtime/listener/control",
    ("server.listener", "runtime_dir"): "runtime/listener/runtime",
    ("server.parser", "sbps_endpoint"): "runtime/control/sb_server.sbps.sock",
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

# A native-server archive contains only the engine/parser libraries listed in
# REQUIRED_LIBRARY_CANDIDATES.  In particular, it must never carry an
# incomplete client driver, adaptor, or MCP bridge merely because that library
# happened to be present in a broad build-output directory.  Client payloads
# have their own completed-component release family and are admitted there
# only after their independent completion evidence exists.
FORBIDDEN_CLIENT_LIBRARY_NAME_FRAGMENTS = (
    "scratchbird_client",
    "scratchbird_odbc",
    "scratchbird_mojo_client_bridge",
    "dbeaver",
)


def forbidden_client_library_name(name: str) -> bool:
    """Return whether *name* identifies a non-server client payload.

    This check is deliberately independent of an extension: it protects the
    source staging boundary as well as the Windows dynamic-DLL closure.
    """

    lowered = name.lower()
    if any(fragment in lowered for fragment in FORBIDDEN_CLIENT_LIBRARY_NAME_FRAGMENTS):
        return True
    if lowered.startswith(("scratchbird_", "libscratchbird_")):
        return True
    return re.search(r"(?:^|[_-])(driver|adaptor|adapter|mcp)(?:[_-]|[.]|$)", lowered) is not None

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

# This is an explicit, reviewed UCRT64 closure captured from the last known
# successful native Windows release run.  It is intentionally an exact-name
# policy instead of a filename pattern: a new dependency must fail staging
# until its provider, reason, and import parent are reviewed here.  No
# ScratchBird client driver, adaptor, or MCP library can become a bundled
# runtime merely by using an unrecognised DLL basename.
WINDOWS_NATIVE_RUNTIME_POLICY = {
    "libllvm-22.dll": {
        "name": "libLLVM-22.dll",
        "provider": "MSYS2 UCRT64 LLVM 22",
        "rationale": "configured native compiler runtime",
        "allowed_parents": frozenset({"release-required-runtime"}),
    },
    "libcrypto-3-x64.dll": {
        "name": "libcrypto-3-x64.dll",
        "provider": "MSYS2 UCRT64 OpenSSL 3",
        "rationale": "native TLS and cryptography runtime",
        "allowed_parents": frozenset({"native-binary", "libssl-3-x64.dll"}),
    },
    "libffi-8.dll": {
        "name": "libffi-8.dll",
        "provider": "MSYS2 UCRT64 libffi",
        "rationale": "LLVM and OpenSSL foreign-function runtime",
        "allowed_parents": frozenset({"libllvm-22.dll", "libcrypto-3-x64.dll"}),
    },
    "libgcc_s_seh-1.dll": {
        "name": "libgcc_s_seh-1.dll",
        "provider": "MSYS2 UCRT64 GCC",
        "rationale": "native C++ exception runtime",
        "allowed_parents": frozenset(
            {
                "native-binary",
                "libffi-8.dll",
                "libicuuc78.dll",
                "libquadmath-0.dll",
                "libstdc++-6.dll",
            }
        ),
    },
    "libiconv-2.dll": {
        "name": "libiconv-2.dll",
        "provider": "MSYS2 UCRT64 libiconv",
        "rationale": "XML runtime character conversion",
        "allowed_parents": frozenset({"libxml2-16.dll"}),
    },
    "libicudt78.dll": {
        "name": "libicudt78.dll",
        "provider": "MSYS2 UCRT64 ICU 78",
        "rationale": "native collation and timezone data runtime",
        "allowed_parents": frozenset({"libiconv-2.dll", "libicuuc78.dll"}),
    },
    "libicuuc78.dll": {
        "name": "libicuuc78.dll",
        "provider": "MSYS2 UCRT64 ICU 78",
        "rationale": "native Unicode runtime",
        "allowed_parents": frozenset({"native-binary", "libicudt78.dll"}),
    },
    "libquadmath-0.dll": {
        "name": "libquadmath-0.dll",
        "provider": "MSYS2 UCRT64 GCC",
        "rationale": "native REAL128 runtime support",
        "allowed_parents": frozenset({"native-binary"}),
    },
    "libssl-3-x64.dll": {
        "name": "libssl-3-x64.dll",
        "provider": "MSYS2 UCRT64 OpenSSL 3",
        "rationale": "native TLS runtime",
        "allowed_parents": frozenset({"native-binary"}),
    },
    "libstdc++-6.dll": {
        "name": "libstdc++-6.dll",
        "provider": "MSYS2 UCRT64 GCC",
        "rationale": "native C++ standard library runtime",
        "allowed_parents": frozenset(
            {"native-binary", "libllvm-22.dll", "libicuuc78.dll"}
        ),
    },
    "libwinpthread-1.dll": {
        "name": "libwinpthread-1.dll",
        "provider": "MSYS2 UCRT64 GCC",
        "rationale": "native threading runtime",
        "allowed_parents": frozenset(
            {
                "native-binary",
                "libgcc_s_seh-1.dll",
                "libicuuc78.dll",
                "libllvm-22.dll",
                "libstdc++-6.dll",
            }
        ),
    },
    "libxml2-16.dll": {
        "name": "libxml2-16.dll",
        "provider": "MSYS2 UCRT64 libxml2",
        "rationale": "LLVM XML runtime",
        "allowed_parents": frozenset({"libllvm-22.dll"}),
    },
    "libzstd.dll": {
        "name": "libzstd.dll",
        "provider": "MSYS2 UCRT64 zstd",
        "rationale": "LLVM compression runtime",
        "allowed_parents": frozenset({"libllvm-22.dll"}),
    },
    "zlib1.dll": {
        "name": "zlib1.dll",
        "provider": "MSYS2 UCRT64 zlib",
        "rationale": "LLVM, XML, and zstd compression runtime",
        "allowed_parents": frozenset(
            {"libllvm-22.dll", "libxml2-16.dll", "libzstd.dll"}
        ),
    },
}
WINDOWS_NATIVE_RUNTIME_NAMES = frozenset(
    str(policy["name"]) for policy in WINDOWS_NATIVE_RUNTIME_POLICY.values()
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


def strip_config_inline_comment(line: str) -> str:
    """Match the SBCD1 comment rule closely enough for package validation."""
    quoted = False
    escaped = False
    for index, character in enumerate(line):
        if escaped:
            escaped = False
            continue
        if quoted and character == "\\":
            escaped = True
            continue
        if character == '"':
            quoted = not quoted
            continue
        if (
            not quoted
            and character in {"#", ";"}
            and (index == 0 or line[index - 1].isspace())
        ):
            return line[:index]
    return line


def parse_config_assignments(text: str) -> dict[tuple[str, str], list[str]]:
    """Return actual section/key assignments, never comments or prose."""
    assignments: dict[tuple[str, str], list[str]] = {}
    section = ""
    for raw_line in text.splitlines():
        line = strip_config_inline_comment(raw_line).strip()
        if not line:
            continue
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1].strip().lower()
            continue
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        canonical_key = key.strip().lower().replace("-", "_")
        if not canonical_key:
            continue
        assignments.setdefault((section, canonical_key), []).append(value.strip())
    return assignments


def native_config_assignment(
    assignments: dict[tuple[str, str], list[str]],
    file_name: str,
    section: str,
    key: str,
) -> str:
    values = assignments.get((section, key), [])
    if len(values) != 1:
        location = f"{section}.{key}" if section else key
        fail(
            "native_config_assignment_cardinality:"
            f"{file_name}:{location}:{len(values)}"
        )
    return values[0]


def native_config_token_present(
    assignments: dict[tuple[str, str], list[str]],
    token: str,
) -> bool:
    if "=" not in token:
        return False
    key, value = token.split("=", 1)
    canonical_key = key.strip().lower().replace("-", "_")
    expected_value = value.strip()
    return any(
        actual_key == canonical_key and expected_value in values
        for (_, actual_key), values in assignments.items()
    )


def require_native_config_tokens(
    config_root: Path,
    token_contract: dict[str, tuple[str, ...]],
) -> dict[str, dict[tuple[str, str], list[str]]]:
    parsed: dict[str, dict[tuple[str, str], list[str]]] = {}
    for file_name, tokens in token_contract.items():
        path = require_regular_file(config_root / file_name, f"native_config:{file_name}")
        text = path.read_text(encoding="utf-8")
        assignments = parse_config_assignments(text)
        if file_name != "SBsrv.conf" and any(section for section, _ in assignments):
            fail(f"native_flat_config_section_forbidden:{file_name}")
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
            if not native_config_token_present(assignments, token):
                fail(f"native_config_token_missing:{file_name}:{token}")
        if any(marker in text for marker in EMBEDDED_TLS_MARKERS):
            fail(f"native_config_embedded_tls_material_forbidden:{file_name}")
        parsed[file_name] = assignments
    return parsed


def require_generic_default_server(
    assignments: dict[tuple[str, str], list[str]],
) -> None:
    for section, key in assignments:
        if section.startswith("server.listener.profile."):
            fail("native_default_server_listener_profile_forbidden")
        if key in DEFAULT_SERVER_PROFILE_ONLY_KEYS:
            fail(f"native_default_server_profile_key_forbidden:{key}")
    for (section, key), expected_value in DEFAULT_SERVER_INVARIANT_ASSIGNMENTS.items():
        actual_value = native_config_assignment(assignments, "SBsrv.conf", section, key)
        if actual_value != expected_value:
            fail(
                "native_default_server_assignment_invalid:"
                f"{section}.{key}:{actual_value}"
            )


def require_portable_default_server(
    assignments: dict[tuple[str, str], list[str]],
) -> None:
    for (section, key), expected_value in PORTABLE_DEFAULT_SERVER_ASSIGNMENTS.items():
        actual_value = native_config_assignment(assignments, "SBsrv.conf", section, key)
        if actual_value != expected_value:
            fail(
                "native_portable_server_assignment_invalid:"
                f"{section}.{key}:{actual_value}"
            )


def normalize_config_path(value: str) -> str:
    return value.replace("\\", "/")


def portable_executable_path(binary: str) -> str:
    return f"bin/{binary}"


def installed_executable_path_allowed(value: str, binary: str, platform: str) -> bool:
    normalized = normalize_config_path(value)
    executable = platform_executable(binary, platform)
    if platform in {"linux", "macos"}:
        return normalized == f"/opt/ScratchBird/bin/{executable}"
    if platform != "windows":
        return False
    if normalized == f"@SCRATCHBIRD_INSTALL_ROOT@/bin/{executable}":
        return True
    return re.fullmatch(
        rf"(?i)[a-z]:/program files/scratchbird/bin/{re.escape(executable)}",
        normalized,
    ) is not None


def classify_configured_executable_paths(
    parsed: dict[str, dict[tuple[str, str], list[str]]],
    platform: str,
) -> str:
    modes: set[str] = set()
    values: dict[tuple[str, str], str] = {}
    for file_name, section, key, binary in NATIVE_EXECUTABLE_CONFIG_BINDINGS:
        value = native_config_assignment(parsed[file_name], file_name, section, key)
        values[(file_name, key)] = value
        if value == portable_executable_path(binary):
            modes.add("portable")
        elif installed_executable_path_allowed(value, binary, platform):
            modes.add("installed")
        else:
            location = f"{section}.{key}" if section else key
            fail(
                "native_config_executable_path_invalid:"
                f"{file_name}:{location}:{value}"
            )
    if len(modes) != 1:
        fail(f"native_config_executable_path_mode_mismatch:{sorted(modes)}")
    listener_parser = values[("SBgate.conf", "parser_executable")]
    declared_parser = values[("SBParser.conf", "parser.worker_binary")]
    if listener_parser != declared_parser:
        fail("native_config_parser_worker_path_mismatch")
    return next(iter(modes))


def require_sbps_endpoint_match(
    parsed: dict[str, dict[tuple[str, str], list[str]]],
) -> None:
    server_endpoint = native_config_assignment(
        parsed["SBsrv.conf"],
        "SBsrv.conf",
        "server.parser",
        "sbps_endpoint",
    )
    listener_endpoint = native_config_assignment(
        parsed["SBgate.conf"],
        "SBgate.conf",
        "",
        "server_endpoint",
    )
    if not server_endpoint or server_endpoint != listener_endpoint:
        fail("native_config_sbps_endpoint_mismatch")


def require_configured_native_binaries(
    runtime_root: Path,
    platform: str,
) -> None:
    for file_name, section, key, binary in NATIVE_EXECUTABLE_CONFIG_BINDINGS:
        executable = runtime_root / "bin" / platform_executable(binary, platform)
        location = f"{section}.{key}" if section else key
        require_regular_file(
            executable,
            f"native_config_binary:{file_name}:{location}",
        )


def require_native_configs(
    config_root: Path,
    runtime_root: Path,
    platform: str,
) -> None:
    """Validate the exact portable/staged native release configuration."""
    parsed = require_native_config_tokens(config_root, REQUIRED_CONFIG_TOKENS)
    require_generic_default_server(parsed["SBsrv.conf"])
    require_portable_default_server(parsed["SBsrv.conf"])
    if classify_configured_executable_paths(parsed, platform) != "portable":
        fail("native_portable_config_path_mode_required")
    require_sbps_endpoint_match(parsed)
    require_configured_native_binaries(runtime_root, platform)


def require_installed_bootstrap_profile(
    assignments: dict[tuple[str, str], list[str]],
    platform: str,
) -> None:
    expected_identity, expected_group = INSTALLED_BOOTSTRAP_IDENTITIES[platform]
    profile_platform = native_config_assignment(
        assignments,
        "SBbootstrap.profile",
        "",
        "platform",
    )
    identity = native_config_assignment(
        assignments,
        "SBbootstrap.profile",
        "",
        "service_identity",
    )
    group = native_config_assignment(
        assignments,
        "SBbootstrap.profile",
        "",
        "service_group",
    )
    if profile_platform != platform:
        fail(
            "native_installed_bootstrap_platform_mismatch:"
            f"expected={platform}:actual={profile_platform}"
        )
    if identity != expected_identity or group != expected_group:
        fail("native_installed_bootstrap_identity_mismatch")
    if "operator_required" in {profile_platform, identity, group}:
        fail("native_installed_bootstrap_placeholder_forbidden")


def require_native_installed_configs(
    config_root: Path,
    runtime_root: Path,
    platform: str,
) -> None:
    """Validate either a portable payload or a platform-materialized install."""
    parsed = require_native_config_tokens(config_root, INSTALLED_CONFIG_TOKENS)
    require_generic_default_server(parsed["SBsrv.conf"])
    mode = classify_configured_executable_paths(parsed, platform)
    if mode == "portable":
        require_native_configs(config_root, runtime_root, platform)
        return
    require_installed_bootstrap_profile(parsed["SBbootstrap.profile"], platform)
    require_sbps_endpoint_match(parsed)
    require_configured_native_binaries(runtime_root, platform)


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
        and not forbidden_client_library_name(name)
        and not lowered.startswith("sbp_")
        and not lowered.startswith("sbu_")
    )


def windows_native_binary_name(name: str) -> bool:
    """Return whether *name* is an exact native executable or engine DLL."""

    expected = {
        platform_executable(executable, "windows").lower()
        for executable in native_executables("windows")
    }
    expected.update(
        candidate.lower()
        for candidates in REQUIRED_LIBRARY_CANDIDATES["windows"].values()
        for candidate in candidates
        if candidate.lower().endswith(".dll")
    )
    return name.lower() in expected


def admitted_windows_runtime_dependency(name: str, parent: str) -> str:
    """Return canonical DLL name only when its reviewed edge is permitted."""

    if not safe_runtime_dependency_name(name):
        fail(f"windows_runtime_dependency_forbidden:{name}")
    policy = WINDOWS_NATIVE_RUNTIME_POLICY.get(name.lower())
    if policy is None:
        fail(f"windows_runtime_dependency_not_admitted:{name}")
    parent_key = "native-binary" if windows_native_binary_name(parent) else parent.lower()
    allowed_parents = policy["allowed_parents"]
    if parent_key not in allowed_parents:
        fail(
            "windows_runtime_dependency_parent_not_admitted:"
            f"{parent}:{name}"
        )
    return str(policy["name"])


def require_windows_runtime_inventory(runtime_dependencies: set[str]) -> None:
    """Require the complete reviewed Windows native runtime set."""

    actual = set(runtime_dependencies)
    if actual != WINDOWS_NATIVE_RUNTIME_NAMES:
        fail(
            "windows_runtime_dependency_inventory_mismatch:"
            f"missing={sorted(WINDOWS_NATIVE_RUNTIME_NAMES - actual)}:"
            f"unexpected={sorted(actual - WINDOWS_NATIVE_RUNTIME_NAMES)}"
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
            or runtime_library.lower() != "libllvm-22.dll"
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
        canonical = admitted_windows_runtime_dependency(
            dependency,
            "release-required-runtime",
        )
        source = locate_case_insensitive(canonical, search_roots)
        if source is None:
            fail(f"windows_required_runtime_dependency_unresolved:{canonical}")
        destination = output_root / "bin" / canonical
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
            if windows_native_binary_name(dependency):
                existing_native = locate_case_insensitive(
                    dependency,
                    (output_root / "bin",),
                )
                if existing_native is None:
                    fail(
                        "windows_native_dependency_unresolved:"
                        f"{binary.name}:{dependency}"
                    )
                queue.append(existing_native)
                continue
            canonical = admitted_windows_runtime_dependency(dependency, binary.name)
            existing = locate_case_insensitive(canonical, (output_root / "bin",))
            if existing is not None:
                queue.append(existing)
                continue
            source = locate_case_insensitive(canonical, roots)
            if source is None:
                fail(f"windows_runtime_dependency_unresolved:{binary.name}:{canonical}")
            destination = output_root / "bin" / canonical
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


def native_share_nonresource_inventory(share_root: Path) -> list[dict[str, str]]:
    """Validate and return the exact docs/examples inventory for native releases.

    Unlike resource data, these files are not governed by a seed-pack index.
    Bind them to the canonical public repository files so a broad directory
    copy cannot add or substitute a platform-agnostic driver/adaptor/MCP
    source module under a neutral name.
    """

    scratchbird_root = share_root / "scratchbird"
    actual_files: set[str] = set()
    actual_directories: set[str] = set()
    for top_level in ("docs", "examples"):
        root = scratchbird_root / top_level
        if not root.is_dir() or root.is_symlink():
            fail(f"native_share_nonresource_root_missing:{top_level}")
        actual_directories.add(top_level)
        for path in sorted(root.rglob("*")):
            relative = path.relative_to(scratchbird_root).as_posix()
            if path.is_symlink() or not (path.is_file() or path.is_dir()):
                fail(f"native_share_nonresource_entry_forbidden:{relative}")
            if path.is_dir():
                actual_directories.add(relative)
            else:
                actual_files.add(relative)

    expected_files = set(NATIVE_SHARE_NONRESOURCE_SOURCE_FILES)
    if actual_files != expected_files:
        fail(
            "native_share_nonresource_file_set_mismatch:"
            f"missing={sorted(expected_files - actual_files)}:"
            f"unexpected={sorted(actual_files - expected_files)}"
        )
    expected_directories = {
        parent.as_posix()
        for relative in expected_files
        for parent in Path(relative).parents
        if parent != Path(".")
    }
    if actual_directories != expected_directories:
        fail(
            "native_share_nonresource_directory_set_mismatch:"
            f"missing={sorted(expected_directories - actual_directories)}:"
            f"unexpected={sorted(actual_directories - expected_directories)}"
        )

    inventory: list[dict[str, str]] = []
    for relative, source_relative in sorted(NATIVE_SHARE_NONRESOURCE_SOURCE_FILES.items()):
        staged = scratchbird_root / relative
        canonical = PUBLIC_REPO_ROOT / source_relative
        require_regular_file(canonical, f"native_share_canonical_source:{relative}")
        staged_digest = hashlib.sha256(staged.read_bytes()).hexdigest()
        canonical_digest = hashlib.sha256(canonical.read_bytes()).hexdigest()
        if staged_digest != canonical_digest:
            fail(f"native_share_nonresource_hash_mismatch:{relative}")
        inventory.append({"path": relative, "sha256": staged_digest})
    return inventory


def native_share_path_has_client_identity(
    relative: Path,
    *,
    allow_nonpayload_exception: bool = True,
) -> bool:
    """Return whether a share-relative path identifies a client component.

    This intentionally examines every path component instead of only the leaf
    name.  A `resources/adaptor/...` or `docs/dbeaver/...` subtree is still a
    release payload, even when its individual file names are neutral.
    """

    normalized = relative.as_posix().casefold()
    if (
        allow_nonpayload_exception
        and normalized in NATIVE_SHARE_NONPAYLOAD_CLIENT_TOKEN_EXCEPTIONS
    ):
        return False
    for part in relative.parts:
        lowered = part.casefold()
        normalized_part = re.sub(r"[-.]", "_", lowered)
        if "dbeaver" in lowered or any(
            fragment in normalized_part
            for fragment in NATIVE_SHARE_CLIENT_NAME_FRAGMENTS
        ):
            return True
        tokens = [token for token in re.split(r"[._-]+", lowered) if token]
        if any(token in NATIVE_SHARE_CLIENT_COMPONENTS for token in tokens):
            return True
    return False


def native_share_payload_magic(header: bytes) -> str | None:
    """Return a recognized executable, library, or package magic class."""

    if header.startswith(b"\x7fELF"):
        return "elf"
    if header.startswith(b"MZ"):
        return "pe"
    if header[:4] in {
        b"\xce\xfa\xed\xfe",
        b"\xfe\xed\xfa\xce",
        b"\xcf\xfa\xed\xfe",
        b"\xfe\xed\xfa\xcf",
        b"\xca\xfe\xba\xbe",
        b"\xbe\xba\xfe\xca",
        b"\xca\xfe\xba\xbf",
        b"\xbf\xba\xfe\xca",
    }:
        return "macho"
    if header.startswith(b"!<arch>\n"):
        return "static_archive"
    if header.startswith((b"PK\x03\x04", b"PK\x05\x06", b"PK\x07\x08")):
        return "zip_archive"
    if header.startswith(b"\x1f\x8b"):
        return "gzip_archive"
    if header.startswith(b"BZh"):
        return "bzip2_archive"
    if header.startswith(b"\xfd7zXZ\x00"):
        return "xz_archive"
    if header.startswith(b"(\xb5/\xfd"):
        return "zstd_archive"
    if header.startswith(b"7z\xbc\xaf'\x1c"):
        return "seven_zip_archive"
    if header.startswith(b"Rar!\x1a\x07"):
        return "rar_archive"
    if header.startswith(b"MSCF"):
        return "cabinet_archive"
    if header.startswith(b"\x00asm"):
        return "wasm"
    return None


def native_share_executable_magic(path: Path) -> str | None:
    """Return a recognized executable/archive payload class, if present."""

    with path.open("rb") as handle:
        return native_share_payload_magic(handle.read(8))


def native_share_forbidden_payload_suffix(relative_text: str) -> bool:
    """Return whether a path name advertises a binary/library/package payload."""

    lowered = relative_text.casefold()
    leaf = Path(lowered).name
    return (
        lowered.endswith(NATIVE_SHARE_FORBIDDEN_SUFFIXES)
        or ".so." in leaf
        or ".dylib." in leaf
    )


def native_share_declared_timezone_archives(scratchbird_root: Path) -> set[str]:
    """Return only the exact tzcode/tzdata archives declared by the seed index.

    Every supported release route validates the seed index before calling the
    share policy.  This narrow lookup ties the sole compressed-archive
    exception to its manifest declarations instead of allowing a whole
    timezone subtree to become an opaque package bypass.
    """

    seed_root = (
        scratchbird_root
        / "resources"
        / "seed-packs"
        / "initial-resource-pack"
    )
    artifact_index = require_regular_file(
        seed_root / "RESOURCE_SEED_ARTIFACTS.csv",
        "native_share_resource_artifact_index",
    )
    with artifact_index.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        if reader.fieldnames != [
            "canonical_path",
            "content_hash",
            "content_size_bytes",
        ]:
            fail("native_share_resource_artifact_index_header_invalid")
        rows = list(reader)

    declared: set[str] = set()
    for row in rows:
        relative = row.get("canonical_path")
        if not isinstance(relative, str):
            fail("native_share_resource_artifact_index_path_invalid")
        safe_relative_path(seed_root, relative, "native_share_resource_artifact")
        if not relative.startswith(NATIVE_SHARE_TIMEZONE_ARCHIVE_SEED_PREFIX):
            continue
        name = Path(relative).name
        if NATIVE_SHARE_TIMEZONE_ARCHIVE_NAME.fullmatch(name):
            declared.add(
                "resources/seed-packs/initial-resource-pack/" + relative
            )
    return declared


def native_share_safe_archive_member_path(member_name: str) -> Path:
    """Return a safe normalized relative member path or fail closed."""

    normalized = member_name.rstrip("/")
    if (
        not normalized
        or "\x00" in normalized
        or "\\" in normalized
        or normalized.startswith("/")
        or re.match(r"^[A-Za-z]:", normalized) is not None
    ):
        fail(f"native_share_archive_member_path_unsafe:{member_name}")
    parts = normalized.split("/")
    if any(part in {"", ".", ".."} for part in parts):
        fail(f"native_share_archive_member_path_unsafe:{member_name}")
    return Path(*parts)


def require_native_share_timezone_archive(
    path: Path,
    relative_text: str,
) -> None:
    """Inspect every member of an admitted timezone source archive.

    The source archives are data inputs, not a general package delivery route.
    Do not extract them: validate member names, entry types, modes, suffixes,
    identities, and header magic while streaming the member headers.
    """

    executable_mask = stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH
    try:
        archive = tarfile.open(path, mode="r:gz")
    except (OSError, tarfile.TarError) as exc:
        fail(f"native_share_timezone_archive_invalid:{relative_text}:{exc}")
    with archive:
        for member in archive:
            member_relative = native_share_safe_archive_member_path(member.name)
            member_text = member_relative.as_posix()
            member_label = f"{relative_text}:{member_text}"
            if native_share_path_has_client_identity(
                member_relative,
                allow_nonpayload_exception=False,
            ):
                fail(f"native_share_archive_member_client_forbidden:{member_label}")
            if native_share_forbidden_payload_suffix(member_text) or member_text.casefold().endswith(
                NATIVE_SHARE_ARCHIVE_SUFFIXES
            ):
                fail(f"native_share_archive_member_suffix_forbidden:{member_label}")
            if member.mode & executable_mask:
                fail(f"native_share_archive_member_executable_forbidden:{member_label}")
            if member.isdir():
                continue
            if not member.isfile():
                fail(f"native_share_archive_member_type_forbidden:{member_label}")
            extracted = archive.extractfile(member)
            if extracted is None:
                fail(f"native_share_archive_member_unreadable:{member_label}")
            with extracted:
                payload_kind = native_share_payload_magic(extracted.read(8))
            if payload_kind is not None:
                fail(
                    "native_share_archive_member_payload_forbidden:"
                    f"{payload_kind}:{member_label}"
                )


def require_native_share_payload_policy(share_root: Path) -> None:
    """Reject every client payload or hidden executable under native share/.

    Resource data is necessarily recursive, so this policy combines explicit
    client identity checks with format and executable-bit checks.  It prevents
    an unfinished driver/adaptor/MCP from bypassing the bin/lib allowlist by
    being copied through a broad shared-resource subtree under a neutral name.
    """

    scratchbird_root = share_root / "scratchbird"
    if not scratchbird_root.is_dir():
        fail(f"native_share_root_missing:{scratchbird_root}")
    declared_timezone_archives = native_share_declared_timezone_archives(
        scratchbird_root
    )
    executable_mask = stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH
    for path in sorted(scratchbird_root.rglob("*")):
        relative = path.relative_to(scratchbird_root)
        relative_text = relative.as_posix()
        if path.is_symlink():
            fail(f"native_share_symlink_forbidden:{relative_text}")
        if not path.is_file() and not path.is_dir():
            fail(f"native_share_entry_type_forbidden:{relative_text}")
        if "__pycache__" in relative.parts:
            fail(f"native_share_cache_forbidden:{relative_text}")
        if native_share_path_has_client_identity(relative):
            fail(f"native_share_client_payload_forbidden:{relative_text}")
        lowered = relative_text.casefold()
        if native_share_forbidden_payload_suffix(relative_text):
            fail(f"native_share_client_file_suffix_forbidden:{relative_text}")
        allowed_timezone_archive = relative_text in declared_timezone_archives
        if lowered.endswith(NATIVE_SHARE_ARCHIVE_SUFFIXES):
            if not allowed_timezone_archive:
                fail(f"native_share_archive_location_forbidden:{relative_text}")
        if not path.is_file():
            continue
        if (
            path.stat().st_mode & executable_mask
            and relative_text not in NATIVE_SHARE_EXECUTABLE_PATHS
        ):
            fail(f"native_share_executable_bit_forbidden:{relative_text}")
        if allowed_timezone_archive:
            require_native_share_timezone_archive(path, relative_text)
            continue
        payload_kind = native_share_executable_magic(path)
        if payload_kind is not None:
            fail(
                "native_share_executable_payload_forbidden:"
                f"{payload_kind}:{relative_text}"
            )


def require_native_share_layout(
    share_root: Path,
    *,
    allow_installed_release_metadata: bool = False,
    allow_system_configuration_defaults: bool = False,
) -> list[dict[str, str]]:
    scratchbird_root = share_root / "scratchbird"
    if not scratchbird_root.is_dir():
        fail(f"native_share_root_missing:{scratchbird_root}")
    expected_top_levels = set(NATIVE_SHARE_TOP_LEVELS)
    if allow_installed_release_metadata:
        # The portable staging tree has no release/ directory.  Installer
        # assembly creates it solely for native manifests and checksums, so it
        # is admitted only for extracted installed payload verification.
        expected_top_levels.add("release")
    if allow_system_configuration_defaults:
        # An explicitly internal system package may relocate the pristine
        # native configuration files beneath the runtime tree.  This is an
        # exact, named exception; it must not turn the native share root into
        # an arbitrary extension point.
        expected_top_levels.add("config-defaults")
    actual_top_levels = {entry.name for entry in scratchbird_root.iterdir()}
    if actual_top_levels != expected_top_levels:
        fail(
            "native_share_top_level_set_mismatch:"
            f"missing={sorted(expected_top_levels - actual_top_levels)}:"
            f"unexpected={sorted(actual_top_levels - expected_top_levels)}"
        )
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
    if allow_system_configuration_defaults:
        config_defaults = scratchbird_root / "config-defaults"
        actual_configs = {
            entry.name for entry in config_defaults.iterdir()
            if entry.is_file() and not entry.is_symlink()
        }
        if actual_configs != set(NATIVE_CONFIGS):
            fail(
                "native_system_config_defaults_set_mismatch:"
                f"missing={sorted(set(NATIVE_CONFIGS) - actual_configs)}:"
                f"unexpected={sorted(actual_configs - set(NATIVE_CONFIGS))}"
            )
        for entry in config_defaults.iterdir():
            if entry.is_symlink() or not entry.is_file():
                fail(f"native_system_config_defaults_entry_forbidden:{entry}")
    require_native_share_payload_policy(share_root)
    return native_share_nonresource_inventory(share_root)


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
    require_native_configs(
        source_root / "etc" / "scratchbird",
        source_root,
        platform,
    )

    if output_root.exists():
        shutil.rmtree(output_root)
    (output_root / "bin").mkdir(parents=True)
    (output_root / "lib").mkdir(parents=True)

    copied_bins: list[str] = []
    for name in native_executables(platform):
        file_name = platform_executable(name, platform)
        copy_file(source_root / "bin" / file_name, output_root / "bin" / file_name)
        copied_bins.append(file_name)

    copied_libraries: list[str] = []
    for label, candidates in REQUIRED_LIBRARY_CANDIDATES[platform].items():
        source = locate_library(source_root, platform, candidates)
        destination_dir = "bin" if source.parent.name == "bin" else "lib"
        copy_file(source, output_root / destination_dir / source.name)
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
    require_native_configs(
        output_root / "etc" / "scratchbird",
        output_root,
        platform,
    )
    nonresource_inventory = require_native_share_layout(output_root / "share")

    runtime_dependencies: list[str] = []
    if platform == "windows":
        runtime_dependencies = stage_windows_runtime_dependencies(
            output_root,
            tuple(path.resolve() for path in runtime_search_roots),
            (str(llvm_runtime["runtime_library"]),),
        )
        require_windows_runtime_inventory(set(runtime_dependencies))
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
        "native_share_nonresource_inventory": nonresource_inventory,
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
