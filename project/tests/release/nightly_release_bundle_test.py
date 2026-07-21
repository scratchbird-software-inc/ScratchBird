#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

"""Unit tests for the native rolling-nightly release bundle."""

from __future__ import annotations

import hashlib
import io
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import sys
import tarfile
import tempfile
import unittest
import zipfile


REPO_ROOT = Path(__file__).resolve().parents[3]
SCRIPT = REPO_ROOT / "project" / "tools" / "installers" / "create_nightly_release_bundle.py"
SPEC = importlib.util.spec_from_file_location("create_nightly_release_bundle", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
bundle = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(bundle)
INSTALLER_VERIFIER_SCRIPT = (
    REPO_ROOT / "project" / "tools" / "installers" / "verify_installer_artifacts.py"
)
INSTALLER_VERIFIER_SPEC = importlib.util.spec_from_file_location(
    "verify_installer_artifacts", INSTALLER_VERIFIER_SCRIPT
)
assert INSTALLER_VERIFIER_SPEC is not None and INSTALLER_VERIFIER_SPEC.loader is not None
installer_verifier = importlib.util.module_from_spec(INSTALLER_VERIFIER_SPEC)
INSTALLER_VERIFIER_SPEC.loader.exec_module(installer_verifier)
sys.path.insert(0, str(REPO_ROOT / "project" / "tools" / "release"))
import stage_native_release_bundle as native  # noqa: E402


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


NATIVE_PROFILE_ARCHIVE_PATH = (
    "opt/ScratchBird/share/scratchbird/release/NATIVE_RELEASE_PROFILE.json"
)


def native_profile_digest_from_archive(content: bytes) -> str | None:
    """Return the raw staged profile hash when *content* is a native archive."""

    try:
        with tarfile.open(fileobj=io.BytesIO(content), mode="r:gz") as archive:
            member = archive.getmember(NATIVE_PROFILE_ARCHIVE_PATH)
            handle = archive.extractfile(member)
            if handle is None:
                raise AssertionError("native profile unreadable")
            return digest(handle.read())
    except (KeyError, tarfile.TarError):
        pass
    try:
        with zipfile.ZipFile(io.BytesIO(content)) as archive:
            return digest(archive.read(NATIVE_PROFILE_ARCHIVE_PATH))
    except (KeyError, zipfile.BadZipFile):
        return None


def native_profile(
    platform: str,
    *,
    native_parser: str = "SBSQL",
    resource_counts: dict[str, int],
    policy_count: int,
    nonresource_inventory: list[dict[str, str]],
    architecture: str | None = None,
    omit_library_role: str | None = None,
) -> bytes:
    suffix = ".exe" if platform == "windows" else ""
    macos_runtime = (
        {
            "link_mode": "dynamic",
            "runtime_library": None,
            "runtime_libraries_by_architecture": {
                "x86_64": "/usr/local/opt/llvm/lib/libLLVM.dylib",
                "arm64": "/opt/homebrew/opt/llvm/lib/libLLVM.dylib",
            },
            "delivery": "external-homebrew",
            "minimum_major": 22,
        }
        if architecture == "universal"
        else {
            "link_mode": "dynamic",
            "runtime_library": (
                "/opt/homebrew/opt/llvm/lib/libLLVM.dylib"
                if architecture == "arm64"
                else "/usr/local/opt/llvm/lib/libLLVM.dylib"
            ),
            "delivery": "external-homebrew",
            "minimum_major": 22,
        }
    )
    llvm_runtime = {
        "linux": {
            "link_mode": "dynamic",
            "runtime_library": "libLLVM.so.23.0",
            "delivery": "system-package",
            "minimum_major": 23,
        },
        "windows": {
            "link_mode": "dynamic",
            "runtime_library": "libLLVM-22.dll",
            "delivery": "bundled",
            "minimum_major": 22,
        },
        "macos": macos_runtime,
    }[platform]
    libraries = []
    for role, candidates in native.REQUIRED_LIBRARY_CANDIDATES[platform].items():
        if role == omit_library_role:
            continue
        name = candidates[0]
        libraries.append(f"{'bin' if name.endswith('.dll') else 'lib'}/{name}")
    return (
        json.dumps(
            {
                "schema_id": "scratchbird.native_release_profile.v1",
                "profile": "native-sbsql-only",
                "platform": platform,
                "native_parser": native_parser,
                "emulation_components": "excluded",
                "executables": sorted(
                    f"{name}{suffix}" for name in native.native_executables(platform)
                ),
                "libraries": sorted(libraries),
                "runtime_dependencies": (
                    sorted(native.WINDOWS_NATIVE_RUNTIME_NAMES)
                    if platform == "windows"
                    else []
                ),
                "llvm_runtime": llvm_runtime,
                "configuration": list(native.NATIVE_CONFIGS),
                "required_resource_directories": list(native.REQUIRED_RESOURCE_DIRS),
                "required_resource_files": list(native.REQUIRED_RESOURCE_FILES),
                "required_operability_files": list(native.REQUIRED_OPERABILITY_FILES),
                "native_share_subtrees": list(native.NATIVE_SHARE_SUBTREES),
                "resource_artifact_counts": resource_counts,
                "policy_content_file_count": policy_count,
                "native_share_nonresource_inventory": nonresource_inventory,
            },
            sort_keys=True,
        )
        + "\n"
    ).encode()


def native_resource_fixture(
    marker: str,
) -> tuple[dict[str, bytes], dict[str, int], int, list[dict[str, str]]]:
    seed_prefix = "resources/seed-packs/initial-resource-pack/"
    policy_prefix = "resources/policy-packs/default-local-password/"
    seed_manifest = seed_prefix + "RESOURCE_SEED_MANIFEST.csv"
    seed_artifacts = seed_prefix + "RESOURCE_SEED_ARTIFACTS.csv"
    policy_manifest = policy_prefix + "POLICY_PACK_MANIFEST.json"

    resources: dict[str, bytes] = {}
    for rel in native.REQUIRED_RESOURCE_FILES:
        if rel in {seed_manifest, seed_artifacts, policy_manifest}:
            continue
        resources[rel] = (f'{{"fixture":"{marker}:{rel}"}}\n').encode()
    for rel in native.REQUIRED_RESOURCE_DIRS:
        resources[f"{rel}/.fixture-resource"] = f"{marker}:{rel}\n".encode()
    # The native installed-payload verifier binds docs/examples to exact
    # public source files.  In particular, this fixture must carry the one
    # approved non-payload filename containing the generic `driver` token:
    # examples/core_beta_qa/driver_route_smoke.sh.  Copying the canonical
    # inventory makes the archive valid before the installer admission layer
    # decides whether that path is the narrow reviewed exception.
    nonresource_inventory: list[dict[str, str]] = []
    for rel, source_rel in sorted(native.NATIVE_SHARE_NONRESOURCE_SOURCE_FILES.items()):
        source = native.PUBLIC_REPO_ROOT / source_rel
        value = source.read_bytes()
        resources[rel] = value
        nonresource_inventory.append({"path": rel, "sha256": digest(value)})

    policy_rows = []
    aggregate = bytearray()
    for rel in sorted(path for path in resources if path.startswith(policy_prefix)):
        policy_rel = rel.removeprefix(policy_prefix)
        value_hash = hashlib.sha256(resources[rel]).hexdigest()
        policy_rows.append({"path": policy_rel, "sha256": value_hash})
        aggregate.extend(policy_rel.encode())
        aggregate.extend(b"\0")
        aggregate.extend(value_hash.encode())
        aggregate.extend(b"\n")
    resources[policy_manifest] = (
        json.dumps(
            {
                "policy_pack_id": "default-local-password",
                "content_manifest": policy_rows,
                "content_sha256": hashlib.sha256(aggregate).hexdigest(),
            },
            sort_keys=True,
        )
        + "\n"
    ).encode()

    seed_rows = []
    family_counts: dict[str, int] = {}
    for rel in sorted(path for path in resources if path.startswith(seed_prefix + "resources/")):
        seed_rel = rel.removeprefix(seed_prefix)
        content = resources[rel].replace(b"\r\n", b"\n").replace(b"\r", b"\n")
        seed_rows.append((seed_rel, native.fnv1a64(content), len(content)))
        parts = Path(seed_rel).parts
        family = parts[1] if len(parts) > 1 and parts[0] == "resources" else "other"
        family_counts[family] = family_counts.get(family, 0) + 1
    resources[seed_artifacts] = (
        "canonical_path,content_hash,content_size_bytes\n"
        + "".join(f"{rel},{content_hash},{size}\n" for rel, content_hash, size in seed_rows)
    ).encode()
    resources[seed_manifest] = b"seed_family,source_pattern\nfixture,resources/**/*\n"
    return (
        resources,
        dict(sorted(family_counts.items())),
        len(policy_rows),
        nonresource_inventory,
    )


def native_config_fixture(name: str, marker: str) -> bytes:
    if name != "SBsrv.conf":
        return (
            f"# {marker}:{name}\n"
            + "\n".join(native.REQUIRED_CONFIG_TOKENS[name])
            + "\n"
        ).encode()
    return (
        f"# {marker}:{name}\n"
        "[server.security]\n"
        "provider_family = local_password\n"
        "default_policy_installed = true\n"
        "[server.database]\n"
        "auto_create = false\n"
        "[server.listener]\n"
        "executable_path = bin/SBgate\n"
        "control_dir = runtime/listener/control\n"
        "runtime_dir = runtime/listener/runtime\n"
        "[server.parser]\n"
        "sbps_enabled = true\n"
        "sbps_endpoint = runtime/control/sb_server.sbps.sock\n"
        "[server.memory]\n"
        "failure_mode = return_error\n"
    ).encode()


def macho_bytes(architecture: str) -> bytes:
    cpu = {"x86_64": 0x01000007, "arm64": 0x0100000C}
    if architecture in cpu:
        return b"\xcf\xfa\xed\xfe" + struct.pack("<I", cpu[architecture]) + b"\0" * 24
    if architecture == "universal":
        return (
            b"\xca\xfe\xba\xbe"
            + struct.pack(">I", 2)
            + struct.pack(">IIIII", cpu["x86_64"], 0, 0, 0, 0)
            + struct.pack(">IIIII", cpu["arm64"], 0, 0, 0, 0)
        )
    raise AssertionError(f"unsupported fixture architecture: {architecture}")


def native_archive(
    platform: str,
    marker: str,
    *,
    native_parser: str = "SBSQL",
    omit_component: str | None = None,
    architecture: str | None = None,
    profile_architecture: str | None = None,
    extra_binary: str | None = None,
    extra_path: str | None = None,
    omit_library_role: str | None = None,
) -> bytes:
    suffix = ".exe" if platform == "windows" else ""
    executable_data = (
        macho_bytes(architecture or "x86_64")
        if platform == "macos"
        else f"{marker}:native-executable".encode()
    )
    libraries: dict[str, bytes] = {}
    for role, candidates in native.REQUIRED_LIBRARY_CANDIDATES[platform].items():
        if role == omit_library_role:
            continue
        name = candidates[0]
        directory = "bin" if name.endswith(".dll") else "lib"
        libraries[f"opt/ScratchBird/{directory}/{name}"] = f"{marker}:{name}".encode()
    (
        resource_files,
        resource_counts,
        policy_count,
        nonresource_inventory,
    ) = native_resource_fixture(marker)
    runtime_files = (
        {
            f"opt/ScratchBird/bin/{name}": f"bundled runtime:{name}".encode()
            for name in native.WINDOWS_NATIVE_RUNTIME_NAMES
        }
        if platform == "windows"
        else {}
    )
    files = {
        **{
            f"opt/ScratchBird/bin/{name}{suffix}": executable_data
            for name in native.native_executables(platform)
            if name != omit_component
        },
        **libraries,
        **runtime_files,
        **{
            f"etc/scratchbird/{name}": native_config_fixture(name, marker)
            for name in native.NATIVE_CONFIGS
        },
        **{
            f"opt/ScratchBird/share/scratchbird/{name}": content
            for name, content in resource_files.items()
        },
        "opt/ScratchBird/share/scratchbird/release/NATIVE_RELEASE_PROFILE.json": native_profile(
            platform,
            native_parser=native_parser,
            resource_counts=resource_counts,
            policy_count=policy_count,
            nonresource_inventory=nonresource_inventory,
            architecture=profile_architecture or architecture,
            omit_library_role=omit_library_role,
        ),
    }
    if extra_binary is not None:
        files[f"opt/ScratchBird/bin/{extra_binary}{suffix}"] = b"forbidden compatibility binary"
    if extra_path is not None:
        files[extra_path] = b"forbidden neutral payload"
    payload_rows = [
        {"path": name, "bytes": len(data), "sha256": digest(data)}
        for name, data in sorted(files.items())
    ]
    install_manifest_path = (
        "opt/ScratchBird/share/scratchbird/release/INSTALL_MANIFEST.json"
    )
    sums_path = "opt/ScratchBird/share/scratchbird/release/SHA256SUMS"
    files[install_manifest_path] = (
        json.dumps(
            {
                "schema_id": "scratchbird.installer_payload_manifest.v1",
                "platform": platform,
                "files": payload_rows,
            },
            sort_keys=True,
        )
        + "\n"
    ).encode()
    files[sums_path] = "".join(
        f"{row['sha256']}  {row['path']}\n" for row in payload_rows
    ).encode()
    stream = io.BytesIO()
    if platform == "windows":
        with zipfile.ZipFile(stream, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            for name, data in files.items():
                archive.writestr(name, data)
    else:
        with tarfile.open(fileobj=stream, mode="w:gz") as archive:
            for name, data in files.items():
                info = tarfile.TarInfo(name)
                info.size = len(data)
                archive.addfile(info, io.BytesIO(data))
    return stream.getvalue()


def archive_with_symlink() -> bytes:
    stream = io.BytesIO()
    with tarfile.open(fileobj=stream, mode="w:gz") as archive:
        member = tarfile.TarInfo("opt/ScratchBird/bin/SBsrv")
        member.type = tarfile.SYMTYPE
        member.linkname = "/tmp/forbidden"
        archive.addfile(member)
    return stream.getvalue()


def write_installer_artifact(
    root: Path,
    platform: str,
    version: str,
    files: dict[str, bytes],
    build_id: str,
) -> None:
    root.mkdir(parents=True, exist_ok=True)
    rows = []
    profile_digests: set[str] = set()
    for rel, content in files.items():
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        rows.append({"path": rel, "bytes": len(content), "sha256": digest(content)})
        profile_digest = native_profile_digest_from_archive(content)
        if profile_digest is not None:
            profile_digests.add(profile_digest)
    rows.sort(key=lambda row: row["path"])
    if len(profile_digests) != 1:
        raise AssertionError(
            f"fixture native profile digest cardinality: {sorted(profile_digests)}"
        )
    manifest = {
        "schema_id": "scratchbird.installer_artifact_manifest.v1",
        "platform": platform,
        "version": version,
        "build_id": build_id,
        "native_server_admission": {
            **bundle.native_admission.NATIVE_SERVER_ADMISSION,
            "native_release_profile_sha256": next(iter(profile_digests)),
        },
        "publication_surface": bundle.PUBLICATION_SURFACE_SCHEMA,
        "publication_policy": bundle.PUBLICATION_SURFACE_POLICY,
        "excluded_package_formats": bundle.PUBLIC_EXCLUDED_PACKAGE_FORMATS,
        "artifacts": rows,
    }
    if platform == "windows":
        manifest["windows"] = {
            "package_mode": "portable_zip_only",
            "system_installer_included": False,
            "portable_archive_smoke_required": True,
            "native_default_port": 3092,
        }
    if platform == "macos":
        manifest["macos_signing_status"] = "qa_unsigned_not_for_public_signed_release"
    (root / "INSTALLER_ARTIFACT_MANIFEST.json").write_text(
        json.dumps(
            manifest,
            indent=2,
            sort_keys=True,
        )
        + "\n",
        encoding="utf-8",
    )
    (root / "SHA256SUMS").write_text(
        "".join(f"{row['sha256']}  {row['path']}\n" for row in rows),
        encoding="utf-8",
    )


def add_manifest_file(root: Path, rel: str, content: bytes) -> None:
    path = root / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(content)
    manifest_path = root / "INSTALLER_ARTIFACT_MANIFEST.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    manifest["artifacts"].append({"path": rel, "bytes": len(content), "sha256": digest(content)})
    manifest["artifacts"].sort(key=lambda row: row["path"])
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    (root / "SHA256SUMS").write_text(
        "".join(f"{row['sha256']}  {row['path']}\n" for row in manifest["artifacts"]),
        encoding="utf-8",
    )


def replace_manifest_file(root: Path, rel: str, content: bytes) -> None:
    (root / rel).write_bytes(content)
    manifest_path = root / "INSTALLER_ARTIFACT_MANIFEST.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    matching = [row for row in manifest["artifacts"] if row["path"] == rel]
    if len(matching) != 1:
        raise AssertionError(f"fixture manifest cardinality for {rel}: {len(matching)}")
    matching[0]["bytes"] = len(content)
    matching[0]["sha256"] = digest(content)
    profile_digest = native_profile_digest_from_archive(content)
    if profile_digest is not None:
        manifest["native_server_admission"][
            "native_release_profile_sha256"
        ] = profile_digest
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    (root / "SHA256SUMS").write_text(
        "".join(f"{row['sha256']}  {row['path']}\n" for row in manifest["artifacts"]),
        encoding="utf-8",
    )


def remove_manifest_file(root: Path, rel: str) -> None:
    (root / rel).unlink()
    manifest_path = root / "INSTALLER_ARTIFACT_MANIFEST.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    manifest["artifacts"] = [
        row for row in manifest["artifacts"] if row.get("path") != rel
    ]
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    (root / "SHA256SUMS").write_text(
        "".join(f"{row['sha256']}  {row['path']}\n" for row in manifest["artifacts"]),
        encoding="utf-8",
    )


def make_fixture(input_root: Path, version: str = "1.2.3-nightly") -> None:
    linux_payload = native_archive("linux", "linux")
    write_installer_artifact(
        input_root / "scratchbird-linux-installers",
        "linux",
        version,
        {
            f"scratchbird-linux-{version}.tar.gz": linux_payload,
        },
        "42",
    )
    windows_files = {
        f"scratchbird-windows-{version}.zip": native_archive("windows", "windows"),
    }
    write_installer_artifact(
        input_root / "scratchbird-windows-installers",
        "windows",
        version,
        windows_files,
        "42",
    )
    for arch in ("x86_64", "arm64"):
        macos_payload = native_archive("macos", f"mac-{arch}", architecture=arch)
        write_installer_artifact(
            input_root / f"scratchbird-macos-{arch}-installers",
            "macos",
            version,
            {
                f"scratchbird-macos-{version}.tar.gz": macos_payload,
            },
            f"42-{arch}",
        )
    universal_root = input_root / "scratchbird-macos-universal-installers"
    universal_root.mkdir(parents=True)
    universal_name = f"scratchbird-macos-universal-{version}-universal.tar.gz"
    universal_data = native_archive("macos", "mac-universal", architecture="universal")
    (universal_root / universal_name).write_bytes(universal_data)
    (universal_root / "MACOS_UNIVERSAL_ARTIFACT_MANIFEST.json").write_text(
        json.dumps(
            {
                "schema_id": "scratchbird.macos_universal_artifact_manifest.v1",
                "version": f"{version}-universal",
                "build_id": "42-universal",
                "native_server_admission": {
                    **bundle.native_admission.NATIVE_SERVER_ADMISSION,
                    "native_release_profile_sha256": native_profile_digest_from_archive(
                        universal_data
                    ),
                },
                "artifact": {
                    "path": universal_name,
                    "bytes": len(universal_data),
                    "sha256": digest(universal_data),
                    "architectures": ["x86_64", "arm64"],
                    "status": "qa_universal_after_per_architecture_artifacts_verify",
                },
            },
            indent=2,
            sort_keys=True,
        )
        + "\n",
        encoding="utf-8",
    )


class NightlyReleaseBundleTest(unittest.TestCase):
    revision = "a" * 40

    def create(
        self,
        input_root: Path,
        output_root: Path,
        *,
        version: str = "1.2.3-nightly",
        release_scope: str = "all",
    ) -> Path:
        return bundle.create_bundle(
            input_root,
            output_root,
            version,
            self.revision,
            "42",
            "1",
            release_scope,
        )

    @staticmethod
    def make_scope_fixture(
        input_root: Path,
        scope: str,
        version: str = "1.2.3-nightly",
    ) -> None:
        make_fixture(input_root, version=version)
        contract = bundle.get_release_contract(scope)
        allowed = {bundle.ARTIFACT_ROOTS[key] for key in contract.artifact_roots}
        for path in input_root.iterdir():
            if path.name not in allowed:
                shutil.rmtree(path)

    def test_native_flat_bundle_excludes_internal_and_smoke_evidence(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-nightly-bundle-") as temp:
            root = Path(temp)
            input_root = root / "input"
            output_root = root / "output"
            make_fixture(input_root)
            manifest_path = self.create(input_root, output_root)
            expected = {
                "scratchbird-nightly-linux-x86_64.tar.gz",
                "scratchbird-nightly-windows-x86_64.zip",
                "scratchbird-nightly-macos-x86_64.tar.gz",
                "scratchbird-nightly-macos-arm64.tar.gz",
                "scratchbird-nightly-macos-universal.tar.gz",
                "scratchbird-nightly-manifest.json",
                "scratchbird-nightly-SHA256SUMS",
            }
            self.assertEqual(expected, {path.name for path in output_root.iterdir()})
            self.assertNotEqual(
                (output_root / "scratchbird-nightly-macos-x86_64.tar.gz").read_bytes(),
                (output_root / "scratchbird-nightly-macos-arm64.tar.gz").read_bytes(),
            )
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            self.assertEqual("all", manifest["release_scope"])
            self.assertEqual("nightly", manifest["release_tag"])
            self.assertEqual("scratchbird_native_no_emulation", manifest["distribution_surface"])
            self.assertEqual("SBSQL", manifest["native_parser"])
            self.assertEqual(
                "exact_manifest_derived_native_portable_archives_only",
                manifest["public_asset_policy"],
            )
            self.assertEqual(
                [
                    "build_recipes",
                    "install_smoke_proof",
                    "system_installer_packages",
                ],
                manifest["internal_only_artifact_classes"],
            )
            verification = {
                row["name"]: row["verification"] for row in manifest["artifacts"]
            }
            self.assertEqual(
                "exact_native_payload_extraction",
                verification["scratchbird-nightly-windows-x86_64.zip"],
            )
            self.assertEqual(
                "exact_native_payload_extraction",
                verification["scratchbird-nightly-linux-x86_64.tar.gz"],
            )
            self.assertEqual(
                "exact_native_payload_extraction",
                verification["scratchbird-nightly-macos-x86_64.tar.gz"],
            )
            self.assertEqual(
                ["SBmgr", "SBgate", "SBParser", "SBsrv"],
                [row["name"] for row in manifest["native_components"]],
            )
            self.assertFalse(manifest["emulation_layers_included"])
            self.assertEqual("system-package", manifest["llvm_runtime"]["linux"]["delivery"])
            self.assertEqual("bundled", manifest["llvm_runtime"]["windows"]["delivery"])
            self.assertEqual(
                "portable_zip_only_no_msi",
                manifest["windows_release_policy"],
            )
            self.assertEqual(
                "external-homebrew", manifest["llvm_runtime"]["macos"]["delivery"]
            )
            output_text = "\n".join(path.name for path in output_root.iterdir())
            for forbidden in (
                "smoke", "PKGBUILD", ".wxs", ".spec", "rpm-build",
                ".deb", ".rpm", ".pkg", "aur",
            ):
                self.assertNotIn(forbidden, output_text)
            sums = bundle.parse_sha256sums(output_root / "scratchbird-nightly-SHA256SUMS")
            for name, expected_digest in sums.items():
                self.assertEqual(expected_digest, bundle.sha256_file(output_root / name))

    def test_platform_scoped_bundles_have_exact_isolated_inventories(self) -> None:
        expected_platforms = {
            "linux": ["linux"],
            "windows": ["windows"],
            "macos": ["macos"],
        }
        with tempfile.TemporaryDirectory(prefix="sb-platform-nightly-bundle-") as temp:
            root = Path(temp)
            for scope, platforms in expected_platforms.items():
                input_root = root / f"input-{scope}"
                output_root = root / f"output-{scope}"
                self.make_scope_fixture(input_root, scope)
                manifest_path = self.create(
                    input_root,
                    output_root,
                    release_scope=scope,
                )
                contract = bundle.get_release_contract(scope)
                self.assertEqual(
                    contract.canonical_asset_names,
                    {path.name for path in output_root.iterdir()},
                )
                manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
                self.assertEqual(scope, manifest["release_scope"])
                self.assertEqual(contract.tag, manifest["release_tag"])
                self.assertEqual(platforms, manifest["included_platforms"])
                self.assertEqual(
                    set(contract.package_names),
                    {row["name"] for row in manifest["artifacts"]},
                )
                sums = bundle.parse_sha256sums(output_root / contract.checksum_name)
                self.assertEqual(
                    set(contract.package_names) | {contract.manifest_name},
                    set(sums),
                )

    def test_platform_scope_rejects_cross_platform_artifact_roots(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-platform-nightly-root-isolation-") as temp:
            root = Path(temp)
            input_root = root / "input"
            make_fixture(input_root)
            with self.assertRaisesRegex(bundle.BundleError, "unexpected_artifact_roots"):
                self.create(input_root, root / "output", release_scope="linux")

    def test_public_release_contains_only_portable_archives(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-nightly-portable-only-") as temp:
            root = Path(temp)
            input_root = root / "input"
            make_fixture(input_root)
            self.create(input_root, root / "output")
            output_names = {path.name for path in (root / "output").iterdir()}
            self.assertEqual(7, len(output_names))
            self.assertIn("scratchbird-nightly-windows-x86_64.zip", output_names)
            self.assertFalse(
                any(
                    name.endswith((".msi", ".deb", ".rpm", ".pkg"))
                    or "aur" in name
                    for name in output_names
                )
            )

    def test_approved_driver_route_example_is_admitted_from_portable_archive(self) -> None:
        """The one reviewed example name must not widen generic driver admission."""

        with tempfile.TemporaryDirectory(prefix="sb-nightly-driver-example-") as temp:
            root = Path(temp)
            input_root = root / "input"
            self.make_scope_fixture(input_root, "linux")
            source_archive = (
                input_root
                / "scratchbird-linux-installers"
                / "scratchbird-linux-1.2.3-nightly.tar.gz"
            )
            expected_path = (
                "opt/ScratchBird/share/scratchbird/examples/core_beta_qa/"
                "driver_route_smoke.sh"
            )
            with tarfile.open(source_archive, "r:gz") as archive:
                self.assertIsNotNone(archive.getmember(expected_path))

            manifest_path = self.create(
                input_root,
                root / "output",
                release_scope="linux",
            )
            self.assertTrue(manifest_path.is_file())
            self.assertTrue(
                (root / "output" / "scratchbird-nightly-linux-x86_64.tar.gz").is_file()
            )

    def test_materialized_public_root_drops_internal_builder_evidence(self) -> None:
        """Only the admitted portable archive and its new inventory may upload."""

        with tempfile.TemporaryDirectory(prefix="sb-installer-public-root-") as temp:
            root = Path(temp)
            raw_root = root / "raw"
            version = "1.2.3-nightly"
            write_installer_artifact(
                raw_root,
                "linux",
                version,
                {
                    f"scratchbird-linux-{version}.tar.gz": native_archive(
                        "linux", "materialize"
                    ),
                },
                "42",
            )
            add_manifest_file(
                raw_root,
                "internal-build-evidence.json",
                b'{"internal_only":true}\n',
            )
            raw_manifest_path = raw_root / "INSTALLER_ARTIFACT_MANIFEST.json"
            raw_manifest = json.loads(raw_manifest_path.read_text(encoding="utf-8"))
            for field in (
                "publication_surface",
                "publication_policy",
                "excluded_package_formats",
            ):
                raw_manifest.pop(field)
            raw_manifest_path.write_text(
                json.dumps(raw_manifest, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )

            raw_files = installer_verifier.artifact_file_rows(
                raw_root, raw_manifest["artifacts"]
            )
            installer_verifier.verify_manifest_checksums(raw_root, raw_files)
            public_root = root / "public"
            installer_verifier.materialize_publication_root(
                raw_root,
                public_root,
                raw_manifest,
                raw_files,
                "linux",
            )

            self.assertEqual(
                {
                    f"scratchbird-linux-{version}.tar.gz",
                    "INSTALLER_ARTIFACT_MANIFEST.json",
                    "SHA256SUMS",
                },
                {path.name for path in public_root.iterdir()},
            )
            public_manifest = json.loads(
                (public_root / "INSTALLER_ARTIFACT_MANIFEST.json").read_text(
                    encoding="utf-8"
                )
            )
            public_files = installer_verifier.artifact_file_rows(
                public_root, public_manifest["artifacts"]
            )
            installer_verifier.verify_manifest_checksums(public_root, public_files)
            installer_verifier.require_publication_manifest(
                public_root, public_manifest, public_files, "linux"
            )
            installer_verifier.verify_native_admission_packages(
                public_root,
                public_manifest,
                public_files,
                "linux",
                "x86_64",
            )

    def test_canonical_names_are_stable_across_versions(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-nightly-stable-") as temp:
            root = Path(temp)
            names = []
            for index, version in enumerate(("1.2.3-nightly", "9.8.7-nightly")):
                input_root = root / f"input-{index}"
                output_root = root / f"output-{index}"
                make_fixture(input_root, version=version)
                self.create(input_root, output_root, version=version)
                names.append({path.name for path in output_root.iterdir()})
            self.assertEqual(names[0], names[1])

    def test_tampered_manifest_artifact_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-nightly-tamper-") as temp:
            root = Path(temp)
            input_root = root / "input"
            make_fixture(input_root)
            target = input_root / "scratchbird-windows-installers" / "scratchbird-windows-1.2.3-nightly.zip"
            target.write_bytes(b"tampered")
            with self.assertRaisesRegex(bundle.BundleError, "installer_manifest_(size|sha256)_mismatch"):
                self.create(input_root, root / "output")

    def test_missing_native_server_admission_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-nightly-missing-admission-") as temp:
            root = Path(temp)
            input_root = root / "input"
            make_fixture(input_root)
            manifest_path = (
                input_root
                / "scratchbird-linux-installers"
                / "INSTALLER_ARTIFACT_MANIFEST.json"
            )
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            manifest.pop("native_server_admission")
            manifest_path.write_text(
                json.dumps(manifest, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(bundle.BundleError, "native_server_admission_missing"):
                self.create(input_root, root / "output")

    def test_tampered_native_server_admission_digest_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-nightly-admission-digest-") as temp:
            root = Path(temp)
            input_root = root / "input"
            make_fixture(input_root)
            manifest_path = (
                input_root
                / "scratchbird-linux-installers"
                / "INSTALLER_ARTIFACT_MANIFEST.json"
            )
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            manifest["native_server_admission"]["native_release_profile_sha256"] = "0" * 64
            manifest_path.write_text(
                json.dumps(manifest, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                bundle.BundleError, "native_server_admission_profile_digest_mismatch"
            ):
                self.create(input_root, root / "output")

    def test_public_installer_root_rejects_system_package(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-nightly-system-package-reject-") as temp:
            root = Path(temp)
            input_root = root / "input"
            make_fixture(input_root)
            add_manifest_file(
                input_root / "scratchbird-linux-installers",
                "scratchbird-linux-1.2.3-nightly.deb",
                b"internal-only-deb",
            )
            with self.assertRaisesRegex(
                bundle.BundleError, "installer_publication_system_package_forbidden"
            ):
                self.create(input_root, root / "output")

    def test_public_installer_root_rejects_unmanifested_neutral_file_and_empty_directory(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-nightly-root-completeness-") as temp:
            root = Path(temp)
            input_root = root / "input"
            make_fixture(input_root)
            linux_root = input_root / "scratchbird-linux-installers"
            (linux_root / "neutral-helper.bin").write_bytes(b"hidden")
            with self.assertRaisesRegex(
                bundle.BundleError, "installer_publication_tree_mismatch"
            ):
                self.create(input_root, root / "output")

            (linux_root / "neutral-helper.bin").unlink()
            (linux_root / "unused").mkdir()
            with self.assertRaisesRegex(
                bundle.BundleError, "installer_publication_directory_mismatch"
            ):
                self.create(input_root, root / "output")

    def test_multiple_package_candidates_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-nightly-collision-") as temp:
            root = Path(temp)
            input_root = root / "input"
            make_fixture(input_root)
            add_manifest_file(
                input_root / "scratchbird-linux-installers",
                "scratchbird-linux-second.tar.gz",
                b"second linux tar",
            )
            with self.assertRaisesRegex(
                bundle.BundleError, "installer_publication_package_cardinality"
            ):
                self.create(input_root, root / "output")

    def test_windows_msi_input_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-nightly-windows-zip-only-") as temp:
            root = Path(temp)
            input_root = root / "input"
            make_fixture(input_root)
            add_manifest_file(
                input_root / "scratchbird-windows-installers",
                "scratchbird-windows-1.2.3-nightly.msi",
                b"forbidden msi",
            )
            with self.assertRaisesRegex(
                bundle.BundleError, "installer_publication_system_package_forbidden"
            ):
                self.create(input_root, root / "output")

    def test_non_native_parser_profile_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-nightly-native-profile-") as temp:
            root = Path(temp)
            input_root = root / "input"
            make_fixture(input_root)
            linux_root = input_root / "scratchbird-linux-installers"
            replacement = native_archive("linux", "bad", native_parser="Firebird")
            replace_manifest_file(
                linux_root,
                "scratchbird-linux-1.2.3-nightly.tar.gz",
                replacement,
            )
            with self.assertRaisesRegex(bundle.BundleError, "native_parser_identity_mismatch"):
                self.create(input_root, root / "output")

    def test_missing_required_native_component_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-nightly-native-component-") as temp:
            root = Path(temp)
            input_root = root / "input"
            make_fixture(input_root)
            linux_root = input_root / "scratchbird-linux-installers"
            replace_manifest_file(
                linux_root,
                "scratchbird-linux-1.2.3-nightly.tar.gz",
                native_archive("linux", "missing", omit_component="SBgate"),
            )
            with self.assertRaisesRegex(bundle.BundleError, "installed_bin_set_mismatch"):
                self.create(input_root, root / "output")

    def test_missing_required_native_library_and_profile_row_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-nightly-native-library-") as temp:
            root = Path(temp)
            input_root = root / "input"
            make_fixture(input_root)
            linux_root = input_root / "scratchbird-linux-installers"
            replace_manifest_file(
                linux_root,
                "scratchbird-linux-1.2.3-nightly.tar.gz",
                native_archive(
                    "linux",
                    "missing-library",
                    omit_library_role="engine_shared",
                ),
            )
            with self.assertRaisesRegex(
                bundle.BundleError, "declared_required_native_library_missing:engine_shared"
            ):
                self.create(input_root, root / "output")

    def test_extra_emulation_binary_fails_exact_native_inventory(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-nightly-extra-emulation-") as temp:
            root = Path(temp)
            input_root = root / "input"
            make_fixture(input_root)
            linux_root = input_root / "scratchbird-linux-installers"
            replace_manifest_file(
                linux_root,
                "scratchbird-linux-1.2.3-nightly.tar.gz",
                native_archive("linux", "extra", extra_binary="sbp_firebird"),
            )
            with self.assertRaisesRegex(bundle.BundleError, "installed_bin_set_mismatch"):
                self.create(input_root, root / "output")

    def test_hidden_driver_and_dbeaver_payloads_fail_closed(self) -> None:
        for identity in ("scratchbird_client", "dbeaver"):
            with self.subTest(identity=identity), tempfile.TemporaryDirectory(
                prefix="sb-nightly-client-boundary-"
            ) as temp:
                root = Path(temp)
                input_root = root / "input"
                make_fixture(input_root)
                replace_manifest_file(
                    input_root / "scratchbird-linux-installers",
                    "scratchbird-linux-1.2.3-nightly.tar.gz",
                    native_archive("linux", identity, extra_binary=identity),
                )
                with self.assertRaisesRegex(
                    bundle.BundleError, "native_admission_client_payload_forbidden"
                ):
                    self.create(input_root, root / "output")

    def test_only_the_exact_driver_route_example_is_exempted(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-nightly-driver-exception-") as temp:
            root = Path(temp)
            input_root = root / "input"
            make_fixture(input_root)
            replace_manifest_file(
                input_root / "scratchbird-linux-installers",
                "scratchbird-linux-1.2.3-nightly.tar.gz",
                native_archive(
                    "linux",
                    "unapproved-driver-name",
                    extra_path=(
                        "opt/ScratchBird/share/scratchbird/examples/core_beta_qa/"
                        "driver_helper.sh"
                    ),
                ),
            )
            with self.assertRaisesRegex(
                bundle.BundleError, "native_admission_client_payload_forbidden"
            ):
                self.create(input_root, root / "output")

    def test_neutral_payload_outside_native_layout_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-nightly-neutral-payload-") as temp:
            root = Path(temp)
            input_root = root / "input"
            make_fixture(input_root)
            replace_manifest_file(
                input_root / "scratchbird-linux-installers",
                "scratchbird-linux-1.2.3-nightly.tar.gz",
                native_archive(
                    "linux",
                    "neutral-outside-layout",
                    extra_path="opt/ScratchBird/extensions/enginehelper.bin",
                ),
            )
            with self.assertRaisesRegex(
                bundle.BundleError, "native_admission_payload_layout_(directory_)?forbidden"
            ):
                self.create(input_root, root / "output")

    def test_macos_artifact_directory_architecture_swap_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-nightly-mac-arch-") as temp:
            root = Path(temp)
            input_root = root / "input"
            make_fixture(input_root)
            x86_root = input_root / "scratchbird-macos-x86_64-installers"
            replace_manifest_file(
                x86_root,
                "scratchbird-macos-1.2.3-nightly.tar.gz",
                native_archive(
                    "macos",
                    "wrong-arch",
                    architecture="arm64",
                    profile_architecture="x86_64",
                ),
            )
            with self.assertRaisesRegex(bundle.BundleError, "macos_architecture_mismatch"):
                self.create(input_root, root / "output")

    def test_universal_macos_requires_per_architecture_llvm_map(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-nightly-mac-llvm-map-") as temp:
            root = Path(temp)
            input_root = root / "input"
            make_fixture(input_root)
            universal_root = input_root / "scratchbird-macos-universal-installers"
            manifest_path = universal_root / "MACOS_UNIVERSAL_ARTIFACT_MANIFEST.json"
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            artifact_path = universal_root / manifest["artifact"]["path"]
            replacement = native_archive(
                "macos",
                "universal-scalar-llvm",
                architecture="universal",
                profile_architecture="arm64",
            )
            artifact_path.write_bytes(replacement)
            manifest["artifact"]["bytes"] = len(replacement)
            manifest["artifact"]["sha256"] = digest(replacement)
            manifest["native_server_admission"]["native_release_profile_sha256"] = (
                native_profile_digest_from_archive(replacement)
            )
            manifest_path.write_text(
                json.dumps(manifest, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                bundle.BundleError, "macos_universal_llvm_runtime_map_required"
            ):
                self.create(input_root, root / "output")

    def test_symlink_and_unexpected_artifact_roots_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-nightly-path-") as temp:
            root = Path(temp)
            input_root = root / "input"
            make_fixture(input_root)
            (input_root / "scratchbird-linux-installers" / "unsafe-link").symlink_to("SHA256SUMS")
            with self.assertRaisesRegex(bundle.BundleError, "symlink_forbidden"):
                self.create(input_root, root / "output")

            (input_root / "scratchbird-linux-installers" / "unsafe-link").unlink()
            (input_root / "scratchbird-linux-install-smoke").mkdir()
            with self.assertRaisesRegex(bundle.BundleError, "unexpected_artifact_roots"):
                self.create(input_root, root / "output")

    def test_archive_symlink_fails_before_payload_verification(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-nightly-archive-link-") as temp:
            root = Path(temp)
            input_root = root / "input"
            make_fixture(input_root)
            replace_manifest_file(
                input_root / "scratchbird-linux-installers",
                "scratchbird-linux-1.2.3-nightly.tar.gz",
                archive_with_symlink(),
            )
            with self.assertRaisesRegex(
                bundle.BundleError, "native_admission_archive_special_entry"
            ):
                self.create(input_root, root / "output")


if __name__ == "__main__":
    unittest.main(verbosity=2)
